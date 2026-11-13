/**
 * vim: filetype=c:fenc=utf-8:ts=4:et:sw=4:sts=4
 *
 * Copyright (C) 2013 Hong MingJian<hongmingjian@gmail.com>
 * All rights reserved.
 *
 * This file is part of the EPOS.
 *
 * Redistribution and use in source and binary forms are freely
 * permitted provided that the above copyright notice and this
 * paragraph and the following disclaimer are duplicated in all
 * such forms.
 *
 * This software is provided "AS IS" and without any express or
 * implied warranties, including, without limitation, the implied
 * warranties of merchantability and fitness for a particular
 * purpose.
 *
 */
#ifndef _KERNEL_H
#define _KERNEL_H

#include <sys/types.h>
#include <inttypes.h>
#include <time.h>
#include "machdep.h"

/*中断向量表*/
extern void (*g_intr_vector[])(uint32_t irq, struct context *ctx);

/*让中断控制器关闭某个中断*/
void disable_irq(uint32_t irq);

/*让中断控制器打开某个中断*/
void enable_irq(uint32_t irq);

/*定时器以HZ的频率中断CPU*/
#define HZ   100

/*记录系统启动以来，定时器中断的次数*/
extern unsigned volatile g_timer_ticks;

/*计算机启动时，自1970-01-01 00:00:00 +0000 (UTC)以来的秒数*/
extern time_t g_startup_time;

time_t mktime(struct tm *tm);

/**
 * 线程控制块
 *
 *       4 kB +---------------------------------+
 *            |          kernel stack           |
 *            |                |                |
 *            |                |                |
 *            |                V                |
 *            |         grows downward          |
 *            |                                 |
 *            |                                 |
 *            |                                 |
 *            |                                 |
 *            |                                 |
 *            |                                 |
 *            |                                 |
 *            |                                 |
 *            +---------------------------------+
 *            |            signature            |
 *            |                :                |
 *            |                :                |
 *            |               tid               |
 *            |              kstack             |
 *       0 kB +---------------------------------+
 */
struct tcb {
    /*hardcoded*/
    uint32_t    kstack;      /*saved top of the kernel stack for this task*/

    int         tid;         /* task id */
    int         state;       /* -1:waiting,0:running,1:ready,2:zombie */
#define TASK_STATE_WAITING  -1
#define TASK_STATE_RUNNING   0
#define TASK_STATE_READY     1
#define TASK_STATE_ZOMBIE    2
    /* S03 双跑保护：RUNNING 表示「已认领，正跑在某个核上」。schedule 挑选
       条件只认 READY（RUNNING 被另一核挑中会双跑互踩 kstack）；认领/降级
       全部在 g_kernel_lock 内完成。task0/AP idle 由 run_as_task0/认领路径
       直接上核，同样遵守该语义。 */

    int         timeslice;   //时间片
#define TASK_TIMESLICE_DEFAULT 4

    int         code_exit;   //保存该线程的退出代码
    struct wait_queue *wq_exit; //等待该线程退出的队列

    int         cpu_binded;  /* S03 CPU 亲和：<0 任意核；>=0 只准跑在该核。
                                用户任务（tos!=0）创建即钉 CPU0（C1）；内核
                                线程 -1；AP idle/demo 线程用
                                sys_task_create_cpu 显式绑核。 */
    struct tcb  *next;
    struct fpu   fpu;        //数学协处理器的寄存器

    uint32_t     signature;  //必须是最后一个字段
#define TASK_SIGNATURE 0x20160201
};

#define TASK_KSTACK 0           /*=offsetof(struct tcb, kstack)*/

/* Per-CPU 当前线程：下标 = 本核 LAPIC ID（QEMU -smp 2 下 ID=0/1，
   ID=index 见 docs/tech-plan-smp-prototype.md U4；-smp 1 恒 0）。
   g_task_running 宏让既有代码零改动；entry.S 直接读 _g_task_running_arr
   （LAPIC ID 直读，C5）。NR_CPUS 自 S02 起为 2。 */
#define NR_CPUS 2
extern struct tcb *g_task_running_arr[NR_CPUS];
#define g_task_running (g_task_running_arr[lapic_id() & (NR_CPUS - 1)])

extern struct tcb *g_task_head;
extern struct tcb *task0;
extern struct tcb *g_task_own_fpu;

/* AP 的 idle 任务（S03）：smp_boot_aps 在 INIT/SIPI 之前为每个 AP 创建
   （绑本核），AP 当前任务阻塞后 schedule 的 fallback 去处——task0 属于
   CPU0，绝不能被 AP 借跑。-smp 1 下不创建（恒 NULL）。 */
extern struct tcb *g_ap_idle[NR_CPUS];

/* 全局内核锁（S01，docs/tech-plan-smp-prototype.md C2）：owner+depth 递归
   自旋锁，lock_irqsave/lock_irqrestore 用法见 spinlock.h；定义在 kernel/task.c。
   睡眠契约：sleep_on 调用方必须持有本锁且 depth==1（sleep_on 切出前放锁）。 */
extern struct kernel_lock g_kernel_lock;

/* 强制重调度请求，per-CPU（S03）：本核 ISR 置位、本核 HWINT 尾部消费——
   全局标量在双核下会被别的核错误消费（entry.S:358 旧读法），数组化后
   语义收敛到「核内」。entry.S 读 _g_resched_arr（取核号 4 条指令）。 */
extern volatile int g_resched_arr[NR_CPUS];
#define g_resched (g_resched_arr[lapic_id() & (NR_CPUS - 1)])

/* 每核节拍计数（S03）：本核 LAPIC/PIC timer ISR 各自递增。墙钟
   g_timer_ticks 只由 CPU0 推进（否则双核 200Hz 会把 gettimeofday/
   sys_sleep/音频计时推快一倍），本数组才是「每核节拍独立」的证据面。 */
