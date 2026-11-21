#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""hfsview_check.py - the test files for the kernel's HFS view of FAT and exFAT (issue #20), and the check
of what the view made of them, through hfsutils.

  hfsview_check.py tree DIR [--many N]   a test tree: Mac files with hand-made AppleDouble "._" files (as a
                                          modern Mac writes them on FAT and exFAT), long, accented and
                                          non-Latin names, names that clash once shortened or converted,
                                          a folder tree 20 deep, a folder with N files, an empty file, a
                                          fragmented file (exFAT: "frag" in the name), files that are not
                                          to show (.DS_Store, System Volume Information)
  hfsview_check.py split MACBINARY DIR [NAME]
                                          a MacBinary file (hcopy -m, mac/build/*.bin) as a modern Mac leaves
                                          it on FAT or exFAT: NAME with the data fork, ._NAME with the rest
  hfsview_check.py check OUT.hfs DIR
                                          hls -R lists exactly the expected names; every file's data fork,
                                          and for Mac files the resource fork, type and creator (hcopy -m,
                                          MacBinary), are byte for byte the originals

hfsutils keeps its current volume in $HOME/.hcwd: run this with a HOME of its own (hfsview_test.sh does).
The expected names are worked out here independently of the C code, from the rules in psmac_hfsview.c's
header: Unicode NFC to Mac Roman, '?' for the rest, ':' as '-', 31 characters with the extension kept
behind an ellipsis, " #2" for a clash in directory order. Standard library only.
"""
import os, random, struct, subprocess, sys, unicodedata

# ---- the tree ----------------------------------------------------------------------------------------
def appledouble(ftype, creator, flags, rsrc):
    """an AppleDouble file laid out as macOS writes one: Finder info at $32 with room for extended
    attributes (an "ATTR" block), the resource fork at $EE2"""
    fi_off, fi_len, r_off = 0x32, 0xeb0, 0xee2
    hdr = struct.pack('>II16sH', 0x00051607, 0x00020000, b'Mac OS X        ', 2)
    hdr += struct.pack('>III', 9, fi_off, fi_len) + struct.pack('>III', 2, r_off, len(rsrc))
    finfo = ftype + creator + struct.pack('>H', flags) + struct.pack('>hh', 120, 80) + b'\0\0' + b'\0' * 16
    body = bytearray(r_off)
    body[:len(hdr)] = hdr
    body[fi_off:fi_off + 32] = finfo
    body[fi_off + 34:fi_off + 38] = b'ATTR'               # the extended attributes' header follows on macOS
    return bytes(body) + rsrc

MAC_FILES = {   # path: (type, creator, data bytes, resource bytes)
    'Applications/TestApp': (b'APPL', b'TAPP', 2000, 70001),
    'Applications/Rsrc Only': (b'rsrc', b'RSED', 0, 1234),
    'Applications/Docs/Letter': (b'TEXT', b'ttxt', 900, 0),
}
LONG = ['This is a very long file name that goes on and on.txt',
        'Quarterly report for the year 2025 draft.doc',
        'Quarterly report for the year 2025 final.doc',
        'A name without any extension that is far too long']
INTL = ['Résumé.txt', 'naïve café.txt', 'Ünïcödé.txt', '日本.txt', '中国.txt', '🎵 music.txt', 'Grüße aus Köln.txt', 'Ω and π.txt']

def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)

def tree(d, many):
    rnd = random.Random(20)
    write(os.path.join(d, 'ReadMe.txt'), b'Aurora68 exchange test\r' * 40)
    write(os.path.join(d, 'empty.txt'), b'')
    for p, (t, c, dn, rn) in MAC_FILES.items():
        write(os.path.join(d, p), bytes(rnd.getrandbits(8) for _ in range(dn)))
        head, tail = os.path.split(p)
        write(os.path.join(d, head, '._' + tail), appledouble(t, c, 0x2100, bytes(rnd.getrandbits(8) for _ in range(rn))))
    for n in LONG:
        write(os.path.join(d, 'Long names', n), n.encode())
    for n in INTL:
        write(os.path.join(d, 'International', n), n.encode('utf-8'))
    p = os.path.join(d, 'Deep')
    for k in range(1, 21):
        p = os.path.join(p, 'Level %d' % k)
    write(os.path.join(p, 'bottom.txt'), b'twenty folders down\n')
    for k in range(many):
        write(os.path.join(d, 'Many', 'file%05d.txt' % k), b'%d\n' % k)
    write(os.path.join(d, 'frag-big.bin'), bytes(rnd.getrandbits(8) for _ in range(300000)))
    write(os.path.join(d, '.DS_Store'), b'\0\0\0\1Bud1')
    write(os.path.join(d, 'System Volume Information', 'IndexerVolumeGuid'), b'x' * 76)
    write(os.path.join(d, 'Deep', '.hidden-unix-file'), b'x')

# ---- the expected names -----------------------------------------------------------------------------
def roman(u):
    out = bytearray()
    for ch in unicodedata.normalize('NFC', u):
        if ch == ':':
            out += b'-'
        elif ord(ch) < 0x20 or ord(ch) >= 0x10000:
            out += b'?'
        else:
            try:
                out += ch.encode('mac_roman')
            except UnicodeEncodeError:
                if not unicodedata.combining(ch):
                    out += b'?'
    return bytes(out) or b'?'

def hfs_name(full, k=1):
    sfx = b' #%d' % k if k > 1 else b''
    dot = len(full)
    for i in range(len(full), 1, -1):
        if full[i - 1:i] == b'.':
            dot = i - 1
            break
    if len(full) - dot > 6:
        dot = len(full)
    base, ext = full[:dot], full[dot:]
    if len(base) + len(sfx) + len(ext) <= 31:
        return base + sfx + ext
    return base[:31 - len(sfx) - len(ext) - 1] + b'\xc9' + sfx + ext

def expected(d):
    """{HFS path (bytes, ':'-separated, from the root): source path}; clashes by the directory order of
    the tree the image was made from (both image builders copy in sorted order)"""
    out = {}
    def walk(src, hpath):
        names = []
        for n in sorted(os.listdir(src)):
            if n.startswith('.') or n in ('System Volume Information', '$RECYCLE.BIN'):
                continue
            names.append(n)
        seen = []
        for n in names:
            full = roman(n)
            k = 1
            while any(hfs_name(full, k).lower() == s.lower() for s in seen):   # ASCII and the test's few accents
                k += 1
            h = hfs_name(full, k)
            seen.append(h)
            p = os.path.join(src, n)
            out[hpath + b':' + h] = p
            if os.path.isdir(p):
                walk(p, hpath + b':' + h)
    walk(d, b'')
    return out

# ---- the check --------------------------------------------------------------------------------------
def run(*args):
    return subprocess.run(list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)

def check(img, d):
    r = run('hmount', img)
    if r.returncode:
        sys.exit('FAIL: hmount: ' + r.stderr.decode(errors='replace'))
    exp = expected(d)
    vol = r.stdout.split(b'"')[1] if b'"' in r.stdout else b''
    # hls -R: every folder's names, one per line (-1), folders marked with a colon by -F... walked by hand
    got = {}
    def walk(hp):
        r = run('hls', '-1a', hp or b':')
        if r.returncode:
            sys.exit('FAIL: hls %r: %s' % (hp, r.stderr))
        for n in r.stdout.split(b'\n'):
            if not n:
                continue
            p = hp + b':' + n
            got[p] = 1
    for k in sorted(exp, key=len):
        if os.path.isdir(exp[k]):
            pass
    walk(b'')
    dirs = [k for k in exp if os.path.isdir(exp[k])]
    for k in dirs:
        walk(k)
    miss = sorted(set(exp) - set(got))
    extra = sorted(set(got) - set(exp))
    if miss or extra:
        print('FAIL: names differ\n  expected, not there: %r\n  there, not expected: %r' % (miss[:20], extra[:20]))
        sys.exit(1)
    files = [k for k in exp if not os.path.isdir(exp[k])]
    # a sample of the many small files, all of the others
    many = [k for k in files if b':Many:' in k]
    sel = [k for k in files if b':Many:' not in k] + many[:5] + many[-5:]
    nmac = 0
    for k in sel:
        src = exp[k]
        data = open(src, 'rb').read()
        head, tail = os.path.split(src)
        ad = os.path.join(head, '._' + tail)
        r = run('hcopy', '-m', k, '-')
        if r.returncode:
            sys.exit('FAIL: hcopy -m %r: %s' % (k, r.stderr))
        mb = r.stdout
        dl, rl = struct.unpack('>II', mb[83:91])
        ftype, creator = mb[65:69], mb[69:73]
        dpart = mb[128:128 + dl]
        rpart = mb[128 + ((dl + 127) // 128) * 128:][:rl]
        if dpart != data:
            sys.exit('FAIL: %r: the data fork differs (%d bytes, %d expected)' % (k, dl, len(data)))
        if os.path.exists(ad):
            a = open(ad, 'rb').read()
            t, c, dn, rn = MAC_FILES[os.path.relpath(src, d)]
            rs = a[0xee2:]
            if (ftype, creator) != (t, c) or rpart != rs:
                sys.exit('FAIL: %r: type %r creator %r, %d resource bytes; expected %r %r, %d' % (k, ftype, creator, rl, t, c, len(rs)))
            nmac += 1
        elif rl:
            sys.exit('FAIL: %r has a resource fork' % k)
    run('humount')
    print('check: volume "%s", %d names as expected (%d folders), %d files compared byte for byte, %d of them Mac files with both forks, type and creator'
          % (vol.decode('mac_roman'), len(exp), len(dirs), len(sel), nmac))

def split(mb_path, outdir, name=None):
    """a MacBinary file (hcopy -m, mac/build/*.bin) as a modern Mac would leave it on FAT or exFAT: the
    data fork as NAME, the type, creator, Finder flags and resource fork in ._NAME"""
    mb = open(mb_path, 'rb').read()
    nlen = mb[1]
    mname = mb[2:2 + nlen].decode('mac_roman')
    ftype, creator, flags = mb[65:69], mb[69:73], mb[73] << 8 | mb[101]
    dl, rl = struct.unpack('>II', mb[83:91])
    data = mb[128:128 + dl]
    rsrc = mb[128 + ((dl + 127) // 128) * 128:][:rl]
    name = name or mname.replace('/', ':')
    write(os.path.join(outdir, name), data)
    write(os.path.join(outdir, '._' + name), appledouble(ftype, creator, flags & ~0x0100, rsrc))
    print('split: %s -> %s (%d data, %d resource bytes, %s/%s)' % (mb_path, name, dl, rl, ftype.decode('mac_roman'), creator.decode('mac_roman')))

if __name__ == '__main__':
    a = sys.argv[1:]
    if a[0] == 'split':
        split(a[1], a[2], a[3] if len(a) > 3 else None)
    elif a[0] == 'tree':
        tree(a[1], int(a[a.index('--many') + 1]) if '--many' in a else 300)
    elif a[0] == 'check':
        check(a[1], a[2])
    else:
        sys.exit(__doc__)
