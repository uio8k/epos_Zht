# 文档托管改造：progress.html 成为唯一人工界面（doc stewardship）

日期：2026-11-12（非 P 项轮次，流程基建）。决策来源 `docs/decisions-docs.md` D-D1–D-D8；
用户 grill 六问逐条确认（范围 / decisions 可改性 / 提交闸门 / 防漂移 / 页面承载 / 触发方式）。
本轮**零代码改动**：不碰 `kernel/`、`userapp/`、`tests/*.c`；只动文档、`tools/`、根 `Makefile`
的一个检查目标，以及仓库外的 skill 双副本与 `AGENTS.md`/`CODEBUDDY.md`。

## 1 范围与原则

- **人工界面收敛为一个文件**：`progress.html`。读者若需要第二份文件才能判断状态，改造即失败。
- **托管范围（D-D1，六类全选）**：内层 `docs/`（`decisions-*` / `tech-plan-*` / `evidence/`）、
  内层 `README.md`、外层 `AGENTS.md`、外层 `CODEBUDDY.md`、外层 `docs/`、skill 双副本
  （`外层\.dsh\skills\autodev\SKILL.md` 为准、`外层\.zcode\skills\autodev\SKILL.md` 为镜像）。
- **历史不可改写（D-D2）**：`decisions-*.md` 只追加；过期表述就地加
  `〔已废止 → 见 D-xx〕` 标注，原句一字不改。
- **证据只增（D-D7）**：`docs/evidence/` 既有文件不删、不改写、不合并。

## 2 依据等级

| 标记 | 内容 | 出处 |
|---|---|---|
| [实测: Makefile:56] | `full: test qtest full-l3`，当前无任何文档检查 | 本轮 `Select-String` 复核实测 |
| [实测: progress.html:325] | 五层表 L3 行仍 `未开始`，而 P10 已 `data-done="1"` | 同上 |
| [实测: docs/evidence/2026-11-11/refs-check-p10.log] | 实为 `SUMMARY: 31 PASS / 0 FAIL` | 同上 |
| [实测: tools/check_docs.py 不存在的旧状态] | 仓库无任何跨文档一致性检查器 | `Get-ChildItem tools` 实测 |
| [src: tools/check_progress.py:40] | 既有的「状态只存属性、判定只有一处实现」范式 | 仓内源码 |
| [src: tests/run_tests.bat:32-78] | L1 六个测试二进制，决定「M 段」的可推导来源 | 仓内源码 |
| [wiki: 无外部依据] | 本轮为仓库内部流程约定，不引入 osdev/QEMU 规范结论 | — |

## 3 备选矩阵

| 方案 | 判据 | 结论 |
|---|---|---|
| A. 只写规则进 SKILL.md，不加脚本 | 零成本；但「硬限制只写在提示词里」正是现存 6 处漂移的成因（AGENTS.md 自身要求硬限制落到工具/测试） | 不采纳 |
| B. 脚本只手动跑，不进 `make full` | 不占 60s 预算；但无人跑时永不红，漂移照样积累 | 不采纳（D-D4） |
| C. **`tools/check_docs.py` + 接 `make full` 首位** | 可执行、红了即停、约 0.3s；失败面在提交前暴露 | **采纳** |
| D. 每次提交前由模型逐条人工核 | 依赖每轮自觉，无法审计「有没有核」 | 不采纳 |
| E. 页面不加健康块，状态只出现在汇报文字 | 汇报过即消失，无留痕 | 不采纳（D-D5） |
| F. 健康块由 CI 生成 | 本仓无 CI，且外层非 git 目录无法进入 CI | 不可行 |

## 4 落点表

