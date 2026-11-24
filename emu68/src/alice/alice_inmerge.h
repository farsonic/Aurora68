/* SPDX-License-Identifier: GPL-2.0-only
 *
 * alice_inmerge.h - several keyboards and mice as one (24 November 2026, docs/08 item 139). The Mac's input ring
 * carries one keyboard state and one set of mouse buttons; the Pi may have a USB mouse, Bluetooth keyboards and mice
 * (one of them both, the owner's) and the remote control at once. Each source's last report is kept apart and the ring
 * gets their union: a button is down while any source holds it, a modifier while any keyboard holds it, a key while
 * any holds it. Plain C without the kernel, so that tools/inmerge_test.c runs it on the PC; alice_psmac.c uses it.
 */
#ifndef ALICE_INMERGE_H
#define ALICE_INMERGE_H
#include <stdint.h>

/* every source's buttons together */
static inline uint32_t inm_buttons(const uint8_t *btn, int n)
{
    uint32_t b = 0;
    for (int s = 0; s < n; s++) b |= btn[s];
    return b;
}
/* a source's new boot keyboard report into its slot. ErrorRollOver (every key 0x01: more keys than the report holds,
   HID 1.11 appendix C) keeps the keys as they were and takes the modifiers, which are still right */
static inline void inm_kb_set(uint8_t kb[8], const uint8_t report[8])
{
    int rollover = 0;
    for (int i = 2; i < 8; i++) if (report[i] == 0x01) rollover = 1;
    kb[0] = report[0];
    if (!rollover) for (int i = 1; i < 8; i++) kb[i] = report[i];
}
/* every source's keyboard as one boot report: the modifiers OR'd, the keys held by any (six at most) */
static inline void inm_kb_merge(const uint8_t (*kb)[8], int n, uint8_t out[8])
{
    unsigned k = 2;
    for (int i = 0; i < 8; i++) out[i] = 0;
    for (int s = 0; s < n; s++) {
        out[0] |= kb[s][0];
        for (int i = 2; i < 8; i++) {
            uint8_t u = kb[s][i];
            if (!u) continue;
            int dup = 0;
            for (unsigned j = 2; j < k; j++) if (out[j] == u) dup = 1;
            if (!dup && k < 8) out[k++] = u;
        }
    }
}
#endif
