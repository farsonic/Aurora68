/* SPDX-License-Identifier: MPL-2.0
 *
 * play/core/include/text.h - the small text routines every part of Aurora68 Play's core uses: bounded copies
 * and appends, numbers in and out, times as a player shows them, letters compared without regard to case,
 * URL escaping, and a name as a record shop files it ("The Cure" under C).
 *
 * The core is portable C (play/core/README.md): the classic Play compiles it with m68k-linux-gnu-gcc and no
 * C library (mac/c), the macOS Play with clang as a Swift package's C target, the host tests with cc. So
 * nothing here includes a header or calls the C library: every loop is written out, every output has a
 * length it never passes, and nothing is global. Text is bytes: Mac Roman for the classic Play, UTF-8 for
 * the macOS one; the routines below never look past ASCII, so either goes through unchanged. Written on
 * 22 November 2026 (docs/08 item 107).
 */
#ifndef A68_TEXT_H
#define A68_TEXT_H

/* the length of a C string */
long a68_len(const char *s);
/* s into d: at most max - 1 bytes and a NUL (nothing when max is 0) */
void a68_copy(char *d, const char *s, long max);
/* [s, s + n) into d, the same way */
void a68_copy_n(char *d, const char *s, long n, long max);
/* s appended at p, never at or past end (the caller keeps a byte for the NUL); the new p */
char *a68_put(char *p, char *end, const char *s);
char *a68_put_num(char *p, char *end, unsigned long v);
/* a time in seconds as a player shows it: "3:07", "1:02:03" */
char *a68_put_time(char *p, char *end, unsigned long secs);
/* s with every byte that is not a letter, digit, '-', '.', '_' or '~' written %XX (RFC 3986's unreserved
   characters), as a URL's query or path segment wants it; keep_slash leaves '/' as it is (a path) */
char *a68_put_escaped(char *p, char *end, const char *s, int keep_slash);

/* the decimal digits at the start of s (after spaces); 0 if none */
unsigned long a68_num(const char *s);
/* a decimal number with an optional fraction ("12.5", "273") in thousandths: 12500, 273000. For the
   positions and lengths the server writes in seconds; no floating point on a 68000 */
unsigned long a68_num_ms(const char *s);
/* does s hold only decimal digits (and at least one)? */
int a68_is_number(const char *s);

int a68_lower(int c);
int a68_same(const char *a, const char *b);            /* the same bytes */
int a68_same_ci(const char *a, const char *b);         /* the same, ASCII letters of either case alike */
int a68_prefix_ci(const char *s, const char *w);       /* does s start with w (either case)? */
int a68_space(int c);                                  /* a space or a tab */

/* a name as a record shop files it: past a leading "The " (either case), so "The Cure" sorts and is found by
   typing under C. A name that is only "The" keeps it */
const char *a68_sort_name(const char *s);

/* a * b / c without overflow in 32-bit arithmetic as long as b * c fits (a position on a bar of c pixels:
   a is a length in ms, b the pixels clicked); 0 when c is 0 */
unsigned long a68_muldiv(unsigned long a, unsigned long b, unsigned long c);

/* a pseudo-random number from *seed (a 32-bit linear congruential generator, the same on every machine and
   either byte order: the queue's shuffle and the device's id) */
unsigned long a68_random(unsigned long *seed);
#endif
