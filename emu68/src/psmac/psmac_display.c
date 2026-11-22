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
 * super slot space (8 MiB at $F9100000 until 1920 x 1200 at 32 bits needed more, docs/10; 15 MiB
 * when the screen is 2560 x 1440, psmb.h), which 32-bit systems use. The block's mode tables say which region each mode uses; the driver never
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
 * any size up to 2560 by 1440 at any depth, and is what the VideoCore shows. Both ticks come from
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
extern uint32_t psmac_fb_big_size(void);
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
    uint32_t block_off, frame_off;           /* where screen_init put it (psmac_display_rotate makes it again) */
    int one_bit;
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
extern uint32_t psmac_vc_alpha;              /* support_rpi.c: the answer to "set alpha mode" */
extern int psmac_vc_query(uint32_t out[8]);
/* What the firmware's frame buffer is after the set-up, read back with a message that changes
   nothing, so that a photograph of the log shows what the HVS scans out, not only what was asked.
   On 20 November 2026 the SE's HDMI stayed black with a buffer the request's answer called good
   (docs/08 item 65); if that happens again this line says whether the firmware agrees. */
static void vc_report(uint32_t want_w, uint32_t want_h)
{
    uint32_t q[8];
    if (!psmac_vc_query(q)) { kprintf("[PSMAC:DISP] VideoCore: the firmware did not answer the read-back of its frame buffer\n"); return; }
    int ok = q[0] == want_w && q[1] == want_h && q[2] == want_w && q[3] == want_h && q[6] == 0 && (!pitch || q[5] == pitch);
    kprintf("[PSMAC:DISP] VideoCore read back: physical %ux%u, virtual %ux%u, depth %u, pitch %u, offset %u,%u, pixel order %s%s\n",
            q[0], q[1], q[2], q[3], q[4], q[5], q[6] & 0xffff, q[6] >> 16, q[7] & 1 ? "RGB" : "BGR",
            ok ? ": as asked" : ": NOT what was asked (the picture may not be on the monitor; mac.vcdepth=16 keeps the firmware's own buffer)");
}
static uint64_t next_tick;
static int con_keep;                          /* the HDMI console (below): mac.console=hdmi keeps the log on the screen */
static int con_off;                           /* the picture has the buffer */
static uint32_t con_x, con_y, con_cols, con_rows;
static unsigned tick_div;
static int vc_fresh;                          /* the buffer was made moments ago and nothing has drawn the picture in it */
static int vc_boot;                           /* the firmware's boot buffer is kept (the SE board's log on HDMI, psmac_display_init) */
static uint8_t *win_base;                     /* the window, for psmac_display_rotate */
static int rot;                               /* the picture turned this far clockwise on the monitor (PiDisplay, below) */
static uint32_t hdmi_w, hdmi_h;               /* the firmware's frame buffer at start: the HDMI mode */

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
static uint32_t mode_limit(const struct screen *s, int mi) { return s->disp->mode_base[mi] == PSMB_FB_BIG_PHYS ? psmac_fb_big_size() : s->win_size; }

/* A screen's display block: its size, the six modes' places and pitches, a colour table for a card
   just switched on. tools/mac/mkdeclrom.py applies the same placement rule to each mode's
   vpBaseOffset, because the ROM takes the address from there and never asks the driver (docs/10,
   "the third trap"); change one and change the other. A mode of at most 8 bits that fits the
   screen's part of the window stays there (reachable in 24-bit mode too), anything larger goes to
   the big region; the SE's screen has only its one bit. */
/* Where a mode's pixels go, as the Mac addresses them: the screen's part of the window (win_mac,
   win_size) for a mode of at most 8 bits that fits there, else the big region; 0 when neither holds
   it, or its rows are wider than QuickDraw takes. psmac_bus.c rewrites the declaration ROM's modes by
   this very function (declrom_resize), so the two cannot disagree. */
