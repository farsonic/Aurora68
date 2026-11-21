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
 *     against the same board protocol it will use on hardware; and unit 0 can be a simulated
 *     radio (mac.wifisim=) that scans, joins and carries frames through a second file pair, so
 *     the WiFi driver, PiWiFi and MacTCP over WiFi can be developed the same way.
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
#include "../psmac/psmac_ip.h"

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
/* mac.altcmd=1: a PC keyboard's Alt is Command and its Windows key Option, by position (the key
   beside the space bar is Command on a Mac keyboard). Without it Apple's own mapping holds:
   Windows is Command, Alt is Option (mac/piinput.s hid_modifiers). Applied to the HID modifier
   byte of every key event, USB and Bluetooth alike. */
static int in_altcmd = -1;
/* one event into the ring as it is: the remote's (psmac_remote.c) come here directly, since its
   Command is Command whatever keyboard the bench has */
void psmac_input_put(uint32_t type, int32_t a, int32_t b, int32_t c)
{
    if (!inr) return;
    uint32_t head = inr->head, tail = inr->tail;
    if (head - tail >= ALICE_IN_SLOTS) { inr->dropped++; return; }
    volatile struct alice_input_event *e = &inr->ev[head % ALICE_IN_SLOTS];
    e->type = type; e->a = a; e->b = b; e->c = c;
    __asm__ volatile("dmb ish" ::: "memory");
    inr->head = head + 1;
}
int psmac_input_attached(void) { return inr && inr->attached; }
uint32_t psmac_input_backlog(void) { return inr && inr->attached ? inr->head - inr->tail : 0; }   /* events .PiInput has not taken yet */
static void in_put(uint32_t type, int32_t a, int32_t b, int32_t c)
{
    if (type == ALICE_IN_KEY) {
        if (in_altcmd < 0) { extern const char *psmac_option(const char *); const char *o = psmac_option("mac.altcmd"); in_altcmd = o && o[0] == '1'; }
        if (in_altcmd) {
            uint32_t m = (uint32_t)c;
            m = (m & ~0xccu) | (m & 0x44u) << 1 | (m & 0x88u) >> 1;      /* L/R Alt (bits 2, 6) <-> L/R GUI (3, 7) */
            c = (int32_t)m;
        }
    }
    psmac_input_put(type, a, b, c);
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
void psmac_netprobe_tx(int unit, const volatile uint8_t *f, uint32_t len, int stage);
void psmac_netprobe_rx(int unit, const volatile uint8_t *f, uint32_t len, uint32_t slot, volatile uint32_t *rx_tail);
/* psmac_mbox.c owns the host file pairs (0: mac.net=, 1: mac.wifinet=): these are its primitives */
int psmac_netfile_up(int which);
int psmac_netfile_send(int which, const uint8_t *frame, uint32_t len);
int psmac_netfile_recv(int which, uint8_t *buf, uint32_t max);      /* a frame's length, 0 if none */
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
        tb_active = psmac_netfile_up(0);
        if (tb_active) {
            u->kind = AN_KIND_ETHER;
            for (int i = 0; i < 6; i++) { u->mac[i] = tb_mac[i]; u->factory_mac[i] = tb_mac[i]; }
            u->speed_mbps = 100; u->link = 1; u->cfg_adv = AN_LM_100F; u->cfg_flags = AN_EC_FULL; u->cfg_speed = 100;
            tb_state(u, AN_ST_IDLE, "testbed Ethernet: frames through the host files (tools/pinet.py)");
            kprintf("[ETH] unit 1 is the testbed file bridge (no GENET in QEMU)\n");
        }
    }
    /* mac.netdown=S1[,S2]: the bridge's cable is pulled S1 seconds after the Pi started and put
       back at S2 (the testbed's way to show a stack failing over to the WiFi, docs/08 item 49) */
    if (tb_active > 0) {
        static int nd_parsed; static uint32_t nd_down, nd_up;
        if (!nd_parsed) {
            extern const char *psmac_option(const char *);
            const char *o = psmac_option("mac.netdown");
            nd_parsed = 1;
            for (; o && *o >= '0' && *o <= '9'; o++) nd_down = nd_down * 10 + (uint32_t)(*o - '0');
            if (o && *o == ',') for (o++; *o >= '0' && *o <= '9'; o++) nd_up = nd_up * 10 + (uint32_t)(*o - '0');
        }
        uint32_t t = (uint32_t)(in_now() / in_hz());
        int want = !(nd_down && t >= nd_down && (!nd_up || t < nd_up));
        if (want != (int)u->link && u->state != AN_ST_OFF) {
            u->link = (uint32_t)want;
            u->speed_mbps = want ? 100 : 0;
            if (!want) u->link_downs++;
            tb_state(u, want ? AN_ST_CONNECTED : AN_ST_IDLE, want ? "testbed Ethernet: cable back in (mac.netdown)" : "testbed Ethernet: cable pulled (mac.netdown)");
            kprintf("[ETH] testbed bridge: link %s at %u s (mac.netdown)\n", want ? "up" : "down", t);
        }
    }
    if (!tb_active) {
        /* no file bridge: on a Pi 3B or 3B+ the wired port is the USB network chip (psmac_usbnet.c);
           on a Pi 4 this answers 0 and alice_net.c drives the GENET */
        extern int psmac_usbnet_poll(volatile struct an_unit *u);
        return psmac_usbnet_poll(u);
    }
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
        if (len >= 14 && len <= AN_SLOT_BYTES - 8 && u->link) {
            for (uint32_t i = 0; i < len; i++) tb_buf[i] = s->data[i];
            psmac_ip_mac_tx(1, tb_buf, len);
            psmac_netprobe_tx(1, tb_buf, len, 0);
            if (psmac_netfile_send(0, tb_buf, len)) { u->tx_pkts++; u->tx_bytes += len; psmac_netprobe_tx(1, tb_buf, len, 1); } else u->tx_err++;
        } else u->tx_err++;
        __asm__ volatile("dmb ish" ::: "memory");
        u->tx_tail++;
    }
    /* the kernel's own frames (psmac_ip.c), once the Mac's are out */
    if (psmac_ip_active && u->link) {
        uint32_t len;
        while ((len = psmac_ip_tx_take(1, tb_buf, sizeof tb_buf)))
            psmac_netfile_send(0, tb_buf, len);
    }
    /* receive ring: host -> Mac (only while the stack takes frames; drop when full) */
    for (int budget = 0; budget < 8; budget++) {
        int len = psmac_netfile_recv(0, tb_buf, AN_SLOT_BYTES - 8);
        if (len <= 0) break;
        if (psmac_ip_active && u->link && psmac_ip_rx(1, tb_buf, (uint32_t)len, tb_mac, tb_promisc) != IPK_TO_MAC) continue;
        if (!tb_rx_on || !u->link) { u->rx_drop++; continue; }
        int bcast = tb_buf[0] == 0xff && tb_buf[1] == 0xff && tb_buf[2] == 0xff && tb_buf[3] == 0xff && tb_buf[4] == 0xff && tb_buf[5] == 0xff;
        int mcast = (tb_buf[0] & 1) && !bcast;
        int mine = tb_promisc || bcast || (mcast && tb_allmulti) ||
                   (tb_buf[0] == tb_mac[0] && tb_buf[1] == tb_mac[1] && tb_buf[2] == tb_mac[2] && tb_buf[3] == tb_mac[3] && tb_buf[4] == tb_mac[4] && tb_buf[5] == tb_mac[5]);
        if (!mine) continue;
        if (u->rx_head - u->rx_tail >= AN_RX_SLOTS) { u->rx_drop++; continue; }
        volatile struct an_slot *s = (volatile struct an_slot *)(board + AN_OFF_RX(1) + (u->rx_head % AN_RX_SLOTS) * AN_SLOT_BYTES);
        for (int i = 0; i < len; i++) s->data[i] = tb_buf[i];
        s->len = (uint32_t)len; s->flags = (bcast ? AN_SF_BCAST : 0) | (mcast ? AN_SF_MCAST : 0);
        psmac_netprobe_rx(1, tb_buf, (uint32_t)len, u->rx_head, &u->rx_tail);
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
static uint32_t intest_at, intest_kind; static int intest_step; static uint64_t intest_next;
static int click_pt[8][3], click_n, click_i, click_phase;   /* mac.inputclick=: x, y, Option */
/* mac.inputabout=SECONDS: a second script, for the Finder: the mouse walks to the Apple menu,
   presses, moves down to the first item and releases, which under System 6 and 7 opens the
   "About This Macintosh" window (the Finder must be in front: no startup items). */
static void in_about_step(int s)
{
    uint32_t gap_ms = 50;
    if (s < 40)       aga_virtual_mouse_update(-30, -20, 0);
    else if (s < 42)  aga_virtual_mouse_update(8, 5, 0);                     /* onto the Apple menu title */
    else if (s == 42) { aga_virtual_mouse_update(0, 0, 1); gap_ms = 300; }   /* press */
    else if (s < 45)  { aga_virtual_mouse_update(0, 6, 1); gap_ms = 120; }   /* drag down onto "About...": y 28, inside the first item (20..35); 40 was the separator under it */
    else if (s == 45) { aga_virtual_mouse_update(0, 0, 0); gap_ms = 500; }   /* release: the item runs */
    else { intest_at = 0; kprintf("[PSMAC] input about script done\n"); return; }
    intest_next = in_now() + in_hz() * gap_ms / 1000;
}
/* typing a string, one HID report per call: the letters, digits and punctuation PiWiFi's prompts
   take; capitals and the shifted marks carry the left-shift modifier (0x02) */
static const char *type_str; static int type_down;
static int in_type_poll(uint32_t *gap_ms)
{
    if (!type_str || !*type_str) return 0;
    char c = *type_str; uint32_t usage = 0x2c /* space */, mods = 0;
    if (c >= 'a' && c <= 'z') usage = 0x04 + (uint32_t)(c - 'a');
    else if (c >= 'A' && c <= 'Z') { usage = 0x04 + (uint32_t)(c - 'A'); mods = 0x02; }
    else if (c >= '1' && c <= '9') usage = 0x1e + (uint32_t)(c - '1');
    else if (c == '0') usage = 0x27;
    else if (c == '\r') usage = 0x28;
    else if (c == '-') usage = 0x2d;
    else if (c == '_') { usage = 0x2d; mods = 0x02; }
    else if (c == '.') usage = 0x37;
    if (!type_down) { in_key(usage, 1, mods); type_down = 1; *gap_ms = 60; }
    else { in_key(usage, 0, mods); type_down = 0; type_str++; *gap_ms = 100; }
    return 1;
}
/* mac.inputwifi=SECONDS: a third script, for PiWiFi running as the startup application with the
   simulated radio (mac.wifisim=): S scans, then J joins the network the radio knows, typing its
   name and password at PiWiFi's prompts. The screenshots show the scan list, then "connected". */
static const char *wsim_script_ssid(void); static const char *wsim_script_pass(void);
static char type_line[72]; static int wifi_phase;
static void in_wifi_step(void)
{
    uint32_t gap_ms = 50;
    if (in_type_poll(&gap_ms)) { intest_next = in_now() + in_hz() * gap_ms / 1000; return; }
    const char *src = 0;
    switch (wifi_phase++) {
    case 0: type_str = "s"; gap_ms = 10; break;                /* scan */
    case 1: gap_ms = 20000; break;                             /* the scan takes two seconds, the redraw of the list through the bridge much longer;
                                                                  the system event queue holds twenty events, so typing ahead loses keys */
    case 2: type_str = "j"; gap_ms = 10; break;                /* join: PiWiFi asks for the name */
    case 3: gap_ms = 4000; break;
    case 4: src = wsim_script_ssid(); break;
    case 5: gap_ms = 4000; break;                              /* then the password */
    case 6: src = wsim_script_pass(); break;
    case 7: gap_ms = 10000; break;                             /* joining, connected, redrawn */
    default: intest_at = 0; kprintf("[PSMAC] input wifi script done\n"); return;
    }
    if (src) { unsigned i = 0; for (; src[i] && i < 64; i++) type_line[i] = src[i]; type_line[i++] = '\r'; type_line[i] = 0; type_str = type_line; gap_ms = 10; }
    intest_next = in_now() + in_hz() * gap_ms / 1000;
}
static void in_testbed_poll(void)
{
    if (!intest_at) return;
    uint64_t now = in_now();
    if (!intest_next) intest_next = now + in_hz() * intest_at;
    if (now < intest_next) return;
    if (intest_kind == 1) { in_about_step(intest_step++); return; }
    if (intest_kind == 2) { in_wifi_step(); return; }
    if (intest_kind == 4) {                                                 /* mac.inputdrag=: drag the first desktop icon onto the second screen */
        int s = intest_step++; uint32_t gap_ms = 50;
        if (s < 40)       aga_virtual_mouse_update(-30, -20, 0);               /* the corner */
        else if (s < 60)  aga_virtual_mouse_update(30, 2, 0);                  /* to (600, 40): the volume icon */
        else if (s == 60) { aga_virtual_mouse_update(0, 0, 1); gap_ms = 300; } /* press */
        else if (s < 86)  { aga_virtual_mouse_update(40, 6, 1); gap_ms = 80; } /* drag right, across the edge */
        else if (s == 86) { aga_virtual_mouse_update(0, 0, 0); gap_ms = 500; } /* drop */
        else { intest_at = 0; kprintf("[PSMAC] input drag script done\n"); return; }
        intest_next = now + in_hz() * gap_ms / 1000; return;
    }
    if (intest_kind == 5) {                                                 /* mac.inputclick=: clicks at points of the screen */
        int s = intest_step++; uint32_t gap_ms = 50;
        static int cx, cy;
        if (s == 0) cx = cy = 0;
        if (s < 40)       aga_virtual_mouse_update(-30, -20, 0);               /* the corner: (0, 0) */
        else if (cx != click_pt[click_i][0] || cy != click_pt[click_i][1]) {   /* to the point, 8 pixels a step */
            int dx = click_pt[click_i][0] - cx, dy = click_pt[click_i][1] - cy;
            if (dx > 8) dx = 8;
            if (dx < -8) dx = -8;
            if (dy > 8) dy = 8;
            if (dy < -8) dy = -8;
            aga_virtual_mouse_update(dx, dy, 0); cx += dx; cy += dy;
        }
        else if (click_phase == 0) { click_phase = 1; aga_virtual_mouse_update(0, 0, click_pt[click_i][2] ? 4 : 1); gap_ms = 200; }   /* press (the middle button: .PiInput makes it an Option-click) */
        else if (click_phase == 1) { click_phase = 2; aga_virtual_mouse_update(0, 0, 0); gap_ms = 200; }      /* release */
        else {
            kprintf("[PSMAC] input click script: clicked (%d, %d)\n", click_pt[click_i][0], click_pt[click_i][1]);
            if (++click_i >= click_n) { intest_at = 0; kprintf("[PSMAC] input click script done\n"); return; }
            click_phase = 0; gap_ms = 3000;                                   /* the next point after three seconds */
        }
        intest_next = now + in_hz() * gap_ms / 1000; return;
    }
    if (intest_kind == 3) {                                                 /* mac.inputright=: walk the mouse onto the second screen */
        int s = intest_step++;
        if (s < 60) { aga_virtual_mouse_update(30, 4, 0); intest_next = now + in_hz() * 50 / 1000; }
        else { intest_at = 0; kprintf("[PSMAC] input right script done\n"); }
        return;
    }
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
void psmac_alice_input_test(uint32_t seconds) { intest_at = seconds; intest_kind = 0; }
void psmac_alice_input_about(uint32_t seconds) { intest_at = seconds; intest_kind = 1; }
void psmac_alice_input_wifi(uint32_t seconds)  { intest_at = seconds; intest_kind = 2; }
void psmac_alice_input_right(uint32_t seconds) { intest_at = seconds; intest_kind = 3; }
/* mac.inputclick=SECONDS,X,Y[,o][,X,Y[,o]...]: the mouse walks to the corner, then to each point of
   the main screen in turn and clicks there, three seconds apart; an Option-click for ",o" (the
   middle button, which .PiInput turns into one). Up to eight points. */
void psmac_alice_input_click(uint32_t seconds, const int *pts, int n)
{
    intest_at = seconds; intest_kind = 5; click_n = n > 8 ? 8 : n; click_i = 0; click_phase = 0;
    for (int i = 0; i < click_n; i++) { click_pt[i][0] = pts[3 * i]; click_pt[i][1] = pts[3 * i + 1]; click_pt[i][2] = pts[3 * i + 2]; }
}
void psmac_alice_input_drag(uint32_t seconds)  { intest_at = seconds; intest_kind = 4; }   /* the cursor walks right, onto the second screen (docs/10) */

void psmac_loop_report(void);
/* heartbeat line for psmac_bus.c: the interrupt line and what holds it */
void psmac_slot_irq_report(void)
{
    { extern void psmac_display_report(void); psmac_display_report(); }
    { extern void psmac_audio_report(void); psmac_audio_report(); }
    { extern void psmac_usb_report(void); psmac_usb_report(); }
    { extern void psmac_usbnet_report(void); psmac_usbnet_report(); }
    psmac_loop_report();
    extern uint32_t psmac_slot_irq_state(void); extern volatile uint8_t *alice_net_board(void);
    volatile struct an_hdr *nh = (volatile struct an_hdr *)alice_net_board();
    uint32_t st = psmac_slot_irq_state();
    kprintf("[PSMAC]   slot-9 line %u (SIER %02x IER %02x) | net irq %u drv %08x | input attached %u head %u tail %u\n",
            st >> 16, (st >> 8) & 0xff, st & 0xff, nh ? nh->irq : 0, nh ? nh->amiga_ver : 0,
            inr ? inr->attached : 0, inr ? inr->head : 0, inr ? inr->tail : 0);
}

/* ---- testbed WiFi: unit 0 as a simulated radio ------------------------------------------------ */
/* QEMU's raspi3b has no WiFi chip, so the real driver (alice_net.c) can only report "off" there.
   Everything above the chip, though, is worth exercising in the testbed: the Mac's WiFi driver
   (.Network_WiFi_PiStorm, enet.s assembled with WIFI), PiWiFi's scan/join/disconnect dialogue
   through the command box, MacTCP picking the WiFi card, and the rings that carry the frames.
   This is a radio that exists only in software: mac.wifisim=SSID:PASSWORD names the one network
   that lets the Mac in; a scan always finds it among a few canned neighbours (a WEP network, an
   open hotspot, a WPA2 network with a wrong password), a join checks the name and the password
   the way the firmware's four-way handshake would, and once "connected" the frames go through
   the second host file pair (mac.wifinet=BASE, served by tools/pinet.py like the Ethernet's).
   State changes take a second or so, as the real ones do, so the programs' waiting code runs.
   Nothing here touches the real driver: alice_net.c asks whether the simulation owns unit 0
   before it would start the WiFi thread, and only mac.wifisim= makes it say yes. */
#define SEC_OPEN 0x01
#define SEC_WEP  0x02
#define SEC_WPA2 0x08                                    /* wifi/fmac.h's bits, the ones PiWiFi names */
const char *psmac_option(const char *key);              /* psmac_bus.c: value of key= in bootargs, or NULL */
struct wsim_net { const char *ssid; const char *pass; int rssi; unsigned channel, security; uint8_t bssid[6]; };
static struct wsim_net wsim_nets[4] = {
    { "Aurora68", "pistorm68", -47, 6,  SEC_WPA2, { 0x02, 0x50, 0x53, 0xAA, 0x00, 0x01 } },   /* replaced by mac.wifisim= */
    { "Macintosh Garden", "(unknown)", -71, 11, SEC_WPA2, { 0x00, 0x1D, 0x7E, 0x4D, 0x47, 0x11 } },
    { "LINKSYS", "(unknown)", -66, 1,  SEC_WEP,  { 0x00, 0x0C, 0x41, 0x5E, 0x33, 0x02 } },
    { "Free Hotspot", "", -82, 11, SEC_OPEN, { 0x00, 0x1E, 0xC2, 0x9B, 0x00, 0xFA } },
};
static char wsim_ssid[36], wsim_pass[68];                 /* the network mac.wifisim= names */
static int wsim_active = -1, wsim_rx_on, wsim_promisc, wsim_allmulti, wsim_want;
static uint8_t wsim_mac[6] = { 0x02, 0x50, 0x53, 0x57, 0x49, 0x46 };   /* "PSWIF", locally administered */
static uint8_t wsim_buf[AN_SLOT_BYTES];
static uint64_t wsim_due;                                 /* when the pending state change lands */
static uint32_t wsim_next_state, wsim_pending_seq; static const char *wsim_next_msg; static int wsim_next_result;
static char wsim_join_ssid[36], wsim_join_pass[68];
static void wsim_state(volatile struct an_unit *u, uint32_t st, const char *msg)
{
    u->state = st;
    unsigned i = 0; for (; msg[i] && i < 95; i++) u->msg[i] = msg[i]; u->msg[i] = 0;
    __asm__ volatile("dmb ish" ::: "memory");
    u->state_seq++;
    ((volatile struct an_hdr *)((volatile uint8_t *)u - AN_OFF_UNIT(0)))->irq = 1;
    kprintf("[WIFI-SIM] %s\n", msg);
}
static void wsim_finish(volatile struct an_unit *u, uint32_t seq, int result)
{
    u->cmd_result = result;
    __asm__ volatile("dmb ish" ::: "memory");
    u->cmd_done = seq;
    ((volatile struct an_hdr *)((volatile uint8_t *)u - AN_OFF_UNIT(0)))->irq = 1;
}
static int wsim_streq(volatile const char *a, const char *b) { unsigned i = 0; for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0; return a[i] == b[i]; }
/* after a delay: enter a state, finish the command that asked for it (seq 0: none pending) */
static void wsim_later(uint32_t ms, uint32_t st, const char *msg, uint32_t seq, int result)
{
    wsim_due = in_now() + in_hz() * ms / 1000; wsim_next_state = st; wsim_next_msg = msg; wsim_pending_seq = seq; wsim_next_result = result;
}
static void wsim_scan_fill(volatile struct an_unit *u)
{
    volatile struct an_scan *t = (volatile struct an_scan *)((volatile uint8_t *)u - AN_OFF_UNIT(0) + AN_OFF_SCAN);
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 6; i++) t[k].bssid[i] = wsim_nets[k].bssid[i];
        unsigned i = 0;
        for (; wsim_nets[k].ssid[i] && i < 32; i++) t[k].ssid[i] = wsim_nets[k].ssid[i];
        t[k].ssid[i] = 0;
        t[k].rssi = wsim_nets[k].rssi; t[k].channel = wsim_nets[k].channel; t[k].security = wsim_nets[k].security;
    }
    __asm__ volatile("dmb ish" ::: "memory");
    u->scan_count = 4; u->scan_seq++;
}
/* the join: what the firmware would decide, decided here */
static void wsim_join(volatile struct an_unit *u, uint32_t seq)
{
    struct wsim_net *n = 0;
    for (int k = 0; k < 4; k++) if (wsim_streq(wsim_join_ssid, wsim_nets[k].ssid)) n = &wsim_nets[k];
    wsim_state(u, AN_ST_JOINING, "joining");
    u->link = 0;
    if (!n) { wsim_later(1500, AN_ST_FAILED, "join failed: no such network in range", seq, 0); return; }
    if (n->security != SEC_OPEN && !wsim_streq(wsim_join_pass, n->pass)) { wsim_later(2500, AN_ST_FAILED, "join failed: the password was not accepted", seq, 0); return; }
    unsigned i = 0;
    for (; n->ssid[i] && i < 32; i++) u->ssid[i] = n->ssid[i];
    u->ssid[i] = 0;
    for (i = 0; i < 6; i++) u->bssid[i] = n->bssid[i];
    u->rssi = n->rssi; u->channel = n->channel; u->security = n->security; u->speed_mbps = 72;
    wsim_later(2000, AN_ST_CONNECTED, "connected (simulated radio: frames through mac.wifinet)", seq, 0);
}
static const char *wsim_script_ssid(void) { return wsim_nets[0].ssid; }
static const char *wsim_script_pass(void) { return wsim_nets[0].pass; }
/* ---- testbed Bluetooth: a simulated host (mac.btsim=1) ------------------------------------------
   QEMU's raspi3b has no Bluetooth chip, so the board's Bluetooth block (alice_net.h struct an_bt)
   only ever said "off" there. For the Mac programs that drive it (PiBlue, the Bluetooth menu of
   mac/wifimenu) this answers the four commands the real host takes (bt_pi.c), with believable
   delays: one paired mouse already connected; a scan that takes two seconds and finds a keyboard,
   a mouse and a pair of headphones; pairing (the keyboard shows a passkey, 123456, for four
   seconds first, as the real host shows one to type); connect; forget. Nothing reaches the input
   ring: there is no device. */
