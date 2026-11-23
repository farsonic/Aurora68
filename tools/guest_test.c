/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/guest_test.c - the guest scheduler's rule on the host (emu68/src/psmac/psmac_guest.c, docs/20-apple-ii-card.md
 * "Sharing the core"), with two pretend guests in the places of the PC card and the Apple II card: which one runs, turn
 * by turn, with either in front, neither, one not ready, and guest.floor= 0 to 4.
 *
 * Backstory (23 November 2026). The real cards together (tools/a2card_test.sh's "sched" runs, the PC with Aurora86's
 * host library) show that both run, but whether the PC is busy depends on what its firmware is doing at the time; the
 * rule itself is checked here, exactly, with guests that are ready when told and take no time.
 *   cc -DPSMAC_GUEST_HOST -DPSMAC_PC -DPSMAC_A2 -Iemu68/src/psmac -Itools/pccard_host tools/guest_test.c emu68/src/psmac/psmac_guest.c
 *   guest_test FLOOR
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "psmac_guest.h"

static uint64_t clock_us;
uint64_t guest_host_now(void) { return clock_us; }
static char floor_opt[8] = "1";
const char *psmac_option(const char *key) { return !strcmp(key, "guest.floor") ? floor_opt : 0; }

static struct fake { int ready, front, ran; } F[2];   /* 0: the PC's place, 1: the Apple's */
static int core0(void) { return 2; }
static int serv(void) { return 0; }
static int ready0(uint64_t t) { (void)t; return F[0].ready; }
static int ready1(uint64_t t) { (void)t; return F[1].ready; }
static int slice0(uint32_t b) { (void)b; F[0].ran++; clock_us += 1000; return 1; }
static int slice1(uint32_t b) { (void)b; F[1].ran++; clock_us += 1000; return 1; }
static int front0(void) { return F[0].front; }
static int front1(void) { return F[1].front; }
struct psmac_guest psmac_pc_guest = { "PC", core0, serv, ready0, slice0, front0, 0, 0, 0, 0 };
struct psmac_guest psmac_a2_guest = { "Apple II", core0, serv, ready1, slice1, front1, 0, 0, 0, 0 };

static int fails;
static void run(const char *what, int r0, int r1, int f0, int f1, int turns, int want0, int want1)
{
    F[0].ready = r0; F[1].ready = r1; F[0].front = f0; F[1].front = f1; F[0].ran = F[1].ran = 0;
    for (int i = 0; i < turns; i++) psmac_guest_poll(2);
    int ok = F[0].ran == want0 && F[1].ran == want1;
    printf("guest_test: %-4s guest.floor=%u, %s: PC %d, Apple %d of %d turns (expected %d, %d)\n", ok ? "ok" : "FAIL",
           psmac_guest_floor(), what, F[0].ran, F[1].ran, turns, want0, want1);
    if (!ok) fails++;
}

int main(int argc, char **argv)
{
    /* guest_test FLOOR (0-4, read once at the first turn): a busy front guest leaves FLOOR turns in 8 to a ready
       background one */
    int fl = argc > 1 ? atoi(argv[1]) : 1;
    snprintf(floor_opt, sizeof floor_opt, "%d", fl);
    run("the PC in front, both always ready", 1, 1, 1, 0, 800, 800 - 100 * fl, 100 * fl);
    run("the Apple in front, both always ready", 1, 1, 0, 1, 800, 100 * fl, 800 - 100 * fl);
    run("the PC in front and not ready (idle): the Apple runs", 0, 1, 1, 0, 80, 0, 80);
    run("the PC in front, the Apple not ready: the PC runs", 1, 0, 1, 0, 80, 80, 0);
    run("neither in front: by turns", 1, 1, 0, 0, 80, 40, 40);
    run("neither ready: nothing runs", 0, 0, 1, 0, 80, 0, 0);
    if (fails) return 1;
    printf("guest_test: the front window's machine first, the background floor as asked\n");
    return 0;
}
