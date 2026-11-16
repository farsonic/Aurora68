#!/usr/bin/env python3
"""Photograph the Pi's VideoCore display in the testbed.

    vcshot.py out.png            one picture now
    vcshot.py out.png --every 10 --count 30     a picture every 10 s, thirty of them (out-NN.png)

QEMU's raspi3b emulates the VideoCore frame buffer that the firmware mailbox hands out, and the
testbed runs it with no display attached; the QEMU monitor's "screendump" still renders it to a
PPM file. This asks the monitor over its socket (tools/run-psmac-qemu.sh opens it at
/tmp/qemu-mon.sock), converts the PPM to PNG without any library, and writes it where asked.
It is how the second display (docs/10, psmac_display.c) is seen before there is a Pi with an
HDMI cable: the MAME screenshots show the SE's own screen, this shows the Pi's.
"""
import argparse, os, socket, struct, sys, time, zlib

ap = argparse.ArgumentParser()
ap.add_argument('out'); ap.add_argument('--every', type=float, default=0); ap.add_argument('--count', type=int, default=1)
ap.add_argument('--sock', default='/tmp/qemu-mon.sock')
a = ap.parse_args()

def png_from_ppm(ppm, out):
    parts = ppm.split(b'\n', 3)
    w, h = map(int, parts[1].split()); px = parts[3]
    raw = b''.join(b'\x00' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
    return w, h

s = socket.socket(socket.AF_UNIX); s.connect(a.sock); time.sleep(0.3); s.recv(4096)
for n in range(a.count):
    tmp = '/tmp/vcshot-%d.ppm' % os.getpid()
    s.send(('screendump %s\n' % tmp).encode()); time.sleep(0.8); s.recv(65536)
    out = a.out if a.count == 1 else a.out.replace('.png', '-%02d.png' % n)
    try:
        w, h = png_from_ppm(open(tmp, 'rb').read(), out); os.unlink(tmp); print(out, w, h)
    except Exception as e:
        print('no picture:', e)
    if n + 1 < a.count: time.sleep(a.every)
