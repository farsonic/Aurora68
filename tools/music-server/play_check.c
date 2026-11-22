/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/music-server/play_check.c - Aurora68 Play's own parsers on the host, for test_server.py.
 *
 *     play_check icy AUDIO_OUT < response      the ICY parser (mac/piplay/icy.c), fed the server's raw
 *                                              answer in pieces of random sizes as TCPRcv hands them over;
 *                                              the audio to AUDIO_OUT, one line per new song title
 *     play_check stations < file               the stations parser (mac/piplay/stations.c): one line per
 *                                              group and station as Aurora68 Play's Radio menu will hold them
 *
 * Written on 22 November 2026, with the music server.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../mac/piplay/icy.h"
#include "../../mac/piplay/stations.h"

static FILE *audio;
static void sink(const unsigned char *p, unsigned long n) { fwrite(p, 1, n, audio); }

static unsigned char buf[64 << 20];

int main(int argc, char **argv)
{
	size_t n = fread(buf, 1, sizeof buf, stdin);
	if (argc == 3 && !strcmp(argv[1], "icy")) {
		static struct icy s;
		unsigned long seen = 0, at = 0, seed = 22;
		audio = fopen(argv[2], "wb");
		if (!audio) return 2;
		icy_init(&s);
		while (at < n) {
			unsigned long k;
			seed = seed * 1103515245u + 12345u;
			k = 1 + (seed >> 16) % 3000;     /* TCPRcv's pieces: anything up to a few KB */
			if (k > n - at) k = n - at;
			unsigned ev = icy_feed(&s, buf + at, k, sink);
			at += k;
			if (ev & ICY_EV_ERROR) { printf("error\n"); return 1; }
			if (s.title_seq != seen) { seen = s.title_seq; printf("title %s\n", s.title); }
		}
		printf("status %d kind %d metaint %lu name %s audio %lu\n", s.status, s.kind, s.metaint, s.name, s.audio_bytes);
		fclose(audio);
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "stations")) {
		static struct stations l;
		int i;
		stations_parse(&l, (const char *)buf, (long)n);
		for (i = 0; i < l.ngroups; i++)
			printf("group %d %s parent %d\n", i, l.group[i].name, l.group[i].parent);
		for (i = 0; i < l.nstations; i++)
			printf("station %s|%s|%d\n", l.station[i].name, l.station[i].url, l.station[i].group);
		printf("total %d %d\n", l.nstations, l.ngroups);
		return 0;
	}
	fprintf(stderr, "play_check icy AUDIO_OUT < response | play_check stations < file\n");
	return 2;
}
