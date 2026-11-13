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
#define LAPIC_EOI           0x0B0
#define LAPIC_SVR           0x0F0
#define LAPIC_ICR_LO        0x300
#define LAPIC_ICR_HI        0x310
/* LAPIC timer 寄存器组（S03，出处 docs/evidence/2026-11-14/
   s03-lapic-timer-research-excerpt.txt §1-§4：QEMU apic-v3.1.0.c 的
   write handler case 表与 periodic 计数式） */
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_TICR          0x380   /* initial count：装填即起表 */
#define LAPIC_TDCR          0x3E0   /* divide configuration */

#define APIC_SV_ENABLE      0x100

/* LVT timer 位段：vector 占 bits7..0、masked bit16、periodic bit17
   [src: apic_internal-v3.1.0.h:53-66] */
#define APIC_LVT_PERIODIC   (1u << 17)
#define APIC_LVT_MASKED     (1u << 16)

/* LAPIC timer 中断向量：PIC 占 0x20-0x2F、syscall 0x82，0x40 无冲突且
   NR_IDT=131 免扩（[实测: kernel/machdep.c init_idt]） */
#define LAPIC_TIMER_VECTOR  0x40

/* 定标（U6）：TDCR=0x3 → ÷16（count_shift=4）[src: apic-v3.1.0.c:848-853]，
   periodic 周期 = initial_count << count_shift ns [src: apic-v3.1.0.c:617-631]
   → 10ms = 10,000,000ns >> 4 = 625000，即 100Hz（实测比对墙钟验证） */
#define LAPIC_TDCR_DIV16    0x3
#define LAPIC_TICR_10MS     625000

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
void isr_lapic_timer(uint32_t irq, struct context *ctx);  /* entry.S hwint_lapic_timer 直调 */

#endif /*_SMP_H*/
