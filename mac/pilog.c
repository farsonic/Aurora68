/* mac/pilog.c - "PiLog": the Pi kernel's log on the Macintosh. MPL-2.0.
 *
 * The kernel copies every character of its log into a 64 KiB ring in the Aurora68 card's
 * window (emu68/src/psmac/psmb.h, struct psmb_log at window + 0x60000), from boot. PiLog shows
 * the ring in a window that can be moved, resized, zoomed and scrolled; new lines arrive while it
 * runs, in the background too. The text can be selected and copied (Edit menu or Command-C).
 *
 * It costs next to nothing while nothing happens: WaitNextEvent sleeps a quarter of a second
 * between looks at the ring's head, which is one memory read.
 *
 * The first row of controls chooses which parts of the log show: Kernel (Emu68's own lines and
 * any other tag), Mac (the logic board, the Mac side's messages, SCSI), WiFi, Bluetooth, Network
 * and Card (the Pi's SD card). A line without a tag goes with the line before it. Option-click a
 * box to see that part alone; All shows everything again.
 *
 * The second row:
 *   Filter:  shows only the lines that contain the text, in any case, together with the check
 *            boxes. Several words must all be there; a word starting "-" and a letter must not
 *            ("wifi -scan"). Typing anywhere in the window goes to the field. The view is redone
 *            a moment after the last key, so typing is not held up by the whole ring being read
 *            at every letter. Esc, or the small box at the field's right end, empties it.
 *   Clear:   empties the view (Edit > Clear Log, Command-K). The kernel's ring is not touched,
 *            other readers and the serial copy need it: PiLog remembers where the ring stood and
 *            shows only what came after. Edit > Show Earlier Lines takes the mark away again.
 *   Follow:  while it is checked (View > Follow New Lines, Command-T) the view stays at the bottom
 *            as lines arrive. Scrolling up unchecks it; scrolling back to the bottom, or End,
 *            checks it again, and so does a new filter, which starts the view at its newest line.
 *   then how many lines show, "12 of 840 lines" while a filter hides some.
 *
 * A change of filter reads the whole ring (or what follows the Clear mark) again, so earlier lines
 * of that part appear too. TextEdit holds at most 32 KB, so the window keeps the newest 24 KB or
 * so and drops whole lines from the top as new ones come in.
 *
 * The strings are in the code, in English, as in the project's other Mac programs (there are no
 * catalogs on the Mac side). The window fits the SE's 512 x 342 screen.
 *
 * Built with the C kit in mac/c (mac/c/README.md). Keys: arrows, Page Up and Down, Home, End
 * scroll the log; Left and Right, Delete and Tab work in the filter field.
 */
#include "toolbox.h"

/* ---- the ring (psmb.h) ---- */
#define LOG_OFF    0x60000
#define LOG_MAGIC  0x4136384Cu     /* 'A68L' */
struct psmb_log { unsigned long magic, version, size, data_off, head, pad[3]; };

#define KEEP       24000           /* the most text in the window */
#define LIMIT      30000           /* above this, lines go from the top */
#define POLL_TICKS 15
#define TYPE_TICKS 12              /* the filter is applied this long after the last key */

static volatile unsigned char *win_base;
static volatile struct psmb_log *lg;
static unsigned long pos;          /* the next byte of the log to show */
static int esc;                    /* inside an ANSI escape sequence */
static unsigned long clear_pos;    /* Clear's mark: the first byte to show, at a line start */
static int cleared, clear_cat;     /* the mark is set; the part of the line before it */
static unsigned long n_lines, n_shown;     /* lines read since the mark, and those shown */
static int save_all;               /* reading for File > Save Log with Option: every line */

/* ---- the parts ---- */
enum { C_KERNEL, C_MAC, C_WIFI, C_BT, C_NET, C_CARD, NCAT };
static const char *const cat_name[NCAT] = { "Kernel", "Mac", "WiFi", "Bluetooth", "Network", "Card" };
static int show[NCAT] = { 1, 1, 1, 1, 1, 1 };
static void *cat_box[NCAT], *all_btn, *clear_btn, *follow_box, *card_btn;
static int line_cat = C_KERNEL;    /* the category of the line being read (an untagged line keeps it) */
static unsigned char line[400];    /* the line being read */
static int line_n;

/* the two rows of controls: check boxes and All; the filter, Clear, Follow and the count */
#define ROW2       23              /* the second row's top */
#define STRIP      44              /* the height of both rows */
#define BASE2      (ROW2 + 12)     /* the second row's text baseline */

/* what a line is, from its tag: "[WIFI] ...", "[PSMAC:SOLO] ..." */
static int pfx(const unsigned char *l, int n, const char *p)
{
	int i;
	for (i = 0; p[i]; i++) if (i >= n || l[i] != (unsigned char)p[i]) return 0;
	return 1;
}
static int classify(const unsigned char *l, int n, int prev)
{
	if (!n || l[0] != '[') return prev;
	if (pfx(l, n, "[WIFI")) return C_WIFI;
	if (pfx(l, n, "[BT")) return C_BT;
	if (pfx(l, n, "[NET") || pfx(l, n, "[ETH")) return C_NET;
	if (pfx(l, n, "[CARD") || pfx(l, n, "[WBACK")) return C_CARD;
	if (pfx(l, n, "[PSMAC") || pfx(l, n, "[MAC") || pfx(l, n, "[SCSI")) return C_MAC;
	return C_KERNEL;
}

/* ---- secrets ---- */
/* The kernel echoes its options ("[PSMAC] options: ... (bootargs: ...)"), and those can hold the
   WiFi password. Whatever the kernel prints, the values of options that are secrets never reach
   the window, the Scrap or a saved file: wifi.psk and anything named psk, pass, key or secret
   (also as the end of a name after _ or -), the password in mac.wifisim=SSID:PASSWORD and the
   link key in bt.dev=ADDRESS:KEY:FLAGS become ***. The kernel does the same for aurora68.log. */
