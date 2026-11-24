/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_wclip.c — drawing into a Mac window from the Pi: the window's visible region as rectangles, the box
 * around the Mac's pointer, and whether a menu is down. Read from the Mac's memory by whichever core draws.
 *
 * Backstory (23 November 2026). The video decoder card learnt the day before to draw only where its window shows
 * (docs/08 item 124, Aurora68 Play 1.8): it reads the window's visRgn through the Mac's memory before every
 * picture, leaves a box around the Mac's pointer alone (the Mac draws its pointer into the frame buffer and keeps
 * what was under it), and draws nothing while a menu is down. The PC card (psmac_pc.c, docs/19-pc-card.md) puts the
 * PC's screen in an ordinary Mac window the same way (Aurora86's docs/DESIGN.md, "P3: the window"), so the three
 * pieces moved here from psmac_video.c (vis_parts, cursor_box) and psmac_videocard.c (the RAM reader and the menu
 * test), unchanged in what they do, and both cards call them. The history of each is beside it.
 */
#include <stdint.h>
#include "psmac_wclip.h"

uint32_t psmac_ram_size(void);
uint8_t *psmac_mac_ram_ptr(void);
int psmac_display_info(int i, uint32_t *w, uint32_t *h, uint32_t *depth, uint32_t *base);

/* the Mac's memory for the card's region and menu checks (23 November 2026, docs/08 item 124): RAM alone, at its own
   address (the ROM maps the Mac's RAM one to one, in 24-bit and 32-bit mode), aligned, inside the RAM, read from the
   card's core. Not through the 68k's MMU tables (psmac_mac_rd32, which the crash record uses): their table can be
   rebuilt by the 68k's core while it is read here, and a first version that went through them stopped the kernel
   during a start-up in the testbed */
uint32_t psmac_wclip_rd32(uint32_t addr, int *ok)
{
    uint32_t size = psmac_ram_size();
    if ((addr & 3) || size < 8 || addr > size - 4) { *ok = 0; return 0; }
    *ok = 1;
    return *(volatile const uint32_t *)(psmac_mac_ram_ptr() + addr);
}
/* a menu down, whichever program's (23 November 2026, docs/08 item 124). The menu bar's definition procedure keeps the
   bits under each menu it draws in a handle at MBSaveLoc ($B5C, Inside Macintosh V, "The Menu Manager"), whose first
   word, lastMBSave, is the offset of the last menu's record there: 0 when no menu is drawn, not 0 while one is (seen
   in the testbed under System 7.1: $1C with the Finder's Apple menu held down, 0 before and after). TheMenu ($A26),
   which a first version looked at, stays 0 while MenuSelect tracks: the Menu Manager sets it only for the title left
   highlighted after a choice. Both are looked at; the master pointer is masked to 24 bits in 24-bit mode (MMU32Bit,
   $CB2, 0) */
int psmac_wclip_menu_down(void)
{
    int ok, ok2, ok3;
    uint32_t v = psmac_wclip_rd32(0xA24, &ok), m32 = psmac_wclip_rd32(0xCB0, &ok2);
    if (ok && (v & 0xFFFFu)) return 1;
    uint32_t h = psmac_wclip_rd32(0xB5C, &ok);
    if (!ok || !h) return 0;
    uint32_t mp = psmac_wclip_rd32(h & ~3u, &ok);
    if (!ok || !mp) return 0;
    if (!ok2 || !((m32 >> 8) & 0xFFu)) mp &= 0x00FFFFFFu;
    uint32_t w = psmac_wclip_rd32(mp & ~3u, &ok3);
    return ok3 && (mp & 2 ? w & 0xFFFFu : w >> 16) != 0;
}

