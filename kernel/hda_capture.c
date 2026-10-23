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

/* 探测输入侧拓扑：遍历 0x01..0x20
     widget type == 1 → ADC（取第一个）
     widget type == 4 且 Pin Capabilities 的 INPUT_CAPABLE 置位 → 候选 Mic Pin
       优先选 Config Default 标记为 Mic In 的那个，否则取第一个可输入的 */
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
        printk("HDA: capture probe: no ADC widget on codec %u\r\n", cad);
        return -1;
    }
    if (pin == 0) {
        printk("HDA: capture probe: no input-capable pin\r\n");
        return -1;
    }

    g_hda.adc_nid   = adc;
    g_hda.input_pin = pin;
    printk("HDA: capture topology ADC=0x%02x MicPin=0x%02x (probed)\r\n", adc, pin);

    /* 输入路径：Pin 打开输入使能；ADC 选到该 Pin 的输入索引并 0dB 取消静音 */
    hda_send_verb(cad, pin, HDA_VERB_SET_PIN_WIDGET_CTRL | HDA_PIN_CTRL_IN_EN);

    uint32_t sel = 0;
    uint64_t clen = hda_send_verb(cad, adc,
                                  HDA_VERB_GET_PARAMETER | HDA_PARAM_CONNLIST_LEN);
    if (clen != ~0ULL) {
        uint32_t n = (uint32_t)clen & 0x7F;
        uint32_t longform = ((uint32_t)clen >> 7) & 1;
        /* 短格式每条响应 4 个 8bit NID；长格式（16bit）本轮不处理，
           取默认索引 0（QEMU 的 hda-duplex 是短格式）。 */
        if (!longform && n > 0) {
            for (uint32_t i = 0; i < n; i += 4) {
                uint64_t r = hda_send_verb(cad, adc,
                                           HDA_VERB_GET_CONNECTION_LIST_ENTRY | i);
                if (r == ~0ULL)
                    break;
                uint32_t v = (uint32_t)r;
                for (uint32_t k = 0; k < 4 && i + k < n; k++) {
                    if (((v >> (8 * k)) & 0xFF) == pin) {
                        sel = i + k;
                        break;
                    }
                }
                if (sel != 0 || ((v & 0xFF) == pin))
                    break;
            }
        }
    }
    hda_send_verb(cad, adc, HDA_VERB_SET_CONNECTION_SELECT | (sel & 0x7F));
    hda_send_verb(cad, adc, HDA_VERB_SET_AMP_GAIN_MUTE |
                            HDA_AMP_SET_INPUT | HDA_AMP_UNMUTE_0DB);
    printk("HDA: capture path Pin=0x%02x -> ADC=0x%02x (conn idx %u)\r\n",
           pin, adc, sel);
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

    uint16_t fmt = cap_stream_fmt(rate, ch, bits);
    hda_writew(HDA_SD_FMT(HDA_INPUT_STREAM), fmt);
    hda_send_verb(g_hda.codec_cad, g_hda.adc_nid, HDA_VERB_SET_CONVERTER_FMT | fmt);
    hda_send_verb(g_hda.codec_cad, g_hda.adc_nid,
                  HDA_VERB_SET_CONVERTER_STREAM_CHANNEL | (HDA_INPUT_TAG << 4));

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
    g_cap_rate    = rate;
    g_cap_ch      = ch;
    g_cap_bits    = bits;

    printk("HDA: capture started (SD%d tag=%d CBL=%u, %uHz %uch %ubit)\r\n",
           HDA_INPUT_STREAM, HDA_INPUT_TAG, g_cap_size, rate, ch, bits);
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
