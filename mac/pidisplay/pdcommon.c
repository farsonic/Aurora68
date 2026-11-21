/* mac/pidisplay/pdcommon.c - the kernel's HDMI questions and the keep-or-revert dialog, for PiDisplay
 * (the application) and PiScrn (the extension, which asks at startup). MPL-2.0.
 *
 * The INFO service (psmb.h) is one command block in the card's window that the drivers also log
 * through at interrupt time, so a command is posted and waited for with interrupts held off, as
 * PiLog and the WiFi menu do it. The answers come from core 3 within microseconds; DISPINFO copies
 * about 2 KB into the Mac's buffer.
 *
 * The question after a new HDMI mode (docs/10, "PiDisplay"): the Pi has just restarted with the mode
 * PiDisplay wrote into aurora68-display.txt, and nobody knows yet whether the monitor shows it. So it
 * is asked on the main screen, which on the SE board is the SE's own, always visible: "Keep this
 * resolution?", Keep and Revert, and a countdown from 15 seconds after which the answer is Revert.
 * The 1992 Human Interface Guidelines' alert: centred left to right, a third of the way down, the
 * caution icon, the default button (Keep) outlined and answering Return and Enter, the other
 * (Revert) answering Escape and Command-period, each flashing when its key is used. Keep is the
 * default because the question is on a screen that works: whoever reads it can see the new mode
 * on the monitor beside it, and a mode that is not there reverts by itself.
 */
#include "pdcommon.h"

#define INFO_CMD 0x1000UL

volatile unsigned char *pd_window(void)
{
	volatile unsigned char *w = LM_MMU32BIT ? (volatile unsigned char *)0xF9000000 : (volatile unsigned char *)0x00900000;
	/* the card's header: 'PSMB'. Without the card the slot space answers a bus error on a real Mac, but
	   PiDisplay runs only where the card is; the Slot Manager's view would be the polite check */
	if (*(volatile unsigned long *)w != 0x50534D42UL) return 0;
	return w;
}

long pd_info(volatile unsigned char *w, int cmd, unsigned long a0, unsigned long a1, unsigned long a2,
             void *buf, unsigned long len, unsigned long *result)
{
	volatile unsigned char *c = w + INFO_CMD;
	volatile unsigned long *req = (volatile unsigned long *)c, *done = (volatile unsigned long *)(c + 4);
	unsigned long r, n;
	long st = -100;
	unsigned short sr = get_sr();
	set_sr(sr | 0x0700);
	r = *req;
	for (n = 0; *done != r && n < 2000000; n++) { }
	*(volatile unsigned short *)(c + 8) = (unsigned short)cmd;
	*(volatile unsigned long *)(c + 12) = a0;
	*(volatile unsigned long *)(c + 16) = a1;
	*(volatile unsigned long *)(c + 20) = a2;
	*(volatile unsigned long *)(c + 28) = (unsigned long)buf;
	*(volatile unsigned long *)(c + 32) = len;
	*req = r + 1;
	for (n = 0; *done != r + 1 && n < 4000000; n++) { }
	if (*done == r + 1) {
		st = *(volatile long *)(c + 36);
		if (result) *result = *(volatile unsigned long *)(c + 40);
	}
	set_sr(sr);
	return st;
}

int pd_fetch(volatile unsigned char *w, struct psmb_dispinfo *di)
{
	unsigned long n = 0;
	if (!w || pd_info(w, PSMB_INFO_DISPINFO, 0, 0, 0, di, sizeof *di, &n)) return 0;
	return n == sizeof *di && di->magic == PSMB_DISPINFO_MAGIC && di->version >= 1;
}

void pd_log(volatile unsigned char *w, const char *text)
{
	if (w) pd_info(w, PSMB_INFO_LOG, 0, 0, 0, (void *)text, strlen(text), 0);
}

