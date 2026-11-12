/**
 * vim: filetype=c:fenc=utf-8:ts=4:et:sw=4:sts=4
 *
 * SMP 最简原型：MPS 表解析 + AP 启动（S02）。
 *
 * 调研与出处：docs/evidence/2026-11-13/s02-apboot-research-excerpt.txt
 * （QEMU apic.c 的 INIT/SIPI 投递、wait_for_sipi 门控、SVR 使能、ICR 位段）
 * 与 s01-lock-research-excerpt.txt（LAPIC ID 直读）。
 *
 * 关键设计事实（S02 实施期发现，[实测: kernel/machdep.c cstart 尾部]）：
 * cstart 主动取消了 [0,1MB) 除 VGA 显存外的全部映射（内核重定位到虚拟
 * 地址后的清理动作），且 do_page_fault 对 vmzone 外的低地址一律拒绝。
 * 因此：
 *   - BIOS/MP 表区（0xE0000-0xFFFFF）在 KERNBASE 别名上临时映射后扫描，
 *     扫完还原；
 *   - trampoline 页 0x8000 在共享页表里单独重映射（AP 开分页后取指必须
 *     有着落），永久保留（一页低内存，代价可忽略）；
 *   - AP 用的页目录物理地址取 PTD[767] & ~0xFFF（内核递归映射，见
 *     kernel.h 的 PTD，久经考验），不另设全局变量。
 *
 * 原型边界（tech-plan C1）：AP 上只跑内核态 idle（ap_main），不调度用户
 * 线程、不取时钟中断（S03 才有 LAPIC timer）；用户任务钉死 CPU0。
 */
#include <stddef.h>
#include <string.h>
#include "kernel.h"
#include "bitmap.h"
#include "smp.h"

/* AP 用的页目录物理地址：PTD[767] 即页目录自映射槽（kernel.h 的 PTD 递归
   视图），&~0xFFF 取其基址。cstart 已取消 [0,1MB) 低地址映射（见下），不
   另设全局变量（首版设过 g_pgdir_pa，实测运行期读到 0，废弃）。 */
#define PD_PA   (PTD[767] & ~0xFFFu)

static unsigned g_cpu_count;
static unsigned g_apic_ids[NR_CPUS];
static volatile int g_ap_online[NR_CPUS];

/* -fleading-underscore：C 侧标识符发射时自动加一个前导下划线。asm 侧
   smptramp.S 的 .globl 写的是 _smp_tramp_*（单下划线），故这里的 C 标识符
   必须不带前导下划线（写 _smp_tramp_start 会发射 __smp_tramp_start 而链接
   失败——首链接实测踩到）。 */
extern uint8_t smp_tramp_start[];
extern uint8_t smp_tramp_pdata[];   /* 页内参数块（3 dwords，见 smptramp.S） */

void ap_main(void);

/**
 * 扫 Intel MP floating pointer（"_MP_"）并解析 processor 条目。
 * [实测: mps-parse-smp2.txt] QEMU 3.1.50 -smp 2 的浮动指针在 0xF58A0、
 * config 表在 0xF58B0；header 的 base_table_length 与实际不符（SeaBIOS
 * 多计尾巴），故条目遍历必须以 entry_count 为边界、按类型定步长
 * （processor=20，其余=8）。
 */
