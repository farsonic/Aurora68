#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""aurora68_video_helper.py - the video card's helper (docs/16-video-card.md, GitHub issue #22).

A small web server on a computer on the same network as the SE (the owner's Mac, a Docker container,
any Linux box). The Mac's program Aurora68 Video talks to it over the Mac's own TCP/IP (Riptide or
MacTCP), as Aurora68 Play fetches internet radio, and writes the stream into the video card, which only
decodes. Nothing here involves the Pi's own network or its remote control.

    GET /info                                  {"name":..., "version":..., "mode": "youtube"|"files"}
    GET /search?q=TEXT                         {"results": [{"video","title","channel","seconds"}...]} or {"error"}
    GET /play?video=ID&w=W&h=H&start=S         an MPEG program stream (MPEG-1 video at W x H, MP2 audio at
                                               44.1 kHz stereo), no Content-Length: it ends when the video
                                               does; the client closing the connection stops it

Finding it: a UDP datagram "AURORA68-VIDEO?" to its port (a broadcast from the Mac does) is answered
with the same JSON as /info plus "port", so the Mac finds it without an address being typed.

The modern work is done by two programs it calls: yt-dlp (search, and YouTube's unlocking of its
streams, kept up to date on this computer) and ffmpeg (conversion to what a Pi 3 core decodes: MPEG-1
and MP2 at the size the Mac will show).

    python3 aurora68_video_helper.py                          YouTube, port 8069
    python3 aurora68_video_helper.py --file a.mp4 --file b.mkv   only these files (no YouTube)
    python3 aurora68_video_helper.py --token SECRET           clients must send X-Aurora-Token: SECRET

Standard library only. A video ID or search text from a client goes to yt-dlp as one argument (never to
a shell); in file mode only the files given on the command line can be played.
"""
import argparse, json, os, shutil, signal, socket, subprocess, sys, threading, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = '2'
PROBE = b'AURORA68-VIDEO?'
# a video's size: what the Mac asked for, within what a Pi 3 core decodes comfortably, and even
MAX_W, MAX_H = 1280, 720


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


class Sources:
    """search and play: YouTube through yt-dlp, or only the files named with --file"""
    def __init__(self, files, ytdlp, ffmpeg):
        self.files = [os.path.abspath(f) for f in files]
        self.ytdlp, self.ffmpeg = ytdlp, ffmpeg

    def mode(self):
        return 'files' if self.files else 'youtube'

    def search(self, q, n=30):
        if self.files:
            ql = q.lower()
            hits = [f for f in self.files if ql in os.path.basename(f).lower()] or self.files
            return [{'video': f'file:{self.files.index(f)}', 'title': os.path.basename(f), 'channel': 'this computer',
                     'seconds': probe_seconds(f)} for f in hits[:n]]
        out = subprocess.run([self.ytdlp, '--flat-playlist', '-J', '--no-warnings', f'ytsearch{n}:{q}'],
                             capture_output=True, text=True, timeout=60)
        if out.returncode:
            raise RuntimeError('yt-dlp: ' + (out.stderr.strip().splitlines() or ['failed'])[-1])
        entries = json.loads(out.stdout).get('entries') or []
        return [{'video': e.get('id', ''), 'title': e.get('title') or '', 'channel': e.get('channel') or e.get('uploader') or '',
                 'seconds': int(e.get('duration') or 0)} for e in entries if e.get('id')]

    def input_args(self, video, start):
        """ffmpeg's input for a video: (arguments, a process feeding its stdin or None)"""
        seek = ['-ss', str(start)] if start else []
        if video.startswith('file:'):
            try:
                return seek + ['-i', self.files[int(video[5:])]], None
            except (ValueError, IndexError):
                raise RuntimeError('no such file')
        if self.files:
            raise RuntimeError('file mode: only the files given with --file')
        if not video or not all(c.isalnum() or c in '-_' for c in video):
            raise RuntimeError('not a video ID')
        url = 'https://www.youtube.com/watch?v=' + video
        # one stream with sound and picture, at most 480 lines (more is wasted at the Pi's sizes)
        feed = subprocess.Popen([self.ytdlp, '-q', '--no-warnings', '-f', 'b[height<=480]/b', '-o', '-', url],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        return seek + ['-i', 'pipe:0'], feed


def probe_seconds(path):
    try:
        out = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', path],
                             capture_output=True, text=True, timeout=20)
        return int(float(out.stdout.strip()))
    except Exception:
        return 0


def even(v, top, default):
    try:
        v = int(v)
    except (TypeError, ValueError):
        v = default
    v = max(64, min(v, top))
    return v - v % 2


