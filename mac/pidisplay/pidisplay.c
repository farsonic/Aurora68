/* mac/pidisplay/pidisplay.c - "PiDisplay": the HDMI screen's monitor, resolution and rotation. MPL-2.0.
 *
 * Backstory. The SE in this project has two screens, its own and an HDMI monitor driven by the Pi
 * (docs/10). Until 21 November 2026 the HDMI mode was a line of config.txt typed on another
 * computer, after reading up on what a Pi 3 can send: the user's Dell UP2716D is 2560 by 1440, which
 * a Pi 3 sends at 30 Hz only. The monitor knows all of it about itself (its EDID), so the kernel now
 * reads it (emu68/src/psmac/psmac_hdmi.c, psmac_edid.c) and this program shows it and lets the mode
 * and the rotation be chosen on the Mac:
 *
 *   Monitor      its name, maker, model, serial number, date, size in cm and inches, shape
 *   Current      the HDMI mode in force, its refresh, where it comes from, the rotation, the depth
 *   Resolutions  every mode the monitor lists, each marked usable, too fast (with a slower
 *                alternative when one fits) or not the panel's shape; the monitor's preferred one
 *                marked, and the one suggested (the bullet) with its reason
 *   Rotation     0, 90, 180, 270 degrees: the picture turned clockwise on the monitor
 *   Sound        what the monitor plays over HDMI, and where the Mac's sound goes
 *   Connection   HDMI or DVI, the pixel clocks, the colour depths
 *
 * Restart to Apply writes the choice into aurora68-display.txt on the SD card (the kernel does it;
 * config.txt includes that file and is never touched) and restarts: the whole Pi for a new mode,
 * since the firmware reads config.txt only when the Pi starts, the Mac alone for a rotation. After
 * a new mode PiScrn asks "Keep this resolution?" on the SE's own screen, and the old mode comes back
 * if nobody answers within 15 seconds (pdcommon.c).
 *
 * Written to the 1992 Human Interface Guidelines, as the user asked: a document window that moves,
 * grows (the grow box), zooms to the screen it is on (the zoom box) and scrolls both ways, with a
 * layout of one column on a small screen and two on a wide one; the Apple, File and Edit menus in
 * their standard order with the standard keys, Copy copying the report, the Edit items it cannot
 * use dimmed (and passed to desk accessories); Save Report As (a TeachText file); an About box;
 * dialogs centred, their default buttons outlined, Command-period and Escape cancelling; activate,
 * update, suspend and resume handled, running in the background; the window's place and size kept
 * in a preferences file (the Preferences folder on System 7, the System Folder on System 6) and put
 * back on a screen if the one it was on is gone; Balloon Help for the menus on System 7 ('hmnu'
 * resources, tools/mac/mkhmnu.py); one-bit and colour screens alike; 32-bit clean. Built with the C
 * kit in mac/c (mac/c/README.md). Strings in English, in the code, as in the project's other programs.
 */
#include "pdcommon.h"

/* ---- glue.s ---- */
long mac_gestalt(long selector, long *answer);
short mac_findfolder(short vref, long type, int create, short *vout, long *dirid);
short mac_sysenvirons(void *rec);
void mac_restart(void);
void mac_disposeptr(void *p);
short mac_PBRead(ParamBlock *pb), mac_PBHOpen(ParamBlock *pb), mac_PBHCreate(ParamBlock *pb),
      mac_PBHGetFInfo(ParamBlock *pb), mac_PBHSetFInfo(ParamBlock *pb);
#define PB_DIRID(pb) (*(long *)((char *)(pb) + 48))   /* HFileParam's ioDirID */

/* ---- state ---- */
static volatile unsigned char *w;          /* the card's window, 0 without the card */
static struct psmb_dispinfo *di;           /* the kernel's answer (a pointer: 2 KB) */
static struct psmb_dispinfo *di_new;
static int have;                           /* di holds an answer */
static const char *why_not;                /* when there is no answer: why */
static int sel = -1;                       /* the resolution chosen in the list */
static int cur = -1;                       /* the one in force */
static int rot_sel;                        /* the rotation chosen */
static int ltop;                           /* the list's first row shown */
static int busy;                           /* writing to the card, about to restart */
static int quit, active = 1, fg = 1;
static void *win, *vbar, *hbar, *lbar, *b_apply, *b_revert, *radio[4];
static void *m_apple, *m_file, *m_edit, *m_display;
static unsigned char pbuf[256];
static char note[96];                      /* the strip's message */
static int sx, sy;                         /* the document's scroll position */
static int doc_w, doc_h;                   /* its size */

enum { M_APPLE = 128, M_FILE, M_EDIT, M_DISPLAY };
enum { F_CLOSE = 1, F_SAVE, F_QUIT = 4 };
enum { E_UNDO = 1, E_CUT = 3, E_COPY, E_PASTE, E_CLEAR, E_SELALL = 8 };
enum { D_SUGGEST = 1, D_REVERT, D_APPLY = 4 };

#define STRIP   28                         /* the top strip: the message and the two buttons */
#define SBAR    15
#define LINE_H  12                         /* Geneva 9 */
#define HEAD_H  18                         /* a section's heading, Chicago 12 */
#define ROW_H   13                         /* a row of the list */
#define LABEL_W 92                         /* the labels' column */
#define MIN_DOC_W 470

static int selectable(int i)
{
	unsigned f;
	if (!have || i < 0 || i >= di->nmodes) return 0;
	f = di->mode[i].flags;
	return (f & (PSMB_DM_USABLE | PSMB_DM_ALT)) && !(f & (PSMB_DM_MACSIZE | PSMB_DM_INTERLACED));
}
static int uses_alt(int i) { return i >= 0 && !(di->mode[i].flags & PSMB_DM_USABLE) && (di->mode[i].flags & PSMB_DM_ALT); }
static unsigned refresh_of(int i) { return uses_alt(i) ? di->mode[i].alt_refresh : di->mode[i].refresh; }
static int changed_mode(void) { return have && sel >= 0 && sel != cur; }
static int changed_rot(void) { return have && rot_sel != di->rotation_next; }

/* ---- text: the sections as lines ---- */
#define MAXL 16
struct section { const char *title; int n; char label[MAXL][24]; char text[MAXL][120]; };
static struct section s_mon, s_cur, s_snd, s_con;
static char reason_line[140], list_head[80];

static void add(struct section *s, const char *label, const char *text)
{
	if (s->n >= MAXL) return;
	pd_put(s->label[s->n], label);
	char *p = s->text[s->n]; int k = 0;
	while (text[k] && k < 119) { p[k] = text[k]; k++; }
	p[k] = 0;
	s->n++;
}
static char *mm_inch(char *p, unsigned mm)              /* tenths of an inch */
{
	unsigned t = (mm * 100 + 127) / 254;
	p = pd_num(p, t / 10); *p++ = '.'; *p++ = (char)('0' + t % 10); *p = 0;
	return p;
}
static unsigned isqrt(unsigned long v) { unsigned long r = 0; while ((r + 1) * (r + 1) <= v) r++; return (unsigned)r; }
static char *size_str(char *p, int i)
{
	p = pd_num(p, di->mode[i].width); p = pd_put(p, " x "); p = pd_num(p, di->mode[i].height);
	return p;
}
static void status_of(int i, char *p)
{
	unsigned f = di->mode[i].flags;
	if (f & PSMB_DM_MACSIZE) p = pd_put(p, "not a size the Mac takes");
	else if (f & PSMB_DM_INTERLACED) p = pd_put(p, "interlaced: not for the Mac");
	else if (f & PSMB_DM_USABLE) p = pd_put(p, "usable");
	else if (f & PSMB_DM_ALT) { p = pd_put(p, "too fast; "); p = pd_hz(p, di->mode[i].alt_refresh); p = pd_put(p, " Hz fits"); }
	else p = pd_put(p, "too fast for the Pi 3");
	if ((f & PSMB_DM_SHAPE) && !(f & (PSMB_DM_MACSIZE | PSMB_DM_INTERLACED))) p = pd_put(p, ", wrong shape");
	if (f & PSMB_DM_PREFERRED) p = pd_put(p, " (preferred)");
	if (f & PSMB_DM_CURRENT) p = pd_put(p, " (in use)");
}
static const char *const speaker_names[7] = { "front left and right", "subwoofer", "front centre", "rear left and right",
                                              "rear centre", "front left and right of centre", "rear left and right of centre" };
