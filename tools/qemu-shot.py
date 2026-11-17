#!/usr/bin/env python3
"""Ask the running QEMU (monitor on /tmp/qemu-mon.sock) for a screendump, converted to PNG.
   usage: tools/qemu-shot.py out.png   (needs Pillow; falls back to leaving the .ppm)"""
import socket, sys, time, os
out = sys.argv[1]; ppm = out[:-4] + '.ppm' if out.endswith('.png') else out + '.ppm'
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(5)
s.connect(os.environ.get('QEMU_MON', '/tmp/qemu-mon.sock'))
time.sleep(0.3); s.recv(4096)
s.sendall(('screendump %s\n' % os.path.abspath(ppm)).encode()); time.sleep(1.0)
try: s.recv(4096)
except Exception: pass
s.close()
try:
    from PIL import Image
    Image.open(ppm).save(out); os.unlink(ppm); print(out)
except Exception as e:
    print(ppm, "(no PNG: %s)" % e)
