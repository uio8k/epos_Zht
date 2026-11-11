#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""L3/L4 整机回归（P10 + P11）：none 后端跑完整条音频栈，不碰宿主声卡。

方案与取证见 docs/tech-plan-full-none.md（2026-11-11 调研轮）。核心依据：
  [Gitee: audio/noaudio.c:120-130]  none 后端采集侧 no_read() 按虚拟时钟算出
                                    to_clear 字节并把缓冲清零后返回 —— 管线照跑、
                                    样本恒静音，故 L3 只断行为不断样本内容。
  [Gitee: audio/noaudio.c:43-61]   播放侧 no_run_out() 同样按虚拟时钟推进 rpos。
  [Gitee: audio/audio.c:1898]      3.1.x 用环境变量 QEMU_AUDIO_DRV 选后端。
  [实测: docs/evidence/2026-11-11/qemu-audio-help-p10.log]
                                   none=播放/采集都多路；wav=不支持采集；
                                   dsound=要碰宿主默认设备（违反 D9）。
  [实测: docs/evidence/2026-11-11/p10-probe-serial-visibility.log]
                                   串口文件在 QEMU 运行期间 ~2.5s 起可见 ——
                                   故边跑边断，不需要 watcher/monitor（D10）。

三条腿各一次构建，覆盖 docs/decisions-audio-test.md 给 L3 定的四项：
  L3-A PLAY     TEST_MODE=1 TEST_SIGNAL=sine  PCM 播放全链 + syscall 往返
  L3-B WAV      SORT_DEMO=1                   盘上 WAV 读取 + 多路混音
  L3-C CAPTURE  CAPTURE_TEST=1                采集全流程 + 用户态 API 参数校验

第四条腿是 P11 的 L4 内核自检（docs/tech-plan-selftest-l4.md）：
  L4 SELFTEST   默认构建                       冷启动打印的 SELFTEST 段全 PASS
                kernel/selftest.c 在 hda_init() 之后跑分配器往返、信号量握手、
                tick 前进、HDA 探测四项，输出走串口，故这里只断那几个固定行。

用法（入口 make full / make full-l3）：
  uv run --no-project python tests/full_run.py            # 四条腿全跑
  uv run --no-project python tests/full_run.py --leg WAV  # 只跑一条（排障）
  uv run --no-project python tests/full_run.py --leg SELFTEST
退出码：全 PASS=0，任一 FAIL=1。
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORKSPACE = os.path.dirname(REPO)
BUILD_DIR = os.path.join(REPO, "tests", "build")
HD_IMG = os.path.join(REPO, "hd.img")

# QEMU 3.1.50 固定在工作区根 Qemu/（D8：不动它、不换版本）；可用 QEMU_EXE 覆盖。
QEMU_EXE = os.environ.get("QEMU_EXE") or os.path.join(
    WORKSPACE, "Qemu", "qemu-system-i386w.exe")

POLL_S = 0.10          # 串口轮询间隔
LEG_TIMEOUT_S = 90.0   # 单腿上限（正常 5-8s，留足机器慢时的余量）


def log(msg):
    print(msg, flush=True)


