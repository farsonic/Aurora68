/* SPDX-License-Identifier: MPL-2.0
 *
 * usb_hcint_test.c - native checks of emu68/src/psmac/usb_hcint.c (docs/08 item 130): the order in which a
 * DWC2 host channel's halt bits are read (Linux's: a completed transfer first, a NAK last), their names
 * for the log, and the USB keyboard's watch (a toggle set again after a failure that may have left it
 * apart, at most once a second; one more enumeration for a keyboard that has sent nothing in 20 s since
 * its set-up, after failures, once until a real plug). The controller itself is only on the Pi.
 *
 *   cc -std=c99 -Wall -Wextra -Werror -I emu68/src/psmac -o /tmp/usb_hcint_test tools/usb_hcint_test.c \
 *      emu68/src/psmac/usb_hcint.c && /tmp/usb_hcint_test       (make -C mac check runs it)
 */
#include <stdio.h>
#include <string.h>
#include "usb_hcint.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_classify(void)
{
    CHECK(uhc_classify(0) == UHO_TIMEOUT);
    CHECK(uhc_classify(UHC_CHHLTD | UHC_XFERCOMPL) == UHO_DONE);
    CHECK(uhc_classify(UHC_CHHLTD | UHC_XFERCOMPL | UHC_ACK) == UHO_DONE);
    /* the bug: a NAK beside a completed report was a NAK, and the report dropped */
    CHECK(uhc_classify(UHC_CHHLTD | UHC_XFERCOMPL | UHC_NAK) == UHO_DONE);
    CHECK(uhc_classify(UHC_CHHLTD | UHC_NAK) == UHO_NAK);
    CHECK(uhc_classify(UHC_CHHLTD | UHC_NYET) == UHO_NYET);
    CHECK(uhc_classify(UHC_CHHLTD | UHC_NAK | UHC_NYET) == UHO_NYET);
    /* errors win over NAK and NYET (Linux: "Must handle xacterr before nak or ack") */
    CHECK(uhc_classify(UHC_CHHLTD | UHC_XACTERR | UHC_NAK) == UHO_XACT);
    CHECK(uhc_classify(UHC_CHHLTD | UHC_DATATGLERR | UHC_NAK) == UHO_TOGGLE);
    CHECK(uhc_classify(UHC_CHHLTD | UHC_FRMOVRUN | UHC_NAK) == UHO_FRMOVRUN);
    CHECK(uhc_classify(UHC_CHHLTD | UHC_BBLERR) == UHO_BABBLE);
    CHECK(uhc_classify(UHC_CHHLTD | UHC_AHBERR) == UHO_AHB);
    CHECK(uhc_classify(UHC_CHHLTD | UHC_STALL | UHC_NAK) == UHO_STALL);
    CHECK(uhc_classify(UHC_CHHLTD | UHC_STALL | UHC_XACTERR) == UHO_STALL);
    CHECK(uhc_classify(UHC_CHHLTD) == UHO_UNKNOWN);
    CHECK(uhc_classify(UHC_CHHLTD | UHC_ACK) == UHO_UNKNOWN);
    for (int o = 0; o < UHO_COUNT; o++)
        CHECK(uhc_outcome_name((enum uhc_outcome)o)[0] != '?');
    CHECK(!strcmp(uhc_outcome_name(UHO_XACT), "transaction error"));
}

static void test_bits(void)
{
    char b[96];
    uhc_bits(0, b, sizeof b);
    CHECK(!strcmp(b, "none"));
    uhc_bits(UHC_CHHLTD | UHC_XACTERR, b, sizeof b);
    CHECK(!strcmp(b, "CHHLTD XACTERR"));
    uhc_bits(0x7ff, b, sizeof b);
    CHECK(!strcmp(b, "XFERCOMPL CHHLTD AHBERR STALL NAK ACK NYET XACTERR BBLERR FRMOVRUN DATATGLERR"));
    uhc_bits(1u << 13 | UHC_NAK, b, sizeof b);
    CHECK(!strcmp(b, "NAK bit13"));
    uhc_bits(0x7ff, b, 12);                     /* cut short, still terminated */
    CHECK(strlen(b) == 11 && !strncmp(b, "XFERCOMPL C", 11));
    uhc_bits(UHC_NAK, b, 1);
    CHECK(b[0] == 0);
}

