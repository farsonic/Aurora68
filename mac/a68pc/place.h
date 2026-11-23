/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/a68pc/place.h - where Aurora68 PC's window is, as the PC card wants it: the screen (A68P_SCREEN_*) and the
 * window's content rectangle in that screen's own pixels (docs/19-pc-card.md, "Aurora68 PC").
 *
 * Backstory (23 November 2026). The owner's SE runs with its own 512 x 342 screen as the main one, at the desktop's
 * 0,0, and the 2560 x 1440 HDMI screen to its right (left 512 to 3072); the testbed had only ever had the HDMI screen
 * as main, or one screen. This is the program's half of the mapping, taken out of pc.c so that tools/pcplace_test.c
 * runs the same lines on the host against a pretend Mac memory with both arrangements: the GDevice list (DeviceList,
 * $8A8), each device's gdRect (its place on the desktop) and its driver's slot (gdRefNum, the unit table at
 * UTableBase, $11C, the DCE's dCtlSlot): slot $A is the HDMI screen, anything else the first (the SE's own on the SE
 * board). Until this day the screen was the one the window's top left was on; now it is the one holding most of the
 * window's content (the card draws on one screen only), the top left's when none holds any.
 *
 * Memory is read through PL_RD32 / PL_RD16 / PL_RD8 (a 68k address in, the value out): on the Mac plain loads, on
 * the host the test's pretend memory. Master pointers are masked to 24 bits in 24-bit mode (PL_MASK).
 */
#ifndef A68PC_PLACE_H
#define A68PC_PLACE_H
#include <stdint.h>

#ifndef PL_RD32
#define PL_RD32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define PL_RD16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define PL_RD8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define PL_MASK(a) (*(volatile uint8_t *)0xCB2 ? (uint32_t)(a) : (uint32_t)(a) & 0x00FFFFFFu)   /* MMU32Bit */
#endif

struct pl_rect { int32_t top, left, bottom, right; };
struct pl_place { int32_t screen, top, left, bottom, right; };

/* the screen a GDevice is: its driver's slot */
static int pl_screen_kind(uint32_t gd)
{
	int16_t ref = (int16_t)PL_RD16(gd);                       /* gdRefNum: the screen's driver */
	uint32_t ut = PL_RD32(0x11C);                             /* UTableBase */
	if (ref < 0 && ut) {
		uint32_t dh = PL_RD32(ut + 4u * (uint32_t)(int32_t)~ref);   /* the DCE's handle */
		uint32_t dce = dh ? PL_MASK(PL_RD32(PL_MASK(dh))) : 0;
		if (dce && PL_RD8(dce + 40) == 0xA) return 1;          /* dCtlSlot $A: A68P_SCREEN_HDMI */
	}
	return 0;                                                 /* A68P_SCREEN_FIRST */
}
static int32_t pl_overlap(const struct pl_rect *a, const struct pl_rect *b)
{
	int32_t t = a->top > b->top ? a->top : b->top, l = a->left > b->left ? a->left : b->left;
	int32_t bo = a->bottom < b->bottom ? a->bottom : b->bottom, r = a->right < b->right ? a->right : b->right;
	return t < bo && l < r ? (bo - t) * (r - l) : 0;
}
/* the window's content c (global coordinates) as the card's place; the screen's gdRect into *sr. 0, or -1 when no
   screen is known (no device list: the place is then c itself on screen 0) */
static int pl_place_of(const struct pl_rect *c, struct pl_place *out, struct pl_rect *sr)
{
	uint32_t gdh = PL_RD32(0x8A8), best = 0, tl = 0;          /* DeviceList */
	int32_t most = 0;
	struct pl_rect br = { 0, 0, 0, 0 }, tr = { 0, 0, 0, 0 };
	for (int n = 0; gdh && n < 16; n++) {
		uint32_t gd = PL_MASK(PL_RD32(PL_MASK(gdh)));
		if (!gd) break;
		struct pl_rect r = { (int16_t)PL_RD16(gd + 34), (int16_t)PL_RD16(gd + 36), (int16_t)PL_RD16(gd + 38), (int16_t)PL_RD16(gd + 40) };   /* gdRect */
		int32_t o = pl_overlap(c, &r);
		if (o > most) { most = o; best = gd; br = r; }
		if (!tl && c->left >= r.left && c->left < r.right && c->top >= r.top && c->top < r.bottom) { tl = gd; tr = r; }
		gdh = PL_RD32(gd + 30);                               /* gdNextGD */
	}
	if (!best) { best = tl; br = tr; }
	if (!best) {
		out->screen = 0; out->top = c->top; out->left = c->left; out->bottom = c->bottom; out->right = c->right;
		if (sr) { sr->top = sr->left = sr->bottom = sr->right = 0; }
		return -1;
	}
	out->screen = pl_screen_kind(best);
	out->top = c->top - br.top; out->left = c->left - br.left; out->bottom = c->bottom - br.top; out->right = c->right - br.left;
	if (sr) *sr = br;
	return 0;
}
#endif
