/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_wclip.c — drawing into a Mac window from the Pi: the window's visible region as rectangles, the box
 * around the Mac's pointer, and whether a menu is down. Read from the Mac's memory by whichever core draws.
 *
 * Backstory (23 November 2026). The video decoder card learnt the day before to draw only where its window shows
 * (docs/08 item 124, Aurora68 Play 1.8): it reads the window's visRgn through the Mac's memory before every
 * picture, leaves a box around the Mac's pointer alone (the Mac draws its pointer into the frame buffer and keeps
 * what was under it), and draws nothing while a menu is down. The PC card (psmac_pc.c, docs/17-pc-card.md) puts the
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
    uint32_t gdh = psmac_wclip_rd32(0x8A8, &ok);
    for (int n = 0; ok && gdh && n < 8; n++) {
        if (gdh & 1) return 0;
        uint32_t gd = psmac_wclip_rd32(gdh & ~3u, &ok);
        if (!ok || !gd || (gdh & 2)) return 0;               /* master pointers are longs at long addresses here */
        uint32_t pmh, pm, ba, t, l, nx;
        if (!rd16(gd + 22, &t) || !rd16(gd + 24, &l)) return 0;
        pmh = t << 16 | l;
        if (!pmh || (pmh & 3)) return 0;
        pm = psmac_wclip_rd32(pmh, &ok);
        if (!ok || !pm || (pm & 3)) return 0;
        ba = psmac_wclip_rd32(pm, &ok);
        if (!ok) return 0;
        if (!rd16(gd + 34, &t) || !rd16(gd + 36, &l)) return 0;
        if (ba == base) {
            int32_t y = mv - (int16_t)t, x = mh - (int16_t)l;
            if (y < -PSMAC_WCLIP_CURSOR || x < -PSMAC_WCLIP_CURSOR || y >= (int32_t)h + PSMAC_WCLIP_CURSOR || x >= (int32_t)w + PSMAC_WCLIP_CURSOR) return 0;
            out[0] = y - PSMAC_WCLIP_CURSOR; out[1] = x - PSMAC_WCLIP_CURSOR; out[2] = y + PSMAC_WCLIP_CURSOR; out[3] = x + PSMAC_WCLIP_CURSOR;
            return 1;
        }
        if (!rd16(gd + 30, &t) || !rd16(gd + 32, &l)) return 0;
        nx = t << 16 | l;
        gdh = nx;
    }
    return 0;
}
