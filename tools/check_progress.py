#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Terminal-state check for progress.html: does /autodev still have work?

Decides one of three states before any item is picked, so "everything is
done" is an executable branch instead of a feeling.

  ELIGIBLE  some pending item is workable right now        -> pick it
  BLOCKED   pending items exist but none is workable       -> ask a human
  TERMINAL  every pending item is an intentional optional  -> stop, report

State is decided here and nowhere else: progress.html only carries the raw
attributes, the page's JavaScript never recomputes the verdict, so there is
exactly one implementation.

Attributes read from each <li class="item">:
  data-id        required, unique
  data-done      required, "0" or "1"
  data-dep       optional, space separated ids that must all be done first
  data-optional  optional, "1" means an intentional optional tier: never
                 auto-picked, and it does not block TERMINAL

Output is ASCII-only on purpose: the evidence log is redirected into a
codepage-bound console, and non-ASCII bytes turn into mojibake there (same
reason as tests/tools/check_plan.py:19). The Chinese wording a human sees is
SKILL.md section 2.5's job, mapping the state printed here.

Usage:
  python tools/check_progress.py
  python tools/check_progress.py --json
  python tools/check_progress.py --file other.html
  python tools/check_progress.py --selftest
Exit: 0 = one of the three states decided, 1 = structural error / unusable.
"""
import argparse
import json
import pathlib
import sys
import tempfile
from html.parser import HTMLParser

REPO = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_FILE = REPO / "progress.html"

ELIGIBLE = "ELIGIBLE"
BLOCKED = "BLOCKED"
TERMINAL = "TERMINAL"


class ItemParser(HTMLParser):
    """Collects the attribute dict of every <li> whose class list has "item"."""

    def __init__(self):
        HTMLParser.__init__(self)
        self.items = []

    def handle_starttag(self, tag, attrs):
        if tag != "li":
            return
        found = {}
        for key, value in attrs:
            found[key] = value
        # Split the class list: the page also has <ul class="items">, so a
        # substring test would swallow the container.
        if "item" in (found.get("class") or "").split():
            self.items.append(found)


def parse_items(path):
    parser = ItemParser()
    parser.feed(path.read_text(encoding="utf-8"))
    parser.close()
    return parser.items


def normalize(raw_items):
    items = []
    for raw in raw_items:
        items.append({
            "id": raw.get("data-id"),
            "done": raw.get("data-done"),
            "dep": (raw.get("data-dep") or "").split(),
            "optional": raw.get("data-optional") == "1",
        })
    return items


def validate(items):
    """Returns a list of structural problems; empty means the input is usable.

    空队列是合法态（SMP 分支 Sxx 队列立基后等待用户发需求，D-A14/D-A15），
    不算结构错误：判定走 BLOCKED（无可做项、无悬置项、也非全部完成）。
    """
    fails = []

    by_id = {}
    for index, item in enumerate(items):
        item_id = item["id"]
        if not item_id:
            fails.append("item #%d has no data-id" % index)
            continue
        if item_id in by_id:
            fails.append("duplicate data-id %s" % item_id)
        by_id[item_id] = item
        if item["done"] is None:
            fails.append("%s has no data-done" % item_id)
        elif item["done"] not in ("0", "1"):
            fails.append("%s has data-done=%r (want 0 or 1)" % (item_id, item["done"]))
        if item["optional"] and item["done"] == "1":
            fails.append("%s is data-optional=1 and data-done=1" % item_id)

    for item in items:
        if not item["id"]:
            continue
        for dep in item["dep"]:
            if dep == item["id"]:
                fails.append("%s depends on itself" % item["id"])
            elif dep not in by_id:
                fails.append("%s depends on unknown %s" % (item["id"], dep))

    if not fails:
        cycle = find_cycle(by_id)
        if cycle is not None:
            fails.append("dependency cycle: %s" % cycle)
    return fails


def find_cycle(by_id):
    """Depth-first colouring. Returns a printable cycle path, or None."""
    white, grey, black = 0, 1, 2
    colour = dict((key, white) for key in by_id)
    stack = []

    def walk(node):
        colour[node] = grey
        stack.append(node)
        for dep in by_id[node]["dep"]:
            if colour[dep] == grey:
                return " -> ".join(stack[stack.index(dep):] + [dep])
            if colour[dep] == white:
                hit = walk(dep)
                if hit is not None:
                    return hit
        stack.pop()
        colour[node] = black
        return None

    for node in by_id:
        if colour[node] == white:
            hit = walk(node)
            if hit is not None:
                return hit
    return None


def decide(items):
    by_id = {}
    for item in items:
        by_id[item["id"]] = item

    eligible = []
    held = []
    for item in items:
        if item["done"] == "1":
            continue
        if item["optional"]:
            held.append({"id": item["id"], "why": "optional"})
            continue
        missing = [dep for dep in item["dep"] if by_id[dep]["done"] != "1"]
        if missing:
            held.append({"id": item["id"], "why": "dep not done: " + ",".join(missing)})
            continue
        eligible.append(item["id"])

    done = sum(1 for item in items if item["done"] == "1")
    blocked = [entry for entry in held if entry["why"] != "optional"]
    if eligible:
        state = ELIGIBLE
    elif blocked:
        state = BLOCKED
    elif not items:
        # 空队列 = 等待用户发需求（SMP 分支 Sxx 立基后初始态，D-A14/D-A15）。
        # 不是 TERMINAL——TERMINAL 的收工句式是「当前已完成」，对空队列是误导。
        state = BLOCKED
    else:
        state = TERMINAL

    return {
        "state": state,
        "id": eligible[0] if eligible else None,
        "eligible": eligible,
        "held": held,
        "done": done,
        "total": len(items),
    }


def check(path):
    """Returns (exit_code, result_or_None, fails)."""
    if not path.exists():
        return 1, None, ["file not found: %s" % path]
    try:
        items = normalize(parse_items(path))
    except (OSError, UnicodeDecodeError) as exc:
        return 1, None, ["cannot read %s: %s" % (path, exc)]
    fails = validate(items)
    if fails:
        return 1, None, fails
    return 0, decide(items), []


def render_text(result):
    lines = ["state=%s id=%s done=%d/%d" % (
        result["state"], result["id"] or "-", result["done"], result["total"])]
    lines.append("eligible: " + (" ".join(result["eligible"]) or "-"))
    if result["held"]:
        for entry in result["held"]:
            lines.append("held: %s (%s)" % (entry["id"], entry["why"]))
    else:
        lines.append("held: -")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# selftest: fixtures live in a temp dir, never in the repo
# --------------------------------------------------------------------------

def build_html(rows):
    parts = ['<!doctype html><html><body><ul class="items">']
    for row in rows:
        attrs = ['class="item"']
        for key in sorted(row):
            attrs.append('%s="%s"' % (key, row[key]))
        parts.append("<li %s></li>" % " ".join(attrs))
    parts.append("</ul></body></html>")
    return "".join(parts)


def selftest_cases():
    done = {"data-id": "P10", "data-done": "1"}
    return [
        ("all done is terminal",
         [{"data-id": "P02", "data-done": "1"}], TERMINAL, 0),
        ("single pending with no dep is eligible",
         [{"data-id": "P11", "data-done": "0"}], ELIGIBLE, 0),
        ("pending whose dep is done is eligible",
         [done, {"data-id": "P11", "data-done": "0", "data-dep": "P10"}], ELIGIBLE, 0),
        ("several pending: earliest in document order wins",
         [{"data-id": "P11", "data-done": "0"}, {"data-id": "P12", "data-done": "0"}],
         ELIGIBLE, 0),
        # BLOCKED needs a pending item that is not itself workable: the only way
        # is a dep that is held (optional). A plain unfinished dep is workable,
        # so that chain yields ELIGIBLE on the dep itself.
        ("pending on an optional dep is blocked",
         [{"data-id": "P13", "data-done": "0", "data-optional": "1"},
          {"data-id": "P11", "data-done": "0", "data-dep": "P13"}],
         BLOCKED, 0),
        ("only optional pending is terminal",
         [{"data-id": "P10", "data-done": "1"},
          {"data-id": "P13", "data-done": "0", "data-optional": "1"}], TERMINAL, 0),
        ("dep on unknown id fails",
         [{"data-id": "P11", "data-done": "0", "data-dep": "P99"}], None, 1),
        ("dependency cycle fails",
         [{"data-id": "P11", "data-done": "0", "data-dep": "P12"},
          {"data-id": "P12", "data-done": "0", "data-dep": "P11"}], None, 1),
        ("self dependency fails",
         [{"data-id": "P11", "data-done": "0", "data-dep": "P11"}], None, 1),
        ("optional and done fails",
         [{"data-id": "P13", "data-done": "1", "data-optional": "1"}], None, 1),
        ("missing data-done fails",
         [{"data-id": "P11"}], None, 1),
    ]


def selftest():
    failures = 0
    total = 0
    temp_dir = tempfile.mkdtemp(prefix="check-progress-")
    try:
        for name, rows, want_state, want_exit in selftest_cases():
            total += 1
            path = pathlib.Path(temp_dir) / ("case%d.html" % total)
            path.write_text(build_html(rows), encoding="utf-8")
            code, result, fails = check(path)
            got_state = result["state"] if result else None
            ok = (code == want_exit and got_state == want_state)
            print("%s %2d %s" % ("ok  " if ok else "FAIL", total, name))
            if not ok:
                failures += 1
                print("     exit: want %d got %d" % (want_exit, code))
                print("     state: want %s got %s" % (want_state, got_state))
                for line in fails:
                    print("       - " + line)
    finally:
        for leftover in pathlib.Path(temp_dir).glob("*.html"):
            leftover.unlink()
        pathlib.Path(temp_dir).rmdir()

    if failures:
        print("RESULT FAIL (%d/%d)" % (failures, total))
        return 1
    print("RESULT PASS: %d cases (eligible/blocked/terminal + 5 structural rules)" % total)
    return 0


def main(argv):
    parser = argparse.ArgumentParser(
        description="Decide whether /autodev has anything left to do.")
    parser.add_argument("--file", default=None, help="progress.html to read")
    parser.add_argument("--json", action="store_true", help="one-line JSON output")
    parser.add_argument("--selftest", action="store_true", help="run built-in cases")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()

    path = pathlib.Path(args.file) if args.file else DEFAULT_FILE
    path = path.resolve()
    code, result, fails = check(path)
    if code != 0:
        print("RESULT FAIL (%d)" % len(fails))
        for line in fails:
            print(" - " + line)
        return 1
    if args.json:
        print(json.dumps(result, sort_keys=True))
    else:
        print(render_text(result))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
