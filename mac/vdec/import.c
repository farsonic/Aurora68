/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/vdec/import.c - the Aurora68 Video Decoder's MPEG-1 movie import: MoviePlayer, and any QuickTime
 * program, opens an MPEG-1 file (.mpg, file types 'MPEG' and 'MPG ') as a QuickTime movie whose pictures the
 * card decodes (docs/16-video-card.md "MPEG-1 in QuickTime", docs/08 item 113, GitHub issue #25).
 *
 * Backstory (23 November 2026). QuickTime 2.5 on a 68k has no MPEG (Apple's MPEG extension wanted a
 * PowerPC), but it has the extension point every format of the period used: a movie import component,
 * type 'eat ', subtype the file type, which QuickTime asks to turn a file into tracks of a movie. Marked
 * canMovieImportInPlace, it may leave the data where it is: the movie's samples are references into the
 * original file, and MoviePlayer opens the file as it opens a movie, with no "Convert..." and no copy.
 * MovieImportFile here:
 *  1. reads the file once, 32 KB at a time, through the indexer (mpgindex.c): every picture's start and
 *     type, the sequence header, the MP2 sound;
 *  2. makes the video track: one sample per picture, in display order (mi_order: QuickTime 2.5 has no
 *     separate decoding order, so the first B-picture after a reference picture also carries the
 *     reference's bytes, and the reference's own sample comes when it is shown), each a byte range of the
 *     file, of the card's type 'mpg1' (the ImageDescription carries the sequence header), I-pictures the
 *     sync samples, every sample one frame long at the film's rate (25, 29.97...: the media's time scale
 *     is the rate's numerator, 30000 with samples of 1001 for 29.97);
 *  3. makes the sound track: the MP2 frames decoded by the card while the file is read (a sound session,
 *     A68V_OP_SOUND), into 16-bit stereo PCM at the film's rate, written to a file in the Preferences
 *     folder's "Aurora68 Video Sounds" (kept for the next opening of the same film, the oldest deleted
 *     beyond eight). QuickTime 2.5's sound media takes only uncompressed sound or formats of a fixed
 *     compression ratio, whose bytes lie one after the other in a file; MP2 at 44.1 kHz has frames of 417
 *     and 418 bytes, and in a program stream they are spread between the video's packets, so neither the
 *     film's own bytes nor a Sound Manager decompressor ('sdec') could serve. The PCM is what the Sound
 *     Manager plays; the card has done the decoding once;
 *  4. without the card, or with its frame decoding switched off, still opens the file: the video track is
 *     made (and disabled) and a text track says that the film needs the card.
 *
 * The component's code is the decompressors' (vdec.c, entry.s, one 'cdec' resource); Toolbox calls by
 * selector come from traps.txt through tools/mac/mktraps.py. Logged as "[MAC] A68 Video Decoder: MPEG-1
 * import: ...".
 */
#include "vdec.h"
#include "mpgindex.h"
#include "toolbox_gen.h"

enum { kOpen = -1, kClose = -2, kCanDo = -3, kVersion = -4, kRegister = -5, kTarget = -6 };
enum { kImportFile = 2, kSetMediaFile = 5, kSetChunkSize = 7, kSetProgressProc = 8, kValidate = 0x0E, kGetFileType = 0x0F };
#define movieImportMustUseTrack 4                /* (movieImportCreateTrack, 1, is not set when QuickTime imports in place) */
#define movieImportResultUsedMultipleTracks 8
#define mediaSampleNotSync      1
#define noErr        0
#define memFullErr   (-108)
#define paramErr     (-50)
#define fnfErr       (-43)
#define noMovieFound (-2048)
#define fsRdPerm     1
#define fsRdWrPerm   3
#define VERSION      0x00010000L

struct imp {
	long type;                   /* 'eat ' (vdec.c's struct inst starts the same way) */
	long self;
	long sub;                    /* 'MPEG' or 'MPG ' */
	unsigned long imports;
};

