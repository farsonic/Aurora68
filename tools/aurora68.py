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
  aurora68.py mkdir PATH [-p] | rm PATH | mv PATH NEWPATH
                                                 rm takes a file or an empty folder, never more; mv stays on one disk
  aurora68.py open PATH                          launch an application, or open a document with its application
  aurora68.py apps [--json]                      the programs that run (System 6: the one in front)
  aurora68.py quit NAME | quit --creator ttxt    ask a program to quit (System 7)

Where: --url (default $AURORA68_URL, else http://aurora68-pi.local), --token (default
$AURORA68_TOKEN). In the QEMU testbed, --semihost BASE talks to the kernel through the files of
mac.remotefile=BASE instead of the network.
"""
import argparse, json, os, re, socket, struct, sys, time, urllib.error, urllib.parse, urllib.request


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

    def stream(self, method, path, upload=None, size=0, sink=None):
        """(status, headers, bytes or None) of a request whose body is the open file upload (size bytes) and
        whose answer, if sink is given, is written to it as it comes: a file of any size in either direction"""
        if self.semihost:
            status, h, data = self._semihost(method, path, upload.read() if upload else None)
            if sink and status == 200:
                sink.write(data); return status, h, None
            return status, h, data
        self._probe()
        req = urllib.request.Request(self.url + path, data=upload, method=method)
        if self.token:
            req.add_header('X-Aurora-Token', self.token)
        if upload is not None:
            req.add_header('Content-Type', 'application/octet-stream')
            req.add_header('Content-Length', str(size))
        try:
            with urllib.request.urlopen(req, timeout=max(self.timeout, 60)) as r:
                if sink is None or r.status != 200:
                    return r.status, dict(r.headers), r.read()
                while True:
                    b = r.read(65536)
                    if not b:
                        break
                    sink.write(b)
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
    def _files(self, method, path, upload=None, size=0, sink=None):
        """a /files or /apps request; one file job runs at a time, so a busy answer is tried again"""
        for attempt in range(40):
            if upload is not None:
                upload.seek(0)
            status, h, data = self.stream(method, path, upload, size, sink)
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

    def pull(self, path, sink, macbinary=False, rsrc=False):
        """the file into sink; its headers (x-aurora-type ...)"""
        h, _ = self._files('GET', '/files/data?path=' + q(path) + ('&macbinary=1' if macbinary else '') + ('&fork=rsrc' if rsrc else ''), sink=sink)
        return h

    def push(self, f, size, path, macbinary=False, ftype=None, creator=None, force=False, mkdir=False):
        args = '/files/data?path=' + q(path) + ('&macbinary=1' if macbinary else '')
        if ftype: args += '&type=' + q(ftype)
        if creator: args += '&creator=' + q(creator)
        if force: args += '&force=1'
        if mkdir: args += '&mkdir=1'
        return json.loads(self._files('PUT', args, upload=f, size=size)[1].decode('utf-8'))

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
         '.aiff': ('AIFF', 'PIPY'), '.wav': ('WAVE', 'PIPY')}


def mac_name(local):
    """a local file's name as a Mac name: 31 characters, no colons"""
    n = os.path.basename(local).replace(':', '-')
    return n[:31]


def human(n):
    for unit in ('bytes', 'KB', 'MB', 'GB'):
        if n < 1024 or unit == 'GB':
            return f'{int(n)} {unit}' if unit == 'bytes' else f'{n:.1f} {unit}'
        n /= 1024


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


def main():
    ap = argparse.ArgumentParser(description='The Pi\'s remote control of the Mac (docs/13).')
    ap.add_argument('--url'); ap.add_argument('--token'); ap.add_argument('--semihost', metavar='BASE')
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
    p = sub.add_parser('restart', help='restart the Mac (or nmi, pi)'); p.add_argument('how', nargs='?', default='mac', choices=('mac', 'nmi', 'pi'))
    p = sub.add_parser('input', help='raw input events as JSON'); p.add_argument('json')
    p = sub.add_parser('ls', help='a folder on the Mac (no PATH: its disks)'); p.add_argument('path', nargs='?', default=''); p.add_argument('--json', action='store_true')
    p = sub.add_parser('info', help='one file or folder on the Mac'); p.add_argument('path'); p.add_argument('--json', action='store_true')
    p = sub.add_parser('pull', help='a file from the Mac (--macbinary: forks and type)'); p.add_argument('path'); p.add_argument('local', nargs='?')
    p.add_argument('--macbinary', action='store_true'); p.add_argument('--rsrc', action='store_true', help='the resource fork instead of the data fork')
    p.add_argument('--text', action='store_true', help='Mac text to UTF-8 with newlines')
    p = sub.add_parser('push', help='a file to the Mac (MacBinary keeps its forks and type)'); p.add_argument('local'); p.add_argument('path')
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
    a = ap.parse_args()
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
        with open(tmp, 'wb') as f:
            h = r.pull(a.path, f, a.macbinary, a.rsrc)
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
                res = r.push(f, size, path, macbinary=True, force=a.force, mkdir=a.mkdir)
            else:
                t, c = TYPES.get(os.path.splitext(a.local)[1].lower(), ('????', '????'))
                t, c = a.type or t, a.creator or c
                if path.endswith(':'):
                    path += mac_name(a.local)
                kind = f'data fork, {t}/{c}'
                res = r.push(f, size, path, ftype=t, creator=c, force=a.force, mkdir=a.mkdir)
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
