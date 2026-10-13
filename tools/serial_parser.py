#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
serial_parser.py - 解析 EPOS serial.log，提取驱动健康与运行状态指标

serial.log 包含内核 printk() 与用户态 printf() 的全部输出，是运行时状态的
首要来源。本模块把它解析成结构化 dict，供 eval_audio.py 评分。

关注点：
  - HDA 初始化是否干净（设备/编解码器/BAR/控制器复位）
  - verb 通信是否全部有响应（无 IC timeout）
  - 连续流是否正常启动/停止，格式是否符合预期
  - 是否有 FIFO/描述符错误
  - 排序是否通过校验（功能正确性）
  - 乐谱音符列表（用于时序分析）
  - 完成标记（demo / test tone）
"""
import json
import re

# ---- 行级正则（与 hda.c / hda_stream.c / main.c 的输出格式对应） ----
RE_DEVICE = re.compile(r'HDA: Found device! BAR0 = (0x[0-9a-fA-F]+)')
RE_PCI_CMD = re.compile(r'HDA: PCI CMD after\s+= (0x[0-9a-fA-F]+) \(BME=(\d)')
RE_GCAP = re.compile(r'HDA: GCAP=(0x[0-9a-fA-F]+) v([\d.]+) \(OSS=(\d+) ISS=(\d+) BSS=(\d+)\)')
RE_GCTL = re.compile(r'HDA: GCTL=(0x[0-9a-fA-F]+) \(CRST=(\d)\)')
RE_INTCTL = re.compile(r'HDA: INTCTL set to (0x[0-9a-fA-F]+)')
RE_STATESTS = re.compile(r'HDA: STATESTS = (0x[0-9a-fA-F]+)')
RE_CODEC = re.compile(r'HDA: Codec (\d+) present')
RE_VERB = re.compile(r'HDA: verb resp data=0x([0-9a-fA-F]+) cad=(\d+)')
RE_SETCONV = re.compile(r'HDA: SET_CONVERTER_STREAM_CHANNEL verb=0x([0-9a-fA-F]+)')
RE_GETCONV = re.compile(r'HDA: GET_CONV resp=0x([0-9a-fA-F]+)')
RE_IC_TIMEOUT = re.compile(r'HDA: IC timeout')
RE_SCAN = re.compile(r'HDA: scan NID=0x([0-9a-fA-F]+) GET_CONV=0x([0-9a-fA-F]+)')
RE_STREAM_START = re.compile(r'continuous stream started \(CBL=(\d+), (\d+)Hz (\d+)ch (\d+)bit\)')
RE_STREAM_STOP = re.compile(r'continuous stream stopped \(tail=(\d+) ms\)')
RE_ERRORS = re.compile(r'\[AUDIO\] errors: fifo=(\d+) desc=(\d+)\s+position: (\d+) ms')
RE_FIFO_ERR = re.compile(r'HDA: FIFO error')
RE_DESC_ERR = re.compile(r'HDA: Desc error')
RE_VOLUME = re.compile(r'volume set to (\d+)')
RE_SORT_VERIFY = re.compile(r'\[SORT\] verify: array is ascending = (OK|FAIL)')
RE_SORT_STEPS = re.compile(r'\[SORT\] ---- done in (\d+) steps ----')
RE_MELODY = re.compile(r'\[MELODY\] #(\d+) note=(\d+) ms=(\d+) voices=(\d+) (\S+)')
# [TEST] 行可能带内核前缀（"...first user task...[TEST] sine 440Hz, 2000ms"），
# 只取 [TEST] 之后的内容。
RE_TEST = re.compile(r'\[TEST\]\s+(.+?)\s*$')

# ---- 调用链日志（hda_trace.c dump，见 kernel/hda_trace.c 的格式注释） ----
RE_TRACE_BEGIN = re.compile(r'\[TRACE\] begin recs=(\d+) lost=(\d+) first=(\d+)')
RE_TRACE_EV = re.compile(
    r'\[TRACE\] t=(\d+) ev=(\w+) id=(\d+) s=(\d+) e=(\d+) rd=(\d+) wr=(\d+)')
RE_TRACE_END = re.compile(r'\[TRACE\] end\s*$')

# ---- 节拍抖动测试（main.c：TEST_SIGNAL=tempo） ----
RE_TEMPO = re.compile(r'\[TEMPO\] #(\d+) t=(\d+) interval=(\d+)')
RE_TEMPO_DONE = re.compile(r'\[TEMPO\] done, underruns=(\d+)')

# ---- underrun 注入测试（main.c：TEST_SIGNAL=underrun） ----
RE_UD_INJECT = re.compile(r'\[UNDERRUN\] injecting (\d+) ms stall')
RE_UD_DETECTED = re.compile(r'\[UNDERRUN\] detected=(\d+)')

MARK_DEMO = 'Demo finished. Check serial.log'
MARK_TEST = 'Test tone finished. Check serial.log'


def _read(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        return f.read()


def parse_serial_log(path):
    """解析 serial.log，返回结构化 dict（全部 JSON 可序列化）。"""
    try:
        text = _read(path)
    except FileNotFoundError:
        return {'error': f'serial log not found: {path}'}

    lines = text.splitlines()
    out = {
        'init': {'device_found': False, 'codec_present': False},
        'verbs': {'total_count': 0, 'timeout_count': 0, 'all_responded': True,
                  'set_converter_ok': False, 'nids_scanned': 0},
        'stream': {'started': False, 'stopped': False},
        'errors': {'fifo_errors': 0, 'desc_errors': 0, 'position_ms': 0, 'has_xrun': False},
        'sort': {'verify_ok': False, 'total_steps': 0},
        'melody': {'notes_played': 0, 'note_list': [], 'note_durations_ms': []},
        'completion': {'demo_finished': False, 'test_tone_finished': False,
                       'vga_ok': False, 'sort_ok': False, 'melody_ok': False},
        'test_signal': None,
        'volume': None,
        'trace': {'present': False, 'recs': 0, 'lost': 0, 'first': 0, 'events': []},
        'tempo': {'present': False, 'times_ms': [], 'intervals_ms': [], 'underruns': None},
        'underrun_test': {'present': False, 'stall_ms': 0, 'detected': None},
        'warnings': [],
        'line_count': len(lines),
    }

    in_trace = False
    for ln in lines:
        # 调用链日志段落（hda_trace_dump 输出）：状态机采集，不参与其它解析
        if RE_TRACE_BEGIN.search(ln):
            m = RE_TRACE_BEGIN.search(ln)
            out['trace'].update(present=True, recs=int(m.group(1)),
                                lost=int(m.group(2)), first=int(m.group(3)))
            in_trace = True
            continue
        if in_trace:
            if RE_TRACE_END.search(ln):
                in_trace = False
            else:
                m = RE_TRACE_EV.search(ln)
                if m:
                    out['trace']['events'].append({
                        't': int(m.group(1)), 'ev': m.group(2), 'id': int(m.group(3)),
                        's': int(m.group(4)), 'e': int(m.group(5)),
                        'rd': int(m.group(6)), 'wr': int(m.group(7)),
                    })
            continue

        m = RE_DEVICE.search(ln)
        if m:
            out['init']['device_found'] = True
            out['init']['bar0'] = m.group(1)
        m = RE_PCI_CMD.search(ln)
        if m:
            out['init']['pci_cmd'] = m.group(1)
            out['init']['bus_master'] = bool(int(m.group(2)))
        m = RE_GCAP.search(ln)
        if m:
            out['init'].update(gcap=m.group(1), version=m.group(2),
                               oss=int(m.group(3)), iss=int(m.group(4)), bss=int(m.group(5)))
        m = RE_GCTL.search(ln)
        if m:
            out['init']['gctl'] = m.group(1)
            out['init']['crst'] = bool(int(m.group(2)))
        m = RE_INTCTL.search(ln)
        if m:
            out['init']['intctl'] = m.group(1)
        m = RE_STATESTS.search(ln)
        if m:
            out['init']['statests'] = m.group(1)
        m = RE_CODEC.search(ln)
        if m:
            out['init']['codec_present'] = True
            out['init']['codec_cad'] = int(m.group(1))

        if RE_IC_TIMEOUT.search(ln):
            out['verbs']['timeout_count'] += 1
            out['warnings'].append(ln.strip())
        m = RE_VERB.search(ln)
        if m:
            out['verbs']['total_count'] += 1
        m = RE_SETCONV.search(ln)
        if m:
            out['verbs']['set_converter_ok'] = True
            out['verbs']['set_converter_verb'] = m.group(1)
        m = RE_SCAN.search(ln)
        if m:
            out['verbs']['nids_scanned'] += 1

        m = RE_STREAM_START.search(ln)
        if m:
            out['stream'].update(started=True, cbl=int(m.group(1)), rate=int(m.group(2)),
                                 channels=int(m.group(3)), bits=int(m.group(4)))
        m = RE_STREAM_STOP.search(ln)
        if m:
            out['stream']['stopped'] = True
            out['stream']['tail_ms'] = int(m.group(1))
        m = RE_FIFO_ERR.search(ln)
        if m:
            out['errors']['fifo_errors'] += 1
            out['warnings'].append(ln.strip())
        m = RE_DESC_ERR.search(ln)
        if m:
            out['errors']['desc_errors'] += 1
            out['warnings'].append(ln.strip())
        m = RE_ERRORS.search(ln)
        if m:
            out['errors']['fifo_errors'] += int(m.group(1))
            out['errors']['desc_errors'] += int(m.group(2))
            out['errors']['position_ms'] = int(m.group(3))

        m = RE_VOLUME.search(ln)
        if m:
            out['volume'] = int(m.group(1))
        m = RE_SORT_VERIFY.search(ln)
        if m:
            out['sort']['verify_ok'] = (m.group(1) == 'OK')
        m = RE_SORT_STEPS.search(ln)
        if m:
            out['sort']['total_steps'] = int(m.group(1))
        m = RE_MELODY.search(ln)
        if m:
            note = {'index': int(m.group(1)), 'midi_note': int(m.group(2)),
                    'ms': int(m.group(3)), 'voices': int(m.group(4)), 'file': m.group(5)}
            out['melody']['note_list'].append(note)
            out['melody']['note_durations_ms'].append(note['ms'])
        out['melody']['notes_played'] = len(out['melody']['note_list'])

        m = RE_TEST.search(ln)
        if m and out['test_signal'] is None:
            out['test_signal'] = m.group(1).strip()

        m = RE_TEMPO.search(ln)
        if m:
            out['tempo']['present'] = True
            out['tempo']['times_ms'].append(int(m.group(2)))
            out['tempo']['intervals_ms'].append(int(m.group(3)))
        m = RE_TEMPO_DONE.search(ln)
        if m:
            out['tempo']['underruns'] = int(m.group(1))

        m = RE_UD_INJECT.search(ln)
        if m:
            out['underrun_test'].update(present=True, stall_ms=int(m.group(1)))
        m = RE_UD_DETECTED.search(ln)
        if m:
            out['underrun_test']['detected'] = int(m.group(1))

        if MARK_DEMO in ln:
            out['completion']['demo_finished'] = True
        if MARK_TEST in ln:
            out['completion']['test_tone_finished'] = True
        if '[VGA]' in ln and 'OK' in ln:
            out['completion']['vga_ok'] = True
        if '[MELODY]' in ln and 'note' in ln:
            out['completion']['melody_ok'] = True

    out['completion']['sort_ok'] = out['sort']['verify_ok']
    out['verbs']['all_responded'] = (out['verbs']['timeout_count'] == 0)
    out['errors']['has_xrun'] = (out['errors']['fifo_errors'] > 0 or
                                 out['errors']['desc_errors'] > 0)

    # 未识别但可疑的错误行（跳过已知的汇总行/正常输出）
    for ln in lines:
        low = ln.lower()
        if any(k in low for k in ('failed', 'error', 'timeout', 'panic', 'fault')):
            if 'continuous stream stopped' in low:
                continue
            if '[audio] errors:' in low:          # 汇总行：计数已单独解析
                continue
            if '[trace]' in low:                  # 调用链日志：已结构化解析
                continue
            if ln.strip() not in out['warnings']:
                out['warnings'].append(ln.strip())

    return out


def write_trace_log(parsed, path, source='serial.log'):
    """把 serial.log 中的 [TRACE] 段落落盘为 audio_trace.log。

    文件包含三部分：原始事件流、按 note_id 重建的音符调用链区间视图、
    error 路径清单。返回写入的事件条数；无 trace 段落时不创建文件、返回 0。
    """
    tr = parsed.get('trace') or {}
    if not tr.get('present'):
        return 0

    events = tr.get('events', [])
    lines = [
        '# HDA audio driver call-chain trace (produced by kernel/hda_trace.c)',
        '# source: %s' % source,
        '# records: %d  lost: %d' % (tr.get('recs', 0), tr.get('lost', 0)),
        '# clock: t = PIT ticks (HZ=100, 10ms); s/e = absolute byte range; rd/wr = DMA ptrs',
        '',
        '# ---- raw events ----',
    ]
    for e in events:
        lines.append('t=%-8d %-12s id=%-3d s=%-10d e=%-10d rd=%-10d wr=%d'
                     % (e['t'], e['ev'], e['id'], e['s'], e['e'], e['rd'], e['wr']))

    # 调用链重建：按 note_id 聚合音符区间，呈现 submit/written/done 三阶段
    notes, order = {}, []
    for e in events:
        nid = e['id']
        if nid not in notes:
            notes[nid] = {'submit': None, 'written': None, 'done': None,
                          'start': e['s'], 'end': e['e']}
            order.append(nid)
        n = notes[nid]
        if e['ev'] == 'NOTE_SUBMIT':
            n['submit'], n['start'] = e['t'], e['s']
        elif e['ev'] == 'NOTE_WRITTEN':
            n['written'], n['end'] = e['t'], e['e']
        elif e['ev'] == 'NOTE_DONE':
            n['done'] = e['t']

    lines.append('')
    lines.append('# ---- note call chain (note_id, byte range, submit/written/done ticks) ----')
    for nid in order:
        n = notes[nid]
        lines.append('note %-3d [%d, %d)  submit@%s  written@%s  done@%s'
                     % (nid, n['start'], n['end'], n['submit'], n['written'], n['done']))

    errors = [e for e in events
              if e['ev'] in ('UNDERRUN', 'REALIGN', 'WAIT_TIMEOUT', 'IRQ_FALLBACK')]
    lines.append('')
    lines.append('# ---- error paths ----')
    if errors:
        for e in errors:
            lines.append('t=%-8d %-12s id=%-3d s=%-10d e=%-10d rd=%-10d wr=%d'
                         % (e['t'], e['ev'], e['id'], e['s'], e['e'], e['rd'], e['wr']))
    else:
        lines.append('(none)')

    with open(path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    return len(events)


if __name__ == '__main__':
    import argparse

    ap = argparse.ArgumentParser(
        description='解析 EPOS serial.log；若含 [TRACE] 段落则落盘调用链日志')
    ap.add_argument('serial_log', help='serial.log 路径')
    ap.add_argument('--out', default='audio_trace.log', help='调用链日志输出路径')
    args = ap.parse_args()

    parsed = parse_serial_log(args.serial_log)
    stripped = {k: v for k, v in parsed.items() if k != 'trace'}
    print(json.dumps(stripped, indent=2, ensure_ascii=False, default=str))

    n = write_trace_log(parsed, args.out, args.serial_log)
    if n:
        print('trace log written: %s (%d events)' % (args.out, n))
    else:
        print('no [TRACE] section found; %s not written' % args.out)
