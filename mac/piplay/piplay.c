/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/piplay.c - "PiPlay": MP3, AIFF and WAV files played on the Aurora68 sound card.
 *
 * Backstory (21 November 2026, docs/08 item 82). The user's Macintosh SE plays the Mac's sound in
 * stereo on its HDMI monitor through the card (docs/08 item 74), and they wanted to play MP3 files.
 * System 7.1 on a 68k has no MP3 player and the test image has no sound-file player at all; a 68000
 * could not decode MP3 in time anyway. So the card decodes, as some real cards did with a DSP: PiPlay
 * reads the file with the File Manager and hands its bytes, as they are, to the card's stream channel
 * (psmb.h, struct psmb_stream: a 44 KiB byte ring in slot 9's window and a small command block); the
 * Pi's core 1 keeps 2 MiB of its own, decodes MP3 with minimp3, converts the PCM of AIFF and WAV,
 * resamples it to the output's rate and mixes it with the Mac's own sound (psmac_audio.c).
 *
 * What a file is, PiPlay finds from its content, never its type or name: a file copied from a PC has
 * no type at all. An ID3v2 tag ("ID3") or an MPEG audio frame header (eleven sync bits and a valid
 * layer, bit rate and sample rate, confirmed by the next header where the frame's length says) is
 * MPEG audio; "FORM" ... "AIFF" or "AIFC" is AIFF (uncompressed: 'NONE', 'twos', 'sowt'); "RIFF" ...
 * "WAVE" is WAV (PCM, or MPEG layer III inside: format 0x55). The ID3 tag gives the title and the
 * artist; else the window shows the file's name. The tag itself, and an ID3v1 tag at the end, are not
 * sent to the card.
 *
 * The file is read in 16 KB pieces from the event loop, as long as the ring has room; the Pi empties
 * the ring into its own buffer at once, so the Mac fills 2 MiB ahead (some 90 seconds of a 192 kbit/s
 * MP3, 12 s of CD-quality PCM) and playback goes on while a menu is held, a dialog is up or another
 * program is in front (the SIZE resource says canBackground). No Time Manager task is needed for it.
 *
 * One window, after the Human Interface Guidelines of 1992 like the other Pi programs: the title, the
 * artist and the format, a progress line with the time played and the length, Play/Pause and Stop
 * buttons, a volume slider (the Sound control panel's volume applies on top). It has a grow box and a
 * zoom box, and what is in it follows its size: the title and the progress line as wide as the window,
 * the buttons and the slider along its bottom. Its place, size and the volume are kept in "PiPlay
 * Prefs" (the Preferences folder on System 7, the System Folder on System 6) and used again at the next
 * start if that place is still on a screen. Menus: Apple (About PiPlay...), File (Open... Cmd-O, Close
 * Cmd-W, Quit Cmd-Q), Edit (Undo to Clear for desk accessories, dimmed for PiPlay itself), Controls
 * (Play/Pause Cmd-P, Stop Cmd-period, Volume Up Cmd-], Volume Down Cmd-[); the space bar plays and
 * pauses while the window is in front. Closing the window closes the sound; Open brings it back.
 * Errors and the About box come in a small window of their own, centred, the default button outlined.
 * Everything is logged through the INFO service ("[MAC] PiPlay: ..."). It starts at 340 x 106, which
 * fits the SE's 512 x 342 screen, draws in black and white on any screen, and runs under System 6.0.8
 * and System 7 on a 68020 or later. Built with the C kit in mac/c (README.md).
 *
 * Internet radio (PiPlay 1.1, 21 November 2026, docs/08 item 83): a Radio menu with the stations of the
 * text file "Aurora68 Play Stations" (Preferences folder; written at the first start from SomaFM's channels)
 * and Open URL... (Cmd-U) for any plain-http MP3 stream. radio.c fetches it with MacTCP and feeds the
 * same stream channel as an open-ended MPEG stream, icy.c takes the station's answer apart; this file
 * only draws the radio's window (the station, "Now playing", the format and the counts, a buffer meter,
 * the state) and turns Play/Pause and Stop into the radio's.
 */
#include "toolbox.h"
#include "psmb.h"
#include "icy.h"
#include "radio.h"

/* piplay/fs.s */
short mac_PBRead(ParamBlock *pb), mac_PBGetEOF(ParamBlock *pb), mac_PBHOpen(ParamBlock *pb), mac_PBHCreate(ParamBlock *pb),
      mac_PBHGetFInfo(ParamBlock *pb), mac_PBHSetFInfo(ParamBlock *pb);
short mac_FindFolder(short vRefNum, long type, int create, short *vref, long *dirid);
#define eofErr (-39)
/* piplay/net.s */
void mac_HLock(void *h), mac_HUnlock(void *h);

/* ---- the card -------------------------------------------------------------------------------- */
static volatile unsigned char *win_base;
volatile struct psmb_stream *ps;               /* radio.c writes the ring too */
static volatile struct psmb_sound *snd;
volatile unsigned char *ring;
static unsigned char pbuf[256];

#define INFO_CMD 0x1000
static unsigned char logline[128];
void pi_log(const char *text)
{
	static const char who[] = "A68 Play: ";
	unsigned long n = 0, k, req;
	if (!win_base) return;
	for (k = 0; who[k]; k++) logline[n++] = (unsigned char)who[k];
	for (k = 0; text[k] && n < sizeof logline - 1; k++) logline[n++] = (unsigned char)text[k];
	volatile unsigned char *c = win_base + INFO_CMD;
	volatile unsigned long *rq = (volatile unsigned long *)c, *dn = (volatile unsigned long *)(c + 4);
	short sr;
	__asm__ volatile("move.w %%sr,%0\n\tori.w #0x0700,%%sr" : "=d"(sr) : : "cc");
	req = *rq;
	for (k = 0; *dn != req && k < 2000000; k++) { }
	*(volatile unsigned short *)(c + 8) = 2;                 /* PSMB_INFO_LOG */
	*(volatile unsigned long *)(c + 28) = (unsigned long)logline;
	*(volatile unsigned long *)(c + 32) = n;
	*rq = req + 1;
	for (k = 0; *dn != req + 1 && k < 4000000; k++) { }
	__asm__ volatile("move.w %0,%%sr" : : "d"(sr) : "cc");
}

/* ---- small text helpers ---------------------------------------------------------------------- */
char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
char *put_num(char *p, unsigned long v)
{
	char t[12];
	int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n) *p++ = t[--n];
	return p;
}
static char *put_time(char *p, unsigned long ms)
{
	unsigned long s = ms / 1000;
	p = put_num(p, s / 60);
	*p++ = ':';
	*p++ = (char)('0' + s % 60 / 10);
	*p++ = (char)('0' + s % 10);
	return p;
}
static char *put_pstr(char *p, const unsigned char *s) { int i; for (i = 1; i <= s[0]; i++) *p++ = (char)s[i]; return p; }
static char *put_khz(char *p, unsigned long hz)
{
	p = put_num(p, hz / 1000);
	if (hz % 1000) { *p++ = '.'; p = put_num(p, hz % 1000 / 100); if (hz % 100) p = put_num(p, hz % 100 / 10); }
	return put_str(p, " kHz");
}

/* ---- the file ---------------------------------------------------------------------------------- */
enum { K_NONE, K_MP3, K_AIFF, K_WAV };
static ParamBlock fpb;
static int file_open;
static unsigned char fname[64];
static short fvref;
static int kind;
static unsigned long data_start, data_len, sent;    /* the bytes for the card: [data_start, data_start + data_len) */
static unsigned long pcm_rate, pcm_ch, pcm_bits, pcm_flags;
static char title[100], artist[100], what[40];       /* C strings, Mac Roman */
static unsigned char *iobuf;
#define IOBUF 32768

static short read_at(unsigned long off, void *buf, long n, long *got)
{
	fpb.u.io.ioBuffer = buf; fpb.u.io.ioReqCount = n;
	fpb.u.io.ioPosMode = fsFromStart; fpb.u.io.ioPosOffset = (long)off;
	short e = mac_PBRead(&fpb);
	*got = fpb.u.io.ioActCount;
	if (e == eofErr && *got > 0) e = 0;
	return e;
}
static unsigned long be32(const unsigned char *b) { return (unsigned long)b[0] << 24 | (unsigned long)b[1] << 16 | (unsigned long)b[2] << 8 | b[3]; }
static unsigned long le32(const unsigned char *b) { return (unsigned long)b[3] << 24 | (unsigned long)b[2] << 16 | (unsigned long)b[1] << 8 | b[0]; }
static unsigned be16(const unsigned char *b) { return (unsigned)b[0] << 8 | b[1]; }
static unsigned le16(const unsigned char *b) { return (unsigned)b[1] << 8 | b[0]; }

/* text from a tag into Mac Roman: icy.c's icy_mac_char (Latin-1 through a table, a few marks of
   punctuation of Unicode, a question mark for the rest) */
/* an ID3 text frame: an encoding byte, then the text (0 Latin-1, 1 UTF-16 with a byte order mark,
   2 UTF-16 big-endian, 3 UTF-8) */
static void id3_text(const unsigned char *b, unsigned long n, char *out, int max)
{
	int o = 0, enc = n ? b[0] : 0;
	unsigned long i = 1;
	int le = 0;
	if (enc == 1 && n >= 3) { le = b[1] == 0xFF && b[2] == 0xFE; i = 3; }
	while (i < n && o < max - 1) {
		unsigned long u;
		if (enc == 1 || enc == 2) {
			if (i + 1 >= n) break;
			u = le ? (unsigned long)b[i] | (unsigned long)b[i + 1] << 8 : (unsigned long)b[i] << 8 | b[i + 1];
			i += 2;
		} else if (enc == 3 && b[i] >= 0xC0) {
			int extra = b[i] >= 0xF0 ? 3 : b[i] >= 0xE0 ? 2 : 1;
			u = b[i] & (0x3F >> extra);
			for (i++; extra && i < n; extra--, i++) u = u << 6 | (b[i] & 0x3F);
		} else u = b[i++];
		if (!u) break;
		out[o++] = (char)icy_mac_char(u);
	}
	while (o && out[o - 1] == ' ') o--;
	out[o] = 0;
}
/* the ID3v2 tag at the start: its length (to skip), and the title and the artist */
static unsigned long id3v2(const unsigned char *b, long got)
{
	if (got < 10 || b[0] != 'I' || b[1] != 'D' || b[2] != '3') return 0;
	unsigned long size = (unsigned long)(b[6] & 0x7F) << 21 | (unsigned long)(b[7] & 0x7F) << 14 | (unsigned long)(b[8] & 0x7F) << 7 | (b[9] & 0x7F);
	int ver = b[3];
	unsigned long end = 10 + size, p = 10;
	if (b[5] & 0x40 && ver >= 3) p += ver == 3 ? 4 + be32(b + 10) : ((b[10] & 0x7F) << 21 | (b[11] & 0x7F) << 14 | (b[12] & 0x7F) << 7 | (b[13] & 0x7F));
	while (p + (ver == 2 ? 6 : 10) <= end && p + 10 <= (unsigned long)got && b[p]) {
		unsigned long fs;
		const unsigned char *id = b + p;
		int hl = ver == 2 ? 6 : 10;
		if (ver == 2) fs = (unsigned long)b[p + 3] << 16 | (unsigned long)b[p + 4] << 8 | b[p + 5];
		else if (ver == 3) fs = be32(b + p + 4);
		else fs = (unsigned long)(b[p + 4] & 0x7F) << 21 | (unsigned long)(b[p + 5] & 0x7F) << 14 | (unsigned long)(b[p + 6] & 0x7F) << 7 | (b[p + 7] & 0x7F);
		if (p + hl + fs > (unsigned long)got) break;
		if ((ver == 2 && id[0] == 'T' && id[1] == 'T' && id[2] == '2') || (ver > 2 && id[0] == 'T' && id[1] == 'I' && id[2] == 'T' && id[3] == '2'))
			id3_text(b + p + hl, fs, title, sizeof title);
		if ((ver == 2 && id[0] == 'T' && id[1] == 'P' && id[2] == '1') || (ver > 2 && id[0] == 'T' && id[1] == 'P' && id[2] == 'E' && id[3] == '1'))
			id3_text(b + p + hl, fs, artist, sizeof artist);
		p += hl + fs;
	}
	return end + ((b[5] & 0x10) ? 10 : 0);
}
/* an MPEG audio frame header and its length in bytes, 0 if it is none */
static unsigned long mpeg_frame_len(const unsigned char *h)
{
	static const unsigned short br[2][3][15] = {
		{ { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },       /* MPEG-1: layer I, II, III */
		  { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },
		  { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 } },
		{ { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },          /* MPEG-2 and 2.5 */
		  { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
		  { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 } } };
	static const unsigned short sr[3] = { 44100, 48000, 32000 };
	if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;
	int ver = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3, bri = h[2] >> 4, sri = (h[2] >> 2) & 3, pad = (h[2] >> 1) & 1;
	if (ver == 1 || layer == 0 || bri == 0 || bri == 15 || sri == 3) return 0;
	unsigned long rate = sr[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
	unsigned long kbps = br[ver != 3][3 - layer][bri];
	if (layer == 3) return (12 * kbps * 1000 / rate + pad) * 4;                  /* layer I: slots of 4 bytes */
	if (layer == 1 && ver != 3) return 72 * kbps * 1000 / rate + pad;              /* layer III of MPEG-2 and 2.5: 576 samples */
	return 144 * kbps * 1000 / rate + pad;
}
/* an MPEG frame at b[i] whose next frame is where its length says (or the data ends first) */
static int mpeg_at(const unsigned char *b, long got, long i)
{
	unsigned long n = i + 4 <= got ? mpeg_frame_len(b + i) : 0;
	if (!n) return 0;
	if (i + (long)n + 4 > got) return 1;
	return mpeg_frame_len(b + i + n) != 0 && (b[i + n + 1] & 0xFE) == (b[i + 1] & 0xFE);
}
/* AIFF's sample rate, an 80-bit IEEE extended number, as an integer */
static unsigned long ext_rate(const unsigned char *e)
{
	int ex = (int)(be16(e) & 0x7FFF) - 16383;
	unsigned long m = be32(e + 2);
	if (ex < 0 || ex > 31) return 0;
	return m >> (31 - ex);
}

static const char *err_text;          /* why a file cannot be played */
static char err_buf[160];
/* what the file is; data_start, data_len and the PCM layout for the card */
static int identify(void)
{
	long got = 0, eof;
	fpb.u.io.ioMisc = 0;
	mac_PBGetEOF(&fpb);
	eof = fpb.u.io.ioMisc;
	if (read_at(0, iobuf, IOBUF, &got) || got < 12) { err_text = "The file is too short to be a sound."; return 0; }
	const unsigned char *b = iobuf;
	title[0] = artist[0] = 0;
	if (b[0] == 'F' && b[1] == 'O' && b[2] == 'R' && b[3] == 'M' && b[8] == 'A' && b[9] == 'I' && b[10] == 'F' && (b[11] == 'F' || b[11] == 'C')) {
		int aifc = b[11] == 'C', have_comm = 0;
		unsigned long p = 12, ssnd = 0, ssnd_len = 0, frames = 0;
		unsigned char comm[32], h[16];
		unsigned long comp = TYPE('N', 'O', 'N', 'E');
		while (p + 8 <= (unsigned long)eof) {
			long g;
			if (read_at(p, h, 16, &g) || g < 8) break;
			unsigned long id = be32(h), len = be32(h + 4);
			if (id == TYPE('C', 'O', 'M', 'M')) {
				if (read_at(p + 8, comm, len < 32 ? (long)len : 32, &g) || g < 18) break;
				have_comm = 1;
				pcm_ch = be16(comm); frames = be32(comm + 2); pcm_bits = be16(comm + 6); pcm_rate = ext_rate(comm + 8);
				if (aifc && g >= 22) comp = be32(comm + 18);
			} else if (id == TYPE('S', 'S', 'N', 'D') && g >= 16) {
				ssnd = p + 16 + be32(h + 8);
				ssnd_len = len >= 8 + be32(h + 8) ? len - 8 - be32(h + 8) : 0;
			}
			p += 8 + len + (len & 1);
		}
		if (!have_comm || !ssnd) { err_text = "This AIFF file has no sound in it, or it is damaged."; return 0; }
		if (comp == TYPE('s', 'o', 'w', 't')) pcm_flags = PSMB_STR_PCM_LE;
		else if (comp == TYPE('N', 'O', 'N', 'E') || comp == TYPE('t', 'w', 'o', 's')) pcm_flags = 0;
		else {
			char *q = put_str(err_buf, "This AIFF-C file is compressed (");
			for (int i = 24; i >= 0; i -= 8) *q++ = (char)(comp >> i);
			put_str(q, "), and Aurora68 Play plays only uncompressed sound.")[0] = 0;
			err_text = err_buf;
			return 0;
		}
		if (pcm_bits != 8 && pcm_bits != 16 && pcm_bits != 24) { err_text = "Aurora68 Play plays AIFF files of 8, 16 or 24 bits only."; return 0; }
		data_start = ssnd;
		data_len = frames * pcm_ch * (pcm_bits / 8);
		if (ssnd_len && data_len > ssnd_len) data_len = ssnd_len;
		if (data_start + data_len > (unsigned long)eof) data_len = (unsigned long)eof > data_start ? eof - data_start : 0;
		kind = K_AIFF;
		put_str(what, aifc ? "AIFF-C" : "AIFF")[0] = 0;
		return 1;
	}
	if (b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' && b[8] == 'W' && b[9] == 'A' && b[10] == 'V' && b[11] == 'E') {
		unsigned long p = 12, fmt = 0;
		unsigned char h[8], f[40];
		int have_fmt = 0;
		data_start = 0;
		while (p + 8 <= (unsigned long)eof) {
			long g;
			if (read_at(p, h, 8, &g) || g < 8) break;
			unsigned long len = le32(h + 4);
			if (h[0] == 'f' && h[1] == 'm' && h[2] == 't' && h[3] == ' ') {
				if (read_at(p + 8, f, len < 40 ? (long)len : 40, &g) || g < 16) break;
				have_fmt = 1;
				fmt = le16(f); pcm_ch = le16(f + 2); pcm_rate = le32(f + 4); pcm_bits = le16(f + 14);
				if (fmt == 0xFFFE && g >= 26) fmt = le16(f + 24);         /* WAVE_FORMAT_EXTENSIBLE: the sub-format's first word */
			} else if (h[0] == 'd' && h[1] == 'a' && h[2] == 't' && h[3] == 'a') {
				data_start = p + 8; data_len = len;
				if (data_start + data_len > (unsigned long)eof || !len) data_len = eof - data_start;
				break;
			}
			p += 8 + len + (len & 1);
		}
		if (!have_fmt || !data_start) { err_text = "This WAV file has no sound in it, or it is damaged."; return 0; }
		if (fmt == 0x55) { kind = K_MP3; put_str(what, "MP3 in WAV")[0] = 0; return 1; }
		if (fmt != 1) { err_text = "This WAV file is compressed or in floating point, and Aurora68 Play plays only plain PCM and MP3."; return 0; }
		if (pcm_bits != 8 && pcm_bits != 16 && pcm_bits != 24) { err_text = "Aurora68 Play plays WAV files of 8, 16 or 24 bits only."; return 0; }
		pcm_flags = PSMB_STR_PCM_LE | (pcm_bits == 8 ? PSMB_STR_PCM_UNSIGNED : 0);
		kind = K_WAV;
		put_str(what, "WAV")[0] = 0;
		return 1;
	}
	/* MPEG audio: an ID3v2 tag, or a frame (with zeros or junk before it, within the first 16 KB) */
	unsigned long skip = id3v2(b, got);
	if (skip) {
		if (skip + 16 > (unsigned long)eof) { err_text = "This file holds an ID3 tag but no sound."; return 0; }
	} else {
		long i;
		for (i = 0; i + 4 <= got && !mpeg_at(b, got, i); i++) { }
		if (i + 4 > got) { err_text = "Aurora68 Play does not know this kind of file. It plays MP3 files, and AIFF and WAV files that are not compressed."; return 0; }
		skip = (unsigned long)i;
	}
	data_start = skip;
	data_len = (unsigned long)eof - skip;
	if (eof >= 128 + (long)skip) {                   /* an ID3v1 tag: the last 128 bytes, "TAG" */
		unsigned char t[128];
		long g;
		if (!read_at((unsigned long)eof - 128, t, 128, &g) && g == 128 && t[0] == 'T' && t[1] == 'A' && t[2] == 'G') {
			data_len -= 128;
			if (!title[0]) { unsigned char tt[31]; tt[0] = 0; memcpy(tt + 1, t + 3, 30); id3_text(tt, 31, title, 31); }
			if (!artist[0]) { unsigned char tt[31]; tt[0] = 0; memcpy(tt + 1, t + 33, 30); id3_text(tt, 31, artist, 31); }
		}
	}
	kind = K_MP3;
	put_str(what, "MP3")[0] = 0;
	return 1;
}

/* ---- the stream -------------------------------------------------------------------------------- */
enum { S_NONE, S_PLAYING, S_PAUSED, S_STOPPED, S_ENDED };
static int state;
static int vol = 80;                  /* the slider, 0-100 */
static unsigned long last_pos;

int pi_command(unsigned long c)
{
	unsigned long seq = ps->cmd_seq + 1, t0 = LM_TICKS;
	ps->cmd = c;
	ps->cmd_seq = seq;
	while (ps->done_seq != seq)
		if (LM_TICKS - t0 > 180) { pi_log("the card did not answer a command in three seconds"); return 0; }
	return 1;
}
static void set_volume(void)
{
	unsigned long panel = *(volatile unsigned char *)0x260 & 7;     /* SdVolume: the Sound control panel's level, 0-7 */
	unsigned long v = (unsigned long)vol * 256 / 100 * (panel ? panel : 7) / 7;
	if (ps) ps->volume = v << 16 | v;
}
/* one piece of the file into the ring: 2 if it wrote some, 1 if more remains but the ring is full
   (the Pi's buffer is), 0 at the end */
static int feed(void)
{
	if (!file_open || sent >= data_len) return 0;
	unsigned long room = PSMB_STR_SIZE - (ps->head - ps->tail);
	if (room < 4096 && room < data_len - sent) return 1;
	long n = (long)(data_len - sent < room ? data_len - sent : room), got;
	if (n > IOBUF) n = IOBUF;
	short e = read_at(data_start + sent, iobuf, n, &got);
	if (e || got <= 0) {
		char t[60];
		put_num(put_str(t, "a read failed, error "), (unsigned long)-e)[0] = 0;
		pi_log(t);
		data_len = sent;                       /* the end, then */
	}
	unsigned long h = ps->head;
	for (long k = 0; k < got; ) {
		unsigned long off = (h + (unsigned long)k) % PSMB_STR_SIZE, c = (unsigned long)(got - k);
		if (c > PSMB_STR_SIZE - off) c = PSMB_STR_SIZE - off;
		memcpy((void *)(ring + off), iobuf + k, c);
		k += (long)c;
	}
	sent += (unsigned long)got;
	ps->head = h + (unsigned long)got;
	if (sent >= data_len) ps->eof = 1;
	return sent < data_len ? 2 : 0;
}
static void stop_stream(void)
{
	if (ps && ps->state != PSMB_STR_S_IDLE) pi_command(PSMB_STR_C_STOP);
}
static void say_error(const char *text);
static void redraw_all(void);
static void close_window(void), resized(void);
/* from the start of the open file */
static void start(void)
{
	if (!file_open) return;
	stop_stream();
	ps->eof = 0;
	ps->format = kind == K_MP3 ? PSMB_STR_MPEG : PSMB_STR_PCM;
	ps->pcm_rate = pcm_rate; ps->pcm_channels = pcm_ch; ps->pcm_bits = pcm_bits; ps->pcm_flags = pcm_flags;
	ps->data_bytes = data_len;
	{   /* what is playing, for the Pi's log (and the block a radio stream will use) */
		char t[128];
		int i = 0;
		if (title[0]) for (; title[i] && i < 127; i++) t[i] = title[i];
		else for (; i < fname[0] && i < 127; i++) t[i] = (char)fname[i + 1];
		t[i] = 0;
		memcpy((void *)ps->title, t, (unsigned long)i + 1);
		ps->title_seq = ps->title_seq + 1;
	}
	set_volume();
	sent = 0;
	feed();
	if (!pi_command(PSMB_STR_C_START)) { say_error("The sound card does not answer. Restart the Macintosh, or the Pi."); return; }
	state = S_PLAYING;
	last_pos = 0;
	char t[120], *p = put_str(t, "playing \"");
	p = put_pstr(p, fname);
	p = put_str(p, "\", ");
	p = put_str(p, what);
	p = put_str(p, ", ");
	p = put_num(p, data_len);
	p = put_str(p, " bytes");
	*p = 0;
	pi_log(t);
}
static void close_file(void)
{
	stop_stream();
	if (file_open) mac_PBClose(&fpb);
	file_open = 0;
	state = S_NONE;
}

/* ---- the window -------------------------------------------------------------------------------- */
static void *win, *play_btn, *stop_btn, *vol_bar;
static void *m_apple, *m_file, *m_edit, *m_ctl, *m_radio;
enum { M_APPLE = 128, M_FILE, M_EDIT, M_CTL, M_RADIO };
static int showing_radio;                            /* the window is the radio's (a station chosen) */
static int radio_url_item;                           /* Open URL...'s place in the Radio menu */
enum { F_OPEN = 1, F_CLOSE, F_QUIT = 4 };
enum { C_PLAY = 1, C_STOP, C_LOUDER = 4, C_SOFTER };      /* Volume Up, Volume Down */
static int quit, fg = 1, shown;
static const char *status_text = "";
#define WIN_W 340                                    /* the first and the smallest size */
#define WIN_H 106
static short W = WIN_W, H = WIN_H, vol_label_h;     /* the window's size now; where "Volume" is drawn */
static Rect bar_r = { 44, 10, 52, WIN_W - 10 };      /* the progress line */
/* everything where the window's size puts it: the title, the format and the progress line across the
   top, as wide as the window; the buttons and the volume slider along the bottom, clear of the grow box */
static void layout(void)
{
	Rect *pr = PORT_RECT(win);
	W = pr->right - pr->left; H = pr->bottom - pr->top;
	bar_r.right = W - 10;
	short by = H - 30, sl = W - 24 - 110;
	if (sl < 220) sl = 220;
	MoveControl(play_btn, 10, by);
	MoveControl(stop_btn, 90, by);
	MoveControl(vol_bar, sl, by + 2);
	SizeControl(vol_bar, W - 24 - sl, 16);
	vol_label_h = sl - 48;
}

static void draw_clipped(const char *s, int max_w)
{
	int n = (int)strlen(s);
	if (TextWidth(s, 0, n) <= max_w) { DrawText(s, 0, n); return; }
	while (n > 0 && TextWidth(s, 0, n) + 12 > max_w) n--;
	DrawText(s, 0, n);
	DrawText("\xC9", 0, 1);                     /* an ellipsis */
}
static void info_line(char *t)
{
	char *p = t;
	if (artist[0]) { p = put_str(p, artist); p = put_str(p, " \xD1 "); }   /* an em dash */
	if (kind) {
		p = put_str(p, what);
		if (ps && ps->rate) {
			p = put_str(p, ", "); p = put_khz(p, ps->rate);
			p = put_str(p, ps->channels == 1 ? " mono" : " stereo");
			if (kind == K_MP3 && ps->bitrate) { p = put_str(p, ", "); p = put_num(p, ps->bitrate); p = put_str(p, " kbit/s"); }
			else if (kind != K_MP3) { p = put_str(p, ", "); p = put_num(p, pcm_bits); p = put_str(p, "-bit"); }
		}
	}
	*p = 0;
}
static void radio_progress(void);
static void draw_progress(void)
{
	char t[60], *p = t;
	if (showing_radio) { radio_progress(); return; }
	unsigned long pos = ps ? ps->pos_ms : 0, len = ps ? ps->length_ms : 0;
	if (state == S_NONE || state == S_STOPPED) pos = 0;
	if (state == S_ENDED && len) pos = len;
	SetPort(win);
	Rect r = bar_r;
	FrameRect(&r);
	InsetRect(&r, 1, 1);
	short w = r.right - r.left, fillw = len ? (short)((pos > len ? len : pos) * (unsigned long)w / len) : 0;
	Rect a = r, b = r;
	a.right = r.left + fillw; b.left = r.left + fillw;
	static const unsigned char gray[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
	if (fillw) FillRect(&a, gray);
	if (b.left < b.right) EraseRect(&b);
	Rect tr = { 56, 0, 70, W };
	EraseRect(&tr);
	TextFont(3); TextSize(9);                   /* Geneva 9 */
	if (state != S_NONE) {
		p = put_time(p, pos);
		if (len) { p = put_str(p, " of "); p = put_time(p, len); }
	}
	*p = 0;
	MoveTo(10, 66);
	DrawText(t, 0, (int)strlen(t));
	int sw = TextWidth(status_text, 0, (int)strlen(status_text));
	MoveTo(W - 10 - sw, 66);
	DrawText(status_text, 0, (int)strlen(status_text));
}
static void radio_texts(void);
static void draw_texts(void)
{
	char t[200];
	if (showing_radio) { radio_texts(); return; }
	SetPort(win);
	Rect r = { 0, 0, 42, W };
	EraseRect(&r);
	TextFont(0); TextSize(12);                  /* Chicago: the title */
	MoveTo(10, 19);
	if (state == S_NONE && !file_open) draw_clipped("Open a sound, or choose a station in Radio.", W - 20);
	else if (title[0]) draw_clipped(title, W - 20);
	else { put_pstr(t, fname)[0] = 0; draw_clipped(t, W - 20); }
	TextFont(3); TextSize(9);
	info_line(t);
	MoveTo(10, 35);
	draw_clipped(t, W - 20);
}
/* the radio's window: the station (what it calls itself), "Now playing", the format and the counts;
   a meter of what the card holds (full at 30 seconds), and the state. Lines 31 and 42 are Geneva 9,
   the meter sits between them and the state line, so the smallest window (106 high) still holds it. */
static void radio_info(char *t)
{
	char *p = t;
	const char *why = radio_problem();
	if (why) { p = put_str(p, "Lost: "); p = put_str(p, why); *p = 0; return; }
	p = put_str(p, "MP3");
	if (radio.st == RS_PLAYING && ps->rate) {
		p = put_str(p, ", "); p = put_khz(p, ps->rate);
		p = put_str(p, ps->channels == 1 ? " mono" : " stereo");
	}
	if (radio.br) { p = put_str(p, ", "); p = put_num(p, radio.br); p = put_str(p, " kbit/s"); }
	p = put_str(p, "; "); p = put_num(p, radio.reconnects); p = put_str(p, radio.reconnects == 1 ? " reconnect, " : " reconnects, ");
	p = put_num(p, radio.underruns); p = put_str(p, radio.underruns == 1 ? " underrun" : " underruns");
	if (radio.ipv) p = put_str(p, radio.ipv == 6 ? " \xD1 IPv6" : " \xD1 IPv4");   /* an em dash: Geneva 9 has no middle dot */
	*p = 0;
}
static void radio_texts(void)
{
	char t[200];
	SetPort(win);
	Rect r = { 0, 0, 45, W };
	EraseRect(&r);
	TextFont(0); TextSize(12);                  /* Chicago: the station */
	MoveTo(10, 19);
	draw_clipped(radio.name[0] ? radio.name : radio.station, W - 20);
	TextFont(3); TextSize(9);                   /* Geneva 9 */
	if (radio.title[0]) put_str(put_str(t, "Now playing: "), radio.title)[0] = 0;
	else put_str(t, radio.url)[0] = 0;
	MoveTo(10, 31);
	draw_clipped(t, W - 20);
	radio_info(t);
	MoveTo(10, 42);
	draw_clipped(t, W - 20);
}
static void radio_progress(void)
{
	char t[80], st[40], *p = t;
	SetPort(win);
	Rect r = { 46, 10, 54, (short)(W - 10) };
	FrameRect(&r);
	InsetRect(&r, 1, 1);
	unsigned long held = radio.st == RS_PLAYING || radio.st == RS_RETRY ? radio.held_ms : 0;
	short w = r.right - r.left, fillw = (short)((held > 30000 ? 30000 : held) * (unsigned long)w / 30000);
	Rect a = r, b = r;
	a.right = r.left + fillw; b.left = r.left + fillw;
	static const unsigned char gray[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
	if (fillw) FillRect(&a, gray);
	if (b.left < b.right) EraseRect(&b);
	Rect tr = { 55, 0, 70, W };
	EraseRect(&tr);
	TextFont(3); TextSize(9);
	if (radio.st == RS_PLAYING || radio.st == RS_RETRY) {
		p = put_str(p, "Buffer ");
		p = put_num(p, held / 1000); *p++ = '.'; p = put_num(p, held / 100 % 10); p = put_str(p, " s");
		if (ps->pos_ms) { p = put_str(p, ", "); p = put_time(p, ps->pos_ms); p = put_str(p, " played"); }
	}
	*p = 0;
	MoveTo(10, 66);
	DrawText(t, 0, (int)strlen(t));
	radio_status(st);
	int sw = TextWidth(st, 0, (int)strlen(st));
	MoveTo(W - 10 - sw, 66);
	DrawText(st, 0, (int)strlen(st));
}
static void grow_icon(void)                /* the grow box alone: the window has no scroll bars, so no lines along its edges */
{
	Rect c = { (short)(H - 15), (short)(W - 15), H, W }, all = { -32000, -32000, 32000, 32000 };
	ClipRect(&c);
	DrawGrowIcon(win);
	ClipRect(&all);
}
static void redraw_all(void)
{
	SetPort(win);
	EraseRect(PORT_RECT(win));                  /* within the update region: what a resize left behind goes */
	draw_texts();
	draw_progress();
	DrawControls(win);
	TextFont(3); TextSize(9);
	MoveTo(vol_label_h, H - 16);
	DrawText("Volume", 0, 6);
	grow_icon();
}
static void set_status(const char *s)
{
	status_text = s;
	draw_progress();
}
/* what Play/Pause and Stop do now: a file, or the radio (which pauses into the card's buffer) */
static int playing_now(void) { return showing_radio ? radio.st != RS_OFF && !radio.paused : state == S_PLAYING; }
static int can_stop_now(void) { return showing_radio ? radio.st != RS_OFF : state == S_PLAYING || state == S_PAUSED; }
static void fix_buttons(void)
{
	static int said = -1;                       /* the button's title, set only when it changes (it redraws) */
	if (playing_now() != said) { said = playing_now(); SetCTitle(play_btn, pstr(pbuf, said ? "Pause" : "Play")); }
	int can_play = file_open || showing_radio, can_stop = can_stop_now();
	if (!fg) return;
	HiliteControl(play_btn, can_play ? 0 : 255);
	HiliteControl(stop_btn, can_stop ? 0 : 255);
}
static void enable(void *m, int item, int on)
{
	if (on) EnableItem(m, item);
	else DisableItem(m, item);
}
static int strcmp_(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a != *b; }
/* dim what cannot be done now; Play says Pause while a sound plays */
static void fix_menus(void)
{
	int da = FrontWindow() != win, i;
	for (i = 1; i <= 6; i++) enable(m_edit, i, da && i != 2);
	enable(m_file, F_CLOSE, shown);
	SetItem(m_ctl, C_PLAY, pstr(pbuf, playing_now() ? "Pause" : "Play"));
	enable(m_ctl, C_PLAY, shown && (file_open || showing_radio));
	enable(m_ctl, C_STOP, can_stop_now());
	for (i = 0; i < nstations; i++)             /* the station that plays, checked */
		CheckItem(m_radio, i + 1, showing_radio && radio.st != RS_OFF && !strcmp_(stations[i].url, radio.url));
	enable(m_ctl, C_LOUDER, vol < 100);
	enable(m_ctl, C_SOFTER, vol > 0);
}

/* ---- a small window that says something, with an OK button (no resources: the program has
   none but its code, like the other Pi programs) ----------------------------------------------- */
static Rect screen_rect(void)
{
	char *a5;
	__asm__("move.l %%a5,%0" : "=r"(a5));
	return *(Rect *)(*(char **)a5 - 116);        /* QuickDraw's screenBits.bounds, below thePort */
}
static void message_box(const char *text, int lines_h)
{
	Rect s = screen_rect();
	short w = 340, h = (short)(lines_h + 52), left = (short)((s.right - s.left - w) / 2), top = (short)((s.bottom - s.top - h) / 3 + 10);
	Rect b = { top, left, (short)(top + h), (short)(left + w) };
	void *d = NewWindow(0, &b, pstr(pbuf, ""), 1, 1 /* dBoxProc */, (void *)-1, 0, 0);
	SetPort(d);
	Rect okr = { (short)(h - 30), (short)(w - 80), (short)(h - 10), (short)(w - 16) };
	void *ok = NewControl(d, &okr, pstr(pbuf, "OK"), 1, 0, 0, 1, pushButProc, 0);
	EventRecord ev;
	int done = 0;
	while (!done) {
		if (!WaitNextEvent(everyEvent, &ev, 10, 0)) { if (file_open && state == S_PLAYING) feed(); radio_poll(); continue; }
		if (ev.what == updateEvt && (void *)ev.message == d) {
			BeginUpdate(d);
			SetPort(d);
			TextFont(0); TextSize(12);
			Rect tr = { 14, 16, (short)(h - 36), (short)(w - 16) };
			TextBox(text, (long)strlen(text), &tr, 0);
			DrawControls(d);
			Rect f = okr;                       /* the default button's bold outline */
			InsetRect(&f, -4, -4);
			PenSize(3, 3);
			FrameRoundRect(&f, 16, 16);
			PenNormal();
			EndUpdate(d);
		} else if (ev.what == updateEvt) {
			if ((void *)ev.message == win) { BeginUpdate(win); redraw_all(); EndUpdate(win); }
		} else if (ev.what == keyDown && ((ev.message & 0xFF) == 0x0D || (ev.message & 0xFF) == 0x03 || (ev.message & 0xFF) == 0x1B)) done = 1;
		else if (ev.what == mouseDown) {
			Point p = ev.where;
			void *c = 0;
			SetPort(d);
			GlobalToLocal(&p);
			if (FindControl(pt_long(p), d, &c) && c == ok && TrackControl(ok, pt_long(p), 0)) done = 1;
			else if (!PtInRect(pt_long(p), PORT_RECT(d))) SysBeep(1);
		}
	}
	DisposeWindow(d);
	SetPort(win);
}
static void say_error(const char *text)
{
	char t[200];
	put_str(put_str(t, "error: "), text)[0] = 0;
	pi_log(t);
	SysBeep(5);
	message_box(text, 48);
}
static void about(void)
{
	message_box("Aurora68 Play 1.1 for the Aurora68 card.\r"
	            "Plays MP3 files, AIFF and WAV files that are not compressed, and internet radio: plain "
	            "http:// MP3 streams through MacTCP. The Pi decodes them (minimp3, public domain) and plays "
	            "them on HDMI or its headphone socket, mixed with the Macintosh's own sound.\r"
	            "The stations in the Radio menu are SomaFM's, listener-supported radio (somafm.com), and NPO Radio 2. MPL-2.0.", 172);
}

/* ---- opening a file ------------------------------------------------------------------------------ */
static void show_file(void)
{
	SetPort(win);
	fix_buttons();
	draw_texts();
	draw_progress();
}
static void open_path(short vref, long dirid, const unsigned char *name);
static void open_file(void)
{
	SFReply rep;
	Rect sr = screen_rect();                    /* the dialog (348 by 200) centred, a third of the way down */
	Point where = { (short)(sr.top + (sr.bottom - sr.top - 200) / 3), (short)(sr.left + (sr.right - sr.left - 348) / 2) };
	SFGetFile(pt_long(where), pstr(pbuf, ""), 0, -1, 0, 0, &rep, 2);
	SetPort(win);
	if (!rep.good) return;
	open_path(rep.vRefNum, 0, rep.fName);
}
/* a file by volume (or working directory), directory ID and name: from Standard File, the Finder's
   application files (a working directory and a name) */
static void leave_radio(void)
{
	if (!showing_radio) return;
	radio_stop();
	showing_radio = 0;
	SetPort(win);
	InvalRect(PORT_RECT(win));
}
static void open_path(short vref, long dirid, const unsigned char *name)
{
	leave_radio();
	close_file();
	if (!shown) { ShowWindow(win); SelectWindow(win); shown = 1; }
	memcpy(fname, name, name[0] < 63 ? name[0] + 1 : 64);
	if (fname[0] > 63) fname[0] = 63;
	fvref = vref;
	memset(&fpb, 0, sizeof fpb);
	fpb.ioNamePtr = fname; fpb.ioVRefNum = fvref; fpb.ioPermssn = 1;     /* fsRdPerm */
	fpb.u.f.ioFlNum = dirid;                    /* ioDirID */
	short e = mac_PBHOpen(&fpb);
	if (e) {
		char t[100];
		put_str(put_num(put_str(t, "The file could not be opened (error -"), (unsigned long)-e), ").")[0] = 0;
		say_error(t);
		return;
	}
	file_open = 1;
	kind = K_NONE;
	err_text = 0;
	if (!identify()) {
		kind = K_NONE;
		mac_PBClose(&fpb);
		file_open = 0;
		show_file();
		say_error(err_text);
		return;
	}
	status_text = "";
	start();
	set_status(ps->out_kind ? "Playing" : "");
	show_file();
}

/* ---- the buttons, the slider, the keys ------------------------------------------------------- */
static void start_station(const char *name, const char *url);
static void play_pause(void)
{
	if (showing_radio) {
		if (radio.st == RS_OFF) start_station(radio.station, radio.url);
		else radio_pause(!radio.paused);
		fix_buttons();
		draw_progress();
		return;
	}
	if (!file_open) { open_file(); return; }
	if (state == S_PLAYING) { pi_command(PSMB_STR_C_PAUSE); state = S_PAUSED; pi_log("paused"); set_status("Paused"); }
	else if (state == S_PAUSED) { pi_command(PSMB_STR_C_RESUME); state = S_PLAYING; pi_log("resumed"); set_status("Playing"); }
	else { start(); set_status("Playing"); }
	fix_buttons();
}
static void stop(void)
{
	if (showing_radio) { radio_stop(); fix_buttons(); draw_progress(); return; }
	if (state != S_PLAYING && state != S_PAUSED) return;
	stop_stream();
	state = S_STOPPED;
	pi_log("stopped");
	set_status("Stopped");
	fix_buttons();
}
static void change_volume(int v)
{
	if (v < 0) v = 0;
	if (v > 100) v = 100;
	vol = v;
	SetCtlValue(vol_bar, vol);
	set_volume();
}
void c_ctl_action(void *ctl, int part)
{
	if (ctl != vol_bar) return;
	switch (part) {
	case inUpButton: change_volume(vol - 5); break;
	case inDownButton: change_volume(vol + 5); break;
	case inPageUp: change_volume(vol - 20); break;
	case inPageDown: change_volume(vol + 20); break;
	}
}
static void content_click(EventRecord *ev)
{
	Point p = ev->where;
	void *ctl = 0;
	SetPort(win);
	GlobalToLocal(&p);
	int part = FindControl(pt_long(p), win, &ctl);
	if (!ctl) return;
	if (ctl == vol_bar) {
		if (part == inThumb) { TrackControl(ctl, pt_long(p), 0); change_volume(GetCtlValue(vol_bar)); }
		else TrackControl(ctl, pt_long(p), (void *)pas_ctl_action);
		return;
	}
	if (!TrackControl(ctl, pt_long(p), 0)) return;
	if (ctl == play_btn) play_pause();
	else if (ctl == stop_btn) stop();
}

/* ---- the radio: a station chosen, an address asked for ------------------------------------------ */
static void start_station(const char *name, const char *url)
{
	char n[48], u[200];
	int i;
	for (i = 0; name[i] && i < 47; i++) n[i] = name[i];
	n[i] = 0;
	for (i = 0; url[i] && i < 199; i++) u[i] = url[i];
	u[i] = 0;
	close_file();
	title[0] = artist[0] = 0; kind = K_NONE; status_text = "";
	if (!shown) { ShowWindow(win); SelectWindow(win); shown = 1; }
	showing_radio = 1;
	SetPort(win);
	InvalRect(PORT_RECT(win));
	set_volume();
	if (!radio_play(n, u)) {
		const char *e = radio.error;
		radio.error = 0;
		fix_buttons();
		say_error(e ? e : "The station could not be played.");
	}
	fix_buttons();
}
/* the Radio menu from the station list: the names put in with SetItem, which takes them as they are
   (AppendMenu would read ";", "(" or "/" in a name as commands of its own) */
static void build_radio_menu(void)
{
	int i, n = CountMItems(m_radio);
	for (i = n; i > 0; i--) DelMenuItem(m_radio, i);
	for (i = 0; i < nstations; i++) {
		AppendMenu(m_radio, pstr(pbuf, "x"));
		SetItem(m_radio, i + 1, pstr(pbuf, stations[i].name));
	}
	if (nstations) AppendMenu(m_radio, pstr(pbuf, "(-"));
	AppendMenu(m_radio, pstr(pbuf, "Open URL\xC9/U"));
	radio_url_item = CountMItems(m_radio);
}
static void TEFromScrap_(void)                 /* TextEdit's scrap from the desk scrap (Inside Macintosh I, TextEdit) */
{
	long off = 0, n = GetScrap(LM_TESCRPH, TYPE('T', 'E', 'X', 'T'), &off);
	if (n >= 0) LM_TESCRPLEN = (short)n;
}
static void TEToScrap_(void)
{
	ZeroScrap();
	mac_HLock(LM_TESCRPH);
	PutScrap(LM_TESCRPLEN, TYPE('T', 'E', 'X', 'T'), *LM_TESCRPH);
	mac_HUnlock(LM_TESCRPH);
}
/* "Open URL...": a small window with a text field, Open (the default) and Cancel; 1 if out has an address */
static char last_url[200] = "http://";
static int ask_url(char *out, int max)
{
	Rect s = screen_rect();
	short w = 380, h = 136, left = (short)((s.right - s.left - w) / 2), top = (short)((s.bottom - s.top - h) / 3 + 10);
	Rect b = { top, left, (short)(top + h), (short)(left + w) };
	void *d = NewWindow(0, &b, pstr(pbuf, ""), 1, 1 /* dBoxProc */, (void *)-1, 0, 0);
	SetPort(d);
	TextFont(3); TextSize(9);                   /* the field in Geneva 9: long addresses fit */
	Rect okr = { (short)(h - 30), (short)(w - 82), (short)(h - 10), (short)(w - 16) };
	Rect car = { (short)(h - 30), (short)(w - 162), (short)(h - 10), (short)(w - 96) };
	Rect fr = { 34, 18, 72, (short)(w - 18) }, vr = fr;
	InsetRect(&vr, 4, 3);
	void *ok = NewControl(d, &okr, pstr(pbuf, "Open"), 1, 0, 0, 1, pushButProc, 0);
	NewControl(d, &car, pstr(pbuf, "Cancel"), 1, 0, 0, 1, pushButProc, 0);   /* any button but Open cancels */
	TERec **te = (TERec **)TENew(&vr, &vr);
	TESetText(last_url, (long)strlen(last_url), te);
	TESetSelect(0, 32767, te);
	TEActivate(te);
	void **ibeam = (void **)GetCursor(1);       /* iBeamCursor */
	EventRecord ev;
	int done = 0;
	while (!done) {
		if (!WaitNextEvent(everyEvent, &ev, 6, 0)) {
			Point p = ev.where;
			SetPort(d);
			GlobalToLocal(&p);
			if (ibeam && *ibeam && PtInRect(pt_long(p), &fr)) SetCursor(*ibeam);
			else InitCursor();
			TEIdle(te);
			if (file_open && state == S_PLAYING) feed();
			radio_poll();
			continue;
		}
		SetPort(d);
		if (ev.what == updateEvt && (void *)ev.message == d) {
			BeginUpdate(d);
			TextFont(0); TextSize(12);
			MoveTo(18, 24);
			DrawText("Play the MP3 stream at this address:", 0, 36);
			TextFont(3); TextSize(9);
			MoveTo(18, 86);
			DrawText("A plain http:// stream (Shoutcast, Icecast), or a .pls or .m3u playlist.", 0, 72);
			Rect f = fr;
			FrameRect(&f);
			TEUpdate(&fr, te);
			DrawControls(d);
			f = okr;                            /* the default button's bold outline */
			InsetRect(&f, -4, -4);
			PenSize(3, 3);
			FrameRoundRect(&f, 16, 16);
			PenNormal();
			EndUpdate(d);
		} else if (ev.what == updateEvt) {
			if ((void *)ev.message == win) { BeginUpdate(win); redraw_all(); EndUpdate(win); }
		} else if (ev.what == keyDown || ev.what == autoKey) {
			int c = (int)(ev.message & charCodeMask);
			if (ev.modifiers & cmdKey) {
				if (c == 'v' || c == 'V') { TEFromScrap_(); TEPaste(te); }
				else if (c == 'c' || c == 'C') { TECopy(te); TEToScrap_(); }
				else if (c == 'x' || c == 'X') { TECut(te); TEToScrap_(); }
				else if (c == 'a' || c == 'A') TESetSelect(0, 32767, te);
				else if (c == '.') done = 2;
				else SysBeep(1);
			} else if (c == 0x0D || c == 0x03) {
				unsigned long t0 = LM_TICKS;    /* the button flashes, as a dialog's does */
				HiliteControl(ok, 1);
				while (LM_TICKS - t0 < 8) { }
				HiliteControl(ok, 0);
				done = 1;
			} else if (c == 0x1B) done = 2;
			else TEKey(c, te);
		} else if (ev.what == mouseDown) {
			void *w = 0;
			int part = FindWindow(pt_long(ev.where), &w);
			if (part == inContent && w == d) {
				Point p = ev.where;
				void *c = 0;
				GlobalToLocal(&p);
				if (PtInRect(pt_long(p), &fr)) TEClick(pt_long(p), (ev.modifiers & shiftKey) != 0, te);
				else if (FindControl(pt_long(p), d, &c) && TrackControl(c, pt_long(p), 0)) done = c == ok ? 1 : 2;
			} else SysBeep(1);
		}
	}
	int n = 0;
	if (done == 1) {
		const char *t = *(*te)->hText;
		int k, len = (*te)->teLength;
		for (k = 0; k < len && n < max - 1; k++)
			if (t[k] > ' ') out[n++] = t[k];     /* no spaces or line ends: a pasted address may have some */
	}
	out[n] = 0;
	TEDispose(te);
	DisposeWindow(d);
	InitCursor();
	SetPort(win);
	return n > 0;
}
static void open_url(void)
{
	char u[200], name[48];
	int i = 0;
	if (!ask_url(u, sizeof u)) return;
	memcpy(last_url, u, strlen(u) + 1);
	const char *h = u;                          /* the window's name for it: its host */
	if ((h[0] | 32) == 'h' && (h[1] | 32) == 't' && (h[2] | 32) == 't' && (h[3] | 32) == 'p' && h[4] == ':' && h[5] == '/' && h[6] == '/') h += 7;
	while (h[i] && h[i] != '/' && h[i] != ':' && i < 47) { name[i] = h[i]; i++; }
	name[i] = 0;
	pi_log("radio: Open URL");
	start_station(name, u);
}

/* ---- menus ----------------------------------------------------------------------------------------- */
static void setup_menus(void)
{
	unsigned char t[2] = { 1, 0x14 };       /* the apple */
	m_apple = NewMenu(M_APPLE, t);
	AppendMenu(m_apple, pstr(pbuf, "About Aurora68 Play\xC9;(-"));
	AddResMenu(m_apple, TYPE('D', 'R', 'V', 'R'));
	InsertMenu(m_apple, 0);
	m_file = NewMenu(M_FILE, pstr(pbuf, "File"));
	AppendMenu(m_file, pstr(pbuf, "Open\xC9/O;Close/W;(-;Quit/Q"));
	InsertMenu(m_file, 0);
	m_edit = NewMenu(M_EDIT, pstr(pbuf, "Edit"));         /* for desk accessories */
	AppendMenu(m_edit, pstr(pbuf, "Undo/Z;(-;Cut/X;Copy/C;Paste/V;Clear"));
	InsertMenu(m_edit, 0);
	m_ctl = NewMenu(M_CTL, pstr(pbuf, "Controls"));
	AppendMenu(m_ctl, pstr(pbuf, "Play/P;Stop/.;(-;Volume Up/];Volume Down/["));
	InsertMenu(m_ctl, 0);
	m_radio = NewMenu(M_RADIO, pstr(pbuf, "Radio"));     /* its items come from the station list */
	InsertMenu(m_radio, 0);
	DrawMenuBar();
}
static void do_menu(long mr)
{
	int id = (int)(mr >> 16), item = (int)(mr & 0xFFFF);
	SetPort(win);
	if (id == M_APPLE) {
		if (item == 1) about();
		else { GetItem(m_apple, item, pbuf); OpenDeskAcc(pbuf); SetPort(win); }
	} else if (id == M_FILE) {
		if (item == F_OPEN) open_file();
		else if (item == F_CLOSE) close_window();
		else if (item == F_QUIT) quit = 1;
	} else if (id == M_EDIT) {
		SystemEdit(item - 1);
	} else if (id == M_CTL) {
		if (item == C_PLAY) play_pause();
		else if (item == C_STOP) stop();
		else if (item == C_LOUDER) change_volume(vol + 10);
		else if (item == C_SOFTER) change_volume(vol - 10);
	} else if (id == M_RADIO) {
		if (item == radio_url_item) open_url();
		else if (item >= 1 && item <= nstations) start_station(stations[item - 1].name, stations[item - 1].url);
	}
	HiliteMenu(0);
}

/* ---- the window's size, closing it, and the preferences file -------------------------------------- */
static void resized(void)
{
	SetPort(win);
	layout();
	InvalRect(PORT_RECT(win));
}
static void close_window(void)
{
	leave_radio();
	close_file();
	title[0] = artist[0] = 0; kind = K_NONE; status_text = "";
	HideWindow(win);
	shown = 0;
	pi_log("window closed (File > Open... brings it back)");
}
/* "PiPlay Prefs" in the Preferences folder (System 7) or the System Folder (System 6): the window's
   place and size and the volume, a few bytes in the data fork */
#define PREF_MAGIC TYPE('P', 'I', 'P', 'Y')
struct prefs { unsigned long magic; short version; Rect r; short vol; short pad; };
static const unsigned char pref_name[] = "\x13" "Aurora68 Play Prefs";
static const unsigned char pref_old_name[] = "\x0C" "PiPlay Prefs";    /* the program's name until 21 November 2026 */
int pref_place(short *vref, long *dirid)
{
	*vref = *(volatile short *)0x210;          /* BootDrive: the System Folder's working directory */
	*dirid = 0;
	if (LM_SYSVERSION >= 0x0700 && mac_FindFolder(-32768, TYPE('p', 'r', 'e', 'f'), 1, vref, dirid)) {
		*vref = *(volatile short *)0x210; *dirid = 0;
	}
	return 1;
}
static void pref_pb_named(ParamBlock *pb, short vref, long dirid, const unsigned char *name)
{
	memset(pb, 0, sizeof *pb);
	pb->ioNamePtr = name; pb->ioVRefNum = vref;
	pb->u.f.ioFlNum = dirid;                    /* ioDirID */
}
static void pref_pb(ParamBlock *pb, short vref, long dirid)
{
	memset(pb, 0, sizeof *pb);
	pb->ioNamePtr = pref_name; pb->ioVRefNum = vref;
	pb->u.f.ioFlNum = dirid;                    /* ioDirID */
}
static int load_prefs(struct prefs *p)
{
	ParamBlock pb;
	short vref; long dirid;
	pref_place(&vref, &dirid);
	pref_pb(&pb, vref, dirid);
	pb.ioPermssn = 1;
	if (mac_PBHOpen(&pb)) {
		pref_pb_named(&pb, vref, dirid, pref_old_name);      /* left by PiPlay: read it, write the new one */
		pb.ioPermssn = 1;
		if (mac_PBHOpen(&pb)) return 0;
		pi_log("the preferences of PiPlay, its old name, read");
	}
	pb.u.io.ioBuffer = p; pb.u.io.ioReqCount = sizeof *p; pb.u.io.ioPosMode = fsFromStart; pb.u.io.ioPosOffset = 0;
	short e = mac_PBRead(&pb);
	mac_PBClose(&pb);
	return !e && pb.u.io.ioActCount == sizeof *p && p->magic == PREF_MAGIC && p->version == 1;
}
static void save_prefs(void)
{
	ParamBlock pb;
	struct prefs p;
	short vref; long dirid;
	Point tl = { 0, 0 };
	SetPort(win);
	LocalToGlobal(&tl);
	p.magic = PREF_MAGIC; p.version = 1; p.vol = (short)vol; p.pad = 0;
	p.r.top = tl.v; p.r.left = tl.h; p.r.bottom = (short)(tl.v + H); p.r.right = (short)(tl.h + W);
	pref_place(&vref, &dirid);
	pref_pb(&pb, vref, dirid);
	short e = mac_PBHCreate(&pb);
	if (!e || e == dupFNErr) {
		pref_pb(&pb, vref, dirid);
		if (!mac_PBHGetFInfo(&pb)) {
			pb.u.f.fdType = TYPE('p', 'r', 'e', 'f'); pb.u.f.fdCreator = TYPE('P', 'I', 'P', 'Y');
			pb.u.f.ioFlNum = dirid;
			mac_PBHSetFInfo(&pb);
		}
		pref_pb(&pb, vref, dirid);
		pb.ioPermssn = fsWrPerm;
		e = mac_PBHOpen(&pb);
	}
	if (e) { pi_log("the preferences file could not be written"); return; }
	pb.u.io.ioBuffer = &p; pb.u.io.ioReqCount = sizeof p; pb.u.io.ioPosMode = fsFromStart; pb.u.io.ioPosOffset = 0;
	mac_PBWrite(&pb);
	pb.u.io.ioMisc = sizeof p;
	mac_PBSetEOF(&pb);
	mac_PBClose(&pb);
	memset(&pb, 0, sizeof pb);
	pb.ioVRefNum = vref;
	mac_PBFlushVol(&pb);
}
/* a remembered place is used only if the window's title bar is on a screen that is still there */
static int on_screen(const Rect *r)
{
	Point a = { (short)(r->top - 10), (short)(r->left + 24) }, b = { (short)(r->top - 10), (short)(r->right - 24) };
	return r->right - r->left >= WIN_W && r->bottom - r->top >= WIN_H &&
	       PtInRgn(pt_long(a), LM_GRAYRGN) && PtInRgn(pt_long(b), LM_GRAYRGN);
}

/* ---- files opened from the Finder ---------------------------------------------------------------
   A file dropped on PiPlay's icon, or opened from the Finder with PiPlay (its bundle says it takes any
   kind of file), arrives in the application parameters (AppParmHandle, $AEC), read once at the start:
   System 6's way, and System 7's for a program that does not say it is high-level-event aware. A first
   version took System 7's Apple event 'odoc' instead (SIZE $58C0, handlers for 'oapp', 'odoc', 'pdoc'
   and 'quit'); in the testbed it stopped with "bad F-Line instruction" as the Finder launched it with a
   document, before PiPlay logged anything after its start, and was taken out rather than chased
   (docs/08 item 82). */
static struct { short vref; long dirid; unsigned char name[64]; } pending;
static int pending_open;
static void finder_files(void)
{
	unsigned char **h = *(unsigned char ***)0xAEC;          /* message, count, then {vRefNum, fType, versNum, fName} */
	if (!h || !*h) return;
	unsigned char *p = *h;
	short count = (short)(p[2] << 8 | p[3]);
	if (count < 1 || p[1] != 0) return;                     /* message 0: open (1 would be print) */
	p += 4;
	pending.vref = (short)(p[0] << 8 | p[1]); pending.dirid = 0;
	memcpy(pending.name, p + 8, p[8] < 63 ? p[8] + 1 : 64);
	pending_open = 1;
}

/* ---- events ---------------------------------------------------------------------------------------- */
static void mouse_down(EventRecord *ev)
{
	void *w = 0;
	int part = FindWindow(pt_long(ev->where), &w);
	switch (part) {
	case inMenuBar:
		if (stations_check()) build_radio_menu();   /* "PiPlay Stations" changed (TeachText) */
		fix_menus();
		do_menu(MenuSelect(pt_long(ev->where)));
		break;
	case inSysWindow: SystemClick(ev, w); break;
	case inDrag: DragWindow(w, pt_long(ev->where), RGN_BBOX(LM_GRAYRGN)); break;
	case inGoAway: if (w == win && TrackGoAway(w, pt_long(ev->where))) close_window(); break;
	case inGrow:
		if (w == win) {
			Rect lim = { WIN_H, WIN_W, 4000, 4000 };     /* top, left: the smallest height and width */
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
		else if (w == win) content_click(ev);
		break;
	}
}
static void key_down(EventRecord *ev)
{
	int c = (int)(ev->message & charCodeMask);
	if (ev->modifiers & cmdKey) {
		fix_menus();
		long mr = MenuKey(c);
		if (mr >> 16) do_menu(mr);
		else SysBeep(1);                       /* a key PiPlay has no command for */
		return;
	}
	if (c == ' ' && FrontWindow() == win) play_pause();
	else if (c == 0x1B) stop();                 /* Esc */
	else if (c == 0x1E) change_volume(vol + 5); /* up arrow */
	else if (c == 0x1F) change_volume(vol - 5); /* down arrow */
	else if (c == 0x0D || c == 0x03) { if (!file_open && !showing_radio) open_file(); else if (!playing_now()) play_pause(); }
	else SysBeep(1);
}
static void activate(int on)
{
	SetPort(win);
	if (on) { fix_buttons(); HiliteControl(vol_bar, 0); }
	else { HiliteControl(play_btn, 255); HiliteControl(stop_btn, 255); HiliteControl(vol_bar, 255); }
}
/* the radio's part of the window: the texts when something in them changed, the meter and the state
   twice a second; a failure said in a window of its own */
static void watch_radio(void)
{
	static unsigned long seen = 0xFFFFFFFF, last_tick;
	if (radio.error) {
		const char *e = radio.error;
		radio.error = 0;
		fix_buttons();
		draw_texts(); draw_progress();
		say_error(e);
		seen = 0xFFFFFFFF;
	}
	if (radio.st != RS_OFF && ps->state == PSMB_STR_S_ERROR) {
		radio_stop();
		say_error(ps->error == PSMB_STR_E_NOMEM ? "The Pi has no memory left for the sound." :
		          "The station sends nothing the card can play: no MP3 sound was found in its stream.");
	}
	if (radio.seq != seen) { seen = radio.seq; draw_texts(); fix_buttons(); draw_progress(); last_tick = LM_TICKS; }
	else if (LM_TICKS - last_tick >= 30) { last_tick = LM_TICKS; draw_progress(); }
}
/* what the Pi says, in the window */
static void watch(void)
{
	static unsigned long last_tick, last_state = 99, last_rate;
	if (!ps) return;
	if (showing_radio) { if (shown) watch_radio(); return; }
	unsigned long pst = ps->state;
	if (state == S_PLAYING && pst == PSMB_STR_S_ENDED) {
		state = S_ENDED;
		char t[80], *p = put_str(t, "finished, ");
		p = put_time(p, ps->pos_ms); p = put_str(p, " played, ");
		p = put_num(p, ps->underruns); p = put_str(p, " underruns, decoder ");
		p = put_num(p, ps->load); p = put_str(p, " per mille of core 1");
		*p = 0;
		pi_log(t);
		status_text = "Finished";
		fix_buttons();
	}
	if (pst == PSMB_STR_S_ERROR && last_state != PSMB_STR_S_ERROR && (state == S_PLAYING || state == S_PAUSED)) {
		state = S_STOPPED;
		fix_buttons();
		say_error(ps->error == PSMB_STR_E_NOAUDIO ? "There is no MP3 sound in this file, or it is damaged." :
		          ps->error == PSMB_STR_E_NOMEM ? "The Pi has no memory left for the sound." :
		                                          "The sound card cannot play this kind of sound.");
		status_text = "";
	}
	last_state = pst;
	if (ps->rate != last_rate) { last_rate = ps->rate; draw_texts(); }       /* the format is known now */
	if (LM_TICKS - last_tick >= 15) {           /* the progress line, when what it says has changed */
		static unsigned long shown = 0xFFFFFFFF;
		unsigned long key = (ps->pos_ms / 1000) ^ ps->length_ms << 12 ^ (unsigned long)state << 28 ^ (unsigned long)status_text;
		last_tick = LM_TICKS;
		if (key != shown) { shown = key; draw_progress(); }
	}
}

int main(void)
{
	setup_menus();
	win_base = LM_MMU32BIT ? (volatile unsigned char *)0xF9000000 : (volatile unsigned char *)0x00900000;
	ps = (volatile struct psmb_stream *)(win_base + PSMB_OFF_STREAM);
	snd = (volatile struct psmb_sound *)(win_base + PSMB_OFF_SOUND);
	if (snd->magic != PSMB_SND_MAGIC) win_base = 0;
	ring = win_base ? win_base + PSMB_OFF_STRRING : 0;
	iobuf = mac_newptr(IOBUF);

	Rect s = screen_rect();
	short left = (short)((s.right - s.left - WIN_W) / 2), top = 50;
	Rect b = { top, left, (short)(top + WIN_H), (short)(left + WIN_W) };
	struct prefs pr;
	if (load_prefs(&pr)) {
		if (pr.vol >= 0 && pr.vol <= 100) vol = pr.vol;
		if (on_screen(&pr.r)) b = pr.r;
		else pi_log("the remembered window place is on no screen now: the window goes on the main screen");
	}
	win = NewWindow(0, &b, pstr(pbuf, "Aurora68 Play"), 0, zoomDocProc, (void *)-1, 1, 0);
	{   /* the zoom box's standard state: as wide as the main screen allows, a little taller */
		Rect *std = (Rect *)((char *)**(void ***)((char *)win + 130) + 8);
		std->top = 40; std->left = 8; std->bottom = 40 + 140;
		std->right = (short)(s.right - s.left > 680 ? 8 + 660 : s.right - 8);
	}
	ShowWindow(win);
	shown = 1;
	SetPort(win);
	Rect r1 = { 76, 10, 96, 80 }, r2 = { 76, 90, 96, 160 }, r3 = { 78, 230, 94, 316 };
	play_btn = NewControl(win, &r1, pstr(pbuf, "Play"), 1, 0, 0, 1, pushButProc, 0);
	stop_btn = NewControl(win, &r2, pstr(pbuf, "Stop"), 1, 0, 0, 1, pushButProc, 0);
	vol_bar = NewControl(win, &r3, pstr(pbuf, ""), 1, vol, 0, 100, scrollBarProc, 0);
	layout();
	fix_buttons();

	/* is there a card that plays, and does this kernel have the stream channel? */
	const char *problem = 0;
	if (!win_base) problem = "There is no Aurora68 sound card in this Macintosh, so Aurora68 Play has nothing to play on.";
	else if (ps->magic != PSMB_STR_MAGIC) { problem = "This Aurora68 kernel is too old for Aurora68 Play: put a newer kernel8.img on the Pi's SD card."; ps = 0; }
	else if (!ps->out_kind) problem = "The Pi plays no sound now: mac.audio=off is in cmdline.txt, or neither HDMI nor the headphone socket could be started (the Pi's log says why).";
	else if (!iobuf) problem = "There is not enough memory for Aurora68 Play.";
	if (ps) {
		char t[90], *p = put_str(t, "started; the card plays on ");
		p = put_str(p, ps->out_kind == PSMB_SND_OUT_HDMI ? "HDMI" : ps->out_kind == PSMB_SND_OUT_JACK ? "the headphone socket" :
		               ps->out_kind == PSMB_SND_OUT_FILE ? "a file (the testbed)" : "nothing");
		p = put_str(p, " at "); p = put_num(p, ps->out_rate); p = put_str(p, " Hz; Sound control panel level ");
		p = put_num(p, *(volatile unsigned char *)0x260 & 7);
		*p = 0;
		pi_log(t);
		stop_stream();                          /* whatever an earlier PiPlay left */
	}
	if (problem) {
		pi_log(problem);
		message_box(problem, 64);
		if (!ps || !ps->out_kind || !iobuf) return 0;
	}

	radio_init();
	stations_load();
	build_radio_menu();
	finder_files();
	EventRecord ev;
	while (!quit) {
		int more = file_open && state == S_PLAYING && sent < data_len && PSMB_STR_SIZE - (ps->head - ps->tail) >= 4096;
		long sleep = more ? 0 : state == S_PLAYING || radio.st != RS_OFF ? 6 : 30;
		if (WaitNextEvent(everyEvent, &ev, sleep, 0)) {
			switch (ev.what) {
			case mouseDown: mouse_down(&ev); break;
			case keyDown: case autoKey: key_down(&ev); break;
			case updateEvt:
				if ((void *)ev.message == win) { BeginUpdate(win); redraw_all(); EndUpdate(win); }
				break;
			case activateEvt: if ((void *)ev.message == win) activate(ev.modifiers & activeFlag); break;
			case osEvt:
				if (((unsigned long)ev.message >> 24) == suspendResumeMessage) {
					fg = ev.message & resumeFlag;
					activate(fg);
					if (fg && stations_check()) build_radio_menu();   /* back from TeachText, perhaps */
				}
				break;
			}
		}
		if (pending_open) {
			pending_open = 0;
			pi_log("a file from the Finder");
			open_path(pending.vref, pending.dirid, pending.name);
		}
		if (file_open && (state == S_PLAYING || state == S_PAUSED)) {
			/* as much as the ring takes, for up to two ticks a pass: the Pi empties the ring into its own
			   buffer as fast as it is filled, and one piece a pass fed CD-quality PCM (176 KB/s) too slowly
			   in the testbed, where each pass through WaitNextEvent gives the Finder its turn */
			unsigned long t0 = LM_TICKS;
			while (feed() == 2 && LM_TICKS - t0 < 2) { }
		}
		radio_poll();
		watch();
	}
	radio_quit();
	close_file();
	save_prefs();
	pi_log("quit");
	return 0;
}
