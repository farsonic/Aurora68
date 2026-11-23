/* mac/wifimenu/clock.c - the clock in the menu bar, as macOS has it. MPL-2.0.
 *
 * The owner asked on 23 November 2026 for "a clock extension next to wifi that shows time and lets me adjust
 * how it is shown, similar to what macOS has". It is part of the menu bar's extras (bar.c places it, rightmost,
 * beside the system menus; wifimenu.c and btmenu.c are the other two):
 *   - the time in the system font, drawn again when the minute (or, with seconds or flashing separators, the
 *     second) moves on: in srcCopy text mode, each glyph's box painted with it, so nothing is erased first and
 *     nothing flickers;
 *   - or, "View as Analog", a 15-pixel clock face with its two hands;
 *   - the options of macOS's Clock: the date, the day of the week, AM/PM, a 24-hour clock, seconds, flashing
 *     time separators. The defaults: the day of the week and the time, 12 or 24 hours as the Mac's own setting
 *     says (the 'itl0' resource's timeCycle, which Date & Time and General Controls set), AM/PM shown;
 *   - the region's formats from the International Utilities: the hour cycle, the leading zero, the time
 *     separator and the morning and evening strings from 'itl0', the order of day and month from its
 *     dateOrder, the names of the days and months from 'itl1' (cut to its abbrLen, three letters in English),
 *     the menu's first line from IUDateString's long date ("Monday, November 23, 2026" in the United States,
 *     "Monday, 23 November 2026" with a British 'itl1');
 *   - a press on it pops up a menu: the full date (dimmed), the time of the last NTP sync when Riptide's NTP is
 *     on (dimmed; below), View as Digital / View as Analog, the options as checked items, and "Date & Time..."
 *     (System 7.1's General Controls when there is no Date & Time): the Finder is brought to the front and asked
 *     to open the control panel ('FNDR' 'sope', the Finder event the Apple menu uses; System 7.1's Finder takes no
 *     'aevt' 'odoc', docs/08 item 90), from the Finder's own GetNextEvent, because a program that takes no Apple
 *     events cannot send one (noPortErr);
 *   - the choices are kept in "Aurora68 Menu Extras Prefs" in the Preferences folder (FindFolder), eight bytes.
 * Riptide's NTP: when Riptide is the Mac's TCP/IP (its .IPP driver open and answering RTMAC_PING), the menu asks
 * it for its status table (net.riptide.status, RTMAC_PORT with RTCMD_TABLES "42": riptide include/rt/rt_ctl.h,
 * src/mac/prefs/mac_ipp_private.h; one Control call, no waiting) and shows "Last NTP sync: 5 minutes ago".
 */
#include "toolbox.h"
#include "menus.h"

/* the File, Device and Process Managers, the Apple Event Manager (entry.s) */
short pb_close(void *), pb_read(void *), pb_write(void *), pb_control(void *), pb_seteof(void *), pb_flushvol(void *),
      pb_hopen(void *), pb_hcreate(void *), pb_hdelete(void *), pb_hgetfinfo(void *), pb_hsetfinfo(void *),
      pb_catmove(void *), pb_dircreate(void *), pb_getfcbinfo(void *), pb_getcatinfo(void *);
typedef struct { short year, month, day, hour, minute, second, dayOfWeek; } DateTimeRec;
void mac_secs2date(unsigned long secs, DateTimeRec *d);
typedef struct { unsigned long hi, lo; } PSN;
short pm_current(PSN *), pm_next(PSN *), pm_setfront(const PSN *), pm_info(const PSN *, void *info);
typedef struct { unsigned long type; void **data; } AEDesc;
short ae_create_desc(unsigned long type, const void *data, long size, AEDesc *result), ae_put_desc(AEDesc *list, long index, const AEDesc *d),
      ae_put_param_desc(AEDesc *ev, unsigned long key, const AEDesc *d), ae_dispose_desc(AEDesc *d), ae_create_list(AEDesc *list),
      ae_create_event(unsigned long cls, unsigned long id, const AEDesc *target, AEDesc *ev), ae_send(const AEDesc *ev, AEDesc *reply, long mode);
