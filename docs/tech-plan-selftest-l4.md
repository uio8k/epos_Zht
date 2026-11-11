# P11 技术方案：L4 QEMU 整机自检（SELFTEST 段）

日期：2026-11-11。范围：`kernel/` 新增自检段 + `kernel/sem.c` 补上信号量实现 + `tests/full_run.py` 增 L4 腿。
依据等级标记见每段；不确定项单列 §7。上游决策：`docs/decisions-audio-test.md` D2（预算）/D8（不动 QEMU 3.1.50）/D9（不碰宿主声卡）/D10（入口统一 make）/D11（指标唯一源）；`docs/decisions-qemu-only.md` D-Q4（P11 改 QEMU 版保留，SELFTEST 段并入 make full）。

## 1 需求与现状

`progress.html` 的 P11：**L4 QEMU 整机自检：QEMU 冷启动打印 `SELFTEST` 段并并入 `make full` 链路，全 PASS**。L4 层定义在 `docs/decisions-audio-test.md:93`：依据 OSDev `Unit_Testing` 的 Run-Time Self-Tests——分配器往返、信号量握手、tick 前进、先打印测试名。

现状（全部实查）：

| 项 | 现状 | 出处 |
|---|---|---|
| 自检段 | 内核里**没有任何**自检代码：`kernel/` 全域搜 `SELFTEST`/`selftest`/`self_test` 命中 0 | [实测: 2026-11-11 全仓 Select-String，0 命中] |
| `SELFTEST` 字样 | 只出现在文档与页面里，不在代码里 | [实测: docs/decisions-qemu-only.md:12]、[实测: docs/tech-plan-full-none.md:19]、[实测: progress.html:326] |
| 信号量 | `sys_sem_create/destroy/wait/signal` 四个函数体都只有 `return -1`；`callout.c` 调用的 `sem_create/sem_wait/sem_signal` 全仓**无定义**，且 `callout.c` 不在编译列表（`COBJS` 无 callout） | [实测: kernel/sem.c:7-25]、[实测: kernel/callout.c:158-159]、[实测: kernel/Makefile:29-33] |
| tick 计数 | `g_timer_ticks` 由 `isr_timer` 自增，HZ=100；`calibrate_delay()` 已靠它工作，说明启动到该点时定时器中断已在跑 | [实测: kernel/timer.c:25]、[实测: kernel/timer.c:32]、[实测: kernel/kernel.h:38]、[实测: kernel/timer.c:74-95] |
| tick 精度取舍 | `g_timer_ticks` 是 `unsigned volatile`，32 位，1 秒 100 跳 | [实测: kernel/timer.c:25]、[实测: kernel/timer.c:32] |
| HDA 探测 | `hda_init()` 成功返回 0、失败 -1；探测结果落在全局 `struct hda_dev g_hda`（`mmio_base`/`codec_cad`/`output_dac`/`output_pin`/`sd_out`） | [实测: kernel/hda.c:109]、[实测: kernel/hda.c:310]、[实测: kernel/hda.h:178-211]、[实测: kernel/hda_internal.h:9] |
| 分配器 | `kmalloc/kfree/kmemalign` 由 TLSF 支撑，cli 保护 | [实测: kernel/kmalloc.c:25]、[实测: kernel/kmalloc.c:49]、[实测: kernel/kmalloc.c:58] |
| 任务与等待 | `sys_task_create(tos,func,pv)`、`sys_task_wait`、`sleep_on(&wq)`/`wake_up(&wq,n)` 均已存在，且 `sys_task_wait` 就是「先 sleep_on 再被 wake_up」的现成范式 | [实测: kernel/kernel.h:161-165]、[实测: kernel/task.c:69-108]、[实测: kernel/task.c:237-272] |
| 启动序列 | `start_user_task()` 在 task0 里跑：先 `calibrate_delay()`，再 IDE/PCI/E1000/`hda_init()`，再 FAT、再加载 a.out | [实测: kernel/startup.c:94]、[实测: kernel/startup.c:99]、[实测: kernel/startup.c:116]、[实测: kernel/startup.c:154] |
| 中断使能 | task0 由 `run_as_task0()` 切入，上下文里的 eflags 是 `0x202`（IF=1） | [实测: kernel/machdep.h:120]、[实测: kernel/machdep.h:143-157] |
| 串口输出 | `sys_putchar` 对 `\n` 自动补 `\r` 再写 COM1（0x3F8），写前等 THR 空 | [实测: kernel/machdep.c:414]、[实测: kernel/machdep.c:479-485] |
| printk 行长 | 内部 `char buf[128]`，超过 128 字节时 `vsnprintf` 返回「应写长度」而循环按该值读缓冲——会越界打印垃圾。**故本轮所有自检行 < 100 字符** | [实测: kernel/printk.c:9]、[实测: kernel/printk.c:14-18] |
| L3 编排 | `full_run.py` 的 `LEGS` 是三腿表（PLAY/WAV/CAPTURE），每条腿=构建→起 QEMU→轮询串口→断言正则 | [实测: tests/full_run.py:64]、[实测: tests/full_run.py:177]、[实测: tests/full_run.py:270] |
| make 入口 | `make full: test qtest full-l3`；`full-l3` 调 `full_run.py` | [实测: Makefile:48]、[实测: Makefile:56] |

