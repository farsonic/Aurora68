/* psmac: Macintosh host layer for the Emu68 fork (Aurora68). MPL-2.0. */
#ifndef PSMAC_H
#define PSMAC_H
#include <stdint.h>

/* Bridge protocol (docs/05-bridge-protocol.md) */
#define PSMAC_OP_READ      1
#define PSMAC_OP_WRITE     2
#define PSMAC_OP_POLL      3
#define PSMAC_OP_RESET_ACK 4
#define PSMAC_FLAG_POSTED  1

/* IIci physical map handled locally vs forwarded to the bridge */
#define PSMAC_FB_SIZE      0x00050000u   /* IIci reserves the first 320 KiB of bank A for RBV video (low memory starts at 0x50000) */
/* The Pi peripherals are mapped twice. Once in the first 4 GiB at 0x3F000000 (start_rpi64.c), where
   upstream Emu68 keeps them so a 68k driver can reach them; and once more here, high in the kernel
   half of the address space, for the kernel's own use from every core. The second mapping exists
   because the Macintosh MMU shim (psmac_mmu.c) swaps the whole of the first 4 GiB when the ROM
   switches translation tables: break-before-make, so for a few microseconds nothing below 4 GiB
   is mapped on any core. The bus core (its console, on hardware the GPIO transport) and the service
   core (the firmware mailbox, SDIO, the Bluetooth UART) touched the peripherals through the low
   window and, now and then, fell into that gap: a data abort on a core whose exception handler
   wants the console lock that the aborted print still holds, and the whole machine stops
   mid-line (docs/08, item 21). Kernel-space addresses are never swapped; this one is above the
   kernel's 320 GiB alias of physical memory and below the shadow of the low tables. */
#define PSMAC_PERIPH_HI    0xffffffe800000000ull   /* clear of the JIT pools at 0xffffffe0..e2 and their read-only mirrors at 0xfffffff0..f2 (start.c) */
#define PSMAC_PERIPH_LOW   0x3f000000ull
#define PSMAC_ROM_BASE     0x40800000u
#define PSMAC_ROM_MIRROR   0x40000000u
#define PSMAC_ROM_SIZE     0x00080000u
#define PSMAC_IO_BASE      0x50000000u
#define PSMAC_IO_END       0x51000000u
#define PSMAC_SLOT_BASE    0xF0000000u

struct psmac_reply { uint8_t status, ipl, flags; uint32_t data; };

/* Interrupts as the 68k takes them (ExecutionLoop.c's ProcessIRQ fills it, the heartbeat prints and
   clears it; 20 November 2026, docs/08 item 76). ProcessIRQ runs with the 68k's registers in fixed
   ARM registers and may not call C, so it only counts here. Two signs of a level the JIT saw after
   its source had gone: the same level taken again at the same PC with the same stack (an RTE went
   straight back into the handler, nothing in between), and the same level taken at the same PC
   with the stack lower by the same stride each time (a handler lowered the mask and took its own
   interrupt again, docs/08 item 48), whose depth in bytes is kept. */
/* The 68k's interrupt level as another core stores it (27 November 2026, docs/23 section 20). A 68k that has executed STOP
   sleeps in the translated code's WFE until the context's interrupt word is not zero (M68k_LINE4.c, EMIT_STOP), and a
   WFE sleeps until an event. Upstream's housekeeper sends one (SEV) with every level it stores; this fork's stores did
   not, so on a real Pi a stopped 68k woke only by accident (an SEV some other part sent, a pending ARM interrupt): QEMU
   treats WFE as a no-op and never showed it. Mac OS does not use STOP; A/UX's idle loop is one, and on the owner's SE,
   past its first processes, A/UX slept in it with the clock's interrupt asserted and nothing to wake it. An event is
   sent when the stored level is not zero and differs from the one before. (Core 0's own timer event stream, set on at
   the 68k's start, is the second line: at most 0.85 ms.) */
#define PSMAC_STORE_IPL(st, v) do { \
        unsigned psmac_new_ipl_ = (v), psmac_old_ipl_ = (st)->INTF.IPL; \
        (st)->INTF.IPL = (uint8_t)psmac_new_ipl_; \
        if (psmac_new_ipl_ && psmac_new_ipl_ != psmac_old_ipl_) __asm__ volatile("dsb ishst; sev" ::: "memory"); \
    } while (0)

struct psmac_irqwatch {
    uint32_t taken[8];                 /* by level */
    uint32_t again;                    /* taken again at once: same level, PC and stack */
    uint32_t deepest, deepest_pc;      /* the deepest run of nested entries, in bytes of stack, and where */
    uint32_t last_level, last_pc, last_sp, run_sp, stride;
    uint32_t run;                      /* entries taken again at once in a row (psmac_board_se.c's storm guard) */
    uint32_t nest;                     /* entries in a row at the same PC, each deeper than the last: a handler taking itself again (the same) */
};
extern struct psmac_irqwatch psmac_irqwatch;

/* mac.mmu=full, phase 3 (psmac_mmu.c, docs/23-mmu.md section 12): the user and supervisor views of the current
   68030 configuration, as TTBR0_EL1 values; split is 0 while both modes share one space. The translated code
   switches between them where it changes the S bit (psmac_mmu_emit_view), the C side with psmac_view_for. guard is
   set while ProcessIRQ or M68K_Exception push a frame or fetch a vector, so that a fault there is the 68030's double
   bus fault (psmac_mmufault.c) and a supervisor access whatever the SR still says. cur is what TTBR0_EL1 holds, kept
   with every write of it, for the code cache's lookups (a load is cheaper than an MRS, under QEMU much cheaper). Inline, without a call: both of
   those are compiled with the 68k's registers fixed and may not call ordinary C. */