short new_alias(const void *from, const void *target, void ***alias);
void mac_hlock(void *h), mac_dispose_handle(void *h), mac_setzone(void *zone);
typedef struct { short vRefNum; long parID; unsigned char name[64]; } FSSpec;

#define W(p, o) (*(short *)((char *)(p) + (o)))
#define L(p, o) (*(long *)((char *)(p) + (o)))
#define P(p, o) (*(void **)((char *)(p) + (o)))
static long pb[28];                               /* a parameter block, 112 bytes: CInfoPBRec is the longest */
static void pb_clear(void) { memset(pb, 0, sizeof pb); }
#define ON_SYSTEM_DISK (-32768)

/* ---- the settings ---- */
#define F_ANALOG 0x01
#define F_DATE   0x02
#define F_DAY    0x04
#define F_AMPM   0x08
#define F_SET24  0x10                             /* chosen in the menu; without it the Mac's own setting */
#define F_24     0x20
#define F_SECS   0x40
#define F_FLASH  0x80
#define PREF_MAGIC TYPE('A', '6', '8', 'C')
static unsigned short cfg = F_DAY | F_AMPM;
static int cfg_loaded;
static const unsigned char pref_name[] = "\x1A" "Aurora68 Menu Extras Prefs";

static void load_prefs(void)
{
	short v, ref;
	long d, buf[2];
	cfg_loaded = 1;
	if (FindFolder(ON_SYSTEM_DISK, TYPE('p', 'r', 'e', 'f'), 0, &v, &d)) return;
	pb_clear();
	P(pb, 18) = (void *)pref_name; W(pb, 22) = v; L(pb, 48) = d; *((char *)pb + 27) = 1;   /* fsRdPerm */
	if (pb_hopen(pb)) return;
	ref = W(pb, 24);
	pb_clear();
	W(pb, 24) = ref; P(pb, 32) = buf; L(pb, 36) = sizeof buf; W(pb, 44) = 1;              /* fsFromStart */
	short e = pb_read(pb);
	long got = L(pb, 40);
	pb_clear(); W(pb, 24) = ref; pb_close(pb);
	if (!e && got == sizeof buf && buf[0] == (long)PREF_MAGIC && (buf[1] >> 16) == 1) cfg = (unsigned short)buf[1];
}
static void save_prefs(void)
{
	short v, ref;
	long d, buf[2] = { (long)PREF_MAGIC, (1L << 16) | cfg };
	if (FindFolder(ON_SYSTEM_DISK, TYPE('p', 'r', 'e', 'f'), 1, &v, &d)) return;
	pb_clear();
	P(pb, 18) = (void *)pref_name; W(pb, 22) = v; L(pb, 48) = d;
	if (!pb_hcreate(pb)) {                        /* a new file: say whose it is */
		pb_clear();
		P(pb, 18) = (void *)pref_name; W(pb, 22) = v; L(pb, 48) = d;
		if (!pb_hgetfinfo(pb)) {
			L(pb, 32) = TYPE('p', 'r', 'e', 'f'); L(pb, 36) = TYPE('P', 'S', 'W', 'M');
			L(pb, 48) = d;                        /* GetFInfo leaves the file number there */
			pb_hsetfinfo(pb);
		}
	}
	pb_clear();
	P(pb, 18) = (void *)pref_name; W(pb, 22) = v; L(pb, 48) = d; *((char *)pb + 27) = 3;   /* fsRdWrPerm */
	if (pb_hopen(pb)) return;                     /* a locked disk: the choice lasts until the restart */
	ref = W(pb, 24);
	pb_clear();
	W(pb, 24) = ref; P(pb, 32) = buf; L(pb, 36) = sizeof buf; W(pb, 44) = 1;
	pb_write(pb);
	pb_clear(); W(pb, 24) = ref; L(pb, 28) = sizeof buf; pb_seteof(pb);
	pb_clear(); W(pb, 24) = ref; pb_close(pb);
	pb_clear(); W(pb, 22) = v; pb_flushvol(pb);
}