long imp_open(long self, struct imp **out)
{
	struct imp *in = mm_newptr_clear(sizeof *in);
	if (!in) return memFullErr;
	in->type = TYPE('e', 'a', 't', ' ');
	in->self = self;
	cm_set_storage(self, (long)in);
	*out = in;
	return 0;
}

/* ---- reading the film ---------------------------------------------------------------------------- */
#define CHUNK     32768L
#define AUD_FLUSH 8192u              /* MP2 bytes handed to the card at a time */
#define PCM_ROOM  65536L
#define SND_HDR   512L               /* the sound file's header; the PCM after it */
#define SND_BLOCK 88200L             /* the PCM in blocks of half a second at 44.1 kHz, 4 bytes apart: one chunk each in
                                        the sound track (a single chunk of the whole sound left playback stuck at the start) */
#define SND_GAP   4L
#define KEEP_SOUNDS 8                /* sound files kept besides the one in use */

struct job {
	struct mi m;
	uint32_t *pics; uint32_t npics, cap; int nomem;
	/* the sound */
	int card, decode_sound;
	unsigned long session;
	uint8_t *aud; uint32_t an;
	uint8_t *pcm;
	short snd_ref;               /* the sound file, open for writing; 0 when none */
	long snd_at;                 /* where the next PCM goes in it */
	uint32_t rate, pcm_bytes, mp2_bytes;
	short snd_err;
	struct a68v_freq req;
	/* the video samples, in batches for AddMediaSampleReferences */
	void *media; void *desc;
	struct { long off, size, dur, n; short flags; } __attribute__((packed)) refs[128];
	uint32_t nref, samples, syncs; long dur; short err;
};
_Static_assert(sizeof(((struct job *)0)->refs[0]) == 18, "SampleReferenceRecord");

static void on_pic(void *u, uint32_t off, uint32_t type)
{
	struct job *j = u;
	if (j->npics == j->cap) {
		uint32_t cap = j->cap ? j->cap * 2 : 4096;
		uint32_t *p = mm_newptr((long)cap * 4);
		if (!p) { j->nomem = 1; return; }
		if (j->pics) { memcpy(p, j->pics, j->npics * 4); mm_disposeptr(j->pics); }
		j->pics = p; j->cap = cap;
	}
	j->pics[j->npics++] = off << 2 | type;
}

static short write_at(short ref, long at, const void *p, long n)
{
	IOParam pb;
	memset(&pb, 0, sizeof pb);
	pb.ioRefNum = ref; pb.ioBuffer = (void *)p; pb.ioReqCount = n; pb.ioPosMode = 1; pb.ioPosOffset = at;
	return fm_write(&pb);
}
static short read_at(short ref, long at, void *p, long n, long *got)
{
	IOParam pb;
	memset(&pb, 0, sizeof pb);
	pb.ioRefNum = ref; pb.ioBuffer = p; pb.ioReqCount = n; pb.ioPosMode = 1; pb.ioPosOffset = at;
	short e = fm_read(&pb);
	*got = pb.ioActCount;
	return e == -39 ? 0 : e;     /* eofErr: what was there */
}
static void close_file(short ref) { IOParam pb; memset(&pb, 0, sizeof pb); pb.ioRefNum = ref; fm_close(&pb); }

