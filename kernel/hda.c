#include <stddef.h>
#include "kernel.h"
#include "cpu.h"
#include "bitmap.h"
#include "hda.h"
#include "hda_internal.h"

// 全局状态
struct hda_dev g_hda;
int g_hda_quiet = 0;

/* IC verb 响应轮询超时（迭代次数） */
#define HDA_IC_TIMEOUT 1000000

/* 刷新 CPU 写回缓存，使 DMA 缓冲区数据对 QEMU 的 PCI DMA 可见。
   EPOS 分页映射用 L2E_V|L2E_W（可缓存），guest 写入在 CPU 缓存里，
   QEMU 的 DMA 直接读物理内存会读到旧数据。
   必须在触发 DMA 前调用。 */
void hda_flush_cache(void)
{
    __asm__ __volatile__("wbinvd" ::: "memory");
}

/* QEMU 3.1 的 PCI DMA（pci_dma_read/pci_dma_rw）读不到 1MB 以下的 guest
   物理内存（数据在 RAM 里但 DMA 读到全 0，监视器 xp 却能读到）。
   因此所有 DMA 缓冲区（BDL、音频数据）必须放在 1MB 以上的物理地址。
   这里从 4MB 开始分配。 */
#define HDA_DMA_BASE  0x400000

/* 在 1MB 以上的物理地址分配 DMA 缓冲区，返回 VA，*pa_out 返回物理地址。
   每次分配后推进 base，避免多次分配冲突。 */
static uint32_t hda_dma_next = HDA_DMA_BASE;
uint8_t *hda_alloc_dma(uint32_t bytes, uint32_t *pa_out)
{
    uint32_t pages = PAGE_ROUNDUP(bytes) / PAGE_SIZE;
    uint32_t pa = frame_alloc_in_addr(hda_dma_next, pages);
    if (pa == BITMAP_ERROR) {
        printk("HDA: high DMA alloc @0x%x failed, falling back\r\n", hda_dma_next);
        pa = frame_alloc(pages);
    } else {
        hda_dma_next = pa + pages * PAGE_SIZE;
    }
    if (pa == BITMAP_ERROR) {
        printk("HDA: DMA alloc failed (%u pages)\r\n", pages);
        return NULL;
    }
    uint8_t *va = (uint8_t *)page_alloc(pages, VM_PROT_RW, 0);
    if (va == (uint8_t *)SIZE_MAX) {
        printk("HDA: page_alloc failed for DMA buf\r\n");
        frame_free(pa, pages);
        return NULL;
    }
    page_map((uint32_t)va, pa, pages, L2E_V | L2E_W);
    *pa_out = pa;
    return va;
}

/* 确保 g_hda.sample_buf 有至少 bytes 字节（页对齐）的可写 DMA 缓冲区。
   若现有缓冲区够大则复用——排序演示每换一次就播一个音符，逐音新分配
   会迅速耗尽物理内存（每个音符 ~370KB，约 80 个音符后 frame_alloc 失败），
   复用后整个演示只需 1~2 块 DMA 缓冲。 */
uint8_t *hda_ensure_dma_buf(uint32_t bytes)
{
    if (g_hda.sample_buf != NULL && g_hda.buf_size >= bytes)
        return g_hda.sample_buf;

    uint32_t pa = 0;
    uint8_t *va = hda_alloc_dma(bytes, &pa);
    if (va == NULL)
        return NULL;
    g_hda.sample_buf = va;
    g_hda.buf_size   = bytes;
    return va;
}

uint64_t hda_send_verb(uint8_t cad, uint8_t nid, uint32_t verb){
    uint32_t cmd = ((cad & 0xF) << 28) | ((nid & 0xFF) << 20) | (verb & 0xFFFFF);

    /* ⚠️ 用 IC（Immediate Command）寄存器发送 verb，绕过 CORB DMA！
       QEMU 3.1 的 CORB DMA 读（ldl_le_pci_dma）读不到 guest CPU 写入的低内存
       （数据确实在物理内存里，但 DMA 读返回 0）。IC 走纯 MMIO 寄存器，
       无 DMA、无缓存一致性问题，QEMU 同步处理。 */

    /* 1. 清除上一个响应的 VALID 位（acknowledge），确保轮询能识别新响应 */
    hda_writew(HDA_ICS, HDA_ICS_VALID);

    /* 2. 写入命令到 ICW */
    hda_writel(HDA_ICW, cmd);

    /* 3. 置 BUSY 触发命令发送 */
    hda_writew(HDA_ICS, HDA_ICS_BUSY);

    /* 4. 轮询等待 VALID（响应就绪） */
    int timeout = HDA_IC_TIMEOUT;
    while (!(hda_readw(HDA_ICS) & HDA_ICS_VALID)) {
        if (--timeout == 0) {
            printk("HDA: IC timeout! cad=%d nid=0x%02x verb=0x%05x ICS=0x%04x\r\n",
                   cad, nid, verb, hda_readw(HDA_ICS));
            return ~0ULL;
        }
    }

    /* 5. 读取响应 */
    uint32_t data = hda_readl(HDA_IRR);
    hda_log("HDA: verb resp data=0x%08x cad=%u\r\n", data, cad);
    return (uint64_t)data;
}