static unsigned char lower(unsigned char c);
static int name_end(const unsigned char *s, int n, const char *k)
{
	int m = (int)strlen(k), j = 0, b = n;
	while (b && s[b - 1] != '.') b--;              /* the last part of a dotted name */
	if (n - b < m) return 0;
	while (j < m && lower(s[n - m + j]) == (unsigned char)k[j]) j++;
	return j == m && (n - m == b || s[n - m - 1] == '_' || s[n - m - 1] == '-');
}
static int name_is(const unsigned char *s, int n, const char *k)
{
	int j = 0;
	while (j < n && k[j] && lower(s[j]) == (unsigned char)k[j]) j++;
	return j == n && !k[j];
}
static void redact(unsigned char *l, int *len)
{
	static const char *const names[] = { "psk", "pass", "passwd", "password", "passphrase", "key", "secret" };
	static unsigned char out[400];
	int n = *len, i, o = 0, ns = 0, k, hide, colon;
	i = 0;
	while (i < n && l[i] != '=') i++;
	if (i == n) return;                         /* most lines: nothing to look at */
	for (i = 0; i < n && o < (int)sizeof out; i++) {
		unsigned char c = l[i];
		int namec = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
		out[o++] = c;
		if (c != '=') { if (!namec) ns = o; continue; }
		hide = colon = 0;
		for (k = 0; k < (int)(sizeof names / sizeof names[0]); k++) if (name_end(out + ns, o - 1 - ns, names[k])) hide = 1;
		if (!hide && (name_is(out + ns, o - 1 - ns, "mac.wifisim") || name_is(out + ns, o - 1 - ns, "bt.dev"))) hide = colon = 1;
		if (!hide) { ns = o; continue; }
		int e = i + 1;
		if (colon) {                                /* keep the part before the first colon */
			while (e < n && l[e] != ':' && l[e] != ' ') { if (o < (int)sizeof out) out[o++] = l[e]; e++; }
			if (e < n && l[e] == ':' && o < (int)sizeof out) out[o++] = l[e++];
			while (e < n && l[e] != ':' && l[e] != ' ' && l[e] != ')' && l[e] != ',') e++;
		} else if (e < n && l[e] == '"') {
			e++;
			while (e < n && l[e] != '"') e++;
			if (e < n) e++;
		} else while (e < n && l[e] != ' ' && l[e] != ')' && l[e] != ',') e++;
		for (k = 0; k < 3 && o < (int)sizeof out; k++) out[o++] = '*';
		i = e - 1; ns = o;
	}
	memcpy(l, out, o);
	*len = o;
}

/* ---- the text filter ---- */
/* As many characters as the field shows: Monaco 9 is 6 pixels a character, the field's text
   area 113 pixels. The field does not scroll sideways, so it takes no more. */
#define FIELD_W    132
#define FMAX       18
#define NTERM      8
static unsigned char fl[FMAX];     /* the filter in force, in lower case */
static int fl_n;
static struct { unsigned char off, len, neg; } term[NTERM];
static int nterm;
static unsigned char low[sizeof line];     /* the line being matched, in lower case */

static unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* is the word w (k bytes) somewhere in l (n bytes)? */
static int has(const unsigned char *l, int n, const unsigned char *w, int k)
{
	int i, j;
	for (i = 0; i + k <= n; i++) {
		if (l[i] != w[0]) continue;
		j = 1;
		while (j < k && l[i + j] == w[j]) j++;
		if (j == k) return 1;
	}
	return 0;
}
static int matches(const unsigned char *l, int n)
{
	int i;
	if (!nterm) return 1;
	for (i = 0; i < n; i++) low[i] = lower(l[i]);
	for (i = 0; i < nterm; i++)
		if (has(low, n, fl + term[i].off, term[i].len) == term[i].neg) return 0;
	return 1;
}

/* the field's text as the filter in force: words between spaces, "-word" one that must not be
   there ("-43", a minus and a digit, is looked for as it is: error numbers are written so) */
static TEHandle fte;
static void take_filter(void)
{
	TERec *t = *fte;
	int n = t->teLength < FMAX ? t->teLength : FMAX, i = 0;
	for (i = 0; i < n; i++) fl[i] = lower((unsigned char)(*t->hText)[i]);
	fl_n = n;
	nterm = 0;
	i = 0;
	while (i < n && nterm < NTERM) {
		int neg = 0, start;
		while (i < n && fl[i] == ' ') i++;
		if (i + 1 < n && fl[i] == '-' && fl[i + 1] >= 'a' && fl[i + 1] <= 'z') { neg = 1; i++; }
		start = i;
		while (i < n && fl[i] != ' ') i++;
		if (i > start) {
			term[nterm].off = start; term[nterm].len = i - start; term[nterm].neg = neg;
			nterm++;
		}
	}
}
/* the field differs from the filter in force */
static int field_changed(void)
{
	TERec *t = *fte;
	int i;
	if (t->teLength != fl_n) return 1;
	for (i = 0; i < fl_n; i++) if (lower((unsigned char)(*t->hText)[i]) != fl[i]) return 1;
	return 0;
}

static void *win, *vbar;
static void refilter(void);
static TEHandle te;
static int top;                    /* the first line in view */
static int follow = 1;             /* the view stays at the bottom as lines arrive */
static int focus;                  /* where keys and the Edit menu go: 0 the log, 1 the field */
static int active = 1;             /* the window is active (and PiLog in front) */
static int fg = 1;                 /* PiLog is the front program */
static int quit;
static int refilter_due;           /* the field changed: apply it at refilter_at */
static unsigned long refilter_at;
static void *m_apple, *m_file, *m_edit, *m_view;
static unsigned char pbuf[256];
static Rect field_r, x_r;          /* the field's frame, its empty-it box */
static int label_h, count_h;       /* where "Filter:" and the count are drawn */
static int card_w;                 /* the Save to Card button's width (it sits at the right end) */
static const char *note;           /* said in place of the count until note_until */
static unsigned long note_until;
static int card_asked;             /* Save to Card asked: the kernel's progress is looked at */
static int card_wait;              /* looks left before an unanswered question counts as a failure */

