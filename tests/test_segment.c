/* L1 有效段裁剪用例（P05 裁段项）。
 *
 * 被测对象是 userapp/segment.c：纯整数函数，宿主直接编进来跑。
 * 断言消息一律 ASCII（Unity 会把非 ASCII 转义，日志不可读）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "segment.h"

void setUp(void) {}
void tearDown(void) {}

static short *make_buf(int n, short fill)
{
    short *p = (short *)malloc((size_t)n * sizeof(short));

    TEST_ASSERT_NOT_NULL(p);
    for (int i = 0; i < n; i++)
        p[i] = fill;
    return p;
}

/* T1: 满幅段原样返回；顺带核对建议门限 */
static void test_span_full(void)
{
    int n = 4096;
    short *s = make_buf(n, 1000);
    int start = -1, len = -1;

    TEST_ASSERT_EQUAL_INT(0, seg_find_span(s, n, 100, &start, &len));
    TEST_ASSERT_EQUAL_INT(0, start);
    TEST_ASSERT_EQUAL_INT(n, len);
    TEST_ASSERT_EQUAL_INT(125, seg_thresh_for_peak(1000));   /* peak/8 > SEG_FLOOR */
    TEST_ASSERT_EQUAL_INT(SEG_FLOOR, seg_thresh_for_peak(0));
    free(s);
}

/* T2: 前后静音被裁掉，只留有声段 */
static void test_span_trims_silence(void)
{
    int n = 5000;
    short *s = make_buf(n, 0);
    int start = -1, len = -1;

    for (int i = 1000; i < 4000; i++)
        s[i] = 800;

    TEST_ASSERT_EQUAL_INT(0, seg_find_span(s, n, 100, &start, &len));
    TEST_ASSERT_EQUAL_INT(1000, start);
    TEST_ASSERT_EQUAL_INT(3000, len);
    free(s);
}

/* T3: 全静音必须报 -2，不能返回一个空段让上层去算 */
static void test_span_all_silent(void)
{
    int n = 4096;
    short *s = make_buf(n, 0);
    int start = -1, len = -1;

    TEST_ASSERT_EQUAL_INT(-2, seg_find_span(s, n, 100, &start, &len));
    free(s);
}

/* T4: 有声但太短，报 -3 */
static void test_span_too_short(void)
{
    int n = 500;
    short *s = make_buf(n, 800);
    int start = -1, len = -1;

    TEST_ASSERT_EQUAL_INT(-3, seg_find_span(s, n, 100, &start, &len));
    free(s);
}

/* T5: 门限为 0 时每个样本都算有声（全零也能成段） */
static void test_span_thresh_zero(void)
{
    int n = SEG_MIN_SAMPLES;
    short *s = make_buf(n, 0);
    int start = -1, len = -1;

    TEST_ASSERT_EQUAL_INT(0, seg_find_span(s, n, 0, &start, &len));
    TEST_ASSERT_EQUAL_INT(0, start);
    TEST_ASSERT_EQUAL_INT(n, len);
    free(s);
}

/* T6: 只有单个样本过门限时，段长不足，报 -3 而不是当有效信号 */
static void test_span_single_sample(void)
{
    int n = 4096;
    short *s = make_buf(n, 0);
    int start = -1, len = -1;

    s[2000] = 3000;

    TEST_ASSERT_EQUAL_INT(-3, seg_find_span(s, n, 100, &start, &len));
    free(s);
}

/* T7: 分析窗落在能量最高的一段（中间有 6k 样本的有声段） */
static void test_loudest_middle(void)
{
    int n = 20000;
    int win = SEG_ANALYZE_SAMPLES;
    short *s = make_buf(n, 50);
    int start = -1;

    for (int i = 8000; i < 14000; i++)
        s[i] = 2000;

    TEST_ASSERT_EQUAL_INT(0, seg_find_loudest(s, n, win, &start));
    TEST_ASSERT_EQUAL_INT(8192, start);
    free(s);
}

/* T8: 样本数不足一个窗时返回 -1，由调用方退回整段分析 */
static void test_loudest_too_few(void)
{
    int n = 1000;
    short *s = make_buf(n, 100);
    int start = -1;

    TEST_ASSERT_EQUAL_INT(-1, seg_find_loudest(s, n, SEG_ANALYZE_SAMPLES, &start));
    free(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_span_full);
    RUN_TEST(test_span_trims_silence);
    RUN_TEST(test_span_all_silent);
    RUN_TEST(test_span_too_short);
    RUN_TEST(test_span_thresh_zero);
    RUN_TEST(test_span_single_sample);
    RUN_TEST(test_loudest_middle);
    RUN_TEST(test_loudest_too_few);
    return UNITY_END();
}
