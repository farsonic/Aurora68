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
 * not fit there, so there is also a big region, 9 MiB of Pi RAM mapped at $90000000 in the slot's
 * super slot space (8 MiB at $F9100000 until 1920 x 1200 at 32 bits needed more, docs/10), which
 * 32-bit systems use. The block's mode tables say which region each mode uses; the driver never
 * decides, and tools/mac/mkdeclrom.py writes the same rule into the declaration ROM. The VideoCore buffer is remade at the screen's size and 32
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
static int vc_fresh;                          /* the buffer was made moments ago and nothing has drawn the picture in it */

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
    vc_fresh = 1;

    disp->width = w; disp->height = h; disp->depth = 1;
    disp->fb_off = PSMB_OFF_BOUNCE; disp->fb_size = PSMB_BOUNCE_SIZE;
    disp->big_base = PSMB_FB_BIG_PHYS; disp->big_size = PSMB_FB_BIG_SIZE;
    disp->vc_width = vc_w; disp->vc_height = vc_h; disp->vc_depth = vc_bpp;
    /* the six modes: where each one's pixels live and its row pitch. tools/mac/mkdeclrom.py applies
       the same rule to each mode's vpBaseOffset, because the ROM takes the address from there and
       never asks the driver (docs/10, "the third trap"); change one and change the other */
    for (int i = 0; i < 6; i++) {
        uint32_t d = 1u << i, rb = (w * d + 7) / 8;
        uint32_t bytes = rb * h;
        if (d <= 8 && bytes <= PSMB_BOUNCE_SIZE) { disp->mode_base[i] = PSMB_WIN_PHYS + 0x900000u + PSMB_OFF_BOUNCE; }   /* $F9980000: reachable in 24-bit mode */
        else if (bytes <= PSMB_FB_BIG_SIZE) disp->mode_base[i] = PSMB_FB_BIG_PHYS;               /* $90000000: 32-bit addressing only */
        else disp->mode_base[i] = 0;                                      /* no such mode at this size */
        disp->mode_rowbytes[i] = rb;
    }
    disp->rowbytes = disp->mode_rowbytes[0];
    /* a colour table for a card that was just switched on: 0 white ... 255 black, the Mac's order */
    for (int i = 0; i < 256; i++) { uint32_t g = 255 - i; disp->clut[i] = g << 16 | g << 8 | g; }
    for (uint32_t i = 0; i < PSMB_BOUNCE_SIZE; i += 8) *(uint64_t *)(frame_win + i) = 0;
    __asm__ volatile("dmb ish" ::: "memory");
    disp->version = 2; disp->magic = PSMB_DISP_MAGIC;
    kprintf("[PSMAC:DISP] second display %ux%u: indexed modes in the window at $F9980000, direct modes in the big region at $%08x (%u MiB); VideoCore %ux%u x%u pitch %u at %p\n",
            w, h, PSMB_FB_BIG_PHYS, PSMB_FB_BIG_SIZE >> 20, vc_w, vc_h, vc_bpp, vc_pitch, framebuffer);
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
    /* The ROM's first RESET comes moments after psmac_display_init made and cleared this very
       buffer; asking the firmware for it again held the 68k's first instruction for a mailbox
       round trip and a clear of up to 9 MB (docs/08 item 50). A Restart's RESET still remakes it. */
    if (vc_fresh) { vc_fresh = 0; return; }
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
        arm_flush_cache((intptr_t)row, 8 * (bpp32 ? 4 : 2));    /* the VideoCore reads memory, not our cache: on the
                                                                   first real Pi the glyphs arrived a cache line at a time */
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
/* The picture, a band at a time. A whole 1920 by 1200 frame in millions of colours is 9 MB read,
   9 MB written and 9 MB flushed, some 20 ms of a Pi 3's core on every other 60 Hz tick, and the
   service loop runs nothing else meanwhile: on 19 November the user's Pi 3B at 1920 by 1200 pinged
   its router over the WiFi in 80 ms, because the WiFi, the network board's interrupt and the disk
   all waited behind the conversion (docs/08 item 49). Now each turn of the loop converts a band of
   about 128 K pixels (a millisecond or so) and the next turn the next band; a frame starts at most
   30 times a second, as before, and takes as many turns as it needs. Small screens are one band. */
