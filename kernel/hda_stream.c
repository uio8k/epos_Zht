#include <string.h>
#include "kernel.h"
#include "cpu.h"
#include "machdep.h"
#include "hda.h"
#include "hda_internal.h"

/* ===================== 连续流播放（单调追加 + 安全余量） =====================
   模型：输出流启动后保持 RUN=1 不停止（消除逐音启停 pop），DMA 循环读取
   512KB 环形缓冲；音符/休止符按时间顺序**严格追加**在真实数据末尾
   （g_cont_data_end 单调递增，音符首尾相接 → 节拍 = 音符时值），写入位置
   与 DMA 读位置之间保持 HDA_CONT_MARGIN 安全余量，消除「写指针贴读指针」
   竞态（该竞态会导致音符头部样本被 DMA 抢先读走而丢失）。

   两个指针语义分离：
     g_cont_data_end  真实数据末尾（单调递增）：underrun 判定基准
     g_cont_wrpos     物理写游标：可被 fill_ahead 推进（预填静音）；写入
                      音符时回到 start——start 恒 ≥ data_end 且距 rdpos
                      ≥ MARGIN，故回退只覆盖自己写的预填静音，不侵入
                      未消费的真实数据。

   underrun = DMA 读位置越过真实数据末尾（此时 DMA 正在读预填静音，
   若未预填则会读到环形缓冲里的旧音频）。QEMU 3.1 对此无任何通知
   （既不置 FIFOE 也不停流），只能由驱动检测：计数 + 记 trace + 重对齐。

   时间轴：play_note 等到「本音符剩余 HDA_CONT_MARGIN 以内」即返回，
   提前量正好是下一个音符写入所需的安全距离。用户程序耗时若 ≤ MARGIN
   对应时间（默认约 93ms）则音符无缝相接、节拍严格等于音符时值；
   超出则出现静音间隙（fill_ahead 已预填，不会重复旧数据）。

   休止符用 sys_audio_play_silence_ms 追加真实静音（有节拍语义）；
   演示结束用 sys_audio_stop 追加尾部静音并等待播完后清 RUN。 */
#define HDA_CONT_BUF_SIZE  0x80000   /* 512KB 环形缓冲（> 最长音符 1600ms+哨兵） */
#define HDA_CONT_FILL_MS   500       /* 预填静音前瞻：覆盖音符间读盘/排序开销 */

/* 安全余量：DMA 读位置与下一次写入位置之间的最小距离，需覆盖
   「读 LPIB → 写入完成」期间 DMA 的前进量。QEMU 3.1 hda-codec.c 默认走
   1ms 虚拟时钟定时器、按实时速率追赶目标位置（约 176 字节/ms @44.1kHz
   立体声 16bit），即 LPIB 步进粒度约 1ms；取 16384 字节（约 93ms）留足
   裕量。该值同时是「提交相对播放」的固定前瞻延迟，处于人耳不可感知范围。
   实测标定：hda_cont_lpib_max_step 记录运行期 LPIB 单次最大前进量。 */
#define HDA_CONT_MARGIN    16384

/* BDL 分段大小：4KB（符合 Intel 硬件惯例）。全条目使能 IOC，BCIS 中断
   间隔 = 4096 B ÷ 176.4 KB/s ≈ 23ms，给中断唤醒足够精度；
   中断频率约 43 次/秒，开销可忽略。 */
#define HDA_CONT_SEG_BYTES 0x1000

/* 中断可用性探测窗口（PIT tick 数；HZ=100 → 20 tick = 200ms） */
#define HDA_IRQ_PROBE_TICKS 20

/* 中断模式下「连续被唤醒但读位置无进展」的判定上限，超过则退回轮询 */
#define HDA_IRQ_STALL_LIMIT 200

/* 轮询模式的「有界自旋」次数：连续读 LPIB（MMIO）后让出一次 CPU。
   为什么不能纯 yield：实测纯 yield 轮询下 QEMU 音频后端会周期性丢帧
   （wav 相位跳变，max_jump 5k~58k，约每 2~4 秒一次）。机制：无 MMIO 的
   纯 CPU 循环让 TCG 大段翻译执行，音频定时器批量补触发（虚拟时钟跳跃），
   单次从 guest 拉取的数据超过其内部 8KB 缓冲 → 触发溢出丢弃（丢 ~46ms）。
   周期性 MMIO 读让 QEMU 主循环按真实节奏调度定时器，消除该现象。 */
