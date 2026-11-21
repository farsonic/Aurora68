/* SPDX-License-Identifier: MPL-2.0
 * mac/piagent/agent.c - the Aurora68 Agent: files and programs on the Mac for the Pi's remote control.
 *
 * Backstory. The remote control (docs/13) gave the owner's MacBook the Mac's screen, mouse, keyboard
 * and log, all of it served by the kernel without any program on the Mac. The owner then asked for
 * "a command line on the client for jobs like this. Screen shots. Push files etc" (21 November 2026).
 * Files and programs are different: only the Mac's File Manager knows where a file's bytes are on a
 * disk, and only its Process Manager can start a program. So this INIT runs on the 68k and does those
 * jobs for the kernel, which keeps the HTTP side (emu68/src/psmac/psmac_files.c).
 *
 * When it works. A synchronous File Manager call is not allowed at interrupt time (it would wait for
 * the File Manager's queue, which the interrupted code may hold), and neither are the Process Manager's
 * calls. Application time is: so the agent hooks the GetNextEvent filter (jGNEFilter),
 * which the Event Manager calls at the end of every GetNextEvent and WaitNextEvent, in whatever program
 * asked for an event, on System 6 (Finder alone or MultiFinder) and System 7 alike; the WiFi menu uses
 * the same hook (mac/wifimenu). Even there the File Manager may be in the middle of a call: a dialog
 * asking for a disk is shown from inside one, and its ModalDialog calls GetNextEvent. So nothing is done
 * while FSBusy ($360) says the File Manager is at work; the request waits for the next event. A VBL task
 * counts a heartbeat, so that the kernel can tell an agent that is installed from one that is not, and
 * under System 7 wakes the program in front with WakeUpProcess when a request waits, so that a program
 * asleep in WaitNextEvent comes round within a tick instead of at the end of its sleep.
 *
 * No state between requests. The filter runs in other programs' process contexts, and System 7 closes
 * the files a program opened when it quits, so a file is opened and closed again within each request;
 * the kernel keeps everything else (where a transfer has got to, the MacBinary header). Every request
 * is a full path ("Volume:Folder:File", Mac Roman); bytes go straight between the file and the card's
 * transfer buffer (psmb.h, struct psmb_agent). Memory the Toolbox allocates for a request (Apple events,
 * an alias) comes from the system heap and is given back before the filter returns.
 *
 * Written 21 November 2026 for System 6.0.8 and 7.x, 68020 to 68040, 24- and 32-bit addressing.
 */
#include "toolbox.h"
#include "psmb.h"

_Static_assert(sizeof(struct psmb_agent) == 0x520 && __builtin_offsetof(struct psmb_agent, path) == 0x80 &&
               __builtin_offsetof(struct psmb_agent, magic) == 0x380, "psmb_agent");
_Static_assert(sizeof(struct psmb_agent_entry) == 80 && sizeof(struct psmb_agent_proc) == 72, "psmb_agent records");

/* ---- the hooks (entry.s) and the calls it glues ---- */
void gne_filter(void), vbl_task(void);
void *old_gne;
short pb_close(void *), pb_read(void *), pb_write(void *), pb_geteof(void *), pb_seteof(void *), pb_flushvol(void *),
      pb_setvol(void *), pb_hopen(void *), pb_hgetvinfo(void *), pb_hcreate(void *), pb_hdelete(void *), pb_hopenrf(void *),
      pb_hrename(void *), pb_hgetfinfo(void *), pb_hsetfinfo(void *), pb_openwd(void *), pb_catmove(void *),
      pb_dircreate(void *), pb_getcatinfo(void *), pb_setcatinfo(void *), pb_catsearch(void *);
typedef struct { unsigned long hi, lo; } PSN;
short pm_current(PSN *), pm_next(PSN *), pm_front(PSN *), pm_setfront(const PSN *), pm_wakeup(const PSN *),
      pm_info(const PSN *, void *info);
short mac_launch(void *block);
typedef struct { unsigned long type; void **data; } AEDesc;
short ae_create_desc(unsigned long type, const void *data, long size, AEDesc *result), ae_put_desc(AEDesc *list, long index, const AEDesc *d),
      ae_put_param_desc(AEDesc *ev, unsigned long key, const AEDesc *d), ae_coerce_desc(const AEDesc *d, unsigned long type, AEDesc *result),
      ae_dispose_desc(AEDesc *d), ae_create_list(AEDesc *list), ae_create_event(unsigned long cls, unsigned long id, const AEDesc *target, AEDesc *ev),
      ae_send(const AEDesc *ev, AEDesc *reply, long mode);
short new_alias(const void *from, const void *target, void ***alias);
void mac_hlock(void *h), mac_dispose_handle(void *h), mac_setzone(void *zone);
short mac_vinstall(void *task);
void *get_tool_trap(int trap);
int get_sr(void);
void set_sr(int sr);

/* ---- low memory ---- */
#define LM_JGNEFILTER (*(void **volatile *)0x29A)
#define LM_FSBUSY     (*(volatile short *)0x360)          /* the File Manager is inside a call */
#define LM_BOOTDRIVE  (*(volatile short *)0x210)          /* the startup folder's working directory (or volume) */
#define LM_CURAPNAME  ((const unsigned char *)0x910)      /* the current application's name, Pascal */
#define LM_SYSZONE    (*(void **)0x2A6)
#define LM_THEZONE    (*(void **)0x118)

/* ---- errors of the File Manager that the agent looks at ---- */
#define fnfErr   (-43)
#define dupFNErr (-48)
#define eofErr   (-39)
#define nsvErr   (-35)
#define dirNFErr (-120)
#define fsRdPerm 1
#define fsWrPerm 2
#define fsFromStart 1

/* ---- the File Manager's HFS parameter block: every variant the agent uses over the same bytes ---- */
typedef struct {
	void *qLink;
	short qType, ioTrap;
	void *ioCmdAddr, *ioCompletion;
	short ioResult;
	const unsigned char *ioNamePtr;
	short ioVRefNum;                                            /* 24 bytes so far */
	union {
		struct { short ioRefNum; char ioVersNum, ioPermssn; void *ioMisc; void *ioBuffer; long ioReqCount, ioActCount;
		         short ioPosMode; long ioPosOffset; } io;           /* I/O: Read, Write, SetEOF, Rename (ioMisc the new name) */
		struct { short ioFRefNum; char ioFVersNum, filler1; short ioFDirIndex; unsigned char ioFlAttrib, ioACUser;
		         unsigned long fdType, fdCreator; unsigned short fdFlags; unsigned long fdLocation; short fdFldr;
		         long ioDirID; unsigned short ioFlStBlk; unsigned long ioFlLgLen, ioFlPyLen; short ioFlRStBlk;
		         unsigned long ioFlRLgLen, ioFlRPyLen, ioFlCrDat, ioFlMdDat, ioFlBkDat; char ioFlXFndrInfo[16];
		         long ioFlParID, ioFlClpSiz; } f;                   /* GetCatInfo (a folder's: ioDirID its ID, ioFlStBlk its valence), HOpen, HCreate ... */
		struct { long filler2; short ioVolIndex; unsigned long ioVCrDate, ioVLsMod; unsigned short ioVAtrb, ioVNmFls, ioVBitMap,
		         ioAllocPtr, ioVNmAlBlks; unsigned long ioVAlBlkSiz, ioVClpSiz; unsigned short ioAlBlSt; unsigned long ioVNxtCNID;
		         unsigned short ioVFrBlk, ioVSigWord; short ioVDrvInfo, ioVDRefNum, ioVFSID; unsigned long ioVBkUp;
		         unsigned short ioVSeqNum; unsigned long ioVWrCnt, ioVFilCnt, ioVDirCnt; long ioVFndrInfo[8]; } v;   /* HGetVInfo */
		struct { long filler1; const unsigned char *ioNewName; long filler2; long ioNewDirID; long filler3[2]; long ioDirID; } m;   /* CatMove */
		struct { short filler1, ioWDIndex; long ioWDProcID; short ioWDVRefNum, filler2[7]; long ioWDDirID; } wd;   /* OpenWD */
		char pad[104];
	} u;
} PB;
_Static_assert(__builtin_offsetof(PB, u.f.ioDirID) == 48 && __builtin_offsetof(PB, u.f.ioFlCrDat) == 72 &&
               __builtin_offsetof(PB, u.f.ioFlParID) == 100 && __builtin_offsetof(PB, u.v.ioVAlBlkSiz) == 48 &&
               __builtin_offsetof(PB, u.v.ioVFrBlk) == 62 && __builtin_offsetof(PB, u.m.ioNewDirID) == 36 &&
               __builtin_offsetof(PB, u.m.ioDirID) == 48 && __builtin_offsetof(PB, u.wd.ioWDDirID) == 48 &&
               __builtin_offsetof(PB, u.io.ioPosOffset) == 46 && sizeof(PB) == 128, "PB");
