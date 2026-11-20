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
 *
 * Two screens on the SE board (20 November 2026, docs/10 "Two screens on the SE"). On mac.board=se
 * the card's screen used to be the SE's own: its 512 by 342 one-bit frame went row by row into the
 * SE's video RAM (psmac_board_se.c) and HDMI showed the same picture or the log. Now a second card
 * in slot $A carries the HDMI screen (psmb.h, "two screens"; declaration ROMs psmac-se-WxH), so the
 * Mac sees two screens, and this file keeps one display block per screen (struct screen below):
 * screen 0 is the SE's, fed to the SE's video RAM and to nothing else; screen 1 is the HDMI screen,
 * any size up to 1920 by 1200 at any depth, and is what the VideoCore shows. Both ticks come from
 * the one 60 Hz clock and both raise slot 9's interrupt line; each driver claims its own block's
 * tick and runs its own slot's VBL queue (9 and $A). mac.hdmi=mirror and =log keep one screen: the
 * kernel leaves slot $A empty (psmac_bus.c) and HDMI shows the SE's picture, or the log.
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
extern void psmac_hdmi_size(uint32_t *w, uint32_t *h);
extern int psmac_board_se, psmac_board_solo;

#define KERNEL_ALIAS 0xffffff9000000000ull

/* one screen of the card: its display block, its part of the window, its colour table */
struct screen {
    volatile struct psmb_display *disp;
    uint8_t *win;                            /* its frame buffer in the window, through the kernel's alias ... */
    uint32_t win_mac, win_size;              /* ... and as the Mac addresses it ($F9980000 or $F9988000), its size */
    uint32_t clut_seen;
    uint32_t rgb[256];                       /* the colour table as XRGB */
};
static struct screen scr[2];
static int nscr;                             /* screens on the card: 1, or 2 on the SE board with its HDMI screen */
static int se_card;                          /* the SE card's layout (psmac-se-*): screen 0 is the SE's, one bit, 32 KiB */
static struct screen *vcs;                   /* the screen the VideoCore shows (0 with mac.hdmi=log) */
static uint8_t *frame_big;                   /* the big region, through the kernel's alias */
static uint8_t *vcfb;                        /* the VideoCore frame buffer, through the alias too (the low identity
                                                mapping the splash drew through does not survive the Mac MMU shim's
                                                table switches, docs/08 item 21) */
static uint32_t vc_pitch, vc_w, vc_h, vc_bpp = 32;
static int vc_swap;                          /* write the bytes R,G,B,x rather than B,G,R,x (see vc_byte_order) */
extern uint32_t psmac_vc_pixel_order;        /* support_rpi.c: the firmware's answer to "get pixel order" */
extern int psmac_bootarg_val(const char *key, char *out, unsigned max);   /* alice_net.c */
static void vc_byte_order(void);   /* the VideoCore buffer's depth: 32 asked for, 16 if that is what came back */
static uint64_t next_tick;
static int con_keep;                          /* the HDMI console (below): mac.console=hdmi keeps the log on the screen */
static int con_off;                           /* the picture has the buffer */
static uint32_t con_x, con_y, con_cols, con_rows;
static unsigned tick_div;
static int vc_fresh;                          /* the buffer was made moments ago and nothing has drawn the picture in it */
static int vc_boot;                           /* the firmware's boot buffer is kept (the SE board's log on HDMI, psmac_display_init) */

/* psmac_bus.c, once the declaration ROM is in place and before the mailbox is made: the SE card's
   layout (a psmac-se file: slot 9's screen is the SE's, one bit), and whether the HDMI screen's card
   in slot $A is there, making it the Mac's second screen */
void psmac_display_layout(int se, int two) { se_card = se; nscr = two ? 2 : 1; }
int psmac_display_count(void) { return nscr ? nscr : 1; }

static int mode_index(uint32_t depth)
{
    return depth == 1 ? 0 : depth == 2 ? 1 : depth == 4 ? 2 : depth == 8 ? 3 : depth == 16 ? 4 : depth == 32 ? 5 : -1;
}
/* the Mac's pixels of mode 128+mi: the screen's part of the window, or the big region */
static const uint8_t *mode_frame(const struct screen *s, int mi) { return s->disp->mode_base[mi] == PSMB_FB_BIG_PHYS ? frame_big : s->win; }
static uint32_t mode_limit(const struct screen *s, int mi) { return s->disp->mode_base[mi] == PSMB_FB_BIG_PHYS ? PSMB_FB_BIG_SIZE : s->win_size; }

