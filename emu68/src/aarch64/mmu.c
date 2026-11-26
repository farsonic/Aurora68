/*
    Copyright © 2020 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <stdint.h>
#include "mmu.h"
#include "support.h"
#include "tlsf.h"
#include "A64.h"
#include "devicetree.h"
#include "globals.h"
#include "spinlock.h"

#define DV2P(x) /* if (is_serial_up()) { x ;} */ /* x */
#define DMAP(x) /* if (is_serial_up()) { x ;} */ /* x */

#define DESC_TYPE_MASK      3ULL
#define DESC_BLOCK          1ULL                    /* L1 1GB / L2 2MB block */
#define DESC_TABLE          3ULL                    /* L1/L2 pointer to next level */
#define DESC_PAGE           3ULL                    /* L3 4K page */
#define DESC_CONTIG         (1ULL << 52)
#define DESC_OA_MASK        0x0000fffffffff000ULL
#define DESC_UATTR_MASK     0xfff0000000000000ULL

#define SZ_4K               0x1000ULL
#define SZ_2M               0x200000ULL
#define SZ_1G               0x40000000ULL

#define L1_IDX(v)           (((v) >> 30) & 0x1ff)
#define L2_IDX(v)           (((v) >> 21) & 0x1ff)
#define L3_IDX(v)           (((v) >> 12) & 0x1ff)

#define IS_KERNEL_VA(v)     (((v) & 0xffff000000000000ULL) != 0)

/* Virtual base of physical address space at 0x0 (320GB) */
static const uintptr_t PHYS_VIRT_OFFSET = 0xffffff9000000000;

/* Number of 4K pages in the MMU pool */
#ifdef PSMAC
#define MMU_POOL_SIZE 8192   /* Aurora68: the 24-bit Mac map replicates 16 MiB 256 times */
#else
#define MMU_POOL_SIZE 2048
#endif

struct mmu_page
{
    union
    {
        struct mmu_page *mp_next;
        uint64_t mp_entries[512];
    };
};

/* L1 table of the half selected by virt. TTBR may carry ASID/CnP bits, mask them off */
static inline struct mmu_page *mmu_root(uintptr_t virt)
{
    uintptr_t ttbr;
 
    if (IS_KERNEL_VA(virt))
        __asm__ volatile("mrs %0, TTBR1_EL1":"=r"(ttbr));
    else
        __asm__ volatile("mrs %0, TTBR0_EL1":"=r"(ttbr));
 
    return (struct mmu_page *)((ttbr & DESC_OA_MASK) + PHYS_VIRT_OFFSET);
}

/* Make table stores visible to the table walker before anything links to them */
void mmu_tlb_paranoid(void);                     /* mac.mmutlb=flush (below, psmac) */
static inline void mmu_publish(void)
{
    __asm__ volatile("dsb ishst" ::: "memory");
}

/* Flush all TLB entries */
static inline void mmu_tlb_flush_all(void)
{
    __asm__ volatile(
"       dsb     ishst               \n"
"       tlbi    VMALLE1IS           \n"
"       dsb     ish                 \n"
"       isb                         \n" ::: "memory");
}


/* L1 table for bottom half. Filled from startup code */
__attribute__((used, section(".mmu"))) struct mmu_page mmu_user_L1;
/* L1 table for top half */
__attribute__((used, section(".mmu"))) struct mmu_page mmu_kernel_L1;
/* One additional directory to map the 1GB kernel address space in 2MB pages here */
__attribute__((used, section(".mmu"))) struct mmu_page mmu_kernel_L2;

/* Serialize access to MMU functions */
static spinlock_t mmu_lock;

/*
    Pool of free 4K pages for translation tables. Single linked list: allocate = pop head,
    free = push head.
*/
static struct mmu_page *mmu_free_pages;

/*
    Tables unlinked during the current map/unmap. They may still be cached by the table walker
    of any core, so they go back to the pool only after the TLB invalidate in mmu_commit().
    Kept as a list with a tail pointer, so returning them is an O(1) splice.
*/
static struct mmu_page *mmu_retired_head;
static struct mmu_page *mmu_retired_tail;

static int mmu_pages_free = 0;
static int mmu_pages_used = 0;
static int mmu_pages_retired = 0;
static int mmu_pages_used_max = 0;

void mmu_print_stats(void)
{
    kprintf("[MMU] Pages free: %d, used: %d, retired: %d, max used: %d\n",
            mmu_pages_free, mmu_pages_used, mmu_pages_retired, mmu_pages_used_max);
}

static void mmu_pool_init(uintptr_t va, uintptr_t size)
{
    for (uintptr_t p = va; p < va + size; p += sizeof(struct mmu_page))
    {
        struct mmu_page *page = (struct mmu_page *)p;
        page->mp_next = mmu_free_pages;
        mmu_free_pages = page;
        mmu_pages_free++;
    }
}

static struct mmu_page* get_4k_page()
{
    struct mmu_page *p = mmu_free_pages;

    /* Check if there is a free 4k page */
    if (!p)
    {
        kprintf("[MMU] Out of 4K pages for MMU tables!\n");
        while(1);
    }

    mmu_free_pages = p->mp_next;

    mmu_pages_free--;
    mmu_pages_used++;
    if (mmu_pages_used > mmu_pages_used_max)
        mmu_pages_used_max = mmu_pages_used;

    return p;
}

/*
    Check if the table is a static slot. In our case this can be only 
    the mmu_kernel_L2 table.
    
    Both L1 tables are root and cannot be freed. All tables **besides** mmu_kernel_L2 
    are allocated from the pool and can be freed.
*/
static inline int is_static_slot(uintptr_t virt)
{
    return IS_KERNEL_VA(virt) && L1_IDX(virt) == 0;
}

/*
    Push an unlinked table on the retired list. Overwriting entry 0 with the link is harmless:
    a kernel VA with bits [1:0] == 0 reads as an invalid descriptor, and the table is already
    unreachable from the live tree.
*/
static void retire_table(struct mmu_page *p)
{
    mmu_pages_retired++;
    p->mp_next = mmu_retired_head;
    if (mmu_retired_head == NULL)
        mmu_retired_tail = p;
    mmu_retired_head = p;
}

