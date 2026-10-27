# Intel HDA 音频驱动（kernel/hda*）

本项目实现基于 epos 内核的 Intel HD Audio 播放驱动，面向定制版 QEMU 3.1.50（i386）：整曲 WAV
流式播放、音符/和声序列、PCM 直投，全程 DMA、零浮点。本文档覆盖文件职责、缓冲
模型、用户态接口、trace 事件、验证基线与已知限制；构建与运行环境细节另见
`docs/运行环境.md`。**全部结论来自实测**（LPIB 行为与标准 QEMU/真硬件不同，见
「已知限制」）。

## 文件与职责

| 文件 | 职责 |
|---|---|
| `hda.c` | 控制器初始化、codec verb 通信（IC 寄存器，绕过 CORB DMA）；DMA 缓冲分配与复用（自 4MB 物理地址起，QEMU 3.1 的 PCI DMA 读不到 1MB 以下）；`wbinvd` 刷缓存保证 DMA 可见性；音量（Q8 定点增益，100=1.0） |
| `hda_stream.c` | 连续流核心：BDL/双档环缓冲、双指针推进、reserve/append/finish、`fill_ahead` 预填静音、BCIS 中断（`isr_hda`）与有界轮询回退、underrun 自检、停流与 `sys_audio_stop/silence_ms/get_*` |
| `hda_audio.c` | WAV 解析（`hda_wav_open/hda_read_wav`）与文件播放：整文件、256KB 分块流式（内存占用恒定）、多音轨和声、内存 PCM |
| `hda_internal.h` | 驱动内部接口、环缓冲尺寸常量、trace 事件枚举 |
| `hda.h` | 用户态 API（全部 `sys_audio_*` 原型；`machdep.h` 另有一份重复声明） |
| `hda_trace.c` | 调用链 trace 环形缓冲；停流时经串口以 `[TRACE]` 前缀导出 |

## 构建 / 运行 / 测量

```bat
make                                    :: 编译内核+用户程序
make hd.img                             :: 产出镜像
tools\rebuild_test.bat <signal>         :: TEST_MODE 测试信号版（构建+静态校验）
tools\run_headless.bat wav <out.wav> "<完成标记>" <超时秒>
```

- 测试信号（`TEST_MODE=1` 编译期选择）：`sine` 440Hz 纯音 / `impulse` 单样本冲激 /
  `step` 中段直流平台 / `sequence` 8×200ms 短音（音符边界）/ `tempo` 节拍抖动 /
  `underrun` 欠载注入。完成标记：测试信号 `Test tone finished...`，默认演示
  `Demo finished...`。
- 串口日志落在 `userapp/serial.log`，每次运行被覆盖，要留档请先复制。
- **monitor 4444 单实例**：`run_headless.bat` 硬编码该端口，并发两个 QEMU 会失败。
- 自动化测试集：仓库外 `../hda_tests/`（不入库；`run_all.bat --suite static|fast|full`）；
  驱动评审报告在 `.codebuddy/hda-review/reports/`（会话数据，不随仓库分发）。

## 用户态接口（syscall 189–198，`include/syscall-nr.h`；machdep.c 分发）

| # | 名称 | 说明 |
|---|---|---|
| 189 | `audio_play_file_ms` | 整文件读入播放（`max_ms` 截断） |
| 190 | `audio_play_silence_ms` | 追加静音 |
| 191 | `audio_stop` | 停流（含按数据量推导的补排与 trace 导出） |
| 192 | `audio_play_voices_ms` | 多文件同时播放（和声） |
| 193 | `audio_set_volume` | Q8 软增益 |
| 194 | `audio_get_errors` | fifo/desc/underrun 计数（低 16 位 fifo，高 16 位 desc） |
| 195 | `audio_get_position_ms` | 播放位置 |
| 196 | `audio_play_pcm_ms` | 内存 PCM 播放（测试信号路径） |
| 197 | `audio_get_underruns` | underrun 计数；dump≠0 时导出 `[TRACE]` |
| 198 | `audio_play_file_stream_ms` | 分块读盘流式播放（256KB/块，内存占用恒定） |

错误码：`-2` WAV 格式不支持（如 8-bit）；`-3` 连续流已 RUN 且参数与现行流不一致
（幂等语义：参数一致返回 0；不一致记 `STREAM_REJECT`，调用方须先 `audio_stop` 再以
新参数启动。跨模式混用 4MB 流 ↔ 512KB 音符即此路径，属设计行为）。错误码经
syscall 原样传回用户态。

## 缓冲模型（连续流）

