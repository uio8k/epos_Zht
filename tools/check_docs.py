#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""check_docs.py -- 文档与进度页一致性检查（D-D4 / D-D5）。

设计约束（与 tools/check_progress.py 同一范式）:
  * 状态只存在一处：进度页的状态写在 data-* 属性里，本脚本只读不猜;
  * 判定只此一处实现：同一份检查既给 `make docs-check` 用，也给 --sync 用;
  * 纯标准库、Python 3.7 兼容、控制台输出 ASCII（非 ASCII 路径转义成 \\uXXXX），
    保证证据日志在任何代码页下都不糊；写进 progress.html 的块才是真 UTF-8。

检查项:
  C1  文档路径引用存在（含 README / docs/*.md / AGENTS.md / CODEBUDDY.md / SKILL 双副本）
  C2  页面自述 `更新于 <日期>` == 变更记录首条日期
  C3  页面引用的 refs-check 计数 == 对应日志的 SUMMARY 行
  C4  未实现表状态机（data-state / data-blocked-by 与 P 项 data-done 一致）
  C5  五层表状态机（同上）
  C6  页面「已实现 N/M 条」== 未实现表 done 行数 / 总行数
  C7  DOCS-HEALTH 块与 --sync 生成结果逐字一致（页面必须由脚本保真）
  C8  autodev SKILL 双副本规范化后逐行一致
  C9  孤儿文档（docs/*.md 在页面正文里 0 引用）-- WARN
  C10 页面 L1 行「N 测试 / M 段」与 tests/run_tests.bat、最新 make-test 日志一致
      （段数 -- FAIL，测试数 -- WARN：段数来源精确，测试数依赖日志解析）

C1 守卫（两条，缺一必假红）:
  (a) 只管「托管面」——文档、证据、四份入口文件。上游 QEMU 源码（`audio/noaudio.c`）、
      方案里尚未实施的产物（`tools/emulator_diff.py`）、仓外历史路径（`hda_tests/lib/*.bat`）
      一律不查，它们不是失效链接;
  (b) 行内抑制标记 `docs-check: ignore`——死链重定向表必须逐字写出旧的失效路径，
      那是描述而非活引用。外加 EXEMPT 词表（不存在/已删/…）覆盖变更记录里的历史说明。

C1 解析基准（层错引用必须保持红）:
  含 `/` 的引用按 基准目录 -> 引用文件自身所在目录（`docs/x.md` 里的 `../progress.html`）
  -> 外层工作区（仅当首段是 `.dsh`/`.zcode` 这类点目录）-> 仓库根（仅外层文件）依次尝试。
  内层文件**不回落外层工作区**——`progress.html` 把外层证据写成内层相对路径
  （`docs/evidence/2026-10-29/verdict-big.png`）正是要抓的层错，回落就漏。
  不含 `/` 的裸文件名只有 `decisions-*.md` / `tech-plan-*.md` 会被校验（落到仓库 docs/），
  其余裸名一律跳过（`外层\\.dsh\\skills\\autodev\\SKILL.md` 这种反斜杠路径会被截成
  `SKILL.md`，校验它只会制造假红）。

用法:
  uv run --no-project python tools/check_docs.py            # 检查（红则 exit 1）
  uv run --no-project python tools/check_docs.py --json     # 机器可读
  uv run --no-project python tools/check_docs.py --sync     # 重写 DOCS-HEALTH 块
  uv run --no-project python tools/check_docs.py --selftest # 10 例自检
"""
from __future__ import print_function

import argparse
import datetime
import fnmatch
import html as _html
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
WORKSPACE = REPO.parent

BEGIN_MARK = u"<!-- DOCS-HEALTH:BEGIN 由 tools/check_docs.py --sync 生成，勿手改 -->"
END_MARK = u"<!-- DOCS-HEALTH:END -->"

INNER_REFS = [u"progress.html", u"README.md"]
OUTER_REFS = [u"AGENTS.md", u"CODEBUDDY.md", u"docs/decisions-agent-loop.md"]
SKILL_REFS = [u".dsh/skills/autodev/SKILL.md", u".zcode/skills/autodev/SKILL.md"]

EXEMPT = (u"不存在", u"已删", u"删除", u"已废弃", u"废除", u"将删")
# 行内抑制标记：文档在「死链重定向表」里必须逐字写出失效的旧路径，
# 那是描述而不是活引用。显式标记胜过把规则放宽到整类文件。
IGNORE_MARK = u"docs-check: ignore"

DOC_EXTS = (u".md", u".png", u".ppm", u".log", u".svg", u".txt", u".json",
            u".py", u".bat", u".c", u".h", u".html")

# C1 只管「托管面」：文档、证据、以及四份入口文件。代码/工具/图件引用一律不查——
# 它们要么是上游 QEMU 源码（audio/noaudio.c、hw/audio/intel-hda.c，本仓不存在也不该存在），
# 要么是方案里尚未实施的产物（tools/emulator_diff.py 属 P13 未来工作），
# 要么是仓外历史路径（hda_tests/lib/*.bat 从未入库）。把这些算失效引用只会制造假红。
MANAGED_BASENAMES = (u"README.md", u"AGENTS.md", u"CODEBUDDY.md", u"SKILL.md", u"progress.html")
PROVENANCE = re.compile(u"\\[(?:Gitee|spec|wiki|实测)[^\\]]*\\]")

# 路径引用候选：含 `/` 或 `../` 前缀、带扩展名、字符集里没有 glob / 占位符 / 反斜杠
# 首段允许以点开头的隐藏目录（../.dsh/skills/autodev/SKILL.md），否则反斜杠路径会被截断成
# dsh/skills/... 而误报。
PATH_CAND = re.compile(u"(?:\\.{1,2}/)*[A-Za-z0-9_\\-.\u4e00-\u9fff][A-Za-z0-9_\\-./\u4e00-\u9fff]*\\.[A-Za-z0-9]+")
BARE_DOC = re.compile(u"^(?:decisions|tech-plan)-[A-Za-z0-9_.\\-]+\\.md$")

TR = re.compile(u"<tr([^>]*)>(.*?)</tr>", re.S)
TD = re.compile(u"<td[^>]*>(.*?)</td>", re.S)
ATTR = re.compile(u'(data-[a-z\\-]+)="([^"]*)"')
ITEM = re.compile(u'<li class="item"([^>]*)>', re.S)
TAG = re.compile(u"<[^>]+>")

STATED = re.compile(u"更新于\\s*(\\d{4}-\\d{2}-\\d{2})")
CHANGELOG = re.compile(u"<li><b>(\\d{4}-\\d{2}-\\d{2})</b>")
REFSCHECK = re.compile(u"refs-check(?:-([a-z0-9][a-z0-9\\-]*))?(?:\\.log)?\\s*(\\d+)\\s*/\\s*(\\d+)")
COUNTSUM = re.compile(u"已实现\\s*(\\d+)\\s*/\\s*(\\d+)\\s*条")
L1ROW = re.compile(u"(\\d+)\\s*测试\\s*/\\s*(\\d+)\\s*段")
FRESH = re.compile(u"test_[a-z0-9_]+_fresh")
TESTSLINE = re.compile(u"^\\s*(\\d+)\\s+Tests\\b", re.M)

