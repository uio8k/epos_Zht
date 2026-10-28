/* 播放侧拓扑发现的宿主侧单测（L1 层）。
 *
 * 做法与 test_capture_probe.c 一致：把 kernel/hda_topo.c 与 kernel/hda_profile.c
 * 整份编进本 TU（构建脚本复制到 tests/build/），static 函数可直接调用，
 * 唯一的硬件出口 hda_send_verb 由下面的「假 codec」实现。驱动源码一行不改。
 *
 * 被测行为（只有外部可观测的）：
 *   - caps 不可用（QEMU）时是否落到 profile，且给出正确的 DAC/Pin/Mixer
 *   - caps 可用时能否沿连接列表找出 Pin -> Mixer -> DAC 的路径与 mixer 索引
 *   - 未知 codec 是否打 SKIP 并回落保底常量，而不是假装发现成功
 *   - 超过 10 跳的长链是否被截断、成环的图是否会终止（不死循环）
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "build/hda_topo_under_test.c"
#include "build/hda_profile_under_test.c"

/* ===================== 假 codec（每 NID 独立连接列表） ===================== */

#define TC_MAX 0x21
#define TC_NO_NODE 0xFF

struct tc_node {
    uint8_t exists;
    uint8_t type;        /* WIDGET_TYPE_* */
    uint8_t out_cap;     /* PIN_CAP 的 OUTPUT 位 */
    uint8_t cfgdev;      /* Config Default bits 23:20 */
    uint8_t nconn;       /* 连接列表条目数 */
    uint8_t conn[8];
};

static struct tc_node tc[TC_MAX];
static uint8_t tc_conn_sel[TC_MAX];
static int     tc_caps_disabled;   /* 模拟 QEMU：WIDGET_CAP 一律返回 0 */
static char    g_log[8192];

void printk(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(g_log);

    va_start(ap, fmt);
    vsnprintf(g_log + n, sizeof(g_log) - n, fmt, ap);
    va_end(ap);
}

void  panic(const char *msg) { (void)msg; }
void *kmalloc(size_t b) { return malloc(b); }
void  kfree(void *p) { free(p); }

struct hda_dev g_hda;

/* 与 test_capture_probe.c 相同的 20bit verb 解码口径 */
uint64_t hda_send_verb(uint8_t cad, uint8_t nid, uint32_t verb)
{
    uint32_t v   = verb & 0xFFFFF;
    uint32_t hi  = (v >> 16) & 0xF;
    uint32_t mid = (v >> 8) & 0xFF;
    uint32_t p16 = v & 0xFFFF;
    uint32_t p8  = v & 0xFF;

    (void)cad;

    if (nid >= TC_MAX || !tc[nid].exists)
        return ~0ULL;

    switch (hi) {
    case 0xF:
        switch (mid) {
        case 0x00:                      /* GET_PARAMETER */
            switch (p8) {
            case HDA_PARAM_AUDIO_WIDGET_CAP:
                return tc_caps_disabled ? 0ULL : (uint64_t)((uint32_t)tc[nid].type << 20);
            case HDA_PARAM_PIN_CAP:
                return tc[nid].out_cap ? (uint64_t)HDA_PINCAP_OUTPUT : 0ULL;
            case HDA_PARAM_CONFIG_DEFAULT:
                return (uint64_t)((uint32_t)tc[nid].cfgdev << 20);
            case HDA_PARAM_CONNLIST_LEN:
                return (uint64_t)tc[nid].nconn;      /* 短格式 */
            default:
                return 0ULL;
            }
        case 0x01:                      /* GET_CONNECTION_SELECT */
            return (uint64_t)tc_conn_sel[nid];
        case 0x02: {                    /* GET_CONNECTION_LIST_ENTRY */
            uint32_t off = p8, w = 0;
            for (uint32_t k = 0; k < 4; k++)
                if (off + k < tc[nid].nconn)
                    w |= (uint32_t)tc[nid].conn[off + k] << (8 * k);
            return (uint64_t)w;
        }
        default:
            return 0ULL;
        }

    case 0x7:
        if (mid == 0x01)                /* SET_CONNECTION_SELECT */
            tc_conn_sel[nid] = (uint8_t)(p16 & 0x7F);
        return 0ULL;

    default:                            /* SET_AMP / SET_PIN_CTRL 等：拓扑发现不发它们 */
        return 0ULL;
    }
}

/* ===================== 夹具 ===================== */

static void tc_reset(void)
{
    memset(tc, 0, sizeof(tc));
    memset(tc_conn_sel, 0, sizeof(tc_conn_sel));
    memset(g_log, 0, sizeof(g_log));
    memset(&g_hda, 0, sizeof(g_hda));
    tc_caps_disabled = 0;
    g_hda.pci_vendor = 0x10ec;          /* 未知 codec，默认走 fallback */
    g_hda.pci_device = 0x0255;
}