uint32_t psmac_display_mode_base(uint32_t w, uint32_t h, uint32_t depth, uint32_t win_mac, uint32_t win_size, int one_bit_only)
{
    uint32_t rb = psmb_rowbytes(w, depth), bytes = rb * h;
    if (rb > PSMB_ROWBYTES_MAX || (one_bit_only && depth > 1)) return 0;
    if (depth <= 8 && bytes <= win_size) return win_mac;
    if (!one_bit_only && bytes <= psmac_fb_big_size()) return PSMB_FB_BIG_PHYS;   /* $90000000: 32-bit addressing only */
    return 0;
}
static void screen_init(struct screen *s, uint8_t *win, uint32_t block_off, uint32_t frame_off, uint32_t frame_size,
                        uint32_t w, uint32_t h, int one_bit_only)
{
    volatile struct psmb_display *d = (volatile struct psmb_display *)(win + block_off);
    s->disp = d; s->win = win + frame_off; s->win_size = frame_size;
    s->block_off = block_off; s->frame_off = frame_off; s->one_bit = one_bit_only;
    s->win_mac = PSMB_WIN_PHYS + 0x900000u + frame_off;                 /* $F99xxxxx: the 24-bit megabyte in 32-bit form */
    s->clut_seen = ~0u;
    for (uint32_t i = 0; i < sizeof(struct psmb_display) / 4; i++) ((volatile uint32_t *)d)[i] = 0;
    d->width = w; d->height = h; d->depth = 1;
    d->fb_off = frame_off; d->fb_size = frame_size;
    uint32_t big = psmac_fb_big_size();
    d->big_base = PSMB_FB_BIG_PHYS; d->big_size = big;
    d->vc_width = vc_w; d->vc_height = vc_h; d->vc_depth = vc_bpp;
    for (int i = 0; i < 6; i++) {
        uint32_t dep = 1u << i, rb = psmb_rowbytes(w, dep);
        d->mode_base[i] = psmac_display_mode_base(w, h, dep, s->win_mac, frame_size, one_bit_only);
        d->mode_rowbytes[i] = rb;
        /* no such mode at this size: the declaration ROM leaves it out too (declrom_resize) */
        if (!d->mode_base[i] && !one_bit_only)
            kprintf("[PSMAC:DISP] %ux%u at %u bits: %u bytes a row, %u KiB a frame, the big region %u KiB: that depth is left out\n",
                    w, h, dep, rb, rb * h >> 10, big >> 10);
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
    win_base = win;
    hdmi_w = fb_width; hdmi_h = fb_height;
    { extern int psmac_hdmi_rotation(void); rot = psmac_hdmi_rotation(); }
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
    /* PiDisplay's rotation (psmac_hdmi.c): at 90 and 270 the Mac's screen is the VideoCore's turned on
       its side, H x W (psmac_bus.c swapped the size), and the VideoCore keeps the HDMI mode's W x H.
       The SE's own screen is never turned; with mac.hdmi=mirror or log there is nothing to turn. */
    int can_turn = nscr == 2 || (!se_card && !(psmac_board_se && !psmac_board_solo));
    if (!can_turn) rot = 0;
    if (rot == 90 || rot == 270) { uint32_t t = vw; vw = vh; vh = t; }
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
    /* The HDMI screen of the SE board's two screens (20 November 2026, docs/08 item 73) takes the
       same buffer when it is already the screen's size: the firmware's own, which the splash and the
       first lines of the log prove on the monitor, converted into at 16 bits (RGB565: 65,536 colours
       on the monitor whatever the Mac draws in, millions included). mac.vcdepth=32 asks the firmware
       for a 32-bit buffer instead, by psmac_vc_setup's sequence; so does a mac.screen= of another
       size, which needs a new buffer whatever its depth. */
    char vd[4] = { 0 };
    int want32 = psmac_bootarg_val("mac.vcdepth", vd, sizeof vd) && vd[0] == '3';
    int keep_boot = psmac_board_se && !psmac_board_solo && framebuffer && pitch && fb_width && fb_height &&
                    ((con_keep && nscr != 2) || (nscr == 2 && !want32 && fb_width == vw && fb_height == vh));
    if (keep_boot) {
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
        if (psmac_vc_alpha != ~0u && psmac_vc_alpha != 2)
            kprintf("[PSMAC:DISP] VideoCore: \"alpha ignored\" answered %u (the pixels' top byte is 0)\n", psmac_vc_alpha);
    }
    vc_report(vw, vh);
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
        kprintf("[PSMAC:DISP] two screens: the SE's 512x342, 1 bit, at $%08x (its own video RAM); HDMI %ux%u: indexed modes that fit at $%08x, the rest in the big region at $%08x (%u MiB); VideoCore %ux%u x%u pitch %u at %p%s\n",
                scr[0].win_mac, w1, h1, scr[1].win_mac, PSMB_FB_BIG_PHYS, psmac_fb_big_size() >> 20, vc_w, vc_h, vc_bpp, vc_pitch, framebuffer,
                vc_boot ? ": the firmware's own buffer, kept (mac.vcdepth=32 asks for a new 32-bit one)" : "");
    else
        kprintf("[PSMAC:DISP] %s %ux%u: indexed modes in the window at $%08x%s; VideoCore %ux%u x%u pitch %u at %p%s\n",
                psmac_board_se && !psmac_board_solo ? "the SE's screen" : "second display", w0, h0, scr[0].win_mac,
                se_card ? ", one bit only (the SE card)" : ", direct modes in the big region at $90000000",
                vc_w, vc_h, vc_bpp, vc_pitch, framebuffer, vc_boot ? ": the firmware's own buffer, kept; the log stays there (mac.hdmi=log)" : con_keep ? ": the log stays there (mac.hdmi=log)" : psmac_board_se && !psmac_board_solo ? ": HDMI mirrors the SE (mac.hdmi=mirror)" : "");
    /* the HDMI monitor's EDID and the mode in force, for PiDisplay (psmac_hdmi.c): the firmware's buffer as it
       was at the start is the HDMI mode */
    { extern void psmac_hdmi_init(uint32_t, uint32_t); psmac_hdmi_init(hdmi_w, hdmi_h); }
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
/* The bus check's report on HDMI (psmac_buscheck.c, 22 November 2026): the screen cleared and the report's
   lines drawn from the top, in the console's 8 by 16 cells when they fit and 8 by 8 when not (QEMU's
   640 by 480 has 30 rows of 16), and the console holds from then on, so that the heartbeat does not
   write over it: the log still goes to the serial line, PiLog's ring and the card. A second page
   replaces the first (mac.buscheck=loop). */
static volatile int con_held;
void psmac_console_page(const char (*lines)[65], int n)
{
    uint8_t *fb = con_fb();
    uint32_t p = vc_pitch ? vc_pitch : pitch, w = vc_w ? vc_w : fb_width, h = vc_h ? vc_h : fb_height;
    if (!fb || !p || !w || !h) return;
    int bpp32 = vc_bpp == 32 && vcfb;
    uint32_t bpp = bpp32 ? 4 : 2, ch = h / 16 >= (uint32_t)n + 1 ? 16 : 8, x0 = w / 8 > 64 ? (w / 8 - 64) / 2 : 0;
    con_held = 1;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = fb + y * p;
        for (uint32_t x = 0; x < w * bpp; x++) row[x] = 0;
        arm_flush_cache((intptr_t)row, w * bpp);
    }
    for (int r = 0; r < n && (uint32_t)(r + 1) * ch <= h; r++) {
        int ended = 0;
        for (uint32_t col = 0; col < 64 && (x0 + col + 1) * 8 <= w; col++) {
            uint8_t c = ended ? 0 : (uint8_t)lines[r][col];
            if (!c) { ended = 1; continue; }
            const uint8_t *data = &topaz8_chardata[(topaz8_charloc[(c >= 32 && c < 128) ? c - 32 : 0] >> 16) >> 3];
            for (uint32_t y = 0; y < ch; y++) {
                uint8_t *row = fb + (r * ch + y) * p + (x0 + col) * 8 * bpp;
                uint8_t bits = c > 32 ? data[(ch == 16 ? y / 2 : y) * modulo] : 0;
                for (uint32_t x = 0; x < 8; x++) {
                    int on = bits & (0x80 >> x);
                    if (bpp32) ((uint32_t *)row)[x] = on ? LE32(0x00e0e0e0u) : 0; else ((uint16_t *)row)[x] = on ? LE16(0xe71c) : 0;
                }
                arm_flush_cache((intptr_t)row, 8 * bpp);
            }
        }
    }
}
void psmac_console_putc(uint8_t c)
{
    if (con_held) return;                                     /* the bus check's page (above) */
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
   Only the screen the VideoCore shows is converted (vcs).
   2560 by 1440 (20 November 2026, docs/10) is 3.7 M pixels, 1.6 times 1920 by 1200: 14.7 MB read
   in millions of colours, 7.4 MB written into the firmware's 16-bit buffer (14.7 MB into a 32-bit
   one) and flushed, every frame. Thirty of those a second would keep core 3 at the conversion most of
   the time, and its disk, network and remote work would get what is left between bands. So a frame is
   also kept to its share of the core: the bands' own time is measured (frame_busy), and the next frame
   starts no sooner than that long after this one ended (FRAME_SHARE: at most half of core 3, on
   average). A frame that takes less than 1/60 s changes nothing: 30 frames a second (1080p and
   below; 1920 by 1200 in millions is about 1/60 s). Larger screens get as many as half the core converts; the heartbeat's display line says
   how long a frame took and how many there were. The two common pairs (millions into 16 or 32 bits)
   have loops of their own, without the per-pixel choices of put(). */
#define BAND_PIXELS 131072u
#define FRAME_SHARE 1u                    /* idle time after a frame, in multiples of its busy time: 1 is half the core */
static uint32_t band_y, band_h, band_w, band_mode, band_active, band_depth, band_rb, band_rot;
static uint64_t frame_t0, frame_end, frame_busy, frame_rest;
static uint32_t frame_bands, rep_busy_max, rep_frames, last_busy_max;   /* the last frame's bands; the heartbeat's figures */
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
        case 32: {                                                   /* 00RRGGBB, big-endian longs read natively */
            const uint32_t *s = (const uint32_t *)src;
            if (vc_bpp == 16) {                                      /* the firmware's buffer: RGB565 */
                uint16_t *d = (uint16_t *)dst;
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t v = s[x];
                    d[x] = LE16((uint16_t)(((v >> 8) & 0xf800) | ((v >> 5) & 0x07e0) | ((v >> 3) & 0x001f)));
                }
            } else if (!vc_swap) {
                uint32_t *d = (uint32_t *)dst;
                for (uint32_t x = 0; x < w; x++)
                    d[x] = LE32(s[x] & 0xffffffu);
            } else {
                for (uint32_t x = 0; x < w; x++)
                    put(dst, x, s[x] & 0xffffffu);
            }
            break;
        }
        case 16: {                                                   /* 0RRRRRGGGGGBBBBB */
            const uint16_t *s = (const uint16_t *)src;
            if (vc_bpp == 16) {                                      /* RGB565: green widened from 5 bits to 6 */
                uint16_t *d = (uint16_t *)dst;
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t v = s[x], g = (v >> 5) & 31;
                    d[x] = LE16((uint16_t)((v & 0x7c00) << 1 | (g << 1 | g >> 4) << 5 | (v & 31)));
                }
                break;
            }
            for (uint32_t x = 0; x < w; x++) {
                uint32_t v = s[x], r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                put(dst, x, (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2));
            }
            break;
        }
        case 8: for (uint32_t x = 0; x < w; x++) put(dst, x, rgb[src[x]]); break;
        /* 1, 2 and 4 bits: a byte holds 8, 4 or 2 pixels, and a width that is not a multiple of that
           (1366) ends inside a byte, whose last pixels are not the screen's: kmax stops there rather
           than write past the row (and, on the last row, past the VideoCore buffer) */
        case 4: for (uint32_t x = 0; x < w; x += 2) { uint8_t b = src[x >> 1]; put(dst, x, rgb[b >> 4]); if (x + 1 < w) put(dst, x + 1, rgb[b & 15]); } break;
        case 2: for (uint32_t x = 0; x < w; x += 4) {
                    uint8_t b = src[x >> 2]; uint32_t kmax = w - x < 4 ? w - x : 4;
                    for (uint32_t k = 0; k < kmax; k++)
                        put(dst, x + k, rgb[(b >> (6 - 2 * k)) & 3]);
                }
                break;
        case 1: for (uint32_t x = 0; x < w; x += 8) {
                    uint8_t b = src[x >> 3]; uint32_t kmax = w - x < 8 ? w - x : 8;
                    for (uint32_t k = 0; k < kmax; k++)
                        put(dst, x + k, rgb[(b >> (7 - k)) & 1]);
                }
                break;
        }
    }
    arm_flush_cache((intptr_t)(vcfb + y0 * vc_pitch), vc_pitch * (y1 - y0));
}
/* The picture turned (PiDisplay's rotation, psmac_hdmi.c, 21 November 2026). rot is how far the picture
   is turned clockwise on the monitor: at 90 the Mac's top row is the monitor's right-hand column, at
   270 its left-hand one, at 180 the Mac is upside down; a monitor stood on its side the other way round
   then shows the Mac upright. The Mac's pixel (x, y) goes to
       90:  (vc_w - 1 - y, x)       180: (vc_w - 1 - x, vc_h - 1 - y)       270: (y, vc_h - 1 - x)
   A turn of 90 or 270 writes the Mac's rows as the monitor's columns, which is what caches dislike: a
   row of 1440 pixels lands one pixel on each of 1440 lines of the VideoCore buffer. So the band is
   done in tiles: TURN_TILE columns of the Mac's band rows at a time (for millions of colours 32 x 4
   bytes = two cache lines of each source row, the whole tile a few KB), each column written as one
   short run along a VideoCore row and flushed as such. The common pairs (millions into the firmware's
   16-bit buffer or a 32-bit one) have loops of their own; the rest go through src_pixel() and put(). */