char *pd_put(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
char *pd_num(char *p, unsigned long v)
{
	char b[12];
	int i = 0;
	do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (i) *p++ = b[--i];
	*p = 0;
	return p;
}
char *pd_hz(char *p, unsigned v)
{
	p = pd_num(p, v / 100);
	if (v % 100) {
		*p++ = '.';
		*p++ = (char)('0' + v % 100 / 10);
		if (v % 10) *p++ = (char)('0' + v % 10);
	}
	*p = 0;
	return p;
}
char *pd_mhz(char *p, unsigned long khz)
{
	p = pd_num(p, khz / 1000);
	if (khz % 1000) {
		*p++ = '.';
		*p++ = (char)('0' + khz % 1000 / 100);
		if (khz % 100 >= 10) *p++ = (char)('0' + khz % 100 / 10);
	}
	*p = 0;
	return p;
}

/* ---- dialogs built in memory (no 'DITL' resources: the programs stay plain code) ---- */
void pd_item(unsigned char **p, int t, int l, int b, int r, int type, const char *text)
{
	unsigned char *q = *p;
	short v[4] = { (short)t, (short)l, (short)b, (short)r };
	int n = (int)strlen(text), i;
	for (i = 0; i < 4; i++) *q++ = 0;
	for (i = 0; i < 4; i++) { *q++ = (unsigned char)(v[i] >> 8); *q++ = (unsigned char)v[i]; }
	*q++ = (unsigned char)type;
	*q++ = (unsigned char)n;
	for (i = 0; i < n; i++) *q++ = (unsigned char)text[i];
	if (n & 1) *q++ = 0;
	*p = q;
}
void pd_icon(unsigned char **p, int t, int l, int id)
{
	unsigned char *q = *p;
	short v[4] = { (short)t, (short)l, (short)(t + 32), (short)(l + 32) };
	int i;
	for (i = 0; i < 4; i++) *q++ = 0;
	for (i = 0; i < 4; i++) { *q++ = (unsigned char)(v[i] >> 8); *q++ = (unsigned char)v[i]; }
	*q++ = 32 | 0x80;                              /* iconItem, disabled */
	*q++ = 2;
	*q++ = (unsigned char)(id >> 8); *q++ = (unsigned char)id;
	*p = q;
}
void pd_centre(Rect *r, int w, int h)
{
	Rect m = { 0, 0, 342, 512 };
	if (LM_MAINDEVICE) m = *GD_RECT(LM_MAINDEVICE);
	m.top += 20;                                   /* the menu bar */
	r->left = (short)(m.left + (m.right - m.left - w) / 2); r->right = (short)(r->left + w);
	r->top = (short)(m.top + (m.bottom - m.top - h) / 3); r->bottom = (short)(r->top + h);
}

int pd_dlg_mode;
static unsigned long cd_end, cd_shown;          /* the countdown: its end in ticks, the second last shown */
static void *cd_text;                           /* the countdown's text item */
static char cd_buf[96];
void c_useritem(void *dlg, int item)
{
	short kind;
	void *h;
	Rect box;
	GetDItem(dlg, item, &kind, &h, &box);
	PenSize(3, 3);
	FrameRoundRect(&box, 16, 16);
	PenNormal();
}
static void flash(void *dlg, int item)
{
	short kind;
	void *h;
	Rect box;
	GetDItem(dlg, item, &kind, &h, &box);
	HiliteControl(h, 1);
	unsigned long t = LM_TICKS + 8;
	while (LM_TICKS < t) ;
	HiliteControl(h, 0);
}
static void countdown_text(unsigned long left)
{
	unsigned char ps[100];
	char *p = pd_put(cd_buf, "If you do nothing, the previous resolution comes back in ");
	p = pd_num(p, left);
	p = pd_put(p, left == 1 ? " second." : " seconds.");
	SetIText(cd_text, pstr(ps, cd_buf));
}
int c_dlgfilter(void *dlg, EventRecord *ev, short *item)
{
	if (pd_dlg_mode == 0) {                    /* the countdown */
		unsigned long now = LM_TICKS, left = cd_end > now ? (cd_end - now + 59) / 60 : 0;
		if (left != cd_shown) { cd_shown = left; countdown_text(left); }
		if (!left) { *item = 2; return 1; }
	}
	if (ev->what != keyDown && ev->what != autoKey) return 0;
	unsigned char c = (unsigned char)(ev->message & 0xFF);
	if (c == 0x0D || c == 0x03) { flash(dlg, 1); *item = 1; return 1; }
	if (pd_dlg_mode != 2 && (c == 0x1B || ((ev->modifiers & cmdKey) && c == '.'))) { flash(dlg, 2); *item = 2; return 1; }
	if (pd_dlg_mode == 2 && (c == 0x1B || ((ev->modifiers & cmdKey) && c == '.'))) { flash(dlg, 1); *item = 1; return 1; }
	return 0;
}

int pd_alert(const char *msg, const char *ok, const char *cancel, int icon)
{
	void **items = mac_newhandle(700);
	if (!items) return 2;
	unsigned char *q = (unsigned char *)*items;
	int lines = (int)(strlen(msg) / 44) + 1, h = 20 + lines * 16;
	if (h < 52) h = 52;
	int bw = 76 + 6 * (int)(strlen(ok) > 8 ? strlen(ok) - 8 : 0);
	*q++ = 0; *q++ = cancel ? 4 : 3;               /* items - 1 */
	pd_item(&q, h + 16, 340 - bw, h + 36, 340, 4, ok);
	if (cancel) pd_item(&q, h + 16, 340 - bw - 86, h + 36, 340 - bw - 10, 4, cancel);
	pd_item(&q, 10, 64, h + 6, 340, 8 | 0x80, msg);
	pd_item(&q, h + 12, 336 - bw, h + 40, 344, 0 | 0x80, "");   /* the default button's outline */
	pd_icon(&q, 10, 20, icon);
	Rect r;
	pd_centre(&r, 356, h + 50);
	unsigned char title[2] = { 0, 0 };
	void *dlg = (void *)NewDialog(0, &r, title, 1, 1, (void *)-1L, 0, 0, items);
	if (!dlg) return 2;
	short kind, hit = 0;
	void *hh;
	Rect box;
	int outline = cancel ? 4 : 3;
	GetDItem(dlg, outline, &kind, &hh, &box);
	SetDItem(dlg, outline, kind, (void *)pas_useritem, &box);
	pd_dlg_mode = cancel ? 1 : 2;
	InitCursor();
	do ModalDialog((void *)pas_dlgfilter, &hit);
	while (hit != 1 && hit != 2);
	DisposDialog(dlg);
	return hit;
}

int pd_confirm(volatile unsigned char *w, const struct psmb_dispinfo *di)
{
	char msg[200], *p;
	unsigned char ps[4];
	p = pd_put(msg, "Keep this resolution? The HDMI screen is now ");
	p = pd_num(p, di->cur_w); p = pd_put(p, " x "); p = pd_num(p, di->cur_h);
	if (di->confirm_refresh || di->cur_refresh) { p = pd_put(p, " at "); p = pd_hz(p, di->cur_refresh ? di->cur_refresh : di->confirm_refresh); p = pd_put(p, " Hz"); }
	p = pd_put(p, ", chosen in PiDisplay.");
	if (di->confirm_w && (di->confirm_w != di->cur_w || di->confirm_h != di->cur_h))
		p = pd_put(p, " (The Pi's firmware did not take the mode asked for: check config.txt's include line.)");
	void **items = mac_newhandle(700);
	if (!items) return 0;
	unsigned char *q = (unsigned char *)*items;
	*q++ = 0; *q++ = 5;                            /* six items */
	pd_item(&q, 112, 270, 132, 340, 4, "Keep");
	pd_item(&q, 112, 186, 132, 256, 4, "Revert");
	pd_item(&q, 10, 64, 68, 340, 8 | 0x80, msg);
	pd_item(&q, 108, 266, 136, 344, 0 | 0x80, "");   /* the default button's outline */
	pd_item(&q, 72, 64, 104, 340, 8 | 0x80, "If you do nothing, the previous resolution comes back in 15 seconds.");
	pd_icon(&q, 10, 20, 2);                         /* the caution icon */
	Rect r;
	pd_centre(&r, 356, 146);
	ps[0] = 0;
	void *dlg = (void *)NewDialog(0, &r, ps, 1, 1, (void *)-1L, 0, 0, items);
	if (!dlg) return 0;
	short kind, hit = 0;
	void *h;
	Rect box;
	GetDItem(dlg, 4, &kind, &h, &box);
	SetDItem(dlg, 4, kind, (void *)pas_useritem, &box);
	GetDItem(dlg, 5, &kind, &cd_text, &box);
	pd_info(w, PSMB_INFO_DISPCONFIRM, PSMB_DC_ASKING, 0, 0, 0, 0, 0);
	pd_log(w, "PiDisplay: Keep this resolution? asked, 15 seconds");
	cd_end = LM_TICKS + 15 * 60; cd_shown = 15;
	pd_dlg_mode = 0;
	SysBeep(10);
	InitCursor();
	do ModalDialog((void *)pas_dlgfilter, &hit);
	while (hit != 1 && hit != 2);
	DisposDialog(dlg);
	unsigned long res = 0;
	pd_info(w, PSMB_INFO_DISPCONFIRM, hit == 1 ? PSMB_DC_KEEP : PSMB_DC_REVERT, 0, 0, 0, 0, &res);
	pd_log(w, hit == 1 ? "PiDisplay: the new resolution kept" : cd_shown ? "PiDisplay: Revert chosen" : "PiDisplay: no answer in 15 seconds, reverting");
	return hit == 1;
}
