#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""aurora68.py - the command line for the Pi's remote control (docs/13-remote-control.md).

The kernel serves the Mac's screen, input, log and state over HTTP when cmdline.txt has
`remote=on remote.token=...`. This is a plain client for it: for a person at a terminal, for
scripts, and for checking the server without an MCP host. Python 3, standard library only.

  aurora68.py state                              the JSON status
  aurora68.py screenshot out.png [--scale 2]     the first screen as PNG (--display 1: the second, HDMI on the SE)
  aurora68.py click X Y [--double] [--mods cmd]  a click at a point of the desktop (pixels, 0,0 the main screen's
                                                 top left; --display N: a point of screen N instead)
  aurora68.py move X Y | drag X Y TO_X TO_Y
  aurora68.py type "some text"                   typed on the US keyboard layout
  aurora68.py key return [--mods cmd]            one key: return, escape, tab, left ..., or a character
  aurora68.py log [--since N] [--tags BOOT,CRASH] [--follow]
  aurora68.py log --records                      the [BOOT] and [CRASH] lines kept since the Pi started
  aurora68.py restart [mac|nmi|pi]
  aurora68.py input '[{"type": "click", "x": 20, "y": 10}]'     events as the server takes them

Where: --url (default $AURORA68_URL, else http://aurora68-pi.local), --token (default
$AURORA68_TOKEN). In the QEMU testbed, --semihost BASE talks to the kernel through the files of
mac.remotefile=BASE instead of the network.
"""
import argparse, json, os, struct, sys, time, urllib.error, urllib.request


class Remote:
    def __init__(self, url=None, token=None, semihost=None, timeout=30):
        self.url = (url or os.environ.get('AURORA68_URL') or 'http://aurora68-pi.local').rstrip('/')
        self.token = token if token is not None else os.environ.get('AURORA68_TOKEN', '')
        self.semihost = semihost
        self.timeout = timeout

    def request(self, method, path, body=None):
        """(status, headers, bytes) of one request"""
        if self.semihost:
            return self._semihost(method, path, body)
        req = urllib.request.Request(self.url + path, data=body, method=method)
        if self.token:
            req.add_header('X-Aurora-Token', self.token)
        if body is not None:
            req.add_header('Content-Type', 'application/json')
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.status, dict(r.headers), r.read()
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


def png_size(data):
    return struct.unpack('>II', data[16:24]) if data[:8] == b'\x89PNG\r\n\x1a\n' else (0, 0)


def main():
    ap = argparse.ArgumentParser(description='The Pi\'s remote control of the Mac (docs/13).')
    ap.add_argument('--url'); ap.add_argument('--token'); ap.add_argument('--semihost', metavar='BASE')
    ap.add_argument('--display', type=int, help='the screen: 0 the first (the SE\'s on the SE board), 1 the second (HDMI); '
                    'for input, coordinates are then that screen\'s rather than the desktop\'s')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('state')
    p = sub.add_parser('screenshot'); p.add_argument('out'); p.add_argument('--scale', type=int, default=1); p.add_argument('--hdmi', action='store_true')
    p = sub.add_parser('click'); p.add_argument('x', type=int); p.add_argument('y', type=int)
    p.add_argument('--double', action='store_true'); p.add_argument('--mods', default=''); p.add_argument('--button', default='left')
    p = sub.add_parser('move'); p.add_argument('x', type=int); p.add_argument('y', type=int)
    p = sub.add_parser('drag'); [p.add_argument(a, type=int) for a in ('x', 'y', 'to_x', 'to_y')]; p.add_argument('--mods', default='')
    p = sub.add_parser('type'); p.add_argument('text')
    p = sub.add_parser('key'); p.add_argument('key'); p.add_argument('--mods', default='')
    p = sub.add_parser('log'); p.add_argument('--since', type=int); p.add_argument('--tags'); p.add_argument('--follow', action='store_true'); p.add_argument('--records', action='store_true')
    p = sub.add_parser('restart'); p.add_argument('how', nargs='?', default='mac', choices=('mac', 'nmi', 'pi'))
    p = sub.add_parser('input'); p.add_argument('json')
    a = ap.parse_args()
    r = Remote(a.url, a.token, a.semihost)
    on = {} if a.display is None else {'display': a.display}       # a point of that screen, not of the desktop
    if a.cmd == 'state':
        print(json.dumps(r.state(), indent=2))
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
