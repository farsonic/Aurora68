#!/usr/bin/env python3
"""Add (or replace) one resource in a Macintosh file's resource fork, rebuilding the fork.

    rsrcadd.py System.bin TYPE ID data.bin -o System-new.bin [--name NAME] [--attrs HEX]

The file is MacBinary (hcopy -m output). Every resource of the fork is read with its id, name and
attributes and written back through rsrc.py's writer, with the new one added or put in place of
the one with the same type and id. Written to give the System file a 'scrn' resource, the
Monitors control panel's screen configuration (docs/10), without a Macintosh to run Monitors on:
QuickDraw uses only the startup screen unless that resource names the others. The System file's
resources keep their attributes byte for byte (several are compressed, attribute bit 0, and the
Resource Manager decompresses them as it loads; the bytes themselves are not touched here).
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
from rsrc import ResourceFork, _crc16_xmodem

ap = argparse.ArgumentParser()
ap.add_argument('file'); ap.add_argument('rtype'); ap.add_argument('rid', type=int); ap.add_argument('data')
ap.add_argument('-o', required=True); ap.add_argument('--name'); ap.add_argument('--attrs', default='0')
a = ap.parse_args()

blob = bytearray(open(a.file, 'rb').read())
dlen, rlen = struct.unpack('>II', blob[83:91]); start = 128 + ((dlen + 127) & ~127)
fork = blob[start:start + rlen]
doff, moff, _, mlen = struct.unpack('>IIII', fork[:16]); m = fork[moff:moff + mlen]
map_attrs = struct.unpack('>H', m[22:24])[0]
tloff, nloff, ntypes = struct.unpack('>HHH', m[24:30]); ntypes = (ntypes + 1) & 0xffff
rf = ResourceFork(); replaced = False
new_type = a.rtype.encode('mac_roman'); new_data = open(a.data, 'rb').read()
for i in range(ntypes):
    rtype, cnt, rloff = struct.unpack('>4sHH', m[tloff + 2 + i * 8:tloff + 10 + i * 8])
    for j in range(cnt + 1):
        e = m[tloff + rloff + j * 12:tloff + rloff + j * 12 + 12]
        rid, noff, ao = struct.unpack('>hHI', e[:8]); attrs, off = ao >> 24, ao & 0xffffff
        size = struct.unpack('>I', fork[doff + off:doff + off + 4])[0]
        data = bytes(fork[doff + off + 4:doff + off + 4 + size])
        name = None
        if noff != 0xffff:
            n = m[nloff + noff]; name = m[nloff + noff + 1:nloff + noff + 1 + n].decode('mac_roman')
        if rtype == new_type and rid == a.rid:
            rf.add(rtype, rid, new_data, name=a.name or name, attrs=int(a.attrs, 16)); replaced = True
        else:
            rf.add(rtype, rid, data, name=name, attrs=attrs)
if not replaced:
    rf.add(new_type, a.rid, new_data, name=a.name, attrs=int(a.attrs, 16))
newfork = bytearray(rf.build())
# keep the map's attributes word (e.g. mapCompact/mapChanged bits) as the original had it
nmoff = struct.unpack('>I', newfork[4:8])[0]; newfork[nmoff + 22:nmoff + 24] = struct.pack('>H', map_attrs)
blob[start:start + rlen] = b''
pad = (-len(newfork)) % 128
blob[start:start] = bytes(newfork) + bytes(pad)
blob[87:91] = struct.pack('>I', len(newfork))
blob[124:126] = struct.pack('>H', _crc16_xmodem(bytes(blob[0:124])))   # MacBinary II header CRC
open(a.o, 'wb').write(blob)
print(f"{a.o}: {len(rf.res)} resources, '{a.rtype}' {a.rid} {len(new_data)} bytes {'replaced' if replaced else 'added'}; fork {rlen} -> {len(newfork)} bytes")
