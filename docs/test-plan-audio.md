# 音频驱动测试方法（写代码前定稿）

配套决策：`docs/decisions-audio-test.md`（D1–D12、§6 测试栈、§8 DSP 与自检层）。
本文只定“怎么测、怎么算过、证据在哪”，不含实现。任何实现都必须先让这里的用例具备红灯能力。

## 0. 原则

1. **只测用户感受得到的行为**：采到几个样本、拓扑找没找对、指标准不准、PASS/FAIL 行对不对。不测内部函数调用次数、变量名、私有结构。
2. **每条断言必须能被打红**：新增检查后立刻做反向验证（弄坏 → 必须失败 → 修回 → 必须通过）。从来没红过的检查等于没加。
3. **期望值不来自被测代码**：指标期望值一律来自 `tools/audio_metrics.py`（Python 参照），拓扑期望值来自 profile 定义，不抄 C 的当前输出。
4. **不落盘不算证据**：所有命令输出写进 `docs/evidence/<YYYY-MM-DD>/`，文件名固定（见 §7）。
5. **失败即停**：红就是红，不用“环境问题”“大致通过”盖过去。

## 1. 被测行为清单

| 编号 | 行为（用户可感知） | 判据 |
|---|---|---|
| B1 | 采集搬够样本且不丢 | 实收字节 ≥ 期望 95%，`overruns=0`，`lpib>0` |
| B2 | 播放拓扑找得对 | 对 QEMU / Realtek 两套 profile 得出正确的 DAC NID 与 Pin NID |
| B3 | 指标算得准 | 与 `audio_metrics.py` 同信号结果在容差内（§4） |
| B4 | 频谱峰落在正确 bin | 1kHz 正弦的峰在 `round(1000/43.07)=23`±1 bin |
| B5 | 判定行机器可读 | 串口出现 `KEY=VALUE` 单行，且与同一轮指标一致 |
| B6 | 真机能冷启动跑完自检 | COM1 出现完整 `SELFTEST` 段，全 PASS |

## 2. 五层分工

| 层 | 入口命令 | 覆盖 | 断言举例 | 预算 |
|---|---|---|---|---|
| L1 单测（宿主 gcc + Unity） | `make test` | B2 拓扑发现、B3 指标纯函数、B4 频谱换算 | `test_topology_qemu()`、`test_thdn_3rd_harmonic()` | <2s |
| L2 qtest | `make qtest` | B1 流侧：BDL/CBL/LVI、RUN、LPIB 推进与环绕回、BCIS | `assert(ctrl_run == 1)`、`assert(lpib_after > lpib_before)` | 数秒 |
| L3 整机 `none` 后端 | `make full` | B1+B5 syscall 往返、WAV 读取、重复 start 幂等 | `CAPTEST PASS bytes=… overruns=0` | 单次 ~10s，总 <60s |
| L4 目标机自检 | 人工上电，COM1 | B6 + B2 真机 profile dump + 四条硬件检查 | `SELFTEST hda_eapd PASS` … | 自检 <2s |
| L5 真实回环（手动） | 3.5mm 回环 + 人跑 | B1 真实音频、B3 用真样本复算 | 峰值非零、`overruns=0`、THD 与离线一致 | 不计预算 |

L1 不覆盖流与中断、用户态 API 边界、WAV 解析（D4/D5/D6 已定）；这些由 L2/L3 承担。

## 3. 黄金向量（对拍）

```
tests/fixtures/gen_vectors.py      # numpy 生成 5 个场景，固定 seed=1234
tests/fixtures/vectors.json        # audio_metrics.py 算出的期望值 + 每条容差
tests/fixtures/vectors.c           # 由 gen_vectors.py 生成的 C 常量表（C 侧不解析 JSON）
```

场景：

| 场景 | 用途 | 关键期望 |
|---|---|---|
| `sine_1k_0dbfs` | 基准 | f0=1000±0.02%，THD+N ≤ -85dB |
| `sine_1k_plus_h2_h3` | 谐波计入 | THD 与 Python 差 ≤0.3dB |
| `noise_white_seed1234` | 噪声底 | SNR 与 Python 差 ≤0.3dB |
| `silence` | 零输入 | RMS=0，判定为 SILENT，不得判 PASS |
| `dc_offset_100` | DC 偏置 | THD+N 变差，用来验证指标对直流敏感（反向用例） |

