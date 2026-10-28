/* 播放侧拓扑发现：沿 codec 连接列表做深度优先搜索，找出 Pin -> ... -> DAC。
 *
 * 算法照 OSDev wiki「Intel High Definition Audio」§8.2：
 *   从接了输出设备的 Pin Complex 出发 → 读它的连接列表 → 逐个下探
 *   → 命中 DAC（widget type 0）即记下路径；某分支走不通就回溯到上一层的
 *   下一项。限制单条路径 10 跳，防止 codec 把遍历带进环（wiki 原文
 *   "a ten link limit would be safe"）。
 *
 * 三级来源（hda_topo_resolve），顺序是「已验证的先行、未知的才探」：
 *   1) profile 表精确命中 (vendor,device) → 直接用表里的已验证路径；
 *   2) 未命中 → 上面的 DFS（真机新鲜 codec 走这里）；
 *   3) DFS 也没结果 → 保底常量（QEMU 时代硬编码形状），并打 SKIP 告警。
 *
 * 为什么 profile 在 DFS 之前：实测 QEMU 3.1.50 的 hda-duplex 并非「caps 全 0」，
 * DFS 能跑通，但它选中 pin=0x03（直连 DAC），与历史验证过的 pin=0x07 +
 * mixer 0x05 不是同一条路。仿真器的 codec 图与真实音频路由未必一致，
 * 拿未验证的通路替换已验证的通路属于回归风险，因此已知机型一律用表。
 *
 * 只读 GET 类 verb，不发任何 SET；失败一律只告警，由调用方决定是否播放。
 */
#include <string.h>

#include "hda_internal.h"

#define TOPO_PARAM_FAILED 0xFFFFFFFFu

/* 读参数：IC 通道偶尔超时，重试一次再判失败（与 hda_capture.c 的 cap_verb 同口径） */
static uint32_t topo_param(uint8_t cad, uint8_t nid, uint32_t param)
{
    for (int i = 0; i < 2; i++) {
        uint64_t r = hda_send_verb(cad, nid, HDA_VERB_GET_PARAMETER | param);
        if (r != ~0ULL)
            return (uint32_t)r;
    }
    return TOPO_PARAM_FAILED;
}

/* 连接列表长度与格式；返回 0 表示该 widget 没有连接列表或读取失败 */
static int topo_conn_info(uint8_t cad, uint8_t nid, uint32_t *count, uint32_t *longform)
{
    uint32_t v = topo_param(cad, nid, HDA_PARAM_CONNLIST_LEN);

    if (v == TOPO_PARAM_FAILED)
        return 0;
    *count = v & 0x7F;
    *longform = (v >> 7) & 1;
    return *count != 0;
}

/* 取连接列表第 idx 项。一条响应含 4 个 8bit NID（短格式）或 2 个 16bit（长格式），
   所以先按 step 对齐到下发的起始索引，再从响应里取出槽位。 */
static int topo_conn_at(uint8_t cad, uint8_t nid, uint32_t idx, uint32_t longform,
                        uint8_t *out)
{
    uint32_t step = longform ? 2 : 4;
    uint32_t base = idx - (idx % step);
    uint32_t slot = idx % step;
    uint64_t r = hda_send_verb(cad, nid, HDA_VERB_GET_CONNECTION_LIST_ENTRY | base);

    if (r == ~0ULL)
        return 0;
    if (longform)
        *out = (uint8_t)(((uint32_t)r >> (16 * slot)) & 0xFFFF);
    else
        *out = (uint8_t)(((uint32_t)r >> (8 * slot)) & 0xFF);
    return *out != 0;
}

static int topo_is_mixer(uint32_t type)
{
    return type == WIDGET_TYPE_AUDIO_MIXER || type == WIDGET_TYPE_AUDIO_SELECTOR;
}

/* 从 nid 往下找 DAC。depth 是当前已走过的 widget 数（起点为 0）。
   from_mixer/from_idx 记录「上一层是 mixer、这一支是它的第几项」，
   命中 DAC 时写进路径，供 SET_CONNECTION_SELECT 使用。 */
static int topo_walk(uint8_t cad, uint8_t nid, uint8_t from_mixer, uint8_t from_idx,
                     int depth, uint64_t visited, struct hda_topo_path *p)
{
    uint32_t cap, type, count, longform;

    if (depth >= HDA_TOPO_MAX_PATH)
        return 0;

    cap = topo_param(cad, nid, HDA_PARAM_AUDIO_WIDGET_CAP);
    if (cap == TOPO_PARAM_FAILED)
        return 0;

    type = (cap >> 20) & 0xF;
    p->chain[depth] = nid;

    if (type == WIDGET_TYPE_AUDIO_OUTPUT) {
        p->dac = nid;
        p->hops = (uint8_t)(depth + 1);
        if (from_mixer) {
            p->mixer = from_mixer;
            p->mixer_idx = from_idx;
        }
        return 1;
    }

    if (!topo_conn_info(cad, nid, &count, &longform))
        return 0;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t child;

        if (!topo_conn_at(cad, nid, i, longform, &child))
            continue;
        if (child >= HDA_TOPO_NID_MAX || (visited & (1ULL << child)))
            continue;

        if (topo_walk(cad, child, topo_is_mixer(type) ? nid : 0, (uint8_t)i,
                      depth + 1, visited | (1ULL << child), p))
            return 1;
    }
    return 0;
}

