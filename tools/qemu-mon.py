#!/usr/bin/env python3
"""Send monitor commands to a running QEMU (-monitor unix:/tmp/qemu-mon.sock,server,nowait)."""
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(sys.argv[1] if len(sys.argv) > 2 else '/tmp/qemu-mon.sock'); s.settimeout(2)
def rx():
    out = b''
    while True:
        try: d = s.recv(65536)
        except socket.timeout: break
        if not d: break
        out += d
    return out.decode(errors='replace')
rx()
for cmd in sys.argv[-1].split(';'):
    s.sendall((cmd.strip() + '\n').encode()); time.sleep(0.5); print(rx())
