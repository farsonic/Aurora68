/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/icy_test.c - PiPlay's radio parser (play/core/icy.c, mac/piplay/icy.c until 22 November 2026) on the host: `make -C mac check`.
 *
 * Streams are made up here the way Icecast and Shoutcast send them (an HTTP or ICY status line,
 * headers, audio with a metadata block after every metaint bytes, sometimes in HTTP/1.1 chunks) and
 * fed to the parser whole, a byte at a time and in pieces of random sizes, as TCPRcv hands them over.
 * The audio that comes out must be the audio that went in, byte for byte, and the titles must come in
 * order, in Mac Roman. Written on 21 November 2026.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../play/core/include/icy.h"

static unsigned char out[1 << 20];
static unsigned long out_n;
static void sink(const unsigned char *p, unsigned long n)
{
	if (out_n + n > sizeof out) { fprintf(stderr, "too much audio\n"); exit(1); }
	memcpy(out + out_n, p, n);
	out_n += n;
}
static int fails, checks;
static void check(int ok, const char *what)
{
	checks++;
	if (!ok) { fails++; printf("FAIL %s\n", what); }
}
static unsigned long rnd_state = 12345;
static unsigned rnd(unsigned n) { rnd_state = rnd_state * 1103515245u + 12345u; return (unsigned)(rnd_state >> 8) % n; }

static unsigned char stream[1 << 21], audio[1 << 20];
static unsigned long stream_n, audio_n;
static void put(const void *p, unsigned long n) { memcpy(stream + stream_n, p, n); stream_n += n; }
static void puts_(const char *s) { put(s, strlen(s)); }
static const char *titles[] = { "First Artist - First Song", "Don't Stop - Apostrophe's Test", "Caf\xC3\xA9 del Mar \xE2\x80\x94 UTF-8",
                                "Caf\xE9 Latin-1", "", "Last" };
static const char *mac_titles[] = { "First Artist - First Song", "Don't Stop - Apostrophe's Test", "Caf\x8E del Mar \xD1 UTF-8",
                                    "Caf\x8E Latin-1", "", "Last" };

/* audio with a metadata block every metaint bytes; a new title every few blocks, empty blocks between */
static void make_body(unsigned long metaint, unsigned blocks)
{
	unsigned b, t = 0;
	audio_n = 0;
	for (b = 0; b < blocks; b++) {
		unsigned long i;
		for (i = 0; i < metaint; i++) {
			unsigned char c = (unsigned char)(audio_n * 7 + (audio_n >> 8));
			audio[audio_n++] = c;
			put(&c, 1);
		}
		if (b % 3 == 0 && t < 6) {
			char m[600];
			int len = snprintf(m, sizeof m, "StreamTitle='%s';StreamUrl='http://example.invalid/';", titles[t++]);
			int L = (len + 15) / 16;
			unsigned char lb = (unsigned char)L;
			put(&lb, 1);
			memset(m + len, 0, (size_t)(L * 16 - len));
			put(m, (unsigned long)L * 16);
		} else {
			unsigned char z = 0;
			put(&z, 1);
		}
	}
	/* a last partial stretch of audio, as a connection that drops leaves */
	for (b = 0; b < metaint / 2; b++) { unsigned char c = (unsigned char)b; audio[audio_n++] = c; put(&c, 1); }
}
/* the same bytes as HTTP/1.1 chunks of random sizes, with an extension now and then */
static void chunk_it(void)
{
	static unsigned char body[1 << 21];
	unsigned long n = stream_n, i = 0;
	memcpy(body, stream, n);
	stream_n = 0;
	while (i < n) {
		unsigned long k = 1 + rnd(3000);
		char h[40];
		if (k > n - i) k = n - i;
		snprintf(h, sizeof h, rnd(5) ? "%lx\r\n" : "%lX;name=value\r\n", k);
		puts_(h);
		put(body + i, k);
		puts_("\r\n");
		i += k;
	}
	puts_("0\r\n\r\n");
}

struct run { unsigned long titles; char last[128]; unsigned events; int heads; };
/* feeds stream[] to a fresh parser in pieces: mode 0 whole, 1 a byte at a time, 2 random pieces */
static struct icy S;
static void feed_all(int mode, struct run *r, char seen[][128])
{
	unsigned long i = 0;
	icy_init(&S);
	out_n = 0;
	memset(r, 0, sizeof *r);
	while (i < stream_n) {
		unsigned long k = mode == 0 ? stream_n : mode == 1 ? 1 : 1 + rnd(9000);
		if (k > stream_n - i) k = stream_n - i;
		unsigned ev = icy_feed(&S, stream + i, k, sink);
		r->events |= ev;
		if (ev & ICY_EV_HEADERS) r->heads++;
		if ((ev & ICY_EV_TITLE) && seen) strcpy(seen[r->titles++], S.title);
		i += k;
	}
}

