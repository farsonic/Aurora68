/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/media.c - FLAC, Ogg Vorbis and Apple Lossless (.m4a) files for Aurora68 Play: what a file
 * is, its title and artist, and the bytes the sound card's decoders want, from the start or from a seek.
 *
 * Backstory (22 November 2026, docs/08 item 103). The owner's music is mostly FLAC and .m4a; Aurora68 Play
 * played MP3, AIFF and WAV. The card decodes (emu68/src/psmac/psmac_codec.h says what it wants of each
 * format); the Mac's part is to know the file, send the card what it needs and nothing more, and seek:
 *
 *  - FLAC: "fLaC" and its metadata blocks, then the frames. The card is sent STREAMINFO alone and then the
 *    frames: a PICTURE block may hold megabytes of cover. The comments (VORBIS_COMMENT) give the title and
 *    artist; a SEEKTABLE, when the encoder wrote one (ffmpeg and flac do), gives exact places to seek to,
 *    and without one a seek is a guess in proportion that the card corrects from the frame it finds.
 *  - Ogg Vorbis: pages ("OggS"), whose first three packets are the identification, comment and setup
 *    headers; the setup header ends its page (the Vorbis I specification requires it), so the headers are
 *    the file's first bytes up to the first audio page. The length is the last page's granule position.
 *    A seek is a guess in proportion, moved on to the next page; the card learns the real place from the
 *    pages' granule positions. Opus and FLAC in Ogg are recognized and refused in words.
 *  - .m4a: an MP4 file whose 'moov' box holds the sample tables, at its start or its end (iTunes put it
 *    at the end for years), so the Mac, which can read anywhere in the file, takes the packets out and
 *    sends them one by one with their lengths, after the decoder's configuration (the 'alac' box of the
 *    sample description). The title and artist are in moov/udta/meta/ilst ("\xA9nam", "\xA9ART"). AAC in
 *    .m4a is recognized and refused: there is no AAC decoder on the card whose licence and size suit it
 *    (docs/09 says more), and the music server converts such files on the way.
 *
 * The file is read through mf_io (the File Manager in Play, stdio in tools/codec_test.c); no allocation;
 * buffers are static, as Play has one file open at a time.
 */
#include "media.h"
#include "icy.h"

static unsigned char tb[4096];                    /* scratch for headers and comments */

static unsigned long be32(const unsigned char *b) { return (unsigned long)b[0] << 24 | (unsigned long)b[1] << 16 | (unsigned long)b[2] << 8 | b[3]; }
static unsigned long le32(const unsigned char *b) { return (unsigned long)b[3] << 24 | (unsigned long)b[2] << 16 | (unsigned long)b[1] << 8 | b[0]; }
static long rd(const struct mf_io *io, unsigned long off, void *buf, long n)
{
	if (off >= io->size) return 0;
	if ((unsigned long)n > io->size - off) n = (long)(io->size - off);
	return io->read(io->u, off, buf, n);
}
static int same4(const unsigned char *b, const char *t) { return b[0] == (unsigned char)t[0] && b[1] == (unsigned char)t[1] && b[2] == (unsigned char)t[2] && b[3] == (unsigned char)t[3]; }
static void cpy(void *d, const void *s, unsigned long n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; }
static unsigned long mul_div(unsigned long a, unsigned long b, unsigned long c) { return c ? (unsigned long)((unsigned long long)a * b / c) : 0; }

