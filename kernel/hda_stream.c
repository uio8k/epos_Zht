#include <string.h>
#include "kernel.h"
#include "cpu.h"
#include "hda.h"
#include "hda_internal.h"

/* ===================== 连续流播放（消除逐音启停 pop） =====================
   方案：输出流启动后保持 RUN=1 不停止——宿主音频后端 voice 持续激活，
   不再每次启停流产生 start/stop pop（wav 捕获已证明 PCM 本身干净）。
   音符按 [lead静音][音符][tail静音] 依序写入固定大小的环形 DMA 缓冲
   （wrpos 前进），DMA 持续循环读取；写入前等待缓冲有空间（前一个音符
   已被消费），写入后等待本音符播放完毕再返回——节奏与旧"逐音启停"一致，
   但流永不停止。休止符用 sys_audio_play_silence_ms 写纯静音；
   演示结束用 sys_audio_stop 等待已写入数据播完再清 RUN（尾哨兵吸收截断）。 */
#define HDA_CONT_BUF_SIZE  0x80000   /* 512KB 环形缓冲（> 最长音符 1600ms+哨兵） */
#define HDA_CONT_FILL_MS   500       /* 提前填零前瞻：覆盖音符间读盘/排序开销 */

static int       g_cont_on     = 0;   /* 连续流是否已启动（RUN=1） */
static uint32_t  g_cont_size   = 0;   /* 环形缓冲大小 S（= CBL） */
static uint32_t  g_cont_wrpos  = 0;   /* 下一次写入/填零位置（绝对字节） */
static uint32_t  g_cont_data_end = 0; /* 最后一段真实数据（音符/静音）末尾 */
static uint32_t  g_cont_rdpos  = 0;   /* DMA 已消费的绝对字节数 */
static uint32_t  g_cont_lpib   = 0;   /* 上一次 LPIB（绕回检测） */
static uint32_t  g_cont_wraps  = 0;   /* DMA 绕回次数 */
static uint32_t  g_cont_rate   = 44100;
static uint32_t  g_cont_ch     = 2;
static uint32_t  g_cont_bits   = 16;

/* 等待 DMA 消费 / 缓冲空间的轮询超时（迭代次数，约 5s） */
#define HDA_CONT_WAIT_TIMEOUT 50000000

/* 停流前追加的尾部静音（吸收停流时 codec/后端缓冲截断） */
#define HDA_STOP_TAIL_MS 250

/* 流错误计数器（供 sys_audio_get_errors 回读） */
static uint32_t g_cont_fifo_errors = 0;
static uint32_t g_cont_desc_errors = 0;

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