typedef struct { short vRefNum; long parID; unsigned char name[64]; } FSSpec;
_Static_assert(sizeof(FSSpec) == 70, "FSSpec");

static PB pb, pb2;                                   /* the filter is never inside itself (busy), so one of each will do */
static void pb_clear(PB *p) { memset(p, 0, sizeof *p); }

/* ---- the card ---- */
static volatile unsigned char *win(void)
{
	/* the window by the mode the processor is in now: a VBL can come while code has switched to 32-bit */
	return (volatile unsigned char *)(LM_MMU32BIT ? 0xF9000000UL : 0x00900000UL);
}
static volatile struct psmb_agent *blk(void) { return (volatile struct psmb_agent *)(win() + PSMB_OFF_AGENT); }
static unsigned char *xbuf(void) { return (unsigned char *)(win() + PSMB_OFF_AGENTBUF); }

/* a line in the Pi's log through the INFO service, as the drivers write theirs ([MAC] A68 Agent: ...) */
static char logline[128];
static void pi_log(const char *a, const char *b)
{
	static const char who[] = "A68 Agent: ";
	unsigned long n = 0, k, req;
	for (k = 0; who[k]; k++) logline[n++] = who[k];
	for (k = 0; a[k] && n < sizeof logline - 1; k++) logline[n++] = a[k];
	for (k = 0; b && b[k] && n < sizeof logline - 1; k++) logline[n++] = b[k];
	volatile unsigned char *c = win() + 0x1000;                 /* the INFO service's command block */
	volatile unsigned long *rq = (volatile unsigned long *)c, *dn = (volatile unsigned long *)(c + 4);
	int sr = get_sr();
	set_sr(sr | 0x0700);
	req = *rq;
	for (k = 0; *dn != req && k < 2000000; k++) ;
	*(volatile unsigned short *)(c + 8) = 2;                    /* PSMB_INFO_LOG */
	*(volatile unsigned long *)(c + 28) = (unsigned long)logline;
	*(volatile unsigned long *)(c + 32) = n;
	*rq = req + 1;
	for (k = 0; *dn != req + 1 && k < 4000000; k++) ;
	set_sr(sr);
}

/* a line in the log with a number after it */
static void log_err(const char *what, long v)
{
	char m[16], *q = m + 15;
	unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
	*q = 0;
	do { *--q = (char)('0' + u % 10); u /= 10; } while (u);
	if (v < 0) *--q = '-';
	pi_log(what, q);
}

/* ---- the system ---- */
static unsigned long features;
static unsigned char startup[32];                    /* the startup volume's name, Pascal */
static PSN front_psn;                                /* the program in front, as last seen (the VBL task wakes it) */
static unsigned long woken_seq, woken_at;
static volatile int busy;                            /* inside a request: the filter called again from within (a dialog) */
static int sys7(void) { return (features & PSMB_AGX_PROCMGR) != 0; }
/* The Process Manager's trap (_OSDispatch) is put in when the Process Manager starts, after the INITs have
   run, and something may call GetNextEvent before that: until then its calls would be _Unimplemented's
   system error. Checked before each use; it costs two trap-table lookups. */
static int pm_ready(void) { return sys7() && get_tool_trap(0xA88F) != get_tool_trap(0xA89F); }

static void note(const char *s)
{
	volatile char *n = blk()->note;
	unsigned i = 0;
	for (; s[i] && i < 63; i++) n[i] = s[i];
	n[i] = 0;
}
static void note_type(const char *before, unsigned long t, const char *after)
{
	char b[64];
	unsigned n = 0;
	for (; *before && n < 40; before++) b[n++] = *before;
	b[n++] = '\'';
	for (int i = 24; i >= 0; i -= 8) { char c = (char)(t >> i); b[n++] = c >= 32 && c < 127 ? c : '?'; }
	b[n++] = '\'';
	for (; *after && n < 62; after++) b[n++] = *after;
	b[n] = 0;
	note(b);
}

/* ---- paths ---- */
static unsigned char full[256], full2[256], leaf[64], parent[256], name32[64];

static int ieq_byte(unsigned char a, unsigned char b) { if (a >= 'a' && a <= 'z') a -= 32; if (b >= 'a' && b <= 'z') b -= 32; return a == b; }
static int pstr_ieq(const unsigned char *a, const unsigned char *b)
{
	if (a[0] != b[0]) return 0;
	for (unsigned i = 1; i <= a[0]; i++) if (!ieq_byte(a[i], b[i])) return 0;
	return 1;
}
/* The request's path (a C string in the block, Mac Roman) as a full path name, Pascal, for the File
   Manager: ":rest" is the startup volume's, a name without a colon a volume ("Mac HD" is "Mac HD:"); a
   trailing colon after a folder goes (the File Manager takes "Vol:Folder" for the folder). Refused: an
   empty part ("a::b" climbs a folder in the File Manager's syntax), a name of more than 31 characters,
   more than 255 in all, a volume name beginning with a dot (the Device Manager's names). */
