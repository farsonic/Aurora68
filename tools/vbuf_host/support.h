/* SPDX-License-Identifier: MPL-2.0
 * tools/vbuf_host/support.h - the little of the kernel's support.h that psmac_video.c uses, from the C library, so
 * the video card's stream core builds on the host for tools/vbuf_test.c (23 November 2026, docs/08 item 125). */
#ifndef VBUF_HOST_SUPPORT_H
#define VBUF_HOST_SUPPORT_H
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define kprintf(...) printf(__VA_ARGS__)
extern void *tlsf;
#endif
