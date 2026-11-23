/* SPDX-License-Identifier: MPL-2.0
 * tools/pccard_host/support.h - the little of the kernel's support.h that psmac_pc.c uses, from the C library, so the
 * PC card builds on the host with Aurora86's host library for tools/pccard_test.c (23 November 2026, docs/19-pc-card.md). */
#ifndef PCCARD_HOST_SUPPORT_H
#define PCCARD_HOST_SUPPORT_H
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define kprintf(...) printf(__VA_ARGS__)
static inline void arm_flush_cache(uintptr_t a, uint32_t n) { (void)a; (void)n; }
#endif
