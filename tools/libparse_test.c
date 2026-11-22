/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/libparse_test.c - Aurora68 Play's reading of the music server's answers (mac/piplay/libparse.c) on
 * the host: `make -C mac check`. The answers are written here as the server writes them
 * (tools/music-server/aurora68_music_server.py, mac_answer and the covers), with the cases a 68k program
 * must not trip over: a record with fewer fields than asked for, an empty field, a field longer than its
 * line, the last line without its Return, a line feed where a Return should be, an answer that is not the
 * server's. Written on 22 November 2026.
 */
#include <stdio.h>
#include <string.h>
#include "../mac/piplay/libparse.h"

static int fails, checks;
static void check(int ok, const char *what)
{
	checks++;
	if (!ok) { fails++; printf("FAIL %s\n", what); }
}

int main(void)
{
	struct lp l;
	char line[64], *f[6];
	int n;
	static const char artists[] = "A68M\t1\tartists\t79\t2\t3\r"
	                              "009d7bdd\tAlpha Band\t2\t5\r"
	                              "64fc4a6e\tBj\x9Ark-like\t1\r"       /* Mac Roman o umlaut; a field missing */
	                              "8c107591\t\t1\t4";                   /* an empty name; no Return at the end */
	check(lp_begin(&l, artists, (long)strlen(artists)), "the first line is the server's");
	check(!strcmp(l.kind, "artists") && l.total == 79 && l.from == 2 && l.count == 3, "kind, total, from, count");
	n = lp_next(&l, line, sizeof line, f, 4);
	check(n == 4 && !strcmp(f[0], "009d7bdd") && !strcmp(f[1], "Alpha Band") && lp_num(f[2]) == 2 && lp_num(f[3]) == 5, "a record's fields");
	n = lp_next(&l, line, sizeof line, f, 4);
	check(n == 3 && !strcmp(f[1], "Bj\x9Ark-like") && f[3][0] == 0, "a missing field reads as empty");
	n = lp_next(&l, line, sizeof line, f, 4);
	check(n == 4 && f[1][0] == 0 && lp_num(f[3]) == 4, "an empty field, and the last line without its Return");
	check(lp_next(&l, line, sizeof line, f, 4) == -1, "then no more");

	static const char lf[] = "A68M\t1\ttracks\t1\t0\t1\n" "aaaaaaaa\t1\tA very long title that is longer than the line it is read into, by far\tX\t200\tbbbbbbbb\n";
	check(lp_begin(&l, lf, (long)strlen(lf)), "line feeds are taken as Returns");
	n = lp_next(&l, line, 32, f, 6);
	check(n >= 3 && !strcmp(f[0], "aaaaaaaa") && strlen(f[2]) < 32, "a long field is cut at the line's end, not beyond it");
	check(lp_next(&l, line, sizeof line, f, 6) == -1, "one record");

	check(!lp_begin(&l, "HTTP/1.0 404", 12), "an answer that is not the server's");
	check(!lp_begin(&l, "A68M\t2\tinfo\t1\t0\t1\r", 18), "a later version is not read as this one");

	static const char art[] = "A68ART\t1\t64\t64\t1\t8\r\xff\x00";
	long v[5], at;
	check(lp_art(art, (long)sizeof art - 1, "A68ART", v, 4, &at) && v[0] == 64 && v[1] == 64 && v[2] == 1 && v[3] == 8 && at == 19,
	      "a cover's first line and where its bits start");
	check(!lp_art(art, (long)sizeof art - 1, "A68ARTS", v, 5, &at), "a cover is not a page of thumbnails");
	static const char arts[] = "A68ARTS\t1\t8\t32\t32\t1\t4\r\x01";
	check(lp_art(arts, (long)sizeof arts - 1, "A68ARTS", v, 5, &at) && v[0] == 8 && v[4] == 4 && arts[at] == 1, "thumbnails' first line");
	check(!lp_art("A68ART\t1\t64", 11, "A68ART", v, 4, &at), "a first line cut short");
	printf("libparse_test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