static void build_text(void)
{
	char t[160], *p;
	unsigned i;
	s_mon.n = s_cur.n = s_snd.n = s_con.n = 0;
	s_mon.title = "Monitor"; s_cur.title = "Current"; s_snd.title = "Sound"; s_con.title = "Connection";
	reason_line[0] = 0;
	if (!have) { add(&s_mon, "", why_not ? why_not : "No answer from the kernel."); return; }
	if (di->flags & PSMB_DI_EDID) {
		add(&s_mon, "Name:", di->name[0] ? di->name : "(the monitor gives none)");
		p = pd_put(t, di->maker_name[0] ? di->maker_name : "unknown"); p = pd_put(p, " ("); p = pd_put(p, di->maker); p = pd_put(p, ")");
		if ((di->flags & PSMB_DI_SAMPLE) && di->maker_name[0] != 'a') p = pd_put(p, ", a sample, not a monitor");
		add(&s_mon, "Maker:", t);
		p = pd_put(t, "product ");
		{ static const char hx[] = "0123456789ABCDEF"; for (i = 0; i < 4; i++) *p++ = hx[(di->product >> (12 - 4 * i)) & 15]; *p = 0; }
		add(&s_mon, "Model:", t);
		p = t; *p = 0;
		if (di->serial_text[0]) p = pd_put(p, di->serial_text);
		if (di->serial) { if (di->serial_text[0]) p = pd_put(p, " ("); p = pd_num(p, di->serial); if (di->serial_text[0]) p = pd_put(p, ")"); }
		if (t[0]) add(&s_mon, "Serial number:", t);
		p = t;
		if (di->week == 255) { p = pd_put(p, "model year "); p = pd_num(p, di->year); }
		else { if (di->week) { p = pd_put(p, "week "); p = pd_num(p, di->week); p = pd_put(p, " of "); } p = pd_num(p, di->year); }
		add(&s_mon, "Made:", t);
		if (di->size_w_mm && di->size_h_mm) {
			p = pd_num(t, (di->size_w_mm + 5) / 10); p = pd_put(p, " x "); p = pd_num(p, (di->size_h_mm + 5) / 10); p = pd_put(p, " cm, ");
			p = mm_inch(p, di->size_w_mm); p = pd_put(p, " x "); p = mm_inch(p, di->size_h_mm); p = pd_put(p, " inches, ");
			p = mm_inch(p, isqrt((unsigned long)di->size_w_mm * di->size_w_mm + (unsigned long)di->size_h_mm * di->size_h_mm));
			p = pd_put(p, "\" diagonal");
			add(&s_mon, "Picture size:", t);
		}
		for (i = 0; i < di->nmodes; i++)
			if (di->mode[i].flags & PSMB_DM_NATIVE) {
				p = pd_num(t, di->mode[i].aspect_w); p = pd_put(p, ":"); p = pd_num(p, di->mode[i].aspect_h);
				p = pd_put(p, ", "); p = size_str(p, (int)i); p = pd_put(p, " pixels");
				add(&s_mon, "Shape:", t);
				break;
			}
		p = pd_put(t, "EDID "); p = pd_num(p, di->edid_version >> 8); p = pd_put(p, "."); p = pd_num(p, di->edid_version & 255);
		p = pd_put(p, ", "); p = pd_num(p, di->edid_blocks); p = pd_put(p, di->edid_blocks == 1 ? " block" : " blocks");
		if (di->edid_status == PSMB_ES_CHECKSUM) p = pd_put(p, " (a checksum is wrong)");
		add(&s_mon, "Description:", t);
	} else add(&s_mon, "", di->edid_status == PSMB_ES_HEADER ? "The monitor's description (EDID) is not readable." :
	                       "No description (EDID) from the monitor: none is connected, or the firmware has none.");

	/* current */
	p = pd_num(t, di->cur_w); p = pd_put(p, " x "); p = pd_num(p, di->cur_h);
	if (di->cur_refresh) { p = pd_put(p, " at "); p = pd_hz(p, di->cur_refresh); p = pd_put(p, " Hz"); }
	add(&s_cur, "HDMI mode:", t);
	p = pd_put(t, di->cur_source == PSMB_SRC_FILE ? "aurora68-display.txt: " : di->cur_source == PSMB_SRC_CONFIG ? "config.txt: " : "");
	p = pd_put(p, di->cur_text);
	add(&s_cur, "Set by:", t);
	p = pd_num(t, di->mac_w); p = pd_put(p, " x "); p = pd_num(p, di->mac_h);
	p = pd_put(p, ", turned "); p = pd_num(p, di->rotation); p = pd_put(p, "\xA1");
	if (di->rotation_next != di->rotation) { p = pd_put(p, " ("); p = pd_num(p, di->rotation_next); p = pd_put(p, "\xA1 from the next restart)"); }
	add(&s_cur, "Mac's screen:", t);
	{   /* the depth the Mac draws in: the HDMI screen's display block (psmb.h) */
		volatile unsigned long *d = (volatile unsigned long *)(w + (di->board == 2 ? 0x59000UL : 0x58000UL));
		unsigned long dep = d[0] == 0x41363856UL ? d[4] : 0;
		static const char *const names[] = { "", "black and white", "4 greys or colours", "", "16 colours", "", "", "", "256 colours" };
		p = t;
		if (dep == 32) p = pd_put(p, "millions of colours");
		else if (dep == 16) p = pd_put(p, "thousands of colours");
		else if (dep >= 1 && dep <= 8 && names[dep][0]) p = pd_put(p, names[dep]);
		else p = pd_put(p, "not drawn yet");
		p = pd_put(p, "; the Pi sends ");
		p = pd_put(p, di->vc_depth == 32 ? "millions (32-bit buffer)" : "65,536 colours (16-bit buffer)");
		add(&s_cur, "Depth:", t);
	}
	if (di->frame_us) { p = pd_num(t, (di->frame_us + 500) / 1000); p = pd_put(p, " ms of the Pi's core 3 for the longest frame lately"); add(&s_cur, "Copy:", t); }
	if (!(di->cfg & PSMB_CF_CONFIG)) add(&s_cur, "Note:", "config.txt was not read (the Pi did not start from its card, or the testbed).");
	else if (!(di->cfg & PSMB_CF_INCLUDE)) add(&s_cur, "Note:", "config.txt has no line \"include aurora68-display.txt\": add it at the end, or a new resolution is not used.");
	else if (di->cfg & PSMB_CF_OVERRIDE) add(&s_cur, "Note:", "config.txt sets an hdmi_ mode after its include line, which wins: move the include line to the end.");
	if (!(di->cfg & PSMB_CF_WRITABLE)) add(&s_cur, "Note:", "The SD card cannot be written: a resolution can be looked at but not chosen.");
	if (di->confirm_state == PSMB_CS_PENDING || di->confirm_state == PSMB_CS_ASKING) add(&s_cur, "Note:", "This mode waits to be kept (the question on the main screen).");
	if (di->confirm_state == PSMB_CS_REVERTING) add(&s_cur, "Note:", "The previous mode is being put back; the Pi restarts.");

	/* the list's heading and the suggestion */
	p = pd_put(list_head, "Pi 3: up to "); p = pd_mhz(p, di->pi_pixclk); p = pd_put(p, " MHz");
	if (di->max_pixclk) { p = pd_put(p, "; the monitor: "); p = pd_mhz(p, di->max_pixclk); p = pd_put(p, " MHz"); }
	if (di->suggested != 0xFFFF) {
		p = pd_put(reason_line, "\xA5 Suggested: "); p = size_str(p, di->suggested); p = pd_put(p, " at ");
		p = pd_hz(p, refresh_of(di->suggested)); p = pd_put(p, " Hz. "); p = pd_put(p, di->reason); p = pd_put(p, ".");
	} else pd_put(reason_line, di->nmodes ? "No mode is suggested: none fits both the Pi 3 and the Mac." : "");

	/* sound */
	if (!(di->flags & PSMB_DI_EDID)) add(&s_snd, "", "Not known without the monitor's description.");
	else if (!(di->conn & PSMB_CN_HDMI)) add(&s_snd, "", "None: the monitor is connected as DVI, which carries no sound.");
	else if (!(di->audio_formats & 2)) add(&s_snd, "", di->conn & PSMB_CN_BASICAUDIO ? "Basic HDMI sound (stereo PCM)." : "The monitor lists no sound.");
	else {
		static const char *const rates[7] = { "32", "44.1", "48", "88.2", "96", "176.4", "192" };
		static const char *const bits[3] = { "16", "20", "24" };
		int first = 1;
		p = pd_put(t, "PCM, "); p = pd_num(p, di->lpcm_channels); p = pd_put(p, di->lpcm_channels == 1 ? " channel, " : " channels, ");
		for (i = 0; i < 7; i++) if (di->lpcm_rates & (1u << i)) { if (!first) p = pd_put(p, ", "); p = pd_put(p, rates[i]); first = 0; }
		p = pd_put(p, " kHz, ");
		first = 1;
		for (i = 0; i < 3; i++) if (di->lpcm_bits & (1u << i)) { if (!first) p = pd_put(p, "/"); p = pd_put(p, bits[i]); first = 0; }
		p = pd_put(p, " bits");
		if (di->audio_formats & ~2u) p = pd_put(p, "; compressed formats too");
		add(&s_snd, "Formats:", t);
		p = t; *p = 0;
		first = 1;
		for (i = 0; i < 7; i++) if (di->speakers & (1u << i)) { if (!first) p = pd_put(p, ", "); p = pd_put(p, speaker_names[i]); first = 0; }
		if (first) pd_put(t, "not said (a monitor with a line-out only, perhaps)");
		add(&s_snd, "Speakers:", t);
	}
	{   /* where the Mac's sound goes: the sound card's block (psmb.h, struct psmb_sound at window + $11000) */
		volatile unsigned long *s = (volatile unsigned long *)(w + 0x11000UL);
		unsigned long k = s[0] == 0x41363853UL ? s[5] : 9;
		add(&s_snd, "The Mac plays:", k == 1 ? "through HDMI: the monitor's speakers, or its line-out" : k == 2 ? "through the Pi's headphone socket" :
		    k == 3 ? "into a file (the testbed)" : k == 0 ? "nowhere on the Pi (mac.audio=off, or no output)" : "not known (an older kernel)");
	}

	/* connection */
	if (di->flags & PSMB_DI_EDID) {
		p = pd_put(t, di->conn & PSMB_CN_HDMI ? "HDMI" : "DVI (no HDMI vendor block)");
		if (di->conn & PSMB_CN_DIGITAL) p = pd_put(p, ", digital input");
		add(&s_con, "Kind:", t);
		p = pd_put(t, "the Pi 3 "); p = pd_mhz(p, di->pi_pixclk); p = pd_put(p, " MHz");
		if (di->max_pixclk) { p = pd_put(p, ", the monitor "); p = pd_mhz(p, di->max_pixclk); p = pd_put(p, " MHz"); }
		if (di->tmds_max) { p = pd_put(p, ", HDMI link "); p = pd_mhz(p, di->tmds_max); p = pd_put(p, " MHz"); }
		add(&s_con, "Pixel clock:", t);
		if (di->min_vhz) {
			p = pd_num(t, di->min_vhz); p = pd_put(p, "\xD0"); p = pd_num(p, di->max_vhz); p = pd_put(p, " Hz vertical, ");
			p = pd_num(p, di->min_hkhz); p = pd_put(p, "\xD0"); p = pd_num(p, di->max_hkhz); p = pd_put(p, " kHz horizontal");
			add(&s_con, "Range:", t);
		}
		static const char *const bpc[8] = { "not said", "6", "8", "10", "12", "14", "16", "" };
		p = pd_put(t, "");
		if (di->edid_version >= 0x0104 && (di->input & 0x80)) { p = pd_put(p, bpc[(di->input >> 4) & 7]); p = pd_put(p, " bits a colour"); }
		else p = pd_put(p, "8 bits a colour");
		if (di->conn & (PSMB_CN_DC30 | PSMB_CN_DC36 | PSMB_CN_DC48)) {
			p = pd_put(p, "; deep colour");
			if (di->conn & PSMB_CN_DC30) p = pd_put(p, " 30");
			if (di->conn & PSMB_CN_DC36) p = pd_put(p, " 36");
			if (di->conn & PSMB_CN_DC48) p = pd_put(p, " 48");
			p = pd_put(p, " bits");
		}
		if (di->conn & PSMB_CN_YCC444) p = pd_put(p, "; YCbCr 4:4:4");
		if (di->conn & PSMB_CN_YCC422) p = pd_put(p, ", 4:2:2");
		add(&s_con, "Colour:", t);
	} else add(&s_con, "", "Not known.");
}

