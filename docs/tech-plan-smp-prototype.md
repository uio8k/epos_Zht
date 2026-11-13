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

### 4.1 S03 实施设计（2026-11-14 开工前细化，依据 docs/evidence/2026-11-14/s03-lapic-timer-research-excerpt.txt）

S02 交付后的新事实（S03 手术的全部动机）：

| # | 现状 | 位置 | S03 对策 |
|---|---|---|---|
| D1 | HWINT 尾部读全局 `g_resched`——双核下 A 核 ISR 置位会被 B 核 HWINT 消费（错误核上调度） | `kernel/entry.S:358`（S02 树行号；S03 已由 RESCHED_AND_SCHEDULE per-CPU 直读替换） | g_resched 数组化 per-CPU（`g_resched_arr[NR_CPUS]` + kernel.h 宏按 lapic_id() 取下标），entry.S 的读改成 4 条 per-CPU 直读（复用 CURRENT_TO_EAX 的取核号模式） |
| D2 | schedule() 环走挑 READY，运行中任务 state 恒为 READY（无 RUNNING 态）——双核同时调度可选中同一任务**双跑**（kstack 互踩） | `kernel/task.c:37-72`（S02 树行号，schedule 已重写） | 引入 `TASK_STATE_RUNNING` 认领语义：持内核锁内把选中任务置 RUNNING（另一核挑选条件 `state==TASK_STATE_READY` 天然跳过）；切出方降级（RUNNING→READY）在 **switch_to 保存现场之后**（实现期复查发现：若在 schedule 锁内提前降级，另一核可在保存完成前认领并装载过期帧——认领协议后半步见 switch_to 注释）；WAITING/ZOMBIE 切出方不动 |
| D3 | 任务无 CPU 亲和字段——C1（用户任务钉 CPU0）无法表达；AP 也无自己的 idle 任务（cur 阻塞后无处可切，task0 属于 CPU0 不能借） | `kernel/task.c:190-229`（S02 树行号，sys_task_create 已加 cpu_binded） | tcb 加 `int cpu_binded`（-1=任意核；用户任务 tos!=0 建时钉 0；`sys_task_create_cpu(cpu,...)` 显式绑核，供 AP idle 任务与 S03 demo 线程使用）；schedule 挑选条件加 `task_allowed_on()`；fallback idle 按 CPU0→task0 / AP→g_ap_idle[cpu] 分派 |
| D4 | switch_to 的保存半段对 `old->kstack` 取址——AP 落地初期 current==NULL 直接写空指针页 | `kernel/machdep.c:122-152`（S02 树行号，switch_to 已加 NULL 守卫与降级后半步） | 保存半段包进 `if (old != NULL)`（ap_main 认领首个任务时现场直接弃置，与 INIT_TASK_CONTEXT 的 fresh-task 启动帧衔接） |
| D5 | AP 无节拍：LAPIC timer 未编程，AP 上任务的 timeslice 永不扣减，也就无时间片抢占 | `kernel/smp.c ap_main`（S02 树，S03 已加装表+timer+认领循环） | ap_main 里 `sti` + TDCR=0x3（÷16→count_shift=4）+ LVT timer=vector 0x40 \| PERIODIC（不 masked）+ TICR=625000（10^7ns>>4 → 10ms/tick，U6 定标，实测比对墙钟验证）；ISR 写 EOI（0xB0） |
| D6 | 双核各 100Hz 会把全局墙钟 g_timer_ticks 推成 200Hz（gettimeofday/sys_sleep/音频计时全翻倍失真） | `kernel/timer.c:38`（S02 树行号） | 墙钟只由 CPU0 推进（`if (cpu == 0) g_timer_ticks++`）；每核另有 `g_cpu_ticks[NR_CPUS]` 独立计数（调度用 timeslice 本就按核扣减，无需全局量）。-smp 1 下 cpu 恒 0，行为与基线逐字节相同 |
| D7 | 验收需要「AP 内核线程被时间片抢占」的串口证据 | — | smp_init（-smp 2 时）spawn 2 个绑死 CPU1 的 demo 线程：busy-wait 不让出，打印 `smpdemo task#T cpu=C tick=N iter=I`——同核两 tid 交错 = 抢占证据，tick 计数独立递增 = 每核节拍证据。S04 会把验收用例收进 selftest，届时移除本 demo |

向量与 IDT：LAPIC timer 取 vector 0x40（PIC 占 0x20-0x2F、syscall 0x82、NR_IDT=131 免扩）[实测: kernel/machdep.c:323,388-405]；新 asm 存根 `hwint_lapic_timer`（HWINT 同构：fake frame→pushal→段重装→PROLOGUE→调 `isr_lapic_timer`→per-CPU resched 检查→schedule→ret_from_syscall）+ `setidt(0x40, ...)`。

BSP 保持 PIC timer（C4：LINTIN0=ExtInt，PIC 只达 CPU0）；BSP 的 LAPIC LVT timer 不编程（TICR 恒 0，QEMU reset 时 LVT masked）——BSP 节拍语义零变化。

