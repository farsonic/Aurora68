// SPDX-License-Identifier: MPL-2.0
/* mac/pislots/pislots.c - "PiSlots": the NuBus cards the Slot Manager knows about. MPL-2.0.
 *
 * Backstory. On Aurora68 the Macintosh SE runs the IIci ROM on a Pi, and the Pi's two cards (slot 9,
 * the Aurora68 card with its disk, network, input and video functions; slot $A on the SE board, the
 * HDMI screen) exist only in software: a declaration ROM served from Pi RAM (tools/mac/mkdeclrom.py).
 * The Slot Manager treats them like real cards, and the user asked how to see which cards a Mac has.
 * System 7.1 has no Apple System Profiler, and the project gives away nothing it did not write, so
 * this is the project's own (20 November 2026).
 *
 * For each slot, the logic board's own (slot 0) and $9-$E, PiSlots asks the Slot Manager
 * (Inside Macintosh: Devices, "Slot Manager"; Designing Cards and Drivers, 3rd ed.):
 *   - SReadInfo: whether a card is there and started (siInitStatusA) and its primary init's status
 *     (siInitStatusV);
 *   - the board sResource (category 1): its name, board ID, vendor information (vendor, serial
 *     number, revision, part number, date: whatever the card gives) and whether it has a primary init;
 *   - every functional sResource (SNextSRsrc, slot by slot, id by id): id, kind (category, type,
 *     driver software, driver hardware) in words where the numbers are Apple's or Aurora68's own
 *     and in brackets as numbers, its name, the driver name the Slot Manager gives it
 *     (SReadDrvrName), whether that driver is open (a DCE in the unit table whose dCtlSlot and
 *     dCtlSlotId are this sResource's), and for a screen its size and depth from its GDevice.
 * It works on any Mac with a Slot Manager (a IIci or SE/30 with real cards too). A Mac without one
 * (a Plus, an SE or Classic, or an Aurora68 SE that runs the SE's own ROM) gets a sentence saying so.
 *
 * The report is plain text in a window that scrolls, resizes and zooms (to the screen it is on) and
 * comes back where it was left ("PiSlots Prefs"); it can be selected and copied (Edit > Copy, all of
 * it when nothing is selected), saved as a TeachText document (File > Save As...), and redone (File >
 * Look Again, Command-R: a network driver opens only when MacTCP or AppleTalk asks for it). The window
 * section below lists what the Human Interface Guidelines asked of it. On an Aurora68
 * Mac a summary goes into the Pi's log too, through the mailbox's INFO service ("[MAC] PiSlots:
 * ..."), so it shows in the kernel log and in PiLog.
 *
 * The strings are in the code, in English, as in the project's other Mac programs. The window fits
 * the SE's 512 x 342 screen. Built with the C kit in mac/c (mac/c/README.md), 68020 code.
 */
#include "toolbox.h"

/* ---- the Slot Manager ---- */
/* its parameter block (Inside Macintosh V, "The Slot Manager"; fields aligned to two bytes, as m68k
   GCC does; the same layout as mac/piscrn/piscrn.c) */
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

/* what SReadInfo copies out: the slot's sInfo record */
typedef struct {
	void *siDirPtr;
	short siInitStatusA;        /* the Slot Manager's verdict on the card: 0 good, else an error */
	short siInitStatusV;        /* what the card's primary init answered */
	unsigned char siState, siCPUByteLanes, siTopOfROM, siStatusFlags;
	short siTOConst;
	char siReserved[2];
	void *siROMAddr;
	unsigned char siSlot, siPadding[3];
} SInfoRecord;
_Static_assert(sizeof(SInfoRecord) == 24, "SInfoRecord");

/* the _SlotManager ($A06E) selectors used (Slots.h's numbers) */
#define sReadWord      1
#define sGetCString    3
#define sFindStruct    6
#define sReadInfo      16
#define sNextSRsrc     20
#define sReadDrvrName  25
#define smEmptySlot    (-300)
#define smNoMoresRsrcs (-344)

/* sResource entry ids (Designing Cards and Drivers, ROMDefs) */
#define sRsrcName      2
#define sRsrcDrvrDir   4
#define sRsrcBootRec   6
#define sRsrcFlags     7
#define BoardId        32
#define PrimaryInit    34
#define VendorInfo     36
#define catBoard       1
#define catDisplay     3
#define catAurora68    0x7050      /* 'pP': the vendor category Aurora68's card uses (mkdeclrom.py) */
#define BOARD_AURORA68 0x5053      /* 'PS': slot 9's BoardId */

/* register-based OS traps: the parameter block in A0, the selector in D0, the answer in D0. The
   Toolbox may change D0-D2/A0-A1 (D2 is GCC's to keep, so it is named) */
static short slot_mgr(short sel, SpBlock *sp)
{
	register long d0 __asm__("d0") = sel;
	register SpBlock *a0 __asm__("a0") = sp;
	__asm__ volatile(".short 0xA06E" : "+d"(d0), "+a"(a0) : : "d1", "d2", "a1", "memory", "cc");
	return (short)d0;
}
static void dispose_ptr(void *p)
{
	register void *a0 __asm__("a0") = p;
	__asm__ volatile(".short 0xA01F" : "+a"(a0) : : "d0", "d1", "d2", "a1", "memory", "cc");
}
/* NGetTrapAddress for an OS trap ($A346) and for a Toolbox trap ($A746) */
static void *os_trap(short t)
{
	register long d0 __asm__("d0") = t;
	register void *a0 __asm__("a0");
	__asm__ volatile(".short 0xA346" : "+d"(d0), "=a"(a0) : : "d1", "d2", "a1", "memory", "cc");
	return a0;
}
static void *tool_trap(short t)
{
	register long d0 __asm__("d0") = t;
	register void *a0 __asm__("a0");
	__asm__ volatile(".short 0xA746" : "+d"(d0), "=a"(a0) : : "d1", "d2", "a1", "memory", "cc");
	return a0;
}
#define UNIMPLEMENTED 0xA89F
static int has_os_trap(short t) { return os_trap(t) != tool_trap(UNIMPLEMENTED); }
static int has_tool_trap(short t) { return tool_trap(t) != tool_trap(UNIMPLEMENTED); }
/* Gestalt ($A1AD): the selector in D0, the answer in A0, the error in D0 */
static short gestalt(long sel, long *resp)
{
	register long d0 __asm__("d0") = sel;
	register long a0 __asm__("a0");
	__asm__ volatile(".short 0xA1AD" : "+d"(d0), "=a"(a0) : : "d1", "d2", "a1", "memory", "cc");
	*resp = a0;
	return (short)d0;
}

/* low memory (Inside Macintosh, MPW's SysEqu.a) */
#define LM_UTABLEBASE  (*(char ***volatile *)0x11C)   /* the unit table: a DCE handle per unit */
#define LM_UNITNTRYCNT (*(volatile short *)0x1D2)
#define LM_DEVICELIST  (*(char **volatile *)0x8A8)    /* the first GDevice (Color QuickDraw) */
#define LM_MAINDEVICE  (*(char **volatile *)0x8A4)
#define LM_ROM85       (*(volatile unsigned short *)0x28E)   /* bit 14 clear: Color QuickDraw */
/* a master pointer's address: in 24-bit mode the high byte holds the block's flags */
static char *deref(char **h)
{
	unsigned long p = (unsigned long)*h;
	if (!LM_MMU32BIT) p &= 0x00FFFFFFUL;
	return (char *)p;
}

