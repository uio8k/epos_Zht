/* BDL 合规断言的宿主侧单测（L1 层，P06，tech-plan-hda.md §5.5）。
 *
 * 做法：把 kernel/hda_bdl.c 整份编进本 TU（构建时复制到 tests/build/），
 * 驱动源码一行不改，测的是真实代码路径。hda_bdl.c 唯一的外部依赖是
 * printk，下面的替身把它捕进缓冲，个别用例顺带断言串口文本。
 *
 * 向量口径（本会话已裁定）：tech-plan §5.5 原表的 T1–T4 测的是 P07-a 的
 * GCAP 解码纯函数（尚未实现），随 P07 落地；本文件 10 条全部针对
 * hda_bdl_validate，覆盖 OK + 全部六个负码 + 双侧边界，其中 ERR_ADDR(-5)
 * 是原表漏掉、此处补上的。
 *
 * 断言消息一律 ASCII（Unity 会把非 ASCII 转义成 \xNN）。
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"

#include "build/hda_bdl_under_test.c"   /* 构建脚本复制到 build/ 的驱动本体 */

/* ===================== 替身与夹具 ===================== */

static char g_log[4096];

void printk(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(g_log);

    va_start(ap, fmt);
    vsnprintf(g_log + n, sizeof(g_log) - n, fmt, ap);
    va_end(ap);
}

void panic(const char *msg) { (void)msg; }

/* 向量缓冲：V4 需要 257 条，按最大开 */
#define V_MAX 257
static struct hda_bdl_entry g_bdl[V_MAX];

/* 铺一份「全绿」BDL：n 条 × seg 字节，地址非零且落在 4MB 起 DMA 区，
   pa 取 1KB 对齐的 0x400000（hda_alloc_dma 的实际起点，hda.c:28） */
static void fill_valid(uint32_t n, uint32_t seg)
{
    for (uint32_t i = 0; i < n; i++) {
        g_bdl[i].addr_lo = 0x00400000u + i * seg;
        g_bdl[i].addr_hi = 0;
        g_bdl[i].length  = seg;
        g_bdl[i].flags   = 0;
    }
}

void setUp(void)    { memset(g_log, 0, sizeof(g_log)); }
void tearDown(void) { }

/* ===================== V1..V10 ===================== */

/* V1：正常 128 条、CBL 匹配、地址 1KB 对齐 → 0，串口打 ok 行 */
void test_bdl_ok_128(void)
{
    fill_valid(128, 4096);
    int rc = hda_bdl_validate(g_bdl, 128, 128u * 4096, 0x00400000u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_BDL_OK, rc, "valid 128-entry BDL must pass");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_log, "HDA: BDL ok entries=128"),
                                 "ok line must be printed on pass");
}

/* V2：pa 偏移 0x200（低 10 位非 0）→ -1 ALIGN */
void test_bdl_err_align(void)
{
    fill_valid(128, 4096);
    int rc = hda_bdl_validate(g_bdl, 128, 128u * 4096, 0x00400200u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_BDL_ERR_ALIGN, rc, "pa+0x200 must be ALIGN reject");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_log, "BDL reject rc=-1"),
                                 "reject line with rc=-1 must be printed");
}

/* V3：1 条 → -2 TOO_FEW */
void test_bdl_err_too_few(void)
{
    fill_valid(1, 4096);
    int rc = hda_bdl_validate(g_bdl, 1, 4096, 0x00400000u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_BDL_ERR_TOO_FEW, rc, "1 entry must be TOO_FEW");
}

/* V4：257 条 → -3 TOO_MANY（数组实开 257，但校验须在遍历前拒绝） */
void test_bdl_err_too_many(void)
{
    fill_valid(V_MAX, 4096);
    int rc = hda_bdl_validate(g_bdl, V_MAX, V_MAX * 4096u, 0x00400000u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_BDL_ERR_TOO_MANY, rc, "257 entries must be TOO_MANY");
}

/* V5：某条 length=0 → -4 SEG */
void test_bdl_err_seg_zero(void)
{
    fill_valid(128, 4096);
    g_bdl[3].length = 0;
    int rc = hda_bdl_validate(g_bdl, 128, 128u * 4096, 0x00400000u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_BDL_ERR_SEG, rc, "zero-length entry must be SEG");
}

/* V6：某条 length=0x10001（> 64KB）→ -4 SEG */
void test_bdl_err_seg_over(void)
{
    fill_valid(128, 4096);
    g_bdl[3].length = HDA_BDL_SEG_MAX + 1;
    int rc = hda_bdl_validate(g_bdl, 128, 128u * 4096, 0x00400000u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_BDL_ERR_SEG, rc, "64KB+1 entry must be SEG");
}

/* V7：某条 addr_lo=0（未初始化）→ -5 ADDR（原表漏掉、此处补上的覆盖） */
void test_bdl_err_addr_zero(void)
{
    fill_valid(128, 4096);
    g_bdl[3].addr_lo = 0;
    int rc = hda_bdl_validate(g_bdl, 128, 128u * 4096, 0x00400000u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_BDL_ERR_ADDR, rc, "zero addr entry must be ADDR");
}

/* V8：长度和比 CBL 少 4 字节 → -6 CBL */
void test_bdl_err_cbl_short(void)
{
    fill_valid(128, 4096);
    int rc = hda_bdl_validate(g_bdl, 128, 128u * 4096 - 4, 0x00400000u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_BDL_ERR_CBL, rc, "sum<CBL must be CBL reject");
}

/* V9：长度和比 CBL 多 4 字节 → -6 CBL */
void test_bdl_err_cbl_long(void)
{
    fill_valid(128, 4096);
    int rc = hda_bdl_validate(g_bdl, 128, 128u * 4096 + 4, 0x00400000u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_BDL_ERR_CBL, rc, "sum>CBL must be CBL reject");
}

/* V10：边界全过：恰好 256 条、每段恰好 64KB、CBL=和（16MB）→ 0 */
void test_bdl_boundary_max(void)
{
    fill_valid(HDA_BDL_ENTRIES, HDA_BDL_SEG_MAX);
    int rc = hda_bdl_validate(g_bdl, HDA_BDL_ENTRIES,
                              HDA_BDL_ENTRIES * (uint32_t)HDA_BDL_SEG_MAX,
                              0x00400000u);
    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_BDL_OK, rc,
                                  "256 x 64KB at upper bounds must pass");
}

/* ===================== 入口 ===================== */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bdl_ok_128);
    RUN_TEST(test_bdl_err_align);
    RUN_TEST(test_bdl_err_too_few);
    RUN_TEST(test_bdl_err_too_many);
    RUN_TEST(test_bdl_err_seg_zero);
    RUN_TEST(test_bdl_err_seg_over);
    RUN_TEST(test_bdl_err_addr_zero);
    RUN_TEST(test_bdl_err_cbl_short);
    RUN_TEST(test_bdl_err_cbl_long);
    RUN_TEST(test_bdl_boundary_max);
    return UNITY_END();
}
