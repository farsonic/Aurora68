/* SPDX-License-Identifier: MPL-2.0: the kernel's support.h as far as psmac_etalk.c needs it, for the host test */
#include <stdint.h>
#include <stdio.h>
#define kprintf printf
static inline void arm_flush_cache(uintptr_t a, uint32_t l) { (void)a; (void)l; }
