/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/libparse.c - the music server's answers to Aurora68 Play's Library, taken apart: the
 * first line of a list, its records one by one, the first line of a cover. The format is the server's
 * (aurora68_music_server.py) and was made for this: text a 68k splits with a loop, no JSON. Records are
 * read one at a time into a small line, so a page of 200 records needs no more memory than one record.
 * No Toolbox calls: tools/libparse_test.c runs it on the host. Written on 22 November 2026 (docs/08
 * item 99).
 */
#include "libparse.h"

unsigned long lp_num(const char *s)
{
	unsigned long v = 0;
	while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned long)(*s++ - '0');
	return v;
}
void lp_copy(char *d, const char *s, int max)
{
	int i = 0;
	while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
	d[i] = 0;
}
int lp_begin(struct lp *p, const char *b, long n)
{
	char line[80], *f[6];
	int k;
	p->b = b; p->n = n; p->at = 0;
	p->total = p->from = p->count = 0;
	p->kind[0] = 0;
	if (n < 5 || b[0] != 'A' || b[1] != '6' || b[2] != '8' || b[3] != 'M' || b[4] != '\t') return 0;
	k = lp_next(p, line, sizeof line, f, 6);
	if (k < 6 || lp_num(f[1]) != 1) return 0;
	lp_copy(p->kind, f[2], sizeof p->kind);
	p->total = (long)lp_num(f[3]);
	p->from = (long)lp_num(f[4]);
	p->count = (long)lp_num(f[5]);
	return 1;
}
int lp_next(struct lp *p, char *line, int max, char **f, int maxf)
{
	int o = 0, nf = 0;
	if (p->at >= p->n) return -1;
	while (p->at < p->n && p->b[p->at] != '\r' && p->b[p->at] != '\n') {
		if (o < max - 1) line[o++] = p->b[p->at];
		p->at++;
	}
	while (p->at < p->n && (p->b[p->at] == '\r' || p->b[p->at] == '\n')) p->at++;
	line[o] = 0;
	if (!o && p->at >= p->n) return -1;
	f[nf++] = line;
	for (o = 0; line[o]; o++)
		if (line[o] == '\t') {
			line[o] = 0;
			if (nf < maxf) f[nf++] = line + o + 1;
		}
	for (o = nf; o < maxf; o++) f[o] = line + max - 1;      /* fields that are not there: empty */
	line[max - 1] = 0;
	return nf;
}
int lp_art(const char *b, long n, const char *magic, long *v, int nv, long *data_at)
{
	long i = 0;
	int k;
	while (magic[i]) { if (i >= n || b[i] != magic[i]) return 0; i++; }
	if (i >= n || b[i] != '\t') return 0;
	i++;
	if (i + 1 >= n || b[i] != '1' || b[i + 1] != '\t') return 0;   /* version 1 */
	i += 2;
	for (k = 0; k < nv; k++) {
		long x = 0;
		if (i >= n || b[i] < '0' || b[i] > '9') return 0;
		while (i < n && b[i] >= '0' && b[i] <= '9') x = x * 10 + (b[i++] - '0');
		v[k] = x;
		if (i < n && b[i] == '\t') i++;
	}
	while (i < n && b[i] != '\r') i++;
	if (i >= n) return 0;
	*data_at = i + 1;
	return 1;
}
