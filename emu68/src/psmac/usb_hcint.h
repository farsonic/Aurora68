/* SPDX-License-Identifier: MPL-2.0
 *
 * usb_hcint.h - what a DWC2 host channel's interrupt bits (HCINTn) say a transfer ended in, and the
 * USB keyboard's watch (docs/08 item 130). Portable: no registers here, so tools/usb_hcint_test.c
 * checks it on the host. psmac_usb.c (the keyboard's and mouse's transport) and psmac_usbdisk.c (its
 * log lines) use it.
 */
#ifndef USB_HCINT_H
#define USB_HCINT_H
#include <stdint.h>

/* HCINTn, the DWC2 programming model's bit positions */
enum {
    UHC_XFERCOMPL = 1u << 0, UHC_CHHLTD = 1u << 1, UHC_AHBERR = 1u << 2, UHC_STALL = 1u << 3,
    UHC_NAK = 1u << 4, UHC_ACK = 1u << 5, UHC_NYET = 1u << 6, UHC_XACTERR = 1u << 7,
    UHC_BBLERR = 1u << 8, UHC_FRMOVRUN = 1u << 9, UHC_DATATGLERR = 1u << 10
};

/* What a halted channel's bits mean, in the order Linux's dwc2 host takes them (hcd_intr.c,
   dwc2_hc_chhltd_intr_dma): a completed transfer first, whatever else is set beside it; then a STALL;
   then the errors; NYET and NAK last. 0 (the channel never halted) is a timeout. */
enum uhc_outcome {
    UHO_DONE, UHO_STALL, UHO_XACT, UHO_BABBLE, UHO_FRMOVRUN, UHO_TOGGLE, UHO_AHB, UHO_NYET, UHO_NAK,
    UHO_TIMEOUT, UHO_UNKNOWN, UHO_COUNT
};
enum uhc_outcome uhc_classify(uint32_t hcint);
const char *uhc_outcome_name(enum uhc_outcome o);
/* the bits by name, "CHHLTD XACTERR ACK", into out (always terminated); "none" for 0 */
void uhc_bits(uint32_t hcint, char *out, unsigned cap);
/* an outcome after which the device may have moved its data toggle without the host (the packet may
   have reached it, or come from it, while the host saw a failure), or its endpoint is halted: the
   endpoint's toggle is to be set again (CLEAR_FEATURE(ENDPOINT_HALT), both sides DATA0) */
int uhc_needs_resync(enum uhc_outcome o);

/* The keyboard's watch. A keyboard that answered every poll with NAK for ever after one failed poll,
   until it was unplugged and plugged in again, is what the owner's Pi showed (kb 1, ok 0, nak 598565,
   err 1). Two steps, both cheap: after a failure that may have left the toggles apart, set them again
   (at most once a second); and when the keyboard has sent nothing at all since it was set up, after
   failures, for 20 seconds, enumerate the bus once more, as a re-plug would (once, until a real plug). */
enum { UKW_NONE, UKW_RESYNC, UKW_ENUMERATE };
struct ukb_watch {
    uint32_t t_setup, t_resync, faults_seen, resyncs;
    int resync_ever, rescued, pending;      /* pending: a fault that wants a resync, not yet done */
};
#define UKW_RESYNC_GAP_MS 1000u
#define UKW_RESCUE_MS 20000u
/* a fresh set-up (every enumeration); replug: the enumeration came from a plug or an unplug, which
   allows one more rescue */
void ukw_setup(struct ukb_watch *w, uint32_t now_ms, int replug);
/* a failed poll of the keyboard, with its outcome */
void ukw_fault(struct ukb_watch *w, enum uhc_outcome o);
/* what to do now: ok and faults are the keyboard's counts since its set-up */
int ukw_step(struct ukb_watch *w, uint32_t now_ms, uint32_t ok, uint32_t faults);
#endif
