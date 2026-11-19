/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_netprobe.c - where a ping's time goes, for the kernel log.
 *
 * Backstory. On the user's Pi 3B, 19 November, Riptide's ping to the home router over the WiFi took
 * 70 to 86 ms (IPv4 and IPv6 alike), where 2 to 10 ms is usual; through the testbed's file bridge the
 * Mac's side answers within a tick. The time is lost somewhere between the Mac's frame entering
 * the network board and the reply leaving it, and only the real Pi can say where. So every ICMP
 * and ICMPv6 echo request a unit sends, and the matching reply, are timed at four points:
 *   took   the kernel takes the request from the unit's transmit ring (the Mac put it there)
 *   out    the request has gone to the chip (SDIO for the WiFi, the USB bulk-out for the wired port)
 *   back   the reply has come out of the chip into the unit's receive ring
 *   mac    the Mac has taken the reply from the ring (its rx_tail passed the slot)
 * and one line per echo goes to the kernel log:
 *   [NET] echo wifi seq 3: took->out 140 us, out->back 2310 us, back->mac 820 us (total 3270 us)
 * out->back is the chip, the air and the router; back->mac is the slot interrupt and the Mac's
 * driver; a long took->out or back->mac with a short out->back points at the kernel's service loop
 * (its longest gap is in the heartbeat, psmac_alice_core). Costs nothing for other frames: a
 * few compares on the frame's type bytes.
 */
#include <stdint.h>
#include "support.h"

static inline uint64_t now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }

/* an echo's identifier and sequence, 0 if the frame is no echo; *reply 1 for a reply */
static uint32_t echo_key(const volatile uint8_t *f, uint32_t len, int *reply)
{
    if (len < 14 + 28)
        return 0;
    uint16_t et = (uint16_t)(f[12] << 8 | f[13]);
    const volatile uint8_t *ic;
    if (et == 0x0800) {
        unsigned ihl = (f[14] & 15u) * 4u;
        if (f[23] != 1 || 14 + ihl + 8 > len)
            return 0;
        ic = f + 14 + ihl;
        if (ic[0] != 8 && ic[0] != 0)
            return 0;
        *reply = ic[0] == 0;
    } else if (et == 0x86dd) {
        if (f[20] != 58 || len < 14 + 40 + 8)
            return 0;
        ic = f + 14 + 40;
        if (ic[0] != 128 && ic[0] != 129)
            return 0;
        *reply = ic[0] == 129;
    } else
        return 0;
    return ((uint32_t)ic[4] << 24 | (uint32_t)ic[5] << 16 | (uint32_t)ic[6] << 8 | ic[7]) + 1u;   /* 0 stays "none" */
}

#define PROBES 8
static struct probe { uint32_t key, slot; int unit, state; uint64_t t[4]; } P[PROBES];
static unsigned next;
static volatile uint32_t *rx_tail_of[2];

/* a request taken from unit's transmit ring (stage 0) or handed to the chip (stage 1) */
void psmac_netprobe_tx(int unit, const volatile uint8_t *f, uint32_t len, int stage)
{
    int reply = 0;
    uint32_t key = echo_key(f, len, &reply);
    if (!key || reply)
        return;
    if (stage == 0) {
        struct probe *p = &P[next++ % PROBES];
        p->key = key; p->unit = unit; p->state = 1; p->t[0] = now();
        return;
    }
    for (int i = 0; i < PROBES; i++)
        if (P[i].state == 1 && P[i].key == key && P[i].unit == unit) { P[i].t[1] = now(); P[i].state = 2; return; }
}
/* a reply put into unit's receive ring at index slot (rx_head before the increment) */
void psmac_netprobe_rx(int unit, const volatile uint8_t *f, uint32_t len, uint32_t slot, volatile uint32_t *rx_tail)
{
    int reply = 0;
    uint32_t key = echo_key(f, len, &reply);
    if (!key || !reply)
        return;
    for (int i = 0; i < PROBES; i++)
        if ((P[i].state == 1 || P[i].state == 2) && P[i].key == key && P[i].unit == unit) {
            P[i].t[2] = now(); P[i].slot = slot;
            if (P[i].state == 1) P[i].t[1] = P[i].t[2];    /* the reply came before the send was seen done */
            P[i].state = 3;
            rx_tail_of[unit & 1] = rx_tail;
            return;
        }
}
/* the service loop, every turn: replies the Mac has taken */
void psmac_netprobe_poll(void)
{
    for (int i = 0; i < PROBES; i++) {
        struct probe *p = &P[i];
        if (p->state != 3 || !rx_tail_of[p->unit & 1])
            continue;
        if ((int32_t)(*rx_tail_of[p->unit & 1] - p->slot) <= 0) {
            if (now() - p->t[2] > hz() * 2) p->state = 0;      /* never taken (the driver closed) */
            continue;
        }
        p->t[3] = now();
        p->state = 0;
        uint64_t us = hz() / 1000000u;
        kprintf("[NET] echo %s seq %u: took->out %u us, out->back %u us, back->mac %u us (total %u us)\n",
                p->unit == 0 ? "wifi" : "wired", (p->key - 1u) & 0xffffu,
                (uint32_t)((p->t[1] - p->t[0]) / us), (uint32_t)((p->t[2] - p->t[1]) / us),
                (uint32_t)((p->t[3] - p->t[2]) / us), (uint32_t)((p->t[3] - p->t[0]) / us));
    }
}
