/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_splash.h - the HDMI splash's drawing (psmac_splash.c): portable C, built into the kernel and, by
 * tools/splash_test.sh, on the host. psmac_display.c decides when it shows and what its lines say.
 */
#ifndef PSMAC_SPLASH_H
#define PSMAC_SPLASH_H
#include <stdint.h>

struct psmac_splash_fb {
    uint8_t *fb;                         /* the frame buffer as the kernel reaches it */
    uint32_t pitch, w, h;                /* bytes a row, pixels */
    int bpp32;                           /* 1: XRGB8888 (bytes B,G,R,x; R,G,B,x with swap), 0: RGB565 little-endian */
    int swap;
    void (*flush)(uint8_t *p, uint32_t n);   /* written bytes out of the cache (the VideoCore reads memory); may be 0 */
};
struct psmac_splash_text {
    const char *name;                    /* "Aurora68" */
    const char *version;                 /* the kernel's version, as the log's first lines print it */
    const char *status;                  /* what the boot is doing now ("Starting the Macintosh..."); may be empty */
    const char *alert;                   /* something the user must see (no declaration ROM, a system error); may be empty */
};
#define PSMAC_SPLASH_ALL   0             /* the whole screen */
#define PSMAC_SPLASH_LINES 1             /* the status and alert lines only (the rest is drawn already) */
void psmac_splash_paint(const struct psmac_splash_fb *f, const struct psmac_splash_text *t, int what);
uint32_t psmac_splash_logo_size(uint32_t w, uint32_t h);     /* the logo's size on such a screen (for the test) */
#endif