static void tc_add(uint8_t nid, uint8_t type)
{
    tc[nid].exists = 1;
    tc[nid].type = type;
}

static void tc_link(uint8_t nid, const uint8_t *conn, int n)
{
    tc[nid].nconn = (uint8_t)n;
    for (int i = 0; i < n && i < 8; i++)
        tc[nid].conn[i] = conn[i];
}

void setUp(void) { tc_reset(); }
void tearDown(void) {}

/* ===================== U-L1-04：QEMU 走 profile ===================== */

static void test_topo_profile_qemu(void)
{
    struct hda_topo_path p;
    int src;

    tc_caps_disabled = 1;               /* caps 全 0 的老 QEMU 行为 */
    g_hda.pci_vendor = 0x8086;
    g_hda.pci_device = 0x2668;

    src = hda_topo_resolve(0, &p);

    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_TOPO_SRC_PROFILE, src, "qemu must resolve via profile");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x02, p.dac, "dac nid");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x07, p.pin, "pin nid");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x05, p.mixer, "mixer nid");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, p.mixer_idx, "mixer conn index");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, p.hops, "hops");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x07, p.chain[0], "chain starts at pin");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_log, "profile hit"), "log should name the profile hit");
}

/* 已验证的 profile 优先于实探：即便 caps 可用、DFS 能走通，也不许换路。
   这是一条回归门——QEMU 3.1.50 实测 caps 可用，DFS 会选中 pin=0x03。 */
static void test_topo_profile_beats_dfs(void)
{
    struct hda_topo_path p;
    const uint8_t pin_to_mixer[1] = { 0x0C };
    const uint8_t mixer_to_dac[1] = { 0x02 };
    int src;

    g_hda.pci_vendor = 0x8086;
    g_hda.pci_device = 0x2668;

    /* 一条完全可走的 DFS 图 */
    tc_add(0x14, WIDGET_TYPE_PIN_COMPLEX);
    tc[0x14].out_cap = 1;
    tc_link(0x14, pin_to_mixer, 1);
    tc_add(0x0C, WIDGET_TYPE_AUDIO_MIXER);
    tc_link(0x0C, mixer_to_dac, 1);
    tc_add(0x02, WIDGET_TYPE_AUDIO_OUTPUT);

    src = hda_topo_resolve(0, &p);

    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_TOPO_SRC_PROFILE, src, "verified profile must win over DFS");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x07, p.pin, "still the profile pin, not the DFS one");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x05, p.mixer, "still the profile mixer");
}

/* ===================== DFS：Pin -> Mixer -> DAC ===================== */

static void test_topo_dfs_mixer_path(void)
{
    struct hda_topo_path p;
    const uint8_t pin_to_mixer[1] = { 0x0C };
    const uint8_t mixer_to_dac[1] = { 0x02 };
    int src;

    tc_add(0x14, WIDGET_TYPE_PIN_COMPLEX);
    tc[0x14].out_cap = 1;
    tc[0x14].cfgdev = HDA_CFG_DEVICE_LINE_OUT;
    tc_link(0x14, pin_to_mixer, 1);

    tc_add(0x0C, WIDGET_TYPE_AUDIO_MIXER);
    tc_link(0x0C, mixer_to_dac, 1);

    tc_add(0x02, WIDGET_TYPE_AUDIO_OUTPUT);

    src = hda_topo_resolve(0, &p);

    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_TOPO_SRC_CAPS_DFS, src, "must resolve by DFS");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x14, p.pin, "pin");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x02, p.dac, "dac");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x0C, p.mixer, "mixer on the path");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, p.mixer_idx, "mixer index pointing at dac");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, p.hops, "pin->mixer->dac");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x14, p.chain[0], "chain[0] = pin");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x0C, p.chain[1], "chain[1] = mixer");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x02, p.chain[2], "chain[2] = dac");
}

/* mixer 的 DAC 不在索引 0：索引必须来自连接列表，不能写死 0 */
static void test_topo_dfs_mixer_index_not_zero(void)
{
    struct hda_topo_path p;
    const uint8_t pin_to_mixer[1] = { 0x0C };
    const uint8_t mixer_conn[3]   = { 0x05, 0x06, 0x02 };   /* DAC 在第 3 项 */
    int src;

    tc_add(0x14, WIDGET_TYPE_PIN_COMPLEX);
    tc[0x14].out_cap = 1;
    tc[0x14].cfgdev = HDA_CFG_DEVICE_SPEAKER;
    tc_link(0x14, pin_to_mixer, 1);

    tc_add(0x0C, WIDGET_TYPE_AUDIO_SELECTOR);   /* selector 也走连接选择 */
    tc_link(0x0C, mixer_conn, 3);

    tc_add(0x05, WIDGET_TYPE_AUDIO_MIXER);
    tc_add(0x06, WIDGET_TYPE_AUDIO_MIXER);
    tc_add(0x02, WIDGET_TYPE_AUDIO_OUTPUT);

    src = hda_topo_resolve(0, &p);

    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_TOPO_SRC_CAPS_DFS, src, "must resolve by DFS");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x0C, p.mixer, "selector is the mixer step");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, p.mixer_idx, "index of the dac entry, not 0");
}