enum { M_APPLE = 128, M_FILE, M_EDIT, M_VIEW };
enum { F_SAVE = 1, F_CARD, F_QUIT = 4 };
enum { E_UNDO = 1, E_CUT = 3, E_COPY, E_PASTE, E_CLEAR, E_SELALL = 8, E_FILTER,
       E_CLEARLOG = 11, E_EARLIER };
enum { V_FOLLOW = 1, V_NOFILTER = 3, V_ALLPARTS };

static TEHandle cur(void) { return focus ? fte : te; }

/* ---- layout and scrolling ---- */
static int line_h(void) { return (*te)->lineHeight > 0 ? (*te)->lineHeight : 11; }
static int visible(void)
{
	Rect *v = &(*te)->viewRect;
	int n = (v->bottom - v->top) / line_h();
	return n > 0 ? n : 1;
}
static int max_top(void)
{
	TERec *t = *te;
	int n = t->nLines;
	/* a final carriage return starts a line TextEdit does not count yet */
	if (t->teLength && (*t->hText)[t->teLength - 1] == '\r') n++;
	n -= visible();
	return n > 0 ? n : 0;
}
static void scroll_to(int n)
{
	int m = max_top();
	if (n > m) n = m;
	if (n < 0) n = 0;
	if (n != top) {
		TEScroll(0, (top - n) * line_h(), te);
		top = n;
	}
	SetCtlMax(vbar, m);
	SetCtlValue(vbar, top);
}

static void set_follow(int on)
{
	follow = on;
	SetCtlValue(follow_box, on);
	if (on) scroll_to(32767);
}
/* the user scrolled: away from the bottom stops following, back at the bottom follows again */
static void user_scroll(int n)
{
	scroll_to(n);
	if ((top >= max_top()) != follow) {
		follow = !follow;
		SetCtlValue(follow_box, follow);
	}
}

/* the scroll bar along the right, the text in the rest */
static void layout(void)
{
	Rect r = *PORT_RECT(win), b, v;
	b.top = r.top + STRIP - 1; b.left = r.right - 15; b.bottom = r.bottom - 14; b.right = r.right + 1;
	HideControl(vbar);
	MoveControl(vbar, b.left, b.top);
	SizeControl(vbar, b.right - b.left, b.bottom - b.top);
	v.top = r.top + STRIP + 2; v.left = r.left + 4; v.bottom = r.bottom - 15; v.right = r.right - 17;
	v.bottom = v.top + (v.bottom - v.top) / line_h() * line_h();
	TERec *t = *te;
	t->viewRect = v;
	t->destRect.left = v.left;
	t->destRect.right = v.right;
	t->destRect.top = v.top - top * line_h();
	t->destRect.bottom = t->destRect.top + 30000;
	TECalText(te);
	ShowControl(vbar);
	MoveControl(card_btn, r.right - 6 - card_w, ROW2 - 1);
}

/* ---- the second row's drawing ---- */
static void draw_x(void)
{
	/* the empty-it box: a small box with a cross, like a window's close box, while there is text */
	EraseRect(&x_r);
	if (!(*fte)->teLength) return;
	FrameRect(&x_r);
	MoveTo(x_r.left + 2, x_r.top + 2); LineTo(x_r.right - 3, x_r.bottom - 3);
	MoveTo(x_r.right - 3, x_r.top + 2); LineTo(x_r.left + 2, x_r.bottom - 3);
}

static char *put_num(char *p, unsigned long v)
{
	char b[12];
	int i = 0;
	do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (i) *p++ = b[--i];
	return p;
}
static int filtering(void)
{
	int i;
	if (nterm) return 1;
	for (i = 0; i < NCAT; i++) if (!show[i]) return 1;
	return 0;
}
static void draw_count(void)
{
	char s[48], *p = s;
	Rect r = { ROW2, count_h, ROW2 + 16, PORT_RECT(win)->right - 10 - card_w };
	EraseRect(&r);
	if (!lg) return;
	TextFont(3); TextSize(9);           /* Geneva 9 */
	MoveTo(count_h, BASE2);
	ClipRect(&r);
	if (note && (long)(note_until - LM_TICKS) > 0) {
		DrawString(pstr(pbuf, note));
		ClipRect(PORT_RECT(win));
		TextFont(4); TextSize(9);
		return;
	}
	if (filtering()) { p = put_num(p, n_shown); memcpy(p, " of ", 4); p += 4; }
	p = put_num(p, n_lines);
	memcpy(p, n_lines == 1 ? " line" : " lines", 6); p += n_lines == 1 ? 5 : 6;
	*p = 0;
	DrawString(pstr(pbuf, s));
	ClipRect(PORT_RECT(win));
	TextFont(4); TextSize(9);
}
static void say(const char *s, int secs)
{
	note = s;
	note_until = LM_TICKS + 60UL * secs;
	draw_count();
}

/* the rows of controls: the check boxes and All, then the filter, Clear and Follow, in the
   system font */