/* ---- the report, as text with Mac line ends ---- */
#define TXT_MAX 30000                  /* TextEdit's limit is 32 KB */
static char *txt;
static long tn;
static void out(const char *s) { while (*s && tn < TXT_MAX) txt[tn++] = *s++; }
static void outc(char c) { if (tn < TXT_MAX) txt[tn++] = c; }
static void nl(void) { outc('\r'); }
static void out_num(long v)
{
	char b[12];
	int i = 0;
	unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
	if (v < 0) outc('-');
	do { b[i++] = (char)('0' + u % 10); u /= 10; } while (u);
	while (i) outc(b[--i]);
}
static void out_hex(unsigned long v, int digits)
{
	static const char hx[] = "0123456789ABCDEF";
	char b[8];
	int i = 0;
	do { b[i++] = hx[v & 15]; v >>= 4; } while (v || i < digits);
	outc('$');
	while (i) outc(b[--i]);
}
/* slot 9 as "9", slot 10 as "$A", as Apple's documents write them */
static void out_slot(int s) { if (s < 10) out_num(s); else out_hex((unsigned long)s, 1); }
/* a hardware id that is two letters ('PS', 'PV') as well as the number */
static void out_id(unsigned short v)
{
	unsigned char a = (unsigned char)(v >> 8), b = (unsigned char)v;
	out_hex(v, 4);
	if (a >= 'A' && a <= 'z' && b >= 'A' && b <= 'z') { out(" '"); outc((char)a); outc((char)b); outc('\''); }
}

/* the Slot Manager's errors in words, those a card's status can be (Slots.h) */
static const char *slot_err(short e)
{
	switch (e) {
	case -300: return "empty";
	case -301: return "its declaration ROM's checksum is wrong";
	case -302: return "its declaration ROM's format is not one the Mac knows";
	case -303: return "its declaration ROM's revision is not one the Mac knows";
	case -305: return "the slot is switched off";
	case -313: return "its declaration ROM has no board sResource";
	case -315: return "its board sResource has no board ID";
	case -316: return "its start-up code (primary init) reported a failure";
	case -319: return "its board ID is not a valid one";
	case -320: return "it did not answer in time (bus error)";
	case -337: return "this Mac has no such slot";
	}
	return 0;
}

/* a C string of the Slot Manager's (SGetCString) into buf; empty when the entry is not there */
static void read_cstring(void *list, int id, char *buf, int len)
{
	SpBlock sp;
	memset(&sp, 0, sizeof sp);
	buf[0] = 0;
	sp.spsPointer = list; sp.spID = (unsigned char)id;
	if (slot_mgr(sGetCString, &sp) || !sp.spResult) return;
	const char *s = (const char *)sp.spResult;
	int i = 0;
	while (s[i] && i < len - 1) { buf[i] = s[i]; i++; }
	buf[i] = 0;
	dispose_ptr((void *)sp.spResult);
}
/* is entry id in this sResource list (SFindStruct finds any entry, whatever it holds) */
static int has_entry(void *list, int id)
{
	SpBlock sp;
	memset(&sp, 0, sizeof sp);
	sp.spsPointer = list; sp.spID = (unsigned char)id;
	return slot_mgr(sFindStruct, &sp) == 0;
}
static int read_word(void *list, int id, long *v)
{
	SpBlock sp;
	memset(&sp, 0, sizeof sp);
	sp.spsPointer = list; sp.spID = (unsigned char)id;
	if (slot_mgr(sReadWord, &sp)) return 0;
	*v = sp.spResult & 0xFFFF;
	return 1;
}

/* the kind of an sResource in words: Apple's categories and types (ROMDefs), Aurora68's own */
static void out_kind(const SpBlock *sp)
{
	int c = sp->spCategory, t = sp->spCType;
	if (c == catDisplay) out(t == 1 ? "display, video" : t == 2 ? "display, LCD" : "display");
	else if (c == 4) out(t == 1 ? "network, Ethernet" : "network");
	else if (c == 10) {
		/* ROMDefs: type68000 2, type68020 3, type68030 4, type68040 5 (the IIci's own CPU sResource, $B5
		   'CPU_68030_\_MacIIci', is type 4) */
		static const char *const cpu[] = { ", 68000", ", 68020", ", 68030", ", 68040" };
		out("processor");
		if (t >= 2 && t <= 5) out(cpu[t - 2]);
	}
	else if (c == 2) out("test");
	else if (c == 8) out("scanner");
	else if (c == 12) out("internal bus");
	else if (c == 17) out("prototype");
	else if (c == 32) out("dock");
	else if (c == catAurora68 && t == 1) out("disk (Aurora68's own kind)");
	else if (c == catAurora68 && t == 2) out("keyboard and mouse (Aurora68's own kind)");
	else out("a kind of its maker's own");
}
/* the four numbers the kind is made of (sRsrcType), on a line of their own */
static void out_type(const SpBlock *sp)
{
	out("category ");
	if (sp->spCategory > 255) out_id((unsigned short)sp->spCategory); else out_num(sp->spCategory);
	out(", type "); out_num(sp->spCType);
	out(", software "); out_num(sp->spDrvrSW);
	out(", hardware "); out_id((unsigned short)sp->spDrvrHW);
}

/* the unit table entry of this sResource's driver: its refnum, 0 if there is none */
static short find_dce(int slot, int id, int ext, int *open)
{
	char ***ut = LM_UTABLEBASE;
	int n = LM_UNITNTRYCNT;
	for (int u = 0; u < n; u++) {
		char **h = ut[u];
		if (!h || !*h) continue;
		char *dce = deref(h);
		/* dCtlSlot +40, dCtlSlotId +41, dCtlExtDev +50; dCtlFlags +4, dOpened bit 5 */
		if ((unsigned char)dce[40] == slot && (unsigned char)dce[41] == id && (unsigned char)dce[50] == ext) {
			*open = (*(unsigned short *)(dce + 4) & 0x0020) != 0;
			return (short)~u;
		}
	}
	*open = 0;
	return 0;
}

/* a screen driven by driver refnum: its GDevice's size and depth */
static void out_screen(short ref)
{
	if (LM_ROM85 & 0x4000) return;                  /* no Color QuickDraw, no GDevices */
	char **gdh = LM_DEVICELIST;
	for (int n = 0; gdh && n < 16; n++) {
		char *gd = deref(gdh);
		if (*(short *)gd == ref) {                   /* gdRefNum */
			char *pm = deref(*(char ***)(gd + 22));  /* gdPMap */
			Rect *b = (Rect *)(pm + 6);              /* its bounds */
			short depth = *(short *)(pm + 32);       /* pixelSize */
			unsigned short flags = *(unsigned short *)(gd + 20);    /* gdFlags */
			out("      screen:  "); out_num(b->right - b->left); out(" x "); out_num(b->bottom - b->top);
			out(", "); out_num(depth); out(depth == 1 ? " bit (" : " bits (");
			if (depth == 1) out("black and white");
			else if (depth <= 8) { out_num(1L << depth); out((flags & 1) ? " colours" : " greys"); }
			else out(depth == 16 ? "thousands of colours" : "millions of colours");
			out(")");
			if (gdh == LM_MAINDEVICE) out(", the main screen");
			else if (!(flags & 0x8000)) out(", not in use");          /* screenActive */
			nl();
			return;
		}
		gdh = *(char ***)(gd + 30);                  /* gdNextGD */
	}
}

