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

/* 环形缓冲大小按用途分配（hda_cont_start 参数）：
   - 流式播放用 4MB（= 256 条 BDL × 16KB，正好用满 LVI 8 位）：
     本机捆绑的 QEMU 3.1.50（定制版）中 SDnLPIB 的推进速率比实际音频
     消费快约 5%（实测：LPIB 每 262KB 块的节流间隔 2.53s，而内容 2.97s；
     20 秒分段相关性在 100s 后开始劣化）。驱动按 LPIB 节流时该差值随时间
     累积（约 4.8KB/s），累积量超过"环缓冲 - 单块 - 在途目标"后，写入就
     会覆盖 DMA 尚未取走的数据（实测伪影内容恰为一整圈之后的数据，
     corr=1.000，长度随时间线性增长）。4MB + 1MB 在途目标 ⟹ 需累积
     >2.8MB（约 580 秒）才触发，整曲（220s）有余量（实测 0 伪影）。
   - 音符/休止符用 512KB（> 最长音符 1600ms + 哨兵）：4MB 常驻会挤压
     钢琴采样的 WAV 缓存（内核堆 12MB，样本约 8.5MB）。 */
#define HDA_CONT_BUF_SIZE_STREAM 0x400000
#define HDA_CONT_BUF_SIZE_NOTE   0x80000

/* 流式播放原语（大文件不整块载入内存，见 sys_audio_play_file_stream_ms）：
   - hda_cont_start：以指定格式启动连续流。已启动时按幂等语义处理：
     参数（rate/ch/bits/buf_size）一致 → 返回 0；不一致 → 记
     STREAM_REJECT trace 并返回 -3，调用方须先 sys_audio_stop 再用新参数启动；
   - hda_cont_append：把一块 PCM 追加到真实数据末尾（内部编号 + trace；
     reserve 负责缓冲空间等待与 underrun 检测；不等待播放）；
   - hda_cont_drain：收尾等待——数据末尾剩余进入 MARGIN 以内后返回。 */
int hda_cont_start(uint32_t rate, uint32_t ch, uint32_t bits, uint32_t buf_size);
int hda_cont_append(const uint8_t *pcm, uint32_t len);
void hda_cont_drain(void);

/* 注册 BCIS 中断（hda_stream.c 定义；由 hda_init 调用）。
   仅当 IRQ 号合法且向量槽空闲时注册；返回非 0 表示退回轮询等待。 */
int hda_cont_register_irq(void);

/* ========== 调用链日志（hda_trace.c 定义） ==========
   内存环形缓冲记录关键事件与全部错误路径，播放期间零串口开销
   （避免 printk 刷屏拖慢播放，见 g_hda_quiet 的教训），
   播放结束后经 hda_trace_dump 批量导出，由 tools/serial_parser.py 落盘。 */
/* 只定义总字节数；条目数由 sizeof(struct hda_trace_rec) 推导
   （历史缺陷：曾按 16B/条假设 4096 条，而实际条目 24B，容量错配）。 */
#define HDA_TRACE_BYTES  (64 * 1024)

enum hda_trace_ev {
    HTEV_STREAM_START = 1,
    HTEV_STREAM_STOP,
    HTEV_NOTE_SUBMIT,
    HTEV_NOTE_WRITTEN,
    HTEV_NOTE_DONE,
    HTEV_UNDERRUN,
    HTEV_REALIGN,
    HTEV_BCIS_IRQ,
    HTEV_IRQ_FALLBACK,
    HTEV_WAIT_TIMEOUT,
    HTEV_STREAM_REJECT,   /* 流已 RUN 时以不同格式/缓冲尺寸重复 start，被拒绝 */
    HTEV_STOP_STALLED,    /* sys_audio_stop 等待停流时 DMA 零进展，强制停流 */
};

struct hda_trace_rec {
    uint32_t tick;      /* PIT 时基（kernel.h 的 g_timer_ticks，HZ=100） */
    uint8_t  ev;        /* enum hda_trace_ev */
    uint8_t  note_id;   /* 音符序号：调用链区间嵌套的关联键 */
    uint16_t flags;     /* 保留 */
    uint32_t start;     /* 绝对字节区间 [start, end) */
    uint32_t end;
    uint32_t rdpos;     /* 事件时刻指针快照 */
    uint32_t wrpos;
};

int  hda_trace_init(void);
void hda_trace_log(uint8_t ev, uint8_t note_id, uint32_t start, uint32_t end);
void hda_trace_dump(void);

/* hda_stream.c 提供的指针快照（trace 条目记录用） */
uint32_t hda_cont_rdpos_snapshot(void);
uint32_t hda_cont_wrpos_snapshot(void);

#endif /* _HDA_INTERNAL_H */