static void test_stream(const char *name, const char *head, unsigned long metaint, int chunked)
{
	char what[200];
	int mode;
	stream_n = 0;
	make_body(metaint, metaint > 20000 ? 20 : 40);
	if (chunked) chunk_it();
	{   /* the head in front */
		static unsigned char tmp[1 << 21];
		unsigned long n = stream_n;
		memcpy(tmp, stream, n);
		stream_n = 0;
		puts_(head);
		put(tmp, n);
	}
	for (mode = 0; mode < 3; mode++) {
		struct run r;
		char seen[10][128];
		unsigned t;
		feed_all(mode, &r, seen);
		snprintf(what, sizeof what, "%s (mode %d): headers once, status 200, audio", name, mode);
		check(r.heads == 1 && S.status == 200 && S.kind == ICY_K_AUDIO && !(r.events & ICY_EV_ERROR), what);
		snprintf(what, sizeof what, "%s (mode %d): the audio byte for byte (%lu of %lu)", name, mode, out_n, audio_n);
		check(out_n == audio_n && !memcmp(out, audio, audio_n), what);
		/* fed whole, the six come in one call and the last is what shows; a byte at a time, each one */
		snprintf(what, sizeof what, "%s (mode %d): six titles, \"Last\" the last", name, mode);
		check(S.title_seq == 6 && !strcmp(S.title, "Last"), what);
		for (t = 0; mode == 1 && t < 6; t++) {
			snprintf(what, sizeof what, "%s (mode %d): title %u \"%s\"", name, mode, t, seen[t]);
			check(!strcmp(seen[t], mac_titles[t]), what);
		}
		snprintf(what, sizeof what, "%s (mode %d): metaint %lu, name \"%s\", br %lu", name, mode, S.metaint, S.name, S.br);
		check(S.metaint == metaint && S.br == 128, what);
	}
}

static void simple(const char *name, const char *text, int status, int kind, int done)
{
	struct run r;
	char what[200];
	stream_n = 0;
	puts_(text);
	feed_all(1, &r, 0);
	snprintf(what, sizeof what, "%s: status %d (got %d), kind %d (got %d), state %s", name, status, S.status, kind, S.kind,
	         done ? "done" : "body");
	check(S.status == status && (!kind || S.kind == kind) && (S.state == ICY_DONE) == done, what);
}

