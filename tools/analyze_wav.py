#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
analyze_wav.py - 分析 QEMU wav 后端捕获 vs 源文件的首尾杂音
用法: python analyze_wav.py <file1> [file2 ...]
"""
import sys, wave
import numpy as np

def read_wav(path):
    with wave.open(path, 'rb') as w:
        ch = w.getnchannels()
        sw = w.getsampwidth()
        rate = w.getframerate()
        n = w.getnframes()
        raw = w.readframes(n)
    if sw == 2:
        a = np.frombuffer(raw, dtype='<i2').astype(np.float64)
    elif sw == 1:
        a = np.frombuffer(raw, dtype='<u1').astype(np.float64) - 128.0
    elif sw == 4:
        a = np.frombuffer(raw, dtype='<i4').astype(np.float64) / 65536.0
    else:
        raise ValueError(f'unsupported sampwidth {sw}')
    a = a.reshape(-1, ch)
    return a, rate, ch, sw

def stats(name, a, rate, label, ms):
    n = int(rate * ms / 1000)
    if len(a) < n:
        n = len(a)
    seg = a[:n] if label == 'HEAD' else a[-n:]
    mono = seg.mean(axis=1)
    dc = float(seg.mean())
    rms = float(np.sqrt((mono ** 2).mean()))
    peak = float(np.abs(mono).max())
    # 采样间最大跳变（click 检测）
    d = np.abs(np.diff(mono))
    maxjump = float(d.max()) if len(d) else 0.0
    # 过零率
    zc = float(np.mean(np.diff(np.sign(mono)) != 0)) if len(mono) > 1 else 0.0
    print(f'  {label} {ms}ms: peak={peak:8.1f} rms={rms:8.1f} dc={dc:8.2f} '
          f'maxSampleJump={maxjump:8.1f} zerox={zc:5.3f}')

def find_silence_edges(a, rate, thr=300.0):
    mono = np.abs(a.mean(axis=1))
    act = mono > thr
    nz = np.nonzero(act)[0]
    if len(nz) == 0:
        return None
    first = nz[0] / rate * 1000
    last = nz[-1] / rate * 1000
    dur = len(a) / rate * 1000
    return first, last, dur

for path in sys.argv[1:]:
    a, rate, ch, sw = read_wav(path)
    dur_s = len(a) / rate
    print(f'\n=== {path} ===')
    print(f'  rate={rate} ch={ch} sampwidth={sw} len={len(a)} samples '
          f'({dur_s:.3f}s = {dur_s*1000:.1f}ms)')
    # 首尾 50ms 详情 + 更细的头部 5ms
    stats(path, a, rate, 'HEAD', 5)
    stats(path, a, rate, 'HEAD', 50)
    stats(path, a, rate, 'TAIL', 50)
    stats(path, a, rate, 'TAIL', 200)
    # 有效音频起止
    se = find_silence_edges(a, rate)
    if se:
        first, last, dur = se
        print(f'  active audio: {first:.2f}ms .. {last:.2f}ms '
              f'(lead_silence={first:.2f}ms tail_silence={dur-last:.2f}ms)')
    # 头部 100 个有效采样的波形（放大看淡入）
    mono = a.mean(axis=1)
    act = np.abs(mono) > 300
    nz = np.nonzero(act)[0]
    if len(nz):
        s0 = max(0, nz[0] - 20)
        s1 = min(len(mono), nz[0] + 80)
        print(f'  head window [{s0}:{s1}] first-active={nz[0]} ({(nz[0]/rate*1000):.2f}ms):')
        vals = [f'{mono[i]:.0f}' for i in range(s0, s1, 3)]
        print('    ' + ' '.join(vals[:40]))