/*
    Commit any changes to the MMU tables. This flushes the TLB and, once flush is complete,
    returns retired tables to the free pool. This is safe because the TLB flush ensures that no core
    can be using the retired tables any more.
*/
static void mmu_commit(void)
{
    mmu_tlb_flush_all();
 
    if (mmu_retired_head)
    {
        mmu_retired_tail->mp_next = mmu_free_pages;
        mmu_free_pages = mmu_retired_head;
        mmu_retired_head = NULL;
        mmu_retired_tail = NULL;
        mmu_pages_free += mmu_pages_retired;
        mmu_pages_used -= mmu_pages_retired;
        mmu_pages_retired = 0;
    }
}

/* Convert a MMU page/directory descriptor to a mmu_page pointer */
static inline struct mmu_page *desc_to_table(uint64_t d)
{
    return (struct mmu_page *)((d & DESC_OA_MASK) + PHYS_VIRT_OFFSET);
}

/* Convert a mmu_page pointer to a MMU page/directory descriptor */
static inline uint64_t table_to_desc(struct mmu_page *p)
{
    return ((uintptr_t)p - PHYS_VIRT_OFFSET) | DESC_TABLE;
}

/* Shift and mask low and high attrs to make them or-able with the descriptor*/
static inline uint64_t make_attrs(uint32_t attr_low, uint32_t attr_high)
{
    /* attr_high is the top 16 bits of the descriptor; [51:48] are output address, not attributes */
    return ((uint64_t)attr_low & 0xffc) | (((uint64_t)attr_high << 48) & DESC_UATTR_MASK);
}

/* Break-before-make replacement of a live L2 or L3 descriptor */
static void replace_entry_bbm(uint64_t *entry, uint64_t new_desc)
{
    *entry = 0;
    mmu_tlb_flush_all();
    *entry = new_desc;
    mmu_publish();
}

/*
    The first 4GB of user space shares its L1 entries with user 4..8GB and with the top 4GB of
    kernel space. Collect all slots belonging to the L1 entry of virt.
*/
static int l1_slots(uintptr_t virt, uint64_t *slots[3])
{
    struct mmu_page *l1 = mmu_root(virt);
    int idx = L1_IDX(virt);
    int n = 0;
 
    slots[n++] = &l1->mp_entries[idx];
 
    if (!IS_KERNEL_VA(virt) && idx < 4)
    {
        slots[n++] = &l1->mp_entries[idx + 4];
        slots[n++] = &mmu_root(~0ULL)->mp_entries[508 + idx];
    }
 
    return n;
}

/* Set an L1 descriptor together with all its aliases. With bbm, the old one is broken first */
static void set_l1_entry(uintptr_t virt, uint64_t new_desc, int bbm)
{
    uint64_t *slots[3];
    int n = l1_slots(virt, slots);
 
    if (bbm)
    {
        for (int i=0; i < n; i++) {
            *slots[i] = 0;
        }
        mmu_tlb_flush_all();
    }
 
    for (int i=0; i < n; i++) {
        *slots[i] = new_desc;
    }
 
    mmu_publish();
}


/*
    Make sure *entry (an L1 entry if level == 1, L2 entry if level == 2) points to a table and
    return it. An invalid entry gets a fresh, zeroed table. A block gets split into 512 blocks
    (or pages) of the next level with identical attributes.
*/
static struct mmu_page *ensure_table(uint64_t *entry, int level, uintptr_t virt)
{
    uint64_t d = *entry;
    struct mmu_page *t;
    int was_block = 0;
 
    if ((d & DESC_TYPE_MASK) == DESC_TABLE) {
        return desc_to_table(d);
    }
 
    t = get_4k_page();
 
    if ((d & DESC_TYPE_MASK) == DESC_BLOCK)
    {
        uint64_t step = (level == 1) ? SZ_2M : SZ_4K;
        uint64_t base = (d & ~(DESC_TYPE_MASK | DESC_CONTIG)) | ((level == 1) ? DESC_BLOCK : DESC_PAGE);
 
        DMAP(kprintf("[MMU] L%d entry is a block. Splitting\n", level));
 
        for (int i=0; i < 512; i++) {
            t->mp_entries[i] = base + i * step;
        }
 
        was_block = 1;
    }
    else
    {
        for (int i=0; i < 512; i++) {
            t->mp_entries[i] = 0;
        }
    }
 
    mmu_publish();
 
    if (level == 1) {
        set_l1_entry(virt, table_to_desc(t), was_block);
    } else if (was_block) {
        replace_entry_bbm(entry, table_to_desc(t));
    } else {
        *entry = table_to_desc(t);
    }
 
    return t;
}

/* Free an L2 table together with all L3 tables it points to */
static void retire_l2_tree(struct mmu_page *l2)
{
    for (int i=0; i < 512; i++)
    {
        if ((l2->mp_entries[i] & DESC_TYPE_MASK) == DESC_TABLE) {
            retire_table(desc_to_table(l2->mp_entries[i]));
        }
    }
    retire_table(l2);
}


/*
    Called on an L2 entry after its 2MB window was modified. If it points to an L3 table:
    - all 512 entries invalid -> drop the table,
    - 512 pages with physically contiguous, 2MB aligned output and identical attributes -> fold
      into a single 2MB block.
*/
static void tidy_l2_entry(uint64_t *e2)
{
    uint64_t d = *e2;
    struct mmu_page *t;
    uint64_t first;
 
    if ((d & DESC_TYPE_MASK) != DESC_TABLE) return;
 
    t = desc_to_table(d);
    first = t->mp_entries[0];
 
    if (first == 0)
    {
        for (int i=1; i < 512; i++) {
            if (t->mp_entries[i]) return;
        }
 
        DMAP(kprintf("[MMU] L3 table empty, dropping it\n"));
        *e2 = 0;
        retire_table(t);
    }
    else if ((first & DESC_TYPE_MASK) == DESC_PAGE && (first & (SZ_2M - SZ_4K)) == 0)
    {
        for (int i=1; i < 512; i++) {
            if (t->mp_entries[i] != first + i * SZ_4K) return;
        }
 
        DMAP(kprintf("[MMU] L3 table maps contiguous 2MB, merging\n"));
        replace_entry_bbm(e2, (first & ~(DESC_TYPE_MASK | DESC_CONTIG)) | DESC_BLOCK);
        retire_table(t);
    }
}

