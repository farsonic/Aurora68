/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/play_core_test.c - Aurora68 Play's shared core (play/core) on the host: `make -C mac check`.
 *
 *     play_core_test               the core's parts on answers written here as the server writes them, with
 *                                  the cases a 68000 must not trip over (a field missing, the last line without
 *                                  its Return, a line feed for a Return, an answer that is not the server's)
 *     play_core_test DIR           the same code on the real server's answers, which tools/play_core_fixtures.py
 *                                  fetched into DIR from tools/music-server on a library of ffmpeg-made tones
 *     (A68_FREESTANDING)           the units alone as a 68020 program, big-endian with 32-bit longs, under
 *                                  qemu-m68k (tools/play_core_m68k.sh): the classic Play's machine
 *
 * Every part is checked: text, track names, addresses and the probe's answer, the queue, the protocol's
 * records and requests, the catalogue's pages. The cases of tools/libparse_test.c (the classic Play's reader
 * before the core, docs/08 item 99) are here, read the new way: in place. Written on 22 November 2026 (docs/08
 * item 107).
 */
#ifdef A68_FREESTANDING                          /* as a 68020 program with no C library: tools/play_core_m68k.sh */
#include "play_core_shim.h"
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif
#include "../play/core/include/PlayCore.h"

static int fails, checks;
static void check(int ok, const char *what)
{
	checks++;
	if (!ok) { fails++; printf("FAIL %s\n", what); }
}
/* a writable copy of a literal answer: the core takes answers apart in place */
static char *dup_(const char *s, long *len)
{
	static char bufs[8][8192];
	static int k;
	char *b = bufs[k++ & 7];
	*len = (long)strlen(s);
	memcpy(b, s, (size_t)*len + 1);
	return b;
}

static void text_units(void)
{
	char b[64], *p, *end = b + sizeof b - 1;
	p = a68_put_time(b, end, 187); *p = 0;
	check(!strcmp(b, "3:07"), "a time: 3:07");
	p = a68_put_time(b, end, 3723); *p = 0;
	check(!strcmp(b, "1:02:03"), "an hour and more: 1:02:03");
	p = a68_put_time(b, end, 5); *p = 0;
	check(!strcmp(b, "0:05"), "five seconds: 0:05");
	p = a68_put_escaped(b, end, "Caf\xC3\xA9 & Co/x", 0); *p = 0;
	check(!strcmp(b, "Caf%C3%A9%20%26%20Co%2Fx"), "escaped for a query: UTF-8 bytes as %XX");
	p = a68_put_escaped(b, end, "a b/c~d", 1); *p = 0;
	check(!strcmp(b, "a%20b/c~d"), "escaped for a path: the slashes kept");
	p = a68_put_escaped(b, b + 5, "abc def", 0); *p = 0;
	check(!strcmp(b, "abc"), "never half an escape at the end");
	check(a68_num_ms("12.5") == 12500 && a68_num_ms("273") == 273000 && a68_num_ms("0.25") == 250 && a68_num_ms("") == 0,
	      "seconds with a fraction, in thousandths");
	check(!strcmp(a68_sort_name("The Cure"), "Cure") && !strcmp(a68_sort_name("the the"), "the") && !strcmp(a68_sort_name("The"), "The") &&
	      !strcmp(a68_sort_name("Theme"), "Theme"), "names as record shops file them");
	check(a68_muldiv(3600000ul, 640, 1280) == 1800000ul && a68_muldiv(4000000000ul, 3, 4) == 3000000000ul, "a * b / c without overflow");
	unsigned long s1 = 7, s2 = 7;
	check(a68_random(&s1) == a68_random(&s2) && s1 < 0x100000000ull, "the generator: 32 bits, the same from the same seed");
	a68_copy(b, "abcdef", 4);
	check(!strcmp(b, "abc"), "a bounded copy");
}

static void names_units(void)
{
	char t[100], s[100];
	unsigned long n;
	n = a68_track_name("01. Semisonic - Closing Time", "Semisonic", t, sizeof t);
	check(n == 1 && !strcmp(t, "Closing Time"), "\"01. Artist - Title\": the artist taken off");
	n = a68_track_name("01 Closing Time", 0, t, sizeof t);
	check(n == 1 && !strcmp(t, "Closing Time"), "\"01 Title\"");
	n = a68_track_name("1-01 Closing Time", 0, t, sizeof t);
	check(n == 1 && !strcmp(t, "Closing Time"), "\"1-01 Title\": disc and track");
	n = a68_track_name("2001 A Space Odyssey", 0, t, sizeof t);
	check(n == 0 && !strcmp(t, "2001 A Space Odyssey"), "a name that only starts with a number keeps it");
	n = a68_track_name("Closing Time", "Semisonic", t, sizeof t);
	check(n == 0 && !strcmp(t, "Closing Time"), "no number");
	n = a68_track_name("12 - Song", 0, t, sizeof t);
	check(n == 12 && !strcmp(t, "Song"), "\"12 - Song\"");
	n = a68_track_name("03_Track", 0, t, sizeof t);
	check(n == 3 && !strcmp(t, "Track"), "\"03_Track\"");
	n = a68_track_name("07) Seven", 0, t, sizeof t);
	check(n == 7 && !strcmp(t, "Seven"), "\"07) Seven\"");
	n = a68_track_name("05. epsilon - Other", "Epsilon", t, sizeof t);
	check(n == 5 && !strcmp(t, "Other"), "the artist in another case");
	n = a68_track_name("05. Epsilon - ", "Epsilon", t, sizeof t);
	check(n == 5 && !strcmp(t, "Epsilon -"), "an artist with no title after it stays");
	a68_stem("Gamma - Converted/01. Gamma - Closing Time.m4a", s, sizeof s);
	check(!strcmp(s, "01. Gamma - Closing Time"), "a path's stem");
	a68_stem("x/.hidden", s, sizeof s);
	check(!strcmp(s, ".hidden"), "a name that starts with a dot keeps it");
	a68_stem("Album/", s, sizeof s);
	check(!strcmp(s, "Album"), "a path ending in a slash");
}

