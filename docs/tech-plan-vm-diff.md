# HDA 第二实现差分技术方案：P13（VBox HD Audio × QEMU 双模拟器）—— 2026-10-31 调研落盘轮

本文档是 2026-10-31 调研轮产出：只写方案与取证，不改内核代码、不改测试、不跑构建。
实施按下轮 §8 排期。基线：内层 x86 分支 `f874c2a`（P09 已提交）。

**路线修正声明**：原定路线「Bochs 差分交叉验证」在本轮调研中被一手证据证伪——Bochs
官方任何版本（至 3.1，2026-08-23 发布）均无 Intel HD Audio 设备仿真（§3 六路证据）。
修正为 **VirtualBox（HD Audio 控制器）× QEMU 3.1.50 双模拟器差分**，Bochs 证伪结论
与其残值价值（引导级第三对照，可选）一并记录。

配套取证（均落 `docs/evidence/2026-10-31/`）：

- `baidu-bochs-hdaudio.log` / `baidu-bochs-download-cn.log` —— 百度 MCP 检索原文（用户点名通道）
- `websearch-snapshot-20261031.log` —— 计划阶段 WebSearch 快照（含失实的「Bochs 2.7 加 HDA」摘要，§3 纠正）
- `gitee-bochs-mirror-search.log` —— Gitee 官方镜像无 bochs 定案（curl 405 不可作数，浏览器实查 404）
- `bochs-sf-reachability.log` —— SourceForge 直连可达 + TUNA/USTC 收录实查
- `spec-bochs-manual-excerpt.txt` —— bochs 手册/源码树/CHANGES/发布公告摘录（证伪主证据）
- `spec-vbox-manual-excerpt.txt` —— VBox 手册摘录（HD Audio 控制器、串口 Raw File、录制）
- `osdev-bochs-emulator-comparison-excerpt.txt` —— osdev 三页浏览器摘录

## 1 范围

只覆盖 P13：L3 层之上的第二实现差分（整机串口行为 + 音频内容）。L1（make test）、
L2（make qtest）不动；QEMU 3.1.50 不动（D8）；真机相关 L4/L5 排期不变（§10 局限）。

## 2 依据等级与采信规则

- `[spec: 出处]` —— 厂商官方文档逐字摘录（VBox 手册、bochs 手册），已落
  `spec-vbox-manual-excerpt.txt` / `spec-bochs-manual-excerpt.txt`。
- `[src: 仓库@分支]` —— 一手源码树摘录。Bochs 源码树 = github bochs-emu/Bochs@master
  （官方站自述源码树 GIT 托管 github；Gitee 无官方镜像已定案，见
  `gitee-bochs-mirror-search.log`），经 jsdelivr CDN 拉取。
- `[wiki: 页名 + 2026-10-31 浏览器摘录]` —— osdev wiki 三页（curl 被 Cloudflare 拦，
  真实浏览器通道读取，无 oldid 快照，页面为当前版）。
- `[实测: 证据文件[:节]]` —— 本机命令与探测的原始输出。
- `[检索: 证据文件]` —— 百度 MCP / WebSearch 检索快照（**本轮新增标记**：检索摘要
  只作线索不作结论，结论必须落到前四类一手证据；§3 即一例）。
- 与实测冲突时以实测为准并显式列出；不确定项全部在 §11 单列。

## 3 证伪记录：Bochs 官方无 Intel HD Audio（本轮关键产出）

| # | 证据 | 结果 |
|---|---|---|
| 1 | master 源码树文件树 `bochs/iodev/sound/` 全清单 `[src: bochs-emu/Bochs@master]` | 只有 sb16/es1370/opl + 宿主驱动（soundwin/soundfile 等），**无 hdaudio 设备源码** |
| 2 | 现行文档 `bochs/doc/man/bochsrc.5` 与示例 `.bochsrc` | `hdaudio` 0 命中 |
| 3 | CHANGES 全文（覆盖 2.7 → post-3.1，头部自述更新至 2026-10-28） | `hdaudio` 0 命中；2.7 Sound 节原文 = "Sound: ported ES1370 soundcard emulation from Qemu" |
| 4 | 官方发布公告 RSS（2.7/2.8/3.0/3.1） | `hda` 0 命中 |
| 5 | 官方 Win64 构建配置 `.conf.win64-vcpp` | 无 `--enable-hdaudio` |
| 6 | osdev wiki Emulator_Comparison 硬件表 Sound 列 `[wiki: Emulator_Comparison]` | Bochs = SB-16 一项 |

结论：Bochs 只支持 SB16 / ES1370 / PC speaker。**「Bochs 差分验证 HDA 驱动」不可行。**

