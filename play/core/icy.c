/* SPDX-License-Identifier: MPL-2.0
 *
 * play/core/icy.c - an internet radio station's answer, taken apart (icy.h says what and why).
 *
 * The protocol is Shoutcast's and Icecast's, as their clients have used it since Winamp: the client
 * asks with "Icy-MetaData: 1", and the server answers with "icy-metaint: N" among its headers and then
 * puts, after every N bytes of audio, one length byte L and 16 x L bytes of text such as
 * "StreamTitle='Artist - Title';StreamUrl='';" padded with NULs (L is 0 when nothing changed). Shoutcast
 * 1 answers "ICY 200 OK" instead of an HTTP status line. Icecast answers HTTP/1.0 to an HTTP/1.0
 * request; an HTTP/1.1 answer may come in chunks (Transfer-Encoding: chunked), which are undone here
 * before the metadata is looked for, because metaint counts the stream's bytes, not the chunks'.
 * Header names are compared without regard to case: SomaFM's Icecast sends "icy-metaint", older
 * servers "Icy-MetaInt". The titles are UTF-8 from Icecast and Latin-1 from older Shoutcasts; both
 * become Mac Roman, which is what the window draws and the card's title field holds.
 *
 * Everything here may run inside MacTCP's completion routine, at interrupt time: no allocation, no
 * Toolbox call, nothing but the struct the caller owns. Written on 21 November 2026.
 */
#include "icy.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
/* does s start with the lower-case word w, ignoring case? */
static int starts(const char *s, const char *w)
{
	for (; *w; s++, w++)
		if (lower((unsigned char)*s) != *w) return 0;
	return 1;
}
/* the same word, ignoring case */
static int same(const char *s, const char *w)
{
	for (; *w; s++, w++)
		if (lower((unsigned char)*s) != *w) return 0;
	return !*s;
}
static void copy(char *d, const char *s, int max)
{
	int i = 0;
	while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
	d[i] = 0;
}
static unsigned long number(const char *s)
{
	unsigned long v = 0;
	while (*s == ' ' || *s == '\t') s++;
	while (*s >= '0' && *s <= '9' && v < 100000000ul) v = v * 10 + (unsigned long)(*s++ - '0');
	return v;
}

/* ---- Mac Roman ----------------------------------------------------------------------------- */
/* Latin-1's upper half, 0xA0-0xFF, in Mac Roman (Inside Macintosh: Text, the standard roman
   character set); characters Mac Roman lacks become their nearest plain form or a question mark */
static const unsigned char latin1_mac[96] = {
	0xCA, 0xC1, 0xA2, 0xA3, 0xDB, 0xB4, '|', 0xA4, 0xAC, 0xA9, 0xBB, 0xC7, 0xC2, '-', 0xA8, 0xF8,
	0xA1, 0xB1, '2', '3', 0xAB, 0xB5, 0xA6, 0xE1, 0xFC, '1', 0xBC, 0xC8, '?', '?', '?', 0xC0,
	0xCB, 0xE7, 0xE5, 0xCC, 0x80, 0x81, 0xAE, 0x82, 0xE9, 0x83, 0xE6, 0xE8, 0xED, 0xEA, 0xEB, 0xEC,
	'?', 0x84, 0xF1, 0xEE, 0xEF, 0xCD, 0x85, 'x', 0xAF, 0xF4, 0xF2, 0xF3, 0x86, 'Y', '?', 0xA7,
	0x88, 0x87, 0x89, 0x8B, 0x8A, 0x8C, 0xBE, 0x8D, 0x8F, 0x8E, 0x90, 0x91, 0x93, 0x92, 0x94, 0x95,
	'?', 0x96, 0x98, 0x97, 0x99, 0x9B, 0x9A, 0xD6, 0xBF, 0x9D, 0x9C, 0x9E, 0x9F, 'y', '?', 0xD8 };