结论：P11 的缺口就是「内核没有自检段」+「信号量是空桩」两件，其余基建（任务/等待队列/tick/HDA 状态/串口/编排）全部现成。

## 2 规范依据（一手摘录）

摘录逐字落盘：`docs/evidence/2026-11-11/osdev-unit-testing-excerpt.txt`、`docs/evidence/2026-11-11/osdev-semaphore-excerpt.txt`（浏览器通道取正文；直连被 Cloudflare 403 拦、`r.jina.ai` 超时）。

- 四项自检的原始表述 [wiki: Unit_Testing / Alternative / Additional Option: Run-Time Self-Tests]：分配器「ask it for memory, record the address, free the memory and ask for memory again. Most allocators are expected to return the very same address」；信号量「launch a thread that is expected to signal a semaphore and then wait on that semaphore」；tick「poll the 'tick counter' and don't go on until it advanced」；以及「print what test you're running before you know if the test was successful or not」。
- 信号量语义与实现要点 [wiki: Semaphore / Implementation]：wait 在值为 0 时「should be removed from the scheduler queue, and only added to it again when the semaphore is signalled」；需「another more primitive form of locking」保证原子；并点名 **lost wake-up** 竞态。
- OSDev 同一段也提醒「As many of those tests may hang」——故本轮每个用例都要么有界自旋、要么由编排侧 90s 腿超时兜底，**不允许无界等待**。

## 3 备选矩阵

| # | 方案 | 取舍 |
|---|---|---|
| A | 自检写成内核 C 函数，在 `start_user_task()` 里调用，输出走 `printk`→COM1；编排侧加 L4 腿断言行 | **选它**。零新依赖、零新 syscall、与 L3 共用同一套串口轮询；自检在每次冷启动都跑，是「整机链路能每天自动跑的回归」 |
| B | 自检写成 userapp 里的模式（`make SELFTEST=1`） | 否。用户态够不到 `kmalloc`/`sleep_on`/`g_hda`，四项里有三项测不到；且要新增构建开关与标记文件范式 |
| C | 自检做成宿主侧脚本读串口日志 | 否。那不叫自检，是 L3 已有的断言形态；内核侧仍为空 |
| D | 只加 SELFTEST 打印、不做真实检查（恒 PASS） | 否。「恒 PASS 的测试等于没测」，违反 `prove-it` 口径 |
| E | 为信号量新增 syscall 号 + 用户态封装 | 本轮否。见 §6 不做项：需要在 `include/syscall-nr.h` 与 `userapp/include/syscall.h` 两侧同步，且 L4 在内核侧就能完成握手，无用户态消费者 |

## 4 实现落点

| 文件 | 改动 |
|---|---|
| `kernel/sem.c` | 由「四个 `return -1` 空桩」改为**计数信号量**实现：静态池 + 等待队列 + `save_flags_cli/restore_flags` 临界区。API 对齐 `callout.c` 既有调用形态 `void *sem_create(int)` / `sem_wait(void*)` / `sem_signal(void*)` / `int sem_destroy(void*)`；原 `sys_sem_*` 保留为同语义的 id 包装（不再 `return -1`） |
| `kernel/selftest.c` | **新增**：`int selftest_run(void)`，四项各「先打印名字 → 跑 → 打印 PASS/FAIL」，末尾一行 `SELFTEST: RESULT PASS n/4`。全部有界，不 abort 启动 |
| `kernel/kernel.h` | 加 `int sem_*` 与 `int selftest_run(void)` 声明 |
| `kernel/Makefile` | `COBJS` 加 `selftest.o`（保持字母序附近位置，最小 diff） |
| `kernel/startup.c` | 在 `hda_init();` 之后调用 `selftest_run();`（一行 + 注释） |
| `tests/full_run.py` | `LEGS` 增 `{"id":"L4","name":"SELFTEST","build":[]}`，断言 4 个单项 PASS 行 + 汇总行 |
| `progress.html` | P11 `data-done="1"`、L4 行状态、完成计数 11→12、changelog 顶部加当日条目、「更新于」日期 |
| `docs/evidence/2026-11-11/` | 两份 wiki 摘录（已落）、`p11-selftest-*.log` 实跑串口与 `make full` 原始输出、`refs-check-p11.log` |

