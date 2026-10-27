# 音频驱动测试体系决策记录

日期：2026-10-27。来源：一轮 grill（深挖），用户逐条确认。供后续实现/评审直接接手。
范围：HDA 音频驱动（codec 配置、流与缓冲、用户态 API、WAV 读取）的测试方式。

## 1. 已定结论

| # | 结论 | 理由 |
|---|---|---|
| D1 | 目的优先级：**开发时快速反馈 > 防回归 > 对外证明（不考虑）** | 主结构按"改一行 2 秒内知道对错"来搭；漂亮指标不是目标 |
| D2 | 速度目标：单测 **<2s**、全套 **<60s**、**提交前跑全套** | 可检验，用来判断某一层是否必修 |
| D3 | 被测对象覆盖**整条音频栈**，但**分层承担**，不是每层都全覆盖 | 覆盖靠层组合，不是靠单层堆砌 |
| D4 | **单测层只做 codec 配置层**：verb 构造、分级回读、连接列表长短格式、amp 方向位 | 这三类 bug（0xF000 方向位、长格式索引、无回读）都在这一层，且假寄存器成本最低 |
| D5 | **流与缓冲**（BDL/CBL/LVI、RUN、LPIB 推进与环绕回、BCIS 中断、256/512KB 切换）与**用户态 API 契约**（参数校验、返回码、重复 start 幂等/拒绝）**不在单测层**，归 qtest 与整机层 | 单测层伪造中断/时序成本高、收益低 |
| D6 | 需要样本时用**内存合成样本**，**不伪造 dosfs**，磁盘与 WAV 解析不在单测层 | 解耦 HDA 驱动与文件系统，二者回归风险不同量级 |
| D7 | 可测性改造限于**5–10 处 `#ifdef HDA_HOST_TEST`**（桩 printk/malloc/中断投递），驱动主体逻辑与已提交结构不动；**不接受拆结构重构** | 拆结构会动 ff1eeba，回归风险最高 |
| D8 | **不动 QEMU**（保持 3.1.50），日常用 `none` 后端 + qtest；"给 QEMU 加读 WAV 的 capture 后端"**推迟**，只有确实要做 SNR/相关性时才重新评估 | 真实回环已降级，不值得先扛 Windows 上重建 3.1.50 的代价 |
| D9 | 自动回归**禁止**改宿主默认音频设备；真实回环（板载 + 立体声混音）改为**手动偶发**，仍作为交付证据 | 碰系统设置 = 不可复现，且失败会把宿主留在错误配置 |
| D10 | 入口统一为 **`make test` / `make qtest` / `make full`**；不用 watcher，不用独立 bat | 与现有 Makefile 体系一致，编辑器无关 |
| D11 | `tools/audio_metrics.py` 作为唯一指标源**不动**；`hda_tests/` 保留 shell/编排层，断言逻辑下沉到测试层；`tools/gdb_mcp_probe.py` 留作最后手段 | 避免两套指标口径，也避免重复造编排 |
| D12 | 失败输出优先"**最小复现命令 + 期望/实际差值**"；寄存器快照只在单测层打印 | 快速反馈需要的是定位入口，不是大段 trace |

## 2. 分层归属与预算

| 层 | 覆盖 | 预算 | 触发 |
|---|---|---|---|
| `make test`（单测，宿主侧，假寄存器/假 codec） | codec 配置层（D4） | <2s | 每次改动 |
| `make qtest`（QEMU 寄存器级，不启动本 OS） | 流与缓冲、中断、LPIB/环绕回（D5） | ~数秒 | 改流/缓冲代码时 |
| `make full`（整机，`none` 后端） | 全流程、syscall 往返、用户态 API 契约、WAV 读取 | 整机单次 ~10s，总计计入 60s 预算 | 提交前 |
| 手动：真实回环 | 真实样本、SNR/相关性、宿主兼容 | 不计入预算 | 交付/偶发 |

分层示意图：`image/test-ladder.svg`（本仓已落盘）。

## 3. 明确不做