struct psmac_mmu_views { uint64_t ttbr[2]; uint32_t split; volatile uint32_t guard; uint64_t cur; };
extern struct psmac_mmu_views psmac_views;
static inline void psmac_view_for(uint32_t sr)
{
    if (psmac_views.split) {
        uint64_t t = psmac_views.ttbr[(sr >> 13) & 1];
        __asm__ volatile("msr TTBR0_EL1, %0\n\tisb" : : "r"(t) : "memory");
        psmac_views.cur = t;
    }
}

// mac.usertrace=PATH (the testbed, with mac.mmu=full; docs/23 section 21): a ring of the last PSMAC_UTRACE_N
// blocks entered in user mode (PC, SR, A7, A6, A0, A1, D0, D1) and the bus errors among them, written to the
// host with the Mac's memory (PATH.ram) the first time a user program's write below $1000, or any access of
// its at $BAD0xxxx (a pointer read from an empty bus), is refused. The 68k waits meanwhile.
#define PSMAC_UTRACE_N 131072u
struct psmac_utrace {
    uint32_t* buf;
    uint32_t n;
    volatile uint32_t dump;
    uint32_t fired;
};
extern struct psmac_utrace psmac_utrace;

void     psmac_setup_memory(void *rom, uint32_t rom_size);
int      psmac_bus_write(uint64_t value, uint64_t value2, int size, uint64_t far);
int      psmac_bus_read(uint64_t *value, uint64_t *value2, int size, uint64_t far);
uint32_t psmac_io_read(uint32_t addr, int size);
void     psmac_io_write(uint32_t addr, int size, uint32_t data, int posted);
void     psmac_service_core(void);     /* runs forever on the service core */
void     psmac_pmove(uint32_t ea, uint32_t ext);
void     psmac_mmu_init(uintptr_t ram_phys, uint32_t ram_size, uintptr_t rom_phys);
uint32_t psmac_mmu_l2p(uint32_t logical, int *valid);
void     psmac_slot_irq(int on);                 /* psmac_bus.c: the virtual slot-$9 interrupt line */
uint32_t psmac_ram_size(void);
uint32_t psmac_mac_ram_mb(void);              /* the same in MiB */
/* Mac-side RAM ceiling: the 32-bit map puts RAM at 0x00000000..0x3FFFFFFF, below the ROM at 0x40000000.
   The IIci ROM itself only sizes two 64 MiB banks; anything above 128 MiB needs the memory-sizing
   patch described in docs/06-rom-patches.md. */
#define PSMAC_RAM_MAX_MB 1024u
#define PSMAC_RAM_DEFAULT_MB 128u    /* without mac.ram=: both of the IIci's banks full, the most the ROM sizes unpatched (docs/08 items 24, 27) */
#define PSMAC_CARD_IMAGE_KEEP_MB 112u /* kept free for a disk image on the card: a 96 MB System 7 image and the loader's slack */
#define PSMAC_PI_SLACK_MB     16u    /* kept free after the disk images (psmac_card.c's take_memory) */
void     psmac_select_transport(int semihost_mode);
/* slot $9 mailbox (psmb.h, psmac_mbox.c) */
void     psmac_mmu_set_slot(uintptr_t win_phys, uintptr_t declrom_phys);
void     psmac_mbox_init(uintptr_t window_phys, uintptr_t ram_phys, uint32_t ram_size);
int      psmac_mbox_poll(void);
void     psmac_mbox_heartbeat(void);
/* the boot timeline (psmac_boot.c): "[BOOT] t=N.NNNs what", t from power-on on real hardware */
void     psmac_boot_entry(void);
void     psmac_boot_peripherals(void);
void     psmac_boot_console(void);
void     psmac_boot_mark(const char *fmt, ...);
uint64_t psmac_boot_now_us(void);
void     psmac_boot_pc(uint32_t pc);
void     psmac_boot_mac_line(const char *text);
void     psmac_boot_curapname(const uint8_t *pascal_name);
void     psmac_boot_check_args(const char *bootargs);
void     psmac_redact(const char *in, char *out, uint32_t cap);    /* psmac_card.c: wifi.psk=*** and the like */
/* the HDMI splash (psmac_display.c, docs/08 item 128): its status line, what the boot is doing now, and its alert
   line, what a user must see (kind 1: the WiFi's, cleared when it connects); no effect where HDMI shows the log */
void     psmac_splash_status(const char *fmt, ...);
void     psmac_splash_alert(int kind, const char *fmt, ...);

/* What the service core is doing besides the Mac's own requests (25 November 2026, docs/08 item 146):
   each errand says so as it starts (psmac_errand), and a request of the Mac's to the IWM that waited more
   than 10 us is filed under the errand that was running when it was posted (psmac_bus.c, the
   [PSMAC:IWM] stalls). */
enum { PSMAC_ERR_LOOP, PSMAC_ERR_HEARTBEAT, PSMAC_ERR_SOUND, PSMAC_ERR_SOUNDLINES, PSMAC_ERR_ROWS, PSMAC_ERR_CHECK,
       PSMAC_ERR_PLUS, PSMAC_ERR_POLL, PSMAC_ERR_SPEED, PSMAC_ERR_GUEST, PSMAC_ERR_N };
void psmac_errand(int e);                  /* the service core only */

#endif
