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
void psmac_mmu_set_fb(uintptr_t f, uint32_t size);    /* psmac_mmu.c */
static uintptr_t fb_big_phys;              /* the second display's big frame buffer (psmb.h) ... */
static uint32_t fb_big_size = PSMB_FB_BIG_SIZE;   /* ... and its size: 9 MiB, more for a screen above 1920 x 1200 */
static uint32_t screen_w, screen_h;        /* mac.screen=WxH: the second display's size (0: the VideoCore's) */
static int no_declrom;                     /* no declaration ROM anywhere: said at every heartbeat, slot 9 is empty */
uintptr_t psmac_fb_big_phys(void) { return fb_big_phys; }
uint32_t psmac_fb_big_size(void) { return fb_big_size; }
/* mac.hdmi= on the SE board (psmac_read_options): what the HDMI output is when the SE's own screen is
   the Mac's first one. 1 screen (the default): the Mac's second screen, at mac.screen=WxH or the
   VideoCore's own size, with a declaration ROM psmac-se-WxH; 0 mirror: the SE's picture, as before
   20 November; 2 log: the kernel's log for the whole run (mac.console=hdmi says the same, on every board) */
int psmac_hdmi_mode = 1;
extern int psmac_board_se, psmac_board_solo;
/* the first screen's size (0: the VideoCore's): the SE's own on the SE board, else mac.screen */
void psmac_screen_size(uint32_t *w, uint32_t *h)
{
    if (psmac_board_se && !psmac_board_solo) { *w = 512; *h = 342; return; }
    *w = screen_w; *h = screen_h;
}
/* the HDMI screen's size when it is a screen of its own (the SE board): mac.screen (0: the VideoCore's) */
void psmac_hdmi_size(uint32_t *w, uint32_t *h) { *w = screen_w; *h = screen_h; }
static volatile int transport_up, transport_dead, service_running;
static volatile int svc_phase; /* 0 idle, 1 writing req, 2 reading reply, 3 heartbeat */
static volatile uint32_t svc_op, svc_addr;
static volatile uint8_t last_ipl, last_flags;
struct psmac_irqwatch psmac_irqwatch;      /* psmac.h: filled by ExecutionLoop.c's ProcessIRQ, printed at the heartbeat */
static uint32_t bus_errors, timeouts;
static volatile uint32_t xfer_count, last_addr;
static int buswait_on;                     /* mac.buswait=1: how long each of the 68k's requests waits (below) */
static volatile uint32_t wire_cycles;      /* every cycle the transport owner has put on the wire */
uint64_t psmac_fault_elr;   /* set by the abort dispatcher: who faulted? */
/* Emu68's own EL0 code (JIT helpers, debug prints) may legitimately touch the Pi peripherals, which share
   the 68k's address space at 0xF2000000-0xF3FFFFFF but are mapped EL1-only in this fork. Perform those
   accesses here; Mac (JIT-generated) code faulting on the same window is forwarded to the bridge. */
static inline int emu68_own_fault(void) { return (psmac_fault_elr >> 32) == 0xffffff80ull; }   /* Emu68 text; JIT code is at 0xffffffe0... */
static inline int in_periph_window(uint64_t a) { return a >= PERIPH_LOW && a < 0x40000000ull; }
static volatile uint32_t dev_cnt[8]; /* VIA1, SCC, SCSI, ASC, SWIM, RBV, FB, other */
extern int psmac_board_se, psmac_board_solo;
/* On the SE board the wire carries the SE's own addresses (psmac_board_se.c translated them), so
   they are classed by the SE's map: RAM below $400000 (the screen rows), the 5380 at $580000-
   $5FFFFF, the SCC at $800000-$BFFFFF, the IWM at $C00000-$DFFFFF, the VIA at $E80000-$EFFFFF.
   Until 20 November 2026 they were classed by the IIci's, and every cycle on the real SE counted
   as "other". */
static inline int dev_class_se(uint32_t a) { if (a < 0x400000) return 6; if (a >= 0x580000 && a < 0x600000) return 2; if (a >= 0x800000 && a < 0xC00000) return 1; if (a >= 0xC00000 && a < 0xE00000) return 4; if (a >= 0xE80000 && a < 0xF00000) return 0; return 7; }
static inline int dev_class(uint32_t a) { uint32_t o = a & 0x000fffff; if (psmac_board_se && !psmac_board_solo) return dev_class_se(a); if (a < 0x80000) return 6; if ((a & 0xff000000) != 0x50000000) return 7; if (o < 0x2000 || (o >= 0x40000 && o < 0x42000)) return 0; if (o >= 0x4000 && o < 0x6000) return 1; if ((o >= 0x10000 && o < 0x14000) || (o >= 0x6000 && o < 0x8000)) return 2; if (o >= 0x14000 && o < 0x16000) return 3; if (o >= 0x16000 && o < 0x18000) return 4; if (o >= 0x24000 && o < 0x28000) return 5; return 7; }

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
unsigned psmac_se_ipl(unsigned raw); void psmac_se_set_level(unsigned raw); void psmac_se_service(void); void psmac_se_start(void); void psmac_se_options(const char *); void psmac_se_report(void);
int psmac_se_rbv_irq(void);
uint32_t psmac_se_wire_write(int reset, uint32_t addr, uint32_t data); void psmac_se_sound(void);
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
        /* up to 4096 x 2560 since 20 November 2026 (1920 x 1200 before): the declaration ROM is rewritten
           for the size (declrom_resize) and the big region grows for it (psmb.h); a depth that does not
           fit at that size is left out */
        if (w >= PSMB_SCREEN_MIN_W && w <= PSMB_SCREEN_MAX_W && h >= PSMB_SCREEN_MIN_H && h <= PSMB_SCREEN_MAX_H) { screen_w = w; screen_h = h; }
        else kprintf("[PSMAC] mac.screen=%ux%u: outside %ux%u to %ux%u, not taken\n", w, h, PSMB_SCREEN_MIN_W, PSMB_SCREEN_MIN_H, PSMB_SCREEN_MAX_W, PSMB_SCREEN_MAX_H);
    }
    if ((tok = find_token(args, "mac.hdmi="))) {        /* mac.hdmi=screen|mirror|log: the SE board's HDMI output */
        tok += 9;
        if (!strncmp(tok, "mirror", 6)) psmac_hdmi_mode = 0;
        else if (!strncmp(tok, "screen", 6)) psmac_hdmi_mode = 1;
        else if (!strncmp(tok, "log", 3)) psmac_hdmi_mode = 2;
        else kprintf("[PSMAC] mac.hdmi=%.8s not understood: screen, mirror or log\n", tok);
    }
    if ((tok = find_token(args, "mac.bustrace="))) { tok += 13; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); psmac_trace(n); bustrace_asked = 1; }
    if ((tok = find_token(args, "mac.console="))) { tok += 12; if (!strncmp(tok, "hdmi", 4)) psmac_hdmi_mode = 2; }
    { extern void psmac_console_option(int); psmac_console_option(psmac_hdmi_mode == 2); }   /* the log on HDMI for the whole run */
    if ((tok = find_token(args, "mac.dbfslow="))) { tok += 12; uint32_t n = 0; while (*tok >= '0' && *tok <= '9') n = n * 10 + (*tok++ - '0'); psmac_dbf_slow = n > 8 ? 8 : n; }
    if ((tok = find_token(args, "mac.pcwatch="))) { tok += 12; uint32_t a = 0; while (1) { char c = *tok; uint32_t d; if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else if (c == 'x' || c == 'X') { tok++; continue; } else break; a = a * 16 + d; tok++; } pcwatch = a; }
    if ((tok = find_token(args, "mac.watchtraps="))) { extern int psmac_watch_traps; psmac_watch_traps = tok[15] == '1'; }   /* report odd stores into the trap tables (psmac_mmu.c) */
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
    if ((tok = find_token(args, "mac.buswait="))) buswait_on = tok[12] == '1';   /* the 68k's waits for the wire, at each heartbeat (bw_report) */
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

/* The CPLD's start (its state machine reset, the motherboard's RESET pulse: some 40 ms) once, by
   whichever core comes first. The service core brings it up as it starts, and on the SE board the
   board layer's first access from core 0 can arrive in the same moment: until 20 November 2026 both
   ran psmac_gpio_init side by side (the QEMU log showed it twice), two cores strobing the CPLD at
   once. The second now waits for the first. */
