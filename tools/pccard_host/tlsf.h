/* SPDX-License-Identifier: MPL-2.0
 * tools/pccard_host/tlsf.h - the kernel's allocator as psmac_pc.c calls it, over the C library's (tools/pccard_test.c) */
#ifndef PCCARD_HOST_TLSF_H
#define PCCARD_HOST_TLSF_H
#include <stdint.h>
extern void *tlsf;
void *tlsf_init_with_memory(void *memory, uintptr_t size);
void *tlsf_malloc(void *handle, uintptr_t size);
void *tlsf_malloc_aligned(void *handle, uintptr_t size, uintptr_t align);
void tlsf_free(void *handle, void *ptr);
uintptr_t tlsf_get_total_size(void *handle);
uintptr_t tlsf_get_free_size(void *handle);
#endif
