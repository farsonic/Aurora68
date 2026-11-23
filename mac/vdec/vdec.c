/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/vdec/vdec.c - the Aurora68 Video Decoder's QuickTime decompressors: Cinepak, Apple Video, Animation
 * and Graphics pictures handed to the card, through its driver, for a Pi core to decode.
 *
 * Backstory (22 November 2026, docs/08 item 110, GitHub issue #13). QuickTime finds a decompressor for a
 * picture by its compression type through the Component Manager: an 'imdc' component whose subtype is the
 * type ('cvid', 'rpza', 'rle ', 'smc '). Video cards of the time (SuperMac's, Radius's) added their own
 * 'imdc' components for the formats they decoded, and every QuickTime program went faster without knowing
 * it. This is the Aurora68 card's. It is registered by QuickTime from the "Aurora68 Video Decoder" file in
 * the Extensions folder (type 'thng', made by tools/mac/elf2thng.py); it says no at registration when the
 * card's driver does not open, so a Macintosh without the card keeps QuickTime's own.
 *
 * What a decompressor is asked (Inside Macintosh: QuickTime Components, "Image Compressor Components"):
 *   GetCodecInfo   its name and what it can do (decompression at 1 to 32 bits, temporal);
 *   PreDecompress  before a sequence of pictures, or when its destination changes: the image's description,
 *                  the destination pixel map; it answers with what it can take (its capabilities): here
 *                  the destination's own depth, the whole picture in one band, no scaling or masking of its
 *                  own (QuickTime then gives it an offscreen buffer and copies through the mask itself);
 *   BandDecompress one picture: its bytes, the pixel map and the rectangle in it.
 * PreDecompress opens a session on the card (a request through the driver's Control 1: the format, size,
 * depth and the ImageDescription with its colour table); BandDecompress hands each picture to that session
 * with the destination (baseAddr, rowBytes, depth, where in it, its colour table) and the card decodes and
 * draws it there while the 68k waits in the driver, a millisecond or two of a Pi core.
 *
 * Falling back. The component opens QuickTime's own decompressor of the same type with it (Apple's,
 * manufacturer 'appl'), and hands it every call it cannot do itself: when frame decoding is switched off
 * (the driver's Control 2, from Aurora68 Video's menu, or mac.vdecode=off on the Pi), when the card does not
 * take this picture's depth or size, when the picture's data has to be fetched in pieces (a data-loading
 * procedure), and for every call it does not implement. Once a sequence has gone to QuickTime's decoder it
 * stays there: the formats keep state from picture to picture.
 *
 * Everything runs in the caller's context (MoviePlayer's, the Finder's...), with its A5 untouched; the
 * per-instance record is a pointer in the caller's heap, given to the Component Manager as the instance's
 * storage. Logged as "[MAC] A68 Video Decoder: ..." through slot 9's INFO service.
 *
 * MPEG-1 (23 November 2026, docs/08 item 113, GitHub issue #25). A fifth decompressor, 'mpg1', the type the
 * extension's own movie import gives an MPEG-1 file's pictures (import.c: the file indexed, its pictures
 * made samples that point into the file itself, in display order). It works as the other four do, with
 * the card's 'mpg1' session around pl_mpeg's decoder; QuickTime has no decompressor of its own for it to
 * fall back to, so without the card (or with frame decoding switched off) there is none, and the import
 * says so in the movie itself. The same code resource serves the import: an instance knows from its
 * component's description which of the two it is, and every call to an import instance goes to import.c.
 */
#include "vdec.h"
#include "mpgindex.h"
#include "toolbox_gen.h"

void *memset(void *d, int c, unsigned long n) { char *a = d; while (n--) *a++ = (char)c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }

/* ---- QuickTime's records, as QuickTime 2.5 lays them out (2-byte alignment, as m68k GCC gives) -------- */
typedef struct { short top, left, bottom, right; } Rect;
typedef struct {
	char *baseAddr;
	short rowBytes;
	Rect bounds;
	short pmVersion, packType;
	long packSize, hRes, vRes;
	short pixelType, pixelSize, cmpCount, cmpSize;
	long planeBytes;
	void **pmTable;
	long pmReserved;
} PixMap;
typedef struct {
	long flags;
	short wantedPixelSize, extendWidth, extendHeight, bandMin, bandInc, pad;
	unsigned long time;
} CodecCapabilities;
typedef struct {
	long sequenceID;
	unsigned char **imageDescription;
	char *data;
	long bufferSize, frameNumber, startLine, stopLine, conditionFlags;
	short callerFlags;
	CodecCapabilities *capabilities;
	void *progressProc; long progressRefCon;
	void *completionProc; long completionRefCon;
	void *dataProc; long dataRefCon;
	void *port;
	PixMap dstPixMap;
	void *maskBits, *mattePixMap;
	Rect srcRect;
	void *matrix;
	long accuracy;
	short transferMode;
	void *frameTime;
	long reserved;
	char matrixFlags, matrixType;
	Rect dstRect;                    /* QuickTime 2.0 and later */
} CodecDecompressParams;
_Static_assert(__builtin_offsetof(CodecDecompressParams, dstPixMap) == 66, "dstPixMap");
_Static_assert(__builtin_offsetof(CodecDecompressParams, maskBits) == 116, "maskBits");
_Static_assert(__builtin_offsetof(CodecDecompressParams, dstRect) == 152, "dstRect");
typedef struct {
	unsigned char typeName[32];
	short version, revisionLevel;
	long vendor, decompressFlags, compressFlags, formatFlags;
	unsigned char compressionAccuracy, decompressionAccuracy;
	unsigned short compressionSpeed, decompressionSpeed;
	unsigned char compressionLevel, resvd;
	short minimumHeight, minimumWidth, decompressPipelineLatency, compressPipelineLatency;
	long privateData;
} CodecInfo;
_Static_assert(sizeof(CodecInfo) == 72, "CodecInfo");
typedef struct { long type, subType, manufacturer, flags, flagsMask; } ComponentDescription;

enum { kOpen = -1, kClose = -2, kCanDo = -3, kVersion = -4, kRegister = -5, kTarget = -6 };
enum { kGetCodecInfo = 0, kPreDecompress = 5, kBandDecompress = 6, kBusy = 7 };
#define codecUnimpErr          (-8962)
#define codecSpoolErr          (-8966)
#define codecInfoDoes1         (1L << 0)
#define codecInfoDoes2         (1L << 1)
#define codecInfoDoes4         (1L << 2)
#define codecInfoDoes8         (1L << 3)
#define codecInfoDoes16        (1L << 4)
#define codecInfoDoes32        (1L << 5)
#define codecInfoDoesTemporal  (1L << 10)
#define codecInfoDepth8        (1L << 3)
#define codecInfoDepth16       (1L << 4)
#define codecInfoDepth24       (1L << 6)
#define codecInfoDepth32       (1L << 5)
#define codecFlagNoScreenUpdate (1 << 3)
#define codecCompletionSource  1
#define codecCompletionDest    2
#define A68_VENDOR             TYPE('A', '6', '8', 'v')
#define VERSION                0x00010000L   /* 1.0 */

/* ---- the card's driver --------------------------------------------------------------------------- */
typedef struct {                 /* the Device Manager's parameter block for Open, Control and Status */
	void *qLink; short qType, ioTrap; void *ioCmdAddr, *ioCompletion; short ioResult;
	const unsigned char *ioNamePtr; short ioVRefNum, ioCRefNum, csCode;
	long csParam[6];
} CntrlParam;
_Static_assert(__builtin_offsetof(CntrlParam, csCode) == 26 && __builtin_offsetof(CntrlParam, csParam) == 28, "CntrlParam");

short drv_err;                   /* the last refusal, for the log */
static short drv_ref;            /* .A68Video's reference number, once opened (kept while the code stays loaded) */
int drv_open(void)
{
	CntrlParam pb;
	if (drv_ref) return 1;
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = (const unsigned char *)"\x09.A68Video";
	if ((drv_err = dm_open(&pb)) != 0) return 0;
	drv_ref = pb.ioCRefNum;
	return 1;
}
short drv_frame(struct a68v_freq *r)
{
	CntrlParam pb;
	memset(&pb, 0, sizeof pb);
	pb.ioCRefNum = drv_ref; pb.csCode = 100;
	pb.csParam[0] = (long)r;
	return dm_control(&pb);
}
int drv_enabled(void)
{
	CntrlParam pb;
	memset(&pb, 0, sizeof pb);
	pb.ioCRefNum = drv_ref; pb.csCode = 100;
	if ((drv_err = dm_status(&pb)) != 0) return 0;
	return pb.csParam[3] != 0;
}

/* ---- the log (slot 9's INFO service, psmb.h) ------------------------------------------------------- */
static unsigned char logline[220];
char *put(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
char *num(char *p, unsigned long v)
{
	char t[12]; int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n) *p++ = t[--n];
	return p;
}
static char *hex(char *p, unsigned long v) { for (int i = 28; i >= 0; i -= 4) *p++ = "0123456789ABCDEF"[v >> i & 15]; return p; }
char *fourcc(char *p, long t) { for (int i = 24; i >= 0; i -= 8) *p++ = (char)(t >> i); return p; }
void vlog(const char *text)
{
	static const char who[] = "A68 Video Decoder: ";
	volatile unsigned char *w = (volatile unsigned char *)(LM_MMU32BIT ? 0xF9000000ul : 0x00900000ul);
	unsigned long n = 0, k, req;
	if (*(volatile unsigned long *)w != 0x50534D42ul) return;      /* 'PSMB' */
	for (k = 0; who[k]; k++) logline[n++] = (unsigned char)who[k];
	for (k = 0; text[k] && n < sizeof logline - 1; k++) logline[n++] = (unsigned char)text[k];
	volatile unsigned char *c = w + 0x1000;
	volatile unsigned long *rq = (volatile unsigned long *)c, *dn = (volatile unsigned long *)(c + 4);
	short sr;
	__asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "cc");
	req = *rq;
	for (k = 0; *dn != req && k < 2000000; k++) { }
	*(volatile unsigned short *)(c + 8) = 2;
	*(volatile unsigned long *)(c + 28) = (unsigned long)logline;
	*(volatile unsigned long *)(c + 32) = n;
	*rq = req + 1;
	for (k = 0; *dn != req + 1 && k < 4000000; k++) { }
	__asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "cc");
}