static void gpio_bring_up(void)
{
    static int claimed;
    if (__atomic_exchange_n(&claimed, 1, __ATOMIC_ACQ_REL)) {
        while (!transport_up)
            __asm__ volatile("yield");
        return;
    }
    psmac_gpio_init();
    transport_up = 1;
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
    if (!transport_up) gpio_bring_up();
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
    /* A cycle the CPLD did not finish is a bus error, reported as one (status 1, counted and logged as
       the MAME bridge's are) and not as a good cycle with $FF.. data. The link is given up only after 16
       such cycles in a row, a Pi with no Macintosh behind it; one that answers in between resets the
       count. Until 21 November 2026 any 16 over the whole run gave it up, and every reply said success
       (an outside review, docs/08 item 92). The 68k itself still sees the $FF.. data: raising a bus error
       exception in the guest is not done yet. */
    uint8_t status = 0;
    if (psmac_gpio_timeouts() != before) {
        status = 1;
        bus_errors++;
        if (++timeouts == 1 || bus_errors <= 8)
            kprintf("[PSMAC:GPIO] the CPLD did not finish a bus cycle (op %d addr %08x, %u in a row): %s\n", op, addr, timeouts,
                    bus_errors == 1 ? "a bus error (is the Pi in a Macintosh?)" : "a bus error");
        if (timeouts >= 16) { transport_dead = 1; kprintf("[PSMAC:GPIO] 16 bus cycles in a row unfinished: the link to the Macintosh is given up\n"); }
        ipl = 0;
    } else timeouts = 0;
    last_ipl = ipl; last_flags = 0;
    if (psmac_board_se) psmac_se_set_level(ipl); else if (__m68k_state) __m68k_state->INTF.IPL = ipl;   /* the SE board adds its RBV, under its lock (psmac_board_se.c) */
    if (r) { r->status = status; r->ipl = ipl; r->flags = 0; r->data = value; }
}

/* Posted writes through semihosting, gathered (testbed only, 20 November 2026). Every request was a
   SYS_WRITE trap of its own, some tens of microseconds each: the SE's speaker, 185 byte writes every
   half frame, took longer to write than the half took to play (docs/08 item 71). Posted writes need
   no answer, so they wait here, up to 64 of them, and go to the FIFO in one trap before the next
   request that does want an answer, when the buffer is full, and when the service core's loop runs
   out of work (psmac_bridge_flush). MAME reads the same 12-byte requests in the same order. */
static uint8_t sh_batch[64 * 12]; static uint32_t sh_batch_len;
static void sh_batch_flush(void)
{
    uint32_t n = sh_batch_len;
    sh_batch_len = 0;
    if (!transport_dead && !sh_write(sh_req, sh_batch, n)) { kprintf("[PSMAC] semihosting bridge write failed\n"); transport_dead = 1; }
}
void psmac_bridge_flush(void) { if (sh_batch_len) sh_batch_flush(); }
static void bridge_xfer(uint8_t op, uint8_t size, uint8_t flags, uint32_t addr, uint32_t data, struct psmac_reply *r)
{
    wire_cycles++;
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
        if (flags & PSMAC_FLAG_POSTED) {
            for (int i = 0; i < 12; i++) sh_batch[sh_batch_len++] = req[i];
            if (sh_batch_len == sizeof sh_batch) sh_batch_flush();
            svc_phase = 0;
            return;
        }
        if (sh_batch_len) sh_batch_flush();
        if (transport_dead || !sh_write(sh_req, req, 12)) { if (++timeouts == 1) kprintf("[PSMAC] semihosting bridge write failed\n"); transport_dead = 1; if (r) { r->status = 0; r->ipl = 0; r->flags = 0; r->data = 0xffffffff; } return; }
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
    if (psmac_board_se) psmac_se_set_level(rep[1]); else if (__m68k_state) __m68k_state->INTF.IPL = rep[1];
    if (r) { r->status = rep[0]; r->ipl = rep[1]; r->flags = rep[2]; r->data = rep[4] | (rep[5] << 8) | (rep[6] << 16) | ((uint32_t)rep[7] << 24); }
    if (rep[0]) { bus_errors++; if (bus_errors <= 8) kprintf("[PSMAC] bridge reported bus error #%u on op %u size %u addr %08x (68k PC %08x)\n", bus_errors, op, size, addr, __m68k_state ? BE32((uint32_t)(uintptr_t)__m68k_state->PC) : 0); }
}

/* ---------------- request hand-off to the service core ---------------- */
struct slot { volatile uint32_t state; uint8_t op, size, flags; uint32_t addr, data; struct psmac_reply rep;
              uint64_t posted_at; uint32_t wire_at; };   /* mac.buswait: when core 0 put it there, and the wire's count then */
static struct slot slot __attribute__((aligned(64)));
enum { SLOT_FREE = 0, SLOT_REQ = 1, SLOT_DONE = 2 };
static volatile uint8_t slot_lock;

static void xfer(uint8_t op, uint8_t size, uint8_t flags, uint32_t addr, uint32_t data, struct psmac_reply *r)
{
    if (!service_running) { bridge_xfer(op, size, flags, addr, data, r); return; }
    while (__atomic_test_and_set(&slot_lock, __ATOMIC_ACQUIRE)) __asm__ volatile("yield");
    slot.op = op; slot.size = size; slot.flags = flags; slot.addr = addr; slot.data = data;
    if (buswait_on) { slot.posted_at = cnt_now(); slot.wire_at = wire_cycles; }
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

/* ---------------- how long the 68k waits for the wire (mac.buswait=1) ----------------
   (21 November 2026, docs/08 item 93.) Core 0 stops at every bus cycle of the Mac's until the
   service core has carried it out, and the service core has errands of its own on the wire: the
   SE's speaker buffer (185 byte writes a half, item 71), the SE's screen rows (32 words each), the
   VIA polls of the speaker and the interrupt level. A request that arrives during an errand waits
   for it. Most of the Mac's cycles do not mind, but the floppy driver polls the IWM in tight loops,
   a GCR byte every 16 us, and a write must hand the IWM its next byte before the previous one has
   gone out. An outside review asked for the worst case, with sound on and off.
   With mac.buswait=1 core 0 notes when it posts each request and how many cycles the wire had seen
   then; the service core, as it takes the request, files the wait and the number of its own cycles
   that went out in between, by whether the SE's speaker was playing, for every request and for the
   IWM's alone. The heartbeat prints each five seconds' figures and the longest since the start.
   Waits are filed in half-octave steps (1, 1.5, 2, 3, 4, 6 us ...), so a percentile is given as the
   upper edge of its step; the longest is exact. The cycle count is the transport's business, not
   the Pi's: under QEMU a cycle is a semihosting trap, on the GPIO a microsecond or two, so "N of the
   service core's cycles" is the figure that carries from the testbed to the SE. */
#define BW_STEPS 48
struct bw_set { uint32_t n, step[BW_STEPS], longest_ns, longest_cycles, most_cycles; };
static struct bw_set bw_win[2][2], bw_run[2][2];  /* [speaker playing][0 every request, 1 the IWM's] */
int psmac_se_speaker_busy(void);                  /* psmac_board_se.c: the SE's speaker is writing its buffer */
static uint32_t bw_step(uint32_t ns)
{
    if (ns < 1024) return 0;                       /* under a microsecond (1024 ns) */
    uint32_t top = 31 - __builtin_clz(ns), half = (ns >> (top - 1)) & 1, k = 2 * (top - 10) + half + 1;
    return k < BW_STEPS ? k : BW_STEPS - 1;
}
static uint32_t bw_edge_ns(uint32_t k)             /* the upper edge of step k */
{
    if (!k) return 1024;
    uint32_t top = (k - 1) / 2 + 10, half = (k - 1) & 1;
    return (2 + half + 1) << (top - 1);
}
static void bw_file(struct bw_set *b, uint32_t ns, uint32_t cycles)
{
    b->n++; b->step[bw_step(ns)]++;
    if (ns > b->longest_ns) { b->longest_ns = ns; b->longest_cycles = cycles; }
    if (cycles > b->most_cycles) b->most_cycles = cycles;
}
static void bw_note(void)
{
    uint64_t ticks = cnt_now() - slot.posted_at;
    uint64_t ns64 = ticks * 1000000000ull / cnt_frq();
    uint32_t ns = ns64 > 0xffffffffull ? 0xffffffffu : (uint32_t)ns64, cycles = wire_cycles - slot.wire_at;
    int playing = psmac_board_se && psmac_se_speaker_busy() ? 1 : 0, iwm = dev_class(slot.addr) == 4;
    bw_file(&bw_win[playing][0], ns, cycles); bw_file(&bw_run[playing][0], ns, cycles);
    if (iwm) { bw_file(&bw_win[playing][1], ns, cycles); bw_file(&bw_run[playing][1], ns, cycles); }
}
static uint32_t bw_within(const struct bw_set *b, uint32_t permille)   /* the step edge below which permille of the waits fell, ns */
{
    uint64_t want = ((uint64_t)b->n * permille + 999) / 1000, seen = 0;
    for (uint32_t k = 0; k < BW_STEPS; k++) { seen += b->step[k]; if (seen >= want) return bw_edge_ns(k); }
    return bw_edge_ns(BW_STEPS - 1);
}
static void bw_line(const char *what, const struct bw_set *b, const struct bw_set *run)
{
    uint32_t p50 = bw_within(b, 500), p99 = bw_within(b, 990), p999 = bw_within(b, 999);
    if (!run->n) return;
    /* microseconds with one decimal */
    kprintf("[PSMAC]   bus waits, %s: %u requests, half within %u.%u us, 99%% within %u.%u, 99.9%% within %u.%u, the longest %u.%u us "
            "(%u of the service core's own cycles before it, at most %u) | since the start %u requests, the longest %u.%u us (%u cycles, at most %u)\n",
            what, b->n, p50 / 1000, p50 % 1000 / 100, p99 / 1000, p99 % 1000 / 100, p999 / 1000, p999 % 1000 / 100,
            b->longest_ns / 1000, b->longest_ns % 1000 / 100, b->longest_cycles, b->most_cycles,
            run->n, run->longest_ns / 1000, run->longest_ns % 1000 / 100, run->longest_cycles, run->most_cycles);
}
static void bw_report(void)
{
    if (!buswait_on) return;
    bw_line("speaker quiet, every request", &bw_win[0][0], &bw_run[0][0]);
    bw_line("speaker quiet, the IWM", &bw_win[0][1], &bw_run[0][1]);
    bw_line("speaker playing, every request", &bw_win[1][0], &bw_run[1][0]);
    bw_line("speaker playing, the IWM", &bw_win[1][1], &bw_run[1][1]);
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) bw_win[i][j] = (struct bw_set){ 0 };
}