/* MP2 bytes to the card, PCM back and into the sound file; n 0: the rest that waits */
static void sound_flush(struct job *j)
{
	int first = 1;
	for (;;) {
		struct a68v_freq *r = &j->req;
		memset(r, 0, sizeof *r);
		r->op = A68V_OP_SOUND; r->session = j->session;
		r->src = first ? (unsigned long)j->aud : 0; r->src_len = first ? j->an : 0;
		r->dst_base = (unsigned long)j->pcm; r->dst_rowbytes = PCM_ROOM;
		first = 0;
		if (drv_frame(r) || r->result) { j->decode_sound = 0; j->snd_err = (short)(r->result ? r->result : 1); break; }
		if (r->depth) j->rate = r->depth;
		for (long done = 0; done < (long)r->width;) {    /* into the blocks, SND_GAP bytes between them */
			long in_block = (long)j->pcm_bytes % SND_BLOCK, k = SND_BLOCK - in_block;
			if (k > (long)r->width - done) k = (long)r->width - done;
			if (write_at(j->snd_ref, j->snd_at, j->pcm + done, k)) { j->decode_sound = 0; j->snd_err = -34; break; }   /* dskFulErr */
			j->snd_at += k; j->pcm_bytes += (uint32_t)k; done += k;
			if ((long)j->pcm_bytes % SND_BLOCK == 0) j->snd_at += SND_GAP;
		}
		if (!j->decode_sound) break;
		if (!r->height) break;
	}
	j->mp2_bytes += j->an;
	j->an = 0;
}
static void on_aud(void *u, const uint8_t *p, uint32_t n)
{
	struct job *j = u;
	while (n && j->decode_sound) {
		uint32_t k = AUD_FLUSH - j->an;
		if (k > n) k = n;
		memcpy(j->aud + j->an, p, k);
		j->an += k; p += k; n -= k;
		if (j->an == AUD_FLUSH) sound_flush(j);
	}
}

/* ---- the sound file ------------------------------------------------------------------------------- */
struct sndhdr {                       /* the first bytes of a sound file: which film it was made from */
	long magic, version;          /* 'A68s', 1 */
	long film_size; unsigned long film_mod; long film_dir;
	unsigned char film_name[32];
	long rate, channels, frames, pcm_bytes, complete;
};
static int sound_folder(short *vref, long *dir)
{
	short pv; long pd;
	FSSpec fs;
	if (FindFolder((short)0x8000, TYPE('p', 'r', 'e', 'f'), 1, &pv, &pd)) return 0;   /* kOnSystemDisk, kPreferencesFolderType */
	short e = FSMakeFSSpec(pv, pd, (const unsigned char *)"\x15" "Aurora68 Video Sounds", &fs);
	if (e == fnfErr) return !FSpDirCreate(&fs, 0, dir) && (*vref = pv, 1);
	if (e) return 0;
	CInfoPB pb;
	memset(&pb, 0, sizeof pb);
	pb.ioNamePtr = fs.name; pb.ioVRefNum = fs.vRefNum; pb.ioDirID = fs.parID;
	if (fm_getcatinfo(&pb) || !(pb.ioFlAttrib & 0x10)) return 0;
	*vref = pv; *dir = pb.ioDirID;
	return 1;
}
/* the oldest files beyond KEEP_SOUNDS deleted (one that a movie still has open stays: FSpDelete refuses) */
static void sound_prune(short vref, long dir, const unsigned char *keep)
{
	for (int round = 0; round < 8; round++) {
		unsigned char name[64], oldest[64];
		unsigned long oldest_mod = 0xFFFFFFFFul;
		int files = 0;
		oldest[0] = 0;
		for (short i = 1; i < 200; i++) {
			CInfoPB pb;
			memset(&pb, 0, sizeof pb);
			name[0] = 0;
			pb.ioNamePtr = name; pb.ioVRefNum = vref; pb.ioDirID = dir; pb.ioFDirIndex = i;
			if (fm_getcatinfo(&pb)) break;
			if (pb.ioFlAttrib & 0x10) continue;
			int same = name[0] == keep[0];
			for (int k = 1; same && k <= name[0]; k++) same = name[k] == keep[k];
			if (same) continue;
			files++;
			if (pb.ioFlMdDat < oldest_mod) { oldest_mod = pb.ioFlMdDat; memcpy(oldest, name, (unsigned long)name[0] + 1); }
		}
		if (files < KEEP_SOUNDS || !oldest[0]) return;
		FSSpec fs;
		if (FSMakeFSSpec(vref, dir, oldest, &fs) || FSpDelete(&fs)) return;
	}
}
/* the film's sound file: an old one made from the same film (*reuse), or a new one open for writing */
static int sound_file(struct job *j, const FSSpec *film, const CInfoPB *fi, FSSpec *out, struct sndhdr *h, int *reuse)
{
	short vref; long dir;
	*reuse = 0;
	if (!sound_folder(&vref, &dir)) return 0;
	unsigned char name[32];
	int n = film->name[0] > 25 ? 25 : film->name[0];
	for (int round = 1; round <= 9; round++) {
		memcpy(name + 1, film->name + 1, (unsigned long)n);
		char *q = (char *)name + 1 + n;
		q = put(q, " sound");
		if (round > 1) { *q++ = ' '; *q++ = (char)('0' + round); }
		name[0] = (unsigned char)(q - (char *)name - 1);
		if (round == 1) sound_prune(vref, dir, name);
		short e = FSMakeFSSpec(vref, dir, name, out);
		if (e && e != fnfErr) return 0;
		if (!e) {                                     /* there already: the same film's, complete? */
			short ref;
			if (FSpOpenDF(out, fsRdWrPerm, &ref) == 0) {
				long got = 0;
				struct sndhdr o;
				memset(&o, 0, sizeof o);
				read_at(ref, 0, &o, sizeof o, &got);
				int same = got == sizeof o && o.magic == TYPE('A', '6', '8', 's') && o.version == 2 && o.complete &&
				           o.film_size == fi->ioFlLgLen && o.film_mod == fi->ioFlMdDat && o.film_dir == film->parID;
				for (int k = 0; same && k <= film->name[0]; k++) same = o.film_name[k] == film->name[k];
				if (same) {
					write_at(ref, 0, &o, sizeof o);   /* its date renewed: the newest of the folder */
					close_file(ref);
					*h = o; *reuse = 1;
					return 1;
				}
				close_file(ref);
			}
			if (FSpDelete(out)) continue;             /* another movie's, open: the next name */
		}
		if (FSpCreate(out, TYPE('A', '6', '8', 'v'), TYPE('A', '6', '8', 's'), 0)) return 0;
		if (FSpOpenDF(out, fsRdWrPerm, &j->snd_ref)) { j->snd_ref = 0; return 0; }
		memset(h, 0, sizeof *h);
		h->magic = TYPE('A', '6', '8', 's'); h->version = 2;   /* 2: blocks of SND_BLOCK bytes, SND_GAP apart */
		h->film_size = fi->ioFlLgLen; h->film_mod = fi->ioFlMdDat; h->film_dir = film->parID;
		memcpy(h->film_name, film->name, (unsigned long)film->name[0] + 1);
		write_at(j->snd_ref, 0, h, sizeof *h);
		j->snd_at = SND_HDR;
		return 1;
	}
	return 0;
}