/* the window's visible region as rectangles (23 November 2026, docs/08 item 124). A QuickDraw region: rgnSize, rgnBBox,
   then (unless rgnSize is 10, a rectangle) inversion lines: a y, the x's where the region's inside changes from that
   line down, $7FFF; the whole ends with another $7FFF. Read through the Mac's memory map at every picture (the Window
   Manager changes it whenever a window moves, also another program's), each band of lines with the same x's a set of
   rectangles, a band like the one above it merged into it; in the window's coordinates, the picture's rectangle at
   0,0, moved to the screen's by x0, y0 and cut to the part on the screen. Anything that does not read as a region (a
   block moving in the heap as it is read) is -1: that picture is not drawn */
static int rd16(uint32_t a, uint32_t *v)
{
    int ok;
    uint32_t w = psmac_wclip_rd32(a & ~3u, &ok);
    *v = a & 2 ? w & 0xFFFFu : w >> 16;
    return ok;
}
/* Is the handle a live region? (23 November 2026, the owner's SE: Aurora68 PC had quit and the PC card went on drawing
   through the visRgn handle it had been given, by then freed memory read as a region, over and under the Finder's
   windows on both screens.) The region is checked as the Memory Manager keeps it (Inside Macintosh: Memory, "Heap
   Zones" and "Block Headers"): the handle's master pointer points to the block's data, after its header, 8 bytes in a
   24-bit zone (the tag in bits 7-6 of the first byte, the physical size in the other 24 bits, the relative handle) and
   12 in a 32-bit zone (the tag byte, the master pointer's flags, two bytes, the physical size, the relative handle).
   The tag says relocatable ($80); the relative handle is the handle's offset from its zone, whose first long (bkLim,
   the zone's last block) lies beyond the block; the physical size holds the region. A freed block's tag is 0, a block
   that moved leaves its master pointer pointing at whatever follows there, and a handle in a heap that has gone with
   its program points at memory that is reused soon. What a stale but untouched heap still holds passes: the PC card
   also watches its program's heartbeat (psmac_pc.c) */
static int rd8(uint32_t a, uint32_t *v)
{
    int ok;
    uint32_t w = psmac_wclip_rd32(a & ~3u, &ok);
    *v = (w >> (24 - 8 * (a & 3))) & 0xFFu;
    return ok;
}
static int rd32u(uint32_t a, uint32_t *v)
{
    uint32_t hi, lo;
    if (!rd16(a, &hi) || !rd16(a + 2, &lo)) return 0;
    *v = hi << 16 | lo;
    return 1;
}
int psmac_wclip_rgn_ok(uint32_t rgn, uint32_t flags, int32_t w, int32_t h)
{
    int ok;
    uint32_t ram = psmac_ram_size(), hdr = flags & PSMAC_WCLIP_24BIT ? 8u : 12u, mp, tag, psize, rel, zone, lim, size, v;
    int32_t bb[4];
    if (ram < 0x10000u || rgn < 0x100u || (rgn & 1)) return 0;
    mp = psmac_wclip_rd32(rgn & ~3u, &ok);
    if (!ok || (rgn & 3)) return 0;                          /* master pointers are longs at long addresses */
    if (flags & PSMAC_WCLIP_24BIT) mp &= 0x00FFFFFFu;
    if ((mp & 1) || mp < 0x1000u || mp > ram - 10u) return 0;
    if (!rd8(mp - hdr, &tag) || (tag >> 6) != 2) return 0;  /* relocatable */
    if (!rd32u(mp - 8, &psize) || !rd32u(mp - 4, &rel)) return 0;
    if (flags & PSMAC_WCLIP_24BIT) psize &= 0x00FFFFFFu;
    if (!rd16(mp, &size) || size < 10 || size > 8192 || (size & 1) || psize < hdr + size || psize > (1u << 24)) return 0;
    if (mp + size > ram) return 0;
    zone = rgn - rel;                                        /* the heap zone the handle belongs to */
    if (!rel || rel > rgn || (zone & 1) || zone < 0x100u || !rd32u(zone, &lim)) return 0;
    if (flags & PSMAC_WCLIP_24BIT) lim &= 0x00FFFFFFu;
    if (lim <= mp || lim > ram) return 0;
    for (int k = 0; k < 4; k++) { if (!rd16(mp + 2 + 2 * (uint32_t)k, &v)) return 0; bb[k] = (int16_t)v; }
    if (bb[0] > bb[2] || bb[1] > bb[3]) return 0;            /* top, left, bottom, right in order */
    if (bb[0] < bb[2] && bb[1] < bb[3] && (w > 0 || h > 0) &&
        (bb[0] < -1 || bb[1] < -1 || bb[2] > h + 1 || bb[3] > w + 1)) return 0;   /* a visRgn lies inside its window */
    if (size > 10 && (!rd16(mp + size - 2, &v) || v != 0x7FFF)) return 0;          /* the inversion lines' end */
    return 1;
}

