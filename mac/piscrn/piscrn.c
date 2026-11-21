/* mac/piscrn/piscrn.c - "PiScrn": the SE board's two screens, both active from the first boot. MPL-2.0.
 *
 * Backstory. On the SE board the Pi presents two video cards, slot 9 with the SE's own 512 x 342
 * screen and slot $A with the HDMI screen (docs/10, "Two screens on the SE"). The IIci ROM makes a
 * GDevice for each, but QuickDraw activates a second screen only when the System file's 'scrn'
 * resource (the arrangement the Monitors control panel saves) names every screen and matches the
 * hardware; without one it
 * keeps the startup screen alone and the HDMI screen stays the driver's grey (docs/08 item 26, which
 * also has the resource's format, read out of the ROM's loader at 0x40827BA0). Monitors writes one
 * as soon as a screen is dragged and OK is clicked, but on a Mac whose only screen at boot is a
 * 9-inch one that is a lot to ask of someone who has just plugged in an HDMI monitor.
 *
 * So this extension, at startup: lists the video sResources the way the ROM's loader does
 * (sNextTypeSRsrc, category 3, type 1, software 1), looks for the Pi's HDMI screen among them
 * (drHw 'PH', 0x5048), and checks the System file's 'scrn' 0 by the loader's own rules: a record per
 * screen in that order, each with the screen's hardware id, slot and dCtlDevBase, and a rectangle of
 * the screen's size. If it matches, Monitors' arrangement (or an earlier one of ours) is kept and
 * nothing happens. If not (no resource, a different HDMI size, a card that changed), it writes one:
 * the first screen (the SE's) main at the origin, each further one to its right with its tops
 * aligned, the HDMI screen in its deepest mode the addressing mode can reach (millions of colours
 * with 32-bit addressing, else the deepest indexed mode in the slot's 24-bit megabyte), and restarts
 * the Mac once, since the loader runs at InitGraf, before any extension. Should the restarted boot
 * still not take it (the same bytes are there already and still do not match), it says so in the
 * Pi's log and does nothing more, rather than restart for ever.
 *
 * Nothing on any other Mac: without the Pi's mailbox window, or without an HDMI screen of the Pi's
 * (mac.hdmi=mirror or log, the solo board, the IIci testbed), it returns at once. Written for the
 * first SE bring-up, 20 November 2026. Built with the C kit in mac/c (mac/c/README.md).
 *
 * The keep-or-revert question (21 November 2026, PiDisplay, docs/10). After PiDisplay has chosen a
 * new HDMI mode, the Pi restarts with it and nobody knows yet whether the monitor shows it. PiScrn
 * runs at every startup of such a Mac, so it is the one that asks: when the kernel says the mode
 * waits to be confirmed, PiScrn stays in the system heap (entry.s copies it there; otherwise the
 * copy goes again) and puts a filter on GetNextEvent (jGNEFilter, as the WiFi menu does); at the
 * first null event of the first program, two seconds on, the filter asks "Keep this resolution?"
 * with its countdown (mac/pidisplay/pdcommon.c), on the main screen, which on the SE board is the
 * SE's own. The filter then does nothing more. Asked on any board, the solo one included, where
 * the question shares the screen whose mode changed and the countdown is what helps.
 */
#include "toolbox.h"
#include "pdcommon.h"

#define INFO_CMD          0x1000UL              /* the INFO service's command block (psmb.h) */
#define DISP2             0x59000UL             /* the HDMI screen's display block (psmb.h, PSMB_OFF_DISPLAY2) */
#define DISP_MAGIC        0x41363856UL          /* 'A68V' */
#define DISP_MODE_BASE    1088                  /* 8 longs: the 68k address of mode 128+i's pixels, 0 = no such mode */
#define HW_SE_SCREEN      0x5056                /* drHw of the card's first screen */
#define HW_HDMI           0x5048                /* drHw of the HDMI screen, 'PH' */
#define MAX_SCREENS       4

