/* SPDX-License-Identifier: MPL-2.0
 *
 * splash_test.c - the HDMI splash (emu68/src/psmac/psmac_splash.c, docs/08 item 128) drawn on the host at the
 * screen sizes a Pi's HDMI gives, into 16- and 32-bit buffers as the VideoCore's, with checks: the background at
 * the corners, the logo in the middle at its size, the text where it belongs, nothing written outside the screen's
 * rows (a guard band past the pitch and below the last row), the 16-bit picture the 32-bit one rounded, and a
 * line redraw that touches only its own rows. `splash_test DIR` also writes DIR/splash-WxH.ppm to look at.
 * Run by tools/splash_test.sh (make -C mac check).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psmac_splash.h"

const int modulo = 192;          /* start_rpi64.c's: the Topaz bitmap's bytes a row */
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t px(const uint8_t *fb, uint32_t pitch, int bpp32, uint32_t x, uint32_t y)
{
    const uint8_t *p = fb + (size_t)y * pitch + x * (bpp32 ? 4 : 2);
    if (bpp32) return (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
    uint32_t v = p[0] | p[1] << 8, r = v >> 11, g = v >> 5 & 63, b = v & 31;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}
static int near(uint32_t a, uint32_t b, int tol)
{
    for (int s = 0; s < 24; s += 8) { int d = (int)(a >> s & 255) - (int)(b >> s & 255); if (d < -tol || d > tol) return 0; }
    return 1;
}

static void one(uint32_t w, uint32_t h, const char *dir)
{
    const struct psmac_splash_text t = { "Aurora68", "Emu68 1.1.0-beta.2 (23.11.2026) git:0123abc", "Starting the Macintosh...",
                                         "*** test: an alert line ***" };
    uint32_t pitch32 = w * 4 + 64, pitch16 = w * 2 + 32, guard = 4096;
    uint8_t *b32 = malloc((size_t)pitch32 * h + guard), *b16 = malloc((size_t)pitch16 * h + guard);
    memset(b32, 0xa5, (size_t)pitch32 * h + guard); memset(b16, 0xa5, (size_t)pitch16 * h + guard);
    struct psmac_splash_fb f32 = { b32, pitch32, w, h, 1, 0, 0 }, f16 = { b16, pitch16, w, h, 0, 0, 0 };
    psmac_splash_paint(&f32, &t, PSMAC_SPLASH_ALL);
    psmac_splash_paint(&f16, &t, PSMAC_SPLASH_ALL);
    const uint32_t bg = 0x0e0f12;
    CHECK(px(b32, pitch32, 1, 0, 0) == bg && px(b32, pitch32, 1, w - 1, h - 1) == bg, "%ux%u: the corners are the background", w, h);
    uint32_t L = psmac_splash_logo_size(w, h);
    if (h >= 480) {
        uint32_t mid = px(b32, pitch32, 1, w / 2, h * 9 / 20 - 40);
        CHECK(L >= 96 && L <= 256, "%ux%u: logo size %u", w, h, L);
        CHECK(!near(mid, bg, 30), "%ux%u: the logo is in the middle (%06x)", w, h, mid);
    } else CHECK(L <= w && L <= h, "%ux%u: logo size %u fits", w, h, L);
    /* nothing past each row's pixels nor below the last row */
    int clean = 1;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = w * 4; x < pitch32; x++) if (b32[(size_t)y * pitch32 + x] != 0xa5) clean = 0;
    for (uint32_t i = 0; i < guard; i++) if (b32[(size_t)pitch32 * h + i] != 0xa5 || b16[(size_t)pitch16 * h + i] != 0xa5) clean = 0;
    CHECK(clean, "%ux%u: nothing written outside the screen", w, h);
    /* the 16-bit picture is the 32-bit one, rounded */
    int same = 1;
    for (uint32_t y = 0; y < h; y += 7) for (uint32_t x = 0; x < w; x += 5) if (!near(px(b32, pitch32, 1, x, y), px(b16, pitch16, 0, x, y), 9)) same = 0;
    CHECK(same, "%ux%u: 16 and 32 bits agree", w, h);
    /* the alert line is amber somewhere, the status line grey somewhere, both in the lower part */
    int amber = 0, grey = 0;
    for (uint32_t y = h / 2; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        uint32_t c = px(b32, pitch32, 1, x, y);
        if (c == 0xf0b44a) amber = 1;
        if (c == 0x9a9ca5) grey = 1;
    }
    CHECK((amber && grey) || h < 480, "%ux%u: the alert and status lines are drawn", w, h);
    /* a new status line: only its rows change, and the old text is gone */
    uint8_t *copy = malloc((size_t)pitch32 * h);
    memcpy(copy, b32, (size_t)pitch32 * h);
    const struct psmac_splash_text t2 = { "Aurora68", t.version, "Starting from the SD card...", "" };
    psmac_splash_paint(&f32, &t2, PSMAC_SPLASH_LINES);
    uint32_t changed_lo = h, changed_hi = 0;
    for (uint32_t y = 0; y < h; y++) if (memcmp(copy + (size_t)y * pitch32, b32 + (size_t)y * pitch32, w * 4)) { if (y < changed_lo) changed_lo = y; changed_hi = y; }
    CHECK((changed_lo > h / 2 && changed_hi < h) || h < 480, "%ux%u: a line redraw keeps to the bottom part (rows %u-%u)", w, h, changed_lo, changed_hi);
    int amber2 = 0;
    for (uint32_t y = h / 2; y < h; y++) for (uint32_t x = 0; x < w; x++) if (px(b32, pitch32, 1, x, y) == 0xf0b44a) amber2 = 1;
    CHECK(!amber2, "%ux%u: an empty alert clears the line", w, h);
    /* the first picture, to look at */
    if (dir) {
        psmac_splash_paint(&f32, &t, PSMAC_SPLASH_LINES);
        char name[512]; snprintf(name, sizeof name, "%s/splash-%ux%u.ppm", dir, w, h);
        FILE *o = fopen(name, "wb");
        if (o) {
            fprintf(o, "P6\n%u %u\n255\n", w, h);
            for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
                uint32_t c = px(b32, pitch32, 1, x, y); fputc(c >> 16, o); fputc(c >> 8 & 255, o); fputc(c & 255, o);
            }
            fclose(o);
        }
    }
    free(copy); free(b32); free(b16);
}

int main(int argc, char **argv)
{
    static const uint32_t sizes[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1366, 768 }, { 1920, 1080 },
                                         { 1920, 1200 }, { 2560, 1440 }, { 480, 640 }, { 512, 342 }, { 64, 64 } };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) one(sizes[i][0], sizes[i][1], argc > 1 ? argv[1] : 0);
    printf("splash_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