/* Same as tidy_l2_entry, one level up: drop an empty L2 table or fold it into a 1GB block */
static void tidy_l1_entry(uintptr_t virt)
{
    uint64_t d = mmu_root(virt)->mp_entries[L1_IDX(virt)];
    struct mmu_page *t;
    uint64_t first;
 
    if (is_static_slot(virt) || (d & DESC_TYPE_MASK) != DESC_TABLE) {
        return;
    }
 
    t = desc_to_table(d);
    first = t->mp_entries[0];
 
    if (first == 0)
    {
        for (int i=1; i < 512; i++) {
            if (t->mp_entries[i]) return;
        }
 
        DMAP(kprintf("[MMU] L2 table empty, dropping it\n"));
        set_l1_entry(virt, 0, 0);
        retire_table(t);
    }
    else if ((first & DESC_TYPE_MASK) == DESC_BLOCK && (first & (SZ_1G - SZ_4K)) == 0)
    {
        for (int i=1; i < 512; i++) {
            if (t->mp_entries[i] != first + i * SZ_2M) return;
        }
 
        DMAP(kprintf("[MMU] L2 table maps contiguous 1GB, merging\n"));
        set_l1_entry(virt, first & ~DESC_CONTIG, 1);
        retire_table(t);
    }
}

static void put_2m_page(uintptr_t phys, uintptr_t virt, uint64_t attrs)
{
    struct mmu_page *l2 = ensure_table(&mmu_root(virt)->mp_entries[L1_IDX(virt)], 1, virt);
    uint64_t *e2 = &l2->mp_entries[L2_IDX(virt)];
    uint64_t new_desc = (phys & DESC_OA_MASK & ~(SZ_2M - 1)) | attrs | DESC_BLOCK;
 
    DMAP(kprintf("[MMU] put_2m_page(%p, %p, %016x)\n", phys, virt, attrs));
 
    if ((*e2 & DESC_TYPE_MASK) == DESC_TABLE)
    {
        struct mmu_page *l3 = desc_to_table(*e2);
 
        DMAP(kprintf("[MMU] L2 entry was pointing to L3 directory. Freeing it now\n"));
        replace_entry_bbm(e2, new_desc);
        retire_table(l3);
    }
    else
    {
        *e2 = new_desc;
    }
}

static void put_4k_page(uintptr_t phys, uintptr_t virt, uint64_t attrs)
{
    struct mmu_page *l2 = ensure_table(&mmu_root(virt)->mp_entries[L1_IDX(virt)], 1, virt);
    struct mmu_page *l3 = ensure_table(&l2->mp_entries[L2_IDX(virt)], 2, virt);
 
    DMAP(kprintf("[MMU] put_4k_page(%p, %p, %016x)\n", phys, virt, attrs));
 
    l3->mp_entries[L3_IDX(virt)] = (phys & DESC_OA_MASK) | attrs | DESC_PAGE;
}

/* Run the merge/drop pass over every 2MB and 1GB window touched by [virt, virt + length) */
static void tidy_range(uintptr_t virt, uintptr_t length)
{
    uintptr_t v = virt & ~(SZ_2M - 1);
    uintptr_t left = length + (virt - v);
 
    while (left)
    {
        uintptr_t step = (left > SZ_2M) ? SZ_2M : left;
        uint64_t d = mmu_root(v)->mp_entries[L1_IDX(v)];
 
        if ((d & DESC_TYPE_MASK) == DESC_TABLE) {
            tidy_l2_entry(&desc_to_table(d)->mp_entries[L2_IDX(v)]);
        }
 
        /* Last 2MB window of a 1GB region, or end of range: try the L1 entry too */
        if (step == left || ((v + SZ_2M) & (SZ_1G - 1)) == 0) {
            tidy_l1_entry(v);
        }
 
        v += SZ_2M;
        left -= step;
    }
}


uintptr_t mmu_virt2phys(uintptr_t addr)
{
    uintptr_t phys = 0;
    uint64_t *tbl = mmu_root(addr)->mp_entries;
    int idx_l1, idx_l2, idx_l3;
    uint64_t tmp;
 
    DV2P(kprintf("[MMU] virt2phys(%p)\n", addr));
    DV2P(kprintf("[MMU] L1 table: %p\n", tbl));
 
    idx_l1 = L1_IDX(addr);
    idx_l2 = L2_IDX(addr);
    idx_l3 = L3_IDX(addr);
 
    DV2P(kprintf("[MMU] idx_l1 = %d, idx_l2 = %d, idx_l3 = %d\n", idx_l1, idx_l2, idx_l3));
 
    tmp = tbl[idx_l1];
    DV2P(kprintf("[MMU] item in L1 table: %016x\n", tmp));
    if (tmp & 1)
    {
        DV2P(kprintf("[MMU] is valid\n"));
        if (tmp & 2) {
            tbl = desc_to_table(tmp)->mp_entries;
            DV2P(kprintf("[MMU] L2 table at %p\n", tbl));
 
            tmp = tbl[idx_l2];
            DV2P(kprintf("[MMU] item in L2 table: %016x\n", tmp));
            if (tmp & 1)
            {
                DV2P(kprintf("[MMU] is valid\n"));
                if (tmp & 2)
                {
                    tbl = desc_to_table(tmp)->mp_entries;
                    DV2P(kprintf("[MMU] L3 table at %p\n", tbl));
 
                    tmp = tbl[idx_l3];
                    DV2P(kprintf("[MMU] item in L3 table: %016x\n", tmp));
                    if ((tmp & 3) == 3)
                    {
                        DV2P(kprintf("[MMU] is valid 4K page\n"));
                        phys = (tmp & 0xfffffffff000) + (addr & 0xfff);
                    }
                    else {
                        DV2P(kprintf("[MMU] invalid!\n"));
                        return -1;
                    }
                }
                else {
                    DV2P(kprintf("[MMU] 2MB page!\n"));
                    phys = (tmp & 0xffffffe00000) + (addr & 0x1fffff);
                }
            }
            else
            {
                DV2P(kprintf("[MMU] invalid!\n"));
                return -1;
            }
        } else {
            DV2P(kprintf("[MMU] 1GB page!\n"));
            phys = (tmp & 0xffffc0000000) + (addr & 0x3fffffff);
        }
    }
    else {
        DV2P(kprintf("[MMU] invalid!\n"));
        return -1;
    }
 
    DV2P(kprintf("[MMU] returning %p\n", phys));
 
    return phys;
}

struct MemoryBlock *sys_memory;

