#include <string.h>
#include "kernel.h"
#include "cpu.h"
#include "machdep.h"
#include "hda.h"
#include "hda_internal.h"

/* ===================== HDA 麦克风采集（输入流） =====================
   与 hda_stream.c 的播放路径对称但完全独立：
     - 流描述符用 SD0（QEMU intel-hda 里 SD0~SD3 是输入方向，见 hda.h 注释）
     - stream tag 用 1，播放用 4，互不串流
     - 自己的环形缓冲与 BDL，不碰 g_hda.sample_buf / g_hda.bdl
     - 位置一律读 SDnLPIB（QEMU 3.1 的 BCIS 中断在采集侧不可靠，不引入
       中断路径，避免与播放的中断处理混在一起）

   数据流：Mic Pin --(codec 内部模拟通路)--> ADC --> 输入流 SD0 --> DMA
   写入环缓冲；驱动按「LPIB - 安全余量」推导可交付区间，用户 read 取走。 */

/* 环缓冲：256KB ≈ 1.48s（44.1kHz 立体声 16bit）。内核堆 12MB 里播放的
   WAV 缓存已占约 8.5MB，采集不能按播放 4MB 那种规模取；请求时长超过
   1.2s 时升到 512KB（≈3s），给用户态留更宽松的取数间隔。 */
#define HDA_CAP_BUF_S   0x40000
#define HDA_CAP_BUF_L   0x80000
#define HDA_CAP_SEG     0x1000   /* 每条 4KB：256KB→64 条，512KB→128 条 */

/* 交付安全余量：DMA 写指针与已交付指针之间的最小距离，覆盖 QEMU 1ms
   虚拟时钟粒度 + 驱动读 LPIB 的延迟。16KB ≈ 93ms（44.1k 立体声）。
   方向与播放相反：播放是「别写太快」，采集是「别读太新」。 */
#define HDA_CAP_GAP     0x4000

static int      g_cap_on;
static uint32_t g_cap_size;      /* CBL */
static uint32_t g_cap_rdpos;     /* 已交付用户的绝对字节数 */
static uint32_t g_cap_lpib;      /* 上一次 LPIB（绕回检测） */
static uint32_t g_cap_wraps;     /* LPIB 绕回圈数 */
static uint32_t g_cap_rate = 44100, g_cap_ch = 2, g_cap_bits = 16;
static uint32_t g_cap_bytes, g_cap_samples, g_cap_nonzero, g_cap_peak, g_cap_overruns;

static uint8_t  *g_cap_buf;
static uint32_t  g_cap_buf_pa;
static uint32_t  g_cap_alloc;    /* g_cap_buf 实际分配容量（只增不减，避免重复分配） */
static struct hda_bdl_entry *g_cap_bdl;
static uint32_t  g_cap_bdl_pa;

/* SDnFMT 编码：bits14=base(0=48k/1=44.1k)，bits6:4=位宽，bits3:0=声道-1 */
static uint16_t cap_stream_fmt(uint32_t rate, uint32_t ch, uint32_t bits)
{
    uint16_t fmt;

    switch (bits) {
    case 8:  fmt = HDA_FMT_BITS_8;  break;
    case 20: fmt = HDA_FMT_BITS_20; break;
    case 24: fmt = HDA_FMT_BITS_24; break;
    case 32: fmt = HDA_FMT_BITS_32; break;
    default: fmt = HDA_FMT_BITS_16; break;
    }
    fmt |= (rate == 44100) ? HDA_FMT_BASE_44 : HDA_FMT_BASE_48;
    fmt |= (ch == 2) ? HDA_FMT_CHAN_STEREO : HDA_FMT_CHAN_MONO;
    return fmt;
}

/* SDnFMT / converter format 的 bit6:4 → 位宽（回读仲裁用） */
static uint32_t cap_fmt_bits(uint16_t fmt)
{
    switch (fmt & 0x70) {
    case HDA_FMT_BITS_8:  return 8;
    case HDA_FMT_BITS_20: return 20;
    case HDA_FMT_BITS_24: return 24;
    case HDA_FMT_BITS_32: return 32;
    default:              return 16;
    }
}