/* ---- the movie ------------------------------------------------------------------------------------ */
static void refs_flush(struct job *j)
{
	long t;
	if (!j->nref || j->err) { j->nref = 0; return; }
	j->err = AddMediaSampleReferences(j->media, j->desc, (long)j->nref, j->refs, &t);
	j->nref = 0;
}
static void emit(void *u, uint32_t off, uint32_t size, int flags)
{
	struct job *j = u;
	int sync = flags & MI_SYNC;
	j->refs[j->nref].off = (long)off; j->refs[j->nref].size = (long)size;
	j->refs[j->nref].dur = j->dur; j->refs[j->nref].n = 1;
	j->refs[j->nref].flags = sync ? 0 : mediaSampleNotSync;
	j->samples++; j->syncs += (uint32_t)sync;
	if (++j->nref == sizeof j->refs / sizeof j->refs[0]) refs_flush(j);
}
static void *new_handle_from(const void *p, long n)
{
	void *h = mm_newhandle(n);
	if (h) memcpy(*(void **)h, p, (unsigned long)n);
	return h;
}

/* the text track that says what is missing: a sample of the text media in memory ('hndl' data) */
static void say_in_movie(long movie, long at, long seconds600, uint32_t w, uint32_t h, const char *text)
{
	void *track = NewMovieTrack(movie, (long)w << 16, (long)h << 16, 0);
	if (!track) return;
	void *data = mm_newhandle(0), *ref = mm_newhandle(4);
	if (!data || !ref) return;
	**(void ***)ref = data;
	void *media = NewTrackMedia(track, TYPE('t', 'e', 'x', 't'), 600, ref, TYPE('h', 'n', 'd', 'l'));
	if (!media) return;
	unsigned char d[60];
	memset(d, 0, sizeof d);
	d[3] = 60;                                                   /* descSize */
	d[4] = 't'; d[5] = 'e'; d[6] = 'x'; d[7] = 't';
	/* displayFlags 0 (16-19); textJustification teCenter (20-23); bgColor black (24-29) */
	d[23] = 1;
	/* defaultTextBox (30-37): the track's rectangle, top 0, left 0, bottom h, right w */
	d[34] = (unsigned char)(h >> 8); d[35] = (unsigned char)h; d[36] = (unsigned char)(w >> 8); d[37] = (unsigned char)w;
	/* defaultStyle (38-57): scrpStartChar 0, scrpHeight 16, scrpAscent 12, font 0 (the system font), bold, size 12, white;
	   defaultFontName (58) empty */
	d[43] = 16; d[45] = 12; d[48] = 1; d[51] = 12;
	d[52] = d[53] = d[54] = d[55] = d[56] = d[57] = 0xFF;
	void *desc = new_handle_from(d, sizeof d);
	unsigned long n = 0;
	while (text[n]) n++;
	unsigned char s[200];
	s[0] = (unsigned char)(n >> 8); s[1] = (unsigned char)n;
	memcpy(s + 2, text, n);
	void *sample = new_handle_from(s, (long)n + 2);
	long t;
	if (!desc || !sample || BeginMediaEdits(media)) return;
	short e = AddMediaSample(media, sample, 0, (long)n + 2, seconds600, desc, 1, 0, &t);
	EndMediaEdits(media);
	if (!e) e = InsertMediaIntoTrack(track, at, 0, GetMediaDuration(media), 0x10000);
	SetTrackLayer(track, -1);
	mm_disposehandle(sample); mm_disposehandle(desc);
	if (e) { char m[60], *q = put(m, "MPEG-1 import: the text track failed, error -"); q = num(q, (unsigned long)-e); *q = 0; vlog(m); }
}