/* 读取 LPIB 更新 g_cont_rdpos（绕回感知），附带错误检测 */
static void hda_cont_refresh_rd(void)
{
    uint32_t lpib = hda_readl(HDA_SD_LPIB(HDA_OUTPUT_STREAM));
    if (lpib < g_cont_lpib)
        g_cont_wraps++;   /* DMA 绕回一圈 */
    g_cont_lpib = lpib;
    g_cont_rdpos = g_cont_wraps * g_cont_size + lpib;
    hda_cont_check_errors();
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

/* 未消费字节数（正确处理 rd > wr 的无符号下溢：DMA 已越过全部已写数据则视为 0） */
static uint32_t hda_cont_inflight(void)
{
    if (g_cont_wrpos >= g_cont_rdpos)
        return g_cont_wrpos - g_cont_rdpos;
    return 0;
}

/* 把 DMA 即将读取的前方区域填零（静音），前瞻 HDA_CONT_FILL_MS。
   这样音符之间（读盘/排序开销）DMA 读到的始终是静音，而不是旧数据。 */
static void hda_cont_fill_ahead(void)
{
    uint32_t bps = (g_cont_bits / 8) * g_cont_ch;
    uint32_t fill = (uint32_t)((uint64_t)g_cont_rate * HDA_CONT_FILL_MS / 1000) * bps;
    uint32_t target = g_cont_rdpos + fill;
    while (g_cont_wrpos < target && hda_cont_inflight() < g_cont_size)
        hda_cont_zero_at_wrpos(64);
}

/* 等待 DMA 消费到 target 绝对字节位置，期间持续把前方填零（上限 ~5s） */
static void hda_cont_wait_and_fill(uint32_t target)
{
    int timeout = HDA_CONT_WAIT_TIMEOUT;
    while (timeout-- > 0) {
        hda_cont_refresh_rd();
        hda_cont_fill_ahead();
        if (g_cont_rdpos >= target)
            break;
    }
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

    /* BDL 指向环形缓冲，拆成 ≤64KB 的段 */
    uint32_t buf_phys = vtop((uint32_t)g_hda.sample_buf);
    uint32_t remaining = g_cont_size;
    uint32_t offset = 0;
    int bdl_entries = 0;
    for (int i = 0; i < HDA_BDL_ENTRIES && remaining > 0; i++) {
        uint32_t chunk = (remaining > 0x10000) ? 0x10000 : remaining;
        g_hda.bdl[i].addr_lo = buf_phys + offset;
        g_hda.bdl[i].addr_hi = 0;
        g_hda.bdl[i].length  = chunk;
        g_hda.bdl[i].flags   = (remaining == chunk) ? 0x01 : 0x00;
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
    return 0;
}

/* 连续流播放一个音符：先把写位置对齐到 DMA 当前读位置（使音符立即被播放），
   再写入 [lead][note][tail]，等待播放完毕（保持流运行）。 */
int hda_cont_play_note(uint32_t rate, uint32_t ch, uint32_t bits,
                       const uint8_t *pcm, uint32_t note_bytes,
                       uint32_t lead_ms, uint32_t tail_ms)
{
    int rc = hda_cont_start(rate, ch, bits);
    if (rc != 0)
        return rc;

    uint32_t bytes_per_sample = (bits / 8) * ch;
    uint32_t lead_len = (rate * lead_ms / 1000) * bytes_per_sample;
    uint32_t tail_len = (rate * tail_ms / 1000) * bytes_per_sample;
    uint32_t total    = lead_len + note_bytes + tail_len;

    if (total > g_cont_size) {
        printk("HDA: cont note too big (%u > %u)\r\n", total, g_cont_size);
        return -2;
    }

    /* 对齐到 DMA 当前读位置：音符紧随 DMA 写入，立即播放、无缝衔接 */
    g_cont_wrpos = g_cont_rdpos;

    /* 等待缓冲有空间（防御：对齐后通常立即有空间） */
    int timeout = HDA_CONT_WAIT_TIMEOUT;
    while (timeout-- > 0) {
        hda_cont_refresh_rd();
        if (hda_cont_inflight() <= g_cont_size - total)
            break;
    }

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
    g_cont_data_end = g_cont_wrpos;

    /* 等待本音符播放完毕（保持流运行，不停止），期间把前方填零 */
    hda_cont_wait_and_fill(g_cont_data_end);
    return 0;
}

/* 休止符：向连续流写入 ms 毫秒纯静音并等待播放完毕 */
int sys_audio_play_silence_ms(uint32_t ms)
{
    if (!g_cont_on)
        return 0;

    uint32_t bps = (g_cont_bits / 8) * g_cont_ch;
    uint32_t bytes = (uint32_t)((uint64_t)g_cont_rate * ms / 1000) * bps;
    if (bytes == 0)
        bytes = 4;

    g_cont_wrpos = g_cont_rdpos;
    hda_cont_zero_at_wrpos(bytes);
    g_cont_data_end = g_cont_wrpos;

    hda_cont_wait_and_fill(g_cont_data_end);
    return 0;
}

/* 停止连续流：在已写入数据末尾追加长尾部静音（吸收停流时 codec/后端缓冲截断），
   等待播完再清 RUN。连续流模式的音符尾哨兵只有 10ms（HDA_CONT_SILENCE_TAIL_MS），
   停流时需要更长的尾部静音（参考 one-shot 的 250ms）。 */
int sys_audio_stop(void)
{
    if (!g_cont_on)
        return 0;

    /* 追加尾部静音，防止停流切断音频末尾 */
    uint32_t bps = (g_cont_bits / 8) * g_cont_ch;
    uint32_t tail_len = (uint32_t)((uint64_t)g_cont_rate * HDA_STOP_TAIL_MS / 1000) * bps;

    g_cont_wrpos = g_cont_rdpos;  /* 对齐到 DMA 当前位置（数据已播放完，通常在 data_end 附近） */
    hda_cont_zero_at_wrpos(tail_len);
    hda_flush_cache();
    uint32_t stop_data_end = g_cont_wrpos;

    hda_cont_wait_and_fill(stop_data_end);
    hda_writeb(HDA_SD_CTL(HDA_OUTPUT_STREAM), 0);
    g_cont_on = 0;
    printk("HDA: continuous stream stopped (tail=%u ms)\r\n", HDA_STOP_TAIL_MS);
    return 0;
}

/* 返回流错误计数：(desc_errors << 16) | fifo_errors */
uint32_t sys_audio_get_errors(void)
{
    return (g_cont_desc_errors << 16) | g_cont_fifo_errors;
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
