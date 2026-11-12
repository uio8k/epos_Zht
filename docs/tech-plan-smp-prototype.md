# tech-plan：SMP 最简原型（x86_SMP 分支，S01–S04）

日期：2026-11-13。前置：用户在对话中审核通过设计稿（裁剪决策 C1–C4），调研取证已完成（`docs/evidence/2026-11-13/qemu-smp-research-excerpt.txt` 汇总 + `qemu-src/` 原文 + `mps-parse-smp2.txt`）。编码由用户手动触发 /autodev 后进行（D-A16），一轮一个 Sxx 项（D-A3/D-A14）。

## 1 目标与现状

目标：QEMU `-smp 2` 下两核同时跑内核线程；用户任务钉死 CPU0；音频功能零回归。不追求负载均衡/细粒度锁/进程级 SMP。

现状单核假设五要害（全部 [实测: 文件:行号]，本次直读源码核实）：

| # | 假设 | 位置 |
|---|---|---|
| F1 | 并发控制 = 全局 cli（save_flags_cli/restore_flags 宏） | `kernel/cpu.h:103` `kernel/cpu.h:112` |
| F2 | g_task_running 全局，被中断入口汇编直读 | `kernel/task.c:26`；`kernel/entry.S:195`、`kernel/entry.S:221` |
| F3 | 上下文切换 popfl 恢复每线程 EFLAGS（睡眠可带关中断切走） | `kernel/machdep.c:121`（switch_to 尾部 popfl） |
| F4 | 单 GDT + 单公共 TSS，esp0 = 一块 8KB 静态栈 | `kernel/machdep.c:282`；`kernel/entry.S:473` |
| F5 | 单一共享地址空间（一个 pgdir，PT/PTD 全局） | `kernel/startup.c:30` `kernel/startup.c:31` |

中断全走 8259 PIC、无任何 APIC/SIPI 代码（全仓 grep 零命中）；QEMU 命令行无 -smp（`Makefile:67`）。

## 2 调研结论（U1–U5）

| # | 结论 | 等级与出处 |
|---|---|---|
| U1 | QEMU 3.1.50 `-smp 2` 生成 MPS 表：`_MP_`@0xF58A0 → PCMP@0xF58B0，20 条目=2 processor+2 bus+1 IOAPIC+13 IOINT+2 LINT；**非 processor 条目一律 8 字节，header 的 base_len=228 不可信（SeaBIOS 多计尾巴），解析以 entry_count 为循环边界** | [实测: mps-parse-smp2.txt] |
| U4 | APIC ID = cpu_index（compat 模式默认关）；实测两核 ID 恰为 0/1，mycpu() 可直读 LAPIC ID 当下标 | [src: qemu-src/pc-v3.1.0.c:691-702] + [实测: mps-parse-smp2.txt] |
| U2 | LAPIC timer：tick = 虚拟时钟纳秒 >> count_shift（TDCR 分频）；写 TICR 即装填；MMIO 偏移 ID=0x020/EOI=0x0B0/SVR=0x0F0/ICR=0x300,0x310/TICR=0x380/TDCR=0x3E0；SVR 写值 &0x1ff bit8=enable；IPI 写 ICR低触发 apic_deliver，dest shorthand bit18-19=01=全含自 | [src: qemu-src/apic-v3.1.0.c:622-651,840-851,820-823] + write handler case 表 |
| U3 | 低 1MB 在帧分配器管理内（init_ram 原样收 AVAILABLE 区含 0x0-0x9FC00）→ trampoline 页（选 0x8000）必须 `frame_alloc_in_addr` 显式预留 | [实测: kernel/frame.c:29；kernel/machdep.c:1150] |
| U5 | 惰性 FPU 跨核安全性靠 C1（用户任务钉死 CPU0）+ S04 断言验证 | [实测: kernel/machdep.c:547-558]（owner 全局，大锁内） |

