/*
 * psmac_bus.c — Macintosh host memory map + bus bridge transport for the Emu68 fork.
 * Copyright (c) 2026 PiStorm-SE project. MPL-2.0.
 *
 * The 68k address space is served as follows (IIci donor ROM, 32-bit physical map):
 *   0x00000000..RAM      Pi RAM (bank A). The first PSMAC_FB_SIZE bytes are mapped read-only so that
 *                        writes trap and are mirrored to the motherboard (RBV frame buffer lives there).
 *   0x40800000/0x40000000 ROM image loaded from the initramfs (read-only).
 *   0x50000000..0x50FFFFFF I/O  -> bridge.   0xF0000000.. slot space -> bridge.
 *   everything else      unpopulated: reads return a fixed pattern, writes are ignored.
 *
 * Transport: on the real PiStorm this is the CPLD/GPIO protocol; in the software testbed it is the
 * Pi's mini UART (QEMU -serial #1) connected to the MAME bridge CPU. A service core owns the
 * transport: core 0 posts requests in a shared slot, the service core executes them and, when idle,
 * polls the interrupt level so that ctx->INTF.IPL tracks the motherboard even without bus traffic.
 */
#include <stdint.h>
#include <stdbool.h>
#include "support.h"
#include "M68k.h"
#include "A64.h"
#include "globals.h"
#include "ps_protocol.h"
#include "psmac.h"
#include "devicetree.h"

void mmu_map(uintptr_t phys, uintptr_t virt, uintptr_t length, uint32_t attr_low, uint32_t attr_high);
uintptr_t mmu_virt2phys(uintptr_t addr);

#define PERIPH_VA        0x3f000000ull   /* relocated out of the Mac's reach (see start_rpi64.c) */
#define AUX_ENABLES      (PERIPH_VA + 0x215004)
#define AUX_MU_IO        (PERIPH_VA + 0x215040)
#define AUX_MU_IER       (PERIPH_VA + 0x215044)
#define AUX_MU_IIR       (PERIPH_VA + 0x215048)
#define AUX_MU_LCR       (PERIPH_VA + 0x21504c)
#define AUX_MU_MCR       (PERIPH_VA + 0x215050)
#define AUX_MU_LSR       (PERIPH_VA + 0x215054)
#define AUX_MU_CNTL      (PERIPH_VA + 0x215060)
#define AUX_MU_BAUD      (PERIPH_VA + 0x215068)
#define KERNEL_ALIAS     0xffffff9000000000ull

extern struct M68KState *__m68k_state;
extern struct MemoryBlock *sys_memory;

static uintptr_t ram_phys, rom_phys;
static uint32_t  ram_size;
static volatile int transport_up, transport_dead, service_running;
static volatile int svc_phase; /* 0 idle, 1 writing req, 2 reading reply, 3 heartbeat */
static volatile uint32_t svc_op, svc_addr;
static volatile uint8_t last_ipl, last_flags;
static uint32_t bus_errors, timeouts;
static volatile uint32_t xfer_count, last_addr;
uint64_t psmac_fault_elr;   /* set by the abort dispatcher: who faulted? */
/* Emu68's own EL0 code (JIT helpers, debug prints) may legitimately touch the Pi peripherals, which share
   the 68k's address space at 0xF2000000-0xF3FFFFFF but are mapped EL1-only in this fork. Perform those
   accesses here; Mac (JIT-generated) code faulting on the same window is forwarded to the bridge. */
static inline int emu68_own_fault(void) { return (psmac_fault_elr >> 32) == 0xffffff80ull; }   /* Emu68 text; JIT code is at 0xffffffe0... */
static inline int in_periph_window(uint64_t a) { return a >= PERIPH_VA && a < 0x40000000ull; }
static volatile uint32_t dev_cnt[8]; /* VIA1, SCC, SCSI, ASC, SWIM, RBV, FB, other */
static inline int dev_class(uint32_t a) { uint32_t o = a & 0x000fffff; if (a < 0x80000) return 6; if ((a & 0xff000000) != 0x50000000) return 7; if (o < 0x2000 || (o >= 0x40000 && o < 0x42000)) return 0; if (o >= 0x4000 && o < 0x6000) return 1; if ((o >= 0x10000 && o < 0x14000) || (o >= 0x6000 && o < 0x8000)) return 2; if (o >= 0x14000 && o < 0x16000) return 3; if (o >= 0x16000 && o < 0x18000) return 4; if (o >= 0x24000 && o < 0x28000) return 5; return 7; }