#define TURN_TILE 32u
static inline uint32_t src_pixel(const uint8_t *row, uint32_t x, uint32_t depth, const uint32_t *rgb)
{
    switch (depth) {
    case 32: return ((const uint32_t *)row)[x] & 0xffffffu;
    case 16: { uint32_t v = ((const uint16_t *)row)[x], r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
               return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2); }
    case 8: return rgb[row[x]];
    case 4: return rgb[(row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15];
    case 2: return rgb[(row[x >> 2] >> (6 - 2 * (x & 3))) & 3];
    default: return rgb[(row[x >> 3] >> (7 - (x & 7))) & 1];
    }
}
static void convert_turned(uint32_t y0, uint32_t y1)
{
    uint32_t depth = band_depth, rb = band_rb, w = band_w, r = band_rot;
    const uint8_t *frame = mode_frame(vcs, (int)band_mode);
    const uint32_t *rgb = vcs->rgb;
    uint32_t bpp = vc_bpp == 32 ? 4 : 2, n = y1 - y0;
    if (r == 180) {
        for (uint32_t y = y0; y < y1; y++) {
            const uint8_t *src = frame + y * rb;
            uint8_t *dst = vcfb + (vc_h - 1 - y) * vc_pitch;
            if (depth == 32 && vc_bpp == 16) {
                const uint32_t *sp = (const uint32_t *)src; uint16_t *d = (uint16_t *)dst;
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t v = sp[x];
                    d[vc_w - 1 - x] = LE16((uint16_t)(((v >> 8) & 0xf800) | ((v >> 5) & 0x07e0) | ((v >> 3) & 0x001f)));
                }
            } else {
                for (uint32_t x = 0; x < w; x++)
                    put(dst, vc_w - 1 - x, src_pixel(src, x, depth, rgb));
            }
            arm_flush_cache((intptr_t)(dst + (vc_w - w) * bpp), w * bpp);
        }
        return;
    }
    for (uint32_t x0 = 0; x0 < w; x0 += TURN_TILE) {
        uint32_t x1 = x0 + TURN_TILE < w ? x0 + TURN_TILE : w;
        for (uint32_t x = x0; x < x1; x++) {
            /* the Mac's column x is the VideoCore's row x (90) or vc_h - 1 - x (270); the band's rows
               y0..y1 fill a run of it, right to left at 90, left to right at 270 */
            uint8_t *drow = vcfb + (r == 90 ? x : vc_h - 1 - x) * vc_pitch;
            uint32_t first = r == 90 ? vc_w - y1 : y0;                    /* the run's leftmost pixel */
            const uint8_t *src = frame + y0 * rb;
            if (depth == 32 && vc_bpp == 16) {
                uint16_t *d = (uint16_t *)drow;
                for (uint32_t k = 0; k < n; k++, src += rb) {
                    uint32_t v = ((const uint32_t *)src)[x];
                    d[r == 90 ? vc_w - 1 - (y0 + k) : y0 + k] = LE16((uint16_t)(((v >> 8) & 0xf800) | ((v >> 5) & 0x07e0) | ((v >> 3) & 0x001f)));
                }
            } else if (depth == 32 && vc_bpp == 32 && !vc_swap) {
                uint32_t *d = (uint32_t *)drow;
                for (uint32_t k = 0; k < n; k++, src += rb)
                    d[r == 90 ? vc_w - 1 - (y0 + k) : y0 + k] = LE32(((const uint32_t *)src)[x] & 0xffffffu);
            } else {
                for (uint32_t k = 0; k < n; k++, src += rb)
                    put(drow, r == 90 ? vc_w - 1 - (y0 + k) : y0 + k, src_pixel(src, x, depth, rgb));
            }
            arm_flush_cache((intptr_t)(drow + first * bpp), n * bpp);
        }
    }
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
    uint32_t r = (uint32_t)rot, tw = r == 90 || r == 270 ? vc_h : vc_w, th = r == 90 || r == 270 ? vc_w : vc_h;   /* the VideoCore as the Mac sees it */
    uint32_t h = d->height < th ? d->height : th, w = d->width < tw ? d->width : tw;
    int mi = mode_index(depth);
    if (mi < 0 || !d->mode_base[mi]) return 0;
    if (rb * h > mode_limit(vcs, mi)) return 0;
    clut_update(vcs);
    band_y = 0; band_h = h; band_w = w; band_mode = (uint32_t)mi; band_depth = depth; band_rb = rb; band_rot = r;
    return 1;
}
/* one band; the frame's end counts it */
static uint64_t cnt_now(void) { uint64_t t; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(t)); return t; }
static void convert_band(void)
{
    /* a turned band twice as tall: each of its columns is one run along a VideoCore row, flushed on its own,
       and longer runs mean fewer of them for the same pixels */
    uint32_t n = band_w ? (band_rot == 90 || band_rot == 270 ? 2 * BAND_PIXELS : BAND_PIXELS) / band_w : band_h;
    if (!n) n = 1;
    if (band_y + n > band_h) n = band_h - band_y;
    uint64_t t0 = cnt_now();
    if (band_rot) convert_turned(band_y, band_y + n);
    else convert_rows(band_y, band_y + n);
    uint64_t t1 = cnt_now();
    frame_busy += t1 - t0; frame_bands++;
    band_y += n;
    if (band_y < band_h) return;
    band_active = 0;
    vcs->disp->frames++;
    frame_end = t1; frame_rest = frame_busy * FRAME_SHARE;
    uint64_t hz; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(hz));
    uint32_t us = hz ? (uint32_t)(frame_busy * 1000000u / hz) : 0;
    if (us > rep_busy_max) rep_busy_max = us;
    rep_frames++;
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
    /* the compact Mac's own screen at every tick, 60 a second (22 November 2026): at 30, the rate the HDMI
       conversion keeps, the pointer on the SE's and the Plus's glass moved at half the display's rate, and
       a Plus tester called the mouse sluggish. Comparing 342 rows of 64 bytes is a few microseconds; only
       rows that changed go out on the wire */
    if (psmac_board_se && !psmac_board_solo) se_frame();
    if (!(++tick_div & 1)) return;                                 /* a new HDMI frame every other tick: 30 a second */
    if ((vd->flags & PSMB_DISP_ENABLED) && !con_keep) {
        con_off = 1;
        /* if the last one is done, a thirtieth of a second has gone by since it began, and core 3 has
           had as long to itself as the last frame took (FRAME_SHARE) */
        if (!band_active && now - frame_t0 >= hz / 31 && now - frame_end >= frame_rest && convert_start()) {
            frame_t0 = now;
            frame_busy = 0; frame_bands = 0;
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
        const char *name = nscr == 1 ? "" : i ? " 1 (HDMI)" : " 0 (SE)";
        /* the VideoCore's picture since the last heartbeat (five seconds): frames converted, and the
           longest one's time on core 3 summed over its bands (FRAME_SHARE above); one kprintf per
           line, so that another core's output cannot land in the middle of it */
        if (&scr[i] == vcs && rep_frames)
            kprintf("[PSMAC]   display%s: %ux%u flags %x depth %u rowbytes %u vbl %s pending %u ticks %u frames %u clut seq %u; "
                    "VideoCore %u frames in 5 s, longest %u us in %u bands%s\n",
                    name, d->width, d->height, d->flags, d->depth, d->rowbytes, d->vbl_enable ? "on" : "off",
                    d->vbl_pending, d->vbl_count, d->frames, d->clut_seq, rep_frames, rep_busy_max, frame_bands,
                    rot == 90 ? ", turned 90" : rot == 180 ? ", turned 180" : rot == 270 ? ", turned 270" : "");
        else
            kprintf("[PSMAC]   display%s: %ux%u flags %x depth %u rowbytes %u vbl %s pending %u ticks %u frames %u clut seq %u\n",
                    name, d->width, d->height, d->flags, d->depth, d->rowbytes, d->vbl_enable ? "on" : "off",
                    d->vbl_pending, d->vbl_count, d->frames, d->clut_seq);
        if (&scr[i] == vcs) { if (rep_frames) last_busy_max = rep_busy_max; rep_frames = 0; rep_busy_max = 0; }
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

/* screen i's pixels for the video card (psmac_videocard.c, docs/16): the frame buffer of the mode in use
   in the kernel's view, its size, pitch and depth, and its colour table as the Mac wrote it (the card
   makes its own match of it, so nothing here is shared with core 3's conversion); 0 if the screen is not
   on or its mode's frame is not where it fits */
int psmac_display_surface(int i, uint8_t **pix, uint32_t *w, uint32_t *h, uint32_t *rb, uint32_t *depth,
                          const volatile uint32_t **clut, uint32_t *clut_seq, int *gray)
{
    if (!vcs || i < 0 || i >= nscr) return 0;
    volatile struct psmb_display *d = scr[i].disp;
    uint32_t dep = d->depth, rows = d->rowbytes, ht = d->height;
    int mi = mode_index(dep);
    if (!(d->flags & PSMB_DISP_ENABLED) || mi < 0 || !d->mode_base[mi] || rows * ht > mode_limit(&scr[i], mi)) return 0;
    *pix = (uint8_t *)mode_frame(&scr[i], mi);
    *w = d->width; *h = ht; *rb = rows; *depth = dep;
    *clut = d->clut; *clut_seq = d->clut_seq; *gray = (d->flags & PSMB_DISP_GRAY) != 0;
    return 1;
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
struct shot { int src; uint32_t w, h, depth, rb; const uint8_t *base; uint32_t pal[256]; };
static struct shot shots[2];                  /* one per screen, so a picture of a window can take from both (21 November) */
static struct shot *shot = &shots[0];         /* the one psmac_display_shot_row reads */
/* the parameters of screen display's picture into t: 1 the Mac's frame buffer, 2 the VideoCore's, 0 none */
static int shot_take(struct shot *t, int display, int want_hdmi)
{
    t->src = 0;
    if (!vcs || display < 0 || display >= nscr || display >= 2) return 0;
    struct screen *s = &scr[display];
    volatile struct psmb_display *d = s->disp;
    int mi = mode_index(d->depth);
    if (!want_hdmi && (d->flags & PSMB_DISP_ENABLED) && mi >= 0 && d->mode_base[mi]) {
        t->w = d->width; t->h = d->height; t->depth = d->depth; t->rb = d->rowbytes;
        t->base = mode_frame(s, mi);
        if (t->rb * t->h > mode_limit(s, mi) || t->rb < (t->w * t->depth + 7) / 8) return 0;
        for (int i = 0; i < 256; i++) {
            uint32_t c = d->clut[i] & 0xffffffu;
            if (d->flags & PSMB_DISP_GRAY) { uint32_t y = (((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 151 + (c & 255) * 28) >> 8; c = y << 16 | y << 8 | y; }
            t->pal[i] = c;
        }
        t->src = 1;
    } else if (vcfb && vc_w && vc_h && (want_hdmi || s == vcs)) {
        t->w = vc_w; t->h = vc_h; t->depth = vc_bpp; t->rb = vc_pitch; t->base = vcfb;
        t->src = 2;
    }
    return t->src;
}
/* a screenshot of screen display begins: its parameters are kept until the next one of that screen */
int psmac_display_shot(uint32_t *w, uint32_t *h, uint32_t *depth, int display, int want_hdmi)
{
    if (display < 0 || display >= 2) return 0;
    shot = &shots[display];
    if (!shot_take(shot, display, want_hdmi)) return 0;
    *w = shot->w; *h = shot->h; *depth = shot->depth;
    return shot->src;
}
/* what a screenshot of screen display would be, without disturbing one being sent (the state
   endpoint asks while a picture of the other screen may be under way) */
int psmac_display_probe(uint32_t *w, uint32_t *h, uint32_t *depth, int display)
{
    static struct shot t;
    if (!shot_take(&t, display, 0)) return 0;
    *w = t.w; *h = t.h; *depth = t.depth;
    return t.src;
}
/* n pixels of row y from pixel x0 of the screenshot of screen display begun above, as R,G,B bytes;
   the caller keeps x0 + n within the screen's width and y within its height */
void psmac_display_shot_span(int display, uint32_t y, uint32_t x0, uint32_t n, uint8_t *out)
{
    const struct shot *t = &shots[display & 1];
    const uint8_t *src = t->base + y * t->rb;
    if (t->src == 2) {                          /* the VideoCore buffer: little-endian XRGB (or swapped), or RGB565 */
        if (t->depth == 32) {
            for (uint32_t x = x0; x < x0 + n; x++, out += 3) {
                const uint8_t *p = src + 4 * x;
                if (vc_swap) { out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; }
                else { out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; }
            }
        } else {
            for (uint32_t x = x0; x < x0 + n; x++, out += 3) {
                const uint8_t *p = src + 2 * x;
                uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8, r = v >> 11, g = (v >> 5) & 63, b = v & 31;
                out[0] = (uint8_t)(r << 3 | r >> 2); out[1] = (uint8_t)(g << 2 | g >> 4); out[2] = (uint8_t)(b << 3 | b >> 2);
            }
        }
        return;
    }
    uint32_t c = 0;
    for (uint32_t x = x0; x < x0 + n; x++, out += 3) {
        switch (t->depth) {
        case 32: c = (uint32_t)src[4 * x + 1] << 16 | (uint32_t)src[4 * x + 2] << 8 | src[4 * x + 3]; break;      /* 00RRGGBB, big-endian */
        case 16: { uint32_t v = (uint32_t)src[2 * x] << 8 | src[2 * x + 1], r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
                   c = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2); break; }
        case 8: c = t->pal[src[x]]; break;
        case 4: c = t->pal[(src[x >> 1] >> ((x & 1) ? 0 : 4)) & 15]; break;
        case 2: c = t->pal[(src[x >> 2] >> (6 - 2 * (x & 3))) & 3]; break;
        default: c = t->pal[(src[x >> 3] >> (7 - (x & 7))) & 1]; break;
        }
        out[0] = (uint8_t)(c >> 16); out[1] = (uint8_t)(c >> 8); out[2] = (uint8_t)c;
    }
}
/* row y of the screenshot begun last, as R,G,B bytes, at most maxw pixels of it */
void psmac_display_shot_row(uint32_t y, uint8_t *out, uint32_t maxw)
{
    psmac_display_shot_span((int)(shot - shots), y, 0, shot->w < maxw ? shot->w : maxw, out);
}

/* ---- PiDisplay (psmac_hdmi.c) --------------------------------------------------------------------- */
/* the screen the VideoCore shows, as the Mac has it, the VideoCore buffer's depth and the longest frame
   on core 3 in the last heartbeat period */
int psmac_display_hdmi_screen(uint32_t *w, uint32_t *h, uint32_t *vc_depth, uint32_t *frame_us)
{
    if (!vcs) return 0;
    *w = vcs->disp->width; *h = vcs->disp->height; *vc_depth = vc_bpp; *frame_us = last_busy_max;
    return 1;
}
/* The 68k's RESET, with a new rotation asked for (core 0, psmac_hdmi_bus_reset): the Mac restarts, and
   its Slot Manager is about to read the declaration ROMs again. The HDMI screen's card gets the turned
   size (psmac_bus.c rewrites its video sResource from the file as it was loaded) and its display block
   is made again for it; the VideoCore keeps its buffer. The display's flags are 0 since the reset
   (psmac_display_bus_reset), so core 3 starts no frame meanwhile, and a band it is converting has its
   own copy of the geometry (band_*). PiScrn then sees the new size and arranges the screens again. */
int psmac_display_rotate(int r)
{
    extern int psmac_declrom_rescreen(uint32_t w, uint32_t h);
    if (!vcs || !win_base || (r != 0 && r != 90 && r != 180 && r != 270)) return 0;
    if (!(nscr == 2 || (!se_card && !(psmac_board_se && !psmac_board_solo)))) { kprintf("[PSMAC:DISP] rotation: the screen HDMI shows here is not turned (mac.hdmi=mirror or log)\n"); return 0; }
    uint32_t vw = vc_w, vh = vc_h, w = r == 90 || r == 270 ? vh : vw, h = r == 90 || r == 270 ? vw : vh;
    if (w < PSMB_SCREEN_MIN_W || h < PSMB_SCREEN_MIN_H || w > PSMB_SCREEN_MAX_W || h > PSMB_SCREEN_MAX_H) {
        kprintf("[PSMAC:DISP] rotation %d: the Mac's screen would be %ux%u, outside the sizes it takes\n", r, w, h); return 0;
    }
    if (!psmac_declrom_rescreen(w, h)) { kprintf("[PSMAC:DISP] rotation %d: the declaration ROM could not be rewritten\n", r); return 0; }
    struct screen *s = vcs;
    screen_init(s, win_base, s->block_off, s->frame_off, s->win_size, w, h, s->one_bit);
    rot = r;
    for (uint32_t i = 0; vcfb && i < vc_pitch * vc_h; i += 8) *(uint64_t *)(vcfb + i) = 0;
    arm_flush_cache((intptr_t)vcfb, vc_pitch * vc_h);
    kprintf("[PSMAC:DISP] rotation %d from this restart: the Mac's HDMI screen is %ux%u, the VideoCore %ux%u\n", r, w, h, vw, vh);
    return 1;
}
