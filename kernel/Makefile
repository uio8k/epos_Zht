include ../Makefile.inc

PROG=		eposkrnl

.PHONY: all
all: run

CPPFLAGS=-DVERBOSE=0 -nostdinc -I../include
ASFLAGS=-m32 -march=i386 -Wall -D__ASSEMBLY__
CFLAGS=	-m32 -march=i386 -Wall -pipe -Wno-address-of-packed-member \
	-std=gnu11 \
	-fomit-frame-pointer \
	-ffreestanding -fleading-underscore -mno-ms-bitfields \
	-fno-pie -fno-stack-check -mno-stack-arg-probe -fno-stack-protector

# 采集侧反向门（验收专用，默认关闭）：CAP_REVERSE=1 时跳过 ADC 的 stream tag
# 绑定，用于证明「Pin -> ADC -> 输入流 SDI」这段绑定确实是数据通路的前提。
ifdef CAP_REVERSE
CFLAGS+=-DCAP_REVERSE_SKIP_ADC_BIND
endif

LDFLAGS=-m32 -Tkernel.ld -nostdlib -nostartfiles -nodefaultlibs \
		-Wl,-Map,$(PROG).map -static
ifneq ($(OS),Windows_NT)
LDFLAGS+=-Wl,-melf_i386
endif
LDFLAGS+=-Wl,--build-id=none

COBJS=	ide.o floppy.o e1000.o hda.o hda_stream.o hda_audio.o hda_capture.o hda_trace.o pci.o vm86.o \
	kbd.o timer.o machdep.o task.o mktime.o sem.o \
	page.o startup.o frame.o kmalloc.o dosfs.o pe.o \
	elf.o printk.o bitmap.o
COBJS+=	../lib/softfloat.o ../lib/string.o ../lib/memcpy.o \
		../lib/memset.o ../lib/snprintf.o ../lib/tlsf/tlsf.o

OBJS=	entry.o $(COBJS)

make.dep: *.c *.h
	$(CC) $(CPPFLAGS) -M $(COBJS:.o=.c) >make.dep

.DELETE_ON_ERROR: make.dep
.FORCE:
.PHONY: .FORCE

../lib/tlsf/tlsf.o: .FORCE
../lib/memcpy.o: .FORCE
../lib/memset.o: .FORCE
../lib/snprintf.o: .FORCE
../lib/softfloat.o: .FORCE
../lib/string.o: .FORCE

$(PROG).bin: $(PROG).elf
	$(OBJCOPY) -S -O binary -R .note.* -R .comment $^ $@

$(PROG).elf: kernel.ld $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

kernel.ld: kernel.ld.in
	$(CC) -E -P -x c-header -o $@ $^

.PHONY: debug
debug: CFLAGS+=-O0 -ggdb
debug: make.dep $(PROG).bin

.PHONY: run
run: CFLAGS+=-O2 -ggdb
run: make.dep $(PROG).bin

.PHONY: clean
clean:
	-$(RM) kernel.ld $(OBJS) $(PROG).bin $(PROG).elf $(PROG).map make.dep .~* *.*~ ._* .DS_Store *.stackdump

-include make.dep