/* The 68k's request, carried out (the service core only). */
static uint32_t scsi_reads;                /* reads of the SCSI chip: the JIT re-checks RAM blocks after them (the service loop) */
static void serve_request(void)
{
    if (buswait_on) bw_note();
    xfer_count++; last_addr = slot.addr; dev_cnt[dev_class(slot.addr)]++;
    /* the SE's speaker drives four bits of its VIA: every write of the Mac's to the VIA gets them as it goes out (psmac_board_se.c) */
    if (psmac_board_se && (slot.op == PSMAC_OP_WRITE || slot.op == PSMAC_OP_RESET_ACK)) slot.data = psmac_se_wire_write(slot.op == PSMAC_OP_RESET_ACK, slot.addr, slot.data);
    if (slot.op == PSMAC_OP_READ && dev_class(slot.addr) == 2) scsi_reads++;
    bridge_xfer(slot.op, slot.size, slot.flags, slot.addr, slot.data, &slot.rep);
    if (scsi_trace_left && dev_class(slot.addr) == 2) scsi_trace(slot.op, slot.addr, slot.data, slot.rep.data);
    __atomic_store_n(&slot.state, SLOT_DONE, __ATOMIC_RELEASE);
}
/* For the board layer's errands of many cycles (the SE's speaker buffer and screen rows,
   psmac_board_se.c): between two of their cycles, the 68k's request, if one has come, goes first.
   The 68000's bus has no notion of who asked: a cycle of the Mac's between two of the speaker's is
   as good as one before them, and the two never touch the same addresses. Not re-entrant into the
   errands: serve_request only carries the one request out. */
void psmac_serve_waiting(void)
{
    if (!service_running || !on_service_core()) return;
    if (__atomic_load_n(&slot.state, __ATOMIC_ACQUIRE) == SLOT_REQ) serve_request();
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
    else if (use_gpio) gpio_bring_up();
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
    uint32_t scsi_reads_seen = 0; uint64_t next_epoch = 0, next_name = 0;
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
                static int jit_steady;
                kprintf("[PSMAC]   D0-7 %08x %08x %08x %08x %08x %08x %08x %08x\n", BE32(m->D[0].u32), BE32(m->D[1].u32), BE32(m->D[2].u32), BE32(m->D[3].u32), BE32(m->D[4].u32), BE32(m->D[5].u32), BE32(m->D[6].u32), BE32(m->D[7].u32));
                kprintf("[PSMAC]   A0-7 %08x %08x %08x %08x %08x %08x %08x %08x ISP %08x\n", BE32(m->A[0].u32), BE32(m->A[1].u32), BE32(m->A[2].u32), BE32(m->A[3].u32), BE32(m->A[4].u32), BE32(m->A[5].u32), BE32(m->A[6].u32), BE32(m->A[7].u32), BE32(m->ISP.u32));
                {   /* the JIT's own counters (22 November 2026, Photoshop's slow start): instructions run and
                       blocks translated in the last 5 s; many translations a second is a cache thrashing,
                       few instructions with no translations is slow code (the FPU's, say) */
                    static uint64_t insn_last; static uint32_t miss_last;
                    uint64_t insn = m->INSN_COUNT; uint32_t miss = m->JIT_CACHE_MISS;
                    kprintf("[PSMAC]   JIT: %u M instructions in 5 s (%u MIPS), %u blocks translated, %u in the cache, %u of %u KB free\n",
                            (uint32_t)((insn - insn_last) / 1000000u), (uint32_t)((insn - insn_last) / 5000000u), miss - miss_last,
                            m->JIT_UNIT_COUNT, m->JIT_CACHE_FREE >> 10, m->JIT_CACHE_TOTAL >> 10);
                    jit_steady = miss - miss_last < 50 && insn - insn_last > 50000000u;   /* running, not loading */
                    insn_last = insn; miss_last = miss;
                }
                uint32_t sp = BE32(m->A[7].u32); int valid; uint32_t ph = psmac_mmu_l2p(sp, &valid);
                if (valid && ph + 32 <= ram_size) {
                    const uint32_t *st = (const uint32_t *)(KERNEL_ALIAS + ram_phys + ph);
                    kprintf("[PSMAC]   stack: %08x %08x %08x %08x %08x %08x %08x %08x\n", st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7]);
                }
                /* A program busy in its own code (22 November 2026: MacWrite and Photoshop on the owner's
                   SE under the stopwatch, the PC in the application's partition beat after beat): when
                   the PC is within 256 bytes of the last beat's and below the ROM, the code around it,
                   16 bytes before to 32 after, so the loop can be read from the log without the program */
                static uint32_t code_pc_last;
                uint32_t pcn = BE32((uint32_t)(uintptr_t)m->PC);
                /* also when the program is in a steady state (busy, translating next to nothing) with the PC
                   outside the ROM: Photoshop's PC moved between its own code and SANE's beat after beat */
                if (pcn < 0x40000000u && (pcn - code_pc_last + 256u < 512u || jit_steady)) {
                    uint32_t at = (pcn & ~1u) - 16u; int ok; uint32_t cph = psmac_mmu_l2p(at, &ok);
                    if (ok && cph + 48 <= ram_size) {
                        const uint16_t *w = (const uint16_t *)(KERNEL_ALIAS + ram_phys + cph);
                        kprintf("[PSMAC]   code at %08x:", at);
                        for (int k = 0; k < 24; k++) kprintf(" %04x", BE16(w[k]));
                        kprintf("\n");
                    }
                }
                code_pc_last = pcn;
            }
            { extern void psmac_slot_irq_report(void); psmac_slot_irq_report(); }
            {   /* the interrupt watch (psmac.h), counted by the 68k's core in ProcessIRQ: a level taken
                   again at once, or nested, is a level the JIT saw after its source had gone */
                struct psmac_irqwatch *w = &psmac_irqwatch;
                kprintf("[PSMAC]   interrupts taken: level 1 %u, 2 %u, 4 %u, other %u; taken again at once %u; nested %u bytes deep%s",
                        w->taken[1], w->taken[2], w->taken[4], w->taken[3] + w->taken[5] + w->taken[6] + w->taken[7], w->again, w->deepest, w->deepest ? "" : "\n");
                if (w->deepest) kprintf(" (at PC %08x)\n", w->deepest_pc);
                for (int k = 0; k < 8; k++) w->taken[k] = 0;
                w->again = 0; w->deepest = 0;
            }
            psmac_se_report();
            bw_report();
            { extern void alice_net_report(void); alice_net_report(); }
            { extern void psmac_ip_report(void); psmac_ip_report(); }
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
        { extern void psmac_loop_watch(void); psmac_loop_watch(); }   /* core 3 stopped for 2 s: where (alice_psmac.c) */
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
            serve_request();
            /* the SE's speaker has deadlines, and a Mac that polls the VIA without pause (the startup
               chime does for a second) would keep the loop from ever reaching psmac_se_service below */
            if (psmac_board_se) psmac_se_sound();
            continue;
        }
        if (!transport_dead && !transport_none && cnt_now() > next_poll) {
            bridge_xfer(PSMAC_OP_POLL, 0, 0, 0, 0, NULL);
            next_poll = cnt_now() + period;
        }
        psmac_se_service();                       /* the SE's screen, a few rows at a time */
        psmac_bridge_flush();                     /* the testbed's gathered posted writes, now that there is nothing else to do */
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
   core it goes through the service core's slot; from the service core it is direct.
   mac.selinefault= (psmac_board_se.c) puts an address-line fault on it for the SE's RAM, so the board
   layer's address-line test can be tried in the testbed. */