static int make_path(volatile const char *in, unsigned char *out)
{
	char p[256];
	unsigned n = 0, o = 0;
	for (; in[n] && n < 255; n++) p[n] = in[n];
	if (in[n]) return PSMB_AGE_BADPATH;
	p[n] = 0;
	if (!n) return PSMB_AGE_BADPATH;
	if (p[0] == ':') {
		for (unsigned i = 1; i <= startup[0]; i++) out[++o] = startup[i];
		if (n == 1) { out[++o] = ':'; out[0] = (unsigned char)o; return 0; }
	}
	int colon = 0;
	for (unsigned i = 0; i < n; i++) {
		if (o >= 254) return PSMB_AGE_BADPATH;
		out[++o] = (unsigned char)p[i];
		if (p[i] == ':') colon = 1;
	}
	if (!colon) out[++o] = ':';
	if (o > 1 && out[o] == ':') {                            /* "Vol:Folder:" is "Vol:Folder"; "Vol:" stays */
		unsigned c = 0;
		for (unsigned i = 1; i <= o; i++) if (out[i] == ':') c++;
		if (c > 1) o--;
	}
	out[0] = (unsigned char)o;
	if (out[1] == '.' || out[1] == ':') return PSMB_AGE_BADPATH;
	unsigned part = 0, first = 1;
	for (unsigned i = 1; i <= o; i++) {
		if (out[i] == ':') {
			if (!part || part > (first ? 27u : 31u)) return PSMB_AGE_BADPATH;
			part = 0; first = 0;
		} else part++;
	}
	if (part > 31) return PSMB_AGE_BADPATH;
	return 0;
}
/* the last name of a full path and the path of its folder ("Vol:" for something at the top) */
static void split(const unsigned char *path, unsigned char *dir, unsigned char *name)
{
	unsigned last = 0;
	for (unsigned i = 1; i <= path[0]; i++) if (path[i] == ':' && i < path[0]) last = i;
	for (unsigned i = 1; i <= last; i++) dir[i] = path[i];
	unsigned dl = last;
	if (dl > 1 && dir[dl] == ':') {                          /* keep the colon only after the volume */
		unsigned c = 0;
		for (unsigned i = 1; i <= dl; i++) if (dir[i] == ':') c++;
		if (c > 1) dl--;
	}
	dir[0] = (unsigned char)dl;
	unsigned n = 0;
	for (unsigned i = last + 1; i <= path[0] && n < 63; i++) if (path[i] != ':') name[++n] = path[i];
	name[0] = (unsigned char)n;
}
static int is_root(const unsigned char *path) { return path[path[0]] == ':'; }
/* the volume part of a full path, "Vol:", and its vRefNum */
static short vol_of(const unsigned char *path, short *vref)
{
	unsigned char v[32];
	unsigned n = 0;
	while (n < path[0] && path[n + 1] != ':' && n < 28) { v[n + 1] = path[n + 1]; n++; }
	v[++n] = ':';
	v[0] = (unsigned char)n;
	pb_clear(&pb2);
	pb2.ioNamePtr = v;
	pb2.u.v.ioVolIndex = -1;                                  /* by name */
	short e = pb_hgetvinfo(&pb2);
	*vref = pb2.ioVRefNum;
	return e;
}

/* the catalog's information about path: 0, fnfErr, dirNFErr ... (pb holds the answer) */
static short get_cat(const unsigned char *path)
{
	pb_clear(&pb);
	pb.ioNamePtr = path;
	short e = pb_getcatinfo(&pb);
	return e;
}
static int cat_is_dir(void) { return (pb.u.f.ioFlAttrib & 0x10) != 0; }

/* ---- the records ---- */
static void put_name(volatile char *dst, const unsigned char *p)
{
	unsigned n = p[0] > 31 ? 31 : p[0];
	for (unsigned i = 0; i < n; i++) dst[i] = (char)p[i + 1];
	for (unsigned i = n; i < 32; i++) dst[i] = 0;
}
static void fill_entry(volatile struct psmb_agent_entry *e, const unsigned char *name)
{
	unsigned long a = pb.u.f.ioFlAttrib;
	e->flags = a | (a & 0x10 ? PSMB_AGN_DIR : 0) | (a & 0x01 ? PSMB_AGN_LOCKED : 0) | (a & 0x80 ? PSMB_AGN_OPEN : 0);
	if (a & 0x10) {
		e->type = e->creator = 0;
		e->dlen = pb.u.f.ioFlStBlk;                           /* a folder's valence */
		e->rlen = 0;
	} else {
		e->type = pb.u.f.fdType; e->creator = pb.u.f.fdCreator;
		e->dlen = pb.u.f.ioFlLgLen; e->rlen = pb.u.f.ioFlRLgLen;
	}
	e->crdate = pb.u.f.ioFlCrDat; e->mddate = pb.u.f.ioFlMdDat; e->bkdate = pb.u.f.ioFlBkDat;
	e->fdflags = pb.u.f.fdFlags;
	e->location = pb.u.f.fdLocation;
	e->id = (unsigned long)pb.u.f.ioDirID;
	e->parid = (unsigned long)pb.u.f.ioFlParID;
	put_name(e->name, name);
}

/* ---- the operations: each sets the reply (status is their result) ---- */
static volatile struct psmb_agent *A;                /* the block, for the request under way */

static short op_stat(void)
{
	short e = make_path(A->path, full);
	if (e) return e;
	if ((e = get_cat(full))) return e;
	split(full, parent, leaf);
	if (!leaf[0]) {                                          /* the volume's root: its name */
		unsigned n = 0;
		while (n < full[0] && full[n + 1] != ':') { leaf[n + 1] = full[n + 1]; n++; }
		leaf[0] = (unsigned char)n;
	}
	fill_entry((volatile struct psmb_agent_entry *)xbuf(), leaf);
	A->result = 1;
	return 0;
}

static short list_volumes(void)
{
	volatile struct psmb_agent_entry *e = (volatile struct psmb_agent_entry *)xbuf();
	unsigned long n = 0;
	for (short i = (short)(A->index ? A->index : 1); n < PSMB_AGENT_BUF / sizeof *e; i++) {
		pb_clear(&pb2);
		pb2.ioNamePtr = name32;
		pb2.u.v.ioVolIndex = i;
		short r = pb_hgetvinfo(&pb2);
		if (r == nsvErr) break;
		if (r) return r;
		unsigned long kb = pb2.u.v.ioVAlBlkSiz / 512;          /* allocation blocks in half-KiB */
		e->flags = PSMB_AGN_VOLUME | PSMB_AGN_DIR | ((pb2.u.v.ioVAtrb & 0x8080) ? PSMB_AGN_LOCKED : 0) |
		           (pstr_ieq(name32, startup) ? PSMB_AGN_STARTUP : 0);
		e->type = e->creator = 0;
		e->dlen = (unsigned long)pb2.u.v.ioVFrBlk * kb / 2;
		e->rlen = (unsigned long)pb2.u.v.ioVNmAlBlks * kb / 2;
		e->crdate = pb2.u.v.ioVCrDate; e->mddate = pb2.u.v.ioVLsMod; e->bkdate = pb2.u.v.ioVBkUp;
		e->fdflags = 0; e->location = 0;
		e->id = (unsigned long)(long)pb2.ioVRefNum; e->parid = 1;
			put_name(e->name, name32);
		e++; n++;
	}
	A->result = n;
	A->more = 0;
	return 0;
}

