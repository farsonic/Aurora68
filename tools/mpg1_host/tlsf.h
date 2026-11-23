/* SPDX-License-Identifier: MPL-2.0
 * tools/mpg1_host/tlsf.h - the kernel's allocator as the C library's, so that psmac_plmpeg.c (pl_mpeg and the
 * card's 'mpg1' and MP2) builds on the host for tools/mpg1_test.c and tools/vcodec_test.c (23 November 2026). */
#ifndef MPG1_HOST_TLSF_H
#define MPG1_HOST_TLSF_H
#include <stdlib.h>
#include <stdint.h>
#define tlsf_malloc(h, s)     ((void)(h), malloc(s))
#define tlsf_realloc(h, p, s) ((void)(h), realloc((p), (s)))
#define tlsf_free(h, p)       ((void)(h), free(p))
#define tlsf_get_free_size(h) ((void)(h), (uintptr_t)256 << 20)   /* psmac_video.c's check of the heap (tools/vbuf_test.c) */
#endif
