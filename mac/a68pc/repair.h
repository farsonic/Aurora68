/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/a68pc/repair.h - the card's repair requests carried out: what the card drew outside the program's window, as the
 * window is now, painted again by the Mac. Shared by Aurora68 PC (pc.c) and Aurora68 Apple II (apple.c), as place.h is.
 *
 * Backstory (24 November 2026, the owner's SE on its 2560 x 1440 HDMI screen; docs/20-apple-ii-card.md "Drawn over another
 * window"). A Finder window dragged over the busy Apple II window kept a rectangle of the Apple's screen inside it: the card
 * had read the window's visRgn, and drew with it while the Mac (another core) moved the Finder's window over it. The card
 * now looks at the region between bands of a picture, and when it finds that it drew something outside the region the
 * window has now, it posts those rectangles (emu68/src/psmac/psmac_repair.h). The program has the Mac paint them again,
 * from every loop it runs, in the background too:
 *  - the rectangles, in the card's screen's own pixels, made global through that screen's gdRect (place.h's mapping
 *    backwards), as one region; less what the window shows now (the card draws that itself), and less anything not on
 *    the desktop (GrayRgn: never the menu bar);
 *  - PaintBehind (Inside Macintosh I, "The Window Manager": "redraws the windows in front-to-back order, beginning with
 *    startWindow ... erases the parts of the desktop within clobberedRgn ... generates update events"; the way screen
 *    savers give the screen back) from the frontmost window of all: each window there has that part erased and an update
 *    event for it, whichever application it belongs to, and the desktop's part is painted again;
 *  - the frontmost window of all: under System 7 each application's windows are in a layer of their own and WindowList
 *    ($9D6) holds only the current application's (docs/13 "Windows", which found how layers are kept, from a RAM dump of
 *    System 7.1): a layer is itself a WindowRecord, its first window in windowPic (148), its parent layer in dataHandle
 *    (130); the long at $A90 is the current layer. So the program climbs from $A90 to the root by the parent links and
 *    starts from the root's first child, the layer that holds every application's layer front to back. If anything
 *    does not hold together (System 6, whose WindowList is everything there is to paint), WindowList.
 * Then the request's seq goes into the block's done word. Toolbox traps clobber the registers the compiler expects them
 * to; the port is put back as it was.
 */
#ifndef A68PC_REPAIR_H
#define A68PC_REPAIR_H
#include <stdint.h>
#include "place.h"
#include "psmac_repair.h"

#define RP_WINDOWLIST (*(volatile uint32_t *)0x9D6)
#define RP_CURLAYER   (*(volatile uint32_t *)0xA90)          /* System 7: the current layer (docs/13 "Windows") */
#define RP_WR_DEFPROC 126
#define RP_WR_DATA    130
#define RP_WR_PIC     148

/* the window PaintBehind starts from: every application's windows behind it */
static void *rp_first(void)
{
	uint32_t wl = PL_MASK(RP_WINDOWLIST);
	if (*(volatile short *)0x15A < 0x0700) return (void *)wl;            /* SysVersion: no layers before System 7 */
	uint32_t cur = PL_MASK(RP_CURLAYER);
	if (!cur || (cur & 1)) return (void *)wl;
	uint32_t dp = PL_RD32(cur + RP_WR_DEFPROC), pic = PL_MASK(PL_RD32(cur + RP_WR_PIC));
	if (!dp || (wl && pic != wl)) return (void *)wl;                     /* not the current layer as docs/13 found it */
	uint32_t p = cur;
	for (int i = 0; i < 6; i++) {                                        /* the climb, bounded */
		if (PL_RD32(p + RP_WR_DEFPROC) != dp) return (void *)wl;          /* every layer has the layers' definition */
		uint32_t up = PL_MASK(PL_RD32(p + RP_WR_DATA));
		if (!up) {                                                       /* the root: its first child */
			uint32_t first = PL_MASK(PL_RD32(p + RP_WR_PIC));
			return first && !(first & 1) ? (void *)first : (void *)wl;
		}
		if (up & 1) return (void *)wl;
		p = up;
	}
	return (void *)wl;
}
/* the card's screen's place on the desktop (its GDevice's gdRect): 0 when there is no such screen */
static int rp_screen(int32_t screen, struct pl_rect *sr)
{
	uint32_t gdh = PL_RD32(0x8A8);                                       /* DeviceList */
	for (int n = 0; gdh && n < 16; n++) {
		uint32_t gd = PL_MASK(PL_RD32(PL_MASK(gdh)));
		if (!gd) break;
		if (pl_screen_kind(gd) == screen) {
			sr->top = (int16_t)PL_RD16(gd + 34); sr->left = (int16_t)PL_RD16(gd + 36);
			sr->bottom = (int16_t)PL_RD16(gd + 38); sr->right = (int16_t)PL_RD16(gd + 40);
			return 1;
		}
		gdh = PL_RD32(gd + 30);                                          /* gdNextGD */
	}
	if (screen) return 0;
	sr->top = sr->left = 0; sr->bottom = sr->right = 0;                  /* no device list (a one-screen Mac): global */
	return 1;
}
/* a request not carried out yet: painted again; win, the program's window (0: none), whose region is left alone. The
   number of requests carried out (0 or 1) */
static int rp_take(volatile struct a68_repair *rp, volatile uint32_t *done, void *win)
{
	uint32_t s1 = rp->seq;
	if ((s1 & 1) || s1 == *done) return 0;
	struct { int32_t screen; int16_t t, l, b, r; } q[A68_REPAIR_MAX];
	uint32_t n = rp->n;
	if (n > A68_REPAIR_MAX) n = A68_REPAIR_MAX;
	for (uint32_t i = 0; i < n; i++) {
		q[i].screen = rp->r[i].screen;
		q[i].t = rp->r[i].top; q[i].l = rp->r[i].left; q[i].b = rp->r[i].bottom; q[i].r = rp->r[i].right;
	}
	if (rp->seq != s1) return 0;                                         /* changed while copied: at the next turn */
	void *port = 0;
	GetPort(&port);
	void *all = (void *)NewRgn(), *one = (void *)NewRgn();
	if (all && one) {
		for (uint32_t i = 0; i < n; i++) {
			struct pl_rect sr;
			if (!rp_screen(q[i].screen, &sr)) continue;
			Rect g = { (short)(q[i].t + sr.top), (short)(q[i].l + sr.left), (short)(q[i].b + sr.top), (short)(q[i].r + sr.left) };
			RectRgn(one, &g);
			UnionRgn(all, one, all);
		}
		if (win) {                                                       /* never what the window shows now */
			Point o = { 0, 0 };
			SetPort(win);
			LocalToGlobal(&o);
			CopyRgn(*(void **)((char *)win + 24), one);                   /* visRgn, in the window's own coordinates */
			OffsetRgn(one, o.h, o.v);
			DiffRgn(all, one, all);
		}
		SectRgn(all, (void *)LM_GRAYRGN, all);                           /* the desktop only: never the menu bar */
		if (!EmptyRgn(all)) PaintBehind(rp_first(), all);
	}
	if (one) DisposeRgn(one);
	if (all) DisposeRgn(all);
	SetPort(port);
	*done = s1;
	return 1;
}
#endif
