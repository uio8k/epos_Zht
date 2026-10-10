#ifndef _HDA_INTERNAL_H
#define _HDA_INTERNAL_H

#include "hda.h"
#include "kernel.h"

/* ========== 共享全局状态 ========== */
extern struct hda_dev g_hda;
extern int g_hda_quiet;
extern uint32_t g_audio_gain_q8;   /* 软件音量增益 Q8（0-256，256=1.0），hda.c 定义 */

/* 音符逐音播放时置 1：抑制逐音播放的进度日志，
   否则几百个音符会向串口刷出成千上万行 printk，拖慢播放甚至导致丢音 */
#define hda_log(fmt, ...) \
    do { if (!g_hda_quiet) printk(fmt, ##__VA_ARGS__); } while (0)

/* ========== MMIO 辅助（static inline，供 hda.c / hda_stream.c 共享） ========== */
static inline uint32_t hda_readl(uint32_t reg)
{
    return *(volatile uint32_t *)(g_hda.mmio_base + reg);
}

static inline uint16_t hda_readw(uint32_t reg)
{
    return *(volatile uint16_t *)(g_hda.mmio_base + reg);
}

static inline uint8_t hda_readb(uint32_t reg)
{
    return *(volatile uint8_t *)(g_hda.mmio_base + reg);
}

static inline void hda_writel(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(g_hda.mmio_base + reg) = val;
}

static inline void hda_writew(uint32_t reg, uint16_t val)
{
    *(volatile uint16_t *)(g_hda.mmio_base + reg) = val;
}

static inline void hda_writeb(uint32_t reg, uint8_t val)
{
    *(volatile uint8_t *)(g_hda.mmio_base + reg) = val;
}

/* ========== 硬件层函数（hda.c 定义） ========== */
uint64_t hda_send_verb(uint8_t cad, uint8_t nid, uint32_t verb);
uint8_t *hda_alloc_dma(uint32_t bytes, uint32_t *pa_out);
uint8_t *hda_ensure_dma_buf(uint32_t bytes);
void hda_flush_cache(void);

/* ========== 流管理层：连续流（hda_stream.c 定义） ==========
   连续流（音符）模式哨兵：流 RUN 永不停止，tail 无需吸收停流截断。
   连续流只需保留少量 DMA/LPIB 读取余量，因此哨兵很短，
   使音符之间几乎无缝衔接。 */
#define HDA_CONT_SILENCE_MS      0
#define HDA_CONT_SILENCE_TAIL_MS 0

int hda_cont_play_note(uint32_t rate, uint32_t ch, uint32_t bits,
                       const uint8_t *pcm, uint32_t note_bytes,
                       uint32_t lead_ms, uint32_t tail_ms);

#endif /* _HDA_INTERNAL_H */