验收判据（S03）：① `make test` 37 用例绿；② -smp 1 `make full` 五腿与基线一致（行为零变化）；③ -smp 2 串口：每核 tick 计数独立递增 + 同核两 demo tid 交错打印（时间片抢占）；④ SORT_DEMO 在 -smp 2 下仍成功（demo 绑 CPU1，不抢 CPU0）。

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
| U9 | **（S01 实施期新增，已解）LAPIC MMIO 页不在页表**：分页开启后 VA 0xFEE00000 未映射，第一次时钟中断的 PROLOGUE 直读即 page fault（首跑 CAPTURE 腿串口 0 字节，证据 make-full-s01-red-boot-hang.log）。解法：init_paging 在开分页前恒等映射 LAPIC 页（PD 槽 0x3FB——首版误写 0x3FA——PT 槽 0x200，V+W）。S02 AP 启动复用同映射 | 已实测修复，回归五腿验证 |
| U10 | **（S02 实施期新增，已解）[0,1MB) 低地址映射被 cstart 主动取消**：[实测: machdep.c cstart 尾部] `for(i=0;i<0x100000;i+=PAGE_SIZE) if(i!=0xB8000) *vtopte(i+KERNBASE)=0;` + PD[0..19] 清零——S02 初版「低地址恒等映射」假设整个不成立。连锁坑（串口路标/页表转储实测定位）：①MP 表扫描需在 KERNBASE 别名临时映射（用完还原）；②trampoline 页拷贝目标走 KERNBASE 别名、源直接用链接期虚拟地址；③**PD[0] 是 vm86 时代的「伪页表」**（cstart 清零后 vm86 区域的 PTE 写落在 PD[0] 指向的帧里，转储实测 0x6067=帧 0x6000 兼任 PT0）——**绝不能重建**：首版重建 PD[0] 顶掉运行期累积的 v86 映射，五腿 WAV(VBE)/CAPTURE 二阶段挂死（bisect 二分实证：仅 reserve 即复现 3/5）；④AP 开分页后从线性 0x8000 取指 → 0x8000 的低别名 PTE 必须写进现存伪页表 | 最终解：**smp_init 挪到 vm86_init/selftest 之后**（届时伪页表就位）+ smp_boot_aps 里只往伪页表 entry 2 加一个 trampoline PTE——[实测: s02-final-smp2-sortdemo.log] -smp 2 双核在线下 VBE 成功设模式，五腿 5/5 |
| U11 | **（S02 实施期新增，已解）trampoline 自身三坑**：mini-GDT 描述符双字字节序写反（0xffff0000 应为 0x0000ffff，base 变 0xFF）；16 位远跳 CS 误写 0x10（数据段）应为 0x08（代码段）；串口路标（outb 0x3F8 单字节）是定位 AP 死点的有效手段（R-G-P-3-C-X-F 七点全部走通后移除） | 已实测修复，-smp 2 上线验证 |
| U12 | **（S03 实施期新增，已解）schedule 环走死循环**：重写时循环体误写成 `select = cur->next`（每圈都取 cur 的 next），应为 `select = select->next` 沿链推进——task0 被 tid==0 跳过后永远回到 cur->next，select==cur 的退出条件永不满足，首次「线程退出→fallback 切 task0」即死循环（五腿 0/5，L4 串口挂 `[2/4] semaphore`）。定位手段：串口单字节路标（e-f-1-D-G-T-x-1，`2` 后无输出）+ QMP pmemsave 转储走页表解码任务链表——链表完好、cur=ZOMBIE、锁被 cur 递归持有，锁定环走不推进 | 已实测修复，五腿复跑验证 |
| U13 | **（S03 实施期新增，已解）AP 系统表缺失**：trampoline 只带 mini-GDT，IDT/TR 保持复位值——S02 里 AP 从不收中断故从未暴露；S03 装 LAPIC timer 后第一响 v=40 即打在 base=0 的 IDT 上（[实测: docs/evidence/2026-11-14/s03-smp2-crash-int.log] event 81：GDT=mini 0x806e、IDT=0/0xffff、垃圾门 → #GP e=0x202 → #DF 复位循环）。解法：ap_main 先调 ap_load_systables()（装载与 BSP 共用的 GDT/IDT；TR 不装——ltr 要求 available 描述符而公共 TSS 已被 BSP 置 busy，且 C1 下 AP 只跑内核态线程不经 TSS 换栈）。另连带修「认领启动窗口」：ap_main 是唯一以 IF=1 调 schedule 的上下文，switch_to→iret 消费启动帧之前中断插入会被 PROLOGUE 撕坏帧——ap_main 循环 cli/sti 包住（BSP 全部调用方天然 IF=0） | 已实测修复，-smp 2 冒烟复跑（无复位、demo 交替、SONG 流正常） |

**S01 实施期设计增补**（不改变路线）：① init_paging 新增 LAPIC 页恒等映射（U9，首跑把 PD 槽写成 0x3FA、实为 0x3FB，二轮修正）；② switch_to 的 asm 操作数从 "m"(g_task_running) 改为显式 old->kstack / new->kstack 字段地址（g_task_running 数组化后不能作 asm 存储器操作数），存取同一字段语义不变；③ run_as_task0 的 "m" 操作数改指 _g_task_running_arr——"m" 装载的即 arr[0] 值（task0 指针），单次解引用到 kstack 与原版一致（首版误加一次解引用致复位循环，冒烟抓到）。

## 7 已知局限（原型接受，记录在案）

- TLB shootdown 不做（C3）；单页表负翻译过期窗口只存在于启动期映射后。
- 全局大锁串行化内核临界区（C2）——原型优先正确性。
- 用户任务单线程钉死 CPU0（C1）；vm86/驱动并发暴露面为零。
- 无负载均衡：全局就绪队列天然随机分布，不做迁移优化。
