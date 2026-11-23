/* SPDX-License-Identifier: MPL-2.0 */
/* mac/c/screens.h - the screens a window can be on, for the C programs (applications only: it reads
 * QuickDraw's globals through A5). Written on 23 November 2026, when every Aurora68 program was made to
 * fit the SE's own 512 x 342 screen as well as a 2560 x 1440 HDMI screen beside it (docs/08 item 134).
 *
 * Several programs had sized their windows by the desktop's bounding box (GrayRgn's), which on one
 * screen is that screen, but with the SE's screen as the main one and the HDMI screen to its right it is
 * 3072 x 1440: a window on the SE's screen then grew to the HDMI screen's height, off the SE's bottom.
 * A window's room is the screen it is (mostly) on: these give that screen, its part below the menu bar
 * and a title bar, and a rectangle moved and cut to fit inside it. Everything is static, so a program
 * that does not include this file is not changed by it.
 *
 * QuickDraw's screens (Inside Macintosh V, "Graphics Devices"): DeviceList ($8A8) links GDevices by
 * gdNextGD (30); gdFlags (20) bit 15 screenActive; gdRect (34) the screen's place on the desktop;
 * MainDevice ($8A4) the one with the menu bar. Without Color QuickDraw (ROM85 bit 14 set) there is one
 * screen, screenBits.bounds: (A5) points at thePort, the last of QuickDraw's globals, and screenBits'
 * bounds are 116 bytes below it. MBarHeight ($BAA) is the menu bar's height.
 */
#ifndef A68_SCREENS_H
#define A68_SCREENS_H
#include "toolbox.h"

#define SCR_MAX 8
#define SCR_TITLE 20                         /* a document window's title bar and its frame above the content */

static inline int scr_mbar(void) { short h = *(volatile short *)0xBAA; return h > 0 && h < 64 ? h : 20; }
static inline char *scr_deref(char **h) { return h ? *h : 0; }
static inline int scr_has_cqd(void)
{
	return !(*(volatile unsigned short *)0x28E & 0x4000) && *(char ***)0x8A8 && *(char ***)0x8A4;
}
/* the screens in use, global coordinates, the main screen (with the menu bar) first; returns how many */
static inline int scr_list(Rect *s)
{
	int n = 0;
	if (!scr_has_cqd()) {
		char *a5;
		__asm__ volatile("move.l %%a5,%0" : "=r"(a5));
		s[0] = *(Rect *)(*(char **)a5 - 116);
		return 1;
	}
	char **main = *(char ***)0x8A4;
	s[n++] = *(Rect *)(scr_deref(main) + 34);
	for (char **g = *(char ***)0x8A8; g && n < SCR_MAX; g = *(char ***)(scr_deref(g) + 30)) {
		char *gd = scr_deref(g);
		if (!gd) break;
		if (g == main || !(*(unsigned short *)(gd + 20) & 0x8000)) continue;
		s[n++] = *(Rect *)(gd + 34);
	}
	return n;
}
static inline long scr_overlap(const Rect *a, const Rect *b)
{
	int t = a->top > b->top ? a->top : b->top, l = a->left > b->left ? a->left : b->left;
	int bo = a->bottom < b->bottom ? a->bottom : b->bottom, r = a->right < b->right ? a->right : b->right;
	return bo > t && r > l ? (long)(bo - t) * (r - l) : 0;
}
/* the screen a window with this content rectangle (global) is mostly on, its title bar included; the
   main screen when it is on none. *main_out (if given) says whether that is the main screen. */
static inline Rect scr_of(const Rect *c, int *main_out)
{
	Rect s[SCR_MAX], w = *c;
	int n = scr_list(s), best = 0;
	long most = -1;
	w.top -= SCR_TITLE;
	for (int i = 0; i < n; i++) {
		long o = scr_overlap(&w, &s[i]);
		if (o > most) { most = o; best = i; }
	}
	if (most <= 0) best = 0;
	if (main_out) *main_out = best == 0;
	return s[best];
}
/* where a window's content may be on that screen: below the menu bar (the main screen) and a title bar,
   4 pixels in from the other edges (room for the frame and its shadow) */
static inline Rect scr_room(const Rect *screen, int is_main)
{
	Rect u = *screen;
	u.top += (is_main ? scr_mbar() : 0) + SCR_TITLE;
	u.left += 4; u.right -= 4; u.bottom -= 4;
	return u;
}
/* a content rectangle moved (and, where it is larger, cut) to lie inside the room of the screen it is
   mostly on; returns 1 if it changed */
static inline int scr_fit(Rect *c)
{
	int m;
	Rect s = scr_of(c, &m), u = scr_room(&s, m), o = *c;
	int w = c->right - c->left, h = c->bottom - c->top;
	if (w > u.right - u.left) w = u.right - u.left;
	if (h > u.bottom - u.top) h = u.bottom - u.top;
	if (c->left + w > u.right) c->left = (short)(u.right - w);
	if (c->top + h > u.bottom) c->top = (short)(u.bottom - h);
	if (c->left < u.left) c->left = u.left;
	if (c->top < u.top) c->top = u.top;
	c->right = (short)(c->left + w); c->bottom = (short)(c->top + h);
	return o.top != c->top || o.left != c->left || o.bottom != c->bottom || o.right != c->right;
}
/* a window's content rectangle in global coordinates (it becomes the current port) */
static inline Rect scr_content(void *win)
{
	Point p = { 0, 0 };
	SetPort(win);
	Rect r = *(Rect *)((char *)win + 16);    /* portRect */
	LocalToGlobal(&p);
	r.top += p.v; r.bottom += p.v; r.left += p.h; r.right += p.h;
	return r;
}
#endif
