#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# coff.py FILE [OUTPREFIX]: list an A/UX COFF file's sections; with OUTPREFIX write each as OUTPREFIX.<name>.bin
import sys, struct
d = open(sys.argv[1], 'rb').read()
magic, nscns, timdat, symptr, nsyms, opthdr, flags = struct.unpack('>HHIIIHH', d[:20])
print('magic %04x nscns %d opthdr %d flags %04x symptr %x nsyms %d' % (magic, nscns, opthdr, flags, symptr, nsyms))
if opthdr >= 28:
    m, vs, ts, ds, bs, entry, tstart, dstart = struct.unpack('>HHIIIIII', d[20:48])
    print('aout magic %04x tsize %x dsize %x bsize %x entry %x text_start %x data_start %x' % (m, ts, ds, bs, entry, tstart, dstart))
o = 20 + opthdr
for i in range(nscns):
    name = d[o:o+8].rstrip(b'\0').decode(); paddr, vaddr, size, scnptr, relptr, lnnoptr, nreloc, nlnno, fl = struct.unpack('>IIIIIIHHI', d[o+8:o+40])
    print('%-8s vaddr %08x size %06x fileoff %06x flags %x' % (name, vaddr, size, scnptr, fl))
    if len(sys.argv) > 2 and scnptr:
        open(sys.argv[2] + name + '.bin', 'wb').write(d[scnptr:scnptr+size])
    o += 40
