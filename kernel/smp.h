/**
 * vim: filetype=c:fenc=utf-8:ts=4:et:sw=4:sts=4
 *
 * SMP 最简原型（S02 起，docs/tech-plan-smp-prototype.md）。
 * LAPIC 寄存器偏移与 ICR 位段出处：
 * docs/evidence/2026-11-13/qemu-src/apic-v3.1.0.c（write handler case 表
 * 与 820-823 的 apic_deliver 拆位）+ s02-apboot-research-excerpt.txt §3。
 */
#ifndef _SMP_H
#define _SMP_H

#define LAPIC_BASE          0xFEE00000
#define LAPIC_ID            0x020
#define LAPIC_SVR           0x0F0
#define LAPIC_ICR_LO        0x300
#define LAPIC_ICR_HI        0x310

#define APIC_SV_ENABLE      0x100

/* ICR 低半字构造：shorthand=11（all excluding self）+ level=1 + delivery mode */
#define ICR_INIT            0x000C4500      /* DM 101 = INIT */
#define ICR_SIPI(v)         (0x000C0600 | ((v) & 0xff))    /* DM 110 = SIPI */

/* trampoline：物理页 0x8000（SIPI vector 0x08）；RAM: 0x2000-0x9f000 区内，
   init_frame 后立刻 smp_reserve_trampoline 显式预留（U3） */
#define SMP_TRAMPOLINE_PA   0x8000

static __inline void apic_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(LAPIC_BASE + reg) = val;
}

static __inline uint32_t apic_read(uint32_t reg)
{
    return *(volatile uint32_t *)(LAPIC_BASE + reg);
}

void smp_init(void);
void smp_reserve_trampoline(void);

#endif /*_SMP_H*/