/* ---- the instance ---------------------------------------------------------------------------------- */
struct inst {
	long type;                   /* 'imdc' (an 'eat ' instance is import.c's struct imp, which starts the same way) */
	long self;                   /* our ComponentInstance */
	long apple;                  /* QuickTime's own decompressor of the type, opened beside us (0: none) */
	int delegate;                /* 1: every call goes to it */
	long codec;                  /* the subtype we were registered for (from the first image) */
	unsigned long session;       /* the card's session, 0 when none */
	long seq;                    /* the sequence it belongs to */
	short w, h, depth;
	unsigned long frames, drawn, errors, last_err;
	unsigned long t_decode, t_draw;
	unsigned long pictures, first_tick, last_tick;   /* every BandDecompress, the card's or delegated: the rate */
	unsigned long us_total, us_max, us_n;                   /* the time inside BandDecompress, whoever decoded: the frame time */
	unsigned long predecompress;                      /* PreDecompress calls (QuickTime makes one when anything changes) */
	long cond_or, flags_or;                           /* the condition and caller flags seen, together */
	/* 'mpg1': catching the card up after QuickTime jumps (the import's index, mpgindex.h "mi_catchup") */
	short film;                                       /* the film, open for reading; 0: no index, no catching up */
	uint32_t *pics, npics, end;                       /* its pictures in coding order, the end of the last */
	long last_k;                                      /* the sample the card decoded last (0-based), -1 none */
	struct mi_sample *cu;                             /* room for one catch-up */
	uint8_t *cbuf; long cbuf_cap;                     /* a sample read from the film */
	unsigned long jumps, caught;
	struct a68v_freq req;
};

