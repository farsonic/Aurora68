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

const char *psmac_option(const char *key);
extern int psmac_board_se, psmac_board_solo, psmac_board_plus;
extern volatile uint8_t *alice_net_board(void);
int psmac_se_switch_held(void);
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
    char flag[120]; int flag_len = -1, count = -1;
    int card = psmac_card_boot_read(&count, flag, sizeof flag, &flag_len) == 0;
    if (!card) kprintf("[RECOVERY] the card was not read before the 68k starts (a bare-ROM testbed run: mac.cardearly=1): no boot counter, no recovery.txt\n");
    boot_count = count;
    if (want_opt) enter(WHY_OPTION, "mac.recovery=1 in cmdline.txt");
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
static char line[SROWS][SCOLS + 1]; static uint8_t lattr[SROWS]; static int nlines;   /* attr 1 title, 2 large */
static int scr_dirty = 1; static uint32_t scr_last;
static uint8_t se_fb[342 * 64] __attribute__((aligned(16)));
static void L(int attr, const char *fmt, ...)
{
    if (nlines >= SROWS) return;
    struct sb b = { line[nlines], 0, SCOLS + 1 };
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
/* the compact Mac's own screen: 512 x 342, one bit, 1 is black; 64 columns, 21 rows of 16 */
static void draw_se(void)
{
    if (!psmac_board_se || psmac_board_solo) return;
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

static void build_page(void)
{
    nlines = 0;
    struct ipk_info ii; psmac_ip_info(&ii);
    L(1, " Aurora68 Setup and Recovery");
    L(0, "Why: %s", why_text);
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
    if (pin_changes) L(0, "%u x 10 wrong PINs: a new PIN each time", pin_changes);
    int ready = psmac_card_rec_ready();
    L(0, "Card:  %s%s", ready == 0 ? "being read" : ready == -1 ? "none" : psmac_card_rec_what(), ready == -2 ? " (read only)" : "");
    if (U.state == UP_RECV && U.total) L(0, "Now:   %s %u of %u KB (%u%%)", U.target, U.got >> 10, U.total >> 10, (uint32_t)((uint64_t)U.got * 100u / U.total));
    else if (U.state == UP_VERIFY) L(0, "Now:   %s: reading it back and checking", U.target);
    else L(0, "Last:  %s", last_action[0] ? last_action : "nothing yet");
    if (why == WHY_COUNTER && have_old) L(0, "kernel8.old is on the card: the page offers Roll back");
    if (cfg_kernel[0]) L(0, "Note: config.txt starts %s, not kernel8.img", cfg_kernel);
    menu_lines();
}
static void screen_poll(void)
{
    uint32_t t = now_ms();
    if (!scr_dirty && t - scr_last < 2000) return;
    if (t - scr_last < 200) return;
    scr_last = t; scr_dirty = 0;
    build_page();
    draw_vc();
    draw_se();
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
static int is_secret(const char *k) { return ieq(k, "wifi.psk") || ieq(k, "remote.token") || ieq(k, "mac.wifisim"); }
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
/* the options the page and the menu know: allowed values, "#" a number, "WxH" a size, "" any word */
struct known { const char *key, *values; };
static const struct known known_opts[] = {
    { "mac.board", "se|plus|512ke|512k|128k|solo" }, { "mac.fpu", "exact|double|none" }, { "mac.sane", "rom|se" },
    { "mac.boot", "auto|image|scsi" }, { "mac.ram", "#" }, { "mac.console", "hdmi" }, { "mac.sesound", "off|on" },
    { "mac.screen", "WxH" }, { "mac.seram", "1|2|2.5|4" }, { "mac.plusmouse", "#" }, { "mac.plusdebounce", "#" }, { "mac.plusjitter", "#" },
    { "mac.screencheck", "on|blind|off" }, { "bt.pair", "" }, { "eth", "usb|off" }, { "remote", "on|off" }, { "remote.token", "" },
    { "wifi.ssid", "" }, { "wifi.psk", "" }, { "wifi.country", "" }, { "mac.recovery", "1" }, { "mac.bootcount", "#|off" },
    { "wifi", "off" }, { "bt", "off" },
};
static const struct known *known_find(const char *k) { for (unsigned i = 0; i < sizeof known_opts / sizeof known_opts[0]; i++) if (ieq(known_opts[i].key, k)) return &known_opts[i]; return 0; }
static int value_ok(const struct known *kn, const char *v, char *why_, unsigned whymax)
{
    if (!kn || !kn->values[0]) return 1;
    for (const char *a = kn->values; *a; ) {
        const char *e = a; while (*e && *e != '|') e++;
        unsigned l = (unsigned)(e - a);
        if (l == 1 && a[0] == '#') { const char *p = v; if (*p) { while (*p >= '0' && *p <= '9') p++; if (!*p) return 1; } }
        else if (l == 3 && starts(a, "WxH")) { const char *p = v; int d1 = 0, d2 = 0; while (*p >= '0' && *p <= '9') { p++; d1++; } if (*p == 'x') { p++; while (*p >= '0' && *p <= '9') { p++; d2++; } } if (!*p && d1 && d2) return 1; }
        else if (slen(v) == l) { unsigned i = 0; while (i < l && lower(v[i]) == lower(a[i])) i++; if (i == l) return 1; }
        a = *e ? e + 1 : e;
    }
    struct sb b = { why_, 0, whymax };
    bprintf(&b, "%s: %s is not one of %s", kn->key, v, kn->values);
    return 0;
}
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
    const char *txt = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" :
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
static int cmdline_has_recovery(void) { char v[8]; return cl_read() == 0 && pcl_get(cl_old, (uint32_t)cl_len, "mac.recovery", v, sizeof v) == 1 && v[0] == '1'; }
static void ans_state(struct rconn *h)
{
    static char j[4096]; struct sb b = { j, 0, sizeof j };
    struct ipk_info ii; psmac_ip_info(&ii);
    static const char *const wn[] = { "none", "option", "request", "counter", "switch" };
    bput(&b, "{\"recovery\": true, \"reason\": "); bjson(&b, wn[why]); bput(&b, ", \"why\": "); bjson(&b, why_text);
    bprintf(&b, ", \"board\": \"%s\", \"boot_count\": %d, \"boot_limit\": %d", psmac_board_solo ? "solo" : psmac_board_plus ? "plus" : psmac_board_se ? "se" : "iici", boot_count, boot_limit);
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
static void ans_options(struct rconn *h, int post, int dry, char *body, uint32_t n)
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
                        if (!value_ok(known_find(l), v, why_, sizeof why_)) { json_err(h, 422, why_); return; }
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
    static char j[8192]; struct sb b = { j, 0, sizeof j };
    static char r[CL_MAX + 512];
    redacted(cl_new, r, sizeof r);
    bput(&b, "{\"line\": "); bjson(&b, r);
    bprintf(&b, ", \"saved\": %s, \"values\": {", post && !dry ? "true" : "false");
    for (unsigned i = 0; i < sizeof known_opts / sizeof known_opts[0]; i++) {
        char v[96]; int k = pcl_get(cl_new, nn, known_opts[i].key, v, sizeof v);
        bput(&b, i ? ", " : ""); bjson(&b, known_opts[i].key); bput(&b, ": ");
        if (!k) bput(&b, "null");
        else if (is_secret(known_opts[i].key)) bput(&b, "\"(set)\"");
        else bjson(&b, k == 2 ? "(flag)" : v);
    }
    bput(&b, "}}\n");
    respond(h, 200, "application/json", j);
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
        if (cmdline_has_recovery()) {                            /* "normally" means without mac.recovery=1 */
            uint32_t nn = (uint32_t)cl_len;
            for (uint32_t i = 0; i < nn; i++) cl_new[i] = cl_old[i];
            if (cl_set(&nn, "mac.recovery", 0) || cl_write(nn, why_, sizeof why_)) { json_err(h, 500, "mac.recovery=1 could not be taken out of cmdline.txt"); return; }
            took = 1;
        }
        did("restart normally asked for%s", took ? ": mac.recovery=1 taken out of cmdline.txt (cmdline.bak keeps the line)" : "");
        respond(h, 200, "application/json", took ? "{\"restart\": \"normal\", \"note\": \"mac.recovery=1 was taken out of cmdline.txt\"}\n" : "{\"restart\": \"normal\"}\n");
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
    if ((get || post) && !__builtin_strcmp(path, "options")) { ans_options(h, post, query(q, "dry", v, sizeof v) && v[0] == '1', body, clen); return; }
    if ((get && !__builtin_strcmp(path, "wifi")) || (post && starts(path, "wifi/"))) { ans_wifi(h, get ? "" : path + 5, body, clen); return; }
    if (post && !__builtin_strcmp(path, "reboot")) { ans_reboot(h, query(q, "how", v, sizeof v)); return; }
    if (post && !__builtin_strcmp(path, "rollback")) { ans_rollback(h, query(q, "force", v, sizeof v) && v[0] == '1'); return; }
    if (get && !__builtin_strcmp(path, "log")) { ans_log(h, c, query(q, "now", v, sizeof v) && v[0] == '1'); return; }
    json_err(h, 404, "GET /r/state, /r/files, /r/file, /r/text, /r/options, /r/wifi, /r/log; PUT /r/file, /r/text; POST /r/rm, /r/mv, /r/options, /r/wifi/..., /r/reboot, /r/rollback");
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
#define MI_N 10
static void menu_load(void)
{
    char v[72];
    m_ssid[0] = m_psk[0] = m_country[0] = 0; m_psk_set = 0; m_changed = 0;
    scpy(m_fpu, "exact", sizeof m_fpu); scpy(m_boot, "auto", sizeof m_boot); scpy(m_console, "off", sizeof m_console);
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
    if (!ieq(m_console, m0_console)) e |= cl_set(&nn, "mac.console", ieq(m_console, "hdmi") ? "hdmi" : 0);
    if (e || cl_write(nn, why_, sizeof why_)) { scpy(m_msg, "not saved: the line would not be valid", sizeof m_msg); return; }
    m_changed = 0; m_psk_set = m_psk[0] ? 1 : 0;
    for (unsigned i = 0; i < sizeof m_psk; i++) m_psk[i] = 0;
    menu_load();
    scpy(m_msg, "saved to cmdline.txt (the line before: cmdline.bak)", sizeof m_msg);
    did("settings saved from the keyboard menu (cmdline.bak keeps the line before)");
}
static void menu_lines(void)
{
    if (!menu_on) { L(0, ""); L(0, "Keyboard: Return opens the settings menu (WiFi, options)"); return; }
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
        default: bput(&b, "Restart into recovery"); break;
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
    if (u == 0x29) { menu_on = 0; return; }
    if (u == 0x52) { menu_sel = menu_sel ? menu_sel - 1 : MI_N - 1; return; }
    if (u == 0x51) { menu_sel = (menu_sel + 1) % MI_N; return; }
    if (u != 0x28) return;
    m_msg[0] = 0;
    static const char *const fpus[] = { "exact", "double", "none" }, *const boots[] = { "auto", "image", "scsi" }, *const cons[] = { "off", "hdmi" };
    switch (menu_sel) {
    case 0: if (wifi_cmd(AN_CMD_SCAN, 0, 0)) scpy(m_msg, "no WiFi radio, or it is busy", sizeof m_msg); else { menu_list = 1; menu_sel = 0; } break;
    case 1: menu_edit = 1; m_len = slen(m_ssid); for (unsigned i = 0; i < m_len; i++) m_buf[i] = m_ssid[i]; break;
    case 2: menu_edit = 2; m_len = 0; break;
    case 3: menu_edit = 3; m_len = 0; break;
    case 4: cycle(m_fpu, sizeof m_fpu, fpus, 3); break;
    case 5: cycle(m_boot, sizeof m_boot, boots, 3); break;
    case 6: cycle(m_console, sizeof m_console, cons, 2); break;
    case 7: menu_save(); break;
    case 8: if (cmdline_has_recovery()) { uint32_t nn = (uint32_t)cl_len; char w2[100]; for (uint32_t i = 0; i < nn; i++) cl_new[i] = cl_old[i]; if (!cl_set(&nn, "mac.recovery", 0)) cl_write(nn, w2, sizeof w2); }
            scpy(m_msg, "restarting ...", sizeof m_msg); did("restart normally, from the keyboard menu"); pi_restart_in(1000); break;
    default: { static const char t[] = "asked for from the keyboard menu\n"; psmac_card_rec_put("recovery.txt", (const uint8_t *)t, sizeof t - 1);
            scpy(m_msg, "restarting into recovery ...", sizeof m_msg); did("restart into recovery, from the keyboard menu"); pi_restart_in(1000); break; }
    }
}
int psmac_recovery_key(uint32_t usage, int down, uint32_t mods)
{
    if (!rec_on) {
        /* a normal start: Esc held a second within the first twenty seconds asks for recovery (the poll) */
        if (usage == 0x29) {
            if (down == 1 && start_ms && now_ms() - start_ms < ESC_WINDOW_MS) esc_down_at = now_ms() | 1;
            else if (!down) esc_down_at = 0;
        }
        return 0;
    }
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
        scr_dirty = 1;
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
    wifi_poll();
    wifi_test_poll2();
    screen_poll();
    reset_poll();
}

/* ==================================================================================== the page
   Plain HTML and a little JavaScript, for a phone as much as a laptop: the PIN once (kept for the tab), the
   state, the files with Get, Rename and Delete, an upload with its progress, the WiFi settings with a scan and
   a test join, the common options as a form (the line it makes shown before it is saved), the two text files,
   the logs and the restarts. Since 22 November 2026 (the owner: "tabbed for each section and modern in
   design") it is tabbed (Overview, Files, WiFi, Options, Text files, Log), follows the system's light or dark
   mode, and lives in tools/recovery/page.html, made into psmac_recovery_page.h by tools/recovery/mkpage.py. */
#include "psmac_recovery_page.h"         /* the page itself: tools/recovery/page.html, made into C by tools/recovery/mkpage.py */