/* Aurora68: where the last memory block will end once mmu_init has taken the MMU pool, the JIT
   caches and the VideoCore/CMA/unicam reservations from its top: the same arithmetic as below, with
   nothing changed. start.c uses it before mmu_init to put a large initramfs just below the kernel's
   final home (the kernel's own KERNEL_RSRVD_PAGES come off this value too). */
uintptr_t mmu_predict_top(void)
{
    of_node_t *e = dt_find_node("/memory");
    if (!e) return 0;
    of_property_t *p = dt_find_property(e, "reg");
    uint32_t *range = p->op_value;
    int size_cells = dt_get_property_value_u32(e, "#size-cells", 1, TRUE);
    int address_cells = dt_get_property_value_u32(e, "#address-cells", 1, TRUE);
    int block_size = 4 * (size_cells + address_cells);
    int block_count = p->op_length / block_size;
    uintptr_t addr = 0, size = 0;
    range += (block_count - 1) * (block_size / 4);
    for (int i = 0; i < address_cells; i++) addr = (addr << 32) | BE32(range[i]);
    for (int i = 0; i < size_cells; i++) size = (size << 32) | BE32(range[i + address_cells]);
    of_node_t *em = dt_find_node("/emu68");
    size -= MMU_POOL_SIZE * sizeof(struct mmu_page);
    size -= (uintptr_t)dt_get_property_value_u32(em, "m68k-jit-size", 0, FALSE) << 20;
    if (dt_find_property(em, "enable-ppc")) size -= (uintptr_t)dt_get_property_value_u32(em, "ppc-jit-size", 0, FALSE) << 20;
    if ((p = dt_find_property(em, "vc4-mem"))) { uint32_t *reg = p->op_value; if (reg[1] != 0 && reg[0] == 0 && addr + size <= 0x40000000) size -= (reg[1] + 0x00200000); }
    if ((p = dt_find_property(dt_find_node("/emu68/cma@0"), "reg"))) { uint32_t *reg = p->op_value; if (reg[1] != 0 && reg[0] == 0 && addr + size <= 0x40000000) { size -= (reg[1] + 0x1fffff) & ~0x1fffff; size -= 2 << 20; } }
    if ((p = dt_find_property(em, "unicam-mem"))) { uint32_t *reg = p->op_value; if (reg[1] != 0 && reg[0] == 0 && addr + size <= 0x40000000) { size -= (reg[1] + 0x1fffff) & ~0x1fffff; size -= 2 << 20; } }
    return addr + size;
}

void mmu_init()
{
    /*
        At this stage the user space memory is not set up yet, but the kernel runs in high address
        space, so it is safe to adjust the MMU tables for lower region. Here, only peripherals are
        mapped, but the rest will come very soon.
    */

    uintptr_t mmu_ploc = 0;

    of_node_t *e = dt_find_node("/memory");

    if (e)
    {
        of_property_t *p = dt_find_property(e, "reg");
        uint32_t *range = p->op_value;
        int size_cells = dt_get_property_value_u32(e, "#size-cells", 1, TRUE);
        int address_cells = dt_get_property_value_u32(e, "#address-cells", 1, TRUE);
        int block_size = 4 * (size_cells + address_cells);
        int block_count = p->op_length / block_size;
        bool unicam_set = false;
        bool vc4_set = false;
        bool cma_set = false;

        sys_memory = tlsf_malloc(tlsf, (1 + block_count) * sizeof(struct MemoryBlock));
        for (int block=0; block < block_count; block++)
        {
            of_property_t *p = NULL;
            int last_block = (block == block_count - 1);
            uintptr_t addr = 0;
            uintptr_t size = 0;
            int update_needed = 0;

            for (int i=0; i < address_cells; i++)
            {
                addr = (addr << 32) | BE32(range[i]);
            }
            for (int i=0; i < size_cells; i++)
            {
                size = (size << 32) | BE32(range[i + address_cells]);
            }

            /* 
                If this is the last block, borrow 2MB from there for initial MMU page pool, then take 
                memory from there for m68k and PPC jit, too.
            */
            if (last_block) {
                of_node_t *e = dt_find_node("/emu68");

                size -= MMU_POOL_SIZE * sizeof(struct mmu_page);
                update_needed = 1;
                mmu_ploc = addr + size;

                uint32_t m68k_jit_size = dt_get_property_value_u32(e, "m68k-jit-size", 0, FALSE) << 20;
                size -= m68k_jit_size;
                globals.m68k_jit_phys_base = (void *)(addr + size);

                if (dt_find_property(e, "enable-ppc"))
                {
                    uint32_t ppc_jit_size = dt_get_property_value_u32(e, "ppc-jit-size", 0, FALSE) << 20;
                    size -= ppc_jit_size;
                    globals.ppc_jit_phys_base = (void *)(addr + size);
                }
            }

            /* 
                If /emu68/vc4-mem property exists, check if its base address is 0. If so, 
                then we can assign it a base address here, provided the memory block ends below 1GB,
                and adjust the size of the memory block accordingly. 
            */
            if (!vc4_set && (p = dt_find_property(dt_find_node("/emu68"), "vc4-mem"))) {
                uint32_t *reg = p->op_value;
                if (reg[1] != 0 && reg[0] == 0) {
                    if (addr + size <= 0x40000000) {
                        size -= (reg[1] + 0x00200000);
                        reg[0] = addr + size + (2 << 20);
                        update_needed = 1;
                        vc4_set = true;
                    }
                }
            }

            /*
                If /emu68/cma node exists, check its reg property. If base address is set to 0,
                assign some memory to it now, provided the memory block ends below 1GB (some 
                Pi peripherals might need it)
            */

            if (!cma_set && (p = dt_find_property(dt_find_node("/emu68/cma@0"), "reg"))) {
                uint32_t *reg = p->op_value;
                if (reg[1] != 0 && reg[0] == 0) {
                    if (addr + size <= 0x40000000) {
                        size -= (reg[1] + 0x1fffff) & ~0x1fffff;
                        size -= 2 << 20;
                        reg[0] = addr + size + (2 << 20);
                        update_needed = 1;
                        cma_set = true;
                    }
                }
            }

            /* 
                If /emu68/unicam-mem property exists, check if its base address is 0. If so,
                then we can assign it a base address here, provided the memory block ends below 1GB,
                and adjust the size of the memory block accordingly. 
            */ 
            if (!unicam_set && (p = dt_find_property(dt_find_node("/emu68"), "unicam-mem"))) {
                uint32_t *reg = p->op_value;
                if (reg[1] != 0 && reg[0] == 0) {
                    if (addr + size <= 0x40000000) {
                        size -= (reg[1] + 0x1fffff) & ~0x1fffff;
                        size -= 2 << 20;
                        reg[0] = addr + size + (2 << 20);
                        update_needed = 1;
                        unicam_set = true;
                    }
                }
            }

#ifdef PISTORM_ANY_MODEL
            // Adjust base and size of the memory block
            if (addr < 0x01000000) {
                size -= 0x01000000 - addr;
                addr = 0x01000000;

                update_needed = 1;
            }
#endif
            if (update_needed)
            {
                // Put the block back into device tree, this time in updated form
                uint32_t *storage = &range[address_cells];
                uintptr_t tmp = addr;
                for (int i=0; i < address_cells; i++)
                {
                    *--storage = (uint32_t)tmp;
                    tmp >>= 32;
                }

                storage = &range[address_cells + size_cells];
                tmp = size;
                for (int i=0; i < size_cells; i++)
                {
                    *--storage = (uint32_t)tmp;
                    tmp >>= 32;
                }

            }

            sys_memory[block].mb_Base = addr;
            sys_memory[block].mb_Size = size;
            
            range += block_size / 4;
        }

        sys_memory[block_count].mb_Base = 0;
        sys_memory[block_count].mb_Size = 0;
    }

    spinlock_init(&mmu_lock);

    mmu_user_L1.mp_entries[0] = 0;
    mmu_user_L1.mp_entries[1] = 0;
    mmu_user_L1.mp_entries[2] = 0;
    mmu_user_L1.mp_entries[3] = 0;

    /* Initialize the MMU page pool */
    mmu_pool_init(mmu_ploc + PHYS_VIRT_OFFSET, MMU_POOL_SIZE * sizeof(struct mmu_page));

    arm_flush_cache((intptr_t)&mmu_user_L1, sizeof(mmu_user_L1));
    arm_flush_cache((intptr_t)&mmu_kernel_L1, sizeof(mmu_kernel_L1));
    arm_flush_cache((intptr_t)&mmu_kernel_L2, sizeof(mmu_kernel_L2));

    mmu_tlb_flush_all();
}

