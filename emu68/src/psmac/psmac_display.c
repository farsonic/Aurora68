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
 * (mac/pivideo.s) draws into a 512 KiB region of the slot window at the depth QuickDraw chose and
 * keeps a colour table beside it (struct psmb_display, psmb.h); the service core, sixty times a
 * second, converts those indexed pixels into the VideoCore frame buffer the boot splash set up
 * (start_rpi64.c: init_display, 640 by 480, RGB565) and, while the driver wants it, raises the
 * slot interrupt, which is the card's vertical blank. In the testbed QEMU emulates that frame
 * buffer and `tools/vcshot.py` photographs it; on hardware it is the HDMI output.
 *
 * Why convert instead of mapping the VideoCore buffer into the Mac's address space: the Mac's
 * modes are 1, 2, 4 and 8 bits with a colour table, which is what System 6 and 7 software expects
 * of a card of 1989, and the VideoCore's are 16 and 32 bits direct; something has to apply the
 * table. A copy of 300 KB sixty times a second is nothing to an ARM core, and it keeps the frame
 * buffer in ordinary cached Pi RAM where the 68k's JIT is fastest. Direct 16/32-bit modes, in
 * which the Mac draws straight into the VideoCore buffer, can come later for the same card.
 */
#include <stdint.h>
#include "support.h"
#include "psmac.h"
#include "psmb.h"

extern void *framebuffer;                    /* start_rpi64.c: the VideoCore frame buffer, RGB565 */
extern uint32_t pitch, fb_width, fb_height;

static volatile struct psmb_display *disp;
static uint8_t *frame;                       /* the Mac's pixels, in the window */
static uint8_t *vcfb;                        /* the VideoCore frame buffer through the kernel's alias of physical
                                                memory: the low identity mapping start_rpi64.c drew the splash
                                                through is inside the first 4 GiB, which the Mac MMU shim replaces
                                                at the ROM's first table switch (docs/08, item 21) */
#define KERNEL_ALIAS 0xffffff9000000000ull
static uint16_t rgb565[256];                 /* the colour table as the VideoCore wants it */
static uint32_t clut_seen = ~0u;
static uint64_t next_tick;
static unsigned tick_div;

void psmac_display_init(uint8_t *win)
{
    disp = (volatile struct psmb_display *)(win + PSMB_OFF_DISPLAY);
    frame = win + PSMB_OFF_BOUNCE;
    for (uint32_t i = 0; i < sizeof(struct psmb_display) / 4; i++) ((volatile uint32_t *)disp)[i] = 0;
    disp->width = 640; disp->height = 480; disp->depth = 1; disp->rowbytes = 80;
    disp->fb_off = PSMB_OFF_BOUNCE; disp->fb_size = PSMB_BOUNCE_SIZE;
    /* a colour table for a card that was just switched on: 0 white ... 255 black, the Mac's order */
    for (int i = 0; i < 256; i++) { uint32_t g = 255 - i; disp->clut[i] = g << 16 | g << 8 | g; }
    for (uint32_t i = 0; i < PSMB_BOUNCE_SIZE; i += 8) *(uint64_t *)(frame + i) = 0;
    __asm__ volatile("dmb ish" ::: "memory");
    disp->version = 1; disp->magic = PSMB_DISP_MAGIC;
    if (framebuffer) vcfb = (uint8_t *)(KERNEL_ALIAS + (((uintptr_t)framebuffer) & 0x3fffffffull));   /* bus/identity -> physical -> alias */
    kprintf("[PSMAC:DISP] second display: 640x480 in the window at +%x, VideoCore frame buffer %s at %p (%ux%u, pitch %u)\n",
            PSMB_OFF_BOUNCE, framebuffer ? "present" : "absent", framebuffer, fb_width, fb_height, pitch);
}

/* the Mac's indexed pixels -> RGB565 through the table; depth 1/2/4: the leftmost pixel is in the
   high bits of the byte (QuickDraw's packing), value 0 is normally white */
static void convert(void)
{
    if (!vcfb) return;
    uint32_t depth = disp->depth, rb = disp->rowbytes, h = disp->height < fb_height ? disp->height : fb_height;
    uint32_t w = disp->width < fb_width ? disp->width : fb_width;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8) return;
    if (rb * h > PSMB_BOUNCE_SIZE) return;
    if (disp->clut_seq != clut_seen) {
        clut_seen = disp->clut_seq;
        for (int i = 0; i < 256; i++) {
            uint32_t c = disp->clut[i];
            if (disp->flags & PSMB_DISP_GRAY) { uint32_t y = (((c >> 16) & 255) * 77 + ((c >> 8) & 255) * 151 + (c & 255) * 28) >> 8; c = y << 16 | y << 8 | y; }
            rgb565[i] = (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
        }
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = frame + y * rb;
        uint16_t *dst = (uint16_t *)(vcfb + y * pitch);
        switch (depth) {
        case 8: for (uint32_t x = 0; x < w; x++) dst[x] = LE16(rgb565[src[x]]); break;
        case 4: for (uint32_t x = 0; x < w; x += 2) { uint8_t b = src[x >> 1]; dst[x] = LE16(rgb565[b >> 4]); dst[x + 1] = LE16(rgb565[b & 15]); } break;
        case 2: for (uint32_t x = 0; x < w; x += 4) { uint8_t b = src[x >> 2]; for (int k = 0; k < 4; k++) dst[x + k] = LE16(rgb565[(b >> (6 - 2 * k)) & 3]); } break;
        case 1: for (uint32_t x = 0; x < w; x += 8) { uint8_t b = src[x >> 3]; for (int k = 0; k < 8; k++) dst[x + k] = LE16(rgb565[(b >> (7 - k)) & 1]); } break;
        }
    }
    arm_flush_cache((intptr_t)vcfb, pitch * h);
    disp->frames++;
}

/* from the service core's loop: the 60 Hz tick (VBL) and, every other tick, the picture */
void psmac_display_poll(void)
{
    if (!disp) return;
    uint64_t now; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(now));
    if (!next_tick) { uint64_t hz; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(hz)); next_tick = now + hz / 60; return; }
    if ((int64_t)(now - next_tick) < 0) return;
    { uint64_t hz; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(hz)); next_tick += hz / 60; if ((int64_t)(now - next_tick) > (int64_t)hz) next_tick = now + hz / 60; }
    disp->vbl_count++;
    if ((disp->flags & PSMB_DISP_ENABLED) && disp->vbl_enable) { disp->vbl_pending = 1; __asm__ volatile("dmb ish" ::: "memory"); }
    if ((disp->flags & PSMB_DISP_ENABLED) && (++tick_div & 1)) convert();
}

/* for the heartbeat */
void psmac_display_report(void)
{
    if (!disp) return;
    kprintf("[PSMAC]   display: flags %x depth %u rowbytes %u vbl %s pending %u ticks %u frames %u clut seq %u\n",
            disp->flags, disp->depth, disp->rowbytes, disp->vbl_enable ? "on" : "off", disp->vbl_pending, disp->vbl_count, disp->frames, disp->clut_seq);
}

/* the slot interrupt line's share: the card's VBL is pending and the driver is there to take it */
int psmac_display_irq(void) { return disp && (disp->flags & PSMB_DISP_ENABLED) && disp->vbl_enable && disp->vbl_pending; }