static void make_controls(void)
{
	int h = 6, i;
	Rect b;
	TextFont(0); TextSize(12);
	for (i = 0; i < NCAT; i++) {
		pstr(pbuf, cat_name[i]);
		b.top = 3; b.bottom = 19; b.left = h; b.right = h + 20 + StringWidth(pbuf);
		cat_box[i] = NewControl(win, &b, pbuf, 1, 1, 0, 1, checkBoxProc, i);
		h = b.right + 6;
	}
	Rect a = { 2, h + 4, 20, h + 4 + 36 };
	all_btn = NewControl(win, &a, pstr(pbuf, "All"), 1, 0, 0, 1, pushButProc, 0);

	label_h = 6;
	h = label_h + StringWidth(pstr(pbuf, "Filter:")) + 5;
	field_r.top = ROW2; field_r.bottom = ROW2 + 16; field_r.left = h; field_r.right = h + FIELD_W;
	x_r.top = field_r.top + 3; x_r.bottom = field_r.bottom - 3;
	x_r.right = field_r.right - 3; x_r.left = x_r.right - 10;
	h = field_r.right + 8;
	b.top = ROW2 - 1; b.bottom = ROW2 + 17; b.left = h; b.right = h + 16 + StringWidth(pstr(pbuf, "Clear"));
	clear_btn = NewControl(win, &b, pbuf, 1, 0, 0, 1, pushButProc, 0);
	h = b.right + 8;
	b.top = ROW2; b.bottom = ROW2 + 16; b.left = h; b.right = h + 20 + StringWidth(pstr(pbuf, "Follow"));
	follow_box = NewControl(win, &b, pbuf, 1, follow, 0, 1, checkBoxProc, 0);
	count_h = b.right + 8;
	card_w = 16 + StringWidth(pstr(pbuf, "Save to Card"));
	b.top = ROW2 - 1; b.bottom = ROW2 + 17; b.left = 0; b.right = card_w;    /* placed by layout() */
	card_btn = NewControl(win, &b, pbuf, 1, 0, 0, 1, pushButProc, 0);

	/* the field: one line of Monaco 9, wide enough never to wrap */
	TextFont(4); TextSize(9);
	Rect v = { field_r.top + 3, field_r.left + 3, field_r.bottom - 2, x_r.left - 2 };
	Rect d = v;
	d.right = d.left + 2000;
	fte = (TEHandle)TENew(&d, &v);
	TEDeactivate(fte);
}

static void box_click(void *ctl, int option)
{
	int i, k = -1;
	for (i = 0; i < NCAT; i++) if (cat_box[i] == ctl) k = i;
	if (ctl == all_btn) for (i = 0; i < NCAT; i++) show[i] = 1;
	else if (k < 0) return;
	else if (option) for (i = 0; i < NCAT; i++) show[i] = i == k;
	else show[k] = !show[k];
	for (i = 0; i < NCAT; i++) SetCtlValue(cat_box[i], show[i]);
	refilter();
}

static void resized(void)
{
	SetPort(win);
	InvalRect(PORT_RECT(win));
	layout();
	scroll_to(follow ? 32767 : top);
}

/* ---- new text from the ring ---- */
static void add_text(const unsigned char *s, int n)
{
	TERec *t = *te;
	int ss = t->selStart, se = t->selEnd;
	if (t->teLength + n > LIMIT) {
		/* drop whole lines from the top: up to the first line end past the cut */
		char *x = *t->hText;
		int cut = t->teLength + n - KEEP, lines = 0, i;
		if (cut > t->teLength) cut = t->teLength;
		for (i = 0; i < cut; i++) if (x[i] == '\r') lines++;
		while (cut < t->teLength && x[cut - 1] != '\r') cut++;
		if (cut && x[cut - 1] == '\r' && cut > i) lines++;
		TESetSelect(0, cut, te);
		TEDelete(te);
		ss -= cut; se -= cut;
		if (ss < 0) ss = 0;
		if (se < 0) se = 0;
		/* the same lines stay in view while the view is not following */
		if (!follow) {
			int k = top < lines ? top : lines;
			TEScroll(0, k * line_h(), te);
			top -= k;
		}
	}
	TESetSelect(32767, 32767, te);
	TEInsert(s, n, te);
	TESetSelect(ss, se, te);
	scroll_to(follow ? 32767 : top);
}

/* The log's bytes from pos to head, line by line through the filter, into out (len bytes at most;
   a full out is handed to flush, which returns what it left there). Returns the bytes in out. */
static int read_log(unsigned long head, unsigned char *out, int len, int n, int (*flush)(unsigned char *, int))
{
	unsigned long size = lg->size;
	while (pos != head) {
		unsigned char c = win_base[lg->data_off + pos % size];
		pos++;
		if (esc) { if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) esc = 0; continue; }
		if (c == 27) { esc = 1; continue; }
		if (c == '\t') c = ' ';
		if (c != '\n') {
			if (c < 32 || c == 127) continue;
			if (line_n < (int)sizeof line) line[line_n++] = c;
			continue;
		}
		redact(line, &line_n);
		line_cat = classify(line, line_n, line_cat);
		n_lines++;
		if (save_all || (show[line_cat] && matches(line, line_n))) {
			n_shown++;
			if (n + line_n + 1 > len) n = flush(out, n);
			memcpy(out + n, line, line_n);
			n += line_n;
			out[n++] = '\r';
		}
		line_n = 0;
	}
	return n;
}

static int flush_te(unsigned char *s, int n) { if (n) add_text(s, n); return 0; }

static void poll_log(void)
{
	static unsigned char buf[2048];
	if (!lg) return;
	unsigned long head = lg->head, size = lg->size, was = n_lines;
	if (head == pos) return;
	if (head - pos > size) { pos = head - size; line_n = 0; }
	flush_te(buf, read_log(head, buf, sizeof buf, 0, flush_te));
	if (n_lines != was) draw_count();
}

/* The filter changed: the whole ring again (from Clear's mark, if there is one), through the new
   filter, into the window. The text is gathered in a buffer of its own, which keeps the newest
   KEEP bytes, and goes in at once. */
static unsigned char *refill_buf;
/* where a reading of the whole ring starts: Clear's mark while it is in the ring (and asked for),
   else the oldest whole line. Returns the head to read to. */
static unsigned long ring_start(int from_mark)
{
	unsigned long head = lg->head, size = lg->size;
	pos = head > size ? head - size : 0;
	line_cat = C_KERNEL;
	if (from_mark && cleared && clear_pos - pos <= head - pos) {
		pos = clear_pos;
		line_cat = clear_cat;
	} else if (head > size) {
		/* the ring's oldest line is probably cut: start at the first line end */
		while (pos != head && win_base[lg->data_off + pos % size] != '\n') pos++;
		if (pos != head) pos++;
	}
	line_n = 0; esc = 0;
	return head;
}
static int flush_refill(unsigned char *s, int n)
{
	/* the buffer is full: keep the newest half, from a line start */
	int cut = n - KEEP / 2;
	while (cut < n && s[cut - 1] != '\r') cut++;
	memmove(s, s + cut, n - cut);
	return n - cut;
}
static void refilter(void)
{
	refilter_due = 0;
	take_filter();
	if (!lg) return;
	if (!refill_buf) refill_buf = mac_newptr(KEEP + 4096);
	if (!refill_buf) { SysBeep(5); return; }
	unsigned long head = ring_start(1);
	n_lines = n_shown = 0;
	int n = read_log(head, refill_buf, KEEP + 4096, 0, flush_refill);
	if (n > KEEP) n = flush_refill(refill_buf, n);
	SetPort(win);
	TESetText(refill_buf, n, te);
	top = 0;
	layout();
	set_follow(1);                      /* the new view starts at its newest line */
	InvalRect(PORT_RECT(win));
}

