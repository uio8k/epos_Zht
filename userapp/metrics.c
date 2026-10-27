/* 音频客观指标实现。算法口径见 metrics.h 顶部说明。 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "metrics.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int next_pow2(int n)
{
    int p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

/* 与 np.hanning(n) 相同：0.5 - 0.5cos(2πi/(n-1)) */
static double hann_at(int i, int n)
{
    if (n <= 1)
        return 1.0;
    return 0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)(n - 1));
}

int met_peak_bin(const short *s, int n, double rate, int kmin, int kmax)
{
    int nfft, best;
    double best_p = -1.0, mean = 0.0, *x;

    (void)rate;
    if (!s || n <= 0)
        return kmin;

    nfft = next_pow2(n);
    if (kmax > nfft / 2)
        kmax = nfft / 2;
    if (kmin < 1)
        kmin = 1;
    best = kmin;

    x = (double *)malloc(sizeof(double) * (size_t)n);
    if (!x)
        return kmin;

    for (int i = 0; i < n; i++)
        mean += (double)s[i];
    mean /= (double)n;
    for (int i = 0; i < n; i++)
        x[i] = ((double)s[i] - mean) * hann_at(i, n);

    for (int k = kmin; k <= kmax; k++) {
        double re = 0.0, im = 0.0, w = 2.0 * M_PI * (double)k / (double)nfft;
        for (int i = 0; i < n; i++) {
            re += x[i] * cos(w * (double)i);
            im -= x[i] * sin(w * (double)i);
        }
        double p = re * re + im * im;
        if (p > best_p) {
            best_p = p;
            best = k;
        }
    }
    free(x);
    return best;
}

/* 拟合 x ≈ a·sin(ωt) + b·cos(ωt)，返回基波功率；strip 时把拟合值从 x 里减掉。
 *
 * 用旋转递推代替逐点 sin/cos：1 秒 44100 点的细化扫描要跑 ~150 次拟合，
 * 逐点三角函数约 2700 万次，会吃掉 2 秒预算；递推每点只要 4 次乘加。
 * 每 1024 点做一次归一化，抑制递推的幅度漂移。 */
static double fit_strip(double *x, int n, double rate, double f, int strip)
{
    double w = 2.0 * M_PI * f / rate;
    double cw = cos(w), sw = sin(w);
    double cs = 1.0, sn = 0.0;          /* cos(ω·0), sin(ω·0) */
    double A = 0.0, B = 0.0, C = 0.0, D = 0.0, E = 0.0;
    double det, a, b, power;

    for (int i = 0; i < n; i++) {
        double nc, ns;
        A += sn * sn;
        B += sn * cs;
        C += cs * cs;
        D += sn * x[i];
        E += cs * x[i];
        nc = cs * cw - sn * sw;
        ns = sn * cw + cs * sw;
        cs = nc;
        sn = ns;
        if ((i & 1023) == 1023) {
            double m = 1.0 / sqrt(cs * cs + sn * sn);
            cs *= m;
            sn *= m;
        }
    }
    det = A * C - B * B;
    if (fabs(det) < 1e-12)
        return 0.0;

    a = (C * D - B * E) / det;
    b = (A * E - B * D) / det;
    power = a * a * A + 2.0 * a * b * B + b * b * C;

    if (strip) {
        cs = 1.0;
        sn = 0.0;
        for (int i = 0; i < n; i++) {
            double nc, ns;
            x[i] -= a * sn + b * cs;
            nc = cs * cw - sn * sw;
            ns = sn * cw + cs * sw;
            cs = nc;
            sn = ns;
            if ((i & 1023) == 1023) {
                double m = 1.0 / sqrt(cs * cs + sn * sn);
                cs *= m;
                sn *= m;
            }
        }
    }
    return power;
}

static double sumsq(const double *x, int n)
{
    double s = 0.0;
    for (int i = 0; i < n; i++)
        s += x[i] * x[i];
    return s;
}

void met_analyze(const short *s, int n, double rate, met_result *out)
{
    double *x, *resid, mean = 0.0, ss, f0, fund_power, harm_power = 0.0, noise_power;
    double peak = 0.0;
    int nfft;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!s || n <= 0)
        return;

    x = (double *)malloc(sizeof(double) * (size_t)n);
    resid = (double *)malloc(sizeof(double) * (size_t)n);
    if (!x || !resid) {
        free(x);
        free(resid);
        return;
    }

    for (int i = 0; i < n; i++) {
        mean += (double)s[i];
        double av = fabs((double)s[i]);
        if (av > peak)
            peak = av;
    }
    mean /= (double)n;
    for (int i = 0; i < n; i++)
        x[i] = (double)s[i] - mean;

    out->dc_offset = mean;
    out->peak = peak;
    out->rms = sqrt(sumsq(x, n) / (double)n);

    /* 频谱窗固定取前 1024 点（B4 的 43.07Hz/bin），与 gen_vectors.py 同定义 */
    nfft = next_pow2(n < 1024 ? n : 1024);
    out->peak_bin = met_peak_bin(s, nfft, rate, 5, nfft / 2);
    f0 = (double)out->peak_bin * rate / (double)nfft;

    /* 频率细化：±30Hz 起步，每轮 21 点扫描后收缩 10 倍（同 _refine_f0） */
    {
        double lo = f0 - 30.0, hi = f0 + 30.0;
        ss = sumsq(x, n);
        for (int it = 0; it < 7; it++) {
            double best = f0, best_r = 0.0;
            int first = 1;
            for (int j = 0; j < 21; j++) {
                double f = lo + (hi - lo) * (double)j / 20.0;
                double sse = ss - fit_strip(x, n, rate, f, 0);
                if (first || sse < best_r) {
                    best_r = sse;
                    best = f;
                    first = 0;
                }
            }
            f0 = best;
            {
                double step = (hi - lo) / 20.0;
                lo = f0 - step;
                hi = f0 + step;
            }
        }
    }
    out->f0_hz = f0;

    memcpy(resid, x, sizeof(double) * (size_t)n);
    fund_power = fit_strip(resid, n, rate, f0, 1);

    for (int k = 2; k <= MET_MAX_HARMONIC; k++) {
        double fk = (double)k * f0;
        double pk;
        if (fk >= rate / 2.0)
            break;
        pk = fit_strip(resid, n, rate, fk, 1);
        if (pk > 0.0)
            harm_power += pk;
    }

    /* 残差直流不计入失真：直流偏置单独报告 */
    noise_power = sumsq(resid, n);
    {
        double rm = 0.0;
        for (int i = 0; i < n; i++)
            rm += resid[i];
        rm /= (double)n;
        for (int i = 0; i < n; i++)
            resid[i] -= rm;
        noise_power = sumsq(resid, n);
    }

    if (fund_power > 0.0) {
        out->thd_pct = sqrt(harm_power / fund_power) * 100.0;
        out->thd_n_pct = sqrt((harm_power + noise_power) / fund_power) * 100.0;
    }
    out->snr_db = (noise_power > 0.0) ? 10.0 * log10(fund_power / noise_power) : 120.0;

    free(x);
    free(resid);
}

int met_verdict(double rms, double thd_n_pct, double snr_db)
{
    if (rms < 1.0)
        return MET_SILENT;
    if (thd_n_pct < 1.0 && snr_db > 60.0)
        return MET_PASS;
    return MET_FAIL;
}