/* ---- the region's formats ---- */
static unsigned char i0[32];                      /* a copy of 'itl0' (Intl0Rec) */
static unsigned char names[7 + 12][16];           /* the days (Sunday first) and the months, from 'itl1' */
static int abbr = 3;
static void get_intl(void)
{
	static const char dflt[] = "SunMonTueWedThuFriSatJanFebMarAprMayJunJulAugSepOctNovDec";
	unsigned char *p = mac_deref(IUGetIntl(0, 6));           /* each copied before the next is asked for: */
	if (p) memcpy(i0, p, sizeof i0);                         /* loading one may move the other */
	else { memset(i0, 0, sizeof i0); i0[10] = 255; i0[11] = 0x40; i0[20] = ':'; memcpy(i0 + 12, " AM\0 PM\0", 8); }
	p = mac_deref(IUGetIntl(1, 6));
	if (p) {
		memcpy(names, p, sizeof names);                      /* days: 7 x STRING[15], then months: 12 x STRING[15] */
		abbr = p[307];                                       /* abbrLen */
	} else {
		for (int i = 0; i < 19; i++) { names[i][0] = 3; memcpy(names[i] + 1, dflt + 3 * i, 3); }
		abbr = 3;
	}
	if (abbr < 1 || abbr > 15) abbr = 3;
}
static int is24(void)
{
	if (cfg & F_SET24) return (cfg & F_24) != 0;
	return i0[10] == 0;                                      /* timeCycle: 0 the 24-hour clock */
}

static void put_c(unsigned char *s, int c) { if (s[0] < 254) s[++s[0]] = (unsigned char)c; }
static void put_n(unsigned char *s, int n, int two) { if (two || n >= 10) put_c(s, '0' + n / 10); put_c(s, '0' + n % 10); }
static void put_name(unsigned char *s, const unsigned char *n)
{
	int k = n[0] < abbr ? n[0] : abbr;
	for (int i = 1; i <= k; i++) put_c(s, n[i]);
}

/* The text in the bar: level 2 the long form (the day, the date, the time with its seconds), 1 the time alone.
   sep[]: where the time separators are, for flashing them. */
static unsigned char sep[2];
static int nsep;
static unsigned long snap;                        /* the time being drawn, the same for the width and the drawing */
static void build(unsigned char *s, int level)
{
	DateTimeRec d;
	mac_secs2date(snap, &d);
	get_intl();
	s[0] = 0;
	nsep = 0;
	if (level == 2 && (cfg & F_DAY) && d.dayOfWeek >= 1 && d.dayOfWeek <= 7) { put_name(s, names[d.dayOfWeek - 1]); put_c(s, ' '); }
	if (level == 2 && (cfg & F_DATE) && d.month >= 1 && d.month <= 12) {
		int order = i0[7];                                   /* dateOrder: mdy, dmy, ymd, myd, dym, ydm */
		if (order == 0 || order == 2 || order == 3) { put_name(s, names[6 + d.month]); put_c(s, ' '); put_n(s, d.day, 0); }
		else { put_n(s, d.day, 0); put_c(s, ' '); put_name(s, names[6 + d.month]); }
		put_c(s, ' ');
	}
	int h24 = is24(), hr = d.hour;
	if (!h24) {
		hr %= 12;
		if (hr == 0 && i0[10] != 1) hr = 12;                 /* timeCycleZero (1) counts from 0 */
	}
	int tsep = i0[20] ? i0[20] : ':';
	put_n(s, hr, (i0[11] & 0x20) != 0);                      /* timeFmt's hrLeadingZ */
	sep[nsep++] = s[0] + 1;
	put_c(s, tsep);
	put_n(s, d.minute, 1);
	if (level == 2 && (cfg & F_SECS)) { sep[nsep++] = s[0] + 1; put_c(s, tsep); put_n(s, d.second, 1); }
	if (!h24 && (cfg & F_AMPM)) {
		const unsigned char *a = i0 + (d.hour < 12 ? 12 : 16);   /* mornStr, eveStr: four characters */
		int n = 0;
		while (n < 4 && a[n]) n++;
		while (n > 0 && a[n - 1] == ' ') n--;
		if (n == 0) { a = (const unsigned char *)(d.hour < 12 ? " AM" : " PM"); n = 3; }
		for (int i = 0; i < n; i++) put_c(s, a[i]);
	}
}