/* 该 Pin 是否被配置为 Mic In（Config Default bits23:20 == 0xA） */
static int cap_is_mic_pin(uint8_t cad, uint8_t nid)
{
    uint64_t cfg = hda_send_verb(cad, nid,
                                 HDA_VERB_GET_PARAMETER | HDA_PARAM_CONFIG_DEFAULT);
    if (cfg == ~0ULL)
        return 0;
    return (((uint32_t)cfg >> 20) & 0xF) == HDA_CFG_DEVICE_MIC_IN;
}

static int cap_pin_input_capable(uint8_t cad, uint8_t nid)
{
    uint64_t pc = hda_send_verb(cad, nid,
                                HDA_VERB_GET_PARAMETER | HDA_PARAM_PIN_CAP);
    if (pc == ~0ULL)
        return 0;
    return ((uint32_t)pc & HDA_PINCAP_INPUT) != 0;
}

/* ===================== verb 读写校验辅助 ===================== */

/* IC 立即命令偶发超时（QEMU 在复位/高负载下会出现），一次重发即可；
   两次都失败才认定为通路故障，避免把瞬时超时误判成配置失败。 */
static uint64_t cap_verb(uint8_t cad, uint8_t nid, uint32_t verb)
{
    for (int i = 0; i < 2; i++) {
        uint64_t r = hda_send_verb(cad, nid, verb);
        if (r != ~0ULL)
            return r;
    }
    return ~0ULL;
}

/* 写入后回读比对：mask 之外的位忽略。
   fatal=1 → 不符返回 err（停用采集）；fatal=0 → 仅告警（规范允许 widget 无 amp）。 */
static int cap_verify(uint8_t cad, uint8_t nid, uint32_t read_verb,
                      uint32_t expect, uint32_t mask, int fatal, int err,
                      const char *what)
{
    uint64_t r = cap_verb(cad, nid, read_verb);

    if (r == ~0ULL) {
        printk("HDA-CAP: %s readback timeout (nid=0x%02x)\r\n", what, nid);
        return fatal ? err : 0;
    }
    if (((uint32_t)r & mask) != (expect & mask)) {
        printk("HDA-CAP: %s mismatch nid=0x%02x got=0x%x want=0x%x (%s)\r\n",
               what, nid, (uint32_t)r & mask, expect & mask,
               fatal ? "fatal" : "warn");
        return fatal ? err : 0;
    }
    return 0;
}

/* 求 pin 在 adc 连接列表中的索引：>=0 命中，-1 未命中，-2 verb 读取失败。
   短格式每条响应 4 个 8bit NID（QEMU hda-duplex 用这个）；长格式 2 个
   16bit NID，按规范实现但未在真机上验证。
   旧实现（9ccf800）的两个缺陷在这里修掉：长格式一律取索引 0；
   命中判断 `if (sel != 0 || (v & 0xFF) == pin)` 在「索引 0 命中」与
   「完全未命中」两种情形下混淆。 */
static int cap_conn_index(uint8_t cad, uint8_t adc, uint8_t pin)
{
    uint64_t clen = cap_verb(cad, adc,
                             HDA_VERB_GET_PARAMETER | HDA_PARAM_CONNLIST_LEN);
    if (clen == ~0ULL)
        return -2;

    uint32_t n        = (uint32_t)clen & 0x7F;
    uint32_t longform = ((uint32_t)clen >> 7) & 1;
    uint32_t step     = longform ? 2 : 4;

    for (uint32_t off = 0; off < n; off += step) {
        uint64_t r = cap_verb(cad, adc, HDA_VERB_GET_CONNECTION_LIST_ENTRY | off);
        if (r == ~0ULL)
            return -2;

        uint32_t v = (uint32_t)r;
        if (longform) {
            for (uint32_t k = 0; k < 2 && off + k < n; k++)
                if (((v >> (16 * k)) & 0xFFFF) == pin)
                    return (int)(off + k);
        } else {
            for (uint32_t k = 0; k < 4 && off + k < n; k++)
                if (((v >> (8 * k)) & 0xFF) == pin)
                    return (int)(off + k);
        }
    }
    return -1;
}