static inline uint64_t cnt_now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0":"=r"(v)); return v; }
static inline uint64_t cnt_frq(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0":"=r"(v)); return v; }

uint32_t psmac_ram_size(void) { return ram_size; }

/* ---------------- mini UART byte transport ---------------- */
static void uart_init(void)
{
    wr32le(AUX_ENABLES, rd32le(AUX_ENABLES) | 1);
    wr32le(AUX_MU_CNTL, 0);
    wr32le(AUX_MU_IER, 0);
    wr32le(AUX_MU_LCR, 3);
    wr32le(AUX_MU_MCR, 0);
    wr32le(AUX_MU_BAUD, 270);
    wr32le(AUX_MU_IIR, 0xc6);
    wr32le(AUX_MU_CNTL, 3);
    transport_up = 1;
}
static inline void uart_putc(uint8_t c)
{
    while ((rd32le(AUX_MU_LSR) & 0x20) == 0) ;
    wr32le(AUX_MU_IO, c);
}
/* returns -1 on timeout */
static int uart_getc(uint64_t deadline)
{
    while ((rd32le(AUX_MU_LSR) & 1) == 0)
        if (cnt_now() > deadline) return -1;
    return rd32le(AUX_MU_IO) & 0xff;
}

/* ---------------- semihosting FIFO transport (QEMU -semihosting-config enable=on) ----------------
   One host syscall per direction instead of one per byte through the UART model. The MAME bridge
   opens /tmp/psmac.req (read) and /tmp/psmac.rep (write); we open them the other way round. */
static int use_semihost = 0;            /* mac.bridge=semihost */
static int64_t sh_req = -1, sh_rep = -1;
static int64_t semihost(uint32_t op, void *param)
{
    register uint64_t x0 __asm__("x0") = op;
    register uint64_t x1 __asm__("x1") = (uint64_t)param;
    __asm__ volatile("hlt #0xf000" : "+r"(x0) : "r"(x1) : "memory");
    return (int64_t)x0;
}
static int sh_swap = 0;   /* QEMU may read the parameter block little-endian while we run big-endian */
static inline uint64_t shw(uint64_t v) { return sh_swap ? __builtin_bswap64(v) : v; }
static int64_t sh_open(const char *name, uint64_t mode)
{
    uint64_t p[3] = { shw((uint64_t)name), shw(mode), shw(strlen(name)) };
    return semihost(0x01, p);
}
static int sh_write(int64_t fd, const void *buf, uint64_t len)
{
    /* SYS_WRITE returns the number of bytes NOT written; a FIFO under back-pressure can take a
       partial write, which would desynchronise the 12-byte request framing: loop until done. */
    uint64_t done = 0;
    while (done < len) {
        uint64_t p[3] = { shw((uint64_t)fd), shw((uint64_t)buf + done), shw(len - done) };
        int64_t left = semihost(0x05, p);
        if (left < 0 || (uint64_t)left > len - done) return 0;
        uint64_t n = (len - done) - (uint64_t)left;
        if (n == 0) return 0;
        done += n;
    }
    return 1;
}
static int sh_read(int64_t fd, void *buf, uint64_t len)
{
    uint64_t got = 0;
    while (got < len) {
        uint64_t p[3] = { shw((uint64_t)fd), shw((uint64_t)buf + got), shw(len - got) };
        int64_t notread = semihost(0x06, p);
        if (notread < 0 || (uint64_t)notread > len - got) return 0;
        uint64_t n = (len - got) - (uint64_t)notread;
        if (n == 0) return 0;   /* EOF */
        got += n;
    }
    return 1;
}
static int semihost_init(void)
{
    static const char hello[] = "[PSMAC] semihosting SYS_WRITE0 probe OK\n";
    semihost(0x04, (void *)hello);                       /* appears on QEMU's stdout if HLT is intercepted */
    sh_swap = 0;
    sh_req = sh_open("/tmp/psmac.req", 5 /* wb */);
    if (sh_req < 0) { sh_swap = 1; sh_req = sh_open("/tmp/psmac.req", 5); }
    sh_rep = sh_req >= 0 ? sh_open("/tmp/psmac.rep", 1 /* rb */) : -1;
    kprintf("[PSMAC] semihosting bridge: req fd %d, rep fd %d (block byte-swap %d)\n", (int)sh_req, (int)sh_rep, sh_swap);
    transport_up = 1;
    return sh_req >= 0 && sh_rep >= 0;
}
void psmac_select_transport(int semihost_mode) { use_semihost = semihost_mode; }

/* Options come from /chosen bootargs (this Emu68 branch does not parse the cmdline itself). */
static int opts_read;
static void psmac_read_options(void)
{
    if (opts_read) return;
    opts_read = 1;
    of_node_t *c = dt_find_node("/chosen");
    of_property_t *p = c ? dt_find_property(c, "bootargs") : NULL;
    if (!p || !p->op_value) return;
    const char *args = p->op_value, *tok;
    if ((tok = find_token(args, "mac.ram="))) { tok += 8; uint32_t mb = 0; while (*tok >= '0' && *tok <= '9') mb = mb * 10 + (*tok++ - '0'); globals.psmac_ram_mb = mb; }
    if (find_token(args, "mac.bridge=semihost")) use_semihost = 1;
    kprintf("[PSMAC] options: ram=%d MiB bridge=%s (bootargs: %s)\n", globals.psmac_ram_mb, use_semihost ? "semihost" : "uart", args);
}

/* One bridge transaction. Must only run on the transport owner. */
static void bridge_xfer(uint8_t op, uint8_t size, uint8_t flags, uint32_t addr, uint32_t data, struct psmac_reply *r)
{
    if (!transport_up) { if (use_semihost) { if (!semihost_init()) transport_dead = 1; } else uart_init(); }
    if (transport_dead) { if (r) { r->status = 0; r->ipl = 0; r->flags = 0; r->data = 0xffffffff; } return; }
    uint8_t req[12] = { op, size, flags, 0,
        (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16), (uint8_t)(addr >> 24),
        (uint8_t)data, (uint8_t)(data >> 8), (uint8_t)(data >> 16), (uint8_t)(data >> 24) };
    uint8_t rep[8];
    svc_op = op; svc_addr = addr;
    if (use_semihost) {
        svc_phase = 1;
        if (!sh_write(sh_req, req, 12)) { if (++timeouts == 1) kprintf("[PSMAC] semihosting bridge write failed\n"); transport_dead = 1; if (r) { r->status = 0; r->ipl = 0; r->flags = 0; r->data = 0xffffffff; } return; }
        if (flags & PSMAC_FLAG_POSTED) { svc_phase = 0; return; }
        svc_phase = 2;
        if (!sh_read(sh_rep, rep, 8)) { if (++timeouts == 1) kprintf("[PSMAC] semihosting bridge read failed/EOF\n"); transport_dead = 1; if (r) { r->status = 0; r->ipl = 0; r->flags = 0; r->data = 0xffffffff; } return; }
    } else {
        for (int i = 0; i < 12; i++) uart_putc(req[i]);
        if (flags & PSMAC_FLAG_POSTED) return;
        uint64_t deadline = cnt_now() + cnt_frq() * 5;   /* 5 s: the bridge may be paused/halted */
        for (int i = 0; i < 8; i++) {
            int c = uart_getc(deadline);
            if (c < 0) {
                if (++timeouts == 1) kprintf("[PSMAC] bridge timeout (op %d addr %08x): is the MAME bridge connected?\n", op, addr);
                transport_dead = 1;
                if (r) { r->status = 0; r->ipl = 0; r->flags = 0; r->data = 0xffffffff; }
                return;
            }
            rep[i] = c;
        }
    }
    svc_phase = 0;
    last_ipl = rep[1]; last_flags = rep[2];
    if (__m68k_state) __m68k_state->INTF.IPL = rep[1];
    if (r) { r->status = rep[0]; r->ipl = rep[1]; r->flags = rep[2]; r->data = rep[4] | (rep[5] << 8) | (rep[6] << 16) | ((uint32_t)rep[7] << 24); }
    if (rep[0]) bus_errors++;
}

/* ---------------- request hand-off to the service core ---------------- */
struct slot { volatile uint32_t state; uint8_t op, size, flags; uint32_t addr, data; struct psmac_reply rep; };
static struct slot slot __attribute__((aligned(64)));
enum { SLOT_FREE = 0, SLOT_REQ = 1, SLOT_DONE = 2 };
static volatile uint8_t slot_lock;

static void xfer(uint8_t op, uint8_t size, uint8_t flags, uint32_t addr, uint32_t data, struct psmac_reply *r)
{
    if (!service_running) { bridge_xfer(op, size, flags, addr, data, r); return; }
    while (__atomic_test_and_set(&slot_lock, __ATOMIC_ACQUIRE)) __asm__ volatile("yield");
    slot.op = op; slot.size = size; slot.flags = flags; slot.addr = addr; slot.data = data;
    __atomic_store_n(&slot.state, SLOT_REQ, __ATOMIC_RELEASE);
    __asm__ volatile("sev");
    { uint64_t t0 = cnt_now(), warn = t0 + cnt_frq() * 3; int warned = 0;
      while (__atomic_load_n(&slot.state, __ATOMIC_ACQUIRE) != SLOT_DONE) {
          __asm__ volatile("yield");
          if (!warned && cnt_now() > warn) { warned = 1;
              kprintf("[PSMAC] bridge stall: waiting on op %d size %d addr %08x; service core phase %d (op %d addr %08x), xfers %u\n", op, size, addr, svc_phase, svc_op, svc_addr, xfer_count); }
      } }
    if (r) *r = slot.rep;
    __atomic_store_n(&slot.state, SLOT_FREE, __ATOMIC_RELEASE);
    __atomic_clear(&slot_lock, __ATOMIC_RELEASE);
}

/* 68k PC breadcrumbs: the service core samples the JIT's exported PC and records every change.
   Dumped when the 68k stops producing bus traffic (stall / fatal exception on core 0). */
#define PCRING 1024
static uint32_t pcring[PCRING]; static uint32_t pcring_n;
volatile uint32_t psmac_exc_flag;   /* set by the 68k exception entry on the first unexpected vector */
static void pcring_dump(void)
{
    uint32_t n = pcring_n < PCRING ? pcring_n : PCRING, start = pcring_n - n;
    kprintf("[PSMAC] last %u distinct 68k PC samples (oldest first):\n", n);
    for (uint32_t i = 0; i < n; i += 8) {
        kprintf("[PSMAC]  ");
        for (uint32_t k = i; k < i + 8 && k < n; k++) kprintf(" %08x", pcring[(start + k) % PCRING]);
        kprintf("\n");
    }
}

/* Called by the translator before translating a new block: report jumps to addresses that cannot hold
   code (odd, or not RAM/ROM in the current map), with the 68k state at that moment. */
static uint32_t mac_rd32(uint32_t logical, int *ok)
{
    int valid; uint32_t ph = psmac_mmu_l2p(logical, &valid);
    if (!valid || ph + 4 > ram_size) { *ok = 0; return 0; }
    *ok = 1; return *(const uint32_t *)(KERNEL_ALIAS + ram_phys + ph);
}
void psmac_check_pc(uint32_t pc)
{
    static int reports;
    if (reports >= 3) return;
    int valid; uint32_t ph = psmac_mmu_l2p(pc, &valid);
    int in_ram = valid && ph < ram_size, in_rom = valid && ph >= 0x40000000u && ph < 0x50000000u;
    if (!(pc & 1) && (in_ram || in_rom)) return;
    reports++;
    struct M68KState *m = __m68k_state;
    kprintf("[PSMAC:PC] translating at bad PC %08x (phys %08x %s)\n", pc, ph, valid ? (in_ram ? "RAM" : in_rom ? "ROM" : "other") : "INVALID");
    if (!m) return;
    kprintf("[PSMAC:PC]   D0-7 %08x %08x %08x %08x %08x %08x %08x %08x\n", BE32(m->D[0].u32), BE32(m->D[1].u32), BE32(m->D[2].u32), BE32(m->D[3].u32), BE32(m->D[4].u32), BE32(m->D[5].u32), BE32(m->D[6].u32), BE32(m->D[7].u32));
    kprintf("[PSMAC:PC]   A0-7 %08x %08x %08x %08x %08x %08x %08x %08x SR %04x\n", BE32(m->A[0].u32), BE32(m->A[1].u32), BE32(m->A[2].u32), BE32(m->A[3].u32), BE32(m->A[4].u32), BE32(m->A[5].u32), BE32(m->A[6].u32), BE32(m->A[7].u32), BE16(m->SR));
    uint32_t sp = BE32(m->A[7].u32); int ok;
    kprintf("[PSMAC:PC]   stack:");
    for (int i = 0; i < 16; i++) { uint32_t v = mac_rd32(sp + 4 * i, &ok); kprintf(" %08x", ok ? v : 0xdeaddead); }
    kprintf("\n[PSMAC:PC]   OS trap table 0x400:");
    for (int i = 0; i < 16; i++) { uint32_t v = mac_rd32(0x400 + 4 * i, &ok); kprintf(" %08x", ok ? v : 0xdeaddead); }
    kprintf("\n");
}

void psmac_service_core(void)
{
    psmac_read_options();
    if (!use_semihost) uart_init(); else { if (!semihost_init()) transport_dead = 1; }
    service_running = 1;
    kprintf("[PSMAC] service core running (bridge transport owner, IPL poll 1 kHz)\n");
    uint64_t period = cnt_frq() / 1000, next_poll = cnt_now() + period;
    uint64_t hb_period = cnt_frq() * 5, next_hb = cnt_now() + hb_period; uint32_t hb_last = 0;
    uint32_t pc_last = 0; int stall_dumped = 0, exc_dumped = 0;
    for (;;) {
        if (cnt_now() > next_hb) {
            svc_phase = 3;
            kprintf("[PSMAC] heartbeat: %u bus xfers (+%u), last %08x, ipl %d%s | via %u scc %u scsi %u asc %u swim %u rbv %u fb %u other %u | PC %08x SR %04x\n", xfer_count, xfer_count - hb_last, last_addr, last_ipl, transport_dead ? " TRANSPORT DEAD" : "",
                dev_cnt[0], dev_cnt[1], dev_cnt[2], dev_cnt[3], dev_cnt[4], dev_cnt[5], dev_cnt[6], dev_cnt[7],
                __m68k_state ? BE32((uint32_t)(uintptr_t)__m68k_state->PC) : 0, __m68k_state ? BE16(__m68k_state->SR) : 0);
            for (int k = 0; k < 8; k++) dev_cnt[k] = 0;
            svc_phase = 0;
            if (__m68k_state) {
                struct M68KState *m = __m68k_state;
                kprintf("[PSMAC]   D0-7 %08x %08x %08x %08x %08x %08x %08x %08x\n", BE32(m->D[0].u32), BE32(m->D[1].u32), BE32(m->D[2].u32), BE32(m->D[3].u32), BE32(m->D[4].u32), BE32(m->D[5].u32), BE32(m->D[6].u32), BE32(m->D[7].u32));
                kprintf("[PSMAC]   A0-7 %08x %08x %08x %08x %08x %08x %08x %08x ISP %08x\n", BE32(m->A[0].u32), BE32(m->A[1].u32), BE32(m->A[2].u32), BE32(m->A[3].u32), BE32(m->A[4].u32), BE32(m->A[5].u32), BE32(m->A[6].u32), BE32(m->A[7].u32), BE32(m->ISP.u32));
                uint32_t sp = BE32(m->A[7].u32); int valid; uint32_t ph = psmac_mmu_l2p(sp, &valid);
                if (valid && ph + 32 <= ram_size) {
                    const uint32_t *st = (const uint32_t *)(KERNEL_ALIAS + ram_phys + ph);
                    kprintf("[PSMAC]   stack: %08x %08x %08x %08x %08x %08x %08x %08x\n", st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7]);
                }
            }
            if (xfer_count == hb_last && !stall_dumped) { stall_dumped = 1; pcring_dump(); }
            hb_last = xfer_count; next_hb = cnt_now() + hb_period;
        }
        if (__m68k_state) {
            uint32_t pc = BE32((uint32_t)(uintptr_t)__m68k_state->PC);
            if (pc != pc_last) { pc_last = pc; pcring[pcring_n++ % PCRING] = pc; }
        }
        if (psmac_exc_flag && !exc_dumped) {
            exc_dumped = 1;
            kprintf("[PSMAC] first unexpected 68k exception (vector %03x): PC trail follows\n", psmac_exc_flag & 0xfff);
            pcring_dump();
        }
        if (__atomic_load_n(&slot.state, __ATOMIC_ACQUIRE) == SLOT_REQ) {
            xfer_count++; last_addr = slot.addr; dev_cnt[dev_class(slot.addr)]++;
            bridge_xfer(slot.op, slot.size, slot.flags, slot.addr, slot.data, &slot.rep);
            __atomic_store_n(&slot.state, SLOT_DONE, __ATOMIC_RELEASE);
            continue;
        }
        if (!transport_dead && cnt_now() > next_poll) {
            bridge_xfer(PSMAC_OP_POLL, 0, 0, 0, 0, NULL);
            next_poll = cnt_now() + period;
        }
        __asm__ volatile("yield");
    }
}

