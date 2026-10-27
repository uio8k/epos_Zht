/* 采集 probe 的宿主侧单测（L1 层，<2s）。
 *
 * 做法：把 kernel/hda_capture.c 整份编进本 TU（构建时复制到 tests/build/），
 * 于是它的 static 函数（cap_conn_index / cap_verify / cap_verb）都能直接调用，
 * 而唯一的硬件出口 hda_send_verb 由下面的「假 codec」实现。
 * 驱动源码一行不改，测的是真实代码路径。
 *
 * 被测行为（只有外部可观测的）：
 *   - probe 是否找出正确的 ADC / Mic Pin
 *   - 是否把正确的 verb 发给 codec（连接选择索引、pin 控制、amp 载荷方向位）
 *   - 回读不符时是否返回对应错误码
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"

#include "build/hda_capture_under_test.c"   /* 构建脚本复制到 build/ 的驱动本体 */

/* ===================== 假 codec ===================== */

#define FC_NID_MAX 0x21
#define FC_TYPE_NONE 0xFF

static struct {
    uint8_t  type[FC_NID_MAX];        /* widget type，FC_TYPE_NONE = 该 NID 不存在 */
    uint8_t  pincap_input[FC_NID_MAX];
    uint8_t  cfg_default[FC_NID_MAX];
    uint8_t  conn_len;                /* 连接列表条目数 */
    uint8_t  conn_long;               /* 1 = 长格式（每条 2 个 16bit NID） */
    uint8_t  conn[8];
    uint8_t  pin_ctrl[FC_NID_MAX];
    uint8_t  conn_sel[FC_NID_MAX];
    uint32_t amp_set[FC_NID_MAX];     /* 最后一次 SET_AMP 的 16bit 载荷 */
    int      ignore_pin_ctl;          /* 模拟「写进去但读不回」 */
    int      ignore_conn_sel;
} fc;

static char g_log[8192];

void printk(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(g_log);

    va_start(ap, fmt);
    vsnprintf(g_log + n, sizeof(g_log) - n, fmt, ap);
    va_end(ap);
}

void panic(const char *msg) { (void)msg; }
void *kmalloc(size_t b) { return malloc(b); }
void  kfree(void *p) { free(p); }

struct hda_dev g_hda;

uint8_t *hda_alloc_dma(uint32_t bytes, uint32_t *pa_out)
{
    uint8_t *p = calloc(1, bytes);
    if (pa_out)
        *pa_out = 0;
    return p;
}

uint8_t *hda_ensure_dma_buf(uint32_t bytes) { return calloc(1, bytes); }
void     hda_flush_cache(void) { }
void     hda_trace_log(uint8_t ev, uint8_t note_id, uint32_t s, uint32_t e)
{
    (void)ev; (void)note_id; (void)s; (void)e;
}
int      hda_trace_init(void) { return 0; }
void     hda_trace_dump(void) { }
uint32_t hda_cont_rdpos_snapshot(void) { return 0; }
uint32_t hda_cont_wrpos_snapshot(void) { return 0; }

/* 唯一的硬件出口：解码驱动实际发出的 20bit verb 字段。
 *
 * 驱动的打包方式是 cmd = (cad<<28)|(nid<<20)|(verb & 0xFFFFF)，即 verb 字段
 * 只有 20 位：低 16 位是 payload，高 4 位（bit19:16）叠着 8~12 位 verb code。
 * 所以不能简单用 (verb>>8)&0xFFF 取 code —— amp 类 verb 带 16 位载荷时，
 * 载荷的 bit15:12 会把 code 污染成 0x370 之类。这里按 verb 字段的高半字节分组，
 * 再用 bit15:8 区分同组内的具体 verb，与真实 codec 的解码口径一致。 */