/* ---- the strip's message ---- */
static void set_note(const char *s) { pd_put(note, s); }
static void strip_note(void)
{
	char *p;
	if (busy) return;
	if (!have) { set_note(why_not ? "No HDMI information" : ""); return; }
	if (changed_mode() || changed_rot()) {
		p = pd_put(note, "Chosen: ");
		if (sel >= 0) { p = size_str(p, sel); p = pd_put(p, " at "); p = pd_hz(p, refresh_of(sel)); p = pd_put(p, " Hz, "); }
		p = pd_num(p, (unsigned long)rot_sel); pd_put(p, "\xA1");
	} else {
		p = pd_put(note, di->name[0] ? di->name : "HDMI"); p = pd_put(p, ": ");
		p = pd_num(p, di->cur_w); p = pd_put(p, " x "); p = pd_num(p, di->cur_h);
		if (di->cur_refresh) { p = pd_put(p, " at "); p = pd_hz(p, di->cur_refresh); pd_put(p, " Hz"); }
	}
}

/* ---- layout ---- */
static Rect content(void)
{
	Rect r = *PORT_RECT(win);
	r.top += STRIP; r.right -= SBAR; r.bottom -= SBAR;
	return r;
}
static int two_cols;
static int col_w[2], col_x[2];
static int list_rows;
/* where each part is in the document: tops of the sections, the list and the radios */
static int y_mon, y_cur, y_snd, y_con, y_res, y_list, y_reason, y_rot, reason_lines;
static int x_mon, x_cur, x_snd, x_con, x_res;

/* the lines a text takes at width wd (Geneva 9), and with draw, drawn from (x, y) */
static int wrap(const char *s, int x, int y, int wd, int draw)
{
	int n = (int)strlen(s), start = 0, lines = 0;
	while (start < n) {
		int end = start, last = -1;
		while (end < n && TextWidth(s, start, end - start + 1) <= wd) { if (s[end] == ' ') last = end; end++; }
		if (end < n && last > start) end = last + 1;
		if (end == start) end = start + 1;
		if (draw) { MoveTo(x, y + lines * LINE_H); DrawText(s, start, end - start); }
		lines++;
		start = end;
	}
	return lines ? lines : 1;
}
static int section_h(const struct section *s, int wd)
{
	int h = HEAD_H, i;
	TextFont(3); TextSize(9);
	for (i = 0; i < s->n; i++) h += wrap(s->text[i], 0, 0, s->label[i][0] ? wd - LABEL_W : wd, 0) * LINE_H;
	return h + 8;
}
/* a control inside the document: moved and shown only when its place or visibility changes, so that a
   redraw does not hide and show it (which on a slow screen is a flicker, and erased what was drawn) */