/* ---- the clock face: 15 x 15, a ring of radius 7, the hands from the middle ---- */
static const signed char sin60[60] = {
	0, 10, 21, 31, 41, 50, 59, 67, 74, 81, 87, 91, 95, 98, 99, 100, 99, 98, 95, 91, 87, 81, 74, 67, 59, 50, 41, 31, 21, 10,
	0, -10, -21, -31, -41, -50, -59, -67, -74, -81, -87, -91, -95, -98, -99, -100, -99, -98, -95, -91, -87, -81, -74, -67, -59,
	-50, -41, -31, -21, -10 };
static unsigned short face[15];
static struct { void *base; short rowBytes; Rect bounds; } fbm;
static int rdiv(int n, int d) { return n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d); }
static void plot(int x, int y) { if (x >= 0 && x < 15 && y >= 0 && y < 15) face[y] |= (unsigned short)(0x8000 >> x); }
static void hand(int idx, int len10)             /* len10: the length in tenths of a pixel */
{
	int x1 = 7 + rdiv(len10 * sin60[idx % 60], 1000), y1 = 7 - rdiv(len10 * sin60[(idx + 15) % 60], 1000);
	int dx = x1 - 7, dy = y1 - 7, sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1, x = 7, y = 7;
	dx *= sx; dy *= sy;
	int err = dx - dy;
	for (;;) {                                   /* Bresenham */
		plot(x, y);
		if (x == x1 && y == y1) break;
		int e2 = 2 * err;
		if (e2 > -dy) { err -= dy; x += sx; }
		if (e2 < dx) { err += dx; y += sy; }
	}
}
static void make_face(void)
{
	DateTimeRec d;
	mac_secs2date(snap, &d);
	memset(face, 0, sizeof face);
	for (int y = 0; y < 15; y++)
		for (int x = 0; x < 15; x++) {
			int r2 = (x - 7) * (x - 7) + (y - 7) * (y - 7);
			if (r2 >= 43 && r2 <= 56) plot(x, y);
		}
	hand(d.minute, 55);
	hand(((d.hour % 12) * 60 + d.minute) / 12, 35);
}

/* ---- in the bar ---- */
static unsigned long last_q = 0xFFFFFFFFUL;
static unsigned long divisor(int level)
{
	if (level == 0 || (cfg & F_ANALOG)) return 60;
	if ((cfg & F_FLASH) || (level == 2 && (cfg & F_SECS))) return 1;
	return 60;
}
void clock_init(void) { snap = LM_TIME; }
int clock_width(int level)
{
	unsigned char s[256];
	if (!cfg_loaded) load_prefs();
	if (level == 1) snap = LM_TIME;               /* bar.c asks for the short form first */
	if (cfg & F_ANALOG) return 15;
	build(s, level);
	return StringWidth(s);
}
int clock_due(void)
{
	if (!cfg_loaded) return 0;
	return LM_TIME / divisor(clk_level) != last_q;
}
void clock_draw(void *wm, short mbh)
{
	last_q = snap / divisor(clk_level);
	if (!wm) return;
	if (cfg & F_ANALOG) {
		Rect r = clk_r;
		r.top = (short)((mbh - 1 - 15) / 2); r.bottom = (short)(r.top + 15);
		make_face();
		fbm.base = face; fbm.rowBytes = 2;
		fbm.bounds.top = 0; fbm.bounds.left = 0; fbm.bounds.bottom = 15; fbm.bounds.right = 15;
		CopyBits(&fbm, (char *)wm + 2, &fbm.bounds, &r, 0, 0);
		return;
	}
	unsigned char s[256];
	short fi[4];                                  /* FontInfo: ascent, descent, widMax, leading */
	build(s, clk_level);
	GetFontInfo(fi);
	short base = (short)((mbh - 1 - (fi[0] + fi[1])) / 2 + fi[0]);
	short w = StringWidth(s), x = (short)(clk_r.right - w);
	TextMode(0);                                  /* srcCopy: each glyph's box painted, nothing erased first */
	MoveTo(x, base);
	DrawString(s);
	TextMode(1);
	Rect e;
	e.top = (short)(base - fi[0]); e.bottom = (short)(base + fi[1]);
	if (x > clk_r.left) { e.left = clk_r.left; e.right = x; EraseRect(&e); }
	if ((cfg & F_FLASH) && (snap & 1))            /* the separators off every other second */
		for (int i = 0; i < nsep; i++) {
			e.left = (short)(x + TextWidth(s + 1, 0, sep[i] - 1));
			e.right = (short)(e.left + TextWidth(s + 1, sep[i] - 1, 1));
			EraseRect(&e);
		}
}

