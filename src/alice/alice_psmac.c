/* SPDX-License-Identifier: GPL-2.0-only
 *
 * alice_psmac.c — the Macintosh (PiStorm-SE / Aurora68) platform layer under the Alice68 modules.
 *
 * Backstory. The Alice68 kernel runs its hardware services (alice_net.c: WiFi and Gigabit Ethernet;
 * bt/: Bluetooth; xhci/: USB) on one ARM core, talking to AmigaOS through shared-memory boards.
 * Those modules expect a handful of things from the kernel around them: a way to interrupt the
 * 68k, places to deliver keyboard and mouse reports, a boot-status strip to write to, a reboot
 * routine, and autoconfig to place their boards. This file provides the Macintosh versions:
 *
 *   - the boards are carved out of the NuBus slot-$9 window (psmb.h); the Mac finds them through
 *     the window's service table, no autoconfig;
 *   - "interrupt the 68k" sets the board's irq flag only; Mac drivers poll from VBL tasks until the
 *     virtual slot interrupt is built (docs/07, interrupts);
 *   - keyboard/mouse reports go into an input ring (alice_psmac.h) that a Mac driver turns into
 *     ADB-style events;
 *   - the splash strip is the console log, and "reboot" is logged and ignored for now;
 *   - in the QEMU testbed, which has no GENET or WiFi chip, unit 1 of the network board is served
 *     from the host frame files (tools/pinet.py), so the Mac Ethernet driver can be developed
 *     against the same board protocol it will use on hardware.
 *
 * The services run on ARM core 3, which upstream Emu68 reserves for the PowerPC emulation we do
 * not use; core 2 stays the bus-transport owner so no Mac bus cycle waits for a WiFi transaction.
 */
#include <stdint.h>
#include <stdbool.h>
#include "support.h"
#include "psmac.h"
#include "psmb.h"
#include "alice_net.h"
#include "alice_psmac.h"

/* ---- time ---------------------------------------------------------------------------------- */
static inline uint64_t ticks(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t tick_hz(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }

/* ---- what the Alice68 modules call --------------------------------------------------------- */
static uint32_t irq_raised;
/* Alice68 raises the Amiga's PORTS interrupt here. The board's irq flag is already set by the
   caller; the Mac drivers poll it. Counted so the heartbeat can show activity. */
void aga_virtual_cia_irq(uint16_t bit, int active) { (void)bit; (void)active; irq_raised++; }

/* the boot splash's status strip: on the Mac build it is the console log */
void frf_post_wifi(const char *text, int col) { if (text && *text) kprintf("[WIFI] status: %s (%d)\n", text, col); }
void frf_post_eth(const char *text, int col)  { if (text && *text) kprintf("[ETH] status: %s (%d)\n", text, col); }
void frf_post_bt(const char *text, int col)   { if (text && *text) kprintf("[BT] status: %s (%d)\n", text, col); }
void frf_post_usb_devices(unsigned kb, unsigned ms, unsigned gp, unsigned msc) { kprintf("[USB] devices: %u keyboards %u mice %u pads %u storage\n", kb, ms, gp, msc); }
void frf_boot_stage(unsigned stage) { (void)stage; }
void aga_reboot(int tryboot) { (void)tryboot; kprintf("[PSMAC] reboot requested by a service module: ignored on the Macintosh build\n"); }

/* ---- input ring: keyboard and mouse reports for the Mac side ------------------------------- */
static volatile struct alice_input_ring *inr;
static void in_put(uint32_t type, int32_t a, int32_t b, int32_t c)
{
    if (!inr) return;
    uint32_t head = inr->head, tail = inr->tail;
    if (head - tail >= ALICE_IN_SLOTS) { inr->dropped++; return; }
    volatile struct alice_input_event *e = &inr->ev[head % ALICE_IN_SLOTS];
    e->type = type; e->a = a; e->b = b; e->c = c;
    __asm__ volatile("dmb ish" ::: "memory");
    inr->head = head + 1;
}
static inline uint64_t in_now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t in_hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }

/* Key repeat. An ADB keyboard repeats keys itself and the Mac's Event Manager turns the repeated
   downs into autoKey events; USB and Bluetooth keyboards only report state changes, so the repeat
   is generated here: the last key held is re-sent with b = 2 ("repeat") after the Mac's default
   delay, then at the default rate, until it is released. The Mac driver (mac/piinput.s) posts
   those as autoKey. The two times are the Keyboard control panel's defaults, so a user who has
   changed them will notice the Pi's keyboards keep their own rhythm; reading the panel's settings
   from low memory (KeyThresh, KeyRepThresh) is a small later improvement. */