关键旁证：MPS local INT 条目 LINTIN0=ExtInt——PIC 仅路由到 BSP 的 LINTIN0，**AP 必须自带 LAPIC timer**，证实 C4 的必要性 [实测: mps-parse-smp2.txt]。

## 3 关键决策（用户已审核）

| # | 决策 | 弃选项 |
|---|---|---|
| C1 | **用户任务钉死 CPU0**：调度器把内核态线程放行到任意核、用户态线程（tos!=NULL 创建的任务）只在 CPU0 上调度。换来 F4 单 TSS/esp0 不改、vm86/HDA/dosfs/e1000 全部免改 | 用户任务全核跑（需 per-CPU GDT/TSS + 驱动全面加锁，规模×3） |
| C2 | **全局大锁 g_kernel_lock**：全部 save_flags_cli 共享区挂这把锁，spinlock-irqsave 模式。实现细化（S01 实施期，2026-11-13）：①递归 owner+depth 语义——调用图实测发现 page.c 锁区内调 kmalloc，且 page_map/page_alloc 存在内部互相调用，非递归锁会自死锁；②kmalloc 单独配叶子锁 g_kmalloc_lock（两锁无反向依赖无环，见 docs/evidence/2026-11-13/s01-lock-research-excerpt.txt §4） | 细粒度锁（每子系统一把，设计负担大） |
| C3 | **不做 TLB shootdown**，记为已知局限：单地址空间（F5）+ 映射只在启动期建立（CPU0），原型不做跨核动态映射 | 映射 IPI 广播 flush（原型不需要） |
| C4 | **PIC timer 只到 CPU0（LINTIN0=ExtInt 实证），AP 用 LAPIC timer**（periodic，独立向量，每核独立节拍） | AP 复用 PIC（不可能——LINTIN0 不通） |
| C5 | **mycpu() = LAPIC ID 直读**（`*(volatile uint32_t*)0xFEE00020 >> 24`），entry.S 的 PROLOGUE/EPILOGUE 用 4 条指令替换 `movl _g_task_running`；不动 fs/ds/es/gs 段语义，vm86 汇编零触碰 | %gs per-CPU 段（要动 vm86 的 gs 透传，险） |
| C6 | **睡眠模式修正**：持锁睡眠必须先放锁再 schedule 再重查（否则锁被睡眠线程带走过期，F3 的 popfl 机制会把锁状态冻结在被切出的核上）。五个睡眠点逐个手术：`kernel/sem.c:85`、`kernel/task.c:256`、`kernel/kbd.c:267`、`kernel/hda_stream.c:301`、`kernel/e1000.c:316` | — |

## 4 分期与落点

| 项 | 内容 | 主要落点 | 验收判据 |
|---|---|---|---|
| S01 | spinlock 原语 + g_kernel_lock + save_flags_cli 共享区换锁 + 五个睡眠点 C6 手术 + current per-CPU 化（entry.S 手术，C5）+ 自检 | 新增 `kernel/spinlock.h`；改 `cpu.h`/`task.c`/`sem.c`/`kmalloc.c`/`page.c`/`frame.c`/`timer.c`/`entry.S`/`machdep.c` | `make test` 37 用例绿；`-smp 1` 下 `make full` 五腿与基线一致（行为零变化）；新增 NR_CPUS=1 单核 spinlock 自检 |
| S02 | MP 表解析 + 每核结构 + trampoline + INIT-SIPI-SIPI + ap_main idle | 新增 `kernel/smp.c`/`smp.h`/`smptramp.S`；改 `machdep.c`（ap_main 启动）、`frame.c`（预留页） | `-smp 2` 串口打印两核 APIC ID 各自 idle；BSP 主流程不回归 |
| S03 | LAPIC timer 每核节拍 + schedule per-CPU 化收尾 + g_resched 简化（HWINT 直接调 schedule） | 改 `machdep.c`（init_apic/lapic timer ISR）、`entry.S`（HWINT）、`timer.c`、`task.c` | `-smp 2` 每核 ticks 独立递增；AP 内核线程被时间片抢占（串口证据） |
| S04 | 端到端：`-smp 2` 进 Makefile；selftest 加 3 个 SMP 用例（双核并行计数/锁互斥计数/跨核信号量唤醒）；全量回归 | 改 `Makefile:67` `Makefile:84`、`kernel/selftest.c` | selftest SMP 用例 PASS；五腿音频 verdict 与基线一致；U5 FPU 断言 PASS |

