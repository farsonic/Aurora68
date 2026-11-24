#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""hfsgrow.py - a larger copy of an HFS disk image ("Mac OS Standard"), with everything on it.

  hfsgrow.py OLD.image NEW.image --size 1G     a new image of 1 GB holding all of OLD's files and folders
  hfsgrow.py OLD.image NEW.image --size 500M --bare
                                               the same as a bare HFS volume (no partition map, no driver)
  hfsgrow.py --info IMAGE                      the volume: its name, size, block size, free space, files
  hfsgrow.py --verify OLD.image NEW.image      every file and folder of OLD on NEW, both forks byte for byte

Written on 24 November 2026 for the owner's startup disk, a 95 MB image that was full. Python 3's standard
library only, like the rest of tools/ (no hfsutils needed); runs on macOS, Linux and Windows. OLD is only
read. NEW is made from nothing, not by growing OLD in place:
  - the boot blocks (the first two sectors of the volume) as they were, so NEW starts the Mac as OLD did;
  - every folder and file under the same folder IDs and file numbers, names, types and creators, Finder flags,
    icon positions, labels, dates, both forks (the data fork and the resource fork) byte for byte;
  - the volume's name, dates, attributes and Finder information, which names the blessed System Folder (the
    folder the Mac starts from: it is a folder ID, and the IDs are the same), and the next free ID;
  - the catalog B-tree's records in their order (the order is the File Manager's sort of the names, which
    therefore need no sorting here), packed into new nodes, with room to grow; an empty extents file;
  - each fork in one piece, so nothing is in the extents overflow file.
  The allocation block is the smallest multiple of 512 bytes that keeps the volume within HFS's 65,535 blocks
  (16 KB for 1 GB): a file takes at least one block, so many small files take more room than on OLD.
An image with an Apple partition map (HD SC Setup's, or the Iomega driver's on the images made for Aurora68
before 22 November 2026) keeps it: the partitions before the HFS partition as they were, the HFS partition
grown, the partitions after it moved to the new end. --bare writes the HFS volume alone (enough for a disk
image on the SD card, which PiDisk serves either way; a SCSI disk or a BlueSCSI needs the partition map and
its driver).