/* caps 是否可用：QEMU hda-duplex 对存在的 NID 也返回 0，此时 DFS 无从起步。
   判据是「扫完一轮至少有一个 NID 报出非 0 的类型」。 */
static int topo_caps_usable(uint8_t cad)
{
    for (uint32_t nid = 1; nid < HDA_TOPO_NID_MAX; nid++) {
        uint32_t cap = topo_param(cad, (uint8_t)nid, HDA_PARAM_AUDIO_WIDGET_CAP);
        if (cap != TOPO_PARAM_FAILED && ((cap >> 20) & 0xF) != 0)
            return 1;
    }
    return 0;
}

/* 输出 Pin 的优先级：真机优先主板 Line Out，其次 Speaker，再是耳机口；
   Config Default 没写好（设备位为 0）的机器上按「先遇到的先用」处理。 */
static int topo_pin_priority(uint8_t cad, uint8_t nid)
{
    uint32_t cfg = topo_param(cad, nid, HDA_PARAM_CONFIG_DEFAULT);
    uint32_t dev = (cfg == TOPO_PARAM_FAILED) ? 0xFF : ((cfg >> 20) & 0xF);

    switch (dev) {
    case HDA_CFG_DEVICE_SPEAKER:   return 5;
    case HDA_CFG_DEVICE_LINE_OUT:  return 4;
    case HDA_CFG_DEVICE_HP_OUT:    return 3;
    case HDA_CFG_DEVICE_SPDIF_OUT: return 2;
    default:                      return 1;
    }
}

/* 收集可输出的 Pin，按优先级从高到低插排（最多 HDA_TOPO_MAX_PINS 个） */
static int topo_collect_pins(uint8_t cad, uint8_t *pins, int max)
{
    int n = 0;

    for (uint32_t nid = 1; nid < HDA_TOPO_NID_MAX; nid++) {
        uint32_t cap = topo_param(cad, (uint8_t)nid, HDA_PARAM_AUDIO_WIDGET_CAP);
        uint32_t pincap;
        int prio, pos;

        if (cap == TOPO_PARAM_FAILED || ((cap >> 20) & 0xF) != WIDGET_TYPE_PIN_COMPLEX)
            continue;

        pincap = topo_param(cad, (uint8_t)nid, HDA_PARAM_PIN_CAP);
        if (pincap == TOPO_PARAM_FAILED || !(pincap & HDA_PINCAP_OUTPUT))
            continue;

        prio = topo_pin_priority(cad, (uint8_t)nid);
        if (n == max) {
            /* 满了还遇到更高优先级：替换掉最后一个（最差）候选 */
            if (prio <= topo_pin_priority(cad, pins[n - 1]))
                continue;
            n--;
        }
        pos = n;
        while (pos > 0 && topo_pin_priority(cad, pins[pos - 1]) < prio) {
            pins[pos] = pins[pos - 1];
            pos--;
        }
        pins[pos] = (uint8_t)nid;
        n++;
    }
    return n;
}

const char *hda_topo_source_name(int src)
{
    switch (src) {
    case HDA_TOPO_SRC_CAPS_DFS: return "caps-dfs";
    case HDA_TOPO_SRC_PROFILE:  return "profile";
    case HDA_TOPO_SRC_FALLBACK: return "fallback";
    default:                    return "none";
    }
}

int hda_topo_resolve(uint8_t cad, struct hda_topo_path *out)
{
    struct hda_topo_path path;
    uint8_t pins[HDA_TOPO_MAX_PINS];
    int npins;

    if (!out)
        return HDA_TOPO_ERR_NO_RESULT;
    memset(out, 0, sizeof(*out));

    /* 1) 已知机型：用表里验证过的路径，不拿仿真/实探结果去赌 */
    if (hda_profile_lookup(g_hda.pci_vendor, g_hda.pci_device, out))
        return HDA_TOPO_SRC_PROFILE;

    /* 2) 未知机型：实探 */
    if (!topo_caps_usable(cad)) {
        printk("HDA: widget caps unusable on codec %u (all zero), skipping DFS\r\n",
               cad);
    } else {
        npins = topo_collect_pins(cad, pins, HDA_TOPO_MAX_PINS);
        if (npins == 0) {
            printk("HDA: no output-capable pin on codec %u\r\n", cad);
        }
        for (int i = 0; i < npins; i++) {
            memset(&path, 0, sizeof(path));
            path.pin = pins[i];
            if (topo_walk(cad, pins[i], 0, 0, 0, 1ULL << pins[i], &path)) {
                *out = path;
                printk("HDA: DFS found path pin=0x%02x dac=0x%02x mixer=0x%02x "
                       "idx=%u hops=%u\r\n",
                       path.pin, path.dac, path.mixer, path.mixer_idx, path.hops);
                return HDA_TOPO_SRC_CAPS_DFS;
            }
        }
        if (npins > 0)
            printk("HDA: DFS from %d candidate pin(s) found no DAC\r\n", npins);
    }

    printk("HDA: profile SKIP %04x:%04x not in table, using unverified fallback\r\n",
           g_hda.pci_vendor, g_hda.pci_device);
    hda_profile_fallback(out);
    return HDA_TOPO_SRC_FALLBACK;
}
