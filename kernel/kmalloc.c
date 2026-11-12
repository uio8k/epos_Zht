/**
 *
 * Copyright (C) 2013 Hong MingJian<hongmingjian@gmail.com>
 * All rights reserved.
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
 */
#include <stddef.h>
#include "cpu.h"
#include "../lib/tlsf/tlsf.h"
#include "spinlock.h"

/* S01：tlsf 非重入，配独立叶子锁（docs/tech-plan-smp-prototype.md C2 /
   s01-lock-research-excerpt.txt §4）：上层（page.c 等）持 g_kernel_lock 时
   调 kmalloc，这里嵌套取自己的锁——两锁无反向依赖，无死锁环。 */
static struct kernel_lock g_kmalloc_lock = { 0, NULL, 0 };

static tlsf_t g_kheap;

void *kmalloc(size_t bytes)
{
    uint32_t flags;
    void *ptr;

    lock_irqsave(&g_kmalloc_lock, flags);
    ptr = tlsf_malloc(g_kheap, bytes);
    lock_irqrestore(&g_kmalloc_lock, flags);

    return ptr;
}

void *krealloc(void *oldptr, size_t bytes)
{
    uint32_t flags;
    void *ptr;

    lock_irqsave(&g_kmalloc_lock, flags);
    ptr = tlsf_realloc(g_kheap, oldptr, bytes);
    lock_irqrestore(&g_kmalloc_lock, flags);

    return ptr;
}

void kfree(void *ptr)
{
    uint32_t flags;

    lock_irqsave(&g_kmalloc_lock, flags);
    tlsf_free(g_kheap, ptr);
    lock_irqrestore(&g_kmalloc_lock, flags);
}

void *kmemalign(size_t align, size_t bytes)
{
    uint32_t flags;
	void *ptr;

    lock_irqsave(&g_kmalloc_lock, flags);
    ptr = tlsf_memalign(g_kheap, align, bytes);
    lock_irqrestore(&g_kmalloc_lock, flags);

	return ptr;
}

void init_kmalloc(void *mem, size_t bytes)
{
	g_kheap = tlsf_create_with_pool(mem, bytes);
}
