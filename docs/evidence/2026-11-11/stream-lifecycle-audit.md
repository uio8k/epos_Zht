# 连续流生命周期审计（起流 / 停流 / 重启）

日期：2026-11-11。触发：用户问「播放完成后会停下吗」，核查 `userapp/main.c` 的 `audio_stop()` 覆盖情况后扩展为全链路审计。
范围：`kernel/hda_stream.c`（播放连续流）、`kernel/hda_audio.c`（播放 API 层）、`kernel/hda_capture.c`（采集流）、`userapp/main.c` 与 `userapp/melody_player.c`（用户态调用点）。
一手依据：`docs/evidence/2026-11-11/gitee-qemu-sdctl-run-excerpt.txt`（QEMU `intel-hda.c:577-604`、`intel-hda-defs.h:131-147`）。

## 1 起流与停流的唯一入口

| 方向 | 入口 | 状态变量 | 置位点 | 清零点 |
|---|---|---|---|---|
| 播放 | `hda_cont_start`（`kernel/hda_stream.c:446`） | `g_cont_on` | `kernel/hda_stream.c:552`（**唯一**） | `kernel/hda_stream.c:707`（空格式早退）、`:753`（正常停流） |
| 采集 | `hda_capture_start`（`kernel/hda_capture.c:280`） | `g_cap_on` | `kernel/hda_capture.c:415`（**唯一**） | `kernel/hda_capture.c:502`（`hda_capture_stop`） |

[实测: `grep -n "g_cont_on" kernel/hda_stream.c` 仅 552 行写 1；`grep -n "g_cap_on" kernel/hda_capture.c` 仅 415 写 1、502 写 0]

播放流的两条上层入口：

| 上层函数 | 调用 `hda_cont_start` 时传入的环大小 | 位置 |
|---|---|---|
| `hda_cont_play_note` | `HDA_CONT_BUF_SIZE_NOTE` = 512KB | `kernel/hda_stream.c:604` |
| `sys_audio_play_file_stream_ms_impl` | `HDA_CONT_BUF_SIZE_STREAM` = 4MB | `kernel/hda_audio.c:385` |

`hda_cont_play_note` 的四个调用者全部在 `kernel/hda_audio.c`：`:317`（文件播放截断分支）、`:329`（文件播放直通分支）、`:561`（多路混音）、`:612`（PCM 直放）。

## 2 用户态起播点全表（`main.c` / `melody_player.c`）

| 行号 | 所调 API | 是否启流 | 之后是否 `audio_stop()` |
|---|---|---|---|
| `main.c:240` | `audio_play_pcm_ms` | 是（512KB） | 是（`:251`） |
| `main.c:282` | `audio_play_pcm_ms` | 是（512KB） | 是（`:297`） |
| `main.c:329` | `audio_play_pcm_ms`（restart 会话 1） | 是（512KB） | 是（`:330`） |
| `main.c:333` | `audio_play_pcm_ms`（restart 会话 2） | 是（512KB） | 是（`:334`） |
| `main.c:376` | `audio_play_pcm_ms`（sequence 8 段） | 是（512KB） | 是（`:416`） |
| `main.c:412` | `audio_play_pcm_ms`（sine/impulse/step） | 是（512KB） | 是（`:416`） |
| `main.c:742`（`cap_play_tone`，`TEST_CAPTURE` 分支） | `audio_play_pcm_ms` | 是（512KB） | 是（`:816`，本轮新增） |
| `main.c:891` | `audio_play_file_stream_ms` | 是（4MB） | 是（`:895`） |
| `melody_player.c:89` | `audio_play_voices_ms` | 是（512KB，经 `kernel/hda_audio.c:561`） | 是（`main.c:856`，`SORT_DEMO` 分支） |
| `melody_player.c:66/69/107` | `audio_play_silence_ms` | **否**（`kernel/hda_stream.c:662` 首行即 `if (!g_cont_on) return 0`） | 不适用 |

注：`main.c` 行号为本轮两批编辑后的实际值（`restart` 块 44 行插在 `TEST_SIGNAL_underrun` 之后，`TEST_CAPTURE` 分支 5 行加在 `main()` 内），与 2026-11-11 早期的行号不同。

## 3 发现

### F1 采集测试分支起流不停（**本轮已修**）

修复前：`TEST_CAPTURE` 分支执行 `run_capture_test()` 后直接 `while (1) ;`，全流程无 `audio_stop()`。`run_capture_test()` 内部的 `cap_play_tone`（`userapp/main.c:742`）会经 `hda_cont_play_note` 把 512KB 播放流置 RUN。

修复前实测证据（`docs/evidence/2026-11-11/p10-probe-capture.log`）：

- `:237` `HDA: continuous stream started (CBL=524288, 44100Hz 2ch 16bit)`
- 全文检索 `continuous stream stopped`：0 命中