#define HDA_POLL_SPIN 64

static int       g_cont_on     = 0;   /* 连续流是否已启动（RUN=1） */
static uint32_t  g_cont_size   = 0;   /* 环形缓冲大小 S（= CBL） */
static uint32_t  g_cont_wrpos  = 0;   /* 物理写游标（写入/预填位置，绝对字节） */
static uint32_t  g_cont_data_end = 0; /* 真实数据（音符/休止符）末尾，单调递增 */
static uint32_t  g_cont_rdpos  = 0;   /* DMA 已消费的绝对字节数 */
static uint32_t  g_cont_lpib   = 0;   /* 上一次 LPIB（绕回检测） */
static uint32_t  g_cont_wraps  = 0;   /* DMA 绕回次数 */
static uint32_t  g_cont_rate   = 44100;
static uint32_t  g_cont_ch     = 2;
static uint32_t  g_cont_bits   = 16;
static uint8_t   g_cont_note_id = 0;  /* 音符序号（调用链日志区间关联键） */
static uint32_t  g_cont_lpib_max_step = 0; /* LPIB 单次最大前进量（标定 MARGIN） */

/* 等待 DMA 消费 / 缓冲空间的轮询超时（迭代次数，约 5s） */
#define HDA_CONT_WAIT_TIMEOUT 50000000

/* 停流前追加的尾部静音（吸收停流时 codec/后端缓冲截断） */
#define HDA_STOP_TAIL_MS 250

/* 流错误计数器（供 sys_audio_get_errors 回读） */
static uint32_t g_cont_fifo_errors = 0;
static uint32_t g_cont_desc_errors = 0;

/* underrun 计数：DMA 读位置越过真实数据末尾的次数。
   QEMU 3.1 对 underrun 无任何通知（不置 FIFOE、不停流），
   这是唯一的观测途径，供 sys_audio_get_underruns 回读 + 日志归因。 */
static uint32_t g_cont_underruns = 0;

/* 读 SDSTS，检测并清除 FIFO/Descriptor 错误。
   轮询方式（不做 ISR）——已在 hda_cont_refresh_rd 中高频调用，
   覆盖播放全程；QEMU TCG 下 underrun 极少但留检测以防万一 */
static void hda_cont_check_errors(void)
{
    uint8_t sts = hda_readb(HDA_SD_STS(HDA_OUTPUT_STREAM));
    if (sts & HDA_SDSTS_FIFOE) {
        g_cont_fifo_errors++;
        printk("HDA: FIFO error (#%u)\r\n", g_cont_fifo_errors);
        hda_writeb(HDA_SD_STS(HDA_OUTPUT_STREAM), HDA_SDSTS_FIFOE);  /* W1C */
    }
    if (sts & HDA_SDSTS_DESE) {
        g_cont_desc_errors++;
        printk("HDA: Desc error (#%u)\r\n", g_cont_desc_errors);
        hda_writeb(HDA_SD_STS(HDA_OUTPUT_STREAM), HDA_SDSTS_DESE);   /* W1C */
    }
}

/* ===================== 连续流辅助函数 ===================== */

/* 读取 LPIB 更新 g_cont_rdpos（绕回感知）+ 步进统计。
   不含错误检查：ISR 也调用本函数，而 ISR 上下文禁止 printk
   （hda_cont_check_errors 会打印），错误检查只在任务上下文轮询路径调用。 */
static void hda_cont_refresh_rd(void)
{
    uint32_t lpib = hda_readl(HDA_SD_LPIB(HDA_OUTPUT_STREAM));
    uint32_t step = (lpib >= g_cont_lpib) ? (lpib - g_cont_lpib)
                                          : (g_cont_size - g_cont_lpib + lpib);
    if (lpib < g_cont_lpib)
        g_cont_wraps++;   /* DMA 绕回一圈 */
    if (step > g_cont_lpib_max_step)
        g_cont_lpib_max_step = step;   /* 标定 HDA_CONT_MARGIN 的依据 */
    g_cont_lpib = lpib;
    g_cont_rdpos = g_cont_wraps * g_cont_size + lpib;
}