static void test_resync_kinds(void)
{
    CHECK(uhc_needs_resync(UHO_XACT));
    CHECK(uhc_needs_resync(UHO_TOGGLE));
    CHECK(uhc_needs_resync(UHO_STALL));
    CHECK(uhc_needs_resync(UHO_NYET));          /* the translator never finished: the report may be lost */
    CHECK(uhc_needs_resync(UHO_TIMEOUT));
    CHECK(!uhc_needs_resync(UHO_NAK));
    CHECK(!uhc_needs_resync(UHO_DONE));
    CHECK(!uhc_needs_resync(UHO_FRMOVRUN));     /* never reached the bus */
}

static void test_watch(void)
{
    struct ukb_watch w = { 0 };
    uint32_t t = 1000;
    ukw_setup(&w, t, 1);
    /* nothing wrong: nothing to do, however long */
    CHECK(ukw_step(&w, t + 1, 0, 0) == UKW_NONE);
    CHECK(ukw_step(&w, t + 60000, 0, 0) == UKW_NONE);
    /* a NAK-only keyboard that has never failed is left alone (an untouched keyboard sends nothing) */
    ukw_setup(&w, t, 1);
    CHECK(ukw_step(&w, t + 25000, 0, 0) == UKW_NONE);
    /* the owner's case: one failed poll (a transaction error), then NAKs only */
    ukw_setup(&w, t, 1);
    ukw_fault(&w, UHO_XACT);
    CHECK(ukw_step(&w, t + 5, 0, 1) == UKW_RESYNC);
    CHECK(ukw_step(&w, t + 6, 0, 1) == UKW_NONE);
    /* a second failure inside the second waits for the gap */
    ukw_fault(&w, UHO_TOGGLE);
    CHECK(ukw_step(&w, t + 500, 0, 2) == UKW_NONE);
    CHECK(ukw_step(&w, t + 1005, 0, 2) == UKW_RESYNC);
    /* still nothing 20 s after the set-up: one enumeration, then no more */
    CHECK(ukw_step(&w, t + 19999, 0, 2) == UKW_NONE);
    CHECK(ukw_step(&w, t + 20000, 0, 2) == UKW_ENUMERATE);
    CHECK(ukw_step(&w, t + 40000, 0, 2) == UKW_NONE);
    /* the enumeration it asked for is not a plug: no second rescue after it */
    ukw_setup(&w, t + 41000, 0);
    ukw_fault(&w, UHO_XACT);
    CHECK(ukw_step(&w, t + 41001, 0, 1) == UKW_RESYNC);
    CHECK(ukw_step(&w, t + 70000, 0, 1) == UKW_NONE);
    /* a real plug allows one again */
    ukw_setup(&w, t + 80000, 1);
    ukw_fault(&w, UHO_TIMEOUT);
    CHECK(ukw_step(&w, t + 80001, 0, 1) == UKW_RESYNC);
    CHECK(ukw_step(&w, t + 100000, 0, 1) == UKW_ENUMERATE);
    /* a keyboard that has sent a report is never re-enumerated by the watch */
    ukw_setup(&w, t, 1);
    ukw_fault(&w, UHO_XACT);
    CHECK(ukw_step(&w, t + 1, 3, 1) == UKW_RESYNC);
    CHECK(ukw_step(&w, t + 30000, 3, 1) == UKW_NONE);
    /* a frame overrun counts as a failure but asks for no resync */
    ukw_setup(&w, t, 1);
    ukw_fault(&w, UHO_FRMOVRUN);
    CHECK(ukw_step(&w, t + 1, 1, 1) == UKW_NONE);
    /* the counter wraps (milliseconds since the Pi started, in 32 bits) */
    ukw_setup(&w, 0xfffff000u, 1);
    ukw_fault(&w, UHO_XACT);
    CHECK(ukw_step(&w, 0xfffff001u, 0, 1) == UKW_RESYNC);
    CHECK(ukw_step(&w, 0xfffff000u + 19000u, 0, 1) == UKW_NONE);
    CHECK(ukw_step(&w, 0xfffff000u + 20000u, 0, 1) == UKW_ENUMERATE);
}

int main(void)
{
    test_classify();
    test_bits();
    test_resync_kinds();
    test_watch();
    printf("usb_hcint_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
