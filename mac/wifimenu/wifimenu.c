/* mac/wifimenu/wifimenu.c - the WiFi menu: the Pi's WiFi from the right of the menu bar. MPL-2.0.
 *
 * The user asked for what a Mac has today: a WiFi icon in the menu bar that shows the signal, and
 * a menu under it with the networks around, the one in use ticked, a way to join one (asking for
 * its password) and to switch the radio off. System 7 has no menu-bar extras, so this is an INIT
 * that does what the clock and battery extensions of the time did:
 *   - _DrawMenuBar draws the icon after the system's menus, left of the Help and Application
 *     menus (a fan of three arcs, filled to the signal's strength, hollow when off);
 *   - a press on the icon is taken from the event stream (jGNEFilter) before the program sees it,
 *     and the WiFi menu pops up there (the first version patched _MenuSelect, which corrupted low
 *     memory under the Process Manager's patch of it: entry.s);
 *   - between events it looks at the radio every second and redraws the icon when the picture
 *     changed.
 * The radio is the Pi's: unit 0 of the network board in the card window, driven through its
 * command box exactly as PiWiFi does (mac/piwifi.s, emu68/src/alice/alice_net.h). A network
 * joined with "Remember on this Pi" ticked is written into cmdline.txt on the Pi's card once the
 * join has succeeded (wifi.ssid= and wifi.psk=, through the mailbox's PSMB_INFO_SETOPT,
 * emu68/src/psmac/psmac_card.c), so the next power-on joins it by itself. Written 18 November 2026.
 */
#include "toolbox.h"

/* ---- the card window (emu68/src/psmac/psmb.h) and the network board (alice_net.h) ---- */
#define PSMB_MAGIC    0x50534D42UL
#define BOARD         0x20000            /* the network board in the window */
#define UNIT0         (BOARD + 0x400)
#define SCANS         (BOARD + 0x1800)
#define SCAN_SIZE     56
#define U_STATE       4
#define U_SSID        220
#define U_RSSI        264
#define U_SCAN_COUNT  416
#define U_SCAN_SEQ    420
#define U_CMD_SEQ     424
#define U_CMD         428
#define U_CMD_DONE    432
#define U_ARG         440
#define U_ADMIN_OFF   628
#define SC_SSID       8
#define SC_RSSI       44
#define SC_SECURITY   52
enum { ST_OFF, ST_STARTING, ST_IDLE, ST_SCANNING, ST_JOINING, ST_CONNECTED, ST_RETRY, ST_FAILED, ST_NOFW };
enum { CMD_JOIN = 2, CMD_SCAN = 3, CMD_DISCONNECT = 4, CMD_ENABLE = 12 };
#define INFO_CMD      0x1000             /* the INFO service's command block (service 0) */
#define INFO_SETOPT   4

/* the hooks (entry.s) and what they replace */
void patch_drawmenubar(void), gne_filter(void);
void *get_tool_trap(int trap);
void set_tool_trap(void *addr, int trap);
void **mac_newhandle(long size);
int get_sr(void);
void set_sr(int sr);
void *old_drawmenubar, *old_gne;

#define LM_MBARHEIGHT (*(volatile short *)0xBAA)
#define LM_JGNEFILTER (*(void **volatile *)0x29A)
#define LM_SYSVERSION (*(volatile short *)0x15A)

static volatile unsigned char *win(void)
{
	return (volatile unsigned char *)(LM_MMU32BIT ? 0xF9000000UL : 0x00900000UL);
}
static unsigned long rd32(unsigned long off) { return *(volatile unsigned long *)(win() + off); }
static void wr32(unsigned long off, unsigned long v) { *(volatile unsigned long *)(win() + off) = v; }

#ifdef WM_DEBUG
static char dbuf[64];
static void dlog(const char *t)
{
	int n = 0;
	while (t[n] && n < 60) { dbuf[n] = t[n]; n++; }
	int sr = get_sr();
	set_sr(sr | 0x0700);
	unsigned long req = rd32(INFO_CMD), k;
	for (k = 0; rd32(INFO_CMD + 4) != req && k < 2000000; k++) ;
	*(volatile unsigned short *)(win() + INFO_CMD + 8) = 2;
	wr32(INFO_CMD + 28, (unsigned long)dbuf);
	wr32(INFO_CMD + 32, (unsigned long)n);
	wr32(INFO_CMD, req + 1);
	for (k = 0; rd32(INFO_CMD + 4) != req + 1 && k < 4000000; k++) ;
	set_sr(sr);
}
static unsigned long slot0;
static void probe(const char *step)
{
	unsigned long v = *(volatile unsigned long *)0x774;
	if (v != slot0) { dlog(step); slot0 = v; }
}
#define PROBE(s) probe(s)
#else
#define PROBE(s)
#endif