/* ---- Riptide's NTP ---- */
#define RTMAC_PING   0x5250
#define RTMAC_PORT   0x5251
#define RTMAC_MAGIC  0x52495054UL                  /* 'RIPT' */
#define RTCMD_TABLES 4
#define LM_UTABLEBASE (*(void ***volatile *)0x11C)
#define LM_UNITNTRYCNT (*(volatile short *)0x1D2)
static long reply[64];
static int ntp_line(unsigned char *s)
{
	short ref = 0;
	void ***ut = LM_UTABLEBASE;
	for (short i = 0; ut && i < LM_UNITNTRYCNT && !ref; i++) {
		unsigned char *dce = mac_deref(ut[i]);
		if (!dce) continue;
		short fl = *(short *)(dce + 4);
		if (!(fl & 0x20)) continue;                   /* dOpened */
		unsigned char *drv = *(unsigned char **)dce;
		drv = (fl & 0x40) ? mac_deref(drv) : (unsigned char *)((unsigned long)drv & (LM_MMU32BIT ? ~0UL : 0x00FFFFFFUL));
		if (drv && drv[18] == 4 && drv[19] == '.' && drv[20] == 'I' && drv[21] == 'P' && drv[22] == 'P') ref = (short)~i;
	}
	if (!ref) return 0;
	pb_clear();
	W(pb, 24) = ref; W(pb, 26) = RTMAC_PING;
	if (pb_control(pb) || (unsigned long)L(pb, 28) != RTMAC_MAGIC) return 0;   /* MacTCP's .IPP */
	static char text[4] = "42";
	pb_clear();
	W(pb, 24) = ref; W(pb, 26) = RTMAC_PORT;
	L(pb, 28) = RTCMD_TABLES; P(pb, 32) = text; L(pb, 36) = 3; P(pb, 40) = reply; L(pb, 44) = sizeof reply;
	if (pb_control(pb) || L(pb, 48) || (unsigned long)L(pb, 52) < 16 + 48) return 0;
	unsigned char *r = (unsigned char *)reply;
	if (L(r, 0) != 42 || L(r, 4) < 8 + 48 || L(r, 12) < 1) return 0;       /* the table, one record */
	unsigned char *st = r + 16;                       /* struct rt_ctl_status */
	unsigned long ago = (unsigned long)L(st, 40);
	s[0] = 0;
	switch (st[37]) {                                 /* ntp_state */
	case 1: pcat(s, "NTP: Not Synced Yet"); return 1;
	case 2: break;
	case 3: if (ago == 0xFFFFFFFFUL) { pcat(s, "NTP: The Time Server Did Not Answer"); return 1; } break;
	default: return 0;                                /* NTP off */
	}
	char num[12], *q = num + 11;
	unsigned long n = ago < 60 ? 0 : ago < 3600 ? ago / 60 : ago < 86400 ? ago / 3600 : ago / 86400;
	const char *unit = ago < 3600 ? " minute" : ago < 86400 ? " hour" : " day";
	*q = 0;
	do { *--q = (char)('0' + n % 10); n /= 10; } while (n);
	pcat(s, "Last NTP sync: ");
	if (ago < 60) pcat(s, "less than a minute");
	else { pcat(s, q); pcat(s, unit); if (q[0] != '1' || q[1]) pcat(s, "s"); }
	pcat(s, " ago");
	if (st[37] == 3) pcat(s, " (the last try failed)");
	return 1;
}