/* ===================== BCIS 中断与等待队列 =====================
   QEMU 3.1：BDL 条目耗尽且该条目 IOC=1 时置 SDSTS.BCIS 并拉 INTx
   （intel-hda.c 的 intel_hda_xfer → intel_hda_update_irq）。中断模式下
   wait 睡眠于 wq_hda，ISR 只做三件事：清 BCIS、刷新读位置、唤醒等待者；
   探测失败（IRQ 不可用）或运行中链路停摆则退回轮询——两条路径共用同一套
   指针/underrun 逻辑，fallback 不牺牲正确性。 */
static struct wait_queue *wq_hda = NULL;

static int      g_hda_irq_ok = -1;             /* -1 未探测 / 0 轮询 / 1 中断 */
static volatile uint32_t g_hda_irq_count = 0;  /* BCIS 累计次数（诊断） */

extern void isr_default(uint32_t irq, struct context *ctx); /* startup.c：空闲槽 */

/* BCIS 中断处理。运行在关中断上下文（HWINT 桩不发 sti，EOI 由桩在 ISR
   返回后发送），故：不睡眠、不 printk、不分配内存。 */
void isr_hda(uint32_t irq, struct context *ctx)
{
    (void)irq;
    (void)ctx;

    uint8_t sts = hda_readb(HDA_SD_STS(HDA_OUTPUT_STREAM));
    if (!(sts & HDA_SDSTS_BCIS))
        return;                      /* 不是本设备的完成中断 */

    hda_writeb(HDA_SD_STS(HDA_OUTPUT_STREAM), HDA_SDSTS_BCIS);  /* W1C */
    g_hda_irq_count++;
    hda_cont_refresh_rd();           /* 推进 rdpos（MMIO 读，允许） */
    wake_up(&wq_hda, -1);            /* 唤醒全部等待者（ISR 内可用，有先例） */
}

/* 注册 BCIS 中断。本内核 g_intr_vector 是单指针覆盖、无共享 IRQ 链，
   故仅在 IRQ 号合法且槽仍为空闲（isr_default）时注册，避免顶掉
   timer/kbd/e1000/floppy 的处理程序；失败仅退回轮询，不影响音频功能。 */
int hda_cont_register_irq(void)
{
    if (g_hda.irq >= NR_IRQ) {
        printk("HDA: invalid IRQ %u, polling mode\r\n", g_hda.irq);
        g_hda_irq_ok = 0;
        return -1;
    }
    if (g_intr_vector[g_hda.irq] != isr_default) {
        printk("HDA: IRQ %u already owned, polling mode\r\n", g_hda.irq);
        g_hda_irq_ok = 0;
        return -1;
    }

    g_intr_vector[g_hda.irq] = isr_hda;
    enable_irq(g_hda.irq);
    printk("HDA: BCIS IRQ registered (irq=%u)\r\n", g_hda.irq);
    return 0;
}

/* 在环形缓冲 wrpos 处写入 len 字节（自动处理跨缓冲末尾的绕回） */
static void hda_cont_write_at_wrpos(const uint8_t *src, uint32_t len)
{
    uint32_t pos = g_cont_wrpos % g_cont_size;
    uint32_t first = g_cont_size - pos;
    if (first > len)
        first = len;
    memcpy(g_hda.sample_buf + pos, src, first);
    if (len > first)
        memcpy(g_hda.sample_buf, src + first, len - first);
    g_cont_wrpos += len;
}

/* 在 wrpos 处写入 len 字节全 0（静音） */
static void hda_cont_zero_at_wrpos(uint32_t len)
{
    uint8_t zbuf[64];
    memset(zbuf, 0, sizeof(zbuf));
    while (len) {
        uint32_t n = (len > sizeof(zbuf)) ? sizeof(zbuf) : len;
        hda_cont_write_at_wrpos(zbuf, n);
        len -= n;
    }
}