static void address_units(void)
{
	char h[A68_HOST_MAX], path[160], u[200];
	unsigned port = 0;
	int br;
	check(a68_parse_address("192.168.1.20:8423", 8000, h, sizeof h, &port) && !strcmp(h, "192.168.1.20") && port == 8423, "host:port");
	check(a68_parse_address("192.168.1.20", 8000, h, sizeof h, &port) && port == 8000, "no port: the server's own");
	check(a68_parse_address("[fd00::20]:8423", 8000, h, sizeof h, &port) && !strcmp(h, "fd00::20") && port == 8423, "[v6]:port");
	check(a68_parse_address("fd00::20", 8000, h, sizeof h, &port) && !strcmp(h, "fd00::20") && port == 8000, "a bare IPv6 address");
	check(a68_parse_address("http://nas.local:8423/live", 8000, h, sizeof h, &port) && !strcmp(h, "nas.local") && port == 8423, "a URL");
	check(a68_parse_address("http://[2001:db8::1]:8423/", 8000, h, sizeof h, &port) && !strcmp(h, "2001:db8::1"), "a URL with IPv6");
	check(a68_parse_address("  nas  ", 8000, h, sizeof h, &port) && !strcmp(h, "nas"), "spaces around");
	check(!a68_parse_address("", 8000, h, sizeof h, &port) && !a68_parse_address("nas:99999", 8000, h, sizeof h, &port) &&
	      !a68_parse_address("[fd00::20", 8000, h, sizeof h, &port) && !a68_parse_address("my nas", 8000, h, sizeof h, &port) &&
	      !a68_parse_address("nas:", 8000, h, sizeof h, &port), "not addresses");
	check(a68_is_ipv6("2401:db8:4f02::be24:11ff:fe3f:b27e") && a68_is_ipv6("[fd00::20]") && a68_is_ipv6("fe80::1%en0") &&
	      !a68_is_ipv6("nas.local") && a68_is_ipv4("192.168.1.20") && !a68_is_ipv4("192.168.1.256") && !a68_is_ipv4("1.2.3") &&
	      !a68_is_ipv4("1.2.3.4.5"), "families");
	check(a68_url(u, sizeof u, "fd00::20", 8423, "/t/x") && !strcmp(u, "http://[fd00::20]:8423/t/x"), "a URL brackets IPv6");
	check(a68_url(u, sizeof u, "[fd00::20]", 80, 0) && !strcmp(u, "http://[fd00::20]:80"), "brackets once");
	check(a68_url(u, sizeof u, "fe80::1%en0", 8000, 0) && !strcmp(u, "http://[fe80::1%25en0]:8000"), "a zone as %25");
	check(a68_url(u, sizeof u, "192.168.1.20", 8423, "/") && !strcmp(u, "http://192.168.1.20:8423/"), "IPv4 as it is");
	{
		const char *hosts[] = { "192.168.1.20", "fd00::20", "nas.local", "192.168.1.20" };
		int o[4], n;
		n = a68_order(hosts, 4, A68_IPV6, o);
		check(n == 3 && o[0] == 1 && o[1] == 0 && o[2] == 2, "IPv6 first, then IPv4, then names, each once");
		n = a68_order(hosts, 4, A68_IPV4, o);
		check(n == 3 && o[0] == 0 && o[1] == 1 && o[2] == 2, "IPv4 first");
	}
	/* a station's address (the classic Play's radio.c, since 21 November 2026) */
	check(!a68_parse_url("http://radio.testbed:8000/stream", h, 100, &port, path, sizeof path, &br) && !strcmp(h, "radio.testbed") &&
	      port == 8000 && !strcmp(path, "/stream") && !br, "a station");
	check(!a68_parse_url("ice2.somafm.com/groovesalad-128-mp3", h, 100, &port, path, sizeof path, &br) && port == 80, "no scheme, no port");
	check(!a68_parse_url("http://[fd68::80]/stream", h, 100, &port, path, sizeof path, &br) && br && !strcmp(h, "fd68::80"), "IPv6 in brackets");
	check(!a68_parse_url("http://host", h, 100, &port, path, sizeof path, &br) && !strcmp(path, "/"), "no path: /");
	check(a68_parse_url("https://x/y", h, 100, &port, path, sizeof path, &br) != 0 && a68_parse_url("ftp://x/", h, 100, &port, path, sizeof path, &br) != 0 &&
	      a68_parse_url("http://[fd68::80/x", h, 100, &port, path, sizeof path, &br) != 0 &&
	      a68_parse_url("http://x:port/", h, 100, &port, path, sizeof path, &br) != 0 &&
	      a68_parse_url("http://x/a b", h, 100, &port, path, sizeof path, &br) != 0, "refused, in words");
	/* the probe's answer */
	{
		struct a68_disc r;
		const char *out[3];
		static const char a1[] = "AURORA68-MUSIC\t1\tHome Music\t8423\t192.168.1.20\tfd00::20\r";
		check(a68_disc_parse((const unsigned char *)a1, (long)strlen(a1), &r) && !strcmp(r.name, "Home Music") && r.port == 8423 &&
		      !strcmp(r.v4, "192.168.1.20") && !strcmp(r.v6, "fd00::20"), "the probe's answer");
		check(a68_disc_hosts(&r, "192.168.1.20", A68_IPV6, 1, out) == 2 && !strcmp(out[0], "fd00::20") && !strcmp(out[1], "192.168.1.20"),
		      "IPv6 first; the sender and the named IPv4 address the same: once");
		check(a68_disc_hosts(&r, "192.168.1.20", A68_IPV4, 1, out) == 2 && !strcmp(out[0], "192.168.1.20") && !strcmp(out[1], "fd00::20"),
		      "IPv4 first");
		check(a68_disc_hosts(&r, "10.0.0.5", A68_IPV4, 0, out) == 2 && !strcmp(out[0], "10.0.0.5") && !strcmp(out[1], "192.168.1.20"),
		      "the sender before the address the server names (its container's, perhaps); no IPv6 on a MacTCP without .IPP6");
		static const char a2[] = "AURORA68-MUSIC\t1\tCaf\x8E\t8000\t\t\r";
		check(a68_disc_parse((const unsigned char *)a2, (long)strlen(a2), &r) && !strcmp(r.name, "Caf\x8E") && !r.v4[0] && !r.v6[0] &&
		      a68_disc_hosts(&r, "10.0.0.5", A68_IPV6, 1, out) == 1 && !strcmp(out[0], "10.0.0.5"), "no addresses: where it came from");
		static const char a3[] = "AURORA68-MUSIC\t1\tNAS\t8000\tnot-an-ip\tzz::\r";
		check(a68_disc_parse((const unsigned char *)a3, (long)strlen(a3), &r) && !r.v4[0] && !r.v6[0], "addresses that are not addresses: left out");
		check(!a68_disc_parse((const unsigned char *)"HELLO\t1\tx\t8000", 15, &r) &&
		      !a68_disc_parse((const unsigned char *)"AURORA68-MUSIC\tx\tNAS\t8000", 26, &r) &&
		      !a68_disc_parse((const unsigned char *)"AURORA68-MUSIC\t1\tNAS\t0", 23, &r), "not an answer");
	}
}

