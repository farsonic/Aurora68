/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68: the network hardware, run by the kernel on CPU1, and the Zorro II
 * board through which AmigaOS uses it (alice_net.h, alice68net.device).
 *
 * Unit 0 is the Pi 4's WiFi (CYW43455 on the Arasan SDIO host): the SD host,
 * the chip's bring-up and its FullMAC protocol are wifi/sdio.c, wifi/chip.c
 * and wifi/fmac.c, written for Alice68 from Linux's brcmfmac and OpenBSD's
 * bwfm (both ISC); the connection manager below was alice68wifi.device's,
 * moved here. Unit 1 is kept for the kernel's own Ethernet driver.
 *
 * CPU1 also runs USB (frf_xhci.c), whose main loop calls alice_net_poll()
 * every turn. The WiFi runs as a coroutine with its own stack: everywhere
 * it would wait (a delay, a command's answer, the next poll) it switches
 * back, so USB is never held up for more than one SDIO transfer. Without
 * USB (kernel8-nousb.img) CPU1 runs alice_net_cpu1_main() alone.
 *
 * The firmware files come from the SD card's FAT partition (folder WIFI or
 * the root): alice68hdf.device reads them at boot and offers them on the
 * board; the kernel copies them. In the Macintosh build (PSMAC) they come
 * from the initramfs container instead (make-container.py puts them there,
 * psmac_bus.c hands each one to alice_fw_preload), and the messages say so.
 * cmdline.txt: wifi.ssid="name" wifi.psk="..."
 * wifi.country=GB join at boot; wifi=off leaves the chip off; wifi.sdclock=Hz.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "support.h"
#include "spinlock.h"
#include "boards.h"
#include "devicetree.h"
#include "alice_net.h"
#include "wifi/bus.h"
#include "wifi/fmac.h"
#ifdef PSMAC
#include "wifi/fmac_ap.h"            /* Aurora68: the firmware as an access point (docs/08 item 118) */
#endif

#define AN_KERNEL_VER 1

/* where the user puts the WiFi firmware files, for the messages that ask for them */
#ifdef PSMAC
#define FW_WHERE "in the container (make-container.py)"
#define FW_WAITING "waiting for the firmware files (in the container: make-container.py)"
#else
#define FW_WHERE "in WIFI on the SD card"
#define FW_WAITING "waiting for the firmware files (WIFI folder on the SD card)"
#endif

_Static_assert(sizeof(struct an_hdr) == 64, "an_hdr");
_Static_assert(sizeof(struct an_unit) <= 0x800, "an_unit");
_Static_assert(sizeof(struct an_fw) == 72 && sizeof(struct an_scan) == 56, "an_fw/an_scan");
_Static_assert(AN_OFF_FW + AN_FW_MAX * 72 <= AN_OFF_SCAN && AN_OFF_SCAN + AN_SCAN_MAX * 56 <= AN_OFF_LOG, "layout");
_Static_assert(AN_OFF_LOG + AN_LOG_BYTES <= AN_OFF_BT && AN_OFF_BT + sizeof(struct an_bt) <= AN_OFF_RX(0), "Bluetooth layout");
_Static_assert(AN_OFF_TX(1) + AN_TX_SLOTS * AN_SLOT_BYTES <= AN_BOARD_BYTES, "slots");

extern void *tlsf;
extern void *tlsf_malloc(void *t, uintptr_t size);
extern void *tlsf_malloc_aligned(void *t, uintptr_t size, uintptr_t align);
extern void tlsf_free(void *t, void *p);
extern void mmu_map(uintptr_t phys, uintptr_t virt, uintptr_t length, uint32_t attr_low, uint32_t attr_high);
extern uintptr_t mmu_virt2phys(uintptr_t addr);
extern void aga_virtual_cia_irq(uint16_t bit, int active);
extern void frf_post_wifi(const char *text, int col);

static int mcmp(const void *a, const void *b, unsigned n) { const uint8_t *x = a, *y = b; while (n--) if (*x++ != *y++) return 1; return 0; }

/* copy between normal (uncached) memories, 8 bytes at a time: unaligned
   accesses are allowed there, unlike device memory */
typedef uint64_t u64u __attribute__((aligned(1), may_alias));
static void ncopy(volatile void *dst, const volatile void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)(uintptr_t)dst; const uint8_t *s = (const uint8_t *)(uintptr_t)src;
    while (n >= 8) { *(u64u *)d = *(const u64u *)s; d += 8; s += 8; n -= 8; }
    while (n--) *d++ = *s++;
    __asm__ volatile("" ::: "memory");
}

/* ---- time ---------------------------------------------------------------- */
static inline uint64_t ticks(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t tick_hz(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
uint32_t wifi_usecs(void) { return (uint32_t)(ticks() / (tick_hz() / 1000000u)); }
static uint32_t now_ms(void) { return (uint32_t)(ticks() / (tick_hz() / 1000u)); }

/* ---- the board ------------------------------------------------------------- */
#define AC_NIB(n)   ((uint16_t)(((n) & 15u) << 12))
#define AC_BYTE(b)  AC_NIB((b) >> 4), AC_NIB(b)
#define AC_INV(b)   AC_NIB((~(b)) >> 4), AC_NIB(~(b))
static const uint16_t an_rom[32] = {
    AC_BYTE(0xC2),                               /* 128 KB Zorro II, I/O, not memory */
    AC_INV(AN_PRODUCT),
    AC_INV(0x00), AC_INV(0x00),
    AC_INV(AN_MANUFACTURER >> 8), AC_INV(AN_MANUFACTURER & 0xff),
    AC_INV(0x4e), AC_INV(0x45), AC_INV(0x54), AC_INV(0x30),   /* "NET0" */
    AC_INV(0x00), AC_INV(0x00), AC_INV(0x00), AC_INV(0x00),
    AC_INV(0x00), AC_INV(0x00)
};
#define AN_MMU_ACCESS 0x400u
#define AN_MMU_ISHARE 0x300u
#define AN_MMU_EL0    0x040u
#define AN_MMU_NC     (2u << 2)

static volatile uint8_t *anb;                    /* the board, kernel side (uncached) */
static void *an_mem;
static volatile int an_reset_req;
static inline volatile struct an_hdr *H(void) { return (volatile struct an_hdr *)anb; }
static inline volatile struct an_unit *U(int u) { return (volatile struct an_unit *)(anb + AN_OFF_UNIT(u)); }

static void an_irq(void)
{
    if (!anb) return;
    if (!H()->irq) {
        H()->irq = 1;
        __asm__ volatile("dmb ish" ::: "memory");
        aga_virtual_cia_irq(0x0008, 1);         /* INTF_PORTS: the Amiga's level 2 */
    }
}

static int bootarg_word(const char *w);
/* both units' addresses start with Commodore International's OUI, 00:80:10, the last three
   bytes staying the hardware's own - unless net.oui=off is in cmdline.txt */
static int oui_default(uint8_t want[6])
{
#ifdef PSMAC
    (void)want; return 0;                       /* the Mac keeps the hardware's own address */
#endif
    if (bootarg_word("net.oui=off")) return 0;
    want[0] = (uint8_t)(AN_OUI_COMMODORE >> 16); want[1] = (uint8_t)(AN_OUI_COMMODORE >> 8);
    want[2] = (uint8_t)AN_OUI_COMMODORE; want[3] = want[4] = want[5] = 0;
    return 1;
}
/* a 32-bit word of a command's arg, big-endian as the 68k wrote it */
static uint32_t arg32(volatile struct an_unit *u, unsigned o)
{
    return (uint32_t)u->arg[o] << 24 | (uint32_t)u->arg[o + 1] << 16 | (uint32_t)u->arg[o + 2] << 8 | u->arg[o + 3];
}
/* the MAC address a unit uses: the one asked for (AN_CMD_SETMAC), its OUI on the hardware's own
   last three bytes, or the hardware's own; 0: none asked for */
static int mac_wanted(const uint8_t want[6], int oui_only, const uint8_t fact[6], uint8_t out[6])
{
    if (!(want[0] | want[1] | want[2] | want[3] | want[4] | want[5])) { memcpy(out, fact, 6); return 0; }
    memcpy(out, want, oui_only ? 3 : 6);
    if (oui_only) memcpy(out + 3, fact + 3, 3);
    return 1;
}
/* the setup screen's changes for cmdline.txt (start_rpi64.c alice_setup_run, CPU0, before
   the 68k starts): onto the board when Kickstart maps it, then CPU1 waits for the answer */
static char setup_text[AN_SETUP_MAX];
static volatile int setup_state;              /* 0 none, 1 waiting for the board, 2 on the board */
static uint64_t setup_t;
void alice_net_setup_offer(const char *edits)
{
    unsigned n = 0;
    for (; edits[n] && n < AN_SETUP_MAX - 1; n++) setup_text[n] = edits[n];
    setup_text[n] = 0;
    __atomic_store_n(&setup_state, 1, __ATOMIC_RELEASE);
}
extern void aga_reboot(int tryboot);
static void setup_poll(void)
{
    if (__atomic_load_n(&setup_state, __ATOMIC_ACQUIRE) != 2) return;
    volatile struct an_setup *su = (volatile struct an_setup *)(anb + AN_OFF_SETUP);
    unsigned st = su->state;
    if (st == AN_SET_SAVED) {
        setup_state = 0;
        kprintf("[BOOT] setup: cmdline.txt written - restarting the Pi\n");
        uint64_t end = ticks() + tick_hz() / 2;           /* the line reaches the serial log */
        while ((int64_t)(end - ticks()) > 0) ;
        aga_reboot(0);
    } else if (st == AN_SET_FAILED) {
        setup_state = 0;
        char why[96];
        unsigned n = 0;
        for (; n < sizeof why - 1 && su->text[n]; n++) why[n] = su->text[n];
        why[n] = 0;
        kprintf("[BOOT] setup: FAIL: cmdline.txt not written: %s\n", why);
    } else if (ticks() - setup_t > tick_hz() * 30) {
        setup_state = 0;
        kprintf("[BOOT] setup: FAIL: cmdline.txt not written: alice68hdf.device did not answer in 30 s\n");
    }
}
static void an_board_map(struct ExpansionBoard *board)
{
    if (!an_mem) {
        an_mem = tlsf_malloc_aligned(tlsf, AN_BOARD_BYTES, AN_BOARD_BYTES);
        if (!an_mem) { kprintf("[NET] network board: no memory\n"); return; }
        uintptr_t phys = mmu_virt2phys((uintptr_t)an_mem);
        arm_flush_cache((uintptr_t)an_mem, AN_BOARD_BYTES);     /* no dirty line may land on the board later */
        mmu_map(phys, (uintptr_t)an_mem, AN_BOARD_BYTES, AN_MMU_ACCESS | AN_MMU_ISHARE | AN_MMU_NC, 0);
        __asm__ volatile("dsb ish; isb" ::: "memory");
        for (uint32_t i = 0; i < AN_BOARD_BYTES / 4; i++) ((volatile uint32_t *)an_mem)[i] = 0;
        mmu_map(phys, board->map_base, AN_BOARD_BYTES, AN_MMU_ACCESS | AN_MMU_ISHARE | AN_MMU_EL0 | AN_MMU_NC, 0);
        __asm__ volatile("dsb ish; isb" ::: "memory");
        volatile struct an_hdr *h = an_mem;
        h->magic = AN_MAGIC; h->version = AN_VERSION; h->nunits = AN_UNITS; h->kernel_ver = AN_KERNEL_VER;
        h->trace = (unsigned)bootarg_word("net.trace=1");
        /* unit 0's kind is set by CPU1 when it first serves the board (alice_net_poll) */
        if (__atomic_load_n(&setup_state, __ATOMIC_ACQUIRE) == 1) {     /* the setup screen's changes */
            volatile struct an_setup *su = (volatile struct an_setup *)((volatile uint8_t *)an_mem + AN_OFF_SETUP);
            unsigned n = 0;
            for (; setup_text[n] && n < AN_SETUP_MAX - 1; n++) su->text[n] = setup_text[n];
            su->text[n] = 0;
            su->len = n;
            __asm__ volatile("dmb ish" ::: "memory");
            su->state = AN_SET_OFFERED;
            setup_t = ticks();
            __atomic_store_n(&setup_state, 2, __ATOMIC_RELEASE);
        }
        __asm__ volatile("dmb ish" ::: "memory");
        __atomic_store_n(&anb, (volatile uint8_t *)an_mem, __ATOMIC_RELEASE);
        kprintf("[NET] network board at $%08x (alice68net.device finds it there)\n", board->map_base);
        return;
    }
    /* a 68k RESET: the same memory where Kickstart put the board again; the old
       AmigaOS's frames and commands are gone - CPU1 tidies up (an_reset_req) */
    mmu_map(mmu_virt2phys((uintptr_t)an_mem), board->map_base, AN_BOARD_BYTES, AN_MMU_ACCESS | AN_MMU_ISHARE | AN_MMU_EL0 | AN_MMU_NC, 0);
    __asm__ volatile("dsb ish; isb" ::: "memory");
    an_reset_req = 1;
}
#ifndef PSMAC
static struct ExpansionBoard an_board = { an_rom, AN_BOARD_BYTES, 0, 0, 1, an_board_map };
static void * __attribute__((used, section(".boards.z2"))) an_board_ref = &an_board;
#else
/* Macintosh (psmac): the board is a fixed 128 KB region inside the slot-$9 window that
   psmac_mbox.c carves; there is no autoconfig and no remapping on reset. The Mac side finds it
   through the window's service table (psmb.h). */
void alice_net_attach(void *base)
{
    (void)an_board_map; (void)an_rom;
    an_mem = base;
    for (uint32_t i = 0; i < AN_BOARD_BYTES / 4; i++) ((volatile uint32_t *)an_mem)[i] = 0;
    volatile struct an_hdr *h = an_mem;
    h->magic = AN_MAGIC; h->version = AN_VERSION; h->nunits = AN_UNITS; h->kernel_ver = AN_KERNEL_VER;
    h->trace = (unsigned)bootarg_word("net.trace=1");
    __asm__ volatile("dmb ish" ::: "memory");
    __atomic_store_n(&anb, (volatile uint8_t *)an_mem, __ATOMIC_RELEASE);
    kprintf("[NET] network board at Mac slot $9 window + 0x%x\n", ALICE_NET_WINDOW_OFF);
}
/* a 68k reset (the bridge saw the Mac's reset line): the old session's frames and commands are gone */
void alice_net_reset(void) { an_reset_req = 1; }
#endif

/* ---- the log: kernel log (splash, serial) + the board's ring ------------------ */
struct lbuf { char s[200]; unsigned n; };
static void lb_c(void *d, char c) { struct lbuf *b = d; if (b->n < sizeof b->s - 2) b->s[b->n++] = c; }
static void ring_put(const char *s, unsigned n)
{
    if (!anb) return;
    volatile char *r = (volatile char *)(anb + AN_OFF_LOG);
    uint32_t w = H()->log_wr;
    for (unsigned i = 0; i < n; i++) r[(w + i) % AN_LOG_BYTES] = s[i];
    __asm__ volatile("dmb ish" ::: "memory");
    H()->log_wr = w + n;
}
int wifi_debug = 1;
void wifi_log(const char *fmt, ...)
{
    struct lbuf b; b.n = 0;
    for (const char *p = "[WIFI] "; *p; ) b.s[b.n++] = *p++;
    va_list ap; va_start(ap, fmt); vkprintf_pc(lb_c, &b, fmt, ap); va_end(ap);
    if (!b.n || b.s[b.n - 1] != '\n') b.s[b.n++] = '\n';
    b.s[b.n] = 0;
    kprintf("%s", b.s);
    ring_put(b.s + 7, b.n - 7);
}
/* Tracing what the Amiga is given: every ICMP frame received and sent, and the first frames
   dropped, with the reason. Off unless "net.trace=1" is in cmdline.txt (it found where pings
   went in November 2026: they arrived; the stack lost them). */
static int an_trace = -1;
static unsigned an_drops_logged;
static void trace_rx(int unit, const uint8_t *f, unsigned len, const char *verdict)
{
    if (an_trace < 0) an_trace = bootarg_word("net.trace=1");
    if (!an_trace || len < 14) return;
    unsigned et = (unsigned)f[12] << 8 | f[13];
    int icmp = et == 0x0800 && len >= 14 + 20 && f[14 + 9] == 1;
    int drop = verdict[0] == 'D';
    if (!icmp && !(drop && an_drops_logged < 40)) return;
    if (drop) an_drops_logged++;
    if (icmp) {
        const uint8_t *ip = f + 14;
        unsigned ihl = (ip[0] & 15u) * 4u, ty = len >= 14 + ihl + 8 ? ip[ihl] : 255u;
        unsigned seq = len >= 14 + ihl + 8 ? (unsigned)ip[ihl + 6] << 8 | ip[ihl + 7] : 0;
        wifi_log("unit %d rx ICMP type %u seq %u %u.%u.%u.%u -> %u.%u.%u.%u len %u: %s\n", unit, ty, seq,
                 ip[12], ip[13], ip[14], ip[15], ip[16], ip[17], ip[18], ip[19], len, verdict);
    } else
        wifi_log("unit %d rx type %04x len %u: %s\n", unit, et, len, verdict);
}

static void trace_tx(int unit, const uint8_t *f, unsigned len, int ok)
{
    if (an_trace < 0) an_trace = bootarg_word("net.trace=1");
    if (!an_trace || len < 14 + 20 + 8 || f[12] != 0x08 || f[13] != 0x00 || f[14 + 9] != 1) return;
    const uint8_t *ip = f + 14, *ic = ip + (ip[0] & 15u) * 4u;
    wifi_log("unit %d tx ICMP type %u seq %u %u.%u.%u.%u -> %u.%u.%u.%u len %u: %s\n", unit, ic[0],
             (unsigned)ic[6] << 8 | ic[7], ip[12], ip[13], ip[14], ip[15], ip[16], ip[17], ip[18], ip[19], len,
             ok ? "sent" : "NOT SENT");
}

void wifi_where(const char *fmt, ...)
{
    if (!anb) return;
    struct lbuf b; b.n = 0;
    va_list ap; va_start(ap, fmt); vkprintf_pc(lb_c, &b, fmt, ap); va_end(ap);
    volatile char *d = U(0)->where;
    unsigned i = 0;
    for (; i < b.n && i < 95; i++) d[i] = b.s[i];
    d[i] = 0;
}

/* ---- the coroutine ------------------------------------------------------------ */
/* A switch keeps what the AAPCS64 asks a callee to keep: x19-x29, the return
   address, sp and d8-d15 (only x12 and v19-v26 are reserved in this file,
   so the compiler uses x19-x28 across the call). x19 also carries the
   coroutine's entry for the trampoline on its first switch. */
struct an_ctx { uint64_t x[11], x30, sp, d[8]; };   /* x19..x29 */
extern void an_switch(struct an_ctx *from, struct an_ctx *to);
extern void an_trampoline(void);
__asm__(
"   .text\n"
"   .globl an_switch\n"
"an_switch:\n"
"   mov  x9, sp\n"
"   stp  x19, x20, [x0, #0]\n"
"   stp  x21, x22, [x0, #16]\n"
"   stp  x23, x24, [x0, #32]\n"
"   stp  x25, x26, [x0, #48]\n"
"   stp  x27, x28, [x0, #64]\n"
"   stp  x29, x30, [x0, #80]\n"
"   str  x9,       [x0, #96]\n"
"   stp  d8,  d9,  [x0, #104]\n"
"   stp  d10, d11, [x0, #120]\n"
"   stp  d12, d13, [x0, #136]\n"
"   stp  d14, d15, [x0, #152]\n"
"   ldp  x19, x20, [x1, #0]\n"
"   ldp  x21, x22, [x1, #16]\n"
"   ldp  x23, x24, [x1, #32]\n"
"   ldp  x25, x26, [x1, #48]\n"
"   ldp  x27, x28, [x1, #64]\n"
"   ldp  x29, x30, [x1, #80]\n"
"   ldr  x9,       [x1, #96]\n"
"   ldp  d8,  d9,  [x1, #104]\n"
"   ldp  d10, d11, [x1, #120]\n"
"   ldp  d12, d13, [x1, #136]\n"
"   ldp  d14, d15, [x1, #152]\n"
"   mov  sp, x9\n"
"   ret\n"
"   .globl an_trampoline\n"
"an_trampoline:\n"
"   blr  x19\n"
"1: b    1b\n"
);
_Static_assert(sizeof(struct an_ctx) == 168, "an_ctx: the offsets in an_switch");
static struct an_ctx ctx_main, ctx_wifi;
static int wifi_running;                     /* the coroutine exists */
static int in_wifi;
#define WIFI_STACK (64 * 1024)

#ifdef PSMAC
/* The coroutine is core 3's (psmac_alice_core), but in_wifi is a plain global, and the mailbox call
   below, which yields, is also made from core 1 (the USB start's power request, psmac_usb.c) and
   core 0 (the board revision, psmac_card.c's early look). A yield from core 1 while core 3 was inside
   the coroutine switched core 1 onto core 3's own loop, on core 3's stack, and left core 3 in the
   coroutine with in_wifi cleared, where every later yield returned at once: the user's SE with WiFi
   enabled, 21 November 2026, "service loop: 0 turns/5 s" for good (docs/08 item 79). Only the core
   that switched into the coroutine may switch out of it. */
static int wifi_core = -1;
static inline int an_this_core(void) { uint64_t id; __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(id)); return (int)(id & 3); }
static int an_on_coroutine(void) { return in_wifi && an_this_core() == wifi_core; }
/* the WiFi's turns, for the service loop's heartbeat line: the longest one and where it was */
static uint64_t wifi_turn_t0, wifi_turn_max;
static char wifi_turn_where[64];
void alice_wifi_turn_report(uint32_t *us, const char **where)
{
    uint64_t hz = tick_hz();
    *us = (uint32_t)(wifi_turn_max * 1000000u / (hz ? hz : 1));
    *where = wifi_turn_where;
    wifi_turn_max = 0;
}
#else
static int an_on_coroutine(void) { return in_wifi; }
#endif
static void an_yield(void)
{
    if (!an_on_coroutine()) return;
#ifdef PSMAC
    uint64_t d = ticks() - wifi_turn_t0;
    if (d > wifi_turn_max && anb) {
        wifi_turn_max = d;
        volatile char *w = U(0)->where;
        unsigned i = 0;
        for (; i < sizeof wifi_turn_where - 1 && w[i]; i++) wifi_turn_where[i] = w[i];
        wifi_turn_where[i] = 0;
    }
#endif
    in_wifi = 0;
    { extern void bt_pi_uart_pump(void); bt_pi_uart_pump(); }   /* the Bluetooth UART beside it, at every yield of the WiFi (docs/21) */
    an_switch(&ctx_wifi, &ctx_main);
    in_wifi = 1;
#ifdef PSMAC
    wifi_turn_t0 = ticks();
#endif
}
/* the long polls in sdio.c and chip.c: CPU1's other work runs meanwhile */
void wifi_relax(void) { an_yield(); }
void wifi_udelay(uint32_t us)
{
    uint64_t end = ticks() + tick_hz() * us / 1000000u;
    if (us < 50 || !an_on_coroutine()) { while ((int64_t)(end - ticks()) > 0) ; return; }
    while ((int64_t)(end - ticks()) > 0) an_yield();
}

/* ---- cmdline.txt -------------------------------------------------------------- */
const char *alice_bootargs(void);
const char *alice_bootargs(void) { of_node_t *ch = dt_find_node("/chosen"); of_property_t *ba = ch ? dt_find_property(ch, "bootargs") : 0; return ba && ba->op_value ? ba->op_value : ""; }
static const char *bootargs(void)
{
    of_node_t *ch = dt_find_node("/chosen");
    of_property_t *ba = ch ? dt_find_property(ch, "bootargs") : 0;
    return ba && ba->op_value ? ba->op_value : "";
}
/* key=value or key="value with spaces"; 1 if found */
static int bootarg_val(const char *key, char *out, unsigned max);
int psmac_bootarg_val(const char *key, char *out, unsigned max) { return bootarg_val(key, out, max); }   /* for alice_psmac.c */
static int bootarg_val(const char *key, char *out, unsigned max)
{
    out[0] = 0;
    unsigned kl = strlen(key);
    for (const char *s = bootargs(); *s; s++) {
        if ((s != bootargs() && s[-1] != ' ') || mcmp(s, key, kl) || s[kl] != '=') continue;
        const char *v = s + kl + 1;
        unsigned n = 0;
        if (*v == '"') { v++; while (*v && *v != '"' && n < max - 1) out[n++] = *v++; }
        else while (*v && *v != ' ' && *v != '\n' && n < max - 1) out[n++] = *v++;
        out[n] = 0;
        return 1;
    }
    return 0;
}
static int bootarg_word(const char *w)
{
    unsigned n = strlen(w);
    for (const char *s = bootargs(); *s; s++)
        if ((s == bootargs() || s[-1] == ' ') && !mcmp(s, w, n) && (!s[n] || s[n] == ' ' || s[n] == '\n')) return 1;
    return 0;
}

/* ---- firmware, copied from what AmigaOS offered ---------------------------------- */
struct fwfile { char name[64]; uint8_t *p; uint32_t len; };
#ifdef PSMAC
/* The container brings every board's WiFi and Bluetooth firmware (get-firmware.sh: 15 files for the
   3B, the Zero 2 W's two chips, and the 3B+, 3A+ and Pi 4). With the board protocol's eight slots
   the 43455 set, last in that list, never got in: a 3A+ or 3B+ booted from the one-card container
   had no WiFi firmware (found 20 November 2026). The kernel's own table is larger; the 68k's offer
   (fw_take) still reads at most AN_FW_MAX entries. */
#define FW_TABLE 24
#else
#define FW_TABLE AN_FW_MAX
#endif
static struct fwfile fw[FW_TABLE];
static int nfw;
static int wifi_disabled;
static volatile uint32_t fw_gen;              /* +1 each time new files came: the WiFi tries again */

static int ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}
/* the 68k's buffer as this core sees it (TTBR0 is the 68k's address space) */
static int m68k_readable(uint32_t a, uint32_t len)
{
    for (uint32_t p = a & ~0xfffu; p < a + len; p += 0x1000u) {
        uint64_t par;
        __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(par) : "r"((uintptr_t)p) : "memory");
        if (par & 1) return 0;
    }
    return 1;
}
static void fw_take(void)
{
    volatile struct an_hdr *h = H();
    if (h->fw_state != AN_FW_OFFERED) return;
    __asm__ volatile("dmb ish" ::: "memory");
    extern int bt_pi_wanted(void);
    if (wifi_disabled && !bt_pi_wanted()) { h->fw_state = AN_FW_REFUSED; return; }
    unsigned n = h->fw_count > AN_FW_MAX ? AN_FW_MAX : h->fw_count;
    volatile struct an_fw *t = (volatile struct an_fw *)(anb + AN_OFF_FW);
    for (unsigned i = 0; i < n; i++) {
        uint32_t a = t[i].addr, len = t[i].len;
        if (!len || len > 4u * 1024 * 1024 || !m68k_readable(a, len)) { kprintf("[WIFI] firmware entry %u: bad buffer\n", i); continue; }
        int k = 0;
        for (; k < nfw; k++) {
            int same = 1;
            for (int c = 0; c < 64; c++) { if (fw[k].name[c] != t[i].name[c]) { same = 0; break; } if (!fw[k].name[c]) break; }
            if (same) break;
        }
        if (k < nfw) continue;                      /* have it: never replaced under the WiFi's feet */
        if (nfw == FW_TABLE) continue;
        nfw++;
        fw[k].p = tlsf_malloc(tlsf, len + 64);
        if (!fw[k].p) { kprintf("[WIFI] no memory for the firmware\n"); fw[k].len = 0; continue; }
        for (int c = 0; c < 63; c++) fw[k].name[c] = t[i].name[c];
        fw[k].name[63] = 0;
        const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)a;
        for (uint32_t c = 0; c < len; c++) fw[k].p[c] = src[c];
        fw[k].len = len;
        kprintf("[WIFI] firmware file %s: %u bytes\n", fw[k].name, len);
    }
    __asm__ volatile("dmb ish" ::: "memory");
    h->fw_state = AN_FW_TAKEN;
    fw_gen++;
}
#ifdef PSMAC
/* psmac: firmware files come from the kernel's own sources (the initramfs container on hardware,
   host files through semihosting in the testbed) instead of from the 68k. Same table as fw_take. */
