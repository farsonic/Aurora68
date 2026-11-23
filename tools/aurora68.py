#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""aurora68.py - the command line for the Pi's remote control (docs/13-remote-control.md).

The kernel serves the Mac's screen, input, log and state over HTTP when cmdline.txt has
`remote=on remote.token=...`. This is a plain client for it: for a person at a terminal, for
scripts, and for checking the server without an MCP host. Python 3, standard library only.

  aurora68.py state                              the JSON status
  aurora68.py screenshot out.png [--scale 2]     the first screen as PNG (--display 1: the second, HDMI on the SE)
  aurora68.py windows                            the Mac's windows, front to back (id, size, screens, title)
  aurora68.py screenshot --window "Untitled" out.png [--content] [--scale 2]
                                                 one window: the frontmost visible one whose title has the text
                                                 (or an id from `windows`, 0x...); --content without its frame;
                                                 --scale 2 doubles each pixel here (on whole screens it shrinks)
  aurora68.py screenshot --window "Untitled" --front [--restore] out.png
                                                 the window brought to the front first, then taken (--restore: the
                                                 program that was in front is put back afterwards)
  aurora68.py screenshot --rect 0,0,640,480 out.png   a rectangle of the desktop, global coordinates
  aurora68.py click X Y [--double] [--mods cmd]  a click at a point of the desktop (pixels, 0,0 the main screen's
                                                 top left; --display N: a point of screen N instead)
  aurora68.py move X Y | drag X Y TO_X TO_Y
  aurora68.py type "some text"                   typed on the US keyboard layout
  aurora68.py key return [--mods cmd]            one key: return, escape, tab, left ..., or a character
  aurora68.py log [--since N] [--tags BOOT,CRASH] [--follow]
  aurora68.py log --records                      the [BOOT] and [CRASH] lines kept since the Pi started
  aurora68.py restart [mac|nmi|pi]
  aurora68.py input '[{"type": "click", "x": 20, "y": 10}]'     events as the server takes them

