/* 播放拓扑 profile 表与保底常量。
 *
 * 为什么需要它：QEMU 3.1 hda-duplex 对 AUDIO_WIDGET_CAP 一律返回 0，
 * 靠 verb 无论如何都发现不了 widget（见 hda.c 旧注释与 docs/decisions-adc.md）。
 * 真机 codec 的拓扑又各不相同且型号未知，因此把「按 PCM 控制器 PCI ID
 * 查一条已知可用的路径」做成数据表，代码不认具体 codec。
 *
 * 数据来源约定（docs/decisions-audio-test.md）：真机 dump 文本为唯一事实源，
 * 后续由 tools/codec_profile.py 生成同形状的 C 表；本轮只有 QEMU 这两条是
 * 实测过的，真机条目等 dump 到位后补，不改任何调用点。
 */
#include <string.h>

#include "hda_internal.h"

struct hda_profile_entry {
    uint16_t vendor;
    uint16_t device;
    uint8_t  dac;
    uint8_t  pin;
    uint8_t  mixer;
    uint8_t  mixer_idx;
    uint8_t  eapd;       /* P07-b：输出 pin EAPD 请求（0=不发；QEMU 条目为 0） */
    uint8_t  vref;       /* P07-c：输入 pin VREFEN 档（0=Hi-Z；QEMU 条目为 0） */
    const char *note;
};

static const struct hda_profile_entry g_profiles[] = {
    /* 2026-10 之前在 hda.c 里硬编码、并在 QEMU hda-duplex 上出声的同一形状：
       DAC 0x02 -> Mixer 0x05（选连接 0）-> Pin 0x07。
       eapd/vref 置 0 保持现状：QEMU hda-codec 无 0x70C/0xF0C case（2026-11-10
       摘录 4），门控不放行即不发，现有链路零扰动。 */
    { 0x8086, 0x2668, 0x02, 0x07, HDA_MIXER_NID, 0, 0, 0, "qemu hda-duplex (ICH6)" },
    { 0x8086, 0x293e, 0x02, 0x07, HDA_MIXER_NID, 0, 0, 0, "qemu hda-duplex (ICH9)" },
};

#define HDA_PROFILE_COUNT (sizeof(g_profiles) / sizeof(g_profiles[0]))

static void profile_fill(struct hda_topo_path *out, uint8_t dac, uint8_t pin,
                         uint8_t mixer, uint8_t mixer_idx,
                         uint8_t eapd, uint8_t vref)
{
    memset(out, 0, sizeof(*out));
    out->dac = dac;
    out->pin = pin;
    out->mixer = mixer;
    out->mixer_idx = mixer_idx;
    out->eapd = eapd;
    out->vref = vref;
    out->chain[0] = pin;
    if (mixer) {
        out->chain[1] = mixer;
        out->chain[2] = dac;
        out->hops = 3;
    } else {
        out->chain[1] = dac;
        out->hops = 2;
    }
}

int hda_profile_lookup(uint16_t vendor, uint16_t device, struct hda_topo_path *out)
{
    if (!out)
        return 0;

    for (unsigned i = 0; i < HDA_PROFILE_COUNT; i++) {
        if (g_profiles[i].vendor == vendor && g_profiles[i].device == device) {
            profile_fill(out, g_profiles[i].dac, g_profiles[i].pin,
                         g_profiles[i].mixer, g_profiles[i].mixer_idx,
                         g_profiles[i].eapd, g_profiles[i].vref);
            printk("HDA: profile hit %04x:%04x -> %s\r\n",
                   vendor, device, g_profiles[i].note);
            return 1;
        }
    }
    return 0;
}

void hda_profile_fallback(struct hda_topo_path *out)
{
    if (!out)
        return;
    profile_fill(out, 0x02, 0x07, HDA_MIXER_NID, 0, 0, 0);
}