int hda_init(void)
{
    uint32_t baddr;
    uint32_t vendor = HDA_VENDOR_ID;
    uint32_t device;

    // 通过PCI发现HDA设备
    device = HDA_DEVICE_ID_ICH6;
    baddr = pci_get_bar_addr(vendor, device);
    if (baddr == (uint32_t)-1) {
        device = HDA_DEVICE_ID_ICH9;
        baddr = pci_get_bar_addr(vendor, device);
    }

    if (baddr == (uint32_t)-1) {
        printk("HDA: No supported HDA device found!\r\n");
        return -1;
    }

    printk("HDA: Found device! BAR0 = 0x%08x\r\n", baddr);

    /* === 排查 #1：PCI Bus Master Enable ===
       QEMU 的 pci_dma_read() 内部检查 PCI Command Register bit 2 (Bus Master)。
       如果未设置，所有 DMA 静默返回 0。SeaBIOS 不会为我们设置。 */
    {
        uint16_t cmd_before = pci_config_read16(vendor, device, 0x04);
        printk("HDA: PCI CMD before = 0x%04x (BME=%d MSE=%d IO=%d)\r\n",
               cmd_before,
               (cmd_before >> 2) & 1,  /* Bus Master Enable */
               (cmd_before >> 1) & 1,  /* Memory Space Enable */
               cmd_before & 1);         /* I/O Space Enable */
        pci_enable_bus_master(vendor, device);
        uint16_t cmd_after = pci_config_read16(vendor, device, 0x04);
        printk("HDA: PCI CMD after  = 0x%04x (BME=%d MSE=%d IO=%d)\r\n",
               cmd_after,
               (cmd_after >> 2) & 1,
               (cmd_after >> 1) & 1,
               cmd_after & 1);
    }

    uint32_t bsize = pci_get_bar_size(vendor, device);
    g_hda.irq = pci_get_intr_line(vendor, device);

    // 初始化 HDA 控制器
    g_hda.mmio_phys = baddr;
    g_hda.mmio_base = page_alloc(PAGE_ROUNDUP(bsize)/PAGE_SIZE, VM_PROT_RW, 0);
    page_map(g_hda.mmio_base, baddr, PAGE_ROUNDUP(bsize)/PAGE_SIZE, L2E_V | L2E_W);

    /* 打印控制器能力 */
    uint16_t gcap = hda_readw(HDA_GCAP);
    uint8_t  vmaj = hda_readb(HDA_VMAJ);
    uint8_t  vmin = hda_readb(HDA_VMIN);
    printk("HDA: GCAP=0x%04x v%d.%d (OSS=%u ISS=%u BSS=%u)\r\n",
           gcap, vmaj, vmin,
           gcap & 0xF, (gcap >> 4) & 0xF, (gcap >> 8) & 0xF);

    /* === 标准初始化：CRST=0 -> CRST=1（IC 寄存器复位后即可用，
       无需 CORB/RIRB DMA 缓冲——IC 走纯 MMIO，无 DMA 路径） === */

    /* 1. 复位控制器 (CRST=0) */
    hda_writel(HDA_GCTL, hda_readl(HDA_GCTL) & ~HDA_GCTL_CRST);
    while (hda_readl(HDA_GCTL) & HDA_GCTL_CRST)
        ;

    /* 2. 退出复位，同时使能 Unsolicited 响应 */
    hda_writel(HDA_GCTL, hda_readl(HDA_GCTL) | HDA_GCTL_CRST | HDA_GCTL_UNSOL);
    while (!(hda_readl(HDA_GCTL) & HDA_GCTL_CRST))
        ;
    printk("HDA: GCTL=0x%08x (CRST=%d)\r\n",
           hda_readl(HDA_GCTL), hda_readl(HDA_GCTL) & HDA_GCTL_CRST);

    /* 3. 使能全局中断（某些 QEMU 版本需要 GIE=1 才能启动流 DMA） */
    hda_writel(HDA_INTCTL, 0x80000000);   /* GIE=1 (bit 31) */
    printk("HDA: INTCTL set to 0x%08x\r\n", hda_readl(HDA_INTCTL));

    /* === Codec 探测：QEMU 3.1 hda-duplex 的 AUDIO_WIDGET_CAP 全部返回 0，
       无法通过 verb 发现 widget。直接硬编码已知拓扑: DAC=0x02 Pin=0x07 === */
    uint16_t statests = hda_readw(HDA_STATESTS);
    printk("HDA: STATESTS = 0x%04x\r\n", statests);
    for (int cad = 0; cad < 15; cad++) {
        if (!(statests & (1 << cad)))
            continue;

        printk("HDA: Codec %d present.\r\n", cad);
        g_hda.codec_cad = cad;
        hda_writew(HDA_STATESTS, statests);

        /* hda-duplex 硬编码 widget 拓扑 */
        g_hda.output_dac = 0x02;
        g_hda.output_pin = 0x07;
        printk("HDA: Using hardcoded hda-duplex topology: DAC=0x02 Pin=0x07\r\n");

        /* 配置音频路径 */
        printk("HDA: Configuring path: DAC=0x%02x Pin=0x%02x\r\n",
               g_hda.output_dac, g_hda.output_pin);
        hda_send_verb(cad, g_hda.output_pin,  HDA_VERB_SET_PIN_WIDGET_CTRL | 0x40);
        hda_send_verb(cad, g_hda.output_dac,  HDA_VERB_SET_AMP_GAIN_MUTE | HDA_AMP_UNMUTE_0DB);
        hda_send_verb(cad, g_hda.output_pin,  HDA_VERB_SET_AMP_GAIN_MUTE | HDA_AMP_UNMUTE_0DB);
        /* 配置 Mixer (NID 0x05): 选择 DAC 输入，取消静音 */
        hda_send_verb(cad, HDA_MIXER_NID, HDA_VERB_SET_CONNECTION_SELECT | 0);
        hda_send_verb(cad, HDA_MIXER_NID, HDA_VERB_SET_AMP_GAIN_MUTE | HDA_AMP_UNMUTE_0DB);
        /* 关联 DAC 到 Stream（关键！否则 Codec 不知道从哪个流读取 PCM 数据）
           必须使用与 SDCTL 中 stream tag 相同的非零流号 */
        uint32_t conv_verb = HDA_VERB_SET_CONVERTER_STREAM_CHANNEL |
                             (HDA_OUTPUT_STREAM << 4) | 0;
        printk("HDA: SET_CONVERTER_STREAM_CHANNEL verb=0x%05x (stream=%d)\r\n",
               conv_verb, HDA_OUTPUT_STREAM);
        hda_send_verb(cad, g_hda.output_dac, conv_verb);
        /* 回读 DAC 的 stream/channel，验证 SET 是否生效 */
        {
            uint64_t conv = hda_send_verb(cad, g_hda.output_dac, HDA_VERB_GET_CONV);
            printk("HDA: GET_CONV resp=0x%08llx (期望 0x%02x = stream<<4|ch)\r\n",
                   conv, (uint32_t)(HDA_OUTPUT_STREAM << 4));
        }

        break;
    }

    /* 校验：必须找到 DAC 和 Pin Complex 才能播放音频 */
    if (g_hda.output_dac == 0 || g_hda.output_pin == 0) {
        printk("HDA: Failed to find DAC/Pin Complex!\r\n");
        return -1;
    }

    /* 调用链日志：分配失败仅关闭日志，不影响音频功能 */
    hda_trace_init();

    /* 注册 BCIS 中断（IRQ 不可用/已被占用时自动退回轮询等待） */
    hda_cont_register_irq();

    /* 输入侧拓扑探测：失败只告警，绝不影响播放路径。
       错误码语义见 hda.h 的 HDA_CAP_ERR_*（-4 无 ADC / -5 无输入 Pin /
       -6 连接列表未命中 / -7 Pin 控制回读不符 / -8 连接选择回读不符）。 */
    {
        int crc = hda_capture_probe(g_hda.codec_cad);
        if (crc != 0)
            printk("HDA-CAP: capture unavailable rc=%d (playback unaffected)\r\n", crc);
        else
            hda_capture_dump_state();
    }

    return 0;
}

