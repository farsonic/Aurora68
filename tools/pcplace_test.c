/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/pcplace_test.c - where Aurora68 PC's window is and what the card may draw through, on the host, without
 * Aurora86 (docs/19-pc-card.md): the program's own mapping (mac/a68pc/place.h, which pc.c includes) and the kernel's
 * window helpers (emu68/src/psmac/psmac_wclip.c: the live-region check, the region's rectangles, the pointer's box),
 * against a pretend Mac memory holding two screens' GDevices, the unit table and their drivers' DCEs, and a heap zone
 * with a window's visRgn in it.
 *
 * Backstory (23 November 2026). On the owner's SE the SE's own 512 x 342 screen is the main one, at 0,0, and the
 * 2560 x 1440 HDMI screen lies to its right (left 512 to 3072); the testbed had only had HDMI as the main screen, or
 * one screen. And after Aurora68 PC had quit the card drew through its window's visRgn handle, freed memory by then.
 * Checked here: a window on either screen and across both, in both arrangements (and with HDMI main and the SE's to its
 * left), in 32-bit and 24-bit mode; the pointer's box on the right screen in its own pixels; a live region accepted and
 * a disposed, purged, moved, oversized, damaged or foreign one refused. Run by `make -C mac check` (tools/pcplace_test.sh).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "psmac_wclip.h"

/* ---- the pretend Mac memory: aligned longs in the host's own order, as the kernel (big-endian like the 68k) reads
   them with one load; psmac_wclip.c takes words and bytes out of longs */
#define RAM (4u << 20)
static uint32_t ram32[RAM / 4];
uint32_t psmac_ram_size(void) { return RAM; }
uint8_t *psmac_mac_ram_ptr(void) { return (uint8_t *)ram32; }
static uint32_t rd16(uint32_t a) { uint32_t w = ram32[(a & ~3u) / 4]; return a & 2 ? w & 0xFFFFu : w >> 16; }
static uint32_t rd32(uint32_t a) { return a & 2 ? rd16(a) << 16 | rd16(a + 2) : ram32[a / 4]; }   /* the 68020 reads longs at word addresses */
static uint32_t rd8(uint32_t a) { return (ram32[(a & ~3u) / 4] >> (24 - 8 * (a & 3))) & 0xFFu; }
static void w32(uint32_t a, uint32_t v) { if (a & 1) abort(); if (a & 2) { uint32_t *p = &ram32[(a & ~3u) / 4]; *p = (*p & 0xFFFF0000u) | v >> 16; p[1] = (p[1] & 0xFFFFu) | v << 16; } else ram32[a / 4] = v; }
static void w16(uint32_t a, uint32_t v) { uint32_t *p = &ram32[(a & ~3u) / 4]; *p = a & 2 ? (*p & 0xFFFF0000u) | (v & 0xFFFFu) : (*p & 0xFFFFu) | v << 16; }
static void w8(uint32_t a, uint32_t v) { uint32_t *p = &ram32[(a & ~3u) / 4], sh = 24 - 8 * (a & 3); *p = (*p & ~(0xFFu << sh)) | (v & 0xFFu) << sh; }

/* the program's mapping, as pc.c compiles it, through this memory */
static int mmu32 = 1;
#define PL_RD32(a) rd32((uint32_t)(a))
#define PL_RD16(a) rd16((uint32_t)(a))
#define PL_RD8(a)  rd8((uint32_t)(a))
#define PL_MASK(a) (mmu32 ? (uint32_t)(a) : (uint32_t)(a) & 0x00FFFFFFu)
#include "../mac/a68pc/place.h"