int alice_fw_preload(const char *name, const uint8_t *data, uint32_t len)
{
    if (!len) return -1;
    if (nfw >= FW_TABLE) { kprintf("[WIFI] firmware file %s left out: the kernel's table holds %d files\n", name, FW_TABLE); return -1; }
    struct fwfile *f = &fw[nfw];
    f->p = tlsf_malloc(tlsf, len + 64);
    if (!f->p) return -1;
    int c = 0; for (; c < 63 && name[c]; c++) f->name[c] = name[c]; f->name[c] = 0;
    memcpy(f->p, data, len); f->len = len;
    nfw++; fw_gen++;
    if (anb) H()->fw_state = AN_FW_TAKEN;
    kprintf("[WIFI] firmware file %s: %u bytes (preloaded)\n", f->name, len);
    return 0;
}
/* 1 if a file of that name is in the table already (psmac_card.c: the card's wifi-firmware folder adds only
   what the container did not bring) */
int alice_fw_have(const char *name)
{
    for (int f = 0; f < nfw; f++)
        if (fw[f].p && ieq(fw[f].name, name))
            return 1;
    return 0;
}
#endif
static const struct fwfile *fw_find(const char *const *stems, const char *suffix)
{
    char want[80];
    for (int i = 0; stems[i]; i++) {
        unsigned k = 0;
        for (const char *p = stems[i]; *p && k < 60; ) want[k++] = *p++;
        for (const char *p = suffix; *p && k < 79; ) want[k++] = *p++;
        want[k] = 0;
        for (int f = 0; f < nfw; f++) if (fw[f].p && ieq(fw[f].name, want)) return &fw[f];
    }
    return 0;
}

#ifdef PSMAC
/* name starts with prefix (lower case), letters compared without case */
static int istarts(const char *name, const char *prefix)
{
    for (; *prefix; name++, prefix++)
        if ((*name | 0x20) != *prefix) return 0;
    return 1;
}
/* 1 if any WiFi chip's firmware is among the files: a name starting brcmfmac43 or cyfmac43 */
static int fw_have_wifi(void)
{
    for (int f = 0; f < nfw; f++)
        if (fw[f].p && (istarts(fw[f].name, "brcmfmac43") || istarts(fw[f].name, "cyfmac43"))) return 1;
    return 0;
}
#endif
/* for bt_pi.c: a firmware file AmigaOS handed over, by stems and suffix ("BCM4345C0", ".hcd") */
const uint8_t *alice_fw_get(const char *const *stems, const char *suffix, uint32_t *len)
{
    const struct fwfile *f = fw_find(stems, suffix);
    if (!f) return 0;
    *len = f->len;
    return f->p;
}
/* 1 once alice68hdf.device has offered its files (or said there are none) */
int alice_fw_offered(void) { return anb && (H()->fw_state == AN_FW_TAKEN || H()->fw_state == AN_FW_REFUSED || nfw > 0); }
volatile uint8_t *alice_net_board(void) { return anb; }
void alice_net_ring(const char *s, unsigned n) { ring_put(s, n); }

/* ---- VideoCore mailbox (own buffer: the kernel's shared one is CPU0's) ------------- */
#ifdef PSMAC
#include "alice_psmac.h"            /* the Macintosh build: the peripheral window, no autoconfig */
#endif
#ifndef ALICE_PERI
#define ALICE_PERI  0xf2000000u
#endif
#define MBOX_READ   (ALICE_PERI + 0x00b880u)
#define MBOX_STATUS (ALICE_PERI + 0x00b898u)
#define MBOX_WRITE  (ALICE_PERI + 0x00b8a0u)
static inline uint32_t mrd(uintptr_t a) { return __builtin_bswap32(*(volatile uint32_t *)(uintptr_t)a); }
static inline void mwr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = __builtin_bswap32(v); }
static uint32_t *mbuf;
static uint32_t mbox_code;                   /* the answer's code when a call failed (-4) */
static uint32_t mbox_w0;                     /* the answer's first value word (GET_THROTTLED's bits) */
static uint32_t mbox_rlen;                   /* the tag's response word: bit 31 once the firmware has answered the tag itself */
static volatile int mbox_busy;               /* the WiFi coroutine is inside mbox_tag (it yields there); PSMAC: anyone holds the mailbox */
static int mbox_tag_(uint32_t tag, uint32_t a, uint32_t b, uint32_t *out);
static int mbox_tag(uint32_t tag, uint32_t a, uint32_t b, uint32_t *out)
{
#ifdef PSMAC
    return mbox_tag_(tag, a, b, out);              /* mbox_busy: while the lock is held, below */
#else
    mbox_busy = 1;
    int r = mbox_tag_(tag, a, b, out);
    mbox_busy = 0;
    return r;
#endif
}
static int mbox_tag_(uint32_t tag, uint32_t a, uint32_t b, uint32_t *out)
{
    /* The VideoCore reads the buffer by its physical address and can only reach the first
       1 GB. The kernel's heap is at the top of RAM (start.c moves it there), so a buffer
       from tlsf is out of its reach on a 2, 4 or 8 GB Pi: it answers, but cannot have read
       the request (every call failed with -4 until November 2026). This one is in the low
       page Emu68's own mailbox buffer (FBReq, support_rpi.c) uses, 2 KB past it. */
    if (!mbuf) mbuf = (uint32_t *)0xffffff9000001800ull;
    extern spinlock_t vc_mbox_lock;                  /* support_rpi.c: one caller at a time, see mbox_recv there */
#ifdef PSMAC
    /* Never a yield with the lock held (21 November 2026, docs/08 item 79). The lock is the whole
       Pi's: support_rpi.c's callers on core 0 and core 3 (the card's clock, the HDMI screen's frame
       buffer) wait for it without yielding, so a coroutine that yielded while holding it left them
       spinning, and one of them on core 3 itself could never get it back. The coroutine waits for the
       lock in slices; once it has it, it waits for the answer without yielding, for at most 100 ms
       (the firmware answers in well under a millisecond), and gives up after that. */
    int coro = an_on_coroutine();
    uint64_t t = ticks(), hz = tick_hz();
    if (coro) {
        while (!spinlock_try_acquire(&vc_mbox_lock)) {
            if (ticks() - t > hz) return -2;
            an_yield();
        }
    } else
        spinlock_acquire(&vc_mbox_lock);
    mbox_busy = 1;                                   /* -9 for alice_mbox's callers meanwhile (they ask again) */
    mbuf[0] = __builtin_bswap32(8 * 4); mbuf[1] = 0; mbuf[2] = __builtin_bswap32(tag);
    mbuf[3] = __builtin_bswap32(8); mbuf[4] = 0; mbuf[5] = __builtin_bswap32(a); mbuf[6] = __builtin_bswap32(b); mbuf[7] = 0;
    arm_flush_cache((uintptr_t)mbuf, 64);
    uint32_t addr = (uint32_t)mmu_virt2phys((uintptr_t)mbuf);
    addr = (addr & 0x3fffffffu) | 0xC0000000u;       /* the VideoCore's uncached alias, as support_rpi.c's mbox_send */
    /* mac.vcslow=US (testbed): the answer is taken no sooner than US microseconds after the request,
       as a real VideoCore's comes; QEMU's comes at once, so no caller ever waited in this loop there */
    static int vcslow = -1;
    if (vcslow < 0) {
        char v[16]; int n = 0;
        if (bootarg_val("mac.vcslow", v, sizeof v))
            for (char *q = v; *q >= '0' && *q <= '9'; q++)
                n = n * 10 + (*q - '0');
        vcslow = n;
    }
    uint64_t limit = coro ? hz / 10 : 2 * hz, slow = hz * (uint64_t)vcslow / 1000000u;
    t = ticks();
    while (mrd(MBOX_STATUS) & (1u << 31)) {
        if (ticks() - t > limit) { mbox_busy = 0; spinlock_release(&vc_mbox_lock); return -2; }
    }
    mwr(MBOX_WRITE, (addr & ~15u) | 8u);
    t = ticks();
    for (;;) {
        if (ticks() - t >= slow && !(mrd(MBOX_STATUS) & (1u << 30))) {
            uint32_t r = mrd(MBOX_READ);
            if ((r & 15u) == 8u && (r & ~15u) == (addr & ~15u)) break;
        }
        if (ticks() - t > limit) { mbox_busy = 0; spinlock_release(&vc_mbox_lock); return -3; }
    }
    /* the answer is read before the lock goes: the buffer is every caller's of this file, on any core */
    arm_dcache_invalidate((uintptr_t)mbuf, 64);
    uint32_t v6 = __builtin_bswap32(mbuf[6]), v5 = __builtin_bswap32(mbuf[5]), v4 = __builtin_bswap32(mbuf[4]), code = __builtin_bswap32(mbuf[1]);
    mbox_busy = 0;
    spinlock_release(&vc_mbox_lock);
    if (out) *out = v6;
    mbox_w0 = v5;
    mbox_rlen = v4;
    if (code != 0x80000000u) { mbox_code = code; return -4; }
    return 0;
#else
    spinlock_acquire(&vc_mbox_lock);
    mbuf[0] = __builtin_bswap32(8 * 4); mbuf[1] = 0; mbuf[2] = __builtin_bswap32(tag);
    mbuf[3] = __builtin_bswap32(8); mbuf[4] = 0; mbuf[5] = __builtin_bswap32(a); mbuf[6] = __builtin_bswap32(b); mbuf[7] = 0;
    arm_flush_cache((uintptr_t)mbuf, 64);
    uint32_t addr = (uint32_t)mmu_virt2phys((uintptr_t)mbuf);
    uint64_t t = ticks(), hz = tick_hz();
    while (mrd(MBOX_STATUS) & (1u << 31)) { if (ticks() - t > hz) { spinlock_release(&vc_mbox_lock); return -2; } an_yield(); }
    mwr(MBOX_WRITE, (addr & ~15u) | 8u);
    t = ticks();
    for (;;) {
        if (!(mrd(MBOX_STATUS) & (1u << 30))) {
            uint32_t r = mrd(MBOX_READ);
            if ((r & 15u) == 8u && (r & ~15u) == (addr & ~15u)) break;
        }
        if (ticks() - t > 2 * hz) { spinlock_release(&vc_mbox_lock); return -3; }
        an_yield();
    }
    spinlock_release(&vc_mbox_lock);
    arm_dcache_invalidate((uintptr_t)mbuf, 64);
    if (out) *out = __builtin_bswap32(mbuf[6]);
    mbox_w0 = __builtin_bswap32(mbuf[5]);
    mbox_rlen = __builtin_bswap32(mbuf[4]);
    uint32_t code = __builtin_bswap32(mbuf[1]);
    if (code != 0x80000000u) { mbox_code = code; return -4; }
    return 0;
#endif
}
/* The mailbox is shared with CPU0, CPU2 and the 68k's mailbox.resource, each of which may take
   another's answer: try again rather than give up. */
static int mbox_try(uint32_t tag, uint32_t a, uint32_t b, uint32_t *out)
{
    int e = -1;
    for (int i = 0; i < 4 && e; i++) {
        e = mbox_tag(tag, a, b, out);
        if (e) wifi_log("mailbox tag %x: error %d (code %x)%s\n", tag, e, e == -4 ? mbox_code : 0, i < 3 ? ", again" : "");
#ifdef PSMAC
        if (e == -3 && an_on_coroutine()) break;    /* no answer in 100 ms: asking again only holds core 3 longer */
#endif
    }
    return e;
}
/* for bt_pi.c, from CPU1's loop (not the WiFi coroutine): -9 while the WiFi has the mailbox */
int alice_mbox(uint32_t tag, uint32_t a, uint32_t b, uint32_t *out)
{
    if (mbox_busy) return -9;
    return mbox_try(tag, a, b, out);
}
/* for alice_status.c: both value words of the answer */
int alice_mbox2(uint32_t tag, uint32_t a, uint32_t b, uint32_t out[2])
{
    if (mbox_busy) return -9;
    int e = mbox_try(tag, a, b, &out[1]);
    out[0] = mbox_w0;
    return e;
}
#ifdef PSMAC
/* for psmac_power.c, every two seconds from the service loop: one try, no retries (a mailbox
   that does not answer must not hold the loop for 8 s), and -5 when the firmware answered the
   buffer but not the tag (QEMU's model knows only some tags, and says so this way) */
int alice_mbox_once(uint32_t tag, uint32_t a, uint32_t b, uint32_t out[2])
{
    if (mbox_busy) return -9;
    int e = mbox_tag(tag, a, b, &out[1]);
    out[0] = mbox_w0;
    if (!e && (!(mbox_rlen & 0x80000000u) || (mbox_rlen & 0x7fffffffu) < 4u)) e = -5;
    return e;
}
#endif
/* ---- the radio's power ----
   Where WL_REG_ON is wired differs by board. The 3B, 3B+, 3A+, 4, 400 and CM4 have it on line
   1 of the VideoCore's GPIO expander (mailbox SET_GPIO_STATE, GPIO 128+1; the 3B's device tree
   names the expander's lines BT_ON, WL_ON, ... and its GPIO 41 is PWM1_OUT, the audio). The
   Zero W and Zero 2 W have WL_ON on the SoC's own GPIO 41 (BT_ON on 45 and 42). All of them
   feed the chip's 32.768 kHz sleep clock from GPCLK2 on GPIO 43 (WIFI_CLK), which start.elf
   sets up; it is checked and logged here, and set up only on the Zeros when it is not running.
   The board type is bits 4-11 of the revision code. */
#define GPIO_BASE   (ALICE_PERI + 0x200000u)
#define CM_BASE     (ALICE_PERI + 0x101000u)
static uint32_t board_rev;
static int board_type(void)
{
    if (!board_rev && !mbox_try(0x00010002u, 0, 0, 0)) board_rev = mbox_w0;      /* GET_BOARD_REVISION */
    if (!(board_rev & 0x800000u)) return -1;                                      /* old-style code: a Pi 1 */
    return (int)((board_rev >> 4) & 0xffu);
}
static int wl_on_soc_gpio(void)
{
    switch (board_type()) {
    case 0x09: case 0x0c: case 0x12:                            /* Zero, Zero W, Zero 2 W */
        return 1;
    default:
        return 0;
    }
}
int alice_pi_board_type(void) { return board_type(); }
#ifdef PSMAC
uint32_t alice_pi_board_rev(void) { board_type(); return board_rev; }   /* psmac_usbnet.c's board line */
#endif
int alice_pi_is_2711(void)
{
    switch (board_type()) {
    case 0x11: case 0x13: case 0x14: return 1;                  /* 4B, 400, CM4 */
    default: return 0;
    }
}
static void lpo_clock(int fix)
{
    /* GPIO 43 to ALT0 (GPCLK2), the clock from the 19.2 MHz oscillator divided by 585.9375 */
    uint32_t f = mrd(GPIO_BASE + 0x10);                         /* GPFSEL4: GPIO 40-49 */
    uint32_t ctl = mrd(CM_BASE + 0x80), div = mrd(CM_BASE + 0x84);
    int alt0 = ((f >> 9) & 7u) == 4u, on = (ctl & 0x10u) != 0;                   /* start.elf's own setting is kept, whatever its divider */
    wifi_log("sleep clock: GPIO 43 function %u, CM_GP2CTL %08x CM_GP2DIV %08x%s\n", (f >> 9) & 7u, ctl, div, alt0 && on ? " (already running)" : "");
    if ((alt0 && on) || !fix) return;
    extern spinlock_t psmac_gpfsel_lock;
    mwr(CM_BASE + 0x80, 0x5a000000u | (ctl & 0x0fu));           /* ENAB off, source kept */
    for (int i = 0; i < 100 && (mrd(CM_BASE + 0x80) & 0x80u); i++) wifi_udelay(10);  /* BUSY */
    mwr(CM_BASE + 0x84, 0x5a000000u | (585u << 12) | 3840u);    /* DIVI 585, DIVF 3840/4096 */
    wifi_udelay(10);
    mwr(CM_BASE + 0x80, 0x5a000000u | (1u << 9) | 1u);          /* MASH 1, oscillator */
    wifi_udelay(10);
    mwr(CM_BASE + 0x80, 0x5a000000u | (1u << 9) | 0x10u | 1u);  /* and enable */
    spinlock_acquire(&psmac_gpfsel_lock);
    f = mrd(GPIO_BASE + 0x10);
    mwr(GPIO_BASE + 0x10, (f & ~(7u << 9)) | (4u << 9));
    spinlock_release(&psmac_gpfsel_lock);
    wifi_udelay(1000);
}
static void wl_on(int on)
{
    static int said;
    int soc = wl_on_soc_gpio();
    if (!said) { said = 1; wifi_log("board type %02x (revision %08x): WL_REG_ON on %s\n", board_type(), board_rev, soc ? "GPIO 41" : "the GPIO expander, line 1"); lpo_clock(soc); }
    if (!soc) { mbox_try(0x00038041u, 128u + 1u, on ? 1u : 0u, 0); return; }   /* SET_GPIO_STATE, expander line 1 */
    extern spinlock_t psmac_gpfsel_lock;
    spinlock_acquire(&psmac_gpfsel_lock);
    uint32_t f = mrd(GPIO_BASE + 0x10);
    mwr(GPIO_BASE + 0x10, (f & ~(7u << 3)) | (1u << 3));        /* GPIO 41: output */
    spinlock_release(&psmac_gpfsel_lock);
    mwr(GPIO_BASE + (on ? 0x20 : 0x2c), 1u << 9);               /* GPSET1 / GPCLR1 */
}

/* ==== the WiFi =================================================================== */
#define BUFSZ   2048
#define CTLMAX  1600