/* ---------------- I/O helpers ---------------- */
uint32_t psmac_io_read(uint32_t addr, int size)
{
    struct psmac_reply r;
    xfer(PSMAC_OP_READ, size, 0, addr, 0, &r);
    return r.data;
}
void psmac_io_write(uint32_t addr, int size, uint32_t data, int posted)
{
    xfer(PSMAC_OP_WRITE, size, posted ? PSMAC_FLAG_POSTED : 0, addr, data, NULL);
}


/* Called from the data abort handler for every access the ARM MMU did not satisfy. Return 1 = handled. */
static uint32_t trace_left = 400;
void psmac_trace(uint32_t n) { trace_left = n; }
uint64_t psmac_fault_pc;   /* JIT's 68k PC register at the fault (block granularity) */
int psmac_bus_write(uint64_t value, uint64_t value2, int size, uint64_t far)
{
    if ((far >> 32) == 1 || (far >> 32) == 0xffffffff) far &= 0xffffffff;
    if (far >> 32) return 0;
    if (in_periph_window(far) && emu68_own_fault()) {
        volatile uint8_t *p = (volatile uint8_t *)far;
        switch (size) { case 1: *p = value; break; case 2: *(volatile uint16_t *)p = value; break; case 4: *(volatile uint32_t *)p = value; break; default: *(volatile uint64_t *)p = value; break; }
        return 1;
    }
    { int valid; uint32_t phys = psmac_mmu_l2p((uint32_t)far, &valid);
      if (trace_left) { trace_left--; kprintf("[PSMAC:BUS] W%d %08x (phys %08x%s) <- %08x\n", size, (uint32_t)far, phys, valid ? "" : " INVALID", (uint32_t)value); }
      if (!valid) return 1;   /* unmapped logical page: write ignored (would be a bus error) */
      far = phys; }

    if ((far & ~0xfffull) == 0x50000) {
        /* watched low-memory page: plain RAM write, report suspicious stores into the OS trap table */
        static int reports;
        uint8_t *p = (uint8_t *)(KERNEL_ALIAS + ram_phys + far);
        uint32_t off = far & 0xfff, top = (uint32_t)(value >> 24) & 0xff;
        if (reports < 24 && off >= 0x400 && off < 0x800 && size == 4 && top != 0x00 && top != 0x40) {
            reports++;
            kprintf("[PSMAC:WATCH] W%d trap table +%03x <- %08x from 68k PC %08x (ARM ELR %p)\n", size, off, (uint32_t)value, (uint32_t)psmac_fault_pc, (void *)psmac_fault_elr);
        }
        switch (size) {
            case 1: *p = value; break; case 2: *(uint16_t *)p = value; break; case 4: *(uint32_t *)p = value; break;
            case 8: *(uint64_t *)p = value; break; case 16: *(uint64_t *)p = value; *(uint64_t *)(p + 8) = value2; break;
            default: return 0;
        }
        return 1;
    }
    if (far < PSMAC_FB_SIZE) {
        /* write-through frame buffer: local copy + mirror to the motherboard */
        uint8_t *p = (uint8_t *)(KERNEL_ALIAS + ram_phys + far);
        switch (size) {
            case 1: *p = value; psmac_io_write(far, 1, value & 0xff, 1); break;
            case 2: *(uint16_t *)p = value; psmac_io_write(far, 2, value & 0xffff, 1); break;
            case 4: *(uint32_t *)p = value; psmac_io_write(far, 4, value, 1); break;
            case 8: *(uint64_t *)p = value; psmac_io_write(far, 4, value >> 32, 1); psmac_io_write(far + 4, 4, value, 1); break;
            case 16: *(uint64_t *)p = value; *(uint64_t *)(p + 8) = value2;
                     psmac_io_write(far, 4, value >> 32, 1); psmac_io_write(far + 4, 4, value, 1);
                     psmac_io_write(far + 8, 4, value2 >> 32, 1); psmac_io_write(far + 12, 4, value2, 1); break;
            default: return 0;
        }
        return 1;
    }
    /* everything that is not local RAM/ROM goes to the motherboard, exactly like the PiStorm would */
    {
        switch (size) {
            case 1: psmac_io_write(far, 1, value & 0xff, 1); break;
            case 2: psmac_io_write(far, 2, value & 0xffff, 1); break;
            case 4: psmac_io_write(far, 4, value, 1); break;
            case 8: psmac_io_write(far, 4, value >> 32, 1); psmac_io_write(far + 4, 4, value, 1); break;
            case 16: psmac_io_write(far, 4, value >> 32, 1); psmac_io_write(far + 4, 4, value, 1);
                     psmac_io_write(far + 8, 4, value2 >> 32, 1); psmac_io_write(far + 12, 4, value2, 1); break;
            default: return 0;
        }
        return 1;
    }
}