struct item { int v; };
static int order_is(const struct a68_queue *q, const char *want)
{
	char got[64];
	int i;
	for (i = 0; i < q->n && i < 60; i++) got[i] = (char)('a' + ((struct item *)a68q_at(q, i))->v);
	got[i] = 0;
	return !strcmp(got, want);
}
static void queue_units(void)
{
	struct item items[30], x;
	struct a68_queue q;
	int i, restart;
	a68q_init(&q, items, sizeof items[0], 30);
	check(q.cur == -1 && a68q_peek_next(&q) == -1, "an empty queue plays nothing");
	for (i = 0; i < 4; i++) items[i].v = i;          /* a, b, c, d */
	a68q_set(&q, 4, 1);
	check(q.cur == 1 && q.feed == 1 && a68q_peek_next(&q) == 2, "from b: c follows");
	x.v = 'x' - 'a';
	check(a68q_insert(&q, a68q_next_place(&q), &x) == 2 && order_is(&q, "abxcd") && q.cur == 1, "Play Next: after the one playing");
	x.v = 'y' - 'a';
	a68q_insert(&q, -1, &x);
	check(order_is(&q, "abxcdy"), "Add: at the end");
	a68q_remove(&q, 0);
	check(order_is(&q, "bxcdy") && q.cur == 0, "a song before the one playing taken out: it stays heard");
	a68q_remove(&q, 0);
	check(order_is(&q, "xcdy") && q.cur == 0 && ((struct item *)a68q_at(&q, 0))->v == 'x' - 'a', "the playing one taken out: its follower");
	a68q_jump(&q, 3);
	a68q_remove(&q, 3);
	check(q.cur == -1, "the last one, playing, taken out: nothing plays");
	x.v = 'z' - 'a';
	check(a68q_insert(&q, -1, &x) == 3 && q.cur == 3, "added to a queue that has played: the new song plays, not the old queue again");
	/* the natural end, Next and repeat */
	a68q_jump(&q, 0);
	check(a68q_advance(&q) == 1 && a68q_advance(&q) == 2 && a68q_advance(&q) == 3 && a68q_advance(&q) == -1, "played to the end");
	q.repeat = A68_REPEAT_ALL;
	a68q_jump(&q, 3);
	check(a68q_peek_next(&q) == 0 && a68q_advance(&q) == 0, "repeat all: round again");
	q.repeat = A68_REPEAT_ONE;
	check(a68q_peek_next(&q) == 0 && a68q_advance(&q) == 0 && a68q_skip(&q) == 1, "repeat one: the same song, but Next moves on");
	q.repeat = A68_REPEAT_OFF;
	a68q_jump(&q, 3);
	check(a68q_skip(&q) == -1, "Next on the last, repeat off: nothing");
	/* Previous */
	a68q_jump(&q, 2);
	check(a68q_previous(&q, 10, &restart) == 2 && restart, "Previous ten seconds in: the start of the song");
	check(a68q_previous(&q, 1, &restart) == 1 && !restart, "Previous at its start: the one before");
	a68q_jump(&q, 0);
	check(a68q_previous(&q, 1, &restart) == 0 && restart, "Previous on the first: its start again");
	/* the classic Play's stream sent ahead */
	a68q_jump(&q, 0);
	check(a68q_feed_next(&q) == 1 && q.cur == 0 && q.feed == 1, "the next stream sent while the first is heard");
	x.v = 'n' - 'a';
	check(a68q_insert(&q, a68q_next_place(&q), &x) == 2, "Play Next goes after the one sent, which is already in the card");
	a68q_heard(&q, 1);
	check(q.cur == 1 && q.feed == 1, "the card reached it");
	/* the shuffle: the same order from the same seed; those heard and sent stay */
	{
		struct item a[30], b[30];
		struct a68_queue qa, qb;
		for (i = 0; i < 20; i++) a[i].v = b[i].v = i;
		a68q_init(&qa, a, sizeof a[0], 30); a68q_init(&qb, b, sizeof b[0], 30);
		a68q_set(&qa, 20, 3); a68q_set(&qb, 20, 3);
		a68q_feed_next(&qa); a68q_feed_next(&qb);
		a68q_shuffle_upcoming(&qa, 42); a68q_shuffle_upcoming(&qb, 42);
		int same = 1, sum = 0, moved = 0;
		for (i = 0; i < 20; i++) { same &= a[i].v == b[i].v; sum += a[i].v; moved += a[i].v != i; }
		check(same && sum == 190 && moved > 5 && a[3].v == 3 && a[4].v == 4 && a[0].v == 0, "shuffled from the same seed alike, heard and sent kept");
	}
	/* full */
	{
		struct item a[2];
		struct a68_queue qf;
		a68q_init(&qf, a, sizeof a[0], 2);
		x.v = 0;
		a68q_insert(&qf, -1, &x); a68q_insert(&qf, -1, &x);
		check(a68q_insert(&qf, -1, &x) == -1 && qf.n == 2, "a full queue takes no more");
	}
}

