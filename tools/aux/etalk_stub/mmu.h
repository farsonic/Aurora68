/* SPDX-License-Identifier: MPL-2.0: the kernel's mmu.h as far as psmac_etalk.c needs it, for the host test */
#include <stdint.h>
static inline uintptr_t mmu_virt2phys(uintptr_t a) { return a; }
