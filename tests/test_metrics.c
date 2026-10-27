/* L1 指标用例（U-L1-06 … U-L1-10）。
 *
 * 期望值全部来自 tests/fixtures/vectors.c（由 gen_vectors.py 用
 * tools/audio_metrics.py 生成）。禁止把 userapp/metrics.c 的输出抄回来当期望。
 *
 * 断言消息一律用 ASCII：Unity 会把非 ASCII 字节转义成 \xE8... ，日志就没法读了。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "metrics.h"
#include "vectors.h"

#define TOL_F0_REL   0.0002   /* +-0.02% */
#define TOL_RMS_DB   0.1
#define TOL_THD_DB   0.3
#define TOL_SNR_DB   0.3
#define TOL_BIN      1

static double db20(double ratio)
{
    return 20.0 * log10(ratio);
}

static const vec_case *need(const char *name)
{
    const vec_case *c = vec_find(name);
    TEST_ASSERT_NOT_NULL_MESSAGE(c, name);
    return c;
}

void setUp(void) {}
void tearDown(void) {}

/* U-L1-06: f0/RMS accuracy, and silence must never read as PASS */
static void test_metrics_sine_f0_rms(void)
{
    const vec_case *c = need("sine_1k_0dbfs");
    met_result r;

    met_analyze(c->samples, c->n, c->rate, &r);

    TEST_ASSERT_TRUE_MESSAGE(fabs(r.f0_hz - c->f0_hz) / c->f0_hz <= TOL_F0_REL,
                             "f0 beyond +-0.02%");
    TEST_ASSERT_TRUE_MESSAGE(fabs(db20(r.rms / c->rms)) <= TOL_RMS_DB,
                             "RMS beyond +-0.1dB");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MET_PASS, met_verdict(r.rms, r.thd_n_pct, r.snr_db),
                                  "pure tone should be PASS");

    {
        const vec_case *z = need("silence");
        met_result rz;
        met_analyze(z->samples, z->n, z->rate, &rz);
        TEST_ASSERT_TRUE_MESSAGE(rz.rms < 1.0, "silence RMS should be 0");
        TEST_ASSERT_EQUAL_INT_MESSAGE(MET_SILENT,
                                      met_verdict(rz.rms, rz.thd_n_pct, rz.snr_db),
                                      "silence must not be PASS");
    }
}

/* U-L1-07: injected h2/h3 -> THD within +-0.3dB of reference */
static void test_thdn_3rd_harmonic(void)
{
    const vec_case *c = need("sine_1k_plus_h2_h3");
    met_result r;

    met_analyze(c->samples, c->n, c->rate, &r);

    TEST_ASSERT_TRUE_MESSAGE(r.thd_pct > 0.0, "THD should be > 0");
    TEST_ASSERT_TRUE_MESSAGE(fabs(db20(r.thd_pct / c->thd_pct)) <= TOL_THD_DB,
                             "THD beyond +-0.3dB");
}

/* U-L1-08: DC offset case matches reference and is clearly worse than the pure tone */
static void test_thdn_dc_offset(void)
{
    const vec_case *c = need("dc_offset_100");
    const vec_case *s = need("sine_1k_0dbfs");
    met_result r, rs;

    met_analyze(c->samples, c->n, c->rate, &r);
    met_analyze(s->samples, s->n, s->rate, &rs);

    TEST_ASSERT_TRUE_MESSAGE(fabs(db20(r.thd_n_pct / c->thd_n_pct)) <= TOL_THD_DB,
                             "DC-case THD+N beyond +-0.3dB");
    TEST_ASSERT_TRUE_MESSAGE(r.thd_n_pct > 1.5 * rs.thd_n_pct,
                             "DC offset should worsen THD+N (DC sensitivity)");
}

/* U-L1-09: white-noise floor SNR within +-0.3dB */
static void test_snr_noise_floor(void)
{
    const vec_case *c = need("noise_white_seed1234");
    met_result r;

    met_analyze(c->samples, c->n, c->rate, &r);

    TEST_ASSERT_TRUE_MESSAGE(fabs(r.snr_db - c->snr_db) <= TOL_SNR_DB,
                             "SNR beyond +-0.3dB");
}

/* U-L1-10: 1 kHz peak lands in bin 23 +-1 (43.07 Hz/bin, 1024-point window) */
static void test_bin_index_1k(void)
{
    const vec_case *c = need("sine_1k_0dbfs");
    met_result r;
    int bin;

    met_analyze(c->samples, c->n, c->rate, &r);
    bin = met_peak_bin(c->samples, 1024, c->rate, 5, 512);

    TEST_ASSERT_EQUAL_INT_MESSAGE(c->peak_bin, r.peak_bin, "peak bin differs from reference");
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(TOL_BIN, abs(r.peak_bin - 23), "peak not in bin 23 +-1");
    TEST_ASSERT_EQUAL_INT_MESSAGE(r.peak_bin, bin, "met_peak_bin disagrees with met_analyze");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_metrics_sine_f0_rms);
    RUN_TEST(test_thdn_3rd_harmonic);
    RUN_TEST(test_thdn_dc_offset);
    RUN_TEST(test_snr_noise_floor);
    RUN_TEST(test_bin_index_1k);
    return UNITY_END();
}