/* ---- the summary for the Pi's log ---- */
static char sum[128], card_line[128];
static int sum_n, card_n;
static void sadd(char *b, int *n, const char *s) { while (*s && *n < 127) b[(*n)++] = *s++; b[*n] = 0; }
static void sadd_slot(char *b, int *n, int s)
{
	char t[4] = { 0 };
	if (s < 10) t[0] = (char)('0' + s);
	else { t[0] = '$'; t[1] = (char)('A' + s - 10); }
	sadd(b, n, t);
}
static int aurora68;                   /* slot 9 holds the Aurora68 card: its mailbox can be used */
static int n_cards;

/* the mailbox's INFO service (emu68/src/psmac/psmb.h): a line for the Pi's log, as PiScrn writes
   its own. The drivers log through the same command block at interrupt time, so interrupts are held
   off meanwhile. The kernel takes 127 characters of a line. */
#define INFO_CMD  0x1000
#define PSMB_MAGIC 0x50534D42UL         /* 'PSMB' at the window's start */
static void pi_log(const char *text)
{
	static char line[128];
	volatile unsigned char *win = (volatile unsigned char *)(LM_MMU32BIT ? 0xF9000000UL : 0x00900000UL);
	volatile unsigned long *req = (volatile unsigned long *)(win + INFO_CMD), *done = req + 1;
	unsigned long r, k;
	int n = 0;
	short sr;
	if (!aurora68 || *(volatile unsigned long *)win != PSMB_MAGIC) return;
	sadd(line, &n, "PiSlots: ");
	sadd(line, &n, text);
	__asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "cc");
	r = *req;
	for (k = 0; *done != r && k < 2000000; k++) { }
	*(volatile unsigned short *)(win + INFO_CMD + 8) = 2;          /* PSMB_INFO_LOG */
	*(volatile unsigned long *)(win + INFO_CMD + 28) = (unsigned long)line;
	*(volatile unsigned long *)(win + INFO_CMD + 32) = (unsigned long)n;
	*req = r + 1;
	for (k = 0; *done != r + 1 && k < 4000000; k++) { }
	__asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "cc");
}

/* ---- one slot ---- */
static void board(int slot, const SpBlock *sp, int status_v)
{
	char s[64];
	long v;
	void *list = sp->spsPointer;
	read_cstring(list, sRsrcName, s, sizeof s);
	out("  Card:        "); out(s[0] ? s : "(no name)"); nl();
	if (slot != 0) {
		sadd(sum, &sum_n, sum_n ? ", slot " : "slot ");
		sadd_slot(sum, &sum_n, slot);
		sadd(sum, &sum_n, " \""); sadd(sum, &sum_n, s); sadd(sum, &sum_n, "\"");
	}
	if (read_word(list, BoardId, &v)) {
		out("  Board ID:    "); out_id((unsigned short)v); nl();
		if (slot == 9 && v == BOARD_AURORA68) aurora68 = 1;
	}
	SpBlock vp;
	memset(&vp, 0, sizeof vp);
	vp.spsPointer = list; vp.spID = VendorInfo;
	if (slot_mgr(sFindStruct, &vp) == 0) {
		static const char *const what[] = { 0, "  Made by:     ", "  Serial:      ", "  Revision:    ", "  Part number: ", "  Date:        " };
		for (int i = 1; i <= 5; i++) {
			read_cstring(vp.spsPointer, i, s, sizeof s);
			if (s[0]) { out(what[i]); out(s); nl(); }
		}
	}
	out("  Start-up:    ");
	if (has_entry(list, PrimaryInit)) { out("its own code (primary init) ran, answer "); out_num(status_v); }
	else { out("no start-up code of its own"); if (status_v) { out(", status "); out_num(status_v); } }
	nl();
}

static void function(int slot, const SpBlock *sp, int nth)
{
	char s[64];
	unsigned char drvr[258];
	long flags;
	void *list = sp->spsPointer;
	read_cstring(list, sRsrcName, s, sizeof s);
	out("  "); out_hex(sp->spID, 2); out(" "); out(s[0] ? s : "(no name)"); nl();
	out("      kind:    "); out_kind(sp); nl();
	out("      numbers: "); out_type(sp); nl();

	SpBlock dp;
	memset(&dp, 0, sizeof dp);
	dp.spSlot = (unsigned char)slot; dp.spID = sp->spID; dp.spExtDev = sp->spExtDev;
	dp.spResult = (long)drvr;
	drvr[0] = 0;
	int named = slot_mgr(sReadDrvrName, &dp) == 0 && drvr[0];
	int in_rom = has_entry(list, sRsrcDrvrDir);
	int open = 0;
	short ref = find_dce(slot, sp->spID, sp->spExtDev, &open);
	out("      driver:  ");
	/* SReadDrvrName makes a name from the sResource's whether or not there is a driver: without one
	   in the card's ROM (sRsrcDrvrDir) or in the unit table there is none to name */
	if (!in_rom && !ref) out("none");
	else if (named) { drvr[drvr[0] + 1] = 0; out((const char *)drvr + 1); }  /* a Pascal string */
	else out("(no name)");
	if (ref && !in_rom) out(" (not from the card's ROM)");
	if (ref && open) { out(", open, reference number "); out_num(ref); }
	else if (ref) { out(", installed but closed, reference number "); out_num(ref); }
	else if (in_rom) {
		/* sRsrcFlags bit 1, fOpenAtStart; without the entry the driver is opened at start */
		int at_start = !read_word(list, sRsrcFlags, &flags) || (flags & 2);
		out(at_start ? ", not open" : ", closed until a program asks for it");
	}
	nl();
	if (sp->spCategory == catDisplay && ref) out_screen(ref);

	if (slot != 0 && s[0]) {
		/* the kernel takes 127 characters of a line: a long card goes on in a second one */
		if (card_n + (int)strlen(s) + 18 > 118) {
			pi_log(card_line);
			card_n = 0;
			sadd(card_line, &card_n, "slot "); sadd_slot(card_line, &card_n, slot); sadd(card_line, &card_n, ", more: ");
			nth = 0;
		}
		if (nth) sadd(card_line, &card_n, ", ");
		sadd(card_line, &card_n, s);
		sadd(card_line, &card_n, ref && open ? " open" : " closed");
	}
}