/* LIST: a folder's entries from index on, as many as the buffer holds or six ticks allow; a file: itself */
static short op_list(void)
{
	if (!A->path[0]) return list_volumes();
	short e = make_path(A->path, full), vref;
	if (e) return e;
	if ((e = get_cat(full))) return e;
	if (!cat_is_dir()) return op_stat();
	long dir = pb.u.f.ioDirID;
	unsigned long valence = pb.u.f.ioFlStBlk, first = A->index ? A->index : 1, n = 0, t0 = LM_TICKS;
	if ((e = vol_of(full, &vref))) return e;
	volatile struct psmb_agent_entry *r = (volatile struct psmb_agent_entry *)xbuf();
	for (unsigned long i = first; i <= valence && n < PSMB_AGENT_BUF / sizeof *r; i++) {
		pb_clear(&pb);
		pb.ioNamePtr = name32;
		pb.ioVRefNum = vref;
		pb.u.f.ioFDirIndex = (short)i;
		pb.u.f.ioDirID = dir;
		e = pb_getcatinfo(&pb);
		if (e == fnfErr) { valence = i - 1; break; }          /* the folder lost entries since it was counted */
		if (e) return e;
		fill_entry(r++, name32);
		n++;
		if (LM_TICKS - t0 > 6) break;                         /* the rest in the next request: the program in front waits */
	}
	A->result = n;
	A->more = first + n - 1 < valence ? valence - (first + n - 1) : 0;
	return 0;
}

/* a fork of path, opened: its reference number in pb.u.io.ioRefNum */
static short open_fork(const unsigned char *path, int rsrc, int perm)
{
	pb_clear(&pb);
	pb.ioNamePtr = path;
	pb.u.io.ioPermssn = (char)perm;
	return rsrc ? pb_hopenrf(&pb) : pb_hopen(&pb);
}
static void close_fork(void)
{
	short ref = pb.u.io.ioRefNum;
	pb_clear(&pb);
	pb.u.io.ioRefNum = ref;
	pb_close(&pb);
}

static short op_read(void)
{
	short e = make_path(A->path, full);
	if (e) return e;
	if (A->length > PSMB_AGENT_BUF) return -50;               /* paramErr */
	if ((e = get_cat(full))) return e;
	if (cat_is_dir()) return PSMB_AGE_ISDIR;
	if ((e = open_fork(full, A->fork == 1, fsRdPerm))) return e;
	short ref = pb.u.io.ioRefNum;
	pb_clear(&pb);
	pb.u.io.ioRefNum = ref;
	pb.u.io.ioBuffer = xbuf();
	pb.u.io.ioReqCount = (long)A->length;
	pb.u.io.ioPosMode = fsFromStart;
	pb.u.io.ioPosOffset = (long)A->offset;
	e = pb_read(&pb);
	A->result = (unsigned long)pb.u.io.ioActCount;
	if (e == eofErr) e = 0;
	pb.u.io.ioRefNum = ref;
	close_fork();
	return e;
}

static short set_finfo(const unsigned char *path, unsigned long type, unsigned long creator, unsigned long fdflags,
                       unsigned long crdate, unsigned long mddate)
{
	pb_clear(&pb);
	pb.ioNamePtr = path;
	short e = pb_hgetfinfo(&pb);
	if (e) return e;
	pb.u.f.fdType = type; pb.u.f.fdCreator = creator;
	pb.u.f.fdFlags = (unsigned short)fdflags;
	pb.u.f.fdLocation = 0;                                    /* the Finder finds it a place in its folder */
	pb.u.f.fdFldr = 0;
	if (crdate) pb.u.f.ioFlCrDat = crdate;
	if (mddate) pb.u.f.ioFlMdDat = mddate;
	pb.u.f.ioDirID = 0;                                       /* HGetFInfo left the file's number here */
	pb.ioNamePtr = path;
	pb.ioVRefNum = 0;
	pb.u.f.ioFDirIndex = 0;
	return pb_hsetfinfo(&pb);
}

/* the folders of path that are not there yet, from the top down (the last name is left alone) */
static short make_parents(const unsigned char *path)
{
	unsigned char p[256];
	unsigned colons = 0;
	for (unsigned i = 1; i < path[0]; i++) {
		if (path[i] != ':') continue;
		if (++colons == 1) continue;                          /* the volume */
		for (unsigned k = 1; k < i; k++) p[k] = path[k];
		p[0] = (unsigned char)(i - 1);
		short e = get_cat(p);
		if (!e) { if (!cat_is_dir()) return PSMB_AGE_NOTDIR; continue; }
		if (e != fnfErr) return e;
		pb_clear(&pb);
		pb.ioNamePtr = p;
		if ((e = pb_dircreate(&pb))) return e;
	}
	return 0;
}

static short truncate_fork(const unsigned char *path, int rsrc)
{
	short e = open_fork(path, rsrc, fsWrPerm);
	if (e) return e;
	short ref = pb.u.io.ioRefNum;
	pb_clear(&pb);
	pb.u.io.ioRefNum = ref;
	pb.u.io.ioMisc = 0;                                       /* the new end of file */
	e = pb_seteof(&pb);
	pb.u.io.ioRefNum = ref;
	close_fork();
	return e;
}

static short op_create(void)
{
	short e = make_path(A->path, full);
	if (e) return e;
	if (is_root(full)) return PSMB_AGE_ISDIR;
	if ((A->flags & PSMB_AGF_MKDIR) && (e = make_parents(full))) return e;
	e = get_cat(full);
	if (!e) {
		if (cat_is_dir()) return PSMB_AGE_ISDIR;
		if (!(A->flags & PSMB_AGF_FORCE)) { note("--force replaces it"); return dupFNErr; }
		if ((e = truncate_fork(full, 0)) || (e = truncate_fork(full, 1))) return e;
		A->result = 0;                                        /* an existing file, emptied */
	} else if (e == fnfErr) {
		pb_clear(&pb);
		pb.ioNamePtr = full;
		if ((e = pb_hcreate(&pb))) return e;
		A->result = 1;                                        /* a new one: the kernel deletes it if the transfer fails */
	} else return e;
	return set_finfo(full, A->ftype, A->fcreator, A->fdflags & ~0x0100u, 0, 0);
}

static short op_write(void)
{
	short e = make_path(A->path, full);
	if (e) return e;
	if (A->length > PSMB_AGENT_BUF) return -50;
	if ((e = open_fork(full, A->fork == 1, fsWrPerm))) return e;
	short ref = pb.u.io.ioRefNum;
	pb_clear(&pb);
	pb.u.io.ioRefNum = ref;
	pb.u.io.ioBuffer = xbuf();
	pb.u.io.ioReqCount = (long)A->length;
	pb.u.io.ioPosMode = fsFromStart;
	pb.u.io.ioPosOffset = (long)A->offset;
	e = pb_write(&pb);
	A->result = (unsigned long)pb.u.io.ioActCount;
	pb.u.io.ioRefNum = ref;
	close_fork();
	return e;
}

static short op_setinfo(void)
{
	short e = make_path(A->path, full), vref;
	if (e) return e;
	/* hasBeenInited off, so that the Finder reads a new application's bundle and gives the file its place */
	if ((e = set_finfo(full, A->ftype, A->fcreator, A->fdflags & ~0x0100u, A->crdate, A->mddate))) return e;
	if (!vol_of(full, &vref)) {
		pb_clear(&pb2);
		pb2.ioVRefNum = vref;
		pb_flushvol(&pb2);
	}
	return 0;
}

static short op_mkdir(void)
{
	short e = make_path(A->path, full);
	if (e) return e;
	if (is_root(full)) return dupFNErr;
	e = get_cat(full);
	if (!e) {
		if (!cat_is_dir()) return PSMB_AGE_NOTDIR;
		if (A->flags & PSMB_AGF_MKDIR) { A->result = (unsigned long)pb.u.f.ioDirID; return 0; }
		return dupFNErr;
	}
	if (e != fnfErr && e != dirNFErr) return e;
	if ((A->flags & PSMB_AGF_MKDIR) && (e = make_parents(full))) return e;
	pb_clear(&pb);
	pb.ioNamePtr = full;
	if ((e = pb_dircreate(&pb))) return e;
	A->result = (unsigned long)pb.u.f.ioDirID;
	return 0;
}

