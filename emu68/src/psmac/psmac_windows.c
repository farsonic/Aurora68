/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_windows.c - the Mac's windows, read by the kernel out of the Window Manager's own records, for
 * the remote control's window list and window screenshots (psmac_remote.c, docs/13 "Windows").
 *
 * Backstory. On 21 November 2026 the user asked for a list of the Mac's windows and a screenshot of the
 * one they choose: they photograph the Mac's screens to show results, and a picture of one window is
 * what they want to show. The kernel can read the Mac's RAM directly (it is the Mac's processor), so the
 * list needs no program on the Mac and works under any system: the Window Manager keeps every window
 * as a WindowRecord in a heap, linked front to back from the low-memory global WindowList, and every
 * field the list needs is in the record (Inside Macintosh I, "The Window Manager", "Window Records").
 *
 * System 7's layers. Under System 7 WindowList holds the windows of one application only, the one whose
 * turn it is: the Window Manager keeps each application's windows in a "layer" of their own, and the
 * Process Manager makes an application's layer the current one when it switches to it (System 6's
 * MultiFinder, without layers, swapped WindowList itself). Layers are not in Inside Macintosh; what
 * follows was found on 21 November 2026 in a RAM dump of System 7.1 with three applications running
 * (the Finder, PiLog, the Calculator in the DA Handler), and is checked at every step before it is
 * believed:
 *   - a layer is a WindowRecord. Its windowDefProc is the layer's (one for all layers), its windowPic
 *     field (148) points to the first window inside it, its dataHandle field (130) to the layer it is
 *     in, 0 for the root, and its strucRgn is the union of what is inside;
 *   - the low-memory long at $A90 is the current layer, whose first window is WindowList;
 *   - the root holds one layer, which holds the applications' layers front to back;
 *   - a layer's refCon is the Process Manager's record of its application (its file type and creator
 *     at 4 and 8, the layer again at $70); the long at $54 there is a handle to the block where the
 *     Process Manager keeps that application's own low memory while another runs: CurApName at $38 and
 *     the layer once more at $58.
 * So the kernel walks the tree from the root down, front to back, and names each layer's windows by its
 * application: the current one by CurApName, the others by their saved CurApName, each used only when
 * both back links to the layer agree. Should any check fail (System 6, a later system that differs, a
 * moment when the tree is being changed) it falls back to WindowList alone, which is always right for
 * the application in front of the Process Manager at that instant.
 *
 * Everything is read while the 68k runs on core 0 and may be changing the list: a window being closed,
 * a heap being compacted, a layer being switched. So nothing read is trusted. Every pointer is checked
 * to be even and inside the Mac's RAM before it is followed, every handle's master pointer likewise,
 * every count and loop has a bound, every region and rectangle is checked to be sane, and the list is
 * read twice: if the two readings differ (the Mac changed it meanwhile) it is read again, up to four
 * times, and the answer says whether it was consistent. Reads go through psmac_mac_rd32, which
 * translates by the 68k's current MMU tables and refuses anything that is not RAM or ROM, so even a
 * wrong pointer that passed the checks cannot read outside the Mac's memory.
 */
#include <stdint.h>
#include "support.h"
#include "psmac.h"
#include "psmac_windows.h"

uint32_t psmac_mac_rd32(uint32_t logical, int *ok);                         /* psmac_bus.c */

/* ---- low-memory globals (Inside Macintosh I-III, "Summary" sections; IM VI for System 7) ---- */
#define LM_CURAPNAME  0x910u      /* CurApName: the current application's name, a Pascal string of up to 31 (IM II-58) */
#define LM_WINDOWLIST 0x9d6u      /* WindowList: the frontmost window (IM I-299) */
#define LM_MMU32BIT   0xcb2u      /* MMU32Bit: nonzero while the 68k runs in 32-bit mode (IM V-592) */
#define LM_CURLAYER   0xa90u      /* System 7: the current layer (not documented; found 21 November 2026, see above) */

