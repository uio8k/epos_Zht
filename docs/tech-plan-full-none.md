# L3 整机 none 后端回归技术方案：P10（`make full`）

日期：2026-11-11（autodev 轮 3）。决策来源 `docs/decisions-audio-test.md` D10（入口统一
`make test` / `make qtest` / `make full`）与 D8（不动 QEMU 3.1.50，日常用 `none` 后端）；
层定义见同文件 L3 行（整机、`none` 后端、覆盖全流程/syscall 往返/用户态 API 契约/WAV 读取）。
基线：内层 x86 分支 `acfa92f`（P05b 已提交）。

配套取证（均落 `docs/evidence/2026-11-11/`）：

- `gitee-p10-noaudio-excerpt.txt` —— QEMU `audio/noaudio.c` 全文摘录（采集/播放两侧行为）
- `gitee-p10-audio-core-excerpt.txt` —— `audio/audio.c` 驱动选择 + `hw/audio/hda-codec.c` 采集通路
- `qemu-audio-help-p10.log` —— 本机 QEMU 3.1.50 `-audio-help` 实测（三个驱动能力对比）
- `p10-uv-path-bug.log` —— `make qtest` 的 uv 定位缺陷复现（本轮顺带修）
- `p10-probe-serial-visibility.log` / `p10-probe-demo.log` / `p10-probe-capture.log` —— 串口可见性探针与三种构建的实跑串口（见 §3.5、§6.2）

## 1 范围与原则

只做 P10：把 D10 早已写定但**仓库里并不存在**的 `make full` 入口落下来，并让它在
`none` 后端下跑通整机回归。P11（QEMU 整机 SELFTEST 段）依赖本项产出，不在本文。

- **不碰内核与用户态程序**：本轮只加构建入口与整机测试编排，
  `kernel/**`、`userapp/**` 零改动。
- **不动 QEMU 3.1.50**（D8）：`none` 后端通过环境变量 `QEMU_AUDIO_DRV=none` 选中，
  不改二进制、不改构建（`[Gitee: audio/audio.c:1898]`）。
- **不碰宿主默认音频设备**（D9）：全程 `QEMU_AUDIO_DRV=none`，不使用 `dsound`。
- **不引入新依赖**：整机编排用 Python 标准库（沿用 `tests/qtest_client.py` 先例，
  选 Python 不选 C 的权衡见 `[spec: docs/tech-plan-qtest.md:98-101]`）。

## 2 依据等级与采信规则

- `[Gitee: 文件:行号]` —— `gitee.com/mirrors/qemu` raw 直连摘录，锚定 tag **v3.1.0**
  （本机二进制 `QEMU emulator version 3.1.50 (v3.1.0-13607-geb2db0f7ba-dirty)`，
  `[实测: qemu-audio-help-p10.log]` 首行）。既有 `tech-plan-qtest.md` §2 用的是
  master 镜像，行号会漂移；本文一律钉 tag，凡涉及本机行为再用 `[实测:]` 交叉。
- `[实测: 文件:行号]` —— 本机命令与串口原始输出。
- `[spec: 文档:行号]` —— 仓内已定决策。
- 三者冲突时以实测为准并显式列出；不确定项在 §9 单列。

## 3 现状核对（基线 acfa92f）

### 3.1 `make full` 根本不存在

全仓 `Makefile`（基线 140 行）只定义 `all / subdirs / hd.img / test / qtest / qemu /
qemudbg / bochs / bochsdbg / vbox / tags / hd.vmdk / run / debug / clean / diff / submit`
（`[实测: git show acfa92f:Makefile 逐行]` 中 15 处 `.PHONY`：`:10 :16 :27 :33 :37 :41
:58 :66 :75 :90 :97 :100 :103 :108 :116`），**没有 `full` 目标**。而 D10 把 `make full`
写成既定入口：

> `D10 | 入口统一为 **\`make test\` / \`make qtest\` / \`make full\`**；不用 watcher，不用独立 bat`
> ——`[spec: docs/decisions-audio-test.md:20]`

> `| \`make full\`（整机，\`none\` 后端） | 全流程、syscall 往返、用户态 API 契约、WAV 读取 | 整机单次 ~10s，总计计入 60s 预算 | 提交前 |`
> ——`[spec: docs/decisions-audio-test.md:30]`

即：**决策已定、实现为零**。P10 就是补这一刀。同时 D-Q4 要求 P11 的 SELFTEST
段「并入 `make full` 整机链路」（`[spec: docs/decisions-qemu-only.md:12]`），
所以 `make full` 的骨架必须先有，否则 P11 无处可挂。

