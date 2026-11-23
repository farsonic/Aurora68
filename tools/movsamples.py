#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""movsamples.py - the video track of a QuickTime movie as its ImageDescription and its samples, for the
video decoder card's host test (tools/vcodec_test.sh, docs/16-video-card.md).

    python3 tools/movsamples.py movie.mov desc.bin frames.bin

desc.bin is the track's first sample description (a QuickTime ImageDescription, as the Image Compression
Manager hands it to a decompressor); frames.bin is every sample in order, each as a 4-byte big-endian
length and the bytes. Reads the atoms QuickTime 1 to 2.5 wrote (stsd, stsz, stsc, stco/co64); standard
library only. Written on 22 November 2026 (docs/08 item 110).
"""
import struct, sys


def atoms(b, off, end):
    while off + 8 <= end:
        size, kind = struct.unpack('>I4s', b[off:off + 8])
        hdr = 8
        if size == 1:
            size = struct.unpack('>Q', b[off + 8:off + 16])[0]; hdr = 16
        elif size == 0:
            size = end - off
        if size < hdr:
            return
        yield kind, off + hdr, off + size
        off += size


def find(b, off, end, path):
    for kind, s, e in atoms(b, off, end):
        if kind == path[0]:
            if len(path) == 1:
                yield s, e
            else:
                yield from find(b, s, e, path[1:])


def main():
    mov, desc_out, frames_out = sys.argv[1:4]
    b = open(mov, 'rb').read()
    for ms, me in find(b, 0, len(b), [b'moov']):
        for ts, te in find(b, ms, me, [b'trak']):
            hd = [b[s + 8:s + 12] for s, e in find(b, ts, te, [b'mdia', b'hdlr'])]
            if not hd or hd[0] != b'vide':
                continue
            st = {}
            for kind in (b'stsd', b'stsz', b'stsc', b'stco', b'co64'):
                for s, e in find(b, ts, te, [b'mdia', b'minf', b'stbl', kind]):
                    st[kind] = b[s:e]
            sd = st[b'stsd']
            esize = struct.unpack('>I', sd[8:12])[0]
            open(desc_out, 'wb').write(sd[8:8 + esize])
            sz = st[b'stsz']
            fixed, count = struct.unpack('>II', sz[4:12])
            sizes = [fixed] * count if fixed else list(struct.unpack(f'>{count}I', sz[12:12 + 4 * count]))
            if b'stco' in st:
                n = struct.unpack('>I', st[b'stco'][4:8])[0]
                chunks = list(struct.unpack(f'>{n}I', st[b'stco'][8:8 + 4 * n]))
            else:
                n = struct.unpack('>I', st[b'co64'][4:8])[0]
                chunks = list(struct.unpack(f'>{n}Q', st[b'co64'][8:8 + 8 * n]))
            sc = st[b'stsc']
            ns = struct.unpack('>I', sc[4:8])[0]
            runs = [struct.unpack('>III', sc[8 + 12 * i:20 + 12 * i]) for i in range(ns)]
            out, k = bytearray(), 0
            for ci, off in enumerate(chunks):
                per = [r[1] for r in runs if r[0] <= ci + 1][-1]
                for _ in range(per):
                    if k >= count:
                        break
                    out += struct.pack('>I', sizes[k]) + b[off:off + sizes[k]]
                    off += sizes[k]; k += 1
            open(frames_out, 'wb').write(out)
            d = sd[8:8 + esize]
            print(f'{mov}: {d[4:8].decode("latin-1")!r} {struct.unpack(">HH", d[32:36])} depth {struct.unpack(">h", d[82:84])[0]}, {k} samples')
            return
    sys.exit(f'{mov}: no video track')


if __name__ == '__main__':
    main()
