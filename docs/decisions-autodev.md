# 自动开发指令（/autodev）决策记录

日期：2026-11-10。来源：一轮 grill（常规强度，2 轮），用户逐条确认；实施按批准计划落盘（含用户追加要求：完成后不对该指令做测试）。
范围：项目级自动开发工作流（skill 机制、选题、干预点、自愈边界），不涉及任何代码改动。

## 1. 机制选型

| # | 结论 | 理由 |
|---|---|---|
| D-A1 | 载体为**工作区级 skill**：`外层\.zcode\skills\autodev\SKILL.md`，仅本项目会话可见可触发 | ZCode 原生发现顺序第 4 级（工作区 `.zcode/skills`），外层 `.zcode/` 已在用（plans 目录）；零新代码零新依赖，skill 正文即工作流指令书 |
| D-A2 | 备选机制不采纳：Cron 定时（违背"用户选中才跑"）、OffPeak 闲时队列（后置项）、CreateWorkflow 多代理编排（单代理顺序工作流杀鸡用牛刀） | 用户要的是"我选择指令，就能自动开始"，触发权在用户手里 |

## 2. 已定结论

| # | 结论 | 理由 |
|---|---|---|
| D-A3 | 触发 = 用户选中（`/autodev` 或点名）；一次调用 = **一个 P 项**从调研到提交全闸门跑完即停，不连跑 | 单次产物边界清晰、可审计；信任建立后再放开连跑（后置项） |
| D-A4 | 选题 = **自动选 + 报备**：按 progress.html 选下一个可做 P 项（data-done=0、依赖满足、同级取靠前），开工前 AskUserQuestion 报备，用户有否决/换项窗口 | 用户不必事事钦点，但保留控制权（grill 第 1 轮确认） |
| D-A5 | tech-plan **落盘即继续**动代码，不等审批；**重大路线变更**（调研结论推翻原定路线，如 Bochs→VBox 量级）必须停下问 | AGENTS.md 只要求"动代码前有调研摘录与 tech-plan"，未要求审批环节；路线级反转影响后续排期，属于用户决策 |
| D-A6 | 测试失败**自修 ≤2 轮**，仍红带完整证据（测试日志、refs-check、最近 diff）停下问 | 有些失败是路线级结论（Bochs 证伪教训），机器不该硬修，也不许放宽断言或删用例来"变绿" |
| D-A7 | 硬干预点四类（任何步骤一律先问）：git 提交、安装/升级宿主软件、不可逆/破坏性操作（含改 QEMU 3.1.50=D8、超范围碰 kernel/userapp/tests）、git push | 提交是用户检查产物的最后闸门；装宿主软件动宿主系统（NDIS 网络闪断类）；破坏性操作不可逆；push 是外发动作（grill 第 1 轮四项全选确认） |
| D-A8 | 流程性干预点三个：选题报备（D-A4）、重大路线变更（D-A5）、自修 2 轮仍红（D-A6） | 与 D-A7 四类合并构成全部停机点，SKILL.md §9 单列 |
| D-A9 | 决策记录**留文件**：本文件（docs/decisions-autodev.md），随内层仓库 git 提交 | 与 decisions-audio-test.md 同范式；grill 开场问确认 |
| D-A10 | 上下文将耗尽时按会话续跑机制继续，**不提前截断闸门** | 单 P 项全闸门可能超单次上下文；续跑从断点恢复而非重做 |

## 3. 铁律继承（skill 正文显式声明冲突时以下列为准）

- 外层 `AGENTS.md` 硬规则表：调研前置、证据落盘、refs-check、Out-File utf8 禁 Tee-Object、Select-String 行号、表格竖线转义、出处标记、doc-only 轮不碰 kernel/userapp/tests。
- `docs/decisions-audio-test.md` D1–D12：D8 不动 QEMU 3.1.50、D9 禁改宿主默认音频设备、D10 入口统一 make、D11 指标唯一源。
- 下载/配置只走国内源；全失败才提替换方案并留痕（用户约定，AGENTS.md 已固化）。
- 检索摘要只作线索，能力类结论必须过 `[spec:]`/`[src:]`/`[实测:]` 一手证据（Bochs 证伪教训，tech-plan-vm-diff.md §3）。

## 4. 明确不做 / 后置项

- **闲时队列叠加**（OffPeak）：跑顺 1–2 次后再评估（grill 第 2 轮确认先不叠加）。
- **连跑放开**（一次指令多个 P 项）：信任建立后再评估。
- **不做真跑验证**：用户 2026-11-10 追加要求，交付即止，/autodev 的实际触发由用户自己执行。
- progress.html 不加 autodev 条目：流程基建不是 P 项（与 P13 的基建项处理同理，但 autodev 与项目进度无关，连条目都不占）。
- 真机 L4/L5、治具相关（原 decisions-audio-test.md §5 治具四件）不在本指令覆盖范围——2026-11-10 QEMU-only 修订后真机路线已整体废弃（`docs/decisions-qemu-only.md` D-Q1），队列中不再有真机项。

## 5. 落点与验证

| 文件 | 说明 |
|---|---|
| `外层\.zcode\skills\autodev\SKILL.md` | 指令本体；外层非 git 仓库，不进版本控制 |
| `docs/decisions-autodev.md`（本文件） | 决策记录，随内层仓库提交 |
| `docs/evidence/2026-11-10/refs-check-autodev.log` | 本文件 文件:行号 引用的 Select-String 复核 |

静态验证（已做）：frontmatter name/description 齐全、description <1024 字符、触发语义前置（skill 加载合规）。