static void place(void *ctl, int x, int y, int w, int h, int show)
{
	Rect *r = (Rect *)((char *)*(void **)ctl + 8);            /* contrlRect */
	unsigned char vis = *((unsigned char *)*(void **)ctl + 16);  /* contrlVis */
	if (r->left == x && r->top == y && r->right - r->left == w && r->bottom - r->top == h && (vis != 0) == (show != 0)) return;
	if (vis) HideControl(ctl);
	MoveControl(ctl, x, y); SizeControl(ctl, w, h);
	if (show) ShowControl(ctl);
}
static void layout(void)
{
	Rect c = content();
	int cw = c.right - c.left, ch = c.bottom - c.top, i;
	two_cols = cw >= 720;
	if (two_cols) {
		doc_w = cw;
		col_x[0] = 8; col_w[0] = (cw - 24) * 45 / 100; col_x[1] = col_x[0] + col_w[0] + 8; col_w[1] = cw - col_x[1] - 8;
		x_mon = x_cur = x_snd = x_con = col_x[0]; x_res = col_x[1];
		y_mon = 6; y_cur = y_mon + section_h(&s_mon, col_w[0]); y_snd = y_cur + section_h(&s_cur, col_w[0]); y_con = y_snd + section_h(&s_snd, col_w[0]);
		int left_h = y_con + section_h(&s_con, col_w[0]);
		y_res = 6;
		TextFont(3); TextSize(9);
		reason_lines = wrap(reason_line, 0, 0, col_w[1], 0);
		y_list = y_res + HEAD_H + LINE_H + 4;
		/* the list takes the height the rest of the column leaves: its frame, the reason, Rotation */
		list_rows = (ch - y_list - 2 - 4 - (LINE_H - 2) - reason_lines * LINE_H - 6 - HEAD_H - 26) / ROW_H;
		if (list_rows < 8) list_rows = 8;
		if (have && list_rows > di->nmodes) list_rows = di->nmodes < 8 ? 8 : di->nmodes;
		y_reason = y_list + list_rows * ROW_H + 4 + LINE_H - 2;
		y_rot = y_reason + reason_lines * LINE_H + 6;
		int right_h = y_rot + HEAD_H + 26;
		doc_h = left_h > right_h ? left_h : right_h;
	} else {
		doc_w = cw > MIN_DOC_W ? cw : MIN_DOC_W;
		col_x[0] = col_x[1] = 8; col_w[0] = col_w[1] = doc_w - 16;
		x_mon = x_cur = x_snd = x_con = x_res = 8;
		y_mon = 6; y_cur = y_mon + section_h(&s_mon, col_w[0]);
		y_res = y_cur + section_h(&s_cur, col_w[0]);
		list_rows = 8;
		if (have && di->nmodes && di->nmodes < 8) list_rows = 4 > di->nmodes ? 4 : di->nmodes;
		TextFont(3); TextSize(9);
		reason_lines = wrap(reason_line, 0, 0, col_w[1], 0);
		y_list = y_res + HEAD_H + LINE_H + 4;
		y_reason = y_list + list_rows * ROW_H + 4 + LINE_H - 2;
		y_rot = y_reason + reason_lines * LINE_H + 6;
		y_snd = y_rot + HEAD_H + 30;
		y_con = y_snd + section_h(&s_snd, col_w[0]);
		doc_h = y_con + section_h(&s_con, col_w[0]);
	}
	/* the scroll bars */
	int mx = doc_w - cw, my = doc_h - ch;
	if (mx < 0) mx = 0;
	if (my < 0) my = 0;
	if (sx > mx) sx = mx;
	if (sy > my) sy = my;
	Rect r = *PORT_RECT(win);
	HideControl(vbar); HideControl(hbar);
	MoveControl(vbar, r.right - SBAR, STRIP - 1); SizeControl(vbar, SBAR + 1, r.bottom - STRIP - SBAR + 2);
	MoveControl(hbar, -1, r.bottom - SBAR); SizeControl(hbar, r.right - SBAR + 2, SBAR + 1);
	SetCtlMax(vbar, my); SetCtlValue(vbar, sy); SetCtlMax(hbar, mx); SetCtlValue(hbar, sx);
	ShowControl(vbar); ShowControl(hbar);
	/* the strip's buttons, at its right, measured in the system font they are drawn in */
	TextFont(0); TextSize(12);
	int bw = 16 + StringWidth(pstr(pbuf, "Restart to Apply\xC9")) , rw = 16 + StringWidth(pstr(pbuf, "Revert"));
	MoveControl(b_apply, r.right - 8 - bw, 4); SizeControl(b_apply, bw, 20);
	MoveControl(b_revert, r.right - 16 - bw - rw, 4); SizeControl(b_revert, rw, 20);
	/* the controls inside the document move with it, and hide when they are not wholly in view */
	int lmax = have && di->nmodes > list_rows ? di->nmodes - list_rows : 0;
	if (ltop > lmax) ltop = lmax;
	SetCtlMax(lbar, lmax); SetCtlValue(lbar, ltop);
	Rect lb = { (short)(c.top + y_list - sy), (short)(c.left + x_res + col_w[1] - SBAR - sx), (short)(c.top + y_list + list_rows * ROW_H + 2 - sy), (short)(c.left + x_res + col_w[1] + 1 - sx) };
	place(lbar, lb.left, lb.top, lb.right - lb.left, lb.bottom - lb.top,
	      have && lb.top >= c.top && lb.bottom <= c.bottom && lb.right <= c.right && lb.left >= c.left);
	static const char *const rn[4] = { "0\xA1", "90\xA1", "180\xA1", "270\xA1" };
	TextFont(0); TextSize(12);
	int x = c.left + x_res + 4 - sx, y = c.top + y_rot + HEAD_H - sy;
	for (i = 0; i < 4; i++) {
		int bwid = 24 + StringWidth(pstr(pbuf, rn[i]));
		place(radio[i], x, y, bwid, 18, have && y >= c.top && y + 18 <= c.bottom && x + bwid <= c.right && x >= c.left);
		x += bwid + 14;
	}
}

