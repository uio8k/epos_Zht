#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
eval_audio.py - HDA 音频驱动质量评价体系主编排器

流程：build -> run(headless, 捕获 wav + serial.log) -> analyze -> score -> report

用法：
  uv run --no-project --with numpy python tools/eval_audio.py [options]

  --mode {test-tone,piano,both}   评价模式（默认 both）
  --full                          test-tone 额外跑 impulse / step / sequence
  --sensitivity {conservative,moderate,aggressive}   artifact 检测灵敏度
  --output PATH                   报告 JSON 输出（默认 tools/eval_report.json）
  --baseline PATH                 基线 JSON（默认 tools/baseline_report.json）
  --update-baseline               把本次结果写入基线文件
  --no-run                        跳过 build/run，仅分析已有 wav + serial.log
  --verbose                       打印子进程输出

设计要点（为什么这么评分）：
  * 绝对评分只取「测试音 + serial.log」——测试音参考信号已知，测得的值经合成
    参考对照验证可达 int16 量化极限（SNR ~97dB），因此绝对阈值是可信的。
  * 钢琴模式的绝对指标（click/间隙/音量一致性/立体声失衡）受**音乐内容**支配：
    音符起音是合法瞬态、休止符是合法静音、样本本身有立体声宽度。这些指标
    绝对阈值无意义，仅作**参考信息**展示，并通过「回归」维度参与评分（相对
    变化才有意义）。
  * 音符边界伪迹由专门的 `sequence` 测试信号（8 段已知短音、独立调用播放）
    给出可评分指标。
