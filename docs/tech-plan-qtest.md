# HDA qtest 技术方案：P09（L2 qtest 骨架）—— Gitee/osdev 调研轮

本文是 2026-10-31 调研轮的产出：只写方案与取证，不改内核代码、不改测试、不跑构建。
实施按第 7 章顺序在下一轮排期。基线：内层 x86 分支 `c0bf9f7`（P06 已提交）。

配套取证（均落 `docs/evidence/2026-10-31/`）：

- `gitee-qemu-qtest-protocol-excerpt.txt` —— qtest 行协议全文（system/qtest.c 60-268 行）
- `gitee-qemu-intelhda-excerpt.txt` —— intel-hda.c GCAP/流方向判定 + intel-hda-defs.h 偏移表
- `gitee-qemu-intelhda-test-excerpt.txt` —— 上游 intel-hda-test.c 全文
- `qemu-capability-probe.log` —— 本地 QEMU 3.1.50 能力探测与 qtest 全链路探针实录

## 1 范围

只覆盖 P09：L2 qtest 层的骨架与前提验证。P07-a（GCAP 解码）的取证补充写在
`docs/tech-plan-hda.md` §4.7，本轮不改它的实现排期。L3（make full）与 L4/L5 不在本文。

## 2 依据等级与采信规则

- `[Gitee: 文件:行号]` —— Gitee QEMU 镜像（gitee.com/mirrors/qemu）源码逐字摘录，
  已存 `docs/evidence/2026-10-31/`。镜像指向 master；本地二进制是 QEMU 3.1.50
  （v3.1.0-13607-geb2db0f7ba-dirty），两者行号可能漂移，凡涉及本地行为的一律以
  `[实测:]` 交叉验证后才采信。
- `[wiki: oldid=30627 摘录]` —— osdev wiki Intel HDA 页 2026-10-28 已存证摘录
  （`docs/evidence/2026-10-28/wiki-hda-excerpt.txt`）。本轮直读 wiki 被反爬墙拦截，
  GCAP/INTCTL/流描述符布局均以已存摘录为准，不重复采信未落盘的网页内容。
- `[实测: 文件:行号]` —— 本机命令与探针的原始输出，见 `qemu-capability-probe.log`。
- wiki/规范与实测冲突时以实测为准并显式列出；不确定项全部在第 8 章单列。

## 3 现状核对（基线 c0bf9f7）

- 内层与外层全仓 grep `qtest` 零命中：`make qtest`、客户端、用例均不存在，
  D5/D8/D10 所述的「QEMU 寄存器级、不启动本 OS」纯属规划
  `[实测: docs/tech-plan-qtest.md 调研轮仓内 grep]`。
- 层级与入口约束 [spec: docs/decisions-audio-test.md D5/D8/D10]：
  流与缓冲（BDL/CBL/LVI、RUN、LPIB 环绕回、BCIS）归 qtest 层；不动 QEMU（保持
  3.1.50），日常用 none 后端 + qtest；入口统一 `make test` / `make qtest` / `make full`，
  不用 watcher、不用独立 bat。
- 现有 QEMU 编排只有一条路：`tools/run_headless.bat` → `qemu-system-i386w -m 48
  -boot order=c -vga std -drive ... -device intel-hda -device hda-duplex -serial file:...
  -monitor telnet:127.0.0.1:4444`，monitor 仅用于 watcher 发 quit
  `[实测: tools/run_headless.bat:52-61]`。qtest 骨架不复用它（见 U5 的端口与单实例约束）。
- 本地 QEMU 能力 `[实测: qemu-capability-probe.log]`：`-qtest` 为隐藏选项但被接受；
  有 `none` 机型（另有 `pc`，默认 pc-i440fx-4.0）、`-S`、`intel-hda`/`hda-duplex` 设备。

## 4 前提验证：「不启 OS 操作 BDL」成立

三路互证：

1. 上游先例 `[Gitee: tests/qtest/intel-hda-test.c:41-68]`：`test_issue542_ich6` 在
   `-nographic -nodefaults -M pc-q35-6.2` 下不启动任何 OS，仅用 qtest 完成
   `outl 0xcf8` / `outw 0xcfc` 写命令寄存器 0x06（MEM|BUSM）、`outl 0xcfc` 自配
   BAR0=0x006a4400、`bufwrite` 直访 HDA MMIO（0x6a44xx 的 CTL/BDP 区）。该文件第 20 行
   自述「Tests only initialization so far」——上游没写功能用例，L2 用例确需自建。
