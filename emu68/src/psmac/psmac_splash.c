/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_splash.c - the HDMI splash: the Aurora68 logo, the name and the kernel's version on a dark screen, with
 * one quiet line near the bottom that says what the boot is doing, and a line above it for what a user must see.
 *
 * Backstory (23 November 2026, docs/08 item 128). Since 17 November the Pi's HDMI screen showed the kernel's log
 * from power-on until the Mac's video driver took the screen (psmac_display.c, "the HDMI console"): the bench's
 * only window into a Pi with no serial cable. Once the machine worked, the owner called it what it had become for
 * everyone else: "we need a subtle splash screen on the HDMI monitor that has the Aurora logo and version and not
 * show the logs unless specifically called for (it's just noise now)". So by default HDMI shows this page instead,
 * and the log only with mac.console=boot (until the Mac takes the screen, as before) or mac.console=hdmi (the whole
 * run). The serial line, the log ring and the card's log are unchanged.
 *
 * What is drawn. Background #0E0F12 (near-black, a touch of blue: the logo's black outline sinks into it, so the
 * aurora square floats); the logo centred a little above the middle, a fifth of the screen's height (96 to 256
 * pixels: 96 on 640 x 480, 216 on 1920 x 1080, 256 on 2560 x 1440), scaled from the 192 x 192 picture
 * tools/mklogo.py makes from docs/img/aurora68-logo.svg (an area average when smaller, bilinear when larger);
 * "Aurora68" under it in Emu68's own Topaz font at twice the size of the rest, the version under that in grey, the
 * status line a tenth of the height from the bottom in a dimmer grey, and the alert line above it in amber. Text
 * is Topaz 8 x 8 drawn at a whole multiple, u = the screen's height / 540 (1 up to 1079 lines, 2 from 1080): square
 * pixels, so it reads as a label rather than as the console's double-height terminal.
 *
 * Portable C with no kernel calls (the frame buffer's bytes are written one by one, so the kernel's big-endian
 * build and the host's little-endian test write the same pixels); the font comes from Emu68's topaz.c. psmac_display.c
 * calls it on the service core, and once from display_logo (start_rpi64.c) before any core but the first runs.
 */
#include "psmac_splash.h"
#include "psmac_splash_logo.h"

extern const uint8_t topaz8_chardata[]; extern const uint32_t topaz8_charloc[]; extern const int modulo;

#define BG       SPLASH_BG_RGB
#define C_NAME   0xe6e7ebu                /* the name: soft white */
#define C_VER    0x7c7f88u                /* the version: grey */
#define C_STATUS 0x9a9ca5u                /* the status line: a quiet grey, a shade lighter than the version */
#define C_ALERT  0xf0b44au                /* the alert line: amber */

static uint32_t mn(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* one pixel's bytes, in the buffer's order */
static void pix_bytes(const struct psmac_splash_fb *f, uint32_t rgb, uint8_t out[4])
{
    uint32_t r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
    if (f->bpp32) {
        out[0] = (uint8_t)(f->swap ? r : b); out[1] = (uint8_t)g; out[2] = (uint8_t)(f->swap ? b : r); out[3] = 0;
    } else {
        uint32_t v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
        out[0] = (uint8_t)v; out[1] = (uint8_t)(v >> 8);
    }
}
static void flush(const struct psmac_splash_fb *f, uint8_t *p, uint32_t n) { if (f->flush && n) f->flush(p, n); }

/* rows y0..y1 filled with one colour */
static void fill_rows(const struct psmac_splash_fb *f, uint32_t y0, uint32_t y1, uint32_t rgb)
{
    uint8_t px[4]; pix_bytes(f, rgb, px);
    uint32_t bpp = f->bpp32 ? 4 : 2, n = f->w * bpp;
    if (y1 > f->h) y1 = f->h;
    for (uint32_t y = y0; y < y1; y++) {
        uint8_t *row = f->fb + (uint64_t)y * f->pitch;
        if (y == y0) for (uint32_t x = 0; x < n; x++) row[x] = px[x % bpp];
        else {
            const uint8_t *first = f->fb + (uint64_t)y0 * f->pitch;
            for (uint32_t x = 0; x < n; x++) row[x] = first[x];
        }
        flush(f, row, n);
    }
}

/* a line of text centred on x = w / 2, its top at y, each of Topaz's pixels sx by sy; cut to the screen's width */
static void text(const struct psmac_splash_fb *f, const char *s, uint32_t y, uint32_t sx, uint32_t sy, uint32_t rgb)
{
    if (!s || !s[0]) return;
    uint32_t n = 0; while (s[n]) n++;
    uint32_t maxc = f->w / (8 * sx);
    if (maxc > 2) maxc -= 2;                                     /* a margin of a character each side */
    if (n > maxc) n = maxc;
    uint32_t x0 = (f->w - n * 8 * sx) / 2, bpp = f->bpp32 ? 4 : 2;
    uint8_t ink[4]; pix_bytes(f, rgb, ink);
    for (uint32_t r = 0; r < 8; r++) {
        for (uint32_t k = 0; k < sy; k++) {
            uint32_t yy = y + r * sy + k;
            if (yy >= f->h) return;
            uint8_t *row = f->fb + (uint64_t)yy * f->pitch;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t c = (uint8_t)s[i];
                if (c < 32 || c >= 128) c = '?';
                uint8_t bits = topaz8_chardata[((topaz8_charloc[c - 32] >> 16) >> 3) + r * (uint32_t)modulo];
                if (c == 32) bits = 0;
                for (uint32_t b = 0; b < 8; b++) {
                    if (!(bits & (0x80 >> b))) continue;
                    uint8_t *p = row + (x0 + (i * 8 + b) * sx) * bpp;
                    for (uint32_t q = 0; q < sx; q++, p += bpp)
                        for (uint32_t j = 0; j < bpp; j++) p[j] = ink[j];
                }
            }
            flush(f, row + x0 * bpp, n * 8 * sx * bpp);
        }
    }
}

/* ---- the logo, scaled: each output pixel a weighted sum of at most three source pixels a side (16.16) ---- */
struct tap { uint16_t k[3]; uint32_t w[3]; uint8_t n; };
static struct tap taps[256];
static void make_taps(uint32_t L)
{
    const uint32_t S = SPLASH_LOGO_N;
    for (uint32_t i = 0; i < L; i++) {
        struct tap *t = &taps[i];
        t->n = 0;
        if (L <= S) {                                            /* area: [i*S, (i+1)*S) in units of 1/L of a source pixel */
            uint32_t a = i * S, b = a + S;
            for (uint32_t k = a / L; k * L < b && t->n < 3; k++) {
                uint32_t lo = a > k * L ? a : k * L, hi = b < (k + 1) * L ? b : (k + 1) * L;
                t->k[t->n] = (uint16_t)k; t->w[t->n] = (uint32_t)(((uint64_t)(hi - lo) << 16) / S); t->n++;
            }
        } else {                                                 /* bilinear: the centre of i at ((2i+1)S - L) / 2L */
            int32_t num = (int32_t)((2 * i + 1) * S) - (int32_t)L;
            int32_t k0 = num < 0 ? -1 : num / (int32_t)(2 * L);
            uint32_t fr = num < 0 ? 0 : (uint32_t)(((uint64_t)(uint32_t)(num - k0 * (int32_t)(2 * L)) << 16) / (2 * L));
            int32_t k1 = k0 + 1;
            if (k0 < 0) { k0 = 0; fr = 0; }
            if (k1 > (int32_t)S - 1) k1 = (int32_t)S - 1;
            t->k[0] = (uint16_t)k0; t->w[0] = 65536u - fr; t->k[1] = (uint16_t)k1; t->w[1] = fr; t->n = 2;
        }
    }
}
uint32_t psmac_splash_logo_size(uint32_t w, uint32_t h)
{
    uint32_t m = mn(w, h), L = m / 5;
    L &= ~7u;
    if (L < 96) L = 96;
    if (L > 256) L = 256;
    if (L > m) L = m & ~7u;
    return L;
}
static void logo(const struct psmac_splash_fb *f, uint32_t L, uint32_t x0, uint32_t y0)
{
    static uint32_t vrow[SPLASH_LOGO_N * 3];
    make_taps(L);
    uint32_t bpp = f->bpp32 ? 4 : 2;
    for (uint32_t y = 0; y < L && y0 + y < f->h; y++) {
        const struct tap *ty = &taps[y];
        for (uint32_t i = 0; i < SPLASH_LOGO_N * 3; i++) {
            uint32_t v = 0;
            for (uint32_t j = 0; j < ty->n; j++) v += splash_logo_rgb[ty->k[j] * SPLASH_LOGO_N * 3 + i] * ty->w[j];
            vrow[i] = v;                                         /* 8.16 */
        }
        uint8_t *row = f->fb + (uint64_t)(y0 + y) * f->pitch + x0 * bpp;
        for (uint32_t x = 0; x < L && x0 + x < f->w; x++) {
            const struct tap *tx = &taps[x];
            uint32_t c[3];
            for (int ch = 0; ch < 3; ch++) {
                uint64_t v = 0;
                for (uint32_t j = 0; j < tx->n; j++) v += (uint64_t)vrow[tx->k[j] * 3 + ch] * tx->w[j];
                uint32_t b = (uint32_t)((v + (1ull << 31)) >> 32);
                c[ch] = b > 255 ? 255 : b;
            }
            uint8_t px[4]; pix_bytes(f, c[0] << 16 | c[1] << 8 | c[2], px);
            for (uint32_t j = 0; j < bpp; j++) row[x * bpp + j] = px[j];
        }
        flush(f, row, L * bpp);
    }
}

/* where everything goes on a w x h screen */
struct layout { uint32_t u, L, logo_x, logo_y, name_y, ver_y, alert_y, status_y, line_h; };
static void place(uint32_t w, uint32_t h, struct layout *o)
{
    o->u = h / 540 ? h / 540 : 1;
    if (o->u > 3) o->u = 3;
    o->L = psmac_splash_logo_size(w, h);
    uint32_t u = o->u, gap1 = o->L / 6, name_h = 16 * u, gap2 = 8 * u, ver_h = 8 * u;
    uint32_t block = o->L + gap1 + name_h + gap2 + ver_h;
    uint32_t top = h * 9 / 20 > block / 2 ? h * 9 / 20 - block / 2 : 0;
    o->logo_x = (w - o->L) / 2; o->logo_y = top;
    o->name_y = top + o->L + gap1;
    o->ver_y = o->name_y + name_h + gap2;
    o->line_h = 8 * u;
    o->status_y = h - h / 10 - o->line_h / 2;
    o->alert_y = o->status_y - 3 * o->line_h;
}

void psmac_splash_paint(const struct psmac_splash_fb *f, const struct psmac_splash_text *t, int what)
{
    if (!f->fb || !f->pitch || f->w < 64 || f->h < 64) return;
    struct layout l; place(f->w, f->h, &l);
    if (what == PSMAC_SPLASH_ALL) {
        fill_rows(f, 0, f->h, BG);
        logo(f, l.L, l.logo_x, l.logo_y);
        text(f, t->name, l.name_y, 2 * l.u, 2 * l.u, C_NAME);
        text(f, t->version, l.ver_y, l.u, l.u, C_VER);
    } else {
        fill_rows(f, l.alert_y, l.alert_y + l.line_h, BG);
        fill_rows(f, l.status_y, l.status_y + l.line_h, BG);
    }
    text(f, t->alert, l.alert_y, l.u, l.u, C_ALERT);
    text(f, t->status, l.status_y, l.u, l.u, C_STATUS);
}
