#ifndef _AUDIO_CAPTURE_H
#define _AUDIO_CAPTURE_H

#include <inttypes.h>

/* 采集统计（内核与用户程序共享布局，见 include/syscall-nr.h 的 199-201）。
   全部为无符号 32 位，避免任何对齐/填充差异。 */
struct audio_capture_stats {
    uint32_t bytes_total;    /* 累计交付字节数 */
    uint32_t samples_total;  /* 累计交付样本数（16bit 计 1 个） */
    uint32_t nonzero;        /* 非零样本数 */
    uint32_t peak;           /* abs 峰值（16bit 样本，0..32768） */
    uint32_t overruns;       /* 用户读太慢导致的覆盖次数 */
    uint32_t lpib;           /* 当前 SDnLPIB 快照 */
    uint32_t wraps;          /* LPIB 绕回圈数 */
    uint32_t reserved;
};

#endif /* _AUDIO_CAPTURE_H */