static void proto_units(void)
{
	struct a68_rec r;
	struct a68_head h;
	char *f[6], *b;
	long n;
	int k;
	/* libparse_test.c's cases */
	b = dup_("A68M\t1\tartists\t79\t2\t3\r"
	         "009d7bdd\tAlpha Band\t2\t5\r"
	         "64fc4a6e\tBj\x9Ark-like\t1\r"
	         "8c107591\t\t1\t4", &n);
	a68_rec_init(&r, b, n);
	check(a68_read_head(&r, &h), "the first line is the server's");
	check(!strcmp(h.kind, "artists") && h.total == 79 && h.from == 2 && h.count == 3 && a68_next_from(&h) == 5, "kind, total, from, count; the next page");
	k = a68_rec_next(&r, f, 4);
	check(k == 4 && !strcmp(f[0], "009d7bdd") && !strcmp(f[1], "Alpha Band") && a68_num(f[2]) == 2 && a68_num(f[3]) == 5, "a record's fields");
	k = a68_rec_next(&r, f, 4);
	check(k == 3 && !strcmp(f[1], "Bj\x9Ark-like") && f[3][0] == 0, "a missing field reads as empty");
	k = a68_rec_next(&r, f, 4);
	check(k == 4 && f[1][0] == 0 && a68_num(f[3]) == 4, "an empty field, and the last line without its Return");
	check(a68_rec_next(&r, f, 4) == -1, "then no more");
	b = dup_("A68M\t1\ttracks\t1\t0\t1\n" "aaaaaaaa\t1\tA very long title that is longer than any line a 68000 would copy it into, by far\tX\t200\tbbbbbbbb\n", &n);
	a68_rec_init(&r, b, n);
	check(a68_read_head(&r, &h), "line feeds are taken as Returns");
	{
		struct a68_track t;
		check(a68_next_track(&r, &t) && !strcmp(t.id, "aaaaaaaa") && strlen(t.title) > 80 && t.secs == 200 && !strcmp(t.album, "bbbbbbbb") &&
		      !t.full && !t.path[0], "a long field read whole, in place; an older record has no path");
		check(!a68_next_track(&r, &t), "one record");
	}
	b = dup_("HTTP/1.0 404", &n);
	a68_rec_init(&r, b, n);
	check(!a68_read_head(&r, &h), "an answer that is not the server's");
	b = dup_("A68M\t2\tinfo\t1\t0\t1\r", &n);
	a68_rec_init(&r, b, n);
	check(!a68_read_head(&r, &h), "a later version is not read as this one");
	{
		static const char art[] = "A68ART\t1\t64\t64\t1\t8\r\xff\x00";
		long v[5], at;
		check(a68_art(art, (long)sizeof art - 1, "A68ART", v, 4, &at) && v[0] == 64 && v[1] == 64 && v[2] == 1 && v[3] == 8 && at == 19,
		      "a cover's first line and where its bits start");
		check(!a68_art(art, (long)sizeof art - 1, "A68ARTS", v, 5, &at), "a cover is not a page of thumbnails");
		static const char arts[] = "A68ARTS\t1\t8\t32\t32\t1\t4\r\x01";
		check(a68_art(arts, (long)sizeof arts - 1, "A68ARTS", v, 5, &at) && v[0] == 8 && v[4] == 4 && arts[at] == 1, "thumbnails' first line");
		check(!a68_art("A68ART\t1\t64", 11, "A68ART", v, 4, &at), "a first line cut short");
	}
	/* the full records (enc=utf8&full=1, the macOS Play) */
	b = dup_("A68M\t1\talbums\t2\t0\t2\r"
	         "a1\tCaf\xC3\xA9 Sessions\tAlpha Band\t1\t5\tb1\tAlpha Band/Caf\xC3\xA9 Sessions\r"
	         "a2\tOne\tZed\t1\t1\r", &n);
	a68_rec_init(&r, b, n);
	a68_read_head(&r, &h);
	{
		struct a68_album a;
		check(a68_next_album(&r, &a) && a.full && !strcmp(a.artist_id, "b1") && !strcmp(a.folder, "Alpha Band/Caf\xC3\xA9 Sessions") && a.secs == 5,
		      "an album with its artist's id and folder");
		check(a68_next_album(&r, &a) && !a.full && !a.folder[0] && !a.artist_id[0], "an older server's album: no folder");
	}
	b = dup_("ok\t7\r", &n);
	check(!strcmp(a68_error_text(b, n), "ok"), "a plain answer's first words");
	b = dup_("A68M\t1\terror\t1\t0\t1\rThere is no such album in the library now.\r", &n);
	check(!strcmp(a68_error_text(b, n), "There is no such album in the library now."), "an error's words");
	/* devices, commands, Live */
	b = dup_("live\tLive\tlive\t1\tA Song\tAn Artist\t1\t12.5\t200.0\t100\tAlbum\tAlbum Folder\t1795000000\t1795000001\t1\t3\t2\r"
	         "mac-0123456789ab\tFran\x8Dois's SE\tclassic\t1\t\t\r"
	         "macos-1\tStudio Mac\tmacos\t0\r", &n);
	a68_rec_init(&r, b, n);
	{
		struct a68_device d;
		check(a68_next_device(&r, &d) && d.full && d.is_live && d.playing && d.position_ms == 12500 && d.length_ms == 200000 &&
		      d.at == 1795000000ul && !strcmp(d.folder, "Album Folder") && d.listeners == 2 && !strcmp(d.version, "3"), "Live, its whole state");
		check(a68_next_device(&r, &d) && !d.full && !strcmp(d.kind, "classic") && !d.title[0] && d.volume == 100,
		      "a player, short record");
		check(a68_next_device(&r, &d) && !d.online && !strcmp(d.name, "Studio Mac"), "off line, fields missing");
		check(!a68_next_device(&r, &d), "then no more");
	}
	b = dup_("3\tplay\t0\t\t1\r"
	         "T\t1a2b3c4d\tFirst Song\tAn Artist\t212\tAlbum/01 First Song.flac\tAlbum\r"
	         "T\t5e6f7a8b\tSecond \xD2Song\xD3\tAn Artist\t180\r"
	         "4\tvolume\t0\t55\r"
	         "5\tplay\t0\t\r"
	         "R\tGroove Salad\thttp://ice1.somafm.com/groovesalad-128-mp3\r"
	         "6\tseek\t95\t\r"
	         "7\tdance\t0\t\r", &n);
	a68_rec_init(&r, b, n);
	{
		struct a68_cmd c;
		check(a68_next_cmd(&r, &c) && c.kind == 'C' && c.n == 3 && a68_action(c.action) == A68_ACT_PLAY && c.volume == -1 && c.shuffle, "play, shuffled");
		check(a68_next_cmd(&r, &c) && c.kind == 'T' && !strcmp(c.tid, "1a2b3c4d") && c.secs == 212 && !strcmp(c.path, "Album/01 First Song.flac") &&
		      !strcmp(c.album, "Album"), "a track with its path and album");
		check(a68_next_cmd(&r, &c) && c.kind == 'T' && !strcmp(c.title, "Second \xD2Song\xD3") && !c.path[0], "Mac Roman kept; no path");
		check(a68_next_cmd(&r, &c) && a68_action(c.action) == A68_ACT_VOLUME && c.volume == 55 && !c.shuffle, "volume 55");
		check(a68_next_cmd(&r, &c) && c.n == 5 && a68_next_cmd(&r, &c) && c.kind == 'R' && !strncmp(c.url, "http://ice1.", 12), "a station");
		check(a68_next_cmd(&r, &c) && a68_action(c.action) == A68_ACT_SEEK && c.position == 95, "seek to 95 s");
		check(a68_next_cmd(&r, &c) && a68_action(c.action) == A68_ACT_UNKNOWN && !a68_next_cmd(&r, &c), "a later version's action, then no more");
	}
	b = dup_("A68M\t1\tnow\t2\t0\t5\rN\tTest Music\t1\t0\t1\t0\t12\t1\rC\tA/01 x.mp3\tA\t01 x\t3.5\t180.0\rQ\tA/01 x.mp3\t01 x\rQ\tA/02 y.mp3\t02 y\rL\t192.168.1.30\t1795000000\r", &n);
	a68_rec_init(&r, b, n);
	{
		struct a68_now o;
		int q = 0;
		check(a68_read_head(&r, &h) && !strcmp(h.kind, "now") && h.total == 2, "Live: its head");
		check(a68_next_now(&r, &o) && o.kind == 'N' && o.playing && !o.paused && o.repeat && o.changes == 12 && o.listeners == 1, "Live: playing, repeat");
		check(a68_next_now(&r, &o) && o.kind == 'C' && o.elapsed_ms == 3500 && o.length_ms == 180000 && !strcmp(o.folder, "A"), "the song heard");
		while (a68_next_now(&r, &o) && o.kind == 'Q') q++;
		check(q == 2 && o.kind == 'L' && o.since == 1795000000ul, "the queue and a listener");
	}
	b = dup_("SomaFM\tGroove Salad\thttp://ice2.somafm.com/groovesalad-128-mp3\rBy country/Austria\t\x85" "1\thttp://ors-sn05.ors-shoutcast.at/oe1-q2a\rbad line\r", &n);
	a68_rec_init(&r, b, n);
	{
		struct a68_radio s;
		check(a68_next_radio(&r, &s) && !strcmp(s.group, "SomaFM") && a68_next_radio(&r, &s) && !strcmp(s.group, "By country/Austria") &&
		      !a68_next_radio(&r, &s), "radio stations, a line without an address passed over");
	}
	/* the requests: the classic Play's as they were (no enc, no full), the macOS Play's with both */
	{
		struct a68_client cl = { 0, 0 }, mac = { 1, 1 };
		char p[400];
		check(a68_path_info(&cl, p, sizeof p) && !strcmp(p, "/mac/info"), "info");
		check(a68_path_artists(&cl, 0, 200, p, sizeof p) && !strcmp(p, "/mac/artists?count=200"), "artists");
		check(a68_path_albums(&cl, "1a2b3c4d", 200, 200, p, sizeof p) && !strcmp(p, "/mac/albums?artist=1a2b3c4d&count=200&from=200"), "albums, a page on");
		check(a68_path_tracks(&cl, "", "9f9f9f9f", 0, 250, p, sizeof p) && !strcmp(p, "/mac/tracks?artist=9f9f9f9f&count=250"), "an artist's tracks");
		check(a68_path_search(&cl, "cr\x8Fme", 0, 200, p, sizeof p) && !strcmp(p, "/mac/search?q=cr%8Fme&count=200"), "a search in Mac Roman");
		check(a68_path_art(&cl, "a1", 64, 8, p, sizeof p) && !strcmp(p, "/mac/art?w=64&h=64&album=a1&depth=8"), "a cover");
		check(a68_path_albums(&mac, 0, 0, 500, p, sizeof p) && !strcmp(p, "/mac/albums?count=500&enc=utf8&full=1"), "every album, UTF-8, full");
		check(a68_path_command(&mac, "live", A68_ACT_PLAY, "f:Caf\xC3\xA9", 1, -1, -1, -1, -1, p, sizeof p) &&
		      !strcmp(p, "/mac/command?to=live&action=play&what=f%3ACaf%C3%A9&shuffle=1&enc=utf8&full=1"), "a command to Live");
		check(a68_path_command(&mac, "mac-1", A68_ACT_SEEK, 0, 0, 12345, -1, -1, -1, p, sizeof p) &&
		      !strcmp(p, "/mac/command?to=mac-1&action=seek&position=12.345&enc=utf8&full=1"), "a seek, to the thousandth");
		check(a68_path_command(&mac, "live", A68_ACT_REPEAT, 0, 0, -1, -1, -1, 0, p, sizeof p) && strstr(p, "action=repeat&on=0"), "repeat off");
		check(a68_path_command(&cl, "live", A68_ACT_JUMP, 0, 0, -1, -1, 4, -1, p, sizeof p) && !strcmp(p, "/mac/command?to=live&action=jump&index=4"), "jump");
		check(a68_path_stream("1a2b3c4d", 12, p, sizeof p) && !strcmp(p, "/t/1a2b3c4d?start=12"), "a track from 12 s");
		check(!a68_path_search(&cl, "a very long search that will not fit", 0, 200, p, 20) && !p[0], "a path that does not fit: none");
		check(a68_what_folder("", p, sizeof p) && !strcmp(p, "f:") && a68_what_radio(3, p, sizeof p) && !strcmp(p, "r:3"), "what: everything, a station");
	}
	{
		struct a68_gen w = { 0 };
		check(!a68_gen_changed(&w, 7) && !a68_gen_changed(&w, 7) && a68_gen_changed(&w, 9) && !a68_gen_changed(&w, 0) && w.seen == 9,
		      "the generation: a change after the first one known");
	}
	check(a68_bar_time(50, 200, 240000) == 60000 && a68_bar_time(-5, 200, 240000) == 0 && a68_bar_time(500, 200, 240000) == 240000 &&
	      a68_bar_time(10, 0, 100) == 0, "a click on the bar as a time");
	check(a68_estimate_ms(10000, 200000, 1, 1000, 1005) == 15000 && a68_estimate_ms(10000, 12000, 1, 1000, 1005) == 12000 &&
	      a68_estimate_ms(10000, 0, 0, 1000, 1005) == 10000, "another player's song, moved on while it plays");
}