/* 指针快照（供 hda_trace.c 记录事件上下文；32 位对齐读天然原子，无需加锁） */
uint32_t hda_cont_rdpos_snapshot(void)
{
    return g_cont_rdpos;
}

uint32_t hda_cont_wrpos_snapshot(void)
{
    return g_cont_wrpos;
}

/* 把 DMA 即将读取的前方区域预填静音，前瞻 HDA_CONT_FILL_MS。
   作用：音符之间（读盘/排序开销）以及 underrun 期间，DMA 读到的是静音，
   而不是环形缓冲里的旧音频。
   约束：预填游标不得侵入真实数据区（wrpos ≥ data_end），也不得绕回覆盖
   尚未消费的区域（wrpos - rdpos < size）。 */
static void hda_cont_fill_ahead(void)
{
    if (g_cont_wrpos < g_cont_data_end)
        g_cont_wrpos = g_cont_data_end;

    uint32_t bps = (g_cont_bits / 8) * g_cont_ch;
    if (bps == 0)
        return;
    uint32_t fill = (uint32_t)((uint64_t)g_cont_rate * HDA_CONT_FILL_MS / 1000) * bps;
    uint32_t target = g_cont_rdpos + fill;
    while (g_cont_wrpos < target && (g_cont_wrpos - g_cont_rdpos) < g_cont_size)
        hda_cont_zero_at_wrpos(64);
}

/* 等待 DMA 消费到 target 绝对字节位置。返回 1 = 正常到达；0 = 超时（约 5s）。
   中断模式（IRQ 可用）：睡眠于 wq_hda，由 BCIS 唤醒，醒来重查条件；
   轮询模式（fallback）：预填静音后 sys_task_yield 让出 CPU，不再忙等独占单核。 */
static int hda_cont_wait_until(uint32_t target)
{
    int timeout = HDA_CONT_WAIT_TIMEOUT;
    uint32_t last = g_cont_rdpos;
    int stuck = 0;

    while (timeout-- > 0) {
        hda_cont_refresh_rd();
        if (g_cont_rdpos >= target)
            return 1;

        if (g_hda_irq_ok == 1) {
            /* 关中断后重查条件再睡眠，避免「检查后、睡眠前」中断到达
               造成丢唤醒；伪唤醒（条件未满足）属正常，醒来继续睡。 */
            uint32_t flags;
            save_flags_cli(flags);
            if (g_cont_rdpos < target)
                sleep_on(&wq_hda);
            restore_flags(flags);

            if (g_cont_rdpos == last) {
                if (++stuck >= HDA_IRQ_STALL_LIMIT) {
                    /* 连续多次唤醒但读位置毫无进展 → 中断链路停摆，退回轮询 */
                    g_hda_irq_ok = 0;
                    hda_trace_log(HTEV_IRQ_FALLBACK, 0, g_cont_rdpos, target);
                    printk("HDA: IRQ stalled at rd=%u, polling fallback\r\n",
                           g_cont_rdpos);
                }
            } else {
                stuck = 0;
                last = g_cont_rdpos;
            }
        } else {
            /* 轮询模式：有界自旋（周期性 MMIO 读锚定 QEMU 音频定时器）+
               每 HDA_POLL_SPIN 轮让出一次 CPU */
            int spin = HDA_POLL_SPIN;
            while (spin-- > 0 && timeout > 0) {
                hda_cont_refresh_rd();
                if (g_cont_rdpos >= target)
                    return 1;
                timeout--;
            }
            hda_cont_check_errors();
            hda_cont_fill_ahead();
            sys_task_yield();
        }
    }
    return 0;
}

/* 预留一段 total 字节的追加区间，返回写入起点。做三件事：
   1) 刷新读位置并检测 underrun（count_underrun 非零时；安全余量已被吃光
      = DMA 读到了 data_end 之后，即正在读预填静音；首音符除外）；
   2) 起点取 max(data_end, rdpos + MARGIN)：正常路径 = data_end
      （严格追加、音符首尾相接），余量不足时前移重对齐；
   3) 等待缓冲空间（真实/新数据不得越过 rdpos + size）。 */
