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
#include "spinlock.h"
#include "M68k.h"
#include "A64.h"
#include "globals.h"
#include "ps_protocol.h"
#include "psmac.h"
#include "psmb.h"
#include "devicetree.h"
#include "psmac_rom_delta.h"
void psmac_trace(uint32_t n);           /* the first-N bus cycle trace (below) */
static int bustrace_asked;              /* mac.bustrace= given: keep it even on the GPIO transport */

void mmu_map(uintptr_t phys, uintptr_t virt, uintptr_t length, uint32_t attr_low, uint32_t attr_high);
uintptr_t mmu_virt2phys(uintptr_t addr);

#define PERIPH_VA        PSMAC_PERIPH_HI /* the kernel-space mapping of the peripherals (psmac.h): never unmapped by an MMU switch */
#define PERIPH_LOW       PSMAC_PERIPH_LOW /* their place in the Mac's 4 GiB (start_rpi64.c): what a 68k address is checked against */
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
static uintptr_t win_phys, declrom_phys;   /* slot $9 mailbox window and declaration ROM (Pi RAM) */
void psmac_mmu_set_fb(uintptr_t f);                   /* psmac_mmu.c */
static uintptr_t fb_big_phys;              /* the second display's big frame buffer (psmb.h) */
static uint32_t screen_w, screen_h;        /* mac.screen=WxH: the second display's size (0: the VideoCore's) */
static int no_declrom;                     /* no declaration ROM anywhere: said at every heartbeat, slot 9 is empty */
uintptr_t psmac_fb_big_phys(void) { return fb_big_phys; }
void psmac_screen_size(uint32_t *w, uint32_t *h) { *w = screen_w; *h = screen_h; }
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
static inline int in_periph_window(uint64_t a) { return a >= PERIPH_LOW && a < 0x40000000ull; }
static volatile uint32_t dev_cnt[8]; /* VIA1, SCC, SCSI, ASC, SWIM, RBV, FB, other */
static inline int dev_class(uint32_t a) { uint32_t o = a & 0x000fffff; if (a < 0x80000) return 6; if ((a & 0xff000000) != 0x50000000) return 7; if (o < 0x2000 || (o >= 0x40000 && o < 0x42000)) return 0; if (o >= 0x4000 && o < 0x6000) return 1; if ((o >= 0x10000 && o < 0x14000) || (o >= 0x6000 && o < 0x8000)) return 2; if (o >= 0x14000 && o < 0x16000) return 3; if (o >= 0x16000 && o < 0x18000) return 4; if (o >= 0x24000 && o < 0x28000) return 5; return 7; }

/* SCSI register trace (mac.scsitrace=N lines): every write, and every read whose value differs from
   the previous read of the same register; a run of identical polls is folded into one line with its
   count. This is how the ROM's selection and transfer sequences are read against the NCR 5380 data
   sheet when a boot stalls in a SCSI wait loop (docs/08, fix 14). */
static uint32_t scsi_trace_left;
static void scsi_trace(int op, uint32_t addr, uint32_t wdata, uint32_t rdata)
{
    static uint32_t last_addr = 0xffffffffu, last_val, repeats;
    if (!scsi_trace_left) return;
    if (op == PSMAC_OP_READ && addr == last_addr && rdata == last_val) { repeats++; return; }
    if (repeats) { kprintf("[SCSI]   ... %u more reads of %08x -> %02x\n", repeats, last_addr, last_val); repeats = 0; }
    scsi_trace_left--;
    if (op == PSMAC_OP_READ) { kprintf("[SCSI] R %08x -> %02x (reg %u) PC %08x\n", addr, rdata & 0xff, (addr >> 4) & 7, __m68k_state ? BE32((uint32_t)(uintptr_t)__m68k_state->PC) : 0); last_addr = addr; last_val = rdata; }
    else { kprintf("[SCSI] W %08x <- %02x (reg %u) PC %08x\n", addr, wdata & 0xff, (addr >> 4) & 7, __m68k_state ? BE32((uint32_t)(uintptr_t)__m68k_state->PC) : 0); last_addr = 0xffffffffu; }
}

static inline int on_service_core(void) { uint64_t id; __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(id)); return (id & 3) == 2; }
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
/* mac.bridge=: how the motherboard is reached. "gpio" is the classic PiStorm's CPLD through the Pi's
   GPIO (psmac_gpio.c), the default, because that is what a Pi plugged into an SE has; "semihost" and
   "uart" are the testbed's two ways to MAME's bridge CPU (QEMU passes both on its command line). */