static void catalog_units(void)
{
	static struct a68_row rows[8];
	struct a68_list l;
	char p[200], *b;
	long n;
	a68_list_init(&l, rows, 8, A68_V_ARTISTS);
	b = dup_("A68M\t1\tartists\t12\t0\t3\r009d7bdd\tAlpha Band\t2\t5\r11111111\tThe Cure\t3\t30\r22222222\tKite\t1\t1\r", &n);
	check(a68_list_load(&l, b, n) && l.n == 3 && l.total == 12 && l.rows[1].kind == 'A' && l.rows[1].count == 3, "artists as rows");
	check(a68_list_has_next(&l) && !a68_list_has_prev(&l) && a68_list_lines(&l) == 4 && a68_list_line_row(&l, 3) == -2, "a next page line");
	check(a68_list_typed(&l, "c", 1) == 1 && a68_list_typed(&l, "k", 1) == 2 && a68_list_typed(&l, "th", 2) == 1 && a68_list_typed(&l, "z", 1) == -1,
	      "typing selects: \"c\" finds The Cure");
	check(a68_list_find(&l, "22222222") == 2 && a68_list_find(&l, "33333333") == -1, "a row found again by its id");
	a68_list_init(&l, rows, 2, A68_V_TRACKS);
	b = dup_("A68M\t1\ttracks\t3\t200\t3\rt1\t1\tOne\tX\t60\ta1\rt2\t2\tTwo\tX\t70\ta1\rt3\t3\tThree\tX\t80\ta1\r", &n);
	check(a68_list_load(&l, b, n) && l.n == 2 && a68_list_secs(&l) == 130 && a68_list_has_prev(&l) && a68_list_line_row(&l, 0) == -1 &&
	      a68_list_row_line(&l, 0) == 1, "a page on, cut at the rows there is room for");
	check(a68_list_path(0, A68_V_TRACKS, "a1", 0, 200, p, sizeof p) && !strcmp(p, "/mac/tracks?album=a1&count=200&from=200"), "the view's request");
	b = dup_("A68M\t1\terror\t1\t0\t1\rThere is no such album\r", &n);
	a68_list_init(&l, rows, 8, A68_V_SEARCH);
	b = dup_("A68M\t1\tsearch\t3\t0\t3\rA\tz1\tZed\t2 albums\rL\tl1\tOne\tZed\rT\tt1\tSong\tZed\tl1\t61\r", &n);
	check(a68_list_load(&l, b, n) && l.n == 3 && l.rows[0].kind == 'A' && !l.rows[0].album[0] && l.rows[2].secs == 61 && !strcmp(l.rows[2].album, "l1"),
	      "a search's rows of three kinds");
}

