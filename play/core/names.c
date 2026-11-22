/* SPDX-License-Identifier: MPL-2.0
 *
 * play/core/names.c - a song's number and title from its file's name (names.h says the rule).
 *
 * Backstory (22 November 2026, docs/08 item 107). The macOS Play had this as a regular expression
 * (TrackName.swift); the classic Play had none and showed what the server sent. With one core both tidy a
 * name the same way, so the expression is written out below in the order its engine would try it: the disc
 * number first with two digits, then with one, then none; the track number with three digits, then two, then
 * one; the separator with a mark first, then spaces alone. The first way that leaves a title wins, which is
 * what the expression's backtracking did.
 */
#include "names.h"
#include "text.h"

static int digit(int c) { return c >= '0' && c <= '9'; }

/* the separator and the title after a number ending at k: where the title starts, or -1 */
static long after_number(const char *s, long k)
{
	long j = k;
	while (a68_space(s[j])) j++;                     /* \s* [-.)_] \s* */
	if (s[j] == '-' || s[j] == '.' || s[j] == ')' || s[j] == '_') {
		j++;
		if (s[j]) {                                  /* .+ wants one character at least, a space will do */
			while (a68_space(s[j])) j++;
			return j;
		}
	}
	if (a68_space(s[k])) {                           /* \s+ */
		j = k;
		while (a68_space(s[j])) j++;
		if (s[j]) return j;
		if (j - k > 1) return j;                     /* spaces only: the expression keeps one for the title */
	}
	return -1;
}
/* the track number at i, three digits first: its value into *num and where the title starts, or -1 */
static long number_at(const char *s, long i, unsigned long *num)
{
	int len, n = 0;
	while (n < 3 && digit(s[i + n])) n++;
	for (len = n; len >= 1; len--) {
		long t = after_number(s, i + len);
		if (t >= 0) {
			int k;
			*num = 0;
			for (k = 0; k < len; k++) *num = *num * 10 + (unsigned long)(s[i + k] - '0');
			return t;
		}
	}
	return -1;
}

unsigned long a68_track_name(const char *s, const char *artist, char *out, long max)
{
	long i = 0, t = -1, e;
	unsigned long num = 0;
	int len;
	while (a68_space(s[i])) i++;
	for (len = 2; len >= 1 && t < 0; len--) {        /* the disc: (?:\d{1,2}[-.])? */
		int k, ok = 1;
		for (k = 0; k < len; k++) ok = ok && digit(s[i + k]);
		if (ok && (s[i + len] == '-' || s[i + len] == '.')) t = number_at(s, i + len + 1, &num);
	}
	if (t < 0) t = number_at(s, i, &num);
	if (t < 0) { t = i; num = 0; }                   /* no number: the whole name, trimmed */
	e = a68_len(s);
	while (e > t && a68_space(s[e - 1])) e--;
	while (t < e && a68_space(s[t])) t++;
	if (artist && artist[0]) {                       /* "Artist - " off the front */
		long a = a68_len(artist), k;
		int ok = e - t > a + 3;
		for (k = 0; ok && k < a; k++) ok = a68_lower((unsigned char)s[t + k]) == a68_lower((unsigned char)artist[k]);
		if (ok && s[t + a] == ' ' && s[t + a + 1] == '-' && s[t + a + 2] == ' ') t += a + 3;
	}
	a68_copy_n(out, s + t, e - t, max);
	return num;
}

long a68_stem(const char *path, char *out, long max)
{
	long n = a68_len(path), a = n, e;
	while (a > 0 && path[a - 1] != '/') a--;
	e = n;
	if (a == n && n) {                               /* a path ending in '/': the part before it */
		e = n - 1;
		a = e;
		while (a > 0 && path[a - 1] != '/') a--;
	}
	{
		long dot = e;
		while (dot > a && path[dot - 1] != '.') dot--;
		if (dot > a + 1) e = dot - 1;                /* ".hidden" keeps its dot */
	}
	a68_copy_n(out, path + a, e - a, max);
	return a68_len(out);
}
