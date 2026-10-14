#include <string.h>
#include "kernel.h"
#include "cpu.h"
#include "hda.h"
#include "hda_internal.h"

/* ===================== 音频驱动调用链日志 =====================
   需求：记录音频驱动关键函数调用链路（音符提交/写入区间/播完）与全部
   error 路径（underrun/重对齐/中断降级/超时），播放结束后导出为文件。

   设计（源自方案决策）：
   - 固定大小条目（struct hda_trace_rec）+ 64KB 环形缓冲，条目数按
     sizeof 推导（= 2730 条），追加只做字段赋值，零格式化开销——
     播放热路径不碰串口（g_hda_quiet 的教训：逐音 printk 刷串口曾
     拖慢播放导致丢音）。
   - 任务上下文与 ISR 双写者：追加用 save_flags_cli 关中断保护。
   - 播放结束后由 sys_audio_stop / sys_audio_get_underruns 触发 dump，
     经 COM1 输出；tools/serial_parser.py 识别 [TRACE] 段落落盘
     audio_trace.log 并重建调用链视图。
   - 条目时间戳取 PIT 时基 g_timer_ticks（HZ=100），同时记录 rdpos/wrpos
     绝对字节位置——节拍抖动可同时用 tick 与音频位置两个基准分析。 */

static struct hda_trace_rec *g_trace_buf     = NULL;
static uint32_t g_trace_cap                  = 0;   /* 条目容量 */
static uint32_t g_trace_head                 = 0;   /* 下一写入槽 */
static uint32_t g_trace_total                = 0;   /* 累计写入条数（含被覆盖） */
static uint32_t g_trace_dropped              = 0;   /* 因绕回被覆盖丢失的条数 */
static uint32_t g_trace_on                   = 0;   /* 日志是否可用 */

/* 事件名表（dump 时使用，非热路径） */
static const char *hda_trace_ev_name(uint8_t ev)
{
    switch (ev) {
    case HTEV_STREAM_START: return "STREAM_START";
    case HTEV_STREAM_STOP:  return "STREAM_STOP";
    case HTEV_NOTE_SUBMIT:  return "NOTE_SUBMIT";
    case HTEV_NOTE_WRITTEN: return "NOTE_WRITTEN";
    case HTEV_NOTE_DONE:    return "NOTE_DONE";
    case HTEV_UNDERRUN:     return "UNDERRUN";
    case HTEV_REALIGN:      return "REALIGN";
    case HTEV_BCIS_IRQ:     return "BCIS_IRQ";
    case HTEV_IRQ_FALLBACK: return "IRQ_FALLBACK";
    case HTEV_WAIT_TIMEOUT: return "WAIT_TIMEOUT";
    case HTEV_STREAM_REJECT: return "STREAM_REJECT";
    case HTEV_STOP_STALLED:  return "STOP_STALLED";
    default:                return "UNKNOWN";
    }
}

/* 初始化：分配 64KB 环形缓冲。失败仅关闭日志，不影响音频功能。 */
int hda_trace_init(void)
{
    if (g_trace_on)
        return 0;

    g_trace_buf = (struct hda_trace_rec *)kmalloc(HDA_TRACE_BYTES);
    if (g_trace_buf == NULL) {
        printk("HDA: trace buffer alloc failed (%u bytes), trace disabled\r\n",
               HDA_TRACE_BYTES);
        return -1;
    }

    memset(g_trace_buf, 0, HDA_TRACE_BYTES);
    g_trace_cap     = HDA_TRACE_BYTES / sizeof(struct hda_trace_rec);
    g_trace_head    = 0;
    g_trace_total   = 0;
    g_trace_dropped = 0;
    g_trace_on      = 1;

    printk("HDA: trace log enabled (%u bytes, %u recs)\r\n",
           HDA_TRACE_BYTES, g_trace_cap);
    return 0;
}

/* 追加一条事件。ISR 安全：关中断保护环形缓冲写者。
   缓冲满时绕回覆盖最旧条目并计数丢失（不阻塞、不丢弃当前事件）。 */
void hda_trace_log(uint8_t ev, uint8_t note_id, uint32_t start, uint32_t end)
{
    if (!g_trace_on)
        return;

    uint32_t flags;
    save_flags_cli(flags);

    struct hda_trace_rec *r = &g_trace_buf[g_trace_head];
    r->tick    = g_timer_ticks;
    r->ev      = ev;
    r->note_id = note_id;
    r->flags   = 0;
    r->start   = start;
    r->end     = end;
    r->rdpos   = hda_cont_rdpos_snapshot();
    r->wrpos   = hda_cont_wrpos_snapshot();

    g_trace_head = (g_trace_head + 1) % g_trace_cap;
    if (g_trace_total >= g_trace_cap)
        g_trace_dropped++;
    g_trace_total++;

    restore_flags(flags);
}

/* 批量导出到 COM1（非热路径，只在播放结束后调用）。
   格式固定，供 tools/serial_parser.py 解析：
     [TRACE] begin recs=<有效条数> lost=<丢失条数> first=<缓冲区起始槽>
     [TRACE] t=<PIT tick> ev=<名称> id=<音符号> s=<start> e=<end> rd=<rdpos> wr=<wrpos>
     [TRACE] end
   注意：单行长度控制在 printk 的 128 字节缓冲内。 */
void hda_trace_dump(void)
{
    if (!g_trace_on) {
        printk("[TRACE] begin recs=0 lost=0 first=0\r\n[TRACE] end\r\n");
        return;
    }

    uint32_t flags;
    save_flags_cli(flags);
    uint32_t total   = g_trace_total;
    uint32_t dropped = g_trace_dropped;
    uint32_t head    = g_trace_head;
    uint32_t cap     = g_trace_cap;
    restore_flags(flags);

    uint32_t count = (total < cap) ? total : cap;
    uint32_t first = (total < cap) ? 0 : head;  /* 绕回后最旧条目在 head */

    printk("[TRACE] begin recs=%u lost=%u first=%u\r\n", count, dropped, first);

    for (uint32_t i = 0; i < count; i++) {
        /* 逐条关中断拷贝到栈上再打印：dump 可能在播放中被调用
           （sys_audio_get_underruns(dump=1)），与 ISR 的追加并发；
           直接读共享槽位可能读到被覆盖一半的条目（撕裂）。 */
        struct hda_trace_rec rec;
        save_flags_cli(flags);
        rec = g_trace_buf[(first + i) % cap];
        restore_flags(flags);

        printk("[TRACE] t=%u ev=%s id=%u s=%u e=%u rd=%u wr=%u\r\n",
               rec.tick, hda_trace_ev_name(rec.ev), rec.note_id,
               rec.start, rec.end, rec.rdpos, rec.wrpos);
    }

    printk("[TRACE] end\r\n");
}
