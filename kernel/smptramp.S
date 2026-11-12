#
# vim: filetype=asm:fenc=utf-8:ts=4:et:sw=4:sts=4
#
# S02 AP 启动 trampoline（docs/tech-plan-smp-prototype.md S02 / U7）。
#
# BSP 把本段代码 memcpy 到物理页 0x8000（SIPI vector 0x08 → CS=0x0800、
# IP=0 → 线性 0x08000），AP 在 16 位实模式从 _smp_tramp_start 起跑：
#
#   实模式(cli/段清零/栈) → 自带 mini-GDT 进保护模式(PE, 远跳换 CS)
#   → 平坦数据段 → 挂共享页目录（页内参数块 pdata[0]，BSP 填写）→ 开分页
#   → 远跳 KCSEL 平坦段（此后 EIP 为绝对线性）→ 从页内参数块取
#   pdata[1]=AP 内核栈顶、pdata[2]=ap_main 进入 C。
#
# 页 0x8000 在共享页表里已由 smp_reserve_trampoline 重新映射（cstart 取消了
# [0,1MB)，不重映射则开分页后的取指缺页），故开分页后继续在页内取指、
# 经页内参数块取虚拟地址均成立。
#
# 页内绝对地址一律写成 (标号 - _smp_tramp_start + 0x8000)（本段链接地址在
# KERNBASE 段内，实跑位置在 0x8000，不能直接用链接期绝对地址）；分页开启
# 后跳往 _smp_tramp_fixup 用的是链接期线性地址（KERNBASE 平坦段）。
#
    .text
    .code32
    .globl _smp_tramp_start
    .globl _smp_tramp_pdata
_smp_tramp_start:
    .code16
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7c00, %sp               # 栈：0x7c00 向下，避开 BIOS 数据区与本页代码

    # lgdt：mini-GDT 就在本页（16 位模式下加 0x66 前缀读 6 字节形式）
    movl $(_smp_tramp_gdtr - _smp_tramp_start + 0x8000), %eax
    .byte 0x66
    lgdt (%eax)

    # 开 PE。CS 此刻的缓存是实模式值（base=0x8000、16 位默认尺寸），
    # 必须立刻远跳装载 GDT 描述符，否则后续 32 位指令全错
    movl %cr0, %eax
    orl $0x00000001, %eax
    movl %eax, %cr0

    .byte 0x66, 0xea                # ljmp $0x08, $prot32（16 位模式下的 32 位远转移）
    .long (_smp_tramp_prot32 - _smp_tramp_start + 0x8000)
    .word 0x08                      # 必须是代码段 0x08（首版误写 0x10 数据段 → #GP）

_smp_tramp_prot32:
    .code32
    movl $0x10, %eax
    movl %eax, %ds
    movl %eax, %es
    movl %eax, %ss
    movl %eax, %fs
    movl %eax, %gs

    # 页内参数块：pdata[0] = 页目录物理地址（BSP 经 PTD[767]&~0xFFF 填写）
    movl $(_smp_tramp_pdata - _smp_tramp_start + 0x8000), %esi
    movl (%esi), %eax
    movl %eax, %cr3

    # 开分页。线性 0x08000 已由 BSP 在共享页表重映射，本段代码继续可执行
    movl %cr0, %eax
    orl $0x80000000, %eax
    movl %eax, %cr0
    jmp 1f
1:
    # CS 仍是「实模式缓存」描述符（base=0x8000、64K limit）——远跳到 KCSEL
    # 平坦段后 EIP 才是绝对线性地址
    .byte 0xea                      # ljmp $0x08, $fixup（32 位模式远转移）
    .long _smp_tramp_fixup
    .word 0x08

_smp_tramp_fixup:
    movl $(_smp_tramp_pdata - _smp_tramp_start + 0x8000), %esi
    movl 4(%esi), %esp              # AP 内核栈顶（虚拟地址）
    movl 8(%esi), %eax              # ap_main（虚拟地址）
    call *%eax
2:  hlt
    jmp 2b

    .align 4
_smp_tramp_gdtr:
    .word 8*3 - 1
    .long _smp_tramp_gdt - _smp_tramp_start + 0x8000

_smp_tramp_gdt:
    .long 0, 0                      # 0x00: null
    .long 0x0000ffff, 0x00cf9a00    # 0x08: code, base 0 / limit 4G / 32 位 / ring0
                                    # （标准平铺段字节序 FF FF 00 00 00 9A CF 00；
                                    #  首版双字写反 → base=0xFF → 远跳后跑飞，串口
                                    #  路标 R-G-P 后复位实测定位）
    .long 0x0000ffff, 0x00cf9200    # 0x10: data, flat

_smp_tramp_pdata:                   # 页内参数块（BSP 填 3 dwords）
    .space 12

_smp_tramp_end:
