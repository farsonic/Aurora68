#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""aurora68_music_server.py - a folder of music as internet radio stations, over IPv4 and IPv6.

22 November 2026. The owner has a folder of MP3s, organised in folders (artist, album, or however they
like), on a NAS, and Aurora68 Play already plays internet radio: plain-http MP3 streams with Shoutcast
and Icecast's "ICY" song titles, over IPv4 or IPv6 (mac/piplay/radio.c). So every folder becomes a
station: its MP3s, and those of the folders inside it, one after another as one continuous stream, the
way a radio station sends them. Anything that plays internet radio plays these too (VLC, iTunes/Music,
a browser, foobar2000, a Squeezebox), and a browser gets a page listing the folders.

    GET /                         a web page: the folders, each with its station and its files
    GET /radio/PATH/              the station for folder PATH ("/radio/" is the whole library), as
                                  audio/mpeg; ?shuffle=1 plays it in random order; ?loop=0 ends it after
                                  one pass. Song titles as ICY metadata when the player asks for them
    GET /s/ID                     the same station by a short name (eight letters and digits), which is
                                  what the stations file uses: Aurora68 Play's addresses are 160 bytes at most
    GET /file/PATH.mp3            one file as it is (with Range, for players that seek)
    GET /stations.txt             the library as an "Aurora68 Play Stations" file (Mac Roman, CR line
                                  ends, [Group/Sub] headings), within Aurora68 Play's limits; ?host=ADDRESS
                                  writes that address into it instead of the one the request used
    GET /playlist.m3u?dir=PATH    an M3U of the folder's files, for players that want a playlist
    GET /api/list?dir=PATH        JSON: the folder's folders and files

How a station is sent. Each file's MP3 frames are found (its ID3v2 tag at the front, ID3v1 or APE tags
at the end and the Xing/Info header frame are left out: inside a stream they are noise to a decoder),
and sent at the rate they play, after the first few seconds sent at once so that the player's buffer
fills quickly. The frames' own headers give their length in time, so files at any bit rate or sample
rate, constant or variable, follow each other. Aurora68 Play's decoder (minimp3, on the sound card)
takes each frame's rate from the frame.

Other formats. The owner's first library turned out not to be MP3 (22 November 2026: the server found
nothing in it), as libraries from today's download services usually are not: FLAC, M4A (AAC or ALAC),
Ogg, Opus, WAV, AIFF. Where ffmpeg is installed (the Docker image has it) those files are converted to
MP3 as they are sent, at MUSIC_BITRATE kbit/s (192), 44.1 kHz stereo: a few per cent of one core per
listener. Without ffmpeg only MP3s are offered, and the scan says how many other files it passed over.

Python's standard library only (and ffmpeg, if there); the folder is only read. Nothing outside it is
served: every path is resolved and must stay inside the folder, symbolic links included.

    python3 aurora68_music_server.py /path/to/music
    python3 aurora68_music_server.py /music --port 8000 --name "NAS Music"
"""
import argparse, hashlib, html, json, mimetypes, os, random, shutil, signal, socket, subprocess, sys, threading, \
    time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = '2'
METAINT = 16000                 # bytes of audio between song titles, as most Icecast servers send
BURST = 6.0                     # seconds of audio sent at once when a listener connects
RESCAN = 30.0                   # seconds a scan of the folder is trusted before it is made again
PLAY_MAX_STATIONS = 88          # Aurora68 Play holds 128 stations and 48 submenus (mac/piplay/stations.h),
PLAY_MAX_GROUPS = 40            # 40 of the stations its own; the stations file stays within the rest
MENU_ROWS = 18                  # a menu on the SE's 342 lines: the menu bar and 18 items of 16 lines
ARTIST_MIN = 6                  # a folder of at least this many "Artist - Album" folders is grouped by artist
CONVERTED = ('.flac', '.m4a', '.aac', '.alac', '.ogg', '.oga', '.opus', '.wav', '.aif', '.aiff', '.wma', '.ape', '.wv', '.mp2')


def log(*a):
    print(time.strftime('%Y-%m-%d %H:%M:%S'), *a, flush=True)


# ---- MP3 frames ---------------------------------------------------------------------------------
# MPEG audio frame headers (ISO 11172-3, 13818-3, and MPEG 2.5): version, layer, bit rate, sample rate.
BITRATES = {  # (version is MPEG-1, layer) -> kbit/s by index; MPEG-2/2.5 share theirs
    (1, 1): [0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448],
    (1, 2): [0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384],
    (1, 3): [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],
    (2, 1): [0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256],
    (2, 2): [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
}
BITRATES[(2, 3)] = BITRATES[(2, 2)]
RATES = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}   # by the version bits


def frame_header(b, i):
    """(frame length in bytes, samples, sample rate) of a frame header at b[i], or None"""
    if i + 4 > len(b) or b[i] != 0xFF or (b[i + 1] & 0xE0) != 0xE0:
        return None
    ver, layer = (b[i + 1] >> 3) & 3, (b[i + 1] >> 1) & 3
    bri, sri, pad = b[i + 2] >> 4, (b[i + 2] >> 2) & 3, (b[i + 2] >> 1) & 1
    if ver == 1 or layer == 0 or bri in (0, 15) or sri == 3:
        return None                         # reserved values; "free format" (index 0) is not streamed
    lay = 4 - layer                         # the bits count down: 3 is layer I
    kbps = BITRATES[(1 if ver == 3 else 2, lay)][bri]
    rate = RATES[ver][sri]
    if lay == 1:
        return (12000 * kbps // rate + pad) * 4, 384, rate
    samples = 1152 if (lay == 2 or ver == 3) else 576
    return samples * 125 * kbps // rate + pad, samples, rate


def is_info_frame(b, i, n):
    """the Xing, Info or VBRI frame an encoder puts first: silence carrying the file's length"""
    return b'Xing' in b[i:i + min(n, 64)] or b'Info' in b[i:i + min(n, 64)] or b[i + 36:i + 40] == b'VBRI'