后果：程序停在 `while(1)` 后 DMA 继续在 512KB 环内循环。环内除已播的音符数据外，其余为 `hda_cont_start` 的 `memset` 零区与 `fill_ahead` 预填静音，故听感为静音循环；但流状态与硬件 RUN 位一直挂着，属状态泄漏。`hda_capture_stop()` 只复位输入流（`kernel/hda_capture.c:501` 的 `cap_stream_reset` 作用于 `sd_in`），不涉输出流。

修复：`userapp/main.c:815-816` 改为 `run_capture_test(); audio_stop();`，并在上方加 4 行注释说明为何两者不能互相代劳。

修复后实测（`tests/build/l3_capture.log`）：`:246` `HDA: continuous stream stopped (tail=250 ms, lpib_max_step=180444, wait=poll bcis=0, underruns=0)`；该腿新增一条断言锁定此行为（`tests/full_run.py:114`），CAPTURE 腿 5/5 PASS。

### F2 停流后重启不重置 `g_cont_data_end`（**本轮已修 + 反向验证**）

`hda_cont_start` 的初始化段（`kernel/hda_stream.c:552-568`）原先复位了 `g_cont_on / g_cont_wrpos / g_cont_rdpos / g_cont_lpib / g_cont_wraps / g_cont_rate / g_cont_ch / g_cont_bits / g_cont_fifo_errors / g_cont_desc_errors / g_cont_note_id`，**唯独漏掉 `g_cont_data_end`**。

而停流时 `sys_audio_stop` 把它写成本次会话总写入量（`kernel/hda_stream.c:715` `g_cont_data_end = g_cont_wrpos;`），该值可远大于一个环（220s 曲目约 19.4MB vs 4MB 环）。

推演路径（修复前）：
1. 重启后 `hda_cont_reserve` 取 `start = g_cont_data_end`（`kernel/hda_stream.c:357`）→ 落在环外的绝对偏移；
2. `start < g_cont_rdpos + HDA_CONT_MARGIN` 不成立（新 `rdpos=0`），不前移；
3. 空间等待条件 `start + total + safety > g_cont_size` 恒真，`need` 被算成数 MB；
4. `wait_until(need)` 等一个不可能到达的位置，连续 3 轮零进展后记 `WAIT_TIMEOUT` 并继续；
5. 写入落在 `start % g_cont_size`，与 DMA 从 LPIB=0 起的读取轨迹错位。

修复：`kernel/hda_stream.c:554-557` 在初始化段补 `g_cont_data_end = 0;`（该行实际落在 `:557`）并加注释说明会话起点三坐标必须同时归零。

**反向验证（同一镜像、同一入口、仅差这一行）**：新增 `TEST_SIGNAL=restart` 模式（`userapp/main.c:307-350`）——同一镜像内跑两轮独立会话（800ms → stop → 200ms → stop），比较两次 `audio_get_position_ms()`。这个判据不是随手取的：会话坐标残留时，第二轮 DMA 必须先播完那段陈旧区段才听到本音，`s2` 必然反超 `s1`。

| 版本 | session1 | session2 | VERDICT | 证据（行号按 Select-String） |
|---|---|---|---|---|
| 修复前（回退该行后重编） | 1050 ms | **1500 ms** | **FAIL** | `p12-restart-before-fix.log:295-297` |
| 修复后 | 1050 ms | **452 ms** | **PASS** | `p12-restart-after-fix.log:297-299` |

`session1` 两版一致（1050 ms），说明反向验证只动了这一个变量，不是环境漂移。修复后 `s2=452ms ≈ 200ms 本音 + 250ms 尾静音`，与设计语义吻合。

**直接证据不止于时长读数**——trace 里能看到会话坐标本身：

| 版本 | 第二轮 `NOTE_SUBMIT` 起点 | 说明 | 出处 |
|---|---|---|---|
| 修复前 | `s=201644` `rd=0` | 起点沿用第一轮 data_end（201644），而 DMA 已从 0 起重读 → 写入落点与被读位置错位 | `p12-restart-before-fix.log:283` |
| 修复后 | `s=0` `rd=0` | 会话坐标归零，写入从环首开始，与 DMA 轨迹对齐 | `p12-restart-after-fix.log:283` |

回归入口：`tests/full_run.py:120-133` 新增 L3-D RESTART 腿（3 条断言），并入 `make full`。实跑 `make full` 5 腿 5 通过 0 失败 55.4s exit=0（`p12-make-full-final.log`）。

回归入口：`tests/full_run.py:120-133` 新增 L3-D RESTART 腿（3 条断言），并入 `make full`。实跑 `make full` 5 腿 5 通过 0 失败 55.4s exit=0（`p12-make-full-final.log`）。