int psmac_wclip_parts(int32_t out[PSMAC_WCLIP_MAX][4], uint32_t rgn, uint32_t flags, int32_t x0, int32_t y0,
                      int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1)
{
    int ok, n = 0, nx = 0, k;
    uint32_t mp = psmac_wclip_rd32(rgn, &ok), size, w, a;
    int32_t xs[32], bb[4];
    if (!ok || !mp) return -1;
    if (flags & PSMAC_WCLIP_24BIT) mp &= 0x00FFFFFFu;
    if ((mp & 1) || !rd16(mp, &size) || size < 10 || size > 8192) return -1;
    for (k = 0; k < 4; k++) { if (!rd16(mp + 2 + 2 * (uint32_t)k, &w)) return -1; bb[k] = (int16_t)w; }
    if (size == 10) {                         /* a rectangle (or empty) */
        int32_t t = bb[0] + y0, l = bb[1] + x0, b = bb[2] + y0, r = bb[3] + x0;
        if (t < cy0) t = cy0;
        if (l < cx0) l = cx0;
        if (b > cy1) b = cy1;
        if (r > cx1) r = cx1;
        if (t >= b || l >= r) return 0;
        out[0][0] = t; out[0][1] = l; out[0][2] = b; out[0][3] = r;
        return 1;
    }
    a = mp + 10;
    uint32_t end = mp + size;
    if (a + 2 > end || !rd16(a, &w)) return -1;
    a += 2;
    while (w != 0x7FFF) {
        int32_t y = (int16_t)w, ny;
        for (;;) {                            /* this line's x's toggle the inside */
            if (a + 2 > end || !rd16(a, &w)) return -1;
            a += 2;
            if (w == 0x7FFF) break;
            int32_t x = (int16_t)w;
            for (k = 0; k < nx && xs[k] < x; k++) { }
            if (k < nx && xs[k] == x) { for (; k + 1 < nx; k++) xs[k] = xs[k + 1]; nx--; }
            else {
                if (nx >= 32) return -1;
                for (int j = nx; j > k; j--) xs[j] = xs[j - 1];
                xs[k] = x; nx++;
            }
        }
        if (a + 2 > end || !rd16(a, &w)) return -1;
        a += 2;
        if (w == 0x7FFF) break;
        ny = (int16_t)w;
        if (ny <= y) return -1;               /* not a region: lines go down */
        for (k = 0; k + 1 < nx; k += 2) {     /* the band y..ny */
            int32_t t = y + y0, l = xs[k] + x0, b = ny + y0, r = xs[k + 1] + x0, j;
            if (t < cy0) t = cy0;
            if (l < cx0) l = cx0;
            if (b > cy1) b = cy1;
            if (r > cx1) r = cx1;
            if (t >= b || l >= r) continue;
            for (j = 0; j < n; j++)           /* the same columns just above: one rectangle */
                if (out[j][2] == t && out[j][1] == l && out[j][3] == r) { out[j][2] = b; break; }
            if (j < n) continue;
            if (n == PSMAC_WCLIP_MAX) return -1;   /* more pieces than this: not drawn rather than drawn over a window */
            out[n][0] = t; out[n][1] = l; out[n][2] = b; out[n][3] = r; n++;
        }
    }
    return n;
}

