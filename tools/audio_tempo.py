#!/usr/bin/env python3
"""Rough tempo / brightness of a 16-bit stereo WAV section: python tools/audio_tempo.py WAV T0 T1 [T0 T1 ...]"""
import array
import math
import sys
import wave


def section(path, t0, t1):
    w = wave.open(path)
    sr = w.getframerate()
    w.setpos(int(t0 * sr))
    raw = array.array('h', w.readframes(int((t1 - t0) * sr)))
    mono = [(raw[i] + raw[i + 1]) / 65536.0 for i in range(0, len(raw) - 1, 2)]
    return mono, sr


def analyse(mono, sr):
    hop = 512
    env = [math.sqrt(sum(x * x for x in mono[i:i + hop]) / hop) for i in range(0, len(mono) - hop, hop)]
    d = [max(env[i + 1] - env[i], 0.0) for i in range(len(env) - 1)]
    m = sum(d) / len(d)
    d = [x - m for x in d]
    fps = sr / hop
    best, lag = -1e9, 0
    for L in range(int(fps * 60 / 220), int(fps * 60 / 60)):
        c = sum(d[i] * d[i + L] for i in range(len(d) - L)) / (len(d) - L)
        if c > best:
            best, lag = c, L
    zc = sum(1 for i in range(1, len(mono)) if (mono[i - 1] < 0) != (mono[i] < 0)) / (len(mono) / sr) / 2
    rms = math.sqrt(sum(x * x for x in mono) / len(mono))
    return 60 * fps / lag, zc, rms


if __name__ == '__main__':
    path = sys.argv[1]
    t = [float(x) for x in sys.argv[2:]]
    for t0, t1 in zip(t[::2], t[1::2]):
        bpm, zc, rms = analyse(*section(path, t0, t1))
        print(f'{path} {t0:.0f}-{t1:.0f}s: tempo ~{bpm:.1f} bpm, zero-cross {zc:.0f}/s, rms {rms:.3f}')