Files and programs on the Mac (the Aurora68 Agent must be installed there; docs/13, "Files and
applications"). Paths are the Mac's: "Mac HD:Folder:File", and ":Folder:File" on the startup disk.
  aurora68.py ls [PATH] [--json]                 a folder's files (no PATH: the disks)
  aurora68.py info PATH [--json]                 one file or folder
  aurora68.py pull PATH [LOCAL] [--macbinary] [--rsrc]
                                                 a file to the computer: its data fork, or with --macbinary both
                                                 forks, type, creator and dates as MacBinary II (LOCAL.bin)
  aurora68.py push LOCAL PATH [--force] [--mkdir] [--type TEXT --creator ttxt] [--raw] [--text]
                                                 a file to the Mac: MacBinary input keeps its forks, type and creator;
                                                 anything else is the data fork, typed by its extension (.txt TEXT/ttxt,
                                                 .mp3 MPG3/PIPY ...); PATH ending in ':' is a folder; never overwrites
                                                 without --force, makes missing folders only with --mkdir;
                                                 --text turns UTF-8 and newlines into the Mac's (pull --text back)
                                                 push and pull show their progress on stderr: one line redrawn on a
                                                 terminal, a line every five seconds otherwise, nothing with -q
  aurora68.py mkdir PATH [-p] | rm PATH | mv PATH NEWPATH
                                                 rm takes a file or an empty folder, never more; mv stays on one disk
  aurora68.py open PATH                          launch an application, or open a document with its application
  aurora68.py apps [--json]                      the programs that run (System 6: the one in front)
  aurora68.py quit NAME | quit --creator ttxt    ask a program to quit (System 7)

Aurora68 Setup and Recovery (docs/08 item 106): a start in which the Mac stays off and the Pi serves its SD card; the PIN
is the one on the Mac's screen (--pin or $AURORA68_PIN; --url defaults to http://aurora68.local).
  aurora68.py recovery --pin N state | ls | put FILE [NAME] | get NAME [OUT] | rm NAME | mv A B
  aurora68.py recovery --pin N cmdline [TEXT|@FILE] | config [TEXT|@FILE] | opt [KEY=VALUE|-KEY|+WORD ...] [--dry]
  aurora68.py recovery --pin N wifi [show|scan|test|save] [--ssid S --psk P --country GB]
  aurora68.py recovery --pin N log [--now] | rollback [--force] | reboot [normal|recovery]
  aurora68.py recovery --pin N display [show] [--json]   the HDMI monitor (EDID), its modes judged, the suggestion
  aurora68.py recovery --pin N display use WxH[@Hz]|auto [--rotate N] [--no-confirm] [--add-include] [--keep-screen]
                                                 aurora68-display.txt written (.bak kept); restart normally applies it
  aurora68.py recovery --pin N display rotate N | include   the rotation alone; the include line into config.txt
  aurora68.py restart recovery                   (the remote control) the Pi restarts into recovery

Where: --url (default $AURORA68_URL, else http://aurora68-pi.local), --token (default
$AURORA68_TOKEN). In the QEMU testbed, --semihost BASE talks to the kernel through the files of
mac.remotefile=BASE instead of the network.
"""
import argparse, json, os, re, socket, struct, sys, threading, time, urllib.error, urllib.parse, urllib.request


class Remote:
    def __init__(self, url=None, token=None, semihost=None, timeout=30):
        self.url = (url or os.environ.get('AURORA68_URL') or 'http://aurora68-pi.local').rstrip('/')
        self.token = token if token is not None else os.environ.get('AURORA68_TOKEN', '')
        self.semihost = semihost
        self.timeout = timeout
        self.probed = False

    def _probe(self):
        """Once per run: a URL without a port that does not answer on port 80 within two seconds is tried
        on 8068, the port the kernel keeps when it shares the Mac's address on the WiFi (docs/13); the one
        that answers is used from then on."""
        if self.probed or self.semihost:
            return
        self.probed = True
        u = urllib.parse.urlsplit(self.url)
        if u.port or u.scheme != 'http' or not u.hostname:
            return
        for port in (80, 8068):
            try:
                socket.create_connection((u.hostname, port), timeout=2).close()
            except OSError:
                continue
            if port != 80:
                self.url = f'http://{u.hostname}:{port}{u.path}'.rstrip('/')
                print(f'aurora68.py: {u.hostname} did not answer on port 80; {self.url} did', file=sys.stderr)
            return

    def request(self, method, path, body=None, ctype='application/json'):
        """(status, headers, bytes) of one request"""
        if self.semihost:
            return self._semihost(method, path, body)
        self._probe()
        req = urllib.request.Request(self.url + path, data=body, method=method)
        if self.token:
            req.add_header('X-Aurora-Token', self.token)
        if body is not None:
            req.add_header('Content-Type', ctype)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.status, dict(r.headers), r.read()
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), e.read()

    def stream(self, method, path, upload=None, size=0, sink=None, on_sent=None, on_received=None):
        """(status, headers, bytes or None) of a request whose body is the open file upload (size bytes) and
        whose answer, if sink is given, is written to it as it comes: a file of any size in either direction.
        on_sent(n): the body's bytes as the socket takes them; on_received(n, length, headers): the answer's"""
        if self.semihost:
            status, h, data = self._semihost(method, path, upload.read() if upload else None)
            if sink and status == 200:
                sink.write(data)
                if on_received: on_received(len(data), len(data), {k.lower(): v for k, v in h.items()})
                return status, h, None
            return status, h, data
        self._probe()
        body = _Counting(upload, on_sent) if upload is not None and on_sent else upload
        req = urllib.request.Request(self.url + path, data=body, method=method)
        if self.token:
            req.add_header('X-Aurora-Token', self.token)
        if upload is not None:
            req.add_header('Content-Type', 'application/octet-stream')
            req.add_header('Content-Length', str(size))
        try:
            with urllib.request.urlopen(req, timeout=max(self.timeout, 60)) as r:
                if sink is None or r.status != 200:
                    return r.status, dict(r.headers), r.read()
                got, length, hl = 0, int(r.headers.get('Content-Length') or 0), {k.lower(): v for k, v in r.headers.items()}
                read = getattr(r, 'read1', r.read)          # what has come, not a full 64 KiB: the progress moves smoothly
                while True:
                    b = read(65536)
                    if not b:
                        break
                    sink.write(b); got += len(b)
                    if on_received: on_received(got, length, hl)
                return r.status, dict(r.headers), None
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), e.read()

    def _semihost(self, method, path, body):
        # mac.remotefile=BASE: a request is {u32 length, bytes} appended to BASE.in; the answer comes
        # as pieces {u32 length, bytes} appended to BASE.out, a length of 0 ending it
        base = self.semihost
        start = os.path.getsize(base + '.out') if os.path.exists(base + '.out') else 0
        raw = f'{method} {path} HTTP/1.0\r\nContent-Length: {len(body or b"")}\r\n\r\n'.encode() + (body or b'')
        with open(base + '.in', 'ab') as f:
            f.write(struct.pack('>I', len(raw)) + raw)
        data, pos, deadline = b'', start, time.time() + self.timeout
        while time.time() < deadline:
            with open(base + '.out', 'rb') as f:
                f.seek(pos); chunk = f.read()
            buf = chunk; done = False
            while len(buf) >= 4:
                n = struct.unpack('>I', buf[:4])[0]
                if n == 0:
                    done = True; break
                if len(buf) < 4 + n:
                    break
                data += buf[4:4 + n]; buf = buf[4 + n:]; pos += 4 + n
            if done:
                break
            time.sleep(0.05)
        else:
            raise TimeoutError('no answer from the kernel through ' + base + '.out')
        head, _, payload = data.partition(b'\r\n\r\n')
        lines = head.decode('latin-1').split('\r\n')
        status = int(lines[0].split()[1])
        headers = dict(l.split(': ', 1) for l in lines[1:] if ': ' in l)
        return status, headers, payload

    def _json(self, method, path, body=None):
        status, _, data = self.request(method, path, None if body is None else json.dumps(body).encode())
        try:
            obj = json.loads(data.decode('latin-1') or '{}')
        except ValueError:
            obj = {'text': data.decode('latin-1')}
        if status != 200:
            raise RuntimeError(f'{status}: {obj.get("error", obj)}')
        return obj

    def state(self):
        return self._json('GET', '/state')

    def screenshot(self, scale=1, hdmi=False, display=0):
        q = f'/screen.png?scale={int(scale)}&display={int(display)}' + ('&src=hdmi' if hdmi else '')
        for attempt in range(20):                     # one screenshot at a time: another one may be under way
            status, _, data = self.request('GET', q)
            if status == 200:
                return data
            if status != 503 or b'another' not in data:
                raise RuntimeError(f'{status}: {data.decode("latin-1").strip()}')
            time.sleep(0.25)
        raise RuntimeError('the screen stayed busy')

    def windows(self):
        status, headers, data = self.request('GET', '/windows')
        if status != 200:
            raise RuntimeError(f'{status}: {data.decode("utf-8", "replace").strip()}')
        return json.loads(data.decode('utf-8'))

    def window_screenshot(self, window=None, rect=None, screen=None, content=False, scale=1, front=False, restore=False):
        """(png, headers) of a window (an id 0x..., or a title substring), a rectangle (l, t, r, b) or a screen"""
        if window is not None:
            w = str(window)
            if re.fullmatch(r'0x[0-9a-fA-F]+', w):
                q = 'window=' + w
            else:
                q = 'title=' + urllib.parse.quote(w, safe='')
        elif rect is not None:
            q = 'rect=' + ','.join(str(int(v)) for v in rect)
        else:
            q = f'screen={int(screen or 0)}'
        q = f'/screenshot?{q}&scale={int(scale)}' + ('&content=1' if content else '') + ('&front=1' if front else '') + ('&restore=1' if restore else '')
        for attempt in range(20):
            status, headers, data = self.request('GET', q)
            if status == 200:
                return data, {k.lower(): v for k, v in headers.items()}
            if status != 503 or b'another' not in data:
                try:
                    msg = json.loads(data.decode('utf-8')).get('error', '')
                except ValueError:
                    msg = data.decode('utf-8', 'replace').strip()
                raise RuntimeError(f'{status}: {msg}')
            time.sleep(0.25)
        raise RuntimeError('the screen stayed busy')

    def input(self, events, wait=True):
        return self._json('POST', '/input' + ('?wait=1' if wait else ''), events)

    def records(self):
        status, _, data = self.request('GET', '/log?records=1')
        if status != 200:
            raise RuntimeError(f'{status}: {data.decode("latin-1").strip()}')
        return data.decode('latin-1')

    def log(self, since=None, tags=None):
        q = '/log'
        args = []
        if since is not None: args.append(f'since={int(since)}')
        if tags: args.append('tags=' + tags)
        if args: q += '?' + '&'.join(args)
        status, headers, data = self.request('GET', q)
        if status != 200:
            raise RuntimeError(f'{status}: {data.decode("latin-1").strip()}')
        h = {k.lower(): v for k, v in headers.items()}
        return data.decode('latin-1'), int(h.get('x-log-next', 0)), int(h.get('x-log-lost', 0))

    def restart(self, how='mac'):
        return self._json('POST', '/restart?how=' + how)

    # ---- files and programs, through the Aurora68 Agent on the Mac (docs/13, "Files and applications") ----
    def _files(self, method, path, upload=None, size=0, sink=None, on_sent=None, on_received=None):
        """a /files or /apps request; one file job runs at a time, so a busy answer is tried again"""
        for attempt in range(40):
            if upload is not None:
                upload.seek(0)
            status, h, data = self.stream(method, path, upload, size, sink, on_sent, on_received)
            if status == 503 and data and b'another file job' in data:
                time.sleep(0.5); continue
            if status != 200:
                try:
                    msg = json.loads(data.decode('utf-8')).get('error', '')
                except (ValueError, AttributeError):
                    msg = (data or b'').decode('utf-8', 'replace').strip()
                raise RuntimeError(f'{status}: {msg}')
            return {k.lower(): v for k, v in h.items()}, data
        raise RuntimeError('another file job kept the agent busy')

    def _fjson(self, method, path):
        return json.loads(self._files(method, path)[1].decode('utf-8'))

    def ls(self, path=''):
        return self._fjson('GET', '/files/list?path=' + q(path))

    def info(self, path):
        return self._fjson('GET', '/files/info?path=' + q(path))

    def progress(self):
        """the kernel's /files/progress (the transfer under way, asked on a connection of its own): a dict; {} when
        the kernel has none (before 23 November 2026: 404), None when it did not answer"""
        if self.semihost:
            return {}                                   # one request at a time on that channel
        req = urllib.request.Request(self.url + '/files/progress')
        if self.token:
            req.add_header('X-Aurora-Token', self.token)
        try:
            with urllib.request.urlopen(req, timeout=3) as r:
                return json.loads(r.read().decode('utf-8'))
        except urllib.error.HTTPError as e:
            return {} if e.code == 404 else None
        except (OSError, ValueError):
            return None

    def pull(self, path, sink, macbinary=False, rsrc=False, progress=None):
        """the file into sink; its headers (x-aurora-type ...). progress: a Progress, fed with the bytes as they arrive
        (each has been read on the Mac), and for MacBinary the fork they belong to"""
        def received(n, length, h):
            fork = None
            if macbinary:
                dlen = int(h.get('x-aurora-data-length') or 0)
                fork = 'header' if n <= 128 else 'data' if n <= 128 + ((dlen + 127) & ~127) and dlen else 'rsrc'
            progress.update(n, length, fork=fork, what='received')
        h, _ = self._files('GET', '/files/data?path=' + q(path) + ('&macbinary=1' if macbinary else '') + ('&fork=rsrc' if rsrc else ''),
                           sink=sink, on_received=received if progress else None)
        return h

    def push(self, f, size, path, macbinary=False, ftype=None, creator=None, force=False, mkdir=False, progress=None):
        """the file f (size bytes) to path; the kernel's answer. progress: a Progress, fed with what the Mac has written
        (polled from /files/progress on a second connection), or with an older kernel the bytes sent"""
        args = '/files/data?path=' + q(path) + ('&macbinary=1' if macbinary else '')
        if ftype: args += '&type=' + q(ftype)
        if creator: args += '&creator=' + q(creator)
        if force: args += '&force=1'
        if mkdir: args += '&mkdir=1'
        if progress is None:
            return json.loads(self._files('PUT', args, upload=f, size=size)[1].decode('utf-8'))
        # served: None until the kernel's first answer (True: it has /files/progress; False: an older kernel, or the
        # semihosting channel, which takes one request at a time); until then, for a second, no bytes "sent" are shown:
        # the socket takes the first tens of KB at once, which says nothing about the Mac
        stop, mac, t0 = threading.Event(), {'seen': False, 'served': False if self.semihost else None, 'sent': 0}, time.time()

        def sent(n):
            mac['sent'] = n
            if not mac['seen'] and (mac['served'] is False or time.time() - t0 > 1.5):
                progress.update(n, size, what='sent')

        def poll():
            while not stop.wait(0.4):
                if mac['served'] is not False:
                    p = self.progress()
                    if p == {}:                         # an older kernel: the bytes sent are all there is
                        mac['served'] = False
                    elif p:
                        mac['served'] = True
                        if p.get('active') and p.get('job') == 'push':
                            mac['seen'] = True
                            received = p.get('received', 0)
                            progress.update(p.get('done', 0), p.get('total') or None, fork=p.get('fork') if macbinary else None,
                                            what='written on the Mac',
                                            note=f'(the Pi has {human(received)})' if received and size and received < size else '')
                        continue
                if mac['served'] is False and not mac['seen']:   # the bytes sent; once all are, the wait for the Mac's answer
                    progress.update(mac['sent'], size, what='sent',
                                    note=f'waiting for the Mac {hms(time.time() - t0)}' if size and mac['sent'] >= size else '')
        t = threading.Thread(target=poll, daemon=True)
        if not self.semihost:                           # one request at a time there: no progress until the end
            t.start()
        try:
            return json.loads(self._files('PUT', args, upload=f, size=size, on_sent=sent)[1].decode('utf-8'))
        finally:
            stop.set()
            if t.is_alive():
                t.join(4)

    def mkdir(self, path, parents=False):
        return self._fjson('POST', '/files/mkdir?path=' + q(path) + ('&parents=1' if parents else ''))

    def rm(self, path):
        return self._fjson('POST', '/files/delete?path=' + q(path))

    def mv(self, path, to):
        return self._fjson('POST', '/files/rename?path=' + q(path) + '&to=' + q(to))

    def open(self, path):
        return self._fjson('POST', '/apps/open?path=' + q(path))

    def apps(self):
        return self._fjson('GET', '/apps')

    def quit(self, name=None, creator=None):
        return self._fjson('POST', '/apps/quit?' + ('creator=' + q(creator) if creator else 'name=' + q(name)))


def q(s):
    return urllib.parse.quote(s, safe='')


# ---- MacBinary II (tools/mac/rsrc.py writes the same) ----
def crc_xmodem(b):
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def macbinary_info(head, size):
    """the name, type, creator and forks' lengths of a MacBinary file's first 128 bytes, or None"""
    if len(head) < 128 or head[0] or head[74] or not 1 <= head[1] <= 63:
        return None
    dlen, rlen = struct.unpack('>II', head[83:91])
    if head[122] >= 129 and struct.unpack('>H', head[124:126])[0] != crc_xmodem(head[:124]):
        return None
    if head[122] < 129 and head[82]:
        return None
    pad = lambda n: (n + 127) & ~127
    if not 128 + pad(dlen) + rlen <= size <= 128 + pad(dlen) + pad(rlen) + 128:
        return None
    return {'name': head[2:2 + head[1]].decode('mac_roman'), 'type': head[65:69].decode('mac_roman'),
            'creator': head[69:73].decode('mac_roman'), 'data': dlen, 'rsrc': rlen}


# the type and creator of a file sent as it is, by its extension (Aurora68 Play's creator is PIPY)
TYPES = {'.txt': ('TEXT', 'ttxt'), '.text': ('TEXT', 'ttxt'), '.mp3': ('MPG3', 'PIPY'), '.aif': ('AIFF', 'PIPY'),
         '.aiff': ('AIFF', 'PIPY'), '.wav': ('WAVE', 'PIPY'),
         '.mpg': ('MPEG', 'A68V'), '.mpeg': ('MPEG', 'A68V'), '.m1v': ('MPEG', 'A68V')}   # MPEG-1 films: Aurora68 Video, MoviePlayer


def mac_name(local):
    """a local file's name as a Mac name: 31 characters, no colons"""
    n = os.path.basename(local).replace(':', '-')
    return n[:31]


def human(n):
    for unit in ('bytes', 'KB', 'MB', 'GB'):
        if n < 1024 or unit == 'GB':
            return f'{int(n)} {unit}' if unit == 'bytes' else f'{n:.1f} {unit}'
        n /= 1024


def hms(s):
    s = int(s + 0.5)
    return f'{s // 3600}:{s // 60 % 60:02d}:{s % 60:02d}' if s >= 3600 else f'{s // 60}:{s % 60:02d}'


class Progress:
    """A transfer's progress on stderr (23 November 2026; the owner: "we need more feedback on progress when pushing or
    downloading files"). On a terminal one line redrawn in place, at most ten times a second: what, the fork of a
    MacBinary file, bytes done of the total, per cent, the rate over the last few seconds and the time left. Not on a
    terminal (a log, a script): a plain line every five seconds. quiet: nothing. `what` says whose bytes the numbers
    are: "written on the Mac" (the kernel's /files/progress), "received", or "sent" (an older kernel: the bytes this
    end's socket took, which run ahead of the Mac by what the Pi holds)."""
    def __init__(self, label, total=0, quiet=False, stream=None):
        self.label, self.total, self.quiet = label, total, quiet
        self.stream = stream or sys.stderr
        self.tty = not quiet and self.stream.isatty()
        self.t0 = time.time(); self.width = 0
        self.shown = 0.0 if self.tty else self.t0           # a log's first line after five seconds, not at 0%
        self.done, self.fork, self.what, self.note = 0, '', '', ''
        self.samples = [(self.t0, 0)]
        self.lock = threading.Lock()                        # a push is fed by its upload and by the kernel's progress

    def update(self, done, total=None, fork=None, what=None, note=None):
        with self.lock:
            if total: self.total = total
            if fork is not None: self.fork = fork
            if what is not None and what != self.what:      # other bytes from now on (the Mac's, not the socket's): a new rate
                if self.what: self.samples = [(self.t0, 0)]
                self.what = what
            if note is not None: self.note = note
            now = time.time()
            if done != self.done:
                self.samples.append((now, done))
                while len(self.samples) > 2 and now - self.samples[1][0] > 4.0:
                    self.samples.pop(0)
            self.done = done
            if self.quiet or now - self.shown < (0.1 if self.tty else 5.0):
                return
            self.shown = now
            self.draw(now)

    def rate(self, now):
        (ta, da), (tb, db) = self.samples[0], self.samples[-1]
        if tb - ta < 0.3:                                   # too short a window: since the start
            ta, da = self.t0, 0
        return (db - da) / max(now - ta, 1e-3) if db > da else 0.0

    def line(self, now):
        r = self.rate(now)
        parts = [self.label]
        if self.fork: parts.append(f'[{self.fork}]')
        if self.total:
            pct = 100.0 * self.done / self.total
            parts.append(f'{human(self.done)} of {human(self.total)} {pct:3.0f}%')
        else:
            parts.append(human(self.done))
        if self.what: parts.append(self.what)
        parts.append(f'{human(r)}/s')
        if self.total and r > 0 and self.done < self.total:
            parts.append(f'{hms((self.total - self.done) / r)} left')
        if self.note: parts.append(self.note)
        return '  '.join(parts)

    def draw(self, now):
        text = self.line(now)
        if self.tty:
            cols = 0
            try:
                cols = os.get_terminal_size(self.stream.fileno()).columns
            except (OSError, ValueError):
                pass
            if cols and len(text) >= cols:
                text = text[:cols - 1]
            self.stream.write('\r' + text + ' ' * max(0, self.width - len(text)))
            self.width = len(text)
        else:
            self.stream.write(text + '\n')
        self.stream.flush()

    def finish(self):
        with self.lock:
            self.quiet = True                               # a late update from the poller draws nothing more
        if self.tty and self.width:
            self.stream.write('\r' + ' ' * self.width + '\r'); self.stream.flush()
        self.width = 0


class _Counting:
    """a file object whose reads are counted (an upload's bytes as this end's socket takes them)"""
    def __init__(self, f, cb):
        self.f, self.cb, self.n = f, cb, 0

    def read(self, n=-1):
        b = self.f.read(n)
        self.n += len(b); self.cb(self.n)
        return b

    def seek(self, *a):
        self.n = 0
        return self.f.seek(*a)


def ls_table(r):
    es = r['entries']
    if not r.get('path'):
        print(f'{"disk":28} {"free":>10} {"size":>10}')
        for e in es:
            print(f'{e["name"] + (" (startup)" if e.get("startup") else ""):28} {human(e["free_kb"] * 1024):>10} {human(e["size_kb"] * 1024):>10}')
        return
    for e in es:
        if e['kind'] == 'folder':
            print(f'{"folder":9} {"":9} {e["entries"]:>9} items {"":>9}  {e["modified"][:16].replace("T", " "):16}  {e["name"]}:')
        else:
            print(f'{e["type"]:4}/{e["creator"]:4} {e["data"]:>10} {e["rsrc"]:>10}  {"":4}{e["modified"][:16].replace("T", " "):16}  {e["name"]}')
    print(f'{r["count"]} items', file=sys.stderr)


def png_size(data):
    return struct.unpack('>II', data[16:24]) if data[:8] == b'\x89PNG\r\n\x1a\n' else (0, 0)


# ---- Aurora68 Setup and Recovery (docs/08 item 106) -------------------------------------------------------------
# A recovery start of the kernel serves the Pi's SD card instead of the Mac: `aurora68.py recovery --pin N ...`.
# Every request carries the PIN the Mac's screen shows (X-Aurora-Pin); no token, and the remote need not be on.
class Recovery:
    def __init__(self, url, pin):
        self.url = (url or os.environ.get('AURORA68_RECOVERY_URL') or 'http://aurora68.local').rstrip('/')
        self.pin = pin or os.environ.get('AURORA68_PIN') or ''
        if not self.pin:
            raise RuntimeError('the PIN shown on the Mac\'s screen is needed: --pin NNNN (or $AURORA68_PIN)')
        u = urllib.parse.urlsplit(self.url)
        self.host, self.port = u.hostname, u.port or 80

    def conn(self, timeout=60):
        import http.client
        return http.client.HTTPConnection(self.host, self.port, timeout=timeout)

    def call(self, method, path, body=None, raw=False, timeout=60):
        c = self.conn(timeout)
        c.request(method, path, body=body, headers={'X-Aurora-Pin': self.pin})
        r = c.getresponse()
        data = r.read()
        c.close()
        if r.status != 200:
            try:
                msg = json.loads(data).get('error', data.decode(errors='replace'))
            except ValueError:
                msg = data.decode(errors='replace')
            raise RuntimeError(f'{method} {path}: {r.status} {msg}')
        return data if raw else json.loads(data)

    def get_file(self, name, local, progress=None):
        """a file of the card into local, as it comes; its length"""
        c = self.conn(600)
        c.request('GET', '/r/file?name=' + urllib.parse.quote(name), headers={'X-Aurora-Pin': self.pin})
        r = c.getresponse()
        if r.status != 200:
            data = r.read(); c.close()
            try:
                msg = json.loads(data).get('error', data.decode(errors='replace'))
            except ValueError:
                msg = data.decode(errors='replace')
            raise RuntimeError(f'GET /r/file: {r.status} {msg}')
        length, got = int(r.getheader('Content-Length') or 0), 0
        with open(local, 'wb') as f:
            while True:
                b = r.read1(65536) if hasattr(r, 'read1') else r.read(65536)
                if not b:
                    break
                f.write(b); got += len(b)
                if progress:
                    progress.update(got, length, what='received')
        c.close()
        if length and got != length:
            raise RuntimeError(f'{name}: {got} of {length} bytes came ({local} kept)')
        return got

    def put_file(self, local, name, progress=None):
        """the file streamed to the card; the kernel reads it back and answers with its CRC32. progress: a Progress,
        fed with the bytes on the card (the upload's "written" in /r/state, asked on a second connection), or the
        bytes sent while /r/state does not say"""
        import zlib
        size = os.path.getsize(local)
        c = self.conn(600)
        c.putrequest('PUT', '/r/file?name=' + urllib.parse.quote(name))
        c.putheader('X-Aurora-Pin', self.pin); c.putheader('Content-Length', str(size)); c.endheaders()
        crc, sent, t0 = 0, 0, time.time()
        stop, card = threading.Event(), {'seen': False}

        def poll():
            while not stop.wait(0.5):
                try:
                    u = self.call('GET', '/r/state', timeout=3).get('upload', {})
                except (RuntimeError, OSError, ValueError):
                    continue
                if u.get('state') in ('receiving', 'checking') and u.get('total') == size:
                    card['seen'] = True
                    progress.update(u.get('written', 0), size, what='on the card',
                                    note='reading it back' if u.get('state') == 'checking' else '')
        t = threading.Thread(target=poll, daemon=True)
        if progress:
            t.start()
        try:
            with open(local, 'rb') as f:
                while True:
                    b = f.read(65536)
                    if not b:
                        break
                    c.send(b); crc = zlib.crc32(b, crc); sent += len(b)
                    if progress and not card['seen']:
                        progress.update(sent, size, what='sent')
            r = c.getresponse(); data = r.read(); c.close()
        finally:
            stop.set()
            if progress:
                t.join(4); progress.finish()
        try:
            j = json.loads(data)
        except ValueError:
            j = {'error': data.decode(errors='replace')}
        if r.status != 200:
            raise RuntimeError(f'{name}: {r.status} {j.get("error")}')
        j['local_crc32'] = f'{crc & 0xffffffff:08x}'
        j['seconds'] = round(time.time() - t0, 1)
        if j.get('crc32') != j['local_crc32']:
            raise RuntimeError(f'{name}: the card holds CRC32 {j.get("crc32")}, the file here is {j["local_crc32"]}')
        return j


def recovery_main(a):
    r = Recovery(a.url, a.pin)
    c = a.rcmd
    if c == 'state':
        print(json.dumps(r.call('GET', '/r/state'), indent=2))
    elif c == 'ls':
        x = r.call('GET', '/r/files')
        for f in x['files']:
            print(f'{human(f["size"]) if not f["folder"] else "folder":>10}  {f["name"]}{"/" if f["folder"] else ""}')
        print(f'{x["count"]} entries; {x.get("card", "")}' + (f', {human(x["free_kb"] * 1024)} free' if 'free_kb' in x else ''))
    elif c == 'put':
        j = r.put_file(a.file, a.name or os.path.basename(a.file), progress=Progress('put ' + (a.name or os.path.basename(a.file)), quiet=a.quiet))
        print(f'{j["name"]}: {human(j["size"])} in {j["seconds"]} s, CRC32 {j["crc32"]} read back from the card and the same as here'
              + (f'; {j["check"]}' if j.get('check') else '') + ('; the old kernel kept as kernel8.old' if j.get('installed') else '')
              + (f'; the old one kept as {j["backup"]}' if j.get('backup') else ''))
    elif c == 'get':
        out = a.out or a.name
        pr, t0 = Progress('get ' + a.name, quiet=a.quiet), time.time()
        try:
            n = r.get_file(a.name, out + '.part', pr)
        finally:
            pr.finish()
        os.replace(out + '.part', out)
        print(f'{out}: {human(n)} in {time.time() - t0:.1f} s')
    elif c == 'rm':
        r.call('POST', '/r/rm?name=' + urllib.parse.quote(a.name)); print(f'{a.name} deleted')
    elif c == 'mv':
        r.call('POST', '/r/mv?from=' + urllib.parse.quote(a.src) + '&to=' + urllib.parse.quote(a.dst)); print(f'{a.src} is now {a.dst}')
    elif c in ('cmdline', 'config'):
        name = c + '.txt'
        if a.text is None:
            sys.stdout.write(r.call('GET', '/r/text?name=' + name, raw=True).decode('latin-1'))
        else:
            text = open(a.text[1:]).read() if a.text.startswith('@') else a.text
            if c == 'cmdline' and not text.endswith('\n'):
                text += '\n'
            j = r.call('PUT', '/r/text?name=' + name, body=text.encode('latin-1'))
            print(f'{name} written ({j["size"]} bytes), the one before kept as {j["backup"]}')
    elif c == 'opt':
        a.dry = '--dry' in a.edits
        a.edits = [e for e in a.edits if e != '--dry']
        edits = '\n'.join(a.edits)
        j = r.call('POST', '/r/options' + ('?dry=1' if a.dry or not a.edits else ''), body=edits.encode('latin-1')) if a.edits else r.call('GET', '/r/options')
        if not a.edits:
            for k, v in j['values'].items():
                if v is not None:
                    print(f'{k} = {v}')
        print(('would be: ' if a.dry else 'saved: ' if a.edits else 'line: ') + j['line'].rstrip())
    elif c == 'wifi':
        w = a.action or 'show'
        if w == 'show':
            j = r.call('GET', '/r/wifi'); s = j['settings']
            print(f'cmdline.txt: {s["ssid"] or "(no network)"}, password {s["psk"]}, country {s["country"] or "(default)"}')
            if j.get('radio'):
                print(f'radio: {j["radio"]["state"]} {j["radio"]["message"]}')
            for n in j['scan']:
                print(f'  {n["ssid"]:32} {n["rssi"]:4} dBm  ch {n["channel"]:3}  {n["security"]}')
            if j.get('note'):
                print('test: ' + j['note'])
        elif w == 'scan':
            r.call('POST', '/r/wifi/scan'); time.sleep(4); a.action = 'show'; recovery_main(a)
        else:
            body = urllib.parse.urlencode({'ssid': a.ssid or '', 'psk': a.psk or '', 'country': a.country or ''})
            r.call('POST', '/r/wifi/' + w, body=body.encode())
            if w == 'test':
                for _ in range(30):
                    time.sleep(1.5)
                    j = r.call('GET', '/r/wifi')
                    if j['test'] != 'running':
                        break
                print('test: ' + (j.get('note') or 'no answer'))
            else:
                print('saved to cmdline.txt (the line before in cmdline.bak)')
    elif c == 'log':
        sys.stdout.write(r.call('GET', '/r/log' + ('?now=1' if a.now else ''), raw=True).decode('latin-1'))
    elif c == 'rollback':
        print(r.call('POST', '/r/rollback' + ('?force=1' if a.force else '')).get('note'))
    elif c == 'reboot':
        j = r.call('POST', '/r/reboot?how=' + a.how); print(f'restarting: {a.how}' + (f' ({j["note"]})' if j.get('note') else ''))
    elif c == 'display':
        display_main(r, a)


def hz_text(x):
    return '?' if x is None else (f'{x:.2f}' if round(x * 100) % 100 else f'{x:.0f}')


def display_show(j):
    """GET /r/display as text: the monitor, what is in force, the suggestion, every mode judged"""
    m = j.get('monitor')
    if m:
        mm = m['size_mm']
        print(f'monitor: {m["name"] or "(no name)"}, {m["maker_name"] or m["maker"]} ({m["maker"]}), product {m["product"]}, serial {m["serial"] or "not given"},'
              f' made {m["year"]}{" week " + str(m["week"]) if m["week"] and m["week"] < 54 else ""}' + (' (the testbed\'s sample)' if m.get('sample') else ''))
        print(f'         {mm[0] / 10:.1f} x {mm[1] / 10:.1f} cm, {m["size_in"]["diagonal"]}" diagonal; {m.get("native", "?")} {m.get("shape", "")};'
              f' {m["connection"]}, sound {m["sound"]}; EDID {m["edid_version"]}, {m["blocks"]} blocks')
    else:
        print('monitor: no EDID (' + ('not readable' if j['edid_status'] == 'header' else 'no monitor, or the firmware gave none') + '): the standard modes')
    n, f, cf = j['now'], j['file'], j['config']
    print(f'in force: {n["w"]}x{n["h"]}' + (f' at {hz_text(n["hz"])} Hz' if n['hz'] else '') + f', turned {n["rotation"]}; '
          + ('the firmware\'s own choice (no hdmi_mode)' if 'firmware' in n['source'] else f'{n["source"]}: {n["text"]}'))
    if f['present']:
        x = f['next']
        print('next start: ' + (f'{x["w"]}x{x["h"]}' + (f' at {hz_text(x["hz"])} Hz' if x['hz'] else '') if x['w'] else x['text'])
              + f', turned {f["rotation"]}' + (', to be kept on the Mac' if f['confirm'] else '') + ' (aurora68-display.txt' + (', .bak there)' if f['bak'] else ')'))
    else:
        print('next start: no aurora68-display.txt yet')
    print('config.txt: ' + ('not on the card' if not cf['read'] else 'no "include aurora68-display.txt" line (display use ... --add-include adds it)' if not cf['include']
                            else 'an hdmi_ line after the include wins (--add-include moves it)' if cf['override'] else 'includes aurora68-display.txt at its end'))
    if j['cmdline'].get('mac_screen'):
        print(f'cmdline.txt: mac.screen={j["cmdline"]["mac_screen"]} (it follows a new mode unless --keep-screen)')
    s = j.get('suggested')
    if s:
        print(f'suggested: {s["use"]}: {s["reason"]}')
    print(f'modes ({"standard list" if j["standard"] else "the monitor\'s"}; the Pi 3 sends up to {j["pi_mhz"]:.0f} MHz):')
    print(f'  {"auto":18} the firmware\'s choice from the monitor' + (f' (its preferred {j["preferred"]["w"]}x{j["preferred"]["h"]})' if j.get('preferred') else ''))
    for x in j['modes']:
        v = x['verdict']
        what = {'usable': f'{hz_text(x["hz"])} Hz, {x["mhz"]:.2f} MHz', 'too_fast': f'{hz_text(x["hz"])} Hz, {x["mhz"]:.2f} MHz: too fast, nothing slower fits',
                'interlaced': 'interlaced: not for the Mac', 'mac_size': 'outside the Mac\'s sizes'}.get(v) or \
            f'{hz_text(x["hz"])} Hz needs {x["mhz"]:.2f} MHz: too fast; {hz_text(x["alt_hz"])} Hz reduced blanking, {x["alt_mhz"]:.2f} MHz'
        tags = [k for k in ('suggested', 'current', 'preferred', 'native', 'shape') if x.get(k)]
        tags = ', '.join({'current': 'in force', 'native': "the panel's own", 'shape': 'stretched'}.get(t, t) for t in tags)
        print(f'  {x.get("use", "-"):18} {x["w"]}x{x["h"]}: {what}' + (f' [{tags}]' if tags else ''))


def display_main(r, a):
    import urllib.parse as up
    act = a.action or 'show'
    if act == 'show':
        j = r.call('GET', '/r/display')
        print(json.dumps(j, indent=2)) if a.json else display_show(j)
        return
    if act == 'use':
        if not a.arg:
            raise RuntimeError('display use WxH[@Hz] or auto')
        body = {'mode': a.arg, 'confirm': '0' if a.no_confirm else '1'}
        if a.rotate is not None:
            body['rotation'] = str(a.rotate)
        if a.add_include:
            body['include'] = '1'
        if a.keep_screen:
            body['screen'] = 'keep'
    elif act == 'rotate':
        body = {'rotation': a.arg or '0'}
    else:
        body = {'include': '1'}
    try:
        j = r.call('POST', '/r/display', body=up.urlencode(body).encode())
    except RuntimeError as e:
        if ': 409 ' in str(e):
            raise RuntimeError(str(e).replace('include=1', '--add-include')) from None
        raise
    if act == 'include':
        print(f'config.txt: the include line {j["include"]}' + (' (config.bak keeps the one before)' if j['include'] in ('added', 'moved') else ''))
        return
    print(f'aurora68-display.txt written and read back ({j.get("mode", "rotation")}, turned {j["rotation"]}); the one before is aurora68-display.bak')
    if j.get('include') in ('added', 'moved'):
        print(f'config.txt: the include line {j["include"]} (config.bak keeps the one before)')
    if j.get('mac_screen'):
        print(j['mac_screen'] + ' (cmdline.bak keeps the line before)')
    print(j['note'] + ' (aurora68.py recovery reboot normal)')


def add_recovery(sub):
    p = sub.add_parser('recovery', help='Aurora68 Setup and Recovery: the Pi\'s SD card when the Mac will not start (docs/08 item 106)')
    p.add_argument('--url', help='default $AURORA68_RECOVERY_URL, else http://aurora68.local')
    p.add_argument('--pin', help='the four digits on the Mac\'s screen (default $AURORA68_PIN)')
    rs = p.add_subparsers(dest='rcmd', required=True, metavar='ACTION')
    rs.add_parser('state', help='the recovery start: why, network, card, upload')
    rs.add_parser('ls', help='the files in the card\'s root')
    q = rs.add_parser('put', help='a file onto the card (kernel8.img is checked first)'); q.add_argument('file'); q.add_argument('name', nargs='?')
    q = rs.add_parser('get', help='a file from the card'); q.add_argument('name'); q.add_argument('out', nargs='?')
    q = rs.add_parser('rm', help='delete a file (never kernel8.img)'); q.add_argument('name')
    q = rs.add_parser('mv', help='rename a file'); q.add_argument('src'); q.add_argument('dst')
    q = rs.add_parser('cmdline', help='cmdline.txt: shown, or written (TEXT, or @FILE); one line; .bak kept'); q.add_argument('text', nargs='?')
    q = rs.add_parser('config', help='config.txt: shown, or written (TEXT, or @FILE); .bak kept'); q.add_argument('text', nargs='?')
    rs.add_parser('opt', help='options: shown, or [--dry] KEY=VALUE / -KEY / +WORD edits (every other word kept)')
    q = rs.add_parser('wifi', help='the WiFi: show, scan, test or save (--ssid --psk --country)'); q.add_argument('action', nargs='?', choices=('show', 'scan', 'test', 'save'))
    q.add_argument('--ssid'); q.add_argument('--psk'); q.add_argument('--country')
    q = rs.add_parser('log', help='the last start\'s aurora68.log (--now: this recovery start\'s)'); q.add_argument('--now', action='store_true')
    q = rs.add_parser('rollback', help='kernel8.old back as kernel8.img'); q.add_argument('--force', action='store_true')
    q = rs.add_parser('reboot', help='restart the Pi: normal or recovery'); q.add_argument('how', nargs='?', default='normal', choices=('normal', 'recovery'))
    q = rs.add_parser('display', help='the HDMI monitor: show, use WxH[@Hz]|auto, rotate N, include (docs/08 item 111)')
    q.add_argument('action', nargs='?', choices=('show', 'use', 'rotate', 'include')); q.add_argument('arg', nargs='?', help='WxH[@Hz] or auto; for rotate 0/90/180/270')
    q.add_argument('--rotate', type=int, choices=(0, 90, 180, 270), help='turn the picture (default: as the file has it)')
    q.add_argument('--no-confirm', action='store_true', help='no "Keep this resolution?" on the Mac at the next start')
    q.add_argument('--add-include', action='store_true', help='put "include aurora68-display.txt" at the end of config.txt if it is not there')
    q.add_argument('--keep-screen', action='store_true', help='leave cmdline.txt\'s mac.screen= as it is')
    q.add_argument('--json', action='store_true', help='show: the answer as it came')


def main():
    ap = argparse.ArgumentParser(description='The Pi\'s remote control of the Mac (docs/13).')
    ap.add_argument('--url'); ap.add_argument('--token'); ap.add_argument('--semihost', metavar='BASE')
    ap.add_argument('-q', '--quiet', action='store_true', help='no progress display for push, pull and recovery put/get')
    ap.add_argument('--display', type=int, help='the screen: 0 the first (the SE\'s on the SE board), 1 the second (HDMI); '
                    'for input, coordinates are then that screen\'s rather than the desktop\'s')
    sub = ap.add_subparsers(dest='cmd', required=True, metavar='COMMAND')
    sub.add_parser('state', help='the JSON status: the Mac, its screens, network, agent')
    p = sub.add_parser('screenshot', help='a screen, a window or a rectangle as PNG'); p.add_argument('out'); p.add_argument('--scale', type=int, default=1); p.add_argument('--hdmi', action='store_true')
    p.add_argument('--window', help='a window: text of its title, or its id (0x...) from `windows`')
    p.add_argument('--rect', help='a rectangle of the desktop: left,top,right,bottom in global coordinates')
    p.add_argument('--content', action='store_true', help='the window\'s content only, without the frame and title bar')
    p.add_argument('--front', action='store_true', help='bring the window to the front first (a covered window shows what covers it)')
    p.add_argument('--restore', action='store_true', help='with --front: put back the program that was in front afterwards')
    sub.add_parser('windows', help='the Mac\'s windows, front to back')
    p = sub.add_parser('click', help='a click at X Y'); p.add_argument('x', type=int); p.add_argument('y', type=int)
    p.add_argument('--double', action='store_true'); p.add_argument('--mods', default=''); p.add_argument('--button', default='left')
    p = sub.add_parser('move', help='the pointer to X Y'); p.add_argument('x', type=int); p.add_argument('y', type=int)
    p = sub.add_parser('drag', help='press at X Y, release at TO_X TO_Y'); [p.add_argument(a, type=int) for a in ('x', 'y', 'to_x', 'to_y')]; p.add_argument('--mods', default='')
    p = sub.add_parser('type', help='type text on the Mac\'s keyboard'); p.add_argument('text')
    p = sub.add_parser('key', help='one key, with --mods'); p.add_argument('key'); p.add_argument('--mods', default='')
    p = sub.add_parser('log', help='the kernel\'s log'); p.add_argument('--since', type=int); p.add_argument('--tags'); p.add_argument('--follow', action='store_true'); p.add_argument('--records', action='store_true')
    p = sub.add_parser('restart', help='restart the Mac (or nmi, pi; recovery: the Pi restarts into Aurora68 Setup and Recovery)'); p.add_argument('how', nargs='?', default='mac', choices=('mac', 'nmi', 'pi', 'recovery'))
    p = sub.add_parser('input', help='raw input events as JSON'); p.add_argument('json')
    p = sub.add_parser('ls', help='a folder on the Mac (no PATH: its disks)'); p.add_argument('path', nargs='?', default=''); p.add_argument('--json', action='store_true')
    p = sub.add_parser('info', help='one file or folder on the Mac'); p.add_argument('path'); p.add_argument('--json', action='store_true')
    p = sub.add_parser('pull', help='a file from the Mac (--macbinary: forks and type)'); p.add_argument('path'); p.add_argument('local', nargs='?')
    p.add_argument('-q', '--quiet', action='store_true', default=argparse.SUPPRESS, help='no progress display')
    p.add_argument('--macbinary', action='store_true'); p.add_argument('--rsrc', action='store_true', help='the resource fork instead of the data fork')
    p.add_argument('--text', action='store_true', help='Mac text to UTF-8 with newlines')
    p = sub.add_parser('push', help='a file to the Mac (MacBinary keeps its forks and type)'); p.add_argument('local'); p.add_argument('path')
    p.add_argument('-q', '--quiet', action='store_true', default=argparse.SUPPRESS, help='no progress display')
    p.add_argument('--force', action='store_true', help='replace a file that is there'); p.add_argument('--mkdir', action='store_true', help='make missing folders')
    p.add_argument('--type'); p.add_argument('--creator'); p.add_argument('--raw', action='store_true', help='send a MacBinary file as it is, as data')
    p.add_argument('--macbinary', action='store_true', help='the file is MacBinary even if it does not look it')
    p.add_argument('--text', action='store_true', help='UTF-8 text to Mac Roman with carriage returns (TEXT/ttxt)')
    p = sub.add_parser('mkdir', help='make a folder on the Mac'); p.add_argument('path'); p.add_argument('-p', '--parents', action='store_true')
    p = sub.add_parser('rm', help='delete a file or an empty folder on the Mac'); p.add_argument('path')
    p = sub.add_parser('mv', help='rename or move on one Mac disk'); p.add_argument('path'); p.add_argument('to')
    p = sub.add_parser('open', help='launch an application or open a document on the Mac'); p.add_argument('path')
    p = sub.add_parser('apps', help='the programs that run on the Mac'); p.add_argument('--json', action='store_true')
    p = sub.add_parser('quit', help='ask a program on the Mac to quit (System 7)'); p.add_argument('name', nargs='?'); p.add_argument('--creator')
    add_recovery(sub)
    argv, edits = sys.argv[1:], []
    if 'recovery' in argv and 'opt' in argv[argv.index('recovery'):]:   # the edits may begin with '-' (-KEY): taken before argparse
        i = argv.index('opt', argv.index('recovery'))
        argv, edits = argv[:i + 1], argv[i + 1:]
    a = ap.parse_args(argv)
    a.edits = edits
    if a.cmd == 'recovery':
        return recovery_main(a)
    r = Remote(a.url, a.token, a.semihost)
    on = {} if a.display is None else {'display': a.display}       # a point of that screen, not of the desktop
    if a.cmd == 'state':
        print(json.dumps(r.state(), indent=2))
    elif a.cmd == 'windows':
        ws = r.windows()
        print(f'{"id":10}  {"#":>2}  {"kind":14} {"size":>11}  {"screens":7}  {"application":16} title')
        for w in ws:
            q = w.get('rect') or [0, 0, 0, 0]
            size = f'{q[2] - q[0]}x{q[3] - q[1]}' + ('' if w.get('visible') else ' hidden')
            print(f'{w["id"]:10}  {w["front_index"]:2}  {w["kind"]:14} {size:>11}  {",".join(map(str, w.get("screens", []))):7}  '
                  f'{w.get("application", "")[:16]:16} {w["title"]}')
    elif a.cmd == 'screenshot' and (a.window or a.rect):
        t0 = time.time()
        rect = [int(v) for v in a.rect.split(',')] if a.rect else None
        data, h = r.window_screenshot(window=a.window, rect=rect, content=a.content, scale=a.scale, front=a.front, restore=a.restore)
        open(a.out, 'wb').write(data)
        w, hh = png_size(data)
        extra = ''.join(f', {k[9:]} {v}' for k, v in h.items() if k.startswith('x-aurora-'))
        print(f'{a.out}: {w}x{hh}, {len(data)} bytes in {time.time() - t0:.2f} s{extra}')
    elif a.cmd == 'screenshot':
        t0 = time.time(); data = r.screenshot(a.scale, a.hdmi, a.display or 0)
        open(a.out, 'wb').write(data)
        w, h = png_size(data)
        print(f'{a.out}: {w}x{h}, {len(data)} bytes in {time.time() - t0:.2f} s')
    elif a.cmd == 'click':
        print(r.input({'type': 'click', 'x': a.x, 'y': a.y, 'count': 2 if a.double else 1, 'mods': a.mods, 'button': a.button, **on}))
    elif a.cmd == 'move':
        print(r.input({'type': 'move', 'x': a.x, 'y': a.y, **on}))
    elif a.cmd == 'drag':
        print(r.input({'type': 'drag', 'x': a.x, 'y': a.y, 'to_x': a.to_x, 'to_y': a.to_y, 'mods': a.mods, **on}))
    elif a.cmd == 'type':
        print(r.input({'type': 'text', 'text': a.text}))
    elif a.cmd == 'key':
        print(r.input({'type': 'key', 'key': a.key, 'mods': a.mods}))
    elif a.cmd == 'input':
        print(r.input(json.loads(a.json)))
    elif a.cmd == 'restart':
        print(r.restart(a.how))
    elif a.cmd == 'ls':
        res = r.ls(a.path)
        print(json.dumps(res, indent=2, ensure_ascii=False)) if a.json else ls_table(res)
    elif a.cmd == 'info':
        e = r.info(a.path)
        if a.json:
            print(json.dumps(e, indent=2, ensure_ascii=False))
        else:
            for k, v in e.items():
                print(f'{k:10} {v}')
    elif a.cmd == 'pull':
        t0 = time.time()
        local = a.local
        if not local or os.path.isdir(local):
            name = a.path.rstrip(':').split(':')[-1].replace('/', '-') + ('.bin' if a.macbinary else '')
            local = os.path.join(local or '.', name)
        tmp = local + '.part'
        pr = Progress('pull ' + a.path.rstrip(':').split(':')[-1], quiet=a.quiet)
        try:
            with open(tmp, 'wb') as f:
                h = r.pull(a.path, f, a.macbinary, a.rsrc, progress=pr)
        finally:
            pr.finish()
        got, want = os.path.getsize(tmp), int(h.get('content-length', -1))
        if want >= 0 and got != want:
            last = r.state().get('agent', {}).get('last', '')
            raise RuntimeError(f'the transfer was cut short at {got} of {want} bytes ({tmp} kept): {last}')
        if a.text:
            with open(tmp, 'rb') as f:
                t = f.read().decode('mac_roman').replace('\r\n', '\n').replace('\r', '\n')
            with open(tmp, 'w', encoding='utf-8') as f:
                f.write(t)
        os.replace(tmp, local)
        dt = time.time() - t0
        tc = urllib.parse.unquote(h.get('x-aurora-type', '')) + '/' + urllib.parse.unquote(h.get('x-aurora-creator', ''))
        print(f'{local}: {got} bytes ({h.get("x-aurora-format")}, {tc}, data {h.get("x-aurora-data-length")}, '
              f'resources {h.get("x-aurora-rsrc-length")}) in {dt:.2f} s, {human(got / max(dt, 1e-3))}/s')
    elif a.cmd == 'push':
        t0 = time.time()
        size = os.path.getsize(a.local)
        src = a.local
        if a.text:                                  # the Mac's text: Mac Roman, carriage returns
            with open(a.local, encoding='utf-8') as f:
                body = f.read().replace('\r\n', '\n').replace('\n', '\r').encode('mac_roman', 'replace')
            src = None
            import io
            fobj = io.BytesIO(body); size = len(body); a.raw = True
            a.type, a.creator = a.type or 'TEXT', a.creator or 'ttxt'
        with (open(src, 'rb') if src else fobj) as f:
            mb = None if a.raw else macbinary_info(f.read(128), size)
            if a.macbinary and not mb:
                raise RuntimeError(f'{a.local} is not a MacBinary file')
            path = a.path
            target = path + (mb['name'] if mb else mac_name(a.local)) if path.endswith(':') else path
            try:                                    # a folder of that name is there: say so before sending
                there = r.info(target)
            except RuntimeError:
                there = {}
            if there.get('kind') == 'folder':
                raise RuntimeError(f'"{target}" is a folder, so the file cannot replace it: to put it inside, '
                                   f'push to "{target}:"')
            if mb:
                kind = f'MacBinary: {mb["type"]}/{mb["creator"]}, data {mb["data"]}, resources {mb["rsrc"]}'
                pr = Progress('push ' + target.split(':')[-1], size, quiet=a.quiet)
                try:
                    res = r.push(f, size, path, macbinary=True, force=a.force, mkdir=a.mkdir, progress=pr)
                finally:
                    pr.finish()
            else:
                t, c = TYPES.get(os.path.splitext(a.local)[1].lower(), ('????', '????'))
                t, c = a.type or t, a.creator or c
                if path.endswith(':'):
                    path += mac_name(a.local)
                kind = f'data fork, {t}/{c}'
                pr = Progress('push ' + path.split(':')[-1], size, quiet=a.quiet)
                try:
                    res = r.push(f, size, path, ftype=t, creator=c, force=a.force, mkdir=a.mkdir, progress=pr)
                finally:
                    pr.finish()
        dt = time.time() - t0
        print(f'{res["path"]}: {size} bytes ({kind}) in {dt:.2f} s, {human(size / max(dt, 1e-3))}/s')
    elif a.cmd == 'mkdir':
        print(r.mkdir(a.path, a.parents))
    elif a.cmd == 'rm':
        print(r.rm(a.path))
    elif a.cmd == 'mv':
        print(r.mv(a.path, a.to))
    elif a.cmd == 'open':
        print(r.open(a.path))
    elif a.cmd == 'apps':
        res = r.apps()
        if a.json:
            print(json.dumps(res, indent=2, ensure_ascii=False))
        else:
            for p in res['apps']:
                extra = ', '.join(x for x, on in (('front', p.get('front')), ('background only', p.get('background_only'))) if on)
                if 'type' not in p:                     # System 6: the one in front, nothing more known
                    print(f'{p["name"]}  (front; {p.get("note", "")})'); continue
                print(f'{p.get("type", ""):4}/{p.get("creator", ""):4} {human(p.get("size", 0)):>9}  {p["name"]}' + (f'  ({extra})' if extra else ''))
    elif a.cmd == 'quit':
        if not a.name and not a.creator:
            raise RuntimeError('quit NAME, or quit --creator CODE')
        print(r.quit(a.name, a.creator))
    elif a.cmd == 'log' and a.records:
        sys.stdout.write(r.records())
    elif a.cmd == 'log':
        since = a.since
        while True:
            text, since, lost = r.log(since, a.tags)
            if lost:
                print(f'[aurora68.py: {lost} bytes of the log were overwritten before they were read]', file=sys.stderr)
            sys.stdout.write(text); sys.stdout.flush()
            if not a.follow:
                print(f'[next: --since {since}]', file=sys.stderr)
                break
            time.sleep(1)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, TimeoutError) as e:
        sys.exit(f'aurora68.py: {e}')