static uint32_t hda_cont_reserve(uint32_t total, uint8_t note_id,
                                 int count_underrun)
{
    hda_cont_refresh_rd();

    /* underrun 判定：DMA 读位置已越过真实数据末尾（此刻正在读预填静音）。
       不能用「安全余量不足」（rdpos + MARGIN > data_end）——finish 的退出
       条件正是让余量逼近 MARGIN，那样每个音符都会被误报为 underrun。 */
    if (count_underrun && g_cont_note_id > 1 &&
        g_cont_rdpos >= g_cont_data_end) {
        g_cont_underruns++;
        hda_trace_log(HTEV_UNDERRUN, note_id, g_cont_data_end, g_cont_rdpos);
    }

    uint32_t start = g_cont_data_end;
    if (start < g_cont_rdpos + HDA_CONT_MARGIN)
        start = g_cont_rdpos + HDA_CONT_MARGIN;

    int timeout = HDA_CONT_WAIT_TIMEOUT;
    while (timeout-- > 0) {
        hda_cont_refresh_rd();
        if (start - g_cont_rdpos + total <= g_cont_size)
            break;
        hda_cont_check_errors();   /* 任务上下文才允许的错误检查/打印 */
        hda_cont_fill_ahead();
        sys_task_yield();
    }
    if (timeout <= 0)
        hda_trace_log(HTEV_WAIT_TIMEOUT, note_id, start, start + total);

    return start;
}

/* 结束一段追加区间：等到「本段剩余 HDA_CONT_MARGIN 以内」才返回，
   为下一次写入预留安全距离（用户程序耗时 ≤ MARGIN 时音符无缝相接）。 */
static void hda_cont_finish(uint8_t note_id)
{
    uint32_t goal = (g_cont_data_end > HDA_CONT_MARGIN)
                    ? g_cont_data_end - HDA_CONT_MARGIN : 0;
    if (!hda_cont_wait_until(goal))
        hda_trace_log(HTEV_WAIT_TIMEOUT, note_id, goal, g_cont_data_end);
    hda_trace_log(HTEV_NOTE_DONE, note_id, goal, g_cont_data_end);
}