static uint32_t rep_usage, rep_mods; static uint64_t rep_due;
#define REP_DELAY_MS 400
#define REP_RATE_MS  66
static void in_key(uint32_t usage, int down, uint32_t mods)
{
    in_put(ALICE_IN_KEY, (int32_t)usage, down, (int32_t)mods);
    if (!usage) return;
    if (down) { rep_usage = usage; rep_mods = mods; rep_due = in_now() + in_hz() * REP_DELAY_MS / 1000; }
    else if (usage == rep_usage) rep_usage = 0;
}
static void in_repeat_poll(void)
{
    if (!rep_usage || in_now() < rep_due) return;
    in_put(ALICE_IN_KEY, (int32_t)rep_usage, 2, (int32_t)rep_mods);
    rep_due = in_now() + in_hz() * REP_RATE_MS / 1000;
}

/* a USB boot-protocol keyboard report (modifiers, reserved, 6 usages): diff against the last one */
void frf_native_usb_keyboard_boot_report(const uint8_t report[8])
{
    static uint8_t last[8];
    for (int i = 2; i < 8; i++) {                    /* released: in last, not in report */
        if (!last[i]) continue;
        int still = 0; for (int j = 2; j < 8; j++) if (report[j] == last[i]) still = 1;
        if (!still) in_key(last[i], 0, report[0]);
    }
    for (int i = 2; i < 8; i++) {                    /* pressed: in report, not in last */
        if (!report[i]) continue;
        int was = 0; for (int j = 2; j < 8; j++) if (last[j] == report[i]) was = 1;
        if (!was) in_key(report[i], 1, report[0]);
    }
    if (report[0] != last[0]) in_put(ALICE_IN_KEY, 0, 0, report[0]);   /* modifiers alone */
    for (int i = 0; i < 8; i++) last[i] = report[i];
}
void frf_native_usb_keyboard_force_release(void) { static const uint8_t none[8]; frf_native_usb_keyboard_boot_report(none); }
int  frf_native_usb_reboot_requested(void) { return 0; }
int  frf_native_usb_amiga_reset_requested(void) { return 0; }
void aga_virtual_mouse_update(int dx, int dy, unsigned buttons) { if (dx || dy || buttons != 0xffffffffu) in_put(ALICE_IN_MOUSE, dx, dy, (int32_t)buttons); }
void pt_mouse_extra(int wheel, int pan, unsigned buttons) { (void)buttons; if (wheel || pan) in_put(ALICE_IN_WHEEL, wheel, pan, 0); }
volatile uint8_t frf_usb_boot_ready, frf_usb_mouse_seen, frf_usb_mouse_buttons, frf_usb_failed;
uintptr_t frf_dma_phys;                              /* xHCI DMA pool: not carved yet (USB is Pi 4 only) */