MANIFEST = [
    (u"docs/decisions-audio-test.md", u"定案 D1–D12（音频测试口径）", u"file"),
    (u"docs/decisions-qemu-only.md", u"定案 D-Q1–D-Q10（QEMU-only）", u"file"),
    (u"docs/decisions-autodev.md", u"定案 D-A1–D-A15（autodev 流程与分支管辖）", u"file"),
    (u"docs/decisions-docs.md", u"定案 D-D1–D-D8（文档托管）", u"file"),
    (u"docs/decisions-adc.md", u"定案（ADC 历史，2026-10-28）", u"file"),
    (u"docs/tech-plan-*.md", u"方案（{n} 份）", u"glob"),
    (u"docs/evidence/", u"证据（{d} 目录 / {f} 文件）", u"dir"),
    (u"README.md", u"驱动说明入口", u"file"),
    (u"../AGENTS.md", u"交差硬规则表", u"outer"),
    (u"../CODEBUDDY.md", u"架构与常用命令", u"outer"),
    (u"../docs/decisions-agent-loop.md", u"agent loop 决策（历史）", u"outer"),
    (u"../.dsh/skills/autodev/SKILL.md", u"autodev 指令（准）", u"skill"),
    (u"../.zcode/skills/autodev/SKILL.md", u"autodev 指令（镜像）", u"skill"),
]


# ---------------------------------------------------------------- helpers

def esc(s):
    """控制台 ASCII 兜底：非 ASCII 转 \\uXXXX。"""
    try:
        return s.encode("unicode_escape").decode("ascii")
    except Exception:
        return repr(s)


def read_text(p):
    for enc in ("utf-8-sig", "utf-8", "gbk", "latin-1"):
        try:
            with open(str(p), "r", encoding=enc, newline="") as f:
                return f.read()
        except (UnicodeDecodeError, LookupError):
            continue
        except (IOError, OSError):
            return None
    return None


def write_text(p, text):
    with open(str(p), "w", encoding="utf-8", newline="") as f:
        f.write(text)


def strip_tags(s):
    return _html.unescape(TAG.sub(u"", s)).replace(u"\xa0", u" ").strip()


def attrs(tag_body):
    return dict(ATTR.findall(tag_body))


def _git_exe():
    """定位 git 可执行文件。

    不能只靠 PATH：`make docs-check` 跑在 `cmd /c "call setvars.bat /ni && ..."`
    之下，而 setvars.bat 会把 PATH 重写成 `%SystemRoot%` + 自带工具链，
    全局安装的 git 就此消失（实测 `git` 报 "not recognized"）。那样 ④ 文档健康
    的「最后更新」会从 git 日期悄悄退化成 mtime，而页面里的块是 `--sync` 在
    普通 shell 下生成的——两边不一致，C7 会在 make full 里红，而且原因藏得很深。
    所以这里显式探测常见安装位置，让结果与调用环境无关。
    """
    found = shutil.which("git")
    if found:
        return found
    for cand in (r"C:\Program Files\Git\cmd\git.exe",
                 r"C:\Program Files (x86)\Git\cmd\git.exe",
                 os.path.expanduser(r"~\AppData\Local\Programs\Git\cmd\git.exe"),
                 "/usr/bin/git",
                 "/usr/local/bin/git"):
        if os.path.isfile(cand):
            return cand
    return "git"


GIT = _git_exe()


def cmd_out(args):
    try:
        p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except (OSError, ValueError):
        return None, u""
    return p.returncode, p.stdout.decode("utf-8", "replace")


def git_date(repo, rel):
    rc, out = cmd_out([GIT, "-C", str(repo), "log", "-1", "--format=%ad",
                       "--date=short", "--", rel])
    if rc != 0:
        return None
    return out.strip() or None