static const char *name_of(long t)
{
	switch (t) {
	case TYPE('c', 'v', 'i', 'd'): return "Cinepak";
	case TYPE('r', 'p', 'z', 'a'): return "Apple Video";
	case TYPE('r', 'l', 'e', ' '): return "Animation";
	case TYPE('s', 'm', 'c', ' '): return "Graphics";
	case TYPE('m', 'p', 'g', '1'): return "MPEG-1";
	}
	return "?";
}

/* QuickTime's own decompressor for type t (manufacturer 'appl'), opened for falling back to */
static long open_apple(long t)
{
	ComponentDescription d = { TYPE('i', 'm', 'd', 'c'), t, TYPE('a', 'p', 'p', 'l'), 0, 0 };
	long c = cm_find_next(0, &d);
	return c ? cm_open(c) : 0;
}

static void index_close(struct inst *in)
{
	if (in->film) { IOParam pb; memset(&pb, 0, sizeof pb); pb.ioRefNum = in->film; fm_close(&pb); }
	if (in->pics) mm_disposeptr(in->pics);
	if (in->cu) mm_disposeptr(in->cu);
	if (in->cbuf) mm_disposeptr(in->cbuf);
	in->film = 0; in->pics = 0; in->cu = 0; in->cbuf = 0; in->cbuf_cap = 0;
}
/* the import's index from an 'mpg1' description, and the film opened, for catching up after jumps */
static void index_open(struct inst *in, const unsigned char *desc, long len)
{
	const uint8_t *spec, *pb;
	uint32_t size, end, n;
	short ref;
	index_close(in);
	in->last_k = -1;
	if (!mi_index_find(desc, (uint32_t)len, &spec, &size, &end, &pb, &n) || !n) return;
	FSSpec fs;
	memcpy(&fs, spec, sizeof fs);
	if (FSpOpenDF(&fs, 1, &ref)) { vlog("MPEG-1: the film named in the movie did not open: no catching up after jumps"); return; }
	IOParam io; memset(&io, 0, sizeof io); io.ioRefNum = ref;
	if (fm_geteof(&io) || (uint32_t)io.ioMisc != size) { io.ioRefNum = ref; fm_close(&io); vlog("MPEG-1: the film is not the one indexed: no catching up after jumps"); return; }
	in->pics = mm_newptr((long)n * 4);
	in->cu = mm_newptr((long)n * (long)sizeof(struct mi_sample) / 4 + 64 * (long)sizeof(struct mi_sample));
	if (!in->pics || !in->cu) { in->film = ref; index_close(in); return; }
	memcpy(in->pics, pb, n * 4);                      /* big-endian, as the 68k reads it */
	in->film = ref; in->npics = n; in->end = end;
}
/* QuickTime gives sample k (its frameNumber is the sample's, 1 up); if the card does not hold the samples
   before it, the ones it needs are read from the film and decoded without being drawn */