/* 探测输入侧拓扑：遍历 0x01..0x20
    widget type == 1 → ADC（取第一个）
    widget type == 4 且 Pin Capabilities 的 INPUT_CAPABLE 置位 → 候选 Mic Pin
      优先选 Config Default 标记为 Mic In 的那个，否则取第一个可输入的

   探测完成后立即把「Pin 输入使能 → ADC 连接选择 → 输入 amp 取消静音」配好，
   每一步都回读校验：PINCTL/CONVSEL 不符则判定通路不可用（返回负码），
   amp 只告警（HDA 规范允许 widget 不带 amp）。 */
int hda_capture_probe(uint8_t cad)
{
    uint8_t adc = 0, mic = 0, any_in = 0;

    for (uint32_t nid = 1; nid <= 0x20; nid++) {
        uint64_t cap = hda_send_verb(cad, nid,
                                     HDA_VERB_GET_PARAMETER | HDA_PARAM_AUDIO_WIDGET_CAP);
        if (cap == ~0ULL)
            continue;   /* IC 超时：该 NID 不存在时会超时，不是错误 */

        uint32_t type = ((uint32_t)cap >> 20) & 0xF;
        if (type == WIDGET_TYPE_AUDIO_INPUT) {
            if (adc == 0)
                adc = (uint8_t)nid;
        } else if (type == WIDGET_TYPE_PIN_COMPLEX) {
            if (!cap_pin_input_capable(cad, (uint8_t)nid))
                continue;
            if (any_in == 0)
                any_in = (uint8_t)nid;
            if (mic == 0 && cap_is_mic_pin(cad, (uint8_t)nid))
                mic = (uint8_t)nid;
        }
    }

    uint8_t pin = (mic != 0) ? mic : any_in;
    if (adc == 0) {
        printk("HDA-CAP: no ADC widget on codec %u\r\n", cad);
        return HDA_CAP_ERR_NO_ADC;
    }
    if (pin == 0) {
        printk("HDA-CAP: no input-capable pin on codec %u\r\n", cad);
        return HDA_CAP_ERR_NO_PIN;
    }

    g_hda.adc_nid   = adc;
    g_hda.input_pin = pin;
    printk("HDA-CAP: topology ADC=0x%02x MicPin=0x%02x (probed)\r\n", adc, pin);

    /* 1. 连接列表定位：定位不到就说明 ADC 收的不是这个 Pin 的信号 */
    int idx = cap_conn_index(cad, adc, pin);
    if (idx < 0) {
        printk("HDA-CAP: conn list lookup failed rc=%d (ADC=0x%02x Pin=0x%02x)\r\n",
               idx, adc, pin);
        return HDA_CAP_ERR_CONN;
    }

    /* 2. Pin 输入使能 + 回读 */
    cap_verb(cad, pin, HDA_VERB_SET_PIN_WIDGET_CTRL | HDA_PIN_CTRL_IN_EN);
    int rc = cap_verify(cad, pin, HDA_VERB_GET_PIN_WIDGET_CTRL,
                        HDA_PIN_CTRL_IN_EN, HDA_PIN_CTRL_IN_EN,
                        1, HDA_CAP_ERR_PINCTL, "pin ctrl IN_EN");
    if (rc != 0)
        return rc;

    /* 3. ADC 连接选择 + 回读 */
    cap_verb(cad, adc, HDA_VERB_SET_CONNECTION_SELECT | ((uint32_t)idx & 0x7F));
    rc = cap_verify(cad, adc, HDA_VERB_GET_CONNECTION_SELECT,
                    (uint32_t)idx, 0x7F, 1, HDA_CAP_ERR_CONVSEL, "conn select");
    if (rc != 0)
        return rc;

    /* 4. 输入方向取消静音：必须用 0x7xxx（bit15/bit14 互斥，
       旧代码 0xB000|(1<<14)=0xF000 会被 codec 当成「输出」方向，等于没配）。
       ADC 与 Mic Pin 各配一次，amp 缺失只告警。 */
    cap_verb(cad, adc, HDA_VERB_SET_AMP_GAIN_MUTE | HDA_AMP_IN_UNMUTE_0DB);
    cap_verify(cad, adc, HDA_VERB_GET_AMP_GAIN_MUTE | HDA_AMP_GET_IN_BOTH,
               0, HDA_AMP_MUTE_BOTH, 0, 0, "adc input amp");
    cap_verb(cad, pin, HDA_VERB_SET_AMP_GAIN_MUTE | HDA_AMP_IN_UNMUTE_0DB);
    cap_verify(cad, pin, HDA_VERB_GET_AMP_GAIN_MUTE | HDA_AMP_GET_IN_BOTH,
               0, HDA_AMP_MUTE_BOTH, 0, 0, "pin input amp");

    printk("HDA-CAP: path ok Pin=0x%02x -> ADC=0x%02x (conn idx %d), input amp unmuted\r\n",
           pin, adc, idx);
    return 0;
}

