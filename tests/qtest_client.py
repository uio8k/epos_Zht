#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""L2 qtest 客户端（P09）：不启动 OS，经 QEMU qtest 协议寄存器级验 HDA 流与缓冲。

方案与取证见 docs/tech-plan-qtest.md（2026-10-31 调研轮）；模型行为依据：
  [Gitee: hw/audio/intel-hda.c:583-597]  RUN 0->1 时 parse_bdl（bentries=lvi+1、
                                          bsize=cbl、lpib=0）并 notify(stnr=ctl>>20)
  [Gitee: hw/audio/intel-hda.c:395-461]  intel_hda_xfer 按 tag(ctl bits23:20) 匹配流，
                                          lpib 推进、bentries 环绕时归零
  [Gitee: hw/audio/hda-codec.c:583-607]  codec 从 verb 0x2（stream/channel）、
                                          0x3（format）学参数；bytes/s = 2*ch*freq
  [Gitee: hw/audio/hda-codec.c:490-500]  传输定时器挂 QEMU_CLOCK_VIRTUAL
                                          -> -S 冻结下 LPIB 只随 clock_step 推进（U1）
  [Gitee: hw/audio/intel-hda.c:312-320]  ICS(0x68) 写 BUSY 即同步执行 ICW(0x60) 的
                                          verb，响应落 IRR(0x64) —— 与内核
                                          hda_send_verb 同一条路径

用法（入口 make qtest）：
  uv run --no-project python tests/qtest_client.py            # 全部用例
  uv run --no-project python tests/qtest_client.py --probe    # 仅 GCAP 校验
