/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_remote.c - the remote control: the Mac's screen, mouse, keyboard, log and reset, over HTTP
 * (docs/13).
 *
 * Backstory. Problems on the user's bench reached the project as photographs of the screen, and
 * reproducing them meant a person at the Mac. On 20 November the user asked for a way for an
 * agent on their MacBook to see and drive the Mac directly. The kernel already owns everything that
 * takes: it converts the Mac's frame buffer for HDMI (psmac_display.c), it feeds .PiInput's ring
 * with every USB and Bluetooth key and mouse movement (alice_psmac.c), it keeps the log PiLog shows,
 * and it runs the 68k. So the remote is a kernel service. It needs no program on the Mac, and it
 * still answers when the Mac has hung or shows a system error, which is when it is wanted most.
 *
 * What it offers (each one a function the HTTP layer calls):
 *   GET  /            a small page for a person: the screen, click to click, a box to type in
 *   GET  /screen.png  a screen as PNG (?display=0|1: the first or the second screen, ?scale=2|4: smaller;
 *                     ?src=hdmi: the VideoCore buffer)
 *   POST /input       JSON events: move, down, up, click, drag, key, keydown, keyup, text, wait
 *   GET  /log         the kernel's log from byte N (?since=N), secrets hidden (?tags=BOOT,CRASH)
 *   GET  /state       JSON: board, screen, the application in front, Ticks, crashes, network, disks
 *   POST /restart     the Mac restarts (?how=nmi: the programmer's switch; ?how=pi: the whole Pi)
 * Every request but the page needs the token (remote.token=, as X-Aurora-Token or Authorization:
 * Bearer), and requests from outside the local network are refused unless remote.any=1.
 *
 * Transport. The TCP connections of psmac_ip.c, and in the testbed a semihosting channel
 * (mac.remotefile=BASE: requests appended to BASE.in, answers to BASE.out, each as a 32-bit length
 * and the bytes) so that every capability could be tried before the network existed. Both are
 * served by the same code: a request is collected, then its answer is produced a piece at a time
 * as the transport asks for bytes. Nothing here waits; a screenshot is encoded a band of rows per
 * request for bytes, so the service loop, and with it the Mac's disks and network, keeps going.
 *
 * The PNG encoder is our own and small: RGB, 8 bits, filter 0, one deflate block with the fixed
 * Huffman codes, and an LZ77 search over four distances only: one and two pixels back, and the
 * same place one and two rows up. That suits a Mac screen (flat colour, dither patterns, text):
 * a Finder desktop compresses forty to a hundred times. Each band of rows goes out as an IDAT chunk
 * of its own, so nothing is held back for the whole image.
 */
#include <stdint.h>
#include "support.h"
#include "psmac.h"
#include "psmb.h"
#include "../alice/alice_psmac.h"
#include "../alice/alice_net.h"
#include "psmac_ip.h"

const char *psmac_option(const char *key);                                  /* psmac_bus.c */
uint32_t psmac_mac_rd32(uint32_t logical, int *ok);                         /* psmac_bus.c */
int64_t psmac_sh_open(const char *name, uint64_t mode);
int     psmac_sh_read(int64_t fd, void *buf, uint64_t len);
int     psmac_sh_write(int64_t fd, const void *buf, uint64_t len);
int64_t psmac_sh_seek(int64_t fd, uint64_t pos);
int64_t psmac_sh_flen(int64_t fd);
int     psmac_sh_available(void);
int  psmac_display_shot(uint32_t *w, uint32_t *h, uint32_t *depth, int display, int want_hdmi);   /* psmac_display.c */
int  psmac_display_info(int i, uint32_t *w, uint32_t *h, uint32_t *depth, uint32_t *base);
int  psmac_display_count(void);
void psmac_display_shot_row(uint32_t y, uint8_t *rgb, uint32_t maxw);
void psmac_crash_last(uint32_t *exc_n, uint32_t *exc_vec, uint32_t *exc_pc, const char **exc_what,
                      uint32_t *sys_n, int16_t *sys_id, const char **sys_what);  /* psmac_crash.c */
const char *psmac_crash_syserr_name(int16_t id);
int psmac_image_count(void);                                                 /* psmac_board_solo.c */
int psmac_image_get(int i, const char **name, uint8_t **data, uint32_t *blocks);
int psmac_image_is_scsi(int i);
extern int psmac_board_se, psmac_board_solo;
extern volatile uint8_t *alice_net_board(void);
extern struct M68KState *__m68k_state;
void sprintf(char *buf, const char *format, ...);                           /* support.c */

static inline uint64_t now_ticks(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t tick_hz(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static uint32_t now_ms(void) { return (uint32_t)(now_ticks() / (tick_hz() / 1000u)); }
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static int ieq_n(const char *a, const char *b, unsigned n) { for (unsigned i = 0; i < n; i++) if (lower(a[i]) != lower(b[i])) return 0; return 1; }

/* ---- configuration ---- */
static int enabled, any_source;
static char token[80]; static unsigned token_len;
static uint8_t *win;                                    /* the slot window, kernel side */

/* ======================================================================================= input */
/* The events the remote asks for wait here with the pause that follows each, and go into .PiInput's
   ring one at a time: the Mac's event queue holds twenty events, and a program busy drawing takes
   them slowly, so a string typed all at once would lose letters. The first test typed a sentence into
   Octavo under QEMU, where it redraws its page for a second after every key, and kept twelve of its
   sixty letters. So an event also waits while the Mac has not taken the last ones: while .PiInput's
   ring holds any, or the Event Manager's queue (EventQueue, $14A) holds eight or more. */
#define IQ 1024
static struct { uint8_t type; int16_t a, b; int16_t c; uint16_t gap_ms; } iq[IQ];
static uint32_t iq_head, iq_tail, iq_due;
static int mouse_x, mouse_y, buttons, mods_now;          /* where the remote last put things */
static uint32_t input_events;
#define EV_MODS 0                                        /* the modifiers alone: ALICE_IN_KEY with usage 0 */
static int iq_put(uint32_t type, int a, int b, int c, uint32_t gap_ms)
{
    if (iq_head - iq_tail >= IQ) return 0;
    iq[iq_head % IQ].type = (uint8_t)type; iq[iq_head % IQ].a = (int16_t)a; iq[iq_head % IQ].b = (int16_t)b;
    iq[iq_head % IQ].c = (int16_t)c; iq[iq_head % IQ].gap_ms = (uint16_t)gap_ms;
    iq_head++;
    return 1;
}
/* the events in the Event Manager's queue, at most 20 counted: EventQueue's qHead at $14C, qLink first in each */
static int mac_events_queued(void)
{
    int ok, n = 0;
    uint32_t mask = (psmac_mac_rd32(0xcb0, &ok) >> 8 & 0xff) ? 0xffffffffu : 0x00ffffffu;   /* MMU32Bit ($CB2): 24-bit pointers carry flags */
    uint32_t e = psmac_mac_rd32(0x14c, &ok) & mask;
    while (ok && e && n < 20) { n++; e = psmac_mac_rd32(e, &ok) & mask; }
    return n;
}
static void input_poll(void)
{
    uint32_t t = now_ms();
    if (iq_tail == iq_head || (int32_t)(t - iq_due) < 0) return;
    /* the Mac is busy: in a moment; but not for ever, since nothing may be taking events at all
       (the system error dialog, the debugger: they read the keyboard their own way) */
    static uint32_t busy_since;
    if (psmac_input_backlog() || mac_events_queued() >= 8) {
        if (!busy_since) busy_since = t;
        if (t - busy_since < 2000) { iq_due = t + 10; return; }
    }
    busy_since = 0;
    uint32_t k = iq_tail % IQ;
    if (iq[k].type != 255) {                                     /* 255: a pause only */
        if (iq[k].type == ALICE_IN_KEY) psmac_input_put(ALICE_IN_KEY, 0, 0, iq[k].c);
        else psmac_input_put(iq[k].type, iq[k].a, iq[k].b, iq[k].c);
        input_events++;
    }
    iq_due = t + iq[k].gap_ms;
    iq_tail++;
}
/* the 68k was reset: what was queued was for the session that has gone */
void psmac_remote_mac_reset(void);
static uint32_t reset_exc_n, reset_sys_n, mac_resets;
static uint32_t reset_ms;
void psmac_remote_mac_reset(void)
{
    iq_tail = iq_head; buttons = 0;
    const char *w1, *w2; uint32_t v, pc; int16_t id;
    psmac_crash_last(&reset_exc_n, &v, &pc, &w1, &reset_sys_n, &id, &w2);
    mac_resets++; reset_ms = now_ms();
}

/* HID modifier byte: Control 0x01, Shift 0x02, Option (Alt) 0x04, Command (GUI) 0x08 */
static int parse_mods(const char *s)
{
    int m = 0;
    while (s && *s) {
        while (*s == ',' || *s == '+' || *s == ' ' || *s == '-') s++;
        const char *e = s; while (*e && *e != ',' && *e != '+' && *e != ' ' && *e != '-') e++;
        unsigned n = (unsigned)(e - s);
        if ((n == 3 && ieq_n(s, "cmd", 3)) || (n == 7 && ieq_n(s, "command", 7)) || (n == 4 && ieq_n(s, "meta", 4))) m |= 0x08;
        else if ((n == 5 && ieq_n(s, "shift", 5))) m |= 0x02;
        else if ((n == 3 && ieq_n(s, "opt", 3)) || (n == 6 && ieq_n(s, "option", 6)) || (n == 3 && ieq_n(s, "alt", 3))) m |= 0x04;
        else if ((n == 4 && ieq_n(s, "ctrl", 4)) || (n == 7 && ieq_n(s, "control", 7))) m |= 0x01;
        s = e;
    }
    return m;
}
/* The US layout: a character's Mac virtual key code (Inside Macintosh: Text, "Virtual Key Codes"),
   with bit 8 for Shift. The Mac turns the code into a character through its own KCHR, so a Mac set
   to another layout types what that layout has on the key. */
static int char_key(int ch)
{
    static const char lower_keys[] = "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\0lj'k;\\,/nm.\0\0`";
    static const char shifted[] = "ASDFHGZXCV\0BQWERYT!@#$^%+(&_*)}OU{IP\0LJ\"K:|<?NM>\0\0~";
    if (ch == '\r' || ch == '\n') return 0x24;
    if (ch == '\t') return 0x30;
    if (ch == ' ') return 0x31;
    if (ch == 8 || ch == 127) return 0x33;
    if (ch == 27) return 0x35;
    for (int i = 0; i < (int)sizeof lower_keys - 1; i++) {
        if (lower_keys[i] && lower_keys[i] == ch) return i;
        if (shifted[i] && shifted[i] == ch) return i | 0x100;
    }
    return -1;
}
static const struct { const char *name; uint8_t code; } key_names[] = {
    { "return", 0x24 }, { "enter", 0x4c }, { "tab", 0x30 }, { "space", 0x31 }, { "delete", 0x33 }, { "backspace", 0x33 },
    { "escape", 0x35 }, { "esc", 0x35 }, { "left", 0x7b }, { "right", 0x7c }, { "down", 0x7d }, { "up", 0x7e },
    { "home", 0x73 }, { "end", 0x77 }, { "pageup", 0x74 }, { "pagedown", 0x79 }, { "forwarddelete", 0x75 }, { "help", 0x72 },
    { "f1", 0x7a }, { "f2", 0x78 }, { "f3", 0x63 }, { "f4", 0x76 }, { "f5", 0x60 }, { "f6", 0x61 }, { "f7", 0x62 },
    { "f8", 0x64 }, { "f9", 0x65 }, { "f10", 0x6d }, { "f11", 0x67 }, { "f12", 0x6f }, { "capslock", 0x39 },
};
/* a key by name, by one character, or by number: its code and the Shift it needs; -1 if unknown */
static int key_code(const char *name, int code_given)
{
    if (code_given >= 0) return code_given & 0x7f;
    if (!name || !name[0]) return -1;
    if (!name[1]) return char_key(name[0]);
    unsigned n = slen(name);
    for (unsigned i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
        if (slen(key_names[i].name) == n && ieq_n(name, key_names[i].name, n)) return key_names[i].code;
    return -1;
}
static void q_mods(int m, uint32_t gap) { if (m != mods_now) { iq_put(ALICE_IN_KEY, 0, 0, m, gap); mods_now = m; } }
static void q_key(int code, int down, int m) { iq_put(ALICE_IN_MACKEY, code & 0x7f, down, m, down ? 30 : 40); }
static void q_press(int code, int m)
{
    int need = m | ((code & 0x100) ? 0x02 : 0);
    q_mods(need, 20);
    q_key(code, 1, need); q_key(code, 0, need);
}
static void q_move(int x, int y) { mouse_x = x; mouse_y = y; iq_put(ALICE_IN_MOUSEABS, x, y, buttons, 30); }
static void q_button(int down, int which)
{
    int bit = which == 2 ? 2 : which == 1 ? 4 : 1;               /* right is a Command-click, middle an Option-click (.PiInput) */
    buttons = down ? (buttons | bit) : (buttons & ~bit);
    iq_put(ALICE_IN_MOUSEABS, mouse_x, mouse_y, buttons, down ? 80 : 60);
}

/* ---- a tiny JSON reader: an object, or an array of objects, of strings, numbers and booleans ---- */
struct jfield { char key[16]; char str[1024]; int num; int has_num; };
#define JF 12
static const char *jskip(const char *p, const char *e) { while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')) p++; return p; }
static const char *jstring(const char *p, const char *e, char *out, unsigned max)
{
    unsigned n = 0;
    if (p >= e || *p != '"') return 0;
    for (p++; p < e && *p != '"'; p++) {
        int c = (uint8_t)*p;
        if (c == '\\' && p + 1 < e) {
            p++;
            c = *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p == 't' ? '\t' : *p == 'b' ? 8 : *p;
            if (*p == 'u' && p + 4 < e) {                        /* \u00XX: Latin-1 only */
                int v = 0; for (int i = 1; i <= 4; i++) { char h = p[i]; v = v * 16 + (h >= '0' && h <= '9' ? h - '0' : (lower(h) - 'a' + 10) & 15); }
                c = v < 256 ? v : '?'; p += 4;
            }
        }
        if (n + 1 < max) out[n++] = (char)c;
    }
    out[n] = 0;
    return p < e ? p + 1 : 0;
}
/* one object at p into f[]; returns the byte after it, 0 on an error */
static const char *jobject(const char *p, const char *e, struct jfield *f, int *nf)
{
    *nf = 0;
    p = jskip(p, e);
    if (p >= e || *p != '{') return 0;
    p++;
    for (;;) {
        p = jskip(p, e);
        if (p >= e) return 0;
        if (*p == '}') return p + 1;
        struct jfield *x = &f[*nf < JF ? *nf : JF - 1];
        p = jstring(p, e, x->key, sizeof x->key);
        if (!p) return 0;
        p = jskip(p, e);
        if (p >= e || *p != ':') return 0;
        p = jskip(p + 1, e);
        x->str[0] = 0; x->has_num = 0; x->num = 0;
        if (p < e && *p == '"') { p = jstring(p, e, x->str, sizeof x->str); if (!p) return 0; }
        else if (p < e && (*p == '-' || (*p >= '0' && *p <= '9'))) {
            int neg = *p == '-'; if (neg) p++;
            int v = 0; while (p < e && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
            while (p < e && (*p == '.' || (*p >= '0' && *p <= '9'))) p++;
            x->num = neg ? -v : v; x->has_num = 1;
        } else if (p + 4 <= e && starts(p, "true")) { x->num = 1; x->has_num = 1; p += 4; }
        else if (p + 5 <= e && starts(p, "false")) { x->num = 0; x->has_num = 1; p += 5; }
        else if (p + 4 <= e && starts(p, "null")) p += 4;
        else return 0;
        if (*nf < JF) (*nf)++;
    }
}
static struct jfield *jget(struct jfield *f, int nf, const char *key)
{
    for (int i = 0; i < nf; i++) if (!__builtin_strcmp(f[i].key, key)) return &f[i];
    return 0;
}
static int jnum(struct jfield *f, int nf, const char *key, int dflt) { struct jfield *x = jget(f, nf, key); return x && x->has_num ? x->num : dflt; }
static const char *jstr(struct jfield *f, int nf, const char *key) { struct jfield *x = jget(f, nf, key); return x && !x->has_num ? x->str : 0; }

/* one event object into the queue; 0 if it was not understood */
static int screen_place(int i, int16_t rect[4], uint32_t *flags);
static int input_event(struct jfield *f, int nf, char *why, unsigned why_max)
{
    const char *type = jstr(f, nf, "type");
    if (!type) { sprintf(why, "an event without \"type\""); return 0; }
    int x = jnum(f, nf, "x", -1), y = jnum(f, nf, "y", -1);
    /* "display": N makes x, y (and to_x, to_y) a point of that screen rather than of the desktop:
       its place on the desktop (the Mac's GDevice, where Monitors put it) is added */
    int display = jnum(f, nf, "display", -1), ox = 0, oy = 0;
    if (display >= 0) {
        int16_t r[4]; uint32_t fl;
        if (screen_place(display, r, &fl)) { ox = r[1]; oy = r[0]; }
        else if (display) { sprintf(why, "display %d is not a screen QuickDraw knows (yet)", display); return 0; }
        /* the first screen before QuickDraw has a device for it (the boot): at the origin */
    }
    int m = parse_mods(jstr(f, nf, "mods"));
    const char *bs = jstr(f, nf, "button");
    int btn = bs && bs[0] == 'r' ? 2 : bs && bs[0] == 'm' ? 1 : 0;
    (void)why_max;
    int has_xy = x >= 0 && y >= 0;                                  /* the point as given; then on the desktop */
    x += ox; y += oy;
    if (!__builtin_strcmp(type, "move")) { if (!has_xy) { sprintf(why, "move needs x and y"); return 0; } q_move(x, y); return 1; }
    if (!__builtin_strcmp(type, "down") || !__builtin_strcmp(type, "up")) {
        if (has_xy) q_move(x, y);
        q_mods(m, 20);
        q_button(type[0] == 'd', btn);
        if (type[0] == 'u') q_mods(0, 20);
        return 1;
    }
    if (!__builtin_strcmp(type, "click")) {
        int count = jnum(f, nf, "count", 1);
        if (count < 1) count = 1;
        if (count > 3) count = 3;
        if (has_xy) q_move(x, y);
        q_mods(m, 20);
        for (int i = 0; i < count; i++) { q_button(1, btn); q_button(0, btn); }   /* inside the Mac's double-click time */
        q_mods(0, 20);
        return 1;
    }
    if (!__builtin_strcmp(type, "drag")) {
        int x2 = jnum(f, nf, "to_x", -1), y2 = jnum(f, nf, "to_y", -1);
        if (!has_xy || x2 < 0 || y2 < 0) { sprintf(why, "drag needs x, y, to_x and to_y"); return 0; }
        x2 += ox; y2 += oy;
        /* a pause after the press and before the release: a menu is drawn and tracked only once
           MenuSelect has seen the button down, and under QEMU that took longer than the steps */
        q_move(x, y); q_mods(m, 20); q_button(1, btn); iq_put(255, 0, 0, 0, 400);
        for (int i = 1; i <= 12; i++) q_move(x + (x2 - x) * i / 12, y + (y2 - y) * i / 12);   /* steps the Finder sees as a drag */
        iq_put(255, 0, 0, 0, 300); q_button(0, btn); q_mods(0, 20);
        return 1;
    }
    if (!__builtin_strcmp(type, "key") || !__builtin_strcmp(type, "keydown") || !__builtin_strcmp(type, "keyup")) {
        int code = key_code(jstr(f, nf, "key"), jnum(f, nf, "code", -1));
        if (code < 0) { sprintf(why, "unknown key"); return 0; }
        if (type[3] == 0) { q_press(code, m); q_mods(0, 20); return 1; }
        int need = m | ((code & 0x100) ? 0x02 : 0);
        if (type[3] == 'd') { q_mods(need, 20); q_key(code, 1, need); }
        else { q_key(code, 0, need); q_mods(0, 20); }
        return 1;
    }
    if (!__builtin_strcmp(type, "text")) {
        const char *t = jstr(f, nf, "text");
        if (!t) { sprintf(why, "text needs \"text\""); return 0; }
        int skipped = 0;
        for (; *t; t++) { int code = char_key((uint8_t)*t); if (code < 0) { skipped++; continue; } q_press(code, m); }
        q_mods(0, 20);
        if (skipped) sprintf(why, "%d characters not on the US keyboard were left out", skipped);
        return 1;
    }
    if (!__builtin_strcmp(type, "wait")) { int ms = jnum(f, nf, "ms", 100); iq_put(255, 0, 0, 0, (uint32_t)(ms < 0 ? 0 : ms > 30000 ? 30000 : ms)); return 1; }
    sprintf(why, "unknown event type");
    return 0;
}

/* ======================================================================================= PNG */
static uint32_t crc_table[256];
static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) { uint32_t c = n; for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1; crc_table[n] = c; }
}
static uint32_t crc_update(uint32_t c, const uint8_t *p, uint32_t n) { while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8); return c; }

#define PNG_MAXW 2048
#define PNG_OUT  40960                                   /* one band's chunk: rows up to 16 KB raw, at worst 9/8 of it */
static struct {
    int active, conn;
    uint32_t w, h, sw, sh, scale, y, src_y;              /* output size, the next output row; the source's */
    uint8_t rows[3][1 + 3 * PNG_MAXW];                   /* the last three rows, filter byte first: the LZ77 window */
    uint8_t srow[3 * PNG_MAXW]; uint32_t acc[3 * PNG_MAXW];
    uint32_t adler_a, adler_b;
    uint64_t bits; int nbits;                            /* the deflate bit writer */
    uint8_t out[PNG_OUT]; uint32_t out_len, out_pos;
    uint32_t bytes, t0, busy_us_max;
} P;
static inline void put_bits(uint32_t v, int n) { P.bits |= (uint64_t)v << P.nbits; P.nbits += n; while (P.nbits >= 8) { P.out[P.out_len++] = (uint8_t)P.bits; P.bits >>= 8; P.nbits -= 8; } }
/* a Huffman code goes most significant bit first */
static inline void put_code(uint32_t code, int n) { uint32_t r = 0; for (int i = 0; i < n; i++) r |= ((code >> i) & 1) << (n - 1 - i); put_bits(r, n); }
static inline void put_lit(uint32_t lit)
{
    if (lit < 144) put_code(0x30 + lit, 8);
    else if (lit < 256) put_code(0x190 + lit - 144, 9);
    else if (lit < 280) put_code(lit - 256, 7);
    else put_code(0xc0 + lit - 280, 8);
}
static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static void put_match(uint32_t len, uint32_t dist)
{
    int i = 28; while (len_base[i] > len) i--;
    put_lit(257 + (uint32_t)i); if (len_extra[i]) put_bits(len - len_base[i], len_extra[i]);
    int d = 29; while (dist_base[d] > dist) d--;
    put_code((uint32_t)d, 5); if (dist_extra[d]) put_bits(dist - dist_base[d], dist_extra[d]);
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
/* a chunk around the bytes from start in P.out: its length before them, its CRC after */
static void chunk_close(uint32_t start)
{
    uint32_t n = P.out_len - start - 8;
    be32(P.out + start, n);
    be32(P.out + P.out_len, crc_update(0xffffffffu, P.out + start + 4, n + 4) ^ 0xffffffffu);
    P.out_len += 4;
}
static uint32_t chunk_open(const char *type) { uint32_t s = P.out_len; P.out_len += 4; for (int i = 0; i < 4; i++) P.out[P.out_len++] = (uint8_t)type[i]; return s; }

static int png_begin(int conn, int scale, int display, int hdmi)
{
    uint32_t w, h, d;
    if (P.active) return -1;
    if (!psmac_display_shot(&w, &h, &d, display, hdmi)) return 0;
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    if (w > PNG_MAXW) w = PNG_MAXW;
    P.active = 1; P.conn = conn; P.sw = w; P.sh = h; P.scale = (uint32_t)scale;
    P.w = w / P.scale; P.h = h / P.scale; P.y = 0; P.src_y = 0;
    if (!P.w) P.w = 1;
    if (!P.h) P.h = 1;
    P.adler_a = 1; P.adler_b = 0; P.bits = 0; P.nbits = 0; P.out_len = P.out_pos = 0; P.bytes = 0; P.t0 = now_ms();
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    for (int i = 0; i < 8; i++) P.out[P.out_len++] = sig[i];
    uint32_t s = chunk_open("IHDR");
    be32(P.out + P.out_len, P.w); be32(P.out + P.out_len + 4, P.h); P.out_len += 8;
    P.out[P.out_len++] = 8; P.out[P.out_len++] = 2; P.out[P.out_len++] = 0; P.out[P.out_len++] = 0; P.out[P.out_len++] = 0;
    chunk_close(s);
    s = chunk_open("IDAT");
    P.out[P.out_len++] = 0x78; P.out[P.out_len++] = 0x01;          /* zlib: deflate, 32 KiB window, no dictionary */
    put_bits(1, 1); put_bits(1, 2);                                /* the one block: final, fixed codes */
    chunk_close(s);
    return 1;
}
/* the next output row into P.rows[y % 3], box-averaged over scale x scale source pixels */
static void png_source_row(uint8_t *dst)
{
    uint32_t sc = P.scale, n = P.w * 3;
    dst[0] = 0;                                                    /* filter: none (the LZ77 search does the work) */
    if (sc == 1) { psmac_display_shot_row(P.src_y++, dst + 1, PNG_MAXW); return; }
    for (uint32_t i = 0; i < n; i++) P.acc[i] = 0;
    for (uint32_t r = 0; r < sc; r++) {
        uint32_t sy = P.src_y < P.sh ? P.src_y : P.sh - 1;
        P.src_y++;
        psmac_display_shot_row(sy, P.srow, PNG_MAXW);
        for (uint32_t x = 0; x < P.w; x++)
            for (uint32_t k = 0; k < sc; k++) {
                const uint8_t *p = P.srow + 3 * (x * sc + k);
                P.acc[3 * x] += p[0]; P.acc[3 * x + 1] += p[1]; P.acc[3 * x + 2] += p[2];
            }
    }
    for (uint32_t i = 0; i < n; i++) dst[1 + i] = (uint8_t)(P.acc[i] / (sc * sc));
}
/* one row: literals and matches against one or two pixels back and one or two rows up */
static void png_encode_row(void)
{
    uint32_t stride = 1 + 3 * P.w, y = P.y;
    uint8_t *c = P.rows[y % 3], *up1 = P.rows[(y + 2) % 3], *up2 = P.rows[(y + 1) % 3];
    png_source_row(c);
    for (uint32_t i = 0; i < stride; i++) { P.adler_a += c[i]; if (P.adler_a >= 65521) P.adler_a -= 65521; P.adler_b += P.adler_a; if (P.adler_b >= 65521) P.adler_b -= 65521; }
    for (uint32_t j = 0; j < stride; ) {
        uint32_t best = 0, bdist = 0, lim = stride - j > 258 ? 258 : stride - j;
        const uint8_t *cand[4] = { j >= 3 ? c + j - 3 : 0, j >= 6 ? c + j - 6 : 0, y >= 1 ? up1 + j : 0, y >= 2 && 2 * stride <= 32768 ? up2 + j : 0 };
        const uint32_t dist[4] = { 3, 6, stride, 2 * stride };
        for (int k = 0; k < 4; k++) {
            if (!cand[k] || (k >= 2 && dist[k] > 32768)) continue;
            uint32_t n = 0;
            while (n < lim && cand[k][n] == c[j + n]) n++;
            if (n > best) { best = n; bdist = dist[k]; }
        }
        if (best >= 3) { put_match(best, bdist); j += best; }
        else put_lit(c[j++]);
    }
    P.y++;
}
/* the next piece of the PNG: a band of rows as an IDAT chunk, or the end */
static void png_band(void)
{
    uint64_t t0 = now_ticks();
    P.out_len = P.out_pos = 0;
    if (P.y >= P.h) {                                              /* the end: the block's end code, the Adler-32, IEND */
        uint32_t s = chunk_open("IDAT");
        put_lit(256);
        if (P.nbits) put_bits(0, 8 - P.nbits);
        be32(P.out + P.out_len, P.adler_b << 16 | P.adler_a); P.out_len += 4;
        chunk_close(s);
        s = chunk_open("IEND"); chunk_close(s);
        P.active = 2;                                              /* done once this is sent */
        return;
    }
    uint32_t s = chunk_open("IDAT");
    uint32_t raw = 0, stride = 1 + 3 * P.w;
    do { png_encode_row(); raw += stride; } while (P.y < P.h && raw + stride <= 8192);
    chunk_close(s);
    uint32_t us = (uint32_t)((now_ticks() - t0) * 1000000u / tick_hz());
    if (us > P.busy_us_max) P.busy_us_max = us;
}

/* ======================================================================================= HTTP */
#define NCONN (IPK_CONNS + 1)                     /* the last one is the semihosting channel */
#define REQ_MAX 8192
#define OUT_MAX 12288
enum { R_NONE, R_BUF, R_PNG, R_LOG, R_RECORDS, R_INPUT_WAIT };
struct hconn {
    int used, local; uint8_t peer[4];
    char req[REQ_MAX + 1]; uint32_t req_len; int handled;
    int kind; char out[OUT_MAX]; uint32_t out_len, out_pos;
    uint32_t log_pos, log_end; char log_tags[64];             /* R_LOG */
    uint32_t wait_until, wait_head;                           /* R_INPUT_WAIT */
    int done;
};
static struct hconn hc[NCONN];
static uint32_t requests, refused;

static __attribute__((noinline)) void out_add(struct hconn *h, const char *s, unsigned n) { if (h->out_len + n > OUT_MAX) n = OUT_MAX - h->out_len; __builtin_memcpy(h->out + h->out_len, s, n); h->out_len += n; }
static void out_str(struct hconn *h, const char *s) { out_add(h, s, slen(s)); }
/* support.c has no vsnprintf for us: a few formatted pieces with sprintf into a buffer instead */
#define OUTF(h, ...) do { char b_[512]; sprintf(b_, __VA_ARGS__); out_str((h), b_); } while (0)
static void head(struct hconn *h, int code, const char *text, const char *ctype, int length)
{
    OUTF(h, "HTTP/1.0 %d %s\r\nServer: Aurora68\r\nConnection: close\r\nCache-Control: no-store\r\n", code, text);
    if (ctype) OUTF(h, "Content-Type: %s\r\n", ctype);
    if (length >= 0) OUTF(h, "Content-Length: %d\r\n", length);
    out_str(h, "\r\n");
}
static void respond(struct hconn *h, int code, const char *text, const char *ctype, const char *body)
{
    h->kind = R_BUF; h->out_len = h->out_pos = 0;
    head(h, code, text, ctype, (int)slen(body));
    out_str(h, body);
}
static void respond_json_error(struct hconn *h, int code, const char *text, const char *msg)
{
    char b[300]; sprintf(b, "{\"error\": \"%s\"}\n", msg);
    respond(h, code, text, "application/json", b);
}

/* ---- the page for a person ---- */
static const char page[] =
"<!DOCTYPE html><html><head><meta charset=utf-8><title>Aurora68</title><style>"
"body{font:14px -apple-system,Helvetica,sans-serif;margin:12px;background:#ddd}#s{image-rendering:pixelated;border:1px solid #444;cursor:crosshair;max-width:100%}"
"input,button,select{font:inherit}#st{font:12px monospace;white-space:pre}</style></head><body>"
"<div>Token <input id=tk type=password size=16> Scale <select id=sc><option>1<option>2<option>4</select> Screen <select id=dp><option>0<option>1</select> "
"<button onclick=k('return')>Return</button> <button onclick=k('escape')>Esc</button> <button onclick=\"k('q','cmd')\">Cmd-Q</button> "
"<input id=tx size=24 placeholder='text to type'><button onclick=ty()>Type</button> "
"<button onclick=\"if(confirm('Restart the Mac?'))cmd('/restart')\">Restart</button></div>"
"<p><img id=s tabindex=0 alt='the Mac'></p><div id=st></div><script>"
"var T=document.getElementById('tk'),S=document.getElementById('s'),sc=document.getElementById('sc'),dp=document.getElementById('dp');"
"T.value=localStorage.a68t||'';T.onchange=function(){localStorage.a68t=T.value};"
"function H(){return{'X-Aurora-Token':T.value}}"
"function send(ev){return fetch('/input',{method:'POST',headers:H(),body:JSON.stringify(ev)})}"
"function cmd(p){fetch(p,{method:'POST',headers:H()})}"
"function k(n,m){send({type:'key',key:n,mods:m||''})}"
"function ty(){var t=document.getElementById('tx');send({type:'text',text:t.value});t.value=''}"
"function xy(e){var r=S.getBoundingClientRect(),f=S.naturalWidth/r.width*(+sc.value);"
"return[Math.round((e.clientX-r.left)*f),Math.round((e.clientY-r.top)*f)]}"
"S.onclick=function(e){var p=xy(e);send({type:'click',display:+dp.value,x:p[0],y:p[1],count:e.detail>1?2:1,mods:e.shiftKey?'shift':''})};"
"S.onkeydown=function(e){var n={Enter:'return',Backspace:'delete',Tab:'tab',Escape:'escape',ArrowLeft:'left',ArrowRight:'right',ArrowUp:'up',ArrowDown:'down'}[e.key];"
"var m=(e.metaKey?'cmd,':'')+(e.altKey?'opt,':'')+(e.ctrlKey?'ctrl':'');"
"if(n)send({type:'key',key:n,mods:m});else if(e.key.length==1){if(m)send({type:'key',key:e.key.toLowerCase(),mods:m});else send({type:'text',text:e.key})}else return;e.preventDefault()};"
"function shot(){fetch('/screen.png?scale='+sc.value+'&display='+dp.value,{headers:H()}).then(function(r){if(!r.ok)throw r.status;return r.blob()})"
".then(function(b){var u=URL.createObjectURL(b);S.onload=function(){URL.revokeObjectURL(u);setTimeout(shot,300)};S.src=u})"
".catch(function(e){document.getElementById('st').textContent='screen: '+e;setTimeout(shot,2000)})}"
"function state(){fetch('/state',{headers:H()}).then(function(r){return r.text()}).then(function(t){document.getElementById('st').textContent=t})"
".catch(function(){}).then(function(){setTimeout(state,2000)})}shot();state();</script></body></html>\n";

/* ---- the log: the window's ring (psmb.h, struct psmb_log), secrets hidden ---- */
/* One line into out: the values of secrets as ***, by the rule the card's aurora68.log and the
   kernel's own lines use (psmac_redact, psmac_card.c: wifi.psk, anything named psk, pass, key, secret
   or token, mac.wifisim's password, bt.dev's key), and then the token itself wherever it still
   appears, should some line have printed it bare. */
#define RED_MAX 1600                        /* a line of 512 at most comes out no longer than this */
static unsigned redact_line(const char *in, unsigned n, char *out)
{
    char line[513], red[RED_MAX];
    if (n > 512) n = 512;
    for (unsigned i = 0; i < n; i++) line[i] = in[i] ? in[i] : ' ';
    line[n] = 0;
    psmac_redact(line, red, sizeof red);
    unsigned o = 0;
    for (unsigned i = 0; red[i] && o + 4 < RED_MAX; i++) {
        if (token_len && red[i] == token[0] && !__builtin_strncmp(red + i, token, token_len)) {
            out[o++] = '*'; out[o++] = '*'; out[o++] = '*'; i += token_len - 1; continue;
        }
        out[o++] = red[i];
    }
    return o;
}
static uint32_t log_head(void) { return *(volatile uint32_t *)(win + PSMB_OFF_LOG + 16); }
static uint8_t log_byte(uint32_t i) { return win[PSMB_OFF_LOGDATA + i % PSMB_LOG_SIZE]; }
static int line_has_tag(const char *l, unsigned n, const char *tags)
{
    if (!tags[0]) return 1;
    if (n < 3 || l[0] != '[') return 0;
    for (const char *t = tags; *t; ) {
        const char *e = t; while (*e && *e != ',') e++;
        unsigned k = (unsigned)(e - t);
        if (k && n > k + 1 && ieq_n(l + 1, t, k) && (l[1 + k] == ']' || l[1 + k] == ':')) return 1;
        t = *e ? e + 1 : e;
    }
    return 0;
}
/* the next lines of a log answer into h->out */
static void log_fill(struct hconn *h)
{
    h->out_len = h->out_pos = 0;
    char line[512], red[RED_MAX];
    while (h->log_pos != h->log_end && h->out_len < 4096) {         /* a few KB a turn: the redaction is byte by byte */
        unsigned n = 0;
        while (h->log_pos != h->log_end && n < sizeof line - 1) { char c = (char)log_byte(h->log_pos++); line[n++] = c; if (c == '\n') break; }
        if (line_has_tag(line, n, h->log_tags)) out_add(h, red, redact_line(line, n, red));
    }
    if (h->log_pos == h->log_end) h->done = 1;
}

/* ---- the records: the [BOOT] and [CRASH] lines, kept after the ring has moved on ----
   The ring holds 64 KiB, an hour or so of a quiet Mac and minutes of a busy one, so the boot
   timeline and an old crash record are soon gone from it. The lines are copied here as they pass:
   the boot timeline's ("[BOOT] t=", psmac_boot.c; 4 KiB, the first ones: Emu68's own [BOOT] lines
   about memory pools and cores are left out, they filled it in the first test) and the crash
   records' (12 KiB, the newest). */
static char rec_boot[4096], rec_crash[12288], rec_line[512];
static uint32_t rec_boot_len, rec_crash_len, rec_line_len, rec_pos;
static void records_scan(void)
{
    uint32_t hd = log_head(), budget = 4096;
    if ((int32_t)(hd - rec_pos) > (int32_t)PSMB_LOG_SIZE) rec_pos = hd - PSMB_LOG_SIZE;   /* fell behind: from the oldest there is */
    while (rec_pos != hd && budget--) {
        char c = (char)log_byte(rec_pos++);
        if (rec_line_len < sizeof rec_line) rec_line[rec_line_len++] = c;
        if (c != '\n') continue;
        uint32_t n = rec_line_len; rec_line_len = 0;
        if (n > 9 && starts(rec_line, "[BOOT] t=") && rec_boot_len + n <= sizeof rec_boot) { __builtin_memcpy(rec_boot + rec_boot_len, rec_line, n); rec_boot_len += n; }
        else if (n > 7 && starts(rec_line, "[CRASH]")) {
            if (n > sizeof rec_crash) continue;
            while (rec_crash_len + n > sizeof rec_crash) {          /* the oldest lines make room */
                uint32_t k = 0; while (k < rec_crash_len && rec_crash[k] != '\n') k++;
                k = k < rec_crash_len ? k + 1 : rec_crash_len;
                __builtin_memmove(rec_crash, rec_crash + k, rec_crash_len - k); rec_crash_len -= k;
            }
            __builtin_memcpy(rec_crash + rec_crash_len, rec_line, n); rec_crash_len += n;
        }
    }
}
/* the next records into h->out: the boot's, then the crashes', redacted; log_pos counts through both */
static void records_fill(struct hconn *h)
{
    h->out_len = h->out_pos = 0;
    char red[RED_MAX];
    while (h->out_len < 4096 && h->log_pos < rec_boot_len + rec_crash_len) {
        const char *src = h->log_pos < rec_boot_len ? rec_boot + h->log_pos : rec_crash + (h->log_pos - rec_boot_len);
        uint32_t left = h->log_pos < rec_boot_len ? rec_boot_len - h->log_pos : rec_boot_len + rec_crash_len - h->log_pos, n = 0;
        while (n < left && n < 500 && src[n] != '\n') n++;
        if (n < left) n++;
        out_add(h, red, redact_line(src, n, red));
        h->log_pos += n;
    }
    if (h->log_pos >= rec_boot_len + rec_crash_len) h->done = 1;
}

/* ---- state ---- */
static void ip_str(char *b, const uint8_t *ip) { sprintf(b, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]); }
static void hw_str(char *b, const volatile uint8_t *m) { sprintf(b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]); }
static void json_name(char *out, const char *in, unsigned max)                     /* a string safe inside "" */
{
    unsigned o = 0;
    for (; *in && o + 2 < max; in++) { char c = *in; if (c == '"' || c == '\\') out[o++] = '\\'; out[o++] = (c >= 32 && c < 127) ? c : '?'; }
    out[o] = 0;
}
static uint32_t ticks_seen, ticks_changed_ms;
static void mac_sample(void)                                     /* once a second: do Ticks move? */
{
    static uint32_t next;
    uint32_t t = now_ms();
    if ((int32_t)(t - next) < 0) return;
    next = t + 1000;
    int ok; uint32_t v = psmac_mac_rd32(0x16a, &ok);             /* Ticks */
    if (ok && v != ticks_seen) { ticks_seen = v; ticks_changed_ms = t; }
}
static void cur_app(char *out)
{
    int ok; uint32_t w[8];
    for (int i = 0; i < 8; i++) w[i] = psmac_mac_rd32(0x910 + 4 * (uint32_t)i, &ok);
    unsigned len = w[0] >> 24;
    char raw[33];
    if (len > 31) len = 0;
    for (unsigned i = 0; i < len; i++) raw[i] = (char)(w[(i + 1) / 4] >> (24 - 8 * ((i + 1) % 4)));
    raw[len] = 0;
    json_name(out, raw, 64);
}
/* ---- the screens on the desktop ----
   Screen i of the card (psmac_display.c: 0 the first, the SE's on the SE board; 1 the HDMI screen)
   as QuickDraw has it: its GDevice, found in DeviceList ($8A8) by its pixel map's base address, which
   the driver gave QuickDraw from the same table the kernel filled. gdRect is where Monitors (or the
   'scrn' resource) put it on the desktop; gdFlags bit 15 is screenActive, bit 11 mainScreen. The
   GDevice: gdFlags at 20, gdPMap at 22, gdNextGD at 30, gdRect at 34; a PixMap's baseAddr at 0.
   Handles carry flag bits in their top byte in 24-bit mode (MMU32Bit, $CB2). */
static uint32_t mac_long(uint32_t a, int *ok) { return psmac_mac_rd32(a, ok); }
static int screen_place(int i, int16_t rect[4], uint32_t *flags)
{
    uint32_t w, h, d, base;
    if (!psmac_display_info(i, &w, &h, &d, &base)) return 0;
    int ok, ok2;
    uint32_t mask = (mac_long(0xcb0, &ok) >> 8 & 0xff) ? 0xffffffffu : 0x00ffffffu;
    uint32_t gdh = mac_long(0x8a8, &ok) & mask;
    for (int n = 0; ok && gdh && n < 8; n++) {
        uint32_t gd = mac_long(gdh, &ok) & mask;
        if (!ok || !gd) return 0;
        uint32_t pm = mac_long(mac_long(gd + 22, &ok) & mask, &ok2) & mask;
        uint32_t ba = ok && ok2 && pm ? mac_long(pm, &ok2) : 0;
        if (ok2 && ba && (ba == base || (ba & 0xffffffu) == (base & 0xffffffu))) {
            uint32_t a = mac_long(gd + 34, &ok), b = mac_long(gd + 38, &ok2);
            rect[0] = (int16_t)(a >> 16); rect[1] = (int16_t)a; rect[2] = (int16_t)(b >> 16); rect[3] = (int16_t)b;
            *flags = mac_long(gd + 20, &ok2) >> 16;
            return ok && ok2;
        }
        gdh = mac_long(gd + 30, &ok) & mask;
    }
    return 0;
}
/* "screens": each screen of the card, its size and depth, and where the Mac has it */
static void screens_json(struct hconn *h)
{
    out_str(h, "  \"screens\": [");
    for (int i = 0; i < psmac_display_count(); i++) {
        uint32_t w, hh, d, base, fl = 0; int16_t r[4];
        const char *what = psmac_board_se && !psmac_board_solo ? (i ? "hdmi" : "se") : "card";
        if (!psmac_display_info(i, &w, &hh, &d, &base)) { OUTF(h, "%s\n    {\"display\": %d, \"what\": \"%s\", \"open\": false}", i ? "," : "", i, what); continue; }
        OUTF(h, "%s\n    {\"display\": %d, \"what\": \"%s\", \"open\": true, \"width\": %u, \"height\": %u, \"depth\": %u, \"base\": \"0x%08x\"",
             i ? "," : "", i, what, w, hh, d, base);
        if (screen_place(i, r, &fl))
            OUTF(h, ", \"left\": %d, \"top\": %d, \"right\": %d, \"bottom\": %d, \"active\": %s, \"main\": %s}",
                 r[1], r[0], r[3], r[2], fl & 0x8000 ? "true" : "false", fl & 0x0800 ? "true" : "false");
        else out_str(h, ", \"left\": null}");
    }
    out_str(h, "\n  ],\n");
}
/* the firmware's GET_THROTTLED bits (psmac_power.c): each "now" bit and each "since boot" bit by name */
static void power_json(struct hconn *h)
{
    extern int64_t psmac_power_bits(void);
    static const char *const names[4] = { "under_voltage", "frequency_capped", "throttled", "soft_temp_limit" };
    int64_t pw = psmac_power_bits();
    if (pw < 0) { out_str(h, "  \"power\": null,\n"); return; }          /* no reading yet, or a firmware (QEMU) that has none */
    uint32_t v = (uint32_t)pw;
    OUTF(h, "  \"power\": {\"throttled\": \"0x%x\"", v);
    for (int i = 0; i < 4; i++) OUTF(h, ", \"%s%s\": %s", names[i], i == 2 ? "_now" : "", v & (1u << i) ? "true" : "false");
    for (int i = 0; i < 4; i++) OUTF(h, ", \"%s_since_boot\": %s", names[i], v & (0x10000u << i) ? "true" : "false");
    out_str(h, "},\n");
}
static void state_json(struct hconn *h)
{
    char a[64], b[64], c[64];
    uint32_t t = now_ms();
    h->kind = R_BUF; h->out_len = h->out_pos = 0;
    head(h, 200, "OK", "application/json", -1);
    uint32_t w = 0, hh = 0, d = 0;
    int src = psmac_display_shot(&w, &hh, &d, 0, 0);
    OUTF(h, "{\n  \"board\": \"%s\",\n  \"uptime_s\": %u,\n", psmac_board_solo ? "solo" : psmac_board_se ? "se" : "iici", t / 1000);
    OUTF(h, "  \"screen\": {\"width\": %u, \"height\": %u, \"depth\": %u, \"source\": \"%s\"},\n", w, hh, d, src == 1 ? "mac" : src == 2 ? "console" : "none");
    int ok; uint32_t ticks = psmac_mac_rd32(0x16a, &ok);
    int16_t dserr = (int16_t)(psmac_mac_rd32(0xaf0, &ok) >> 16);
    uint32_t exc_n, vec, pc, sys_n; int16_t sid; const char *ew, *sw;
    psmac_crash_last(&exc_n, &vec, &pc, &ew, &sys_n, &sid, &sw);
    cur_app(a);
    int still = ticks_changed_ms && t - ticks_changed_ms > 3000;
    int bombed = (sys_n != reset_sys_n && sid > 0 && sid != 40 && sid < 20000) ||
                 (exc_n != reset_exc_n && dserr > 0 && dserr != 40 && dserr < 20000);
    const char *st = still ? "stopped" : bombed ? "system error" : !a[0] ? "starting" : "running";
    OUTF(h, "  \"mac\": {\"state\": \"%s\", \"application\": \"%s\", \"ticks\": %u, \"ticks_moving\": %s, \"input_attached\": %s, \"resets\": %u, \"seconds_since_reset\": %u,\n",
         st, a, ticks, still ? "false" : "true", psmac_input_attached() ? "true" : "false", mac_resets, mac_resets ? (t - reset_ms) / 1000 : t / 1000);
    OUTF(h, "    \"ds_err_code\": %d, \"ds_err_name\": \"%s\",\n", dserr, psmac_crash_syserr_name(dserr));
    if (sys_n != reset_sys_n && sid > 0 && sid != 40 && sid < 20000) OUTF(h, "    \"system_error\": {\"id\": %d, \"name\": \"%s\"},\n", sid, sw);
    else out_str(h, "    \"system_error\": null,\n");
    if (exc_n != reset_exc_n) OUTF(h, "    \"exception\": {\"what\": \"%s\", \"vector\": \"0x%03x\", \"pc\": \"0x%08x\", \"count\": %u}},\n", ew, vec, pc, exc_n - reset_exc_n);
    else out_str(h, "    \"exception\": null},\n");
    /* the network */
    struct ipk_info in; psmac_ip_info(&in);
    ip_str(a, in.ip); hw_str(b, in.hw);
    OUTF(h, "  \"remote\": {\"link\": \"%s\", \"phase\": \"%s\", \"ip\": \"%s\", \"mac\": \"%s\", \"name\": \"%s.local\", \"shares_mac\": %s, \"shares_ip\": %s, \"requests\": %u, \"refused\": %u, \"input_events\": %u, \"png_band_us_max\": %u},\n",
         in.unit == 1 ? "wired" : in.unit == 0 ? "wifi" : "none", in.phase, a, b, psmac_ip_name(), in.shared ? "true" : "false", in.shared == 2 ? "true" : "false",
         requests, refused, input_events, P.busy_us_max);
    screens_json(h);
    power_json(h);
    out_str(h, "  \"network\": [");
    volatile uint8_t *nb = alice_net_board();
    for (int u = 0; u < 2 && nb; u++) {
        volatile struct an_unit *x = (volatile struct an_unit *)(nb + AN_OFF_UNIT(u));
        hw_str(b, x->mac); ip_str(c, in.mac_ip[u]);
        OUTF(h, "%s\n    {\"unit\": %d, \"kind\": \"%s\", \"link\": %s, \"state\": %u, \"mac\": \"%s\", \"mac_ip\": \"%s\", \"mac_rx_on\": %s, \"rx\": %u, \"rx_dropped\": %u, \"tx\": %u}",
             u ? "," : "", u, x->kind == AN_KIND_WIFI ? "wifi" : x->kind == AN_KIND_ETHER ? "wired" : "none", x->link ? "true" : "false", x->state, b, c,
             x->rx_on ? "true" : "false", x->rx_pkts, x->rx_drop, x->tx_pkts);
    }
    out_str(h, "\n  ],\n  \"disks\": [");
    int n = psmac_image_count();
    for (int i = 0; i < n; i++) {
        const char *nm = "?"; uint8_t *data; uint32_t blocks = 0;
        psmac_image_get(i, &nm, &data, &blocks);
        json_name(a, nm, sizeof a);
        OUTF(h, "%s\n    {\"name\": \"%s\", \"blocks\": %u, \"as\": \"%s\"}", i ? "," : "", a, blocks, psmac_image_is_scsi(i) ? "SCSI" : "PiDisk");
    }
    OUTF(h, "\n  ],\n  \"log_head\": %u\n}\n", log_head());
}

/* ---- control ---- */
#define PM_RSTC (PSMAC_PERIPH_HI + 0x10001cu)
#define PM_WDOG (PSMAC_PERIPH_HI + 0x100024u)
static uint32_t pi_reset_at;
static void pi_reset_poll(void)
{
    if (!pi_reset_at || (int32_t)(now_ms() - pi_reset_at) < 0) return;
    wr32le(PM_WDOG, 0x5a000000u | 10u);                         /* ten of the watchdog's 16 us ticks */
    wr32le(PM_RSTC, 0x5a000020u);                               /* a full reset when it runs out */
    pi_reset_at = 0;
}
uint32_t psmac_restart_pc(void)                               /* ExecutionLoop.c: the ROM's reset entry, its second long */
{
    int ok; uint32_t pc = psmac_mac_rd32(PSMAC_ROM_BASE + 4, &ok);
    return ok && pc >= PSMAC_ROM_BASE && pc < PSMAC_ROM_BASE + PSMAC_ROM_SIZE ? pc : PSMAC_ROM_BASE + 0x2a;
}
static void control(struct hconn *h, const char *how)
{
    if (!__m68k_state) { respond_json_error(h, 503, "Service Unavailable", "the 68k has not started"); return; }
    if (!how || !how[0] || starts(how, "mac")) {
        kprintf("[REMOTE] restart of the Mac asked for: the 68k goes to the ROM's reset entry %08x\n", psmac_restart_pc());
        iq_tail = iq_head; buttons = 0; mods_now = 0;
        __atomic_store_n(&__m68k_state->INTF.RESET, 1, __ATOMIC_RELEASE);
        respond(h, 200, "OK", "application/json", "{\"restart\": \"mac\"}\n");
    } else if (starts(how, "nmi")) {
        kprintf("[REMOTE] NMI (the programmer's switch) asked for\n");
        __atomic_store_n(&__m68k_state->INTF.ARM_err, 1, __ATOMIC_RELEASE);
        respond(h, 200, "OK", "application/json", "{\"restart\": \"nmi\"}\n");
    } else if (starts(how, "pi")) {
        kprintf("[REMOTE] the whole Pi restarts (watchdog) in a second and a half, once this answer is out\n");
        respond(h, 200, "OK", "application/json", "{\"restart\": \"pi\"}\n");
        pi_reset_at = now_ms() + 1500;                          /* the poll pulls the watchdog: QEMU's resets at once */
    } else respond_json_error(h, 400, "Bad Request", "how is mac, nmi or pi");
}

/* ---- the request ---- */
static const char *query(const char *q, const char *key, char *out, unsigned max)
{
    unsigned kl = slen(key);
    for (const char *s = q; s && *s; ) {
        if (starts(s, key) && s[kl] == '=') {
            unsigned n = 0;
            for (s += kl + 1; *s && *s != '&' && n + 1 < max; s++) out[n++] = *s == '+' ? ' ' : *s;
            out[n] = 0;
            return out;
        }
        while (*s && *s != '&') s++;
        if (*s) s++;
    }
    return 0;
}
static int private_source(const uint8_t *ip)
{
    return ip[0] == 10 || (ip[0] == 172 && (ip[1] & 0xf0) == 16) || (ip[0] == 192 && ip[1] == 168) || (ip[0] == 169 && ip[1] == 254) || ip[0] == 127;
}
/* the token, compared in constant time */
static int token_ok(const char *got, unsigned n)
{
    unsigned diff = n ^ token_len;
    for (unsigned i = 0; i < token_len; i++) diff |= (unsigned)(uint8_t)(i < n ? got[i] : 0) ^ (uint8_t)token[i];
    return token_len && !diff;
}
static void handle(int c)
{
    struct hconn *h = &hc[c];
    char *r = h->req;
    char *eoh = 0;
    for (uint32_t i = 0; i + 3 < h->req_len; i++) if (r[i] == '\r' && r[i + 1] == '\n' && r[i + 2] == '\r' && r[i + 3] == '\n') { eoh = r + i + 4; break; }
    if (!eoh) { if (h->req_len >= REQ_MAX) { h->handled = 1; respond_json_error(h, 413, "Request Entity Too Large", "the request is too long"); } return; }
    /* the headers we read: Content-Length and the token */
    uint32_t clen = 0; const char *tok = 0; unsigned tok_n = 0;
    for (char *l = r; l < eoh; ) {
        char *e = l; while (e < eoh && *e != '\r') e++;
        unsigned n = (unsigned)(e - l);
        if (n > 15 && ieq_n(l, "content-length:", 15)) { const char *p = l + 15; while (*p == ' ') p++; while (*p >= '0' && *p <= '9') clen = clen * 10 + (uint32_t)(*p++ - '0'); }
        else if (n > 15 && ieq_n(l, "x-aurora-token:", 15)) { tok = l + 15; while (*tok == ' ') tok++; tok_n = (unsigned)(e - tok); }
        else if (n > 21 && ieq_n(l, "authorization: bearer", 21)) { tok = l + 21; while (*tok == ' ') tok++; tok_n = (unsigned)(e - tok); }
        l = e + 2;
    }
    if ((uint32_t)(eoh - r) + clen > REQ_MAX) { h->handled = 1; respond_json_error(h, 413, "Request Entity Too Large", "the request is too long"); return; }
    if ((uint32_t)(eoh - r) + clen > h->req_len) return;          /* the body is still coming */
    h->handled = 1; requests++;
    char *body = eoh; body[clen] = 0;
    /* the request line: METHOD PATH?QUERY HTTP/x */
    char *sp = r; while (*sp && *sp != ' ') sp++;
    if (!*sp) { respond_json_error(h, 400, "Bad Request", "no request line"); return; }
    *sp = 0;
    char *path = sp + 1, *pe = path; while (*pe && *pe != ' ' && *pe != '\r') pe++;
    *pe = 0;
    char *q = path; while (*q && *q != '?') q++;
    if (*q) *q++ = 0;
    const char *method = r;
    int get = !__builtin_strcmp(method, "GET"), post = !__builtin_strcmp(method, "POST");
    if (!h->local && !any_source && !private_source(h->peer)) {
        refused++;
        respond_json_error(h, 403, "Forbidden", "only the local network (remote.any=1 allows every address)");
        return;
    }
    if (get && (!__builtin_strcmp(path, "/") || !__builtin_strcmp(path, "/index.html"))) { respond(h, 200, "OK", "text/html; charset=utf-8", page); return; }
    if (!h->local && !token_ok(tok, tok_n)) { refused++; respond_json_error(h, 401, "Unauthorized", "the token is missing or wrong (X-Aurora-Token)"); return; }
    char v[64];
    if (get && !__builtin_strcmp(path, "/screen.png")) {
        int scale = query(q, "scale", v, sizeof v) ? v[0] - '0' : 1;
        int hdmi = query(q, "src", v, sizeof v) && v[0] == 'h';
        int display = query(q, "display", v, sizeof v) ? v[0] - '0' : 0;
        if (display < 0 || display >= psmac_display_count()) { respond_json_error(h, 404, "Not Found", "no such display (0 is the first screen, 1 the second)"); return; }
        int k = png_begin(c, scale, display, hdmi);
        if (k < 0) { respond_json_error(h, 503, "Service Unavailable", "another screenshot is being sent: try again"); return; }
        if (!k) { respond_json_error(h, 503, "Service Unavailable", "no screen yet"); return; }
        h->kind = R_PNG; h->out_len = h->out_pos = 0;
        head(h, 200, "OK", "image/png", -1);
        return;
    }
    if (get && !__builtin_strcmp(path, "/state")) { state_json(h); return; }
    if (get && !__builtin_strcmp(path, "/log") && query(q, "records", v, sizeof v) && v[0] == '1') {
        h->kind = R_RECORDS; h->log_pos = 0; h->out_len = h->out_pos = 0;
        OUTF(h, "HTTP/1.0 200 OK\r\nServer: Aurora68\r\nConnection: close\r\nContent-Type: text/plain; charset=iso-8859-1\r\n\r\n");
        return;
    }
    if (get && !__builtin_strcmp(path, "/log")) {
        uint32_t hd = log_head(), start = hd > PSMB_LOG_SIZE ? hd - PSMB_LOG_SIZE : 0, since = start;
        if (query(q, "since", v, sizeof v)) { since = 0; for (const char *p = v; *p >= '0' && *p <= '9'; p++) since = since * 10 + (uint32_t)(*p - '0'); }
        uint32_t lost = 0;
        if ((int32_t)(since - start) < 0) { lost = start - since; since = start; }
        if ((int32_t)(since - hd) > 0) since = hd;
        h->log_tags[0] = 0;
        query(q, "tags", h->log_tags, sizeof h->log_tags);
        h->kind = R_LOG; h->log_pos = since; h->log_end = hd; h->out_len = h->out_pos = 0;
        OUTF(h, "HTTP/1.0 200 OK\r\nServer: Aurora68\r\nConnection: close\r\nContent-Type: text/plain; charset=iso-8859-1\r\n"
                "X-Log-Start: %u\r\nX-Log-Next: %u\r\nX-Log-Lost: %u\r\n\r\n", since, hd, lost);
        return;
    }
    if (post && !__builtin_strcmp(path, "/input")) {
        if (!psmac_input_attached()) { respond_json_error(h, 503, "Service Unavailable", ".PiInput is not open on the Mac (yet)"); return; }
        static struct jfield f[JF];
        const char *p = body, *e = body + clen;
        p = jskip(p, e);
        if (p < e && *p == '{') {                                  /* {"events": [...]}: the array inside */
            for (const char *k = p; k + 8 < e; k++)
                if (starts(k, "\"events\"")) { k += 8; while (k < e && *k != '[' && *k != '{') k++; p = k; break; }
        }
        int arr = p < e && *p == '[', nev = 0; char why[128] = "";
        if (arr) p++;
        while (p && p < e) {
            p = jskip(p, e);
            if (p >= e || *p == ']') break;
            int nf;
            p = jobject(p, e, f, &nf);
            if (!p) { sprintf(why, "the JSON was not understood (event %d)", nev); break; }
            if (!input_event(f, nf, why, sizeof why)) break;
            nev++;
            if (!arr) break;
        }
        if (!p || (why[0] && !nev)) { respond_json_error(h, 400, "Bad Request", why[0] ? why : "no events"); return; }
        if (query(q, "wait", v, sizeof v) && v[0] == '1') {
            h->kind = R_INPUT_WAIT; h->wait_head = iq_head; h->wait_until = now_ms() + 25000;
            sprintf(h->out, "%d", nev); h->out_len = 0;
            return;
        }
        char b[200]; sprintf(b, "{\"events\": %d, \"queued\": %u%s%s%s}\n", nev, iq_head - iq_tail, why[0] ? ", \"note\": \"" : "", why, why[0] ? "\"" : "");
        respond(h, 200, "OK", "application/json", b);
        return;
    }
    if (post && !__builtin_strcmp(path, "/restart")) { control(h, query(q, "how", v, sizeof v)); return; }
    if (get || post) { respond_json_error(h, 404, "Not Found", "GET /, /screen.png, /state, /log; POST /input, /restart"); return; }
    respond_json_error(h, 405, "Method Not Allowed", "GET or POST");
}

/* the bytes of the answer, as room allows; *finished once it is all out */
static uint32_t produce(int c, uint8_t *buf, uint32_t room, int *finished)
{
    struct hconn *h = &hc[c];
    *finished = 0;
    if (!h->handled) return 0;
    if (h->kind == R_INPUT_WAIT) {
        if ((int32_t)(iq_tail - h->wait_head) < 0 && (int32_t)(now_ms() - h->wait_until) < 0) return 0;   /* still being played */
        char b[96]; uint32_t left = (int32_t)(iq_tail - h->wait_head) < 0 ? h->wait_head - iq_tail : 0;
        if (left) sprintf(b, "{\"events\": %s, \"done\": false, \"still_queued\": %u}\n", h->out, left);
        else sprintf(b, "{\"events\": %s, \"done\": true}\n", h->out);
        h->out_len = 0; respond(h, 200, "OK", "application/json", b);
    }
    uint32_t n = 0;
    for (;;) {
        if (h->out_pos < h->out_len) {
            uint32_t k = h->out_len - h->out_pos;
            if (k > room - n) k = room - n;
            __builtin_memcpy(buf + n, h->out + h->out_pos, k);
            h->out_pos += k; n += k;
            if (n == room) return n;
            continue;
        }
        if (h->kind == R_PNG && P.active && P.conn == c) {
            if (P.out_pos < P.out_len) {
                uint32_t k = P.out_len - P.out_pos;
                if (k > room - n) k = room - n;
                __builtin_memcpy(buf + n, P.out + P.out_pos, k);
                P.out_pos += k; n += k; P.bytes += k;
                if (n == room) return n;
                continue;
            }
            if (P.active == 2) {
                P.active = 0; *finished = 1;
                kprintf("[REMOTE] screenshot %ux%u: %u bytes in %u ms, longest band %u us\n", P.w, P.h, P.bytes, now_ms() - P.t0, P.busy_us_max);
                return n;
            }
            if (n) return n;                                       /* the next band on the next turn */
            png_band();
            continue;
        }
        if (h->kind == R_LOG && !h->done) { log_fill(h); continue; }
        if (h->kind == R_RECORDS && !h->done) { records_fill(h); continue; }
        *finished = 1;
        return n;
    }
}

/* ---- the TCP side (psmac_ip.c) ---- */
static void conn_reset(int c) { struct hconn *h = &hc[c]; if (P.active && P.conn == c) P.active = 0; h->used = 0; h->req_len = 0; h->handled = 0; h->kind = R_NONE; h->out_len = h->out_pos = 0; h->done = 0; }
int remote_tcp_accept(int c, uint16_t port, const uint8_t peer[4])
{
    (void)port;
    conn_reset(c);
    hc[c].used = 1; hc[c].local = 0;
    for (int i = 0; i < 4; i++) hc[c].peer[i] = peer[i];
    return 1;
}
void remote_tcp_recv(int c, const uint8_t *data, uint32_t len)
{
    struct hconn *h = &hc[c];
    if (h->handled) return;                                        /* one request per connection */
    if (len > REQ_MAX - h->req_len) len = REQ_MAX - h->req_len;
    __builtin_memcpy(h->req + h->req_len, data, len);
    h->req_len += len; h->req[h->req_len] = 0;
    handle(c);
}
uint32_t remote_tcp_produce(int c, uint8_t *buf, uint32_t room)
{
    int fin;
    uint32_t n = produce(c, buf, room, &fin);
    if (fin) ipk_tcp_close(c);
    return n;
}
void remote_tcp_closed(int c) { conn_reset(c); }

/* ---- the testbed's channel: mac.remotefile=BASE through semihosting ---- */
static int64_t sh_in = -1, sh_out = -1; static uint64_t sh_off;
static uint8_t sh_buf[16384];
static void semihost_poll(void)
{
    const int c = NCONN - 1;
    struct hconn *h = &hc[c];
    if (sh_in < 0) return;
    if (!h->used) {                                                /* the next request: {u32 length, the bytes} */
        static uint32_t next_look;                                 /* a semihosting call is a trap to QEMU: twenty a second is plenty */
        if ((int32_t)(now_ms() - next_look) < 0) return;
        next_look = now_ms() + 50;
        int64_t len = psmac_sh_flen(sh_in);
        if (len < 0 || (uint64_t)len < sh_off + 4) return;
        uint8_t l4[4];
        if (psmac_sh_seek(sh_in, sh_off) < 0 || !psmac_sh_read(sh_in, l4, 4)) return;
        uint32_t n = (uint32_t)l4[0] << 24 | (uint32_t)l4[1] << 16 | (uint32_t)l4[2] << 8 | l4[3];
        if (n > REQ_MAX || (uint64_t)len < sh_off + 4 + n) return;
        conn_reset(c);
        h->used = 1; h->local = 1;
        if (!psmac_sh_read(sh_in, h->req, n)) return;
        sh_off += 4 + n;
        h->req_len = n; h->req[n] = 0;
        handle(c);
        if (!h->handled) { respond_json_error(h, 400, "Bad Request", "an incomplete request"); h->handled = 1; }
        return;
    }
    /* the answer, a piece per turn, each piece {u32 length, bytes}; a length of 0 ends it */
    int fin;
    uint32_t n = produce(c, sh_buf + 4, sizeof sh_buf - 4, &fin);
    if (n) { sh_buf[0] = (uint8_t)(n >> 24); sh_buf[1] = (uint8_t)(n >> 16); sh_buf[2] = (uint8_t)(n >> 8); sh_buf[3] = (uint8_t)n; psmac_sh_write(sh_out, sh_buf, n + 4); }
    if (fin) { uint8_t z[4] = { 0 }; psmac_sh_write(sh_out, z, 4); conn_reset(c); }
}

/* ======================================================================================= the service */
void psmac_remote_init(uint8_t *window)
{
    win = window;
    crc_init();
    rec_pos = log_head() > PSMB_LOG_SIZE ? log_head() - PSMB_LOG_SIZE : 0;
    const char *o = psmac_option("mac.remotefile");
    if (o && psmac_sh_available()) {
        char a[140], b[140]; unsigned n = 0;
        for (; o[n] && n < 120; n++) { a[n] = b[n] = o[n]; }
        __builtin_memcpy(a + n, ".in", 4); __builtin_memcpy(b + n, ".out", 5);
        sh_in = psmac_sh_open(a, 1); sh_out = psmac_sh_open(b, 9);
        if (sh_in >= 0 && sh_out >= 0) { sh_off = (uint64_t)psmac_sh_flen(sh_in); kprintf("[REMOTE] testbed channel: requests from %s, answers to %s\n", a, b); }
        else { sh_in = sh_out = -1; kprintf("[REMOTE] testbed channel %s: cannot open its files\n", o); }
    }
    o = psmac_option("remote");
    if (!o || !(starts(o, "on") || o[0] == '1')) return;        /* off: nothing of it runs */
    o = psmac_option("remote.token");
    if (o) { unsigned n = 0; for (; o[n] && n < sizeof token - 1; n++) token[n] = o[n]; token[n] = 0; token_len = n; }
    if (token_len < 8) {
        kprintf("[REMOTE] remote=on, but remote.token= is missing or shorter than 8 characters: the remote control stays off\n");
        token_len = 0;
        return;
    }
    o = psmac_option("remote.any");
    any_source = o && o[0] == '1';
    enabled = 1;
    psmac_ip_init();
    kprintf("[REMOTE] on: HTTP on ports 80 and 8068 with the token from cmdline.txt, %s (docs/13)\n",
            any_source ? "from any address (remote.any=1)" : "from the local network only");
}

/* every turn of the service loop. With nothing to do it returns at once, looks at the clock every
   32nd turn, and does its rounds once a millisecond: the first version did them every turn, and
   under QEMU that was a fifth of core 3's time with nobody connected (the log line "remote N us
   (M us in all)"; QEMU emulates every read of the timer, which a Pi does in a cycle or two) */
void psmac_remote_poll(void)
{
    if (!enabled && sh_in < 0) return;
    static uint64_t next_round; static uint32_t turns;
    int busy = (enabled && psmac_ip_busy()) || iq_tail != iq_head || P.active || hc[NCONN - 1].used;
    if (!busy && (++turns & 31)) return;
    uint64_t t = now_ticks();
    if (!busy && (int64_t)(t - next_round) < 0) return;
    next_round = t + tick_hz() / 1000;
    if (sh_in >= 0) semihost_poll();
    records_scan();
    input_poll();
    pi_reset_poll();
    mac_sample();
    if (enabled) psmac_ip_poll();
}