static void catch_up(struct inst *in, CodecDecompressParams *p)
{
	long k = p->frameNumber - 1;
	uint32_t off, size;
	int flags;
	if (!in->film || k < 0 || k == in->last_k || k == in->last_k + 1 || (uint32_t)k >= in->npics) return;
	if (!mi_sample_at(in->pics, in->npics, in->end, (uint32_t)k, &off, &size, &flags) || (long)size != p->bufferSize) return;
	if (flags & MI_SYNC) return;
	uint32_t max = in->npics / 4 + 64;
	uint32_t n = mi_catchup(in->pics, in->npics, in->end, (uint32_t)k, in->cu, max);
	in->jumps++;
	for (uint32_t i = 0; i < n; i++) {
		struct mi_sample *s = &in->cu[i];
		if ((long)s->size > in->cbuf_cap) {
			if (in->cbuf) mm_disposeptr(in->cbuf);
			in->cbuf_cap = ((long)s->size + 0xFFFF) & ~0xFFFFL;
			if (!(in->cbuf = mm_newptr(in->cbuf_cap))) { in->cbuf_cap = 0; return; }
		}
		IOParam io; memset(&io, 0, sizeof io);
		io.ioRefNum = in->film; io.ioBuffer = in->cbuf; io.ioReqCount = (long)s->size; io.ioPosMode = 1; io.ioPosOffset = (long)s->off;
		if (fm_read(&io) || io.ioActCount != (long)s->size) return;
		struct a68v_freq *r = &in->req;
		memset(r, 0, sizeof *r);
		r->op = A68V_OP_DECODE; r->session = in->session;
		r->src = (unsigned long)in->cbuf; r->src_len = s->size;
		r->flags = A68V_FF_NODRAW; r->frame = s->index + 1;
		if (drv_frame(r) || (r->result && r->result != A68V_FE_DATA)) return;
		in->caught++;
	}
}

