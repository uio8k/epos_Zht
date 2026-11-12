# CODEBUDDY.md This file provides guidance to CodeBuddy when working with code in this repository.

本工作区是 EPOS 教学操作系统（32 位 x86 freestanding 内核 + 用户程序）及其便携运行环境。源码仓库在子目录 `epos_Zht/`（git 仓库根），工作区根目录另有 `msys/`（i686 工具链）、`Qemu/`（qemu-system-i386w 3.1.50）、`setvars.bat`（配 PATH）——**不需要安装任何东西，环境是随工作区捆绑的**。

## 常用命令

所有构建命令在 `epos_Zht/`（仓库根）下执行，且 make 依赖 MSYS bash（`SHELL=/bin/bash`）：

```bash
cd epos_Zht
make          # -O2 编译内核与用户程序
make hd.img   # 构建 + 制作硬盘镜像（首次从 hd.img.bz2.txt 还原，再 mtools 写入内核/用户程序）
make qemu     # 构建 + 镜像 + 启动 QEMU（= make run）
make debug    # -O0 调试构建 + 启动 QEMU(-S -s) 与 GDB（= make qemudbg）
make clean    # 清理产物与 hd.img
```

- 单独构建某部分：`make -C kernel`、`make -C userapp`（顶层 Makefile 通过 `MODE=run|debug` 传参控制 -O2/-O0）。
- PowerShell/新 shell 里构建前需先 `setvars.bat`（开一个配好 PATH 的 cmd），或手动 `$env:PATH = "…\msys\2.0\usr\bin;…\Qemu;" + $env:PATH`；bash 里 MSYS `/usr/bin` 已在 PATH，补 QEMU 即可。
- 调试：`make debug` 后 GDB 连 `localhost:1234`，仓库根的 `.gdbinit` 会把 `kernel/eposkrnl.elf` 符号按 0x100000（物理）与 0xc0100000（虚拟）各加载一份。
- 没有单元测试框架。「音频评测」是事实上的测试：`make TEST_MODE=1 TEST_SIGNAL=sine|impulse|step` 构建测试信号版用户程序，再 `uv run --no-project --with numpy python tools/eval_audio.py`（构建→无头运行→SNR/THD/爆音分析→报告，分数低则退出码非零）；单独分析 wav 用 `uv run --no-project --with numpy python tools/analyze_wav.py <file.wav>`。
- **注意**：`tools/` 评测脚本与源码树的匹配状态有已知问题（路径硬编码为 `epos`、依赖 run_headless.bat 与 monitor 端口 4444），详见 `epos_Zht/docs/运行环境.md` 文末；`analyze_wav.py` 是唯一可直接使用的。
- `make diff`/`make submit` 面向 svn 时代，本仓库已改用 git，勿用。

### git 远程

`origin` = GitHub 上游（uio8k/epos_Zht，分支 `x86`）。**2026-10-25 实测：Gitee 远程已不存在**（`git remote -v` 只剩 origin），如需恢复开发线再 `git remote add Gitee https://gitee.com/zhou-huaitao/os.git`。分支：`x86`（工作线，跟随 origin/x86）、`x86_SMP`（SMP 支线）。**建分支/push 新分支前必须先经 Master 确认**；切已有分支、已有分支上 commit/pull 不受限。引用提交号前先 `git log` 核对（并行会话会 rebase 改号）。

**产物已由 `.gitignore` 排除**（`*.o`、`kernel/eposkrnl.*`、`userapp/a.out`、`tools/**/*.exe`、`samples/song*.wav` 等），`git ls-files` 中构建产物计数为 0，重建后工作树保持干净，不再需要 `git clean -fd`。git author 已在 `~/.gitconfig` 配好（`ZHT <1477255795@qq.com>`）。

> 上述两条为 2026-10-25 实测更正，推翻了早期「双远程（Gitee master 开发线 + origin/x86 上游）」「产物提交进了版本库、pull 前需 git clean」的描述。

## 架构

### 启动链路（multiboot → 内核 → 用户程序）

