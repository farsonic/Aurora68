/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_hfsview.c - an HFS volume made up from a FAT or exFAT file system, for the Mac's own HFS to read.
 *
 * Backstory (21 November 2026, issue #20). A stick from a modern Mac, Windows or Linux is FAT32 or exFAT;
 * System 6.0.8 and 7.1 read HFS ("Mac OS Standard") and nothing else without PC Exchange. So the Pi shows
 * the Mac a genuine HFS volume, made in memory from the FAT side's directories: a boot block area, a
 * master directory block, a volume bitmap, an extents B-tree (empty: every fork here is one extent) and
 * a catalog B-tree, all in 512-byte blocks, as Inside Macintosh: Files (chapter 2, "Data Organization on
 * Volumes") describes them. Only that metadata lives in memory. A file's data is not copied: the volume's
 * allocation blocks are laid out fork after fork, and a read of one of them becomes a read of the FAT or
 * exFAT clusters that hold those bytes, from the medium, when the Mac asks.
 *
 * Mac files on FAT. When a modern Mac copies a file with a resource fork, a type and a creator to a FAT
 * or exFAT disk, it writes them beside the file in an AppleDouble file named "._" and the file's name
 * (Apple's AppleSingle/AppleDouble Formats, version 2, 1990: a header, then entries by ID; ID 2 is the
 * resource fork, ID 9 the Finder information, FInfo and FXInfo, 32 bytes, followed on macOS by its
 * extended attributes). So a classic application copied from a MacBook to a stick comes back whole: its
 * data fork from the file, its resource fork, type and creator from the "._" file, which itself is not
 * shown. The resource fork is read in place too, at its offset inside the "._" file.
 *
 * Names. FAT and exFAT names are UTF-16, up to 255 units; HFS names are Mac Roman, 31 characters, and a
 * folder's names must differ in the File Manager's order (case folded, accents kept). So each name is
 * converted (decomposed accents, which macOS may write, are put back together first), a character Mac
 * Roman lacks becomes '?', a name longer than 31 keeps its extension behind an ellipsis, and names that
 * then clash in one folder get " #2", " #3"... in the order the directory lists them, so the same medium
 * always gives the same names. Names beginning with a dot (macOS's .DS_Store, .Trashes, .Spotlight-V100,
 * Unix's hidden files) are not shown, as the Finder on a modern Mac does not show them; files marked
 * hidden on FAT are shown invisible. "System Volume Information" and "$RECYCLE.BIN" are Windows' own.
 *
 * The catalog order is the File Manager's: Apple's RelString ordering with case folded, the one HFS's
 * catalog keys use. The table below gives each Mac Roman character its rank in that order; it is the
 * same order Linux's fs/hfs and hfsutils sort HFS catalogs by (and hfsutils is how this is checked).
 *
 * Size. HFS counts allocation blocks in 16 bits: at most 65,535 per volume, each a multiple of 512 bytes.
 * The allocation block size is chosen as the smallest that holds everything; a volume of more than 2 GB
 * (System 7.1's limit: 65,535 blocks of 32 KB) cannot be made, and then the largest files are left out
 * until the rest fits, and the log says how many. Every fork has one extent, so the
 * extents B-tree is only its header node.
 *
 * The volume is locked (drAtrb's software lock, and the PiDisk unit is not writable), so the Finder
 * refuses to change it, as for a CD-ROM. Writing back to FAT is the issue's stage 3.
 *
 * Memory. The nodes (about 130 bytes per file or folder, at most HV_MAX_NODES), the clusters' runs (8
 * bytes per fragment), the catalog's record list (4 bytes per record), the header and index nodes, and
 * 8 KB of scratch. The UTF-16 names are kept only while the volume is being made. Leaf nodes of the
 * catalog are put together again each time the Mac reads one (a few microseconds), so that the largest
 * part of the catalog never sits in memory.
 *
 * Steps. hv_step does a bounded piece of the work (a few dozen sectors at most), so core 3 (the card)
 * and core 1 (USB sticks) go on serving everything else while a large folder is read.
 */
#include <stdint.h>
#include <string.h>
#include "psmac_hfsview.h"

/* ---- Mac Roman ------------------------------------------------------------------------------------- */
/* $80-$FF as Unicode (Apple's ROMAN.TXT; $DB is the euro since Mac OS 8.5, the currency sign before) */
static const uint16_t roman_hi[128] = {
    0x00c4, 0x00c5, 0x00c7, 0x00c9, 0x00d1, 0x00d6, 0x00dc, 0x00e1, 0x00e0, 0x00e2, 0x00e4, 0x00e3, 0x00e5, 0x00e7, 0x00e9, 0x00e8,
    0x00ea, 0x00eb, 0x00ed, 0x00ec, 0x00ee, 0x00ef, 0x00f1, 0x00f3, 0x00f2, 0x00f4, 0x00f6, 0x00f5, 0x00fa, 0x00f9, 0x00fb, 0x00fc,
    0x2020, 0x00b0, 0x00a2, 0x00a3, 0x00a7, 0x2022, 0x00b6, 0x00df, 0x00ae, 0x00a9, 0x2122, 0x00b4, 0x00a8, 0x2260, 0x00c6, 0x00d8,
    0x221e, 0x00b1, 0x2264, 0x2265, 0x00a5, 0x00b5, 0x2202, 0x2211, 0x220f, 0x03c0, 0x222b, 0x00aa, 0x00ba, 0x03a9, 0x00e6, 0x00f8,
    0x00bf, 0x00a1, 0x00ac, 0x221a, 0x0192, 0x2248, 0x2206, 0x00ab, 0x00bb, 0x2026, 0x00a0, 0x00c0, 0x00c3, 0x00d5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201c, 0x201d, 0x2018, 0x2019, 0x00f7, 0x25ca, 0x00ff, 0x0178, 0x2044, 0x20ac, 0x2039, 0x203a, 0xfb01, 0xfb02,
    0x2021, 0x00b7, 0x201a, 0x201e, 0x2030, 0x00c2, 0x00ca, 0x00c1, 0x00cb, 0x00c8, 0x00cd, 0x00ce, 0x00cf, 0x00cc, 0x00d3, 0x00d4,
    0xf8ff, 0x00d2, 0x00da, 0x00db, 0x00d9, 0x0131, 0x02c6, 0x02dc, 0x00af, 0x02d8, 0x02d9, 0x02da, 0x00b8, 0x02dd, 0x02db, 0x02c7,
};
/* a letter and a combining mark (U+0300 grave, 0301 acute, 0302 circumflex, 0303 tilde, 0308 diaeresis,
   030A ring, 0327 cedilla, 0338 the slash of not-equal) as one Mac Roman character: the canonical
   decompositions of the Mac Roman letters (Unicode's UnicodeData.txt). macOS writes names decomposed. */
static const uint16_t marks[8] = { 0x300, 0x301, 0x302, 0x303, 0x308, 0x30a, 0x327, 0x338 };
static const struct { char base; uint8_t mark, mac; } composed[] = {
    { 'A', 4, 0x80 }, { 'A', 5, 0x81 }, { 'C', 6, 0x82 }, { 'E', 1, 0x83 }, { 'N', 3, 0x84 }, { 'O', 4, 0x85 }, { 'U', 4, 0x86 },
    { 'a', 1, 0x87 }, { 'a', 0, 0x88 }, { 'a', 2, 0x89 }, { 'a', 4, 0x8a }, { 'a', 3, 0x8b }, { 'a', 5, 0x8c }, { 'c', 6, 0x8d },
    { 'e', 1, 0x8e }, { 'e', 0, 0x8f }, { 'e', 2, 0x90 }, { 'e', 4, 0x91 }, { 'i', 1, 0x92 }, { 'i', 0, 0x93 }, { 'i', 2, 0x94 },
    { 'i', 4, 0x95 }, { 'n', 3, 0x96 }, { 'o', 1, 0x97 }, { 'o', 0, 0x98 }, { 'o', 2, 0x99 }, { 'o', 4, 0x9a }, { 'o', 3, 0x9b },
    { 'u', 1, 0x9c }, { 'u', 0, 0x9d }, { 'u', 2, 0x9e }, { 'u', 4, 0x9f }, { '=', 7, 0xad }, { 'A', 0, 0xcb }, { 'A', 3, 0xcc },
    { 'O', 3, 0xcd }, { 'y', 4, 0xd8 }, { 'Y', 4, 0xd9 }, { 'A', 2, 0xe5 }, { 'E', 2, 0xe6 }, { 'A', 1, 0xe7 }, { 'E', 4, 0xe8 },
    { 'E', 0, 0xe9 }, { 'I', 1, 0xea }, { 'I', 2, 0xeb }, { 'I', 4, 0xec }, { 'I', 0, 0xed }, { 'O', 1, 0xee }, { 'O', 2, 0xef },
    { 'O', 0, 0xf1 }, { 'U', 1, 0xf2 }, { 'U', 2, 0xf3 }, { 'U', 0, 0xf4 },
};
/* each Mac Roman character's rank in the catalog's order: upper and lower case share a rank, accented
   letters follow their letter, a no-break space ranks as a space */
static const uint8_t order[256] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    0x20, 0x22, 0x23, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
    0x47, 0x48, 0x57, 0x59, 0x5d, 0x5f, 0x66, 0x68, 0x6a, 0x6c, 0x72, 0x74, 0x76, 0x78, 0x7a, 0x7e,
    0x8c, 0x8e, 0x90, 0x92, 0x95, 0x97, 0x9e, 0xa0, 0xa2, 0xa4, 0xa7, 0xa9, 0xaa, 0xab, 0xac, 0xad,
    0x4e, 0x48, 0x57, 0x59, 0x5d, 0x5f, 0x66, 0x68, 0x6a, 0x6c, 0x72, 0x74, 0x76, 0x78, 0x7a, 0x7e,
    0x8c, 0x8e, 0x90, 0x92, 0x95, 0x97, 0x9e, 0xa0, 0xa2, 0xa4, 0xa7, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3,
    0x4a, 0x4c, 0x5a, 0x60, 0x7b, 0x7f, 0x98, 0x4f, 0x49, 0x51, 0x4a, 0x4b, 0x4c, 0x5a, 0x60, 0x63,
    0x64, 0x65, 0x6e, 0x6f, 0x70, 0x71, 0x7b, 0x84, 0x85, 0x86, 0x7f, 0x80, 0x9a, 0x9b, 0x9c, 0x98,
    0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0x94, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0x4d, 0x81,
    0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0x55, 0x8a, 0xcc, 0x4d, 0x81,
    0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0x26, 0x27, 0xd4, 0x20, 0x49, 0x4b, 0x80, 0x82, 0x82,
    0xd5, 0xd6, 0x24, 0x25, 0x2d, 0x2e, 0xd7, 0xd8, 0xa6, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
    0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};
#define UNMAPPED '?'                        /* what a character Mac Roman lacks becomes: seen, and legal in HFS */
#define ELLIPSIS 0xc9

/* ---- the nodes ------------------------------------------------------------------------------------- */
enum { F_DIR = 1, F_AD = 2, F_HIDE = 4, F_CONTIG = 8, F_INVIS = 16, F_INFO = 32, F_DROP = 64 };
struct hv_node {
    uint32_t parent;                        /* the folder's node (the root is node 0) */
    uint32_t clus, dlen, dvalid;            /* the data: first cluster, length, bytes that are not zeros */
    uint32_t run, nrun;                     /* its clusters in h->runs */
    uint32_t ad;                            /* an AppleDouble file's own node: its file; a file's: its AppleDouble */
    uint32_t roff, rlen;                    /* the resource fork, inside the AppleDouble file */
    uint32_t cdate, mdate;                  /* Mac time */
    uint32_t nm, hash;                      /* the UTF-16 name in h->names (while building); its hash */
    uint16_t nlen, valence, dstart, dblks, rstart, rblks, sfx;
    uint8_t flags;
    uint8_t mname[32];                      /* the HFS name, Mac Roman, a Pascal string */
    uint8_t finfo[32];                      /* FInfo and FXInfo (DInfo and DXInfo for a folder) */
};
enum { P_SCAN, P_RUNS, P_PAIR, P_AD, P_NAMES, P_LAYOUT, P_DONE };

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t cnid(uint32_t i) { return i ? 16u + i : 2u; }

void hv_init(struct hv *h, struct fx_vol *fx, void *(*alloc)(uint32_t), void (*release)(void *), void (*log)(const char *, ...), const char *tag)
{
    memset(h, 0, sizeof *h);
    h->fx = fx; h->alloc = alloc; h->release = release; h->log = log; h->tag = tag;
    h->root_volume = 1;
}
void hv_set_root(struct hv *h, const struct fx_dirent *f)
{
    h->root_volume = 0;
    h->root_clus = f->clus; h->root_size = (uint32_t)f->size; h->root_contig = f->contig;
    h->root_nlen = f->nlen < 64 ? f->nlen : 64;
    memcpy(h->root_name, f->name, (size_t)h->root_nlen * 2);
}
static void fail(struct hv *h, const char *why)
{
    h->failed = 1;
    h->log("%s: %s\n", h->tag, why);
}
/* a pool twice as large when full: 0, or -1 at its limit or without memory */
static int grow(struct hv *h, void **p, uint32_t *cap, uint32_t need, uint32_t each, uint32_t limit)
{
    if (need <= *cap)
        return 0;
    uint32_t nc = *cap ? *cap * 2 : 256;
    while (nc < need)
        nc *= 2;
    if (nc > limit)
        nc = limit;
    if (need > nc)
        return -1;
    void *q = h->alloc(nc * each);
    if (!q)
        return -1;
    if (*p) {
        memcpy(q, *p, *cap * each);
        h->release(*p);
    }
    *p = q; *cap = nc;
    return 0;
}
#define RUNS_MAX  (1u << 18)                /* 2 MB of fragments: a heavily fragmented stick stops showing files there */
#define NAMES_MAX (1u << 20)                /* 2 MB of UTF-16 while building */

/* ---- dates ----------------------------------------------------------------------------------------- */
static int32_t days_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= m <= 2;
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}
/* a DOS date and time (the medium's local time, as the Mac's clock is) as seconds since 1 January 1904 */
static uint32_t mac_time(uint32_t dos)
{
    uint32_t y = 1980 + (dos >> 25), mo = (dos >> 21) & 15u, d = (dos >> 16) & 31u;
    uint32_t hh = (dos >> 11) & 31u, mi = (dos >> 5) & 63u, s = (dos & 31u) * 2;
    if (!dos || mo < 1 || mo > 12 || d < 1 || hh > 23 || mi > 59)
        return 0;
    int32_t days = days_civil((int32_t)y, mo, d) - days_civil(1904, 1, 1);
    return (uint32_t)days * 86400u + hh * 3600u + mi * 60u + s;
}

/* ---- names ----------------------------------------------------------------------------------------- */
static uint32_t name_hash(const uint16_t *s, uint32_t n)
{
    uint32_t x = 2166136261u;                       /* FNV-1a */
    for (uint32_t i = 0; i < n; i++)
        x = (x ^ s[i]) * 16777619u;
    return x;
}
static uint8_t to_roman(uint32_t u, struct hv *h)
{
    if (u >= 0x20 && u < 0x7f)
        return u == ':' ? '-' : (uint8_t)u;         /* ':' separates HFS path names */
    if (u == 0xa4)
        return 0xdb;                                /* the currency sign, $DB before Mac OS 8.5 */
    for (int i = 0; i < 128; i++)
        if (roman_hi[i] == u)
            return (uint8_t)(0x80 + i);
    h->n_unmapped++;
    return UNMAPPED;
}
/* UTF-16 into Mac Roman, the whole name (up to 255): its length */
static uint32_t roman_name(struct hv *h, const uint16_t *s, uint32_t n, uint8_t *out)
{
    uint32_t o = 0;
    for (uint32_t i = 0; i < n && o < 255; i++) {
        uint32_t u = s[i];
        if (u >= 0xd800 && u < 0xdc00 && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
            i++;                                    /* a surrogate pair: one character beyond Mac Roman */
            out[o++] = UNMAPPED; h->n_unmapped++;
            continue;
        }
        if (i + 1 < n && u < 0x80) {
            int m = -1;
            for (int k = 0; k < 8; k++)
                if (s[i + 1] == marks[k])
                    m = k;
            if (m >= 0) {
                uint8_t c = 0;
                for (unsigned k = 0; k < sizeof composed / sizeof composed[0]; k++)
                    if (composed[k].base == (char)u && composed[k].mark == m)
                        c = composed[k].mac;
                if (c) {
                    out[o++] = c; i++;
                    continue;
                }
            }
        }
        if (u >= 0x300 && u < 0x370)
            continue;                               /* a combining mark that has no place: the letter stays */
        out[o++] = to_roman(u, h);
    }
    return o;
}
/* the HFS name (a Pascal string) from the full Mac Roman name, with " #k" if k > 1: at most 31
   characters, the extension kept behind an ellipsis when it is shortened */
static void hfs_name(const uint8_t *full, uint32_t n, unsigned k, uint8_t *out)
{
    char sfx[8]; unsigned sl = 0;
    if (k > 1) {
        char d[6]; unsigned nd = 0;
        while (k && nd < 5) { d[nd++] = (char)('0' + k % 10); k /= 10; }
        sfx[sl++] = ' '; sfx[sl++] = '#';
        while (nd) sfx[sl++] = d[--nd];
    }
    uint32_t dot = n;
    for (uint32_t i = n; i > 1; i--)
        if (full[i - 1] == '.') { dot = i - 1; break; }
    if (n - dot > 6)
        dot = n;                                    /* no extension worth keeping: ".jpeg" at most */
    uint32_t ext = n - dot, base = dot;
    uint8_t *p = out + 1;
    if (base + sl + ext <= 31) {
        memcpy(p, full, base); p += base;
    } else {
        uint32_t keep = 31 - sl - ext - 1;
        memcpy(p, full, keep); p += keep;
        *p++ = ELLIPSIS;
    }
    memcpy(p, sfx, sl); p += sl;
    memcpy(p, full + dot, ext); p += ext;
    out[0] = (uint8_t)(p - out - 1);
}
/* the File Manager's comparison of two names: < 0, 0, > 0 */
static int rel(const uint8_t *a, const uint8_t *b)
{
    unsigned n = a[0] < b[0] ? a[0] : b[0];
    for (unsigned i = 1; i <= n; i++)
        if (order[a[i]] != order[b[i]])
            return (int)order[a[i]] - (int)order[b[i]];
    return (int)a[0] - (int)b[0];
}

/* ---- sorting (heapsort: bounded memory, no recursion) ---------------------------------------------- */
typedef int (*cmp_fn)(struct hv *h, uint32_t a, uint32_t b);
static void sift(struct hv *h, uint32_t *a, uint32_t i, uint32_t n, cmp_fn c)
{
    for (;;) {
        uint32_t l = 2 * i + 1, m = i;
        if (l < n && c(h, a[l], a[m]) > 0) m = l;
        if (l + 1 < n && c(h, a[l + 1], a[m]) > 0) m = l + 1;
        if (m == i) return;
        uint32_t t = a[i]; a[i] = a[m]; a[m] = t;
        i = m;
    }
}
static void hsort(struct hv *h, uint32_t *a, uint32_t n, cmp_fn c)
{
    if (n < 2) return;
    for (uint32_t i = n / 2; i-- > 0; )
        sift(h, a, i, n, c);
    for (uint32_t e = n - 1; e > 0; e--) {
        uint32_t t = a[0]; a[0] = a[e]; a[e] = t;
        sift(h, a, 0, e, c);
    }
}

/* a step reads at most this many sectors and one read more (8 at most): about 30 KB, a few milliseconds
   from a card or a stick, so core 3's other work and core 1's sound go on between steps */
#define STEP_SECTORS 48u

/* ---- scan: every folder's entries, breadth first (the node list is the queue) ---------------------- */
static int ascii_eq16(const uint16_t *s, int n, const char *a)
{
    int i = 0;
    for (; i < n && a[i]; i++)
        if (s[i] != (uint8_t)a[i]) return 0;
    return i == n && !a[i];
}
static struct hv_node *new_node(struct hv *h, const struct fx_dirent *e, uint32_t parent)
{
    if (h->nn >= HV_MAX_NODES || grow(h, (void **)&h->n, &h->ncap, h->nn + 1, sizeof *h->n, HV_MAX_NODES) ||
        grow(h, (void **)&h->names, &h->namecap, h->nnames + (uint32_t)e->nlen, 2, NAMES_MAX)) {
        h->n_full++;
        return 0;
    }
    struct hv_node *x = &h->n[h->nn++];
    memset(x, 0, sizeof *x);
    x->parent = parent;
    x->nm = h->nnames; x->nlen = (uint16_t)e->nlen;
    memcpy(h->names + h->nnames, e->name, (size_t)e->nlen * 2);
    h->nnames += (uint32_t)e->nlen;
    x->clus = e->clus;
    x->dlen = (uint32_t)e->size; x->dvalid = (uint32_t)e->valid;
    x->cdate = mac_time(e->ctime); x->mdate = mac_time(e->mtime);
    if (!x->cdate) x->cdate = x->mdate;
    if (e->attr & FX_ATTR_DIR) x->flags |= F_DIR;
    if (e->contig) x->flags |= F_CONTIG;
    if (e->attr & FX_ATTR_HIDDEN) x->flags |= F_INVIS;
    return x;
}
static int scan_step(struct hv *h)
{
    uint32_t r0 = h->fx->read_secs;
    while (h->fx->read_secs - r0 < STEP_SECTORS) {
        if (!h->dir_open) {
            while (h->scan_i < h->nn && (!(h->n[h->scan_i].flags & F_DIR) || (h->n[h->scan_i].flags & F_HIDE)))
                h->scan_i++;
            if (h->scan_i >= h->nn)
                return 1;
            struct hv_node *d = &h->n[h->scan_i];
            if (h->scan_i == 0 && h->root_volume)
                fx_dir_open(h->dir, h->fx, 0);
            else {
                struct fx_dirent t;
                t.clus = d->clus; t.size = t.valid = d->dlen; t.contig = (d->flags & F_CONTIG) != 0; t.attr = FX_ATTR_DIR;
                fx_dir_open(h->dir, h->fx, &t);
            }
            h->dir_open = 1;
        }
        int r = fx_dir_next(h->dir, h->ent);
        if (r < 0) {
            h->log("%s: a folder could not be read (a read error or a broken chain): what was read of it is shown\n", h->tag);
            h->n_broken++;
        }
        if (r <= 0) {
            h->dir_open = 0;
            h->scan_i++;
            continue;
        }
        struct fx_dirent *e = h->ent;
        int dir = (e->attr & FX_ATTR_DIR) != 0;
        if (e->nlen > 2 && e->name[0] == '.' && e->name[1] == '_' && !dir) {
            struct hv_node *x = new_node(h, e, h->scan_i);
            if (x) { x->flags |= F_AD | F_HIDE; h->n_ad++; }
            continue;
        }
        if (e->name[0] == '.' || ascii_eq16(e->name, e->nlen, "System Volume Information") || ascii_eq16(e->name, e->nlen, "$RECYCLE.BIN")) {
            h->n_hidden++;
            continue;
        }
        if (h->scan_i == 0 && h->skip && h->skip(h->skip_arg, e)) {
            h->n_hidden++;
            continue;
        }
        if (!dir && e->size >= 0x7fffffffu) {       /* HFS on System 7.1: no file of 2 GB or more */
            h->n_big++;
            h->bytes_dropped += e->size;
            continue;
        }
        struct hv_node *x = new_node(h, e, h->scan_i);
        if (x) {
            if (dir) h->n_dirs++; else h->n_files++;
        }
    }
    return 0;
}

/* ---- runs: each file's clusters ------------------------------------------------------------------- */
static int add_run(void *arg, uint32_t c, uint32_t n)
{
    struct hv *h = arg;
    if (grow(h, (void **)&h->runs, &h->rcap, h->nruns + 1, sizeof *h->runs, RUNS_MAX))
        return -1;
    h->runs[h->nruns].clus = c; h->runs[h->nruns].n = n;
    h->nruns++;
    return 0;
}
static int runs_step(struct hv *h)
{
    uint32_t r0 = h->fx->read_secs;
    while (h->fx->read_secs - r0 < STEP_SECTORS) {
        if (h->work_i >= h->nn)
            return 1;
        struct hv_node *x = &h->n[h->work_i++];
        if ((x->flags & F_DIR) || !x->dlen)
            continue;
        uint32_t nc, first = h->nruns;
        int r = fx_runs(h->fx, x->clus, x->dlen, (x->flags & F_CONTIG) != 0, add_run, h, &nc);
        x->run = first; x->nrun = h->nruns - first;
        if (r) {
            h->nruns = first;
            x->nrun = 0;
            x->flags |= F_HIDE | F_DROP;            /* a chain that ends early, or too fragmented to keep */
            h->n_broken++;
        }
    }
    return 0;
}

/* ---- AppleDouble: each "._name" with the file or folder "name" beside it --------------------------- */
static uint32_t target_hash(struct hv *h, uint32_t i)
{
    const struct hv_node *x = &h->n[i];
    return x->flags & F_AD ? name_hash(h->names + x->nm + 2, x->nlen - 2u) : name_hash(h->names + x->nm, x->nlen);
}
static int cmp_pair(struct hv *h, uint32_t a, uint32_t b)
{
    const struct hv_node *x = &h->n[a], *y = &h->n[b];
    if (x->parent != y->parent) return x->parent < y->parent ? -1 : 1;
    if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
    return a < b ? -1 : a > b;
}
static void pair(struct hv *h)
{
    uint32_t n = h->nn, *ix = h->alloc(n * 4 + 4);
    if (!ix) { fail(h, "no memory to match AppleDouble files"); return; }
    for (uint32_t i = 0; i < n; i++) {
        ix[i] = i;
        h->n[i].hash = i ? target_hash(h, i) : 0;
    }
    hsort(h, ix + 1, n - 1, cmp_pair);
    for (uint32_t s = 1; s < n; ) {
        uint32_t e = s;
        while (e < n && h->n[ix[e]].parent == h->n[ix[s]].parent && h->n[ix[e]].hash == h->n[ix[s]].hash)
            e++;
        for (uint32_t a = s; a < e; a++) {
            struct hv_node *ad = &h->n[ix[a]];
            if (!(ad->flags & F_AD) || (ad->flags & F_DROP))
                continue;
            for (uint32_t b = s; b < e; b++) {
                struct hv_node *t = &h->n[ix[b]];
                if ((t->flags & (F_AD | F_HIDE)) || t->ad || t->nlen != ad->nlen - 2u ||
                    memcmp(h->names + t->nm, h->names + ad->nm + 2, (size_t)t->nlen * 2))
                    continue;
                t->ad = ix[a]; ad->ad = ix[b];
                break;
            }
        }
        s = e;
    }
    h->release(ix);
}
/* bytes [off, off + len) of the file whose clusters are node f's: 0, or -1 */
static int file_read(struct hv *h, const struct hv_node *f, uint32_t off, uint32_t len, uint8_t *out);
static int ad_step(struct hv *h)
{
    uint32_t r0 = h->fx->read_secs;
    while (h->fx->read_secs - r0 < STEP_SECTORS) {
        if (h->work_i >= h->nn)
            return 1;
        uint32_t ai = h->work_i++;
        struct hv_node *a = &h->n[ai];
        if (!(a->flags & F_AD) || !a->ad || (a->flags & F_DROP))
            continue;
        struct hv_node *t = &h->n[a->ad];
        uint8_t *b = h->adbuf;
        uint32_t have = a->dlen < 1024 ? a->dlen : 1024;
        if (have < 26 || file_read(h, a, 0, have, b))
            continue;
        /* AppleDouble version 2 (and 1), header: magic, version, 16 bytes of filler, the entry count */
        if (be32(b) != 0x00051607u || (be32(b + 4) != 0x00020000u && be32(b + 4) != 0x00010000u))
            continue;
        uint32_t ne = (uint32_t)b[24] << 8 | b[25], fi = 0, fl = 0;
        for (uint32_t k = 0; k < ne && 26 + 12 * k + 12 <= have; k++) {
            const uint8_t *e = b + 26 + 12 * k;
            uint32_t id = be32(e), o = be32(e + 4), l = be32(e + 8);
            if ((uint64_t)o + l > a->dlen)
                continue;
            if (id == 2 && !(t->flags & F_DIR)) { t->roff = o; t->rlen = l; }
            if (id == 9 && l >= 32) { fi = o; fl = 32; }
        }
        if (fl) {
            uint8_t fin[32];
            if (fi + 32 <= have) memcpy(fin, b + fi, 32);
            else if (file_read(h, a, fi, 32, fin)) fl = 0;
            if (fl) { memcpy(t->finfo, fin, 32); t->flags |= F_INFO; }
        }
    }
    return 0;
}

/* ---- names in each folder -------------------------------------------------------------------------- */
static int cmp_name(struct hv *h, uint32_t a, uint32_t b)
{
    const struct hv_node *x = &h->n[a], *y = &h->n[b];
    if (x->parent != y->parent) return x->parent < y->parent ? -1 : 1;
    int r = rel(x->mname, y->mname);
    if (r) return r;
    return a < b ? -1 : a > b;
}
static int visible(const struct hv_node *x) { return !(x->flags & F_HIDE); }
static void make_name(struct hv *h, uint32_t i, unsigned k)
{
    uint8_t full[256];
    struct hv_node *x = &h->n[i];
    uint32_t n = roman_name(h, h->names + x->nm, x->nlen, full);
    if (!n) { full[0] = UNMAPPED; n = 1; }
    hfs_name(full, n, k, x->mname);
    if (k <= 1 && n > 31) h->n_short++;
}
static void names(struct hv *h)
{
    uint32_t n = 0, *ix = h->alloc(h->nn * 4 + 4);
    if (!ix) { fail(h, "no memory to sort the names"); return; }
    for (uint32_t i = 1; i < h->nn; i++)
        if (visible(&h->n[i])) {
            make_name(h, i, 1);
            h->n[i].sfx = 1;
            ix[n++] = i;
        }
    /* names that clash in one folder: the second " #2", the third " #3", in directory order; a name made
       that way may clash again ("a #2" was already there), so round again until none does */
    for (int round = 0; round < 32; round++) {
        uint32_t clashes = 0, saved = h->n_unmapped;
        hsort(h, ix, n, cmp_name);
        for (uint32_t s = 0; s < n; ) {
            uint32_t e = s + 1;
            while (e < n && h->n[ix[e]].parent == h->n[ix[s]].parent && !rel(h->n[ix[e]].mname, h->n[ix[s]].mname))
                e++;
            /* the first in directory order keeps the name: the run is sorted by node number within it */
            for (uint32_t j = s + 1; j < e; j++) {
                struct hv_node *x = &h->n[ix[j]];
                x->sfx = (uint16_t)(x->sfx < 2 ? 2 + (j - s - 1) : x->sfx + (e - s));
                make_name(h, ix[j], x->sfx);
                clashes++;
                if (!round) h->n_renamed++;
            }
            s = e;
        }
        h->n_unmapped = saved;                      /* counted once, in the first conversion */
        if (!clashes) break;
    }
    h->release(ix);
    /* the volume's name: the label, else the folder's own name, at most 27 characters */
    uint8_t full[256];
    uint32_t vl;
    if (h->root_volume && h->fx->label_len) vl = roman_name(h, h->fx->label, (uint32_t)h->fx->label_len, full);
    else if (!h->root_volume && h->root_nlen) vl = roman_name(h, h->root_name, (uint32_t)h->root_nlen, full);
    else { memcpy(full, "Untitled", 8); vl = 8; }
    if (vl > 27) vl = 27;
    h->vname[0] = (char)vl;
    memcpy(h->vname + 1, full, vl);
    for (uint32_t i = 0; i < vl; i++)
        h->vname_c[i] = full[i] >= 32 && full[i] < 127 ? (char)full[i] : '?';
    h->vname_c[vl] = 0;
}

/* ---- the catalog's records ------------------------------------------------------------------------- */
static const uint8_t *rec_name(struct hv *h, uint32_t r)
{
    static const uint8_t empty[1] = { 0 };
    if (r & 1) return empty;                        /* a thread: (folder, "") */
    return (r >> 1) ? h->n[r >> 1].mname : (const uint8_t *)h->vname;
}
static uint32_t rec_parent(struct hv *h, uint32_t r)
{
    uint32_t i = r >> 1;
    if (r & 1) return cnid(i);
    return i ? cnid(h->n[i].parent) : 1u;           /* the root folder's parent is 1 */
}
static int cmp_rec(struct hv *h, uint32_t a, uint32_t b)
{
    uint32_t pa = rec_parent(h, a), pb = rec_parent(h, b);
    if (pa != pb) return pa < pb ? -1 : 1;
    return rel(rec_name(h, a), rec_name(h, b));
}
static uint32_t key_size(struct hv *h, uint32_t r) { return (7u + rec_name(h, r)[0] + 1u) & ~1u; }
static uint32_t rec_size(struct hv *h, uint32_t r)
{
    uint32_t i = r >> 1;
    return key_size(h, r) + ((r & 1) ? 46u : (h->n[i].flags & F_DIR) ? 70u : 102u);
}
static void put_key(struct hv *h, uint32_t r, uint8_t *p, int index)
{
    const uint8_t *nm = rec_name(h, r);
    memset(p, 0, index ? 38 : key_size(h, r));
    p[0] = (uint8_t)(index ? 37 : 6 + nm[0]);       /* index keys are always the longest (HFS's catalog) */
    put32(p + 2, rec_parent(h, r));
    memcpy(p + 6, nm, 1u + nm[0]);
}
static void put_rec(struct hv *h, uint32_t r, uint8_t *p)
{
    uint32_t i = r >> 1;
    const struct hv_node *x = &h->n[i];
    put_key(h, r, p, 0);
    p += key_size(h, r);
    if (r & 1) {                                    /* a folder's thread: its parent and its name */
        memset(p, 0, 46);
        p[0] = 3;
        put32(p + 10, i ? cnid(x->parent) : 1u);
        const uint8_t *nm = i ? x->mname : (const uint8_t *)h->vname;
        memcpy(p + 14, nm, 1u + nm[0]);
        return;
    }
    if (x->flags & F_DIR) {
        memset(p, 0, 70);
        p[0] = 1;
        put16(p + 4, x->valence);
        put32(p + 6, cnid(i));
        put32(p + 10, x->cdate); put32(p + 14, x->mdate);
        memcpy(p + 22, x->finfo, 32);
        return;
    }
    memset(p, 0, 102);
    p[0] = 2;
    memcpy(p + 4, x->finfo, 16);
    put32(p + 20, cnid(i));
    if (x->dlen) {
        put16(p + 24, x->dstart);
        put32(p + 26, x->dlen); put32(p + 30, (uint32_t)x->dblks * h->ablk);
        put16(p + 74, x->dstart); put16(p + 76, x->dblks);
    }
    if (x->rlen) {
        put16(p + 34, x->rstart);
        put32(p + 36, x->rlen); put32(p + 40, (uint32_t)x->rblks * h->ablk);
        put16(p + 86, x->rstart); put16(p + 88, x->rblks);
    }
    put32(p + 44, x->cdate); put32(p + 48, x->mdate);
    memcpy(p + 56, x->finfo + 16, 16);
}
/* how many records from first fit in one 512-byte node: the 14-byte descriptor, the records, and an
   offset of two bytes for each and one for the free space */
static uint32_t leaf_pack(struct hv *h, uint32_t first)
{
    uint32_t used = 14, k = 0;
    while (first + k < h->nrecs) {
        uint32_t s = rec_size(h, h->recs[first + k]);
        if (used + s + 2 * (k + 2) > 512) break;
        used += s; k++;
    }
    return k;
}
static void node_desc(uint8_t *nd, uint32_t flink, uint32_t blink, uint8_t type, uint8_t height, uint32_t nrecs)
{
    memset(nd, 0, 512);
    put32(nd, flink); put32(nd + 4, blink);
    nd[8] = type; nd[9] = height;
    put16(nd + 10, nrecs);
}
static void set_off(uint8_t *nd, uint32_t k, uint32_t off) { put16(nd + 512 - 2 * (k + 1), off); }
static void render_leaf(struct hv *h, uint32_t leaf, uint8_t *nd)
{
    uint32_t first = h->leaf_first[leaf], k = leaf_pack(h, first), off = 14;
    node_desc(nd, leaf + 1 < h->nleaves ? h->leaf0 + leaf + 1 : 0, leaf ? h->leaf0 + leaf - 1 : 0, 0xff, 1, k);
    for (uint32_t j = 0; j < k; j++) {
        set_off(nd, j, off);
        put_rec(h, h->recs[first + j], nd + off);
        off += rec_size(h, h->recs[first + j]);
    }
    set_off(nd, k, off);
}
/* the record list in key order, and the leaves */
static int catalog_records(struct hv *h)
{
    uint32_t n = 0;
    if (h->recs) { h->release(h->recs); h->recs = 0; }
    if (h->leaf_first) { h->release(h->leaf_first); h->leaf_first = 0; }
    h->recs = h->alloc(h->nn * 8 + 8);
    if (!h->recs) return -1;
    for (uint32_t i = 0; i < h->nn; i++) {
        const struct hv_node *x = &h->n[i];
        if (!visible(x)) continue;
        h->recs[n++] = i << 1;
        if (x->flags & F_DIR) h->recs[n++] = i << 1 | 1;
    }
    h->nrecs = n;
    hsort(h, h->recs, n, cmp_rec);
    uint32_t leaves = 0;
    for (uint32_t r = 0; r < n; leaves++) {
        uint32_t k = leaf_pack(h, r);
        if (!k) return -1;
        r += k;
    }
    h->leaf_first = h->alloc(leaves * 4 + 4);
    if (!h->leaf_first) return -1;
    h->nleaves = 0;
    for (uint32_t r = 0; r < n; ) {
        h->leaf_first[h->nleaves++] = r;
        r += leaf_pack(h, r);
    }
    return 0;
}
#define IDX_PER_NODE 11u                    /* (512 - 14 - 2) / (38-byte key + 4-byte pointer + 2-byte offset) */
static uint32_t index_nodes(uint32_t leaves, uint32_t *depth)
{
    uint32_t total = 0, level = leaves;
    *depth = 1;
    while (level > 1) {
        level = (level + IDX_PER_NODE - 1) / IDX_PER_NODE;
        total += level;
        ++*depth;
    }
    return total;
}

/* ---- layout: the allocation block size, then every fork's place --------------------------------- */
static uint32_t blocks_of(uint32_t bytes, uint32_t ablk) { return (uint32_t)(((uint64_t)bytes + ablk - 1) / ablk); }
/* the catalog file's allocation blocks with ablk, and its map nodes */
static uint32_t cat_blocks(struct hv *h, uint32_t ablk, uint32_t *maps)
{
    uint32_t depth, used0 = 1 + h->nleaves + index_nodes(h->nleaves, &depth), m = 0;
    for (;;) {
        uint32_t blks = blocks_of((used0 + m) * 512u, ablk), total = blks * (ablk / 512u);
        /* the header node's map record holds 2048 nodes' bits, each map node 3936 more: its one record is
           bytes 14-505 (492 bytes, what hfsutils requires of a map node: offsets $00E and $1FA) */
        uint32_t need = total <= 2048 ? 0 : (total - 2048 + 3935) / 3936;
        if (need <= m) { *maps = m; return blks; }
        m = need;
    }
}
static uint64_t data_blocks(struct hv *h, uint32_t ablk)
{
    uint64_t s = 0;
    for (uint32_t i = 1; i < h->nn; i++) {
        const struct hv_node *x = &h->n[i];
        if (!visible(x) || (x->flags & F_DIR)) continue;
        s += blocks_of(x->dlen, ablk) + blocks_of(x->rlen, ablk);
    }
    return s;
}
/* the largest first; of two the same size, the one found later */
static int cmp_size(struct hv *h, uint32_t a, uint32_t b)
{
    uint64_t x = (uint64_t)h->n[a].dlen + h->n[a].rlen, y = (uint64_t)h->n[b].dlen + h->n[b].rlen;
    if (x != y) return x > y ? -1 : 1;
    return a > b ? -1 : a < b;
}
#define VOL_MAX_SECTORS 4194304u            /* 2 GB: System 7.1's largest HFS volume */
static int fits(struct hv *h, uint32_t ablk, uint64_t data)
{
    uint32_t maps, ct = cat_blocks(h, ablk, &maps), xt = 1;
    uint64_t nab = data + ct + xt;
    if (nab > 65535) return 0;
    uint32_t bm = (uint32_t)((nab + 4095) / 4096);
    return 3 + bm + nab * (ablk / 512u) + 2 <= VOL_MAX_SECTORS;
}
static int layout(struct hv *h)
{
    if (catalog_records(h)) { fail(h, "no memory for the catalog"); return -1; }
    uint32_t ablk = 0;
    for (uint32_t k = 1; k <= 64 && !ablk; k++)
        if (fits(h, 512u * k, data_blocks(h, 512u * k)))
            ablk = 512u * k;
    if (!ablk) {
        /* too much for one HFS volume: the largest files are left out until the rest fits (the most files
           stay: a stick with films and Mac programs keeps the programs), the later of two the same size */
        ablk = 64u * 512u;
        uint64_t d = data_blocks(h, ablk);
        uint32_t n = 0, *ix = h->alloc(h->nn * 4 + 4);
        if (!ix) { fail(h, "no memory to choose what fits"); return -1; }
        for (uint32_t i = 1; i < h->nn; i++)
            if (visible(&h->n[i]) && !(h->n[i].flags & F_DIR)) ix[n++] = i;
        hsort(h, ix, n, cmp_size);
        for (uint32_t j = 0; j < n && !fits(h, ablk, d); j++) {
            struct hv_node *x = &h->n[ix[j]];
            d -= blocks_of(x->dlen, ablk) + blocks_of(x->rlen, ablk);
            x->flags |= F_HIDE | F_DROP;
            h->n_dropped++;
            h->bytes_dropped += (uint64_t)x->dlen + x->rlen;
        }
        h->release(ix);
        if (catalog_records(h)) { fail(h, "no memory for the catalog"); return -1; }
        /* the catalog only shrank; the smallest block size that holds what is left */
        for (uint32_t k = 1; k <= 64; k++)
            if (fits(h, 512u * k, data_blocks(h, 512u * k))) { ablk = 512u * k; break; }
    }
    h->ablk = ablk; h->spa = ablk / 512u;
    h->xt_start = 0; h->xt_blks = 1;
    h->ct_start = 1; h->ct_blks = cat_blocks(h, ablk, &h->map_nodes);
    /* the forks, in node order: data then resource */
    uint32_t nf = 2;
    for (uint32_t i = 1; i < h->nn; i++) {
        const struct hv_node *x = &h->n[i];
        if (visible(x) && !(x->flags & F_DIR)) nf += (x->dlen ? 1 : 0) + (x->rlen ? 1 : 0);
    }
    h->forks = h->alloc(nf * sizeof *h->forks);
    if (!h->forks) { fail(h, "no memory for the fork map"); return -1; }
    h->forks[0] = (struct hv_fork){ h->xt_start, h->xt_blks, 0, 2 };
    h->forks[1] = (struct hv_fork){ h->ct_start, h->ct_blks, 0, 3 };
    uint32_t ab = h->ct_start + h->ct_blks;
    h->nforks = 2;
    h->files_all = h->dirs_all = h->files_root = h->dirs_root = 0;
    for (uint32_t i = 0; i < h->nn; i++) h->n[i].valence = 0;
    for (uint32_t i = 1; i < h->nn; i++) {
        struct hv_node *x = &h->n[i];
        if (!visible(x)) continue;
        h->n[x->parent].valence++;
        if (x->flags & F_DIR) { h->dirs_all++; if (!x->parent) h->dirs_root++; continue; }
        h->files_all++; if (!x->parent) h->files_root++;
        if (x->dlen) {
            x->dstart = (uint16_t)ab; x->dblks = (uint16_t)blocks_of(x->dlen, ablk);
            h->forks[h->nforks++] = (struct hv_fork){ ab, x->dblks, i, 0 };
            ab += x->dblks;
        }
        if (x->rlen) {
            x->rstart = (uint16_t)ab; x->rblks = (uint16_t)blocks_of(x->rlen, ablk);
            h->forks[h->nforks++] = (struct hv_fork){ ab, x->rblks, i, 1 };
            ab += x->rblks;
        }
    }
    /* at least 800 KB, a double-sided floppy's size: hfsutils refuses smaller volumes, and a folder with
       a few small files would make one; the blocks beyond the files are free */
    h->used_ablks = ab;
    h->nablks = ab;
    if (3 + 1 + ab * h->spa + 2 < 1600) h->nablks = (1600 - 6 + h->spa - 1) / h->spa;
    h->bmsecs = (h->nablks + 4095) / 4096;
    h->alblst = 3 + h->bmsecs;
    h->total = h->alblst + h->nablks * h->spa + 2;
    return 0;
}

/* ---- the catalog's header, map and index nodes, and the master directory block --------------------- */
static uint8_t *fixed_node(struct hv *h, uint32_t k)
{
    if (k < h->leaf0) return h->fixed + 512u * k;
    if (k >= h->leaf0 + h->nleaves && k < h->cat_used) return h->fixed + 512u * (h->leaf0 + k - h->leaf0 - h->nleaves);
    return 0;
}
static void header_node(uint8_t *nd, uint32_t depth, uint32_t root, uint32_t nrecs, uint32_t first, uint32_t last,
                        uint32_t keylen, uint32_t nodes, uint32_t used, uint32_t maps0)
{
    node_desc(nd, maps0, 0, 1, 0, 3);
    uint8_t *r = nd + 14;
    put16(r, depth); put32(r + 2, root); put32(r + 6, nrecs); put32(r + 10, first); put32(r + 14, last);
    put16(r + 18, 512); put16(r + 20, keylen); put32(r + 22, nodes); put32(r + 26, nodes - used);
    set_off(nd, 0, 14); set_off(nd, 1, 120); set_off(nd, 2, 248); set_off(nd, 3, 504);
    /* the map record (bytes 248-503): a bit per node, the first nodes in use */
    for (uint32_t i = 0; i < used && i < 2048; i++) nd[248 + i / 8] |= (uint8_t)(0x80u >> (i % 8));
}
static int render_fixed(struct hv *h)
{
    uint32_t depth, ni = index_nodes(h->nleaves, &depth);
    h->leaf0 = 1 + h->map_nodes;
    h->cat_used = h->leaf0 + h->nleaves + ni;
    h->cat_nodes = h->ct_blks * h->spa;
    h->nfixed = h->leaf0 + ni;
    h->fixed = h->alloc(h->nfixed * 512u);
    if (!h->fixed) return -1;
    memset(h->fixed, 0, h->nfixed * 512u);
    /* the index levels, bottom up: each level's nodes, their first records */
    uint32_t level_n = h->nleaves, next = h->leaf0 + h->nleaves;
    uint32_t *kid = h->alloc(level_n * 8 + 8);
    if (!kid) return -1;
    uint32_t *first = kid + level_n;
    for (uint32_t i = 0; i < level_n; i++) { kid[i] = h->leaf0 + i; first[i] = h->recs[h->leaf_first[i]]; }
    uint32_t root = h->leaf0, height = 1;
    while (level_n > 1) {
        uint32_t up = (level_n + IDX_PER_NODE - 1) / IDX_PER_NODE, base = next;
        height++;
        for (uint32_t u = 0; u < up; u++) {
            uint8_t *nd = fixed_node(h, base + u);
            uint32_t s = u * IDX_PER_NODE, k = level_n - s < IDX_PER_NODE ? level_n - s : IDX_PER_NODE, off = 14;
            node_desc(nd, u + 1 < up ? base + u + 1 : 0, u ? base + u - 1 : 0, 0, (uint8_t)height, k);
            for (uint32_t j = 0; j < k; j++) {
                set_off(nd, j, off);
                put_key(h, first[s + j], nd + off, 1);
                put32(nd + off + 38, kid[s + j]);
                off += 42;
            }
            set_off(nd, k, off);
        }
        for (uint32_t u = 0; u < up; u++) { kid[u] = base + u; first[u] = first[u * IDX_PER_NODE]; }
        level_n = up; next = base + up; root = base;
    }
    h->release(kid);
    h->depth = height; h->root_node = root;
    header_node(h->fixed, height, root, h->nrecs, h->leaf0, h->leaf0 + h->nleaves - 1, 37, h->cat_nodes, h->cat_used, h->map_nodes ? 1 : 0);
    /* map nodes for the nodes past the header's 2048 */
    for (uint32_t m = 0; m < h->map_nodes; m++) {
        uint8_t *nd = fixed_node(h, 1 + m);
        node_desc(nd, m + 1 < h->map_nodes ? 2 + m : 0, 0, 2, 0, 1);
        set_off(nd, 0, 14); set_off(nd, 1, 506);
        uint32_t b0 = 2048 + m * 3936;
        for (uint32_t i = b0; i < h->cat_used && i < b0 + 3936; i++) nd[14 + (i - b0) / 8] |= (uint8_t)(0x80u >> ((i - b0) % 8));
    }
    /* the master directory block */
    uint8_t *m = h->mdb;
    memset(m, 0, 512);
    put16(m, 0x4244);                               /* 'BD' */
    put32(m + 2, h->cdate); put32(m + 6, h->mdate);
    put16(m + 10, 0x8100);                          /* locked by software (bit 15), unmounted cleanly (bit 8) */
    put16(m + 12, h->files_root);
    put16(m + 14, 3);                               /* the volume bitmap's first block */
    put16(m + 18, h->nablks);
    put32(m + 20, h->ablk); put32(m + 24, h->ablk * 4u);
    put16(m + 28, h->alblst);
    put32(m + 30, cnid(h->nn));                     /* the next catalog node ID */
    put16(m + 34, h->nablks - h->used_ablks);       /* free blocks */
    memcpy(m + 36, h->vname, 1u + (uint8_t)h->vname[0]);
    put32(m + 70, 1);
    put32(m + 74, h->xt_blks * h->ablk); put32(m + 78, h->ct_blks * h->ablk);
    put16(m + 82, h->dirs_root); put32(m + 84, h->files_all); put32(m + 88, h->dirs_all);
    put32(m + 130, h->xt_blks * h->ablk); put16(m + 134, h->xt_start); put16(m + 136, h->xt_blks);
    put32(m + 146, h->ct_blks * h->ablk); put16(m + 150, h->ct_start); put16(m + 152, h->ct_blks);
    return 0;
}

/* ---- the Finder's information for files that came without any ------------------------------------- */
static void default_info(struct hv *h, uint32_t i)
{
    static const struct { const char *ext, *type, *creator; } kinds[] = {
        { "txt", "TEXT", "ttxt" }, { "text", "TEXT", "ttxt" }, { "md", "TEXT", "ttxt" }, { "c", "TEXT", "ttxt" },
        { "h", "TEXT", "ttxt" }, { "s", "TEXT", "ttxt" }, { "log", "TEXT", "ttxt" }, { "csv", "TEXT", "ttxt" },
        { "ini", "TEXT", "ttxt" }, { "cfg", "TEXT", "ttxt" }, { "html", "TEXT", "ttxt" }, { "htm", "TEXT", "ttxt" },
        { "hqx", "TEXT", "SITx" }, { "sit", "SIT!", "SITx" }, { "sea", "APPL", "????" }, { "cpt", "PACT", "CPCT" },
        { "pict", "PICT", "ttxt" }, { "pct", "PICT", "ttxt" }, { "gif", "GIFf", "ogle" }, { "jpg", "JPEG", "ogle" },
        { "jpeg", "JPEG", "ogle" }, { "mp3", "MPG3", "PIPY" }, { "aif", "AIFF", "PIPY" }, { "aiff", "AIFF", "PIPY" },
        { "wav", "WAVE", "PIPY" }, { "pdf", "PDF ", "CARO" }, { "zip", "ZIP ", "SITx" },
    };
    struct hv_node *x = &h->n[i];
    const uint16_t *s = h->names + x->nm;
    uint32_t n = x->nlen, dot = n;
    for (uint32_t k = n; k > 1; k--) if (s[k - 1] == '.') { dot = k - 1; break; }
    if (dot == n || n - dot > 5) return;
    for (unsigned k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        const char *e = kinds[k].ext;
        uint32_t j = 0;
        while (e[j] && dot + 1 + j < n && (s[dot + 1 + j] | 0x20) == (uint8_t)e[j]) j++;
        if (!e[j] && dot + 1 + j == n) {
            memcpy(x->finfo, kinds[k].type, 4); memcpy(x->finfo + 4, kinds[k].creator, 4);
            return;
        }
    }
}
/* Finder flags that would be wrong here: on the desktop (kIsOnDesk, $0001), and the place the Finder
   gave an icon on another Mac (kHasBeenInited, $0100, with fdLocation): this Finder places the icons */
static void tidy_info(struct hv *h)
{
    for (uint32_t i = 1; i < h->nn; i++) {
        struct hv_node *x = &h->n[i];
        if (!visible(x)) continue;
        if (!(x->flags & F_DIR) && !(x->flags & F_INFO)) default_info(h, i);
        uint8_t *f = x->finfo + 8;                  /* fdFlags, frFlags */
        uint32_t fl = (uint32_t)f[0] << 8 | f[1];
        fl &= ~0x0101u;
        if (x->flags & F_INVIS) fl |= 0x4000u;      /* kIsInvisible */
        put16(f, fl);
        memset(x->finfo + 10, 0, 4);                /* fdLocation / frLocation */
        if (!(x->flags & F_DIR)) put16(x->finfo + 14, 0);    /* fdFldr */
        if ((x->flags & F_DIR) && !(x->flags & F_INFO)) {
            /* a window that fits the SE's 512 by 342 screen: top 60, left 30, bottom 300, right 400 */
            put16(x->finfo, 60); put16(x->finfo + 2, 30); put16(x->finfo + 4, 300); put16(x->finfo + 6, 400);
        }
    }
}

/* ---- the steps ------------------------------------------------------------------------------------- */
static int start(struct hv *h)
{
    h->dir = h->alloc(sizeof *h->dir);
    h->ent = h->alloc(sizeof *h->ent);
    h->scratch = h->alloc(HV_SCRATCH);
    if (!h->dir || !h->ent || !h->scratch || grow(h, (void **)&h->n, &h->ncap, 1, sizeof *h->n, HV_MAX_NODES)) {
        fail(h, "no memory to start");
        return -1;
    }
    struct hv_node *r = &h->n[0];
    memset(r, 0, sizeof *r);
    r->flags = F_DIR;
    r->clus = h->root_clus; r->dlen = h->root_size;
    if (h->root_contig) r->flags |= F_CONTIG;
    h->nn = 1;
    return 0;
}
int hv_step(struct hv *h)
{
    if (h->failed) return -1;
    switch (h->phase) {
    case P_SCAN:
        if (!h->nn && start(h)) return -1;
        if (scan_step(h)) { h->phase = P_RUNS; h->work_i = 0; }
        if (h->fx->errors > 64) { fail(h, "the medium gives read errors: given up"); return -1; }
        return 0;
    case P_RUNS:
        if (runs_step(h)) h->phase = P_PAIR;
        return 0;
    case P_PAIR:
        pair(h);
        h->phase = P_AD; h->work_i = 0;
        return h->failed ? -1 : 0;
    case P_AD:
        if (ad_step(h)) h->phase = P_NAMES;
        return 0;
    case P_NAMES:
        names(h);
        tidy_info(h);
        h->phase = P_LAYOUT;
        return h->failed ? -1 : 0;
    case P_LAYOUT: {
        uint32_t newest = 0, oldest = 0xffffffffu;
        for (uint32_t i = 1; i < h->nn; i++) {
            if (h->n[i].mdate > newest) newest = h->n[i].mdate;
            if (h->n[i].cdate && h->n[i].cdate < oldest) oldest = h->n[i].cdate;
        }
        if (!newest) newest = 0xe1c5e000u;          /* 2024: a medium with no dates at all */
        if (oldest > newest) oldest = newest;
        h->cdate = oldest; h->mdate = newest;
        if (!h->n[0].mdate) { h->n[0].cdate = oldest; h->n[0].mdate = newest; }
        if (!(h->n[0].flags & F_INFO)) {
            put16(h->n[0].finfo, 60); put16(h->n[0].finfo + 2, 30); put16(h->n[0].finfo + 4, 300); put16(h->n[0].finfo + 6, 400);
        }
        if (layout(h) || render_fixed(h)) { if (!h->failed) fail(h, "no memory for the catalog"); return -1; }
        if (h->names) { h->release(h->names); h->names = 0; h->namecap = h->nnames = 0; }
        if (h->dir) { h->release(h->dir); h->dir = 0; }
        if (h->ent) { h->release(h->ent); h->ent = 0; }
        h->mem = h->ncap * (uint32_t)sizeof *h->n + h->rcap * (uint32_t)sizeof *h->runs + h->nrecs * 4 + h->nleaves * 4 +
                 h->nfixed * 512 + h->nforks * (uint32_t)sizeof *h->forks + HV_SCRATCH + (uint32_t)sizeof *h;
        h->phase = P_DONE;
        return 1;
    }
    default:
        return 1;
    }
}

/* ---- reading the volume ---------------------------------------------------------------------------- */
/* bytes [off, off + len) of the file whose clusters are node f's; nothing past its valid length is read */
static int file_read(struct hv *h, const struct hv_node *f, uint32_t off, uint32_t len, uint8_t *out)
{
    struct fx_vol *v = h->fx;
    uint32_t csz = v->spc * 512u, valid = f->dvalid < f->dlen ? f->dvalid : f->dlen;
    while (len) {
        if (off >= valid) { memset(out, 0, len); return 0; }
        uint32_t want = len < valid - off ? len : valid - off;
        /* the run that holds cluster off / csz */
        uint32_t ci = off / csz, k = 0, lba = 0, avail = 0;
        for (k = 0; k < f->nrun; k++) {
            const struct hv_run *r = &h->runs[f->run + k];
            if (ci < r->n) {
                uint32_t in = off % csz;
                lba = fx_clus_lba(v, r->clus + ci) + in / 512u;
                avail = (r->n - ci) * csz - in;     /* bytes contiguous on the medium from off */
                break;
            }
            ci -= r->n;
        }
        if (k == f->nrun) return -1;
        if (want > avail) want = avail;
        uint32_t shift = off % 512u;
        if (!shift && want >= 512 && !((uintptr_t)out & 63)) {
            uint32_t secs = want / 512u;
            if (v->read(v->ctx, lba, secs, out)) return -1;
            out += secs * 512u; off += secs * 512u; len -= secs * 512u;
            continue;
        }
        /* through the scratch: a resource fork at an odd offset of its AppleDouble file, or a tail */
        uint32_t secs = (shift + want + 511u) / 512u;
        if (secs > HV_SCRATCH / 512u) secs = HV_SCRATCH / 512u;
        if (v->read(v->ctx, lba, secs, h->scratch)) return -1;
        uint32_t got = secs * 512u - shift;
        if (got > want) got = want;
        memcpy(out, h->scratch + shift, got);
        out += got; off += got; len -= got;
    }
    return 0;
}
static void extents_node(struct hv *h, uint32_t k, uint8_t *nd)
{
    if (k) { memset(nd, 0, 512); return; }
    header_node(nd, 0, 0, 0, 0, 0, 7, h->xt_blks * h->spa, 1, 0);
}
int hv_read(struct hv *h, uint32_t lba, uint32_t n, uint8_t *out)
{
    if (h->phase != P_DONE) return -1;
    while (n) {
        if (lba < h->alblst || lba >= h->alblst + h->nablks * h->spa) {
            memset(out, 0, 512);
            if (lba == 2 || lba == h->total - 2) memcpy(out, h->mdb, 512);
            else if (lba >= 3 && lba < 3 + h->bmsecs) {
                uint32_t b0 = (lba - 3) * 4096;     /* the blocks of the files, the catalog and the extents */
                for (uint32_t b = b0; b < b0 + 4096 && b < h->used_ablks; b++) out[(b - b0) / 8] |= (uint8_t)(0x80u >> (b % 8));
            }
            out += 512; lba++; n--;
            continue;
        }
        uint32_t rel_s = lba - h->alblst, ab = rel_s / h->spa;
        uint32_t lo = 0, hi = h->nforks;            /* the fork holding allocation block ab */
        while (lo + 1 < hi) { uint32_t mid = (lo + hi) / 2; if (h->forks[mid].start <= ab) lo = mid; else hi = mid; }
        const struct hv_fork *f = &h->forks[lo];
        if (ab < f->start || ab >= f->start + f->blks) { memset(out, 0, 512); out += 512; lba++; n--; continue; }
        uint32_t fsec = rel_s - f->start * h->spa, fend = f->blks * h->spa;
        if (f->which >= 2) {
            if (f->which == 2) extents_node(h, fsec, out);
            else {
                uint8_t *fx = fixed_node(h, fsec);
                if (fx) memcpy(out, fx, 512);
                else if (fsec >= h->leaf0 && fsec < h->leaf0 + h->nleaves) render_leaf(h, fsec - h->leaf0, out);
                else memset(out, 0, 512);
            }
            out += 512; lba++; n--;
            continue;
        }
        /* a fork's data: as many blocks as stay inside it, in one go */
        uint32_t m = fend - fsec < n ? fend - fsec : n;
        const struct hv_node *x = &h->n[f->node];
        uint32_t flen = f->which ? x->rlen : x->dlen, off = fsec * 512u, bytes = m * 512u, real = 0;
        if (off < flen) real = flen - off < bytes ? flen - off : bytes;
        if (real) {
            int r;
            if (f->which) {                         /* the resource fork: inside the AppleDouble file */
                const struct hv_node *a = &h->n[x->ad];
                r = file_read(h, a, x->roff + off, real, out);
            } else
                r = file_read(h, x, off, real, out);
            if (r) return -1;
        }
        memset(out + real, 0, bytes - real);
        out += bytes; lba += m; n -= m;
    }
    return 0;
}
void hv_release(struct hv *h)
{
    void *p[] = { h->n, h->runs, h->names, h->dir, h->ent, h->forks, h->recs, h->leaf_first, h->fixed, h->scratch };
    for (unsigned i = 0; i < sizeof p / sizeof p[0]; i++)
        if (p[i]) h->release(p[i]);
    h->n = 0; h->runs = 0; h->names = 0; h->dir = 0; h->ent = 0; h->forks = 0; h->recs = 0; h->leaf_first = 0; h->fixed = 0; h->scratch = 0;
    h->phase = P_DONE + 1;
}

/* the log's account of a view that is ready: what is in it, what was left out and why, its size, memory */
void hv_report(struct hv *h, uint32_t ms)
{
    uint32_t mac = 0;
    for (uint32_t i = 1; i < h->nn; i++)
        if (visible(&h->n[i]) && (h->n[i].flags & F_INFO)) mac++;
    h->log("%s: ready as the HFS volume \"%s\": %u files, %u folders, %u of them with Mac information from AppleDouble files; "
           "%u blocks (%u MB), allocation blocks of %u bytes; locked; %u KB of memory; made in %u ms (%u reads)\n",
           h->tag, h->vname_c, h->files_all, h->dirs_all, mac, h->total, h->total / 2048, h->ablk, h->mem / 1024, ms, h->fx->reads);
    if (h->n_short || h->n_renamed || h->n_unmapped)
        h->log("%s: names: %u longer than 31 characters shortened, %u renamed \" #2\"... where two would clash, %u characters Mac Roman lacks shown as '?'\n",
               h->tag, h->n_short, h->n_renamed, h->n_unmapped);
    if (h->n_dropped)
        h->log("%s: more than one HFS volume holds (2 GB on System 7.1): %u files (%u MB) are not shown, the largest first\n",
               h->tag, h->n_dropped, (uint32_t)(h->bytes_dropped >> 20));
    if (h->n_big)
        h->log("%s: %u files of 2 GB or more are not shown (HFS on System 7.1 holds none that large)\n", h->tag, h->n_big);
    if (h->n_full)
        h->log("%s: only the first %u files and folders are shown (%u more found)\n", h->tag, HV_MAX_NODES, h->n_full);
    if (h->n_broken)
        h->log("%s: %u files or folders whose clusters could not be followed are not shown\n", h->tag, h->n_broken);
}
