/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/play_core_shim.h - the few C library calls tools/play_core_test.c makes, for building it as a 68020
 * Linux program with no C library (tools/play_core_m68k.sh): the cross compiler here has no m68k libc, and the
 * core needs none, so only the test's own printf and string calls are given (play_core_shim.c). Written on
 * 22 November 2026 (docs/08 item 107).
 */
#ifndef A68_PLAY_CORE_SHIM_H
#define A68_PLAY_CORE_SHIM_H
typedef unsigned long size_t;
int printf(const char *fmt, ...);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
size_t strlen(const char *s);
char *strcpy(char *d, const char *s);
char *strstr(const char *h, const char *n);
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memmove(void *d, const void *s, size_t n);
#endif
