#!/usr/bin/env python3
"""把 MP3（或 miniaudio 支持的其它格式）转成 EPOS 驱动可直接播放的 WAV。

产物格式由驱动约束决定（见 kernel/hda_audio.c、kernel/hda_stream.c）：
  - 单声道 44.1kHz 16-bit PCM：连续流只接受 16-bit；流格式只映射 44.1k/48k，
    故必须重采样到 44100；单声道把体积减半（整曲 220s ≈ 18.5MB）。
  - 整曲不一次性载入内核堆（12MB），由 sys_audio_play_file_stream_ms
    （syscall 198）从 FAT 分块流式播放，故文件长度不受内存限制。

用法（miniaudio 提供 MP3 解码；宿主无需 ffmpeg）：
  uv run --no-project --with miniaudio python tools/prepare_song.py \
      "E:/Download/KwDownload/song/Enya-We Wish You a Merry Christmas.mp3" \
      samples/song.wav

生成的 samples/song.wav 由顶层 Makefile 的 `samples/*.wav` 规则自动拷入
hd.img；文件不进 git（见 .gitignore 的 samples/song*.wav）。
"""
import os
import sys

import miniaudio

DEFAULT_SRC = (r"E:\Download\KwDownload\song"
               r"\Enya-We Wish You a Merry Christmas.mp3")
DEFAULT_DST = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           os.pardir, 'samples', 'song.wav')


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_SRC
    dst = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else DEFAULT_DST)

    if not os.path.exists(src):
        print('source not found: %s' % src)
        return 1

    info = miniaudio.get_file_info(src)
    print('source : %s' % info)

    decoded = miniaudio.decode_file(src,
                                    output_format=miniaudio.SampleFormat.SIGNED16,
                                    nchannels=1, sample_rate=44100)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    miniaudio.wav_write_file(dst, decoded)

    size = os.path.getsize(dst)
    print('written: %s' % dst)
    print('format : %d ch, %d Hz, 16-bit, %.1f s, %.2f MB'
          % (decoded.nchannels, decoded.sample_rate,
             len(decoded.samples) / float(decoded.sample_rate),
             size / 1048576.0))
    print('next   : cd %s && make hd.img  (自动 mcopy samples/*.wav)'
          % os.path.dirname(os.path.dirname(dst)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