教训（写入流程）：检索摘要可直接失实——本轮计划阶段的 WebSearch 快照明确写了
"Bochs 2.7 added Intel HD Audio device emulation / hdaudio 设备 + --enable-hdaudio"
（`websearch-snapshot-20261031.log` 检索 1），与一手源码相反。AGENTS.md 的「结论必须
带一手出处」规则是必要的；今后凡能力类结论一律先过 `[spec:]`/`[src:]`。

Bochs 残值（可选，不排期）：指令级仿真 + 内置调试器对**引导/内核级**问题（三重
错误、寄存器被谁改写）仍是第三只眼；`com1: enabled=1, mode=file` 串口落盘可用
`[spec: bochsrc.html §4.3.28]`。但设备层无 HDA，对音频栈差分无贡献，优先级低。

## 4 现状核对（基线 f874c2a）

- L1 `make test` 36 测试 / 5 段；L2 `make qtest` 15 断言（U-L2-01/02）；L3 整机链路
  走 `tools/run_headless.bat`（QEMU 启动 + 串口落盘 + watcher 发 quit）+ `hda_tests`
  的 QEMU 五用例 `[实测: docs/tech-plan-qtest.md §3 既有复核]`。
- 判定行协议 `KEY=VALUE` 单行（decisions §6 定稿），宿主正则可抓，
  `tools/serial_parser.py` 为现成解析器（D11：`audio_metrics.py` 唯一指标源、shell/
  编排层留在 hda_tests）。
- 缺口：QEMU 3.1.50 是唯一参照实现，「QEMU 上绿」只证明 QEMU 兼容；真机无排期
  （decisions §5 治具四件未到位），怪癖风险无兜底。
- osdev 依据：多模拟器互测 "It can't hurt to use more than one emulator…"；同时警告
  "if you test using an emulator, it might not work on the real thing"（差分 ≠ 真机）
  `[wiki: Emulator_Comparison / Testing + 2026-10-31 浏览器摘录]`。

## 5 备选对比矩阵（结论表）

| 选项 | 判定 | 关键依据 |
|---|---|---|
| A. Bochs 差分（原推荐） | **证伪** | §3 六路证据：官方无 HDA 设备 |
| B. VBox HD Audio 差分 | **采纳** | 手册 Audio Controller 含 "Intel HD Audio controller" `[spec: VBox 手册]`；TUNA 国内源直供 `[实测: bochs-sf-reachability.log D2]` |
| C. 升级 QEMU 新版 | 否掉 | 无国内镜像（官方推荐源 qemu.weilnetz.de 德国）`[检索: websearch-snapshot-20261031.log 检索 5]` + 违反 D8 |
| D. Gitee Go CI | 缓做 | 免费额度足够 `[检索: 同上 检索 6]`；但 qtest/整机需自建 Windows 构建节点，等差分主线稳后再接 |
| E. 维持现状 | 不采纳 | 判据 3（不依赖单一实现）继续裸奔，真机又无排期 |

## 6 VBox 差分方案设计（细化）

### 6.1 下载与安装（国内源）

1. 下载：`https://mirrors.tuna.tsinghua.edu.cn/virtualbox/` 目录按版本号分文件夹，
   取最新 7.x 及以上的 `VirtualBox-x.x.x-Win.exe`（具体版本号实施轮 curl 目录列表
   后定，落证据；帮助页 `/help/virtualbox`）`[实测: TUNA /virtualbox/ -> HTTP 200]`。
2. 校验：SHA256 与 TUNA 同目录/官方 checksums 对照，差异即弃。
3. 安装：常规安装器；装 NDIS 网卡驱动时宿主网络会闪断一下（预期）；**不需要**
   扩展包（录制是基础功能，扩展包只管 USB2/3 与远程显示）。
4. 与 Hyper-V/WSL2 共存：若宿主开着 Hyper-V，VBox 7+ 自动走兼容后端（能跑、慢）。
   实施轮先 `systeminfo` 探测 Hyper-V 状态落证据（U7）。

### 6.2 VM 构建命令草案（全程 VBoxManage，可脚本化）

```bat
VBoxManage createvm --name epos-xdiff --ostype Other --register
VBoxManage modifyvm epos-xdiff --memory 48 --firmware bios --boot1 disk ^
  --audio-enabled on --audio-controller hda --audio-driver null ^
  --uart1 0x3F8 4 --uart-mode1 <RawFile 模式，路径=xdiff-vbox-serial.log> ^
  --recording on --recording-audio on --recording-file xdiff-vbox.webm
VBoxManage storagectl epos-xdiff --name IDE --add ide
VBoxManage storageattach epos-xdiff --storagectl IDE --port 0 --device 0 --type hdd --medium hd.vmdk
VBoxManage startvm epos-xdiff --type headless
```