static struct {
    struct sdhost h;
    struct chip   c;
    uint8_t *rxbuf, *txbuf, *ctlbuf, *iobuf;
    uint8_t  txseq, maxseq, fcstate, flowctl, rxskip, rx_signalled, chip_up, keyed;
    uint16_t reqid, ctl_wait;
    volatile uint8_t ctl_got;
    int32_t  ctl_status;
    uint8_t *ctl_out;
    uint32_t ctl_outmax, rxskip_t0, junk, ack_fail, traffic_ms, next_poll;
    /* connecting */
    char     ssid[33], pass[65], country[4];
    int      want, scan_busy, scan_for_join, scan_done, scan_cmd;
    uint32_t scan_cmd_seq;
    uint16_t scan_sync;
    uint32_t scan_t0, join_t0, retry_at, info_at, backoff;
    int      ev_setssid_fail, ev_link_up, ev_link_down, ev_keyed, has_sae, ev_assoc_ok;
    struct fm_bss scan[AN_SCAN_MAX];
    int      nscan;
    struct fm_bss cur;
    int      rssi, rate;
    /* choosing and keeping an access point among several of one SSID (docs/08 item 147, 25 November 2026) */
    struct fm_pick pick;                    /* the kicked list, what was seen, failed pinned joins (wifi/fmac.c) */
    int      pinned;                        /* the join going on names a BSSID */
    int      scan_pass;                     /* 0 the first pass of a scan, 1 the one more (longer dwell, merged) */
    int32_t  scan_status;                   /* the escan's final status (E_STATUS_*), -1 no end yet or in 10 s, < -1 refused */
    int      quick;                         /* tries again at once (no backoff) left after a lost connection */
    int      aps_logged;                    /* the first scan for a join logged every access point of the SSID */
    int      roam_mode, roam_off_now, roam_applied;   /* wifi.roam= 0 auto, 1 on, 2 off; auto turned it off; what the firmware has */
    int      fw_drops;                      /* links lost just after a join or roam attempt of the firmware's own */
    uint32_t fw_attempt_at, fw_drop_t0, connected_at;
    int      ev_roamed, need_disassoc, drop_by_ap, drop_ev, drop_reason;
    uint32_t sdclock;
    char     caps[256], fwver[96];
    uint8_t  mac[6];
    uint8_t  fmac[6], want_mac[6];          /* the chip's own; the one asked for (zeros: the chip's own) */
    int      want_oui;                      /* want_mac: only its OUI */
    int      start_req, off_req;
    uint32_t host_c0, host_c1, host_hz;     /* the SD host's bus and clock settings after bring-up */
    uint32_t restart_at;                    /* the chip is lost: start it again then (0 = no) */
    int      restarts;
    const char *lost;                       /* why it was lost, for the log */
#ifdef PSMAC
    /* the access point (AN_CMD_AP, Aurora68's Setup and Recovery, docs/08 item 118) */
    int      ap_want, ap_up, ap_wpa2, ap_stations, ap_events;
    char     ap_ssid[33], ap_pass[64];
    unsigned ap_channel, ap_flags;
    uint32_t iover;                         /* WLC_GET_VERSION: 1 is the older 802.11n chanspec encoding */
    int      ev_ap_link, ev_ap_ssid;        /* E_LINK up (1) or down (-1); E_SET_SSID's status + 1 */
    int      sim;                           /* mac.wifisimfw=1: a simulated firmware below this driver (alice_psmac.c, testbed) */
#endif
} W;

static const char *const state_word[] = { "off", "starting", "ready", "scanning", "joining", "connected", "retrying", "FAILED", "no firmware" };

/* state and message: the board, the splash, the log */
/* one line for the kernel's heartbeat (psmac_bus.c): where the WiFi stands, for a screen without scrollback */
void alice_net_report(void)
{
    static const char *const names[] = { "off", "starting", "idle", "scanning", "joining", "connected", "retry", "failed", "no firmware" };
    if (!anb) { kprintf("[WIFI]   no board yet\n"); return; }
    volatile struct an_unit *u = U(0);
    unsigned st = u->state;
#ifdef PSMAC
    /* The Mac shows the log in PiLog, where a heartbeat's unchanged line every five seconds
       buried everything else ("no firmware" dozens of times): print it only when it changes. */
    static unsigned last_st = ~0u, last_chip, last_mhz;
    static char last_msg[96];
    int same = st == last_st && u->chip == last_chip && u->sdmhz == last_mhz;
    for (unsigned i = 0; same && i < sizeof last_msg; i++) {
        if (last_msg[i] != u->msg[i]) same = 0;
        if (!u->msg[i]) break;
    }
    /* unchanged: again once a minute while the WiFi is not connected, so a saved log (a tail of
       64 KB) still says why (the first real 3A+, 27 November 2026, left no [WIFI] line at all) */
    static unsigned quiet;
    if (same && (st == 5 || ++quiet < 12)) return;
    quiet = 0;
    last_st = st; last_chip = u->chip; last_mhz = u->sdmhz;
    for (unsigned i = 0; i < sizeof last_msg; i++)
        last_msg[i] = u->msg[i];
#endif
#ifdef PSMAC
    kprintf("[WIFI]   %s: %s (chip %x rev %u, SD %u MHz; Pi board %06x)\n", st < 9 ? names[st] : "?", (const char *)u->msg, u->chip, u->chiprev, u->sdmhz, alice_pi_board_rev());
#else
    kprintf("[WIFI]   %s: %s (chip %x rev %u, SD %u MHz)\n", st < 9 ? names[st] : "?", (const char *)u->msg, u->chip, u->chiprev, u->sdmhz);
#endif
}

/* bt/bt_pi.c: the WiFi is in use (scanning, joining, connected, trying again): Bluetooth holds its LE interval floor
   meanwhile (docs/08 item 147) */
extern void bt_pi_wifi_use(int on) __attribute__((weak));
static void wstate(unsigned st, const char *msg)
{
    volatile struct an_unit *u = U(0);
    if (bt_pi_wifi_use) bt_pi_wifi_use(st >= AN_ST_SCANNING && st <= AN_ST_RETRY);
    if (msg) {
        unsigned i = 0;
        for (; msg[i] && i < 95; i++) u->msg[i] = msg[i];
        u->msg[i] = 0;
        if (msg[0]) wifi_log("%s\n", msg);
    }
    u->state = st;
    u->state_seq++;
    an_irq();
    char s[48]; unsigned k = 0;
    for (const char *p = state_word[st < 9 ? st : 0]; *p && k < 20; ) s[k++] = *p++;
    if ((st == AN_ST_CONNECTED || st == AN_ST_JOINING) && W.ssid[0]) { s[k++] = ' '; for (const char *p = W.ssid; *p && k < 46; ) s[k++] = *p++; }
    s[k] = 0;
    frf_post_wifi(s, st == AN_ST_CONNECTED || st == AN_ST_IDLE ? 1 : st == AN_ST_FAILED ? 2 : 0);
}
/* bt/bt_pi.c: the WiFi needs the shared antenna (the Pi 3B's BCM43438) for a few seconds: Bluetooth's background scans
   stop meanwhile (docs/08 item 140) */
extern void bt_pi_wifi_busy(const char *why) __attribute__((weak));
static void link_set(int up)
{
    volatile struct an_unit *u = U(0);
    if ((int)u->link == up) return;
    if (up && bt_pi_wifi_busy) bt_pi_wifi_busy("the WiFi joined: its address next");
    u->link = (unsigned)up;
    u->speed_mbps = up ? (W.rate ? (unsigned)W.rate : 54u) : 0u;
    u->state_seq++;
    an_irq();
}

/* ---- control messages ---- */
static void scan_reset(void);
static void poll_frames(void);
static void tx_fail(void);
static void ctl_rx(const uint8_t *m, unsigned len)
{
    struct bcdc_reply r;
    if (bcdc_parse_reply(m, len, &r) || r.reqid != W.ctl_wait || W.ctl_got) return;
    unsigned n = r.len < W.ctl_outmax ? r.len : W.ctl_outmax;
    if (W.ctl_out) { memcpy(W.ctl_out, r.data, n); memset(W.ctl_out + n, 0, W.ctl_outmax - n); }
    W.ctl_status = r.error ? (r.status ? (int32_t)r.status : -1) : 0;
    W.ctl_got = 1;
}
static int wait_credit(void)
{
    uint32_t t0 = now_ms();
    while (!sdpcm_credit(W.txseq, W.maxseq)) {
        poll_frames();
        if (now_ms() - t0 > 1000) return -1;
        an_yield();
    }
    return 0;
}
#ifdef PSMAC
/* mac.wifisimfw=1 (the testbed, docs/08 item 147): QEMU has no WiFi chip, so a simulated FullMAC firmware in
   alice_psmac.c answers the commands and sends the events, and everything above the chip - the scans, the choice of
   access point, the joins, roaming, the losses - runs as it does on a Pi */
int psmac_wfw_on(void);
int32_t psmac_wfw_ctl(uint32_t cmd, int set, uint8_t *buf, unsigned len, unsigned outmax);
int psmac_wfw_event(struct fm_event *e);
#endif
static int32_t ctl(uint32_t cmd, int set, void *buf, unsigned len, unsigned outmax)
{
#ifdef PSMAC
    if (W.sim) { if (!W.chip_up) return -100; wifi_where("firmware command %u (simulated)", cmd); return psmac_wfw_ctl(cmd, set, buf, len, outmax); }
#endif
    if (!W.chip_up) return -100;
    unsigned plen = set || len > outmax ? len : outmax;
    if (plen > CTLMAX) return -101;
    uint8_t *f = W.ctlbuf;
    memcpy(f + SDPCM_HDRLEN + BCDC_DCMD_HDR, buf, len);
    memset(f + SDPCM_HDRLEN + BCDC_DCMD_HDR + len, 0, plen - len);
    uint16_t id = ++W.reqid;
    bcdc_dcmd(f + SDPCM_HDRLEN, cmd, set, id, 0, plen);
    if (wait_credit()) { wifi_log("no credit for command %u\n", cmd); return -102; }
    unsigned n = sdpcm_frame(f, SDPCM_CHAN_CONTROL, W.txseq, BCDC_DCMD_HDR + plen);
    W.ctl_wait = id; W.ctl_got = 0; W.ctl_out = buf; W.ctl_outmax = outmax; W.ctl_status = 0;
    if (cmd == WLC_GET_VAR || cmd == WLC_SET_VAR) wifi_where("firmware command %u %s", cmd, (const char *)buf);
    else wifi_where("firmware command %u", cmd);
    if (chip_frame(&W.c, f, n, 1)) { tx_fail(); wifi_log("command %u not sent\n", cmd); return -103; }
    W.txseq++;
    uint32_t t0 = now_ms();
    while (!W.ctl_got) {
        poll_frames();
        if (W.ctl_got) break;
        if (now_ms() - t0 > 2000) { wifi_log("command %u: no answer\n", cmd); W.ctl_wait = 0; return -104; }
        an_yield();
    }
    W.ctl_wait = 0;
    return W.ctl_status;
}
static int32_t cmd_int(uint32_t cmd, uint32_t v) { uint8_t b[4]; fm_put32(b, v); return ctl(cmd, 1, b, 4, 0); }
static int32_t cmd_get_int(uint32_t cmd, uint32_t *v)
{
    uint8_t b[4] = { 0, 0, 0, 0 };
    int32_t e = ctl(cmd, 0, b, 4, 4);
    if (!e) *v = fm_le32(b);
    return e;
}
static int32_t iov_set(const char *name, const void *val, unsigned len)
{
    unsigned n = bcdc_iovar(W.iobuf, CTLMAX, name, val, len);
    if (!n) return -105;
    return ctl(WLC_SET_VAR, 1, W.iobuf, n, 0);
}
static int32_t iov_int(const char *name, uint32_t v) { uint8_t b[4]; fm_put32(b, v); return iov_set(name, b, 4); }
static int32_t iov_get(const char *name, void *out, unsigned outlen)
{
    unsigned n = bcdc_iovar(W.iobuf, CTLMAX, name, 0, 0);
    if (!n || outlen > CTLMAX) return -105;
    int32_t e = ctl(WLC_GET_VAR, 0, W.iobuf, n, outlen);
    if (!e) memcpy(out, W.iobuf, outlen);
    return e;
}

/* ---- events ---- */
static void scan_add(void *ctx, const struct fm_bss *b)
{
    (void)ctx;
    if (!b->ssid_len) return;
    for (int i = 0; i < W.nscan; i++)
        if (!mcmp(W.scan[i].bssid, b->bssid, 6)) { if (b->rssi > W.scan[i].rssi) W.scan[i] = *b; return; }
    if (W.nscan < AN_SCAN_MAX) { W.scan[W.nscan++] = *b; return; }
    /* full (an office has more than AN_SCAN_MAX BSSIDs in range): the weakest other network's place, for one of the SSID
       wanted or a stronger one; until 25 November 2026 the first 36 heard were kept, whatever they were */
    unsigned sl = strlen(W.ssid);
    int want = sl && b->ssid_len == sl && !mcmp(b->ssid, W.ssid, sl), weak = -1;
    for (int i = 0; i < W.nscan; i++) {
        int theirs = sl && W.scan[i].ssid_len == sl && !mcmp(W.scan[i].ssid, W.ssid, sl);
        if (theirs) continue;
        if (weak < 0 || W.scan[i].rssi < W.scan[weak].rssi) weak = i;
    }
    if (weak >= 0 && (want || b->rssi > W.scan[weak].rssi)) W.scan[weak] = *b;
}
/* "..cc:dd:ee": the last three octets of a BSSID, enough to tell an office's access points apart in the log */
static const char *b3(const uint8_t *b)
{
    static char t[4][12]; static unsigned r;
    char *o = t[r++ & 3];
    static const char hx[] = "0123456789abcdef";
    o[0] = o[1] = '.';
    for (int i = 0; i < 3; i++) { o[2 + 3 * i] = hx[b[3 + i] >> 4]; o[3 + 3 * i] = hx[b[3 + i] & 15]; o[4 + 3 * i] = i < 2 ? ':' : 0; }
    return o;
}
#ifdef PSMAC
static void ap_event(const struct fm_event *e);
#endif
static void event(const struct fm_event *e)
{
#ifdef PSMAC
    if (W.ap_want && e->type != E_ESCAN_RESULT) { ap_event(e); return; }   /* an access point's events are not a station's */
#endif
    switch (e->type) {
    case E_ESCAN_RESULT:
        if (e->status == E_STATUS_PARTIAL) fm_escan_parse(e->data, e->datalen, scan_add, 0);
        else { W.scan_done = 1; W.scan_status = (int32_t)e->status; }   /* aborted or cut short too: the status says (conn_tick) */
        break;
    case E_SET_SSID:
        /* While connected, a join's end is not ours: the firmware's own attempt (a roam, or an 802.11v transition it
           made). On the owner's SE (25 November 2026) "join failed (status 1)" came seconds after every connection,
           just before the firmware left the access point (event 11, reason 8): it is noted, not counted as our join. */
        if (U(0)->state == AN_ST_CONNECTED) {
            if (e->status != E_STATUS_SUCCESS) {
                W.fw_attempt_at = now_ms();
                wifi_log("the firmware's own join attempt failed while connected (status %u: %s, %s): not counted as ours\n", e->status, fm_status_word(e->status), b3(e->addr));
            } else wifi_log("the firmware joined %s on its own\n", b3(e->addr));
        } else if (e->status != E_STATUS_SUCCESS) { W.ev_setssid_fail = 1; wifi_log("join failed (status %u: %s)\n", e->status, fm_status_word(e->status)); }
        break;
    case E_ROAM:
        /* the firmware roamed between the network's access points (Linux's brcmf_notify_roaming_status): the link stays
           up; conn_tick reads the new BSSID. A link-down event before it was the old access point's. */
        if (e->status == E_STATUS_SUCCESS) { W.ev_roamed = 1; W.ev_link_down = 0; wifi_log("the firmware roamed (reason %u) to %s\n", e->reason, b3(e->addr)); }
        else { W.fw_attempt_at = now_ms(); wifi_log("the firmware's roam attempt ended: status %u (%s), reason %u\n", e->status, fm_status_word(e->status), e->reason); }
        break;
    case E_ROAM_START: case E_JOIN_START: case E_ASSOC_START: case E_ROAM_PREP:
        if (U(0)->state == AN_ST_CONNECTED) {         /* while joining these are ours: only the firmware's own are said */
            W.fw_attempt_at = now_ms();
            wifi_log("the firmware starts a %s of its own while connected (event %u, reason %u, %s)\n",
                     e->type == E_ROAM_START || e->type == E_ROAM_PREP ? "roam" : e->type == E_JOIN_START ? "join" : "association", e->type, e->reason, b3(e->addr));
        }
        break;
    case E_BCNLOST:
        wifi_log("beacons lost from %s (event 31): the access point not heard for a while\n", b3(e->addr));
        break;
    case E_LINK:
        if (e->flags & E_FLAG_LINK) { W.ev_link_up = 1; wifi_log("link up\n"); }
        else { W.ev_link_down = 1; wifi_log("link down (reason %u)\n", e->reason); }
        break;
    case E_PSK_SUP:
        if (e->status == E_STATUS_FWSUP_COMPLETED) { W.ev_keyed = 1; wifi_log("WPA handshake done\n"); }
        else wifi_log("WPA handshake: state %u, reason %u%s\n", e->status, e->reason,
                      e->status == 7 ? " (timed out: is the password right?)" : "");
        break;
    case E_AUTH: case E_ASSOC: case E_REASSOC:
        wifi_log("%s: status %u, reason %u\n", e->type == E_AUTH ? "authentication" : "association", e->status, e->reason);
        if (e->type != E_AUTH && e->status == E_STATUS_SUCCESS) W.ev_assoc_ok = 1;
        break;
    case E_DEAUTH: case E_DEAUTH_IND: case E_DISASSOC: case E_DISASSOC_IND:
        /* 6 and 12 are the access point's frames; 5 and 11 our own side's (the firmware left: a roam or join of its own,
           or our WLC_DISASSOC) - until 25 November 2026 both were "disconnected by the network" */
        W.drop_by_ap = e->type == E_DEAUTH_IND || e->type == E_DISASSOC_IND;
        W.drop_ev = (int)e->type; W.drop_reason = (int)e->reason;
        wifi_log("%s %s %s (event %u, reason %u: %s)\n", e->type == E_DEAUTH || e->type == E_DEAUTH_IND ? "deauthenticated" : "disassociated",
                 W.drop_by_ap ? "by the access point" : "by our side, from", b3(e->addr), e->type, e->reason, fm_reason_word(e->reason));
        W.ev_link_down = 1;
        break;
    default:
        break;
    }
}

/* ---- receive: data frames into the board's slots ---- */
#ifdef PSMAC
/* psmac_netprobe.c: an echo's time through the kernel (the WiFi's 80 ms ping, 20 November) */
void psmac_netprobe_tx(int unit, const volatile uint8_t *f, uint32_t len, int stage);
void psmac_netprobe_rx(int unit, const volatile uint8_t *f, uint32_t len, uint32_t slot, volatile uint32_t *rx_tail);
#define NETPROBE_TX(u, f, l, st) psmac_netprobe_tx(u, f, l, st)
#define NETPROBE_RX(u, f, l, sl, t) psmac_netprobe_rx(u, f, l, sl, t)
/* psmac_ip.c: the kernel's own address, for the remote control (docs/13). Every received frame is
   shown to it first; its frames go out when the Amiga's (the Mac's) ring is empty */
#include "../psmac/psmac_ip.h"
#else
#define NETPROBE_TX(u, f, l, st) ((void)0)
#define NETPROBE_RX(u, f, l, sl, t) ((void)0)
#endif
static void rx_payload(const uint8_t *p, unsigned len)
{
    unsigned dl;
    const uint8_t *d = bcdc_data(p, len, &dl);
    struct fm_event e;
    volatile struct an_unit *u = U(0);
    if (!d) return;
    if (fm_event_parse(d, dl, &e)) { event(&e); return; }
    if (dl < 14 || dl > AN_SLOT_BYTES - 8) { u->rx_drop++; trace_rx(0, d, dl, "DROPPED: length"); return; }
#ifdef PSMAC
    if (psmac_ip_active && psmac_ip_rx(0, d, dl, u->mac, 0) != IPK_TO_MAC) return;
#endif
    if (!u->rx_on) { u->rx_drop++; trace_rx(0, d, dl, "DROPPED: the Amiga takes no frames (RXOFF)"); return; }
    if (u->rx_head - u->rx_tail >= AN_RX_SLOTS) { u->rx_drop++; trace_rx(0, d, dl, "DROPPED: all 16 slots full"); return; }
    trace_rx(0, d, dl, "to the Amiga");
    volatile struct an_slot *s = (volatile struct an_slot *)(anb + AN_OFF_RX(0) + (u->rx_head % AN_RX_SLOTS) * AN_SLOT_BYTES);
    ncopy(s->data, d, dl);
    s->len = dl;
    s->flags = (d[0] & 1) ? (d[0] == 0xff && d[1] == 0xff ? AN_SF_BCAST : AN_SF_MCAST) : 0;
    NETPROBE_RX(0, d, dl, u->rx_head, &u->rx_tail);
    __asm__ volatile("dmb ish" ::: "memory");
    u->rx_head++;
    u->rx_pkts++; u->rx_bytes += dl;
    W.traffic_ms = now_ms();
    an_irq();
}
static void rx_fail(void)
{
    sdh_abort(&W.h, 2);
    chip_w8(&W.c, 0x1000d, 1);                      /* FRAMECTRL: RF_TERM */
    for (int i = 0; i < 100; i++) {
        if (!chip_r8(&W.c, 0x1001b) && !chip_r8(&W.c, 0x1001c)) break;
        wifi_udelay(10);
    }
    chip_w32(&W.c, W.c.sdio_base + SD_TOSBMAILBOX, 1);       /* NAK */
    W.rxskip = 1;
    W.rxskip_t0 = now_ms();
}
static void tx_fail(void)
{
    sdh_abort(&W.h, 2);
    chip_w8(&W.c, 0x1000d, 2);                      /* FRAMECTRL: WF_TERM */
    for (int i = 0; i < 100; i++) {
        if (!chip_r8(&W.c, 0x10019) && !chip_r8(&W.c, 0x1001a)) break;
        wifi_udelay(10);
    }
}
static void rx_glom(const uint8_t *desc, unsigned dlen)
{
    unsigned nsub = dlen / 2;
    uint16_t sub[32];
    if (nsub > 32) nsub = 32;
    for (unsigned i = 0; i < nsub; i++) sub[i] = fm_le16(desc + 2 * i);
    for (unsigned i = 0; i < nsub; i++) {
        unsigned n = (sub[i] + 3) & ~3u;
        if (n > BUFSZ || n < SDPCM_HDRLEN) { rx_fail(); return; }
        if (chip_frame(&W.c, W.rxbuf, n, 0)) return;
        uint8_t *f = W.rxbuf;
        if (i == 0) f += SDPCM_HDRLEN;
        struct sdpcm_rx r;
        if (sdpcm_parse(f, &r) || r.chan == SDPCM_CHAN_CONTROL || r.chan == SDPCM_CHAN_GLOM ||
            (unsigned)(f - W.rxbuf) + r.len > n) continue;
        rx_payload(f + r.dataoff, r.len - r.dataoff);
    }
}
static void rx_all(void)
{
    unsigned nextlen = 0;
    if (W.rxskip) {
        if (now_ms() - W.rxskip_t0 < 200) return;
        W.rxskip = 0;
    }
    for (int guard = 0; guard < 64; guard++) {
        uint8_t *buf = W.rxbuf;
        unsigned first = nextlen && nextlen <= BUFSZ ? nextlen : 16;
        if (chip_frame(&W.c, buf, first, 0)) break;
        struct sdpcm_rx r;
        int k = sdpcm_parse(buf, &r);
        if (k == 1) break;
        if (k < 0 && !nextlen && !W.rx_signalled) { W.junk++; break; }
        if (k < 0 || (nextlen && r.len > first)) { wifi_log("bad frame header (%d)\n", k); rx_fail(); break; }
        W.maxseq = (uint8_t)(r.maxseq - W.txseq) > 0x40 ? (uint8_t)(W.txseq + 2) : r.maxseq;
        W.flowctl = r.flowctl;
        if (r.len > BUFSZ) { rx_fail(); break; }
        unsigned total = (r.len + 3) & ~3u;
        if (total > first && chip_frame(&W.c, buf + first, total - first, 0)) break;
        nextlen = r.nextlen;
        const uint8_t *p = buf + r.dataoff;
        unsigned plen = r.len - r.dataoff;
        switch (r.chan) {
        case SDPCM_CHAN_CONTROL: ctl_rx(p, plen); break;
        case SDPCM_CHAN_EVENT:
        case SDPCM_CHAN_DATA: rx_payload(p, plen); break;
        case SDPCM_CHAN_GLOM: {
            uint8_t d[64]; unsigned n = plen > sizeof d ? sizeof d : plen;
            memcpy(d, p, n);
            rx_glom(d, n);
            nextlen = 0;
            break;
        }
        default: break;
        }
    }
}
static void poll_frames(void)
{
    if (!W.chip_up) return;
#ifdef PSMAC
    if (W.sim) { struct fm_event e; U(0)->polls++; while (psmac_wfw_event(&e)) event(&e); return; }
#endif
    uint32_t is, mb, wk = W.c.wakes;
    U(0)->polls++;
    if (chip_irq_ack(&W.c, &is, &mb)) {
        if (++W.ack_fail == 50) {
            wifi_log("the chip's status cannot be read (SDIO errors %u)\n", W.h.errors);
            W.lost = "the WiFi chip stopped answering";
            W.chip_up = 0;                          /* the main loop starts it again */
        }
        return;
    }
    W.ack_fail = 0;
    if (!wk && W.c.wakes) wifi_log("the chip sleeps when idle (save/restore): waking it as needed\n");
    if (mb & HMB_DATA_NAKHANDLED) { W.rxskip = 0; is |= I_HMB_FRAME_IND; }
    if (is & I_HMB_FC_CHANGE) W.fcstate = (is & I_HMB_FC_STATE) != 0;
    if (mb & HMB_DATA_FWHALT) {
        W.chip_up = 0;
        link_set(0);
        wstate(AN_ST_FAILED, "the WiFi firmware stopped");
        return;
    }
    W.rx_signalled = (is & I_HMB_FRAME_IND) != 0;
    rx_all();
    U(0)->sd_errors = W.h.errors;
}

