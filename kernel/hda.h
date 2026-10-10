#ifndef _HDA_H
#define _HDA_H

#include <inttypes.h>

/* ========== PCI IDs ========== */
#define HDA_VENDOR_ID       0x8086
#define HDA_DEVICE_ID_ICH6  0x2668
#define HDA_DEVICE_ID_ICH9  0x293E

/* ========== 全局寄存器偏移 ========== */
#define HDA_GCAP            0x00    /* Global Capabilities */
#define HDA_VMIN            0x02    /* Minor Version */
#define HDA_VMAJ            0x03    /* Major Version */
#define HDA_OUTPAY          0x04    /* Output Payload Capability */
#define HDA_INPAY           0x06    /* Input Payload Capability */
#define HDA_GCTL            0x08    /* Global Control */
#define HDA_WAKEEN          0x0C    /* Wake Enable */
#define HDA_STATESTS        0x0E    /* State Change Status */
#define HDA_GSTS            0x10    /* Global Status */
#define HDA_INTCTL          0x20    /* Interrupt Control */
#define HDA_INTSTS          0x24    /* Interrupt Status */

/* CORB (Command Output Ring Buffer) */
#define HDA_CORBLBASE       0x40    /* CORB Lower Base Address */
#define HDA_CORBUBASE       0x44    /* CORB Upper Base Address */
#define HDA_CORBWP          0x48    /* CORB Write Pointer */
#define HDA_CORBRP          0x4A    /* CORB Read Pointer */
#define HDA_CORBCTL         0x4C    /* CORB Control */
#define HDA_CORBSTS         0x4D    /* CORB Status */
#define HDA_CORBSIZE        0x4E    /* CORB Size */

/* RIRB (Response Input Ring Buffer) */
#define HDA_RIRBLBASE       0x50    /* RIRB Lower Base Address */
#define HDA_RIRBUBASE       0x54    /* RIRB Upper Base Address */
#define HDA_RIRBWP          0x58    /* RIRB Write Pointer */
#define HDA_RIRBRP          0x5A    /* RIRB Read Pointer */
#define HDA_RIRBCTL         0x5C    /* RIRB Control */
#define HDA_RIRBSTS         0x5D    /* RIRB Status */
#define HDA_RIRBSIZE        0x5E    /* RIRB Size */
#define HDA_RINTCNT         0x5A    /* RIRB Interrupt Count (alias of RIRBRP high byte) */

/* DMA Position Buffer */
#define HDA_DPLBASE         0x70    /* DMA Position Lower Base */
#define HDA_DPUBASE         0x74    /* DMA Position Upper Base */

/* Immediate Command (IC) —— 用寄存器发送 verb，绕过 CORB DMA！
   QEMU 3.1 的 CORB DMA 读（ldl_le_pci_dma）读不到 guest 写入的低内存，
   而 IC 走纯 MMIO 寄存器，无 DMA、无一致性问题 */
#define HDA_ICW             0x60    /* Immediate Command Write */
#define HDA_IRR             0x64    /* Immediate Response Read */
#define HDA_ICS             0x68    /* Immediate Command Status */
#define HDA_ICS_BUSY        0x01    /* bit0: 置1 触发命令发送 */
#define HDA_ICS_VALID       0x02    /* bit1: 响应有效（QEMU 置位，guest 写1清除） */

/* ========== 流描述符寄存器 (Stream Descriptor) ========== */
/* 每个流占用 0x20 字节, 输入流和输出流使用相同偏移 */
#define HDA_SD_BASE(n)      (0x80 + (n) * 0x20)

/* QEMU intel-hda 模型中播放流必须用 SD4（关键！）：
   - QEMU 源码 hw/audio/intel-hda.c 的 intel_hda_set_st_ctl() 中
     output = (stream >= 4)，即 SD0~SD3 是"输入流"(ADC)、SD4~SD7 才是
     "输出流"(DAC)。用 SD0~SD3 播放会被当作录音方向，
     trace 显示 "hda_audio_running st adc"，听不到声音
   - stream tag 必须非零（Intel HDA 规范要求 1-15，tag 0 无效） */
