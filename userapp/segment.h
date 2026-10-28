/* 有效段裁剪（纯函数，无 IO、无图形依赖）。
 *
 * 用途：采集到的 PCM 首尾通常带静音（起录延迟、播完后等待），直接算 THD/SNR
 * 会把静音算成噪声、把 RMS 拉低。这里按幅度门限从两端收窄，只把有声段交给
 * userapp/metrics.c —— metrics 本身不做裁剪（见 metrics.h 顶部注释）。
 */
#ifndef SEGMENT_H
#define SEGMENT_H

/* 有效段最短长度（样本）。短于这个数做不了 10 次谐波剥离：
   1kHz 至少要覆盖几个周期，2048 @44.1kHz ≈ 46ms ≈ 46 个周期。 */
#define SEG_MIN_SAMPLES 2048

/* 幅度门限下限，与 main.c 的 CAP_ACTIVE_PEAK 对齐（约 -44dBFS）。 */
#define SEG_FLOOR 20

/* 返回：0 成功；-1 样本为空（n<=0 或 s==NULL）；-2 全静音；
   -3 有效段短于 SEG_MIN_SAMPLES。 */
int seg_find_span(const short *s, int n, int thresh, int *out_start, int *out_len);

/* 按峰值给建议门限：max(SEG_FLOOR, peak/8)。
   取 peak/8（约 -18dB）而不是 peak/2，是为了保留起振与衰减沿，
   否则有效段两端会被削掉一小截，THD 计算反而更抖。 */
int seg_thresh_for_peak(int peak);

/* 分析窗长度：8192 样本 @44.1kHz ≈ 186ms ≈ 186 个 1kHz 周期。
   取窗口而不是整段，是因为采集里除了参考音还有底噪与其它声源，
   整段算 THD+N 会把非参考音成分全算成「失真」。 */
#define SEG_ANALYZE_SAMPLES 8192

/* 在 s[0..n) 内找能量最高的 win 样本窗口，步进 win/2，结果写 *out_start。
   返回 0 成功；-1 参数非法或 n < win。纯整数实现，能量用 64 位累加。 */
int seg_find_loudest(const short *s, int n, int win, int *out_start);

#endif /* SEGMENT_H */