/* ---- drawing ---- */
static void heading(const char *t, int x, int y, int wd)
{
	TextFont(0); TextSize(12);
	MoveTo(x, y + 13); DrawText(t, 0, (int)strlen(t));
	MoveTo(x + StringWidth(pstr(pbuf, t)) + 6, y + 8); LineTo(x + wd, y + 8);
}
static void draw_section(const struct section *s, int x, int y, int wd)
{
	Rect c = content();
	int i, yy;
	x += c.left - sx; y += c.top - sy;
	heading(s->title, x, y, wd);
	TextFont(3); TextSize(9);
	yy = y + HEAD_H + 9;
	for (i = 0; i < s->n; i++) {
		if (s->label[i][0]) { MoveTo(x, yy); DrawText(s->label[i], 0, (int)strlen(s->label[i])); yy += wrap(s->text[i], x + LABEL_W, yy, wd - LABEL_W, 1) * LINE_H; }
		else yy += wrap(s->text[i], x, yy, wd, 1) * LINE_H;
	}
}
static Rect row_rect(int k)
{
	Rect c = content(), r;
	r.top = (short)(c.top + y_list + 1 + (k - ltop) * ROW_H - sy); r.bottom = (short)(r.top + ROW_H);
	r.left = (short)(c.left + x_res + 1 - sx); r.right = (short)(c.left + x_res + col_w[1] - SBAR - sx);
	return r;
}
static void draw_row(int k)
{
	Rect r = row_rect(k), cl = content(), clip;
	char t[120], *p;
	if (!SectRect(&r, &cl, &clip)) return;
	ClipRect(&clip);
	EraseRect(&r);
	if (k < di->nmodes) {
		const struct psmb_dispmode *m = &di->mode[k];
		int x = r.left + 4, y = r.top + 10;
		TextFont(3); TextSize(9);
		TextFace((m->flags & PSMB_DM_SUGGESTED) ? 1 : 0);
		if (m->flags & PSMB_DM_SUGGESTED) { MoveTo(x, y); DrawText("\xA5", 0, 1); }
		p = size_str(t, k); MoveTo(x + 10, y); DrawText(t, 0, (int)(p - t));
		p = pd_hz(t, m->refresh); p = pd_put(p, " Hz"); MoveTo(x + 88, y); DrawText(t, 0, (int)(p - t));
		p = pd_mhz(t, m->pixclk); p = pd_put(p, " MHz"); MoveTo(x + 142, y); DrawText(t, 0, (int)(p - t));
		status_of(k, t); MoveTo(x + 212, y); DrawText(t, 0, (int)strlen(t));
		TextFace(0);
		if (!selectable(k)) {                      /* what cannot be chosen is shown dimmed (grey over it) */
			static const unsigned char grey[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
			PenPat(grey); PenMode(11); PaintRect(&r); PenNormal();       /* patBic: every other pixel cleared */
		}
		if (k == sel) {
			if (active) InvertRect(&r);
			else FrameRect(&r);                    /* an inactive window's selection: an outline */
		}
	}
	ClipRect(PORT_RECT(win));
}
static void draw_list(void)
{
	Rect c = content(), fr, clip;
	int k;
	fr.top = (short)(c.top + y_list - sy); fr.left = (short)(c.left + x_res - sx);
	fr.bottom = (short)(fr.top + list_rows * ROW_H + 2); fr.right = (short)(c.left + x_res + col_w[1] + 1 - sx);
	if (SectRect(&fr, &c, &clip)) { ClipRect(&clip); FrameRect(&fr); ClipRect(PORT_RECT(win)); }
	for (k = ltop; k < ltop + list_rows; k++) draw_row(k);
}
static void draw_strip(void)
{
	Rect r = *PORT_RECT(win), s = { 0, 0, STRIP - 1, 0 };
	s.right = r.right;
	TextFont(0); TextSize(12);
	int bw = 16 + StringWidth(pstr(pbuf, "Restart to Apply\xC9")), rw = 16 + StringWidth(pstr(pbuf, "Revert"));
	s.right = (short)(r.right - 24 - bw - rw);
	EraseRect(&s);
	ClipRect(&s);
	TextFont(3); TextSize(9);
	MoveTo(8, 17);
	DrawText(note, 0, (int)strlen(note));
	ClipRect(PORT_RECT(win));
	MoveTo(0, STRIP - 1); LineTo(r.right, STRIP - 1);
	Draw1Control(b_apply); Draw1Control(b_revert);
}
static void draw_content(void)
{
	Rect c = content();
	ClipRect(&c);
	EraseRect(&c);
	draw_section(&s_mon, x_mon, y_mon, col_w[0]);
	draw_section(&s_cur, x_cur, y_cur, col_w[0]);
	if (have) {
		int x = c.left + x_res - sx, y = c.top + y_res - sy;
		heading("Resolutions", x, y, col_w[1]);
		TextFont(3); TextSize(9);
		MoveTo(x, y + HEAD_H + 8); DrawText(list_head, 0, (int)strlen(list_head));
		wrap(reason_line, x, c.top + y_reason - sy, col_w[1], 1);
		ClipRect(PORT_RECT(win));
		draw_list();
		ClipRect(&c);
		heading("Rotation", c.left + x_res - sx, c.top + y_rot - sy, col_w[1]);
	}
	draw_section(&s_snd, x_snd, y_snd, col_w[0]);
	draw_section(&s_con, x_con, y_con, col_w[0]);
	ClipRect(PORT_RECT(win));
	{ int i; for (i = 0; i < 4; i++) Draw1Control(radio[i]); }
	Draw1Control(lbar);
}

/* HiliteControl draws the control again even when its state does not change, so on the real SE (21
   November 2026) the two buttons, dimmed, flickered at every look at the kernel's answer: the state is
   set only when it differs from the control record's contrlHilite (byte 17: nextControl, contrlOwner,
   contrlRect, contrlVis come first). SetCtlValue likewise only for a change (contrlValue at 18). */
static void set_hilite(void *h, int v)
{
	if ((*(unsigned char **)h)[17] != (unsigned char)v) HiliteControl(h, v);
}
static void set_value(void *h, int v)
{
	if (*(short *)(*(unsigned char **)h + 18) != v) SetCtlValue(h, v);
}
static void fix_buttons(void)
{
	int can = have && !busy && (di->cfg & PSMB_CF_WRITABLE) && (changed_mode() || changed_rot())
	          && di->confirm_state != PSMB_CS_REVERTING;
	set_hilite(b_apply, active && can ? 0 : 255);
	set_hilite(b_revert, active && have && !busy && (changed_mode() || changed_rot()) ? 0 : 255);
	int i;
	for (i = 0; i < 4; i++) { set_value(radio[i], have && rot_sel == i * 90); set_hilite(radio[i], active && have && !busy ? 0 : 255); }
}
static void redraw_all(void) { SetPort(win); InvalRect(PORT_RECT(win)); }

/* ---- the kernel's answer ---- */
static void take_answer(int first)
{
	int i;
	have = 1;
	cur = -1;
	for (i = 0; i < di->nmodes; i++) if (di->mode[i].flags & PSMB_DM_CURRENT) cur = i;
	if (first) { sel = cur >= 0 ? cur : -1; rot_sel = di->rotation_next; }
	build_text();
	strip_note();
}
static int refresh(int first)
{
	if (!w) return 0;
	if (!pd_fetch(w, di_new)) {
		if (first) { have = 0; why_not = "The Pi's kernel does not answer PiDisplay's question: it needs an Aurora68 kernel from 21 November 2026 on."; build_text(); }
		return 0;
	}
	/* only a change is redrawn: the answer is the same most of the time, and the copy's time on core 3
	   (frame_us), which moves at every look, waits for a change of anything else rather than redrawing
	   the whole window every two seconds */
	if (!first && have) {
		unsigned short us = di_new->frame_us;
		di_new->frame_us = di->frame_us;
		int same = !memcmp(di_new, di, sizeof *di);
		di_new->frame_us = us;
		if (same) return 0;
	}
	memcpy(di, di_new, sizeof *di);
	take_answer(first);
	return 1;
}

/* ---- the report: Copy and Save Report As ---- */
static char *rep_section(char *p, const struct section *s)
{
	int i;
	p = pd_put(p, s->title); *p++ = '\r';
	for (i = 0; i < s->n; i++) { p = pd_put(p, "  "); p = pd_put(p, s->label[i]); if (s->label[i][0]) *p++ = ' '; p = pd_put(p, s->text[i]); *p++ = '\r'; }
	return p;
}
static long build_report(char *buf)
{
	char *p = buf, t[120];
	int i;
	p = pd_put(p, "PiDisplay: the Aurora68 card's HDMI screen\r\r");
	p = rep_section(p, &s_mon);
	p = rep_section(p, &s_cur);
	if (have) {
		p = pd_put(p, "Resolutions ("); p = pd_put(p, list_head); p = pd_put(p, ")\r");
		for (i = 0; i < di->nmodes; i++) {
			const struct psmb_dispmode *m = &di->mode[i];
			p = pd_put(p, (m->flags & PSMB_DM_SUGGESTED) ? "* " : "  ");
			p = size_str(p, i); p = pd_put(p, "  "); p = pd_hz(p, m->refresh); p = pd_put(p, " Hz  "); p = pd_mhz(p, m->pixclk); p = pd_put(p, " MHz  ");
			status_of(i, t); p = pd_put(p, t); *p++ = '\r';
		}
		p = pd_put(p, reason_line); *p++ = '\r';
		p = pd_put(p, "Rotation\r  "); p = pd_num(p, di->rotation); p = pd_put(p, " degrees\r");
	}
	p = rep_section(p, &s_snd);
	p = rep_section(p, &s_con);
	return p - buf;
}
static void copy_report(void)
{
	char *buf = mac_newptr(12000);
	if (!buf) { SysBeep(5); return; }
	long n = build_report(buf);
	ZeroScrap();
	PutScrap(n, TYPE('T', 'E', 'X', 'T'), buf);
	mac_disposeptr(buf);
}
static void save_report(void)
{
	SFReply rep;
	Point where = { 60, 104 };
	if (LM_MAINDEVICE) { Rect m = *GD_RECT(LM_MAINDEVICE); where.h = (short)(m.left + (m.right - m.left - 304) / 2); where.v = (short)(m.top + 50); }
	SFPutFile(pt_long(where), pstr(pbuf, "Save the report as:"), pstr(pbuf + 128, "PiDisplay Report"), 0, &rep, 1);
	SetPort(win);
	if (!rep.good) return;
	char *buf = mac_newptr(12000);
	if (!buf) { SysBeep(5); return; }
	long n = build_report(buf);
	ParamBlock pb;
	short e;
	memset(&pb, 0, sizeof pb); pb.ioNamePtr = rep.fName; pb.ioVRefNum = rep.vRefNum;
	e = mac_PBCreate(&pb);
	if (e == dupFNErr) e = 0;                      /* Standard File has asked about replacing it */
	if (!e) {
		memset(&pb, 0, sizeof pb); pb.ioNamePtr = rep.fName; pb.ioVRefNum = rep.vRefNum;
		e = mac_PBGetFInfo(&pb);
		pb.u.f.fdType = TYPE('T', 'E', 'X', 'T'); pb.u.f.fdCreator = TYPE('t', 't', 'x', 't');
		if (!e) e = mac_PBSetFInfo(&pb);
	}
	if (!e) { memset(&pb, 0, sizeof pb); pb.ioNamePtr = rep.fName; pb.ioVRefNum = rep.vRefNum; pb.ioPermssn = fsWrPerm; e = mac_PBOpen(&pb); }
	if (!e) {
		pb.u.io.ioBuffer = buf; pb.u.io.ioReqCount = n; pb.u.io.ioPosMode = fsFromStart; pb.u.io.ioPosOffset = 0;
		e = mac_PBWrite(&pb);
		pb.u.io.ioMisc = n; mac_PBSetEOF(&pb);
		mac_PBClose(&pb);
		memset(&pb, 0, sizeof pb); pb.ioVRefNum = rep.vRefNum; mac_PBFlushVol(&pb);
	}
	mac_disposeptr(buf);
	if (e) pd_alert("The report could not be saved (the disk may be full or locked).", "OK", 0, 2);
	else pd_log(w, "PiDisplay: report saved");
}

/* ---- the window's place: the preferences file ---- */
#define PREFS_MAGIC TYPE('P', 'i', 'D', 'p')
struct prefs { long magic; short version; Rect place; };
static struct prefs pf;
static int prefs_where(short *vref, long *dir)
{
	long fold = 0;
	if (!mac_gestalt(TYPE('f', 'o', 'l', 'd'), &fold) && (fold & 1) &&
	    !mac_findfolder(-32768, TYPE('p', 'r', 'e', 'f'), 1, vref, dir)) return 1;   /* System 7: the Preferences folder */
	short env[8];
	memset(env, 0, sizeof env);
	if (mac_sysenvirons(env)) return 0;
	*vref = env[7]; *dir = 0;                      /* System 6: the System Folder's working directory (sysVRefNum) */
	return 1;
}
static void prefs_io(int write)
{
	short vref; long dir;
	ParamBlock pb;
	if (!prefs_where(&vref, &dir)) return;
	pstr(pbuf, "PiDisplay Prefs");
	memset(&pb, 0, sizeof pb); pb.ioNamePtr = pbuf; pb.ioVRefNum = vref; PB_DIRID(&pb) = dir;
	if (write) {
		short e = mac_PBHCreate(&pb);
		if (e && e != dupFNErr) return;
		memset(&pb, 0, sizeof pb); pb.ioNamePtr = pbuf; pb.ioVRefNum = vref; PB_DIRID(&pb) = dir;
		if (!mac_PBHGetFInfo(&pb)) { pb.u.f.fdType = TYPE('p', 'r', 'e', 'f'); pb.u.f.fdCreator = TYPE('P', 'I', 'D', 'Y'); PB_DIRID(&pb) = dir; mac_PBHSetFInfo(&pb); }
	}
	memset(&pb, 0, sizeof pb); pb.ioNamePtr = pbuf; pb.ioVRefNum = vref; PB_DIRID(&pb) = dir; pb.ioPermssn = write ? fsWrPerm : 1;
	if (mac_PBHOpen(&pb)) return;
	pb.u.io.ioBuffer = &pf; pb.u.io.ioReqCount = sizeof pf; pb.u.io.ioPosMode = fsFromStart; pb.u.io.ioPosOffset = 0;
	if (write) { mac_PBWrite(&pb); pb.u.io.ioMisc = sizeof pf; mac_PBSetEOF(&pb); }
	else if (mac_PBRead(&pb) || pb.u.io.ioActCount != sizeof pf) pf.magic = 0;
	mac_PBClose(&pb);
	if (write) { memset(&pb, 0, sizeof pb); pb.ioVRefNum = vref; mac_PBFlushVol(&pb); }
}
/* the window's content rectangle on the screen, for the preferences and for zooming */
static Rect global_rect(void)
{
	Rect r = *PORT_RECT(win);
	Point a = { r.top, r.left }, b = { r.bottom, r.right };
	SetPort(win);
	LocalToGlobal(&a); LocalToGlobal(&b);
	r.top = a.v; r.left = a.h; r.bottom = b.v; r.right = b.h;
	return r;
}
static void save_place(void)
{
	pf.magic = PREFS_MAGIC; pf.version = 1; pf.place = global_rect();
	prefs_io(1);
}
/* a saved place is used only if its title bar is on a screen that is there now (a second monitor
   taken away, a smaller one put in its place): 40 pixels of it, wholly below a screen's top */
static int place_ok(const Rect *r)
{
	char **gd = LM_DEVICELIST;
	int n;
	for (n = 0; gd && n < 8; n++, gd = GD_NEXT(gd)) {
		Rect s = *GD_RECT(gd), bar = { (short)(r->top - 18), r->left, r->top, r->right }, x;
		if (!(GD_FLAGS(gd) & 0x8000)) continue;                /* not an active screen */
		if (gd == LM_MAINDEVICE) s.top += 20;                  /* the menu bar */
		if (SectRect(&bar, &s, &x) && x.right - x.left >= 40 && x.top == bar.top && r->right - r->left >= 300 && r->bottom - r->top >= 180) return 1;
	}
	return 0;
}
/* the screen with most of the window on it */
static Rect window_screen(void)
{
	Rect g = global_rect(), best = { 0, 0, 342, 512 }, x;
	long most = -1;
	char **gd = LM_DEVICELIST;
	int n;
	if (LM_MAINDEVICE) best = *GD_RECT(LM_MAINDEVICE);
	for (n = 0; gd && n < 8; n++, gd = GD_NEXT(gd)) {
		if (!(GD_FLAGS(gd) & 0x8000)) continue;
		Rect s = *GD_RECT(gd);
		long a = SectRect(&g, &s, &x) ? (long)(x.right - x.left) * (x.bottom - x.top) : 0;
		if (a > most) { most = a; best = s; if (gd == LM_MAINDEVICE) best.top += 20; }
	}
	return best;
}

/* ---- applying ---- */
static void watch(int on)
{
	static void **h;
	if (on) { if (!h) h = (void **)GetCursor(4); if (h) SetCursor(*h); }
	else InitCursor();
}
static void apply(void)
{
	char msg[300], *p;
	unsigned long r = 0;
	long st;
	int mode = changed_mode();
	if (!have || !(changed_mode() || changed_rot())) return;
	if (mode && !(di->cfg & PSMB_CF_INCLUDE) && (di->cfg & PSMB_CF_CONFIG)) {
		pd_alert("config.txt has no line \"include aurora68-display.txt\". Add that line at the end of config.txt (on the SD card, from another computer), restart, and choose the resolution again.", "OK", 0, 2);
		return;
	}
	if (mode) {
		p = pd_put(msg, "Restart to use "); p = size_str(p, sel); p = pd_put(p, " at "); p = pd_hz(p, refresh_of(sel)); p = pd_put(p, " Hz");
		if (rot_sel != di->rotation) { p = pd_put(p, ", turned "); p = pd_num(p, (unsigned long)rot_sel); p = pd_put(p, "\xA1"); }
		p = pd_put(p, "? Programs are asked to quit, then the Pi and the Macintosh restart. If the monitor shows nothing, the previous resolution comes back after 15 seconds.");
	} else {
		p = pd_put(msg, "Restart the Macintosh to turn the HDMI screen to "); p = pd_num(p, (unsigned long)rot_sel);
		p = pd_put(p, "\xA1? Programs are asked to quit first. The Pi keeps running; PiScrn arranges the screens again, which takes one more restart.");
	}
	if (pd_alert(msg, "Restart", "Cancel", 2) != 1) return;
	SetPort(win);
	busy = 1; set_note("Writing to the SD card\xC9"); fix_buttons(); draw_strip(); watch(1);
	st = mode ? pd_info(w, PSMB_INFO_DISPSET, (unsigned long)sel, (unsigned long)uses_alt(sel), (unsigned long)rot_sel, 0, 0, &r)
	          : pd_info(w, PSMB_INFO_DISPROTATE, (unsigned long)rot_sel, 0, 0, 0, 0, &r);
	unsigned long until = LM_TICKS + 600;          /* ten seconds for the card */
	int state = st ? 3 : 1;
	while (!st && state == 1 && LM_TICKS < until) {
		EventRecord ev;
		WaitNextEvent(0, &ev, 10, 0);                  /* null events only: time for the others */
		if (pd_fetch(w, di_new)) state = (int)di_new->apply_state;
	}
	watch(0);
	if (state != 2) {
		busy = 0;
		p = pd_put(msg, "The choice could not be written to the SD card");
		if (st == PSMB_E_RO) p = pd_put(p, ": the card cannot be written");
		else if (have && di_new->message[0]) { p = pd_put(p, ": "); p = pd_put(p, di_new->message); }
		pd_put(p, ". Nothing has changed.");
		pd_alert(msg, "OK", 0, 0);
		refresh(0); strip_note(); fix_buttons(); redraw_all();
		return;
	}
	if (mode) pd_info(w, PSMB_INFO_DISPRESTART, 0, 0, 0, 0, 0, 0);
	pd_log(w, mode ? "PiDisplay: a new resolution written; restarting the Pi and the Mac" : "PiDisplay: a new rotation written; restarting the Mac");
	set_note("Restarting\xC9"); draw_strip();
	save_place();
	mac_restart();
	/* still here: a program refused to quit. The Pi restarts with the Mac's next restart, whenever that is */
	busy = 0;
	set_note(mode ? "The Pi restarts at the Macintosh's next restart." : "The rotation takes effect at the next restart.");
	refresh(0); fix_buttons(); redraw_all();
}
static void revert(void)
{
	if (!have) return;
	sel = cur; rot_sel = di->rotation_next;
	strip_note(); fix_buttons(); redraw_all();
}
static void use_suggested(void)
{
	if (!have || di->suggested == 0xFFFF) return;
	sel = di->suggested;
	int rows = list_rows ? list_rows : 1;
	if (sel < ltop || sel >= ltop + rows) ltop = sel - rows / 2 < 0 ? 0 : sel - rows / 2;
	layout(); strip_note(); fix_buttons(); redraw_all();
}

/* ---- menus ---- */
static void setup_menus(void)
{
	unsigned char t[2] = { 1, 0x14 };              /* the apple */
	m_apple = NewMenu(M_APPLE, t);
	AppendMenu(m_apple, pstr(pbuf, "About PiDisplay\xC9;(-"));
	AddResMenu(m_apple, TYPE('D', 'R', 'V', 'R'));
	InsertMenu(m_apple, 0);
	m_file = NewMenu(M_FILE, pstr(pbuf, "File"));
	AppendMenu(m_file, pstr(pbuf, "Close/W;Save Report As\xC9/S;(-;Quit/Q"));
	InsertMenu(m_file, 0);
	m_edit = NewMenu(M_EDIT, pstr(pbuf, "Edit"));
	AppendMenu(m_edit, pstr(pbuf, "Undo/Z;(-;Cut/X;Copy/C;Paste/V;Clear;(-;Select All/A"));
	InsertMenu(m_edit, 0);
	m_display = NewMenu(M_DISPLAY, pstr(pbuf, "Display"));
	AppendMenu(m_display, pstr(pbuf, "Use Suggested Resolution/U;Revert/R;(-;Restart to Apply\xC9"));
	InsertMenu(m_display, 0);
	DrawMenuBar();
}
static void enable(void *m, int item, int on) { if (on) EnableItem(m, item); else DisableItem(m, item); }
static void fix_menus(void)
{
	int da = FrontWindow() != win;
	enable(m_file, F_CLOSE, 1);
	enable(m_file, F_SAVE, !da);
	enable(m_edit, E_UNDO, da); enable(m_edit, E_CUT, da); enable(m_edit, E_PASTE, da); enable(m_edit, E_CLEAR, da);
	enable(m_edit, E_COPY, 1); enable(m_edit, E_SELALL, da);
	enable(m_display, 0, !da && have);
	enable(m_display, D_SUGGEST, !da && have && !busy && di->suggested != 0xFFFF && di->suggested != sel);
	enable(m_display, D_REVERT, !da && have && !busy && (changed_mode() || changed_rot()));
	enable(m_display, D_APPLY, !da && have && !busy && (di->cfg & PSMB_CF_WRITABLE) && (changed_mode() || changed_rot()));
	DrawMenuBar();
}
static void about(void)
{
	pd_alert("PiDisplay 1.0, for the Aurora68 card's HDMI screen: what the monitor says about itself, which of its resolutions a Pi 3 can send, and the screen's resolution and rotation. MPL-2.0, 2026.", "OK", 0, 1);
	SetPort(win);
}
static void do_menu(long mr)
{
	int id = (int)(mr >> 16), item = (int)(mr & 0xFFFF);
	if (id == M_APPLE) {
		if (item == 1) about();
		else { GetItem(m_apple, item, pbuf); OpenDeskAcc(pbuf); SetPort(win); }
	} else if (id == M_FILE) {
		if (item == F_CLOSE) { if (FrontWindow() != win) SystemEdit(-1); else quit = 1; }   /* the one window: closing it ends PiDisplay */
		else if (item == F_SAVE) save_report();
		else if (item == F_QUIT) quit = 1;
	} else if (id == M_EDIT) {
		if (item <= E_CLEAR && SystemEdit(item - 1)) { HiliteMenu(0); return; }      /* a desk accessory's */
		if (item == E_COPY) copy_report();
	} else if (id == M_DISPLAY) {
		if (item == D_SUGGEST) use_suggested();
		else if (item == D_REVERT) revert();
		else if (item == D_APPLY) apply();
	}
	HiliteMenu(0);
}

/* ---- events ---- */
static void scroll_doc(int nx, int ny)
{
	int mx = GetCtlMax(hbar), my = GetCtlMax(vbar);
	if (nx > mx) nx = mx;
	if (nx < 0) nx = 0;
	if (ny > my) ny = my;
	if (ny < 0) ny = 0;
	if (nx == sx && ny == sy) return;
	sx = nx; sy = ny;
	SetCtlValue(hbar, sx); SetCtlValue(vbar, sy);
	SetPort(win);
	layout();
	draw_content();
}
static void scroll_list(int n)
{
	int m = GetCtlMax(lbar);
	if (n > m) n = m;
	if (n < 0) n = 0;
	if (n == ltop) return;
	ltop = n; SetCtlValue(lbar, ltop);
	draw_list();
}
void c_ctl_action(void *ctl, int part)
{
	Rect c = content();
	int ch = c.bottom - c.top - LINE_H, cw = c.right - c.left - 16;
	if (ctl == lbar) {
		int page = list_rows > 1 ? list_rows - 1 : 1;
		switch (part) {
		case inUpButton: scroll_list(ltop - 1); break;
		case inDownButton: scroll_list(ltop + 1); break;
		case inPageUp: scroll_list(ltop - page); break;
		case inPageDown: scroll_list(ltop + page); break;
		}
		return;
	}
	int v = ctl == vbar, d = 0;
	switch (part) {
	case inUpButton: d = v ? -LINE_H : -16; break;
	case inDownButton: d = v ? LINE_H : 16; break;
	case inPageUp: d = v ? -ch : -cw; break;
	case inPageDown: d = v ? ch : cw; break;
	}
	if (v) scroll_doc(sx, sy + d); else scroll_doc(sx + d, sy);
}
static void choose(int k)
{
	if (k == sel) return;
	if (!selectable(k)) { SysBeep(5); return; }
	int was = sel;
	sel = k;
	if (was >= ltop && was < ltop + list_rows) draw_row(was);
	draw_row(sel);
	strip_note(); draw_strip(); fix_buttons();
}
static void content_click(EventRecord *ev)
{
	Point p = ev->where;
	void *ctl = 0;
	int i;
	SetPort(win);
	GlobalToLocal(&p);
	int part = FindControl(pt_long(p), win, &ctl);
	if (ctl == vbar || ctl == hbar || ctl == lbar) {
		if (part == inThumb) {
			TrackControl(ctl, pt_long(p), 0);
			if (ctl == lbar) scroll_list(GetCtlValue(lbar));
			else scroll_doc(GetCtlValue(hbar), GetCtlValue(vbar));
		} else if (part) TrackControl(ctl, pt_long(p), (void *)pas_ctl_action);
		return;
	}
	if (ctl && part) {
		if (!TrackControl(ctl, pt_long(p), 0)) return;
		if (ctl == b_apply) apply();
		else if (ctl == b_revert) revert();
		for (i = 0; i < 4; i++) if (ctl == radio[i]) { rot_sel = i * 90; strip_note(); draw_strip(); fix_buttons(); }
		return;
	}
	if (!have) return;
	Rect c = content();
	if (!PtInRect(pt_long(p), &c)) return;
	for (i = ltop; i < ltop + list_rows && i < di->nmodes; i++) {
		Rect r = row_rect(i);
		if (PtInRect(pt_long(p), &r)) { choose(i); return; }
	}
}
static Rect std_state(void)
{
	/* the zoomed size: as large as the document wants, on the screen the window is mostly on */
	Rect s = window_screen(), r;
	int want_w = 760 + SBAR, want_h = doc_h + STRIP + SBAR;
	if (want_h < 300) want_h = 300;
	r.top = (short)(s.top + 22); r.left = (short)(s.left + 4);
	r.bottom = (short)(s.bottom - 4); r.right = (short)(s.right - 4);
	if (r.right - r.left > want_w) r.right = (short)(r.left + want_w);
	if (r.bottom - r.top > want_h) r.bottom = (short)(r.top + want_h);
	return r;
}
static void resized(void)
{
	SetPort(win);
	EraseRect(PORT_RECT(win));
	InvalRect(PORT_RECT(win));
	layout();
}
static void mouse_down(EventRecord *ev)
{
	void *ww = 0;
	int part = FindWindow(pt_long(ev->where), &ww);
	switch (part) {
	case inMenuBar: fix_menus(); do_menu(MenuSelect(pt_long(ev->where))); break;
	case inSysWindow: SystemClick(ev, ww); break;
	case inDrag: DragWindow(ww, pt_long(ev->where), RGN_BBOX(LM_GRAYRGN)); break;
	case inGoAway: if (ww == win && TrackGoAway(ww, pt_long(ev->where))) quit = 1; break;
	case inGrow:
		if (ww == win) {
			Rect lim = { 180, 320, 4000, 4000 };   /* top, left: the smallest height and width */
			long sz = GrowWindow(ww, pt_long(ev->where), &lim);
			if (sz) { SizeWindow(ww, (short)(sz & 0xFFFF), (short)(sz >> 16), 1); resized(); }
		}
		break;
	case inZoomIn:
	case inZoomOut:
		if (ww == win && TrackBox(ww, pt_long(ev->where), part)) {
			/* WStateData (the window's dataHandle, at 130 in the record): the user state, then the standard state */
			Rect **ws = *(Rect ***)((char *)win + 130);
			if (ws && *ws) (*ws)[1] = std_state();
			SetPort(win);
			EraseRect(PORT_RECT(win));
			ZoomWindow(ww, part, 0);
			resized();
		}
		break;
	case inContent:
		if (ww != FrontWindow()) SelectWindow(ww);
		else content_click(ev);
		break;
	}
}
static void next_selectable(int dir)
{
	int k = sel < 0 ? (dir > 0 ? -1 : di->nmodes) : sel;
	for (k += dir; k >= 0 && k < di->nmodes; k += dir)
		if (selectable(k)) {
			if (k < ltop) scroll_list(k);
			else if (k >= ltop + list_rows) scroll_list(k - list_rows + 1);
			choose(k);
			return;
		}
	SysBeep(5);
}
static void key_down(EventRecord *ev)
{
	int c = (int)(ev->message & charCodeMask);
	if (ev->modifiers & cmdKey) { fix_menus(); do_menu(MenuKey(c)); return; }
	SetPort(win);
	Rect r = content();
	int page = r.bottom - r.top - LINE_H;
	switch (c) {
	case 0x1E: if (have) next_selectable(-1); break;          /* up arrow: the list */
	case 0x1F: if (have) next_selectable(1); break;           /* down arrow */
	case 0x0B: scroll_doc(sx, sy - page); break;             /* page up: the window */
	case 0x0C: scroll_doc(sx, sy + page); break;             /* page down */
	case 0x01: scroll_doc(0, 0); break;                      /* home */
	case 0x04: scroll_doc(0, 32767); break;                  /* end */
	default: SysBeep(1); break;
	}
}
static void update(void *ww)
{
	if (ww != win) return;
	SetPort(win);
	BeginUpdate(win);
	layout();
	draw_content();
	draw_strip();
	DrawControls(win);
	DrawGrowIcon(win);
	EndUpdate(win);
}
static void activate(int on)
{
	SetPort(win);
	active = on;
	HiliteControl(vbar, on ? 0 : 255); HiliteControl(hbar, on ? 0 : 255); HiliteControl(lbar, on ? 0 : 255);
	fix_buttons();
	if (have) draw_list();
	DrawGrowIcon(win);
}

int main(void)
{
	EventRecord ev;
	unsigned long last = 0;
	int i;
	setup_menus();
	di = mac_newptr(sizeof *di); di_new = mac_newptr(sizeof *di);
	w = pd_window();
	if (!di || !di_new) { SysBeep(10); return 0; }
	if (!w) why_not = "There is no Aurora68 card in this Macintosh: PiDisplay shows the HDMI screen of the Pi in a PiStorm.";
	refresh(1);
	if (!have) build_text();
	/* the window: where it was last time, if that is on a screen now, else the main screen's top left
	   (the SE's 512 x 342 fits it) */
	Rect b = { 42, 6, 334, 506 };
	prefs_io(0);
	if (pf.magic == PREFS_MAGIC && pf.version == 1 && place_ok(&pf.place)) b = pf.place;
	win = NewWindow(0, &b, pstr(pbuf, "PiDisplay"), 1, zoomDocProc, (void *)-1, 1, 0);
	SetPort(win);
	Rect z = { 0, 0, 10, 10 };
	vbar = NewControl(win, &z, pstr(pbuf, ""), 1, 0, 0, 0, scrollBarProc, 0);
	hbar = NewControl(win, &z, pstr(pbuf, ""), 1, 0, 0, 0, scrollBarProc, 0);
	lbar = NewControl(win, &z, pstr(pbuf, ""), 0, 0, 0, 0, scrollBarProc, 0);
	b_apply = NewControl(win, &z, pstr(pbuf, "Restart to Apply\xC9"), 1, 0, 0, 1, pushButProc, 0);
	b_revert = NewControl(win, &z, pstr(pbuf, "Revert"), 1, 0, 0, 1, pushButProc, 0);
	{ static const char *const rn[4] = { "0\xA1", "90\xA1", "180\xA1", "270\xA1" };
	  for (i = 0; i < 4; i++) radio[i] = NewControl(win, &z, pstr(pbuf, rn[i]), 0, 0, 0, 1, radioButProc, i); }
	if (have) {
		char t[100], *p = pd_put(t, "PiDisplay: ");
		p = pd_put(p, di->name[0] ? di->name : "no monitor description"); p = pd_put(p, ", ");
		p = pd_num(p, di->cur_w); p = pd_put(p, "x"); p = pd_num(p, di->cur_h); p = pd_put(p, ", ");
		p = pd_num(p, di->nmodes); pd_put(p, " modes");
		pd_log(w, t);
		/* started while a new mode waits to be kept (no PiScrn to ask at startup): ask here */
		if (di->confirm_state == PSMB_CS_PENDING) { pd_confirm(w, di); SetPort(win); refresh(0); }
	}
	layout();
	fix_buttons();
	while (!quit) {
		if (WaitNextEvent(everyEvent, &ev, fg ? 20 : 60, 0)) {
			switch (ev.what) {
			case mouseDown: mouse_down(&ev); break;
			case keyDown: case autoKey: key_down(&ev); break;
			case updateEvt: update((void *)ev.message); break;
			case activateEvt: if ((void *)ev.message == win) activate(ev.modifiers & activeFlag); break;
			case osEvt:
				if (((unsigned long)ev.message >> 24) == suspendResumeMessage) { fg = ev.message & resumeFlag; activate(fg); }
				break;
			}
		}
		/* the kernel's answer every two seconds: a mode kept or reverted elsewhere, the copy's time */
		if (LM_TICKS - last >= 120) {
			last = LM_TICKS;
			if (refresh(0)) {
				/* the content below the top strip only: the strip's note is drawn in place and its
				   buttons are not erased, so nothing flickers when the answer changes */
				Rect below = *PORT_RECT(win);
				below.top = STRIP;
				SetPort(win); layout(); fix_buttons(); draw_strip(); InvalRect(&below);
			}
		}
	}
	save_place();
	return 0;
}