/* ---- the icon: 16 x 12, a dot and three arcs; level 0-3 filled, the rest dotted ---- */
static unsigned short icons[6][12];      /* 0-3 signal, 4 off, 5 searching */
static struct { void *base; short rowBytes; Rect bounds; } bm;
static Rect icon_r, hit_r;
static int shown = -1;

static void make_icons(void)
{
	for (int lv = 0; lv < 6; lv++)
		for (int y = 0; y < 12; y++) {
			unsigned short row = 0;
			for (int x = 0; x < 16; x++) {
				int dx = 2 * x + 1 - 16, dy = 2 * (11 - y) + 1, r2 = dx * dx + dy * dy;
				int adx = dx < 0 ? -dx : dx, ring = -1;
				if (r2 <= 12)
					ring = 0;
				else if (adx * 10 <= dy * 11) {
					if (r2 >= 49 && r2 <= 100) ring = 1;
					else if (r2 >= 196 && r2 <= 289) ring = 2;
					else if (r2 >= 400 && r2 <= 529) ring = 3;
				}
				if (ring < 0) continue;
				int faint = (x + y) & 1, on;
				if (lv == 4) on = ring > 0 && faint;                    /* off: no dot, faint arcs */
				else if (lv == 5) on = ring == 0 || faint;               /* looking: the dot, faint arcs */
				else on = ring <= lv || faint;                           /* the signal: solid up to lv */
				if (on) row |= (unsigned short)(0x8000 >> x);
			}
			icons[lv][y] = row;
		}
}

static int rssi_bars(int rssi) { return rssi >= -55 ? 3 : rssi >= -67 ? 2 : rssi >= -78 ? 1 : 0; }
static int picture(void)
{
	unsigned long st = rd32(UNIT0 + U_STATE);
	if (rd32(UNIT0 + U_ADMIN_OFF) || st == ST_OFF || st == ST_NOFW) return 4;
	if (st == ST_CONNECTED) return rssi_bars((int)rd32(UNIT0 + U_RSSI));
	return 5;
}

void c_draw_icon(void)
{
	void *save, *wm;
	short mbh = LM_MBARHEIGHT;
	if (mbh < 12) return;
	GetPort(&save);
	GetWMgrPort(&wm);
	SetPort(wm);
	Rect *pr = PORT_RECT(wm);
	icon_r.right = pr->right - 76;
	icon_r.left = icon_r.right - 16;
	icon_r.top = (short)((mbh - 12) / 2);
	icon_r.bottom = icon_r.top + 12;
	hit_r = icon_r;
	InsetRect(&hit_r, -6, 0);
	hit_r.top = 0;
	hit_r.bottom = mbh;
	shown = picture();
	bm.base = icons[shown];
	bm.rowBytes = 2;
	bm.bounds.top = 0; bm.bounds.left = 0; bm.bounds.bottom = 12; bm.bounds.right = 16;
	CopyBits(&bm, (char *)wm + 2, &bm.bounds, &icon_r, 0, 0);
	SetPort(save);
}

/* ---- the radio's command box ---- */
static int box_free(void) { return rd32(UNIT0 + U_CMD_DONE) == rd32(UNIT0 + U_CMD_SEQ); }
static int command(unsigned long cmd, const void *arg, int len)
{
	if (!box_free()) return -1;
	for (int i = 0; i < len; i++) win()[UNIT0 + U_ARG + i] = ((const unsigned char *)arg)[i];
	wr32(UNIT0 + U_CMD, cmd);
	wr32(UNIT0 + U_CMD_SEQ, rd32(UNIT0 + U_CMD_SEQ) + 1);
	return 0;
}

