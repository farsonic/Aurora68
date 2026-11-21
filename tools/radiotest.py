#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""radiotest.py - a small Icecast-like internet radio station for testing PiPlay's radio in the testbed.

    tools/radiotest.py STREAM.mp3 [--port 18000] [--control 18001] [--metaint 16000] [--title-every 20]
                                  [--burst 65536] [--bind 127.0.0.1]

The testbed has no Internet, so this plays the station: it serves an MP3 file as a live stream, the way
SomaFM's Icecast does (docs/09, "Internet radio"). There is one clock for all listeners, started with
the server: a listener who connects gets the stream from where it is now, with a burst of the last
--burst bytes first (Icecast's burst-size, 64 KB by default), then the frames at their own pace. The file's
ID3 tags and the Xing/Info frame are left out, and it plays round and round. A client that sends
"Icy-MetaData: 1" gets "icy-metaint: N" and a metadata block after every N audio bytes: the title when it
changed (and in the first block), else a zero length byte. The title changes every --title-every
seconds of the clock ("Aurora68 test signal - part 3 (Don't panic) - Cafe" with an e acute and an em dash,
in UTF-8, as Icecast sends it).

Paths:
  /stream     HTTP/1.0 200 OK, Icecast's headers (icy-name, icy-br, icy-genre, icy-metaint ...)
  /icy        "ICY 200 OK", as Shoutcast 1 answers
  /redirect   302 Found to /stream on the host the client named (its Host header)
  /moved      301 Moved Permanently to /redirect (two hops)
  /loop       302 to itself: the client must give up after a few
  /secure     302 to an https:// address
  /pls        a .pls playlist (audio/x-scpls) naming an https:// stream and then /stream
  /aac        200 OK with Content-Type audio/aacp: a kind of stream PiPlay does not play
  anything else  404 Not Found
In the testbed, `PINET_FORWARD="8000=127.0.0.1:18000" python3 tools/pinet.py BASE peer` relays the peer's
port 8000 here, so the Mac reaches the station as http://192.168.68.1:8000/stream, or by name as
http://radio.testbed:8000/stream (pinet answers DNS for every *.testbed name with the peer's address).

The control port (127.0.0.1 only) tests the client's recovery:
  curl http://127.0.0.1:18001/drop            close every listener's connection (a FIN, as a server
                                              restarting does); ?how=reset sends a reset instead
  curl http://127.0.0.1:18001/stall?s=15      stop sending for 15 s without closing
  curl http://127.0.0.1:18001/title?t=Hello   a new title now
  curl http://127.0.0.1:18001/status          listeners, bytes, the title, as JSON
