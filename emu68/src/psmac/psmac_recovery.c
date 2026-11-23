/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_recovery.c - Aurora68 Setup and Recovery: the kernel as a small file server for its own SD card, with the
 * Mac left switched off.
 *
 * Backstory (22 November 2026, docs/08 item 106). Inside an SE or a Plus the Pi sits in the 68000 socket
 * under the logic board, and its microSD card cannot be reached without opening the Mac. Until now a
 * kernel8.img that would not start the Mac, a cmdline.txt with a typing mistake in it or a disk image the
 * Mac could not boot from meant opening the case. The owner approved a design in which the kernel itself
 * can be the way back: a start in which the 68k is never released, no Mac code runs, and the Pi offers its
 * card over the network (a page that works on a phone, and `aurora68.py recovery`), shows on the Mac's own
 * screen and on HDMI where to find it, and takes a keyboard for the settings that get it onto the network.
 *
 * Getting in, any of:
 *   - the boot counter: boot.txt in the card's root counts the starts in a row that did not reach the
 *     Finder (psmac_card.c; incremented before the 68k starts, back to 0 when CurApName says Finder). At
 *     mac.bootcount= (3 by default; off for none) the next start is a recovery start. A start the Pi was
 *     switched off in before the Finder counts too: there is no telling it from one that hung, and the
 *     price is a press of "restart normally". A deliberate restart from the Finder never counts, since
 *     the Finder was reached before it;
 *   - the programmer's interrupt switch held as the 68k would be released (SE, Plus, 512K: the raw level's
 *     bit 2 on the wire, psmac_board_se.c), or Esc held for a second on a USB keyboard in the first twenty
 *     seconds (the keyboard is not up before the 68k starts, so Esc writes recovery.txt and restarts the Pi);
 *   - recovery.txt in the card's root (taken away on entry): the remote control's POST
 *     /restart?how=recovery and the recovery page's own "restart into recovery" write it;
 *   - mac.recovery=1 in cmdline.txt ("restart normally" takes it out again).
 * In recovery: the network (the wired port or a USB Ethernet adapter by DHCP, a link-local address when
 * no DHCP server answers; the WiFi with the wifi.ssid/psk/country options, on an address of its own), mDNS
 * as aurora68.local (remote.name= for another name), HTTP on port 80 (and 8068). Every request but the page
 * itself needs the four-digit PIN shown on the screens (a new one at every recovery start; after ten wrong
 * ones a new PIN and a minute's wait); requests from outside the local network are refused, by the remote
 * control's rule. The PIN is drawn on the screens only: the log says "PIN shown on screen".
 *
 * Safe kernels: an uploaded kernel8.img goes to kernel8.new, is read back from the card and checked (gzip:
 * header, deflate stream, CRC32 and length, by libdeflate; then the ARM64 image header's "ARM\x64" at offset
 * 56 and the "[BOOT] Build ID" and "[PSMAC]" texts every Aurora68 kernel prints), and only then becomes
 * kernel8.img, the previous one kept as kernel8.old. The order of the card writes that keeps a kernel8.img
 * on the card at every instant is psmac_card.c's ("the card's root as a file manager sees it").
 *
 * What this cannot do: rescue a kernel that dies before it has read the card. The Pi 3's firmware has no
 * fallback kernel of its own that we know of, so a microSD extension cable led out of the Mac is the
 * physical way back for that case (README, "Recovery").
 *
 * The HDMI monitor (22 November 2026, docs/08 item 111): the page's Display tab shows the monitor's EDID as
 * psmac_hdmi.c read it at this start, judged by psmac_edid.c, and writes aurora68-display.txt as Aurora68 Display
 * does (with the include line config.txt needs), so that a monitor which shows nothing can be put right without
 * the Mac.
 *
 * The access point (23 November 2026, docs/08 item 118; the owner: "is there a way we can trigger the Pi going into
 * an AP mode so we can connect to it for initial setup or later rescue ... This would not be the default but would
 * need to use the programmer's buttons"). On the recovery screen, the programmer's interrupt button held for two
 * seconds (after it has been let go: the press that brought recovery on does not count) makes the Pi's WiFi an
 * access point of its own, "Aurora68-XXXX" (XXXX from the radio's MAC address), WPA2 with a password made at
 * each start and shown on the screens (open when the firmware cannot be the authenticator: the PIN still guards
 * the page), 192.168.4.1/24 with a DHCP server and every DNS name answered with that address (psmac_ip.c,
 * psmac_apnet.c), the page at http://192.168.4.1/ as on any network. The keyboard menu has it too, and
 * recovery.ap=on in cmdline.txt starts a recovery start in it (the bench's way). It is never kept: "Restart
 * normally" takes recovery.ap out again, and the reset button, which restarts the Pi from the recovery screen,
 * starts it as cmdline.txt says. The firmware's side (alice_net.c, wifi/fmac_ap.c) could only be tried against
 * the testbed's simulated radio; every refusal of a real chip is named on the screen.
 *
 * Diagnostics (23 November 2026, docs/08 item 122; the owner: "do we have all the debug options in there for Mac,
 * Mac Plus etc. Can we trigger a debug run directly in there for this and other platforms? With a report in the
 * Web GUI?"). The Options tab offers every option a user or tester sets, grouped and filtered by the board, from
 * psmac_diag.c's table (which is also the validator). The Diagnostics tab runs, while no Mac runs: the bus check on
 * the compact Macs (psmac_buscheck.c, on the service core, which owns the transport; its lines come to the page as
 * they are made, and the report holds the Mac's screen and HDMI until the page or a key gives them back), the card's
 * read test (here, on core 3) and a fresh read of the CPLD's JTAG usercode. It arms a one-shot debug start:
 * debug-once.txt in the card's root, one line of debug options that the next start merges into its boot arguments
 * before anything reads them (psmac_recovery_once_early) and at once renames debug-once.old, so it happens once
 * whatever that start does. And it lists the reports on the card and makes one bundle of them with the machine's
 * summary and the logs, every secret "(hidden)" (psmac_diag.c's pdg_scrub).
 *
 * Everything runs on core 3 inside the service loop, a bounded step per turn; core 0 decides (phase 1 and
 * 2) and then waits for good.
 */
#include <stdint.h>
#include <stdarg.h>
#include "support.h"
#include "tlsf.h"
#include "libdeflate.h"
#include "psmac.h"
#include "psmb.h"
#include "../alice/alice_net.h"
#include "psmac_ip.h"
#include "psmac_cmdline.h"
#include "psmac_recovery.h"
#include "psmac_edid.h"
#include "psmac_diag.h"
#include "version.h"
#include "devicetree.h"

const char *psmac_option(const char *key);
extern int psmac_board_se, psmac_board_solo, psmac_board_plus;
extern volatile uint8_t *alice_net_board(void);
int psmac_se_switch_held(void);
int psmac_gpio_reset_asserted(void);
void fm_sha1(const uint8_t *p, unsigned n, uint8_t out[20]);    /* alice/wifi/fmac.c */
int psmac_remote_private_source(const uint8_t *ip);
uint8_t *psmac_mac_ram_ptr(void);
uint32_t psmac_log_copy(uint8_t *out, uint32_t max);
int psmac_card_idle(void);
void psmac_display_recovery(void);
int psmac_display_vc(uint8_t **fb, uint32_t *pitch, uint32_t *w, uint32_t *h, int *bpp, int *swap);
void psmac_se_mark(const uint8_t *frame, uint32_t width, uint32_t height, uint32_t rowbytes, uint32_t depth);
extern const uint8_t topaz8_chardata[]; extern const uint32_t topaz8_charloc[]; extern const int modulo;

static inline uint64_t now_ticks(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t tick_hz(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static uint32_t now_ms(void) { return (uint32_t)(now_ticks() / (tick_hz() / 1000u)); }
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static int ieq(const char *a, const char *b) { while (*a && *b) if (lower(*a++) != lower(*b++)) return 0; return !*a && !*b; }
static void scpy(char *d, const char *s, unsigned cap) { unsigned i = 0; for (; s[i] && i + 1 < cap; i++) d[i] = s[i]; if (cap) d[i] = 0; }

/* a bounded printf into a buffer (support.c's sprintf has no limit) */
struct sb { char *s; unsigned n, cap; };
static void sb_putc(void *d, char c) { struct sb *b = d; if (b->n + 1 < b->cap) b->s[b->n++] = c; }
static void bprintf(struct sb *b, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vkprintf_pc(sb_putc, b, fmt, ap); va_end(ap);
    if (b->cap) b->s[b->n] = 0;
}
static void bput(struct sb *b, const char *s) { while (*s) sb_putc(b, *s++); if (b->cap) b->s[b->n] = 0; }
static void bjson(struct sb *b, const char *s)                 /* a JSON string, quotes included */
{
    sb_putc(b, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { sb_putc(b, '\\'); sb_putc(b, (char)c); }
        else if (c == '\n' || c == '\r' || c == '\t') { sb_putc(b, '\\'); sb_putc(b, c == '\n' ? 'n' : c == '\r' ? 'r' : 't'); }
        else if (c < 32 || c > 126) sb_putc(b, '?');
        else sb_putc(b, (char)c);
    }
    sb_putc(b, '"');
    if (b->cap) b->s[b->n] = 0;
}

/* ===================================================================================== the mode */
enum { WHY_NONE, WHY_OPTION, WHY_REQUEST, WHY_COUNTER, WHY_SWITCH };
static volatile int rec_on;
static int why, boot_count = -1, boot_limit = 3;
static char why_text[160];
int psmac_recovery_on(void) { return __atomic_load_n(&rec_on, __ATOMIC_ACQUIRE); }

/* the access point (docs/08 item 118): its state; the work is below ("the access point") */
enum { AP_OFF, AP_WAITING, AP_STARTING, AP_UP, AP_FAILED };
static int ap_state, ap_open, ap_channel = 6, ap_flags;
static char ap_how[96], ap_ssid[36], ap_pass[16], ap_err[100], ap_asked[96];
static uint32_t ap_t0, ap_seq;
static const uint8_t ap_ip[4] = { 192, 168, 4, 1 };
static int ap_active(void) { return ap_state == AP_WAITING || ap_state == AP_STARTING || ap_state == AP_UP; }

/* Diagnostics (docs/08 item 122, below): who has the screens, and the one-shot debug start this start took */
#define BC_PAGE_ROWS 46
static int diag_screen;                       /* 0 the recovery page; 1 a bus check runs (the compact Mac's screen is the check's); 2 its report on both screens */
static void diag_screen_poll(void), diag_lines(void);
static const char *board_model(void);
struct sb; static void j_once(struct sb *b);
struct bun; static void bt_bundle(struct bun *bb);
int psmac_buscheck_request_state(void);
static char once_used[600]; static int once_taken;
static int once_armed;                         /* debug-once.txt is on the card (for the screen; the page reads the card) */

static void enter(int w, const char *text)
{
    why = w;
    scpy(why_text, text, sizeof why_text);
    psmac_display_recovery();                                    /* the HDMI console stays off the page from now on */
    __atomic_store_n(&rec_on, 1, __ATOMIC_RELEASE);
    kprintf("[RECOVERY] Aurora68 Setup and Recovery: %s. The 68k is not started; the card is served on the network (docs/08 item 106)\n", why_text);
    psmac_boot_mark("recovery start: %s", why_text);
}

/* core 0, in psmac_setup_memory before the mailbox and core 3: the option, the request file, the counter.
   The card is there to be read when the Pi booted from a container (on hardware it always does) or with
   mac.cardearly=1 (the testbed); otherwise only the option is seen. */
void psmac_recovery_phase1(void)
{
    const char *o = psmac_option("mac.bootcount");
    if (o) { if (o[0] == 'o') boot_limit = 0; else { int v = 0; while (*o >= '0' && *o <= '9') v = v * 10 + (*o++ - '0'); boot_limit = v; } }
    o = psmac_option("mac.recovery");
    int want_opt = o && o[0] == '1';
    o = psmac_option("recovery.ap");                             /* the bench's way to the access point (docs/08 item 118) */
    int want_ap = o && ((o[0] == 'o' && o[1] == 'n') || o[0] == '1');
    if (want_ap) scpy(ap_asked, "recovery.ap=on in cmdline.txt", sizeof ap_asked);
    char flag[120]; int flag_len = -1, count = -1;
    int card = psmac_card_boot_read(&count, flag, sizeof flag, &flag_len) == 0;
    if (!card) kprintf("[RECOVERY] the card was not read before the 68k starts (a bare-ROM testbed run: mac.cardearly=1): no boot counter, no recovery.txt\n");
    boot_count = count;
    if (want_opt) enter(WHY_OPTION, "mac.recovery=1 in cmdline.txt");
    else if (want_ap) enter(WHY_OPTION, "recovery.ap=on in cmdline.txt (an access point for a phone)");
    else if (flag_len >= 0) {
        char t[160]; struct sb b = { t, 0, sizeof t };
        if (flag_len > 0) bprintf(&b, "asked for: %s", flag); else bput(&b, "recovery.txt on the card");
        enter(WHY_REQUEST, t);
    } else if (boot_limit && count >= boot_limit) {
        char t[160]; struct sb b = { t, 0, sizeof t };
        bprintf(&b, "the last %d starts did not reach the Finder (the boot counter)", count);
        enter(WHY_COUNTER, t);
    }
    if (!card) return;
    if (flag_len >= 0) psmac_card_boot_flag_drop();
    if (rec_on) { if (count > 0) psmac_card_boot_write(0); }
    else if (boot_limit) {
        int n = count < 0 ? 1 : count + 1;
        psmac_card_boot_write(n);
        boot_count = n;                                          /* what the Finder puts back to 0 */
        kprintf("[RECOVERY] boot counter: this is start %d in a row without the Finder so far (recovery after %d: mac.bootcount=)\n", n, boot_limit);
    }
}
/* core 0, as the 68k would be released: the programmer's switch, held for a twentieth of a second */
void psmac_recovery_phase2(void)
{
    if (rec_on || !psmac_board_se || psmac_board_solo || !psmac_se_switch_held()) return;
    uint64_t t0 = now_ticks();
    while (now_ticks() - t0 < tick_hz() / 20) if (!psmac_se_switch_held()) return;
    enter(WHY_SWITCH, "the programmer's interrupt switch was held at power-on");
    psmac_card_boot_reset_later();                               /* phase 1 counted this start */
}
void __attribute__((noreturn)) psmac_recovery_park(void)
{
    kprintf("[RECOVERY] core 0 waits; core 3 serves the card\n");
    for (;;) __asm__ volatile("wfe");
}
/* the service core, when CurApName says Finder: the counter back to 0 */
void psmac_recovery_finder(void)
{
    if (rec_on || !boot_limit || boot_count == 0) return;
    boot_count = 0;
    psmac_card_boot_reset_later();
}

/* ---- a running system asks for recovery: recovery.txt, then the Pi restarts --------------------- */
#define PM_RSTC (PSMAC_PERIPH_HI + 0x10001cu)
#define PM_WDOG (PSMAC_PERIPH_HI + 0x100024u)
static uint32_t reset_at; static int ask_active; static uint32_t ask_t0;
static void pi_restart_in(uint32_t ms_) { reset_at = now_ms() + ms_; if (!reset_at) reset_at = 1; }
static void reset_poll(void)
{
    if (!reset_at || (int32_t)(now_ms() - reset_at) < 0) return;
    if (!psmac_card_idle() && now_ms() - reset_at < 10000) return;   /* the card's last writes first, up to 10 s */
    kprintf("[RECOVERY] the Pi restarts (watchdog)\n");
    wr32le(PM_WDOG, 0x5a000000u | 10u);
    wr32le(PM_RSTC, 0x5a000020u);
    reset_at = 0;
}
int psmac_recovery_ask(const char *why_)
{
    if (ask_active) return 0;
    if (psmac_card_ask_recovery(why_)) return -1;
    ask_active = 1; ask_t0 = now_ms();
    kprintf("[RECOVERY] a recovery start asked for (%s): recovery.txt, then the Pi restarts\n", why_);
    return 0;
}
static void ask_poll(void)
{
    if (ask_active != 1) return;
    int st = psmac_card_ask_state();
    if (st == 2 || st == 3 || now_ms() - ask_t0 > 15000) { ask_active = 2; pi_restart_in(800); }
}

/* ---- Esc held in the first seconds of a normal start --------------------------------------------- */
#define ESC_WINDOW_MS 20000u                 /* the kernel's first twenty seconds: USB is up in about two */
#define ESC_HOLD_MS   1000u                  /* held a second, so a tap that belongs to the Mac does not count */
static uint32_t esc_down_at; static uint32_t start_ms;

/* ==================================================================================== the PIN */
static char pin[8]; static uint32_t pin_wrong, pin_locked_until, pin_changes;
static uint32_t rng_word(void)
{
    /* the BCM2835's hardware generator (QEMU models it too), mixed with the free-running counters */
    uint64_t base = PSMAC_PERIPH_HI + 0x104000u;
    static int up;
    if (!up) { wr32le(base + 0x4, 0x40000u); wr32le(base + 0x0, 1u); up = 1; }
    uint32_t v = (uint32_t)now_ticks() * 2654435761u;
    for (int i = 0; i < 1000 && !(rd32le(base + 0x4) >> 24); i++) ;
    if (rd32le(base + 0x4) >> 24) v ^= rd32le(base + 0x8);
    uint32_t st; __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(st));
    return v ^ (st << 7) ^ (uint32_t)(now_ticks() >> 3);
}
static void new_pin(void)
{
    uint32_t v = rng_word() % 10000u;
    /* mac.recoverypin=NNNN, the testbed's: scripts cannot read a PIN off a screen. Only the first PIN of the
       start; one made after ten wrong ones is random again */
    static int first = 1;
    const char *o = first ? psmac_option("mac.recoverypin") : 0;
    first = 0;
    if (o && o[0] >= '0' && o[0] <= '9' && o[1] >= '0' && o[1] <= '9' && o[2] >= '0' && o[2] <= '9' && o[3] >= '0' && o[3] <= '9' && !o[4])
        v = (uint32_t)((o[0] - '0') * 1000 + (o[1] - '0') * 100 + (o[2] - '0') * 10 + (o[3] - '0'));
    pin[0] = (char)('0' + v / 1000); pin[1] = (char)('0' + v / 100 % 10); pin[2] = (char)('0' + v / 10 % 10); pin[3] = (char)('0' + v % 10); pin[4] = 0;
    pin_wrong = 0;
}
static int pin_ok(const char *got, unsigned n)
{
    unsigned diff = n ^ 4u;
    for (unsigned i = 0; i < 4; i++) diff |= (unsigned)(uint8_t)(i < n ? got[i] : 0) ^ (uint8_t)pin[i];
    return !diff;
}

/* ================================================================================ the screens */
#define SCOLS 80
#define SROWS 30
static char line[SROWS][2 * SCOLS + 1]; static uint8_t lattr[SROWS]; static int nlines;   /* attr 1 title, 2 large; up to two screen lines of 80 (layout wraps them) */
static int scr_dirty = 1; static uint32_t scr_last;
static uint8_t se_fb[342 * 64] __attribute__((aligned(16)));
static void L(int attr, const char *fmt, ...)
{
    if (nlines >= SROWS) return;
    struct sb b = { line[nlines], 0, 2 * SCOLS + 1 };
    va_list ap; va_start(ap, fmt); vkprintf_pc(sb_putc, &b, fmt, ap); va_end(ap);
    line[nlines][b.n] = 0;
    lattr[nlines] = (uint8_t)attr;
    nlines++;
    if (attr == 2 && nlines < SROWS) { line[nlines][0] = 0; lattr[nlines] = 3; nlines++; }   /* a large line takes two */
}
static const uint8_t *glyph(uint8_t c) { return &topaz8_chardata[(topaz8_charloc[(c >= 32 && c < 128) ? c - 32 : 0] >> 16) >> 3]; }
/* The lines wrapped to a screen's width (22 November 2026): the page is written at 80 columns, the compact
   Mac's screen has 64, and its longer lines (the reason, the card, the last action) ran off the right edge.
   A normal line breaks at the last space that fits, and its continuation is indented two columns; the
   title and the large PIN line are kept as they are (the PIN fits; the title is cut). */
#define VROWS 40
static char vline[VROWS][SCOLS + 1]; static uint8_t vattr[VROWS];
static int layout(unsigned cols)
{
    int nv = 0;
    if (cols > SCOLS) cols = SCOLS;
    for (int r = 0; r < nlines && nv < VROWS; r++) {
        const char *t = line[r]; unsigned n = slen(t), at = 0, indent = 0;
        if (lattr[r] != 0 || n <= cols) {
            unsigned k = 0; for (; k < n && k < SCOLS; k++) vline[nv][k] = t[k];
            vline[nv][k] = 0; vattr[nv++] = lattr[r];
            continue;
        }
        while (at < n && nv < VROWS) {
            unsigned room = cols - indent, take = n - at;
            if (take > room) {
                take = room;
                for (unsigned k = room; k > room / 2; k--) if (t[at + k] == ' ') { take = k; break; }
            }
            unsigned o = 0;
            for (unsigned k = 0; k < indent; k++) vline[nv][o++] = ' ';
            for (unsigned k = 0; k < take; k++) vline[nv][o++] = t[at + k];
            vline[nv][o] = 0; vattr[nv++] = 0;
            at += take;
            while (at < n && t[at] == ' ') at++;
            indent = 2;
        }
    }
    return nv;
}
/* one text cell: 8 x 16 pixels at (x, y), or 16 x 32 with big */
static void cell_vc(uint8_t *fb, uint32_t pitch, int bpp, int swap, uint32_t x, uint32_t y, uint8_t c, uint32_t fg, uint32_t bg, int big)
{
    const uint8_t *data = glyph(c);
    int s = big ? 2 : 1;
    if (swap && bpp == 32) { fg = (fg & 0xff00) | (fg >> 16 & 0xff) | (fg & 0xff) << 16; bg = (bg & 0xff00) | (bg >> 16 & 0xff) | (bg & 0xff) << 16; }
    uint16_t fg16 = (uint16_t)((fg >> 8 & 0xf800) | (fg >> 5 & 0x07e0) | (fg >> 3 & 0x1f)), bg16 = (uint16_t)((bg >> 8 & 0xf800) | (bg >> 5 & 0x07e0) | (bg >> 3 & 0x1f));
    for (uint32_t r = 0; r < 16; r++) {
        uint8_t bits = c >= 32 ? *data : 0;
        for (int sy = 0; sy < s; sy++) {
            uint8_t *row = fb + (y + r * (uint32_t)s + (uint32_t)sy) * pitch + x * (bpp == 32 ? 4u : 2u);
            for (uint32_t k = 0; k < 8u * (uint32_t)s; k++) {
                int on = bits & (0x80 >> (k / (uint32_t)s));
                if (bpp == 32) ((uint32_t *)row)[k] = LE32(on ? fg : bg); else ((uint16_t *)row)[k] = LE16(on ? fg16 : bg16);
            }
            arm_flush_cache((intptr_t)row, 8u * (uint32_t)s * (bpp == 32 ? 4u : 2u));
        }
        if (r & 1) data += modulo;
    }
}
static void draw_vc(void)
{
    uint8_t *fb; uint32_t pitch, w, h; int bpp, swap;
    if (psmac_display_vc(&fb, &pitch, &w, &h, &bpp, &swap)) return;
    uint32_t cols = w / 8, rows = h / 16;
    if (cols > SCOLS) cols = SCOLS;
    int nv = layout(cols);
    for (uint32_t r = 0; r < rows && r < VROWS; r++) {
        int a = r < (uint32_t)nv ? vattr[r] : 0;
        if (a == 3) continue;                                     /* the lower half of a large line */
        const char *t = r < (uint32_t)nv ? vline[r] : "";
        uint32_t fg = a == 1 ? 0x101030u : 0xe0e0e0u, bg = a == 1 ? 0x80c0ffu : 0x101830u;
        if (a == 2) { fg = 0xffe060u; }
        unsigned n = slen(t);
        for (uint32_t cx = 0; cx < cols; cx++) {
            if (a == 2) { if (cx * 2 + 1 < cols && r + 1 < rows) cell_vc(fb, pitch, bpp, swap, cx * 16, r * 16, cx < n ? (uint8_t)t[cx] : ' ', fg, bg, 1); continue; }
            cell_vc(fb, pitch, bpp, swap, cx * 8, r * 16, cx < n ? (uint8_t)t[cx] : ' ', fg, bg, 0);
        }
    }
}
/* the compact Mac's own screen: 512 x 342, one bit, 1 is black; 64 columns, 21 rows of 16. se_render draws it into
   se_fb on any board (GET /r/screen.pbm with mac.recoveryscreen=1 photographs it in the testbed, docs/08 item 118) */
static int screen_shot_ok(void) { const char *o = psmac_option("mac.recoveryscreen"); return o && o[0] == '1'; }
static void se_render(void)
{
    for (unsigned i = 0; i < sizeof se_fb; i++) se_fb[i] = 0;
    int nv = layout(64);
    for (int r = 0; r < 21 && r < nv; r++) {
        int a = vattr[r];
        if (a == 3) continue;
        const char *t = vline[r]; unsigned n = slen(t);
        int s = a == 2 ? 2 : 1;
        for (unsigned cx = 0; cx * (unsigned)s < 64; cx++) {
            const uint8_t *data = glyph(cx < n ? (uint8_t)t[cx] : ' ');
            for (uint32_t y = 0; y < 16; y++) {
                uint8_t bits = (cx < n && (uint8_t)t[cx] >= 32) ? *data : 0;
                if (a == 1) bits = (uint8_t)~bits;
                for (int sy = 0; sy < s; sy++) {
                    uint32_t py = (uint32_t)r * 16 + y * (uint32_t)s + (uint32_t)sy;
                    if (py >= 342) break;
                    uint8_t *row = se_fb + py * 64;
                    if (s == 1) row[cx] = bits;
                    else {
                        uint16_t wide = 0; for (int k = 0; k < 8; k++) if (bits & (0x80 >> k)) wide |= (uint16_t)(3u << (14 - 2 * k));
                        row[2 * cx] = (uint8_t)(wide >> 8); row[2 * cx + 1] = (uint8_t)wide;
                    }
                }
                if (y & 1) data += modulo;
            }
        }
    }
    for (int y = 21 * 16; y < 342; y++) for (int k = 0; k < 64; k++) se_fb[y * 64 + k] = 0;
}
static void draw_se(void)
{
    if (!psmac_board_se || psmac_board_solo) return;
    se_render();
    psmac_se_mark(se_fb, 512, 342, 64, 1);
}

/* ============================================================== the network, the card, progress */
static volatile struct an_unit *unit(int u)
{
    volatile uint8_t *nb = alice_net_board();
    return nb ? (volatile struct an_unit *)(nb + AN_OFF_UNIT(u)) : 0;
}
static const char *st_name(uint32_t s)
{
    static const char *const n[] = { "off", "starting", "idle", "scanning", "joining", "connected", "retrying", "failed", "no firmware" };
    return s < 9 ? n[s] : "?";
}
static char last_action[120];                                   /* the screen's and the state's "last" */
static void did(const char *fmt, ...)
{
    struct sb b = { last_action, 0, sizeof last_action };
    va_list ap; va_start(ap, fmt); vkprintf_pc(sb_putc, &b, fmt, ap); va_end(ap);
    last_action[b.n] = 0;
    kprintf("[RECOVERY] %s\n", last_action);
    scr_dirty = 1;
}
static int have_old;                                            /* kernel8.old is on the card */
static int card_seen; static uint32_t card_files;
static char cfg_kernel[64];                                     /* config.txt's kernel= when it is not kernel8.img */

/* the upload in hand (one at a time) */
enum { UP_IDLE, UP_RECV, UP_VERIFY, UP_DONE };
enum { UK_PLAIN, UK_KERNEL, UK_CMDLINE, UK_CONFIG };
#define RING (64u * 1024u)
static struct {
    int state, conn, kind, code;
    char target[REC_NAME_MAX], temp[16];
    uint32_t total, got, written, crc, vcrc, t0;
    struct rec_wr wr; struct rec_rd rd;
    uint8_t *ring; uint32_t rh, rt;
    uint8_t *kbuf; uint32_t kmax, klen;                         /* the file read back whole (kernels and texts) */
    char result[640];
} U;

/* the keyboard menu (below) and its pending edits */
static int menu_on, menu_sel, menu_edit, menu_list;            /* menu_edit: 1 network name, 2 password, 3 country; menu_list: the scan list */
static char m_ssid[36], m_psk[68], m_country[4], m_fpu[8], m_boot[8], m_console[8];
static char m0_ssid[36], m0_country[4], m0_fpu[8], m0_boot[8], m0_console[8];       /* as loaded: only changes are written */
static int m_psk_set, m_changed;
static char m_buf[68]; static unsigned m_len;
static char m_msg[64];
static void menu_lines(void);
static int menu_bt, bt_sel, bt_forget_armed = -1;            /* the keyboard menu's Bluetooth list (docs/08 item 136) */
static void bt_lines(int brief);
static volatile struct an_bt *btb(void);
static void bt_name(const volatile char *s, char *o, unsigned cap);
static const char *bt_state_name(uint32_t st);

/* the programmer's switch on this board (the SE, Plus, 512K; the testbed's mac.switchsim=) */
static int switch_board(void) { return (psmac_board_se && !psmac_board_solo) || psmac_option("mac.switchsim") != 0; }
static int sw_armed; static uint32_t sw_free_since, sw_held_since;
/* The access point's screen: what to join, the password, where to go, the PIN, how to leave. Short lines: the
   compact Mac's screen has 64 columns and 21 rows, and each large line takes two */
static void ap_lines(void)
{
    const char *pinshown = pin_locked_until && (int32_t)(now_ms() - pin_locked_until) < 0 ? "----" : pin;
    L(1, " Aurora68 Setup and Recovery: access point");
    if (ap_state != AP_UP) {
        L(0, "Starting the access point%s%s ...", ap_ssid[0] ? " " : "", ap_ssid);
        L(0, "Asked for: %s", ap_how);
        L(0, "");
        L(2, "PIN %s", pinshown);
    } else {
        L(0, "On a phone or laptop, join the WiFi network");
        L(2, "%s", ap_ssid);
        if (!ap_open) { L(0, "with the password (WPA2)"); L(2, "%s", ap_pass); }
        else L(0, "(no password: %s; the PIN guards the page)", ap_pass[0] ? "this WiFi firmware cannot do WPA2 as an access point" : "recovery.apsec=open");
        L(0, "then open  http://192.168.4.1/  and give the PIN");
        L(2, "PIN %s", pinshown);
        int c = psmac_ip_ap_clients();
        L(0, "Devices with an address: %d", c);
    }
    bt_lines(1);
    if (pin_changes) L(0, "%u x 10 wrong PINs: a new PIN each time", pin_changes);
    if (U.state == UP_RECV && U.total) L(0, "Now:   %s %u of %u KB (%u%%)", U.target, U.got >> 10, U.total >> 10, (uint32_t)((uint64_t)U.got * 100u / U.total));
    else if (U.state == UP_VERIFY) L(0, "Now:   %s: reading it back and checking", U.target);
    else L(0, "Last:  %s", last_action[0] ? last_action : "nothing yet");
    L(0, "To leave: Restart on the page, or press the reset button.");
}
static void build_page(void)
{
    nlines = 0;
    struct ipk_info ii; psmac_ip_info(&ii);
    if (ap_active()) { ap_lines(); menu_lines(); return; }
    L(1, " Aurora68 Setup and Recovery");
    L(0, "Why: %s", why_text);
    if (ap_state == AP_FAILED) L(0, "The access point did not start: %s", ap_err);
    L(0, "The 68k was not started: no Mac code runs. Restart from the page.");
    if (ii.unit >= 0 && ii.ip[0])
        L(0, "Open  http://%d.%d.%d.%d/   or  http://%s.local/", ii.ip[0], ii.ip[1], ii.ip[2], ii.ip[3], psmac_ip_name());
    else L(0, "Open  http://%s.local/  (waiting for an address)", psmac_ip_name());
    L(2, "PIN %s", pin_locked_until && (int32_t)(now_ms() - pin_locked_until) < 0 ? "----" : pin);
    volatile struct an_unit *w = unit(1), *wi = unit(0);
    if (w && w->kind) L(0, "Wired: %s%s", w->link ? "link up" : "no link", ii.unit == 1 ? (ii.ip[0] ? ii.ip[0] == 169 ? ", link-local (no DHCP)" : ", DHCP" : ", asking DHCP") : "");
    else L(0, "Wired: none");
    if (wi && wi->kind) {
        char s[40]; unsigned k = 0; for (; k < 32 && wi->ssid[k]; k++) s[k] = wi->ssid[k];
        s[k] = 0;
        L(0, "WiFi:  %s%s%s%s", st_name(wi->state), s[0] && wi->state == AN_ST_CONNECTED ? " to " : "", wi->state == AN_ST_CONNECTED ? s : "", ii.unit == 0 && ii.ip[0] ? " (in use)" : "");
    } else L(0, "WiFi:  none");
    bt_lines(0);
    if (pin_changes) L(0, "%u x 10 wrong PINs: a new PIN each time", pin_changes);
    int ready = psmac_card_rec_ready();
    L(0, "Card:  %s%s", ready == 0 ? "being read" : ready == -1 ? "none" : psmac_card_rec_what(), ready == -2 ? " (read only)" : "");
    if (U.state == UP_RECV && U.total) L(0, "Now:   %s %u of %u KB (%u%%)", U.target, U.got >> 10, U.total >> 10, (uint32_t)((uint64_t)U.got * 100u / U.total));
    else if (U.state == UP_VERIFY) L(0, "Now:   %s: reading it back and checking", U.target);
    else L(0, "Last:  %s", last_action[0] ? last_action : "nothing yet");
    diag_lines();
    if (why == WHY_COUNTER && have_old) L(0, "kernel8.old is on the card: the page offers Roll back");
    if (cfg_kernel[0]) L(0, "Note: config.txt starts %s, not kernel8.img", cfg_kernel);
    if (switch_board()) {
        uint32_t held = now_ms() - sw_held_since;
        if (sw_armed && sw_held_since) L(0, "Keep holding the interrupt button: the access point starts in %u s", held >= 2000u ? 0u : (2000u - held + 999u) / 1000u);
        else L(0, "Hold the interrupt button 2 s: a WiFi network of the Pi's own for a phone");
    }
    menu_lines();
}
static void screen_poll(void)
{
    uint32_t t = now_ms();
    if (!scr_dirty && t - scr_last < 2000) return;
    if (t - scr_last < 200) return;
    scr_last = t; scr_dirty = 0;
    diag_screen_poll();
    build_page();
    if (diag_screen != 2) draw_vc();              /* the bus check's report holds HDMI until the page or a key gives it back */
    if (!diag_screen) draw_se();                  /* and the compact Mac's screen from the check's start */
}

/* =========================================================================== cmdline.txt edits */
#define CL_MAX 2048
static char cl_old[CL_MAX + 1], cl_new[CL_MAX + 1]; static int cl_len;
static int cl_read(void)
{
    int n = psmac_card_rec_get("cmdline.txt", (uint8_t *)cl_old, CL_MAX);
    if (n < 0) { cl_len = 0; cl_old[0] = 0; return -1; }
    cl_old[n] = 0; cl_len = n;
    return 0;
}
static int is_secret(const char *k) { const struct pdg_opt *o = pdg_find(k); return (o && (o->flags & PDG_F_SECRET)) || ieq(k, "mac.wifisim"); }
/* the line as the browser may see it: secrets as *** */
static void redacted(const char *in, char *out, uint32_t cap) { psmac_redact(in, out, cap); }
/* cl_new (n bytes) onto the card, cmdline.bak keeping the line before; 0, or -1 and why */
static int cl_write(uint32_t n, char *why_, unsigned whymax)
{
    if (pcl_check_line(cl_new, n, why_, whymax)) return -1;
    if (cl_len > 0 && psmac_card_rec_put("cmdline.bak", (const uint8_t *)cl_old, (uint32_t)cl_len)) { scpy(why_, "cmdline.bak could not be written", whymax); return -1; }
    if (psmac_card_rec_put("cmdline.txt", (const uint8_t *)cl_new, n)) { scpy(why_, psmac_card_rec_error(), whymax); return -1; }
    for (uint32_t i = 0; i < n; i++) cl_old[i] = cl_new[i];
    cl_old[n] = 0; cl_len = (int)n;
    return 0;
}
/* one edit of cl_new in place: key to value (NULL removes) */
static int cl_set(uint32_t *n, const char *key, const char *value)
{
    static char tmp[CL_MAX + 1];
    int k = pcl_set(cl_new, *n, tmp, CL_MAX, key, value);
    if (k < 0) return k;
    for (int i = 0; i < k; i++) cl_new[i] = tmp[i];
    cl_new[k] = 0; *n = (uint32_t)k;
    return 0;
}
/* the options the page knows, their values and the boards that use them: psmac_diag.c's table (docs/08 item 122) */
static int country_ok(const char *c, char *why_, unsigned whymax)
{
    if (!c[0]) return 1;
    if (slen(c) != 2 || c[0] < 'A' || c[0] > 'Z' || c[1] < 'A' || c[1] > 'Z') { scpy(why_, "the country is two capital letters (GB, DE, US ...)", whymax); return 0; }
    if (c[0] == 'X' && c[1] == 'X') { scpy(why_, "XX is the instructions' placeholder, which the kernel ignores: your own country's two letters", whymax); return 0; }
    return 1;
}

/* ========================================================================== the WiFi from here */
static int wifi_test, wifi_test_ok; static uint32_t wifi_test_t0; static char wifi_test_ssid[36], wifi_note[120];
static int wifi_cmd(uint32_t cmd, const void *arg, unsigned n)
{
    volatile struct an_unit *u = unit(0);
    if (!u || !u->kind) return -1;
    if (u->cmd_seq != u->cmd_done) return -2;                   /* the last one still under way */
    for (unsigned i = 0; i < sizeof u->arg; i++) u->arg[i] = i < n ? ((const uint8_t *)arg)[i] : 0;
    u->cmd = cmd;
    __asm__ volatile("dmb ish" ::: "memory");
    u->cmd_seq = u->cmd_seq + 1;
    return 0;
}
static struct an_join join_wait; static int join_waiting; static uint32_t join_wait_t0;
static int wifi_join(const char *ssid, const char *psk, const char *country)
{
    struct an_join j;
    for (unsigned i = 0; i < sizeof j; i++) ((uint8_t *)&j)[i] = 0;
    scpy(j.ssid, ssid, sizeof j.ssid); scpy(j.pass, psk, sizeof j.pass); scpy(j.country, country, sizeof j.country);
    int r = wifi_cmd(AN_CMD_JOIN, &j, sizeof j);
    if (r == -2) { join_wait = j; join_waiting = 1; join_wait_t0 = now_ms(); return 0; }   /* the radio is busy: sent when it is free */
    join_waiting = 0;
    return r;
}
static void wifi_poll(void)
{
    if (join_waiting) {
        int r = wifi_cmd(AN_CMD_JOIN, &join_wait, sizeof join_wait);
        if (r != -2 || now_ms() - join_wait_t0 > 10000) { join_waiting = 0; if (wifi_test) wifi_test_t0 = now_ms(); }
        for (unsigned i = 0; i < sizeof join_wait.pass; i++) if (!join_waiting) join_wait.pass[i] = 0;
        return;
    }
    if (!wifi_test) return;
    volatile struct an_unit *u = unit(0);
    if (!u) return;
    uint32_t t = now_ms() - wifi_test_t0;
    if (t < 1500) return;                                       /* the join command's own state changes first */
    struct ipk_info ii; psmac_ip_info(&ii);
    if (u->state == AN_ST_CONNECTED) {
        wifi_test = 0; wifi_test_ok = 1;
        struct sb b = { wifi_note, 0, sizeof wifi_note };
        if (ii.unit == 0 && ii.ip[0]) bprintf(&b, "joined %s: address %d.%d.%d.%d", wifi_test_ssid, ii.ip[0], ii.ip[1], ii.ip[2], ii.ip[3]);
        else if (ii.unit == 0) { bprintf(&b, "joined %s, asking for an address", wifi_test_ssid); wifi_test = 2; }
        else bprintf(&b, "joined %s (the wired port is in use, so the WiFi was not given an address here)", wifi_test_ssid);
        did("WiFi test: %s", wifi_note);
        return;
    }
    if (t > 25000 || u->state == AN_ST_FAILED) {
        wifi_test = 0; wifi_test_ok = 0;
        char m[100]; unsigned k = 0; for (; k < 95 && u->msg[k]; k++) m[k] = u->msg[k];
        m[k] = 0;
        struct sb b = { wifi_note, 0, sizeof wifi_note };
        bprintf(&b, "could not join %s (%s)", wifi_test_ssid, m[0] ? m : st_name(u->state));
        did("WiFi test: %s; the settings in cmdline.txt are joined again", wifi_note);
        char s[40] = "", p[70] = "", c[4] = "";
        if (cl_read() == 0) { pcl_get(cl_old, (uint32_t)cl_len, "wifi.ssid", s, sizeof s); pcl_get(cl_old, (uint32_t)cl_len, "wifi.psk", p, sizeof p); pcl_get(cl_old, (uint32_t)cl_len, "wifi.country", c, sizeof c); }
        if (s[0]) wifi_join(s, p, c);
    }
}
static void wifi_test_poll2(void)                               /* joined, waiting for DHCP on the WiFi */
{
    if (wifi_test != 2) return;
    struct ipk_info ii; psmac_ip_info(&ii);
    if (ii.unit == 0 && ii.ip[0]) {
        wifi_test = 0;
        struct sb b = { wifi_note, 0, sizeof wifi_note };
        bprintf(&b, "joined %s: address %d.%d.%d.%d", wifi_test_ssid, ii.ip[0], ii.ip[1], ii.ip[2], ii.ip[3]);
        did("WiFi test: %s", wifi_note);
    } else if (now_ms() - wifi_test_t0 > 40000) { wifi_test = 0; did("WiFi test: joined %s, but no address came", wifi_test_ssid); }
}

/* ========================================================================== the access point
   23 November 2026, docs/08 item 118. Asked for by the interrupt button held two seconds on this screen
   (switch_poll), the keyboard menu, or recovery.ap=on; the WiFi driver does the firmware's part (AN_CMD_AP,
   alice_net.c and wifi/fmac_ap.c, or the testbed's simulated radio), the IP stack the address and the guests'
   DHCP and DNS (psmac_ip_ap). The password: ten characters from 31 that cannot be misread on a one-bit
   screen (no 0/o, 1/l/i), from the BCM2835's random generator, about 50 bits: for a network that lives
   for one start, beside a PIN. It is drawn on the screens only, never logged and never served. */
static void ap_fail(const char *fmt, ...)
{
    struct sb b = { ap_err, 0, sizeof ap_err };
    va_list ap; va_start(ap, fmt); vkprintf_pc(sb_putc, &b, fmt, ap); va_end(ap);
    ap_err[b.n] = 0;
    ap_state = AP_FAILED;
    for (unsigned i = 0; i < sizeof ap_pass; i++) ap_pass[i] = 0;
    did("access point: %s", ap_err);
    psmac_ip_ap_off();                                           /* back to the wired port or the saved WiFi */
    char s_[40] = "", p_[70] = "", c_[4] = "";
    if (cl_read() == 0) { pcl_get(cl_old, (uint32_t)cl_len, "wifi.ssid", s_, sizeof s_); pcl_get(cl_old, (uint32_t)cl_len, "wifi.psk", p_, sizeof p_); pcl_get(cl_old, (uint32_t)cl_len, "wifi.country", c_, sizeof c_); }
    if (!s_[0]) {                                                /* none in cmdline.txt: this start's own options */
        const char *ks[3] = { "wifi.ssid", "wifi.psk", "wifi.country" }; char *vs[3] = { s_, p_, c_ }; unsigned caps[3] = { sizeof s_, sizeof p_, sizeof c_ };
        for (int k = 0; k < 3; k++) { const char *v = psmac_option(ks[k]); unsigned i = 0; for (; v && v[i] && v[i] != ' ' && i + 1 < caps[k]; i++) vs[k][i] = v[i]; vs[k][i] = 0; }
    }
    if (s_[0] && unit(0) && unit(0)->kind) wifi_join(s_, p_, c_);
    for (unsigned i = 0; i < sizeof p_; i++) p_[i] = 0;
}
static void ap_request(const char *how)
{
    if (ap_active() || !rec_on) return;
    const char *o = psmac_option("recovery.apsec");
    int open_ = o && o[0] == 'o';
    ap_flags = o && o[0] == 'w' ? AN_AP_WPA2_ANYWAY : 0;
    ap_channel = 6;                                              /* allowed in every country */
    if ((o = psmac_option("recovery.apchannel"))) { int v = 0; while (*o >= '0' && *o <= '9') v = v * 10 + (*o++ - '0'); if (v >= 1 && v <= 13) ap_channel = v; }
    for (unsigned i = 0; i < sizeof ap_pass; i++) ap_pass[i] = 0;
    if (!open_) {
        /* sixteen words of the generator (and the counters it is mixed with) through SHA-1, so that no pattern of
           a counter shows even where the generator is slow to give words (QEMU's) */
        static const char abc[] = "abcdefghjkmnpqrstuvwxyz23456789";
        uint8_t pool[64 + 8], d[20];
        for (int round = 0, i = 0; i < 10; round++) {
            for (int k = 0; k < 16; k++) { uint32_t r = rng_word(); pool[4 * k] = (uint8_t)r; pool[4 * k + 1] = (uint8_t)(r >> 8); pool[4 * k + 2] = (uint8_t)(r >> 16); pool[4 * k + 3] = (uint8_t)(r >> 24); }
            uint64_t tk = now_ticks(); for (int k = 0; k < 8; k++) pool[64 + k] = (uint8_t)(tk >> (8 * k) ^ (uint64_t)round);
            fm_sha1(pool, sizeof pool, d);
            for (int k = 0; k < 20 && i < 10; k++) if (d[k] < 248) ap_pass[i++] = abc[d[k] % 31];
        }
        for (unsigned k = 0; k < sizeof pool; k++) pool[k] = 0;
        for (unsigned k = 0; k < sizeof d; k++) d[k] = 0;
    }
    scpy(ap_how, how, sizeof ap_how);
    ap_ssid[0] = 0; ap_err[0] = 0;
    ap_state = AP_WAITING; ap_t0 = now_ms();
    wifi_test = 0; join_waiting = 0;
    psmac_ip_ap(ap_ip);
    did("access point asked for: %s", how);
}
static void ap_poll(void)
{
    if (!ap_active()) return;
    volatile struct an_unit *u = unit(0);
    uint32_t t = now_ms() - ap_t0;
    char m[100]; unsigned k = 0;
    if (u) for (; k < 95 && u->msg[k]; k++) m[k] = u->msg[k];
    m[k] = 0;
    if (ap_state == AP_WAITING) {
        if (!u || !u->kind) { if (t > 5000) ap_fail("this Pi's WiFi is off (wifi=off in cmdline.txt) or not there"); return; }
        int known = (u->mac[0] | u->mac[1] | u->mac[2] | u->mac[3] | u->mac[4] | u->mac[5]) != 0;
        if (!known || u->state == AN_ST_OFF || u->state == AN_ST_STARTING || u->state == AN_ST_NOFW) {
            if (t > 45000) ap_fail("the WiFi chip did not start (%s)", m[0] ? m : st_name(u->state));
            return;
        }
        struct sb b = { ap_ssid, 0, sizeof ap_ssid };
        bprintf(&b, "Aurora68-%02X%02X", u->mac[4], u->mac[5]);
        struct an_ap a;
        for (unsigned i = 0; i < sizeof a; i++) ((uint8_t *)&a)[i] = 0;
        scpy(a.ssid, ap_ssid, sizeof a.ssid); scpy(a.pass, ap_pass, sizeof a.pass);
        if (cl_read() == 0) { char c_[8] = ""; pcl_get(cl_old, (uint32_t)cl_len, "wifi.country", c_, sizeof c_); if (country_ok(c_, m, sizeof m)) scpy(a.country, c_, sizeof a.country); }
        a.channel = (unsigned)ap_channel; a.flags = (unsigned)ap_flags;
        int r = wifi_cmd(AN_CMD_AP, &a, sizeof a);
        for (unsigned i = 0; i < sizeof a; i++) ((volatile uint8_t *)&a)[i] = 0;
        if (r == -2) { if (t > 45000) ap_fail("the WiFi radio stayed busy with another command"); return; }
        if (r) { ap_fail("there is no WiFi radio"); return; }
        ap_seq = u->cmd_seq; ap_state = AP_STARTING; ap_t0 = now_ms();
        kprintf("[RECOVERY] access point %s: asked of the WiFi driver (channel %d, %s)\n", ap_ssid, ap_channel,
                ap_pass[0] ? (ap_flags ? "WPA2 even without idauth: recovery.apsec=wpa2" : "WPA2 if the firmware can be the authenticator") : "open: recovery.apsec=open");
        scr_dirty = 1;
        return;
    }
    char ss[36]; unsigned q = 0; for (; q < 32 && u->ssid[q]; q++) ss[q] = u->ssid[q];
    ss[q] = 0;
    int ours = !__builtin_strcmp(ss, ap_ssid);
    if (ap_state == AP_STARTING) {
        if (u->cmd_done == ap_seq && u->cmd_result == -2) { ap_fail("the WiFi driver refused the access point's settings"); return; }
        if (u->state == AN_ST_CONNECTED && u->link && ours) {
            ap_state = AP_UP; ap_open = !(u->security & 0x08);
            did("access point %s is up: channel %d, %s", ap_ssid, ap_channel, ap_open ? "open" : "WPA2");
            if (ap_open && ap_pass[0]) kprintf("[RECOVERY] the access point is open: the firmware cannot be its WPA2 authenticator (the PIN guards the page; the screen says so)\n");
            return;
        }
        if (u->state == AN_ST_FAILED && u->cmd_done == ap_seq) { ap_fail("%s", m[0] ? m : "the WiFi chip failed"); return; }
        if (t > 40000) ap_fail("no answer from the WiFi chip within 40 s (%s)", m[0] ? m : st_name(u->state));
        return;
    }
    /* up: a scan pauses it; a chip that was lost is started again by the driver, access point and all */
    if (u->state == AN_ST_SCANNING) return;
    if (u->state != AN_ST_CONNECTED || !u->link || !ours) { ap_state = AP_STARTING; ap_t0 = now_ms(); did("the access point went down (%s): the WiFi driver starts it again", m[0] ? m : st_name(u->state)); return; }
    static int clients = -1;
    int c = psmac_ip_ap_clients();
    if (c != clients) { clients = c; scr_dirty = 1; }
}
/* The programmer's interrupt button on the recovery screen: let go for a second (the press that brought recovery
   on, or a finger still on it, does not count), then held two seconds without a break. A glitch on the wire is
   milliseconds; nobody holds the button that long by accident, and on this screen it has no other meaning. */
static void switch_poll(void)
{
    if (!switch_board() || ap_active()) { sw_held_since = 0; return; }
    uint32_t t = now_ms();
    if (!psmac_se_switch_held()) {
        if (sw_held_since) { sw_held_since = 0; scr_dirty = 1; }
        if (!sw_free_since) sw_free_since = t ? t : 1;
        if (!sw_armed && t - sw_free_since >= 1000) { sw_armed = 1; kprintf("[RECOVERY] the interrupt button is free: holding it two seconds now starts the access point\n"); }
        return;
    }
    sw_free_since = 0;
    if (!sw_armed) return;
    if (!sw_held_since) { sw_held_since = t ? t : 1; scr_dirty = 1; kprintf("[RECOVERY] the interrupt button is held\n"); return; }
    static uint32_t last_draw;
    if (t - last_draw >= 250) { last_draw = t; scr_dirty = 1; }
    if (t - sw_held_since >= 2000) { sw_held_since = 0; sw_armed = 0; ap_request("the interrupt button held two seconds on the recovery screen"); }
}
/* The programmer's reset button: on the SE the PiStorm sees the 68000's reset line (psmac_gpio.c). With no Mac
   running here it restarts the Pi, which starts as cmdline.txt says: the access point is not kept. Armed once
   the line has been free for three seconds (the Mac's own power-on reset is long over by then), taken when held
   a tenth of a second. mac.resetsim=S is the testbed's press, S seconds into the recovery start. */
static void reset_button_poll(void)
{
    static uint32_t t_start, free_since, held_since; static int armed, done;
    uint32_t t = now_ms();
    if (!t_start) t_start = t ? t : 1;
    if (done) return;
    int down;
    const char *o = psmac_option("mac.resetsim");
    if (o) { uint32_t sec = 0; while (*o >= '0' && *o <= '9') sec = sec * 10 + (uint32_t)(*o++ - '0'); down = t - t_start >= sec * 1000u && t - t_start < sec * 1000u + 500u; }
    else down = psmac_board_se && !psmac_board_solo && psmac_gpio_reset_asserted();
    if (!down) { held_since = 0; if (!free_since) free_since = t ? t : 1; if (!armed && t - free_since >= 3000) armed = 1; return; }
    free_since = 0;
    if (!armed) return;
    if (!held_since) { held_since = t ? t : 1; return; }
    if (t - held_since < 100) return;
    done = 1;
    did("the reset button: the Pi restarts%s", ap_active() ? " (the access point is not kept)" : "");
    pi_restart_in(300);
}

/* ============================================================================ checking a kernel */
static int find_bytes(const uint8_t *h, uint32_t n, const char *needle)
{
    uint32_t m = slen(needle);
    for (uint32_t i = 0; i + m <= n; i++) {
        if (h[i] != (uint8_t)needle[0]) continue;
        uint32_t k = 1; while (k < m && h[i + k] == (uint8_t)needle[k]) k++;
        if (k == m) return 1;
    }
    return 0;
}
/* 0 when k (n bytes) is an Aurora68 kernel the Pi can start; else why. info: what it is */
static int kernel_check(const uint8_t *k, uint32_t n, char *why_, unsigned whymax, char *info, unsigned infomax)
{
    struct sb bi = { info, 0, infomax }, bw = { why_, 0, whymax };
    const uint8_t *img = k; uint32_t len = n;
    static uint8_t *out; static uint32_t out_max;
    if (n < 4096) { bput(&bw, "too short for a kernel"); return -1; }
    if (k[0] == 0x1f && k[1] == 0x8b) {
        if (!out) {
            uint32_t ram = psmac_ram_size();
            if (ram >= (48u << 20)) { out = psmac_mac_ram_ptr() + (16u << 20); out_max = ram - (16u << 20); if (out_max > (64u << 20)) out_max = 64u << 20; }
            else { out_max = 24u << 20; out = tlsf_malloc(tlsf, out_max); }
            if (!out) { out_max = 0; bput(&bw, "no memory to decompress it into"); return -1; }
        }
        struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
        if (!d) { bput(&bw, "no memory for the decompressor"); return -1; }
        size_t used = 0, got = 0;
        enum libdeflate_result r = libdeflate_gzip_decompress_ex(d, k, n, out, out_max, &used, &got);
        libdeflate_free_decompressor(d);
        if (r == LIBDEFLATE_BAD_DATA) { bput(&bw, "the gzip data is damaged: its header, its deflate stream, or its CRC32 or length trailer does not check"); return -1; }
        if (r != LIBDEFLATE_SUCCESS) { bprintf(&bw, "it does not decompress into %u MB (libdeflate %d)", out_max >> 20, (int)r); return -1; }
        if (used != n) { bprintf(&bw, "%u bytes follow the gzip stream", (uint32_t)(n - used)); return -1; }
        img = out; len = (uint32_t)got;
        bprintf(&bi, "gzip, %u KB, CRC32 and length checked, %u KB decompressed", n >> 10, len >> 10);
    } else bprintf(&bi, "not compressed, %u KB", n >> 10);
    if (len < 64 || img[56] != 'A' || img[57] != 'R' || img[58] != 'M' || img[59] != 0x64) { bput(&bw, "not an ARM64 kernel image: no \"ARM\\x64\" at offset 56"); return -1; }
    if (!find_bytes(img, len, "[BOOT] Build ID")) { bput(&bw, "an ARM64 image, but not Emu68: no \"[BOOT] Build ID\" in it"); return -1; }
    if (!find_bytes(img, len, "[PSMAC]")) { bput(&bw, "an Emu68 kernel, but not Aurora68's (variant psmac): no \"[PSMAC]\" in it"); return -1; }
    bput(&bi, "; ARM64 header, Emu68 build ID text and Aurora68 (psmac) texts found");
    return 0;
}
static int kernel_install(char *why_, unsigned whymax)
{
    struct rec_ent k, o;
    int hk = !psmac_card_rec_find("kernel8.img", &k), ho = !psmac_card_rec_find("kernel8.old", &o);
    if (ho && psmac_card_rec_delete("kernel8.old")) { scpy(why_, psmac_card_rec_error(), whymax); return -1; }
    if (!hk) { if (psmac_card_rec_replace("kernel8.img", "kernel8.new")) { scpy(why_, psmac_card_rec_error(), whymax); return -1; } return 0; }
    /* the new kernel is renamed kernel8.old first (one write), then the two change contents (one write, or
       two with repair's safety net): kernel8.img is there all the time, and holds one kernel or the other */
    if (psmac_card_rec_rename("kernel8.new", "kernel8.old")) { scpy(why_, psmac_card_rec_error(), whymax); return -1; }
    if (psmac_card_rec_swap("kernel8.img", "kernel8.old")) { scpy(why_, psmac_card_rec_error(), whymax); return -1; }
    have_old = 1;
    return 0;
}

/* a whole kernel's bytes: the Mac's first 16 MB when it has 48 MB or more (no Mac runs), else 8 MB of the kernel's heap */
static uint8_t *kernel_buf(uint32_t *max)
{
    static uint8_t *kb;
    if (psmac_ram_size() >= (48u << 20)) { *max = 16u << 20; return psmac_mac_ram_ptr(); }
    if (!kb) kb = tlsf_malloc(tlsf, 8u << 20);
    *max = kb ? 8u << 20 : 0;
    return kb;
}

/* ================================================================================ the uploads */
static uint8_t text_buf[16384];
static const char *temp_for(int kind) { return kind == UK_KERNEL ? "kernel8.new" : "aurora68.up"; }
static int up_start(int c, const char *name, uint32_t total, char *why_, unsigned whymax)
{
    if (U.state == UP_RECV || U.state == UP_VERIFY) { scpy(why_, "another upload is under way", whymax); return 409; }
    if (psmac_card_rec_ready() != 1) { scpy(why_, "no writable card", whymax); return 503; }
    U.kind = ieq(name, "kernel8.img") ? UK_KERNEL : ieq(name, "cmdline.txt") ? UK_CMDLINE : ieq(name, "config.txt") ? UK_CONFIG : UK_PLAIN;
    if (U.kind == UK_CMDLINE || U.kind == UK_CONFIG) { if (total > sizeof text_buf) { scpy(why_, "longer than 16 KB: not a cmdline.txt or config.txt", whymax); return 413; } U.kbuf = text_buf; U.kmax = sizeof text_buf; }
    else if (U.kind == UK_KERNEL) {
        U.kbuf = kernel_buf(&U.kmax);
        if (total > U.kmax) { struct sb b = { why_, 0, whymax }; bprintf(&b, "larger than %u MB: not a kernel8.img", U.kmax >> 20); return 413; }
    } else { U.kbuf = 0; U.kmax = 0; }
    struct rec_ent e;
    scpy(U.temp, temp_for(U.kind), sizeof U.temp);
    if (!psmac_card_rec_find(U.temp, &e)) psmac_card_rec_delete(U.temp);
    if (!U.ring) U.ring = tlsf_malloc(tlsf, RING);
    if (!U.ring) { scpy(why_, "no memory for the upload's buffer", whymax); return 503; }
    if (psmac_card_rec_create(U.temp, U.kind == UK_KERNEL ? "kernel8.img" : name, &U.wr)) { scpy(why_, psmac_card_rec_error(), whymax); return 507; }
    scpy(U.target, name, sizeof U.target);
    U.state = UP_RECV; U.conn = c; U.total = total; U.got = U.written = 0; U.rh = U.rt = 0; U.crc = 0; U.klen = 0; U.t0 = now_ms();
    U.result[0] = 0;
    kprintf("[RECOVERY] upload of %s, %u bytes, into %s\n", name, total, U.temp);
    scr_dirty = 1;
    return 0;
}
static uint32_t up_room(void) { return RING - (U.rh - U.rt); }
static void up_feed(const uint8_t *d, uint32_t n)
{
    if (U.state != UP_RECV) return;
    if (n > U.total - U.got) n = U.total - U.got;
    if (n > up_room()) n = up_room();
    for (uint32_t i = 0; i < n; i++) U.ring[(U.rh + i) % RING] = d[i];
    U.rh += n; U.got += n;
}
static void up_answer(int code, const char *fmt, ...)
{
    struct sb b = { U.result, 0, sizeof U.result };
    va_list ap; va_start(ap, fmt); vkprintf_pc(sb_putc, &b, fmt, ap); va_end(ap);
    U.result[b.n] = 0;
    U.code = code; U.state = UP_DONE;
    scr_dirty = 1;
}
static void up_fail(int code, const char *why_)
{
    struct rec_ent e;
    if (U.wr.open) psmac_card_rec_abandon(&U.wr);
    if (!psmac_card_rec_find(U.temp, &e)) psmac_card_rec_delete(U.temp);
    char j[300]; struct sb b = { j, 0, sizeof j }; bjson(&b, why_);
    up_answer(code, "{\"error\": %s, \"name\": \"%s\"}\n", j, U.target);
    did("upload of %s refused: %s", U.target, why_);
}
static void cl_unsecret(char *t, uint32_t *n);
static void up_finish(void)
{
    char why_[200], info[200];
    uint32_t secs = (now_ms() - U.t0) / 1000u;
    if (U.vcrc != U.crc) { struct sb b = { why_, 0, sizeof why_ }; bprintf(&b, "the card gave back other bytes than were sent (CRC32 %08x, sent %08x)", U.vcrc, U.crc); up_fail(500, why_); return; }
    if (U.kind == UK_KERNEL) {
        if (kernel_check(U.kbuf, U.klen, why_, sizeof why_, info, sizeof info)) { up_fail(422, why_); return; }
        if (kernel_install(why_, sizeof why_)) { up_fail(500, why_); return; }
        up_answer(200, "{\"name\": \"kernel8.img\", \"size\": %u, \"crc32\": \"%08x\", \"verified\": true, \"installed\": true, \"old_kept\": \"kernel8.old\", \"check\": \"%s\"}\n", U.total, U.crc, info);
        did("kernel8.img installed (%s); the one before is kernel8.old", info);
        return;
    }
    if (U.kind == UK_CMDLINE) {
        uint32_t n = U.klen;
        cl_read();
        for (uint32_t i = 0; i < n && i < CL_MAX; i++) cl_new[i] = (char)U.kbuf[i];
        if (n > CL_MAX) n = CL_MAX;
        cl_new[n] = 0;
        cl_unsecret(cl_new, &n);
        struct rec_ent e; if (!psmac_card_rec_find(U.temp, &e)) psmac_card_rec_delete(U.temp);
        if (cl_write(n, why_, sizeof why_)) { up_fail(422, why_); return; }
        up_answer(200, "{\"name\": \"cmdline.txt\", \"size\": %u, \"crc32\": \"%08x\", \"verified\": true, \"backup\": \"cmdline.bak\"}\n", n, U.crc);
        did("cmdline.txt written (%u bytes; the line before in cmdline.bak)", n);
        return;
    }
    if (U.kind == UK_CONFIG) {
        static uint8_t old[16384];
        int on = psmac_card_rec_get("config.txt", old, sizeof old);
        if (on > 0 && psmac_card_rec_put("config.bak", old, (uint32_t)on)) { up_fail(500, "config.bak could not be written"); return; }
    }
    if (psmac_card_rec_replace(U.target, U.temp)) { up_fail(500, psmac_card_rec_error()); return; }
    U.state = UP_DONE;
    up_answer(200, "{\"name\": \"%s\", \"size\": %u, \"crc32\": \"%08x\", \"verified\": true, \"seconds\": %u%s}\n", U.target, U.total, U.crc, secs,
              U.kind == UK_CONFIG ? ", \"backup\": \"config.bak\"" : "");
    did("%s written: %u KB in %u s, read back and checked (CRC32 %08x)", U.target, U.total >> 10, secs, U.crc);
}
static void up_pump(void)
{
    if (U.state == UP_RECV) {
        uint32_t cb = psmac_card_rec_cluster();
        for (int k = 0; k < 4 && U.rh != U.rt; k++) {
            uint32_t avail = U.rh - U.rt, at = U.rt % RING, n = avail;
            if (n > RING - at) n = RING - at;
            if (n > cb) n = cb;
            if (psmac_card_rec_write(&U.wr, U.ring + at, n)) { up_fail(507, psmac_card_rec_error()); return; }
            U.crc = (uint32_t)libdeflate_crc32(U.crc, U.ring + at, n);
            U.rt += n; U.written += n;
            scr_dirty = 1;
        }
        if (U.written == U.total) {
            if (psmac_card_rec_close(&U.wr)) { up_fail(507, psmac_card_rec_error()); return; }
            struct rec_ent e;
            if (psmac_card_rec_find(U.temp, &e) || e.size != U.total) { up_fail(500, "the file is not on the card as written"); return; }
            psmac_card_rec_read_open(&e, &U.rd);
            U.vcrc = 0; U.klen = 0; U.state = UP_VERIFY;
        }
        return;
    }
    if (U.state == UP_VERIFY) {
        static uint8_t vb[32768];
        for (int k = 0; k < 4; k++) {
            int32_t n = psmac_card_rec_read(&U.rd, vb, sizeof vb);
            if (n < 0) { up_fail(500, "the card could not be read back"); return; }
            if (n == 0) { up_finish(); return; }
            U.vcrc = (uint32_t)libdeflate_crc32(U.vcrc, vb, (size_t)n);
            if (U.kbuf && U.klen + (uint32_t)n <= U.kmax) { memcpy(U.kbuf + U.klen, vb, (size_t)n); U.klen += (uint32_t)n; }
        }
    }
}

/* ============================================================================== HTTP on core 3 */
#define NC IPK_CONNS
#define REQ_MAX 17408                                 /* a config.txt of 16 KB and its headers */
enum { RK_NONE, RK_OUT, RK_SRC, RK_FILE, RK_UPLOAD };
struct rconn {
    int used; uint8_t peer[4];
    char req[REQ_MAX + 1]; uint32_t req_len; int handled;
    int kind; char out[2048]; uint32_t out_len, out_pos;
    const char *src; uint32_t src_len, src_pos;
    struct rec_rd rd;
};
static struct rconn rc[NC];
static char big[65536]; static int big_conn = -1;
extern const char recovery_page[];
static void out_head(struct rconn *h, int code, const char *ctype, int64_t len, const char *extra)
{
    const char *txt = code == 200 ? "OK" : code == 204 ? "No Content" : code == 302 ? "Found" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" :
                      code == 409 ? "Conflict" : code == 411 ? "Length Required" : code == 413 ? "Request Entity Too Large" : code == 422 ? "Unprocessable Entity" :
                      code == 429 ? "Too Many Requests" : code == 503 ? "Service Unavailable" : code == 507 ? "Insufficient Storage" : "Error";
    struct sb b = { h->out, 0, sizeof h->out };
    bprintf(&b, "HTTP/1.0 %d %s\r\nServer: Aurora68 Setup and Recovery\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Type: %s\r\n", code, txt, ctype);
    if (len >= 0) bprintf(&b, "Content-Length: %u\r\n", (uint32_t)len);
    if (extra) bput(&b, extra);
    bput(&b, "\r\n");
    h->out_len = b.n; h->out_pos = 0;
}
static void respond(struct rconn *h, int code, const char *ctype, const char *body)
{
    out_head(h, code, ctype, (int64_t)slen(body), 0);
    struct sb b = { h->out, h->out_len, sizeof h->out };
    bput(&b, body);
    h->out_len = b.n; h->kind = RK_OUT;
}
static void json_err(struct rconn *h, int code, const char *msg)
{
    char j[400]; struct sb b = { j, 0, sizeof j };
    bput(&b, "{\"error\": "); bjson(&b, msg); bput(&b, "}\n");
    respond(h, code, "application/json", j);
}
static void respond_src(struct rconn *h, const char *ctype, const char *src, uint32_t len)
{
    out_head(h, 200, ctype, len, 0);
    h->src = src; h->src_len = len; h->src_pos = 0; h->kind = RK_SRC;
}
static int take_big(int c) { if (big_conn >= 0 && big_conn != c) return 0; big_conn = c; return 1; }
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) { *o++ = (char)(hexv(s[1]) * 16 + hexv(s[2])); s += 2; }
        else *o++ = *s;
    }
    *o = 0;
}
static const char *query(const char *q, const char *key, char *out, unsigned max)
{
    unsigned kl = slen(key);
    for (const char *s = q; s && *s; ) {
        if (starts(s, key) && s[kl] == '=') {
            unsigned n = 0;
            for (s += kl + 1; *s && *s != '&' && n + 1 < max; s++) out[n++] = *s;
            out[n] = 0;
            url_decode(out);
            return out;
        }
        while (*s && *s != '&') s++;
        if (*s) s++;
    }
    return 0;
}

/* ---- the answers ---- */
struct list_ctx { struct sb *b; int n; };
static int list_cb(void *vc, const struct rec_ent *e)
{
    struct list_ctx *x = vc;
    if (e->attr & 0x08) return 0;
    if (x->b->n + 300 > x->b->cap) return 1;
    bput(x->b, x->n ? ",\n  " : "\n  ");
    bput(x->b, "{\"name\": "); bjson(x->b, e->name);
    bprintf(x->b, ", \"size\": %u, \"folder\": %s, \"hidden\": %s}", e->size, e->attr & 0x10 ? "true" : "false", e->attr & 0x02 ? "true" : "false");
    x->n++;
    return 0;
}
static int count_cb(void *vc, const struct rec_ent *e) { (void)e; (*(uint32_t *)vc)++; return 0; }
static void ans_files(struct rconn *h, int c)
{
    if (!take_big(c)) { json_err(h, 503, "another listing is being sent: try again"); return; }
    struct sb b = { big, 0, sizeof big };
    bput(&b, "{\"files\": [");
    struct list_ctx x = { &b, 0 };
    if (psmac_card_rec_list(list_cb, &x)) { big_conn = -1; json_err(h, 503, psmac_card_rec_error()); return; }
    uint32_t kb = 0;
    bprintf(&b, "\n], \"count\": %d, \"card\": ", x.n); bjson(&b, psmac_card_rec_what());
    if (!psmac_card_rec_free_kb(&kb)) bprintf(&b, ", \"free_kb\": %u", kb);
    bput(&b, "}\n");
    respond_src(h, "application/json", big, b.n);
}
static int cmdline_has_recovery(void)
{
    char v[8];
    if (cl_read()) return 0;
    return (pcl_get(cl_old, (uint32_t)cl_len, "mac.recovery", v, sizeof v) == 1 && v[0] == '1') || pcl_get(cl_old, (uint32_t)cl_len, "recovery.ap", v, sizeof v) == 1;
}
/* "restart normally": mac.recovery=1 and recovery.ap= out of cmdline.txt (after cmdline_has_recovery read it). 0, or -1 */
static int cl_drop_recovery(void)
{
    char w2[200];
    uint32_t nn = (uint32_t)cl_len;
    for (uint32_t i = 0; i < nn; i++) cl_new[i] = cl_old[i];
    cl_new[nn] = 0;
    if (cl_set(&nn, "mac.recovery", 0) || cl_set(&nn, "recovery.ap", 0)) return -1;
    return cl_write(nn, w2, sizeof w2);
}
static void ans_state(struct rconn *h)
{
    static char j[4096]; struct sb b = { j, 0, sizeof j };
    struct ipk_info ii; psmac_ip_info(&ii);
    static const char *const wn[] = { "none", "option", "request", "counter", "switch" };
    bput(&b, "{\"recovery\": true, \"reason\": "); bjson(&b, wn[why]); bput(&b, ", \"why\": "); bjson(&b, why_text);
    bprintf(&b, ", \"board\": \"%s\", \"board_model\": \"%s\", \"boot_count\": %d, \"boot_limit\": %d", psmac_board_solo ? "solo" : psmac_board_plus ? "plus" : psmac_board_se ? "se" : "iici", board_model(), boot_count, boot_limit);
    bprintf(&b, ",\n \"network\": {\"name\": \"%s.local\", \"link\": \"%s\", \"phase\": \"%s\", \"ip\": \"%d.%d.%d.%d\"", psmac_ip_name(), ii.unit == 1 ? "wired" : ii.unit == 0 ? "wifi" : "none", ii.phase, ii.ip[0], ii.ip[1], ii.ip[2], ii.ip[3]);
    volatile struct an_unit *w = unit(1), *wi = unit(0);
    bprintf(&b, ", \"wired\": %s", w && w->kind ? (w->link ? "\"link up\"" : "\"no link\"") : "null");
    if (wi && wi->kind) { char s[40]; unsigned k = 0; for (; k < 32 && wi->ssid[k]; k++) s[k] = wi->ssid[k]; s[k] = 0; bprintf(&b, ", \"wifi\": {\"state\": \"%s\", \"ssid\": ", st_name(wi->state)); bjson(&b, s); bput(&b, "}"); }
    else bput(&b, ", \"wifi\": null");
    bput(&b, "},\n \"card\": "); bjson(&b, psmac_card_rec_ready() == 0 ? "being read" : psmac_card_rec_ready() == -1 ? "none" : psmac_card_rec_what());
    bprintf(&b, ", \"writable\": %s", psmac_card_rec_ready() == 1 ? "true" : "false");
    bprintf(&b, ",\n \"upload\": {\"state\": \"%s\", \"name\": ", U.state == UP_RECV ? "receiving" : U.state == UP_VERIFY ? "checking" : U.state == UP_DONE ? "done" : "idle"); bjson(&b, U.target);
    bprintf(&b, ", \"received\": %u, \"written\": %u, \"total\": %u}", U.got, U.written, U.total);
    bput(&b, ",\n \"last\": "); bjson(&b, last_action);
    bprintf(&b, ", \"kernel8_old\": %s, \"rollback_offered\": %s, \"cmdline_recovery\": %s", have_old ? "true" : "false", why == WHY_COUNTER && have_old ? "true" : "false", cmdline_has_recovery() ? "true" : "false");
    bput(&b, ", \"config_kernel\": "); bjson(&b, cfg_kernel);
    bprintf(&b, ", \"wifi_test\": %s, \"wifi_note\": ", wifi_test ? "\"running\"" : wifi_test_ok ? "\"joined\"" : "\"\""); bjson(&b, wifi_note);
    if (ap_state != AP_OFF) {                                    /* the access point (its password is on the screens only) */
        static const char *const as[] = { "off", "waiting for the WiFi chip", "starting", "up", "failed" };
        bput(&b, ",\n \"ap\": {\"state\": "); bjson(&b, ap_state == AP_UP ? "up" : ap_state == AP_FAILED ? "failed" : "starting");
        bput(&b, ", \"detail\": "); bjson(&b, as[ap_state]);
        bput(&b, ", \"ssid\": "); bjson(&b, ap_ssid);
        bprintf(&b, ", \"security\": \"%s\", \"channel\": %d, \"address\": \"192.168.4.1\", \"clients\": %d, \"how\": ", ap_state != AP_UP ? "" : ap_open ? "open (the PIN guards the page)" : "WPA2", ap_channel, psmac_ip_ap_clients());
        bjson(&b, ap_how); bput(&b, ", \"error\": "); bjson(&b, ap_err); bput(&b, "}");
    } else bput(&b, ",\n \"ap\": null");
    {   /* Bluetooth (docs/08 item 136): its state and who is connected; the Bluetooth tab has the rest (GET /r/bt) */
        volatile struct an_bt *x = btb();
        if (x) {
            char m[64]; bt_name(x->msg, m, sizeof m);
            bput(&b, ",\n \"bt\": {\"state\": "); bjson(&b, bt_state_name(x->state)); bput(&b, ", \"message\": "); bjson(&b, m);
            bprintf(&b, ", \"paired\": %u, \"passkey\": ", x->nkeys);
            if (x->passkey_shown) bprintf(&b, "\"%06u\"", x->passkey); else bput(&b, "null");
            bput(&b, ", \"connected\": [");
            for (unsigned k = 0, n = 0; k < x->nkeys && k < AN_BT_KEYS; k++) if (x->keys[k].flags & AN_BTK_CONNECTED) { char nm[40]; bt_name(x->keys[k].name, nm, sizeof nm); if (n++) bput(&b, ", "); bjson(&b, nm); }
            bput(&b, "]}");
        } else bput(&b, ",\n \"bt\": null");
    }
    bput(&b, ",\n \"once\": "); j_once(&b);                  /* the one-shot debug start (docs/08 item 122) */
    bprintf(&b, ", \"diag\": {\"buscheck\": \"%s\", \"screen\": \"%s\"}", psmac_buscheck_request_state() == 2 || psmac_buscheck_request_state() == 1 ? "running" : psmac_buscheck_request_state() == 3 ? "done" : "idle",
            diag_screen == 2 ? "report" : diag_screen == 1 ? "check" : "recovery");
    bput(&b, "}\n");
    respond(h, 200, "application/json", j);
}
/* the secrets of the line shown as *** come back as they were on the card */
static void cl_unsecret(char *t, uint32_t *n)
{
    static const char *const keys[] = { "wifi.psk", "remote.token" };
    for (unsigned i = 0; i < 2; i++) {
        char v[96], old[96];
        if (pcl_get(t, *n, keys[i], v, sizeof v) != 1 || !starts(v, "***")) continue;
        if (cl_len > 0 && pcl_get(cl_old, (uint32_t)cl_len, keys[i], old, sizeof old) == 1) {
            static char tmp[CL_MAX + 1];
            int k = pcl_set(t, *n, tmp, CL_MAX, keys[i], old);
            if (k >= 0) { for (int j = 0; j < k; j++) t[j] = tmp[j]; t[k] = 0; *n = (uint32_t)k; }
        }
    }
}
static void ans_text_get(struct rconn *h, int c, const char *name)
{
    if (!ieq(name, "cmdline.txt") && !ieq(name, "config.txt")) { json_err(h, 400, "name is cmdline.txt or config.txt"); return; }
    if (!take_big(c)) { json_err(h, 503, "busy: try again"); return; }
    int n = psmac_card_rec_get(name, (uint8_t *)big, sizeof big - 1);
    if (n < 0) { big_conn = -1; json_err(h, 404, "not on the card"); return; }
    big[n] = 0;
    if (ieq(name, "cmdline.txt")) { static char r[CL_MAX + 512]; redacted(big, r, sizeof r); scpy(big, r, sizeof big); n = (int)slen(big); }
    respond_src(h, "text/plain; charset=iso-8859-1", big, (uint32_t)n);
}
static void ans_text_put(struct rconn *h, const char *name, const char *body, uint32_t n)
{
    char why_[200];
    if (ieq(name, "cmdline.txt")) {
        if (n > CL_MAX) { json_err(h, 413, "longer than 2 KB"); return; }
        cl_read();
        for (uint32_t i = 0; i < n; i++) cl_new[i] = body[i];
        cl_new[n] = 0;
        cl_unsecret(cl_new, &n);
        if (cl_write(n, why_, sizeof why_)) { json_err(h, 422, why_); return; }
        did("cmdline.txt edited on the page (%u bytes; the line before in cmdline.bak)", n);
        char j[200]; struct sb b = { j, 0, sizeof j }; bprintf(&b, "{\"saved\": \"cmdline.txt\", \"size\": %u, \"backup\": \"cmdline.bak\"}\n", n);
        respond(h, 200, "application/json", j);
        return;
    }
    if (ieq(name, "config.txt")) {
        for (uint32_t i = 0; i < n; i++) { unsigned char ch = (unsigned char)body[i]; if ((ch < 32 && ch != '\n' && ch != '\r' && ch != '\t') || ch > 126) { json_err(h, 422, "config.txt holds plain ASCII text only"); return; } }
        static uint8_t old[16384];
        int on = psmac_card_rec_get("config.txt", old, sizeof old);
        if (on > 0 && psmac_card_rec_put("config.bak", old, (uint32_t)on)) { json_err(h, 500, "config.bak could not be written"); return; }
        if (psmac_card_rec_put("config.txt", (const uint8_t *)body, n)) { json_err(h, 500, psmac_card_rec_error()); return; }
        did("config.txt edited on the page (%u bytes; the one before in config.bak)", n);
        char j[200]; struct sb b = { j, 0, sizeof j }; bprintf(&b, "{\"saved\": \"config.txt\", \"size\": %u, \"backup\": \"config.bak\"}\n", n);
        respond(h, 200, "application/json", j);
        return;
    }
    json_err(h, 400, "name is cmdline.txt or config.txt");
}
static void j_defs(struct sb *b);
static void ans_options(struct rconn *h, int c, int post, int dry, char *body, uint32_t n)
{
    char why_[200];
    if (cl_read()) { json_err(h, 404, "no cmdline.txt on the card"); return; }
    uint32_t nn = (uint32_t)cl_len;
    for (uint32_t i = 0; i < nn; i++) cl_new[i] = cl_old[i];
    cl_new[nn] = 0;
    if (post) {
        /* the edits: one a line, "key=value" set, "-key" taken out, "+word" a bare word; a value "***" leaves a secret as it is */
        body[n] = 0;
        for (char *l = body; *l; ) {
            char *e = l; while (*e && *e != '\n' && *e != '\r') e++;
            char ce = *e; *e = 0;
            if (*l) {
                int r = 0;
                if (*l == '-') r = cl_set(&nn, l + 1, 0);
                else if (*l == '+') r = cl_set(&nn, l + 1, PCL_FLAG);
                else {
                    char *eq = l; while (*eq && *eq != '=') eq++;
                    if (!*eq) { json_err(h, 400, "an edit is key=value, -key or +word"); return; }
                    *eq = 0;
                    const char *v = eq + 1;
                    if (!(is_secret(l) && starts(v, "***"))) {
                        if (!pdg_value_ok(pdg_find(l), v, why_, sizeof why_)) { json_err(h, 422, why_); return; }
                        if (ieq(l, "wifi.country") && !country_ok(v, why_, sizeof why_)) { json_err(h, 422, why_); return; }
                        if (ieq(l, "remote.token") && slen(v) < 8) { json_err(h, 422, "remote.token is 8 characters or more"); return; }
                        r = v[0] ? cl_set(&nn, l, v) : cl_set(&nn, l, 0);
                    }
                }
                if (r == -2) { json_err(h, 422, "a value with a quote or a control character in it"); return; }
                if (r) { json_err(h, 413, "the line would be longer than 2 KB"); return; }
            }
            if (!ce) break;
            l = e + 1;
        }
        if (pcl_check_line(cl_new, nn, why_, sizeof why_)) { json_err(h, 422, why_); return; }
        if (!dry) {
            if (cl_write(nn, why_, sizeof why_)) { json_err(h, 500, why_); return; }
            did("cmdline.txt: options changed on the page (the line before in cmdline.bak)");
        }
    }
    if (!take_big(c)) { json_err(h, 503, "busy: try again"); return; }
    struct sb b = { big, 0, sizeof big };
    static char r[CL_MAX + 512];
    redacted(cl_new, r, sizeof r);
    bput(&b, "{\"line\": "); bjson(&b, r);
    char bv[16] = ""; pcl_get(cl_new, nn, "mac.board", bv, sizeof bv);
    int board = pdg_board(bv);
    bprintf(&b, ", \"saved\": %s, \"board\": \"%s\", \"values\": {", post && !dry ? "true" : "false", pdg_board_word(board));
    for (unsigned i = 0; i < pdg_nopts; i++) {
        char v[96]; int k = pcl_get(cl_new, nn, pdg_opts[i].key, v, sizeof v);
        bput(&b, i ? ", " : ""); bjson(&b, pdg_opts[i].key); bput(&b, ": ");
        if (!k) bput(&b, "null");
        else if (is_secret(pdg_opts[i].key)) bput(&b, "\"(set)\"");
        else bjson(&b, k == 2 ? "(flag)" : v);
    }
    /* options in the line that the chosen board does not use (they are kept: a board may change back) */
    bput(&b, "}, \"unused\": [");
    for (unsigned i = 0, k = 0; i < pdg_nopts; i++) {
        char v[8];
        if (!pdg_uses(&pdg_opts[i], board) && pcl_get(cl_new, nn, pdg_opts[i].key, v, sizeof v)) { bput(&b, k++ ? ", " : ""); bjson(&b, pdg_opts[i].key); }
    }
    bput(&b, "],\n ");
    j_defs(&b);
    bput(&b, "}\n");
    respond_src(h, "application/json", big, b.n);
}
/* the table itself, for the page: the groups, and each option's group, boards, flags, values, example and text */
static void j_defs(struct sb *b)
{
    bput(b, "\"groups\": [");
    for (int g = 0; g < PDG_GROUPS; g++) { bput(b, g ? ", " : ""); bjson(b, pdg_group_name[g]); }
    bput(b, "], \"defs\": [");
    for (unsigned i = 0; i < pdg_nopts; i++) {
        const struct pdg_opt *o = &pdg_opts[i];
        bput(b, i ? ",\n  " : "\n  "); bput(b, "{\"key\": "); bjson(b, o->key);
        bprintf(b, ", \"group\": %u, \"boards\": \"", o->group);
        for (int k = 0, first = 1; k < 7; k++) if (o->boards & (1 << k)) { bput(b, first ? "" : " "); bput(b, pdg_board_word(1 << k)); first = 0; }
        bprintf(b, "\", \"menu\": %s, \"secret\": %s, \"wifitab\": %s, \"once\": %s, \"values\": ", pdg_choices(o) ? "true" : "false",
                o->flags & PDG_F_SECRET ? "true" : "false", o->flags & PDG_F_WIFITAB ? "true" : "false", o->flags & PDG_F_ONCE ? "true" : "false");
        bjson(b, o->values); bput(b, ", \"example\": "); bjson(b, o->example ? o->example : ""); bput(b, ", \"text\": "); bjson(b, o->text); bput(b, "}");
    }
    bput(b, "]");
}
static void form_get(const char *body, const char *key, char *out, unsigned max) { if (!query(body, key, out, max)) out[0] = 0; }
static void ans_wifi(struct rconn *h, const char *sub, char *body, uint32_t n)
{
    char why_[200], ssid[40], psk[72], country[8];
    body[n] = 0;
    volatile struct an_unit *u = unit(0);
    if (!sub[0]) {
        static char j[8192]; struct sb b = { j, 0, sizeof j };
        cl_read();
        char s[40] = "", c[8] = "", p[72] = "";
        pcl_get(cl_old, (uint32_t)cl_len, "wifi.ssid", s, sizeof s); pcl_get(cl_old, (uint32_t)cl_len, "wifi.country", c, sizeof c);
        int ps = pcl_get(cl_old, (uint32_t)cl_len, "wifi.psk", p, sizeof p) == 1 && p[0];
        bput(&b, "{\"settings\": {\"ssid\": "); bjson(&b, s); bprintf(&b, ", \"psk\": \"%s\", \"country\": ", ps ? "set" : "not set"); bjson(&b, c); bput(&b, "}");
        if (u && u->kind) {
            char m[100]; unsigned k = 0; for (; k < 95 && u->msg[k]; k++) m[k] = u->msg[k];
            m[k] = 0;
            bprintf(&b, ", \"radio\": {\"state\": \"%s\", \"message\": ", st_name(u->state)); bjson(&b, m); bput(&b, "}, \"scan\": [");
            volatile struct an_scan *sc = (volatile struct an_scan *)((volatile uint8_t *)alice_net_board() + AN_OFF_SCAN);
            for (uint32_t i = 0; i < u->scan_count && i < 32; i++) {
                char ss[40]; unsigned q = 0; for (; q < 32 && sc[i].ssid[q]; q++) ss[q] = sc[i].ssid[q];
                ss[q] = 0;
                bput(&b, i ? ", " : ""); bput(&b, "{\"ssid\": "); bjson(&b, ss);
                bprintf(&b, ", \"rssi\": %d, \"channel\": %u, \"security\": \"%s\"}", sc[i].rssi, sc[i].channel, sc[i].security & 0x08 ? "WPA2" : sc[i].security & 0x04 ? "WPA" : sc[i].security & 0x02 ? "WEP" : "open");
            }
            bput(&b, "]");
        } else bput(&b, ", \"radio\": null, \"scan\": []");
        bprintf(&b, ", \"test\": %s, \"note\": ", wifi_test ? "\"running\"" : wifi_test_ok ? "\"joined\"" : "\"\""); bjson(&b, wifi_note);
        bput(&b, "}\n");
        respond(h, 200, "application/json", j);
        return;
    }
    if (!u || !u->kind) { json_err(h, 503, "no WiFi on this Pi (or wifi=off)"); return; }
    if (starts(sub, "scan")) {
        int r = wifi_cmd(AN_CMD_SCAN, 0, 0);
        if (r) { json_err(h, 503, r == -2 ? "the radio is busy with the last command: try again" : "no radio"); return; }
        did("WiFi: scanning");
        respond(h, 200, "application/json", "{\"scanning\": true}\n");
        return;
    }
    form_get(body, "ssid", ssid, sizeof ssid); form_get(body, "psk", psk, sizeof psk); form_get(body, "country", country, sizeof country);
    for (char *p = country; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    if (!ssid[0] || slen(ssid) > 32) { json_err(h, 422, "the network's name (SSID) is 1 to 32 characters"); return; }
    if (psk[0] && (slen(psk) < 8 || slen(psk) > 64)) { json_err(h, 422, "a WPA password is 8 to 63 characters (or 64 hex digits)"); return; }
    if (!country_ok(country, why_, sizeof why_)) { json_err(h, 422, why_); return; }
    char open_[8]; form_get(body, "open", open_, sizeof open_);
    cl_read();
    if (!psk[0] && open_[0] != '1') {                            /* the stored password, when the network is the same */
        char s[40] = ""; pcl_get(cl_old, (uint32_t)cl_len, "wifi.ssid", s, sizeof s);
        if (!__builtin_strcmp(s, ssid)) pcl_get(cl_old, (uint32_t)cl_len, "wifi.psk", psk, sizeof psk);
    }
    if (starts(sub, "test")) {
        if (ap_active()) { json_err(h, 409, "the radio is the access point you are on: a test join would end it. Save the network and restart to join it"); return; }
        if (wifi_join(ssid, psk, country)) { json_err(h, 503, "the radio is busy: try again"); return; }
        wifi_test = 1; wifi_test_ok = 0; wifi_test_t0 = now_ms(); scpy(wifi_test_ssid, ssid, sizeof wifi_test_ssid); wifi_note[0] = 0;
        did("WiFi test: joining %s", ssid);
        respond(h, 200, "application/json", "{\"testing\": true}\n");
        return;
    }
    if (starts(sub, "save")) {
        uint32_t nn = (uint32_t)cl_len;
        for (uint32_t i = 0; i < nn; i++) cl_new[i] = cl_old[i];
        cl_new[nn] = 0;
        if (cl_set(&nn, "wifi.ssid", ssid) || cl_set(&nn, "wifi.psk", psk[0] ? psk : 0) || cl_set(&nn, "wifi.country", country[0] ? country : 0)) { json_err(h, 422, "the settings cannot be written into the line"); return; }
        { char v[8]; if (pcl_get(cl_new, nn, "wifi", v, sizeof v) == 1 && ieq(v, "off")) cl_set(&nn, "wifi", 0); }
        if (cl_write(nn, why_, sizeof why_)) { json_err(h, 500, why_); return; }
        did("WiFi settings saved for %s (the line before in cmdline.bak)", ssid);
        respond(h, 200, "application/json", "{\"saved\": true, \"backup\": \"cmdline.bak\"}\n");
        return;
    }
    json_err(h, 404, "POST /r/wifi/scan, /r/wifi/test or /r/wifi/save");
}
static void ans_reboot(struct rconn *h, const char *how)
{
    char why_[200];
    if (how && starts(how, "recovery")) {
        static const char t[] = "asked for from the recovery page\n";
        if (psmac_card_rec_put("recovery.txt", (const uint8_t *)t, sizeof t - 1)) { json_err(h, 500, psmac_card_rec_error()); return; }
        did("restart into recovery asked for: recovery.txt written");
        respond(h, 200, "application/json", "{\"restart\": \"recovery\"}\n");
    } else {
        int took = 0;
        (void)why_;
        if (cmdline_has_recovery()) {                            /* "normally" means without mac.recovery=1 and recovery.ap= */
            if (cl_drop_recovery()) { json_err(h, 500, "mac.recovery=1 / recovery.ap could not be taken out of cmdline.txt"); return; }
            took = 1;
        }
        did("restart normally asked for%s", took ? ": mac.recovery=1 / recovery.ap taken out of cmdline.txt (cmdline.bak keeps the line)" : "");
        respond(h, 200, "application/json", took ? "{\"restart\": \"normal\", \"note\": \"mac.recovery=1 / recovery.ap was taken out of cmdline.txt\"}\n" : "{\"restart\": \"normal\"}\n");
    }
    pi_restart_in(1500);
}
static void ans_rollback(struct rconn *h, int force)
{
    char why_[200], info[200];
    struct rec_ent o;
    if (psmac_card_rec_find("kernel8.old", &o)) { json_err(h, 404, "there is no kernel8.old on the card"); return; }
    if (!force) {
        uint32_t max;
        uint8_t *buf = kernel_buf(&max);
        if (!buf || o.size > max) { json_err(h, 503, "no room to check kernel8.old here (force=1 rolls back unchecked)"); return; }
        int n = psmac_card_rec_get("kernel8.old", buf, max);
        if (n < 0) { json_err(h, 500, "kernel8.old could not be read"); return; }
        if (kernel_check(buf, (uint32_t)n, why_, sizeof why_, info, sizeof info)) { struct sb b = { info, 0, sizeof info }; bprintf(&b, "kernel8.old does not check: %s (force=1 rolls back all the same)", why_); json_err(h, 422, info); return; }
    }
    if (psmac_card_rec_swap("kernel8.img", "kernel8.old")) { json_err(h, 500, psmac_card_rec_error()); return; }
    did("rolled back: kernel8.old is kernel8.img again, and the kernel it replaced is kernel8.old");
    respond(h, 200, "application/json", "{\"rolled_back\": true, \"note\": \"kernel8.img and kernel8.old changed places\"}\n");
}
static void ans_log(struct rconn *h, int c, int now)
{
    if (now) {
        if (!take_big(c)) { json_err(h, 503, "busy: try again"); return; }
        static uint8_t raw[PSMB_LOG_SIZE];
        uint32_t n = psmac_log_copy(raw, sizeof raw), s = 0;
        if (n == sizeof raw) { while (s < n && raw[s] != '\n') s++; if (s < n) s++; }
        static char t[PSMB_LOG_SIZE + 1];
        for (uint32_t i = s; i < n; i++) t[i - s] = (char)raw[i];
        t[n - s] = 0;
        psmac_redact(t, big, sizeof big);
        respond_src(h, "text/plain; charset=iso-8859-1", big, slen(big));
        return;
    }
    struct rec_ent e;
    if (psmac_card_rec_find("aurora68.log", &e)) { json_err(h, 404, "no aurora68.log on the card: the SE board saves it every 30 s; other boards after a crash, or from Aurora68 Log"); return; }
    out_head(h, 200, "text/plain; charset=iso-8859-1", e.size, 0);
    psmac_card_rec_read_open(&e, &h->rd); h->kind = RK_FILE;
}

/* ============================================================================== the Display tab
   22 November 2026 (docs/08 item 111). The HDMI monitor's EDID and the mode the Pi sends it, chosen from the page
   as Aurora68 Display chooses it on the Mac (docs/10 "Aurora68 Display"), for a Pi whose Mac does not start or
   whose monitor shows nothing. A recovery start reads and parses the EDID like any other (psmac_display_init calls
   psmac_hdmi_init on core 0 before core 0 parks), so the monitor's list is psmac_hdmi.c's; without an EDID (no
   monitor, or QEMU without mac.edidtest) the standard modes psmac_edid.c knows by number are offered instead. The
   file is written by psmac_edid.c's writer, the old one kept as aurora68-display.bak, as Aurora68 Display does; what
   Aurora68 Display can only tell the user to do on another computer, the include line at the end of config.txt, the
   page can do (config.bak keeping the old config.txt). The firmware reads config.txt only when the Pi starts, so the
   choice is in force from "Restart normally". */
const struct psmb_dispinfo *psmac_hdmi_info(void);
const char *psmac_hdmi_default_text(void);
static struct psmb_dispinfo std_di, file_di;
static int std_made;
static char d_cfg[16384], d_txt[1536], d_new[1536], d_cfgnew[16384 + 256];
static int d_cfg_len = -1, d_txt_len = -1, d_bak_len = -1;
/* the list to choose from: the monitor's, else the standard one (made once, and only then: psmac_edid.c keeps
   each mode's timing for its writer in one table, the last list's) */
static const struct psmb_dispinfo *disp_list(int *standard)
{
    const struct psmb_dispinfo *d = psmac_hdmi_info();
    *standard = 0;
    if (d->nmodes) return d;
    if (!std_made) { std_made = 1; edid_standard_modes(&std_di, d->pi_pixclk); }
    *standard = 1;
    return &std_di;
}
/* config.txt, aurora68-display.txt and whether the .bak is there, read now; and what they set for the next start
   (file_di: the include line, the mode's lines, the rotation, the confirm mark) */
static int disp_read(int *rot, int *confirm)
{
    d_cfg_len = psmac_card_rec_get("config.txt", (uint8_t *)d_cfg, sizeof d_cfg - 1);
    if (d_cfg_len < 0) d_cfg_len = -1; else d_cfg[d_cfg_len] = 0;
    d_txt_len = psmac_card_rec_get("aurora68-display.txt", (uint8_t *)d_txt, sizeof d_txt - 1);
    if (d_txt_len < 0) d_txt_len = -1; else d_txt[d_txt_len] = 0;
    struct rec_ent e;
    d_bak_len = psmac_card_rec_find("aurora68-display.bak", &e) ? -1 : (int)e.size;
    for (unsigned i = 0; i < sizeof file_di; i++) ((uint8_t *)&file_di)[i] = 0;
    struct edid_file f = { d_cfg_len >= 0 ? d_cfg : 0, d_cfg_len >= 0 ? (uint32_t)d_cfg_len : 0, d_txt_len >= 0 ? d_txt : 0, d_txt_len >= 0 ? (uint32_t)d_txt_len : 0 };
    *rot = edid_current(&file_di, &f, confirm);
    return 0;
}
static void bhz(struct sb *b, uint32_t c) { bprintf(b, "%u.%u%u", c / 100, c % 100 / 10, c % 10); }               /* centihertz as 59.95 */
static void bmhz(struct sb *b, uint32_t k) { uint32_t f = k % 1000 / 10; bprintf(b, "%u.%u%u", k / 1000, f / 10, f % 10); }   /* kHz as 241.50 */
static void btenth(struct sb *b, uint32_t t) { bprintf(b, "%u.%u", t / 10, t % 10); }
static uint32_t isqrt32(uint32_t v) { uint32_t r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }
static uint32_t refresh_of(const struct psmb_dispmode *m) { return !(m->flags & PSMB_DM_USABLE) && (m->flags & PSMB_DM_ALT) ? m->alt_refresh : m->refresh; }
static void use_text(struct sb *b, const struct psmb_dispmode *m) { bprintf(b, "\"%ux%u@%u\"", m->width, m->height, (refresh_of(m) + 50) / 100); }
static void j_monitor(struct sb *b, const struct psmb_dispinfo *d)
{
    if (!(d->flags & PSMB_DI_EDID)) { bput(b, "null"); return; }
    bput(b, "{\"name\": "); bjson(b, d->name); bput(b, ", \"maker\": "); bjson(b, d->maker); bput(b, ", \"maker_name\": "); bjson(b, d->maker_name);
    bprintf(b, ", \"product\": \"%x\"", d->product);
    /* the serial number shortened: its last four characters, enough to tell two monitors apart, not to name one */
    char sh[16] = ""; unsigned n = slen(d->serial_text);
    if (n) { unsigned k = n > 4 ? 4 : (n + 1) / 2; struct sb s = { sh, 0, sizeof sh }; bput(&s, "..."); bput(&s, d->serial_text + n - k); }
    else if (d->serial) scpy(sh, "(a number, not shown)", sizeof sh);
    bput(b, ", \"serial\": "); bjson(b, sh);
    bprintf(b, ", \"week\": %u, \"year\": %u, \"edid_version\": \"%u.%u\", \"blocks\": %u, \"checksum_ok\": %s, \"sample\": %s",
            d->week, d->year, d->edid_version >> 8, d->edid_version & 255, d->edid_blocks, d->edid_status == PSMB_ES_CHECKSUM ? "false" : "true",
            d->flags & PSMB_DI_SAMPLE ? "true" : "false");
    uint32_t w = d->size_w_mm, h = d->size_h_mm, dg = isqrt32(w * w + h * h);
    bprintf(b, ",\n  \"size_mm\": [%u, %u], \"size_in\": {\"w\": ", w, h); btenth(b, w * 100 / 254); bput(b, ", \"h\": "); btenth(b, h * 100 / 254);
    bput(b, ", \"diagonal\": "); btenth(b, dg * 100 / 254); bput(b, "}");
    for (int i = 0; i < d->nmodes; i++)
        if (d->mode[i].flags & PSMB_DM_NATIVE) { bprintf(b, ", \"native\": \"%ux%u\", \"shape\": \"%u:%u\"", d->mode[i].width, d->mode[i].height, d->mode[i].aspect_w, d->mode[i].aspect_h); break; }
    bprintf(b, ", \"connection\": \"%s\"", d->conn & PSMB_CN_HDMI ? "HDMI" : "DVI");
    char snd[80] = ""; struct sb s = { snd, 0, sizeof snd };
    if (!(d->conn & PSMB_CN_HDMI)) bput(&s, "none (DVI carries no sound)");
    else if (!(d->audio_formats & 2)) bput(&s, d->conn & PSMB_CN_BASICAUDIO ? "basic HDMI sound" : "none listed");
    else {
        static const char *const rates[7] = { "32", "44.1", "48", "88.2", "96", "176.4", "192" };
        bprintf(&s, "PCM, %u channel%s, ", d->lpcm_channels, d->lpcm_channels == 1 ? "" : "s");
        int first = 1;
        for (int i = 0; i < 7; i++) if (d->lpcm_rates & (1u << i)) { bput(&s, first ? "" : "/"); bput(&s, rates[i]); first = 0; }
        bput(&s, " kHz");
    }
    bput(b, ", \"sound\": "); bjson(b, snd);
    bput(b, ", \"max_mhz\": "); if (d->max_pixclk) bmhz(b, d->max_pixclk); else bput(b, "null");
    if (d->min_vhz) bprintf(b, ", \"range\": \"%u-%u Hz, %u-%u kHz\"", d->min_vhz, d->max_vhz, d->min_hkhz, d->max_hkhz);
    bput(b, "}");
}
static const char *verdict(const struct psmb_dispmode *m)
{
    if (m->flags & PSMB_DM_INTERLACED) return "interlaced";
    if (m->flags & PSMB_DM_MACSIZE) return "mac_size";
    if (m->flags & PSMB_DM_USABLE) return "usable";
    return m->flags & PSMB_DM_ALT ? "alternative" : "too_fast";
}
static void ans_display_get(struct rconn *h, int c)
{
    if (!take_big(c)) { json_err(h, 503, "busy: try again"); return; }
    int standard, frot = 0, fconf = 0;
    const struct psmb_dispinfo *hd = psmac_hdmi_info(), *d = disp_list(&standard);
    int ready = psmac_card_rec_ready();
    if (ready == 1 || ready == -2) disp_read(&frot, &fconf);
    else { d_cfg_len = d_txt_len = d_bak_len = -1; for (unsigned i = 0; i < sizeof file_di; i++) ((uint8_t *)&file_di)[i] = 0; }
    struct sb b = { big, 0, sizeof big };
    static const char *const es[] = { "ok", "none", "header", "checksum" };
    bprintf(&b, "{\"edid\": %s, \"edid_status\": \"%s\", \"standard\": %s, \"pi_mhz\": ", hd->flags & PSMB_DI_EDID ? "true" : "false",
            hd->edid_status < 4 ? es[hd->edid_status] : "?", standard ? "true" : "false");
    bmhz(&b, d->pi_pixclk);
    bput(&b, ",\n \"monitor\": "); j_monitor(&b, hd);
    /* the mode in force: the firmware's frame buffer at this start, and the lines that set it as the start read them */
    static const char *const src[] = { "the firmware's own choice", "config.txt", "aurora68-display.txt" };
    bprintf(&b, ",\n \"now\": {\"w\": %u, \"h\": %u, \"hz\": ", hd->cur_w, hd->cur_h);
    if (hd->cur_refresh) bhz(&b, hd->cur_refresh); else bput(&b, "null");
    bprintf(&b, ", \"source\": \"%s\", \"text\": ", hd->cur_source < 3 ? src[hd->cur_source] : "?"); bjson(&b, hd->cur_text);
    bprintf(&b, ", \"rotation\": %u, \"confirm_pending\": %s}", hd->rotation, hd->confirm_state == PSMB_CS_PENDING ? "true" : "false");
    bput(&b, ",\n \"preferred\": ");
    { int k = -1; for (int i = 0; i < d->nmodes && k < 0; i++) if (d->mode[i].flags & PSMB_DM_PREFERRED) k = i;
      if (k < 0) bput(&b, "null"); else { bprintf(&b, "{\"w\": %u, \"h\": %u, \"hz\": ", d->mode[k].width, d->mode[k].height); bhz(&b, d->mode[k].refresh); bput(&b, "}"); } }
    bput(&b, ",\n \"suggested\": ");
    if (d->suggested == 0xffff || d->suggested >= d->nmodes) bput(&b, "null");
    else {
        const struct psmb_dispmode *m = &d->mode[d->suggested];
        bprintf(&b, "{\"i\": %u, \"w\": %u, \"h\": %u, \"hz\": ", d->suggested, m->width, m->height); bhz(&b, refresh_of(m));
        bprintf(&b, ", \"alt\": %s, \"use\": ", m->flags & PSMB_DM_USABLE ? "false" : "true"); use_text(&b, m);
        bput(&b, ", \"reason\": "); bjson(&b, d->reason); bput(&b, "}");
    }
    bput(&b, ",\n \"modes\": [");
    for (int i = 0; i < d->nmodes && b.n + 600 < b.cap; i++) {
        const struct psmb_dispmode *m = &d->mode[i];
        bprintf(&b, "%s\n  {\"i\": %d, \"w\": %u, \"h\": %u, \"hz\": ", i ? "," : "", i, m->width, m->height); bhz(&b, m->refresh);
        bput(&b, ", \"mhz\": "); bmhz(&b, m->pixclk);
        bprintf(&b, ", \"verdict\": \"%s\"", verdict(m));
        if (m->flags & PSMB_DM_ALT) { bput(&b, ", \"alt_hz\": "); bhz(&b, m->alt_refresh); bput(&b, ", \"alt_mhz\": "); bmhz(&b, m->alt_pixclk); }
        bprintf(&b, ", \"choose\": %s", edid_mode_choosable(m) ? "true" : "false");
        if (edid_mode_choosable(m)) { bput(&b, ", \"use\": "); use_text(&b, m); }
        bprintf(&b, ", \"aspect\": \"%u:%u\", \"source\": \"%s\", \"config\": \"hdmi_group=%u hdmi_mode=%u\"", m->aspect_w, m->aspect_h, edid_source_name(m->source), m->cfg_group, m->cfg_mode);
        static const struct { uint16_t f; const char *k; } fl[] = { { PSMB_DM_PREFERRED, "preferred" }, { PSMB_DM_NATIVE, "native" }, { PSMB_DM_CURRENT, "current" },
                                                                     { PSMB_DM_SUGGESTED, "suggested" }, { PSMB_DM_SHAPE, "shape" } };
        for (unsigned k = 0; k < sizeof fl / sizeof fl[0]; k++) if (m->flags & fl[k].f) bprintf(&b, ", \"%s\": true", fl[k].k);
        bput(&b, "}");
    }
    bput(&b, "],\n \"file\": {\"present\": "); bput(&b, d_txt_len >= 0 ? "true" : "false");
    bprintf(&b, ", \"bak\": %s, \"text\": ", d_bak_len >= 0 ? "true" : "false"); bjson(&b, d_txt_len >= 0 ? d_txt : "");
    { uint32_t fw = 0, fh = 0; struct edid_file f = { d_cfg_len >= 0 ? d_cfg : 0, d_cfg_len >= 0 ? (uint32_t)d_cfg_len : 0, d_txt_len >= 0 ? d_txt : 0, d_txt_len >= 0 ? (uint32_t)d_txt_len : 0 };
      edid_config_size(&f, &fw, &fh);
      bprintf(&b, ", \"next\": {\"w\": %u, \"h\": %u, \"hz\": ", fw, fh); if (file_di.cur_refresh) bhz(&b, file_di.cur_refresh); else bput(&b, "null");
      bprintf(&b, ", \"source\": \"%s\", \"text\": ", file_di.cur_source < 3 ? src[file_di.cur_source] : "?"); bjson(&b, file_di.cur_text); bput(&b, "}"); }
    bprintf(&b, ", \"rotation\": %d, \"confirm\": %s}", frot, fconf ? "true" : "false");
    bprintf(&b, ",\n \"config\": {\"read\": %s, \"include\": %s, \"override\": %s}", d_cfg_len >= 0 ? "true" : "false",
            file_di.cfg & PSMB_CF_INCLUDE ? "true" : "false", file_di.cfg & PSMB_CF_OVERRIDE ? "true" : "false");
    char v[24] = ""; int ms = cl_read() == 0 && pcl_get(cl_old, (uint32_t)cl_len, "mac.screen", v, sizeof v) == 1;
    bput(&b, ",\n \"cmdline\": {\"mac_screen\": "); if (ms) bjson(&b, v); else bput(&b, "null"); bput(&b, "}");
    bprintf(&b, ", \"writable\": %s, \"board\": \"%s\"}\n", ready == 1 ? "true" : "false", psmac_board_solo ? "solo" : psmac_board_plus ? "plus" : psmac_board_se ? "se" : "iici");
    respond_src(h, "application/json", big, b.n);
}
/* "2560x1440", "2560x1440@30": 0, or -1 */
static int parse_mode(const char *s, uint32_t *w, uint32_t *h, uint32_t *hz)
{
    uint32_t v[3] = { 0, 0, 0 }; int k = 0;
    for (; k < 3; k++) {
        const char *s0 = s; while (*s >= '0' && *s <= '9') v[k] = v[k] * 10 + (uint32_t)(*s++ - '0');
        if (s == s0 || v[k] > 100000) return -1;
        if (!*s) break;
        if ((k == 0 && *s != 'x' && *s != 'X') || (k == 1 && *s != '@') || k == 2) return -1;
        s++;
    }
    if (k < 1) return -1;
    *w = v[0]; *h = v[1]; *hz = v[2];
    return 0;
}
static int put_checked(const char *name, const char *t, uint32_t n, char *why_, unsigned whymax)
{
    if (psmac_card_rec_put(name, (const uint8_t *)t, n)) { struct sb b = { why_, 0, whymax }; bprintf(&b, "%s could not be written: %s", name, psmac_card_rec_error()); return -1; }
    static char back[sizeof d_cfgnew];
    int k = psmac_card_rec_get(name, (uint8_t *)back, sizeof back);
    int same = k == (int)n;
    for (uint32_t i = 0; same && i < n; i++) same = back[i] == t[i];
    if (!same) { struct sb b = { why_, 0, whymax }; bprintf(&b, "%s read back from the card is not what was written", name); return -1; }
    return 0;
}
static void ans_display_post(struct rconn *h, int c, char *body, uint32_t n)
{
    char why_[200], mv[40], rv[8], iv[4], cv[4], sv[12];
    body[n] = 0;
    if (psmac_card_rec_ready() != 1) { json_err(h, 503, "no writable card"); return; }
    if (!take_big(c)) { json_err(h, 503, "busy: try again"); return; }     /* the answer carries the file's text */
    form_get(body, "mode", mv, sizeof mv); form_get(body, "rotation", rv, sizeof rv); form_get(body, "include", iv, sizeof iv);
    form_get(body, "confirm", cv, sizeof cv); form_get(body, "screen", sv, sizeof sv);
    int standard, frot = 0, fconf = 0;
    const struct psmb_dispinfo *d = disp_list(&standard);
    disp_read(&frot, &fconf);
    int rot = frot, want_mode = mv[0] != 0, is_auto = ieq(mv, "auto"), idx = -1, alt = 0, add_inc = iv[0] == '1', confirm = cv[0] ? cv[0] == '1' : 1;
    uint32_t w = 0, hh = 0, hz = 0;
    if (rv[0]) {
        int r = 0; const char *p = rv; while (*p >= '0' && *p <= '9') r = r * 10 + (*p++ - '0');
        if (*p || (r != 0 && r != 90 && r != 180 && r != 270)) { json_err(h, 422, "rotation is 0, 90, 180 or 270"); return; }
        rot = r;
    }
    if (want_mode && !is_auto) {
        if (parse_mode(mv, &w, &hh, &hz)) { json_err(h, 400, "mode is WxH, WxH@Hz or auto"); return; }
        idx = edid_find_mode(d, w, hh, hz, &alt);
        if (idx < 0) {
            struct sb b = { why_, 0, sizeof why_ };
            bprintf(&b, "%s is not a mode the Pi 3 can send to %s (GET /r/display lists them)", mv, standard ? "a monitor (the standard list)" : "this monitor");
            json_err(h, 422, why_); return;
        }
    }
    if (!want_mode && !rv[0] && !add_inc) { json_err(h, 400, "mode=WxH[@Hz] or auto, rotation=0|90|180|270, include=1"); return; }
    /* the firmware reads the file only through config.txt's include line, at its end; the rotation and the mark are
       the kernel's own and are read without it */
    int has_inc = (file_di.cfg & PSMB_CF_INCLUDE) != 0, over = (file_di.cfg & PSMB_CF_OVERRIDE) != 0;
    if (want_mode && d_cfg_len >= 0 && (!has_inc || over) && !add_inc) {
        json_err(h, 409, !has_inc ? "config.txt has no line \"include aurora68-display.txt\", so the firmware would not use the new mode: include=1 adds it at the end (config.bak keeps the old config.txt)"
                                  : "config.txt sets an hdmi_ mode after its include line, which wins: include=1 moves the line to the end (config.bak keeps the old config.txt)");
        return;
    }
    const char *inc = "";
    if (add_inc) {
        if (d_cfg_len < 0) { json_err(h, 404, "there is no config.txt on the card to add the line to"); return; }
        if (!has_inc || over) {
            uint32_t cn = edid_config_include(d_cfg, (uint32_t)d_cfg_len, d_cfgnew, sizeof d_cfgnew);
            if (!cn) { json_err(h, 413, "config.txt would be too long"); return; }
            if (d_cfg_len > 0 && put_checked("config.bak", d_cfg, (uint32_t)d_cfg_len, why_, sizeof why_)) { json_err(h, 500, why_); return; }
            if (put_checked("config.txt", d_cfgnew, cn, why_, sizeof why_)) { json_err(h, 500, why_); return; }
            inc = has_inc ? "moved" : "added";
            did("config.txt: the line \"include aurora68-display.txt\" %s at its end (config.bak keeps the one before)", inc);
        } else inc = "present";
    }
    uint32_t tn = 0;
    if (want_mode || rv[0]) {
        const char *base = d_txt_len >= 0 ? d_txt : psmac_hdmi_default_text();
        uint32_t bn = d_txt_len >= 0 ? (uint32_t)d_txt_len : slen(base);
        if (is_auto) tn = edid_display_auto(d, rot, confirm, d_new, sizeof d_new);
        else if (idx >= 0) tn = edid_display_text(d, idx, alt, rot, confirm, d_new, sizeof d_new);
        else tn = edid_display_rotate(base, bn, rot, d_new, sizeof d_new);
        if (put_checked("aurora68-display.bak", base, bn, why_, sizeof why_) || put_checked("aurora68-display.txt", d_new, tn, why_, sizeof why_)) { json_err(h, 500, why_); return; }
    }
    /* mac.screen= in cmdline.txt overrides the firmware's size for the Mac's screen (docs/10 "Any size": needed only to
       differ from the mode). Written down to the old size, it would keep the old size: it follows the new mode, or for
       Automatic, whose size is the firmware's to choose, it is taken out. Without it nothing is added. screen=keep leaves it. */
    char ms[80] = "";
    if (want_mode && !ieq(sv, "keep") && cl_read() == 0) {
        char v[24], nv[24] = ""; struct sb b = { nv, 0, sizeof nv };
        if (!is_auto) bprintf(&b, "%ux%u", d->mode[idx].width, d->mode[idx].height);
        if (pcl_get(cl_old, (uint32_t)cl_len, "mac.screen", v, sizeof v) == 1 && (is_auto || !ieq(v, nv))) {
            uint32_t nn = (uint32_t)cl_len;
            for (uint32_t i = 0; i < nn; i++) cl_new[i] = cl_old[i];
            cl_new[nn] = 0;
            if (cl_set(&nn, "mac.screen", is_auto ? 0 : nv) || cl_write(nn, why_, sizeof why_)) { json_err(h, 500, "the file is written, but mac.screen= could not be changed in cmdline.txt"); return; }
            struct sb m = { ms, 0, sizeof ms };
            if (is_auto) bprintf(&m, "mac.screen=%s taken out of cmdline.txt", v); else bprintf(&m, "mac.screen=%s in cmdline.txt (it was %s)", nv, v);
        }
    }
    if (want_mode && is_auto) did("Display: Automatic (hdmi_group=0, the firmware's choice), rotation %d%s", rot, confirm ? ", to be kept on the Mac" : "");
    else if (want_mode) did("Display: %ux%u at %u Hz%s, rotation %d: aurora68-display.txt written (the one before as .bak)", d->mode[idx].width, d->mode[idx].height,
                            (refresh_of(&d->mode[idx]) + 50) / 100, alt ? " (reduced blanking)" : "", rot);
    else if (rv[0]) did("Display: rotation %d kept in aurora68-display.txt", rot);
    if (ms[0]) did("Display: %s (cmdline.bak keeps the line before)", ms);
    struct sb b = { big, 0, sizeof big };
    bprintf(&b, "{\"saved\": %s, \"verified\": true", tn ? "true" : "false");
    if (tn) { bput(&b, ", \"file\": \"aurora68-display.txt\", \"backup\": \"aurora68-display.bak\", \"text\": "); bjson(&b, d_new); }
    if (want_mode && !is_auto) { bput(&b, ", \"mode\": "); use_text(&b, &d->mode[idx]); }
    else if (is_auto) bput(&b, ", \"mode\": \"auto\"");
    bprintf(&b, ", \"rotation\": %d, \"confirm\": %s, \"include\": \"%s\", \"mac_screen\": ", rot, confirm && want_mode ? "true" : "false", inc); bjson(&b, ms);
    bput(&b, ", \"note\": ");
    bjson(&b, !tn ? "config.txt saved" : want_mode ? (confirm ? "Restart normally to use it: the firmware reads config.txt only when the Pi starts. Aurora68 Screen Setup then asks on the Mac's screen whether to keep it; without an answer the old mode comes back."
                                                                : "Restart normally to use it: the firmware reads config.txt only when the Pi starts.")
                                     : "The rotation is in force from the next start.");
    bput(&b, "}\n");
    respond_src(h, "application/json", big, b.n);
}


/* ============================================================================== Diagnostics
   23 November 2026 (docs/08 item 122). Run now, with no Mac running: the bus check on the compact Macs (on the
   service core, which owns the transport: psmac_buscheck_request, its lines read here as they come), the card's
   read test (here), the CPLD's JTAG usercode read again (the service core). Run on the next start: debug-once.txt
   (psmac_recovery_once_early). The reports on the card, and one bundle of everything for a tester to send. */
int psmac_buscheck_request(int force, const char **why);
int psmac_buscheck_request_state(void), psmac_buscheck_request_count(void), psmac_buscheck_request_verdict(void);
const char *psmac_buscheck_request_line(int i), *psmac_buscheck_request_headline(void), *psmac_buscheck_refusal(int force);
const char *psmac_buscheck_text(uint32_t *len);
int psmac_buscheck_render(uint8_t *fb);
int psmac_buscheck_lines(const char (**out)[65]);
int psmac_buscheck_jtag_request(const char **why), psmac_buscheck_jtag_result(uint32_t *id, uint32_t *uc);
int psmac_gpio_cpld(uint32_t *id, uint32_t *uc);
const char *psmac_cpld_name(uint32_t uc);
int psmac_bus_uses_gpio(void);
uint32_t psmac_se_ram_bytes(void);
void psmac_console_page(const char (*lines)[65], int n);
int psmac_card_once_take(char *buf, uint32_t max, int take), psmac_card_once_rearm(void);
int psmac_card_early(void);
extern int psmac_board_orig;

static const char *board_model(void)
{
    static const char *const o[] = { "plus", "128k", "512k", "512ke" };
    return psmac_board_solo ? "solo" : psmac_board_plus ? o[psmac_board_orig & 3] : psmac_board_se ? "se" : "iici";
}
static const char *board_title(void)
{
    static const char *const o[] = { "Macintosh Plus", "Macintosh 128K", "Macintosh 512K", "Macintosh 512Ke" };
    return psmac_board_solo ? "the solo board (no Mac: the bench)" : psmac_board_plus ? o[psmac_board_orig & 3] : psmac_board_se ? "Macintosh SE" : "the testbed's Macintosh IIci";
}
static const char *verdict_word(int v) { static const char *const w[] = { "SKIP", "CLEAN", "INFO", "STRAY", "UNSURE", "FAIL" }; return v >= 0 && v < 6 ? w[v] : "?"; }
static const char *bootargs_now(void)
{
    of_node_t *c = dt_find_node("/chosen");
    of_property_t *p = c ? dt_find_property(c, "bootargs") : NULL;
    return p && p->op_value ? (const char *)p->op_value : "";
}

/* ---- the one-shot debug start, at the next start: core 0, first thing in psmac_setup_memory ---- */
static char once_args[4096];
void psmac_recovery_once_early(const void *initramfs, uint32_t len)
{
    const uint8_t *p = initramfs;
    const char *o = psmac_option("mac.cardearly");
    int container = len > 6 && p[0] == '0' && p[1] == '7' && p[2] == '0' && p[3] == '7' && p[4] == '0' && p[5] == '1';
    if (!container && !(o && o[0] == '1')) return;               /* a bare-ROM testbed run has no card to look at (psmac_hdmi_early's rule) */
    if (psmac_card_early()) return;
    char t[600];
    int n = psmac_card_once_take(t, sizeof t, 0);
    if (n < 0) return;
    /* a start that is going to be Setup and Recovery keeps it for the next normal one (phase 1's reasons; the
       programmer's switch, phase 2, is known only later: card_first_look puts the file back then) */
    o = psmac_option("mac.recovery"); int rec = o && o[0] == '1';
    o = psmac_option("recovery.ap"); if (o && ((o[0] == 'o' && o[1] == 'n') || o[0] == '1')) rec = 1;
    if (!rec) {
        int count = -1, fl = -1, lim = 3; char flag[8];
        if ((o = psmac_option("mac.bootcount"))) { if (o[0] == 'o') lim = 0; else { lim = 0; while (*o >= '0' && *o <= '9') lim = lim * 10 + (*o++ - '0'); } }
        if (!psmac_card_boot_read(&count, flag, sizeof flag, &fl) && (fl >= 0 || (lim && count >= lim))) rec = 1;
    }
    if (rec) { kprintf("[DEBUG] debug-once.txt is on the card: kept for the next normal start (this one is Setup and Recovery)\n"); return; }
    o = psmac_option("mac.card");
    if (o && o[0] == 'r' && o[1] == 'o') { kprintf("[DEBUG] debug-once.txt is on the card, but mac.card=ro: not used (it could not be taken away, and would happen at every start)\n"); return; }
    n = psmac_card_once_take(t, sizeof t, 1);
    if (n < 0) return;                                            /* the rename failed: said in the log, not used */
    of_node_t *c = dt_find_node("/chosen");
    of_property_t *pp = c ? dt_find_property(c, "bootargs") : NULL;
    if (!pp || !pp->op_value) { kprintf("[DEBUG] debug-once.txt: the firmware gave no boot arguments to add it to\n"); return; }
    int merged = 0, skipped = 0; char why_[160];
    const char *args = pp->op_value;
    int k = pdg_once_merge(args, slen(args), t, (uint32_t)n, once_args, sizeof once_args, &merged, &skipped, why_, sizeof why_);
    if (k < 0) { kprintf("[DEBUG] debug-once.txt: the boot arguments would be too long with it: not used\n"); return; }
    pp->op_value = once_args; pp->op_length = (uint32_t)k + 1;
    once_taken = 1;
    scpy(once_used, t, sizeof once_used);
    for (char *q = once_used; *q; q++) if (*q == '\n') *q = ' ';
    for (unsigned q = slen(once_used); q && (once_used[q - 1] == ' ' || once_used[q - 1] == '\r'); q--) once_used[q - 1] = 0;
    kprintf("[DEBUG] a one-shot debug start (debug-once.txt, now debug-once.old): %u option%s added to the boot arguments: %s%s%s. The next start is as cmdline.txt says (docs/08 item 122)\n",
            merged, merged == 1 ? "" : "s", once_used, skipped ? "; left out: " : "", skipped ? why_ : "");
}
/* the card's debug-once.txt (armed) or debug-once.old (the last one used) as one line: its length, or -1 */
static int once_file(const char *name, char *out, unsigned cap)
{
    int n = psmac_card_rec_get(name, (uint8_t *)out, cap - 1);
    if (n < 0) { out[0] = 0; return -1; }
    out[n] = 0;
    for (int i = 0; i < n; i++) if (out[i] == '\n' || out[i] == '\r') out[i] = ' ';
    while (n && out[n - 1] == ' ') out[--n] = 0;
    return n;
}
static void j_once(struct sb *b)
{
    static char armed[600], last[600];
    int a = once_file("debug-once.txt", armed, sizeof armed), l = once_file("debug-once.old", last, sizeof last);
    once_armed = a >= 0;
    bprintf(b, "{\"armed\": %s, \"words\": ", a >= 0 ? "true" : "false"); bjson(b, a >= 0 ? armed : "");
    bput(b, ", \"last\": "); bjson(b, l >= 0 ? last : "");
    bput(b, ", \"used_now\": "); bjson(b, once_used);
    bput(b, "}");
}

/* ---- the screens while the bus check has them ---- */
static uint32_t diag_drawn;
static int bc_file = 0;                       /* buscheck.txt at this start: 1 written, -1 not */
static void vc_clear(void)
{
    uint8_t *fb; uint32_t pitch, w, h; int bpp, swap;
    if (psmac_display_vc(&fb, &pitch, &w, &h, &bpp, &swap)) return;
    uint32_t rowb = w * (bpp == 32 ? 4u : 2u);
    for (uint32_t y = 0; y < h; y++) { uint8_t *row = fb + y * pitch; for (uint32_t x = 0; x < rowb; x++) row[x] = 0; arm_flush_cache((intptr_t)row, rowb); }
}
static void report_show(void)
{
    if (psmac_board_se && !psmac_board_solo && !psmac_buscheck_render(se_fb)) psmac_se_mark(se_fb, 512, 342, 64, 1);
    const char (*ls)[65]; int n = psmac_buscheck_lines(&ls);
    static char page[BC_PAGE_ROWS][65];
    int k = 0;
    for (; k < n && k < BC_PAGE_ROWS - 2; k++) scpy(page[k], ls[k], 65);
    scpy(page[k++], "", 65);
    struct sb b = { page[k], 0, 65 }; bprintf(&b, "Setup and Recovery, PIN %s: Esc or the page's Back returns", pin); k++;
    psmac_console_page((const char (*)[65])page, k);
    diag_drawn = now_ms();
}
static void diag_back(void)
{
    if (!diag_screen) return;
    diag_screen = 0;
    vc_clear();
    scr_dirty = 1;
    did("the recovery screen is back (the bus check's report is in buscheck.txt)");
}
static void diag_screen_poll(void)
{
    static int seen = -1;
    int n = psmac_buscheck_request_count();
    if (diag_screen == 1 && n != seen) { seen = n; scr_dirty = 1; }
    if (diag_screen == 1 && psmac_buscheck_request_state() == 3) {
        diag_screen = 2;
        report_show();
        uint32_t len; const char *t = psmac_buscheck_text(&len);
        bc_file = len && psmac_card_rec_ready() == 1 && !psmac_card_rec_put("buscheck.txt", (const uint8_t *)t, len) ? 1 : -1;
        did("bus check: %s %s (%s)", verdict_word(psmac_buscheck_request_verdict()), psmac_buscheck_request_headline(), bc_file > 0 ? "buscheck.txt written" : "buscheck.txt not written: no writable card");
    } else if (diag_screen == 2 && now_ms() - diag_drawn >= 10000) report_show();   /* again every ten seconds, as the bus check does */
}

/* ---- the card's read test (here, on core 3: the card is the recovery's) ---- */
static struct { int state; char name[REC_NAME_MAX]; struct rec_rd rd; uint32_t total, got, errors, chunks; uint64_t ticks, worst, first; } CT;
static char ct_text[2048], ct_verdict[200]; static int ct_file;
static uint8_t ct_buf[32768];
static int ct_pick(void *vc, const struct rec_ent *e)
{
    struct rec_ent *best = vc;
    if (!(e->attr & 0x18) && e->size > best->size && e->clus >= 2) *best = *e;
    return 0;
}
static int card_test_start(const char **why_)
{
    if (CT.state == 1) { *why_ = "the card test is running"; return 409; }
    if (U.state == UP_RECV || U.state == UP_VERIFY) { *why_ = "an upload is under way: try again when it is done"; return 409; }
    if (psmac_card_rec_ready() != 1 && psmac_card_rec_ready() != -2) { *why_ = "no card"; return 503; }
    struct rec_ent best; best.size = 0;
    if (psmac_card_rec_list(ct_pick, &best) || !best.size) { *why_ = "no file on the card to read"; return 404; }
    scpy(CT.name, best.name, sizeof CT.name);
    psmac_card_rec_read_open(&best, &CT.rd);
    CT.total = best.size > (16u << 20) ? 16u << 20 : best.size;  /* 16 MB at most: a few seconds on a real card */
    CT.got = CT.errors = CT.chunks = 0; CT.ticks = CT.worst = CT.first = 0;
    CT.state = 1; ct_text[0] = 0; ct_verdict[0] = 0; ct_file = 0;
    did("card test: reading %s (%u KB)", CT.name, CT.total >> 10);
    return 0;
}
static void card_test_finish(void)
{
    uint64_t hz = tick_hz();
    uint32_t ms = (uint32_t)(CT.ticks * 1000u / hz), kbs = CT.ticks ? (uint32_t)((uint64_t)CT.got * hz / CT.ticks / 1024u) : 0;
    struct sb v = { ct_verdict, 0, sizeof ct_verdict };
    if (CT.errors) bprintf(&v, "FAIL: the card gave a read error after %u KB: the card, its contacts or the Pi's slot", CT.got >> 10);
    else if (kbs >= 4096) bprintf(&v, "GOOD: the card reads at %u KB/s", kbs);
    else if (kbs >= 1024) bprintf(&v, "SLOW: the card reads at %u KB/s; a 20 MB disk image takes about %u s to load", kbs, 20480u / (kbs ? kbs : 1));
    else bprintf(&v, "VERY SLOW: the card reads at %u KB/s; a 20 MB disk image takes about %u s to load (a card of speed class 10 or A1 is faster)", kbs, 20480u / (kbs ? kbs : 1));
    struct sb b = { ct_text, 0, sizeof ct_text };
    char sha[8]; { const char *g = GIT_SHA; int i = 0; for (; i < 7 && g[i]; i++) sha[i] = g[i]; sha[i] = 0; }
    bprintf(&b, "AURORA68 CARD READ TEST  kernel %s %s %s\n", sha, __DATE__, __TIME__);
    bprintf(&b, "Verdict: %s.\n", ct_verdict);
    bprintf(&b, "Card: %s\n", psmac_card_rec_what());
    bprintf(&b, "Read: %s, %u of its KB in %u.%03u s, %u reads of up to 32 KB (as Setup and Recovery reads files: the clusters in order)\n",
            CT.name, CT.got >> 10, ms / 1000u, ms % 1000u, CT.chunks);
    bprintf(&b, "Speed: %u KB/s; the first read %u us, the slowest %u us\n", kbs, (uint32_t)(CT.first * 1000000u / hz), (uint32_t)(CT.worst * 1000000u / hz));
    bprintf(&b, "Read errors: %u\n", CT.errors);
    bprintf(&b, "Board: %s; the Mac's memory %u MB\n", board_title(), psmac_ram_size() >> 20);
    if (!psmac_bus_uses_gpio()) bput(&b, "Note: in the testbed the card is a file on the host computer and QEMU's emulated card controller sets the speed.\n");
    CT.state = 2;
    ct_file = psmac_card_rec_ready() == 1 && !psmac_card_rec_put("cardcheck.txt", (const uint8_t *)ct_text, b.n) ? 1 : -1;
    did("card test: %s (%s)", ct_verdict, ct_file > 0 ? "cardcheck.txt written" : "cardcheck.txt not written: the card is read only");
}
static void card_test_poll(void)
{
    if (CT.state != 1) return;
    uint32_t want = CT.total - CT.got; if (want > sizeof ct_buf) want = sizeof ct_buf;
    if (!want) { card_test_finish(); return; }
    uint64_t t0 = now_ticks();
    int32_t k = psmac_card_rec_read(&CT.rd, ct_buf, want);
    uint64_t dt = now_ticks() - t0;
    if (k <= 0) { CT.errors += k < 0; card_test_finish(); return; }
    if (!CT.chunks) CT.first = dt;
    if (dt > CT.worst) CT.worst = dt;
    CT.ticks += dt; CT.got += (uint32_t)k; CT.chunks++;
    scr_dirty = (CT.chunks & 63) == 0 ? 1 : scr_dirty;
}

/* the recovery page's lines for what Diagnostics is doing */
static void diag_lines(void)
{
    int st = psmac_buscheck_request_state();
    if (diag_screen == 1 || st == 1 || st == 2) { int n = psmac_buscheck_request_count(); L(0, "Bus check: %s", n ? psmac_buscheck_request_line(n - 1) : "starting ..."); }
    else if (st == 3) L(0, "Bus check: %s %s", verdict_word(psmac_buscheck_request_verdict()), psmac_buscheck_request_headline());
    if (CT.state == 1) L(0, "Card test: %u of %u KB of %s read", CT.got >> 10, CT.total >> 10, CT.name);
    if (once_armed) L(0, "A one-shot debug start is armed: the next normal start uses it, once");
}
/* ---- the CPLD's JTAG usercode ---- */
static const char *jtag_refusal(void)
{
    if (psmac_board_solo || !psmac_board_se) return "the solo board and the IIci have no PiStorm: there is no CPLD to read";
    if (!psmac_bus_uses_gpio()) return "this is the testbed's bridge, not a PiStorm: there is no CPLD to read";
    const char *jo = psmac_option("mac.jtag");
    if (jo && jo[0] == 'o' && jo[1] == 'f') return "mac.jtag=off: the JTAG pins are left alone";
    return 0;
}
static int jtag_asked;
static void j_jtag(struct sb *b)
{
    uint32_t id = 0, uc = 0;
    const char *why_ = jtag_refusal();
    bput(b, "{\"refusal\": "); if (why_) bjson(b, why_); else bput(b, "null");
    int rd = psmac_board_se && !psmac_board_solo && psmac_bus_uses_gpio() && psmac_gpio_cpld(&id, &uc);
    if (rd) { bprintf(b, ", \"at_start\": {\"idcode\": \"%08X\", \"usercode\": \"%08X\", \"name\": ", id, uc); bjson(b, psmac_cpld_name(uc)); bput(b, "}"); }
    else bput(b, ", \"at_start\": null");
    int r = jtag_asked ? psmac_buscheck_jtag_result(&id, &uc) : 0;
    bprintf(b, ", \"again\": \"%s\"", !jtag_asked ? "" : r == 0 ? "reading" : r > 0 ? "read" : "failed");
    if (r > 0) { bprintf(b, ", \"idcode\": \"%08X\", \"usercode\": \"%08X\", \"name\": ", id, uc); bjson(b, psmac_cpld_name(uc)); }
    bput(b, "}");
}
/* the summary's CPLD line */
static void cpld_text(struct sb *b)
{
    uint32_t id = 0, uc = 0;
    if (jtag_asked && psmac_buscheck_jtag_result(&id, &uc) > 0) bprintf(b, "usercode %08X (IDCODE %08X): %s (read again from the Diagnostics tab)", uc, id, psmac_cpld_name(uc));
    else if (psmac_board_se && !psmac_board_solo && psmac_bus_uses_gpio() && psmac_gpio_cpld(&id, &uc)) bprintf(b, "usercode %08X (IDCODE %08X): %s", uc, id, psmac_cpld_name(uc));
    else bput(b, jtag_refusal() ? jtag_refusal() : "not read at this start");
}

/* ---- GET /r/diag ---- */
static void ans_diag(struct rconn *h, int c, const char *from_s)
{
    if (!take_big(c)) { json_err(h, 503, "busy: try again"); return; }
    struct sb b = { big, 0, sizeof big };
    int from = 0; while (*from_s >= '0' && *from_s <= '9') from = from * 10 + (*from_s++ - '0');
    bprintf(&b, "{\"board\": \"%s\", \"board_name\": ", board_model()); bjson(&b, board_title());
    bprintf(&b, ", \"transport\": \"%s\"", psmac_board_solo ? "none" : psmac_bus_uses_gpio() ? "gpio" : "bridge");
    const char *ref = psmac_buscheck_refusal(0), *ref_f = psmac_buscheck_refusal(1);
    int st = psmac_buscheck_request_state(), n = psmac_buscheck_request_count();
    static const char *const sn[] = { "idle", "asked", "running", "done" };
    bput(&b, ",\n \"buscheck\": {\"refusal\": "); if (ref) bjson(&b, ref); else bput(&b, "null");
    bprintf(&b, ", \"force_runs\": %s, \"state\": \"%s\", \"count\": %d, \"from\": %d, \"screen\": \"%s\", \"lines\": [", ref && !ref_f ? "true" : "false",
            st >= 0 && st < 4 ? sn[st] : "?", n, from, diag_screen == 2 ? "report" : diag_screen == 1 ? "check" : "recovery");
    for (int i = from; i < n && b.n + 200 < b.cap; i++) { bput(&b, i > from ? ",\n  " : "\n  "); bjson(&b, psmac_buscheck_request_line(i)); }
    bput(&b, "]");
    if (st == 3) { bprintf(&b, ", \"verdict\": \"%s\", \"headline\": ", verdict_word(psmac_buscheck_request_verdict())); bjson(&b, psmac_buscheck_request_headline()); bprintf(&b, ", \"file\": %d", bc_file); }
    bput(&b, "},\n \"cardtest\": {\"state\": ");
    bjson(&b, CT.state == 1 ? "running" : CT.state == 2 ? "done" : "idle");
    bprintf(&b, ", \"file\": "); bjson(&b, CT.name); bprintf(&b, ", \"got\": %u, \"total\": %u, \"verdict\": ", CT.got, CT.total); bjson(&b, ct_verdict);
    bput(&b, ", \"text\": "); bjson(&b, CT.state == 2 ? ct_text : ""); bprintf(&b, ", \"saved\": %d}", ct_file);
    bput(&b, ",\n \"jtag\": "); j_jtag(&b);
    bput(&b, ",\n \"once\": "); j_once(&b);
    bput(&b, "}\n");
    respond_src(h, "application/json", big, b.n);
}
/* ---- POST /r/diag/run?test=buscheck|card|jtag[&force=1], /r/diag/screen, /r/diag/once[?cancel=1] ---- */
static void ans_diag_post(struct rconn *h, const char *sub, const char *q, char *body, uint32_t n)
{
    char v[40], why_[200];
    if (starts(sub, "run")) {
        if (!query(q, "test", v, sizeof v)) { json_err(h, 400, "?test=buscheck, card or jtag"); return; }
        const char *w = 0;
        if (ieq(v, "buscheck")) {
            char f[4]; int force = query(q, "force", f, sizeof f) && f[0] == '1';
            int r = psmac_buscheck_request(force, &w);
            if (r) { json_err(h, r == -2 ? 409 : 422, w); return; }
            diag_screen = 1; scr_dirty = 1; bc_file = 0;
            did("bus check asked for from the Diagnostics tab%s", force ? " (force: over the testbed's bridge)" : "");
            respond(h, 200, "application/json", "{\"started\": \"buscheck\"}\n");
            return;
        }
        if (ieq(v, "card")) { int r = card_test_start(&w); if (r) { json_err(h, r, w); return; } respond(h, 200, "application/json", "{\"started\": \"card\"}\n"); return; }
        if (ieq(v, "jtag")) {
            if (psmac_buscheck_jtag_request(&w)) { json_err(h, 422, w); return; }
            jtag_asked = 1; did("the CPLD's JTAG usercode read again");
            respond(h, 200, "application/json", "{\"started\": \"jtag\"}\n");
            return;
        }
        json_err(h, 400, "?test=buscheck, card or jtag");
        return;
    }
    if (starts(sub, "screen")) {
        if (diag_screen == 1) { json_err(h, 409, "the bus check is running: the screen is its own until it ends"); return; }
        diag_back();
        respond(h, 200, "application/json", "{\"screen\": \"recovery\"}\n");
        return;
    }
    if (starts(sub, "once")) {
        if (psmac_card_rec_ready() != 1) { json_err(h, 503, "no writable card"); return; }
        if (query(q, "cancel", v, sizeof v) && v[0] == '1') {
            struct rec_ent e;
            if (psmac_card_rec_find("debug-once.txt", &e)) { respond(h, 200, "application/json", "{\"armed\": false, \"note\": \"nothing was armed\"}\n"); return; }
            if (psmac_card_rec_delete("debug-once.txt")) { json_err(h, 500, psmac_card_rec_error()); return; }
            did("the one-shot debug start was cancelled (debug-once.txt deleted)");
            once_armed = 0;
            respond(h, 200, "application/json", "{\"armed\": false, \"note\": \"cancelled: the next start is as cmdline.txt says\"}\n");
            return;
        }
        body[n] = 0;
        while (n && (body[n - 1] == '\n' || body[n - 1] == '\r' || body[n - 1] == ' ')) body[--n] = 0;
        if (pdg_once_check(body, n, why_, sizeof why_)) { json_err(h, 422, why_); return; }
        char line_[600]; scpy(line_, body, sizeof line_ - 1);
        uint32_t k = slen(line_); line_[k++] = '\n'; line_[k] = 0;
        if (psmac_card_rec_put("debug-once.txt", (const uint8_t *)line_, k)) { json_err(h, 500, psmac_card_rec_error()); return; }
        line_[k - 1] = 0;
        once_armed = 1;
        did("a one-shot debug start armed: %s (debug-once.txt; the next start uses it once)", line_);
        struct sb b = { why_, 0, sizeof why_ }; bput(&b, "{\"armed\": true, \"words\": "); bjson(&b, line_); bput(&b, "}\n");
        respond(h, 200, "application/json", why_);
        return;
    }
    json_err(h, 404, "POST /r/diag/run?test=..., /r/diag/screen, /r/diag/once");
}

/* ---- the reports on the card ---- */
static uint8_t *bun_raw, *bun_out;                             /* the bundle's text, and the same scrubbed */
#define BUN_MAX (256u * 1024u)
static int bun_mem(void)
{
    if (!bun_raw) bun_raw = tlsf_malloc(tlsf, BUN_MAX);
    if (!bun_out) bun_out = tlsf_malloc(tlsf, BUN_MAX);
    return bun_raw && bun_out ? 0 : -1;
}
static const char *const rep_names[] = { "buscheck.txt", "cardcheck.txt", "aurora68.log", "debug-once.old" };
static const char *const rep_what[] = { "the bus check (mac.buscheck, or this tab)", "the card's read test (this tab)", "the kernel's log, saved by the start before this one (mac.logsave, after a crash, or from Aurora68 Log)",
                                        "the options of the last one-shot debug start" };
static int is_report(const struct rec_ent *e)
{
    if (e->attr & 0x18) return 0;
    for (unsigned i = 0; i < sizeof rep_names / sizeof rep_names[0]; i++) if (ieq(e->name, rep_names[i])) return 1;
    unsigned n = slen(e->name);
    return n > 4 && ieq(e->name + n - 4, ".log");
}
/* a report's verdict, from its text */
static void verdict_of(const char *name, const char *t, uint32_t n, char *out, unsigned cap)
{
    struct sb b = { out, 0, cap }; out[0] = 0;
    if (ieq(name, "buscheck.txt")) {
        /* the SUMMARY bar ends in the verdict's word; the summary's first sentence follows */
        for (uint32_t i = 0; i + 7 < n; i++) {
            if ((i && t[i - 1] != '\n') || !starts(t + i, "SUMMARY")) continue;
            uint32_t e = i; while (e < n && t[e] != '\n') e++;
            uint32_t w = e; while (w > i && t[w - 1] == ' ') w--;
            uint32_t s = w; while (s > i && t[s - 1] != ' ') s--;
            for (uint32_t k = s; k < w; k++) sb_putc(&b, t[k]);
            bput(&b, ": ");
            for (uint32_t k = e + 1, sp = 0; k < n && b.n + 2 < cap; k++) {
                char ch = t[k] == '\n' ? ' ' : t[k];
                if (ch == ' ' && sp) continue;
                sp = ch == ' ';
                sb_putc(&b, ch);
                if (ch == '.' && (k + 1 >= n || t[k + 1] == ' ' || t[k + 1] == '\n')) break;
            }
            return;
        }
        bput(&b, "(no SUMMARY line: an older or cut report)");
        return;
    }
    if (ieq(name, "cardcheck.txt")) {
        for (uint32_t i = 0; i + 9 < n; i++) if ((!i || t[i - 1] == '\n') && starts(t + i, "Verdict: ")) { for (uint32_t k = i + 9; k < n && t[k] != '\n'; k++) sb_putc(&b, t[k]); return; }
        return;
    }
    if (ieq(name, "debug-once.old")) { bput(&b, "used at a start: "); for (uint32_t k = 0; k < n && t[k] != '\n' && b.n + 2 < cap; k++) sb_putc(&b, t[k]); return; }
    /* a log: how it was saved, and its crash records */
    uint32_t crash = 0, i = 0;
    for (; i < n && t[i] != '\n' && b.n + 2 < cap; i++) sb_putc(&b, t[i]);
    for (uint32_t k = 0; k + 7 < n; k++) if ((!k || t[k - 1] == '\n') && starts(t + k, "[CRASH]")) crash++;
    if (crash) bprintf(&b, "; %u [CRASH] lines (a crash record)", crash); else bput(&b, "; no crash record");
}
static int kernel_date(uint16_t d, uint16_t t) { return !t && d >> 9 == 2026 - 1980 && (d >> 5 & 15) == 11 && ((d & 31) == 18 || (d & 31) == 21 || (d & 31) == 22); }
/* the list first (the card's lock is held while it walks the root), then each report read */
#define REP_MAX 16
struct rep_ctx { struct rec_ent e[REP_MAX]; int n; };
static int rep_cb(void *vc, const struct rec_ent *e)
{
    struct rep_ctx *x = vc;
    if (is_report(e) && x->n < REP_MAX) x->e[x->n++] = *e;
    return 0;
}
static void rep_json(struct sb *b, const struct rec_ent *e, int i)
{
    char vd[260] = "";
    int k = e->size <= BUN_MAX - 1 ? psmac_card_rec_get(e->name, bun_raw, BUN_MAX - 1) : -1;
    if (k >= 0) verdict_of(e->name, (const char *)bun_raw, (uint32_t)k, vd, sizeof vd);
    bput(b, i ? ",\n  " : "\n  "); bput(b, "{\"name\": "); bjson(b, e->name);
    bprintf(b, ", \"size\": %u, \"time\": ", e->size);
    if (!e->wdate || kernel_date(e->wdate, e->wtime)) bput(b, "null");
    else bprintf(b, "\"%04u-%02u-%02u %02u:%02u\"", 1980u + (e->wdate >> 9), e->wdate >> 5 & 15u, e->wdate & 31u, e->wtime >> 11, e->wtime >> 5 & 63u);
    int now_ = (ieq(e->name, "buscheck.txt") && bc_file > 0) || (ieq(e->name, "cardcheck.txt") && ct_file > 0);
    bprintf(b, ", \"this_start\": %s, \"what\": ", now_ ? "true" : "false");
    const char *w = "a log on the card";
    for (unsigned j = 0; j < sizeof rep_names / sizeof rep_names[0]; j++) if (ieq(e->name, rep_names[j])) w = rep_what[j];
    bjson(b, w); bput(b, ", \"verdict\": "); bjson(b, vd); bput(b, "}");
}
static void ans_reports(struct rconn *h, int c)
{
    if (psmac_card_rec_ready() != 1 && psmac_card_rec_ready() != -2) { json_err(h, 503, "no card"); return; }
    if (bun_mem()) { json_err(h, 503, "no memory for the reports"); return; }
    if (!take_big(c)) { json_err(h, 503, "busy: try again"); return; }
    struct sb b = { big, 0, sizeof big };
    static struct rep_ctx x; x.n = 0;
    bput(&b, "{\"reports\": [");
    if (psmac_card_rec_list(rep_cb, &x)) { big_conn = -1; json_err(h, 503, psmac_card_rec_error()); return; }
    for (int i = 0; i < x.n && b.n + 800 < b.cap; i++) rep_json(&b, &x.e[i], i);
    bprintf(&b, "\n], \"count\": %d, \"clock\": false}\n", x.n);
    respond_src(h, "application/json", big, b.n);
}

/* ---- the report bundle: one text file for a tester to send, every secret "(hidden)" ---- */
struct bun { uint8_t *p; uint32_t n, cap; };
static void bn_putc(void *d, char ch) { struct bun *b = d; if (b->n + 1 < b->cap) b->p[b->n++] = (uint8_t)ch; }
static void bnf(struct bun *b, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vkprintf_pc(bn_putc, b, fmt, ap); va_end(ap); }
static void bn_file(struct bun *b, const char *name, const char *what)
{
    struct rec_ent e;
    if (psmac_card_rec_find(name, &e)) { bnf(b, "\n=== %s: not on the card (%s)\n", name, what); return; }
    bnf(b, "\n=== %s, %u bytes: %s\n", name, e.size, what);
    uint32_t room = b->cap - b->n - 64;
    if (e.size > room) { bnf(b, "(too long for the bundle: its last %u KB follow)\n", room >> 10); room -= 64; }
    struct rec_rd rd; psmac_card_rec_read_open(&e, &rd);
    uint32_t skip = e.size > room ? e.size - room : 0, at = 0;
    static uint8_t chunk[4096];
    for (;;) {
        int32_t k = psmac_card_rec_read(&rd, chunk, sizeof chunk);
        if (k <= 0) { if (k < 0) bnf(b, "\n(a read error at %u bytes)\n", at); break; }
        for (int32_t i = 0; i < k; i++, at++) if (at >= skip) { uint8_t ch = chunk[i]; bn_putc(b, ch == '\t' || ch == '\n' || (ch >= 32 && ch < 127) ? (char)ch : '?'); }
    }
    if (b->n && b->p[b->n - 1] != '\n') bn_putc(b, '\n');
}
static void ans_bundle(struct rconn *h, int c)
{
    if (bun_mem()) { json_err(h, 503, "no memory for the bundle"); return; }
    if (!take_big(c)) { json_err(h, 503, "busy: try again"); return; }
    struct bun b = { bun_raw, 0, BUN_MAX };
    char sha[8]; { const char *g = GIT_SHA; int i = 0; for (; i < 7 && g[i]; i++) sha[i] = g[i]; sha[i] = 0; }
    bnf(&b, "Aurora68 report bundle, made by Setup and Recovery (docs/08 item 122). Passwords, tokens and the PIN are shown as (hidden).\n");
    bnf(&b, "Made %u s after this start (the Pi has no clock).\n", (now_ms() - start_ms) / 1000u);
    bnf(&b, "\n=== The machine\n");
    bnf(&b, "Board: %s (mac.board=%s)\n", board_title(), board_model());
    bnf(&b, "Kernel: %s, built %s %s\n", sha, __DATE__, __TIME__);
    { char t[200]; struct sb s = { t, 0, sizeof t }; cpld_text(&s); bnf(&b, "CPLD: %s\n", t); }
    bnf(&b, "Bus: %s\n", psmac_board_solo ? "none (the solo board)" : psmac_bus_uses_gpio() ? "the classic PiStorm's GPIO" : "the testbed's bridge to MAME");
    bnf(&b, "Memory: the Mac's %u MB (the Pi's)", psmac_ram_size() >> 20);
    if (psmac_board_se && !psmac_board_solo) bnf(&b, "; the logic board's own %u KB", psmac_se_ram_bytes() >> 10);
    bnf(&b, "\nCard: %s\n", psmac_card_rec_ready() == 0 ? "being read" : psmac_card_rec_ready() == -1 ? "none" : psmac_card_rec_what());
    bnf(&b, "This start: Setup and Recovery, because %s; boot counter %d of %d\n", why_text, boot_count, boot_limit);
    { struct ipk_info ii; psmac_ip_info(&ii); bnf(&b, "Network: %s, %d.%d.%d.%d (%s)\n", ii.unit == 1 ? "wired" : ii.unit == 0 ? "WiFi" : "none", ii.ip[0], ii.ip[1], ii.ip[2], ii.ip[3], ii.phase); }
    { static char a[600], l[600]; int ar = once_file("debug-once.txt", a, sizeof a), la = once_file("debug-once.old", l, sizeof l);
      bnf(&b, "One-shot debug start: %s%s; the last one used: %s\n", ar >= 0 ? "armed: " : "none armed", ar >= 0 ? a : "", la >= 0 ? l : "(none)"); }
    bnf(&b, "Options in effect at this start (the boot arguments):\n  %s\n", bootargs_now());
    if (cl_read() == 0) {
        bnf(&b, "cmdline.txt on the card now:\n  %s\n", cl_old);
        char bv[16] = ""; pcl_get(cl_old, (uint32_t)cl_len, "mac.board", bv, sizeof bv);
        int board = pdg_board(bv), first = 1;
        for (unsigned i = 0; i < pdg_nopts; i++) { char v[8];
            if (!pdg_uses(&pdg_opts[i], board) && pcl_get(cl_old, (uint32_t)cl_len, pdg_opts[i].key, v, sizeof v)) { bnf(&b, "%s%s", first ? "Set in cmdline.txt but not used on this board: " : ", ", pdg_opts[i].key); first = 0; } }
        if (!first) bnf(&b, "\n");
    }
    int st = psmac_buscheck_request_state();
    if (st == 3) { bnf(&b, "\n=== The bus check run from the Diagnostics tab at this start: %s %s\n", verdict_word(psmac_buscheck_request_verdict()), psmac_buscheck_request_headline());
                   for (int i = 0, n = psmac_buscheck_request_count(); i < n; i++) bnf(&b, "%s\n", psmac_buscheck_request_line(i)); }
    else if (psmac_buscheck_refusal(0)) bnf(&b, "\nBus check: not for this board here: %s\n", psmac_buscheck_refusal(0));
    if (CT.state == 2) bnf(&b, "\n=== The card's read test at this start\n%s", ct_text);
    bt_bundle(&b);
    bn_file(&b, "buscheck.txt", "the last bus check's report");
    bn_file(&b, "cardcheck.txt", "the last card read test");
    bn_file(&b, "aurora68.log", "the log the start before this one saved");
    bnf(&b, "\n=== This start's log (Setup and Recovery)\n");
    { uint32_t room = b.cap - b.n - 16; if (room > PSMB_LOG_SIZE) room = PSMB_LOG_SIZE;
      uint32_t k = psmac_log_copy(b.p + b.n, room), s = 0;
      if (k == PSMB_LOG_SIZE) { while (s < k && b.p[b.n + s] != '\n') s++; if (s < k) s++; }
      for (uint32_t i = s; i < k; i++) { uint8_t ch = b.p[b.n + i]; b.p[b.n + i - s] = ch == '\t' || ch == '\n' || (ch >= 32 && ch < 127) ? ch : '?'; }
      b.n += k - s; }
    bnf(&b, "\n=== End of the bundle\n");
    /* the secrets this start knows, hidden wherever they appear: the WiFi password and the remote's token (the boot
       arguments' and cmdline.txt's), the simulated radio's password, the access point's password, the PIN */
    static char sv[6][100]; const char *secrets[6]; int ns = 0;
    const char *ks[3] = { "wifi.psk", "remote.token", "mac.wifisim" };
    for (int i = 0; i < 3; i++) {
        const char *o = psmac_option(ks[i]);
        if (o && ns < 6) { const char *pw = o; if (i == 2) { while (*pw && *pw != ':') pw++; if (*pw) pw++; } scpy(sv[ns], pw, sizeof sv[ns]); secrets[ns] = sv[ns]; ns++; }
        if (cl_len > 0 && i < 2 && ns < 6 && pcl_get(cl_old, (uint32_t)cl_len, ks[i], sv[ns], sizeof sv[ns]) == 1) { secrets[ns] = sv[ns]; ns++; }
    }
    if (ap_pass[0] && ns < 6) secrets[ns++] = ap_pass;
    if (ns < 6) secrets[ns++] = pin;
    uint32_t out = pdg_scrub((const char *)b.p, b.n, (char *)bun_out, BUN_MAX, secrets, ns);
    for (int i = 0; i < 6; i++) for (unsigned k = 0; k < sizeof sv[i]; k++) sv[i][k] = 0;
    did("report bundle made: %u KB, secrets hidden", out >> 10);
    out_head(h, 200, "text/plain; charset=iso-8859-1", out, "Content-Disposition: attachment; filename=\"aurora68-report.txt\"\r\n");
    h->src = (const char *)bun_out; h->src_len = out; h->src_pos = 0; h->kind = RK_SRC;
}
/* the access point's sign-in (23 November 2026, the owner's first try with a Mac: the "Join Network" sheet kept its
   Cancel button after the PIN, and Cancel leaves the network). A phone or a Mac decides a network is signed in when
   its own check page answers as on the internet; until then it shows the page as a sign-in, with Cancel. So once a
   device has given the right PIN (any /r/ request with it), its checks get the answers they expect and the sheet's
   button becomes Done; the device stays joined and the page opens in its own browser at http://192.168.4.1/. Others'
   checks are still sent to the page. The list is the access point's guests only, forgotten at restart. */
static uint8_t ap_in[16][4]; static unsigned ap_in_n;
static int ap_signed_in(const uint8_t ip[4])
{
    for (unsigned i = 0; i < ap_in_n && i < 16; i++)
        if (!__builtin_memcmp(ap_in[i], ip, 4)) return 1;
    return 0;
}
static void ap_sign_in(const uint8_t ip[4])
{
    if (ap_signed_in(ip)) return;
    __builtin_memcpy(ap_in[ap_in_n % 16], ip, 4);
    ap_in_n++;
    kprintf("[RECOVERY] access point: %u.%u.%u.%u signed in with the PIN: its network checks are answered as online\n", ip[0], ip[1], ip[2], ip[3]);
}
static int ends(const char *s, const char *t)
{
    unsigned a = 0, b = 0;
    while (s[a]) a++;
    while (t[b]) b++;
    return a >= b && !__builtin_memcmp(s + a - b, t, b);
}
static int ap_probe_answer(struct rconn *h, const char *path)
{
    static const char apple[] = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
    static const char firefox[] = "<meta http-equiv=\"refresh\" content=\"0;url=https://support.mozilla.org/kb/captive-portal\"/>";
    if (ends(path, "/hotspot-detect.html") || ends(path, "/success.html")) { respond_src(h, "text/html", apple, sizeof apple - 1); return 1; }
    if (ends(path, "/generate_204") || ends(path, "/gen_204")) { out_head(h, 204, "text/plain", 0, 0); h->kind = RK_OUT; return 1; }
    if (ends(path, "/connecttest.txt")) { respond_src(h, "text/plain", "Microsoft Connect Test", 22); return 1; }
    if (ends(path, "/ncsi.txt")) { respond_src(h, "text/plain", "Microsoft NCSI", 14); return 1; }
    if (ends(path, "/canonical.html")) { respond_src(h, "text/html", firefox, sizeof firefox - 1); return 1; }
    if (ends(path, "/success.txt")) { respond_src(h, "text/plain", "success\n", 8); return 1; }
    if (ends(path, "/check_network_status.txt")) { respond_src(h, "text/plain", "NetworkManager is online\n", 25); return 1; }
    return 0;
}

/* ==================================================================================== Bluetooth
   (23 November 2026, docs/08 item 136; the owner: "Can we add Bluetooth connectivity into the early startup also for
   mouse and keyboards".) The Pi's Bluetooth host (src/alice/bt: bt_pi.c on the service loop, every start) runs here as
   in any start: it powers the chip, loads its firmware and reconnects the devices in pairings.txt, and a paired
   keyboard's reports go where a USB keyboard's go (frf_native_usb_keyboard_boot_report, in_key, psmac_recovery_key):
   the keyboard menu. What recovery lacked was a way to see it and to pair: the board's Bluetooth block (alice_net.h
   struct an_bt) is what the Mac's Bluetooth menu reads and commands (scan, pair, connect, forget, with cmd_seq and
   cmd_done), and with no Mac running this file is its one client. The screens show the state, the devices connected
   and a keyboard's passkey in large digits while it pairs; the page has a Bluetooth tab (GET /r/bt, POST
   /r/bt/scan|pair|connect|forget?addr=); the keyboard menu has the same four. A pairing made here reaches
   pairings.txt through this file's card layer (psmac_card_pairings_take, psmac_card_rec_put), so the next start,
   normal or not, finds it. A mouse moves through the menu (psmac_recovery_mouse): there is no pointer to draw. */
uint32_t psmac_card_pairings_take(char *out, uint32_t max);
static volatile struct an_bt *btb(void)
{
    volatile uint8_t *nb = alice_net_board();
    return nb ? (volatile struct an_bt *)(nb + AN_OFF_BT) : 0;
}
static void bt_addr_text(const volatile uint8_t *a, char o[18])
{
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) { o[i * 3] = hx[a[5 - i] >> 4]; o[i * 3 + 1] = hx[a[5 - i] & 15]; o[i * 3 + 2] = i < 5 ? ':' : 0; }
}
static int bt_addr_parse(const char *s, uint8_t a[6])           /* "AA:BB:CC:DD:EE:FF" (or without the colons), most significant first */
{
    int n = 0;
    for (; *s && n < 12; s++) {
        if (*s == ':' || *s == '-') continue;
        int v = hexv(*s);
        if (v < 0) return -1;
        if (!(n & 1)) a[5 - n / 2] = (uint8_t)(v << 4); else a[5 - n / 2] |= (uint8_t)v;
        n++;
    }
    return n == 12 && !*s ? 0 : -1;
}
static const char *bt_state_name(uint32_t st)
{
    static const char *const n[] = { "off", "waiting for its firmware", "starting", "ready", "failed" };
    return st < 5 ? n[st] : "?";
}
static const char *bt_kind(uint32_t flags) { return (flags & AN_BTK_KEYBOARD) && (flags & AN_BTK_MOUSE) ? "keyboard and mouse" : flags & AN_BTK_KEYBOARD ? "keyboard" : flags & AN_BTK_MOUSE ? "mouse" : "device"; }
static const char *bt_kind_cod(uint32_t cod)
{
    uint32_t m = ((cod >> 8) & 0x1f) == 5 ? (cod >> 6) & 3 : 0;
    return m == 3 ? "keyboard and mouse" : m == 1 ? "keyboard" : m == 2 ? "mouse" : "device";
}
static void bt_name(const volatile char *s, char *o, unsigned cap) { unsigned i = 0; for (; i + 1 < cap && i < 39 && s[i]; i++) o[i] = (s[i] >= ' ' && s[i] < 127) ? s[i] : '?'; o[i] = 0; }
static uint32_t bt_asked_seq; static uint32_t bt_asked_cmd; static char bt_asked_name[44];
/* a command, as the Mac's Bluetooth menu sends one: 0 sent, -1 not ready, -2 the one before is not done */
static int bt_cmd(uint32_t cmd, const uint8_t *a, const char *name)
{
    volatile struct an_bt *b = btb();
    if (!b || b->state != AN_BT_READY) return -1;
    if (b->cmd_seq != b->cmd_done) return -2;
    for (int i = 0; i < 8; i++) b->cmd_addr[i] = a && i < 6 ? a[i] : 0;
    b->cmd = cmd;
    __asm__ volatile("dmb ish" ::: "memory");
    b->cmd_seq = b->cmd_seq + 1;
    bt_asked_seq = b->cmd_seq; bt_asked_cmd = cmd;
    scpy(bt_asked_name, name ? name : "", sizeof bt_asked_name);
    static const char *const what[] = { "", "look for devices", "pair with", "forget", "connect" };
    did("Bluetooth: %s%s%s", what[cmd < 5 ? cmd : 0], name && name[0] ? " " : "", name ? name : "");
    return 0;
}
/* every turn: pairings for the card, and the screens drawn again when what they show of Bluetooth changes */
static void bt_poll_rec(void)
{
    volatile struct an_bt *b = btb();
    if (!b) return;
    static uint32_t sig;
    uint32_t s = b->state * 7u + b->scanning * 11u + b->pairing * 13u + (b->passkey_shown ? b->passkey : 0) + b->nkeys * 17u + b->nfound * 19u;
    for (unsigned k = 0; k < b->nkeys && k < AN_BT_KEYS; k++) s = s * 31u + b->keys[k].flags;
    for (unsigned i = 0; i < sizeof b->msg && b->msg[i]; i++) s = s * 33u + (uint8_t)b->msg[i];
    if (s != sig) { sig = s; scr_dirty = 1; }
    if (U.state == UP_RECV || U.state == UP_VERIFY || CT.state == 1 || psmac_card_rec_ready() != 1) return;
    static char t[4096];
    uint32_t n = psmac_card_pairings_take(t, sizeof t);
    if (!n) return;
    unsigned devs = 0; for (uint32_t i = 0; i + 7 < n; i++) if ((i == 0 || t[i - 1] == '\n') && starts(t + i, "bt.dev=")) devs++;
    if (psmac_card_rec_put("pairings.txt", (const uint8_t *)t, n)) did("Bluetooth: pairings.txt could not be written: %s", psmac_card_rec_error());
    else did("Bluetooth: pairings.txt written, %u device%s (the next start reconnects them)", devs, devs == 1 ? "" : "s");
}
/* the screens' lines: the state, what is connected, a keyboard's passkey */
static void bt_lines(int brief)
{
    volatile struct an_bt *b = btb();
    if (!b) return;
    if (b->passkey_shown) {
        L(0, "Bluetooth keyboard: type this passkey on it, then Return");
        L(2, "%06u", b->passkey);
        return;
    }
    if (brief) return;
    char m[64]; bt_name(b->msg, m, sizeof m);
    if (b->state != AN_BT_READY) { L(0, "Bluetooth: %s%s%s", bt_state_name(b->state), m[0] ? ": " : "", m); return; }
    char t[160]; struct sb s = { t, 0, sizeof t }; int conn = 0;
    for (unsigned k = 0; k < b->nkeys && k < AN_BT_KEYS; k++) if (b->keys[k].flags & AN_BTK_CONNECTED) {
        char nm[40]; bt_name(b->keys[k].name, nm, sizeof nm);
        bprintf(&s, "%s%s (%s)", conn++ ? ", " : "", nm[0] ? nm : "a device", bt_kind(b->keys[k].flags));
    }
    if (b->scanning) L(0, "Bluetooth: looking for devices in pairing mode ...");
    else if (b->pairing) L(0, "Bluetooth: pairing%s%s ...", bt_asked_name[0] ? " with " : "", bt_asked_name);
    else if (conn) L(0, "Bluetooth: %s", t);
    else L(0, "Bluetooth: ready, %u paired, none connected%s", b->nkeys, b->nkeys ? " (press a key, move the mouse)" : "");
}
/* GET /r/bt */
static void ans_bt_get(struct rconn *h)
{
    static char j[6144]; struct sb b = { j, 0, sizeof j };
    volatile struct an_bt *x = btb();
    if (!x) { respond(h, 200, "application/json", "{\"state\": \"none\", \"paired\": [], \"found\": []}\n"); return; }
    char m[64], a[18]; bt_name(x->msg, m, sizeof m); bt_addr_text(x->bdaddr, a);
    bput(&b, "{\"state\": "); bjson(&b, x->state == AN_BT_READY ? "ready" : x->state == AN_BT_FAILED ? "failed" : x->state == AN_BT_OFF ? "off" : "starting");
    bput(&b, ", \"detail\": "); bjson(&b, bt_state_name(x->state)); bput(&b, ", \"message\": "); bjson(&b, m);
    bprintf(&b, ", \"address\": \"%s\", \"scanning\": %s, \"pairing\": %s, \"busy\": %s", x->state == AN_BT_READY ? a : "", x->scanning ? "true" : "false", x->pairing ? "true" : "false", x->cmd_seq != x->cmd_done ? "true" : "false");
    if (x->passkey_shown) bprintf(&b, ", \"passkey\": \"%06u\"", x->passkey); else bput(&b, ", \"passkey\": null");
    bprintf(&b, ", \"last\": {\"command\": \"%s\", \"done\": %s, \"result\": %d}", bt_asked_cmd == AN_BT_CMD_SCAN ? "scan" : bt_asked_cmd == AN_BT_CMD_PAIR ? "pair" : bt_asked_cmd == AN_BT_CMD_CONNECT ? "connect" : bt_asked_cmd == AN_BT_CMD_FORGET ? "forget" : "",
            bt_asked_seq && x->cmd_done == bt_asked_seq ? "true" : "false", (int)x->cmd_result);
    bput(&b, ",\n \"paired\": [");
    for (unsigned k = 0; k < x->nkeys && k < AN_BT_KEYS; k++) {
        char nm[40]; bt_name(x->keys[k].name, nm, sizeof nm); bt_addr_text(x->keys[k].addr, a);
        bprintf(&b, "%s{\"address\": \"%s\", \"name\": ", k ? ", " : "", a); bjson(&b, nm);
        bprintf(&b, ", \"kind\": \"%s\", \"le\": %s, \"connected\": %s}", bt_kind(x->keys[k].flags), x->keys[k].flags & AN_BTK_LE ? "true" : "false", x->keys[k].flags & AN_BTK_CONNECTED ? "true" : "false");
    }
    bput(&b, "],\n \"found\": [");
    for (unsigned k = 0; k < x->nfound && k < AN_BT_FOUND; k++) {
        char nm[40]; bt_name(x->found[k].name, nm, sizeof nm); bt_addr_text(x->found[k].addr, a);
        bprintf(&b, "%s{\"address\": \"%s\", \"name\": ", k ? ", " : "", a); bjson(&b, nm);
        bprintf(&b, ", \"kind\": \"%s\", \"le\": %s, \"rssi\": %d}", bt_kind_cod(x->found[k].cod & 0xffffffu), x->found[k].cod & 0x80000000u ? "true" : "false", x->found[k].rssi);
    }
    struct rec_ent e; int have = psmac_card_rec_ready() == 1 && !psmac_card_rec_find("pairings.txt", &e);
    bprintf(&b, "],\n \"pairings_txt\": %s}\n", have ? "true" : "false");
    respond(h, 200, "application/json", j);
}
/* POST /r/bt/scan, /r/bt/pair?addr=, /r/bt/connect?addr=, /r/bt/forget?addr= */
static void ans_bt_post(struct rconn *h, const char *sub, const char *q)
{
    uint32_t cmd = !__builtin_strcmp(sub, "scan") ? AN_BT_CMD_SCAN : !__builtin_strcmp(sub, "pair") ? AN_BT_CMD_PAIR :
                   !__builtin_strcmp(sub, "connect") ? AN_BT_CMD_CONNECT : !__builtin_strcmp(sub, "forget") ? AN_BT_CMD_FORGET : 0;
    if (!cmd) { json_err(h, 404, "POST /r/bt/scan, /r/bt/pair?addr=, /r/bt/connect?addr=, /r/bt/forget?addr="); return; }
    uint8_t a[6] = { 0 }; char v[40], nm[44] = "";
    volatile struct an_bt *x = btb();
    if (cmd != AN_BT_CMD_SCAN) {
        if (!query(q, "addr", v, sizeof v) || bt_addr_parse(v, a)) { json_err(h, 400, "?addr=AA:BB:CC:DD:EE:FF"); return; }
        int known = 0;
        for (unsigned k = 0; x && k < x->nkeys && k < AN_BT_KEYS; k++) { int same = 1; for (int i = 0; i < 6; i++) if (x->keys[k].addr[i] != a[i]) same = 0; if (same) { known = 1; bt_name(x->keys[k].name, nm, sizeof nm); } }
        for (unsigned k = 0; x && !known && k < x->nfound && k < AN_BT_FOUND; k++) { int same = 1; for (int i = 0; i < 6; i++) if (x->found[k].addr[i] != a[i]) same = 0; if (same) bt_name(x->found[k].name, nm, sizeof nm); }
        if (cmd != AN_BT_CMD_PAIR && !known) { json_err(h, 404, "that device is not paired"); return; }
        if (!nm[0]) scpy(nm, v, sizeof nm);
    }
    int r = bt_cmd(cmd, cmd == AN_BT_CMD_SCAN ? 0 : a, nm);
    if (r == -1) { json_err(h, 409, x && x->state == AN_BT_OFF ? "Bluetooth is off (bt=off in cmdline.txt)" : "Bluetooth is not ready yet"); return; }
    if (r == -2) { json_err(h, 409, "busy with the command before: try again in a moment"); return; }
    respond(h, 200, "application/json", cmd == AN_BT_CMD_SCAN ? "{\"sent\": true, \"note\": \"Looking for devices for about ten seconds: put the keyboard or mouse in pairing mode\"}\n" :
            cmd == AN_BT_CMD_PAIR ? "{\"sent\": true, \"note\": \"Pairing: a keyboard shows a passkey to type on it\"}\n" : "{\"sent\": true}\n");
}
/* a mouse in Setup and Recovery: no pointer is drawn; it moves through the keyboard menu. Up and down are a move of
   about half a centimetre (or the wheel), the left button is Return, the right one Esc. 0 outside recovery */
int psmac_recovery_mouse(int dx, int dy, unsigned buttons, int wheel)
{
    (void)dx;
    if (!psmac_recovery_on()) return 0;
    static int acc; static unsigned last_b;
    if (wheel) { psmac_recovery_key(wheel > 0 ? 0x52 : 0x51, 1, 0); return 1; }
    acc += dy;
    while (acc >= 120) { acc -= 120; psmac_recovery_key(0x51, 1, 0); }
    while (acc <= -120) { acc += 120; psmac_recovery_key(0x52, 1, 0); }
    if (buttons != 0xffffffffu) {
        unsigned pressed = buttons & ~last_b;
        last_b = buttons;
        if (pressed & 1) psmac_recovery_key(0x28, 1, 0);
        if (pressed & 2) psmac_recovery_key(0x29, 1, 0);
    }
    return 1;
}
/* the bundle's part: the state and the devices, their addresses shortened, no keys (pairings.txt is not included) */
static void bt_bundle(struct bun *bb)
{
    volatile struct an_bt *x = btb();
    bnf(bb, "\n=== Bluetooth\n");
    if (!x) { bnf(bb, "No Bluetooth block (the network board is not up)\n"); return; }
    char m[64]; bt_name(x->msg, m, sizeof m);
    bnf(bb, "State: %s%s%s\n", bt_state_name(x->state), m[0] ? ": " : "", m);
    bnf(bb, "Paired: %u%s\n", x->nkeys, x->nkeys ? " (addresses shortened; their keys are not in this bundle)" : "");
    for (unsigned k = 0; k < x->nkeys && k < AN_BT_KEYS; k++) {
        char nm[40]; bt_name(x->keys[k].name, nm, sizeof nm);
        bnf(bb, "  %s: %s%s, %s, ..:%02X:%02X\n", nm[0] ? nm : "(no name)", bt_kind(x->keys[k].flags), x->keys[k].flags & AN_BTK_LE ? " (LE)" : "",
            x->keys[k].flags & AN_BTK_CONNECTED ? "connected" : "not connected", x->keys[k].addr[1], x->keys[k].addr[0]);
    }
    if (x->nfound) bnf(bb, "Found by the last scan: %u\n", x->nfound);
    struct rec_ent e;
    if (psmac_card_rec_ready() == 1) bnf(bb, "pairings.txt: %s\n", psmac_card_rec_find("pairings.txt", &e) ? "not on the card" : "on the card");
    { const char *o = psmac_option("bt"); bnf(bb, "bt=%s\n", o ? o : " not given (on)"); }
}

static void handle(int c)
{
    struct rconn *h = &rc[c];
    char *r = h->req, *eoh = 0;
    for (uint32_t i = 0; i + 3 < h->req_len; i++) if (r[i] == '\r' && r[i + 1] == '\n' && r[i + 2] == '\r' && r[i + 3] == '\n') { eoh = r + i + 4; break; }
    if (!eoh) { if (h->req_len >= REQ_MAX) { h->handled = 1; json_err(h, 413, "the request is too long"); } return; }
    uint32_t clen = 0; int has_clen = 0; const char *pn = 0; unsigned pl = 0;
    for (char *l = r; l < eoh; ) {
        char *e = l; while (e < eoh && *e != '\r') e++;
        unsigned n = (unsigned)(e - l);
        if (n > 15 && (lower(l[0]) == 'c') && starts(l + 1, "ontent-") && (lower(l[8]) == 'l')) { const char *p = l + 15; while (*p == ' ') p++; has_clen = 1; while (*p >= '0' && *p <= '9') clen = clen * 10 + (uint32_t)(*p++ - '0'); }
        else if (n > 13 && (lower(l[0]) == 'x') && l[1] == '-' && lower(l[2]) == 'a' && lower(l[9]) == 'p' && l[12] == ':') { pn = l + 13; while (*pn == ' ') pn++; pl = (unsigned)(e - pn); }
        l = e + 2;
    }
    int upload = starts(r, "PUT /r/file");
    uint32_t hl = (uint32_t)(eoh - r);
    if (!upload && hl + clen > REQ_MAX) { h->handled = 1; json_err(h, 413, "the request is too long"); return; }
    if (!upload && hl + clen > h->req_len) return;                 /* the body is still coming */
    h->handled = 1;
    char *body = eoh; uint32_t have = h->req_len - hl;
    char *sp = r; while (*sp && *sp != ' ') sp++;
    if (!*sp) { json_err(h, 400, "no request line"); return; }
    *sp = 0;
    char *path = sp + 1, *pe = path; while (*pe && *pe != ' ' && *pe != '\r') pe++;
    *pe = 0;
    char *q = path; while (*q && *q != '?') q++;
    if (*q) *q++ = 0;
    int get = !__builtin_strcmp(r, "GET"), post = !__builtin_strcmp(r, "POST"), put = !__builtin_strcmp(r, "PUT");
    if (!psmac_remote_private_source(h->peer)) { json_err(h, 403, "only the local network"); return; }
    if (get && (!__builtin_strcmp(path, "/") || !__builtin_strcmp(path, "/index.html"))) { respond_src(h, "text/html; charset=utf-8", recovery_page, slen(recovery_page)); return; }
    /* the access point's guests: a phone checks a network by fetching a known page (generate_204, hotspot-detect.html);
       every name leads here (psmac_apnet.c), and the answer sends it to the page, which it offers as a sign-in */
    if (get && ap_active() && !starts(path, "/r/")) {
        if (ap_signed_in(h->peer) && ap_probe_answer(h, path)) return;
        out_head(h, 302, "text/html", 0, "Location: http://192.168.4.1/?signin=1\r\n");
        h->kind = RK_OUT;
        return;
    }
    if (!starts(path, "/r/")) { json_err(h, 404, "the page is /, the API /r/..."); return; }
    if (pin_locked_until && (int32_t)(now_ms() - pin_locked_until) < 0) { json_err(h, 429, "ten wrong PINs: a new PIN is on the screen; requests are refused for a minute"); return; }
    if (!pn || !pin_ok(pn, pl)) {
        if (++pin_wrong >= 10) {
            new_pin(); pin_changes++; pin_locked_until = now_ms() + 60000u; if (!pin_locked_until) pin_locked_until = 1;
            kprintf("[RECOVERY] ten wrong PINs: a new PIN (shown on screen) and a minute without answers\n");
            scr_dirty = 1;
        }
        json_err(h, 401, "the PIN shown on the Mac's screen is missing or wrong (X-Aurora-Pin)");
        return;
    }
    pin_wrong = 0;
    if (ap_active()) ap_sign_in(h->peer);
    char v[REC_NAME_MAX + 8], v2[REC_NAME_MAX + 8];
    path += 3;
    if (get && !__builtin_strcmp(path, "state")) { ans_state(h); return; }
    if (get && !__builtin_strcmp(path, "files")) { ans_files(h, c); return; }
    if ((get || put) && !__builtin_strcmp(path, "file")) {
        if (!query(q, "name", v, sizeof v) || !v[0]) { json_err(h, 400, "?name=FILE"); return; }
        if (get) {
            struct rec_ent e;
            if (psmac_card_rec_find(v, &e) || (e.attr & 0x10)) { json_err(h, 404, "no such file"); return; }
            if ((ieq(e.name, "cmdline.txt") || ieq(e.name, "cmdline.bak")) && e.size < sizeof big && take_big(c)) {   /* the WiFi password stays on the card */
                static char raw[sizeof big];
                int n = psmac_card_rec_get(e.name, (uint8_t *)raw, sizeof raw - 1);
                if (n < 0) { big_conn = -1; json_err(h, 500, "it could not be read"); return; }
                raw[n] = 0; redacted(raw, big, sizeof big);
                respond_src(h, "text/plain; charset=iso-8859-1", big, slen(big));
                return;
            }
            char x[200]; struct sb b = { x, 0, sizeof x }; bput(&b, "Content-Disposition: attachment; filename=\""); bput(&b, e.name); bput(&b, "\"\r\n");
            out_head(h, 200, "application/octet-stream", e.size, x);
            psmac_card_rec_read_open(&e, &h->rd); h->kind = RK_FILE;
            return;
        }
        if (!has_clen) { json_err(h, 411, "an upload needs Content-Length"); return; }
        char why_[200];
        int code = up_start(c, v, clen, why_, sizeof why_);
        if (code) { json_err(h, code, why_); return; }
        h->kind = RK_UPLOAD;
        up_feed((const uint8_t *)body, have);
        if (!clen) up_pump();
        return;
    }
    if (post && !__builtin_strcmp(path, "rm")) {
        if (!query(q, "name", v, sizeof v)) { json_err(h, 400, "?name=FILE"); return; }
        if (psmac_card_rec_delete(v)) { json_err(h, ieq(psmac_card_rec_error(), "no such file") ? 404 : 409, psmac_card_rec_error()); return; }
        did("%s deleted", v);
        respond(h, 200, "application/json", "{\"deleted\": true}\n");
        return;
    }
    if (post && !__builtin_strcmp(path, "mv")) {
        if (!query(q, "from", v, sizeof v) || !query(q, "to", v2, sizeof v2)) { json_err(h, 400, "?from=A&to=B"); return; }
        if (psmac_card_rec_rename(v, v2)) { json_err(h, 409, psmac_card_rec_error()); return; }
        did("%s renamed %s", v, v2);
        respond(h, 200, "application/json", "{\"renamed\": true}\n");
        return;
    }
    if (get && !__builtin_strcmp(path, "text")) { ans_text_get(h, c, query(q, "name", v, sizeof v) ? v : ""); return; }
    if (put && !__builtin_strcmp(path, "text")) { ans_text_put(h, query(q, "name", v, sizeof v) ? v : "", body, clen); return; }
    if ((get || post) && !__builtin_strcmp(path, "options")) { ans_options(h, c, post, query(q, "dry", v, sizeof v) && v[0] == '1', body, clen); return; }
    if (get && !__builtin_strcmp(path, "diag")) { ans_diag(h, c, query(q, "from", v, sizeof v) ? v : "0"); return; }
    if (post && starts(path, "diag/")) { ans_diag_post(h, path + 5, q, body, clen); return; }
    if (get && !__builtin_strcmp(path, "reports")) { ans_reports(h, c); return; }
    if (get && !__builtin_strcmp(path, "bundle")) { ans_bundle(h, c); return; }
    if (get && !__builtin_strcmp(path, "bt")) { ans_bt_get(h); return; }
    if (post && starts(path, "bt/")) { ans_bt_post(h, path + 3, q); return; }
    if ((get && !__builtin_strcmp(path, "wifi")) || (post && starts(path, "wifi/"))) { ans_wifi(h, get ? "" : path + 5, body, clen); return; }
    if (post && !__builtin_strcmp(path, "reboot")) { ans_reboot(h, query(q, "how", v, sizeof v)); return; }
    if (post && !__builtin_strcmp(path, "rollback")) { ans_rollback(h, query(q, "force", v, sizeof v) && v[0] == '1'); return; }
    if (get && !__builtin_strcmp(path, "log")) { ans_log(h, c, query(q, "now", v, sizeof v) && v[0] == '1'); return; }
    if (get && !__builtin_strcmp(path, "display")) { ans_display_get(h, c); return; }
    if (get && !__builtin_strcmp(path, "screen.pbm") && screen_shot_ok()) {       /* the testbed's photograph of the compact Mac's screen */
        static char pbm[16 + sizeof se_fb];
        se_render();
        struct sb b = { pbm, 0, sizeof pbm }; bput(&b, "P4\n512 342\n");
        uint32_t n = b.n; memcpy(pbm + n, se_fb, sizeof se_fb);
        respond_src(h, "image/x-portable-bitmap", pbm, n + (uint32_t)sizeof se_fb);
        return;
    }
    if (post && !__builtin_strcmp(path, "display")) { ans_display_post(h, c, body, clen); return; }
    json_err(h, 404, "GET /r/state, /r/files, /r/file, /r/text, /r/options, /r/wifi, /r/bt, /r/log, /r/display, /r/diag, /r/reports, /r/bundle; PUT /r/file, /r/text; POST /r/rm, /r/mv, /r/options, /r/wifi/..., /r/bt/..., /r/display, /r/diag/..., /r/reboot, /r/rollback");
}
static uint32_t produce(int c, uint8_t *buf, uint32_t room, int *fin)
{
    struct rconn *h = &rc[c];
    *fin = 0;
    if (!h->handled) return 0;
    if (h->kind == RK_UPLOAD) {
        if (U.conn != c || U.state != UP_DONE) return 0;
        out_head(h, U.code, "application/json", (int64_t)slen(U.result), 0);
        struct sb b = { h->out, h->out_len, sizeof h->out }; bput(&b, U.result); h->out_len = b.n;
        h->kind = RK_OUT; U.state = UP_IDLE; U.conn = -1;
    }
    uint32_t n = 0;
    while (n < room) {
        if (h->out_pos < h->out_len) {
            uint32_t k = h->out_len - h->out_pos; if (k > room - n) k = room - n;
            memcpy(buf + n, h->out + h->out_pos, k); h->out_pos += k; n += k;
            continue;
        }
        if (h->kind == RK_SRC && h->src_pos < h->src_len) {
            uint32_t k = h->src_len - h->src_pos; if (k > room - n) k = room - n;
            memcpy(buf + n, h->src + h->src_pos, k); h->src_pos += k; n += k;
            continue;
        }
        if (h->kind == RK_FILE && h->rd.pos < h->rd.size) {
            if (room - n < 512 && n) return n;                     /* a whole sector at least next time */
            int32_t k = psmac_card_rec_read(&h->rd, buf + n, room - n);
            if (k <= 0) { ipk_tcp_abort(c); return n; }            /* a card error mid-file: the client sees it short */
            n += (uint32_t)k;
            return n;                                               /* one card read per call */
        }
        if (big_conn == c) big_conn = -1;
        *fin = 1;
        break;
    }
    return n;
}
int recovery_tcp_accept(int c, uint16_t port, const uint8_t peer[4])
{
    (void)port;
    struct rconn *h = &rc[c];
    h->used = 1; h->req_len = 0; h->handled = 0; h->kind = RK_NONE; h->out_len = h->out_pos = 0;
    for (int i = 0; i < 4; i++) h->peer[i] = peer[i];
    return 1;
}
void recovery_tcp_recv(int c, const uint8_t *data, uint32_t len)
{
    struct rconn *h = &rc[c];
    if (h->handled) { if (h->kind == RK_UPLOAD && U.conn == c) { up_feed(data, len); scr_dirty = 1; } return; }
    if (len > REQ_MAX - h->req_len) len = REQ_MAX - h->req_len;
    memcpy(h->req + h->req_len, data, len);
    h->req_len += len; h->req[h->req_len] = 0;
    handle(c);
}
uint32_t recovery_tcp_room(int c)
{
    struct rconn *h = &rc[c];
    if (!h->handled) return REQ_MAX - h->req_len;
    if (h->kind == RK_UPLOAD && U.conn == c && U.state == UP_RECV) return up_room() > 65535 ? 65535 : up_room();
    return 65535;
}
uint32_t recovery_tcp_produce(int c, uint8_t *buf, uint32_t room)
{
    int fin;
    uint32_t n = produce(c, buf, room, &fin);
    if (fin) ipk_tcp_close(c);
    return n;
}
void recovery_tcp_closed(int c)
{
    struct rconn *h = &rc[c];
    if (U.conn == c && U.state == UP_RECV) { did("the upload of %s stopped after %u of %u bytes (the connection closed): nothing kept", U.target, U.got, U.total); up_fail(400, "the connection closed"); U.state = UP_IDLE; U.conn = -1; }
    else if (U.conn == c) U.conn = -1;
    if (big_conn == c) big_conn = -1;
    h->used = 0; h->handled = 0; h->kind = RK_NONE;
}

/* ======================================================================== the keyboard menu */
/* For a Pi that is on no network at all (no cable, no WiFi settings or wrong ones): a USB keyboard on the
   Pi (and the Plus's and 512K's own keyboards, which this kernel reads itself; the SE's ADB keyboard is
   read by the Mac's ROM, which does not run here) chooses a network from a scan, types its password and
   country, sets three options and saves them into cmdline.txt, or restarts. Up and Down choose, Return
   changes, Esc goes back. The password is shown as stars and never logged. */
#define MI_N 12
/* mac.console's values, from the options table (psmac_diag.c): splash (the default, written as no word), boot, hdmi
   (23 November 2026, docs/08 item 128; the menu offered off and hdmi before) */
static char cons_buf[40]; static const char *cons[4]; static int ncons;
static void cons_load(void)
{
    if (ncons) return;
    const struct pdg_opt *o = pdg_find("mac.console");
    scpy(cons_buf, o ? o->values : "splash|boot|hdmi", sizeof cons_buf);
    for (char *p = cons_buf; *p && ncons < 4; ) {
        cons[ncons++] = p;
        while (*p && *p != '|') p++;
        if (*p) *p++ = 0;
    }
}
static void menu_load(void)
{
    char v[72];
    cons_load();
    m_ssid[0] = m_psk[0] = m_country[0] = 0; m_psk_set = 0; m_changed = 0;
    scpy(m_fpu, "exact", sizeof m_fpu); scpy(m_boot, "auto", sizeof m_boot); scpy(m_console, cons[0], sizeof m_console);
    if (cl_read()) return;
    if (pcl_get(cl_old, (uint32_t)cl_len, "wifi.ssid", v, sizeof v) == 1) scpy(m_ssid, v, sizeof m_ssid);
    if (pcl_get(cl_old, (uint32_t)cl_len, "wifi.psk", v, sizeof v) == 1 && v[0]) m_psk_set = 1;
    if (pcl_get(cl_old, (uint32_t)cl_len, "wifi.country", v, sizeof v) == 1) scpy(m_country, v, sizeof m_country);
    if (pcl_get(cl_old, (uint32_t)cl_len, "mac.fpu", v, sizeof v) == 1) scpy(m_fpu, v, sizeof m_fpu);
    if (pcl_get(cl_old, (uint32_t)cl_len, "mac.boot", v, sizeof v) == 1) scpy(m_boot, v, sizeof m_boot);
    if (pcl_get(cl_old, (uint32_t)cl_len, "mac.console", v, sizeof v) == 1) scpy(m_console, v, sizeof m_console);
    scpy(m0_ssid, m_ssid, sizeof m0_ssid); scpy(m0_country, m_country, sizeof m0_country);
    scpy(m0_fpu, m_fpu, sizeof m0_fpu); scpy(m0_boot, m_boot, sizeof m0_boot); scpy(m0_console, m_console, sizeof m0_console);
}
static void cycle(char *v, unsigned cap, const char *const *opts, int n)
{
    int i = 0; while (i < n && !ieq(v, opts[i])) i++;
    scpy(v, opts[(i + 1) % n], cap);
    m_changed = 1;
}
static void menu_save(void)
{
    char why_[200];
    if (cl_read()) { scpy(m_msg, "no cmdline.txt on the card", sizeof m_msg); return; }
    if (!country_ok(m_country, why_, sizeof why_)) { scpy(m_msg, "the country: two letters, not XX", sizeof m_msg); return; }
    uint32_t nn = (uint32_t)cl_len;
    for (uint32_t i = 0; i < nn; i++) cl_new[i] = cl_old[i];
    cl_new[nn] = 0;
    int e = 0;
    if (m_ssid[0] && !ieq(m_ssid, m0_ssid)) e |= cl_set(&nn, "wifi.ssid", m_ssid);
    if (m_psk_set == 2) e |= cl_set(&nn, "wifi.psk", m_psk[0] ? m_psk : 0);
    if (m_country[0] && !ieq(m_country, m0_country)) e |= cl_set(&nn, "wifi.country", m_country);
    if (!ieq(m_fpu, m0_fpu)) e |= cl_set(&nn, "mac.fpu", m_fpu);
    if (!ieq(m_boot, m0_boot)) e |= cl_set(&nn, "mac.boot", m_boot);
    if (!ieq(m_console, m0_console)) e |= cl_set(&nn, "mac.console", ieq(m_console, cons[0]) ? 0 : m_console);   /* the default: no word */
    if (e || cl_write(nn, why_, sizeof why_)) { scpy(m_msg, "not saved: the line would not be valid", sizeof m_msg); return; }
    m_changed = 0; m_psk_set = m_psk[0] ? 1 : 0;
    for (unsigned i = 0; i < sizeof m_psk; i++) m_psk[i] = 0;
    menu_load();
    scpy(m_msg, "saved to cmdline.txt (the line before: cmdline.bak)", sizeof m_msg);
    did("settings saved from the keyboard menu (cmdline.bak keeps the line before)");
}
static void menu_lines(void)
{
    if (!menu_on) { L(0, ""); L(0, "Keyboard: Return opens the settings menu (WiFi, Bluetooth, options); a mouse: click"); return; }
    L(0, "");
    if (menu_edit) {
        static const char *const what[] = { "", "Network name", "Password", "Country (two letters)" };
        char shown[70]; unsigned i = 0;
        for (; i < m_len && i < 60; i++) shown[i] = menu_edit == 2 ? '*' : m_buf[i];
        shown[i] = 0;
        L(0, "%s: %s_", what[menu_edit], shown);
        L(0, "Return to keep it, Esc to cancel");
        return;
    }
    if (menu_bt) {
        volatile struct an_bt *b = btb();
        unsigned nf = b ? (b->nfound < AN_BT_FOUND ? b->nfound : AN_BT_FOUND) : 0, nk = b ? (b->nkeys < AN_BT_KEYS ? b->nkeys : AN_BT_KEYS) : 0;
        int n = 1 + (int)nf + (int)nk;
        if (bt_sel >= n) bt_sel = n - 1;
        L(0, "Bluetooth (Return: look, pair, connect; Backspace twice forgets; Esc back):");
        int first = bt_sel > 5 ? bt_sel - 5 : 0;
        for (int i = first; i < n && i < first + 7; i++) {
            char nm[40]; char t[80]; struct sb sb_ = { t, 0, sizeof t };
            if (i == 0) bput(&sb_, b && b->scanning ? "Looking for devices (about ten seconds) ..." : "Look for devices (put the keyboard or mouse in pairing mode)");
            else if (i <= (int)nf) { bt_name(b->found[i - 1].name, nm, sizeof nm); bprintf(&sb_, "Pair:    %s (%s%s)", nm[0] ? nm : "(no name)", bt_kind_cod(b->found[i - 1].cod & 0xffffffu), b->found[i - 1].cod & 0x80000000u ? ", LE" : ""); }
            else { unsigned k = (unsigned)i - 1 - nf; bt_name(b->keys[k].name, nm, sizeof nm); bprintf(&sb_, "Paired:  %s (%s, %s)", nm[0] ? nm : "(no name)", bt_kind(b->keys[k].flags), b->keys[k].flags & AN_BTK_CONNECTED ? "connected" : "Return connects"); }
            L(0, "%c %s", i == bt_sel ? '>' : ' ', t);
        }
        if (m_msg[0]) L(0, "%s", m_msg);
        else if (b) { char m[64]; bt_name(b->msg, m, sizeof m); L(0, "Bluetooth: %s%s%s", bt_state_name(b->state), m[0] ? ", " : "", m); }
        return;
    }
    if (menu_list) {
        volatile struct an_unit *u = unit(0);
        uint32_t n = u ? u->scan_count : 0;
        volatile struct an_scan *sc = u ? (volatile struct an_scan *)((volatile uint8_t *)alice_net_board() + AN_OFF_SCAN) : 0;
        L(0, "Networks%s (Return chooses, Esc back):", u && u->state == AN_ST_SCANNING ? " (scanning)" : "");
        int first = menu_sel > 6 ? menu_sel - 6 : 0;
        for (int i = first; i < (int)n && i < first + 7; i++) {
            char ss[36]; unsigned q = 0; for (; q < 32 && sc[i].ssid[q]; q++) ss[q] = sc[i].ssid[q];
            ss[q] = 0;
            L(0, "%c %-32s %4d dBm %s", i == menu_sel ? '>' : ' ', ss, sc[i].rssi, sc[i].security & 0x08 ? "WPA2" : sc[i].security & 0x06 ? "WPA/WEP" : "open");
        }
        if (!n) L(0, "  (none found yet)");
        return;
    }
    const char *items[MI_N]; char it[MI_N][64];
    struct sb b;
    for (int i = 0; i < MI_N; i++) { b.s = it[i]; b.n = 0; b.cap = sizeof it[i];
        switch (i) {
        case 0: bput(&b, "Scan for WiFi networks"); break;
        case 1: bprintf(&b, "Network:  %s", m_ssid[0] ? m_ssid : "(none)"); break;
        case 2: bprintf(&b, "Password: %s", m_psk_set == 2 ? "typed" : m_psk_set ? "set" : "not set"); break;
        case 3: bprintf(&b, "Country:  %s", m_country[0] ? m_country : "(the firmware's)"); break;
        case 4: bprintf(&b, "mac.fpu:  %s", m_fpu); break;
        case 5: bprintf(&b, "mac.boot: %s", m_boot); break;
        case 6: bprintf(&b, "mac.console: %s", m_console); break;
        case 7: bprintf(&b, "Save to cmdline.txt%s", m_changed ? " (changed)" : ""); break;
        case 8: bput(&b, "Restart normally"); break;
        case 9: bput(&b, "Restart into recovery"); break;
        case 10: bput(&b, ap_active() ? "Access point: running" : "Start an access point (a WiFi network for a phone)"); break;
        default: bput(&b, "Bluetooth keyboards and mice: look, pair, connect, forget"); break;
        }
        items[i] = it[i];
    }
    int first = menu_sel > 5 ? menu_sel - 5 : 0;
    for (int i = first; i < MI_N && i < first + 7; i++) L(0, "%c %s", i == menu_sel ? '>' : ' ', items[i]);
    L(0, "%s", m_msg[0] ? m_msg : "Up/Down choose, Return changes, Esc closes");
}
static char key_char(uint32_t u, int shift)
{
    static const char sh_digits[] = "!@#$%^&*()";
    if (u >= 0x04 && u <= 0x1d) return (char)((shift ? 'A' : 'a') + (int)(u - 0x04));
    if (u >= 0x1e && u <= 0x27) { int d = (int)(u - 0x1e); return shift ? sh_digits[d] : (char)(d == 9 ? '0' : '1' + d); }
    switch (u) {
    case 0x2c: return ' ';
    case 0x2d: return shift ? '_' : '-';
    case 0x2e: return shift ? '+' : '=';
    case 0x2f: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    default: return 0;
    }
}
static void menu_key(uint32_t u, uint32_t mods)
{
    int shift = (mods & 0x22) != 0;
    scr_dirty = 1;
    if (!menu_on) { if (u == 0x28 || u == 0x52 || u == 0x51) { menu_on = 1; menu_sel = 0; m_msg[0] = 0; menu_load(); } return; }
    if (menu_edit) {
        if (u == 0x29) { menu_edit = 0; return; }
        if (u == 0x2a) { if (m_len) m_len--; return; }
        if (u == 0x28) {
            m_buf[m_len] = 0;
            if (menu_edit == 1) { scpy(m_ssid, m_buf, sizeof m_ssid); }
            else if (menu_edit == 2) { scpy(m_psk, m_buf, sizeof m_psk); m_psk_set = 2; }
            else { for (unsigned i = 0; i < m_len; i++) if (m_buf[i] >= 'a' && m_buf[i] <= 'z') m_buf[i] = (char)(m_buf[i] - 32); scpy(m_country, m_buf, sizeof m_country); }
            menu_edit = 0; m_changed = 1;
            return;
        }
        char ch = key_char(u, shift);
        unsigned cap = menu_edit == 1 ? 32 : menu_edit == 2 ? 63 : 2;
        if (ch && m_len < cap) m_buf[m_len++] = ch;
        return;
    }
    if (menu_bt) {
        volatile struct an_bt *b = btb();
        unsigned nf = b ? (b->nfound < AN_BT_FOUND ? b->nfound : AN_BT_FOUND) : 0, nk = b ? (b->nkeys < AN_BT_KEYS ? b->nkeys : AN_BT_KEYS) : 0;
        int n = 1 + (int)nf + (int)nk;
        if (u == 0x29) { menu_bt = 0; menu_sel = MI_N - 1; m_msg[0] = 0; return; }
        if (u == 0x52) { if (bt_sel > 0) bt_sel--; bt_forget_armed = -1; m_msg[0] = 0; return; }
        if (u == 0x51) { if (bt_sel + 1 < n) bt_sel++; bt_forget_armed = -1; m_msg[0] = 0; return; }
        if (!b) return;
        uint8_t a[6]; char nm[40];
        int paired = bt_sel > (int)nf;
        unsigned k = paired ? (unsigned)bt_sel - 1 - nf : (unsigned)bt_sel - 1;
        if (bt_sel > 0) { for (int i = 0; i < 6; i++) a[i] = paired ? b->keys[k].addr[i] : b->found[k].addr[i]; bt_name(paired ? b->keys[k].name : b->found[k].name, nm, sizeof nm); }
        int r = 0;
        if ((u == 0x2a || u == 0x4c) && paired) {
            if (bt_forget_armed != bt_sel) { bt_forget_armed = bt_sel; scpy(m_msg, "Backspace again to forget it", sizeof m_msg); return; }
            bt_forget_armed = -1; r = bt_cmd(AN_BT_CMD_FORGET, a, nm);
            if (!r) scpy(m_msg, "forgotten (pairings.txt follows)", sizeof m_msg);
        } else if (u == 0x28) {
            bt_forget_armed = -1;
            if (bt_sel == 0) { r = bt_cmd(AN_BT_CMD_SCAN, 0, 0); if (!r) scpy(m_msg, "looking: put the device in pairing mode", sizeof m_msg); }
            else if (!paired) { r = bt_cmd(AN_BT_CMD_PAIR, a, nm); if (!r) scpy(m_msg, "pairing: a keyboard shows a passkey above", sizeof m_msg); }
            else { r = bt_cmd(AN_BT_CMD_CONNECT, a, nm); if (!r) scpy(m_msg, "connecting: wake the device (a key, a move)", sizeof m_msg); }
        } else return;
        if (r == -1) scpy(m_msg, "Bluetooth is not ready", sizeof m_msg);
        if (r == -2) scpy(m_msg, "busy with the command before: again in a moment", sizeof m_msg);
        return;
    }
    if (menu_list) {
        volatile struct an_unit *un = unit(0);
        int n = un ? (int)un->scan_count : 0;
        if (u == 0x29) { menu_list = 0; menu_sel = 0; return; }
        if (u == 0x52 && menu_sel > 0) menu_sel--;
        if (u == 0x51 && menu_sel + 1 < n) menu_sel++;
        if (u == 0x28 && menu_sel < n) {
            volatile struct an_scan *sc = (volatile struct an_scan *)((volatile uint8_t *)alice_net_board() + AN_OFF_SCAN);
            unsigned q = 0; for (; q < 32 && sc[menu_sel].ssid[q]; q++) m_ssid[q] = sc[menu_sel].ssid[q];
            m_ssid[q] = 0;
            menu_list = 0; menu_sel = 2; m_changed = 1;
            menu_edit = 2; m_len = 0;                             /* the password next */
        }
        return;
    }
    if (u == 0x29) { menu_on = 0; menu_bt = 0; return; }
    if (u == 0x52) { menu_sel = menu_sel ? menu_sel - 1 : MI_N - 1; return; }
    if (u == 0x51) { menu_sel = (menu_sel + 1) % MI_N; return; }
    if (u != 0x28) return;
    m_msg[0] = 0;
    static const char *const fpus[] = { "exact", "double", "none" }, *const boots[] = { "auto", "image", "scsi" };
    switch (menu_sel) {
    case 0: if (wifi_cmd(AN_CMD_SCAN, 0, 0)) scpy(m_msg, "no WiFi radio, or it is busy", sizeof m_msg); else { menu_list = 1; menu_sel = 0; } break;
    case 1: menu_edit = 1; m_len = slen(m_ssid); for (unsigned i = 0; i < m_len; i++) m_buf[i] = m_ssid[i]; break;
    case 2: menu_edit = 2; m_len = 0; break;
    case 3: menu_edit = 3; m_len = 0; break;
    case 4: cycle(m_fpu, sizeof m_fpu, fpus, 3); break;
    case 5: cycle(m_boot, sizeof m_boot, boots, 3); break;
    case 6: cons_load(); cycle(m_console, sizeof m_console, cons, ncons); break;
    case 7: menu_save(); break;
    case 8: if (cmdline_has_recovery()) cl_drop_recovery();
            scpy(m_msg, "restarting ...", sizeof m_msg); did("restart normally, from the keyboard menu"); pi_restart_in(1000); break;
    case 10: if (ap_active()) scpy(m_msg, "the access point is already running", sizeof m_msg); else { ap_request("the keyboard menu"); menu_on = 0; } break;
    case 11: menu_bt = 1; bt_sel = 0; bt_forget_armed = -1; break;
    default: { static const char t[] = "asked for from the keyboard menu\n"; psmac_card_rec_put("recovery.txt", (const uint8_t *)t, sizeof t - 1);
            scpy(m_msg, "restarting into recovery ...", sizeof m_msg); did("restart into recovery, from the keyboard menu"); pi_restart_in(1000); break; }
    }
}
int psmac_recovery_key(uint32_t usage, int down, uint32_t mods)
{
    if (!rec_on) {
        /* a normal start: Esc held a second within the first twenty seconds asks for recovery (the poll) */
        if (usage == 0x29) {
            if (down == 1 && start_ms && now_ms() - start_ms < ESC_WINDOW_MS) { uint32_t t_ = now_ms(); esc_down_at = t_ ? t_ : 1; }
            else if (!down) esc_down_at = 0;
        }
        return 0;
    }
    if (down == 1 && usage && diag_screen == 2) { diag_back(); return 1; }   /* any key gives the screens back from the bus check's report */
    if (down == 1 && usage) menu_key(usage, mods);
    return 1;
}
/* the Plus's and the 512K's keyboards, as ADB key codes (psmac_board_plus.c): the same menu */
void psmac_recovery_adb_key(uint8_t code, int up)
{
    static const uint8_t to_hid[0x40] = {
        0x04, 0x16, 0x07, 0x09, 0x0b, 0x0a, 0x1d, 0x1b, 0x06, 0x19, 0, 0x05, 0x14, 0x1a, 0x08, 0x15,   /* a s d f h g z x c v - b q w e r */
        0x1c, 0x17, 0x1e, 0x1f, 0x20, 0x21, 0x23, 0x22, 0x2e, 0x26, 0x24, 0x2d, 0x25, 0x27, 0x30, 0x12, /* y t 1 2 3 4 6 5 = 9 7 - 8 0 ] o */
        0x18, 0x2f, 0x0c, 0x13, 0x28, 0x0f, 0x0d, 0x34, 0x0e, 0x33, 0x31, 0x36, 0x38, 0x11, 0x10, 0x37, /* u [ i p ret l j ' k ; \ , / n m . */
        0x2b, 0x2c, 0x35, 0x2a, 0, 0x29, 0, 0, 0, 0, 0, 0x50, 0x4f, 0x51, 0x52, 0,                       /* tab space ` del - esc ... left right down up */
    };
    static int shift;
    if (code == 0x38 || code == 0x7b) { shift = !up; return; }
    if (code >= 0x40 || up) return;
    if (to_hid[code]) psmac_recovery_key(to_hid[code], 1, shift ? 0x02 : 0);
}

/* ====================================================================== the service loop's turn */
static void card_first_look(void)
{
    psmac_card_rec_repair();
    struct rec_ent e;
    have_old = !psmac_card_rec_find("kernel8.old", &e);
    uint32_t n = 0;
    psmac_card_rec_list(count_cb, &n);
    card_files = n;
    /* config.txt naming another kernel: the page says so, since kernel8.img is then not what starts */
    int k = psmac_card_rec_get("config.txt", text_buf, sizeof text_buf - 1);
    cfg_kernel[0] = 0;
    for (int i = 0; k > 0 && i < k; i++) {
        if ((i == 0 || text_buf[i - 1] == '\n') && starts((const char *)text_buf + i, "kernel=")) {
            unsigned j = 0; const char *v = (const char *)text_buf + i + 7;
            while (j < sizeof cfg_kernel - 1 && v + j < (const char *)text_buf + k && v[j] != '\n' && v[j] != '\r' && v[j] != ' ') { cfg_kernel[j] = v[j]; j++; }
            cfg_kernel[j] = 0;
        }
    }
    if (ieq(cfg_kernel, "kernel8.img")) cfg_kernel[0] = 0;
    /* the programmer's switch made this start a recovery start after debug-once.txt was taken (phase 2 comes after
       the boot arguments are read): it goes back for the next normal start */
    once_armed = !psmac_card_rec_find("debug-once.txt", &e);
    if (once_taken && !psmac_card_once_rearm()) { once_taken = 0; once_used[0] = 0; once_armed = 1; did("the one-shot debug start is kept for the next normal start (this one became Setup and Recovery)"); }
    did("the card is ready: %s, %u entries in its root%s", psmac_card_rec_what(), card_files, have_old ? ", kernel8.old there" : "");
}
void psmac_recovery_poll(void)
{
    if (!start_ms) start_ms = now_ms() | 1;
    if (!rec_on) {
        if (esc_down_at && now_ms() - esc_down_at >= ESC_HOLD_MS) { esc_down_at = 0; psmac_recovery_ask("Esc held on a USB keyboard during the start"); }
        ask_poll();
        reset_poll();
        return;
    }
    static int init;
    if (!init) {
        init = 1;
        U.conn = -1;
        new_pin();
        psmac_ip_init_recovery("aurora68");
        kprintf("[RECOVERY] the page: http://%s.local/ (and the address the screen shows); PIN shown on screen\n", psmac_ip_name());
        if (switch_board()) kprintf("[RECOVERY] the programmer's interrupt button held two seconds on this screen starts an access point (docs/08 item 118)\n");
        scr_dirty = 1;
        if (ap_asked[0]) ap_request(ap_asked);
    }
    if (!card_seen && psmac_card_rec_ready() != 0) { card_seen = 1; card_first_look(); }
    /* the stack every turn while it has work, else once a millisecond (QEMU emulates every timer read) */
    static uint64_t next_round; static uint32_t turns;
    int busy = psmac_ip_busy() || U.state == UP_RECV || U.state == UP_VERIFY;
    if (!busy && (++turns & 31)) return;
    uint64_t t = now_ticks();
    if (!busy && (int64_t)(t - next_round) < 0) return;
    next_round = t + tick_hz() / 1000;
    psmac_ip_poll();
    up_pump();
    card_test_poll();
    wifi_poll();
    wifi_test_poll2();
    bt_poll_rec();
    switch_poll();
    ap_poll();
    reset_button_poll();
    screen_poll();
    reset_poll();
}

/* ==================================================================================== the page
   Plain HTML and a little JavaScript, for a phone as much as a laptop: the PIN once (kept for the tab), the
   state, the files with Get, Rename and Delete, an upload with its progress, the WiFi settings with a scan and
   a test join, the common options as a form (the line it makes shown before it is saved), the two text files,
   the logs and the restarts. Since 22 November 2026 (the owner: "tabbed for each section and modern in
   design") it is tabbed (Overview, Files, WiFi, Options, Text files, Log, and since docs/08 item 111 Display, the
   HDMI monitor's mode), and lives in tools/recovery/page.html, made into psmac_recovery_page.h by
   tools/recovery/mkpage.py. */
#include "psmac_recovery_page.h"         /* the page itself: tools/recovery/page.html, made into C by tools/recovery/mkpage.py */
