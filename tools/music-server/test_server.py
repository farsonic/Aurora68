#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""test_server.py - the music server as Aurora68 Play and other players use it.

Makes a small library with ffmpeg: folders inside folders, a file at the top, files at 44.1 and 48 kHz,
variable bit rate (with its Xing frame) and 22.05 kHz mono, ID3v2.3 and 2.4 titles in UTF-8 and Latin-1,
a name with accents, a file that is not MP3 at all, and two symbolic links that lead out of the folder.
Then it starts the server on a free port and checks:

  - the stations file, through Aurora68 Play's own parser (mac/piplay/stations.c, built on the host
    into play_check): Mac Roman, the groups, every address short enough and answering;
  - a station with ICY titles, through Aurora68 Play's own ICY parser (mac/piplay/icy.c) in pieces of
    random sizes: the titles in order, and the audio decoding with ffmpeg without a single error, as
    long as the files together;
  - pacing: the first second brings the burst, not the whole station;
  - a file with Range, the M3U, the JSON listing, the web page;
  - refusals: paths out of the folder, the links that lead out, a folder that is not there;
  - IPv6 (::1) where this computer has it; where it has not, that the server says so and serves IPv4.

    python3 tools/music-server/test_server.py
"""
import json, os, socket, subprocess, sys, tempfile, time, urllib.error, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
fails = 0


def check(ok, what):
    global fails
    print(('ok   ' if ok else 'FAIL ') + what, flush=True)
    if not ok:
        fails += 1


def ffmpeg(*a):
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y'] + list(a), check=True)


def make_library(d):
    def tone(path, secs, freq, extra, meta):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        m = sum((['-metadata', f'{k}={v}'] for k, v in meta.items()), [])
        ffmpeg('-f', 'lavfi', '-i', f'sine=frequency={freq}:duration={secs}', *extra, *m, path)
    cbr = ['-c:a', 'libmp3lame', '-b:a', '128k', '-ar', '44100', '-ac', '2', '-id3v2_version', '3']
    vbr = ['-c:a', 'libmp3lame', '-q:a', '4', '-ar', '48000', '-ac', '2']                 # ID3v2.4, Xing frame
    mono = ['-c:a', 'libmp3lame', '-b:a', '32k', '-ar', '22050', '-ac', '1', '-id3v2_version', '3']
    tone(f'{d}/Alpha Band/First Album/01 Opening.mp3', 2, 440, cbr, {'title': 'Opening', 'artist': 'Alpha Band'})
    tone(f'{d}/Alpha Band/First Album/02 Second.mp3', 2, 550, cbr, {'title': 'Second Song', 'artist': 'Alpha Band'})
    tone(f'{d}/Alpha Band/Café Sessions/01 Crème.mp3', 5, 660, vbr, {'title': 'Crème brûlée', 'artist': 'Alpha Band'})
    tone(f'{d}/Beta/Talk/Deeper/01 Voice.mp3', 2, 330, mono, {})                          # no tag: its name
    tone(f'{d}/Loose track.mp3', 1, 880, cbr, {'title': "It's loose", 'artist': 'Nobody'})
    for album in ['Zed - One', 'Zed - Two', 'The Apes - First', 'Moss - A', 'Moss - B', 'Kite - K']:   # by artist
        tone(f'{d}/Shelf/{album}/01 Song.mp3', 1, 500, cbr, {'title': album, 'artist': album.split(' - ')[0]})
    open(f'{d}/Beta/Talk/broken.mp3', 'wb').write(os.urandom(5000))                       # not MP3 at all
    os.makedirs(f'{d}/Empty folder', exist_ok=True)
    os.symlink('/etc/passwd', f'{d}/Beta/evil.mp3')
    os.symlink('/etc', f'{d}/Beta/outside')


def get(base, path, headers=None):
    req = urllib.request.Request(base + path, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def raw(host, port, path, seconds=None, family=socket.AF_INET):
    """the whole answer, headers and all, as Aurora68 Play's TCPRcv sees it; seconds: stop after that"""
    s = socket.socket(family)
    s.connect((host, port))
    s.sendall(f'GET {path} HTTP/1.0\r\nHost: {host}:{port}\r\nUser-Agent: Aurora68-Play/1.1 (Macintosh)\r\n'
              f'Accept: */*\r\nIcy-MetaData: 1\r\nConnection: close\r\n\r\n'.encode())
    out, t0 = b'', time.monotonic()
    s.settimeout(1)
    while seconds is None or time.monotonic() - t0 < seconds:
        try:
            b = s.recv(65536)
        except socket.timeout:
            continue
        if not b:
            break
        out += b
    s.close()
    return out