/* one slot's part of the report; returns 0 for an empty slot, which is left for the end */
static int one_slot(int slot)
{
	SInfoRecord info;
	SpBlock sp;
	memset(&sp, 0, sizeof sp);
	memset(&info, 0, sizeof info);
	sp.spSlot = (unsigned char)slot;
	sp.spResult = (long)&info;
	short err = slot_mgr(sReadInfo, &sp);
	short st = err ? err : info.siInitStatusA;
	/* nothing there: smEmptySlot; smBLFieldBad (-309), no byte lane holds a format block, which is
	   what the IIci ROM answered for every empty slot of the testbed; smSlotOOBErr, no such slot */
	if (st == smEmptySlot || st == -309 || st == -337) {
		return 0;                            /* (slot 0: a logic board without a declaration ROM) */
	}
	nl();
	if (slot == 0) out("The logic board (slot 0, the built-in declaration ROM)");
	else { out("Slot "); out_slot(slot); out(": a card"); }
	if (st) {
		const char *w = slot_err(st);
		out(", but it did not start: "); out(w ? w : "an error"); out(" ["); out_num(st); out("]");
		nl();
		return 1;
	}
	nl();
	if (slot != 0) n_cards++;

	/* its sResources in id order: the board (category 1) first, then the functions */
	int n = 0;
	card_n = 0;
	card_line[0] = 0;
	if (slot != 0) { sadd(card_line, &card_n, "slot "); sadd_slot(card_line, &card_n, slot); sadd(card_line, &card_n, ": "); }
	memset(&sp, 0, sizeof sp);
	sp.spSlot = (unsigned char)slot; sp.spID = 0; sp.spExtDev = 0;
	while (slot_mgr(sNextSRsrc, &sp) == 0 && sp.spSlot == slot) {
		if (sp.spCategory == catBoard) board(slot, &sp, info.siInitStatusV);
		else {
			if (!n++) { out("  Functions:"); nl(); }
			function(slot, &sp, n - 1);
		}
	}
	if (!n) { out("  No functions."); nl(); }
	if (slot != 0 && n) pi_log(card_line);
	return 1;
}

/* the whole report, from the start */
static void survey(void)
{
	tn = 0;
	sum_n = 0; sum[0] = 0;
	aurora68 = 0; n_cards = 0;
	long resp = 0;
	int sm;
	/* Gestalt's 'slot' (gestaltSlotAttr) where the System has it, else whether _SlotManager is a
	   trap of this ROM: a Plus, SE or Classic ROM leaves it unimplemented */
	if (has_os_trap((short)0xA1AD) && gestalt(TYPE('s', 'l', 'o', 't'), &resp) == 0) sm = resp & 1;
	else sm = has_os_trap((short)0xA06E);
	if (!sm) {
		out("This Macintosh has no Slot Manager, so it has no NuBus cards to list.\r\r"
		    "A Macintosh Plus, SE or Classic has no NuBus slots; an Aurora68 SE that runs the "
		    "SE's own ROM has none either. With the IIci ROM (the usual Aurora68 set-up) the "
		    "Pi's cards show here.\r");
		return;
	}
	out("The cards in this Macintosh, as the Slot Manager sees them."); nl();
	one_slot(0);
	int empty[16], ne = 0;
	for (int s = 9; s <= 14; s++)
		if (!one_slot(s)) empty[ne++] = s;        /* the empty slots are said once, at the end */
	nl();
	if (ne) {
		out(ne == 1 ? "Empty slot: " : "Empty slots: ");
		for (int i = 0; i < ne; i++) { if (i) out(", "); out_slot(empty[i]); }
		nl();
	}
	if (!n_cards) { out("No cards."); nl(); }
	/* the summary, with the empty slots */
	if (!sum_n) sadd(sum, &sum_n, "no cards");
	if (ne) {
		sadd(sum, &sum_n, "; empty:");
		for (int i = 0; i < ne; i++) { sadd(sum, &sum_n, " "); sadd_slot(sum, &sum_n, empty[i]); }
	}
	pi_log(sum);
}

/* ---- the window ----
 * The Human Interface Guidelines (1992), as the user asked of the project's Mac programs: a document
 * window with a close box, a zoom box, a grow box and two scroll bars that follow every resize (the
 * report is not wrapped, so its columns stay in line); the zoom goes to the screen that holds most
 * of the window; the window's place and size are kept in "PiSlots Prefs" (the Preferences folder on
 * System 7, the System Folder on System 6) and put back only while a screen still shows its title
 * bar; Apple, File and Edit menus with the standard key equivalents, the Edit menu's items there for
 * desk accessories and dimmed otherwise; an About box and alerts centred on the main screen with the
 * default button outlined, answering Return, Enter, Escape and Command-period; the scroll bars
 * blank while the window is inactive or PiSlots is in the background. No Balloon Help: the kit has
 * no resource compiler for 'hmnu' (mac/c/README.md). */
static void *win, *vbar, *hbar;
static TEHandle te;
static int top;                        /* the first line in view */
static int hleft;                      /* pixels scrolled to the right */
static int text_w = 480;               /* the widest line, in pixels */
static int quit, active = 1, fg = 1, use_wne;
static void *m_apple, *m_file, *m_edit;
static unsigned char pbuf[256];
enum { M_APPLE = 128, M_FILE, M_EDIT };
enum { F_CLOSE = 1, F_SAVE, F_AGAIN = 4, F_QUIT = 6 };
enum { E_UNDO = 1, E_CUT = 3, E_COPY, E_PASTE, E_CLEAR, E_SELALL = 8 };
#define IDEAL_W   (text_w + 6 + 2 + 17)   /* the widest line, the margin and the scroll bar */
#define MIN_W     220
#define MIN_H     80

/* ---- screens ---- */
#define LM_MBARHEIGHT (*(volatile short *)0xBAA)
static int mbar(void) { short h = LM_MBARHEIGHT; return h > 0 && h < 64 ? h : 20; }
static int has_cqd(void) { return !(LM_ROM85 & 0x4000) && LM_DEVICELIST && LM_MAINDEVICE; }
/* QuickDraw's screenBits.bounds, the only screen of a Mac without Color QuickDraw: (A5) points at
   thePort, the last of QuickDraw's globals; screenBits ends 108 bytes below it (arrow 68, five
   patterns 40), its bounds 6 bytes into it */