uint32_t psmac_se_wire_addr(uint32_t addr);
static inline uint32_t wire_addr(uint32_t addr) { return psmac_se_wire_addr(addr); }
uint32_t psmac_wire_read(uint32_t addr, int size)
{
    struct psmac_reply r;
    addr = wire_addr(addr);
    if (service_running && on_service_core()) bridge_xfer(PSMAC_OP_READ, size, 0, addr, 0, &r); else xfer(PSMAC_OP_READ, size, 0, addr, 0, &r);
    return r.data;
}
void psmac_wire_write(uint32_t addr, int size, uint32_t data, int posted)
{
    addr = wire_addr(addr);
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

    extern int psmac_watch_traps;
    if (psmac_watch_traps && (far & ~0xfffull) == 0x50000) {
        /* watched low-memory page (mac.watchtraps=1, psmac_mmu.c): plain RAM write, report suspicious stores into the OS trap table */
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
    if (*p != 'x' && *p != 'X') return 0;                       /* X: a card's short (8.3) name is in capitals */
    p++;
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
/* a disk image in the container, as walk_container will find it: the card's images are then not
   loaded (psmac_card.c), so the memory they would need need not be kept free for them */
static int container_has_image(const uint8_t *img, uint32_t size)
{
    uint32_t off = 0;
    while (img && off + 110 <= size && bytes_eq(img + off, "070701", 6)) {
        const char *h = (const char *)(img + off);
        uint32_t fsize = hex8(h + 54), nsize = hex8(h + 94);
        const char *name = h + 110;
        uint32_t data = (off + 110 + nsize + 3) & ~3u;
        if (data + fsize > size || (nsize == 11 && bytes_eq(name, "TRAILER!!!", 10)))
            break;
        if (fsize && (ends_with(name, ".img") || ends_with(name, ".hda") || ends_with(name, ".image") || ends_with(name, ".hfv") || ends_with(name, ".scsi")))
            return 1;
        off = (data + fsize + 3) & ~3u;
    }
    return 0;
}

/* "...-se-WxH.declrom", or "...-se.declrom" (any size, since 20 November 2026): the SE board's two cards,
   slot 9 with the SE's screen and slot $A with an HDMI one */
static int declrom_se(const char *name)
{
    for (const char *p = name; *p; p++)
        if (p[0] == '-' && (p[1] == 's' || p[1] == 'S') && (p[2] == 'e' || p[2] == 'E') && (p[3] == '-' || p[3] == '.')) return 1;
    return 0;
}
/* The declaration ROMs on offer: the container's (walk_container) and, since 20 November 2026, the
   card's root (psmac_card.c, docs/10 "Declaration ROMs from the card"), which win a tie. Each is known
   by its name: a size at the end (-WxH), "-se-" or "-se." for the SE board's two-screen file. The
   card's are read only once one of them is chosen. */
struct declcand { const uint8_t *p; uint32_t len, w, h; int sized, se, card, bad, novideo, wifionly, noboot, booted; char name[64]; };
#define DECLCAND_MAX 48                     /* a release has 29 declaration ROMs; room for the card's beside the container's */
static struct declcand dcand[DECLCAND_MAX];
static int ndcand;
static uint8_t card_declrom[PSMB_DECLROM_SIZE] __attribute__((aligned(16)));   /* the card's chosen one, read here */
static void declcand_add(const char *name, const uint8_t *p, uint32_t len, int card)
{
    if (ndcand >= DECLCAND_MAX) return;
    struct declcand *c = &dcand[ndcand++];
    unsigned k = 0; while (name[k] && k < sizeof c->name - 1) { c->name[k] = name[k]; k++; } c->name[k] = 0;
    c->p = p; c->len = len; c->card = card; c->bad = 0; c->novideo = 0; c->noboot = 0; c->booted = 0;
    c->sized = declrom_res(c->name, &c->w, &c->h);
    /* "-wifi" in the name (aurora68-wifi-WxH, aurora68-wifionly, psmac_wifi): the cards with the WiFi
       as their only network card, made for Pis without a wired port; the full card serves those too
       (wired_ethernet_works drops its wired sResource there), so it wins a tie */
    c->wifionly = 0;
    for (const char *q = c->name; *q; q++)
        if ((q[0] == '-' || q[0] == '_') && (q[1] | 32) == 'w' && (q[2] | 32) == 'i' && (q[3] | 32) == 'f' && (q[4] | 32) == 'i') c->wifionly = 1;
    c->se = declrom_se(c->name);
    if (!c->sized) { c->w = 640; c->h = 480; }                    /* the plain ROM is mkdeclrom.py's default, 640x480 */
}
static void declcand_card(const char *name, uint32_t size) { declcand_add(name, 0, size, 1); }

static void walk_container(const uint8_t *img, uint32_t size, struct container_result *r)
{
    r->rom = img; r->rom_len = size; r->declrom = NULL; r->declrom_len = 0; r->declrom_sized = 0;
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
                if (fsize <= PSMB_DECLROM_SIZE) declcand_add(base, img + data, fsize, 0);   /* chosen in declrom_choose */
                kprintf("[PSMAC] container: declaration ROM %s, %u bytes\n", base, fsize);
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
}

/* Which declaration ROM. Everywhere but the SE board: a one-screen card, best of the screen's size.
   On the SE board the SE's screen is the first, 512 x 342: with mac.hdmi=screen a two-screen file
   (psmac-se-*), best of the HDMI size; otherwise the one-screen card, best of 512 x 342. Since
   20 November 2026 every ROM's screen is rewritten for the size in use (declrom_resize, below), so a
   file of another size serves as well as one of the right size: the exact size is preferred only so
   that a container made for one size keeps doing what it did. Until then a ROM of another size gave
   its size to the screen ("so the screen is WxH"), and before 19 November it was not taken at all and
   slot 9 stayed empty. On the SE board with no two-screen file HDMI mirrors the SE, as before; the
   two-screen files are left alone on the other boards (their first screen is the SE's one bit). Ties
   go to the card's root, and among exact matches from one place to the last one listed (as before).
   Lower ranks are better; -1 is "not for this board". */
static int declrom_rank(const struct declcand *c, int se_board, int want_se, uint32_t want_w, uint32_t want_h)
{
    int exact = c->sized && c->w == want_w && c->h == want_h;
    if (want_se) return c->se ? (exact ? 0 : 1) : c->sized ? (c->w == 512 && c->h == 342 ? 2 : 3) : 4;
    if (se_board) return c->se ? 3 : c->sized ? (c->w == 512 && c->h == 342 ? 0 : 1) : 2;
    if (c->se) return -1;
    return exact ? 0 : c->sized ? 2 : 1;
}
static int declrom_has_video(const uint8_t *p, uint32_t len, int se);   /* below, with the ROM's lists */
static int declrom_has_bootrec(const uint8_t *p, uint32_t len);         /* below */
static const uint8_t *declrom_below(const uint8_t *top, uint32_t len, uint32_t *out_len);   /* below */
/* the rank with what has been found out by looking into the file: no screen, no boot record */
static int declrom_rank_now(const struct declcand *c, int se_board, int want_se, uint32_t want_w, uint32_t want_h)
{
    int rk = c->bad ? -1 : declrom_rank(c, se_board, want_se, want_w, want_h);
    if (rk < 0) return rk;
    if (c->novideo) rk += 10;                       /* a card without a screen (aurora68-wifionly): only if nothing else */
    if (c->noboot) rk += 5;                         /* PiDisk without its boot record: an old file (docs/08 item 79) */
    return rk;
}
/* whether a beats b at the same rank: the full card before a WiFi-only one, the card's root before the
   container, and among exact matches from one place the last one listed (as before) */
static int declrom_tie(const struct declcand *a, const struct declcand *b, int rk)
{
    if (a->wifionly != b->wifionly) return a->wifionly < b->wifionly;
    if (a->card != b->card) return a->card > b->card;
    return rk == 0;
}
/* one line: the other files, and why each was passed over */
static void declrom_why(const struct declcand *best, int br, int se_board, int want_se, uint32_t want_w, uint32_t want_h)
{
    int n = 0;
    for (int i = 0; i < ndcand; i++) {
        const struct declcand *c = &dcand[i];
        if (c == best) continue;
        int rk = declrom_rank_now(c, se_board, want_se, want_w, want_h);
        const char *why = c->bad ? "could not be read" : rk < 0 ? "not for this board" : c->novideo ? "no screen in it"
                        : c->noboot ? "its PiDisk has no boot record (an older file): no startup disk from an image"
                        : rk > br ? (c->se && !want_se ? "the two-screen card" : !c->se && want_se ? "a one-screen card" : "made for another screen size")
                        : c->wifionly && !best->wifionly ? "a WiFi-only card (the full card has the WiFi too)"
                        : c->card != best->card ? "the card's root wins a tie" : "listed before it";
        kprintf("%s%s%s (%s)", n++ ? ", " : "[PSMAC]   chosen over: ", c->card ? "" : "the container's ", c->name, why);
    }
    if (n) kprintf("\n");
}
static struct declcand *declrom_choose(uint32_t want_w, uint32_t want_h)
{
    int se_board = psmac_board_se && !psmac_board_solo, want_se = se_board && psmac_hdmi_mode == 1;
    if (se_board && !want_se) { want_w = 512; want_h = 342; }
    for (;;) {
        struct declcand *best = 0; int br = 99;
        for (int i = 0; i < ndcand; i++) {
            struct declcand *c = &dcand[i];
            int rk = declrom_rank_now(c, se_board, want_se, want_w, want_h);
            if (rk < 0) continue;
            if (!best || rk < br || (rk == br && declrom_tie(c, best, rk))) { best = c; br = rk; }
        }
        if (!best) {
            for (int i = 0; i < ndcand; i++)
                if (dcand[i].se) { kprintf("[PSMAC] declaration ROM: %s is the SE board's two-screen card (mac.board=se); no declaration ROM for this board\n", dcand[i].name); break; }
            return 0;
        }
        if (best->card && !best->p) {
            extern int32_t psmac_card_read_file(const char *name, uint8_t *buf, uint32_t max);
            int32_t n = psmac_card_read_file(best->name, card_declrom, sizeof card_declrom);
            if (n < 20) { kprintf("[PSMAC] declaration ROM %s on the card could not be read: the next best is taken\n", best->name); best->bad = 1; continue; }
            best->p = card_declrom; best->len = (uint32_t)n;
        }
        /* A release's card root holds every declaration ROM of the release, and aurora68-wifionly.declrom
           has no video sResource: taken for a size no file names, the Mac would have no screen at all.
           So the chosen file is looked into, and one without a screen goes to the back of the queue. */
        if (!best->novideo && !declrom_has_video(best->p, best->len, best->se)) {
            best->novideo = 1;
            if (best->card) best->p = 0;                 /* read again if it is taken after all */
            continue;
        }
        /* The same for PiDisk's boot record (sRsrcBootRec in its sResource, 262c344): an older file
           without it gives a Mac that cannot start from the disk image, only the question mark. The
           user's SE, 21 November 2026: a card root with a whole older release's declaration ROMs, and
           aurora68-wifi-512x342.declrom of an old build taken over aurora68-512x342.declrom, the last
           of two equal ones listed. Such a file is taken only when no other will do. */
        if (!best->noboot && !best->booted) {
            if (!declrom_has_bootrec(best->p, best->len)) {
                best->noboot = 1;
                if (best->card) best->p = 0;
                continue;
            }
            best->booted = 1;
        }
        const char *from = best->card ? "the card's root" : "the container";
        if (want_se && !best->se) {
            kprintf("[PSMAC] declaration ROM: %s from %s, a one-screen card: no two-screen file (aurora68-se.declrom or aurora68-se-WxH.declrom), so HDMI mirrors the SE's screen\n", best->name, from);
            psmac_hdmi_mode = 0;
        } else
            kprintf("[PSMAC] declaration ROM: %s from %s taken (%s screen %ux%u%s)\n", best->name, from, want_se ? "the HDMI" : se_board ? "the SE's" : "the",
                    want_w, want_h, declrom_rank(best, se_board, want_se, want_w, want_h) == 0 ? ", the file's own size" : ": the file's screen is rewritten for it");
        if (best->noboot)
            kprintf("[PSMAC] *** declaration ROM %s: its PiDisk has no boot record, so the Mac cannot start from a disk image (only the question mark): put a current declaration ROM on the card ***\n", best->name);
        declrom_why(best, br, se_board, want_se, want_w, want_h);
        return best;
    }
}

/* ---- the declaration ROM's screen, rewritten for the size in use (20 November 2026) ---------------
   One ROM per screen size meant a container rebuilt for each monitor, and the screen took the ROM's
   size when the two disagreed. Now the video sResource of the one-screen card ($84, slot 9) or of the
   SE board's HDMI card ($80, slot $A) is rewritten in Pi RAM before the Slot Manager reads it: each
   mode's VPBlock (mVidParams, entry 1 of mode 128+i) gets the screen's bounds, its row pitch
   (psmb_rowbytes) and the vpBaseOffset of its place (psmac_display_mode_base, the rule the display
   block uses: mkdeclrom.py's too), and a depth with no place at this size leaves the mode list (the
   list's later entries move up, as in declrom_drop). Then the ROM's CRC again. The ROM's other
   contents are not touched; the driver, PiScrn and Monitors take the size from the ROM and the
   display block, never from tables of their own. Returns the modes left, 0 if the ROM has no such
   video sResource. */
static int32_t ent_off(const uint8_t *e) { return (int32_t)(BE32(*(const uint32_t *)e) << 8) >> 8; }
static uint8_t *list_entry(uint8_t *list, const uint8_t *lo, const uint8_t *hi, uint8_t id)
{
    for (uint8_t *l = list; l >= lo && l + 4 <= hi && l[0] != 0xff; l += 4)
        if (l[0] == id) return l;
    return 0;
}
/* the entries after e move up four bytes (their offsets are relative to their own address, so each grows by four) */
static void list_drop(uint8_t *e)
{
    for (; e[4] != 0xff; e += 4) {
        uint32_t next = BE32(*(uint32_t *)(e + 4));
        int32_t off = ((int32_t)(next << 8) >> 8) + 4;
        *(uint32_t *)e = BE32((next & 0xff000000u) | ((uint32_t)off & 0xffffffu));
    }
    *(uint32_t *)e = BE32(0xff000000u);                 /* the end mark, one entry earlier */
    *(uint32_t *)(e + 4) = 0;
}
static void declrom_crc(uint8_t *top)
{
    uint8_t *fb = top - 20;
    uint32_t length = BE32(*(uint32_t *)(fb + 4)), crc = 0;
    *(uint32_t *)(fb + 8) = 0;
    for (uint8_t *p = top - length; p < top; p++) crc = ((crc << 1) | (crc >> 31)) + *p;
    *(uint32_t *)(fb + 8) = BE32(crc);
}
static int declrom_resize(uint8_t *top, uint8_t rid, uint32_t slot, uint32_t w, uint32_t h, uint32_t win_mac, uint32_t win_size)
{
    extern uint32_t psmac_display_mode_base(uint32_t w, uint32_t h, uint32_t depth, uint32_t win_mac, uint32_t win_size, int one_bit_only);
    uint8_t *fb = top - 20;
    uint32_t length = BE32(*(uint32_t *)(fb + 4));
    if (length < 20 || length > PSMB_DECLROM_SIZE || BE32(*(uint32_t *)(fb + 14)) != 0x5A932BC7u) return 0;
    const uint8_t *lo = top - length;
    uint8_t *e = list_entry(fb + ((int32_t)(BE32(*(uint32_t *)fb) << 8) >> 8), lo, fb, rid);
    if (!e) return 0;
    uint8_t *list = e + ent_off(e), *t = list_entry(list, lo, fb, 1), *mb = list_entry(list, lo, fb, 10);
    if (!t || !mb || BE32(*(uint32_t *)(t + ent_off(t))) != 0x00030001u) return 0;   /* sRsrcType {catDisplay, typVideo} */
    uint32_t devbase = 0xF0000000u | slot << 24 | (BE32(*(uint32_t *)(mb + ent_off(mb))) & 0xffffffu);   /* slot space + MinorBaseOS */
    uint32_t ow = 0, oh = 0; int kept = 0;
    for (int i = 5; i >= 0; i--) {                      /* the deepest first, so that a drop moves nothing still to be done */
        uint8_t *m = list_entry(list, lo, fb, (uint8_t)(128 + i));
        if (!m) continue;
        uint8_t *ml = m + ent_off(m), *vp = list_entry(ml, lo, fb, 1);
        if (!vp) continue;
        uint8_t *b = vp + ent_off(vp) + 4;              /* the sBlock's length, then the VPBlock */
        uint32_t depth = 1u << i;
        if (BE16(*(uint16_t *)(b + 32)) != depth) continue;    /* vpPixelSize (VPBlock offset 32): not a mode this file has as the rule says */
        if (!i) { oh = BE16(*(uint16_t *)(b + 10)) - BE16(*(uint16_t *)(b + 6)); ow = BE16(*(uint16_t *)(b + 12)) - BE16(*(uint16_t *)(b + 8)); }
        uint32_t base = psmac_display_mode_base(w, h, depth, win_mac, win_size, 0);
        if (!base) { list_drop(m); continue; }
        *(uint32_t *)b = BE32(base - devbase);          /* vpBaseOffset: wraps round to the right address in 32 bits */
        *(uint16_t *)(b + 4) = BE16((uint16_t)psmb_rowbytes(w, depth));
        *(uint16_t *)(b + 6) = 0; *(uint16_t *)(b + 8) = 0;
        *(uint16_t *)(b + 10) = BE16((uint16_t)h); *(uint16_t *)(b + 12) = BE16((uint16_t)w);
        kept++;
    }
    declrom_crc(top);
    if (kept) kprintf("[PSMAC] declaration ROM: slot $%X's screen %s%ux%u, %d depth%s (sResource $%02x)\n", slot,
                      ow == w && oh == h ? "kept at " : "rewritten for ", w, h, kept, kept == 1 ? "" : "s", rid);
    if (kept && (ow != w || oh != h)) kprintf("[PSMAC]   (the file described %ux%u)\n", ow, oh);
    return kept;
}
/* whether a declaration ROM file has the screen it is chosen for: a video sResource ($84 of slot 9's
   ROM at the file's end; for a two-screen file, $80 of slot $A's ROM below it) */
static int declrom_has_video(const uint8_t *p, uint32_t len, int se)
{
    if (!p || len < 20) return 0;
    const uint8_t *top = p + len;
    uint8_t rid = 0x84;
    if (se) {
        uint32_t len2 = 0;
        const uint8_t *r2 = declrom_below(top, len, &len2);
        if (!r2) return 0;
        top = r2 + len2; rid = 0x80;
    }
    const uint8_t *fb = top - 20;
    uint32_t length = BE32(*(const uint32_t *)(fb + 4));
    if (length < 20 || length > len || BE32(*(const uint32_t *)(fb + 14)) != 0x5A932BC7u) return 0;
    uint8_t *dir = (uint8_t *)fb + ((int32_t)(BE32(*(const uint32_t *)fb) << 8) >> 8);
    return list_entry(dir, top - length, fb, rid) != 0;
}

/* whether slot 9's ROM (at the file's end, in the two-screen files too) has PiDisk's boot record: entry
   6, sRsrcBootRec, in sResource $80 (mkdeclrom.py --boot) */
static int declrom_has_bootrec(const uint8_t *p, uint32_t len)
{
    if (!p || len < 20) return 0;
    const uint8_t *top = p + len, *fb = top - 20;
    uint32_t length = BE32(*(const uint32_t *)(fb + 4));
    if (length < 20 || length > len || BE32(*(const uint32_t *)(fb + 14)) != 0x5A932BC7u) return 0;
    uint8_t *dir = (uint8_t *)fb + ((int32_t)(BE32(*(const uint32_t *)fb) << 8) >> 8);
    uint8_t *e = list_entry(dir, top - length, fb, 0x80);
    if (!e) return 0;
    return list_entry(e + ent_off(e), top - length, fb, 6) != 0;
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
       3A+, Zero 2 W with eth=usb   a USB adapter       $81 kept (psmac_usbnet.c)
       eth=off             (any board)                  $81 left out
   The testbed's file bridge (mac.net=) counts as a wired port. The board comes from its revision
   code, else the device tree (psmac_board_wired: the 3A+'s firmware passes the 3B+'s tree, so the
   tree alone took a 3A+ for a 3B+ until 20 November 2026), so this is decided
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
    list_drop(e);
    declrom_crc(top);
    return 1;
}

/* A second declaration ROM below the one whose format block ends at top, in a file of len bytes
   loaded right-aligned there: the SE board's file holds slot $A's ROM before slot 9's (psmb.h, "two
   screens"; tools/mac/mkdeclrom.py --se). Its own format block ends where slot 9's ROM begins
   (length, at 4 in the block) and carries the test pattern ($5A932BC7, at 14). Returns its first
   byte and size, or 0. */
static const uint8_t *declrom_below(const uint8_t *top, uint32_t len, uint32_t *out_len)
{
    uint32_t len9 = BE32(*(const uint32_t *)(top - 16));
    if (len9 < 20 || len9 + 20 > len) return 0;
    const uint8_t *top2 = top - len9, *fb = top2 - 20;
    uint32_t len2 = BE32(*(const uint32_t *)(fb + 4));
    if (BE32(*(const uint32_t *)(fb + 14)) != 0x5A932BC7u || len2 < 20 || len2 > PSMB_DECLROM2_SIZE || len9 + len2 > len) return 0;
    *out_len = len2;
    return top2 - len2;
}

/* PiDisplay's rotation (psmac_hdmi.c): the declaration ROM as it was before its screen was rewritten,
   so that a restart with another rotation can rewrite it again from the file's own (psmac_declrom_rescreen) */
static uint8_t rescreen_copy[PSMB_DECLROM_SIZE];
static uint32_t rescreen_len;
static uint8_t *rescreen_top;                  /* where the live ROM ends: slot $A's in the window, or slot 9's */
static uint8_t rescreen_rid, rescreen_slot;
static uint32_t rescreen_win, rescreen_winsize;
int psmac_declrom_rescreen(uint32_t w, uint32_t h)
{
    if (!rescreen_len || !rescreen_top) return 0;
    uint8_t *lo = rescreen_top - rescreen_len;
    memcpy(lo, rescreen_copy, rescreen_len);
    int k = declrom_resize(rescreen_top, rescreen_rid, rescreen_slot, w, h, rescreen_win, rescreen_winsize);
    arm_flush_cache((uintptr_t)lo, rescreen_len);
    return k;
}
static void rescreen_keep(uint8_t *top, uint32_t len, uint8_t rid, uint8_t slot, uint32_t win, uint32_t winsize)
{
    if (len > sizeof rescreen_copy) return;
    memcpy(rescreen_copy, top - len, len);
    rescreen_len = len; rescreen_top = top; rescreen_rid = rid; rescreen_slot = slot; rescreen_win = win; rescreen_winsize = winsize;
}

void psmac_setup_memory(void *rom, uint32_t rom_sz)
{
    psmac_read_options();
    { extern void psmac_fpu_init(void); psmac_fpu_init(); }          /* mac.fpu: exact or double (psmac_fpu.c) */
    /* the card's config.txt and aurora68-display.txt, before the screen's size is decided: PiDisplay's
       rotation turns it, and in the testbed mac.hdmisim=1 makes the firmware's buffer the mode they set */
    { extern void psmac_hdmi_early(const void *, uint32_t); psmac_hdmi_early(rom, rom_sz); }
    uintptr_t base = (sys_memory[0].mb_Base + 0x1fffff) & ~0x1fffffull;
    {   /* The big frame buffer region: 9 MiB holds every screen up to 1920 x 1200 in millions of colours,
           and stays that size for them, so that nothing changes for the Mac's memory there. A larger
           screen (mac.screen=, else the size the firmware's HDMI mode gave the VideoCore) gets its
           32-bit frame rounded up to a MiB: 15 MiB at 2560 x 1440 (psmb.h). Decided here, before the
           container is read: the declaration ROM is rewritten for this size later (declrom_resize),
           with only the modes that fit. */
        extern uint32_t fb_width, fb_height;
        /* Without mac.screen= the screen is the size the firmware's HDMI mode gave its frame buffer
           (config.txt's hdmi_mode, or hdmi_cvt), so that config.txt alone chooses it; outside the sizes
           the kernel takes, 1024 x 768 */
        if (!screen_w) {
            if (fb_width >= PSMB_SCREEN_MIN_W && fb_width <= PSMB_SCREEN_MAX_W && fb_height >= PSMB_SCREEN_MIN_H && fb_height <= PSMB_SCREEN_MAX_H) {
                screen_w = fb_width; screen_h = fb_height;
                kprintf("[PSMAC] screen: %ux%u, the firmware's HDMI mode (mac.screen= not given)\n", screen_w, screen_h);
            } else {
                screen_w = 1024; screen_h = 768;
                kprintf("[PSMAC] screen: the firmware's frame buffer is %ux%u, outside %ux%u to %ux%u: 1024x768 (mac.screen= chooses)\n",
                        fb_width, fb_height, PSMB_SCREEN_MIN_W, PSMB_SCREEN_MIN_H, PSMB_SCREEN_MAX_W, PSMB_SCREEN_MAX_H);
            }
        }
        {   /* PiDisplay's rotation: at 90 and 270 the Mac's HDMI screen is the mode turned on its side */
            extern int psmac_hdmi_rotation(void);
            int r = psmac_hdmi_rotation();
            if (r == 90 || r == 270) {
                uint32_t t = screen_w; screen_w = screen_h; screen_h = t;
                kprintf("[PSMAC] screen: turned %d (aurora68-display.txt): the Mac's HDMI screen is %ux%u\n", r, screen_w, screen_h);
            }
        }
        uint64_t w = screen_w, h = screen_h;
        uint64_t need = (w * h * 4 + 0xfffff) & ~0xfffffull;
        if (need > PSMB_FB_BIG_SIZE) {
            fb_big_size = need > PSMB_FB_BIG_MAX ? PSMB_FB_BIG_MAX : (uint32_t)need;
            kprintf("[PSMAC] screen %ux%u: the big frame buffer region is %u MiB (9 MiB up to 1920x1200%s)\n",
                    (uint32_t)w, (uint32_t)h, fb_big_size >> 20, need > PSMB_FB_BIG_MAX ? "; at most 32 MiB, the deepest modes are left out" : "");
        }
    }
    extern uint32_t psmac_videocard_size(void);                                  /* the video card, slot $C (psmac_videocard.c): 0 with mac.video=off */
    const uintptr_t vid_size = psmac_videocard_size();
    const uintptr_t extra = PSMB_WIN_SIZE + PSMB_DECLROM_SIZE + fb_big_size + vid_size;    /* slot $9 mailbox window + declaration ROM + the big frame buffer + the video card */
    uint32_t mb = globals.psmac_ram_mb;
    uintptr_t avail = sys_memory[0].mb_Base + sys_memory[0].mb_Size - base;
    int image_inside = container_has_image((const uint8_t *)rom, rom_sz);
    uint32_t keep_mb = image_inside ? PSMAC_PI_SLACK_MB : PSMAC_CARD_IMAGE_KEEP_MB;
    {   /* the board first: its type decides the wired card and the firmware, and its memory is what
           this function shares out (psmac_usbnet.c; the first call asks the mailbox, on this core,
           before the services start) */
        extern int psmac_board_type(void); extern const char *psmac_board_name(void); extern uint32_t psmac_board_revision(void);
        extern const char *psmac_board_wired_name(void);
        int t = psmac_board_type();
        kprintf("[PSMAC] board: %s (revision %06x, type %02x); wired port: %s; %u MiB of RAM free for the Mac and its disks\n",
                psmac_board_name(), psmac_board_revision(), t < 0 ? 0xff : t, psmac_board_wired_name(), (uint32_t)(avail >> 20));
    }
    if (!mb) {
        /* No mac.ram=: take what the Pi can spare, up to PSMAC_RAM_DEFAULT_MB, which is what the IIci
           ROM sizes without a memory patch (two banks of 64 MiB, docs/08 item 11). The kernel's own
           pools (the JIT's 64 MiB, its heap, the VideoCore's memory) are already gone from this
           block; what else comes out of it later is a disk image on the card (psmac_card.c), so that
           much is kept back: PSMAC_CARD_IMAGE_KEEP_MB, room for a 96 MB System 7 image, or only the
           loader's slack when the container brought its own image (the card's are then not read).
           Until 20 November 2026 a flat 192 MiB was kept, which on a 512 MiB Pi (a 3A+ or Zero 2 W:
           448 MiB for the ARM with gpu_mem=64) left the Mac 72 MiB, and 8 MiB with a 96 MB image in
           the container (QEMU's raspi3ap, docs/08 item 60). Sizes that are not a full bank or a
           full bank A plus part of bank B still work (the shim aliases what is missing,
           psmac_mmu.c), but a real IIci never had them, so the rounding stays at 8. */
        uintptr_t fixed = PSMAC_ROM_SIZE + extra + ((uintptr_t)keep_mb << 20);
        uintptr_t spare = avail > fixed ? avail - fixed : 0;
        mb = (uint32_t)(spare >> 20) & ~7u;
        if (mb > PSMAC_RAM_DEFAULT_MB) mb = PSMAC_RAM_DEFAULT_MB;
        if (mb < 8) mb = 8;
        kprintf("[PSMAC] no mac.ram=: %u MiB for the Mac; %u MiB kept back for %s\n", mb, keep_mb,
                image_inside ? "the kernel (the container brings the disk image)" : "a disk image on the card and the kernel");
        if (mb < PSMAC_RAM_DEFAULT_MB)
            kprintf("[PSMAC] MEMORY: the Mac gets less than %u MiB: the Pi has %u MiB free%s. A smaller disk image, or mac.ram=N to choose\n",
                    PSMAC_RAM_DEFAULT_MB, (uint32_t)(avail >> 20), image_inside ? " beside the container's disk image" : "");
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
    {
        uint32_t left = (uint32_t)(sys_memory[0].mb_Size >> 20);
        if (image_inside)
            kprintf("[PSMAC] memory: Mac %u MiB, %u MiB left for the kernel\n", ram_size >> 20, left);
        else
            kprintf("[PSMAC] memory: Mac %u MiB, %u MiB left: a disk image on the card can be up to %u MB\n", ram_size >> 20, left,
                    left > PSMAC_PI_SLACK_MB ? left - PSMAC_PI_SLACK_MB : 0);
        if (left < keep_mb && globals.psmac_ram_mb)
            kprintf("[PSMAC] MEMORY: mac.ram=%u leaves %u MiB: a disk image on the card larger than %u MB will not be loaded\n",
                    globals.psmac_ram_mb, left, left > PSMAC_PI_SLACK_MB ? left - PSMAC_PI_SLACK_MB : 0);
    }

    uint64_t parse_t0 = psmac_boot_now_us();
    struct container_result cont; walk_container((const uint8_t *)rom, rom_sz, &cont);
    psmac_boot_mark("container parsed: %u KiB in %u us", rom_sz >> 10, (uint32_t)(psmac_boot_now_us() - parse_t0));
    /* Declaration ROMs in the card's root, beside the container's (psmac_card.c), when the Pi booted
       from a container, which on hardware it always does: a testbed run from a bare ROM has no card
       to look at, and would wait 1.5 s for one. mac.cardrom=off leaves the card's out. */
    {   extern int psmac_card_declroms(void (*cb)(const char *name, uint32_t size));
        const char *o = psmac_option("mac.cardrom");
        int from_container = cont.rom != (const uint8_t *)rom;      /* walk_container leaves a bare ROM where it was */
        if (from_container && !(o && o[0] == 'o' && o[1] == 'f')) {
            int before = ndcand;
            if (psmac_card_declroms(declcand_card) == 0)
                for (int i = before; i < ndcand; i++)
                    kprintf("[PSMAC] card: declaration ROM %s, %u bytes\n", dcand[i].name, dcand[i].len);
        }
    }
    {   struct declcand *c = declrom_choose(screen_w, screen_h);
        if (c) { cont.declrom = c->p; cont.declrom_len = c->len; cont.declrom_sized = c->sized; }
    }
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
        /* A container made by make-container.py holds the ROM as that script patched it, with the
           patch set of its day (369A5A97 until 21 November 2026). The delta is a superset of every
           earlier set, so it is tried on such an image too: if the result's sum is the current
           patched ROM's, the image was an older patched IIci ROM and is now the current one (the
           Memory Manager's 24-bit retry, docs/08 item 78, would otherwise never reach those cards);
           otherwise it was some other ROM and is copied back as it came. */
        uint32_t first = BE32(*(uint32_t *)romk);
        if (n == PSMAC_ROM_SIZE && first != PSMAC_ROM_DELTA_OUT) {
            for (unsigned i = 0; i < sizeof(psmac_rom_delta) / sizeof(psmac_rom_delta[0]); i++)
                memcpy(romk + psmac_rom_delta[i].off, psmac_rom_delta[i].bytes, psmac_rom_delta[i].len);
            uint32_t sum = 0;
            for (uint32_t o = 4; o < PSMAC_ROM_SIZE; o += 2) sum += BE16(*(uint16_t *)(romk + o));
            if (first == PSMAC_ROM_DELTA_IN)
                kprintf("[PSMAC] stock IIci ROM %08x in the container: patched in memory, checksum word %08x (%s)\n",
                        PSMAC_ROM_DELTA_IN, BE32(*(uint32_t *)romk), sum == PSMAC_ROM_DELTA_OUT ? "verified" : "MISMATCH");
            else if (sum == PSMAC_ROM_DELTA_OUT)
                kprintf("[PSMAC] IIci ROM %08x in the container, patched by an older patch set: brought up to date in memory (checksum word %08x, verified)\n",
                        first, PSMAC_ROM_DELTA_OUT);
            else
                memcpy(romk, rom, n);
        }
        { extern void psmac_se_rom_fixup(uint8_t *, uint32_t); psmac_se_rom_fixup(romk, n); }   /* the SE with its speaker: the full startup chime */
        { extern void psmac_se_scsi_fixup(uint8_t *, uint32_t); psmac_se_scsi_fixup(romk, n); } /* the SE's own SCSI bus: Apple's selection timeout */
        for (uint32_t o = n; o < PSMAC_ROM_SIZE; o += n) {
            memcpy(romk + o, rom, (o + n <= PSMAC_ROM_SIZE) ? n : PSMAC_ROM_SIZE - o);
        }
    } else {
        kprintf("[PSMAC] WARNING: no ROM in initramfs!\n");
    }
    /* identity mapping (MMU disabled) and the 68030 MMU shadowing machinery */
    psmac_mmu_set_slot(win_phys, declrom_phys);
    psmac_mmu_set_fb(fb_big_phys, fb_big_size);
    { extern void psmac_videocard_map(uintptr_t); if (vid_size) psmac_videocard_map(fb_big_phys + fb_big_size); }   /* slot $C, after the big frame buffer */
    {   /* slot $A, the HDMI screen's card, on the SE board with mac.hdmi=screen (psmb.h, "two screens"):
           mapped before the identity map is made; its ROM is put in place below, if the file has one,
           else those 16 KiB stay zero and the Slot Manager finds no card there */
        extern void psmac_mmu_set_slot2(uintptr_t);
        if (psmac_board_se && !psmac_board_solo && psmac_hdmi_mode == 1) psmac_mmu_set_slot2(win_phys + PSMB_OFF_DECLROM2);
    }
    psmac_mmu_init(ram_phys, ram_size, rom_phys);
    const uint8_t *slot2_rom = 0; uint32_t slot2_len = 0;
    {   /* declaration ROM image (testbed: host file via semihosting; hardware: SD card later) */
        uint8_t *dr = (uint8_t *)(KERNEL_ALIAS + declrom_phys);
        for (uint32_t i = 0; i < PSMB_DECLROM_SIZE; i += 8) *(uint64_t *)(dr + i) = 0;
        const char *path = psmac_option("mac.declrom");
        uint32_t declrom_len = 0;
        if (cont.declrom && cont.declrom_len <= PSMB_DECLROM_SIZE) {
            declrom_len = cont.declrom_len;
            memcpy(dr + PSMB_DECLROM_SIZE - cont.declrom_len, cont.declrom, cont.declrom_len);
            kprintf("[PSMAC] declaration ROM in place: %u bytes at Mac 0x%08x\n", cont.declrom_len, PSMB_DECLROM_PHYS + PSMB_DECLROM_SIZE - cont.declrom_len);
        } else if (path && (use_semihost || sh_files)) {
            int64_t fd = sh_open(path, 1);
            int64_t len = fd >= 0 ? psmac_sh_flen(fd) : -1;
            if (len > 0 && len <= (int64_t)PSMB_DECLROM_SIZE) {
                /* the format block must end at the top of the slot space: right-align the image */
                declrom_len = (uint32_t)len;
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
        /* The SE board's two cards (psmac-se-WxH, psmb.h "two screens"): the file holds slot $A's
           declaration ROM, the HDMI screen's card, below slot 9's. It is served (from the window, once
           psmac_mbox_init has made it, below) on the SE board with mac.hdmi=screen; otherwise slot $A
           stays empty and HDMI shows the SE's picture or the log. */
        {   extern void psmac_display_layout(int se_card, int two);
            slot2_rom = declrom_below(dr + PSMB_DECLROM_SIZE, declrom_len, &slot2_len);
            int se_card = slot2_rom != 0, two = se_card && psmac_board_se && !psmac_board_solo && psmac_hdmi_mode == 1;
            if (two) kprintf("[PSMAC] declaration ROM: two cards, the SE's screen in slot 9 (main) and the HDMI screen in slot $A (%u bytes at Mac 0x%08x)\n",
                             slot2_len, PSMB_DECLROM2_PHYS + PSMB_DECLROM2_SIZE - slot2_len);
            else if (se_card) kprintf("[PSMAC] declaration ROM: the HDMI screen's card (slot $A) left out: %s\n", !psmac_board_se || psmac_board_solo ? "this is the SE board's file, and this board is not an SE"
                                       : psmac_hdmi_mode == 2 ? "HDMI shows the log (mac.hdmi=log)" : "HDMI mirrors the SE's screen (mac.hdmi=mirror)");
            else if (psmac_board_se && !psmac_board_solo && psmac_hdmi_mode == 1 && !no_declrom)
                kprintf("[PSMAC] declaration ROM: one screen, the SE's; HDMI mirrors it (a psmac-se-WxH declaration ROM gives HDMI a screen of its own)\n");
            if (!two) slot2_rom = 0;
            /* the screens' sizes into the ROM (declrom_resize): the HDMI card's for the HDMI size, a
               one-screen card's for its screen (the SE's 512 x 342 on the SE board, where it is the
               SE's; mac.screen or the firmware's size elsewhere). The SE file's own slot-9 screen is
               the SE's one bit and stays as it is. */
            if (two) rescreen_keep((uint8_t *)slot2_rom + slot2_len, slot2_len, 0x80, PSMB_SLOT2, PSMB_WIN_PHYS + 0x900000u + PSMB_OFF_BOUNCE2, PSMB_BOUNCE2_SIZE);
            if (two && !declrom_resize((uint8_t *)slot2_rom + slot2_len, 0x80, PSMB_SLOT2, screen_w, screen_h,
                                       PSMB_WIN_PHYS + 0x900000u + PSMB_OFF_BOUNCE2, PSMB_BOUNCE2_SIZE))
                kprintf("[PSMAC] declaration ROM: slot $A's video sResource not found; its screen stays as the file has it\n");
            if (!se_card && !no_declrom) {
                uint32_t w0 = psmac_board_se && !psmac_board_solo ? 512 : screen_w, h0 = psmac_board_se && !psmac_board_solo ? 342 : screen_h;
                if (!(psmac_board_se && !psmac_board_solo)) rescreen_keep(dr + PSMB_DECLROM_SIZE, declrom_len, 0x84, 9, PSMB_WIN_PHYS + 0x900000u + PSMB_OFF_BOUNCE, PSMB_BOUNCE_SIZE);
                declrom_resize(dr + PSMB_DECLROM_SIZE, 0x84, 9, w0, h0, PSMB_WIN_PHYS + 0x900000u + PSMB_OFF_BOUNCE, PSMB_BOUNCE_SIZE);
            }
            psmac_display_layout(se_card, two); }
    }
    psmac_mbox_init(win_phys, ram_phys, ram_size);
    { extern void psmac_videocard_init(void); if (vid_size) psmac_videocard_init(); }   /* its ROM and block, once the screens are known */
    if (slot2_rom) {   /* the window exists (and was cleared) now: slot $A's ROM into its top 16 KiB, right-aligned */
        uint8_t *w2 = (uint8_t *)(KERNEL_ALIAS + win_phys + PSMB_OFF_DECLROM2);
        memcpy(w2 + PSMB_DECLROM2_SIZE - slot2_len, slot2_rom, slot2_len);
        arm_flush_cache((uintptr_t)w2, PSMB_DECLROM2_SIZE);
        rescreen_top = w2 + PSMB_DECLROM2_SIZE;     /* the live copy of slot $A's ROM is this one from now on */
    }
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
void ps_send_reset(void)
{
    extern void psmac_card_reset(void), psmac_startup_reset(void), psmac_se_after_reset(void), psmac_hdmi_bus_reset(void);
    extern void psmac_fpu_reset(void);
    psmac_card_reset();
    psmac_fpu_reset();                     /* the exact FPU's registers and its null state (psmac_fpu.c) */
    psmac_hdmi_bus_reset();                /* PiDisplay: the Pi's restart, or a new rotation for the Slot Manager to read (psmac_hdmi.c) */
    psmac_startup_reset();                 /* the startup disk's view of parameter RAM (psmac_mbox.c) */
    ps_pulse_reset();
    psmac_se_after_reset();                /* the SE board: its VIA reset, its overlay back (psmac_board_se.c) */
}
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
static int con_mini;                    /* 1: the log goes to the mini UART, Bluetooth has the PL011 (bt_pi.c);
                                           2: no serial line at all (the classic PiStorm's GPIO transport owns GPIO 14/15) */
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
    if (con_mini == 2) return 1;                            /* no serial line: the ring drains into nothing */
    int room = con_mini ? (rd32le(AUX_MU_LSR) & 0x20) != 0 : !(rd32le(PL011_BASE + 0x18) & (1 << 5));
    return room && con_dead_test <= 0;                      /* the test still reads the port: the wait costs what it did */
}
static void con_send(uint8_t c)
{
    if (con_mini == 2) return;                              /* no serial line: GPIO 14/15 carry the PiStorm's data bus */
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
    if (con_stuck) kprintf("[PSMAC] the serial port is not sending (on a Pi 3 the PL011 belongs to Bluetooth until it starts): the log is on the screen and in Aurora68 Log\n");
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
    /* In a Macintosh, on the classic PiStorm, GPIO 8-23 are the 68000's data bus (psmac_gpio.c):
       GPIO 14 and 15 are D6 and D7, rewritten as outputs and inputs at every bus cycle. Putting them
       in ALT5 for the mini UART would hand two data lines to the UART between cycles, so there the
       log simply stops using a serial line once Bluetooth takes the PL011; the HDMI screen, PiLog
       and aurora68.log still get every character. (The bench Pi, mac.board=solo, has no PiStorm on
       those pins and keeps the mini UART.) */
    extern int psmac_gpio_active(void);
    if (psmac_gpio_active()) {
        spinlock_acquire(&con_lock);
        for (int i = 0; i < 100000 && !con_stuck && (rd32le(PL011_BASE + 0x18) & (1 << 3)); i++) ;   /* the PL011 drains what it has */
        con_mini = 2;
        spinlock_release(&con_lock);
        kprintf("[PSMAC] no serial log from here: Bluetooth has the PL011, and GPIO 14/15 are the PiStorm's data lines (HDMI, Aurora68 Log and aurora68.log still have it)\n");
        return;
    }
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