static void session_close(struct inst *in)
{
	index_close(in);
	if (!in->session) return;
	memset(&in->req, 0, sizeof in->req);
	in->req.op = A68V_OP_CLOSE; in->req.session = in->session;
	drv_frame(&in->req);
	in->session = 0;
}

static void go_delegate(struct inst *in, const char *why)
{
	char t[140], *p = put(t, why);
	if (!in->delegate) {
		p = put(p, in->apple ? ": QuickTime's own decompressor takes it" : ": and QuickTime has no decompressor of its own for it");
		*p = 0;
		vlog(t);
	}
	in->delegate = 1;
	session_close(in);
}

static long do_open(long self)
{
	ComponentDescription d;
	memset(&d, 0, sizeof d);
	short ie = cm_get_info(self, &d);
	if (!ie && d.type == TYPE('e', 'a', 't', ' ')) {         /* the MPEG-1 import */
		struct imp *ip;
		return imp_open(self, &ip);
	}
	struct inst *in = mm_newptr_clear(sizeof *in);
	if (!in) return -108;                    /* memFullErr */
	in->type = TYPE('i', 'm', 'd', 'c');
	in->self = self;
	cm_set_storage(self, (long)in);
	if (!ie) in->codec = d.subType;          /* which of the five we were opened as */
	if (in->codec) in->apple = open_apple(in->codec);
	return 0;
}
/* how fast the pictures came, whoever decoded them: "N pictures in S.ss s, F.f a second" */
static char *rate(char *p, struct inst *in)
{
	unsigned long t = in->last_tick - in->first_tick, fps10;
	p = num(p, in->pictures); p = put(p, " pictures in ");
	unsigned long cs = t % 60 * 100 / 60;
	p = num(p, t / 60); *p++ = '.'; *p++ = (char)('0' + cs / 10); *p++ = (char)('0' + cs % 10); p = put(p, " s");
	if (t && in->pictures > 1) {
		fps10 = (in->pictures - 1) * 600 / t;
		p = put(p, ", "); p = num(p, fps10 / 10); *p++ = '.'; p = num(p, fps10 % 10); p = put(p, " a second");
	}
	return p;
}
static long do_close(struct inst *in)
{
	if (!in) return 0;
	if (in->pictures) {
		char t[260], *p = put(t, name_of(in->codec));
		p = put(p, in->delegate ? ": QuickTime's own decompressor: " : ": the card: ");
		p = rate(p, in);
		p = put(p, "; a picture took "); p = num(p, in->us_n ? in->us_total / in->us_n : 0); p = put(p, " us (at most ");
		p = num(p, in->us_max); p = put(p, ") in the decompressor");
		if (in->jumps) {
			p = put(p, "; "); p = num(p, in->jumps); p = put(p, " jumps caught up with "); p = num(p, in->caught); p = put(p, " samples");
		}
		if (in->frames) {
			p = put(p, "; "); p = num(p, in->frames); p = put(p, " by the card ("); p = num(p, in->drawn); p = put(p, " drawn, ");
			p = num(p, in->errors); p = put(p, " refused), on the Pi "); p = num(p, in->t_decode / in->frames); p = put(p, " us to decode and ");
			p = num(p, in->t_draw / in->frames); p = put(p, " to draw");
		}
		*p = 0;
		vlog(t);
		p = put(t, "flags seen: "); p = num(p, in->predecompress); p = put(p, " PreDecompress, condition $"); p = hex(p, (unsigned long)in->cond_or); p = put(p, ", caller $"); p = hex(p, (unsigned long)in->flags_or & 0xFFFF); *p = 0;
		vlog(t);
	}
	session_close(in);
	if (in->apple) cm_close(in->apple);
	mm_disposeptr(in);
	return 0;
}

