/* SPDX-License-Identifier: GPL-2.0-only
 *
 * alice_psmac.c — the Macintosh (Aurora68) platform layer under the Alice68 modules.
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
#include <stdarg.h>
#include "support.h"
#include "psmac.h"
#include "psmb.h"
#include "alice_net.h"
#include "alice_psmac.h"
#include "alice_inmerge.h"
#include "../psmac/psmac_ip.h"
#include "wifi/fmac.h"
#include "wifi/fmac_ap.h"

/* ---- time ---------------------------------------------------------------------------------- */
static inline uint64_t ticks(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t tick_hz(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }

/* ---- what the Alice68 modules call --------------------------------------------------------- */
static uint32_t irq_raised;
/* Alice68 raises the Amiga's PORTS interrupt here. The board's irq flag is already set by the
   caller; the Mac drivers poll it. Counted so the heartbeat can show activity. */
void aga_virtual_cia_irq(uint16_t bit, int active) { (void)bit; (void)active; irq_raised++; }

/* the boot splash's status strip: on the Mac build it is the console log */
void frf_post_wifi(const char *text, int col)
{
    if (!text || !*text) return;
    kprintf("[WIFI] status: %s (%d)\n", text, col);
    { extern void psmac_splash_wifi(const char *, int); psmac_splash_wifi(text, col); }   /* the HDMI splash's line (psmac_display.c) */
}
void frf_post_eth(const char *text, int col)  { if (text && *text) kprintf("[ETH] status: %s (%d)\n", text, col); }
void frf_post_bt(const char *text, int col)   { if (text && *text) kprintf("[BT] status: %s (%d)\n", text, col); }
void frf_post_usb_devices(unsigned kb, unsigned ms, unsigned gp, unsigned msc) { kprintf("[USB] devices: %u keyboards %u mice %u pads %u storage\n", kb, ms, gp, msc); }
void frf_boot_stage(unsigned stage) { (void)stage; }
void aga_reboot(int tryboot) { (void)tryboot; kprintf("[PSMAC] reboot requested by a service module: ignored on the Macintosh build\n"); }

/* ---- input ring: keyboard and mouse reports for the Mac side ------------------------------- */
static volatile struct alice_input_ring *inr;
static volatile struct alice_input_fb *infb;     /* .PiInput's feedback, right after the ring (alice_psmac.h) */
_Static_assert(sizeof(struct alice_input_ring) == ALICE_IN_FB_OFF, "the feedback block follows the ring (mac/piinput.s IN_FB)");
_Static_assert(ALICE_IN_FB_OFF + sizeof(struct alice_input_fb) <= 4096, "the input page is 4 KB");
/* mac.altcmd=1: a PC keyboard's Alt is Command and its Windows key Option, by position (the key
   beside the space bar is Command on a Mac keyboard). Without it Apple's own mapping holds:
   Windows is Command, Alt is Option (mac/piinput.s hid_modifiers). Applied to the HID modifier
   byte of every key event, USB and Bluetooth alike. */
static int in_altcmd = -1;

/* Sources (24 November 2026, docs/08 item 139: the owner's SE, "it's like I have some key held down"). The ring carries
   one keyboard state and one set of mouse buttons, and until this day every keyboard and mouse wrote them directly: a
   USB mouse's report with its button up let go of a Bluetooth mouse's button held at the same moment, and a device that
   went away with a button down left nothing to let go of it. Now each source has its own (kb_src, btn_src), the ring
   gets their OR, and psmac_input_gone lets go of one source's alone. Bluetooth says which link a report comes from
   (bt_pi.c), USB is the default, the remote control marks its own. */
static int in_src;                                  /* ALICE_SRC_*: whose report this is */
static uint8_t in_src_addr[ALICE_NSRC][2];          /* a Bluetooth source's address, its last two bytes, for the log */
static uint8_t kb_src[ALICE_NSRC][8];               /* each source's last keyboard report */
static uint8_t btn_src[ALICE_NSRC];                 /* each source's mouse buttons */
void psmac_input_from(int src, const uint8_t *addr)
{
    if (src < 0 || src >= ALICE_NSRC) src = ALICE_SRC_USB;
    in_src = src;
    if (addr) { in_src_addr[src][0] = addr[1]; in_src_addr[src][1] = addr[0]; }   /* least significant byte first, as HCI */
}
static uint32_t btn_all(void) { return inm_buttons(btn_src, ALICE_NSRC); }
static const char *src_name(int s, char *t)          /* "USB", "BT1 44:55", "remote" */
{
    static const char hx[] = "0123456789ABCDEF";
    if (s == ALICE_SRC_USB) return "USB";
    if (s == ALICE_SRC_TEST) return "test";
    if (s == ALICE_SRC_REMOTE) return "remote";
    t[0] = 'B'; t[1] = 'T'; t[2] = (char)('0' + s - ALICE_SRC_BT0); t[3] = ' ';
    t[4] = hx[in_src_addr[s][0] >> 4]; t[5] = hx[in_src_addr[s][0] & 15]; t[6] = ':'; t[7] = hx[in_src_addr[s][1] >> 4]; t[8] = hx[in_src_addr[s][1] & 15]; t[9] = 0;
    return t;
}

/* mac.inputtrace=N: the first N events of the ring in the log, each with its source, what it carried and what .PiInput
   made of it (the feedback block: what it posted, MBState, the modifiers, KeyMap's modifier keys after it). One line per
   event, written once .PiInput has taken it (or after two seconds without). */
struct in_tr { uint32_t seq; uint8_t used, src, sa0, sa1; uint32_t type; int32_t a, b, c, raw; uint64_t t; };
static struct in_tr in_tr[ALICE_IN_SLOTS];
static int in_tr_left = -1;                          /* -1: the option not read yet */
static uint32_t in_tr_n;
static int in_trace_on(void)
{
    if (in_tr_left < 0) {
        extern const char *psmac_option(const char *); const char *o = psmac_option("mac.inputtrace");
        int v = 0; for (; o && *o >= '0' && *o <= '9'; o++) v = v * 10 + (*o - '0');
        in_tr_left = v > 100000 ? 100000 : v;
        in_tr_n = (uint32_t)in_tr_left;
        if (in_tr_left) kprintf("[INPUT] mac.inputtrace: the next %d input events in the log\n", in_tr_left);
    }
    return in_tr_n != 0;
}
/* one event into the ring as it is; raw: the source's own buttons, for the trace */
static void ring_put(uint32_t type, int32_t a, int32_t b, int32_t c, int32_t raw)
{
    if (!inr) return;
    uint32_t head = inr->head, tail = inr->tail;
    if (head - tail >= ALICE_IN_SLOTS) { inr->dropped++; return; }
    volatile struct alice_input_event *e = &inr->ev[head % ALICE_IN_SLOTS];
    e->type = type; e->a = a; e->b = b; e->c = c;
    if (in_trace_on() && in_tr_left > 0) {
        struct in_tr *r = &in_tr[head % ALICE_IN_SLOTS];
        in_tr_left--;
        r->seq = head; r->used = 1; r->src = (uint8_t)in_src; r->sa0 = in_src_addr[in_src][0]; r->sa1 = in_src_addr[in_src][1];
        r->type = type; r->a = a; r->b = b; r->c = c; r->raw = raw;
        __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(r->t));
    }
    __asm__ volatile("dmb ish" ::: "memory");
    inr->head = head + 1;
}
/* the remote control's events (psmac_remote.c) come here directly, since its Command is Command whatever keyboard the
   bench has; its absolute pointer's buttons are a source of their own like any mouse's */