#define HDA_OUTPUT_STREAM   4

/* ========== Codec 拓扑（QEMU hda-duplex 硬编码，见 hda_init） ========== */
#define HDA_MIXER_NID       0x05    /* Mixer widget NID（DAC/Pin 存于 g_hda） */

#define HDA_SD_CTL(n)       (HDA_SD_BASE(n) + 0x00)  /* 3 bytes: Control */
#define HDA_SD_STS(n)       (HDA_SD_BASE(n) + 0x03)  /* 1 byte:  Status */
#define HDA_SD_LPIB(n)      (HDA_SD_BASE(n) + 0x04)  /* Link Position in Buffer */
#define HDA_SD_CBL(n)       (HDA_SD_BASE(n) + 0x08)  /* Cyclic Buffer Length */
#define HDA_SD_LVI(n)       (HDA_SD_BASE(n) + 0x0C)  /* 2 bytes: Last Valid Index */
#define HDA_SD_FIFOW(n)     (HDA_SD_BASE(n) + 0x0E)  /* 2 bytes: FIFO Watermark (reserved) */
#define HDA_SD_FIFOS(n)     (HDA_SD_BASE(n) + 0x10)  /* 2 bytes: FIFO Size */
#define HDA_SD_FMT(n)       (HDA_SD_BASE(n) + 0x12)  /* 2 bytes: Format */
#define HDA_SD_BDPL(n)      (HDA_SD_BASE(n) + 0x18)  /* BDL Pointer Lower */
#define HDA_SD_BDPU(n)      (HDA_SD_BASE(n) + 0x1C)  /* BDL Pointer Upper */

/* ========== SDnCTL 位定义 ========== */
#define HDA_SDCTL_SRST      (1 << 0)   /* Stream Reset */
#define HDA_SDCTL_RUN       (1 << 1)   /* Stream Run */
#define HDA_SDCTL_IOCE      (1 << 2)   /* Interrupt on Completion Enable */
#define HDA_SDCTL_FEIE      (1 << 3)   /* FIFO Error Interrupt Enable */
#define HDA_SDCTL_DEIE      (1 << 4)   /* Descriptor Error Interrupt Enable */

/* ========== SDnSTS 位定义 ========== */
#define HDA_SDSTS_BCIS      (1 << 2)   /* Buffer Completion Interrupt Status */
#define HDA_SDSTS_FIFOE     (1 << 3)   /* FIFO Error */
#define HDA_SDSTS_DESE      (1 << 4)   /* Descriptor Error */

/* ========== GCTL 位定义 ========== */
#define HDA_GCTL_CRST       (1 << 0)   /* Controller Reset */
#define HDA_GCTL_FCNTRL     (1 << 1)   /* Flush Control */
#define HDA_GCTL_UNSOL      (1 << 8)   /* Accept Unsolicited Responses */

/* ========== CORBCTL 位定义 ========== */
#define HDA_CORBCTL_RUN     (1 << 1)   /* CORB DMA Run */
#define HDA_CORBCTL_CMEIE   (1 << 0)   /* CORB Memory Error Intr Enable */

/* ========== RIRBCTL 位定义 ========== */
#define HDA_RIRBCTL_RINTL   (1 << 0)   /* Response Interrupt */
#define HDA_RIRBCTL_RUN     (1 << 1)   /* RIRB DMA Run */
#define HDA_RIRBCTL_ROI     (1 << 2)   /* Response Overrun Interrupt */

/* ========== CORB/RIRB 大小 ========== */
#define HDA_CORB_SIZE       256
#define HDA_RIRB_SIZE       256

/* ========== BDL (Buffer Descriptor List) ========== */
#define HDA_BDL_ENTRIES     16
#define HDA_FIFO_SIZE       0x00FF  /* 最大 FIFO 大小 */