/* ---- testbed Ethernet: unit 1 served from the host frame files ------------------------------ */
/* psmac_mbox.c owns the host file pair (mac.net=): these are its primitives */
int psmac_netfile_up(void);
int psmac_netfile_send(const uint8_t *frame, uint32_t len);
int psmac_netfile_recv(uint8_t *buf, uint32_t max);      /* a frame's length, 0 if none */
static int tb_active = -1, tb_rx_on, tb_promisc, tb_allmulti;
static uint8_t tb_mac[6] = { 0x02, 0x50, 0x53, 0x4D, 0x41, 0x43 };
static uint8_t tb_buf[AN_SLOT_BYTES];
static inline uint32_t arg32(volatile struct an_unit *u, unsigned o) { return (uint32_t)u->arg[o] << 24 | (uint32_t)u->arg[o + 1] << 16 | (uint32_t)u->arg[o + 2] << 8 | u->arg[o + 3]; }
static void tb_state(volatile struct an_unit *u, uint32_t st, const char *msg)
{
    u->state = st;
    unsigned i = 0; for (; msg[i] && i < 95; i++) u->msg[i] = msg[i]; u->msg[i] = 0;
    __asm__ volatile("dmb ish" ::: "memory");
    u->state_seq++;
    volatile struct an_hdr *h = (volatile struct an_hdr *)((volatile uint8_t *)u - AN_OFF_UNIT(1));
    h->irq = 1;
}
int psmac_eth_testbed_poll(volatile struct an_unit *u)
{
    if (tb_active < 0) {
        tb_active = psmac_netfile_up();
        if (tb_active) {
            u->kind = AN_KIND_ETHER;
            for (int i = 0; i < 6; i++) { u->mac[i] = tb_mac[i]; u->factory_mac[i] = tb_mac[i]; }
            u->speed_mbps = 100; u->link = 1; u->cfg_adv = AN_LM_100F; u->cfg_flags = AN_EC_FULL; u->cfg_speed = 100;
            tb_state(u, AN_ST_IDLE, "testbed Ethernet: frames through the host files (tools/pinet.py)");
            kprintf("[ETH] unit 1 is the testbed file bridge (no GENET in QEMU)\n");
        }
    }
    if (!tb_active) return 0;
    /* commands */
    if (u->cmd_seq != u->cmd_done) {
        int r = 0;
        switch (u->cmd) {
        case AN_CMD_START: case AN_CMD_ENABLE: tb_state(u, AN_ST_CONNECTED, "up"); u->link = 1; break;
        case AN_CMD_OFF: case AN_CMD_DISCONNECT: tb_rx_on = 0; tb_state(u, AN_ST_OFF, "off"); break;
        case AN_CMD_RXON: tb_rx_on = 1; u->rx_on = 1; break;
        case AN_CMD_RXOFF: tb_rx_on = 0; u->rx_on = 0; break;
        case AN_CMD_FILTER: { uint32_t f = arg32(u, 0); tb_promisc = !!(f & AN_FILT_PROMISC); tb_allmulti = !!(f & AN_FILT_ALLMULTI); break; }
        case AN_CMD_SETMAC: {
            int zero = 1; for (int i = 0; i < 6; i++) if (u->arg[i]) zero = 0;
            for (int i = 0; i < 6; i++) tb_mac[i] = zero ? u->factory_mac[i] : u->arg[i];
            if (u->arg[6] == 1) for (int i = 3; i < 6; i++) tb_mac[i] = u->factory_mac[i];
            for (int i = 0; i < 6; i++) u->mac[i] = tb_mac[i];
            break; }
        case AN_CMD_ETHCFG: case AN_CMD_SCAN: case AN_CMD_JOIN: case AN_CMD_WIFICFG: break;
        default: r = -1;
        }
        u->cmd_result = r;
        __asm__ volatile("dmb ish" ::: "memory");
        u->cmd_done = u->cmd_seq;
        ((volatile struct an_hdr *)((volatile uint8_t *)u - AN_OFF_UNIT(1)))->irq = 1;
    }
    volatile uint8_t *board = (volatile uint8_t *)u - AN_OFF_UNIT(1);
    /* transmit ring: Mac -> host */
    while (u->tx_tail != u->tx_head) {
        volatile struct an_slot *s = (volatile struct an_slot *)(board + AN_OFF_TX(1) + (u->tx_tail % AN_TX_SLOTS) * AN_SLOT_BYTES);
        uint32_t len = s->len;
        if (len >= 14 && len <= AN_SLOT_BYTES - 8) {
            for (uint32_t i = 0; i < len; i++) tb_buf[i] = s->data[i];
            if (psmac_netfile_send(tb_buf, len)) { u->tx_pkts++; u->tx_bytes += len; } else u->tx_err++;
        } else u->tx_err++;
        __asm__ volatile("dmb ish" ::: "memory");
        u->tx_tail++;
    }
    /* receive ring: host -> Mac (only while the stack takes frames; drop when full) */
    for (int budget = 0; budget < 8; budget++) {
        int len = psmac_netfile_recv(tb_buf, AN_SLOT_BYTES - 8);
        if (len <= 0) break;
        if (!tb_rx_on) { u->rx_drop++; continue; }
        int bcast = tb_buf[0] == 0xff && tb_buf[1] == 0xff && tb_buf[2] == 0xff && tb_buf[3] == 0xff && tb_buf[4] == 0xff && tb_buf[5] == 0xff;
        int mcast = (tb_buf[0] & 1) && !bcast;
        int mine = tb_promisc || bcast || (mcast && tb_allmulti) ||
                   (tb_buf[0] == tb_mac[0] && tb_buf[1] == tb_mac[1] && tb_buf[2] == tb_mac[2] && tb_buf[3] == tb_mac[3] && tb_buf[4] == tb_mac[4] && tb_buf[5] == tb_mac[5]);
        if (!mine) continue;
        if (u->rx_head - u->rx_tail >= AN_RX_SLOTS) { u->rx_drop++; continue; }
        volatile struct an_slot *s = (volatile struct an_slot *)(board + AN_OFF_RX(1) + (u->rx_head % AN_RX_SLOTS) * AN_SLOT_BYTES);
        for (int i = 0; i < len; i++) s->data[i] = tb_buf[i];
        s->len = (uint32_t)len; s->flags = (bcast ? AN_SF_BCAST : 0) | (mcast ? AN_SF_MCAST : 0);
        __asm__ volatile("dmb ish" ::: "memory");
        u->rx_head++; u->rx_pkts++; u->rx_bytes += (uint32_t)len;
        ((volatile struct an_hdr *)board)->irq = 1;
    }
    return 1;
}

