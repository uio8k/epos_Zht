/**
 * vim: filetype=c:fenc=utf-8:ts=4:et:sw=4:sts=4
 *
 * Copyright (C) 2008, 2013 Hong MingJian<hongmingjian@gmail.com>
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
#include <stddef.h>
#include <string.h>
#include "kernel.h"
#include "spinlock.h"

volatile int g_resched_arr[NR_CPUS];
struct tcb *g_task_head;
struct tcb *g_task_running_arr[NR_CPUS];
struct kernel_lock g_kernel_lock = { 0, NULL, 0 };
struct tcb *task0;
struct tcb *g_task_own_fpu;
struct tcb *g_ap_idle[NR_CPUS];

/* 亲和检查（S03 D3）：cpu_binded<0 任意核，否则只准跑在绑定的核上 */
static int task_allowed_on(struct tcb *t, unsigned cpu)
{
    return (t->cpu_binded < 0) || ((unsigned)t->cpu_binded == cpu);
}

/* 当前核阻塞后的 idle 去处：CPU0 用 task0（教科书内核的空闲线程），
   AP 用各自的 g_ap_idle（task0 属于 CPU0，借给 AP 会双跑互踩 kstack）。
   AP idle 缺失只可能出现在配置矛盾（-smp 2 但创建失败）时，防御性停机。 */
static struct tcb *idle_for(unsigned cpu)
{
    if (cpu == 0)
        return task0;
    if (g_ap_idle[cpu] == NULL) {
        printk("fatal: no idle task for cpu%u\r\n", cpu);
        for (;;)
            cpu_idle();
    }
    return g_ap_idle[cpu];
}

/**
 * CPU调度器函数，这里只实现了轮转调度算法
 *
 * S03 双核化（docs/tech-plan-smp-prototype.md §4.1 D1-D4）：
 *  - 挑选全程持 g_kernel_lock，选中即认领（READY→RUNNING），另一核的
 *    挑选条件 state==TASK_STATE_READY 天然跳过已认领任务——双跑防护。
 *    切出方的降级（RUNNING→READY）在 switch_to 保存现场之后做（认领
 *    协议后半步）——降早了另一核会读到过期帧（实现期复查发现的真实
 *    竞态，见 switch_to 注释）。
 *  - 挑选与 fallback 都按 task_allowed_on 过滤亲和（C1：用户任务钉 CPU0）。
 *  - cur==NULL（AP 落地初期）从表头认领第一个可用任务；没有就空手返回，
 *    ap_main 继续 hlt idle。
 *
 * 注意：该函数的执行不能被中断
 */
void schedule()
{
    uint32_t flags;
    struct tcb *select, *cur = g_task_running;
    unsigned cpu = lapic_id() & (NR_CPUS - 1);

    lock_irqsave(&g_kernel_lock, flags);

    if (cur == NULL) {
        /* AP 认领首个任务：从任务表头线性找 READY 且亲和兼容者 */
        select = g_task_head;
        while (select != NULL) {
            if ((select->tid != 0) &&
                (select->state == TASK_STATE_READY) &&
                task_allowed_on(select, cpu))
                break;
            select = select->next;
        }
        if (select != NULL) {
            select->state = TASK_STATE_RUNNING;
        }
        lock_irqrestore(&g_kernel_lock, flags);
        if (select != NULL)
            /* old==NULL：无现场可存，直接进新任务。注意不要在这里先写
               g_task_running——switch_to 以 current 作 old，提前认领会
               让保存半段覆盖新任务的 INIT_TASK_CONTEXT 启动帧。 */
            switch_to(select);
        return;
    }

    select = cur;
    do {
        select = select->next;
        if(select == NULL)
            select = g_task_head;
        if(select == cur)
            break;
        if((select->tid != 0) &&
           (select->state == TASK_STATE_READY) &&
           task_allowed_on(select, cpu))
            break;
    } while(1);

    if(select == cur) {
        if(cur->state == TASK_STATE_READY ||
           cur->state == TASK_STATE_RUNNING) {
            /* 当前任务仍可跑（含 RUNNING：被时间片打断但无他者可选），
               继续运行 */

            lock_irqrestore(&g_kernel_lock, flags);
            return;
        }
        /* 当前已阻塞/退出：切到本核 idle */
        select = idle_for(cpu);

    } else {

    }

    if(select->signature != TASK_SIGNATURE)
        printk("warning: kernel stack of task #%d overflow!!!", select->tid);

    g_resched = 0;

    if (select != cur)
        select->state = TASK_STATE_RUNNING;     /* 认领：另一核跳过它 */

    /* 切出前放锁（C6）：锁不能被 switch_to 冻结在被切出的核上。
       cur 的降级（RUNNING→READY）由 switch_to 在保存现场之后完成。 */

    lock_irqrestore(&g_kernel_lock, flags);
    switch_to(select);
}