2. 协议能力 `[Gitee: system/qtest.c:65-268]`：行协议按「命令→OK/FAIL+值」应答；
   PIO 走 `outb/outw/outl`、`inb/inw/inl`；物理内存走 `writeb..writeq`、
   `readb..readq`、`read/write`、`b64write/b64read`；另有 `clock_step/clock_set`
   （客户端掌控虚拟时钟）与 `irq_intercept_in/out`（中断观测，后置 BCIS 用例用）。
3. 本地全链路 `[实测: qemu-capability-probe.log v2 探针]`：起 `-M pc -S -nodefaults
   -display none -serial none -monitor none -device intel-hda -qtest
   tcp:127.0.0.1:4653,server,nowait`，Python stdlib 客户端 23 条命令全绿——
   PCI 扫描命中 intel-hda 于 00:02.0（vid:did=0x26688086）、命令寄存器写 0x06、
   自配 BAR0=0xE0000000 并读回一致、`readl` 读 **GCAP=0x4401**（OSS=4 ISS=4 BSS=0
   64OK=1）、SD0 LPIB=0。

   GCAP=0x4401 三方一致：`[Gitee: hw/audio/intel-hda.c:611-614]`（regtab `.reset=0x4401`）、
   `[实测: qemu-capability-probe.log]`（qtest 读回）、
   `[实测: docs/evidence/2026-10-28/qemu-serial.log:31]`（OS 启动串口）。

探针教训（进 §8 的 U4/客户端实现约定）：MMIO 必须用 `read/write` 系列（物理内存
语义，上游同款）；v1 探针用 `inl`（PIO 语义，x86 端口空间 64KB）读 MMIO 直接把
QEMU 连接打崩（WinError 10054）。PIO 与 MMIO 两族命令不可混用。

## 5 骨架设计

### 5.1 QEMU 启动形态

```bat
set QEMU_AUDIO_DRV=none
qemu-system-i386w.exe -M pc -nodefaults -display none -serial none -monitor none ^
  -device intel-hda -device hda-duplex ^
  -qtest tcp:127.0.0.1:4653,server,nowait
```

- **不冻结 VM（2026-10-31 实现轮修正，撤回原 -S 方案）**：本地 3.1.50 w 构建未编
  qtest accelerator（`[实测: "-machine accel=qtest" -> "No accelerator found /
  QTest not supported for this target"]`），`clock_step` 被判 Unknown command；
  机器照常运行，SeaBIOS 空转不碰 HDA，虚拟时钟按真实时间走，codec 的虚时钟
  定时器才会在真实时间触发 —— 上游 ich6_test 同为运行中的机器。
- `-nodefaults` 去掉默认 VGA/网卡，PCI 扫描面干净（探针实测 00:00.0 i440fx、
  00:01.0 PIIX3、00:01.1 IDE、00:02.0 intel-hda）。
- `QEMU_AUDIO_DRV=none` 即 D8 的「none 后端」在 3.1.50 上的写法（3.1 没有
  `-audiodev`，那是 4.2+ 的选项）；codec 挂 hda-duplex 但不发声。
- `-qtest tcp:...`：Windows 上不用 unix socket；端口 4653（避开 monitor 4444）。

### 5.2 客户端

- 新文件 `tests/qtest_client.py`（建议名）：Python 标准库 socket 实现 qtest 行客户端，
  `uv run --no-project python tests/qtest_client.py` 调用。选 Python 不选 C 的理由：
  stdlib 零依赖、与 hda_tests 的 uv 用法一致、行协议解析三行代码；C 客户端要走
  mingw 编译链而收益为零。
- 客户端结构（下轮实现）：connect → `cmd()`（发命令、收 OK/FAIL 行，带超时）→
  封装 `outl/inl`（PIO）、`readl/writel`（MMIO）、`b64write`（大块内存）→ 用例断言。
- 失败即停：任何一条 FAIL/超时立即退出非零并打印最后一条命令，不降级续跑。

### 5.3 寄存器链路（用例共用的初始化序列）