/* 复位并停止输入流（SRST 脉冲），可在 start 前/stop 时调用 */
static void cap_stream_reset(void)
{
    hda_writeb(HDA_SD_CTL(HDA_INPUT_STREAM), 0);
    hda_writeb(HDA_SD_CTL(HDA_INPUT_STREAM), HDA_SDCTL_SRST);
    while (hda_readb(HDA_SD_CTL(HDA_INPUT_STREAM)) & HDA_SDCTL_SRST)
        ;
    hda_writeb(HDA_SD_CTL(HDA_INPUT_STREAM), 0);
}

int hda_capture_start(uint32_t rate, uint32_t ch, uint32_t bits, uint32_t ms)
{
    if (g_hda.adc_nid == 0 || g_hda.input_pin == 0) {
        printk("HDA: capture start: topology not probed\r\n");
        return -1;
    }
    if (g_cap_on) {
        /* 幂等：参数一致直接成功；不一致要求先 stop，避免在旧环上换格式 */
        if (rate == g_cap_rate && ch == g_cap_ch && bits == g_cap_bits)
            return 0;
        printk("HDA: capture start rejected: active (%uHz %uch %ubit)\r\n",
               g_cap_rate, g_cap_ch, g_cap_bits);
        return -2;
    }

    uint32_t size = (ms > 1200) ? HDA_CAP_BUF_L : HDA_CAP_BUF_S;
    if (g_cap_buf == NULL || g_cap_alloc < size) {
        uint8_t *va = hda_alloc_dma(size, &g_cap_buf_pa);
        if (va == NULL) {
            printk("HDA: capture DMA alloc failed (%u bytes)\r\n", size);
            return -3;
        }
        /* 升级到更大缓冲时旧块不再回收（内核堆无 free 路径给 DMA 缓冲），
           每个镜像最多发生一次 256KB→512KB 的升级。 */
        g_cap_buf   = va;
        g_cap_alloc = size;
    }
    g_cap_size = size;
    memset(g_cap_buf, 0, g_cap_size);

    if (g_cap_bdl == NULL) {
        g_cap_bdl = (struct hda_bdl_entry *)hda_alloc_dma(
            sizeof(struct hda_bdl_entry) * HDA_BDL_ENTRIES, &g_cap_bdl_pa);
        if (g_cap_bdl == NULL) {
            printk("HDA: capture BDL alloc failed\r\n");
            return -3;
        }
    }

    /* BDL 首尾相接铺满环缓冲。采集侧不需要 IOC：位置一律读 LPIB，
       开中断只会让播放的 BCIS 统计混入无关事件。 */
    uint32_t entries = g_cap_size / HDA_CAP_SEG;
    for (uint32_t i = 0; i < entries; i++) {
        g_cap_bdl[i].addr_lo = g_cap_buf_pa + i * HDA_CAP_SEG;
        g_cap_bdl[i].addr_hi = 0;
        g_cap_bdl[i].length  = HDA_CAP_SEG;
        g_cap_bdl[i].flags   = 0;
    }
    for (uint32_t i = entries; i < HDA_BDL_ENTRIES; i++)
        memset(&g_cap_bdl[i], 0, sizeof(g_cap_bdl[i]));

    cap_stream_reset();

    hda_writel(HDA_SD_BDPL(HDA_INPUT_STREAM), g_cap_bdl_pa);
    hda_writel(HDA_SD_BDPU(HDA_INPUT_STREAM), 0);
    hda_writel(HDA_SD_CBL(HDA_INPUT_STREAM), g_cap_size);
    hda_writew(HDA_SD_LVI(HDA_INPUT_STREAM), (uint16_t)(entries - 1));

    /* ---- ADC 绑定：converter format + stream tag，逐条回读校验 ----
       放在任何一方 RUN 之前，失败可以干净回滚（SRST 复位输入流）。
       回读策略：
         - IC 超时（~0ULL）      → 致命，codec 无响应
         - 回读值 0              → 该 widget 不支持回读（QEMU 常见），
                                   只告警并沿用请求值，绝不因此判死
         - 回读值与请求值不一致  → 以回读值为生效值（绝静默沿用请求值），
                                   并把生效值写进 SDnFMT，保证两侧一致 */
    uint16_t fmt      = cap_stream_fmt(rate, ch, bits);
    uint32_t eff_rate = rate, eff_ch = ch, eff_bits = bits;

    cap_verb(g_hda.codec_cad, g_hda.adc_nid, HDA_VERB_SET_CONVERTER_FMT | fmt);
    {
        uint64_t rb = cap_verb(g_hda.codec_cad, g_hda.adc_nid,
                               HDA_VERB_GET_CONVERTER_FMT);
        if (rb == ~0ULL) {
            printk("HDA-CAP: converter format readback timeout (ADC=0x%02x)\r\n",
                   g_hda.adc_nid);
            cap_stream_reset();
            return HDA_CAP_ERR_FMT;
        }
        uint16_t got = (uint16_t)rb;
        if (got == 0) {
            printk("HDA-CAP: converter format readback unsupported, use requested 0x%04x\r\n",
                   fmt);
        } else if ((got & 0x407F) != (fmt & 0x407F)) {
            eff_rate = (got & HDA_FMT_BASE_44) ? 44100 : 48000;
            eff_ch   = (uint32_t)(got & 0x0F) + 1;
            eff_bits = cap_fmt_bits(got);
            printk("HDA-CAP: converter format adopted 0x%04x (want 0x%04x): %uHz %uch %ubit\r\n",
                   got, fmt, eff_rate, eff_ch, eff_bits);
            fmt = got;
        }
    }
    /* 控制器侧用生效格式，避免 codec 与流描述符各说一套（重采样时采样率会错） */
    hda_writew(HDA_SD_FMT(HDA_INPUT_STREAM), fmt);

#ifdef CAP_REVERSE_SKIP_ADC_BIND
    /* 反向门（验收专用，-DCAP_REVERSE_SKIP_ADC_BIND）：故意不把 ADC 绑到输入流，
       预期采集静音——用来证明这一步是数据通路的前提而不是摆设。 */
    printk("HDA-CAP: [reverse-gate] skip ADC stream tag bind\r\n");
#else
    cap_verb(g_hda.codec_cad, g_hda.adc_nid,
             HDA_VERB_SET_CONVERTER_STREAM_CHANNEL | (HDA_INPUT_TAG << 4));
#endif
    {
        uint64_t rb = cap_verb(g_hda.codec_cad, g_hda.adc_nid, HDA_VERB_GET_CONV);
        if (rb == ~0ULL) {
            printk("HDA-CAP: converter stream/channel readback timeout\r\n");
            cap_stream_reset();
            return HDA_CAP_ERR_TAG;
        }
        uint32_t got = (uint32_t)rb & 0xFF;
        if (got == 0) {
            printk("HDA-CAP: converter stream/channel readback unsupported, keep tag=%d\r\n",
                   HDA_INPUT_TAG);
        } else if (got != (uint32_t)(HDA_INPUT_TAG << 4)) {
            printk("HDA-CAP: stream tag mismatch got=0x%02x want=0x%02x (ADC 不会取到本流数据)\r\n",
                   got, (uint32_t)(HDA_INPUT_TAG << 4));
            cap_stream_reset();
            return HDA_CAP_ERR_TAG;
        }
    }

    /* 启动：先写 tag（CTL+2），再置 RUN；与播放路径同一时序 */
    hda_flush_cache();
    hda_writeb(HDA_SD_CTL(HDA_INPUT_STREAM) + 1, 0);
    hda_writeb(HDA_SD_CTL(HDA_INPUT_STREAM) + 2, (uint8_t)(HDA_INPUT_TAG << 4));
    hda_writeb(HDA_SD_CTL(HDA_INPUT_STREAM),
               HDA_SDCTL_FEIE | HDA_SDCTL_DEIE | HDA_SDCTL_RUN);

    g_cap_on      = 1;
    g_cap_rdpos   = 0;
    g_cap_lpib    = 0;
    g_cap_wraps   = 0;
    g_cap_bytes   = 0;
    g_cap_samples = 0;
    g_cap_nonzero = 0;
    g_cap_peak    = 0;
    g_cap_overruns = 0;
    g_cap_rate    = eff_rate;
    g_cap_ch      = eff_ch;
    g_cap_bits    = eff_bits;

    printk("HDA-CAP: stream started (SD%d tag=%d CBL=%u, %uHz %uch %ubit fmt=0x%04x)\r\n",
           HDA_INPUT_STREAM, HDA_INPUT_TAG, g_cap_size, eff_rate, eff_ch, eff_bits, fmt);
    hda_trace_log(HTEV_CAP_START, 0, g_cap_size, 0);
    return 0;
}