static long get_info(struct inst *in, CodecInfo *ci, long subtype)
{
	const char *n = name_of(subtype);
	unsigned i;
	memset(ci, 0, sizeof *ci);
	for (i = 0; n[i] && i < 31; i++) ci->typeName[i + 1] = (unsigned char)n[i];
	ci->typeName[0] = (unsigned char)i;
	ci->version = 1; ci->revisionLevel = 1;
	ci->vendor = A68_VENDOR;
	ci->decompressFlags = codecInfoDoes1 | codecInfoDoes2 | codecInfoDoes4 | codecInfoDoes8 | codecInfoDoes16 | codecInfoDoes32 |
	                      codecInfoDoesTemporal;
	switch (subtype) {
	case TYPE('c', 'v', 'i', 'd'): ci->formatFlags = codecInfoDepth24; break;
	case TYPE('r', 'p', 'z', 'a'): ci->formatFlags = codecInfoDepth16; break;
	case TYPE('s', 'm', 'c', ' '): ci->formatFlags = codecInfoDepth8; break;
	case TYPE('m', 'p', 'g', '1'): ci->formatFlags = codecInfoDepth24; break;
	default: ci->formatFlags = codecInfoDepth8 | codecInfoDepth16 | codecInfoDepth24 | codecInfoDepth32;
	}
	ci->decompressionAccuracy = 128;
	ci->decompressionSpeed = 1;              /* milliseconds for 320 x 240 "on a Mac II": the Pi's, and the fastest */
	ci->minimumHeight = ci->minimumWidth = 1;
	(void)in;
	return 0;
}

static int depth_ok(int d) { return d == 1 || d == 2 || d == 4 || d == 8 || d == 16 || d == 32; }