static int mp_parse(void)
{
    uint32_t pa, fp = 0, cfg, count, p;
    uint8_t *base;
    unsigned n = 0;

    /* 临时映射 BIOS 扫描区 [0xE0000,0x100000) 到 KERNBASE 别名 */
    for (pa = 0xE0000; pa < 0x100000; pa += PAGE_SIZE) {
        *vtopte(pa + KERNBASE) = pa | L2E_V | L2E_W;
        invlpg(pa + KERNBASE);
    }

    for (pa = 0xE0000; pa < 0x100000; pa += 4) {
        const char *s = (const char *)(pa + KERNBASE);
        if (s[0] == '_' && s[1] == 'M' && s[2] == 'P' && s[3] == '_') {
            fp = pa;
            break;
        }
    }
    if (fp != 0) {
        cfg = *(const volatile uint32_t *)(fp + KERNBASE + 4);  /* 指向 config 表 */
        count = *(const volatile uint16_t *)(cfg + KERNBASE + 34);
        base = (uint8_t *)(cfg + KERNBASE + 44);

        for (p = 0; p < count && n < NR_CPUS; ) {
            uint8_t t = base[p];
            if (t == 0) {                       /* processor entry */
                if (base[p + 3] & 1) {          /* bit0 = enabled */
                    g_apic_ids[n++] = base[p + 1];
                }
                p += 20;
            } else if (t <= 4) {
                p += 8;                         /* bus/IOAPIC/INT 类条目（实测全 8 字节） */
            } else {
                p += 8;
            }
        }
        printk("SMP: MPS fp@0x%08x cfg@0x%08x entries=%u, %u enabled processor(s)\r\n",
               fp, cfg, count, n);
    }

    /* 还原 cstart 的取消映射状态 */
    for (pa = 0xE0000; pa < 0x100000; pa += PAGE_SIZE) {
        *vtopte(pa + KERNBASE) = 0;
        invlpg(pa + KERNBASE);
    }
    return n;
}

/**
 * AP 落地后的主体（运行在 trampoline 给的内核栈上，g_task_running 为 NULL
 * ——PROLOGUE 对 NULL 直接走 3f 不拷贝，安全）。S02 只做：使能 LAPIC、
 * 打卡、hlt idle；S03 才有 LAPIC timer 与真实调度。
 */
void ap_main(void)
{
    unsigned id = lapic_id();

    apic_write(LAPIC_SVR, APIC_SV_ENABLE | 0xff);    /* 使能本核 LAPIC */
    g_ap_online[id] = 1;
    printk("SMP: AP online, APIC ID=%u, entering idle loop\r\n", id);
    for (;;)
        cpu_idle();
}

static void smp_boot_aps(void)
{
    void *stack;
    unsigned i, t;
    uint32_t *pdata;
    unsigned long copy_len =
        (unsigned long)(smp_tramp_pdata - smp_tramp_start) + 12;

    stack = kmalloc(PAGE_SIZE);
    if (stack == NULL) {
        printk("SMP: AP stack alloc failed, skip\r\n");
        return;
    }

    /* 页拷贝：目标走 0x8000 的 KERNBASE 别名（VA 0x8000 本体被 cstart 取消
       了映射，直接写会缺页——首跑实测踩到）；源直接用链接期虚拟地址（内核
       镜像在 KERNBASE 段恒在映射，写 -KERNBASE 的裸物理地址反而缺页——
       二跑实测踩到）。拷完往页内参数块写三件事：页目录物理地址 / AP 栈顶 /
       ap_main */
    memcpy((void *)(SMP_TRAMPOLINE_PA + KERNBASE),
           (void *)smp_tramp_start,
           copy_len);
    pdata = (uint32_t *)(SMP_TRAMPOLINE_PA + KERNBASE +
                         ((uint32_t)smp_tramp_pdata - (uint32_t)smp_tramp_start));
    pdata[0] = PD_PA;                       /* AP 开分页前挂这个页目录 */
    pdata[1] = (uint32_t)stack + PAGE_SIZE; /* AP 内核栈顶（虚拟，向下生长） */
    pdata[2] = (uint32_t)ap_main;

    /* 低地址映射：PD[0] 伪页表此刻已由 vm86_init 建好（start_user_task 把
       smp_init 排在 vm86_init/selftest 之后），只往 entry 2 加一个
       trampoline PTE，v86 区域其余映射原样保留；用完即还别名。 */
    {
        uint32_t pd0 = PTD[0];
        uint32_t pt_pa, *pt;

        if (!(pd0 & L2E_V)) {
            printk("SMP: PD[0] not present, AP boot aborted\r\n");
            return;
        }
        pt_pa = pd0 & ~0xFFFu;
        *vtopte(pt_pa + KERNBASE) = pt_pa | L2E_V | L2E_W;
        invlpg(pt_pa + KERNBASE);
        pt = (uint32_t *)(pt_pa + KERNBASE);
        pt[SMP_TRAMPOLINE_PA >> PAGE_SHIFT] =
            SMP_TRAMPOLINE_PA | L2E_V | L2E_W | L2E_U;
        invlpg(SMP_TRAMPOLINE_PA + KERNBASE);
        *vtopte(pt_pa + KERNBASE) = 0;        /* 伪页表帧别名用完即还 */
        invlpg(pt_pa + KERNBASE);
    }

    for (i = 1; i < g_cpu_count; i++) {
        unsigned id = g_apic_ids[i];

        apic_write(LAPIC_SVR, APIC_SV_ENABLE | 0xff);     /* BSP 本核 LAPIC 使能 */
        apic_write(LAPIC_ICR_LO, ICR_INIT);               /* INIT（shorthand=全部含自除外） */
        mdelay(10);                                       /* Intel MP 规范时序 */
        apic_write(LAPIC_ICR_LO, ICR_SIPI(0x08));         /* SIPI vector 0x08 → 0x8000 */
        mdelay(1);
        apic_write(LAPIC_ICR_LO, ICR_SIPI(0x08));         /* 第二发（保险） */
        mdelay(1);

        for (t = 0; t < 20 && !g_ap_online[id]; t++)      /* ≤200ms 等 AP 上线 */
            mdelay(10);

        if (g_ap_online[id])
            printk("SMP: AP cpu%u (APIC ID=%u) boot OK\r\n", i, id);
        else
            printk("SMP: AP cpu%u (APIC ID=%u) boot TIMEOUT\r\n", i, id);
    }
}