# --------------------------------------------------------------------------
# 三条腿的断言表：每条 = (正则, 说明)。全部必须命中；缺一条即该腿 FAIL。
# 正则都取自实际串口原文（见 docs/evidence/2026-11-11/p10-probe-*.log），
# 不写「大概长这样」的宽松模式 —— 断错东西等于没断。
# --------------------------------------------------------------------------
LEGS = [
    {
        "id": "L3-A",
        "name": "PLAY",
        "build": ["TEST_MODE=1", "TEST_SIGNAL=sine"],
        "why": "PCM 播放全链：音量 syscall -> BDL 配置 -> 流启动 -> 停流 -> 状态 syscall",
        "checks": [
            (r"\[TEST\] volume set to 100",
             "音量 syscall 往返（audio_set_volume；TEST_MODE 满音量播，避免软件增益污染）"),
            (r"\[TEST\] sine 440Hz, 2000ms",
             "用户态生成参考信号并提交播放"),
            (r"HDA: BDL ok entries=128 cbl=524288",
             "BDL 配置成功（128 条 x 4KB）"),
            (r"HDA: continuous stream started \(CBL=524288, 44100Hz 2ch 16bit\)",
             "流启动（格式与内核下发的配置一致）"),
            (r"HDA: continuous stream stopped \(tail=\d+ ms, lpib_max_step=\d+, "
             r"wait=poll bcis=0, underruns=0\)",
             "停流干净：underruns=0（none 后端下播放侧照常被消费，不应饿死）"),
            (r"\[AUDIO\] errors: fifo=0 desc=0  position: \d+ ms",
             "状态 syscall 往返（audio_get_errors / audio_get_position_ms）"),
        ],
    },
    {
        "id": "L3-B",
        "name": "WAV",
        "build": ["SORT_DEMO=1"],
        "why": "真实盘上 WAV 读取（song.wav 不在 git 里，故用 tracked 的 s*.wav）+ 多路混音",
        "checks": [
            (r"\[AUDIO\] volume set to 80",
             "音量 syscall 往返"),
            (r"\[MELODY\] Shell sort driving melody, one note per swap",
             "进入音符驱动路径"),
            (r"\[MELODY\] #0 note=\d+ ms=\d+ voices=3 s\d+\.wav",
             "从磁盘读到 WAV 并完成 3 路混音后提交播放"),
        ],
    },
    {
        "id": "L3-C",
        "name": "CAPTURE",
        "build": ["CAPTURE_TEST=1"],
        "why": "采集全流程（1s + 3s 环绕）+ 用户态 API 幂等/参数校验",
        "checks": [
            (r"\[CAP-A\] VERDICT: len=OK path=OK signal=SILENT",
             "1s 采集：字节数达标、LPIB 在走；none 后端下信号必为 SILENT（预期值）"),
            (r"\[CAP-A\] bytes=\d+ expect=176400 diff=-?\d+ lpib=\d+ wraps=0 overruns=0",
             "1s 采集：无 overrun、无环绕（256KB 环装得下）"),
            (r"\[CAP-B\] bytes=\d+ expect=529200 diff=-?\d+ lpib=\d+ wraps=1 overruns=0",
             "3s 采集：529KB > 256KB 环 -> 缓冲升 512KB 且 LPIB 绕回 1 次"),
            (r"\[CAP-C\] first=0 same=0 diff=-2 stop=0 \(expect 0/0/-2/0\)",
             "用户态 API 契约：同参幂等、异参拒绝、停流可重入"),
            (r"HDA: continuous stream stopped \(tail=\d+ ms",
             "采集测试结束时播放流被显式停掉（cap_play_tone 起流，采集停流不代劳）"),
            (r"AUDIO_VERDICT v=2 result=SILENT .*silent=1",
             "串口判定行按 v=2 格式输出（P05b 后的新格式）"),
        ],
    },
    {
        "id": "L3-D",
        "name": "RESTART",
        "build": ["TEST_MODE=1", "TEST_SIGNAL=restart"],
        "why": "同一镜像内停流->重启两轮会话，验会话坐标（data_end/rdpos/wrpos）在起流时确实归零",
        "checks": [
            (r"\[TEST\] restart: 800ms session then 200ms session",
             "进入双会话路径（第一轮 800ms、第二轮 200ms，各自独立 stop）"),
            (r"\[RESTART\] session1=\d+ ms session2=\d+ ms",
             "两轮会话各自的 position 读数（stop 后 rdpos 即该会话总消费量）"),
            (r"\[RESTART\] VERDICT: PASS",
             "第二轮消费量小于第一轮：会话坐标未残留；残留时 s2 会反超 s1"),
        ],
    },
    {
        "id": "L4",
        "name": "SELFTEST",
        "build": [],
        "why": "内核整机自检：分配器往返 + 信号量握手 + tick 前进 + HDA 探测（kernel/selftest.c）",
        "checks": [
            (r"SELFTEST: begin \(4 cases\)",
             "自检段起跑（冷启动即执行，不依赖用户态）"),
            (r"SELFTEST \[1/4\] allocator: PASS",
             "分配器往返：kmalloc/kfree 后同尺寸再申请拿回同一地址"),
            (r"SELFTEST \[2/4\] semaphore: PASS",
             "信号量握手：内核线程 signal -> 主线程 wait -> task_wait 回收"),
            (r"SELFTEST \[3/4\] tick: PASS",
             "tick 前进：g_timer_ticks 在定时器中断下自增"),
            (r"SELFTEST \[4/4\] hda: PASS",
             "HDA 探测：MMIO/codec/DAC/Pin 四项事实齐全"),
            (r"SELFTEST: RESULT PASS 4/4",
             "四项全通过（任一项 FAIL 则该行变 FAIL 且数字不足）"),
        ],
    },
]


def qemu_binary():
    if os.path.isfile(QEMU_EXE):
        return QEMU_EXE
    found = shutil.which("qemu-system-i386w")
    if found:
        return found
    return None


def run_build(target_args):
    """make hd.img <VAR=1> ...；返回 (rc, 输出)。串行，不加 -j（见 tech-plan §6.1）。"""
    cmd = ["make", "hd.img"] + list(target_args)
    p = subprocess.run(cmd, cwd=REPO, capture_output=True)
    out = (p.stdout or b"").decode("utf-8", "replace") + \
          (p.stderr or b"").decode("utf-8", "replace")
    return p.returncode, out


def kill_stale():
    """清掉残留 QEMU（上一轮超时/被中断留下的），避免抢 serial 与 CPU。"""
    exe = qemu_binary()
    if not exe:
        return
    name = os.path.basename(exe)
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq " + name, "/FO", "CSV"],
                             capture_output=True, timeout=20).stdout.decode("utf-8", "replace")
    except Exception:
        return
    if name.lower() not in out.lower():
        return
    log("  [warn] 发现残留 %s，先结束" % name)
    subprocess.run(["taskkill", "/F", "/IM", name], capture_output=True, timeout=20)
    time.sleep(1.0)