/* 启动连续流：分配 512KB 环形缓冲 + BDL，配置流并 RUN=1（此后不再停止） */
static int hda_cont_start(uint32_t rate, uint32_t ch, uint32_t bits)
{
    if (g_cont_on)
        return 0;

    g_cont_size = HDA_CONT_BUF_SIZE;

    if (hda_ensure_dma_buf(g_cont_size) == NULL) {
        printk("HDA: cont DMA alloc failed (%u bytes)\r\n", g_cont_size);
        return -2;
    }
    memset(g_hda.sample_buf, 0, g_cont_size);

    uint32_t bdl_phys = 0;
    g_hda.bdl = (struct hda_bdl_entry *)hda_alloc_dma(
        sizeof(struct hda_bdl_entry) * HDA_BDL_ENTRIES, &bdl_phys);
    if (g_hda.bdl == NULL) {
        printk("HDA: cont BDL alloc failed\r\n");
        return -2;
    }

    /* BDL 指向环形缓冲，拆成 HDA_CONT_SEG_BYTES 段并**全条目使能 IOC**：
       每段消费完触发一次 BCIS 中断（约 23ms），中断等待的唤醒精度由此决定；
       段数 = 512KB / 4KB = 128 ≤ HDA_BDL_ENTRIES（LVI 8 位上限 256）。 */
    uint32_t buf_phys = vtop((uint32_t)g_hda.sample_buf);
    uint32_t remaining = g_cont_size;
    uint32_t offset = 0;
    int bdl_entries = 0;
    for (int i = 0; i < HDA_BDL_ENTRIES && remaining > 0; i++) {
        uint32_t chunk = (remaining > HDA_CONT_SEG_BYTES)
                       ? HDA_CONT_SEG_BYTES : remaining;
        g_hda.bdl[i].addr_lo = buf_phys + offset;
        g_hda.bdl[i].addr_hi = 0;
        g_hda.bdl[i].length  = chunk;
        g_hda.bdl[i].flags   = 0x01;   /* IOC：条目完成即中断 */
        offset    += chunk;
        remaining -= chunk;
        bdl_entries++;
    }
    for (int i = bdl_entries; i < HDA_BDL_ENTRIES; i++)
        memset(&g_hda.bdl[i], 0, sizeof(g_hda.bdl[i]));

    /* Stream Reset */
    hda_writeb(HDA_SD_CTL(HDA_OUTPUT_STREAM), HDA_SDCTL_SRST);
    while (hda_readb(HDA_SD_CTL(HDA_OUTPUT_STREAM)) & HDA_SDCTL_SRST)
        ;
    hda_writeb(HDA_SD_CTL(HDA_OUTPUT_STREAM), 0);

    /* 配置流（RUN=0 状态下写入，QEMU 在 RUN 0→1 时锁存） */
    hda_writel(HDA_SD_BDPL(HDA_OUTPUT_STREAM), bdl_phys);
    hda_writel(HDA_SD_BDPU(HDA_OUTPUT_STREAM), 0);
    hda_writel(HDA_SD_CBL(HDA_OUTPUT_STREAM), g_cont_size);
    hda_writew(HDA_SD_LVI(HDA_OUTPUT_STREAM), bdl_entries - 1);

    uint16_t fmt = HDA_FMT_BITS_16;
    if (rate == 44100) fmt |= HDA_FMT_BASE_44;
    else               fmt |= HDA_FMT_BASE_48;
    fmt |= HDA_FMT_MULT_1X | HDA_FMT_DIV_1X;
    if (ch == 2) fmt |= HDA_FMT_CHAN_STEREO;
    else         fmt |= HDA_FMT_CHAN_MONO;
    hda_writew(HDA_SD_FMT(HDA_OUTPUT_STREAM), fmt);
    hda_send_verb(g_hda.codec_cad, g_hda.output_dac,
                  HDA_VERB_SET_CONVERTER_FMT |
                  hda_readw(HDA_SD_FMT(HDA_OUTPUT_STREAM)));

    /* 启动流（先 tag 后 RUN） */
    hda_flush_cache();
    hda_writeb(HDA_SD_CTL(HDA_OUTPUT_STREAM) + 1, 0);
    hda_writeb(HDA_SD_CTL(HDA_OUTPUT_STREAM) + 2, (uint8_t)(HDA_OUTPUT_STREAM << 4));
    hda_writeb(HDA_SD_CTL(HDA_OUTPUT_STREAM) + 0,
               HDA_SDCTL_IOCE | HDA_SDCTL_FEIE | HDA_SDCTL_DEIE | HDA_SDCTL_RUN);

    printk("HDA: continuous stream started (CBL=%u, %uHz %uch %ubit)\r\n",
           g_cont_size, rate, ch, bits);

    g_cont_on    = 1;
    g_cont_wrpos = 0;
    g_cont_rdpos = 0;
    g_cont_lpib  = 0;
    g_cont_wraps = 0;
    g_cont_rate  = rate;
    g_cont_ch    = ch;
    g_cont_bits  = bits;
    g_cont_fifo_errors = 0;
    g_cont_desc_errors = 0;
    hda_trace_log(HTEV_STREAM_START, 0, 0, g_cont_size);

    /* 中断可用性探测（仅首次启动）：流 RUN 后 DMA 立即开始消费，
       中断链路正常时首个 BCIS 应在约 23ms 内到达；窗口内无中断则退回
       轮询。探测期间 DMA 读到的是全零缓冲（已 memset），不会出声。 */
    if (g_hda_irq_ok < 0) {
        uint32_t t0 = g_timer_ticks;
        while (g_timer_ticks - t0 < HDA_IRQ_PROBE_TICKS) {
            if (g_hda_irq_count > 0) {
                g_hda_irq_ok = 1;
                hda_trace_log(HTEV_BCIS_IRQ, 0, 0, g_hda_irq_count);
                printk("HDA: BCIS IRQ active (first within %u ms)\r\n",
                       (g_timer_ticks - t0) * 10);
                break;
            }
            sys_task_yield();
        }
        if (g_hda_irq_ok < 0) {
            g_hda_irq_ok = 0;
            hda_trace_log(HTEV_IRQ_FALLBACK, 0, 0, 0);
            printk("HDA: no BCIS IRQ in %u ms, polling mode\r\n",
                   HDA_IRQ_PROBE_TICKS * 10);
        }
    }
    return 0;
}