def main():
    tmp = tempfile.mkdtemp(prefix='a68-music-test-')
    lib = os.path.join(tmp, 'music')
    make_library(lib)
    pc = os.path.join(tmp, 'play_check')
    subprocess.run(['cc', '-std=c99', '-O1', '-o', pc, os.path.join(HERE, 'play_check.c'),
                    os.path.join(ROOT, 'mac/piplay/icy.c'), os.path.join(ROOT, 'mac/piplay/stations.c')], check=True)

    s = socket.socket(); s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]; s.close()
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'aurora68_music_server.py'), lib, '--port', str(port),
                            '--name', 'Test Music'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for _ in range(50):
        try:
            socket.create_connection(('127.0.0.1', port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    base = f'http://127.0.0.1:{port}'

    # the stations file, as Aurora68 Play reads it
    st, hd, body = get(base, '/stations.txt')
    check(st == 200 and b'\n' not in body and b'\r' in body, 'the stations file: Return-ended lines, no line feeds')
    check('Caf\x8e Sessions'.encode('latin-1') in body, 'the folder names in Mac Roman (Café: $8E)')
    parsed = subprocess.run([pc, 'stations'], input=body, capture_output=True).stdout.decode('mac_roman')
    stations = [l[8:].split('|') for l in parsed.splitlines() if l.startswith('station ')]
    groups = [l for l in parsed.splitlines() if l.startswith('group ')]
    names = [n for n, _, _ in stations]
    check(names[:3] == ['Everything, shuffled', 'Alpha Band', 'Beta'] and 'Loose track' not in names,
          f'Aurora68 Play reads the top stations ({names[:4]})')
    check(len(groups) == 4 and 'All of Alpha Band, shuffled' in names and 'Café Sessions' in names and 'Talk' in names,
          f'and the submenus: Test Music, its Alpha Band, Beta and Shelf ({len(groups)} groups, {len(stations)} stations)')
    check('Empty folder' not in names, 'a folder without music is no station')
    shelf = [n for n, _, g in stations if g == str(len(groups) - 1)]
    check(shelf == ['All of Shelf, shuffled', 'The Apes', 'Kite', 'Moss', 'Zed'],
          f'"Artist - Album" folders become artists, "The" filed under its next word ({shelf})')
    zed = [u for n, u, _ in stations if n == 'Zed'][0][len(base):]
    out = subprocess.run([pc, 'icy', os.path.join(tmp, 'zed.mp3')], input=raw('127.0.0.1', port, zed + '?loop=0'),
                         capture_output=True).stdout.decode('mac_roman')
    check('status 200 kind 1 metaint 16000 name Zed ' in out, f'an artist has a station ({out.splitlines()[-1] if out else ""})')
    pcm = subprocess.run(['ffmpeg', '-v', 'error', '-i', os.path.join(tmp, 'zed.mp3'), '-f', 's16le', '-ac', '1', '-ar', '8000', '-'],
                         capture_output=True).stdout
    check(1.8 < len(pcm) / 16000 < 2.3, f'playing both their albums, a second each ({len(pcm) / 16000:.2f} s)')
    j = json.loads(get(base, '/api/list?dir=Shelf')[2])
    check([a['name'] for a in j.get('artists', [])] == ['The Apes', 'Kite', 'Moss', 'Zed'], 'the JSON listing names the artists')
    for h, want in [('192.168.1.20', f'http://192.168.1.20:{port}/s/'), ('fd00::20', f'http://[fd00::20]:{port}/s/'),
                    ('bad host/x', base + '/s/')]:
        b2 = get(base, '/stations.txt?host=' + urllib.parse.quote(h))[2]
        check(f'= {want}'.encode() in b2, f'?host={h}: addresses {want}...')
    check(all(len(u) < 160 and u.startswith(base + '/s/') for _, u, _ in stations), 'every address is short (/s/ID)')
    check(all(raw('127.0.0.1', port, u[len(base):], seconds=0.5).startswith(b'HTTP/1.0 200') for _, u, _ in stations),
          'and every one answers as a station')

    # a station through Aurora68 Play's ICY parser: Alpha Band, in order, one pass
    alpha = [u for n, u, _ in stations if n == 'Alpha Band'][0][len(base):]
    t0 = time.monotonic()
    answer = raw('127.0.0.1', port, alpha + '?loop=0')
    took = time.monotonic() - t0
    audio = os.path.join(tmp, 'alpha.mp3')
    out = subprocess.run([pc, 'icy', audio], input=answer, capture_output=True).stdout.decode('mac_roman')
    titles = [l[6:] for l in out.splitlines() if l.startswith('title ')]   # each within 16,000 bytes of its start
    summary = out.splitlines()[-1] if out else ''
    check('status 200 kind 1 metaint 16000 name Alpha Band' in summary, f'Play sees a radio station ({summary})')
    check(titles == ['Alpha Band - Crème brûlée', 'Alpha Band - Opening', 'Alpha Band - Second Song'],
          f'the song titles, in Mac Roman and in folder order ({titles})')
    dec = subprocess.run(['ffmpeg', '-v', 'error', '-i', audio, '-f', 'null', '-'], capture_output=True, text=True)
    dur = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', audio],
                         capture_output=True, text=True).stdout.strip()
    check(dec.returncode == 0 and not dec.stderr.strip(), f'the audio decodes without an error ({dec.stderr.strip()[:200]!r})')
    # ffprobe estimates a mixed-rate stream's length from its first frames: decode it to count instead
    pcm = subprocess.run(['ffmpeg', '-v', 'error', '-i', audio, '-f', 's16le', '-ac', '1', '-ar', '8000', '-'],
                         capture_output=True).stdout
    secs = len(pcm) / 16000
    check(8.7 < secs < 9.3, f'nine seconds of audio: three files, the Xing frame and the tags left out ({secs:.2f} s; ffprobe {dur})')
    check(2.0 < took < 4.5, f'nine seconds of audio take about three: six at once, then as they play ({took:.1f} s)')

    # pacing: everything, looping: the first second brings about the burst (6 s), not more
    answer = raw('127.0.0.1', port, '/radio/?shuffle=1', seconds=2.5)
    subprocess.run([pc, 'icy', audio], input=answer, capture_output=True)
    pcm = subprocess.run(['ffmpeg', '-v', 'error', '-i', audio, '-f', 's16le', '-ac', '1', '-ar', '8000', '-'],
                         capture_output=True).stdout
    secs = len(pcm) / 16000
    check(6 <= secs < 10.5, f'pacing: 2.5 s of listening brings the burst and the time since ({secs:.1f} s of audio)')

    # files, playlists, listing, page
    st, hd, body = get(base, '/file/Alpha%20Band/First%20Album/01%20Opening.mp3', {'Range': 'bytes=100-199'})
    check(st == 206 and len(body) == 100 and hd.get('Content-Range', '').startswith('bytes 100-199/'), 'a file with Range')
    st, _, body = get(base, '/playlist.m3u?dir=Alpha%20Band')
    check(st == 200 and body.count(b'/file/') == 3 and body.startswith(b'#EXTM3U'), 'the M3U names three files')
    st, _, body = get(base, '/api/list?dir=Beta/Talk')
    j = json.loads(body)
    check(j['folders'] == ['Deeper'] and j['files'] == ['broken.mp3'], f'the JSON listing ({j})')
    st, _, body = get(base, '/browse/Alpha%20Band/')
    check(st == 200 and b'Caf\xc3\xa9 Sessions' in body and b'/s/' in body, 'the web page lists the folders and their stations')
    st, hd, _ = get(base, '/radio/Beta/Talk?loop=0')
    check(st == 200, 'a station with a broken file plays the rest')

    # refusals
    for p in ['/file/../../../etc/passwd', '/file/%2e%2e/%2e%2e/etc/passwd', '/file/Beta/evil.mp3',
              '/file/Beta/outside/passwd', '/radio/../', '/radio/Beta/outside/', '/radio/Nowhere/', '/s/00000000',
              '/browse/../', '/api/list?dir=..']:
        st, _, body = get(base, p)
        check(st == 404 and b'root:' not in body, f'refused: {p}')

    # IPv6
    if socket.has_ipv6:
        try:
            c = socket.socket(socket.AF_INET6); c.settimeout(3); c.connect(('::1', port)); c.close()
            answer = raw('::1', port, alpha + '?loop=0', family=socket.AF_INET6)
            check(answer.startswith(b'HTTP/1.0 200'), 'IPv6: the same station over ::1')
        except OSError as e:
            srv.terminate(); srv.wait(10)
            logtext = srv.stdout.read()
            check('IPv4 only' in logtext, f'no IPv6 on this computer ({e}): the server said so and served IPv4')
            print(logtext)
            print(f'{fails} failed'); sys.exit(1 if fails else 0)
    srv.terminate(); srv.wait(10)
    print(srv.stdout.read())
    print(f'{fails} failed')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