/* 当前音量（0-100）；g_audio_gain_q8 为 Q8 定点增益（0-256，256=1.0）
   供 hda_stream.c 软件增益使用（QEMU hda-duplex 不将 codec amp 应用到音频输出，
   需在驱动侧缩放 PCM；codec verb 仍发送，供真实硬件用） */
static uint32_t g_audio_volume = 100;
uint32_t g_audio_gain_q8 = 256;

int sys_audio_set_volume(uint32_t volume)
{
    if (volume > 100) volume = 100;
    g_audio_volume = volume;
    g_audio_gain_q8 = (uint32_t)volume * 256 / 100;

    /* volume 100 → gain 0 (0dB); volume 1 → gain 127 (max 衰减) */
    uint32_t gain = (volume == 0) ? 0 : (100 - volume) * 127 / 100;
    uint32_t payload = (volume == 0) ? HDA_AMP_MUTE : HDA_AMP_GAIN(gain);

    /* 与 init 相同的三个 widget：DAC + Pin + Mixer */
    hda_send_verb(g_hda.codec_cad, g_hda.output_dac, HDA_VERB_SET_AMP_GAIN_MUTE | payload);
    hda_send_verb(g_hda.codec_cad, g_hda.output_pin, HDA_VERB_SET_AMP_GAIN_MUTE | payload);
    hda_send_verb(g_hda.codec_cad, HDA_MIXER_NID,    HDA_VERB_SET_AMP_GAIN_MUTE | payload);

    printk("HDA: volume=%u gain=%u\r\n", volume, gain);
    return 0;
}