void mirror_page(uintptr_t virt)
{
    int idx_l1 = (virt >> 30) & 0x1ff;

    /* For 0..4GB create a shadow in the 4..8GB and -4..0GB areas */
    if (0 == (virt  & 0xffff000000000000))
    {
        if (idx_l1 < 4) {
            struct mmu_page *tbl_kernel;
            struct mmu_page *tbl;

            /* Update user space area */
            __asm__ volatile("mrs %0, TTBR0_EL1":"=r"(tbl));
            tbl = (struct mmu_page *)((uintptr_t)tbl + PHYS_VIRT_OFFSET);
            tbl->mp_entries[idx_l1 + 4] = tbl->mp_entries[idx_l1];
            
            /* Now fetch kernel table and update the topmost region, too */
            __asm__ volatile("mrs %0, TTBR1_EL1":"=r"(tbl_kernel));
            tbl_kernel = (struct mmu_page *)((uintptr_t)tbl_kernel + PHYS_VIRT_OFFSET);
            tbl_kernel->mp_entries[508 + idx_l1] = tbl->mp_entries[idx_l1];
        }
    }
}

void mmu_map(uintptr_t phys, uintptr_t virt, uintptr_t length, uint32_t attr_low, uint32_t attr_high)
{
    uint64_t attrs = make_attrs(attr_low, attr_high);
    uintptr_t virt_start = virt;
    uintptr_t mapped;
 
    DMAP(kprintf("[MMU] mmu_map(%p, %p, %x, %04x00000000%04x)\n", phys, virt, length, attr_high, attr_low));
 
    /* Partial pages are not mapped, same as before. Callers pass page aligned ranges */
    length &= ~(SZ_4K - 1);
    mapped = length;
 
    spinlock_acquire(&mmu_lock);
 
    /* Align virt up to 2M boundary with 4K pages */
    while ((virt & (SZ_2M - 1)) && length)
    {
        put_4k_page(phys, virt, attrs);
        phys += SZ_4K;
        virt += SZ_4K;
        length -= SZ_4K;
    }
 
    /* Now check if phys is sill aligned to 2M boundary. If not, continue using 4K pages */
    if ((phys & (SZ_2M - 1)) == 0)
    {
        while (length >= SZ_2M)
        {
            put_2m_page(phys, virt, attrs);
            phys += SZ_2M;
            virt += SZ_2M;
            length -= SZ_2M;
        }
    }
 
    /* Put the rest using 4K pages */
    while (length)
    {
        put_4k_page(phys, virt, attrs);
        phys += SZ_4K;
        virt += SZ_4K;
        length -= SZ_4K;
    }
 
    if (mapped)
        tidy_range(virt_start, mapped);
 
    mmu_commit();
 
    spinlock_release(&mmu_lock);
}
 
void mmu_unmap(uintptr_t virt, uintptr_t length)
{
    uintptr_t virt_start, left;
 
    DMAP(kprintf("[MMU] mmu_unmap(%p, %x)\n", virt, length));
 
    /* Unmap every page touched by the range */
    left = ((virt + length + SZ_4K - 1) & ~(SZ_4K - 1)) - (virt & ~(SZ_4K - 1));
    virt &= ~(SZ_4K - 1);
    virt_start = virt;
    length = left;
 
    if (!left)
        return;
 
    spinlock_acquire(&mmu_lock);
 
    while (left)
    {
        uint64_t *e1 = &mmu_root(virt)->mp_entries[L1_IDX(virt)];
        uintptr_t span1 = SZ_1G - (virt & (SZ_1G - 1));
 
        if (span1 > left)
            span1 = left;
 
        if ((*e1 & 1) == 0)
        {
            /* Nothing mapped here */
        }
        else if (span1 == SZ_1G && !is_static_slot(virt))
        {
            uint64_t old = *e1;
 
            set_l1_entry(virt, 0, 0);
            if ((old & DESC_TYPE_MASK) == DESC_TABLE)
                retire_l2_tree(desc_to_table(old));
        }
        else
        {
            struct mmu_page *l2 = ensure_table(e1, 1, virt);
            uintptr_t v = virt;
            uintptr_t l = span1;
 
            while (l)
            {
                uint64_t *e2 = &l2->mp_entries[L2_IDX(v)];
                uintptr_t span2 = SZ_2M - (v & (SZ_2M - 1));
 
                if (span2 > l)
                    span2 = l;
 
                if ((*e2 & 1) == 0)
                {
                    /* Nothing mapped here */
                }
                else if (span2 == SZ_2M)
                {
                    uint64_t old = *e2;
 
                    *e2 = 0;
                    if ((old & DESC_TYPE_MASK) == DESC_TABLE)
                        retire_table(desc_to_table(old));
                }
                else
                {
                    struct mmu_page *l3 = ensure_table(e2, 2, v);
 
                    for (uintptr_t x = v; x < v + span2; x += SZ_4K)
                        l3->mp_entries[L3_IDX(x)] = 0;
                }
 
                v += span2;
                l -= span2;
            }
        }
 
        virt += span1;
        left -= span1;
    }
 
    tidy_range(virt_start, length);
 
    mmu_commit();
 
    spinlock_release(&mmu_lock);
}