### 3.2 `make qtest` 当前是坏的

脱离本次修复直接跑会失败：

```
"${HOME:-${USERPROFILE}}/.local/bin/uv.exe" run --no-project python tests/qtest_client.py
/bin/bash: /home/Administrator/.local/bin/uv.exe: No such file or directory
make: *** [Makefile:35: qtest] Error 127
```
（`[实测: docs/evidence/2026-11-11/p10-uv-path-bug.log]`）

根因：`make qtest` 原来那行（修复前的 `Makefile:35`，本轮改动后为 `Makefile:42`）
的 uv 优先级写反了。`make` 的 `SHELL=/bin/bash`（`[实测: Makefile.inc:7]`）
是 MSYS bash，它把 `HOME` 覆盖成 `/home/Administrator`（本仓 MSYS 2.0 安装无此目录），
于是 `${HOME:-...}` 永远命中错误值；正确位置在 `USERPROFILE`。同 shell 实测：

| 变量 | cmd 层 | bash 层 |
|---|---|---|
| `HOME` | （空） | `/home/Administrator` |
| `USERPROFILE` | `C:\Users\Administrator` | `C:\Users\Administrator` |

`test -x "$USERPROFILE/.local/bin/uv.exe"` → YES；`test -x "$HOME/.local/bin/uv.exe"` → NO。

历史上能跑通，是因为一直在会话里手工 `$env:HOME='C:\Users\Administrator'` 兜着——
这违反 D10「入口统一」的精神：入口自己不稳，等于没有入口。**本轮一并修**。

### 3.3 `none` 后端下整机到底还能测什么

这是 P10 的核心未知（progress.html 原文：`无 —— 需先确认 none 后端下采集返回静音但管线照跑`）。
一手源码给出确定的答案：

- 采集侧 `no_read()` 按虚拟时钟算出应交付字节数 `to_clear`，把缓冲**清零**后返回
  `to_clear << shift`（`[Gitee: audio/noaudio.c:120-130]`）；`no_run_in()` 的样本数由
  `QEMU_CLOCK_VIRTUAL` 差值算出（`[Gitee: audio/noaudio.c:99-118]`）。
- 播放侧 `no_run_out()` 同样按虚拟时钟推进 `hw->rpos`（`[Gitee: audio/noaudio.c:43-61]`），
  写进环形缓冲的数据被正常消费。
- 采集数据源是 `AUD_read(st->voice.in, ...)`（`[Gitee: hw/audio/hda-codec.c:271]`），
  在 none 后端下正是上面那个 `no_read()`。

结论：**管线照跑、时长照走、样本恒为静音（全 0）**。因此可断言的是
「字节数随时间线性增长、停流后不再增长、LPIB 推进、环绕次数、overrun=0、
用户态参数校验返回值」；**不可断言**的是任何样本内容（峰值/过零/频率恒为 0）。
这与 D-Q5「模拟器值非真值」的口径一致，也是 P05b 删掉 guest 侧真值指标后剩下的全部可测面。

### 3.4 后端选择是被 D8/D9 夹死的唯一解

`-audio-help` 实测本机构建只有三个驱动（`[实测: qemu-audio-help-p10.log]`）：

| 驱动 | 播放 | 采集 | 用于 L3？ |
|---|---|---|---|
| `none` | 多路 | **多路** | ✅ 唯一同时满足 D8+D9 |
| `wav` | 1 路 | **Does not support capture** | ❌ 采集回归跑不了 |
| `dsound` | 多路 | 1 路 | ❌ 要碰宿主默认设备，违反 D9 |

补充：即使不设 `QEMU_AUDIO_DRV`，`audio_init()` 最后也有硬兜底
`audio_driver_lookup("none")`（`[Gitee: audio/audio.c:1920-1925]`），所以无宿主声卡时
QEMU 不会因音频初始化失败而退出——`none` 是天然兜底，不是特例路径。

### 3.5 现有整机编排不能复用

`tools/run_headless.bat` 是既有的无头运行脚本，但它有三处与 D10 冲突：
默认 `QEMU_AUDIO_DRV=dsound`（`:32`）、靠 `start /b` 起后台 watcher（`:52`）、
用 telnet monitor 发 quit（`:61`）。D10 明写「不用 watcher、不用独立 bat」。
故本轮**新写**一个同步的、纯标准库的整机编排，不复用该 bat。