extern volatile uint8_t *alice_net_board(void);
static int bsim_on = -1, bsim_phase;
static uint64_t bsim_due;
static uint32_t bsim_pending;                               /* a command in progress: its sequence number */
static uint32_t bsim_cmd; static uint8_t bsim_addr[6];
struct bsim_dev { const char *name; uint8_t addr[6]; uint32_t cod, flags; int rssi; };
static const struct bsim_dev bsim_near[3] = {
    { "Magic Keyboard", { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 }, 0x002540, AN_BTK_KEYBOARD, -52 },
    { "MX Anywhere 2", { 0x21, 0x32, 0x43, 0x54, 0x65, 0x76 }, 0x002580, AN_BTK_MOUSE, -61 },
    { "Studio Headphones", { 0x31, 0x42, 0x53, 0x64, 0x75, 0x86 }, 0x240404, 0, -70 },
};
static void bsim_name(volatile char *d, const char *s) { int i = 0; for (; s[i] && i < 39; i++) d[i] = s[i]; for (; i < 40; i++) d[i] = 0; }
static void bsim_msg(volatile struct an_bt *b, const char *m) { int i = 0; for (; m[i] && i < 63; i++) b->msg[i] = m[i]; b->msg[i] = 0; }
static int bsim_key(volatile struct an_bt *b, const uint8_t *a)
{
    for (unsigned k = 0; k < b->nkeys; k++) { int same = 1; for (int i = 0; i < 6; i++) if (b->keys[k].addr[i] != a[i]) same = 0; if (same) return (int)k; }
    return -1;
}
static void bsim_add(volatile struct an_bt *b, const char *name, const uint8_t *a, uint32_t flags)
{
    if (b->nkeys >= AN_BT_KEYS || bsim_key(b, a) >= 0) return;
    volatile struct an_btkey *k = &b->keys[b->nkeys];
    for (int i = 0; i < 8; i++) k->addr[i] = i < 6 ? a[i] : 0;
    for (int i = 0; i < 16; i++) k->key[i] = (uint8_t)(0x5a ^ i);
    k->type = 4; k->flags = flags; bsim_name(k->name, name);
    b->nkeys++; b->keys_gen++;
}
int psmac_bt_testbed_poll(void)
{
    if (bsim_on < 0) { const char *o = psmac_option("mac.btsim"); bsim_on = o && o[0] == '1'; }
    if (!bsim_on) return 0;
    volatile uint8_t *nb = alice_net_board();
    if (!nb) return 1;
    volatile struct an_bt *b = (volatile struct an_bt *)(nb + AN_OFF_BT);
    uint64_t now = in_now();
    if (bsim_phase == 0) {
        for (unsigned i = 0; i < sizeof *b; i++) ((volatile uint8_t *)b)[i] = 0;
        static const uint8_t own[6] = { 0x02, 0x50, 0x53, 0x42, 0x54, 0x01 }, mouse[6] = { 0x41, 0x52, 0x63, 0x74, 0x85, 0x96 };
        for (int i = 0; i < 6; i++) b->bdaddr[i] = own[i];
        bsim_add(b, "Logitech M337", mouse, AN_BTK_MOUSE | AN_BTK_CONNECTED);
        b->state = AN_BT_READY; bsim_msg(b, "ready (simulated)");
        bsim_phase = 1;
        kprintf("[BT-SIM] testbed Bluetooth: a simulated host, one paired mouse\n");
    }
    /* a new command */
    if (!bsim_pending && b->cmd_seq != b->cmd_done) {
        bsim_pending = b->cmd_seq; bsim_cmd = b->cmd;
        for (int i = 0; i < 6; i++) bsim_addr[i] = b->cmd_addr[i];
        switch (bsim_cmd) {
        case AN_BT_CMD_SCAN: b->scanning = 1; b->nfound = 0; bsim_msg(b, "scanning"); bsim_due = now + in_hz() * 2; break;
        case AN_BT_CMD_PAIR: b->pairing = 1; bsim_msg(b, "pairing"); bsim_due = now + in_hz() * 3 / 2; break;
        case AN_BT_CMD_CONNECT: bsim_msg(b, "connecting"); bsim_due = now + in_hz(); break;
        default: bsim_due = now; break;
        }
        kprintf("[BT-SIM] command %u\n", bsim_cmd);
    }
    if (bsim_pending && now >= bsim_due) {
        int r = 0;
        switch (bsim_cmd) {
        case AN_BT_CMD_SCAN:
            for (int k = 0; k < 3; k++) {
                if (bsim_key(b, bsim_near[k].addr) >= 0) continue;
                volatile struct an_btfound *f = &b->found[b->nfound++];
                for (int i = 0; i < 8; i++) f->addr[i] = i < 6 ? bsim_near[k].addr[i] : 0;
                f->cod = bsim_near[k].cod; f->rssi = bsim_near[k].rssi; bsim_name(f->name, bsim_near[k].name);
            }
            b->scanning = 0; bsim_msg(b, "scan done");
            break;
        case AN_BT_CMD_PAIR: {
            int k = -1;
            for (int j = 0; j < 3; j++) { int same = 1; for (int i = 0; i < 6; i++) if (bsim_near[j].addr[i] != bsim_addr[i]) same = 0; if (same) k = j; }
            if (k < 0 || !bsim_near[k].flags) { b->pairing = 0; bsim_msg(b, "pairing failed: not a keyboard or mouse"); r = -1; break; }
            if ((bsim_near[k].flags & AN_BTK_KEYBOARD) && !b->passkey_shown) {      /* first the passkey, for four seconds */
                b->passkey = 123456; b->passkey_shown = 1; bsim_msg(b, "type the passkey on the keyboard");
                bsim_due = now + in_hz() * 4; return 1;
            }
            b->passkey_shown = 0; b->pairing = 0;
            bsim_add(b, bsim_near[k].name, bsim_near[k].addr, bsim_near[k].flags | AN_BTK_CONNECTED);
            unsigned n = 0;                                                   /* no longer "found": paired */
            for (unsigned j = 0; j < b->nfound; j++) { int same = 1; for (int i = 0; i < 6; i++) if (b->found[j].addr[i] != bsim_addr[i]) same = 0; if (!same) { if (n != j) for (unsigned i = 0; i < sizeof b->found[0]; i++) ((volatile uint8_t *)&b->found[n])[i] = ((volatile uint8_t *)&b->found[j])[i]; n++; } }
            b->nfound = n;
            bsim_msg(b, "paired and connected");
            break; }
        case AN_BT_CMD_CONNECT: {
            int k = bsim_key(b, bsim_addr);
            if (k < 0) r = -1; else b->keys[k].flags |= AN_BTK_CONNECTED;
            bsim_msg(b, k < 0 ? "no such device" : "connected");
            break; }
        case AN_BT_CMD_FORGET: {
            int k = bsim_key(b, bsim_addr);
            if (k < 0) { r = -1; break; }
            for (unsigned j = (unsigned)k; j + 1 < b->nkeys; j++) for (unsigned i = 0; i < sizeof b->keys[0]; i++) ((volatile uint8_t *)&b->keys[j])[i] = ((volatile uint8_t *)&b->keys[j + 1])[i];
            b->nkeys--; b->keys_gen++; bsim_msg(b, "forgotten");
            break; }
        default: r = -1; break;
        }
        b->cmd_result = (uint32_t)r;
        __asm__ volatile("dmb ish" ::: "memory");
        b->cmd_done = bsim_pending; bsim_pending = 0;
        kprintf("[BT-SIM] command %u done: %d\n", bsim_cmd, r);
    }
    return 1;
}

