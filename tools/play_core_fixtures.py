#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""tools/play_core_fixtures.py - the real music server's answers for Aurora68 Play's core tests.

    python3 tools/play_core_fixtures.py OUTDIR [play_core_test]

Makes a small library of tones with ffmpeg (never music: the project's rule), starts
tools/music-server/aurora68_music_server.py on it on free ports, and keeps the answers the two Plays get: as
the classic Play asks (Mac Roman, the short records) and as the macOS Play asks (enc=utf8&full=1), the lists
in pages, a search, an error, the radio list, a player announced and reporting, the commands sent to it, the
Live channel after commands, and the UDP probe's answer. Then, given the test program, it runs
`play_core_test OUTDIR`, which reads them with the core (tools/play_core_test.c). Without ffmpeg it says so
and does nothing (exit 0), as tools/codec_test.sh does. `make -C mac check` runs it.

Written on 22 November 2026 (docs/08 item 107).
"""
import os, shutil, socket, subprocess, sys, time, urllib.error, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, 'music-server', 'aurora68_music_server.py')


def ffmpeg(*a):
    subprocess.run(['ffmpeg', '-nostdin', '-hide_banner', '-loglevel', 'error', '-y'] + list(a), check=True)


def make_library(d):
    cbr = ['-c:a', 'libmp3lame', '-b:a', '64k', '-ar', '44100', '-ac', '2', '-id3v2_version', '3']

    def tone(path, secs, freq, meta, extra=cbr):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        m = sum((['-metadata', f'{k}={v}'] for k, v in meta.items()), [])
        ffmpeg('-f', 'lavfi', '-i', f'sine=frequency={freq}:duration={secs}', *extra, *m, path)
    tone(f'{d}/Alpha Band/First Album/01 Opening.mp3', 1, 440, {'title': 'Opening', 'artist': 'Alpha Band'})
    tone(f'{d}/Alpha Band/First Album/02 Second.mp3', 1, 550, {'title': 'Second Song', 'artist': 'Alpha Band'})
    tone(f'{d}/Alpha Band/Café Sessions/01 Crème.mp3', 1, 660, {'title': 'Crème brûlée', 'artist': 'Alpha Band'})
    for album in ['The Apes - First', 'Kite - K', 'Moss - A', 'Zed - One']:
        tone(f'{d}/Shelf/{album}/01 Song.mp3', 1, 500, {'title': album, 'artist': album.split(' - ')[0]})
    g = f'{d}/Gamma - Converted'
    tone(f'{g}/01. Gamma - Closing Time.m4a', 3, 300, {'title': 'Closing Time', 'artist': 'Gamma'}, ['-c:a', 'aac', '-b:a', '64k'])
    tone(f'{g}/02. Gamma - Lossless.m4a', 2, 350, {'title': 'Lossless', 'artist': 'Gamma'}, ['-c:a', 'alac'])
    tone(f'{g}/03. Gamma - Flac Song.flac', 2, 400, {'TITLE': 'Flac Song', 'ARTIST': 'Gamma'}, ['-c:a', 'flac'])


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = os.path.abspath(sys.argv[1])
    if not shutil.which('ffmpeg'):
        print('play_core_fixtures: ffmpeg not installed, the server\'s answers skipped')
        return 0
    lib = os.path.join(out, 'music')
    done = os.path.join(out, 'music.made')          # an earlier run's library is used again: ffmpeg runs once
    if not os.path.exists(done):
        if os.path.isdir(lib):                      # one left half made: moved aside, never deleted
            os.rename(lib, f'{lib}.{int(time.time() * 1000)}.old')
        os.makedirs(lib)
        make_library(lib)
        open(done, 'w').close()
    port, dport = free_port(), free_port(socket.SOCK_DGRAM)
    srv = subprocess.Popen([sys.executable, SERVER, lib, '--port', str(port), '--bind', '127.0.0.1', '--name', 'Core Test',
                            '--discovery', str(dport)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                           env=dict(os.environ, MUSIC_RESCAN='1'))
    base = f'http://127.0.0.1:{port}'
    for _ in range(100):
        try:
            socket.create_connection(('127.0.0.1', port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)

    def get(path, name=None):
        req = urllib.request.Request(base + path, headers={'X-Aurora': '1', 'User-Agent': 'Aurora68-Play-core-test'})
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                body = r.read()
        except urllib.error.HTTPError as e:
            body = e.read()
        if name:
            with open(os.path.join(out, name), 'wb') as f:
                f.write(body)
        return body
    U = 'enc=utf8&full=1'
    try:
        get('/mac/info?' + U, 'info.u8')
        get('/mac/info', 'info.mr')
        get('/mac/artists?count=500&' + U, 'artists.u8')
        for i in range(20):                          # the same list, two at a time
            b = get(f'/mac/artists?count=2&from={2 * i}&' + U, f'artists-p{i}.u8')
            head = b.split(b'\r')[0].split(b'\t')
            if 2 * i + 2 >= int(head[3]):
                break
        get('/mac/albums?count=500&' + U, 'albums.u8')
        artists = [l.split(b'\t') for l in get('/mac/artists').split(b'\r')[1:] if l]
        alpha = [a[0].decode() for a in artists if a[1] == b'Alpha Band'][0]
        gamma = [a[0].decode() for a in artists if a[1] == b'Gamma'][0]
        get(f'/mac/albums?artist={alpha}', 'albums-alpha.mr')
        galbum = [l.split(b'\t')[0].decode() for l in get(f'/mac/albums?artist={gamma}').split(b'\r')[1:] if l][0]
        get(f'/mac/tracks?album={galbum}&' + U, 'tracks.u8')
        get(f'/mac/tracks?album={galbum}&count=200', 'tracks.mr')
        get('/mac/search?q=cr%8Fme&count=200', 'search.mr')
        get('/mac/tracks?album=00000000', 'error.mr')
        get('/mac/radio?' + U, 'radio.u8')
        get('/mac/radio', 'radio.mr')
        # a player of the macOS kind: announced, its state a library track (the server names it), then commands
        me = 'mac-fixture0001'
        name = urllib.parse.quote('Café Mac')
        get(f'/mac/announce?id={me}&name={name}&kind=macos&v=1.1&' + U)
        tracks = [l.split(b'\t') for l in get(f'/mac/tracks?album={galbum}').split(b'\r')[1:] if l]
        get(f'/mac/state?id={me}&p=1&pos=1&len=3&vol=70&t={tracks[0][0].decode()}&' + U)
        get('/mac/devices?' + U, 'devices.u8')
        folder = urllib.parse.quote('f:Gamma - Converted')
        get(f'/mac/command?to={me}&action=play&what={folder}&' + U, 'command-ok.u8')
        get(f'/mac/command?to={me}&action=volume&volume=40&' + U)
        get(f'/mac/commands?id={me}&wait=0&' + U, 'commands.u8')
        get(f'/mac/command?to={me}&action=play&what={folder}&' + U)
        get(f'/mac/commands?id={me}&wait=0', 'commands.mr')
        # the Live channel, driven by /mac/command as the macOS Play drives it
        get(f'/mac/command?to=live&action=play&what={folder}&' + U)
        get('/mac/command?to=live&action=repeat&on=1&' + U)
        get('/mac/command?to=live&action=play&what=r%3A0&' + U, 'command-refused.u8')
        get('/mac/now?' + U, 'now.u8')
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.settimeout(5)
        u.sendto(b'AURORA68-MUSIC?', ('127.0.0.1', dport))
        with open(os.path.join(out, 'probe.bin'), 'wb') as f:
            f.write(u.recvfrom(600)[0])
    finally:
        srv.terminate()
        srv.wait(10)
    if len(sys.argv) > 2:
        return subprocess.run([sys.argv[2], out]).returncode
    return 0


if __name__ == '__main__':
    sys.exit(main())