def git_dirty(repo):
    rc, out = cmd_out([GIT, "-C", str(repo), "status", "--porcelain"])
    if rc != 0:
        return None
    dirty = set()
    for line in out.splitlines():
        if not line.strip():
            continue
        body = line[3:] if len(line) > 3 else line.strip()
        if u" -> " in body:
            body = body.split(u" -> ", 1)[1]
        dirty.add(body.strip().strip(u'"').replace(u"\\", u"/"))
    return dirty


def mdate(p):
    try:
        return datetime.date.fromtimestamp(os.path.getmtime(str(p))).isoformat()
    except (OSError, ValueError):
        return u"?"


def finding(level, code, where, msg):
    return {"level": level, "code": code, "where": where, "msg": msg}


# ---------------------------------------------------------------- C1

def managed(tok, repo_name=None):
    """只有「托管面」的路径参与 C1：doc 扩展名 + 已知入口/文档目录前缀。"""
    if repo_name and tok.startswith(repo_name + u"/"):
        tok = tok[len(repo_name) + 1:]  # 外层文档常带仓库目录名前缀
    base = os.path.basename(tok)
    if base in MANAGED_BASENAMES:
        return True
    if base.startswith(u"decisions-") and base.endswith(u".md"):
        return True
    if base.startswith(u"tech-plan-") and base.endswith(u".md"):
        return True
    if u"/" not in tok:
        return False
    head = tok.split(u"/", 1)[0]
    if head in (u"docs", u"..", u".", u"tests", u"tools"):
        # tests/ 与 tools/ 只收 .md/.log/.json/.bat 这类文档性产物，
        # 源码（.c/.h/.py）多半是引用尚未落地的实现或上游源码。
        ext = os.path.splitext(tok)[1].lower()
        return ext in (u".md", u".log", u".svg", u".png", u".txt")
    return False


def check_refs(repo, workspace):
    out = []
    seen = set()
    repo_name = repo.name
    sources = []
    for rel in INNER_REFS:
        p = repo / rel
        if p.is_file():
            sources.append((rel, p, repo, False))
    docs = sorted((repo / u"docs").glob(u"*.md")) if (repo / u"docs").is_dir() else []
    for p in docs:
        sources.append((u"docs/" + p.name, p, repo, False))
    for rel in OUTER_REFS + SKILL_REFS:
        p = workspace / rel
        if p.is_file():
            sources.append((rel, p, workspace, True))

    for rel, path, base, fallback in sources:
        text = read_text(path)
        if not text:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            if any(mark in line for mark in EXEMPT):
                continue
            if IGNORE_MARK in line:
                continue
            bare = PROVENANCE.sub(u"", line)  # 出处标记里的路径不是本仓引用
            for tok in PATH_CAND.findall(bare):
                tok = tok.strip()
                if u"/" not in tok and not BARE_DOC.match(tok):
                    continue
                if any(c in tok for c in u"*<>{}|$\\"):
                    continue
                ext = os.path.splitext(tok)[1].lower()
                if ext not in DOC_EXTS:
                    continue
                if not managed(tok, repo_name):
                    continue
                key = (rel, lineno, tok)
                if key in seen:
                    continue
                seen.add(key)
                ok = _resolves(tok, base, path.parent, repo, workspace, fallback, repo_name)
                if not ok:
                    out.append(finding("FAIL", "C1", u"%s:%d" % (rel, lineno),
                                       u"%s (not found)" % tok))
    return out


def _resolves(tok, base, here, repo, workspace, fallback, repo_name=None):
    """解析顺序固定，层错引用必须保持红。

    * 裸文件名（decisions-*/tech-plan-*）只在仓库 docs/ 下找;
    * 含 `/` 的：基准目录 -> 引用文件自身所在目录（`docs/x.md` 里的 `../progress.html`）
      -> 外层工作区（仅首段是 .dsh/.zcode 这类点目录时，因为内层文档会直接写外层相对路径）;
    * 外层文件（fallback=True）额外回落仓库根，因为它们常不带 `epos_Zht/` 前缀引用内层路径。
    注意：内层文件**不回落外层**——`docs/evidence/2026-10-29/verdict-big.png` 这种把外层证据
    写成内层相对路径的层错引用，回落就会漏掉（那条路径在外层工作区确实存在）。
    """
    if repo_name and tok.startswith(repo_name + u"/"):
        stripped = tok[len(repo_name) + 1:]
        return (repo / stripped).exists() or (workspace / stripped).exists()
    if BARE_DOC.match(tok) and u"/" not in tok:
        return (repo / u"docs" / tok).exists()
    if (base / tok).exists():
        return True
    if here is not None and (here / tok).exists():
        return True
    first = tok.split(u"/", 1)[0]
    if first.startswith(u".") and (workspace / tok).exists():
        return True
    if fallback and (repo / tok).exists():
        return True
    return False


# ---------------------------------------------------------------- C2

def check_stated_date(page):
    m = STATED.search(page)
    if not m:
        return [finding("FAIL", "C2", u"progress.html", u"no 更新于 <date> in header")]
    stated = m.group(1)
    head = page.find(u"变更记录")
    region = page[head:] if head >= 0 else page
    m2 = CHANGELOG.search(region)
    if not m2:
        return [finding("FAIL", "C2", u"progress.html", u"no <li><b>date</b> changelog entry")]
    first = m2.group(1)
    if stated != first:
        return [finding("FAIL", "C2", u"progress.html:header",
                        u"更新于 %s != changelog first %s" % (stated, first))]
    return []


# ---------------------------------------------------------------- C3