int psmac_wifi_testbed_poll(volatile struct an_unit *u)
{
    if (wsim_active < 0) {
        const char *cfg = psmac_option("mac.wifisim");
        wsim_active = cfg != 0;
        if (!wsim_active) return 0;
        /* SSID:PASSWORD, or just "1" for the defaults above */
        if (cfg[0] && !(cfg[0] == '1' && (cfg[1] == 0 || cfg[1] == ' '))) {
            unsigned i = 0, j = 0;
            for (; cfg[i] && cfg[i] != ':' && cfg[i] != ' ' && i < 32; i++) wsim_ssid[i] = cfg[i];
            wsim_ssid[i] = 0;
            if (cfg[i] == ':') for (i++; cfg[i] && cfg[i] != ' ' && j < 63; i++, j++) wsim_pass[j] = cfg[i];
            wsim_pass[j] = 0;
            wsim_nets[0].ssid = wsim_ssid; wsim_nets[0].pass = wsim_pass; wsim_nets[0].security = wsim_pass[0] ? SEC_WPA2 : SEC_OPEN;
        }
        u->kind = AN_KIND_WIFI;
        for (int i = 0; i < 6; i++) { u->mac[i] = wsim_mac[i]; u->factory_mac[i] = wsim_mac[i]; }
        u->chip = 0x4345; u->chiprev = 6; u->sdmhz = 25;                /* a BCM43455, as on a Pi 3B+/4 */
        { const char *f = "simulated radio (testbed)"; unsigned i = 0;
          for (; f[i]; i++) u->fwver[i] = f[i];
          u->fwver[i] = 0; }
        wsim_state(u, AN_ST_STARTING, "testbed WiFi: simulated radio starting");
        wsim_later(1200, AN_ST_IDLE, "idle: scan or join (S / J in PiWiFi)", 0, 0);
        kprintf("[WIFI-SIM] unit 0 is a simulated radio: network \"%s\" lets the Mac in; frames %s\n",
                wsim_nets[0].ssid, psmac_netfile_up(1) ? "through mac.wifinet" : "go nowhere (no mac.wifinet=)");
        /* wifi.ssid=/wifi.psk= on the command line join at boot, as the real driver does */
        { extern int psmac_bootarg_val(const char *key, char *out, unsigned max);
          if (psmac_bootarg_val("wifi.ssid", wsim_join_ssid, sizeof wsim_join_ssid)) { psmac_bootarg_val("wifi.psk", wsim_join_pass, sizeof wsim_join_pass); wsim_want = 1; } }
    }
    if (!wsim_active) return 0;
    /* a state change falls due */
    if (wsim_due && (int64_t)(in_now() - wsim_due) >= 0) {
        wsim_due = 0;
        uint32_t st = wsim_next_state;
        if (st == AN_ST_SCANNING) { wsim_scan_fill(u); st = u->link ? AN_ST_CONNECTED : AN_ST_IDLE; wsim_next_msg = u->link ? "connected" : "idle: 4 networks found"; }
        if (st == AN_ST_CONNECTED) u->link = 1;
        wsim_state(u, st, wsim_next_msg);
        if (wsim_pending_seq) wsim_finish(u, wsim_pending_seq, wsim_next_result);
        if (st == AN_ST_IDLE && wsim_want && !u->admin_off) { wsim_want = 0; wsim_join(u, 0); }
    }
    /* commands, one at a time; the slow ones answer when their state change lands */
    if (u->cmd_seq != u->cmd_done && !wsim_due) {
        uint32_t seq = u->cmd_seq;
        switch (u->cmd) {
        case AN_CMD_START: case AN_CMD_ENABLE:
            if (u->cmd == AN_CMD_ENABLE && arg32(u, 0) == 0) { u->admin_off = 1; u->link = 0; wsim_state(u, AN_ST_OFF, "off: switched off from the Mac"); wsim_finish(u, seq, 0); break; }
            u->admin_off = 0;
            if (u->state == AN_ST_OFF) { wsim_state(u, AN_ST_STARTING, "starting"); wsim_later(1200, AN_ST_IDLE, "idle", seq, 0); }
            else wsim_finish(u, seq, 0);
            break;
        case AN_CMD_OFF: u->admin_off = 1; u->link = 0; wsim_state(u, AN_ST_OFF, "off"); wsim_finish(u, seq, 0); break;
        case AN_CMD_SCAN:
            if (u->state == AN_ST_OFF) { wsim_finish(u, seq, -1); break; }
            wsim_state(u, AN_ST_SCANNING, "scanning 2.4 GHz channels 1-13");
            wsim_later(1800, AN_ST_SCANNING, "", seq, 0);
            break;
        case AN_CMD_JOIN: {
            volatile struct an_join *j = (volatile struct an_join *)u->arg;
            if (j->ssid[0]) {
                unsigned i = 0;
                for (; i < 32 && j->ssid[i]; i++) wsim_join_ssid[i] = j->ssid[i];
                wsim_join_ssid[i] = 0;
                for (i = 0; i < 64 && j->pass[i]; i++) wsim_join_pass[i] = j->pass[i];
                wsim_join_pass[i] = 0;
            }
            if (!wsim_join_ssid[0]) { wsim_finish(u, seq, -2); break; }
            u->admin_off = 0;
            if (u->state == AN_ST_OFF) { wsim_want = 1; wsim_state(u, AN_ST_STARTING, "starting"); wsim_later(1200, AN_ST_IDLE, "idle", seq, 0); break; }
            wsim_join(u, seq);
            break; }
        case AN_CMD_DISCONNECT: u->link = 0; wsim_want = 0; wsim_state(u, u->state == AN_ST_OFF ? AN_ST_OFF : AN_ST_IDLE, "disconnected"); wsim_finish(u, seq, 0); break;
        case AN_CMD_RXON: wsim_rx_on = 1; u->rx_on = 1; wsim_finish(u, seq, 0); break;
        case AN_CMD_RXOFF: wsim_rx_on = 0; u->rx_on = 0; wsim_finish(u, seq, 0); break;
        case AN_CMD_FILTER: { uint32_t f = arg32(u, 0); wsim_promisc = !!(f & AN_FILT_PROMISC); wsim_allmulti = !!(f & AN_FILT_ALLMULTI); wsim_finish(u, seq, 0); break; }
        case AN_CMD_SETMAC: {
            int zero = 1; for (int i = 0; i < 6; i++) if (u->arg[i]) zero = 0;
            for (int i = 0; i < 6; i++) wsim_mac[i] = zero ? u->factory_mac[i] : u->arg[i];
            for (int i = 0; i < 6; i++) u->mac[i] = wsim_mac[i];
            wsim_finish(u, seq, 0); break; }
        case AN_CMD_WIFICFG: case AN_CMD_ETHCFG: wsim_finish(u, seq, 0); break;
        default: wsim_finish(u, seq, -3);
        }
    }
    volatile uint8_t *board = (volatile uint8_t *)u - AN_OFF_UNIT(0);
    /* transmit ring: Mac -> "air" (the host file); without a connection the frames are lost, as in the air */
    while (u->tx_tail != u->tx_head) {
        volatile struct an_slot *s = (volatile struct an_slot *)(board + AN_OFF_TX(0) + (u->tx_tail % AN_TX_SLOTS) * AN_SLOT_BYTES);
        uint32_t len = s->len;
        if (len >= 14 && len <= AN_SLOT_BYTES - 8 && u->link) {
            for (uint32_t i = 0; i < len; i++) wsim_buf[i] = s->data[i];
            psmac_ip_mac_tx(0, wsim_buf, len);
            psmac_netprobe_tx(0, wsim_buf, len, 0);
            if (psmac_netfile_send(1, wsim_buf, len)) { u->tx_pkts++; u->tx_bytes += len; psmac_netprobe_tx(0, wsim_buf, len, 1); } else u->tx_err++;
        } else u->tx_err++;
        __asm__ volatile("dmb ish" ::: "memory");
        u->tx_tail++;
    }
    /* the kernel's own frames (psmac_ip.c): on the WiFi it shares the Mac's MAC address */
    if (psmac_ip_active && u->link) {
        uint32_t len;
        while ((len = psmac_ip_tx_take(0, wsim_buf, sizeof wsim_buf)))
            psmac_netfile_send(1, wsim_buf, len);
    }
    /* receive ring: "air" -> Mac, only while connected and the stack takes frames */
    for (int budget = 0; budget < 8; budget++) {
        int len = psmac_netfile_recv(1, wsim_buf, AN_SLOT_BYTES - 8);
        if (len <= 0) break;
        if (psmac_ip_active && u->link && psmac_ip_rx(0, wsim_buf, (uint32_t)len, wsim_mac, wsim_promisc) != IPK_TO_MAC) continue;
        if (!wsim_rx_on || !u->link) { u->rx_drop++; continue; }
        int bcast = wsim_buf[0] == 0xff && wsim_buf[1] == 0xff && wsim_buf[2] == 0xff && wsim_buf[3] == 0xff && wsim_buf[4] == 0xff && wsim_buf[5] == 0xff;
        int mcast = (wsim_buf[0] & 1) && !bcast;
        int mine = wsim_promisc || bcast || (mcast && wsim_allmulti) ||
                   (wsim_buf[0] == wsim_mac[0] && wsim_buf[1] == wsim_mac[1] && wsim_buf[2] == wsim_mac[2] && wsim_buf[3] == wsim_mac[3] && wsim_buf[4] == wsim_mac[4] && wsim_buf[5] == wsim_mac[5]);
        if (!mine) continue;
        if (u->rx_head - u->rx_tail >= AN_RX_SLOTS) { u->rx_drop++; continue; }
        volatile struct an_slot *s = (volatile struct an_slot *)(board + AN_OFF_RX(0) + (u->rx_head % AN_RX_SLOTS) * AN_SLOT_BYTES);
        for (int i = 0; i < len; i++) s->data[i] = wsim_buf[i];
        s->len = (uint32_t)len; s->flags = (bcast ? AN_SF_BCAST : 0) | (mcast ? AN_SF_MCAST : 0);
        psmac_netprobe_rx(0, wsim_buf, (uint32_t)len, u->rx_head, &u->rx_tail);
        __asm__ volatile("dmb ish" ::: "memory");
        u->rx_head++; u->rx_pkts++; u->rx_bytes += (uint32_t)len;
        ((volatile struct an_hdr *)board)->irq = 1;
    }
    return 1;
}

