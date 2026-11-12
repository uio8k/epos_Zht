# 自动开发指令（/autodev）决策记录

日期：2026-11-10 建；2026-11-11 追加 D-A11（终态判定）与 D-A12（双副本归一）。来源：一轮 grill（常规强度，2 轮），用户逐条确认；实施按批准计划落盘（含用户追加要求：完成后不对该指令做测试）。
范围：项目级自动开发工作流（skill 机制、选题、干预点、自愈边界），不涉及任何代码改动。

## 1. 机制选型

| # | 结论 | 理由 |
|---|---|---|
| D-A1 | 载体为**工作区级 skill**：`外层\.dsh\skills\autodev\SKILL.md`（2026-11-11 由 `.zcode` 改准，见 D-A12），仅本项目会话可见可触发 | DSH 工作区 `.dsh/skills` 与 ZCode 工作区 `.zcode/skills` 都是项目级发现路径；零新代码零新依赖，skill 正文即工作流指令书 |
| D-A2 | 备选机制不采纳：Cron 定时（违背"用户选中才跑"）、OffPeak 闲时队列（后置项）、CreateWorkflow 多代理编排（单代理顺序工作流杀鸡用牛刀） | 用户要的是"我选择指令，就能自动开始"，触发权在用户手里 |

## 2. 已定结论