/* 读取已采集数据：非阻塞，无数据返回 0，调用方自行 sleep 后重试。
   统计在同一次遍历里顺带算完，不额外扫描缓冲。 */
int hda_capture_read(void *ubuf, uint32_t len, struct audio_capture_stats *ust)
{
    if (!g_cap_on)
        return -1;

    uint32_t lpib = hda_readl(HDA_SD_LPIB(HDA_INPUT_STREAM));
    if (lpib < g_cap_lpib)
        g_cap_wraps++;
    g_cap_lpib = lpib;
    uint32_t wp = g_cap_wraps * g_cap_size + lpib;

    /* 溢出：DMA 已绕到读指针后面一圈以上（用户读太慢）。不静默覆盖，
       把读指针追到「写指针 - 余量」并计数。 */
    if (wp - g_cap_rdpos > g_cap_size) {
        g_cap_rdpos = wp - HDA_CAP_GAP;
        g_cap_overruns++;
        hda_trace_log(HTEV_CAP_OVERFLOW, 0, g_cap_rdpos, wp);
    }

    uint32_t avail = (wp > g_cap_rdpos + HDA_CAP_GAP)
                     ? (wp - g_cap_rdpos - HDA_CAP_GAP) : 0;
    uint32_t n = (len < avail) ? len : avail;

    if (n > 0 && ubuf != NULL) {
        uint32_t off = g_cap_rdpos % g_cap_size;
        uint32_t first = (n < g_cap_size - off) ? n : (g_cap_size - off);
        memcpy(ubuf, g_cap_buf + off, first);
        if (n > first)
            memcpy((uint8_t *)ubuf + first, g_cap_buf, n - first);

        /* 统计按 16bit 小端样本；非 16bit 格式下仅作粗粒度参考 */
        const uint8_t *p = (const uint8_t *)ubuf;
        for (uint32_t i = 0; i + 1 < n; i += 2) {
            int v = (int)(int16_t)(p[i] | (p[i + 1] << 8));
            if (v != 0) {
                g_cap_nonzero++;
                if (v < 0)
                    v = -v;
                if ((uint32_t)v > g_cap_peak)
                    g_cap_peak = (uint32_t)v;
            }
        }
        g_cap_bytes   += n;
        g_cap_samples += n / 2;
        g_cap_rdpos   += n;
    }

    if (ust != NULL) {
        memset(ust, 0, sizeof(*ust));
        ust->bytes_total   = g_cap_bytes;
        ust->samples_total = g_cap_samples;
        ust->nonzero       = g_cap_nonzero;
        ust->peak          = g_cap_peak;
        ust->overruns      = g_cap_overruns;
        ust->lpib          = g_cap_lpib;
        ust->wraps         = g_cap_wraps;
    }
    return (int)n;
}

