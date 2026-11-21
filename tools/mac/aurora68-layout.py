#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""aurora68-layout.py - the Aurora68 Macintosh programs in their folders, on a PC or on an HFS disk.

    aurora68-layout.py stage OUTDIR [--build mac/build]
        the release's layout as folders of MacBinary files (the release zip's Mac/ folder)
    aurora68-layout.py disk IMAGE [--part N] [--build mac/build]
        the same layout onto an HFS volume that is not a startup disk (Aurora68-Utilities.img)
    aurora68-layout.py install IMAGE [--part N] [--build mac/build] [--music FILE ...]
                       [--remove-old] [--rebuild-desktop] [--startup-log]
        onto a System 7 (or 6) startup disk: the extensions into the System Folder's Extensions
        folder (the System Folder itself on System 6), the folder "Aurora68" at the top of the disk

Written on 21 November 2026, when the programs were renamed "Aurora68 Xxxx" and given folders
(docs/08 item 84, docs/09 "The programs and their folders"). Standard library and hfsutils
(hmount, hmkdir, hcopy, hattrib, hdel, hls, humount). hfsutils keeps the "current volume" in a file
in $HOME (.hcwd), shared by every hfsutils process of that user, so every call here runs with a HOME
of its own, made for the run: two runs side by side, or someone else's hmount, cannot mix up disks.

The layout (the build products of `make -C mac`, their Finder names inside the MacBinary files):

    Aurora68/
      Aurora68 Play/          Aurora68 Play, Music/ (MP3, AIFF and WAV files: --music puts them there,
                              typed and given Aurora68 Play's creator, so a double-click opens them)
      Display/                Aurora68 Display, Aurora68 Screens, Aurora68 Colour, Aurora68 Depth 8
      Network/                Aurora68 Network, Aurora68 Wi-Fi, Aurora68 Bluetooth, Aurora68 Ping
      System/                 Aurora68 Slots, Aurora68 Log, Aurora68 Status
      Extensions (to install)/  Aurora68 Disk, Aurora68 Machine, Aurora68 Screen Setup, Aurora68 Sound,
                              Aurora68 Wi-Fi & Bluetooth, Read Me     (stage and disk only; install puts
                              them into the System Folder)

--remove-old takes away the programs' files under their names before 21 November 2026 (PiPlay,
PiDisk, "Sound PiStorm", "Wi-Fi & Bluetooth"...) from the System Folder, Extensions, Startup Items,
the top of the disk and its Utilities and Apps folders; without it only the old extensions go, because two
copies of an extension both run at startup (two sound devices, two screen arrangers). Preferences
files are left alone: the programs read their old ones. --rebuild-desktop deletes the Finder's
desktop database (Desktop DB, Desktop DF; System 6's Desktop) so that the next start builds it again
with the new icons. --startup-log puts a second copy of Aurora68 Log into Startup Items.
"""
import argparse, os, shutil, subprocess, sys, tempfile

FOLDER = 'Aurora68'
EXT_FOLDER = 'Extensions (to install)'
# (build file, folder inside Aurora68, Finder name); the Finder name is the MacBinary's own
APPS = [
    ('Aurora68-Play.bin', 'Aurora68 Play', 'Aurora68 Play'),
    ('Aurora68-Display.bin', 'Display', 'Aurora68 Display'),
    ('Aurora68-Screens.bin', 'Display', 'Aurora68 Screens'),
    ('Aurora68-Colour.bin', 'Display', 'Aurora68 Colour'),
    ('Aurora68-Depth.bin', 'Display', 'Aurora68 Depth 8'),
    ('Aurora68-Network.bin', 'Network', 'Aurora68 Network'),
    ('Aurora68-WiFi.bin', 'Network', 'Aurora68 Wi-Fi'),
    ('Aurora68-Bluetooth.bin', 'Network', 'Aurora68 Bluetooth'),
    ('Aurora68-Ping.bin', 'Network', 'Aurora68 Ping'),
    ('Aurora68-Slots.bin', 'System', 'Aurora68 Slots'),
    ('Aurora68-Log.bin', 'System', 'Aurora68 Log'),
    ('Aurora68-Status.bin', 'System', 'Aurora68 Status'),
]
EXTENSIONS = [
    ('Aurora68-Disk.bin', 'Aurora68 Disk'),
    ('Aurora68-Machine.bin', 'Aurora68 Machine'),
    ('Aurora68-ScreenSetup.bin', 'Aurora68 Screen Setup'),
    ('Aurora68-Sound.bin', 'Aurora68 Sound'),
    ('Aurora68-WiFi-Bluetooth.bin', 'Aurora68 Wi-Fi & Bluetooth'),
]
OLD_EXTENSIONS = ['PiDisk', 'PiMach', 'PiScrn', 'Sound PiStorm', 'Wi-Fi & Bluetooth']
OLD_APPS = ['PiPlay', 'PiDisplay', 'PiSlots', 'PiLog', 'PiStatus', 'PiNet', 'PiWiFi', 'PiBlue', 'PiPing',
            'PiColor', 'PiScreens', 'PiDepth', 'PiDepth 8']
MUSIC_TYPES = {'.mp3': 'MPG3', '.aif': 'AIFF', '.aiff': 'AIFF', '.aifc': 'AIFC', '.wav': 'WAVE'}

EXT_README = """Aurora68 extensions

Copy these into the System Folder (System 6) or drop them on the closed System Folder (System 7,
which puts them into Extensions), then restart. Take out the old ones first if they are there:
PiDisk, PiMach, PiScrn, Sound PiStorm and Wi-Fi & Bluetooth are the same extensions under their
names before 21 November 2026.

Aurora68 Disk - the Pi's disk images as disks (only needed with a declaration ROM older than the
    kernel's; the card's ROM normally brings the driver).