static int use_gpio = 1;
/* mac.board=solo (psmac_board_solo.c): no motherboard at all; the kernel answers every chip itself */
static int transport_none = 0;
static int sh_files = 0;                /* mac.semihost=1: host files through semihosting without the bridge (the solo testbed) */
/* the board layer (psmac_board_se.c): mac.board=se puts the Macintosh SE's chips behind the IIci ROM's addresses */
extern int psmac_board_se, psmac_board_solo;
void psmac_solo_reset(void); void psmac_solo_init(void); void psmac_image_add(const char *, uint8_t *, uint32_t);
int psmac_se_access(uint32_t addr, int size, int write, uint32_t data, uint32_t *val, int posted);
unsigned psmac_se_ipl(unsigned raw); void psmac_se_service(void); void psmac_se_start(void); void psmac_se_options(const char *); void psmac_se_report(void);
int psmac_se_rbv_irq(void);
void psmac_gpio_init(void); uint32_t psmac_gpio_read(uint32_t, int); void psmac_gpio_write(uint32_t, int, uint32_t);
unsigned psmac_gpio_ipl(void); void psmac_gpio_pulse_reset(void); uint32_t psmac_gpio_timeouts(void); int psmac_gpio_reset_asserted(void);
static uint32_t peek_addr, peek_len;
static uint32_t pctrail_at;             /* mac.pctrail=N: dump the 68k PC ring at the Nth heartbeat (a loop that is not a stall) */
static uint32_t pcwatch;                /* mac.pcwatch=HEX: dump the ring the first time the sampled PC lands in that 256-byte page */
uint32_t psmac_dbf_slow = 1;            /* mac.dbfslow=N: device reads per turn of a self-looping dbf (M68k_LINE5.c) */    /* mac.peek=: Mac memory dumped at each heartbeat (debugging) */
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
static const char *psmac_bootargs(void)
{
    of_node_t *c = dt_find_node("/chosen");
    of_property_t *p = c ? dt_find_property(c, "bootargs") : NULL;
    return (p && p->op_value) ? (const char *)p->op_value : "";
}
/* exported for psmac_mbox.c (testbed backends) */
int64_t psmac_sh_open(const char *name, uint64_t mode) { return sh_open(name, mode); }
int     psmac_sh_read(int64_t fd, void *buf, uint64_t len) { return sh_read(fd, buf, len); }
int     psmac_sh_write(int64_t fd, const void *buf, uint64_t len) { return sh_write(fd, buf, len); }
int64_t psmac_sh_seek(int64_t fd, uint64_t pos) { uint64_t p[2] = { shw((uint64_t)fd), shw(pos) }; return semihost(0x0A, p); }
int64_t psmac_sh_flen(int64_t fd) { uint64_t p[1] = { shw((uint64_t)fd) }; return semihost(0x0C, p); }
int     psmac_sh_available(void) { return (use_semihost && !transport_dead) || sh_files; }
uint32_t psmac_mac_ram_mb(void) { return ram_size >> 20; }
/* value of "key=" in the boot arguments, copied into a static buffer (space-terminated); NULL if absent */
const char *psmac_option(const char *key)
{
    static char val[8][128]; static int slot;
    const char *args = psmac_bootargs();
    size_t kl = strlen(key);
    for (const char *s = args; s && *s; s++) {
        if ((s == args || s[-1] == ' ') && strncmp(s, key, kl) == 0 && s[kl] == '=') {
            char *out = val[slot = (slot + 1) & 7]; int n = 0;
            for (const char *v = s + kl + 1; *v && *v != ' ' && n < 127; v++) out[n++] = *v;
            out[n] = 0; return out;
        }
    }
    return NULL;
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
void psmac_select_transport(int semihost_mode) { use_semihost = semihost_mode; if (semihost_mode) use_gpio = 0; }

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
    psmac_boot_check_args(args);                         /* an option given twice: say which one counts */
    psmac_se_options(args);                              /* mac.board=se first: the screen default below depends on it */
    if ((tok = find_token(args, "mac.dbg="))) {   /* mac.dbg=LO,HI: print the translator's view of blocks starting in [LO,HI] */
        tok += 8; uint32_t lo = 0, hi = 0;
        while ((*tok >= '0' && *tok <= '9') || (*tok >= 'a' && *tok <= 'f')) { lo = lo * 16 + (*tok <= '9' ? *tok - '0' : *tok - 'a' + 10); tok++; }
        if (*tok == ',') { tok++; while ((*tok >= '0' && *tok <= '9') || (*tok >= 'a' && *tok <= 'f')) { hi = hi * 16 + (*tok <= '9' ? *tok - '0' : *tok - 'a' + 10); tok++; } }
        globals.debug_range_min = lo; globals.debug_range_max = hi; globals.debug = 1; globals.disasm = true;
    }
    if ((tok = find_token(args, "mac.inputtest="))) { tok += 14; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); extern void psmac_alice_input_test(uint32_t); psmac_alice_input_test(n); }
    if ((tok = find_token(args, "mac.inputabout="))) { tok += 15; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); extern void psmac_alice_input_about(uint32_t); psmac_alice_input_about(n); }
    if ((tok = find_token(args, "mac.screen="))) {      /* mac.screen=WxH: the second display's resolution */
        tok += 11; uint32_t w = 0, h = 0;
        while (*tok >= '0' && *tok <= '9') w = w * 10 + (*tok++ - '0');
        if (*tok == 'x') { tok++; while (*tok >= '0' && *tok <= '9') h = h * 10 + (*tok++ - '0'); }
        if (w >= 512 && w <= 1920 && h >= 342 && h <= 1200) { screen_w = w; screen_h = h; }
    }
    if (psmac_board_se && !psmac_board_solo && !screen_w) { screen_w = 512; screen_h = 342; }   /* the SE's own screen is the card's frame on that board */
    if ((tok = find_token(args, "mac.bustrace="))) { tok += 13; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); psmac_trace(n); bustrace_asked = 1; }
    if ((tok = find_token(args, "mac.console="))) { tok += 12; extern void psmac_console_option(int); psmac_console_option(strncmp(tok, "hdmi", 4) == 0); }
    if ((tok = find_token(args, "mac.dbfslow="))) { tok += 12; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); psmac_dbf_slow = n > 8 ? 8 : n; }
    if ((tok = find_token(args, "mac.pcwatch="))) { tok += 12; uint32_t a = 0; while (1) { char c = *tok; uint32_t d; if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else if (c == 'x' || c == 'X') { tok++; continue; } else break; a = a * 16 + d; tok++; } pcwatch = a; }
    if ((tok = find_token(args, "mac.pctrail="))) { tok += 12; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); pctrail_at = n; }
    if ((tok = find_token(args, "mac.peek="))) {        /* mac.peek=HEXADDR,LEN: dump Mac memory at each heartbeat (debugging) */
        tok += 9; uint32_t a = 0, n = 0;
        while (1) { char c = *tok; uint32_t d; if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else if (c == 'x' || c == 'X') { tok++; continue; } else break; a = a * 16 + d; tok++; }
        if (*tok == ',') { tok++; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); }
        peek_addr = a; peek_len = n > 512 ? 512 : (n ? n : 64);
    }
    if ((tok = find_token(args, "mac.inputclick="))) {
        /* SECONDS,X,Y[,o],X,Y[,o]...: the points, each with an Option flag */
        int pts[24], n = 0; uint32_t secs = 0;
        tok += 15;
        while (*tok >= '0' && *tok <= '9') secs = secs * 10 + (uint32_t)(*tok++ - '0');
        while (*tok == ',' && n < 8) {
            int xy[2] = { 0, 0 };
            for (int k = 0; k < 2 && *tok == ','; k++) { tok++; while (*tok >= '0' && *tok <= '9') xy[k] = xy[k] * 10 + (*tok++ - '0'); }
            pts[3 * n] = xy[0]; pts[3 * n + 1] = xy[1]; pts[3 * n + 2] = 0;
            if (tok[0] == ',' && tok[1] == 'o') { pts[3 * n + 2] = 1; tok += 2; }
            n++;
        }
        extern void psmac_alice_input_click(uint32_t, const int *, int); psmac_alice_input_click(secs, pts, n);
    }
    if ((tok = find_token(args, "mac.inputdrag="))) { tok += 14; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); extern void psmac_alice_input_drag(uint32_t); psmac_alice_input_drag(n); }
    if ((tok = find_token(args, "mac.inputright="))) { tok += 15; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); extern void psmac_alice_input_right(uint32_t); psmac_alice_input_right(n); }
    if ((tok = find_token(args, "mac.inputwifi="))) { tok += 14; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); extern void psmac_alice_input_wifi(uint32_t); psmac_alice_input_wifi(n); }
    if ((tok = find_token(args, "mac.scsitrace="))) { tok += 14; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); scsi_trace_left = n; }
    if ((tok = find_token(args, "mac.ram="))) { tok += 8; uint32_t mb = 0; while (*tok >= '0' && *tok <= '9') mb = mb * 10 + (*tok++ - '0'); globals.psmac_ram_mb = mb; }
    if ((tok = find_token(args, "mac.bridge="))) {        /* find_token matches up to the '=': compare the value ourselves */
        tok += 11;
        if (strncmp(tok, "semihost", 8) == 0) { use_semihost = 1; use_gpio = 0; }
        else if (strncmp(tok, "uart", 4) == 0) { use_semihost = 0; use_gpio = 0; }
        else if (strncmp(tok, "gpio", 4) == 0) { use_semihost = 0; use_gpio = 1; }
    }
    if ((tok = find_token(args, "mac.semihost="))) {   /* host files without the bridge (the solo testbed): find the parameter block's byte order first */
        tok += 13;
        if (*tok == '1') { sh_swap = 0; int64_t fd = sh_open(":tt", 0); if (fd < 0) { sh_swap = 1; fd = sh_open(":tt", 0); } sh_files = fd >= 0; kprintf("[PSMAC] semihosting files %s (byte-swap %d)\n", sh_files ? "on" : "FAILED", sh_swap); }
    }
    if (psmac_board_solo) { use_gpio = 0; use_semihost = 0; transport_none = 1; psmac_solo_init(); }
    static char shown[1024];                             /* the WiFi password and the like as *** */
    psmac_redact(args, shown, sizeof shown);
    kprintf("[PSMAC] options: ram=%d MiB bridge=%s (bootargs: %s)\n", globals.psmac_ram_mb, transport_none ? "none (solo)" : use_gpio ? "gpio (classic PiStorm)" : use_semihost ? "semihost" : "uart", shown);
}

/* One bridge transaction. Must only run on the transport owner. */
/* The same transaction over the classic PiStorm's GPIO protocol. No bridge, no reply frame: a read
   is a 68000 bus cycle and the interrupt level is a status register away. The 68000 socket has 24
   address lines, so only the low 24 bits travel; what the IIci ROM's 32-bit addresses mean on an SE
   bus is the board layer's business (docs/12), not the wire's. A cycle the CPLD never finishes
   (no SE behind the Pi) is abandoned after the timeout and the transport declared dead, so the
   kernel still reaches its second screen and its log on the bench. */
static void gpio_xfer(uint8_t op, uint8_t size, uint8_t flags, uint32_t addr, uint32_t data, struct psmac_reply *r)
{
    (void)flags;
    if (!transport_up) { psmac_gpio_init(); transport_up = 1; }
    uint32_t before = psmac_gpio_timeouts(), value = 0xffffffffu;
    svc_op = op; svc_addr = addr; svc_phase = 1;
    switch (op) {
    case PSMAC_OP_READ:  value = psmac_gpio_read(addr, size); break;
    case PSMAC_OP_WRITE: psmac_gpio_write(addr, size, data); break;
    case PSMAC_OP_RESET_ACK: psmac_gpio_pulse_reset(); break;
    default: break;                                          /* POLL: the level below is the whole point */
    }
    unsigned ipl = psmac_gpio_ipl();
    svc_phase = 0;
    if (psmac_gpio_timeouts() != before) {
        if (++timeouts == 1) kprintf("[PSMAC:GPIO] the CPLD did not finish a bus cycle (op %d addr %08x): is the Pi in a Macintosh?\n", op, addr);
        if (timeouts >= 16) transport_dead = 1;
        ipl = 0;
    }
    last_ipl = ipl; last_flags = 0;
    if (__m68k_state) __m68k_state->INTF.IPL = psmac_board_se ? psmac_se_ipl(ipl) : ipl;
    if (r) { r->status = 0; r->ipl = ipl; r->flags = 0; r->data = value; }
}