struct hda_bdl_entry {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t length;       /* 每段最大 0x10000 字节 */
    uint32_t flags;        /* bit 0 = IOC (中断) */
} __attribute__((packed));

/* ========== PCM 格式位定义 (SDnFMT) ========== */
#define HDA_FMT_BITS_8      0x0000  /* 8-bit */
#define HDA_FMT_BITS_16     0x0010  /* 16-bit */
#define HDA_FMT_BITS_20     0x0020  /* 20-bit */
#define HDA_FMT_BITS_24     0x0030  /* 24-bit */
#define HDA_FMT_BITS_32     0x0040  /* 32-bit */

#define HDA_FMT_CHAN_MONO   0x0000  /* 1 channel */
#define HDA_FMT_CHAN_STEREO 0x0001  /* 2 channels */

#define HDA_FMT_BASE_48     0x0000  /* 48 kHz base */
#define HDA_FMT_BASE_44     0x4000  /* 44.1 kHz base */
#define HDA_FMT_MULT_1X     0x0000  /* 1x multiplier */
#define HDA_FMT_DIV_1X      0x0000  /* 1x divisor */

/* ========== 设备结构 ========== */
struct hda_dev {
    uint32_t mmio_base;     /* MMIO 寄存器虚拟地址 */
    uint32_t mmio_phys;     /* MMIO 物理地址 (BAR0) */
    uint32_t irq;           /* 中断号 */

    /* 输出流 */
    struct hda_bdl_entry *bdl;  /* BDL 虚拟地址 */
    uint8_t  *sample_buf;       /* PCM 数据 DMA 缓冲区 */
    uint32_t  buf_size;         /* 缓冲区大小 */

    /* Codec 信息 */
    uint8_t  codec_cad;     /* Codec 地址 (0-14) */
    uint32_t output_dac;    /* DAC NID */
    uint32_t output_pin;    /* Pin Complex NID */
};

/* ========== 函数声明 ========== */
int  hda_init(void);
int  sys_audio_play_file_ms(const char *filename, uint32_t max_ms);
int  sys_audio_play_silence_ms(uint32_t ms);
int  sys_audio_play_voices_ms(const char **files, uint32_t n, uint32_t ms);
int  sys_audio_stop(void);
int  sys_audio_set_volume(uint32_t volume);
uint32_t sys_audio_get_errors(void);
uint32_t sys_audio_get_position_ms(void);

/* ========== HDA Verb 定义 ========== */
#define HDA_VERB_GET_PARAMETER      0xF0000
#define HDA_VERB_GET_CONV           0x0F0600  /* 读回转换器 stream/channel */
#define HDA_VERB_SET_CONVERTER_FMT  0x20000
#define HDA_VERB_SET_CONVERTER_STREAM_CHANNEL 0x70600
#define HDA_VERB_SET_CONNECTION_SELECT 0x70100
#define HDA_VERB_SET_AMP_GAIN_MUTE  0x30000
#define HDA_AMP_UNMUTE_0DB          0xB000  /* 输出增益 0dB 且取消静音（verb | 0xB000） */
#define HDA_AMP_MUTE                0xB080  /* mute bit 置 1（verb | 0xB080） */
#define HDA_AMP_GAIN(g)             (0xB000 | ((g) & 0x7F))  /* unmute + gain step 0-127 */
#define HDA_VERB_SET_PIN_WIDGET_CTRL 0x70700

/* GET_PARAMETER 子参数 */
#define HDA_PARAM_NODE_COUNT         0x04
#define HDA_PARAM_FUNC_GRP_TYPE      0x05
#define HDA_PARAM_AUDIO_WIDGET_CAP   0x09
#define HDA_PARAM_CONNLIST_LEN       0x0E

/* Widget 类型 (AUDIO_WIDGET_CAP 的高 4 位) */
#define WIDGET_TYPE_AUDIO_OUTPUT  0x0  /* DAC */
#define WIDGET_TYPE_AUDIO_MIXER   0x2
#define WIDGET_TYPE_PIN_COMPLEX   0x4

#endif /* _HDA_H */