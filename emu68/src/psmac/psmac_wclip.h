/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_wclip.h — drawing into a Mac window from the Pi: where the window shows, where the Mac's pointer is,
 * whether a menu is down (psmac_wclip.c). Shared by the cards that draw into the Mac's own frame buffer: the
 * video decoder card (psmac_video.c) and the PC card (psmac_pc.c).
 */
#ifndef PSMAC_WCLIP_H
#define PSMAC_WCLIP_H
#include <stdint.h>
#include "psmac_repair.h"

#define PSMAC_WCLIP_MAX    16                /* the most rectangles a window's visible region is drawn as */
#define PSMAC_WCLIP_24BIT  1u                /* the program runs in 24-bit mode: a master pointer's high byte is flags */
#define PSMAC_WCLIP_CURSOR 24                /* pixels left alone each side of the Mouse point (rounded out to the frame buffer's longwords across) */

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
/* 1 when rgn (a visRgn handle, flags PSMAC_WCLIP_24BIT) reads as a live region: a master pointer into the RAM, the
   block it points to a relocatable block of the heap that handle belongs to (its header's tag and relative handle), big
   enough for the region, which is well formed (rgnSize even and at least 10, a bounding box in order, a last $7FFF);
   and, when w and h are given (not 0), its bounding box inside a w x h window's content at 0,0, a pixel to spare.
   0 for anything else: a disposed handle, a heap gone with its program, a block that has moved, memory reused */
int psmac_wclip_rgn_ok(uint32_t rgn, uint32_t flags, int32_t w, int32_t h);
/* A region's fingerprint (24 November 2026, docs/20-apple-ii-card.md "Drawn over another window"): the handle's master
   pointer, rgnSize and a checksum of the region's bytes (its bounding box among them). The Window Manager rewrites a
   window's visRgn in place, or moves it, whenever any window moves over it or away; two fingerprints that differ mean
   the region changed between them. psmac_wclip_sig: 1 when read, 0 when the region cannot be read now */
struct psmac_wclip_sig { uint32_t mp, size, sum; };
int psmac_wclip_sig(uint32_t rgn, uint32_t flags, struct psmac_wclip_sig *s);
static inline int psmac_wclip_sig_eq(const struct psmac_wclip_sig *a, const struct psmac_wclip_sig *b)
{
    return a->mp == b->mp && a->size == b->size && a->sum == b->sum;
}
/* psmac_wclip_parts with the region's fingerprint read before and after it (into *sig): -1 also when the two differ */
int psmac_wclip_parts_sig(int32_t out[PSMAC_WCLIP_MAX][4], uint32_t rgn, uint32_t flags, int32_t x0, int32_t y0,
                          int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1, struct psmac_wclip_sig *sig);
/* a minus b, in place: a's n rectangles (top, left, bottom, right; room for max) less each of b's nb. The count; when
   the pieces would be more than max, the bounding box of what is left is the one rectangle (more than the difference,
   never less) */
int psmac_wclip_minus(int32_t a[][4], int n, int max, const int32_t b[][4], int nb);

/* The overdraw guard (24 November 2026, the owner's SE: a Finder window dragged over the busy Apple II window kept a
   rectangle of the Apple's screen). A card reads its window's region at the start of a picture and draws for a while;
   the Mac, on another core, can move a window over it meanwhile. So:
    (a) psmac_wclip_guard_still before each band of a picture: the region's fingerprint again, and the picture is given
        up (drawn again whole with the new region) as soon as it differs; and once more at its end;
    (b) every band is noted before it is drawn (psmac_wclip_guard_add): what the pictures drew with the region they were
        read with. When the region (or the window's place) is found to have changed, what was drawn outside the new
        region is to be painted again by the Mac (psmac_wclip_guard_watch, then psmac_wclip_repair_post), if it was
        drawn recently enough to have landed after the Mac's change (young: the caller's window of time) or by a
        picture that saw the change itself. */
#define PSMAC_WCLIP_DRAWN 16
struct psmac_wclip_guard {
    uint32_t rgn, flags;                     /* the picture's region, as read at its start */
    struct psmac_wclip_sig sig;
    int n;                                   /* rectangles drawn (r) with sig since it was first read */
    uint32_t screen;
    int32_t x0, y0;                          /* the window's place then (its region's 0,0 on the screen) */
    uint64_t t_end;                          /* when the last picture drawn with it ended (the caller's clock) */
    int suspect;                             /* a picture saw the region change while it drew */
    int32_t r[PSMAC_WCLIP_DRAWN][4];
    uint32_t looks, given_up, late, repairs, repair_rects;   /* for the heartbeat */
};
/* a picture begins with this region (its fingerprint s, read with its parts), the window at x0, y0 on screen */
void psmac_wclip_guard_begin(struct psmac_wclip_guard *g, uint32_t rgn, uint32_t flags, const struct psmac_wclip_sig *s,
                             uint32_t screen, int32_t x0, int32_t y0);
/* 1 while the region is still the one the picture began with */
int psmac_wclip_guard_still(struct psmac_wclip_guard *g);
/* a band about to be drawn (screen pixels) */
void psmac_wclip_guard_add(struct psmac_wclip_guard *g, int32_t t, int32_t l, int32_t b, int32_t r);
/* the picture's end at time t; changed: it saw the region change (given up, or at its last look) */
void psmac_wclip_guard_end(struct psmac_wclip_guard *g, uint64_t t, int changed);
/* At every turn: has the region, or the window's place, changed since the pictures noted were drawn? Then the
   rectangles drawn outside the region now (rgn, the window at x0, y0 on screen, w x h, on the screen sw x sh) go into
   out, their count returned, and the notes start again. 0 when nothing is to be painted again (unchanged, grown, or
   drawn longer ago than young allows, now - t_end > young); -1 when the region cannot be read now (asked again). */
int psmac_wclip_guard_watch(struct psmac_wclip_guard *g, uint32_t rgn, uint32_t flags, uint32_t screen, int32_t x0,
                            int32_t y0, int32_t w, int32_t h, int32_t sw, int32_t sh, uint64_t now, uint64_t young,
                            int32_t out[PSMAC_WCLIP_DRAWN][4]);
/* the rectangles (on screen) into the card's repair request for its program (psmac_repair.h); done: the Mac's done
   word. Added to a request the program has not carried out yet */
void psmac_wclip_repair_post(volatile struct a68_repair *rp, uint32_t done, uint32_t screen, int32_t r[][4], int n);

/* the box around the Mac's pointer on that screen (psmac_display.c's numbering), top, left, bottom, right;
   0 when the pointer is not on it */
int psmac_wclip_cursor_box(uint32_t screen, int32_t out[4]);

#endif
