/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_cmdline.h - cmdline.txt as words: read one option, change one, keep the rest (psmac_cmdline.c).
 * Plain C99 with no kernel headers, so tools/cmdline_test.c tests it on the host.
 */
#ifndef PSMAC_CMDLINE_H
#define PSMAC_CMDLINE_H
#include <stdint.h>

#define PCL_FLAG ((const char *)1)     /* pcl_set's value for a bare word such as enable_cache */

/* the value of key= in the line (quotes taken off) into out: 1 found, 2 found as a bare word, 0 not there */
int pcl_get(const char *line, uint32_t n, const char *key, char *out, uint32_t max);
/* in (n bytes) with key set to value (NULL: every key= word and a bare word key taken out; PCL_FLAG: the
   bare word). The first word of the key keeps its place, later ones go; a key that is not there goes at
   the end. Every other byte of the line stays as it was. Returns the new length, -1 when out is too small,
   -2 when the value cannot be written (a quote or a control character in it) */
int pcl_set(const char *in, uint32_t n, char *out, uint32_t cap, const char *key, const char *value);
/* the text is one line of options: 0, or -1 and a reason in why */
int pcl_check_line(const char *text, uint32_t n, char *why, uint32_t whymax);
/* the words of the line, for listing: calls f for each with its key (or the bare word) and value */
void pcl_words(const char *line, uint32_t n, void (*f)(void *ctx, const char *w, uint32_t wl), void *ctx);

#endif
