# ADC 转换器（HDA 输入侧）决策记录

会话日期：2026-10-26。范围、结论、证据、遗留问题一次记全，供后续会话直接接手。
边界以 `image/hda-input-path.svg` 为准：本轮覆盖 codec 内部 `麦克风 Pin -> ADC 转换器`，
并把 ADC 的 `SET_CONVERTER_FORMAT` / `SET_CONVERTER_STREAM_CHANNEL` 绑定含在内；
`输入流 SDI` 之后的 BDL/DMA、环形缓冲、读指针、syscall、用户态功能**未改**（沿用 9ccf800）。

## 1. 已定结论

| # | 结论 | 理由 |
|---|---|---|
| D1 | 交付边界 = Pin 输入使能 + ADC 连接选择 + 输入 amp 取消静音 + ADC format/tag 绑定 + 全部回读校验 | 只做前四项时 ADC 永不产出数据，任何验收都落不了地 |
| D2 | 输入 amp 的载荷必须用 `0x7000`，不能用 `HDA_AMP_UNMUTE_0DB(0xB000) \| (1<<14)` | payload bit15=输出方向、bit14=输入方向，**两者互斥**；`0xB000\|0x4000=0xF000` 被 codec 当成「输出」方向，等于输入 amp 从未取消静音。旧代码正是 0xF000 |
| D3 | 回读校验分级：`pinctrl`/`conn select`/`converter format`/`stream tag` 不符即致命（返回 `-7/-8/-9/-10` 并复位输入流）；`amp` 不符只告警 | HDA 规范允许 widget 不带 amp；把非关键项做成致命会让整条通路被判死 |
| D4 | 回读值 `0` 视为「widget 不支持回读」，只告警并沿用请求值；回读值非 0 且与请求不符时**以回读值为生效值**并写回 SDnFMT | QEMU hda-duplex 的 `GET_CONVERTER_FORMAT`/`GET_CONV` 返回 0，若当成不一致会导致可用通路被误判；真实硬件上若格式被改写，必须两侧一致否则采样率错 |
| D5 | 连接列表定位重写：短格式 4 NID/响应、长格式 2 NID/响应，用显式命中返回值，不再靠 `sel != 0` 判断 | 旧实现长格式直接取索引 0；`if (sel != 0 \|\| (v & 0xFF) == pin)` 在「索引 0 命中」与「完全未命中」两种情形下无法区分 |
| D6 | 验收激励改为**客户机自发**（A 场景里 guest 播 700ms 1kHz、幅度 30000）；宿主只负责设备路由 | 宿主侧 winsound 播放那一路实测不稳定（同一接线时好时坏：一次 median freq=1000/duty 0.70 通过，随后几次只有底噪）；guest 自发使整条回环在 QEMU 内部闭环 |
| D7 | 音调判据用 Goertzel 1kHz 能量比，不用绝对电平/过零估频 | 「板载渲染 -> 立体声混音 -> dsound 采集」实测衰减约 50dB，满刻度激励只有 ±100~200 LSB；过零估频被噪声抬高（同一段数据估出 920/1991/3938Hz） |

## 2. QEMU 与本机能力（实测，勿重复探测）

- QEMU 3.1.50 为**旧音频框架**：`-audiodev` 是 invalid option，配置靠 `QEMU_AUDIO_DRV`。
- 可用后端三个：`none`、`wav`（`-audio-help` 明确写 **Does not support capture**）、`dsound`（**One capture voice**）。
  → **采集只能走 dsound**；这也解释了 9ccf800 提交说明里「QEMU 不能从系统默认麦克风收到声音」。
- dsound 的 capture 在本机是**真的实现了**（不是桩）：`[CAP-A] bytes=180224 / expect=176400`、`lpib` 推进、`overruns=0`。
- QEMU 侧固定：`QEMU_AUDIO_ADC_FIXED_FREQ=44100 / FMT=S16 / CHANNELS=2`。
- QEMU hda-duplex 拓扑（探测所得）：`ADC=0x04`、`MicPin=0x05`、该 Pin 在 ADC 连接列表中的索引 `0`。
- 回读能力：`GET_CONVERTER_FORMAT` 与 `GET_CONV` 返回 `0`（未实现）；`GET_PIN_WIDGET_CTRL` 返回 `0x20`（IN_EN 生效）；`GET_AMP_GAIN_MUTE` 返回 `0x0000`。

## 3. 宿主环境改动（已还原）

数字回环要求「QEMU 输出」与「立体声混音」在同一块声卡上，故测试期需要同时切两个默认端点：

| 端点 | 测试期 | 原值（已还原） |
|---|---|---|
| 播放 | 扬声器 `{a7a1a3c9-f671-45f3-9ddb-255c8f50966a}`（板载 Realtek） | 耳机 `{60490a4c-fcef-4bb1-9728-3f8b8e999149}`（UGREEN CM379 USB） |
| 录音 | 立体声混音 `{bc78f4a1-8a3d-47bc-abe6-e3d9aebc0aaf}`（板载） | 麦克风 `{c5033a49-a259-4034-9dd7-3cb188fa69e2}`（UGREEN CM379 USB） |

切换/还原脚本：`hda_tests/lib/set_default_endpoint.ps1`（IPolicyConfig COM，`-List` 枚举、`-Kind` + `-Id` 切换），
包一层 `lib/loopback_endpoints.bat` / `lib/restore_endpoints.bat`。**跑完必须还原**，否则宿主声音全部走板载。

