/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_fatfs.c - a reader for FAT12, FAT16, FAT32 and exFAT, shared by the SD card and USB sticks.
 *
 * Backstory (21 November 2026, issue #20). No file system is native to both a classic Mac and a modern
 * computer: System 6 and 7 read HFS, and macOS 10.15 and later have no HFS at all. What Windows, macOS
 * and Linux all write is FAT32 and exFAT, so the Pi reads those and shows the Mac an HFS volume made
 * from them (psmac_hfsview.c). The SD card's own code (psmac_card.c) has had a small FAT16/FAT32 reader
 * and writer for the kernel's files since 18 November; this is the reader both the card's exchange
 * folder and USB sticks use, with exFAT added: the one format a modern Mac offers for a stick beside
 * FAT32 ("ExFAT" and "MS-DOS (FAT)" in Disk Utility).
 *
 * Sources: Microsoft's "FAT: General Overview of On-Disk Format" (1.03) for FAT, with the VFAT long-name
 * entries; Microsoft's "exFAT file system specification" (2019) for exFAT: the boot sector (section 3.1),
 * the FAT (4), the cluster heap, directory entry sets (6: a File entry 0x85, a Stream Extension 0xC0,
 * File Name entries 0xC1, with a checksum over the set, 6.3.3), NoFatChain (6.6.1.2: the clusters follow
 * each other and the FAT says nothing about them), ValidDataLength (7.6.4: bytes beyond it read as
 * zeros). The allocation bitmap and the up-case table are only needed to write, and are not read.
 *
 * What is not done: TexFAT, a second FAT that differs from the first (the first is read), FAT32's
 * FSInfo, names in an OEM code page (a short name's bytes above $7F are taken as Latin-1).
 *
 * Portable C, no kernel calls: the host test (tools/hfsview_test.c) builds this file as it is. Every
 * read goes through the caller's function, a few sectors at a time, so the caller can bound how much
 * one step reads (core 3 serves the mailbox between steps, core 1 the sound output).
 */
#include <stdint.h>
#include <string.h>
#include "psmac_fatfs.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static int rd(struct fx_vol *v, uint32_t lba, uint32_t n, uint8_t *buf)
{
    v->reads++;
    v->read_secs += n;
    if (v->read(v->ctx, lba, n, buf)) {
        v->errors++;
        return -1;
    }
    return 0;
}

const char *fx_type_name(int type)
{
    switch (type) {
    case FX_FAT12: return "FAT12";
    case FX_FAT16: return "FAT16";
    case FX_FAT32: return "FAT32";
    case FX_EXFAT: return "exFAT";
    default: return "no FAT";
    }
}

/* ---- the boot sector ------------------------------------------------------------------------------- */
static int is_exfat(const uint8_t *b) { return b[510] == 0x55 && b[511] == 0xaa && !memcmp(b + 3, "EXFAT   ", 8); }
static int is_fat(const uint8_t *b)
{
    /* a FAT boot sector: a jump, 512 bytes per sector (the only size PiDisk's devices have), a power of two
       sectors per cluster, reserved sectors and at least one FAT */
    uint8_t spc = b[13];
    return b[510] == 0x55 && b[511] == 0xaa && (b[0] == 0xeb || b[0] == 0xe9) && le16(b + 11) == 512 &&
           spc && !(spc & (spc - 1)) && le16(b + 14) && b[16] && memcmp(b + 3, "NTFS", 4) && memcmp(b + 3, "EXFAT", 5);
}

int fx_mount(struct fx_vol *v, fx_read_fn read, void *ctx, uint32_t lba)
{
    uint8_t *b = v->sec;
    v->read = read; v->ctx = ctx; v->type = FX_NONE; v->part = lba; v->fat_at = 0xffffffffu;
    v->label_len = 0;
    if (rd(v, lba, 1, b))
        return -1;
    if (is_exfat(b)) {
        unsigned bps = b[108], spcs = b[109];
        if (bps < 9 || bps > 12 || bps + spcs > 25 || b[110] < 1)
            return -2;                                  /* sectors of 512-4096 bytes, clusters up to 32 MB (7.1) */
        uint32_t k = 1u << (bps - 9);                   /* device sectors per exFAT sector */
        uint64_t vlen = le64(b + 72) * k;
        v->type = FX_EXFAT;
        v->total = vlen > 0xffffffffu ? 0xffffffffu : (uint32_t)vlen;
        v->spc = 1u << (bps + spcs - 9);
        v->fat_lba = lba + le32(b + 80) * k;
        v->fat_secs = le32(b + 84) * k;
        v->data_lba = lba + le32(b + 88) * k;
        v->clusters = le32(b + 92);
        v->root_clus = le32(b + 96);
        v->root_lba = v->root_secs = 0;
        if (v->root_clus < 2 || v->root_clus >= v->clusters + 2)
            return -3;
        return 0;
    }
    if (!is_fat(b))
        return -4;
    uint32_t rsvd = le16(b + 14), nfats = b[16], rootent = le16(b + 17);
    uint32_t tot = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    uint32_t fatsz = le16(b + 22) ? le16(b + 22) : le32(b + 36);
    v->spc = b[13];
    v->total = tot;
    v->fat_lba = lba + rsvd;
    v->fat_secs = fatsz;
    v->root_lba = v->fat_lba + nfats * fatsz;
    v->root_secs = (rootent * 32u + 511u) / 512u;
    v->data_lba = v->root_lba + v->root_secs;
    if (tot <= rsvd + nfats * fatsz + v->root_secs)
        return -5;
    v->clusters = (tot - (rsvd + nfats * fatsz + v->root_secs)) / v->spc;
    /* the type follows from the number of clusters alone (the overview's "the only way") */
    v->type = v->clusters < 4085u ? FX_FAT12 : v->clusters < 65525u ? FX_FAT16 : FX_FAT32;
    v->root_clus = v->type == FX_FAT32 ? le32(b + 44) : 0;
    if (v->type == FX_FAT32) {
        v->root_lba = v->root_secs = 0;
        if (v->root_clus < 2 || v->root_clus >= v->clusters + 2)
            return -6;
    }
    /* the label of the boot sector; the root directory's label entry, read with the root, wins */
    const uint8_t *lab = b + (v->type == FX_FAT32 ? 71 : 43);
    if (b[v->type == FX_FAT32 ? 66 : 38] == 0x29 && memcmp(lab, "NO NAME    ", 11)) {
        int n = 11;
        while (n && lab[n - 1] == ' ')
            n--;
        for (int i = 0; i < n; i++)
            v->label[i] = lab[i];
        v->label_len = n;
    }
    return 0;
}

uint32_t fx_clus_lba(const struct fx_vol *v, uint32_t c) { return v->data_lba + (c - 2) * v->spc; }

/* the FAT entry of cluster c through the window: FX_EOC at the end of a chain, 0 for a free, bad or
   impossible entry (the chain is broken there) */
static int fat_bytes(struct fx_vol *v, uint32_t off, uint32_t len, uint8_t *out)
{
    uint32_t s = off / 512u, e = (off + len - 1) / 512u;
    if (e >= v->fat_secs)
        return -1;
    if (v->fat_at == 0xffffffffu || s < v->fat_at || e >= v->fat_at + FX_FATWIN) {
        uint32_t n = v->fat_secs - s < FX_FATWIN ? v->fat_secs - s : FX_FATWIN;
        if (rd(v, v->fat_lba + s, n, v->fat))
            return v->fat_at = 0xffffffffu, -1;
        v->fat_at = s;
    }
    memcpy(out, v->fat + (off - v->fat_at * 512u), len);
    return 0;
}
uint32_t fx_next(struct fx_vol *v, uint32_t c)
{
    uint8_t b[4];
    uint32_t n;
    if (c < 2 || c >= v->clusters + 2)
        return 0;
    switch (v->type) {
    case FX_FAT12:
        if (fat_bytes(v, c + c / 2, 2, b))
            return 0;
        n = le16(b);
        n = c & 1 ? n >> 4 : n & 0xfffu;
        if (n >= 0xff8u)
            return FX_EOC;
        break;
    case FX_FAT16:
        if (fat_bytes(v, c * 2, 2, b))
            return 0;
        n = le16(b);
        if (n >= 0xfff8u)
            return FX_EOC;
        break;
    case FX_FAT32:
        if (fat_bytes(v, c * 4, 4, b))
            return 0;
        n = le32(b) & 0x0fffffffu;
        if (n >= 0x0ffffff8u)
            return FX_EOC;
        break;
    case FX_EXFAT:
        if (fat_bytes(v, c * 4, 4, b))
            return 0;
        n = le32(b);
        if (n >= 0xfffffff8u)
            return FX_EOC;
        break;
    default:
        return 0;
    }
    return n >= 2 && n < v->clusters + 2 ? n : 0;
}

/* ---- finding the volume ---------------------------------------------------------------------------- */
static int vbr_type(struct fx_vol *s, uint32_t lba)
{
    if (rd(s, lba, 1, s->sec))
        return FX_NONE;
    if (is_exfat(s->sec))
        return FX_EXFAT;
    return is_fat(s->sec) ? FX_FAT32 : FX_NONE;        /* which FAT is decided by fx_mount */
}
int fx_probe(fx_read_fn read, void *ctx, uint32_t dev_blocks, struct fx_vol *s, uint32_t *lba, int *type, const char **what)
{
    s->read = read; s->ctx = ctx;
    uint8_t m[512];
    if (rd(s, 0, 1, s->sec))
        return 0;
    memcpy(m, s->sec, 512);
    if ((*type = vbr_type(s, 0)) != FX_NONE) {
        *lba = 0; *what = "with no partition table";
        return 1;
    }
    if (m[510] != 0x55 || m[511] != 0xaa)
        return 0;
    if (m[446 + 4] == 0xee && dev_blocks > 2 && !rd(s, 1, 1, s->sec) && !memcmp(s->sec, "EFI PART", 8)) {
        /* GPT (UEFI 2.x, 5.3): the entries' first block and size from the header at block 1 */
        uint32_t elba = le32(s->sec + 72), nent = le32(s->sec + 80), esz = le32(s->sec + 84);
        if (esz < 128 || esz > 512 || (esz & (esz - 1)) || nent > 128)
            return 0;
        for (uint32_t i = 0; i < nent; i++) {
            uint32_t at = i * esz;
            if (rd(s, elba + at / 512u, 1, s->sec))
                return 0;
            const uint8_t *e = s->sec + at % 512u;
            /* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, Microsoft basic data, its first field little-endian;
               EFI system partitions (C12A7328-...) are FAT32 too, and are passed over */
            if (le32(e) != 0xebd0a0a2u || le16(e + 4) != 0xb9e5u)
                continue;
            if (le32(e + 36) || le32(e + 44))
                continue;                               /* beyond 2 TB: PiDisk's blocks are 32-bit */
            uint32_t st = le32(e + 32);
            int t = vbr_type(s, st);
            if (t != FX_NONE) {
                *lba = st; *type = t; *what = "in a GPT partition";
                return 1;
            }
        }
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = m + 446 + 16 * i;
        uint8_t t = e[4];
        /* FAT12, FAT16 (<32 MB, >32 MB, LBA), FAT32 (CHS, LBA), and $07 (exFAT or NTFS: the boot sector says) */
        if (t != 0x01 && t != 0x04 && t != 0x06 && t != 0x0e && t != 0x0b && t != 0x0c && t != 0x07)
            continue;
        uint32_t st = le32(e + 8);
        if (!st || st >= dev_blocks)
            continue;
        int ft = vbr_type(s, st);
        if (ft != FX_NONE) {
            *lba = st; *type = ft; *what = "in an MBR partition";
            return 1;
        }
    }
    return 0;
}

/* ---- directories ----------------------------------------------------------------------------------- */
void fx_dir_open(struct fx_dir *it, struct fx_vol *v, const struct fx_dirent *d)
{
    it->v = v; it->sec = 0; it->nbuf = it->pos = 0; it->end = it->err = 0; it->left = 0; it->contig = 0; it->root = !d;
    it->lfn_ok = 0; it->set_n = 0; it->set_want = 0;
    if (!d) {
        it->clus = v->root_clus;                        /* 0 for FAT12/16: the fixed root */
        return;
    }
    it->clus = d->clus;
    if (d->clus < 2)
        it->end = 1;                                    /* an empty folder (exFAT allows it) */
    if (v->type == FX_EXFAT && d->contig) {
        it->contig = 1;
        it->left = (uint32_t)((d->size + 511u) / 512u);
    }
}
/* the next few sectors of the directory into buf: 0, 1 at its end, -1 on an error */
static int dir_fill(struct fx_dir *it)
{
    struct fx_vol *v = it->v;
    uint32_t lba, n;
    if (it->end)
        return 1;
    if (!it->clus) {                                    /* FAT12/16 root */
        if (it->sec >= v->root_secs)
            return it->end = 1, 1;
        lba = v->root_lba + it->sec;
        n = v->root_secs - it->sec;
    } else {
        if (it->sec >= v->spc) {
            if (it->contig)
                it->clus++;
            else {
                uint32_t nx = fx_next(v, it->clus);
                if (nx == FX_EOC)
                    return it->end = 1, 1;
                if (!nx)
                    return it->err = 1, -1;
                it->clus = nx;
            }
            it->sec = 0;
        }
        if (it->clus < 2 || it->clus >= v->clusters + 2)
            return it->err = 1, -1;
        if (it->contig && !it->left)
            return it->end = 1, 1;
        lba = fx_clus_lba(v, it->clus) + it->sec;
        n = v->spc - it->sec;
        if (it->contig && n > it->left)
            n = it->left;
    }
    if (n > FX_FATWIN)
        n = FX_FATWIN;
    if (rd(v, lba, n, it->buf))
        return it->err = 1, -1;
    it->sec += n;
    if (it->contig)
        it->left -= n;
    it->nbuf = n * 16u;
    it->pos = 0;
    return 0;
}
static uint8_t sfn_sum(const uint8_t *e)
{
    uint8_t c = 0;
    for (int i = 0; i < 11; i++)
        c = (uint8_t)(((c & 1) << 7) + (c >> 1) + e[i]);
    return c;
}
static uint16_t exfat_set_sum(const uint8_t *s, int bytes)
{
    uint16_t c = 0;
    for (int i = 0; i < bytes; i++) {
        if (i == 2 || i == 3)
            continue;                                   /* the checksum's own field (6.3.3) */
        c = (uint16_t)(((c & 1) ? 0x8000u : 0) + (c >> 1) + s[i]);
    }
    return c;
}
/* a FAT 8.3 entry as a name: lower case where the NT flags of byte 12 say so */
static void short_name(const uint8_t *e, struct fx_dirent *o)
{
    int n = 0;
    for (int k = 0; k < 8 && e[k] != ' '; k++) {
        uint8_t c = k == 0 && e[0] == 0x05 ? 0xe5 : e[k];
        if ((e[12] & 0x08) && c >= 'A' && c <= 'Z')
            c = (uint8_t)(c + 32);
        o->name[n++] = c;
    }
    if (e[8] != ' ') {
        o->name[n++] = '.';
        for (int k = 8; k < 11 && e[k] != ' '; k++)
            o->name[n++] = (uint8_t)((e[12] & 0x10) && e[k] >= 'A' && e[k] <= 'Z' ? e[k] + 32 : e[k]);
    }
    o->nlen = n;
}
static int fat_entry(struct fx_dir *it, const uint8_t *e, struct fx_dirent *o)
{
    static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    struct fx_vol *v = it->v;
    if (e[0] == 0xe5) {
        it->lfn_ok = 0;
        return 0;
    }
    if (e[11] == 0x0f) {                                /* a long-name piece: the last first, 13 units each */
        unsigned seq = e[0] & 0x1fu;
        if (!seq || seq > 20) {
            it->lfn_ok = 0;
            return 0;
        }
        if (e[0] & 0x40) {
            it->lfn_ok = 1; it->lfn_sum = e[13]; it->lfn_n = (int)seq;
            for (int k = 0; k < FX_NAME_MAX + 1 + 13; k++)
                it->lfn[k] = 0;
        } else if (!it->lfn_ok || e[13] != it->lfn_sum || (int)seq != it->lfn_n - 1) {
            it->lfn_ok = 0;
            return 0;
        } else
            it->lfn_n = (int)seq;
        for (unsigned k = 0; k < 13; k++) {
            unsigned i = (seq - 1) * 13 + k;
            if (i < FX_NAME_MAX + 13)
                it->lfn[i] = le16(e + at[k]);
        }
        return 0;
    }
    int lfn = it->lfn_ok && it->lfn_n == 1 && it->lfn_sum == sfn_sum(e);
    it->lfn_ok = 0;
    if (e[11] & 0x08) {                                 /* the volume label (the root's: the name of the volume) */
        if (!(e[11] & 0x10) && it->root) {
            int n = 11;
            while (n && e[n - 1] == ' ')
                n--;
            for (int i = 0; i < n; i++)
                v->label[i] = e[i];
            v->label_len = n;
        }
        return 0;
    }
    if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' ')))
        return 0;                                       /* . and .. */
    if (lfn) {
        int n = 0;
        while (n < FX_NAME_MAX && it->lfn[n] && it->lfn[n] != 0xffff)
            n++;
        memcpy(o->name, it->lfn, (size_t)n * 2);
        o->nlen = n;
    } else
        short_name(e, o);
    o->attr = e[11];
    o->contig = 0;
    o->clus = (v->type == FX_FAT32 ? (uint32_t)le16(e + 20) << 16 : 0) | le16(e + 26);
    o->size = o->valid = o->attr & FX_ATTR_DIR ? 0 : le32(e + 28);
    o->ctime = (uint32_t)le16(e + 16) << 16 | le16(e + 14);
    o->mtime = (uint32_t)le16(e + 24) << 16 | le16(e + 22);
    return 1;
}
/* exFAT: entries are gathered into it->set until the File entry's secondary count is met */
static int exfat_entry(struct fx_dir *it, const uint8_t *e, struct fx_dirent *o)
{
    struct fx_vol *v = it->v;
    uint8_t t = e[0];
    if (it->set_n) {
        if (t < 0xc0) {                                 /* not a secondary entry (0xC0-0xFF) */
            it->set_n = 0;                              /* a set cut short: dropped, this entry looked at afresh */
            return exfat_entry(it, e, o);
        }
        memcpy(it->set + 32 * it->set_n++, e, 32);
        if (it->set_n < it->set_want)
            return 0;
        int n = it->set_n;
        it->set_n = 0;
        const uint8_t *f = it->set, *s = it->set + 32;
        if (exfat_set_sum(it->set, 32 * n) != le16(f + 2) || s[0] != 0xc0)
            return 0;                                   /* a damaged set is left out rather than guessed at */
        int len = s[3], k = 0;
        for (int i = 2; i < n && k < len; i++) {
            const uint8_t *ne = it->set + 32 * i;
            if (ne[0] != 0xc1)
                continue;
            for (int j = 0; j < 15 && k < len; j++)
                o->name[k++] = le16(ne + 2 + 2 * j);
        }
        o->nlen = k;
        o->attr = (uint8_t)le16(f + 4);
        o->ctime = le32(f + 8);
        o->mtime = le32(f + 12);
        o->contig = (s[1] & 2) != 0;
        o->valid = le64(s + 8);
        o->clus = le32(s + 20);
        o->size = le64(s + 24);
        if (o->attr & FX_ATTR_DIR)
            o->valid = o->size;
        if (o->valid > o->size)
            o->valid = o->size;
        return k > 0;
    }
    if (t == 0x85) {
        int sc = e[1];
        if (sc < 2 || sc > 18)
            return 0;
        memcpy(it->set, e, 32);
        it->set_n = 1;
        it->set_want = sc + 1;
        return 0;
    }
    if (t == 0x83 && it->root) {     /* the volume label, in the root */
        int n = e[1] > 11 ? 11 : e[1];
        for (int i = 0; i < n; i++)
            v->label[i] = le16(e + 2 + 2 * i);
        v->label_len = n;
    }
    return 0;                                           /* the bitmap, the up-case table, GUIDs, unused entries */
}
int fx_dir_next(struct fx_dir *it, struct fx_dirent *o)
{
    for (;;) {
        if (it->err)
            return -1;
        if (it->pos >= it->nbuf) {
            int r = dir_fill(it);
            if (r)
                return r > 0 ? 0 : -1;
        }
        const uint8_t *e = it->buf + 32 * it->pos++;
        if (e[0] == 0) {                                /* the end of the directory, in both */
            it->end = 1;
            it->nbuf = it->pos = 0;
            return 0;
        }
        int r = it->v->type == FX_EXFAT ? exfat_entry(it, e, o) : fat_entry(it, e, o);
        if (r)
            return 1;
    }
}

/* ---- a file's clusters ----------------------------------------------------------------------------- */
int fx_runs(struct fx_vol *v, uint32_t clus, uint64_t size, int contig, int (*f)(void *arg, uint32_t c, uint32_t n), void *arg, uint32_t *nclus)
{
    uint64_t csz = (uint64_t)v->spc * 512u;
    uint32_t want = (uint32_t)((size + csz - 1) / csz), got = 0;
    *nclus = 0;
    if (!want)
        return 0;
    if (clus < 2 || clus >= v->clusters + 2)
        return -1;
    if (contig) {
        if ((uint64_t)clus + want > (uint64_t)v->clusters + 2)
            return -1;
        *nclus = want;
        return f(arg, clus, want);
    }
    uint32_t run = clus, n = 1;
    for (got = 1; got < want; got++) {
        uint32_t nx = fx_next(v, clus);
        if (nx == FX_EOC || !nx)
            break;
        if (nx == clus + 1)
            n++;
        else {
            if (f(arg, run, n))
                return -2;
            run = nx; n = 1;
        }
        clus = nx;
    }
    if (f(arg, run, n))
        return -2;
    *nclus = got;
    return got == want ? 0 : -1;
}
