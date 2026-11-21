#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""mksounds.py - PiPlay's test sounds and three short pieces of music, made here, from nothing.

    tools/mksounds.py OUTDIR [--no-mp3] [--quick]

PiPlay (mac/piplay.c) plays MP3, AIFF and WAV files through the sound card's stream channel, decoded
and resampled on the Pi (psmac_audio.c, docs/09 "PiPlay"). Testing it needs files whose content is
known exactly, and the user wanted music to try as well; nothing is downloaded and nothing is anyone's
recording. Written on 21 November 2026. Standard library only, like the rest of tools/; the MP3 step
runs `lame` (Debian's lame package) or, without it, ffmpeg's libmp3lame encoder.

The test signals (44,100 Hz, 16-bit stereo), each followed by half a second of silence:
  LR check   440 Hz on the left alone for 2 s, 660 Hz on the right alone for 2 s, 880 Hz on both for
             2 s, with half a second of silence between them: the channels' order and separation, the
             pitch (speed) and the durations, which tools/audiocheck.py measures.
  Sweep      a sine gliding from 100 Hz to 10 kHz in 10 s (exponentially: at t seconds it is
             100 * 100^(t/10) Hz), then 1 kHz for 3 s: the resampler across the band.
  as AIFF, WAV and MP3 (192 kbit/s), the LR check also as AIFF-C 'sowt' (little-endian), and the sweep
  as an 8-bit unsigned mono WAV at 22,050 Hz: the PCM layouts PiPlay hands the Pi.

The music, public-domain compositions played by a small additive synthesizer (a few voices panned
left and right, plucked and blown envelopes, a light stereo echo):
  Minuet in G      the minuet of the Notebook for Anna Magdalena Bach (BWV Anh. 114, attributed today
                   to Christian Petzold), AABB, 44,100 Hz: MP3 at 192 kbit/s and the AIFF
  Ode to Joy       the theme of the last movement of Beethoven's Ninth, three times round, 32,000 Hz:
                   MP3 at 160 kbit/s (MPEG-1 at 32 kHz)
  The Entertainer  the first strain of Scott Joplin's rag (1902), four times, 22,050 Hz: MP3 at
                   128 kbit/s (MPEG-2, the half rates)
Each MP3 carries an ID3v2 tag with the title and the artist "Aurora68 test", which PiPlay shows.
The notes were written down here from the pieces as they are generally known; any slip is ours.

--quick renders the music at a quarter of its length (for a fast check of the script itself).

--radio makes only the stream for the radio test (tools/radiotest.py, docs/09 "Internet radio"):
  Radio tone       1000 Hz on the left and 1500 Hz on the right, steady, for 5 minutes at 44,100 Hz,
                   MP3 at 128 kbit/s like SomaFM's streams. Ten milliseconds (441 samples) hold exactly
                   10 and 15 periods, so the signal is one block repeated and is made in a moment; a
                   steady sine shows every lost, doubled or damaged frame as a break in its phase
                   (tools/audiocheck.py --breaks 1000,1500).
"""
import argparse, array, math, os, shutil, struct, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('outdir')
ap.add_argument('--no-mp3', action='store_true')
ap.add_argument('--quick', action='store_true')
ap.add_argument('--radio', action='store_true', help='only the radio test stream (Radio tone.mp3)')
a = ap.parse_args()
os.makedirs(a.outdir, exist_ok=True)
ARTIST = 'Aurora68 test'


# ---- files --------------------------------------------------------------------------------------
def ieee_extended(rate):
    """the 80-bit IEEE extended number AIFF keeps its sample rate in (integer rates only)"""
    e = rate.bit_length() - 1
    return struct.pack('>HQ', 16383 + e, rate << (63 - e))


def to_frames(left, right, peak=None):
    """two lists of floats as 16-bit integers, scaled so the loudest sample is 0.89 of full scale"""
    if peak is None:
        peak = max(max(map(abs, left)), max(map(abs, right))) or 1.0
    k = 0.89 * 32767 / peak
    out = array.array('h', bytes(4 * len(left)))
    out[0::2] = array.array('h', (int(round(x * k)) for x in left))
    out[1::2] = array.array('h', (int(round(x * k)) for x in right))
    return out


def write_aiff(path, frames, rate, channels=2, compression=None):
    """AIFF (big-endian, signed) or AIFF-C with compression 'sowt' (little-endian, signed)"""
    data = array.array('h', frames)
    if (compression == b'sowt') != (sys.byteorder == 'little'):
        data.byteswap()
    data = data.tobytes()
    nframes = len(data) // (2 * channels)
    comm = struct.pack('>hIh', channels, nframes, 16) + ieee_extended(rate)
    if compression:
        name = b'Signed little-endian'          # a Pascal string, padded to an even length
        comm += compression + bytes([len(name)]) + name + (b'' if len(name) % 2 else b'\0')
    chunks = (b'FVER' + struct.pack('>II', 4, 0xA2805140) if compression else b'')
    chunks += b'COMM' + struct.pack('>I', len(comm)) + comm
    chunks += b'SSND' + struct.pack('>III', len(data) + 8, 0, 0) + data
    with open(path, 'wb') as f:
        f.write(b'FORM' + struct.pack('>I', len(chunks) + 4) + (b'AIFC' if compression else b'AIFF') + chunks)


def write_wav(path, frames, rate, channels=2, bits=16):
    if bits == 16:
        data = array.array('h', frames)
        if sys.byteorder == 'big':
            data.byteswap()
        data = data.tobytes()
    else:                                    # 8 bits: unsigned, 128 the middle
        data = bytes((v >> 8) + 128 for v in frames)
    fmt = struct.pack('<HHIIHH', 1, channels, rate, rate * channels * bits // 8, channels * bits // 8, bits)
    body = b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(data)) + data
    if len(data) % 2:
        body += b'\0'
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', len(body)) + body)


def mp3(wav, out, kbps, title):
    """an MP3 with an ID3v2 tag (title, artist), by lame or ffmpeg"""
    if a.no_mp3:
        return
    if shutil.which('lame'):
        cmd = ['lame', '--quiet', '-b', str(kbps), '--add-id3v2', '--tt', title, '--ta', ARTIST,
               '--tl', 'PiPlay test sounds', '--ty', '2026', wav, out]
    elif shutil.which('ffmpeg'):
        cmd = ['ffmpeg', '-loglevel', 'error', '-y', '-i', wav, '-codec:a', 'libmp3lame', '-b:a', f'{kbps}k',
               '-id3v2_version', '3', '-metadata', f'title={title}', '-metadata', f'artist={ARTIST}', out]
    else:
        sys.exit('mksounds: neither lame nor ffmpeg is installed: no MP3 files (--no-mp3 makes the rest)')
    subprocess.run(cmd, check=True)
    print(f'  {out}: {os.path.getsize(out)} bytes, {kbps} kbit/s ({cmd[0]})')


def save(name, left, right, rate, kbps, title, aiff=False, wav_keep=False):
    frames = to_frames(left, right)
    base = os.path.join(a.outdir, name)
    write_wav(base + '.wav', frames, rate)
    if aiff:
        write_aiff(base + '.aiff', frames, rate)
    print(f'{name}: {len(left) / rate:.1f} s at {rate} Hz')
    mp3(base + '.wav', base + '.mp3', kbps, title)
    if not wav_keep:
        os.remove(base + '.wav')
    return frames


# ---- the test signals ---------------------------------------------------------------------------
def tone(freq, secs, rate, amp=0.5):
    n, fade = int(secs * rate), int(0.01 * rate)
    return [amp * math.sin(2 * math.pi * freq * i / rate) * min(1.0, i / fade, (n - 1 - i) / fade) for i in range(n)]


def silence(secs, rate):
    return [0.0] * int(secs * rate)


if a.radio:
    blk = array.array('h')
    for i in range(441):
        blk.append(int(round(0.4 * 32767 * math.sin(2 * math.pi * 1000 * i / 44100))))
        blk.append(int(round(0.4 * 32767 * math.sin(2 * math.pi * 1500 * i / 44100))))
    if sys.byteorder == 'big':
        blk.byteswap()
    secs = 30 if a.quick else 300
    wav = os.path.join(a.outdir, 'Radio tone.wav')
    data = blk.tobytes() * (secs * 100)
    with open(wav, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 2, 44100, 176400, 4, 16)
                + b'data' + struct.pack('<I', len(data)) + data)
    print(f'Radio tone: {secs} s at 44100 Hz')
    mp3(wav, os.path.join(a.outdir, 'Radio tone.mp3'), 128, 'Radio tone')
    os.remove(wav)
    sys.exit(0)

R = 44100
l, r = [], []
for fl, fr in ((440, None), (None, 660), (880, 880)):
    t = tone(fl or fr, 2.0, R)
    l += t if fl else [0.0] * len(t)
    r += t if fr else [0.0] * len(t)
    l += silence(0.5, R); r += silence(0.5, R)
fr_lr = save('LR check', l, r, R, 192, 'Left, right, both', aiff=True, wav_keep=True)
write_aiff(os.path.join(a.outdir, 'LR check sowt.aifc'), fr_lr, R, compression=b'sowt')

n, sw, ph = 10 * R, [], 0.0
for i in range(n):
    f = 100.0 * 100.0 ** (i / n)
    ph += 2 * math.pi * f / R
    sw.append(0.5 * math.sin(ph) * min(1.0, i / 441, (n - 1 - i) / 441))
sw = silence(0.5, R) + sw + silence(0.5, R) + tone(1000, 3.0, R) + silence(0.5, R)
save('Sweep', sw, sw, R, 192, 'Sweep 100 Hz to 10 kHz', aiff=True, wav_keep=True)
# the same, 8-bit unsigned mono at 22,050 Hz (every other sample: the sweep's top is 10 kHz, under 11,025)
mono8 = to_frames(sw[::2], sw[::2])[0::2]
write_wav(os.path.join(a.outdir, 'Sweep 8-bit mono.wav'), mono8, 22050, channels=1, bits=8)
print('Sweep 8-bit mono.wav: 22050 Hz, 8 bits, one channel')


# ---- the synthesizer ----------------------------------------------------------------------------
NOTE = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}


def freq(name):
    """'F#4' -> Hz (A4 = 440)"""
    k, i = NOTE[name[0]], 1
    while name[i] in '#b':
        k += 1 if name[i] == '#' else -1
        i += 1
    return 440.0 * 2 ** ((k + 12 * (int(name[i:]) + 1) - 69) / 12)


def parse(text, unit=1.0):
    """'D5:1 G4:.5 R:1 [C4 E4]:2' -> [(start beat, [names], beats)]"""
    out, t = [], 0.0
    for tok in text.split():
        name, _, dur = tok.rpartition(':')
        d = float(dur) * unit
        if name != 'R':
            out.append((t, name.strip('[]').split('+'), d))
        t += d
    return out, t


TABLE = 2048


def table(harmonics):
    w = [sum(a * math.sin(2 * math.pi * h * i / TABLE) for h, a in harmonics) for i in range(TABLE)]
    m = max(map(abs, w))
    return [x / m for x in w]


SINE = table([(1, 1)])
# timbre: (bright table, dull table, how fast bright turns to dull (s), attack (s), decay time (s),
#          sustain level, release (s), vibrato depth)
TIMBRES = {
    'pluck': (table([(1, 1), (2, .6), (3, .45), (4, .3), (5, .2), (6, .12), (8, .06)]), table([(1, 1), (2, .25), (3, .08)]),
              0.12, 0.004, 0.9, 0.0, 0.08, 0.0),
    'harpsi': (table([(1, 1), (2, .8), (3, .6), (4, .5), (5, .35), (6, .3), (7, .2), (9, .1)]), table([(1, 1), (2, .4), (3, .2), (4, .1)]),
               0.2, 0.002, 1.4, 0.0, 0.06, 0.0),
    'bass': (table([(1, 1), (2, .5), (3, .25), (4, .1)]), table([(1, 1), (2, .2)]), 0.25, 0.006, 1.2, 0.0, 0.07, 0.0),
    'horn': (table([(1, 1), (2, .45), (3, .3), (4, .15), (5, .08)]), table([(1, 1), (2, .35), (3, .18)]), 0.5, 0.04, 0.4, 0.7, 0.12, 0.004),
    'strings': (table([(1, 1), (2, .5), (3, .33), (4, .25), (5, .2), (6, .14)]), table([(1, 1), (2, .4), (3, .2)]), 1.0, 0.12, 1.0, 0.8, 0.25, 0.003),
}


class Mix:
    def __init__(self, secs, rate):
        self.rate = rate
        self.l = [0.0] * int(secs * rate + rate)
        self.r = [0.0] * len(self.l)

    def note(self, start, f, secs, timbre, gain, pan):
        bright, dull, morph, att, dec, sus, rel, vib = TIMBRES[timbre]
        rate = self.rate
        n = int((secs + rel) * rate)
        i0 = int(start * rate)
        if i0 + n > len(self.l):
            n = len(self.l) - i0
        gl, gr = gain * math.cos((pan + 1) * math.pi / 4), gain * math.sin((pan + 1) * math.pi / 4)
        inc, ph = f * TABLE / rate, 0.0
        na, nhold = max(1, int(att * rate)), int(secs * rate)
        kd = math.exp(-1.0 / (dec * rate))            # the decay towards the sustain level, per sample
        km = math.exp(-1.0 / (morph * rate))          # bright to dull
        kr = math.exp(-1.0 / (rel * rate / 4))        # the release
        env, w = 0.0, 1.0
        vinc, vph = 5.5 * TABLE / rate, 0.0
        L, Rr = self.l, self.r
        for i in range(n):
            if i < na:
                env = i / na
            elif i < nhold:
                env = sus + (env - sus) * kd
            else:
                env *= kr
            k = int(ph) & (TABLE - 1)
            s = env * (w * bright[k] + (1.0 - w) * dull[k])
            w *= km
            L[i0 + i] += s * gl
            Rr[i0 + i] += s * gr
            if vib and i > na * 3:
                vph += vinc                           # a slow vibrato once the note has started
                ph += inc * (1.0 + vib * SINE[int(vph) & (TABLE - 1)])
            else:
                ph += inc

    def part(self, text, bpm, timbre, gain, pan, offset=0.0, transpose=0, legato=0.92):
        notes, beats = parse(text)
        spb = 60.0 / bpm
        for t, names, d in notes:
            for nm in names:
                self.note(offset + t * spb, freq(nm) * 2 ** (transpose / 12), d * spb * legato, timbre, gain, pan)
        return beats * spb

    def echo(self):
        """a light stereo echo: each side's sound comes back on the other, 23 and 31 ms later, fading"""
        dl, dr = int(0.023 * self.rate), int(0.031 * self.rate)
        L, Rr = self.l, self.r
        for i in range(max(dl, dr), len(L)):
            L[i] += 0.22 * Rr[i - dr]
            Rr[i] += 0.22 * L[i - dl]

    def end(self, secs):
        n = int(secs * self.rate)
        return self.l[:n], self.r[:n]


def bars(*b):
    return ' '.join(b)


# ---- Minuet in G (BWV Anh. 114), 3/4 ------------------------------------------------------------
A_TOP = ['D5:1 G4:.5 A4:.5 B4:.5 C5:.5', 'D5:1 G4:1 G4:1', 'E5:1 C5:.5 D5:.5 E5:.5 F#5:.5', 'G5:1 G4:1 G4:1',
         'C5:1 D5:.5 C5:.5 B4:.5 A4:.5', 'B4:1 C5:.5 B4:.5 A4:.5 G4:.5', 'F#4:1 G4:.5 A4:.5 B4:.5 G4:.5', 'A4:3']
MIN_A = A_TOP + A_TOP[:6] + ['A4:1 B4:.5 A4:.5 G4:.5 F#4:.5', 'G4:3']
MIN_B = ['B5:1 G5:.5 A5:.5 B5:.5 G5:.5', 'A5:1 D5:.5 E5:.5 F#5:.5 D5:.5', 'G5:1 E5:.5 F#5:.5 G5:.5 D5:.5',
         'C#5:1 B4:.5 C#5:.5 A4:1', 'A4:.5 B4:.5 C#5:.5 D5:.5 E5:.5 F#5:.5', 'G5:1 F#5:1 E5:1', 'F#5:1 A4:1 C#5:1', 'D5:3',
         'D5:1 G4:.5 F#4:.5 G4:1', 'E5:1 G4:.5 F#4:.5 G4:1', 'D5:1 C5:1 B4:1', 'A4:.5 G4:.5 F#4:.5 G4:.5 A4:1',
         'D4:.5 E4:.5 F#4:.5 G4:.5 A4:.5 B4:.5', 'C5:1 B4:1 A4:1', 'B4:.5 D5:.5 G4:1 F#4:1', 'G4:3']
MIN_A_BASS = ['G3+B3+D4:2 A3:1', 'B3:3', 'C4:3', 'B3:3', 'A3:3', 'G3:3', 'D4:1 B3:1 G3:1', 'D4:1 D3:.5 C4:.5 B3:.5 A3:.5',
              'B3:2 A3:1', 'G3:1 B3:1 G3:1', 'C4:3', 'B3:.5 C4:.5 B3:.5 A3:.5 G3:1', 'A3:2 F#3:1', 'G3:2 B3:1',
              'C4:1 D4:1 D3:1', 'G3:2 G2:1']
MIN_B_BASS = ['G3:3', 'F#3:3', 'E3:2 G3:1', 'A3:2 A2:1', 'A3:3', 'B3:1 D4:1 C#4:1', 'D4:1 F#3:1 A3:1', 'D4:1 D3:1 C4:1',
              'B3:3', 'C4:3', 'B3:1 A3:1 G3:1', 'D4:3', 'D3:3', 'E3:1 G3:1 F#3:1', 'G3:1 B2:1 D3:1', 'G3:1 D3:1 G2:1']


def minuet(rate, quick):
    bpm, beat = 112.0, 60.0 / 112.0
    top = bars(*MIN_A, *MIN_A, *MIN_B, *MIN_B)
    bot = bars(*MIN_A_BASS, *MIN_A_BASS, *MIN_B_BASS, *MIN_B_BASS)
    if quick:
        top, bot = bars(*MIN_A[:8]), bars(*MIN_A_BASS[:8])
    m = Mix(200 if not quick else 20, rate)
    secs = m.part(top, bpm, 'harpsi', 0.30, -0.45)
    m.part(bot, bpm, 'pluck', 0.30, 0.45)
    if not quick:                       # the repeats: the tune an octave up, quietly, on the right
        m.part(bars(*MIN_A), bpm, 'pluck', 0.10, 0.25, offset=16 * 3 * beat, transpose=12)
        m.part(bars(*MIN_B), bpm, 'pluck', 0.10, 0.25, offset=48 * 3 * beat, transpose=12)
    m.echo()
    return m.end(secs + 1.5)


# ---- Ode to Joy (Beethoven, Symphony No. 9), 4/4, in D -------------------------------------------
ODE = ['F#4:1 F#4:1 G4:1 A4:1', 'A4:1 G4:1 F#4:1 E4:1', 'D4:1 D4:1 E4:1 F#4:1', 'F#4:1.5 E4:.5 E4:2',
       'F#4:1 F#4:1 G4:1 A4:1', 'A4:1 G4:1 F#4:1 E4:1', 'D4:1 D4:1 E4:1 F#4:1', 'E4:1.5 D4:.5 D4:2',
       'E4:1 E4:1 F#4:1 D4:1', 'E4:1 F#4:.5 G4:.5 F#4:1 D4:1', 'E4:1 F#4:.5 G4:.5 F#4:1 E4:1', 'D4:1 E4:1 A3:2',
       'F#4:1 F#4:1 G4:1 A4:1', 'A4:1 G4:1 F#4:1 E4:1', 'D4:1 D4:1 E4:1 F#4:1', 'E4:1.5 D4:.5 D4:2']
ODE_LOW = ['D4:1 D4:1 E4:1 F#4:1', 'F#4:1 E4:1 D4:1 C#4:1', 'A3:1 A3:1 C#4:1 D4:1', 'D4:1.5 C#4:.5 C#4:2',
           'D4:1 D4:1 E4:1 F#4:1', 'F#4:1 E4:1 D4:1 C#4:1', 'A3:1 A3:1 C#4:1 D4:1', 'C#4:1.5 A3:.5 A3:2',
           'C#4:1 C#4:1 D4:1 A3:1', 'C#4:1 D4:.5 E4:.5 D4:1 A3:1', 'C#4:1 D4:.5 E4:.5 D4:1 C#4:1', 'A3:1 C#4:1 E3:2',
           'D4:1 D4:1 E4:1 F#4:1', 'F#4:1 E4:1 D4:1 C#4:1', 'A3:1 A3:1 C#4:1 D4:1', 'C#4:1.5 A3:.5 A3:2']
ODE_BASS = ['D3:2 A2:2', 'A2:2 E3:2', 'D3:2 F#3:2', 'A2:2 A2:2', 'D3:2 A2:2', 'A2:2 E3:2', 'D3:2 F#3:2', 'A2:2 D3:2',
            'A2:2 A2:2', 'A2:2 D3:2', 'A2:2 D3:2', 'D3:2 A2:2', 'D3:2 A2:2', 'A2:2 E3:2', 'D3:2 F#3:2', 'A2:2 D3:2']
ODE_PAD = ['A3+D4:4', 'C#4+E4:4', 'A3+D4:4', 'C#4+E4:4', 'A3+D4:4', 'C#4+E4:4', 'A3+D4:4', 'C#4+E4:2 A3+D4:2',
           'C#4+E4:4', 'A3+D4:4', 'C#4+E4:4', 'A3+D4:2 C#4+E4:2', 'A3+D4:4', 'C#4+E4:4', 'A3+D4:4', 'C#4+E4:2 A3+D4:2']


def ode(rate, quick):
    bpm = 108.0
    rounds = 1 if quick else 3
    length = 16 * 4 * 60.0 / bpm
    m = Mix(rounds * length + 4, rate)
    for k in range(rounds):
        off = k * length
        m.part(bars(*ODE), bpm, 'horn', 0.30, -0.35, offset=off, transpose=12 if k == 1 else 0)
        m.part(bars(*ODE_BASS), bpm, 'bass', 0.32, 0.45, offset=off)
        m.part(bars(*ODE_PAD), bpm, 'strings', 0.07, 0.0, offset=off, legato=1.0)
        if k >= 1:
            m.part(bars(*ODE_LOW), bpm, 'horn', 0.20, 0.5, offset=off, transpose=12 if k == 1 else 0)
    m.part('D3+A3+D4+F#4:4', bpm, 'strings', 0.12, 0.0, offset=rounds * length)
    m.echo()
    return m.end(rounds * length + 3.5)


# ---- The Entertainer (Scott Joplin, 1902), the first strain, 2/4, in C; a sixteenth is .25 -------
ENT_PICKUP = 'D4:.25 D#4:.25'
ENT = ['E4:.25 C5:.5 E4:.25 C5:.5 E4:.25 C5:.25', 'C5:1.25 C5:.25 D5:.25 D#5:.25', 'E5:.25 C5:.25 D5:.25 E5:.5 B4:.25 D5:.5',
       'C5:1.5 D4:.25 D#4:.25',
       'E4:.25 C5:.5 E4:.25 C5:.5 E4:.25 C5:.25', 'C5:1.5 A4:.25 G4:.25', 'F#4:.25 A4:.25 C5:.25 E5:.5 D5:.25 C5:.25 A4:.25',
       'D5:1.5 D4:.25 D#4:.25',
       'E4:.25 C5:.5 E4:.25 C5:.5 E4:.25 C5:.25', 'C5:1.25 C5:.25 D5:.25 D#5:.25', 'E5:.25 C5:.25 D5:.25 E5:.5 B4:.25 D5:.5',
       'C5:1.5 C5:.25 D5:.25',
       'E5:.25 C5:.25 D5:.25 E5:.5 C5:.25 D5:.25 C5:.25', 'E5:.25 C5:.25 D5:.25 E5:.5 C5:.25 D5:.25 C5:.25',
       'E5:.25 C5:.25 D5:.25 E5:.5 B4:.25 D5:.5', 'C5:1.5 D4:.25 D#4:.25']
# the stride left hand: the bass on the beat, the chord on the off-beat
ENT_CHORDS = ['C', 'C', 'G7', 'C', 'C', 'C', 'D7', 'G7', 'C', 'C', 'G7', 'C', 'C', 'C7', 'G7', 'C']
STRIDE = {'C': ('C3', 'G2', 'E3+G3+C4'), 'G7': ('G2', 'D3', 'F3+G3+B3'), 'D7': ('D3', 'A2', 'F#3+A3+C4'),
          'C7': ('C3', 'G2', 'E3+A#3+C4')}


def entertainer(rate, quick):
    bpm = 76.0
    strains = 1 if quick else 4
    spb = 60.0 / bpm
    m = Mix(strains * 32 * spb + 6, rate)
    pick = 0.5 * spb
    m.part(ENT_PICKUP, bpm, 'pluck', 0.30, -0.4)
    for k in range(strains):
        off = pick + k * 32 * spb
        top = bars(*ENT)
        if k == strains - 1:
            top = top[:top.rfind('C5:1.5')] + 'C5:2'
        m.part(top, bpm, 'pluck', 0.30, -0.4, offset=off, transpose=12 if k == 2 else 0)
        if k in (1, 3):
            m.part(top, bpm, 'harpsi', 0.10, 0.15, offset=off, transpose=12)
        left = ' '.join(f'{STRIDE[c][0]}:.5 {STRIDE[c][2]}:.5 {STRIDE[c][1]}:.5 {STRIDE[c][2]}:.5' for c in ENT_CHORDS)
        if k == strains - 1:
            left = left[:left.rfind(STRIDE['C'][0])] + 'C3+E3+G3+C4:2'
        m.part(left, bpm, 'pluck', 0.22, 0.45, offset=off, legato=0.7)
    m.echo()
    return m.end(pick + strains * 32 * spb + 2.0)


print('music (a minute or two of Python per piece)')
l, r = minuet(44100, a.quick)
save('Minuet in G', l, r, 44100, 192, 'Minuet in G (BWV Anh. 114)', aiff=True)
l, r = ode(32000, a.quick)
save('Ode to Joy', l, r, 32000, 160, 'Ode to Joy (Symphony No. 9)')
l, r = entertainer(22050, a.quick)
save('The Entertainer', l, r, 22050, 128, 'The Entertainer')
print('done:', ', '.join(sorted(os.listdir(a.outdir))))