#ifdef PSMAC
/*
    Aurora68: the 68k address space is the first 4 GiB of user space, i.e. user L1 entries 0..3
    (plus their aliases, see l1_slots). The Mac MMU shim keeps one ARM subtree per 68030 table
    configuration and swaps them in here instead of rebuilding thousands of mappings on every
    SwapMMUMode. A subtree that is not installed is never touched by mmu_map/mmu_unmap.
*/
void mmu_low4g_get(uint64_t out[4])
{
    spinlock_acquire(&mmu_lock);
    for (int i = 0; i < 4; i++)
        out[i] = mmu_root(0)->mp_entries[i];
    spinlock_release(&mmu_lock);
}

void mmu_low4g_set(const uint64_t in[4])
{
    spinlock_acquire(&mmu_lock);
    for (int i = 0; i < 4; i++)
        set_l1_entry((uintptr_t)i << 30, 0, 0);          /* break ... */
    mmu_tlb_flush_all();
    for (int i = 0; i < 4; i++)
        set_l1_entry((uintptr_t)i << 30, in[i], 0);      /* ... before make */
    mmu_tlb_flush_all();
    spinlock_release(&mmu_lock);
}

/* The TLB entries of one 4 KiB page of the 68k's space (26 November 2026, docs/23 section 19). The first 4 GiB appear
   three times: at their own addresses, again at +4 GiB (L1 entries 4-7 point at the same tables, mirror_page and
   mmu_space_new: the translator's `ldr w, [x_base, w_disp, UXTW]` with a negative displacement lands there, as a
   68020 memory-indirect operand with a negative base displacement does) and, outside mac.mmu=full's spaces, at the
   top of the kernel's half (L1 entries 508-511). One descriptor, three addresses, three TLB entries. Until now a page
   replaced or removed was invalidated at its own address alone, so on a real core the +4 GiB entry went on naming
   the old page; QEMU, which drops its whole TLB whenever TTBR0's ASID changes, never kept one. A/UX swaps each
   process's u-area and kernel stack in at one address ($12FFF000) with a PFLUSH of that page, and its kernel then read
   a pointer from its stack through `([-16,A6])`: the owner's Pi read the previous process's stack there, the new
   process's page table entry went astray and A/UX panicked. asid_field: the ASID in bits 63-48, or ~0 for every ASID. */
int mmu_tlbi_low_only;                       /* the hardware self-check alone (psmac_mmu.c): the flush as it was */
static inline void tlbi_page_everywhere(uint64_t asid_field, uintptr_t virt)
{
    uint64_t p = (virt >> 12) & 0xfffffull;
    if (asid_field == ~0ull) {
        __asm__ volatile("dsb ishst\n\ttlbi vaae1is, %0" : : "r"(p) : "memory");
        if (!mmu_tlbi_low_only) {
            __asm__ volatile("tlbi vaae1is, %0" : : "r"(p | (1ull << 20)) : "memory");
            __asm__ volatile("tlbi vaae1is, %0" : : "r"(p | (0xffffffull << 20)) : "memory");
        }
    } else {
        __asm__ volatile("dsb ishst\n\ttlbi vae1is, %0" : : "r"(asid_field | p) : "memory");
        if (!mmu_tlbi_low_only) {
            __asm__ volatile("tlbi vae1is, %0" : : "r"(asid_field | p | (1ull << 20)) : "memory");
            __asm__ volatile("tlbi vaae1is, %0" : : "r"(p | (0xffffffull << 20)) : "memory");
        }
    }
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}

/*
    Aurora68, mac.mmu=full (psmac/psmac_mmu.c, docs/23-mmu.md, phase 2): one 4 KiB page of the installed
    low-4G subtree at a time, for the soft TLB that maps the 68k's pages on their first use and drops them
    when the 68k flushes its ATC. phys == ~0 removes the page. A page that was not mapped needs no TLB
    maintenance (an entry that faults is never cached); one that was is invalidated by its address alone
    (TLBI VAAE1IS, every ASID) when tlbi is set, or by the caller's mmu_low4g_flush() after a batch. Nothing
    is merged into blocks or tidied away: the tables of a set live as long as the set. Returns 1 when a
    valid entry was replaced or removed.
*/
int mmu_low4g_page(uintptr_t virt, uintptr_t phys, uint32_t attr_low, int tlbi)
{
    int was = 0;
    virt &= ~(SZ_4K - 1);
    spinlock_acquire(&mmu_lock);
    uint64_t *e1 = &mmu_root(virt)->mp_entries[L1_IDX(virt)];
    if (phys == ~(uintptr_t)0 && (*e1 & DESC_TYPE_MASK) != DESC_TABLE && !(*e1 & 1)) {
        spinlock_release(&mmu_lock);
        return 0;
    }
    struct mmu_page *l2 = ensure_table(e1, 1, virt);
    uint64_t *e2 = &l2->mp_entries[L2_IDX(virt)];
    if (phys == ~(uintptr_t)0 && !(*e2 & 1)) {
        spinlock_release(&mmu_lock);
        return 0;
    }
    struct mmu_page *l3 = ensure_table(e2, 2, virt);
    uint64_t *e3 = &l3->mp_entries[L3_IDX(virt)];
    uint64_t nd = (phys == ~(uintptr_t)0) ? 0 : ((phys & DESC_OA_MASK) | make_attrs(attr_low, 0) | DESC_PAGE);
    was = (*e3 & 1) != 0;
    if (was && *e3 != nd) {
        *e3 = 0;                                   /* break ... */
        if (tlbi)
            tlbi_page_everywhere(~0ull, virt);     /* its own address and its two aliases */
    }
    *e3 = nd;                                      /* ... before make */
    mmu_publish();
    mmu_tlb_paranoid();
    spinlock_release(&mmu_lock);
    return was;
}