- **双指针语义**：`g_cont_data_end` 单调递增，是 underrun 判定基准；`g_cont_wrpos`
  是环上物理写游标。二者分离是正确性的根基。
- **两档环缓冲**：流式 4MB（BDL 256×16KB，条目数恰为 LVI 8 位上限 256）；
  音符 512KB（128×4KB）。
- **前瞻与余量**：`HDA_CONT_MARGIN` = 16384B（立体声 ≈93ms / 单声道 ≈186ms 的
  提交前瞻）；`HDA_CONT_WRITE_SAFETY` = 2883584B（= 环 4MB − 单块 256KB −
  在途目标 1MB）。
- **空间等待**：`reserve` 在写入会越过 `rdpos + size − safety` 时等待读位置推进，
  为「有进展即续等」的有界重试（连续 3 轮零进展才判 DMA 停摆并留下界兜底）；写只在
  `rd ≥ need` 时发生 → 在途量恒 ≤ 环 − safety（1.25MB），不侵入 safety 余量。
- **underrun 判定**：rdpos 越过 data_end。QEMU 不主动通知欠载，驱动自检；越界区间
  由 `fill_ahead` 预填静音，不重放旧数据。
- **QEMU 3.1 特性应对**（均经本机实测）：
  - LPIB 推进快于真实消费 ≈4.8%（4.2–4.8KB/s）→ 停流补排按
    `extra = data_end × 5.5% + 256KB` 推导（220s 曲目 ≈1.33MB；500s 实测 2.69MB，
    与公式吻合），对任意时长有余量；
    串口打印 `HDA: stop drain_extra=… B (data_end=…, pct=…)` 供实测核对；
  - PCI DMA 读不到 1MB 以下物理内存 → DMA 缓冲自 4MB（`HDA_DMA_BASE`）起分配；
  - CORB DMA 读不到低内存写入 → verb 改走 IC（Immediate Command）寄存器；
  - guest 写后须 `wbinvd`（`hda_flush_cache`）方对 QEMU 侧 DMA 可见；
  - e1000 占用 IRQ 11 → BCIS 中断代码在位，实际运行于有界轮询模式
    （`HDA_POLL_SPIN` + 停摆检测回退）。
- **停流有界**：连续 `HDA_STOP_STALL_LIMIT`(3) 轮等待后读位置零进展 → 记
  `STOP_STALLED` 并强制停流，不会无限挂死。

## trace 事件（`[TRACE]`，停流时导出；事件名表与枚举一一对应）

`STREAM_START` / `STREAM_STOP` / `NOTE_SUBMIT` / `NOTE_WRITTEN` / `NOTE_DONE` /
`UNDERRUN` / `REALIGN` / `BCIS_IRQ` / `IRQ_FALLBACK` / `WAIT_TIMEOUT` /
`STREAM_REJECT`（差异位掩码：1=rate 2=ch 4=bits 8=buf_size）/ `STOP_STALLED`

格式：`[TRACE] begin recs=N lost=M first=T` → 逐条
`t=… ev=… id=… s=… e=… rd=… wr=…` → `[TRACE] end`；`rd/wr` 快照可核算在途量。

## 验证基线与待办

**进度：见 `progress.html`。** 验证数据、完成状态与待办事项统一维护在那份文档里，本文件只描述架构、命令与纪律。

## 测量纪律（改驱动前必读）

- **音频/整曲评测期间宿主必须静默**：任何并发宿主命令都会触发 QEMU 音频定时器
  批量补触发 → 单次取数超 codec 8KB 缓冲 → 溢出丢弃，录音出现段级劣化，**而串口
  `underruns` 仍为 0**。
- 因此判定音质回归**必须做录音逐段相关性分析**，只看串口计数两者都会误判（宿主
  噪声当无回归 / 真缺口被漏掉）。发现异常先静默复跑再定论。
- 调环缓冲/BDL/安全余量/补排百分比前，先读 `hda_stream.c` 顶部参数区注释与
  `docs/运行环境.md` 的缓冲模型与踩坑表——别按"标准 QEMU 行为"推断。

## 工具

- `tools/rebuild_test.bat <sine|impulse|step|tempo|underrun|sequence>`：切测试信号
  重建并静态校验
- `tools/run_headless.bat`：无头 QEMU + 录音 + 完成标记退出（monitor 4444 单实例）
- `tools/eval_audio.py` / `tools/audio_metrics.py`：SNR / THD / 爆音分析
- 自动化测试集：仓库外 `../hda_tests/`；评审报告：`.codebuddy/hda-review/reports/`
