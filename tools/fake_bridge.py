#!/usr/bin/env python3
"""Stand-in for the MAME bridge: UNIX socket server speaking docs/05-bridge-protocol.md.
Logs every request; reads return a constant (default 0x00), POLL returns IPL 0. Useful to validate the
Emu68 side of the transport and the ROM's first accesses without MAME. usage: fake_bridge.py [sock] [logfile]"""
import os, socket, struct, sys, time
sock_path = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('PSMAC_BRIDGE_SOCK', '/tmp/psmac-bridge.sock')
log = open(sys.argv[2] if len(sys.argv) > 2 else 'fake_bridge.log', 'w')
try: os.unlink(sock_path)
except FileNotFoundError: pass
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); srv.bind(sock_path); srv.listen(1)
print("fake bridge listening on", sock_path, flush=True)
n = 0; t0 = time.time()
while True:
    c, _ = srv.accept(); print("client connected", flush=True)
    buf = b''
    try:
        while True:
            d = c.recv(4096)
            if not d: break
            buf += d
            while len(buf) >= 12:
                op, size, flags, pad, addr, data = struct.unpack('<BBBBII', buf[:12]); buf = buf[12:]
                n += 1
                if n <= 400 or n % 10000 == 0:
                    log.write(f"{time.time()-t0:8.3f} #{n} op {op} size {size} flags {flags} addr {addr:08X} data {data:08X}\n"); log.flush()
                if op == 2 and (flags & 1): continue
                rdata = 0x00000000 if op == 1 else 0
                c.sendall(struct.pack('<BBBBI', 0, 0, 0, 0, rdata))
    finally:
        c.close(); print("client disconnected after", n, "requests", flush=True)