int psmac_bus_read(uint64_t *value, uint64_t *value2, int size, uint64_t far)
{
    if ((far >> 32) == 1 || (far >> 32) == 0xffffffff) far &= 0xffffffff;
    if (far >> 32) return 0;
    if (in_periph_window(far) && emu68_own_fault()) {
        volatile uint8_t *p = (volatile uint8_t *)far;
        switch (size) { case 1: *value = *p; break; case 2: *value = *(volatile uint16_t *)p; break; case 4: *value = *(volatile uint32_t *)p; break; default: *value = *(volatile uint64_t *)p; break; }
        return 1;
    }
    { int valid; uint32_t phys = psmac_mmu_l2p((uint32_t)far, &valid);
      if (trace_left) { trace_left--; kprintf("[PSMAC:BUS] R%d %08x (phys %08x%s)\n", size, (uint32_t)far, phys, valid ? "" : " INVALID"); }
      if (!valid) { *value = 0xBAD0BAD0BAD0BAD0ull; if (value2) *value2 = *value; return 1; }
      far = phys; }

    {
        switch (size) {
            case 1: *value = psmac_io_read(far, 1) & 0xff; break;
            case 2: *value = psmac_io_read(far, 2) & 0xffff; break;
            case 4: *value = psmac_io_read(far, 4); break;
            case 8: *value = ((uint64_t)psmac_io_read(far, 4) << 32) | psmac_io_read(far + 4, 4); break;
            case 16: *value = ((uint64_t)psmac_io_read(far, 4) << 32) | psmac_io_read(far + 4, 4);
                     if (value2) { *value2 = ((uint64_t)psmac_io_read(far + 8, 4) << 32) | psmac_io_read(far + 12, 4); }
                     break;
            default: return 0;
        }
        return 1;
    }
}