/* UTF-8 into Mac Roman (icy.c's table), at most max - 1 characters */
static void utf8_mac(const unsigned char *s, unsigned long n, char *out, int max)
{
	unsigned long i = 0;
	int o = 0;
	while (i < n && o < max - 1) {
		unsigned long u = s[i++];
		if (u >= 0xC0) {
			int extra = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : 1;
			u &= 0x3F >> extra;
			for (; extra && i < n; extra--, i++) u = u << 6 | (s[i] & 0x3F);
		}
		if (!u) break;
		out[o++] = (char)icy_mac_char(u);
	}
	while (o && out[o - 1] == ' ') o--;
	out[o] = 0;
}
static int key_is(const unsigned char *b, unsigned long n, const char *key)
{
	unsigned long i;
	for (i = 0; key[i]; i++) {
		int c = i < n ? b[i] : 0;
		if (c >= 'a' && c <= 'z') c -= 32;
		if (c != key[i]) return 0;
	}
	return i < n && b[i] == '=';
}
void mf_vorbis_comments(struct media *m, const unsigned char *b, unsigned long n)
{
	unsigned long p, count, k;
	if (n < 8) return;
	p = 4 + le32(b);
	if (p + 4 > n || p < 4) return;
	count = le32(b + p);
	p += 4;
	for (k = 0; k < count && p + 4 <= n; k++) {
		unsigned long len = le32(b + p), avail;
		const unsigned char *c = b + p + 4;
		p += 4;
		avail = len < n - p ? len : n - p;           /* a comment cut off by the scratch buffer: what is there */
		if (key_is(c, avail, "TITLE") && !m->title[0]) utf8_mac(c + 6, avail - 6, m->title, sizeof m->title);
		else if (key_is(c, avail, "ARTIST") && !m->artist[0]) utf8_mac(c + 7, avail - 7, m->artist, sizeof m->artist);
		else if (key_is(c, avail, "ALBUM") && !m->album[0]) utf8_mac(c + 6, avail - 6, m->album, sizeof m->album);
		if (len > n - p) break;
		p += len;
	}
}

/* ---- FLAC ------------------------------------------------------------------------------------------ */
static int flac(struct media *m, const struct mf_io *io, unsigned long p)
{
	unsigned char h[4];
	int have_info = 0;
	for (;;) {
		unsigned long len;
		if (rd(io, p, h, 4) != 4) break;
		len = (unsigned long)h[1] << 16 | (unsigned long)h[2] << 8 | h[3];
		if ((h[0] & 0x7F) == 0 && len >= 34) {            /* STREAMINFO */
			unsigned char *s = m->pre + 8;
			if (rd(io, p + 4, s, 34) != 34) break;
			m->pre[0] = 'f'; m->pre[1] = 'L'; m->pre[2] = 'a'; m->pre[3] = 'C';
			m->pre[4] = 0x80; m->pre[5] = 0; m->pre[6] = 0; m->pre[7] = 34;   /* the last block, 34 bytes */
			m->pre_n = 42;
			m->rate = (unsigned long)s[10] << 12 | (unsigned long)s[11] << 4 | s[12] >> 4;
			m->channels = ((s[12] >> 1) & 7) + 1;
			m->bits = ((unsigned long)(s[12] & 1) << 4 | s[13] >> 4) + 1;
			m->frames = (s[13] & 15) ? 0xFFFFFFFFul : be32(s + 14);
			have_info = 1;
		} else if ((h[0] & 0x7F) == 3) {                  /* SEEKTABLE */
			m->seek_off = p + 4; m->seek_n = len / 18;
		} else if ((h[0] & 0x7F) == 4) {                  /* VORBIS_COMMENT */
			long n = rd(io, p + 4, tb, len < sizeof tb ? (long)len : (long)sizeof tb);
			if (n > 0) mf_vorbis_comments(m, tb, (unsigned long)n);
		}
		p += 4 + len;
		if (h[0] & 0x80) { m->audio_off = p; break; }
		if (p >= io->size) break;
	}
	if (!have_info || !m->audio_off || !m->rate) { m->kind = MF_DAMAGED; return 0; }
	m->audio_len = io->size - m->audio_off;
	if (m->frames) m->length_ms = mul_div(m->frames, 1000, m->rate);
	m->kind = MF_FLAC;
	return 1;
}