#undef LM_DEVICELIST
#define LM_DEVICELIST     (*(char **volatile *)0x8A8)            /* the first GDevice (a handle) */
#define LM_UTABLEBASE     (*(char ***volatile *)0x11C)           /* the unit table: a DCE handle per unit */
#define LM_UNITNTRYCNT    (*(volatile short *)0x1D2)
#define LM_SYSMAP         (*(volatile short *)0xA58)

void restart_mac(void);                         /* entry.s */
short slot_next_video(void *spblock);
void *new_handle_sys(long size);
void dispose_handle(void *h);
unsigned short get_sr(void);
void set_sr(unsigned short sr);

/* the Slot Manager's parameter block (Inside Macintosh V, "The Slot Manager"); the Toolbox aligns
   its fields to two bytes, as m68k GCC does */
typedef struct {
	long spResult;
	void *spsPointer;
	long spSize, spOffsetData;
	void *spIOFileName, *spsExecPBlk;
	long spParamData, spMisc, spReserved;
	short spIOReserved, spRefNum, spCategory, spCType, spDrvrSW, spDrvrHW;
	unsigned char spTBMask, spSlot, spID, spExtDev, spHWDev, spByteLanes, spFlags, spKey;
} SpBlock;
_Static_assert(__builtin_offsetof(SpBlock, spRefNum) == 38 && __builtin_offsetof(SpBlock, spSlot) == 49 && sizeof(SpBlock) == 56, "SpBlock");

/* one screen record of a 'scrn' resource (tools/mac/mkscrn.py has the rules) */
typedef struct {
	unsigned short hw, slot;
	unsigned long base;
	short mode, mask, flags, clut, gamma;
	Rect rect;
	short controls;                             /* then per control: csCode, length, data */
} ScrnRec;
_Static_assert(sizeof(ScrnRec) == 28, "ScrnRec");

/* a video sResource and what the loader compares it with */
struct screen {
	unsigned short hw;
	unsigned char slot;
	unsigned long base;                         /* the driver's dCtlDevBase */
	short width, height;                        /* its GDevice's pixel map */
	short mode;                                 /* the mode it is in */
};

static volatile unsigned char *win(void) { return (volatile unsigned char *)(LM_MMU32BIT ? 0xF9000000UL : 0x00900000UL); }
static unsigned long rd32(unsigned long off) { return *(volatile unsigned long *)(win() + off); }
static void wr32(unsigned long off, unsigned long v) { *(volatile unsigned long *)(win() + off) = v; }

/* a line in the Pi's log through the INFO service, as the drivers write theirs ([MAC] PiScrn: ...) */
static char line[128];
static void pi_log(const char *text)
{
	static const char who[] = "A68 Screen Setup: ";
	unsigned long n = 0, k, req;
	for (k = 0; who[k]; k++) line[n++] = who[k];
	for (k = 0; text[k] && n < sizeof line - 1; k++) line[n++] = text[k];
	unsigned short sr = get_sr();
	set_sr(sr | 0x0700);
	req = rd32(INFO_CMD);
	for (k = 0; rd32(INFO_CMD + 4) != req && k < 2000000; k++) ;
	*(volatile unsigned short *)(win() + INFO_CMD + 8) = 2;       /* PSMB_INFO_LOG */
	wr32(INFO_CMD + 28, (unsigned long)line);
	wr32(INFO_CMD + 32, n);
	wr32(INFO_CMD, req + 1);
	for (k = 0; rd32(INFO_CMD + 4) != req + 1 && k < 4000000; k++) ;
	set_sr(sr);
}

/* the GDevice whose driver is refnum: its pixel map's size and its mode */
static int device_of(short refnum, struct screen *s)
{
	char **gdh = LM_DEVICELIST;
	for (int n = 0; gdh && n < 8; n++) {
		char *gd = *gdh;
		if (*(short *)gd == refnum) {                         /* gdRefNum */
			char *pm = **(char ***)(gd + 22);                 /* gdPMap -> the PixMap */
			Rect *b = (Rect *)(pm + 6);                       /* its bounds */
			s->width = b->right - b->left;
			s->height = b->bottom - b->top;
			s->mode = (short)*(long *)(gd + 42);              /* gdMode */
			return 1;
		}
		gdh = *(char ***)(gd + 30);                           /* gdNextGD */
	}
	return 0;
}