| 文件 | 改动 |
|---|---|
| `tools/check_docs.py` | **新建**。C1–C10 + `--json` / `--sync` / `--selftest`；纯标准库、Python 3.7 语法、控制台输出 ASCII |
| `Makefile` | 新增 `.PHONY: docs-check` 目标；`full:` 前置 `docs-check` |
| `progress.html` | 6 处漂移修正；两表加 `data-*` 属性；新增 `④ 文档健康` 标记块；`使用说明` 改写；变更记录新增本轮条目；`:214` 自述日期 |
| `docs/decisions-docs.md` | **新建**。D-D1–D-D8 + 文档归属表（单一事实源） |
| `docs/tech-plan-docs.md` | 本文件 |
| `docs/decisions-autodev.md` | 追加 D-A13；D-A7 加 `〔已废止 → 见 D-A13〕` 标注（原句保留） |
| `外层\.dsh\skills\autodev\SKILL.md` | frontmatter 干预点列表；`§1.4` 文档边界；`§7.5` 文档同步；`§8` 自动提交；`§9` 干预点三类 |
| `外层\.zcode\skills\autodev\SKILL.md` | 与上者逐行同步（D-A12） |
| `外层\AGENTS.md` | 规则表加 **6 行**（唯一界面 / decisions 只追加 / docs-check 红线 / 证据 append-only / 外层改动先备份 / skill 改完同步镜像） |
| `外层\CODEBUDDY.md` | `:22`、`:76` 死链 `epos_Zht/docs/运行环境.md` → `setvars.bat` + 本文件「常用命令」节 | <!-- docs-check: ignore -->
| `README.md` | `:6`、`:106` 死链重定向（见 §6） |
| `docs/tech-plan-full-none.md`、`docs/tech-plan-metrics-purge.md` | 追加 2026-11-12 基线追注（`ST-ENUM-TABLE` 已修，现行 15/16），原句保留 |
| `docs/evidence/2026-11-12/` | 改造前红日志、改造后绿日志、`make full` 计时、`make test` / `make qtest` / `hda_tests fast`、refs-check、双副本 diff、外层备份 |

## 5 检查项矩阵（C1–C10）

| # | 级别 | 检查 | 判据与失败样例 |
|---|---|---|---|
| C1 | fail | 失效文档路径 | 引用按**引用文件所在层**解析（内层→仓库根、外层→工作区根），必须存在。样例：`README.md:6` → `docs/运行环境.md` 不存在 |
| C2 | fail | 页面自述日期 | `progress.html:214`「更新于」== 变更记录首条 `<b>` 日期 |
| C3 | fail | refs-check 计数 | 页面 `refs-check-<tag>` 旁计数 == `docs/evidence/*/refs-check-<tag>.log` 的 `SUMMARY:`。样例：页面 24/24 vs 日志 31 PASS |
| C4 | fail | 未实现表状态机 | `data-state="open"` 行必有 `data-blocked-by` 且该 P 项 `data-done="0"`；`done` 行不得指向未完成项 |
| C5 | fail | 五层表状态机 | 同上，`data-state` ∈ `none`/`partial`/`done` |
| C6 | fail | 表格计数句 | 「已实现 X/Y 条」== 该表 `data-state="done"` 行数 / 总行数 |
| C7 | fail | 健康块保真 | `DOCS-HEALTH` 标记块内容 == `--sync` 生成内容（逐字） |
| C8 | fail | skill 双副本 | 规范化 `AskUserQuestion`↔`ask_user_question` 后逐行相等；两侧文件不全则 skip+warn |
| C9 | warn | 孤儿文档 | `docs/*.md` 既不在 `MANIFEST` 登记，也没出现在 `progress.html` 正文 |
| C10 | fail / warn | L1 计数 | 「M 段」== `tests/run_tests.bat` 中 `test_*_fresh.exe` **去重后**个数（现 6；文件里每个都出现两次：编译+执行，不去重会数成 12）——来源精确，故 **fail**；「N 测试」与最新 `*make-test*.log` 各段 `N Tests` 求和比对，解析不出打印 `SKIP C10` 不静默通过——依赖日志解析，故 **warn** |

退出码语义与 `tools/check_progress.py` 一致：`0` = fail 级全过（warn 不拦），`1` = 有 fail 或结构不可读。

## 6 死链重定向依据

