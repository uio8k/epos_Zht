/**
 * vim: filetype=c:fenc=utf-8:ts=4:et:sw=4:sts=4
 *
 * 计数信号量。上游原样是四个 return -1 的空桩，没有任何实现；
 * kernel/callout.c 里 sem_create/sem_wait/sem_signal 的调用方因此从未可用
 * （该文件也不在 kernel/Makefile 的 COBJS 里，未被编译）。
 *
 * 语义按 OSDev Semaphore：wait 在计数为 0 时把当前线程移出调度队列，signal
 * 时再放回；「检查计数 + 入队」全程关中断，避免 lost wake-up。摘录见
 * docs/evidence/2026-11-11/osdev-semaphore-excerpt.txt。
 */
#include <stddef.h>
#include "kernel.h"
#include "spinlock.h"

#define SEM_POOL_SIZE   16
#define SEM_SIGNATURE   0x53454d31  /* 'S','E','M','1' */

struct semaphore {
    int     value;          /* >0 可直接通过，<=0 需要等待 */
    int     waiters;        /* 当前排在 wq 上的线程数（destroy 要用） */
    struct wait_queue *wq;  /* 等待队列 */
    uint32_t signature;
};

static struct semaphore g_sem_pool[SEM_POOL_SIZE];

static struct semaphore *sem_lookup(void *handle)
{
    uint32_t p = (uint32_t)handle;
    uint32_t base = (uint32_t)&g_sem_pool[0];
    uint32_t off;

    if (handle == NULL)
        return NULL;
    if (p < base || p >= base + (uint32_t)sizeof(g_sem_pool))
        return NULL;
    off = p - base;
    if (off % sizeof(struct semaphore) != 0)
        return NULL;
    if (g_sem_pool[off / sizeof(struct semaphore)].signature != SEM_SIGNATURE)
        return NULL;

    return &g_sem_pool[off / sizeof(struct semaphore)];
}

void *sem_create(int value)
{
    uint32_t flags;
    int i;

    if (value < 0)
        return NULL;

    lock_irqsave(&g_kernel_lock, flags);
    for (i = 0; i < SEM_POOL_SIZE; i++) {
        if (g_sem_pool[i].signature == SEM_SIGNATURE)
            continue;
        g_sem_pool[i].value = value;
        g_sem_pool[i].waiters = 0;
        g_sem_pool[i].wq = NULL;
        g_sem_pool[i].signature = SEM_SIGNATURE;
        lock_irqrestore(&g_kernel_lock, flags);
        return (void *)&g_sem_pool[i];
    }
    lock_irqrestore(&g_kernel_lock, flags);

    return NULL;
}

/**
 * 等待信号量：计数 >0 时原子递减后返回 0；计数为 0 时睡眠，直到被 signal
 * 唤醒并成功递减。句柄非法或已被销毁返回 -1。
 */
int sem_wait(void *handle)
{
    struct semaphore *sem = sem_lookup(handle);
    uint32_t flags;

    if (sem == NULL)
        return -1;

    lock_irqsave(&g_kernel_lock, flags);
    while (sem->value <= 0) {
        sem->waiters++;
        sleep_on(&sem->wq);              /* 放锁睡眠，返回时锁已放、中断仍关 */
        restore_flags(flags);            /* 恢复睡眠前的中断态 */
        lock_irqsave(&g_kernel_lock, flags); /* 重新持锁重查条件（C6 契约） */
        sem->waiters--;
        if (sem->signature != SEM_SIGNATURE) {
            lock_irqrestore(&g_kernel_lock, flags);
            return -1;
        }
    }
    sem->value--;
    lock_irqrestore(&g_kernel_lock, flags);

    return 0;
}

/**
 * 释放信号量：计数加一，唤醒全部等待者（它们各自重查计数，只有一个能拿到
 * 名额）。不用「只唤醒 n 个」是因为计数 >1 时会漏唤醒，OSDev 把这类错误
 * 单列为 lost wake-up。
 */
int sem_signal(void *handle)
{
    struct semaphore *sem = sem_lookup(handle);
    uint32_t flags;

    if (sem == NULL)
        return -1;

    lock_irqsave(&g_kernel_lock, flags);
    sem->value++;
    wake_up(&sem->wq, -1);
    lock_irqrestore(&g_kernel_lock, flags);

    return 0;
}

/**
 * 销毁信号量。契约：调用时不得有线程正等待在它上面（waiters 必须为 0）——
 * 有等待者时返回 -1 且不改动任何状态。这样避免把 wq 清成 NULL 后
 * sleep_on 摘链时解引用空指针（kernel/task.c:80-93）。
 */
int sem_destroy(void *handle)
{
    struct semaphore *sem = sem_lookup(handle);
    uint32_t flags;
    int rc = 0;

    if (sem == NULL)
        return -1;

    lock_irqsave(&g_kernel_lock, flags);
    if (sem->waiters != 0) {
        rc = -1;
    } else {
        sem->value = 0;
        sem->wq = NULL;
        sem->signature = 0;
    }
    lock_irqrestore(&g_kernel_lock, flags);

    return rc;
}

/**
 * id 接口：semid = 池下标 + 1，0 表示非法。sys_sem_* 原本是系统调用桩，
 * 这里让它们成为同一实现的数值句柄包装（syscall 号仍未注册，见
 * docs/tech-plan-selftest-l4.md §6）。
 */
static void *sem_by_id(int semid)
{
    if (semid < 1 || semid > SEM_POOL_SIZE)
        return NULL;
    return (void *)&g_sem_pool[semid - 1];
}

int sys_sem_create(int value)
{
    void *handle = sem_create(value);
    int i;

    if (handle == NULL)
        return -1;
    for (i = 0; i < SEM_POOL_SIZE; i++) {
        if ((void *)&g_sem_pool[i] == handle)
            return i + 1;
    }

    return -1;
}

int sys_sem_destroy(int semid)
{
    return sem_destroy(sem_by_id(semid));
}

int sys_sem_wait(int semid)
{
    return sem_wait(sem_by_id(semid));
}

int sys_sem_signal(int semid)
{
    return sem_signal(sem_by_id(semid));
}