static short op_delete(void)
{
	short e = make_path(A->path, full);
	if (e) return e;
	if (is_root(full)) return PSMB_AGE_BADPATH;
	if ((e = get_cat(full))) return e;
	if (cat_is_dir() && pb.u.f.ioFlStBlk) return PSMB_AGE_NOTEMPTY;
	pb_clear(&pb);
	pb.ioNamePtr = full;
	return pb_hdelete(&pb);
}

/* path renamed to name in the same folder. The new name goes as a full path too: given as a bare name the
   File Manager takes it relative to the default volume, and answered bdNamErr (-37) in the first test. */
static short rename_to(const unsigned char *path, const unsigned char *name)
{
	static unsigned char dir[256], old[64], np[256];
	split(path, dir, old);
	unsigned n = dir[0];
	for (unsigned i = 1; i <= n; i++) np[i] = dir[i];
	if (np[n] != ':') np[++n] = ':';
	for (unsigned i = 1; i <= name[0] && n < 255; i++) np[++n] = name[i];
	np[0] = (unsigned char)n;
	short vref;
	if (vol_of(path, &vref)) vref = 0;
	pb_clear(&pb);
	pb.ioNamePtr = path;
	pb.ioVRefNum = vref;
	pb.u.io.ioMisc = (void *)np;
	return pb_hrename(&pb);
}
static short catmove(const unsigned char *path, long to_dir)
{
	pb_clear(&pb);
	pb.ioNamePtr = path;
	pb.u.m.ioNewDirID = to_dir;
	return pb_catmove(&pb);
}
/* RENAME: path to path2 on the same volume. path2 an existing folder: into it under the same name; else
   path2's folder must exist, and the thing gets path2's last name. A move with a new name renames first
   (in its own folder), then moves, and renames back if the move fails. */
static short op_rename(void)
{
	static unsigned char newleaf[64], target[256], moved[256];
	short e = make_path(A->path, full), v1, v2;
	if (e || (e = make_path(A->path2, full2))) return e;
	if (is_root(full)) return PSMB_AGE_BADPATH;
	if ((e = vol_of(full, &v1)) || (e = vol_of(full2, &v2))) return e;
	if (v1 != v2) return PSMB_AGE_XVOLUME;
	if ((e = get_cat(full))) return e;
	long from_dir = pb.u.f.ioFlParID;
	split(full, parent, leaf);
	e = get_cat(full2);
	if (!e && cat_is_dir()) {                                /* into the folder */
		for (unsigned i = 0; i <= full2[0]; i++) target[i] = full2[i];
		for (unsigned i = 0; i <= leaf[0]; i++) newleaf[i] = leaf[i];
	} else if (!e) {
		note("there is a file of that name already");
		return dupFNErr;
	} else if (e == fnfErr) {
		split(full2, target, newleaf);
	} else return e;
	if (!newleaf[0]) return PSMB_AGE_BADPATH;
	if ((e = get_cat(target))) return e == fnfErr ? dirNFErr : e;
	if (!cat_is_dir()) return PSMB_AGE_NOTDIR;
	long to_dir = pb.u.f.ioDirID;
	int same_name = newleaf[0] == leaf[0] && !memcmp(newleaf + 1, leaf + 1, leaf[0]);
	if (to_dir == from_dir) return same_name ? 0 : rename_to(full, newleaf);
	if (same_name) return catmove(full, to_dir);
	if ((e = rename_to(full, newleaf))) return e;
	unsigned n = parent[0];
	for (unsigned i = 1; i <= n; i++) moved[i] = parent[i];
	if (moved[n] != ':') moved[++n] = ':';
	for (unsigned i = 1; i <= newleaf[0]; i++) moved[++n] = newleaf[i];
	moved[0] = (unsigned char)n;
	if ((e = catmove(moved, to_dir))) rename_to(moved, leaf);
	return e;
}

/* ---- programs ---- */
#define T_APPL 0x4150504CUL                                   /* 'APPL' */
static int is_app_type(unsigned long t)
{
	return t == T_APPL || t == 0x61707065UL /* appe */ || t == 0x41505043UL /* APPC */ || t == 0x41505044UL /* APPD */;
}
static struct {                                               /* the Process Manager's ProcessInfoRec */
	long length;
	unsigned char *name;
	PSN psn;
	unsigned long type, signature, mode;
	void *location;
	unsigned long size, freemem;
	PSN launcher;
	unsigned long launched, active;
	FSSpec *spec;
} pinfo;
_Static_assert(sizeof(pinfo) == 60, "ProcessInfoRec");
static FSSpec app_spec, doc_spec, run_spec;

/* the running program whose file is spec (spec_match) or whose name or signature is asked for */
static int find_process(const FSSpec *spec, const unsigned char *pname, unsigned long sig, PSN *out)
{
	PSN p = { 0, 0 };
	while (!pm_next(&p)) {
		pinfo.length = sizeof pinfo; pinfo.name = name32; pinfo.spec = &run_spec;
		if (pm_info(&p, &pinfo)) continue;
		int hit = spec ? run_spec.vRefNum == spec->vRefNum && run_spec.parID == spec->parID && pstr_ieq(run_spec.name, spec->name)
		               : (pname && pname[0] && pstr_ieq(name32, pname)) || (sig && pinfo.signature == sig);
		if (hit) { *out = p; return 1; }
	}
	return 0;
}

static short dt_err;                                          /* the catalog search's last error, for the note */
/* The application of creator on some volume (the document's first), by searching the catalog (PBCatSearch,
   System 7) for a file of type 'APPL' and that creator. The desktop database was asked first in the first
   version (PBDTGetPath, PBDTGetAPPL): on the testbed's disk it knew none of the applications (-5012), and
   after each such look the Finder fell into a loop of alerts, "an error of type -192"; the catalog search
   touches nothing that the Finder keeps. */
static short find_app(unsigned long creator, short first_vref, FSSpec *out)
{
	static PB want, mask;
	static struct {
		void *qLink; short qType, ioTrap; void *ioCmdAddr, *ioCompletion; short ioResult;
		const unsigned char *ioNamePtr; short ioVRefNum;
		FSSpec *ioMatchPtr; long ioReqMatchCount, ioActMatchCount, ioSearchBits;
		void *ioSearchInfo1, *ioSearchInfo2; long ioSearchTime, ioCatPosition[4]; void *ioOptBuffer; long ioOptBufSize;
	} cs;
	_Static_assert(sizeof cs == 76 && __builtin_offsetof(__typeof__(cs), ioSearchBits) == 36, "CSParam");
	for (short i = 0; i < 16; i++) {
		short vref = first_vref;
		if (i) {
			pb_clear(&pb2);
			pb2.u.v.ioVolIndex = i;
			if (pb_hgetvinfo(&pb2)) break;
			vref = pb2.ioVRefNum;
			if (vref == first_vref) continue;
		}
		pb_clear(&want); pb_clear(&mask);
		want.u.f.fdType = T_APPL; want.u.f.fdCreator = creator;
		mask.u.f.fdType = 0xFFFFFFFFUL; mask.u.f.fdCreator = 0xFFFFFFFFUL;
		memset(&cs, 0, sizeof cs);
		cs.ioVRefNum = vref;
		cs.ioMatchPtr = out;
		cs.ioReqMatchCount = 1;
		cs.ioSearchBits = 8;                                  /* fsSBFlFndrInfo: the type and creator, under the mask */
		cs.ioSearchInfo1 = &want; cs.ioSearchInfo2 = &mask;
		short e = pb_catsearch(&cs);
		if ((e == 0 || e == eofErr) && cs.ioActMatchCount > 0) return 0;
		if (e && e != eofErr) dt_err = e;
	}
	return PSMB_AGE_NOAPP;
}