- 不换/不升级 QEMU，不改 `Makefile` 里的 `qemu:` 目标形态。
- 不让自动回归切宿主默认设备。
- 不为可测性拆 `hda_capture.c`/`hda_stream.c` 的结构。
- 不在单测层伪造 dosfs 或测 WAV 头解析。
- 不写自有断言库（允许引入第三方测试框架）。

## 4. 待定问题

1. ~~测试框架选型~~ → 已定：Unity（见 §6）。备选退化路径：50 行手写断言宏。
2. ~~假 codec 状态机形态~~ → 已定：dump 文本为唯一事实源，脚本生成 C 常量表（见 §6）。
3. **60s 预算内整机冒烟能跑几次**：`none` 后端单次约 10s，是否允许占 3–5 次。
4. **qtest 层如何制造中断/定时**：沿用轮询 BCIS，还是显式注入；这决定"中断语义"是否真的被覆盖。
5. **与现有 `hda_tests/` 六维套件的重叠部分**是否合并、还是保留双轨一段时间。

## 5. 目标平台事实（2026-10-27 补充）

最终目标是**把 epos 跑在真机上验证实践效果**，设备与接法见工作区 `fixture-plan.html`：

- 治具机：二手 Dell **OptiPlex 7020 / 9020 SFF**，BIOS 需 `SATA Operation = ATA`、`Serial Port = COM1`、关 Secure Boot、开 Legacy/Legacy OPROM。
- 启动盘：闲置 2.5" SATA 硬盘/SSD，整盘写入 `hd.img`（Win32DiskImager / Rufus dd 模式，不能只拷文件）。
- 观测：**USB-TTL（CH340，全新）接 COM1** 收日志；判定先落在串口文本 + VGA 大字，继电器与气动件推迟。
- 激励：`line-out ↔ line-in` 用 3.5mm 公对公线做电气回环；声学件（参考麦、USB 音频接口、隔音箱）推迟。
- DUT：首版用 QEMU `-device hda-duplex` 顶替被测件。

已核对的两个事实：
1. **串口可用**：`kernel/machdep.c` 的 `printk` 直接 `outportb(0x3F8, c)`，轮询写无中断，真机 COM1 能出日志。
2. **真机第一阻塞不是测试，是播放侧硬编码**：`kernel/hda.c` L183–225 走"QEMU 的 `AUDIO_WIDGET_CAP` 全返回 0 → 硬编码 DAC=0x02 / Pin=0x07 / Mixer=0x05"分支，Realtek codec 拓扑不同，不改成就地一句 verb 都发不对。该项必须排在真机验证之前。

## 6. 测试栈选型（2026-10-27 定稿，用户确认）

**选定：方案 A —— 三段式混合（宿主单测 + QEMU 整机 + 真机半自动 HIL）**

选它的决定性理由：真机只有一台、可自动化通道只有串口、时间不着急——因此"判定必须机器可读、动作必须尽量人工"，而 profile 机制把"未知的 Realtek 拓扑"变成可增量补的数据，不必改驱动代码。

被否的两套：
- **方案 B（只做单测 + 真机，砍 QEMU 整机）**：改一行要等一次写盘 + 重启，直接违反 D1。
- **方案 C（全套自动化治具：可控电源 + USB 音频接口 + 参考麦 + 继电器 + 隔音箱）**：多 ¥1700–3400 与 ACPI/MCR/声学校准工作量，与"先验实践效果"不匹配；留到阶段 2 再评估。

四个子选型：

| 子项 | 结论 | 理由 |
|---|---|---|
| 断言框架 | **Unity**，vendor `unity.c/h` 进 `tests/` | MIT、单文件、MSYS 下 `gcc` 直编，不引包管理器 |
| codec profile | **dump 文本为唯一事实源**，`tools/codec_profile.py` 生成 `codec_profile_{qemu,realtek}.c` | epos 无 JSON 库，C 侧不解析文本；生成 C 表零运行时成本 |
| 串口判定协议 | **`KEY=VALUE` 单行**，如 `CAPTEST PASS bytes=176400 overruns=0 peak=231`，人类日志另起行 | 无 JSON 库、串口速率低，宿主正则即可抓 |
| 重启策略 | 第一版**人工按电源**；要无人值守再加 USB 可控插座 | 避免为"关机"去实现 ACPI |