def ffmpeg_args(ffmpeg, inp, w, h):
    """MPEG-1 video scaled to fit w x h (keeping its shape, padded black), 25 frames a second, no B-frames
    (each frame is shown as it is decoded); MP2 audio, 44.1 kHz stereo; an MPEG program stream"""
    rate = max(300, min(2500, w * h * 25 // 450))          # kbit/s: about 0.09 bit per pixel per frame
    vf = (f'scale={w}:{h}:force_original_aspect_ratio=decrease,pad={w}:{h}:(ow-iw)/2:(oh-ih)/2,'
          f'fps=25,format=yuv420p')
    return [ffmpeg, '-hide_banner', '-loglevel', 'error', '-nostdin'] + inp + [
        '-vf', vf, '-c:v', 'mpeg1video', '-b:v', f'{rate}k', '-maxrate', f'{rate * 2}k', '-bufsize', f'{rate}k',
        '-bf', '0', '-g', '25', '-c:a', 'mp2', '-b:a', '128k', '-ar', '44100', '-ac', '2',
        '-f', 'mpeg', '-muxdelay', '0.5', 'pipe:1']     # muxdelay 0: the muxer complains of underflow


def make_handler(sources, token, name):
    class Handler(BaseHTTPRequestHandler):
        server_version = f'aurora68-video-helper/{VERSION}'
        protocol_version = 'HTTP/1.0'

        def log_message(self, fmt, *args):
            pass                                             # our own lines, below, instead

        def json(self, code, obj):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urllib.parse.urlsplit(self.path)
            q = dict(urllib.parse.parse_qsl(u.query))
            if token and self.headers.get('X-Aurora-Token') != token:
                return self.json(403, {'error': 'the helper needs its token (X-Aurora-Token)'})
            who = self.client_address[0]
            if u.path == '/info':
                return self.json(200, {'name': name, 'version': VERSION, 'mode': sources.mode()})
            if u.path == '/search':
                text = q.get('q', '')[:200]
                log(f'{who}: search {text!r}')
                try:
                    return self.json(200, {'results': sources.search(text)})
                except Exception as e:                       # yt-dlp's own failures, in words
                    return self.json(502, {'error': str(e)})
            if u.path == '/play':
                return self.play(who, q.get('video', ''), even(q.get('w'), MAX_W, 320), even(q.get('h'), MAX_H, 240),
                                 int(q['start']) if q.get('start', '').isdigit() else 0)
            return self.json(404, {'error': 'GET /info, /search?q=, /play?video=&w=&h=&start='})

        def play(self, who, video, w, h, start):
            procs = []
            try:
                inp, feed = sources.input_args(video, start)
            except RuntimeError as e:
                return self.json(404, {'error': str(e)})
            if feed:
                procs.append(feed)
            ff = subprocess.Popen(ffmpeg_args(sources.ffmpeg, inp, w, h), stdin=feed.stdout if feed else subprocess.DEVNULL,
                                  stdout=subprocess.PIPE)
            procs.append(ff)
            self.send_response(200)
            self.send_header('Content-Type', 'video/mpeg')
            self.send_header('Connection', 'close')
            self.end_headers()
            log(f'{who}: play {video} at {w}x{h} from {start} s')
            sent = 0
            try:
                while True:
                    b = ff.stdout.read(16384)
                    if not b:
                        break
                    self.wfile.write(b)                      # TCP holds it back while the Mac is paused
                    sent += len(b)
                log(f'{who}: {video} ended, {sent // 1024} KB sent')
            except (BrokenPipeError, ConnectionResetError, OSError):
                log(f'{who}: {video} stopped after {sent // 1024} KB')
            finally:
                for p in procs:
                    try:
                        p.kill()
                    except OSError:
                        pass
    return Handler


def discovery(port, name, mode):
    """answer "AURORA68-VIDEO?" datagrams on the port: how the Mac finds the helper"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(('', port))
    except OSError as e:
        log(f'no discovery on UDP port {port} ({e}): give the Mac the address by hand')
        return
    answer = json.dumps({'name': name, 'version': VERSION, 'mode': mode, 'port': port}).encode()
    while True:
        data, peer = s.recvfrom(512)
        if data.strip() == PROBE:
            s.sendto(answer, peer)


def main():
    ap = argparse.ArgumentParser(description='The Aurora68 video card helper (docs/16-video-card.md)')
    ap.add_argument('--port', type=int, default=int(os.environ.get('AURORA68_VIDEO_PORT', '8069')), help='TCP and UDP port (8069)')
    ap.add_argument('--bind', default='0.0.0.0', help='the address to listen on (all)')
    ap.add_argument('--token', default=os.environ.get('AURORA68_VIDEO_TOKEN', ''), help='require X-Aurora-Token from clients')
    ap.add_argument('--name', default=os.environ.get('AURORA68_VIDEO_NAME', socket.gethostname().split('.')[0]), help='the name the Mac shows')
    ap.add_argument('--file', action='append', default=[], help='file mode: play only these files (no YouTube); may be repeated')
    ap.add_argument('--yt-dlp', dest='ytdlp', default=shutil.which('yt-dlp') or 'yt-dlp')
    ap.add_argument('--ffmpeg', default=shutil.which('ffmpeg') or 'ffmpeg')
    a = ap.parse_args()
    for prog in [a.ffmpeg] + ([] if a.file else [a.ytdlp]):
        if not shutil.which(prog) and not os.path.exists(prog):
            sys.exit(f'{prog} is not installed (macOS: brew install ffmpeg yt-dlp; or use the Docker image)')
    for f in a.file:
        if not os.path.isfile(f):
            sys.exit(f'no such file: {f}')
    sources = Sources(a.file, a.ytdlp, a.ffmpeg)
    srv = ThreadingHTTPServer((a.bind, a.port), make_handler(sources, a.token, a.name))
    srv.daemon_threads = True
    threading.Thread(target=discovery, args=(a.port, a.name, sources.mode()), daemon=True).start()
    signal.signal(signal.SIGTERM, lambda *x: sys.exit(0))
    log(f'aurora68 video helper {VERSION} "{a.name}" on port {a.port}: '
        f'{"file mode, " + str(len(a.file)) + " file(s)" if a.file else "YouTube through yt-dlp"}{", token required" if a.token else ""}')
    srv.serve_forever()


if __name__ == '__main__':
    main()