def audio_span(b):
    """where the audio is in a file's bytes: after an ID3v2 tag, before ID3v1 and APE tags"""
    start, end = 0, len(b)
    while b[start:start + 3] == b'ID3' and start + 10 <= end:     # some files have two
        size = (b[start + 6] << 21) | (b[start + 7] << 14) | (b[start + 8] << 7) | b[start + 9]
        start += 10 + size + (10 if b[start + 5] & 0x10 else 0)    # a v2.4 footer
    if end - start >= 128 and b[end - 128:end - 125] == b'TAG':
        end -= 128
    if end - start >= 32 and b[end - 32:end - 24] == b'APETAGEX':
        end -= 32 + int.from_bytes(b[end - 20:end - 16], 'little')
    return start, max(start, end)


def frames(b):
    """the file's MPEG audio frames as (offset, length, seconds): a frame counts once the next one
    follows it where its header says (a lone 0xFFE in a tag or picture is not a frame)"""
    start, end = audio_span(b)
    i, out, first = start, [], True
    while i + 4 <= end:
        h = frame_header(b, i)
        if h and i + h[0] <= end and (i + h[0] == end or frame_header(b, i + h[0]) or i + h[0] + 4 > end):
            if not (first and is_info_frame(b, i, h[0])):
                out.append((i, h[0], h[1] / h[2]))
            first = False
            i += h[0]
        else:
            i += 1                           # resynchronise a byte at a time
    return out


# ---- song titles -----------------------------------------------------------------------------------
def id3_text(data, enc):
    try:
        if enc == 0:
            t = data.decode('latin-1')
        elif enc == 1:
            t = data.decode('utf-16')
        elif enc == 2:
            t = data.decode('utf-16-be')
        else:
            t = data.decode('utf-8', 'replace')
    except UnicodeDecodeError:
        return ''
    return t.split('\x00')[0].strip()


def song_title(path):
    """"Artist - Title" from the file's ID3 tag, or its name without .mp3"""
    found = {}
    try:
        with open(path, 'rb') as f:
            head = f.read(10)
            if head[:3] == b'ID3' and len(head) == 10 and not head[5] & 0x80:   # not unsynchronised
                major = head[3]
                size = (head[6] << 21) | (head[7] << 14) | (head[8] << 7) | head[9]
                tag, i = f.read(min(size, 1 << 20)), 0
                idlen, hlen = (3, 6) if major == 2 else (4, 10)
                while i + hlen <= len(tag) and tag[i] != 0:
                    fid = tag[i:i + idlen].decode('latin-1')
                    if major == 2:
                        n = int.from_bytes(tag[i + 3:i + 6], 'big')
                    elif major == 4:
                        n = (tag[i + 4] << 21) | (tag[i + 5] << 14) | (tag[i + 6] << 7) | tag[i + 7]
                    else:
                        n = int.from_bytes(tag[i + 4:i + 8], 'big')
                    body = tag[i + hlen:i + hlen + n]
                    if fid in ('TIT2', 'TT2', 'TPE1', 'TP1') and body:
                        found[fid[:2] if major == 2 else fid] = id3_text(body[1:], body[0])
                    i += hlen + n
            title = found.get('TIT2') or found.get('TT')
            artist = found.get('TPE1') or found.get('TP')
            if not title:
                f.seek(-128, os.SEEK_END)
                v1 = f.read(128)
                if v1[:3] == b'TAG':
                    title = v1[3:33].split(b'\0')[0].decode('latin-1').strip()
                    artist = artist or v1[33:63].split(b'\0')[0].decode('latin-1').strip()
    except OSError:
        pass
    name = title or os.path.splitext(os.path.basename(path))[0]
    return f'{artist} - {name}' if artist and title else name