/* Edit > Clear Log: the view from here on. The mark goes at the start of the line being written,
   so that line shows whole when it ends. */
static void clear_log(void)
{
	if (!lg) return;
	poll_log();                         /* line_cat is now the part of the last whole line */
	unsigned long head = lg->head, size = lg->size, low_end = head > size ? head - size : 0;
	unsigned long p = head;
	while (p > low_end && win_base[lg->data_off + (p - 1) % size] != '\n') p--;
	clear_pos = p;
	clear_cat = line_cat;
	cleared = 1;
	refilter();
}
static void show_earlier(void)
{
	cleared = 0;
	refilter();
}

/* ---- the filter field ---- */
static void set_focus(int f)
{
	if (f == focus) return;
	if (active) TEDeactivate(cur());
	focus = f;
	if (active) TEActivate(cur());
}

/* the field was edited: the view follows a moment later, or at once */
static void field_edited(int now)
{
	draw_x();
	if (now) { if (field_changed() || refilter_due) refilter(); }
	else if (field_changed()) { refilter_due = 1; refilter_at = LM_TICKS + TYPE_TICKS; }
	else refilter_due = 0;
}

static void empty_field(void)
{
	TESetSelect(0, 32767, fte);
	TEDelete(fte);
	field_edited(1);
}

static void field_key(int c)
{
	TERec *t = *fte;
	set_focus(1);
	if (c >= 32 && c != 127 && t->teLength - (t->selEnd - t->selStart) >= FMAX) { SysBeep(5); return; }
	if (c == 127) {                     /* forward delete, which TextEdit does not know */
		if (t->selStart == t->selEnd) {
			if (t->selEnd >= t->teLength) return;
			TESetSelect(t->selStart, t->selStart + 1, fte);
		}
		TEDelete(fte);
	} else TEKey(c, fte);
	field_edited(0);
}

/* the Scrap's text into the field: its first line, as much as fits */
static void paste_field(void)
{
	char buf[FMAX];
	long off = 0, len = GetScrap((void *)LM_TESCRPH, TYPE('T', 'E', 'X', 'T'), &off);
	TERec *t = *fte;
	int room = FMAX - (t->teLength - (t->selEnd - t->selStart)), n = 0;
	if (len <= 0) return;
	const char *s = *LM_TESCRPH;
	while (n < len && n < room && s[n] != '\r' && s[n] != '\n') { buf[n] = s[n] == '\t' ? ' ' : s[n]; n++; }
	TEDelete(fte);
	TEInsert(buf, n, fte);
	field_edited(0);
}

/* ---- saving the log ---- */
/* For handing the log on as text instead of a photograph of the window. File > Save Log puts the
   lines the window shows (the parts, the filter, from Clear's mark) into a TEXT file of
   TeachText's, named "PiLog 2026-11-19 14.05"; with Option held, the whole ring. TeachText opens
   files up to 32 KB; a whole ring is 64 KB, which a modern machine reads anyway (copy it there
   through Lagoon's share). Save to Card asks the kernel to write the ring to aurora68.log in the
   SD card's root (psmac_card.c), to be read on another computer. Secrets are hidden either way. */
static int option_held(void) { return (LM_KEYMAP[7] & 0x04) != 0; }     /* key code 0x3A */

static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put2(char *p, int v) { *p++ = (char)('0' + v / 10); *p++ = (char)('0' + v % 10); return p; }
/* the clock as "2026-11-19" and the time with sep between hours and minutes */
static char *put_date(char *p, unsigned long t, char sep)
{
	static const unsigned char mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	unsigned long days = t / 86400, rest = t % 86400;
	int y = 1904, m = 0;
	while (days >= (unsigned long)(y % 4 ? 365 : 366)) { days -= y % 4 ? 365 : 366; y++; }    /* every fourth year to 2099 */
	while (days >= (unsigned long)(mdays[m] + (m == 1 && y % 4 == 0))) { days -= mdays[m] + (m == 1 && y % 4 == 0); m++; }
	p = put_num(p, (unsigned long)y); *p++ = '-'; p = put2(p, m + 1); *p++ = '-'; p = put2(p, (int)days + 1);
	*p++ = ' '; p = put2(p, (int)(rest / 3600)); *p++ = sep; p = put2(p, (int)(rest / 60 % 60));
	return p;
}