static long pre_decompress(struct inst *in, CodecDecompressParams *p, void *params)
{
	unsigned char *desc = mm_strip(*p->imageDescription);
	long t = *(long *)(desc + 4);
	short w = *(short *)(desc + 32), h = *(short *)(desc + 34), depth = *(short *)(desc + 82);
	int dst = p->dstPixMap.pixelSize;
	if (in->delegate) return in->apple ? cm_delegate(params, in->apple) : codecUnimpErr;
	if (!in->codec) { in->codec = t; in->apple = open_apple(t); }
	if (!drv_open()) { go_delegate(in, "the card's driver did not open"); return in->apple ? cm_delegate(params, in->apple) : codecUnimpErr; }
	if (!drv_enabled()) {
		char m[80], *q = put(m, "frame decoding is switched off");
		if (drv_err) { q = put(q, " (the driver's status: error -"); q = num(q, (unsigned long)-drv_err); q = put(q, ")"); }
		*q = 0;
		go_delegate(in, m);
		return in->apple ? cm_delegate(params, in->apple) : codecUnimpErr;
	}
	if (!depth_ok(dst)) { go_delegate(in, "a destination depth the card does not draw"); return in->apple ? cm_delegate(params, in->apple) : codecUnimpErr; }
	/* a session for this image, kept while the image stays the same */
	if (in->session && (in->w != w || in->h != h || in->depth != depth)) session_close(in);
	if (!in->session) {
		memset(&in->req, 0, sizeof in->req);
		in->req.op = A68V_OP_OPEN;
		in->req.codec = (unsigned long)t; in->req.width = (unsigned long)w; in->req.height = (unsigned long)h;
		in->req.depth = (unsigned long)(unsigned short)depth;
		in->req.desc = (unsigned long)desc;
		in->req.desc_len = (unsigned long)*(long *)desc;          /* idSize: the description with its colour table */
		if (drv_frame(&in->req) || in->req.result) {
			char m[80], *q = put(m, "the card did not take ");
			q = fourcc(q, t); q = put(q, " "); q = num(q, (unsigned long)w); q = put(q, "x"); q = num(q, (unsigned long)h);
			q = put(q, " at depth "); q = num(q, (unsigned long)depth); q = put(q, " (its answer "); q = num(q, in->req.result); q = put(q, ")"); *q = 0;
			go_delegate(in, m);
			return in->apple ? cm_delegate(params, in->apple) : codecUnimpErr;
		}
		in->session = in->req.session;
		in->w = w; in->h = h; in->depth = depth;
		if (t == TYPE('m', 'p', 'g', '1')) index_open(in, desc, *(long *)desc);
		char m[120], *q = put(m, name_of(t));
		q = put(q, " "); q = num(q, (unsigned long)w); q = put(q, "x"); q = num(q, (unsigned long)h);
		q = put(q, ": decoded by the card into a "); q = num(q, (unsigned long)dst); q = put(q, "-bit pixel map"); *q = 0;
		vlog(m);
	}
	in->seq = p->sequenceID;
	CodecCapabilities *c = p->capabilities;
	c->flags = 0;                             /* no scaling, masking or matting of its own: QuickTime does those */
	c->wantedPixelSize = (short)dst;
	c->extendWidth = c->extendHeight = 0;
	c->bandMin = h; c->bandInc = h;           /* the whole picture at once */
	c->time = 0;
	return 0;
}