1. PCI 扫描 bus 0（dev 0-31）：`outl 0xcf8, 0x80000000|dev<<11` + `inl 0xcfc` 找
   0x26688086；**禁止硬编码 00:02.0**（设备增减会变槽位，探针脚本已按扫描写）。
2. 命令寄存器（offset 0x04）写 0x06（MEM|BUSM）。
3. BAR0（offset 0x10）：先读，为 0 才自配 0xE0000000 并读回校验——机器运行中
   SeaBIOS 可能已代配（2026-10-31 实现轮实测 SeaBIOS 分配 0xfebfc000，客户端
   自适应读取）。
4. GCAP 校验：`readl bar+0x00 == 0x4401`，解出 OSS/ISS；SD 基址按
   `0x80 + 0x20*n`，输入流索引=k、输出流索引=ISS+k `[wiki: oldid=30627 摘录 §三]`
   `[实测: Gitee intel-hda-defs.h 偏移表]`。
5. 物理内存铺 BDL 与环形缓冲：`-M pc` 默认 RAM 128MB，SeaBIOS 未运行，低内存干净；
   取 BDL@0x00300000（4KB 内）、环形缓冲@0x00400000（256KB），用 `b64write` 写入。
6. 写 SD 寄存器：BDPL/BDPU=BDL 物理地址、CBL=256KB、LVI=n-1；`writel bar+SDn+CTL`
   置 RUN；`readl` 回读校验。
7. 读 LPIB（SD+0x04）观察推进与环绕（U-L2-02）；结束时清 RUN。

### 5.4 用例规划（对应「未实现与受阻」表的 U-L2-01/02）

| 用例 | 内容 | 判据 |
|---|---|---|
| U-L2-01 | BDL 链写读回 + RUN 位：GCAP 校验 → SD 基址推算 → b64write 写 2 条 BDL → readl 逐字段读回 → 置 RUN 读回 | 字段全等；RUN=1；CBL/LVI 回读一致 |
| U-L2-02 | LPIB 推进与环绕回：RUN=1 后轮询 LPIB，覆盖 CBL 一次环绕 | LPIB 单调推进且发生 ≥1 次回绕；停 RUN 后不再推进。绑定用 tag=0：无 verb 时 codec 默认 `st->stream=0`，`intel_hda_xfer` 按 `ctl>>20` 匹配 `[Gitee: hw/audio/intel-hda.c:404-410]`；清洁 verb 绑定（tag=1 + SET_CHANNEL_STREAMID/SET_STREAM_FORMAT）实测不推进，客户端内置组合回退并全部留痕（2026-10-31 实测 tag=0 组合 300ms 推进 ~62KB） |
| 后置 | BCIS 中断：`irq_intercept_out` + `clock_step` 观测中断 | 中断计数随条目完成递增 |

U-L2-02 的时钟问题见 U1：`-S` 下虚拟时钟是否自行推进、audio 定时器挂哪个时钟域，
决定轮询是真实时间等待还是 `clock_step` 驱动，下轮先做 5 分钟实验再定稿用例。

### 5.5 落点

| 文件 | 改动 |
|---|---|
| 根 `Makefile` | 新增 `qtest:` 目标：`uv run --no-project python tests/qtest_client.py`（照 `test:` 目标的薄转发风格，不加 watcher、不加 bat，D10） |
| `tests/qtest_client.py` | 新增：行客户端 + PCI/GCAP 初始化序列 + U-L2-01/02 两用例 |
| `tests/Makefile` | 不动（qtest 不走 Unity 宿主框架） |
| `docs/evidence/<日期>/` | 每轮跑 qtest 的原始输出（含探针复跑） |

hda_tests（外层）不接 qtest：它测的是整机行为，L2 归内层 `make qtest`（D5/D10）。

## 6 验收判据（下轮交差时逐条对照）

1. `make qtest` 一条命令跑完 U-L2-01/02，输出逐命令 transcript + PASS/FAIL，退出码 0。
2. 客户端禁止硬编码 BDF 与 BAR 地址（扫描 + 分配 + 回读校验）。
3. GCAP 校验值来自寄存器而非常量比对放行：读值 != 0x4401 时 FAIL 并打印。
4. 证据落 `docs/evidence/<日期>/`，含命令原文与退出码；行号复核走 refs-check。
5. `make test` 不退化（L1 与 L2 互不影响）；本轮基线 c0bf9f7。
6. 不动 QEMU、不动 hda_tests、不引入 watcher/独立 bat（D8/D10）。