/* ---- Ogg ------------------------------------------------------------------------------------------- */
/* the page at off: its length (0 if none there), granule (low 32 bits; 0xFFFFFFFF for none) and segments */
static unsigned long ogg_page(const struct mf_io *io, unsigned long off, unsigned long *granule, unsigned char *seg, int *nseg)
{
	unsigned char h[27];
	unsigned long len = 0;
	int i;
	if (rd(io, off, h, 27) != 27 || !same4(h, "OggS") || h[4] != 0) return 0;
	*granule = (h[10] | h[11] | h[12] | h[13]) ? 0xFFFFFFFFul : le32(h + 6);
	*nseg = h[26];
	if (rd(io, off + 27, seg, *nseg) != *nseg) return 0;
	for (i = 0; i < *nseg; i++) len += seg[i];
	return 27 + (unsigned long)*nseg + len;
}
static int ogg(struct media *m, const struct mf_io *io)
{
	static unsigned char seg[255];
	unsigned long off = 0, gran, pkt_len = 0;
	int packet = 0, pages = 0, nseg, i;
	unsigned char id[30];
	int id_n = 0;
	unsigned long com_n = 0;
	while (packet < 3 && pages++ < 400) {
		unsigned long plen = ogg_page(io, off, &gran, seg, &nseg), d = off + 27 + (unsigned long)nseg;
		if (!plen) { m->kind = MF_DAMAGED; return 0; }
		for (i = 0; i < nseg; i++) {
			unsigned long sl = seg[i];
			if (packet == 0 && id_n < 30) {           /* the identification header's first 30 bytes */
				long want = (long)(sl < (unsigned long)(30 - id_n) ? sl : (unsigned long)(30 - id_n));
				if (rd(io, d, id + id_n, want) != want) { m->kind = MF_DAMAGED; return 0; }
				id_n += (int)want;
			} else if (packet == 1 && com_n < sizeof tb) {
				long want = (long)(sl < sizeof tb - com_n ? sl : sizeof tb - com_n);
				if (rd(io, d, tb + com_n, want) != want) { m->kind = MF_DAMAGED; return 0; }
				com_n += (unsigned long)want;
			}
			d += sl; pkt_len += sl;
			if (sl < 255) {                           /* a packet ends here */
				if (packet == 0) {
					if (id_n >= 8 && id[0] == 'O' && id[1] == 'p' && id[2] == 'u' && id[3] == 's') { m->kind = MF_OPUS; return 0; }
					if (id_n >= 5 && id[0] == 0x7F && id[1] == 'F' && id[2] == 'L' && id[3] == 'A' && id[4] == 'C') { m->kind = MF_OGGFLAC; return 0; }
					if (id_n < 30 || id[0] != 1 || !same4(id + 1, "vorb") || id[5] != 'i' || id[6] != 's') { m->kind = MF_DAMAGED; return 0; }
					m->channels = id[11]; m->rate = le32(id + 12); m->bits = 16;
				}
				packet++; pkt_len = 0;
				if (packet == 3) break;
			}
		}
		off += plen;
	}
	if (packet < 3 || !m->rate) { m->kind = MF_DAMAGED; return 0; }
	if (com_n > 7 && tb[0] == 3) mf_vorbis_comments(m, tb + 7, com_n - 7);
	m->hdr_off = 0; m->hdr_len = off;
	m->audio_off = off; m->audio_len = io->size - off;
	{   /* the length: the last page's granule position, from the file's last 64 KB */
		unsigned long end = io->size, lo = end > 65536 ? end - 65536 : 0, s;
		if (lo < off) lo = off;
		for (s = end; s > lo && !m->frames; ) {        /* backwards in pieces that overlap by a page header */
			unsigned long b0 = s > lo + 4000 ? s - 4000 : lo, b1 = s + 27 < end ? s + 27 : end;
			long n = rd(io, b0, tb, (long)(b1 - b0)), k;
			for (k = n - 27; k >= 0; k--)
				if (tb[k] == 'O' && tb[k + 1] == 'g' && tb[k + 2] == 'g' && tb[k + 3] == 'S' && tb[k + 4] == 0 && !(tb[k + 10] | tb[k + 11] | tb[k + 12] | tb[k + 13])) {
					unsigned long g = le32(tb + k + 6);
					if (g && g != 0xFFFFFFFFul) { m->frames = g; break; }
				}
			s = b0;
		}
	}
	if (m->frames) m->length_ms = mul_div(m->frames, 1000, m->rate);
	m->kind = MF_OGG;
	return 1;
}

