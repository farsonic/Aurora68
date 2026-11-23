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
