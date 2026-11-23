#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""mpg1_compare.py - the card's MPEG-1 pictures and sound against FFmpeg's (tools/mpg1_test.sh).

    mpg1_compare.py NAME WIDTH HEIGHT card.yuv ffmpeg.yuv card.pcm ffmpeg.pcm

The pictures (planar 4:2:0) must be as many as FFmpeg's and each within the IDCT's rounding of FFmpeg's:
at least 40 dB PSNR on every picture (MPEG-1 lets decoders' inverse DCTs differ slightly, and FFmpeg's is not
pl_mpeg's). The sound (16-bit big-endian stereo) must be as long as FFmpeg's to within a frame (1152
samples) and match it, once scaled by the gain that fits best (pl_mpeg's synthesis comes out 0.2 to 0.7 dB
quieter than FFmpeg's), to at least 40 dB of signal against difference, the gain within 1 dB. Standard library
only. Written on 23 November 2026 (docs/08 item 113).
"""
import math, struct, sys

name, w, h, a_yuv, b_yuv, a_pcm, b_pcm = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), *sys.argv[4:8]
fs = w * h * 3 // 2
a, b = open(a_yuv, 'rb').read(), open(b_yuv, 'rb').read()
ok = True
if len(a) != len(b):
    print(f'mpg1_test: {name}: {len(a) // fs} pictures against FFmpeg\'s {len(b) // fs}')
    ok = False
n = min(len(a), len(b)) // fs
worst_psnr, worst_at, worst_diff = 99.0, -1, 0
for k in range(n):
    x, y = a[k * fs:(k + 1) * fs], b[k * fs:(k + 1) * fs]
    if x == y:
        continue
    se = sum((p - q) * (p - q) for p, q in zip(x, y))
    psnr = 99.0 if not se else 10 * math.log10(255 * 255 * fs / se)
    d = max(abs(p - q) for p, q in zip(x, y))
    if psnr < worst_psnr:
        worst_psnr, worst_at = psnr, k
    worst_diff = max(worst_diff, d)
if worst_psnr < 40:
    ok = False
pa, pb = open(a_pcm, 'rb').read(), open(b_pcm, 'rb').read()
snd = ''
if pa or pb:
    na, nb = len(pa) // 4, len(pb) // 4
    m = min(na, nb) * 2
    sa = struct.unpack(f'>{m}h', pa[:m * 2]); sb = struct.unpack(f'>{m}h', pb[:m * 2])
    sig = sum(v * v for v in sb) or 1
    gain = sum(p * q for p, q in zip(sa, sb)) / (sum(p * p for p in sa) or 1)
    err = sum((p * gain - q) ** 2 for p, q in zip(sa, sb))
    snr = 99.0 if not err else 10 * math.log10(sig / err)
    gdb = 20 * math.log10(gain) if gain > 0 else -99.0
    snd = f'; sound {na} frames against FFmpeg\'s {nb}, {snr:.1f} dB apart once {-gdb:+.2f} dB of gain is allowed for'
    if abs(na - nb) > 1152 or snr < 40 or abs(gdb) > 1:
        ok = False
pic = 'identical' if worst_at < 0 else f'worst {worst_psnr:.1f} dB PSNR (picture {worst_at}), at most {worst_diff} apart'
print(f'mpg1_test: {name}: {n} pictures against FFmpeg\'s decoding, {pic}{snd}' + ('' if ok else ': FAILED'))
sys.exit(0 if ok else 1)