| 位置 | 原引用 | 重定向到 | 理由 |
|---|---|---|---|
| `README.md:6` | `docs/运行环境.md` | `progress.html` + `../CODEBUDDY.md` | 该文件在 `9ccf800` 删除 [实测: git log --diff-filter=D]；状态看进度页，环境看 CODEBUDDY 常用命令 |
| `README.md:106` | `docs/运行环境.md` 的缓冲模型与踩坑表 | `kernel/hda_stream.c` 顶部参数区注释 + `docs/tech-plan-hda.md` | 同节上一句已指定 `hda_stream.c` 注释；方案细节在 tech-plan-hda | <!-- docs-check: ignore -->
| `CODEBUDDY.md:76` | `epos_Zht/docs/运行环境.md` | `setvars.bat` + 本文件「常用命令」 | 工具链版本锚在 `setvars.bat`，命令清单在 CODEBUDDY 自身 | <!-- docs-check: ignore -->
| `progress.html`（大字截图） | `docs/evidence/2026-10-29/verdict-big.png`（内层相对） | `../docs/evidence/2026-10-29/verdict-big.png` | 该证据目录实际在外层工作区（内层无 `2026-10-29`） | <!-- docs-check: ignore -->

## 7 契约变更

- `progress.html` 新增可机读属性：表格行 `data-case` / `data-state` / `data-blocked-by`，
  五层表行 `data-layer`。生产者 autodev，消费者 `check_docs.py`。
- **`tools/check_progress.py` 不受影响**：它只读 `<li class="item">` 上的
  `data-id`/`data-done`/`data-dep`/`data-optional` [src: tools/check_progress.py:63-67]。
- `DOCS-HEALTH` 块是生成产物，唯一源是 `check_docs.py`；人工改必红（C7）。
- 文档归属以 `docs/decisions-docs.md` §1 为单一事实源；`AGENTS.md` 与 SKILL 只引用不复述。

## 8 D1–D12 与 D-A 系列相容核对

| 决策 | 相容性 |
|---|---|
| D1 快速反馈 | 相容：docs-check 约 0.3s，失败即停 |
| D2 全套 60s 预算 | **压线**：当前 55.0s + 0.3s ≈ 55.3s，余量 4.7s；再加腿须重核 |
| D3–D7 分层/桩 | 不涉及 |
| D8 不动 QEMU | 不涉及 |
| D9 禁改宿主音频设备 | 不涉及 |
| D10 入口统一 make | **强化**：`make docs-check` 与 `make full` 同入口，无独立 bat |
| D11 指标唯一源 | 不涉及 |
| D12 最小复现 | 相容：检查失败输出带文件行号与期望值 |
| D-A5 tech-plan 落盘即继续 | 相容：本文件落盘后直接实施 |
| D-A7 干预点四类 | **变更**：删「git 提交」一项 → D-A13；其余三类保留 |
| D-A11 判定单一实现 | 相容：文档健康与状态机判定同样只由脚本实现 |
| D-A12 双副本归一 | **强化**：C8 把「必须同步」变成可执行检查 |

## 9 验收判据（实测回填）

