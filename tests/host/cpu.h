#ifndef HOST_CPU_H
#define HOST_CPU_H
/* 宿主侧替身：被测路径不执行端口 I/O，这里只保证能编译。 */
#include <stdint.h>

static inline uint8_t inportb(uint16_t port) { (void)port; return 0; }
static inline void outportb(uint16_t port, uint8_t val) { (void)port; (void)val; }

#endif