不改：`userapp/**`、`include/**`、`tests/*.c`、`Makefile`（`full` 目标本身不用动，L4 由 `full-l3` 里的 `full_run.py` 覆盖）、QEMU 3.1.50、宿主音频。

## 5 关键设计决定

**D1 自检放在内核、放在 `hda_init()` 之后。** 四个用例里三个（分配器/信号量/tick）在用户态够不到，HDA 探测要读 `g_hda` 的探测结果，而 `hda_init()` 在 `kernel/startup.c:116` 才把结果填好 → 自检点定在 `:116` 之后、FAT 初始化之前。此刻 task0 正在跑、中断已开（eflags 0x202）、还没有用户任务，调度器只有「我和即将创建的自检线程」，握手最简单。

**D2 tick 用例用「有界自旋等计数前进」，不用 sleep。** `sys_sleep` 走 `nanosleep`→等待队列，在 task0 里可能没有别的可跑任务而被切到 idle（`hlt` 直到下次中断才回），虽有中断仍能前进，但路径更长更绕。有界自旋（上限 5×10⁷ 次读）最直白：读 `g_timer_ticks` 前后对比，HZ=100 下一次跳变 10ms，实测远小于上限；超限即 FAIL 并给出「中断没开 / 定时器没跳」的可读结论，不会挂死。

**D3 信号量用「静态池 + 等待队列」，不用动态分配。** `sem_create` 若走 `kmalloc`，就在「测分配器」的同一条链路上引入分配依赖，分配器坏了信号量也建不出来，两项会一起红、定位变糊。静态池 16 个，`used` 位标记占用，天然可重入、无失败路径。

**D4 握手线程用 `sys_task_create(0, func, pv)` 建内核线程。** `tos==0` 即内核栈线程（`startup.c` 里 task0 本身就是这么建的），不需要用户页表；线程里 `sem_signal` 后 `sys_task_exit(0)`，主线程拿到信号后 `sys_task_wait` 回收，避免 zombie 泄漏。

**D5 计数语义靠「全唤 + 各自重查」。** 实现取 `sem_signal` 递增后 `wake_up(&sem->wq, -1)` 唤醒全部等待者，每个被唤醒的线程在 `while (sem->value <= 0)` 里自行重查，只有一个能递减成功、其余继续睡。初稿曾打算「`wake_up(n)` 传棒」，实际改为全唤：等待者数量在这条链路上极小，全唤无漏唤醒风险，`wake_up` 本就支持 `n<0` 表示全唤（`kernel/task.c:102-108`）。lost wake-up 由「检查-睡眠全程 `save_flags_cli`」堵住，与 `sys_task_wait` 的既有写法一致。

**D6 输出行严格 < 100 字符**（`printk` 的 128 字节缓冲越界风险，见 §1）。每项分两行：先 `SELFTEST [i/4] <名字>`，跑完 `SELFTEST [i/4] <名字>: PASS|FAIL<细节>`。分开打印既落实「先打印测试名」，也让编排侧断言只匹配结果行、不受中间输出干扰（compression 干扰不影响：结果行是原子 `printk`）。

**D7 L4 作为第四条腿并入 `full_run.py`，不新建 make 目标。** `make full` 已经串起 `full-l3`，加腿即「并入链路」，也保住 D10「入口统一 make」。L4 用默认构建（不带 TEST_MODE/CAPTURE_TEST 等），断言命中即 kill QEMU，不必等整曲播放完。

**D8 自检永不 abort 启动。** 失败只打印 FAIL 并计入 `RESULT`，不 `return` 中断 `start_user_task`——否则自检一红就把 L3 三腿一起打死，排障信息全丢。红不红由编排侧断言决定。

## 6 与上游决策的相容核对

| 决策 | 本方案是否相容 |
|---|---|
| D2（单测 <2s、全套 <60s） | 相容。四项自检全在有界自旋内，实测总耗时远小于 1s；L4 腿只加一次冷启动 + 一次默认构建，`make full` 预计仍在 40s 内 |
| D8（不动 QEMU 3.1.50） | 相容。只读 `g_timer_ticks`/`g_hda`，不碰 QEMU 二进制 |
| D9（不碰宿主默认音频设备） | 相容。L4 腿沿用 `QEMU_AUDIO_DRV=none` |
| D10（入口统一 make） | 相容。仍是 `make full` |
| D11（指标唯一源 audio_metrics.py） | 相容。自检只做行为断言（PASS/FAIL），不产任何指标数值 |
| D-Q4（P11 改 QEMU 版保留） | 正是本次实现对象 |
| `AGENTS.md` 动代码前调研 | 已做：两份 osdev 摘录逐字落盘，含 URL/oldid/最后编辑时间 |

