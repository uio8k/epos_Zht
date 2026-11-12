# AGENTS.md

本文件只记「怎么交差」的硬规矩，不重复 `CODEBUDDY.md` 的架构与命令。规则来源：2026-10-28 写 `docs/tech-plan-hda.md` 时踩到的坑（行号引用错 2 处、证据日志写成 UTF-16、表格竖线断表）。

| 规则 | 由什么拦住 |
|---|---|
| 交差前跑真实测试入口（`make test`、`hda_tests\run_all.bat --suite fast`），原始输出落 `docs/evidence/<YYYY-MM-DD>/` | 人工：日志文件必须存在，且里面能看到命令原文与退出码 |
| 文档里写 `文件:行号` 之前，用 `Select-String` 逐条复核，输出落 `docs/evidence/<日期>/*refs-check*.log` | 人工：没有 refs-check 日志就不算核对过 |
| 证据日志写入只用 `Out-File` / `Set-Content -Encoding utf8`，禁用 `Tee-Object` | 人工：PowerShell 5.1 的 `Tee-Object` 写 UTF-16，中文全成间隔字符；打开首行读不通就是没落对 |
| 串口/日志的行号一律以 `Select-String` 结果为准，不用 ripgrep 的行号 | 人工：本仓日志含 CR-only 行尾，rg 与 PowerShell 计数会差十几行（2026-10-28 实例：`:16` 实为 `:31`） |
| Markdown 表格单元格里出现竖线写成 `\|` | 人工：渲染后表头与分隔行列数对不上就是断了 |
| 规范类结论必须带出处标记 `[wiki: 段落名]` / `[spec: 条目]` / `[实测: 文件:行号]`；wiki 与规范冲突以规范为准并显式列出；不确定项单列一章 | 人工：用 `Select-String -Pattern '\[wiki:'` 或 `'\[实测:'` 能查到；无出处的判断句视为未完成 |
| 每项工作（进度页一个 P 项）动代码之前，先上 Gitee / osdev wiki 调研：摘录逐字落 `docs/evidence/<日期>/`（带 URL/oldid/行号），方案写 `docs/tech-plan-*.md` 并加 `[Gitee: 文件:行号]` 标记（规则来源：2026-10-31 用户约定，首轮实践为 P09 调研轮） | 人工：动代码前没有调研摘录与 tech-plan 就算未完成；QEMU 源走 gitee.com/mirrors/qemu（raw 链接有签名重定向，`curl -L` 可直接跟）；osdev wiki 直读会被反爬拦，用仓内已存摘录 |
| 只写文档的会话不碰 `kernel/`、`userapp/`、`tests/` | 人工：收尾 `git status --short` 只应出现文档与 `docs/evidence/` 路径 |