/* ---- bring-up and the core-3 loop ---------------------------------------------------------- */
static volatile int alice_ready;
static uint8_t *alice_win;
void psmac_alice_init(uint8_t *win)
{
    alice_win = win;
    alice_net_attach(win + ALICE_NET_WINDOW_OFF);
    alice_status_attach(win + ALICE_STATUS_WINDOW_OFF);
    inr = (volatile struct alice_input_ring *)(win + ALICE_INPUT_WINDOW_OFF);
    for (uint32_t i = 0; i < sizeof(struct alice_input_ring) / 4; i++) ((volatile uint32_t *)inr)[i] = 0;
    inr->magic = ALICE_IN_MAGIC; inr->version = 1; inr->slots = ALICE_IN_SLOTS;
    __asm__ volatile("dmb ish" ::: "memory");
    alice_ready = 1;
}
uint32_t psmac_alice_irqs(void) { return irq_raised; }

/* The slot-9 line this instant, for the bus layer's register reads (psmac_bus.c): the same
   expression the loop below samples, minus its storm guard. -1 before the services are up. */
/* The 68k's RESET (psmac_bus.c ps_send_reset): what a NuBus card's /RESET does to its interrupt.
   Restart from the Finder runs the ROM's boot again with the Pi's state as the last session left
   it: .PiInput's ring still "attached", the Ethernet driver's version in the board header, the
   video driver's VBL on. The first mouse movement during the restart, a frame, or the next VBL
   then raised the slot-9 line before any driver of the new session had installed a handler, and
   the ROM stopped with a Sad Mac, 0000000F 00000033 (system error 51, an unserviceable slot
   interrupt), photographed on the user's Pi 3, 18 November. Every source now drops with the reset
   and comes back when its driver opens again. */
