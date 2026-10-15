# Intel HDA 音频驱动

面向 QEMU 3.1.50（i386）的 Intel HD Audio 播放驱动。本文档覆盖文件职责、连续流
缓冲模型、用户态接口、trace 事件、验证基线与已知限制；构建与运行环境细节见
`docs/运行环境.md`。

## 文件与职责

| 文件 | 职责 |
|---|---|
| `hda.c` | 控制器初始化、codec verb 通信（IC 寄存器）、DMA 缓冲分配与复用、音量设置 |
| `hda_stream.c` | 连续流核心：BDL/环缓冲、双指针推进、reserve/append/drain、underrun 自检、IRQ 注册与轮询回退、停流 |
| `hda_audio.c` | WAV 解析与文件播放：整文件、分块流式、多音轨同时播放、内存 PCM |
| `hda_internal.h` | 驱动内部接口、环缓冲尺寸常量、trace 事件枚举 |
| `hda.h` | 用户态可见 API（`sys_audio_*` 原型） |
| `hda_trace.c` | 调用链 trace 环形缓冲；停流时经串口以 `[TRACE]` 前缀导出 |

## 缓冲模型（连续流）

- **双指针语义**：`g_cont_data_end` 单调递增，是 underrun 判定基准；`g_cont_wrpos`
  是环上物理写游标。二者分离是正确性的根基。
- **两档环缓冲**：流式 4MB（BDL 256×16KB，条目数恰为 LVI 的 8 位上限 256）；
  音符 512KB（128×4KB）。
- **前瞻与余量**：`HDA_CONT_MARGIN` = 16384B（立体声 ≈93ms / 单声道 ≈186ms 的
  提交前瞻）；`HDA_CONT_WRITE_SAFETY` = 2883584B（= 环 4MB − 单块 256KB −
  在途目标 1MB）。
- **QEMU 3.1 特性应对**（均经本机实测）：
  - LPIB 推进快于真实消费 ≈4.8%（4.2–4.8KB/s）→ 停流补排按
    `extra = data_end × 5.5% + 256KB` 推导（220s 曲目 ≈1.33MB），对任意时长有余量；
  - PCI DMA 读不到 1MB 以下物理内存 → DMA 缓冲自 4MB（`HDA_DMA_BASE`）起分配；
  - CORB DMA 读不到低内存写入 → verb 改走 IC（Immediate Command）寄存器；
  - guest 写后须 `wbinvd`（`hda_flush_cache`）方对 QEMU 侧 DMA 可见；
  - e1000 占用 IRQ 11 → BCIS 中断代码在位，实际运行于有界轮询模式。

## 用户态接口（syscall 189–198）

| # | 名称 | 说明 |
|---|---|---|
| 189 | `audio_play_file_ms` | 整文件读入播放（`max_ms` 截断） |
| 190 | `audio_play_silence_ms` | 追加静音 |
| 191 | `audio_stop` | 停流（含按数据量推导的补排与 trace 导出） |
| 192 | `audio_play_voices_ms` | 多文件同时播放（和声） |
| 193 | `audio_set_volume` | Q8 软增益 |
| 194 | `audio_get_errors` | fifo/desc/underrun 计数 |
| 195 | `audio_get_position_ms` | 播放位置 |
| 196 | `audio_play_pcm_ms` | 内存 PCM 播放（测试信号路径） |
| 197 | `audio_get_underruns` | underrun 计数；dump≠0 时导出 `[TRACE]` |
| 198 | `audio_play_file_stream_ms` | 分块读盘流式播放（256KB/块，内存占用恒定） |

错误码：连续流已启动时重复 `start` 为幂等语义——参数一致返回 0；不一致返回
**-3** 并记 `STREAM_REJECT`（调用方须先 `audio_stop` 再以新参数启动）。错误码经
syscall 原样传回用户态。

## trace 事件（`[TRACE]`，停流时导出）

`STREAM_START` / `STREAM_STOP` / `NOTE_SUBMIT` / `NOTE_WRITTEN` / `NOTE_DONE` /
`UNDERRUN` / `REALIGN` / `BCIS_IRQ` / `IRQ_FALLBACK` / `WAIT_TIMEOUT` /
`STREAM_REJECT`（差异位掩码：1=rate 2=ch 4=bits 8=buf_size）/ `STOP_STALLED`

## 验证基线（2026-10-15，代码基线 `56b1903`，宿主静默）

- sine 440Hz：THD+N 0.00170%、频率偏差 0.00000%、零削波、零高置信爆音
- 整曲 220s 流式：11/11 段相关性 1.000000、对齐零漂移、残差 ≈1.3e-4、
  underruns=0；停流收敛 rd ≥ goal（精确达成）
- QEMU（轮询模式）宿主 CPU / 墙钟 ≈ 0.76

**测量纪律**：音频评测期间宿主必须静默——并发负载会触发 TCG 定时器批量补触发，
造成录音段级劣化而串口 `underruns` 仍为 0；音质判定必须基于录音分段分析，
不可只看串口计数。

## 已知限制与待办

- P2-3：IRQ 模式的 `wait_until` 分支缺 `fill_ahead`（真硬件路径；当前 QEMU
  轮询模式不受影响）
- P2-4：audio syscall 无互斥（单任务 userapp 下无碍）
- P3-5：`reserve` 空间等待超时后继续写（实测侵入设计上限 ≈1.2MB，由 ≈2.75MB
  物理余量兜底，无伪影；代价是流式后半程每块一次无效自旋）
- 跨模式混用（4MB 流 ↔ 512KB 音符）会被 -3 拒绝（设计行为，见错误码）

## 工具

- `tools/rebuild_test.bat <sine|impulse|step|...>`：切测试信号重建并静态校验
- `tools/run_headless.bat`：无头 QEMU + 录音 + 完成标记退出（monitor 端口 4444，
  同时仅允许一个实例）
- `tools/eval_audio.py` / `audio_metrics.py`：SNR / THD / 爆音分析
- 自动化测试集：仓库外 `../hda_tests/`（不入库；static / fast / full 三套件）
- 驱动评审报告：仓库外 `.codebuddy/hda-review/reports/`