/* the L3 descriptor of a 4 KiB page of the installed subtree (0 if none: not mapped, or inside a block) */
uint64_t mmu_low4g_entry(uintptr_t virt)
{
    uint64_t d = mmu_root(virt)->mp_entries[L1_IDX(virt)];
    if ((d & DESC_TYPE_MASK) != DESC_TABLE) return 0;
    d = desc_to_table(d)->mp_entries[L2_IDX(virt)];
    if ((d & DESC_TYPE_MASK) != DESC_TABLE) return 0;
    return desc_to_table(d)->mp_entries[L3_IDX(virt)];
}

/* the whole-TLB flush that ends a batch of mmu_low4g_page(..., 0) calls */
void mmu_low4g_flush(void)
{
    spinlock_acquire(&mmu_lock);
    mmu_tlb_flush_all();
    spinlock_release(&mmu_lock);
}

/* Re-create, in the installed subtree, every mapping of the subtree described by from[] (used to
   give a fresh Mac map the Pi peripheral windows that live inside the first 4 GiB). */
void mmu_low4g_clone(const uint64_t from[4])
{
    for (int i = 0; i < 4; i++) {
        uint64_t d1 = from[i];
        uintptr_t va1 = (uintptr_t)i << 30;
        if (!(d1 & 1)) continue;
        if ((d1 & DESC_TYPE_MASK) == DESC_BLOCK) {
            mmu_map(d1 & DESC_OA_MASK & ~(SZ_1G - 1), va1, SZ_1G, (uint32_t)(d1 & 0xffc), (uint32_t)(d1 >> 48));
            continue;
        }
        struct mmu_page *l2 = desc_to_table(d1);
        for (int j = 0; j < 512; j++) {
            uint64_t d2 = l2->mp_entries[j];
            uintptr_t va2 = va1 | ((uintptr_t)j << 21);
            if (!(d2 & 1)) continue;
            if ((d2 & DESC_TYPE_MASK) == DESC_BLOCK) {
                mmu_map(d2 & DESC_OA_MASK & ~(SZ_2M - 1), va2, SZ_2M, (uint32_t)(d2 & 0xffc), (uint32_t)(d2 >> 48));
                continue;
            }
            struct mmu_page *l3 = desc_to_table(d2);
            for (int k = 0; k < 512; k++) {
                uint64_t d3 = l3->mp_entries[k];
                if (d3 & 1)
                    mmu_map(d3 & DESC_OA_MASK, va2 | ((uintptr_t)k << 12), SZ_4K, (uint32_t)(d3 & 0xffc), (uint32_t)(d3 >> 48));
            }
        }
    }
}
/*
    Aurora68, mac.mmu=full, phase 3 (docs/23-mmu.md section 12): address spaces. Each 68030 configuration the soft
    TLB keeps (a root pointer, user or supervisor view) gets an L1 table of its own and an ASID: entries 0..3 point
    to the space's own L2 tables (4..7 repeat them, the 4..8 GiB alias), the rest is the user L1 of the moment, and
    the Pi windows inside the first 4 GiB are copied in from the pristine subtree. A switch is a write of TTBR0_EL1
    with the space's ASID and needs no TLB maintenance; the 68k's pages are mapped non-global (MMU_NG), so the TLB
    keeps every space's translations apart. These functions never touch mmu_user_L1 or the kernel's alias of the
    first 4 GiB in TTBR1 (mmu_space_leave_alias takes that one away while spaces are in use: its entries are global
    and could only show one space).
*/
static struct mmu_page *space_l2_of(const uint64_t pristine[4], int i)
{
    uint64_t d = pristine[i];
    return ((d & DESC_TYPE_MASK) == DESC_TABLE) ? desc_to_table(d) : NULL;
}

/* the valid level 2 entries of a low-4-GiB subtree, in the log: the Pi's windows that the spaces leave out
   (psmac_mmu.c, docs/23 section 21) */
void mmu_low4g_describe(const uint64_t l1[4])
{
    for (int i = 0; i < 4; i++) {
        struct mmu_page *p = space_l2_of(l1, i);
        for (int j = 0; p && j < 512; j++) {
            uint64_t e = p->mp_entries[j];
            int k = j;
            if (!(e & 1)) continue;
            while (k + 1 < 512 && (p->mp_entries[k + 1] & 1) && (p->mp_entries[k + 1] & DESC_TYPE_MASK) == (e & DESC_TYPE_MASK) &&
                   ((e & DESC_TYPE_MASK) == DESC_TABLE || (p->mp_entries[k + 1] & DESC_OA_MASK) == (e & DESC_OA_MASK) + ((uint64_t)(k + 1 - j) << 21)))
                k++;
            kprintf("[MMU]   %08x-%08x: %s%08x, attributes %03x\n", (uint32_t)(((uint32_t)i << 30) | ((uint32_t)j << 21)),
                    (uint32_t)((((uint32_t)i << 30) | ((uint32_t)(k + 1) << 21)) - 1), (e & DESC_TYPE_MASK) == DESC_TABLE ? "pages, table at " : "blocks at ",
                    (uint32_t)(e & DESC_OA_MASK), (uint32_t)(e & 0xffc));
            j = k;
        }
    }
}

void *mmu_space_new(const uint64_t pristine[4])
{
    spinlock_acquire(&mmu_lock);
    struct mmu_page *l1 = get_4k_page();
    for (int i = 0; i < 512; i++)
        l1->mp_entries[i] = i < 8 ? 0 : mmu_user_L1.mp_entries[i];
    for (int i = 0; i < 4; i++) {
        struct mmu_page *l2 = get_4k_page(), *p = space_l2_of(pristine, i);
        for (int j = 0; j < 512; j++)
            l2->mp_entries[j] = p ? p->mp_entries[j] : 0;
        l1->mp_entries[i] = l1->mp_entries[i + 4] = table_to_desc(l2);
    }
    mmu_publish();
    spinlock_release(&mmu_lock);
    return l1;
}