/* ---- testbed input script (mac.inputtest=SECONDS) ------------------------------------------- */
/* QEMU's Pi has no USB or Bluetooth, so the input path is exercised with a scripted session: the
   given number of seconds after the 68k starts (long enough for the Finder), the "mouse" walks to
   the top-left corner, across to the Apple menu, clicks it open, and a moment later the "keyboard"
   types a few letters, with the whole thing visible in the MAME screenshots. On hardware the
   option is simply not given. */
static uint32_t intest_at; static int intest_step; static uint64_t intest_next;
static void in_testbed_poll(void)
{
    if (!intest_at) return;
    uint64_t now = in_now();
    if (!intest_next) intest_next = now + in_hz() * intest_at;
    if (now < intest_next) return;
    static const uint8_t typed[] = { 0x0b, 0x08, 0x0f, 0x0f, 0x12, 0 };     /* h e l l o in USB usages */
    int s = intest_step++;
    uint32_t gap_ms = 50;
    if (s < 40)       aga_virtual_mouse_update(-30, -20, 0);                 /* to the top-left corner */
    else if (s < 42)  aga_virtual_mouse_update(12, 6, 0);                    /* onto the Apple menu */
    else if (s == 42) { aga_virtual_mouse_update(0, 0, 1); gap_ms = 300; }   /* press */
    else if (s == 43) { aga_virtual_mouse_update(0, 0, 0); gap_ms = 1500; }  /* release: the menu stays open on System 7 */
    else if (s == 44) { aga_virtual_mouse_update(0, 0, 1); gap_ms = 200; }   /* click again to close it */
    else if (s == 45) { aga_virtual_mouse_update(0, 0, 0); gap_ms = 500; }
    else if (s < 46 + 2 * (int)(sizeof typed - 1)) {                        /* type: down, up per letter */
        int k = (s - 46) / 2, down = !((s - 46) & 1);
        in_key(typed[k], down, 0); gap_ms = 80;
    } else { intest_at = 0; kprintf("[PSMAC] input test script done\n"); return; }
    intest_next = now + in_hz() * gap_ms / 1000;
}
void psmac_alice_input_test(uint32_t seconds) { intest_at = seconds; }

/* ---- bring-up and the core-3 loop ---------------------------------------------------------- */
static volatile int alice_ready;
void psmac_alice_init(uint8_t *win)
{
    alice_net_attach(win + ALICE_NET_WINDOW_OFF);
    alice_status_attach(win + ALICE_STATUS_WINDOW_OFF);
    inr = (volatile struct alice_input_ring *)(win + ALICE_INPUT_WINDOW_OFF);
    for (uint32_t i = 0; i < sizeof(struct alice_input_ring) / 4; i++) ((volatile uint32_t *)inr)[i] = 0;
    inr->magic = ALICE_IN_MAGIC; inr->version = 1; inr->slots = ALICE_IN_SLOTS;
    __asm__ volatile("dmb ish" ::: "memory");
    alice_ready = 1;
}
uint32_t psmac_alice_irqs(void) { return irq_raised; }

/* core 3: the Alice68 services plus the mailbox services, for ever */
void __attribute__((noreturn)) psmac_alice_core(void)
{
    while (!__atomic_load_n(&alice_ready, __ATOMIC_ACQUIRE)) __asm__ volatile("yield");
    kprintf("[PSMAC] core 3: Alice68 services (network board, Bluetooth, status) and the mailbox\n");
    for (;;) {
        psmac_mbox_poll();
        alice_net_poll();
        in_repeat_poll();
        in_testbed_poll();
        /* The card's interrupt line: up while a board has something the Mac has not taken (the
           network board's irq flag, which the Mac's Ethernet driver clears; input events not yet
           read), but only from boards whose Mac driver has attached (the Ethernet driver writes
           its version into the board header, the input driver sets the ring's attached word).
           The first version raised the line unconditionally: the ROM enables every slot's
           interrupt early in the boot, no driver was open to clear anything, and the level-2
           storm kept the Mac in its boot chime for good. */
        { extern volatile uint8_t *alice_net_board(void); extern void psmac_slot_irq(int);
          volatile struct an_hdr *nh = (volatile struct an_hdr *)alice_net_board();
          int line = (nh && nh->amiga_ver && nh->irq) || (inr && inr->attached && inr->head != inr->tail);
          psmac_slot_irq(line); }
        __asm__ volatile("yield");
    }
}

/* the testbed Ethernet owns the host frame files once it has come up */
int psmac_alice_owns_net(void) { return tb_active > 0; }
