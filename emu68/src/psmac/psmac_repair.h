/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_repair.h — what a card that draws into a Mac window asks its program to have the Mac paint again
 * (docs/20-apple-ii-card.md, "Drawn over another window"; docs/19-pc-card.md). Shared by the cards (psmac_wclip.c
 * writes it) and their programs (mac/a68pc/repair.h reads it); the Apple II card's and the PC card's windows hold one
 * at A68A_OFF_REPAIR / A68P_OFF_REPAIR, and the Mac's half of their blocks says which request was carried out.
 *
 * Backstory (24 November 2026, the owner's SE on the 2560 x 1440 HDMI screen): a Finder window dragged over the busy
 * Apple II window was left with a rectangle of the Apple's screen inside it. The card had read the window's visRgn,
 * and was still drawing with it when the Mac moved the Finder's window over it and gave the Apple's window its new,
 * smaller region; nothing on the Mac knew to draw that part of the Finder's window again. The card now looks at the
 * region again between the bands it draws, but a band begun just before the Mac changes the screen can still land on
 * another window; so the card says which rectangles it drew outside the region the window has now, and the program
 * has the Mac paint them again (PaintBehind: the windows there are erased and given update events, the desktop
 * repainted).
 *
 * The card writes a request as a seqlock: seq + 1 (odd: being written), the rectangles and n, then seq + 1 again
 * (even). A request the program has not carried out yet (its done word in the Mac's half is not seq) is added to, not
 * replaced. The program copies n and the rectangles while seq is even and the same before and after, has them painted,
 * then writes seq into its done word. Every word big-endian, as the rest of the window.
 */
#ifndef PSMAC_REPAIR_H
#define PSMAC_REPAIR_H
#include <stdint.h>

#define A68_REPAIR_MAX 16                    /* rectangles in one request; more are merged into the last */
struct a68_repair_rect {
    int32_t screen;                          /* the card's screen number (A68A_SCREEN_*, A68P_SCREEN_*) */
    int16_t top, left, bottom, right;        /* in that screen's own pixels, QuickDraw's order */
};
struct a68_repair {
    uint32_t seq;                            /* even when complete; + 2 for every request (or addition to one) */
    uint32_t n;                              /* rectangles in r */
    uint32_t posted;                         /* requests posted since the card started (for the log) */
    uint32_t pad;
    struct a68_repair_rect r[A68_REPAIR_MAX];
};
_Static_assert(sizeof(struct a68_repair_rect) == 12 && sizeof(struct a68_repair) == 16 + 12 * A68_REPAIR_MAX, "a68_repair");

#endif
