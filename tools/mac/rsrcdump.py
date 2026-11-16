#!/usr/bin/env python3
"""List, and optionally dump, the resources of a Macintosh file taken off an HFS image.

    rsrcdump.py file.bin                 a MacBinary file (hcopy -m output): list its resources
    rsrcdump.py file.rsrc --raw          a bare resource fork (hcopy -r output)
    rsrcdump.py file.bin --dump TYPE ID  hex dump of one resource

Written to look inside MacTCP's control panel and its "MacTCP Prep" file: their configuration
(which card, which address) lives in resources whose layout Apple never documented, and the way
to learn it is to look. The resource fork format is from Inside Macintosh I, "Resource Manager":
header (data offset, map offset, data length, map length), the data area (each resource a
longword length and the bytes), the map (copy of the header, handles, attributes, type list
offset, name list offset, type count minus one, then the type list of {type, count minus one,
reference list offset} and the reference lists of {id, name offset, attributes and 3-byte data
offset, handle}). MacBinary II puts a 128-byte header first and the data fork before the
resource fork, each padded to 128 bytes.
"""
import argparse, struct, sys

ap = argparse.ArgumentParser()
ap.add_argument('file'); ap.add_argument('--raw', action='store_true', help='the file is a bare resource fork')
ap.add_argument('--dump', nargs=2, metavar=('TYPE', 'ID'))
a = ap.parse_args()
blob = open(a.file, 'rb').read()
if not a.raw:
    dlen, rlen = struct.unpack('>II', blob[83:91])
    start = 128 + ((dlen + 127) & ~127)
    fork = blob[start:start + rlen]
    print(f"MacBinary: data fork {dlen} bytes, resource fork {rlen} bytes, type/creator {blob[65:73]!r}")
else:
    fork = blob
doff, moff, dlen, mlen = struct.unpack('>IIII', fork[:16])
m = fork[moff:moff + mlen]
tloff, nloff, ntypes = struct.unpack('>HHH', m[24:30]); ntypes = (ntypes + 1) & 0xffff
tl = m[tloff:]
res = []
for i in range(ntypes):
    rtype, cnt, rloff = struct.unpack('>4sHH', tl[2 + i * 8:10 + i * 8]); cnt += 1
    for j in range(cnt):
        e = m[tloff + rloff + j * 12: tloff + rloff + j * 12 + 12]
        rid, noff, attr_off = struct.unpack('>hHI', e[:8])
        attrs, off = attr_off >> 24, attr_off & 0xffffff
        size = struct.unpack('>I', fork[doff + off:doff + off + 4])[0]
        name = ''
        if noff != 0xffff:
            n = m[nloff + noff]; name = m[nloff + noff + 1:nloff + noff + 1 + n].decode('mac_roman', 'replace')
        res.append((rtype.decode('mac_roman', 'replace'), rid, size, attrs, name, doff + off + 4))
for rtype, rid, size, attrs, name, pos in res:
    print(f"  {rtype!r:8} {rid:6d} {size:7d} bytes  attrs {attrs:02x}  {name}")
if a.dump:
    want_type, want_id = a.dump[0], int(a.dump[1])
    for rtype, rid, size, attrs, name, pos in res:
        if rtype == want_type and rid == want_id:
            data = fork[pos:pos + size]
            for o in range(0, len(data), 16):
                chunk = data[o:o + 16]
                print(f"{o:06x}  {chunk.hex(' '):48}  {''.join(chr(c) if 32 <= c < 127 else '.' for c in chunk)}")
            break
    else:
        sys.exit(f"no {want_type!r} {want_id}")
