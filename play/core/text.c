/* SPDX-License-Identifier: MPL-2.0
 *
 * play/core/text.c - the core's text routines (text.h says what and why).
 *
 * Backstory (22 November 2026, docs/08 item 107). Before the core, the classic Play had a put_str, a
 * put_num, a copy and a lower in nearly every file (piplay.c, radio.c, fetch.c, library.c, devices.c), each a
 * little different, and the macOS Play had Swift's own. These are the ones both now use. They take an end
 * pointer, not a count, because building a request is a run of appends and an end stays the same while a
 * count would have to be carried along.
 */
#include "text.h"

long a68_len(const char *s)
{
	long n = 0;
	if (!s) return 0;
	while (s[n]) n++;
	return n;
}
void a68_copy(char *d, const char *s, long max)
{
	long i = 0;
	if (max <= 0) return;
	while (s && s[i] && i < max - 1) { d[i] = s[i]; i++; }
	d[i] = 0;
}
void a68_copy_n(char *d, const char *s, long n, long max)
{
	long i = 0;
	if (max <= 0) return;
	while (i < n && i < max - 1 && s[i]) { d[i] = s[i]; i++; }
	d[i] = 0;
}
char *a68_put(char *p, char *end, const char *s)
{
	while (s && *s && p < end) *p++ = *s++;
	return p;
}
char *a68_put_num(char *p, char *end, unsigned long v)
{
	char t[12];
	int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
	while (n && p < end) *p++ = t[--n];
	return p;
}
char *a68_put_time(char *p, char *end, unsigned long s)
{
	if (s >= 3600) {
		p = a68_put_num(p, end, s / 3600);
		if (p < end) *p++ = ':';
		if (p < end) *p++ = (char)('0' + s / 600 % 6);
		if (p < end) *p++ = (char)('0' + s / 60 % 10);
	} else p = a68_put_num(p, end, s / 60);
	if (p < end) *p++ = ':';
	if (p < end) *p++ = (char)('0' + s % 60 / 10);
	if (p < end) *p++ = (char)('0' + s % 10);
	return p;
}
char *a68_put_escaped(char *p, char *end, const char *s, int keep_slash)
{
	static const char hx[] = "0123456789ABCDEF";
	for (; s && *s; s++) {
		unsigned char c = (unsigned char)*s;
		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' ||
		    c == '~' || (keep_slash && c == '/')) {
			if (p >= end) break;
			*p++ = (char)c;
		} else {
			if (end - p < 3) break;          /* never half an escape */
			*p++ = '%'; *p++ = hx[c >> 4]; *p++ = hx[c & 15];
		}
	}
	return p;
}

unsigned long a68_num(const char *s)
{
	unsigned long v = 0;
	if (!s) return 0;
	while (*s == ' ' || *s == '\t') s++;
	while (*s >= '0' && *s <= '9' && v < 400000000ul) v = v * 10 + (unsigned long)(*s++ - '0');
	return v;
}
unsigned long a68_num_ms(const char *s)
{
	unsigned long v = a68_num(s) * 1000ul, f = 100;
	if (!s) return 0;
	while (*s == ' ' || *s == '\t') s++;
	while (*s >= '0' && *s <= '9') s++;
	if (*s == '.')
		for (s++; *s >= '0' && *s <= '9'; s++) { v += (unsigned long)(*s - '0') * f; f /= 10; }
	return v;
}
int a68_is_number(const char *s)
{
	if (!s || !*s) return 0;
	for (; *s; s++)
		if (*s < '0' || *s > '9') return 0;
	return 1;
}

int a68_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int a68_space(int c) { return c == ' ' || c == '\t'; }
int a68_same(const char *a, const char *b)
{
	if (!a || !b) return a == b;
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}
int a68_same_ci(const char *a, const char *b)
{
	while (*a && a68_lower((unsigned char)*a) == a68_lower((unsigned char)*b)) { a++; b++; }
	return a68_lower((unsigned char)*a) == a68_lower((unsigned char)*b);
}
int a68_prefix_ci(const char *s, const char *w)
{
	for (; *w; s++, w++)
		if (a68_lower((unsigned char)*s) != a68_lower((unsigned char)*w)) return 0;
	return 1;
}
const char *a68_sort_name(const char *s)
{
	if (a68_prefix_ci(s, "the ") && s[4]) return s + 4;
	return s;
}

unsigned long a68_muldiv(unsigned long a, unsigned long b, unsigned long c)
{
	if (!c) return 0;
	return a / c * b + a % c * b / c;
}
unsigned long a68_random(unsigned long *seed)
{
	/* the constants of the C standard's example rand(), masked to 32 bits so a 64-bit long gives the same */
	*seed = (*seed * 1103515245ul + 12345ul) & 0xFFFFFFFFul;
	return *seed >> 8;
}
