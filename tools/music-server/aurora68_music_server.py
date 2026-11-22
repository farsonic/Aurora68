#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""aurora68_music_server.py - a folder of music as internet radio stations, over IPv4 and IPv6.

22 November 2026. The owner has a folder of MP3s, organised in folders (artist, album, or however they
like), on a NAS, and Aurora68 Play already plays internet radio: plain-http MP3 streams with Shoutcast
and Icecast's "ICY" song titles, over IPv4 or IPv6 (mac/piplay/radio.c). So every folder becomes a
station: its MP3s, and those of the folders inside it, one after another as one continuous stream, the
way a radio station sends them. Anything that plays internet radio plays these too (VLC, iTunes/Music,
a browser, foobar2000, a Squeezebox), and a browser gets a page listing the folders.

    GET /                         the control panel: the library as cover pictures, and the live channel's
                                  Now Playing, queue and listeners
    GET /live                     the live channel: one stream every listener hears, playing what the panel
                                  chose (POST /api/live; GET /api/now says what plays)
    GET /browse/PATH/             a plain web page of a folder: its folders, station and files
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
    GET /video/ID                 a music video (22 November 2026): the file with Range when a Mac plays it as it is,
                                  else remuxed (or, last, transcoded) to fragmented MP4 as it is sent;
                                  /video/ID/index.m3u8 the same as HLS; /vthumb/ID its poster; /vaudio/ID its
                                  sound as MP3; /vmpeg/ID MPEG-1 for the video card. Below, "music videos"

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
import argparse, hashlib, html, json, mimetypes, urllib.error, urllib.request, os, random, re, shutil, signal, socket, subprocess, sys, tempfile, \
    threading, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = '4'                   # 4: music videos (22 November 2026)
METAINT = 16000                 # bytes of audio between song titles, as most Icecast servers send
BURST = 6.0                     # seconds of audio sent at once when a listener connects
RESCAN = float(os.environ.get('MUSIC_RESCAN', 30))   # seconds a scan of the folder is trusted before it is made again
PLAY_MAX_STATIONS = 88          # Aurora68 Play holds 128 stations and 48 submenus (play/core/include/stations.h),
PLAY_MAX_GROUPS = 40            # 40 of the stations its own; the stations file stays within the rest
MENU_ROWS = 18                  # a menu on the SE's 342 lines: the menu bar and 18 items of 16 lines
ARTIST_MIN = 6                  # a folder of at least this many "Artist - Album" folders is grouped by artist
# /file/'s Content-Type: named here, not left to mimetypes, which on a NAS's minimal Linux may know no .m4a or
# .flac (AVFoundation, the macOS Play's player, decides by it whether it can play and seek a file)
AUDIO_TYPES = {'.mp3': 'audio/mpeg', '.m4a': 'audio/mp4', '.aac': 'audio/aac', '.alac': 'audio/mp4', '.flac': 'audio/flac',
               '.ogg': 'audio/ogg', '.oga': 'audio/ogg', '.opus': 'audio/ogg', '.wav': 'audio/wav', '.aif': 'audio/aiff',
               '.aiff': 'audio/aiff', '.mp2': 'audio/mpeg'}
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


DUP_NUMBER = __import__('re').compile(r'^\s*(\d{1,3}(?:-\d{1,3})?)[\s.\-_]+')
QUALITY = {'.flac': 0, '.wav': 1, '.aif': 1, '.aiff': 1, '.m4a': 2, '.alac': 2, '.ogg': 3, '.opus': 3, '.mp3': 4}


def without_copies(folder, songs, quality=QUALITY, numbered=True):
    """a folder's songs with copies of one track left out: (kept, left out).

    22 November 2026: the owner's Daft Punk album holds seven tracks twice, downloaded eleven days apart,
    "05. Daft Punk, Julian Casablancas - Instant Crush (...).m4a" and "05. Daft Punk; Julian Casablancas
    - Instant Crush (...).m4a": the artists written with a different separator, the files 103 bytes apart
    (their tags). Files are copies when their track number and title (after " - ", case and spacing
    aside) agree and their sizes are within 2%: a live version or a remix has a title of its own. Of the
    copies the best kind is kept (FLAC, then M4A, then MP3), then the larger. The files are not touched.

    Music videos (the same day) go through the same rule with their own order of kinds (those that play as they
    are first) and numbered=False: a video's name rarely has a number ("Artist - Title (Official Video).mp4"), so
    one without a number is compared by its title alone, still within 2% in size."""
    groups = {}
    for f in songs:
        stem = os.path.splitext(f)[0]
        m = DUP_NUMBER.match(stem)
        if not m and numbered:
            continue
        title = stem[m.end():].split(' - ', 1)[-1] if m else stem.split(' - ', 1)[-1]
        groups.setdefault((m.group(1).lstrip('0') if m else '', ' '.join(title.casefold().split())), []).append(f)
    out = set()
    for same in groups.values():
        if len(same) < 2:
            continue
        sizes = {}
        for f in same:
            try:
                sizes[f] = os.path.getsize(os.path.join(folder, f))
            except OSError:
                sizes[f] = 0
        same.sort(key=lambda f: (quality.get(os.path.splitext(f)[1].lower(), 5), -sizes[f], f))
        best = same[0]
        for f in same[1:]:
            if sizes[best] and abs(sizes[f] - sizes[best]) <= 0.02 * sizes[best]:
                out.add(f)
    return [f for f in songs if f not in out], sorted(out)


def artist_of(folder):
    """"Weezer" of "Weezer - Pinkerton (Explicit)", or None"""
    a, sep, rest = folder.partition(' - ')
    return a.strip() if sep and a.strip() and rest.strip() else None


# ---- music videos: what they are, how each plays, and the folder where the server keeps what it makes -----------
# 22 November 2026. The owner's library also holds music videos, in the same folders as the albums ("Artist - Album/
# Song (Official Video).mp4") and in artist folders. The scan finds them beside the songs; the catalogue attaches each
# to its artist, and to its album when it sits in an album's folder (or in a "Videos" folder inside one). They are
# served the cheapest way that plays on a Mac:
#   direct     the file as it is, with Range: MP4, M4V or QuickTime holding H.264 or HEVC with AAC, ALAC or MP3
#              sound, which AVFoundation (the macOS Play, Safari, QuickTime Player) plays from a file;
#   remux      the same streams copied into fragmented MP4 as they are sent, no picture re-encoded: an MKV or TS of
#              H.264, or sound the Mac lacks (AC-3, DTS, Opus beside H.264) converted to AAC on the way (sound is
#              cheap; the picture is what a NAS cannot afford to encode);
#   transcode  H.264 and AAC made anew, the last resort (VP9, AV1, MPEG-2, 10-bit H.264), at most VIDEO_TRANSCODES at
#              once and off altogether with VIDEO_TRANSCODE=off.
# A remuxed or transcoded stream has no length in advance, and AVFoundation will not play a progressive file whose
# server cannot answer a byte range (its error -11850, "server not correctly configured"), so those two are also
# offered as HLS (/video/ID/index.m3u8: fragmented-MP4 segments written to the cache as ffmpeg makes them), which
# AVFoundation plays as it grows; browsers other than Safari take the plain fragmented MP4 stream.
VIDEO_TYPES = {'.mp4': 'video/mp4', '.m4v': 'video/mp4', '.mov': 'video/quicktime', '.mkv': 'video/x-matroska',
               '.webm': 'video/webm', '.avi': 'video/x-msvideo', '.mpg': 'video/mpeg', '.mpeg': 'video/mpeg',
               '.ts': 'video/mp2t'}
VIDEO_AS_IS = ('.mp4', '.m4v', '.mov')       # containers AVFoundation plays from a file
VIDEO_CODECS = ('h264', 'hevc')              # pictures every Mac decodes (HEVC since macOS 10.13)
VIDEO_AUDIO = ('aac', 'alac', 'mp3')         # sound AVFoundation plays inside MP4 and QuickTime
VIDEO_PIX = ('yuv420p', 'yuvj420p')          # H.264 at 8 bits 4:2:0: Apple's decoders refuse "High 10" and 4:2:2
VIDEO_FOLDERS = ('videos', 'video', 'music videos', 'clips')   # such a folder inside an album or artist folder is part of it
VIDEO_QUALITY = {'.mp4': 0, '.m4v': 0, '.mov': 1, '.mkv': 2, '.webm': 3, '.ts': 4}   # of two copies, the one kept
VIDEO_PRESETS = ('ultrafast', 'superfast', 'veryfast', 'faster', 'fast', 'medium')


def env_int(name, default, lo, hi):
    try:
        return max(lo, min(hi, int(os.environ.get(name, default))))
    except ValueError:
        return default


VIDEOS = os.environ.get('VIDEOS', 'on').lower() != 'off'                  # off: no videos found, listed or served
VIDEO_TRANSCODE = os.environ.get('VIDEO_TRANSCODE', 'on').lower() != 'off'
VIDEO_TRANSCODES = env_int('VIDEO_TRANSCODES', 1, 1, 16)   # at once: a NAS's CPU encodes one 1080p H.264 at about its speed
VIDEO_BITRATE = env_int('VIDEO_BITRATE', 4000, 300, 40000)  # kbit/s ceiling of a transcode's picture: 1080p H.264, good
VIDEO_MAX_HEIGHT = env_int('VIDEO_MAX_HEIGHT', 1080, 144, 4320)   # a transcode taller than this is scaled down
VIDEO_PRESET = os.environ.get('VIDEO_PRESET', 'veryfast') if os.environ.get('VIDEO_PRESET', 'veryfast') in VIDEO_PRESETS \
    else 'veryfast'                                          # x264's speed: veryfast is what a NAS keeps up with
VIDEO_CACHE_MB = env_int('VIDEO_CACHE_MB', 2048, 16, 10 ** 7)   # posters, soundtracks and HLS segments kept, at most
VIDEO_IDLE = 60                 # seconds an HLS conversion runs with nobody fetching its segments before it is stopped
VIDEO_HLS_SECONDS = 4           # an HLS segment: short, so the first plays soon after a conversion starts
CACHE_DIR = os.environ.get('MUSIC_CACHE') or os.path.join(tempfile.gettempdir(), 'aurora68-music-cache')
# The Aurora68 Video Decoder card (docs/16): MPEG-1 program streams as tools/video-helper's /play makes them, so the
# classic Mac's player can show a library video through the card. The same limits: what a Pi 3 core decodes
MPEG_MAX_W, MPEG_MAX_H = 1280, 720


class Cache:
    """the server's own folder (MUSIC_CACHE) for what it makes once and keeps: ffprobe's answers about the videos
    (videos.json), posters (thumbs/), soundtracks as MP3 (vaudio/) and HLS conversions (hls/). The music folder
    stays read-only. Without a folder it can write, everything is made each time it is asked for, and said once"""
    def __init__(self, root):
        self.root, self.ok, self.lock = root, False, threading.Lock()
        try:
            for sub in ('thumbs', 'vaudio', 'hls'):
                os.makedirs(os.path.join(root, sub), exist_ok=True)
            t = os.path.join(root, '.write-test')
            open(t, 'wb').close()
            os.remove(t)
            self.ok = True
        except OSError as e:
            log(f'cache: cannot write {root} ({e.strerror}): video posters and soundtracks are made each time')

    def path(self, *parts):
        return os.path.join(self.root, *parts)

    def used(self, path):
        try:
            os.utime(path)
        except OSError:
            pass

    @staticmethod
    def drop(path):
        """a cached file, or a conversion's folder of segments: its files one by one, then the folder"""
        try:
            if os.path.isdir(path):
                for f in os.listdir(path):
                    os.remove(os.path.join(path, f))
                os.rmdir(path)
            else:
                os.remove(path)
        except OSError:
            pass

    def trim(self, busy=()):
        """posters, soundtracks and finished conversions within VIDEO_CACHE_MB: the least recently used go first"""
        if not self.ok:
            return
        with self.lock:
            items = []
            for sub in ('thumbs', 'vaudio', 'hls'):
                try:
                    names = os.listdir(self.path(sub))
                except OSError:
                    continue
                for n in names:
                    p = self.path(sub, n)
                    if p in busy or n.endswith('.part'):
                        continue
                    try:
                        if os.path.isdir(p):
                            files = [os.path.join(p, f) for f in os.listdir(p)]
                            size = sum(os.path.getsize(f) for f in files)
                            when = os.path.getmtime(os.path.join(p, 'index.m3u8')) if os.path.exists(os.path.join(p, 'index.m3u8')) \
                                else os.path.getmtime(p)
                        else:
                            size, when = os.path.getsize(p), os.path.getmtime(p)
                    except OSError:
                        continue
                    items.append((when, size, p))
            total, cap = sum(i[1] for i in items), VIDEO_CACHE_MB << 20
            for when, size, p in sorted(items):
                if total <= cap:
                    break
                self.drop(p)
                total -= size


class VideoProbe:
    """what ffprobe says of each video (length, size, codecs), kept by path, modification time and size in the cache
    folder's videos.json: the NAS is slow and a library's videos are probed once, not at every scan or restart"""
    def __init__(self, cache, ffprobe):
        self.ffprobe, self.file, self.known, self.dirty = ffprobe, cache.path('videos.json') if cache.ok else None, {}, False
        try:
            with open(self.file, encoding='utf-8') as f:
                self.known = {k: v for k, v in json.load(f).items() if isinstance(v, list) and len(v) == 3}
        except (OSError, ValueError, TypeError):
            pass

    def get(self, path):
        """the video's facts, {} when there is no ffprobe to ask, None when it holds no picture"""
        if not self.ffprobe:
            return {}
        try:
            st = os.stat(path)
        except OSError:
            return None
        k = self.known.get(path)
        if k and k[0] == st.st_mtime and k[1] == st.st_size:
            return k[2]
        info = self.probe(path)
        self.known[path], self.dirty = [st.st_mtime, st.st_size, info], True
        return info

    def probe(self, path):
        try:
            out = subprocess.run([self.ffprobe, '-v', 'error', '-show_entries',
                                  'format=duration,format_name:stream=codec_type,codec_name,codec_tag_string,width,height,'
                                  'pix_fmt,channels:stream_disposition=attached_pic', '-of', 'json', path],
                                 capture_output=True, text=True, timeout=60)
            j = json.loads(out.stdout or '{}')
        except (OSError, ValueError, subprocess.SubprocessError):
            return None
        streams = j.get('streams') or []
        v = next((s for s in streams if s.get('codec_type') == 'video' and not (s.get('disposition') or {}).get('attached_pic')), None)
        a = next((s for s in streams if s.get('codec_type') == 'audio'), None)
        if not v:
            return None                              # a cover picture alone, or no picture at all: not a video
        try:
            secs = float((j.get('format') or {}).get('duration') or 0)
        except ValueError:
            secs = 0.0
        return {'secs': round(secs, 2), 'w': int(v.get('width') or 0), 'h': int(v.get('height') or 0),
                'vcodec': v.get('codec_name', ''), 'vtag': v.get('codec_tag_string', ''), 'pix': v.get('pix_fmt', ''),
                'acodec': a.get('codec_name', '') if a else '', 'format': (j.get('format') or {}).get('format_name', '')}

    def save(self, keep):
        """forget videos no longer there, and write the file if anything changed"""
        gone = [p for p in self.known if p not in keep]
        for p in gone:
            del self.known[p]
        if not self.file or not (self.dirty or gone):
            return
        try:
            with open(self.file + '.part', 'w', encoding='utf-8') as f:
                json.dump(self.known, f)
            os.replace(self.file + '.part', self.file)
            self.dirty = False
        except OSError:
            pass


CODEC_NAMES = {'h264': 'H.264', 'hevc': 'HEVC', 'vp9': 'VP9', 'vp8': 'VP8', 'av1': 'AV1', 'mpeg2video': 'MPEG-2',
               'mpeg4': 'MPEG-4 Part 2', 'aac': 'AAC', 'alac': 'ALAC', 'mp3': 'MP3', 'ac3': 'AC-3', 'eac3': 'E-AC-3',
               'dts': 'DTS', 'opus': 'Opus', 'vorbis': 'Vorbis', 'flac': 'FLAC'}


def codec_name(c):
    return CODEC_NAMES.get(c, c.upper())


def video_how(ext, info):
    """how a video plays: ('direct' | 'remux' | 'transcode', the reason in words); (None, why) when it cannot be told"""
    if not info:                                     # no ffprobe (no ffmpeg): trust the container, offer only those
        return ('direct', 'not probed: an MP4 or QuickTime file goes as it is') if ext in VIDEO_AS_IS else \
            (None, 'without ffmpeg only MP4 and QuickTime files are offered')
    v, a, pix = info.get('vcodec', ''), info.get('acodec', ''), info.get('pix', '')
    if v not in VIDEO_CODECS:
        return 'transcode', f'{codec_name(v) if v else "its"} picture plays on no Mac as it is'
    if v == 'h264' and pix and pix not in VIDEO_PIX:
        return 'transcode', f'H.264 in {pix} (not 8-bit 4:2:0) plays on no Mac as it is'
    sound_ok = not a or a in VIDEO_AUDIO
    retag = v == 'hevc' and info.get('vtag') != 'hvc1'
    if ext in VIDEO_AS_IS and sound_ok and not retag:
        return 'direct', f'{ext[1:].upper()} with {codec_name(v)}{" and " + codec_name(a) if a else ""} plays as it is'
    why = []
    if ext not in VIDEO_AS_IS:
        why.append(f'{ext[1:].upper()} into MP4, {codec_name(v)} copied')
    if not sound_ok:
        why.append(f'{codec_name(a)} sound converted to AAC')
    if retag:
        why.append('HEVC labelled hvc1 for Apple')
    return 'remux', '; '.join(why)


def video_codec_args(v, how):
    """ffmpeg's stream arguments for a remux or a transcode (v: the catalogue's video)"""
    sound = ['-c:a', 'copy'] if v['acodec'] in VIDEO_AUDIO else ['-c:a', 'aac', '-b:a', '192k', '-ac', '2']
    if how == 'remux':
        return ['-map', '0:v:0', '-map', '0:a:0?', '-c:v', 'copy'] + (['-tag:v', 'hvc1'] if v['vcodec'] == 'hevc' else []) + sound
    return ['-map', '0:v:0', '-map', '0:a:0?', '-c:v', 'libx264', '-preset', VIDEO_PRESET, '-crf', '21',
            '-maxrate', f'{VIDEO_BITRATE}k', '-bufsize', f'{2 * VIDEO_BITRATE}k', '-pix_fmt', 'yuv420p',
            '-vf', f"scale=-2:'trunc(min({VIDEO_MAX_HEIGHT},ih)/2)*2'",
            '-force_key_frames', f'expr:gte(t,n_forced*{VIDEO_HLS_SECONDS // 2})',   # a fragment every 2 s, a segment every 4
            '-c:a', 'aac', '-b:a', '192k', '-ac', '2']


def mpeg_size(v, top, default):
    """a size for the video card's MPEG-1 (the helper's even()): a number, 64 to top, even"""
    try:
        v = int(v)
    except (TypeError, ValueError):
        v = default
    v = max(64, min(v, top))
    return v - v % 2


