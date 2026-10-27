#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Self-consistency check for docs/test-plan-audio.md.

NOTE 2026-10-28: that plan file was deleted, so this script is currently NOT
wired into make test (see tests/run_tests.bat). It is kept so that restoring a
plan with the same section layout re-enables the check without rewriting it.


Checks structure only (never facts). Seven rules:
  1. behaviors B1..B6 are contiguous and every one is claimed by some layer
  2. every case row has a layer and a reverse gate, and cites defined behaviors
  3. every gate points at an existing case; every case except manual L5 has a gate
  4. every golden-vector scenario binds at least one case
  5. L1+L2+L3 budgets sum to <= 60s (D2)
  6. no status wording (this doc records no progress)
  7. case ids are unique

Output is ASCII-only on purpose: the log is redirected into a codepage-bound
console, and non-ASCII bytes turn into mojibake there.

Usage:  python tests/tools/check_plan.py [plan.md]
Exit:   0 pass, 1 fail (each failing rule printed).
"""
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_PLAN = REPO / "docs" / "test-plan-audio.md"

BANNED = ["已落地", "已完成", "已跑通", "现已通过"]


def section(lines, start_kw):
    out, on = [], False
    for ln in lines:
        if not on and re.match(r"^#{1,3} ", ln) and start_kw in ln:
            on = True
            continue
        if on and re.match(r"^#{1,3} ", ln):
            break
        if on:
            out.append(ln)
    return out


def cells(line):
    return [c.strip() for c in line.strip().strip("|").split("|")]


def main():
    path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PLAN
    if not path.is_file():
        print(f"FAIL: plan not found: {path}")
        return 1

    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    fails = []

    beh = [re.match(r"\|\s*(B\d)\s*\|", l).group(1)
           for l in section(lines, "1. 被测行为") if re.match(r"\|\s*B\d\s*\|", l)]
    if beh != ["B1", "B2", "B3", "B4", "B5", "B6"]:
        fails.append(f"S1 behavior ids not contiguous: {beh}")

    cover, budget = {}, {}
    for l in section(lines, "2. 分层、入口与预算"):
        m = re.match(r"\|\s*(L\d)\s*\|", l)
        if not m:
            continue
        c = cells(l)
        cover[m.group(1)] = set(re.findall(r"B\d", c[2]))
        bm = re.search(r"≤(\d+)s", c[3])
        if bm:
            budget[m.group(1)] = int(bm.group(1))
    for b in beh:
        if not any(b in s for s in cover.values()):
            fails.append(f"{b} not claimed by any layer")
    auto = sum(budget.get(k, 0) for k in ("L1", "L2", "L3"))
    if auto > 60:
        fails.append(f"auto-layer budget {auto}s exceeds 60s")
    if budget.get("L3", 0) >= 60:
        fails.append("L3 budget alone eats the whole 60s")

    cases = {}
    for l in section(lines, "3. 用例矩阵"):
        if not l.strip().startswith("|"):
            continue
        c = cells(l)
        ids = re.findall(r"U-L\d-\d\d", c[0]) if c else []
        if not ids:
            continue
        if len(c) < 6:
            fails.append(f"{ids[0]} has too few columns ({len(c)})")
            continue
        uid = ids[0]
        if uid in cases:
            fails.append(f"duplicate case id: {uid}")
        cases[uid] = {"beh": set(re.findall(r"B\d", c[2])), "gate": c[4]}
        for b in cases[uid]["beh"]:
            if b not in beh:
                fails.append(f"{uid} references undefined behavior {b}")
        if not cases[uid]["gate"]:
            fails.append(f"{uid} has no reverse gate")
    if not cases:
        fails.append("S3 parsed no cases")

    gates, hit = {}, set()
    for l in section(lines, "6. 反向门"):
        m = re.match(r"\|\s*(G\d+)\s*\|", l)
        if not m:
            continue
        refs = set(re.findall(r"U-L\d-\d\d", cells(l)[2]))
        gates[m.group(1)] = refs
        hit |= refs
        for r in refs:
            if r not in cases:
                fails.append(f"{m.group(1)} points to unknown case {r}")
    for uid in cases:
        if uid == "U-L5-01":
            continue
        if uid not in hit:
            fails.append(f"{uid} has no matching gate")

    for l in section(lines, "4. 黄金向量"):
        if not re.match(r"\|\s*`?[a-z0-9_]+`?\s*\|", l):
            continue
        refs = re.findall(r"U-L\d-\d\d", l)
        if not refs:
            fails.append(f"scenario binds no case: {l[:48]}")
        for r in refs:
            if r not in cases:
                fails.append(f"scenario references unknown case {r}")

    for w in BANNED:
        if w in text:
            fails.append(f"status wording found: {w}")

    print(f"file={path.name} lines={len(lines)} bytes={path.stat().st_size}")
    print(f"behaviors={len(beh)} cases={len(cases)} gates={len(gates)} "
          f"budget(L1+L2+L3)={auto}s")
    if fails:
        print(f"RESULT FAIL ({len(fails)})")
        for f in fails:
            print(" -", f)
        return 1
    print("RESULT PASS: behaviors/layers/cases/gates/scenarios/budget/wording OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
