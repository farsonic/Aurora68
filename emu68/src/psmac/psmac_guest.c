/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_guest.c — the guest scheduler: the PC card's PC and the Apple II card's Apple sharing a Pi core, the front
 * window's machine first (Aurora65's docs/DESIGN.md, "The guest scheduler on the shared core"; docs/20-apple-ii-card.md).
 *
 * Backstory (23 November 2026). The PC card (psmac_pc.c, docs/19-pc-card.md) had a core's loop to itself: core 1 called
 * psmac_pc_poll(1) between the sound output's turns and the video decoder's (psmac_usb.c), core 2 psmac_pc_poll(2) on the
 * solo board (psmac_bus.c), and the poll ran a slice of the PC each time. The Apple II card (psmac_a2.c) wants the same
 * core: an 8-bit Apple needs a twentieth of it, the PC all it gets. The owner's answer (Aurora65's DESIGN, "The owner's
 * answers"): "the front window first": both may run, the one whose window is in front gets the core first, the other
 * what is left. So the two call sites call psmac_guest_poll(core) instead, and each card is a guest, a table of
 * functions (psmac_guest.h): its service (commands, the window, input, drawing, write-back: every turn, cheap when
 * nothing changed), whether it is ready to run, and a slice of it.
 *
 * The rule, once a turn of the core's loop (at most one slice, so the sound output and the video decoder wait no longer
 * than they did for the PC alone):
 *  1. every guest of this core is serviced (a service that did real work, a step of the PC's start, takes the turn);
 *  2. among the ready guests, the front one (its program's window is the Mac's front window: the block's front word,
 *     set by the program on its activate events) runs; with none in front, or the front one not ready (idle, or an
 *     Apple ahead of its own time), the ready one that ran least recently;
 *  3. the background floor (guest.floor=N, eighths, 0 to 4, 1 by default): of every 8 turns in a row in which the front
 *     guest is ready, the last N go to a ready background guest. An 8-bit Apple behind a busy PC keeps its own speed
 *     with one turn in 8 (it needs about a twentieth of the core); with guest.floor=0 (the owner's rule taken strictly)
 *     a background Apple stops whenever the PC in front never idles, and its clock, disk motor and sound with it. The
 *     owner has not chosen yet: 1 is the default, 0 is there to choose (docs/20-apple-ii-card.md, "Sharing the core").
 * With one guest on a core the rule does nothing: it is serviced and, when ready, run, as psmac_pc_poll did. A guest
 * on another core (pc.core=, a2.core=) is that core's alone.
 */
#include <stdint.h>
#include "support.h"
#include "psmac_guest.h"

extern const char *psmac_option(const char *key);
#ifdef PSMAC_PC
extern struct psmac_guest psmac_pc_guest;
#endif
#ifdef PSMAC_A2
extern struct psmac_guest psmac_a2_guest;
#endif

#ifdef PSMAC_GUEST_HOST
uint64_t guest_host_now(void);
static inline uint64_t now(void) { return guest_host_now(); }
static inline uint64_t hz(void)  { return 1000000u; }
#else
static inline uint64_t now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
#endif

#define SLICE_US_CORE2 2000u         /* a slice on the solo board's core 2: the VIA's time waits this long at most */
#define SLICE_US_CORE1 1000u         /* on core 1: the sound output's DMA ring (48 ms) is looked at between slices */

static struct psmac_guest *const guests[] = {
#ifdef PSMAC_PC
    &psmac_pc_guest,
#endif
#ifdef PSMAC_A2
    &psmac_a2_guest,
#endif
};
#define NGUESTS (sizeof guests / sizeof guests[0])

/* The sound's turn (23 November 2026, the owner's SE: noise on HDMI right after the boot chime with the PC kernel). The
   guests run on core 1 on the SE board and the IIci testbed, and core 1 also refills the HDMI sound's DMA ring (six 8 ms
   blocks, 48 ms, played again when not refilled, psmac_audio.c). They cannot move off it there: core 2 is the SE's bus
   (the loop in psmac_bus.c that serves every one of the 68k's cycles to the SE's chips, and the SE speaker's deadlines;
   a slice there holds the Mac), core 3 the service loop (86 per cent busy on the owner's SE), core 0 the 68k. So the
   sound goes first instead: the scheduler gives it its turn before a guest's service and after its slice, and the guests
   call this inside their long work (the PC: Bochs reads the clock several times a millisecond, psmac_pc.c), so core 1
   never keeps the ring waiting longer than about a millisecond. guest.yield=off (testbed only) leaves it out, to compare;
   the heartbeat's audio line says core 1's longest wait. The solo board's guests are on core 2, which is light. */
#ifndef PSMAC_GUEST_HOST
extern int psmac_audio_poll(void);
static int yield_off = -1, in_yield;
static uint64_t yield_t;
#endif
void psmac_guest_yield(void)
{
#ifndef PSMAC_GUEST_HOST
    if (in_yield) return;
    uint64_t m; __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(m));
    if ((m & 3) != 1) return;
    if (yield_off < 0) { const char *v = psmac_option("guest.yield"); yield_off = v && v[0] == 'o' && v[1] == 'f'; }
    uint64_t t = now();
    if (yield_off || t - yield_t < hz() / 1000u) return;
    in_yield = 1;
    psmac_audio_poll();
    in_yield = 0;
    yield_t = now();
#endif
}

static struct turn { uint64_t n; uint32_t streak; } turns[4];   /* per core: turns taken, front-ready turns in a row */
static uint32_t floor_eighths = ~0u;

uint32_t psmac_guest_floor(void)
{
    if (floor_eighths == ~0u) {
        const char *v = psmac_option("guest.floor");
        uint32_t f = 1;
        if (v && v[0] >= '0' && v[0] <= '9') f = (uint32_t)(v[0] - '0');
        if (f > 4) f = 4;
        floor_eighths = f;
    }
    return floor_eighths;
}

int psmac_guest_poll(int core)
{
    if (core < 0 || core > 3) return 0;
    struct turn *tn = &turns[core];
    struct psmac_guest *mine[NGUESTS ? NGUESTS : 1];
    uint32_t n = 0;
    int worked = 0;
    for (uint32_t i = 0; i < NGUESTS; i++) {
        struct psmac_guest *g = guests[i];
        if (g->core() != core) continue;
        mine[n++] = g;
        psmac_guest_yield();                                        /* core 1: the sound first */
        if (g->service()) worked = 1;
    }
    if (!n) return 0;
    tn->n++;
    if (worked) return 1;
    uint64_t t = now();
    struct psmac_guest *front = 0, *back = 0;
    for (uint32_t i = 0; i < n; i++) {
        struct psmac_guest *g = mine[i];
        if (!g->ready(t)) continue;
        if (g->front() && !front) front = g;
        else if (!back || g->last_run < back->last_run) back = g;   /* the background guest that ran least recently */
    }
    struct psmac_guest *run = 0;
    if (front) {
        uint32_t fl = psmac_guest_floor();
        if (back && fl && tn->streak % 8u >= 8u - fl) { run = back; back->floor_runs++; }
        else run = front;
        tn->streak++;
    } else {
        tn->streak = 0;
        run = back;
    }
    if (!run) return 0;
    uint64_t t0 = now();
    psmac_guest_yield();
    int ran = run->slice(core == 2 ? SLICE_US_CORE2 : SLICE_US_CORE1);
    psmac_guest_yield();
    run->busy += now() - t0;
    run->runs++;
    run->last_run = tn->n;
    return ran;
}

void psmac_guest_report(void)
{
    static uint64_t busy0[NGUESTS ? NGUESTS : 1], runs0[NGUESTS ? NGUESTS : 1], floor0[NGUESTS ? NGUESTS : 1], t0;
    uint64_t t = now(), el = t - t0;
    if (!t0 || !el) { t0 = t; return; }
    t0 = t;
    int any = 0;
    for (uint32_t i = 0; i < NGUESTS; i++) if (guests[i]->core() >= 0) any++;
    if (any < 2) {                    /* one guest: its own line says it all */
        for (uint32_t i = 0; i < NGUESTS; i++) { busy0[i] = guests[i]->busy; runs0[i] = guests[i]->runs; floor0[i] = guests[i]->floor_runs; }
        return;
    }
    kprintf("[GUEST] guest.floor=%u:", psmac_guest_floor());
    for (uint32_t i = 0; i < NGUESTS; i++) {
        struct psmac_guest *g = guests[i];
        uint32_t pm = (uint32_t)((g->busy - busy0[i]) * 1000u / el);
        kprintf(" %s on core %d%s: %u slices (%u by the floor), %u per mille of the core;", g->name, g->core(), g->front() ? " (front)" : "",
                (uint32_t)(g->runs - runs0[i]), (uint32_t)(g->floor_runs - floor0[i]), pm);
        busy0[i] = g->busy; runs0[i] = g->runs; floor0[i] = g->floor_runs;
    }
    kprintf("\n");
}