#define BAND_PIXELS 131072u
static uint32_t band_y, band_h, band_w, band_mode, band_active;
static uint64_t frame_t0;
static void convert_rows(uint32_t y0, uint32_t y1)
{
    uint32_t depth = disp->depth, rb = disp->rowbytes, w = band_w;
    const uint8_t *frame = disp->mode_base[band_mode] == PSMB_FB_BIG_PHYS ? frame_big : frame_win;
    for (uint32_t y = y0; y < y1; y++) {
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
    arm_flush_cache((intptr_t)(vcfb + y0 * vc_pitch), vc_pitch * (y1 - y0));
}
/* a new frame: what to convert (0 if nothing can be) */
static int convert_start(void)
{
    if (!vcfb) return 0;
    uint32_t depth = disp->depth, rb = disp->rowbytes;
    uint32_t h = disp->height < vc_h ? disp->height : vc_h, w = disp->width < vc_w ? disp->width : vc_w;
    int mi = depth == 1 ? 0 : depth == 2 ? 1 : depth == 4 ? 2 : depth == 8 ? 3 : depth == 16 ? 4 : depth == 32 ? 5 : -1;
    if (mi < 0 || !disp->mode_base[mi]) return 0;
    uint32_t limit = disp->mode_base[mi] == PSMB_FB_BIG_PHYS ? PSMB_FB_BIG_SIZE : PSMB_BOUNCE_SIZE;
    if (rb * h > limit) return 0;
    if (disp->clut_seq != clut_seen) {
        clut_seen = disp->clut_seq;
        for (int i = 0; i < 256; i++) {
            uint32_t c = disp->clut[i] & 0xffffffu;
            if (disp->flags & PSMB_DISP_GRAY) { uint32_t y = (((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 151 + (c & 255) * 28) >> 8; c = y << 16 | y << 8 | y; }
            rgb[i] = c;
        }
    }
    band_y = 0; band_h = h; band_w = w; band_mode = (uint32_t)mi;
    return 1;
}
/* one band; the frame's end does what the whole frame's conversion did at its end */
static void convert_band(void)
{
    uint32_t n = band_w ? BAND_PIXELS / band_w : band_h;
    if (!n) n = 1;
    if (band_y + n > band_h) n = band_h - band_y;
    convert_rows(band_y, band_y + n);
    band_y += n;
    if (band_y < band_h) return;
    band_active = 0;
    { extern void psmac_se_mark(const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);   /* the SE's own screen (psmac_board_se.c) */
      psmac_se_mark(disp->mode_base[band_mode] == PSMB_FB_BIG_PHYS ? frame_big : frame_win, disp->width, disp->height, disp->rowbytes, disp->depth); }
    disp->frames++;
}

/* from the service core's loop, every turn: the 60 Hz tick (VBL), and a band of the picture */
void psmac_display_poll(void)
{
    if (!disp) return;
    uint64_t now; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(now));
    uint64_t hz; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(hz));
    if (!next_tick) { next_tick = now + hz / 60; return; }
    if (band_active) {
        if ((disp->flags & PSMB_DISP_ENABLED) && !con_keep) convert_band();
        else band_active = 0;
    }
    if ((int64_t)(now - next_tick) < 0) return;
    next_tick += hz / 60; if ((int64_t)(now - next_tick) > (int64_t)hz) next_tick = now + hz / 60;
    disp->vbl_count++;
    if ((disp->flags & PSMB_DISP_ENABLED) && disp->vbl_enable) { disp->vbl_pending = 1; __asm__ volatile("dmb ish" ::: "memory"); }
    if ((disp->flags & PSMB_DISP_ENABLED) && !con_keep) {
        con_off = 1;
        /* a new frame every other tick (30 a second) if the last one is done */
        if (++tick_div & 1 && !band_active && now - frame_t0 >= hz / 31 && convert_start()) {
            frame_t0 = now;
            band_active = 1;
            convert_band();
        }
    }
}

/* for the heartbeat */
void psmac_display_report(void)
{
    if (!disp) return;
    kprintf("[PSMAC]   display: %ux%u flags %x depth %u rowbytes %u vbl %s pending %u ticks %u frames %u clut seq %u\n",
            disp->width, disp->height, disp->flags, disp->depth, disp->rowbytes, disp->vbl_enable ? "on" : "off", disp->vbl_pending, disp->vbl_count, disp->frames, disp->clut_seq);
}

/* the slot interrupt line's share: the card's VBL is pending and the driver is there to take it */
/* the 68k was reset: the video driver's VBL interrupt is off until it opens again */
void psmac_display_bus_reset(void) { if (disp) { disp->vbl_enable = 0; disp->vbl_pending = 0; __asm__ volatile("dmb ish" ::: "memory"); } }
int psmac_display_irq(void) { return disp && (disp->flags & PSMB_DISP_ENABLED) && disp->vbl_enable && disp->vbl_pending; }

/* ---- the remote's screenshot (psmac_remote.c, docs/13) ---------------------------------------
   The picture as the Mac drew it, not as the VideoCore scans it out: the frame buffer of the mode
   in use, at its depth, through the colour table, so a screenshot is right whatever the HDMI side
   does (mac.console=hdmi, a 16-bit scan-out, swapped bytes). Until the video driver has switched
   the display on there is no Mac picture, and the VideoCore buffer is the screen (the boot log on
   the console). The parameters are taken once per screenshot, so a depth change half way through
   cannot mix two layouts; the pixels are read while the Mac goes on drawing, as a card's frame
   grabber would. */
static struct { int src; uint32_t w, h, depth, rb; const uint8_t *base; uint32_t pal[256]; } shot;
int psmac_display_shot(uint32_t *w, uint32_t *h, uint32_t *depth, int want_hdmi)
{
    shot.src = 0;
    if (!disp) return 0;
    int mi = disp->depth == 1 ? 0 : disp->depth == 2 ? 1 : disp->depth == 4 ? 2 : disp->depth == 8 ? 3 : disp->depth == 16 ? 4 : disp->depth == 32 ? 5 : -1;
    if (!want_hdmi && (disp->flags & PSMB_DISP_ENABLED) && mi >= 0 && disp->mode_base[mi]) {
        shot.w = disp->width; shot.h = disp->height; shot.depth = disp->depth; shot.rb = disp->rowbytes;
        shot.base = disp->mode_base[mi] == PSMB_FB_BIG_PHYS ? frame_big : frame_win;
        uint32_t limit = disp->mode_base[mi] == PSMB_FB_BIG_PHYS ? PSMB_FB_BIG_SIZE : PSMB_BOUNCE_SIZE;
        if (shot.rb * shot.h > limit || shot.rb < (shot.w * shot.depth + 7) / 8) return 0;
        for (int i = 0; i < 256; i++) {
            uint32_t c = disp->clut[i] & 0xffffffu;
            if (disp->flags & PSMB_DISP_GRAY) { uint32_t y = (((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 151 + (c & 255) * 28) >> 8; c = y << 16 | y << 8 | y; }
            shot.pal[i] = c;
        }
        shot.src = 1;
    } else if (vcfb && vc_w && vc_h) {
        shot.w = vc_w; shot.h = vc_h; shot.depth = vc_bpp; shot.rb = vc_pitch; shot.base = vcfb;
        shot.src = 2;
    } else return 0;
    *w = shot.w; *h = shot.h; *depth = shot.depth;
    return shot.src;
}
/* row y of the screenshot begun above, as R,G,B bytes, at most maxw pixels of it */
void psmac_display_shot_row(uint32_t y, uint8_t *out, uint32_t maxw)
{
    const uint8_t *src = shot.base + y * shot.rb;
    uint32_t w = shot.w < maxw ? shot.w : maxw;
    if (shot.src == 2) {                        /* the VideoCore buffer: little-endian XRGB (or swapped), or RGB565 */
        if (shot.depth == 32) {
            for (uint32_t x = 0; x < w; x++, src += 4, out += 3) {
                if (vc_swap) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; }
                else { out[0] = src[2]; out[1] = src[1]; out[2] = src[0]; }
            }
        } else {
            for (uint32_t x = 0; x < w; x++, src += 2, out += 3) {
                uint32_t v = (uint32_t)src[0] | (uint32_t)src[1] << 8, r = v >> 11, g = (v >> 5) & 63, b = v & 31;
                out[0] = (uint8_t)(r << 3 | r >> 2); out[1] = (uint8_t)(g << 2 | g >> 4); out[2] = (uint8_t)(b << 3 | b >> 2);
            }
        }
        return;
    }
    uint32_t c = 0;
    for (uint32_t x = 0; x < w; x++, out += 3) {
        switch (shot.depth) {
        case 32: c = (uint32_t)src[4 * x + 1] << 16 | (uint32_t)src[4 * x + 2] << 8 | src[4 * x + 3]; break;      /* 00RRGGBB, big-endian */
        case 16: { uint32_t v = (uint32_t)src[2 * x] << 8 | src[2 * x + 1], r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                   c = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2); break; }
        case 8: c = shot.pal[src[x]]; break;
        case 4: c = shot.pal[(src[x >> 1] >> ((x & 1) ? 0 : 4)) & 15]; break;
        case 2: c = shot.pal[(src[x >> 2] >> (6 - 2 * (x & 3))) & 3]; break;
        default: c = shot.pal[(src[x >> 3] >> (7 - (x & 7))) & 1]; break;
        }
        out[0] = (uint8_t)(c >> 16); out[1] = (uint8_t)(c >> 8); out[2] = (uint8_t)c;
    }
}
