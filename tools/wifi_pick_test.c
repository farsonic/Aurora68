/* SPDX-License-Identifier: GPL-2.0-only
 *
 * wifi_pick_test.c - the WiFi's choice of access point (emu68/src/alice/wifi/fmac.c, fm_pick_*) on the PC: one SSID
 * served by several BSSIDs, as in the owner's office (25 November 2026, docs/08 item 147: five access points, the SE
 * thrown off every few seconds). The strongest is chosen; one that disconnected us or could not be joined is skipped
 * for a while when another within 10 dB is there, and taken again when it is far better or its time is up; three
 * failed joins with a BSSID pinned leave the choice to the firmware; nothing under -80 dBm is chosen while a better one
 * was seen in the last minute (the scan is done again, twice, then the firmware chooses). Run by tools/wifi_pick_test.sh
 * from `make -C mac check`. GPL-2.0-only like the code it tests.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "fmac.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct fm_bss net(const char *ssid, uint8_t last, int rssi, unsigned ch)
{
    struct fm_bss b;
    memset(&b, 0, sizeof b);
    b.bssid[0] = 0x02; b.bssid[5] = last;
    b.ssid_len = (uint8_t)strlen(ssid);
    memcpy(b.ssid, ssid, b.ssid_len);
    b.rssi = (int16_t)rssi; b.channel = (uint8_t)ch; b.security = SEC_WPA2; b.pairwise = b.group_cipher = 4;
    return b;
}
static const char *const S = "office";

int main(void)
{
    struct fm_pick p;
    int why, best;
    uint32_t t = 1000;
    /* five access points of one SSID and two neighbours, as an office's scan */
    struct fm_bss sc[7] = {
        net(S, 0xa1, -68, 1), net(S, 0xa2, -43, 1), net(S, 0xa3, -72, 6), net(S, 0xa4, -78, 11), net(S, 0xa5, -50, 11),
        net("neighbour", 0xb1, -40, 6), net("officeX", 0xb2, -30, 1),
    };
    fm_pick_init(&p, -80);
    int k = fm_pick_choose(&p, sc, 7, S, 6, t, &why, &best);
    CHECK(k == 1 && why == FM_WHY_STRONGEST && best == 1, "the strongest of the five (-43 dBm), not a stronger neighbour or a longer name (%d, why %d)", k, why);

    /* it could not be joined: skipped for a minute, the next best (-50, within 10 dB) instead */
    fm_pick_kick(&p, sc[1].bssid, t, FM_KICK_FAIL_MS, FM_KICK_FAILED);
    fm_pick_result(&p, 1, 0);
    k = fm_pick_choose(&p, sc, 7, S, 6, t + 1000, &why, &best);
    CHECK(k == 4 && why == FM_WHY_SKIPPED_KICKED, "a BSSID we could not join is skipped for the next within 10 dB (%d, why %d)", k, why);
    /* that one disconnects us: the strongest (-43) could not be joined, so the next not skipped, -68, though 25 dB down */
    fm_pick_kick(&p, sc[4].bssid, t + 2000, FM_KICK_MS, FM_KICK_BY_AP);
    k = fm_pick_choose(&p, sc, 7, S, 6, t + 3000, &why, &best);
    CHECK(k == 0 && why == FM_WHY_SKIPPED_KICKED, "the strongest could not be joined and the next disconnected us: the -68 dBm one (%d, why %d)", k, why);
    /* after the kick runs out it is the strongest again, plainly */
    k = fm_pick_choose(&p, sc, 7, S, 6, t + 1000 + FM_KICK_FAIL_MS + 1, &why, &best);
    CHECK(k == 1 && why == FM_WHY_STRONGEST, "a minute later the BSSID that failed is taken again (%d, why %d)", k, why);
    CHECK(fm_pick_kicked(&p, sc[4].bssid, t + 2000 + FM_KICK_MS - 1) && !fm_pick_kicked(&p, sc[4].bssid, t + 2000 + FM_KICK_MS + 1), "a kick lasts 90 s");

    /* the owner's log: -68 dBm on channel 1 throws us off again and again; -72 and -78 are within 10 dB */
    fm_pick_init(&p, -80);
    struct fm_bss own[3] = { net(S, 0xa1, -68, 1), net(S, 0xa3, -72, 6), net(S, 0xa4, -78, 11) };
    k = fm_pick_choose(&p, own, 3, S, 6, t, &why, &best);
    CHECK(k == 0, "the -68 dBm one first");
    fm_pick_kick(&p, own[0].bssid, t + 5000, FM_KICK_MS, FM_KICK_BY_AP);
    k = fm_pick_choose(&p, own, 3, S, 6, t + 6000, &why, &best);
    CHECK(k == 1 && why == FM_WHY_SKIPPED_KICKED, "it disconnected us: the -72 dBm one on channel 6 (%d)", k);
    fm_pick_kick(&p, own[1].bssid, t + 9000, FM_KICK_MS, FM_KICK_BY_AP);
    k = fm_pick_choose(&p, own, 3, S, 6, t + 10000, &why, &best);
    CHECK(k == 2 && why == FM_WHY_SKIPPED_KICKED, "that one too: the -78 dBm one (10 dB down) (%d)", k);

    /* the owner's -43 dBm access point that answered every join with status 3: the -68 dBm one instead, 25 dB down */
    fm_pick_init(&p, -80);
    struct fm_bss strong[2] = { net(S, 0xa2, -43, 1), net(S, 0xa1, -68, 1) };
    fm_pick_kick(&p, strong[0].bssid, t, FM_KICK_FAIL_MS, FM_KICK_FAILED);
    k = fm_pick_choose(&p, strong, 2, S, 6, t + 1000, &why, &best);
    CHECK(k == 1 && why == FM_WHY_SKIPPED_KICKED, "a strong BSSID that could not be joined: any other above -80 dBm instead (%d, why %d)", k, why);
    fm_pick_kick(&p, strong[0].bssid, t, FM_KICK_MS, FM_KICK_BY_AP);
    k = fm_pick_choose(&p, strong, 2, S, 6, t + 1000, &why, &best);
    CHECK(k == 0 && why == FM_WHY_KICKED_ANYWAY, "but one that only disconnected us is kept when the next is 25 dB down (%d)", k);

    /* three failed pinned joins: the firmware chooses (an SSID-only join); its outcome starts the count again */
    fm_pick_init(&p, -80);
    for (int i = 0; i < 3; i++) fm_pick_result(&p, 1, 0);
    k = fm_pick_choose(&p, own, 3, S, 6, t, &why, &best);
    CHECK(k == FM_PICK_ANY && why == FM_WHY_FALLBACK && best == 0, "three failures in a row with a BSSID pinned: SSID only (%d, why %d)", k, why);
    fm_pick_result(&p, 0, 0);
    k = fm_pick_choose(&p, own, 3, S, 6, t, &why, &best);
    CHECK(k == 0, "then pinned again");
    fm_pick_result(&p, 1, 0); fm_pick_result(&p, 1, 1);
    CHECK(p.pinned_fails == 0, "a join that works clears the count");

    /* not seen: SSID only, as before (a hidden network) */
    k = fm_pick_choose(&p, sc + 5, 2, S, 6, t, &why, &best);
    CHECK(k == FM_PICK_ANY && why == FM_WHY_NOT_SEEN && best == -1, "the SSID not in the scan: SSID only");

    /* the floor: a scan that only heard a -82 dBm one while a -65 dBm one was seen half a minute ago */
    fm_pick_init(&p, -80);
    struct fm_bss good = net(S, 0xa2, -65, 1), weak = net(S, 0xa4, -82, 11);
    k = fm_pick_choose(&p, &good, 1, S, 6, t, &why, &best);
    CHECK(k == 0, "the -65 dBm one");
    k = fm_pick_choose(&p, &weak, 1, S, 6, t + 30000, &why, &best);
    CHECK(k == FM_PICK_RESCAN && why == FM_WHY_WEAK_RESCAN, "-82 dBm with -65 seen 30 s ago: scan again (%d)", k);
    k = fm_pick_choose(&p, &weak, 1, S, 6, t + 31000, &why, &best);
    CHECK(k == FM_PICK_RESCAN, "and again");
    k = fm_pick_choose(&p, &weak, 1, S, 6, t + 32000, &why, &best);
    CHECK(k == FM_PICK_ANY && why == FM_WHY_WEAK_ANY, "then the firmware chooses (%d, why %d)", k, why);
    k = fm_pick_choose(&p, &weak, 1, S, 6, t + 30000 + FM_SEEN_MS + 1000, &why, &best);
    CHECK(k == 0 && why == FM_WHY_WEAK_ONLY, "a minute on, the weak one is all there is: taken (%d, why %d)", k, why);
    k = fm_pick_choose(&p, &good, 1, S, 6, t + 100000, &why, &best);
    CHECK(k == 0 && why == FM_WHY_STRONGEST && p.weak_rescans == 0, "a good one again: plainly");

    /* the escan dwell */
    {
        uint8_t b[80];
        unsigned n = fm_escan_params_dwell(b, 7, 80);
        CHECK(n == 72 && fm_le32(b + 8 + 48) == 80 && fm_le16(b + 6) == 7, "escan with an 80 ms active dwell per channel");
        fm_escan_params(b, 8);
        CHECK(fm_le32(b + 8 + 48) == 0xffffffffu, "the plain escan leaves the dwell to the firmware (40 ms)");
    }
    CHECK(!strcmp(fm_status_word(E_STATUS_ABORT), "aborted") && !strcmp(fm_reason_word(8), "leaving the BSS"), "the words");

    printf("wifi_pick_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
