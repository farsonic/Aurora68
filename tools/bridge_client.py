#!/usr/bin/env python3
"""psmac bus-bridge client (docs/05-bridge-protocol.md).

Talks to the bridge68k_device inside the MAME ``maciici_ps`` machine over the
UNIX stream socket (``$PSMAC_BRIDGE_SOCK``, default /tmp/psmac-bridge.sock) and
performs 68k bus cycles on the emulated motherboard.

Library use::

    from bridge_client import BridgeClient
    with BridgeClient() as b:
        b.read8(0x50F00000)            # VIA1 ORB
        b.write8(0x50F01C00, 0x82)     # VIA1 IER: enable CA1
        b.write32(0, 0xFFFFFFFF, posted=True)
        st = b.poll()                  # -> Reply(status, ipl, flags, data)

Command line::

    bridge_client.py read8|read16|read32 ADDR
    bridge_client.py write8|write16|write32 ADDR VALUE [--posted]
    bridge_client.py poll | reset-ack | dump ADDR LEN
    bridge_client.py --selftest        # loopback test against a fake server

All integers on the wire are little-endian.  Request = 12 bytes
(op u8, size u8, flags u8, pad u8, addr u32, data u32); reply = 8 bytes
(status u8, ipl u8, flags u8, pad u8, data u32).
"""
import argparse
import os
import socket
import struct
import sys
import threading
from collections import namedtuple

DEFAULT_SOCK = "/tmp/psmac-bridge.sock"

OP_READ, OP_WRITE, OP_POLL, OP_RESET_ACK = 1, 2, 3, 4
REQ_FLAG_POSTED = 0x01
STATUS_OK, STATUS_BUSERROR = 0, 1
STATUS_BADREQ = 2            # bridge68k extension: malformed request
REPLY_FLAG_RESET, REPLY_FLAG_HALT = 0x01, 0x02

REQ = struct.Struct("<BBBBII")
REPLY = struct.Struct("<BBBBI")

Reply = namedtuple("Reply", "status ipl flags data")


class BusError(Exception):
    def __init__(self, op, size, addr, reply):
        self.op, self.size, self.addr, self.reply = op, size, addr, reply
        super().__init__("bus error on %s%d at %08X (ipl=%d flags=%02X)" %
                         ("read" if op == OP_READ else "write", size * 8, addr,
                          reply.ipl, reply.flags))