/* ---- transmit: the board's slots to the chip ---- */
/* an IPv4 UDP datagram to port 67, a DHCP client's (the Mac getting or renewing its address over the WiFi) */
static int is_dhcp_request(const uint8_t *f, uint32_t len)
{
    if (len < 14 + 20 + 8 || f[12] != 0x08 || f[13] != 0x00 || (f[14] >> 4) != 4 || f[23] != 17) return 0;
    uint32_t ihl = (f[14] & 15u) * 4u;
    if (ihl < 20 || 14 + ihl + 8 > len) return 0;
    return f[14 + ihl + 2] == 0 && f[14 + ihl + 3] == 67;
}
static void tx_all(void)
{
    volatile struct an_unit *u = U(0);
#ifdef PSMAC
    if (W.sim) {                                    /* the simulated firmware carries no frames: sent into the air */
        while (u->tx_tail != u->tx_head) { if (u->link) u->tx_pkts++; else u->tx_err++; u->tx_tail++; }
        an_irq();
        return;
    }
#endif
    while (u->tx_tail != u->tx_head) {
        if (!sdpcm_credit(W.txseq, W.maxseq) || W.fcstate || (W.flowctl & 1)) break;
        __asm__ volatile("dmb ish" ::: "memory");
        volatile struct an_slot *s = (volatile struct an_slot *)(anb + AN_OFF_TX(0) + (u->tx_tail % AN_TX_SLOTS) * AN_SLOT_BYTES);
        uint32_t len = s->len;
        if (len < 14 || len > 1518 || !u->link) u->tx_err++;
        else {
            uint8_t *f = W.txbuf, *q = f + SDPCM_HDRLEN + BCDC_DATA_HDR;
            for (uint32_t i = 0; i < len; i++) q[i] = s->data[i];
#ifdef PSMAC
            psmac_ip_mac_tx(0, q, len);
#endif
            if (is_dhcp_request(q, len) && bt_pi_wifi_busy) bt_pi_wifi_busy("the Mac's DHCP over the WiFi");   /* Bluetooth quiet meanwhile (docs/08 item 140) */
            NETPROBE_TX(0, q, len, 0);
            unsigned n = sdpcm_frame(f, SDPCM_CHAN_DATA, W.txseq, len);
            int bad = chip_frame(&W.c, f, n, 1);
            if (!bad) NETPROBE_TX(0, q, len, 1);
            trace_tx(0, q, len, !bad);
            if (bad) { u->tx_err++; tx_fail(); }
            else { W.txseq++; u->tx_pkts++; u->tx_bytes += len; }
        }
        __asm__ volatile("dmb ish" ::: "memory");
        u->tx_tail++;
        W.traffic_ms = now_ms();
        an_irq();                                   /* a slot free again */
    }
#ifdef PSMAC
    /* the kernel's own frames, once the Mac's are out, with the same credit */
    while (psmac_ip_active && u->link && u->tx_tail == u->tx_head && sdpcm_credit(W.txseq, W.maxseq) && !W.fcstate && !(W.flowctl & 1)) {
        uint8_t *f = W.txbuf, *q = f + SDPCM_HDRLEN + BCDC_DATA_HDR;
        uint32_t len = psmac_ip_tx_take(0, q, 1518);
        if (!len) break;
        if (is_dhcp_request(q, len) && bt_pi_wifi_busy) bt_pi_wifi_busy("the kernel's DHCP over the WiFi");   /* the kernel's own address, the same */
        unsigned n = sdpcm_frame(f, SDPCM_CHAN_DATA, W.txseq, len);
        if (chip_frame(&W.c, f, n, 1)) tx_fail();
        else W.txseq++;
        W.traffic_ms = now_ms();
    }
#endif
}

/* ---- bring-up ---- */
static int clm_upload(const uint8_t *blob, uint32_t len)
{
    uint8_t *d = W.txbuf;
    uint32_t off = 0;
    while (off < len) {
        uint32_t n = len - off > 1400 ? 1400 : len - off;
        uint16_t flag = 0x1000 | (off == 0 ? 2 : 0) | (off + n >= len ? 4 : 0);
        fm_put16(d, flag); fm_put16(d + 2, 2); fm_put32(d + 4, n); fm_put32(d + 8, 0);
        memcpy(d + 12, blob + off, n);
        int32_t e = iov_set("clmload", d, 12 + n);
        if (e) { wifi_log("CLM upload failed at %u (%d)\n", off, e); return -1; }
        off += n;
    }
    return 0;
}
/* The Mac's receive filter (promiscuous, all multicast), as its driver last asked. Until 21 November
   2026 a request that came while the chip was still starting was answered and dropped, and the chip
   kept its default of no multicast at all. On the real SE the WiFi now comes up after Riptide has
   opened its interfaces and asked for all multicast (IPv6's all-nodes and solicited-node groups), so
   router advertisements and the router's neighbour solicitations never reached the Mac: no global
   IPv6 address, and no IPv6 beyond the link. The wish is now kept and applied when the chip comes up. */
static uint32_t wifi_filter_want;
static int wifi_filter_set;
static void wifi_filter_apply(void)
{
    cmd_int(10, (wifi_filter_want & AN_FILT_PROMISC) ? 1 : 0);                 /* 10: WLC_SET_PROMISC */
    iov_int("allmulti", (wifi_filter_want & AN_FILT_ALLMULTI) ? 1 : 0);
}
/* a word in the firmware's "cap" list */
static int cap_has(const char *w)
{
    unsigned n = strlen(w);
    for (char *p = W.caps; *p; p++)
        if ((p == W.caps || p[-1] == ' ') && !mcmp(p, w, n) && (p[n] == ' ' || !p[n])) return 1;
    return 0;
}
/* Roaming between the access points of one SSID (docs/08 item 147). Linux's brcmfmac leaves it to the firmware unless
   roamoff=1 (brcmf_dongle_roam: roam_off 0, WLC_SET_ROAM_TRIGGER -75 dBm, WLC_SET_ROAM_DELTA 20 dB, for all bands), and so
   does this by default, handling E_ROAM. wifi.roam=off sets roam_off 1 (the kernel moves itself, on a loss, with the kicked
   list); wifi.roam=auto (the default) turns it off for the session when the firmware's own attempts twice came just before
   the link was lost. Set before WLC_UP at the start, and before a join (not associated) when it changes. */
