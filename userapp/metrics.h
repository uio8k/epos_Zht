/* 音频行为指标（用户态纯函数，无 IO、无内核依赖）。
 *
 * 算法口径与 tools/audio_metrics.py 的 fft_analysis 一致（行为指标部分）：
 *   去直流 → Hann 窗 DFT 找峰 → 正弦最小二乘拟合细化 f0
 * 输出 f0/rms/peak/dc_offset/peak_bin 五项行为量。
 *
 * 2026-11-10 P05b（docs/decisions-qemu-only.md D-Q5）：SNR/THD 真值指标整体
 * 删除——真值档位原靠真机 3.5mm 回环定档，真机路线废弃后无定档通道，模拟器
 * 值非真值（[wiki: Testing] "might not work on the real thing"）；谐波剥离与
 * THD/SNR 计算随之移除，真值分析只保留在宿主工具 tools/audio_metrics.py
 * （D11 唯一指标源）作趋势参考。
 *
 * 与 Python 的差异只有一处：本模块不做 active_span 裁剪，
 * 调用方必须传入已经裁好的有效段（测试向量本来就是满幅段）。
 */
#ifndef METRICS_H
#define METRICS_H

typedef struct {
    double f0_hz;
    double rms;
    double peak;
    double dc_offset;
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

/* 判定行语义（B5 的纯函数部分）：静音优先于 PASS。
 * P05b 后判据=通路活性：非静音即 PASS（真值门线已随真机路线废弃）。 */
int met_verdict(double rms);

#endif /* METRICS_H */
