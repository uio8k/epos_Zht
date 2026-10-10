#include "syscall-nr.h"

#define WRAPPER(name) \
  .globl _ ## name; \
_ ## name: \
    movl $SYSCALL_ ## name, %eax; \
    int $0x82; \
    ret

WRAPPER(task_exit)
WRAPPER(task_create)
WRAPPER(task_getid)
WRAPPER(task_yield)
WRAPPER(task_wait)
WRAPPER(reboot)
WRAPPER(mmap)
WRAPPER(munmap)
WRAPPER(sleep)
WRAPPER(nanosleep)
WRAPPER(beep)
WRAPPER(vm86)
WRAPPER(putchar)
WRAPPER(getchar)
WRAPPER(recv)
WRAPPER(send)
WRAPPER(ioctl)
WRAPPER(gettimeofday)
WRAPPER(audio_play_file_ms)
WRAPPER(audio_play_silence_ms)
WRAPPER(audio_stop)
WRAPPER(audio_play_voices_ms)
WRAPPER(audio_set_volume)
WRAPPER(audio_get_errors)
WRAPPER(audio_get_position_ms)
WRAPPER(audio_play_pcm_ms)