/* The Mac's mouse pointer over the picture (23 November 2026, the owner's SE on HDMI: "mouse has vanished and screen is
   a bit corrupted"). The Mac draws its pointer into the frame buffer itself and keeps what was under it; the card
   drawing a picture over it erased the pointer, and when the pointer moved on the Mac put back an old piece of
   picture. So the card leaves a box around the pointer alone: PSMAC_WCLIP_CURSOR pixels each side of the Mouse point
   ($830, global), which covers any hot spot of a 16 x 16 pointer and a beat of movement. The box is found in the
   screen's own pixels through the device list ($8A8): the GDevice whose PixMap's baseAddr is this screen's, minus its
   gdRect's top left. 0 if the pointer is not on this screen (or the Mac's memory can't be read now). */
int psmac_wclip_cursor_box(uint32_t screen, int32_t out[4])
{
    int ok;
    uint32_t w, h, d, base, v;
    if (!psmac_display_info((int)screen, &w, &h, &d, &base)) return 0;
    if (!rd16(0x830, &v)) return 0;
    int32_t mv = (int16_t)v;
    if (!rd16(0x832, &v)) return 0;
    int32_t mh = (int16_t)v;
    uint32_t gdh = psmac_wclip_rd32(0x8A8, &ok), m32;
    /* in 24-bit mode (MMU32Bit, $CB2, 0) a master pointer's top byte is the block's flags (23 November 2026: found by
       tools/pcplace_test.c; the box was never found in 24-bit mode, and the card drew over the pointer there) */
    if (!rd8(0xCB2, &m32)) return 0;
    uint32_t mask = m32 ? 0xFFFFFFFFu : 0x00FFFFFFu;
    for (int n = 0; ok && gdh && n < 8; n++) {
        if (gdh & 1) return 0;
        uint32_t gd = psmac_wclip_rd32(gdh & ~3u, &ok) & mask;
        if (!ok || !gd || (gdh & 2)) return 0;               /* master pointers are longs at long addresses here */
        uint32_t pmh, pm, ba, t, l, nx;
        if (!rd16(gd + 22, &t) || !rd16(gd + 24, &l)) return 0;
        pmh = t << 16 | l;
        if (!pmh || (pmh & 3)) return 0;
        pm = psmac_wclip_rd32(pmh, &ok) & mask;
        if (!ok || !pm || (pm & 3)) return 0;
        ba = psmac_wclip_rd32(pm, &ok);
        if (!ok) return 0;
        if (!rd16(gd + 34, &t) || !rd16(gd + 36, &l)) return 0;
        if (ba == base) {
            int32_t y = mv - (int16_t)t, x = mh - (int16_t)l;
            if (y < -PSMAC_WCLIP_CURSOR || x < -PSMAC_WCLIP_CURSOR || y >= (int32_t)h + PSMAC_WCLIP_CURSOR || x >= (int32_t)w + PSMAC_WCLIP_CURSOR) return 0;
            /* Widened to the frame buffer's longwords (23 November 2026, the PC card in the testbed at 512 x 342): the Mac
               saves and puts back what is under its pointer in whole longs of the frame buffer, 32 pixels at one bit, so
               at one bit the part it puts back reaches up to 31 pixels beyond the 16-pixel pointer on either side. With
               the box at 24 pixels each side, a pointer that had rested over text that changed under it put back, as it
               moved on, two letters just right of the box from its saved copy, which nothing drew again. Rounded out to
               32 / depth pixels the box holds every pixel the Mac may put back: 64 or 96 wide at one bit, 48 to 54 at 8 bits */
            int32_t al = d >= 32 || !d ? 1 : (int32_t)(32 / d), x0 = x - PSMAC_WCLIP_CURSOR, x1 = x + PSMAC_WCLIP_CURSOR;
            x0 = x0 >= 0 ? x0 / al * al : -((-x0 + al - 1) / al * al);
            x1 = x1 >= 0 ? (x1 + al - 1) / al * al : -(-x1 / al * al);
            out[0] = y - PSMAC_WCLIP_CURSOR; out[1] = x0; out[2] = y + PSMAC_WCLIP_CURSOR; out[3] = x1;
            return 1;
        }
        if (!rd16(gd + 30, &t) || !rd16(gd + 32, &l)) return 0;
        nx = t << 16 | l;
        gdh = nx;
    }
    return 0;
}