static ParamBlock spb;             /* the open file */
static short save_err;
static long save_at;
static int flush_file(unsigned char *s, int n)
{
	if (n && !save_err) {
		spb.u.io.ioBuffer = s; spb.u.io.ioReqCount = n;
		spb.u.io.ioPosMode = fsFromStart; spb.u.io.ioPosOffset = save_at;
		save_err = mac_PBWrite(&spb);
		save_at += n;
	}
	return 0;
}
static void save_log(int all)
{
	static unsigned char name[64], buf[2048];
	static unsigned char keep_line[sizeof line];
	SFReply rep;
	char t[300], *p;
	int i;
	if (!lg) return;
	p = put_date(put_str(t, "PiLog "), LM_TIME, '.');
	*p = 0;
	pstr(name, t);
	Point where = { 60, 104 };          /* the dialog's top left: centred on the SE's 512 x 342 */
	SFPutFile(pt_long(where), pstr(pbuf, all ? "Save all of the log as:" : "Save the lines shown as:"), name, 0, &rep, 1);
	if (!rep.good) return;
	SetPort(win);

	memset(&spb, 0, sizeof spb);
	spb.ioNamePtr = rep.fName; spb.ioVRefNum = rep.vRefNum;
	save_err = mac_PBCreate(&spb);
	if (save_err == dupFNErr) save_err = 0;          /* Standard File has asked about replacing it */
	if (!save_err) {
		memset(&spb, 0, sizeof spb);
		spb.ioNamePtr = rep.fName; spb.ioVRefNum = rep.vRefNum;
		save_err = mac_PBGetFInfo(&spb);
		spb.u.f.fdType = TYPE('T', 'E', 'X', 'T'); spb.u.f.fdCreator = TYPE('t', 't', 'x', 't');
		if (!save_err) save_err = mac_PBSetFInfo(&spb);
	}
	if (!save_err) {
		memset(&spb, 0, sizeof spb);
		spb.ioNamePtr = rep.fName; spb.ioVRefNum = rep.vRefNum; spb.ioPermssn = fsWrPerm;
		save_err = mac_PBOpen(&spb);
	}
	if (save_err) { SysBeep(5); say("Not saved", 5); return; }

	/* a first line saying what this is */
	save_at = 0;
	p = put_date(put_str(t, "PiLog: the Aurora68 kernel log, saved "), LM_TIME, ':');
	if (all) p = put_str(p, ". The whole ring.");
	else {
		if (cleared) p = put_str(p, ". From Clear on");
		p = put_str(p, ". Parts:");
		for (i = 0; i < NCAT; i++) if (show[i]) { *p++ = ' '; p = put_str(p, cat_name[i]); }
		if ((*fte)->teLength) {
			p = put_str(p, ". Filter: \"");
			memcpy(p, *(*fte)->hText, (*fte)->teLength); p += (*fte)->teLength;
			*p++ = '"';
		}
		*p++ = '.';
	}
	*p++ = '\r';
	flush_file((unsigned char *)t, (int)(p - t));

	/* the ring through the filter, as refilter() reads it, keeping the window's reading state */
	unsigned long k_pos = pos, k_lines = n_lines, k_shown = n_shown;
	int k_esc = esc, k_n = line_n, k_cat = line_cat;
	memcpy(keep_line, line, line_n);
	save_all = all;
	unsigned long head = ring_start(!all);
	flush_file(buf, read_log(head, buf, sizeof buf, 0, flush_file));
	save_all = 0;
	pos = k_pos; n_lines = k_lines; n_shown = k_shown; esc = k_esc; line_n = k_n; line_cat = k_cat;
	memcpy(line, keep_line, line_n);

	short werr = save_err;
	spb.u.io.ioMisc = save_at;
	mac_PBSetEOF(&spb);
	mac_PBClose(&spb);
	memset(&spb, 0, sizeof spb);
	spb.ioVRefNum = rep.vRefNum;
	mac_PBFlushVol(&spb);
	if (werr) { SysBeep(5); say("Not saved", 5); }
	else say("Saved", 4);
}

/* the mailbox's INFO service (psmb.h): one command, waited for; the drivers log through the same
   block at interrupt time, so interrupts are held off meanwhile, as the WiFi menu does */
#define INFO_CMD      0x1000
#define INFO_SAVELOG  6
#define INFO_LOGSTATE 7
static long info_call(int cmd, unsigned long *result)
{
	volatile unsigned char *c = win_base + INFO_CMD;
	volatile unsigned long *req = (volatile unsigned long *)c, *done = (volatile unsigned long *)(c + 4);
	unsigned long r, n;
	long st = -1;
	short sr;
	__asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "cc");
	r = *req;
	for (n = 0; *done != r && n < 2000000; n++) { }
	*(volatile unsigned short *)(c + 8) = (unsigned short)cmd;
	*(volatile unsigned long *)(c + 28) = 0;
	*(volatile unsigned long *)(c + 32) = 0;
	*req = r + 1;
	for (n = 0; *done != r + 1 && n < 4000000; n++) { }
	if (*done == r + 1) {
		st = *(volatile long *)(c + 36);
		if (result) *result = *(volatile unsigned long *)(c + 40);
	}
	__asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "cc");
	return st;
}
static void save_to_card(void)
{
	if (!lg || card_asked) return;
	long st = info_call(INFO_SAVELOG, 0);
	if (st == 0) { card_asked = 1; card_wait = 240; say("Saving\xC9", 60); }
	else { SysBeep(5); say(st == -6 ? "No SD card" : "Not saved", 5); }
}
static void card_progress(void)
{
	unsigned long state = 1;
	long st = info_call(INFO_LOGSTATE, &state);
	/* no answer: the core that writes the card also serves the mailbox, and may be in a card write */
	if (st == -1) state = --card_wait > 0 ? 1 : 3;
	else if (st) state = 3;
	if (state == 1) return;
	card_asked = 0;
	if (state == 2) say("Saved to card", 6);
	else { SysBeep(5); say("Not saved", 6); }
}

/* ---- menus ---- */
static void setup_menus(void)
{
	unsigned char t[2] = { 1, 0x14 };       /* the apple */
	m_apple = NewMenu(M_APPLE, t);
	AppendMenu(m_apple, pstr(pbuf, "About PiLog\xC9;(-"));
	AddResMenu(m_apple, TYPE('D', 'R', 'V', 'R'));
	InsertMenu(m_apple, 0);
	m_file = NewMenu(M_FILE, pstr(pbuf, "File"));
	AppendMenu(m_file, pstr(pbuf, "Save Log\xC9/S;Save Log to SD Card;(-;Quit/Q"));
	InsertMenu(m_file, 0);
	/* Undo to Clear in the standard order, so SystemEdit gives desk accessories the right ones */
	m_edit = NewMenu(M_EDIT, pstr(pbuf, "Edit"));
	AppendMenu(m_edit, pstr(pbuf, "Undo/Z;(-;Cut/X;Copy/C;Paste/V;Clear;(-;Select All/A;Filter Lines/F;(-;"
	                              "Clear Log/K;Show Earlier Lines"));
	InsertMenu(m_edit, 0);
	m_view = NewMenu(M_VIEW, pstr(pbuf, "View"));
	AppendMenu(m_view, pstr(pbuf, "Follow New Lines/T;(-;Clear Filter;Show All Parts"));
	InsertMenu(m_view, 0);
	DrawMenuBar();
}

