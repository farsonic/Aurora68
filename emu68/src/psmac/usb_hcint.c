/* SPDX-License-Identifier: MPL-2.0
 *
 * usb_hcint.c - a DWC2 host channel's halt, read; and the USB keyboard's watch (docs/08 item 130).
 *
 * Backstory. Until 23 November 2026 psmac_usb.c's DMA transport read a halted channel's interrupt bits
 * in its own order: STALL, then NAK, then the errors, and XFERCOMPL last. A halt with NAK set beside
 * anything else was a NAK: "no data now, poll again". On the owner's Pi 3B (a keyboard behind the
 * LAN9514's transaction translator) the heartbeat said, for long stretches after power-on,
 * "kb 1 (ok 0 nak 598565 err 1)": one failed poll, then nothing but NAKs, until the keyboard was
 * unplugged and plugged in again. With NAK taken first, a poll whose bits also carried a transaction
 * error, a toggle error or even a completed transfer was counted as a NAK, and the report in it, if
 * there was one, dropped without the host's data toggle moving on while the device's had. The order
 * here is Linux's (drivers/usb/dwc2/hcd_intr.c, dwc2_hc_chhltd_intr_dma, read for its order, none of
 * its code used): XFERCOMPL first, then STALL, the errors, NYET, NAK.
 *
 * The keyboard's watch is what happens after a failure. The data toggle (DATA0/DATA1, which tells a
 * repeated packet from a new one) is kept apart on the two sides, and a failure in the middle of a
 * split transaction can leave them apart: the hub's translator may have taken the keyboard's report and
 * acknowledged it, so the keyboard moved its toggle, while the host saw only the failure. The USB 2.0
 * specification (9.4.5) has CLEAR_FEATURE(ENDPOINT_HALT) set an endpoint's toggle to DATA0 whether it
 * was halted or not; the host's is set to DATA0 with it. That costs one control request and is done
 * at most once a second, after a failure of the kinds that can leave the toggles apart (uhc_needs_resync).
 * If the keyboard has still sent nothing at all since it was set up, after failures, 20 seconds on,
 * the bus is enumerated again once, which is what the owner's re-plug did.
 */
#include "usb_hcint.h"

enum uhc_outcome uhc_classify(uint32_t s)
{
    if (!s)
        return UHO_TIMEOUT;
    if (s & UHC_XFERCOMPL)
        return UHO_DONE;
    if (s & UHC_STALL)
        return UHO_STALL;
    if (s & UHC_XACTERR)
        return UHO_XACT;
    if (s & UHC_BBLERR)
        return UHO_BABBLE;
    if (s & UHC_FRMOVRUN)
        return UHO_FRMOVRUN;
    if (s & UHC_DATATGLERR)
        return UHO_TOGGLE;
    if (s & UHC_AHBERR)
        return UHO_AHB;
    if (s & UHC_NYET)
        return UHO_NYET;
    if (s & UHC_NAK)
        return UHO_NAK;
    return UHO_UNKNOWN;
}

const char *uhc_outcome_name(enum uhc_outcome o)
{
    static const char *const n[UHO_COUNT] = { "done", "stall", "transaction error", "babble", "frame overrun", "data toggle error",
                                              "AHB error", "NYET", "NAK", "timeout", "halted for no reason given" };
    return (unsigned)o < UHO_COUNT ? n[o] : "?";
}

void uhc_bits(uint32_t s, char *out, unsigned cap)
{
    static const char *const n[11] = { "XFERCOMPL", "CHHLTD", "AHBERR", "STALL", "NAK", "ACK", "NYET", "XACTERR", "BBLERR",
                                       "FRMOVRUN", "DATATGLERR" };
    unsigned k = 0;
    if (!cap)
        return;
    if (!s) {
        const char *z = "none";
        while (*z && k + 1 < cap)
            out[k++] = *z++;
    }
    for (unsigned b = 0; b < 32; b++) {
        if (!(s & (1u << b)))
            continue;
        char hex[8];
        const char *w = n[b < 11 ? b : 0];
        if (b >= 11) {                          /* a bit the programming model does not name */
            hex[0] = 'b'; hex[1] = 'i'; hex[2] = 't'; hex[3] = (char)('0' + b / 10); hex[4] = (char)('0' + b % 10); hex[5] = 0;
            w = hex;
        }
        if (k && k + 1 < cap)
            out[k++] = ' ';
        while (*w && k + 1 < cap)
            out[k++] = *w++;
    }
    out[k] = 0;
}

int uhc_needs_resync(enum uhc_outcome o)
{
    /* a frame overrun never reached the bus; a NAK carries no data; the rest may have */
    return o == UHO_STALL || o == UHO_XACT || o == UHO_BABBLE || o == UHO_TOGGLE || o == UHO_NYET || o == UHO_TIMEOUT ||
           o == UHO_UNKNOWN;
}

void ukw_setup(struct ukb_watch *w, uint32_t now_ms, int replug)
{
    int rescued = replug ? 0 : w->rescued;
    uint32_t resyncs = w->resyncs;
    *w = (struct ukb_watch){ 0 };
    w->t_setup = now_ms;
    w->rescued = rescued;
    w->resyncs = resyncs;
}

void ukw_fault(struct ukb_watch *w, enum uhc_outcome o)
{
    w->faults_seen++;
    if (uhc_needs_resync(o))
        w->pending = 1;
}

int ukw_step(struct ukb_watch *w, uint32_t now, uint32_t ok, uint32_t faults)
{
    if (w->pending && (!w->resync_ever || now - w->t_resync >= UKW_RESYNC_GAP_MS)) {
        w->pending = 0;
        w->resync_ever = 1;
        w->t_resync = now;
        w->resyncs++;
        return UKW_RESYNC;
    }
    if (!ok && faults && !w->rescued && now - w->t_setup >= UKW_RESCUE_MS) {
        w->rescued = 1;
        return UKW_ENUMERATE;
    }
    return UKW_NONE;
}