void psmac_card_reset(void)
{
    extern volatile uint8_t *alice_net_board(void); extern void psmac_display_bus_reset(void);
    if (!alice_ready) return;
    volatile struct an_hdr *nh = (volatile struct an_hdr *)alice_net_board();
    if (nh) { nh->amiga_ver = 0; nh->irq = 0; }
    alice_net_reset();                          /* the rest of the session (queues, buffers) on core 3's next turn */
    if (inr) { inr->attached = 0; inr->tail = inr->head; }
    psmac_display_bus_reset();
    { extern void psmac_remote_mac_reset(void); psmac_remote_mac_reset(); }   /* the remote's input queue and records (psmac_remote.c) */
    __asm__ volatile("dmb ish" ::: "memory");
    kprintf("[PSMAC] 68k RESET: the card's interrupt sources are off until their drivers open\n");
}

int psmac_slot_line_now(void)
{
    extern volatile uint8_t *alice_net_board(void); extern int psmac_display_irq(void);
    if (!alice_ready) return -1;
    volatile struct an_hdr *nh = (volatile struct an_hdr *)alice_net_board();
    int net_line = nh && nh->amiga_ver && nh->irq;
    return (net_line && nh->irq) || (inr && inr->attached && inr->head != inr->tail) || psmac_display_irq();
}