static void roam_apply(void)
{
    static const char *const mw[] = { "auto", "on", "off" };
    int off = W.roam_mode == 2 || W.roam_off_now;
    int32_t e = iov_int("roam_off", off ? 1u : 0u);
    W.roam_applied = off;
    if (off) {
        wifi_log("roaming: off in the firmware (roam_off 1%s; %s): the kernel moves between access points when the link is lost\n", e ? " REFUSED" : "",
                 W.roam_mode == 2 ? "wifi.roam=off" : "turned off for this session by wifi.roam=auto");
        return;
    }
    uint8_t b[8];
    fm_put32(b, (uint32_t)-75); fm_put32(b + 4, 3);       /* WL_ROAM_TRIGGER_LEVEL, BRCM_BAND_ALL */
    int32_t e1 = ctl(WLC_SET_ROAM_TRIGGER, 1, b, 8, 0);
    fm_put32(b, 20); fm_put32(b + 4, 3);                   /* WL_ROAM_DELTA */
    int32_t e2 = ctl(WLC_SET_ROAM_DELTA, 1, b, 8, 0);
    wifi_log("roaming: the firmware's own, between the network's access points (roam_off 0%s, trigger -75 dBm%s, delta 20 dB%s; wifi.roam=%s)\n",
             e ? " REFUSED" : "", e1 ? " REFUSED" : "", e2 ? " REFUSED" : "", mw[W.roam_mode >= 0 && W.roam_mode < 3 ? W.roam_mode : 0]);
}
static int fw_setup(const struct fwfile *clm);
static int bringup(void)
{
    static const char *const s43455[] = { "cyfmac43455-sdio", "cyfmac43455-sdio-standard", "brcmfmac43455-sdio.raspberrypi,4-model-b", "brcmfmac43455-sdio", 0 };
    static const char *const s43456[] = { "brcmfmac43456-sdio", "brcmfmac43456-sdio.raspberrypi,4-compute-module", 0 };
    static const char *const nv43455[] = { "brcmfmac43455-sdio.raspberrypi,4-model-b", "brcmfmac43455-sdio", "cyfmac43455-sdio", 0 };
    static const char *const nv43456[] = { "brcmfmac43456-sdio.raspberrypi,4-compute-module", "brcmfmac43456-sdio", 0 };
    /* the Pi 3B and Zero W carry the older BCM43438 (chip id 43430); the Zero 2 W a 43436 that answers as a 43430 too */
    static const char *const s43430[] = { "brcmfmac43430-sdio", "cyfmac43430-sdio", "brcmfmac43436-sdio", "brcmfmac43430-sdio.raspberrypi,3-model-b", 0 };
    static const char *const nv43430[] = { "brcmfmac43430-sdio.raspberrypi,3-model-b", "brcmfmac43430-sdio", "brcmfmac43436-sdio", 0 };
    volatile struct an_unit *u = U(0);
    wstate(AN_ST_STARTING, "starting the WiFi chip");
    if (!W.rxbuf) {
        uint8_t *m = tlsf_malloc_aligned(tlsf, 4 * BUFSZ, 64);
        W.c.bounce = tlsf_malloc_aligned(tlsf, CHIP_BOUNCE, 64);
        if (!m || !W.c.bounce) { wstate(AN_ST_FAILED, "no memory"); return -1; }
        W.rxbuf = m; W.txbuf = m + BUFSZ; W.ctlbuf = m + 2 * BUFSZ; W.iobuf = m + 3 * BUFSZ;
    }
    W.chip_up = 0;
    scan_reset();
#ifdef PSMAC
    if (W.sim) {
        u->chip = W.c.id = 0xa9a6; u->chiprev = W.c.rev = 1; u->sdmhz = 25;
        W.chip_up = 1;
        wifi_log("the simulated firmware (mac.wifisimfw=1): no chip, no SDIO\n");
        return fw_setup(0);
    }
#endif
    wifi_where("pins and power");
    sdh_pins(alice_pi_is_2711());
    wl_on(0);
    wifi_udelay(20000);
    wl_on(1);
    wifi_udelay(150000);
    wifi_where("the EMMC clock (mailbox)");
    mbox_try(0x00038001u, 1, 1, 0);                 /* SET_CLOCK_STATE: the EMMC clock on */
    uint32_t base = 0;
    if (mbox_try(0x00030002u, 1, 0, &base) || !base) base = 250000000u;     /* GET_CLOCK_RATE */
    wifi_log("SD host clock %u Hz\n", base);
    wifi_where("SD host");
    W.h.regs = (volatile uint32_t *)(uintptr_t)SDH_BASE;
#ifdef PSMAC
    /* the reason in plain words, and what it means for the Mac: nothing tries again on its own (PiWiFi's
       Join or a restart does), so a Pi with no working WiFi chip costs the Mac this one start */
    if (sdh_init(&W.h, base) || sdh_bus4(&W.h, W.sdclock)) { wstate(AN_ST_FAILED, "the WiFi chip does not answer (SDIO): WiFi off, the Mac carries on"); return -2; }
#else
    if (sdh_init(&W.h, base) || sdh_bus4(&W.h, W.sdclock)) { wstate(AN_ST_FAILED, "the WiFi chip does not answer on SDIO"); return -2; }
#endif
    W.h.regs[0x38 / 4] = 0;                         /* never signal: GIC 158 is the SD card's too */
    u->sdmhz = W.h.sd_hz / 1000000u;
    wifi_where("chip");
#ifdef PSMAC
    if (chip_attach(&W.c, &W.h)) { wstate(AN_ST_FAILED, "WiFi chip not recognised: WiFi off, the Mac carries on"); return -3; }
#else
    if (chip_attach(&W.c, &W.h)) { wstate(AN_ST_FAILED, "WiFi chip not recognised"); return -3; }
#endif
    u->chip = W.c.id; u->chiprev = W.c.rev;
    int is56 = W.c.id == 0x4345 && W.c.rev == 9;
    int is30 = W.c.id == 43430 || W.c.id == 0xa9a6;
    const char *const *stems = is56 ? s43456 : is30 ? s43430 : s43455;
    const char *const *nvs = is56 ? nv43456 : is30 ? nv43430 : nv43455;
#ifdef PSMAC
    /* one container for every board (get-firmware.sh): a Zero 2 W's chip answers as a 43430, so
       the board says which set is its own. Zero 2 Ws come with two chips, the BCM43436 and the
       later 43436s. Linux tells them apart by the 43430's revision: revision 1 is the name
       "brcmfmac43430-sdio" (the Pi 3B's 43438 is revision 1 too), and Raspberry Pi's firmware
       package points that name on a Zero 2 W (brcmfmac43430-sdio.raspberrypi,model-zero-2-w.bin
       and .txt) at brcmfmac43436s-sdio, with no CLM blob of its own, so the 43430's is taken;
       any other revision is "brcmfmac43436-sdio". The same choice here. */
    static const char *const s43436[] = { "brcmfmac43436-sdio", "brcmfmac43430-sdio", "cyfmac43430-sdio", 0 };
    static const char *const nv43436[] = { "brcmfmac43436-sdio", "brcmfmac43430-sdio", 0 };
    static const char *const s43436s[] = { "brcmfmac43436s-sdio", "brcmfmac43430-sdio", "cyfmac43430-sdio", 0 };
    static const char *const nv43436s[] = { "brcmfmac43436s-sdio", "brcmfmac43430-sdio", 0 };
    if (is30 && board_type() == 0x12) {
        int s_variant = W.c.rev == 1;
        stems = s_variant ? s43436s : s43436;
        nvs = s_variant ? nv43436s : nv43436;
        wifi_log("Zero 2 W: chip 43430 revision %u, the %s\n", W.c.rev, s_variant ? "BCM43436s (brcmfmac43436s-sdio)" : "BCM43436 (brcmfmac43436-sdio)");
    }
#endif
    const struct fwfile *fb = fw_find(stems, ".bin");
    const struct fwfile *nv = fw_find(nvs, ".txt");
    const struct fwfile *clm = fw_find(stems, ".clm_blob");
    if (!fb) { wstate(AN_ST_NOFW, is56 ? "firmware missing: put brcmfmac43456-sdio.bin " FW_WHERE : is30 ? "firmware missing: put brcmfmac43430-sdio.bin " FW_WHERE : "firmware missing: put cyfmac43455-sdio-standard.bin " FW_WHERE); return -4; }
    if (!nv) { wstate(AN_ST_NOFW, is30 ? "NVRAM missing: put brcmfmac43430-sdio.txt " FW_WHERE : "NVRAM missing: put brcmfmac43455-sdio.txt " FW_WHERE); return -4; }
    wifi_log("chip %x rev %u: %s, %s%s%s\n", W.c.id, W.c.rev, fb->name, nv->name, clm ? ", " : "", clm ? clm->name : "");
    uint8_t *nvimg = tlsf_malloc(tlsf, nv->len + 64);
    if (!nvimg) { wstate(AN_ST_FAILED, "no memory"); return -1; }
    uint32_t nvlen = fm_nvram(nv->p, nv->len, nvimg);
    wifi_where("firmware download");
    int e = chip_load(&W.c, fb->p, fb->len, nvimg, nvlen);
    tlsf_free(tlsf, nvimg);
    if (e) { wstate(AN_ST_FAILED, "the firmware did not start"); return -5; }
    W.chip_up = 1;
    return fw_setup(clm);
}
/* the firmware is running: its settings, as Linux's brcmf_c_preinit_dcmds and brcmf_config_dongle */
static int fw_setup(const struct fwfile *clm)
{
    volatile struct an_unit *u = U(0);
    W.txseq = 0xff; W.maxseq = 0; W.rxskip = 0;
    wifi_where("firmware setup");
    iov_int("bus:txglom", 0);
    uint32_t ver = 0;
    if (cmd_get_int(WLC_GET_VERSION, &ver)) { W.chip_up = 0; wstate(AN_ST_FAILED, "the firmware does not answer"); return -6; }
#ifdef PSMAC
    W.iover = ver;
#endif
    iov_get("cur_etheraddr", W.mac, 6);
    memcpy(W.fmac, W.mac, 6);
    for (int i = 0; i < 6; i++) u->factory_mac[i] = W.fmac[i];
    {
        uint8_t a[6];
        if (mac_wanted(W.want_mac, W.want_oui, W.fmac, a)) {
            if (iov_set("cur_etheraddr", a, 6)) wifi_log("the firmware kept its MAC address\n");
            else memcpy(W.mac, a, 6);
        }
    }
    memset(W.fwver, 0, sizeof W.fwver);
    iov_get("ver", W.fwver, sizeof W.fwver - 1);
    for (char *p = W.fwver; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    for (int i = 0; i < 6; i++) u->mac[i] = W.mac[i];
    for (unsigned i = 0; i < sizeof W.fwver; i++) u->fwver[i] = W.fwver[i];
    wifi_log("MAC %02x:%02x:%02x:%02x:%02x:%02x, %s\n", W.mac[0], W.mac[1], W.mac[2], W.mac[3], W.mac[4], W.mac[5], W.fwver);
    if (clm) { if (!clm_upload(clm->p, clm->len)) wifi_log("CLM uploaded\n"); }
    else wifi_log("no .clm_blob: the firmware's built-in regulatory data\n");
    memset(W.caps, 0, sizeof W.caps);
    iov_get("cap", W.caps, sizeof W.caps - 1);
    wifi_log("firmware features: %s\n", W.caps);
    {   /* Bluetooth coexistence (docs/08 item 140): on the Pi 3B's BCM43438 WiFi and Bluetooth share one antenna, and the
           WiFi firmware divides its time as the NVRAM says (brcmfmac43430-sdio.txt: btc_mode=1, btc_params1 and 8, as
           Linux leaves it: brcmfmac sends no coexistence settings of its own). Said here so that a log shows it. */
        uint8_t b[4] = { 0, 0, 0, 0 };
        if (!iov_get("btc_mode", b, 4)) wifi_log("Bluetooth coexistence: btc_mode %u (the NVRAM's; 0 none, 1 time sharing)\n", fm_le32(b));
    }
    W.has_sae = 0;
    for (char *p = W.caps; *p; p++)
        if ((p == W.caps || p[-1] == ' ') && p[0] == 's' && p[1] == 'a' && p[2] == 'e' && (p[3] == ' ' || !p[3])) W.has_sae = 1;
    if (W.country[0] && W.country[1]) {
        uint8_t cc[12];
        fm_country(cc, W.country);
        if (iov_set("country", cc, 12)) wifi_log("country %s not accepted\n", W.country);
    }
    if (wifi_filter_set) { wifi_filter_apply(); wifi_log("receive filter as the Mac asked before the chip was up: %s\n", (wifi_filter_want & AN_FILT_PROMISC) ? "promiscuous" : (wifi_filter_want & AN_FILT_ALLMULTI) ? "all multicast" : "unicast and broadcast"); }
    uint8_t mask[EVENT_MASK_LEN];
    memset(mask, 0, sizeof mask);
    static const uint8_t evs[] = { E_SET_SSID, E_LINK, E_AUTH, E_ASSOC, E_REASSOC, E_DEAUTH, E_DEAUTH_IND, E_DISASSOC, E_DISASSOC_IND, E_PSK_SUP, E_ESCAN_RESULT, E_IF,
                                   E_ROAM, E_BCNLOST, E_ROAM_PREP, E_JOIN_START, E_ROAM_START, E_ASSOC_START };   /* the firmware's own moves: docs/08 item 147 */
    for (unsigned i = 0; i < sizeof evs; i++) fm_event_mask_set(mask, evs[i]);
    iov_set("event_msgs", mask, sizeof mask);
    cmd_int(WLC_SET_SCAN_CHANNEL_TIME, 40);
    cmd_int(WLC_SET_SCAN_UNASSOC_TIME, 40);
    cmd_int(WLC_SET_SCAN_PASSIVE_TIME, 120);
    roam_apply();
    {   /* what the firmware does about 802.11v (BSS transition: an access point steering us to another) and 802.11k, said
           for a log from a real chip; nothing is changed (docs/08 item 147) */
        uint8_t b[4] = { 0, 0, 0, 0 }, t[4] = { 0, 0, 0, 0 };
        int32_t e = iov_get("wnm", b, 4), et = iov_get("bcn_timeout", t, 4);
        wifi_log("802.11v/k: the firmware lists%s%s%s%s; wnm %s%x (left as it is); bcn_timeout %u\n", cap_has("wnm") ? " wnm" : "", cap_has("bsstrans") ? " bsstrans" : "",
                 cap_has("rm") ? " rm" : "", !cap_has("wnm") && !cap_has("bsstrans") && !cap_has("rm") ? " none of wnm, bsstrans, rm" : "", e ? "not readable " : "0x", e ? 0u : fm_le32(b), et ? 0u : fm_le32(t));
    }
    cmd_int(WLC_SET_PM, (U(0)->cfg_flags & AN_WC_POWERSAVE) ? 2 : 0);      /* 2: PM_FAST */
    iov_int("arp_ol", 0); iov_int("arpoe", 0); iov_int("ndoe", 0); iov_int("toe", 0);
    cmd_int(WLC_UP, 0);
    cmd_int(WLC_SET_INFRA, 1);
    cmd_int(WLC_SET_AP, 0);
#ifdef PSMAC
    if (!W.sim) {
#endif
    W.host_c0 = __builtin_bswap32(W.h.regs[0x28 / 4]) & 0x06u;          /* CONTROL0: 4-bit bus, high speed */
    W.host_c1 = __builtin_bswap32(W.h.regs[0x2c / 4]) & 0xffe5u;        /* CONTROL1: divider, clock on, internal clock */
    W.host_hz = W.h.sd_hz;
#ifdef PSMAC
    }
#endif
    wstate(AN_ST_IDLE, W.ssid[0] ? "ready" : "ready - no network yet (Alice68-WiFi JOIN, or wifi.ssid= in cmdline.txt)");
    return 0;
}
static void cmd_finish(uint32_t seq, int result);
/* a scan the chip will never finish (restart, off): its command answered, nothing joined from it */
static void scan_reset(void)
{
    if (W.scan_cmd) { W.scan_cmd = 0; cmd_finish(W.scan_cmd_seq, -1); }
    W.scan_busy = W.scan_for_join = W.scan_done = 0;
}
static void chip_off(void)
{
    scan_reset();
    if (W.chip_up) cmd_int(WLC_DOWN, 0);
    W.chip_up = 0;
    link_set(0);
#ifdef PSMAC
    if (W.sim) return;
#endif
    wl_on(0);
}

/* ---- connecting ---- */
/* pass 0: a fresh scan (the table emptied), the firmware's dwell (40 ms per channel, as Linux); pass 1: one more,
   merged into the table, listening 80 ms on each channel (docs/08 item 147: on the owner's SE a join's scan heard one or
   two networks in an office) */
static void scan_start(int pass)
{
    uint8_t p[80];
    if (!pass) W.nscan = 0;
    W.scan_pass = pass;
    W.scan_done = 0;
    W.scan_status = -1;
    W.scan_t0 = now_ms();
    unsigned n = fm_escan_params_dwell(p, ++W.scan_sync, pass ? 80u : 0u);
    int32_t e = iov_set("escan", p, n);
    if (e) { wifi_log("scan refused (%d)\n", e); W.scan_done = 1; W.scan_status = e < -1 ? e : -2; }
    W.scan_busy = 1;
}
static void retry(const char *why)
{
    link_set(0);
    if (W.quick > 0) {                              /* just lost a connection: the next access point at once (docs/08 item 147) */
        W.quick--;
        W.retry_at = now_ms() + 300;
        wstate(AN_ST_RETRY, why);
        wifi_log("trying again at once (%d more quick tries)\n", W.quick);
        return;
    }
    W.backoff = W.backoff ? (W.backoff * 2 > 30000 ? 30000 : W.backoff * 2) : 2000;
    W.retry_at = now_ms() + W.backoff;
    wstate(AN_ST_RETRY, why);
}
/* a join of ours that did not work: counted against pinned joins, and the BSSID skipped for a minute where another is near */
static void join_failed(const char *why)
{
    fm_pick_result(&W.pick, W.pinned, 0);
    if (W.pinned) {
        fm_pick_kick(&W.pick, W.cur.bssid, now_ms(), FM_KICK_FAIL_MS, FM_KICK_FAILED);
        wifi_log("%s could not be joined: skipped for %u s while another is heard (%d failed in a row)\n", b3(W.cur.bssid), FM_KICK_FAIL_MS / 1000u, W.pick.pinned_fails);
    }
    retry(why);
}
static int hex64(const char *s)
{
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}
/* b: the access point (0: none seen); pin: name its BSSID and channel in the join, else the SSID only (the firmware
   chooses), with b's security */
static void join(const struct fm_bss *b, int pin)
{
    struct fm_bss any;
    if (!b) {
        memset(&any, 0, sizeof any);
        any.security = W.pass[0] ? SEC_WPA2 : SEC_OPEN;
        any.group_cipher = any.pairwise = 4;
        b = &any;
        pin = 0;
    }
    W.cur = *b;
    W.pinned = pin;
    int open = !W.pass[0];
    int sae = !open && (b->security & SEC_WPA3) && !(b->security & (SEC_WPA2 | SEC_WPA)) && W.has_sae;
    if (!open && (b->security & (SEC_OPEN | SEC_WEP)) && !(b->security & (SEC_WPA | SEC_WPA2 | SEC_WPA3))) {
        if (b->security & SEC_WEP) { retry("the network uses WEP, which is not supported"); return; }
        open = 1;
    }
    if (!open && (b->security & SEC_WPA3) && !(b->security & (SEC_WPA2 | SEC_WPA)) && !W.has_sae) { retry("the network is WPA3-only and this firmware has no SAE"); return; }
    if (!open && (b->security & SEC_8021X) && !(b->security & (SEC_WPA | SEC_WPA2 | SEC_WPA3))) { retry("the network is WPA-Enterprise, which is not supported"); return; }
    uint32_t wsec = 0, auth = 0;
    if (!open) {
        if (b->pairwise == 4 || b->group_cipher == 4) wsec |= WSEC_AES;
        if (b->pairwise == 2 || b->group_cipher == 2) wsec |= WSEC_TKIP;
        if (!wsec) wsec = WSEC_AES;
        auth = sae ? WPA3_AUTH_SAE_PSK : (b->security & (SEC_WPA2 | SEC_WPA3)) ? WPA2_AUTH_PSK : WPA_AUTH_PSK;
    }
    cmd_int(WLC_DISASSOC, 0);
    if (W.roam_applied != (W.roam_mode == 2 || W.roam_off_now)) roam_apply();   /* not associated now */
    iov_int("wsec", wsec);
    iov_int("wpa_auth", auth);
    iov_int("auth", sae ? 3 : 0);
    iov_int("mfp", sae ? MFP_REQUIRED : MFP_NONE);
    if (!open) {
        uint8_t ie[32];
        unsigned n = sae ? 0 : fm_build_wpaie(ie, b, 0);
        iov_set("wpaie", ie, n);
        int32_t se = iov_int("sup_wpa", 1);
        if (se <= -100) { retry("the WiFi chip is not answering"); return; }
        if (se) { retry("this firmware has no WPA supplicant: use 7.45.241 (README, WiFi)"); return; }
        uint8_t k[140];
        unsigned pl = strlen(W.pass);
        if (sae) {
            n = fm_sae_password(k, W.pass, pl);
            if (iov_set("sae_password", k, n)) { retry("SAE password not accepted"); return; }
        } else if (pl < 8 || pl > 64 || (pl == 64 && !hex64(W.pass))) {
            retry("the password must have 8 to 63 characters (or 64 hex digits)");
            return;
        } else {
            /* the key itself, as Linux gives it: PBKDF2 of the passphrase and the SSID,
               as 64 hex digits - the firmware's supplicant does not hash a passphrase */
            uint8_t pmk[32];
            uint32_t t0 = now_ms();
            if (pl != 64 || fm_hex_pmk(W.pass, pmk)) fm_wpa_pmk(W.pass, pl, W.ssid, strlen(W.ssid), pmk, wifi_relax);
            wifi_log("WPA key from the password: %u ms\n", now_ms() - t0);
            n = fm_pmk_hex(k, pmk);
            if (ctl(WLC_SET_WSEC_PMK, 1, k, n, 0)) {
                n = fm_pmk(k, W.pass, pl, 0);       /* the older passphrase form */
                if (ctl(WLC_SET_WSEC_PMK, 1, k, n, 0)) { retry("the firmware refused the password"); return; }
                wifi_log("the firmware took the password only as a passphrase\n");
            }
        }
    } else
        iov_int("sup_wpa", 0);
    uint8_t jp[80];
    unsigned sl = strlen(W.ssid);
    int known = pin;
    unsigned n = fm_join_params(jp, W.ssid, sl, known ? b->bssid : 0, known ? b->chanspec : 0);
    if (iov_set("join", jp, n)) {
        n = fm_ssid_params(jp, W.ssid, sl, known ? b->bssid : 0);
        if (ctl(WLC_SET_SSID, 1, jp, n, 0)) { retry("join refused by the firmware"); return; }
    }
    W.keyed = (uint8_t)open;
    W.ev_setssid_fail = W.ev_link_up = W.ev_link_down = W.ev_keyed = W.ev_assoc_ok = 0;
    W.join_t0 = now_ms();
    char m[96]; unsigned i = 0;
    for (const char *p = "joining "; *p; ) m[i++] = *p++;
    for (const char *p = W.ssid; *p && i < 90; ) m[i++] = *p++;
    m[i] = 0;
    wstate(AN_ST_JOINING, m);
    if (pin) wifi_log("%s, channel %u, %s\n", b3(b->bssid), b->channel, open ? "open" : sae ? "WPA3-SAE" : (b->security & (SEC_WPA2 | SEC_WPA3)) ? "WPA2-PSK" : "WPA-PSK");
    else wifi_log("by name only: the firmware chooses the access point, %s\n", open ? "open" : sae ? "WPA3-SAE" : (b->security & (SEC_WPA2 | SEC_WPA3)) ? "WPA2-PSK" : "WPA-PSK");
}
/* the link's figures for the board; 1 if the BSSID is not the one we had (a roam, or the firmware's choice), its
   channel then from the scan or the firmware */
static int connect_info(void)
{
    uint32_t v;
    volatile struct an_unit *u = U(0);
    int moved = 0;
    if (!cmd_get_int(WLC_GET_RSSI, &v)) W.rssi = (int32_t)v;
    if (!cmd_get_int(WLC_GET_RATE, &v)) W.rate = (int)(v / 2);
    uint8_t b[8];
    if (!ctl(WLC_GET_BSSID, 0, b, 6, 6) && (b[0] | b[1] | b[2] | b[3] | b[4] | b[5]) && mcmp(W.cur.bssid, b, 6)) {
        memcpy(W.cur.bssid, b, 6);
        moved = 1;
        int found = 0;
        for (int i = 0; i < W.nscan && !found; i++) if (!mcmp(W.scan[i].bssid, b, 6)) { W.cur.channel = W.scan[i].channel; W.cur.chanspec = W.scan[i].chanspec; found = 1; }
        uint8_t ci[12];
        if (!found && !ctl(WLC_GET_CHANNEL, 0, ci, 12, 12) && fm_le32(ci) && fm_le32(ci) < 200) W.cur.channel = (uint8_t)fm_le32(ci);   /* hw_channel */
    }
    u->rssi = W.rssi; u->speed_mbps = (unsigned)W.rate; u->channel = W.cur.channel; u->security = W.cur.security;
    for (int i = 0; i < 6; i++) u->bssid[i] = W.cur.bssid[i];
    unsigned i = 0;
    for (; W.ssid[i] && i < 35; i++) u->ssid[i] = W.ssid[i];
    u->ssid[i] = 0;
    return moved;
}
/* the access points of the SSID in the scan, strongest first: one line each the first time, then one line in all */
static void scan_log_aps(uint32_t now)
{
    int idx[AN_SCAN_MAX], m = 0;
    unsigned sl = strlen(W.ssid);
    for (int i = 0; i < W.nscan; i++)
        if (W.scan[i].ssid_len == sl && !mcmp(W.scan[i].ssid, W.ssid, sl)) {
            int j = m++;
            while (j > 0 && W.scan[idx[j - 1]].rssi < W.scan[i].rssi) { idx[j] = idx[j - 1]; j--; }
            idx[j] = i;
        }
    if (!m) return;
    if (!W.aps_logged) {
        W.aps_logged = 1;
        for (int j = 0; j < m; j++) {
            const struct fm_bss *b = &W.scan[idx[j]];
            const struct fm_kick *k = fm_pick_kicked(&W.pick, b->bssid, now);
            wifi_log("  AP %s: channel %u, %d dBm, %s%s\n", b3(b->bssid), b->channel, b->rssi,
                     (b->security & SEC_WPA3) && !(b->security & SEC_WPA2) ? "WPA3" : (b->security & SEC_WPA2) ? ((b->security & SEC_WPA3) ? "WPA2/WPA3" : "WPA2") :
                     (b->security & SEC_WPA) ? "WPA" : (b->security & SEC_WEP) ? "WEP" : "open", k ? " (skipped for now)" : "");
        }
        return;
    }
    char t[160]; unsigned n = 0; int anyk = 0;
    for (int j = 0; j < m && j < 6; j++) {
        const struct fm_bss *b = &W.scan[idx[j]];
        const char *id = b3(b->bssid);
        if (n) { t[n++] = ','; t[n++] = ' '; }
        while (*id) t[n++] = *id++;
        t[n++] = ' '; t[n++] = 'c'; t[n++] = 'h';
        if (b->channel >= 100) t[n++] = (char)('0' + b->channel / 100);
        if (b->channel >= 10) t[n++] = (char)('0' + b->channel / 10 % 10);
        t[n++] = (char)('0' + b->channel % 10);
        t[n++] = ' '; t[n++] = '-';
        int r = b->rssi < 0 ? -b->rssi : b->rssi;
        if (r >= 100) t[n++] = (char)('0' + r / 100);
        t[n++] = (char)('0' + r / 10 % 10); t[n++] = (char)('0' + r % 10);
        if (fm_pick_kicked(&W.pick, b->bssid, now)) { t[n++] = '*'; anyk = 1; }
    }
    t[n] = 0;
    wifi_log("  %d APs: %s%s%s\n", m, t, m > 6 ? ", ..." : "", anyk ? " (* skipped for now)" : "");
}
static void scan_publish(void)
{
    volatile struct an_scan *t = (volatile struct an_scan *)(anb + AN_OFF_SCAN);
    for (int k = 0; k < W.nscan; k++) {
        for (int i = 0; i < 6; i++) t[k].bssid[i] = W.scan[k].bssid[i];
        unsigned i = 0;
        for (; i < W.scan[k].ssid_len && i < 32; i++) t[k].ssid[i] = W.scan[k].ssid[i];
        t[k].ssid[i] = 0;
        t[k].rssi = W.scan[k].rssi;
        t[k].channel = W.scan[k].channel;
        t[k].security = W.scan[k].security;
    }
    U(0)->scan_count = (unsigned)W.nscan;
    U(0)->scan_seq++;
}
static void cmd_finish(uint32_t seq, int result)
{
    volatile struct an_unit *u = U(0);
    if (seq != u->cmd_seq) return;                  /* the Amiga has moved on: done is never moved backwards */
    u->cmd_result = result;
    __asm__ volatile("dmb ish" ::: "memory");
    u->cmd_done = seq;
    an_irq();
}
/* the words for why a kicked BSSID is skipped */
static const char *kick_word(int why) { return why == FM_KICK_FAILED ? "could not be joined" : why == FM_KICK_BY_AP ? "disconnected us" : "lost the link"; }
/* a join's scan is over: choose (wifi/fmac.c fm_pick_choose, docs/08 item 147) and join */
static void choose_and_join(uint32_t now)
{
    unsigned sl = strlen(W.ssid);
    int why, best, m = 0;
    for (int i = 0; i < W.nscan; i++) if (W.scan[i].ssid_len == sl && !mcmp(W.scan[i].ssid, W.ssid, sl)) m++;
    scan_log_aps(now);
    int k = fm_pick_choose(&W.pick, W.scan, W.nscan, W.ssid, sl, now, &why, &best);
    if (k == FM_PICK_RESCAN) {
        const struct fm_seen *bs = fm_pick_better_seen(&W.pick, W.scan[best].bssid, W.pick.minrssi - 1, now);
        wifi_log("the best heard now is %s at %d dBm, under %d dBm, but %s was %d dBm %u s ago: scanning again\n", b3(W.scan[best].bssid), W.scan[best].rssi,
                 W.pick.minrssi, bs ? b3(bs->bssid) : "?", bs ? bs->rssi : 0, bs ? (now - bs->at) / 1000u : 0u);
        W.scan_for_join = 1;
        scan_start(0);
        wstate(AN_ST_SCANNING, 0);
        return;
    }
    if (k >= 0) {
        const struct fm_bss *b = &W.scan[k];
        if (why == FM_WHY_SKIPPED_KICKED) {
            const struct fm_kick *kk = fm_pick_kicked(&W.pick, W.scan[best].bssid, now);
            wifi_log("choosing %s: channel %u, %d dBm (%s at %d dBm %s: skipped %u s more)\n", b3(b->bssid), b->channel, b->rssi, b3(W.scan[best].bssid), W.scan[best].rssi,
                     kk ? kick_word(kk->why) : "?", kk ? (kk->until - now) / 1000u : 0u);
        } else if (why == FM_WHY_KICKED_ANYWAY) {
            const struct fm_kick *kk = fm_pick_kicked(&W.pick, b->bssid, now);
            wifi_log("choosing %s: channel %u, %d dBm, though it %s: no other within %u dB\n", b3(b->bssid), b->channel, b->rssi, kk ? kick_word(kk->why) : "?", FM_KICK_DB);
        } else
            wifi_log("choosing %s: channel %u, %d dBm (the strongest of %d%s)\n", b3(b->bssid), b->channel, b->rssi, m, why == FM_WHY_WEAK_ONLY ? ", weak: nothing better heard in a minute" : "");
        join(b, 1);
        return;
    }
    /* by name only: the firmware's own choice */
    if (why == FM_WHY_NOT_SEEN) wifi_log("\"%s\" not seen in the scan: joining by name only\n", W.ssid);
    else if (why == FM_WHY_FALLBACK) wifi_log("%d joins with an access point named failed in a row: by name only, the firmware chooses\n", W.pick.pinned_fails);
    else wifi_log("only access points under %d dBm heard, twice more, while a better one was seen: by name only, the firmware chooses\n", W.pick.minrssi);
    join(best >= 0 ? &W.scan[best] : 0, 0);
}
static void conn_tick(void)
{
    uint32_t now = now_ms();
    if (!W.chip_up) return;
    if (W.scan_busy && (W.scan_done || (int32_t)(now - W.scan_t0) > 10000)) {
        int32_t st = W.scan_done ? W.scan_status : -1;
        unsigned sl = strlen(W.ssid);
        int m = 0;
        for (int i = 0; i < W.nscan; i++) if (sl && W.scan[i].ssid_len == sl && !mcmp(W.scan[i].ssid, W.ssid, sl)) m++;
        /* the escan's end, said: until 25 November 2026 an aborted or cut-short scan looked like a finished one */
        char sw[48]; unsigned q = 0;
        const char *w = st >= 0 ? fm_status_word((uint32_t)st) : st == -1 ? "no end in 10 s" : "refused";
        while (*w && q < 30) sw[q++] = *w++;
        if (st > 0) { const char *x = " (status "; while (*x) sw[q++] = *x++; if (st >= 10) sw[q++] = (char)('0' + st / 10 % 10); sw[q++] = (char)('0' + st % 10); sw[q++] = ')'; }
        sw[q] = 0;
        if (sl && (W.scan_for_join || W.want)) wifi_log("scan: %d networks, %d of them \"%s\" (%s, %u ms%s)\n", W.nscan, m, W.ssid, sw, now - W.scan_t0, W.scan_pass ? ", second pass" : "");
        else wifi_log("scan: %d networks (%s, %u ms%s)\n", W.nscan, sw, now - W.scan_t0, W.scan_pass ? ", second pass" : "");
        /* one more pass, merged, when the scan did not end cleanly or a join's scan heard fewer than three of the SSID */
        if (!W.scan_pass && (st != E_STATUS_SUCCESS || (W.scan_for_join && W.want && m < 3))) {
            wifi_log("scanning once more, 80 ms on each channel (%s)\n", st != E_STATUS_SUCCESS ? "the scan did not end cleanly" : "fewer than three access points of the network heard");
            scan_start(1);
            return;
        }
        W.scan_busy = 0;
        scan_publish();
        if (W.scan_cmd) { W.scan_cmd = 0; cmd_finish(W.scan_cmd_seq, 0); }
        if (W.scan_for_join) {
            W.scan_for_join = 0;
            if (!W.want) { wifi_log("scan done; no longer asked to join\n"); }
            else choose_and_join(now);
        }
    }
#ifdef PSMAC
    if (W.ap_want) return;                          /* an access point: no station to keep connected (scans above still finish) */
#endif
    /* join() above takes some time (the key, several commands) and starts the
       join clock: the times below must be after it, or a join that has just begun
       looks 49 days old and is given up at once (the second test on a Pi) */
    now = now_ms();
    unsigned st = U(0)->state;
    switch (st) {
    case AN_ST_IDLE:
        if (W.want && W.ssid[0] && !W.scan_busy) { W.scan_for_join = 1; scan_start(0); wstate(AN_ST_SCANNING, 0); }
        break;
    case AN_ST_RETRY:
        if ((int32_t)(now - W.retry_at) >= 0 && !W.scan_busy && W.want) {
            /* after a lost link the firmware may still be trying to get back on its own; its scans cut ours short
               (docs/08 item 147): stopped first */
            if (W.need_disassoc) { W.need_disassoc = 0; cmd_int(WLC_DISASSOC, 0); }
            W.scan_for_join = 1; scan_start(0); wstate(AN_ST_SCANNING, 0);
        }
        break;
    case AN_ST_JOINING:
        if (W.ev_setssid_fail) { join_failed("could not join the network"); break; }
        if (W.ev_link_down && (int32_t)(now - W.join_t0) < 1000) W.ev_link_down = 0;
        if (W.ev_link_down) { join_failed(W.ev_assoc_ok && !W.keyed ? "the WPA handshake failed: is the password right?" : "the network refused us"); break; }
        if (W.ev_keyed) W.keyed = 1;
        if (W.ev_link_up && W.keyed) {
            fm_pick_result(&W.pick, W.pinned, 1);
            W.connected_at = now;                   /* the backoff is cleared once the link has lasted 30 s */
            W.ev_roamed = 0;
            connect_info();
            W.info_at = now + 5000;
            char m[96]; unsigned i = 0;
            for (const char *p = "connected to "; *p; ) m[i++] = *p++;
            for (const char *p = W.ssid; *p && i < 90; ) m[i++] = *p++;
            m[i] = 0;
            link_set(1);
            wstate(AN_ST_CONNECTED, m);
            wifi_log("%s, channel %u, %d dBm, %d Mbit/s\n", b3(W.cur.bssid), W.cur.channel, W.rssi, W.rate);
#ifdef PSMAC
            /* The power-save mode is set before the join, but the firmware may take up its own on
               associating (Linux sets it again after every connect). With PM_FAST the radio dozes
               between beacons once the link has been idle a moment, and the access point keeps a
               reply until the next beacon: tens of milliseconds on every ping a second apart, the
               80 ms the user's Pi 3B showed on 19 November. Read it back and put it right. */
            {
                uint32_t pm = 99, want = (U(0)->cfg_flags & AN_WC_POWERSAVE) ? 2 : 0;
                cmd_get_int(85 /* WLC_GET_PM */, &pm);
                if (pm != want)
                    cmd_int(WLC_SET_PM, want);
                wifi_log("power save: PM %u after joining%s\n", pm, pm != want ? (want ? ", set to 2" : ", set to 0 (off)") : " (as wanted)");
            }
#endif
        } else if ((int32_t)(now - W.join_t0) > 20000)
            join_failed(W.ev_link_up || W.ev_assoc_ok ? "associated, but the WPA handshake did not finish: is the password right?" : "the network did not answer");
        break;
    case AN_ST_CONNECTED:
        if (W.ev_link_down) {
            /* lost: that access point skipped for a while where another is near, the next one at once (docs/08 item 147) */
            W.ev_link_down = 0;
            uint32_t up = now - W.connected_at;
            fm_pick_kick(&W.pick, W.cur.bssid, now, FM_KICK_MS, W.drop_by_ap ? FM_KICK_BY_AP : FM_KICK_LOST);
            int fw = W.fw_attempt_at && now - W.fw_attempt_at < 5000u;
            wifi_log("lost %s after %u s (%s%s): skipped for %u s where another is within %u dB\n", b3(W.cur.bssid), up / 1000u,
                     W.drop_by_ap ? "the access point disconnected us" : W.drop_ev ? "our side left it" : "the link went down",
                     fw ? ", just after a join or roam attempt of the firmware's own" : "", FM_KICK_MS / 1000u, FM_KICK_DB);
            if (fw && W.roam_mode == 0 && !W.roam_off_now) {
                if (!W.fw_drops || now - W.fw_drop_t0 > 600000u) { W.fw_drops = 0; W.fw_drop_t0 = now; }
                if (++W.fw_drops >= 2) {
                    W.roam_off_now = 1;
                    wifi_log("firmware roaming off for this session: twice in %u s its own attempt came just before the link was lost (wifi.roam=on keeps it)\n", (now - W.fw_drop_t0) / 1000u);
                }
            }
            W.drop_by_ap = W.drop_ev = 0;
            W.fw_attempt_at = 0;
            W.need_disassoc = 1;
            W.quick = up >= 30000u ? 3 : 1;         /* a link that lasted: three quick tries; one that did not: one */
            retry("connection lost");
            break;
        }
        if (W.backoff && now - W.connected_at > 30000u) W.backoff = 0;
        if (W.ev_roamed || (int32_t)(now - W.info_at) >= 0) {
            uint8_t was[6];
            memcpy(was, W.cur.bssid, 6);
            int roamed = W.ev_roamed;
            W.ev_roamed = 0;
            W.info_at = now + 5000;
            if (connect_info()) {
                /* the firmware moved to another access point of the network: the link stays up */
                wifi_log("roamed to %s: channel %u, %d dBm (from %s%s); the link stays up\n", b3(W.cur.bssid), W.cur.channel, W.rssi, b3(was), roamed ? "" : ", seen in the link's figures");
                U(0)->state_seq++;
                an_irq();
            }
            U(0)->speed_mbps = W.rate ? (unsigned)W.rate : 54u;
        }
        break;
    default:
        break;
    }
}

#ifdef PSMAC
/* ---- the access point (Aurora68, 23 November 2026, docs/08 item 118) ------------------------------
   Setup and Recovery can make the radio an access point of its own, for a phone to reach the recovery page
   where there is no other network. The firmware's SoftAP is set up with the sequence wifi/fmac_ap.c builds
   (Linux brcmfmac's order); each step is logged, and the first one the firmware refuses is named in the
   state message ("the WiFi chip refused AP mode: WLC_SET_AP error -23"), which the recovery screen shows,
   so that the first try on a real Pi says what to change. Nothing of this could be tried on a real chip
   when it was written: QEMU has none. WPA2 needs the firmware to be the authenticator (the 4-way handshake
   runs in it, as with Linux's 4WAY_HANDSHAKE_AP_PSK), which brcmfmac takes "idauth" in the "cap" list to
   mean; without it the network is open, and the recovery page's PIN is what guards it. */
static void wstatef(unsigned st, const char *fmt, ...)
{
    struct lbuf b; b.n = 0;
    va_list ap; va_start(ap, fmt); vkprintf_pc(lb_c, &b, fmt, ap); va_end(ap);
    b.s[b.n < 95 ? b.n : 95] = 0;
    wstate(st, b.s);
}
static void ap_msg(void)
{
    wstatef(AN_ST_CONNECTED, "access point %s up: channel %u, %s, %d device%s joined", W.ap_ssid, W.ap_channel,
            W.ap_wpa2 ? "WPA2" : "open", W.ap_stations, W.ap_stations == 1 ? "" : "s");
}
static void ap_event(const struct fm_event *e)
{
    switch (e->type) {
    case E_LINK: W.ev_ap_link = (e->flags & E_FLAG_LINK) ? 1 : -1; break;
    case E_SET_SSID: W.ev_ap_ssid = (int)e->status + 1; break;
    case E_ASSOC_IND: case E_REASSOC_IND:
        if (e->status == E_STATUS_SUCCESS) { W.ap_stations++; if (W.ap_up) ap_msg(); }
        break;
    case E_DISASSOC_IND: case E_DEAUTH_IND: case E_DEAUTH:
        if (W.ap_stations > 0) W.ap_stations--;
        if (W.ap_up) ap_msg();
        break;
    default: break;
    }
    /* the first events in full: what a real firmware sends as an access point is not known yet (no addresses: they are the guests') */
    if (W.ap_events < 60) { W.ap_events++; wifi_log("access point: firmware event %u, status %u, reason %u, flags %u\n", e->type, e->status, e->reason, e->flags); }
}
static int32_t ap_step_send(void *ctx, const struct fm_ap_step *s)
{
    static uint8_t b[FMS_DATA_MAX];
    (void)ctx;
    switch (s->kind) {
    case FMS_CMD_INT: return cmd_int(s->cmd, s->val);
    case FMS_IOV_INT: return iov_int(s->name, s->val);
    case FMS_CMD_DATA: { memcpy(b, s->data, s->len); int32_t e = ctl(s->cmd, 1, b, s->len, 0); memset(b, 0, sizeof b); return e; }
    default: return iov_set(s->name, s->data, s->len);
    }
}
static void ap_step_log(void *ctx, const struct fm_ap_step *s, int32_t e, int skipped)
{
    char nm[40];
    (void)ctx;
    if (skipped) return;
    fm_ap_step_name(s, nm, sizeof nm);
    int num = s->kind == FMS_CMD_INT || s->kind == FMS_IOV_INT;     /* data steps (the key) are named, never shown */
    if (!e) wifi_log("access point: %s %u%s ok (%s)\n", nm, num ? s->val : s->len, num ? "" : " bytes", s->what);
    else wifi_log("access point: %s %u%s REFUSED, error %d (%s)%s\n", nm, num ? s->val : s->len, num ? "" : " bytes", e, s->what,
                  (s->flags & FMS_FATAL) ? "" : ": carrying on, as Linux does");
}
static int ap_start(void)
{
    volatile struct an_unit *u = U(0);
    W.want = 0; W.scan_for_join = 0; W.ap_up = 0; W.ap_stations = 0; W.ev_ap_link = 0; W.ev_ap_ssid = 0; W.ap_events = 0;
    link_set(0);
    if (!W.chip_up) return -1;
    int idauth = fm_caps_has(W.caps, "idauth");
    W.ap_wpa2 = W.ap_pass[0] && (idauth || (W.ap_flags & AN_AP_WPA2_ANYWAY));
    if (W.ap_pass[0] && !W.ap_wpa2)
        wifi_log("access point: the firmware does not list \"idauth\" (it would be the WPA2 authenticator): the network is open, and the recovery page's PIN guards it\n");
    else if (W.ap_wpa2 && !idauth) wifi_log("access point: WPA2 asked for although the firmware does not list \"idauth\" (recovery.apsec=wpa2): phones may not finish joining\n");
    wstatef(AN_ST_JOINING, "starting the access point %s", W.ap_ssid);
    uint8_t pmk[32];
    if (W.ap_wpa2) {
        uint32_t t0 = now_ms();
        fm_wpa_pmk(W.ap_pass, strlen(W.ap_pass), W.ap_ssid, strlen(W.ap_ssid), pmk, wifi_relax);
        wifi_log("access point: the WPA2 key from the password, %u ms\n", now_ms() - t0);
    }
    struct fm_ap_cfg c = { W.ap_ssid, W.ap_channel, W.iover == 1, W.ap_wpa2 ? pmk : 0, W.ap_wpa2 ? W.ap_pass : 0, W.country[0] ? W.country : 0 };
    static struct fm_ap_step steps[FM_AP_STEPS_MAX];
    unsigned n = fm_ap_steps(steps, FM_AP_STEPS_MAX, &c);
    memset(pmk, 0, sizeof pmk);
    if (!n) { wstatef(AN_ST_FAILED, "the access point's name or channel is not usable"); return -2; }
    wifi_log("access point %s, channel %u (chanspec 0x%x, firmware interface version %u), %s: %u firmware commands\n", W.ap_ssid, W.ap_channel,
             fm_chanspec_2g(W.ap_channel, W.iover == 1), W.iover, W.ap_wpa2 ? "WPA2-PSK, AES" : "open", n);
    char why[64];
    int32_t e = fm_ap_run(steps, n, ap_step_send, ap_step_log, 0, why, sizeof why);
    memset(steps, 0, sizeof steps);                 /* the key's bytes go */
    if (e) { wstatef(AN_ST_FAILED, "the WiFi chip refused AP mode: %s", why); return -5; }
    /* the firmware says the network is up with E_LINK (Linux waits for it with "mbss"), or with E_SET_SSID's status */
    uint32_t t0 = now_ms();
    while (!W.ev_ap_link && !W.ev_ap_ssid && now_ms() - t0 < 3000) { poll_frames(); an_yield(); }
    if (W.ev_ap_ssid > 1) { wstatef(AN_ST_FAILED, "the WiFi chip refused AP mode: WLC_SET_SSID's event, status %d", W.ev_ap_ssid - 1); return -6; }
    if (W.ev_ap_link < 0) { wstatef(AN_ST_FAILED, "the WiFi chip refused AP mode: the firmware said link down at once"); return -7; }
    wifi_log("access point: %s\n", W.ev_ap_link > 0 ? "the firmware says the network is up (E_LINK)" : W.ev_ap_ssid ? "the firmware took the SSID (E_SET_SSID, success)"
             : "no event from the firmware within 3 s: taken as up (a real chip's first try will tell)");
    unsigned i = 0;
    for (; W.ap_ssid[i] && i < 35; i++) u->ssid[i] = W.ap_ssid[i];
    u->ssid[i] = 0;
    for (i = 0; i < 6; i++) u->bssid[i] = W.mac[i];
    u->channel = W.ap_channel; u->security = W.ap_wpa2 ? SEC_WPA2 : SEC_OPEN; u->rssi = 0;
    W.ap_up = 1;
    link_set(1);
    ap_msg();
    return 0;
}
static void ap_stop(void)
{
    if (!W.ap_want) return;
    W.ap_want = 0; W.ap_up = 0;
    link_set(0);
    if (!W.chip_up) return;
    cmd_int(WLC_DOWN, 1);
    cmd_int(WLC_SET_AP, 0);
    cmd_int(WLC_SET_INFRA, 1);
    cmd_int(WLC_UP, 1);
    wifi_log("access point stopped: a station again\n");
}
#endif

/* ---- the Amiga's commands ---- */
/* the user's off switch: the chip goes off at the next turn of wifi_main's loop and stays off
   (start_req is ignored while off_req is set); what was waiting to be sent is dropped */
static void wifi_switch_off(volatile struct an_unit *u)
{
    u->admin_off = 1;
    W.off_req = 1;
    W.want = 0;
    W.scan_for_join = 0;
    link_set(0);
    while (u->tx_tail != u->tx_head) { u->tx_tail++; u->tx_err++; }
    wifi_log("switched off\n");
}

static void commands(void)
{
    volatile struct an_unit *u = U(0);
    uint32_t seq = u->cmd_seq;
    if (seq == u->cmd_done || (W.scan_cmd && seq == W.scan_cmd_seq)) return;
    __asm__ volatile("dmb ish" ::: "memory");
    uint32_t c = u->cmd;
    switch (c) {
    case AN_CMD_START:
        u->admin_off = 0;
        W.start_req = 1; W.off_req = 0; W.restarts = 0;
        cmd_finish(seq, 0);
        break;
    case AN_CMD_ENABLE:
        if (arg32(u, 0)) {
            u->admin_off = 0;
            W.start_req = 1; W.off_req = 0; W.restarts = 0;
            W.want = W.ssid[0] != 0; W.backoff = 0;    /* back on the network it was on */
            wifi_log("switched on\n");
        } else wifi_switch_off(u);
        u->state_seq++;
        cmd_finish(seq, 0);
        break;
    case AN_CMD_JOIN: {
        volatile struct an_join *j = (volatile struct an_join *)u->arg;
        unsigned i;
#ifdef PSMAC
        ap_stop();                                  /* a station again */
#endif
        if (j->ssid[0]) {                           /* no name: the last network again */
            for (i = 0; i < 32 && j->ssid[i]; i++) W.ssid[i] = j->ssid[i];
            W.ssid[i] = 0;
            for (i = 0; i < 64 && j->pass[i]; i++) W.pass[i] = j->pass[i];
            W.pass[i] = 0;
        }
        if (!W.ssid[0]) { cmd_finish(seq, -2); break; }
        if (j->country[0]) { W.country[0] = j->country[0]; W.country[1] = j->country[1]; W.country[2] = 0; }
        W.want = 1; W.backoff = 0; W.start_req = 1; W.off_req = 0;
        u->admin_off = 0;                           /* joining is asking for it on */
        W.scan_for_join = 0;
        if (W.chip_up) {
            if (u->state == AN_ST_CONNECTED || u->state == AN_ST_JOINING) cmd_int(WLC_DISASSOC, 0);
            link_set(0);
            if (W.country[0] && W.country[1]) { uint8_t cc[12]; fm_country(cc, W.country); iov_set("country", cc, 12); }
            wstate(AN_ST_IDLE, "");
        }
        cmd_finish(seq, 0);
        break;
    }
    case AN_CMD_SCAN:
        if (!W.chip_up) { cmd_finish(seq, -1); break; }
        W.scan_cmd = 1; W.scan_cmd_seq = seq;
        if (!W.scan_busy) scan_start(0);
        break;
    case AN_CMD_DISCONNECT:
#ifdef PSMAC
        ap_stop();
#endif
        W.want = 0;
        W.scan_for_join = 0;
        if (W.chip_up) cmd_int(WLC_DISASSOC, 0);
        link_set(0);
        if (W.chip_up) wstate(AN_ST_IDLE, "disconnected");
        cmd_finish(seq, 0);
        break;
    case AN_CMD_OFF:
        wifi_switch_off(u);
        cmd_finish(seq, 0);
        break;
    case AN_CMD_RXON:                               /* the Amiga skips what is in the slots itself */
        u->rx_on = 1;
        cmd_finish(seq, 0);
        break;
    case AN_CMD_RXOFF:
        u->rx_on = 0;
        cmd_finish(seq, 0);
        break;
    case AN_CMD_WIFICFG:
        u->cfg_flags = arg32(u, 4) & AN_WC_POWERSAVE;
        if (W.chip_up) cmd_int(WLC_SET_PM, (u->cfg_flags & AN_WC_POWERSAVE) ? 2 : 0);
        wifi_log("power saving %s\n", (u->cfg_flags & AN_WC_POWERSAVE) ? "on" : "off");
        u->state_seq++;
        cmd_finish(seq, 0);
        break;
    case AN_CMD_SETMAC: {
        uint8_t m[6]; int any = 0;
        for (int i = 0; i < 6; i++) { m[i] = u->arg[i]; any |= m[i]; }
        if (any && (m[0] & 1)) { cmd_finish(seq, -2); break; }
        memcpy(W.want_mac, m, 6);
        W.want_oui = u->arg[6] == 1;
        int r = 0;
        if (W.chip_up) {
            /* the firmware takes a new address only while down; the link drops, and the
               connection manager joins again */
            uint8_t a[6];
            mac_wanted(W.want_mac, W.want_oui, W.fmac, a);
            cmd_int(WLC_DOWN, 0);
            r = iov_set("cur_etheraddr", a, 6) ? -5 : 0;
            cmd_int(WLC_UP, 0);
            if (!r) {
                memcpy(W.mac, a, 6);
                for (int i = 0; i < 6; i++) u->mac[i] = W.mac[i];
                wifi_log("MAC address now %02x:%02x:%02x:%02x:%02x:%02x%s\n", W.mac[0], W.mac[1], W.mac[2], W.mac[3], W.mac[4], W.mac[5], any ? "" : " (the chip's own)");
            } else wifi_log("the firmware would not take the MAC address\n");
            u->state_seq++;
        }
        cmd_finish(seq, r);
        break;
    }
    case AN_CMD_FILTER: {
        uint32_t f = (uint32_t)u->arg[0] << 24 | (uint32_t)u->arg[1] << 16 | (uint32_t)u->arg[2] << 8 | u->arg[3];
        wifi_filter_want = f; wifi_filter_set = 1;     /* kept for a chip that comes up later (wifi_filter_apply) */
        if (W.chip_up) wifi_filter_apply();
        cmd_finish(seq, 0);
        break;
    }
#ifdef PSMAC
    case AN_CMD_AP: {
        volatile struct an_ap *a = (volatile struct an_ap *)u->arg;
        unsigned i, pl;
        for (i = 0; i < 32 && a->ssid[i]; i++) W.ap_ssid[i] = a->ssid[i];
        W.ap_ssid[i] = 0;
        for (pl = 0; pl < 63 && a->pass[pl]; pl++) W.ap_pass[pl] = a->pass[pl];
        W.ap_pass[pl] = 0;
        if (a->country[0] && a->country[1]) { W.country[0] = a->country[0]; W.country[1] = a->country[1]; W.country[2] = 0; }
        W.ap_channel = a->channel; W.ap_flags = a->flags;
        for (i = 0; i < sizeof u->arg; i++) u->arg[i] = 0;          /* the password does not stay on the board */
        if (!W.ap_ssid[0] || W.ap_channel < 1 || W.ap_channel > 13 || (pl && pl < 8)) { cmd_finish(seq, -2); break; }
        W.ap_want = 1; W.want = 0; W.scan_for_join = 0;
        W.start_req = 1; W.off_req = 0; u->admin_off = 0;
        if (W.chip_up) {
            if (u->state == AN_ST_CONNECTED || u->state == AN_ST_JOINING) cmd_int(WLC_DISASSOC, 0);
            cmd_finish(seq, ap_start());
        } else {
            wifi_log("access point asked for: it starts once the chip is up\n");
            cmd_finish(seq, 0);
        }
        break;
    }
#endif
    default:
        cmd_finish(seq, -3);
        break;
    }
}

static void wifi_thread(void)
{
    volatile struct an_unit *u = U(0);
    u->kind = AN_KIND_WIFI;
    u->cfg_flags = 0;                               /* no power saving: it costs latency */
    W.want_oui = oui_default(W.want_mac);
    W.sdclock = 25000000u;
    char v[80];
    if (bootarg_val("wifi.sdclock", v, sizeof v)) { uint32_t n = 0; for (char *p = v; *p >= '0' && *p <= '9'; p++) n = n * 10 + (uint32_t)(*p - '0'); if (n >= 400000 && n <= 50000000) W.sdclock = n; }
    if (bootarg_val("wifi.ssid", v, sizeof v)) { unsigned i = 0; for (; v[i] && i < 32; i++) W.ssid[i] = v[i]; W.ssid[i] = 0; W.want = W.ssid[0] != 0; }
    if (bootarg_val("wifi.psk", v, sizeof v)) { unsigned i = 0; for (; v[i] && i < 64; i++) W.pass[i] = v[i]; W.pass[i] = 0; }
    /* wifi.roam=auto|on|off and wifi.minrssi=DBM (docs/08 item 147): the firmware's own roaming, and the weakest access
       point chosen while a better one was seen in the last minute (-80 dBm) */
    W.roam_mode = 0;
    if (bootarg_val("wifi.roam", v, sizeof v)) W.roam_mode = ieq(v, "on") ? 1 : ieq(v, "off") ? 2 : 0;
    {
        int mr = -80;
        if (bootarg_val("wifi.minrssi", v, sizeof v)) {
            int n = 0; const char *q = v[0] == '-' ? v + 1 : v;
            for (; *q >= '0' && *q <= '9'; q++) n = n * 10 + (*q - '0');
            if (n >= 40 && n <= 100) mr = -n;
        }
        fm_pick_init(&W.pick, mr);
    }
#ifdef PSMAC
    /* Two letters, A to Z (ISO 3166). "XX" is the placeholder of the instructions, not a country: the
       user's SE had it on 21 November 2026. It never reached the chip (the WiFi stopped earlier), but
       the firmware would have been handed a code it does not know; now it keeps its own default. */
    if (bootarg_val("wifi.country", v, sizeof v)) {
        char c0 = v[0] >= 'a' && v[0] <= 'z' ? (char)(v[0] - 32) : v[0], c1 = v[1] >= 'a' && v[1] <= 'z' ? (char)(v[1] - 32) : v[1];
        if (c0 < 'A' || c0 > 'Z' || c1 < 'A' || c1 > 'Z' || v[2])
            wifi_log("wifi.country=%s is not a two-letter country code: the firmware's default is kept\n", v);
        else if (c0 == 'X' && c1 == 'X')
            wifi_log("wifi.country=XX is the instructions' placeholder: put your own country's two letters (GB, DE, US ...); the firmware's default is kept\n");
        else { W.country[0] = c0; W.country[1] = c1; W.country[2] = 0; }
    }
#else
    if (bootarg_val("wifi.country", v, sizeof v)) { W.country[0] = v[0]; W.country[1] = v[1]; W.country[2] = 0; }
#endif
    W.start_req = 1;                                /* as soon as the firmware is there */
#ifdef PSMAC
    W.sim = psmac_wfw_on();
    if (W.sim) kprintf("[WIFI] mac.wifisimfw=1: the real driver above a simulated firmware (testbed)\n");
    /* the container was read at boot (psmac_bus.c), before this thread starts: with no WiFi
       firmware in it nothing will ever arrive, so say once what to add */
    if (!W.sim && !fw_have_wifi())
        kprintf("[WIFI] the container has no WiFi firmware: add cyfmac43430-sdio.bin, brcmfmac43430-sdio.txt and cyfmac43430-sdio.clm_blob (Pi 3B) with make-container.py\n");
#endif
    wstate(AN_ST_NOFW, FW_WAITING);
    uint32_t seen_gen = 0;
    for (;;) {
        /* off, or no firmware yet: commands only */
#ifdef PSMAC
        while (!(W.start_req && !W.off_req && (nfw || W.sim))) {
#else
        while (!(W.start_req && !W.off_req && nfw)) {
#endif
            if (seen_gen != fw_gen) { seen_gen = fw_gen; if (u->state == AN_ST_NOFW || u->state == AN_ST_FAILED) W.start_req = 1; continue; }
            commands();
            if (W.off_req && u->state != AN_ST_OFF) { chip_off(); wstate(AN_ST_OFF, "switched off"); }
            an_yield();
        }
        W.start_req = 0;
        seen_gen = fw_gen;
        if (bringup()) {
            chip_off();
            continue;                               /* START (or new firmware) tries again */
        }
        W.lost = 0;
#ifdef PSMAC
        if (W.ap_want) ap_start();                  /* asked for before the chip was up, or the chip was lost and is back */
#endif
        while (W.chip_up && !W.off_req) {
            /* Another driver in AmigaOS (WiFiPi.device, say) resetting the same SD host
               shows as changed bus or clock settings: say so, and take the chip back. */
#ifdef PSMAC
            uint32_t c0 = W.sim ? W.host_c0 : __builtin_bswap32(W.h.regs[0x28 / 4]) & 0x06u, c1 = W.sim ? W.host_c1 : __builtin_bswap32(W.h.regs[0x2c / 4]) & 0xffe5u;
#else
            uint32_t c0 = __builtin_bswap32(W.h.regs[0x28 / 4]) & 0x06u, c1 = __builtin_bswap32(W.h.regs[0x2c / 4]) & 0xffe5u;
#endif
            if (W.h.sd_hz != W.host_hz) { W.host_hz = W.h.sd_hz; W.host_c1 = c1; }    /* our own slow-down after errors */
            if (c0 != W.host_c0 || c1 != W.host_c1) {
                wifi_log("the WiFi controller was reset by someone else (CONTROL0 %x, CONTROL1 %x)\n", c0, c1);
                wifi_log("is another WiFi driver (WiFiPi.device?) started in AmigaOS? The kernel drives the WiFi now: remove it\n");
                W.lost = "another driver took the WiFi controller (WiFiPi.device?) - remove it from AmigaOS";
                W.chip_up = 0;
                break;
            }
            if (u->state == AN_ST_CONNECTED) W.restarts = 0;
            commands();
            uint32_t now = now_ms();
            int busy = now - W.traffic_ms < 300 || u->state == AN_ST_JOINING || u->state == AN_ST_SCANNING || W.scan_busy || u->tx_tail != u->tx_head;
#ifdef PSMAC
            if ((int32_t)(now - W.next_poll) >= 0 || u->tx_tail != u->tx_head || psmac_ip_tx_pending(0)) {
#else
            if ((int32_t)(now - W.next_poll) >= 0 || u->tx_tail != u->tx_head) {
#endif
                W.next_poll = now + (busy ? 1 : 10);
                poll_frames();
                if (W.chip_up) tx_all();
            }
            conn_tick();
            an_yield();
        }
        if (W.off_req) { chip_off(); wstate(AN_ST_OFF, "switched off"); continue; }
        /* the chip was lost: off, and on again in 5 s (5 times at most in a row) */
        link_set(0);
        W.chip_up = 0;
        chip_off();
        if (W.restarts < 5) {
            W.restarts++;
            char m[96]; unsigned i = 0;
            for (const char *p = W.lost ? W.lost : "the WiFi chip was lost"; *p && i < 70; ) m[i++] = *p++;
            for (const char *p = ": again in 5 s"; *p && i < 95; ) m[i++] = *p++;
            m[i] = 0;
            wstate(AN_ST_RETRY, m);
            W.restart_at = now_ms() + 5000;
            while ((int32_t)(now_ms() - W.restart_at) < 0 && !W.off_req) { commands(); an_yield(); }
            if (!W.off_req) W.start_req = 1;
        } else
            wstate(AN_ST_FAILED, W.lost ? W.lost : "the WiFi chip was lost 5 times: Alice68-WiFi START tries again");
    }
}

/* The kernel owns the WiFi's SD host: its device-tree node (reg 0x7e300000
   under /soc) is marked disabled, as for hardware the OS does not own, so a
   driver in AmigaOS that reads the device tree leaves it alone. */
static void claim_host_in_dt(void)
{
    of_node_t *soc = dt_find_node("/soc");
    for (of_node_t *n = soc ? soc->on_children : 0; n; n = n->on_next) {
        of_property_t *reg = dt_find_property(n, "reg");
        if (!reg || reg->op_length < 4 || *(uint32_t *)reg->op_value != 0x7e300000u) continue;
        static char disabled[] = "disabled";
        of_property_t *st = dt_find_property(n, "status");
        if (st) { st->op_value = disabled; st->op_length = sizeof disabled; }
        else dt_add_property(n, "status", disabled, sizeof disabled);
        wifi_log("%s: marked disabled for AmigaOS - the kernel drives the WiFi (wifi=off gives it back)\n", n->on_name);
    }
}


/* ==== Gigabit Ethernet: GENET v5 and its BCM54213PE PHY (unit 1) ==================
   A polled driver, a state machine that never waits: CPU1 runs it every turn
   beside USB and the WiFi. Register layout and start-up order follow U-Boot's
   bcmgenet.c (GPL-2.0+, itself from Linux's bcmgenet); the PHY's RGMII delay
   settings follow Linux's Broadcom PHY driver (GPL-2.0). Descriptors live in
   GENET's registers; the frame buffers in uncached memory, so no cache work. */
extern void frf_post_eth(const char *text, int col);
#define G_SYS_REV       0x0000
#define G_PORT_CTRL     0x0004
#define G_RBUF_FLUSH    0x0008
#define G_RGMII_OOB     0x008c
#define G_RBUF_CTRL     0x0300
#define G_RBUF_TBUF_SZ  0x03b4
#define G_CMD           0x0808
#define G_MAC0          0x080c
#define G_MAC1          0x0810
#define G_MAX_FRAME     0x0814
#define G_TX_FLUSH      0x0b34
#define G_MIB_CTRL      0x0d80
#define G_MDIO          0x0e14
#define G_RXD           0x2000          /* 256 descriptors * 12 bytes */
#define G_TXD           0x4000
#define G_RR            0x3000          /* RDMA ring 16 (the default queue) */
#define G_TR            0x5000          /* TDMA ring 16 */
#define G_RDMA          0x3040          /* RDMA control */
#define G_TDMA          0x5040
#define CMD_TX_EN       (1u << 0)
#define CMD_RX_EN       (1u << 1)
#define CMD_PROMISC     (1u << 4)
#define CMD_HD_EN       (1u << 10)
#define CMD_SW_RESET    (1u << 13)
#define CMD_LCL_LOOP    (1u << 15)
#define DMA_EN          1u
#define DESC_SOP        0x2000u
#define DESC_EOP        0x4000u
#define DESC_CRC        0x0040u
#define NRX             256
#define NTXB            64
#define GBUF            2048

static struct {
    volatile uint8_t *regs;
    uint8_t *rxb, *txb;                  /* uncached */
    uint64_t rxp, txp;                   /* their physical addresses */
    uint32_t state, t0, next_link, cidx, tprod, cmd_base, rx_err, rx_waits;
    int      phy, up, speed, full, started, promisc, disabled;
    uint8_t  mac[6], fmac[6], want_mac[6];   /* in use; the Pi's own; asked for (zeros: the Pi's own) */
    int      want_oui;                       /* want_mac: only its OUI */
} E;
enum { E_OFF, E_PHYRESET, E_LINKWAIT, E_RUN, E_FAILED };

/* the remote control's own MAC address needs GENET to pass it too: GENET has one address register */
#ifdef PSMAC
#define GENET_PROMISC() (E.promisc || psmac_ip_promisc(1))
#else
#define GENET_PROMISC() (E.promisc)
#endif
static inline uint32_t grd(uint32_t o) { return __builtin_bswap32(*(volatile uint32_t *)(E.regs + o)); }
static inline void gwr(uint32_t o, uint32_t v) { *(volatile uint32_t *)(E.regs + o) = __builtin_bswap32(v); }
static void gdelay(uint32_t us) { uint64_t end = ticks() + tick_hz() * us / 1000000u; while ((int64_t)(end - ticks()) > 0) ; }

static void eth_log(const char *fmt, ...)
{
    struct lbuf b; b.n = 0;
    for (const char *p = "[ETH] "; *p; ) b.s[b.n++] = *p++;
    va_list ap; va_start(ap, fmt); vkprintf_pc(lb_c, &b, fmt, ap); va_end(ap);
    if (!b.n || b.s[b.n - 1] != '\n') b.s[b.n++] = '\n';
    b.s[b.n] = 0;
    kprintf("%s", b.s);
    ring_put(b.s, b.n);
}
static void estate(unsigned st, const char *msg)
{
    volatile struct an_unit *u = U(1);
    unsigned i = 0;
    for (; msg[i] && i < 95; i++) u->msg[i] = msg[i];
    u->msg[i] = 0;
    u->state = st;
    u->state_seq++;
    an_irq();
    eth_log("%s\n", msg);
    char t[48]; unsigned k = 0;
    if (st == AN_ST_CONNECTED) {
        uint32_t v = (uint32_t)E.speed; char d[8]; int nd = 0;
        do { d[nd++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (nd) t[k++] = d[--nd];
        for (const char *p = E.full ? " Mbit/s" : " Mbit/s half"; *p; ) t[k++] = *p++;
    } else for (const char *p = st == AN_ST_IDLE ? "no link" : st == AN_ST_STARTING ? "starting" : st == AN_ST_FAILED ? "FAILED" : "off"; *p; ) t[k++] = *p++;
    t[k] = 0;
    frf_post_eth(st == AN_ST_OFF ? "" : t, st == AN_ST_CONNECTED ? 1 : st == AN_ST_FAILED ? 2 : 0);
}

static int mdio(int wr, int reg, uint16_t v)
{
    gwr(G_MDIO, (wr ? (1u << 26) : (2u << 26)) | ((uint32_t)E.phy << 21) | ((uint32_t)reg << 16) | (wr ? v : 0));
    gwr(G_MDIO, grd(G_MDIO) | (1u << 29));           /* start */
    for (int i = 0; i < 1000; i++) {
        uint32_t c = grd(G_MDIO);
        if (!(c & (1u << 29))) return (!wr && (c & (1u << 28))) ? -1 : (int)(c & 0xffff);
        gdelay(1);
    }
    return -2;
}
static int phy_rd(int reg) { return mdio(0, reg, 0); }
static void phy_wr(int reg, uint16_t v) { mdio(1, reg, v); }

/* the genet node: its registers (through /scb's ranges, rewritten to virtual
   addresses at boot) and its MAC address */
static of_node_t *genet_node(void)
{
    of_node_t *al = dt_find_node("/aliases");
    of_property_t *ap = al ? dt_find_property(al, "ethernet0") : 0;
    return (ap && ap->op_value) ? dt_find_node((char *)ap->op_value) : 0;
}
static uintptr_t scb_virt(uint32_t bus)
{
    of_node_t *e = dt_find_node("/scb");
    of_property_t *p = e ? dt_find_property(e, "ranges") : 0;
    if (!p) return 0;
    int ac = (int)dt_get_property_value_u32(e->on_parent, "#address-cells", 1, 0);
    int ab = (int)dt_get_property_value_u32(e, "#address-cells", 1, 1);
    int sb = (int)dt_get_property_value_u32(e, "#size-cells", 1, 1);
    int n = ab + ac + sb;
    uint32_t *r = p->op_value;
    for (int32_t len = (int32_t)p->op_length; len >= n * 4; len -= n * 4, r += n) {
        uint32_t b = r[ab - 1], c = r[ab + ac - 1], l = r[n - 1];
        if (b == c) continue;
        if (bus - b < l) return (uintptr_t)c + (bus - b);
    }
    return 0;
}
/* the addresses alone, at boot: so that a stack can attach to a port that is switched off
   (it is told the address then). The port is not claimed here: eth_find does that, when it starts. */
static void eth_learn_mac(volatile struct an_unit *u)
{
    of_node_t *g = genet_node();
    of_property_t *m = g ? dt_find_property(g, "local-mac-address") : 0;
    if (!m || m->op_length < 6) return;
    memcpy(E.fmac, m->op_value, 6);
    mac_wanted(E.want_mac, E.want_oui, E.fmac, E.mac);
    for (int i = 0; i < 6; i++) { u->mac[i] = E.mac[i]; u->factory_mac[i] = E.fmac[i]; }
}
static int eth_find(void)
{
    if (E.regs) return 0;
    of_node_t *g = genet_node();
    if (!g) { eth_log("no ethernet0 in the device tree\n"); return -1; }
    of_property_t *reg = dt_find_property(g, "reg");
    int ac = (int)dt_get_property_value_u32(g->on_parent, "#address-cells", 1, 0);
    if (!reg || reg->op_length < (uint32_t)ac * 4) return -1;
    uintptr_t v = scb_virt(((uint32_t *)reg->op_value)[ac - 1]);
    if (!v) { eth_log("GENET's registers are not mapped\n"); return -1; }
#ifdef PSMAC
    v = alice_periph_hi(v);                     /* through the mapping no Mac MMU switch unmaps */
#endif
    E.regs = (volatile uint8_t *)v;
    of_property_t *m = dt_find_property(g, "local-mac-address");
    if (m && m->op_length >= 6) { memcpy(E.fmac, m->op_value, 6); mac_wanted(E.want_mac, E.want_oui, E.fmac, E.mac); }
    if (!(E.mac[0] | E.mac[1] | E.mac[2] | E.mac[3] | E.mac[4] | E.mac[5])) {
        eth_log("no local-mac-address in the device tree (start4.elf fills it in): no MAC address\n");
        return -1;
    }
    /* the kernel drives it now: a driver in AmigaOS that reads the device tree leaves it alone */
    static char disabled[] = "disabled";
    of_property_t *st = dt_find_property(g, "status");
    if (st) { st->op_value = disabled; st->op_length = sizeof disabled; }
    else dt_add_property(g, "status", disabled, sizeof disabled);
    eth_log("GENET at %p, MAC %02x:%02x:%02x:%02x:%02x:%02x; %s marked disabled for AmigaOS\n", (void *)v,
            E.mac[0], E.mac[1], E.mac[2], E.mac[3], E.mac[4], E.mac[5], g->on_name);
    return 0;
}
static int eth_buffers(void)
{
    if (E.rxb) return 0;
    uint32_t size = (NRX + NTXB) * GBUF;
    uint8_t *m = tlsf_malloc_aligned(tlsf, size, 0x10000);
    if (!m) return -1;
    uintptr_t phys = mmu_virt2phys((uintptr_t)m);
    arm_flush_cache((uintptr_t)m, size);
    mmu_map(phys, (uintptr_t)m, size, AN_MMU_ACCESS | AN_MMU_ISHARE | AN_MMU_NC, 0);
    __asm__ volatile("dsb ish; isb" ::: "memory");
    E.rxb = m; E.txb = m + NRX * GBUF;
    E.rxp = phys; E.txp = phys + NRX * GBUF;
    return 0;
}

static void eth_start(void)
{
    volatile struct an_unit *u = U(1);
    estate(AN_ST_STARTING, "starting the Ethernet");
    if (eth_find() || eth_buffers()) { E.state = E_FAILED; estate(AN_ST_FAILED, "Ethernet: no GENET, or no memory"); return; }
    uint32_t rev = grd(G_SYS_REV);
    if (((rev >> 24) & 15) != 6) { E.state = E_FAILED; estate(AN_ST_FAILED, "Ethernet: not a GENET v5"); eth_log("SYS_REV %x\n", rev); return; }
    for (int i = 0; i < 6; i++) u->mac[i] = E.mac[i];
    if (!(u->factory_mac[0] | u->factory_mac[1] | u->factory_mac[2])) for (int i = 0; i < 6; i++) u->factory_mac[i] = E.fmac[i];
    gwr(G_PORT_CTRL, 3);                                /* external gigabit PHY (RGMII) */
    /* the MAC reset, as U-Boot */
    gwr(G_RBUF_FLUSH, grd(G_RBUF_FLUSH) | 2); gdelay(10);
    gwr(G_RBUF_FLUSH, grd(G_RBUF_FLUSH) & ~2u); gdelay(10);
    gwr(G_RBUF_FLUSH, 0); gdelay(10);
    gwr(G_CMD, 0);
    gwr(G_CMD, CMD_SW_RESET | CMD_LCL_LOOP); gdelay(2);
    gwr(G_CMD, 0);
    gwr(G_MIB_CTRL, 7); gwr(G_MIB_CTRL, 0);
    gwr(G_MAX_FRAME, 1536);
    gwr(G_RBUF_CTRL, (grd(G_RBUF_CTRL) & ~1u) | 2);     /* 2-byte pad before each frame, no 64-byte status block */
    gwr(G_RBUF_TBUF_SZ, 1);
    gwr(G_MAC0, (uint32_t)E.mac[0] << 24 | (uint32_t)E.mac[1] << 16 | (uint32_t)E.mac[2] << 8 | E.mac[3]);
    gwr(G_MAC1, (uint32_t)E.mac[4] << 8 | E.mac[5]);
    /* DMA off, TX flushed */
    gwr(G_TDMA + 4, grd(G_TDMA + 4) & ~DMA_EN);
    gwr(G_RDMA + 4, grd(G_RDMA + 4) & ~DMA_EN);
    gwr(G_TX_FLUSH, 1); gdelay(10); gwr(G_TX_FLUSH, 0);
    /* receive ring: 256 buffers, all of them ours to fill */
    gwr(G_RDMA + 0x0c, 8);                              /* SCB burst */
    gwr(G_RR + 0x14, 0); gwr(G_RR + 0x2c, 0); gwr(G_RR + 0x00, 0);
    gwr(G_RR + 0x1c, NRX * 12 / 4 - 1);
    E.cidx = grd(G_RR + 0x08) & 0xffff;                 /* the producer index cannot be set: follow it */
    gwr(G_RR + 0x0c, E.cidx);
    gwr(G_RR + 0x00, (E.cidx & 0xff) * 3); gwr(G_RR + 0x2c, (E.cidx & 0xff) * 3);   /* pointers (words) where the index is */
    gwr(G_RR + 0x10, (uint32_t)NRX << 16 | GBUF);
    gwr(G_RR + 0x28, (5u << 16) | (NRX >> 4));
    gwr(G_RDMA + 0x00, 1u << 16);
    for (int i = 0; i < NRX; i++) {
        uint64_t a = E.rxp + (uint64_t)i * GBUF;
        gwr(G_RXD + i * 12 + 4, (uint32_t)a);
        gwr(G_RXD + i * 12 + 8, (uint32_t)(a >> 32));
        gwr(G_RXD + i * 12 + 0, (uint32_t)GBUF << 16 | 0x8000u);
    }
    /* transmit ring */
    gwr(G_TDMA + 0x0c, 8);
    gwr(G_TR + 0x14, 0); gwr(G_TR + 0x00, 0); gwr(G_TR + 0x2c, 0);
    gwr(G_TR + 0x1c, 256 * 12 / 4 - 1);
    E.tprod = grd(G_TR + 0x08) & 0xffff;
    gwr(G_TR + 0x0c, E.tprod);
    gwr(G_TR + 0x00, (E.tprod & 0xff) * 3); gwr(G_TR + 0x2c, (E.tprod & 0xff) * 3);
    gwr(G_TR + 0x24, 1); gwr(G_TR + 0x28, 0);
    gwr(G_TR + 0x10, 256u << 16 | GBUF);
    gwr(G_TDMA + 0x00, 1u << 16);
    /* DMA on */
    gwr(G_TDMA + 4, (1u << 17) | DMA_EN);
    gwr(G_RDMA + 4, grd(G_RDMA + 4) | (1u << 17) | DMA_EN);
    /* the PHY: the first address that answers (1 on a Pi 4), reset */
    E.phy = -1;
    for (int a = 0; a < 32 && E.phy < 0; a++) {
        E.phy = a;
        int id1 = phy_rd(2), id2 = phy_rd(3);
        if (id1 <= 0 || id1 == 0xffff) { E.phy = -1; continue; }
        eth_log("PHY at %d: id %04x:%04x\n", a, id1, id2);
    }
    if (E.phy < 0) { E.state = E_FAILED; estate(AN_ST_FAILED, "Ethernet: no PHY answers"); return; }
    phy_wr(0, 0x8000);                                    /* BMCR: reset */
    E.t0 = now_ms();
    E.state = E_PHYRESET;
    E.up = 0;
}
/* the PHY as the settings say: what to advertise and autonegotiate, or a fixed speed */
static void eth_phy_apply(void)
{
    volatile struct an_unit *u = U(1);
    uint32_t a = u->cfg_adv, f = u->cfg_flags;
    if (f & AN_EC_AUTONEG) {
        int adv4 = 0x0001 | ((a & AN_LM_10H) ? 0x020 : 0) | ((a & AN_LM_10F) ? 0x040 : 0) |
                   ((a & AN_LM_100H) ? 0x080 : 0) | ((a & AN_LM_100F) ? 0x100 : 0) | ((f & AN_EC_PAUSE) ? 0x400 : 0);
        int adv9 = ((a & AN_LM_1000H) ? 0x100 : 0) | ((a & AN_LM_1000F) ? 0x200 : 0);
        phy_wr(4, adv4);
        phy_wr(9, adv9);
        phy_wr(0, 0x1200);                              /* autonegotiation on, restart */
    } else {
        int bmcr = (u->cfg_speed == 1000 ? 0x0040 : u->cfg_speed == 100 ? 0x2000 : 0) | ((f & AN_EC_FULL) ? 0x0100 : 0);
        phy_wr(0, bmcr);                                /* autonegotiation off: this speed and duplex */
    }
}
static void eth_link(int bmsr)
{
    volatile struct an_unit *u = U(1);
    int link = (bmsr & 4) != 0;
    if (link && !E.up) {
        int c1000 = phy_rd(9), s1000 = phy_rd(10), adv = phy_rd(4), lpa = phy_rd(5);
        if (c1000 < 0 || s1000 < 0 || adv < 0 || lpa < 0) return;   /* an MDIO error: the next poll tries again */
        int com = adv & lpa;
        uint32_t cf = u->cfg_flags;
        /* what each side advertises, for people (Alice68-Network's Ethernet page) */
        static const uint16_t bits[6] = { 0x020, 0x040, 0x080, 0x100, 0x400, 0x800 };
        static const uint8_t  flag[6] = { AN_LM_10H, AN_LM_10F, AN_LM_100H, AN_LM_100F, AN_LM_PAUSE, AN_LM_ASYM };
        unsigned la = 0, lp = 0;
        for (int i = 0; i < 6; i++) {
            if (adv & bits[i]) la |= flag[i];
            if (lpa & bits[i]) lp |= flag[i];
        }
        la |= ((c1000 & 0x100) ? AN_LM_1000H : 0) | ((c1000 & 0x200) ? AN_LM_1000F : 0);
        lp |= ((s1000 & 0x400) ? AN_LM_1000H : 0) | ((s1000 & 0x800) ? AN_LM_1000F : 0);
        u->link_adv = la; u->link_lpa = lp;
        if (!(cf & AN_EC_AUTONEG)) { E.speed = (int)u->cfg_speed; E.full = (cf & AN_EC_FULL) != 0; }
        else if ((c1000 & 0x200) && (s1000 & 0x800)) { E.speed = 1000; E.full = 1; }
        else if ((c1000 & 0x100) && (s1000 & 0x400)) { E.speed = 1000; E.full = 0; }
        else if (com & 0x100) { E.speed = 100; E.full = 1; }
        else if (com & 0x080) { E.speed = 100; E.full = 0; }
        else if (com & 0x040) { E.speed = 10; E.full = 1; }
        else { E.speed = 10; E.full = 0; }
        gwr(G_RGMII_OOB, (grd(G_RGMII_OOB) & ~(1u << 5)) | (1u << 4) | (1u << 6) | (1u << 16));
        /* flow control when both sides offer it (full duplex only); else pause frames are ignored */
        int pause = (cf & AN_EC_AUTONEG) && (cf & AN_EC_PAUSE) && E.full && (adv & lpa & 0x400);
        E.cmd_base = (uint32_t)(E.speed == 1000 ? 2 : E.speed == 100 ? 1 : 0) << 2 | (E.full ? 0 : CMD_HD_EN)
                   | (pause ? 0 : (1u << 8) | (1u << 28));
        gwr(G_CMD, E.cmd_base | (GENET_PROMISC() ? CMD_PROMISC : 0) | CMD_TX_EN | CMD_RX_EN);
        E.up = 1;
        u->link_flags = ((cf & AN_EC_AUTONEG) && (bmsr & 0x20) ? AN_LF_AUTONEG : 0) | (E.full ? AN_LF_FULL : 0) |
                        (pause ? AN_LF_RXPAUSE | AN_LF_TXPAUSE : 0);
        u->link_up_s = wifi_usecs() / 1000000u;
        u->link = 1; u->speed_mbps = (unsigned)E.speed;
        char m[64]; unsigned k = 0;
        for (const char *p = "link up, "; *p; ) m[k++] = *p++;
        { uint32_t v = (uint32_t)E.speed; char d[8]; int nd = 0; do { d[nd++] = (char)('0' + v % 10); v /= 10; } while (v); while (nd) m[k++] = d[--nd]; }
        for (const char *p = E.full ? " Mbit/s full duplex" : " Mbit/s half duplex"; *p; ) m[k++] = *p++;
        m[k] = 0;
        estate(AN_ST_CONNECTED, m);
    } else if (!link && E.up) {
        E.up = 0;
        gwr(G_CMD, grd(G_CMD) & ~(CMD_TX_EN | CMD_RX_EN));
        u->link = 0; u->speed_mbps = 0;
        u->link_downs++;
        u->state_seq++;
        estate(AN_ST_IDLE, "no link: is the cable in?");
    }
}
static void eth_rx(void)
{
    volatile struct an_unit *u = U(1);
    uint32_t prod = grd(G_RR + 0x08) & 0xffff, n = 0;
    __asm__ volatile("dmb oshld" ::: "memory");          /* the frames after the index that says they are there */
    while (E.cidx != prod && n < 64) {
        uint32_t d = E.cidx & 0xff, ls = grd(G_RXD + d * 12);
        uint32_t len = (ls >> 16) & 0xfff, st = ls & 0xffff;
        const uint8_t *f = E.rxb + d * GBUF + 2;
        if ((st & (DESC_SOP | DESC_EOP)) != (DESC_SOP | DESC_EOP) || (st & 0x1f) || len < 16 || len > 1536 + 2) {
            E.rx_err++, u->rx_drop++;
            if (len >= 16 && len <= 1536 + 2) trace_rx(1, f, len - 2, (st & 0x1f) ? "DROPPED: GENET says the frame is bad" : "DROPPED: not one buffer");
            else if (an_drops_logged < 40) { an_drops_logged++; eth_log("rx descriptor status %04x len %u: DROPPED\n", st, len); }
        }
#ifdef PSMAC
        else if (psmac_ip_active && psmac_ip_rx(1, f, len - 2, E.mac, E.promisc) != IPK_TO_MAC) { }
#endif
        else if (!u->rx_on) { u->rx_drop++; trace_rx(1, f, len - 2, "DROPPED: the Amiga takes no frames (RXOFF)"); }
        else if (u->rx_head - u->rx_tail >= AN_RX_SLOTS) {
            /* the Amiga's 16 slots are full: leave this frame, and the ones behind it, in GENET's
               own ring (256 frames) until a slot is free. Dropping it here cost a burst its tail
               and TCP a retransmit each time: 2869 retransmits in one speed test. GENET drops
               only once its ring is full too. */
            E.rx_waits++;
            break;
        } else {
            len -= 2;
            trace_rx(1, f, len, "to the Amiga");
            volatile struct an_slot *s = (volatile struct an_slot *)(anb + AN_OFF_RX(1) + (u->rx_head % AN_RX_SLOTS) * AN_SLOT_BYTES);
            ncopy(s->data, f, len);
            s->len = len;
            s->flags = (f[0] & 1) ? (f[0] == 0xff && f[1] == 0xff ? AN_SF_BCAST : AN_SF_MCAST) : 0;
            NETPROBE_RX(1, f, len, u->rx_head, &u->rx_tail);
            __asm__ volatile("dmb ish" ::: "memory");
            u->rx_head++;
            u->rx_pkts++; u->rx_bytes += len;
            an_irq();
        }
        E.cidx = (E.cidx + 1) & 0xffff;
        n++;
    }
    if (n) { __asm__ volatile("dmb osh" ::: "memory"); gwr(G_RR + 0x0c, E.cidx); }
}
static void eth_tx(void)
{
    volatile struct an_unit *u = U(1);
    int sent = 0;
    while (u->tx_tail != u->tx_head) {
        uint32_t cons = grd(G_TR + 0x08) & 0xffff;
        if (((E.tprod - cons) & 0xffff) >= 32) break;  /* in flight: at most 32 (the buffers are 64) */
        __asm__ volatile("dmb ish" ::: "memory");
        volatile struct an_slot *s = (volatile struct an_slot *)(anb + AN_OFF_TX(1) + (u->tx_tail % AN_TX_SLOTS) * AN_SLOT_BYTES);
        uint32_t len = s->len;
        if (len < 14 || len > 1518 || !E.up) u->tx_err++;
        else {
            uint32_t b = E.tprod % NTXB, d = E.tprod & 0xff;
            uint8_t *q = E.txb + b * GBUF;
            ncopy(q, s->data, len);
#ifdef PSMAC
            psmac_ip_mac_tx(1, q, len);
#endif
            trace_tx(1, q, len, 1);
            NETPROBE_TX(1, q, len, 0);
            NETPROBE_TX(1, q, len, 1);                  /* GENET's DMA takes it from here: "out" when queued */
            for (; len < 60; len++) q[len] = 0;         /* the shortest Ethernet frame */
            uint64_t a = E.txp + (uint64_t)b * GBUF;
            gwr(G_TXD + d * 12 + 4, (uint32_t)a);
            gwr(G_TXD + d * 12 + 8, (uint32_t)(a >> 32));
            gwr(G_TXD + d * 12 + 0, len << 16 | (0x3fu << 7) | DESC_CRC | DESC_SOP | DESC_EOP);
            E.tprod = (E.tprod + 1) & 0xffff;
            u->tx_pkts++; u->tx_bytes += len;
            sent = 1;
        }
        __asm__ volatile("dmb ish" ::: "memory");
        u->tx_tail++;
        an_irq();
    }
#ifdef PSMAC
    /* the kernel's own frames, once the Mac's are out */
    while (psmac_ip_active && E.up && u->tx_tail == u->tx_head && ((E.tprod - (grd(G_TR + 0x08) & 0xffff)) & 0xffff) < 32) {
        uint32_t b = E.tprod % NTXB, d = E.tprod & 0xff;
        uint8_t *q = E.txb + b * GBUF;
        uint32_t len = psmac_ip_tx_take(1, q, GBUF);
        if (!len) break;
        uint64_t a = E.txp + (uint64_t)b * GBUF;
        gwr(G_TXD + d * 12 + 4, (uint32_t)a);
        gwr(G_TXD + d * 12 + 8, (uint32_t)(a >> 32));
        gwr(G_TXD + d * 12 + 0, len << 16 | (0x3fu << 7) | DESC_CRC | DESC_SOP | DESC_EOP);
        E.tprod = (E.tprod + 1) & 0xffff;
        sent = 1;
    }
#endif
    if (sent) { __asm__ volatile("dsb sy" ::: "memory"); gwr(G_TR + 0x0c, E.tprod); }
}
static void eth_stop(void)
{
    if (E.regs && E.state != E_OFF && E.state != E_FAILED) {
        gwr(G_CMD, grd(G_CMD) & ~(CMD_TX_EN | CMD_RX_EN));
        gwr(G_TDMA + 4, grd(G_TDMA + 4) & ~DMA_EN);
        gwr(G_RDMA + 4, grd(G_RDMA + 4) & ~DMA_EN);
        if (E.phy >= 0) phy_wr(0, 0x0800);                /* PHY power down */
    }
    E.up = 0;
    U(1)->link = 0;
    E.state = E_OFF;
    estate(AN_ST_OFF, "switched off");
}
static void eth_discard_tx(volatile struct an_unit *u)
{
    while (u->tx_tail != u->tx_head) { u->tx_tail++; u->tx_err++; }
}
static void eth_commands(void)
{
    volatile struct an_unit *u = U(1);
    uint32_t seq = u->cmd_seq;
    if (seq == u->cmd_done) return;
    __asm__ volatile("dmb ish" ::: "memory");
    int r = 0;
    switch (u->cmd) {
    case AN_CMD_START: u->admin_off = 0; if (E.state == E_OFF || E.state == E_FAILED) eth_start(); break;
    case AN_CMD_OFF:   u->admin_off = 1; eth_stop(); eth_discard_tx(u); break;
    case AN_CMD_ENABLE:
        if (arg32(u, 0)) {
            u->admin_off = 0;
            if (u->rx_on && (E.state == E_OFF || E.state == E_FAILED)) eth_start();     /* a stack is using it */
            else if (E.state == E_OFF) estate(AN_ST_OFF, "on: starts when a TCP/IP stack uses unit 1");
            eth_log("switched on\n");
        } else {
            u->admin_off = 1;
            eth_stop();
            eth_discard_tx(u);
            estate(AN_ST_OFF, "switched off");
            eth_log("switched off\n");
        }
        u->state_seq++;
        break;
    case AN_CMD_RXON:  u->rx_on = 1; break;
    case AN_CMD_RXOFF: u->rx_on = 0; break;
    case AN_CMD_ETHCFG: {
        uint32_t a = arg32(u, 0) & 0x3fu, f = arg32(u, 4) & (AN_EC_AUTONEG | AN_EC_FULL | AN_EC_PAUSE), sp = arg32(u, 8);
        if ((f & AN_EC_AUTONEG) ? !a : (sp != 10 && sp != 100 && sp != 1000)) { r = -2; break; }
        u->cfg_adv = a; u->cfg_flags = f; u->cfg_speed = sp ? sp : 1000;
        if (E.state == E_LINKWAIT || E.state == E_RUN) {
            eth_phy_apply();                           /* the link drops and comes back as set */
            eth_log("settings: %s\n", (f & AN_EC_AUTONEG) ? "autonegotiation" : "fixed speed");
        }
        u->state_seq++;
        break;
    }
    case AN_CMD_SETMAC: {
        uint8_t m[6]; int any = 0;
        for (int i = 0; i < 6; i++) { m[i] = u->arg[i]; any |= m[i]; }
        if (any && (m[0] & 1)) { r = -2; break; }      /* a group address is no station's */
        memcpy(E.want_mac, m, 6);
        E.want_oui = u->arg[6] == 1;
        if (E.fmac[0] | E.fmac[1] | E.fmac[2]) {       /* known: now, else when the Ethernet starts */
            mac_wanted(E.want_mac, E.want_oui, E.fmac, E.mac);
            for (int i = 0; i < 6; i++) u->mac[i] = E.mac[i];
            if (E.regs) {
                gwr(G_MAC0, (uint32_t)E.mac[0] << 24 | (uint32_t)E.mac[1] << 16 | (uint32_t)E.mac[2] << 8 | E.mac[3]);
                gwr(G_MAC1, (uint32_t)E.mac[4] << 8 | E.mac[5]);
            }
            eth_log("MAC address now %02x:%02x:%02x:%02x:%02x:%02x%s\n", E.mac[0], E.mac[1], E.mac[2], E.mac[3], E.mac[4], E.mac[5], any ? "" : " (the Pi's own)");
        }
        u->state_seq++;
        break;
    }
    case AN_CMD_FILTER:
        E.promisc = u->arg[3] & (AN_FILT_PROMISC | AN_FILT_ALLMULTI);   /* no multicast filter yet: all of it */
        if (E.up) gwr(G_CMD, E.cmd_base | (GENET_PROMISC() ? CMD_PROMISC : 0) | CMD_TX_EN | CMD_RX_EN);
        break;
    default: r = -3; break;
    }
    u->cmd_result = r;
    __asm__ volatile("dmb ish" ::: "memory");
    u->cmd_done = seq;
    an_irq();
}
static void eth_poll(void)
{
    volatile struct an_unit *u = U(1);
#ifdef PSMAC
    extern int psmac_eth_testbed_poll(volatile struct an_unit *u);   /* alice_psmac.c: 1 = it owns unit 1 */
    if (psmac_eth_testbed_poll(u)) return;
#endif
    if (!E.started) {
        E.started = 1;
        E.disabled = bootarg_word("eth=off");
        if (E.disabled) { u->kind = AN_KIND_NONE; kprintf("[ETH] off (eth=off in cmdline.txt)\n"); return; }
        u->kind = AN_KIND_ETHER;
        u->cfg_adv = AN_LM_10H | AN_LM_10F | AN_LM_100H | AN_LM_100F | AN_LM_1000H | AN_LM_1000F;
        u->cfg_flags = AN_EC_AUTONEG | AN_EC_FULL;
        u->cfg_speed = 1000;
        E.want_oui = oui_default(E.want_mac);
        E.state = E_OFF;
        eth_learn_mac(u);
        estate(AN_ST_OFF, "starts when a TCP/IP stack uses unit 1, or eth=on in cmdline.txt");
        if (bootarg_word("eth=on")) eth_start();
    }
    if (E.disabled) {                                   /* eth=off: no Ethernet, but no command waits for ever */
        if (u->cmd_seq != u->cmd_done) {
            u->cmd_result = -1;
            __asm__ volatile("dmb ish" ::: "memory");
            u->cmd_done = u->cmd_seq;
            an_irq();
        }
        return;
    }
    eth_commands();
    uint32_t now = now_ms();
    switch (E.state) {
    case E_PHYRESET: {
        int bmcr = phy_rd(0);
        if ((bmcr >= 0 && !(bmcr & 0x8000)) || now - E.t0 > 500) {
            /* the RGMII clock delays: the PHY's own defaults, as Linux and U-Boot leave them on a Pi 4 */
            eth_phy_apply();
            E.state = E_LINKWAIT;
            E.next_link = now;
            estate(AN_ST_IDLE, "waiting for the link");
        }
        break;
    }
    case E_LINKWAIT:
    case E_RUN:
        if ((int32_t)(now - E.next_link) >= 0) {
            E.next_link = now + 250;
            phy_rd(1);                                     /* BMSR's link bit latches low: read twice */
            int bmsr = phy_rd(1);
            if (bmsr >= 0) eth_link(bmsr);
            E.state = E.up ? E_RUN : E_LINKWAIT;
        }
        if (E.up) { eth_rx(); eth_tx(); }
        else while (u->tx_tail != u->tx_head) { u->tx_tail++; u->tx_err++; }
        break;
    default:
        break;
    }
}

/* ---- CPU1's turn ------------------------------------------------------------- */
static void an_reset_session(void)
{
    an_reset_req = 0;
    H()->amiga_ver = 0;
    if (H()->fw_state == AN_FW_OFFERED) H()->fw_state = AN_FW_NONE;     /* its buffers belonged to the old AmigaOS */
    for (int i = 0; i < AN_UNITS; i++) {
        volatile struct an_unit *u = U(i);
        u->rx_on = 0;
        u->rx_tail = u->rx_head;
        u->tx_tail = u->tx_head;
        u->cmd_done = u->cmd_seq;
    }
#ifdef PSMAC
    kprintf("[NET] new Mac session (68k reset)\n");
#else
    kprintf("[NET] new AmigaOS session (68k reset)\n");
#endif
}
/* How CPU1 serves the board, once a second into unit 1's "where" (Alice68-Monitor DUMP):
   how often this runs, the longest gap between two runs, and the frames moved. A board of
   16 slots per unit moves at most 16 frames a run, so the gap bounds the throughput. */
static struct { uint32_t t0, last, runs, gap, rx0, tx0, waits0; } ls;
static void loop_stats(void)
{
    uint32_t now = wifi_usecs();
    if (ls.last && now - ls.last > ls.gap) ls.gap = now - ls.last;
    ls.last = now;
    ls.runs++;
    if (!ls.t0) { ls.t0 = now; return; }
    if (now - ls.t0 < 1000000u) return;
    volatile struct an_unit *u = U(1);
    uint32_t rx = u->rx_pkts, tx = u->tx_pkts;
    char t[96];
    unsigned n = 0;
    #define PUTS(x) for (const char *q_ = (x); *q_ && n < sizeof t - 1; ) t[n++] = *q_++
    #define PUTN(v) do { uint32_t v_ = (v); char d_[11]; int k_ = 0; do d_[k_++] = (char)('0' + v_ % 10); while ((v_ /= 10) && k_ < 10); while (k_ && n < sizeof t - 1) t[n++] = d_[--k_]; } while (0)
    PUTS("CPU1 "); PUTN(ls.runs); PUTS(" runs/s, longest gap "); PUTN(ls.gap); PUTS(" us; rx ");
    PUTN(rx - ls.rx0); PUTS("/s tx "); PUTN(tx - ls.tx0); PUTS("/s; held in GENET "); PUTN(E.rx_waits - ls.waits0);
    #undef PUTS
    #undef PUTN
    for (unsigned i = 0; i < sizeof t; i++) u->where[i] = i < n ? t[i] : 0;
    ls.t0 = now; ls.runs = 0; ls.gap = 0; ls.rx0 = rx; ls.tx0 = tx; ls.waits0 = E.rx_waits;
}

#ifdef PSMAC
extern void psmac_loop_at(const char *name);       /* alice_psmac.c: for the service loop's watch */
#define NET_AT(name) psmac_loop_at(name)
/* where the WiFi is (wifi_where), for the watch's line */
const char *alice_wifi_where(void) { return anb ? (const char *)U(0)->where : ""; }
#else
#define NET_AT(name) ((void)0)
#endif
void alice_net_poll(void)
{
    extern void bt_pi_poll(void);
    extern void alice_status_loop(void), alice_status_poll(void);
    NET_AT("network: the status page");
    alice_status_loop();                            /* CPU1's turn, timed for the status page */
    alice_status_poll();                            /* the status page, about once a second (alice_status.c) */
    NET_AT("network: Bluetooth");
    bt_pi_poll();                                   /* Bluetooth (src/alice/bt/bt_pi.c): it waits for the board itself */
    NET_AT("network: the Ethernet");
    if (!anb) return;
    setup_poll();                                   /* the setup screen's changes written yet? */
    if (H()->restart == AN_RESTART) {               /* AmigaOS asks for a fresh start (Alice68-Card) */
        H()->restart = 0;
        kprintf("[BOOT] restart asked for by AmigaOS (Alice68-Card) - restarting the Pi\n");
        uint64_t end = ticks() + tick_hz() / 2;
        while ((int64_t)(end - ticks()) > 0) ;
        aga_reboot(0);
    }
    H()->boot_us = wifi_usecs();
    loop_stats();
    if (an_reset_req) an_reset_session();
    eth_poll();                                     /* the Ethernet: a state machine, never waits */
#ifdef PSMAC
    { extern int psmac_wifi_testbed_poll(volatile struct an_unit *u);   /* alice_psmac.c: 1 = a simulated radio owns unit 0 */
      if (psmac_wifi_testbed_poll(U(0))) return; }
#endif
    if (!wifi_running) { wifi_disabled = bootarg_word("wifi=off"); U(0)->kind = AN_KIND_WIFI; }
    fw_take();
    if (!wifi_running) {
        wifi_running = 1;
        if (wifi_disabled) {
            kprintf("[WIFI] off (wifi=off in cmdline.txt)\n");
            wstate(AN_ST_OFF, "off: wifi=off in cmdline.txt");
            return;
        }
        uint8_t *stk = tlsf_malloc_aligned(tlsf, WIFI_STACK, 16);
        if (!stk) { kprintf("[WIFI] no memory for its stack\n"); wifi_disabled = 1; return; }
        memset(&ctx_wifi, 0, sizeof ctx_wifi);
        ctx_wifi.x[0] = (uint64_t)(uintptr_t)wifi_thread;               /* x19: the trampoline calls it */
        ctx_wifi.x30 = (uint64_t)(uintptr_t)an_trampoline;
        ctx_wifi.sp = ((uint64_t)(uintptr_t)(stk + WIFI_STACK)) & ~15ull;
        kprintf("[WIFI] the WiFi runs on CPU1 (alice_net.c)\n");
        claim_host_in_dt();
    }
    if (wifi_disabled) return;
#ifdef PSMAC
    wifi_core = an_this_core();
    wifi_turn_t0 = ticks();
    NET_AT("network: the WiFi's turn");
#endif
    in_wifi = 1;
    an_switch(&ctx_main, &ctx_wifi);
    in_wifi = 0;
    NET_AT("network: the Ethernet");
    if (E.up) { eth_rx(); eth_tx(); }               /* again after the WiFi's turn: a long one delays it less */
}

/* the no-USB kernel: CPU1 is the network's alone */
void __attribute__((noreturn)) alice_net_cpu1_main(void)
{
    kprintf("[NET] CPU1: network only (no USB in this kernel)\n");
    for (;;) {
        alice_net_poll();
        __asm__ volatile("yield");
    }
}
