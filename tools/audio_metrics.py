#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
audio_metrics.py - 音频质量客观指标分析（供 eval_audio.py 使用）

在 analyze_wav.py 的基础上扩展为可评分的指标体系：
  - 频率准确度 / THD / THD+N / SNR（时域基频拟合-剥离法，避免窗泄漏伪影）
  - 频谱平坦度 / 峰值因子
  - 多方法 click 检测：局部斜率比 + 短时频谱平坦度 + 短时峰值因子
  - 冲激响应 / 阶跃响应分析
  - 静音区 / 振幅包络（钢琴模式）
  - 源 WAV 对比：样本级 diff + log-spectral distance + 互相关
  - xrun（缓冲欠载）检测

参考：
  Analog Devices MT-003 (SINAD/SNR/THD/THD+N 定义)
  Audio Precision TN-104 (六项基本音频测量)
  DSP-Artifact-Detection (click/pop 检测算法与阈值)
  仅依赖 numpy。

注：THD/SNR 不用「FFT 取峰 + 邻带求和」，因为 Hann 窗积分旁瓣泄漏约 -31.5dB，
会使纯数字正弦也测出 ~1.8% 的假 THD+N（实测验证）。改用 3 参数最小二乘拟合
基频并从时域残差中顺序剥离各次谐波，可将本底压到 int16 量化极限（~-90dB）。
"""
import wave
import numpy as np

EPS = 1e-12


# ============================ 基础 I/O ============================

def read_wav(path):
    """读取 WAV，返回 (samples float64 [N, ch], rate, ch, sampwidth)。"""
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
    return a.reshape(-1, ch), rate, ch, sw


def _mono(samples):
    """下混为单声道 float64。"""
    s = np.asarray(samples, dtype=np.float64)
    return s if s.ndim == 1 else s.mean(axis=1)


def active_span(mono, threshold=300.0):
    """返回 (start_idx, end_idx) —— |mono|>threshold 的首末样本；全静音返回 None。"""
    nz = np.nonzero(np.abs(mono) > threshold)[0]
    if len(nz) == 0:
        return None
    return int(nz[0]), int(nz[-1]) + 1


def _next_pow2(n):
    p = 1
    while p < n:
        p <<= 1
    return p


def _moving_mean(x, win):
    """滑动均值（cumsum 实现，O(N)）。"""
    win = max(int(win), 1)
    half = win // 2
    n = len(x)
    cs = np.concatenate(([0.0], np.cumsum(x)))
    idx = np.arange(n)
    lo = np.clip(idx - half, 0, n)
    hi = np.clip(idx + half + 1, 0, n)
    return (cs[hi] - cs[lo]) / np.maximum(hi - lo, 1)


# ============================ 正弦拟合（THD/SNR 核心） ============================

def _fit_sin(x, rate, f):
    """最小二乘拟合 x ≈ a·sin(2πft) + b·cos(2πft)；返回 (fit, power)。

    2x2 正规方程解析解，O(N)。x 应已去直流。
    """
    n = len(x)
    t = np.arange(n) / rate
    w = 2.0 * np.pi * f * t
    s, c = np.sin(w), np.cos(w)
    A = float(np.dot(s, s)); B = float(np.dot(s, c)); C = float(np.dot(c, c))
    D = float(np.dot(s, x)); E = float(np.dot(c, x))
    det = A * C - B * B
    if abs(det) < EPS:
        return np.zeros(n), 0.0
    a = (C * D - B * E) / det
    b = (A * E - B * D) / det
    fit = a * s + b * c
    return fit, float(np.sum(fit * fit))


def _refine_f0(x, rate, f0, span_hz=0.5, iters=6):
    """在 f0 附近细化频率：以单频拟合残差最小为目标做逐步收缩扫描。

    精确的 f0 是基频剥离的前提：δf 的频率偏差会在残差里留下约
    (π·δf·T)²/3 的相对能量——δf=2e-4Hz、T=2s 时约 -63dB，足以成为
    整条测量链的本底。6 轮 21 点扫描可把分辨率压到 ~1e-7 Hz。
    """
    lo, hi = f0 - span_hz, f0 + span_hz
    best = f0
    for _ in range(iters):
        cands = np.linspace(lo, hi, 21)
        best_r, best_f = None, best
        for f in cands:
            fit, _ = _fit_sin(x, rate, f)
            r = float(np.sum((x - fit) ** 2))
            if best_r is None or r < best_r:
                best_r, best_f = r, float(f)
        best = best_f
        step = (hi - lo) / 20.0
        lo, hi = best - step, best + step
    return best


# ============================ FFT / 频谱指标 ============================

def fft_analysis(samples, rate, freq_expected=None, threshold=300.0, max_harmonic=10):
    """测试音（正弦）分析：频率 / THD / THD+N / SNR / 平坦度 / 峰值因子。

    仅分析有效音频段（去首尾静音）。频率由 Hann 窗 FFT 峰值初估后用正弦
    拟合细化；THD/SNR 由时域顺序剥离基频与各次谐波得到（见模块 docstring）。
    """
    mono = _mono(samples)
    span = active_span(mono, threshold)
    seg = mono[span[0]:span[1]] if span else mono
    n = len(seg)
    if n < 256:
        return {'error': 'signal too short', 'samples': int(n)}

    dc = float(np.mean(seg))
    x = seg - dc

    # 频率初估：Hann 窗 FFT 峰值 + 抛物线插值
    nfft = _next_pow2(n)
    power = np.abs(np.fft.rfft(x * np.hanning(n), nfft)) ** 2
    freqs = np.fft.rfftfreq(nfft, 1.0 / rate)
    k0 = int(np.argmax(power[1:])) + 1
    if 0 < k0 < len(power) - 1:
        y0, y1, y2 = power[k0 - 1], power[k0], power[k0 + 1]
        denom = y0 - 2 * y1 + y2
        delta = 0.5 * (y0 - y2) / denom if abs(denom) > EPS else 0.0
    else:
        delta = 0.0
    f_coarse = (k0 + delta) * (rate / nfft)

    # 频率细化 + 基频剥离
    f0 = _refine_f0(x, rate, f_coarse)
    fund_fit, fund_power = _fit_sin(x, rate, f0)
    resid = x - fund_fit

    # 顺序剥离 2..max_harmonic 次谐波
    harm_power = 0.0
    harmonics = []
    for k in range(2, max_harmonic + 1):
        fk = k * f0
        if fk >= rate / 2.0:
            break
        fitk, pk = _fit_sin(resid, rate, fk)
        if pk <= 0:
            continue
        harm_power += pk
        if fund_power > 0:
            harmonics.append({'harmonic': k, 'freq_hz': float(fk),
                              'pct_of_fund': float(np.sqrt(pk / fund_power) * 100.0)})
        resid = resid - fitk

    noise_power = float(np.sum(resid * resid))
    # 残差直流不计入失真/噪声：直流偏置是独立的量（dc_offset 单独报告）。
    # 实测捕获可能带 ~10 LSB 的直流偏置（连续流写位置对齐时丢掉起始少量
    # 样本所致），若不去除会把纯音误判成 THD+N 0.048% / SNR 66dB。
    resid = resid - float(resid.mean())
    noise_power = float(np.sum(resid * resid))

    thd = np.sqrt(harm_power / fund_power) * 100.0 if fund_power > 0 else 0.0
    thd_n = np.sqrt((harm_power + noise_power) / fund_power) * 100.0 if fund_power > 0 else 0.0
    snr_db = 10.0 * np.log10(fund_power / noise_power) if noise_power > 0 else 120.0

    # 频谱平坦度（几何均值/算术均值，纯音 -> ~0）
    p = power[1:] + EPS
    spectral_flatness = float(np.exp(np.mean(np.log(p))) / np.mean(p))

    # 时域指标
    peak = float(np.max(np.abs(seg)))
    rms = float(np.sqrt(np.mean(x ** 2)))
    crest_db = 20.0 * np.log10(peak / rms) if rms > 0 else 0.0
    rms_noise = float(np.sqrt(noise_power / n)) if n else 0.0
    dyn_range_db = 20.0 * np.log10(peak / rms_noise) if rms_noise > 0 else 120.0

    result = {
        'samples': int(n),
        'active_ms': float(n / rate * 1000.0),
        'fundamental_hz': float(f0),
        'thd_pct': float(thd),
        'thd_n_pct': float(thd_n),
        'snr_db': float(snr_db),
        'spectral_flatness': spectral_flatness,
        'dynamic_range_db': float(dyn_range_db),
        'dc_offset': dc,
        'peak_amplitude': peak,
        'rms': rms,
        'crest_factor_db': float(crest_db),
        'clipping_count': int(np.sum(np.abs(seg) >= 32767)),
        'harmonics': harmonics,
    }
    if freq_expected is not None:
        result['freq_expected_hz'] = float(freq_expected)
        result['freq_error_hz'] = float(abs(f0 - freq_expected))
    return result


# ============================ Click / Pop 检测 ============================

def _merge_indices(idx, max_gap=5):
    """合并相邻检测索引（gap<=max_gap），返回代表索引列表。"""
    if len(idx) == 0:
        return []
    merged = [int(idx[0])]
    for i in idx[1:]:
        if i - merged[-1] <= max_gap:
            continue
        merged.append(int(i))
    return merged


def _detect_derivative(mono, rate, ratio_threshold=4.0, window_ms=5.0,
                       abs_floor_frac=0.01):
    """方法一：一阶差分 + 局部均值比（对纯音固有斜率与静音不误报）。

    ratio[n] = |d[n]| / max(moving_mean(|d|), abs_floor)，其中
    abs_floor = abs_floor_frac × 峰值斜率。两个必要条件：
      (1) |d| 远大于局部典型斜率 —— 排除正弦自身的固有斜率（比值上界 ~1.6）；
      (2) |d| 绝对量级足够大 —— 排除静音段的微跳变（局部均值≈0 会让比值爆炸）。
    """
    d = np.abs(np.diff(mono))
    if len(d) < 4:
        return [], 0.0, 0.0
    rms = float(np.sqrt(np.mean(mono ** 2)))
    abs_floor = max(abs_floor_frac * rms, 16.0)
    local = np.maximum(_moving_mean(d, int(rate * window_ms / 1000.0)), abs_floor)
    ratio = d / (local + EPS)
    idx = np.nonzero((ratio > ratio_threshold) & (d > abs_floor))[0]
    return _merge_indices(idx), float(ratio_threshold), float(np.max(d))


def _frames(mono, frame, hop):
    """分帧，返回 (索引矩阵, 帧数)。"""
    nf = (len(mono) - frame) // hop + 1
    if nf <= 0:
        return None, 0
    base = np.arange(0, nf * hop, hop)[:, None] + np.arange(frame)[None, :]
    return base, nf


def _detect_spectral_flatness(mono, rate, frame=512, hop=256, thr=0.8, min_rms=300.0):
    """方法二：短时频谱平坦度（click 为宽带 -> 平坦度高）。

    必须有能量门限：静音帧各 bin 都接近 0，几何/算术均值之比趋近 1，
    会把纯静音误判为「宽带事件」。
    """
    idx, nf = _frames(mono, frame, hop)
    if nf == 0:
        return []
    frames = mono[idx]
    fr = np.sqrt(np.mean(frames ** 2, axis=1))
    p = np.abs(np.fft.rfft(frames * np.hanning(frame), axis=1)) ** 2 + EPS
    flat = np.exp(np.mean(np.log(p), axis=1)) / np.mean(p, axis=1)
    hits = [i for i in np.nonzero(flat > thr)[0] if fr[i] > min_rms]
    return [{"frame": int(i), "time_ms": float(i * hop / rate * 1000.0)} for i in hits]


def _detect_crest(mono, rate, frame=512, hop=256, thr_db=18.0, min_rms=300.0):
    """方法三：短时峰值因子（click 为脉冲 -> 峰值因子高）。"""
    idx, nf = _frames(mono, frame, hop)
    if nf == 0:
        return []
    frames = mono[idx]
    peak = np.max(np.abs(frames), axis=1)
    rms = np.sqrt(np.mean(frames ** 2, axis=1))
    crest = np.full(nf, -np.inf)
    ok = rms > min_rms
    crest[ok] = 20.0 * np.log10(peak[ok] / rms[ok])
    hits = [i for i in np.nonzero(crest > thr_db)[0] if ok[i]]
    return [{"frame": int(i), "time_ms": float(i * hop / rate * 1000.0)} for i in hits]


def click_detection(samples, rate, sensitivity=4.0,
                    flatness_threshold=0.8, crest_threshold_db=18.0):
    """多方法 click/pop 检测，三种方法按样本位置投票。

    返回 dict：click_count / high_confidence_count / low_confidence_count /
    clicks / max_jump / mean_jump / method_counts。
    高置信度 = 至少两种方法在相近位置同时命中（降低单方法误报）。
    """
    mono = _mono(samples)
    idx_d, thr_d, max_jump = _detect_derivative(mono, rate, ratio_threshold=sensitivity)
    hits_f = _detect_spectral_flatness(mono, rate, thr=flatness_threshold)
    hits_c = _detect_crest(mono, rate, thr_db=crest_threshold_db)

    n = max(len(mono), 1)
    votes = np.zeros(n, dtype=np.int8)
    for i in idx_d:
        votes[min(i + 1, n - 1)] |= 1
    for h in hits_f:
        s = int(h["time_ms"] / 1000.0 * rate)
        if 0 <= s < n:
            votes[s] |= 2
    for h in hits_c:
        s = int(h["time_ms"] / 1000.0 * rate)
        if 0 <= s < n:
            votes[s] |= 4

    clicks = []
    for i in np.nonzero(votes)[0]:
        clicks.append({
            'sample_idx': int(i),
            'time_ms': float(i / rate * 1000.0),
            'amplitude': float(mono[i]),
            'methods': [bool(votes[i] & 1), bool(votes[i] & 2), bool(votes[i] & 4)],
        })
    high = [c for c in clicks if sum(c['methods']) >= 2]

    d = np.abs(np.diff(mono)) if len(mono) > 1 else np.array([0.0])
    return {
        'click_count': len(clicks),
        'high_confidence_count': len(high),
        'low_confidence_count': len(clicks) - len(high),
        'clicks': clicks[:50],
        'max_jump': max_jump,
        'mean_jump': float(np.mean(d)),
        'derivative_ratio_threshold': thr_d,
        'method_counts': {
            'derivative': len(idx_d),
            'spectral_flatness': len(hits_f),
            'crest': len(hits_c),
        },
    }


# ============================ 冲激 / 阶跃响应 ============================

def impulse_response_analysis(samples, rate, settle_ms=50.0):
    """冲激响应：频率响应平坦度 + 冲激稳定后的异常 click 检测。

    冲激本身就是人为的样本级阶跃，因此不能把冲激本身当成 click；只统计
    冲激后 settle_ms 之外的不连续点（缓冲边界 / DMA 伪迹才会出现在那里）。
    """
    mono = _mono(samples)
    if len(mono) == 0:
        return {'error': 'empty'}

    k = int(np.argmax(np.abs(mono)))
    peak = float(mono[k])
    t0_ms = k / rate * 1000.0
    tail = np.abs(mono[k:])
    if tail.max() > 0:
        thr = tail.max() * 1e-3
        over = np.nonzero(tail > thr)[0]
        decay_samples = int(over[-1]) if len(over) else 0
    else:
        decay_samples = 0

    seg = mono[k:k + min(rate, len(mono) - k)]
    flat = 0.0
    if len(seg) > 64:
        p = (np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2)[1:] + EPS
        flat = float(np.exp(np.mean(np.log(p))) / np.mean(p))

    cd = click_detection(samples, rate)
    post = [c for c in cd['clicks'] if c['time_ms'] > t0_ms + settle_ms]
    post_high = [c for c in post if sum(c['methods']) >= 2]
    return {
        'impulse_peak': peak,
        'impulse_time_ms': t0_ms,
        'ring_decay_ms': float(decay_samples / rate * 1000.0),
        'post_impulse_flatness': flat,
        'post_impulse_clicks': len(post),
        'post_impulse_high_clicks': len(post_high),
        'clicks': post[:20],
    }


def step_response_analysis(samples, rate, threshold=300.0, ramp_ms=5.0):
    """阶跃响应：过冲 / 平台时长 / 阶跃边沿 click。

    参考信号在边沿带 ramp_ms 线性斜坡，故边沿本身不含阶跃；若在边沿附近
    检出高置信 click，即为驱动引入的伪迹。
    """
    mono = _mono(samples)
    span = active_span(mono, threshold)
    if span is None:
        return {'error': 'no active audio'}
    seg = mono[span[0]:span[1]]

    plateau = float(np.median(seg))
    if abs(plateau) < EPS:
        return {'error': 'no step (plateau ~ 0)'}

    overshoot = max(0.0, (float(np.max(np.abs(seg))) / abs(plateau) - 1.0) * 100.0)
    cd = click_detection(samples, rate)
    edge_ms = [
        span[0] / rate * 1000.0,
        span[0] / rate * 1000.0 + ramp_ms,
        span[1] / rate * 1000.0 - ramp_ms,
        span[1] / rate * 1000.0,
    ]
    edge_clicks = [c for c in cd['clicks']
                   if any(abs(c['time_ms'] - b) < 20.0 for b in edge_ms)]
    edge_high = [c for c in edge_clicks if sum(c['methods']) >= 2]

    return {
        'plateau': plateau,
        'overshoot_pct': float(overshoot),
        'plateau_duration_ms': float(len(seg) / rate * 1000.0),
        'edge_clicks': len(edge_clicks),
        'edge_high_clicks': len(edge_high),
        'clicks_at_edges': len(edge_clicks),
        'clicks': edge_clicks[:20],
    }


# ============================ 钢琴模式分析 ============================

def silence_region_analysis(samples, rate, threshold=300.0, min_gap_ms=8.0):
    """静音区分析：首尾静音 + 内部 gap 数量/时长/位置。"""
    mono = _mono(samples)
    act = np.abs(mono) > threshold
    n = len(mono)
    dur_ms = n / rate * 1000.0

    if not np.any(act):
        return {'lead_silence_ms': dur_ms, 'tail_silence_ms': 0.0,
                'gap_count': 0, 'gaps': [], 'gap_durations_ms': [],
                'total_duration_ms': dur_ms}

    nz = np.nonzero(act)[0]
    first, last = int(nz[0]), int(nz[-1])

    gaps = []
    min_gap = int(rate * min_gap_ms / 1000.0)
    i = first
    while i <= last:
        if not act[i]:
            j = i
            while j <= last and not act[j]:
                j += 1
            if j - i >= min_gap:
                gaps.append({'start_ms': float(i / rate * 1000.0),
                             'duration_ms': float((j - i) / rate * 1000.0)})
            i = j
        else:
            i += 1

    return {
        'lead_silence_ms': float(first / rate * 1000.0),
        'tail_silence_ms': float((n - last) / rate * 1000.0),
        'active_start_ms': float(first / rate * 1000.0),
        'active_end_ms': float(last / rate * 1000.0),
        'total_duration_ms': dur_ms,
        'gap_count': len(gaps),
        'gaps': gaps[:100],
        'gap_durations_ms': [g['duration_ms'] for g in gaps],
    }


def amplitude_envelope(samples, rate, window_ms=50.0, threshold=300.0):
    """滑动窗口 RMS 包络 -> 音量一致性。"""
    mono = _mono(samples)
    w = max(int(rate * window_ms / 1000.0), 1)
    nf = len(mono) // w
    if nf == 0:
        return {'mean_rms': 0.0, 'std_rms': 0.0, 'consistency_pct': 0.0, 'envelope': []}
    env = np.sqrt(np.mean(mono[:nf * w].reshape(nf, w) ** 2, axis=1))
    active = env[env > threshold]
    if len(active) == 0:
        return {'mean_rms': 0.0, 'std_rms': 0.0, 'consistency_pct': 100.0, 'envelope': []}
    mean = float(np.mean(active))
    std = float(np.std(active))
    return {
        'mean_rms': mean,
        'std_rms': std,
        'consistency_pct': float(max(0.0, (1.0 - std / mean) * 100.0)) if mean > 0 else 0.0,
        'window_ms': window_ms,
        'envelope': [float(x) for x in env],
    }


def stereo_balance(samples):
    """立体声平衡：L/R RMS 差。"""
    s = np.asarray(samples, dtype=np.float64)
    if s.ndim == 1 or s.shape[1] < 2:
        return {'channels': 1, 'balance_db': 0.0, 'channel_match': True}
    l = float(np.sqrt(np.mean(s[:, 0] ** 2)))
    r = float(np.sqrt(np.mean(s[:, 1] ** 2)))
    bal = 20.0 * np.log10(l / r) if (l > 0 and r > 0) else 0.0
    return {
        'channels': int(s.shape[1]),
        'left_rms': l,
        'right_rms': r,
        'balance_db': float(bal),
        'channel_match': bool(abs(bal) < 0.5),
    }


# ============================ xrun（缓冲欠载）检测 ============================

def xrun_detection(samples, rate, threshold=20.0, min_silence_ms=30.0):
    """检测播放中途的非预期静音段（xrun/underrun 的音频特征）。

    连续流模式驱动会预填零，正常音符间隙很短；明显长于预期的内部静音段
    可疑为缓冲欠载。
    """
    mono = _mono(samples)
    span = active_span(mono, threshold)
    if span is None:
        return {'xrun_count': 0, 'suspected_xruns': []}

    start, end = span
    min_len = int(rate * min_silence_ms / 1000.0)
    quiet = np.abs(mono[start:end]) <= threshold

    suspected = []
    i = 0
    while i < len(quiet):
        if quiet[i]:
            j = i
            while j < len(quiet) and quiet[j]:
                j += 1
            if j - i >= min_len:
                suspected.append({'time_ms': float((start + i) / rate * 1000.0),
                                  'duration_ms': float((j - i) / rate * 1000.0)})
            i = j
        else:
            i += 1

    return {'xrun_count': len(suspected), 'suspected_xruns': suspected[:50]}


# ============================ 音符序列（边界伪迹）分析 ============================

def sequence_burst_analysis(samples, rate, burst_ms=200, expected_bursts=8,
                            threshold=300.0, edge_ms=10.0):
    """音符序列测试：定位各短音段，检查段数/时长与**段边界 click**。

    序列由 8 次独立的 audio_play_pcm_ms 调用产生（与音符播放共用连续流路径，
    每次调用重新对齐 wrpos=rdpos）。参考信号由用户态生成且每段自带 5ms 淡入
    淡出，因此若在段边界检测到高置信 click，即可判定为驱动连续流写位置对齐 /
    淡出逻辑引入的伪迹 —— 这是钢琴模式无法可靠给出的可评分指标。
    """
    mono = _mono(samples)
    n = len(mono)
    # 用短时包络判定有声段：直接对 |mono| 取阈值会被正弦的过零点切成碎片
    # （440Hz 每 ~50 样本就掉到阈值以下一次）。
    env = _moving_mean(np.abs(mono), max(int(rate * 0.003), 3))
    act = env > threshold

    bursts = []
    i = 0
    while i < n:
        if act[i]:
            j = i
            while j < n and act[j]:
                j += 1
            bursts.append((i, j))
            i = j
        else:
            i += 1
    min_len = int(rate * 0.010)                      # 忽略 <10ms 的碎片
    bursts = [b for b in bursts if b[1] - b[0] >= min_len]

    cd = click_detection(samples, rate)
    high = [c for c in cd['clicks'] if sum(c['methods']) >= 2]

    boundaries_ms = []
    for s, e in bursts:
        boundaries_ms.append(s / rate * 1000.0)
        boundaries_ms.append(e / rate * 1000.0)
    boundary_clicks = [c for c in high
                       if any(abs(c['time_ms'] - b) < edge_ms for b in boundaries_ms)]

    rms = [float(np.sqrt(np.mean(mono[s:e] ** 2))) for s, e in bursts]
    gaps = [float((bursts[k + 1][0] - bursts[k][1]) / rate * 1000.0)
            for k in range(len(bursts) - 1)]

    return {
        'burst_count': len(bursts),
        'expected_bursts': expected_bursts,
        'burst_durations_ms': [float((e - s) / rate * 1000.0) for s, e in bursts],
        'burst_rms': rms,
        'burst_rms_consistency_pct': (float(max(0.0, (1.0 - np.std(rms) / np.mean(rms)) * 100.0))
                                      if rms and np.mean(rms) > 0 else 0.0),
        'inter_burst_gaps_ms': gaps,
        'boundary_clicks': len(boundary_clicks),
        'boundary_click_list': boundary_clicks[:20],
        'total_clicks': cd['click_count'],
        'high_confidence_clicks': cd['high_confidence_count'],
        'max_jump': cd['max_jump'],
    }


# ============================ 源 WAV 对比 ============================

def align_and_compare(captured_mono, rate, source_mono,
                      approx_start_ms=0.0, max_offset_ms=120.0):
    """把源信号对齐到捕获信号并比较（测量 DMA 链引入的退化）。

    粗到细两级搜索：先在 approx_start ± max_offset_ms 内以 2ms 步长用归一化
    互相关定位，再在粗定位附近以 1 样本步长细化。最后计算样本级 diff 与频域距离。
    """
    cap = np.asarray(captured_mono, dtype=np.float64)
    src = np.asarray(source_mono, dtype=np.float64)
    if len(src) == 0 or len(cap) < len(src):
        return {'error': 'source longer than capture'}

    src_c = src - src.mean()
    src_s = float(np.std(src_c))

    def _ncc(off):
        seg = cap[off:off + len(src)]
        ss = float(np.std(seg))
        if ss * src_s < EPS:
            return -2.0
        return float(np.mean((seg - seg.mean()) * src_c) / (ss * src_s))

    base = int(approx_start_ms / 1000.0 * rate)
    win = int(max_offset_ms / 1000.0 * rate)
    lo = max(0, base - win)
    hi = min(len(cap) - len(src), base + win)
    if hi < lo:
        hi = lo = max(0, min(base, len(cap) - len(src)))

    # 粗搜（2ms）
    coarse = max(1, rate // 500)
    best_off, best_corr = lo, -2.0
    for off in range(lo, hi + 1, coarse):
        c = _ncc(off)
        if c > best_corr:
            best_corr, best_off = c, off

    # 细搜（1 样本）±coarse
    fine_lo = max(lo, best_off - coarse)
    fine_hi = min(hi, best_off + coarse)
    for off in range(fine_lo, fine_hi + 1):
        c = _ncc(off)
        if c > best_corr:
            best_corr, best_off = c, off

    seg = cap[best_off:best_off + len(src)]
    diff = seg - src
    nfft = _next_pow2(max(len(src), 1024))
    w = np.hanning(len(src))
    ps = (np.abs(np.fft.rfft(src * w, nfft)) ** 2) + EPS
    pc = (np.abs(np.fft.rfft(seg * w, nfft)) ** 2) + EPS
    lsd = float(np.mean(np.abs(10.0 * np.log10(pc) - 10.0 * np.log10(ps))))
    conv = float(np.linalg.norm(pc - ps) / (np.linalg.norm(ps) + EPS))

    return {
        'alignment_offset_ms': float(best_off / rate * 1000.0),
        'cross_correlation': float(best_corr),
        'max_sample_diff': float(np.max(np.abs(diff))),
        'rms_diff': float(np.sqrt(np.mean(diff ** 2))),
        'log_spectral_distance_db': lsd,
        'spectral_convergence': conv,
    }