/* core 1: the USB controller's start, which takes about a second (power, reset, the first
   enumeration behind the hub), away from core 3's loop, whose mailbox, card and display services
   the Mac needs from its first instruction; core 3 polls the devices once it is done. Then every
   later enumeration (a plug, an unplug, a retry), for the same reason (psmac_usb.c). Since 21
   November 2026 core 1 is also the sound output's (psmac_audio.c): started first, so that the
   startup chime, which comes during the USB start's second, reaches HDMI; fed from the USB code's
   waits and from the loop between enumerations. */
void psmac_alice_core1(void)
{
    while (!__atomic_load_n(&alice_ready, __ATOMIC_ACQUIRE)) __asm__ volatile("yield");
    {   /* mac.mboxstress=S (testbed): for S seconds core 1 asks the firmware's mailbox through alice_net.c's
           call (the board revision) every millisecond, as the USB start's power request does once, while
           core 3 starts the WiFi: the race of docs/08 item 79, which QEMU's timing seldom gives by itself */
        extern const char *psmac_option(const char *);
        const char *o = psmac_option("mac.mboxstress");
        uint32_t secs = 0;
        if (o)
            for (; *o >= '0' && *o <= '9'; o++)
                secs = secs * 10 + (uint32_t)(*o - '0');
        if (secs) {
            extern int alice_mbox(uint32_t tag, uint32_t a, uint32_t b, uint32_t *out);
            uint64_t end = in_now() + in_hz() * secs;
            uint32_t calls = 0, busy = 0, bad = 0, v;
            kprintf("[PSMAC] mac.mboxstress=%u: core 1 asks the mailbox every millisecond for %u s\n", secs, secs);
            while (in_now() < end) {
                int e = alice_mbox(0x00010002u, 0, 0, &v);       /* GET_BOARD_REVISION */
                calls++;
                if (e == -9) busy++;
                else if (e) bad++;
                uint64_t t = in_now() + in_hz() / 1000;
                while (in_now() < t)
                    __asm__ volatile("yield");
            }
            kprintf("[PSMAC] mac.mboxstress: %u calls from core 1, %u found the mailbox busy, %u failed\n", calls, busy, bad);
        }
    }
    { extern void psmac_audio_start(uint8_t *win); psmac_audio_start(alice_win); }   /* the sound output first: the chime comes within the USB start's second (psmac_audio.c) */
    { extern void psmac_usb_start(void); psmac_usb_start(); }
    psmac_boot_mark("core 1: USB started");
    { extern void psmac_usb_core1(void); psmac_usb_core1(); }
}