/* ---- the control panel ---- */
static FSSpec cp, cp_folder;
static PSN finder;
static int open_pending;
static unsigned long open_until;
static int find_cpanel(void)
{
	short v;
	long d;
	int best = 0;
	if (FindFolder(ON_SYSTEM_DISK, TYPE('c', 't', 'r', 'l'), 0, &v, &d)) return 0;
	for (short i = 1; i < 200; i++) {
		unsigned char nm[64];
		pb_clear();
		P(pb, 18) = nm; W(pb, 22) = v; W(pb, 28) = i; L(pb, 48) = d;
		if (pb_getcatinfo(pb)) break;
		if (*((unsigned char *)pb + 30) & 0x10) continue;       /* a folder */
		if (L(pb, 32) != TYPE('c', 'd', 'e', 'v')) continue;
		int k = L(pb, 36) == TYPE('t', 'i', 'm', 'e') ? 2 : L(pb, 36) == TYPE('m', 'i', 's', 'c') ? 1 : 0;   /* Date & Time, General Controls */
		if (k > best) { best = k; cp.vRefNum = v; cp.parID = d; memcpy(cp.name, nm, (unsigned long)nm[0] + 1); }
	}
	if (!best) return 0;
	pb_clear();                                       /* the Control Panels folder itself: its name and parent */
	P(pb, 18) = cp_folder.name; W(pb, 22) = v; W(pb, 28) = -1; L(pb, 48) = d;
	if (pb_getcatinfo(pb)) return 0;
	cp_folder.vRefNum = v; cp_folder.parID = L(pb, 100);   /* ioDrParID */
	return best;
}
static int find_finder(void)
{
	PSN p = { 0, 0 };
	struct { long length; unsigned char *name; PSN psn; unsigned long type, signature, mode; void *location;
	         unsigned long size, freemem; PSN launcher; unsigned long launch, active; void *spec; } info;
	while (!pm_next(&p)) {
		memset(&info, 0, sizeof info);
		info.length = sizeof info;
		if (!pm_info(&p, &info) && info.signature == TYPE('M', 'A', 'C', 'S') && info.type == TYPE('F', 'N', 'D', 'R')) { finder = p; return 1; }
	}
	return 0;
}
static short alias_desc(const FSSpec *s, AEDesc *out)
{
	void **a = 0;
	short e = new_alias(0, s, &a);
	if (e) return e;
	mac_hlock(a);
	e = ae_create_desc(TYPE('a', 'l', 'i', 's'), *a, *(unsigned short *)((char *)*a + 4), out);
	mac_dispose_handle(a);
	return e;
}
/* between events: the Finder asked, from its own GetNextEvent, to open the control panel */
void clock_idle(void)
{
	PSN cur;
	if (!open_pending) return;
	if (LM_TICKS > open_until) { open_pending = 0; mlog("Menu Extras: the Finder did not come to the front; Date & Time not opened"); return; }
	if (pm_current(&cur) || cur.hi != finder.hi || cur.lo != finder.lo) return;
	open_pending = 0;
	void *zone = LM_THEZONE;
	mac_setzone(LM_SYSZONE);
	AEDesc target = { 0, 0 }, ev = { 0, 0 }, folder = { 0, 0 }, item = { 0, 0 }, list = { 0, 0 }, rep = { 0, 0 };
	short e = ae_create_desc(TYPE('p', 's', 'n', ' '), &finder, 8, &target);
	if (!e) e = ae_create_event(TYPE('F', 'N', 'D', 'R'), TYPE('s', 'o', 'p', 'e'), &target, &ev);
	if (!e) e = alias_desc(&cp_folder, &folder);
	if (!e) e = alias_desc(&cp, &item);
	if (!e) e = ae_create_list(&list);
	if (!e) e = ae_put_desc(&list, 0, &item);
	if (!e) e = ae_put_param_desc(&ev, 0x2D2D2D2DUL /* keyDirectObject */, &folder);
	if (!e) e = ae_put_param_desc(&ev, TYPE('f', 's', 'e', 'l') /* keySelection */, &list);
	if (!e) e = ae_send(&ev, &rep, 1 /* kAENoReply */ | 0x20 /* kAECanInteract */);
	AEDesc *all[] = { &target, &ev, &folder, &item, &list, &rep };
	for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++)
		if (all[i]->data) ae_dispose_desc(all[i]);
	mac_setzone(zone);
	if (e) mlog("Menu Extras: the Finder could not be asked to open the control panel");
}

