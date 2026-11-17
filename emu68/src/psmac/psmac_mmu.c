/*
 * psmac_mmu.c — 68030 MMU shadowing for the Macintosh IIci ROM on Emu68 (PiStorm-SE fork). MPL-2.0.
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

static struct { uint32_t tc, tt0, tt1, srp_hi, srp_lo, crp_hi, crp_lo; uint16_t mmusr; } mmu;
static uintptr_t ram_phys, rom_phys; static uint32_t ram_size;
static uint32_t *l2p;          /* per 4 KiB logical page of the CURRENT set: physical page | flags (base region only) */
static int fill_enabled;       /* walk mode: 1 = fill l2p, 0 = hash the descriptors only */
static int translate_on;       /* TC.E of the current set */
static uint32_t is_bits;       /* TC.IS of the current set */
static uint32_t rebuilds;
static int crp_dirty;          /* CRP changed while translating; rebuild pending until TC load or PFLUSH */
static uint64_t last_install;  /* counter value of the last TC/CRP switch (its PFLUSH is implied) */

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
void psmac_mmu_set_slot(uintptr_t w, uintptr_t d) { slot_win_phys = w; slot_declrom_phys = d; }
void psmac_mmu_set_fb(uintptr_t f) { slot_fb_phys = f; }       /* the second display's big frame buffer (psmb.h) */
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
    if (slot_fb_phys && p >= PSMB_FB_BIG_PHYS && p < PSMB_FB_BIG_PHYS + PSMB_FB_BIG_SIZE) return slot_fb_phys + (p - PSMB_FB_BIG_PHYS);
    return 0;
}
/* Mac physical pages that must be read-only for the 68k: ROM, declaration ROM, write-through
   frame buffer and the (debug) watched low-memory page */
static inline int ro_phys(uint32_t p)
{
    return (p < PSMAC_FB_SIZE) || (p >= 0x40000000u && p < 0x50000000u) || (p == WATCH_PHYS_PAGE) ||
           (p >= PSMB_DECLROM_PHYS && p < PSMB_DECLROM_PHYS + PSMB_DECLROM_SIZE);
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
    for (uint32_t p = 0; p < PSMB_FB_BIG_SIZE; p += 4096) l2p[(PSMB_FB_BIG_PHYS + p) >> 12] = (PSMB_FB_BIG_PHYS + p) | L2P_VALID;   /* the big frame buffer */
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
}

/* PFLUSH: the 68030 drops its ATC, so table edits made in place become visible. Re-walk and remap if
   the tables differ from what the current set was built from. The PFLUSHA that follows every TC load
   in SwapMMUMode is implied by the switch itself and skipped. */
static void pflush(void)
{
    if (crp_dirty) { crp_dirty = 0; kprintf("[PSMAC:MMU] PFLUSH applies deferred CRP change\n"); rebuild(); return; }
    if (!cur || !translate_on) return;
    if (cnt_now() - last_install < cnt_frq() / 200) return;     /* within 5 ms of the switch */
    struct walk w;
    fill_enabled = 0;
    if (!walk_current(&w)) return;
    if (w.hash == cur->hash && w.fills == cur->fills) return;
    kprintf("[PSMAC:MMU] PFLUSH: tables changed in place, remapping set TC=%08x CRP=%08x:%08x\n", cur->tc, cur->crp_hi, cur->crp_lo);
    unmap_set(cur);
    l2p = set_l2p(cur, w.npages);
    fill_enabled = 1; walk_current(&w);
    apply_l2p(); cur->hash = w.hash; cur->fills = w.fills;
    mmu_low4g_get(cur->l1);
}

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
    mmu_low4g_get(pristine);
    kprintf("[PSMAC:MMU] pristine low-4G subtree: %016llx %016llx %016llx %016llx\n", pristine[0], pristine[1], pristine[2], pristine[3]);
    mmu.tc = 0;
    rebuild();   /* identity set */
}

/* logical -> physical for the abort handler; returns 0 and *valid=0 for unmapped pages */
uint32_t psmac_mmu_l2p(uint32_t logical, int *valid)
{
    if (!translate_on || !l2p) { *valid = 1; return logical; }
    uint32_t idx = (is_bits ? (logical & ((1u << (32 - is_bits)) - 1)) : logical) >> 12;
    uint32_t e = l2p[idx];
    if (!(e & L2P_VALID)) { *valid = 0; return logical; }
    *valid = 1;
    return (e & ~0xfffu) | (logical & 0xfff);
}

void psmac_pmove(uint32_t ea, uint32_t ext)
{
    int fmt = (ext >> 13) & 7, preg = (ext >> 10) & 7, rw = (ext >> 9) & 1;
    switch (fmt) {
    case 1: /* PFLUSH / PFLUSHA (PLOAD never gets here) */
        pflush();
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