Size: System 7.1 takes HFS volumes up to 2 GB, 7.5 and later up to 4 GB (--size up to 2G unless --big).
A FAT32 SD card takes files up to 4 GB less a byte. The new file is written sparse where it is zero: the
free space costs nothing on the computer's disk until the Mac uses it (copying it onto the SD card writes it
all, of course).
"""
import argparse, hashlib, os, struct, sys

SECTOR = 512
NODE = 512

class HFSError(Exception):
    pass

# ---- the image: a bare HFS volume, or the Apple_HFS partition of a partitioned disk ----------------------------
class Image:
    def __init__(self, path, mode='rb'):
        self.path = path
        self.f = open(path, mode)
        self.f.seek(0, 2)
        self.size = self.f.tell()
        self.apm = None                      # (block 0, [(index, start, count, type, entry bytes)])
        self.vol_off = 0
        b0 = self.read(0, SECTOR * 2)
        if b0[:2] == b'ER' and b0[512:514] == b'PM':
            n = struct.unpack('>I', b0[516:520])[0]
            ents = []
            raw = self.read(512, 512 * n)
            for i in range(n):
                e = raw[512 * i:512 * i + 512]
                if e[:2] != b'PM':
                    break
                st, ct = struct.unpack('>II', e[8:16])
                typ = e[48:80].split(b'\0')[0].decode('latin-1')
                ents.append((i + 1, st, ct, typ, e))
            self.apm = (b0[:512], ents)
            hfs = [x for x in ents if x[3] == 'Apple_HFS']
            if not hfs:
                raise HFSError('%s: a partition map without an Apple_HFS partition' % path)
            self.part = hfs[0]
            self.vol_off = hfs[0][1] * SECTOR
            self.vol_size = hfs[0][2] * SECTOR
        else:
            self.part = None
            self.vol_size = self.size

    def read(self, off, n):
        self.f.seek(off)
        d = self.f.read(n)
        if len(d) < n:
            d += b'\0' * (n - len(d))
        return d

    def vread(self, off, n):
        return self.read(self.vol_off + off, n)

# ---- reading a volume ------------------------------------------------------------------------------------------
MDB_FMT = '>2sIIHHHHHIIHIH28sIHIIIHII32sHHHI12sI12s'
MDB_NAMES = ('sig crdate lsmod atrb nmfls vbmst allocptr nmalblks alblksiz clpsiz alblst nxtcnid freebks vn '
             'volbkup vseqnum wrcnt xtclpsiz ctclpsiz nmrtdirs filcnt dircnt fndrinfo vcsize vbmcsize ctlcsize '
             'xtflsize xtextrec ctflsize ctextrec').split()

def ext3(b):
    return [struct.unpack('>HH', b[i:i + 4]) for i in (0, 4, 8)]

def put_ext3(exts):
    exts = list(exts) + [(0, 0)] * 3
    return b''.join(struct.pack('>HH', s, c) for s, c in exts[:3])

class Volume:
    def __init__(self, img):
        self.img = img
        raw = img.vread(1024, 162)
        vals = struct.unpack(MDB_FMT, raw)
        self.mdb = dict(zip(MDB_NAMES, vals))
        self.mdb_raw = raw
        if self.mdb['sig'] != b'BD':
            if raw[:2] == b'H+':
                raise HFSError('%s: an HFS Plus volume ("Mac OS Extended"): System 6 and 7 cannot read it' % img.path)
            raise HFSError('%s: no HFS volume (signature %r)' % (img.path, self.mdb['sig']))
        if struct.unpack('>H', raw[124:126])[0] == 0x482B:
            raise HFSError('%s: an HFS wrapper around an HFS Plus volume: not for System 6 or 7' % img.path)
        self.abs = self.mdb['alblksiz']
        self.alst = self.mdb['alblst']
        self.name = self.mdb['vn'][1:1 + self.mdb['vn'][0]]
        self.xt_overflow = {}                # (forktype, fileid) -> [(startblock, [(s, c)...])]
        xt_exts = ext3(self.mdb['xtextrec'])
        self.xt = self._file_bytes_ranges(xt_exts, self.mdb['xtflsize'])
        self._read_extents_tree()
        ct_exts = ext3(self.mdb['ctextrec']) + self._overflow(0, 4, sum(c for _, c in ext3(self.mdb['ctextrec'])))
        self.ct = self._file_bytes_ranges(ct_exts, self.mdb['ctflsize'])

    def block_off(self, b):
        return self.alst * SECTOR + b * self.abs

    def _file_bytes_ranges(self, exts, length):
        """a fork's bytes as (volume offset, length) runs"""
        runs, left = [], length
        for s, c in exts:
            if not c or left <= 0:
                continue
            n = min(left, c * self.abs)
            runs.append((self.block_off(s), n))
            left -= n
        if left > 0:
            raise HFSError('a fork is longer than its extents (%d bytes missing)' % left)
        return runs

    def _runs_read(self, runs, off, n):
        out = bytearray()
        for ro, rl in runs:
            if off >= rl:
                off -= rl
                continue
            k = min(n - len(out), rl - off)
            out += self.img.vread(ro + off, k)
            off = 0
            if len(out) >= n:
                break
        return bytes(out)

    def _node(self, runs, i):
        return self._runs_read(runs, i * NODE, NODE)

    @staticmethod
    def records(node):
        n = struct.unpack('>H', node[10:12])[0]
        offs = [struct.unpack('>H', node[510 - 2 * i:512 - 2 * i])[0] for i in range(n + 1)]
        return [node[offs[i]:offs[i + 1]] for i in range(n)]

    def _leaves(self, runs):
        hdr = self._node(runs, 0)
        if hdr[8] != 1:
            raise HFSError('a B-tree whose node 0 is not a header node')
        depth, root, nrecs, fnode, lnode = struct.unpack('>HIIII', hdr[14:32])
        seen, i = set(), fnode
        while i and i not in seen:
            seen.add(i)
            node = self._node(runs, i)
            if node[8] != 0xff:
                raise HFSError('B-tree node %d in the leaf chain is not a leaf' % i)
            for r in self.records(node):
                yield r
            i = struct.unpack('>I', node[0:4])[0]

    def _read_extents_tree(self):
        if not self.mdb['xtflsize']:
            return
        for r in self._leaves(self.xt):
            klen = r[0]
            ftype, fid, start = struct.unpack('>BIH', r[1:8])
            data = r[1 + klen + ((1 + klen) & 1):]
            self.xt_overflow.setdefault((ftype, fid), []).append((start, ext3(data[:12])))

    def _overflow(self, ftype, fid, have):
        out = []
        for start, exts in sorted(self.xt_overflow.get((ftype, fid), [])):
            if start < have:
                continue
            out += exts
            have += sum(c for _, c in exts)
        return out

    def catalog(self):
        """the catalog's leaf records in order: (raw record, key bytes, data bytes)"""
        for r in self._leaves(self.ct):
            klen = r[0]
            doff = 1 + klen + ((1 + klen) & 1)
            yield r, r[:1 + klen], r[doff:]

    def fork_runs(self, fid, ftype, rec_exts, length):
        exts = rec_exts + self._overflow(ftype, fid, sum(c for _, c in rec_exts))
        return self._file_bytes_ranges(exts, length)

    def fork_chunks(self, runs, chunk=1 << 20):
        for ro, rl in runs:
            o = 0
            while o < rl:
                k = min(chunk, rl - o)
                yield self.img.vread(ro + o, k)
                o += k

