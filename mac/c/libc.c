/* mac/c/libc.c - the few C library routines GCC may call on its own (structure copies,
 * clearing arrays) and the small helpers of the C programs in mac/. MPL-2.0. */
#include "toolbox.h"

void *memcpy(void *d, const void *s, unsigned long n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, unsigned long n)
{
	char *a = d; const char *b = s;
	if (a < b) while (n--) *a++ = *b++;
	else { a += n; b += n; while (n--) *--a = *--b; }
	return d;
}
void *memset(void *d, int c, unsigned long n) { char *a = d; while (n--) *a++ = (char)c; return d; }
int memcmp(const void *x, const void *y, unsigned long n)
{
	const unsigned char *a = x, *b = y;
	for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
	return 0;
}
unsigned long strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }

unsigned char *pstr(unsigned char *buf, const char *c)
{
	unsigned n = 0;
	while (c[n] && n < 255) { buf[n + 1] = (unsigned char)c[n]; n++; }
	buf[0] = (unsigned char)n;
	return buf;
}

/* a control's action procedure (crt0.s pas_ctl_action): programs with scroll bars give their own */
__attribute__((weak)) void c_ctl_action(void *ctl, int part)
{
	(void)ctl;
	(void)part;
}