static void bridge_xfer(uint8_t op, uint8_t size, uint8_t flags, uint32_t addr, uint32_t data, struct psmac_reply *r)
{
    if (transport_none) { if (op == PSMAC_OP_RESET_ACK) psmac_solo_reset(); if (r) { r->status = 0; r->ipl = 0; r->flags = 0; r->data = 0xffffffff; } return; }
    if (use_gpio) { if (!transport_dead) gpio_xfer(op, size, flags, addr, data, r); else if (r) { r->status = 0; r->ipl = 0; r->flags = 0; r->data = 0xffffffff; } return; }
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
    if (__m68k_state) __m68k_state->INTF.IPL = psmac_board_se ? psmac_se_ipl(rep[1]) : rep[1];
    if (r) { r->status = rep[0]; r->ipl = rep[1]; r->flags = rep[2]; r->data = rep[4] | (rep[5] << 8) | (rep[6] << 16) | ((uint32_t)rep[7] << 24); }
    if (rep[0]) { bus_errors++; if (bus_errors <= 8) kprintf("[PSMAC] bridge reported bus error #%u on op %u size %u addr %08x (68k PC %08x)\n", bus_errors, op, size, addr, __m68k_state ? BE32((uint32_t)(uintptr_t)__m68k_state->PC) : 0); }
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
/* for the crash record (psmac_crash.c): RAM or the ROM, wherever the 68k's map puts them */
uint32_t psmac_mac_rd32(uint32_t logical, int *ok)
{
    int valid; uint32_t ph = psmac_mmu_l2p(logical, &valid);
    if (valid && ph >= 0x40000000u && ph < 0x50000000u && !(ph & 3)) {
        *ok = 1; return *(const uint32_t *)(KERNEL_ALIAS + rom_phys + (ph & (PSMAC_ROM_SIZE - 1)));
    }
    return mac_rd32(logical, ok);
}
void psmac_pcring_tail(unsigned n)
{
    if (n > pcring_n) n = pcring_n;
    if (n > PCRING) n = PCRING;
    for (unsigned i = 0; i < n; i += 8) {
        kprintf("[CRASH]  ");
        for (unsigned k = i; k < i + 8 && k < n; k++) kprintf(" %08x", pcring[(pcring_n - n + k) % PCRING]);
        kprintf("\n");
    }
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

static void con_start_async(void), con_drain(void);   /* the serial log's ring (below, beside con_putc) */
void psmac_service_core(void)
{
    psmac_read_options();
    if (transport_none) transport_up = 1;
    else if (use_gpio) { psmac_gpio_init(); transport_up = 1; }
    else if (!use_semihost) uart_init(); else { if (!semihost_init()) transport_dead = 1; }
    service_running = 1;
    if ((use_gpio || transport_none) && !bustrace_asked) psmac_trace(0);   /* the first 400 bus cycles are the testbed's diagnostic; on a bench they bury the log, and on the HDMI console they cost the ROM minutes (mac.bustrace=N asks for them) */
    kprintf("[PSMAC] service core running (%s transport owner, IPL poll %s)\n", transport_none ? "no" : use_gpio ? "GPIO" : "bridge", transport_none ? "solo" : use_gpio ? "20 kHz" : "1 kHz");
    psmac_se_start();
    con_start_async();                        /* the serial log from a ring from now on (con_putc) */
    /* a status register read on the GPIO costs a microsecond, a bridge poll a round trip: poll accordingly */
    uint64_t period = cnt_frq() / (use_gpio ? 20000 : 1000), next_poll = cnt_now() + period;
    uint64_t hb_period = cnt_frq() * 5, next_hb = cnt_now() + hb_period; uint32_t hb_last = 0;
    uint32_t pc_last = 0; int stall_dumped = 0, exc_dumped = 0;
    uint32_t scsi_reads = 0, scsi_reads_seen = 0; uint64_t next_epoch = 0, next_name = 0;
    for (;;) {
        con_drain();                              /* the serial log (con_putc), first: the bus's turns below may continue */
        if (cnt_now() > next_hb) {
            svc_phase = 3;
            kprintf("[PSMAC] heartbeat: %u bus xfers (+%u, %u berr), last %08x, ipl %d%s | via %u scc %u scsi %u asc %u swim %u rbv %u fb %u other %u | PC %08x SR %04x\n", xfer_count, xfer_count - hb_last, bus_errors, last_addr, last_ipl, transport_none ? " (solo)" : transport_dead ? " TRANSPORT DEAD" : "",
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
            { extern void psmac_slot_irq_report(void); psmac_slot_irq_report(); }
            psmac_se_report();
            { extern void alice_net_report(void); alice_net_report(); }
            { extern void bt_pi_report(void); bt_pi_report(); }
            { extern void psmac_card_report(void); psmac_card_report(); }
            { int ok1, ok2, ok3; uint32_t t1 = mac_rd32(0xd00, &ok1), t2 = mac_rd32(0xb24, &ok2), t3 = mac_rd32(0xcea, &ok3);
              if (ok1 && ok2 && ok3) kprintf("[PSMAC]   timing globals: TimeDBRA %04x TimeSCCDB %04x TimeSCSIDB %04x TimeVIADB %04x\n", t1 >> 16, t1 & 0xffff, t2 >> 16, t3 >> 16); }
            if (peek_len) {                    /* mac.peek=: the long at $1B40 (KCHRPtr) and the bytes asked for */
                int ok; uint32_t k = mac_rd32(0x1B40, &ok);
                kprintf("[PSMAC]   peek: KCHRPtr %08x; %08x:", ok ? k : 0xdeaddead, peek_addr);
                for (uint32_t i = 0; i < peek_len; i += 4) { uint32_t v = mac_rd32(peek_addr + i, &ok); if ((i & 31) == 0 && i) kprintf("\n[PSMAC]   %08x:", peek_addr + i); kprintf(" %08x", ok ? v : 0xdeaddead); }
                kprintf("\n");
            }
            if (xfer_count == hb_last && !stall_dumped) { stall_dumped = 1; pcring_dump(); }
            { static uint32_t hb_n; if (++hb_n == pctrail_at) pcring_dump(); }
            psmac_mbox_heartbeat();
            if (no_declrom) kprintf("[PSMAC] *** NO DECLARATION ROM in the container: slot 9 is empty (no Pi screen, PiDisk or input) ***\n");
            hb_last = xfer_count; next_hb = cnt_now() + hb_period;
        }
        if (__m68k_state) {
            uint32_t pc = BE32((uint32_t)(uintptr_t)__m68k_state->PC);
            if (pc != pc_last) {
                pc_last = pc; pcring[pcring_n++ % PCRING] = pc;
                psmac_boot_pc(pc);                 /* the boot timeline's 68k phases (psmac_boot.c) */
                static int watched;
                if (pcwatch && !watched && (pc & ~0xffu) == (pcwatch & ~0xffu)) { watched = 1; kprintf("[PSMAC] PC reached %08x (mac.pcwatch): trail follows\n", pc); pcring_dump(); }
            }
        }
        if (__m68k_state && cnt_now() > next_name) {     /* the boot timeline's "Finder is running" (psmac_boot.c) */
            int ok1, ok2; uint32_t a = mac_rd32(0x910, &ok1), b = mac_rd32(0x914, &ok2);
            if (ok1 && ok2) { const uint8_t n[8] = { a >> 24, a >> 16, a >> 8, a, b >> 24, b >> 16, b >> 8, b }; psmac_boot_curapname(n); }
            next_name = cnt_now() + cnt_frq() / 10;
        }
        { extern void psmac_crash_poll(void); psmac_crash_poll(); }     /* the System error dialog's record (psmac_crash.c) */
        if (psmac_exc_flag && !exc_dumped) {
            exc_dumped = 1;
            kprintf("[PSMAC] first unexpected 68k exception (vector %03x): PC trail follows\n", psmac_exc_flag & 0xfff);
            pcring_dump();
        }
        /* the mailbox services run on core 3 (alice_psmac.c) */
        if (scsi_reads != scsi_reads_seen && cnt_now() > next_epoch) {
            /* data came in from the SCSI chip: code loaded from disk may sit where translated
               blocks were; make the JIT re-check RAM blocks on their next entry (M68k_Translator.c) */
            extern uint32_t psmac_icache_epoch;
            psmac_icache_epoch++; scsi_reads_seen = scsi_reads; next_epoch = cnt_now() + cnt_frq() / 1000;
        }
        if (__atomic_load_n(&slot.state, __ATOMIC_ACQUIRE) == SLOT_REQ) {
            xfer_count++; last_addr = slot.addr; dev_cnt[dev_class(slot.addr)]++;
            if (slot.op == PSMAC_OP_READ && dev_class(slot.addr) == 2) scsi_reads++;
            bridge_xfer(slot.op, slot.size, slot.flags, slot.addr, slot.data, &slot.rep);
            if (scsi_trace_left && dev_class(slot.addr) == 2) scsi_trace(slot.op, slot.addr, slot.data, slot.rep.data);
            __atomic_store_n(&slot.state, SLOT_DONE, __ATOMIC_RELEASE);
            continue;
        }
        if (!transport_dead && !transport_none && cnt_now() > next_poll) {
            bridge_xfer(PSMAC_OP_POLL, 0, 0, 0, 0, NULL);
            next_poll = cnt_now() + period;
        }
        psmac_se_service();                       /* the SE's screen, a few rows at a time */
        __asm__ volatile("yield");
    }
}

/* ---------------- the virtual slot-$9 interrupt ----------------
   A NuBus card interrupts the IIci through the RBV: each slot has a line, the RBV shows the lines
   in its slot interrupt flag register (SIFR, pseudo-VIA offset $02, a 0 bit = that slot's line is
   asserted, bit 0 = slot $9 ... bit 5 = slot $E), the slot interrupt enable register (SIER, $12,
   written with bit 7 set to enable the bits given, clear to disable them) gates them into the
   "any slot" bit (bit 1) of the interrupt flag register (IFR, $03), which the interrupt enable
   register (IER, $13) gates onto the 68030's level 2. The ROM's level-2 handler reads IFR, then
   SIFR, and for each asserted slot runs the handlers queued with the Slot Manager's SIntInstall
   (Inside Macintosh: Devices, "Slot Manager"; Designing Cards and Drivers, "Interrupts").
   The Pi's card does not exist on the motherboard, so the kernel adds its line here: the service
   core raises slot_line while a board has something the Mac has not taken (level-sensitive, as
   on a real card), the Mac's SIER/IER writes are snooped as they pass through to the bridge, SIFR
   and IFR reads coming back from the bridge have the slot-9 bit merged in, and the interrupt
   level the JIT sees is raised to 2 while the line is up and enabled. MAME's own RBV never sees
   the line (it is not wired to a slot) and keeps serving the real registers. */
static volatile uint8_t slot_line, rbv_sier, rbv_ier;
void psmac_slot_irq(int on) { slot_line = on ? 1 : 0; }
/* The line as it is this instant, not as core 3 last sampled it. The ROM's level-2 handler reads
   the slot flags again right after a handler returns; if the line it sees is a stale copy still
   up, it dispatches once more, the handler finds nothing pending, and the ROM declares the
   interrupt unserviceable (system error 51, the first Sad Mac on the SE, docs/08 item 28). On the
   IIci the input driver's catch-all handler happened to be installed first and hid this. */
extern int psmac_slot_line_now(void);
static inline int line_now(void) { int l = psmac_slot_line_now(); if (l >= 0) slot_line = l; return slot_line; }
uint32_t psmac_slot_irq_state(void) { return (uint32_t)line_now() << 16 | (uint32_t)rbv_sier << 8 | rbv_ier; }
static inline int slot_irq_pending(void) { return line_now() && (rbv_sier & 0x01) && (rbv_ier & 0x02); }
static inline int rbv_reg(uint32_t addr)              /* pseudo-VIA register index, or -1 */
{
    if ((addr & 0xff0fe000u) != 0x50026000u) return -1;
    uint32_t off = addr & 0x1fff;
    if (off < 0x100) return (int)off;
    switch (off >> 9) { case 13: return 0x03; case 14: return 0x13; default: return -1; }
}
static uint32_t rbv_merge_read(uint32_t addr, int size, uint32_t data)
{
    if (size != 1) return data;
    switch (rbv_reg(addr)) {
    case 0x02: return line_now() ? (data & ~0x01u) : data;                      /* SIFR: 0 = asserted */
    case 0x03: return slot_irq_pending() ? (data | 0x82) : data;                /* IFR: any slot + IRQ */
    case 0x12: return (data & ~0x01u) | (rbv_sier & 0x01);                      /* SIER: our enable bit */
    default:   return data;
    }
}
static void rbv_snoop_write(uint32_t addr, int size, uint32_t data)
{
    if (size != 1) return;
    switch (rbv_reg(addr)) {
    case 0x12: if (data & 0x80) rbv_sier |= data & 0x7f; else rbv_sier &= ~(data & 0x7f); break;
    case 0x13: if (data & 0x80) rbv_ier |= data & 0x7f; else rbv_ier &= ~(data & 0x7f); break;
    default: break;
    }
}

/* ---------------- I/O helpers ---------------- */
uint32_t psmac_io_read(uint32_t addr, int size)
{
    struct psmac_reply r;
    if (psmac_board_se) { uint32_t v = 0; if (psmac_se_access(addr, size, 0, 0, &v, 0)) return v; }
    xfer(PSMAC_OP_READ, size, 0, addr, 0, &r);
    return rbv_merge_read(addr, size, r.data);
}
void psmac_io_write(uint32_t addr, int size, uint32_t data, int posted)
{
    if (psmac_board_se) { uint32_t v; if (psmac_se_access(addr, size, 1, data, &v, posted)) return; }
    rbv_snoop_write(addr, size, data);
    xfer(PSMAC_OP_WRITE, size, posted ? PSMAC_FLAG_POSTED : 0, addr, data, NULL);
}
/* the wire itself, for the board layer: an address already in the SE's own terms. From the JIT
   core it goes through the service core's slot; from the service core it is direct. */
uint32_t psmac_wire_read(uint32_t addr, int size)
{
    struct psmac_reply r;
    if (service_running && on_service_core()) bridge_xfer(PSMAC_OP_READ, size, 0, addr, 0, &r); else xfer(PSMAC_OP_READ, size, 0, addr, 0, &r);
    return r.data;
}
void psmac_wire_write(uint32_t addr, int size, uint32_t data, int posted)
{
    if (service_running && on_service_core()) bridge_xfer(PSMAC_OP_WRITE, size, posted ? PSMAC_FLAG_POSTED : 0, addr, data, NULL);
    else xfer(PSMAC_OP_WRITE, size, posted ? PSMAC_FLAG_POSTED : 0, addr, data, NULL);
}


/* Called from the data abort handler for every access the ARM MMU did not satisfy. Return 1 = handled. */
static uint32_t trace_left = 400;
void psmac_trace(uint32_t n) { trace_left = n; }
uint64_t psmac_fault_pc, psmac_fault_a7;   /* JIT's 68k PC (block granularity) and A7 at the fault */
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
        uint32_t pc68 = (uint32_t)psmac_fault_pc;
        int ram_test = pc68 >= 0x40844000u && pc68 < 0x40846000u;      /* ROM memory test sweeps all of RAM */
        uint32_t oldv = *(uint32_t *)p, oldtop = oldv >> 24;
        int old_ok = (oldtop == 0x40) || (oldtop == 0x00 && oldv != 0);   /* entry currently holds a plausible address */
        if (reports < 40 && !ram_test && off >= 0x400 && off < 0x800 && size == 4 && old_ok && top != 0x00 && top != 0x40 && (uint32_t)value != 0xffffffffu) {
            reports++;
            kprintf("[PSMAC:WATCH] W%d trap table +%03x <- %08x from 68k PC %08x A7 %08x (ARM ELR %p)\n", size, off, (uint32_t)value, (uint32_t)psmac_fault_pc, (uint32_t)psmac_fault_a7, (void *)psmac_fault_elr);
            if (reports <= 3) {
                uint32_t sp = (uint32_t)psmac_fault_a7; int ok;
                kprintf("[PSMAC:WATCH]   stack:");
                for (int i = 0; i < 24; i++) { uint32_t v = mac_rd32(sp + 4 * i, &ok); kprintf(" %08x", ok ? v : 0xdeaddead); }
                kprintf("\n");
            }
        }
        switch (size) {
            case 1: *p = value; break; case 2: *(uint16_t *)p = value; break; case 4: *(uint32_t *)p = value; break;
            case 8: *(uint64_t *)p = value; break; case 16: *(uint64_t *)p = value; *(uint64_t *)(p + 8) = value2; break;
            default: return 0;
        }
        return 1;
    }
    if (far < PSMAC_FB_SIZE) {
        /* write-through frame buffer: local copy + mirror to the motherboard (the IIci's RBV scans
           bank A). On the SE board the local copy only: its screen is fed by the board layer. The
           first SE boots lost these writes altogether and failed the ROM's RAM test (fs176). */
        uint8_t *p = (uint8_t *)(KERNEL_ALIAS + ram_phys + far);
        int m = !psmac_board_se;
        switch (size) {
            case 1: *p = value; if (m) psmac_io_write(far, 1, value & 0xff, 1); break;
            case 2: *(uint16_t *)p = value; if (m) psmac_io_write(far, 2, value & 0xffff, 1); break;
            case 4: *(uint32_t *)p = value; if (m) psmac_io_write(far, 4, value, 1); break;
            case 8: *(uint64_t *)p = value; if (m) { psmac_io_write(far, 4, value >> 32, 1); psmac_io_write(far + 4, 4, value, 1); } break;
            case 16: *(uint64_t *)p = value; *(uint64_t *)(p + 8) = value2;
                     if (m) { psmac_io_write(far, 4, value >> 32, 1); psmac_io_write(far + 4, 4, value, 1);
                              psmac_io_write(far + 8, 4, value2 >> 32, 1); psmac_io_write(far + 12, 4, value2, 1); } break;
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
/* ---------------- the initramfs container ----------------
   The Pi firmware loads exactly one file besides the kernel (initramfs in config.txt), so
   everything the kernel needs before the Mac can help travels inside it. Emu68 on the Amiga uses
   that slot for the Kickstart ROM; the first days of this project did the same with the bare
   IIci ROM. Hardware needs more: the WiFi firmware (brcmfmac43455-sdio.bin, its NVRAM .txt and
   .clm_blob), the Bluetooth firmware (BCM4345C0.hcd) and the declaration ROM. Rather than invent a
   format, the container is a cpio archive in the "newc" format (the same one Linux initramfs
   images use, so `cpio -o -H newc` and Python both write it, and `cpio -t` lists it):
     *.rom                  the Macintosh ROM (one; the last one wins)
     *.declrom              the slot-$9 declaration ROM
     anything else          handed to the Alice68 modules as a firmware file, by its base name
   A container that does not start with the cpio magic is taken to be a bare ROM, as before. */
struct container_result { const uint8_t *rom; uint32_t rom_len; const uint8_t *declrom; uint32_t declrom_len; int declrom_sized; };
/* "...-WxH.declrom": the resolution a declaration ROM was built for (0 if the name has none) */
static int declrom_res(const char *name, uint32_t *w, uint32_t *h)
{
    int n = 0; while (name[n]) n++;
    int i = n - 1; while (i >= 0 && name[i] != '-') i--;
    if (i < 0) return 0;
    const char *p = name + i + 1; uint32_t a = 0, b = 0;
    while (*p >= '0' && *p <= '9') a = a * 10 + (*p++ - '0');
    if (*p++ != 'x') return 0;
    while (*p >= '0' && *p <= '9') b = b * 10 + (*p++ - '0');
    if (!a || !b) return 0;
    *w = a; *h = b; return 1;
}
static uint32_t hex8(const char *p) { uint32_t v = 0; for (int i = 0; i < 8; i++) { char c = p[i]; v = v * 16 + (c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'); } return v; }
static int bytes_eq(const void *a, const char *b, uint32_t n) { const uint8_t *x = a; for (uint32_t i = 0; i < n; i++) if (x[i] != (uint8_t)b[i]) return 0; return 1; }
static int ends_with(const char *name, const char *suffix)
{
    uint32_t n = 0, m = 0; while (name[n]) n++; while (suffix[m]) m++;
    if (m > n) return 0;
    for (uint32_t i = 0; i < m; i++) if (name[n - m + i] != suffix[i]) return 0;
    return 1;
}
static void walk_container(const uint8_t *img, uint32_t size, struct container_result *r)
{
    r->rom = img; r->rom_len = size; r->declrom = NULL; r->declrom_len = 0; r->declrom_sized = 0;
    extern uint32_t fb_width, fb_height;
    uint32_t want_w = screen_w ? screen_w : fb_width, want_h = screen_h ? screen_h : fb_height;
    struct { const uint8_t *p; uint32_t len, w, h; const char *name; } plain = { 0, 0, 0, 0, 0 }, sized_one = { 0, 0, 0, 0, 0 };
    if (size < 110 || !bytes_eq(img, "070701", 6)) return;         /* a bare ROM */
    { extern void psmac_card_container(const uint8_t *, uint32_t); psmac_card_container(img, size); }
    r->rom = NULL; r->rom_len = 0;
    uint32_t off = 0;
    while (off + 110 <= size && bytes_eq(img + off, "070701", 6)) {
        const char *h = (const char *)(img + off);
        uint32_t fsize = hex8(h + 54), nsize = hex8(h + 94);
        const char *name = h + 110;
        uint32_t data = (off + 110 + nsize + 3) & ~3u;
        if (data + fsize > size) break;
        if (nsize == 11 && bytes_eq(name, "TRAILER!!!", 10)) break;
        const char *base = name; for (const char *p = name; *p; p++) if (*p == '/') base = p + 1;
        if (fsize) {
            if (ends_with(base, ".rom"))          { r->rom = img + data; r->rom_len = fsize; kprintf("[PSMAC] container: ROM %s, %u bytes\n", base, fsize); }
            else if (ends_with(base, ".declrom")) {
                /* one declaration ROM per screen size may be in the container: take the one whose name
                   ends in the second display's WxH, else the plain one (docs/10) */
                uint32_t w, h; int sized = declrom_res(base, &w, &h);
                int take = sized && w == want_w && h == want_h;
                if (take) { r->declrom = img + data; r->declrom_len = fsize; r->declrom_sized = sized; }
                if (!sized && !plain.p) { plain.p = img + data; plain.len = fsize; plain.name = base; }
                if (sized && !sized_one.p) { sized_one.p = img + data; sized_one.len = fsize; sized_one.w = w; sized_one.h = h; sized_one.name = base; }
                kprintf("[PSMAC] container: declaration ROM %s, %u bytes%s\n", base, fsize, take ? " (taken)" : "");
            }
            else if (ends_with(base, ".img") || ends_with(base, ".hda") || ends_with(base, ".image") || ends_with(base, ".hfv") || ends_with(base, ".scsi")) {
                /* a disk image: the solo board's SCSI disk, or PiDisk's volume on the other boards; it
                   lives in the initramfs copy start.c made on the heap, and is written there */
                psmac_image_add(base, (uint8_t *)(img + data), fsize);
                { extern void psmac_card_track(const char *, const uint8_t *, uint32_t, uint32_t); psmac_card_track(base, img + data, data, fsize); }
                kprintf("[PSMAC] container: disk image %s, %u bytes (%u MB)\n", base, fsize, fsize >> 20);
            }
            else { extern int alice_fw_preload(const char *, const uint8_t *, uint32_t); alice_fw_preload(base, img + data, fsize); }
        }
        off = (data + fsize + 3) & ~3u;
    }
    /* No declaration ROM of the screen's size. Until 19 November nothing was taken then, slot 9 stayed
       empty, no video, disk or input driver ever opened, and the user's Pi 3B ran the Mac blind with
       the log on HDMI (a 1920x1200 ROM with mac.screen=1920x1080), the only sign the missing
       "(taken)". Now the plain ROM is taken, else a sized one, and the screen takes the size the ROM
       describes, since that is the only size the Mac can use: the VideoCore is asked for it
       (psmac_display_init, called after this). The SE board keeps its own 512x342. */
    if (!r->declrom && (plain.p || sized_one.p)) {
        int keep_size = psmac_board_se && !psmac_board_solo;
        uint32_t w = plain.p ? 640 : sized_one.w, h = plain.p ? 480 : sized_one.h;   /* the plain ROM is mkdeclrom.py's default, 640x480 */
        const char *nm = plain.p ? plain.name : sized_one.name;
        r->declrom = plain.p ? plain.p : sized_one.p; r->declrom_len = plain.p ? plain.len : sized_one.len; r->declrom_sized = !plain.p;
        if (!sized_one.p || keep_size || (w == want_w && h == want_h))
            kprintf("[PSMAC] container: declaration ROM %s taken\n", nm);
        else {
            kprintf("[PSMAC] container: no declaration ROM for %ux%u; using %s, so the screen is %ux%u (set mac.screen=%ux%u or add the %ux%u ROM)\n",
                    want_w, want_h, nm, w, h, w, h, want_w, want_h);
            screen_w = w; screen_h = h;
        }
    }
}

/* ---- the wired network card, shown only where there is one ------------------------------------------
   The declaration ROM lists the wired Ethernet card (sResource $81, Alice68 unit 1) before the WiFi
   ($83, unit 0) on the one slot-9 card, and MacTCP, Riptide's defaults and the Network control
   panel's EtherTalk take the first Ethernet-type sResource the Slot Manager returns. On 19 November a
   Pi 3B's wired port was driven by nothing, Riptide bound the dead card and its pings found nobody,
   so e5ff17d took $81 out of the declaration ROM in RAM on every Pi without a GENET. Later the
   same day the Pi 3B's LAN9514 and the 3B+'s LAN7800 are driven (psmac_usbnet.c, docs/08 item 49),
   and hiding the card is the exception again:
       Pi 4, 400, CM4      GENET (alice_net.c)          $81 kept
       Pi 3B, 2B v1.2      LAN9514 on USB (smsc95xx)    $81 kept
       Pi 3B+              LAN7800 on USB (lan78xx)     $81 kept
       Pi 3A+, Zero 2 W    none                         $81 left out: the WiFi is the Mac's first card
       eth=off             (any board)                  $81 left out
   The testbed's file bridge (mac.net=) counts as a wired port. The board comes from the device tree
   (psmac_board_wired: the ethernet0 alias's compatible, else the board's model), so this is decided
   before the 68k starts, as it must be: the Slot Manager reads the ROM once, at the first boot, long
   before the USB bus is enumerated. A chip that is then not found makes unit 1 report no unit, so the
   card's driver fails its Open as with eth=off. Lagoon EtherTalk with $81 in parameter RAM ($E0) and
   no wired card falls back to the first card it finds, the WiFi. */
int psmac_board_wired(void);
static int wired_ethernet_works(void)
{
    const char *args = psmac_bootargs();
    for (const char *s = args; s && *s; s++)
        if ((s == args || s[-1] == ' ') && strncmp(s, "eth=off", 7) == 0 && (s[7] == 0 || s[7] == ' ')) return 0;
    if (psmac_option("mac.net")) return 1;
    return psmac_board_wired() != 0;
}
/* drop one sResource from the declaration ROM whose format block ends at top: the entries after it
   move up four bytes (their offsets are relative to their own address, so each grows by four),
   and the CRC is made again over the image (declrom.py's rule: rol 1, add the byte, the CRC field
   as zero) */
static int declrom_drop(uint8_t *top, uint8_t rid, uint32_t want_type_lo)
{
    uint8_t *fb = top - 20;
    int32_t dir_off = (int32_t)(BE32(*(uint32_t *)fb) << 8) >> 8;
    uint32_t length = BE32(*(uint32_t *)(fb + 4));
    if (length < 20 || length > PSMB_DECLROM_SIZE) return 0;
    uint8_t *e = fb + dir_off;
    while (e >= top - length && e < fb && e[0] != 0xff && e[0] != rid) e += 4;
    if (e < top - length || e >= fb || e[0] != rid) return 0;
    {   /* only a network card of ours: sRsrcType (entry 1) {catNetwork 4, typeEtherNet 1, 1, 'PS'} */
        int32_t lo = (int32_t)(BE32(*(uint32_t *)e) << 8) >> 8;
        uint8_t *list = e + lo, *t = 0;
        for (uint8_t *l = list; l >= top - length && l < fb && l[0] != 0xff; l += 4)
            if (l[0] == 1) { t = l + ((int32_t)(BE32(*(uint32_t *)l) << 8) >> 8); break; }
        if (!t || BE32(*(uint32_t *)t) != 0x00040001u || BE32(*(uint32_t *)(t + 4)) != want_type_lo) return 0;
    }
    for (; e[4] != 0xff; e += 4) {
        uint32_t next = BE32(*(uint32_t *)(e + 4));
        int32_t off = ((int32_t)(next << 8) >> 8) + 4;
        *(uint32_t *)e = BE32((next & 0xff000000u) | ((uint32_t)off & 0xffffffu));
    }
    *(uint32_t *)e = BE32(0xff000000u);                 /* the end mark, one entry earlier */
    *(uint32_t *)(e + 4) = 0;
    *(uint32_t *)(fb + 8) = 0;
    uint32_t crc = 0;
    for (uint8_t *p = top - length; p < top; p++) crc = ((crc << 1) | (crc >> 31)) + *p;
    *(uint32_t *)(fb + 8) = BE32(crc);
    return 1;
}

void psmac_setup_memory(void *rom, uint32_t rom_sz)
{
    psmac_read_options();
    uintptr_t base = (sys_memory[0].mb_Base + 0x1fffff) & ~0x1fffffull;
    const uintptr_t extra = PSMB_WIN_SIZE + PSMB_DECLROM_SIZE + PSMB_FB_BIG_SIZE;    /* slot $9 mailbox window + declaration ROM + the big frame buffer */
    uint32_t mb = globals.psmac_ram_mb;
    if (!mb) {
        /* No mac.ram=: take what the Pi can spare. The kernel keeps PSMAC_PI_KEEP_MB for its own
           pools (the JIT cache, the service core's buffers, firmware); the rest is the Mac's, up
           to PSMAC_RAM_DEFAULT_MB, which is what the IIci ROM sizes without a memory patch (two
           banks of 64 MiB, docs/08 item 11). A Pi Zero 2 W or 3A+ (512 MiB) and a Pi 4 (2-8 GiB)
           both end up with 128 MiB; smaller boards get what is left. Sizes that are not a full
           bank or a full bank A plus part of bank B still work (the shim aliases what is
           missing, psmac_mmu.c), but a real IIci never had them, so the rounding stays at 8. */
        uintptr_t avail = sys_memory[0].mb_Base + sys_memory[0].mb_Size - base;
        uintptr_t spare = avail > ((uintptr_t)PSMAC_PI_KEEP_MB << 20) ? avail - ((uintptr_t)PSMAC_PI_KEEP_MB << 20) : 0;
        mb = (uint32_t)(spare >> 20) & ~7u;
        if (mb > PSMAC_RAM_DEFAULT_MB) mb = PSMAC_RAM_DEFAULT_MB;
        if (mb < 8) mb = 8;
        kprintf("[PSMAC] no mac.ram=: %u MiB of the Pi's %u MiB free go to the Mac\n", mb, (uint32_t)(avail >> 20));
    }
    if (mb > PSMAC_RAM_MAX_MB) { kprintf("[PSMAC] mac.ram=%d clamped to %d MiB (RAM space below the ROM)\n", mb, PSMAC_RAM_MAX_MB); mb = PSMAC_RAM_MAX_MB; }
    if (mb < 1) mb = 1;
    ram_size = mb << 20;
    uintptr_t need = ram_size + PSMAC_ROM_SIZE + extra;
    if (sys_memory[0].mb_Base + sys_memory[0].mb_Size < base + need) {
        kprintf("[PSMAC] not enough system memory for %d MiB Mac RAM, falling back to 8 MiB\n", mb);
        ram_size = 8 << 20; need = ram_size + PSMAC_ROM_SIZE + extra;
    }
    ram_phys = base; rom_phys = base + ram_size;
    win_phys = rom_phys + PSMAC_ROM_SIZE; declrom_phys = win_phys + PSMB_WIN_SIZE; fb_big_phys = declrom_phys + PSMB_DECLROM_SIZE;
    sys_memory[0].mb_Size -= (base + need) - sys_memory[0].mb_Base;
    sys_memory[0].mb_Base = base + need;

    uint64_t parse_t0 = psmac_boot_now_us();
    struct container_result cont; walk_container((const uint8_t *)rom, rom_sz, &cont);
    psmac_boot_mark("container parsed: %u KiB in %u us", rom_sz >> 10, (uint32_t)(psmac_boot_now_us() - parse_t0));
    rom = (void *)cont.rom; rom_sz = cont.rom_len;
    kprintf("[PSMAC] Mac RAM %d MiB at Pi %p, ROM at Pi %p (%d bytes from initramfs)\n", ram_size >> 20, ram_phys, rom_phys, rom_sz);
    { extern void psmac_solo_start(void); psmac_solo_start(); }      /* the solo board's disks, now that the container is read */


    /* ROM image */
    uint8_t *romk = (uint8_t *)(KERNEL_ALIAS + rom_phys);
    if (rom && rom_sz) {
        uint32_t n = rom_sz > PSMAC_ROM_SIZE ? PSMAC_ROM_SIZE : rom_sz;
        memcpy(romk, rom, n);
        /* A stock IIci ROM in the container: patch it here (psmac_rom_delta.h, the assembled
           rom/patches/iici.json: the timing calibration taken from the Pi, docs/06). The checksum
           word is part of the delta; the ROM's own test (test 1 at boot) then passes. Nothing on
           the SD card needs patching by hand, which the first user asked for. */
        if (n == PSMAC_ROM_SIZE && BE32(*(uint32_t *)romk) == PSMAC_ROM_DELTA_IN) {
            for (unsigned i = 0; i < sizeof(psmac_rom_delta) / sizeof(psmac_rom_delta[0]); i++)
                memcpy(romk + psmac_rom_delta[i].off, psmac_rom_delta[i].bytes, psmac_rom_delta[i].len);
            uint32_t sum = 0;
            for (uint32_t o = 4; o < PSMAC_ROM_SIZE; o += 2) sum += BE16(*(uint16_t *)(romk + o));
            kprintf("[PSMAC] stock IIci ROM %08x in the container: patched in memory, checksum word %08x (%s)\n",
                    PSMAC_ROM_DELTA_IN, BE32(*(uint32_t *)romk), sum == PSMAC_ROM_DELTA_OUT ? "verified" : "MISMATCH");
        }
        for (uint32_t o = n; o < PSMAC_ROM_SIZE; o += n) {
            memcpy(romk + o, rom, (o + n <= PSMAC_ROM_SIZE) ? n : PSMAC_ROM_SIZE - o);
        }
    } else {
        kprintf("[PSMAC] WARNING: no ROM in initramfs!\n");
    }
    /* identity mapping (MMU disabled) and the 68030 MMU shadowing machinery */
    psmac_mmu_set_slot(win_phys, declrom_phys);
    psmac_mmu_set_fb(fb_big_phys);
    psmac_mmu_init(ram_phys, ram_size, rom_phys);
    {   /* declaration ROM image (testbed: host file via semihosting; hardware: SD card later) */
        uint8_t *dr = (uint8_t *)(KERNEL_ALIAS + declrom_phys);
        for (uint32_t i = 0; i < PSMB_DECLROM_SIZE; i += 8) *(uint64_t *)(dr + i) = 0;
        const char *path = psmac_option("mac.declrom");
        if (cont.declrom && cont.declrom_len <= PSMB_DECLROM_SIZE) {
            memcpy(dr + PSMB_DECLROM_SIZE - cont.declrom_len, cont.declrom, cont.declrom_len);
            kprintf("[PSMAC] declaration ROM from the container: %u bytes at Mac 0x%08x\n", cont.declrom_len, PSMB_DECLROM_PHYS + PSMB_DECLROM_SIZE - cont.declrom_len);
        } else if (path && (use_semihost || sh_files)) {
            int64_t fd = sh_open(path, 1);
            int64_t len = fd >= 0 ? psmac_sh_flen(fd) : -1;
            if (len > 0 && len <= (int64_t)PSMB_DECLROM_SIZE) {
                /* the format block must end at the top of the slot space: right-align the image */
                if (sh_read(fd, dr + PSMB_DECLROM_SIZE - len, len)) kprintf("[PSMAC] declaration ROM %s: %d bytes at Mac 0x%08x\n", path, (int)len, PSMB_DECLROM_PHYS + PSMB_DECLROM_SIZE - (uint32_t)len);
            } else { kprintf("[PSMAC] declaration ROM %s not loaded (len %d)\n", path, (int)len); no_declrom = 1; }
        } else no_declrom = 1;
        if (no_declrom)
            kprintf("[PSMAC] ********************************************************************************\n"
                    "[PSMAC] *** NO DECLARATION ROM: put aurora68.declrom (or the one for your screen size) ***\n"
                    "[PSMAC] *** in the container. Without it the Mac has no Pi screen, PiDisk or input.  ***\n"
                    "[PSMAC] ********************************************************************************\n");
        if (!wired_ethernet_works() && declrom_drop(dr + PSMB_DECLROM_SIZE, 0x81, 0x00015053u))   /* drSwApple 1, drHw 'PS' */
            kprintf("[PSMAC] declaration ROM: the wired Ethernet card ($81) left out, this Pi has no wired port (or eth=off); the WiFi is the Mac's network card\n");
        else if (wired_ethernet_works()) { extern const char *psmac_board_wired_name(void);
            kprintf("[PSMAC] declaration ROM: the wired Ethernet card ($81) and the WiFi ($83) both on slot 9; wired port: %s\n", psmac_option("mac.net") ? "the testbed's file bridge" : psmac_board_wired_name()); }
    }
    psmac_mbox_init(win_phys, ram_phys, ram_size);
    psmac_boot_mark("ROM, MMU shim, mailbox, boards and the second display's VideoCore buffer ready");

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
        { extern void psmac_mbox_set_calib(uint32_t); psmac_mbox_set_calib((uint32_t)(dt * 1000000 / cnt_frq() / 200)); }
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
void ps_setup_protocol(void) { kprintf("[PSMAC] motherboard transport: %s\n", transport_none ? "none: the kernel is the logic board (mac.board=solo)" : use_gpio ? "classic PiStorm CPLD over GPIO (mac.bridge=gpio)" : use_semihost ? "semihosting FIFOs -> MAME bridge" : "mini UART (QEMU -serial 1) -> MAME bridge"); }
void ps_reset_state_machine(void) {}
void ps_pulse_reset(void) { struct psmac_reply r; xfer(PSMAC_OP_RESET_ACK, 0, 0, 0, 0, &r); }
/* The 68k's RESET instruction (the ROM's first act on every boot, cold or after Restart): the
   motherboard's reset line, and the virtual card's (alice_psmac.c: its interrupt sources drop) */
void ps_send_reset(void) { extern void psmac_card_reset(void); psmac_card_reset(); ps_pulse_reset(); }
void ps_set_control(unsigned int v) { (void)v; }
void ps_clr_control(unsigned int v) { (void)v; }
static inline unsigned effective_ipl(void) { unsigned ipl = last_ipl; if (psmac_board_se) return psmac_se_ipl(ipl); if (slot_irq_pending() && ipl < 2) ipl = 2; return ipl; }
unsigned int ps_read_status_reg(void) { return effective_ipl() << 13; }
void ps_write_status_reg(unsigned int v) { (void)v; }
void ps_memtest(unsigned int s) { (void)s; }
void ps_buptest(unsigned int s, unsigned int m) { (void)s; (void)m; }
unsigned int ps_get_ipl_zero(void) { return effective_ipl() == 0; }
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
static int con_mini;                    /* the log goes to the mini UART: Bluetooth has the PL011 (bt_pi.c) */
/* A UART that does not send. On a Pi 3 with Bluetooth on the PL011 (no disable-bt overlay, the
   release's config.txt) the firmware leaves the PL011 switched off, so its FIFO stays full and every
   character waited the whole bound below, 7 to 10 ms on a Pi 3B: the 2 KB the kernel prints before
   the service core takes the log over held the boot for 15 seconds, most of it between "kernel
   moved" and "cores 1-3 started" (the user's log, 20 November 2026). Now one such wait marks the
   UART stuck, and from then on a character goes to it only if it has room at that moment, until it
   sends again. The HDMI screen and the Mac's log window get every character either way.
   mac.deaduart=1 makes the testbed's UART look full to the kernel (it still prints, so the log can
   be read); mac.deaduart=2 also waits for every character as the kernel used to, to measure it. */
static int con_stuck, con_dead_test = -1;
static int con_room(void)
{
    if (con_dead_test < 0 && dt_find_node("/")) { const char *o = psmac_option("mac.deaduart"); con_dead_test = o ? o[0] - '0' : 0; }
    int room = con_mini ? (rd32le(AUX_MU_LSR) & 0x20) != 0 : !(rd32le(PL011_BASE + 0x18) & (1 << 5));
    return room && con_dead_test <= 0;                      /* the test still reads the port: the wait costs what it did */
}
static void con_send(uint8_t c)
{
    /* bounded wait: a wedged UART must never deadlock the emulation (the console lock is shared by all cores) */
    if (!con_stuck || con_dead_test == 2)
        for (int i = 0; i < 100000 && !con_room(); i++) ;
    if (!con_room()) { con_stuck = 1; if (con_dead_test <= 0) return; }   /* the testbed's port still prints, to be read */
    else con_stuck = 0;
    if (con_mini) wr32le(AUX_MU_IO, c); else wr32le(PL011_BASE, c);
}
/* The serial line runs at 115200 baud, 11.5 characters a millisecond, and every kprintf used to wait
   for it: the 6 KB the kernel prints before the 68k starts held the boot for half a second on real
   hardware, and the service core's heartbeat stopped the bus for a sixth of a second every five
   (docs/08 item 50). Once the service core's loop runs, the log goes into a ring that the loop
   sends from, a character whenever the UART has room. A full ring is sent from by the writer
   itself, so nothing is lost, and before the loop runs (or with mac.synclog=1) every character is
   sent at once as before, so a kernel that dies early still says where. con_lock keeps the sender
   and the switch to the mini UART (Bluetooth's start) apart: a stray byte on the PL011 after the
   switch would go to the Bluetooth chip. */
static uint8_t con_ring[16384];
static volatile uint32_t con_head, con_tail;
static volatile int con_async;
static spinlock_t con_lock;
static int con_drain_one(void)
{
    if (con_tail == con_head || !con_room()) return 0;
    con_send(con_ring[con_tail % sizeof con_ring]);
    __asm__ volatile("dmb ish" ::: "memory");
    con_tail++;
    return 1;
}
static void con_putc(uint8_t c)
{
    if (!__atomic_load_n(&con_async, __ATOMIC_ACQUIRE)) { spinlock_acquire(&con_lock); con_send(c); spinlock_release(&con_lock); return; }
    while (con_head - con_tail >= sizeof con_ring) {                 /* full: send from it here */
        spinlock_acquire(&con_lock);
        if (con_head - con_tail >= sizeof con_ring && !con_drain_one()) { con_send(con_ring[con_tail % sizeof con_ring]); con_tail++; }
        spinlock_release(&con_lock);
    }
    con_ring[con_head % sizeof con_ring] = c;
    __asm__ volatile("dmb ish" ::: "memory");
    con_head++;
}
static void con_start_async(void)
{
    const char *o = psmac_option("mac.synclog");
    if (!o || o[0] != '1') __atomic_store_n(&con_async, 1, __ATOMIC_RELEASE);
    if (con_stuck) kprintf("[PSMAC] the serial port is not sending (on a Pi 3 the PL011 belongs to Bluetooth until it starts): the log is on the screen and in PiLog\n");
}
/* the service core's loop: a few characters per turn, never waiting on the UART */
static void con_drain(void)
{
    if (con_tail == con_head || !spinlock_try_acquire(&con_lock)) return;
    for (int i = 0; i < 16 && con_drain_one(); i++) ;
    spinlock_release(&con_lock);
}
/* A Pi 3 has one PL011 and the Bluetooth half of its WiFi chip hangs off it (GPIO 30-33). When the
   Bluetooth module takes it, the kernel's log moves to the mini UART on the same header pins (GPIO
   14/15 in ALT5, 115200 from the core clock, which enable_uart=1 keeps fixed); the HDMI console is
   unaffected. Called once by bt_pi.c before it touches the PL011. */
extern uint32_t get_clock_rate(uint32_t clock_id);
void psmac_console_miniuart(void)
{
    if (con_mini) return;
    uint32_t core = get_clock_rate(4);                                  /* CORE */
    if (core < 100000000u) core = 250000000u;
    spinlock_acquire(&con_lock);                                        /* the ring's sender stays off the PL011 from here */
    for (int i = 0; i < 100000 && !con_stuck && (rd32le(PL011_BASE + 0x18) & (1 << 3)); i++) ;   /* the PL011 drains what it has */
    extern spinlock_t psmac_gpfsel_lock;
    spinlock_acquire(&psmac_gpfsel_lock);
    uint32_t f = rd32le(PERIPH_VA + 0x200004);                          /* GPFSEL1: GPIO 14, 15 to ALT5 (TXD1, RXD1) */
    f = (f & ~(077u << 12)) | (022u << 12);
    wr32le(PERIPH_VA + 0x200004, f);
    spinlock_release(&psmac_gpfsel_lock);
    wr32le(AUX_ENABLES, rd32le(AUX_ENABLES) | 1);
    wr32le(AUX_MU_CNTL, 0);
    wr32le(AUX_MU_IER, 0);
    wr32le(AUX_MU_LCR, 3);
    wr32le(AUX_MU_MCR, 0);
    wr32le(AUX_MU_BAUD, core / (8u * 115200u) - 1u);
    wr32le(AUX_MU_IIR, 0xc6);
    wr32le(AUX_MU_CNTL, 2);                                             /* transmitter only */
    con_mini = 1;
    spinlock_release(&con_lock);
    kprintf("[PSMAC] the log continues on the mini UART (GPIO 14, 115200): Bluetooth has the PL011\n");
}
void psmac_console_putc(uint8_t c);     /* psmac_display.c: the same text on the HDMI screen */
/* The log for the Mac (psmb.h, struct psmb_log; mac/pilog.c reads it). Until the window exists
   the bytes go into a ring of the kernel's own, copied in by psmac_log_attach. The callers hold
   the console lock (kprintf) or are the one serial writer, so there is one writer at a time. */
static uint8_t log_early[PSMB_LOG_SIZE];
static uint32_t log_head;
static volatile uint8_t *log_win;            /* the window's struct psmb_log, once attached */
static void log_putc(uint8_t c)
{
    if (c == '\r') return;
    volatile uint8_t *w = log_win;
    if (!w) { log_early[log_head++ % PSMB_LOG_SIZE] = c; return; }
    w[PSMB_OFF_LOGDATA - PSMB_OFF_LOG + log_head % PSMB_LOG_SIZE] = c;
    log_head++;
    __asm__ volatile("dmb ishst" ::: "memory");
    *(volatile uint32_t *)(w + 16) = log_head;
}
void psmac_log_attach(uint8_t *win)
{
    volatile uint8_t *w = win + PSMB_OFF_LOG;
    uint32_t n = log_head < PSMB_LOG_SIZE ? log_head : PSMB_LOG_SIZE;
    for (uint32_t i = log_head - n; i != log_head; i++) w[PSMB_OFF_LOGDATA - PSMB_OFF_LOG + i % PSMB_LOG_SIZE] = log_early[i % PSMB_LOG_SIZE];
    *(volatile uint32_t *)(w + 0) = PSMB_LOG_MAGIC;
    *(volatile uint32_t *)(w + 4) = 1;
    *(volatile uint32_t *)(w + 8) = PSMB_LOG_SIZE;
    *(volatile uint32_t *)(w + 12) = PSMB_OFF_LOGDATA;
    *(volatile uint32_t *)(w + 16) = log_head;
    __asm__ volatile("dmb ish" ::: "memory");
    log_win = w;
}
/* psmac_card.c (aurora68.log): the ring's bytes, oldest first, at most max; returns how many */
uint32_t psmac_log_copy(uint8_t *out, uint32_t max)
{
    uint32_t head = __atomic_load_n(&log_head, __ATOMIC_ACQUIRE), n = head < PSMB_LOG_SIZE ? head : PSMB_LOG_SIZE;
    volatile uint8_t *w = log_win;
    if (n > max) n = max;
    for (uint32_t i = head - n, k = 0; i != head; i++, k++)
        out[k] = w ? w[PSMB_OFF_LOGDATA - PSMB_OFF_LOG + i % PSMB_LOG_SIZE] : log_early[i % PSMB_LOG_SIZE];
    return n;
}
void fastSerial_putByte(uint8_t b) { if (b == '\n') con_putc('\r'); con_putc(b); psmac_console_putc(b); log_putc(b); }
void bitbang_putByte(uint8_t b) { fastSerial_putByte(b); }
