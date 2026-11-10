# QEMU-only 修订决策记录（真机验证路线废弃）

日期：2026-11-10。来源：一轮 grill（常规强度，3 轮卡片），用户逐条确认。本文记录对既有开发计划的一次方向性修订，**只改文档**（含证据目录处置），代码删除项立为后续 P 项。

## 1. 已定结论

| # | 结论 | 理由 |
|---|---|---|
| D-Q1 | **去掉治具机实机验证，QEMU 3.1.50 为项目唯一与最终验证平台** | 用户 2026-11-10 决定；治具机从未采购（progress.html 治具四件长期「未到位」），路线悬置成本大于价值 |
| D-Q2 | 文档层面**彻底删除**真机内容：progress.html（阶段 1/2、治具四件节、P01、真机受阻行、L5 层）、decisions-audio-test.md（§5 及散布真机语义）、tech-plan×3 真机段落；docs/evidence/ 历史证据一并清理 | 用户明确选「彻底删除」而非「标注废弃」；git 历史仍可回溯全部被删内容 |
| D-Q3 | 证据目录实查结果：**无真机路线专属证据文件**——`docs/evidence/` 三个命中文件（osdev 摘录、两份 refs-check）里的真机提及均为第三方摘录原文或对将删段落的复核，不属于真机验证内容本身；osdev「模拟器 ≠ 真机」警告摘录**保留**（它是本决策 D-Q1 的依据）。`docs/evidence/2026-11-10/refs-check-autodev.log` 中检查 §5 治具段的 1 行因对象已删，追加修订说明，不删档 | 删证据的边界按「删真机路线内容、不删决策依据」执行；详见 §4 修订说明 |
| D-Q4 | P11（原 L4 目标机自检）**改 QEMU 版保留**：SELFTEST 段并入 make full 整机链路；L4 层表格改「QEMU 整机自检」，数据通道（COM1 串口日志）不变——QEMU 串口即自检输出 | 自检能力本身不依赖真机，只是原定在治具上跑；改平台不砍能力（grill 第 2 轮确认） |
| D-Q5 | **SNR/THD 真值指标整体删除**：只留 overruns/过零样本/峰值等行为指标；`userapp/metrics.h` 的 `MET_THD_N_MAX_PCT`/`MET_SNR_MIN_DB` 常量与 `AUDIO_VERDICT` 行 thd_n=/snr= 字段的代码清理立为后续 P 项（原 P05b 槽位改造承载） | 真值档位原定靠真机 3.5mm 回环定档（P05b「受真机阻塞」）；真机删后无定档通道，模拟器值非真值（tech-plan-vm-diff.md §6.4 原结论），指标本身失去判据意义（grill 第 2 轮确认「删指标」） |
| D-Q6 | **QEMU 单档为最高验证档**：P13（VBox×QEMU 差分）不升格，维持「提交前可选第三档」原定位（tech-plan-vm-diff.md §6.6） | 用户选「QEMU 单档」非「差分升格」；交付证明力以 QEMU 体系为准 |
| D-Q7 | 本轮**文档先行**：metrics 代码删除、P05b 改造后的代码执行走后续 P 项轮（AGENTS 调研前置规则适用） | 与「只写文档的会话不碰 kernel/userapp/tests」既有约定一致 |
| D-Q8 | P 项编号不重排（P01 删除后编号留空洞），避免全仓 data-id/l-Pxx 引用级联改动 | grill 收尾声明，用户未见异议 |
| D-Q9 | L5 层（手动 3.5mm 真实样本回环）删除；P12 宿主声卡回环为已发生的历史事实，条目保留并注明「QEMU-only 后不再扩展」 | 宿主回环非 QEMU 通道，与「仅在 QEMU 上跑」冲突；历史不抹除 |
| D-Q10 | 本文件为修订记录本体；decisions-audio-test.md 落「真机路线 2026-11-10 废弃」指针，其余真机内容物理删除 | 历史决策可经 git 历史与本文件双重回溯 |

## 2. 与既有决策的相容性

| 既有 | 影响 |
|---|---|
| D1–D12（decisions-audio-test.md） | 全部不受影响：D8（不动 QEMU）反而是本决策的基石；§5/§6/§8 真机段落按 D-Q2 删除或改写 |
| D-A1–A10（decisions-autodev.md） | 不受影响；/autodev 后续选题自动按修订后的 progress.html 队列执行 |
| P13（tech-plan-vm-diff.md） | 不升格（D-Q6）；§1/§4/§5/§6.4/§10 真机表述改写为 QEMU-only 口径 |
| AGENTS.md 调研前置规则 | 代码删除 P 项（D-Q5/D-Q7）执行时照常适用 |

## 3. 明确不做

- 不采购治具机、不做 3.5mm 电气回环、不做声学件（参考麦/隔音/声压）——相关阶段与条目全部删除。
- 不保留「等真机」语义的任何排期（U3/VMIN/VMAJ 真机核对、U-L1-05 Realtek dump、EAPD/VrefEn 真机效果验证——后者降为 QEMU 侧仅验 verb 发送与回读打印）。
- 不把 SNR/THD 改成模拟器定档（grill 第 2 轮否决「模拟器定档」选项）——指标直接删除。

## 4. 修订范围执行清单（本轮实际改动）

1. `progress.html`：删阶段 1/2 与治具四件节（含 P01）；P05b 改造为「删 SNR/THD 指标代码」新 P 项；P07 去真机化改写（GCAP 解码保留）；P11 改 QEMU 版；L4 表行改 QEMU 整机自检、L5 表行删；U 表 U-L1-05/U-L5-02 删、U-L4-01 改 QEMU 口径、U-L5-01 注历史；判定输出节点「阈值待真机」改「真值指标已删」；changelog 顶部加 2026-11-10 条目。
2. `docs/decisions-audio-test.md`：文首加废弃指针；§5 整节删；§6 选型改两段式（宿主单测+QEMU 整机）+删阶段 1/2；§7 验收 (d) 真机串口改 QEMU 串口；§8 定点/自检层/四条要求中真机语境改写；真机排查顺序段删。
3. `docs/tech-plan-vm-diff.md`：§1/§4/§5/§6.4/§10 真机表述改 QEMU-only 口径（wiki「模拟器≠真机」警告保留为已接受的已知局限）。
4. `docs/tech-plan-qtest.md`：174 行真机轮 dump 改废弃注记。
5. `docs/tech-plan-hda.md`：真机语境逐处改写/删除（规范内容保留，真机验证展望删）。
6. `docs/evidence/2026-11-10/refs-check-autodev.log`：追加修订说明行（该日志 1 行检查对象 §5 已删）。

## 5. 验收判据（本轮交差对照）

1. `git grep` 全仓（除 git 历史与本文件/证据说明行）无「治具/OptiPlex/3.5mm 电气回环」命中；「真机」仅存于本文件、decisions-audio-test.md 废弃指针、tech-plan 中 QEMU-only 已接受局限表述。
2. progress.html 结构完好（data-id 连续可勾选、计数更新、changelog 有条目）。
3. refs-check（Select-String）复核本文件引用的文件与段落，全 PASS 落 `docs/evidence/2026-11-10/refs-check-qemu-only.log`。
4. `git status --short` 只见文档与 evidence 路径（kernel/userapp/tests/tools 零改动）。