/* ---- the WindowRecord (IM I-276, "Window Records"): a GrafPort of 108 bytes, then ---- */
#define WR_PORTRECT   16          /* the GrafPort's portRect (and a CGrafPort's, IM V-49) */
#define WR_KIND       108         /* windowKind: 2 dialogKind, 8 userKind, negative a desk accessory's refNum */
#define WR_VISIBLE    110         /* visible (Boolean) */
#define WR_HILITED    111         /* hilited */
#define WR_STRUCRGN   114         /* strucRgn: the frame, title bar and content, global coordinates */
#define WR_CONTRGN    118         /* contRgn: the content */
#define WR_UPDATERGN  122         /* updateRgn: what the application has still to draw (the remote's front=1 waits for it) */
#define WR_DEFPROC    126         /* windowDefProc */
#define WR_DATA       130         /* dataHandle; a layer's parent layer */
#define WR_TITLE      134         /* titleHandle: a StringHandle */
#define WR_NEXT       144         /* nextWindow: the window behind */
#define WR_PIC        148         /* windowPic; a layer's first window */
#define WR_REFCON     152
#define WR_SIZE       156
/* a region (IM I-141): rgnSize (16 bits), then rgnBBox: top, left, bottom, right */
#define RGN_BBOX      2

/* the Process Manager's record of an application (System 7.1, found as above) */
#define PE_LAYER      0x70        /* its layer */
#define PE_PCB        0x54        /* a handle to its saved low memory */
#define PCB_APNAME    0x38        /* its CurApName there */
#define PCB_LAYER     0x58        /* its layer there */

#define MAX_PER_LIST  64          /* windows in one list (a layer): beyond this the list is taken to be corrupt or looping */
#define MAX_DEPTH     6           /* layers in layers: the root, the applications' layer, the applications' */
#define MAX_LAYERS    32

static uint32_t mask, ram_top;    /* the 24-bit mask in 24-bit mode; the end of RAM */

/* a long anywhere even inside RAM, from aligned reads only (a long at an address that is 2 mod 4 is
   two halves: the 68k allows it, a page boundary may lie between them) */
static int mrd32(uint32_t a, uint32_t *v)
{
    int ok, ok2;
    if ((a & 1) || a > ram_top - 4) return 0;
    if (!(a & 2)) { *v = psmac_mac_rd32(a, &ok); return ok; }
    uint32_t x = psmac_mac_rd32(a - 2, &ok), y = psmac_mac_rd32(a + 2, &ok2);
    *v = x << 16 | y >> 16;
    return ok && ok2;
}
static int mrd16(uint32_t a, int16_t *v)
{
    int ok;
    if ((a & 1) || a > ram_top - 2) return 0;
    uint32_t x = psmac_mac_rd32(a & ~3u, &ok);
    *v = (int16_t)((a & 2) ? x : x >> 16);
    return ok;
}
static int mrd8(uint32_t a, uint8_t *v)
{
    int ok;
    if (a > ram_top - 1) return 0;
    uint32_t x = psmac_mac_rd32(a & ~3u, &ok);
    *v = (uint8_t)(x >> (24 - 8 * (a & 3)));
    return ok;
}
/* a pointer worth following to a record of size bytes: even, above the vectors and inside RAM */
static int ptr_ok(uint32_t p, uint32_t size) { return p && !(p & 1) && p >= 0x100 && p < ram_top && size <= ram_top - p; }
/* a handle's master pointer, masked; 0 if the handle is not one or its block is purged (a purged
   handle's master pointer is NIL, IM II-14; in 24-bit mode its top byte holds the lock, purge and
   resource bits, IM II-25, which the mask removes) */
static uint32_t deref(uint32_t h, uint32_t size)
{
    uint32_t mp;
    h &= mask;
    if (!ptr_ok(h, 4) || !mrd32(h, &mp)) return 0;
    mp &= mask;
    return ptr_ok(mp, size) ? mp : 0;
}
/* a region's bounding box as left, top, right, bottom; 0 if the handle or the region is not sane */
static int region_box(uint32_t rh, int16_t r[4])
{
    uint32_t p = deref(rh, 10);
    int16_t size, t, l, b, rr;
    if (!p || !mrd16(p, &size) || !mrd16(p + RGN_BBOX, &t) || !mrd16(p + RGN_BBOX + 2, &l) ||
        !mrd16(p + RGN_BBOX + 4, &b) || !mrd16(p + RGN_BBOX + 6, &rr)) return 0;
    if (size < 10 || (size & 1) || l > rr || t > b) return 0;      /* a region is 10 bytes at least, even */
    r[0] = l; r[1] = t; r[2] = rr; r[3] = b;
    return 1;
}

