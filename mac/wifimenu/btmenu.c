/* mac/wifimenu/btmenu.c - the Bluetooth menu: the Pi's Bluetooth from the right of the menu bar.
 * MPL-2.0.
 *
 * The user asked for a Bluetooth menu doing for Bluetooth what the WiFi menu does for WiFi. It sits
 * left of the WiFi icon, in the same extension (wifimenu.c draws both and takes the presses):
 *   - the icon is the Bluetooth rune, solid while the Pi's Bluetooth is ready, faint while it is
 *     off or starting;
 *   - the menu shows the state, the paired devices (a tick on those connected; choosing one that
 *     is not connected connects it), the devices the last search found (choosing one pairs it),
 *     "Look for Devices", and "Forget Device" with the paired ones in a submenu (it asks first:
 *     forgetting means pairing again).
 * Pairing a keyboard needs a passkey typed on it: a dialog shows the number while the Pi's host
 * waits, and closes when the pairing is done (Cancel leaves it). The Bluetooth block of the network
 * board (emu68/src/alice/alice_net.h struct an_bt, the kernel's bt_pi.c) takes four commands: scan,
 * pair, connect, forget, the same as PiBlue (mac/piblue.s) uses; pairings are kept on the Pi's card
 * by the kernel (pairings.txt). There is no command to switch the radio off or to disconnect, so
 * the menu offers neither. Written 18 November 2026.
 */
#include "toolbox.h"
#include "menus.h"

#define BT            (BOARD + 0x3800)
#define B_STATE       0
#define B_SCANNING    4
#define B_PAIRING     8
#define B_PASSKEY     12
#define B_PASSKEY_SHOWN 16
#define B_NKEYS       24
#define B_NFOUND      28
#define B_CMD         32
#define B_CMD_SEQ     36
#define B_CMD_DONE    40
#define B_CMD_ADDR    48
#define B_KEYS        128                /* 6 x 72: addr 0, flags 28, name 32 */
#define KEY_SIZE      72
#define K_FLAGS       28
#define K_NAME        32
#define B_FOUND       560                /* 12 x 56: addr 0, cod 8, rssi 12, name 16 */
#define FOUND_SIZE    56
#define F_COD         8
#define F_NAME        16
enum { BT_OFF, BT_NOFW, BT_STARTING, BT_READY, BT_FAILED };
enum { BCMD_SCAN = 1, BCMD_PAIR = 2, BCMD_FORGET = 3, BCMD_CONNECT = 4 };
#define BTK_CONNECTED 1

/* ---- the icon: the rune, 9 x 12 ---- */
static const char *const rune[12] = {
	"....#....", "....##...", "....#.#..", ".#..#..#.", "..#.#.#..", "...###...",
	"...###...", "..#.#.#..", ".#..#..#.", "....#.#..", "....##...", "....#....",
};
static unsigned short bt_icons[2][12];             /* 0 faint (off), 1 solid (ready) */
static struct { void *base; short rowBytes; Rect bounds; } bbm;
static Rect bt_r, bt_hit;
int bt_shown = -1;

void bt_make_icons(void)
{
	for (int y = 0; y < 12; y++) {
		unsigned short row = 0, faint = 0;
		for (int x = 0; x < 9; x++)
			if (rune[y][x] == '#') { row |= (unsigned short)(0x8000 >> x); if ((x + y) & 1) faint |= (unsigned short)(0x8000 >> x); }
		bt_icons[1][y] = row;
		bt_icons[0][y] = faint;
	}
}
int bt_picture(void) { return rd32(BT + B_STATE) == BT_READY; }
void bt_draw(void *wm, short mbh)
{
	bt_r.right = icon_r.left - 14;
	bt_r.left = bt_r.right - 9;
	bt_r.top = (short)((mbh - 12) / 2);
	bt_r.bottom = bt_r.top + 12;
	bt_hit = bt_r;
	bt_hit.left -= 6; bt_hit.right += 6; bt_hit.top = 0; bt_hit.bottom = mbh;
	bt_shown = bt_picture();
	bbm.base = bt_icons[bt_shown];
	bbm.rowBytes = 2;
	bbm.bounds.top = 0; bbm.bounds.left = 0; bbm.bounds.bottom = 12; bbm.bounds.right = 9;
	CopyBits(&bbm, (char *)wm + 2, &bbm.bounds, &bt_r, 0, 0);
}

/* ---- the command box ---- */
static int bt_command(unsigned long cmd, const volatile unsigned char *addr)
{
	if (rd32(BT + B_CMD_DONE) != rd32(BT + B_CMD_SEQ)) return -1;
	for (int i = 0; i < 8; i++) win()[BT + B_CMD_ADDR + i] = addr ? addr[i] : 0;
	wr32(BT + B_CMD, cmd);
	wr32(BT + B_CMD_SEQ, rd32(BT + B_CMD_SEQ) + 1);
	return 0;
}
static void name_of(unsigned long rec, char *out)
{
	for (int i = 0; i < 39; i++) out[i] = (char)win()[rec + i];
	out[39] = 0;
	if (!out[0]) { char *p = out; add(&p, "(no name)"); *p = 0; }
}

