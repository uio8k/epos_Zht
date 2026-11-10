/* GCAP 解码与流描述符索引的宿主侧单测（L1 层，P07-a，tech-plan-hda.md §4.5 向量 T1–T4）。
 *
 * 做法：把 kernel/hda_regs.c 整份编进本 TU（构建时复制到 tests/build/）。
 * hda_regs.c 无任何外部依赖（纯函数、不碰 MMIO），无需替身。
 *
 * 向量口径（tech-plan §5.5 表）：T1/T2 解码、T3/T4 索引换算。
 * 断言消息一律 ASCII（Unity 会把非 ASCII 转义成 \xNN）。
 */
#include <stdio.h>
#include "unity.h"

#include "build/hda_regs_under_test.c"   /* 构建脚本复制到 build/ 的驱动本体 */

void setUp(void)    { }
void tearDown(void) { }

/* ---------------- T1: QEMU 实测值 0x4401 ---------------- */
static void t1_decode_0x4401(void)
{
    struct hda_gcap c = hda_gcap_decode(0x4401);
    TEST_ASSERT_EQUAL_UINT(4,  c.oss);
    TEST_ASSERT_EQUAL_UINT(4,  c.iss);
    TEST_ASSERT_EQUAL_UINT(0,  c.bss);
    TEST_ASSERT_EQUAL_UINT(1,  c.ok64);
}

/* ---------------- T2: 1 出 8 进的非 QEMU 形状 ---------------- */
static void t2_decode_0x1801(void)
{
    struct hda_gcap c = hda_gcap_decode(0x1801);
    TEST_ASSERT_EQUAL_UINT(1,  c.oss);
    TEST_ASSERT_EQUAL_UINT(8,  c.iss);
    TEST_ASSERT_EQUAL_UINT(0,  c.bss);
    TEST_ASSERT_EQUAL_UINT(1,  c.ok64);
}

/* ---------------- T3: 0x4401 的首条输入/输出索引 = 0 / 4 ---------------- */
static void t3_index_0x4401(void)
{
    struct hda_gcap c = hda_gcap_decode(0x4401);
    TEST_ASSERT_EQUAL_UINT(0, hda_sd_index_in(&c, 0));
    TEST_ASSERT_EQUAL_UINT(4, hda_sd_index_out(&c, 0));
}

/* ---------------- T4: 0x1801 的输出流第 0/2 条 = 8 / 10 ---------------- */
static void t4_index_0x1801(void)
{
    struct hda_gcap c = hda_gcap_decode(0x1801);
    TEST_ASSERT_EQUAL_UINT(8,  hda_sd_index_out(&c, 0));
    TEST_ASSERT_EQUAL_UINT(10, hda_sd_index_out(&c, 2));
}

int main(void)
{
    UnityBegin("test_hda_regs.c");
    RUN_TEST(t1_decode_0x4401,  __LINE__);
    RUN_TEST(t2_decode_0x1801,  __LINE__);
    RUN_TEST(t3_index_0x4401,   __LINE__);
    RUN_TEST(t4_index_0x1801,   __LINE__);
    return UnityEnd();
}