uint64_t hda_send_verb(uint8_t cad, uint8_t nid, uint32_t verb)
{
    uint32_t v   = verb & 0xFFFFF;
    uint32_t hi  = (v >> 16) & 0xF;   /* verb 字段高半字节 */
    uint32_t mid = (v >> 8) & 0xFF;   /* bit15:8 */
    uint32_t p16 = v & 0xFFFF;
    uint32_t p8  = v & 0xFF;

    (void)cad;

    if (nid >= FC_NID_MAX || fc.type[nid] == FC_TYPE_NONE)
        return ~0ULL;

    switch (hi) {
    case 0xF:   /* GET 组（短格式 verb code 落在 0xFxx） */
        switch (mid) {
        case 0x00:  /* GET_PARAMETER，具体参数在低 8 位 */
            switch (p8) {
            case HDA_PARAM_AUDIO_WIDGET_CAP:
                return (uint64_t)((uint32_t)fc.type[nid] << 20);
            case HDA_PARAM_PIN_CAP:
                return fc.pincap_input[nid] ? (uint64_t)HDA_PINCAP_INPUT : 0ULL;
            case HDA_PARAM_CONFIG_DEFAULT:
                return (uint64_t)((uint32_t)fc.cfg_default[nid] << 20);
            case HDA_PARAM_CONNLIST_LEN:
                return (uint64_t)((uint32_t)fc.conn_len | ((uint32_t)fc.conn_long << 7));
            default:
                return 0ULL;
            }
        case 0x01:  /* GET_CONNECTION_SELECT */
            return (uint64_t)(fc.ignore_conn_sel ? 0u : (uint32_t)fc.conn_sel[nid]);
        case 0x02: { /* GET_CONNECTION_LIST_ENTRY，索引在低 8 位 */
            uint32_t off = p8, w = 0;
            if (fc.conn_long) {
                for (uint32_t k = 0; k < 2; k++)
                    w |= (uint32_t)fc.conn[off + k] << (16 * k);
            } else {
                for (uint32_t k = 0; k < 4; k++)
                    w |= (uint32_t)fc.conn[off + k] << (8 * k);
            }
            return (uint64_t)w;
        }
        case 0x07:  /* GET_PIN_WIDGET_CTRL */
            return (uint64_t)(fc.ignore_pin_ctl ? 0u : (uint32_t)fc.pin_ctrl[nid]);
        default:
            return 0ULL;
        }

    case 0x3:   /* SET_AMP_GAIN_MUTE：载荷 16 位，原样记录供断言 */
        fc.amp_set[nid] = p16;
        return 0ULL;

    case 0x7:   /* SET 组 0x7xx */
        if (mid == 0x01) {          /* SET_CONNECTION_SELECT */
            if (!fc.ignore_conn_sel)
                fc.conn_sel[nid] = (uint8_t)(p16 & 0x7F);
        } else if (mid == 0x07) {   /* SET_PIN_WIDGET_CTRL */
            if (!fc.ignore_pin_ctl)
                fc.pin_ctrl[nid] = (uint8_t)(p16 & 0xFF);
        }
        return 0ULL;

    case 0x0:   /* GET_AMP_GAIN_MUTE（0xB00 | 0x7000 = 0x7B00） */
        if (mid == 0x7B)
            return (uint64_t)(fc.amp_set[nid] & HDA_AMP_MUTE_BOTH);
        return 0ULL;

    default:
        return 0ULL;
    }
}

/* ===================== 两套 codec profile ===================== */

/* QEMU hda-duplex：ADC=0x04，Mic Pin=0x05，短格式连接列表，pin 在索引 0 */
static void load_profile_qemu(void)
{
    memset(&fc, 0, sizeof(fc));
    memset(fc.type, FC_TYPE_NONE, sizeof(fc.type));

    fc.type[0x04] = WIDGET_TYPE_AUDIO_INPUT;
    fc.type[0x05] = WIDGET_TYPE_PIN_COMPLEX;
    fc.pincap_input[0x05] = 1;
    fc.cfg_default[0x05]  = HDA_CFG_DEVICE_MIC_IN;

    fc.conn_long = 0;
    fc.conn_len  = 4;
    fc.conn[0] = 0x05;   /* pin 在索引 0 */
    fc.conn[1] = 0x06;
    fc.conn[2] = 0x07;
    fc.conn[3] = 0x08;
}