判据：C 侧算出的值与 `vectors.json` 比，超出容差即失败，失败信息打印 `expected/actual/delta` 三段。

## 4. 容差（先按此表，实测后再收紧）

| 指标 | 容差 | 依据 |
|---|---|---|
| f0 | ±0.02% | `audio_metrics.py` 的 `_refine_f0` 精度约 1e-7Hz，C 侧允许两个数量级的退化 |
| RMS | ±0.1dB | int16 量化极限远小于此 |
| THD / THD+N | ±0.3dB | 与 Python 同算法，仅剩数值细节差异 |
| SNR | ±0.3dB | 同上 |
| 频谱峰 bin | ±1 bin | Hann 窗主瓣宽度 |

## 5. 反向门（每条新检查必须做一次）

| 弄坏什么 | 期望哪条变红 |
|---|---|
| `0x7000` 改回 `0xF000` | `test_amp_input_direction` |
| 连接列表只走短格式 | `test_conn_index_long_form` |
| 删掉 `cap_verify` 回读 | `test_readback_mismatch_fails` |
| 拓扑 DFS 去掉"沿 association list 继续" | `test_topology_qemu` / `test_topology_realtek` |
| 指标漏掉 DC 去除 | `test_thdn_dc_offset` |
| 判定行写成人类语句（`The test passed`） | `test_verdict_line_format` |

做法固定三步：弄坏 → 记 `red.log`（必须非 0 退出、错因明确）→ 修回 → 记 `green.log`。

## 6. 三类垃圾测试（本项目具体形态）

| 类型 | 本项目的样子 | 改法 |
|---|---|---|
| 假装测试 | 给 `hda_readl` 打桩返回预设值，去“验证”拓扑发现 | 用 profile 驱动**真实的 DFS 代码**，只把 MMIO 换成内存数组 |
| 自己考自己 | 期望值从当前 C 输出抄进用例 | 期望值只从 `vectors.json`（Python 生成）取 |
| 死抠细节 | 断言 `hda_send_verb` 被调用 7 次 | 断言**最终发出的 verb 序列**（NID/verb/param 三元组列表），换内部写法不影响 |

## 7. 证据规范

目录：`docs/evidence/<YYYY-MM-DD>/`，固定文件名：

```
build.log      编译/链接原始输出（含命令原文与退出码）
test.log       L1 单测（Unity 输出）
qtest.log      L2 寄存器级
full.log       L3 整机 none 后端 + 串口片段
selftest.log   L4 真机 COM1 原始日志（人工贴回或脚本抓）
red.log        反向门：故意弄坏后的失败输出
green.log      反向门：修回后的通过输出
```

防假绿三条（来自实测教训）：先删产物再编（`make clean` 或删 `tests/build`）；测试程序名带 `_fresh`；日志里附源码与产物时间戳（`ls -l --time-style=full-iso` 或 Windows 的 `Get-Item | Select LastWriteTime`）。

## 8. 不测什么

- 流与中断、用户态 API 边界 → L2/L3，不进 L1。
- WAV 头解析与磁盘读取 → L3/L5，不伪造 dosfs。
- 宿主默认音频设备切换 → 禁止（D9）。
- 真实声学指标（声压、频响）→ 阶段 2 才有硬件。

## 9. TDD 起点（先写这四条，且必须先红）

1. `test_amp_input_direction`：`0xB000 | (1<<14)` 必须被拒/被改写成 `0x7000`（现值 0xF000 会红）。
2. `test_conn_index_long_form`：长格式连接列表返回第 2 个 NID（旧实现取索引 0 会红）。
3. `test_readback_mismatch_fails`：回读值与请求不符时返回 `-8`（无回读校验会红）。
4. `test_thdn_3rd_harmonic`：THD 与参照差 ≤0.3dB（先假定分析模块为空，红）。

前三条跑在 L1，第四条跑在 L1 的纯函数上（喂 `vectors.c` 里的样本数组）。

## 10. 交差格式（每次实现完照抄）

```
做了什么：一句话
证据：<命令> → 通过 N，失败 0（日志：docs/evidence/<日期>/test.log）
反向门：<弄坏了什么> → 红（red.log）→ 修回 → 绿（green.log）
没验证到的：哪些、为什么；没有就写「无」
```

交差前先确认日志里确有命令原文与退出码，并核对该目录下文件真实存在。