/* ---- cmdline.txt, through the mailbox ---- */
static char edit[300];
static int set_options(const char *text, unsigned long len)
{
	int sr = get_sr(), ok = -1;
	set_sr(sr | 0x0700);                           /* the INFO block is shared with the drivers' logs */
	unsigned long req = rd32(INFO_CMD), n;
	for (n = 0; rd32(INFO_CMD + 4) != req && n < 2000000; n++) ;
	*(volatile unsigned short *)(win() + INFO_CMD + 8) = INFO_SETOPT;
	wr32(INFO_CMD + 28, (unsigned long)text);
	wr32(INFO_CMD + 32, len);
	wr32(INFO_CMD, req + 1);
	for (n = 0; rd32(INFO_CMD + 4) != req + 1 && n < 4000000; n++) ;
	if (rd32(INFO_CMD + 4) == req + 1) ok = (long)rd32(INFO_CMD + 36) == 0 ? 0 : -1;
	set_sr(sr);
	return ok;
}
static char save_ssid[36], save_pass[68];
static int save_pending;
static void add(char **p, const char *s) { while (*s) *(*p)++ = *s++; }
static void save_network(void)
{
	char *p = edit;
	add(&p, "-wifi.ssid\n-wifi.psk\n+wifi.ssid=\"");
	add(&p, save_ssid);
	add(&p, "\"\n");
	if (save_pass[0]) { add(&p, "+wifi.psk=\""); add(&p, save_pass); add(&p, "\"\n"); }
	set_options(edit, (unsigned long)(p - edit));
	for (unsigned i = 0; i < sizeof save_pass; i++) save_pass[i] = 0;
}

/* between events: redraw when the picture changes; save a network once it has been joined */
static unsigned long next_look;
void c_tick(void)
{
	unsigned long t = LM_TICKS;
	if (t < next_look) return;
	next_look = t + 60;
	if (save_pending) {
		unsigned long st = rd32(UNIT0 + U_STATE);
		if (st == ST_CONNECTED) { save_pending = 0; save_network(); }
		else if (st == ST_FAILED || st == ST_IDLE || --save_pending == 0) { save_pending = 0; for (unsigned i = 0; i < sizeof save_pass; i++) save_pass[i] = 0; }
	}
	PROBE("wm: tick before draw");
	if (picture() != shown) c_draw_icon();
	PROBE("wm: tick after draw");
}

/* ---- the password dialog: Join, Cancel, the text, the field, Remember ---- */
static void put_item(unsigned char **p, short t, short l, short b, short r, unsigned char kind, const char *text)
{
	unsigned char *q = *p;
	int n = 0;
	while (text[n]) n++;
	for (int i = 0; i < 4; i++) *q++ = 0;
	short v[4] = { t, l, b, r };
	for (int i = 0; i < 4; i++) { *q++ = (unsigned char)(v[i] >> 8); *q++ = (unsigned char)v[i]; }
	*q++ = kind;
	*q++ = (unsigned char)n;
	for (int i = 0; i < n; i++) *q++ = (unsigned char)text[i];
	if (n & 1) *q++ = 0;
	*p = q;
}
static int ask_password(const char *ssid, char *pass, int *remember)
{
	char prompt[96];
	char *p = prompt;
	add(&p, "The Wi-Fi network \xD2");
	add(&p, ssid);
	add(&p, "\xD3 needs a password.");
	*p = 0;
	void **items = mac_newhandle(400);
	if (!items) return 0;
	unsigned char *q = (unsigned char *)*items;
	*q++ = 0; *q++ = 5;                            /* six items */
	put_item(&q, 100, 290, 120, 360, 4, "Join");
	put_item(&q, 100, 210, 120, 280, 4, "Cancel");
	put_item(&q, 10, 60, 42, 360, 8 | 0x80, prompt);
	put_item(&q, 48, 140, 64, 356, 16, "");
	put_item(&q, 72, 60, 90, 360, 5, "Remember on this Pi (cmdline.txt)");
	put_item(&q, 48, 60, 64, 136, 8 | 0x80, "Password:");
	void *wm;
	GetWMgrPort(&wm);
	Rect *pr = PORT_RECT(wm);
	Rect r;
	r.left = (short)((pr->right - 372) / 2); r.right = r.left + 372;
	r.top = 60; r.bottom = r.top + 130;
	unsigned char title[2] = { 0, 0 };
	void *dlg = (void *)NewDialog(0, &r, title, 1, 1, (void *)-1L, 0, 0, items);
	if (!dlg) return 0;
	short kind, hit = 0;
	void *h;
	Rect box;
	GetDItem(dlg, 5, &kind, &h, &box);
	SetCtlValue(h, 1);
	SelIText(dlg, 4, 0, 32767);
	for (;;) {
		ModalDialog(0, &hit);
		if (hit == 1 || hit == 2) break;
		if (hit == 5) { GetDItem(dlg, 5, &kind, &h, &box); SetCtlValue(h, !GetCtlValue(h)); }
	}
	unsigned char s[256];
	GetDItem(dlg, 4, &kind, &h, &box);
	GetIText(h, s);
	GetDItem(dlg, 5, &kind, &h, &box);
	*remember = GetCtlValue(h);
	DisposDialog(dlg);
	if (hit != 1) return 0;
	int n = s[0] > 63 ? 63 : s[0];
	for (int i = 0; i < n; i++) pass[i] = (char)s[i + 1];
	pass[n] = 0;
	return 1;
}