unsigned char icy_mac_char(unsigned long u)
{
	if (u < 0x20) return ' ';
	if (u < 0x80) return (unsigned char)u;
	if (u >= 0xA0 && u <= 0xFF) return latin1_mac[u - 0xA0];
	switch (u) {
	case 0x2018: return 0xD4; case 0x2019: return 0xD5; case 0x201C: return 0xD2; case 0x201D: return 0xD3;
	case 0x2013: return 0xD0; case 0x2014: return 0xD1; case 0x2026: return 0xC9; case 0x2022: return 0xA5;
	}
	return '?';
}
/* is the text valid UTF-8 (and not plain ASCII, which is the same either way)? */
static int is_utf8(const unsigned char *t, unsigned long n)
{
	unsigned long i = 0;
	while (i < n) {
		unsigned c = t[i];
		int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
		if (extra < 0 || (extra && c < 0xC2 && extra == 1)) return 0;
		for (i++; extra; extra--, i++)
			if (i >= n || (t[i] & 0xC0) != 0x80) return 0;
	}
	return 1;
}
void icy_to_mac(const unsigned char *in, unsigned long n, char *out, int max)
{
	int o = 0, utf8 = is_utf8(in, n);
	unsigned long i = 0;
	while (i < n && o < max - 1) {
		unsigned long u = in[i++];
		if (utf8 && u >= 0xC0) {
			int extra = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : 1;
			u &= 0x3F >> extra;
			for (; extra && i < n; extra--, i++) u = u << 6 | (in[i] & 0x3F);
		}
		if (!u) break;
		out[o++] = (char)icy_mac_char(u);
	}
	while (o && out[o - 1] == ' ') o--;
	out[o] = 0;
}

/* ---- the metadata blocks ------------------------------------------------------------------- */
/* StreamTitle='...'; out of a block. The value ends at a quote followed by ";" (or by the block's end
   or its NUL padding), so a title with an apostrophe in it ("Don't Stop") is kept whole. */
static void meta_block(struct icy *s, unsigned *ev)
{
	unsigned n = s->meta_got < sizeof s->meta - 1 ? s->meta_got : (unsigned)sizeof s->meta - 1;
	unsigned i, a = 0, b = 0, found = 0;
	char t[128];
	s->meta[n] = 0;
	s->meta_blocks++;
	for (i = 0; i + 13 <= n; i++)
		if (starts(s->meta + i, "streamtitle='")) { a = i + 13; found = 1; break; }
	if (!found) return;
	for (b = a; b < n && s->meta[b]; b++)
		if (s->meta[b] == '\'' && (b + 1 >= n || s->meta[b + 1] == ';' || !s->meta[b + 1])) break;
	icy_to_mac((const unsigned char *)s->meta + a, b - a, t, sizeof t);
	for (i = 0; t[i] == s->title[i] && t[i]; i++) { }
	if (t[i] == s->title[i]) return;                 /* the same title again */
	copy(s->title, t, sizeof s->title);
	s->title_seq++;
	*ev |= ICY_EV_TITLE;
}
/* the body without its chunks: audio and metadata apart, or a playlist's text */
static void body(struct icy *s, const unsigned char *p, unsigned long n, icy_audio_fn audio, unsigned *ev)
{
	s->body_bytes += n;
	if (s->kind == ICY_K_PLAYLIST) {
		while (n-- && s->pl_len < ICY_PLAYLIST) s->pl[s->pl_len++] = *p++;
		return;
	}
	while (n) {
		if (!s->meta_state) {
			unsigned long k = n;
			if (s->metaint && k > s->audio_left) k = s->audio_left;
			if (k) {
				audio(p, k);
				s->audio_bytes += k;
				p += k; n -= k;
				if (s->metaint) s->audio_left -= k;
			}
			if (s->metaint && !s->audio_left && n) {     /* the length byte */
				s->meta_len = 16u * *p++; n--;
				s->meta_got = 0;
				if (s->meta_len) s->meta_state = 1;
				else s->audio_left = s->metaint;
			}
		} else {
			unsigned long k = s->meta_len - s->meta_got, j;
			if (k > n) k = n;
			for (j = 0; j < k; j++)
				if (s->meta_got + j < sizeof s->meta - 1) s->meta[s->meta_got + j] = (char)p[j];
			s->meta_got += (unsigned)k; p += k; n -= k;
			if (s->meta_got == s->meta_len) {
				meta_block(s, ev);
				s->meta_state = 0;
				s->audio_left = s->metaint;
			}
		}
	}
}
/* HTTP/1.1's chunks (RFC 7230 4.1): a size in hex (perhaps with ";extensions"), CRLF, the data, CRLF;
   a size of 0 ends the body */