/* ---- dialogs: a question (Forget), and the pairing ---- */
static int dialog(const char *text, const char *ok, const char *cancel, int mode)
{
	void **items = mac_newhandle(400);
	if (!items) return 0;
	unsigned char *q = (unsigned char *)*items;
	*q++ = 0; *q++ = 4;                            /* five items */
	put_item(&q, 82, 266, 102, 336, 4, ok);
	put_item(&q, 82, 186, 102, 256, 4, cancel);
	put_item(&q, 12, 60, 72, 336, 8 | 0x80, text);
	put_item(&q, 78, 262, 106, 340, 0 | 0x80, "");
	put_icon(&q, 12, 16, mode == 1 ? 2 : 1);        /* caution for a question, note for pairing */
	void *wm;
	GetWMgrPort(&wm);
	Rect *pr = PORT_RECT(wm);
	Rect r;
	r.left = (short)((pr->right - 348) / 2); r.right = r.left + 348;
	r.top = (short)((pr->bottom - 114) / 3); r.bottom = r.top + 114;
	unsigned char title[2] = { 0, 0 };
	void *dlg = (void *)NewDialog(0, &r, title, 1, 1, (void *)-1L, 0, 0, items);
	if (!dlg) return 0;
	short kind, hit = 0;
	void *h;
	Rect box;
	if (mode == 1) {                               /* the default button's outline */
		GetDItem(dlg, 4, &kind, &h, &box);
		SetDItem(dlg, 4, kind, (void *)pas_useritem, &box);
	} else {                                       /* pairing: only Cancel, no default */
		GetDItem(dlg, 1, &kind, &h, &box);
		HideControl(h);
	}
	dlg_mode = mode;
	InitCursor();
	ModalDialog((void *)pas_dlgfilter, &hit);
	dlg_mode = 0;
	DisposDialog(dlg);
	return hit;
}

/* the pairing dialog's filter: the passkey when the host shows one, closed when the pairing ends */
static int passkey_up;
int bt_pair_filter(void *dlg, EventRecord *ev, short *item)
{
	if (ev->what == keyDown || ev->what == autoKey) {
		unsigned char c = (unsigned char)(ev->message & 0xFF);
		if (c == 0x1B || ((ev->modifiers & cmdKey) && c == '.')) { flash(dlg, 2); *item = 2; return 1; }
		ev->what = nullEvent;
		return 0;
	}
	if (!rd32(BT + B_PAIRING) && rd32(BT + B_CMD_DONE) == rd32(BT + B_CMD_SEQ)) { *item = 3; return 1; }
	if (rd32(BT + B_PASSKEY_SHOWN) && !passkey_up) {
		unsigned long pk = rd32(BT + B_PASSKEY);
		unsigned char s[96];
		char num[8];
		for (int i = 5; i >= 0; i--) { num[i] = (char)('0' + pk % 10); pk /= 10; }
		num[6] = 0;
		s[0] = 0;
		pcat(s, "On the keyboard, type ");
		pcat(s, num);
		pcat(s, " and press Return.");
		short kind;
		void *h;
		Rect box;
		GetDItem(dlg, 3, &kind, &h, &box);
		SetIText(h, s);
		passkey_up = 1;
	}
	return 0;
}

/* ---- the menu ---- */
#define BT_MENU 23458
#define FORGET_MENU 201                    /* a submenu's ID must fit in the item's mark byte */
static unsigned long paired_rec[6], found_rec[12];