Aurora68 Machine - About This Macintosh and every program are told "Macintosh SE".
Aurora68 Screen Setup - the SE board's two screens arranged at the first start; asks "Keep this
    resolution?" after Aurora68 Display has changed the HDMI mode.
Aurora68 Sound - the Pi's sound card as the Sound Manager's output (HDMI or the headphone socket).
    Needs Sound Manager 3.0 (the "Sound Manager" extension on System 7.1, built into 7.5).
Aurora68 Wi-Fi & Bluetooth - the Wi-Fi and Bluetooth menus in the menu bar.
"""


class HFS:
    """hfsutils with a HOME of its own"""
    def __init__(self, image, part=None):
        self.home = tempfile.mkdtemp(prefix='aurora68-hfs-')
        self.env = dict(os.environ, HOME=self.home)
        self.run('hmount', image, *([str(part)] if part is not None else []))

    def run(self, *args, check=True):
        r = subprocess.run(args, env=self.env, capture_output=True, text=True, errors='replace')
        if check and r.returncode:
            sys.exit(f"{' '.join(args)}: {r.stderr.strip() or r.stdout.strip()}")
        return r

    def exists(self, path):
        r = self.run('hls', '-d', path, check=False)      # hls answers 0 for a missing file too
        return r.returncode == 0 and not r.stderr.strip() and bool(r.stdout.strip())

    def is_dir(self, path):
        return self.run('hls', '-ld', path, check=False).stdout.startswith('d')

    def mkdir(self, path):
        if not self.exists(path):
            self.run('hmkdir', path)

    def put(self, src, folder):
        self.run('hcopy', '-m', src, folder + ':')

    def delete(self, path):
        self.run('hdel', path)

    def close(self):
        self.run('humount', check=False)
        for f in os.listdir(self.home):                # hfsutils' .hcwd, in the directory made for the run
            os.remove(os.path.join(self.home, f))
        os.rmdir(self.home)


def build_path(build, f):
    p = os.path.join(build, f)
    if not os.path.exists(p):
        sys.exit(f"{p}: not built (make -C mac)")
    return p


def stage(build, out):
    root = os.path.join(out, FOLDER)
    for f, folder, _ in APPS:
        d = os.path.join(root, folder)
        os.makedirs(d, exist_ok=True)
        shutil.copy2(build_path(build, f), d)
    os.makedirs(os.path.join(root, 'Aurora68 Play', 'Music'), exist_ok=True)
    d = os.path.join(root, EXT_FOLDER)
    os.makedirs(d, exist_ok=True)
    for f, _ in EXTENSIONS:
        shutil.copy2(build_path(build, f), d)
    with open(os.path.join(d, 'Read Me.txt'), 'w') as t:
        t.write(EXT_README)
    print(f"{root}: {len(APPS)} programs, {len(EXTENSIONS)} extensions")


def put_apps(h, build, top):
    h.mkdir(top)
    for f, folder, _ in APPS:
        h.mkdir(f'{top}:{folder}')
        h.put(build_path(build, f), f'{top}:{folder}')
    h.mkdir(f'{top}:Aurora68 Play:Music')


def readme(h, folder):
    with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as t:
        t.write(EXT_README)
    h.run('hcopy', '-t', t.name, f'{folder}:Read Me')
    os.unlink(t.name)


def disk(build, image, part):
    h = HFS(image, part)
    try:
        put_apps(h, build, f':{FOLDER}')
        h.mkdir(f':{FOLDER}:{EXT_FOLDER}')
        for f, _ in EXTENSIONS:
            h.put(build_path(build, f), f':{FOLDER}:{EXT_FOLDER}')
        readme(h, f':{FOLDER}:{EXT_FOLDER}')
    finally:
        h.close()
    print(f"{image}: the Aurora68 folder")


def install(build, image, part, music, remove_old, rebuild_desktop, startup_log):
    h = HFS(image, part)
    try:
        sf = ':System Folder'
        if not h.exists(sf):
            sys.exit(f"{image}: no System Folder at the top of the disk")
        ext = f'{sf}:Extensions' if h.exists(f'{sf}:Extensions') else sf
        places = [sf, f'{sf}:Extensions', f'{sf}:Startup Items', '', ':Utilities', ':Apps']
        for name in OLD_EXTENSIONS + (OLD_APPS if remove_old else []):
            for p in places:
                path = f'{p}:{name}'
                if h.exists(path) and h.is_dir(path):     # a folder named after the program (Apps:PiPlay)
                    path = f'{path}:{name}'               # holds it; the folder and the rest stay
                if h.exists(path) and not h.is_dir(path):
                    h.delete(path)
                    print(f"  removed {path}")
        for f, name in EXTENSIONS:
            if h.exists(f'{ext}:{name}'):
                h.delete(f'{ext}:{name}')
            h.put(build_path(build, f), ext)
        top = f':{FOLDER}'
        for f, folder, name in APPS:
            if h.exists(f'{top}:{folder}:{name}'):
                h.delete(f'{top}:{folder}:{name}')
        put_apps(h, build, top)
        for m in music or []:
            t = MUSIC_TYPES.get(os.path.splitext(m)[1].lower())
            name = os.path.basename(m)[:31]
            dest = f'{top}:Aurora68 Play:Music:{name}'
            if h.exists(dest):
                h.delete(dest)
            h.run('hcopy', '-r', m, dest)
            if t:
                h.run('hattrib', '-t', t, '-c', 'PIPY', dest)
        if startup_log and h.exists(f'{sf}:Startup Items'):
            if h.exists(f'{sf}:Startup Items:Aurora68 Log'):
                h.delete(f'{sf}:Startup Items:Aurora68 Log')
            h.put(build_path(build, 'Aurora68-Log.bin'), f'{sf}:Startup Items')
        if rebuild_desktop:
            for d in (':Desktop DB', ':Desktop DF', ':Desktop'):
                if h.exists(d):
                    h.delete(d)
                    print(f"  removed {d} (the Finder builds it again at the next start)")
    finally:
        h.close()
    print(f"{image}: extensions in {ext[1:]}, programs in {FOLDER}" + (f", {len(music)} sound files in Music" if music else ''))


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__.split('\n\n', 1)[1])
    ap.add_argument('mode', choices=['stage', 'disk', 'install'])
    ap.add_argument('target', help='the output folder (stage) or the HFS image')
    ap.add_argument('--build', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'mac', 'build'))
    ap.add_argument('--part', type=int, help='the partition of a partitioned disk (hfsutils numbering)')
    ap.add_argument('--music', nargs='*', default=[], help='sound files for Aurora68 Play:Music (install)')
    ap.add_argument('--remove-old', action='store_true', help='also take away the programs under their old names')
    ap.add_argument('--rebuild-desktop', action='store_true', help="delete the Finder's desktop database")
    ap.add_argument('--startup-log', action='store_true', help='Aurora68 Log into Startup Items')
    a = ap.parse_args()
    if a.mode == 'stage':
        stage(a.build, a.target)
    elif a.mode == 'disk':
        disk(a.build, a.target, a.part)
    else:
        install(a.build, a.target, a.part, a.music, a.remove_old, a.rebuild_desktop, a.startup_log)
