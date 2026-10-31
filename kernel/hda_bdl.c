/* hda_bdl.c —— BDL 合规断言（tech-plan-hda.md §5）。

   背景：BDL 的五条硬要求（1KB 对齐、至少 2 条、每条 16B、写后 wbinvd、
   CBL = 条目长度和，[wiki: Create Buffer Descriptor List]）此前只是
   「事实合规」：对齐靠页分配器隐式保证，条目数/段长/CBL 连检查都没有，
   16 字节布局只写在注释里。本文件把可静态判定的四条变成启动前的显式
   断言：任一负码都放弃启动流，不降级、不「尽量跑」——错的对齐或错的
   CBL 在 QEMU 上表现为爆音，真机上是 DMA 越界。

   只读遍历 O(n)，n ≤ 256，µs 级；只在 start 前调一次，不进热路径。
   五条里的 wbinvd 不归本函数管：两条 start 路径在置 RUN 前各自调
   hda_flush_cache()，职责不挪。 */

#include "hda_internal.h"

int hda_bdl_validate(const struct hda_bdl_entry *bdl, uint16_t entries,
                     uint32_t cbl, uint32_t bdl_pa)
{
    int rc = HDA_BDL_OK;

    if (bdl_pa & 0x3FFu) {
        rc = HDA_BDL_ERR_ALIGN;      /* 低 10 位必须为 0（1KB 对齐） */
    } else if (entries < 2) {
        rc = HDA_BDL_ERR_TOO_FEW;    /* 规范：至少 2 条 */
    } else if (entries > HDA_BDL_ENTRIES) {
        rc = HDA_BDL_ERR_TOO_MANY;   /* 规范条目数上限 256 */
    } else {
        uint32_t sum = 0;
        for (uint16_t i = 0; i < entries; i++) {
            if (bdl[i].length == 0 || bdl[i].length > HDA_BDL_SEG_MAX) {
                rc = HDA_BDL_ERR_SEG;
                break;
            }
            if (bdl[i].addr_lo == 0) {   /* addr_hi 恒 0：32 位地址空间 */
                rc = HDA_BDL_ERR_ADDR;
                break;
            }
            sum += bdl[i].length;        /* ≤256×64KB=16MB，uint32 不溢出 */
        }
        if (rc == HDA_BDL_OK && sum != cbl)
            rc = HDA_BDL_ERR_CBL;
    }

    if (rc != HDA_BDL_OK)
        printk("HDA: BDL reject rc=%d entries=%u cbl=%u pa=0x%x\r\n",
               rc, entries, cbl, bdl_pa);
    else
        printk("HDA: BDL ok entries=%u cbl=%u\r\n", entries, cbl);
    return rc;
}