# catalog records
CDR_DIR, CDR_FILE, CDR_DTHD, CDR_FTHD = 1, 2, 3, 4

def key_parts(key):
    parid = struct.unpack('>I', key[2:6])[0]
    name = key[7:7 + key[6]]
    return parid, name

def file_forks(data):
    fid = struct.unpack('>I', data[20:24])[0]
    dlen = struct.unpack('>I', data[26:30])[0]
    rlen = struct.unpack('>I', data[36:40])[0]
    return fid, dlen, ext3(data[74:86]), rlen, ext3(data[86:98])

def mac_name(b):
    return b.decode('mac_roman', 'replace')

# ---- writing a volume ------------------------------------------------------------------------------------------
def parse_size(s):
    s = s.strip().upper()
    mult = 1
    for suf, m in (('KB', 1 << 10), ('MB', 1 << 20), ('GB', 1 << 30), ('K', 1 << 10), ('M', 1 << 20), ('G', 1 << 30)):
        if s.endswith(suf):
            s, mult = s[:-len(suf)], m
            break
    v = int(float(s) * mult)
    return v - v % SECTOR

def btree_nodes(leaf_recs, keylen, nnodes_total):
    """the B-tree as node images: [header, map nodes..., leaves..., index nodes..., free...]. leaf_recs are whole
    records (key and data) in key order. nnodes_total is the file's size in nodes."""
    def node(flink, blink, kind, height, recs):
        d = bytearray(NODE)
        struct.pack_into('>IIBBH', d, 0, flink, blink, kind & 0xff, height, len(recs))
        off = 14
        offs = []
        for r in recs:
            offs.append(off)
            d[off:off + len(r)] = r
            off += len(r)
        offs.append(off)
        for i, o in enumerate(offs):
            struct.pack_into('>H', d, 510 - 2 * i, o)
        return d

    def fits(recs):
        return 14 + sum(len(r) for r in recs) + 2 * (len(recs) + 1) <= NODE

    # leaves
    groups, cur = [], []
    for r in leaf_recs:
        if len(r) & 1:
            raise HFSError('a catalog record of odd length')
        if cur and not fits(cur + [r]):
            groups.append(cur)
            cur = []
        cur.append(r)
    if cur:
        groups.append(cur)
    # map nodes: the header's map record holds 2048 nodes' bits, each map node 3936 more (its record is the node less 20 bytes, as Apple lays it out)
    nmap = 0
    while 2048 + 3936 * nmap < nnodes_total:
        nmap += 1
    first_leaf = 1 + nmap
    levels = []                               # levels[0]: the leaves' groups; then each index level
    nodes = {}
    # node numbers: leaves first, then the index levels upwards
    num = first_leaf
    level_nums = []
    lv = groups
    height = 1
    while True:
        nums = list(range(num, num + len(lv)))
        num += len(lv)
        level_nums.append((lv, nums, height))
        if len(lv) <= 1:
            break
        # the next level up: one index record per node of this one, its first key at full length
        idx, cur = [], []
        for g, n in zip(lv, nums):
            k = g[0]
            klen0 = k[0]
            key = bytearray(1 + keylen)
            key[0] = keylen
            key[1:1 + klen0] = k[1:1 + klen0]
            rec = bytes(key) + (b'\0' if (1 + keylen) & 1 else b'') + struct.pack('>I', n)
            if cur and not fits(cur + [rec]):
                idx.append(cur)
                cur = []
            cur.append(rec)
        if cur:
            idx.append(cur)
        lv = idx
        height += 1
    used = num
    if used > nnodes_total:
        raise HFSError('the B-tree needs %d nodes and has %d' % (used, nnodes_total))
    for lv, nums, h in level_nums:
        for j, (g, n) in enumerate(zip(lv, nums)):
            flink = nums[j + 1] if j + 1 < len(nums) else 0
            blink = nums[j - 1] if j else 0
            nodes[n] = node(flink, blink, 0xff if h == 1 else 0, h, g)
    if not groups:
        depth, root, fnode, lnode = 0, 0, 0, 0
        used = 1 + nmap
    else:
        depth = len(level_nums)
        root = level_nums[-1][1][0]
        fnode, lnode = level_nums[0][1][0], level_nums[0][1][-1]
    # the map: header node 0, the map nodes, and everything up to `used`
    bits = bytearray((nnodes_total + 7) // 8)
    for i in range(used):
        bits[i // 8] |= 0x80 >> (i % 8)
    hdr_rec = struct.pack('>HIIIIHHII', depth, root, len(leaf_recs), fnode, lnode, NODE, keylen, nnodes_total,
                          nnodes_total - used) + b'\0' * 76
    hdr = node(1 if nmap else 0, 0, 1, 0, [hdr_rec, b'\0' * 128, bytes(bits[:256]).ljust(256, b'\0')])
    out = [hdr]
    for m in range(nmap):
        part = bytes(bits[256 + 492 * m:256 + 492 * (m + 1)]).ljust(492, b'\0')
        out.append(node(2 + m if m + 1 < nmap else 0, 0, 2, 0, [part]))
    for n in range(first_leaf, used):
        out.append(nodes[n])
    return out, used

def grow(src_path, dst_path, size, bare=False, big=False, quiet=False):
    say = (lambda *a: None) if quiet else (lambda *a: print(*a))
    src = Image(src_path)
    vol = Volume(src)
    if size > (4 << 30) - SECTOR or (size > 2 << 30 and not big):
        raise HFSError('%d MB: more than %s (System 7.1 takes HFS volumes up to 2 GB; --big for up to 4 GB, System 7.5 '
                       'and later)' % (size >> 20, '4 GB' if big else '2 GB'))
    if os.path.exists(dst_path) and os.path.samefile(src_path, dst_path):
        raise HFSError('the new image must be a new file')

    # the new disk's layout
    pre, post = b'', []
    if src.apm and not bare:
        ddm, ents = src.apm
        tail = [x for x in ents if x[0] > src.part[0]]
        tail_blocks = sum(x[2] for x in tail)
        total = size // SECTOR
        hfs_start = src.part[1]
        hfs_count = total - hfs_start - tail_blocks
    else:
        total = size // SECTOR
        hfs_start, hfs_count = 0, total
    if hfs_count * SECTOR <= vol.img.vol_size:
        raise HFSError('%d MB is not larger than the volume now (%d MB)' % (size >> 20, vol.img.vol_size >> 20))
    S = hfs_count                                  # the new volume's sectors
    # allocation blocks: at most 65535; the bitmap after the MDB; two sectors at the end (the alternate MDB)
    k = 1
    while True:
        bm = ((S - 5) // k + 4095) // 4096        # bitmap sectors, at most
        alst = 3 + bm
        nblocks = (S - alst - 2) // k
        if nblocks <= 65535:
            break
        k += 1
    abs_ = k * SECTOR

    # what is on the old volume
    recs = list(vol.catalog())
    nfiles = sum(1 for _, _, d in recs if d[0] == CDR_FILE)
    ndirs = sum(1 for _, _, d in recs if d[0] == CDR_DIR)
    nleaf_bytes = sum(len(r) + 2 for r, _, _ in recs)
    def blocks_for(n):
        return (n + abs_ - 1) // abs_
    # the catalog file: room for growth, at least four times what the records take, and 1/256 of the volume
    cat_bytes = max(4 * (nleaf_bytes * 3 // 2 + 16 * NODE), S * SECTOR // 256)
    cat_bytes = min(cat_bytes, 64 << 20)
    cat_blocks = blocks_for(cat_bytes)
    # The extents file: at most 1024 nodes (each fork is in one piece, so it starts empty). Not 2048: a B-tree of
    # exactly 2048 nodes (all the header node's map record describes, no map node) made the IIci ROM's check of a
    # volume that was not unmounted cleanly fail as soon as the tree held a record, and the Mac no longer started from
    # it (24 November 2026, docs/08 item 142). Nor a catalog of exactly 2048 nodes, for the same reason.
    xt_blocks = blocks_for(min(max(S * SECTOR // 1024, 64 << 10), 1024 * NODE))
    if cat_blocks * abs_ // NODE == 2048:
        cat_blocks += 1
    cat_nodes = cat_blocks * abs_ // NODE
    xt_nodes = xt_blocks * abs_ // NODE
    assert xt_nodes < 2048 and cat_nodes != 2048

    # allocation: the extents file, the catalog, then the forks in catalog order
    nxt = 0
    xt_start = nxt; nxt += xt_blocks
    ct_start = nxt; nxt += cat_blocks
    plan = []                                      # (record index, fork type, source runs, length, new start, blocks)
    new_recs = []
    root_files = root_dirs = 0
    for i, (r, key, data) in enumerate(recs):
        parid, nm = key_parts(key)
        if data[0] == CDR_FILE:
            if parid == 2:
                root_files += 1
            fid, dlen, dext, rlen, rext = file_forks(data)
            d = bytearray(data)
            for ftype, length, exts, eo, so, po in ((0, dlen, dext, 74, 24, 30), (0xff, rlen, rext, 86, 34, 40)):
                nb = blocks_for(length)
                start = nxt if nb else 0
                if nb:
                    plan.append((fid, ftype, vol.fork_runs(fid, ftype, exts, length), length, start, nb))
                    nxt += nb
                struct.pack_into('>H', d, so, start if nb else 0)
                struct.pack_into('>I', d, po, nb * abs_)
                d[eo:eo + 12] = put_ext3([(start, nb)] if nb else [])
            data = bytes(d)
        elif data[0] == CDR_DIR and parid == 2:
            root_dirs += 1
        klen = key[0]
        rec = key + (b'\0' if (1 + klen) & 1 else b'') + data
        new_recs.append(rec)
    if nxt > nblocks:
        raise HFSError('the files take %d MB in %d-byte blocks, more than the new volume\'s %d MB'
                       % (nxt * abs_ >> 20, abs_, nblocks * abs_ >> 20))
    cat_tree, cat_used = btree_nodes(new_recs, 37, cat_nodes)
    xt_tree, _ = btree_nodes([], 7, xt_nodes)

    say('%s: "%s", %d MB, %d-byte blocks, %d files, %d folders' % (src_path, mac_name(vol.name), vol.img.vol_size >> 20,
                                                                    vol.abs, nfiles, ndirs))
    say('%s: %d MB%s, %d blocks of %d bytes; catalog %d KB (%d of %d nodes used), the files %d MB, %d MB free'
        % (dst_path, size >> 20, ' (an Apple partition map, the HFS partition %d MB)' % (S * SECTOR >> 20) if src.apm and not bare else ', bare HFS',
           nblocks, abs_, cat_blocks * abs_ >> 10, cat_used, cat_nodes, (nxt - xt_blocks - cat_blocks) * abs_ >> 20,
           (nblocks - nxt) * abs_ >> 20))

    # the new file
    tmp = dst_path + '.part'
    out = open(tmp, 'wb')
    try:
        out.truncate(size)
        def put(off, b):
            # sparse: the file is zeros already; only what is not zero is written
            z = 0
            step = 65536
            for o in range(0, len(b), step):
                piece = b[o:o + step]
                if piece.count(0) != len(piece):
                    out.seek(off + o)
                    out.write(piece)
                    z += 1
        if src.apm and not bare:
            ddm, ents = src.apm
            d0 = bytearray(ddm)
            struct.pack_into('>I', d0, 4, total)                     # sbBlkCount
            put(0, bytes(d0))
            # the partitions before the HFS one as they were (the map itself is written below)
            for (i, st, ct, t, e) in ents:
                if st < hfs_start and t != 'Apple_partition_map':
                    put(st * SECTOR, src.read(st * SECTOR, ct * SECTOR))
            pos = hfs_start + hfs_count
            new_ents = []
            for (i, st, ct, t, e) in ents:
                e = bytearray(e)
                if i == src.part[0]:
                    struct.pack_into('>I', e, 12, hfs_count)
                    if struct.unpack('>I', e[84:88])[0] == ct:
                        struct.pack_into('>I', e, 84, hfs_count)
                elif i > src.part[0]:
                    put(pos * SECTOR, src.read(st * SECTOR, ct * SECTOR))
                    struct.pack_into('>I', e, 8, pos)
                    pos += ct
                new_ents.append(bytes(e))
            for j, e in enumerate(new_ents):
                put((1 + j) * SECTOR, e)
        voff = hfs_start * SECTOR
        put(voff, vol.img.vread(0, 1024))                        # the boot blocks
        # the bitmap
        bmap = bytearray(bm * SECTOR)
        for b in range(nxt):
            bmap[b // 8] |= 0x80 >> (b % 8)
        put(voff + 3 * SECTOR, bytes(bmap))
        def boff(b):
            return voff + alst * SECTOR + b * abs_
        put(boff(xt_start), b''.join(bytes(n) for n in xt_tree))
        put(boff(ct_start), b''.join(bytes(n) for n in cat_tree))
        copied = 0
        for fid, ftype, runs, length, start, nb in plan:
            o = boff(start)
            for chunk in vol.fork_chunks(runs):
                put(o, chunk)
                o += len(chunk)
                copied += len(chunk)
        # the MDB, and its copy in the volume's last sector but one
        m = dict(vol.mdb)
        m.update(atrb=(m['atrb'] | 0x0100) & ~0x0880, nmfls=root_files, vbmst=3, allocptr=nxt, nmalblks=nblocks,
                 alblksiz=abs_, clpsiz=4 * abs_, alblst=alst, freebks=nblocks - nxt,
                 wrcnt=m['wrcnt'] + 1, xtclpsiz=xt_blocks * abs_, ctclpsiz=cat_blocks * abs_, nmrtdirs=root_dirs,
                 filcnt=nfiles, dircnt=ndirs - 1 if ndirs else 0, xtflsize=xt_blocks * abs_,
                 xtextrec=put_ext3([(xt_start, xt_blocks)]), ctflsize=cat_blocks * abs_,
                 ctextrec=put_ext3([(ct_start, cat_blocks)]))
        mdb = struct.pack(MDB_FMT, *[m[k_] for k_ in MDB_NAMES]).ljust(SECTOR, b'\0')
        put(voff + 2 * SECTOR, mdb)
        put(voff + (S - 2) * SECTOR, mdb)
        out.close()
        os.replace(tmp, dst_path)
    except BaseException:
        out.close()
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise
    say('copied %d forks, %d MB' % (len(plan), copied >> 20))
    return dst_path

# ---- checking ------------------------------------------------------------------------------------------------
def describe(path):
    img = Image(path)
    v = Volume(img)
    m = v.mdb
    recs = list(v.catalog())
    files = sum(1 for _, _, d in recs if d[0] == CDR_FILE)
    dirs = sum(1 for _, _, d in recs if d[0] == CDR_DIR)
    print('%s: %s' % (path, 'Apple partition map, HFS partition at block %d' % img.part[1] if img.part else 'bare HFS'))
    print('  volume "%s", %d MB, %d blocks of %d bytes, %d MB free (%d%%)' % (mac_name(v.name), img.vol_size >> 20, m['nmalblks'],
          v.abs, m['freebks'] * v.abs >> 20, 100 * m['freebks'] // max(1, m['nmalblks'])))
    print('  %d files, %d folders; blessed folder ID %d; catalog %d KB, extents %d KB'
          % (files, dirs, struct.unpack('>I', m['fndrinfo'][:4])[0], m['ctflsize'] >> 10, m['xtflsize'] >> 10))

def tree(vol):
    """{(parent ID, name): (kind, record data without allocation fields, forks' SHA-256)}"""
    out = {}
    for r, key, data in vol.catalog():
        parid, nm = key_parts(key)
        if data[0] == CDR_FILE:
            fid, dlen, dext, rlen, rext = file_forks(data)
            hs = []
            for ftype, length, exts in ((0, dlen, dext), (0xff, rlen, rext)):
                h = hashlib.sha256()
                for c in vol.fork_chunks(vol.fork_runs(fid, ftype, exts, length)):
                    h.update(c)
                hs.append(h.hexdigest())
            d = bytearray(data)
            for o, n in ((24, 2), (30, 4), (34, 2), (40, 4), (74, 24)):
                d[o:o + n] = b'\0' * n
            out[(parid, nm)] = ('file', bytes(d), tuple(hs))
        else:
            out[(parid, nm)] = ('dir' if data[0] == CDR_DIR else 'thread', bytes(data), ())
    return out

def verify(old, new):
    a, b = Volume(Image(old)), Volume(Image(new))
    ta, tb = tree(a), tree(b)
    bad = 0
    for k, v in ta.items():
        if k not in tb:
            print('missing on %s: parent %d "%s"' % (new, k[0], mac_name(k[1])))
            bad += 1
        elif tb[k] != v:
            print('different on %s: parent %d "%s" (%s)' % (new, k[0], mac_name(k[1]), v[0]))
            bad += 1
    for k in tb:
        if k not in ta:
            print('only on %s: parent %d "%s"' % (new, k[0], mac_name(k[1])))
            bad += 1
    if a.img.vread(0, 1024) != b.img.vread(0, 1024):
        print('the boot blocks differ')
        bad += 1
    for f in ('crdate', 'fndrinfo', 'nxtcnid', 'filcnt', 'dircnt', 'nmfls', 'nmrtdirs'):
        if a.mdb[f] != b.mdb[f]:
            print('the volume\'s %s differs: %r and %r' % (f, a.mdb[f], b.mdb[f]))
            bad += 1
    n = sum(1 for v in ta.values() if v[0] == 'file')
    print('%s: %d files and %d folders compared with %s: %s' % (new, n, sum(1 for v in ta.values() if v[0] == 'dir'), old,
                                                               'all the same' if not bad else '%d differences' % bad))
    return bad == 0

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0], formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog=__doc__[__doc__.index('\n\n'):])
    ap.add_argument('old', nargs='?')
    ap.add_argument('new', nargs='?')
    ap.add_argument('--size', help='the new image\'s size: 500M, 1G, 1500M ...')
    ap.add_argument('--bare', action='store_true', help='the HFS volume alone, without the partition map and driver')
    ap.add_argument('--big', action='store_true', help='allow up to 4 GB (System 7.5 and later)')
    ap.add_argument('--info', metavar='IMAGE', help='describe an image')
    ap.add_argument('--verify', action='store_true', help='compare OLD and NEW, file by file')
    ap.add_argument('--no-verify', action='store_true', help='do not check NEW after making it')
    a = ap.parse_args()
    try:
        if a.info:
            describe(a.info)
            return 0
        if not a.old or not a.new:
            ap.error('OLD and NEW images are needed')
        if a.verify:
            return 0 if verify(a.old, a.new) else 1
        if not a.size:
            ap.error('--size is needed (500M, 1G ...)')
        grow(a.old, a.new, parse_size(a.size), a.bare, a.big)
        if not a.no_verify and not verify(a.old, a.new):
            return 1
        describe(a.new)
        return 0
    except HFSError as e:
        print('hfsgrow: %s' % e, file=sys.stderr)
        return 2

if __name__ == '__main__':
    sys.exit(main())