static long import_file(struct imp *in, const FSSpec *film, long movie, long *used, long at, long *added, long flags, long *outflags)
{
	if (flags & movieImportMustUseTrack) return paramErr;       /* only new tracks: there is no track an MPEG film goes into */
	struct job *j = mm_newptr_clear(sizeof *j);
	uint8_t *buf = mm_newptr(CHUNK);
	char msg[200], *q;
	long result = noErr;
	short ref = 0;
	if (!j || !buf) { if (j) mm_disposeptr(j); if (buf) mm_disposeptr(buf); return memFullErr; }
	in->imports++;
	CInfoPB fi;
	memset(&fi, 0, sizeof fi);
	fi.ioNamePtr = (unsigned char *)film->name; fi.ioVRefNum = film->vRefNum; fi.ioDirID = film->parID;
	fm_getcatinfo(&fi);
	unsigned long t0 = LM_TICKS;
	/* the card: is it there, and does it decode? */
	j->card = drv_open() && drv_enabled();
	int card_present = drv_open();
	FSSpec sfs; struct sndhdr sh; int reuse = 0, have_sound_file = 0;
	if (j->card) {
		memset(&j->req, 0, sizeof j->req);
		j->req.op = A68V_OP_OPEN; j->req.codec = A68V_CODEC_MP2;
		if (!drv_frame(&j->req) && !j->req.result) j->session = j->req.session;
	}
	if (FSpOpenDF(film, fsRdPerm, &ref)) { ref = 0; result = fnfErr; goto out; }
	if (j->session && (have_sound_file = sound_file(j, film, &fi, &sfs, &sh, &reuse)) && !reuse) {
		j->aud = mm_newptr(AUD_FLUSH); j->pcm = mm_newptr(PCM_ROOM);
		j->decode_sound = j->aud && j->pcm && j->snd_ref;
	}
	mi_init(&j->m, on_pic, on_aud, j);
	for (long at_ = 0;; at_ += CHUNK) {
		long got = 0;
		short e = read_at(ref, at_, buf, CHUNK, &got);
		if (e || got <= 0) break;
		mi_feed(&j->m, buf, (uint32_t)got);
		if (j->nomem) break;
		if (got < CHUNK) break;
	}
	close_file(ref);
	if (j->decode_sound) {
		if (j->an) sound_flush(j);
		j->an = 0;
		if (j->decode_sound) sound_flush(j);          /* what the card still holds */
	}
	if (j->session) {                                 /* the card's sound session closed */
		memset(&j->req, 0, sizeof j->req);
		j->req.op = A68V_OP_CLOSE; j->req.session = j->session;
		drv_frame(&j->req);
	}
	if (j->snd_ref) {
		if (j->decode_sound && j->pcm_bytes) {
			sh.rate = (long)j->rate; sh.channels = 2; sh.pcm_bytes = (long)j->pcm_bytes; sh.frames = (long)(j->pcm_bytes / 4);
			sh.complete = 1;
			write_at(j->snd_ref, 0, &sh, sizeof sh);
			IOParam pb; memset(&pb, 0, sizeof pb); pb.ioRefNum = j->snd_ref; pb.ioMisc = j->snd_at; fm_seteof(&pb);
		}
		close_file(j->snd_ref);
		j->snd_ref = 0;
		if (!(j->decode_sound && j->pcm_bytes)) { FSpDelete(&sfs); have_sound_file = 0; }
	}
	if (j->nomem) { result = memFullErr; goto out; }
	if (!j->m.seq.have || !j->npics) {
		q = put(msg, "MPEG-1 import: no MPEG-1 video in "); memcpy(q, film->name + 1, film->name[0]); q += film->name[0]; *q = 0;
		vlog(msg);
		result = noMovieFound;
		goto out;
	}

	/* the video track */
	uint32_t num_, den;
	mi_rate(j->m.seq.rate_code, &num_, &den);
	if (!num_) { num_ = 25; den = 1; }
	long scale = den == 1001 ? (long)num_ : (long)num_ * 100;
	j->dur = den == 1001 ? 1001 : 100;
	uint8_t d[MI_DESC_MAX];
	uint32_t dl = mi_image_description(d, sizeof d, &j->m);
	void *alias = 0;
	/* the description: 'mpgh' for the card, then 'mpgx', the decompressor's index for catching up after a jump */
	uint32_t xl = j->npics <= MI_INDEX_MAX ? MI_INDEX_HDR + 4 * j->npics : 0;
	j->desc = mm_newhandle((long)(dl + xl));
	if (!j->desc || NewAliasMinimal(film, &alias)) { result = memFullErr; goto out; }
	uint8_t *dp = *(uint8_t **)j->desc;
	memcpy(dp, d, dl);
	if (xl && mi_index_atom(dp + dl, xl, (const uint8_t *)film, (uint32_t)fi.ioFlLgLen, j->m.video_end, j->pics, j->npics) == xl) {
		dl += xl;
		dp[0] = (uint8_t)(dl >> 24); dp[1] = (uint8_t)(dl >> 16); dp[2] = (uint8_t)(dl >> 8); dp[3] = (uint8_t)dl;   /* idSize */
	} else mm_sethandlesize(j->desc, (long)dl);
	void *vtrack = NewMovieTrack(movie, (long)j->m.seq.w << 16, (long)j->m.seq.h << 16, 0);
	j->media = vtrack ? NewTrackMedia(vtrack, TYPE('v', 'i', 'd', 'e'), scale, alias, TYPE('a', 'l', 'i', 's')) : 0;
	if (!j->media) { result = GetMoviesError(); if (!result) result = memFullErr; goto out; }
	mi_order(j->pics, j->npics, j->m.video_end, emit, j);
	refs_flush(j);
	if (!j->err) j->err = InsertMediaIntoTrack(vtrack, at, 0, GetMediaDuration(j->media), 0x10000);
	if (j->err) { result = j->err; goto out; }
	int tracks = 1;
	long seconds600 = (long)((unsigned long)j->npics * den * 600u / num_);

	/* the sound track */
	if (have_sound_file && (reuse ? sh.pcm_bytes : (long)j->pcm_bytes)) {
		long rate = reuse ? sh.rate : (long)j->rate, bytes = reuse ? sh.pcm_bytes : (long)j->pcm_bytes;
		/* no longer than the pictures: a sound track that outlasts the video leaves the window white at the end */
		unsigned long vframes = (unsigned long)j->npics * den / num_ * (unsigned long)rate +
		                        (unsigned long)j->npics * den % num_ * (unsigned long)rate / num_;
		if ((unsigned long)bytes / 4 > vframes) bytes = (long)vframes * 4;
		void *salias = 0;
		void *strack = NewMovieTrack(movie, 0, 0, 0x100);
		void *smedia = strack && !NewAliasMinimal(&sfs, &salias) ?
		               NewTrackMedia(strack, TYPE('s', 'o', 'u', 'n'), rate, salias, TYPE('a', 'l', 'i', 's')) : 0;
		unsigned char sd[36];
		memset(sd, 0, sizeof sd);
		sd[3] = 36; sd[4] = 't'; sd[5] = 'w'; sd[6] = 'o'; sd[7] = 's';   /* 16-bit signed, big-endian */
		sd[25] = 2; sd[27] = 16;                                          /* numChannels, sampleSize */
		sd[32] = (unsigned char)(rate >> 8); sd[33] = (unsigned char)rate; /* sampleRate, 16.16 */
		void *sdesc = new_handle_from(sd, sizeof sd);
		long t;
		short e = smedia && sdesc ? 0 : -1;
		for (long at = 0; !e && at < bytes; at += SND_BLOCK) {
			long k = bytes - at < SND_BLOCK ? bytes - at : SND_BLOCK;
			e = AddMediaSampleReference(smedia, SND_HDR + at / SND_BLOCK * (SND_BLOCK + SND_GAP), k, 1, sdesc, k / 4, 0, &t);
		}
		if (!e) e = InsertMediaIntoTrack(strack, at, 0, GetMediaDuration(smedia), 0x10000);
		if (sdesc) mm_disposehandle(sdesc);
		if (!e) tracks++;
		else { q = put(msg, "MPEG-1 import: the sound track failed, error -"); q = num(q, (unsigned long)-e); *q = 0; vlog(msg); }
	}

	/* without the card: the film opens and says why it shows nothing */
	if (!j->card) {
		SetTrackEnabled(vtrack, 0);
		say_in_movie(movie, at, seconds600, j->m.seq.w, j->m.seq.h, card_present ?
		             "Frame decoding on the Aurora68 Video Decoder card is switched off (Aurora68 Video's Card menu, "
		             "or mac.vdecode=off on the Pi): this MPEG-1 film's pictures need it." :
		             "This MPEG-1 film needs the Aurora68 Video Decoder card: QuickTime has no MPEG-1 decoder on a 68k Macintosh.");
		tracks++;
	}
	if (used) *used = (long)vtrack;
	if (added) *added = GetTrackDuration(vtrack);
	if (outflags && tracks > 1) *outflags |= movieImportResultUsedMultipleTracks;

	/* the log: what was made of it */
	q = put(msg, "MPEG-1 import: ");
	memcpy(q, film->name + 1, film->name[0]); q += film->name[0];
	q = put(q, j->m.kind == MI_PS ? " (program stream): " : " (video stream): ");
	q = num(q, j->m.seq.w); *q++ = 'x'; q = num(q, j->m.seq.h); q = put(q, " at ");
	q = num(q, num_ * 100 / den / 100); *q++ = '.'; { unsigned long c = num_ * 100 / den % 100; *q++ = (char)('0' + c / 10); *q++ = (char)('0' + c % 10); }
	q = put(q, " fps, "); q = num(q, j->npics); q = put(q, " pictures ("); q = num(q, j->m.ni); q = put(q, " I, ");
	q = num(q, j->m.np); q = put(q, " P, "); q = num(q, j->m.nb); q = put(q, " B), "); q = num(q, j->samples); q = put(q, " samples (");
	q = num(q, j->syncs); q = put(q, " sync)");
	*q = 0;
	vlog(msg);
	q = put(msg, "MPEG-1 import: sound: ");
	if (!j->m.aud.have) q = put(q, "none in the film");
	else if (reuse) { q = put(q, "the film's PCM from an earlier opening, "); q = num(q, (unsigned long)sh.pcm_bytes >> 10); q = put(q, " KB"); }
	else if (!have_sound_file || !j->pcm_bytes) {
		q = put(q, !card_present ? "not decoded (no card)" : !j->card ? "not decoded (the card's decoding is switched off)" :
		           !j->session ? "the card took no sound session" : "not decoded, error ");
		if (j->card && j->session) q = num(q, (unsigned long)(j->snd_err < 0 ? -j->snd_err : j->snd_err));
	} else {
		q = num(q, j->mp2_bytes >> 10); q = put(q, " KB of MP2 at "); q = num(q, j->m.aud.rate); q = put(q, " Hz decoded by the card into ");
		q = num(q, j->pcm_bytes >> 10); q = put(q, " KB of 16-bit PCM");
	}
	q = put(q, "; "); q = num(q, (LM_TICKS - t0) * 100 / 60); q = put(q, " cs to import; ");
	q = num(q, (unsigned long)tracks); q = put(q, " tracks");
	*q = 0;
	vlog(msg);
out:
	if (result) { q = put(msg, "MPEG-1 import: failed, error -"); q = num(q, (unsigned long)-result); *q = 0; vlog(msg); }
	if (j->desc) mm_disposehandle(j->desc);
	if (j->pics) mm_disposeptr(j->pics);
	if (j->aud) mm_disposeptr(j->aud);
	if (j->pcm) mm_disposeptr(j->pcm);
	mm_disposeptr(j);
	mm_disposeptr(buf);
	return result;
}

