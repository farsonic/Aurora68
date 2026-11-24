/* SPDX-License-Identifier: GPL-2.0-only
 *
 * inmerge_test.c - several keyboards and mice as one (emu68/src/alice/alice_inmerge.h, which alice_psmac.c uses to
 * feed the Mac's input ring), on the PC. 24 November 2026, docs/08 item 139. The owner's SE has its own mouse (which
 * the Pi never sees), a Bluetooth keyboard that is a pointer too, and a Bluetooth mouse; a USB mouse or the remote
 * control may be there besides. Checked: one source's button up does not let go of another's; a source gone lets go
 * of its own alone; the modifiers of two keyboards OR'd and let go one by one; the keys of both; ErrorRollOver.
 * GPL-2.0-only like the header. Run by tools/bt_test.sh.
 */
#include <stdio.h>
#include <string.h>
#include "alice_psmac.h"
#include "alice_inmerge.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t btn[ALICE_NSRC], kb[ALICE_NSRC][8];
static int has(const uint8_t m[8], uint8_t u) { for (int i = 2; i < 8; i++) if (m[i] == u) return 1; return 0; }

int main(void)
{
    enum { USB = ALICE_SRC_USB, BTK = ALICE_SRC_BT0, BTM = ALICE_SRC_BT0 + 1, REM = ALICE_SRC_REMOTE };
    /* the combo keyboard's pointer holds the left button; a USB mouse's report with no button comes meanwhile */
    btn[BTK] = 1;
    CHECK(inm_buttons(btn, ALICE_NSRC) == 1, "the Bluetooth pointer's left button down");
    btn[USB] = 0;
    CHECK(inm_buttons(btn, ALICE_NSRC) == 1, "a USB mouse's report with its button up does not let go of it");
    btn[BTM] = 2;
    CHECK(inm_buttons(btn, ALICE_NSRC) == 3, "a second mouse's right button: both");
    btn[BTK] = 0;
    CHECK(inm_buttons(btn, ALICE_NSRC) == 2, "the first let go: the second's still down");
    btn[BTM] = 0;   /* its link went down: psmac_input_gone clears its slot */
    CHECK(inm_buttons(btn, ALICE_NSRC) == 0, "the second gone: all up");
    btn[REM] = 1;
    CHECK(inm_buttons(btn, ALICE_NSRC) == 1, "the remote control's button is a source like any");
    btn[REM] = 0;

    /* two keyboards: Shift on one, Command on the other */
    uint8_t m[8];
    { uint8_t r[8] = { 0x02, 0, 0x04 }; inm_kb_set(kb[BTK], r); }
    { uint8_t r[8] = { 0x08, 0, 0x13 }; inm_kb_set(kb[USB], r); }
    inm_kb_merge((const uint8_t (*)[8])kb, ALICE_NSRC, m);
    CHECK(m[0] == 0x0a && has(m, 0x04) && has(m, 0x13), "Shift and Command, A and P, from two keyboards (mods %02x)", m[0]);
    { uint8_t r[8] = { 0 }; inm_kb_set(kb[USB], r); }
    inm_kb_merge((const uint8_t (*)[8])kb, ALICE_NSRC, m);
    CHECK(m[0] == 0x02 && has(m, 0x04) && !has(m, 0x13), "the USB keyboard let go: the Bluetooth one's Shift and A kept");
    { uint8_t r[8] = { 0x02, 0, 0x04 }; inm_kb_set(kb[USB], r); }
    inm_kb_merge((const uint8_t (*)[8])kb, ALICE_NSRC, m);
    CHECK(m[2] == 0x04 && m[3] == 0, "the same key on both: once");
    /* ErrorRollOver: the keys as they were, the modifiers new */
    { uint8_t r[8] = { 0x00, 0, 1, 1, 1, 1, 1, 1 }; inm_kb_set(kb[BTK], r); }
    CHECK(kb[BTK][0] == 0 && kb[BTK][2] == 0x04 && kb[BTK][3] == 0, "ErrorRollOver: A still held, Shift let go");
    /* the Bluetooth keyboard's link goes down: its slot cleared (psmac_input_gone) */
    memset(kb[BTK], 0, 8);
    memset(kb[USB], 0, 8);
    inm_kb_merge((const uint8_t (*)[8])kb, ALICE_NSRC, m);
    CHECK(!m[0] && !m[2], "both gone: nothing held");
    /* seven keys from two keyboards: six reach the ring */
    { uint8_t r[8] = { 0, 0, 4, 5, 6, 7 }; inm_kb_set(kb[USB], r); }
    { uint8_t r[8] = { 0, 0, 8, 9, 10 }; inm_kb_set(kb[BTK], r); }
    inm_kb_merge((const uint8_t (*)[8])kb, ALICE_NSRC, m);
    CHECK(m[7] != 0 && has(m, 4) && has(m, 8), "seven keys held: six in the report");

    printf("inmerge_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