/* the video sResources in the loader's order, with their drivers' dCtlDevBase and their devices */
static int list_screens(struct screen *s)
{
	SpBlock sp;
	unsigned char *z = (unsigned char *)&sp;
	for (unsigned i = 0; i < sizeof sp; i++) z[i] = 0;
	sp.spCategory = 3; sp.spCType = 1; sp.spDrvrSW = 1;
	sp.spTBMask = 1;                                          /* any hardware id */
	int n = 0;
	while (n < MAX_SCREENS && slot_next_video(&sp) == 0) {
		struct screen *x = &s[n];
		x->hw = (unsigned short)sp.spDrvrHW; x->slot = sp.spSlot; x->base = 0;
		x->width = x->height = 0; x->mode = 128;
		short unit = (short)~sp.spRefNum;
		if (sp.spRefNum < 0 && unit < LM_UNITNTRYCNT) {
			char **dce = LM_UTABLEBASE[unit];
			if (dce) x->base = *(unsigned long *)(*dce + 42);    /* dCtlDevBase */
		}
		if (!device_of(sp.spRefNum, x)) return -1;            /* a screen without a GDevice: leave it all alone */
		n++;
	}
	return n;
}

/* does the 'scrn' at p, len bytes, pass the loader's comparison with these screens? */
static int scrn_matches(const unsigned char *p, long len, const struct screen *s, int n)
{
	if (len < 2 || *(const unsigned short *)p != n) return 0;
	long off = 2;
	for (int i = 0; i < n; i++) {
		if (off + (long)sizeof(ScrnRec) > len) return 0;
		const ScrnRec *r = (const ScrnRec *)(p + off);
		if (r->hw != s[i].hw || (r->slot & 0xFF) != s[i].slot || r->base != s[i].base) return 0;
		if (r->rect.bottom - r->rect.top != s[i].height || r->rect.right - r->rect.left != s[i].width) return 0;
		off += sizeof(ScrnRec);
		for (int c = 0; c < r->controls; c++) {               /* csCode, length, the data (kept even) */
			if (off + 4 > len) return 0;
			off += 4 + ((*(const unsigned short *)(p + off + 2) + 1) & ~1);
		}
	}
	return off <= len;
}

/* the HDMI screen's deepest mode whose pixels this addressing mode reaches: all of them with 32-bit
   addressing, else those in the slot's 24-bit megabyte ($F99xxxxx, the window), not the big region */
static short best_mode(void)
{
	if (rd32(DISP2) != DISP_MAGIC) return 128;
	for (int i = 5; i > 0; i--) {
		unsigned long base = rd32(DISP2 + DISP_MODE_BASE + 4 * (unsigned long)i);
		if (base && (LM_MMU32BIT || (base >> 20) == 0xF99)) return (short)(128 + i);
	}
	return 128;
}

static unsigned char want[2 + MAX_SCREENS * sizeof(ScrnRec)];