/**
 * 把当前线程切换为等待状态，等待在*head队列中
 *
 * 注意：该函数的执行不能被中断
 */
void sleep_on(struct wait_queue **head)
{
    struct wait_queue wait;

    wait.tsk = g_task_running;
    wait.next = *head;
    *head = &wait;

    g_task_running->state = TASK_STATE_WAITING;

    /* C6：放锁再切。若带着 g_kernel_lock 睡眠，switch_to 的 popfl 会把
       持锁状态冻结在被切出的核上（线程不再运行，无从 release），
       其它核将自旋至死锁。「入队 + 置 WAITING」已在调用方持有的同一把
       锁内完成，跨核无丢唤醒窗口。 */
    kernel_lock_release(&g_kernel_lock);

    schedule();


    /* 唤醒后取回锁摘链，返回时放锁（调用方按 kernel.h 的契约处理） */
    kernel_lock_acquire(&g_kernel_lock);

    if(*head == &wait)
        *head = wait.next;
    else {
        struct wait_queue *p, *q;
        p = *head;
        do {
            q = p;
            p = p->next;
            if(p == &wait) {
                q->next = p->next;
                break;
            }
        } while(p != NULL);
    }

    kernel_lock_release(&g_kernel_lock);
}

/**
 * 唤醒n个等待在*head队列中的线程。
 * 如果n<0，唤醒队列中的所有线程
 *
 * 注意：该函数的执行不能被中断
 */
void wake_up(struct wait_queue **head, int n)
{
    struct wait_queue *p;

    for(p = *head; (p!=NULL) && n; p = p->next, n--)
        p->tsk->state = TASK_STATE_READY;
}

static
void add_task(struct tcb *tsk)
{
    if(g_task_head == NULL)
        g_task_head = tsk;
    else {
        struct tcb *p, *q;
        p = g_task_head;
        do {
            q = p;
            p = p->next;
        } while(p != NULL);
        q->next = tsk;
    }
}

static
void remove_task(struct tcb *tsk)
{
    if(g_task_head != NULL) {
        if(tsk == g_task_head) {
            g_task_head = g_task_head->next;
        } else {
            struct tcb *p, *q;
            p = g_task_head;
            do {
                q = p;
                p = p->next;
                if(p == tsk)
                    break;
            } while(p != NULL);

            if(p == tsk)
                q->next = p->next;
        }
    }
}

static
struct tcb* get_task(int tid)
{
    struct tcb *tsk;

    tsk = g_task_head;
    while(tsk != NULL) {
        if(tsk->tid == tid)
            break;
        tsk = tsk->next;
    }

    return tsk;
}

/**
 * 系统调用task_create的执行函数
 *
 * 创建一个新的线程，该线程执行func函数，并向新线程传递参数pv
 */
struct tcb *sys_task_create(void *tos,
                            void (*func)(void *pv), void *pv)
{
    static int tid = 0;
    struct tcb *new;
    char *p;
    uint32_t flags;
    uint32_t ustack=(uint32_t)tos;

    if(ustack & 3)
        return NULL;

    p = (char *)kmemalign(PAGE_SIZE, PAGE_SIZE);
    if(p == NULL)
        return NULL;

    new = (struct tcb *)p;

    memset(new, 0, sizeof(struct tcb));

    new->kstack = (uint32_t)(p+PAGE_SIZE);
    new->tid = tid++;
    new->state = TASK_STATE_READY;
    new->timeslice = TASK_TIMESLICE_DEFAULT;
    new->wq_exit = NULL;
    new->cpu_binded = (ustack != 0) ? 0 : -1;  /* S03：用户任务钉 CPU0（C1），
                                                  内核线程任意核 */
    new->next = NULL;
    new->signature = TASK_SIGNATURE;

    /*XXX - should be elsewhere*/
    new->fpu.cwd = 0x37f;
    new->fpu.twd = 0xffff;

    INIT_TASK_CONTEXT(ustack, new->kstack, func, pv);

    lock_irqsave(&g_kernel_lock, flags);
    add_task(new);
    lock_irqrestore(&g_kernel_lock, flags);

    return new;
}