/**
 * smp_init：start_user_task 在 calibrate_delay 之后调用（mdelay 依赖
 * loops_per_tick）。-smp 1 下只有 BSP，直接跳过。
 */
void smp_init(void)
{
    g_cpu_count = mp_parse();
    if (g_cpu_count > NR_CPUS) {
        printk("SMP: found %u CPUs, capped to NR_CPUS=%d\r\n", g_cpu_count, NR_CPUS);
        g_cpu_count = NR_CPUS;
    }
    if (g_cpu_count < 2) {
        printk("SMP: single CPU, AP boot skipped\r\n");
        return;
    }
    smp_boot_aps();
}

/**
 * mi_startup 在 init_frame 之后立刻调用：
 * 1) 从帧分配器预留 trampoline 页（U3：低 1MB 在管理区内，不预留会被
 *    kmalloc/page fault 链路分走）；
 * 2) 在共享页表里重映射 0x8000（cstart 已取消 [0,1MB)，而 AP 开分页后
 *    仍要从线性 0x8000 取指走完 ljmp——不映射即取指缺页）。
 */
void smp_reserve_trampoline(void)
{
    if (frame_alloc_in_addr(SMP_TRAMPOLINE_PA, 1) == BITMAP_ERROR)
        printk("SMP: trampoline page 0x8000 not free!\r\n");

    /* KERNBASE 别名映射：BSP 侧拷贝/写参数用 */
    *vtopte(SMP_TRAMPOLINE_PA + KERNBASE) = SMP_TRAMPOLINE_PA | L2E_V | L2E_W;
    invlpg(SMP_TRAMPOLINE_PA + KERNBASE);

    /* 注意：这里【不碰 PD[0]】。PD[0] 是 vm86 时代的「伪页表」（cstart 清零
       低 4MB 槽位后，vm86 区域的 PTE 写落在 PD[0] 指向的帧里，页表转储实测
       PD[0]=0x6067=帧 0x6000 兼任 PT0）——在 vm86_init 之前它还不存在，
       首版在此重建 PD[0] 顶掉了运行期累积的 v86 映射，WAV(VBE)/CAPTURE
       二阶段挂死（make-full-s02-bisect2.log 五腿 3/5 根因）。低地址映射
       改在 smp_boot_aps 里做：start_user_task 已把 smp_init 排到
       vm86_init/selftest 之后，届时伪页表就位，只加一个 PTE。 */
}