"""
import argparse
import json
import os
import subprocess
import sys
from datetime import datetime

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio_metrics as am          # noqa: E402
import serial_parser as sp          # noqa: E402

# Windows 控制台默认代码页（如 cp936）与 UTF-8 混用会导致中文乱码；显式改用 UTF-8。
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERIAL_LOG = os.path.join(ROOT, 'epos', 'userapp', 'serial.log')
SAMPLES_DIR = os.path.join(ROOT, 'epos', 'samples')
DEFAULT_OUTPUT = os.path.join(ROOT, 'tools', 'eval_report.json')
DEFAULT_BASELINE = os.path.join(ROOT, 'tools', 'baseline_report.json')

TEST_FREQ = 440.0
FULL_SIGNALS = ['impulse', 'step', 'sequence']

# artifact 检测灵敏度档位（差分比倍数 / 频谱平坦度 / 峰值因子 dB）
SENSITIVITY = {
    'conservative': {'derivative_ratio': 3.0, 'flatness': 0.7, 'crest': 15.0},
    'moderate':     {'derivative_ratio': 4.0, 'flatness': 0.8, 'crest': 18.0},
    'aggressive':   {'derivative_ratio': 8.0, 'flatness': 0.9, 'crest': 22.0},
}

# 优化建议映射：指标名 -> (可能根因, 优化方向, 代码位置)
ADVICE = {
    'thd_n_pct': ('Q8 软增益截断 / int16 量化', '考虑四舍五入或抖动(dither)', 'hda_audio.c: g_audio_gain_q8'),
    'thd_pct': ('谐波失真', '检查增益/混音路径的整数运算', 'hda_audio.c'),
    'snr_db': ('DMA 缓冲残余数据 / 噪声底偏高', '检查 ring buffer memset 与 DMA 对齐', 'hda_stream.c'),
    'dynamic_range_db': ('噪声底偏高', '检查 DMA 缓冲初始化', 'hda_stream.c'),
    'dc_offset': ('直流偏置', '检查混音钳位与增益', 'hda_audio.c'),
    'clipping_count': ('电平过高/混音溢出', '降低幅度或加限幅', 'hda_audio.c: 混音钳位'),
    'freq_error_hz': ('测试音生成精度 / 相位累加', '核对 440Hz 样本数与整数周期', 'main.c: run_test_signal'),
    'clicks_high': ('样本级不连续（写位置/淡出/边界）', '检查 g_cont_wrpos 对齐与 HDA_FADE_MS', 'hda_stream.c / hda_audio.c'),
    'clicks_total': ('疑似不连续点。若位于音频起始处，多为连续流写位置对齐时'
                     'DMA 读取头已前移，丢弃了音符起始的少量样本',
                     '检查 wrpos=rdpos 的对齐时机；可预填起始静音或让 DMA 回退',
                     'hda_stream.c: hda_cont_play_note'),
    'boundary_clicks': ('音符边界 click（写位置重对齐/淡出）', '检查 wrpos=rdpos 对齐与 fade 斜率', 'hda_stream.c: hda_cont_play_note'),
    'post_impulse_clicks': ('冲激稳定后的不连续（缓冲边界/DMA）', '检查 BDL 环绕与 fill_ahead', 'hda_stream.c'),
    'edge_clicks': ('阶跃边沿 click', '增益跳变需平滑', 'hda_audio.c'),
    'burst_rms_consistency_pct': ('各音符电平不一致', '核对混音权重与样本电平', 'hda_audio.c: 混音'),
    'ring_decay_ms': ('冲激后振铃过长', '检查缓冲边界处理', 'hda_stream.c'),
    'post_impulse_flatness': ('频率响应不平坦', '检查采样格式/率设置', 'hda_stream.c'),
    'overshoot_pct': ('阶跃过冲', '增益变化需平滑', 'hda_audio.c'),
    'xrun_count': ('缓冲不足 / CPU 抢占导致欠载', '增大 HDA_CONT_BUF_SIZE', 'hda_stream.c'),
    'fifo_errors': ('采样格式不匹配 / DMA 太慢', '核对 SDnFMT 与样本格式', 'hda_stream.c'),
    'desc_errors': ('BDL 条目非法（地址/长度/对齐）', '检查 BDL 64KB 拆分与物理地址', 'hda_stream.c'),
    'verb_timeouts': ('IC verb 通信异常', '检查 RINTCNT / CORB-RIRB 使能', 'hda.c'),
    'stream_format_ok': ('流格式不符预期', '核对 44100Hz/2ch/16bit', 'hda_stream.c'),
    'init_ok': ('HDA 初始化异常', '检查 PCI/BAR/控制器复位', 'hda.c'),
    'clean_start_stop': ('流未正常启停', '检查 RUN 位与完成判定', 'hda_stream.c'),
}


# ============================ 运行 ============================

def run_headless(signal, out_wav, timeout=900, verbose=False):
    """调用 tools/run_headless.bat 构建并运行（TEST_MODE 经环境变量传入）。"""
    env = dict(os.environ)
    if signal:
        env['TEST_MODE'] = '1'
        env['TEST_SIGNAL'] = signal
        marker = 'Test tone finished. Check serial.log'
    else:
        env.pop('TEST_MODE', None)
        env.pop('TEST_SIGNAL', None)
        marker = 'Demo finished. Check serial.log'

    bat = os.path.join(ROOT, 'tools', 'run_headless.bat')
    cmd = ['cmd', '/c', bat, 'wav', out_wav, marker]
    print(f'  [run] signal={signal or "piano"}  ->  {os.path.basename(out_wav)}')
    p = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True,
                       text=True, errors='replace', timeout=timeout)
    if verbose or p.returncode != 0:
        print(p.stdout[-4000:] if p.stdout else '')
        if p.stderr:
            print(p.stderr[-2000:])
    if p.returncode != 0:
        raise RuntimeError(f'run_headless.bat 失败 (exit {p.returncode})')
    if not os.path.exists(out_wav):
        raise RuntimeError(f'未生成捕获文件: {out_wav}')
    return out_wav


# ============================ 分析 ============================

def _click_kwargs(sens):
    return {'sensitivity': sens['derivative_ratio'],
            'flatness_threshold': sens['flatness'],
            'crest_threshold_db': sens['crest']}


def analyze_test_tone(wav_path, sig, sens):
    """测试音分析（sine / impulse / step / sequence）。返回 (metrics, raw)。"""
    a, rate, ch, sw = am.read_wav(wav_path)
    metrics, raw = {}, {}

    if sig == 'sine':
        f = am.fft_analysis(a, rate, TEST_FREQ)
        raw['fft'] = f
        metrics.update({
            'thd_pct': f.get('thd_pct'), 'thd_n_pct': f.get('thd_n_pct'),
            'snr_db': f.get('snr_db'), 'freq_error_hz': f.get('freq_error_hz'),
            'fundamental_hz': f.get('fundamental_hz'),
            'spectral_flatness': f.get('spectral_flatness'),
            'dynamic_range_db': f.get('dynamic_range_db'),
            'dc_offset': f.get('dc_offset'), 'peak_amplitude': f.get('peak_amplitude'),
            'crest_factor_db': f.get('crest_factor_db'),
            'clipping_count': f.get('clipping_count'),
        })
        cd = am.click_detection(a, rate, **_click_kwargs(sens))
        raw['clicks'] = cd
        metrics.update({'clicks_total': cd['click_count'],
                        'clicks_high': cd['high_confidence_count'],
                        'max_jump': cd['max_jump']})
        metrics['xrun_count'] = am.xrun_detection(a, rate)['xrun_count']

    elif sig == 'impulse':
        imp = am.impulse_response_analysis(a, rate)
        raw['impulse'] = imp
        metrics.update({'ring_decay_ms': imp.get('ring_decay_ms'),
                        'post_impulse_flatness': imp.get('post_impulse_flatness'),
                        'post_impulse_clicks': imp.get('post_impulse_clicks'),
                        'post_impulse_high_clicks': imp.get('post_impulse_high_clicks')})

    elif sig == 'step':
        stp = am.step_response_analysis(a, rate)
        raw['step'] = stp
        metrics.update({'overshoot_pct': stp.get('overshoot_pct'),
                        'edge_clicks': stp.get('edge_clicks'),
                        'edge_high_clicks': stp.get('edge_high_clicks')})

    elif sig == 'sequence':
        sq = am.sequence_burst_analysis(a, rate)
        raw['sequence'] = sq
        metrics.update({'boundary_clicks': sq['boundary_clicks'],
                        'burst_count': sq['burst_count'],
                        'expected_bursts': sq['expected_bursts'],
                        'burst_rms_consistency_pct': sq['burst_rms_consistency_pct']})

    st = am.stereo_balance(a)
    raw['stereo'] = st
    metrics['stereo_balance_db'] = abs(st.get('balance_db', 0.0))
    return metrics, raw


def analyze_piano(wav_path, sens, max_source_notes=8):
    """钢琴模式分析（绝对指标仅作参考，见模块 docstring）。返回 (metrics, raw)。"""
    a, rate, ch, sw = am.read_wav(wav_path)
    mono = am._mono(a)
    raw, metrics = {}, {}

    cd = am.click_detection(a, rate, **_click_kwargs(sens))
    raw['clicks'] = cd
    metrics.update({'clicks_total': cd['click_count'],
                    'clicks_high': cd['high_confidence_count'],
                    'max_jump': cd['max_jump']})

    sil = am.silence_region_analysis(a, rate)
    raw['silence'] = sil
    gaps = sil.get('gap_durations_ms', [])
    metrics['gap_count'] = sil.get('gap_count', 0)
    metrics['gap_mean_ms'] = float(np.mean(gaps)) if gaps else 0.0
    metrics['gap_std_ms'] = float(np.std(gaps)) if gaps else 0.0

    env = am.amplitude_envelope(a, rate)
    raw['envelope'] = {k: env[k] for k in ('mean_rms', 'std_rms', 'consistency_pct',
                                           'window_ms')}
    metrics['amplitude_consistency_pct'] = env.get('consistency_pct')

    st = am.stereo_balance(a)
    raw['stereo'] = st
    metrics['stereo_balance_db'] = abs(st.get('balance_db', 0.0))

    metrics['xrun_count'] = am.xrun_detection(a, rate)['xrun_count']

    logp = sp.parse_serial_log(SERIAL_LOG)
    src_cmp = _compare_sources(mono, rate, logp['melody']['note_list'],
                               max_notes=max_source_notes)
    raw['source_compare'] = src_cmp
    ok = [c for c in src_cmp if 'cross_correlation' in c and c['cross_correlation'] > 0.9]
    if ok:
        metrics['source_lsd_db'] = float(np.mean([c['log_spectral_distance_db'] for c in ok]))
        metrics['source_cross_correlation'] = float(np.mean([c['cross_correlation'] for c in ok]))
        metrics['source_notes_matched'] = len(ok)
    return metrics, raw


def _compare_sources(cap_mono, rate, notes, max_notes=8):
    """对前若干个单声部音符做源样本对齐比较，顺序推进时间轴以跟踪漂移。"""
    out = []
    t = 0.0
    for note in notes:
        if len(out) >= max_notes:
            break
        if note.get('voices', 1) != 1:
            t += note['ms']
            continue
        path = os.path.join(SAMPLES_DIR, note['file'])
        if not os.path.exists(path):
            t += note['ms']
            continue
        sa, srate, sch, ssw = am.read_wav(path)
        if srate != rate:
            t += note['ms']
            continue
        s = am._mono(sa)
        n = int(rate * note['ms'] / 1000.0)
        if not (0 < n < len(s)):
            t += note['ms']
            continue
        s = s[:n].copy()
        # 复现驱动的 80ms Q8 线性淡出（截断处），使残差仅反映 DMA 链路
        fade = min(int(rate * 80 / 1000.0), n)
        for k in range(fade):
            s[n - fade + k] *= (256 - 256 * k // fade) / 256.0

        r = am.align_and_compare(cap_mono, rate, s, approx_start_ms=t,
                                 max_offset_ms=150.0)
        r['index'] = note['index']
        r['file'] = note['file']
        out.append(r)
        t = (r['alignment_offset_ms'] + note['ms']
             if r.get('cross_correlation', 0.0) > 0.5 else t + note['ms'])
    return out


# ============================ 评分 ============================

def _lo(v, good, fail):
    """越小越好：<=good -> 100，>=fail -> 0，中间线性。"""
    if v is None:
        return None
    if v <= good:
        return 100.0
    if v >= fail:
        return 0.0
    return 100.0 * (fail - v) / (fail - good)


def _hi(v, good, fail):
    """越大越好：>=good -> 100，<=fail -> 0，中间线性。"""
    if v is None:
        return None
    if v >= good:
        return 100.0
    if v <= fail:
        return 0.0
    return 100.0 * (v - fail) / (good - fail)


def _bool(v):
    return None if v is None else (100.0 if v else 0.0)


def score(metrics, logp, mode, regression):
    """按维度评分。绝对评分只用测试音 + serial.log（见模块 docstring）。"""
    dims = {}

    def add(name, weight, items):
        vals = [s for _, s in items if s is not None]
        dims[name] = {
            'weight': weight,
            'score': float(np.mean(vals)) if vals else None,
            'items': {k: (None if s is None else round(s, 1)) for k, s in items},
        }

    sig = [
        ('thd_n_pct', _lo(metrics.get('thd_n_pct'), 0.1, 3.0)),
        ('snr_db', _hi(metrics.get('snr_db'), 60.0, 40.0)),
        ('dynamic_range_db', _hi(metrics.get('dynamic_range_db'), 60.0, 40.0)),
        ('clipping_count', _lo(metrics.get('clipping_count'), 0, 10)),
        ('dc_offset', _lo(abs(metrics['dc_offset']) if metrics.get('dc_offset') is not None else None,
                          50.0, 300.0)),
    ]
    if any(s is not None for _, s in sig):
        add('signal_fidelity', 30, sig)

    if metrics.get('freq_error_hz') is not None:
        add('frequency_accuracy', 15,
            [('freq_error_hz', _lo(metrics['freq_error_hz'], 0.5, 2.0))])

    art = [
        ('clicks_high', _lo(metrics.get('clicks_high'), 0, 3)),
        ('clicks_total', _lo(metrics.get('clicks_total'), 0, 10)),
        ('xrun_count', _lo(metrics.get('xrun_count'), 0, 1)),
    ]
    if metrics.get('spectral_flatness') is not None:
        art.append(('spectral_flatness', _lo(metrics['spectral_flatness'], 1e-3, 0.1)))
    if metrics.get('boundary_clicks') is not None:
        art.append(('boundary_clicks', _lo(metrics['boundary_clicks'], 0, 2)))
    if metrics.get('post_impulse_clicks') is not None:
        art.append(('post_impulse_clicks', _lo(metrics['post_impulse_clicks'], 0, 3)))
    if metrics.get('edge_clicks') is not None:
        art.append(('edge_clicks', _lo(metrics['edge_clicks'], 0, 3)))
    if metrics.get('burst_rms_consistency_pct') is not None:
        art.append(('burst_rms_consistency_pct',
                    _hi(metrics['burst_rms_consistency_pct'], 95.0, 80.0)))
    if any(s is not None for _, s in art):
        add('artifacts', 25, art)

    ver, err, st, ini = (logp.get('verbs', {}), logp.get('errors', {}),
                         logp.get('stream', {}), logp.get('init', {}))
    init_ok = bool(ini.get('device_found') and ini.get('codec_present') and
                   ini.get('crst') and ini.get('bus_master'))
    add('driver_health', 15, [
        ('verb_timeouts', _lo(ver.get('timeout_count'), 0, 1)),
        ('fifo_errors', _lo(err.get('fifo_errors'), 0, 1)),
        ('desc_errors', _lo(err.get('desc_errors'), 0, 1)),
        ('clean_start_stop', _bool(bool(st.get('started') and st.get('stopped')))),
        ('init_ok', _bool(init_ok)),
        ('stream_format_ok', _bool(st.get('rate') == 44100 and st.get('channels') == 2
                                   and st.get('bits') == 16)),
    ])

    if regression is not None:
        add('regression', 15, [('baseline_score', float(regression['score']))])

    weights = {k: v['weight'] for k, v in dims.items() if v['score'] is not None}
    wsum = sum(weights.values())
    overall = (sum(dims[k]['score'] * w / wsum for k, w in weights.items())
               if wsum > 0 else None)
    return dims, overall


# ============================ 回归对比 ============================

# 参与回归对比的指标：(键, 越大越好?)
REG_KEYS = [
    ('snr_db', True), ('thd_n_pct', False), ('freq_error_hz', False),
    ('clicks_high', False), ('xrun_count', False), ('boundary_clicks', False),
    ('piano_clicks_high', False), ('piano_gap_std_ms', False),
    ('piano_amplitude_consistency_pct', True), ('piano_source_cross_correlation', True),
]


def compare_baseline(cur, baseline):
    """与基线对比，返回 (regression dict, diff dict)。无基线返回 (None, {})。"""
    if not baseline:
        return None, {}
    bm = baseline.get('metrics', {})
    diff, penalties = {}, []
    for key, higher_better in REG_KEYS:
        if key not in cur or cur[key] is None or bm.get(key) is None:
            continue
        d = cur[key] - bm[key]
        diff[key] = round(d, 6)
        base = abs(bm[key]) if abs(bm[key]) > 1e-9 else 1.0
        rel = d / base
        if (higher_better and rel < -0.05) or ((not higher_better) and rel > 0.05):
            penalties.append(min(abs(rel), 1.0) * 100.0)
    nb = (cur.get('piano_clicks_high', 0) or 0) - (bm.get('piano_clicks_high', 0) or 0)
    if nb > 0:
        diff['new_piano_clicks'] = nb
        penalties.append(min(nb / 3.0, 1.0) * 100.0)
    s = 100.0 if not penalties else max(0.0, 100.0 - max(penalties))
    return {'score': s, 'penalties': [round(p, 1) for p in penalties]}, diff


# ============================ 报告 ============================

def bar(score, width=20):
    if score is None:
        return '[' + '-' * width + ']'
    n = int(round(score / 100.0 * width))
    return '[' + '#' * n + '-' * (width - n) + ']'


DIM_LABEL = {
    'signal_fidelity': '信号保真度',
    'frequency_accuracy': '频率准确度',
    'artifacts': '伪迹(artifact)',
    'driver_health': '驱动健康',
    'regression': '回归',
}

# (标签, 键, 单位, 目标)
SCORED_ROWS = [
    ('THD+N', 'thd_n_pct', '%', '目标<0.1'),
    ('THD', 'thd_pct', '%', '目标<0.5'),
    ('SNR', 'snr_db', 'dB', '目标>60'),
    ('动态范围', 'dynamic_range_db', 'dB', '目标>60'),
    ('频率误差', 'freq_error_hz', 'Hz', '目标<0.5'),
    ('直流偏置', 'dc_offset', '', '目标<50'),
    ('削波计数', 'clipping_count', '', '目标 0'),
    ('高置信 click', 'clicks_high', '', '目标 0'),
    ('click 总数', 'clicks_total', '', '目标 0'),
    ('xrun', 'xrun_count', '', '目标 0'),
    ('边界 click', 'boundary_clicks', '', '目标 0 (sequence)'),
    ('冲激后 click', 'post_impulse_clicks', '', '目标 0 (impulse)'),
    ('阶跃边沿 click', 'edge_clicks', '', '目标 0 (step)'),
    ('短音电平一致性', 'burst_rms_consistency_pct', '%', '目标>95 (sequence)'),
]
PIANO_ROWS = [
    ('click 总数', 'clicks_total', '', '参考(音乐瞬态)'),
    ('高置信 click', 'clicks_high', '', '参考'),
    ('内部静音段', 'gap_count', '', '参考(含休止符)'),
    ('间隙抖动', 'gap_std_ms', 'ms', '参考(含休止符)'),
    ('音量一致性', 'amplitude_consistency_pct', '%', '参考(音乐动态)'),
    ('立体声失衡', 'stereo_balance_db', 'dB', '参考(样本固有)'),
    ('源波形相关', 'source_cross_correlation', '', '参考>0.99'),
    ('源谱距离', 'source_lsd_db', 'dB', '参考<1.0'),
]


def build_recommendations(metrics, dims):
    """根据低于满分的分项生成带代码位置的优化建议。"""
    recs = []
    for dim, d in dims.items():
        for key, sc in d['items'].items():
            if sc is None or sc >= 99.9 or key == 'baseline_score':
                continue
            sev = 'warning' if sc < 60 else 'info'
            root, sug, loc = ADVICE.get(key, ('—', '—', '—'))
            val = metrics.get(key)
            recs.append({
                'severity': sev, 'dimension': dim, 'metric': key,
                'value': (None if val is None else round(float(val), 6)),
                'score': sc, 'root_cause': root, 'suggestion': sug,
                'code_location': loc,
            })
    recs.sort(key=lambda r: r['score'])
    return recs


def _fmt_row(metrics, log, label, key, unit, target):
    if key.startswith('_'):          # 伪键
        if key == '_hwerr':
            v = '%d / %d' % (log['errors']['fifo_errors'], log['errors']['desc_errors'])
        else:
            v = 'n/a'
    else:
        raw = metrics.get(key)
        v = 'n/a' if raw is None else (format(float(raw), '.4g') + unit)
    print(f"    {label:<14} {v:>14}   ({target})")


def print_report(report):
    o = report.get('overall_score')
    print('=' * 66)
    print('  HDA 音频驱动评价报告')
    print(f"  模式: {report['mode']}   灵敏度: {report['sensitivity']}   时间: {report['timestamp']}")
    print('=' * 66)
    print(f"  总分: {'n/a' if o is None else format(o, '.1f')} / 100")
    print('-' * 66)
    for dim, d in report['dimensions'].items():
        label = DIM_LABEL.get(dim, dim)
        sc = d['score']
        print(f"  {label:<14} {d['weight']:>3}%  "
              f"{'   n/a' if sc is None else format(sc, '6.1f')}  {bar(sc)}")
    m = report['metrics']
    log = report['log']
    print('-' * 66)
    print('  评分指标（测试音 + 驱动日志）')
    for label, key, unit, target in SCORED_ROWS:
        if key in m or key.startswith('_'):
            _fmt_row(m, log, label, key, unit, target)
    _fmt_row(m, log, 'FIFO/DESC 错误', '_hwerr', '', '目标 0/0')
    c = log['completion']
    sortv = 'OK' if log['sort']['verify_ok'] else (
        'FAIL' if c.get('demo_finished') else 'n/a')
    print(f"    {'排序校验':<14} {sortv:>14}   (目标 OK)")

    pm = report.get('piano_metrics', {})
    if pm:
        print('-' * 66)
        print('  参考指标（钢琴模式：音乐内容使绝对阈值不可靠，仅作对照/回归）')
        for label, key, unit, target in PIANO_ROWS:
            raw = pm.get(key)
            v = 'n/a' if raw is None else (format(float(raw), '.4g') + unit)
            print(f"    {label:<14} {v:>14}   ({target})")
        print(f"    {'音符数':<14} {log['melody']['notes_played']:>14}   (乐谱 84)")

    if report['recommendations']:
        print('-' * 66)
        print('  优化建议')
        for i, r in enumerate(report['recommendations'], 1):
            print(f"   {i}. [{r['severity']}] {r['dimension']}.{r['metric']} "
                  f"= {r['value']} (得分 {r['score']})")
            print(f"      根因: {r['root_cause']}")
            print(f"      方向: {r['suggestion']}")
            print(f"      代码: {r['code_location']}")
    if report.get('baseline_diff'):
        print('-' * 66)
        print('  与基线差异')
        for k, v in report['baseline_diff'].items():
            print(f"    {k:<34} {v:+.6g}")
    print('=' * 66)


# ============================ main ============================

def main():
    ap = argparse.ArgumentParser(description='HDA 音频驱动质量评价体系')
    ap.add_argument('--mode', choices=['test-tone', 'piano', 'both'], default='both')
    ap.add_argument('--full', action='store_true',
                    help='test-tone 额外跑 impulse/step/sequence')
    ap.add_argument('--sensitivity', choices=list(SENSITIVITY), default='moderate')
    ap.add_argument('--output', default=DEFAULT_OUTPUT)
    ap.add_argument('--baseline', default=DEFAULT_BASELINE)
    ap.add_argument('--update-baseline', action='store_true')
    ap.add_argument('--no-run', action='store_true', help='仅分析已有文件')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    sens = SENSITIVITY[args.sensitivity]
    signals = ['sine'] + (FULL_SIGNALS if args.full else [])

    metrics, piano_metrics, raw = {}, {}, {}
    logp = sp.parse_serial_log(SERIAL_LOG)

    if args.mode in ('test-tone', 'both'):
        for sig in signals:
            wav = os.path.join(ROOT, 'out_eval.wav' if sig == 'sine' else f'out_{sig}.wav')
            if not args.no_run:
                run_headless(sig, wav, verbose=args.verbose)
            m, r = analyze_test_tone(wav, sig, sens)
            for k, v in m.items():          # sine 优先，其余信号补充
                if v is not None and k not in metrics:
                    metrics[k] = v
            raw[f'test_{sig}'] = r
            logp = sp.parse_serial_log(SERIAL_LOG)

    if args.mode in ('piano', 'both'):
        piano_wav = os.path.join(ROOT, 'out_piano.wav')
        if not args.no_run:
            run_headless(None, piano_wav, verbose=args.verbose)
        m, r = analyze_piano(piano_wav, sens)
        piano_metrics = {k: v for k, v in m.items() if v is not None}
        raw['piano'] = r
        logp = sp.parse_serial_log(SERIAL_LOG)

    baseline = None
    if os.path.exists(args.baseline):
        with open(args.baseline, 'r', encoding='utf-8') as f:
            baseline = json.load(f)

    # 回归对比：评分指标 + 带 piano_ 前缀的参考指标
    reg_cur = dict(metrics)
    for k, v in piano_metrics.items():
        reg_cur[f'piano_{k}'] = v
    regression, diff = compare_baseline(reg_cur, baseline)

    dims, overall = score(metrics, logp, args.mode, regression)
    recs = build_recommendations(metrics, dims)

    def _j(v):
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            return None if v is None else float(v)
        return v

    report = {
        'timestamp': datetime.now().isoformat(timespec='seconds'),
        'mode': args.mode, 'sensitivity': args.sensitivity, 'signals': signals,
        'overall_score': overall,
        'dimensions': dims,
        'metrics': {k: _j(v) for k, v in metrics.items()},
        'piano_metrics': {k: _j(v) for k, v in piano_metrics.items()},
        'raw': raw,
        'log': logp,
        'regression': regression,
        'recommendations': recs,
        'baseline_diff': diff,
    }
    # 基线保存为「评分指标 + piano_ 前缀参考指标」，供下次回归对比
    report['metrics'] = {**report['metrics'],
                         **{f'piano_{k}': v for k, v in report['piano_metrics'].items()}}

    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, 'w', encoding='utf-8') as f:
        json.dump(report, f, ensure_ascii=False, indent=2)
    print_report(report)
    print(f"  报告已写入: {args.output}")

    if args.update_baseline:
        with open(args.baseline, 'w', encoding='utf-8') as f:
            json.dump(report, f, ensure_ascii=False, indent=2)
        print(f"  基线已更新: {args.baseline}")

    return 0 if (overall is None or overall >= 60.0) else 1


if __name__ == '__main__':
    sys.exit(main())