/* ---- MP4 (.m4a) --------------------------------------------------------------------------------------- */
/* the first box of type t among the boxes in [from, to): its contents' offset and length; 0 if none */
static int box(const struct mf_io *io, unsigned long from, unsigned long to, const char *t, unsigned long *at, unsigned long *len)
{
	unsigned char h[16];
	while (from + 8 <= to) {
		unsigned long size, hl = 8;
		if (rd(io, from, h, 16) < 8) return 0;
		size = be32(h);
		if (size == 1) { if (be32(h + 8)) size = to - from; else size = be32(h + 12); hl = 16; }
		else if (size == 0) size = to - from;
		if (size < hl || from + size > to) size = to - from;     /* damaged: the rest */
		if (same4(h + 4, t)) { *at = from + hl; *len = size - hl; return 1; }
		from += size;
	}
	return 0;
}
static int path(const struct mf_io *io, unsigned long from, unsigned long to, const char *p, unsigned long *at, unsigned long *len)
{
	/* p: four-character types one after another, "moovudtameta" */
	while (*p) {
		if (!box(io, from, to, p, at, len)) return 0;
		from = *at; to = *at + *len;
		p += 4;
	}
	return 1;
}
static void ilst_text(const struct mf_io *io, unsigned long ilst, unsigned long n, const char *t, char *out, int max)
{
	unsigned long at, len;
	unsigned char h[8];
	if (!box(io, ilst, ilst + n, t, &at, &len)) return;
	if (!box(io, at, at + len, "data", &at, &len) || len < 8) return;
	if (rd(io, at, h, 8) != 8 || be32(h) != 1) return;           /* type 1: UTF-8 */
	len -= 8;
	if (len > 255) len = 255;
	if (rd(io, at + 8, tb, (long)len) != (long)len) return;
	utf8_mac(tb, len, out, max);
}
static int m4a(struct media *m, const struct mf_io *io)
{
	unsigned long moov, moov_n, trak, trak_n, at, len, from;
	unsigned char h[40];
	if (!box(io, 0, io->size, "moov", &moov, &moov_n)) { m->kind = MF_DAMAGED; return 0; }
	/* the first track whose handler is 'soun' */
	for (from = moov; ; from = trak + trak_n) {
		if (!box(io, from, moov + moov_n, "trak", &trak, &trak_n)) { m->kind = MF_M4A_OTHER; return 0; }
		if (path(io, trak, trak + trak_n, "mdiahdlr", &at, &len) && len >= 12 && rd(io, at, h, 12) == 12 && same4(h + 8, "soun")) break;
	}
	if (!path(io, trak, trak + trak_n, "mdiaminfstblstsd", &at, &len) || len < 16 || rd(io, at, h, 16) != 16) { m->kind = MF_DAMAGED; return 0; }
	if (same4(h + 12, "mp4a")) { m->kind = MF_AAC; return 0; }
	if (!same4(h + 12, "alac")) { m->kind = MF_M4A_OTHER; return 0; }
	{   /* the sample entry 'alac' (8 + 28 bytes), then its box 'alac': version and flags, then the 24-byte configuration */
		unsigned long e = at + 8, elen = be32(h + 8), cat, clen;
		if (!box(io, e + 8 + 28, e + elen, "alac", &cat, &clen) || clen < 28 || rd(io, cat + 4, m->pre + 8, 24) != 24) { m->kind = MF_DAMAGED; return 0; }
		m->pre[0] = 'a'; m->pre[1] = 'l'; m->pre[2] = 'a'; m->pre[3] = 'c';
		m->pre[4] = 0; m->pre[5] = 0; m->pre[6] = 0; m->pre[7] = 24;
		m->pre_n = 32;
		m->frame_len = be32(m->pre + 8);
		m->bits = m->pre[13]; m->channels = m->pre[17]; m->rate = be32(m->pre + 28);
	}
	{
		unsigned long stbl, stbl_n;
		if (!path(io, trak, trak + trak_n, "mdiaminfstbl", &stbl, &stbl_n)) { m->kind = MF_DAMAGED; return 0; }
		if (box(io, stbl, stbl + stbl_n, "stts", &at, &len) && len >= 16 && rd(io, at, h, 16) == 16) {
			unsigned long n = be32(h + 4), k, frames = 0;
			m->frame_len = be32(h + 12) ? be32(h + 12) : m->frame_len;
			for (k = 0; k < n && k < 4096; k++) {
				unsigned char e[8];
				if (rd(io, at + 8 + 8 * k, e, 8) != 8) break;
				frames += be32(e) * be32(e + 4);
			}
			m->frames = frames;
		}
		if (!box(io, stbl, stbl + stbl_n, "stsz", &at, &len) || len < 12 || rd(io, at, h, 12) != 12) { m->kind = MF_DAMAGED; return 0; }
		m->fixed_size = be32(h + 4); m->npackets = be32(h + 8);
		m->stsz.off = at + 12; m->stsz.n = m->fixed_size ? 0 : m->npackets;
		if (!box(io, stbl, stbl + stbl_n, "stsc", &at, &len) || len < 8 || rd(io, at, h, 8) != 8) { m->kind = MF_DAMAGED; return 0; }
		m->stsc.off = at + 8; m->stsc.n = be32(h + 4) * 3;
		if (box(io, stbl, stbl + stbl_n, "stco", &at, &len)) m->co64 = 0;
		else if (box(io, stbl, stbl + stbl_n, "co64", &at, &len)) m->co64 = 1;
		else { m->kind = MF_DAMAGED; return 0; }
		if (len < 8 || rd(io, at, h, 8) != 8) { m->kind = MF_DAMAGED; return 0; }
		m->stco.off = at + 8; m->stco.n = be32(h + 4) * (m->co64 ? 2 : 1);
		m->stsz.cache_n = m->stsc.cache_n = m->stco.cache_n = 0;
		if (!m->npackets || !m->stsc.n || !m->stco.n || !m->frame_len || !m->rate) { m->kind = MF_DAMAGED; return 0; }
	}
	if (path(io, moov, moov + moov_n, "udtameta", &at, &len) && len > 12) {
		unsigned long ilst, ilst_n;
		unsigned char v[8];
		/* 'meta' is a full box (version and flags first) in MP4, a plain one in some QuickTime files */
		if (rd(io, at, v, 8) == 8 && !same4(v + 4, "hdlr") && !same4(v + 4, "ilst")) { at += 4; len -= 4; }
		if (box(io, at, at + len, "ilst", &ilst, &ilst_n)) {
			ilst_text(io, ilst, ilst_n, "\xA9nam", m->title, sizeof m->title);
			ilst_text(io, ilst, ilst_n, "\xA9" "ART", m->artist, sizeof m->artist);
			ilst_text(io, ilst, ilst_n, "\xA9" "alb", m->album, sizeof m->album);
		}
	}
	if (!m->frames) m->frames = m->npackets * m->frame_len;
	m->length_ms = mul_div(m->frames, 1000, m->rate);
	m->kind = MF_ALAC;
	return 1;
}
/* a word of a sample table, through a cache of 32 */
static unsigned long tab(const struct mf_io *io, struct mf_tab *t, unsigned long i)
{
	if (i >= t->n) return 0;
	if (!t->cache_n || i < t->cache_at || i >= t->cache_at + (unsigned long)t->cache_n) {
		unsigned char b[128];
		unsigned long k, n = t->n - i < 32 ? t->n - i : 32;
		long got = rd(io, t->off + 4 * i, b, (long)(4 * n));
		if (got < 4) return 0;
		t->cache_at = i; t->cache_n = (int)(got / 4);
		for (k = 0; k < (unsigned long)t->cache_n; k++) t->cache[k] = be32(b + 4 * k);
	}
	return t->cache[i - t->cache_at];
}
static unsigned long pkt_size(struct media *m, const struct mf_io *io, unsigned long i) { return m->fixed_size ? m->fixed_size : tab(io, &m->stsz, i); }
static unsigned long chunk_off(struct media *m, const struct mf_io *io, unsigned long c)   /* c from 1 */
{
	return m->co64 ? tab(io, &m->stco, 2 * (c - 1) + 1) : tab(io, &m->stco, c - 1);
}
static void stsc_entry(struct media *m, const struct mf_io *io, struct mf_stream *s)
{
	unsigned long e = s->stsc_i, n = m->stsc.n / 3, chunks = m->co64 ? m->stco.n / 2 : m->stco.n;
	s->spc = tab(io, &m->stsc, 3 * e + 1);
	s->next_first = e + 1 < n ? tab(io, &m->stsc, 3 * (e + 1)) : chunks + 1;
}
/* the packet i's place: its chunk, its place in the chunk, its offset */
static int alac_at(struct media *m, const struct mf_io *io, struct mf_stream *s, unsigned long i)
{
	unsigned long first_sample = 0, n = m->stsc.n / 3, k;
	for (s->stsc_i = 0; s->stsc_i < n; s->stsc_i++) {
		unsigned long first = tab(io, &m->stsc, 3 * s->stsc_i), per;
		stsc_entry(m, io, s);
		if (!s->spc || s->next_first <= first) return 0;
		per = (s->next_first - first) * s->spc;
		if (i < first_sample + per) {
			s->chunk = first + (i - first_sample) / s->spc;
			s->in_chunk = (i - first_sample) % s->spc;
			s->pkt_off = chunk_off(m, io, s->chunk);
			for (k = i - s->in_chunk; k < i; k++) s->pkt_off += pkt_size(m, io, k);
			s->pkt = i; s->pkt_size = pkt_size(m, io, i); s->pkt_sent = 0;
			return 1;
		}
		first_sample += per;
	}
	return 0;
}
static void alac_advance(struct media *m, const struct mf_io *io, struct mf_stream *s)
{
	s->pkt_off += s->pkt_size;
	s->pkt++;
	if (++s->in_chunk >= s->spc) {
		s->in_chunk = 0;
		s->chunk++;
		if (s->chunk >= s->next_first) { s->stsc_i++; stsc_entry(m, io, s); }
		s->pkt_off = chunk_off(m, io, s->chunk);
	}
	s->pkt_size = pkt_size(m, io, s->pkt);
	s->pkt_sent = 0;
}