/* 连续流播放一个音符：严格追加在真实数据末尾（首尾相接 → 节拍 = 音符时值），
   写入位置与 DMA 读位置保持 HDA_CONT_MARGIN 安全余量；写入后等到
   「本音符剩余 MARGIN 以内」返回，提前量为下一音符写入留出安全距离。
   underrun（读位置越过真实数据末尾）在预留阶段检测、计数、记日志。 */
int hda_cont_play_note(uint32_t rate, uint32_t ch, uint32_t bits,
                       const uint8_t *pcm, uint32_t note_bytes,
                       uint32_t lead_ms, uint32_t tail_ms)
{
    int rc = hda_cont_start(rate, ch, bits);
    if (rc != 0)
        return rc;

    uint32_t bytes_per_sample = (bits / 8) * ch;
    if (bytes_per_sample == 0)
        return -2;
    uint32_t lead_len = (rate * lead_ms / 1000) * bytes_per_sample;
    uint32_t tail_len = (rate * tail_ms / 1000) * bytes_per_sample;
    uint32_t total    = lead_len + note_bytes + tail_len;

    if (total > g_cont_size) {
        printk("HDA: cont note too big (%u > %u)\r\n", total, g_cont_size);
        return -2;
    }

    uint8_t note_id = ++g_cont_note_id;
    hda_trace_log(HTEV_NOTE_SUBMIT, note_id, g_cont_data_end,
                  g_cont_data_end + total);

    uint32_t start = hda_cont_reserve(total, note_id, 1);

    g_cont_wrpos = start;
    hda_cont_zero_at_wrpos(lead_len);            /* lead 静音 */
    /* 软件音量增益：QEMU hda-duplex 不将 codec amp 应用到音频输出，
       需在驱动侧缩放 PCM。gain_q8=256 直写，=0 写零，中间值临时缓冲缩放。
       注：codec amp verb 已在 sys_audio_set_volume 中发送（供真实硬件）。 */
    if (g_audio_gain_q8 >= 256) {
        hda_cont_write_at_wrpos(pcm, note_bytes);     /* 音符 */
    } else if (g_audio_gain_q8 == 0) {
        hda_cont_zero_at_wrpos(note_bytes);           /* 静音 */
    } else {
        uint8_t *tmp = kmalloc(note_bytes);
        if (tmp) {
            memcpy(tmp, pcm, note_bytes);
            int16_t *s = (int16_t *)tmp;
            for (uint32_t i = 0; i < note_bytes / 2; i++)
                s[i] = (int16_t)((s[i] * (int32_t)g_audio_gain_q8) >> 8);
            hda_cont_write_at_wrpos(tmp, note_bytes);
            kfree(tmp);
        } else {
            printk("HDA: gain alloc failed (%u bytes), unscaled\r\n", note_bytes);
            hda_cont_write_at_wrpos(pcm, note_bytes); /* 分配失败：不缩放 */
        }
    }
    hda_cont_zero_at_wrpos(tail_len);            /* tail 静音 */
    g_cont_data_end = g_cont_wrpos;              /* 单调：start ≥ 旧 data_end */
    hda_flush_cache();                           /* 刷缓存使 DMA 可见 */
    hda_trace_log(HTEV_NOTE_WRITTEN, note_id, start, g_cont_data_end);

    hda_cont_finish(note_id);
    return 0;
}

/* 休止符：向连续流追加 ms 毫秒**真实静音数据**（有节拍语义，与 fill_ahead
   的 underrun 兜底预填区分）并等待播放完毕（提前 MARGIN 返回）。 */