def tag_title(ffprobe, path):
    """"Artist - Title" of a file in another format, from its tags (ffprobe), or its name"""
    name = os.path.splitext(os.path.basename(path))[0]
    if not ffprobe:
        return name
    try:
        out = subprocess.run([ffprobe, '-v', 'error', '-show_entries', 'format_tags', '-of', 'json', path],
                             capture_output=True, text=True, timeout=20)
        tags = {k.lower(): v for k, v in (json.loads(out.stdout or '{}').get('format', {}).get('tags') or {}).items()}
    except (OSError, ValueError, subprocess.SubprocessError):
        return name
    title, artist = tags.get('title', '').strip(), tags.get('artist', '').strip()
    return f'{artist} - {title}' if artist and title else (title or name)


# ---- the library -------------------------------------------------------------------------------
def short_id(rel):
    return hashlib.sha1(rel.encode('utf-8', 'surrogateescape')).hexdigest()[:8]


def sort_key(name):
    """artists in the order record shops file them: "The Cure" under C"""
    n = name.casefold()
    return n[4:] if n.startswith('the ') and len(n) > 4 else n


def artist_of(folder):
    """"Weezer" of "Weezer - Pinkerton (Explicit)", or None"""
    a, sep, rest = folder.partition(' - ')
    return a.strip() if sep and a.strip() and rest.strip() else None


class Library:
    """the folder: its MP3s by folder, scanned again when the last scan is RESCAN seconds old.

    A station is a folder (its relative name, '' for the whole library) or an artist: (folder, name).
    Many music libraries keep their albums side by side as "Artist - Album" folders; a folder of at
    least ARTIST_MIN folders of which two in three are named so is also offered by artist, each artist
    one station playing all their albums."""
    def __init__(self, root, ffmpeg=None):
        self.root = os.path.realpath(root)
        self.ffmpeg = ffmpeg             # converts the formats in CONVERTED; None: MP3s only
        self.ffprobe = shutil.which('ffprobe') if ffmpeg else None
        self.lock = threading.Lock()
        self.when = 0.0
        self.dirs = {}               # relative folder ('' for the root) -> (folders in it, songs in it), sorted
        self.ids = {}                # short ID -> a station: a relative folder, or (folder, artist)
        self.artists = {}            # relative folder -> {artist: [its folders inside it]}, for those offered so

    def scan(self):
        with self.lock:
            if time.monotonic() - self.when < RESCAN:
                return
            dirs, ids, errors, passed = {}, {}, [], {}
            for top, subdirs, files in os.walk(self.root, onerror=errors.append, followlinks=False):
                rel = os.path.relpath(top, self.root)
                rel = '' if rel == '.' else rel.replace(os.sep, '/')
                subdirs[:] = sorted((d for d in subdirs if not d.startswith('.')), key=str.casefold)
                songs = []
                for f in files:
                    ext = os.path.splitext(f)[1].lower()
                    if f.startswith('.'):
                        continue
                    if self.playable(f):
                        songs.append(f)
                    elif ext in CONVERTED:
                        passed[ext] = passed.get(ext, 0) + 1
                dirs[rel] = (list(subdirs), sorted(songs, key=str.casefold))
                ids[short_id(rel)] = rel
            # folders with no MP3 anywhere inside them are no stations
            def has_music(rel):
                sub, mp3s = dirs[rel]
                return bool(mp3s) or any(has_music(f'{rel}/{s}' if rel else s) for s in sub if (f'{rel}/{s}' if rel else s) in dirs)
            keep = {r for r in dirs if has_music(r)} | {''}
            dirs.setdefault('', ([], []))
            self.dirs = {r: ([s for s in v[0] if (f'{r}/{s}' if r else s) in keep], v[1]) for r, v in dirs.items() if r in keep}
            self.ids = {k: r for k, r in ids.items() if r in keep}
            self.artists = {}
            for r, (sub, _) in self.dirs.items():
                named = [(artist_of(f), f) for f in sub]
                if len(sub) >= ARTIST_MIN and 3 * sum(1 for a, _ in named if a) >= 2 * len(sub):
                    by = {}
                    for a, f in named:
                        by.setdefault(a or f, []).append(f)
                    self.artists[r] = dict(sorted(by.items(), key=lambda kv: sort_key(kv[0])))
                    for a in by:
                        self.ids[short_id(r + '\0' + a)] = (r, a)
            self.when = time.monotonic()
            n = sum(len(v[1]) for v in self.dirs.values())
            kinds = {}
            for v in self.dirs.values():
                for f in v[1]:
                    e = os.path.splitext(f)[1].lower().lstrip('.').upper()
                    kinds[e] = kinds.get(e, 0) + 1
            what = ', '.join(f'{c} {k}' for k, c in sorted(kinds.items(), key=lambda kv: -kv[1])) or 'no music'
            log(f'library: {n} songs ({what}) in {len(self.dirs) - 1} folders under {self.root}')
            for e in errors[:5]:                       # what could not be read: permissions, usually
                log(f'library: cannot read {e.filename}: {e.strerror} (the server runs as user {os.getuid()}:{os.getgid()})')
            if len(errors) > 5:
                log(f'library: ... and {len(errors) - 5} more that cannot be read')
            if passed:
                log('library: passed over ' + ', '.join(f'{c} {e[1:].upper()}' for e, c in sorted(passed.items(), key=lambda kv: (-kv[1], kv[0]))) +
                    ' files: ffmpeg is not installed here to convert them (the Docker image has it)')
            if not n and not errors and not passed:
                log('library: no music files found: is the right folder mounted at ' + self.root + '?')

    def playable(self, name):
        ext = os.path.splitext(name)[1].lower()
        return ext == '.mp3' or (self.ffmpeg is not None and ext in CONVERTED)

    def station_id(self, key):
        return short_id(key[0] + '\0' + key[1]) if isinstance(key, tuple) else short_id(key)

    def items(self, rel):
        """what a folder offers as stations: its artists if it is grouped so, else its folders, as
        (name, station) pairs"""
        if rel in self.artists:
            return [(a, (rel, a)) for a in self.artists[rel]]
        return [(f, f'{rel}/{f}' if rel else f) for f in self.dirs.get(rel, ([], []))[0]]

    def real(self, rel):
        """the path of a relative name inside the folder, or None if it is not inside it"""
        p = os.path.realpath(os.path.join(self.root, rel))
        return p if p == self.root or p.startswith(self.root + os.sep) else None

    def folder(self, rel):
        self.scan()
        rel = rel.strip('/')
        return rel if rel in self.dirs else None

    def tracks(self, rel):
        """every song in the folder and the folders inside it, in order; an artist's: all their albums"""
        if isinstance(rel, tuple):
            folder, artist = rel
            return [t for f in self.artists.get(folder, {}).get(artist, [])
                    for t in self.tracks(f'{folder}/{f}' if folder else f)]
        sub, songs = self.dirs.get(rel, ([], []))
        out = [f'{rel}/{m}' if rel else m for m in songs]
        for s in sub:
            out += self.tracks(f'{rel}/{s}' if rel else s)
        return out


