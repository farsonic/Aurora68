#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""test_helper.py - the video helper as the Mac's program will use it (docs/16, issue #22).

Makes a short test movie with ffmpeg (a test pattern and a tone), starts the helper in file mode on a
free port, and does what Aurora68 Video does: finds it with the UDP probe, asks /info and /search, plays
the result and checks with ffprobe that the stream is an MPEG program stream with MPEG-1 video at the
size asked for and MP2 audio at 44.1 kHz stereo. Then: a play stopped early (the helper's ffmpeg must
go), refusals (a file not given, a path), and a helper with a token refusing a client without it.

    python3 tools/video-helper/test_helper.py
"""
import json, os, socket, subprocess, sys, tempfile, time, urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
fails = 0


def check(ok, what):
    global fails
    print(('ok   ' if ok else 'FAIL ') + what, flush=True)
    if not ok:
        fails += 1


def free_port():
    s = socket.socket(); s.bind(('127.0.0.1', 0)); p = s.getsockname()[1]; s.close()
    return p


def start(args, port):
    p = subprocess.Popen([sys.executable, os.path.join(HERE, 'aurora68_video_helper.py'), '--port', str(port), '--bind', '127.0.0.1',
                          '--name', 'testhelper'] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for _ in range(50):
        try:
            socket.create_connection(('127.0.0.1', port), timeout=0.2).close()
            return p
        except OSError:
            time.sleep(0.1)
    return p


def get(port, path, token=None):
    req = urllib.request.Request(f'http://127.0.0.1:{port}{path}', headers={'X-Aurora-Token': token} if token else {})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def main():
    tmp = tempfile.mkdtemp(prefix='a68-helper-test-')
    movie = os.path.join(tmp, 'Test pattern.mp4')
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i', 'testsrc=size=640x480:rate=30:duration=4',
                    '-f', 'lavfi', '-i', 'sine=frequency=440:duration=4', '-shortest', '-c:v', 'libx264', '-pix_fmt', 'yuv420p',
                    '-c:a', 'aac', movie], check=True)
    port = free_port()
    helper = start(['--file', movie], port)

    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.settimeout(5)
    u.sendto(b'AURORA68-VIDEO?', ('127.0.0.1', port))
    try:
        found = json.loads(u.recvfrom(512)[0])
    except (socket.timeout, ValueError):
        found = {}
    check(found.get('name') == 'testhelper' and found.get('port') == port and found.get('mode') == 'files',
          f'the UDP probe finds it ({found})')

    st, body = get(port, '/info')
    check(st == 200 and json.loads(body).get('mode') == 'files', f'/info answers ({body[:80]})')
    st, body = get(port, '/search?q=pattern')
    res = json.loads(body).get('results') or []
    check(st == 200 and len(res) == 1 and res[0]['title'] == 'Test pattern.mp4' and res[0]['seconds'] == 4 and res[0]['video'] == 'file:0',
          f'/search names the file and its length ({res[:1]})')

    st, data = get(port, '/play?video=file:0&w=320&h=240')
    out = os.path.join(tmp, 'got.mpg'); open(out, 'wb').write(data)
    probe = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=format_name:stream=codec_name,width,height,sample_rate,channels',
                            '-of', 'json', out], capture_output=True, text=True)
    info = json.loads(probe.stdout or '{}')
    streams = {s['codec_name']: s for s in info.get('streams', [])}
    check(st == 200 and info.get('format', {}).get('format_name', '').startswith('mpeg'), f'/play sends an MPEG program stream ({len(data)} bytes)')
    v, a = streams.get('mpeg1video', {}), streams.get('mp2', {})
    check(v.get('width') == 320 and v.get('height') == 240, f'MPEG-1 video at 320x240 ({v.get("width")}x{v.get("height")})')
    check(a.get('sample_rate') == '44100' and a.get('channels') == 2, f'MP2 audio, 44.1 kHz stereo ({a.get("sample_rate")}, {a.get("channels")})')

    # a play the Mac stops after the first bytes: the helper's ffmpeg must not be left running
    s = socket.create_connection(('127.0.0.1', port))
    s.sendall(b'GET /play?video=file:0&w=640&h=480 HTTP/1.0\r\n\r\n')
    s.recv(4096); s.close()
    time.sleep(3)
    left = subprocess.run(['pgrep', '-f', f'ffmpeg.*{tmp}'], capture_output=True, text=True).stdout.split()
    check(not left, f'a stopped play leaves no ffmpeg behind ({left})')

    st, body = get(port, '/play?video=file:7')
    check(st == 404 and b'error' in body, 'a file that was not given is refused, in words')
    st, body = get(port, '/play?video=../../etc/passwd')
    check(st == 404, 'a path is refused')
    helper.terminate(); helper.wait(10)

    port2 = free_port()
    h2 = start(['--file', movie, '--token', 'sekrit-token'], port2)
    st, _ = get(port2, '/info')
    check(st == 403, 'with --token, a client without it is refused')
    st, _ = get(port2, '/info', token='sekrit-token')
    check(st == 200, 'and one with it is served')
    h2.terminate(); h2.wait(10)
    print(helper.stdout.read())
    print(f'{fails} failed')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