/* A screen's display block: its size, the six modes' places and pitches, a colour table for a card
   just switched on. tools/mac/mkdeclrom.py applies the same placement rule to each mode's
   vpBaseOffset, because the ROM takes the address from there and never asks the driver (docs/10,
   "the third trap"); change one and change the other. A mode of at most 8 bits that fits the
   screen's part of the window stays there (reachable in 24-bit mode too), anything larger goes to
   the big region; the SE's screen has only its one bit. */
static void screen_init(struct screen *s, uint8_t *win, uint32_t block_off, uint32_t frame_off, uint32_t frame_size,
                        uint32_t w, uint32_t h, int one_bit_only)
{
    volatile struct psmb_display *d = (volatile struct psmb_display *)(win + block_off);
    s->disp = d; s->win = win + frame_off; s->win_size = frame_size;
    s->win_mac = PSMB_WIN_PHYS + 0x900000u + frame_off;                 /* $F99xxxxx: the 24-bit megabyte in 32-bit form */
    s->clut_seen = ~0u;
    for (uint32_t i = 0; i < sizeof(struct psmb_display) / 4; i++) ((volatile uint32_t *)d)[i] = 0;
    d->width = w; d->height = h; d->depth = 1;
    d->fb_off = frame_off; d->fb_size = frame_size;
    d->big_base = PSMB_FB_BIG_PHYS; d->big_size = PSMB_FB_BIG_SIZE;
    d->vc_width = vc_w; d->vc_height = vc_h; d->vc_depth = vc_bpp;
    for (int i = 0; i < 6; i++) {
        uint32_t dep = 1u << i, rb = (w * dep + 7) / 8;
        uint32_t bytes = rb * h;
        if (one_bit_only && i) d->mode_base[i] = 0;
        else if (dep <= 8 && bytes <= frame_size) d->mode_base[i] = s->win_mac;
        else if (!one_bit_only && bytes <= PSMB_FB_BIG_SIZE) d->mode_base[i] = PSMB_FB_BIG_PHYS;   /* $90000000: 32-bit addressing only */
        else d->mode_base[i] = 0;                                         /* no such mode at this size */
        d->mode_rowbytes[i] = rb;
    }
    d->rowbytes = d->mode_rowbytes[0];
    for (int i = 0; i < 256; i++) { uint32_t g = 255 - i; d->clut[i] = g << 16 | g << 8 | g; }   /* 0 white ... 255 black, the Mac's order */
    for (uint32_t i = 0; i < frame_size; i += 8) *(uint64_t *)(s->win + i) = 0;
    __asm__ volatile("dmb ish" ::: "memory");
    d->version = 2; d->magic = PSMB_DISP_MAGIC;
}