/* ---- the menu ---- */
#define MENU_ID 23457
static char ssids[16][36];
static int nets, secured[16];
static unsigned long last_scan;
static unsigned char *pcat(unsigned char *ps, const char *c) { while (*c && ps[0] < 255) ps[++ps[0]] = (unsigned char)*c++; return ps; }

static void join(const char *ssid, int is_secured)
{
	unsigned char j[108];
	char pass[64];
	int remember = 1;
	pass[0] = 0;
	if (is_secured && !ask_password(ssid, pass, &remember)) return;
	for (int i = 0; i < 108; i++) j[i] = 0;
	for (int i = 0; ssid[i] && i < 35; i++) j[i] = (unsigned char)ssid[i];
	for (int i = 0; pass[i] && i < 67; i++) j[36 + i] = (unsigned char)pass[i];
	if (command(CMD_JOIN, j, sizeof j)) { SysBeep(10); return; }
	if (remember) {
		int quote = 0;
		for (int i = 0; ssid[i]; i++) if (ssid[i] == '"') quote = 1;
		for (int i = 0; pass[i]; i++) if (pass[i] == '"') quote = 1;
		if (!quote) {
			for (int i = 0; i < 36; i++) save_ssid[i] = i < 35 ? ssid[i] : 0;
			for (int i = 0; i < 64; i++) save_pass[i] = pass[i];
			save_pending = 40;                     /* looks, a second apart */
		}
	}
	for (int i = 0; i < 64; i++) pass[i] = 0;
}