# ---- a station's stream ------------------------------------------------------------------------
class IcyWriter:
    """the stream to one listener: audio, with a song title block every METAINT bytes if it asked"""
    def __init__(self, wfile, meta):
        self.w, self.meta, self.left, self.title, self.sent_title = wfile, meta, METAINT, '', None

    def write(self, b):
        while b:
            if not self.meta:
                self.w.write(b)
                return
            n = min(len(b), self.left)
            self.w.write(b[:n])
            b, self.left = b[n:], self.left - n
            if self.left == 0:
                self.left = METAINT
                if self.title == self.sent_title:
                    self.w.write(b'\0')                       # the same title: an empty block
                else:
                    t = ("StreamTitle='" + self.title.replace("'", '’') + "';").encode('utf-8')[:4000]
                    t += b'\0' * (-len(t) % 16)
                    self.w.write(bytes([len(t) // 16]) + t)
                    self.sent_title = self.title


def mp3_chunks(b):
    """an MP3 file's frames, half a second at a time: (bytes, seconds)"""
    chunk, secs = [], 0.0
    for off, n, d in frames(b):
        chunk.append(b[off:off + n])
        secs += d
        if secs >= 0.5:
            yield b''.join(chunk), secs
            chunk, secs = [], 0.0
    if chunk:
        yield b''.join(chunk), secs


def converted_chunks(ffmpeg, path, kbps):
    """another format through ffmpeg into MP3, framed as it comes, half a second at a time"""
    p = subprocess.Popen([ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-i', path, '-map', '0:a:0',
                          '-c:a', 'libmp3lame', '-b:a', f'{kbps}k', '-ar', '44100', '-ac', '2',
                          '-write_xing', '0', '-id3v2_version', '0', '-f', 'mp3', 'pipe:1'],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    buf, chunk, secs = bytearray(), [], 0.0
    try:
        while True:
            more = p.stdout.read(32768)
            buf += more
            i = 0
            while i + 4 <= len(buf):
                h = frame_header(buf, i)
                if not h:
                    i += 1                     # not at a frame: resynchronise
                    continue
                if i + h[0] > len(buf):
                    break                      # the rest of this frame has not come yet
                chunk.append(bytes(buf[i:i + h[0]]))
                secs += h[1] / h[2]
                i += h[0]
                if secs >= 0.5:
                    yield b''.join(chunk), secs
                    chunk, secs = [], 0.0
            del buf[:i]
            if not more:
                break
        if chunk:
            yield b''.join(chunk), secs
    finally:
        p.kill()
        p.wait()


def stream_station(lib, rel, wfile, meta, shuffle, loop, who, kbps):
    tracks = lib.tracks(rel)
    if not tracks:
        return
    out = IcyWriter(wfile, meta)
    t0 = time.monotonic() - BURST         # the audio is due BURST seconds ahead of the clock
    due = 0.0                             # seconds of audio sent
    bad = set()                           # files found unplayable: said once, then passed over
    while True:
        order = [t for t in tracks if t not in bad]
        if not order:
            return                        # nothing in the station plays
        if shuffle:
            random.shuffle(order)
        for t in order:
            path = lib.real(t)
            if not path:
                bad.add(t)
                continue
            if t.lower().endswith('.mp3'):
                try:
                    with open(path, 'rb') as f:
                        source = mp3_chunks(f.read())
                except OSError as e:
                    log(f'{who}: cannot read {t}: {e.strerror}')
                    bad.add(t)
                    continue
                title = song_title(path)
            else:
                source = converted_chunks(lib.ffmpeg, path, kbps)
                title = tag_title(lib.ffprobe, path)
            sent = 0.0
            try:
                for data, secs in source:
                    if not sent:
                        out.title = title
                        log(f'{who}: {title}')
                    out.write(data)
                    sent += secs
                    due += secs
                    ahead = due - (time.monotonic() - t0)
                    if ahead > 0:
                        time.sleep(ahead)
            finally:
                source.close()            # a converting ffmpeg stops when the listener leaves
            if not sent:
                log(f'{who}: no audio in {t}, passed over')
                bad.add(t)
        if not loop:
            return
        tracks = lib.tracks(rel) or tracks          # a pass ends: anything added since joins in


# ---- the stations file for Aurora68 Play ---------------------------------------------------------
def mac_roman(s, n=60, line_start=True):
    s = ' '.join(s.split())[:n]
    if line_start:
        s = s.lstrip('#') or '?'                         # a line that starts with # is a note
    return s.encode('mac_roman', 'replace').replace(b'=', b'-').replace(b'[', b'(').replace(b']', b')')


def initial(name):
    """the letter a name is filed under, or '#' for one that starts with a digit or a sign"""
    c = next((c for c in sort_key(name).upper() if c.isalnum()), '#')
    return c if c.isalpha() else '#'


def menus(items):
    """a list cut into menus of at most MENU_ROWS, at letter boundaries where it can be: [(label, items)].
    A letter with more names than a menu holds is cut by its first two letters ("Sa-Sm")."""
    groups = []                                   # (letter, items), in order
    for it in items:
        k = initial(it[0])
        if groups and groups[-1][0] == k:
            groups[-1][1].append(it)
        else:
            groups.append((k, [it]))
    pieces = []                                   # (first label, last label, items)
    for k, its in groups:
        if len(its) <= MENU_ROWS:
            pieces.append((k, k, its))
            continue
        for i in range(0, len(its), MENU_ROWS):
            part = its[i:i + MENU_ROWS]
            two = lambda n: (sort_key(n)[:2].title() if k != '#' else '#')
            pieces.append((two(part[0][0]), two(part[-1][0]), part))
    out = []
    for first, last, its in pieces:
        if out and len(out[-1][2]) + len(its) <= MENU_ROWS and len(first) == 1 and len(out[-1][1]) == 1:
            out[-1] = (out[-1][0], last, out[-1][2] + its)
        else:
            out.append((first, last, list(its)))
    return [(f if f == l else f'{f}\u2013{l}', its) for f, l, its in out]


def stations_file(lib, base, name):
    """an "Aurora68 Play Stations" file. A submenu named after the library holds "Everything,
    shuffled" and the top-level folders; each top-level folder has a submenu of its folders (or its
    artists); a list longer than a menu on the SE's screen is cut into alphabetical submenus ("A-F").
    Deeper folders play inside their parents' stations. Within Aurora68 Play's limits, with a note
    where the library holds more than that."""
    lib.scan()
    top = mac_roman(name, 24)
    lines = [b'# Aurora68 Play Stations from ' + mac_roman(name) + b' (' + base.encode('ascii', 'replace') + b')',
             b'# Each folder is a station; its MP3s, and those of the folders inside it, play one after another.',
             b'# Put this file in the Preferences folder as "Aurora68 Play Stations", or add its lines to yours.',
             b'[' + top + b']']
    url = lambda key, shuffle: f'{base}/s/{lib.station_id(key)}' + ('?shuffle=1' if shuffle else '')
    entry = lambda label, key, shuffle=False: mac_roman(label) + b' = ' + url(key, shuffle).encode()
    lines.append(entry('Everything, shuffled', '', True))
    stations, groups = 1, 1
    root, here = lib.items(''), ''
    while len(root) == 1 and isinstance(root[0][1], str) and not lib.dirs[here][1]:
        here = root[0][1]                         # one folder holding everything ("Albums"): its contents
        root = lib.items(here)
    if len(root) <= MENU_ROWS - 1:
        for n, key in root:
            lines.append(entry(n, key))
        stations += len(root)
        sections = [(n, [(f'All of {n}, shuffled', key, True)] + [(m, k, False) for m, k in lib.items(key)])
                    for n, key in root if isinstance(key, str) and lib.items(key)]
    else:
        sections = [(None, [(n, k, False) for n, k in root])]
    for heading, items in sections:
        cut = menus([it for it in items if not it[2]])
        shuffled = [it for it in items if it[2]]
        if stations + len(items) > PLAY_MAX_STATIONS or groups + len(cut) > PLAY_MAX_GROUPS:
            lines.append(b"# (more folders than Aurora68 Play's menus hold: the rest play inside the stations above)")
            break
        for i, (rng, its) in enumerate(cut):
            label = mac_roman(heading, 20) if heading else b''
            if len(cut) > 1:
                label = (label + b' ' if label else b'') + mac_roman(rng, line_start=False)
            lines.append(b'[' + top + b'/' + label + b']')
            lines += [entry(n, k, sh) for n, k, sh in (shuffled if i == 0 else []) + its]
        stations += len(items)
        groups += len(cut)
    lines.append(b'[]')
    return b'\r'.join(lines) + b'\r'


# ---- the web server ------------------------------------------------------------------------------
PAGE = """<!DOCTYPE html><html><head><meta charset="utf-8"><title>{title}</title>
<style>body{{font:15px/1.5 system-ui,sans-serif;max-width:52em;margin:2em auto;padding:0 1em}}
a{{text-decoration:none}} li{{margin:.15em 0}} .s{{color:#666;font-size:90%}}</style></head><body>
<h1>{title}</h1><p>{crumbs}</p><p><a href="{station}">&#9654; Play this folder</a> &middot;
<a href="{station}?shuffle=1">shuffled</a> &middot; <a href="{m3u}">M3U</a> &middot;
<a href="/stations.txt">Aurora68 Play stations file</a></p>{body}
<p class="s">Aurora68 music server {version}. Station address for any internet radio player: <code>{full}</code></p></body></html>"""


def make_handler(lib, name, max_listeners, kbps):
    listeners = threading.BoundedSemaphore(max_listeners)

    class Handler(BaseHTTPRequestHandler):
        server_version = f'aurora68-music-server/{VERSION}'
        protocol_version = 'HTTP/1.0'

        def log_message(self, fmt, *args):
            pass

        def who(self):
            a = self.client_address[0]
            return a[7:] if a.startswith('::ffff:') else a          # an IPv4 listener on the IPv6 socket

        def base(self, host=None):
            """the server's address as the client wrote it (Host), or an address asked for without a port
            (/stations.txt?host=192.168.1.20, or an IPv6 address), for a Mac whose name service cannot look
            up the name a modern computer used"""
            port = self.server.server_address[1]
            if host and all(c.isalnum() or c in '.-:' for c in host.strip('[]')):
                host = host.strip('[]')
                return f'http://[{host}]:{port}' if ':' in host else f'http://{host}:{port}'
            return 'http://' + (self.headers.get('Host') or f'{socket.gethostname()}:{port}')

        def send(self, code, ctype, body, extra=()):
            self.send_response(code)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            for k, v in extra:
                self.send_header(k, v)
            self.end_headers()
            if self.command != 'HEAD':
                self.wfile.write(body)

        def fail(self, code, text):
            self.send(code, 'text/plain; charset=utf-8', (text + '\n').encode())

        def do_HEAD(self):
            self.do_GET()

        def do_GET(self):
            u = urllib.parse.urlsplit(self.path)
            q = dict(urllib.parse.parse_qsl(u.query))
            path = urllib.parse.unquote(u.path)
            try:
                if path == '/' or path.startswith('/browse/'):
                    return self.page(path[len('/browse/'):] if path != '/' else '')
                if path.startswith('/radio/'):
                    return self.station(lib.folder(path[len('/radio/'):]), q)
                if path.startswith('/s/'):
                    lib.scan()
                    return self.station(lib.ids.get(path[3:].strip('/')), q)
                if path.startswith('/file/'):
                    return self.file(path[len('/file/'):])
                if path == '/stations.txt':
                    return self.send(200, 'text/plain; charset=macintosh', stations_file(lib, self.base(q.get('host')), name),
                                     [('Content-Disposition', 'inline; filename="Aurora68 Play Stations"')])
                if path == '/playlist.m3u':
                    return self.m3u(lib.folder(q.get('dir', '')))
                if path == '/api/list':
                    rel = lib.folder(q.get('dir', ''))
                    if rel is None:
                        return self.fail(404, 'no such folder')
                    sub, mp3s = lib.dirs[rel]
                    out = {'folder': rel, 'station': f'/s/{short_id(rel)}', 'folders': sub, 'files': mp3s}
                    if rel in lib.artists:
                        out['artists'] = [{'name': a, 'station': f'/s/{lib.station_id((rel, a))}', 'folders': f}
                                          for a, f in lib.artists[rel].items()]
                    return self.send(200, 'application/json', json.dumps(out).encode())
                return self.fail(404, 'not here: try /')
            except (BrokenPipeError, ConnectionResetError):
                pass

        def station(self, rel, q):
            if rel is None:
                return self.fail(404, 'no such folder')
            if not listeners.acquire(blocking=False):
                return self.fail(503, f'{max_listeners} listeners already')
            who = self.who()
            title = rel[1] if isinstance(rel, tuple) else (rel.split('/')[-1] if rel else name)
            what = f'{rel[1]} (all their albums)' if isinstance(rel, tuple) else (rel or 'everything')
            meta = self.headers.get('Icy-MetaData') == '1'
            try:
                self.send_response(200)
                self.send_header('Content-Type', 'audio/mpeg')
                self.send_header('Cache-Control', 'no-cache, no-store')
                self.send_header('Connection', 'close')
                self.send_header('icy-name', title)
                self.send_header('icy-description', f'{name}: {what}')
                if meta:
                    self.send_header('icy-metaint', str(METAINT))
                self.end_headers()
                if self.command == 'HEAD':
                    return
                log(f'{who}: listening to {what}{" shuffled" if q.get("shuffle") == "1" else ""}')
                stream_station(lib, rel, self.wfile, meta, q.get('shuffle') == '1', q.get('loop', '1') != '0', who, kbps)
                log(f'{who}: {what} ended')
            except (BrokenPipeError, ConnectionResetError, OSError):
                log(f'{who}: left {what}')
            finally:
                listeners.release()

        def file(self, rel):
            p = lib.real(rel)
            if not p or not os.path.isfile(p) or not lib.playable(p):
                return self.fail(404, 'no such file')
            size = os.path.getsize(p)
            start, end = 0, size - 1
            rng = self.headers.get('Range', '')
            if rng.startswith('bytes=') and ',' not in rng:
                a, _, b = rng[6:].partition('-')
                try:
                    start, end = (int(a), int(b) if b else size - 1) if a else (max(0, size - int(b)), size - 1)
                except ValueError:
                    start, end = 0, size - 1
                if start > end or start >= size:
                    self.send_response(416)
                    self.send_header('Content-Range', f'bytes */{size}')
                    self.end_headers()
                    return
                end = min(end, size - 1)
            self.send_response(206 if rng else 200)
            self.send_header('Content-Type', mimetypes.guess_type(p)[0] or 'application/octet-stream')
            self.send_header('Accept-Ranges', 'bytes')
            self.send_header('Content-Length', str(end - start + 1))
            if rng:
                self.send_header('Content-Range', f'bytes {start}-{end}/{size}')
            self.end_headers()
            if self.command == 'HEAD':
                return
            with open(p, 'rb') as f:
                f.seek(start)
                left = end - start + 1
                while left:
                    b = f.read(min(65536, left))
                    if not b:
                        break
                    self.wfile.write(b)
                    left -= len(b)

        def m3u(self, rel):
            if rel is None:
                return self.fail(404, 'no such folder')
            base = self.base()
            lines = ['#EXTM3U'] + [f'#EXTINF:-1,{os.path.splitext(t.split("/")[-1])[0]}\n{base}/file/{urllib.parse.quote(t)}'
                                   for t in lib.tracks(rel)]
            self.send(200, 'audio/x-mpegurl; charset=utf-8', ('\n'.join(lines) + '\n').encode(),
                      [('Content-Disposition', f'inline; filename="{short_id(rel)}.m3u"')])

        def page(self, rel):
            rel = lib.folder(rel)
            if rel is None:
                return self.fail(404, 'no such folder')
            sub, mp3s = lib.dirs[rel]
            esc = html.escape
            link = lambda r: '/browse/' + urllib.parse.quote(r + '/' if r else '')
            parts, acc = [f'<a href="/">{esc(name)}</a>'], ''
            for c in rel.split('/') if rel else []:
                acc = f'{acc}/{c}' if acc else c
                parts.append(f'<a href="{link(acc)}">{esc(c)}</a>')
            body = ''
            if rel in lib.artists:
                body += '<h2>Artists</h2><ul>' + ''.join(
                    f'<li><a href="/s/{lib.station_id((rel, a))}">&#9654; {esc(a)}</a> '
                    f'<span class="s">{len(f)} album{"s" if len(f) > 1 else ""}</span></li>'
                    for a, f in lib.artists[rel].items()) + '</ul>'
            if sub:
                body += '<h2>Folders</h2><ul>' + ''.join(
                    f'<li><a href="{link(f"{rel}/{s}" if rel else s)}">{esc(s)}</a> '
                    f'<a class="s" href="/s/{short_id(f"{rel}/{s}" if rel else s)}">&#9654;</a></li>' for s in sub) + '</ul>'
            if mp3s:
                body += '<h2>Files</h2><ul>' + ''.join(
                    f'<li><a href="/file/{urllib.parse.quote(f"{rel}/{m}" if rel else m)}">{esc(os.path.splitext(m)[0])}</a></li>'
                    for m in mp3s) + '</ul>'
            station = f'/s/{short_id(rel)}'
            self.send(200, 'text/html; charset=utf-8', PAGE.format(
                title=esc(rel.split('/')[-1] if rel else name), crumbs=' / '.join(parts), station=station,
                m3u='/playlist.m3u?dir=' + urllib.parse.quote(rel), body=body, version=VERSION,
                full=esc(self.base() + station)).encode())
    return Handler


class DualStackServer(ThreadingHTTPServer):
    """one socket for IPv6 and IPv4 ("::" with IPV6_V6ONLY off), or IPv4 alone where there is no IPv6"""
    daemon_threads = True
    allow_reuse_address = True

    def server_bind(self):
        if self.address_family == socket.AF_INET6:
            try:
                self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
            except OSError:
                pass
        super().server_bind()


def serve(bind, port, handler):
    """the listening socket: "::" is every address, IPv6 and IPv4 on one socket, falling back to IPv4
    alone on a computer without IPv6; any other address is taken as it is"""
    def server(family, addr):
        cls = type('Server', (DualStackServer,), {'address_family': family})
        return cls((addr, port), handler)
    if bind in ('', '::'):
        try:
            return server(socket.AF_INET6, '::'), 'IPv6 and IPv4'
        except OSError as e:
            log(f'no IPv6 here ({e.strerror}): IPv4 only')
            return server(socket.AF_INET, '0.0.0.0'), 'IPv4'
    if ':' in bind:
        return server(socket.AF_INET6, bind), 'IPv6'
    return server(socket.AF_INET, bind), 'IPv4'


def main():
    ap = argparse.ArgumentParser(description='A folder of MP3s as internet radio stations (IPv4 and IPv6)')
    ap.add_argument('music', nargs='?', default=os.environ.get('MUSIC_DIR', '/music'), help='the music folder (/music)')
    ap.add_argument('--port', type=int, default=int(os.environ.get('MUSIC_PORT', '8000')), help='TCP port (8000)')
    ap.add_argument('--bind', default=os.environ.get('MUSIC_BIND', '::'),
                    help='address to listen on: "::" is every address, IPv6 and IPv4 (the default); 0.0.0.0 IPv4 only')
    ap.add_argument('--name', default=os.environ.get('MUSIC_NAME', 'Music'), help='the library\'s name in menus and pages')
    ap.add_argument('--max-listeners', type=int, default=int(os.environ.get('MUSIC_MAX_LISTENERS', '16')))
    ap.add_argument('--bitrate', type=int, default=int(os.environ.get('MUSIC_BITRATE', '192')),
                    help='kbit/s of the MP3 that FLAC, M4A and other formats are converted to (192)')
    ap.add_argument('--ffmpeg', default=os.environ.get('MUSIC_FFMPEG', shutil.which('ffmpeg') or ''),
                    help='ffmpeg, to play formats other than MP3 ("" for MP3s only)')
    a = ap.parse_args()
    if not os.path.isdir(a.music):
        sys.exit(f'no such folder: {a.music}')
    if a.bitrate not in (64, 80, 96, 112, 128, 160, 192, 224, 256, 320):
        sys.exit('--bitrate: one of 64, 80, 96, 112, 128, 160, 192, 224, 256, 320')
    lib = Library(a.music, a.ffmpeg or None)
    lib.scan()
    srv, how = serve(a.bind, a.port, make_handler(lib, a.name, a.max_listeners, a.bitrate))
    signal.signal(signal.SIGTERM, lambda *x: sys.exit(0))
    log(f'aurora68 music server {VERSION} "{a.name}" on port {a.port} ({how}): {lib.root}; '
        + (f'other formats converted to MP3 at {a.bitrate} kbit/s' if lib.ffmpeg else 'MP3s only (no ffmpeg)'))
    srv.serve_forever()


if __name__ == '__main__':
    main()