static void dechunk(struct icy *s, const unsigned char *p, unsigned long n, icy_audio_fn audio, unsigned *ev)
{
	while (n && s->state == ICY_BODY) {
		int c;
		switch (s->ch_state) {
		case 0:                                          /* the size */
		case 4:                                          /* its extensions, up to the LF */
			c = *p++; n--;
			if (c == '\n') {
				s->ch_state = s->ch_left ? 1 : 3;
				if (s->ch_state == 3) s->state = ICY_DONE;
			} else if (s->ch_state == 0 && c != '\r' && c != ' ') {
				int d = c >= '0' && c <= '9' ? c - '0' : lower(c) >= 'a' && lower(c) <= 'f' ? lower(c) - 'a' + 10 : -1;
				if (c == ';') s->ch_state = 4;
				else if (d < 0 || s->ch_left > 0x0FFFFFFFul) { s->state = ICY_DONE; *ev |= ICY_EV_ERROR; }
				else s->ch_left = s->ch_left * 16 + (unsigned long)d;
			}
			break;
		case 1: {                                        /* data */
			unsigned long k = n < s->ch_left ? n : s->ch_left;
			body(s, p, k, audio, ev);
			p += k; n -= k; s->ch_left -= k;
			if (!s->ch_left) s->ch_state = 2;
			break;
		}
		case 2:                                          /* the CRLF after the data */
			c = *p++; n--;
			if (c == '\n') { s->ch_state = 0; s->ch_left = 0; }
			else if (c != '\r') { s->state = ICY_DONE; *ev |= ICY_EV_ERROR; }
			break;
		default:
			n = 0;
		}
	}
}

/* ---- the status line and the headers ------------------------------------------------------- */
static int kind_of(const char *t)
{
	static const char *const audio[] = { "audio/mpeg", "audio/mp3", "audio/mpeg3", "audio/x-mpeg", "audio/x-mp3",
	                                     "audio/mpg", "application/octet-stream", 0 };
	static const char *const lists[] = { "audio/x-scpls", "audio/scpls", "audio/x-mpegurl", "audio/mpegurl",
	                                     "application/pls+xml", "application/x-mpegurl", 0 };
	int i;
	if (!t[0]) return ICY_K_AUDIO;                   /* Shoutcast 1 often says nothing: MP3 */
	for (i = 0; audio[i]; i++)
		if (same(t, audio[i])) return ICY_K_AUDIO;
	for (i = 0; lists[i]; i++)
		if (same(t, lists[i])) return ICY_K_PLAYLIST;
	if (same(t, "video/mpeg") || same(t, "video/mpg") || same(t, "video/x-mpeg")) return ICY_K_VIDEO;
	return ICY_K_OTHER;
}
static void header_line(struct icy *s, unsigned *ev)
{
	char *l = s->line, *v;
	if (s->state == ICY_STATUS) {
		if (starts(l, "http/")) { while (*l && *l != ' ') l++; }
		else if (starts(l, "icy ")) { s->icy = 1; l += 3; }
		else { s->state = ICY_DONE; *ev |= ICY_EV_ERROR; return; }
		s->status = (int)number(l);
		while (*l == ' ') l++;
		while (*l >= '0' && *l <= '9') l++;
		while (*l == ' ') l++;
		copy(s->reason, l, sizeof s->reason);
		if (!s->status) { s->state = ICY_DONE; *ev |= ICY_EV_ERROR; return; }
		s->state = ICY_HEADERS;
		return;
	}
	if (!l[0]) {                                     /* the blank line: the body follows */
		s->kind = kind_of(s->ctype);
		*ev |= ICY_EV_HEADERS;
		s->audio_left = s->metaint;
		s->state = s->status >= 200 && s->status < 300 && s->kind != ICY_K_OTHER ? ICY_BODY : ICY_DONE;
		return;
	}
	for (v = l; *v && *v != ':'; v++) { }
	if (!*v) return;
	*v++ = 0;
	while (*v == ' ' || *v == '\t') v++;
	if (same(l, "icy-metaint")) s->metaint = number(v);
	else if (same(l, "icy-br")) s->br = number(v);
	else if (same(l, "icy-name")) {
		unsigned long n = 0;
		while (v[n]) n++;
		icy_to_mac((const unsigned char *)v, n, s->name, sizeof s->name);
	} else if (same(l, "location")) copy(s->location, v, sizeof s->location);
	else if (same(l, "transfer-encoding")) s->chunked = starts(v, "chunked");
	else if (same(l, "content-type")) {
		int i;
		for (i = 0; v[i] && v[i] != ';' && v[i] != ' ' && i < (int)sizeof s->ctype - 1; i++) s->ctype[i] = (char)lower((unsigned char)v[i]);
		s->ctype[i] = 0;
	}
}