static AEDesc d_target, d_event, d_list, d_alias, d_params, d_reply;
static void dispose_all(void)
{
	AEDesc *d[] = { &d_target, &d_event, &d_list, &d_alias, &d_params, &d_reply };
	for (unsigned i = 0; i < sizeof d / sizeof d[0]; i++)
		if (d[i]->data) { ae_dispose_desc(d[i]); d[i]->data = 0; }
}
/* an 'odoc' Apple event for doc_spec, addressed to target (a signature or a process) */
static short make_odoc(unsigned long target_type, const void *target, long target_size)
{
	void **alias = 0;
	short e;
	if ((e = new_alias(0, &doc_spec, &alias))) return e;
	mac_hlock(alias);
	e = ae_create_desc(0x616C6973UL /* 'alis' */, *alias, *(unsigned short *)((char *)*alias + 4), &d_alias);
	mac_dispose_handle(alias);
	if (e) return e;
	if ((e = ae_create_list(&d_list)) || (e = ae_put_desc(&d_list, 0, &d_alias))) return e;
	if ((e = ae_create_desc(target_type, target, target_size, &d_target))) return e;
	if ((e = ae_create_event(0x61657674UL /* 'aevt' */, 0x6F646F63UL /* 'odoc' */, &d_target, &d_event))) return e;
	return ae_put_param_desc(&d_event, 0x2D2D2D2DUL /* keyDirectObject '----' */, &d_list);
}

static struct {                                               /* LaunchParamBlockRec (System 7), extending System 6's */
	const unsigned char *name;                                /* System 6: the application's name in the default folder */
	short param;
	short block_id;                                           /* 'LC': the extended block */
	long epb_length;
	short file_flags, control_flags;
	FSSpec *spec;
	PSN psn;
	long preferred, minimum, available;
	void *parameters;
} launch;
_Static_assert(sizeof(launch) == 44, "LaunchParamBlockRec");

static int launch_pending;                                    /* open7 has a launch ready: launch_now after the answer */
static void launch_now(void);
static short open7(void)
{
	short vref;
	short e = vol_of(full, &vref);
	if (e) return e;
	if ((e = get_cat(full))) return e;
	if (cat_is_dir()) { note("a folder is not opened (only applications and documents)"); return PSMB_AGE_ISDIR; }
	unsigned long type = pb.u.f.fdType, creator = pb.u.f.fdCreator;
	FSSpec here;
	here.vRefNum = vref; here.parID = pb.u.f.ioFlParID;
	split(full, parent, leaf);
	for (unsigned i = 0; i <= leaf[0]; i++) here.name[i] = leaf[i];
	int doc = !is_app_type(type);
	if (doc) {
		doc_spec = here;
		dt_err = 0;
		if ((e = find_app(creator, vref, &app_spec))) {
			char t[24], *q = t + 23;                          /* the catalog search's error, as a number */
			int v = dt_err < 0 ? -dt_err : dt_err;
			*q = 0; *--q = ')';
			do { *--q = (char)('0' + v % 10); v /= 10; } while (v);
			if (dt_err < 0) *--q = '-';
			*--q = ' '; *--q = ':'; *--q = 'h'; *--q = 'c'; *--q = 'r'; *--q = 'a'; *--q = 'e'; *--q = 's'; *--q = '('; *--q = ' ';
			note_type("no application for the creator ", creator, q);
			return e;
		}
	} else app_spec = here;
	PSN psn;
	void *zone = LM_THEZONE;
	mac_setzone(LM_SYSZONE);                                  /* the Apple events' and the alias's memory */
	if (find_process(&app_spec, 0, 0, &psn)) {
		e = 0;
		if (doc && !(e = make_odoc(0x70736E20UL /* 'psn ' */, &psn, 8)))
			e = ae_send(&d_event, &d_reply, 1 /* kAENoReply */ | 0x20 /* kAECanInteract */);
		if (!e) e = pm_setfront(&psn);
		A->result = 2;                                        /* it was running */
		if (e == -903 || e == -906)                           /* noPortErr, destPortErr: an application of before System 7 */
			note("it takes no Apple events: quit it, then open the document");
		else note(doc ? "running already: the document sent to it, brought to the front" : "running already: brought to the front");
	} else {
		/* Not running: launched with the 'odoc' event as its launch parameters (AECoerceDesc to 'appa', the
		   documented way), which the Process Manager also turns into the Finder information an application
		   of before System 7 reads (Aurora68 Play takes no Apple events). The System 7.1 Finder itself takes no
		   Apple events (noPortErr), so it cannot be asked to open the document as a double-click would. */
		e = doc ? make_odoc(0x7369676EUL /* 'sign' */, &creator, 4) : 0;
		if (!e && doc && !(e = ae_coerce_desc(&d_event, 0x61707061UL /* 'appa' */, &d_params))) mac_hlock(d_params.data);
		if (!e) {
			/* The application's FSSpec in a place of its own for this launch: the Process Manager reads it
			   while the new process starts, after LaunchApplication has come back, and the next request
			   (which reuses app_spec) came before that in the testbed: "the application 'unknown' has
			   unexpectedly quit". Four in turn; a launch is long done before its record comes round again. */
			static FSSpec specs[4];
			static unsigned turn;
			FSSpec *sp = &specs[turn++ & 3];
			*sp = app_spec;
			memset(&launch, 0, sizeof launch);
			launch.block_id = 0x4C43;                         /* 'LC' */
			launch.epb_length = sizeof launch - 12;
			launch.control_flags = 0x4000 | 0x0800;           /* launchContinue, launchNoFileFlags */
			launch.spec = sp;
			launch.parameters = d_params.data ? *d_params.data : 0;
			launch_pending = 1;                               /* after the answer: c_filter; the events are its to dispose of */
			mac_setzone(zone);
			A->result = 1;
			note(doc ? "its application is being launched with it" : "being launched");
			return 0;
		}
		A->result = 1;
	}
	dispose_all();
	mac_setzone(zone);
	return e;
}

/* The launch itself, once the request is answered and the filter is no longer inside it. LaunchApplication
   from inside the request came back into the filter while the request was still under way, as if the
   filter's guard had been lifted: in the first tests the request ran twice, and with a guard against that
   it spun until the kernel gave up. So it is called last, as System 6's Launch is (open6), and its error, if
   any, goes to the log. */