def check_refscheck(repo, page):
    out = []
    seen = set()
    for m in REFSCHECK.finditer(page):
        tag, cited = m.group(1), int(m.group(2))
        if not tag:
            continue  # 未点名的 refs-check N/N（历史条目）无法定位日志
        key = (tag, cited)
        if key in seen:
            continue
        seen.add(key)
        ev = repo / u"docs" / u"evidence"
        logs = sorted(ev.glob(u"*/refs-check-%s*" % tag)) if ev.is_dir() else []
        if not logs:
            out.append(finding("FAIL", "C3", u"refs-check-%s" % tag,
                               u"cited %d but no docs/evidence/*/refs-check-%s.log" % (cited, tag)))
            continue
        summary = None
        for lg in reversed(logs):
            body = read_text(lg) or u""
            mm = re.search(u"SUMMARY:\\s*(\\d+)\\s*PASS", body)
            if mm:
                summary = (int(mm.group(1)), lg.name, lg.parent.name, False)
                break
        if summary is None:
            # 早期 refs-check 日志没有 SUMMARY 行，只有若干 `PASS  ...` 明细。
            # 证据是 append-only 的（D-D7），不能为迁就脚本去改它，所以这里按 PASS 明细行
            # 计数，并额外报一条 WARN 说明该日志缺 SUMMARY。
            for lg in reversed(logs):
                body = read_text(lg) or u""
                n = len(re.findall(u"(?m)^PASS\\b", body))
                if n:
                    summary = (n, lg.name, lg.parent.name, True)
                    break
        if summary is None:
            out.append(finding("FAIL", "C3", logs[-1].name,
                               u"no SUMMARY and no PASS lines in refs-check-%s log" % tag))
            continue
        if summary[3]:
            out.append(finding("WARN", "C3", summary[1],
                               u"no SUMMARY line; counted %d PASS lines instead" % summary[0]))
        if cited != summary[0]:
            out.append(finding("FAIL", "C3", u"progress.html",
                               u"refs-check-%s cited %d/%s but %s/%s says %d PASS"
                               % (tag, cited, m.group(3), summary[2], summary[1], summary[0])))
    return out


# ---------------------------------------------------------------- C4/C5

def parse_items(page):
    items = {}
    for m in ITEM.finditer(page):
        a = attrs(m.group(1))
        pid = a.get(u"data-id")
        if pid:
            items[pid] = a
    return items


def parse_rows(page):
    rows = []
    for m in TR.finditer(page):
        cells = [strip_tags(c) for c in TD.findall(m.group(2))]
        if not cells:
            continue
        rows.append((attrs(m.group(1)), cells))
    return rows


def _blocked_ids(a):
    raw = a.get(u"data-blocked-by", u"")
    return [x for x in re.split(u"[,\\s]+", raw) if x]


def _state_row(a, cells, allowed, state_attr, code, items, where_attr):
    out = []
    state = a.get(state_attr)
    label = cells[0].strip()
    if not state:
        return [finding("FAIL", code, label,
                        u"row lacks %s (allowed: %s)" % (state_attr, u"/".join(sorted(allowed))))]
    if state not in allowed:
        return [finding("FAIL", code, label,
                        u"%s=%s not in %s" % (state_attr, state, u"/".join(sorted(allowed))))]
    ids = _blocked_ids(a)
    if state in (u"open", u"none"):
        if not ids:
            out.append(finding("FAIL", code, label,
                               u"%s=%s but no %s" % (state_attr, state, where_attr)))
        for pid in ids:
            if pid not in items:
                out.append(finding("FAIL", code, label, u"%s=%s unknown P id" % (where_attr, pid)))
            elif items[pid].get(u"data-done") != u"0":
                out.append(finding("FAIL", code, label,
                                   u"%s=%s but %s is done (data-done=%s)"
                                   % (where_attr, pid, pid, items[pid].get(u"data-done"))))
    else:
        for pid in ids:
            out.append(finding("FAIL", code, label,
                               u"state=%s must not carry %s=%s" % (state, where_attr, pid)))
    return out


def check_tables(page):
    items = parse_items(page)
    fails = []
    for a, cells in parse_rows(page):
        label = cells[0].strip()
        if re.match(u"^U-[A-Za-z]", label):
            fails += _state_row(a, cells, {u"done", u"open"}, u"data-state", "C4",
                                items, u"data-blocked-by")
        elif re.match(u"^L[1-5]$", label):
            fails += _state_row(a, cells, {u"done", u"partial", u"none"}, u"data-state", "C5",
                                items, u"data-blocked-by")
    return fails


# ---------------------------------------------------------------- C6

def check_count(page):
    m = COUNTSUM.search(page)
    has_cases = any(re.match(u"^U-[A-Za-z]", c[1][0].strip()) for c in parse_rows(page))
    if not m:
        # 空队列（SMP 族，D-A14）：页面没有用例表就没有计数句，不算错；
        # 有用例行却没写计数句才是漂移。
        if not has_cases:
            return []
        return [finding("FAIL", "C6", u"progress.html", u"no 已实现 N/M 条 sentence")]
    done = total = 0
    for a, cells in parse_rows(page):
        if re.match(u"^U-[A-Za-z]", cells[0].strip()):
            total += 1
            if a.get(u"data-state") == u"done":
                done += 1
    if (int(m.group(1)), int(m.group(2))) != (done, total):
        return [finding("FAIL", "C6", u"progress.html",
                        u"page says %s/%s but table has %d done / %d rows"
                        % (m.group(1), m.group(2), done, total))]
    return []


# ---------------------------------------------------------------- C9 / C10

def manifest_covers(rel):
    for path, _role, kind in MANIFEST:
        if kind == u"glob":
            pattern = path[len(u"docs/"):] if path.startswith(u"docs/") else path
            if fnmatch.fnmatch(rel.split(u"/")[-1], pattern):
                return True
        elif path == rel:
            return True
    return False