/* the service loop's longest part-turns since the last heartbeat (ticks), and its turns */
static uint64_t loop_max[6], loop_remote_sum;     /* the remote's share is also summed: its cost when nobody is connected */
static uint32_t loop_turns;
static volatile uint32_t loop_turns_all;          /* never reset: the watch below compares it */
static const char *volatile loop_step;            /* the part of the turn core 3 is in, for the watch */
#define LOOP_T(i, d) do { if ((d) > loop_max[i]) loop_max[i] = (d); } while (0)
#define LOOP_AT(name) (loop_step = (name))
void psmac_loop_at(const char *name) { loop_step = name; }   /* alice_net.c: the network part's own parts */
void psmac_loop_report(void)
{
    uint64_t us = in_hz() / 1000000u;
    kprintf("[PSMAC]   service loop: %u turns/5 s, longest: mailbox %u us, card %u us, network %u us, USB %u us, display %u us%s",
            loop_turns, (uint32_t)(loop_max[0] / us), (uint32_t)(loop_max[1] / us), (uint32_t)(loop_max[2] / us),
            (uint32_t)(loop_max[3] / us), (uint32_t)(loop_max[4] / us), psmac_ip_active ? ", remote " : "\n");
    if (psmac_ip_active) kprintf("%u us (%u us in all)\n", (uint32_t)(loop_max[5] / us), (uint32_t)(loop_remote_sum / us));
    {   /* the WiFi's share of "network": its longest turn between two yields, and where it was then */
        extern void alice_wifi_turn_report(uint32_t *us, const char **where);
        uint32_t wus; const char *wwhere;
        alice_wifi_turn_report(&wus, &wwhere);
        if (wus >= 20000) kprintf("[PSMAC]   the WiFi's longest turn %u us, at \"%s\"\n", wus, wwhere);
    }
    for (int i = 0; i < 6; i++) loop_max[i] = 0;
    loop_remote_sum = 0;
    loop_turns = 0;
}
/* The service loop's watch, from the bus core (psmac_bus.c), which keeps going when core 3 stops:
   when the loop has made no turn for 2 s, one line says which part of the turn core 3 is in (and,
   in the network part, where the WiFi is), then one every 10 s while it lasts, and one when it turns
   again. Before 21 November 2026 a stopped core 3 showed only as "0 turns/5 s" (docs/08 item 79). */
