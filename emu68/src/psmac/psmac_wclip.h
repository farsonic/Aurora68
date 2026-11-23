/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_wclip.h — drawing into a Mac window from the Pi: where the window shows, where the Mac's pointer is,
 * whether a menu is down (psmac_wclip.c). Shared by the cards that draw into the Mac's own frame buffer: the
 * video decoder card (psmac_video.c) and the PC card (psmac_pc.c).
 */
#ifndef PSMAC_WCLIP_H
#define PSMAC_WCLIP_H
#include <stdint.h>

#define PSMAC_WCLIP_MAX    16                /* the most rectangles a window's visible region is drawn as */
#define PSMAC_WCLIP_24BIT  1u                /* the program runs in 24-bit mode: a master pointer's high byte is flags */
#define PSMAC_WCLIP_CURSOR 24                /* pixels left alone each side of the Mouse point */

/* a long of the Mac's RAM at its own address (aligned), *ok 0 outside the RAM */
uint32_t psmac_wclip_rd32(uint32_t addr, int *ok);
/* 1 while the Mac has a menu pulled down, whichever program's; 0 where it cannot tell */
int psmac_wclip_menu_down(void);
/* the window's visible region (rgn: its visRgn handle, a 68k address; flags PSMAC_WCLIP_24BIT) as at most
   PSMAC_WCLIP_MAX rectangles (top, left, bottom, right) in the screen's pixels: the region's coordinates are the
   window's own, its 0,0 at x0, y0 on the screen; cut to cx0..cx1, cy0..cy1. The count, or -1 when the region
   cannot be read now (nothing should be drawn then) */
int psmac_wclip_parts(int32_t out[PSMAC_WCLIP_MAX][4], uint32_t rgn, uint32_t flags, int32_t x0, int32_t y0,
                      int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1);
/* the box around the Mac's pointer on that screen (psmac_display.c's numbering), top, left, bottom, right;
   0 when the pointer is not on it */
int psmac_wclip_cursor_box(uint32_t screen, int32_t out[4]);

#endif