static Rect *screen_bits(void)
{
	char *a5;
	__asm__ volatile("move.l %%a5,%0" : "=r"(a5));
	return (Rect *)(*(char **)a5 - 116);
}
#define MAX_SCR 8
/* the screens in use, global coordinates, the main screen (with the menu bar) first */
static int screens(Rect *s)
{
	int n = 0;
	if (!has_cqd()) { s[0] = *screen_bits(); return 1; }
	s[n++] = *(Rect *)(deref(LM_MAINDEVICE) + 34);                    /* gdRect */
	for (char **g = LM_DEVICELIST; g && n < MAX_SCR; g = *(char ***)(deref(g) + 30)) {
		char *gd = deref(g);
		if (g == LM_MAINDEVICE || !(*(unsigned short *)(gd + 20) & 0x8000)) continue;   /* screenActive */
		s[n++] = *(Rect *)(gd + 34);
	}
	return n;
}
static int sect(const Rect *a, const Rect *b, Rect *o)
{
	o->top = a->top > b->top ? a->top : b->top;
	o->left = a->left > b->left ? a->left : b->left;
	o->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
	o->right = a->right < b->right ? a->right : b->right;
	return o->bottom > o->top && o->right > o->left;
}
/* where a window's content may go on screen i: below the menu bar and the title bar, inside the edges */
static void usable(const Rect *s, int i, Rect *u)
{
	*u = *s;
	u->top += (i == 0 ? mbar() : 0) + 20;
	u->left += 4; u->right -= 4; u->bottom -= 4;
}
/* the window's content in global coordinates */
static void content_global(Rect *r)
{
	Point p = { 0, 0 };
	SetPort(win);
	*r = *PORT_RECT(win);
	LocalToGlobal(&p);
	r->top += p.v; r->bottom += p.v; r->left += p.h; r->right += p.h;
}
/* is the title bar of a window with this content seen on some screen, and is the window not too small */
static int on_screen(const Rect *c)
{
	Rect s[MAX_SCR], o, bar = { (short)(c->top - 18), c->left, (short)(c->top - 2), c->right };
	int n = screens(s);
	if (c->right - c->left < MIN_W || c->bottom - c->top < MIN_H) return 0;
	for (int i = 0; i < n; i++) {
		Rect u = s[i];
		if (i == 0) u.top += mbar();
		if (sect(&bar, &u, &o) && o.right - o.left >= 40 && o.bottom - o.top >= 12) return 1;
	}
	return 0;
}
/* the first window on the main screen: at most what the SE's 512 x 342 shows, taller where the
   screen is (44, 8, 334, 504 on an SE) */
static void default_rect(Rect *b)
{
	Rect s[MAX_SCR], u;
	screens(s);
	usable(&s[0], 0, &u);
	b->top = (short)(u.top + 4); b->left = (short)(s[0].left + 8);
	b->right = (short)(b->left + 496 < u.right ? b->left + 496 : u.right);
	b->bottom = (short)(b->top + 560 < u.bottom - 4 ? b->top + 560 : u.bottom - 4);
}

/* ---- the File Manager, with directory IDs (HFS calls: the Preferences folder has one) ---- */
#define FS_CALL(name, trap) static short name(ParamBlock *pb) { \
	register ParamBlock *a0 __asm__("a0") = pb; register long d0 __asm__("d0"); \
	__asm__ volatile(".short " #trap : "=d"(d0), "+a"(a0) : : "d1", "d2", "a1", "memory", "cc"); \
	return (short)d0; }
FS_CALL(fs_hopen, 0xA200)
FS_CALL(fs_read, 0xA002)
FS_CALL(fs_hcreate, 0xA208)
FS_CALL(fs_hgetfinfo, 0xA20C)
FS_CALL(fs_hsetfinfo, 0xA20D)
#define DIR_ID(pb) ((pb)->u.f.ioFlNum)  /* ioDirID: offset 48, where the file information calls have ioFlNum */

/* FindFolder ($A823, D0 = 0): FUNCTION FindFolder(vRefNum: INTEGER; folderType: OSType;
   createFolder: BOOLEAN; VAR foundVRefNum: INTEGER; VAR foundDirID: LONGINT): OSErr */
static short find_folder(short vref, long type, short *fvref, long *fdir)
{
	short err;
	__asm__ volatile("clr.w -(%%sp)\n\t"
	                 "move.w %1,-(%%sp)\n\t"
	                 "move.l %2,-(%%sp)\n\t"
	                 "move.w #0x0100,-(%%sp)\n\t"      /* createFolder TRUE: a BOOLEAN in the word's high byte */
	                 "move.l %3,-(%%sp)\n\t"
	                 "move.l %4,-(%%sp)\n\t"
	                 "moveq #0,%%d0\n\t"
	                 ".short 0xA823\n\t"
	                 "move.w (%%sp)+,%0"
	                 : "=d"(err) : "d"(vref), "d"(type), "a"(fvref), "a"(fdir) : "d0", "d1", "d2", "a0", "a1", "memory", "cc");
	return err;
}
/* SysEnvirons ($A090): A0 the record, D0 the version; sysVRefNum (offset 14) is the System Folder's
   working directory */
static short sys_folder(void)
{
	static short rec[8];
	register void *a0 __asm__("a0") = rec;
	register long d0 __asm__("d0") = 1;
	__asm__ volatile(".short 0xA090" : "+d"(d0), "+a"(a0) : : "d1", "d2", "a1", "memory", "cc");
	return rec[7];
}

/* ---- the window's place, kept between runs ---- */
#define PREF_MAGIC TYPE('P', 'S', 'L', '1')
static int pref_place(short *vref, long *dir)
{
	long resp = 0;
	if (has_os_trap((short)0xA1AD) && gestalt(TYPE('f', 'o', 'l', 'd'), &resp) == 0 && (resp & 1))
		return find_folder(-32768 /* kOnSystemDisk */, TYPE('p', 'r', 'e', 'f'), vref, dir) == 0;
	*vref = sys_folder(); *dir = 0;
	return *vref != 0;
}
static int load_prefs(Rect *r)
{
	ParamBlock pb;
	short vref, ref;
	long dir, buf[3];
	if (!pref_place(&vref, &dir)) return 0;
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = pstr(pbuf, "PiSlots Prefs"); pb.ioVRefNum = vref; DIR_ID(&pb) = dir; pb.ioPermssn = 1;
	if (fs_hopen(&pb)) return 0;
	ref = pb.ioRefNum;
	memset(&pb, 0, sizeof pb);
	pb.ioRefNum = ref; pb.u.io.ioBuffer = buf; pb.u.io.ioReqCount = sizeof buf; pb.u.io.ioPosMode = fsFromStart;
	short err = fs_read(&pb);
	mac_PBClose(&pb);
	if (err || pb.u.io.ioActCount != sizeof buf || buf[0] != (long)PREF_MAGIC) return 0;
	memcpy(r, &buf[1], sizeof *r);
	return 1;
}
static void save_prefs(void)
{
	ParamBlock pb;
	short vref, ref;
	long dir, buf[3];
	Rect c;
	if (!win || !pref_place(&vref, &dir)) return;
	content_global(&c);
	buf[0] = (long)PREF_MAGIC;
	memcpy(&buf[1], &c, sizeof c);
	pstr(pbuf, "PiSlots Prefs");
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = pbuf; pb.ioVRefNum = vref; DIR_ID(&pb) = dir;
	if (!fs_hcreate(&pb)) {                                    /* a new file: say whose it is */
		memset(&pb, 0, sizeof pb);
		pb.ioNamePtr = pbuf; pb.ioVRefNum = vref; DIR_ID(&pb) = dir;
		if (!fs_hgetfinfo(&pb)) {
			pb.u.f.fdType = TYPE('p', 'r', 'e', 'f'); pb.u.f.fdCreator = TYPE('P', 'I', 'S', 'L');
			DIR_ID(&pb) = dir;                                 /* GetFInfo leaves the file number there */
			fs_hsetfinfo(&pb);
		}
	}
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = pbuf; pb.ioVRefNum = vref; DIR_ID(&pb) = dir; pb.ioPermssn = 3;
	if (fs_hopen(&pb)) return;                                 /* a locked disk: the place is not kept */
	ref = pb.ioRefNum;
	memset(&pb, 0, sizeof pb);
	pb.ioRefNum = ref; pb.u.io.ioBuffer = buf; pb.u.io.ioReqCount = sizeof buf; pb.u.io.ioPosMode = fsFromStart;
	mac_PBWrite(&pb);
	pb.u.io.ioMisc = sizeof buf;
	mac_PBSetEOF(&pb);
	mac_PBClose(&pb);
	memset(&pb, 0, sizeof pb);
	pb.ioVRefNum = vref;
	mac_PBFlushVol(&pb);
}