def check_orphans(repo, page):
    """孤儿文档 = 既不在文档归属表里，页面正文也没提到。

    归属表（MANIFEST）本身就是「这份文档归 DSH 管」的登记册：新建文档必须登记，
    否则它既不上 ④ 文档健康，也没有入口，等于悄悄存在。历史文档（decisions-adc.md
    这类）已在表内，故不会再被反复提醒。
    """
    out = []
    body = page
    i, j = page.find(BEGIN_MARK), page.find(END_MARK)
    if i >= 0 and j > i:
        body = page[:i] + page[j + len(END_MARK):]
    if not (repo / u"docs").is_dir():
        return out
    for p in sorted((repo / u"docs").glob(u"*.md")):
        rel = u"docs/" + p.name
        if rel in body or manifest_covers(rel):
            continue
        out.append(finding("WARN", "C9", rel,
                           u"not in docs manifest and 0 references from progress.html body"))
    return out


def check_l1(repo, page):
    out = []
    m = L1ROW.search(page)
    bat = repo / u"tests" / u"run_tests.bat"
    bat_text = read_text(bat) if bat.is_file() else None
    if m and bat_text is not None:
        segs = len(set(FRESH.findall(bat_text)))
        if segs != int(m.group(2)):
            out.append(finding("FAIL", "C10", u"progress.html L1 row",
                               u"page says %s 段 but run_tests.bat has %d" % (m.group(2), segs)))
    elif m:
        out.append(finding("WARN", "C10", u"tests/run_tests.bat", u"missing, skipped 段数"))
    ev = repo / u"docs" / u"evidence"
    logs = []
    if ev.is_dir():
        for d in sorted(ev.iterdir()):
            if d.is_dir():
                for f in d.glob(u"*make-test*.log"):
                    logs.append((os.path.getmtime(str(f)), f))
    if m and logs:
        logs.sort()
        body = read_text(logs[-1][1]) or u""
        nums = [int(x) for x in TESTSLINE.findall(body)]
        if nums:
            total = sum(nums)
            if total != int(m.group(1)):
                out.append(finding("WARN", "C10", logs[-1][1].name,
                                   u"page says %s 测试 but log sums %d" % (m.group(1), total)))
        else:
            out.append(finding("WARN", "C10", logs[-1][1].name, u"SKIP C10: no 'N Tests' line"))
    elif m:
        out.append(finding("WARN", "C10", u"docs/evidence", u"SKIP C10: no make-test log"))
    return out


# ---------------------------------------------------------------- C8

def norm_skill(text):
    return (text or u"").replace(u"AskUserQuestion", u"ask_user_question")


def check_skill(workspace):
    a = workspace / SKILL_REFS[0]
    b = workspace / SKILL_REFS[1]
    if not a.is_file() or not b.is_file():
        return [finding("WARN", "C8", u"skill", u"one copy missing, skipped")]
    ta = norm_skill(read_text(a))
    tb = norm_skill(read_text(b))
    la, lb = ta.splitlines(), tb.splitlines()
    for i in range(max(len(la), len(lb))):
        x = la[i] if i < len(la) else u"<eof>"
        y = lb[i] if i < len(lb) else u"<eof>"
        if x != y:
            return [finding("FAIL", "C8", u"SKILL.md:%d" % (i + 1),
                            u"copies differ: .dsh=%r .zcode=%r"
                            % (x[:60], y[:60]))]
    return []


# ---------------------------------------------------------------- C7 / health block

def render_health(repo, workspace, fails, warns):
    rows = []
    per_file = {}
    for f in fails:
        if f["code"] == "C1":
            src = f["where"].rsplit(u":", 1)[0]
            per_file[src] = per_file.get(src, 0) + 1
    plans = sorted((repo / u"docs").glob(u"tech-plan-*.md")) if (repo / u"docs").is_dir() else []
    ev = repo / u"docs" / u"evidence"
    ev_dirs = [d for d in sorted(ev.iterdir()) if d.is_dir()] if ev.is_dir() else []
    ev_files = 0
    for d in ev_dirs:
        ev_files += len([f for f in d.iterdir() if f.is_file()])
    today = datetime.date.today().isoformat()

    def last_update(base, rel, is_outer):
        """「最后更新」= 该文件最后一次 git 提交的日期；取不到（外层非仓库）才用 mtime。

        这里**故意不掺工作树脏标记**：本块是提交进仓库的，一旦把「脏」写进块里，
        提交完成的那一刻块就与现场不符，下一次 `make docs-check` 必然红——等于每轮
        提交都给自己埋一颗 C7 地雷。块只记「已提交内容」的时间，工作树状态交给
        `git status`，两者不混。
        """
        p = base / rel
        if not p.exists():
            return u"-"
        d = None if is_outer else git_date(repo, rel)
        if d:
            return d
        return mdate(p) + u" (mtime)"

    for path, role, kind in MANIFEST:
        if kind == u"glob":
            if plans:
                role = role.replace(u"{n}", str(len(plans)))
                newest = max(plans, key=lambda p: os.path.getmtime(str(p)))
                when = mdate(newest) + u" (mtime)"
            else:
                role, when = role.replace(u"{n}", u"0"), u"-"
            nbad = sum(per_file.get(u"docs/" + p.name, 0) for p in plans)
        elif kind == u"dir":
            role = role.replace(u"{d}", str(len(ev_dirs))).replace(u"{f}", str(ev_files))
            when = ev_dirs[-1].name if ev_dirs else u"-"
            nbad = 0
        elif kind in (u"outer", u"skill"):
            # MANIFEST 里的 `../x` 是相对内层仓库根的写法，落到外层工作区要去掉前缀
            rel_ws = path[3:] if path.startswith(u"../") else path
            target = workspace / rel_ws
            when = (mdate(target) + u" (mtime)") if target.exists() else u"-"
            nbad = per_file.get(rel_ws, 0)
        else:
            when = last_update(repo, path, False)
            nbad = per_file.get(path, 0)
        rows.append(u"        <tr><td><code>%s</code></td><td>%s</td><td>%s</td><td>%d</td><td>DSH</td></tr>"
                    % (_html.escape(path), _html.escape(role), _html.escape(when), nbad))

    nfail = len([f for f in fails if f["code"] != "C7"])
    nwarn = len([f for f in warns if f["code"] != "C7"])
    lines = [
        BEGIN_MARK,
        u'  <div class="table-wrap">',
        u'    <table>',
        u'      <thead><tr><th>文档</th><th>角色</th><th>最后更新</th><th>失效引用</th><th>维护</th></tr></thead>',
        u'      <tbody>',
    ] + rows + [
        u'      </tbody>',
        u'    </table>',
        u'  </div>',
        u'  <p class="small">自检 <code>tools/check_docs.py --sync</code> 生成：C1–C10 中除本块保真（C7）外 '
        u'<b>%d 失败 / %d 警告</b> · 生成于 %s · 红了先修再 --sync</p>' % (nfail, nwarn, today),
        END_MARK,
    ]
    return u"\n".join(lines)