void psmac_loop_watch(void)
{
    static uint64_t next, last_change, said_at; static uint32_t last_turns; static int stalled;
    uint64_t now = in_now(), hz = in_hz();
    if (now < next) return;
    next = now + hz / 4;
    const char *step = loop_step;
    if (!step) return;                              /* core 3's loop has not begun */
    uint32_t t = loop_turns_all;
    if (t != last_turns || !last_change) {
        if (stalled) kprintf("[PSMAC] core 3's service loop turns again, after %u ms\n", (uint32_t)((now - last_change) * 1000u / hz));
        last_turns = t; last_change = now; stalled = 0;
        return;
    }
    if (now - last_change < 2 * hz || (stalled && now - said_at < 10 * hz)) return;
    extern const char *alice_wifi_where(void);
    const char *ww = alice_wifi_where();
    kprintf("[PSMAC] WATCH: core 3's service loop has made no turn for %u s: it is in \"%s\"%s%s%s\n", (uint32_t)((now - last_change) / hz), step,
            ww && ww[0] ? " (the WiFi: \"" : "", ww && ww[0] ? ww : "", ww && ww[0] ? "\")" : "");
    stalled = 1; said_at = now;
}

/* core 3: the Alice68 services plus the mailbox services, for ever */
void __attribute__((noreturn)) psmac_alice_core(void)
{
    while (!__atomic_load_n(&alice_ready, __ATOMIC_ACQUIRE)) __asm__ volatile("yield");
    kprintf("[PSMAC] core 3: Alice68 services (network board, Bluetooth, USB input, status) and the mailbox\n");
    psmac_boot_mark("core 3: the service loop begins");
    { extern void psmac_remote_init(uint8_t *win); psmac_remote_init(alice_win); }   /* remote=on: the remote control (docs/13) */
    for (;;) {
        /* every part of the turn timed: the longest of each over the heartbeat's five seconds goes
           into its line (psmac_loop_report), so a photograph of the log says what holds the loop */
        uint64_t t0 = in_now(), t1;
        LOOP_AT("mailbox");
        psmac_mbox_poll();
        t1 = in_now(); LOOP_T(0, t1 - t0); t0 = t1;
        LOOP_AT("card");
        { extern void psmac_card_poll(void); psmac_card_poll(); }        /* the Pi's own SD card: pairings, the disk image's changes (psmac_card.c) */
        t1 = in_now(); LOOP_T(1, t1 - t0); t0 = t1;
        LOOP_AT("network");
        alice_net_poll();
        t1 = in_now(); LOOP_T(2, t1 - t0); t0 = t1;
        LOOP_AT("USB");
        { extern void psmac_usb_poll(void); psmac_usb_poll(); }          /* USB keyboard and mouse (psmac_usb.c) */
        t1 = in_now(); LOOP_T(3, t1 - t0); t0 = t1;
        LOOP_AT("input");
        in_repeat_poll();
        in_testbed_poll();
        LOOP_AT("echo timings");
        { extern void psmac_netprobe_poll(void); psmac_netprobe_poll(); }  /* echo timings (psmac_netprobe.c) */
        LOOP_AT("power state");
        { extern void psmac_power_poll(void); psmac_power_poll(); }        /* the Pi's power state, every 2 s (psmac_power.c) */
        LOOP_AT("display");
        { extern void psmac_display_poll(void); psmac_display_poll(); }   /* the second display's tick and picture */
        t1 = in_now(); LOOP_T(4, t1 - t0); t0 = t1; loop_turns++; loop_turns_all++;
        LOOP_AT("remote control");
        { extern void psmac_remote_poll(void); psmac_remote_poll(); }     /* the remote control: its IP stack, HTTP and input queue (docs/13) */
        t1 = in_now(); LOOP_T(5, t1 - t0); loop_remote_sum += t1 - t0;
        LOOP_AT("the slot interrupt line");
        /* The card's interrupt line: up while a board has something the Mac has not taken (the
           network board's irq flag, which the Mac's Ethernet driver clears; input events not yet
           read), but only from boards whose Mac driver has attached (the Ethernet driver writes
           its version into the board header, the input driver sets the ring's attached word).
           The first version raised the line unconditionally: the ROM enables every slot's
           interrupt early in the boot, no driver was open to clear anything, and the level-2
           storm kept the Mac in its boot chime for good. */
        { extern volatile uint8_t *alice_net_board(void); extern void psmac_slot_irq(int);
          static uint64_t up_since; static uint32_t storms;
          volatile struct an_hdr *nh = (volatile struct an_hdr *)alice_net_board();
          extern int psmac_display_irq(void);
          int net_line = nh && nh->amiga_ver && nh->irq;
          int line = net_line || (inr && inr->attached && inr->head != inr->tail) || psmac_display_irq();
          /* Safety net: a level-sensitive line that nobody services wedges the 68k in its
             level-2 handler for good (seen once, when a driver announced itself before its
             handler was in). A real card cannot help that; this one can: if the network
             board's flag stays up for half a second after the driver attached, it is cleared
             here and the kernel log says so. The driver sees the frame on its next poll. */
          if (!line) up_since = 0;
          else if (!up_since) up_since = in_now();
          else if (net_line && in_now() - up_since > in_hz() / 2) {
              nh->irq = 0; up_since = 0;
              if (storms++ < 3) kprintf("[PSMAC] slot-9 interrupt not serviced in 500 ms: network board flag cleared\n");
          }
          psmac_slot_irq(line && !(net_line && !nh->irq)); }
        __asm__ volatile("yield");
    }
}

/* the testbed Ethernet owns the host frame files once it has come up */
int psmac_alice_owns_net(void) { return tb_active > 0; }