void psmac_input_put(uint32_t type, int32_t a, int32_t b, int32_t c)
{
    int was = in_src;
    int32_t raw = c;
    in_src = ALICE_SRC_REMOTE;
    if (type == ALICE_IN_MOUSEABS && c != -1) { btn_src[ALICE_SRC_REMOTE] = (uint8_t)(c & 7); c = (int32_t)btn_all(); }
    ring_put(type, a, b, c, raw);
    in_src = was;
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
    ring_put(type, a, b, c, c);
}
static inline uint64_t in_now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t in_hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
/* a bounded sprintf for the trace's lines (the kernel's own printf, into a buffer that cannot overflow) */
struct in_fmtb { char *p; unsigned n, cap; };
static void in_fmt_putc(void *d, char c) { struct in_fmtb *b = d; if (b->n + 1 < b->cap) { b->p[b->n++] = c; b->p[b->n] = 0; } }
static unsigned in_fmt(char *t, unsigned cap, const char *fmt, ...)
{
    struct in_fmtb b = { t, 0, cap };
    if (cap) t[0] = 0;
    va_list v; va_start(v, fmt); vkprintf_pc(in_fmt_putc, &b, fmt, v); va_end(v);
    return b.n;
}

/* the modifier keys as words: from event-record bits (Inside Macintosh I), and from KeyMap's word at +6 (Command is
   code 0x37: byte 6 bit 7; Shift, Caps Lock, Option, Control 0x38-0x3B: byte 7 bits 0-3) */
static void mods_words(uint32_t m, char *t)
{
    unsigned k = 0;
    static const struct { uint32_t bit; const char *w; } n[] = { { 0x100, "Command" }, { 0x200, "Shift" }, { 0x400, "CapsLock" }, { 0x800, "Option" }, { 0x1000, "Control" } };
    for (unsigned i = 0; i < 5; i++) if (m & n[i].bit) { if (k) t[k++] = '+'; for (const char *w = n[i].w; *w; w++) t[k++] = *w; }
    if (!k) { t[k++] = 'n'; t[k++] = 'o'; t[k++] = 'n'; t[k++] = 'e'; }
    t[k] = 0;
}
static uint32_t km_mods(uint32_t w)                 /* KeyMap+6 word -> event-record bits */
{
    return (w & 0x8000u ? 0x100u : 0) | (w & 1u ? 0x200u : 0) | (w & 2u ? 0x400u : 0) | (w & 4u ? 0x800u : 0) | (w & 8u ? 0x1000u : 0);
}
static void in_trace_poll(void)
{
    if (!inr || !in_tr_n) return;
    uint32_t tail = inr->tail;
    uint64_t now = in_now(), late = in_hz() * 2;
    int fb = infb && infb->magic == ALICE_IN_FB_MAGIC;
    for (unsigned i = 0; i < ALICE_IN_SLOTS; i++) {
        struct in_tr *r = &in_tr[i];
        if (!r->used) continue;
        int taken = (int32_t)(tail - r->seq) > 0;
        if (!taken && now - r->t < late) continue;
        r->used = 0;
        char sn[12], what[64], res[160], m1[48], m2[48], m3[48];
        const char *src;
        { uint8_t s0 = in_src_addr[r->src][0], s1 = in_src_addr[r->src][1]; in_src_addr[r->src][0] = r->sa0; in_src_addr[r->src][1] = r->sa1; src = src_name(r->src, sn); in_src_addr[r->src][0] = s0; in_src_addr[r->src][1] = s1; }
        if (r->type == ALICE_IN_MOUSE)
            in_fmt(what, sizeof what, "mouse %d,%d buttons %x (all %x)", r->a, r->b, r->raw == -1 ? 0xffu : (uint32_t)r->raw, r->c == -1 ? 0xffu : (uint32_t)r->c);
        else if (r->type == ALICE_IN_KEY)
            in_fmt(what, sizeof what, "key %02x %s mods %02x", (uint32_t)r->a, !r->a ? "(modifiers alone)" : r->b == 2 ? "repeat" : r->b ? "down" : "up", (uint32_t)r->c);
        else if (r->type == ALICE_IN_MOUSEABS)
            in_fmt(what, sizeof what, "point %d,%d buttons %x (all %x)", r->a, r->b, (uint32_t)r->raw & 0xffu, (uint32_t)r->c & 0xffu);
        else if (r->type == ALICE_IN_MACKEY)
            in_fmt(what, sizeof what, "Mac key %02x %s mods %02x", (uint32_t)r->a, r->b ? "down" : "up", (uint32_t)r->c);
        else in_fmt(what, sizeof what, "event %u (%d,%d,%d)", r->type, r->a, r->b, r->c);
        if (!taken) in_fmt(res, sizeof res, "not taken by .PiInput after 2 s (attached %u)", inr->attached);
        else if (!fb) in_fmt(res, sizeof res, "taken (an older .PiInput: no feedback)");
        else {
            uint32_t v = infb->res[i], k = infb->km[i];
            static const char *const wn[] = { "nothing posted", "mouseDown", "mouseUp", "keyDown", "keyUp", "autoKey" };
            mods_words(v & 0xffffu, m1); mods_words(km_mods(k >> 16), m2); mods_words(k & 0xffffu, m3);
            in_fmt(res, sizeof res, "%s, MBState %02x, modifiers %04x (%s); KeyMap %s, of them .PiInput's %s",
                       (v >> 24) < 6 ? wn[v >> 24] : "?", (v >> 16) & 0xffu, v & 0xffffu, m1, m2, m3);
        }
        kprintf("[INPUT] #%u %s %s -> %s\n", r->seq, src, what, res);
        if (in_tr_left == 0) { int left = 0; for (unsigned j = 0; j < ALICE_IN_SLOTS; j++) left |= in_tr[j].used; if (!left) { kprintf("[INPUT] mac.inputtrace: %u events traced, the trace ends\n", in_tr_n); in_tr_n = 0; return; } }
    }
}
/* the heartbeat's [INPUT] line (psmac_slot_irq_report): every source's buttons and modifier keys, .PiInput's state and
   the Mac's own (MBState at $172, KeyMap's modifier keys at $17A); every heartbeat while mac.inputtrace is on, else only
   while something is held, which is the state the owner photographs */
