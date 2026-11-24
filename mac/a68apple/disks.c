/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/a68apple/disks.c - Aurora68 Apple II's Disks menu: the card's own images (the SD card's root), and disks from
 * the Mac: Insert Disk... (Standard File, any Mac volume), images dropped on the program's icon or opened with it (the
 * Finder's files at launch, Open Documents Apple events while it runs), the "Apple II Disks" folder beside the program
 * (its images and folders as submenus); and the card's requests for their bytes answered from the event loop
 * (docs/20-apple-ii-card.md "Disks from the Mac"; the card's half: emu68/src/psmac/psmac_a2.h, "disks from the Mac").
 *
 * Backstory (24 November 2026). The owner keeps folders of Apple II disk images on the Mac and the NAS (an AppleShare
 * volume through Lagoon, a SCSI disk), and the card offered only the images in the SD card's root. So the program opens
 * the Mac's files itself, by volume, folder and name (HOpen: a file at any depth, never a path, which a Pascal string
 * cuts at 255 characters), describes each in a media slot of the window and sends A68A_C_MACINSERT; the card then asks
 * for its bytes (all of a floppy at once, a hard disk's pieces on demand) and sends what the Apple writes, through a
 * ring of 16 requests, and the program answers each with the File Manager, in the order of their seq, at every turn of
 * every loop it runs (the main loop, Standard File's dialog, its own boxes), so that the Apple waits for its disk but
 * the Mac never waits for the Apple. While disks are in, WaitNextEvent sleeps a tick at most (none for a second after a
 * request), in front or behind. A file the card has let go of (ejected and written back, refused) is closed; at the end
 * every disk is ejected and its last writes answered before the files close. How the Mac's program finds its own
 * folder and the Finder's documents follows Octavo's (the Quill port: CurApRefNum's FCB for the folder, AppParmHandle
 * at launch, the required Apple events while it runs).
 *
 * The units: Insert Disk... puts an image into the first free drive of its kind (a 5.25" image: drive 1, then 2; a block
 * image: HD1 to HD4), the Disks menu's unit items into that unit (an empty one asks for a file, a full one ejects);
 * documents dropped together fill drive 1, drive 2, then (140K sector images) the HDs; block images HD1 to HD4.
 */
#include "apple.h"

void sf_filter_stub(void), sf_dfilter_stub(void), ae_handler_stub(void);
short fm_HOpen(void *pb), fm_Read(void *pb), fm_Write(void *pb), fm_Close(void *pb), fm_GetEOF(void *pb), fm_FlushFile(void *pb),
      fm_DirCreate(void *pb), fm_GetFCBInfo(void *pb), fm_GetCatInfo(void *pb);
short a2_gestalt(long selector, long *response);
void a2_hlock(void **h), a2_disposehandle(void **h);
short GetNextProcess(unsigned long *psn), GetProcessInformation(const unsigned long *psn, void *info), SetFrontProcess(const unsigned long *psn);

/* the File Manager's parameter blocks (HParamBlockRec, CInfoPBRec), by offset (Inside Macintosh: Files) */
#define PB_SIZE    108
#define W(pb, o)   (*(short *)((char *)(pb) + (o)))
#define L(pb, o)   (*(long *)((char *)(pb) + (o)))
#define P(pb, o)   (*(void **)((char *)(pb) + (o)))
#define B(pb, o)   (*(unsigned char *)((char *)(pb) + (o)))
#define PB_NAME    18
#define PB_VREF    22
#define PB_REF     24
#define PB_PERM    27
#define PB_MISC    28                  /* ioMisc (GetEOF's answer); ioFDirIndex, ioFCBIndx for the catalogue calls */
#define PB_ATTRIB  30
#define PB_BUF     32                  /* ioBuffer; ioFlFndrInfo for the catalogue (fdType at 32, fdFlags at 40) */
#define PB_REQ     36
#define PB_ACT     40
#define PB_FLAGS   40
#define PB_POSMODE 44
#define PB_POSOFF  46
#define PB_DIRID   48
#define PB_LGLEN   54
#define PB_FCBVREF 52
#define PB_FCBPAR  58
#define LM_CURAPREFNUM (*(volatile short *)0x900)
#define LM_APPPARMHANDLE (*(unsigned char ***)0xAEC)
#define fsRdPerm   1
#define fsRdWrPerm 3
#define opWrErr    (-49)
#define kHighLevelEvent 23

static volatile struct a68a_mac *mh;      /* the program's half of the disks from the Mac, and the card's */
static volatile struct a68a_macc *mc;
static unsigned long session;
static unsigned long last_served;          /* Ticks at the last request answered */
static int quit_asked, ae_ok, notice_due;
static char notice_text[128];

/* ---- names and kinds ----------------------------------------------------------------------------------------- */
static void cstr(const unsigned char *p, char *out, int max)
{
	int n = p[0] < max - 1 ? p[0] : max - 1;
	memcpy(out, p + 1, (unsigned long)n);
	out[n] = 0;
}
static int ends_ci(const char *s, const char *suf)
{
	unsigned long n = strlen(s), k = strlen(suf);
	if (k > n) return 0;
	for (unsigned long i = 0; i < k; i++) { char c = s[n - k + i]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if (c != suf[i]) return 0; }
	return 1;
}
static int a2_ext(const char *n)
{
	return ends_ci(n, ".dsk") || ends_ci(n, ".do") || ends_ci(n, ".po") || ends_ci(n, ".nib") || ends_ci(n, ".woz") ||
	       ends_ci(n, ".2mg") || ends_ci(n, ".hdv");
}
/* the Mac file types some programs give these images: the extension the card's library needs for each */
static const char *type_ext(long t)
{
	switch (t) {
	case TYPE('2', 'I', 'M', 'G'): return ".2mg";
	case TYPE('W', 'O', 'Z', ' '): case TYPE('W', 'O', 'Z', '2'): return ".woz";
	case TYPE('D', 'S', 'K', ' '): return ".dsk";
	case TYPE('P', 'O', ' ', ' '): return ".po";
	case TYPE('D', 'O', ' ', ' '): return ".do";
	case TYPE('N', 'I', 'B', ' '): return ".nib";
	case TYPE('H', 'D', 'V', ' '): return ".hdv";
	}
	return 0;
}
static int wanted(const unsigned char *pname, long type)
{
	char n[64];
	cstr(pname, n, sizeof n);
	return a2_ext(n) || type_ext(type);
}
/* the card's rule (psmac_a2.c kind_of): a 5.25" image or a block image, by its name and size */
static unsigned kind_of(const char *n, long size)
{
	if (ends_ci(n, ".nib") || ends_ci(n, ".woz")) return A68A_KIND_525;
	if (ends_ci(n, ".hdv")) return A68A_KIND_BLOCK;
	if (ends_ci(n, ".2mg")) return size >= 143360 && size <= 143360 + 1024 ? A68A_KIND_525 : A68A_KIND_BLOCK;
	return size == 143360 || size == 116480 ? A68A_KIND_525 : A68A_KIND_BLOCK;
}
static void say(const char *a, const char *b, const char *c)
{
	char *p = notice_text, *e = notice_text + sizeof notice_text - 1;
	for (const char *s = a; s && *s && p < e; ) *p++ = *s++;
	for (const char *s = b; s && *s && p < e; ) *p++ = *s++;
	for (const char *s = c; s && *s && p < e; ) *p++ = *s++;
	*p = 0;
}
static char *num(char *p, long v)
{
	char t[12]; int n = 0;
	if (v < 0) { *p++ = '-'; v = -v; }
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n) *p++ = t[--n];
	*p = 0;
	return p;
}

/* ---- the Mac's files the card holds ---------------------------------------------------------------------------- */
struct med {
	long id, size, type;
	short ref, vref;                       /* the access path; the volume (the real one, from the FCB) */
	long dir;                              /* the folder it is in */
	unsigned char name[64];                /* its name on the Mac (Pascal) */
	int open, seen, ro, wrote;
};
static struct med md[A68A_MEDIA];
static long next_id = 1;
static int ready(void) { return mh && mh->magic == A68A_MAC_MAGIC && mc->session == session; }
static int any_open(void) { for (int s = 0; s < (int)A68A_MEDIA; s++) if (md[s].open) return 1; return 0; }
static void file_close(int s)
{
	unsigned char pb[PB_SIZE];
	memset(pb, 0, sizeof pb);
	W(pb, PB_REF) = md[s].ref;
	fm_Close(pb);
	md[s].open = 0;
}

/* a file into a media slot (not yet the card's): -1 with notice_text when it cannot be */
static int file_open(short vref, long dir, const unsigned char *pname, long type)
{
	char n[72];
	cstr(pname, n, 64);
	if (!ready()) { say("The Aurora68 Apple II Card is not ready for disks from the Mac.", 0, 0); return -1; }
	int s = 0;
	while (s < (int)A68A_MEDIA && md[s].open) s++;
	if (s == (int)A68A_MEDIA) { say("Eight disks from the Mac at most: eject one first.", 0, 0); return -1; }
	unsigned char pb[PB_SIZE];
	memset(pb, 0, sizeof pb);
	P(pb, PB_NAME) = (void *)pname; W(pb, PB_VREF) = vref; L(pb, PB_DIRID) = dir; B(pb, PB_PERM) = fsRdWrPerm;
	short err = fm_HOpen(pb), ro = 0;
	if (err == opWrErr) {                  /* open already, for writing: one of ours */
		for (int k = 0; k < (int)A68A_MEDIA; k++) if (md[k].open && md[k].ref == W(pb, PB_REF)) { say(n, " is in a drive already.", 0); return -1; }
	}
	if (err) {                             /* locked, on a locked volume or a read-only share: read only */
		memset(pb, 0, sizeof pb);
		P(pb, PB_NAME) = (void *)pname; W(pb, PB_VREF) = vref; L(pb, PB_DIRID) = dir; B(pb, PB_PERM) = fsRdPerm;
		err = fm_HOpen(pb); ro = 1;
	}
	if (err) { char e[16]; num(e, err); say(n, " could not be opened: File Manager error ", e); return -1; }
	struct med *m = &md[s];
	memset(m, 0, sizeof *m);
	m->ref = W(pb, PB_REF); m->ro = ro; m->type = type; m->open = 1;
	memset(pb, 0, sizeof pb);
	W(pb, PB_REF) = m->ref;
	m->size = fm_GetEOF(pb) ? 0 : L(pb, PB_MISC);
	memset(pb, 0, sizeof pb);              /* where it is, as the folder's menus name their files: the FCB's volume and folder */
	P(pb, PB_NAME) = m->name; W(pb, PB_REF) = m->ref;
	if (fm_GetFCBInfo(pb)) { memcpy(m->name, pname, (unsigned long)pname[0] + 1); m->vref = vref; m->dir = dir; }
	else { m->vref = W(pb, PB_FCBVREF); m->dir = L(pb, PB_FCBPAR); }
	if (!a2_ext(n)) {
		const char *x = type_ext(type);
		if (!x) { file_close(s); say(n, " is not an Apple II disk image (.dsk .do .po .nib .woz .2mg .hdv).", 0); return -1; }
		unsigned long k = strlen(n);
		if (k > 58) k = 58;
		memcpy(n + k, x, strlen(x) + 1);   /* the card's library tells the format by the name's ending */
	}
	if (m->size < 1024) { file_close(s); say(n, " is too small for a disk image.", 0); return -1; }
	m->id = next_id++;
	volatile struct a68a_mmedia *x = &mh->media[s];
	x->id = (unsigned long)m->id; x->size = (unsigned long)m->size; x->flags = ro ? A68A_MF_RO : 0;
	unsigned long k = strlen(n);
	for (unsigned long i = 0; i < sizeof x->name; i++) x->name[i] = i < k ? n[i] : 0;
	return s;
}
static unsigned slot_kind(int s) { char n[64]; for (unsigned i = 0; i < 64; i++) n[i] = mh->media[s].name[i]; n[63] = 0; return kind_of(n, md[s].size); }
static int slot_nibbles(int s) { char n[64]; for (unsigned i = 0; i < 64; i++) n[i] = mh->media[s].name[i]; n[63] = 0; return ends_ci(n, ".nib") || ends_ci(n, ".woz"); }
/* the slot's file to the card, into unit (or A68A_UNIT_AUTO): 0, or -1 (refused: notice_text says why) */
static int slot_insert(int s, unsigned long unit)
{
	command(A68A_C_MACINSERT, (unsigned long)s, unit);
	if (mc->ms[s].id == (unsigned long)md[s].id && mc->ms[s].state == A68A_MS_REFUSED) {
		say((const char *)blk->message, 0, 0);
		file_close(s);
		return -1;
	}
	md[s].seen = 0;
	return 0;
}
static void insert_file(short vref, long dir, const unsigned char *pname, long type, unsigned long unit)
{
	int s = file_open(vref, dir, pname, type);
	if (s < 0 || slot_insert(s, unit) < 0) notice(notice_text);
}

/* ---- the card's requests ------------------------------------------------------------------------------------- */
int disks_serve(void)
{
	if (!ready()) return 0;
	int n = 0;
	for (;;) {
		int best = -1;
		unsigned long bs = 0;
		for (int i = 0; i < (int)A68A_REQS; i++) {
			unsigned long q = mc->req[i].seq;
			if (q && q != mh->ans_seq[i] && (best < 0 || (long)(q - bs) < 0)) { best = i; bs = q; }
		}
		if (best < 0 || n >= 48) break;           /* (the rest at the next turn: the Mac's own events go on) */
		volatile struct a68a_mreq *r = &mc->req[best];
		unsigned long sl = r->slot, len = r->len, op = r->op;
		short err = 0;
		if (sl >= A68A_MEDIA || !md[sl].open || (unsigned long)md[sl].id != r->id || len > A68A_XFER_MAX) err = -51;   /* rfNumErr */
		else if (op == A68A_R_WRITE && md[sl].ro) err = -61;                                                       /* wrPermErr */
		else {
			unsigned char pb[PB_SIZE];
			memset(pb, 0, sizeof pb);
			W(pb, PB_REF) = md[sl].ref;
			P(pb, PB_BUF) = (void *)(card + A68A_OFF_XFER + (unsigned long)best * A68A_XFER_MAX);
			L(pb, PB_REQ) = (long)len; W(pb, PB_POSMODE) = fsFromStart; L(pb, PB_POSOFF) = (long)r->off;
			err = op == A68A_R_READ ? fm_Read(pb) : op == A68A_R_WRITE ? fm_Write(pb) : -50;
			if (!err && (unsigned long)L(pb, PB_ACT) != len) err = -39;       /* eofErr: the file shorter than the card thinks */
			if (!err && op == A68A_R_WRITE) md[sl].wrote = 1;
		}
		mh->ans_err[best] = err;
		mh->ans_seq[best] = bs;
		n++;
	}
	if (n) last_served = LM_TICKS;
	for (int s = 0; s < (int)A68A_MEDIA; s++) {
		if (!md[s].open) continue;
		if (md[s].wrote && !n) {               /* a turn without requests after writes: the file's blocks to its disk */
			unsigned char pb[PB_SIZE];
			memset(pb, 0, sizeof pb);
			W(pb, PB_REF) = md[s].ref;
			fm_FlushFile(pb);
			md[s].wrote = 0;
		}
		if (mc->ms[s].id != (unsigned long)md[s].id) continue;
		unsigned long st = mc->ms[s].state;
		if (st != A68A_MS_NONE) md[s].seen = 1;
		if (st == A68A_MS_REFUSED || (st == A68A_MS_NONE && md[s].seen)) {
			if (md[s].wrote) { unsigned char pb[PB_SIZE]; memset(pb, 0, sizeof pb); W(pb, PB_REF) = md[s].ref; fm_FlushFile(pb); }
			file_close(s);
			if (st == A68A_MS_REFUSED && !notice_due) { say((const char *)blk->message, 0, 0); notice_due = 1; }
		}
	}
	return n;
}
long disks_sleep(long dflt)
{
	if (!any_open()) return dflt;
	return LM_TICKS - last_served < 60 ? 0 : 1;
}
/* every disk back to the Mac (the card writes back what changed and lets go), then the files closed */
void disks_quit(void)
{
	if (!mh) return;
	if (ready() && any_open()) {
		command(A68A_C_MACEJECT, A68A_UNIT_AUTO, 0);
		EventRecord ev;
		unsigned long t0 = LM_TICKS;
		while (any_open() && LM_TICKS - t0 < 15 * 60) {
			beat();
			if (!disks_serve()) WaitNextEvent(0, &ev, 1, 0);
		}
	}
	for (int s = 0; s < (int)A68A_MEDIA; s++) if (md[s].open) file_close(s);
	mh->magic = 0;
}

/* ---- Standard File ------------------------------------------------------------------------------------------- */
int c_sf_filter(void *pb);
int c_sf_filter(void *pb) { return !wanted((const unsigned char *)P(pb, PB_NAME), L(pb, PB_BUF)); }
int c_sf_dfilter(void *d, EventRecord *ev, short *item);
int c_sf_dfilter(void *d, EventRecord *ev, short *item)
{
	(void)d; (void)ev; (void)item;
	beat();
	disks_serve();                          /* the Apple's disks go on while the dialog is open */
	return 0;
}
static void insert_dialog(unsigned long unit)
{
	SFReply r;
	Point where = { 60, 82 };
	memset(&r, 0, sizeof r);
	SFPGetFile(pt_long(where), pstr(pbuf, ""), (void *)sf_filter_stub, -1, 0, 0, &r, -4000, (void *)sf_dfilter_stub, 4);
	if (r.good) insert_file(r.vRefNum, 0, r.fName, r.fType, unit);
}

/* ---- documents from the Finder: dropped on the icon, opened with the program ----------------------------------- */
#define DOCS_MAX 12
static struct { short vref; long dir; unsigned char name[64]; long type; } docs[DOCS_MAX];
static int ndocs;
static void doc_add(short vref, long dir, const unsigned char *name, long type)
{
	if (ndocs >= DOCS_MAX) return;
	docs[ndocs].vref = vref; docs[ndocs].dir = dir; docs[ndocs].type = type;
	memcpy(docs[ndocs].name, name, (unsigned long)(name[0] < 63 ? name[0] : 63) + 1);
	docs[ndocs].name[0] = name[0] < 63 ? name[0] : 63;
	ndocs++;
}
/* those given together: 5.25" images fill drive 1, drive 2, then (sector images) the HDs; block images HD1 to HD4 */
static void docs_open(void)
{
	int used[A68A_UNITS] = { 0, 0, 0, 0, 0, 0 }, n = ndocs, refused = 0;
	ndocs = 0;
	for (int i = 0; i < n; i++) {
		int s = file_open(docs[i].vref, docs[i].dir, docs[i].name, docs[i].type);
		if (s < 0) { notice(notice_text); continue; }
		unsigned k = slot_kind(s), u = A68A_UNITS;
		static const unsigned char o525[] = { A68A_UNIT_D1, A68A_UNIT_D2, A68A_UNIT_HD1, A68A_UNIT_HD2, A68A_UNIT_HD3, A68A_UNIT_HD4 };
		for (unsigned j = k == A68A_KIND_525 ? 0 : 2; j < A68A_UNITS; j++) {
			if (j >= 2 && k == A68A_KIND_525 && slot_nibbles(s)) break;
			if (!used[o525[j]]) { u = o525[j]; break; }
		}
		if (u == A68A_UNITS) { file_close(s); refused++; continue; }
		used[u] = 1;
		if (slot_insert(s, u) < 0) notice(notice_text);
	}
	if (refused) notice("Some of the disks did not fit: two 5.25\" drives and four hard disks (HD1-HD4) at most.");
}
/* at launch, the Finder's files (System 6, or System 7 for a program not told of by an Apple event): AppParmHandle,
   a message, a count, then each file's working directory, type, version and name (padded to an even length) */
static void finder_files(void)
{
	unsigned char **h = LM_APPPARMHANDLE, *p;
	if (!h || !*h) return;
	p = *h;
	short msg = (short)(p[0] << 8 | p[1]), n = (short)(p[2] << 8 | p[3]);
	if (msg != 0 || n < 1) return;
	p += 4;
	for (short i = 0; i < n && i < DOCS_MAX; i++) {
		short vref = (short)(p[0] << 8 | p[1]);
		long type = (long)p[2] << 24 | (long)p[3] << 16 | (long)p[4] << 8 | p[5];
		doc_add(vref, 0, p + 8, type);
		p += 8 + ((p[8] + 2) & ~1);
	}
	docs_open();
}
/* Apple events (System 7): Aurora68 Apple II is high-level-event aware (its SIZE resource), so the Finder sends it Open
   Documents for files dropped on its icon or opened with it, whether it runs already or is started by them */
short c_ae_event(const void *ev, long refcon);
short c_ae_event(const void *ev, long refcon)
{
	if (refcon == TYPE('q', 'u', 'i', 't')) { quit_asked = 1; return 0; }
	if (refcon != TYPE('o', 'd', 'o', 'c') && refcon != TYPE('p', 'd', 'o', 'c')) return 0;
	struct { long type; void *h; } list = { 0, 0 };
	if (AEGetParamDesc(ev, TYPE('-', '-', '-', '-'), TYPE('l', 'i', 's', 't'), &list)) return -1701;   /* errAEDescNotFound */
	long n = 0;
	AECountItems(&list, &n);
	for (long i = 1; i <= n; i++) {
		struct { short vref; long dir; unsigned char name[64]; } fss;
		long kw, typ, got;
		if (AEGetNthPtr(&list, i, TYPE('f', 's', 's', ' '), &kw, &typ, &fss, (long)sizeof fss, &got)) continue;
		/* the file's type, for an image without its extension */
		unsigned char pb[PB_SIZE];
		memset(pb, 0, sizeof pb);
		P(pb, PB_NAME) = fss.name; W(pb, PB_VREF) = fss.vref; L(pb, PB_DIRID) = fss.dir;
		doc_add(fss.vref, fss.dir, fss.name, fm_GetCatInfo(pb) ? 0 : L(pb, PB_BUF));
	}
	AEDisposeDesc(&list);
	return 0;
}
void disks_hle(EventRecord *ev)
{
	if (ev->what != kHighLevelEvent || !ae_ok) return;
	AEProcessAppleEvent(ev);
	if (ndocs) docs_open();
}
int disks_quit_asked(void) { return quit_asked; }

/* ---- the Apple II Disks folder: beside the program, found from the program's own file -------------------------- */
#define SUB_FIRST 160                       /* the submenus' IDs: hierarchical menus are 1-235 */
#define SUB_MAX   40
#define FE_MAX    240
#define FOLDER_DEPTH 4
static const unsigned char folder_name[] = "\x0E" "Apple II Disks";
struct fent { short vref, menu, item; char isdir; long dir, type; unsigned char name[32]; };
static struct fent fe[FE_MAX];
static int nfe, nsub;
static void *subm[SUB_MAX];
static short app_vref;
static long app_dir, a2d_dir;
static int have_folder, app_known;
static void app_folder(void)
{
	unsigned char pb[PB_SIZE], nm[64];
	memset(pb, 0, sizeof pb);
	P(pb, PB_NAME) = nm; W(pb, PB_REF) = LM_CURAPREFNUM;
	if (fm_GetFCBInfo(pb)) return;
	app_vref = W(pb, PB_FCBVREF); app_dir = L(pb, PB_FCBPAR); app_known = 1;
}
static void scan(short vref, long dir, short id, int depth)
{
	void *m = NewMenu(id, pstr(pbuf, "x"));
	subm[id - SUB_FIRST] = m;
	short items = 0;
	for (short idx = 1; idx < 500 && nfe < FE_MAX; idx++) {
		unsigned char pb[PB_SIZE], nm[64];
		memset(pb, 0, sizeof pb);
		nm[0] = 0;
		P(pb, PB_NAME) = nm; W(pb, PB_VREF) = vref; W(pb, PB_MISC) = idx; L(pb, PB_DIRID) = dir;
		if (fm_GetCatInfo(pb)) break;
		if (W(pb, PB_FLAGS) & 0x4000) continue;            /* invisible */
		int isdir = (B(pb, PB_ATTRIB) & 0x10) != 0;
		if (nm[0] > 31) nm[0] = 31;
		if (isdir ? depth >= FOLDER_DEPTH || nsub >= SUB_MAX : !wanted(nm, L(pb, PB_BUF))) continue;
		struct fent *f = &fe[nfe++];
		f->vref = vref; f->menu = id; f->item = ++items; f->isdir = (char)isdir;
		f->dir = isdir ? L(pb, PB_DIRID) : dir; f->type = isdir ? 0 : L(pb, PB_BUF);
		memcpy(f->name, nm, (unsigned long)nm[0] + 1);
		AppendMenu(m, pstr(pbuf, "x"));
		SetItem(m, items, f->name);                        /* SetItem: the name's characters as they are */
		if (isdir) {
			short sub = (short)(SUB_FIRST + nsub++);
			SetItemCmd(m, items, 0x1B);                     /* hierMenuCmd: the folder's own submenu */
			SetItemMark(m, items, sub);
			scan(vref, f->dir, sub, depth + 1);
		}
	}
	if (!items) { AppendMenu(m, pstr(pbuf, "x")); SetItem(m, 1, pstr(pbuf, "No disk images")); DisableItem(m, 1); }
	InsertMenu(m, -1);
}
static void folder_scan(void)
{
	for (int i = 0; i < nsub; i++) if (subm[i]) { DeleteMenu((short)(SUB_FIRST + i)); DisposeMenu(subm[i]); subm[i] = 0; }
	nsub = 0; nfe = 0; have_folder = 0;
	if (!app_known) return;
	unsigned char pb[PB_SIZE], nm[64];
	memset(pb, 0, sizeof pb);
	memcpy(nm, folder_name, sizeof folder_name);
	P(pb, PB_NAME) = nm; W(pb, PB_VREF) = app_vref; L(pb, PB_DIRID) = app_dir;
	if (fm_GetCatInfo(pb) || !(B(pb, PB_ATTRIB) & 0x10)) return;
	a2d_dir = L(pb, PB_DIRID); have_folder = 1;
	nsub = 1;
	scan(app_vref, a2d_dir, SUB_FIRST, 1);
}
static void fe_chosen(short menu, short item)
{
	for (int i = 0; i < nfe; i++) {
		struct fent *f = &fe[i];
		if (f->menu != menu || f->item != item || f->isdir) continue;
		for (int s = 0; s < (int)A68A_MEDIA; s++)             /* in a drive already: out */
			if (md[s].open && md[s].vref == f->vref && md[s].dir == f->dir && !memcmp(md[s].name, f->name, (unsigned long)f->name[0] + 1)) {
				if (mc->ms[s].state == A68A_MS_IN) command(A68A_C_MACEJECT, (unsigned long)s, 0);
				return;
			}
		insert_file(f->vref, f->dir, f->name, f->type, A68A_UNIT_AUTO);
		return;
	}
}
/* Show Apple II Disks Folder: made if it is not there, then opened in the Finder. System 7.1's Finder takes no 'aevt'
   'odoc' (docs/08 item 90: Aurora68 Menu Extras met it), so the event the Apple menu sends it, 'FNDR' 'sope' (open the
   selection: the folder's parent as the direct object, the folder as the selection, both aliases), then the Finder is
   brought to the front to show the window */
static void show_folder(void)
{
	if (!app_known) return;
	if (!have_folder) {
		unsigned char pb[PB_SIZE], nm[64];
		memset(pb, 0, sizeof pb);
		memcpy(nm, folder_name, sizeof folder_name);
		P(pb, PB_NAME) = nm; W(pb, PB_VREF) = app_vref; L(pb, PB_DIRID) = app_dir;
		fm_DirCreate(pb);
		folder_scan();
	}
	if (!ae_ok) return;
	typedef struct { long type; void *h; } AEDesc;
	typedef struct { short vref; long dir; unsigned char name[64]; } FSSpec;
	AEDesc target = { 0, 0 }, ev = { 0, 0 }, reply = { 0, 0 }, list = { 0, 0 }, parent = { 0, 0 }, item = { 0, 0 };
	FSSpec fol, par;
	memset(&fol, 0, sizeof fol); memset(&par, 0, sizeof par);
	fol.vref = app_vref; fol.dir = app_dir;
	memcpy(fol.name, folder_name, sizeof folder_name);
	unsigned char pb[PB_SIZE];                          /* the program's folder as its own parent's item */
	memset(pb, 0, sizeof pb);
	P(pb, PB_NAME) = par.name; W(pb, PB_VREF) = app_vref; W(pb, PB_MISC) = -1; L(pb, PB_DIRID) = app_dir;
	if (fm_GetCatInfo(pb)) return;
	par.vref = app_vref; par.dir = L(pb, 100);          /* ioDrParID */
	long finder = TYPE('M', 'A', 'C', 'S');
	short e = AECreateDesc(TYPE('s', 'i', 'g', 'n'), &finder, 4, &target);
	if (!e) e = AECreateAppleEvent(TYPE('F', 'N', 'D', 'R'), TYPE('s', 'o', 'p', 'e'), &target, -1, 0, &ev);
	FSSpec *specs[2] = { &par, &fol };
	AEDesc *descs[2] = { &parent, &item };
	for (int i = 0; i < 2 && !e; i++) {
		void **a = 0;
		e = NewAlias(0, specs[i], &a);
		if (!e) { a2_hlock(a); e = AECreateDesc(TYPE('a', 'l', 'i', 's'), *a, *(unsigned short *)((char *)*a + 4), descs[i]); a2_disposehandle(a); }
	}
	if (!e) e = AECreateList(0, 0, 0, &list);
	if (!e) e = AEPutDesc(&list, 0, &item);
	if (!e) e = AEPutParamDesc(&ev, TYPE('-', '-', '-', '-'), &parent);
	if (!e) e = AEPutParamDesc(&ev, TYPE('f', 's', 'e', 'l'), &list);
	if (!e) e = AESend(&ev, &reply, 0x21 /* kAENoReply | kAECanInteract */, 0, -1, 0, 0);
	if (!e) {                                           /* the Finder to the front, to show the window it opens */
		struct { long length; unsigned char *name; unsigned long psn[2]; unsigned long type, signature, mode; void *location;
		         unsigned long size, freemem, launcher[2], launch, active; void *spec; } info;
		unsigned long psn[2] = { 0, 0 };
		while (!GetNextProcess(psn)) {
			memset(&info, 0, sizeof info);
			info.length = sizeof info;
			if (!GetProcessInformation(psn, &info) && info.signature == (unsigned long)TYPE('M', 'A', 'C', 'S')) { SetFrontProcess(psn); break; }
		}
	}
	AEDesc *all[] = { &target, &ev, &reply, &list, &parent, &item };
	for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) if (all[i]->h) AEDisposeDesc(all[i]);
}

/* ---- the Disks menu ------------------------------------------------------------------------------------------ */
static const char *const unit_label[A68A_UNITS] = { "Drive 1: ", "Drive 2: ", "HD1: ", "HD2: ", "HD3: ", "HD4: " };
static unsigned long disks_seq = ~0u, macc_seq = ~0u;
static int ncard;                                  /* the card images' items shown (kept: no DeleteItem before System 7) */
static signed char card_item[A68A_DISKS_MAX];      /* item - DK_CARD1 -> the card's image index */
static int is_sub(short menu) { return menu >= SUB_FIRST && menu < SUB_FIRST + nsub; }
int disks_is_menu(short menu) { return menu == M_DISKS_ID || is_sub(menu); }
static char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_name(char *p, const volatile char *n)
{
	unsigned k = 0;
	while (k < 50 && n[k]) { *p++ = n[k]; k++; }
	if (!k) p = put(p, "no name");
	return p;
}
void disks_menu_build(void)
{
	AppendMenu(m_disks, pstr(pbuf, "Insert Disk\xC9/I;(-;1;2;3;4;5;6;(-;Apple II Disks;Show Apple II Disks Folder;Look for Disks Again;(-"));
}
void disks_menu(void)
{
	if (!card) return;
	volatile struct a68a_disks *dk = (volatile struct a68a_disks *)(card + A68A_OFF_DISKS);
	char t[100], *p;
	if (dk->seq != disks_seq || (mc && mc->seq != macc_seq)) {
		disks_seq = dk->seq; macc_seq = mc ? mc->seq : 0;
		for (unsigned u = 0; u < A68A_UNITS; u++) {        /* the units: what is in each, or Insert Disk... */
			long k = dk->unit[u];
			p = put(t, unit_label[u]);
			if (k >= 0 && k < (long)A68A_DISKS_MAX && dk->d[k].name[0]) {
				p = put_name(p, dk->d[k].name);
				if (dk->d[k].flags & A68A_DF_MAC) {
					int reading = 0;
					for (int s = 0; s < (int)A68A_MEDIA && mc; s++) if (md[s].open && mc->ms[s].id == (unsigned long)md[s].id && mc->ms[s].unit == u && mc->ms[s].state == A68A_MS_LOADING) reading = 1;
					p = put(p, reading ? " (from the Mac: reading)" : " (from the Mac)");
				}
				if (dk->d[k].flags & A68A_DF_RO) p = put(p, ", read only");
			} else p = put(p, "Insert Disk\xC9");
			*p = 0;
			SetItem(m_disks, (short)(DK_UNIT1 + u), pstr(pbuf, t));
			CheckItem(m_disks, (short)(DK_UNIT1 + u), k >= 0);
		}
		/* the card's own images (the SD card's root): each with its kind and, while it is in a drive, the drive, checked */
		unsigned n = dk->count < A68A_DISKS_MAX ? dk->count : A68A_DISKS_MAX, shown = 0;
		for (unsigned i = 0; i < n; i++) {
			if (!dk->d[i].name[0] || !dk->d[i].kind || (dk->d[i].flags & A68A_DF_MAC)) continue;
			p = put_name(t, dk->d[i].name);
			p = put(p, dk->d[i].kind == A68A_KIND_525 ? " - 5.25\"" : " - block");
			if (dk->d[i].unit >= 0 && dk->d[i].unit < (long)A68A_UNITS) { p = put(p, ", in "); p = put(p, unit_label[dk->d[i].unit]); p -= 2; }
			if (dk->d[i].flags & A68A_DF_RO) p = put(p, ", read only");
			*p = 0;
			short it = (short)(DK_CARD1 + shown);
			if ((int)shown >= ncard) { AppendMenu(m_disks, pstr(pbuf, "x")); ncard = (int)shown + 1; }
			SetItem(m_disks, it, pstr(pbuf, t));
			EnableItem(m_disks, it);
			CheckItem(m_disks, it, dk->d[i].unit >= 0);
			card_item[shown++] = (signed char)i;
		}
		for (unsigned i = shown; i < (unsigned)ncard; i++) { SetItem(m_disks, (short)(DK_CARD1 + i), pstr(pbuf, " ")); DisableItem(m_disks, (short)(DK_CARD1 + i)); CheckItem(m_disks, (short)(DK_CARD1 + i), 0); }
		for (unsigned i = shown; i < A68A_DISKS_MAX; i++) card_item[i] = -1;
	}
	if (ready()) EnableItem(m_disks, DK_INSERT); else DisableItem(m_disks, DK_INSERT);
	if (have_folder) {
		SetItem(m_disks, DK_FOLDER, folder_name);
		SetItemCmd(m_disks, DK_FOLDER, 0x1B); SetItemMark(m_disks, DK_FOLDER, SUB_FIRST);
		EnableItem(m_disks, DK_FOLDER);
	} else {
		SetItemCmd(m_disks, DK_FOLDER, 0); SetItemMark(m_disks, DK_FOLDER, 0);
		SetItem(m_disks, DK_FOLDER, pstr(pbuf, "Apple II Disks (no such folder beside the program)"));
		DisableItem(m_disks, DK_FOLDER);
	}
	if (app_known && (ae_ok || !have_folder)) EnableItem(m_disks, DK_SHOW); else DisableItem(m_disks, DK_SHOW);
	SetItem(m_disks, DK_SHOW, pstr(pbuf, have_folder ? "Show Apple II Disks Folder" : "Make an Apple II Disks Folder"));
	for (int i = 0; i < nfe; i++) {                     /* the folder's images in a drive: checked */
		if (fe[i].isdir) continue;
		int in = 0;
		for (int s = 0; s < (int)A68A_MEDIA; s++)
			if (md[s].open && md[s].vref == fe[i].vref && md[s].dir == fe[i].dir && !memcmp(md[s].name, fe[i].name, (unsigned long)fe[i].name[0] + 1)) in = 1;
		CheckItem(subm[fe[i].menu - SUB_FIRST], fe[i].item, in);
	}
}
void disks_chosen(short menu, short item)
{
	if (is_sub(menu)) { fe_chosen(menu, item); return; }
	if (menu != M_DISKS_ID || !card) return;
	volatile struct a68a_disks *dk = (volatile struct a68a_disks *)(card + A68A_OFF_DISKS);
	if (item == DK_INSERT) { insert_dialog(A68A_UNIT_AUTO); return; }
	if (item >= DK_UNIT1 && item < DK_UNIT1 + (short)A68A_UNITS) {
		unsigned u = (unsigned)(item - DK_UNIT1);
		if (dk->unit[u] >= 0) command(A68A_C_DETACH, 0, u);  /* a Mac's disk: written back to its file, then let go */
		else insert_dialog(u);
		return;
	}
	if (item == DK_SHOW) { show_folder(); return; }
	if (item == DK_AGAIN) { command(A68A_C_RESCAN, 0, 0); folder_scan(); return; }
	if (item >= DK_CARD1 && item - DK_CARD1 < (short)A68A_DISKS_MAX && card_item[item - DK_CARD1] >= 0) {
		unsigned i = (unsigned)card_item[item - DK_CARD1];
		unsigned long seq = dk->seq;
		command(dk->d[i].unit >= 0 ? A68A_C_DETACH : A68A_C_ATTACH, i, A68A_UNIT_AUTO);
		if (dk->seq == seq && blk->message[0]) notice(blk->message);   /* nothing changed: the card says why */
	}
}
void disks_resume(void) { folder_scan(); }

/* ---- the start ----------------------------------------------------------------------------------------------- */
void disks_idle(void);
void disks_idle(void)
{
	disks_serve();
	if (notice_due) { notice_due = 0; notice(notice_text); }
}
void disks_init(void)
{
	app_folder();
	long r = 0;
	if (LM_SYSVERSION >= 0x0700 && !a2_gestalt(TYPE('e', 'v', 'n', 't'), &r) && (r & 1)) {
		ae_ok = !AEInstallEventHandler(TYPE('a', 'e', 'v', 't'), TYPE('o', 'a', 'p', 'p'), (void *)ae_handler_stub, TYPE('o', 'a', 'p', 'p'), 0);
		AEInstallEventHandler(TYPE('a', 'e', 'v', 't'), TYPE('o', 'd', 'o', 'c'), (void *)ae_handler_stub, TYPE('o', 'd', 'o', 'c'), 0);
		AEInstallEventHandler(TYPE('a', 'e', 'v', 't'), TYPE('p', 'd', 'o', 'c'), (void *)ae_handler_stub, TYPE('p', 'd', 'o', 'c'), 0);
		AEInstallEventHandler(TYPE('a', 'e', 'v', 't'), TYPE('q', 'u', 'i', 't'), (void *)ae_handler_stub, TYPE('q', 'u', 'i', 't'), 0);
	}
	folder_scan();
	if (!card) return;
	mh = (volatile struct a68a_mac *)(card + A68A_OFF_MAC);
	mc = (volatile struct a68a_macc *)(card + A68A_OFF_MACC);
	for (unsigned i = 0; i < sizeof *mh / 4; i++) ((volatile unsigned long *)mh)[i] = 0;
	session = (LM_TICKS << 8 | 0xA2) & 0x7FFFFFFF;
	mh->session = session;
	next_id = (long)((session & 0x07FFFFFF) << 4) + 1;   /* ids of this run unlike any of an earlier one */
	mh->magic = A68A_MAC_MAGIC;
	unsigned long t0 = LM_TICKS;               /* the card takes the session at its next turn */
	while (mc->session != session && LM_TICKS - t0 < 60) beat();
	if (!ae_ok) finder_files();                /* System 7 sends the Finder's files as an Open Documents event instead */
}