/* ---- the overdraw guard (24 November 2026) ---------------------------------------------------------------------
   The owner's SE, the 2560 x 1440 HDMI screen: the Apple II window open and busy at the BASIC prompt, the Finder in
   front, a Finder window ("Utilities") dragged over part of the Apple's window; afterwards a rectangle of the Apple's
   screen, several rows of its text, stayed inside the Finder's window on top of its white content. Each card read
   the visRgn once per picture and drew every rectangle of it afterwards; on a deep, large screen a picture takes
   milliseconds, and its core can be given to the sound or the other guest between the reading and the drawing, while
   the Mac, on core 0, finishes the drag: MoveWindow puts the Finder's window in its new place and gives the Apple's
   window its smaller region. The picture then went on with the region it had read, over the Finder's window, and
   nothing on the Mac had a reason to draw it again. The cure is in two layers (psmac_wclip.h): the region is looked
   at again before every band of a picture, and what was drawn shortly before the region changed, outside the new
   region, is handed to the program to have the Mac paint it again. */
int psmac_wclip_sig(uint32_t rgn, uint32_t flags, struct psmac_wclip_sig *s)
{
    int ok;
    uint32_t mp, size, v, sum = 0x9E3779B9u;
    if (rgn & 3) return 0;
    mp = psmac_wclip_rd32(rgn, &ok);
    if (!ok) return 0;
    if (flags & PSMAC_WCLIP_24BIT) mp &= 0x00FFFFFFu;
    if ((mp & 1) || !rd16(mp, &size) || size < 10 || size > 8192 || (size & 1)) return 0;
    for (uint32_t a = mp; a < mp + size; a += 2) {     /* the region's own bytes only: what follows it is another block's */
        if (!rd16(a, &v)) return 0;
        sum = (sum << 5 | sum >> 27) ^ v;
    }
    s->mp = mp; s->size = size; s->sum = sum;
    return 1;
}
int psmac_wclip_parts_sig(int32_t out[PSMAC_WCLIP_MAX][4], uint32_t rgn, uint32_t flags, int32_t x0, int32_t y0,
                          int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1, struct psmac_wclip_sig *sig)
{
    struct psmac_wclip_sig again;
    if (!psmac_wclip_sig(rgn, flags, sig)) return -1;
    int n = psmac_wclip_parts(out, rgn, flags, x0, y0, cx0, cy0, cx1, cy1);
    if (n < 0 || !psmac_wclip_sig(rgn, flags, &again) || !psmac_wclip_sig_eq(sig, &again)) return -1;
    return n;
}
int psmac_wclip_minus(int32_t a[][4], int n, int max, const int32_t b[][4], int nb)
{
    int32_t tmp[64][4];
    if (max > 64) max = 64;
    for (int j = 0; j < nb && n > 0; j++) {
        int m = 0, over = 0;
        for (int i = 0; i < n; i++) {
            int32_t t = a[i][0], l = a[i][1], bo = a[i][2], r = a[i][3];
            int32_t it = t > b[j][0] ? t : b[j][0], il = l > b[j][1] ? l : b[j][1];
            int32_t ib = bo < b[j][2] ? bo : b[j][2], ir = r < b[j][3] ? r : b[j][3];
            int32_t p[4][4];
            int k = 0;
            if (it >= ib || il >= ir) { p[k][0] = t; p[k][1] = l; p[k][2] = bo; p[k][3] = r; k++; }   /* untouched */
            else {                                                  /* up to four pieces round the overlap */
                if (t < it) { p[k][0] = t; p[k][1] = l; p[k][2] = it; p[k][3] = r; k++; }
                if (ib < bo) { p[k][0] = ib; p[k][1] = l; p[k][2] = bo; p[k][3] = r; k++; }
                if (l < il) { p[k][0] = it; p[k][1] = l; p[k][2] = ib; p[k][3] = il; k++; }
                if (ir < r) { p[k][0] = it; p[k][1] = ir; p[k][2] = ib; p[k][3] = r; k++; }
            }
            for (int q = 0; q < k; q++) {
                if (m == max) { over = 1; break; }
                for (int c = 0; c < 4; c++) tmp[m][c] = p[q][c];
                m++;
            }
            if (over) break;
        }
        if (over) {                                             /* too many pieces: the bounding box of what a held */
            int32_t bb[4] = { a[0][0], a[0][1], a[0][2], a[0][3] };
            for (int i = 1; i < n; i++) {
                if (a[i][0] < bb[0]) bb[0] = a[i][0];
                if (a[i][1] < bb[1]) bb[1] = a[i][1];
                if (a[i][2] > bb[2]) bb[2] = a[i][2];
                if (a[i][3] > bb[3]) bb[3] = a[i][3];
            }
            for (int c = 0; c < 4; c++) a[0][c] = bb[c];
            return 1;
        }
        for (int i = 0; i < m; i++) for (int c = 0; c < 4; c++) a[i][c] = tmp[i][c];
        n = m;
    }
    return n;
}