static void devices_units(void)
{
	struct dv d;
	char p[400], b[64];
	dv_init(&d, "mac-0123456789ab", "Caf\xC3\xA9 Mac", "1.1", "macos");
	d.client.utf8 = 1; d.client.full = 1; d.wait = 20; d.refresh = 5;
	dv_start(&d, 0);
	dv_path(&d, DV_ANNOUNCE, p, sizeof p);
	check(!strcmp(p, "/mac/announce?id=mac-0123456789ab&name=Caf%C3%A9%20Mac&kind=macos&v=1.1&enc=utf8&full=1"), "the macOS Play's announce");
	strcpy(b, "ok\r20\r4\r");
	dv_answer(&d, DV_ANNOUNCE, 200, b, (long)strlen(b), 0);
	struct dv_state s = { 1, 10, 200, 80, "t1", "", "" };
	dv_set_state(&d, &s, 0);
	dv_path(&d, DV_STATE, p, sizeof p);
	strcpy(b, "ok\r");
	dv_answer(&d, DV_STATE, 200, b, 3, 0);
	s.pos = 14; dv_set_state(&d, &s, 4);
	check(!d.state_dirty, "four seconds on, playing: no report yet");
	s.pos = 15; dv_set_state(&d, &s, 5);
	check(d.state_dirty, "five seconds on: the macOS Play's report while playing");
	dv_path(&d, DV_COMMANDS, p, sizeof p);
	check(!strcmp(p, "/mac/commands?id=mac-0123456789ab&wait=20&enc=utf8&full=1"), "its long poll: 20 seconds");
	check(dv_valid_id("mac-0123456789ab") && !dv_valid_id("live") && !dv_valid_id("") && !dv_valid_id("a b") && !dv_valid_id("caf\xC3\xA9"),
	      "ids the server takes");
	{
		char a[17], c[17];
		dv_make_id(a, 0xDEADBEEFul, 12345);
		dv_make_id(c, 0xDEADBEEFul, 12345);
		check(!strcmp(a, c) && dv_valid_id(a) && !strcmp(a, "mac-h3l4n7gl9elu"), "an id: the same on a 68000 and on a Mac of today");
	}
}