/* ---------------- memory map ---------------- */
void psmac_setup_memory(void *rom, uint32_t rom_sz)
{
    psmac_read_options();
    uint32_t mb = globals.psmac_ram_mb ? globals.psmac_ram_mb : 8;
    if (mb > PSMAC_RAM_MAX_MB) { kprintf("[PSMAC] mac.ram=%d clamped to %d MiB (RAM space below the ROM)\n", mb, PSMAC_RAM_MAX_MB); mb = PSMAC_RAM_MAX_MB; }
    if (mb < 1) mb = 1;
    ram_size = mb << 20;

    uintptr_t base = (sys_memory[0].mb_Base + 0x1fffff) & ~0x1fffffull;
    uintptr_t need = ram_size + PSMAC_ROM_SIZE;
    if (sys_memory[0].mb_Base + sys_memory[0].mb_Size < base + need) {
        kprintf("[PSMAC] not enough system memory for %d MiB Mac RAM, falling back to 8 MiB\n", mb);
        ram_size = 8 << 20; need = ram_size + PSMAC_ROM_SIZE;
    }
    ram_phys = base; rom_phys = base + ram_size;
    sys_memory[0].mb_Size -= (base + need) - sys_memory[0].mb_Base;
    sys_memory[0].mb_Base = base + need;

    kprintf("[PSMAC] Mac RAM %d MiB at Pi %p, ROM at Pi %p (%d bytes from initramfs)\n", ram_size >> 20, ram_phys, rom_phys, rom_sz);


    /* ROM image */
    uint8_t *romk = (uint8_t *)(KERNEL_ALIAS + rom_phys);
    if (rom && rom_sz) {
        uint32_t n = rom_sz > PSMAC_ROM_SIZE ? PSMAC_ROM_SIZE : rom_sz;
        memcpy(romk, rom, n);
        for (uint32_t o = n; o < PSMAC_ROM_SIZE; o += n) {
            memcpy(romk + o, rom, (o + n <= PSMAC_ROM_SIZE) ? n : PSMAC_ROM_SIZE - o);
        }
    } else {
        kprintf("[PSMAC] WARNING: no ROM in initramfs!\n");
    }
    /* identity mapping (MMU disabled) and the 68030 MMU shadowing machinery */
    psmac_mmu_init(ram_phys, ram_size, rom_phys);

    /* Overlay: reset vectors come from the ROM. Copy SP/PC to RAM 0 so the generic start-up finds them. */
    memcpy((void *)(KERNEL_ALIAS + ram_phys), romk, 8);
    arm_flush_cache(KERNEL_ALIAS + ram_phys, 4096);
    arm_flush_cache(KERNEL_ALIAS + rom_phys, PSMAC_ROM_SIZE);

    globals.zorro_disable = true;
    {
        static const uint32_t probes[] = { 0x0, 0x7f000, 0x80000, 0x40800000, 0x50f01c00, 0x50f21c00, 0x50026000, 0x58000000, 0x04000000 };
        for (unsigned i = 0; i < sizeof(probes)/sizeof(probes[0]); i++)
            kprintf("[PSMAC] map %08x -> phys %p\n", probes[i], mmu_virt2phys(probes[i]));
    }
    kprintf("[PSMAC] EL1 probe read of 50f01c00 -> %02x\n", *(volatile uint8_t *)0x50f01c00ull);
    {   /* bridge latency benchmark: 200 synchronous byte reads of VIA1 ORB */
        uint64_t t0 = cnt_now(); volatile uint32_t sink = 0;
        for (int i = 0; i < 200; i++) sink += psmac_io_read(0x50f00000, 1);
        uint64_t dt = cnt_now() - t0;
        kprintf("[PSMAC] bridge latency: %d us per read (service core %s)\n", (int)(dt * 1000000 / cnt_frq() / 200), service_running ? "active" : "inactive");
    }
    kprintf("[PSMAC] reset SP %08x PC %08x\n", BE32(*(uint32_t *)romk), BE32(*(uint32_t *)(romk + 4)));
}

