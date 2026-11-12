/**
 * vim: filetype=c:fenc=utf-8:ts=4:et:sw=4:sts=4
 *
 * 自旋锁原语（S01，docs/tech-plan-smp-prototype.md §3 C2）。
 *
 * 原子性来自 __sync_lock_test_and_set → xchg 指令（Intel：xchg 对内存操作数
 * 恒发 LOCK#，隐含全屏障；i486+ 恒可用，无 libatomic 依赖）。发射实测见
 * docs/evidence/2026-11-13/s01-lock-research-excerpt.txt §1。
 *
 * g_kernel_lock 是 owner+depth 递归锁：同一线程重复 acquire 只加深度（调用图
 * 里 page_alloc→page_map 这类内部嵌套不构成自死锁）。同核 ISR 不可能撞上
 * 持锁线程的 owner——持锁区间 IF=0，同核中断进不来；跨核 ISR 的 current
 * 必不相同 → 自旋等待，语义正确。睡眠点（sleep_on）契约：调用方持有
 * g_kernel_lock 且 depth==1；sleep_on 切出前放锁、唤醒后取回锁摘链、
 * 返回时放锁（C6，防止锁被睡眠线程带走过期——switch_to 的 popfl 会把
 * 持锁状态冻结在被切出的核上）。
 */
#ifndef _SPINLOCK_H
#define _SPINLOCK_H

#include <stddef.h>     /* NULL（freestanding，repo include/ 自带） */
#include "cpu.h"
#include "kernel.h"     /* struct tcb / g_task_running */

typedef volatile uint32_t spinlock_t;

static __inline void spin_lock(spinlock_t *l)
{
    while (__sync_lock_test_and_set(l, 1)) {
        while (*l)
            cpu_pause();
    }
}

static __inline void spin_unlock(spinlock_t *l)
{
    __sync_lock_release(l);
}

/* 递归内核锁：owner 是当前线程的 tcb 指针，depth 记嵌套层数 */
struct kernel_lock {
    spinlock_t      locked;
    struct tcb     *owner;
    int             depth;
};

static __inline void kernel_lock_acquire(struct kernel_lock *l)
{
    if (l->owner == g_task_running) {
        l->depth++;
        return;
    }
    spin_lock(&l->locked);
    l->owner = g_task_running;
    l->depth = 1;
}

static __inline void kernel_lock_release(struct kernel_lock *l)
{
    if (l->owner != g_task_running)
        return;                 /* 未持锁的 release：不动作（防御） */
    if (--l->depth == 0) {
        l->owner = NULL;
        spin_unlock(&l->locked);
    }
}

/* save_flags_cli/restore_flags 的锁化替身：同名 flags 语义不变 */
#define lock_irqsave(l, flags)      \
    do {                            \
        save_flags_cli(flags);      \
        kernel_lock_acquire(l);     \
    } while (0)

#define lock_irqrestore(l, flags)   \
    do {                            \
        kernel_lock_release(l);     \
        restore_flags(flags);       \
    } while (0)

#endif /*_SPINLOCK_H*/