def check_health(page, repo, workspace, fails, warns):
    i, j = page.find(BEGIN_MARK), page.find(END_MARK)
    if i < 0 or j < 0 or j < i:
        return [finding("FAIL", "C7", u"progress.html",
                        u"no DOCS-HEALTH block (BEGIN/END markers missing)")]
    body = page[i + len(BEGIN_MARK):j]
    want = render_health(repo, workspace, fails, warns)
    want_body = want[len(BEGIN_MARK):want.rfind(END_MARK)]
    have_body = body
    if have_body.strip() != want_body.strip():
        return [finding("FAIL", "C7", u"progress.html:health",
                        u"block differs from --sync output (%d vs %d lines)"
                        % (len(have_body.splitlines()), len(want_body.splitlines())))]
    return []


# ---------------------------------------------------------------- aggregate

def check(repo, workspace):
    raw = []
    page_path = repo / u"progress.html"
    page = read_text(page_path)
    if page is None:
        return [finding("FAIL", "C0", u"progress.html", u"unreadable")], []
    raw += check_refs(repo, workspace)
    raw += check_stated_date(page)
    raw += check_refscheck(repo, page)
    raw += check_tables(page)
    raw += check_count(page)
    raw += check_skill(workspace)
    raw += check_orphans(repo, page)
    raw += check_l1(repo, page)
    # 先按已确定的 fail/warn 渲染健康块，再校验块保真（C7 自己必然是 fail 级）。
    fails = [f for f in raw if f["level"] == "FAIL"]
    warns = [f for f in raw if f["level"] != "FAIL"]
    fails += check_health(page, repo, workspace, fails, warns)
    return fails, warns


def counts(fails, warns):
    out = {}
    for code in [u"C%d" % i for i in range(1, 11)]:
        out[code] = [len([f for f in fails if f["code"] == code]),
                     len([w for w in warns if w["code"] == code])]
    return out


def sync_block(repo):
    page_path = repo / u"progress.html"
    page = read_text(page_path)
    if page is None:
        print(u"docs-check: cannot read progress.html")
        return 1
    i, j = page.find(BEGIN_MARK), page.find(END_MARK)
    if i < 0 or j < 0 or j < i:
        print(u"docs-check: no DOCS-HEALTH markers; add the section first")
        return 1
    fails, warns = check(repo, WORKSPACE)
    block = render_health(repo, WORKSPACE, fails, warns)
    new = page[:i] + block + page[j + len(END_MARK):]
    if new == page:
        return 0
    write_text(page_path, new)
    return 0


def report(repo, workspace, fails, warns, elapsed):
    print(u"docs-check: repo=%s workspace=%s" % (esc(str(repo)), esc(str(workspace))))
    for f in fails:
        print(u"FAIL %-3s %s -> %s" % (f["code"], esc(f["where"]), esc(f["msg"])))
    for w in warns:
        print(u"WARN %-3s %s -> %s" % (w["code"], esc(w["where"]), esc(w["msg"])))
    c = counts(fails, warns)
    print(u"--- checks ---")
    for code in [u"C%d" % i for i in range(1, 11)]:
        nf, nw = c[code]
        state = u"FAIL" if nf else (u"WARN" if nw else u"ok")
        print(u"%-4s %-5s %d fail / %d warn" % (code, state, nf, nw))
    ok = not fails
    print(u"RESULT %s (%d fail / %d warn) %.2fs"
          % (u"PASS" if ok else u"FAIL", len(fails), len(warns), elapsed))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=u"docs and progress page consistency check")
    ap.add_argument("--json", action="store_true", help=u"machine readable output")
    ap.add_argument("--sync", action="store_true", help=u"rewrite the DOCS-HEALTH block")
    ap.add_argument("--selftest", action="store_true", help=u"run built-in self tests")
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    t0 = time.time()
    if args.sync:
        rc = sync_block(REPO)
        if rc:
            return rc
    fails, warns = check(REPO, WORKSPACE)
    if args.json:
        print(json.dumps({
            "repo": str(REPO), "workspace": str(WORKSPACE),
            "pass": not fails, "fails": fails, "warns": warns,
            "counts": counts(fails, warns),
            "elapsed_ms": int((time.time() - t0) * 1000),
        }, indent=2, sort_keys=True))
        return 0 if not fails else 1
    return report(REPO, WORKSPACE, fails, warns, time.time() - t0)