/* ---- the text and its scroll bars ---- */
/* The report is not wrapped, so its columns stay in line however narrow the window: TextEdit's
   destination rectangle is as wide as the widest line, the view is the window, and the horizontal
   bar moves one over the other. */
static int line_h(void) { return (*te)->lineHeight > 0 ? (*te)->lineHeight : 11; }
static int visible(void)
{
	Rect *v = &(*te)->viewRect;
	int n = (v->bottom - v->top) / line_h();
	return n > 0 ? n : 1;
}
static int max_top(void)
{
	int n = (*te)->nLines - visible();
	return n > 0 ? n : 0;
}
static void scroll_to(int n)
{
	int m = max_top();
	if (n > m) n = m;
	if (n < 0) n = 0;
	if (n != top) { TEScroll(0, (top - n) * line_h(), te); top = n; }
	SetCtlMax(vbar, m);
	SetCtlValue(vbar, top);
}
static int view_w(void) { Rect *v = &(*te)->viewRect; return v->right - v->left; }
static void hscroll_to(int x)
{
	int m = text_w - view_w();
	if (m < 0) m = 0;
	if (x > m) x = m;
	if (x < 0) x = 0;
	if (x != hleft) { TEScroll(hleft - x, 0, te); hleft = x; }
	SetCtlMax(hbar, m);
	SetCtlValue(hbar, hleft);
}
/* the scroll bars along the right and bottom edges up to the grow box, the text in the rest */
static void layout(void)
{
	Rect r = *PORT_RECT(win), b, v;
	b.top = r.top - 1; b.left = r.right - 15; b.bottom = r.bottom - 14; b.right = r.right + 1;
	HideControl(vbar);
	MoveControl(vbar, b.left, b.top);
	SizeControl(vbar, b.right - b.left, b.bottom - b.top);
	b.top = r.bottom - 15; b.left = r.left - 1; b.bottom = r.bottom + 1; b.right = r.right - 14;
	HideControl(hbar);
	MoveControl(hbar, b.left, b.top);
	SizeControl(hbar, b.right - b.left, b.bottom - b.top);
	v.top = r.top + 4; v.left = r.left + 6; v.bottom = r.bottom - 17; v.right = r.right - 17;
	v.bottom = v.top + (v.bottom - v.top) / line_h() * line_h();
	TERec *t = *te;
	t->viewRect = v;
	t->destRect.left = v.left - hleft; t->destRect.right = t->destRect.left + text_w + 2;
	t->destRect.top = v.top - top * line_h(); t->destRect.bottom = t->destRect.top + 30000;
	TECalText(te);
	if (active) { ShowControl(vbar); ShowControl(hbar); }
}
static void resized(void)
{
	SetPort(win);
	InvalRect(PORT_RECT(win));
	layout();
	scroll_to(top);
	hscroll_to(hleft);
}
static void look_again(void)
{
	survey();
	SetPort(win);
	/* the widest line in Monaco 9, the window's font */
	text_w = 200;
	for (long i = 0, start = 0; i <= tn; i++)
		if (i == tn || txt[i] == '\r') {
			int w = TextWidth(txt, (short)start, (short)(i - start));
			if (w > text_w) text_w = w;
			start = i + 1;
		}
	TESetText(txt, tn, te);
	TESetSelect(0, 0, te);
	top = 0; hleft = 0;
	layout();
	scroll_to(0);
	hscroll_to(0);
	InvalRect(PORT_RECT(win));
}
/* the zoom box's standard state: the screen holding most of the window, as wide as the report needs
   and as tall as it is, within that screen (WStateData's stdState, the window's dataHandle at 130) */
static void set_std_state(void)
{
	Rect s[MAX_SCR], c, o, u;
	int n = screens(s), best = 0;
	long most = -1;
	content_global(&c);
	for (int i = 0; i < n; i++)
		if (sect(&c, &s[i], &o)) {
			long a = (long)(o.bottom - o.top) * (o.right - o.left);
			if (a > most) { most = a; best = i; }
		}
	usable(&s[best], best, &u);
	int need = ((*te)->nLines + 1) * line_h() + 4 + 17;
	if (u.right - u.left > IDEAL_W) u.right = (short)(u.left + IDEAL_W);
	if (u.bottom - u.top > need) u.bottom = (short)(u.top + need);
	char **wsd = *(char ***)((char *)win + 130);
	if (wsd && *wsd) ((Rect *)deref(wsd))[1] = u;
}

/* ---- modal boxes: the About box and alerts ---- */
static int next_event(EventRecord *ev, long sleep)
{
	if (use_wne) return WaitNextEvent(everyEvent, ev, sleep, 0);
	SystemTask();
	return GetNextEvent(everyEvent, ev);
}
static void update(void *w);
static void activate(int on);
/* A modal box of w x h with a bold heading, a text and an OK button, its heavy rounded outline
   showing it is the default; icon: a system alert icon (0 stop, 1 note, 2 caution) or -1. Centred
   on the main screen, a third of the way down, as Alert places its boxes. */