extern volatile unsigned g_cpu_ticks[NR_CPUS];

void schedule();
void switch_to(struct tcb *new);

struct wait_queue {
    struct tcb *tsk;
    struct wait_queue *next;
};
/* sleep_on：调用方持 g_kernel_lock（depth==1）进入；内部「入队+置
   WAITING」在锁内完成（防丢唤醒），切出前放锁（C6）；被唤醒后取回锁摘链，
   返回时已放锁——调用方如需继续访问共享状态须重新 lock_irqsave。 */
void sleep_on(struct wait_queue **head);
void wake_up(struct wait_queue **head, int n);

void init_task(void);
void syscall(struct context *ctx);
extern void *ret_from_syscall;

/**
 * `VADDR' comes from FreeBSD
 */
#define VADDR(pdi, pti) ((uint32_t)(((pdi)<<PGDR_SHIFT)|((pti)<<PAGE_SHIFT)))

#define KERN_MAX_ADDR VADDR(1023, 1023)
#define KERN_MIN_ADDR VADDR(767, 767)
#define USER_MAX_ADDR VADDR(767, 0)
#define USER_MIN_ADDR VADDR(1, 0)

#define KERNBASE  VADDR(768, 0)
#define R(x) ((x)-KERNBASE)

#define NR_KERN_PAGETABLE 20

#define IN_USER_VM(va, len) \
       ((uint32_t)(va) >= USER_MIN_ADDR && \
        (uint32_t)(va) <  USER_MAX_ADDR && \
        (uint32_t)(va) + (len) <= USER_MAX_ADDR)

/**
 * `PT', `PTD' and `vtopte' come from FreeBSD
 */
extern uint8_t   end;
extern uint32_t *PT;
extern uint32_t *PTD;
#define vtopte(va) (PT+((va)>>PAGE_SHIFT))
#define vtop(va) (((*vtopte(va))&(~PAGE_MASK))|((va)&PAGE_MASK))

void  init_kmalloc(void *mem, size_t bytes);
void *kmalloc(size_t bytes);
void *krealloc(void *oldptr, size_t size);
void  kfree(void *ptr);
void *kmemalign(size_t align, size_t bytes);

#define RAM_ZONE_LEN (2 * 8)
extern uint32_t g_ram_zone[RAM_ZONE_LEN];

int do_page_fault(struct context *ctx, uint32_t vaddr, uint32_t code);

int     sys_putchar(int c);
int     sys_getchar();

struct tcb *sys_task_create(void *tos, void (*func)(void *pv), void *pv);
/* 绑核版创建（S03）：内核线程钉在指定核上（AP idle 任务、SMP demo 线程
   用）。tos 恒 NULL（纯内核线程）；cpu 越界返回 NULL。 */
struct tcb *sys_task_create_cpu(unsigned cpu, void (*func)(void *pv), void *pv);
void        sys_task_exit(int code_exit);
int         sys_task_wait(int tid, int *pcode_exit);
int         sys_task_getid();
void        sys_task_yield();

/* 计数信号量（kernel/sem.c）：句柄是静态池里的对象地址，NULL 表示失败。
   wait 在计数为 0 时睡眠，signal 唤醒全部等待者；临界区全程关中断。 */
void *sem_create(int value);
int   sem_destroy(void *handle);
int   sem_wait(void *handle);
int   sem_signal(void *handle);

/* L4 整机自检（kernel/selftest.c）：返回失败用例数，0 = 全通过 */
int   selftest_run(void);

int printk(const char *fmt,...);

void     init_vmspace(uint32_t brk);
uint32_t page_alloc(int npages, uint32_t prot, uint32_t user);
uint32_t page_alloc_in_addr(uint32_t va, int npages, uint32_t prot);
int      page_free(uint32_t va, int npages);
uint32_t page_prot(uint32_t va);
#define VM_PROT_NONE   0x00
#define VM_PROT_READ   0x01
#define VM_PROT_WRITE  0x02
#define VM_PROT_EXEC   0x04
#define VM_PROT_RW     (VM_PROT_READ|VM_PROT_WRITE)
#define VM_PROT_ALL    (VM_PROT_RW  |VM_PROT_EXEC)

void     page_map(uint32_t vaddr, uint32_t paddr, uint32_t npages, uint32_t flags);
void     page_unmap(uint32_t vaddr, uint32_t npages);

uint32_t init_frame(uint32_t brk);
uint32_t frame_alloc(uint32_t npages);
uint32_t frame_alloc_in_addr(uint32_t pa, uint32_t npages);
void     frame_free(uint32_t paddr, uint32_t npages);

void     calibrate_delay(void);
void     mdelay(unsigned ms);     /* 毫秒忙等（loops_per_tick 定标，S02 AP 时序用） */
unsigned sys_sleep(unsigned seconds);
int      sys_nanosleep(const struct timespec *rqtp, struct timespec *rmtp);
struct timeval {
	long tv_sec;
	long tv_usec;
};

struct timezone {
	int tz_minuteswest;
	int tz_dsttime;
};
int sys_gettimeofday(struct timeval *tv, struct timezone *tz);

/* SMP（kernel/smp.c，S02）：MPS 解析 + AP 启动。smp_reserve_trampoline 在
   init_frame 后立刻调用（帧分配器预留 0x8000）；smp_init 在 calibrate_delay
   之后调用（mdelay 依赖 loops_per_tick）。-smp 1 下两者均为空转。 */
void     smp_init(void);
void     smp_reserve_trampoline(void);

void     mi_startup();
#endif /*_KERNEL_H*/
