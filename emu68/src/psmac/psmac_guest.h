/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_guest.h — the guest scheduler: the cards that run a whole other computer on a Pi core (the PC card,
 * psmac_pc.c; the Apple II card, psmac_a2.c) share that core, the one whose program's window is the Mac's front
 * window first (psmac_guest.c; Aurora65's docs/DESIGN.md, "The guest scheduler on the shared core").
 *
 * Only in a kernel built with a guest (-DPSMAC_PC=ON or -DPSMAC_A2=ON): the normal kernel has none of this.
 */
#ifndef PSMAC_GUEST_H
#define PSMAC_GUEST_H
#include <stdint.h>

struct psmac_guest {
    const char *name;                        /* "PC", "Apple II": the log's and the heartbeat's */
    /* the core it runs on (decided by the card at its first call), or -1: not on any (the card is absent, off) */
    int      (*core)(void);
    /* every turn, on its core: commands, the window's place, input, drawing, write-back; 1 if it did real work
       there (a step of the PC's start: that turn's slice is then not given) */
    int      (*service)(void);
    /* 0: nothing to run now (off, paused, idle until later, ahead of its own time); else 1 */
    int      (*ready)(uint64_t now);
    /* runs at most budget_us microseconds of the core's time; returns 1 if it ran */
    int      (*slice)(uint32_t budget_us);
    /* 1 while its program's window is the Mac's front window (the program sets its block's front word) */
    int      (*front)(void);
    /* the scheduler's own */
    uint64_t busy;                           /* counter ticks in its slices, since the start */
    uint64_t runs, last_run;                 /* slices given; the turn of the last one */
    uint64_t floor_runs;                     /* slices it had as a background guest through the floor */
};

/* one turn of a core's loop: every guest of that core serviced, at most one slice given; 1 if a guest ran */
int  psmac_guest_poll(int core);
/* the heartbeat's line: each guest's share of the turns */
void psmac_guest_report(void);
/* guest.floor=: eighths of the turns a busy front guest leaves to a ready background guest (0-4; 1 by default) */
uint32_t psmac_guest_floor(void);

#endif
