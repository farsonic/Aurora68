/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/play_core_shim.c - a 68020 Linux program's start, its exit and the C library calls tools/play_core_test.c
 * makes (play_core_shim.h says why), so the core's tests run on a big-endian 68020 with 32-bit longs under
 * qemu-m68k, as the classic Play's code runs on the SE: tools/play_core_m68k.sh. The system calls are Linux's
 * on m68k: the number in d0, the arguments in d1 to d3, trap #0. Written on 22 November 2026 (docs/08 item 107).
 */
#include <stdarg.h>
#include "play_core_shim.h"

static long sys3(long n, long a, long b, long c)
{
	register long d0 __asm__("d0") = n;
	register long d1 __asm__("d1") = a;
	register long d2 __asm__("d2") = b;
	register long d3 __asm__("d3") = c;
	__asm__ volatile("trap #0" : "+d"(d0) : "d"(d1), "d"(d2), "d"(d3) : "memory");
	return d0;
}
int main(int argc, char **argv);
void _start(void)
{
	static char *argv[] = { "play_core_test", 0 };
	int r = main(1, argv);
	sys3(1, r, 0, 0);                                /* exit */
	for (;;) { }
}

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) { if (*a != *b) return (unsigned char)*a - (unsigned char)*b; if (!*a) return 0; }
	return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) { } return r; }
char *strstr(const char *h, const char *n)
{
	size_t k = strlen(n);
	for (; *h; h++)
		if (!strncmp(h, n, k)) return (char *)h;
	return k ? 0 : (char *)h;
}
void *memcpy(void *d, const void *s, size_t n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { char *a = d; while (n--) *a++ = (char)c; return d; }
void *memmove(void *d, const void *s, size_t n)
{
	char *a = d; const char *b = s;
	if (a < b) while (n--) *a++ = *b++;
	else { a += n; b += n; while (n--) *--a = *--b; }
	return d;
}

/* printf: %s, %d, %ld, %lu, %c and %%, which is all the test says */
static char out[1024];
static int on;
static void put(char c) { if (on < (int)sizeof out) out[on++] = c; }
static void num(unsigned long v, int neg)
{
	char t[12];
	int n = 0;
	if (neg) put('-');
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n) put(t[--n]);
}
int printf(const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	on = 0;
	for (; *f; f++) {
		int l = 0;
		if (*f != '%') { put(*f); continue; }
		f++;
		if (*f == 'l') { l = 1; f++; }
		switch (*f) {
		case 's': { const char *s = va_arg(ap, const char *); while (s && *s) put(*s++); break; }
		case 'd': { long v = l ? va_arg(ap, long) : va_arg(ap, int); num(v < 0 ? (unsigned long)-v : (unsigned long)v, v < 0); break; }
		case 'u': num(l ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 0); break;
		case 'c': put((char)va_arg(ap, int)); break;
		default: put(*f);
		}
	}
	va_end(ap);
	sys3(4, 1, (long)out, on);                       /* write to standard output */
	return on;
}