Everything is logged on stdout with the time. Standard library only, like the rest of tools/.
Written on 21 November 2026.
"""
import argparse, json, socket, socketserver, struct, sys, threading, time, urllib.parse

ap = argparse.ArgumentParser()
ap.add_argument('mp3')
ap.add_argument('--port', type=int, default=18000)
ap.add_argument('--control', type=int, default=18001)
ap.add_argument('--bind', default='127.0.0.1')
ap.add_argument('--metaint', type=int, default=16000)
ap.add_argument('--title-every', type=float, default=20.0)
ap.add_argument('--burst', type=int, default=65536)
ap.add_argument('--name', default='Aurora68 Test Radio: a tone that shows every gap')
a = ap.parse_args()


def log(*what):
    print(time.strftime('%H:%M:%S'), *what, flush=True)


# ---- the MP3 file as frames -------------------------------------------------------------------------
BR = {(1, 3): [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],
      (1, 2): [0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384],
      (1, 1): [0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448],
      (2, 3): [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
      (2, 2): [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
      (2, 1): [0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256]}
SR = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}


def frames_of(data):
    """the MPEG audio frames of a file: (bytes, seconds) each, the tags and the Xing/Info frame left out"""
    i, out = 0, []
    if data[:3] == b'ID3':
        size = (data[6] & 0x7f) << 21 | (data[7] & 0x7f) << 14 | (data[8] & 0x7f) << 7 | (data[9] & 0x7f)
        i = 10 + size + (10 if data[5] & 0x10 else 0)
    end = len(data) - (128 if data[-128:-125] == b'TAG' else 0)
    while i + 4 <= end:
        h = data[i:i + 4]
        if h[0] != 0xff or (h[1] & 0xe0) != 0xe0:
            i += 1
            continue
        ver, layer, bri, sri, pad = (h[1] >> 3) & 3, 4 - ((h[1] >> 1) & 3), h[2] >> 4, (h[2] >> 2) & 3, (h[2] >> 1) & 1
        if ver == 1 or layer == 4 or bri in (0, 15) or sri == 3:
            i += 1
            continue
        rate, kbps = SR[ver][sri], BR[(1 if ver == 3 else 2, layer)][bri]
        spf = 384 if layer == 1 else (1152 if ver == 3 or layer == 2 else 576)
        n = (12 * kbps * 1000 // rate + pad) * 4 if layer == 1 else spf // 8 * kbps * 1000 // rate + pad
        f = data[i:i + n]
        if not out and (b'Xing' in f[:64] or b'Info' in f[:64]):
            pass                                       # the encoder's tag frame: no audio
        else:
            out.append((f, spf / rate, kbps, rate))
        i += n
    return out


FR = frames_of(open(a.mp3, 'rb').read())
if not FR:
    sys.exit('radiotest: no MPEG audio frames in ' + a.mp3)
FDUR = FR[0][1]
KBPS = round(sum(f[2] for f in FR) / len(FR))
log(f'{a.mp3}: {len(FR)} frames, {len(FR) * FDUR:.1f} s, {KBPS} kbit/s, {FR[0][3]} Hz; a burst of {a.burst} bytes')
T0 = time.monotonic()


def live_frame(now=None):
    """the frame the clock is at (it counts for ever; the file goes round)"""
    return int(((now or time.monotonic()) - T0) / FDUR)


# ---- titles -------------------------------------------------------------------------------------------
forced_title = [None, 0.0]


def title_now():
    if forced_title[0] is not None:
        return forced_title[0]
    n = int((time.monotonic() - T0) / a.title_every) + 1
    return f"Aurora68 test signal - part {n} (Don't panic) — Café"


def meta_block(title):
    m = f"StreamTitle='{title}';StreamUrl='';".encode('utf-8')
    n = (len(m) + 15) // 16
    return bytes([n]) + m + b'\0' * (n * 16 - len(m))


# ---- listeners ----------------------------------------------------------------------------------------
listeners = {}                                         # id -> dict
lock = threading.Lock()
next_id = [1]


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        s = self.request
        s.settimeout(10)
        req = b''
        try:
            while b'\r\n\r\n' not in req and b'\n\n' not in req and len(req) < 8192:
                d = s.recv(2048)
                if not d:
                    return
                req += d
        except OSError:
            return
        lines = req.decode('latin-1').replace('\r', '').split('\n')
        parts = lines[0].split()
        path = parts[1] if len(parts) > 1 else '/'
        hdr = {}
        for ln in lines[1:]:
            if ':' in ln:
                k, v = ln.split(':', 1)
                hdr[k.strip().lower()] = v.strip()
        host = hdr.get('host', '192.168.68.1:8000')
        log(f'{self.client_address[0]}: {lines[0]!r}, Host {host!r}, User-Agent {hdr.get("user-agent")!r}, '
            f'Icy-MetaData {hdr.get("icy-metadata")!r}')

        def answer(status, headers, body=b''):
            h = f'HTTP/1.0 {status}\r\n' + ''.join(f'{k}: {v}\r\n' for k, v in headers) + '\r\n'
            s.sendall(h.encode('latin-1') + body)
            log(f'  -> {status}' + (f', {dict(headers).get("Location")}' if dict(headers).get('Location') else ''))

        if path in ('/redirect',):
            return answer('302 Found', [('Location', f'http://{host}/stream'), ('Content-Length', '0')])
        if path == '/moved':
            return answer('301 Moved Permanently', [('Location', f'http://{host}/redirect'), ('Content-Length', '0')])
        if path == '/loop':
            return answer('302 Found', [('Location', f'http://{host}/loop')])
        if path == '/secure':
            return answer('302 Found', [('Location', 'https://ice2.example.invalid/stream')])
        if path == '/pls':
            body = (f'[playlist]\nnumberofentries=2\nFile1=https://ice6.example.invalid/stream\nTitle1=secure\n'
                    f'File2=http://{host}/stream\nTitle2=plain\nLength2=-1\nVersion=2\n').encode()
            return answer('200 OK', [('Content-Type', 'audio/x-scpls'), ('Content-Length', str(len(body)))], body)
        if path == '/aac':
            return answer('200 OK', [('Content-Type', 'audio/aacp'), ('icy-br', '64')])
        if path not in ('/stream', '/icy', '/'):
            return answer('404 Not Found', [('Content-Type', 'text/html')], b'<html><body>404</body></html>\r\n')
        meta = hdr.get('icy-metadata') == '1'
        head = ('ICY 200 OK\r\n' if path == '/icy' else
                'HTTP/1.0 200 OK\r\nServer: radiotest.py (Icecast-like)\r\nContent-Type: audio/mpeg\r\nCache-Control: no-cache\r\n')
        head += (f'icy-name: {a.name}\r\nicy-genre: Test\r\nicy-br: {KBPS}\r\nicy-pub: 0\r\n'
                 f'icy-url: http://radio.testbed/\r\n')
        if meta:
            head += f'icy-metaint: {a.metaint}\r\n'
        head += '\r\n'
        with lock:
            me = {'id': next_id[0], 'addr': self.client_address[0], 'sock': s, 'sent': 0, 'audio': 0, 'drop': None,
                  'stall_until': 0.0, 'since': time.time(), 'meta': meta, 'titles': 0}
            next_id[0] += 1
            listeners[me['id']] = me
        try:
            s.sendall(head.encode('latin-1'))
            self.stream(s, me)
        except OSError as e:
            log(f'listener {me["id"]}: {e}')
        finally:
            with lock:
                listeners.pop(me['id'], None)
            log(f'listener {me["id"]} gone after {me["audio"]} audio bytes, {me["titles"]} titles, '
                f'{time.time() - me["since"]:.1f} s')

    def stream(self, s, me):
        burst_frames = max(1, a.burst // len(FR[0][0]))
        k = max(0, live_frame() - burst_frames)        # the burst: the last 64 KB, at once
        left = a.metaint                                # audio bytes before the next metadata block
        last_title = None
        log(f'listener {me["id"]}: from frame {k} ({burst_frames} frames of burst)'
            + (f', metadata every {a.metaint} bytes' if me['meta'] else ', no metadata'))
        while True:
            if me['drop']:
                if me['drop'] == 'reset':
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                log(f'listener {me["id"]}: dropped ({me["drop"]}) at {me["audio"]} audio bytes')
                s.close()
                return
            now = time.monotonic()
            if now < me['stall_until'] or k > live_frame(now):
                time.sleep(0.01)
                continue
            data = FR[k % len(FR)][0]
            k += 1
            out = b''
            while data:
                if not me['meta']:
                    out += data
                    me['audio'] += len(data)
                    data = b''
                    break
                n = min(left, len(data))
                out += data[:n]
                me['audio'] += n
                data, left = data[n:], left - n
                if left == 0:
                    t = title_now()
                    if t != last_title:
                        out += meta_block(t)
                        last_title = t
                        me['titles'] += 1
                        log(f'listener {me["id"]}: title {t!r} at {me["audio"]} audio bytes')
                    else:
                        out += b'\0'
                    left = a.metaint
            s.sendall(out)
            me['sent'] += len(out)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class Control(socketserver.BaseRequestHandler):
    def handle(self):
        s = self.request
        req = s.recv(4096).decode('latin-1')
        url = urllib.parse.urlparse(req.split()[1] if len(req.split()) > 1 else '/')
        q = urllib.parse.parse_qs(url.query)
        body = {}
        with lock:
            if url.path == '/drop':
                how = q.get('how', ['close'])[0]
                for m in listeners.values():
                    m['drop'] = how
                body = {'dropped': len(listeners), 'how': how}
            elif url.path == '/stall':
                secs = float(q.get('s', ['15'])[0])
                for m in listeners.values():
                    m['stall_until'] = time.monotonic() + secs
                body = {'stalled': len(listeners), 'seconds': secs}
            elif url.path == '/title':
                forced_title[0] = q.get('t', [''])[0]
                body = {'title': forced_title[0]}
            body.update({'listeners': [{k: m[k] for k in ('id', 'addr', 'sent', 'audio', 'titles')} for m in listeners.values()],
                         'title': title_now(), 'clock': round(time.monotonic() - T0, 1)})
        log(f'control {url.path}: {body}')
        b = json.dumps(body).encode() + b'\n'
        s.sendall(b'HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n' % len(b) + b)


srv = Server((a.bind, a.port), Handler)
ctl = Server(('127.0.0.1', a.control), Control)
threading.Thread(target=ctl.serve_forever, daemon=True).start()
log(f'station on {a.bind}:{a.port}, control on 127.0.0.1:{a.control}, titles every {a.title_every} s')
try:
    srv.serve_forever()
except KeyboardInterrupt:
    pass