/* ---- the three together ------------------------------------------------------------------------------- */
int mf_identify(struct media *m, const struct mf_io *io, const unsigned char *b, long got)
{
	unsigned long p = 0;
	unsigned char *z = (unsigned char *)m;
	unsigned long i;
	for (i = 0; i < sizeof *m; i++) z[i] = 0;
	if (got < 12) return 0;
	if (b[0] == 'I' && b[1] == 'D' && b[2] == '3' && got >= 10)       /* an ID3v2 tag before FLAC: some taggers write one */
		p = 10 + ((unsigned long)(b[6] & 0x7F) << 21 | (unsigned long)(b[7] & 0x7F) << 14 | (unsigned long)(b[8] & 0x7F) << 7 | (b[9] & 0x7F)) + ((b[5] & 0x10) ? 10 : 0);
	if (p + 4 <= (unsigned long)got ? same4(b + p, "fLaC") : (rd(io, p, tb, 4) == 4 && same4(tb, "fLaC"))) return flac(m, io, p + 4);
	if (same4(b, "OggS")) return ogg(m, io);
	if (same4(b + 4, "ftyp")) return m4a(m, io);
	return 0;
}
const char *mf_name(const struct media *m)
{
	return m->kind == MF_FLAC ? "FLAC" : m->kind == MF_OGG ? "Ogg Vorbis" : m->kind == MF_ALAC ? "Apple Lossless" : "";
}
const char *mf_why(const struct media *m)
{
	switch (m->kind) {
	case MF_AAC: return "This file is AAC (an .m4a file from iTunes or a phone). The sound card cannot decode AAC, "
	                    "but the music server can: put the file in its library and play it from the Library.";
	case MF_OPUS: return "This file is Opus in an Ogg file. Aurora68 Play plays Ogg Vorbis but not Opus; the music server converts Opus.";
	case MF_OGGFLAC: return "This file is FLAC in an Ogg file, which Aurora68 Play does not play. A plain .flac file of it plays.";
	case MF_M4A_OTHER: return "This MP4 file holds no sound Aurora68 Play knows: it plays Apple Lossless in .m4a files.";
	case MF_DAMAGED: return "This file looks like FLAC, Ogg or .m4a, but its headers are damaged or cut short.";
	}
	return "";
}
int mf_seek(struct media *m, struct mf_stream *s, const struct mf_io *io, unsigned long ms, unsigned long *start_ms)
{
	unsigned char *z = (unsigned char *)s;
	unsigned long i;
	for (i = 0; i < sizeof *s; i++) z[i] = 0;
	s->phase = m->pre_n ? 0 : 1;
	*start_ms = 0;
	if (m->length_ms && ms >= m->length_ms) ms = m->length_ms ? m->length_ms - 1 : 0;
	s->file_at = m->audio_off;
	if (m->kind == MF_ALAC) {
		unsigned long frame = mul_div(ms, m->rate, 1000), pk = frame / m->frame_len;
		if (pk >= m->npackets) pk = m->npackets ? m->npackets - 1 : 0;
		*start_ms = mul_div(pk * m->frame_len, 1000, m->rate);
		return alac_at(m, io, s, pk);
	}
	if (!ms) return 1;
	if (m->kind == MF_FLAC && m->seek_n) {                /* the last seek point at or before the time */
		unsigned long want = mul_div(ms, m->rate, 1000), best_s = 0, best_o = 0, k;
		for (k = 0; k < m->seek_n; k++) {
			unsigned char e[18];
			unsigned long smp, off;
			if (rd(io, m->seek_off + 18 * k, e, 18) != 18) break;
			if (be32(e) == 0xFFFFFFFFul || be32(e)) continue;      /* a placeholder, or beyond 32 bits */
			smp = be32(e + 4); off = be32(e + 12);
			if (be32(e + 8)) continue;
			if (smp > want) break;
			best_s = smp; best_o = off;
		}
		s->file_at = m->audio_off + best_o;
		*start_ms = mul_div(best_s, 1000, m->rate);
		return 1;
	}
	if (!m->length_ms) return 1;
	s->file_at = m->audio_off + mul_div(m->audio_len, ms, m->length_ms);
	*start_ms = ms;
	if (m->kind == MF_OGG) {                               /* on to the next page */
		unsigned long at = s->file_at, end = at + 131072 > io->size ? io->size : at + 131072;
		while (at + 27 < end) {
			long n = rd(io, at, tb, sizeof tb), k;
			for (k = 0; k + 27 <= n; k++)
				if (tb[k] == 'O' && tb[k + 1] == 'g' && tb[k + 2] == 'g' && tb[k + 3] == 'S' && tb[k + 4] == 0) {
					unsigned long g = le32(tb + k + 6);
					s->file_at = at + (unsigned long)k;
					if (g != 0xFFFFFFFFul && !(tb[k + 10] | tb[k + 11] | tb[k + 12] | tb[k + 13])) *start_ms = mul_div(g, 1000, m->rate);
					return 1;
				}
			if (n < 28) break;
			at += (unsigned long)n - 26;
		}
		return 0;
	}
	return 1;
}
long mf_next(struct media *m, struct mf_stream *s, const struct mf_io *io, unsigned char *buf, long max)
{
	long put = 0;
	while (put < max) {
		if (s->phase == 0) {                              /* the configuration or STREAMINFO */
			long n = m->pre_n - (long)s->at;
			if (n > max - put) n = max - put;
			cpy(buf + put, m->pre + s->at, (unsigned long)n);
			put += n; s->at += (unsigned long)n;
			if (s->at >= (unsigned long)m->pre_n) { s->phase = 1; s->at = 0; }
		} else if (s->phase == 1) {                       /* the file's own headers (Ogg) */
			long n = (long)(m->hdr_len - s->at), got;
			if (n <= 0) { s->phase = 2; continue; }
			if (n > max - put) n = max - put;
			got = rd(io, m->hdr_off + s->at, buf + put, n);
			if (got <= 0) return -1;
			put += got; s->at += (unsigned long)got;
		} else if (s->phase == 2 && m->kind == MF_ALAC) { /* a packet: its length, then its bytes */
			if (s->pkt >= m->npackets) { s->phase = 3; break; }
			if (!s->pkt_sent) {
				if (max - put < 4) break;
				buf[put] = (unsigned char)(s->pkt_size >> 24); buf[put + 1] = (unsigned char)(s->pkt_size >> 16);
				buf[put + 2] = (unsigned char)(s->pkt_size >> 8); buf[put + 3] = (unsigned char)s->pkt_size;
				put += 4;
				s->pkt_sent = 4;
			}
			{
				long n = (long)(s->pkt_size + 4 - s->pkt_sent), got;
				if (n > max - put) n = max - put;
				if (n > 0) {
					got = rd(io, s->pkt_off + s->pkt_sent - 4, buf + put, n);
					if (got <= 0) return -1;
					put += got; s->pkt_sent += (unsigned long)got;
				}
				if (s->pkt_sent >= s->pkt_size + 4) alac_advance(m, io, s);
			}
		} else if (s->phase == 2) {                       /* FLAC frames, Ogg pages: the file as it is */
			long n, got;
			if (s->file_at >= io->size) { s->phase = 3; break; }
			n = (long)(io->size - s->file_at);
			if (n > max - put) n = max - put;
			got = rd(io, s->file_at, buf + put, n);
			if (got <= 0) return -1;
			put += got; s->file_at += (unsigned long)got;
		} else break;
	}
	return put;
}