/* the value of TTBR0_EL1 for a space */
uint64_t mmu_space_ttbr(void *l1, unsigned asid)
{
    return ((uintptr_t)l1 - PHYS_VIRT_OFFSET) | ((uint64_t)(asid & 0xff) << 48);
}

/* empty a space again (every 68k page unmapped, the Pi windows kept), for reuse under another ASID or key; the
   L3 tables go back to the pool after a TLB flush of every ASID (rare: a space is reset when it is evicted) */
void mmu_space_reset(void *l1v, const uint64_t pristine[4])
{
    struct mmu_page *l1 = l1v;
    spinlock_acquire(&mmu_lock);
    for (int i = 0; i < 4; i++) {
        struct mmu_page *l2 = desc_to_table(l1->mp_entries[i]), *p = space_l2_of(pristine, i);
        for (int j = 0; j < 512; j++) {
            uint64_t e = l2->mp_entries[j], q = p ? p->mp_entries[j] : 0;
            if (e == q) continue;
            l2->mp_entries[j] = q;
            if ((e & DESC_TYPE_MASK) == DESC_TABLE && (!p || (q & DESC_TYPE_MASK) != DESC_TABLE || desc_to_table(q) != desc_to_table(e)))
                retire_table(desc_to_table(e));
        }
    }
    mmu_publish();
    mmu_commit();
    spinlock_release(&mmu_lock);
}

/* one 4 KiB page of a space, installed or not (phys == ~0 removes it). An entry replaced or removed is invalidated
   for this ASID alone (TLBI VAE1IS) when tlbi is set, else by the caller's mmu_space_flush_asid after a batch.
   Returns 1 when a valid entry was replaced or removed. */
/* mac.mmutlb=flush (26 November 2026, docs/23 section 18, a diagnostic): every TLB entry of every ASID dropped after each
   change to a space's descriptors, each install of a space and each PFLUSH, as QEMU's softmmu in effect does (it flushes
   its whole TLB at a TTBR change). A/UX failed on the owner's Pi where the same kernel goes on under QEMU. */
int psmac_tlb_paranoid;
void mmu_tlb_paranoid(void)
{
    if (psmac_tlb_paranoid)
        __asm__ volatile("dsb ishst\n\ttlbi vmalle1is\n\tdsb ish\n\tisb" ::: "memory");
}
int mmu_space_page(void *l1v, unsigned asid, uintptr_t virt, uintptr_t phys, uint32_t attr_low, int tlbi)
{
    struct mmu_page *l1 = l1v;
    int was;
    virt &= ~(SZ_4K - 1);
    spinlock_acquire(&mmu_lock);
    uint64_t *e1 = &l1->mp_entries[L1_IDX(virt)];
    if ((*e1 & DESC_TYPE_MASK) != DESC_TABLE) {
        spinlock_release(&mmu_lock);
        return 0;
    }
    struct mmu_page *l2 = desc_to_table(*e1);
    uint64_t *e2 = &l2->mp_entries[L2_IDX(virt)];
    if (phys == ~(uintptr_t)0 && (*e2 & DESC_TYPE_MASK) != DESC_TABLE) {
        spinlock_release(&mmu_lock);
        return 0;
    }
    if ((*e2 & 1) && (*e2 & DESC_TYPE_MASK) != DESC_TABLE) {     /* a block (the Pi windows): never the 68k's */
        spinlock_release(&mmu_lock);
        return 0;
    }
    struct mmu_page *l3 = ensure_table(e2, 2, virt);
    uint64_t *e3 = &l3->mp_entries[L3_IDX(virt)];
    uint64_t nd = (phys == ~(uintptr_t)0) ? 0 : ((phys & DESC_OA_MASK) | make_attrs(attr_low, 0) | DESC_PAGE);
    was = (*e3 & 1) != 0;
    if (was && *e3 != nd) {
        *e3 = 0;                                   /* break ... */
        if (tlbi)
            tlbi_page_everywhere((uint64_t)(asid & 0xff) << 48, virt);   /* its own address and the +4 GiB shadow */
    }
    *e3 = nd;                                      /* ... before make */
    mmu_publish();
    mmu_tlb_paranoid();
    spinlock_release(&mmu_lock);
    return was;
}

/* the L3 descriptor of a 4 KiB page of a space (0 if none) */
uint64_t mmu_space_entry(void *l1v, uintptr_t virt)
{
    struct mmu_page *l1 = l1v;
    uint64_t d = l1->mp_entries[L1_IDX(virt)];
    if ((d & DESC_TYPE_MASK) != DESC_TABLE) return 0;
    d = desc_to_table(d)->mp_entries[L2_IDX(virt)];
    if ((d & DESC_TYPE_MASK) != DESC_TABLE) return 0;
    return desc_to_table(d)->mp_entries[L3_IDX(virt)];
}

/* every translation of one ASID (a PFLUSHA of the spaces in use, a space's reuse) */
void mmu_space_flush_asid(unsigned asid)
{
    __asm__ volatile(
        "dsb ishst              \n"
        "tlbi aside1is, %0      \n"
        "dsb ish                \n"
        "isb                    \n" : : "r"((uint64_t)(asid & 0xff) << 48) : "memory");
}

/* switch this core to a space: no TLB maintenance, the ASID keeps the spaces apart */
void mmu_space_install(uint64_t ttbr)
{
    __asm__ volatile("msr TTBR0_EL1, %0\n\tisb" : : "r"(ttbr) : "memory");
    mmu_tlb_paranoid();
}

/* The kernel's alias of the first 4 GiB in TTBR1 (L1 entries 508..511, l1_slots) holds global entries, which
   would show the shared subtree whatever space is installed: while spaces are in use it is emptied, and an access
   through it (a 68k address computation that wrapped below zero) faults and is emulated (psmac_mmufault.c). */
void mmu_space_leave_alias(void)
{
    spinlock_acquire(&mmu_lock);
    struct mmu_page *k = mmu_root(~0ULL);
    for (int i = 0; i < 4; i++)
        k->mp_entries[508 + i] = 0;
    mmu_publish();
    mmu_tlb_flush_all();
    spinlock_release(&mmu_lock);
}
#endif