/* ---- the menu ---- */
#define CLK_MENU 23459
static void item(void *m, int *n, const char *t, int on, int enabled)
{
	unsigned char s[256];
	AppendMenu(m, (const unsigned char *)"\x01 ");    /* SetItem: no metacharacters ('/' in AM/PM) */
	(*n)++;
	s[0] = 0;
	SetItem(m, *n, pcat(s, t));
	if (on) SetItemMark(m, *n, 0x12);
	if (!enabled) DisableItem(m, *n);
}
int clock_menuselect(long pt)
{
	if (!on_extra(&clk_hit, pt)) return 0;
	unsigned char s[256];
	void *m = (void *)NewMenu(CLK_MENU, (const unsigned char *)"\x05" "Clock");
	if (!m) return 1;
	int n = 0;
	AppendMenu(m, (const unsigned char *)"\x01 "); n++;
	IUDateString((long)LM_TIME, 1 /* longDate */, s, 0);
	SetItem(m, n, s);
	DisableItem(m, n);
	if (ntp_line(s)) { AppendMenu(m, (const unsigned char *)"\x01 "); n++; SetItem(m, n, s); DisableItem(m, n); }
	get_intl();
	int analog = (cfg & F_ANALOG) != 0, h24 = is24();
	AppendMenu(m, (const unsigned char *)"\x02(-"); n++;
	int i_dig = n + 1;
	item(m, &n, "View as Digital", !analog, 1);
	item(m, &n, "View as Analog", analog, 1);
	AppendMenu(m, (const unsigned char *)"\x02(-"); n++;
	int i_date = n + 1;
	item(m, &n, "Show the Date", cfg & F_DATE, !analog);
	item(m, &n, "Show the Day of the Week", cfg & F_DAY, !analog);
	item(m, &n, "Show AM/PM", (cfg & F_AMPM) && !h24, !analog && !h24);
	item(m, &n, "Use a 24-Hour Clock", h24, !analog);
	item(m, &n, "Show Seconds", cfg & F_SECS, !analog);
	item(m, &n, "Flash the Time Separators", cfg & F_FLASH, !analog);
	AppendMenu(m, (const unsigned char *)"\x02(-"); n++;
	int kind = find_cpanel();
	int i_open = n + 1;
	item(m, &n, kind == 1 ? "General Controls\xC9" : "Date & Time\xC9", 0, kind != 0);

	InsertMenu(m, -1);
	InitCursor();
	short mbh = LM_MBARHEIGHT;
	Rect hl = clk_hit;
	hl.bottom = (short)(mbh - 1);
	{ void *save, *wm; GetPort(&save); GetWMgrPort(&wm); SetPort(wm); InvertRect(&hl); SetPort(save); }
	long r = PopUpMenuSelect(m, mbh + 2, clk_hit.left, 1);
	DeleteMenu(CLK_MENU);
	DisposeMenu(m);
	{ void *save, *wm; GetPort(&save); GetWMgrPort(&wm); SetPort(wm); EraseRect(&hl); SetPort(save); }
	int it = (short)(r & 0xFFFF);
	unsigned short was = cfg;
	if ((r >> 16) == CLK_MENU && it) {
		if (it == i_dig) cfg &= ~F_ANALOG;
		else if (it == i_dig + 1) cfg |= F_ANALOG;
		else if (it == i_date) cfg ^= F_DATE;
		else if (it == i_date + 1) cfg ^= F_DAY;
		else if (it == i_date + 2) cfg ^= F_AMPM;
		else if (it == i_date + 3) cfg = (unsigned short)((cfg & ~F_24) | F_SET24 | (h24 ? 0 : F_24));
		else if (it == i_date + 4) cfg ^= F_SECS;
		else if (it == i_date + 5) cfg ^= F_FLASH;
		else if (it == i_open && kind && find_finder()) {
			open_pending = 1;
			open_until = LM_TICKS + 10 * 60;
			pm_setfront(&finder);
		}
	}
	if (cfg != was) save_prefs();
	last_q = 0xFFFFFFFFUL;
	draw_extras(0);
	return 1;
}

