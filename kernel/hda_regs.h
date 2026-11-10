#ifndef _HDA_REGS_H
#define _HDA_REGS_H

#include <inttypes.h>

/* GCAP 解码与流描述符索引（P07-a，tech-plan-hda.md §4）。
 *
 * 纯函数、不碰 MMIO，宿主 L1 可直接把 hda_regs.c 编进测试
 * （沿用 test_topo.c 把源文件复制进 tests/build/ 的做法）。
 *
 * 位段依据 [wiki: 0x00 Global Capabilities, oldid=30627] 与 QEMU 头文件
 * 双重一致：OSS=bits15:12、ISS=bits11:8、BSS=bits7:3、64OK=bit0。
 */
struct hda_gcap {
    uint8_t oss;    /* 输出流条数 */
    uint8_t iss;    /* 输入流条数 */
    uint8_t bss;    /* 双向流条数 */
    uint8_t ok64;   /* 64 位寻址支持 */
};

struct hda_gcap hda_gcap_decode(uint16_t raw);

/* 流描述符绝对索引：输入在前、输出居中、双向最后
 * （[wiki: Registers of Stream Descriptors]，SD 基址公式 0x80 + 0x20*n
 *  对绝对索引通用，见 hda.h 的 HDA_SD_BASE）。 */
uint8_t hda_sd_index_in (const struct hda_gcap *c, uint8_t k);
uint8_t hda_sd_index_out(const struct hda_gcap *c, uint8_t k);

#endif /* _HDA_REGS_H */
