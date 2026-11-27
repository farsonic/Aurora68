/*
 * psmac_mmu.c — 68030 MMU shadowing for the Macintosh IIci ROM on Emu68 (Aurora68 fork). MPL-2.0.
 *
 * Emu68 has no 68k MMU. The IIci ROM programs the 68030 MMU with two static table trees (24-bit and
 * 32-bit mode) and switches between them with PMOVE CRP/TC (SwapMMUMode), hundreds of times during a
 * boot. We shadow that: on every TC/CRP load the 68030 tables are walked into a 4 KiB-page
 * logical->physical table (l2p) and the ARM stage-1 mapping of the 68k address space is made to match,
 * so JIT loads/stores hit Pi RAM directly. Pages whose physical address is I/O stay unmapped; the
 * data-abort handler translates the faulting logical address with l2p (psmac_mmu_l2p) and forwards the
 * physical address to the bridge. Frame-buffer pages (physical < PSMAC_FB_SIZE) are mapped read-only
 * wherever they appear so writes trap and are mirrored to the motherboard.
 *
 * Cost control: the 68k space is user L1 entries 0..3 of the ARM tables. Each distinct (TC, CRP)
 * configuration gets its own cached ARM subtree ("map set"); a switch to a known configuration is a
 * walk (to detect table changes, by hash) plus a swap of four L1 descriptors. The Pi peripheral windows
 * that live inside the first 4 GiB are cloned into every fresh set and never overwritten by Mac runs.
 *
 * 68030 semantics honoured: a CRP load does not take effect until the following TC load or PFLUSH
 * (the ROM relies on the translation of its MMU record surviving the CRP load); PFLUSH re-walks the
 * tables so an OS that edits them in place (System 7 VM, A/UX) is picked up.
 * Not supported: page sizes below 4 KiB, supervisor root pointer (SRE), function-code lookup (FCL).
 *
 * mac.mmu=full (25 November 2026, experimental, docs/23-mmu.md): an access by translated code to a logical
 * page the guest's tables mark invalid becomes a 68030 bus error (psmac_mmufault.c) instead of the shim's
 * $BAD0BAD0 read and ignored write. Since phase 2 (26 November, docs/23 section 11) the option replaces the
 * whole-table shadowing with the complete 68030 PMMU and a soft TLB, at the end of this file; System 7.1's
 * Virtual Memory runs on it. Without the option nothing here behaves differently.
 */
#include <stdint.h>
#include <stdbool.h>
#include "support.h"
#include "tlsf.h"
#include "A64.h"
#include "psmac.h"
#include "psmb.h"

void mmu_map(uintptr_t phys, uintptr_t virt, uintptr_t length, uint32_t attr_low, uint32_t attr_high);
void mmu_unmap(uintptr_t virt, uintptr_t length);
void mmu_print_stats(void);
void mmu_low4g_get(uint64_t out[4]);
void mmu_low4g_set(const uint64_t in[4]);
void mmu_low4g_clone(const uint64_t from[4]);

#define KERNEL_ALIAS 0xffffff9000000000ull
#define PAGES_4K     (1u << 20)                 /* 4 GiB / 4 KiB */
#define L2P_VALID    1u
#define L2P_WP       2u
#define RW_ATTR  (MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED)
#define RO_ATTR  (MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED)
#define NSETS    6
#define WATCH_PHYS_PAGE 0x50000u   /* debug: Mac physical page holding low memory (trap tables); mapped RO so writes trap */
/* mac.watchtraps=1 (psmac_bus.c): the watch above. Off by default since 20 November 2026: every
   write to that page (the trap tables, and all of page 0's low-memory globals, Ticks among them)
   was a data abort and a store by hand, and on the SE's HDMI log the ROM's own trap table set-up
   (PC 0x4080C9DC) printed forty reports and their stacks at every start. */
int psmac_watch_traps;

static struct { uint32_t tc, tt0, tt1, srp_hi, srp_lo, crp_hi, crp_lo; uint16_t mmusr; } mmu;
/* mac.mmu=full: guest page faults are delivered (psmac_mmufault.c, the translator's fault table,
   EMIT_RTE's access-fault frames). Read at translation time and in the abort handler only. */
int psmac_mmu_full;
static uintptr_t ram_phys, rom_phys; static uint32_t ram_size;
static uint32_t *l2p;          /* per 4 KiB logical page of the CURRENT set: physical page | flags (base region only) */
static int fill_enabled;       /* walk mode: 1 = fill l2p, 0 = hash the descriptors only */
static int translate_on;       /* TC.E of the current set */
static uint32_t is_bits;       /* TC.IS of the current set */
static uint32_t rebuilds;
static int quiet_apply;        /* mac.mmu=full: the remap after a page-fault handler's PFLUSH logs only the summary */
static uint32_t pflush_remaps, pflush_us;
static int crp_dirty;          /* CRP changed while translating; rebuild pending until TC load or PFLUSH */
static uint64_t last_install;  /* counter value of the last TC/CRP switch (the PFLUSHA right after it is implied) */

struct mapset {
    uint32_t tc, crp_hi, crp_lo;   /* key */
    uint32_t hash, fills;          /* identity of the walked tables */
    uint32_t is_bits; int translate_on;
    uint64_t l1[4];                /* the ARM subtree (user L1 entries 0..3) */
    uint32_t *bits;                /* bitmap of 4 KiB logical pages we mapped in this subtree */
    uint32_t *l2p; uint32_t l2p_pages;   /* this set's logical->physical table (base region) */
    uint32_t lru; int used;
};
static struct mapset sets[NSETS];
static struct mapset *cur;
static uint64_t pristine[4];       /* subtree as Emu68 left it: Pi peripheral windows only */
static uint32_t lru_clock;