def mpeg1_args(w, h):
    """tools/video-helper's ffmpeg_args, kept identical: MPEG-1 video fitted into w x h (its shape kept, padded black),
    25 frames a second, no B-frames (each frame shown as it is decoded), MP2 sound at 44.1 kHz stereo, 128 kbit/s, an
    MPEG program stream; about 0.09 bit per pixel per frame, 300 to 2,500 kbit/s"""
    rate = max(300, min(2500, w * h * 25 // 450))
    vf = (f'scale={w}:{h}:force_original_aspect_ratio=decrease,pad={w}:{h}:(ow-iw)/2:(oh-ih)/2,'
          f'fps=25,format=yuv420p')
    return ['-vf', vf, '-c:v', 'mpeg1video', '-b:v', f'{rate}k', '-maxrate', f'{rate * 2}k', '-bufsize', f'{rate}k',
            '-bf', '0', '-g', '25', '-c:a', 'mp2', '-b:a', '128k', '-ar', '44100', '-ac', '2',
            '-f', 'mpeg', '-muxdelay', '0.5', 'pipe:1']


def tidy_title(stem, artist):
    """a track's (or a video's) number and title from its file's name: "01. Artist - Title" is 1, "Title" when the
    artist is the album's (the rule of play/core/names.c, which the core's tests check against this one)"""
    m = MAC_TRACK_NO.match(stem)
    number, title = (int(m.group(1)), m.group(2)) if m else (0, stem)
    if artist and title.casefold().startswith(artist.casefold() + ' - '):
        title = title[len(artist) + 3:]
    return number, title


class Library:
    """the folder: its MP3s by folder, scanned again when the last scan is RESCAN seconds old.

    A station is a folder (its relative name, '' for the whole library) or an artist: (folder, name).
    Many music libraries keep their albums side by side as "Artist - Album" folders; a folder of at
    least ARTIST_MIN folders of which two in three are named so is also offered by artist, each artist
    one station playing all their albums."""
    def __init__(self, root, ffmpeg=None):
        self.root = os.path.realpath(root)
        self.ffmpeg = ffmpeg             # converts the formats in CONVERTED; None: MP3s only
        self.copies = os.environ.get('MUSIC_COPIES', 'hide') != 'show'   # leave out copies of one track
        self.ffprobe = shutil.which('ffprobe') if ffmpeg else None
        self.lock = threading.Lock()
        self.when = 0.0
        self.generation = 0          # bumped by each scan that finds the library changed: players refresh their lists
        self.signature = None
        self.dirs = {}               # relative folder ('' for the root) -> (folders in it, songs in it), sorted
        self.ids = {}                # short ID -> a station: a relative folder, or (folder, artist)
        self.artists = {}            # relative folder -> {artist: [its folders inside it]}, for those offered so
        self.videos = {}             # relative folder -> its music videos' file names, sorted (folders with music or not)
        self.vinfo = {}              # a video's relative path -> what ffprobe says of it ({} without ffprobe)
        self.cache = Cache(CACHE_DIR)
        self.vprobe = VideoProbe(self.cache, self.ffprobe)

    def scan(self):
        with self.lock:
            if time.monotonic() - self.when < RESCAN:
                return
            dirs, ids, errors, passed, hidden, videos = {}, {}, [], {}, [], {}
            for top, subdirs, files in os.walk(self.root, onerror=errors.append, followlinks=False):
                rel = os.path.relpath(top, self.root)
                rel = '' if rel == '.' else rel.replace(os.sep, '/')
                subdirs[:] = sorted((d for d in subdirs if not d.startswith('.')), key=str.casefold)
                songs, vids = [], []
                for f in files:
                    ext = os.path.splitext(f)[1].lower()
                    if f.startswith('.'):
                        continue
                    if self.playable(f):
                        songs.append(f)
                    elif ext in VIDEO_TYPES and VIDEOS:
                        vids.append(f)
                    elif ext in CONVERTED:
                        passed[ext] = passed.get(ext, 0) + 1
                if self.copies and len(songs) > 1:
                    songs, gone = without_copies(top, songs)
                    if gone:
                        hidden.append((rel, gone))
                if self.copies and len(vids) > 1:
                    vids, gone = without_copies(top, vids, VIDEO_QUALITY, numbered=False)
                    if gone:
                        hidden.append((rel, gone))
                if vids:
                    videos[rel] = sorted(vids, key=str.casefold)
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
            vstat = self.find_videos(videos)
            self.when = time.monotonic()
            sig = hashlib.sha1((repr(sorted(self.dirs.items())) + (repr(sorted(self.videos.items())) if self.videos else ''))
                               .encode('utf-8', 'surrogateescape')).digest()
            if sig == self.signature:
                return                                 # nothing changed: nothing to say again every RESCAN seconds
            self.signature, self.generation = sig, self.generation + 1
            n = sum(len(v[1]) for v in self.dirs.values())
            kinds = {}
            for v in self.dirs.values():
                for f in v[1]:
                    e = os.path.splitext(f)[1].lower().lstrip('.').upper()
                    kinds[e] = kinds.get(e, 0) + 1
            what = ', '.join(f'{c} {k}' for k, c in sorted(kinds.items(), key=lambda kv: -kv[1])) or 'no music'
            nv = sum(len(v) for v in self.videos.values())
            log(f'library: {n} songs ({what}) in {len(self.dirs) - 1} folders under {self.root}' +
                (f', and {nv} music video{"" if nv == 1 else "s"} ({vstat})' if nv else '') +
                (f' (changed: generation {self.generation})' if self.generation > 1 else ''))
            for e in errors[:5]:                       # what could not be read: permissions, usually
                log(f'library: cannot read {e.filename}: {e.strerror} (the server runs as user {os.getuid()}:{os.getgid()})')
            if len(errors) > 5:
                log(f'library: ... and {len(errors) - 5} more that cannot be read')
            if hidden:
                n_gone = sum(len(g) for _, g in hidden)
                log(f'library: {n_gone} {"copy" if n_gone == 1 else "copies"} of tracks left out (the same number, title '
                    f'and size) in {len(hidden)} folder{"" if len(hidden) == 1 else "s"}, e.g. "{hidden[0][0]}/{hidden[0][1][0]}"; '
                    f'MUSIC_COPIES=show keeps them')
            if passed:
                log('library: passed over ' + ', '.join(f'{c} {e[1:].upper()}' for e, c in sorted(passed.items(), key=lambda kv: (-kv[1], kv[0]))) +
                    ' files: ffmpeg is not installed here to convert them (the Docker image has it)')
            if not n and not errors and not passed:
                log('library: no music files found: is the right folder mounted at ' + self.root + '?')

    def find_videos(self, found):
        """the scan's videos: those inside the folder (a link out of it is no video of ours), probed (the cache
        answers for those seen before), those ffprobe finds no picture in passed over; a summary of how they play"""
        videos, vinfo, how, bad, seen = {}, {}, {}, [], set()
        for rel, files in found.items():
            keep = []
            for f in files:
                r = f'{rel}/{f}' if rel else f
                p = self.real(r)
                if not p or not os.path.isfile(p):
                    continue
                info = self.vprobe.get(p)
                seen.add(p)                        # kept in the cache even when no video: not probed again at each scan
                ok = video_how(os.path.splitext(f)[1].lower(), info)[0] if info is not None else None
                if not ok:
                    bad.append(r)
                    continue
                keep.append(f)
                vinfo[r] = info
                how[ok] = how.get(ok, 0) + 1
            if keep:
                videos[rel] = keep
        self.vprobe.save(seen)
        new_bad = [r for r in bad if r not in getattr(self, 'vbad', ())]
        self.vbad = set(bad)
        if new_bad:
            log(f'library: {len(new_bad)} video file{"" if len(new_bad) == 1 else "s"} passed over (no picture in it, or '
                f'{"no ffmpeg to play it" if not self.ffmpeg else "ffprobe could not read it"}), e.g. "{new_bad[0]}"')
        self.videos, self.vinfo = videos, vinfo
        names = {'direct': 'as they are', 'remux': 'remuxed', 'transcode': 'transcoded'}
        return ', '.join(f'{c} {names[k]}' for k, c in sorted(how.items(), key=lambda kv: list(names).index(kv[0])))

    def video_path(self, rel):
        """a video of the library by its relative path: its real path, or None"""
        folder, _, f = rel.rpartition('/')
        return self.real(rel) if f in self.videos.get(folder, ()) else None

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


def converted_chunks(ffmpeg, path, kbps, start=0):
    """another format through ffmpeg into MP3, framed as it comes, half a second at a time"""
    p = subprocess.Popen([ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error'] + (['-ss', str(start)] if start else []) +
                         ['-i', path, '-map', '0:a:0',
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


# ---- the live channel: one stream, chosen from the control panel -----------------------------------
# 22 November 2026: the owner asked for the server to be "more of a control panel", choosing what plays.
# So besides its stations the server has one channel every listener hears at the same moment, like a
# radio station of one's own: GET /live. What it plays is a queue set from the web page (or from anything
# that can POST /api/live): an album, an artist, a folder or single songs, with pause, skip, previous,
# shuffle and repeat. The SE tunes to it once, in Aurora68 Play's Radio menu, and is then controlled
# from a phone or another computer.
#
# It keeps LIVE_LEAD seconds ahead of the clock (a station keeps BURST), so a change made on the panel
# is heard within a few seconds; while nothing is queued or it is paused it sends silence, so listeners
# stay connected. While nobody listens it holds its place: a queue started before the Mac tunes in is
# heard from its start.
LIVE_LEAD = 2.0
SILENT_FRAME = b'\xff\xfb\x90\x44' + bytes(413)    # MPEG-1 layer III, 128 kbit/s, 44.1 kHz, joint stereo, all
SILENT_SECS = 1152 / 44100                         # side information and main data zero: 26 ms of silence


class Listener:
    def __init__(self, who):
        self.who, self.since, self.cond = who, time.time(), threading.Condition()
        self.chunks, self.secs, self.gone = [], 0.0, False

    def push(self, data, secs, title):
        with self.cond:
            self.chunks.append((data, secs, title))
            self.secs += secs
            self.cond.notify()

    def pop(self, timeout=5.0):
        with self.cond:
            if not self.chunks:
                self.cond.wait(timeout)
            if not self.chunks:
                return None
            data, secs, title = self.chunks.pop(0)
            self.secs -= secs
            return data, title


class Live:
    def __init__(self, lib, name, kbps):
        self.lib, self.name, self.kbps = lib, name, kbps
        self.lock = threading.Condition()
        self.queue, self.pos = [], 0          # songs (relative paths) and the one playing (or about to)
        self.paused = self.repeat = False
        self.source = None                    # the playing song's chunks, a generator
        self.title, self.done, self.length = '', 0.0, 0.0   # its title, seconds sent of it, its length
        self.listeners = []
        self.recent = []                      # the last LIVE_LEAD seconds sent: what a new listener gets first
        self.titles = {}                      # songs' titles, looked up once
        self.changes = 0                      # bumped at each change, for the panel
        self.t0 = None                        # when the stream's clock started (None while nobody listens)
        self.video_rel = None                 # a video's id -> its relative path (the catalogue's, set by the handler)
        self.marks = []                       # (seconds into the stream, pos, song, title, its start, length):
        self.started = 0.0                    # where each song begins, so the panel shows what is heard,
        threading.Thread(target=self.run, daemon=True).start()   # LIVE_LEAD behind what is sent

    # -- what the panel sees and does
    def song_name(self, rel):
        if rel not in self.titles:
            path = self.lib.real(rel)
            self.titles[rel] = (song_title(path) if rel.lower().endswith('.mp3') else tag_title(self.lib.ffprobe, path)) \
                if path else rel
        return self.titles[rel]

    def heard(self):
        """(pos, song, title, seconds into it, length) of what listeners hear now; song None for silence"""
        if self.t0 is not None and self.marks:
            at = time.monotonic() - self.t0
            m = self.marks[0]
            for mark in self.marks:
                if mark[0] <= at:
                    m = mark
            return m[1], m[2], m[3], max(0.0, at - m[4]), m[5]
        cur = self.queue[self.pos] if self.pos < len(self.queue) else None
        return self.pos, cur, self.song_name(cur) if cur else '', 0.0, 0.0

    def now(self):
        with self.lock:
            pos, cur, title, elapsed, length = self.heard()
            return {'name': self.name, 'playing': bool(cur) and not self.paused, 'paused': self.paused,
                    'repeat': self.repeat, 'pos': pos, 'changes': self.changes,
                    'current': {'path': cur, 'folder': cur.rsplit('/', 1)[0] if cur and '/' in cur else '',
                                'title': title, 'elapsed': round(min(elapsed, length or elapsed), 1),
                                'length': round(length, 1)} if cur else None,
                    'queue': [{'path': r, 'title': os.path.splitext(r.rsplit('/', 1)[-1])[0]} for r in self.queue],
                    'listeners': [{'who': l.who, 'since': int(l.since)} for l in self.listeners]}

    def resolve(self, what):
        """songs named by the panel: s:ID (a station: folder or artist), f:FOLDER, t:SONG"""
        kind, _, arg = what.partition(':')
        if kind == 's':
            self.lib.scan()
            key = self.lib.ids.get(arg)
            return self.lib.tracks(key) if key is not None else []
        if kind == 'f':
            rel = self.lib.folder(arg)
            return self.lib.tracks(rel) if rel is not None else []
        if kind == 't':
            p = self.lib.real(arg)
            return [arg.strip('/')] if p and os.path.isfile(p) and self.lib.playable(p) else []
        if kind == 'v' and self.video_rel:                 # a music video: Live plays its sound
            rel = self.video_rel(arg)
            return [rel] if rel else []
        return []

    def act(self, a):
        """one change from the panel; an error in words, or None"""
        action = a.get('action', '')
        songs = self.resolve(a.get('what', '')) if a.get('what') else []
        if action in ('play', 'add', 'next') and not songs:
            return 'nothing to play there'
        with self.lock:
            hpos, hsong, _, helapsed, _ = self.heard()
            if hsong is not None and hsong == (self.queue[hpos] if hpos < len(self.queue) else None):
                self.pos = hpos if self.pos != hpos and action in ('skip', 'previous') else self.pos
            if action in ('play', 'add', 'next') and a.get('shuffle'):
                random.shuffle(songs)
            if action == 'play':
                self.queue, self.pos, self.paused = songs, 0, False
                self.drop_source()
            elif action == 'add':
                self.queue += songs
            elif action == 'next':
                at = min((hpos if hsong else self.pos) + 1, len(self.queue))
                self.queue[at:at] = songs
                if self.pos >= at:                         # what was sent ahead is no longer next
                    self.pos = at
                    self.drop_source()
            elif action == 'skip':
                self.pos = min(self.pos + 1, len(self.queue))
                self.drop_source()
            elif action == 'previous':
                if helapsed < 3 and self.pos > 0:
                    self.pos -= 1
                self.drop_source()
            elif action == 'jump':
                i = int(a.get('index', -1))
                if not 0 <= i < len(self.queue):
                    return 'no such place in the queue'
                self.pos, self.paused = i, False
                self.drop_source()
            elif action == 'remove':
                i = int(a.get('index', -1))
                if not 0 <= i < len(self.queue):
                    return 'no such place in the queue'
                del self.queue[i]
                if i < self.pos:
                    self.pos -= 1
                elif i == self.pos:
                    self.drop_source()
            elif action == 'clear':
                self.queue, self.pos = [], 0
                self.drop_source()
            elif action == 'shuffle':                      # the songs after the playing one
                rest = self.queue[self.pos + 1:]
                random.shuffle(rest)
                self.queue[self.pos + 1:] = rest
            elif action == 'pause':
                self.paused = True
            elif action == 'resume':
                self.paused = False
            elif action == 'repeat':
                self.repeat = bool(a.get('on', not self.repeat))
            else:
                return f'no such action: {action}'
            self.changes += 1
            self.lock.notify_all()
        log(f'live: {action} {a.get("what", "")}'.rstrip())
        return None

    # -- the stream
    def drop_source(self):
        if self.source:
            self.source.close()
        self.source, self.title, self.done, self.length = None, '', 0.0, 0.0

    def join(self, who):
        l = Listener(who)
        with self.lock:
            for c in self.recent:
                l.push(*c)
            self.listeners.append(l)
            self.changes += 1
            self.lock.notify_all()
        log(f'live: {who} tuned in')
        return l

    def leave(self, l):
        with self.lock:
            if l in self.listeners:
                self.listeners.remove(l)
            self.changes += 1
        log(f'live: {l.who} left')

    def next_chunk(self):
        """(bytes, seconds, title): the next half second of the playing song, or of silence"""
        silence = (SILENT_FRAME * 19, SILENT_SECS * 19)
        for _ in range(len(self.queue) + 1):
            if self.paused:
                return silence + (f'{self.name}: paused',)
            if self.pos >= len(self.queue):
                if self.repeat and self.queue:
                    self.pos = 0
                else:
                    return silence + (f'{self.name}: nothing queued',)
            if self.source is None:
                rel = self.queue[self.pos]
                path = self.lib.real(rel)
                if not path:
                    self.pos += 1
                    continue
                self.source = mp3_chunks(open(path, 'rb').read()) if rel.lower().endswith('.mp3') \
                    else converted_chunks(self.lib.ffmpeg, path, self.kbps)
                self.title, self.done = self.song_name(rel), 0.0
                self.length = probe_length(self.lib.ffprobe, path)
                log(f'live: {self.title}')
            try:
                data, secs = next(self.source)
                self.done += secs
                return data, secs, self.title
            except (StopIteration, OSError):
                self.drop_source()
                self.pos += 1
                self.changes += 1
        return silence + (self.name,)

    def run(self):
        t0, due = None, 0.0
        while True:
            with self.lock:
                while not self.listeners:
                    t0 = self.t0 = None                    # nobody listens: hold the place
                    self.lock.wait()
                if t0 is None:
                    t0, due = time.monotonic(), 0.0
                    self.t0, self.marks = t0, []
                try:
                    data, secs, title = self.next_chunk()
                except Exception as e:                     # a song that cannot be read: pass it
                    log(f'live: {e}')
                    self.drop_source()
                    self.pos += 1
                    continue
                song = self.queue[self.pos] if self.source is not None and self.pos < len(self.queue) else None
                if not self.marks or self.marks[-1][1:4] != (self.pos, song, title):
                    self.marks.append((due, self.pos, song, title, due, self.length))
                    self.marks = [m for m in self.marks if m[0] > due - 60] or self.marks[-1:]
                self.recent.append((data, secs, title))
                while sum(c[1] for c in self.recent) > LIVE_LEAD + 0.5:
                    self.recent.pop(0)
                for l in list(self.listeners):
                    if l.secs > 60:                        # a listener a minute behind is not listening
                        l.gone = True
                        self.listeners.remove(l)
                    else:
                        l.push(data, secs, title)
            due += secs
            ahead = due - (time.monotonic() - t0) - LIVE_LEAD
            if ahead > 0:
                time.sleep(ahead)


def probe_length(ffprobe, path):
    if not ffprobe:
        return 0.0
    try:
        out = subprocess.run([ffprobe, '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', path],
                             capture_output=True, text=True, timeout=20)
        return float(out.stdout.strip() or 0)
    except (OSError, ValueError, subprocess.SubprocessError):
        return 0.0


EMBEDDED = {}                       # folder -> its first song's own picture (JPEG bytes, or b'' for none)


def cover(lib, rel):
    """a folder's cover picture as (bytes, type): cover, folder or front .jpg/.png, else any picture in it,
    else the picture inside its first song (M4A, FLAC and MP3 carry one), looked for once"""
    path = lib.real(rel)
    if not path or not os.path.isdir(path):
        return None
    names = sorted(f for f in os.listdir(path) if not f.startswith('.'))
    pics = [f for f in names if f.lower().endswith(('.jpg', '.jpeg', '.png'))]
    pics.sort(key=lambda f: (os.path.splitext(f)[0].lower() not in ('cover', 'folder', 'front', 'album'), f))
    if pics:
        with open(os.path.join(path, pics[0]), 'rb') as f:
            return f.read(), mimetypes.guess_type(pics[0])[0] or 'image/jpeg'
    if path not in EMBEDDED and lib.ffmpeg:
        song = next((f for f in names if lib.playable(f)), None)
        pic = b''
        if song:
            try:
                pic = subprocess.run([lib.ffmpeg, '-nostdin', '-v', 'error', '-i', os.path.join(path, song), '-an',
                                      '-frames:v', '1', '-c:v', 'mjpeg', '-f', 'image2pipe', 'pipe:1'],
                                     capture_output=True, timeout=20).stdout
            except (OSError, subprocess.SubprocessError):
                pass
        if len(EMBEDDED) > 2000:
            EMBEDDED.clear()
        EMBEDDED[path] = pic
    return (EMBEDDED[path], 'image/jpeg') if EMBEDDED.get(path) else None


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
    lines.append(mac_roman(f'{name} Live') + b' = ' + f'{base}/live'.encode())   # what the control panel plays
    stations, groups = 2, 1
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


# ---- getting albums: a Newznab indexer (NZBGeek) and SABnzbd -----------------------------------------
# 22 November 2026: the owner asked to find albums with NZBGeek's search and hand them to SABnzbd, which
# already runs beside this server. The panel searches (GET /api/get/search), the server asks the indexer
# (the Newznab API that NZBGeek and others speak, audio categories) and keeps each result's download link
# itself: the link carries the indexer's key, so the browser sees only a short id. POST /api/get/grab
# gives the link to SABnzbd (mode=addurl, in its "music" category); GET /api/get/queue shows SABnzbd's
# queue and recent history for that category. SABnzbd files a finished album where its category says (the
# owner points it at the Albums folder) and the library finds it at its next scan.
#
# Off unless both keys are set: GET_INDEXER_KEY (and GET_INDEXER_URL, NZBGeek's by default) and
# GET_SAB_URL with GET_SAB_KEY. The keys come from the environment (docker-compose's .env), are never
# logged or sent to a browser, and are cut out of any error text.
class Getter:
    def __init__(self):
        e = os.environ.get
        self.indexer = e('GET_INDEXER_URL', 'https://api.nzbgeek.info').rstrip('/')
        self.ikey = e('GET_INDEXER_KEY', '').strip()
        self.sab = e('GET_SAB_URL', '').rstrip('/')
        self.skey = e('GET_SAB_KEY', '').strip()
        self.category = e('GET_SAB_CATEGORY', 'music').strip() or 'music'
        self.cats = e('GET_INDEXER_CATS', '3000')       # Newznab's audio: 3010 MP3, 3040 lossless ...
        self.results = {}                               # short id -> (title, link), the last searches'
        self.lock = threading.Lock()

    def enabled(self):
        return bool(self.ikey and self.sab and self.skey)

    def status(self):
        return {'enabled': self.enabled(), 'indexer': urllib.parse.urlsplit(self.indexer).hostname or '',
                'sab': bool(self.sab and self.skey), 'category': self.category}

    def clean(self, text):
        for k in (self.ikey, self.skey):
            if k:
                text = text.replace(k, '…')
        return text

    def fetch(self, url, timeout=25):
        req = urllib.request.Request(url, headers={'User-Agent': f'aurora68-music-server/{VERSION}'})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return r.read(4 << 20)
        except urllib.error.HTTPError as e:
            raise RuntimeError(self.clean(f'{urllib.parse.urlsplit(url).hostname} answered {e.code} {e.reason}'))
        except (urllib.error.URLError, OSError) as e:
            raise RuntimeError(self.clean(f'{urllib.parse.urlsplit(url).hostname}: {getattr(e, "reason", e)}'))

    def search(self, q):
        """[{id, title, size, age_days, grabs, category}] from the indexer, newest first"""
        import xml.etree.ElementTree as ET
        url = f'{self.indexer}/api?' + urllib.parse.urlencode({'t': 'search', 'q': q, 'cat': self.cats, 'limit': '100',
                                                               'extended': '1', 'apikey': self.ikey})
        raw = self.fetch(url)
        try:
            root = ET.fromstring(raw)
        except ET.ParseError:
            raise RuntimeError('the indexer\'s answer was not understood')
        if root.tag == 'error':
            raise RuntimeError(self.clean('the indexer: ' + (root.get('description') or root.get('code') or 'error')))
        ns = '{http://www.newznab.com/DTD/2010/feeds/attributes/}attr'
        out = []
        for item in root.iter('item'):
            title = (item.findtext('title') or '').strip()
            enc = item.find('enclosure')
            link = (enc.get('url') if enc is not None else None) or (item.findtext('link') or '').strip()
            if not title or not link.startswith(('http://', 'https://')):
                continue
            attrs = {a.get('name'): a.get('value') for a in item.iter(ns)}
            size = int(attrs.get('size') or (enc.get('length') if enc is not None else 0) or 0)
            age = 0
            try:
                from email.utils import parsedate_to_datetime
                age = max(0, int((time.time() - parsedate_to_datetime(item.findtext('pubDate')).timestamp()) // 86400))
            except (TypeError, ValueError):
                pass
            rid = hashlib.sha1(link.encode()).hexdigest()[:10]
            out.append({'id': rid, 'title': title, 'size': size, 'age_days': age,
                        'grabs': int(attrs.get('grabs') or 0), 'category': attrs.get('category', '')})
            with self.lock:
                self.results[rid] = (title, link)
        with self.lock:
            while len(self.results) > 2000:
                self.results.pop(next(iter(self.results)))
        return out

    def sab_api(self, **args):
        url = f'{self.sab}/api?' + urllib.parse.urlencode({**args, 'apikey': self.skey, 'output': 'json'})
        try:
            return json.loads(self.fetch(url) or b'{}')
        except ValueError:
            raise RuntimeError('SABnzbd\'s answer was not understood')

    def grab(self, rid):
        with self.lock:
            got = self.results.get(rid)
        if not got:
            return 'that result is no longer known: search again'
        title, link = got
        r = self.sab_api(mode='addurl', name=link, cat=self.category, nzbname=title)
        if not r.get('status'):
            return self.clean('SABnzbd refused it: ' + str(r.get('error') or r))
        log(f'get: sent "{title}" to SABnzbd ({self.category})')
        return None

    def queue(self):
        q = self.sab_api(mode='queue').get('queue', {})
        h = self.sab_api(mode='history', limit='15', category=self.category).get('history', {})
        items = [{'name': s.get('filename', ''), 'status': s.get('status', ''), 'percent': int(float(s.get('percentage') or 0)),
                  'left': s.get('timeleft', ''), 'size': s.get('size', '')}
                 for s in q.get('slots', []) if (s.get('cat') or '') == self.category]
        done = [{'name': s.get('name', ''), 'status': s.get('status', ''), 'size': s.get('size', ''),
                 'message': self.clean(s.get('fail_message') or '')} for s in h.get('slots', [])]
        return {'downloading': items, 'recent': done, 'speed': q.get('speed', ''), 'paused': bool(q.get('paused'))}


# ---- the server's own addresses: which to hand out, IPv6 or IPv4 ------------------------------------
# The owner wants IPv6 to work and to choose which of the two the Mac is given (22 November 2026). The
# server listens on both (MUSIC_LISTEN); the stations file and the control panel give the Mac an address,
# and MUSIC_PREFER says which: "ipv6", "ipv4", or "asked" (the one the browser or Mac used, the default).
# A name such as home-docker.local is no help to a Mac whose name service cannot look it up, so a
# preference writes the address itself. IPv6: a stable one (not a temporary "privacy" address, which
# changes every day or so), a unique local one (fd..., the same whatever the provider does) before a
# global one; MUSIC_IPV6_ADDRESS and MUSIC_IPV4_ADDRESS name one by hand.
def own_addresses():
    """{'ipv4': '192.168.1.20', 'ipv6': 'fd00::20'} as far as this computer has them"""
    out = {}
    v4 = os.environ.get('MUSIC_IPV4_ADDRESS', '').strip()
    if not v4:
        try:
            u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            u.connect(('192.0.2.1', 9))                # a documentation address: nothing is sent
            v4 = u.getsockname()[0]
            u.close()
        except OSError:
            v4 = ''
    if v4 and not v4.startswith('127.'):
        out['ipv4'] = v4
    v6 = os.environ.get('MUSIC_IPV6_ADDRESS', '').strip().strip('[]')
    if not v6:
        stable = []
        try:
            with open('/proc/net/if_inet6') as f:     # Linux: address, index, prefix, scope, flags, device
                for line in f:
                    a, _, _, scope, flags, dev = line.split()[:6]
                    fl = int(flags, 16)
                    if scope != '00' or fl & (0x01 | 0x20 | 0x40) or dev == 'lo' or \
                            dev.startswith(('docker', 'br-', 'veth', 'virbr')):
                        continue                       # temporary, deprecated, tentative, or a container's
                    addr = socket.inet_ntop(socket.AF_INET6, bytes.fromhex(a))
                    # unique local first, then global; the documentation range 2001:db8::/32 last: a
                    # router may advertise it on a home network, but not every device takes it up
                    rank = 0 if addr.startswith(('fc', 'fd')) else 2 if addr.startswith('2001:db8:') else 1
                    stable.append((rank, addr))
        except (OSError, ValueError):
            pass
        if stable:
            v6 = sorted(stable)[0][1]
        elif socket.has_ipv6:
            try:
                u = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
                u.connect(('2001:db8::1', 9))
                v6 = u.getsockname()[0]
                u.close()
            except OSError:
                v6 = ''
    if v6 and not v6.startswith('fe80') and v6 != '::1':
        out['ipv6'] = v6
    return out


def address_url(addr, port):
    return f'http://[{addr}]:{port}' if ':' in addr else f'http://{addr}:{port}'


# ---- the library for Aurora68 Play's Library window (/mac/...) -------------------------------------
# 22 November 2026 (docs/08 item 99). Aurora68 Play browses the library as artists, their albums and the
# albums' tracks, plays a track, an album or an artist with a queue, and shows the album's cover. Its
# answers are made for a 68k program written in C with no library: text, not JSON; Mac Roman; one record a
# line, ended by a Return; fields separated by tabs; a first line that says what follows:
#     A68M <tab> 1 <tab> KIND <tab> TOTAL <tab> FROM <tab> COUNT
# then COUNT records, records FROM to FROM + COUNT - 1 of TOTAL (?from=N&count=M pages a long list; the
# program keeps a page, not the library). Covers come as ready-made bitmaps, dithered here: a 68k has no
# JPEG decoder worth the wait, and an SE's screen has one bit a pixel.
MAC_DISCOVERY = 8070            # UDP: "AURORA68-MUSIC?" is answered with the server's name and TCP port
MAC_PAGE = 200                  # records in an answer when the program does not say
MAC_PAGE_MAX = 500
MAC_ART_MIN, MAC_ART_MAX = 8, 128       # a cover's side in pixels
MAC_COVERS = ('cover', 'folder', 'front', 'albumart', 'album')   # picture files that are an album's cover
MAC_TRACK_NO = re.compile(r'^\s*(\d{1,3})\s*[.\-_)]?\s+(.*)$')     # "01. Artist - Title", "01 Title", "1-Title"


MAC_WIDE = 2000                 # a path or a folder (full=1): longer than a name may be, never cut short at 120


def mac_field(s, n=120):
    """a field of a /mac/ record: one line, no tabs, at most n characters"""
    return ' '.join(str(s).replace('\t', ' ').split())[:n]


class Wide(str):
    """a field that is a path, a folder or an address: kept whole (up to MAC_WIDE), as /file/ and f: need it"""


def mac_line(r, n=120):
    return '\t'.join(mac_field(f, MAC_WIDE if isinstance(f, Wide) else n) for f in r)


def mac_answer(kind, total, start, rows, enc='mac_roman'):
    head = f'A68M\t1\t{kind}\t{total}\t{start}\t{len(rows)}'
    text = '\r'.join([head] + [mac_line(r) for r in rows]) + '\r'
    return text.encode(enc, 'replace')


def page_of(items, q):
    try:
        start = max(0, int(q.get('from', 0)))
        count = max(1, min(MAC_PAGE_MAX, int(q.get('count', MAC_PAGE))))
    except ValueError:
        start, count = 0, MAC_PAGE
    return items[start:start + count], start


class Catalogue:
    """the library as artists, albums and tracks, made again whenever the folder's scan is new.

    An album is a folder that holds songs itself. Its artist: the part before " - " of a folder named
    "Artist - Album" (most libraries' way, the owner's among them); else the folder it is in; an album at
    the top with neither is its own artist. A track's number and title come from its file's name
    ("01. Artist - Title.m4a") at once, and from its tags with its length when its album is first listed
    (ffprobe, a few hundredths of a second a file, kept): a library of thousands of files is not read
    through at the start."""
    def __init__(self, lib, name):
        self.lib, self.name = lib, name
        self.lock = threading.Lock()
        self.made = None
        self.artists, self.albums, self.tracks = {}, {}, {}
        self.artist_order = []
        self.videos = {}                     # id -> a music video (Catalogue.add_videos says what it holds)
        self.album_videos, self.artist_videos = {}, {}    # album id / artist id -> its videos' ids, in order
        self.video_artists = {}              # artist id -> name, for every artist with a video (some have no album)
        self.probed = {}                     # a file's path -> (mtime, size, number, title, artist, seconds)

    def fresh(self):
        self.lib.scan()
        with self.lock:
            if self.made == self.lib.when:
                return
            artists, albums, tracks, parents = {}, {}, {}, {}
            for rel, (sub, songs) in self.lib.dirs.items():
                if not songs:
                    continue
                folder = rel.split('/')[-1] if rel else self.name
                parent = rel.rsplit('/', 1)[0].split('/')[-1] if '/' in rel else ''
                a = artist_of(folder)
                if a:
                    artist, title = a, folder.partition(' - ')[2].strip()
                elif parent:
                    artist, title = parent, folder
                    parents[rel.rsplit('/', 1)[0]] = artist        # an artist's folder, its albums inside: for videos
                else:
                    artist, title = folder, folder
                aid = short_id('\x01' + artist.casefold())
                al = short_id(rel)
                artists.setdefault(aid, {'name': artist, 'albums': []})['albums'].append(al)
                ts = []
                for s in songs:
                    t = f'{rel}/{s}' if rel else s
                    tid = short_id('\x02' + t)
                    tracks[tid] = (al, t)
                    ts.append(tid)
                albums[al] = {'rel': rel, 'title': title, 'artist': artist, 'aid': aid, 'tracks': ts}
            for v in artists.values():
                v['albums'].sort(key=lambda i: albums[i]['title'].casefold())
            self.artists, self.albums, self.tracks = artists, albums, tracks
            self.artist_order = sorted(artists, key=lambda i: sort_key(artists[i]['name']))
            self.add_videos(parents)
            self.made = self.lib.when

    def add_videos(self, parents):
        """the scan's videos in the catalogue (22 November 2026). A video's home is the folder it is in, or the album or
        artist folder holding its "Videos" folder. In an album's folder (one with songs) it is that album's, by that
        album's artist; in an artist's folder (one whose albums are folders inside it), or a folder inside that, it is
        that artist's; else its folder's name is the artist ("Artist - Videos" gives "Artist"); at the top of the
        library the file's own name ("Artist - Title.mp4"), else the library's name. Its title is its file's name
        tidied as a track's is"""
        videos, by_album, by_artist, names = {}, {}, {}, {}
        for rel, files in self.lib.videos.items():
            home = rel
            if home and home.split('/')[-1].casefold() in VIDEO_FOLDERS:
                home = home.rsplit('/', 1)[0] if '/' in home else ''
            al = short_id(home) if short_id(home) in self.albums and self.albums[short_id(home)]['rel'] == home else ''
            up = home.rsplit('/', 1)[0] if '/' in home else ''
            for f in files:
                r = f'{rel}/{f}' if rel else f
                stem, ext = os.path.splitext(f)
                if al:
                    artist = self.albums[al]['artist']
                elif home in parents:
                    artist = parents[home]
                elif home and up in parents:
                    artist = parents[up]
                elif home:
                    name = home.split('/')[-1]
                    artist = artist_of(name) or name
                else:
                    m = MAC_TRACK_NO.match(stem)
                    artist = artist_of(m.group(2) if m else stem) or self.name
                number, title = tidy_title(stem, artist)
                info = self.lib.vinfo.get(r) or {}
                how, why = video_how(ext.lower(), info)
                vid, aid = short_id('\x03' + r), short_id('\x01' + artist.casefold())
                videos[vid] = {'rel': r, 'folder': rel, 'home': home, 'title': title, 'number': number, 'artist': artist,
                               'aid': aid, 'album': al, 'secs': int(info.get('secs', 0) + 0.5), 'w': info.get('w', 0),
                               'h': info.get('h', 0), 'vcodec': info.get('vcodec', ''), 'acodec': info.get('acodec', ''),
                               'ext': ext.lower(), 'how': how, 'why': why}
                by_artist.setdefault(aid, []).append(vid)
                if al:
                    by_album.setdefault(al, []).append(vid)
                names.setdefault(aid, artist)
        order = lambda v: (self.albums[videos[v]['album']]['title'].casefold() if videos[v]['album'] else '\uffff',
                           videos[v]['number'] or 999, videos[v]['title'].casefold())
        for d in (by_album, by_artist):
            for k in d:
                d[k].sort(key=order)
        self.videos, self.album_videos, self.artist_videos, self.video_artists = videos, by_album, by_artist, names

    def artists_with_videos(self):
        """every artist, those with only videos too, in the record shop's order: (id, name, albums, tracks, videos)"""
        ids = list(self.artists) + [a for a in self.video_artists if a not in self.artists]
        rows = []
        for i in sorted(ids, key=lambda i: sort_key(self.artists[i]['name'] if i in self.artists else self.video_artists[i])):
            a = self.artists.get(i)
            rows.append((i, a['name'] if a else self.video_artists[i], len(a['albums']) if a else 0,
                         sum(len(self.albums[x]['tracks']) for x in a['albums']) if a else 0, len(self.artist_videos.get(i, []))))
        return rows

    def video_row(self, vid, full=False):
        """a video in /mac/ answers: V, id, title, artist, seconds, album (its id, or empty), width, height; with full=1
        also its path (for /file/), the album's title, its folder and how it plays (direct, remux, transcode)"""
        v = self.videos[vid]
        row = ('V', vid, v['title'], v['artist'], v['secs'], v['album'], v['w'], v['h'])
        if full:
            row += (Wide(v['rel']), self.albums[v['album']]['title'] if v['album'] else '', Wide(v['folder']), v['how'] or '')
        return row

    def video_json(self, vid):
        v = self.videos[vid]
        playable = v['how'] != 'transcode' or VIDEO_TRANSCODE
        return {'id': vid, 'title': v['title'], 'artist': v['artist'], 'artist_id': v['aid'], 'album': v['album'],
                'album_title': self.albums[v['album']]['title'] if v['album'] else '', 'folder': v['folder'], 'path': v['rel'],
                'number': v['number'], 'seconds': v['secs'], 'width': v['w'], 'height': v['h'], 'vcodec': v['vcodec'],
                'acodec': v['acodec'], 'how': v['how'], 'why': v['why'] if playable else v['why'] + '; transcoding is off here',
                'playable': playable, 'video': f'/video/{vid}', 'hls': f'/video/{vid}/index.m3u8' if v['how'] != 'direct' else '',
                'poster': f'/vthumb/{vid}', 'audio': f'/vaudio/{vid}'}

    def track_info(self, tid, probe=True):
        """(number, title, artist, seconds) of a track: from its name, and its tags and length once probed"""
        al, rel = self.tracks[tid]
        album = self.albums[al]
        number, title = tidy_title(os.path.splitext(rel.split('/')[-1])[0], album['artist'])
        path = self.lib.real(rel)
        try:
            st = os.stat(path)
        except (OSError, TypeError):
            return number, title, album['artist'], 0
        key = (st.st_mtime, st.st_size)
        got = self.probed.get(path)
        if (not got or got[:2] != key) and probe:
            got = key + self.probe(path)
            self.probed[path] = got
        if got and got[:2] == key:
            n, t, a, s = got[2:]
            return n or number, t or title, a or album['artist'], s
        return number, title, album['artist'], 0

    def probe(self, path):
        """(number, title, artist, seconds) from the file's tags and length; zeros and blanks for what it lacks"""
        ffprobe = self.lib.ffprobe or shutil.which('ffprobe')
        if ffprobe:
            try:
                out = subprocess.run([ffprobe, '-v', 'error', '-show_entries', 'format=duration:format_tags', '-of', 'json', path],
                                     capture_output=True, text=True, timeout=20)
                f = json.loads(out.stdout or '{}').get('format', {})
                tags = {k.lower(): v for k, v in (f.get('tags') or {}).items()}
                m = re.match(r'\d+', tags.get('track', ''))
                return (int(m.group(0)) if m else 0, tags.get('title', '').strip(), tags.get('artist', '').strip(),
                        int(float(f.get('duration') or 0) + 0.5))
            except (OSError, ValueError, subprocess.SubprocessError):
                pass
        if path.lower().endswith('.mp3'):                      # no ffprobe: the frames give the length
            try:
                with open(path, 'rb') as fh:
                    secs = sum(d for _, _, d in frames(fh.read()))
            except OSError:
                secs = 0
            t = song_title(path)
            artist, _, title = t.partition(' - ') if ' - ' in t else ('', '', t)
            return 0, title, artist, int(secs + 0.5)
        return 0, '', '', 0

    def album_rows(self, ids, full=False):
        """id, title, artist, tracks, seconds; with full=1 (the macOS Play) also the artist's id and the folder"""
        rows = []
        for al in ids:
            a = self.albums[al]
            secs = sum(self.track_info(t, probe=False)[3] for t in a['tracks'])
            rows.append((al, a['title'], a['artist'], len(a['tracks']), secs) + ((a['aid'], Wide(a['rel'])) if full else ()))
        return rows

    def track_rows(self, tids, probe=True, full=False):
        """id, number, title, artist, seconds, album; with full=1 also the path (for /file/), the album's title
        and its folder"""
        rows = []
        for t in tids:
            n, title, artist, secs = self.track_info(t, probe)
            al, rel = self.tracks[t]
            rows.append((t, n, title, artist, secs, al) +
                        ((Wide(rel), self.albums[al]['title'], Wide(self.albums[al]['rel'])) if full else ()))
        return rows


# ---- album covers as bitmaps: Atkinson's dithering for one bit, the Mac's own palette for eight ----------
def mac_palette():
    """the colours of the Macintosh's standard 8-bit palette ('clut' 8, the System's): the 6 x 6 x 6 cube
    of $FF, $CC ... $00 (blue changing fastest, white first), ramps of ten reds, greens, blues and greys
    ($EE, $DD, $BB, $AA, $88, $77, $55, $44, $22, $11), and black last"""
    lv = [255, 204, 153, 102, 51, 0]
    pal = [(r, g, b) for r in lv for g in lv for b in lv][:215]     # the cube without its black
    ramp = [0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11]
    pal += [(v, 0, 0) for v in ramp] + [(0, v, 0) for v in ramp] + [(0, 0, v) for v in ramp] + [(v, v, v) for v in ramp]
    return pal + [(0, 0, 0)]


PALETTE = mac_palette()
GREYS = [(i, PALETTE[i][0]) for i in range(245, 256)] + [(0, 255)]   # the grey ramp, black and white


def nearest(r, g, b):
    """the palette's colour nearest (r, g, b): the cube's, or a grey of the ramp for a colour without hue"""
    lv = [min(5, max(0, (255 - c + 25) // 51)) for c in (r, g, b)]  # 0 is $FF, 5 is $00
    i = lv[0] * 36 + lv[1] * 6 + lv[2]
    i = 255 if i == 215 else i
    if max(r, g, b) - min(r, g, b) < 24:
        y = (r + g + b) // 3
        j = min(GREYS, key=lambda e: abs(e[1] - y))[0]
        pr, pg, pb = PALETTE[i]
        if abs(PALETTE[j][0] - y) * 3 <= abs(pr - r) + abs(pg - g) + abs(pb - b):
            i = j
    return i


def dither1(gray, w, h):
    """one bit a pixel, 1 black, rows of an even number of bytes: Bill Atkinson's dithering (MacPaint's,
    HyperCard's), which passes on three quarters of the error and so keeps light and dark areas clean"""
    px = [float(v) for v in gray]
    rb = ((w + 15) // 16) * 2
    out = bytearray(rb * h)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            old = px[i]
            new = 0.0 if old < 128 else 255.0
            if new == 0.0:
                out[y * rb + x // 8] |= 0x80 >> (x % 8)
            e = (old - new) / 8
            for dx, dy in ((1, 0), (2, 0), (-1, 1), (0, 1), (1, 1), (0, 2)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < w and yy < h:
                    px[yy * w + xx] += e
    return rb, bytes(out)


def dither8(rgb, w, h):
    """indices into the Mac's 8-bit palette, Floyd and Steinberg's error diffusion; rows of an even length"""
    px = [[float(rgb[(y * w + x) * 3 + c]) for c in range(3)] for y in range(h) for x in range(w)]
    rb = w + (w & 1)
    out = bytearray(rb * h)
    for y in range(h):
        for x in range(w):
            p = px[y * w + x]
            r, g, b = (min(255, max(0, int(v + 0.5))) for v in p)
            i = nearest(r, g, b)
            out[y * rb + x] = i
            q = PALETTE[i]
            err = (r - q[0], g - q[1], b - q[2])
            for dx, dy, f in ((1, 0, 7), (-1, 1, 3), (0, 1, 5), (1, 1, 1)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < w and yy < h:
                    t = px[yy * w + xx]
                    for c in range(3):
                        t[c] += err[c] * f / 16
    return rb, bytes(out)


class Covers:
    """an album's cover as a bitmap at a size and depth, made once and kept"""
    def __init__(self, lib):
        self.lib = lib
        self.kept = {}
        self.lock = threading.Lock()

    def source(self, album):
        """the cover's picture: a file beside the songs (cover.jpg, folder.png ...), else the picture inside
        the first song (M4A and FLAC from download services carry it); its path"""
        folder = self.lib.real(album['rel'])
        try:
            names = sorted(os.listdir(folder), key=str.casefold)
        except (OSError, TypeError):
            return None
        pics = [n for n in names if os.path.splitext(n)[1].lower() in ('.jpg', '.jpeg', '.png', '.gif', '.bmp')]
        for want in MAC_COVERS:
            for n in pics:
                if os.path.splitext(n)[0].lower().startswith(want):
                    return os.path.join(folder, n)
        if len(pics) == 1:
            return os.path.join(folder, pics[0])
        songs = self.lib.dirs.get(album['rel'], ([], []))[1]
        return os.path.join(folder, songs[0]) if songs else None

    def get(self, album_id, album, w, h, depth):
        key = (album_id, w, h, depth)
        with self.lock:
            if key in self.kept:
                return self.kept[key]
        got = self.make(album, w, h, depth)
        with self.lock:
            if len(self.kept) > 2000:
                self.kept.clear()
            self.kept[key] = got
        return got

    def make(self, album, w, h, depth):
        """(rowBytes, bits) or None"""
        if not self.lib.ffmpeg:
            return None
        src = self.source(album)
        if not src:
            return None
        vf = (f'scale={w}:{h}:force_original_aspect_ratio=decrease:flags=area,'
              f'pad={w}:{h}:(ow-iw)/2:(oh-ih)/2:color=white')
        fmt = 'gray' if depth == 1 else 'rgb24'
        try:
            out = subprocess.run([self.lib.ffmpeg, '-nostdin', '-v', 'error', '-i', src, '-map', '0:v:0', '-frames:v', '1',
                                  '-vf', vf, '-f', 'rawvideo', '-pix_fmt', fmt, 'pipe:1'], capture_output=True, timeout=30)
        except (OSError, subprocess.SubprocessError):
            return None
        need = w * h * (1 if depth == 1 else 3)
        if out.returncode or len(out.stdout) < need:
            return None
        return dither1(out.stdout[:need], w, h) if depth == 1 else dither8(out.stdout[:need], w, h)


def stream_track(lib, rel, wfile, meta, start, kbps, title, source=None):
    """one track from start seconds, at the rate it plays after a first burst, then the end; source: chunks made
    elsewhere (a video's soundtrack)"""
    path = lib.real(rel)
    if not path:
        return
    out = IcyWriter(wfile, meta)
    out.title = title
    if source is not None:
        pass
    elif rel.lower().endswith('.mp3'):
        with open(path, 'rb') as f:
            b = f.read()

        def skip(chunks):
            gone = 0.0
            for data, secs in chunks:
                if gone + secs <= start:
                    gone += secs
                    continue
                yield data, secs
        source = skip(mp3_chunks(b)) if start else mp3_chunks(b)
    else:
        source = converted_chunks(lib.ffmpeg, path, kbps, start)
    t0 = time.monotonic() - BURST
    due = 0.0
    try:
        for data, secs in source:
            out.write(data)
            due += secs
            ahead = due - (time.monotonic() - t0)
            if ahead > 0:
                time.sleep(ahead)
    finally:
        source.close()


class VideoWork:
    """the ffmpeg work behind the videos (22 November 2026): HLS conversions in the cache folder, the count of
    transcodes running, posters and soundtracks made once and kept. A remux costs a NAS little (its streams are
    copied), a transcode a whole core or more, so only transcodes are counted against VIDEO_TRANSCODES (the video
    card's MPEG-1 is one too); an HLS conversion nobody has fetched from for VIDEO_IDLE seconds is stopped"""
    SEGMENT = re.compile(r'(init\.mp4|seg\d{5}\.m4s)')

    def __init__(self, lib):
        self.lib, self.cache = lib, lib.cache
        self.lock = threading.Lock()
        self.busy = 0                     # transcodes running
        self.jobs = {}                    # video id -> {'dir', 'proc', 'seen', 'how', 'killed'}
        self.posters = {}                 # without a cache folder: the posters in memory
        threading.Thread(target=self.reaper, daemon=True).start()

    def take(self, how, what='this video'):
        """a transcode's turn: None, or (HTTP status, why not); a remux or a file sent as it is needs none"""
        if how != 'transcode':
            return None
        if not VIDEO_TRANSCODE:
            return 415, f'{what} needs transcoding, which is off on this server (VIDEO_TRANSCODE=off)'
        with self.lock:
            if self.busy >= VIDEO_TRANSCODES:
                return 503, f'{self.busy} transcode{"" if self.busy == 1 else "s"} running already (VIDEO_TRANSCODES={VIDEO_TRANSCODES})'
            self.busy += 1
        return None

    def give(self, how):
        if how == 'transcode':
            with self.lock:
                self.busy -= 1

    def running(self):
        with self.lock:
            return self.busy

    def key(self, v, path):
        """what a cached thing was made from: the file as it is now, and the settings that shape it"""
        try:
            st = os.stat(path)
            stamp = (st.st_mtime, st.st_size)
        except OSError:
            stamp = (0, 0)
        return hashlib.sha1(repr((v['rel'], stamp, v['how'], VIDEO_BITRATE, VIDEO_MAX_HEIGHT, VIDEO_PRESET))
                            .encode('utf-8', 'surrogateescape')).hexdigest()[:10]

    # -- HLS: /video/ID/index.m3u8 and its segments
    def hls(self, vid, v, path):
        """the folder of the video's HLS conversion, started if need be: (folder, None) or (None, (status, why))"""
        d = self.cache.path('hls', f'{vid}-{self.key(v, path)}')
        with self.lock:
            job = self.jobs.get(vid)
            if job and job['dir'] == d:
                job['seen'] = time.monotonic()
                return d, None
        if self.complete(d):
            self.cache.used(os.path.join(d, 'index.m3u8'))
            return d, None
        if not self.cache.ok:
            return None, (503, 'HLS needs a folder to write its segments in (MUSIC_CACHE), and this server has none it can write')
        refused = self.take(v['how'])
        if refused:
            return None, refused
        with self.lock:
            old = self.jobs.get(vid)
            if old and old['dir'] == d:                   # another request started it meanwhile
                self.busy -= v['how'] == 'transcode'
                old['seen'] = time.monotonic()
                return d, None
            if old:                                       # a conversion of the file as it was before it changed
                old['killed'] = True
                old['proc'].kill()
            if os.path.isdir(d):
                Cache.drop(d)                             # a conversion that never finished (a restart, a stop)
            os.makedirs(d, exist_ok=True)
            cmd = [self.lib.ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-i', path] + video_codec_args(v, v['how']) + [
                '-f', 'hls', '-hls_time', str(VIDEO_HLS_SECONDS), '-hls_playlist_type', 'event', '-hls_list_size', '0',
                '-hls_segment_type', 'fmp4', '-hls_fmp4_init_filename', 'init.mp4',
                '-hls_segment_filename', os.path.join(d, 'seg%05d.m4s'), '-hls_flags', 'temp_file+independent_segments',
                os.path.join(d, 'index.m3u8')]
            try:
                p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            except OSError as e:
                self.busy -= v['how'] == 'transcode'
                return None, (500, f'ffmpeg: {e.strerror}')
            self.jobs[vid] = job = {'dir': d, 'proc': p, 'seen': time.monotonic(), 'how': v['how'], 'killed': False}
        log(f'video: {v["artist"]} - {v["title"]}: HLS, {v["how"]} ({v["why"]})')
        threading.Thread(target=self.finish, args=(vid, job, v), daemon=True).start()
        return d, None

    @staticmethod
    def complete(d):
        try:
            with open(os.path.join(d, 'index.m3u8'), encoding='utf-8') as f:
                return '#EXT-X-ENDLIST' in f.read()
        except OSError:
            return False

    def finish(self, vid, job, v):
        err = job['proc'].communicate()[1] or b''
        self.give(job['how'])
        with self.lock:
            if self.jobs.get(vid) is job:
                del self.jobs[vid]
        if job['killed'] or job['proc'].returncode:
            if not job['killed']:
                log(f'video: {v["title"]}: ffmpeg failed: ' + (err.decode('utf-8', 'replace').strip().splitlines() or ['?'])[-1][:200])
            Cache.drop(job['dir'])                        # half a conversion is of no use to anyone
        else:
            log(f'video: {v["title"]}: HLS ready')
            with self.lock:
                busy = {j['dir'] for j in self.jobs.values()}
            self.cache.trim(busy)

    def reaper(self):
        while True:
            time.sleep(5)
            with self.lock:
                idle = [j for j in self.jobs.values() if time.monotonic() - j['seen'] > VIDEO_IDLE and j['proc'].poll() is None]
            for j in idle:
                j['killed'] = True
                j['proc'].kill()

    def stop_all(self):
        with self.lock:
            jobs = list(self.jobs.values())
        for j in jobs:
            j['killed'] = True
            j['proc'].kill()

    def playlist(self, vid, d):
        """the playlist once ffmpeg has written it (a transcode's first segment can take seconds), starting at the
        beginning: AVFoundation would start a growing ("event") playlist near its end"""
        end = time.monotonic() + 45
        while time.monotonic() < end:
            try:
                with open(os.path.join(d, 'index.m3u8'), encoding='utf-8') as f:
                    text = f.read()
                if '#EXTINF' in text:
                    break
            except OSError:
                pass
            with self.lock:
                job = self.jobs.get(vid)
                if job:
                    job['seen'] = time.monotonic()
            if not job:
                return None
            time.sleep(0.25)
        else:
            return None
        if '#EXT-X-START' not in text:
            text = text.replace('#EXTM3U\n', '#EXTM3U\n#EXT-X-START:TIME-OFFSET=0,PRECISE=YES\n', 1)
        return text.encode()

    def segment(self, vid, d, name):
        """a segment's bytes, waiting a little for one the player asked for before ffmpeg wrote it"""
        if not self.SEGMENT.fullmatch(name):
            return None
        p = os.path.join(d, name)
        end = time.monotonic() + 20
        while not os.path.isfile(p):
            with self.lock:
                job = self.jobs.get(vid)
                if job:
                    job['seen'] = time.monotonic()
            if not job or time.monotonic() > end:
                return None
            time.sleep(0.25)
        with self.lock:
            if vid in self.jobs:
                self.jobs[vid]['seen'] = time.monotonic()
        try:
            with open(p, 'rb') as f:
                return f.read()
        except OSError:
            return None

    # -- the poster and the soundtrack
    def poster(self, vid, v, path):
        """a JPEG of the picture at a tenth of the video (past its titles), 480 pixels wide at most; kept"""
        key = f'{vid}-{self.key(v, path)}'
        f = self.cache.path('thumbs', key + '.jpg')
        if self.cache.ok and os.path.isfile(f):
            self.cache.used(f)
            with open(f, 'rb') as fh:
                return fh.read()
        if key in self.posters:
            return self.posters[key]
        pic = b''
        for at in (v['secs'] * 0.1, 0):
            try:
                pic = subprocess.run([self.lib.ffmpeg, '-nostdin', '-v', 'error', '-ss', f'{at:.2f}', '-i', path, '-map', '0:v:0',
                                      '-frames:v', '1', '-vf', "scale='min(480,iw)':-2", '-q:v', '4', '-c:v', 'mjpeg',
                                      '-f', 'image2pipe', 'pipe:1'], capture_output=True, timeout=60).stdout
            except (OSError, subprocess.SubprocessError):
                pic = b''
            if pic[:2] == b'\xff\xd8':
                break
        if pic[:2] != b'\xff\xd8':
            return None
        if self.cache.ok:
            try:
                with open(f + '.part', 'wb') as fh:
                    fh.write(pic)
                os.replace(f + '.part', f)
            except OSError:
                pass
        else:
            if len(self.posters) > 500:
                self.posters.clear()
            self.posters[key] = pic
        return pic

    def soundtrack(self, vid, v, path, start, kbps):
        """the video's sound as MP3 at the library's bitrate, half a second at a time from start seconds: from the
        cache once it has been converted whole, else converted as it is sent (and kept, when sent from the start)"""
        f = self.cache.path('vaudio', f'{vid}-{self.key(v, path)}-{kbps}.mp3')
        if self.cache.ok and os.path.isfile(f):
            self.cache.used(f)
            with open(f, 'rb') as fh:
                b = fh.read()

            def from_start(b):                         # frame by frame to the start, then half a second at a time
                chunk, secs, gone = [], 0.0, 0.0
                for off, n, d in frames(b):
                    if gone + d <= start:
                        gone += d
                        continue
                    chunk.append(b[off:off + n])
                    secs += d
                    if secs >= 0.5:
                        yield b''.join(chunk), secs
                        chunk, secs = [], 0.0
                if chunk:
                    yield b''.join(chunk), secs
            return from_start(b)
        src = converted_chunks(self.lib.ffmpeg, path, kbps, start)
        if start or not self.cache.ok:
            return src
        return self.keep(src, f)

    def keep(self, chunks, final):
        part = f'{final}.{threading.get_ident()}.part'
        try:
            out = open(part, 'wb')
        except OSError:
            yield from chunks
            return
        whole = False
        try:
            for data, secs in chunks:
                out.write(data)
                yield data, secs
            whole = True
        finally:
            chunks.close()
            out.close()
            if whole and os.path.getsize(part):
                os.replace(part, final)
                self.cache.trim()
            else:
                Cache.drop(part)


def music_discovery(port, name, tcp_port):
    """answer "AURORA68-MUSIC?" on a UDP port: how Aurora68 Play finds the server. The probe comes as a broadcast
    (Apple's MacTCP sends one) or to the group 239.255.68.70 (Riptide's MacTCP interface sends that where it refuses
    a broadcast). The answer: "AURORA68-MUSIC <tab> 1 <tab> NAME <tab> PORT <tab> IPV4 <tab> IPV6", the server's own
    addresses as numbers (either may be empty), so that Play takes the family its preference names."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(('', port))
    except OSError as e:
        log(f'no discovery on UDP port {port} ({e.strerror}): Aurora68 Play needs the address typed in')
        return
    try:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, socket.inet_aton('239.255.68.70') + socket.inet_aton('0.0.0.0'))
    except OSError:
        pass                                           # no multicast here: broadcasts still answered
    while True:
        try:
            data, peer = s.recvfrom(512)
        except OSError:
            continue
        if data.strip() == b'AURORA68-MUSIC?':
            have = own_addresses()
            answer = f'AURORA68-MUSIC\t1\t{mac_field(name, 40)}\t{tcp_port}\t{have.get("ipv4", "")}\t{have.get("ipv6", "")}\r'
            s.sendto(answer.encode('mac_roman', 'replace'), peer)


# ---- internet radio for every player: the classic Aurora68 Play's own list, and the owner's -------------
# 22 November 2026. The macOS Aurora68 Play's Live page offered nothing to start (the owner: "Pressing Live
# shows nothing... should there not be a list of stations"). The classic Play has carried a list of internet
# radio stations since its Radio menu (play/core/stations.c); the server hands the same list to every
# player at /api/radio (and /mac/radio), so the stations are the same everywhere. RADIO_BUILTIN is that
# list's stations, in the stations file's own format; test_server.py reads stations.c and checks that
# the two agree, so a station added there and not here fails the test. The owner's own stations: a
# stations file in the music folder ("Aurora68 Play Stations" or radio.txt, or MUSIC_RADIO=PATH), the
# classic Play's own file copied there will do; its stations come after the built-in ones.
RADIO_BUILTIN = '''\
[SomaFM]
Groove Salad = http://ice2.somafm.com/groovesalad-128-mp3
Drone Zone = http://ice2.somafm.com/dronezone-128-mp3
Secret Agent = http://ice2.somafm.com/secretagent-128-mp3
Lush = http://ice2.somafm.com/lush-128-mp3
Space Station Soma = http://ice2.somafm.com/spacestation-128-mp3
Indie Pop Rocks! = http://ice2.somafm.com/indiepop-128-mp3
Beat Blender = http://ice2.somafm.com/beatblender-128-mp3
Deep Space One = http://ice2.somafm.com/deepspaceone-128-mp3
DEF CON Radio = http://ice2.somafm.com/defcon-128-mp3
Left Coast 70s = http://ice2.somafm.com/seventies-128-mp3
Underground 80s = http://ice2.somafm.com/u80s-128-mp3
Sonic Universe = http://ice2.somafm.com/sonicuniverse-128-mp3
[Listener-supported]
Radio Paradise Main = http://stream.radioparadise.com/mp3-192
Radio Paradise Mellow = http://stream.radioparadise.com/mellow-192
Radio Paradise Rock = http://stream.radioparadise.com/rock-192
Radio Paradise Global = http://stream.radioparadise.com/global-192
KEXP Seattle = http://kexp-mp3-128.streamguys1.com/kexp128.mp3
WFMU Freeform = http://stream0.wfmu.org/freeform-128k.mp3
WWOZ New Orleans = http://wwoz-sc.streamguys1.com/wwoz-hi.mp3
Radio Swiss Jazz = http://stream.srg-ssr.ch/m/rsj/mp3_128
Radio Swiss Classic = http://stream.srg-ssr.ch/m/rsc_de/mp3_128
Radio Swiss Pop = http://stream.srg-ssr.ch/m/rsp/mp3_128
[By country/Netherlands]
NPO Radio 2 = http://icecast.omroep.nl/radio2-bb-mp3
NPO 3FM = http://icecast.omroep.nl/3fm-bb-mp3
NPO Klassiek = http://icecast.omroep.nl/radio4-bb-mp3
[By country/Switzerland]
Radio Swiss Jazz = http://stream.srg-ssr.ch/m/rsj/mp3_128
Radio Swiss Classic = http://stream.srg-ssr.ch/m/rsc_de/mp3_128
Radio Swiss Pop = http://stream.srg-ssr.ch/m/rsp/mp3_128
[By country/Austria]
Ö1 = http://ors-sn05.ors-shoutcast.at/oe1-q2a
[By country/United Kingdom]
Classic FM = http://media-ice.musicradio.com/ClassicFMMP3
BBC World Service = http://stream.live.vc.bbcmedia.co.uk/bbc_world_service
[By country/France]
Radio Nova = http://novazz.ice.infomaniak.ch/novazz-128.mp3
[By country/Germany]
90s90s Pop = http://streams.90s90s.de/pop/mp3-128/
[By country/Greece]
ERT Kosmos = http://radiostreaming.ert.gr/ert-kosmos
[By country/New Zealand]
RNZ Concert = http://radionz-ice.streamguys.com/concert.mp3
[By country/United States]
KEXP Seattle = http://kexp-mp3-128.streamguys1.com/kexp128.mp3
WFMU Freeform = http://stream0.wfmu.org/freeform-128k.mp3
WWOZ New Orleans = http://wwoz-sc.streamguys1.com/wwoz-hi.mp3
181.fm The Beatles = http://listen.181fm.com/181-beatles_128k.mp3
181.fm Classic Hits = http://listen.181fm.com/181-greatoldies_128k.mp3
'''
RADIO_FILES = ('Aurora68 Play Stations', 'radio.txt')    # in the music folder: the owner's own stations


def parse_stations(text):
    """the stations of a stations file, as the classic Play reads one (stations.c): [Group] or [Group/Sub]
    starts a group, [] ends it, "Name = http://..." is a station, # and ; lines are notes"""
    out, group = [], ''
    for line in text.replace('\r\n', '\n').replace('\r', '\n').split('\n'):
        line = line.strip()
        if not line or line[0] in '#;':
            continue
        if line.startswith('[') and line.endswith(']'):
            group = '/'.join(p.strip() for p in line[1:-1].split('/') if p.strip())
            continue
        n, eq, u = line.partition('=')
        n, u = n.strip(), u.strip()
        if eq and n and u.lower().startswith(('http://', 'https://')):
            out.append({'group': group, 'name': n[:63], 'url': u[:255]})
    return out


class Radio:
    """the stations every player offers: RADIO_BUILTIN, then the owner's file's (read again when it changes)"""
    def __init__(self, root):
        self.root = root
        self.builtin = parse_stations(RADIO_BUILTIN)
        self.seen, self.extra = None, []

    def path(self):
        if os.environ.get('MUSIC_RADIO'):
            return os.environ['MUSIC_RADIO']
        for f in RADIO_FILES:
            p = os.path.join(self.root, f)
            if os.path.isfile(p):
                return p
        return None

    def stations(self):
        p = self.path()
        try:
            key = (p, os.stat(p).st_mtime) if p else None
        except OSError:
            key = None
        if key != self.seen:
            self.seen, self.extra = key, []
            if key:
                try:
                    raw = open(p, 'rb').read(256 * 1024)
                    try:
                        text = raw.decode('utf-8')
                    except UnicodeDecodeError:
                        text = raw.decode('mac_roman')      # the classic Play's own file
                    self.extra = [s for s in parse_stations(text) if s not in self.builtin]
                    log(f'radio: {len(self.extra)} stations of your own from {p}')
                except OSError as e:
                    log(f'radio: cannot read {p}: {e.strerror}')
        return self.builtin + self.extra


# ---- players as devices: who is online, what each plays, and commands sent to them ---------------------
# 22 November 2026. The owner: "Perhaps other devices running Aurora Player need to broadcast they are
# online for redirection". Every Aurora68 Play (macOS and classic) announces itself here every
# DEVICE_EVERY seconds; one not heard for DEVICE_TTL seconds is offline. Any controller (the web panel,
# another player) lists them and sends one a command ("play this album there"); the device fetches its
# commands by a long poll (it waits here up to 20 seconds for one), so no player has to accept
# connections: a Mac behind its own firewall, or a classic Mac with MacTCP, works the same. The Live
# channel is listed as the device "live". The interface is the README's "Devices"; the shared design
# that the macOS and the classic Play were written against says the same.
DEVICE_EVERY = 20
DEVICE_TTL = float(os.environ.get('MUSIC_DEVICE_TTL', 60))   # shorter in test_server.py
DEVICE_ID = re.compile(r'[A-Za-z0-9_-]{1,64}')
DEVICE_KINDS = ('macos', 'classic', 'browser')
DEVICE_ACTIONS = ('play', 'add', 'next', 'pause', 'resume', 'skip', 'previous', 'seek', 'volume', 'stop')
DEVICE_MAX = 64                 # devices remembered; the longest silent is forgotten first
DEVICE_QUEUE = 32               # commands waiting for one device; the oldest are dropped
DEVICE_TRACKS = 2000            # tracks in one command (the whole library shuffled, on a big one, is cut)
DEVICE_PROBE = 40               # a command of up to this many tracks has their tags and lengths read


def clean_text(s, n=200):
    return ' '.join(str(s if s is not None else '').split())[:n]


class Devices:
    def __init__(self, ttl=None):
        self.ttl = DEVICE_TTL if ttl is None else ttl
        self.lock = threading.Condition()
        self.known = {}              # id -> {'id', 'name', 'kind', 'version', 'seen', 'when', 'state', 'queue', 'n'}

    def online(self, d):
        return time.monotonic() - d['seen'] < self.ttl

    def announce(self, did, name, kind, version, who='', video=None):
        """a device says it is there, and whether it shows videos (video=True: the macOS Play, a browser; False or
        not said: sound only, as the classic Play is until the video card's program exists); an error in words, or None"""
        if not isinstance(did, str) or not DEVICE_ID.fullmatch(did) or did == 'live':
            return 'id: 1 to 64 letters, digits, - and _ (and not "live")'
        if kind not in DEVICE_KINDS:
            return 'kind: ' + ', '.join(DEVICE_KINDS)
        name = clean_text(name, 64) or did
        with self.lock:
            d = self.known.get(did)
            back = d is None or not self.online(d)
            if d is None:
                if len(self.known) >= DEVICE_MAX:
                    del self.known[min(self.known, key=lambda k: self.known[k]['seen'])]
                d = self.known[did] = {'id': did, 'state': {}, 'queue': [], 'n': 0}
            d.update(name=name, kind=kind, version=clean_text(version, 16), seen=time.monotonic(), when=time.time(),
                     video=bool(video))
        if back:
            log(f'devices: {name} ({kind}{", " + who if who else ""}) is online')
        return None

    def set_state(self, did, st):
        with self.lock:
            d = self.known.get(did)
            if d is None:
                return 'announce first'
            out = {'playing': bool(st.get('playing'))}
            for k in ('title', 'artist', 'album', 'folder'):
                out[k] = clean_text(st.get(k))
            for k in ('position', 'length'):
                try:
                    out[k] = round(max(0.0, float(st.get(k) or 0)), 1)
                except (TypeError, ValueError):
                    out[k] = 0.0
            try:
                out['volume'] = max(0, min(100, int(st.get('volume', 100))))
            except (TypeError, ValueError):
                out['volume'] = 100
            out['at'] = int(time.time())
            if st.get('video'):                    # what plays is a music video (its sound, on a player without a screen)
                out['video'] = True
            d['state'] = out
            d['seen'], d['when'] = time.monotonic(), time.time()
        return None

    def listing(self):
        with self.lock:
            return [{'id': d['id'], 'name': d['name'], 'kind': d['kind'], 'version': d['version'],
                     'online': self.online(d), 'last_seen': int(d['when']), 'state': dict(d['state']), 'is_live': False,
                     'video': d.get('video', False)}
                    for d in sorted(self.known.values(), key=lambda d: d['name'].casefold())]

    def shows_video(self, did):
        with self.lock:
            return bool(self.known.get(did, {}).get('video'))

    def send(self, did, cmd):
        """queue a command for a device: (its number, None) or (None, an error in words)"""
        with self.lock:
            d = self.known.get(did)
            if d is None:
                return None, 'no such device'
            if not self.online(d):
                return None, f'{d["name"]} is not online'
            d['n'] += 1
            d['queue'] = d['queue'][-(DEVICE_QUEUE - 1):] + [dict(cmd, id=d['n'])]
            self.lock.notify_all()
            return d['n'], None

    def take(self, did, wait):
        """the commands waiting for a device, waiting up to `wait` seconds for one; (list, None) or (None, error).
        Asking counts as being heard from"""
        end = time.monotonic() + max(0.0, min(float(wait), 20.0))
        with self.lock:
            while True:
                d = self.known.get(did)
                if d is None:
                    return None, 'announce first'
                d['seen'], d['when'] = time.monotonic(), time.time()
                if d['queue'] or time.monotonic() >= end:
                    out, d['queue'] = d['queue'], []
                    return out, None
                self.lock.wait(end - time.monotonic())


# ---- the web server ------------------------------------------------------------------------------
# The control panel ("/"): the library as cover pictures (artists, albums, songs), the live channel's
# Now Playing, queue and listeners. Plain HTML and a little JavaScript, no outside files, so it works
# with no internet; it polls /api/now every two seconds.
PANEL = r'''<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>__NAME__</title>
<meta name="color-scheme" content="light"><link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32' aria-hidden='true'%3E%3Cdefs%3E%3CclipPath id='lc'%3E%3Crect x='1.25' y='1.25' width='29.5' height='29.5' rx='7'/%3E%3C/clipPath%3E%3ClinearGradient id='ls' x1='0' y1='0' x2='0' y2='1'%3E%3Cstop offset='0' stop-color='%23060a22'/%3E%3Cstop offset='.7' stop-color='%2316225a'/%3E%3Cstop offset='1' stop-color='%2322306e'/%3E%3C/linearGradient%3E%3ClinearGradient id='la' x1='0' y1='0' x2='1' y2='0'%3E%3Cstop offset='.08' stop-color='%2361bb46'/%3E%3Cstop offset='.28' stop-color='%23fdb827'/%3E%3Cstop offset='.44' stop-color='%23f5821f'/%3E%3Cstop offset='.6' stop-color='%23e03a3e'/%3E%3Cstop offset='.77' stop-color='%23963d97'/%3E%3Cstop offset='.94' stop-color='%23009ddc'/%3E%3C/linearGradient%3E%3ClinearGradient id='lf' x1='0' y1='0' x2='0' y2='1'%3E%3Cstop offset='.18' stop-color='%23fff'/%3E%3Cstop offset='.45' stop-color='%23fff' stop-opacity='.8'/%3E%3Cstop offset='.8' stop-color='%23fff' stop-opacity='0'/%3E%3C/linearGradient%3E%3Cmask id='lm'%3E%3Crect width='32' height='32' fill='url(%23lf)'/%3E%3C/mask%3E%3Cpattern id='lr' width='1.8' height='32' patternUnits='userSpaceOnUse'%3E%3Crect x='.5' width='.7' height='32' fill='%23060a22' fill-opacity='.35'/%3E%3C/pattern%3E%3Cfilter id='lb' x='-10%' y='-10%' width='120%' height='120%'%3E%3CfeGaussianBlur stdDeviation='.35'/%3E%3C/filter%3E%3C/defs%3E%3Cg clip-path='url(%23lc)'%3E%3Crect width='32' height='32' fill='url(%23ls)'/%3E%3Cg fill='%23fff'%3E%3Ccircle cx='7' cy='4.2' r='.4'/%3E%3Ccircle cx='16' cy='3.2' r='.3'/%3E%3Ccircle cx='26.5' cy='4' r='.38'/%3E%3Ccircle cx='22.5' cy='21' r='.28'/%3E%3Ccircle cx='5' cy='19' r='.3'/%3E%3C/g%3E%3Cg mask='url(%23lm)' filter='url(%23lb)'%3E%3Cpath d='M-2 10 C3 6 8 9.5 13 8 S22 4 27 6.5 S32 9 34 8 V27 H-2Z' fill='url(%23la)'/%3E%3Cpath d='M-2 10 C3 6 8 9.5 13 8 S22 4 27 6.5 S32 9 34 8 V27 H-2Z' fill='url(%23lr)'/%3E%3C/g%3E%3Cpath d='M-2 10 C3 6 8 9.5 13 8 S22 4 27 6.5 S32 9 34 8' fill='none' stroke='%23fff' stroke-opacity='.55' stroke-width='.7' filter='url(%23lb)'/%3E%3Cpath d='M-2 26 C5 23.5 10 26.5 16 24.5 S27 23 34 25.5 V34 H-2Z' fill='%2305081a'/%3E%3C/g%3E%3Crect x='1.25' y='1.25' width='29.5' height='29.5' rx='7' fill='none' stroke='%23000' stroke-width='1.2'/%3E%3C/svg%3E"><style>
/* 22 November 2026: the look of tools/recovery/page.html, the Setup and Recovery page (the owner: "an 80s Apple style
   and colour" for the project's web pages): the Macintosh of 1984-89, black on white over the grey desktop pattern,
   windows with pinstriped title bars and a close box, a hard drop shadow, rounded buttons with the default ring,
   alert-box toasts, Chicago and Monaco where the computer has them (nothing is fetched from the network), and the
   six colours of the period under the top bar and one for each window. The logo and the favicon are
   docs/img/aurora68-logo.svg, the same as that page's. */
:root{--g:#61bb46;--y:#fdb827;--o:#f5821f;--r:#e03a3e;--p:#963d97;--b:#009ddc;--ui:Chicago,ChicagoFLF,Charcoal,"Geneva",-apple-system,"Segoe UI",Helvetica,Arial,sans-serif;--mono:Monaco,"Andale Mono",Menlo,Consolas,monospace}
*{box-sizing:border-box}html{background:#fff repeating-conic-gradient(#000 0 25%,#fff 0 50%) 0 0/2px 2px}
body{margin:0;font:14px/1.45 var(--ui);color:#000;min-height:100vh}
header{background:#fff;border-bottom:2px solid #000;position:sticky;top:0;z-index:3}
.top{display:flex;align-items:center;gap:10px;flex-wrap:wrap;padding:6px 12px}
.logo{width:28px;height:28px;flex:none;display:block}
header h1{font-size:15px;margin:0;font-weight:700;flex:1 1 160px;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.rainbow{height:6px;background:linear-gradient(90deg,var(--g) 0 16.67%,var(--y) 0 33.33%,var(--o) 0 50%,var(--r) 0 66.67%,var(--p) 0 83.33%,var(--b) 0)}
header select,header input,#gq{font:13px var(--ui);color:#000;background:#fff;border:1px solid #000;border-radius:0;padding:4px 7px}
header select{max-width:14em}header input{flex:1 1 14em;min-width:9em}input:focus,select:focus{outline:none;box-shadow:inset 0 0 0 1px #000}
main{display:grid;grid-template-columns:minmax(0,1fr) 22em;gap:18px;padding:18px 14px 40px;max-width:80em;margin:auto}
@media (max-width:52em){main{grid-template-columns:1fr}aside{order:-1}#np .art{width:55%;margin:0 auto .8em}}
aside{display:flex;flex-direction:column;gap:18px}
.card{background:#fff;border:1px solid #000;box-shadow:2px 2px 0 #000;padding:0 14px 14px;min-width:0}
.card h2{font-size:13px;margin:0 -14px 12px;padding:2px 0;text-align:center;border-bottom:1px solid #000;position:relative;font-weight:700;
 background:repeating-linear-gradient(#fff 0 1px,#000 1px 2px) 0 3px/100% calc(100% - 6px) no-repeat}
.card h2::before{content:"";position:absolute;left:8px;top:50%;width:11px;height:11px;margin-top:-6px;background:#fff;border:1px solid #000;box-shadow:0 0 0 2px #fff}
.card h2 span{background:#fff;padding:0 8px;font-weight:700}
.card h2 span::before{content:"";display:inline-block;width:9px;height:9px;border:1px solid #000;margin-right:6px;vertical-align:-1px;background:var(--c,#fff)}
h3{font-size:13px;margin:1.2em 0 .6em;text-decoration:underline;font-weight:700}
.crumbs{margin-bottom:.8em}.crumbs a{cursor:pointer;text-decoration:underline}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(8.5em,1fr));gap:12px}
.tile{cursor:pointer;padding:4px;border:1px solid transparent;border-radius:6px}.tile:hover{border-color:#000}.tile:active{background:#000;color:#fff}
.tile .art{aspect-ratio:1;border:1px solid #000;background:#fff center/cover;display:flex;align-items:center;justify-content:center;font-size:2em}
.tile .t{font-weight:700;margin-top:.35em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.tile .s{font-size:12px}
.tile .art.vid{aspect-ratio:16/9;color:#fff;text-shadow:0 0 3px #000}
.row{display:flex;align-items:center;gap:.6em;padding:.35em .2em;border-bottom:1px dotted #000}.row:last-child{border:0}
.row .n{width:1.8em;text-align:right;font:12px var(--mono)}.row .t{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.row.cur .t,.dev.sel .t{font-weight:700;text-decoration:underline}
.dev{cursor:pointer}.dot{width:9px;height:9px;border:1px solid #000;flex:none;background:#fff}.dot.on{background:var(--g)}
button{font:inherit;font-size:13px;color:#000;background:#fff;border:1px solid #000;border-radius:9px;padding:3px 12px;cursor:pointer;min-height:24px}
button:active{background:#000;color:#fff}button.p{box-shadow:0 0 0 1px #fff,0 0 0 4px #000;margin:4px 6px 4px 4px;font-weight:700}
button.i{border:0;background:none;padding:2px 6px;min-height:0;border-radius:4px}button.i:active{background:#000;color:#fff}
.acts{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:.2em 0 1em}
#np .art{width:100%;aspect-ratio:1;border:1px solid #000;background:#fff center/cover;margin-bottom:.8em}
#np .t{font-weight:700;font-size:15px}
.bar{height:12px;border:1px solid #000;margin:.7em 0 .2em;background:#fff}.bar i{display:block;height:100%;width:0;background:repeating-linear-gradient(90deg,#000 0 2px,#fff 2px 4px)}
.times{display:flex;justify-content:space-between;font:12px var(--mono)}
.ctl{display:flex;justify-content:center;align-items:center;gap:8px;margin:.8em 0 .4em;flex-wrap:wrap}.ctl button.i{font-size:18px}
.tog button.on{background:#000;color:#fff}
.small{font-size:12px}code{font:12px var(--mono);word-break:break-all}a{color:#000}
#msg{position:fixed;left:50%;top:50%;transform:translate(-50%,-50%);background:#fff;color:#000;padding:14px 18px;border:1px solid #000;outline:3px double #000;
 outline-offset:3px;font-size:13px;max-width:88vw;z-index:6;box-shadow:4px 4px 0 #000;opacity:0;pointer-events:none;transition:opacity .2s}
#vbox{position:fixed;inset:0;background:#fff repeating-conic-gradient(#000 0 25%,#fff 0 50%) 0 0/2px 2px;display:flex;align-items:center;justify-content:center;padding:14px;z-index:5}
#vbox>.card{width:min(60em,96vw)}#vbox video{width:100%;max-height:70vh;background:#000;border:1px solid #000;display:block}
.vh{display:flex;justify-content:space-between;align-items:center;margin-bottom:.5em;gap:8px}
</style></head><body>
<header><div class="top"><svg class="logo" viewBox="0 0 32 32" aria-hidden="true"><defs><clipPath id="lc"><rect x="1.25" y="1.25" width="29.5" height="29.5" rx="7"/></clipPath><linearGradient id="ls" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#060a22"/><stop offset=".7" stop-color="#16225a"/><stop offset="1" stop-color="#22306e"/></linearGradient><linearGradient id="la" x1="0" y1="0" x2="1" y2="0"><stop offset=".08" stop-color="#61bb46"/><stop offset=".28" stop-color="#fdb827"/><stop offset=".44" stop-color="#f5821f"/><stop offset=".6" stop-color="#e03a3e"/><stop offset=".77" stop-color="#963d97"/><stop offset=".94" stop-color="#009ddc"/></linearGradient><linearGradient id="lf" x1="0" y1="0" x2="0" y2="1"><stop offset=".18" stop-color="#fff"/><stop offset=".45" stop-color="#fff" stop-opacity=".8"/><stop offset=".8" stop-color="#fff" stop-opacity="0"/></linearGradient><mask id="lm"><rect width="32" height="32" fill="url(#lf)"/></mask><pattern id="lr" width="1.8" height="32" patternUnits="userSpaceOnUse"><rect x=".5" width=".7" height="32" fill="#060a22" fill-opacity=".35"/></pattern><filter id="lb" x="-10%" y="-10%" width="120%" height="120%"><feGaussianBlur stdDeviation=".35"/></filter></defs><g clip-path="url(#lc)"><rect width="32" height="32" fill="url(#ls)"/><g fill="#fff"><circle cx="7" cy="4.2" r=".4"/><circle cx="16" cy="3.2" r=".3"/><circle cx="26.5" cy="4" r=".38"/><circle cx="22.5" cy="21" r=".28"/><circle cx="5" cy="19" r=".3"/></g><g mask="url(#lm)" filter="url(#lb)"><path d="M-2 10 C3 6 8 9.5 13 8 S22 4 27 6.5 S32 9 34 8 V27 H-2Z" fill="url(#la)"/><path d="M-2 10 C3 6 8 9.5 13 8 S22 4 27 6.5 S32 9 34 8 V27 H-2Z" fill="url(#lr)"/></g><path d="M-2 10 C3 6 8 9.5 13 8 S22 4 27 6.5 S32 9 34 8" fill="none" stroke="#fff" stroke-opacity=".55" stroke-width=".7" filter="url(#lb)"/><path d="M-2 26 C5 23.5 10 26.5 16 24.5 S27 23 34 25.5 V34 H-2Z" fill="#05081a"/></g><rect x="1.25" y="1.25" width="29.5" height="29.5" rx="7" fill="none" stroke="#000" stroke-width="1.2"/></svg><h1>__NAME__</h1><label class="small" for="target">Play on</label><select id="target" onchange="choose(this.value)"><option value="live">Live</option></select>
<button onclick="showRadio()" title="Internet radio stations, for a player">Radio</button><button id="vidbtn" style="display:none" onclick="showVideos()" title="The library's music videos">Videos</button><button id="getbtn" style="display:none" onclick="showGet()">Get albums</button>
<input id="q" type="search" placeholder="Filter artists and albums"></div><div class="rainbow"></div></header>
<main>
<section class="card" style="--c:var(--g)"><h2><span>Library</span></h2><div class="crumbs" id="crumbs"></div><div id="acts" class="acts"></div><div id="view"></div></section>
<aside>
<div id="np" class="card" style="--c:var(--y)"><h2><span id="nph">Live: now playing</span></h2><div class="art" id="npart"></div><div class="t" id="npt">Nothing queued</div>
<div class="s" id="nps">Choose an album, an artist or a song</div><div class="bar"><i id="npbar"></i></div>
<div class="times"><span id="npe">0:00</span><span id="npl"></span></div>
<div class="ctl"><button class="i" title="Previous" onclick="act('previous')">&#9198;</button>
<button class="p" id="pp" title="Play or pause" onclick="pp()">&#9208;</button>
<button class="i" title="Next" onclick="act('skip')">&#9197;</button></div>
<div class="ctl tog"><button id="rep" onclick="act('repeat')" title="Play the queue again when it ends">Repeat</button>
<button onclick="act('shuffle')" title="Shuffle what is still to come">Shuffle rest</button>
<button onclick="act('clear')" title="Empty the queue">Clear</button></div></div>
<div class="card" style="--c:var(--o)"><h2><span>Devices</span></h2><div id="devs" class="small">Only Live: an Aurora68 Play on a Mac shows up here when it runs</div></div>
<div class="card" style="--c:var(--r)"><h2><span>Live queue</span></h2><div id="queue" class="small">Empty</div></div>
<div id="dl" class="card" style="display:none;--c:var(--p)"><h2><span>Downloads</span></h2><div id="dlist" class="small">Nothing yet</div></div>
<div class="card" style="--c:var(--b)"><h2><span>Listening</span></h2><div id="who" class="small">Nobody yet</div>
<p class="small">On the Mac: Radio &gt; Open URL&hellip; and one of these, or the Live station in the
stations file:</p><div id="liveurl" class="small"></div><button id="here" onclick="here()">Listen here</button>
<audio id="aud" preload="none"></audio></div>
</aside></main><div id="msg"></div>
<div id="vbox" style="display:none" onclick="if(event.target==this)closeV()"><div class="card" style="--c:var(--p)"><h2><span id="vt">Video</span></h2>
<div class="vh"><span class="small" id="vwhy"></span><button title="Close" onclick="closeV()">Close</button></div>
<video id="vid" controls playsinline></video><div class="acts" id="von" style="margin:.8em 0 0"></div></div></div>
<script>
let S={}, path='', listing=null;
const $=id=>document.getElementById(id), esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const enc=p=>p.split('/').map(encodeURIComponent).join('/'), art=d=>`url('/art?dir=${encodeURIComponent(d)}')`;
const mmss=s=>{s=Math.max(0,Math.round(s||0));return Math.floor(s/60)+':'+String(s%60).padStart(2,'0')};
function toast(t){const m=$('msg');m.textContent=t;m.style.opacity=1;clearTimeout(m.h);m.h=setTimeout(()=>m.style.opacity=0,1800)}
// what the library's buttons and the transport play on: Live (the shared channel) or a player (the README's "Devices")
let T='live', D=[];
const TO=['play','add','next','pause','resume','skip','previous','seek','volume','stop'];
async function act(action,o={}){const dev=T!='live'&&TO.includes(action);
 const r=await fetch(dev?'/api/devices/'+encodeURIComponent(T)+'/command':'/api/live',{method:'POST',headers:{'Content-Type':'application/json','X-Aurora':'1'},body:JSON.stringify({action,...o})});
 const j=await r.json();if(j.error)toast(j.error);else if(o.toast)toast(o.toast+(dev?' on '+dname(T):''));poll()}
const dname=id=>(D.find(d=>d.id==id)||{name:id}).name;
function choose(id){T=id;$('target').value=id;show();devs()}
function devs(){const sel=$('target'),on_=D.filter(d=>d.online);
 if(!on_.some(d=>d.id==T))T='live';
 sel.innerHTML=on_.map(d=>`<option value="${esc(d.id)}">${esc(d.is_live?'Live':d.name)}</option>`).join('');sel.value=T;
 $('devs').innerHTML=D.map(d=>{const s=d.state||{},np=s.title?(s.artist?s.artist+' - ':'')+s.title:(d.online?'Not playing':'Offline');
  return `<div class="row dev${d.id==T?' sel':''}" ${d.online?on('choose('+J(d.id)+')'):''} title="${d.online?'Play on '+esc(d.name):'Not heard from for a minute'}">`+
  `<span class="dot${d.online?' on':''}"></span><span class="t">${esc(d.is_live?'Live ('+d.listeners+' listening)':d.name)}<br><span class="small">${esc(np)}${s.playing||!d.online?'':s.title?' (paused)':''}</span></span>`+
  `<span class="small">${esc(d.kind)}</span></div>`}).join('')}
function show(){const d=D.find(x=>x.id==T);document.querySelector('#np .tog').style.display=T=='live'||!d?'':'none';if(T=='live'||!d)return live_np();const s=d.state||{};
 $('nph').textContent=d.name+': now playing';$('npt').textContent=s.title||'Nothing playing';$('nps').textContent=s.artist?(s.artist+(s.album?' - '+s.album:'')):(s.album||'');
 $('npart').style.backgroundImage=s.folder?art(s.folder):'none';$('npbar').style.width=s.length?Math.min(100,100*s.position/s.length)+'%':'0';
 $('npe').textContent=s.title?mmss(s.position):'';$('npl').textContent=s.length?mmss(s.length):'';$('pp').innerHTML=s.playing?'&#9208;':'&#9654;'}
function pp(){if(T=='live')return act(S.paused?'resume':'pause');const d=D.find(x=>x.id==T);act(d&&d.state&&d.state.playing?'pause':'resume')}
let R=[];
async function showRadio(){window.artist=null;$('crumbs').innerHTML=`<a ${on("open_('')")}>Library</a> / Radio`;$('acts').innerHTML='';
 R=(await (await fetch('/api/radio')).json()).stations;let h='',g=null;
 R.forEach((s,i)=>{if(s.group!=g){g=s.group;h+=`<h3>${esc(g||'Stations')}</h3>`}
  h+=`<div class="row"><span class="t">${esc(s.name)}</span><span class="small">${esc(s.url.replace(/^https?:\/\//,'').split('/')[0])}</span>`+
  `<button class="i" title="Play on the chosen player" ${on('radio('+i+')')}>&#9654;</button></div>`});
 $('view').innerHTML='<p class="small">Internet radio plays on a player (choose one in Play on): the Live channel plays the library.</p>'+h}
function radio(i){if(T=='live'){toast('Choose a player in Play on: Live plays the library');return}act('play',{what:'r:'+i,toast:'Playing '+R[i].name})}
const on=js=>`onclick="${esc(js)}"`, J=JSON.stringify;     // every handler escaped once, here
const join=(d,x)=>d?d+'/'+x:x;
async function open_(p){path=p;window.artist=null;const r=await fetch('/api/list?dir='+encodeURIComponent(p));listing=await r.json();render()}
function crumbs(){let h=`<a ${on("open_('')")}>Library</a>`,acc='';
 for(const c of path?path.split('/'):[]){acc=join(acc,c);h+=` / <a ${on('open_('+J(acc)+')')}>${esc(c)}</a>`}$('crumbs').innerHTML=h}
function tile(title,sub,dir,js){return `<div class="tile" ${on(js)}><div class="art" style="background-image:${art(dir)}"></div>`+
 `<div class="t" title="${esc(title)}">${esc(title)}</div><div class="s">${esc(sub)}</div></div>`}
function buttons(what,label){return `<button class="p" ${on('act("play",{what:'+J(what)+'})')}>&#9654; ${esc(label)}</button>`+
 `<button ${on('act("play",{what:'+J(what)+',shuffle:1})')}>Shuffle</button>`+
 `<button ${on('act("add",{what:'+J(what)+',toast:"Added to the queue"})')}>Add to queue</button>`}
function render(){crumbs();const L=listing,f=$('q').value.toLowerCase(),here=path,A=window.artist&&L.artists&&L.artists.find(x=>x.name==window.artist);
 $('acts').innerHTML=A?buttons('s:'+A.station.slice(3),'Play '+A.name)+`<button ${on('window.artist=null;render()')}>All artists</button>`
   :buttons('s:'+L.station.slice(3),here?'Play this':'Play everything');
 let h='';
 if(A)h+='<div class="grid">'+A.folders.map(x=>tile(x.startsWith(A.name+' - ')?x.slice(A.name.length+3):x,'',join(here,x),'open_('+J(join(here,x))+')')).join('')+'</div>';
 else if(L.artists)h+='<div class="grid">'+L.artists.filter(a=>a.name.toLowerCase().includes(f)||a.folders.some(x=>x.toLowerCase().includes(f)))
   .map(a=>tile(a.name,a.folders.length+' album'+(a.folders.length>1?'s':''),join(here,a.folders[0]),'showArtist('+J(a.name)+')')).join('')+'</div>';
 else if(L.folders.length)h+='<div class="grid">'+L.folders.filter(x=>x.toLowerCase().includes(f)).map(x=>tile(x,'',join(here,x),'open_('+J(join(here,x))+')')).join('')+'</div>';
 if(L.files.length&&!A)h+=(h?'<h3>Songs</h3>':'')+L.files.map((x,i)=>{const t='t:'+join(here,x);
   return `<div class="row"><span class="n">${i+1}</span><span class="t">${esc(x.replace(/\.[^.]+$/,''))}</span>`+
   `<button class="i" title="Play now" ${on('act("play",{what:'+J(t)+'})')}>&#9654;</button>`+
   `<button class="i" title="Play next" ${on('act("next",{what:'+J(t)+',toast:"Plays next"})')}>&#8618;</button>`+
   `<button class="i" title="Add to queue" ${on('act("add",{what:'+J(t)+',toast:"Added"})')}>+</button></div>`}).join('');
 h+=vgrid(A?A.videos:L.videos);
 $('view').innerHTML=h||'<p class="small">Nothing here.</p>'}
function showArtist(n){window.artist=n;render()}
// music videos (22 November 2026): posters, played here in the page (the file as it is, HLS in Safari, else a fragmented
// MP4 made as it is sent), or sent to the chosen player (a Mac shows it; the SE plays its sound; Live plays its sound)
let V={};
function vtile(v){V[v.id]=v;return `<div class="tile" ${on('watch('+J(v.id)+')')}><div class="art vid" style="background-image:url('${v.poster}')">&#9654;</div>`+
 `<div class="t" title="${esc(v.title)}">${esc(v.title)}</div><div class="s">${esc(v.artist)} &middot; ${mmss(v.seconds)}</div></div>`}
const vgrid=vs=>vs&&vs.length?'<h3>Videos</h3><div class="grid">'+vs.map(vtile).join('')+'</div>':'';
function watch(id){const v=V[id],el=$('vid');$('vt').textContent=v.artist+' - '+v.title;
 const there=T=='live'?`<button ${on('act("play",{what:'+J('v:'+id)+',toast:"Its sound plays on Live"})')}>&#9654; Its sound on Live</button>`
  :`<button class="p" ${on('closeV();act("play",{what:'+J('v:'+id)+',toast:"Playing"})')}>&#9654; Play on ${esc(dname(T))}</button>`;
 $('von').innerHTML=there+`<button ${on('act("add",{what:'+J('v:'+id)+',toast:"Added to the queue"})')}>Add to queue</button>`;
 $('vwhy').textContent=v.playable?(v.how=='direct'?'':v.how+': '+v.why):v.why;$('vbox').style.display='flex';
 if(!v.playable){el.removeAttribute('src');return}
 el.src=v.how=='direct'?v.video:(v.hls&&el.canPlayType('application/vnd.apple.mpegurl')?v.hls:v.video);el.play().catch(()=>{})}
function closeV(){const el=$('vid');el.pause();el.removeAttribute('src');el.load();$('vbox').style.display='none'}
async function showVideos(){window.artist=null;$('crumbs').innerHTML=`<a ${on("open_('')")}>Library</a> / Videos`;$('acts').innerHTML='';
 const vs=(await (await fetch('/api/videos')).json()).videos;$('view').innerHTML=vs.length?'<div class="grid">'+vs.map(vtile).join('')+'</div>':'<p class="small">No videos.</p>'}
// getting albums: the indexer's search and SABnzbd (only when the server has both set up)
const H={'X-Aurora':'1'}, mb=b=>b>=1e9?(b/1e9).toFixed(1)+' GB':Math.round(b/1e6)+' MB';
function showGet(){window.artist=null;$('crumbs').innerHTML=`<a ${on("open_('')")}>Library</a> / Get albums`;$('acts').innerHTML='';
 $('view').innerHTML=`<form onsubmit="find();return false" class="acts"><input id="gq" type="search" placeholder="An artist or an album" style="flex:1;padding:.45em .7em;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--ink)">`+
 `<button class="p">Search ${esc(G.indexer||'the indexer')}</button></form><div id="gres" class="small">Results come from your indexer; Download sends one to SABnzbd, which files it in your albums.</div>`;$('gq').focus()}
async function find(){const t=$('gq').value.trim();if(t.length<2)return;$('gres').textContent='Searching…';
 const j=await (await fetch('/api/get/search?q='+encodeURIComponent(t),{headers:H})).json();
 if(j.error){$('gres').textContent=j.error;return}
 $('gres').innerHTML=j.results.length?j.results.map(r=>`<div class="row"><span class="t" title="${esc(r.title)}">${esc(r.title)}</span>`+
  `<span class="small">${mb(r.size)} &middot; ${r.age_days} d${r.grabs?' &middot; '+r.grabs+' grabs':''}</span>`+
  `<button ${on('grab('+J(r.id)+','+J(r.title)+')')}>Download</button></div>`).join(''):'Nothing found.'}
async function grab(id,title){const j=await (await fetch('/api/get/grab',{method:'POST',headers:{...H,'Content-Type':'application/json'},body:J({id})})).json();
 toast(j.error||('Sent to SABnzbd: '+title));dl()}
async function dl(){if(!G.enabled)return;try{const j=await (await fetch('/api/get/queue',{headers:H})).json();if(j.error){$('dlist').textContent=j.error;return}
 const rows=j.downloading.map(d=>`<div class="row"><span class="t" title="${esc(d.name)}">${esc(d.name)}</span><span class="small">${d.percent}% ${esc(d.left)}</span></div>`)
  .concat(j.recent.slice(0,6).map(d=>`<div class="row"><span class="t" title="${esc(d.name)}">${esc(d.name)}</span><span class="small">${esc(d.status)}${d.message?': '+esc(d.message):''}</span></div>`));
 $('dlist').innerHTML=rows.join('')||'Nothing yet'}catch(e){}}
fetch('/api/info').then(r=>r.json()).then(i=>{if(i.videos)$('vidbtn').style.display=''});
let G={};fetch('/api/get/status').then(r=>r.json()).then(g=>{G=g;if(g.enabled){$('getbtn').style.display='';$('dl').style.display='';dl();setInterval(dl,5000)}});
$('q').oninput=()=>{if(listing)render()};
async function poll(){try{D=(await (await fetch('/api/devices')).json()).devices;devs();const j=await (await fetch('/api/now')).json();S=j;show();
 $('rep').classList.toggle('on',j.repeat);
 $('queue').innerHTML=j.queue.length?j.queue.map((q,i)=>`<div class="row${i==j.pos?' cur':''}"><span class="n">${i+1}</span><span class="t" title="${esc(q.path)}">${esc(q.title)}</span><button class="i" title="Play this" onclick="act('jump',{index:${i}})">&#9654;</button><button class="i" title="Remove" onclick="act('remove',{index:${i}})">&#10005;</button></div>`).join(''):'Empty';
 $('who').innerHTML=j.listeners.length?j.listeners.map(l=>esc(l.who)).join('<br>'):'Nobody yet: the queue waits for a listener';}catch(e){}}
function live_np(){const j=S;if(!j.queue)return;$('nph').textContent='Live: now playing';
 const c=j.current;$('npt').textContent=c?c.title:'Nothing queued';$('nps').textContent=c?(c.folder.split('/').pop()||''):'Choose an album, an artist or a song';
 $('npart').style.backgroundImage=c?art(c.folder):'none';$('npbar').style.width=c&&c.length?Math.min(100,100*c.elapsed/c.length)+'%':'0';
 $('npe').textContent=c?mmss(c.elapsed):'';$('npl').textContent=c&&c.length?mmss(c.length):'';
 $('pp').innerHTML=j.paused||!c?'&#9654;':'&#9208;'}
function here(){const a=$('aud');if(a.paused){a.src='/live?t='+Date.now();a.play();$('here').textContent='Stop listening'}else{a.pause();a.removeAttribute('src');a.load();$('here').textContent='Listen here'}}
fetch('/api/addresses').then(r=>r.json()).then(a=>{$('liveurl').innerHTML=a.urls.map(u=>
 `<div><code>${esc(u.url)}/live</code> <span class="small">(${esc(u.family)})</span> <a href="/stations.txt${u.family=='ipv6'||u.family=='ipv4'?'?prefer='+u.family:''}">stations file</a></div>`).join('')});open_('');poll();setInterval(poll,2000);
</script></body></html>
'''


PAGE = """<!DOCTYPE html><html><head><meta charset="utf-8"><title>{title}</title>
<style>body{{font:15px/1.5 system-ui,sans-serif;max-width:52em;margin:2em auto;padding:0 1em}}
a{{text-decoration:none}} li{{margin:.15em 0}} .s{{color:#666;font-size:90%}}</style></head><body>
<h1>{title}</h1><p>{crumbs}</p><p><a href="{station}">&#9654; Play this folder</a> &middot;
<a href="{station}?shuffle=1">shuffled</a> &middot; <a href="{m3u}">M3U</a> &middot;
<a href="/stations.txt">Aurora68 Play stations file</a></p>{body}
<p class="s">Aurora68 music server {version}. Station address for any internet radio player: <code>{full}</code></p></body></html>"""


def make_handler(lib, name, max_listeners, kbps, live, prefer, getter=None):
    getter = getter or Getter()
    listeners = threading.BoundedSemaphore(max_listeners)
    cat, covers = Catalogue(lib, name), Covers(lib)
    radio, devices = Radio(lib.root), Devices()
    vw = VideoWork(lib)
    globals_prefer = prefer

    def capabilities():
        """what this server does with videos, for players to choose by: hls (the conversions a Mac plays), remux,
        transcode, mpeg1 (the video card's streams, /vmpeg/)"""
        c = ['videos'] if VIDEOS else []
        if VIDEOS and lib.ffmpeg:
            c += ['remux'] + (['hls'] if lib.cache.ok else []) + (['transcode', 'mpeg1'] if VIDEO_TRANSCODE else [])
        return c

    def video_rel(vid):
        cat.fresh()
        return cat.videos[vid]['rel'] if vid in cat.videos else None
    live.video_rel = video_rel

    def live_device():
        """the Live channel as a device in the list: its now-playing as a device's state"""
        n = live.now()
        c = n['current'] or {}
        artist, _, title = c.get('title', '').partition(' - ') if ' - ' in c.get('title', '') else ('', '', c.get('title', ''))
        return {'id': 'live', 'name': f'{name} Live', 'kind': 'live', 'version': VERSION, 'online': True,
                'last_seen': int(time.time()), 'is_live': True, 'listeners': len(n['listeners']),
                'state': {'playing': n['playing'], 'title': title, 'artist': artist,
                          'album': (c.get('folder') or '').split('/')[-1], 'folder': c.get('folder', ''),
                          'position': c.get('elapsed', 0.0), 'length': c.get('length', 0.0), 'volume': 100}}

    def track_entry(rel, probe):
        """a song of a command, as a device needs it: its path (for /file/), its id (for /t/), its names"""
        tid = short_id('\x02' + rel)
        if tid in cat.tracks:
            n, title, artist, secs = cat.track_info(tid, probe)
            album = cat.albums[cat.tracks[tid][0]]['title']
        else:
            title, artist, album, secs = os.path.splitext(rel.split('/')[-1])[0], '', '', 0
        return {'path': rel, 'tid': tid, 'title': title, 'artist': artist, 'album': album, 'seconds': secs}

    def video_entry(vid):
        """a music video in a command: a track that says it is one (a player without a screen plays /t/ID or
        /vaudio/ID, its sound), with how it plays and its size"""
        v = cat.videos[vid]
        return {'path': v['rel'], 'tid': vid, 'title': v['title'], 'artist': v['artist'],
                'album': cat.albums[v['album']]['title'] if v['album'] else '', 'seconds': v['secs'], 'video': True,
                'how': v['how'], 'width': v['w'], 'height': v['h']}

    def device_command(did, a):
        """POST /api/devices/<id>/command: (answer, HTTP status)"""
        action = a.get('action', '')
        if action not in DEVICE_ACTIONS:
            return {'error': f'no such action: {action}'}, 400
        what = str(a.get('what') or '')
        if did == 'live':                              # the Live channel: what /api/live does
            if what.startswith('r:') or action in ('seek', 'volume'):
                return {'error': 'the Live channel plays the library; it cannot play radio, seek or change volume'}, 400
            err = live.act({'action': 'clear'} if action == 'stop' else
                           {k: v for k, v in a.items() if k in ('action', 'what', 'shuffle')})
            return ({'error': err}, 400) if err else ({'ok': True, 'live': live.now()}, 200)
        cmd = {'action': action}
        if action in ('play', 'add', 'next'):
            if what.startswith('r:'):
                st = radio.stations()
                try:
                    i = int(what[2:])
                except ValueError:
                    i = -1
                if not 0 <= i < len(st) or action != 'play':
                    return {'error': 'no such radio station' if action == 'play' else 'a radio station can only be played'}, 400
                cmd.update(tracks=[], radio={'name': st[i]['name'], 'url': st[i]['url']})
            elif what.startswith('v:'):                # a music video: on a Mac it shows, on the SE its sound plays
                cat.fresh()
                if what[2:] not in cat.videos:
                    return {'error': 'no such video'}, 400
                cmd['tracks'] = [video_entry(what[2:])]
            else:
                cat.fresh()
                songs = live.resolve(what)
                if not songs:
                    return {'error': 'nothing to play there'}, 400
                if a.get('shuffle'):
                    random.shuffle(songs)
                    cmd['shuffle'] = 1
                songs = songs[:DEVICE_TRACKS]
                cmd['tracks'] = [track_entry(r, len(songs) <= DEVICE_PROBE) for r in songs]
        if action == 'seek' or 'position' in a:
            try:
                cmd['position'] = max(0.0, float(a.get('position')))
            except (TypeError, ValueError):
                return {'error': 'seek: a position in seconds'}, 400
        if action == 'volume' or 'volume' in a:
            try:
                cmd['volume'] = max(0, min(100, int(a.get('volume'))))
            except (TypeError, ValueError):
                return {'error': 'volume: 0 to 100'}, 400
        n, err = devices.send(did, cmd)
        if err:
            return {'error': err}, (404 if err == 'no such device' else 409)
        log(f'devices: {action} {what} to {did}'.replace('  ', ' '))
        if what.startswith('v:') and not devices.shows_video(did):
            return {'ok': True, 'id': n, 'audio_only': True}, 200   # the device plays the video's sound
        return {'ok': True, 'id': n}, 200

    def mac_command_lines(cmds, full=False, videos=False):
        """the classic Play's commands: "N action position volume", then T and R lines; with full=1 (the macOS
        Play) the command line also says whether the server shuffled, and each T line the song's path and album.
        A music video is a V line (id, title, artist, seconds; full=1 also path, album, how it plays, width and
        height) for a Play that asked with videos=1, else a T line, its id being one /t/ answers with its sound"""
        rows = []
        for c in cmds:
            rows.append((c['id'], c['action'], int(c.get('position', 0)), c.get('volume', '')) + ((c.get('shuffle', 0),) if full else ()))
            rows += [(('V', t['tid'], t['title'], t['artist'], t['seconds']) +
                      ((Wide(t['path']), t['album'], t['how'] or '', t['width'], t['height']) if full else ()))
                     if t.get('video') and videos else
                     ('T', t['tid'], t['title'], t['artist'], t['seconds']) + ((Wide(t['path']), t['album']) if full else ())
                     for t in c.get('tracks', [])]
            if c.get('radio'):
                rows.append(('R', c['radio']['name'], Wide(c['radio']['url'])))
        return rows

    def mac_now_rows():
        """/mac/now: what the Live channel plays, as /api/now says it, in lines (22 November 2026, the macOS Play)"""
        n = live.now()
        rows = [('N', n['name'], int(n['playing']), int(n['paused']), int(n['repeat']), n['pos'], n['changes'], len(n['listeners']))]
        c = n['current']
        if c:
            rows.append(('C', Wide(c['path']), Wide(c['folder']), c['title'], f'{c["elapsed"]:.1f}', f'{c["length"]:.1f}'))
        rows += [('Q', Wide(i['path']), i['title']) for i in n['queue']]
        rows += [('L', l['who'], l['since']) for l in n['listeners']]
        return len(n['queue']), rows

    class Handler(BaseHTTPRequestHandler):
        server_version = f'aurora68-music-server/{VERSION}'
        protocol_version = 'HTTP/1.0'

        def log_message(self, fmt, *args):
            pass

        def who(self):
            a = self.client_address[0]
            return a[7:] if a.startswith('::ffff:') else a          # an IPv4 listener on the IPv6 socket

        def preferred(self, prefer=None):
            """the address to hand out: by preference (MUSIC_PREFER, or ?prefer= on the request), else as asked"""
            prefer = prefer if prefer in ('ipv4', 'ipv6', 'asked') else globals_prefer
            host = (self.headers.get('Host') or '').rsplit(':', 1)[0] if not (self.headers.get('Host') or '').endswith(']') \
                else self.headers.get('Host')
            loopback = host.startswith(('127.', '[::1]', '[::ffff:127.'))
            if not loopback and ((prefer == 'ipv6' and host.startswith('[')) or (prefer == 'ipv4' and host[:1].isdigit())):
                return self.base()                     # asked by an address of that kind: it reaches this computer
            if prefer != 'asked':
                have = own_addresses()
                other = 'ipv4' if prefer == 'ipv6' else 'ipv6'
                addr = have.get(prefer) or have.get(other)
                if addr:
                    return address_url(addr, self.server.server_address[1])
            return self.base()

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

        def json(self, code, obj):
            self.send(code, 'application/json', json.dumps(obj).encode(), [('Cache-Control', 'no-store')])

        def do_HEAD(self):
            self.do_GET()

        def do_GET(self):
            u = urllib.parse.urlsplit(self.path)
            q = dict(urllib.parse.parse_qsl(u.query))
            path = urllib.parse.unquote(u.path)
            try:
                if path == '/':
                    if 'text/html' not in (self.headers.get('Accept') or ''):
                        # a player, not a browser (Aurora68 Play asks for */*): the server's bare address
                        # plays the Live channel, so typing just http://NAS:8423/ into a player works
                        self.send_response(302)
                        self.send_header('Location', '/live')
                        self.send_header('Content-Length', '0')
                        self.end_headers()
                        return
                    return self.send(200, 'text/html; charset=utf-8', PANEL.replace('__NAME__', html.escape(name)).encode())
                if path == '/live':
                    return self.live_stream()
                if path == '/api/now':
                    return self.send(200, 'application/json', json.dumps(live.now()).encode(), [('Cache-Control', 'no-store')])
                if path == '/api/get/status':
                    return self.send(200, 'application/json', json.dumps(getter.status()).encode())
                if path in ('/api/get/search', '/api/get/queue'):
                    if self.headers.get('X-Aurora') != '1':   # another web site may not spend the indexer's searches
                        return self.send(403, 'application/json', b'{"error": "the header X-Aurora: 1 is needed"}')
                    if not getter.enabled():
                        return self.send(404, 'application/json', b'{"error": "finding albums is not set up: GET_INDEXER_KEY, GET_SAB_URL and GET_SAB_KEY"}')
                    try:
                        if path == '/api/get/queue':
                            out = getter.queue()
                        else:
                            text = q.get('q', '').strip()[:200]
                            if len(text) < 2:
                                return self.send(400, 'application/json', b'{"error": "type an artist or an album"}')
                            log(f'{self.who()}: find albums {text!r}')
                            out = {'results': getter.search(text)}
                    except RuntimeError as e:
                        return self.send(502, 'application/json', json.dumps({'error': str(e)}).encode())
                    return self.send(200, 'application/json', json.dumps(out).encode(), [('Cache-Control', 'no-store')])
                if path == '/api/addresses':                  # for the panel: where the Mac tunes in
                    have, port = own_addresses(), self.server.server_address[1]
                    order = ['ipv6', 'ipv4'] if prefer == 'ipv6' else ['ipv4', 'ipv6'] if prefer == 'ipv4' else []
                    urls = [{'family': f, 'url': address_url(have[f], port)} for f in order if f in have] or \
                           [{'family': 'as asked', 'url': self.base()}]
                    asked = self.base()                    # the address this browser used works: list it first
                    if order and '[' in asked and 'ipv6' in have and asked != address_url(have['ipv6'], port):
                        urls.insert(0 if prefer == 'ipv6' else 1, {'family': 'ipv6', 'url': asked})
                    return self.send(200, 'application/json', json.dumps({'prefer': prefer, 'urls': urls,
                                                                          'listen': self.server.listen}).encode())
                if path == '/art':
                    pic = cover(lib, q.get('dir', ''))
                    if not pic:
                        return self.fail(404, 'no picture')
                    return self.send(200, pic[1], pic[0], [('Cache-Control', 'max-age=3600')])
                if path.startswith('/browse/'):
                    return self.page(path[len('/browse/'):])
                if path.startswith('/radio/'):
                    return self.station(lib.folder(path[len('/radio/'):]), q)
                if path.startswith('/s/'):
                    lib.scan()
                    return self.station(lib.ids.get(path[3:].strip('/')), q)
                if path.startswith('/file/'):
                    return self.file(path[len('/file/'):])
                if path == '/stations.txt':
                    return self.send(200, 'text/plain; charset=macintosh', stations_file(lib, self.base(q['host']) if q.get('host') else self.preferred(q.get('prefer')), name),
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
                    cat.fresh()
                    if cat.videos:                             # the videos at home here (a folder of videos only: its parent's)
                        shown = lambda h: h if h in lib.dirs or not h else shown(h.rsplit('/', 1)[0] if '/' in h else '')
                        out['videos'] = [cat.video_json(v) for v in sorted(cat.videos, key=lambda v: (cat.videos[v]['number'] or 999,
                                         cat.videos[v]['title'].casefold())) if shown(cat.videos[v]['home']) == rel]
                        for a in out.get('artists', []):
                            a['videos'] = [cat.video_json(v) for v in cat.artist_videos.get(short_id('\x01' + a['name'].casefold()), [])]
                    return self.send(200, 'application/json', json.dumps(out).encode())
                if path == '/api/catalog':                    # the macOS Aurora68 Play's library, in one answer
                    cat.fresh()
                    return self.send(200, 'application/json', json.dumps({
                        'name': name, 'version': VERSION,
                        'artists': [{'id': i, 'name': n, 'albums': cat.artists[i]['albums'] if i in cat.artists else [],
                                     'videos': nv} for i, n, _, _, nv in cat.artists_with_videos()
                                    if i in cat.artists or nv],
                        'albums': [{'id': i, 'title': a['title'], 'artist': a['artist'], 'artist_id': a['aid'],
                                    'folder': a['rel'], 'tracks': len(a['tracks']), 'videos': len(cat.album_videos.get(i, []))}
                                   for i, a in sorted(cat.albums.items(), key=lambda kv: (sort_key(kv[1]['artist']),
                                                                                          kv[1]['title'].casefold()))]
                    }).encode(), [('Cache-Control', 'no-store')])
                if path == '/api/tracks':                     # an album's (or artist's) tracks: titles, lengths, files
                    cat.fresh()
                    if q.get('album') in cat.albums:
                        tids, probe = cat.albums[q['album']]['tracks'], True
                    elif q.get('artist') in cat.artists:
                        tids = [t for al in cat.artists[q['artist']]['albums'] for t in cat.albums[al]['tracks']]
                        probe = True
                    elif q.get('artist') in cat.video_artists:     # an artist with videos and no album
                        tids, probe = [], False
                    else:
                        return self.send(404, 'application/json', b'{"error": "tracks?album=ID or tracks?artist=ID"}')
                    out = []
                    for t in tids:
                        n, title, artist, secs = cat.track_info(t, probe)
                        al, rel = cat.tracks[t]
                        out.append({'id': t, 'number': n, 'title': title, 'artist': artist, 'seconds': secs,
                                    'path': rel, 'album': al, 'album_title': cat.albums[al]['title'],
                                    'folder': cat.albums[al]['rel']})
                    if q.get('album'):
                        out.sort(key=lambda r: (r['number'] or 999, r['title'].casefold()))
                    vids = cat.album_videos.get(q['album'], []) if q.get('album') in cat.albums else cat.artist_videos.get(q['artist'], [])
                    return self.send(200, 'application/json', json.dumps({'tracks': out,
                                                                          'videos': [cat.video_json(v) for v in vids]}).encode())
                if path == '/api/info':                       # what players poll: "library" changes with the folder
                    cat.fresh()
                    return self.json(200, {'name': name, 'version': VERSION, 'library': lib.generation,
                                           'artists': len(cat.artists), 'albums': len(cat.albums), 'tracks': len(cat.tracks),
                                           'videos': len(cat.videos), 'every': DEVICE_EVERY, 'capabilities': capabilities(),
                                           'transcoding': vw.running()})
                if path == '/api/videos':                     # music videos: an artist's, an album's, or every one
                    cat.fresh()
                    if q.get('album'):
                        vids = cat.album_videos.get(q['album'], [])
                    elif q.get('artist'):
                        vids = cat.artist_videos.get(q['artist'], [])
                    else:
                        vids = sorted(cat.videos, key=lambda v: (sort_key(cat.videos[v]['artist']), cat.videos[v]['title'].casefold()))
                    return self.json(200, {'videos': [cat.video_json(v) for v in vids]})
                if path.startswith('/video/'):
                    vid, _, rest = path[len('/video/'):].partition('/')
                    return self.video(vid, rest, q)
                if path.startswith('/vthumb/'):
                    return self.vthumb(path[len('/vthumb/'):].strip('/'))
                if path.startswith('/vaudio/'):
                    return self.vaudio(path[len('/vaudio/'):].strip('/'), q)
                if path.startswith('/vmpeg/'):
                    return self.vmpeg(path[len('/vmpeg/'):].strip('/'), q)
                if path == '/api/radio':
                    return self.json(200, {'stations': radio.stations()})
                if path == '/api/devices':
                    return self.json(200, {'devices': [live_device()] + devices.listing()})
                if path.startswith('/api/devices/') and path.endswith('/commands'):
                    if self.headers.get('X-Aurora') != '1':
                        return self.json(403, {'error': 'the header X-Aurora: 1 is needed'})
                    try:
                        wait = float(q.get('wait', 0))
                    except ValueError:
                        wait = 0
                    got, err = devices.take(path[len('/api/devices/'):-len('/commands')], wait)
                    return self.json(404, {'error': err}) if err else self.json(200, {'commands': got})
                if path.startswith('/mac/'):
                    return self.mac(path[5:], q)
                if path.startswith('/t/'):
                    return self.track(path[3:].strip('/'), q)
                return self.fail(404, 'not here: try /')
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_POST(self):
            """POST /api/live, JSON: {"action": play|add|next|skip|previous|jump|remove|clear|shuffle|pause|
            resume|repeat, "what": "s:ID"|"f:FOLDER"|"t:SONG", "shuffle": 1, "index": N, "on": true}. The header
            X-Aurora: 1 is required, which a web page elsewhere cannot send without asking first (CORS), so
            no other site can change what plays"""
            where = urllib.parse.urlsplit(self.path).path
            dev = where.startswith('/api/devices/')
            if where not in ('/api/live', '/api/get/grab') and not dev:
                return self.fail(404, 'POST /api/live, /api/get/grab or /api/devices/...')
            if self.headers.get('X-Aurora') != '1':
                return self.send(403, 'application/json', b'{"error": "the header X-Aurora: 1 is needed"}')
            if dev:                                       # announce, state, command (the README's "Devices")
                try:
                    n = int(self.headers.get('Content-Length') or 0)
                    a = json.loads(self.rfile.read(min(n, 65536)) or b'{}')
                    if not isinstance(a, dict):
                        raise ValueError('a JSON object, please')
                except (ValueError, TypeError) as e:
                    return self.json(400, {'error': f'not understood: {e}'})
                rest = where[len('/api/devices/'):]
                if rest == 'announce':
                    err = devices.announce(a.get('id'), a.get('name'), a.get('kind'), a.get('version', ''), self.who(),
                                           a.get('video') in (True, 1, '1'))
                    if err:
                        return self.json(400, {'error': err})
                    lib.scan()
                    return self.json(200, {'ok': True, 'every': DEVICE_EVERY, 'library': lib.generation})
                did, _, verb = rest.partition('/')
                if verb == 'state':
                    err = devices.set_state(did, a)
                    return self.json(404, {'error': err}) if err else self.json(200, {'ok': True})
                if verb == 'command':
                    out, code = device_command(did, a)
                    return self.json(code, out)
                return self.json(404, {'error': 'POST /api/devices/announce, /api/devices/ID/state or /api/devices/ID/command'})
            if where == '/api/get/grab':                  # {"id": a search result's}: to SABnzbd
                if not getter.enabled():
                    return self.send(404, 'application/json', b'{"error": "finding albums is not set up"}')
                try:
                    n = int(self.headers.get('Content-Length') or 0)
                    a = json.loads(self.rfile.read(min(n, 4096)) or b'{}')
                    err = getter.grab(str(a.get('id', ''))) if isinstance(a, dict) else 'a JSON object, please'
                except (ValueError, TypeError) as e:
                    err = f'not understood: {e}'
                except RuntimeError as e:
                    err = str(e)
                return self.send(400 if err else 200, 'application/json', json.dumps({'error': err} if err else {'sent': True}).encode())
            try:
                n = int(self.headers.get('Content-Length') or 0)
                a = json.loads(self.rfile.read(min(n, 65536)) or b'{}')
                err = live.act(a) if isinstance(a, dict) else 'a JSON object, please'
            except (ValueError, TypeError) as e:
                err = f'not understood: {e}'
            body = json.dumps({'error': err} if err else live.now()).encode()
            self.send(400 if err else 200, 'application/json', body)

        def live_stream(self):
            if not listeners.acquire(blocking=False):
                return self.fail(503, f'{max_listeners} listeners already')
            meta = self.headers.get('Icy-MetaData') == '1'
            who = self.who()
            l = None
            try:
                self.send_response(200)
                self.send_header('Content-Type', 'audio/mpeg')
                self.send_header('Cache-Control', 'no-cache, no-store')
                self.send_header('Connection', 'close')
                self.send_header('icy-name', f'{name} Live')
                self.send_header('icy-description', f'{name}: what the control panel plays')
                if meta:
                    self.send_header('icy-metaint', str(METAINT))
                self.end_headers()
                if self.command == 'HEAD':
                    return
                out = IcyWriter(self.wfile, meta)
                l = live.join(who)
                while not l.gone:
                    got = l.pop()
                    if got:
                        out.title = got[1]
                        out.write(got[0])
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                if l:
                    live.leave(l)
                listeners.release()

        # ---- Aurora68 Play's Library (the catalogue above): /mac/..., and /t/ID for one track ----
        # enc=utf8 (the macOS Play, 22 November 2026): the answer in UTF-8, and the query's words read as UTF-8;
        # else Mac Roman both ways, as the classic Play wants. full=1: the longer records the macOS Play needs
        # (paths, folders, the devices' whole state); without it every answer is what it was before.
        def mac_q(self):
            raw_q = urllib.parse.urlsplit(self.path).query
            enc = 'utf-8' if 'enc=utf8' in raw_q.split('&') else 'mac_roman'
            return dict(urllib.parse.parse_qsl(raw_q, encoding=enc))

        def mac_enc(self):
            return 'utf-8' if 'enc=utf8' in urllib.parse.urlsplit(self.path).query.split('&') else 'mac_roman'

        def mac_send(self, code, body):
            self.send(code, 'text/plain; charset=' + ('utf-8' if self.mac_enc() == 'utf-8' else 'macintosh'), body)

        def mac_error(self, code, text):
            self.mac_send(code, mac_answer('error', 1, 0, [(text,)], self.mac_enc()))

        def mac(self, what, q):
            cat.fresh()
            enc, full = self.mac_enc(), q.get('full') == '1'
            vids = q.get('videos') == '1'             # 22 November 2026: music videos, for a Play that knows them
            if what == 'info':                       # videos=1: also every, how many videos, and what the server can do
                return self.mac_send(200, mac_answer('info', 1, 0, [(name, VERSION, len(cat.artists), len(cat.albums), len(cat.tracks),
                                                                      lib.generation) + ((DEVICE_EVERY,) if full or vids else ()) +
                                                                     ((len(cat.videos), ','.join(capabilities())) if vids else ())], enc))
            if what in ('announce', 'state', 'devices', 'commands', 'radio', 'command'):
                return self.mac_device(what)
            if what == 'videos':                     # an artist's or an album's videos, or every one: V records
                if q.get('album'):
                    ids = cat.album_videos.get(q['album'], [])
                elif q.get('artist'):
                    ids = cat.artist_videos.get(q['artist'], [])
                else:
                    ids = sorted(cat.videos, key=lambda v: (sort_key(cat.videos[v]['artist']), cat.videos[v]['title'].casefold()))
                part, start = page_of(ids, q)
                return self.mac_send(200, mac_answer('videos', len(ids), start, [cat.video_row(v, full) for v in part], enc))
            if what == 'now':
                total, rows = mac_now_rows()
                return self.mac_send(200, mac_answer('now', total, 0, rows, enc))
            if what == 'artists':
                rows = [(i, cat.artists[i]['name'], len(cat.artists[i]['albums']),
                         sum(len(cat.albums[a]['tracks']) for a in cat.artists[i]['albums'])) for i in cat.artist_order] if not vids \
                    else [r for r in cat.artists_with_videos() if r[0] in cat.artists or r[4]]   # a fifth field: videos
                part, start = page_of(rows, q)
                return self.mac_send(200, mac_answer('artists', len(rows), start, part, enc))
            if what == 'albums':
                aid = q.get('artist', '')
                if aid and aid not in cat.artists:
                    return self.mac_error(404, 'There is no such artist in the library now.')
                ids = cat.artists[aid]['albums'] if aid else sorted(cat.albums, key=lambda i: (sort_key(cat.albums[i]['artist']),
                                                                                              cat.albums[i]['title'].casefold()))
                part, start = page_of(ids, q)
                return self.mac_send(200, mac_answer('albums', len(ids), start, cat.album_rows(part, full), enc))
            if what == 'tracks':
                if q.get('album'):
                    a = cat.albums.get(q['album'])
                    if not a:
                        return self.mac_error(404, 'There is no such album in the library now.')
                    tids, probe = a['tracks'], True
                elif q.get('artist'):
                    ar = cat.artists.get(q['artist'])
                    if not ar and not (vids and q['artist'] in cat.video_artists):
                        return self.mac_error(404, 'There is no such artist in the library now.')
                    tids, probe = [t for al in ar['albums'] for t in cat.albums[al]['tracks']] if ar else [], False
                else:
                    return self.mac_error(400, 'tracks?album=ID or tracks?artist=ID')
                if vids:                             # the tracks, then the album's (or the artist's) videos: V records
                    vl = cat.album_videos.get(q['album'], []) if q.get('album') else cat.artist_videos.get(q['artist'], [])
                    part, start = page_of([('T', t) for t in tids] + [('V', v) for v in vl], q)
                    rows = cat.track_rows([t for k, t in part if k == 'T'], probe, full)
                    if probe:
                        rows.sort(key=lambda r: (r[1] or 999, r[2].casefold()) if any(x[1] for x in rows) else 0)
                    rows += [cat.video_row(v, full) for k, v in part if k == 'V']
                    return self.mac_send(200, mac_answer('tracks', len(tids) + len(vl), start, rows, enc))
                part, start = page_of(tids, q)
                rows = cat.track_rows(part, probe, full)
                if probe:
                    rows.sort(key=lambda r: (r[1] or 999, r[2].casefold()) if any(x[1] for x in rows) else 0)
                return self.mac_send(200, mac_answer('tracks', len(tids), start, rows, enc))
            if what == 'search':                  # the Mac sends its words in Mac Roman, %XX for each byte (enc=utf8: UTF-8)
                mq = self.mac_q()
                words = mq.get('q', '').casefold().split()
                if not words:
                    return self.mac_send(200, mac_answer('search', 0, 0, [], enc))
                hit = lambda text: all(w in text.casefold() for w in words)
                rows = [('A', i, cat.artists[i]['name'], f'{len(cat.artists[i]["albums"])} albums')
                        for i in cat.artist_order if hit(cat.artists[i]['name'])]
                if vids:                             # artists with videos only, then (after the albums) videos, as tracks are
                    rows += [('A', i, n, '0 albums') for i, n in sorted(cat.video_artists.items(), key=lambda kv: sort_key(kv[1]))
                             if i not in cat.artists and hit(n)]
                rows += [('L', i, a['title'], a['artist']) for i, a in sorted(cat.albums.items(), key=lambda kv: kv[1]['title'].casefold())
                         if hit(a['title'] + ' ' + a['artist'])]
                if vids:
                    rows += [('V', i, v['title'], v['artist'], v['album'], v['secs'])
                             for i, v in sorted(cat.videos.items(), key=lambda kv: kv[1]['title'].casefold())
                             if hit(v['title'] + ' ' + v['artist'])][:100]
                for t in cat.tracks:
                    if len(rows) >= 300:
                        break
                    n, title, artist, secs = cat.track_info(t, probe=False)
                    if hit(title + ' ' + artist):
                        rows.append(('T', t, title, artist, cat.tracks[t][0], secs))
                part, start = page_of(rows, q)
                return self.mac_send(200, mac_answer('search', len(rows), start, part, enc))
            if what in ('art', 'arts'):
                try:
                    w = max(MAC_ART_MIN, min(MAC_ART_MAX, int(q.get('w', 64))))
                    h = max(MAC_ART_MIN, min(MAC_ART_MAX, int(q.get('h', w))))
                    depth = 8 if q.get('depth') == '8' else 1
                except ValueError:
                    return self.mac_error(400, 'w, h: numbers')
                rb = ((w + 15) // 16) * 2 if depth == 1 else w + (w & 1)
                if what == 'art':
                    a = cat.albums.get(q.get('album', ''))
                    got = covers.get(q['album'], a, w, h, depth) if a else None
                    if not got:
                        return self.mac_error(404, 'This album has no cover picture.')
                    return self.send(200, 'application/octet-stream',
                                     f'A68ART\t1\t{w}\t{h}\t{depth}\t{rb}\r'.encode() + got[1])
                ids = [i for i in q.get('albums', '').split(',') if i][:64]
                body = bytearray(f'A68ARTS\t1\t{len(ids)}\t{w}\t{h}\t{depth}\t{rb}\r'.encode())
                for i in ids:                    # each: a byte (1 a cover, 0 none), then rb * h bytes
                    a = cat.albums.get(i)
                    got = covers.get(i, a, w, h, depth) if a else None
                    body += (b'\1' + got[1]) if got else (b'\0' + bytes(rb * h))
                return self.send(200, 'application/octet-stream', bytes(body))
            return self.mac_error(404, 'info, artists, albums, tracks, search, art, arts, now, announce, state, devices, commands, '
                                       'command, radio')

        def mac_device(self, what):
            """the classic Play's side of "Devices": plain lines (Mac Roman, CR ends, tab fields), no A68M head"""
            mq, enc = self.mac_q(), self.mac_enc()
            full = mq.get('full') == '1'
            vids = mq.get('videos') == '1'
            lines = lambda rows: ''.join(mac_line(r, 255) + '\r' for r in rows).encode(enc, 'replace')
            if what == 'radio':
                return self.mac_send(200, lines((s['group'], s['name'], Wide(s['url'])) for s in radio.stations()))
            if what == 'devices':
                rows = []
                for d in [live_device()] + devices.listing():
                    st = d['state']
                    row = (d['id'], d['name'], d['kind'], int(d['online']), st.get('title', ''), st.get('artist', ''))
                    if full or vids:                 # videos=1: the full record and an 18th field, whether it shows videos                     # the rest of the state, as /api/devices has it (the macOS Play's "Play on")
                        row += (int(bool(st.get('playing'))), st.get('position', 0), st.get('length', 0), st.get('volume', 100),
                                st.get('album', ''), Wide(st.get('folder', '')), st.get('at', 0), d.get('last_seen', 0),
                                int(bool(d.get('is_live'))), d.get('version', ''), d.get('listeners', 0))
                    if vids:
                        row += (int(bool(d.get('video'))),)
                    rows.append(row)
                return self.mac_send(200, lines(rows))
            if self.headers.get('X-Aurora') != '1':
                return self.mac_send(403, b'the header X-Aurora: 1 is needed\r')
            if what == 'command':                # "play this there": POST /api/devices/ID/command's, and /api/live's for "live"
                did = mq.get('to', '')
                a = {k: mq[k] for k in ('action', 'what', 'shuffle', 'position', 'volume', 'index', 'on') if mq.get(k, '') != ''}
                if 'shuffle' in a:
                    a['shuffle'] = a['shuffle'] == '1'
                if 'on' in a:
                    a['on'] = a['on'] == '1'
                if did == 'live' and a.get('action') not in DEVICE_ACTIONS:   # the Live queue's own: jump, remove, clear ...
                    err = live.act(a)
                    return self.mac_send(400 if err else 200, lines([(err or 'ok',)]))
                out, code = device_command(did, a)
                return self.mac_send(code, lines([(out['error'],)] if 'error' in out else [('ok', out.get('id', 0))]))
            did = mq.get('id', '')
            if what == 'announce':
                err = devices.announce(did, mq.get('name'), mq.get('kind', 'classic'), mq.get('v', ''), self.who(),
                                       mq.get('video') == '1')
                if err:
                    return self.mac_send(400, lines([(err,)]))
                lib.scan()
                return self.mac_send(200, lines([('ok',), (DEVICE_EVERY,), (lib.generation,)]))
            if what == 'state':
                st = {'playing': mq.get('p') == '1', 'position': mq.get('pos'), 'length': mq.get('len'),
                      'volume': mq.get('vol', 100), 'title': mq.get('title', ''), 'artist': mq.get('artist', '')}
                tid = mq.get('t', '')
                if tid in cat.videos:                  # a music video (its sound, on the SE): its names from the catalogue
                    v = cat.videos[tid]
                    st.update(title=v['title'], artist=v['artist'], album=cat.albums[v['album']]['title'] if v['album'] else '',
                              folder=v['folder'], video=True)
                    try:
                        st['length'] = float(st['length'] or 0) or v['secs']
                    except ValueError:
                        st['length'] = v['secs']
                if tid in cat.tracks:                  # a library track: its names from the catalogue
                    n, title, artist, secs = cat.track_info(tid, probe=False)
                    al = cat.albums[cat.tracks[tid][0]]
                    st.update(title=title, artist=artist, album=al['title'], folder=al['rel'])
                    try:
                        st['length'] = float(st['length'] or 0) or secs
                    except ValueError:
                        st['length'] = secs
                err = devices.set_state(did, st)
                return self.mac_send(404 if err else 200, lines([(err or 'ok',)]))
            try:
                wait = float(mq.get('wait', 0))
            except ValueError:
                wait = 0
            got, err = devices.take(did, wait)
            if err:
                return self.mac_send(404, lines([(err,)]))
            return self.mac_send(200, lines(mac_command_lines(got, full, vids)))

        def track(self, tid, q):
            cat.fresh()
            if tid not in cat.tracks and tid in cat.videos:
                return self.vaudio(tid, q)             # a video's id where a track's goes: its sound (an older Play)
            if tid not in cat.tracks:
                return self.fail(404, 'no such track')
            if not listeners.acquire(blocking=False):
                return self.fail(503, f'{max_listeners} listeners already')
            al, rel = cat.tracks[tid]
            n, title, artist, secs = cat.track_info(tid)
            try:
                start = max(0, int(q.get('start', 0)))
            except ValueError:
                start = 0
            meta = self.headers.get('Icy-MetaData') == '1'
            who = self.who()
            try:
                self.send_response(200)
                self.send_header('Content-Type', 'audio/mpeg')
                self.send_header('Cache-Control', 'no-cache, no-store')
                self.send_header('Connection', 'close')
                self.send_header('icy-name', mac_field(f'{artist} - {title}', 60))
                self.send_header('X-Aurora-Seconds', str(secs))
                if meta:
                    self.send_header('icy-metaint', str(METAINT))
                self.end_headers()
                if self.command == 'HEAD':
                    return
                log(f'{who}: track {artist} - {title}' + (f' from {start} s' if start else ''))
                stream_track(lib, rel, self.wfile, meta, start, kbps, f'{artist} - {title}')
            except (BrokenPipeError, ConnectionResetError, OSError):
                log(f'{who}: left the track {title}')
            finally:
                listeners.release()

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
            video = p and lib.video_path(rel.strip('/')) == p
            if not p or not os.path.isfile(p) or not (lib.playable(p) or video):
                return self.fail(404, 'no such file')
            self.send_file(p, VIDEO_TYPES.get(os.path.splitext(p)[1].lower()) if video else None)

        def send_file(self, p, ctype=None):
            """a file as it is, with Range (206, Content-Range), as AVFoundation and browsers ask for it"""
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
            self.send_header('Content-Type', ctype or AUDIO_TYPES.get(os.path.splitext(p)[1].lower()) or mimetypes.guess_type(p)[0]
                             or 'application/octet-stream')
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

        # ---- music videos (22 November 2026): /video/, /vthumb/, /vaudio/, /vmpeg/
        def the_video(self, vid):
            """(the catalogue's video, its real path), or None after a 404 has been sent"""
            cat.fresh()
            v = cat.videos.get(vid)
            p = lib.video_path(v['rel']) if v else None
            if not p:
                self.fail(404, 'no such video')
                return None
            return v, p

        def video(self, vid, rest, q):
            """/video/ID: the file as it is when a Mac plays it so (Range), else a fragmented MP4 made as it is sent
            (?start=S to begin later); /video/ID/index.m3u8 and its segments: the same as HLS, for AVFoundation"""
            got = self.the_video(vid)
            if not got:
                return
            v, p = got
            who = self.who()
            if rest:
                if v['how'] == 'direct' and rest == 'index.m3u8':
                    return self.fail(404, 'this video plays as it is: /video/' + vid)
                if not lib.ffmpeg:
                    return self.fail(404, 'no ffmpeg here')
                d, refused = vw.hls(vid, v, p)
                if refused:
                    log(f'{who}: video {v["title"]} refused: {refused[1]}')
                    return self.fail(*refused)
                if rest == 'index.m3u8':
                    body = vw.playlist(vid, d)
                    if body is None:
                        return self.fail(500, 'the conversion did not start: the log says why')
                    return self.send(200, 'application/vnd.apple.mpegurl', body, [('Cache-Control', 'no-cache')])
                body = vw.segment(vid, d, rest)
                if body is None:
                    return self.fail(404, 'no such segment')
                return self.send(200, 'video/mp4' if rest.endswith('.mp4') else 'video/iso.segment', body,
                                 [('Cache-Control', 'max-age=3600')])
            if v['how'] == 'direct':
                return self.send_file(p, 'video/quicktime' if v['ext'] == '.mov' else 'video/mp4')
            if not lib.ffmpeg:
                return self.fail(404, 'no ffmpeg here')
            try:
                start = max(0.0, float(q.get('start', 0)))
            except ValueError:
                start = 0.0
            cmd = [lib.ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error'] + (['-ss', f'{start:.3f}'] if start else []) + \
                ['-i', p] + video_codec_args(v, v['how']) + ['-movflags', 'frag_keyframe+empty_moov+default_base_moof', '-f', 'mp4', 'pipe:1']
            self.pipe(v, cmd, 'video/mp4', v['how'], f'video {v["artist"]} - {v["title"]}, {v["how"]} ({v["why"]})')

        def pipe(self, v, cmd, ctype, how, what):
            """an ffmpeg's output as the answer, no length (it ends when ffmpeg does); the client leaving stops it"""
            refused = None if self.command == 'HEAD' else vw.take(how)
            if refused:
                log(f'{self.who()}: {what}: refused, {refused[1]}')
                return self.fail(*refused)
            ff = None
            try:
                self.send_response(200)
                self.send_header('Content-Type', ctype)
                self.send_header('Cache-Control', 'no-cache, no-store')
                self.send_header('Connection', 'close')
                self.send_header('X-Aurora-Seconds', str(v['secs']))
                self.end_headers()
                if self.command == 'HEAD':
                    return
                ff = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
                log(f'{self.who()}: {what}')
                while True:
                    b = ff.stdout.read(65536)
                    if not b:
                        break
                    self.wfile.write(b)               # TCP holds it back while the player waits; ffmpeg waits with it
            except (BrokenPipeError, ConnectionResetError, OSError):
                log(f'{self.who()}: left the {what.split(",")[0]}')
            finally:
                if ff:
                    ff.kill()
                    ff.wait()
                if self.command != 'HEAD':
                    vw.give(how)

        def vthumb(self, vid):
            got = self.the_video(vid)
            if not got:
                return
            pic = vw.poster(vid, *got) if lib.ffmpeg else None
            if not pic:
                return self.fail(404, 'no poster for this video')
            return self.send(200, 'image/jpeg', pic, [('Cache-Control', 'max-age=86400')])

        def vaudio(self, vid, q):
            """a video's sound as an MP3 with an end, like /t/ID (the classic Play plays it so), from ?start=S"""
            got = self.the_video(vid)
            if not got:
                return
            v, p = got
            if not lib.ffmpeg:
                return self.fail(404, 'no ffmpeg here to take the sound out of a video')
            if not v['acodec'] and lib.ffprobe:
                return self.fail(404, 'this video has no sound')
            if not listeners.acquire(blocking=False):
                return self.fail(503, f'{max_listeners} listeners already')
            try:
                start = max(0, int(float(q.get('start', 0))))
            except ValueError:
                start = 0
            title = f'{v["artist"]} - {v["title"]}'
            meta = self.headers.get('Icy-MetaData') == '1'
            try:
                self.send_response(200)
                self.send_header('Content-Type', 'audio/mpeg')
                self.send_header('Cache-Control', 'no-cache, no-store')
                self.send_header('Connection', 'close')
                self.send_header('icy-name', mac_field(title, 60))
                self.send_header('X-Aurora-Seconds', str(v['secs']))
                if meta:
                    self.send_header('icy-metaint', str(METAINT))
                self.end_headers()
                if self.command == 'HEAD':
                    return
                log(f'{self.who()}: the sound of the video {title}' + (f' from {start} s' if start else ''))
                stream_track(lib, v['rel'], self.wfile, meta, start, kbps, title, vw.soundtrack(vid, v, p, start, kbps))
            except (BrokenPipeError, ConnectionResetError, OSError):
                log(f'{self.who()}: left the video {title}')
            finally:
                listeners.release()

        def vmpeg(self, vid, q):
            """/vmpeg/ID?w=W&h=H&start=S: the video as an MPEG-1 program stream for the Aurora68 Video Decoder card
            (docs/16), made exactly as tools/video-helper's /play makes it; always a transcode"""
            got = self.the_video(vid)
            if not got:
                return
            v, p = got
            if not lib.ffmpeg:
                return self.fail(404, 'no ffmpeg here')
            w, h = mpeg_size(q.get('w'), MPEG_MAX_W, 320), mpeg_size(q.get('h'), MPEG_MAX_H, 240)
            start = int(q['start']) if q.get('start', '').isdigit() else 0
            cmd = [lib.ffmpeg, '-hide_banner', '-loglevel', 'error', '-nostdin'] + (['-ss', str(start)] if start else []) + \
                ['-i', p] + mpeg1_args(w, h)
            self.pipe(v, cmd, 'video/mpeg', 'transcode', f'video {v["artist"]} - {v["title"]} as MPEG-1 at {w}x{h}' +
                      (f' from {start} s' if start else ''))

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

    v6only = None

    def server_bind(self):
        if self.address_family == socket.AF_INET6 and self.v6only is not None:
            try:
                self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1 if self.v6only else 0)
            except OSError:
                pass
        super().server_bind()


def serve(bind, port, handler, listen='both'):
    """the listening socket. listen "both" (the default): IPv6 and IPv4 on one socket, falling back to
    IPv4 alone on a computer without IPv6; "ipv4" or "ipv6": only that one. --bind ADDR: that address."""
    def server(family, addr, v6only=None):
        cls = type('Server', (DualStackServer,), {'address_family': family, 'v6only': v6only})
        srv = cls((addr, port), handler)
        return srv
    if bind not in ('', '::'):
        fam = socket.AF_INET6 if ':' in bind else socket.AF_INET
        srv, how = server(fam, bind, True if fam == socket.AF_INET6 else None), 'IPv6' if fam == socket.AF_INET6 else 'IPv4'
    elif listen == 'ipv4':
        srv, how = server(socket.AF_INET, '0.0.0.0'), 'IPv4 only'
    elif listen == 'ipv6':
        srv, how = server(socket.AF_INET6, '::', True), 'IPv6 only'
    else:
        try:
            srv, how = server(socket.AF_INET6, '::', False), 'IPv6 and IPv4'
        except OSError as e:
            log(f'no IPv6 here ({e.strerror}): IPv4 only')
            srv, how = server(socket.AF_INET, '0.0.0.0'), 'IPv4'
    srv.listen = how
    return srv, how


def main():
    ap = argparse.ArgumentParser(description='A folder of MP3s as internet radio stations (IPv4 and IPv6)')
    ap.add_argument('music', nargs='?', default=os.environ.get('MUSIC_DIR', '/music'), help='the music folder (/music)')
    ap.add_argument('--port', type=int, default=int(os.environ.get('MUSIC_PORT', '8000')), help='TCP port (8000)')
    ap.add_argument('--listen', choices=['both', 'ipv4', 'ipv6'], default=os.environ.get('MUSIC_LISTEN', 'both'),
                    help='answer on IPv6 and IPv4 (both, the default), or on one of them only')
    ap.add_argument('--prefer', choices=['asked', 'ipv6', 'ipv4'], default=os.environ.get('MUSIC_PREFER', 'asked'),
                    help='the address handed to the Mac in the stations file and on the panel: as the request '
                         'came (asked, the default), or this computer\'s IPv6 or IPv4 address (the other if it has none)')
    ap.add_argument('--bind', default=os.environ.get('MUSIC_BIND', '::'),
                    help='one address to listen on only (the default "::" means every address, as --listen says)')
    ap.add_argument('--name', default=os.environ.get('MUSIC_NAME', 'Music'), help='the library\'s name in menus and pages')
    ap.add_argument('--max-listeners', type=int, default=int(os.environ.get('MUSIC_MAX_LISTENERS', '16')))
    ap.add_argument('--bitrate', type=int, default=int(os.environ.get('MUSIC_BITRATE', '192')),
                    help='kbit/s of the MP3 that FLAC, M4A and other formats are converted to (192)')
    ap.add_argument('--ffmpeg', default=os.environ.get('MUSIC_FFMPEG', shutil.which('ffmpeg') or ''),
                    help='ffmpeg, to play formats other than MP3 ("" for MP3s only)')
    ap.add_argument('--discovery', type=int, default=int(os.environ.get('MUSIC_DISCOVERY_PORT', str(MAC_DISCOVERY))),
                    help=f'UDP port on which Aurora68 Play finds the server ({MAC_DISCOVERY}; 0: none)')
    a = ap.parse_args()
    if not os.path.isdir(a.music):
        sys.exit(f'no such folder: {a.music}')
    if a.bitrate not in (64, 80, 96, 112, 128, 160, 192, 224, 256, 320):
        sys.exit('--bitrate: one of 64, 80, 96, 112, 128, 160, 192, 224, 256, 320')
    lib = Library(a.music, a.ffmpeg or None)
    lib.scan()
    getter = Getter()
    srv, how = serve(a.bind, a.port, make_handler(lib, a.name, a.max_listeners, a.bitrate, Live(lib, a.name, a.bitrate),
                                                  a.prefer, getter), a.listen)
    gs = getter.status()
    log('finding albums: ' + (f'on, {gs["indexer"]} and SABnzbd (category {gs["category"]})' if gs['enabled'] else
                              'off (GET_INDEXER_KEY, GET_SAB_URL and GET_SAB_KEY turn it on)'))
    have = own_addresses()
    if a.discovery:
        threading.Thread(target=music_discovery, args=(a.discovery, a.name, a.port), daemon=True).start()
    log('addresses: ' + (', '.join(f'{f} {address_url(v, a.port)}' for f, v in sorted(have.items(), reverse=True)) or 'none found')
        + f'; handing out {a.prefer if a.prefer != "asked" else "the address each request used"}')
    signal.signal(signal.SIGTERM, lambda *x: sys.exit(0))
    log(f'aurora68 music server {VERSION} "{a.name}" on port {a.port} ({how}): {lib.root}; '
        + (f'other formats converted to MP3 at {a.bitrate} kbit/s' if lib.ffmpeg else 'MP3s only (no ffmpeg)'))
    srv.serve_forever()


if __name__ == '__main__':
    main()