### F3 中断等待路径不预填静音（latent，机制性）

`hda_cont_fill_ahead()` 只在轮询分支被调用（`kernel/hda_stream.c:327`）；中断分支（`:295-315`，`:295` 为 `if (g_hda_irq_ok == 1)`）睡眠于 `wq_hda`，期间无人预填。而 `fill_ahead` 的职责正是「DMA 越过真实数据末尾后读到静音而非环内旧音频」（`kernel/hda_stream.c:259-263`）。中断模式下若等待跨越 `data_end`，会复播旧数据。

当前不触发：本机 HDA 的 IRQ 11 已被占用，`hda_cont_register_irq` 明确退回轮询（`kernel/hda_stream.c:211-215`），串口实测行 `HDA: IRQ 11 already owned, polling mode`（`docs/evidence/2026-11-11/p10-probe-capture.log`）。故 `g_hda_irq_ok` 恒为 0，中断分支在本环境不可达。

### F4 起流失败的早退路径干净（无问题，记录以闭环）

`hda_cont_start` 的三个失败出口——`hda_ensure_dma_buf` 失败（`:479`）、BDL 分配失败（`:488`）、BDL 校验失败（`:517`）——都发生在 `g_cont_on = 1`（`:552`）之前，不留下「已置位但流未跑」的中间态。采集侧同理：`hda_capture_start` 的三条回读失败分支都显式 `cap_stream_reset()` 后才 return（`kernel/hda_capture.c:362`、`:393`、`:403`），且 `g_cap_on = 1` 在 `:415`，全部早退都在其之前。

### F5 trace 事件名表缺 CAP_* 三个枚举，采集事件全被打成 UNKNOWN（**本轮已修**）

`kernel/hda_internal.h:144-146` 定义了 `HTEV_CAP_START / HTEV_CAP_OVERFLOW / HTEV_CAP_STOP` 三个枚举，但 `hda_trace_ev_name` 的 switch（`kernel/hda_trace.c:32-48`）没有对应 case，全部落到 `default: return "UNKNOWN";`。

发现过程：本轮给采集分支补上 `audio_stop()` 后，`[TRACE] begin` 段首次在采集腿里出现（此前无 `audio_stop` 就没有 dump），随即暴露该缺陷——`tests/build/cap_verify.log` 内 6 行 `ev=UNKNOWN`，其 `s` 字段分别是 262144 / 180224 / 524288 等，正是采集环形缓冲大小与交付字节数，可确认是 CAP_* 事件。

修复：`kernel/hda_trace.c:46-48` 补三个 case。修复后同一腿的事件名统计为 `CAP_START×3 / CAP_STOP×3 / NOTE_*×3 / REALIGN×2 / STREAM_START×1 / STREAM_STOP×1`，UNKNOWN 归零。

注：该缺陷不影响音频功能，只影响诊断可读性；但它会掩盖采集侧的时序问题，属「补了停流才看得见」的连带修复。

## 4 结论

- 用户问的「播放完成后会停下吗」：默认演示与 `SORT_DEMO` 都有显式 `audio_stop()`，会停；`TEST_CAPTURE` 分支原本不会——F1，本轮已修并加断言锁定。
- `audio_stop()` 的语义边界：它停的是**播放输出流**，不碰采集输入流；采集侧有自己的 `audio_capture_stop()`。两者互不代劳——这正是 F1 的成因。
- F2 不是「起了不停」，而是「停了再起就错」，影响面比 F1 大：它会让第二次会话的每次 append 都白等 3 轮超时、写入落进陈旧区段。本轮已修，且有反向验证（同一镜像回退一行即 FAIL）。
- F3 / F5 是「中断模式下才暴露」与「补了停流才暴露」的 latent 项。F5 已修；F3 因本环境 HDA IRQ 被独占、中断分支不可达，无法用实跑证伪，留档待有中断条件时复核。

## 5 本轮改动清单

| 文件 | 改动 | 性质 |
|---|---|---|
| `userapp/main.c:815-816` | `TEST_CAPTURE` 分支补 `audio_stop()` + 4 行注释 | F1 修复 |
| `userapp/main.c:307-350` | 新增 `TEST_SIGNAL=restart` 双会话模式 | F2 的验证手段 |
| `kernel/hda_stream.c:554-557` | 起流初始化段补 `g_cont_data_end = 0;` | F2 修复（1 行 + 注释） |
| `kernel/hda_trace.c:46-48` | 补 `HTEV_CAP_*` 三个事件名 case | F5 修复 |
| `tests/full_run.py:120-133` | 新增 L3-D RESTART 腿（3 条断言） | F2 回归锁定 |
| `tests/full_run.py:128-129` | CAPTURE 腿补停流断言 | F1 回归锁定 |