# ---------------------------------------------------------------- selftest

def _page(state_overrides=None, count=None, refs=None, stated=None, block=True,
          changelog_date=None, l1=None):
    today = u"2026-11-12"
    stated = stated or today
    changelog_date = changelog_date or today
    l1 = l1 or u"部分 · 37 测试 / 6 段"
    count = count or u"已实现 3/3 条"
    refs = refs or u"refs-check-p10.log 31/31 PASS"
    so = state_overrides or {}
    def row(label, layer, state, blocked=None, done_badge=u"已实现"):
        a = u' data-state="%s"' % state
        if blocked:
            a += u' data-blocked-by="%s"' % blocked
        badge = u'<span class="badge ok">已实现</span>' if state == u"done" else u'<span class="badge">%s</span>' % done_badge
        return u'<tr%s><td>%s</td><td>%s</td><td>%s</td><td>-</td></tr>' % (a, label, layer, badge)
    body = [
        u'<!doctype html><html><head><meta charset="utf-8"><title>t</title></head><body>',
        u'<header><div class="sub">唯一进度文档 · 更新于 %s · 状态写在每条目的 <code>data-done</code> 里</div></header>' % stated,
        u'<ul class="items">',
        u'<li class="item" data-id="P10" data-stage="0" data-done="1">P10</li>',
        u'<li class="item" data-id="P11" data-stage="0" data-done="1" data-dep="P10">P11</li>',
        u'<li class="item" data-id="P13" data-stage="0" data-done="0" data-optional="1">P13</li>',
        u'</ul>',
        u'<table><thead><tr><th>层</th><th>入口</th><th>覆盖</th><th>预算</th><th>状态</th></tr></thead><tbody>',
        u'<tr data-state="%s"><td>L1</td><td><code>make test</code></td><td>x</td><td>2s</td><td>%s</td></tr>'
        % (so.get(u"L1", u"partial"), l1),
        u'<tr data-state="%s"><td>L2</td><td><code>make qtest</code></td><td>x</td><td>s</td><td>部分</td></tr>' % so.get(u"L2", u"partial"),
        u'<tr data-state="%s"%s><td>L3</td><td><code>make full</code></td><td>x</td><td>60s</td><td>已完成</td></tr>'
        % (so.get(u"L3", u"done"), (u' data-blocked-by="%s"' % so[u"L3-blocked"]) if u"L3-blocked" in so else u""),
        u'<tr data-state="%s"><td>L4</td><td>QEMU 自检</td><td>x</td><td>2s</td><td>已完成</td></tr>' % so.get(u"L4", u"done"),
        u'</tbody></table>',
        u'<table><thead><tr><th>用例</th><th>层</th><th>状态</th><th>卡在哪</th></tr></thead><tbody>',
    ]
    for label, layer, state, blocked, badge in [
            (u"U-L1-01…03", u"L1", so.get(u"U-L1-01", u"done"), None, u"已实现"),
            (u"U-L2-01 / 02", u"L2", so.get(u"U-L2-01", u"done"), None, u"已实现"),
            (u"U-L3-01 / 02", u"L3", so.get(u"U-L3-01", u"done"), so.get(u"U-L3-01-blocked"), u"空着")]:
        body.append(row(label, layer, state, blocked, badge))
    body += [
        u'</tbody></table>',
        u'<p class="small">%s</p>' % count,
        u'<p><code>%s</code></p>' % refs,
        u'<p>另见 <code>docs/decisions-docs.md</code> 与 <code>docs/tech-plan-x.md</code>；'
        u'历史见 <code>docs/decisions-adc.md</code>、<code>docs/decisions-audio-test.md</code>、'
        u'<code>docs/decisions-qemu-only.md</code>、<code>docs/decisions-autodev.md</code>。</p>',
    ]
    if block:
        body.append(u'<section><div class="sec-head"><h2>④ 文档健康</h2></div>')
        body.append(u'<!-- DOCS-HEALTH:PLACEHOLDER -->')
        body.append(u'</section>')
    body += [
        u'<h2>变更记录</h2><ul>',
        u'<li><b>%s</b> x</li>' % changelog_date,
        u'</ul></body></html>',
    ]
    return u"\n".join(body)