static void launch_now(void)
{
	void *zone = LM_THEZONE;
	launch_pending = 0;
	/* The Apple event and its launch parameters are this launch's own, kept until the next launch: a request
	   taken while it runs (the filter is open again) could otherwise dispose of them under LaunchApplication,
	   and the Process Manager may read them once the new process starts, after the call has come back. */
	static AEDesc held[6];
	mac_setzone(LM_SYSZONE);
	for (unsigned i = 0; i < 6; i++)
		if (held[i].data) { ae_dispose_desc(&held[i]); held[i].data = 0; }
	mac_setzone(zone);
	held[0] = d_target; held[1] = d_event; held[2] = d_list; held[3] = d_alias; held[4] = d_params; held[5] = d_reply;
	d_target.data = d_event.data = d_list.data = d_alias.data = d_params.data = d_reply.data = 0;
	short e = mac_launch(&launch);
	if (e) log_err("LaunchApplication answered ", e);
}

/* System 6: an application only, by _Launch from its folder (made the default through a working directory);
   with MultiFinder beside the others (launchContinue), without it in place of the program in front */
static short open6(void)
{
	short vref;
	short e = vol_of(full, &vref);
	if (e || (e = get_cat(full))) return e;
	if (cat_is_dir()) return PSMB_AGE_ISDIR;
	if (!is_app_type(pb.u.f.fdType)) { note("System 6: only applications are opened"); return PSMB_AGE_UNSUPPORTED; }
	long dir = pb.u.f.ioFlParID;
	unsigned short fdflags = pb.u.f.fdFlags;
	split(full, parent, leaf);
	pb_clear(&pb2);
	pb2.ioVRefNum = vref;
	pb2.u.wd.ioWDDirID = dir;
	pb2.u.wd.ioWDProcID = (long)0x45524943UL;                 /* the working directory's owner: any four letters */
	if ((e = pb_openwd(&pb2))) return e;
	short wd = pb2.ioVRefNum;
	pb_clear(&pb2);
	pb2.ioVRefNum = wd;
	if ((e = pb_setvol(&pb2))) return e;
	memset(&launch, 0, sizeof launch);
	launch.name = leaf;
	launch.block_id = 0x4C43;
	launch.epb_length = 4;                                   /* the two flag words after it */
	launch.file_flags = (short)fdflags;
	A->result = 1;
	if (features & PSMB_AGX_MULTIFINDER) {
		launch.control_flags = 0x4000;                        /* launchContinue */
		return mac_launch(&launch);
	}
	/* The Finder alone: Launch does not come back (the program in front is replaced, its stack and heap
	   with it), so the request is answered first and the filter forgets it is inside one. */
	A->status = 0;
	note("launched in place of the program in front (no MultiFinder)");
	A->served++;
	A->done_seq = A->req_seq;
	busy = 0;
	mac_launch(&launch);
	return 0;
}

static short op_open(void)
{
	short e = make_path(A->path, full);
	if (e) return e;
	if (is_root(full)) return PSMB_AGE_ISDIR;
	if (sys7() && !pm_ready()) { note("the Process Manager has not started yet"); return PSMB_AGE_BUSY; }
	return sys7() ? open7() : open6();
}

static short op_apps(void)
{
	volatile struct psmb_agent_proc *r = (volatile struct psmb_agent_proc *)xbuf();
	unsigned long n = 0, i = 0, skip = A->index;
	if (!sys7()) {                                            /* System 6: what CurApName says */
		if (skip) { A->result = 0; A->more = 0; return 0; }
		r->psn_hi = 0; r->psn_lo = 0; r->type = T_APPL; r->creator = 0; r->mode = 0; r->size = r->free = 0;
		r->launched = r->active = 0; r->flags = PSMB_AGP_CURRENT | PSMB_AGP_FRONT;
		put_name(r->name, LM_CURAPNAME);
		A->result = 1; A->more = 0;
		return 0;
	}
	if (!pm_ready()) { note("the Process Manager has not started yet"); return PSMB_AGE_BUSY; }
	PSN p = { 0, 0 }, front = { 0, 0 }, cur = { 0, 0 };
	pm_front(&front);
	pm_current(&cur);
	while (!pm_next(&p)) {
		if (i++ < skip) continue;
		if (n >= PSMB_AGENT_BUF / sizeof *r) { A->more = 1; break; }
		pinfo.length = sizeof pinfo; pinfo.name = name32; pinfo.spec = &run_spec;
		if (pm_info(&p, &pinfo)) continue;
		r->psn_hi = p.hi; r->psn_lo = p.lo;
		r->type = pinfo.type; r->creator = pinfo.signature; r->mode = pinfo.mode;
		r->size = pinfo.size; r->free = pinfo.freemem;
		r->launched = pinfo.launched; r->active = pinfo.active;
		r->flags = (p.hi == front.hi && p.lo == front.lo ? PSMB_AGP_FRONT : 0) | (p.hi == cur.hi && p.lo == cur.lo ? PSMB_AGP_AGENT : 0);
		put_name(r->name, name32);
		r++; n++;
	}
	A->result = n;
	return 0;
}

/* QUIT: a 'quit' Apple event, the program may still ask to save; the Finder is not quit this way */
static short op_quit(void)
{
	if (!sys7()) { note("System 6 has no Apple events: quit the program on the Mac"); return PSMB_AGE_UNSUPPORTED; }
	if (!pm_ready()) { note("the Process Manager has not started yet"); return PSMB_AGE_BUSY; }
	unsigned char want[64];
	unsigned n = 0;
	for (; A->path[n] && n < 31; n++) want[n + 1] = (unsigned char)A->path[n];
	want[0] = (unsigned char)n;
	PSN psn;
	if (!find_process(0, want, A->fcreator, &psn)) return PSMB_AGE_NOPROC;
	if (pinfo.signature == 0x4D414353UL /* 'MACS' */ || pinfo.type == 0x464E4452UL /* 'FNDR' */) {
		note("the Finder is not quit (restart the Mac instead)");
		return PSMB_AGE_UNSUPPORTED;
	}
	put_name(A->note, name32);
	void *zone = LM_THEZONE;
	mac_setzone(LM_SYSZONE);
	short e = ae_create_desc(0x70736E20UL /* 'psn ' */, &psn, 8, &d_target);
	if (!e) e = ae_create_event(0x61657674UL /* 'aevt' */, 0x71756974UL /* 'quit' */, &d_target, &d_event);
	if (!e) e = ae_send(&d_event, &d_reply, 1 /* kAENoReply */ | 0x20 /* kAECanInteract */);
	dispose_all();
	mac_setzone(zone);
	A->result = 1;
	return e;
}

/* FRONT: the application to the front (the remote's /screenshot?front=1); note says which one was there */
static short op_front(void)
{
	if (!sys7()) { note("System 6: the program in front is not changed"); return PSMB_AGE_UNSUPPORTED; }
	if (!pm_ready()) { note("the Process Manager has not started yet"); return PSMB_AGE_BUSY; }
	unsigned char want[64];
	unsigned n = 0;
	for (; A->path[n] && n < 31; n++) want[n + 1] = (unsigned char)A->path[n];
	want[0] = (unsigned char)n;
	PSN psn, prev = { 0, 0 };
	if (!find_process(0, want, A->fcreator, &psn)) return PSMB_AGE_NOPROC;
	pm_front(&prev);
	pinfo.length = sizeof pinfo; pinfo.name = name32; pinfo.spec = &run_spec;
	if (!pm_info(&prev, &pinfo)) put_name(A->note, name32);
	A->result = prev.hi == psn.hi && prev.lo == psn.lo ? 2 : 1;   /* 2: it was in front already */
	return pm_setfront(&psn);
}