int bt_menuselect(long pt)
{
	short v = (short)(pt >> 16), hh = (short)pt;
	if (bt_shown < 0 || v < bt_hit.top || v >= bt_hit.bottom || hh < bt_hit.left || hh >= bt_hit.right)
		return 0;
	unsigned long st = rd32(BT + B_STATE);
	int ready = st == BT_READY, scanning = rd32(BT + B_SCANNING) != 0;
	unsigned char s[256];
	char nm[40];
	void *m = (void *)NewMenu(BT_MENU, (const unsigned char *)"\x09" "Bluetooth");
	void *fm = (void *)NewMenu(FORGET_MENU, (const unsigned char *)"\x06" "Forget");
	if (!m || !fm) return 1;
	int n = 0, i_first_paired = 0, i_first_found = 0, i_scan, i_forget, npaired = 0, nfound = 0;
	AppendMenu(m, (const unsigned char *)"\x01 "); n++;
	s[0] = 0;
	SetItem(m, n, pcat(s, ready ? "Bluetooth: On" : st == BT_STARTING || st == BT_NOFW ? "Bluetooth: Starting\xC9" :
	                      st == BT_FAILED ? "Bluetooth: Not working" : "Bluetooth: Off"));
	DisableItem(m, n);
	if (ready) {
		unsigned long nk = rd32(BT + B_NKEYS);
		if (nk > 6) nk = 6;
		AppendMenu(m, (const unsigned char *)"\x02(-"); n++;
		AppendMenu(m, (const unsigned char *)"\x01 "); n++;
		s[0] = 0;
		SetItem(m, n, pcat(s, nk ? "Devices" : "No paired devices"));
		DisableItem(m, n);
		for (unsigned long k = 0; k < nk; k++) {
			unsigned long rec = BT + B_KEYS + k * KEY_SIZE;
			name_of(rec + K_NAME, nm);
			AppendMenu(m, (const unsigned char *)"\x01 "); n++;
			if (!i_first_paired) i_first_paired = n;
			s[0] = 0;
			SetItem(m, n, pcat(s, nm));
			if (rd32(rec + K_FLAGS) & BTK_CONNECTED) SetItemMark(m, n, 0x12);
			AppendMenu(fm, (const unsigned char *)"\x01 ");
			s[0] = 0;
			SetItem(fm, (int)k + 1, pcat(s, nm));
			paired_rec[npaired++] = rec;
		}
		unsigned long nf = rd32(BT + B_NFOUND);
		if (nf > 12) nf = 12;
		if (nf) {
			AppendMenu(m, (const unsigned char *)"\x02(-"); n++;
			AppendMenu(m, (const unsigned char *)"\x01 "); n++;
			s[0] = 0;
			SetItem(m, n, pcat(s, "Nearby"));
			DisableItem(m, n);
			for (unsigned long k = 0; k < nf; k++) {
				unsigned long rec = BT + B_FOUND + k * FOUND_SIZE;
				name_of(rec + F_NAME, nm);
				AppendMenu(m, (const unsigned char *)"\x01 "); n++;
				if (!i_first_found) i_first_found = n;
				s[0] = 0;
				pcat(s, nm);
				unsigned long major = (rd32(rec + F_COD) >> 8) & 0x1F;   /* class of device: 5 peripheral */
				if (major != 5) { pcat(s, "  (not a keyboard or mouse)"); DisableItem(m, n); }
				SetItem(m, n, s);
				found_rec[nfound++] = rec;
			}
		}
	}
	AppendMenu(m, (const unsigned char *)"\x02(-"); n++;
	AppendMenu(m, (const unsigned char *)"\x01 "); n++; i_scan = n;
	s[0] = 0;
	SetItem(m, n, pcat(s, scanning ? "Looking for Devices\xC9" : "Look for Devices"));
	if (!ready || scanning) DisableItem(m, n);
	AppendMenu(m, (const unsigned char *)"\x01 "); n++; i_forget = n;
	s[0] = 0;
	SetItem(m, n, pcat(s, "Forget Device"));
	if (npaired) { SetItemCmd(m, n, 0x1B); SetItemMark(m, n, FORGET_MENU); }  /* the hierarchical submenu */
	else DisableItem(m, n);

	InsertMenu(m, -1);
	InsertMenu(fm, -1);
	InitCursor();
	InvertRect(&bt_r);
	long r = PopUpMenuSelect(m, LM_MBARHEIGHT + 2, bt_r.left - 4, 1);
	DeleteMenu(FORGET_MENU);
	DeleteMenu(BT_MENU);
	DisposeMenu(fm);
	DisposeMenu(m);
	bt_shown = -1;
	{ void *save, *wm; GetPort(&save); GetWMgrPort(&wm); SetPort(wm); bt_draw(wm, LM_MBARHEIGHT); SetPort(save); }
	int id = (int)(r >> 16), item = (short)(r & 0xFFFF);
	if (!item) return 1;
	if (id == FORGET_MENU && item <= npaired) {
		unsigned long rec = paired_rec[item - 1];
		char text[120], *p = text;
		name_of(rec + K_NAME, nm);
		add(&p, "Forget \xD2"); add(&p, nm); add(&p, "\xD3? It will have to be paired again before it can be used.");
		*p = 0;
		if (dialog(text, "Forget", "Cancel", 1) == 1 && bt_command(BCMD_FORGET, win() + rec)) SysBeep(10);
	} else if (id == BT_MENU) {
		if (item == i_scan) {
			if (bt_command(BCMD_SCAN, 0)) SysBeep(10);
		} else if (i_first_paired && item >= i_first_paired && item < i_first_paired + npaired) {
			unsigned long rec = paired_rec[item - i_first_paired];
			if (!(rd32(rec + K_FLAGS) & BTK_CONNECTED) && bt_command(BCMD_CONNECT, win() + rec)) SysBeep(10);
		} else if (i_first_found && item >= i_first_found && item < i_first_found + nfound) {
			unsigned long rec = found_rec[item - i_first_found];
			char text[120], *p = text;
			name_of(rec + F_NAME, nm);
			if (bt_command(BCMD_PAIR, win() + rec)) { SysBeep(10); return 1; }
			add(&p, "Pairing with \xD2"); add(&p, nm); add(&p, "\xD3\xC9");
			*p = 0;
			passkey_up = 0;
			dialog(text, "OK", "Cancel", 2);
		}
		(void)i_forget;
	}
	return 1;
}