## 4 备选对比矩阵

| 方案 | 做法 | 优点 | 缺点 | 结论 |
|---|---|---|---|---|
| A. 复用 `run_headless.bat` | `make full` 直接调现有 bat | 零新代码 | 违反 D10（watcher + bat）；默认 dsound 违反 D9；marker 靠 watcher 轮询，失败模式难判 | ❌ |
| B. 只用 qtest 扩用例 | 把整机项都塞进 L2 qtest | 复用现有骨架 | qtest 不启动 OS，**测不到** syscall 往返、用户态 API 契约、WAV 读取——正是 D2 表给 L3 定的覆盖 | ❌ |
| C. **新增 `tests/full_run.py` + `make full`** | Python 标准库起停 QEMU，读串口断言 | 满足 D10（纯 make 入口、无 watcher、无独立 bat）；满足 D9（全程 none）；与被测 OS 零耦合 | 需新写约 200 行编排 | ✅ **采用** |
| D. 整曲跑完再断言 | 等 220s 全曲结束 | 覆盖最完整 | 单次 220s，爆掉 D2 的 60s 全套预算 4 倍 | ❌ |

方案 C 的判定设计：**不等收尾，边跑边断**。串口在 QEMU 运行期间就可读
（`[实测: p10-probe-serial-visibility.log]`：t=2.5s 首次可见，t=5.5s 出现完成标记），
所以取「轮询到目标标记即断言 → 主动结束 QEMU」的模式，不依赖优雅退出。

## 5 落点表

| 文件 | 改动 | 说明 |
|---|---|---|
| `Makefile` | ① 新增 `.PHONY: full` 与 `full:` 目标（`test` → `qtest` → `full-l3`）；② 新增 `full-l3` 目标调 `tests/full_run.py`；③ **修 `qtest` 的 uv 定位**：`${HOME:-${USERPROFILE}}` → `${USERPROFILE:-${HOME}}`；④ 新增 `UV` 变量供 `qtest` / `full-l3` 共用 | 只碰构建入口 |
| `tests/full_run.py` | 新增：L3 整机编排（三条腿，见 §6.2） | 纯标准库 |
| `progress.html` | P10 `data-done="0"→"1"`、状态改「已完成」、`ev` 换成实测依据、变更记录顶部加 2026-11-11 条目 | 第 8 步 |
| `docs/tech-plan-full-none.md` | 本文 | — |
| `docs/evidence/2026-11-11/*` | 调研摘录 + 各腿实跑日志 + refs-check | — |

`kernel/**`、`userapp/**`、`hda_tests/**`（外层）、`Qemu/**` **零改动**。

## 6 实现设计

### 6.1 `make full` 目标结构

```makefile
.PHONY: full
full: test qtest full-l3

.PHONY: full-l3
full-l3:
	$(UV) run --no-project python tests/full_run.py
```

保持串行（不写 `-j`）：**这是保守取舍，不是硬约束**。初稿曾以「`../lib/*.o` 的
`.FORCE` 规则在 `-j` 下必现 `file truncated`」为由，但该依据所引的
`docs/evidence/2026-11-10/build-j4.log` 在仓库与工作区均不存在（git 历史也无此路径），
属不可引用证据。本轮重新实测：`make clean && make -j4` 与 `-j8` 各连跑 3 次，
**6/6 退出码 0、无 `file truncated` / `file not recognized` / `Error 1`**
（`[实测: docs/evidence/2026-11-11/p10-build-parallel-check.log]`）。

串行的真实理由是成本收益：串行实测 29.2-29.6s、`make clean` 后冷起点 35.7s，均已满足 D2 的 60s 预算
（`[实测: docs/evidence/2026-11-11/p10-make-full.log]`），没有为省几秒去引入并行
不确定性、让提交前档变得时好时坏的收益。`.PHONY` 依赖顺序 = 执行顺序。

若将来确需并行，先用上述日志的方法重测并补证据，不得凭旧印象断言。

### 6.2 三条腿（`tests/full_run.py`）

三条腿各用一次构建，覆盖 D2 给 L3 定的四项：