依赖链 S01→S02→S03→S04（progress.html data-dep）。

## 5 相容核对

- D-A16：本 tech-plan 为前置设计，编码待 /autodev 触发；触发只授权当轮报备的 Sxx。
- D-A14：本文件与 SMP evidence 属 x86_SMP 管辖族；不读不改音频族 tech-plan。
- D8（不动 QEMU 3.1.50）：本方案只改 QEMU 命令行参数（-smp 2），不换不改 QEMU。相容。
- D9（不改宿主音频设备）/ D10（入口统一 make）/ D11（指标唯一源）：SMP 轮新增测试入口统一走 make 目标与 selftest，音频指标仍出自 audio_metrics 链路。相容。
- D-A13/D-D3：轮内自动提交；D-A17：交差按固定格式。
- 检索摘要纪律：本计划能力类结论全部过 [src:]/[实测:]（U1–U4），无 [检索:] 孤证。

## 6 不确定项（单列，每条带验证方法）

| # | 疑点 | 验证方法 |
|---|---|---|
| U5 | 惰性 FPU 切换跨核是否真不触发（C1 下理论安全） | S04 在 AP 内核线程用一条 x87 指令 + 断言 frstor 状态自洽 |
| U6 | LAPIC timer 定标（TICR 装填多少得 HZ=100） | S03 实测：写 TDCR=分频、TICR=T，读 ticks 与墙钟比对 |
| U7 | SIPI 时序（INIT 后延时窗口）在 QEMU TCG 下的容忍度 | S02 实测；失败按 [src: apic.c ICR] 换 shorthand/重试路径 |
| U8 | entry.S PROLOGUE 手术后 22 字深度拷贝边界（vm86 路径） | S01 回归五腿 + RESTART 腿（vm86 在启动路径被调用） |
| U9 | **（S01 实施期新增，已解）LAPIC MMIO 页不在页表**：分页开启后 VA 0xFEE00000 未映射，第一次时钟中断的 PROLOGUE 直读即 page fault（首跑 CAPTURE 腿串口 0 字节，证据 make-full-s01-red-boot-hang.log）。解法：init_paging 在开分页前恒等映射 LAPIC 页（PD 槽 0x3FA / PT 槽 0x200，V+W）。S02 AP 启动复用同映射 | 已实测修复，回归五腿验证 |

**S01 实施期设计增补**（不改变路线）：① init_paging 新增 LAPIC 页恒等映射（U9，首跑把 PD 槽写成 0x3FA、实为 0x3FB，二轮修正）；② switch_to 的 asm 操作数从 "m"(g_task_running) 改为显式 old->kstack / new->kstack 字段地址（g_task_running 数组化后不能作 asm 存储器操作数），存取同一字段语义不变；③ run_as_task0 的 "m" 操作数改指 _g_task_running_arr——"m" 装载的即 arr[0] 值（task0 指针），单次解引用到 kstack 与原版一致（首版误加一次解引用致复位循环，冒烟抓到）。

## 7 已知局限（原型接受，记录在案）

- TLB shootdown 不做（C3）；单页表负翻译过期窗口只存在于启动期映射后。
- 全局大锁串行化内核临界区（C2）——原型优先正确性。
- 用户任务单线程钉死 CPU0（C1）；vm86/驱动并发暴露面为零。
- 无负载均衡：全局就绪队列天然随机分布，不做迁移优化。