1. **反向验证**：`docs-check-before.log` 在改造前树上确实红 —— `RESULT FAIL (28 fail / 6 warn)`，命中 8 类：README 死链 2 处、CODEBUDDY 死链 2 处、`progress.html:271` 外层证据写成内层相对路径、`refs-check-p10` 24 vs 31、五层表 L3 `未开始`、`U-L3-01/02` 与 `U-L4-01` 的 `空着`、计数句 `12/14`、自述日期 `2026-11-11`（外加两表全缺 `data-state`，C4/C5 因此全表 4+7 行皆红）。
2. `make docs-check` exit 0，`RESULT PASS (0 fail / 1 warn)`；`--selftest` **10/10 PASS**（计划要求 ≥8）。
3. `make full` exit 0、墙钟 **41.4s**（<60s，余量 18.6s）；L1 37（5+2+8+8+10+4）、L2 15、L3 四腿 PASS、L4 4/4；`汇总：5 腿，5 通过，0 失败，用时 33.4s`。
4. `progress.html` 出现 `④ 文档健康`（`:398` 起，块 `:401`–`:423`），`--sync` 后 `check` 通过（C7 ok）。
5. skill 双副本规范化后 `IDENTICAL`（`skill-diff.log`：`.dsh` 13329B / `.zcode` 13327B，仅 `ask_user_question`↔`AskUserQuestion` 一处差）。
6. 改动范围 = §4 落点表：`M Makefile`、`M README.md`、`M docs/decisions-autodev.md`、`M progress.html`、新增 `docs/decisions-docs.md`、`docs/tech-plan-docs.md`、`tools/check_docs.py`、`docs/evidence/2026-11-12/`；`git status --short` 不含 `kernel/`、`userapp/`、`tests/*.c`。
7. `hda_tests --suite fast` **实测 15/16**，唯一 FAIL = `ST-SYSCALL-NR`（`include/syscall-nr.h` 把 audio 号段延到 201，而 `hda_tests/lib/static_checks.py:213` 仍钉 `len==10`），非本轮红线。
   **基线订正**：本项此前一律记作「14/16，唯二存量 `ST-ENUM-TABLE` / `ST-SYSCALL-NR`」——已过期。`ST-ENUM-TABLE` 由提交 `2fce427`（2026-11-12，连续启停支持）在 `kernel/hda_trace.c:46-48` 补上 `HTEV_CAP_START` / `HTEV_CAP_OVERFLOW` / `HTEV_CAP_STOP` 三个 case 后转 PASS。本轮未碰这两处文件，数字变化不是文档改造引入的回归；`docs/evidence/2026-11-12/hda-tests-fast.log` 末尾附了同一段订正。

**实施期两处自我更正**（写进本文件是为了让后来者别踩同一个坑）：

- **`make docs-check` 与手动 `--sync` 结果不一致**：`make` 跑在 `cmd /c "call setvars.bat /ni && ..."` 之下，而 `setvars.bat:3-12` 会把 `PATH` 重写成 `%SystemRoot%` + 自带工具链，全局 git 就此消失（实测 `'git' is not recognized` → `git_date()` 返回 None → 健康块「最后更新」退化成 `(mtime)`，与 `--sync` 生成的 git 日期块不符，C7 在 `make full` 里红）。修法：`_git_exe()` 在 `shutil.which` 失败后探测 `C:\Program Files\Git\cmd\git.exe` 等常见位置，`GIT` 常量全局复用，结果不再依赖调用环境。
- **块里记「工作树脏」是个地雷**：初稿在最后更新后加 `+` 表示未提交，但块本身要提交——提交那一刻它就与现场不符，下一轮 `make docs-check` 必然红。已删掉脏标记（`git_dirty()` 保留但不再参与渲染），块只记「已提交内容」的日期，工作树状态交给 `git status`。另：健康块里记着 `docs/evidence` 的**文件数**，所以落证据要先把本轮所有日志文件建出来再 `--sync`，否则边写日志边比重算永远差一个。

## 10 不确定项（每条带验证方法）

| # | 不确定 | 验证方法 |
|---|---|---|
| U1 | 外层非 git 文件（`AGENTS.md`/`CODEBUDDY.md`）无版本回退 | 改前复制到 `docs/evidence/2026-11-12/backup/`，`git` 之外的兜底靠该副本 |
| U2 | 并发会话可能在本轮期间改动同一批文档 | 每步写文件前重跑 `git status --short`；发现他人改动即停（D-D8） |
| U3 | 「最后更新」对外层文件只能用 mtime，会被复制/还原扰动 | 健康块对 mtime 来源显式标注「mtime」，读者可据此判可信度 |
| U4 | C10 的「N 测试」依赖历史日志可解析，日志格式若变会静默 skip | skip 时打印 `SKIP C10`，不静默通过；日志格式变更在证据里可见 |
| U5 | 60s 预算余量仅 4.7s | `make-full.log` 记录墙钟；后续加腿须先看该数字 |