int hda_capture_stop(void)
{
    if (!g_cap_on)
        return 0;

    cap_stream_reset();
    g_cap_on = 0;

    printk("HDA: capture stopped (bytes=%u samples=%u nonzero=%u peak=%u overruns=%u)\r\n",
           g_cap_bytes, g_cap_samples, g_cap_nonzero, g_cap_peak, g_cap_overruns);
    hda_trace_log(HTEV_CAP_STOP, 0, g_cap_bytes, g_cap_overruns);
    return 0;
}

int hda_capture_is_on(void)
{
    return g_cap_on;
}

/* ===================== 配置快照（串口证据） =====================
   单行 key=value 输出 codec 侧输入路径的**回读现状**（不是我们写下去的值），
   供串口解析与人工复核；采集未启动时也能调用。
   返回 0 表示全部关键项读取成功，否则返回首个失败项对应的负码。 */
#define CAP_RB(v) (((v) == ~0ULL) ? 0xFFFFFFFFu : (uint32_t)(v))

int hda_capture_dump_state(void)
{
    uint8_t cad = g_hda.codec_cad;
    uint8_t adc = g_hda.adc_nid;
    uint8_t pin = g_hda.input_pin;

    if (adc == 0 || pin == 0) {
        printk("HDA-CAP: state adc=none pin=none\r\n");
        return HDA_CAP_ERR_NO_ADC;
    }

    uint32_t pctl = CAP_RB(cap_verb(cad, pin, HDA_VERB_GET_PIN_WIDGET_CTRL));
    uint32_t sel  = CAP_RB(cap_verb(cad, adc, HDA_VERB_GET_CONNECTION_SELECT));
    uint32_t fmt  = CAP_RB(cap_verb(cad, adc, HDA_VERB_GET_CONVERTER_FMT));
    uint32_t conv = CAP_RB(cap_verb(cad, adc, HDA_VERB_GET_CONV));
    uint32_t aamp = CAP_RB(cap_verb(cad, adc,
                          HDA_VERB_GET_AMP_GAIN_MUTE | HDA_AMP_GET_IN_BOTH));
    uint32_t pamp = CAP_RB(cap_verb(cad, pin,
                          HDA_VERB_GET_AMP_GAIN_MUTE | HDA_AMP_GET_IN_BOTH));
    uint32_t sdc  = (uint32_t)hda_readb(HDA_SD_CTL(HDA_INPUT_STREAM))
                    | ((uint32_t)hda_readb(HDA_SD_CTL(HDA_INPUT_STREAM) + 1) << 8)
                    | ((uint32_t)hda_readb(HDA_SD_CTL(HDA_INPUT_STREAM) + 2) << 16);

    /* 0xFFFFFFFF = 回读超时；未实现回读的 widget 返回 0（QEMU 常见） */
    printk("HDA-CAP: state adc=0x%02x pin=0x%02x pinctrl=0x%02x conn=%u fmt=0x%04x "
           "conv=0x%02x amp_adc=0x%04x amp_pin=0x%04x sd%u_ctl=0x%06x\r\n",
           adc, pin, pctl & 0xFF, sel & 0x7F, fmt & 0xFFFF, conv & 0xFF,
           aamp & 0xFFFF, pamp & 0xFFFF, HDA_INPUT_STREAM, sdc);

    if (pctl == 0xFFFFFFFFu) return HDA_CAP_ERR_PINCTL;
    if (sel  == 0xFFFFFFFFu) return HDA_CAP_ERR_CONVSEL;
    if (fmt  == 0xFFFFFFFFu) return HDA_CAP_ERR_FMT;
    if (conv == 0xFFFFFFFFu) return HDA_CAP_ERR_TAG;
    return 0;
}

/* ===================== syscall 层 ===================== */
int sys_audio_capture_start(uint32_t rate, uint32_t ch, uint32_t bits, uint32_t ms)
{
    return hda_capture_start(rate, ch, bits, ms);
}

int sys_audio_capture_read(void *buf, uint32_t len, void *ustats)
{
    return hda_capture_read(buf, len, (struct audio_capture_stats *)ustats);
}

int sys_audio_capture_stop(void)
{
    return hda_capture_stop();
}