def _mk(root, mutate=None):
    """建最小 fixture 树：root/epos_Zht 为仓库根，root 为外层工作区。"""
    repo = root / u"epos_Zht"
    ws = root
    for d in [repo / u"docs" / u"evidence" / u"2026-11-12", repo / u"tests", repo / u"tools",
              ws / u"docs", ws / u".dsh" / u"skills" / u"autodev",
              ws / u".zcode" / u"skills" / u"autodev"]:
        d.mkdir(parents=True, exist_ok=True)
    write_text(repo / u"tools" / u"check_docs.py", u"# stub\n")
    write_text(repo / u"README.md", u"# t\n\n见 progress.html 与 docs/tech-plan-x.md。\n")
    for name in (u"decisions-docs.md", u"decisions-adc.md", u"decisions-audio-test.md",
                 u"decisions-qemu-only.md", u"decisions-autodev.md", u"tech-plan-x.md"):
        write_text(repo / u"docs" / name, u"# doc\n")
    write_text(repo / u"docs" / u"evidence" / u"2026-11-12" / u"refs-check-p10.log",
               u"SUMMARY: 31 PASS / 0 FAIL\n")
    write_text(repo / u"docs" / u"evidence" / u"2026-11-12" / u"make-test.log",
               u"5 Tests 0 Failures\n2 Tests 0 Failures\n8 Tests 0 Failures\n"
               u"8 Tests 0 Failures\n10 Tests 0 Failures\n4 Tests 0 Failures\n")
    write_text(repo / u"tests" / u"run_tests.bat",
               u"\n".join([u"tests\\test_%d_fresh.exe" % i for i in range(1, 7)]) + u"\n")
    write_text(ws / u"AGENTS.md", u"# a\n")
    write_text(ws / u"CODEBUDDY.md", u"# c\n")
    write_text(ws / u"docs" / u"decisions-agent-loop.md", u"# l\n")
    write_text(ws / u".dsh" / u"skills" / u"autodev" / u"SKILL.md", u"name: autodev\nask_user_question\n")
    write_text(ws / u".zcode" / u"skills" / u"autodev" / u"SKILL.md", u"name: autodev\nAskUserQuestion\n")
    page = _page()
    if mutate:
        page = mutate(page)
    write_text(repo / u"progress.html", page)
    # 用 --sync 的渲染结果填块，保证基线是绿的
    fails, warns = check(repo, ws)
    block = render_health(repo, ws, fails, warns)
    page = page.replace(u"<!-- DOCS-HEALTH:PLACEHOLDER -->", block)
    write_text(repo / u"progress.html", page)
    return repo, ws


def _codes(findings):
    return sorted(set(f["code"] for f in findings))


def selftest():
    cases = []
    tmp = tempfile.mkdtemp(prefix=u"docscheck")

    def case(name):
        def deco(fn):
            cases.append((name, fn))
            return fn
        return deco

    @case(u"baseline-green")
    def _(root):
        repo, ws = _mk(root)
        f, w = check(repo, ws)
        return f, w, []

    @case(u"C1-dangling-inner-ref")
    def _(root):
        repo, ws = _mk(root, lambda p: p.replace(u"另见 <code>docs/decisions-docs.md</code>",
                                                 u"另见 <code>docs/missing.md</code>"))
        return check(repo, ws) + (["C1"],)

    @case(u"C1-outer-ref-fallback")
    def _(root):
        repo, ws = _mk(root)
        write_text(ws / u"AGENTS.md", u"# a\n\n见 epos_Zht/docs/nope.md 一节。\n")
        return check(repo, ws) + (["C1"],)

    @case(u"C2-stated-date")
    def _(root):
        repo, ws = _mk(root, lambda p: p.replace(u"更新于 2026-11-12", u"更新于 2020-01-01"))
        return check(repo, ws) + (["C2"],)

    @case(u"C3-refscheck-count")
    def _(root):
        repo, ws = _mk(root, lambda p: p.replace(u"31/31", u"24/24"))
        return check(repo, ws) + (["C3"],)

    @case(u"C4-open-blocked-by-done")
    def _(root):
        repo, ws = _mk(root, lambda p: p.replace(
            u'<tr data-state="done"><td>U-L3-01 / 02</td>',
            u'<tr data-state="open" data-blocked-by="P10"><td>U-L3-01 / 02</td>'))
        return check(repo, ws) + (["C4"],)

    @case(u"C5-none-without-blocked-by")
    def _(root):
        repo, ws = _mk(root, lambda p: p.replace(
            u'<tr data-state="done"><td>L3</td>', u'<tr data-state="none"><td>L3</td>'))
        return check(repo, ws) + (["C5"],)

    @case(u"C6-count-sentence")
    def _(root):
        repo, ws = _mk(root, lambda p: p.replace(u"已实现 3/3 条", u"已实现 2/3 条"))
        return check(repo, ws) + (["C6"],)

    @case(u"C7-block-tampered")
    def _(root):
        repo, ws = _mk(root)
        p = repo / u"progress.html"
        write_text(p, read_text(p).replace(u'<th>失效引用</th>', u'<th>漂移</th>'))
        return check(repo, ws) + (["C7"],)

    @case(u"C8-skill-drift")
    def _(root):
        repo, ws = _mk(root)
        write_text(ws / u".zcode" / u"skills" / u"autodev" / u"SKILL.md",
                   u"name: autodev\nAskUserQuestion extra\n")
        return check(repo, ws) + (["C8"],)

    npass = 0
    print(u"selftest: %d cases" % len(cases))
    for name, fn in cases:
        root = Path(tempfile.mkdtemp(dir=tmp))
        try:
            fails, warns, expect = fn(root)
            got = _codes(fails)
            if expect:
                ok = all(e in got for e in expect)
                detail = u"expect %s got %s" % (u",".join(expect), u",".join(got))
            else:
                ok = not fails and not warns
                detail = u"expect green, got fail=%s warn=%s" % (u",".join(got), u",".join(_codes(warns)))
            if ok:
                npass += 1
                print(u"PASS %s" % name)
            else:
                print(u"FAIL %s: %s" % (name, esc(detail)))
        except Exception as exc:  # noqa: BLE001 - selftest 要把异常当失败看
            print(u"FAIL %s: exception %s" % (name, esc(repr(exc))))
        finally:
            shutil.rmtree(str(root), ignore_errors=True)
    shutil.rmtree(tmp, ignore_errors=True)
    print(u"selftest RESULT: %d/%d pass" % (npass, len(cases)))
    return 0 if npass == len(cases) else 1


if __name__ == u"__main__":
    sys.exit(main())