static void enable(void *m, int item, int on)
{
	if (on) EnableItem(m, item);
	else DisableItem(m, item);
}
/* dim what cannot be done now, check Follow */
static void fix_menus(void)
{
	int da = FrontWindow() != win, i, some_off = 0;
	TERec *t = *cur();
	int sel = t->selEnd > t->selStart;
	enable(m_file, F_SAVE, lg != 0);
	enable(m_file, F_CARD, lg != 0 && !card_asked);
	enable(m_edit, E_UNDO, da);
	enable(m_edit, E_CUT, da || (focus && sel));
	enable(m_edit, E_COPY, da || sel);
	enable(m_edit, E_PASTE, da || focus);
	enable(m_edit, E_CLEAR, da || (focus && sel));
	enable(m_edit, E_SELALL, !da);
	enable(m_edit, E_FILTER, !da);
	enable(m_edit, E_CLEARLOG, !da && lg);
	enable(m_edit, E_EARLIER, !da && cleared);
	CheckItem(m_view, V_FOLLOW, follow);
	enable(m_view, V_NOFILTER, (*fte)->teLength > 0);
	for (i = 0; i < NCAT; i++) if (!show[i]) some_off = 1;
	enable(m_view, V_ALLPARTS, some_off);
}

static void about(void)
{
	/* a line in the window itself (an alert would need 'ALRT' and 'DITL' resources) */
	static const char msg[] = "PiLog: the Pi kernel's log (Aurora68). Filter by part and by text; "
	                          "Clear empties the view, Follow keeps it at the newest line.\r";
	add_text((const unsigned char *)msg, sizeof msg - 1);
}

static void to_scrap(TEHandle h)
{
	TECopy(h);
	ZeroScrap();
	PutScrap(LM_TESCRPLEN, TYPE('T', 'E', 'X', 'T'), *LM_TESCRPH);
}

static void do_menu(long mr)
{
	int id = (int)(mr >> 16), item = (int)(mr & 0xFFFF);
	SetPort(win);
	if (id == M_APPLE) {
		if (item == 1) about();
		else { GetItem(m_apple, item, pbuf); OpenDeskAcc(pbuf); }
	} else if (id == M_FILE) {
		if (item == F_SAVE) save_log(option_held());
		else if (item == F_CARD) save_to_card();
		else if (item == F_QUIT) quit = 1;
	} else if (id == M_EDIT) {
		if (item <= E_CLEAR && SystemEdit(item - 1)) { HiliteMenu(0); return; }     /* a desk accessory's */
		switch (item) {
		case E_CUT:   if (focus) { to_scrap(fte); TEDelete(fte); field_edited(0); } break;
		case E_COPY:  to_scrap(cur()); break;
		case E_PASTE: if (focus) paste_field(); break;
		case E_CLEAR: if (focus) { TEDelete(fte); field_edited(0); } break;
		case E_SELALL: TESetSelect(0, 32767, cur()); break;
		case E_FILTER: set_focus(1); TESetSelect(0, 32767, fte); break;
		case E_CLEARLOG: clear_log(); break;
		case E_EARLIER: show_earlier(); break;
		}
	} else if (id == M_VIEW) {
		if (item == V_FOLLOW) set_follow(!follow);
		else if (item == V_NOFILTER) empty_field();
		else if (item == V_ALLPARTS) box_click(all_btn, 0);
	}
	HiliteMenu(0);
}

/* ---- events ---- */
void c_ctl_action(void *ctl, int part)
{
	(void)ctl;
	int page = visible() > 1 ? visible() - 1 : 1;
	switch (part) {
	case inUpButton:   user_scroll(top - 1); break;
	case inDownButton: user_scroll(top + 1); break;
	case inPageUp:     user_scroll(top - page); break;
	case inPageDown:   user_scroll(top + page); break;
	}
}

static void activate(int on)
{
	int i, st = on ? 0 : 255;
	SetPort(win);
	active = on;
	if (on) TEActivate(cur());
	else TEDeactivate(cur());
	HiliteControl(vbar, st);
	for (i = 0; i < NCAT; i++) HiliteControl(cat_box[i], st);
	HiliteControl(all_btn, st);
	HiliteControl(clear_btn, st);
	HiliteControl(follow_box, st);
	HiliteControl(card_btn, st);
	DrawGrowIcon(win);
}

static void content_click(EventRecord *ev)
{
	Point p = ev->where;
	void *ctl = 0;
	int shift = (ev->modifiers & shiftKey) != 0;
	SetPort(win);
	GlobalToLocal(&p);
	int part = FindControl(pt_long(p), win, &ctl);
	if (ctl && ctl != vbar) {
		if (!TrackControl(ctl, pt_long(p), 0)) return;
		if (ctl == clear_btn) clear_log();
		else if (ctl == card_btn) save_to_card();
		else if (ctl == follow_box) set_follow(!follow);
		else box_click(ctl, (ev->modifiers & optionKey) != 0);
		return;
	}
	if (ctl == vbar && part) {
		if (part == inThumb) {
			TrackControl(ctl, pt_long(p), 0);
			user_scroll(GetCtlValue(vbar));
		} else TrackControl(ctl, pt_long(p), (void *)pas_ctl_action);
		return;
	}
	if ((*fte)->teLength && PtInRect(pt_long(p), &x_r)) { empty_field(); return; }
	if (PtInRect(pt_long(p), &field_r)) {
		set_focus(1);
		TEClick(pt_long(p), shift, fte);
	} else if (PtInRect(pt_long(p), &(*te)->viewRect)) {
		set_focus(0);
		TEClick(pt_long(p), shift, te);
	}
}

