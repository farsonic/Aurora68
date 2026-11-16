#!/usr/bin/env python3
"""Build a NuBus declaration ROM image (format block at the END, as the Slot Manager expects).

Layout written (all big-endian), offsets relative to the start of the image:
    [sResource data blocks ...][sResource lists ...][sResource directory][format block (20 bytes)]
Entries are {id byte, 24-bit signed offset relative to the entry's own address}; lists and the
directory end with id 0xFF. The format block: directory offset (signed 32-bit, relative to the
block), ROM length, CRC, format (1), revision (1), test pattern 0x5A932BC7, reserved 0, byte lanes.
CRC: over the whole image with the CRC field as 0, crc = rol32(crc, 1) + byte  (verified against
the IIci ROM's built-in declaration ROM, tools/declrom.py).

Usage from Python:
    rom = DeclROM()
    board = rom.sresource(1)                      # board sResource (id 1)
    board.type(1, 0, 0, 0); board.name('PiStorm-SE'); board.long(32, 0x1234)   # BoardId
    func = rom.sresource(0x80); ...
    open('psmac.declrom', 'wb').write(rom.build())
"""
import struct

TEST_PATTERN = 0x5A932BC7

class SResource:
    def __init__(self, rid):
        self.rid = rid
        self.entries = []        # (id, kind, payload) kind: 'off' -> payload bytes block, 'word' -> 16-bit value, 'long' -> block holding a long

    def block(self, eid, data):          # entry points at a data block
        self.entries.append((eid, 'off', bytes(data)))
    def word(self, eid, value):          # entry holds a 16-bit value in its offset field (sRsrcFlags, etc.)
        self.entries.append((eid, 'word', value & 0xFFFF))
    def long(self, eid, value):          # entry points at a 4-byte block (BoardId style values are words; use block for exact)
        self.block(eid, struct.pack('>I', value))
    def type(self, cat, ctype, drsw, drhw):   # sRsrcType = 1
        self.block(1, struct.pack('>HHHH', cat, ctype, drsw, drhw))
    def name(self, s):                        # sRsrcName = 2, C string
        self.block(2, s.encode('mac_roman') + b'\0')
    def cstring(self, eid, s):
        self.block(eid, s.encode('mac_roman') + b'\0')
    def sublist(self, eid, sub):              # entry points at another list (e.g. sDriverDir, VendorInfo)
        self.entries.append((eid, 'list', sub))
    def longblock(self, eid, data):           # sBlock: 4-byte length (including the length field) + data
        self.block(eid, struct.pack('>I', len(data) + 4) + bytes(data))
    def sexec(self, eid, code, cpu=2):        # sExecBlock: size, revision 2, CPU id, reserved, code offset (4 = right after), code
        body = struct.pack('>BBHI', 2, cpu, 0, 4) + bytes(code)
        self.block(eid, struct.pack('>I', len(body) + 4) + body)

class DeclROM:
    def __init__(self, byte_lanes=0x0F, revision=1, fmt=1):
        self.sres = []; self.byte_lanes = byte_lanes; self.revision = revision; self.fmt = fmt
    def sresource(self, rid):
        s = SResource(rid); self.sres.append(s); return s

    def _emit_list(self, out, sres):
        """Append data blocks then the list itself; return the list's offset in out."""
        placed = []
        for eid, kind, payload in sres.entries:
            if kind == 'off':
                while len(out) % 4: out.append(0)
                placed.append((eid, 'off', len(out))); out += payload
            elif kind == 'list':
                off = self._emit_list(out, payload); placed.append((eid, 'off', off))
            else:
                placed.append((eid, 'word', payload))
        while len(out) % 4: out.append(0)
        loff = len(out)
        for eid, kind, v in placed:
            here = len(out)
            if kind == 'off':
                rel = v - here
                out += bytes([eid]) + (rel & 0xFFFFFF).to_bytes(3, 'big')
            else:
                out += bytes([eid]) + b'\0' + struct.pack('>H', v)
        out += b'\xff\0\0\0'
        return loff

    def build(self):
        out = bytearray()
        offs = []
        for s in sorted(self.sres, key=lambda s: s.rid):
            offs.append((s.rid, self._emit_list(out, s)))
        while len(out) % 4: out.append(0)
        diroff = len(out)
        for rid, loff in offs:
            here = len(out); rel = loff - here
            out += bytes([rid]) + (rel & 0xFFFFFF).to_bytes(3, 'big')
        out += b'\xff\0\0\0'
        fb = len(out)
        length = fb + 20
        # fhDirOffset: signed 24-bit, self-relative, with the top byte 0 (the Slot Manager rejects a non-zero top byte)
        out += struct.pack('>IIIBBIBB', (diroff - fb) & 0xFFFFFF, length, 0, self.fmt, self.revision, TEST_PATTERN, 0, self.byte_lanes)
        crc = 0
        for b in out:
            crc = ((crc << 1) | (crc >> 31)) & 0xFFFFFFFF
            crc = (crc + b) & 0xFFFFFFFF
        out[fb + 8:fb + 12] = struct.pack('>I', crc)
        return bytes(out)

if __name__ == '__main__':
    import sys
    rom = DeclROM()
    b = rom.sresource(1); b.type(1, 0, 0, 0); b.name('PiStorm-SE test'); b.block(32, struct.pack('>H', 0x5053))
    sys.stdout.buffer.write(rom.build())
