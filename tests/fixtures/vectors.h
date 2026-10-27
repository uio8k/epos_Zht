/* 黄金向量接口。
 *
 * 期望值全部由 tools/audio_metrics.py（Python 参照实现）算出，
 * 再由 tests/fixtures/gen_vectors.py 写进 vectors.c。C 侧不解析 JSON，
 * 也不许把被测代码的输出抄回来当期望值（见 docs/test-plan-audio.md §7）。
 */
#ifndef VECTORS_H
#define VECTORS_H

typedef struct {
    const char *name;
    const short *samples;
    int     n;
    double  rate;
    double  f0_hz;      /* 参照实现的基频 */
    double  rms;
    double  thd_pct;
    double  thd_n_pct;
    double  snr_db;
    int     peak_bin;   /* Hann 窗 DFT 峰值所在 bin（nfft = next_pow2(n)） */
} vec_case;

extern const vec_case vec_cases[];
extern const int vec_case_count;

/* 按名字取向量；找不到返回 0。 */
const vec_case *vec_find(const char *name);

#endif /* VECTORS_H */