/* MovieImportValidate: an MPEG-1 file starts with a pack or a sequence header */
static long validate(const FSSpec *film, unsigned char *valid)
{
	short ref;
	unsigned char b[4];
	long got = 0;
	*valid = 0;
	if (FSpOpenDF(film, fsRdPerm, &ref)) return noErr;
	read_at(ref, 0, b, 4, &got);
	close_file(ref);
	*valid = got == 4 && b[0] == 0 && b[1] == 0 && b[2] == 1 && (b[3] == 0xBA || b[3] == 0xB3);
	return noErr;
}

long imp_dispatch(unsigned char *params, struct imp *in, short what)
{
	unsigned char *a = params + 4;            /* the call's arguments, the last one first */
	switch (what) {
	case kClose: if (in) mm_disposeptr(in); return 0;
	case kVersion: return VERSION;
	case kCanDo: {
		short sel = *(short *)a;
		return sel == kOpen || sel == kClose || sel == kCanDo || sel == kVersion || sel == kImportFile || sel == kSetMediaFile ||
		       sel == kSetChunkSize || sel == kSetProgressProc || sel == kValidate || sel == kGetFileType;
	}
	case kImportFile:
		return import_file(in, *(const FSSpec **)(a + 28), *(long *)(a + 24), *(long **)(a + 16), *(long *)(a + 12),
		                   *(long **)(a + 8), *(long *)(a + 4), *(long **)a);
	case kValidate: return validate(*(const FSSpec **)(a + 8), *(unsigned char **)a);
	case kGetFileType: **(long **)a = TYPE('M', 'P', 'E', 'G'); return 0;
	case kSetMediaFile: case kSetChunkSize: case kSetProgressProc: return 0;   /* the media stays where it is; no progress shown */
	}
	return badComponentSelector;
}
