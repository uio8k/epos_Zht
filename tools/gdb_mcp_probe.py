#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gdb_mcp_probe.py - 通过 stdio 驱动 gdb-mcp 服务器（冒烟测试 / 脚本化调试）

用法（在项目根目录执行）:
  uv run --no-project tools/gdb_mcp_probe.py [--exe PATH] [--binary PATH]
      [--cwd DIR] [--cmd "GDB 命令"]... [--timeout 秒]

不带 --cmd 时执行内置的 EPOS/QEMU 默认序列: 加载 eposkrnl.out 符号,
连接 QEMU gdbstub (127.0.0.1:1234), 断在 hda_init 并读取变量。
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEFAULT_CMDS = [
    "target remote 127.0.0.1:1234",
    "info registers eip",
    "info address hda_init",
    "break hda_init",
    "continue",
    "bt",
    "info registers eip",
    "print/x g_hda.mmio_base",
]


def default_exe():
    p = os.path.join(os.path.expanduser("~"), ".local", "bin", "gdb-mcp.exe")
    return p if os.path.exists(p) else "gdb-mcp"


class McpClient:
    def __init__(self, exe):
        self.stderr_path = os.path.join(tempfile.gettempdir(), "gdb_mcp_probe_stderr.log")
        self.stderr_file = open(self.stderr_path, "wb")
        self.proc = subprocess.Popen([exe], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=self.stderr_file)
        self._id = 0

    def _send(self, obj):
        self.proc.stdin.write((json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8"))
        self.proc.stdin.flush()

    def request(self, method, params=None, timeout=60.0):
        self._id += 1
        rid = self._id
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        self._send(msg)
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("server closed stdout, see %s" % self.stderr_path)
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line.decode("utf-8", "replace"))
            except json.JSONDecodeError:
                print("  [raw] %s" % line.decode("utf-8", "replace"))
                continue
            if obj.get("id") == rid:
                if "error" in obj:
                    raise RuntimeError("MCP error: %s" % obj["error"])
                return obj.get("result", {})
            if "method" in obj:
                print("  [notify] %s" % obj["method"])
        raise TimeoutError("no response to %s within %ss" % (method, timeout))

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self._send(msg)

    def tool(self, name, arguments, timeout=60.0):
        return self.request("tools/call", {"name": name, "arguments": arguments}, timeout)

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()
        self.stderr_file.close()


def tool_text(result):
    parts = []
    for item in result.get("content", []):
        if item.get("type") == "text":
            parts.append(item.get("text", ""))
    return "\n".join(parts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=default_exe())
    ap.add_argument("--binary", default=os.path.join(ROOT, "epos", "kernel", "eposkrnl.out"))
    ap.add_argument("--cwd", default=os.path.join(ROOT, "epos"))
    ap.add_argument("--cmd", action="append", default=None)
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()

    cmds = args.cmd or DEFAULT_CMDS

    cli = McpClient(args.exe)
    ok = False
    try:
        r = cli.request("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "gdb-mcp-probe", "version": "0.1"},
        })
        print("server: %s" % r.get("serverInfo"))
        cli.notify("notifications/initialized")

        tools = cli.request("tools/list").get("tools", [])
        print("tools (%d): %s" % (len(tools), ", ".join(t.get("name", "?") for t in tools)))

        print("\n== start_binary %s ==" % args.binary)
        text = tool_text(cli.tool("start_binary", {
            "binary_path": args.binary, "cwd": args.cwd, "load_init": False,
        }, timeout=args.timeout))
        print(text)
        m = (re.search(r'"session_id"\s*:\s*"([^"]+)"', text)
             or re.search(r"session[_ ]?id[\"']?\s*[:=]\s*[\"']?([0-9a-zA-Z._-]+)", text))
        if not m:
            raise RuntimeError("cannot find session_id in start_binary result")
        session = m.group(1)
        print("session_id = %s" % session)

        for cmd in cmds:
            print("\n== gdb: %s ==" % cmd)
            text = tool_text(cli.tool("gdb_command", {
                "session_id": session, "command": cmd,
                "timeout_seconds": args.timeout,
            }, timeout=args.timeout + 20))
            print(text)

        print("\n== stop_session ==")
        print(tool_text(cli.tool("stop_session", {"session_id": session}, timeout=30)))
        ok = True
    finally:
        cli.close()
        print("\nprobe %s. server stderr: %s" % ("OK" if ok else "FAILED", cli.stderr_path))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