def run_leg(leg):
    """构建 -> 起 QEMU(none 后端) -> 轮询串口直到断言全中/超时 -> 结束 QEMU。"""
    exe = qemu_binary()
    if not exe:
        log("!! 找不到 qemu-system-i386w（期望 %s）" % QEMU_EXE)
        return False

    os.makedirs(BUILD_DIR, exist_ok=True)
    # L3 腿沿用 l3_*.log（与 p10 证据同一命名），L4 腿落 l4_selftest.log
    serial = os.path.join(BUILD_DIR, "%s_%s.log" % (leg["id"].split("-")[0].lower(),
                                                    leg["name"].lower()))
    if os.path.exists(serial):
        os.remove(serial)

    log("")
    log("== %s %s ==" % (leg["id"], leg["name"]))
    log("   覆盖：%s" % leg["why"])
    log("   构建：make hd.img %s" % " ".join(leg["build"]))
    t0 = time.time()
    rc, out = run_build(leg["build"])
    build_s = time.time() - t0
    if rc != 0:
        log("   [FAIL] 构建失败 rc=%d（%.1fs）" % (rc, build_s))
        for line in out.strip().splitlines()[-15:]:
            log("          | " + line)
        return False
    log("   构建 OK（%.1fs）" % build_s)

    kill_stale()

    env = dict(os.environ)
    env["QEMU_AUDIO_DRV"] = "none"     # D8/D9：唯一可行的后端（tech-plan §3.4）

    cmd = [exe, "-m", "48", "-boot", "order=c", "-vga", "std",
           "-drive", "format=raw,file=%s,index=0,media=disk" % HD_IMG.replace("\\", "/"),
           "-device", "intel-hda", "-device", "hda-duplex",
           "-serial", "file:%s" % serial.replace("\\", "/"),
           "-display", "none"]
    log("   运行：QEMU_AUDIO_DRV=none %s ...（无 monitor / 无 watcher）" % os.path.basename(exe))

    proc = subprocess.Popen(cmd, cwd=REPO, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    t_run = time.time()
    text = ""
    try:
        deadline = t_run + LEG_TIMEOUT_S
        while time.time() < deadline:
            if proc.poll() is not None:
                break
            try:
                with open(serial, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                text = ""
            if all(re.search(pat, text) for pat, _ in leg["checks"]):
                break
            time.sleep(POLL_S)
    finally:
        if proc.poll() is None:
            proc.kill()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
    run_s = time.time() - t_run

    # 结束后再读一次：QEMU 退出时会 flush 串口，尾部几行可能刚落地
    try:
        with open(serial, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        pass

    ok = True
    for pat, desc in leg["checks"]:
        hit = re.search(pat, text) is not None
        log("   [%s] %s" % ("PASS" if hit else "FAIL", desc))
        if not hit:
            ok = False
            log("          期望匹配：%s" % pat)

    log("   用时：构建 %.1fs + 运行 %.1fs；串口 %d 字节 -> %s"
        % (build_s, run_s, len(text), os.path.relpath(serial, REPO)))

    if not ok:
        tail = text.strip().splitlines()[-20:]
        log("   --- 串口尾部 ---")
        for line in tail:
            log("   | " + line)
    return ok


def main():
    ap = argparse.ArgumentParser(description="epos_Zht L3 整机回归（none 后端）")
    ap.add_argument("--leg", default=None,
                    help="只跑一条腿：PLAY / WAV / CAPTURE（默认全跑）")
    args = ap.parse_args()

    legs = LEGS
    if args.leg:
        want = args.leg.strip().upper()
        legs = [lg for lg in LEGS if lg["name"] == want or lg["id"].upper() == want]
        if not legs:
            log("!! 未知腿 %r；可选：%s" % (args.leg, ", ".join(l["name"] for l in LEGS)))
            return 1

    if shutil.which("make") is None:
        log("!! PATH 里没有 make —— 先 call ..\\setvars.bat /ni（入口应走 make full）")
        return 1

    log("== L3 整机回归：QEMU 3.1.50 + none 后端 ==")
    log("   仓库：%s" % REPO)
    log("   后端：QEMU_AUDIO_DRV=none（不碰宿主声卡，D9）")

    t0 = time.time()
    results = []
    for leg in legs:
        results.append((leg, run_leg(leg)))
    total = time.time() - t0

    log("")
    log("== 汇总：%d 腿，%d 通过，%d 失败，用时 %.1fs =="
        % (len(results), sum(1 for _, ok in results if ok),
           sum(1 for _, ok in results if not ok), total))
    for leg, ok in results:
        log("   %-6s %-8s %s" % (leg["id"], leg["name"], "PASS" if ok else "FAIL"))
    return 0 if all(ok for _, ok in results) else 1


if __name__ == "__main__":
    sys.exit(main())
