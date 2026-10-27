#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成黄金向量：tests/fixtures/vectors.{c,json}

期望值一律来自 tools/audio_metrics.py（Python 参照实现），
C 侧只读生成的常量表，不解析 JSON，也不许把被测代码的输出抄回来。

用法：
    uv run --no-project --with numpy python tests/fixtures/gen_vectors.py
"""
import json
import pathlib
import sys

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "tools"))

import audio_metrics as am  # noqa: E402

RATE = 44100.0
N = 8192           # 0.186s：参照实现的 ±0.5Hz 细化窗口要粗估够准（1024 点差 3.9Hz 会失准），
                   # 44100 点则让 vectors.c 涨到 1.4MB，8192 点是两者的折中
SPECTRUM_N = 1024  # B4 的频谱窗，对应 43.07Hz/bin


def q(x):
    return np.clip(np.round(x), -32768, 32767).astype(np.int16)


def build_cases():
    t = np.arange(N) / RATE
    two_pi = 2.0 * np.pi
    rng = np.random.default_rng(1234)

    sine = 0.9 * 32767.0 * np.sin(two_pi * 1000.0 * t)
    harm = (0.80 * np.sin(two_pi * 1000.0 * t)
            + 0.0080 * np.sin(two_pi * 2000.0 * t)
            + 0.0040 * np.sin(two_pi * 3000.0 * t)) * 32767.0
    noise = 0.5 * 32767.0 * np.sin(two_pi * 1000.0 * t) + rng.normal(0.0, 0.5 * 32767.0 * 10 ** (-50 / 20.0), N)
    silence = np.zeros(N)
    dc = 0.5 * 32767.0 * np.sin(two_pi * 1000.0 * t) + 100.0

    return [
        ("sine_1k_0dbfs", q(sine)),
        ("sine_1k_plus_h2_h3", q(harm)),
        ("noise_white_seed1234", q(noise)),
        ("silence", q(silence)),
        ("dc_offset_100", q(dc)),
    ]


def peak_bin(samples):
    """与 C 侧 met_peak_bin 同定义：取前 SPECTRUM_N 点、Hann 窗、只看 5..nfft/2。"""
    x = samples[:SPECTRUM_N].astype(np.float64)
    x = x - x.mean()
    nfft = 1 << (len(x) - 1).bit_length()
    power = np.abs(np.fft.rfft(x * np.hanning(len(x)), nfft)) ** 2
    hi = nfft // 2
    k = int(np.argmax(power[5:hi + 1])) + 5
    return k


def main():
    rows = []
    for name, samples in build_cases():
        res = am.fft_analysis(samples, RATE)
        if "error" in res:
            raise SystemExit(f"{name}: audio_metrics 返回错误 {res['error']}")
        rows.append({
            "name": name,
            "n": int(len(samples)),
            "rate": RATE,
            "f0_hz": res["fundamental_hz"],
            "rms": res["rms"],
            "thd_pct": res["thd_pct"],
            "thd_n_pct": res["thd_n_pct"],
            "snr_db": res["snr_db"],
            "peak_bin": peak_bin(samples),
            "samples": samples,
        })

    (HERE / "vectors.json").write_text(
        json.dumps([{k: v for k, v in r.items() if k != "samples"} for r in rows], indent=2, ensure_ascii=False),
        encoding="utf-8",
    )

    out = [
        "/* 自动生成，勿手改：tests/fixtures/gen_vectors.py",
        " * 期望值来源：tools/audio_metrics.py（Python 参照实现） */",
        '#include "vectors.h"',
        "",
    ]
    for r in rows:
        vals = r["samples"].tolist()
        out.append(f"static const short vec_{r['name']}[{len(vals)}] = {{")
        for i in range(0, len(vals), 16):
            out.append("    " + ", ".join(str(v) for v in vals[i:i + 16]) + ",")
        out.append("};")
        out.append("")

    out.append("const vec_case vec_cases[] = {")
    for r in rows:
        out.append(
            f'    {{ "{r["name"]}", vec_{r["name"]}, {r["n"]}, {r["rate"]:.1f}, '
            f'{r["f0_hz"]:.9f}, {r["rms"]:.9f}, {r["thd_pct"]:.9f}, '
            f'{r["thd_n_pct"]:.9f}, {r["snr_db"]:.9f}, {r["peak_bin"]} }},'
        )
    out.append("};")
    out.append("")
    out.append(f"const int vec_case_count = {len(rows)};")
    out.append("")
    out.append("""const vec_case *vec_find(const char *name)
{
    for (int i = 0; i < vec_case_count; i++) {
        const char *a = vec_cases[i].name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\\0' && *b == '\\0')
            return &vec_cases[i];
    }
    return 0;
}""")
    out.append("")
    (HERE / "vectors.c").write_text("\n".join(out), encoding="utf-8")

    for r in rows:
        print(f'{r["name"]:<22} f0={r["f0_hz"]:.6f} rms={r["rms"]:.3f} '
              f'thd={r["thd_pct"]:.6f}% thdn={r["thd_n_pct"]:.6f}% snr={r["snr_db"]:.3f}dB bin={r["peak_bin"]}')
    print(f'\nwrote {HERE / "vectors.c"} and vectors.json ({len(rows)} cases, n={N}, rate={RATE:.0f})')


if __name__ == "__main__":
    main()