| 腿 | 构建 | 覆盖 | 判据（串口行） | 实测 |
|---|---|---|---|---|
| **L3-A PLAY** | `make hd.img TEST_MODE=1 TEST_SIGNAL=sine` | PCM 播放全链、syscall 往返（volume/errors/position） | `[TEST] volume set to 100`、`[TEST] sine 440Hz, 2000ms`、`BDL ok entries=128 cbl=524288`、`continuous stream started (CBL=524288, 44100Hz 2ch 16bit)`、`stopped (... underruns=0)`、`[AUDIO] errors: fifo=0 desc=0  position: N ms` | 2.5s + 4.5s |
| **L3-B WAV** | `make hd.img SORT_DEMO=1` | 盘上 WAV 读取 + 3 路混音 | `[AUDIO] volume set to 80`、`[MELODY] Shell sort driving melody`、`[MELODY] #0 note=N ms=N voices=3 sNN.wav` | 2.6s + 2.2s |
| **L3-C CAPTURE** | `make hd.img CAPTURE_TEST=1` | 采集全流程、512KB 环绕、用户态 API 契约 | `[CAP-A] VERDICT: len=OK path=OK signal=SILENT`、`bytes=... expect=176400 ... overruns=0`、`[CAP-B] ... expect=529200 ... wraps=1 overruns=0`、`[CAP-C] first=0 same=0 diff=-2 stop=0`、`AUDIO_VERDICT v=2 ... silent=1` | 2.5s + 7.2s |

**为什么腿 B 不用默认演示（整曲）**：甲、整曲 220s 超 D2 预算；乙、`song.wav` 是
`.gitignore` 里的大文件（`[实测: .gitignore:3-4]` 与 `git ls-files samples/song.wav` 空结果），
干净 clone 里不存在——拿它当必跑判据会让 `make full` 在新机器上直接红。
改用 `SORT_DEMO=1`：走的仍是 `hda_play_file_ms` → 盘上 `sNN.wav` → 3 路混音这条真实读盘
路径，2.2s 就落到第一音符。整曲完整性继续由外层 `hda_tests --suite full`（6 分钟档）覆盖。

腿 C 的断言**只断行为不断样本**：`wraps=1`（512KB 环绕）与 `overruns=0` 是时序事实，
`signal=SILENT` / `peak=0` 在 none 后端下是**预期值**（§3.3），所以断「等于 SILENT」而非「不等于」。

### 6.3 起停与判定约定

- 启动：`QEMU_AUDIO_DRV=none` + `-display none` + `-serial file:<tmp>` + **无 monitor**
  （不设 monitor 就不需要 watcher——D10）。设备沿用生产配置
  `-device intel-hda -device hda-duplex`（`[实测: Makefile:60]`）。
- 轮询：每 100ms 读串口文件，正则命中目标标记即判定；单腿超时 = `tone_run_timeout`
  同量级（120s），超时按 FAIL 并打印已捕获的尾部。
- 结束：`proc.kill()`（qtest 客户端同款做法，`[实测: tests/qtest_client.py:112-115]`）。
  判定已在运行中完成，不依赖优雅退出。
- 残留：起跑前探端口/进程，有残留先清（理由同 `tests/qtest_client.py:60-69`）。
- 退出码：任一腿 FAIL → 非 0，`make` 直接失败（D12 最小复现优先）。

### 6.4 uv 定位修正

```makefile
UV ?= "$${USERPROFILE:-$${HOME}}/.local/bin/uv.exe"
```

`qtest` 与 `full-l3` 共用。Windows 取 `USERPROFILE`；Linux 上 `USERPROFILE` 未定义，
自然退回 `HOME`——两边都对，且**不再依赖调用方手工导出 HOME**。

## 7 与 decisions-audio-test.md D1–D12 的相容性核对

| 决策 | 相容性 |
|---|---|
| D1 快速反馈 > 防回归 | 相容：`make full` 是提交前档，不进「改一行 2 秒」的循环 |
| D2 单测 <2s、全套 <60s、提交前跑全套 | **达标**：实测 `make full` 整条链 29.2-29.6s、冷起点（`make clean` 后）35.7s（`[实测: docs/evidence/2026-11-11/p10-make-full.log` / `p10-make-full-clean.log`），L3 三条腿各 2.6-7.4s 构建 + 2.6-7.3s 运行；预算 60s，余量约 40% |
| D3 分层承担 | **强化**：L3 首次真正承载「整机」这一层，此前该层只有规划 |
| D4 单测只做 codec 配置层 | 不涉及 |
| D5 流与缓冲不在单测层 | 相容：本项不新增 L1 用例 |
| D6 内存合成样本、不伪造 dosfs | 相容：腿 B 走真实盘上 `sNN.wav` 读盘（整机层本就该测盘），L1 的单测样本仍全是内存合成，两者解耦 |
| D7 可测性改造限 5–10 处 `#ifdef HDA_HOST_TEST` | 不涉及：本轮 `kernel/**` 零改动 |
| D8 不动 QEMU、日常 none 后端 | **落实**：全程 `QEMU_AUDIO_DRV=none`，不碰 3.1.50 |
| D9 禁止改宿主默认音频设备 | **落实**：不使用 `dsound`；`-display none` 也不碰宿主图形 |
| D10 入口统一 make、不用 watcher/独立 bat | **落实**：`make full` 一条命令；编排无 watcher、无 bat；顺带修好 `qtest` |
| D11 `audio_metrics.py` 唯一指标源 | 相容：L3 只读串口行，不做音频分析 |
| D12 最小复现 + 期望/实际差值 | 相容：每腿失败打印命令行 + 期望正则 + 串口尾部 |

