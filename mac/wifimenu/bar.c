/* mac/wifimenu/bar.c - where the menu bar's extras go: the clock, the WiFi icon, the Bluetooth icon. MPL-2.0.
 *
 * Until 23 November 2026 the WiFi icon stood 76 pixels left of the menu bar's right end and the Bluetooth
 * one 23 pixels left of that, whatever menus the program had. On the SE's 512-pixel screen a program with a
 * long menu bar (Aurora68 Play: File, Edit, Controls, Video Quality, Library, Servers, Radio) ran under them,
 * and the click filter took presses meant for its last menus. Now, at every _DrawMenuBar:
 *   - the system menus' left edge is found in the menu list (MenuList, $A1C: a header of three words, then
 *     a handle and a left edge for each menu; the Help, Keyboard and Application menus have negative IDs),
 *     and the program's last menu's right edge: its left plus its title's width plus the gap the menu bar
 *     leaves after a title, measured between the program's own titles (13 pixels when there is only one);
 *   - the extras are placed right to left from the system menus, as macOS places its own: the clock, the
 *     WiFi icon, the Bluetooth icon, each with seven pixels either side (the area a press is taken in);
 *   - when they do not all fit beside the program's menus the least important go first: Bluetooth, then
 *     WiFi, then the clock's long form (the time alone is left), then the clock;
 *   - what is not drawn takes no presses (on_extra).
 * Between events (jGNEFilter) the extras are drawn again in place when what they show has changed, never
 * while a menu is down (anyone's: the menu bar's saved bits, MBSaveLoc) and never once the program's menu
 * list has grown into them (the next _DrawMenuBar places them again). Written 23 November 2026.
 */
#include "toolbox.h"
#include "menus.h"

#define LM_MENULIST  (*(void **volatile *)0xA1C)
#define LM_MBSAVELOC (*(void **volatile *)0xB5C)
#define HALF   7                    /* either side of an extra: the press area, half the gap between two */
#define EDGE   2                    /* between the rightmost press area and the system menus */

Rect icon_r, hit_r, bt_r, bt_hit, clk_r, clk_hit;
int clk_level;
static short bar_sys, bar_app, drawn_left = 0x7FFF;
static int placed;                  /* the extras were drawn by the last _DrawMenuBar (or since) */

void *mac_deref(void *h)
{
	unsigned long mp = h ? *(volatile unsigned long *)h : 0;
	if (!LM_MMU32BIT) mp &= 0x00FFFFFFUL;
	return (void *)mp;
}

int on_extra(const Rect *r, long pt)
{
	short v = (short)(pt >> 16), h = (short)pt;
	return r->left < r->right && v >= r->top && v < r->bottom && h >= r->left && h < r->right;
}

/* lastMBSave, the first word of the menu bar's saved bits: 0 when no menu is drawn (psmac_wclip.c) */
int menu_down(void)
{
	unsigned char *p = mac_deref(LM_MBSAVELOC);
	return p && *(volatile short *)p != 0;
}

/* the program's menus' right edge and the system menus' left edge, in the port's font (the system font) */
static void measure(short right, short *app, short *sys)
{
	short prev_left = -1, prev_w = 0, last_left = -1, last_w = 0, pad = 0;
	*app = 0;
	*sys = right;
	unsigned char *p = mac_deref(LM_MENULIST);
	if (!p) return;
	short last = *(short *)p;
	for (short off = 6; off <= last && off <= 6 * 64; off += 6) {
		short left = *(short *)(p + off + 4);
		unsigned char *m = mac_deref(*(void **)(p + off));
		if (!m) continue;
		if (*(short *)m < 0) {                       /* a system menu */
			if (left < *sys) *sys = left;
			continue;
		}
		short w = StringWidth(m + 14);               /* menuData: the title */
		if (prev_left >= 0 && left > prev_left && left - prev_left - prev_w > pad) pad = (short)(left - prev_left - prev_w);
		prev_left = left; prev_w = w;
		if (left >= last_left) { last_left = left; last_w = w; }
	}
	if (pad < 8) pad = 13;                           /* one title only: System 7.1's gap */
	if (pad > 24) pad = 24;
	if (last_left >= 0) *app = (short)(last_left + last_w + pad);
}

static void set_slot(Rect *r, Rect *hit, short right, short w, short h, short mbh)
{
	hit->right = right; hit->left = (short)(right - w - 2 * HALF);
	hit->top = 0; hit->bottom = mbh;
	r->right = (short)(right - HALF); r->left = (short)(r->right - w);
	r->top = (short)((mbh - h) / 2); r->bottom = (short)(r->top + h);
}
static void clear(Rect *r) { r->top = r->left = r->bottom = r->right = 0; }

/* The Window Manager's port, its clipping opened over the menu bar (and the old one kept): after a
   PopUpMenuSelect the Menu Manager left WMgrPort's clipRgn narrowed to where its menu had been, and the
   extras drawn after it, their highlight taken off, the clock's next minutes, never showed. */