/* ---------------- PiStorm protocol API, redirected ---------------- */
static unsigned int r8(unsigned int a)  { return psmac_io_read(a, 1) & 0xff; }
static unsigned int r16(unsigned int a) { return psmac_io_read(a, 2) & 0xffff; }
static unsigned int r32(unsigned int a) { return psmac_io_read(a, 4); }
static uint64_t     r64(unsigned int a) { return ((uint64_t)r32(a) << 32) | r32(a + 4); }
static uint128_t    r128(unsigned int a){ uint128_t v; v.hi = r64(a); v.lo = r64(a + 8); return v; }
static void w8(unsigned int a, unsigned int d)  { psmac_io_write(a, 1, d & 0xff, 1); }
static void w16(unsigned int a, unsigned int d) { psmac_io_write(a, 2, d & 0xffff, 1); }
static void w32(unsigned int a, unsigned int d) { psmac_io_write(a, 4, d, 1); }
static void w64(unsigned int a, uint64_t d)     { w32(a, d >> 32); w32(a + 4, d); }
static void w128(unsigned int a, uint128_t d)   { w64(a, d.hi); w64(a + 8, d.lo); }

unsigned int (*ps_read_8)(unsigned int)  = r8;
unsigned int (*ps_read_16)(unsigned int) = r16;
unsigned int (*ps_read_32)(unsigned int) = r32;
uint64_t     (*ps_read_64)(unsigned int) = r64;
uint128_t    (*ps_read_128)(unsigned int)= r128;
void (*ps_write_8)(unsigned int, unsigned int)  = w8;
void (*ps_write_16)(unsigned int, unsigned int) = w16;
void (*ps_write_32)(unsigned int, unsigned int) = w32;
void (*ps_write_64)(unsigned int, uint64_t)     = w64;
void (*ps_write_128)(unsigned int, uint128_t)   = w128;
unsigned int (*ps_read_8_int)(unsigned int)  = r8;
unsigned int (*ps_read_16_int)(unsigned int) = r16;
unsigned int (*ps_read_32_int)(unsigned int) = r32;
void (*ps_write_8_int)(unsigned int, unsigned int)  = w8;
void (*ps_write_16_int)(unsigned int, unsigned int) = w16;
void (*ps_write_32_int)(unsigned int, unsigned int) = w32;