GRUB 按 `kernel/entry.S` 的 multiboot 头把内核加载到物理 1MB，`_entry` 在未开分页的物理地址下 `call _cstart`（`kernel/machdep.c`）→ `md_startup`：`init_paging` 建恒等+KERNBASE 双映射页表并开 PG，装 GDT/IDT/8259/8253，然后跳到高地址执行并撤掉恒等映射 → `mi_startup()`（`kernel/startup.c`）：初始化物理帧、内核堆（TLSF）、task0，切到 task0 内核栈 → `start_user_task()`：顺序初始化各设备（ide、pci、vm86、e1000、hda…），用 dosfs 挂 FAT 分区（块设备桥 `DFS_ReadSector/WriteSector` 转发到 ide/floppy），`load_aout` 把用户程序按目标格式读入用户地址空间（Windows 构建用 `kernel/pe.c`，Unix 用 `kernel/elf.c`，编译期二选一），最后 `sys_task_create` 用伪造的 iret 帧创建首个任务，调度时即运行 crt0 → `main`。用户程序段页由首次访问触发的 `do_page_fault` 按需补帧。

### 地址空间与内存三层

- `kernel/kernel.ld.in`：VMA = KERNBASE(0xC0000000)+1MB，各段 `AT(ADDR-KERNBASE)`（LMA=物理 1MB 起）。Windows 下链接成 **PE**（`pei-i386`），由 Makefile `objcopy` 成裸 `eposkrnl.bin` 写入镜像；Unix 下是 ELF，直接写 `.elf`。`kernel.ld` 由 cpp 从 `.in` 生成。
- 页表由 `init_paging`（machdep.c）静态建立，页目录自映射在 pgdir[767]：`0xBFC00000` 起是 PTE 窗口（`PTE(va) = PT+(va>>12)`），页目录本体映射在 `0xBFEFF000`。读懂 `page.c`/`machdep.c` 必须先懂这个自映射——写 PTE 即顺带安装页目录项。用户/内核边界：`USER_MAX_ADDR=0xBFC00000`、`USER_MIN_ADDR=0x400000`。
- 内存三层职责：`frame.c`（物理帧，RAM zone bitmap）→ `page.c`（虚拟区间 vmzone + PTE 增删）→ `kmalloc.c`（内核堆，TLSF，套在 startup 预留的 12MiB 上——扩到 12M 是为了缓存 HDA 的 WAV）。用户堆：`main` 里 `mmap` 32MB + 同一份 `lib/tlsf`。

### 任务与调度

单一地址空间的内核线程模型：无 per-task CR3，`switch_to` 只换内核栈指针（TCB 即 4KB 内核栈，栈顶硬编码在偏移 0，供 `entry.S` 的 PROLOGUE/EPILOGUE 直接切换），FPU 懒切换靠 CR0.TS。抢占+协作混合：PIT 100Hz，时间片默认 4 tick（40ms），HWINT 桩在 EOI 后按 `g_resched` 时机性调度，RR 轮转，task0 兜底；协作点为 `sys_task_yield` 与 `sleep_on/wake_up` 等待队列（键盘、task_wait 用到）。`sys_sleep/nanosleep` 是忙等+yield。**`kernel/sem.c` 全是返回 -1 的 stub（教学留白）**；`callout.c` 参考代码未列入编译。

### 系统调用

用户侧 `userapp/lib/syscall-wrapper.S` 生成 `movl $编号,%eax; int $0x82; ret` 的桩；编号在 `include/syscall-nr.h`（**内核与用户程序共享同一份头**，两端一致靠它保证）。内核侧 IDT 把 0x82 装成 DPL=3 的中断门（`machdep.c`），`entry.S` 保存上下文后调 `syscall()`（`machdep.c`）：**参数从用户栈 `ctx->esp+4/+8/...` 直读，返回值写 `ctx->eax`**，用户指针一律 `IN_USER_VM` 校验。`sys_*` 前缀的函数是各 syscall 的内核实现层（`sys_task_*`、`sys_audio_*`…）。

### 中断与驱动