static void mouse_down(EventRecord *ev)
{
	void *w = 0;
	int part = FindWindow(pt_long(ev->where), &w);
	switch (part) {
	case inMenuBar: fix_menus(); do_menu(MenuSelect(pt_long(ev->where))); break;
	case inSysWindow: SystemClick(ev, w); break;
	case inDrag: DragWindow(w, pt_long(ev->where), RGN_BBOX(LM_GRAYRGN)); break;
	case inGoAway: if (w == win && TrackGoAway(w, pt_long(ev->where))) quit = 1; break;
	case inGrow:
		if (w == win) {
			Rect lim = { 100, 200, 4000, 4000 };     /* top/left: the smallest height and width */
			long sz = GrowWindow(w, pt_long(ev->where), &lim);
			if (sz) { SizeWindow(w, (short)(sz & 0xFFFF), (short)(sz >> 16), 1); resized(); }
		}
		break;
	case inZoomIn:
	case inZoomOut:
		if (w == win && TrackBox(w, pt_long(ev->where), part)) {
			SetPort(win);
			EraseRect(PORT_RECT(win));
			ZoomWindow(w, part, 0);
			resized();
		}
		break;
	case inContent:
		if (w != FrontWindow()) SelectWindow(w);
		else content_click(ev);
		break;
	}
}

static void key_down(EventRecord *ev)
{
	int c = (int)(ev->message & charCodeMask);
	if (ev->modifiers & cmdKey) {
		if (((ev->message >> 8) & 0xFF) == 0x01) c = 's';      /* S, whatever Option made of it */
		fix_menus();
		do_menu(MenuKey(c));
		return;
	}
	SetPort(win);
	int page = visible() > 1 ? visible() - 1 : 1;
	switch (c) {
	case 0x1E: user_scroll(top - 1); break;     /* up arrow */
	case 0x1F: user_scroll(top + 1); break;     /* down arrow */
	case 0x0B: user_scroll(top - page); break;  /* page up */
	case 0x0C: user_scroll(top + page); break;  /* page down */
	case 0x01: user_scroll(0); break;           /* home */
	case 0x04: user_scroll(32767); break;       /* end */
	case 0x1B: if ((*fte)->teLength) empty_field(); break;     /* Esc, and the keypad's Clear */
	case 0x0D: case 0x03: field_edited(1); break;              /* Return, Enter: the filter now */
	case 0x09: set_focus(!focus); if (focus) TESetSelect(0, 32767, fte); break;    /* Tab */
	case 0x1C: case 0x1D: if (focus) TEKey(c, fte); break;     /* left, right */
	default:
		/* the log takes no typing: letters, Delete and the rest go to the filter */
		if (c >= 32 || c == 0x08) field_key(c);
		break;
	}
}

static void update(void *w)
{
	if (w != win) return;
	SetPort(win);
	BeginUpdate(win);
	EraseRect(PORT_RECT(win));
	TEUpdate(PORT_RECT(win), te);
	TEUpdate(PORT_RECT(win), fte);
	FrameRect(&field_r);
	draw_x();
	TextFont(0); TextSize(12);
	MoveTo(label_h, BASE2);
	DrawString(pstr(pbuf, "Filter:"));
	TextFont(4); TextSize(9);
	draw_count();
	DrawControls(win);
	MoveTo(PORT_RECT(win)->left, STRIP - 1);
	LineTo(PORT_RECT(win)->right - 16, STRIP - 1);
	DrawGrowIcon(win);
	EndUpdate(win);
}

int main(void)
{
	setup_menus();
	win_base = LM_MMU32BIT ? (volatile unsigned char *)0xF9000000 : (volatile unsigned char *)0x00900000;
	lg = (volatile struct psmb_log *)(win_base + LOG_OFF);
	if (lg->magic != LOG_MAGIC || lg->size == 0 || lg->data_off != LOG_OFF + 0x1000) lg = 0;

	/* the main screen's top left, room for the menu bar; the SE's screen is 512 x 342 */
	Rect b = { 44, 8, 330, 504 };
	win = NewWindow(0, &b, pstr(pbuf, "PiLog"), 1, zoomDocProc, (void *)-1, 1, 0);
	SetPort(win);
	TextFont(4);        /* Monaco */
	TextSize(9);
	Rect r = *PORT_RECT(win);
	vbar = NewControl(win, &r, pstr(pbuf, ""), 1, 0, 0, 0, scrollBarProc, 0);
	te = (TEHandle)TENew(&r, &r);
	make_controls();
	layout();
	if (!lg) {
		static const char msg[] = "No kernel log here: PiLog needs an Aurora68 kernel from version 0.5 on.\r";
		add_text((const unsigned char *)msg, sizeof msg - 1);
	}
	refilter();

	unsigned long last = LM_TICKS;
	EventRecord ev;
	while (!quit) {
		long sleep = refilter_due ? 1 : fg ? POLL_TICKS : POLL_TICKS * 2;
		int got = WaitNextEvent(everyEvent, &ev, sleep, 0);
		if (got) {
			switch (ev.what) {
			case mouseDown: mouse_down(&ev); break;
			case keyDown: case autoKey: key_down(&ev); break;
			case updateEvt: update((void *)ev.message); break;
			case activateEvt: if ((void *)ev.message == win) activate(ev.modifiers & activeFlag); break;
			case osEvt:
				if (((unsigned long)ev.message >> 24) == suspendResumeMessage) {
					fg = ev.message & resumeFlag;
					activate(fg);
				}
				break;
			}
		}
		if (fg) TEIdle(cur());
		if (refilter_due && (long)(LM_TICKS - refilter_at) >= 0) {
			SetPort(win);
			refilter();
		}
		if (LM_TICKS - last >= POLL_TICKS) {
			last = LM_TICKS;
			SetPort(win);
			poll_log();
			if (card_asked) card_progress();
			if (note && (long)(note_until - LM_TICKS) <= 0) { note = 0; draw_count(); }
		}
	}
	return 0;
}
