/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_display.c — the second display: the Mac's frame buffer shown by the Pi's VideoCore.
 *
 * Backstory. The Macintosh SE has one screen, 512 by 342, one bit, drawn by the motherboard out of
 * the first 22 KB of RAM. The IIci ROM this machine runs was made for colour cards in NuBus slots,
 * so the whole software side of a second monitor exists already: the Slot Manager finds a video
 * sResource, the Start Manager opens its driver, QuickDraw draws into its frame buffer, Monitors
 * arranges it (docs/10). What a card does in hardware, scanning the buffer out to a monitor, the
 * Pi's VideoCore does here, and this file is the glue between the two: the Mac's video driver
 * (mac/pivideo.s) draws at the depth QuickDraw chose and keeps a colour table beside it (struct
 * psmb_display, psmb.h); the service core, sixty times a second, converts those pixels into the
 * VideoCore frame buffer and, while the driver wants it, raises the slot interrupt, which is the
 * card's vertical blank. In the testbed QEMU emulates that frame buffer and `tools/vcshot.py`
 * photographs it; on hardware it is the HDMI output.
 *
 * Two frame buffers, by history. The first version kept the Mac's pixels in the window's top
 * 512 KiB, because that megabyte is the one a 24-bit System 6 can reach ($980000); it holds
 * 640 x 480 at up to 8 bits and still does. Direct colour (16 and 32 bits) and larger screens do
 * not fit there, so there is also a big region, 8 MiB of Pi RAM mapped at $F9100000 in the slot's
 * 16 MiB space, which 32-bit systems use. The block's mode tables say which region each mode
 * uses; the driver never decides. The VideoCore buffer is remade at the screen's size and 32
 * bits at start-up (the boot splash used 16), and the conversion loop writes XRGB words into it:
 * the Mac's indexed pixels through the colour table, its 16-bit 0RRRRRGGGGGBBBBB words widened,
 * its 32-bit 00RRGGBB longs byte-swapped. A copy is cheap next to the alternative of mapping the
 * VideoCore buffer into the Mac's space: the pixel orders differ, the Mac's modes need the table,
 * and Pi RAM is where the 68k's JIT is fastest. The screen's size comes from mac.screen=WxH, else
 * from the VideoCore (the firmware's HDMI mode on hardware, 640 x 480 in QEMU).
 */
#include <stdint.h>
#include "support.h"
#include "psmac.h"
#include "psmb.h"

extern void *framebuffer;                    /* start_rpi64.c: the VideoCore frame buffer */
extern uint32_t pitch, fb_width, fb_height;
extern uint32_t psmac_vc_setup(uint32_t w, uint32_t h, uint32_t depth, void **fb, uint32_t *pitch_out);
extern uintptr_t psmac_fb_big_phys(void);
extern void psmac_screen_size(uint32_t *w, uint32_t *h);

#define KERNEL_ALIAS 0xffffff9000000000ull
static volatile struct psmb_display *disp;
static uint8_t *frame_win;                   /* the Mac's pixels: the window region ... */
static uint8_t *frame_big;                   /* ... and the big region, through the kernel's alias */
static uint8_t *vcfb;                        /* the VideoCore frame buffer, through the alias too (the low identity
                                                mapping the splash drew through does not survive the Mac MMU shim's
                                                table switches, docs/08 item 21) */
static uint32_t vc_pitch, vc_w, vc_h, vc_bpp = 32;
static int vc_swap;                          /* write the bytes R,G,B,x rather than B,G,R,x (see vc_byte_order) */
extern uint32_t psmac_vc_pixel_order;        /* support_rpi.c: the firmware's answer to "get pixel order" */
extern int psmac_bootarg_val(const char *key, char *out, unsigned max);   /* alice_net.c */
static void vc_byte_order(void);   /* the VideoCore buffer's depth: 32 asked for, 16 if that is what came back */
static uint32_t rgb[256];                    /* the colour table as XRGB */
static uint32_t clut_seen = ~0u;
static uint64_t next_tick;
static int con_keep;                          /* the HDMI console (below): mac.console=hdmi keeps the log on the screen */
static int con_off;                           /* the picture has the buffer */
static uint32_t con_x, con_y, con_cols, con_rows;
static unsigned tick_div;

void psmac_display_init(uint8_t *win)
{
    disp = (volatile struct psmb_display *)(win + PSMB_OFF_DISPLAY);
    frame_win = win + PSMB_OFF_BOUNCE;
    frame_big = (uint8_t *)(KERNEL_ALIAS + psmac_fb_big_phys());
    for (uint32_t i = 0; i < sizeof(struct psmb_display) / 4; i++) ((volatile uint32_t *)disp)[i] = 0;

    uint32_t w, h; psmac_screen_size(&w, &h);
    if (!w || !h) { w = fb_width ? fb_width : 640; h = fb_height ? fb_height : 480; }
    /* the VideoCore buffer at that size, 32 bits */
    void *fb = 0; uint32_t p = 0;
    uint32_t got = psmac_vc_setup(w, h, 32, &fb, &p);
    if (got != 32 || p < w * 4) {               /* not granted (QEMU's model stays at 16): ask for 16 and convert to RGB565 */
        got = psmac_vc_setup(w, h, 16, &fb, &p);
        vc_bpp = 16;
    }
    if (fb && p) { framebuffer = fb; pitch = p; fb_width = w; fb_height = h; }
    vc_w = fb_width; vc_h = fb_height; vc_pitch = pitch;
    vc_byte_order();
    if (framebuffer) vcfb = (uint8_t *)(KERNEL_ALIAS + (((uintptr_t)framebuffer) & 0x3fffffffull));
    if (vcfb) for (uint32_t i = 0; i < vc_pitch * vc_h; i += 8) *(uint64_t *)(vcfb + i) = 0;
    con_cols = 0; con_x = 0; con_y = 0;                        /* the console starts again in the new buffer */

    disp->width = w; disp->height = h; disp->depth = 1;
    disp->fb_off = PSMB_OFF_BOUNCE; disp->fb_size = PSMB_BOUNCE_SIZE;
    disp->big_base = PSMB_FB_BIG_PHYS; disp->big_size = PSMB_FB_BIG_SIZE;
    disp->vc_width = vc_w; disp->vc_height = vc_h; disp->vc_depth = vc_bpp;
    /* the six modes: where each one's pixels live and its row pitch */
    for (int i = 0; i < 6; i++) {
        uint32_t d = 1u << i, rb = (w * d + 7) / 8;
        uint32_t bytes = rb * h;
        if (d <= 8 && bytes <= PSMB_BOUNCE_SIZE) { disp->mode_base[i] = PSMB_WIN_PHYS + 0x900000u + PSMB_OFF_BOUNCE; }   /* $F9980000: reachable in 24-bit mode */
        else if (bytes <= PSMB_FB_BIG_SIZE) disp->mode_base[i] = PSMB_FB_BIG_PHYS;
        else disp->mode_base[i] = 0;                                      /* no such mode at this size */
        disp->mode_rowbytes[i] = rb;
    }
    disp->rowbytes = disp->mode_rowbytes[0];
    /* a colour table for a card that was just switched on: 0 white ... 255 black, the Mac's order */
    for (int i = 0; i < 256; i++) { uint32_t g = 255 - i; disp->clut[i] = g << 16 | g << 8 | g; }
    for (uint32_t i = 0; i < PSMB_BOUNCE_SIZE; i += 8) *(uint64_t *)(frame_win + i) = 0;
    __asm__ volatile("dmb ish" ::: "memory");
    disp->version = 2; disp->magic = PSMB_DISP_MAGIC;
    kprintf("[PSMAC:DISP] second display %ux%u: indexed modes in the window at $F9980000, direct modes in the big region at $F9100000 (%u MiB); VideoCore %ux%u x%u pitch %u at %p\n",
            w, h, PSMB_FB_BIG_SIZE >> 20, vc_w, vc_h, vc_bpp, vc_pitch, framebuffer);
}

/* Which byte of a 32-bit pixel is red. The firmware is asked (tag 0x40006): 1 means the bytes
   run R,G,B,x in memory, 0 means B,G,R,x. QEMU's model answers 1 and reads the first byte as red;
   a real Pi answers for its own scan-out. Should a board show the Mac's reds as blues anyway,
   mac.vcswap=1 (or 0) overrides the answer, so no rebuild stands between a user and the right
   colours. 16-bit buffers are RGB565 in both worlds and need no choice. */
static void vc_byte_order(void)
{
    vc_swap = psmac_vc_pixel_order == 1;
    char o[4];
    if (psmac_bootarg_val("mac.vcswap", o, sizeof o) && (o[0] == '0' || o[0] == '1')) vc_swap = o[0] == '1';
}

/* After a 68000 RESET instruction (the ROM issues one at the start of every boot) the generic
   Emu68 code used to remake the VideoCore buffer at 16 bits: the Mac's second display went
   on converting into a 32-bit layout the scan-out no longer read, and showed thin dark stripes.
   The JIT now calls this instead, which asks for the same buffer again. */
void psmac_display_reinit(void)
{
    if (!vc_w || !vc_h) return;
    void *fb = 0; uint32_t p = 0;
    psmac_vc_setup(vc_w, vc_h, vc_bpp, &fb, &p);
    if (fb && p) { framebuffer = fb; pitch = p; vc_pitch = p; vcfb = (uint8_t *)(KERNEL_ALIAS + (((uintptr_t)fb) & 0x3fffffffull)); }
    vc_byte_order();
    if (vcfb) for (uint32_t i = 0; i < vc_pitch * vc_h; i += 8) *(uint64_t *)(vcfb + i) = 0;
    con_cols = 0; con_x = 0; con_y = 0;                        /* the console starts again in the new buffer */
}

/* ---- the HDMI console ------------------------------------------------------------------------
   A bench with no serial cable has no log. So the kernel's text output is drawn here too, into
   whatever VideoCore buffer there is (Emu68's 16-bit splash buffer first, the 32-bit one once
   psmac_display_init has made it), with Emu68's own Topaz font, until the Macintosh's video driver
   turns the second display on and the picture takes the buffer over. mac.console=hdmi keeps the
   log there for the whole run and never shows the picture: that is the bench setting. The cursor
   wraps to the top when it reaches the bottom and clears the line ahead of it, which costs nothing
   and reads like a terminal that lost its scrollback. Written for the user's first standalone Pi,
   17 November, 05:40 (docs/12). */
extern const uint8_t topaz8_chardata[]; extern const uint32_t topaz8_charloc[]; extern const int modulo;
static uint8_t *con_fb(void)
{
    if (vcfb) return vcfb;
    if (framebuffer) return (uint8_t *)(KERNEL_ALIAS + (((uintptr_t)framebuffer) & 0x3fffffffull));
    return 0;
}
static void con_cell(uint8_t *fb, uint32_t pitch_, int bpp32, uint32_t cx, uint32_t cy, uint8_t c)
{
    uint32_t loc = (topaz8_charloc[(c >= 32 && c < 128) ? c - 32 : 0] >> 16) >> 3;
    const uint8_t *data = &topaz8_chardata[loc];
    for (uint32_t y = 0; y < 16; y++) {
        uint8_t *row = fb + (cy * 16 + y) * pitch_ + cx * 8 * (bpp32 ? 4 : 2);
        uint8_t bits = c >= 32 ? *data : 0;
        for (uint32_t x = 0; x < 8; x++) {
            int on = bits & (0x80 >> x);
            if (bpp32) ((uint32_t *)row)[x] = on ? LE32(0x00e0e0e0u) : 0; else ((uint16_t *)row)[x] = on ? LE16(0xe71c) : 0;
        }
        if (y & 1) data += modulo;
    }
}
void psmac_console_putc(uint8_t c)
{
    if (con_off && !con_keep) return;
    uint8_t *fb = con_fb();
    uint32_t p = vc_pitch ? vc_pitch : pitch;
    if (!fb || !p) return;
    if (!con_cols) {
        uint32_t w = vc_w ? vc_w : fb_width, h = vc_h ? vc_h : fb_height;
        con_cols = w / 8; con_rows = h / 16;
        if (!con_cols || !con_rows) { con_cols = 0; return; }
        con_y = con_rows - 1; con_x = 0;                      /* start under the splash */
    }
    int bpp32 = vc_bpp == 32 && vcfb;
    if (c == '\r') return;
    if (c == '\n' || con_x >= con_cols) {
        con_x = 0; con_y++;
        if (con_y >= con_rows) con_y = 0;
        for (uint32_t cx = 0; cx < con_cols; cx++) con_cell(fb, p, bpp32, cx, con_y, 0);
        if (c == '\n') return;
    }
    con_cell(fb, p, bpp32, con_x++, con_y, c);
}
void psmac_console_option(int keep) { con_keep = keep; }

/* one output pixel: XRGB8888 when the VideoCore buffer is 32 bits, RGB565 when it is 16 */
static inline void put(void *row, uint32_t x, uint32_t xrgb)
{
    if (vc_bpp == 32) ((uint32_t *)row)[x] = LE32(vc_swap ? (xrgb & 0xff00) | (xrgb >> 16 & 0xff) | (xrgb & 0xff) << 16 : xrgb);
    else ((uint16_t *)row)[x] = LE16((uint16_t)(((xrgb >> 8) & 0xf800) | ((xrgb >> 5) & 0x07e0) | ((xrgb >> 3) & 0x001f)));
}

/* the Mac's pixels -> XRGB; depth 1/2/4: the leftmost pixel is in the high bits of the byte */
static void convert(void)
{
    if (!vcfb) return;
    uint32_t depth = disp->depth, rb = disp->rowbytes;
    uint32_t h = disp->height < vc_h ? disp->height : vc_h, w = disp->width < vc_w ? disp->width : vc_w;
    int mi = depth == 1 ? 0 : depth == 2 ? 1 : depth == 4 ? 2 : depth == 8 ? 3 : depth == 16 ? 4 : depth == 32 ? 5 : -1;
    if (mi < 0 || !disp->mode_base[mi]) return;
    const uint8_t *frame = disp->mode_base[mi] == PSMB_FB_BIG_PHYS ? frame_big : frame_win;
    uint32_t limit = disp->mode_base[mi] == PSMB_FB_BIG_PHYS ? PSMB_FB_BIG_SIZE : PSMB_BOUNCE_SIZE;
    if (rb * h > limit) return;
    if (disp->clut_seq != clut_seen) {
        clut_seen = disp->clut_seq;
        for (int i = 0; i < 256; i++) {
            uint32_t c = disp->clut[i] & 0xffffffu;
            if (disp->flags & PSMB_DISP_GRAY) { uint32_t y = (((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 151 + (c & 255) * 28) >> 8; c = y << 16 | y << 8 | y; }
            rgb[i] = c;
        }
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = frame + y * rb;
        void *dst = vcfb + y * vc_pitch;
        switch (depth) {
        case 32: { const uint32_t *s = (const uint32_t *)src; for (uint32_t x = 0; x < w; x++) put(dst, x, s[x] & 0xffffffu); break; }   /* 00RRGGBB, big-endian longs read natively */
        case 16: { const uint16_t *s = (const uint16_t *)src;
                   for (uint32_t x = 0; x < w; x++) { uint32_t v = s[x], r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                       put(dst, x, (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2)); }
                   break; }
        case 8: for (uint32_t x = 0; x < w; x++) put(dst, x, rgb[src[x]]); break;
        case 4: for (uint32_t x = 0; x < w; x += 2) { uint8_t b = src[x >> 1]; put(dst, x, rgb[b >> 4]); put(dst, x + 1, rgb[b & 15]); } break;
        case 2: for (uint32_t x = 0; x < w; x += 4) { uint8_t b = src[x >> 2]; for (int k = 0; k < 4; k++) put(dst, x + k, rgb[(b >> (6 - 2 * k)) & 3]); } break;
        case 1: for (uint32_t x = 0; x < w; x += 8) { uint8_t b = src[x >> 3]; for (int k = 0; k < 8; k++) put(dst, x + k, rgb[(b >> (7 - k)) & 1]); } break;
        }
    }
    arm_flush_cache((intptr_t)vcfb, vc_pitch * h);
    { extern void psmac_se_mark(const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t); psmac_se_mark(frame, disp->width, disp->height, rb, depth); }   /* the SE's own screen (psmac_board_se.c) */
    disp->frames++;
}

/* from the service core's loop: the 60 Hz tick (VBL) and, every other tick, the picture */
void psmac_display_poll(void)
{
    if (!disp) return;
    uint64_t now; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(now));
    uint64_t hz; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(hz));
    if (!next_tick) { next_tick = now + hz / 60; return; }
    if ((int64_t)(now - next_tick) < 0) return;
    next_tick += hz / 60; if ((int64_t)(now - next_tick) > (int64_t)hz) next_tick = now + hz / 60;
    disp->vbl_count++;
    if ((disp->flags & PSMB_DISP_ENABLED) && disp->vbl_enable) { disp->vbl_pending = 1; __asm__ volatile("dmb ish" ::: "memory"); }
    if ((disp->flags & PSMB_DISP_ENABLED) && !con_keep) { con_off = 1; if (++tick_div & 1) convert(); }
}

/* for the heartbeat */
void psmac_display_report(void)
{
    if (!disp) return;
    kprintf("[PSMAC]   display: %ux%u flags %x depth %u rowbytes %u vbl %s pending %u ticks %u frames %u clut seq %u\n",
            disp->width, disp->height, disp->flags, disp->depth, disp->rowbytes, disp->vbl_enable ? "on" : "off", disp->vbl_pending, disp->vbl_count, disp->frames, disp->clut_seq);
}

/* the slot interrupt line's share: the card's VBL is pending and the driver is there to take it */
int psmac_display_irq(void) { return disp && (disp->flags & PSMB_DISP_ENABLED) && disp->vbl_enable && disp->vbl_pending; }