/* the kernel's screens (psmac_display.c's numbering: 0 the SE's own, 1 HDMI) for the pointer's box */
static uint32_t scr_w[2], scr_h[2], scr_base[2];
int psmac_display_info(int i, uint32_t *w, uint32_t *h, uint32_t *depth, uint32_t *base)
{
    if (i < 0 || i > 1 || !scr_w[i]) return 0;
    *w = scr_w[i]; *h = scr_h[i]; *depth = i ? 8 : 1; *base = scr_base[i];
    return 1;
}

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("pcplace_test: FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* two screens: the SE's (slot 9, 512 x 342) and HDMI (slot $A, 2560 x 1440), at the given desktop places; the
   handles' master pointers carry flag bits in their top byte in 24-bit mode, as a 24-bit heap's do */
#define UT      0x4000u
#define HFLAGS(p) (mmu32 ? (p) : (p) | 0x40000000u)
static void screens(int se_t, int se_l, int hd_t, int hd_l, int hdmi_first)
{
    memset(ram32, 0, sizeof ram32);
    w8(0xCB2, (uint32_t)mmu32);                                       /* MMU32Bit */
    w32(0x11C, UT);                                                    /* UTableBase */
    uint32_t gd[2] = { 0x10000, 0x10100 }, gh[2] = { 0x9000, 0x9004 }, dce[2] = { 0x11000, 0x11100 }, dh[2] = { 0x9010, 0x9014 };
    uint32_t pm[2] = { 0x12000, 0x12100 }, pmh[2] = { 0x9020, 0x9024 };
    int32_t rc[2][4] = { { se_t, se_l, se_t + 342, se_l + 512 }, { hd_t, hd_l, hd_t + 1440, hd_l + 2560 } };
    scr_w[0] = 512; scr_h[0] = 342; scr_base[0] = 0xF9000000u + 0x8000u;
    scr_w[1] = 2560; scr_h[1] = 1440; scr_base[1] = 0x90000000u;
    for (int i = 0; i < 2; i++) {
        int ref = -(40 + i) - 1;                                       /* unit 40 and 41 */
        w32(UT + 4u * (uint32_t)~ref, dh[i]); w32(dh[i], HFLAGS(dce[i])); w8(dce[i] + 40, i ? 0xA : 0x9);
        w16(gd[i], (uint32_t)(uint16_t)ref);                          /* gdRefNum */
        w32(gh[i], HFLAGS(gd[i]));
        w32(pmh[i], HFLAGS(pm[i])); w32(pm[i], scr_base[i]);           /* gdPMap -> baseAddr */
        w16(gd[i] + 22, pmh[i] >> 16); w16(gd[i] + 24, pmh[i] & 0xFFFF);
        for (int k = 0; k < 4; k++) w16(gd[i] + 34 + 2u * (uint32_t)k, (uint32_t)(uint16_t)rc[i][k]);
    }
    int first = hdmi_first ? 1 : 0;
    w32(0x8A8, gh[first]);                                             /* DeviceList: the main screen first */
    w16(gd[first] + 30, gh[!first] >> 16); w16(gd[first] + 32, gh[!first] & 0xFFFF);
}
static void place(const char *what, int32_t t, int32_t l, int32_t b, int32_t r, int32_t es, int32_t et, int32_t el)
{
    struct pl_rect c = { t, l, b, r };
    struct pl_place p;
    pl_place_of(&c, &p, 0);
    CHECK(p.screen == es && p.top == et && p.left == el && p.bottom - p.top == b - t && p.right - p.left == r - l,
          "%s (%s mode): content %d,%d-%d,%d gave screen %d at %d,%d-%d,%d, not screen %d at %d,%d", what, mmu32 ? "32-bit" : "24-bit",
          l, t, r, b, p.screen, p.left, p.top, p.right, p.bottom, es, el, et);
}
static void pointer(const char *what, int16_t v, int16_t h, int screen, int32_t ex, int32_t ey)
{
    int32_t box[4];
    w16(0x830, (uint16_t)v); w16(0x832, (uint16_t)h);                 /* Mouse */
    int on = psmac_wclip_cursor_box((uint32_t)screen, box);
    if (ex == -9999) { CHECK(!on, "%s: a pointer box on screen %d (%d,%d-%d,%d) for the pointer at %d,%d", what, screen, box[1], box[0], box[3], box[2], h, v); return; }
    CHECK(on && box[0] <= ey && box[2] > ey && box[1] <= ex && box[3] > ex && box[2] - box[0] == 2 * PSMAC_WCLIP_CURSOR,
          "%s: the pointer at %d,%d: box on screen %d %s %d,%d-%d,%d, its point %d,%d", what, h, v, screen, on ? "at" : "missing", box[1], box[0], box[3], box[2], ex, ey);
}

/* a heap zone with a window's visRgn: the zone at Z, its master pointers at Z + $100, the region's block at Z + $400 */
#define Z  0x200000u
#define HR (Z + 0x100u)
static uint32_t region(int bits24, uint32_t size, const int16_t *data, uint32_t n)
{
    uint32_t hdr = bits24 ? 8 : 12, blk = Z + 0x400u, mp = blk + hdr, phys = (hdr + size + 3) & ~3u;
    w32(Z, bits24 ? (Z + 0x40000u) | 0x00000000u : Z + 0x40000u);    /* bkLim */
    if (bits24) { w32(blk, 0x80u << 24 | phys); w32(blk + 4, HR - Z); }
    else { w8(blk, 0x80); w8(blk + 1, 0x00); w16(blk + 2, 0); w32(blk + 4, phys); w32(blk + 8, HR - Z); }
    w32(HR, bits24 ? mp | 0x80000000u : mp);                          /* the master pointer (locked, in 24-bit mode) */
    w16(mp, size);
    for (uint32_t i = 0; i < n; i++) w16(mp + 2 + 2 * i, (uint32_t)(uint16_t)data[i]);
    return mp;
}
static void regions(int bits24)
{
    uint32_t f = bits24 ? PSMAC_WCLIP_24BIT : 0, hdr = bits24 ? 8 : 12;
    const char *m = bits24 ? "24-bit" : "32-bit";
    static const int16_t rect[] = { 0, 0, 300, 484 };
    uint32_t mp = region(bits24, 10, rect, 4);
    CHECK(psmac_wclip_rgn_ok(HR, f, 484, 300) == 1, "%s: a window's whole visRgn refused", m);
    int32_t rr[PSMAC_WCLIP_MAX][4];
    int n = psmac_wclip_parts(rr, HR, f, 88, 60, 88, 60, 88 + 484, 60 + 300);
    CHECK(n == 1 && rr[0][0] == 60 && rr[0][1] == 88 && rr[0][2] == 360 && rr[0][3] == 572, "%s: its rectangle on the screen", m);
    /* covered on the right by another window: an L-shaped... a rectangle with a notch: inside 0..484 x 0..300 less 300..484 x 0..100 */
    static const int16_t notch[] = { 0, 0, 300, 484, 0, 0, 300, 0x7FFF, 100, 300, 484, 0x7FFF, 300, 0, 484, 0x7FFF, 0x7FFF };
    region(bits24, 10 + 2 * 13, notch, 17);
    CHECK(psmac_wclip_rgn_ok(HR, f, 484, 300) == 1, "%s: a partly covered window's visRgn refused", m);
    n = psmac_wclip_parts(rr, HR, f, 0, 0, 0, 0, 484, 300);
    CHECK(n == 2, "%s: the notched region as %d rectangles, not 2", m, n);
    w16(Z + 0x400u + hdr + 10 + 2 * 13 - 2, 0x1234);
    CHECK(!psmac_wclip_rgn_ok(HR, f, 484, 300), "%s: a region without its last $7FFF taken", m);
    /* the block freed (DisposeHandle, or the heap gone with its program and its blocks merged into free ones) */
    mp = region(bits24, 10, rect, 4);
    if (bits24) w32(mp - hdr, rd32(mp - hdr) & 0x00FFFFFFu); else w8(mp - hdr, 0x00);   /* the tag: free */
    CHECK(!psmac_wclip_rgn_ok(HR, f, 484, 300), "%s: a freed block taken for a region", m);
    /* purged / disposed: the master pointer NIL, or on the free list (pointing at the next free master pointer) */
    mp = region(bits24, 10, rect, 4);
    w32(HR, 0);
    CHECK(!psmac_wclip_rgn_ok(HR, f, 484, 300), "%s: a NIL master pointer taken", m);
    w32(HR, HR + 4); w32(HR + 4, 0);
    CHECK(!psmac_wclip_rgn_ok(HR, f, 484, 300), "%s: a master pointer on the free list taken", m);
    /* the block moved (compaction): the master pointer points at whatever follows, here another handle's block */
    mp = region(bits24, 10, rect, 4);
    w32(mp - 4, 0x123450u);
    CHECK(!psmac_wclip_rgn_ok(HR, f, 484, 300), "%s: another handle's block taken (its relative handle)", m);
    /* a region larger than the window it is said to be of (another window's, or junk) */
    static const int16_t big[] = { -40, -20, 900, 1500 };
    region(bits24, 10, big, 4);
    CHECK(!psmac_wclip_rgn_ok(HR, f, 484, 300), "%s: a region beyond its window taken", m);
    CHECK(psmac_wclip_rgn_ok(HR, f, 0, 0) == 1, "%s: a large region refused without a window size (the video card's use)", m);
    /* nonsense sizes */
    region(bits24, 7, rect, 4);
    CHECK(!psmac_wclip_rgn_ok(HR, f, 484, 300), "%s: rgnSize 7 taken", m);
    region(bits24, 9000, rect, 4);
    CHECK(!psmac_wclip_rgn_ok(HR, f, 484, 300), "%s: rgnSize 9000 taken", m);
    static const int16_t upside[] = { 300, 0, 0, 484 };
    region(bits24, 10, upside, 4);
    CHECK(!psmac_wclip_rgn_ok(HR, f, 484, 300), "%s: a bounding box upside down taken", m);
    /* outside the RAM, odd, low memory */
    CHECK(!psmac_wclip_rgn_ok(RAM + 16, f, 484, 300) && !psmac_wclip_rgn_ok(HR + 1, f, 484, 300) && !psmac_wclip_rgn_ok(0x20, f, 484, 300),
          "%s: a handle outside the RAM, odd or in low memory taken", m);
    /* an empty region (the window wholly covered) is a live one: nothing to draw */
    static const int16_t none[] = { 0, 0, 0, 0 };
    region(bits24, 10, none, 4);
    CHECK(psmac_wclip_rgn_ok(HR, f, 484, 300) == 1 && psmac_wclip_parts(rr, HR, f, 0, 0, 0, 0, 484, 300) == 0, "%s: an empty visRgn", m);
}

int main(void)
{
    for (mmu32 = 1; mmu32 >= 0; mmu32--) {
        /* the owner's SE: its own screen main at 0,0, HDMI to its right at left 512 */
        screens(0, 0, 0, 512, 0);
        place("SE main, the window on the SE", 40, 8, 340, 492, 0, 40, 8);
        place("SE main, the window on HDMI", 60, 600, 360, 1084, 1, 60, 88);
        place("SE main, the window across, mostly on HDMI", 40, 400, 340, 884, 1, 40, -112);
        place("SE main, the window across, mostly on the SE", 40, 100, 340, 584, 0, 40, 100);
        place("SE main, the window at HDMI's far right", 1100, 2588, 1400, 3072, 1, 1100, 2076);
        pointer("SE main, the pointer on HDMI", 100, 700, 1, 188, 100);
        pointer("SE main, the pointer on HDMI, asked of the SE", 100, 700, 0, -9999, 0);
        pointer("SE main, the pointer on the SE", 200, 300, 0, 300, 200);
        /* HDMI lower down beside the SE (Monitors lets the screens' tops differ) */
        screens(0, 0, -400, 512, 0);
        place("SE main, HDMI raised by 400, the window on HDMI", 60, 600, 360, 1084, 1, 460, 88);
        pointer("SE main, HDMI raised, the pointer on HDMI", 10, 600, 1, 88, 410);
        /* the testbed's arrangement: HDMI main at 0,0, the SE's screen to its right, and to its left */
        screens(0, 2560, 0, 0, 1);
        place("HDMI main, the window on HDMI", 40, 8, 340, 492, 1, 40, 8);
        place("HDMI main, the window on the SE (right)", 40, 2570, 340, 3054, 0, 40, 10);
        screens(0, -512, 0, 0, 1);
        place("HDMI main, the window on the SE (left)", 40, -500, 340, -16, 0, 40, 12);
        pointer("HDMI main, the pointer on the SE (left)", 50, -100, 0, 412, 50);
    }
    /* no device list (a Mac without Color QuickDraw's): the global rectangle itself on screen 0 */
    memset(ram32, 0, sizeof ram32); mmu32 = 1;
    { struct pl_rect c = { 40, 8, 340, 492 }; struct pl_place p; int r = pl_place_of(&c, &p, 0);
      CHECK(r == -1 && p.screen == 0 && p.left == 8 && p.top == 40, "no device list"); }
    regions(0);
    regions(1);
    printf("pcplace_test: %d checks, %d failed: the window's place on either screen in both arrangements, the pointer's box, the live region\n", checks, fails);
    return fails != 0;
}