int sys_audio_play_silence_ms(uint32_t ms)
{
    if (!g_cont_on)
        return 0;

    uint32_t bps = (g_cont_bits / 8) * g_cont_ch;
    if (bps == 0)
        return 0;
    uint32_t bytes = (uint32_t)((uint64_t)g_cont_rate * ms / 1000) * bps;
    if (bytes == 0)
        bytes = 4;
    if (bytes > g_cont_size)
        bytes = g_cont_size;

    uint8_t note_id = ++g_cont_note_id;
    hda_trace_log(HTEV_NOTE_SUBMIT, note_id, g_cont_data_end,
                  g_cont_data_end + bytes);

    uint32_t start = hda_cont_reserve(bytes, note_id, 1);

    g_cont_wrpos = start;
    hda_cont_zero_at_wrpos(bytes);
    g_cont_data_end = g_cont_wrpos;
    hda_flush_cache();
    hda_trace_log(HTEV_NOTE_WRITTEN, note_id, start, g_cont_data_end);

    hda_cont_finish(note_id);
    return 0;
}

/* 停止连续流：在真实数据末尾追加长尾部静音（吸收停流时 codec/后端缓冲
   截断），等待播放完（提前 MARGIN 返回）再清 RUN。stop 不是音符提交，
   间距无害，故不参与 underrun 计数。 */
int sys_audio_stop(void)
{
    if (!g_cont_on)
        return 0;

    uint32_t bps = (g_cont_bits / 8) * g_cont_ch;
    if (bps == 0) {
        hda_writeb(HDA_SD_CTL(HDA_OUTPUT_STREAM), 0);
        g_cont_on = 0;
        return 0;
    }
    uint32_t tail_len = (uint32_t)((uint64_t)g_cont_rate * HDA_STOP_TAIL_MS / 1000) * bps;

    uint32_t start = hda_cont_reserve(tail_len, 0, 0);
    g_cont_wrpos = start;
    hda_cont_zero_at_wrpos(tail_len);
    g_cont_data_end = g_cont_wrpos;
    hda_flush_cache();

    uint32_t goal = (g_cont_data_end > HDA_CONT_MARGIN)
                    ? g_cont_data_end - HDA_CONT_MARGIN : 0;
    hda_cont_wait_until(goal);

    hda_writeb(HDA_SD_CTL(HDA_OUTPUT_STREAM), 0);
    g_cont_on = 0;
    hda_trace_log(HTEV_STREAM_STOP, 0, start, g_cont_data_end);
    printk("HDA: continuous stream stopped (tail=%u ms, lpib_max_step=%u, "
           "wait=%s bcis=%u, underruns=%u)\r\n",
           HDA_STOP_TAIL_MS, g_cont_lpib_max_step,
           (g_hda_irq_ok == 1) ? "irq" : "poll",
           g_hda_irq_count, g_cont_underruns);

    /* 播放结束：批量导出调用链日志（此时不再有实时性要求） */
    hda_trace_dump();
    return 0;
}

/* 返回流错误计数：(desc_errors << 16) | fifo_errors */
uint32_t sys_audio_get_errors(void)
{
    return (g_cont_desc_errors << 16) | g_cont_fifo_errors;
}

/* 返回 underrun 计数（DMA 读位置越过真实数据末尾的次数，QEMU 下这是
   唯一可观测的播放异常）。dump 非零时顺带导出调用链日志，供评测在
   播放结束后一次性取回全部诊断数据。 */
uint32_t sys_audio_get_underruns(uint32_t dump)
{
    if (dump)
        hda_trace_dump();
    return g_cont_underruns;
}

/* 返回 DMA 已消费的播放时间（ms）。流停止后 rdpos 保留最终值，即总播放时长；
   未启动时 rdpos=0 返回 0，无需检查 g_cont_on。
   用 64 位中间值避免长时间播放时 rdpos/bps*1000 溢出 uint32 */
uint32_t sys_audio_get_position_ms(void)
{
    uint32_t bps = (g_cont_bits / 8) * g_cont_ch;
    if (bps == 0 || g_cont_rate == 0)
        return 0;
    return (uint32_t)((uint64_t)g_cont_rdpos / bps * 1000 / g_cont_rate);
}