void psmac_input_report(void)
{
    if (!inr) return;
    extern int psmac_boot_mac_talking(void);
    extern uint8_t *psmac_mac_ram_ptr(void);
    int talking = psmac_boot_mac_talking();
    uint32_t mb = 0x80, kmw = 0;
    if (talking) { volatile uint8_t *ram = psmac_mac_ram_ptr(); mb = ram[0x172]; kmw = (uint32_t)ram[0x17a] << 8 | ram[0x17b]; }
    int fb = infb && infb->magic == ALICE_IN_FB_MAGIC;
    uint32_t any = btn_all() | (fb ? infb->mods | infb->kmown | infb->btnmods : 0) | km_mods(kmw) | (talking && mb != 0x80);
    for (int s = 0; s < ALICE_NSRC; s++) for (int i = 0; i < 8; i++) any |= kb_src[s][i];
    in_trace_on();
    if (!any && !in_tr_n && in_tr_left <= 0) return;
    char t[400], sn[12], m1[48], m2[48], m3[48]; unsigned k = 0;
    k += in_fmt(t + k, sizeof t - k, "[INPUT] buttons");
    for (int s = 0; s < ALICE_NSRC; s++) if (btn_src[s] || s == ALICE_SRC_USB) k += in_fmt(t + k, sizeof t - k, " %s %x", src_name(s, sn), btn_src[s]);
    k += in_fmt(t + k, sizeof t - k, " -> %x | keyboards", btn_all());
    int kn = 0;
    for (int s = 0; s < ALICE_NSRC; s++) {
        int held = 0; for (int i = 2; i < 8; i++) held += kb_src[s][i] != 0;
        if (kb_src[s][0] || held) { k += in_fmt(t + k, sizeof t - k, " %s mods %02x keys %d", src_name(s, sn), kb_src[s][0], held); kn++; }
    }
    if (!kn) k += in_fmt(t + k, sizeof t - k, " nothing held");
    if (fb) {
        mods_words(infb->mods, m1); mods_words(infb->kmown, m2); mods_words(infb->btnmods, m3);
        k += in_fmt(t + k, sizeof t - k, " | .PiInput buttons %04x mods %s, its in KeyMap %s, the buttons' %s, %u taken",
                        infb->buttons, m1, m2, m3, infb->taken);
    } else k += in_fmt(t + k, sizeof t - k, " | .PiInput %s", inr->attached ? "attached (no feedback: an older one)" : "not open");
    if (talking) { mods_words(km_mods(kmw), m1); k += in_fmt(t + k, sizeof t - k, " | Mac MBState %02x (%s), KeyMap %s", mb, mb & 0x80 ? "up" : "DOWN", m1); }
    if (in_tr_n) k += in_fmt(t + k, sizeof t - k, " | trace %d to come", in_tr_left);
    kprintf("%s\n", t);
}

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
    /* Aurora68 Setup and Recovery's keyboard menu takes the keys in a recovery start; in any start it watches for Esc
       held in the first seconds (psmac_recovery.c) */
    { extern int psmac_recovery_key(uint32_t, int, uint32_t); if (psmac_recovery_key(usage, down, mods)) return; }
    { extern int psmac_splash_key(uint32_t, int); if (psmac_splash_key(usage, down)) return; }   /* F12: the log over the HDMI splash */
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
static uint8_t kb_last[8];                       /* the keys held now, all sources together (in_attach_poll gives them to .PiInput when it opens) */
static void kb_apply(const uint8_t report[8])
{
    uint8_t *last = kb_last;
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
/* every source's keyboard together: the modifiers OR'd, the keys held by any (six at most, as a boot report has) */
static void kb_merge_apply(void)
{
    uint8_t m[8];
    inm_kb_merge((const uint8_t (*)[8])kb_src, ALICE_NSRC, m);
    kb_apply(m);
}
void frf_native_usb_keyboard_boot_report(const uint8_t report[8])
{
    inm_kb_set(kb_src[in_src], report);             /* this source's (ErrorRollOver keeps its keys: alice_inmerge.h) */
    kb_merge_apply();
}
/* a source gone (a Bluetooth link down, a USB enumeration): its keys and buttons let go, the others' kept */
void psmac_input_gone(int src)
{
    if (src < 0 || src >= ALICE_NSRC) return;
    int was = in_src, any = 0;
    in_src = src;
    for (int i = 0; i < 8; i++) { any |= kb_src[src][i]; kb_src[src][i] = 0; }
    if (any) kb_merge_apply();
    if (btn_src[src]) { btn_src[src] = 0; in_put(ALICE_IN_MOUSE, 0, 0, (int32_t)btn_all()); }
    in_src = was;
}
void frf_native_usb_keyboard_force_release(void) { psmac_input_gone(ALICE_SRC_USB); }
int  frf_native_usb_reboot_requested(void) { return 0; }
int  frf_native_usb_amiga_reset_requested(void) { return 0; }
/* a mouse (USB, Bluetooth, the testbed's scripts): in Setup and Recovery it moves through the keyboard menu (psmac_recovery_mouse) */
extern int psmac_recovery_mouse(int dx, int dy, unsigned buttons, int wheel);
void aga_virtual_mouse_update(int dx, int dy, unsigned buttons)
{
    if (psmac_recovery_mouse(dx, dy, buttons, 0)) return;
    int32_t c = -1;
    if (buttons != 0xffffffffu) { btn_src[in_src] = (uint8_t)(buttons & 7u); c = (int32_t)btn_all(); }
    if (dx || dy || buttons != 0xffffffffu) ring_put(ALICE_IN_MOUSE, dx, dy, c, buttons == 0xffffffffu ? -1 : (int32_t)(buttons & 7u));
}
void pt_mouse_extra(int wheel, int pan, unsigned buttons) { (void)buttons; if (wheel && psmac_recovery_mouse(0, 0, 0xffffffffu, wheel)) return; if (wheel || pan) in_put(ALICE_IN_WHEEL, wheel, pan, 0); }

/* The keys held when .PiInput opens (23 November 2026, docs/08 item 136). The driver starts from an empty ring
   (piinput.s: "start from now: drop what happened before boot"), so a key that went down before it opened was never
   seen: a keyboard only reports changes, and a Shift held from power-on for "Extensions off" (or the Option or
   Command keys of the other startup combinations) had already been reported. When the ring's attached word comes
   on, the modifiers and every key held are put into it again, as if pressed then; the driver sets KeyMap from them,
   which is what the System's checks read. */
/* And before it opens (the same day, the first test of the above: the testbed's System 7.1 decided "Extensions on"
   with Shift held from power-on, since .PiInput opens only some 1.5 s before the first INIT, after the System has
   looked). What the System and the ROM look at is KeyMap, sixteen bytes of the Mac's low memory at $174, one bit per
   virtual key code (code k: byte k >> 3, bit k & 7, item 39): so from the Mac's first driver Open (its low memory is
   set up by then, the RAM test long over) until .PiInput takes over, the keys held on the Pi's keyboards are kept set
   in KeyMap by the kernel itself, every hundredth of a second, and taken out again when let go. Only in the Mac's first
   start after power-on: a restart from the Finder runs the ROM's tests again over that memory. */
static const uint8_t adb_to_hid_tab[0x36] = {
    0x04, 0x16, 0x07, 0x09, 0x0b, 0x0a, 0x1d, 0x1b, 0x06, 0x19, 0, 0x05, 0x14, 0x1a, 0x08, 0x15,
    0x1c, 0x17, 0x1e, 0x1f, 0x20, 0x21, 0x23, 0x22, 0x2e, 0x26, 0x24, 0x2d, 0x25, 0x27, 0x30, 0x12,
    0x18, 0x2f, 0x0c, 0x13, 0x28, 0x0f, 0x0d, 0x34, 0x0e, 0x33, 0x31, 0x36, 0x38, 0x11, 0x10, 0x37,
    0x2b, 0x2c, 0x35, 0x2a, 0, 0x29,
};
static void in_keymap_poll(void)
{
    static int done; static uint8_t ours[16]; static uint64_t next, first; static int said;
    extern int psmac_boot_mac_talking(void);
    extern uint8_t *psmac_mac_ram_ptr(void);
    if (done) return;
    /* .PiInput has it now (the ring's attached word, or since 24 November 2026 its feedback block's magic, written at
       its Open whether or not its slot interrupt could be installed); or two minutes have gone by since the Mac began
       to talk (a start without .PiInput, which should never keep KeyMap from the kernel for ever) */
    int taken = inr && (inr->attached || (infb && infb->magic == ALICE_IN_FB_MAGIC));
    uint64_t t = in_now();
    if (taken || (first && t - first > in_hz() * 120)) {
        done = 1;
        /* Our bits out of KeyMap first: .PiInput is given the keys held again (in_attach_poll, just after this) and sets
           them itself, as its own, so that it takes them out when they are let go. Until this day the kernel's bits
           stayed: a Shift held from power-on and let go after .PiInput had opened stayed down in KeyMap for good. */
        if (psmac_boot_mac_talking()) {
            volatile uint8_t *km = psmac_mac_ram_ptr() + 0x174;
            for (int i = 0; i < 16; i++) if (ours[i]) km[i] = km[i] & (uint8_t)~ours[i];
        }
        if (said) kprintf("[PSMAC] KeyMap: %s now, the kernel's bits taken out\n", taken ? ".PiInput has the keyboards" : "no .PiInput after two minutes: the kernel stops keeping it");
        return;
    }
    if (t < next || !psmac_boot_mac_talking()) return;
    if (!first) first = t;
    next = t + in_hz() / 100;
    uint8_t want[16] = { 0 };
    uint32_t m = kb_last[0];
    if (in_altcmd < 0) { extern const char *psmac_option(const char *); const char *o = psmac_option("mac.altcmd"); in_altcmd = o && o[0] == '1'; }
    if (in_altcmd) m = (m & ~0xccu) | (m & 0x44u) << 1 | (m & 0x88u) >> 1;
    if (m & 0x22) want[0x38 >> 3] |= 1u << (0x38 & 7);          /* Shift */
    if (m & 0x11) want[0x3b >> 3] |= 1u << (0x3b & 7);          /* Control */
    if (m & 0x44) want[0x3a >> 3] |= 1u << (0x3a & 7);          /* Alt: Option */
    if (m & 0x88) want[0x37 >> 3] |= 1u << (0x37 & 7);          /* GUI: Command */
    for (int i = 2; i < 8; i++) for (int k = 0; kb_last[i] && k < (int)sizeof adb_to_hid_tab; k++)
        if (adb_to_hid_tab[k] == kb_last[i]) want[k >> 3] |= (uint8_t)(1u << (k & 7));
    volatile uint8_t *km = psmac_mac_ram_ptr() + 0x174;
    int any = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t clear = ours[i] & (uint8_t)~want[i];            /* let go: our bits out again, the Mac's own left alone */
        if (clear) km[i] = km[i] & (uint8_t)~clear;
        if (want[i] && (km[i] & want[i]) != want[i]) km[i] = km[i] | want[i];
        ours[i] = want[i]; any |= want[i];
    }
    if (any && !said) {
        said = 1;
        extern void psmac_boot_mark(const char *fmt, ...);
        psmac_boot_mark("keys held on the Pi's keyboards put into the Mac's KeyMap (modifiers %02x) until .PiInput opens", kb_last[0]);
    }
}
static void in_attach_poll(void)
{
    static int was;
    in_keymap_poll();
    in_trace_poll();
    int now_ = inr && inr->attached;
    if (now_ && !was) {
        extern void psmac_boot_mark(const char *fmt, ...);
        int n = 0;
        if (kb_last[0]) in_put(ALICE_IN_KEY, 0, 0, kb_last[0]);
        for (int i = 2; i < 8; i++) if (kb_last[i]) { in_put(ALICE_IN_KEY, kb_last[i], 1, kb_last[0]); n++; }
        if (n || kb_last[0]) psmac_boot_mark(".PiInput opened: keyboards and mice reach the Mac; %d key(s) and modifiers %02x held, given to it again", n, kb_last[0]);
        else psmac_boot_mark(".PiInput opened: keyboards and mice reach the Mac");
    }
    was = now_;
}
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
    if (intest_kind == 6) {                                                 /* mac.inputedge=: the edge jump between two screens (docs/10) */
        /* For Aurora68 Screen Setup's arrangement, the SE's screen at (0,0)-(512,342) and the HDMI one to its
           right, tops aligned: across where they touch, then down the HDMI screen and against the wall below
           the SE's, then back. Each pause is long enough for a MAME snapshot of the SE's screen (every ten
           seconds) and a tools/vcshot.py of the HDMI one; .PiInput logs every jump with its points. */
        static const struct { int16_t n, dx, dy, pause_s; const char *what; } edge_script[] = {
            { 40, -30, -20, 0, 0 },                 /* the top-left corner: (0, 0) on the SE's screen */
            { 10, 0, 10, 0, "on the SE screen at (0,100)" },
            { 26, 20, 0, 12, "across the SE screen's right edge at y 100" },
            { 20, 0, 25, 12, "down the HDMI screen" },
            { 4, -20, 0, 12, "against the HDMI screen's left edge below the SE screen" },
            { 8, 20, 0, 12, "back across the SE screen's right edge" },
        };
        static int phase, k;
        uint32_t gap_ms = 50;
        if (phase >= (int)(sizeof edge_script / sizeof edge_script[0])) { intest_at = 0; kprintf("[PSMAC] input edge script done\n"); return; }
        if (k < edge_script[phase].n) { aga_virtual_mouse_update(edge_script[phase].dx, edge_script[phase].dy, 0); k++; }
        else {
            if (edge_script[phase].what) kprintf("[PSMAC] input edge script: %s, pausing %d s\n", edge_script[phase].what, edge_script[phase].pause_s);
            gap_ms = 1000u * (uint32_t)edge_script[phase].pause_s + 50; phase++; k = 0;
        }
        intest_next = now + in_hz() * gap_ms / 1000; return;
    }
    if (intest_kind == 7) {
        /* mac.inputmods=SECONDS (24 November 2026, docs/08 item 139): the owner's "some key held down", through the
           real report path with sources. A Bluetooth keyboard with a pointer (link 0, 00:11:22:33:44:55): Control-
           Option-Command-P as Aurora68 PC's capture key is typed (P let go first, then the modifiers one by one), then
           a Shift-H; its pointer presses the left button while a USB mouse reports its own button up; a second
           Bluetooth mouse (link 1) presses the right button and its link goes down. With mac.inputtrace= each event's
           line says what .PiInput did and KeyMap's modifier keys after it, and the six-second pauses after the modifiers are
           let go put a heartbeat's [INPUT] line there: nothing may be left in KeyMap (with .PiInput before this day,
           Command+Option+Control and then Shift were). */
        static const uint8_t bt_a[6] = { 0x55, 0x44, 0x33, 0x22, 0x11, 0x00 }, bt_b[6] = { 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
        static const struct { uint8_t src, kind, mods, key; int8_t b; uint16_t gap; } ms[] = {
            { 0, 0, 0x01, 0, 0, 120 }, { 0, 0, 0x05, 0, 0, 120 }, { 0, 0, 0x0d, 0, 0, 120 },   /* Control, Option, Command */
            { 0, 0, 0x0d, 0x13, 0, 120 }, { 0, 0, 0x0d, 0, 0, 120 },                            /* P down, up */
            { 0, 0, 0x0c, 0, 0, 120 }, { 0, 0, 0x08, 0, 0, 120 }, { 0, 0, 0x00, 0, 0, 6000 },  /* let go one by one; a heartbeat */
            { 0, 0, 0x02, 0, 0, 120 }, { 0, 0, 0x02, 0x0b, 0, 120 }, { 0, 0, 0x02, 0, 0, 120 }, { 0, 0, 0x00, 0, 0, 6000 },  /* Shift-H; a heartbeat */
            { 0, 1, 0, 0, 1, 200 }, { 2, 1, 0, 0, 0, 200 }, { 0, 1, 0, 0, 0, 600 },             /* BT pointer down, USB up, BT up */
            { 1, 1, 0, 0, 2, 300 }, { 1, 2, 0, 0, 0, 600 },                                     /* BT mouse 2 right down; its link goes */
        };
        int s = intest_step++;
        if (s >= (int)(sizeof ms / sizeof ms[0])) { intest_at = 0; kprintf("[PSMAC] input modifier script done\n"); return; }
        int src = ms[s].src == 2 ? ALICE_SRC_USB : ALICE_SRC_BT0 + ms[s].src;
        psmac_input_from(src, ms[s].src == 2 ? 0 : ms[s].src ? bt_b : bt_a);
        if (ms[s].kind == 0) { uint8_t r[8] = { ms[s].mods, 0, ms[s].key }; frf_native_usb_keyboard_boot_report(r); }
        else if (ms[s].kind == 1) aga_virtual_mouse_update(0, 0, (unsigned)ms[s].b);
        else psmac_input_gone(src);
        psmac_input_from(ALICE_SRC_USB, 0);
        intest_next = now + in_hz() * ms[s].gap / 1000; return;
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
void psmac_alice_input_edge(uint32_t seconds)  { intest_at = seconds; intest_kind = 6; }   /* the edge jump between the screens (docs/10) */
void psmac_alice_input_mods(uint32_t seconds)  { intest_at = seconds; intest_kind = 7; }   /* the stuck-modifier script (docs/08 item 139) */

void psmac_loop_report(void);
/* heartbeat line for psmac_bus.c: the interrupt line and what holds it */
void psmac_slot_irq_report(void)
{
    { extern void psmac_display_report(void); psmac_display_report(); }
    { extern void psmac_audio_report(void); psmac_audio_report(); }
    { extern void psmac_video_report(void); psmac_video_report(); }
    { extern void psmac_usb_report(void); psmac_usb_report(); }
    { extern void psmac_usbnet_report(void); psmac_usbnet_report(); }
    psmac_loop_report();
    extern uint32_t psmac_slot_irq_state(void); extern volatile uint8_t *alice_net_board(void);
    volatile struct an_hdr *nh = (volatile struct an_hdr *)alice_net_board();
    uint32_t st = psmac_slot_irq_state();
    kprintf("[PSMAC]   slot-9 line %u (SIER %02x IER %02x) | net irq %u drv %08x | input attached %u head %u tail %u\n",
            st >> 16, (st >> 8) & 0xff, st & 0xff, nh ? nh->irq : 0, nh ? nh->amiga_ver : 0,
            inr ? inr->attached : 0, inr ? inr->head : 0, inr ? inr->tail : 0);
    psmac_input_report();
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
/* The access point (23 November 2026, docs/08 item 118): AN_CMD_AP. The simulated radio is given the very
   sequence the real driver sends (wifi/fmac_ap.c) and checks each step against a small model of what the
   firmware takes: the role and infrastructure mode only while down, the key only while up as an access
   point, the SSID last, with the key there when WPA2 is asked for. It answers with the Broadcom error
   numbers the firmware would (BCME_NOTUP -4, NOTDOWN -5, NOTAP -6, BADARG -2, UNSUPPORTED -23), so the
   recovery screen's message for a refusal can be seen here. mac.wifisimap= changes the model:
   "refuse:NAME" refuses that step (NAME as the log names it: WLC_SET_AP, chanspec, WLC_SET_WSEC_PMK ...),
   "noidauth" is a firmware without the WPA2 authenticator (the network is then open). Once up, the frames
   of mac.wifinet are the guests': tools/pinet.py with PINET_APCLIENT=1 is a phone that joined. */
static int wsim_ap;
static char wsim_ap_msg[96];
struct wsim_fw { int up, ap, infra, pmk, wpa2; const char *refuse; unsigned refuse_len; };
static int32_t wsim_ap_step(void *ctx, const struct fm_ap_step *st)
{
    struct wsim_fw *f = ctx;
    char nm[40]; fm_ap_step_name(st, nm, sizeof nm);
    const char *bare = nm[0] == '"' ? nm + 1 : nm;
    unsigned bl = 0; while (bare[bl] && bare[bl] != '"') bl++;
    if (f->refuse && f->refuse_len == bl) { unsigned k = 0; while (k < bl && bare[k] == f->refuse[k]) k++; if (k == bl) return -23; }
    switch (st->kind) {
    case FMS_CMD_INT:
        if (st->cmd == WLC_DOWN) { f->up = 0; return 0; }
        if (st->cmd == WLC_UP) { f->up = 1; return 0; }
        if (st->cmd == WLC_SET_INFRA) { if (f->up) return -5; f->infra = (int)st->val; return 0; }
        if (st->cmd == WLC_SET_AP) { if (f->up) return -5; f->ap = (int)st->val; return 0; }
        if (st->cmd == WLC_SET_CHANNEL) return st->val >= 1 && st->val <= 13 ? 0 : -2;
        return 0;
    case FMS_IOV_INT:
        if (bl == 8 && bare[0] == 'c' && bare[1] == 'h') return (st->val & 0xff) >= 1 && (st->val & 0xff) <= 13 ? 0 : -2;   /* chanspec */
        if (bl == 8 && bare[0] == 'w' && bare[1] == 'p') { f->wpa2 = st->val == WPA2_AUTH_PSK; return 0; }                 /* wpa_auth */
        return 0;
    case FMS_CMD_DATA:
        if (st->cmd == WLC_SET_WSEC_PMK) {
            if (!f->up) return -4;
            if (!f->ap) return -6;
            unsigned kl = (unsigned)st->data[0] | (unsigned)st->data[1] << 8, fl = (unsigned)st->data[2] | (unsigned)st->data[3] << 8;
            if (!((kl == 32 && fl == 0) || (kl >= 8 && kl <= 63 && fl == 1))) return -2;
            f->pmk = 1; return 0;
        }
        if (st->cmd == WLC_SET_SSID) {
            if (!f->up) return -4;
            if (!f->ap || f->infra != 1) return -6;
            if (!st->data[0] || st->data[0] > 32 || (f->wpa2 && !f->pmk)) return -2;
            return 0;
        }
        return 0;
    default:
        return 0;
    }
}
static void wsim_ap_log(void *ctx, const struct fm_ap_step *st, int32_t e, int skipped)
{
    (void)ctx;
    if (skipped) return;
    char nm[40]; fm_ap_step_name(st, nm, sizeof nm);
    int num = st->kind == FMS_CMD_INT || st->kind == FMS_IOV_INT;
    if (!e) kprintf("[WIFI-SIM] access point: %s %u%s ok (%s)\n", nm, num ? st->val : st->len, num ? "" : " bytes", st->what);
    else kprintf("[WIFI-SIM] access point: %s %u%s REFUSED, error %d (%s)\n", nm, num ? st->val : st->len, num ? "" : " bytes", e, st->what);
}
static void wsim_ap_start(volatile struct an_unit *u, uint32_t seq)
{
    volatile struct an_ap *a = (volatile struct an_ap *)u->arg;
    char ssid[33], pass[64], cc[3];
    unsigned i = 0, pl = 0;
    for (; i < 32 && a->ssid[i]; i++) ssid[i] = a->ssid[i];
    ssid[i] = 0;
    for (; pl < 63 && a->pass[pl]; pl++) pass[pl] = a->pass[pl];
    pass[pl] = 0;
    cc[0] = a->country[0]; cc[1] = a->country[1]; cc[2] = 0;
    unsigned channel = a->channel, flags = a->flags;
    for (i = 0; i < sizeof u->arg; i++) u->arg[i] = 0;
    if (!ssid[0] || channel < 1 || channel > 13 || (pl && pl < 8)) { wsim_finish(u, seq, -2); return; }
    const char *model = psmac_option("mac.wifisimap");
    int idauth = !(model && model[0] == 'n');
    int wpa2 = pass[0] && (idauth || (flags & AN_AP_WPA2_ANYWAY));
    kprintf("[WIFI-SIM] access point %s asked for on channel %u: the simulated firmware %s \"idauth\": %s\n", ssid, channel, idauth ? "lists" : "does not list", wpa2 ? "WPA2-PSK" : "open");
    uint8_t pmk[32];
    if (wpa2) fm_wpa_pmk(pass, pl, ssid, (unsigned)__builtin_strlen(ssid), pmk, 0);
    struct fm_ap_cfg c = { ssid, channel, 0, wpa2 ? pmk : 0, wpa2 ? pass : 0, cc[0] ? cc : 0 };
    static struct fm_ap_step steps[FM_AP_STEPS_MAX];
    unsigned n = fm_ap_steps(steps, FM_AP_STEPS_MAX, &c);
    struct wsim_fw fw = { 1, 0, 1, 0, 0, 0, 0 };
    if (model && model[0] == 'r' && model[6] == ':') { fw.refuse = model + 7; while (fw.refuse[fw.refuse_len] && fw.refuse[fw.refuse_len] != ' ' && fw.refuse[fw.refuse_len] != ',') fw.refuse_len++; }
    char why[64];
    int32_t e = n ? fm_ap_run(steps, n, wsim_ap_step, wsim_ap_log, &fw, why, sizeof why) : -2;
    for (i = 0; i < sizeof steps; i++) ((uint8_t *)steps)[i] = 0;
    for (i = 0; i < 32; i++) pmk[i] = 0;
    for (i = 0; i < sizeof pass; i++) pass[i] = 0;
    wsim_state(u, AN_ST_JOINING, "starting the access point (simulated radio)");
    u->link = 0; wsim_want = 0;
    unsigned k = 0;
    const char *t;
    if (e) {
        for (t = "the WiFi chip refused AP mode: "; *t; ) wsim_ap_msg[k++] = *t++;
        for (t = n ? why : "its name or channel"; *t && k < 94; ) wsim_ap_msg[k++] = *t++;
        wsim_ap_msg[k] = 0;
        wsim_ap = 0;
        wsim_later(800, AN_ST_FAILED, wsim_ap_msg, seq, -5);
        return;
    }
    for (t = "access point "; *t; ) wsim_ap_msg[k++] = *t++;
    for (t = ssid; *t && k < 60; ) wsim_ap_msg[k++] = *t++;
    for (t = " up: channel "; *t; ) wsim_ap_msg[k++] = *t++;
    if (channel >= 10) wsim_ap_msg[k++] = '1';
    wsim_ap_msg[k++] = (char)('0' + channel % 10);
    for (t = wpa2 ? ", WPA2 (simulated radio)" : ", open (simulated radio)"; *t && k < 94; ) wsim_ap_msg[k++] = *t++;
    wsim_ap_msg[k] = 0;
    for (i = 0; ssid[i]; i++) u->ssid[i] = ssid[i];
    u->ssid[i] = 0;
    for (i = 0; i < 6; i++) u->bssid[i] = wsim_mac[i];
    u->channel = channel; u->security = wpa2 ? SEC_WPA2 : SEC_OPEN; u->rssi = 0; u->speed_mbps = 72;
    wsim_ap = 1;
    wsim_later(1500, AN_ST_CONNECTED, wsim_ap_msg, seq, 0);
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
   ring: there is no device.
   Since 23 November 2026 (docs/08 item 136, Bluetooth in Setup and Recovery) the simulated host also keeps its
   pairings where the real one does: every change goes to psmac_card_save_pairings as bt_pi.c's bt.dev= lines (so
   pairings.txt is written, and in Setup and Recovery through the recovery's own file layer), and a start reads
   pairings.txt back: a simulated device found there is paired again and "reconnects" a second later. And
   mac.btsimkeys=SECONDS:TOKENS is a paired keyboard and mouse typing and moving, injected where bt_pi.c's HID
   reports go (frf_native_usb_keyboard_boot_report, aga_virtual_mouse_update), SECONDS after the simulated host is up,
   one token every quarter of a second: a letter or digit, ret esc up down left right bs tab space, shift / noshift
   (Shift held from then on, or let go), mdown mup (the mouse 120 counts down or up), click rclick, wN (N tenths of a
   second's pause). Keys wait until the simulated keyboard is connected (pair "Magic Keyboard" first, or have it in
   pairings.txt), mouse tokens until a mouse is. */
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
/* the simulated pairings as bt_pi.c writes them (keys_save): "# NAME" and "bt.dev=ADDR:KEY:KIND", the address most
   significant octet first */
extern void psmac_card_save_pairings(const char *text, uint32_t len);
extern const char *psmac_card_pairings(int *state);
static void bsim_save(volatile struct an_bt *b)
{
    static char t[1024]; unsigned n = 0; static const char hx[] = "0123456789abcdef";
    static const char head[] = "# Bluetooth pairings, written by the Aurora68 kernel (the testbed's simulated host)\n";
    for (unsigned i = 0; head[i]; i++) t[n++] = head[i];
    for (unsigned k = 0; k < b->nkeys && n < sizeof t - 120; k++) {
        t[n++] = '#'; t[n++] = ' '; for (int i = 0; i < 39 && b->keys[k].name[i]; i++) t[n++] = b->keys[k].name[i];
        t[n++] = '\n';
        for (const char *q = "bt.dev="; *q; q++) t[n++] = *q;
        for (int i = 5; i >= 0; i--) { t[n++] = hx[b->keys[k].addr[i] >> 4]; t[n++] = hx[b->keys[k].addr[i] & 15]; }
        t[n++] = ':';
        for (int i = 0; i < 16; i++) { t[n++] = hx[b->keys[k].key[i] >> 4]; t[n++] = hx[b->keys[k].key[i] & 15]; }
        t[n++] = ':'; t[n++] = hx[(b->keys[k].flags & AN_BTK_KEYBOARD ? 1 : 0) | (b->keys[k].flags & AN_BTK_MOUSE ? 2 : 0)];
        t[n++] = '\n';
    }
    psmac_card_save_pairings(t, n);
    kprintf("[BT-SIM] pairings handed to the card: %u device(s)\n", b->nkeys);
}
static int bsim_hex(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static uint64_t bsim_reconnect_at;
/* pairings.txt as the card had it at this start: the simulated devices named there are paired again */
static void bsim_load(volatile struct an_bt *b, const char *t)
{
    for (const char *p = t; p && *p; p++) {
        if ((p != t && p[-1] != '\n') || p[0] != 'b' || p[1] != 't' || p[2] != '.' || p[3] != 'd' || p[4] != 'e' || p[5] != 'v' || p[6] != '=') continue;
        uint8_t a[6]; int ok = 1;
        for (int i = 0; i < 6 && ok; i++) { int h = bsim_hex(p[7 + 2 * i]), l = bsim_hex(p[8 + 2 * i]); if (h < 0 || l < 0) ok = 0; else a[5 - i] = (uint8_t)(h << 4 | l); }
        if (!ok) continue;
        for (int k = 0; k < 3; k++) {
            int same = 1; for (int i = 0; i < 6; i++) if (bsim_near[k].addr[i] != a[i]) same = 0;
            if (same && bsim_near[k].flags) { bsim_add(b, bsim_near[k].name, a, bsim_near[k].flags); kprintf("[BT-SIM] %s paired (pairings.txt): it reconnects in a second\n", bsim_near[k].name); bsim_reconnect_at = in_now() + in_hz(); }
        }
    }
}
/* mac.btsimkeys=SECONDS:TOKENS (above) */
static const char *bks; static uint64_t bks_next; static int bks_shift, bks_phase, bks_rep; static uint8_t bks_usage; static unsigned bks_button;
static int bsim_connected(volatile struct an_bt *b, uint32_t kind)
{
    for (unsigned k = 0; k < b->nkeys; k++) if ((b->keys[k].flags & AN_BTK_CONNECTED) && (b->keys[k].flags & kind)) return 1;
    return 0;
}
static void bsim_keys_poll(volatile struct an_bt *b, uint64_t now)
{
    if (!bks || !*bks || now < bks_next) return;
    if (bks_phase == 1) {                                       /* the key or button let go */
        uint8_t r[8] = { (uint8_t)(bks_shift ? 0x02 : 0) };
        if (bks_usage) frf_native_usb_keyboard_boot_report(r);
        if (bks_button) aga_virtual_mouse_update(0, 0, 0);
        bks_usage = 0; bks_button = 0; bks_phase = 0;
        bks_next = now + in_hz() * 170 / 1000;
        return;
    }
    char tok[16]; unsigned n = 0, len = 0;
    while (bks[len] && bks[len] != ',') len++;
    while (n < len && n < sizeof tok - 1) { tok[n] = bks[n]; n++; }
    tok[n] = 0;
    int reps = 1;                                               /* TOKENxN: N times (the boot arguments' line is short) */
    for (int i = (int)n - 1; i > 0; i--) {
        if (tok[i] >= '0' && tok[i] <= '9') continue;
        if (tok[i] == 'x' && i + 1 < (int)n) { reps = 0; for (unsigned k = (unsigned)i + 1; k < n; k++) reps = reps * 10 + (tok[k] - '0'); tok[i] = 0; n = (unsigned)i; }
        break;
    }
    int mouse = tok[0] == 'm' || !__builtin_strcmp(tok, "click") || !__builtin_strcmp(tok, "rclick");
    if (tok[0] && !(tok[0] == 'w' && tok[1] >= '0' && tok[1] <= '9') && !bsim_connected(b, mouse ? AN_BTK_MOUSE : AN_BTK_KEYBOARD)) { bks_next = now + in_hz() / 4; return; }
    if (++bks_rep >= reps) { bks_rep = 0; bks += len; if (*bks == ',') bks++; }
    static const struct { const char *w; uint8_t u; } named[] = {
        { "ret", 0x28 }, { "esc", 0x29 }, { "bs", 0x2a }, { "tab", 0x2b }, { "space", 0x2c }, { "right", 0x4f }, { "left", 0x50 }, { "down", 0x51 }, { "up", 0x52 } };
    uint8_t u = 0;
    if (n == 1 && tok[0] >= 'a' && tok[0] <= 'z') u = (uint8_t)(0x04 + tok[0] - 'a');
    else if (n == 1 && tok[0] >= '1' && tok[0] <= '9') u = (uint8_t)(0x1e + tok[0] - '1');
    else if (n == 1 && tok[0] == '0') u = 0x27;
    for (unsigned i = 0; !u && i < sizeof named / sizeof named[0]; i++) if (!__builtin_strcmp(tok, named[i].w)) u = named[i].u;
    bks_next = now + in_hz() * 80 / 1000;
    if (u) { uint8_t r[8] = { (uint8_t)(bks_shift ? 0x02 : 0), 0, u }; frf_native_usb_keyboard_boot_report(r); bks_usage = u; bks_phase = 1; kprintf("[BT-SIM] key %s\n", tok); return; }
    if (!__builtin_strcmp(tok, "shift") || !__builtin_strcmp(tok, "noshift")) {
        bks_shift = tok[0] == 's'; uint8_t r[8] = { (uint8_t)(bks_shift ? 0x02 : 0) }; frf_native_usb_keyboard_boot_report(r);
        kprintf("[BT-SIM] Shift %s\n", bks_shift ? "held" : "let go"); bks_next = now + in_hz() / 4; return;
    }
    if (!__builtin_strcmp(tok, "mdown") || !__builtin_strcmp(tok, "mup")) { aga_virtual_mouse_update(0, tok[1] == 'd' ? 120 : -120, 0); kprintf("[BT-SIM] mouse %s\n", tok + 1); bks_next = now + in_hz() / 4; return; }
    if (!__builtin_strcmp(tok, "click") || !__builtin_strcmp(tok, "rclick")) { bks_button = tok[0] == 'r' ? 2 : 1; aga_virtual_mouse_update(0, 0, bks_button); bks_phase = 1; kprintf("[BT-SIM] %s\n", tok); return; }
    if (tok[0] == 'w') { uint32_t v = 0; for (const char *q = tok + 1; *q >= '0' && *q <= '9'; q++) v = v * 10 + (uint32_t)(*q - '0'); bks_next = now + in_hz() * v / 10; return; }
    kprintf("[BT-SIM] mac.btsimkeys: \"%s\" is not a token\n", tok);
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
        int st = -1; const char *pt = psmac_card_pairings(&st);
        static uint64_t wait0; if (!wait0) wait0 = now;
        if (st == 0 && now - wait0 < in_hz() * 10) return 1;        /* the card's pairings first, as bt_pi.c waits for them */
        for (unsigned i = 0; i < sizeof *b; i++) ((volatile uint8_t *)b)[i] = 0;
        static const uint8_t own[6] = { 0x02, 0x50, 0x53, 0x42, 0x54, 0x01 }, mouse[6] = { 0x41, 0x52, 0x63, 0x74, 0x85, 0x96 };
        for (int i = 0; i < 6; i++) b->bdaddr[i] = own[i];
        bsim_add(b, "Logitech M337", mouse, AN_BTK_MOUSE | AN_BTK_CONNECTED);
        if (st == 1 && pt) bsim_load(b, pt);
        b->state = AN_BT_READY; bsim_msg(b, "ready (simulated)");
        bsim_phase = 1;
        kprintf("[BT-SIM] testbed Bluetooth: a simulated host, %u paired\n", b->nkeys);
        { extern void psmac_boot_mark(const char *fmt, ...); psmac_boot_mark("Bluetooth (simulated): ready, %u paired", b->nkeys); }
        const char *o = psmac_option("mac.btsimkeys");
        if (o) { uint32_t v = 0; while (*o >= '0' && *o <= '9') v = v * 10 + (uint32_t)(*o++ - '0'); if (*o == ':') o++; bks = o; bks_next = now + in_hz() * v; kprintf("[BT-SIM] mac.btsimkeys: from %u s after the start of the simulated host\n", v); }
    }
    if (bsim_reconnect_at && now >= bsim_reconnect_at) {
        bsim_reconnect_at = 0;
        for (unsigned k = 0; k < b->nkeys; k++) if (!(b->keys[k].flags & AN_BTK_CONNECTED)) {
            char nm[40]; int i = 0; for (; i < 39 && b->keys[k].name[i]; i++) nm[i] = b->keys[k].name[i];
            nm[i] = 0;
            b->keys[k].flags |= AN_BTK_CONNECTED; kprintf("[BT-SIM] %s connected\n", nm);
            extern void psmac_boot_mark(const char *fmt, ...); psmac_boot_mark("Bluetooth (simulated): a paired device connected");
        }
        bsim_msg(b, "connected");
    }
    bsim_keys_poll(b, now);
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
            bsim_save(b);
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
            bsim_save(b);
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
        wsim_later(1200, AN_ST_IDLE, "idle: scan or join (S / J in Aurora68 Wi-Fi)", 0, 0);
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
        if (st == AN_ST_SCANNING) { wsim_scan_fill(u); st = u->link ? AN_ST_CONNECTED : AN_ST_IDLE; wsim_next_msg = u->link ? (wsim_ap ? wsim_ap_msg : "connected") : "idle: 4 networks found"; }
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
            wsim_ap = 0;
            if (u->state == AN_ST_OFF) { wsim_want = 1; wsim_state(u, AN_ST_STARTING, "starting"); wsim_later(1200, AN_ST_IDLE, "idle", seq, 0); break; }
            wsim_join(u, seq);
            break; }
        case AN_CMD_DISCONNECT: u->link = 0; wsim_want = 0; wsim_ap = 0; wsim_state(u, u->state == AN_ST_OFF ? AN_ST_OFF : AN_ST_IDLE, "disconnected"); wsim_finish(u, seq, 0); break;
        case AN_CMD_RXON: wsim_rx_on = 1; u->rx_on = 1; wsim_finish(u, seq, 0); break;
        case AN_CMD_RXOFF: wsim_rx_on = 0; u->rx_on = 0; wsim_finish(u, seq, 0); break;
        case AN_CMD_FILTER: { uint32_t f = arg32(u, 0); wsim_promisc = !!(f & AN_FILT_PROMISC); wsim_allmulti = !!(f & AN_FILT_ALLMULTI); wsim_finish(u, seq, 0); break; }
        case AN_CMD_SETMAC: {
            int zero = 1; for (int i = 0; i < 6; i++) if (u->arg[i]) zero = 0;
            for (int i = 0; i < 6; i++) wsim_mac[i] = zero ? u->factory_mac[i] : u->arg[i];
            for (int i = 0; i < 6; i++) u->mac[i] = wsim_mac[i];
            wsim_finish(u, seq, 0); break; }
        case AN_CMD_WIFICFG: case AN_CMD_ETHCFG: wsim_finish(u, seq, 0); break;
        case AN_CMD_AP: u->admin_off = 0; wsim_ap_start(u, seq); break;
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
    infb = (volatile struct alice_input_fb *)((volatile uint8_t *)inr + ALICE_IN_FB_OFF);
    for (uint32_t i = 0; i < sizeof(struct alice_input_fb) / 4; i++) ((volatile uint32_t *)infb)[i] = 0;
    inr->magic = ALICE_IN_MAGIC; inr->version = 1; inr->slots = ALICE_IN_SLOTS;
    /* mac.edgejump=off: the cursor stops at a screen's edge where the neighbour does not touch it, as the ROM
       has it; on, the default, .PiInput takes it onto the neighbour at the same proportion (docs/10, "The edge jump") */
    { extern int psmac_bootarg_val(const char *key, char *out, unsigned max); char o[8];
      if (psmac_bootarg_val("mac.edgejump", o, sizeof o) && (o[0] == '0' || ((o[0] | 32) == 'o' && (o[1] | 32) == 'f')))
          inr->options |= ALICE_IN_OPT_NOEDGE; }
    /* mac.rightbutton=cmd|ctrl|none (24 November 2026): what a mouse's right button adds to its click; cmd, the default,
       is the Command-click Apple's own two-button drivers of the day made of it (docs/09, .PiInput) */
    { extern int psmac_bootarg_val(const char *key, char *out, unsigned max); char o[8];
      if (psmac_bootarg_val("mac.rightbutton", o, sizeof o)) {
          if ((o[0] | 32) == 'c' && (o[1] | 32) == 't') inr->options |= ALICE_IN_OPT_RBCTRL;
          else if ((o[0] | 32) == 'n' || (o[0] | 32) == 'o' || o[0] == '0') inr->options |= ALICE_IN_OPT_RBNONE;
      } }
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
    { extern void psmac_audio_mac_reset(void); psmac_audio_mac_reset(); }     /* PiPlay's stream stops (psmac_audio.c) */
    { extern void psmac_video_reset(void); psmac_video_reset(); }             /* and the video card's (psmac_video.c) */
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
        /* bt_pi_uart_pump between the parts (docs/21): the Pi 3B's Bluetooth UART has no flow control and a 16-byte FIFO,
           1.4 ms at 115200 baud, and the parts of one turn add up to more; a register read when nothing waits */
        LOOP_AT("mailbox");
        psmac_mbox_poll();
        bt_pi_uart_pump();
        t1 = in_now(); LOOP_T(0, t1 - t0); t0 = t1;
        LOOP_AT("card");
        { extern void psmac_card_poll(void); psmac_card_poll(); }        /* the Pi's own SD card: pairings, the disk image's changes (psmac_card.c) */
        bt_pi_uart_pump();
        t1 = in_now(); LOOP_T(1, t1 - t0); t0 = t1;
        LOOP_AT("network");
        alice_net_poll();
        bt_pi_uart_pump();
        t1 = in_now(); LOOP_T(2, t1 - t0); t0 = t1;
        LOOP_AT("USB");
        { extern void psmac_usb_poll(void); psmac_usb_poll(); }          /* USB keyboard and mouse (psmac_usb.c) */
        bt_pi_uart_pump();
        t1 = in_now(); LOOP_T(3, t1 - t0); t0 = t1;
        LOOP_AT("input");
        in_attach_poll();
        in_repeat_poll();
        in_testbed_poll();
        LOOP_AT("echo timings");
        { extern void psmac_netprobe_poll(void); psmac_netprobe_poll(); }  /* echo timings (psmac_netprobe.c) */
        LOOP_AT("power state");
        { extern void psmac_power_poll(void); psmac_power_poll(); }        /* the Pi's power state, every 2 s (psmac_power.c) */
        LOOP_AT("display");
        { extern void psmac_display_poll(void); psmac_display_poll(); }   /* the second display's tick and picture */
        { extern void psmac_hdmi_poll(void); psmac_hdmi_poll(); }         /* PiDisplay's card writes, its keep-or-revert question, the Pi's restart */
        t1 = in_now(); LOOP_T(4, t1 - t0); t0 = t1; loop_turns++; loop_turns_all++;
        LOOP_AT("remote control");
        { extern void psmac_remote_poll(void); psmac_remote_poll(); }     /* the remote control: its IP stack, HTTP and input queue (docs/13) */
        LOOP_AT("recovery");
        { extern void psmac_recovery_poll(void); psmac_recovery_poll(); } /* Aurora68 Setup and Recovery: its page and screen, or in a normal start its few watches */
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
