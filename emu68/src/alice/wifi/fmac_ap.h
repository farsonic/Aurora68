/* SPDX-License-Identifier: GPL-2.0-only
 * fmac_ap.h - the FullMAC firmware as an access point: the command sequence, built as data.
 *
 * Aurora68's addition to the Alice68 WiFi code (23 November 2026, docs/08 item 118), not Alice68's own:
 * Alice68 drives the firmware as a station only. Aurora68 Setup and Recovery can make the Pi's radio an
 * access point of its own (a phone joins it and opens the recovery page), so the firmware has to be put
 * into its SoftAP mode, the sequence Linux's brcmfmac sends in brcmf_cfg80211_start_ap (cfg80211.c) for a
 * single interface with the firmware's own authenticator: down, the AP role, the channel, up, the key,
 * the security, the SSID.
 *
 * The sequence is built as a list of steps rather than sent from inside the driver, so that one builder
 * serves three callers: the real driver (alice_net.c, which sends each step to the chip), the testbed's
 * simulated radio (alice_psmac.c, which checks each step against a model of what the firmware accepts)
 * and the host test (tools/apnet_test.c, which checks the bytes). Like fmac.c, host-independent: every
 * value is written byte by byte in the firmware's little-endian order.
 */
#ifndef ALICE68_FMAC_AP_H
#define ALICE68_FMAC_AP_H
#include <stdint.h>

#define WLC_SET_CHANNEL     30
#define WLC_SET_BCNPRD      76
#define WLC_SET_DTIMPRD     78

/* events an access point is told about, beside the station's (fmac.h) */
#define E_AUTH_IND          4
#define E_ASSOC_IND         8
#define E_REASSOC_IND       10

enum { FMS_CMD_INT, FMS_IOV_INT, FMS_CMD_DATA, FMS_IOV_DATA };
#define FMS_FATAL 1u          /* a refusal stops the sequence (after the step's ALT, if it has one) */
#define FMS_ALT   2u          /* the fall-back of the step before: sent only if that one was refused */

#define FMS_DATA_MAX 136
struct fm_ap_step {
    uint8_t  kind, flags;
    uint16_t len;             /* data bytes (CMD_DATA, IOV_DATA) */
    uint32_t cmd;             /* WLC_* (CMD_*) */
    const char *name;         /* the iovar (IOV_*) */
    uint32_t val;             /* CMD_INT, IOV_INT */
    uint8_t  data[FMS_DATA_MAX];
    const char *what;         /* for the log: what the step is for */
};

struct fm_ap_cfg {
    const char *ssid;         /* 1..32 bytes */
    unsigned channel;         /* 1..13 (2.4 GHz, 20 MHz) */
    int d11n;                 /* the firmware's chanspec encoding: 1 for the older 802.11n one (WLC_GET_VERSION 1) */
    const uint8_t *pmk;       /* WPA2-PSK: the 32-byte key (PBKDF2 of the passphrase and the SSID); NULL: open */
    const char *pass;         /* the passphrase too, for the older passphrase form of the key (the fall-back) */
    const char *country;      /* "GB", or NULL/"" for the firmware's default */
};

#define FM_AP_STEPS_MAX 24
/* The sequence for cfg into s (FM_AP_STEPS_MAX room): the number of steps, 0 if cfg is not usable */
unsigned fm_ap_steps(struct fm_ap_step *s, unsigned max, const struct fm_ap_cfg *cfg);
/* The chanspec of a 2.4 GHz 20 MHz channel in either encoding */
uint16_t fm_chanspec_2g(unsigned channel, int d11n);
/* "WLC_SET_AP", or "\"chanspec\"": how a step is named in the log and on the screen */
void fm_ap_step_name(const struct fm_ap_step *s, char *out, unsigned max);

/* Running a sequence: run() sends one step and returns 0 or the firmware's error; log() (may be NULL)
   hears each result. Returns 0, or the error of the step that stopped it, with its name and error in
   why ("WLC_SET_AP error -23"). A refused step without FMS_FATAL is only logged. */
int32_t fm_ap_run(const struct fm_ap_step *s, unsigned n,
                  int32_t (*run)(void *ctx, const struct fm_ap_step *st),
                  void (*log)(void *ctx, const struct fm_ap_step *st, int32_t e, int skipped),
                  void *ctx, char *why, unsigned whymax);

/* "idauth" in the firmware's "cap" list: it can be the authenticator of a WPA2 network (Linux's
   BRCMF_FEAT_FWAUTH, which is when brcmfmac offers the access point's 4-way handshake offload) */
int fm_caps_has(const char *caps, const char *word);

#endif