- 磁盘沿用仓库既有链路：`hd.img` → `qemu-img convert -O vmdk` → `hd.vmdk`（根
  Makefile 已有 `hd.vmdk:` 目标，零新增依赖）。
- `--uart-mode1` 的 Raw File 具体参数值、`--audio-controller hda` 的合法枚举、
  `--recording-audio` 的确切写法，实施轮以 `VBoxManage modifyvm --help` 输出为准
  （U2/U1/U4，逐条落证据）——手册只保证能力存在（§5 矩阵），不背具体语法。
- 收尾：`VBoxManage controlvm epos-xdiff poweroff` + `VBoxManage unregistervm
  epos-xdiff --delete`，不留常驻 VM。

### 6.3 差分判定设计（两级规则）

VBox 的 HDA codec 模型与 QEMU `hda-duplex` **不是同一拓扑**（两家各自建模），因此
判定分两级，规则表放 `tools/emulator_diff.py` 内置：

| 级别 | 内容 | 规则 |
|---|---|---|
| 行为级（必须一致） | `AUDIO_VERDICT` 的 PASS/FAIL、播放/采集流程是否走完（`BDL ok`、Demo 结束行）、`CAPTEST` 的 PASS/FAIL 与 overruns=0 | 两侧集合必须相等，不等 = 差分 FAIL，报告逐行列出 |
| 拓扑级（语义等价，不要求逐字节） | widget 地址类字段（DAC=、Pin=、节点号）、bytes/peak 等依赖 codec 实现的数值 | 两侧都出现且语义成立（发现可用 DAC/Pin 路径、bytes 非零）即 PASS；数值差异记入差异表供人工判读 |

- 单侧独有行单独归类 `ONLY-IN-QEMU` / `ONLY-IN-VBOX`，第一版只报告不判失败（VBox
  首启后按实际输出收敛规则表）。
- 解析复用 `tools/serial_parser.py` 的 KEY=VALUE 抓取，不新写解析口径（D11 同思路）。

### 6.4 音频内容校验（第二通道）

1. VM 录制开 `--recording`（含音轨，WebM 容器）`[spec: VBox 手册 Recording 节]`。
2. 音轨提取：`uv run --no-project --with imageio-ffmpeg python ...`（ffmpeg 由 PyPI
   wheel 携带，uv 走清华 PyPI 镜像，满足国内源约束）；`ffprobe` 核采样率/声道。
3. 提取出的 PCM 喂 `tools/audio_metrics.py`（D11 唯一指标源）出 RMS/峰值/时长；
   第一版验收线 = 「音轨非零、时长与播放样本匹配」；SNR/相关性等真值指标**仍归
   真机**（§10），模拟器值只做趋势参考。

### 6.5 落点表

| 文件 | 改动 |
|---|---|
| 根 `Makefile` | 新增 `xdiff:` 目标：转 vmdk → 建/起 VM（headless）→ 等串口收敛 → `tools/emulator_diff.py` 差分 → 清理 VM。不加 watcher、不加独立 bat（D10） |
| `tools/emulator_diff.py` | 新增：读两份串口日志 → 两级规则差分 → markdown 报告 + 退出码 |
| `kernel/`、`userapp/`、`tests/` | **零改动**（差分是外环，被测件原样） |
| `docs/evidence/<日期>/` | 双侧串口原始日志、VBoxManage 命令原文与退出码、音轨指标输出 |

目标名候选 `xdiff`（避开既有 `diff:`——svn diff 包装）。是否并入 `make full` 触发
链：不并入，独立手动触发（见 6.6）。

### 6.6 预算归属（对 D2 的显式补充）

VBox headless 冷启动预计 10–30s（U5 实测），加 QEMU 侧重放，单次差分总计可能超
D2 的 60s 全套预算。设计为**提交前可选第三档**：不进 `make full` 主链、不进日常
迭代路径（D1 快速反馈优先），频率 = 每次提交前跑一次或驱动行为变更时跑。若下轮
实测远快于预期（<20s），再评估并入 full；本轮不预设。

## 7 与 decisions-audio-test.md D1–D12 的相容性核对