void psmac_wclip_guard_begin(struct psmac_wclip_guard *g, uint32_t rgn, uint32_t flags, const struct psmac_wclip_sig *s,
                             uint32_t screen, int32_t x0, int32_t y0)
{
    /* the notes go on while the region and the place are the ones they were made with (psmac_wclip_guard_watch has
       looked at any change before this picture); else they start again */
    if (g->n && (g->rgn != rgn || g->flags != flags || !psmac_wclip_sig_eq(&g->sig, s) || g->screen != screen || g->x0 != x0 || g->y0 != y0)) {
        g->n = 0;
        g->suspect = 0;
    }
    g->rgn = rgn; g->flags = flags; g->sig = *s; g->screen = screen; g->x0 = x0; g->y0 = y0;
}
int psmac_wclip_guard_still(struct psmac_wclip_guard *g)
{
    struct psmac_wclip_sig now;
    g->looks++;
    return psmac_wclip_sig(g->rgn, g->flags, &now) && psmac_wclip_sig_eq(&now, &g->sig);
}
void psmac_wclip_guard_add(struct psmac_wclip_guard *g, int32_t t, int32_t l, int32_t b, int32_t r)
{
    if (t >= b || l >= r) return;
    for (int i = 0; i < g->n; i++) {
        int32_t *q = g->r[i];
        if (t >= q[0] && l >= q[1] && b <= q[2] && r <= q[3]) return;          /* already noted */
        if (q[1] == l && q[3] == r && t <= q[2] && b >= q[0]) {               /* the same columns, touching: one */
            if (t < q[0]) q[0] = t;
            if (b > q[2]) q[2] = b;
            return;
        }
    }
    if (g->n == PSMAC_WCLIP_DRAWN) {                                           /* full: the last grows to hold it */
        int32_t *q = g->r[g->n - 1];
        if (t < q[0]) q[0] = t;
        if (l < q[1]) q[1] = l;
        if (b > q[2]) q[2] = b;
        if (r > q[3]) q[3] = r;
        return;
    }
    g->r[g->n][0] = t; g->r[g->n][1] = l; g->r[g->n][2] = b; g->r[g->n][3] = r;
    g->n++;
}
void psmac_wclip_guard_end(struct psmac_wclip_guard *g, uint64_t t, int changed)
{
    if (!g->n) return;
    g->t_end = t;
    if (changed) { g->suspect = 1; g->given_up++; }
}
int psmac_wclip_guard_watch(struct psmac_wclip_guard *g, uint32_t rgn, uint32_t flags, uint32_t screen, int32_t x0,
                            int32_t y0, int32_t w, int32_t h, int32_t sw, int32_t sh, uint64_t now, uint64_t young,
                            int32_t out[PSMAC_WCLIP_DRAWN][4])
{
    if (!g->n) return 0;
    if ((!g->suspect && now - g->t_end > young) || now - g->t_end > 20 * young) {   /* drawn long before any change since */
        g->n = 0; g->suspect = 0;
        return 0;
    }
    struct psmac_wclip_sig s;
    int moved = screen != g->screen || x0 != g->x0 || y0 != g->y0 || rgn != g->rgn || flags != g->flags;
    if (!moved) {
        if (!psmac_wclip_sig(rgn, flags, &s)) return -1;
        if (psmac_wclip_sig_eq(&s, &g->sig)) return 0;                       /* the same region: nothing */
    }
    /* changed: what was drawn, less the region now (on its screen: none if the window went to another) */
    int32_t vis[PSMAC_WCLIP_MAX][4];
    int nv = 0;
    if (rgn && screen == g->screen) {
        int32_t cx0 = x0 > 0 ? x0 : 0, cy0 = y0 > 0 ? y0 : 0, cx1 = x0 + w < sw ? x0 + w : sw, cy1 = y0 + h < sh ? y0 + h : sh;
        if (cx0 < cx1 && cy0 < cy1) {
            if (!psmac_wclip_rgn_ok(rgn, flags, w, h)) return -1;
            nv = psmac_wclip_parts_sig(vis, rgn, flags, x0, y0, cx0, cy0, cx1, cy1, &s);
            if (nv < 0) return -1;
        }
    }
    for (int i = 0; i < g->n; i++) for (int c = 0; c < 4; c++) out[i][c] = g->r[i][c];
    int n = psmac_wclip_minus(out, g->n, PSMAC_WCLIP_DRAWN, (const int32_t (*)[4])vis, nv);
    if (n && !g->suspect) g->late++;                                         /* a picture that looked unchanged throughout */
    g->n = 0;
    g->suspect = 0;
    return n;
}
void psmac_wclip_repair_post(volatile struct a68_repair *rp, uint32_t done, uint32_t screen, int32_t r[][4], int n)
{
    if (n <= 0) return;
    uint32_t seq = rp->seq & ~1u;
    uint32_t k = done == seq ? 0 : rp->n;                                    /* added to one not carried out yet */
    if (k > A68_REPAIR_MAX) k = 0;
    rp->seq = seq + 1;                                                       /* odd: being written */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (int i = 0; i < n; i++) {
        volatile struct a68_repair_rect *q;
        if (k < A68_REPAIR_MAX) q = &rp->r[k++];
        else {                                                               /* full: the last grows to hold it */
            q = &rp->r[A68_REPAIR_MAX - 1];
            if (q->screen == (int32_t)screen) {
                if (r[i][0] < q->top) q->top = (int16_t)r[i][0];
                if (r[i][1] < q->left) q->left = (int16_t)r[i][1];
                if (r[i][2] > q->bottom) q->bottom = (int16_t)r[i][2];
                if (r[i][3] > q->right) q->right = (int16_t)r[i][3];
                continue;
            }
        }
        q->screen = (int32_t)screen;
        q->top = (int16_t)r[i][0]; q->left = (int16_t)r[i][1]; q->bottom = (int16_t)r[i][2]; q->right = (int16_t)r[i][3];
    }
    rp->n = k;
    rp->posted = rp->posted + 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    rp->seq = seq + 2;
}
