#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""test_server.py - the music server as Aurora68 Play and other players use it.

Makes a small library with ffmpeg: folders inside folders, a file at the top, files at 44.1 and 48 kHz,
variable bit rate (with its Xing frame) and 22.05 kHz mono, ID3v2.3 and 2.4 titles in UTF-8 and Latin-1,
a name with accents, a file that is not MP3 at all, and two symbolic links that lead out of the folder.
Then it starts the server on a free port and checks:

  - the stations file, through Aurora68 Play's own parser (play/core/stations.c, built on the host
    into play_check): Mac Roman, the groups, every address short enough and answering;
  - a station with ICY titles, through Aurora68 Play's own ICY parser (play/core/icy.c) in pieces of
    random sizes: the titles in order, and the audio decoding with ffmpeg without a single error, as
    long as the files together;
  - pacing: the first second brings the burst, not the whole station;
  - a file with Range, the M3U, the JSON listing, the web page;
  - refusals: paths out of the folder, the links that lead out, a folder that is not there;
  - Aurora68 Play's Library (/mac/, 22 November 2026): artists, albums and tracks in Mac Roman text, pages, search,
    covers dithered to one bit or the Mac's 256 colours, a page of thumbnails, one track from its start or from a
    second, and the UDP probe's answer; the same in UTF-8 with the longer records the macOS Play asks for
    (enc=utf8&full=1: folders, paths, the devices' whole state), the Live channel in text (/mac/now) and changed
    by /mac/command (22 November 2026, one wire format for both Plays: docs/08 item 107);
  - music videos (22 November 2026, videos_check): found beside the albums, probed once (the cache folder), attached
    to their album or artist, a copy left out; the file with Range, a remux (its picture's bytes unchanged) as
    fragmented MP4 and as HLS, a transcode, refused when off and beyond the limit, posters, soundtracks, the video
    card's MPEG-1 (/vmpeg/), the JSON and the /mac/ text with videos=1 in both forms, every /mac/ answer without it
    byte for byte a server's without videos, and videos sent to a Mac, to the SE (its sound) and to Live;
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


LONG_FOLDER = 'Beta/Talk/Deeper/' + ' '.join(['A very long folder name for a test of the fields that are not cut short'] * 2)


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
    # what download services give today: M4A (AAC with its cover picture, and ALAC), FLAC with Vorbis tags
    g = f'{d}/Gamma - Converted'
    os.makedirs(g, exist_ok=True)
    ffmpeg('-f', 'lavfi', '-i', 'color=red:s=64x64', '-frames:v', '1', f'{g}/cover.jpg')
    ffmpeg('-f', 'lavfi', '-i', 'sine=frequency=300:duration=3', '-i', f'{g}/cover.jpg', '-map', '0', '-map', '1',
           '-c:a', 'aac', '-b:a', '128k', '-c:v', 'mjpeg', '-disposition:v', 'attached_pic',
           '-metadata', 'title=Closing Time', '-metadata', 'artist=Gamma', f'{g}/01. Gamma - Closing Time.m4a')
    ffmpeg('-f', 'lavfi', '-i', 'sine=frequency=350:duration=2', '-c:a', 'alac', '-metadata', 'title=Lossless',
           '-metadata', 'artist=Gamma', f'{g}/02. Gamma - Lossless.m4a')
    ffmpeg('-f', 'lavfi', '-i', 'sine=frequency=400:duration=2', '-c:a', 'flac', '-metadata', 'TITLE=Flac Song',
           '-metadata', 'ARTIST=Gamma', f'{g}/03. Gamma - Flac Song.flac')
    # copies of one track, as the owner's Daft Punk album has (the artists' separator differs, the tags too)
    e = f'{d}/Epsilon - Copies'
    os.makedirs(e, exist_ok=True)
    for name, artist in [('05. Epsilon, Guest - Song (feat. Guest).m4a', 'Epsilon, Guest'),
                         ('05. Epsilon; Guest - Song (feat. Guest).m4a', 'Epsilon; Guest'),
                         ('05. Epsilon - Song (Live).m4a', 'Epsilon'), ('06. Epsilon - Other.m4a', 'Epsilon')]:
        ffmpeg('-f', 'lavfi', '-i', 'sine=frequency=520:duration=2', '-c:a', 'aac', '-b:a', '96k',
               '-metadata', f'artist={artist}', '-metadata', 'title=Song', f'{e}/{name}')
    tone(f'{d}/{LONG_FOLDER}/01 Long.mp3', 1, 770, cbr, {'title': 'Long', 'artist': 'Long'})   # a folder of more than 120 characters
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


def post(base, path, obj, aurora=True):
    req = urllib.request.Request(base + path, data=json.dumps(obj).encode(),
                                 headers={'Content-Type': 'application/json', **({'X-Aurora': '1'} if aurora else {})})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b'{}')


def stations_c():
    """the classic Play's built-in stations (play/core/stations.c's C string, Mac Roman) as text"""
    import re
    src = open(os.path.join(ROOT, 'play/core/stations.c'), encoding='latin-1').read()
    a = src.index('const char stations_builtin[] =')
    out = bytearray()
    for t in re.findall(r'"((?:[^"\\]|\\.)*)"', src[a:src.index('const long stations_builtin_len', a)]):
        i = 0
        while i < len(t):
            if t[i] == '\\':
                if t[i + 1] == 'x':
                    h = re.match(r'[0-9a-fA-F]+', t[i + 2:]).group(0)
                    out.append(int(h, 16) & 0xff)
                    i += 2 + len(h)
                    continue
                out += {'r': b'\r', 'n': b'\n', 't': b'\t'}.get(t[i + 1], t[i + 1].encode('latin-1'))
                i += 2
                continue
            out += t[i].encode('latin-1')
            i += 1
    return out.decode('mac_roman')


def devices_check(tmp):
    """22 November 2026: players as devices (announce, state, list, commands by long poll, the classic Play's
    text forms), internet radio for every player, and the library's generation, on a server of its own with
    a rescan every second and devices offline after three"""
    import shutil
    sys.path.insert(0, HERE)
    import aurora68_music_server as m
    check(m.parse_stations(m.RADIO_BUILTIN) == m.parse_stations(stations_c()) and len(m.parse_stations(m.RADIO_BUILTIN)) >= 40,
          'the server\'s radio list is the classic Play\'s built-in one (play/core/stations.c)')
    lib = os.path.join(tmp, 'music-devices')
    os.makedirs(lib)
    shutil.copytree(os.path.join(tmp, 'music', 'Alpha Band', 'First Album'), os.path.join(lib, 'Alpha - First'))
    s = socket.socket(); s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]; s.close()
    env = dict(os.environ, MUSIC_RESCAN='1', MUSIC_DEVICE_TTL='3')
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'aurora68_music_server.py'), lib, '--port', str(port),
                            '--name', 'Dev Music', '--discovery', '0'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, env=env)
    base = f'http://127.0.0.1:{port}'
    for _ in range(50):
        try:
            socket.create_connection(('127.0.0.1', port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    A = {'X-Aurora': '1'}
    try:
        # the library's generation: the same while nothing changes, one more when an album arrives
        info = json.loads(get(base, '/api/info')[2])
        g = info.get('library')
        check(info.get('name') == 'Dev Music' and isinstance(g, int) and info.get('albums') == 1, f'/api/info ({info})')
        time.sleep(1.3)
        check(json.loads(get(base, '/api/info')[2])['library'] == g, 'a rescan that finds nothing new keeps the generation')
        shutil.copytree(os.path.join(tmp, 'music', 'Shelf', 'Zed - One'), os.path.join(lib, 'Zed - One'))
        time.sleep(1.3)
        info = json.loads(get(base, '/api/info')[2])
        cat = json.loads(get(base, '/api/catalog')[2])
        check(info['library'] == g + 1 and info['albums'] == 2 and 'Zed' in [a['name'] for a in cat['artists']],
              f'a new album: the generation goes up and the catalogue has it, no restart ({g} -> {info["library"]})')
        g += 1
        mi = get(base, '/mac/info')[2].split(b'\r')[1].split(b'\t')
        check(mi[5] == str(g).encode(), f'/mac/info\'s sixth field is the generation ({mi})')

        # internet radio
        st = json.loads(get(base, '/api/radio')[2])['stations']
        check(st[0] == {'group': 'SomaFM', 'name': 'Groove Salad', 'url': 'http://ice2.somafm.com/groovesalad-128-mp3'}
              and any(x['name'] == 'Ö1' and x['group'] == 'By country/Austria' for x in st), '/api/radio: the stations, grouped')
        open(os.path.join(lib, 'radio.txt'), 'w').write('# mine\n[Home]\nKitchen = http://192.168.1.20:8000/kitchen.mp3\n')
        st2 = json.loads(get(base, '/api/radio')[2])['stations']
        check(len(st2) == len(st) + 1 and st2[-1] == {'group': 'Home', 'name': 'Kitchen', 'url': 'http://192.168.1.20:8000/kitchen.mp3'},
              'a stations file in the music folder adds the owner\'s own')
        os.rename(os.path.join(lib, 'radio.txt'), os.path.join(tmp, 'radio.txt.aside'))
        code, _, mr = get(base, '/mac/radio')
        check(code == 200 and b'By country/Austria\t\x851\thttp://ors-sn05.ors-shoutcast.at/oe1-q2a\r' in mr and b'\n' not in mr,
              '/mac/radio: tab fields, Return-ended lines, Mac Roman (Ö: $85)')

        # announce, state, list
        check(post(base, '/api/devices/announce', {'id': 'mac1', 'name': 'Study iMac', 'kind': 'macos'}, aurora=False)[0] == 403,
              'an announce without X-Aurora is refused')
        code, j = post(base, '/api/devices/announce', {'id': '../x', 'name': 'Bad', 'kind': 'macos'})
        check(code == 400 and 'id' in j.get('error', ''), 'an id that is not letters and digits is refused')
        code, j = post(base, '/api/devices/announce', {'id': 'mac1', 'name': 'Study iMac', 'kind': 'macos', 'version': '1.3'})
        check(code == 200 and j == {'ok': True, 'every': 20, 'library': g}, f'announce: every 20 s, and the generation ({j})')
        check(post(base, '/api/devices/nobody/state', {'playing': True})[0] == 404, 'a state from a device that never announced: refused')
        code, j = post(base, '/api/devices/mac1/state', {'playing': True, 'title': 'Opening', 'artist': 'Alpha Band', 'album': 'First',
                                                       'folder': 'Alpha - First', 'position': 12.34, 'length': 120, 'volume': 250})
        check(code == 200, 'a device\'s state')
        devs = json.loads(get(base, '/api/devices')[2])['devices']
        live_d, mac = devs[0], next((d for d in devs if d['id'] == 'mac1'), {})
        check(live_d['id'] == 'live' and live_d['is_live'] and live_d['online'] and live_d['name'] == 'Dev Music Live',
              'the list: the Live channel first, as the device "live"')
        check(mac.get('online') and mac['name'] == 'Study iMac' and mac['kind'] == 'macos' and not mac['is_live'] and
              mac['state']['title'] == 'Opening' and mac['state']['position'] == 12.3 and mac['state']['volume'] == 100,
              f'and the Mac, online, with what it plays ({mac})')

        # commands: queued for the device, fetched by a long poll
        tracks = json.loads(get(base, '/api/catalog')[2])['albums']
        alpha = next(a for a in tracks if a['artist'] == 'Alpha')
        code, j = post(base, '/api/devices/mac1/command', {'action': 'play', 'what': 'f:' + alpha['folder']})
        check(code == 200 and j.get('id') == 1, f'a command to the Mac: queued as number 1 ({j})')
        t0 = time.monotonic()
        code, _, body = get(base, '/api/devices/mac1/commands?wait=10', A)
        c = json.loads(body)['commands']
        check(code == 200 and time.monotonic() - t0 < 2 and len(c) == 1 and c[0]['action'] == 'play' and c[0]['id'] == 1 and
              [t['title'] for t in c[0]['tracks']] == ['Opening', 'Second Song'] and c[0]['tracks'][0]['seconds'] == 2 and
              c[0]['tracks'][0]['path'] == 'Alpha - First/01 Opening.mp3' and len(c[0]['tracks'][0]['tid']) == 8,
              f'the device fetches it at once: the tracks with path, id, title and length ({c})')
        check(get(base, '/api/devices/mac1/commands?wait=1')[0] == 403, 'fetching commands needs X-Aurora too')
        t0 = time.monotonic()
        c = json.loads(get(base, '/api/devices/mac1/commands?wait=1.5', A)[2])['commands']
        check(c == [] and 1.3 < time.monotonic() - t0 < 3, 'nothing to do: an empty list when the wait is over')
        got = {}
        th = __import__('threading').Thread(target=lambda: got.update(r=get(base, '/api/devices/mac1/commands?wait=10', A)))
        t0 = time.monotonic()
        th.start()
        time.sleep(0.5)
        post(base, '/api/devices/mac1/command', {'action': 'seek', 'position': 42.5})
        th.join(15)
        c = json.loads(got['r'][2])['commands']
        check(c and c[0]['action'] == 'seek' and c[0]['position'] == 42.5 and time.monotonic() - t0 < 3,
              'a command sent during the wait arrives at once (the long poll)')
        post(base, '/api/devices/mac1/command', {'action': 'volume', 'volume': 150})
        post(base, '/api/devices/mac1/command', {'action': 'play', 'what': 'r:0'})
        post(base, '/api/devices/mac1/command', {'action': 'play', 'what': 's:' + alpha['id'], 'shuffle': 1})
        c = json.loads(get(base, '/api/devices/mac1/commands?wait=0', A)[2])['commands']
        check([x['action'] for x in c] == ['volume', 'play', 'play'] and c[0]['volume'] == 100 and
              c[1]['radio'] == {'name': 'Groove Salad', 'url': 'http://ice2.somafm.com/groovesalad-128-mp3'} and c[1]['tracks'] == [],
              f'volume (kept to 100), and a radio station by its index in /api/radio ({c[:2]})')
        check(post(base, '/api/devices/mac1/command', {'action': 'dance'})[0] == 400 and
              post(base, '/api/devices/mac1/command', {'action': 'play', 'what': 't:../../etc/passwd'})[0] == 400 and
              post(base, '/api/devices/mac1/command', {'action': 'play', 'what': 'r:999'})[0] == 400 and
              post(base, '/api/devices/mac1/command', {'action': 'seek'})[0] == 400 and
              post(base, '/api/devices/nobody/command', {'action': 'pause'})[0] == 404, 'nonsense commands are refused')

        # the Live channel as a device
        code, j = post(base, '/api/devices/live/command', {'action': 'play', 'what': 'f:' + alpha['folder']})
        check(code == 200 and [q['path'] for q in j['live']['queue']] == ['Alpha - First/01 Opening.mp3', 'Alpha - First/02 Second.mp3'],
              'a command to "live" is the Live channel\'s own')
        check(post(base, '/api/devices/live/command', {'action': 'seek', 'position': 3})[0] == 400 and
              post(base, '/api/devices/live/command', {'action': 'play', 'what': 'r:0'})[0] == 400, 'Live does not seek or play radio')
        post(base, '/api/devices/live/command', {'action': 'stop'})
        check(json.loads(get(base, '/api/now')[2])['queue'] == [], 'stop on Live empties its queue')

        # the classic Play: text
        check(get(base, '/mac/announce?id=se1&name=Caf%8E%20SE&kind=classic&v=1.3')[0] == 403, 'a classic announce without X-Aurora: refused')
        code, _, body = get(base, '/mac/announce?id=se1&name=Caf%8E%20SE&kind=classic&v=1.3', A)
        check(code == 200 and body == f'ok\r20\r{g}\r'.encode(), f'/mac/announce: ok, 20, the generation ({body})')
        tid = c[2]['tracks'][0]['tid']
        title = c[2]['tracks'][0]['title']
        code, _, body = get(base, f'/mac/state?id=se1&p=1&pos=10&len=0&vol=80&t={tid}', A)
        se = next(d for d in json.loads(get(base, '/api/devices')[2])['devices'] if d['id'] == 'se1')
        check(code == 200 and se['name'] == 'Café SE' and se['kind'] == 'classic' and se['state']['title'] == title and
              se['state']['artist'] == 'Alpha Band' and se['state']['length'] == 2 and se['state']['volume'] == 80,
              f'/mac/state: the track\'s names and length from its id ({se["state"]})')
        code, _, body = get(base, '/mac/devices')
        rows = [r.split(b'\t') for r in body.split(b'\r') if r]
        check(code == 200 and rows[0][:4] == [b'live', b'Dev Music Live', b'live', b'1'] and
              [b'se1', b'Caf\x8e SE', b'classic', b'1', title.encode(), b'Alpha Band'] in rows, f'/mac/devices ({rows})')
        code, _, body = get(base, '/mac/devices?enc=utf8&full=1')
        rows = [r.split(b'\t') for r in body.split(b'\r') if r]
        se_row = next(r for r in rows if r[0] == b'se1')
        check(len(se_row) == 17 and se_row[1] == 'Café SE'.encode() and se_row[6] == b'1' and se_row[7] == b'10.0' and
              se_row[9] == b'80' and se_row[10] == b'First' and se_row[11] == b'Alpha - First' and int(se_row[12]) > 0 and
              rows[0][14] == b'1' and rows[0][0] == b'live',
              f'/mac/devices?full=1: the whole state, as /api/devices has it ({se_row})')
        post(base, '/api/devices/se1/command', {'action': 'play', 'what': 'f:' + alpha['folder']})
        post(base, '/api/devices/se1/command', {'action': 'play', 'what': 'r:' + str(next(i for i, x in enumerate(st) if x['name'] == 'Ö1'))})
        code, _, body = get(base, '/mac/commands?id=se1&wait=5', A)
        rows = [r.split(b'\t') for r in body.split(b'\r') if r]
        check(code == 200 and rows[0] == [b'1', b'play', b'0', b''] and rows[1][0] == b'T' and rows[1][2] == b'Opening'
              and rows[1][4] == b'2' and rows[3] == [b'2', b'play', b'0', b''] and rows[4] == [b'R', b'\x851', b'http://ors-sn05.ors-shoutcast.at/oe1-q2a'],
              f'/mac/commands: N action position volume, then T and R lines ({rows})')
        t0 = time.monotonic()
        code, _, body = get(base, '/mac/commands?id=se1&wait=1', A)
        check(code == 200 and body == b'' and time.monotonic() - t0 > 0.8, 'and an empty answer when the wait is over')
        code, _, body = get(base, '/mac/command?to=se1&action=play&what=' + urllib.parse.quote('f:' + alpha['folder']) +
                            '&shuffle=1&enc=utf8', A)
        code2, _, body2 = get(base, '/mac/commands?id=se1&wait=0&enc=utf8&full=1', A)
        rows = [r.split(b'\t') for r in body2.split(b'\r') if r]
        check(code == 200 and body.startswith(b'ok\t') and rows[0][1] == b'play' and rows[0][4] == b'1' and
              sorted(r[5] for r in rows[1:]) == [b'Alpha - First/01 Opening.mp3', b'Alpha - First/02 Second.mp3'] and
              rows[1][6] == b'First', f'/mac/command to a player, and its commands with full=1: shuffled, paths and album ({rows})')

        # offline: not heard from for the TTL (3 s here, 60 normally)
        for _ in range(4):                         # se1 keeps asking for commands: that counts as being heard from
            get(base, '/mac/commands?id=se1&wait=1', A)
        devs = {d['id']: d for d in json.loads(get(base, '/api/devices')[2])['devices']}
        check(not devs['mac1']['online'] and devs['se1']['online'], 'a device not heard from goes offline; one asking for commands stays on')
        code, j = post(base, '/api/devices/mac1/command', {'action': 'pause'})
        check(code == 409 and 'not online' in j.get('error', ''), f'a command to an offline device is refused ({j})')
        check(post(base, '/api/devices/announce', {'id': 'mac1', 'name': 'Study iMac', 'kind': 'macos'})[0] == 200 and
              post(base, '/api/devices/mac1/command', {'action': 'pause'})[0] == 200, 'and it is back with its next announce')
    finally:
        srv.terminate()
        srv.wait(10)
    out = srv.stdout.read()
    check('devices: Study iMac (macos, 127.0.0.1) is online' in out and '(changed: generation' in out,
          'the log says who came online and that the library changed')


def start_server(lib, name, env_extra):
    s = socket.socket(); s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]; s.close()
    env = dict(os.environ, **env_extra)
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'aurora68_music_server.py'), lib, '--port', str(port),
                            '--name', name, '--discovery', '0', '--bind', '0.0.0.0'], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, env=env)
    for _ in range(100):
        try:
            socket.create_connection(('127.0.0.1', port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    return srv, port


def make_videos(tmp):
    """music videos as the owner keeps them (22 November 2026), each a few seconds of ffmpeg's test picture and a
    tone at 160 x 120 (tens of kilobytes; never committed): an MP4 of H.264 and AAC in an album's folder (plays as it
    is) and its copy as QuickTime (left out), an MKV of H.264 and AAC in another album (remuxed), a WebM of VP9 and
    Opus in an artist's "Videos" folder with no album (transcoded), and a file named .mp4 that holds no picture"""
    import shutil
    lib = os.path.join(tmp, 'music-videos')
    shutil.copytree(os.path.join(tmp, 'music', 'Alpha Band', 'First Album'), os.path.join(lib, 'Alpha Band', 'First Album'))
    shutil.copytree(os.path.join(tmp, 'music', 'Gamma - Converted'), os.path.join(lib, 'Gamma - Converted'))
    os.makedirs(os.path.join(lib, 'Delta', 'Videos'))
    pic = ['-f', 'lavfi', '-i', 'testsrc=size=160x120:rate=25:duration=3']
    h264 = ['-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-g', '25']
    first = os.path.join(lib, 'Alpha Band', 'First Album', 'Opening (Café Video).mp4')
    ffmpeg(*pic, '-f', 'lavfi', '-i', 'sine=frequency=500:duration=3', *h264, '-c:a', 'aac', '-b:a', '64k', '-shortest', first)
    ffmpeg('-i', first, '-c', 'copy', first[:-4] + '.mov')                       # the same video twice: one is listed
    ffmpeg(*pic, '-f', 'lavfi', '-i', 'sine=frequency=600:duration=3', *h264, '-c:a', 'aac', '-b:a', '64k', '-shortest',
           os.path.join(lib, 'Gamma - Converted', '02. Gamma - Live Clip.mkv'))
    ffmpeg(*pic, '-f', 'lavfi', '-i', 'sine=frequency=700:duration=3', '-c:v', 'libvpx-vp9', '-b:v', '100k', '-deadline', 'realtime',
           '-c:a', 'libopus', '-b:a', '48k', '-shortest', os.path.join(lib, 'Delta', 'Videos', 'Delta - Webm Clip.webm'))
    ffmpeg('-f', 'lavfi', '-i', 'sine=frequency=300:duration=1', '-c:a', 'aac', os.path.join(lib, 'Delta', 'Videos', 'Not a video.mp4'))
    return lib


def probe(url_or_path, what='stream=codec_name,width,height,sample_rate,channels:format=format_name,duration'):
    out = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', what, '-of', 'json', url_or_path], capture_output=True, text=True)
    try:
        return json.loads(out.stdout or '{}')
    except ValueError:
        return {}


def pcm_seconds(path):
    pcm = subprocess.run(['ffmpeg', '-v', 'error', '-i', path, '-f', 's16le', '-ac', '1', '-ar', '8000', '-'], capture_output=True).stdout
    return len(pcm) / 16000


def no_ffmpeg_left(lib):
    for _ in range(20):
        if subprocess.run(['pgrep', '-f', f'ffmpeg.*{lib}'], capture_output=True).stdout == b'':
            return True
        time.sleep(0.25)
    return False


def videos_check(tmp, pc):
    """22 November 2026: music videos. Their scan, facts and attachment, the three ways they are sent (the file with
    Range; remuxed, as fragmented MP4 and as HLS; transcoded), a transcode refused when off and when the limit is
    reached, posters, soundtracks, the video card's MPEG-1, the JSON and /mac/ text in both forms, the classic answers
    unchanged without videos=1 (byte for byte against a server with VIDEOS=off), and videos sent to devices"""
    lib = make_videos(tmp)
    cache = os.path.join(tmp, 'cache-videos')
    srv, port = start_server(lib, 'Video Music', {'MUSIC_CACHE': cache, 'VIDEO_TRANSCODES': '1', 'MUSIC_RESCAN': '1'})
    off, port_off = start_server(lib, 'Video Music', {'MUSIC_CACHE': os.path.join(tmp, 'cache-off'), 'VIDEOS': 'off'})
    no_tc, port_tc = start_server(lib, 'Video Music', {'MUSIC_CACHE': os.path.join(tmp, 'cache-tc'), 'VIDEO_TRANSCODE': 'off'})
    base, b_off, b_tc = f'http://127.0.0.1:{port}', f'http://127.0.0.1:{port_off}', f'http://127.0.0.1:{port_tc}'
    A = {'X-Aurora': '1'}
    try:
        # the scan: three videos (the QuickTime copy left out, the .mp4 without a picture passed over), counted
        info = json.loads(get(base, '/api/info')[2])
        check(info['videos'] == 3 and info['tracks'] == 5 and set(info['capabilities']) >= {'videos', 'remux', 'hls', 'transcode', 'mpeg1'},
              f'/api/info: three videos and what the server can do ({info})')
        check(json.loads(get(b_off, '/api/info')[2])['videos'] == 0, 'VIDEOS=off: no videos')
        vs = {v['title']: v for v in json.loads(get(base, '/api/videos')[2])['videos']}
        mp4, mkv, webm = vs.get('Opening (Café Video)', {}), vs.get('Live Clip', {}), vs.get('Webm Clip', {})
        check(sorted(vs) == ['Live Clip', 'Opening (Café Video)', 'Webm Clip'] and mp4.get('path', '').endswith('.mp4'),
              f'the copy (the same title, sizes within 2%) is listed once, the MP4 kept; no picture, no video ({sorted(vs)})')
        check(mp4['seconds'] == 3 and mp4['width'] == 160 and mp4['height'] == 120 and mp4['vcodec'] == 'h264' and
              mp4['acodec'] == 'aac' and mp4['how'] == 'direct' and mkv['how'] == 'remux' and webm['how'] == 'transcode' and
              webm['vcodec'] == 'vp9' and webm['acodec'] == 'opus', f'length, size and codecs from ffprobe, and how each plays '
              f'({[(v["title"], v["how"], v["why"]) for v in vs.values()]})')
        check(mp4['artist'] == 'Alpha Band' and mp4['album_title'] == 'First Album' and mkv['artist'] == 'Gamma' and
              mkv['album_title'] == 'Converted' and mkv['number'] == 2 and webm['artist'] == 'Delta' and webm['album'] == '',
              'in an album\'s folder a video is the album\'s; in an artist\'s "Videos" folder the artist\'s; names tidied as tracks\'')
        check(os.path.isfile(os.path.join(cache, 'videos.json')) and len(json.load(open(os.path.join(cache, 'videos.json')))) == 4,
              'ffprobe\'s answers kept in the cache folder (videos.json), the file without a picture among them')
        time.sleep(1.2)
        check(json.loads(get(base, '/api/info')[2])['library'] == info['library'], 'a rescan with nothing new: the same generation')

        # the file as it is, with Range
        st, hd, body = get(base, mp4['video'], {'Range': 'bytes=0-1'})
        size = os.path.getsize(os.path.join(lib, mp4['path']))
        check(st == 206 and len(body) == 2 and hd.get('Content-Range') == f'bytes 0-1/{size}' and hd.get('Content-Type') == 'video/mp4',
              f'/video/ID, an MP4 of H.264 and AAC: the file, with Range ({st} {hd.get("Content-Range")})')
        st, hd, body = get(base, mp4['video'], {'Range': f'bytes={size - 100}-'})
        check(st == 206 and len(body) == 100, 'and its end, as AVFoundation asks for it')
        check(get(base, '/file/' + urllib.parse.quote(mp4['path']))[0] == 200 and get(base, '/video/00000000')[0] == 404,
              '/file/ gives a video as it is; a video that is not there: 404')

        # remuxed: fragmented MP4, the picture copied; and HLS
        out = os.path.join(tmp, 'remux.mp4')
        st, hd, body = get(base, mkv['video'])
        open(out, 'wb').write(body)
        j = probe(out)
        codecs = [x.get('codec_name') for x in j.get('streams', [])]
        dec = subprocess.run(['ffmpeg', '-v', 'error', '-i', out, '-f', 'null', '-'], capture_output=True, text=True)
        check(st == 200 and hd.get('Content-Type') == 'video/mp4' and 'mp4' in j.get('format', {}).get('format_name', '') and
              codecs == ['h264', 'aac'] and dec.returncode == 0 and not dec.stderr.strip() and 2.8 < float(j['format']['duration']) < 3.3,
              f'/video/ID, an MKV: remuxed into MP4 that ffprobe reads and ffmpeg decodes cleanly ({codecs}, {dec.stderr.strip()[:80]!r})')
        h = subprocess.run(['ffmpeg', '-v', 'error', '-i', out, '-map', '0:v', '-c', 'copy', '-f', 'h264', '-'], capture_output=True).stdout
        h0 = subprocess.run(['ffmpeg', '-v', 'error', '-i', os.path.join(lib, mkv['path']), '-map', '0:v', '-c', 'copy', '-f', 'h264', '-'],
                            capture_output=True).stdout
        check(h and h == h0, 'the picture was copied, not encoded again: the same H.264 bytes')
        st, hd, pl = get(base, mkv['hls'])
        j = probe(base + mkv['hls'])
        check(st == 200 and 'mpegurl' in hd.get('Content-Type', '') and b'#EXT-X-START:TIME-OFFSET=0' in pl and b'#EXT-X-MAP:URI="init.mp4"' in pl
              and [x.get('codec_name') for x in j.get('streams', [])] == ['h264', 'aac'],
              f'/video/ID/index.m3u8: HLS of fragmented MP4 from the start, which ffprobe plays ({pl[:60]!r})')
        hls_dir = [d for d in os.listdir(os.path.join(cache, 'hls')) if d.startswith(mkv['id'])]
        check(hls_dir and b'#EXT-X-ENDLIST' in get(base, mkv['hls'])[2] and get(base, f'/video/{mkv["id"]}/../../etc/passwd')[0] == 404,
              'the conversion kept in the cache, finished; a segment name that is not one: refused')
        check(get(base, mp4['video'] + '/index.m3u8')[0] == 404, 'a video that plays as it is has no HLS')

        # transcoded: VP9 and Opus become H.264 and AAC
        out = os.path.join(tmp, 'transcode.mp4')
        st, hd, body = get(base, webm['video'])
        open(out, 'wb').write(body)
        j = probe(out)
        check(st == 200 and [x.get('codec_name') for x in j.get('streams', [])] == ['h264', 'aac'] and j['streams'][0]['width'] == 160,
              f'/video/ID, a WebM of VP9 and Opus: transcoded to H.264 and AAC ({[x.get("codec_name") for x in j.get("streams", [])]})')
        j = probe(base + webm['hls'])
        check([x.get('codec_name') for x in j.get('streams', [])] == ['h264', 'aac'], 'and as HLS')
        check(no_ffmpeg_left(lib) and json.loads(get(base, '/api/info')[2])['transcoding'] == 0, 'no ffmpeg left, no transcode counted')

        # refused: transcoding off, and the limit (VIDEO_TRANSCODES=1) reached
        st, _, body = get(b_tc, webm['video'])
        tcv = {v['title']: v for v in json.loads(get(b_tc, '/api/videos')[2])['videos']}
        check(st == 415 and b'VIDEO_TRANSCODE=off' in body and get(b_tc, webm['hls'])[0] == 415 and
              get(b_tc, f'/vmpeg/{webm["id"]}')[0] == 415 and not tcv['Webm Clip']['playable'] and tcv['Live Clip']['playable'],
              f'VIDEO_TRANSCODE=off: a transcode refused in words, the video card\'s MPEG-1 too; a remux still plays ({body[:80]!r})')
        check(get(b_tc, mkv['video'])[0] == 200, 'and the remux plays there')
        # the limit: a long video (a minute of a 64 x 48 picture) held open by a client that reads no more
        longlib = os.path.join(tmp, 'music-long')
        os.makedirs(longlib)
        ffmpeg('-f', 'lavfi', '-i', 'testsrc=size=64x48:rate=25:duration=60', '-c:v', 'libvpx-vp9', '-b:v', '20k', '-deadline', 'realtime',
               os.path.join(longlib, 'Long - Clip.webm'))
        lsrv, lport = start_server(longlib, 'Long', {'MUSIC_CACHE': os.path.join(tmp, 'cache-long'), 'VIDEO_TRANSCODES': '1'})
        try:
            lb = f'http://127.0.0.1:{lport}'
            lv = json.loads(get(lb, '/api/videos')[2])['videos'][0]
            s = socket.create_connection(('127.0.0.1', lport))
            s.sendall(f'GET /vmpeg/{lv["id"]}?w=1280&h=720 HTTP/1.0\r\nHost: x\r\n\r\n'.encode())
            s.recv(100)
            time.sleep(1)
            st, _, body = get(lb, lv['video'])
            busy = json.loads(get(lb, '/api/info')[2])['transcoding']
            check(st == 503 and b'VIDEO_TRANSCODES=1' in body and busy == 1,
                  f'with one transcode running (the video card\'s), a second is refused (503, {body[:60]!r})')
            s.close()
            check(no_ffmpeg_left(longlib), 'a stream stopped by its client leaves no ffmpeg behind')
            time.sleep(0.3)
            check(json.loads(get(lb, '/api/info')[2])['transcoding'] == 0, 'and its turn is free again')
        finally:
            lsrv.terminate()
            lsrv.wait(10)
        llog = lsrv.stdout.read()
        check('refused, 1 transcode running already' in llog, 'the log says why it was refused')

        # the video card's MPEG-1 (docs/16), as the video helper makes it
        for (w, h), want in (((320, 240), (320, 240)), ((513, 343), (512, 342)), ((4000, 3000), (1280, 720))):
            out = os.path.join(tmp, 'card.mpg')
            st, hd, body = get(base, f'/vmpeg/{webm["id"]}?w={w}&h={h}')
            open(out, 'wb').write(body)
            j = probe(out)
            sv = [x for x in j.get('streams', []) if x.get('codec_name') == 'mpeg1video']
            sa = [x for x in j.get('streams', []) if x.get('codec_name') == 'mp2']
            check(st == 200 and hd.get('Content-Type') == 'video/mpeg' and 'Content-Length' not in hd and 'mpeg' in j.get('format', {}).get('format_name', '')
                  and sv and (sv[0]['width'], sv[0]['height']) == want and sa and sa[0]['sample_rate'] == '44100' and sa[0]['channels'] == 2,
                  f'/vmpeg/ID?w={w}&h={h}: an MPEG program stream, MPEG-1 at {want[0]}x{want[1]}, MP2 at 44.1 kHz stereo, no length')
        fr = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v', '-show_entries', 'frame=pict_type', '-of', 'csv=p=0', out],
                            capture_output=True, text=True).stdout.split()
        check(fr and 'B' not in fr, f'no B-frames ({"".join(fr)[:30]})')
        # rate= (the classic Play 1.6's, 23 November 2026): the video's kbit/s asked for, the helper's own without it
        big = len(get(base, f'/vmpeg/{webm["id"]}?w=1280&h=720')[2])
        st, hd, small = get(base, f'/vmpeg/{webm["id"]}?w=1280&h=720&rate=200')
        check(st == 200 and 0 < len(small) < big * 0.6, f'/vmpeg/ with rate=200: a smaller stream ({len(small)} bytes against {big})')
        raw('127.0.0.1', port, f'/vmpeg/{mp4["id"]}?w=320&h=240', seconds=0.5)
        check(no_ffmpeg_left(lib), 'an MPEG-1 stream stopped by its client leaves no ffmpeg behind')

        # the poster and the soundtrack
        st, hd, pic = get(base, webm['poster'])
        check(st == 200 and pic[:2] == b'\xff\xd8' and hd.get('Content-Type') == 'image/jpeg' and
              len(os.listdir(os.path.join(cache, 'thumbs'))) == 1, '/vthumb/ID: a JPEG poster, kept in the cache')
        check(get(base, webm['poster'])[2] == pic, 'asked again: the same picture, from the cache')
        for start, want in ((0, 3.0), (2, 1.0)):
            ans = raw('127.0.0.1', port, mkv['audio'] + (f'?start={start}' if start else ''))
            tone = os.path.join(tmp, 'vaudio.mp3')
            out = subprocess.run([pc, 'icy', tone], input=ans, capture_output=True).stdout.decode('mac_roman')
            secs = pcm_seconds(tone)
            check('status 200 kind 1' in out and abs(secs - want) < 0.4,
                  f'/vaudio/ID{"?start=" + str(start) if start else ""}: the soundtrack as MP3, as Aurora68 Play reads a track ({secs:.2f} s)')
        kept = os.listdir(os.path.join(cache, 'vaudio'))
        check(len(kept) == 1 and kept[0].endswith('-192.mp3'), f'the soundtrack converted once and kept ({kept})')
        ans = raw('127.0.0.1', port, f'/t/{mkv["id"]}')
        open(os.path.join(tmp, 'vaudio2.mp3'), 'wb').write(ans.split(b'\r\n\r\n', 1)[1])
        check(ans.startswith(b'HTTP/1.0 200') and abs(pcm_seconds(os.path.join(tmp, 'vaudio2.mp3')) - 3) < 0.4,
              '/t/ with a video\'s id: its sound (an older Play given a video as a track)')

        # the JSON
        cat = json.loads(get(base, '/api/catalog')[2])
        delta = next((a for a in cat['artists'] if a['name'] == 'Delta'), {})
        gal = next(a for a in cat['albums'] if a['title'] == 'Converted')
        check(delta.get('videos') == 1 and delta.get('albums') == [] and gal['videos'] == 1,
              '/api/catalog: an artist with only videos, and each album\'s videos counted')
        tr = json.loads(get(base, '/api/tracks?album=' + gal['id'])[2])
        check(len(tr['tracks']) == 3 and [v['title'] for v in tr['videos']] == ['Live Clip'], '/api/tracks: the album\'s videos after its tracks')
        check([v['title'] for v in json.loads(get(base, '/api/videos?artist=' + delta['id'])[2])['videos']] == ['Webm Clip'] and
              json.loads(get(base, '/api/tracks?artist=' + delta['id'])[2])['videos'][0]['title'] == 'Webm Clip',
              '/api/videos?artist=, and /api/tracks for an artist with videos alone')
        top = json.loads(get(base, '/api/list?dir=')[2])
        alb = json.loads(get(base, '/api/list?dir=' + urllib.parse.quote('Alpha Band/First Album'))[2])
        check([v['title'] for v in top.get('videos', [])] == ['Webm Clip'] and [v['title'] for v in alb.get('videos', [])] == ['Opening (Café Video)'],
              '/api/list: a folder\'s videos (a folder of videos alone shown in its parent), for the panel')
        page = get(base, '/', {'Accept': 'text/html'})[2]
        check(b'showVideos' in page and b'<video id="vid"' in page and b"application/vnd.apple.mpegurl" in page,
              'the control panel has videos: posters, a player in the page, Play on')

        # /mac/: the classic Play's form (Mac Roman) and the macOS Play's (UTF-8, full=1), with videos=1
        def rows(b, path, h=None):
            body = get(b, path, h)[2]
            return body, [l.split(b'\t') for l in body.split(b'\r') if l]
        _, r = rows(base, '/mac/info?videos=1')
        check(r[1][6:] == [b'20', b'3', b'videos,remux,hls,transcode,mpeg1'], f'/mac/info?videos=1: every, the videos, what it can do ({r[1]})')
        _, r = rows(base, '/mac/artists?videos=1')
        d = next((x for x in r[1:] if x[1] == b'Delta'), [])
        check(r[0][3] == b'3' and d[2:] == [b'0', b'0', b'1'] and all(len(x) == 5 for x in r[1:]),
              f'/mac/artists?videos=1: a fifth field, and Delta with videos alone ({d})')
        aid = next(x[0] for x in r[1:] if x[1] == b'Alpha Band').decode()
        alpha_al = next(a['id'] for a in cat['albums'] if a['title'] == 'First Album')
        body, r = rows(base, f'/mac/tracks?album={alpha_al}&videos=1')
        check(r[0][3:] == [b'3', b'0', b'3'] and r[3] == [b'V', mp4['id'].encode(), b'Opening (Caf\x8e Video)', b'Alpha Band', b'3',
                                                           alpha_al.encode(), b'160', b'120'],
              f'/mac/tracks?videos=1: the tracks, then V, id, title (Mac Roman), artist, seconds, album, width, height ({r[3]})')
        body, r = rows(base, f'/mac/tracks?album={alpha_al}&videos=1&enc=utf8&full=1')
        check(r[3][:3] == [b'V', mp4['id'].encode(), 'Opening (Café Video)'.encode()] and
              r[3][8:] == [mp4['path'].encode(), b'First Album', b'Alpha Band/First Album', b'direct'],
              f'and with enc=utf8&full=1: UTF-8, its path, album, folder, how it plays ({r[3][8:]})')
        _, r = rows(base, f'/mac/tracks?album={alpha_al}&videos=1&from=2&count=1')
        check(r[0][3:] == [b'3', b'2', b'1'] and r[1][0] == b'V', 'pages run through the tracks, then the videos')
        _, r = rows(base, f'/mac/tracks?artist={delta["id"]}&videos=1')
        check(r[0][3] == b'1' and r[1][0] == b'V' and get(base, f'/mac/tracks?artist={delta["id"]}')[0] == 404,
              'an artist with videos alone: its videos (without videos=1 there is no such artist)')
        _, r = rows(base, f'/mac/videos?artist={aid}&full=1&enc=utf8')
        check(r[0][2] == b'videos' and r[0][3] == b'1' and r[1][1] == mp4['id'].encode() and r[1][11] == b'direct', f'/mac/videos ({r})')
        _, r = rows(base, '/mac/search?q=clip&videos=1')
        check([x[0] for x in r[1:]] == [b'V', b'V'] and get(base, '/mac/search?q=clip')[2].count(b'\r') == 1,
              'a search with videos=1 finds videos; without it, none')

        # the classic answers are what they were without videos=1: byte for byte those of a server without videos
        for b in (base, b_off):                     # the same tracks' lengths read on both (an album's first listing reads them)
            for al in (alpha_al, gal['id']):
                get(b, '/mac/tracks?album=' + al)
        same = True
        for p in ['/mac/info', '/mac/info?enc=utf8&full=1', '/mac/artists', '/mac/artists?enc=utf8&full=1', '/mac/albums',
                  f'/mac/albums?artist={aid}&enc=utf8&full=1', f'/mac/tracks?album={alpha_al}', f'/mac/tracks?album={gal["id"]}&enc=utf8&full=1',
                  f'/mac/tracks?artist={aid}', '/mac/search?q=a', '/mac/search?q=clip&enc=utf8', '/mac/devices', '/mac/devices?full=1',
                  '/mac/now', '/mac/radio']:
            a, b = get(base, p)[2], get(b_off, p)[2]
            if a != b:
                same = False
                print('   differs:', p, a[:200], b[:200])
        check(same, 'without videos=1 every /mac/ answer is byte for byte what a server without videos sends')

        # devices: a Mac that shows videos, a classic Play that plays their sound
        check(post(base, '/api/devices/announce', {'id': 'mac1', 'name': 'iMac', 'kind': 'macos', 'version': '1.3', 'video': True})[0] == 200,
              'the macOS Play announces that it shows videos')
        st, _, body = get(base, '/mac/announce?id=se1&name=SE&kind=classic&v=1.5', A)
        code, j = post(base, '/api/devices/mac1/command', {'action': 'play', 'what': 'v:' + mkv['id']})
        c = json.loads(get(base, '/api/devices/mac1/commands?wait=0', A)[2])['commands']
        t = c[0]['tracks'][0] if c and c[0].get('tracks') else {}
        check(code == 200 and 'audio_only' not in j and t.get('video') is True and t.get('how') == 'remux' and t.get('tid') == mkv['id'] and
              t.get('path') == mkv['path'] and t.get('width') == 160, f'v:ID to the Mac: a track that is a video, how it plays, its size ({t})')
        code, j = post(base, '/api/devices/se1/command', {'action': 'play', 'what': 'v:' + mkv['id']})
        check(code == 200 and j.get('audio_only') is True, 'to the classic Play, which shows none: sent, and said to be its sound only')
        post(base, '/api/devices/se1/command', {'action': 'add', 'what': 'v:' + mp4['id']})
        _, r = rows(base, '/mac/commands?id=se1&wait=0&videos=1', A)
        check(r[0][:2] == [b'1', b'play'] and r[1] == [b'V', mkv['id'].encode(), b'Live Clip', b'Gamma', b'3'] and r[3][0] == b'V',
              f'/mac/commands?videos=1: a V line, id, title, artist, seconds ({r})')
        post(base, '/api/devices/se1/command', {'action': 'play', 'what': 'v:' + mkv['id']})
        _, r = rows(base, '/mac/commands?id=se1&wait=0', A)
        check(r[1] == [b'T', mkv['id'].encode(), b'Live Clip', b'Gamma', b'3'], 'without videos=1: a T line, whose id /t/ plays as the sound')
        post(base, '/api/devices/mac1/command', {'action': 'next', 'what': 'v:' + mp4['id']})
        _, r = rows(base, '/mac/commands?id=mac1&wait=0&videos=1&full=1&enc=utf8', A)
        check(r[1][0] == b'V' and r[1][5:] == [mp4['path'].encode(), b'First Album', b'direct', b'160', b'120'],
              f'with full=1: path, album, how, width, height ({r[1]})')
        check(post(base, '/api/devices/mac1/command', {'action': 'play', 'what': 'v:nothing0'})[0] == 400, 'a video that is not there: refused')
        get(base, f'/mac/state?id=se1&p=1&pos=1&len=0&vol=80&t={mkv["id"]}', A)
        devs = {d['id']: d for d in json.loads(get(base, '/api/devices')[2])['devices']}
        check(devs['mac1']['video'] and not devs['se1']['video'] and devs['se1']['state']['title'] == 'Live Clip' and
              devs['se1']['state']['video'] and devs['se1']['state']['length'] == 3,
              '/api/devices: who shows videos; the SE playing a video\'s sound, named from the catalogue')
        _, r = rows(base, '/mac/devices?videos=1')
        byid = {x[0]: x for x in r}
        check(len(byid[b'mac1']) == 18 and byid[b'mac1'][17] == b'1' and byid[b'se1'][17] == b'0' and byid[b'live'][17] == b'0',
              '/mac/devices?videos=1: the full record and an 18th field, whether the device shows videos')
        code, j = post(base, '/api/devices/live/command', {'action': 'play', 'what': 'v:' + mkv['id']})
        check(code == 200 and [q['path'] for q in j['live']['queue']] == [mkv['path']], 'v:ID on Live: its sound joins the Live queue')
        post(base, '/api/devices/live/command', {'action': 'stop'})
    finally:
        for p in (srv, off, no_tc):
            p.terminate()
            p.wait(10)
    log = srv.stdout.read()
    want = ['and 3 music videos (1 as they are, 1 remuxed, 1 transcoded)', '1 video file passed over', 'HLS ready',
            'copy of tracks left out']
    check(all(w in log for w in want), f'the log: the videos found, how they play, the copy, the HLS ({[w for w in want if w not in log]})')
    check('VIDEO_TRANSCODE=off' in no_tc.stdout.read(), 'and the server with transcoding off says why it refused')
    check(no_ffmpeg_left(lib), 'no ffmpeg left when the servers stop')


def links_check(tmp):
    """23 November 2026: an album's songs in track order when the files are named by title alone (the owner's "Discovery",
    listed by title by the macOS Play), across pages and by disc; and music videos linked to songs (README "Videos linked
    to songs"): by artist and title, the tags a video's name carries left out, a Live video to the Live song, a video of
    one song on two albums, an album's list with its songs' videos from the artist's folder, each once. Its own library
    of tones and ffmpeg's test picture, a few seconds each, never committed"""
    import shutil
    sys.path.insert(0, HERE)
    import aurora68_music_server as m
    lt = m.link_title
    check(lt('One More Time (Official Video)') == ('one more time', 'one more time') and
          lt('Harder, Better, Faster, Stronger (Official Music Video) [HD]') == ('harder better faster stronger',) * 2 and
          lt('Song [4K] (Lyric Video)') == ('song', 'song') and lt('Song (feat. Guest) - Remastered 2011') == ('song', 'song') and
          lt('Get Lucky feat. Pharrell Williams - Radio Edit') == ('get lucky radio edit', 'get lucky') and
          lt('Song (Live)') == ('song live', 'song') and lt('Y (Acoustic Version)') == ('y acoustic version', 'y') and
          lt('Don’t Stop (Visualiser)') == lt("Don't stop") and lt('ＯＮＥ') == ('one', 'one'),
          'a title\'s keys: casefolded, NFKC, the file\'s tags left out (official video, HD, lyric video, feat., remastered), '
          'a version\'s kept (Live, Radio Edit, Acoustic)')
    check(m.link_names('Daft Punk, Julian Casablancas - Instant Crush', 'Daft Punk') == [('daft punk', 'instant crush', 'instant crush')] and
          ('daft punk', 'around the world', 'around the world') in m.link_names('Daft Punk - Around the World', 'Various') and
          m.link_artist('The Beatles') == 'beatles', 'an "Artist - " in front: the artist\'s own title, or another artist\'s')
    lib = os.path.join(tmp, 'music-links')
    dp = os.path.join(lib, 'Daft Punk')

    def song(path, n, title, disc=None):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        ffmpeg('-f', 'lavfi', '-i', f'sine=frequency={300 + 20 * n}:duration=1', '-c:a', 'flac', '-metadata', f'TITLE={title}',
               '-metadata', 'ARTIST=Daft Punk', '-metadata', f'TRACKNUMBER={n}', *(['-metadata', f'DISCNUMBER={disc}'] if disc else []), path)
    disc = [(1, 'One More Time'), (2, 'Aerodynamic'), (3, 'Digital Love'), (4, 'Harder, Better, Faster, Stronger'),
            (5, 'Crescendolls'), (6, 'Face to Face')]
    for n, t in disc:                                # named by title alone, as the owner's downloader names them
        song(f'{dp}/Discovery/{t}.flac', n, t)
    song(f'{dp}/Discovery (Deluxe)/One More Time (Radio Edit).flac', 1, 'One More Time (Radio Edit)')
    song(f'{dp}/Discovery (Deluxe)/Aerodynamic.flac', 2, 'Aerodynamic')
    song(f'{dp}/Alive 2007/One More Time (Live).flac', 1, 'One More Time (Live)')
    for name, n, d in (('A Song', 1, '2/2'), ('B Song', 2, '1/2'), ('C Song', 1, '1/2')):   # two discs in one folder
        song(f'{lib}/Zeta - Double/{name}.flac', n, name, d)
    clip = os.path.join(tmp, 'links-clip.mp4')
    ffmpeg('-f', 'lavfi', '-i', 'testsrc=size=160x120:rate=25:duration=2', '-f', 'lavfi', '-i', 'sine=frequency=500:duration=2',
           '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '64k', '-shortest', clip)
    vids = {'official': f'{dp}/One More Time (Official Video).mp4', 'live': f'{dp}/Daft Punk - One More Time (Live).mp4',
            'hbfs': f'{dp}/Videos/Daft Punk - Harder, Better, Faster, Stronger (Official Music Video) [HD].mp4',
            'aero': f'{dp}/Videos/Aerodynamic (Visualizer).mp4', 'other': f'{lib}/Other Band - One More Time.mp4'}
    for k, p in vids.items():
        os.makedirs(os.path.dirname(p), exist_ok=True)
        shutil.copy(clip, p)
    srv, port = start_server(lib, 'Links', {'MUSIC_CACHE': os.path.join(tmp, 'cache-links')})
    base = f'http://127.0.0.1:{port}'
    try:
        cat = json.loads(get(base, '/api/catalog')[2])
        al = {a['title']: a for a in cat['albums']}
        dpa = next(a for a in cat['artists'] if a['name'] == 'Daft Punk')
        vs = {v['path']: v for v in json.loads(get(base, '/api/videos')[2])['videos']}
        vid = {k: vs[os.path.relpath(p, lib)]['id'] for k, p in vids.items()}
        byid = {v['id']: v for v in vs.values()}

        def rows(path):
            body = get(base, path)[2]
            return [x.split(b'\t') for x in body.split(b'\r') if x]
        # the order: by track number across two pages, as the macOS Play asks (and the classic Play, and the JSON)
        d = al['Discovery']['id']
        want = [t for _, t in disc]
        page1 = rows(f'/mac/tracks?album={d}&enc=utf8&full=1&videos=1&count=4')
        page2 = rows(f'/mac/tracks?album={d}&enc=utf8&full=1&videos=1&from=4&count=10')
        t_rows = [r for r in page1[1:] + page2[1:] if r[0] != b'V']
        check([r[2].decode() for r in t_rows] == want and [int(r[1]) for r in t_rows] == list(range(1, 7)),
              f'files named by title, numbers in the tags: the album in track order across pages ({[r[2].decode() for r in t_rows]})')
        classic = rows(f'/mac/tracks?album={d}&count=4')[1:] + rows(f'/mac/tracks?album={d}&from=4')[1:]
        check([r[2].decode('mac_roman') for r in classic] == want and all(len(r) == 6 for r in classic),
              'and as the classic Play asks (Mac Roman, short records): the same order, six fields')
        js = json.loads(get(base, f'/api/tracks?album={d}')[2])
        check([t['title'] for t in js['tracks']] == want, '/api/tracks: the same order')
        z = rows(f'/mac/tracks?album={al["Double"]["id"]}&enc=utf8&full=1')
        check([r[2] for r in z[1:]] == [b'C Song', b'B Song', b'A Song'], f'two discs in one folder: disc, then track ({[r[2] for r in z[1:]]})')

        # the links: T records with full=1&videos=1 end with the song's video; V records list the album's and its songs'
        tv = {r[2].decode(): r for r in t_rows}
        check(all(len(r) == 10 for r in t_rows) and tv['One More Time'][9] == vid['official'].encode() and
              tv['Aerodynamic'][9] == vid['aero'].encode() and tv['Harder, Better, Faster, Stronger'][9] == vid['hbfs'].encode() and
              tv['Digital Love'][9] == b'', 'full=1&videos=1: a tenth field, the song\'s video (official video, visualizer, '
              '[HD] left out), empty for a song without one')
        v_rows = [r[1].decode() for r in page1[1:] + page2[1:] if r[0] == b'V']
        check(v_rows == [vid['official'], vid['aero'], vid['hbfs']] and page2[0][3] == b'9',
              f'the album lists the videos of its songs from the artist\'s folder, in its songs\' order, once ({v_rows})')
        check(byid[vid['live']]['tracks'] == [t['id'] for t in json.loads(get(base, f'/api/tracks?album={al["Alive 2007"]["id"]}')[2])['tracks']]
              and byid[vid['official']]['tracks'] == [js['tracks'][0]['id']],
              'a Live video is the Live song\'s; the official video the studio song\'s, not the Radio Edit\'s (nothing better only)')
        deluxe = json.loads(get(base, f'/api/tracks?album={al["Discovery (Deluxe)"]["id"]}')[2])
        aero = [t['id'] for t in js['tracks'] + deluxe['tracks'] if t['title'] == 'Aerodynamic']
        check(byid[vid['aero']]['tracks'] == [aero[1], aero[0]] or sorted(byid[vid['aero']]['tracks']) == sorted(aero),
              'one video of a song on two albums: linked to both')
        check(js['tracks'][0].get('video') == vid['official'] and 'video' not in js['tracks'][2] and
              [v['id'] for v in js['videos']] == v_rows and byid[vid['other']]['tracks'] == [],
              '/api/tracks: "video" on a song that has one; another artist\'s video of the same title is not linked')
        arts = rows(f'/mac/videos?artist={dpa["id"]}&enc=utf8&full=1')
        ids = [r[1].decode() for r in arts[1:]]
        check(sorted(ids) == sorted(vid[k] for k in ('official', 'live', 'hbfs', 'aero')) and len(set(ids)) == len(ids) and dpa['videos'] == 4,
              f'the artist\'s videos: those attached to it, each once ({len(ids)})')
        alb = rows(f'/mac/albums?artist={dpa["id"]}&enc=utf8&full=1&videos=1')
        counts = {r[1].decode(): r[7] for r in alb[1:]}
        check(counts == {'Alive 2007': b'1', 'Discovery': b'3', 'Discovery (Deluxe)': b'1'} and
              all(len(r) == 7 for r in rows(f'/mac/albums?artist={dpa["id"]}&enc=utf8&full=1')[1:]),
              f'/mac/albums with full=1&videos=1: an eighth field, the album\'s videos; without videos=1 seven ({counts})')
        lst = json.loads(get(base, '/api/list?dir=' + urllib.parse.quote('Daft Punk/Discovery'))[2])
        check(sorted(lst.get('file_videos', {})) == ['Aerodynamic.flac', 'Harder, Better, Faster, Stronger.flac', 'One More Time.flac'],
              '/api/list: the songs with a video, for the panel\'s film mark')
    finally:
        srv.terminate()
        srv.wait(10)


def main():
    tmp = tempfile.mkdtemp(prefix='a68-music-test-')
    lib = os.path.join(tmp, 'music')
    make_library(lib)
    pc = os.path.join(tmp, 'play_check')
    subprocess.run(['cc', '-std=c99', '-O1', '-I', os.path.join(ROOT, 'play/core/include'), '-o', pc, os.path.join(HERE, 'play_check.c'),
                    os.path.join(ROOT, 'play/core/icy.c'), os.path.join(ROOT, 'play/core/stations.c')], check=True)

    s = socket.socket(); s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]; s.close()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind(('127.0.0.1', 0)); dport = s.getsockname()[1]; s.close()
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'aurora68_music_server.py'), lib, '--port', str(port),
                            '--name', 'Test Music', '--discovery', str(dport)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
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
    check(names[:4] == ['Everything, shuffled', 'Test Music Live', 'Alpha Band', 'Beta'] and 'Loose track' not in names,
          f'Aurora68 Play reads the top stations ({names[:4]})')
    check(len(groups) == 4 and 'Gamma - Converted' in names and 'All of Alpha Band, shuffled' in names and 'Café Sessions' in names and 'Talk' in names,
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
    check(all(len(u) < 160 and (u.startswith(base + '/s/') or u == base + '/live') for _, u, _ in stations), 'every address is short (/s/ID, /live)')
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

    # other formats, converted to MP3 as they are sent: AAC with a cover picture, ALAC, FLAC
    gamma = [u for n, u, _ in stations if n == 'Gamma - Converted'][0][len(base):]
    answer = raw('127.0.0.1', port, gamma + '?loop=0')
    out = subprocess.run([pc, 'icy', audio], input=answer, capture_output=True).stdout.decode('mac_roman')
    titles = [l[6:] for l in out.splitlines() if l.startswith('title ')]
    check(titles == ['Gamma - Closing Time', 'Gamma - Lossless', 'Gamma - Flac Song'],
          f'M4A (AAC and ALAC) and FLAC play, titles from their tags ({titles})')
    dec = subprocess.run(['ffmpeg', '-v', 'error', '-i', audio, '-f', 'null', '-'], capture_output=True, text=True)
    pcm = subprocess.run(['ffmpeg', '-v', 'error', '-i', audio, '-f', 's16le', '-ac', '1', '-ar', '8000', '-'],
                         capture_output=True).stdout
    check(dec.returncode == 0 and not dec.stderr.strip() and 6.7 < len(pcm) / 16000 < 7.4,
          f'converted to MP3 that decodes cleanly, all seven seconds ({len(pcm) / 16000:.2f} s) {dec.stderr.strip()[:100]!r}')
    st, hd, body = get(base, '/file/Gamma%20-%20Converted/03.%20Gamma%20-%20Flac%20Song.flac')
    check(st == 200 and body[:4] == b'fLaC' and 'flac' in hd.get('Content-Type', ''), 'a FLAC file as it is, for browsers')
    check(subprocess.run(['pgrep', '-f', f'ffmpeg.*{tmp}'], capture_output=True).stdout == b'',
          'no converting ffmpeg left running')

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

    # a listener who leaves in the middle of a converted song: its ffmpeg goes with them
    raw('127.0.0.1', port, gamma, seconds=0.5)
    time.sleep(1.5)
    check(subprocess.run(['pgrep', '-f', f'ffmpeg.*{tmp}'], capture_output=True).stdout == b'',
          'a listener leaving stops the conversion')

    # refusals
    for p in ['/file/../../../etc/passwd', '/file/%2e%2e/%2e%2e/etc/passwd', '/file/Beta/evil.mp3',
              '/file/Beta/outside/passwd', '/radio/../', '/radio/Beta/outside/', '/radio/Nowhere/', '/s/00000000',
              '/browse/../', '/api/list?dir=..']:
        st, _, body = get(base, p)
        check(st == 404 and b'root:' not in body, f'refused: {p}')

    # copies of one track are listed once; a live version, another title, stays
    files = json.loads(get(base, '/api/list?dir=Epsilon%20-%20Copies')[2])['files']
    check(len(files) == 3 and sum('feat. Guest' in f for f in files) == 1 and any('(Live)' in f for f in files),
          f'copies of one track are listed once, other versions stay ({files})')

    # the control panel and the live channel
    st, _, body = get(base, '/', {'Accept': 'text/html,application/xhtml+xml,*/*;q=0.8'})
    check(st == 200 and b'Test Music' in body and b'/api/live' in body, 'the control panel page, for a browser')
    first = raw('127.0.0.1', port, '/', seconds=0.5)          # as Aurora68 Play asks: Accept: */*
    check(first.startswith(b'HTTP/1.0 302') and b'Location: /live' in first, 'a player asking for "/" is sent to /live')
    out = subprocess.run([pc, 'icy', os.path.join(tmp, 'redir.mp3')], input=first, capture_output=True).stdout.decode('mac_roman')
    check('status 302' in out, f'which Aurora68 Play reads as a redirection it follows ({out.splitlines()[-1] if out else ""})')
    st, hd, body = get(base, '/art?dir=Gamma%20-%20Converted')
    check(st == 200 and body[:2] == b'\xff\xd8', 'a folder\'s cover.jpg')
    st, hd, body = get(base, '/art?dir=Shelf/Zed%20-%20One')
    check(st == 404, 'no picture where there is none')
    def live(action, **kw):
        req = urllib.request.Request(base + '/api/live', data=json.dumps(dict(action=action, **kw)).encode(),
                                     headers={'Content-Type': 'application/json', 'X-Aurora': '1'})
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return json.loads(r.read())
        except urllib.error.HTTPError as e:
            return json.loads(e.read())
    req = urllib.request.Request(base + '/api/live', data=b'{"action":"skip"}')
    try:
        urllib.request.urlopen(req, timeout=5); refused = False
    except urllib.error.HTTPError as e:
        refused = e.code == 403
    check(refused, 'a change without the X-Aurora header is refused (another web site cannot send it)')
    gid = [u for n, u, _ in stations if n == 'Gamma - Converted'][0].split('/s/')[1]
    j = live('play', what='s:' + gid)
    check([q['title'] for q in j.get('queue', [])] == ['01. Gamma - Closing Time', '02. Gamma - Lossless', '03. Gamma - Flac Song'],
          f'play an album: the queue ({[q["title"] for q in j.get("queue", [])]})')
    j = live('add', what='t:Loose track.mp3')
    check(len(j['queue']) == 4 and j['queue'][3]['path'] == 'Loose track.mp3', 'add a song to the end')
    j = live('next', what='t:Alpha Band/First Album/02 Second.mp3')
    check(j['queue'][1]['path'] == 'Alpha Band/First Album/02 Second.mp3', 'play a song next')
    check(live('play', what='t:../../etc/passwd').get('error') and live('bogus').get('error'), 'nonsense is refused in words')
    j = json.loads(get(base, '/api/now')[2])
    check(j['queue'][0]['path'].endswith('Closing Time.m4a') and not j['listeners'], 'it waits for a listener, holding its place')
    # tune in: the album from its start, titles as it goes; then skip and pause
    import threading
    got = {}
    def tune():
        got['raw'] = raw('127.0.0.1', port, '/live', seconds=10.5)
    th = threading.Thread(target=tune); th.start()
    time.sleep(2)
    j = json.loads(get(base, '/api/now')[2])
    check(len(j['listeners']) == 1 and j['current'] and j['current']['title'] == 'Gamma - Closing Time' and j['current']['length'] > 2.5,
          f'a listener tunes in: the queue plays from its start ({j["current"]})')
    live('skip'); time.sleep(2.6)       # what was sent ahead (LIVE_LEAD, 2 s) is heard first
    j = json.loads(get(base, '/api/now')[2])
    check(j['pos'] == 1 and j['current']['title'] == 'Alpha Band - Second Song', f'skip ({j["current"]["title"]})')
    time.sleep(1.5); live('pause'); th.join(); time.sleep(1.5)
    out = subprocess.run([pc, 'icy', os.path.join(tmp, 'live.mp3')], input=got['raw'], capture_output=True).stdout.decode('mac_roman')
    titles = [l[6:] for l in out.splitlines() if l.startswith('title ')]
    check('Gamma - Closing Time' in titles and 'Alpha Band - Second Song' in titles and titles[-1] == 'Test Music: paused',
          f'Aurora68 Play hears it: the titles as they came ({titles})')
    dec = subprocess.run(['ffmpeg', '-v', 'error', '-i', os.path.join(tmp, 'live.mp3'), '-f', 'null', '-'], capture_output=True, text=True)
    check(dec.returncode == 0 and not dec.stderr.strip(), f'songs and silence decode cleanly ({dec.stderr.strip()[:120]!r})')
    j = json.loads(get(base, '/api/now')[2])
    check(not j['listeners'] and j['paused'], 'the listener left; the channel is paused')
    live('clear')

    # the macOS app's JSON: the whole catalogue at once, then an album's tracks with their files
    c = json.loads(get(base, '/api/catalog')[2])
    names = [a['name'] for a in c['artists']]
    zed = next((a for a in c['artists'] if a['name'] == 'Zed'), None)
    check(c['name'] == 'Test Music' and 'Zed' in names and 'Gamma' in names and zed and len(zed['albums']) == 2,
          f'/api/catalog: artists with their albums ({names})')
    gam = next(a for a in c['albums'] if a['artist'] == 'Gamma')
    check(gam['folder'] == 'Gamma - Converted' and gam['tracks'] == 3 and gam['title'] == 'Converted',
          f'and albums: title, artist, folder, track count ({gam})')
    tr = json.loads(get(base, '/api/tracks?album=' + gam['id'])[2])['tracks']
    check([t['title'] for t in tr] == ['Closing Time', 'Lossless', 'Flac Song'] and tr[0]['seconds'] == 3 and
          tr[0]['path'] == 'Gamma - Converted/01. Gamma - Closing Time.m4a',
          f'/api/tracks: in order, titles and lengths from the tags, the file to play ({[(t["title"], t["seconds"]) for t in tr]})')
    st, hd, body = get(base, '/file/' + urllib.parse.quote(tr[0]['path']), {'Range': 'bytes=0-7'})
    check(st == 206 and body[4:8] == b'ftyp', 'and /file/ gives that file as it is, with Range (what AVPlayer asks for)')
    zt = json.loads(get(base, '/api/tracks?artist=' + zed['id'])[2])['tracks']
    check(len(zt) == 2 and get(base, '/api/tracks?album=nothing')[0] == 404, 'an artist\'s tracks; a wrong id is refused')

    # which address the Mac is given: as asked, or this computer's IPv4 or IPv6 address by preference
    import re
    a = json.loads(get(base, '/api/addresses')[2])
    check(a['prefer'] == 'asked' and a['urls'][0]['url'] == base, f'by default the address as asked ({a["urls"]})')
    v4 = get(base, '/stations.txt?prefer=ipv4')[2]
    m = re.search(rb'Live = http://(\[[0-9a-f:]+\]|[0-9.]+):(\d+)/live', v4)
    check(m and m.group(2) == str(port).encode() and m.group(1) != b'127.0.0.1',
          f'?prefer=ipv4: the stations file names this computer by address ({m.group(0).decode() if m else v4[:200]})')
    v6 = get(base, '/stations.txt?prefer=ipv6')[2]
    m6 = re.search(rb'Live = http://(\[[0-9a-f:]+\]|[0-9.]+):\d+/live', v6)
    check(m6 is not None, f'?prefer=ipv6: its IPv6 address, or IPv4 where it has none ({m6.group(1).decode() if m6 else "?"})')

    # Aurora68 Play's Library: /mac/ answers, as the core's proto.c reads them (play/core; mac/piplay/libparse.c before)
    def mac(path):
        st, _, body = get(base, path)
        lines = body.split(b'\r')
        head = lines[0].split(b'\t')
        rows = [[f.decode('mac_roman') for f in l.split(b'\t')] for l in lines[1:] if l]
        return st, head, rows, body
    st, head, rows, _ = mac('/mac/info')
    check(st == 200 and head[:3] == [b'A68M', b'1', b'info'] and rows[0][0] == 'Test Music' and int(rows[0][4]) > 10,
          f'/mac/info: the name and the counts ({rows[:1]})')
    st, head, artists, body = mac('/mac/artists')
    names = [r[1] for r in artists]
    byname = {r[1]: r for r in artists}
    check(head[2] == b'artists' and int(head[3]) == len(artists) and names.index('The Apes') < names.index('Kite') < names.index('Moss'),
          f'/mac/artists: every artist, "The Apes" filed under A ({names})')
    check(byname.get('Zed', [0, 0, 0])[2] == '2' and byname.get('Alpha Band', [0, 0, 0])[2] == '2' and 'Gamma' in byname,
          'artists from "Artist - Album" folders, and from the folder albums are in')
    st, head, part, _ = mac('/mac/artists?from=2&count=3')
    check(head[4:6] == [b'2', b'3'] and [r[1] for r in part] == names[2:5], 'a page: from 2, three of them')
    st, head, albums, body = mac('/mac/albums?artist=' + byname['Alpha Band'][0])
    check(sorted(r[1] for r in albums) == ['Café Sessions', 'First Album'] and b'Caf\x8e Sessions' in body,
          f'an artist\'s albums, in Mac Roman ({[r[1] for r in albums]})')
    st, head, galbums, _ = mac('/mac/albums?artist=' + byname['Gamma'][0])
    gid = galbums[0][0]
    st, head, tracks, _ = mac('/mac/tracks?album=' + gid)
    check([(r[1], r[2], r[4]) for r in tracks] == [('1', 'Closing Time', '3'), ('2', 'Lossless', '2'), ('3', 'Flac Song', '2')]
          and all(r[5] == gid for r in tracks), f'an album\'s tracks: number, title from the tag, length ({[r[1:5] for r in tracks]})')
    st, head, found, _ = mac('/mac/search?q=cr%8Fme')                  # "crème", as the Mac sends it: Mac Roman bytes
    check(any(r[0] == 'T' and r[2].startswith('Crème') for r in found),        # a search reads names, not tags: no file is opened
          f'a search in Mac Roman finds the track ({found[:2]})')
    st, head, found, _ = mac('/mac/search?q=zed')
    check([r[0] for r in found][:3] == ['A', 'L', 'L'], 'a search: the artist first, then the albums')
    st, head, err, _ = mac('/mac/tracks?album=00000000')
    check(st == 404 and head[2] == b'error' and err and 'no such album' in err[0][0], 'an album that is not there: an error, in words')
    st, _, art = get(base, f'/mac/art?album={gid}&w=64&h=64&depth=1')
    hl = art.index(b'\r') + 1
    check(st == 200 and art[:hl] == b'A68ART\t1\t64\t64\t1\t8\r' and len(art) - hl == 8 * 64,
          f'a cover at one bit: 64 x 64, 8 bytes a row ({art[:hl]!r}, {len(art) - hl} bytes)')
    st, _, art8 = get(base, f'/mac/art?album={gid}&w=64&h=64&depth=8')
    bits = art8[art8.index(b'\r') + 1:]
    check(st == 200 and len(bits) == 64 * 64 and max(set(bits), key=bits.count) == 35,
          f'at eight bits: the red cover is the System palette\'s red, index 35 ({max(set(bits), key=bits.count)})')
    zid = mac('/mac/albums?artist=' + byname['Zed'][0])[2][0][0]
    check(get(base, f'/mac/art?album={zid}')[0] == 404, 'an album without a picture: 404')
    st, _, page = get(base, f'/mac/arts?albums={gid},{zid}&w=32&h=32')
    hl = page.index(b'\r') + 1
    check(page[:hl] == b'A68ARTS\t1\t2\t32\t32\t1\t4\r' and len(page) - hl == 2 * 129 and page[hl] == 1 and page[hl + 129] == 0,
          'a page of thumbnails: a byte for each saying whether it has one')
    tone = os.path.join(tmp, 'track.mp3')
    for start, want in ((0, 3.0), (2, 1.0)):
        answer = raw('127.0.0.1', port, f'/t/{tracks[0][0]}' + (f'?start={start}' if start else ''))
        out = subprocess.run([pc, 'icy', tone], input=answer, capture_output=True).stdout.decode('mac_roman')
        pcm = subprocess.run(['ffmpeg', '-v', 'error', '-i', tone, '-f', 's16le', '-ac', '1', '-ar', '8000', '-'], capture_output=True).stdout
        check('status 200 kind 1' in out and abs(len(pcm) / 16000 - want) < 0.4,
              f'one track from {start} s: an MP3 stream with an end, {len(pcm) / 16000:.2f} s of {want:.0f} ({out.splitlines()[-1] if out else ""})')
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.settimeout(5)
    u.sendto(b'AURORA68-MUSIC?', ('127.0.0.1', dport))
    try:
        f = u.recvfrom(512)[0].rstrip(b'\r').split(b'\t')
    except socket.timeout:
        f = []
    check(len(f) == 6 and f[:4] == [b'AURORA68-MUSIC', b'1', b'Test Music', str(port).encode()],
          f'the probe\'s answer: the name, the port, and the IPv4 and IPv6 addresses ({f})')

    # the same interface for the macOS Play (22 November 2026): UTF-8 (enc=utf8), the longer records (full=1)
    def mac8(path):
        st, h, body = get(base, path, {'X-Aurora': '1'})
        lines = body.split(b'\r')
        return st, h, [[x.decode('utf-8') for x in l.split(b'\t')] for l in lines if l], body
    st, h, rows, body = mac8('/mac/albums?artist=' + byname['Alpha Band'][0] + '&enc=utf8&full=1')
    cafe = [r for r in rows[1:] if r[1] == 'Café Sessions']
    check(st == 200 and 'utf-8' in h.get('Content-Type', '') and 'Café Sessions'.encode() in body and cafe and
          cafe[0][5] == byname['Alpha Band'][0] and cafe[0][6] == 'Alpha Band/Café Sessions',
          f'enc=utf8&full=1: an album in UTF-8, with its artist\'s id and its folder ({cafe})')
    st, _, rows, _ = mac8(f'/mac/tracks?album={gid}&enc=utf8&full=1')
    check([(r[6], r[7], r[8]) for r in rows[1:]][:1] == [('Gamma - Converted/01. Gamma - Closing Time.m4a', 'Converted', 'Gamma - Converted')],
          f'full=1: a track\'s path (for /file/), its album\'s title and folder ({rows[1:2]})')
    check(len(mac('/mac/tracks?album=' + gid)[2][0]) == 6, 'without full=1 a track has its six fields, as the classic Play reads them')
    st, _, rows, _ = mac8('/mac/info?enc=utf8&full=1')
    check(rows[1][6] == '20' and len(mac('/mac/info')[2][0]) == 6, f'full=1: /mac/info says how often to announce ({rows[1]})')
    st, _, rows, _ = mac8('/mac/search?q=cr%C3%A8me&enc=utf8')
    check(any(r[0] == 'T' and r[2].startswith('Crème') for r in rows[1:]), 'enc=utf8: the words of a search read as UTF-8')
    st, _, rows, _ = mac8('/mac/albums?count=500&enc=utf8&full=1')
    lr = [r for r in rows[1:] if r[6].startswith('Beta/Talk/Deeper/A very')]
    check(lr and lr[0][6] == LONG_FOLDER and len(lr[0][6]) > 120,
          f'a folder longer than 120 characters comes whole ({len(lr[0][6]) if lr else 0} characters)')
    st, _, rows, _ = mac8('/mac/now?enc=utf8')
    check(st == 200 and rows[0][:3] == ['A68M', '1', 'now'] and rows[1][0] == 'N' and rows[1][1] == 'Test Music',
          f'/mac/now: the Live channel in lines ({rows[:2]})')
    check(get(base, '/mac/command?to=live&action=play&what=f%3AGamma%20-%20Converted')[0] == 403,
          '/mac/command without X-Aurora is refused')
    st, _, rows, _ = mac8('/mac/command?to=live&action=play&what=f%3AGamma%20-%20Converted&enc=utf8')
    st2, _, now, _ = mac8('/mac/now?enc=utf8')
    q = [r[1] for r in now if r[0] == 'Q']
    check(st == 200 and rows[0][0] == 'ok' and q[0] == 'Gamma - Converted/01. Gamma - Closing Time.m4a' and len(q) == 3,
          f'/mac/command to live: play an album; /mac/now has its queue ({q})')
    mac8('/mac/command?to=live&action=repeat&on=1&enc=utf8')
    mac8('/mac/command?to=live&action=remove&index=2&enc=utf8')
    st2, _, now, _ = mac8('/mac/now?enc=utf8')
    check(now[1][4] == '1' and len([r for r in now if r[0] == 'Q']) == 2, 'the Live queue\'s own actions: repeat on, remove')
    st, _, rows, _ = mac8('/mac/command?to=live&action=seek&position=3&enc=utf8')
    check(st == 400 and 'seek' in rows[0][0], f'Live does not seek: refused in words ({rows})')
    st, _, rows, _ = mac8('/mac/command?to=nobody&action=pause')
    check(st == 404 and rows[0][0] == 'no such device', 'a command to a device that is not there')
    mac8('/mac/command?to=live&action=clear')
    mac8('/mac/command?to=live&action=repeat&on=0')

    # getting albums: an indexer and SABnzbd, both stand-ins here; the keys must stay on the server
    check(json.loads(get(base, '/api/get/status')[2])['enabled'] is False, 'finding albums is off without keys')
    from http.server import BaseHTTPRequestHandler as BH, ThreadingHTTPServer as TS
    import threading as th2
    seen = {'sab': []}
    RSS = """<?xml version="1.0" encoding="UTF-8"?><rss version="2.0" xmlns:newznab="http://www.newznab.com/DTD/2010/feeds/attributes/">
<channel><item><title>Zed - One (2001) [FLAC]</title><pubDate>Mon, 03 Nov 2026 10:00:00 +0000</pubDate>
<enclosure url="http://indexer.example/getnzb/abc.nzb&amp;i=1&amp;r=sekritkey123" length="345000000" type="application/x-nzb"/>
<newznab:attr name="category" value="3040"/><newznab:attr name="size" value="345000000"/><newznab:attr name="grabs" value="17"/></item>
<item><title>Zed - One (2001) MP3 320</title><pubDate>Sat, 01 Nov 2026 10:00:00 +0000</pubDate>
<link>http://indexer.example/getnzb/def.nzb&amp;r=sekritkey123</link><newznab:attr name="size" value="120000000"/></item></channel></rss>"""
    class Fake(BH):
        def log_message(self, *a):
            pass
        def do_GET(self):
            qq = dict(urllib.parse.parse_qsl(urllib.parse.urlsplit(self.path).query))
            if 't' in qq:                                    # the indexer
                body = (b'<error code="100" description="Incorrect user credentials (sekritkey123)"/>' if qq.get('q') == 'badkey'
                        else RSS.encode()) if qq.get('apikey') == 'sekritkey123' else b'<error code="100"/>'
                ctype = 'application/xml'
            else:                                            # SABnzbd
                assert qq.get('apikey') == 'sabkey456' and qq.get('output') == 'json'
                seen['sab'].append(qq)
                body = json.dumps({'addurl': {'status': True, 'nzo_ids': ['SABnzbd_nzo_1']},
                                   'queue': {'queue': {'speed': '4.2 M', 'slots': [{'filename': 'Zed - One (2001) [FLAC]', 'status': 'Downloading',
                                                                                    'percentage': '42', 'timeleft': '0:01:10', 'cat': 'music'},
                                                                                   {'filename': 'Some Film', 'cat': 'movies', 'percentage': '5'}]}},
                                   'history': {'history': {'slots': [{'name': 'Older - Album', 'status': 'Completed', 'category': 'music'}]}}}
                                  [qq.get('mode')]).encode()
                ctype = 'application/json'
            self.send_response(200); self.send_header('Content-Type', ctype); self.send_header('Content-Length', str(len(body)))
            self.end_headers(); self.wfile.write(body)
    fk = TS(('127.0.0.1', 0), Fake); fk.daemon_threads = True
    th2.Thread(target=fk.serve_forever, daemon=True).start()
    fport = fk.server_address[1]
    s3 = socket.socket(); s3.bind(('127.0.0.1', 0)); port3 = s3.getsockname()[1]; s3.close()
    env = dict(os.environ, GET_INDEXER_URL=f'http://127.0.0.1:{fport}', GET_INDEXER_KEY='sekritkey123',
               GET_SAB_URL=f'http://127.0.0.1:{fport}', GET_SAB_KEY='sabkey456')
    srv3 = subprocess.Popen([sys.executable, os.path.join(HERE, 'aurora68_music_server.py'), lib, '--port', str(port3),
                             '--bind', '0.0.0.0'], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(1.5)
    b3 = f'http://127.0.0.1:{port3}'
    check(json.loads(get(b3, '/api/get/status')[2]).get('enabled') is True, 'with the keys set, finding albums is on')
    check(get(b3, '/api/get/search?q=zed')[0] == 403, 'a search without the X-Aurora header is refused')
    st, _, body = get(b3, '/api/get/search?q=zed%20one', {'X-Aurora': '1'})
    res = json.loads(body).get('results', [])
    check(st == 200 and [r['title'] for r in res] == ['Zed - One (2001) [FLAC]', 'Zed - One (2001) MP3 320'] and res[0]['grabs'] == 17
          and res[0]['size'] == 345000000, f'the indexer\'s results ({[(r["title"], r["size"]) for r in res]})')
    check(b'sekritkey' not in body and b'getnzb' not in body, 'the results carry no key and no download link')
    st, _, body = get(b3, '/api/get/search?q=badkey', {'X-Aurora': '1'})
    check(st == 502 and b'Incorrect user credentials' in body and b'sekritkey' not in body, f'an indexer\'s error, in words, its key cut out ({body[:120]})')
    req = urllib.request.Request(b3 + '/api/get/grab', data=json.dumps({'id': res[0]['id']}).encode(),
                                 headers={'X-Aurora': '1', 'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=10) as r:
        sent = json.loads(r.read())
    add = [x for x in seen['sab'] if x.get('mode') == 'addurl']
    check(sent.get('sent') and add and add[0]['name'].endswith('r=sekritkey123') and add[0]['cat'] == 'music'
          and add[0]['nzbname'] == 'Zed - One (2001) [FLAC]', 'Download hands the link to SABnzbd, category music')
    qj = json.loads(get(b3, '/api/get/queue', {'X-Aurora': '1'})[2])
    check([d['name'] for d in qj['downloading']] == ['Zed - One (2001) [FLAC]'] and qj['downloading'][0]['percent'] == 42
          and qj['recent'][0]['name'] == 'Older - Album', 'Downloads: SABnzbd\'s music queue and history, nothing else')
    srv3.terminate(); srv3.wait(10)
    log3 = srv3.stdout.read()
    check('finding albums: on, 127.0.0.1' in log3 and 'sekritkey' not in log3 and 'sabkey' not in log3, 'the log names it, never the keys')
    fk.shutdown()

    # without ffmpeg: MP3s only, and the scan says what it passed over
    devices_check(tmp)
    videos_check(tmp, pc)
    links_check(tmp)

    s2 = socket.socket(); s2.bind(('127.0.0.1', 0)); port2 = s2.getsockname()[1]; s2.close()
    srv2 = subprocess.Popen([sys.executable, os.path.join(HERE, 'aurora68_music_server.py'), lib, '--port', str(port2),
                             '--bind', '0.0.0.0', '--ffmpeg', ''], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(1.5)
    st2 = get(f'http://127.0.0.1:{port2}', '/api/list?dir=Gamma%20-%20Converted')[0]   # no music there now
    srv2.terminate(); srv2.wait(10)
    log2 = srv2.stdout.read()
    check('passed over 6 M4A, 1 FLAC files' in log2 and st2 == 404,
          'without ffmpeg: MP3s only, and the log says what was passed over')

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