void icy_init(struct icy *s)
{
	unsigned char *b = (unsigned char *)s;
	unsigned long i;
	for (i = 0; i < sizeof *s; i++) b[i] = 0;
}
unsigned icy_feed(struct icy *s, const unsigned char *p, unsigned long n, icy_audio_fn audio)
{
	unsigned ev = 0;
	while (n && (s->state == ICY_STATUS || s->state == ICY_HEADERS)) {
		int c = *p++;
		n--;
		if (c == '\n') {
			while (s->linelen && s->line[s->linelen - 1] == '\r') s->linelen--;
			s->line[s->linelen] = 0;
			s->linelen = 0;
			header_line(s, &ev);
		} else {
			if (s->linelen < sizeof s->line - 1) s->line[s->linelen++] = (char)c;
			/* an answer that is no HTTP: say so at once rather than read a megabyte for a line */
			if (s->state == ICY_STATUS && s->linelen == 4 && !starts(s->line, "http") && !starts(s->line, "icy ")) {
				s->state = ICY_DONE;
				ev |= ICY_EV_ERROR;
			}
		}
	}
	if (n && s->state == ICY_BODY) {
		if (s->chunked) dechunk(s, p, n, audio, &ev);
		else body(s, p, n, audio, &ev);
	}
	return ev;
}

int icy_playlist_url(const unsigned char *t, unsigned n, char *out, int max, int *https)
{
	unsigned i = 0;
	*https = 0;
	while (i < n) {
		unsigned a = i, e;
		while (i < n && t[i] != '\r' && t[i] != '\n') i++;
		e = i;
		while (i < n && (t[i] == '\r' || t[i] == '\n')) i++;
		while (a < e && (t[a] == ' ' || t[a] == '\t')) a++;
		if (e - a > 4 && lower(t[a]) == 'f' && lower(t[a + 1]) == 'i' && lower(t[a + 2]) == 'l' && lower(t[a + 3]) == 'e') {
			unsigned k = a + 4;                      /* .pls: FileN=URL */
			while (k < e && t[k] >= '0' && t[k] <= '9') k++;
			if (k < e && t[k] == '=') a = k + 1;
		}
		while (e > a && (t[e - 1] == ' ' || t[e - 1] == '\t')) e--;
		if (e - a > 8 && starts((const char *)t + a, "https://")) *https = 1;
		if (e - a > 7 && starts((const char *)t + a, "http://")) {
			int o = 0;
			while (a < e && o < max - 1) out[o++] = (char)t[a++];
			out[o] = 0;
			return 1;
		}
	}
	return 0;
}

/* MacTCP sees IPv4 addresses only. Riptide gives a MacTCP program a stand-in from 198.18.0.0/15 (the
   benchmarking block, RFC 2544, which no real host has) for a host it reaches over IPv6, and carries the
   connection over IPv6 (RFC 6535, "Bump-in-the-Host"; Riptide's docs/MAC.md, "IPv6 for MacTCP
   programs"). So the address alone says which: nothing Riptide's own is asked, and with Apple's MacTCP
   every address is an IPv4 one. */
int icy_ip_version(unsigned long addr)
{
	return (addr & 0xFFFE0000ul) == 0xC6120000ul ? 6 : 4;
}
