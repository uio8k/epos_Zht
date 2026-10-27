/* 音频客观指标（用户态纯函数，无 IO、无内核依赖）。
 *
 * 算法口径与 tools/audio_metrics.py 的 fft_analysis 完全一致：
 *   去直流 → Hann 窗 DFT 找峰 → 正弦最小二乘拟合细化 f0
 *   → 时域顺序剥离 2..10 次谐波 → 残差即噪声 → THD / THD+N / SNR
 * 不用「FFT 取峰 + 邻带求和」：Hann 窗积分旁瓣泄漏约 -31.5dB，
 * 会给纯数字正弦测出约 1.8% 的假 THD+N。
 *
 * 与 Python 的差异只有一处：本模块不做 active_span 裁剪，
 * 调用方必须传入已经裁好的有效段（测试向量本来就是满幅段）。
 */
#ifndef METRICS_H
#define METRICS_H

#define MET_MAX_HARMONIC 10

typedef struct {
    double f0_hz;
    double rms;
    double peak;
    double dc_offset;
    double thd_pct;
    double thd_n_pct;
    double snr_db;
    int    peak_bin;
} met_result;

enum {
    MET_SILENT = 0,   /* 低于 1 LSB，判为静音，不得判 PASS */
    MET_PASS   = 1,
    MET_FAIL   = 2
};

/* 主入口：s 为 int16 样本，n 为样本数，rate 为采样率。 */
void met_analyze(const short *s, int n, double rate, met_result *out);

/* 只在 [kmin, kmax] 内找 Hann 窗 DFT 幅度峰，返回 bin 号；用于 B4。 */
int met_peak_bin(const short *s, int n, double rate, int kmin, int kmax);

/* 判定行语义（B5 的纯函数部分）：静音优先于 PASS。 */
int met_verdict(double rms, double thd_n_pct, double snr_db);

#endif /* METRICS_H */
