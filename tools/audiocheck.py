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

    tools/audiocheck.py dump.pcm --breaks 1000,1500 [--threshold 0.05]

--breaks FL,FR looks for every break in a steady sine, FL Hz on the left and FR Hz on the right (the
radio test's tone, tools/mksounds.py --radio; multiples of 100 Hz, so that 10 ms hold whole periods). It
measures the tone's phase and amplitude in every 10 ms: a lost or doubled MPEG frame moves the phase (the
decoder's overlapping windows blend the two sides over a millisecond or so, which is why the phase is
watched rather than the samples), a gap is a stretch of silence, a weak spot a dip. Damage (noise, a
damaged frame) shows in the residual of x[n+1] + x[n-1] = 2 cos(w) x[n], which a sine keeps near zero;
above --threshold times the amplitude a sample counts as damaged. A radio stream's metadata blocks and
reconnects can so be checked to the sample (21 November 2026).
"""
import argparse, array, cmath, math, operator, sys

ap = argparse.ArgumentParser()
ap.add_argument('file')
ap.add_argument('--rate', type=int, default=48000)
ap.add_argument('--from', dest='start', type=float)
ap.add_argument('--len', type=float, default=1.0)
ap.add_argument('--peaks', type=int, default=8)
ap.add_argument('--freq', type=float, action='append', default=[])
ap.add_argument('--level', type=float, default=300.0, help='RMS that counts as sound (16-bit units)')
ap.add_argument('--fft', type=int, default=32768)
ap.add_argument('--breaks', help='FL,FR: find the breaks in steady sines of these frequencies')
ap.add_argument('--threshold', type=float, default=0.05, help='--breaks: a residual above this times the amplitude is a break')
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


def breaks(ch, freq, name):
    """the stretches of steady tone in one channel, and what broke them"""
    N = a.rate // 100                                     # 10 ms: a whole number of periods of FL and FR
    w = 2 * math.pi * freq / a.rate
    tc, ts = [math.cos(w * k) for k in range(N)], [math.sin(w * k) for k in range(N)]
    wins = []                                             # (start, amplitude, phase) of each 10 ms
    for b in range(0, n - N, N):
        seg = ch[b:b + N]
        c, s = sum(map(operator.mul, seg, tc)), sum(map(operator.mul, seg, ts))
        wins.append((b, 2 * math.hypot(c, s) / N, math.atan2(c, s)))
    amps = sorted(x[1] for x in wins)
    amp = amps[len(amps) * 3 // 4] if amps else 0.0       # the tone's amplitude where it sounds
    if amp < 100:
        print(f"  {name}: no tone")
        return
    runs, cur = [], None
    for b, v, _ in wins:
        if v > 0.5 * amp and cur is None:
            cur = b
        elif v <= 0.5 * amp and cur is not None:
            runs.append((cur, b)); cur = None
    if cur is not None:
        runs.append((cur, wins[-1][0] + N))
    steps, dips, dmg = [], [], []
    c2, lim = 2 * math.cos(w), a.threshold * amp
    drift, nd = 0.0, 0
    for s0, s1 in runs:
        prev, ev = None, None                             # a phase step: consecutive windows that moved
        for b, v, ph in wins[s0 // N + 2:s1 // N - 2]:
            if v < 0.9 * amp:
                if not dips or b - dips[-1][1] > N:
                    dips.append([b, b + N, v])
                else:
                    dips[-1][1] = b + N; dips[-1][2] = min(dips[-1][2], v)
            if prev is not None:
                d = (ph - prev + math.pi) % (2 * math.pi) - math.pi
                if abs(d) > 0.05:
                    if ev and b - ev[1] <= 5 * N:
                        ev[1] = b; ev[2] += d
                    else:
                        ev = [b, b, d]; steps.append(ev)
                else:
                    drift += d; nd += 1
            prev = ph
        last = -a.rate                                    # damage: a sample that is no part of a sine
        for i in range(s0 + 2 * N, s1 - 2 * N):
            r = ch[i + 1] + ch[i - 1] - c2 * ch[i]
            if abs(r) > lim and i - last > a.rate // 50:
                dmg.append((i, abs(r) / amp)); last = i
    print(f"  {name} ({freq:g} Hz, amplitude {amp:.0f}): {len(runs)} stretch{'es' if len(runs) != 1 else ''} of tone; "
          f"inside them {len(steps)} phase step{'s' if len(steps) != 1 else ''}, {len(dips)} dip{'s' if len(dips) != 1 else ''}, "
          f"{len(dmg)} damaged sample{'s' if len(dmg) != 1 else ''}; drift {drift / max(1, nd) * 100:+.5f} rad/s")
    for k, (s0, s1) in enumerate(runs):
        gap = f", then {(runs[k + 1][0] - s1) / a.rate:.2f} s of silence" if k + 1 < len(runs) else ""
        print(f"    tone {s0 / a.rate:9.2f} s to {s1 / a.rate:9.2f} s ({(s1 - s0) / a.rate:.2f} s){gap}")
    for b0, b1, d in steps:
        print(f"    phase step at {b0 / a.rate:9.2f} s: {d:+.2f} rad (a frame lost, doubled or out of place)")
    for b0, b1, v in dips:
        print(f"    dip {b0 / a.rate:9.2f} s to {b1 / a.rate:.2f} s: down to {v / amp:.2f} of the amplitude")
    for i, v in dmg:
        print(f"    damage at {i / a.rate:9.3f} s (residual {v:.2f} of the amplitude)")


if a.breaks:
    fl, fr = (float(x) for x in a.breaks.split(','))
    print(f"breaks in steady tones, a residual over {a.threshold:g} of the amplitude:")
    breaks(left, fl, 'left')
    breaks(right, fr, 'right')
elif a.start is not None:
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
