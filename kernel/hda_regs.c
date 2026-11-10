/* GCAP 解码与流描述符索引（P07-a，tech-plan-hda.md §4.3/§4.5）。
 *
 * 旧代码 hda.c 直接 gcap&0xF / >>4 / >>8，三个字段整体错位 8 位
 * （QEMU 实测 GCAP=0x4401 被打成 OSS=1 ISS=0 BSS=4）。本文件只做
 * 纯函数解码与索引换算，不碰 MMIO。
 */
#include <stdint.h>

#include "hda_regs.h"

struct hda_gcap hda_gcap_decode(uint16_t raw)
{
    struct hda_gcap c;
    c.oss   = (raw >> 12) & 0xF;
    c.iss   = (raw >> 8) & 0xF;
    c.bss   = (raw >> 3) & 0x1F;
    c.ok64  = (uint8_t)(raw & 1);
    return c;
}

/* 输入流第 k 条：绝对索引就是 k（输入在前）。c 仅为签名对称保留。 */
uint8_t hda_sd_index_in(const struct hda_gcap *c, uint8_t k)
{
    (void)c;
    return (uint8_t)(k & 0xFF);
}

/* 输出流第 k 条：跳过全部输入流。 */
uint8_t hda_sd_index_out(const struct hda_gcap *c, uint8_t k)
{
    return (uint8_t)((c->iss + k) & 0xFF);
}
