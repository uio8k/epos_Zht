#ifndef HOST_KERNEL_H
#define HOST_KERNEL_H
/* 宿主侧替身：只提供被测代码真正用到的内核原语。
   放在 tests/build/ 里，靠引号包含的就近规则覆盖 kernel/kernel.h。 */
#include <stdint.h>
#include <stddef.h>

void printk(const char *fmt, ...);
void panic(const char *msg);

void *kmalloc(size_t bytes);
void  kfree(void *ptr);

#endif
