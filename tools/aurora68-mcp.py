#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""aurora68-mcp.py - an MCP server (stdio) that lets an agent see and drive the Mac through the Pi.

The Aurora68 kernel serves the Mac's screen, mouse, keyboard, log and reset over HTTP when the
Pi's cmdline.txt has `remote=on remote.token=...` (docs/13-remote-control.md). This script speaks
the Model Context Protocol on stdin and stdout (JSON-RPC 2.0, one message per line) and turns its
tool calls into those HTTP requests, so Claude Code or Claude Desktop can take a screenshot of the
Mac, click, type, read the kernel's log and restart the Mac. Python 3, standard library only; it is
one file so it can be copied anywhere.

Configuration, by environment or arguments:
  AURORA68_URL    or --url     the Pi, default http://aurora68-pi.local (mDNS)
  AURORA68_TOKEN  or --token   the token from remote.token= in cmdline.txt

Claude Code:     claude mcp add aurora68 --env AURORA68_URL=http://aurora68-pi.local \\
                     --env AURORA68_TOKEN=YOUR-TOKEN -- python3 /path/to/aurora68-mcp.py
Claude Desktop:  claude_desktop_config.json, "mcpServers": {"aurora68": {"command": "python3",
                     "args": ["/path/to/aurora68-mcp.py"], "env": {"AURORA68_URL": "...", "AURORA68_TOKEN": "..."}}}