static long band_decompress(struct inst *in, CodecDecompressParams *p, void *params)
{
	if (in->delegate || !in->session) return in->apple ? cm_delegate(params, in->apple) : codecUnimpErr;
	if (p->dataProc) {                        /* the picture comes in pieces: QuickTime's own decoder knows how */
		go_delegate(in, "a picture loaded in pieces");
		return in->apple ? cm_delegate(params, in->apple) : codecSpoolErr;
	}
	PixMap *pm = &p->dstPixMap;
	struct a68v_freq *r = &in->req;
	memset(r, 0, sizeof *r);
	r->op = A68V_OP_DECODE;
	r->session = in->session;
	r->src = (unsigned long)mm_strip(p->data);
	r->src_len = (unsigned long)p->bufferSize;
	if (r->src_len > A68V_MAX_DATA) r->src_len = A68V_MAX_DATA;
	r->dst_base = (unsigned long)mm_strip(pm->baseAddr);
	r->dst_rowbytes = (unsigned long)(pm->rowBytes & 0x3FFF);
	r->dst_depth = (unsigned long)pm->pixelSize;
	r->dst_x = p->dstRect.left - pm->bounds.left;
	r->dst_y = p->dstRect.top - pm->bounds.top;
	r->dst_w = in->w; r->dst_h = in->h;
	r->clip_left = 0; r->clip_top = 0;
	r->clip_right = pm->bounds.right - pm->bounds.left; r->clip_bottom = pm->bounds.bottom - pm->bounds.top;
	if (pm->pixelSize <= 8 && pm->pmTable && *pm->pmTable) {
		unsigned char *ct = mm_strip(*pm->pmTable);
		r->ctab = (unsigned long)ct;
		r->ctab_seed = *(unsigned long *)ct;
	}
	if (p->callerFlags & codecFlagNoScreenUpdate) r->flags |= A68V_FF_NODRAW;
	r->frame = (unsigned long)p->frameNumber;
	if (in->codec == TYPE('m', 'p', 'g', '1') && in->film) {
		struct a68v_freq keep = *r;              /* catch_up uses the request block */
		catch_up(in, p);
		*r = keep;
		in->last_k = p->frameNumber - 1;
	}
	short e = drv_frame(r);
	if (e || (r->result && r->result != A68V_FE_DATA)) {
		in->errors++;
		if (r->result != in->last_err) {
			char m[80], *q = put(m, name_of(in->codec));
			q = put(q, ": picture "); q = num(q, in->frames); q = put(q, " refused by the card, its answer ");
			q = num(q, e ? 1000 + (unsigned long)-e : r->result); *q = 0;
			vlog(m);
			in->last_err = r->result;
		}
		if (r->result == A68V_FE_ADDR || r->result == A68V_FE_OFF || e) {
			go_delegate(in, "the card cannot reach this destination");
			return in->apple ? cm_delegate(params, in->apple) : codecUnimpErr;
		}
	}
	in->frames++;
	if (!(r->flags & A68V_FF_NODRAW)) in->drawn++;
	in->t_decode += r->decode_us; in->t_draw += r->draw_us;
	if (p->completionProc) call_completion(p->completionProc, 0, codecCompletionSource | codecCompletionDest, p->completionRefCon);
	return 0;
}

long c_dispatch(unsigned char *params, struct inst *in)
{
	short what = *(short *)(params + 2);
	unsigned char *a = params + 4;            /* the call's arguments, the last one first */
	if (what != kOpen && in && in->type == TYPE('e', 'a', 't', ' ')) return imp_dispatch(params, (struct imp *)in, what);
	switch (what) {
	case kOpen: return do_open(*(long *)a);
	case kClose: return do_close(in);
	case kVersion: return VERSION;
	case kRegister:                           /* 0: register; anything else: not on this Macintosh */
		if (drv_open()) return 0;
		return 1;
	case kCanDo: {
		short sel = *(short *)a;
		if (sel == kOpen || sel == kClose || sel == kCanDo || sel == kVersion || sel == kRegister ||
		    sel == kGetCodecInfo || sel == kPreDecompress || sel == kBandDecompress || sel == kBusy) return 1;
		return in && in->apple ? cm_delegate(params, in->apple) : 0;
	}
	case kGetCodecInfo: {
		CodecInfo *ci = *(CodecInfo **)a;
		return get_info(in, ci, in ? in->codec : 0);
	}
	case kPreDecompress:
		if (in) { in->predecompress++; in->cond_or |= (*(CodecDecompressParams **)a)->conditionFlags; }
		return pre_decompress(in, *(CodecDecompressParams **)a, params);
	case kBandDecompress: {
		unsigned long t0 = tm_micro();
		long r = band_decompress(in, *(CodecDecompressParams **)a, params);
		if (in) {
			unsigned long us = tm_micro() - t0;
			if (us < 2000000ul) {            /* the testbed's timer has been seen to step back: such a reading is left out */
				in->us_total += us; in->us_n++;
				if (us > in->us_max) in->us_max = us;
			}
			in->flags_or |= (*(CodecDecompressParams **)a)->callerFlags;
			in->cond_or |= (*(CodecDecompressParams **)a)->conditionFlags;
			in->last_tick = LM_TICKS;
			if (!in->pictures++) in->first_tick = in->last_tick;
		}
		return r;
	}
	case kBusy: return in && in->delegate && in->apple ? cm_delegate(params, in->apple) : 0;
	}
	if (in && in->apple) return cm_delegate(params, in->apple);
	return badComponentSelector;
}
