#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""The host's end of "Aurora68 IO Test"'s serial check (mac/iotest/iotest.c, docs/08 item 93).

MAME's Macintosh SE takes a null modem on its modem port whose far side is a TCP socket that MAME
connects to (-modem null_modem -bitb socket.127.0.0.1:PORT). This script listens on that port,
receives what the Mac sends, finds the line "A68SER <speed> BEGIN", the 4096 bytes after it and the
line "A68SER END", checks the bytes against the Mac's pattern, then sends the Mac 4096 bytes of the
host's pattern and prints a summary (also as JSON with --json FILE).

    python3 tools/iotest_host.py --port 6809 [--json result.json] [--timeout 900]

The patterns (both ends): the Mac's byte i is ((i * 37 + 11) ^ (i >> 8)) & 255, the host's
((i * 13 + 5) ^ (i >> 7)) & 255. Standard library only.
"""
import argparse
import json
import socket
import time
import zlib

N = 4096
MAC = bytes(((i * 37 + 11) ^ (i >> 8)) & 255 for i in range(N))
HOST = bytes(((i * 13 + 5) ^ (i >> 7)) & 255 for i in range(N))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=6809)
    ap.add_argument('--timeout', type=float, default=900, help='seconds to wait for the whole exchange')
    ap.add_argument('--json', help='write the summary here as well')
    ap.add_argument('--raw', help='write every byte received here')
    a = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', a.port))
    srv.listen(1)
    srv.settimeout(a.timeout)
    res = {'port': a.port}
    deadline = time.time() + a.timeout
    try:
        conn, _ = srv.accept()
    except socket.timeout:
        res['error'] = 'MAME never connected'
        return finish(a, res)
    print('MAME connected', flush=True)
    conn.settimeout(1.0)
    rx = bytearray()
    t_begin = None
    while time.time() < deadline:
        try:
            chunk = conn.recv(4096)
            if chunk == b'':
                break                    # MAME went away
        except socket.timeout:
            chunk = b''
        if chunk:
            rx += chunk
            if t_begin is None and b'BEGIN\r\n' in rx:
                t_begin = time.time()
        b = rx.find(b'A68SER')
        e = rx.find(b'\r\nA68SER END\r\n', b + 1) if b >= 0 else -1
        if b >= 0 and e >= 0:
            break
    if a.raw:
        open(a.raw, 'wb').write(rx)
    res['received_total'] = len(rx)
    b = rx.find(b'A68SER')
    bl = rx.find(b'BEGIN\r\n', b) if b >= 0 else -1
    e = rx.find(b'\r\nA68SER END\r\n', bl) if bl >= 0 else -1
    if bl < 0 or e < 0:
        res['error'] = 'no complete BEGIN ... END block from the Mac'
        res['head'] = rx[:80].decode('latin-1')
        return finish(a, res)
    res['speed_line'] = rx[b:bl + 5].decode('latin-1')
    data = bytes(rx[bl + 7:e])
    res['mac_to_host_bytes'] = len(data)
    res['mac_to_host_crc32'] = '%08x' % zlib.crc32(data)
    res['mac_to_host_differ'] = sum(1 for i in range(min(len(data), N)) if data[i] != MAC[i]) + abs(len(data) - N)
    res['mac_to_host_ok'] = data == MAC
    if t_begin:
        res['mac_to_host_seconds'] = round(time.time() - t_begin, 2)
    print('from the Mac: %(mac_to_host_bytes)d bytes, CRC-32 %(mac_to_host_crc32)s, %(mac_to_host_differ)d differ' % res, flush=True)
    time.sleep(1.0)                      # the Mac turns to reading once its write has returned
    conn.settimeout(None)
    conn.sendall(HOST)
    res['host_to_mac_bytes'] = N
    res['host_to_mac_crc32'] = '%08x' % zlib.crc32(HOST)
    print('to the Mac: %d bytes, CRC-32 %s' % (N, res['host_to_mac_crc32']), flush=True)
    conn.settimeout(1.0)                 # keep the socket open while MAME passes them on, until MAME goes
    while time.time() < deadline:
        try:
            if conn.recv(4096) == b'':
                break
        except socket.timeout:
            pass
    conn.close()
    return finish(a, res)


def finish(a, res):
    print(json.dumps(res, indent=1), flush=True)
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(res, f, indent=1)
    return 0 if res.get('mac_to_host_ok') else 1


if __name__ == '__main__':
    raise SystemExit(main())