| 决策 | 相容性 |
|---|---|
| D1 快速反馈优先 | 相容：差分在提交前档，不占 2s 反馈路径 |
| D2 速度预算 | 补充而非违反：新增「提交前第三档」，§6.6 显式声明 |
| D3–D7 分层/桩/结构 | 相容：kernel/userapp/tests 零改动 |
| D8 不动 QEMU 3.1.50 | 相容：QEMU 原样作第一实现 |
| D9 禁改宿主默认音频设备 | 相容：`--audio-driver null` 优先；Recording 为 VM 内录不碰宿主端点（若 null 下录不到音，见 U4 处置） |
| D10 入口统一 make | 相容：`make xdiff`，无 watcher/独立 bat |
| D11 指标唯一源 | 相容：复用 `audio_metrics.py` 与 `serial_parser.py` |
| D12 最小复现输出 | 相容：差分报告直接给两侧原始行 + 规则名 |

## 8 实施排期（下一轮）

1. 下载安装 VBox（TUNA，SHA256 校验）+ `VBoxManage --help` 探测 U1/U2/U4 语法，
   全部落证据。
2. 首次建 VM 启动 epos：确认引导兼容（U3）、拿到第一份 VBox 串口日志（codec
   dump/枚举行顺带回答 VBox 的 codec 型号，供 6.3 规则表定稿）。
3. `tools/emulator_diff.py` 落地 + 首次差分报告。
4. 录制音轨提取 + `audio_metrics.py` 指标（U4）。
5. `make xdiff` 接入 + progress.html 勾选 P13 + 证据 refs-check。

## 9 验收判据（下轮交差逐条对照）

1. 全程 CLI 可复现：从 createvm 到差分报告一条命令链跑通，无 GUI 依赖，VM 用后即删。
2. 差分报告对现有串口判定行给出两级结论，行为级不一致时退出码非零。
3. 音轨指标输出落证据（非零、时长匹配的第一版验收线）。
4. `make test` / `make qtest` 不退化；kernel/userapp/tests 零改动（`git diff --name-only` 核对）。
5. 证据目录含双侧原始日志 + 命令原文 + 退出码；refs-check 全 PASS。
6. 下载只走国内源（TUNA；PyPI 走清华镜像），如失败按用户约定提出替换方案并留痕。

## 10 局限性（不许被差分掩盖的部分）

- **差分 ≠ 真机验证**：osdev 明言 "if you test using an emulator, it might not work on
  the real thing" `[wiki: Testing]`。Realtek 真实拓扑发现、EAPD/VrefEn/Unsol 三初始化、
  SNR/THD 真值，仍只能等 P01 治具机（decisions §5 阶段 1 排期不变）。
- VBox HDA codec 模型 ≠ QEMU hda-duplex：拓扑级差异可能来自模拟器建模而非驱动 bug，
  判读规则必须先收敛再下结论（6.3）。
- 模拟器差分只能证明「驱动不依赖单一实现的行为怪癖」，不能证明「符合 Intel 规范
  全集」；规范符合性继续靠 [wiki 摘录]/[Gitee] 取证 + L2 寄存器级断言。

## 11 不确定项（每条带下轮验证方法）

- **U1 --audio-controller 合法枚举与 VBox codec 型号**：`VBoxManage modifyvm --help`；
  首启串口的 codec dump 行比对。
- **U2 --uart-mode1 Raw File 参数值语法**：`VBoxManage modifyvm --help`；先手工一条
  VM 试跑确认文件确实落盘再脚本化。
- **U3 epos 引导兼容**（hd.vmdk 几何、BIOS 语义）：首启实测；参照 bochsrc-win.txt 的
  cylinders=65/heads=16/spt=63 与 QEMU 侧 boot 行为差分定位。
- **U4 录制音轨格式与 null 后端可录性**：首次录制后 ffprobe；若 null 后端无音轨，
  评估 `--audio-driver default`（只播放不改宿主设置，不违反 D9）并留证据。
- **U5 headless 冷启动耗时**：计时落证据，决定 6.6 的档位归属。
- **U6 VBox 版本选择**（TUNA 最新 vs 保守 7.1 LTS 思路）：目录列表 + 手册对照后定，
  锁版本号写入 Makefile 注释。
- **U7 宿主 Hyper-V/WSL2 共存模式**：`systeminfo` 探测 + 首启速度对比落证据；若兼容
  模式过慢，向用户报备再定。

## 12 与既有文档的关系

- `docs/decisions-audio-test.md`：D1–D12 全部不违反（§7 逐条核对）；§6.6 对 D2 是
  显式补充，已在文中声明。
- `docs/tech-plan-qtest.md`：L2 体系不变；本文是 L3 层之上的第二实现外环。
- `progress.html`：新增 P13 条目（方案已定）+ 变更记录；未实现与受阻表保持用例
  语义不加行（差分是基建项不是用例，故 P13 条目承载）。
- 外层 `AGENTS.md`：调研规则本轮已实践（百度 MCP + Gitee/osdev + 一手摘录落证据）。
