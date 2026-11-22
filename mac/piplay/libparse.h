/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/libparse.h - the music server's answers to Aurora68 Play's Library, taken apart
 * (libparse.c). No Toolbox calls and no allocation: tools/libparse_test.c runs it on the host.
 *
 * The server's /mac/ answers (tools/music-server/aurora68_music_server.py, "the library for Aurora68
 * Play's Library window"): Mac Roman text, one record a line ended by a Return, fields separated by tabs,
 * a first line "A68M <tab> 1 <tab> KIND <tab> TOTAL <tab> FROM <tab> COUNT". Covers: a line "A68ART 1 W H
 * DEPTH ROWBYTES" (or "A68ARTS 1 N W H DEPTH ROWBYTES", then N times a byte and a bitmap) and the bits.
 * Written on 22 November 2026 (docs/08 item 99).
 */
#ifndef PIPLAY_LIBPARSE_H
#define PIPLAY_LIBPARSE_H

struct lp {
	const char *b;                   /* the answer's body */
	long n, at;                      /* its length; where the next record starts */
	long total, from, count;         /* from the first line */
	char kind[12];
};
/* the first line: 1 if it is the music server's, 0 if not */
int lp_begin(struct lp *p, const char *b, long n);
/* the next record's fields, copied into line (max bytes) and split at its tabs: f[0] .. f[n-1], each
   a C string; the number of fields, or -1 when there are no more records */
int lp_next(struct lp *p, char *line, int max, char **f, int maxf);
unsigned long lp_num(const char *s);
void lp_copy(char *d, const char *s, int max);
/* a cover's first line ("A68ART" or "A68ARTS"): its numbers after the version into v (nv of them), and
   where the bits start; 1 if it is one */
int lp_art(const char *b, long n, const char *magic, long *v, int nv, long *data_at);
#endif