/* one window's record into w; 0 if it does not look like a window */
static int read_window(uint32_t p, struct psmac_window *w)
{
    uint8_t vis, hil, len;
    uint32_t th, sh, ch;
    if (!ptr_ok(p, WR_SIZE) || !mrd16(p + WR_KIND, &w->kind) || !mrd8(p + WR_VISIBLE, &vis) || !mrd8(p + WR_HILITED, &hil)) return 0;
    /* visible and hilited are Booleans: anything but 0 is true (System 7.1 keeps hilited as $FF) */
    w->ptr = p; w->visible = vis != 0; w->hilited = hil != 0; w->app = -1;
    w->has_rect = mrd32(p + WR_STRUCRGN, &sh) && region_box(sh, w->rect);
    w->has_content = mrd32(p + WR_CONTRGN, &ch) && region_box(ch, w->content);
    { uint32_t uh; int16_t u[4];                                  /* an empty update region: drawn */
      w->update_empty = !(mrd32(p + WR_UPDATERGN, &uh) && region_box(uh, u)) || u[0] >= u[2] || u[1] >= u[3]; }
    w->title_len = 0;
    uint32_t ts = mrd32(p + WR_TITLE, &th) && th ? deref(th, 1) : 0;
    if (ts && mrd8(ts, &len) && ptr_ok(ts, 1u + len)) {
        for (unsigned i = 0; i < len; i++)
            if (!mrd8(ts + 1 + i, &w->title[i])) { len = (uint8_t)i; break; }
        w->title_len = len;
    }
    return 1;
}

/* ---- Mac Roman ---- */
/* The upper half of Mac Roman as Unicode, the System 7 era's table: $DB is the currency sign, U+00A4 (it
   became the euro sign with Mac OS 8.5), $F0 the Apple logo, which Unicode has only in the private use
   area (U+F8FF, as Apple's own tables map it). */
static const uint16_t mac_roman_hi[128] = {
    0x00c4, 0x00c5, 0x00c7, 0x00c9, 0x00d1, 0x00d6, 0x00dc, 0x00e1, 0x00e0, 0x00e2, 0x00e4, 0x00e3, 0x00e5, 0x00e7, 0x00e9, 0x00e8,
    0x00ea, 0x00eb, 0x00ed, 0x00ec, 0x00ee, 0x00ef, 0x00f1, 0x00f3, 0x00f2, 0x00f4, 0x00f6, 0x00f5, 0x00fa, 0x00f9, 0x00fb, 0x00fc,
    0x2020, 0x00b0, 0x00a2, 0x00a3, 0x00a7, 0x2022, 0x00b6, 0x00df, 0x00ae, 0x00a9, 0x2122, 0x00b4, 0x00a8, 0x2260, 0x00c6, 0x00d8,
    0x221e, 0x00b1, 0x2264, 0x2265, 0x00a5, 0x00b5, 0x2202, 0x2211, 0x220f, 0x03c0, 0x222b, 0x00aa, 0x00ba, 0x03a9, 0x00e6, 0x00f8,
    0x00bf, 0x00a1, 0x00ac, 0x221a, 0x0192, 0x2248, 0x2206, 0x00ab, 0x00bb, 0x2026, 0x00a0, 0x00c0, 0x00c3, 0x00d5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201c, 0x201d, 0x2018, 0x2019, 0x00f7, 0x25ca, 0x00ff, 0x0178, 0x2044, 0x00a4, 0x2039, 0x203a, 0xfb01, 0xfb02,
    0x2021, 0x00b7, 0x201a, 0x201e, 0x2030, 0x00c2, 0x00ca, 0x00c1, 0x00cb, 0x00c8, 0x00cd, 0x00ce, 0x00cf, 0x00cc, 0x00d3, 0x00d4,
    0xf8ff, 0x00d2, 0x00da, 0x00db, 0x00d9, 0x0131, 0x02c6, 0x02dc, 0x00af, 0x02d8, 0x02d9, 0x02da, 0x00b8, 0x02dd, 0x02db, 0x02c7,
};
unsigned psmac_macroman_utf8(const uint8_t *in, unsigned n, char *out, unsigned max, int json)
{
    static const char hex[] = "0123456789abcdef";
    unsigned o = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t c = in[i] < 0x80 ? in[i] : mac_roman_hi[in[i] - 0x80];
        char b[8]; unsigned k = 0;
        if (json && (c == '"' || c == '\\')) { b[k++] = '\\'; b[k++] = (char)c; }
        else if (c < 0x20 || c == 0x7f) {
            if (json) { b[k++] = '\\'; b[k++] = 'u'; b[k++] = '0'; b[k++] = '0'; b[k++] = hex[c >> 4]; b[k++] = hex[c & 15]; }
            else b[k++] = ' ';
        }
        else if (c < 0x80) b[k++] = (char)c;
        else if (c < 0x800) { b[k++] = (char)(0xc0 | c >> 6); b[k++] = (char)(0x80 | (c & 63)); }
        else { b[k++] = (char)(0xe0 | c >> 12); b[k++] = (char)(0x80 | ((c >> 6) & 63)); b[k++] = (char)(0x80 | (c & 63)); }
        if (o + k + 1 > max) break;
        for (unsigned j = 0; j < k; j++)
            out[o++] = b[j];
    }
    if (max) out[o] = 0;
    return o;
}