/* 长格式连接列表：ADC=0x12，pin=0x19 在索引 1（旧实现取索引 0 会错） */
static void load_profile_longform(void)
{
    memset(&fc, 0, sizeof(fc));
    memset(fc.type, FC_TYPE_NONE, sizeof(fc.type));

    fc.type[0x12] = WIDGET_TYPE_AUDIO_INPUT;
    fc.type[0x18] = WIDGET_TYPE_PIN_COMPLEX;
    fc.type[0x19] = WIDGET_TYPE_PIN_COMPLEX;
    fc.pincap_input[0x19] = 1;
    fc.cfg_default[0x19]  = HDA_CFG_DEVICE_MIC_IN;

    fc.conn_long = 1;
    fc.conn_len  = 2;
    fc.conn[0] = 0x18;
    fc.conn[1] = 0x19;   /* pin 在索引 1 */
}

/* ===================== 用例 ===================== */

void setUp(void) { g_log[0] = '\0'; memset(&g_hda, 0, sizeof(g_hda)); }
void tearDown(void) { }

/* 行为：QEMU 拓扑下 probe 成功，且发出的 verb 指向正确的 widget 与索引 */
void test_probe_qemu_profile(void)
{
    load_profile_qemu();

    TEST_ASSERT_EQUAL_INT(0, hda_capture_probe(0));
    TEST_ASSERT_EQUAL_UINT8(0x04, g_hda.adc_nid);
    TEST_ASSERT_EQUAL_UINT8(0x05, g_hda.input_pin);
    TEST_ASSERT_EQUAL_UINT8(0, fc.conn_sel[0x04]);          /* pin 在索引 0 */
    TEST_ASSERT_EQUAL_UINT8(HDA_PIN_CTRL_IN_EN, fc.pin_ctrl[0x05]);
}

/* 行为：输入 amp 的载荷必须是输入方向（bit15=0、bit14=1、增益 0dB）
   历史缺陷：旧代码 0xB000|(1<<14) = 0xF000，bit15 也被置位，codec 当成输出方向 */
void test_amp_input_direction(void)
{
    load_profile_qemu();

    TEST_ASSERT_EQUAL_INT(0, hda_capture_probe(0));

    TEST_ASSERT_EQUAL_HEX32(HDA_AMP_IN_UNMUTE_0DB, fc.amp_set[0x04]);
    TEST_ASSERT_EQUAL_HEX32(0x7000, fc.amp_set[0x04]);
    TEST_ASSERT_EQUAL_UINT32(0, fc.amp_set[0x04] & HDA_AMP_DIR_OUTPUT);
    TEST_ASSERT_EQUAL_HEX32(0x7000, fc.amp_set[0x05]);
}

/* 行为：长格式连接列表里 pin 在索引 1 时，连接选择必须写 1 */
void test_conn_index_long_form(void)
{
    load_profile_longform();

    TEST_ASSERT_EQUAL_INT(0, hda_capture_probe(0));
    TEST_ASSERT_EQUAL_UINT8(0x12, g_hda.adc_nid);
    TEST_ASSERT_EQUAL_UINT8(0x19, g_hda.input_pin);
    TEST_ASSERT_EQUAL_UINT8(1, fc.conn_sel[0x12]);
}

/* 行为：pin 控制写进去却读不回时，probe 必须报 PINCTL 错误而不是假装成功 */
void test_readback_mismatch_fails(void)
{
    load_profile_qemu();
    fc.ignore_pin_ctl = 1;

    TEST_ASSERT_EQUAL_INT(HDA_CAP_ERR_PINCTL, hda_capture_probe(0));
}

/* 行为：没有 ADC widget 时报 NO_ADC */
void test_no_adc_fails(void)
{
    memset(&fc, 0, sizeof(fc));
    memset(fc.type, FC_TYPE_NONE, sizeof(fc.type));
    fc.type[0x05] = WIDGET_TYPE_PIN_COMPLEX;
    fc.pincap_input[0x05] = 1;

    TEST_ASSERT_EQUAL_INT(HDA_CAP_ERR_NO_ADC, hda_capture_probe(0));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_probe_qemu_profile);
    RUN_TEST(test_amp_input_direction);
    RUN_TEST(test_conn_index_long_form);
    RUN_TEST(test_readback_mismatch_fails);
    RUN_TEST(test_no_adc_fails);
    return UNITY_END();
}