| # | 结论 | 理由 |
|---|---|---|
| D-A3 | 触发 = 用户选中（`/autodev` 或点名）；一次调用 = **一个 P 项**从调研到提交全闸门跑完即停，不连跑 | 单次产物边界清晰、可审计；信任建立后再放开连跑（后置项） |
| D-A4 | 选题 = **自动选 + 报备**：按 progress.html 选下一个可做 P 项（data-done=0、依赖满足、同级取靠前），开工前 AskUserQuestion 报备，用户有否决/换项窗口 | 用户不必事事钦点，但保留控制权（grill 第 1 轮确认） |
| D-A5 | tech-plan **落盘即继续**动代码，不等审批；**重大路线变更**（调研结论推翻原定路线，如 Bochs→VBox 量级）必须停下问 | AGENTS.md 只要求"动代码前有调研摘录与 tech-plan"，未要求审批环节；路线级反转影响后续排期，属于用户决策 |
| D-A6 | 测试失败**自修 ≤2 轮**，仍红带完整证据（测试日志、refs-check、最近 diff）停下问 | 有些失败是路线级结论（Bochs 证伪教训），机器不该硬修，也不许放宽断言或删用例来"变绿" |
| D-A7 | 硬干预点四类（任何步骤一律先问）：git 提交〔**已废止 → 见 D-A13 / D-D3**：提交改为自动，不再询问〕、安装/升级宿主软件、不可逆/破坏性操作（含改 QEMU 3.1.50=D8、超范围碰 kernel/userapp/tests）、git push | 提交是用户检查产物的最后闸门；装宿主软件动宿主系统（NDIS 网络闪断类）；破坏性操作不可逆；push 是外发动作（grill 第 1 轮四项全选确认） |
| D-A8 | 流程性干预点三个：选题报备（D-A4）、重大路线变更（D-A5）、自修 2 轮仍红（D-A6） | 与 D-A7 四类合并构成全部停机点，SKILL.md §9 单列 |
| D-A9 | 决策记录**留文件**：本文件（docs/decisions-autodev.md），随内层仓库 git 提交 | 与 decisions-audio-test.md 同范式；grill 开场问确认 |
| D-A10 | 上下文将耗尽时按会话续跑机制继续，**不提前截断闸门** | 单 P 项全闸门可能超单次上下文；续跑从断点恢复而非重做 |
| D-A11 | **终态判定**（2026-11-11 追加）：选题前先跑 `tools/check_progress.py --json`，按 `ELIGIBLE` / `BLOCKED` / `TERMINAL` 三态分派；`TERMINAL` 用固定句式收工「根据开发计划，当前已完成，暂无可自动化开发任务。」，且不 commit、不改 progress.html、不新造 P 项。判定逻辑只此一处实现，页面 JS 不重算 | 原 D-A3/D-A4 只有「有活干」的正向分支，无「全部做完」的出口；判定若靠模型自评会漂移，故落成脚本单一实现。三态互斥穷尽：有可做项 → `ELIGIBLE`；无但被挡 → `BLOCKED`；剩余全是有意悬置 → `TERMINAL` |
| D-A12 | **两份 SKILL 副本归一**：`.dsh/skills/autodev/SKILL.md` 为准，`.zcode/skills/autodev/SKILL.md` 为镜像（逐行一致，仅 `ask_user_question` / `AskUserQuestion` 大小写不同）；任何改动必须同步两份，不得只改一边 | D-A1 原定 `.zcode`，但 `.dsh` 那份先加上了附 A 而分叉（前 24 行相同、之后不一致）；两边都是工作区级发现路径，只改一边会出现「同一个 /autodev 两种行为」 |
| D-A13 | **取消「git 提交」干预点**（2026-11-12 追加）：轮末一律自动提交，**连代码轮也不问**；提交后汇报 commit hash + `git show --stat`，不请求确认。硬干预点由四类减为三类（装宿主软件、不可逆/破坏性操作、git push）。前提见 D-D8：开工树必须干净，有并发会话改动时停下报备，不替他人提交 | D-A7 把提交定为「用户检查产物的最后闸门」，前提是用户会逐轮审 diff；实际每轮都只回一句「继续」，闸门退化为等待。用户 2026-11-12 明确选择最激进档（grill 六问之三：一律自动提交），控制权改由「可随时回看的 git 历史 + 每轮汇报的 stat」承载。push 仍手动——远端外发与本地提交不同级 |
| D-A14 | **分支文档管辖 / 注意力隔离**（2026-11-12 追加，用户指示：「我们现在进行 SMP 开发，就不该再去管之前 x86 那个音频驱动分支的文档」）：智能体只管**当前分支**的文档族，SKILL §1.5 为映射唯一实现——`x86`→音频族（Pxx 队列、decisions-audio-test/adc/qemu-only、tech-plan-hda/vm-diff/full-none/metrics-purge/qtest、音频 evidence）；`x86_SMP`→SMP 族（Sxx 队列自 S01 起编、decisions-smp-\*、tech-plan-smp-\*、SMP evidence）。管辖域外文档**不读、不改、不进 refs-check/docs-check 关注面**；共享流程层（AGENTS/CODEBUDDY、decisions-autodev/docs、check_progress.py、check_docs.py、full_run.py、SKILL 双副本）两分支逐字一致，改动提交后 cherry-pick 到另一分支；需求落在管辖域外→停下报备，切分支由用户定 | 分支间文档全共享会让每轮被无关文档的 docs-check/健康块牵制，注意力涣散；隔离后每轮只面对本族文档。映射表唯一实现防「模型自评管什么」漂移，与 D-A11 同精神 |
| D-A15 | **x86_SMP 基线重置**（2026-11-12 追加）：本地 `git branch -f x86_SMP x86` 把 SMP 分支重置到 x86 HEAD——调研实证旧 x86_SMP tip 即 merge-base（0 独有提交，无 progress.html/docs/tests/SMP 代码），直接切过去等于丢掉 22 个成果提交；SMP 开发在 x86 全部成果之上起步。**只动本地**，远端 origin/x86_SMP 保持旧快照（force push 属不可逆干预点，另行确认）；SMP 队列编号自 S01 起，与音频 Pxx 命名空间隔离 | 用户目标「继承全部成果 + 转 SMP 开发」与「切到 x86_SMP」字面冲突，grill 两轮收敛为重置方案；旧位置经 reflog+远端可回溯，无信息丢失 |
| D-A16 | **/autodev 是智能体唯一开始开发编码的入口**（2026-11-12 追加，用户约定）：文档管理、资料搜索、系统结构设计、代码设计等**前置工作**可在用户日常对话中进行；但进入**实现/编码**（写 kernel/userapp/tests 的代码或改其构建）必须由**用户手动触发 /autodev**。未经触发即编码 = 违规，停下报备；用户在对话里直接要求「实现 X」时也先确认是否启动 /autodev。触发只授权本轮选题报备的 scope，scope 外编码仍需再走入口 | 用户原话「除此之外的开发编码都要向我确认」「前置工作做好以后，由我手动启动 /autodev 开始开发」——开发启动权收归用户手里，智能体的自主边界收敛到「想清楚」而非「动手」；与 D-A13（轮内自动提交）不冲突：入口管「何时开始」，D-A3/D-D3 管「怎么收尾」 |

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
| `外层\.dsh\skills\autodev\SKILL.md` | 指令本体（**准**，D-A12）；外层非 git 仓库，不进版本控制 |
| `外层\.zcode\skills\autodev\SKILL.md` | 指令镜像（D-A12），逐行与上者一致 |
| `epos_Zht\tools\check_progress.py` | 终态判定脚本（D-A11），仅标准库、Python 3.7 语法兼容、不进 `make test` |
| `epos_Zht\progress.html` | 队列属性扩展：`data-dep`（P11）/ `data-optional`（P13），判定只读不改 |
| `docs/decisions-autodev.md`（本文件） | 决策记录，随内层仓库提交 |
| `docs/evidence/2026-11-10/refs-check-autodev.log` | 本文件 文件:行号 引用的 Select-String 复核 |
| `docs/evidence/2026-11-11/autodev-terminal-check.log` | D-A11 的判定实测（三态 + 结构错误 + 两种解释器） |
| `docs/evidence/2026-11-11/refs-check-autodev-terminal.log` | 本次新增 D-A11/D-A12 与 SKILL 2.5 节的行号复核 |
| `外层\AGENTS.md` | 交差硬规则表（2026-11-12 追加 6 行文档托管规则，见 D-D1/D-D2/D-D4/D-D7/D-A12） |
| `epos_Zht\tools\check_docs.py` | 文档一致性检查（C1–C10），`make docs-check` 入口；D-D4 的落点 |
| `epos_Zht\docs\decisions-docs.md` | 文档托管决策 D-D1–D-D8（文档归属单一事实源） |

静态验证（已做）：frontmatter name/description 齐全、description <1024 字符、触发语义前置（skill 加载合规）。