/**
 * 绑核版创建（S03）：把内核线程钉在指定核上。AP idle 任务与 SMP 验收
 * demo 线程用它；创建逻辑与 sys_task_create 完全一致，仅覆盖亲和。
 */
struct tcb *sys_task_create_cpu(unsigned cpu, void (*func)(void *pv), void *pv)
{
    struct tcb *new;

    if (cpu >= NR_CPUS)
        return NULL;

    new = sys_task_create(NULL, func, pv);
    if (new != NULL)
        new->cpu_binded = (int)cpu;

    return new;
}

/**
 * 系统调用task_exit的执行函数
 *
 * 结束当前线程，code_exit是它的退出代码
 */
void sys_task_exit(int code_exit)
{
    uint32_t flags;

    lock_irqsave(&g_kernel_lock, flags);

    wake_up(&g_task_running->wq_exit, -1);

    g_task_running->code_exit = code_exit;
    g_task_running->state = TASK_STATE_ZOMBIE;

    if(g_task_own_fpu == g_task_running)
        g_task_own_fpu = NULL;

    /* 先放锁再调度（schedule 自带取锁，带着锁进去会自死锁） */

    lock_irqrestore(&g_kernel_lock, flags);
    schedule();
}

/**
 * 系统调用task_wait的执行函数
 *
 * 当前线程等待线程tid结束执行。
 * 如果pcode_exit不是NULL，用于保存线程tid的退出代码
 */
int sys_task_wait(int tid, int *pcode_exit)
{
    uint32_t flags;
    struct tcb *tsk;

    if(g_task_running == NULL)
        return -1;

    if(tid == 0)
        return -1;

    lock_irqsave(&g_kernel_lock, flags);

    if((tsk = get_task(tid)) == NULL) {
        lock_irqrestore(&g_kernel_lock, flags);
        return -1;
    }

    /* S04：轮询等待（与 sys_sleep 的 _sleep 同一 yield 轮询习语），每圈
       重查状态、无伪唤醒窗口。不能 sleep_on：task0 在 selftest 里会等待
       绑在别的核上的 SMP 线程，此时 CPU0 的 schedule fallback 只剩 task0
       自己（别的核的任务被亲和过滤）——select==cur 的自切换会直接恢复刚
       保存的睡眠帧＝伪唤醒，task0 以 WAITING 状态继续跑、把还活着的线程
       当 ZOMBIE 提前收割（remove+kfree，快于 AP 的 10ms tick，线程一次都
       没跑过）。实测：SMPTEST dual n1=0、mutex counter=10000、crosswake
       rc=-1 全部源于此（pmemsave 转储：task0 state=WAITING）。 */
    while (tsk->state != TASK_STATE_ZOMBIE) {
        lock_irqrestore(&g_kernel_lock, flags);
        sys_task_yield();
        lock_irqsave(&g_kernel_lock, flags);
    }

    if(pcode_exit != NULL)
        *pcode_exit= tsk->code_exit;

    if(tsk->wq_exit == NULL) {
        remove_task(tsk);
        //printk("%d: Task %d reaped\r\n", sys_task_getid(), tsk->tid);
        lock_irqrestore(&g_kernel_lock, flags);

        kfree(tsk);
        return 0;
    }

    lock_irqrestore(&g_kernel_lock, flags);
    return 0;
}

/**
 * 系统调用task_getid的执行函数
 *
 * 获取当前线程的tid
 */
int sys_task_getid()
{
    return (g_task_running==NULL)?-1:g_task_running->tid;
}

/**
 * 系统调用task_yield的执行函数
 *
 * 当前线程主动放弃CPU，让调度器调度其他线程运行
 */
void sys_task_yield()
{
    uint32_t flags;
    save_flags_cli(flags);
    schedule();
    restore_flags(flags);
}

/**
 * 初始化多线程子系统
 */
void init_task()
{
    int i;
    for (i = 0; i < NR_CPUS; i++) {
        g_resched_arr[i] = 0;
        g_task_running_arr[i] = NULL;
        g_ap_idle[i] = NULL;
    }
    g_task_head = NULL;
    g_task_own_fpu = NULL;

    /*
     * 创建线程task0，即系统空闲线程
     */
    task0 = sys_task_create(NULL, NULL/*task0执行的函数将由run_as_task0填充*/, NULL);
}