void psmac_display_init(uint8_t *win)
{
    if (!nscr) nscr = 1;
    frame_big = (uint8_t *)(KERNEL_ALIAS + psmac_fb_big_phys());
    uint32_t w0, h0, w1 = 0, h1 = 0;
    psmac_screen_size(&w0, &h0);                 /* the first screen: mac.screen, or 512 x 342 on the SE board */
    if (!w0 || !h0) { w0 = fb_width ? fb_width : 640; h0 = fb_height ? fb_height : 480; }
    if (se_card) { w0 = 512; h0 = 342; }
    /* The VideoCore's size: the screen it shows. On the SE board that is the HDMI screen when there
       are two, the log at the HDMI size with mac.hdmi=log, and the SE's picture (the firmware scales
       it up) with mac.hdmi=mirror, as before. */
    uint32_t vw = w0, vh = h0;
    if (psmac_board_se && !psmac_board_solo && (nscr == 2 || con_keep)) {
        psmac_hdmi_size(&w1, &h1);
        if (!w1 || !h1) { w1 = fb_width ? fb_width : 640; h1 = fb_height ? fb_height : 480; }
        vw = w1; vh = h1;
    }
    /* With the log on HDMI and no second screen (mac.hdmi=log, which mac.console=hdmi means) the
       VideoCore shows nothing but text, so the buffer the firmware made at boot for the splash is
       kept, 16 bits as it is, rather than remade. On 20 November 2026 the second boot on the SE
       (kernel c6d203f, HDMI mode 1024 x 768) asked the firmware for a 32-bit buffer of that same
       size, got one at 0x3E8FA000 that overlapped the boot buffer's top half, drew the log into
       it, and the HDMI stayed black from then on; the kernel before (5ff7f89) asked for 512 x 342
       there and its log was on the screen. Why the firmware did not show the new buffer is not
       known (QEMU's model of the VideoCore shows either); the boot buffer is the one it is known
       to scan out, since the splash and the first lines of the log are drawn into it on every Pi.
       The console draws 16-bit cells there (psmac_console_putc), and a Restart's RESET leaves it
       alone (psmac_display_reinit). Every other case remakes the buffer as before. */
    if (psmac_board_se && !psmac_board_solo && con_keep && nscr != 2 && framebuffer && pitch && fb_width && fb_height) {
        vc_boot = 1;
        vc_bpp = pitch >= fb_width * 4 ? 32 : 16;
    } else {
        /* the VideoCore buffer at that size, 32 bits */
        void *fb = 0; uint32_t p = 0;
        uint32_t got = psmac_vc_setup(vw, vh, 32, &fb, &p);
        if (got != 32 || p < vw * 4) {          /* not granted (QEMU's model stays at 16): ask for 16 and convert to RGB565 */
            got = psmac_vc_setup(vw, vh, 16, &fb, &p);
            vc_bpp = 16;
        }
        if (fb && p) { framebuffer = fb; pitch = p; fb_width = vw; fb_height = vh; }
    }
    vc_w = fb_width; vc_h = fb_height; vc_pitch = pitch;
    vc_byte_order();
    if (framebuffer) vcfb = (uint8_t *)(KERNEL_ALIAS + (((uintptr_t)framebuffer) & 0x3fffffffull));
    if (vcfb && !vc_boot) {
        for (uint32_t i = 0; i < vc_pitch * vc_h; i += 8) *(uint64_t *)(vcfb + i) = 0;
        con_cols = 0; con_x = 0; con_y = 0;                    /* the console starts again in the new buffer */
    }
    vc_fresh = 1;

    if (se_card) screen_init(&scr[0], win, PSMB_OFF_DISPLAY, PSMB_OFF_BOUNCE, PSMB_SE_FRAME_SIZE, w0, h0, 1);
    else screen_init(&scr[0], win, PSMB_OFF_DISPLAY, PSMB_OFF_BOUNCE, PSMB_BOUNCE_SIZE, w0, h0, 0);
    if (nscr == 2) screen_init(&scr[1], win, PSMB_OFF_DISPLAY2, PSMB_OFF_BOUNCE2, PSMB_BOUNCE2_SIZE, w1, h1, 0);
    vcs = nscr == 2 ? &scr[1] : &scr[0];
    if (nscr == 2)
        kprintf("[PSMAC:DISP] two screens: the SE's 512x342, 1 bit, at $%08x (its own video RAM); HDMI %ux%u: indexed modes that fit at $%08x, the rest in the big region at $%08x (%u MiB); VideoCore %ux%u x%u pitch %u at %p\n",
                scr[0].win_mac, w1, h1, scr[1].win_mac, PSMB_FB_BIG_PHYS, PSMB_FB_BIG_SIZE >> 20, vc_w, vc_h, vc_bpp, vc_pitch, framebuffer);
    else
        kprintf("[PSMAC:DISP] %s %ux%u: indexed modes in the window at $%08x%s; VideoCore %ux%u x%u pitch %u at %p%s\n",
                psmac_board_se && !psmac_board_solo ? "the SE's screen" : "second display", w0, h0, scr[0].win_mac,
                se_card ? ", one bit only (the SE card)" : ", direct modes in the big region at $90000000",
                vc_w, vc_h, vc_bpp, vc_pitch, framebuffer, vc_boot ? ": the firmware's own buffer, kept; the log stays there (mac.hdmi=log)" : con_keep ? ": the log stays there (mac.hdmi=log)" : psmac_board_se && !psmac_board_solo ? ": HDMI mirrors the SE (mac.hdmi=mirror)" : "");
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
    if (vc_boot) return;                     /* the firmware's boot buffer, kept for the log (psmac_display_init) */
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


/* The picture, a band at a time. A whole 1920 by 1200 frame in millions of colours is 9 MB read,
   9 MB written and 9 MB flushed, some 20 ms of a Pi 3's core on every other 60 Hz tick, and the
   service loop runs nothing else meanwhile: on 19 November the user's Pi 3B at 1920 by 1200 pinged
   its router over the WiFi in 80 ms, because the WiFi, the network board's interrupt and the disk
   all waited behind the conversion (docs/08 item 49). Now each turn of the loop converts a band of
   about 128 K pixels (a millisecond or so) and the next turn the next band; a frame starts at most
   30 times a second, as before, and takes as many turns as it needs. Small screens are one band.
   Only the screen the VideoCore shows is converted (vcs). */
#define BAND_PIXELS 131072u
static uint32_t band_y, band_h, band_w, band_mode, band_active, band_depth, band_rb;
static uint64_t frame_t0;
/* the Mac's pixels -> XRGB; depth 1/2/4: the leftmost pixel is in the high bits of the byte */
static void convert_rows(uint32_t y0, uint32_t y1)
{
    uint32_t depth = band_depth, rb = band_rb, w = band_w;
    const uint8_t *frame = mode_frame(vcs, (int)band_mode);
    const uint32_t *rgb = vcs->rgb;
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
/* a screen's colour table as XRGB, when the Mac has changed it (or its luminance mapping) */
static void clut_update(struct screen *s)
{
    volatile struct psmb_display *d = s->disp;
    if (d->clut_seq == s->clut_seen) return;
    s->clut_seen = d->clut_seq;
    for (int i = 0; i < 256; i++) {
        uint32_t c = d->clut[i] & 0xffffffu;
        if (d->flags & PSMB_DISP_GRAY) { uint32_t y = (((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 151 + (c & 255) * 28) >> 8; c = y << 16 | y << 8 | y; }
        s->rgb[i] = c;
    }
}
/* a new frame: what to convert (0 if nothing can be). The depth and pitch are taken once per frame,
   so a mode change half way through cannot mix two layouts in one picture. */
static int convert_start(void)
{
    if (!vcfb) return 0;
    volatile struct psmb_display *d = vcs->disp;
    uint32_t depth = d->depth, rb = d->rowbytes;
    uint32_t h = d->height < vc_h ? d->height : vc_h, w = d->width < vc_w ? d->width : vc_w;
    int mi = mode_index(depth);
    if (mi < 0 || !d->mode_base[mi]) return 0;
    if (rb * h > mode_limit(vcs, mi)) return 0;
    clut_update(vcs);
    band_y = 0; band_h = h; band_w = w; band_mode = (uint32_t)mi; band_depth = depth; band_rb = rb;
    return 1;
}
/* one band; the frame's end counts it */
static void convert_band(void)
{
    uint32_t n = band_w ? BAND_PIXELS / band_w : band_h;
    if (!n) n = 1;
    if (band_y + n > band_h) n = band_h - band_y;
    convert_rows(band_y, band_y + n);
    band_y += n;
    if (band_y < band_h) return;
    band_active = 0;
    vcs->disp->frames++;
}
/* the SE's own screen (psmac_board_se.c compares the rows and sends what changed): screen 0's
   frame, whatever the VideoCore shows. Until 20 November it was called at the end of each HDMI
   conversion, so with mac.console=hdmi, which stops the conversion, the SE's screen never changed. */
static void se_frame(void)
{
    extern void psmac_se_mark(const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);
    volatile struct psmb_display *d = scr[0].disp;
    int mi = mode_index(d->depth);
    if (mi < 0 || !d->mode_base[mi] || !(d->flags & PSMB_DISP_ENABLED)) return;
    psmac_se_mark(mode_frame(&scr[0], mi), d->width, d->height, d->rowbytes, d->depth);
    if (vcs != &scr[0] || con_keep) d->frames++;
}

/* from the service core's loop, every turn: the 60 Hz tick (VBL), and a band of the picture */
void psmac_display_poll(void)
{
    if (!vcs) return;
    uint64_t now; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(now));
    uint64_t hz; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(hz));
    if (!next_tick) { next_tick = now + hz / 60; return; }
    volatile struct psmb_display *vd = vcs->disp;
    if (band_active) {
        if ((vd->flags & PSMB_DISP_ENABLED) && !con_keep) convert_band();
        else band_active = 0;
    }
    if ((int64_t)(now - next_tick) < 0) return;
    next_tick += hz / 60; if ((int64_t)(now - next_tick) > (int64_t)hz) next_tick = now + hz / 60;
    /* each screen's VBL: its driver's handler takes it and runs its own slot's VBL queue (QuickDraw's
       cursor, SlotVInstall's tasks) with _DoVBLTask */
    for (int i = 0; i < nscr; i++) {
        volatile struct psmb_display *d = scr[i].disp;
        d->vbl_count++;
        if ((d->flags & PSMB_DISP_ENABLED) && d->vbl_enable) { d->vbl_pending = 1; __asm__ volatile("dmb ish" ::: "memory"); }
    }
    if (!(++tick_div & 1)) return;                                 /* a new frame every other tick: 30 a second */
    if (psmac_board_se && !psmac_board_solo) se_frame();
    if ((vd->flags & PSMB_DISP_ENABLED) && !con_keep) {
        con_off = 1;
        if (!band_active && now - frame_t0 >= hz / 31 && convert_start()) {   /* if the last one is done */
            frame_t0 = now;
            band_active = 1;
            convert_band();
        }
    }
}

/* for the heartbeat */
void psmac_display_report(void)
{
    for (int i = 0; vcs && i < nscr; i++) {
        volatile struct psmb_display *d = scr[i].disp;
        kprintf("[PSMAC]   display%s: %ux%u flags %x depth %u rowbytes %u vbl %s pending %u ticks %u frames %u clut seq %u\n",
                nscr == 1 ? "" : i ? " 1 (HDMI)" : " 0 (SE)", d->width, d->height, d->flags, d->depth, d->rowbytes, d->vbl_enable ? "on" : "off",
                d->vbl_pending, d->vbl_count, d->frames, d->clut_seq);
    }
}

/* The 68k was reset: the video drivers' VBL interrupts are off until they open again, and so is the
   display, whose flag a driver sets only once its interrupt handler is in. Until 20 November the flag
   outlived the reset: the HDMI driver's Open, which turns VBLs on before it installs its handler
   (and grey-fills 288 KB of a 1920 by 1200 screen in between), had its first tick raise slot 9's
   line with nobody to take it but the input driver's catch-all, which cannot lower it, and the
   restarted Mac spent the rest of its life in its level-2 interrupt (PiScrn's restart, docs/10). */
void psmac_display_bus_reset(void)
{
    for (int i = 0; vcs && i < nscr; i++) { scr[i].disp->vbl_enable = 0; scr[i].disp->vbl_pending = 0; scr[i].disp->flags = 0; }
    __asm__ volatile("dmb ish" ::: "memory");
}
/* the slot interrupt line's share: a screen's VBL is pending and its driver is there to take it */
int psmac_display_irq(void)
{
    for (int i = 0; vcs && i < nscr; i++) {
        volatile struct psmb_display *d = scr[i].disp;
        if ((d->flags & PSMB_DISP_ENABLED) && d->vbl_enable && d->vbl_pending) return 1;
    }
    return 0;
}

/* screen i as the Mac uses it, for the remote (psmac_remote.c): its size, depth and the 68k address
   of its pixels in the current mode, which is the baseAddr of its GDevice's pixel map; 0 if there
   is no such screen or its driver is not open */
int psmac_display_info(int i, uint32_t *w, uint32_t *h, uint32_t *depth, uint32_t *base)
{
    if (!vcs || i < 0 || i >= nscr) return 0;
    volatile struct psmb_display *d = scr[i].disp;
    int mi = mode_index(d->depth);
    if (!(d->flags & PSMB_DISP_ENABLED) || mi < 0) return 0;
    *w = d->width; *h = d->height; *depth = d->depth; *base = d->mode_base[mi];
    return 1;
}

/* ---- the remote's screenshot (psmac_remote.c, docs/13) ---------------------------------------
   The picture as the Mac drew it, not as the VideoCore scans it out: the frame buffer of the mode
   in use, at its depth, through the colour table, so a screenshot is right whatever the HDMI side
   does (mac.console=hdmi, a 16-bit scan-out, swapped bytes). Until the video driver has switched
   the display on there is no Mac picture, and the VideoCore buffer is the screen (the boot log on
   the console). The parameters are taken once per screenshot, so a depth change half way through
   cannot mix two layouts; the pixels are read while the Mac goes on drawing, as a card's frame
   grabber would. display is the screen: 0 the first (the SE's on the SE board), 1 the HDMI screen
   when there are two. */
static struct { int src; uint32_t w, h, depth, rb; const uint8_t *base; uint32_t pal[256]; } shot;
int psmac_display_shot(uint32_t *w, uint32_t *h, uint32_t *depth, int display, int want_hdmi)
{
    shot.src = 0;
    if (!vcs || display < 0 || display >= nscr) return 0;
    struct screen *s = &scr[display];
    volatile struct psmb_display *d = s->disp;
    int mi = mode_index(d->depth);
    if (!want_hdmi && (d->flags & PSMB_DISP_ENABLED) && mi >= 0 && d->mode_base[mi]) {
        shot.w = d->width; shot.h = d->height; shot.depth = d->depth; shot.rb = d->rowbytes;
        shot.base = mode_frame(s, mi);
        if (shot.rb * shot.h > mode_limit(s, mi) || shot.rb < (shot.w * shot.depth + 7) / 8) return 0;
        for (int i = 0; i < 256; i++) {
            uint32_t c = d->clut[i] & 0xffffffu;
            if (d->flags & PSMB_DISP_GRAY) { uint32_t y = (((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 151 + (c & 255) * 28) >> 8; c = y << 16 | y << 8 | y; }
            shot.pal[i] = c;
        }
        shot.src = 1;
    } else if (vcfb && vc_w && vc_h && (want_hdmi || s == vcs)) {
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