## 7 实施顺序（下一轮）

1. `tests/qtest_client.py` 骨架：行客户端 + 探针序列（第 4 章的 7 步）固化为
   `--probe` 模式，`make qtest` 跑通第一条 PASS（GCAP 读回校验）。
2. U-L2-01 落地（BDL 链 + RUN 位）。
3. U1 实验（LPIB 时钟域）→ U-L2-02 落地（推进与环绕）。
4. 后置：BCIS 中断用例（irq_intercept_out + clock_step）。
5. progress.html 勾选 P09、更新 ④ 表 L2 行。

## 8 风险、回退与不确定项

- **U1 LPIB 时钟域（已定论，2026-10-31 实现轮）**：原假设 `-S` 冻结 + `clock_step`
  驱动。实测本地 3.1.50 w 构建未编 qtest accelerator（`-machine accel=qtest` ->
  "No accelerator found"），`clock_step` 不可用；改用「机器不冻结、真实时间驱动」
  （上游 ich6_test 同模式）后 LPIB 正常推进与环绕。客户端去掉 clock_step 依赖。
- **U2 codec 挂载（已消解）**：hda-duplex 全程在场，LPIB 实测推进（见 U-L2-02）；
  `QEMU_AUDIO_DRV=none` 工作正常。
- **U3 VMaj/VMIN 偏移**：`[Gitee: hw/audio/intel-hda-defs.h:19-20]` VMIN@0x02、
  VMAJ@0x03，内核 `hda.h:13-14` 同序，探针实测 0x02=0、0x03=1 三方一致；但本仓
  wiki 摘录未含这两个寄存器，Intel 规范常见表述为 VMaj@0x02——QEMU 与内核可能是
  「同错互配」。影响仅串口版本打印（v1.0），无数据通路。原验证方法（真机轮 dump
  0x02/0x03 两字节核对）随真机路线废弃（2026-11-10 QEMU-only，
  `docs/decisions-qemu-only.md`）——维持三方一致现状，若改 §4.2 的打印，按实测定序，
  不按记忆。
- **U4 qtest 无 quit 命令**：协议无优雅退出命令，客户端收尾用外部 `taskkill`（探针
  已验证 Stop-Process 可行）。L2 不写 wav 文件，无「优雅退出 flush」诉求，风险低。
- **U5 端口与单实例**：monitor 4444 的单实例约束（env.py docstring）扩展到 qtest
  端口 4653：`make qtest` 启动前必须确认无残留 QEMU 实例，客户端 connect 失败时报
  「疑似残留实例」而不是无限重试。
- **U6 -M pc 默认机型漂移**：默认 pc-i440fx-4.0 随 QEMU 版本走；本仓锁定 3.1.50
  不受影响，但骨架在机型参数上显式写 `-M pc`（探针同款），不依赖默认值。

## 9 与既有文档的关系

- `docs/decisions-audio-test.md` D5/D8/D10 不变，本文是它们在 L2 的落地细化。
- `docs/tech-plan-hda.md` §4（P07-a）的 GCAP 位段与索引公式被 §5.3 第 4 步直接
  复用；其取证补充见该文件 §4.7。
- progress.html 的 P09 条目随本文更新为「方案已定（调研完成）」，勾选仍在下一轮
  实现落地后进行。

## 10 落地记录（2026-10-31 实现轮）

- `tests/qtest_client.py` 落地：行客户端 + PCI 扫描 + BAR 自适应（SeaBIOS 代配
  0xfebfc000 时直接采用）+ ICS verb 路径 + U-L2-01/02；`--probe` 单独跑 GCAP 校验。
- 根 `Makefile` 增 `qtest:` 目标（uv 直调，无 watcher/独立 bat，D10）。
- `make qtest` 15 PASS / 0 FAIL（证据 `docs/evidence/2026-10-31/qtest-run.log`）；
  `make test` 36 条不退化。
- 与 §5 方案的两处偏差已回写 §5.1/§5.3/§5.4：不冻结 VM（qtest accel 未编译进本
  构建）、BAR 自适应、U-L2-02 用 tag=0 绑定（含组合回退）。
- 后置：BCIS 中断用例（irq_intercept_out + IOC=1 条目）仍待排期。