#ifndef A68_FREESTANDING
/* ---- the real server's answers (tools/play_core_fixtures.py) ---------------------------------------------- */
static char *load(const char *dir, const char *name, long *n)
{
	static char bufs[24][1 << 16];
	static int k;
	char path[600];
	FILE *fp;
	char *b = bufs[k++ % 24];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	fp = fopen(path, "rb");
	if (!fp) { printf("FAIL no fixture %s\n", path); fails++; *n = 0; b[0] = 0; return b; }
	*n = (long)fread(b, 1, sizeof bufs[0] - 1, fp);
	fclose(fp);
	b[*n] = 0;
	return b;
}
static void fixtures(const char *dir)
{
	struct a68_rec r;
	struct a68_head h;
	long n;
	char *b;
	char say[300];
	/* info */
	b = load(dir, "info.u8", &n);
	a68_rec_init(&r, b, n);
	{
		struct a68_info i;
		check(a68_read_head(&r, &h) && !strcmp(h.kind, "info") && a68_info_of(&r, &i) && !strcmp(i.name, "Core Test") && i.tracks >= 10 &&
		      i.generation >= 1 && i.every == 20, "server: /mac/info, with how often to announce (full)");
	}
	b = load(dir, "info.mr", &n);
	a68_rec_init(&r, b, n);
	{
		struct a68_info i;
		check(a68_read_head(&r, &h) && a68_info_of(&r, &i) && i.every == 0, "server: /mac/info as the classic Play reads it: as before");
	}
	/* artists: the whole list, and the same in pages of two */
	{
		static char names[64][80];
		int na = 0, np = 0, same = 1, page;
		b = load(dir, "artists.u8", &n);
		a68_rec_init(&r, b, n);
		struct a68_artist a;
		check(a68_read_head(&r, &h) && !strcmp(h.kind, "artists") && h.total >= 5, "server: /mac/artists");
		while (a68_next_artist(&r, &a) && na < 64) snprintf(names[na++], 80, "%s", a.name);
		int apes = -1, kite = -1;
		for (int i = 0; i < na; i++) { if (!strcmp(names[i], "The Apes")) apes = i; if (!strcmp(names[i], "Kite")) kite = i; }
		check(na == h.total && apes >= 0 && kite > apes, "server: every artist, \"The Apes\" filed under A");
		for (page = 0; page < 20; page++) {
			char fn[40];
			snprintf(fn, sizeof fn, "artists-p%d.u8", page);
			b = load(dir, fn, &n);
			if (!n) { fails--; break; }
			a68_rec_init(&r, b, n);
			if (!a68_read_head(&r, &h)) break;
			while (a68_next_artist(&r, &a)) { same &= np < na && !strcmp(names[np], a.name); np++; }
			if (a68_next_from(&h) < 0) break;
		}
		snprintf(say, sizeof say, "server: the artists in pages of two, %d pages, the same %d artists (a68_next_from)", page + 1, np);
		check(same && np == na, say);
	}
	/* albums, full: an artist id and a folder each; UTF-8 */
	{
		struct a68_album a;
		int cafe = 0, all_full = 1, k = 0;
		b = load(dir, "albums.u8", &n);
		a68_rec_init(&r, b, n);
		check(a68_read_head(&r, &h) && !strcmp(h.kind, "albums"), "server: /mac/albums, every album");
		while (a68_next_album(&r, &a)) {
			k++;
			all_full &= a.full && a.folder[0] && a.artist_id[0];
			if (!strcmp(a.title, "Caf\xC3\xA9 Sessions") && !strcmp(a.folder, "Alpha Band/Caf\xC3\xA9 Sessions")) cafe = 1;
		}
		check(k == h.total && all_full && cafe, "server: full albums, with folder and artist id, in UTF-8 (enc=utf8)");
		b = load(dir, "albums-alpha.mr", &n);
		a68_rec_init(&r, b, n);
		cafe = 0;
		a68_read_head(&r, &h);
		while (a68_next_album(&r, &a)) if (!strcmp(a.title, "Caf\x8E Sessions") && !a.full) cafe = 1;
		check(cafe, "server: an artist's albums in Mac Roman, short records, as the classic Play has them");
	}
	/* tracks, full: the path, the album's title and folder; names tidied alike */
	{
		struct a68_track t;
		int k = 0, ok = 1;
		b = load(dir, "tracks.u8", &n);
		a68_rec_init(&r, b, n);
		check(a68_read_head(&r, &h) && !strcmp(h.kind, "tracks") && h.total == 3, "server: /mac/tracks of an album");
		while (a68_next_track(&r, &t)) {
			char stem[200], title[200];
			unsigned long num;
			k++;
			a68_stem(t.path, stem, sizeof stem);
			num = a68_track_name(stem, "Gamma", title, sizeof title);
			ok &= t.full && !strcmp(t.folder, "Gamma - Converted") && !strcmp(t.album_title, "Converted") && num == t.number &&
			      !strcmp(title, t.title) && !strncmp(t.path, "Gamma - Converted/", 18);
		}
		check(k == 3 && ok, "server: full tracks; the core's tidied file names agree with the tags (01. Gamma - Closing Time)");
		b = load(dir, "tracks.mr", &n);
		{
			static struct a68_row rows[A68_PAGE];
			struct a68_list l;
			a68_list_init(&l, rows, A68_PAGE, A68_V_TRACKS);
			check(a68_list_load(&l, b, n) && l.n == 3 && rows[0].num == 1 && !strcmp(rows[0].text, "Closing Time") && rows[0].secs == 3 &&
			      a68_list_secs(&l) == 7, "server: the classic Play's page of an album's tracks, as rows");
		}
	}
	/* a search (Mac Roman), an error, the radio list */
	{
		static struct a68_row rows[A68_PAGE];
		struct a68_list l;
		int i, found = 0;
		b = load(dir, "search.mr", &n);
		a68_list_init(&l, rows, A68_PAGE, A68_V_SEARCH);
		check(a68_list_load(&l, b, n), "server: a search's answer");
		for (i = 0; i < l.n; i++) found |= rows[i].kind == 'T' && !strncmp(rows[i].text, "Cr\x8Fme", 5);
		check(found, "server: a search in Mac Roman finds the track");
		b = load(dir, "error.mr", &n);
		check(strstr(a68_error_text(b, n), "no such album") != 0, "server: an error, in words");
		struct a68_radio s;
		int k = 0, austria = 0;
		b = load(dir, "radio.u8", &n);
		a68_rec_init(&r, b, n);
		while (a68_next_radio(&r, &s)) { k++; austria |= !strcmp(s.group, "By country/Austria") && !strcmp(s.name, "\xC3\x96" "1"); }
		check(k >= 40 && austria, "server: /mac/radio in UTF-8: the built-in list, \xC3\x96" "1 under By country/Austria");
		b = load(dir, "radio.mr", &n);
		a68_rec_init(&r, b, n);
		austria = 0;
		while (a68_next_radio(&r, &s)) austria |= !strcmp(s.name, "\x85" "1");
		check(austria, "server: /mac/radio in Mac Roman");
	}
	/* devices: this test's player announced and reporting, Live first */
	{
		struct a68_device d;
		int live = 0, me = 0;
		b = load(dir, "devices.u8", &n);
		a68_rec_init(&r, b, n);
		while (a68_next_device(&r, &d)) {
			if (d.is_live && d.full) live = 1;
			if (!strcmp(d.id, "mac-fixture0001") && d.online && d.full && !strcmp(d.kind, "macos") && !strcmp(d.name, "Caf\xC3\xA9 Mac") &&
			    d.playing && !strcmp(d.title, "Closing Time") && !strcmp(d.album, "Converted") && d.volume == 70 && d.at > 0)
				me = 1;
		}
		check(live && me, "server: /mac/devices, full: Live and this player with its state named from the catalogue");
	}
	/* the commands this player was sent: a whole album, then a volume */
	{
		struct a68_cmd c;
		int t = 0, paths = 1, play = 0, vol = 0;
		b = load(dir, "commands.u8", &n);
		a68_rec_init(&r, b, n);
		while (a68_next_cmd(&r, &c)) {
			if (c.kind == 'C' && a68_action(c.action) == A68_ACT_PLAY) play = 1;
			if (c.kind == 'C' && a68_action(c.action) == A68_ACT_VOLUME && c.volume == 40) vol = 1;
			if (c.kind == 'T') { t++; paths &= !strncmp(c.path, "Gamma - Converted/", 18) && !strcmp(c.album, "Converted"); }
		}
		check(play && vol && t == 3 && paths, "server: /mac/commands, full: play an album (its three tracks with paths), volume 40");
		b = load(dir, "commands.mr", &n);
		a68_rec_init(&r, b, n);
		t = 0; paths = 1;
		while (a68_next_cmd(&r, &c)) if (c.kind == 'T') { t++; paths &= !c.path[0]; }
		check(t == 3 && paths, "server: the classic Play's commands, short lines as before");
	}
	/* Live */
	{
		struct a68_now o;
		int q = 0, nline = 0;
		b = load(dir, "now.u8", &n);
		a68_rec_init(&r, b, n);
		check(a68_read_head(&r, &h) && !strcmp(h.kind, "now") && h.total == 3, "server: /mac/now: the Live channel's queue of three");
		while (a68_next_now(&r, &o)) { if (o.kind == 'N' && o.repeat) nline = 1; if (o.kind == 'Q') q++; }
		check(nline && q == 3, "server: Live, repeat on (set by /mac/command), the queue's songs");
	}
	/* the probe */
	{
		struct a68_disc d;
		const char *out[3];
		b = load(dir, "probe.bin", &n);
		check(a68_disc_parse((const unsigned char *)b, n, &d) && !strcmp(d.name, "Core Test") && d.port > 0 &&
		      a68_disc_hosts(&d, "127.0.0.1", A68_IPV4, 0, out) >= 1 && !strcmp(out[0], "127.0.0.1"), "server: the probe's answer");
	}
	/* the commands' answers */
	b = load(dir, "command-ok.u8", &n);
	check(!strncmp(a68_error_text(b, n), "ok", 2), "server: /mac/command answered ok");
	b = load(dir, "command-refused.u8", &n);
	check(strstr(a68_error_text(b, n), "radio") != 0, "server: /mac/command refused in words (radio on Live)");
}

#endif

int main(int argc, char **argv)
{
	text_units();
	names_units();
	address_units();
	queue_units();
	proto_units();
	catalog_units();
	devices_units();
#ifndef A68_FREESTANDING
	if (argc > 1) fixtures(argv[1]);
#endif
	printf("play_core_test: %d checks, %d failed%s\n", checks, fails, argc > 1 ? " (with the server's answers)" : "");
	return fails != 0;
}