/* ---- at start: an older copy of the extension is moved aside ----
   It was "Aurora68 Wi-Fi & Bluetooth" until 23 November 2026 ("Wi-Fi & Bluetooth" before 21 November, "WiFi
   Menu" at first), the same creator. Two copies would both patch _DrawMenuBar, and the old one would draw its
   icons at their fixed places over the clock. The Start Manager runs the extensions in the order of their
   names, so this one comes before any of those: every other INIT file of creator PSWM in Extensions or the
   System Folder goes into "Extensions (Disabled)", where System 7.5's Extensions Manager keeps what it turns
   off, before its turn comes. Moving files the Start Manager has not reached yet changes nothing for the ones
   it has. */
static const unsigned char disabled_name[] = "\x15" "Extensions (Disabled)";
static int move_aside(short v, long from, unsigned char *nm)
{
	short sv;
	long sd, dd;
	if (FindFolder(ON_SYSTEM_DISK, TYPE('m', 'a', 'c', 's'), 0, &sv, &sd) || sv != v) return 0;
	pb_clear();
	P(pb, 18) = (void *)disabled_name; W(pb, 22) = v; L(pb, 48) = sd;
	if (pb_dircreate(pb)) {                       /* there already: its directory ID */
		pb_clear();
		P(pb, 18) = (void *)disabled_name; W(pb, 22) = v; W(pb, 28) = 0; L(pb, 48) = sd;
		if (pb_getcatinfo(pb) || !(*((unsigned char *)pb + 30) & 0x10)) return 0;
	}
	dd = L(pb, 48);
	for (int tries = 0; tries < 2; tries++) {
		pb_clear();
		P(pb, 18) = nm; W(pb, 22) = v; P(pb, 28) = 0; L(pb, 36) = dd; L(pb, 48) = from;
		short e = pb_catmove(pb);
		if (!e) {
			char line[128], *p = line;
			add(&p, "Menu Extras: an older copy, \"");
			for (int i = 1; i <= nm[0] && i < 40; i++) *p++ = (char)nm[i];
			add(&p, "\", moved to Extensions (Disabled)");
			*p = 0;
			mlog(line);
			return 1;
		}
		if (e != -48) return 0;                      /* dupFNErr: an older copy put there before */
		pb_clear();
		P(pb, 18) = nm; W(pb, 22) = v; W(pb, 28) = 0; L(pb, 48) = dd;
		if (pb_getcatinfo(pb) || L(pb, 36) != TYPE('P', 'S', 'W', 'M')) return 0;
		pb_clear();
		P(pb, 18) = nm; W(pb, 22) = v; L(pb, 48) = dd;
		if (pb_hdelete(pb)) return 0;
	}
	return 0;
}
void retire_old_copies(void)
{
	unsigned char me[64];
	pb_clear();
	P(pb, 18) = me; W(pb, 24) = LM_CURMAP; W(pb, 28) = 0;      /* this INIT's file: CurMap at INIT time */
	if (pb_getfcbinfo(pb)) return;
	long my_num = L(pb, 32);
	unsigned long kinds[2] = { TYPE('e', 'x', 't', 'n'), TYPE('m', 'a', 'c', 's') };
	for (int k = 0; k < 2; k++) {
		short v;
		long d;
		if (FindFolder(ON_SYSTEM_DISK, (long)kinds[k], 0, &v, &d)) continue;
		for (short i = 1; i < 400; i++) {
			unsigned char nm[64];
			pb_clear();
			P(pb, 18) = nm; W(pb, 22) = v; W(pb, 28) = i; L(pb, 48) = d;
			if (pb_getcatinfo(pb)) break;
			if (*((unsigned char *)pb + 30) & 0x10) continue;
			if (L(pb, 32) != TYPE('I', 'N', 'I', 'T') || L(pb, 36) != TYPE('P', 'S', 'W', 'M') || L(pb, 48) == my_num) continue;
			if (move_aside(v, d, nm)) i--;          /* the folder's next file is now at this index */
		}
	}
}