两个坑：
1. `IPolicyConfig` 接口必须带 `[ComImport]`，否则 QI 以 `E_INVALIDARG` 失败；
2. 本机 `MMDevices\Audio\<Kind>` 的键名只是端点 GUID，`SetDefaultEndpoint` 需要完整 MMDevice ID
   `{0.0.0.00000000}.{guid}`（播放）/ `{0.0.1.00000000}.{guid}`（录音），传裸 GUID 会 `E_INVALIDARG`。

## 4. 验收证据（四道门）

复现：`cmd /c start /b e:\ComputerScience\epos_Zht\hda_tests\run_capture_all.bat`（加 `--reverse` 跑反向门），
结果落 `hda_tests/reports/capture_loopback{,_reverse}.json` 与 `reports/serial_*.log`。

| 门 | 判定 | 实测 |
|---|---|---|
| 静态/编译 | 内核 + 用户程序零告警（仅 `lib/memcpy.c` 既有 `-Wreturn-type`） | `make -C kernel`、`build_capture.bat` |
| 回读门 | probe 逐项回读一致、`dump_state` 一行可读 | `pinctrl=0x20 conn=0 fmt=0x0000(不支持回读) conv=0x00 amp_adc=0x0000 amp_pin=0x0000 sd0_ctl=0x000000` |
| 回环门 | G1_len/G2_idem/G3_tone/G4_level 全 PASS | `A: bytes=180224 expect=176400 diff=+3824 lpib=206148 wraps=0 overruns=0`；`B: bytes=532332 expect=529200 wraps=1 overruns=0`；`C: 0/0/-2/0`；`A METER: toneblocks=10/20 toneduty=50%`、`median_tone=86.5%`、`peak_max=231` |
| 反向门 | 跳过 ADC stream tag 绑定 → 采集静音 | `[CAP-A] bytes=0 lpib=0 peak=0`，`G1/G3/G4 FAIL`，`REVERSE GATE PASS` |

连续两轮标准构建均为 4/4 PASS（tone_duty 均为 0.50，median_tone 76% / 86.5%），可重复。

## 5. 待定问题

1. **真实硬件未验证**：QEMU 对 amp 只回 0（不实现），D2 的 amp 方向修正在真机上是否生效、长格式连接列表分支是否正确，都需要一台真实 HDA codec 才能确认。
2. **非 44.1k/立体声/16bit 未跑**：`QEMU_AUDIO_ADC_FIXED_*` 固定 44.1k，48k 或单声道的实际行为（重采样、通道映射）没有实测；`hda_capture_start` 的格式仲裁只在「回读非 0」时才生效。
3. **时钟漂移未量化**：采集侧没有像播放侧那样记录 LPIB 与实际耗时的偏差；B 场景 `bytes` 比期望多 0.6%，来源未分离（起始/停止边界 vs 真实速率差）。
4. **回环链路 50dB 衰减原因未定位**：可能在宿主「立体声混音」录音电平、Windows 重采样或 QEMU dsound capture 增益，未逐段测量。若后续要测真实采集质量（SNR/THD），必须先解决这一点。
5. **`CAP_ACTIVE_PEAK=20` 是经验门限**，只适配当前这条低电平回环；接真实麦克风时应改回 -44dBFS 量级。
6. 反向门目前只有一个（跳过 tag 绑定）。Pin IN_EN、连接选择这两个开关是否在 QEMU 里也是「承重」的，未做反向验证——QEMU 很可能忽略它们，这点只在真实硬件上才有意义。

## 6. 改动清单

- `kernel/hda.h`：新增回读 verb（`GET_CONVERTER_FMT`/`GET_PIN_WIDGET_CTRL`/`GET_CONNECTION_SELECT`/`GET_AMP_GAIN_MUTE`）、amp 方向与声道位、`HDA_AMP_IN_UNMUTE_0DB(0x7000)`、`HDA_CAP_ERR_*(-4..-10)`。
- `kernel/hda_capture.c`：`cap_verb`（有界重试）、`cap_verify`（分级回读）、`cap_conn_index`（短/长格式定位）、重写 `hda_capture_probe`、`hda_capture_start` 加 format/tag 回读与生效值仲裁、新增 `hda_capture_dump_state`、反向门 `CAP_REVERSE_SKIP_ADC_BIND`。
- `kernel/hda_internal.h`：声明 `hda_capture_dump_state`。
- `kernel/hda.c`：probe 成功后打快照、失败按负码提示（不影响播放路径）。
- `kernel/Makefile`、`Makefile`：`CAP_REVERSE=1` → `-DCAP_REVERSE_SKIP_ADC_BIND` 透传（默认关闭）。
- `userapp/main.c`：`TEST_CAPTURE` 分支加块包络/过零估频/Goertzel 1kHz 能量比、客户机自发 1kHz 激励、波形快照。
- 仓库外（不进版本库）：`hda_tests/lib/build_capture.bat`、`hda_tests/run_capture_loopback.bat`、`hda_tests/run_capture_all.bat`、`hda_tests/tools/capture_loopback.py`、`hda_tests/lib/set_default_endpoint.ps1`、`hda_tests/lib/{loopback,restore}_endpoints.bat`。