**本轮明确不做**：
- 不新增信号量 syscall 号，不加用户态封装（无消费者；会牵 `include/syscall-nr.h` 与 `userapp/include/syscall.h` 两侧一致性）。
- 不把自检接进 `make test`（L1 是宿主侧代码，跑不了内核自检）；L4 归 L3 那条整机链路。
- 不复活 `callout.c`（它依赖的信号量本轮实现了，但它本身不在 `COBJS` 里，属于另一个待定问题，不在 P11 范围）。
- 不修 `progress.html` 里与 P11 无关的历史矛盾（L3 行「未开始」但 P10 已 done、L1 行「36 测试」实为 37、`U-L3-01/02` 用例号在代码里搜不到）。

## 7 不确定项（每条带验证方法）

1. **`switch_to(task0)` 后 task0 的 eflags 是否真的恢 IF=1**——依据是 `INIT_TASK_CONTEXT` 压栈 `0x202`。验证：L4 的 tick 用例若 PASS，即证明中断在自检点确实是开的；若 FAIL 且 `RESULT` 打出 tick 失败，则说明中断状态与预期不符，届时用 `pushfl` 打印实际 eflags 定位。**实测结果：PASS**（tick 用例 10ms 量级内就等到跳变），无需 `pushfl` 兜底。
2. **信号量握手在 task0 上下文里睡眠是否安全**——`schedule()` 在「当前任务非 READY 且无其它 READY 任务」时会选 task0；此时 task0 自己就是 `g_task_running`，不会切走而是在 `hlt` 里等中断。验证：握手用例若挂死，编排侧 90s 腿超时 + 串口尾部会暴露在 `SELFTEST [2/4]` 之后没有结果行；届时改用「先建线程再等待」并打印线程 tid 复核。**实测结果：PASS**，`[2/4] semaphore: PASS` 之后照常进 `[3/4]`，无挂死、无「结果行缺失」。
3. **`kmalloc(64)→kfree→kmalloc(64)` 地址是否必然相同**——TLSF 对同尺寸刚释放的块通常原址返回，但不是契约。验证：若实测不等，把判据改成「第二次分配成功且地址落在内核堆区间内」，并在 tech-plan 记下实测地址。**实测结果：同址**（断言原样保留，未降级）。
4. **合并（coalesce）smoke 是否真的触发合并**——`8×512→全释放→kmalloc(2048)` 只能证明「合并后能拼出更大块」，不能证明奇偶分释放顺序的完整场景。验证：本轮跑通的是**奇偶分释放**版本（先释偶数下标、再释奇数下标，最后要 2048B 并断言落在原 8 块区间内），比初稿的「全释放」更贴近 wiki 原文。**实测结果：PASS**。仍需注意这条只证明「那一片能拼出更大块」，不证明任意碎片形态，保留为已知局限。

**实施中与初稿的两处偏离（如实记录）**：

- 初稿 D5 写「`sem_wait` 递减后若 value 仍 >0 再 `wake_up` 一次（传棒），`sem_signal` 唤醒一个」。实现改为 `sem_signal` 里 `wake_up(&sem->wq, -1)` **全唤**、由各等待者重查计数决定谁通过。理由：传棒方案要求唤醒顺序与重查顺序一致，否则仍可能漏；全唤在这条链路上（等待者数量极小、唤醒后立刻重查）更简单且无漏唤醒，`wake_up` 已支持 `n<0` 全唤（`kernel/task.c:102-108`）。
- 初稿说合并 smoke 用 `kmalloc(4096)`，实现用 `kmalloc(2048)`（8×512 = 4096，但要求 2048 足够证明合并且不易触发「堆里别处有更大空闲块」的误判）。

## 8 验收判据

1. `make full` 全绿（L1/L2/L3 三腿不变 + L4 新腿 PASS），原始输出落 `docs/evidence/2026-11-11/p11-make-full.log`。
2. 串口里能看到 `SELFTEST [1/4]`…`[4/4]` 四行 PASS 与 `SELFTEST: RESULT PASS 4/4`，原始串口落 `docs/evidence/2026-11-11/p11-selftest-serial.log`。
3. 反向验证：临时把某一项判据改坏，确认 L4 腿会红、且退出码非 0；恢复后复跑全绿。**实际做法**（与初稿的「只在临时副本里」有偏差，如实记录）：改的是工作区真文件 `kernel/selftest.c`，先把原件备份为 `.p11-orig.c`，跑完 `cp .p11-orig.c kernel/selftest.c && touch` 还原并复绿，备份文件已删。红/绿两次原始输出分别落 `p11-negative-selftest.log`、`p11-selftest-leg.log`。
4. `Select-String` 复核本文件所有 `文件:行号`，落 `docs/evidence/2026-11-11/refs-check-p11.log`。
5. `git status --short` 只出现落点表声明的文件。