int main(void)
{
	test_stream("Icecast, HTTP/1.0", "HTTP/1.0 200 OK\r\nServer: Icecast 2.4.4\r\ncontent-type: audio/mpeg\r\nicy-br:128\r\n"
	            "icy-name: Test Station\r\nicy-metaint:16000\r\nicy-pub: 0\r\n\r\n", 16000, 0);
	test_stream("Icecast, HTTP/1.1 chunks", "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nTransfer-Encoding: chunked\r\n"
	            "icy-br: 128\r\nicy-name: Test Station\r\nicy-metaint: 8192\r\n\r\n", 8192, 1);
	test_stream("Shoutcast 1, ICY and LF only", "ICY 200 OK\nicy-notice1:<BR>This stream requires Winamp<BR>\n"
	            "icy-name:Test Station\nicy-genre:Test\nicy-br:128\nIcy-MetaInt:45000\n\n", 45000, 0);
	test_stream("small metaint", "HTTP/1.0 200 OK\r\nContent-Type: audio/mpeg; charset=none\r\nicy-br: 128\r\nicy-metaint: 1\r\n\r\n", 1, 0);
	check(!strcmp(S.ctype, "audio/mpeg"), "Content-Type's parameters dropped");
	{   /* no metadata asked for or given: everything is audio */
		struct run r;
		stream_n = 0;
		puts_("HTTP/1.0 200 OK\r\nContent-Type: audio/mpeg\r\n\r\n");
		unsigned long i;
		for (i = 0; i < 70000; i++) { unsigned char c = (unsigned char)(i * 13); put(&c, 1); audio[i] = c; }
		feed_all(2, &r, 0);
		check(out_n == 70000 && !memcmp(out, audio, 70000), "no icy-metaint: all audio");
	}
	simple("a redirection", "HTTP/1.1 302 Found\r\nLocation: http://ice2.example.invalid:8000/stream-128-mp3\r\n"
	       "Content-Length: 5\r\n\r\nmoved", 302, ICY_K_AUDIO, 1);
	check(!strcmp(S.location, "http://ice2.example.invalid:8000/stream-128-mp3") && out_n == 0, "the Location, and no audio from a 302");
	simple("not found", "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n<html>", 404, ICY_K_OTHER, 1);
	check(!strcmp(S.reason, "Not Found"), "the reason");
	simple("426", "HTTP/1.1 426 Upgrade Required\r\ncontent-length: 16\r\n\r\nUpgrade Required", 426, 0, 1);
	simple("a web page", "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n<html>", 200, ICY_K_OTHER, 1);
	simple("AAC", "HTTP/1.0 200 OK\r\nContent-Type: audio/aacp\r\n\r\n", 200, ICY_K_OTHER, 1);
	{   /* the music server's /vmpeg/ (23 November 2026): video/mpeg, its body passed on whole for the video card */
		struct run r;
		unsigned long i;
		stream_n = 0;
		puts_("HTTP/1.0 200 OK\r\nContent-Type: video/mpeg\r\nConnection: close\r\nX-Aurora-Seconds: 40\r\n\r\n");
		for (i = 0; i < 5000; i++) { unsigned char c = (unsigned char)(i * 7 + 1); put(&c, 1); audio[i] = c; }
		feed_all(2, &r, 0);
		check(S.status == 200 && S.kind == ICY_K_VIDEO && S.state == ICY_BODY, "video/mpeg: a video body");
		check(out_n == 5000 && !memcmp(out, audio, 5000), "video/mpeg: every byte passed on");
	}
	simple("a playlist", "HTTP/1.0 200 OK\r\nContent-Type: audio/x-scpls\r\n\r\n[playlist]\nnumberofentries=2\n"
	       "File1=https://ice6.example.invalid/x-128-mp3\nTitle1=x\nFile2=http://ice2.example.invalid/x-128-mp3\nLength2=-1\n", 200, ICY_K_PLAYLIST, 0);
	{
		char u[200];
		int https;
		int ok = icy_playlist_url(S.pl, S.pl_len, u, sizeof u, &https);
		check(ok && !strcmp(u, "http://ice2.example.invalid/x-128-mp3") && https, "the playlist's first http:// URL");
		static const char m3u[] = "#EXTM3U\r\n#EXTINF:-1,Test\r\nhttps://a.example.invalid/s\r\n";
		ok = icy_playlist_url((const unsigned char *)m3u, sizeof m3u - 1, u, sizeof u, &https);
		check(!ok && https, "an .m3u with https only");
	}
	{   /* no HTTP at all (a raw MP3 server), noticed at once */
		struct run r;
		stream_n = 0;
		unsigned char junk[] = { 0xFF, 0xFB, 0x90, 0x64, 1, 2, 3, 4, 5, 6 };
		put(junk, sizeof junk);
		feed_all(1, &r, 0);
		check((r.events & ICY_EV_ERROR) && S.state == ICY_DONE && out_n == 0, "an answer that is no HTTP");
	}
	{
		char t[64];
		static const char u8[] = "Bj\xC3\xB6rk \xE2\x80\x9CJ\xC3\xB3ga\xE2\x80\x9D  ";
		icy_to_mac((const unsigned char *)u8, sizeof u8 - 1, t, sizeof t);
		check(!strcmp(t, "Bj\x9Ark \xD2J\x97ga\xD3"), "UTF-8 into Mac Roman, the trailing spaces dropped");
	}
	/* the IP version from MacTCP's address: Riptide's IPv6 stand-ins are 198.18.0.0/15 */
	check(icy_ip_version(0xC6120005ul) == 6, "198.18.0.5 is an IPv6 stand-in");
	check(icy_ip_version(0xC613FFFEul) == 6, "198.19.255.254 is an IPv6 stand-in");
	check(icy_ip_version(0xC6110001ul) == 4, "198.17.0.1 is IPv4");
	check(icy_ip_version(0xC6140000ul) == 4, "198.20.0.0 is IPv4");
	check(icy_ip_version(0xC0A84401ul) == 4, "192.168.68.1 is IPv4");
	check(icy_ip_version(0x2D4F7012ul) == 4, "a public IPv4 address is IPv4");
	printf("icy_test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
