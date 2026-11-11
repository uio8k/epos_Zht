# P05b 删除 SNR/THD 真值指标：技术方案（metrics purge）

日期：2026-11-10（autodev 轮 2）。决策来源 `docs/decisions-qemu-only.md` D-Q5；
调研盘点 `docs/evidence/2026-11-10/p05b-recon.log`；外部依据 [wiki: Testing +
2026-10-31 浏览器摘录]（模拟器层只断言行为级，真值分析留宿主工具）。

## 1 范围与原则

删的是 **guest 侧（userapp + tests fixtures）的 SNR/THD 真值指标链**：
字段、阈值常量、谐波剥离算法、串口判定行字段、直接消费它们的 L1 用例与 fixture
case。**不动**：宿主 `tools/audio_metrics.py`（D11 唯一指标源）、hda_tests 套件
（其 SNR/THD 判定来自宿主 wav 分析，已证不依赖串口行，`p05b-recon.log` §1.1）、
f0/RMS/peak/dc_offset/peak_bin（行为指标，静音判定与频率检查仍需要）。

## 2 落点表

| 文件 | 改动 |
|---|---|
| `userapp/metrics.h` | met_result 删 `thd_pct/thd_n_pct/snr_db`；删 `MET_THD_N_MAX_PCT/MET_SNR_MIN_DB`；`met_verdict(double rms)` 改签名；头注释同步 |
| `userapp/metrics.c` | 删谐波剥离与 THD/SNR 计算段；met_verdict 新实现（rms<1.0 → MET_SILENT，否则 MET_PASS） |
| `userapp/main.c` | `cap_verdict_line`：AUDIO_VERDICT 行 **v=1→v=2**，删 thd_n=/snr= 字段（含两处失败分支）；`cap_show_verdict` 签名删 thd_n/snr，VGA 第二行改 `f0= rms=` |
| `tests/test_metrics.c` | 删 `test_thdn_3rd_harmonic / test_thdn_dc_offset / test_snr_noise_floor` 三条；`met_verdict` 调用改新签名；TOL_THD_DB/TOL_SNR_DB 删 |
| `tests/fixtures/vectors.h` | vec_case 删 3 期望字段 |
| `tests/fixtures/vectors.c` | 删 3 个仅被 THD/SNR 用例消费的 case（sine_1k_plus_h2_h3 / noise_white_seed1234 / dc_offset_100）及其样本数组；保留 sine_1k_0dbfs / silence |
| `tests/fixtures/gen_vectors.py` | 同步删列与 case 生成段（保持「期望值可再生成」链路） |
| `userapp/segment.h` | 注释中 THD 动机句改为行为指标口径（两处） |

## 3 语义变更声明

- **AUDIO_VERDICT 行格式变更**：`v=1`（含 thd_n=/snr=）→ `v=2`（result/f0/rms/peak/silent[/rc]）。套件与 serial_parser 均不解析该行（0 命中），升版本号防旧脚本误读。
- **met_verdict 判定语义**：原「非静音 && THD+N<1% && SNR>60dB」→ 新「非静音即 PASS」。QEMU-only 下 guest 侧判据降为通路活性（信号在+非静音+频率可测），真值门线随真机路线废弃（D-Q5），屏幕/串口不再展示假精度。

## 4 D1–D12 相容核对

| 决策 | 相容性 |
|---|---|
| D1 快速反馈 | 相容：判定更简单 |
| D2 预算 | 相容：谐波剥离删除后 met_analyze 更快 |
| D3–D7 分层/桩 | 相容：只删用户态纯函数与用例 |
| D8/D9/D10 | 不涉及 |
| D11 指标唯一源 | **强化**：删除 guest 侧重复口径后，SNR/THD 仅存 audio_metrics.py 一处 |
| D12 最小复现 | 相容：串口行字段更少 |

## 5 验收判据

1. 全仓 grep：`MET_THD_N_MAX_PCT/MET_SNR_MIN_DB/thd_n_pct/snr_db/thd_pct` 在
   userapp/ tests/ 无命中（宿主 hda_tests/tools 不在范围）。
2. `make test` 全绿（第 2 段 5→2 条，总数 40→37）。
3. `hda_tests --suite fast` 不退化（14/16，唯二存量）。
4. 整机串口：CAPTEST 模式下 AUDIO_VERDICT 行以 `v=2` 出现且无 thd_n=/snr= 字段
   （本轮以 TEST_MODE 构建验证，落证据）。
5. gen_vectors.py 可再生成 vectors.c（口径一致）。