static void *saved_clip;
void *wm_begin(void **save)
{
	void *wm;
	GetPort(save);
	GetWMgrPort(&wm);
	SetPort(wm);
	saved_clip = NewRgn();
	if (saved_clip) GetClip(saved_clip);
	Rect bar = *PORT_RECT(wm);
	bar.top = 0; bar.bottom = LM_MBARHEIGHT;
	ClipRect(&bar);
	return wm;
}
void wm_end(void *save)
{
	if (saved_clip) { SetClip(saved_clip); DisposeRgn(saved_clip); saved_clip = 0; }
	SetPort(save);
}

void draw_extras(int from_bar)
{
	short mbh = LM_MBARHEIGHT;
	void *save, *wm;
	if (mbh < 12) { clear(&icon_r); clear(&hit_r); clear(&bt_r); clear(&bt_hit); clear(&clk_r); clear(&clk_hit); placed = 0; return; }
#ifdef WM_DEBUG
	static int why0 = -1;
	int why = !from_bar ? (!placed ? 1 : menu_down() ? 2 : 0) : 0;
	if (why != why0) { mlog(why == 1 ? "Menu Extras: not placed" : why == 2 ? "Menu Extras: a menu is down" : "Menu Extras: drawing"); why0 = why; }
#endif
	if (!from_bar && (!placed || menu_down())) return;
	wm = wm_begin(&save);
	short font = *(short *)((char *)wm + 68), mode = *(short *)((char *)wm + 72), size = *(short *)((char *)wm + 74);
	unsigned char face = *((unsigned char *)wm + 70);
	TextFont(0); TextFace(0); TextSize(0); TextMode(1);
	short app, sys;
	measure(PORT_RECT(wm)->right, &app, &sys);
	if (from_bar) { bar_app = app; bar_sys = sys; }
	else if (sys != bar_sys || app > drawn_left) {         /* the menus changed: wait for _DrawMenuBar */
#ifdef WM_DEBUG
		mlog("Menu Extras: the menus changed");
#endif
		goto out;
	}
	short cw[3];
	cw[0] = 0;
	cw[1] = (short)clock_width(1);               /* the short form first: it takes the time both are drawn at */
	cw[2] = (short)clock_width(2);
	Rect ni, nh, nb, nbh, nc, nch;
	int lv;
	short x = 0;
	for (lv = 0; lv < 5; lv++) {
		int clk = lv <= 2 ? 2 : lv == 3 ? 1 : 0;
		x = (short)(bar_sys - EDGE);
		clear(&nc); clear(&nch); clear(&ni); clear(&nh); clear(&nb); clear(&nbh);
		if (clk) { set_slot(&nc, &nch, x, cw[clk], 12, mbh); x = nch.left; }
		if (lv <= 1) { set_slot(&ni, &nh, x, 16, 12, mbh); x = nh.left; }
		if (lv == 0) { set_slot(&nb, &nbh, x, 9, 12, mbh); x = nbh.left; }
		if (x >= bar_app) break;
	}
	if (lv == 5) x = bar_sys;
	int level = lv <= 2 ? 2 : lv == 3 ? 1 : 0;
	if (!from_bar && placed && (level != clk_level || nch.left != clk_hit.left || nch.right != clk_hit.right ||
	                            nh.left != hit_r.left || nbh.left != bt_hit.left)) {
		/* a new arrangement between two _DrawMenuBars (a wider clock, a choice in its menu): the old one
		   goes (the bar's line under it stays) */
		Rect e;
		e.top = 0; e.bottom = (short)(mbh - 1);
		e.left = drawn_left < x ? drawn_left : x;
		e.right = (short)(bar_sys - EDGE);
		if (e.left < bar_app) e.left = bar_app;
		if (e.left < e.right) EraseRect(&e);
	}
#ifdef WM_DEBUG
	if (from_bar) {                               /* the bar's measures in the Pi's log */
		char line[128], *p = line;
		int vals[6] = { bar_app, bar_sys, level, nch.left, nh.left, nbh.left };
		add(&p, "Menu Extras: app, sys, level, clock, wifi, bt:");
		for (int i = 0; i < 6; i++) {
			int v = vals[i];
			char d[8], *q = d + 7;
			*q = 0;
			do { *--q = (char)('0' + v % 10); v /= 10; } while (v > 0);
			*p++ = ' ';
			add(&p, q);
		}
		*p = 0;
		mlog(line);
	}
#endif
	icon_r = ni; hit_r = nh; bt_r = nb; bt_hit = nbh; clk_r = nc; clk_hit = nch; clk_level = level;
	drawn_left = x;
	placed = 1;
	if (level) clock_draw(wm, mbh); else clock_draw(0, mbh);
	if (ni.left < ni.right) wifi_draw(wm); else shown = -1;
	if (nb.left < nb.right) bt_draw(wm); else bt_shown = -1;
out:
	TextFont(font); TextFace(face); TextSize(size); TextMode(mode);
	wm_end(save);
}

/* the patch of _DrawMenuBar (entry.s) */
void c_draw_icon(void)
{
	draw_extras(1);
}
