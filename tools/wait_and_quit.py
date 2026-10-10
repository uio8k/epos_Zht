#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""wait_and_quit.py - 等 serial.log 出现完成标记，再经 telnet monitor 发送 quit
用法: python wait_and_quit.py <serial_log> [monitor_port] [timeout_s] [marker]
"""
import sys, time, socket, os

serial_log = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 4444
timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0
marker = sys.argv[4] if len(sys.argv) > 4 else "Demo finished. Check serial.log"

start = time.time()
while time.time() - start < timeout:
    try:
        with open(serial_log, 'rb') as f:
            data = f.read()
        if marker.encode() in data:
            print(f"[ok] marker found after {time.time()-start:.1f}s")
            break
    except FileNotFoundError:
        pass
    time.sleep(0.5)
else:
    print("[warn] timeout waiting for marker; sending quit anyway")

time.sleep(1.0)  # let last audio flush

# 连接 telnet monitor 并发送 quit
try:
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    # 等待 monitor 提示符
    time.sleep(1.0)
    s.sendall(b"quit\n")
    time.sleep(1.0)
    s.close()
    print("[ok] quit sent to monitor")
except Exception as e:
    print(f"[err] monitor quit failed: {e}")

# 等待 QEMU 进程退出（wav 头 finalize）
deadline = time.time() + 10
while time.time() < deadline:
    if not any(p.lower().startswith("qemu-system") for p in
               os.popen("tasklist /FI \"IMAGENAME eq qemu-system-i386w.exe\" /FO CSV")):
        print("[ok] QEMU exited")
        break
    time.sleep(0.5)