IDT/GDT/8259 全在 `machdep.c`。唯一的驱动抽象是一张函数指针表 `g_intr_vector[NR_IRQ]`（`startup.c`）+ `enable_irq/disable_irq`。各驱动注册方式各异：timer/kbd 由 `mi_startup` 装，floppy/e1000 自装，**ide 是纯 PIO 轮询**（不注册 ISR），**HDA 读了 IRQ line 但故意不注册 ISR**，改用 LPIB/SDSTS 轮询（见下）。没有统一设备模型。

### HDA 音频驱动（课程扩展，三个文件三层）

- `kernel/hda.c` 硬件层：PCI 发现 + MMIO 映射，控制器复位，verb 走 **Immediate Command（ICW/IRR）而非 CORB/RIRB**——绕开 QEMU 3.1 的 CORB DMA bug；codec 拓扑按 QEMU 的 hda-duplex **硬编码**（DAC 0x02 / Pin 0x07 / Mixer 0x05）；DMA 缓冲必须分配在 >1MB 的物理地址，写后 `wbinvd` 刷缓存；音量 = Q8 软件增益 + codec verb。
- `kernel/hda_stream.c` 流层：「连续流」引擎——512KB 环形缓冲 + BDL，流 RUN 后不停；写指针对齐 LPIB，写入后**阻塞到本音符播完**（这是排序动画节拍同步的机制）；错误/位置全靠轮询。
- `kernel/hda_audio.c` 播放层：dosfs 解析 WAV 并缓存进内核堆；多路混音（旋律×2+伴奏×1）用 **Q8 定点数**（内核路径禁浮点）；淡出同样定点实现。
- 对用户的 API 是 8 个 syscall（`SYSCALL_audio_*` 189–196，内核实现在 `machdep.c` 的 syscall 分发表里）；`userapp/melody_player.c` 按乐谱逐音符调用播放，由排序 swap 触发。

### 用户程序（userapp/）

单个静态链接程序 `a.out`：VGA 256 色希尔排序柱状图 + 每次 swap 播一个钢琴音符。VBE 模式切换靠 **vm86 BIOS int 0x10**（`graphics.c` + `vm86call.c` 的 HLT 蹦床），帧缓冲用 `mmap(fd=0x8000)` 映射物理显存（内核 mmap 的 /dev/mem 特例）。`printf` → 共享 `lib/snprintf` → `putchar` syscall → 内核写 VGA 文本 + **COM1**（串口输出即程序日志，无头评测靠它）。`TEST_MODE` 是音频质量评估模式：编译期生成 sine/impulse/step 参考信号走同一条 HDA 链路，供 `tools/eval_audio.py` 分析。

### 全局约定（写代码前必读）

- **freestanding**：内核与用户程序都 `-nostdinc -ffreestanding -nostdlib -m32 -march=i386`，没有 libgcc——`lib/softfloat.c` 名不副实，实为 `__udivmoddi4/__divdi3` 等 64 位除法软例程，`snprintf` 的 `%llu` 和 HDA 的 uint64 运算都依赖它。内核路径刻意不用 FPU（任务级懒保存存在，但混音/淡出用定点数）。
- **`-fleading-underscore`** 是 C 与手写汇编的符号契约：C 符号带前导 `_`，所以 `entry.S` 引用 `_cstart/_syscall/_schedule`，`syscall-wrapper.S` 生成 `_task_create` 等名字。新增汇编↔C 边界时必须遵守。
- 命名模式：`g_*` 全局、`sys_*` syscall 实现、`isr_*` 中断处理、`init_*` 初始化、`page_*/frame_*` 内存、`hda_*` 音频驱动、`tlsf_*` 分配器。
- 启动分层：`cstart → md_startup → mi_startup → start_user_task`，machdep 是平台层，startup 是设备/流程编排层。
- `include/` 是内核与用户程序共享的头文件层（`syscall-nr.h`、`time.h` 等）；`userapp/include/` 才是用户侧私有头。

详细环境文档（安装、工具链版本、Unix 分支）见 `epos_Zht/docs/运行环境.md`。