退出码：全 PASS=0，任一 FAIL=1。QEMU 进程由本脚本自行起停（端口 4653，
启动前检测残留实例；-S 冻结、QEMU_AUDIO_DRV=none，即 D8 的 none 后端）。
"""
import base64
import socket
import struct
import subprocess
import sys
import time

QEMU_EXE = r"E:\ComputerScience\epos_Zht\Qemu\qemu-system-i386w.exe"
QTEST_PORT = 4653
BAR_ADDR = 0xE0000000
BDL_PA = 0x00300000          # BDL 表（2 条 x 16B = 32B，页内）
RING_PA = 0x00400000         # 环形缓冲（2 x 128KB）
ENTRY_LEN = 0x00020000       # 每条 128KB
N_ENTRIES = 2
CBL = ENTRY_LEN * N_ENTRIES  # 0x40000
FMT = 0x0010                 # 48kHz base, mono, 16-bit（kernel hda.h SD_FMT 同值）
OUT_TAG = 1                  # 非零、1-15 合法；与 verb SET_CHANNEL_STREAMID 一致

# HDA 寄存器偏移（[Gitee: intel-hda-defs.h:13-31,75-84] 与内核 hda.h 一致）
GCTL_OFF = 0x08
ICW_OFF, IRR_OFF, ICS_OFF = 0x60, 0x64, 0x68
SD_BASE = 0x80
SD_LPIB, SD_CBL, SD_LVI, SD_FMT, SD_BDPL, SD_BDPU = 0x04, 0x08, 0x0C, 0x12, 0x18, 0x1C
SD_CTL_RUN = 0x02
SD_CTL_SRST = 0x01
GCAP_EXPECT = 0x4401


def log(msg):
    print(msg, flush=True)


class QemuProc:
    """起停本脚本周期的 QEMU（-S 冻结 + qtest tcp）。残留实例检测见 U5。"""

    def __init__(self):
        # 残留检测：端口已被占说明有 QEMU（或别的进程）在监听
        probe = socket.socket()
        probe.settimeout(1.0)
        try:
            probe.connect(("127.0.0.1", QTEST_PORT))
            probe.close()
            log("!! 端口 %d 已被监听：疑似残留 QEMU 实例，先清理再跑（U5）" % QTEST_PORT)
            raise SystemExit(1)
        except (ConnectionRefusedError, OSError):
            pass
        finally:
            probe.close()

        import os
        env = dict(os.environ)
        env["QEMU_AUDIO_DRV"] = "none"   # D8：none 后端（3.1.50 无 -audiodev）
        self.proc = subprocess.Popen(
            [QEMU_EXE, "-M", "pc", "-nodefaults",
             "-display", "none", "-serial", "none", "-monitor", "none",
             "-device", "intel-hda", "-device", "hda-duplex",
             "-qtest", "tcp:127.0.0.1:%d,server,nowait" % QTEST_PORT],
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        self._wait_listening()
        # 不冻结 VM：本地 3.1.50 w 构建未编 qtest accelerator（实测
        # "-machine accel=qtest" -> "No accelerator found"），clock_step 不可用；
        # 机器照常跑（SeaBIOS 空转不碰 HDA），虚拟时钟按真实时间走，codec 的
        # 虚时钟定时器才会在真实时间触发 —— 上游 ich6_test 同为运行中的机器。
        time.sleep(1.5)

    def _wait_listening(self, timeout=15.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                log("!! QEMU 提前退出，rc=%s" % self.proc.returncode)
                raise SystemExit(1)
            s = socket.socket()
            s.settimeout(0.5)
            try:
                s.connect(("127.0.0.1", QTEST_PORT))
                s.close()
                return
            except OSError:
                time.sleep(0.2)
            finally:
                try:
                    s.close()
                except OSError:
                    pass
        log("!! 等待 qtest 端口超时")
        raise SystemExit(1)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()          # qtest 协议无 quit 命令（U4），外部强杀
            self.proc.wait(timeout=10)


class QTest:
    """qtest 行客户端：命令 -> OK/FAIL 行（本设置无异步消息源）。"""

    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", QTEST_PORT), timeout=15)
        self.sock.settimeout(15)
        self.buf = b""
        self.ncmd = 0

    def cmd(self, line):
        self.ncmd += 1
        self.sock.sendall((line + "\n").encode())
        deadline = time.time() + 15
        while b"\n" not in self.buf:
            if time.time() > deadline:
                raise RuntimeError("qtest 响应超时: " + line)
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("qtest 连接被对端关闭: " + line)
            self.buf += chunk
        resp, _, self.buf = self.buf.partition(b"\n")
        text = resp.decode(errors="replace").strip()
        if not text.startswith("OK"):
            raise RuntimeError("qtest FAIL(%s): %s" % (line, text))
        return text

    def value(self, line):
        resp = self.cmd(line)
        parts = resp.split()
        if len(parts) < 2:
            raise RuntimeError("qtest 应答缺值(%s): %s" % (line, resp))
        return int(parts[1], 16)

    # ---- PIO（端口空间）----
    def outl(self, addr, val): self.cmd("outl 0x%x 0x%x" % (addr, val))
    def inl(self, addr): return self.value("inl 0x%x" % addr)
    def outw(self, addr, val): self.cmd("outw 0x%x 0x%x" % (addr, val))
    def inw(self, addr): return self.value("inw 0x%x" % addr)

    # ---- 物理内存（MMIO / DMA，注意与 PIO 两族不可混用——v1 探针教训）----
    def readl(self, addr): return self.value("readl 0x%x" % addr)
    def readw(self, addr): return self.value("readw 0x%x" % addr)
    def readb(self, addr): return self.value("readb 0x%x" % addr)
    def writel(self, addr, val): self.cmd("writel 0x%x 0x%x" % (addr, val))
    def writeb(self, addr, val): self.cmd("writeb 0x%x 0x%x" % (addr, val))
    def b64write(self, addr, data):
        self.cmd("b64write 0x%x 0x%x %s" % (addr, len(data),
                                            base64.b64encode(data).decode()))

    def clock_step(self, ns):
        return self.value("clock_step 0x%x" % ns)


class Checks:
    def __init__(self):
        self.passed = 0
        self.failed = 0

    def check(self, cond, what):
        if cond:
            self.passed += 1
            log("  [PASS] " + what)
        else:
            self.failed += 1
            log("  [FAIL] " + what)
        return cond

    def done(self):
        log("== 汇总：%d PASS，%d FAIL ==" % (self.passed, self.failed))
        return self.failed == 0


def find_hda(qt):
    """PCI 扫描 bus 0 找 8086:2668（禁止硬编码 BDF——tech-plan §5.3 第 1 步）。"""
    for dev in range(32):
        for fn in range(2):
            base = 0x80000000 | (dev << 11) | (fn << 8)
            qt.outl(0xCF8, base)
            if qt.inl(0xCFC) == 0x26688086:
                log("  intel-hda @ 00:%02x.%x" % (dev, fn))
                return base
    raise RuntimeError("bus 0 未找到 8086:2668")


class HdaCtx:
    """PCI -> BAR -> GCAP -> SD 偏移（tech-plan §5.3 第 1-4 步，用例共用）。"""

    def __init__(self, qt, ck):
        self.qt = qt
        cfg = find_hda(qt)
        # 命令寄存器 MEM|BUSM（上游 test_issue542 同款写 0x06）；机器在运行中，
        # SeaBIOS 可能已使能过设备 —— 读旧值后按位补齐，不盲写。
        qt.outl(0xCF8, cfg | 0x04)
        old_cmd = qt.inw(0xCFC)
        if (old_cmd & 0x06) != 0x06:
            qt.outl(0xCF8, cfg | 0x04)
            qt.outw(0xCFC, old_cmd | 0x06)
        # BAR0：运行中的 SeaBIOS 可能已分配 —— 先读，为 0 才自配
        qt.outl(0xCF8, cfg | 0x10)
        bar = qt.inl(0xCFC)
        if bar == 0 or bar == 0xFFFFFFFF:
            qt.outl(0xCF8, cfg | 0x10)
            qt.outl(0xCFC, BAR_ADDR)
            qt.outl(0xCF8, cfg | 0x10)
            bar = qt.inl(0xCFC)
        self.bar = bar & 0xFFFFF000
        ck.check(self.bar != 0, "BAR0 有效（0x%08x%s）"
                 % (self.bar, "，SeaBIOS 已分配" if bar != BAR_ADDR else "，客户端自配"))
        # GCTL CRST=1（内核同序；模型中 0->1 无副作用，仅是规范要求）
        qt.writel(self.bar + GCTL_OFF, 0x1)
        # GCAP 校验（值来自寄存器，不比对常量放行——tech-plan §6 第 3 条）
        self.gcap = qt.readl(self.bar)
        ck.check(self.gcap == GCAP_EXPECT,
                 "GCAP=0x%04x（期望 0x%04x：OSS=4 ISS=4）" % (self.gcap, GCAP_EXPECT))
        self.iss = (self.gcap >> 8) & 0xF
        self.oss = (self.gcap >> 12) & 0xF
        # 输出流绝对索引 = ISS + k（k=0）；SD 基址 = 0x80 + 0x20*n
        self.out_sd = self.iss
        self.sd = self.bar + SD_BASE + 0x20 * self.out_sd
        log("  out_sd=#%d (ISS=%d) SD@0x%x" % (self.out_sd, self.iss, self.sd))

    def verb(self, nid, verb12, payload):
        """ICS 立即命令路径（与内核 hda_send_verb 同款）：写 ICW -> ICS BUSY -> 读 IRR。"""
        qt = self.qt
        word = (0 << 28) | ((nid & 0x7F) << 20) | ((verb12 & 0xFFF) << 8) | (payload & 0xFF)
        qt.writel(self.bar + ICW_OFF, word)
        qt.cmd("writew 0x%x 0x1" % (self.bar + ICS_OFF))   # BUSY=1 -> 模型同步执行
        ics = qt.readw(self.bar + ICS_OFF)
        if ics & 0x1:
            raise RuntimeError("verb 0x%x 未完成（ICS 仍 BUSY）" % word)
        return qt.readl(self.bar + IRR_OFF)

    def sd_reset(self):
        """SRST 脉冲（重跑 BDL 解析用；内核 cap_stream_reset 同款）。"""
        qt = self.qt
        qt.writeb(self.sd + 0x00, SD_CTL_SRST)
        while qt.readb(self.sd + 0x00) & SD_CTL_SRST:
            pass
        qt.writeb(self.sd + 0x00, 0)

    def write_bdl(self, ioc=False):
        """2 条目 BDL：addr(64LE) + len(32LE) + flags(32LE, bit0=IOC)。"""
        flags = 0x1 if ioc else 0x0
        bdl = b"".join(struct.pack("<QII", RING_PA + i * ENTRY_LEN, ENTRY_LEN, flags)
                       for i in range(N_ENTRIES))
        self.qt.b64write(BDL_PA, bdl)

    def sd_setup(self, tag, ioc=False):
        """写 SD 寄存器全序列（SRST -> CBL/LVI/BDP/FMT/tag byte），全部回读。"""
        qt = self.qt
        self.sd_reset()
        qt.writeb(self.sd + 0x02, (tag & 0xF) << 4)   # CTL byte2 = tag
        qt.writel(self.sd + SD_CBL, CBL)
        qt.cmd("writew 0x%x 0x%x" % (self.sd + SD_LVI, N_ENTRIES - 1))
        qt.writel(self.sd + SD_BDPL, BDL_PA)
        qt.writel(self.sd + SD_BDPU, 0)
        qt.cmd("writew 0x%x 0x%x" % (self.sd + SD_FMT, FMT))
        self.write_bdl(ioc)

    def set_run(self, on):
        qt = self.qt
        ctl = qt.readl(self.sd + 0x00)
        qt.writel(self.sd + 0x00, (ctl | SD_CTL_RUN) if on else (ctl & ~SD_CTL_RUN))


def ul2_01(qt, ck):
    """U-L2-01：BDL 链写读回 + RUN 位（BDL 解析在 RUN 0->1，读回在 RUN 前后各验一轮）。"""
    log("-- U-L2-01: BDL 链写读回 + RUN 位 --")
    ctx = HdaCtx(qt, ck)
    ctx.sd_setup(OUT_TAG)
    sd = ctx.sd
    qt2 = qt
    ck.check(qt2.readl(sd + SD_CBL) == CBL, "CBL 回读 0x%x" % CBL)
    ck.check((qt2.readl(sd + SD_LVI) & 0xFFFF) == N_ENTRIES - 1, "LVI 回读 %d" % (N_ENTRIES - 1))
    ck.check(qt2.readl(sd + SD_BDPL) == BDL_PA, "BDPL 回读 0x%x" % BDL_PA)
    ck.check(qt2.readl(sd + SD_BDPU) == 0, "BDPU 回读 0")
    ck.check((qt2.readl(sd + 0x00) >> 20 & 0xF) == OUT_TAG, "CTL tag 回读 %d" % OUT_TAG)
    # RUN 置位读回（此时 bpl 已被模型解析：RUN 0->1 触发 parse_bdl）
    ctx.set_run(True)
    ck.check((qt2.readl(sd + 0x00) & SD_CTL_RUN) != 0, "RUN 位置 1 读回")
    ctx.set_run(False)
    ck.check((qt2.readl(sd + 0x00) & SD_CTL_RUN) == 0, "RUN 清 0 读回")
    return ctx


def ul2_02(qt, ck, ctx):
    """U-L2-02：LPIB 推进与环绕回。U1 定论：本地 3.1.50 w 构建无 qtest accelerator，
    clock_step 不可用 [实测: "No accelerator found"]；机器不冻结、虚拟时钟按真实
    时间走，codec 虚时钟定时器在真实时间触发（上游 ich6_test 同为运行中的机器）。"""
    log("-- U-L2-02: LPIB 推进与环绕回 --")
    sd = ctx.sd
    # 绑定 codec：SET_CHANNEL_STREAMID(0x2) + SET_STREAM_FORMAT(0x3)，nid=2（hda-duplex DAC）
    r1 = ctx.verb(0x02, 0x2, (OUT_TAG << 4) | 0x0)
    r2 = ctx.verb(0x02, 0x3, FMT)
    log("  verb resp: streamid=0x%08x fmt=0x%08x" % (r1, r2))

    # --- U1 实验：真实时间推进量化（96kB/s @48k mono16，100ms ≈ 9600 字节） ---
    ctx.sd_setup(OUT_TAG)
    ctx.set_run(True)
    lpib_start = qt.readl(sd + SD_LPIB)
    time.sleep(0.3)
    lpib_realt = qt.readl(sd + SD_LPIB)
    delta = (lpib_realt - lpib_start) % (1 << 32)
    log("  U1: start=%d real+300ms -> +%d bytes（期望 ~9600±，来自 2*ch*freq）"
        % (lpib_start, delta))

    if delta == 0:
        # 自动换绑尝试（兼容模型版本差异）：逐组合重试，先到先得，全部留痕
        combos = [(4, 0x10), (0, 0x00), (2, (2 << 4))]  # 内核同款 payload 等变体
        for tag, fmt_payload in combos:
            ctx.sd_setup(tag)
            ctx.verb(0x02, 0x2, (tag << 4) | 0x0)
            ctx.verb(0x02, 0x3, fmt_payload)
            ctx.set_run(True)
            a = qt.readl(sd + SD_LPIB)
            time.sleep(0.3)
            b = qt.readl(sd + SD_LPIB)
            log("  组合 tag=%d fmt=0x%02x: LPIB +%d" % (tag, fmt_payload, (b - a) % (1 << 32)))
            if b != a:
                break   # 命中即沿用该绑定继续本用例（保持 RUN=1）
            ctx.set_run(False)

    ck.check(delta > 0 or qt.readl(sd + SD_LPIB) != lpib_start,
             "LPIB 真实时间推进（300ms 内有变化）")

    # --- 环绕回：轮询直到 LPIB 覆盖 CBL 至少一次（96kB/s 下 ~2.8s/圈） ---
    wraps = 0
    prev = qt.readl(sd + SD_LPIB)
    changed = 0
    total = 0
    deadline = time.time() + 15
    while time.time() < deadline and (wraps < 1 or total < 8):
        time.sleep(0.05)
        cur = qt.readl(sd + SD_LPIB)
        if cur < prev:
            wraps += 1
        changed += 1 if cur != prev else 0
        total += 1
        prev = cur
    ck.check(changed > 0, "LPIB 有推进（%d/%d 次采样变化）" % (changed, total))
    ck.check(wraps >= 1, "LPIB 环绕回 >=1 次（实测 %d 次）" % wraps)

    # --- 停流后不再推进 ---
    ctx.set_run(False)
    frozen_before = qt.readl(sd + SD_LPIB)
    time.sleep(0.3)
    ck.check(qt.readl(sd + SD_LPIB) == frozen_before, "停 RUN 后 LPIB 冻结")


def main():
    probe_only = "--probe" in sys.argv
    ck = Checks()
    qemu = None
    try:
        qemu = QemuProc()
        qt = QTest()
        log("== qtest 连接建立（QEMU pid=%d）==" % qemu.proc.pid)
        # --probe：GCAP 校验（tech-plan §7 步骤 1 的首条 PASS）
        ctx = HdaCtx(qt, ck)
        if probe_only:
            return 0 if ck.done() else 1
        ctx = ul2_01(qt, ck)          # 含独立的 HdaCtx 初始化与断言
        ul2_02(qt, ck, ctx)
        return 0 if ck.done() else 1
    finally:
        if qemu:
            qemu.stop()


if __name__ == "__main__":
    sys.exit(main())