static void modal_box(int w, int h, const char *head, const char *body, int icon)
{
	Rect s[MAX_SCR];
	screens(s);
	int sw = s[0].right - s[0].left, sh = s[0].bottom - s[0].top - mbar();
	Rect r;
	r.left = (short)(s[0].left + (sw - w) / 2); r.right = (short)(r.left + w);
	r.top = (short)(s[0].top + mbar() + (sh - h) / 3); r.bottom = (short)(r.top + h);
	void *box = NewWindow(0, &r, pstr(pbuf, ""), 1, 1 /* dBoxProc */, (void *)-1, 0, 0);
	SetPort(box);
	int tl = icon >= 0 ? 74 : 18;
	Rect ok = { (short)(h - 34), (short)(w - 82), (short)(h - 14), (short)(w - 18) };
	Rect text = { (short)(head ? 38 : 14), (short)tl, (short)(h - 42), (short)(w - 18) };
	Rect ic = { 14, 20, 46, 52 };
	void *btn = NewControl(box, &ok, pstr(pbuf, "OK"), 1, 0, 0, 1, pushButProc, 0);
	void *ih = icon >= 0 ? (void *)GetIcon(icon) : 0;
	EventRecord ev;
	int done = 0;
	while (!done) {
		if (!next_event(&ev, 10)) continue;
		if (ev.what == updateEvt && (void *)ev.message != box) { update((void *)ev.message); SetPort(box); continue; }
		if (ev.what == activateEvt) { if ((void *)ev.message == win) { activate(ev.modifiers & activeFlag); SetPort(box); } continue; }
		switch (ev.what) {
		case updateEvt:
			BeginUpdate(box);
			if (ih) PlotIcon(&ic, ih);
			if (head) { TextFont(0); TextSize(12); TextFace(1); MoveTo((short)tl, 28); DrawString(pstr(pbuf, head)); TextFace(0); }
			TextFont(3); TextSize(10);                              /* Geneva 10 */
			TETextBox(body, (long)strlen(body), &text, 0);
			TextFont(0); TextSize(12);
			DrawControls(box);
			{ Rect o = ok; InsetRect(&o, -4, -4); PenSize(3, 3); FrameRoundRect(&o, 16, 16); PenNormal(); }
			EndUpdate(box);
			break;
		case mouseDown: {
			Point p = ev.where;
			void *w2 = 0, *ctl = 0;
			if (FindWindow(pt_long(p), &w2) != inContent || w2 != box) { SysBeep(1); break; }
			GlobalToLocal(&p);
			if (FindControl(pt_long(p), box, &ctl) && ctl == btn && TrackControl(btn, pt_long(p), 0)) done = 1;
			break;
		}
		case keyDown: case autoKey: {
			int c = (int)(ev.message & charCodeMask);
			if (c == 0x0D || c == 0x03 || c == 0x1B || ((ev.modifiers & cmdKey) && c == '.')) {
				HiliteControl(btn, 1);                              /* the button flashes */
				unsigned long t = LM_TICKS + 8;
				while (LM_TICKS < t) { }
				done = 1;
			}
			break;
		}
		case osEvt:
			if (((unsigned long)ev.message >> 24) == suspendResumeMessage) fg = ev.message & resumeFlag;
			break;
		}
	}
	DisposeWindow(box);
	SetPort(win);
}
static void about(void)
{
	modal_box(360, 168, "PiSlots",
	          "The NuBus cards in this Macintosh: each slot's card, its maker, and what it does, with "
	          "each function's driver and whether it is open.\r\r"
	          "Part of Aurora68, the Macintosh SE with a Raspberry Pi inside. Free software under the "
	          "Mozilla Public License 2.0.", -1);
}

/* ---- File > Save As...: the report as a TeachText document ---- */
static char *put2(char *p, int v) { *p++ = (char)('0' + v / 10); *p++ = (char)('0' + v % 10); return p; }
static char *put_num(char *p, unsigned long v)
{
	char b[12];
	int i = 0;
	do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (i) *p++ = b[--i];
	return p;
}
/* the clock (seconds since 1904, local time) as "2026-11-20" */
static char *put_date(char *p, unsigned long t)
{
	static const unsigned char mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	unsigned long days = t / 86400;
	int y = 1904, m = 0;
	while (days >= (unsigned long)(y % 4 ? 365 : 366)) { days -= y % 4 ? 365 : 366; y++; }    /* every fourth year to 2099 */
	while (days >= (unsigned long)(mdays[m] + (m == 1 && y % 4 == 0))) { days -= mdays[m] + (m == 1 && y % 4 == 0); m++; }
	p = put_num(p, (unsigned long)y); *p++ = '-'; p = put2(p, m + 1); *p++ = '-'; p = put2(p, (int)days + 1);
	return p;
}
static void save_as(void)
{
	static unsigned char name[64];
	static char msg[120];
	SFReply rep;
	ParamBlock pb;
	char t[40], *p;
	memcpy(t, "PiSlots ", 8);
	p = put_date(t + 8, LM_TIME);           /* "PiSlots 2026-11-20" */
	*p = 0;
	pstr(name, t);
	/* Standard File's box (304 x 104 on System 6) centred on the main screen, a third of the way down */
	Rect s[MAX_SCR];
	screens(s);
	Point where = { (short)(s[0].top + mbar() + (s[0].bottom - s[0].top - mbar() - 104) / 3),
	                (short)(s[0].left + (s[0].right - s[0].left - 304) / 2) };
	SFPutFile(pt_long(where), pstr(pbuf, "Save the list of cards as:"), name, 0, &rep, 1);
	if (!rep.good) return;
	SetPort(win);
	short err;
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = rep.fName; pb.ioVRefNum = rep.vRefNum;
	err = mac_PBCreate(&pb);
	if (err == dupFNErr) err = 0;          /* Standard File has asked about replacing it */
	if (!err) {
		memset(&pb, 0, sizeof pb);
		pb.ioNamePtr = rep.fName; pb.ioVRefNum = rep.vRefNum;
		err = mac_PBGetFInfo(&pb);
		pb.u.f.fdType = TYPE('T', 'E', 'X', 'T'); pb.u.f.fdCreator = TYPE('t', 't', 'x', 't');
		if (!err) err = mac_PBSetFInfo(&pb);
	}
	if (!err) {
		memset(&pb, 0, sizeof pb);
		pb.ioNamePtr = rep.fName; pb.ioVRefNum = rep.vRefNum; pb.ioPermssn = fsWrPerm;
		err = mac_PBOpen(&pb);
		if (!err) {
			pb.u.io.ioBuffer = txt; pb.u.io.ioReqCount = tn;
			pb.u.io.ioPosMode = fsFromStart; pb.u.io.ioPosOffset = 0;
			err = mac_PBWrite(&pb);
			pb.u.io.ioMisc = tn;
			mac_PBSetEOF(&pb);
			mac_PBClose(&pb);
			memset(&pb, 0, sizeof pb);
			pb.ioVRefNum = rep.vRefNum;
			mac_PBFlushVol(&pb);
		}
	}
	if (err) {
		char *q = msg;
		const char *a = err == -34 ? "The disk is full." : err == -44 || err == -46 || err == -61 ? "The disk is locked." :
		                "The disk could not be written.";
		while (*a) *q++ = *a++;
		a = " (error ";
		while (*a) *q++ = *a++;
		*q++ = '-'; q = put_num(q, (unsigned long)-err); *q++ = ')'; *q = 0;
		modal_box(340, 110, "The list of cards was not saved.", msg, 0);    /* a stop alert */
	}
}