/* ---- the keep-or-revert question: a filter on GetNextEvent (entry.s gne_filter) ---- */
static struct psmb_dispinfo dinfo;
static int asked;
static unsigned long not_before;
void *old_gne;
#define LM_JGNEFILTER (*(void **volatile *)0x29A)
void gne_filter(void);
int c_filter(EventRecord *ev)
{
	if (asked || ev->what != nullEvent || LM_TICKS < not_before || !LM_CURAPNAME[0]) return 0;
	asked = 1;
	volatile unsigned char *w = pd_window();
	/* PiDisplay, started first, may have asked already, or the kernel given up waiting */
	if (!pd_fetch(w, &dinfo) || dinfo.confirm_state != PSMB_CS_PENDING) return 0;
	void *port;
	GetPort(&port);
	pd_confirm(w, &dinfo);
	SetPort(port);
	return 0;
}
static int wait_confirm(void)
{
	volatile unsigned char *w = pd_window();
	if (!pd_fetch(w, &dinfo) || dinfo.confirm_state != PSMB_CS_PENDING) return 0;
	old_gne = LM_JGNEFILTER;
	LM_JGNEFILTER = (void *)gne_filter;
	not_before = LM_TICKS + 120;
	pi_log("a new HDMI mode waits to be kept: the question comes once the Finder is up");
	return 1;                                                 /* the copy in the system heap stays */
}

static int arrange(void);
int init_main(void)
{
	if (rd32(0) != PSMB_MAGIC) return 0;                      /* not an Aurora68 Mac */
	arrange();                                                /* restarts the Mac if the screens need a new 'scrn' */
	return wait_confirm();
}

static int arrange(void)
{
	struct screen s[MAX_SCREENS];
	int n = list_screens(s), hdmi = 0;
	for (int i = 0; i < n; i++) if (s[i].hw == HW_HDMI) hdmi = 1;
	if (n < 2 || !hdmi) return 0;                             /* one screen (mirror, log, another board): nothing to arrange */

	short old = CurResFile();
	UseResFile(LM_SYSMAP);
	char **have = (char **)Get1Resource(TYPE('s', 'c', 'r', 'n'), 0);
	long have_len = have ? SizeResource(have) : 0;
	if (have && scrn_matches((const unsigned char *)*have, have_len, s, n)) {
		ReleaseResource(have);                                /* Monitors' arrangement, or ours: kept */
		UseResFile(old);
		return 0;
	}

	/* the arrangement: the first screen (the SE's) main at the origin, the others to its right */
	*(unsigned short *)want = (unsigned short)n;
	ScrnRec *r = (ScrnRec *)(want + 2);
	short left = 0;
	for (int i = 0; i < n; i++, r++) {
		r->hw = s[i].hw; r->slot = s[i].slot; r->base = s[i].base;
		r->mode = s[i].hw == HW_HDMI ? best_mode() : s[i].hw == HW_SE_SCREEN ? 128 : s[i].mode;
		r->mask = 0x1001;                                     /* keep allInit and the colour bit (mkscrn.py says why) */
		r->flags = i == 0 ? (short)0x8800 : (short)0x8001;    /* active and main; active and colour */
		r->clut = -1; r->gamma = -1;
		r->rect.top = 0; r->rect.left = left; r->rect.bottom = s[i].height; r->rect.right = left + s[i].width;
		r->controls = 0;
		left += s[i].width;
	}
	long len = 2 + n * (long)sizeof(ScrnRec);
	if (have && have_len == len) {
		int same = 1;
		for (long i = 0; i < len && same; i++) same = (*have)[i] == (char)want[i];
		if (same) {
			ReleaseResource(have);
			UseResFile(old);
			pi_log("the 'scrn' written at the last start did not take; leaving it (arrange the screens in Monitors)");
			return 0;
		}
	}
	if (have) { RmveResource(have); dispose_handle(have); }
	char **h = new_handle_sys(len);
	if (!h) { UseResFile(old); pi_log("no memory for the 'scrn' resource"); return 0; }
	for (long i = 0; i < len; i++) (*h)[i] = (char)want[i];
	AddResource(h, TYPE('s', 'c', 'r', 'n'), 0, (const unsigned char *)"");
	WriteResource(h);
	UpdateResFile(LM_SYSMAP);
	short err = ResError();
	UseResFile(old);
	if (err) { pi_log("could not write the 'scrn' resource into the System file (a locked disk?)"); return 0; }
	pi_log(have ? "the screens changed: a new arrangement in the System file; restarting once"
	            : "two screens: their arrangement written into the System file; restarting once");
	restart_mac();
	return 1;
}