/* 直连：Pin 直接列到 DAC，路径上没有 mixer */
static void test_topo_dfs_direct_path(void)
{
    struct hda_topo_path p;
    const uint8_t pin_to_dac[1] = { 0x03 };
    int src;

    tc_add(0x10, WIDGET_TYPE_PIN_COMPLEX);
    tc[0x10].out_cap = 1;
    tc_link(0x10, pin_to_dac, 1);
    tc_add(0x03, WIDGET_TYPE_AUDIO_OUTPUT);

    src = hda_topo_resolve(0, &p);

    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_TOPO_SRC_CAPS_DFS, src, "must resolve by DFS");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x10, p.pin, "pin");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x03, p.dac, "dac");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, p.mixer, "no mixer on a direct path");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, p.hops, "pin->dac");
}

/* ===================== U-L1-05：未知 codec 打 SKIP ===================== */

static void test_topo_unknown_codec_skips_profile(void)
{
    struct hda_topo_path p;
    int src;

    /* caps 可用，但没有任何输出 Pin（真机 dump 缺失时的典型形态） */
    tc_add(0x02, WIDGET_TYPE_AUDIO_OUTPUT);

    src = hda_topo_resolve(0, &p);

    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_TOPO_SRC_FALLBACK, src, "unknown codec falls back");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(g_log, "SKIP"), "must log SKIP for a codec without profile");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x02, p.dac, "fallback keeps the verified qemu shape");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0x07, p.pin, "fallback pin");
}

/* ===================== 防御：跳数上限与成环 ===================== */

static void test_topo_depth_limit(void)
{
    struct hda_topo_path p;
    const uint8_t pin_to_first[1] = { 0x08 };
    int src;

    /* 12 级 mixer 串联：0x14 -> 0x08 -> ... -> 0x13 -> 0x02，超过 10 跳上限 */
    tc_add(0x14, WIDGET_TYPE_PIN_COMPLEX);
    tc[0x14].out_cap = 1;
    tc_link(0x14, pin_to_first, 1);

    for (uint8_t nid = 0x08; nid <= 0x13; nid++) {
        uint8_t next = (nid == 0x13) ? 0x02 : (uint8_t)(nid + 1);
        tc_add(nid, WIDGET_TYPE_AUDIO_MIXER);
        tc_link(nid, &next, 1);
    }
    tc_add(0x02, WIDGET_TYPE_AUDIO_OUTPUT);

    src = hda_topo_resolve(0, &p);

    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_TOPO_SRC_FALLBACK, src,
                                  "path longer than 10 hops must be abandoned");
}

static void test_topo_cycle_terminates(void)
{
    struct hda_topo_path p;
    const uint8_t pin_to_a[1] = { 0x0A };
    const uint8_t a_to_b[1]   = { 0x0B };
    const uint8_t b_to_a[1]   = { 0x0A };
    int src;

    /* 0x14 -> 0x0A -> 0x0B -> 0x0A ...：成环且没有 DAC，必须能返回 */
    tc_add(0x14, WIDGET_TYPE_PIN_COMPLEX);
    tc[0x14].out_cap = 1;
    tc_link(0x14, pin_to_a, 1);

    tc_add(0x0A, WIDGET_TYPE_AUDIO_MIXER);
    tc_link(0x0A, a_to_b, 1);

    tc_add(0x0B, WIDGET_TYPE_AUDIO_MIXER);
    tc_link(0x0B, b_to_a, 1);

    src = hda_topo_resolve(0, &p);

    TEST_ASSERT_EQUAL_INT_MESSAGE(HDA_TOPO_SRC_FALLBACK, src, "cycle yields no path, falls back");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_topo_profile_qemu);
    RUN_TEST(test_topo_profile_beats_dfs);
    RUN_TEST(test_topo_dfs_mixer_path);
    RUN_TEST(test_topo_dfs_mixer_index_not_zero);
    RUN_TEST(test_topo_dfs_direct_path);
    RUN_TEST(test_topo_unknown_codec_skips_profile);
    RUN_TEST(test_topo_depth_limit);
    RUN_TEST(test_topo_cycle_terminates);
    return UNITY_END();
}