"""
import argparse, base64, json, os, re, struct, sys, time, urllib.error, urllib.parse, urllib.request

VERSION = '1.0'
PROTOCOLS = ('2025-06-18', '2025-03-26', '2024-11-05')


class Pi:
    def __init__(self, url, token, timeout=30):
        self.url, self.token, self.timeout = url.rstrip('/'), token, timeout
        self.last_headers = {}

    def call(self, method, path, body=None):
        req = urllib.request.Request(self.url + path, data=None if body is None else json.dumps(body).encode(), method=method)
        req.add_header('X-Aurora-Token', self.token)
        if body is not None:
            req.add_header('Content-Type', 'application/json')
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                return r.status, {k.lower(): v for k, v in r.headers.items()}, r.read()
        except urllib.error.HTTPError as e:
            return e.code, {k.lower(): v for k, v in e.headers.items()}, e.read()

    def json(self, method, path, body=None):
        status, _, data = self.call(method, path, body)
        try:
            obj = json.loads(data.decode('latin-1') or '{}')
        except ValueError:
            obj = {'text': data.decode('latin-1')}
        if status != 200:
            raise RuntimeError(f'HTTP {status}: {obj.get("error", obj)}')
        return obj

    def screenshot(self, scale, window=None, content=False):
        if window is None:
            q = f'/screen.png?scale={scale}'
        else:
            w = str(window)
            q = '/screenshot?' + ('window=' + w if re.fullmatch(r'0x[0-9a-fA-F]+', w) else 'title=' + urllib.parse.quote(w, safe=''))
            q += f'&scale={scale}' + ('&content=1' if content else '')
        for _ in range(20):
            status, h, data = self.call('GET', q)
            if status == 200:
                self.last_headers = h
                return data
            if status != 503 or b'another' not in data:
                try:
                    msg = json.loads(data.decode('utf-8')).get('error', '')
                except ValueError:
                    msg = data.decode('utf-8', 'replace').strip()
                raise RuntimeError(f'HTTP {status}: {msg}')
            time.sleep(0.25)
        raise RuntimeError('the screen stayed busy')


MODS = {'type': 'string', 'description': 'modifier keys held, comma-separated: cmd, shift, option, control (e.g. "cmd,shift")'}
XY = {'x': {'type': 'integer', 'description': 'pixels from the left of the main screen'},
      'y': {'type': 'integer', 'description': 'pixels from the top of the main screen'}}
TOOLS = [
    {'name': 'screenshot', 'description': 'A picture of the Mac\'s main screen as it is now (PNG). Coordinates for click and move are '
     'pixels of this picture at scale 1 (0,0 is the top left; the menu bar is the top 20 pixels). With "window", a picture of '
     'that one window instead (its frame and title bar, or its content only), taken from whichever screens it is on.',
     'inputSchema': {'type': 'object', 'properties': {
         'scale': {'type': 'integer', 'enum': [1, 2, 4], 'default': 1,
                   'description': 'whole screen: 2 or 4 for a smaller picture (coordinates are then multiplied back by it); '
                                  'with window: 2 or 4 enlarges each pixel (good for the one-bit SE screen)'},
         'window': {'type': 'string', 'description': 'a window\'s id from list_windows (0x...), or text of its title '
                    '(the frontmost visible window whose title contains it, case ignored)'},
         'content': {'type': 'boolean', 'default': False, 'description': 'with window: the content only, without frame and title bar'}}}},
    {'name': 'list_windows', 'description': 'The Mac\'s windows, front to back, read from the Window Manager by the Pi: id, title, '
     'kind (document, dialog, desk accessory, ...), visible, the rectangle with frame and title bar and the content rectangle '
     '(global coordinates, left, top, right, bottom), the screens it is on and the application it belongs to. Pass an id or '
     'part of a title to screenshot as "window" for a picture of one.',
     'inputSchema': {'type': 'object', 'properties': {}}},
    {'name': 'click', 'description': 'Click the mouse at a point of the screen. count 2 is a double-click (opens icons in the Finder). '
     'button "right" is a Command-click.',
     'inputSchema': {'type': 'object', 'properties': dict(XY, count={'type': 'integer', 'enum': [1, 2, 3], 'default': 1},
                     button={'type': 'string', 'enum': ['left', 'right'], 'default': 'left'}, mods=MODS), 'required': ['x', 'y']}},
    {'name': 'move', 'description': 'Move the mouse pointer to a point without clicking.',
     'inputSchema': {'type': 'object', 'properties': XY, 'required': ['x', 'y']}},
    {'name': 'drag', 'description': 'Press the button at (x, y), move to (to_x, to_y) and release: drag an icon, a window, a selection, or choose from a menu.',
     'inputSchema': {'type': 'object', 'properties': dict(XY, to_x={'type': 'integer'}, to_y={'type': 'integer'}, mods=MODS),
                     'required': ['x', 'y', 'to_x', 'to_y']}},
    {'name': 'type', 'description': 'Type text on the Mac\'s keyboard (US layout keys; \\n is Return).',
     'inputSchema': {'type': 'object', 'properties': {'text': {'type': 'string'}}, 'required': ['text']}},
    {'name': 'key', 'description': 'Press one key, optionally with modifiers: a character ("q" with mods "cmd" is Quit) or a name: '
     'return, enter, tab, space, delete, escape, left, right, up, down, home, end, pageup, pagedown, forwarddelete, help, f1..f12.',
     'inputSchema': {'type': 'object', 'properties': {'key': {'type': 'string'}, 'mods': MODS}, 'required': ['key']}},
    {'name': 'log', 'description': 'The Pi kernel\'s log ([MAC] lines are the Mac\'s drivers and programs, [CRASH] the crash records, '
     '[BOOT] the boot timeline). Returns the text and the cursor to pass as "since" next time for only what is new.',
     'inputSchema': {'type': 'object', 'properties': {'since': {'type': 'integer', 'description': 'the cursor from the last call'},
                     'tags': {'type': 'string', 'description': 'only lines of these parts, e.g. "CRASH,BOOT" or "MAC"'},
                     'records': {'type': 'boolean', 'description': 'the [BOOT] and [CRASH] lines kept since the Pi started, even those the 64 KB log has lost'},
                     'max_chars': {'type': 'integer', 'default': 20000}}}},
    {'name': 'state', 'description': 'A small JSON status: the board, the screen size and depth, the application in front, whether the '
     'Mac runs or is stopped at a system error (and which), the network, the disks.',
     'inputSchema': {'type': 'object', 'properties': {}}},
    {'name': 'restart', 'description': 'Restart the Mac (how "mac", works even when it is hung), press the programmer\'s interrupt switch '
     '("nmi": the debugger prompt), or reboot the whole Pi ("pi"). Ask the user before using it.',
     'inputSchema': {'type': 'object', 'properties': {'how': {'type': 'string', 'enum': ['mac', 'nmi', 'pi'], 'default': 'mac'}}}},
]


def text(s):
    return {'content': [{'type': 'text', 'text': s}]}


def run_tool(pi, name, a):
    if name == 'screenshot' and a.get('window'):
        scale = int(a.get('scale', 1) or 1)
        data = pi.screenshot(scale, a['window'], bool(a.get('content')))
        w, h = struct.unpack('>II', data[16:24])
        hd = pi.last_headers
        note = f'{w}x{h} pixels: window {hd.get("x-aurora-window", "?")}, the desktop\'s rectangle {hd.get("x-aurora-rect", "?")}'
        note += f' (left, top, right, bottom), each pixel {hd.get("x-aurora-scale", scale)} times'
        if hd.get('x-aurora-clipped-from'):
            note += f'; the window reaches beyond the screens ({hd["x-aurora-clipped-from"]}): only what is on them is shown'
        if hd.get('x-aurora-covered-by'):
            note += f'; windows in front cover part of it ({hd["x-aurora-covered-by"]}): the picture shows them, as the screen does'
        return {'content': [{'type': 'image', 'data': base64.b64encode(data).decode(), 'mimeType': 'image/png'},
                            {'type': 'text', 'text': note}]}
    if name == 'screenshot':
        scale = int(a.get('scale', 1) or 1)
        data = pi.screenshot(scale)
        w, h = struct.unpack('>II', data[16:24])
        return {'content': [{'type': 'image', 'data': base64.b64encode(data).decode(), 'mimeType': 'image/png'},
                            {'type': 'text', 'text': f'{w}x{h} pixels at scale {scale} ({len(data)} bytes)' +
                             (f'; multiply coordinates by {scale} for click' if scale > 1 else '')}]}
    if name == 'list_windows':
        status, _, data = pi.call('GET', '/windows')
        if status != 200:
            raise RuntimeError(f'HTTP {status}: {data.decode("utf-8", "replace").strip()}')
        ws = json.loads(data.decode('utf-8'))
        lines = [f'{len(ws)} windows, front to back:']
        for w in ws:
            q = w.get('rect') or [0, 0, 0, 0]
            lines.append(f'{w["id"]} "{w["title"]}" {w["kind"]}{"" if w["visible"] else " (hidden)"}, '
                         f'{q[2] - q[0]}x{q[3] - q[1]} at {q[0]},{q[1]}, screens {w.get("screens")}' +
                         (f', {w["application"]}' if w.get('application') else ''))
        return text('\n'.join(lines) + '\n\n' + json.dumps(ws))
    if name in ('click', 'move', 'drag'):
        ev = {'type': name, 'x': int(a['x']), 'y': int(a['y'])}
        if name == 'click':
            ev.update(count=int(a.get('count', 1) or 1), button=a.get('button', 'left'), mods=a.get('mods', ''))
        if name == 'drag':
            ev.update(to_x=int(a['to_x']), to_y=int(a['to_y']), mods=a.get('mods', ''))
        return text(json.dumps(pi.json('POST', '/input?wait=1', ev)))
    if name == 'type':
        return text(json.dumps(pi.json('POST', '/input?wait=1', {'type': 'text', 'text': a['text']})))
    if name == 'key':
        return text(json.dumps(pi.json('POST', '/input?wait=1', {'type': 'key', 'key': a['key'], 'mods': a.get('mods', '')})))
    if name == 'state':
        return text(json.dumps(pi.json('GET', '/state'), indent=1))
    if name == 'restart':
        return text(json.dumps(pi.json('POST', '/restart?how=' + a.get('how', 'mac'))))
    if name == 'log' and a.get('records'):
        status, h, data = pi.call('GET', '/log?records=1')
        if status != 200:
            raise RuntimeError(f'HTTP {status}: {data.decode("latin-1").strip()}')
        return text(data.decode('latin-1') or '(no records)')
    if name == 'log':
        q = []
        if a.get('since') is not None: q.append(f'since={int(a["since"])}')
        if a.get('tags'): q.append('tags=' + a['tags'])
        status, h, data = pi.call('GET', '/log' + ('?' + '&'.join(q) if q else ''))
        if status != 200:
            raise RuntimeError(f'HTTP {status}: {data.decode("latin-1").strip()}')
        t = data.decode('latin-1')
        cap = int(a.get('max_chars', 20000) or 20000)
        if len(t) > cap:
            t = f'[{len(t) - cap} earlier characters left out]\n' + t[-cap:]
        lost = int(h.get('x-log-lost', 0))
        return text(t + f'\n[next since: {h.get("x-log-next", "?")}' + (f'; {lost} bytes were overwritten before this read' if lost else '') + ']')
    raise KeyError(name)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--url', default=os.environ.get('AURORA68_URL', 'http://aurora68-pi.local'))
    ap.add_argument('--token', default=os.environ.get('AURORA68_TOKEN', ''))
    a = ap.parse_args()
    pi = Pi(a.url, a.token)
    out = sys.stdout
    def reply(mid, result=None, error=None):
        msg = {'jsonrpc': '2.0', 'id': mid}
        if error is not None: msg['error'] = error
        else: msg['result'] = result
        out.write(json.dumps(msg) + '\n'); out.flush()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            reply(None, error={'code': -32700, 'message': 'parse error'}); continue
        mid, method, params = msg.get('id'), msg.get('method'), msg.get('params') or {}
        if mid is None:
            continue                                               # a notification (initialized, cancelled): nothing to answer
        if method == 'initialize':
            want = params.get('protocolVersion')
            reply(mid, {'protocolVersion': want if want in PROTOCOLS else PROTOCOLS[0],
                        'capabilities': {'tools': {}},
                        'serverInfo': {'name': 'aurora68', 'version': VERSION},
                        'instructions': 'Tools to see and drive a classic Macintosh (System 7) through the Raspberry Pi that is its CPU. '
                                        'Take a screenshot first; coordinates are its pixels. Use restart only when the user agrees.'})
        elif method == 'ping':
            reply(mid, {})
        elif method == 'tools/list':
            reply(mid, {'tools': TOOLS})
        elif method == 'tools/call':
            name, args = params.get('name'), params.get('arguments') or {}
            try:
                reply(mid, run_tool(pi, name, args))
            except KeyError as e:
                reply(mid, error={'code': -32602, 'message': f'unknown tool or argument: {e}'})
            except (RuntimeError, OSError, ValueError) as e:
                reply(mid, {'content': [{'type': 'text', 'text': f'{name} failed: {e} (is the Pi at {a.url} with remote=on?)'}], 'isError': True})
        else:
            reply(mid, error={'code': -32601, 'message': f'no method {method}'})


if __name__ == '__main__':
    main()