static int c_menuselect(long pt)
{
#ifdef WM_DEBUG
	slot0 = *(volatile unsigned long *)0x774;
	dlog("wifimenu: click");
#endif
	short v = (short)(pt >> 16), h = (short)pt;
	if (shown < 0 || LM_MBARHEIGHT < 12 || v < hit_r.top || v >= hit_r.bottom || h < hit_r.left || h >= hit_r.right)
		return 0;
	if (rd32(0) != PSMB_MAGIC) return 0;
	unsigned long st = rd32(UNIT0 + U_STATE);
	int off = rd32(UNIT0 + U_ADMIN_OFF) || st == ST_OFF;
	int connected = st == ST_CONNECTED;
	/* a fresh scan, as a Mac does when its WiFi menu opens */
	if (!off && (st == ST_IDLE || st == ST_CONNECTED) && LM_TICKS - last_scan > 20 * 60 && box_free()) {
		command(CMD_SCAN, 0, 0);
		last_scan = LM_TICKS;
	}
	PROBE("wm: after scan cmd");
	char cur[36];
	for (int i = 0; i < 36; i++) cur[i] = (char)win()[UNIT0 + U_SSID + i];
	cur[35] = 0;
	unsigned char s[256];
	void *m = (void *)NewMenu(MENU_ID, (const unsigned char *)"\x05Wi-Fi");
	PROBE("wm: after NewMenu");
	if (!m) return 1;
	int n = 0, i_toggle, i_first = 0, i_scan, i_disc;
	s[0] = 0;
	AppendMenu(m, (const unsigned char *)"\x01 "); n++;
	SetItem(m, n, pcat(s, off ? "Wi-Fi: Off" : st == ST_CONNECTED ? "Wi-Fi: On" : st == ST_JOINING || st == ST_RETRY ? "Wi-Fi: Joining\xC9" :
	                      st == ST_FAILED ? "Wi-Fi: Could not join" : st == ST_STARTING || st == ST_NOFW ? "Wi-Fi: Starting\xC9" : "Wi-Fi: Not connected"));
	DisableItem(m, n);
	AppendMenu(m, (const unsigned char *)"\x01 "); n++; i_toggle = n;
	s[0] = 0;
	SetItem(m, n, pcat(s, off ? "Turn Wi-Fi On" : "Turn Wi-Fi Off"));
	AppendMenu(m, (const unsigned char *)"\x02(-"); n++;
	nets = 0;
	if (!off) {
		unsigned long cnt = rd32(UNIT0 + U_SCAN_COUNT);
		if (cnt > 36) cnt = 36;
		for (unsigned long k = 0; k < cnt && nets < 16; k++) {
			unsigned long e = SCANS + k * SCAN_SIZE;
			char nm[36];
			for (int i = 0; i < 36; i++) nm[i] = (char)win()[e + SC_SSID + i];
			nm[35] = 0;
			if (!nm[0]) continue;
			int dup = 0;
			for (int q = 0; q < nets; q++) { int same = 1; for (int i = 0; i < 36; i++) { if (ssids[q][i] != nm[i]) { same = 0; break; } if (!nm[i]) break; } if (same) dup = 1; }
			if (dup) continue;
			for (int i = 0; i < 36; i++) ssids[nets][i] = nm[i];
			secured[nets] = (rd32(e + SC_SECURITY) & ~1UL) != 0;      /* SEC_OPEN is 1 (alice/wifi/fmac.h) */
			int bars = rssi_bars((int)rd32(e + SC_RSSI)) + 1;
			AppendMenu(m, (const unsigned char *)"\x01 "); n++;
			if (!i_first) i_first = n;
			s[0] = 0;
			pcat(s, nm);
			pcat(s, "  ");
			for (int b = 0; b < bars; b++) pcat(s, "\xA5");          /* bullets: the signal */
			if (!secured[nets]) pcat(s, "  (open)");
			SetItem(m, n, s);
			if (connected) { int same = 1; for (int i = 0; i < 36; i++) { if (cur[i] != nm[i]) { same = 0; break; } if (!nm[i]) break; } if (same) SetItemMark(m, n, 0x12); }
			nets++;
		}
		if (!nets) {
			AppendMenu(m, (const unsigned char *)"\x01 "); n++;
			s[0] = 0;
			SetItem(m, n, pcat(s, st == ST_SCANNING ? "Looking for networks\xC9" : "No networks found"));
			DisableItem(m, n);
		}
		AppendMenu(m, (const unsigned char *)"\x02(-"); n++;
	}
	AppendMenu(m, (const unsigned char *)"\x01 "); n++; i_scan = n;
	s[0] = 0;
	SetItem(m, n, pcat(s, "Look for Networks"));
	if (off) DisableItem(m, n);
	AppendMenu(m, (const unsigned char *)"\x01 "); n++; i_disc = n;
	s[0] = 0;
	SetItem(m, n, pcat(s, "Disconnect"));
	if (!connected) DisableItem(m, n);
	AppendMenu(m, (const unsigned char *)"\x01 "); n++;
	int i_forget = n;
	s[0] = 0;
	SetItem(m, n, pcat(s, "Forget the Network Saved on the Pi"));

	PROBE("wm: after items");
	InsertMenu(m, -1);
	PROBE("wm: after InsertMenu");
	InvertRect(&icon_r);
	long r = PopUpMenuSelect(m, LM_MBARHEIGHT + 1, icon_r.left - 4, 1);
	PROBE("wm: after popup");
	DeleteMenu(MENU_ID);
	PROBE("wm: after DeleteMenu");
	DisposeMenu(m);
	c_draw_icon();
	PROBE("wm: after Dispose+draw");
	int item = (short)(r & 0xFFFF);
	if ((r >> 16) == MENU_ID && item) {
		if (item == i_toggle) {
			unsigned long on = off ? 1 : 0;
			command(CMD_ENABLE, &on, 4);
		} else if (item == i_scan) {
			command(CMD_SCAN, 0, 0);
			last_scan = LM_TICKS;
		} else if (item == i_disc) {
			command(CMD_DISCONNECT, 0, 0);
		} else if (item == i_forget) {
			static const char fg[] = "-wifi.ssid\n-wifi.psk\n";
			if (set_options(fg, sizeof fg - 1)) SysBeep(10);
		} else if (i_first && item >= i_first && item < i_first + nets) {
			int k = item - i_first;
			int same = connected;
			for (int i = 0; i < 36 && same; i++) { if (cur[i] != ssids[k][i]) same = 0; if (!cur[i]) break; }
			if (!same) join(ssids[k], secured[k]);
		}
	}
	return 1;
}

/* jGNEFilter: the time, and a press on the icon */
int c_filter(EventRecord *ev)
{
	c_tick();
	if (ev->what != mouseDown || LM_MBARHEIGHT < 12) return 0;
	if (!c_menuselect(pt_long(ev->where))) return 0;
	ev->what = nullEvent;
	return 1;
}

void init_main(void)
{
	if (LM_SYSVERSION < 0x0700) return;            /* PopUpMenuSelect and the rest are System 7's */
	if (rd32(0) != PSMB_MAGIC) return;             /* not an Aurora68 Mac */
	make_icons();
	old_drawmenubar = get_tool_trap(0xA937);
	set_tool_trap((void *)patch_drawmenubar, 0xA937);
	old_gne = (void *)LM_JGNEFILTER;
	LM_JGNEFILTER = (void **)gne_filter;
}