## 8 验收判据

1. `make full` 在**干净 shell**（不预设 `HOME`）下一条命令跑完，退出码 0。
2. 同条件下 `make qtest` 恢复可用（修复前复现为 exit 2，见 `p10-uv-path-bug.log`；修复后 exit 0、15/15）。
3. `make test` 仍 37/37 全绿；`hda_tests --suite fast` 仍 14/16（唯二存量）。
4. 三条腿的断言全部命中，原始串口落 `docs/evidence/2026-11-11/p10-make-full.log`（含各腿串口路径 `tests/build/l3_*.log`）。
5. `git status --short` 收尾只出现 `Makefile`、`tests/full_run.py`、`progress.html`、
   `docs/**`——`kernel/`、`userapp/` 零命中。
6. 全套墙钟 < 60s（D2）。

## 9 不确定项（每条带验证方法）

| 编号 | 不确定项 | 验证方法 |
|---|---|---|
| U1 | 串口文件在 QEMU 运行期间的可读时序（有无缓冲导致标记迟到） | 已完成初测：t=2.5s 首见、t=5.5s 见完成标记（`p10-probe-serial-visibility.log`）；实现后再用轮询日志复核 |
| U2 | 各腿能否在超时内稳定拿全标记（不同机器负载下） | 已实测：L3-A 4.5s、L3-B 2.2s、L3-C 7.2s，均远低于 90s 上限；不达标时提高超时而非删断言 |
| U3 | 腿 B 的 `bytes` 与 `expect` 差值是否为确定值 | 实测两跑：A 腿 `diff=3824` 两次一致；B 腿 `diff=3280/756` **不一致** → 断言用区间（`0 ≤ diff ≤ expect/20`），不用等值 |
| U4 | 构建缓存（`hd.img` 已存在）时各腿的增量耗时 | 已实测：冷构建 7.8s，三条腿模式切换各 2.5-2.6s（`p10-make-full.log` 逐腿记录） |
| U5 | `CAPTURE_TEST=1` 与非 `CAPTURE_TEST` 之间切换是否残留 mark 文件 | `userapp/Makefile:46` 的 `MODE_STAMP` 含 `$(CAPTURE_TEST)`，理论自洽；实跑两腿后核对 `a.out` 的串口输出确实换了腿 |
| U6 | `-display none` 下 `vga8_enter()` 是否仍返回成功 | 已实测：返回成功并打印 `[VGA] verdict shown: SILENT`（`p10-probe-capture.log`），不阻塞判定 |
| U7 | 该不该把 `hda_tests --suite full`（6 分钟档）并进 `make full` | 不并：超 D2 的 60s 预算，且它属外层工作区、不在仓库内。留作手动档 |

## 10 落地顺序（本轮执行）

1. 改 `Makefile`：加 `UV` 变量、修 `qtest`、加 `full` / `full-l3`。
2. 写 `tests/full_run.py` 三条腿。
3. 跑 `make full`，红则自修（≤2 轮，禁止放宽断言）。**实际自修 1 轮**：L3-A 首跑两条正则与实际串口不符（音量行是 100 不是 80；position 后多一个冒号），照实跑串口改正。
4. 补跑 `make test`、`make qtest`、`hda_tests --suite fast`。
5. 证据落盘 + refs-check。
6. 更新 `progress.html`，征得确认后提交。

## 11 与既有文档的关系

- 本文补的是 `tech-plan-qtest.md` §1 明确排除在外的「L3（make full）」
  （`[spec: docs/tech-plan-qtest.md:16]`）。
- `tech-plan-hda.md` 的实现排期不变。
- P11 的 SELFTEST 段将挂在本轮产出的 `make full` 骨架上（D-Q4）。