static inline uint64_t cnt_now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0":"=r"(v)); return v; }
static inline uint64_t cnt_frq(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0":"=r"(v)); return v; }
static inline uint32_t ms_since(uint64_t t0) { return (uint32_t)((cnt_now() - t0) * 1000 / cnt_frq()); }

static inline uint32_t norm_phys(uint32_t p);
static inline uint32_t rd32p(uint32_t paddr)   /* read a longword of Mac physical RAM */
{
    paddr = norm_phys(paddr);
    if (paddr > ram_size - 4) return 0;          /* table outside RAM (or garbage TC/CRP): treat as invalid descriptor */
    return *(volatile uint32_t *)(KERNEL_ALIAS + ram_phys + paddr);
}
static inline uint32_t rd32(uint32_t ea) { return *(volatile uint32_t *)(uintptr_t)ea; }
static inline void wr32(uint32_t ea, uint32_t v) { *(volatile uint32_t *)(uintptr_t)ea = v; }

/* Mac physical -> Pi physical for mappable memory; returns 0 if not RAM/ROM */
#define BANK_A_WINDOW 0x04000000u   /* IIci bank A decodes 64 MiB; installed RAM aliases across it */
/* RAM smaller than a bank's window aliases across it (the ROM sizes memory by looking for the wrap).
   The IIci has two banks of 64 MiB: A at 0 and B at 0x04000000. Up to 64 MiB everything is bank A
   and aliases across its window; past that bank A is full and the rest is bank B, which aliases
   across its own window (72 MiB is 64 + 8: the first boot with a part-filled bank B found nothing
   past 72 MiB and the ROM never got out of its memory test, docs/08 item 27). Beyond 128 MiB the
   ROM sees nothing more without the memory-sizing patch (docs/06), but mac.ram= may ask. */
static inline uint32_t norm_phys(uint32_t p)
{
    if (p < ram_size) return p;
    if (ram_size <= BANK_A_WINDOW) return p < BANK_A_WINDOW ? p % ram_size : p;
    uint32_t bank_b = ram_size - BANK_A_WINDOW;
    if (p < 2 * BANK_A_WINDOW && bank_b < BANK_A_WINDOW) return BANK_A_WINDOW + (p - BANK_A_WINDOW) % bank_b;
    return p;
}
static uintptr_t slot_win_phys, slot_declrom_phys, slot_fb_phys;   /* Pi RAM behind slot $9 (0 until set) */
static uint32_t slot_fb_size;                                      /* the big frame buffer's size: 9 or 15 MiB (psmb.h) */
void psmac_mmu_set_slot(uintptr_t w, uintptr_t d) { slot_win_phys = w; slot_declrom_phys = d; }
void psmac_mmu_set_fb(uintptr_t f, uint32_t size) { slot_fb_phys = f; slot_fb_size = size; }   /* the second display's big frame buffer */
/* slot $A, the SE board's HDMI card (psmb.h, "two screens"): its declaration ROM lives in the slot-9
   window's top 16 KiB; the rest of slot $A's space is empty, as before. 0: no such card (every other
   board, and mac.hdmi=mirror or log, where slot $A stays empty for the Slot Manager) */
static uintptr_t slot2_rom_phys;
void psmac_mmu_set_slot2(uintptr_t d) { slot2_rom_phys = d; }
/* slot $C, the video card (psmb.h, psmac_videocard.c): its 1 MiB window twice (32-bit and 24-bit
   form) and its ROM page; 0 with mac.video=off */
static uintptr_t vid_win_phys, vid_rom_phys;
#define VID_WIN_SIZE 0x100000u                /* psmac_video.h: A68V_WIN_SIZE */
void psmac_mmu_set_video(uintptr_t w, uintptr_t r) { vid_win_phys = w; vid_rom_phys = r; }
#ifdef PSMAC_PC
/* slot $D, the PC card (psmac_pc.c, 23 November 2026): the same three pieces as slot $C's; 0 with pc=off */
static uintptr_t pc_win_phys, pc_rom_phys;
#define PC_WIN_SIZE 0x100000u                 /* psmac_pc.h: A68P_WIN_SIZE */
void psmac_mmu_set_pc(uintptr_t w, uintptr_t r) { pc_win_phys = w; pc_rom_phys = r; }
#endif
#ifdef PSMAC_A2
/* slot $E, the Apple II card (psmac_a2.c, 23 November 2026): the same three pieces again; 0 with a2=off */
static uintptr_t a2_win_phys, a2_rom_phys;
#define A2_WIN_SIZE 0x100000u                 /* psmac_a2.h: A68A_WIN_SIZE */
void psmac_mmu_set_a2(uintptr_t w, uintptr_t r) { a2_win_phys = w; a2_rom_phys = r; }
#endif
/* the card's window as Pi memory, for a 68k physical address in either of its forms, and how many bytes of it
   are left from there; 0 outside it. The mailbox's disk and USB services use it (psmac_mbox.c mac_page): Aurora68
   Video reads a film from disk straight into the card's ring (23 November 2026, the SE: without this every such
   read failed with a bad address and the program ended the stream before its first byte) */
uintptr_t psmac_mmu_video_phys(uint32_t p, uint32_t *left)
{
    uint32_t off;
    if (!vid_win_phys) return 0;
    if (p >= PSMB_VID_WIN_PHYS && p < PSMB_VID_WIN_PHYS + VID_WIN_SIZE) off = p - PSMB_VID_WIN_PHYS;
    else if (p >= PSMB_VID_WIN24_PHYS && p < PSMB_VID_WIN24_PHYS + VID_WIN_SIZE) off = p - PSMB_VID_WIN24_PHYS;
    else return 0;
    *left = VID_WIN_SIZE - off;
    return vid_win_phys + off;
}
static inline uintptr_t pi_phys(uint32_t p)
{
    p = norm_phys(p);
    if (p < ram_size) return ram_phys + p;
    if (p >= 0x40000000u && p < 0x50000000u) return rom_phys + (p & (PSMAC_ROM_SIZE - 1));
    if (slot_win_phys && p >= PSMB_WIN_PHYS && p < PSMB_WIN_PHYS + PSMB_WIN_SIZE) return slot_win_phys + (p - PSMB_WIN_PHYS);
    /* the same megabyte once more at $F9900000: NuBus geometry puts the slot's 24-bit megabyte
       ($900000-$9FFFFF) there in 32-bit space, and the ROM's screen set-up addresses the video
       card's frame buffer in that form (docs/10) */
    if (slot_win_phys && p >= PSMB_WIN_PHYS + 0x900000u && p < PSMB_WIN_PHYS + 0x900000u + PSMB_WIN_SIZE) return slot_win_phys + (p - PSMB_WIN_PHYS - 0x900000u);
    if (slot_declrom_phys && p >= PSMB_DECLROM_PHYS && p < PSMB_DECLROM_PHYS + PSMB_DECLROM_SIZE) return slot_declrom_phys + (p - PSMB_DECLROM_PHYS);
    if (slot2_rom_phys && p >= PSMB_DECLROM2_PHYS && p < PSMB_DECLROM2_PHYS + PSMB_DECLROM2_SIZE) return slot2_rom_phys + (p - PSMB_DECLROM2_PHYS);
    if (vid_win_phys && p >= PSMB_VID_WIN_PHYS && p < PSMB_VID_WIN_PHYS + VID_WIN_SIZE) return vid_win_phys + (p - PSMB_VID_WIN_PHYS);
    if (vid_win_phys && p >= PSMB_VID_WIN24_PHYS && p < PSMB_VID_WIN24_PHYS + VID_WIN_SIZE) return vid_win_phys + (p - PSMB_VID_WIN24_PHYS);
    if (vid_rom_phys && p >= PSMB_VID_ROM_PHYS && p < PSMB_VID_ROM_PHYS + PSMB_VID_ROM_SIZE) return vid_rom_phys + (p - PSMB_VID_ROM_PHYS);
#ifdef PSMAC_PC
    if (pc_win_phys && p >= PSMB_PC_WIN_PHYS && p < PSMB_PC_WIN_PHYS + PC_WIN_SIZE) return pc_win_phys + (p - PSMB_PC_WIN_PHYS);
    if (pc_win_phys && p >= PSMB_PC_WIN24_PHYS && p < PSMB_PC_WIN24_PHYS + PC_WIN_SIZE) return pc_win_phys + (p - PSMB_PC_WIN24_PHYS);
    if (pc_rom_phys && p >= PSMB_PC_ROM_PHYS && p < PSMB_PC_ROM_PHYS + PSMB_PC_ROM_SIZE) return pc_rom_phys + (p - PSMB_PC_ROM_PHYS);
#endif
#ifdef PSMAC_A2
    if (a2_win_phys && p >= PSMB_A2_WIN_PHYS && p < PSMB_A2_WIN_PHYS + A2_WIN_SIZE) return a2_win_phys + (p - PSMB_A2_WIN_PHYS);
    if (a2_win_phys && p >= PSMB_A2_WIN24_PHYS && p < PSMB_A2_WIN24_PHYS + A2_WIN_SIZE) return a2_win_phys + (p - PSMB_A2_WIN24_PHYS);
    if (a2_rom_phys && p >= PSMB_A2_ROM_PHYS && p < PSMB_A2_ROM_PHYS + PSMB_A2_ROM_SIZE) return a2_rom_phys + (p - PSMB_A2_ROM_PHYS);
#endif
    /* the second display's big frame buffer, in the slot's super slot space ($90000000): the ROM's
       32-bit tables map $40000000-$FBAFFFFF one to one, so logical and physical agree there */
    if (slot_fb_phys && p >= PSMB_FB_BIG_PHYS && p < PSMB_FB_BIG_PHYS + slot_fb_size) return slot_fb_phys + (p - PSMB_FB_BIG_PHYS);
    if (p >= 0xF0000000u) { extern uintptr_t psmac_etalk_phys(uint32_t); return psmac_etalk_phys(p); }   /* the A/UX network card's buffer and ROM (psmac_etalk.c, mac.auxnet=) */
    return 0;
}
/* Mac physical pages that must be read-only for the 68k: ROM, declaration ROM, write-through
   frame buffer and the (debug) watched low-memory page.
   The write-through frame buffer is the IIci's (its RBV scans the first 320 KiB of bank A on the motherboard, so every
   write there is mirrored out, psmac_bus.c). The SE board has no RBV video and never mirrored those writes (its
   screens are the card's), so since 26 November 2026 that memory is ordinary RAM there (docs/23 section 18): A/UX keeps
   its kernel stack and a new process's page tables in it ($4C000, $4D000), every write went through the abort path,
   and on the owner's Pi a slot of the kernel's stack read back other than what had just been written through that
   path ($4CF14: $4D000 written, something else read), so the root table entry went astray and A/UX panicked. */
extern int psmac_board_se;
extern int psmac_etalk_ro(uint32_t p);
static inline int ro_phys(uint32_t p)
{
    return (p < PSMAC_FB_SIZE && !psmac_board_se) || (p >= 0x40000000u && p < 0x50000000u) || (psmac_watch_traps && p == WATCH_PHYS_PAGE) ||
           (p >= PSMB_DECLROM_PHYS && p < PSMB_DECLROM_PHYS + PSMB_DECLROM_SIZE) ||
           (slot2_rom_phys && p >= PSMB_DECLROM2_PHYS && p < PSMB_DECLROM2_PHYS + PSMB_DECLROM2_SIZE) ||
           (vid_rom_phys && p >= PSMB_VID_ROM_PHYS && p < PSMB_VID_ROM_PHYS + PSMB_VID_ROM_SIZE) ||
           psmac_etalk_ro(p)                                    /* the A/UX network card's ROM (psmac_etalk.c) */
#ifdef PSMAC_PC
           || (pc_rom_phys && p >= PSMB_PC_ROM_PHYS && p < PSMB_PC_ROM_PHYS + PSMB_PC_ROM_SIZE)
#endif
#ifdef PSMAC_A2
           || (a2_rom_phys && p >= PSMB_A2_ROM_PHYS && p < PSMB_A2_ROM_PHYS + PSMB_A2_ROM_SIZE)
#endif
           ;
}

/* ---- per-set bookkeeping of what we mapped ---- */
static inline void bit_set(uint32_t *b, uint32_t i) { b[i >> 5] |= 1u << (i & 31); }
static inline int  bit_get(const uint32_t *b, uint32_t i) { return (b[i >> 5] >> (i & 31)) & 1; }

static void unmap_set(struct mapset *s)        /* s must be installed */
{
    uint32_t i = 0;
    while (i < PAGES_4K) {
        if (!s->bits[i >> 5]) { i = (i | 31) + 1; continue; }
        if (!bit_get(s->bits, i)) { i++; continue; }
        uint32_t j = i;
        while (j < PAGES_4K && bit_get(s->bits, j)) j++;
        mmu_unmap((uintptr_t)i << 12, (uintptr_t)(j - i) << 12);
        i = j;
    }
    for (uint32_t k = 0; k < PAGES_4K / 32; k++) s->bits[k] = 0;
}

/* Windows inside the 68k address space that belong to the Pi (cloned from the pristine subtree):
   the BCM283x peripherals at 0x3F000000 and the local interrupt controller at 0x40000000. Mac runs are
   clipped around them; Mac accesses there fault and go to the bridge like any other slot space. */
static const struct { uintptr_t lo, hi; } pi_win[] = { { 0x3f000000ull, 0x40000000ull }, { 0x40000000ull, 0x40200000ull } };
static void map_run(uintptr_t pi, uintptr_t virt, uintptr_t len, uint32_t attr)
{
    for (unsigned k = 0; k < sizeof(pi_win) / sizeof(pi_win[0]); k++) {
        if (virt < pi_win[k].hi && virt + len > pi_win[k].lo) {
            if (virt < pi_win[k].lo) map_run(pi, virt, pi_win[k].lo - virt, attr);
            if (virt + len > pi_win[k].hi) { uintptr_t skip = pi_win[k].hi - virt; map_run(pi + skip, pi_win[k].hi, virt + len - pi_win[k].hi, attr); }
            return;
        }
    }
    mmu_map(pi, virt, len, attr, 0);
    for (uintptr_t v = virt >> 12; v < (virt + len) >> 12; v++) bit_set(cur->bits, (uint32_t)v);
}

/* ---- apply l2p (base region of 2^(32-IS) bytes, replicated over the IS aliases) ---- */
static void apply_l2p(void)
{
    uint32_t region_pages = (is_bits == 0) ? PAGES_4K : (1u << (32 - is_bits - 12));
    uint32_t aliases = 1u << is_bits;
    uint32_t runs = 0;
    for (uint32_t a = 0; a < aliases; a++) {
        uintptr_t abase = (uintptr_t)a << (32 - is_bits);
        if (is_bits == 0 && a) break;
        uint32_t i = 0;
        while (i < region_pages) {
            uint32_t e = l2p[i];
            if (!(e & L2P_VALID) || !pi_phys(e & ~0xfffu)) { i++; continue; }
            uint32_t attr_ro = (e & L2P_WP) || ro_phys(e & ~0xfffu);
            uintptr_t pi0 = pi_phys(e & ~0xfffu);
            uint32_t j = i + 1;
            while (j < region_pages) {
                uint32_t f = l2p[j];
                if (!(f & L2P_VALID)) break;
                uintptr_t pj = pi_phys(f & ~0xfffu);
                if (pj != pi0 + ((uintptr_t)(j - i) << 12)) break;
                uint32_t ro = (f & L2P_WP) || ro_phys(f & ~0xfffu);
                if (ro != attr_ro) break;
                j++;
            }
            map_run(pi0, abase + ((uintptr_t)i << 12), (uintptr_t)(j - i) << 12, attr_ro ? RO_ATTR : RW_ATTR);
            runs++;
            i = j;
        }
    }
    if (!quiet_apply)
        kprintf("[PSMAC:MMU]   applied %u mapping runs (IS=%u, %u aliases)\n", runs, is_bits, aliases);
}

/* ---- identity map (MMU disabled) ---- */
static void build_identity(void)
{
    is_bits = 0;
    for (uint32_t i = 0; i < PAGES_4K; i++) l2p[i] = 0;
    /* RAM and its aliases: bank A's window when that is all there is, both windows once bank B holds anything */
    uint32_t top = ram_size <= BANK_A_WINDOW ? BANK_A_WINDOW : (ram_size < 2 * BANK_A_WINDOW ? 2 * BANK_A_WINDOW : ram_size);
    for (uint32_t p = 0; p < top; p += 4096) l2p[p >> 12] = norm_phys(p) | L2P_VALID;   /* RAM + its aliases */
    for (uint32_t p = 0; p < PSMAC_ROM_SIZE; p += 4096) {
        l2p[(PSMAC_ROM_BASE + p) >> 12]   = (PSMAC_ROM_BASE + p) | L2P_VALID | L2P_WP;
        l2p[(PSMAC_ROM_MIRROR + p) >> 12] = (PSMAC_ROM_MIRROR + p) | L2P_VALID | L2P_WP;
    }
    for (uint32_t p = 0; p < PSMB_WIN_SIZE; p += 4096) l2p[(PSMB_WIN_PHYS + p) >> 12] = (PSMB_WIN_PHYS + p) | L2P_VALID;
    for (uint32_t p = 0; p < PSMB_WIN_SIZE; p += 4096) l2p[(PSMB_WIN_PHYS + 0x900000u + p) >> 12] = (PSMB_WIN_PHYS + 0x900000u + p) | L2P_VALID;   /* its 24-bit-megabyte alias */
    for (uint32_t p = 0; p < PSMB_DECLROM_SIZE; p += 4096) l2p[(PSMB_DECLROM_PHYS + p) >> 12] = (PSMB_DECLROM_PHYS + p) | L2P_VALID | L2P_WP;
    if (slot2_rom_phys)
        for (uint32_t p = 0; p < PSMB_DECLROM2_SIZE; p += 4096) l2p[(PSMB_DECLROM2_PHYS + p) >> 12] = (PSMB_DECLROM2_PHYS + p) | L2P_VALID | L2P_WP;
    for (uint32_t p = 0; p < slot_fb_size; p += 4096) l2p[(PSMB_FB_BIG_PHYS + p) >> 12] = (PSMB_FB_BIG_PHYS + p) | L2P_VALID;   /* the big frame buffer */
    if (vid_win_phys) {                                                                                   /* the video card, slot $C */
        for (uint32_t p = 0; p < VID_WIN_SIZE; p += 4096) l2p[(PSMB_VID_WIN_PHYS + p) >> 12] = (PSMB_VID_WIN_PHYS + p) | L2P_VALID;
        for (uint32_t p = 0; p < VID_WIN_SIZE; p += 4096) l2p[(PSMB_VID_WIN24_PHYS + p) >> 12] = (PSMB_VID_WIN24_PHYS + p) | L2P_VALID;
        for (uint32_t p = 0; p < PSMB_VID_ROM_SIZE; p += 4096) l2p[(PSMB_VID_ROM_PHYS + p) >> 12] = (PSMB_VID_ROM_PHYS + p) | L2P_VALID | L2P_WP;
    }
#ifdef PSMAC_PC
    if (pc_win_phys) {                                                                                    /* the PC card, slot $D */
        for (uint32_t p = 0; p < PC_WIN_SIZE; p += 4096) l2p[(PSMB_PC_WIN_PHYS + p) >> 12] = (PSMB_PC_WIN_PHYS + p) | L2P_VALID;
        for (uint32_t p = 0; p < PC_WIN_SIZE; p += 4096) l2p[(PSMB_PC_WIN24_PHYS + p) >> 12] = (PSMB_PC_WIN24_PHYS + p) | L2P_VALID;
        for (uint32_t p = 0; p < PSMB_PC_ROM_SIZE; p += 4096) l2p[(PSMB_PC_ROM_PHYS + p) >> 12] = (PSMB_PC_ROM_PHYS + p) | L2P_VALID | L2P_WP;
    }
#endif
#ifdef PSMAC_A2
    if (a2_win_phys) {                                                                                    /* the Apple II card, slot $E */
        for (uint32_t p = 0; p < A2_WIN_SIZE; p += 4096) l2p[(PSMB_A2_WIN_PHYS + p) >> 12] = (PSMB_A2_WIN_PHYS + p) | L2P_VALID;
        for (uint32_t p = 0; p < A2_WIN_SIZE; p += 4096) l2p[(PSMB_A2_WIN24_PHYS + p) >> 12] = (PSMB_A2_WIN24_PHYS + p) | L2P_VALID;
        for (uint32_t p = 0; p < PSMB_A2_ROM_SIZE; p += 4096) l2p[(PSMB_A2_ROM_PHYS + p) >> 12] = (PSMB_A2_ROM_PHYS + p) | L2P_VALID | L2P_WP;
    }
#endif
}

/* ---- 68030 table walk ---- */
struct walk { uint32_t ps, ti[4]; uint32_t npages; uint32_t hash, fills; };
static void fill(uint32_t log, uint32_t size, uint32_t phys, int wp, struct walk *w)
{
    uint32_t base = (norm_phys(phys) & ~0xfffu) | L2P_VALID | (wp ? L2P_WP : 0);
    w->hash = (w->hash ^ log) * 16777619u; w->hash = (w->hash ^ size) * 16777619u; w->hash = (w->hash ^ base) * 16777619u;
    w->fills++;
    if (!fill_enabled) return;
    for (uint32_t o = 0; o < size; o += 4096) {
        uint32_t idx = (log + o) >> 12;
        if (idx >= w->npages) break;
        l2p[idx] = (phys + o < 2 * BANK_A_WINDOW) ? ((norm_phys(phys + o) & ~0xfffu) | (base & 3)) : (base + o);   /* page by page through the bank aliases */
    }
}
/* table at Mac physical 'addr' with descriptors of 'dsize' bytes, this level has ti bits, covering
   logical [log_base, log_base + 2^(bits_below+ti)); bits_below = bits of address below this index */
static void walk_table(uint32_t addr, int dsize, int level, uint32_t log_base, uint32_t bits_below, uint32_t limit, int lower_limit, struct walk *w)
{
    uint32_t ti = w->ti[level];
    uint32_t n = 1u << ti;
    uint32_t span = (bits_below >= 32) ? 0 : (1u << bits_below);
    for (uint32_t idx = 0; idx < n; idx++) {
        if (lower_limit ? (idx < limit) : (idx > limit)) continue;
        uint32_t log = log_base + idx * span;
        uint32_t d0 = rd32p(addr + idx * dsize), d1 = (dsize == 8) ? rd32p(addr + idx * dsize + 4) : 0;
        uint32_t dt = d0 & 3;
        if (dt == 0) continue;
        if (dt == 1) {                                  /* page descriptor (early termination allowed) */
            uint32_t phys = (dsize == 8 ? d1 : d0) & 0xffffff00u;
            fill(log, span, phys, (d0 >> 2) & 1, w);
            continue;
        }
        /* table descriptor: next level */
        int next = level + 1;
        if (next > 3 || w->ti[next] == 0) continue;      /* malformed */
        uint32_t taddr = (dsize == 8 ? d1 : d0) & 0xfffffff0u;
        uint32_t nlimit = 0x7fff; int nlower = 0;
        if (dsize == 8) { nlimit = (d0 >> 16) & 0x7fff; nlower = d0 >> 31; }
        walk_table(taddr, dt == 3 ? 8 : 4, next, log, bits_below - w->ti[next], nlimit, nlower, w);
    }
}

/* print the logical->physical runs of the base region (alias 0) so the ROM's tables can be checked */
static void dump_l2p(uint32_t npages)
{
    uint32_t i = 0, lines = 0;
    while (i < npages && lines < 40) {
        uint32_t e = l2p[i];
        if (!(e & L2P_VALID)) { i++; continue; }
        uint32_t j = i + 1;
        while (j < npages && (l2p[j] & L2P_VALID) && (l2p[j] & L2P_WP) == (e & L2P_WP) &&
               (l2p[j] & ~0xfffu) == (e & ~0xfffu) + ((j - i) << 12)) j++;
        kprintf("[PSMAC:MMU]   log %08x-%08x -> phys %08x%s\n", i << 12, (j << 12) - 1, e & ~0xfffu, (e & L2P_WP) ? " WP" : "");
        lines++; i = j;
    }
}

/* Walk the current TC/CRP into l2p. Returns 0 if TC is unusable (then nothing was touched). */
static int walk_current(struct walk *w)
{
    w->hash = 2166136261u; w->fills = 0;
    w->ps = (mmu.tc >> 20) & 0xf; uint32_t is = (mmu.tc >> 16) & 0xf;
    w->ti[0] = (mmu.tc >> 12) & 0xf; w->ti[1] = (mmu.tc >> 8) & 0xf; w->ti[2] = (mmu.tc >> 4) & 0xf; w->ti[3] = mmu.tc & 0xf;
    if (!(mmu.tc & 0x80000000u)) { translate_on = 0; is_bits = 0; w->npages = PAGES_4K; if (fill_enabled) build_identity(); return 1; }
    uint32_t sum = w->ps + is + w->ti[0] + w->ti[1] + w->ti[2] + w->ti[3];
    /* 68030 rule: TIA != 0 and the fields must account for all 32 address bits; we also need PS >= 4 KiB */
    if (w->ps < 12 || w->ti[0] == 0 || sum != 32 || (mmu.crp_hi & 3) == 0) {
        kprintf("[PSMAC:MMU] TC=%08x CRP=%08x:%08x rejected (fields %u/%u/%u/%u/%u/%u), keeping current map\n",
                mmu.tc, mmu.crp_hi, mmu.crp_lo, w->ps, is, w->ti[0], w->ti[1], w->ti[2], w->ti[3]);
        return 0;
    }
    if (mmu.tc & (1u << 25)) kprintf("[PSMAC:MMU] warning: SRE set, using CRP for all accesses\n");
    translate_on = 1; is_bits = is;
    uint32_t region_bits = 32 - is_bits;
    w->npages = 1u << (region_bits - 12);
    if (fill_enabled) for (uint32_t i = 0; i < w->npages; i += 2) *(uint64_t *)&l2p[i] = 0;
    uint32_t root_dt = mmu.crp_hi & 3, root = mmu.crp_lo & 0xfffffff0u;
    uint32_t limit = (mmu.crp_hi >> 16) & 0x7fff; int lower = mmu.crp_hi >> 31;
    if (root_dt == 1) fill(0, 0xffffffffu, mmu.crp_lo & 0xffffff00u, 0, w);
    else walk_table(root, root_dt == 3 ? 8 : 4, 0, 0, region_bits - w->ti[0], limit, lower, w);
    return 1;
}

static void install(struct mapset *s)
{
    mmu_low4g_set(s->l1);
    cur = s; s->lru = ++lru_clock;
    last_install = cnt_now();
}

/* Switch to (or create) the set for the current TC/CRP and bring its ARM mapping up to date. */
static uint32_t *set_l2p(struct mapset *s, uint32_t npages)
{
    extern void *tlsf;
    if (s->l2p && s->l2p_pages >= npages) return s->l2p;
    if (s->l2p) tlsf_free(tlsf, s->l2p);
    s->l2p = tlsf_malloc(tlsf, (size_t)npages * sizeof(uint32_t));
    s->l2p_pages = s->l2p ? npages : 0;
    if (!s->l2p) kprintf("[PSMAC:MMU] out of memory for a %u-page map\n", npages);
    return s->l2p;
}

/* Switch to (or create) the set for the current TC/CRP and bring its ARM mapping up to date.
   The 68030 tables are hashed on every switch (a few hundred descriptor reads); the expensive
   per-page fill and ARM remap only happen when the hash differs from the set's. */
static void rebuild(void)
{
    uint64_t t0 = cnt_now();
    struct walk w;
    rebuilds++;
    fill_enabled = 0;
    if (!walk_current(&w)) return;
    uint32_t key_tc = translate_on ? mmu.tc : 0, key_hi = translate_on ? mmu.crp_hi : 0, key_lo = translate_on ? mmu.crp_lo : 0;
    uint32_t t_walk = ms_since(t0);

    struct mapset *s = NULL, *victim = NULL;
    for (int i = 0; i < NSETS; i++) {
        if (sets[i].used && sets[i].tc == key_tc && sets[i].crp_hi == key_hi && sets[i].crp_lo == key_lo) { s = &sets[i]; break; }
        if (!sets[i].used) { if (!victim || victim->used) victim = &sets[i]; }
        else if (&sets[i] != cur && (!victim || (victim->used && sets[i].lru < victim->lru))) victim = &sets[i];
    }
    int fresh = 0;
    if (!s) {
        s = victim;
        if (s->used) {                     /* evict: the subtree must be installed to be unmapped */
            kprintf("[PSMAC:MMU]   evicting set TC=%08x CRP=%08x:%08x\n", s->tc, s->crp_hi, s->crp_lo);
            install(s); unmap_set(s);
        }
        s->used = 1; s->tc = key_tc; s->crp_hi = key_hi; s->crp_lo = key_lo;
        s->hash = ~w.hash; s->fills = 0;
        for (int i = 0; i < 4; i++) s->l1[i] = 0;
        install(s);
        mmu_low4g_clone(pristine);         /* Pi windows first, then the Mac runs around them */
        fresh = 1;
    } else install(s);

    int changed = (s->hash != w.hash || s->fills != w.fills);
    l2p = set_l2p(s, w.npages);
    if (changed) {
        if (!fresh) { kprintf("[PSMAC:MMU]   tables changed under set, remapping\n"); unmap_set(s); }
        fill_enabled = 1;
        walk_current(&w);                  /* same tables, this time filling the set's l2p */
        apply_l2p();
        s->hash = w.hash; s->fills = w.fills;
    }
    s->is_bits = is_bits; s->translate_on = translate_on;
    mmu_low4g_get(s->l1);                  /* mmu_map/unmap may have replaced the L1 descriptors */
    if (fresh || changed)
        kprintf("[PSMAC:MMU] switch #%u: TC=%08x CRP=%08x:%08x %s%s (hash %u ms, total %u ms, %u fills)\n", rebuilds, mmu.tc, mmu.crp_hi, mmu.crp_lo,
                fresh ? "new set" : "cached set, remapped", translate_on ? "" : " [identity]", t_walk, ms_since(t0), w.fills);
    else if ((rebuilds & 1023) == 0)
        kprintf("[PSMAC:MMU] switch #%u: cached (%u us)\n", rebuilds, (uint32_t)((cnt_now() - t0) * 1000000 / cnt_frq()));
    if (fresh && translate_on) dump_l2p(w.npages);
    if (fresh) mmu_print_stats();
    last_install = cnt_now();              /* the switch is complete: the PFLUSHA right after it is implied */
}

/* PFLUSH: the 68030 drops its ATC, so table edits made in place become visible. Re-walk and remap if
   the tables differ from what the current set was built from. The PFLUSHA that follows every TC load
   in SwapMMUMode is implied by the switch itself and skipped: only that one, the JIT telling which it
   is (the very next instruction after a PMOVE to TC, after_tc), and only within a millisecond of the
   switch. Until 21 November 2026 every PFLUSH within 5 ms of a switch was skipped, so a descriptor the
   guest changed and flushed in that time kept its old page (found by an outside review, docs/08 item 92). */
static void pflush(int after_tc)
{
    if (crp_dirty) { crp_dirty = 0; kprintf("[PSMAC:MMU] PFLUSH applies deferred CRP change\n"); rebuild(); return; }
    if (!cur || !translate_on) return;
    if (after_tc && cnt_now() - last_install < cnt_frq() / 1000) return;   /* SwapMMUMode's own pflusha */
    struct walk w;
    fill_enabled = 0;
    if (!walk_current(&w)) return;
    if (w.hash == cur->hash && w.fills == cur->fills) return;
    uint64_t t0 = cnt_now();
    if (!psmac_mmu_full)
        kprintf("[PSMAC:MMU] PFLUSH: tables changed in place, remapping set TC=%08x CRP=%08x:%08x\n", cur->tc, cur->crp_hi, cur->crp_lo);
    unmap_set(cur);
    l2p = set_l2p(cur, w.npages);
    fill_enabled = 1; walk_current(&w);
    quiet_apply = psmac_mmu_full;
    apply_l2p(); cur->hash = w.hash; cur->fills = w.fills;
    quiet_apply = 0;
    mmu_low4g_get(cur->l1);
    if (psmac_mmu_full) {
        /* a guest's page-fault handler flushes after every descriptor it validates: count them and the
           time they cost (the whole set is re-walked and remapped, docs/23-mmu.md "phase 2") */
        pflush_remaps++; pflush_us += (uint32_t)((cnt_now() - t0) * 1000000 / cnt_frq());
        if (pflush_remaps <= 2 || (pflush_remaps & 63) == 0)
            kprintf("[PSMAC:MMU] PFLUSH remap #%u: tables changed in place, set TC=%08x CRP=%08x:%08x (%u us, %u us on average)\n",
                    pflush_remaps, cur->tc, cur->crp_hi, cur->crp_lo, (uint32_t)((cnt_now() - t0) * 1000000 / cnt_frq()), pflush_us / pflush_remaps);
    }
}

static void spaces_init(void);             /* mac.mmu=full (the end of this file) */
static void alias_selfcheck(void);

/* ---- public ---- */
void psmac_mmu_init(uintptr_t ramp, uint32_t rams, uintptr_t romp)
{
    extern void *tlsf;
    ram_phys = ramp; ram_size = rams; rom_phys = romp;
    for (int i = 0; i < NSETS; i++) {
        sets[i].l2p = NULL; sets[i].l2p_pages = 0;
        sets[i].bits = tlsf_malloc(tlsf, PAGES_4K / 8);
        for (uint32_t k = 0; k < PAGES_4K / 32; k++) sets[i].bits[k] = 0;
        sets[i].used = 0;
    }
    {   extern const char *psmac_option(const char *key);
        const char *o = psmac_option("mac.mmu");
        psmac_mmu_full = o && o[0] == 'f' && o[1] == 'u' && o[2] == 'l' && o[3] == 'l' && !o[4];
        /* mac.mmu=nosnap: the same without the translator's copies of the address registers, a diagnostic
           that shows which restarts need them (docs/23-mmu.md, "the spike") */
        if (o && o[0] == 'n' && o[1] == 'o' && o[2] == 's' && o[3] == 'n' && o[4] == 'a' && o[5] == 'p' && !o[6])
            psmac_mmu_full = 2;
        if (psmac_mmu_full)
            kprintf("[PSMAC:MMU] mac.mmu=%s: guest page faults become 68030 bus errors (experimental, docs/23-mmu.md)\n", o);
    }
    mmu_low4g_get(pristine);
    kprintf("[PSMAC:MMU] pristine low-4G subtree: %016llx %016llx %016llx %016llx\n", pristine[0], pristine[1], pristine[2], pristine[3]);
    mmu.tc = 0;
    rebuild();   /* identity set (with mac.mmu=full, what the other cores keep seeing in mmu_user_L1) */
    if (psmac_mmu_full) { spaces_init(); alias_selfcheck(); }
}

/* mac.mmu=full (psmac_mmufault.c): 1 when the guest translates, and the kernel's own pointer to the Mac RAM
   behind a logical address (for building a bus-error frame from the abort handler), 0 if the page is
   invalid or not RAM */
uint8_t *psmac_mmu_ram_ptr(uint32_t logical)
{
    int valid;
    uint32_t p = psmac_mmu_l2p(logical, &valid);
    if (!valid) return 0;
    p = norm_phys(p);
    if (p >= ram_size) return 0;
    return (uint8_t *)(KERNEL_ALIAS + ram_phys + p);
}

/* The same at a given function code, whichever view TTBR0 holds: an exception's frame and its vector are supervisor
   data even when the fault came from user mode with the views apart (psmac_mmufault.c's push_frame). Until 26 November
   2026 the frame of a bus error from user mode was looked for in the user's view: A/UX's init, its first page of text
   not yet there, could not be given its page fault (docs/23 section 15). */
static uint32_t full_l2p_fc(uint32_t logical, int fc, int *valid);
uint8_t *psmac_mmu_ram_ptr_fc(uint32_t logical, int fc)
{
    if (!(psmac_mmu_full && translate_on)) return psmac_mmu_ram_ptr(logical);
    int valid;
    uint32_t p = full_l2p_fc(logical, fc, &valid);
    if (!valid) return 0;
    p = norm_phys(p);
    if (p >= ram_size) return 0;
    return (uint8_t *)(KERNEL_ALIAS + ram_phys + p);
}

/* logical -> physical for the abort handler; returns 0 and *valid=0 for unmapped pages */
static uint32_t full_l2p(uint32_t logical, int *valid);
uint32_t psmac_mmu_l2p(uint32_t logical, int *valid)
{
    if (unlikely(psmac_mmu_full) && translate_on) return full_l2p(logical, valid);
    if (!translate_on || !l2p) { *valid = 1; return logical; }
    uint32_t idx = (is_bits ? (logical & ((1u << (32 - is_bits)) - 1)) : logical) >> 12;
    uint32_t e = l2p[idx];
    if (!(e & L2P_VALID)) { *valid = 0; return logical; }
    *valid = 1;
    return (e & ~0xfffu) | (logical & 0xfff);
}

void psmac_pmove(uint32_t ea, uint32_t ext)
{
    int fmt = (ext >> 13) & 7, preg = (ext >> 10) & 7, rw = (ext >> 9) & 1;   /* ext: the extension word, flags above it */
    switch (fmt) {
    case 1: /* PFLUSH / PFLUSHA (PLOAD never gets here); bit 16 of ext: right after a PMOVE to TC */
        pflush((ext >> 16) & 1);
        break;
    case 2: /* TC, SRP, CRP */
        if (preg == 0) {
            if (rw) wr32(ea, mmu.tc);
            else { uint32_t old = mmu.tc; mmu.tc = rd32(ea); if (old != mmu.tc || crp_dirty) { crp_dirty = 0; rebuild(); } }
        } else if (preg == 2) {
            if (rw) { wr32(ea, mmu.srp_hi); wr32(ea + 4, mmu.srp_lo); } else { mmu.srp_hi = rd32(ea); mmu.srp_lo = rd32(ea + 4); }
        } else if (preg == 3) {
            if (rw) { wr32(ea, mmu.crp_hi); wr32(ea + 4, mmu.crp_lo); }
            else {
                uint32_t oh = mmu.crp_hi, ol = mmu.crp_lo;
                mmu.crp_hi = rd32(ea); mmu.crp_lo = rd32(ea + 4);
                /* The ROM loads CRP then TC from one record (SwapMMUMode: ploadr 8(a0); pmove (a0),crp;
                   pmove 8(a0),tc; pflusha). Between the two loads the record is only reachable through the
                   translation the 68030 still holds in its ATC, so the switch is deferred until the TC load
                   or the PFLUSH, which is also where the real chip starts using the new tables. */
                if ((mmu.tc & 0x80000000u) && (oh != mmu.crp_hi || ol != mmu.crp_lo)) crp_dirty = 1;
            }
        } else kprintf("[PSMAC:MMU] PMOVE fmt2 preg %d unsupported\n", preg);
        break;
    case 0: /* TT0 / TT1 */
        if (preg == 2) { if (rw) wr32(ea, mmu.tt0); else mmu.tt0 = rd32(ea); }
        else if (preg == 3) { if (rw) wr32(ea, mmu.tt1); else mmu.tt1 = rd32(ea); }
        else kprintf("[PSMAC:MMU] PMOVE fmt0 preg %d unsupported\n", preg);
        break;
    case 3: /* MMUSR */
        if (rw) *(volatile uint16_t *)(uintptr_t)ea = mmu.mmusr; else mmu.mmusr = *(volatile uint16_t *)(uintptr_t)ea;
        break;
    default:
        kprintf("[PSMAC:MMU] PMOVE ext %04x unsupported\n", ext);
    }
}

/* =====================================================================================================
   mac.mmu=full, phase 2 (25-26 November 2026, docs/23-mmu.md section 11): the 68030 PMMU, complete, on a
   soft TLB. Everything below runs only with the option; the stock path above is untouched by it.

   The model. The guest's own tables are the truth, read one address at a time by pwalk(), the 68030's
   table search (MC68030UM section 9.5): the root pointer (CRP, or SRP for supervisor accesses when TC.SRE
   is set), an optional function-code level (TC.FCL), up to four levels of short or long descriptors,
   limits, early termination, indirect descriptors at the last level, WP and S gathered along the way, U
   set in every descriptor fetched and M in the page descriptor on a write. TT0/TT1 are matched first.
   The ARM page tables of a configuration ("set", as before) play the 68030's ATC: empty when the set is
   made, a 4 KiB page mapped on its first access (an ARM data abort, psmac_mmu_access from the abort
   path), dropped again by the PFLUSH that names it. A page whose descriptor has M clear is mapped
   read-only until its first write, which faults, sets M and maps it writable, as a real 030 marks a
   page dirty; WP, and S for a user access, make the access a bus error. Pages that are not RAM (the
   slots' I/O, the ROM written to, the frame buffer, docs/10) go on being emulated through psmac_bus, now
   translated by the walk (a small cache in front, l2pc).
   ===================================================================================================== */
int  mmu_low4g_page(uintptr_t virt, uintptr_t phys, uint32_t attr_low, int tlbi);
void mmu_low4g_flush(void);
extern uint32_t psmac_icache_epoch;

#define MMUSR_B 0x8000u
#define MMUSR_L 0x4000u
#define MMUSR_S 0x2000u
#define MMUSR_W 0x0800u
#define MMUSR_I 0x0400u
#define MMUSR_M 0x0200u
#define MMUSR_T 0x0040u
#define TC_E    0x80000000u
#define TC_SRE  0x02000000u
#define TC_FCL  0x01000000u
#define PW_WRITE  1         /* the access is a write */
#define PW_UPDATE 2         /* a real access's table search: set U, and M for a write */

struct pwalk {
    uint32_t pa;            /* the physical address of the logical one */
    uint32_t desc;          /* physical address of the last descriptor fetched (PTEST's An) */
    uint16_t mmusr;         /* as PTEST reports it */
    uint8_t  span_bits;     /* log2 of the bytes the final page descriptor covers (32: all of it) */
    uint8_t  levels, valid, wp, s, m, ci, tt;
};

static struct {
    uint32_t walks, maps, mapped_pages, m_faults, emulated, tt_hits, buserrs;
    uint32_t pflush_all, pflush_fc, pflush_ea, pflush_kept, pflush_dropped, pload, ptest, pmove, switches, epoch_bumps;
    uint32_t spaces, evictions, moves;
    uint64_t flush_ticks, switch_ticks;
} fs;                                           /* the soft TLB's counters (psmac_mmu_full_stats) */

static inline int desc_in_ram(uint32_t p) { p = norm_phys(p); return ram_size >= 4 && p <= ram_size - 4; }
static inline volatile uint32_t *desc_at(uint32_t p) { return (volatile uint32_t *)(KERNEL_ALIAS + ram_phys + norm_phys(p)); }

/* TT0/TT1 (MC68030UM 9.7.3): logical address base and mask (bits 31-24, 23-16), E, CI, R/W and RWM, the
   function code base and mask. A match is translated one to one with no table search. */
static int tt_match(uint32_t tt, uint32_t la, int fc, int write)
{
    if (!(tt & 0x8000u)) return 0;
    uint32_t base = tt >> 24, mask = (tt >> 16) & 0xff;
    if (((la >> 24) & ~mask & 0xff) != (base & ~mask & 0xff)) return 0;
    uint32_t fcb = (tt >> 4) & 7, fcm = tt & 7;
    if (((uint32_t)fc & ~fcm & 7) != (fcb & ~fcm & 7)) return 0;
    if (!(tt & 0x100u)) {                       /* RWM clear: only the cycles R/W names */
        int rw = (tt >> 9) & 1;                 /* 1: reads */
        if (rw ? write : !write) return 0;
    }
    return 1;
}

/* The 68030's table search for one logical address, 'maxlevel' levels at most (PTEST's level, 1-7; more
   for an access). Never raises anything: the result says what an access would meet. */
static void pwalk(uint32_t la, int fc, int flags, int maxlevel, struct pwalk *r)
{
    r->pa = 0; r->desc = 0; r->mmusr = 0; r->span_bits = 32; r->levels = 0;
    r->valid = r->wp = r->s = r->m = r->ci = r->tt = 0;
    fs.walks++;
    uint32_t tc = mmu.tc;
    if (!(tc & TC_E)) { r->pa = la; r->valid = 1; return; }
    uint32_t is = (tc >> 16) & 15, ti[4] = { (tc >> 12) & 15, (tc >> 8) & 15, (tc >> 4) & 15, tc & 15 };
    int super = (fc & 4) != 0;
    uint32_t rhi = mmu.crp_hi, rlo = mmu.crp_lo;
    if ((tc & TC_SRE) && super) { rhi = mmu.srp_hi; rlo = mmu.srp_lo; }
    uint32_t dt = rhi & 3, used = is;
    uint32_t lim = (rhi >> 16) & 0x7fff; int lower = (rhi >> 31) & 1, lim_on = 1;
    if (dt == 0) { r->mmusr = MMUSR_I; return; }
    if (dt == 1) {                              /* the root pointer is a page descriptor: early termination at once */
        r->span_bits = 32 - is;
        r->pa = (rlo & 0xffffff00u) + (is ? (la & ((1u << (32 - is)) - 1)) : la);
        r->valid = 1;
        return;
    }
    uint32_t taddr = rlo & 0xfffffff0u; int dsize = dt == 3 ? 8 : 4;
    int bits[5], nl = 0;
    if (tc & TC_FCL) bits[nl++] = -1;           /* the function-code level, indexed by FC, takes no address bits */
    for (int i = 0; i < 4 && ti[i]; i++) bits[nl++] = (int)ti[i];
    for (int L = 0; L < nl; L++) {
        uint32_t idx;
        if (bits[L] < 0) idx = (uint32_t)fc & 7;
        else { idx = used >= 32 ? 0 : (uint32_t)(la << used) >> (32 - bits[L]); used += (uint32_t)bits[L]; }
        if (lim_on && (lower ? idx < lim : idx > lim)) { r->mmusr = MMUSR_L | MMUSR_I; goto out; }
        if (r->levels >= maxlevel) goto out;
        uint32_t dp = taddr + idx * (uint32_t)dsize;
        if (!desc_in_ram(dp) || (dsize == 8 && !desc_in_ram(dp + 4))) { r->mmusr = MMUSR_B | MMUSR_I; goto out; }
        volatile uint32_t *d = desc_at(dp);
        uint32_t d0 = d[0], d1 = dsize == 8 ? *desc_at(dp + 4) : 0;
        r->levels++; r->desc = dp;
        uint32_t ddt = d0 & 3;
        if (ddt == 0) { r->mmusr = MMUSR_I; goto out; }
        if (dsize == 8 && (d0 & 0x100u)) r->s = 1;
        if (d0 & 4u) r->wp = 1;
        if (ddt != 1 && L == nl - 1) {          /* an indirect descriptor where a page descriptor belongs */
            if ((flags & PW_UPDATE) && !(d0 & 8u) && dsize == 8) d[0] = d0 | 8u;
            uint32_t ip = (dsize == 8 ? d1 : d0) & 0xfffffffcu; int isz = ddt == 3 ? 8 : 4;
            if (r->levels >= maxlevel) goto out;
            if (!desc_in_ram(ip) || (isz == 8 && !desc_in_ram(ip + 4))) { r->mmusr = MMUSR_B | MMUSR_I; goto out; }
            d = desc_at(ip); d0 = d[0]; d1 = isz == 8 ? *desc_at(ip + 4) : 0;
            r->levels++; r->desc = ip; dsize = isz;
            if ((d0 & 3) != 1) { r->mmusr = MMUSR_I; goto out; }
            if (dsize == 8 && (d0 & 0x100u)) r->s = 1;
            if (d0 & 4u) r->wp = 1;
            ddt = 1;
        }
        if (ddt == 1) {                         /* a page descriptor, maybe early termination */
            /* The 030 takes from the address field only the bits above the page size: the ones below are free, and
               System 7's VM keeps a flag of its own in bit 8 of a 4 KiB page's descriptor (ptch 42, $1ED8). The rest
               of the logical address within the descriptor's span is added, not concatenated: an early-terminated
               descriptor's base need only be page-aligned, and the IIci ROM's tables on a machine with RBV video map
               low memory through one whose base sits above the frame buffer (MAME's IIci: logical 0 is not physical
               0). The first version of this (26 November, for VM's flag) concatenated and sent MAME's IIci's low
               memory into the frame buffer. */
            uint32_t base = (dsize == 8 ? d1 : d0) & 0xffffff00u;
            uint32_t ps = (tc >> 20) & 15, sb = 32 - used;
            uint32_t pmask = (1u << ps) - 1;
            uint32_t lowmask = sb >= 32 ? 0xffffffffu : ((1u << sb) - 1);
            r->span_bits = (uint8_t)(32 - used);
            r->pa = (base & ~pmask) + (la & lowmask);
            if (flags & PW_UPDATE) {
                uint32_t nd = d0 | 8u;
                if ((flags & PW_WRITE) && !r->wp && !(r->s && !super)) nd |= 0x10u;
                if (nd != d0) d[0] = nd;
                d0 = nd;
            }
            r->m = (d0 & 0x10u) != 0; r->ci = (d0 & 0x40u) != 0;
            r->valid = 1;
            goto out;
        }
        if ((flags & PW_UPDATE) && !(d0 & 8u)) d[0] = d0 | 8u;     /* a table descriptor: U, next level */
        taddr = (dsize == 8 ? d1 : d0) & 0xfffffff0u;
        if (dsize == 8) { lim = (d0 >> 16) & 0x7fff; lower = (d0 >> 31) & 1; lim_on = 1; } else lim_on = 0;
        dsize = ddt == 3 ? 8 : 4;
    }
    r->mmusr = MMUSR_I;                         /* ran out of levels on a table descriptor: malformed */
out:
    if (r->wp) r->mmusr |= MMUSR_W;
    if (r->s && !super) r->mmusr |= MMUSR_S;
    if (r->valid && r->m) r->mmusr |= MMUSR_M;
    r->mmusr |= r->levels & 7;
}

/* =====================================================================================================
   mac.mmu=full, phase 3 (26 November 2026, docs/23-mmu.md section 12): address spaces and views.

   A "space" is the ARM side of one 68030 translation: its own L1 table and ASID (aarch64/mmu.c,
   mmu_space_*), keyed by TC, a root pointer and a mode. A configuration (TC, CRP, SRP, TT0, TT1) has a
   user view and a supervisor view; they are one space unless the two modes can see different memory:
   TC.SRE with an SRP other than the CRP, TC.FCL, a TT register that compares FC2, or a long descriptor's
   S bit met by the walk. Then they are two spaces, the user one keyed by the CRP, the supervisor one by
   the SRP (or the CRP again), and the 68k's S bit picks the one in TTBR0_EL1: the translated code switches
   at RTE, MOVE/ANDI/EORI to SR and STOP (psmac_mmu_emit_view), the C side at exceptions, interrupts and
   bus errors (psmac_view_for, psmac.h). A context switch (PMOVE to CRP) installs the spaces of the new
   root: TTBR0 and an ASID, no TLB flush, and the pages they already map are kept if the tables still say
   the same thing (revalidated, as a PFLUSHA does; the 68030 flushes its ATC at the PMOVE). Thirty-two
   spaces are kept, the least recently used one is emptied for a new root, and the code cache forgets
   the blocks it translated under it (psmac_jit_purge_space). With translation off the identity is a
   space too, mapped on demand like the others, so that core 0 runs on spaces only from the first one.
   ===================================================================================================== */
void *mmu_space_new(const uint64_t pristine[4]);
uint64_t mmu_space_ttbr(void *l1, unsigned asid);
void mmu_space_reset(void *l1, const uint64_t pristine[4]);
int mmu_space_page(void *l1, unsigned asid, uintptr_t virt, uintptr_t phys, uint32_t attr_low, int tlbi);
uint64_t mmu_space_entry(void *l1, uintptr_t virt);
void mmu_space_flush_asid(unsigned asid);
void mmu_space_install(uint64_t ttbr);
void mmu_space_leave_alias(void);
void psmac_jit_purge_space(uint64_t ttbr);                  /* M68k_Translator.c */

#define NSPACES 32
#define SUM_WORDS (PAGES_4K / 32 / 32)
struct space {
    uint32_t tc, rhi, rlo;          /* key: TC (0: translation off), the root pointer */
    uint8_t mode;                   /* key: 0 both modes, 1 the user view, 2 the supervisor view */
    uint8_t used, asid, idx, pinned;
    void *l1; uint64_t ttbr;
    uint32_t *bits, *sum;           /* the 4 KiB pages mapped: one bit each, and one bit per word of bits */
    uint32_t lru, mapped;
};
static struct space spaces[NSPACES];
static struct space *view[2];       /* the user and supervisor views of the current configuration */
static uint32_t space_clock;
static int spaces_on;               /* core 0 has left mmu_user_L1 for the spaces */
static int s_seen;                  /* a long descriptor's S bit has been met: the views must differ */
static int split_now;
struct psmac_mmu_views psmac_views; /* what the translated code switches between (psmac.h) */

#define RW_ATTR_NG (RW_ATTR | MMU_NG)
#define RO_ATTR_NG (RO_ATTR | MMU_NG)

static inline void sp_set(struct space *s, uint32_t i)
{
    uint32_t m = 1u << (i & 31);
    if (s->bits[i >> 5] & m) return;
    s->bits[i >> 5] |= m; s->sum[i >> 10] |= 1u << ((i >> 5) & 31); s->mapped++;
}
static inline void sp_clr(struct space *s, uint32_t i)
{
    uint32_t m = 1u << (i & 31);
    if (!(s->bits[i >> 5] & m)) return;
    s->bits[i >> 5] &= ~m; s->mapped--;
    if (!s->bits[i >> 5]) s->sum[i >> 10] &= ~(1u << ((i >> 5) & 31));
}
static inline int sp_get(const struct space *s, uint32_t i) { return (s->bits[i >> 5] >> (i & 31)) & 1; }

/* the space that holds a function code's translations */
static inline struct space *space_for_fc(int fc) { return view[(split_now && (fc & 4)) ? 1 : 0]; }

/* the view TTBR0 holds now: 1 supervisor, 0 user, -1 when the views are one space (then the SR says). Another core
   (the mailbox's translations) runs on mmu_user_L1: the supervisor's view for it */
int psmac_mmu_cur_view(void)
{
    if (!split_now) return -1;
    uint64_t t; __asm__ volatile("mrs %0, TTBR0_EL1" : "=r"(t));
    return t == psmac_views.ttbr[0] ? 0 : 1;
}

static int tt_splits(uint32_t tt) { return (tt & 0x8000u) && !(tt & 4u); }   /* enabled, FC2 compared */
static int need_split(void)
{
    if (!(mmu.tc & TC_E)) return 0;
    if ((mmu.tc & TC_SRE) && (mmu.srp_hi != mmu.crp_hi || mmu.srp_lo != mmu.crp_lo)) return 1;
    return (mmu.tc & TC_FCL) || tt_splits(mmu.tt0) || tt_splits(mmu.tt1) || s_seen;
}

/* ---- the soft TLB's logical -> physical cache for the emulated accesses and the kernel's readers; an entry
   names the space it belongs to in its low bits ---- */
#define L2PC_N 512
static volatile uint64_t l2pc[L2PC_N];          /* (logical page << 32) | physical page | space << 1 | 1; 0 = empty */
static void l2pc_flush(void) { for (int i = 0; i < L2PC_N; i++) l2pc[i] = 0; }
static inline void l2pc_put(const struct space *s, uint32_t la, uint32_t pa)
{
    l2pc[(la >> 12) & (L2PC_N - 1)] = ((uint64_t)(la & ~0xfffu) << 32) | (pa & ~0xfffu) | ((uint32_t)s->idx << 1) | 1u;
}
static inline int l2pc_get(const struct space *s, uint32_t la, uint32_t *pa)
{
    uint64_t e = l2pc[(la >> 12) & (L2PC_N - 1)];
    if (!(e & 1) || (uint32_t)(e >> 32) != (la & ~0xfffu) || ((e >> 1) & 0x3f) != s->idx) return 0;
    *pa = ((uint32_t)e & ~0xfffu) | (la & 0xfffu);
    return 1;
}

/* where the emulated accesses go (the heartbeat shows the busiest pages): 256 slots by logical page */
static struct { uint32_t la, pa, n, w; } emu_hist[256];
static void emu_note(uint32_t la, uint32_t pa, int write)
{
    unsigned i = (la >> 12) & 255;
    if (emu_hist[i].la != (la & ~0xfffu)) { emu_hist[i].la = la & ~0xfffu; emu_hist[i].pa = pa & ~0xfffu; emu_hist[i].n = 0; emu_hist[i].w = 0; }
    emu_hist[i].n++; emu_hist[i].w += write;
}

/* The spaces carry none of the Pi's windows (docs/23 section 21): translated code runs at the kernel's own exception
   level, so a window mapped in a space is memory the 68k reaches past its tables, and A/UX's Macintosh environment
   keeps its programs' stacks at $3FFFFxxx, where the Pi's peripherals are. The kernel's own code reaches the
   peripherals and the VideoCore's memory through its mappings in the kernel's half (PSMAC_PERIPH_HI, KERNEL_ALIAS). */
static const uint64_t no_windows[4];
void mmu_low4g_describe(const uint64_t l1[4]);

/* Map one 4 KiB logical page of a space; 0 if its physical page is not memory the ARM can map */
static int sp_map4k(struct space *s, uint32_t la, uint32_t pa, int writable, int tlbi)
{
    la &= ~0xfffu; pa &= ~0xfffu;
    uintptr_t pi = pi_phys(pa);
    if (!pi) return 0;
    if (ro_phys(pa)) writable = 0;
    mmu_space_page(s->l1, s->asid, la, pi, writable ? RW_ATTR_NG : RO_ATTR_NG, tlbi);
    sp_set(s, la >> 12);
    fs.mapped_pages++;
    return 1;
}
static void sp_unmap4k(struct space *s, uint32_t la, int tlbi)
{
    if (!sp_get(s, la >> 12)) return;
    mmu_space_page(s->l1, s->asid, (uintptr_t)la & ~0xfffull, ~(uintptr_t)0, 0, tlbi);
    sp_clr(s, la >> 12);
}

/* ---- spaces ---- */
static void space_install(struct space *s)
{
    mmu_space_install(s->ttbr);
    psmac_views.cur = s->ttbr;
    if (!spaces_on) {
        /* the first one: from here on core 0 never runs on mmu_user_L1 again, whose entries for the 68k's space
           (the stock identity set, global) leave the TLB now, with the kernel's alias of them */
        spaces_on = 1;
        { extern uintptr_t psmac_periph_base; psmac_periph_base = (uintptr_t)PSMAC_PERIPH_HI; }   /* support_rpi.c: the mailbox */
        mmu_space_leave_alias();
        kprintf("[PSMAC:MMU] address spaces on: TTBR0 %016llx (ASID %u); the kernel's alias of the 68k's first 4 GiB is "
                "emptied, accesses through it are emulated\n", (unsigned long long)s->ttbr, s->asid);
    }
}

static struct space *space_get(uint32_t tc, uint32_t rhi, uint32_t rlo, int mode, int *fresh)
{
    extern void *tlsf;
    struct space *victim = NULL;
    *fresh = 0;
    for (int i = 0; i < NSPACES; i++) {
        struct space *s = &spaces[i];
        if (s->used && s->tc == tc && s->rhi == rhi && s->rlo == rlo && s->mode == mode) { s->lru = ++space_clock; return s; }
        if (!s->used) { if (!victim || victim->used) victim = s; }
        else if (!s->pinned && s != view[0] && s != view[1] && (!victim || (victim->used && s->lru < victim->lru))) victim = s;
    }
    if (!victim) return NULL;                  /* cannot happen: at most four spaces are pinned */
    struct space *s = victim;
    if (s->used) {
        fs.evictions++;
        if (fs.evictions <= 8 || (fs.evictions & 255) == 0)
            kprintf("[PSMAC:MMU]   space %u (ASID %u) reused: TC=%08x root %08x:%08x %s, %u pages, %u evictions so far\n", s->idx, s->asid,
                    s->tc, s->rhi, s->rlo, s->mode == 1 ? "user" : s->mode == 2 ? "supervisor" : "both modes", s->mapped, fs.evictions);
        mmu_space_reset(s->l1, no_windows);
        for (uint32_t k = 0; k < PAGES_4K / 32; k++) s->bits[k] = 0;
        for (uint32_t k = 0; k < SUM_WORDS; k++) s->sum[k] = 0;
        s->mapped = 0;
        psmac_jit_purge_space(s->ttbr);
    }
    if (!s->l1) {
        s->bits = tlsf_malloc(tlsf, PAGES_4K / 8);
        s->sum = tlsf_malloc(tlsf, SUM_WORDS * 4);
        if (!s->bits || !s->sum) { kprintf("[PSMAC:MMU] out of memory for space %u\n", s->idx); while (1) __asm__ volatile("wfi"); }
        for (uint32_t k = 0; k < PAGES_4K / 32; k++) s->bits[k] = 0;
        for (uint32_t k = 0; k < SUM_WORDS; k++) s->sum[k] = 0;
        s->l1 = mmu_space_new(no_windows);
        s->ttbr = mmu_space_ttbr(s->l1, s->asid);
        s->mapped = 0;
    }
    s->used = 1; s->tc = tc; s->rhi = rhi; s->rlo = rlo; s->mode = (uint8_t)mode; s->lru = ++space_clock;
    fs.spaces++;
    *fresh = 1;
    return s;
}

/* ---- revalidation (PFLUSH, and a space installed again: the 68030 flushes its ATC at a PMOVE to CRP, SRP or TC)
   Keep a mapped page only if the descriptor still says what the mapping says: the same frame, U still set (a guest
   that clears U wants to see the next access), M set if it is mapped writable, no WP, and not a supervisor page in a
   user view or a view both modes share. A page whose frame changed may hold other code: the code cache is told
   (psmac_icache_epoch, its blocks are checked again by checksum on their next entry). ---- */
static int reval_page(struct space *s, int fc, uint32_t la, struct pwalk *r, uint32_t *span_lo, uint32_t *span_hi)
{
    uint64_t e = mmu_space_entry(s->l1, la);
    if (!(e & 1)) return 0;
    uintptr_t had = (uintptr_t)(e & 0x0000fffffffff000ull);
    if (tt_match(mmu.tt0, la, fc, 0) || tt_match(mmu.tt1, la, fc, 0))       /* transparent: one to one */
        return had == pi_phys(la & ~0xfffu);
    if (!(la >= *span_lo && la < *span_hi)) {
        pwalk(la, fc, 0, 99, r);
        if (r->valid && r->span_bits >= 12) {
            uint32_t sz = r->span_bits >= 32 ? 0 : (1u << r->span_bits);
            *span_lo = sz ? la & ~(sz - 1) : 0; *span_hi = sz ? *span_lo + sz : 0xffffffffu;
            r->pa -= la - *span_lo;                 /* pa of the span's start, below */
        } else { *span_lo = 1; *span_hi = 0; }
    }
    /* A page that is no longer valid may come back with other contents and no instruction-cache clear in between (A/UX's
       exec replaces a process's text in place, its pages paged in again on demand): the code cache must check its blocks
       before it runs them again, as it does for a page on a new frame (26 November 2026, docs/23 section 15) */
    if (!r->valid || r->span_bits < 12) { psmac_icache_epoch++; fs.epoch_bumps++; return 0; }
    if (r->s && (!(fc & 4) || !split_now)) return 0;
    uint32_t pa = r->pa + (la - *span_lo);
    if (pi_phys(pa & ~0xfffu) != had) { psmac_icache_epoch++; fs.epoch_bumps++; return 0; }
    if (r->desc && !(*desc_at(r->desc) & 8u)) return 0;     /* U cleared by the guest */
    int rw = !(e & (1ull << 7));                 /* AP[2] clear: writable */
    if (rw && (!r->m || r->wp)) return 0;        /* M cleared, or write protected since */
    return 1;
}
static void space_flush(struct space *s, int fc)
{
    if (!s || !s->mapped) return;
    if (!s->tc) return;                          /* the identity never changes */
    uint64_t t0 = cnt_now();
    uint32_t kept = 0, dropped = 0, lo = 1, hi = 0;
    struct pwalk r;
    for (uint32_t w1 = 0; w1 < SUM_WORDS; w1++) {
        uint32_t sb = s->sum[w1];
        while (sb) {
            uint32_t w = (w1 << 5) | (uint32_t)__builtin_ctz(sb); sb &= sb - 1;
            uint32_t b = s->bits[w];
            while (b) {
                uint32_t la = ((w << 5) | (uint32_t)__builtin_ctz(b)) << 12; b &= b - 1;
                if (reval_page(s, fc, la, &r, &lo, &hi)) { kept++; continue; }
                sp_unmap4k(s, la, 0); dropped++;
            }
        }
    }
    if (dropped) mmu_space_flush_asid(s->asid);
    { void mmu_tlb_paranoid(void); mmu_tlb_paranoid(); }
    l2pc_flush();
    fs.pflush_kept += kept; fs.pflush_dropped += dropped;
    fs.flush_ticks += cnt_now() - t0;
}
static void space_flush_page(struct space *s, int fc, uint32_t ea)
{
    if (!s || !s->tc) return;
    uint32_t ps = (mmu.tc >> 20) & 15, sz = ps >= 12 ? (1u << ps) : 0x1000u;
    uint32_t lo = 1, hi = 0, base = ea & ~(sz - 1);
    struct pwalk r;
    for (uint32_t o = 0; o < sz; o += 0x1000u) {
        uint32_t la = base + o;
        if (!sp_get(s, la >> 12)) continue;
        if (reval_page(s, fc, la, &r, &lo, &hi)) { fs.pflush_kept++; continue; }
        sp_unmap4k(s, la, 1); fs.pflush_dropped++;
    }
    { void mmu_tlb_paranoid(void); mmu_tlb_paranoid(); }
    l2pc_flush();
}
/* the views a PFLUSH's function code and mask name (an entry's FC matches when (FC & mask) == (fc & mask)) */
static int view_matches(int v, int fc, int mask)
{
    static const uint8_t user_fcs[2] = { 1, 2 }, super_fcs[2] = { 5, 6 };
    const uint8_t *f = v ? super_fcs : user_fcs;
    for (int i = 0; i < 2; i++)
        if (((f[i] ^ fc) & mask & 7) == 0) return 1;
    if (!split_now)                              /* one space for both: either mode's codes */
        for (int i = 0; i < 2; i++)
            if (((super_fcs[i] ^ fc) & mask & 7) == 0) return 1;
    return 0;
}
static void views_flush(int fc, int mask, int by_page, uint32_t ea)
{
    for (int v = 0; v < 2; v++) {
        if (v == 1 && !split_now) break;
        if (!view_matches(v, fc, mask)) continue;
        if (by_page) space_flush_page(view[v], v ? 5 : 1, ea);
        else space_flush(view[v], v ? 5 : 1);
    }
}

/* ---- a new configuration (PMOVE to TC, CRP, SRP, TT0, TT1; the deferred CRP; the views changing) ----
   fd: PMOVE's FD bit, no flush; super: the 68k's S bit now (the view to install) */
static void views_update(int super, int fd)
{
    uint64_t t0 = cnt_now();
    uint32_t tc = mmu.tc;
    int on = (tc & TC_E) != 0;
    if (on) {
        uint32_t ps = (tc >> 20) & 15, is = (tc >> 16) & 15;
        uint32_t sum = ps + is + ((tc >> 12) & 15) + ((tc >> 8) & 15) + ((tc >> 4) & 15) + (tc & 15);
        if (ps < 8 || ((tc >> 12) & 15) == 0 || sum != 32 || (mmu.crp_hi & 3) == 0) {
            /* a real 68030 takes an MMU configuration exception (vector 56) here; nothing has asked for one yet */
            kprintf("[PSMAC:MMU] TC=%08x CRP=%08x:%08x is not a valid configuration (fields %u/%u/%u/%u/%u/%u), keeping the current map\n",
                    tc, mmu.crp_hi, mmu.crp_lo, ps, is, (tc >> 12) & 15, (tc >> 8) & 15, (tc >> 4) & 15, tc & 15);
            return;
        }
        is_bits = is;
    } else is_bits = 0;
    translate_on = on;
    rebuilds++; fs.switches++;
    int split = on && need_split();
    struct space *o0 = view[0], *o1 = view[1];
    if (o0) o0->pinned = 1;
    if (o1) o1->pinned = 1;
    int f0 = 0, f1 = 0;
    struct space *u = space_get(on ? tc : 0, on ? mmu.crp_hi : 0, on ? mmu.crp_lo : 0, split ? 1 : 0, &f0);
    u->pinned = 1;
    struct space *s = u;
    if (split) {
        int sre = (tc & TC_SRE) != 0;
        s = space_get(tc, sre ? mmu.srp_hi : mmu.crp_hi, sre ? mmu.srp_lo : mmu.crp_lo, 2, &f1);
    }
    if (o0) o0->pinned = 0;
    if (o1) o1->pinned = 0;
    u->pinned = 0;
    view[0] = u; view[1] = s; split_now = split;
    if (!fd) {
        if (!f0) space_flush(u, split ? 1 : 5);
        if (split && !f1) space_flush(s, 5);
    }
    l2pc_flush();
    psmac_views.ttbr[0] = u->ttbr; psmac_views.ttbr[1] = s->ttbr; psmac_views.split = (uint32_t)split;
    space_install(super ? s : u);
    fs.switch_ticks += cnt_now() - t0;
    if (f0 || f1 || fs.switches <= 8 || (fs.switches & 1023) == 0) {
        kprintf("[PSMAC:MMU] switch #%u: TC=%08x CRP=%08x:%08x", rebuilds, tc, mmu.crp_hi, mmu.crp_lo);
        if (tc & TC_SRE) kprintf(" SRE SRP=%08x:%08x", mmu.srp_hi, mmu.srp_lo);
        if (split) kprintf(" -> user space %u (ASID %u, %s), supervisor space %u (ASID %u, %s)", u->idx, u->asid, f0 ? "new" : "cached",
                           s->idx, s->asid, f1 ? "new" : "cached");
        else kprintf(" -> space %u (ASID %u, %s, both modes)", u->idx, u->asid, f0 ? "new" : "cached");
        kprintf("%s (%u us)\n", fd ? ", FD" : "", (uint32_t)((cnt_now() - t0) * 1000000 / cnt_frq()));
    }
    if ((f0 || f1) && fs.spaces <= 4) mmu_print_stats();
    last_install = cnt_now();
}

/* The fault side (psmac_mmufault.c): an ARM data abort on a logical address with mac.mmu=full.
   fc: the 68030 function code of the access (1 user data, 5 supervisor data, 2/6 program); it picks the view.
   Returns 1 when the page is now mapped (the access is retried), 0 when it is to be emulated (I/O, a write to
   the ROM or the frame buffer, pages below 4 KiB, a supervisor page while the views are one; the translation is
   in l2pc for psmac_mmu_l2p), -1 when the 68030 would take a bus error (*mmusr says why). */
int psmac_mmu_access(uint32_t la, int fc, int write, uint16_t *mmusr)
{
    struct space *sp = space_for_fc(fc);
    if (!sp) return 0;
    {   /* the slots' I/O, and writes to the frame buffer and the ROM, are emulated on every access: once their page
           has been walked (U set, M too for a write) the cache answers */
        uint32_t pa;
        if (l2pc_get(sp, la, &pa) && (!pi_phys(pa & ~0xfffu) || (write && ro_phys(pa & ~0xfffu)))) { fs.emulated++; emu_note(la, pa, write); return 0; }
    }
    if (!(mmu.tc & TC_E)) {                         /* translation off: the identity, mapped page by page */
        l2pc_put(sp, la, la);
        if (!pi_phys(la & ~0xfffu) || (write && ro_phys(la & ~0xfffu))) { fs.emulated++; emu_note(la, la, write); return 0; }
        if (!sp_map4k(sp, la, la, 1, 1)) return 0;
        fs.maps++;
        return 1;
    }
    if (tt_match(mmu.tt0, la, fc, write) || tt_match(mmu.tt1, la, fc, write)) {
        fs.tt_hits++;
        l2pc_put(sp, la, la);
        /* a TT that matches reads only is mapped read-only: a write then goes through the tables */
        int ro = !(tt_match(mmu.tt0, la, fc, 1) || tt_match(mmu.tt1, la, fc, 1));
        if (ro && write) return 0;
        if (!pi_phys(la & ~0xfffu) || (write && ro_phys(la & ~0xfffu))) { fs.emulated++; return 0; }
        return sp_map4k(sp, la, la, !ro, 1) ? 1 : 0;
    }
    struct pwalk r;
    pwalk(la, fc, PW_UPDATE | (write ? PW_WRITE : 0), 99, &r);
    if (mmusr) *mmusr = r.mmusr;
    if (!r.valid || (r.s && !(fc & 4)) || (write && r.wp)) { fs.buserrs++; return -1; }
    if (r.span_bits < 12) { fs.emulated++; return 0; }          /* a page below 4 KiB: every access emulated */
    if (r.s && !split_now) {
        /* a supervisor-only page while both modes share one space: emulated, so that user mode cannot reach it
           through the mapping, until the next flush or switch gives the modes a space each */
        if (!s_seen) { s_seen = 1; kprintf("[PSMAC:MMU] a long descriptor's S bit at %08x: user and supervisor get a space each from the next flush\n", la); }
        l2pc_put(sp, la, r.pa); fs.emulated++; return 0;
    }
    l2pc_put(sp, la, r.pa);
    uint32_t pa4 = r.pa & ~0xfffu;
    if (!pi_phys(pa4) || (write && ro_phys(pa4))) { fs.emulated++; emu_note(la, r.pa, write); return 0; }
    int writable = !r.wp && r.m;
    if (write) fs.m_faults += sp_get(sp, la >> 12);       /* mapped read-only until now: the first write */
    /* the chunk around la that the same descriptor covers, at most 32 KiB, in one go */
    uint32_t span = r.span_bits >= 15 ? 0x8000u : (1u << r.span_bits);
    uint32_t c0 = la & ~(span - 1), p0 = r.pa - (la - c0);
    for (uint32_t o = 0; o < span; o += 0x1000u) {
        uint32_t pa = p0 + o;
        if (o != (la & ~0xfffu) - c0 && !pi_phys(pa & ~0xfffu)) continue;
        sp_map4k(sp, c0 + o, pa, writable, 1);
    }
    fs.maps++;
    return sp_get(sp, la >> 12) ? 1 : 0;
}

/* psmac_bus.c, mac.mmu=full: an access the abort path emulates whose page is memory (the kernel's alias of the
   68k's space, a supervisor page shared by both modes): done here, on the Pi's copy. 0: not memory */
int psmac_mmu_ram_access(uint32_t pa, int size, uint64_t *v, uint64_t *v2, int write)
{
    if (!psmac_mmu_full || size > 16) return 0;
    uintptr_t pi = pi_phys(pa & ~0xfffu);
    if (!pi || (write && ro_phys(pa & ~0xfffu)) || (pa & 0xfffu) + (uint32_t)size > 0x1000u) return 0;
    volatile uint8_t *p = (volatile uint8_t *)(KERNEL_ALIAS + pi + (pa & 0xfffu));
    switch (size) {
    case 1: if (write) *p = (uint8_t)*v; else *v = *p; break;
    case 2: if (write) *(volatile uint16_t *)p = (uint16_t)*v; else *v = *(volatile uint16_t *)p; break;
    case 4: if (write) *(volatile uint32_t *)p = (uint32_t)*v; else *v = *(volatile uint32_t *)p; break;
    case 8: if (write) *(volatile uint64_t *)p = *v; else *v = *(volatile uint64_t *)p; break;
    default:
        if (write) { *(volatile uint64_t *)p = *v; *(volatile uint64_t *)(p + 8) = v2 ? *v2 : 0; }
        else { *v = *(volatile uint64_t *)p; if (v2) *v2 = *(volatile uint64_t *)(p + 8); }
        break;
    }
    return 1;
}

/* ---- the instructions ---- */
static inline uint32_t *r68(uint64_t *regs, int n)       /* the 68k's D0-D7/A0-A7 in the saved x registers (big-endian) */
{
    static const uint8_t map[16] = { 19, 20, 21, 22, 23, 24, 25, 26, 13, 14, 15, 16, 17, 27, 28, 29 };
    return (uint32_t *)&regs[map[n & 15]] + 1;
}
static int pmmu_fc(uint32_t f, uint64_t *regs)
{
    if ((f & 0x18) == 0x10) return (int)(f & 7);               /* #immediate */
    if ((f & 0x18) == 0x08) return (int)(*r68(regs, (int)(f & 7)) & 7);   /* Dn */
    if (f == 0 || f == 1) { extern int psmac_m68k_fcreg(int dfc); return psmac_m68k_fcreg((int)f); }   /* SFC, DFC */
    return 5;
}

/* [a, a + len) reachable by the 68k at function code fc (for a write if write)? 0 and *bad = the first
   address that is not: for the C helpers (PMOVE's operand here, the translator's instruction fetches), which
   check before they touch memory, so that a fault becomes the 68030's bus error instead of a hole in the bus */
int psmac_mmu_probe(uint32_t a, uint32_t len, int fc, int write, uint32_t *bad)
{
    if (!psmac_mmu_full || !translate_on || !view[0] || !len) return 1;
    uint32_t ps = (mmu.tc >> 20) & 15, step = ps < 12 ? (1u << ps) : 0x1000u;
    uint32_t end = a + len - 1, p = a;
    for (;;) {
        if (!(tt_match(mmu.tt0, p, fc, write) || tt_match(mmu.tt1, p, fc, write))) {
            struct pwalk r;
            pwalk(p, fc, 0, 99, &r);
            if (!r.valid || (r.s && !(fc & 4)) || (write && r.wp)) { *bad = p; return 0; }
        }
        uint32_t next = (p & ~(step - 1)) + step;
        if (next == 0 || next > end) break;
        p = next;
    }
    return 1;
}

/* the physical address of an access at function code fc, as the 68030 would make it (U set, M for a write), for
   MOVES; -1 for a bus error (*mmusr why), 0 with *pa */
static int fc_translate(uint32_t la, int fc, int write, uint32_t *pa)
{
    if (!(mmu.tc & TC_E) || tt_match(mmu.tt0, la, fc, write) || tt_match(mmu.tt1, la, fc, write)) { *pa = la; return 0; }
    struct pwalk r;
    pwalk(la, fc, PW_UPDATE | (write ? PW_WRITE : 0), 99, &r);
    if (!r.valid || (r.s && !(fc & 4)) || (write && r.wp)) return -1;
    *pa = r.pa;
    return 0;
}

uint32_t psmac_mmu_deliver_helper(uint64_t *regs, uint32_t sr, uint32_t pc, uint32_t addr, int write, int size, int fc);
uint32_t psmac_io_read(uint32_t addr, int size);
void psmac_io_write(uint32_t addr, int size, uint32_t value, int wait);

/* MOVES with mac.mmu=full (M68k_LINE0.c, EMIT_MOVES_full): the access goes to the address space SFC (a read) or DFC
   (a write) names, whichever view is installed: through the tables at that function code, not through the ARM
   mapping. FC 7 is CPU space, where nothing on a Macintosh answers a data cycle: a bus error. A page the tables
   refuse is a bus error with that function code in the SSW. op: opcode << 16 | extension word; ea: the address
   the block computed for the control modes; regs and the return value as psmac_pmmu_exec's. */
uint32_t psmac_moves_exec(uint32_t ea, uint32_t op, uint64_t *regs, uint32_t sr, uint32_t len)
{
    uint16_t opcode = op >> 16, ext = op & 0xffff;
    uint32_t pc = (uint32_t)regs[18];
    int mode = (opcode >> 3) & 7, rn = opcode & 7, sz = (opcode >> 6) & 3, size = sz == 0 ? 1 : sz == 1 ? 2 : 4;
    int write = (ext >> 11) & 1, reg = ext >> 12;
    extern int psmac_m68k_fcreg(int dfc);
    int fc = psmac_m68k_fcreg(write);
    uint32_t a = ea, step = (mode == 3 || mode == 4) ? ((rn == 7 && size == 1) ? 2u : (uint32_t)size) : 0;
    if (mode == 3) a = *r68(regs, 8 + rn);
    if (mode == 4) a = *r68(regs, 8 + rn) - step;
    uint32_t val = 0;
    if (write) {
        val = *r68(regs, reg);
        if (reg == 8 + rn && mode == 3) val += step;         /* MOVES An,(An)+ / -(An): the 68030 stores the updated An */
        if (reg == 8 + rn && mode == 4) val -= step;
    }
    fs.moves++;
    if (fs.moves <= 8 || (fs.moves & 4095) == 0)
        kprintf("[PSMAC:MMU] MOVES #%u at %08x: %s %08x fc %d size %d\n", fs.moves, pc, write ? "write" : "read", a, fc, size);
    uint32_t pa[2]; int n = 1;
    if (fc == 7 || fc_translate(a, fc, write, &pa[0]) < 0) {
        fs.buserrs++;
        return psmac_mmu_deliver_helper(regs, sr, pc, a, write, size, fc);
    }
    if (((a & 0xfffu) + (uint32_t)size) > 0x1000u) {           /* across a page */
        if (fc_translate((a + (uint32_t)size - 1) & ~0xfffu, fc, write, &pa[1]) < 0) {
            fs.buserrs++;
            return psmac_mmu_deliver_helper(regs, sr, pc, (a + (uint32_t)size - 1) & ~0xfffu, write, size, fc);
        }
        n = 2;
    }
    for (int i = 0; i < size; i++) {                          /* byte by byte, big-endian, each to its own page */
        uint32_t la = a + (uint32_t)i, p = (n == 2 && ((la ^ a) & ~0xfffu)) ? (pa[1] & ~0xfffu) | (la & 0xfffu) : pa[0] + (uint32_t)i;
        uintptr_t pi = pi_phys(p & ~0xfffu);
        int shift = 8 * (size - 1 - i);
        if (write) {
            uint8_t b = (uint8_t)(val >> shift);
            if (pi && !ro_phys(p & ~0xfffu)) *(volatile uint8_t *)(KERNEL_ALIAS + pi + (p & 0xfffu)) = b;
            else if (!pi) psmac_io_write(p, 1, b, 1);
        } else {
            uint8_t b = pi ? *(volatile uint8_t *)(KERNEL_ALIAS + pi + (p & 0xfffu)) : (uint8_t)psmac_io_read(p, 1);
            val |= (uint32_t)b << shift;
        }
    }
    if (!write) {
        uint32_t *d = r68(regs, reg);
        if (reg >= 8) *d = size == 1 ? (uint32_t)(int32_t)(int8_t)val : size == 2 ? (uint32_t)(int32_t)(int16_t)val : val;
        else *d = size == 1 ? ((*d & ~0xffu) | val) : size == 2 ? ((*d & ~0xffffu) | val) : val;
    }
    if (mode == 3) *r68(regs, 8 + rn) += step;
    if (mode == 4) *r68(regs, 8 + rn) -= step;
    if (!write && reg == 8 + rn && (mode == 3 || mode == 4))   /* MOVES (An)+,An: the loaded value wins */
        *r68(regs, reg) = size == 4 ? val : size == 2 ? (uint32_t)(int32_t)(int16_t)val : (uint32_t)(int32_t)(int8_t)val;
    regs[18] = pc + len;
    return 0;
}

/* mac.mmu=full: every 68030 MMU instruction, called from the block (M68k_LINEF.c, EMIT_PMMU_full) in supervisor
   mode (the block raised the privilege violation itself otherwise). regs: the block's AArch64 registers saved on
   the stack (the 68k's D0-D7 in x19-x26, A0-A4 in x13-x17, A5-A7 in x27-x29, the PC in x18), changed here where
   the instruction changes them. ea: the effective address for the control modes, computed by the block. op:
   opcode << 16 | extension word. lenflags: the instruction's length in bytes, and bit 16 for a PFLUSH right after
   a PMOVE to TC. Returns 0 with x18 at the next instruction, or SR | 0x10000 when the operand's page faulted and a
   bus error was delivered (x18 at the handler, the frame pushed). */
uint32_t psmac_pmmu_exec(uint32_t ea, uint32_t op, uint64_t *regs, uint32_t sr, uint32_t lenflags)
{
    uint16_t opcode = op >> 16, ext = op & 0xffff;
    uint32_t pc = (uint32_t)regs[18], len = lenflags & 0xff;
    int after_tc = (lenflags >> 16) & 1, mode = (opcode >> 3) & 7, rn = opcode & 7, top = ext >> 13;
    int dfc = (sr & 0x2000) ? 5 : 1;            /* the operand's function code: supervisor data */
    int super = (sr & 0x2000) != 0;
    static uint32_t logged;
    if (logged < 64 || (logged & 1023) == 0)
        kprintf("[PSMAC:MMU] %08x: %04x %04x ea %08x%s\n", pc, opcode, ext, ea, top == 4 ? " (PTEST)" : top == 1 ? (((ext >> 10) & 7) ? " (PFLUSH)" : " (PLOAD)") : " (PMOVE)");
    logged++;

    if (top == 1 || top == 4) {                 /* PLOAD, PFLUSH, PTEST: control modes only, the address is ea */
        int fc = pmmu_fc(ext & 0x1f, regs);
        if (top == 4) {
            int level = (ext >> 10) & 7, rd = (ext >> 9) & 1, an = (ext >> 5) & 7;
            uint16_t ms;
            fs.ptest++;
            if (level == 0) {                   /* the ATC alone: here, the ARM mapping of fc's view */
                struct space *sp = space_for_fc(fc);
                if (tt_match(mmu.tt0, ea, fc, !rd) || tt_match(mmu.tt1, ea, fc, !rd)) ms = MMUSR_T;
                else if (translate_on && sp && sp_get(sp, ea >> 12)) {
                    struct pwalk r; pwalk(ea, fc, 0, 99, &r);
                    ms = r.mmusr & (MMUSR_B | MMUSR_S | MMUSR_W | MMUSR_I | MMUSR_M);
                } else ms = MMUSR_I;
                if ((ext >> 8) & 1) *r68(regs, 8 + an) = 0;
            } else {
                struct pwalk r; pwalk(ea, fc, 0, level, &r);
                ms = r.mmusr;
                if ((ext >> 8) & 1) *r68(regs, 8 + an) = r.desc;
            }
            mmu.mmusr = ms;
            if (fs.ptest <= 16 || (fs.ptest & 1023) == 0)
                kprintf("[PSMAC:MMU] PTEST #%u at %08x: %08x fc %d level %d -> MMUSR %04x\n", fs.ptest, pc, ea, fc, level, ms);
        } else if (((ext >> 10) & 7) == 0) {   /* PLOADR / PLOADW: a table search as the access would make it, into fc's view */
            fs.pload++;
            uint16_t ms;
            if (translate_on && view[0]) psmac_mmu_access(ea, fc, !((ext >> 9) & 1), &ms);
        } else {
            int m = (ext >> 10) & 7, mask = (ext >> 5) & 7;
            if (crp_dirty) { crp_dirty = 0; kprintf("[PSMAC:MMU] PFLUSH applies deferred CRP change\n"); views_update(super, 0); }
            else if (view[0] && translate_on) {
                if (s_seen && !split_now && need_split()) views_update(super, 0);   /* an S bit was met: a space for each mode */
                else if (m == 1) {              /* PFLUSHA */
                    fs.pflush_all++;
                    if (!(after_tc && cnt_now() - last_install < cnt_frq() / 1000)) views_flush(0, 0, 0, 0);
                } else if (m == 4) { fs.pflush_fc++; views_flush(fc, mask, 0, 0); }     /* by FC and mask */
                else if (m == 6) { fs.pflush_ea++; views_flush(fc, mask, 1, ea); }    /* by FC, mask and address */
                else kprintf("[PSMAC:MMU] PFLUSH mode %d at %08x not a 68030 form\n", m, pc);
            }
        }
        regs[18] = pc + len;
        return 0;
    }

    /* PMOVE: TT0/TT1 (top 0), TC/SRP/CRP (2), MMUSR (3) */
    int preg = (ext >> 10) & 7, tomem = (ext >> 9) & 1, fd = (ext >> 8) & 1;
    int size = top == 3 ? 2 : (top == 2 && preg != 0) ? 8 : 4;
    fs.pmove++;
    if ((top == 0 && preg != 2 && preg != 3) || (top == 2 && preg == 1) || (top == 2 && preg > 3)) {
        kprintf("[PSMAC:MMU] PMOVE %04x %04x at %08x: no such register on a 68030\n", opcode, ext, pc);
        regs[18] = pc + len;
        return 0;
    }
    uint32_t a = ea, v[2] = { 0, 0 };
    int inreg = (mode == 0 || mode == 1);
    if (mode == 3) { a = *r68(regs, 8 + rn); }
    if (mode == 4) { a = *r68(regs, 8 + rn) - (uint32_t)size; }
    if (!inreg) {
        uint32_t bad;
        if (!psmac_mmu_probe(a, (uint32_t)size, dfc, tomem, &bad))
            return psmac_mmu_deliver_helper(regs, sr, pc, bad, tomem, size, dfc);
    }
    if (tomem) {
        switch (top) {
        case 0: v[0] = preg == 2 ? mmu.tt0 : mmu.tt1; break;
        case 2: if (preg == 0) v[0] = mmu.tc; else if (preg == 2) { v[0] = mmu.srp_hi; v[1] = mmu.srp_lo; } else { v[0] = mmu.crp_hi; v[1] = mmu.crp_lo; } break;
        default: v[0] = mmu.mmusr; break;
        }
        if (inreg) *r68(regs, mode * 8 + rn) = size == 2 ? ((*r68(regs, mode * 8 + rn) & 0xffff0000u) | v[0]) : v[0];
        else if (size == 2) *(volatile uint16_t *)(uintptr_t)a = (uint16_t)v[0];
        else { *(volatile uint32_t *)(uintptr_t)a = v[0]; if (size == 8) *(volatile uint32_t *)(uintptr_t)(a + 4) = v[1]; }
    } else {
        if (inreg) v[0] = *r68(regs, mode * 8 + rn);
        else if (size == 2) v[0] = *(volatile uint16_t *)(uintptr_t)a;
        else { v[0] = *(volatile uint32_t *)(uintptr_t)a; if (size == 8) v[1] = *(volatile uint32_t *)(uintptr_t)(a + 4); }
        switch (top) {
        case 0: {
            int was = split_now;
            if (preg == 2) mmu.tt0 = v[0]; else mmu.tt1 = v[0];
            if (translate_on && need_split() != was) views_update(super, fd);     /* the TT now separates the modes, or no longer */
            else if (view[0] && translate_on && !fd) views_flush(0, 0, 0, 0);
            break; }
        case 2:
            if (preg == 0) {
                uint32_t old = mmu.tc; mmu.tc = v[0];
                if (old != mmu.tc || crp_dirty) { crp_dirty = 0; views_update(super, fd); }
                else if (!fd && view[0] && translate_on) views_flush(0, 0, 0, 0);
            } else if (preg == 2) {
                mmu.srp_hi = v[0]; mmu.srp_lo = v[1];
                if ((mmu.tc & TC_E) && (mmu.tc & TC_SRE)) views_update(super, fd);
            } else {
                uint32_t oh = mmu.crp_hi, ol = mmu.crp_lo;
                mmu.crp_hi = v[0]; mmu.crp_lo = v[1];
                /* PMOVE to CRP flushes the 68030's ATC and the new root is searched at once; PMOVEFD keeps the ATC, whose
                   entries go on translating with the old tables until a flush. The IIci ROM's SwapMMUMode counts on that:
                   "ploadr 8(a0); pmovefd (a0),crp; pmove 8(a0),tc" reaches its record through the entry the PLOADR made
                   (ROM $40804BFC). So FD defers the switch to the TC load or the PFLUSH that follows (phases 1 and 2 deferred
                   every CRP load; a context switch, A/UX's or a BSD's, is a plain PMOVE and takes effect at once). */
                if ((mmu.tc & TC_E) && (oh != mmu.crp_hi || ol != mmu.crp_lo)) {
                    if (fd) crp_dirty = 1;
                    else { crp_dirty = 0; views_update(super, 0); }
                } else if ((mmu.tc & TC_E) && !fd && view[0]) views_flush(0, 0, 0, 0);
            }
            break;
        default:
            mmu.mmusr = (uint16_t)v[0];
            break;
        }
    }
    if (mode == 3) *r68(regs, 8 + rn) += (uint32_t)size;
    if (mode == 4) *r68(regs, 8 + rn) -= (uint32_t)size;
    regs[18] = pc + len;
    return 0;
}

/* the soft TLB's numbers, for the heartbeat (psmac_bus.c) */
volatile uint32_t psmac_priv_user;      /* M68k_ExceptionEntry.c: privilege violations from user mode (VM's emulation) */
void psmac_mmu_fault_counts(uint32_t *delivered, uint32_t *helper, uint32_t *fetch, uint32_t *lazy, uint32_t *missed);
void psmac_mmu_full_stats(void)
{
    if (!psmac_mmu_full) return;
    uint32_t d, h, f, l, m, nsp = 0;
    psmac_mmu_fault_counts(&d, &h, &f, &l, &m);
    for (int i = 0; i < NSPACES; i++) nsp += spaces[i].used;
    kprintf("[PSMAC:MMU]   faults: %u bus errors delivered (%u data, %u operand, %u fetch), %u not delivered, %u lazy maps; "
            "%u privilege violations from user mode\n", d + h + f, d, h, f, m, l, psmac_priv_user);
    { extern void psmac_mmu_first_faults(void); psmac_mmu_first_faults(); }
    {
        extern void psmac_mmu_miss_report(void);
        psmac_mmu_miss_report();
    }
    kprintf("[PSMAC:MMU]   soft TLB: %u walks, %u maps (%u pages), %u first writes (M), %u emulated, %u TT, %u bus errors; "
            "PFLUSH %u all %u fc %u ea (%u pages kept, %u dropped, %u us), PLOAD %u, PTEST %u, PMOVE %u, MOVES %u, %u switches, "
            "%u code-cache checks; spaces: %u in use (%s), %u made, %u reused, %u us a switch on average\n",
            fs.walks, fs.maps, fs.mapped_pages, fs.m_faults, fs.emulated, fs.tt_hits, fs.buserrs,
            fs.pflush_all, fs.pflush_fc, fs.pflush_ea, fs.pflush_kept, fs.pflush_dropped,
            (uint32_t)(fs.flush_ticks * 1000000 / cnt_frq()), fs.pload, fs.ptest, fs.pmove, fs.moves, fs.switches, fs.epoch_bumps,
            nsp, split_now ? "user and supervisor views apart" : "one view", fs.spaces, fs.evictions,
            fs.switches ? (uint32_t)(fs.switch_ticks * 1000000 / cnt_frq() / fs.switches) : 0);
    kprintf("[PSMAC:MMU]   busiest emulated pages since the last report:");
    for (int k = 0; k < 6; k++) {
        int mx = -1;
        for (int i = 0; i < 256; i++)
            if (emu_hist[i].n && (mx < 0 || emu_hist[i].n > emu_hist[mx].n)) mx = i;
        if (mx < 0) break;
        kprintf(" %08x->%08x %u (%u w)", emu_hist[mx].la, emu_hist[mx].pa, emu_hist[mx].n, emu_hist[mx].w);
        emu_hist[mx].n = 0;
    }
    for (int i = 0; i < 256; i++) emu_hist[i].n = 0;
    kprintf("\n");
}

/* psmac_mmu_l2p with the option: the cache the fault path filled, else a walk that changes nothing (it serves
   the emulated accesses, the mailbox's transfers and the kernel's diagnostics, some of them on other cores). The
   supervisor's view: the Mac's drivers and the emulated accesses of a supervisor page; the fault path has put a
   user access's translation in the cache under the user view, which a user access finds by its own walk. */
static uint32_t full_l2p(uint32_t logical, int *valid)
{
    int v = psmac_mmu_cur_view();
    return full_l2p_fc(logical, v == 0 ? 1 : 5, valid);
}
static uint32_t full_l2p_fc(uint32_t logical, int fc, int *valid)
{
    uint32_t pa;
    struct space *sp = space_for_fc(fc);
    if (sp && l2pc_get(sp, logical, &pa)) { *valid = 1; return pa; }
    if (tt_match(mmu.tt0, logical, fc, 0) || tt_match(mmu.tt1, logical, fc, 0)) { *valid = 1; return logical; }
    struct pwalk r;
    pwalk(logical, fc, 0, 99, &r);
    *valid = r.valid && !(r.s && !(fc & 4));
    return r.valid ? r.pa : logical;
}

/* a diagnostic for the fault path (psmac_mmufault.c): every descriptor the search for la meets */
// the MMU's registers in the log, for a table walker on the host (mac.usertrace, docs/23 section 21)
void psmac_mmu_debug_regs(void)
{
    kprintf("[PSMAC:MMU]   TC %08x CRP %08x:%08x SRP %08x:%08x TT0 %08x TT1 %08x\n", mmu.tc, mmu.crp_hi, mmu.crp_lo,
            mmu.srp_hi, mmu.srp_lo, mmu.tt0, mmu.tt1);
}
void psmac_mmu_debug_walk(uint32_t la, int fc)
{
    struct pwalk r;
    struct space *sp = space_for_fc(fc);
    pwalk(la, fc, 0, 99, &r);
    kprintf("[PSMAC:MMU]   walk of %08x fc %d: TC %08x CRP %08x:%08x SRP %08x:%08x TT %08x %08x -> %s pa %08x MMUSR %04x, last descriptor at %08x = %08x",
            la, fc, mmu.tc, mmu.crp_hi, mmu.crp_lo, mmu.srp_hi, mmu.srp_lo, mmu.tt0, mmu.tt1, r.valid ? "valid" : "invalid", r.pa, r.mmusr, r.desc,
            r.desc && desc_in_ram(r.desc) ? *desc_at(r.desc) : 0);
    uint32_t pa;
    kprintf(", space %d (%s), ARM entry %016llx, mapped bit %d, cache %s\n", sp ? sp->idx : -1, split_now ? "views apart" : "one view",
            sp ? (unsigned long long)mmu_space_entry(sp->l1, la & ~0xfffu) : 0ull,
            sp ? sp_get(sp, la >> 12) : -1, sp && l2pc_get(sp, la, &pa) ? "hit" : "miss");
}

/* mac.mmu=full: the spaces, at the start (psmac_mmu_init), core 0 in supervisor mode with translation off */
static void spaces_init(void)
{
    for (int i = 0; i < NSPACES; i++) { spaces[i].idx = (uint8_t)i; spaces[i].asid = (uint8_t)(i + 1); }
    kprintf("[PSMAC:MMU] the Pi's windows in the first 4 GiB, which the 68k's address spaces leave out:\n");
    mmu_low4g_describe(pristine);
    views_update(1, 1);
}

/* The translator reads a block's code through the data view (the ARM mapping), while a 68030 fetches it at the
   program function code (2 or 6). They can differ only with TC.FCL or a TT register that compares FC0/FC1: then the
   two searches are compared once per block and a difference is logged (the first eight), a sign that the program
   space is not what the translated code was read from (docs/23-mmu.md section 12.8). */
void psmac_mmu_check_code_space(uint32_t pc, int super)
{
    if (!translate_on) return;
    int fcl = (mmu.tc & TC_FCL) != 0;
    int tts = ((mmu.tt0 & 0x8000u) && (mmu.tt0 & 3u) != 3u) || ((mmu.tt1 & 0x8000u) && (mmu.tt1 & 3u) != 3u);
    if (!fcl && !tts) return;
    static uint32_t told;
    if (told >= 8) return;
    int fp = super ? 6 : 2, fd = super ? 5 : 1;
    uint32_t pp, pd;
    int vp = tt_match(mmu.tt0, pc, fp, 0) || tt_match(mmu.tt1, pc, fp, 0), vd = tt_match(mmu.tt0, pc, fd, 0) || tt_match(mmu.tt1, pc, fd, 0);
    struct pwalk r;
    if (vp) pp = pc; else { pwalk(pc, fp, 0, 99, &r); pp = r.valid ? r.pa : ~0u; }
    if (vd) pd = pc; else { pwalk(pc, fd, 0, 99, &r); pd = r.valid ? r.pa : ~0u; }
    if (pp != pd) {
        told++;
        kprintf("[PSMAC:MMU] code at %08x: the program space gives %08x, the data space %08x; the block is read from the data "
                "space (FCL or a TT register tells them apart, docs/23-mmu.md section 12.8)\n", pc, pp, pd);
    }
}

/* 1 when the fault path has work to do: with the option, from the first space on (translation off included, whose
   identity is mapped page by page too) */
/* the guest's user and supervisor views kept apart (SRE with its own SRP, FCL, ...): A/UX's tables, never the Mac OS's;
   psmac_board_se.c's mac.sccirq=aux asks (docs/23 section 14) */
int psmac_mmu_views_apart(void) { return split_now; }
int psmac_mmu_translating(void) { return (psmac_mmu_full && view[0]) || (translate_on && l2p); }

/* A check the hardware alone can fail (26 November 2026, docs/23 section 19; aarch64/mmu.c, tlbi_page_everywhere). A
   scratch address of the supervisor's space is mapped on one page of the Mac's RAM, read at its own address and at
   +4 GiB (two TLB entries of one descriptor), moved to a second page, and read again at +4 GiB: first with the flush as
   it was until today (the page's own address alone), then with the flush as it is. A real core answers "the old page"
   to the first and "the new page" to the second; QEMU "the new page" to both when its TLB happens to have dropped the
   entry. One line in the log, at the start, so that a log from a Pi says by itself what its TLB keeps. */
static void alias_selfcheck(void)
{
    extern int mmu_tlbi_low_only;
    struct space *s = view[1];
    if (!s || ram_size < 0x400000u) return;
    const uint32_t V = 0x7e000000u;                       /* nothing of the Mac's or the Pi's is there at the start */
    const uint32_t PA = 0x00200000u, PB = 0x00201000u;    /* two pages of RAM the ROM has not touched yet */
    volatile uint32_t *ka = (volatile uint32_t *)(KERNEL_ALIAS + ram_phys + PA), *kb = (volatile uint32_t *)(KERNEL_ALIAS + ram_phys + PB);
    volatile uint32_t *low = (volatile uint32_t *)(uintptr_t)V, *high = (volatile uint32_t *)((uintptr_t)V + 0x100000000ull);
    uint32_t sa = *ka, sb = *kb, r[2];
    *ka = 0xa0a0a0a0u; *kb = 0xb0b0b0b0u;
    for (int pass = 0; pass < 2; pass++) {
        mmu_space_page(s->l1, s->asid, V, ram_phys + PA, RW_ATTR_NG, 1);
        volatile uint32_t t = *low; t = *high; (void)t;   /* both entries into the TLB */
        mmu_tlbi_low_only = pass == 0;
        mmu_space_page(s->l1, s->asid, V, ram_phys + PB, RW_ATTR_NG, 1);
        mmu_tlbi_low_only = 0;
        r[pass] = *high;
        mmu_space_page(s->l1, s->asid, V, ~(uintptr_t)0, 0, 1);
    }
    *ka = sa; *kb = sb;
    kprintf("[PSMAC:MMU] TLB self-check: a page moved, then read at +4 GiB: the %s page after a flush of its own address alone "
            "(%s), the %s page after the flush of both (%s)\n",
            r[0] == 0xb0b0b0b0u ? "new" : r[0] == 0xa0a0a0a0u ? "OLD" : "?", r[0] == 0xb0b0b0b0u ? "this TLB had dropped the entry, as QEMU's does" : "a real TLB keeps it: the fault of docs/23 section 19",
            r[1] == 0xb0b0b0b0u ? "new" : r[1] == 0xa0a0a0a0u ? "OLD" : "?", r[1] == 0xb0b0b0b0u ? "right" : "WRONG: the flush does not reach the alias");
}