unsigned int ps_read_16_int_nowbwait(unsigned int a) { return r16(a); }
uint64_t ps_read_64_int(unsigned int a) { return r64(a); }
void ps_write_64_int(unsigned int a, uint64_t d) { w64(a, d); }
uint8_t pistorm_get_model(void) { return PISTORM_MODEL_32; }
void ps_setup_protocol(void) { kprintf("[PSMAC] bus bridge transport: mini UART (QEMU -serial 1) -> MAME bridge\n"); }
void ps_reset_state_machine(void) {}
void ps_pulse_reset(void) { struct psmac_reply r; xfer(PSMAC_OP_RESET_ACK, 0, 0, 0, 0, &r); }
void ps_send_reset(void) { ps_pulse_reset(); }
void ps_set_control(unsigned int v) { (void)v; }
void ps_clr_control(unsigned int v) { (void)v; }
unsigned int ps_read_status_reg(void) { return (unsigned int)last_ipl << 13; }
void ps_write_status_reg(unsigned int v) { (void)v; }
void ps_memtest(unsigned int s) { (void)s; }
void ps_buptest(unsigned int s, unsigned int m) { (void)s; (void)m; }
unsigned int ps_get_ipl_zero(void) { return last_ipl == 0; }
void ps_housekeeper(void) { psmac_service_core(); }
void ps_efinix_setup(uint8_t model) { (void)model; }
void ps_efinix_reset(void) {}
void ps_efinix_write(uint8_t b) { (void)b; }
void ps_efinix_load(char *buffer, long length) { (void)buffer; (void)length; }

/* symbols the raspi platform code expects from the PiStorm protocol module */
volatile int housekeeper_enabled = 0;
void fastSerial_init(void) {}

/* Console: under the PiStorm build variants kprintf goes to the "fast serial" (CPLD) path; we send it
   to the PL011 instead so the QEMU/serial console keeps working. */
#define PL011_BASE (PERIPH_VA + 0x201000)
static void con_putc(uint8_t c)
{
    /* bounded wait: a wedged UART must never deadlock the emulation (the console lock is shared by all cores) */
    for (int i = 0; i < 100000 && (rd32le(PL011_BASE + 0x18) & (1 << 5)); i++) ;
    wr32le(PL011_BASE, c);
}
void fastSerial_putByte(uint8_t b) { if (b == '\n') con_putc('\r'); con_putc(b); }
void bitbang_putByte(uint8_t b) { fastSerial_putByte(b); }