/* ---- menus ---- */
static void setup_menus(void)
{
	unsigned char t[2] = { 1, 0x14 };     /* the apple */
	m_apple = NewMenu(M_APPLE, t);
	AppendMenu(m_apple, pstr(pbuf, "About PiSlots\xC9;(-"));
	AddResMenu(m_apple, TYPE('D', 'R', 'V', 'R'));
	InsertMenu(m_apple, 0);
	m_file = NewMenu(M_FILE, pstr(pbuf, "File"));
	AppendMenu(m_file, pstr(pbuf, "Close/W;Save As\xC9;(-;Look Again/R;(-;Quit/Q"));
	InsertMenu(m_file, 0);
	/* Undo to Clear in the standard order, so SystemEdit gives desk accessories the right ones */
	m_edit = NewMenu(M_EDIT, pstr(pbuf, "Edit"));
	AppendMenu(m_edit, pstr(pbuf, "Undo/Z;(-;Cut/X;Copy/C;Paste/V;Clear;(-;Select All/A"));
	InsertMenu(m_edit, 0);
	DrawMenuBar();
}
static void enable(void *m, int item, int on) { if (on) EnableItem(m, item); else DisableItem(m, item); }
/* the report cannot be changed: Undo, Cut, Paste and Clear are for desk accessories only */
static void fix_menus(void)
{
	int da = FrontWindow() != win;
	enable(m_file, F_CLOSE, !da);
	enable(m_edit, E_UNDO, da);
	enable(m_edit, E_CUT, da);
	enable(m_edit, E_COPY, 1);
	enable(m_edit, E_PASTE, da);
	enable(m_edit, E_CLEAR, da);
	enable(m_edit, E_SELALL, !da);
}
/* Edit > Copy: the selection, or the whole report when nothing is selected */
static void copy(void)
{
	TERec *t = *te;
	ZeroScrap();
	if (t->selEnd > t->selStart) {
		TECopy(te);
		PutScrap(LM_TESCRPLEN, TYPE('T', 'E', 'X', 'T'), *LM_TESCRPH);
	} else PutScrap(tn, TYPE('T', 'E', 'X', 'T'), txt);
}
static void do_menu(long mr)
{
	int id = (int)(mr >> 16), item = (int)(mr & 0xFFFF);
	SetPort(win);
	if (id == M_APPLE) {
		if (item == 1) about();
		else { GetItem(m_apple, item, pbuf); OpenDeskAcc(pbuf); SetPort(win); }
	} else if (id == M_FILE) {
		if (item == F_SAVE) save_as();
		else if (item == F_AGAIN) look_again();
		else if (item == F_CLOSE || item == F_QUIT) quit = 1;
	} else if (id == M_EDIT) {
		if (item <= E_CLEAR && SystemEdit(item - 1)) { HiliteMenu(0); return; }     /* a desk accessory's */
		if (item == E_COPY) copy();
		else if (item == E_SELALL) TESetSelect(0, 32767, te);
	}
	HiliteMenu(0);
}

/* ---- events ---- */
void c_ctl_action(void *ctl, int part)
{
	if (ctl == hbar) {                       /* two characters an arrow, the view less two a page */
		int page = view_w() > 24 ? view_w() - 12 : 12;
		switch (part) {
		case inUpButton:   hscroll_to(hleft - 12); break;
		case inDownButton: hscroll_to(hleft + 12); break;
		case inPageUp:     hscroll_to(hleft - page); break;
		case inPageDown:   hscroll_to(hleft + page); break;
		}
		return;
	}
	int page = visible() > 1 ? visible() - 1 : 1;
	switch (part) {
	case inUpButton:   scroll_to(top - 1); break;
	case inDownButton: scroll_to(top + 1); break;
	case inPageUp:     scroll_to(top - page); break;
	case inPageDown:   scroll_to(top + page); break;
	}
}
static void activate(int on)
{
	SetPort(win);
	active = on;
	/* an inactive window's scroll bars are blank, outlined by DrawGrowIcon, as the Finder's are:
	   hidden rather than dimmed (System 7.1's dimmed bar still draws its arrows) */
	if (on) { TEActivate(te); ShowControl(vbar); ShowControl(hbar); }
	else { TEDeactivate(te); HideControl(vbar); HideControl(hbar); }
	DrawGrowIcon(win);
}
static void content_click(EventRecord *ev)
{
	Point p = ev->where;
	void *ctl = 0;
	SetPort(win);
	GlobalToLocal(&p);
	int part = FindControl(pt_long(p), win, &ctl);
	if ((ctl == vbar || ctl == hbar) && part) {
		if (part == inThumb) {
			TrackControl(ctl, pt_long(p), 0);
			if (ctl == vbar) scroll_to(GetCtlValue(vbar));
			else hscroll_to(GetCtlValue(hbar));
		} else TrackControl(ctl, pt_long(p), (void *)pas_ctl_action);
		return;
	}
	if (PtInRect(pt_long(p), &(*te)->viewRect)) TEClick(pt_long(p), (ev->modifiers & shiftKey) != 0, te);
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
			Rect lim = { MIN_H, MIN_W, 32767, 32767 };       /* top, left: the smallest height and width */
			long sz = GrowWindow(w, pt_long(ev->where), &lim);
			if (sz) { SizeWindow(w, (short)(sz & 0xFFFF), (short)(sz >> 16), 1); resized(); }
		}
		break;
	case inZoomIn: case inZoomOut:
		if (w == win && TrackBox(w, pt_long(ev->where), part)) {
			if (part == inZoomOut) set_std_state();
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
	if (ev->modifiers & cmdKey) { if (c != '.') { fix_menus(); do_menu(MenuKey(c)); } return; }
	SetPort(win);
	int page = visible() > 1 ? visible() - 1 : 1;
	switch (c) {
	case 0x1E: scroll_to(top - 1); break;      /* up arrow */
	case 0x1F: scroll_to(top + 1); break;      /* down arrow */
	case 0x0B: scroll_to(top - page); break;   /* page up */
	case 0x0C: scroll_to(top + page); break;   /* page down */
	case 0x01: scroll_to(0); break;            /* home */
	case 0x04: scroll_to(32767); break;        /* end */
	default: break;                            /* the report takes no typing */
	}
}
static void update(void *w)
{
	if (w != win) return;
	SetPort(win);
	BeginUpdate(win);
	EraseRect(PORT_RECT(win));
	TEUpdate(PORT_RECT(win), te);
	DrawControls(win);
	DrawGrowIcon(win);
	EndUpdate(win);
}

int main(void)
{
	use_wne = has_tool_trap((short)0xA860);
	setup_menus();
	txt = mac_newptr(TXT_MAX + 16);
	if (!txt) { SysBeep(5); return 0; }

	Rect b;
	if (!load_prefs(&b) || !on_screen(&b)) default_rect(&b);
	win = NewWindow(0, &b, pstr(pbuf, "PiSlots"), 1, zoomDocProc, (void *)-1, 1, 0);
	SetPort(win);
	TextFont(4); TextSize(9);                  /* Monaco 9: the numbers line up */
	Rect r = *PORT_RECT(win);
	vbar = NewControl(win, &r, pstr(pbuf, ""), 1, 0, 0, 0, scrollBarProc, 0);
	hbar = NewControl(win, &r, pstr(pbuf, ""), 1, 0, 0, 0, scrollBarProc, 0);
	te = (TEHandle)TENew(&r, &r);
	look_again();

	EventRecord ev;
	while (!quit) {
		if (next_event(&ev, fg ? 30 : 60)) {
			switch (ev.what) {
			case mouseDown: mouse_down(&ev); break;
			case keyDown: case autoKey: key_down(&ev); break;
			case updateEvt: update((void *)ev.message); break;
			case activateEvt: if ((void *)ev.message == win) activate(ev.modifiers & activeFlag); break;
			case osEvt:
				/* suspend and resume (MultiFinder, System 7): the window is drawn inactive behind
				   another program, its scroll bar dimmed */
				if (((unsigned long)ev.message >> 24) == suspendResumeMessage) {
					fg = ev.message & resumeFlag;
					if (FrontWindow() == win) activate(fg);
				}
				break;
			}
		}
		if (fg && active) TEIdle(te);
	}
	save_prefs();
	return 0;
}