阶段计划：
1. **阶段 0（现在，不需要真机）**：播放侧发现逻辑从硬编码改为按能力遍历；Unity 单测骨架 + QEMU 型 profile；把 0xF000 方向位、长格式连接列表、无回读校验三个 bug 固化成断言。
2. **阶段 1（真机到货）**：真机 dump 真实拓扑 → 生成 Realtek profile → 单测先绿 → 跑 3.5mm 电气回环基线。
3. **阶段 2（声学件到位）**：麦克风、隔音、声压，届时再评估方案 C 的能力。

## 7. 下一步

阶段 0 的第一刀：`kernel/hda.c` 播放侧由硬编码拓扑改为按 `AUDIO_WIDGET_CAP` / `CONNLIST_LEN` / `GET_CONNECTION_LIST_ENTRY` 遍历发现 DAC 与输出 Pin（采集侧的 `cap_conn_index` 已有可复用的短/长格式定位逻辑）。

交接：下一步可以跑 **prove-it**，验收标准 ——(a) 播放侧拓扑发现对 QEMU 与 Realtek 两套 profile 都得出正确 DAC/Pin；(b) 三个已知 bug 各有一条会红的断言；(c) `make test` 在 2 秒内跑完，(d) 真机串口能打出一行 `CAPTEST PASS ...`。

## 8. 定点分析（DSP）选型与自检层（2026-10-27 定稿）

**选定：用户态分析，内核只供样本（原方案 B）。** 被否：A（内核 Q15 定点 FFT）—— 内核堆只有 `12 × PAGE_SIZE = 48KB`，1024 点复 FFT 的缓冲+表占 12KB，且内核无图形驱动（画点能力在 `userapp/graphics.c`）；C（内核 Goertzel + 用户态 FFT）—— 两处口径易漂。

三条子结论：
- **指标口径**：时域 3 参数最小二乘拟合基频 + 逐次谐波剥离，与 `tools/audio_metrics.py` 完全一致。不用 FFT 取峰算 THD+N（Hann 窗积分旁瓣泄漏会给纯数字正弦约 1.8% 的假 THD）。
- **频谱**：10 fps，N=1024（44.1kHz 下 43Hz/bin），Hann 窗。
- **“定点”不作硬要求**：分析在用户态，FPU 可用（`TEST_TONE` 已用 `sin/cos`）；真机跑通后再评估是否值得为“纯整数链路”付出精度代价。

**层数 4 → 5**：新增**目标机启动自检**（依据 OSDev `Unit_Testing` 的 Run-Time Self-Tests：分配器往返、信号量握手、tick 前进、先打印测试名）。它不依赖 GUI 与人工，是治具机上唯一能每天自动跑的回归。

**外部资料核对（OSDev wiki）对代码的四条要求**：
1. 播放侧拓扑发现按 `Intel_High_Definition_Audio` 的算法：从接了输出设备的 pin 出发，沿该 widget 的 association/connection list 走，命中 DAC（或输入 pin）即存路径；`AUDIO_WIDGET_CAP` 返回 0 时退化为连接列表 DFS。
2. **ICS（immediate command）与 CORB/RIRB 不可混用**，前者只作探索期手段；真机批量遍历 codec 时应评估换 CORB/RIRB。
3. 初始化缺项：**EAPD 开外部功放**（步骤 11）、mic 的 **VrefEn**、jack 的 **Unsol**（步骤 9）。
4. 流数量必须读 **GCAP 的 ISS/OSS**，不能假设 4 进 1 出。

真机无声时的检查顺序：EAPD → VrefEn → Unsol/jack → GCAP 流数 → codec dump 对照。

详细测试方法（分层、断言、黄金向量、反向门、证据规范）见 `docs/test-plan-audio.md`。
