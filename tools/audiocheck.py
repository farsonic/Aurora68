#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Look at what the kernel's sound output played in the testbed (mac.audiodump=PATH, psmac_audio.c).

    tools/audiocheck.py dump.pcm [--rate 48000] [--from S --len S] [--peaks 8] [--freq 440 --freq 660]

The file is what core 1 would have handed to HDMI or the jack: 16-bit little-endian stereo frames
at the output's rate. The tool finds the stretches where something sounds (RMS above --level), and
for each, or for the window --from/--len, takes a Hann-windowed spectrum of each channel and lists
the strongest peaks, interpolated between bins. --freq F prints the level of F in both channels and
their difference in dB, which is the channels' separation for a tone played on one side only.
Standard library only, like the rest of tools/ (a radix-2 FFT in Python: a second or two for a
65,536-point window). Written on 20 November 2026 for the startup chime (C4 260.8 Hz, F4 347.8 Hz,
C5 521.6 Hz, docs/08 item 71) and the stereo test of mac.audiotest=.
"""
import argparse, array, cmath, math, sys

ap = argparse.ArgumentParser()
ap.add_argument('file')
ap.add_argument('--rate', type=int, default=48000)
ap.add_argument('--from', dest='start', type=float)
ap.add_argument('--len', type=float, default=1.0)
ap.add_argument('--peaks', type=int, default=8)
ap.add_argument('--freq', type=float, action='append', default=[])
ap.add_argument('--level', type=float, default=300.0, help='RMS that counts as sound (16-bit units)')
ap.add_argument('--fft', type=int, default=32768)
a = ap.parse_args()

pcm = array.array('h')
with open(a.file, 'rb') as f:
    pcm.frombytes(f.read())
if sys.byteorder == 'big':
    pcm.byteswap()
left, right = pcm[0::2], pcm[1::2]
n = len(left)
print(f"{a.file}: {n} frames, {n / a.rate:.2f} s at {a.rate} Hz")


def fft(x):
    """iterative radix-2, in place on a list of complex numbers"""
    n = len(x)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            x[i], x[j] = x[j], x[i]
    size = 2
    while size <= n:
        w = cmath.exp(-2j * math.pi / size)
        half = size // 2
        tw = [w ** k for k in range(half)]
        for s in range(0, n, size):
            for k in range(half):
                u, v = x[s + k], x[s + k + half] * tw[k]
                x[s + k], x[s + k + half] = u + v, u - v
        size *= 2
    return x


def spectrum(ch, start, length):
    m = 1
    while m * 2 <= min(length, a.fft):
        m *= 2
    seg = ch[start:start + m]
    win = [0.5 - 0.5 * math.cos(2 * math.pi * i / (m - 1)) for i in range(m)]
    mag = [abs(v) for v in fft([complex(seg[i] * win[i]) for i in range(m)])[:m // 2]]
    return mag, m


def db(v):
    return 20 * math.log10(v) if v > 0 else -999.0


def peaks(mag, m, count):
    found = []
    for i in range(2, len(mag) - 1):
        if mag[i] > mag[i - 1] and mag[i] >= mag[i + 1]:
            found.append((mag[i], i))
    found.sort(reverse=True)
    out = []
    for v, i in found:
        y0, y1, y2 = db(mag[i - 1]), db(mag[i]), db(mag[i + 1])
        d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2) if (y0 - 2 * y1 + y2) else 0.0
        f = (i + d) * a.rate / m
        if all(abs(f - g) > 2 * a.rate / m for g, _ in out):
            out.append((f, y1))
        if len(out) == count:
            break
    return out


def level_at(mag, m, freq):
    i = round(freq * m / a.rate)
    return max(mag[max(0, i - 2):i + 3]) if i < len(mag) else 0.0


def report(start, length, title):
    print(f"\n{title}: {start / a.rate:.2f} s to {(start + length) / a.rate:.2f} s")
    specs = []
    for name, ch in (('left', left), ('right', right)):
        mag, m = spectrum(ch, start, length)
        specs.append((mag, m))
        rms = math.sqrt(sum(v * v for v in ch[start:start + length]) / max(1, length))
        top = db(max(mag[4:]))
        print(f"  {name:5}: RMS {rms:7.1f}, peaks " + ", ".join(f"{f:.1f} Hz ({y - top:+.0f} dB)" for f, y in peaks(mag, m, a.peaks)))
    for fq in a.freq:
        l, r = level_at(*specs[0], fq), level_at(*specs[1], fq)
        print(f"  {fq:.1f} Hz: left {db(l):6.1f} dB, right {db(r):6.1f} dB, left - right {db(l) - db(r):+.1f} dB")


if a.start is not None:
    s = int(a.start * a.rate)
    report(s, min(int(a.len * a.rate), n - s), "window")
else:
    blk = a.rate // 20
    runs, cur = [], None
    for b in range(0, n - blk, blk):
        loud = max(math.sqrt(sum(v * v for v in ch[b:b + blk]) / blk) for ch in (left, right)) > a.level
        if loud and cur is None:
            cur = b
        if not loud and cur is not None:
            runs.append((cur, b - cur)); cur = None
    if cur is not None:
        runs.append((cur, n - cur))
    if not runs:
        print("no sound above the level anywhere")
    for i, (s, length) in enumerate(runs):
        report(s, length, f"sound {i + 1}")