/* UTF-8 to Mac Roman, for the paths the remote's file endpoints are given (psmac_files.c, 21 November
   2026): each character found in the table above, '?' for one Mac Roman does not have, NUL-terminated.
   Returns the bytes written, or -1 if a character was not in Mac Roman (the path could not name a file). */
int psmac_utf8_macroman(const char *in, uint8_t *out, unsigned max)
{
    unsigned o = 0; int lost = 0;
    const uint8_t *p = (const uint8_t *)in;
    while (*p && o + 1 < max) {
        uint32_t c = *p++;
        if (c >= 0xc0 && c < 0xe0 && (p[0] & 0xc0) == 0x80) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
        else if (c >= 0xe0 && c < 0xf0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
        else if (c >= 0x80) { lost = 1; out[o++] = '?'; continue; }
        if (c < 0x80) { out[o++] = (uint8_t)c; continue; }
        unsigned k = 0;
        while (k < 128 && mac_roman_hi[k] != c) k++;
        if (k == 128) { lost = 1; out[o++] = '?'; } else out[o++] = (uint8_t)(0x80 + k);
    }
    if (max) out[o] = 0;
    return lost ? -1 : (int)o;
}

/* ---- the walk ---- */
static void setup(void)
{
    uint8_t m32;
    /* the Mac's RAM as the kernel gave it (logical 0 up; MemTop is no help: System 7's Process Manager
       leaves it at the top of the system's part, 2.8 MB on a 16 MB Mac in the first test) */
    ram_top = psmac_ram_size();
    if (ram_top < 0x100000u || ram_top > 0x40000000u) ram_top = 0x100000u;
    mask = 0x00ffffffu;
    if (mrd8(LM_MMU32BIT, &m32) && m32) mask = 0xffffffffu;
    else if (ram_top > 0x800000u) ram_top = 0x800000u;           /* 24-bit mode reaches 8 MB of RAM */
}
static int app_name(struct psmac_winlist *info, uint32_t pstr)
{
    uint8_t len, c;
    if (info->napps >= PSMAC_WIN_APPS || !mrd8(pstr, &len) || !len || len > 31) return -1;
    int a = info->napps; unsigned o = 0;
    for (unsigned i = 0; i < len; i++) {
        if (!mrd8(pstr + 1 + i, &c)) return -1;
        if (o || c) info->app[a][o++] = c;                         /* a desk accessory's name begins with a NUL (IM I-439) */
    }
    if (!o) return -1;
    info->app_len[a] = (uint8_t)o;
    info->napps++;
    return a;
}
/* the name of the application whose layer this is, from the Process Manager's record of it */
static int layer_app(struct psmac_winlist *info, uint32_t layer)
{
    uint32_t pe, back, pcbh, pcb, back2;
    if (!mrd32(layer + WR_REFCON, &pe)) return -1;
    pe &= mask;
    if (!ptr_ok(pe, PE_LAYER + 4) || !mrd32(pe + PE_LAYER, &back) || (back & mask) != layer) return -1;
    if (!mrd32(pe + PE_PCB, &pcbh) || !(pcb = deref(pcbh, PCB_LAYER + 4))) return -1;
    if (!mrd32(pcb + PCB_LAYER, &back2) || (back2 & mask) != layer) return -1;
    return app_name(info, pcb + PCB_APNAME);
}

struct walk {
    struct psmac_window *w; int n, max;
    struct psmac_winlist *info;
    uint32_t defproc, cur, cur_first;                              /* the layers' windowDefProc, the current layer, WindowList */
    int cur_app, layers;
};
/* is p a layer inside parent? its windowDefProc is the layers' and its parent link is parent */
static int is_layer(struct walk *k, uint32_t p, uint32_t parent)
{
    uint32_t dp, up;
    return mrd32(p + WR_DEFPROC, &dp) && dp == k->defproc && mrd32(p + WR_DATA, &up) && (up & mask) == parent;
}
/* the windows of the list from first, front to back; layers in it are walked into, depth first */
static void walk_list(struct walk *k, uint32_t first, uint32_t parent, int app, int depth)
{
    int count = 0;
    for (uint32_t p = first & mask; p; count++) {
        if (count == MAX_PER_LIST || k->n >= k->max || !ptr_ok(p, WR_SIZE)) { k->info->truncated = 1; return; }
        if (parent && depth < MAX_DEPTH && is_layer(k, p, parent)) {
            uint32_t sub = 0;
            if (k->layers++ >= MAX_LAYERS) { k->info->truncated = 1; return; }
            int a = p == k->cur ? k->cur_app : layer_app(k->info, p);
            if (p == k->cur) sub = k->cur_first;                   /* the current layer's windows are WindowList */
            else if (!mrd32(p + WR_PIC, &sub)) { k->info->truncated = 1; return; }
            walk_list(k, sub, p, a, depth + 1);
        } else {
            if (!read_window(p, &k->w[k->n])) { k->info->truncated = 1; return; }   /* not a window: the list is being changed */
            k->w[k->n].app = (int8_t)app;
            k->n++;
        }
        uint32_t next;
        if (!mrd32(p + WR_NEXT, &next)) { k->info->truncated = 1; return; }
        p = next & mask;
    }
}
/* System 7: the root layer, found from the current one by the parent links; 0 if there are no layers
   (System 6), or what $A90 holds does not hold together as layers */
static uint32_t find_root(struct walk *k)
{
    uint32_t cur, p, up, dp, pic;
    if (!mrd32(LM_CURLAYER, &cur)) return 0;
    cur &= mask;
    /* the current layer: a window record whose first window is WindowList (or which is empty) */
    if (!ptr_ok(cur, WR_SIZE) || !mrd32(cur + WR_DEFPROC, &k->defproc) || !k->defproc || !mrd32(cur + WR_PIC, &pic)) return 0;
    if ((pic & mask) != k->cur_first && k->cur_first) return 0;
    k->cur = cur;
    p = cur;
    for (int i = 0; i < MAX_DEPTH; i++) {                          /* the climb, bounded */
        if (!ptr_ok(p, WR_SIZE) || !mrd32(p + WR_DEFPROC, &dp) || dp != k->defproc || !mrd32(p + WR_DATA, &up)) return 0;
        if (!(up & mask)) return p;                                /* no parent: the root */
        p = up & mask;
    }
    return 0;
}
/* one reading of everything */
static int read_once(struct psmac_window *w, int max, struct psmac_winlist *info)
{
    struct walk k = { .w = w, .max = max, .info = info };
    setup();
    info->napps = 0; info->truncated = 0; info->layers = 0; info->how = "WindowList";
    if (!mrd32(LM_WINDOWLIST, &k.cur_first)) return 0;
    k.cur_first &= mask;
    k.cur_app = app_name(info, LM_CURAPNAME);
    uint32_t root = find_root(&k), first;
    if (root && mrd32(root + WR_PIC, &first)) {
        walk_list(&k, first, root, -1, 1);
        if (!info->truncated && k.layers) { info->how = "layers"; info->layers = k.layers; return k.n; }
        info->napps = 0; info->truncated = 0;                       /* the layers did not hold together: WindowList alone */
        k.n = 0; k.cur_app = app_name(info, LM_CURAPNAME);
    }
    walk_list(&k, k.cur_first, 0, k.cur_app, 0);
    return k.n;
}
static int same(const struct psmac_window *a, const struct psmac_window *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i].ptr != b[i].ptr || a[i].visible != b[i].visible || a[i].title_len != b[i].title_len || a[i].has_rect != b[i].has_rect) return 0;
        for (int k = 0; k < 4; k++)
            if (a[i].rect[k] != b[i].rect[k]) return 0;
    }
    return 1;
}
int psmac_windows_read(struct psmac_window *w, int max, struct psmac_winlist *info)
{
    static struct psmac_window again[PSMAC_WIN_MAX];
    static struct psmac_winlist info2;
    if (max > PSMAC_WIN_MAX) max = PSMAC_WIN_MAX;
    int n = read_once(w, max, info);
    info->consistent = 0;
    for (int tries = 0; tries < 4; tries++) {                     /* two readings that agree, or the last one */
        int n2 = read_once(again, max, &info2);
        if (n2 == n && same(w, again, n)) { info->consistent = 1; break; }
        __builtin_memcpy(w, again, sizeof again[0] * (unsigned)n2);
        *info = info2; n = n2;
    }
    info->n = n;
    return n;
}
