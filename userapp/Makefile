include ../Makefile.inc

PROG=	a

.PHONY: all
all: run

CPPFLAGS=-nostdinc -I../include -Iinclude
ASFLAGS=-m32 -march=i386 -Wall -D__ASSEMBLY__
CFLAGS=	-m32 -march=i386 -Wall -pipe\
	-DSNPRINTF_FLOATPOINT \
	-ffreestanding -fleading-underscore -mno-ms-bitfields \
	-fno-pie -fno-stack-check -mno-stack-arg-probe -fno-stack-protector
LDFLAGS=-m32 -nostdlib -nostartfiles -nodefaultlibs \
		-Wl,-Map,$(PROG).map -static
ifneq ($(OS),Windows_NT)
LDFLAGS+=-Wl,-melf_i386
endif

# 音频质量评估测试信号模式（tools/eval_audio.py 使用）：
#   make TEST_MODE=1 [TEST_SIGNAL=sine|impulse|step]
# main.o 依赖模式标记文件——CFLAGS 变化本身不会触发重建，靠标记文件在
# 切换模式时强制重编译 main.o，避免残留上一次模式的信号。
TEST_MODE   ?= 0
TEST_SIGNAL ?= sine
ifeq ($(TEST_MODE),1)
CFLAGS+= -DTEST_MODE -DTEST_SIGNAL_$(TEST_SIGNAL)
# 测试模式有意排除演示代码（graphics/sort/melody），其函数/变量在此模式下
# 不再被引用；屏蔽相应的 unused 警告以维持零警告构建。
CFLAGS+= -Wno-unused-function -Wno-unused-variable
endif
MODE_STAMP = .testmode_$(TEST_MODE)_$(TEST_SIGNAL)
main.o: $(MODE_STAMP)
$(MODE_STAMP):
	-$(RM) .testmode_*
	touch $@

COBJS=	vm86call.o graphics.o main.o melody_player.o NoteSequence.o
COBJS+=	lib/sysconf.o lib/math.o lib/stdio.o lib/stdlib.o \
		lib/qsort.o
COBJS+=	../lib/softfloat.o ../lib/string.o ../lib/memcpy.o \
		../lib/memset.o ../lib/snprintf.o
COBJS+=	lib/malloc.o ../lib/tlsf/tlsf.o

OBJS=	lib/crt0.o lib/setjmp.o lib/syscall-wrapper.o $(COBJS)

$(PROG).out: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

make.dep: *.c *.h
	$(CC) $(CPPFLAGS) -M $(COBJS:.o=.c) lib/syscall-wrapper.S >make.dep

.DELETE_ON_ERROR: make.dep
.FORCE:
.PHONY: .FORCE

../lib/tlsf/tlsf.o: .FORCE
../lib/memcpy.o: .FORCE
../lib/memset.o: .FORCE
../lib/snprintf.o: .FORCE
../lib/softfloat.o: .FORCE
../lib/string.o: .FORCE

.PHONY: debug
debug: CFLAGS+=-O0 -ggdb
debug: make.dep $(PROG).out

.PHONY: run
run: CFLAGS+=-O2 -ggdb
run: make.dep $(PROG).out

.PHONY: clean
clean:
	-$(RM) $(OBJS) $(PROG).out $(PROG).map make.dep .testmode_* .~* *.*~ ._* .DS_Store *.stackdump

-include make.dep