/* ---- the request ---- */
/* Each request is carried out once. A launch under System 7 ran its request twice in the first tests (the
   second time finding the program it had just launched): LaunchApplication from within the filter came
   back into it with the request not yet answered. The request's number is taken before the work starts,
   so a second look at it while the first is under way only counts the repeat (repeats, /state). */
static unsigned long serving;
static int serve(unsigned long req)
{
	short e;
	if (req == serving) { A->repeats++; return 0; }
	serving = req;
	A->note[0] = 0;
	A->result = 0; A->more = 0;
	switch (A->op) {
	case PSMB_AG_PING:    e = 0; break;
	case PSMB_AG_LIST:    e = op_list(); break;
	case PSMB_AG_STAT:    e = op_stat(); break;
	case PSMB_AG_READ:    e = op_read(); break;
	case PSMB_AG_CREATE:  e = op_create(); break;
	case PSMB_AG_WRITE:   e = op_write(); break;
	case PSMB_AG_SETINFO: e = op_setinfo(); break;
	case PSMB_AG_MKDIR:   e = op_mkdir(); break;
	case PSMB_AG_DELETE:  e = op_delete(); break;
	case PSMB_AG_RENAME:  e = op_rename(); break;
	case PSMB_AG_OPEN:    e = op_open(); break;
	case PSMB_AG_APPS:    e = op_apps(); break;
	case PSMB_AG_QUIT:    e = op_quit(); break;
	case PSMB_AG_FRONT:   e = op_front(); break;
	default:              e = PSMB_AGE_BADOP; break;
	}
	/* a change to the catalog onto the disk at once: the first tests lost a folder made shortly before the
	   Mac crashed, the File Manager having kept it in its cache (a transfer flushes in SETINFO) */
	if (!e && (A->op == PSMB_AG_MKDIR || A->op == PSMB_AG_DELETE || A->op == PSMB_AG_RENAME)) {
		short vref;
		if (!vol_of(full, &vref)) { pb_clear(&pb2); pb2.ioVRefNum = vref; pb_flushvol(&pb2); }
	}
	A->status = e;
	A->served++;
	A->done_seq = req;                                        /* last: the answer is complete */
	return 1;
}

/* The GetNextEvent filter. Idle: one look at the block. A request: carried out, and the next ones too
   while they follow at once (a transfer's chunks), for at most five ticks, so that the program in front
   is not held up for long; the kernel's next request comes within a millisecond of the answer. */
void c_filter(void *event)
{
	(void)event;
	if (busy) return;
	A = blk();
	A->beat++;
	unsigned long req = A->req_seq;
	if (sys7()) {                                             /* the program the VBL task wakes */
		static unsigned long looked;
		if (LM_TICKS - looked > 30) { looked = LM_TICKS; if (pm_ready()) pm_front(&front_psn); }
	}
	if (req == A->done_seq || LM_FSBUSY) return;
	busy = 1;
	unsigned long t0 = LM_TICKS;
	for (;;) {
		if (!serve(req)) break;                               /* that one is under way already (above) */
		if (!busy) return;                                    /* System 6's Launch answered for itself (open6) */
		if (launch_pending) break;
		unsigned long t1 = LM_TICKS;
		while ((req = A->req_seq) == A->done_seq && LM_TICKS - t1 < 2) ;   /* a transfer's next chunk follows at once */
		if (req == A->done_seq || LM_TICKS - t0 >= 5 || LM_FSBUSY) break;
	}
	busy = 0;
	if (launch_pending) launch_now();
}

/* The VBL task: a heartbeat for the kernel, and the program in front woken while a request waits, again
   every quarter of a second while nobody answers (a program may have been busy when woken). WakeUpProcess
   is the Process Manager's one call allowed at interrupt time, made for completion routines; it makes a
   program asleep in WaitNextEvent return at once. Without it a request waited for the end of the front
   program's sleep: 120 to 200 ms each in the testbed, a megabyte in nine seconds. (The first version's
   crash, blamed on this call for a day, was GetFrontProcess's glue: entry.s, pm_front.) */
static struct { void *qLink; short qType; void *vblAddr; short vblCount, vblPhase; } vbl;
void c_vbl(void *task)
{
	(void)task;
	vbl.vblCount = 1;
	volatile struct psmb_agent *a = blk();
	a->vbl_beat++;
	if (!(features & PSMB_AGX_WAKE) || busy) return;
	unsigned long req = a->req_seq, t = LM_TICKS;
	if (req == a->done_seq || !front_psn.lo) return;
	if (req == woken_seq && t - woken_at < 15) return;
	woken_seq = req; woken_at = t;
	a->wakes++;
	pm_wakeup(&front_psn);
}

int init_main(void)
{
	if (*(volatile unsigned long *)win() != PSMB_MAGIC) return 0;   /* not an Aurora68 Mac */
	volatile struct psmb_agent *a = blk();
	if (a->kmagic != PSMB_AGENT_KMAGIC) { pi_log("the kernel has no agent block (a kernel before 21 November 2026): not installed", 0); return 0; }
	short sv = LM_SYSVERSION;
	features = 0;
	if (sv >= 0x0700) features |= PSMB_AGX_PROCMGR | PSMB_AGX_AEVENTS | PSMB_AGX_WAKE;
	else if (get_tool_trap(0xA88F) != get_tool_trap(0xA89F)) features |= PSMB_AGX_MULTIFINDER;   /* _OSDispatch: MultiFinder's */
	pb_clear(&pb2);                                           /* the startup volume, from the startup folder's working directory */
	pb2.ioNamePtr = startup;
	pb2.ioVRefNum = LM_BOOTDRIVE;
	if (pb_hgetvinfo(&pb2)) startup[0] = 0;
	put_name(a->startup, startup);
	a->sysversion = (unsigned long)sv;
	a->features = features;
	a->done_seq = a->req_seq;                                 /* nothing from before this start */
	a->served = 0; a->wakes = 0; a->repeats = 0;
	a->version = 1;
	vbl.qType = 1;                                            /* vType */
	vbl.vblAddr = (void *)vbl_task;
	vbl.vblCount = 1;
	if (mac_vinstall(&vbl)) features &= ~PSMB_AGX_WAKE;
	old_gne = (void *)LM_JGNEFILTER;
	LM_JGNEFILTER = (void **)gne_filter;
	a->magic = PSMB_AGENT_MAGIC;
	char v[100], *p = v;                                      /* the longest line below is 70 characters */
	const char *s = "installed under System ";
	while (*s) *p++ = *s++;
	*p++ = (char)('0' + (sv >> 8 & 15));
	*p++ = '.'; *p++ = (char)('0' + (sv >> 4 & 15));
	if (sv & 15) { *p++ = '.'; *p++ = (char)('0' + (sv & 15)); }
	s = sys7() ? ", the Process Manager, startup volume " : (features & PSMB_AGX_MULTIFINDER) ? ", MultiFinder, startup volume " : ", startup volume ";
	while (*s) *p++ = *s++;
	*p = 0;
	char sn[32];
	unsigned k = 0;
	for (; k < startup[0] && k < 31; k++) sn[k] = (char)startup[k + 1];
	sn[k] = 0;
	pi_log(v, sn);
	return 1;
}