class BridgeClient:
    """Synchronous client: one outstanding non-posted request at a time."""

    def __init__(self, path=None, timeout=10.0, raise_on_berr=True):
        self.path = path or os.environ.get("PSMAC_BRIDGE_SOCK") or DEFAULT_SOCK
        self.timeout = timeout
        self.raise_on_berr = raise_on_berr
        self.sock = None
        self.last = Reply(0, 0, 0, 0)   # last reply seen (ipl/flags snapshot)

    # -- connection -------------------------------------------------------
    def connect(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(self.timeout)
        s.connect(self.path)
        self.sock = s
        return self

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def __enter__(self):
        return self.connect()

    def __exit__(self, *exc):
        self.close()

    # -- raw protocol -----------------------------------------------------
    def _send(self, op, size=0, flags=0, addr=0, data=0):
        self.sock.sendall(REQ.pack(op, size, flags, 0, addr & 0xFFFFFFFF, data & 0xFFFFFFFF))

    def _recv_reply(self):
        buf = b""
        while len(buf) < REPLY.size:
            chunk = self.sock.recv(REPLY.size - len(buf))
            if not chunk:
                raise ConnectionError("bridge closed the connection")
            buf += chunk
        status, ipl, flags, _pad, data = REPLY.unpack(buf)
        self.last = Reply(status, ipl, flags, data)
        return self.last

    def request(self, op, size=0, flags=0, addr=0, data=0):
        """Send one request; returns the Reply (None for posted writes)."""
        self._send(op, size, flags, addr, data)
        if op == OP_WRITE and (flags & REQ_FLAG_POSTED):
            return None
        return self._recv_reply()

    # -- bus cycles -------------------------------------------------------
    def read(self, addr, size):
        r = self.request(OP_READ, size, 0, addr)
        if r.status == STATUS_BUSERROR and self.raise_on_berr:
            raise BusError(OP_READ, size, addr, r)
        if r.status == STATUS_BADREQ:
            raise ValueError("bridge rejected request (bad size/op)")
        return r.data & ((1 << (8 * size)) - 1)

    def write(self, addr, size, value, posted=False):
        flags = REQ_FLAG_POSTED if posted else 0
        r = self.request(OP_WRITE, size, flags, addr, value & ((1 << (8 * size)) - 1))
        if r is not None:
            if r.status == STATUS_BUSERROR and self.raise_on_berr:
                raise BusError(OP_WRITE, size, addr, r)
            if r.status == STATUS_BADREQ:
                raise ValueError("bridge rejected request (bad size/op)")
        return r

    def read8(self, addr):  return self.read(addr, 1)
    def read16(self, addr): return self.read(addr, 2)
    def read32(self, addr): return self.read(addr, 4)

    def write8(self, addr, value, posted=False):  return self.write(addr, 1, value, posted)
    def write16(self, addr, value, posted=False): return self.write(addr, 2, value, posted)
    def write32(self, addr, value, posted=False): return self.write(addr, 4, value, posted)

    def poll(self):
        """No bus access; returns Reply with the current ipl and flags."""
        return self.request(OP_POLL)

    def reset_ack(self):
        """Acknowledge a motherboard-initiated reset (clears reply flag bit0)."""
        return self.request(OP_RESET_ACK)

    @property
    def ipl(self):
        return self.last.ipl

    def reset_requested(self):
        return bool(self.last.flags & REPLY_FLAG_RESET)


# ---------------------------------------------------------------------------
# self test: a tiny fake bridge server with 64 KiB of big-endian memory
# ---------------------------------------------------------------------------
class _FakeBridge(threading.Thread):
    def __init__(self, path):
        super().__init__(daemon=True)
        self.path = path
        self.mem = bytearray(0x10000)
        self.ipl = 0
        self.reset_req = True
        self.berr_at = 0xDEAD0000
        self.posted_seen = 0
        self.ls = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        self.ls.bind(path)
        self.ls.listen(1)

    def run(self):
        conn, _ = self.ls.accept()
        with conn:
            buf = b""
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                buf += chunk
                while len(buf) >= REQ.size:
                    req, buf = buf[:REQ.size], buf[REQ.size:]
                    op, size, flags, _pad, addr, data = REQ.unpack(req)
                    status, rdata = STATUS_OK, 0
                    if op in (OP_READ, OP_WRITE):
                        if addr & 0xFFFF0000 == self.berr_at:
                            status = STATUS_BUSERROR
                        elif op == OP_READ:
                            rdata = int.from_bytes(self.mem[addr:addr + size], "big")
                        else:
                            self.mem[addr:addr + size] = data.to_bytes(size, "big")
                            if flags & REQ_FLAG_POSTED:
                                self.posted_seen += 1
                                continue       # no reply
                    elif op == OP_RESET_ACK:
                        self.reset_req = False
                    elif op != OP_POLL:
                        status = STATUS_BADREQ
                    rflags = REPLY_FLAG_RESET if self.reset_req else 0
                    conn.sendall(REPLY.pack(status, self.ipl, rflags, 0, rdata))


def selftest():
    import tempfile
    import time
    path = os.path.join(tempfile.mkdtemp(prefix="psmac-selftest-"), "bridge.sock")
    srv = _FakeBridge(path)
    srv.start()
    with BridgeClient(path) as b:
        r = b.poll()
        assert r.status == STATUS_OK and r.flags & REPLY_FLAG_RESET, r
        assert b.reset_requested()
        b.reset_ack()
        assert not b.poll().flags & REPLY_FLAG_RESET

        b.write32(0x100, 0x11223344)
        assert b.read32(0x100) == 0x11223344
        assert b.read16(0x100) == 0x1122 and b.read16(0x102) == 0x3344, "big-endian lanes"
        assert b.read8(0x103) == 0x44
        b.write8(0x100, 0xAA)
        b.write16(0x102, 0xBBCC)
        assert b.read32(0x100) == 0xAA22BBCC

        for i in range(16):
            b.write8(0x200 + i, i, posted=True)
        time.sleep(0.05)
        assert srv.posted_seen == 16, srv.posted_seen
        assert b.read32(0x200) == 0x00010203, "posted writes landed in order"

        srv.ipl = 5
        assert b.poll().ipl == 5 and b.ipl == 5
        srv.ipl = 0

        try:
            b.read8(0xDEAD0010)
        except BusError as e:
            assert e.reply.status == STATUS_BUSERROR
        else:
            raise AssertionError("expected BusError")
        b.raise_on_berr = False
        b.read8(0xDEAD0010)
        assert b.last.status == STATUS_BUSERROR

        try:
            b.request(9)
            assert b.last.status == STATUS_BADREQ
        except ValueError:
            pass
    print("bridge_client selftest: OK")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--sock", default=None, help="socket path (default $PSMAC_BRIDGE_SOCK or %s)" % DEFAULT_SOCK)
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--posted", action="store_true", help="posted write (no reply)")
    ap.add_argument("cmd", nargs="?", choices=["read8", "read16", "read32", "write8", "write16", "write32", "poll", "reset-ack", "dump"])
    ap.add_argument("args", nargs="*")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    if not a.cmd:
        ap.print_help()
        return 2
    ints = [int(x, 0) for x in a.args]
    with BridgeClient(a.sock) as b:
        if a.cmd.startswith("read"):
            size = int(a.cmd[4:]) // 8
            v = b.read(ints[0], size)
            print("%0*X  (ipl=%d flags=%02X)" % (size * 2, v, b.last.ipl, b.last.flags))
        elif a.cmd.startswith("write"):
            size = int(a.cmd[5:]) // 8
            r = b.write(ints[0], size, ints[1], posted=a.posted)
            if r is not None:
                print("ok  (ipl=%d flags=%02X)" % (r.ipl, r.flags))
        elif a.cmd == "poll":
            r = b.poll()
            print("status=%d ipl=%d flags=%02X" % (r.status, r.ipl, r.flags))
        elif a.cmd == "reset-ack":
            r = b.reset_ack()
            print("status=%d ipl=%d flags=%02X" % (r.status, r.ipl, r.flags))
        elif a.cmd == "dump":
            addr, length = ints[0], ints[1] if len(ints) > 1 else 64
            for off in range(0, length, 16):
                row = [b.read8(addr + off + i) for i in range(min(16, length - off))]
                print("%08X: %s" % (addr + off, " ".join("%02X" % x for x in row)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
