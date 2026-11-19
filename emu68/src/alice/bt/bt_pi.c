/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Alice68 kernel Bluetooth on the Pi 4: the CYW43455's Bluetooth half on UART0
 * (the PL011, GPIO 30-33 in ALT3: CTS, RTS, TXD, RXD, with hardware flow
 * control) and its BT_ON line (the firmware's GPIO expander, line 0), driving
 * bthost.c from CPU1's loop (alice_net_poll). Everything is polled: no
 * interrupts, no waiting - the start-up's pauses are timestamps.
 *
 * The firmware (BCM4345C0.hcd, or BCM4345C5.hcd) comes from AmigaOS like the
 * WiFi's: alice68hdf.device hands over the WIFI folder's files. Paired devices
 * come from cmdline.txt (bt.dev=ADDR:KEY:KIND, written by Alice68-Card), so a
 * keyboard known before reconnects as soon as Bluetooth is up.
 *
 * Keyboards feed the same path as USB ones (frf_native_usb_keyboard_boot_report:
 * Ctrl-Amiga-Amiga and the rest work alike); mice the virtual mouse port and
 * the wheel (pt_mouse_extra) for Alice68-Wheel.
 */
#include <stdint.h>
#include "support.h"
#include "spinlock.h"
#include "bthost.h"
#include "../alice_net.h"

extern int alice_mbox(uint32_t tag, uint32_t a, uint32_t b, uint32_t *out);
extern const uint8_t *alice_fw_get(const char *const *stems, const char *suffix, uint32_t *len);
extern int alice_fw_offered(void);
extern volatile uint8_t *alice_net_board(void);
extern void alice_net_ring(const char *s, unsigned n);
extern const char *alice_bootargs(void);
extern void frf_post_bt(const char *text, int col);
extern void frf_native_usb_keyboard_boot_report(const uint8_t report[8]) __attribute__((weak));
extern void aga_virtual_mouse_update(int dx, int dy, unsigned buttons);
extern void pt_mouse_extra(int wheel, int pan, unsigned buttons) __attribute__((weak));
extern int alice_pi_board_type(void);                    /* alice_net.c: revision code bits 4-11 */
extern int alice_pi_is_2711(void);
extern void psmac_console_miniuart(void) __attribute__((weak));   /* psmac_bus.c: the log leaves the PL011 */

#ifdef PSMAC
#include "../alice_psmac.h"
#define PERI      ALICE_PERI
#else
#define PERI      0xf2000000u
#endif
#define GPIO      (PERI + 0x200000u)
#define UART0     (PERI + 0x201000u)
#define U_DR      0x00
#define U_FR      0x18
#define U_IBRD    0x24
#define U_FBRD    0x28
#define U_LCRH    0x2c
#define U_CR      0x30
#define U_IMSC    0x38
#define U_ICR     0x44
#define FR_RXFE   (1u << 4)
#define FR_TXFF   (1u << 5)
#define FR_BUSY   (1u << 3)
#define FR_CTS    (1u << 0)                   /* the chip lets us send */
#define FR_TXFE   (1u << 7)
#define CR_FLOW   (1u | (1u << 8) | (1u << 9) | (1u << 14) | (1u << 15))   /* on, TX, RX, RTS and CTS flow control */
#define CR_NOFLOW (1u | (1u << 8) | (1u << 9))                             /* on, TX, RX */
/* Which boards wire the chip's CTS/RTS to GPIO 30/31: the Pi 4 family and the Zero W / Zero 2 W.
   The 3B, 3B+ and 3A+ have TX and RX only (their device trees: uart0_pins <32 33>, and Raspberry Pi
   OS's btuart runs them "noflow" at 921600). With CTS flow control on and nothing driving CTS, the
   PL011 never sends: the first 3B try failed with "the chip does not answer (command 0c03)". */
static uint32_t CR_ON = CR_FLOW;
#define CR_CTSEN  (1u << 15)

static inline uint32_t rd(uintptr_t a) { return __builtin_bswap32(*(volatile uint32_t *)(uintptr_t)a); }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = __builtin_bswap32(v); }
static inline uint64_t ticks(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t tick_hz(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static uint32_t ms_now(void) { return (uint32_t)(ticks() / (tick_hz() / 1000u)); }

static struct bt_host bh;
static uint32_t uart_hz = 48000000u;
static int phase;                              /* 0 not started, 1 waiting for firmware, 2 BT_ON low, 3 BT_ON high, 4 running, 7 power off for a retry, 8 failed, 9 off */
static uint32_t phase_t, wait_since;
static char shown[64];
static int shown_col = -1;
static int keys_loaded, uart_done, retried, fail_logged;
static uint32_t rx_total, rx_errors;

static void blog(const char *s)
{
    kprintf("%s\n", s);
    unsigned n = 0;
    while (s[n]) n++;
    if (n > 5 && s[0] == '[') alice_net_ring(s, n), alice_net_ring("\n", 1);
}

/* ---- the UART ------------------------------------------------------------------- */
static void uart_baud(uint32_t baud)
{
    uint64_t t = ticks();
    while ((rd(UART0 + U_FR) & FR_BUSY) && ticks() - t < tick_hz() / 50) ;
    uint32_t cr = rd(UART0 + U_CR);
    wr(UART0 + U_CR, 0);
    uint32_t div = (uint32_t)(((uint64_t)uart_hz * 4u + baud / 2) / baud);       /* 64ths of the divisor */
    wr(UART0 + U_IBRD, div >> 6);
    wr(UART0 + U_FBRD, div & 63u);
    wr(UART0 + U_LCRH, 0x60u | 0x10u);                                          /* 8 bits, FIFOs on */
    wr(UART0 + U_CR, cr ? cr : CR_ON);
}
/* bytes still in the PL011's FIFO (held there while the chip keeps CTS off) are thrown away:
   they would reach the chip late, as a stale command, once it lets us send again */
static void uart_flush(uint32_t cr)
{
    wr(UART0 + U_CR, 0);
    uint32_t l = rd(UART0 + U_LCRH);
    wr(UART0 + U_LCRH, l & ~0x10u);                     /* FIFOs off: emptied */
    wr(UART0 + U_LCRH, l | 0x10u);
    wr(UART0 + U_ICR, 0x7ffu);
    wr(UART0 + U_CR, cr);
}
/* ---- BT_ON: where it is wired differs by board, like the WiFi's WL_ON (alice_net.c) ----
   The 3B, 3B+, 3A+, 4, 400 and CM4 have it on line 0 of the firmware's GPIO expander (mailbox
   SET_GPIO_STATE, GPIO 128+0); the Zero W has it on SoC GPIO 45, the Zero 2 W on GPIO 42
   (their device trees). Returns 0 when done, else "again later" (the mailbox was busy). */
static int bt_on(int on)
{
    int t = alice_pi_board_type(), pin = t == 0x0c ? 45 : t == 0x12 ? 42 : 0;
    if (!pin) return alice_mbox(0x00038041u, 128u + 0u, on ? 1u : 0u, 0);
    extern spinlock_t psmac_gpfsel_lock;
    spinlock_acquire(&psmac_gpfsel_lock);
    uint32_t f = rd(GPIO + 0x10);                                       /* GPFSEL4: GPIO 40-49 */
    wr(GPIO + 0x10, (f & ~(7u << (3 * (pin - 40)))) | (1u << (3 * (pin - 40))));
    spinlock_release(&psmac_gpfsel_lock);
    wr(GPIO + (on ? 0x20 : 0x2c), 1u << (pin - 32));                    /* GPSET1 / GPCLR1 */
    return 0;
}
static void uart_init(void)
{
    if (psmac_console_miniuart) psmac_console_miniuart();    /* one PL011 on a Pi 3: the log leaves it first */
    int t = alice_pi_board_type();
    int flow = alice_pi_is_2711() || t == 0x0c || t == 0x12;
    CR_ON = flow ? CR_FLOW : CR_NOFLOW;
    blog(flow ? "[BT] UART: TX, RX, CTS and RTS (GPIO 30-33)" : "[BT] UART: TX and RX only (GPIO 32, 33): this board has no CTS/RTS to the chip");
    extern spinlock_t psmac_gpfsel_lock;
    spinlock_acquire(&psmac_gpfsel_lock);
    uint32_t f = rd(GPIO + 0x0c);                       /* GPFSEL3: GPIO 30-33 (or 32-33) to ALT3 (UART0 CTS RTS TXD RXD) */
    if (flow) f = (f & ~0xfffu) | 07777u;
    else f = (f & ~(077u << 6)) | (077u << 6);
    wr(GPIO + 0x0c, f);
    uint32_t f1 = rd(GPIO + 0x04);                      /* GPIO 15 an input: UART0's RX comes from GPIO 33 only */
    wr(GPIO + 0x04, f1 & ~(7u << 15));
    spinlock_release(&psmac_gpfsel_lock);
    if (alice_pi_is_2711()) {
        uint32_t p1 = rd(GPIO + 0xe8), p2 = rd(GPIO + 0xec);    /* pulls (BCM2711): CTS and RXD up */
        p1 = (p1 & ~(0xfu << 28)) | (1u << 28);
        p2 = (p2 & ~0xfu) | (1u << 2);
        wr(GPIO + 0xe8, p1); wr(GPIO + 0xec, p2);
    } else {                                            /* BCM2835-37: GPPUD, then the pins clocked in */
        wr(GPIO + 0x94, 2u);
        for (volatile int i = 0; i < 300; i++) ;
        wr(GPIO + 0x98, flow ? 1u << 30 : 0); wr(GPIO + 0x9c, 1u << 1);   /* GPIO 30 (CTS, when wired), GPIO 33 (RXD) */
        for (volatile int i = 0; i < 300; i++) ;
        wr(GPIO + 0x94, 0); wr(GPIO + 0x98, 0); wr(GPIO + 0x9c, 0);
    }
    uint32_t hz = 0;
    if (alice_mbox(0x00030002u, 2u, 0u, &hz) == 0 && hz > 1000000u) uart_hz = hz;    /* GET_CLOCK_RATE: UART */
    wr(UART0 + U_CR, 0);
    wr(UART0 + U_IMSC, 0);
    wr(UART0 + U_ICR, 0x7ffu);
    uart_baud(115200);
    wr(UART0 + U_CR, CR_ON);
    while (!(rd(UART0 + U_FR) & FR_RXFE)) (void)rd(UART0 + U_DR);              /* whatever was there */
}
/* what goes to the chip waits in a ring and leaves as fast as the UART (and the chip's CTS)
   let it, from every turn of CPU1's loop: the chip may hold CTS for a while after a command,
   and a byte cut off mid-command loses the whole command */
static uint8_t txr[8192];
static unsigned tx_h, tx_t;
static uint32_t tx_moved, tx_warned, tx_sent;
static void tx_drain(void)
{
    while (tx_h != tx_t && !(rd(UART0 + U_FR) & FR_TXFF)) { wr(UART0 + U_DR, txr[tx_h % sizeof txr]); tx_h++; tx_sent++; tx_moved = ms_now(); }
    if (tx_h != tx_t && ms_now() - tx_moved > 2000 && !tx_warned) {
        tx_warned = 1;
        blog("[BT] the chip has held CTS for 2 s: it is not taking data");
    }
    if (tx_h == tx_t) tx_warned = 0;
}
static void io_send(void *c, const uint8_t *p, unsigned n)
{
    (void)c;
    if (tx_t - tx_h + n > sizeof txr) { blog("[BT] the send ring is full: a packet dropped"); return; }
    if (tx_h == tx_t) tx_moved = ms_now();
    for (unsigned i = 0; i < n; i++) txr[tx_t++ % sizeof txr] = p[i];
    tx_drain();
}
static void io_baud(void *c, uint32_t b) { (void)c; uart_baud(b); }
static uint32_t io_ms(void *c) { (void)c; return ms_now(); }
static void fr_log(const char *what);
/* the reset after the firmware's launch went unanswered */
static void io_kick(void *c, int attempt)
{
    (void)c;
    fr_log("[BT] reset unanswered:");
    tx_h = tx_t = 0;
    if (attempt == 1) { uart_flush(CR_ON); blog("[BT] UART FIFO emptied, the reset again"); }
    else { uart_flush(CR_ON & ~CR_CTSEN); blog("[BT] the reset again without CTS flow control"); }
}
static void io_log(void *c, const char *l) { (void)c; blog(l); }
static void io_kb(void *c, const uint8_t r[8]) { (void)c; if (frf_native_usb_keyboard_boot_report) frf_native_usb_keyboard_boot_report(r); }
static void io_mouse(void *c, int dx, int dy, int w, unsigned b)
{
    (void)c;
    aga_virtual_mouse_update(dx, dy, b & 7u);
    if (pt_mouse_extra) pt_mouse_extra(w, 0, b);
}
static void keys_log(void);
static void keys_save(void);
static void io_keys(void *c) { (void)c; keys_log(); keys_save(); }
static const struct bt_io bio = { 0, io_send, io_baud, io_ms, io_log, io_kb, io_mouse, io_keys, io_kick };

static unsigned put_num(char *t, unsigned k, uint32_t v, int hex);
static unsigned put_str(char *t, unsigned k, const char *s);
static const char hexd[] = "0123456789abcdef";
static unsigned put_hex(char *t, unsigned k, const uint8_t *p, int n, int rev)
{
    for (int i = 0; i < n; i++) { uint8_t b = p[rev ? n - 1 - i : i]; t[k++] = hexd[b >> 4]; t[k++] = hexd[b & 15]; }
    return k;
}
/* the Pi's own SD card (psmac_card.c, Macintosh build only): pairings.txt read at start, written on every change */
extern const char *psmac_card_pairings(int *state) __attribute__((weak));
extern void psmac_card_save_pairings(const char *text, uint32_t len) __attribute__((weak));
static int card_saves(void) { int st = -1; if (psmac_card_pairings) psmac_card_pairings(&st); return st == 1 && psmac_card_save_pairings; }
static unsigned key_word(char *t, unsigned n, const struct bt_key *k);
static void keys_save(void)
{
    if (!card_saves()) return;
    static char txt[4096]; unsigned n = 0;
    n = put_str(txt, 0, "# Bluetooth pairings, written by the Aurora68 kernel (one bt.dev= per line; delete a line to forget)\n");
    for (int i = 0; i < bh.nkeys && n < sizeof txt - 300; i++) {
        const struct bt_key *k = &bh.keys[i];
        txt[n++] = '#'; txt[n++] = ' '; for (int j = 0; k->name[j] && j < 39; j++) txt[n++] = (k->name[j] >= ' ' && k->name[j] < 127) ? k->name[j] : '?';
        txt[n++] = '\n';
        n = key_word(txt, n, k); txt[n++] = '\n';
    }
    psmac_card_save_pairings(txt, n);
}
/* every paired device as the cmdline.txt word that brings it back after a power cycle */
static void keys_log(void)
{
    int saved = card_saves();
    for (int i = 0; i < bh.nkeys; i++) {
        struct bt_key *k = &bh.keys[i];
        char t[240]; unsigned n = 0;
        n = put_str(t, 0, "[BT] paired: "); for (int j = 0; k->name[j] && j < 39; j++) t[n++] = k->name[j];
        n = put_str(t, n, saved ? " (kept in pairings.txt on the card): " : " -> add to cmdline.txt: ");
        n = key_word(t, n, k);
        t[n] = 0; blog(t);
    }
}
static unsigned key_word(char *t, unsigned n, const struct bt_key *k)
{
    {
        n = put_str(t, n, "bt.dev=");
        n = put_hex(t, n, k->addr, 6, 1); t[n++] = ':';
        n = put_hex(t, n, k->key, 16, 0); t[n++] = ':';
        unsigned kind = ((k->flags & BTK_KEYBOARD) ? 1u : 0u) | ((k->flags & BTK_MOUSE) ? 2u : 0u) | ((k->flags & BTK_LE) ? 4u : 0u);
        t[n++] = hexd[kind];
        if (k->flags & BTK_LE) {
            uint8_t ed[2] = { (uint8_t)(k->ediv >> 8), (uint8_t)k->ediv };
            t[n++] = ':'; n = put_hex(t, n, ed, 2, 0);
            t[n++] = ':'; n = put_hex(t, n, k->rand, 8, 0);
            t[n++] = ':'; t[n++] = (char)('0' + (k->addr_type & 1));
            if (k->has_irk) { t[n++] = ':'; n = put_hex(t, n, k->irk, 16, 0); }
        }
        return n;
    }
}

/* ---- cmdline.txt: bt=off, bt.dev=ADDR:KEY:KIND -----------------------------------------
   KIND: a hex digit, 1 keyboard + 2 mouse + 4 LE. An LE device goes on with
   :EDIV:RAND:T[:IRK] - EDIV 4 hex digits (the number), RAND 16 hex digits (its
   bytes in order), T the address type (0 public, 1 random), IRK 32 hex digits.
   KEY is then its LTK. Older kernels read the first three fields only. */
static int word(const char *s, const char *w)
{
    unsigned n = 0;
    while (w[n]) n++;
    for (const char *p = s; *p; p++) {
        if (p != s && p[-1] != ' ') continue;
        unsigned i = 0;
        while (i < n && p[i] == w[i]) i++;
        if (i == n && (!p[n] || p[n] == ' ' || p[n] == '\n')) return 1;
    }
    return 0;
}
int bt_pi_wanted(void) { return !word(alice_bootargs(), "bt=off"); }
/* bt.pair=1: pair with any keyboard or mouse a scan finds; bt.pair=NAME: only one whose name holds NAME.
   For a bench with no pointer yet: there is no way to click "pair" in PiBlue without a mouse. */
static int auto_pair; static char pair_name[32];
static void pair_option(void)
{
    const char *s = alice_bootargs();
    for (const char *p = s; *p; p++) {
        if ((p != s && p[-1] != ' ') || p[0] != 'b' || p[1] != 't' || p[2] != '.' || p[3] != 'p' || p[4] != 'a' || p[5] != 'i' || p[6] != 'r' || p[7] != '=') continue;
        const char *v = p + 8; unsigned n = 0;
        while (v[n] && v[n] != ' ' && n < 31) { pair_name[n] = v[n]; n++; }
        pair_name[n] = 0;
        auto_pair = n > 0 && !(n == 1 && pair_name[0] == '0');
        if (n == 1 && pair_name[0] == '1') pair_name[0] = 0;
    }
}
static int name_has(const char *name, const char *part)
{
    if (!part[0]) return 1;
    for (unsigned i = 0; name[i]; i++) {
        unsigned j = 0;
        while (part[j] && name[i + j] && ((name[i + j] | 32) == (part[j] | 32))) j++;
        if (!part[j]) return 1;
    }
    return 0;
}
static uint32_t pair_scan_at, pair_tried_at; static uint8_t pair_tried[6]; static int passkey_said;
static void auto_pair_poll(uint32_t now)
{
    if (!auto_pair || bh.state != BT_READY) return;
    if (bh.passkey_shown && !passkey_said) {
        passkey_said = 1;
        char t[80]; unsigned k = put_str(t, 0, "[BT] type this passkey on the keyboard, then Return: ");
        k = put_num(t, k, bh.passkey, 0); t[k] = 0; blog(t);
    }
    if (!bh.passkey_shown) passkey_said = 0;
    if (bh.pairing || bh.scanning) return;
    int connected = 0;
    for (int i = 0; i < bh.nkeys; i++) if (bh.keys[i].flags & BTK_CONNECTED) connected = 1;
    if (connected) return;
    /* a scan just ended: pair with the first keyboard or mouse it found (the name, when asked for) */
    for (int i = 0; i < bh.nfound; i++) {
        struct bt_found *f = &bh.found[i];
        if (!name_has(f->name, pair_name)) continue;
        int same = 1; for (int j = 0; j < 6; j++) if (f->addr[j] != pair_tried[j]) same = 0;
        if (same && now - pair_tried_at < 60000) continue;     /* not the same one again within a minute */
        for (int j = 0; j < 6; j++) pair_tried[j] = f->addr[j];
        pair_tried_at = now;
        char t[96]; unsigned k = put_str(t, 0, "[BT] bt.pair: pairing with \""); for (unsigned j = 0; f->name[j] && j < 39; j++) t[k++] = f->name[j]; k = put_str(t, k, "\""); t[k] = 0; blog(t);
        bt_pair(&bh, f->addr);
        return;
    }
    if (now - pair_scan_at > 20000) {                           /* nothing yet: look again every 20 s */
        pair_scan_at = now;
        bh.nfound = 0;
        blog("[BT] bt.pair: scanning for a keyboard or mouse in pairing mode");
        bt_scan(&bh);
    }
}
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void load_keys(const char *s)
{
    for (const char *p = s; *p; p++) {
        if ((p != s && p[-1] != ' ' && p[-1] != '\n' && p[-1] != '\r' && p[-1] != '\t') || p[0] != 'b' || p[1] != 't' || p[2] != '.' || p[3] != 'd' || p[4] != 'e' || p[5] != 'v' || p[6] != '=') continue;
        const char *q = p + 7;
        uint8_t a[6], k[16];
        int ok = 1;
        for (int i = 0; i < 6 && ok; i++) { int h = hexv(q[2 * i]), l = hexv(q[2 * i + 1]); if (h < 0 || l < 0) ok = 0; else a[5 - i] = (uint8_t)(h << 4 | l); }
        if (!ok || q[12] != ':') continue;
        q += 13;
        for (int i = 0; i < 16 && ok; i++) { int h = hexv(q[2 * i]), l = hexv(q[2 * i + 1]); if (h < 0 || l < 0) ok = 0; else k[i] = (uint8_t)(h << 4 | l); }
        if (!ok) continue;
        uint32_t kind = q[32] == ':' && hexv(q[33]) >= 0 ? (uint32_t)hexv(q[33]) : 0;
        if (kind & 4) {                                 /* LE: :EDIV:RAND:T[:IRK] */
            const uint8_t *r = (const uint8_t *)q + 34;
            uint8_t rand[8], irk[16]; uint32_t ediv = 0; int t = -1, has_irk = 0;
            if (r[0] != ':') continue;
            for (int i = 1; i <= 4 && ok; i++) { int v = hexv((char)r[i]); if (v < 0) ok = 0; else ediv = ediv << 4 | (uint32_t)v; }
            if (!ok || r[5] != ':') continue;
            for (int i = 0; i < 8 && ok; i++) { int h = hexv((char)r[6 + 2 * i]), l = hexv((char)r[7 + 2 * i]); if (h < 0 || l < 0) ok = 0; else rand[i] = (uint8_t)(h << 4 | l); }
            if (!ok || r[22] != ':') continue;
            t = hexv((char)r[23]);
            if (t < 0 || t > 1) continue;
            if (r[24] == ':') {
                has_irk = 1;
                for (int i = 0; i < 16 && has_irk; i++) { int h = hexv((char)r[25 + 2 * i]), l = hexv((char)r[26 + 2 * i]); if (h < 0 || l < 0) has_irk = 0; else irk[i] = (uint8_t)(h << 4 | l); }
            }
            bt_add_le_key(&bh, a, (uint8_t)t, k, (uint16_t)ediv, rand, has_irk ? irk : 0, kind << 1 & (BTK_KEYBOARD | BTK_MOUSE));
            continue;
        }
        bt_add_key(&bh, a, k, 4, kind << 1 & (BTK_KEYBOARD | BTK_MOUSE));
    }
}

/* ---- the board: what Alice68-Card sees and asks ---------------------------------------- */
static void board_sync(void)
{
    volatile uint8_t *b = alice_net_board();
    if (!b) return;
    volatile struct an_bt *x = (volatile struct an_bt *)(b + AN_OFF_BT);
    if (phase == 4 && x->cmd_seq != x->cmd_done) {
        uint8_t a[6];
        for (int i = 0; i < 6; i++) a[i] = x->cmd_addr[i];
        int r = -1;
        switch (x->cmd) {
        case AN_BT_CMD_SCAN:    r = bt_scan(&bh); break;
        case AN_BT_CMD_PAIR:    r = bt_pair(&bh, a); break;
        case AN_BT_CMD_FORGET:  r = bt_forget(&bh, a); break;
        case AN_BT_CMD_CONNECT: r = bt_connect(&bh, a); break;
        }
        x->cmd_result = (unsigned)r;
        __asm__ volatile("dmb ish" ::: "memory");
        x->cmd_done = x->cmd_seq;
    }
    x->state = phase == 9 ? AN_BT_OFF : phase == 8 ? AN_BT_FAILED : phase < 4 || phase == 7 ? (phase == 1 ? AN_BT_NOFW : AN_BT_STARTING) :
               bh.state == BT_READY ? AN_BT_READY : bh.state == BT_FAILED ? AN_BT_FAILED : AN_BT_STARTING;
    x->scanning = (unsigned)bh.scanning; x->pairing = (unsigned)bh.pairing;
    x->passkey = bh.passkey; x->passkey_shown = (unsigned)bh.passkey_shown;
    x->keys_gen = bh.keys_gen;
    for (int i = 0; i < 6; i++) x->bdaddr[i] = bh.bdaddr[i];
    for (unsigned i = 0; i < sizeof x->msg; i++) { x->msg[i] = bh.status[i]; if (!bh.status[i]) break; }
    x->nkeys = (unsigned)bh.nkeys;
    for (int i = 0; i < bh.nkeys && i < AN_BT_KEYS; i++) {
        volatile struct an_btkey *k = &x->keys[i];
        for (int j = 0; j < 6; j++) k->addr[j] = bh.keys[i].addr[j];
        for (int j = 0; j < 16; j++) k->key[j] = bh.keys[i].key[j];
        k->type = bh.keys[i].type; k->flags = bh.keys[i].flags;
        k->addr[6] = bh.keys[i].addr_type; k->addr[7] = bh.keys[i].has_irk;
        volatile struct an_btle *e = &x->le[i];
        for (int j = 0; j < 8; j++) e->rand[j] = bh.keys[i].rand[j];
        e->ediv = bh.keys[i].ediv;
        for (int j = 0; j < 16; j++) e->irk[j] = bh.keys[i].irk[j];
        for (int j = 0; j < 40; j++) { k->name[j] = bh.keys[i].name[j]; if (!bh.keys[i].name[j]) break; }
    }
    x->nfound = (unsigned)bh.nfound;
    for (int i = 0; i < bh.nfound && i < AN_BT_FOUND; i++) {
        volatile struct an_btfound *f = &x->found[i];
        for (int j = 0; j < 6; j++) f->addr[j] = bh.found[i].addr[j];
        f->cod = bh.found[i].cod | (bh.found[i].le ? 0x80000000u : 0); f->rssi = bh.found[i].rssi;
        for (int j = 0; j < 40; j++) { f->name[j] = bh.found[i].name[j]; if (!bh.found[i].name[j]) break; }
    }
}

static void post(const char *s, int col)
{
    int same = col == shown_col;
    for (unsigned i = 0; same && i < sizeof shown; i++) { if (shown[i] != s[i]) same = 0; if (!s[i]) break; }
    if (same) return;
    unsigned i = 0;
    for (; s[i] && i + 1 < sizeof shown; i++) shown[i] = s[i];
    shown[i] = 0;
    shown_col = col;
    frf_post_bt(shown, col);
}

static unsigned put_num(char *t, unsigned k, uint32_t v, int hex)
{
    char d[12]; int n = 0;
    do { d[n++] = "0123456789abcdef"[v % (hex ? 16u : 10u)]; v /= hex ? 16u : 10u; } while (v && n < 11);
    while (n) t[k++] = d[--n];
    return k;
}
static unsigned put_str(char *t, unsigned k, const char *s) { while (*s) t[k++] = *s++; return k; }
static int cts_last = -1, cts_changes;
static uint32_t t_init;
static void cts_note(const char *what)
{
    if (!(CR_ON & CR_CTSEN)) return;
    char t[48]; unsigned k = put_str(t, 0, what);
    k = put_str(t, k, (rd(UART0 + U_FR) & FR_CTS) ? "on" : "OFF"); t[k] = 0; blog(t);
}
/* the UART in two short lines (Alice68-Card's list shows about 70 characters) */
static void fr_log(const char *what)
{
    char t[96]; unsigned k = 0;
    uint32_t fr = rd(UART0 + U_FR), cr = rd(UART0 + U_CR);
    k = put_str(t, k, what);
    if (CR_ON & CR_CTSEN) { k = put_str(t, k, " CTS "); k = put_str(t, k, fr & FR_CTS ? "on" : "OFF (chip not taking data)"); }
    else k = put_str(t, k, " no flow control");
    k = put_str(t, k, ", FIFO "); k = put_str(t, k, fr & FR_TXFE ? "empty" : "NOT EMPTY");
    k = put_str(t, k, fr & FR_BUSY ? ", busy" : "");
    t[k] = 0; blog(t);
    k = put_str(t, 0, "[BT] UART: in ");
    k = put_num(t, k, rx_total, 0); k = put_str(t, k, " (bad "); k = put_num(t, k, rx_errors, 0);
    k = put_str(t, k, "), out "); k = put_num(t, k, tx_sent, 0); k = put_str(t, k, " (queued "); k = put_num(t, k, tx_t - tx_h, 0);
    k = put_str(t, k, "), FR "); k = put_num(t, k, fr, 1); k = put_str(t, k, " CR "); k = put_num(t, k, cr, 1);
    k = put_str(t, k, ", "); k = put_num(t, k, uart_hz / 1000u, 0); k = put_str(t, k, " kHz");
    t[k] = 0; blog(t);
}

/* ---- CPU1's turn ------------------------------------------------------------------- */
/* one line for the kernel's heartbeat (psmac_bus.c): where Bluetooth stands, for a screen without scrollback */
void bt_pi_report(void)
{
    static const char *const ph[] = { "not started", "waiting for firmware", "powering", "powering", "running", "?", "?", "restarting", "FAILED", "off" };
    char t[320]; unsigned k = put_str(t, 0, "[BT]   ");
    k = put_str(t, k, phase >= 0 && phase <= 9 ? ph[phase] : "?");
    if (phase == 4) {
        k = put_str(t, k, bh.state == BT_READY ? " ready" : bh.state == BT_FAILED ? " FAILED" : " starting");
        k = put_str(t, k, ": "); for (unsigned i = 0; bh.status[i] && k < 150; i++) t[k++] = bh.status[i];
        k = put_str(t, k, " | paired "); k = put_num(t, k, (uint32_t)bh.nkeys, 0);
        k = put_str(t, k, " found "); k = put_num(t, k, (uint32_t)bh.nfound, 0);
        if (bh.scanning) k = put_str(t, k, " scanning");
        if (bh.pairing) k = put_str(t, k, " pairing");
        if (bh.passkey_shown) { k = put_str(t, k, " passkey "); k = put_num(t, k, bh.passkey, 0); }
        for (int i = 0; i < bh.nkeys; i++) if (bh.keys[i].flags & BTK_CONNECTED) { k = put_str(t, k, " | connected: "); for (unsigned j = 0; bh.keys[i].name[j] && k < 190; j++) t[k++] = bh.keys[i].name[j]; }
        if (auto_pair) k = put_str(t, k, " | bt.pair on");
    }
    t[k] = 0; kprintf("%s\n", t);
    if (phase == 4) for (int i = 0; i < BT_MAXCONN; i++) {      /* what each LE device presented */
        struct bt_conn *c = &bh.conn[i];
        if (!c->used || !c->le) continue;
        struct bt_le *l = &c->l;
        static const char *const gn[] = { "idle", "mtu", "svc", "chr", "desc", "refs", "map", "cccd", "up", "fail" };
        k = put_str(t, 0, "[BT]   LE: stage "); k = put_str(t, k, l->g < 10 ? gn[l->g] : "?");
        k = put_str(t, k, " svc "); k = put_num(t, k, l->svci + 1u, 0); k = put_str(t, k, "/"); k = put_num(t, k, l->nsvc, 0);
        k = put_str(t, k, " chars"); for (int j = 0; j < l->nchr && k < 60; j++) { t[k++] = ' '; k = put_num(t, k, l->chr_uuid[j], 1); if (l->chr_uuid[j] == 0x2a4d) { t[k++] = '/'; k = put_num(t, k, l->rep_id[j], 0); t[k++] = '/'; k = put_num(t, k, l->rep_type[j], 0); } if (l->chr_cccd[j]) t[k++] = '*'; }
        k = put_str(t, k, " | map "); k = put_num(t, k, l->map_len, 0);
        k = put_str(t, k, l->mouse_ok ? " mouse" : ""); k = put_str(t, k, l->kbm.ok ? " kbd" : "");
        k = put_str(t, k, l->boot ? " boot" : " report"); k = put_str(t, k, " pm "); k = put_num(t, k, l->pm, 1);
        k = put_str(t, k, " | err "); k = put_num(t, k, l->att_err, 1); t[k++] = '@'; k = put_str(t, k, l->att_err_g < 10 ? gn[l->att_err_g] : "?");
        k = put_str(t, k, " | reports "); k = put_num(t, k, l->nnotify, 0);
        k = put_str(t, k, " | since up: frames "); k = put_num(t, k, l->up_frames, 0);
        k = put_str(t, k, " att "); k = put_num(t, k, l->up_att, 0); k = put_str(t, k, " last op "); k = put_num(t, k, l->up_last_op, 1);
        k = put_str(t, k, " other "); k = put_num(t, k, l->up_other, 0); k = put_str(t, k, " cid "); k = put_num(t, k, l->up_last_cid, 1);
        t[k] = 0; kprintf("%s\n", t);
        k = put_str(t, 0, "[BT]   map:"); for (unsigned j = 0; j < sizeof l->map_head && j < l->map_len; j++) { t[k++] = ' '; t[k++] = hexd[l->map_head[j] >> 4]; t[k++] = hexd[l->map_head[j] & 15]; }
        t[k] = 0; kprintf("%s\n", t);
    }
    if (phase == 4 && bh.nkeys) keys_log();             /* the bt.dev= words at every heartbeat: one photo whenever it suits */
}
void bt_pi_poll(void)
{
#ifdef PSMAC
    { extern int psmac_bt_testbed_poll(void); if (psmac_bt_testbed_poll()) return; }   /* alice_psmac.c: mac.btsim= owns the board */
#endif
#ifdef PSMAC
    /* One SD card for a Pi 3B and a Pi 4 (19 November): the container may hold the .hcd of both
       chips, so the board says which to take first, else a 3B's BCM43438 got the 4's BCM4345C0 */
    static const char *const s_3b[] = { "BCM43430A1", "BCM4343A1", 0 };
    static const char *const s_z2w[] = { "BCM43430B0", "BCM43430A1", 0 };
    static const char *const s_4345[] = { "BCM4345C0", "bcm4345c0", "BCM4345C5", 0 };
    static const char *const s_4345c5[] = { "BCM4345C5", "BCM4345C0", "bcm4345c0", 0 };
    extern int psmac_board_bt_chip(void);           /* psmac_usbnet.c, from the device tree's model */
    static const char *const *stems_by_board;
    if (!stems_by_board) {
        int c = psmac_board_bt_chip();
        stems_by_board = c == 1 ? s_3b : c == 2 ? s_z2w : c == 3 ? s_4345c5 : s_4345;
    }
    const char *const *stems = stems_by_board;
#else
    static const char *const stems[] = { "BCM4345C0", "BCM4345C5", "bcm4345c0", "BCM43430A1", "BCM4343A1", 0 };
#endif
    uint32_t now = ms_now();
    if (!keys_loaded) {
        /* cmdline.txt's bt.dev= words, then pairings.txt from the card (newer: it wins); the card is
           read on core 3 at start, a few hundred milliseconds - wait for it, at most ten seconds */
        static uint32_t card_wait;
        int st = -1; const char *txt = psmac_card_pairings ? psmac_card_pairings(&st) : 0;
        if (!card_wait) card_wait = now ? now : 1;
        if (st == 0 && now - card_wait < 10000) return;
        keys_loaded = 1;
        load_keys(alice_bootargs());
        if (txt && txt[0]) { int before = bh.nkeys; load_keys(txt); char t[64]; unsigned k = put_str(t, 0, "[BT] pairings.txt on the card: "); k = put_num(t, k, (uint32_t)bh.nkeys - (uint32_t)before, 0); k = put_str(t, k, " more device(s)"); t[k] = 0; blog(t); }
        pair_option();
    }
    switch (phase) {
    case 0:
        if (!bt_pi_wanted()) { phase = 9; post("off (bt=off)", 0); blog("[BT] off: bt=off in cmdline.txt"); break; }
        if (!alice_net_board()) return;                 /* AmigaOS is not up yet */
        phase = 1; wait_since = now;
        post("waiting for its firmware", 0);
        /* fall through */
    case 1: {
        uint32_t len = 0;
        const uint8_t *fw = 0;
        int which = -1;
        for (int i = 0; stems[i] && !fw; i++) { const char *one[2] = { stems[i], 0 }; fw = alice_fw_get(one, ".hcd", &len); if (fw) which = i; }
        if (!fw && !(alice_fw_offered() && now - wait_since > 3000) && now - wait_since < 30000) break;
        if (!fw) blog("[BT] no BCM4345C0.hcd in the WIFI folder: the chip's own (older) firmware is used");
        else { char t[64]; unsigned k = put_str(t, 0, "[BT] firmware from the WIFI folder: "); k = put_str(t, k, stems[which]); k = put_str(t, k, ".hcd"); t[k] = 0; blog(t); }
        bh.fw = fw; bh.fwlen = fw ? len : 0;
        if (!uart_done) { uart_init(); uart_done = 1; }
        if ((int32_t)(now - phase_t) < 250) break;      /* a mailbox try at most 4 times a second */
        phase_t = now;
        if (bt_on(0)) {                                             /* BT_ON low first; the mailbox may be busy */
            if (now - wait_since > 40000) { phase = 8; post("FAILED: BT_ON could not be set", 2); blog("[BT] FAIL: the mailbox would not set BT_ON"); }
            break;
        }
        phase = 2; phase_t = now;
        post("starting", 0);
        break;
    }
    case 2:
        if ((int32_t)(now - phase_t) < 20) break;
        cts_note("[BT] chip off: CTS ");
        if (bt_on(1)) { phase_t = now + 230; break; }               /* BT_ON high; again in 250 ms */
        phase = 3; phase_t = now;
        break;
    case 3:
        if ((int32_t)(now - phase_t) < 250) break;      /* the chip boots its ROM */
        cts_note("[BT] chip on: CTS ");
        uart_flush(CR_ON);                              /* nothing stale from an earlier try */
        cts_last = -1; cts_changes = 0; t_init = now;
        while (!(rd(UART0 + U_FR) & FR_RXFE)) (void)rd(UART0 + U_DR);
        phase = 4;
        tx_h = tx_t = 0;
        bt_init(&bh, &bio, bh.fw, bh.fwlen);
        break;
    case 7:                                             /* BT_ON low again, for a fresh start */
        if ((int32_t)(now - phase_t) < 0) break;
        if (bt_on(0)) { phase_t = now + 250; break; }
        phase = 2; phase_t = now + 300;                 /* off long enough for the chip to really reset */
        break;
    case 4: {
        tx_drain();
        {
            int c = (CR_ON & CR_CTSEN) ? ((rd(UART0 + U_FR) & FR_CTS) ? 1 : 0) : 1;
            if (c != cts_last && cts_changes < 24) {        /* when the chip lets us send and when not: the firmware's launch shows here */
                char t[64]; unsigned k = put_str(t, 0, "[BT] CTS ");
                k = put_str(t, k, c ? "on" : "OFF"); k = put_str(t, k, " at +"); k = put_num(t, k, now - t_init, 0);
                k = put_str(t, k, " ms, step "); k = put_num(t, k, (uint32_t)bh.step, 0); t[k] = 0;
                if (cts_last >= 0) blog(t);
                cts_changes++;
            }
            cts_last = c;
        }
        if (bh.state == BT_FAILED && bh.fw && !retried) {  /* the firmware file did not go in: the chip's own, once */
            retried = 1;
            fail_logged = 0;
            blog("[BT] the firmware file did not start: trying again with the chip's own firmware");
            bh.fw = 0; bh.fwlen = 0;
            phase = 7; phase_t = now;
            post("starting again (without the firmware file)", 0);
            break;
        }
        uint8_t buf[64];
        for (int rounds = 0; rounds < 8; rounds++) {
            unsigned n = 0;
            while (n < sizeof buf && !(rd(UART0 + U_FR) & FR_RXFE)) {
                uint32_t v = rd(UART0 + U_DR);
                if (v & 0xf00u) rx_errors++;               /* framing, parity, break, overrun */
                buf[n++] = (uint8_t)v;
            }
            rx_total += n;
            if (!n) break;
            bt_rx(&bh, buf, n);
        }
        bt_poll(&bh);
        auto_pair_poll(now);
        tx_drain();
        int col = bh.state == BT_READY ? 1 : bh.state == BT_FAILED ? 2 : 0;
        if (bh.state == BT_FAILED && !fail_logged) {
            fail_logged = 1;
            fr_log("[BT] UART:");
        }
        if (bh.state == BT_FAILED) { char t[64] = "FAILED: "; unsigned k = 8; for (unsigned i = 0; bh.why[i] && k < 63; i++) t[k++] = bh.why[i]; t[k] = 0; post(t, 2); }
        else post(bh.status[0] ? bh.status : "starting", col);
        break;
    }
    default:
        break;
    }
    board_sync();
}
