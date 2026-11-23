/* SPDX-License-Identifier: MPL-2.0
 *
 * apnet_test.c - Setup and Recovery's access point on the host (23 November 2026, docs/08 item 117): the DHCP
 * server and DNS answerer (emu68/src/psmac/psmac_apnet.c), the firmware's SoftAP command sequence
 * (emu68/src/alice/wifi/fmac_ap.c, with fmac.c's WPA key) and how a refused step is reported. The real
 * firmware cannot be tried here; what can be is that the bytes and the order are the ones intended.
 * Built and run by tools/apnet_test.sh, from `make -C mac check`.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "psmac_apnet.h"
#include "fmac.h"
#include "fmac_ap.h"

static int checks, failed;
#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const uint8_t AP_IP[4] = { 192, 168, 4, 1 };

/* a client's DHCP message */
static uint32_t dhcp_msg(uint8_t *m, int type, const uint8_t hw[6], uint32_t xid, const uint8_t *req, const uint8_t *server, const uint8_t *ciaddr, int bcast)
{
    memset(m, 0, 300);
    m[0] = 1; m[1] = 1; m[2] = 6;
    m[4] = (uint8_t)(xid >> 24); m[5] = (uint8_t)(xid >> 16); m[6] = (uint8_t)(xid >> 8); m[7] = (uint8_t)xid;
    if (bcast) m[10] = 0x80;
    if (ciaddr) memcpy(m + 12, ciaddr, 4);
    memcpy(m + 28, hw, 6);
    m[236] = 0x63; m[237] = 0x82; m[238] = 0x53; m[239] = 0x63;
    uint8_t *o = m + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
    if (req) { *o++ = 50; *o++ = 4; memcpy(o, req, 4); o += 4; }
    if (server) { *o++ = 54; *o++ = 4; memcpy(o, server, 4); o += 4; }
    *o++ = 12; *o++ = 5; memcpy(o, "phone", 5); o += 5;
    *o++ = 255;
    return 300;
}
/* an answer's type and an option's value */
static int opt(const uint8_t *r, uint32_t n, int t, uint8_t *v, int vmax)
{
    for (uint32_t k = 240; k + 1 < n && r[k] != 255; ) {
        if (r[k] == 0) { k++; continue; }
        if (r[k] == t) { int l = r[k + 1]; if (v) memcpy(v, r + k + 2, l < vmax ? l : vmax); return l; }
        k += 2u + r[k + 1];
    }
    return -1;
}
static int type_of(const uint8_t *r, uint32_t n) { uint8_t v = 0; return opt(r, n, 53, &v, 1) == 1 ? v : 0; }

static void test_dhcp(void)
{
    struct apn a; apn_init(&a, AP_IP, 10, 600);
    uint8_t m[300], r[600], to_ip[4], to_hw[6]; int bc;
    const uint8_t hw1[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x01 }, hw2[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x02 };
    /* DISCOVER -> OFFER of .10, to the client's hardware address (no broadcast flag) */
    dhcp_msg(m, 1, hw1, 0x1234, 0, 0, 0, 0);
    uint32_t n = apn_dhcp(&a, 1000, m, 300, r, sizeof r, &bc, to_ip, to_hw);
    CHECK(n >= 300 && type_of(r, n) == 2, "DISCOVER answered with an OFFER (got %u bytes, type %d)", n, type_of(r, n));
    CHECK(r[0] == 2 && r[4] == 0 && r[5] == 0 && r[6] == 0x12 && r[7] == 0x34, "the OFFER is a BOOTREPLY with the client's xid");
    CHECK(r[16] == 192 && r[17] == 168 && r[18] == 4 && r[19] == 10, "the first address offered is 192.168.4.10 (got %u.%u.%u.%u)", r[16], r[17], r[18], r[19]);
    CHECK(!bc && !memcmp(to_hw, hw1, 6) && to_ip[3] == 10, "without the broadcast flag the OFFER goes to the client's own hardware address");
    uint8_t v[8];
    CHECK(opt(r, n, 54, v, 4) == 4 && !memcmp(v, AP_IP, 4), "server identifier 192.168.4.1");
    CHECK(opt(r, n, 3, v, 4) == 4 && !memcmp(v, AP_IP, 4), "router 192.168.4.1");
    CHECK(opt(r, n, 6, v, 4) == 4 && !memcmp(v, AP_IP, 4), "DNS server 192.168.4.1");
    CHECK(opt(r, n, 1, v, 4) == 4 && v[0] == 255 && v[3] == 0, "mask 255.255.255.0");
    CHECK(opt(r, n, 51, v, 4) == 4 && v[2] == 0x02 && v[3] == 0x58, "lease 600 s");
    /* REQUEST for it -> ACK */
    const uint8_t ip10[4] = { 192, 168, 4, 10 };
    dhcp_msg(m, 3, hw1, 0x1235, ip10, AP_IP, 0, 1);
    n = apn_dhcp(&a, 1001, m, 300, r, sizeof r, &bc, to_ip, to_hw);
    CHECK(n && type_of(r, n) == 5 && r[19] == 10, "REQUEST of the offer answered with an ACK for .10");
    CHECK(bc, "with the broadcast flag the ACK is broadcast");
    CHECK(apn_clients(&a, 1002) == 1, "one client holds a lease");
    /* the same client again: the same address */
    dhcp_msg(m, 1, hw1, 0x2000, 0, 0, 0, 0);
    n = apn_dhcp(&a, 1100, m, 300, r, sizeof r, &bc, to_ip, to_hw);
    CHECK(n && r[19] == 10, "a client that comes back is offered its address again");
    /* a second client: .11 */
    dhcp_msg(m, 1, hw2, 0x3000, 0, 0, 0, 1);
    n = apn_dhcp(&a, 1101, m, 300, r, sizeof r, &bc, to_ip, to_hw);
    CHECK(n && r[19] == 11 && bc, "a second client is offered .11, broadcast as it asked");
    /* the second client chose another server: no answer, and its offer goes */
    const uint8_t other[4] = { 10, 0, 0, 1 }, ip11[4] = { 192, 168, 4, 11 };
    dhcp_msg(m, 3, hw2, 0x3001, ip11, other, 0, 0);
    n = apn_dhcp(&a, 1102, m, 300, r, sizeof r, &bc, to_ip, to_hw);
    CHECK(n == 0, "a REQUEST naming another server is not answered");
    /* a phone that was last on a home network asks for its old address: NAK, broadcast */
    const uint8_t home[4] = { 192, 168, 1, 20 };
    dhcp_msg(m, 3, hw2, 0x3002, home, 0, 0, 0);
    n = apn_dhcp(&a, 1103, m, 300, r, sizeof r, &bc, to_ip, to_hw);
    CHECK(n && type_of(r, n) == 6 && bc, "a REQUEST for another network's address is NAKed, by broadcast");
    CHECK(r[16] == 0 && r[19] == 0, "a NAK carries no address");
    /* asking for another client's address: NAK */
    dhcp_msg(m, 3, hw2, 0x3003, ip10, 0, 0, 0);
    n = apn_dhcp(&a, 1104, m, 300, r, sizeof r, &bc, to_ip, to_hw);
    CHECK(n && type_of(r, n) == 6, "a REQUEST for an address another client holds is NAKed");
    /* INIT-REBOOT with a free address of the pool (the Pi restarted, the phone remembers .12): ACK */
    const uint8_t ip12[4] = { 192, 168, 4, 12 };
    dhcp_msg(m, 3, hw2, 0x3004, ip12, 0, 0, 0);
    n = apn_dhcp(&a, 1105, m, 300, r, sizeof r, &bc, to_ip, to_hw);
    CHECK(n && type_of(r, n) == 5 && r[19] == 12, "a REQUEST for a free address of the pool is granted (after the Pi restarted)");
    /* renewal from the client's address: unicast to ciaddr */
    dhcp_msg(m, 3, hw2, 0x3005, 0, 0, ip12, 0);
    n = apn_dhcp(&a, 1300, m, 300, r, sizeof r, &bc, to_ip, to_hw);
    CHECK(n && type_of(r, n) == 5 && !bc && !memcmp(to_ip, ip12, 4), "a renewal is ACKed to the client's own address");
    /* RELEASE */
    dhcp_msg(m, 7, hw2, 0x3006, 0, AP_IP, ip12, 0);
    n = apn_dhcp(&a, 1301, m, 300, r, sizeof r, &bc, to_ip, to_hw);
    CHECK(n == 0 && apn_clients(&a, 1302) == 1, "RELEASE gives the lease back without an answer");
    /* INFORM */
    dhcp_msg(m, 8, hw2, 0x3007, 0, 0, ip12, 0);
    n = apn_dhcp(&a, 1303, m, 300, r, sizeof r, &bc, to_ip, to_hw);
    CHECK(n && type_of(r, n) == 5 && r[16] == 0 && opt(r, n, 51, 0, 0) < 0, "INFORM is ACKed with the options and no lease");
    /* the pool: sixteen leases, the seventeenth client gets nothing */
    struct apn b; apn_init(&b, AP_IP, 10, 600);
    int offered = 0, last = -1;
    for (int c = 0; c < 17; c++) {
        uint8_t hw[6] = { 0x02, 0, 0, 0, 0x10, (uint8_t)c };
        dhcp_msg(m, 1, hw, 0x4000u + (uint32_t)c, 0, 0, 0, 0);
        n = apn_dhcp(&b, 5000, m, 300, r, sizeof r, &bc, to_ip, to_hw);
        if (n) { offered++; last = r[19]; }
    }
    CHECK(offered == 16 && last == 25, "sixteen offers from .10 to .25, none for a seventeenth (got %d, last .%d)", offered, last);
    /* offers lapse after a minute */
    uint8_t hwx[6] = { 0x02, 0, 0, 0, 0x20, 1 };
    dhcp_msg(m, 1, hwx, 0x5000, 0, 0, 0, 0);
    n = apn_dhcp(&b, 5061, m, 300, r, sizeof r, &bc, to_ip, to_hw);
    CHECK(n && r[19] == 10, "an offer nobody took is free again after a minute (got .%d)", n ? r[19] : -1);
    /* rubbish */
    uint8_t junk[300]; memset(junk, 0, sizeof junk);
    CHECK(apn_dhcp(&a, 1, junk, 300, r, sizeof r, &bc, to_ip, to_hw) == 0, "a message without the magic cookie is ignored");
    dhcp_msg(m, 1, hw1, 1, 0, 0, 0, 0); m[0] = 2;
    CHECK(apn_dhcp(&a, 1, m, 300, r, sizeof r, &bc, to_ip, to_hw) == 0, "a BOOTREPLY is ignored");
    dhcp_msg(m, 1, hw1, 1, 0, 0, 0, 0);
    CHECK(apn_dhcp(&a, 1, m, 239, r, sizeof r, &bc, to_ip, to_hw) == 0, "a short message is ignored");
}

static uint32_t dns_q(uint8_t *q, const char *name, uint16_t type, uint16_t flags)
{
    memset(q, 0, 12);
    q[0] = 0xab; q[1] = 0xcd; q[2] = (uint8_t)(flags >> 8); q[3] = (uint8_t)flags; q[5] = 1;
    uint32_t p = 12;
    for (const char *s = name; *s; ) {
        const char *e = strchr(s, '.'); if (!e) e = s + strlen(s);
        q[p++] = (uint8_t)(e - s); memcpy(q + p, s, (size_t)(e - s)); p += (uint32_t)(e - s);
        s = *e ? e + 1 : e;
    }
    q[p++] = 0; q[p++] = (uint8_t)(type >> 8); q[p++] = (uint8_t)type; q[p++] = 0; q[p++] = 1;
    return p;
}
static void test_dns(void)
{
    struct apn a; apn_init(&a, AP_IP, 10, 600);
    uint8_t q[600], r[600];
    uint32_t qn = dns_q(q, "connectivitycheck.gstatic.com", 1, 0x0100);
    uint32_t n = apn_dns(&a, q, qn, r, sizeof r);
    CHECK(n == qn + 16, "an A query is answered with one record (%u bytes, query %u)", n, qn);
    CHECK(r[0] == 0xab && r[1] == 0xcd && r[2] == 0x85 && r[3] == 0x00, "id kept; answer, authoritative, RD echoed, no error (flags %02x%02x)", r[2], r[3]);
    CHECK(r[7] == 1 && !memcmp(r + n - 4, AP_IP, 4) && r[qn] == 0xc0 && r[qn + 1] == 12, "the record points at the question's name and holds 192.168.4.1");
    CHECK(r[qn + 6] == 0 && r[qn + 7] == 0 && r[qn + 8] == 0 && r[qn + 9] == 10, "TTL ten seconds");
    qn = dns_q(q, "captive.apple.com", 28, 0x0100);
    n = apn_dns(&a, q, qn, r, sizeof r);
    CHECK(n == qn && r[7] == 0 && (r[3] & 15) == 0, "AAAA: no records, no error (so the phone uses IPv4)");
    qn = dns_q(q, "aurora68.local", 255, 0);
    n = apn_dns(&a, q, qn, r, sizeof r);
    CHECK(n == qn + 16, "ANY is answered with the A record");
    qn = dns_q(q, "x.example", 1, 0x8000);
    CHECK(apn_dns(&a, q, qn, r, sizeof r) == 0, "an answer is not answered");
    qn = dns_q(q, "x.example", 1, 0); q[5] = 2;
    CHECK(apn_dns(&a, q, qn, r, sizeof r) == 0, "two questions: not answered");
    qn = dns_q(q, "x.example", 1, 0); q[12] = 0xc0;
    CHECK(apn_dns(&a, q, qn, r, sizeof r) == 0, "a compressed name in a question: not answered");
    qn = dns_q(q, "x.example", 1, 0);
    CHECK(apn_dns(&a, q, qn - 3, r, sizeof r) == 0, "a cut query: not answered");
    CHECK(a.dns_queries == 3 && a.dns_answers == 2, "counted: %u queries, %u answered with an address", a.dns_queries, a.dns_answers);
}

/* ---- the firmware's sequence ---- */
static char order[1024];
static int fail_at = -1, run_no;
static int32_t fake_run(void *ctx, const struct fm_ap_step *s)
{
    (void)ctx;
    char nm[40]; fm_ap_step_name(s, nm, sizeof nm);
    strcat(order, nm); strcat(order, " ");
    return run_no++ == fail_at ? -23 : 0;
}
static void test_steps(void)
{
    /* the WPA key: IEEE 802.11i-2004 H.4.1's test vector, "password" on the network "IEEE" */
    uint8_t pmk[32];
    fm_wpa_pmk("password", 8, "IEEE", 4, pmk, 0);
    static const uint8_t want[32] = { 0xf4, 0x2c, 0x6f, 0xc5, 0x2d, 0xf0, 0xeb, 0xef, 0x9e, 0xbb, 0x4b, 0x90, 0xb3, 0x8a, 0x5f, 0x90,
                                      0x2e, 0x83, 0xfe, 0x1b, 0x13, 0x5a, 0x70, 0xe2, 0x3a, 0xed, 0x76, 0x2e, 0x97, 0x10, 0xa1, 0x2e };
    CHECK(!memcmp(pmk, want, 32), "PBKDF2-SHA1 WPA key matches IEEE 802.11i's test vector");
    CHECK(fm_chanspec_2g(6, 0) == 0x1006 && fm_chanspec_2g(6, 1) == 0x2b06 && fm_chanspec_2g(11, 0) == 0x100b, "chanspec of channel 6: d11ac 0x1006, d11n 0x2b06");

    struct fm_ap_step s[FM_AP_STEPS_MAX];
    struct fm_ap_cfg c = { "Aurora68-4946", 6, 0, pmk, "abcdefghjk", "GB" };
    unsigned n = fm_ap_steps(s, FM_AP_STEPS_MAX, &c);
    CHECK(n > 10, "a WPA2 sequence (%u steps)", n);
    char why[96];
    order[0] = 0; fail_at = -1; run_no = 0;
    int32_t e = fm_ap_run(s, n, fake_run, 0, 0, why, sizeof why);
    CHECK(e == 0 && !why[0], "the sequence runs to its end when nothing is refused");
    const char *wpa2 = "\"event_msgs\" \"sup_wpa\" \"mpc\" \"apsta\" WLC_DOWN \"country\" WLC_SET_BCNPRD WLC_SET_DTIMPRD WLC_SET_INFRA WLC_SET_AP \"chanspec\" "
                       "WLC_UP WLC_SET_WSEC_PMK \"auth\" \"wsec\" \"wpa_auth\" \"mfp\" WLC_SET_SSID \"closednet\" ";
    CHECK(!strcmp(order, wpa2), "WPA2 order as Linux's brcmf_cfg80211_start_ap:\n  got  %s\n  want %s", order, wpa2);
    /* the values that matter */
    for (unsigned i = 0; i < n; i++) {
        char nm[40]; fm_ap_step_name(&s[i], nm, sizeof nm);
        if (!strcmp(nm, "WLC_SET_AP")) CHECK(s[i].val == 1 && (s[i].flags & FMS_FATAL), "WLC_SET_AP 1, fatal");
        if (!strcmp(nm, "WLC_SET_INFRA")) CHECK(s[i].val == 1, "WLC_SET_INFRA 1");
        if (!strcmp(nm, "\"mpc\"")) CHECK(s[i].val == 0 && !(s[i].flags & FMS_FATAL), "mpc 0, not fatal");
        if (!strcmp(nm, "\"chanspec\"")) CHECK(s[i].val == 0x1006, "chanspec 0x1006");
        if (!strcmp(nm, "\"wsec\"")) CHECK(s[i].val == WSEC_AES, "wsec AES");
        if (!strcmp(nm, "\"wpa_auth\"")) CHECK(s[i].val == WPA2_AUTH_PSK, "wpa_auth WPA2_AUTH_PSK (0x80)");
        if (s[i].kind == FMS_CMD_DATA && s[i].cmd == WLC_SET_WSEC_PMK && !(s[i].flags & FMS_ALT))
            CHECK(s[i].len == 132 && s[i].data[0] == 32 && s[i].data[1] == 0 && s[i].data[2] == 0 && s[i].data[3] == 0 && !memcmp(s[i].data + 4, pmk, 32),
                  "WLC_SET_WSEC_PMK: key_len 32, flags 0, the key itself");
        if (s[i].kind == FMS_CMD_DATA && s[i].cmd == WLC_SET_WSEC_PMK && (s[i].flags & FMS_ALT))
            CHECK(s[i].data[0] == 10 && s[i].data[2] == 1 && !memcmp(s[i].data + 4, "abcdefghjk", 10), "the fall-back: the passphrase, flags 1 (WSEC_PASSPHRASE)");
        if (s[i].kind == FMS_CMD_DATA && s[i].cmd == WLC_SET_SSID) {
            static const uint8_t zero6[6];
            CHECK(s[i].len == 52 && s[i].data[0] == 13 && !memcmp(s[i].data + 4, "Aurora68-4946", 13) && !memcmp(s[i].data + 36, zero6, 6),
                  "WLC_SET_SSID: 52 bytes, the SSID's length and name, the BSSID left zero (as Linux clears it)");
        }
        if (s[i].kind == FMS_IOV_DATA && !strcmp(s[i].name, "event_msgs"))
            CHECK(s[i].len == EVENT_MASK_LEN && (s[i].data[E_ASSOC_IND / 8] & (1 << (E_ASSOC_IND % 8))) && (s[i].data[E_DISASSOC_IND / 8] & (1 << (E_DISASSOC_IND % 8))),
                  "the event mask has a station's association and departure");
    }
    /* refusals */
    order[0] = 0; run_no = 0; fail_at = 9;                      /* the tenth step sent: WLC_SET_AP */
    e = fm_ap_run(s, n, fake_run, 0, 0, why, sizeof why);
    CHECK(e == -23 && !strcmp(why, "WLC_SET_AP error -23"), "WLC_SET_AP refused stops the sequence with \"%s\"", why);
    order[0] = 0; run_no = 0; fail_at = 10;                     /* "chanspec": its fall-back WLC_SET_CHANNEL is sent */
    e = fm_ap_run(s, n, fake_run, 0, 0, why, sizeof why);
    CHECK(e == 0 && strstr(order, "\"chanspec\" WLC_SET_CHANNEL WLC_UP"), "a refused chanspec falls back to WLC_SET_CHANNEL (%s)", order);
    order[0] = 0; run_no = 0; fail_at = 2;                      /* "mpc": not fatal */
    e = fm_ap_run(s, n, fake_run, 0, 0, why, sizeof why);
    CHECK(e == 0, "a refused mpc is only logged");
    order[0] = 0; run_no = 0; fail_at = 12;                     /* the binary key: the passphrase form is tried */
    e = fm_ap_run(s, n, fake_run, 0, 0, why, sizeof why);
    CHECK(e == 0 && strstr(order, "WLC_SET_WSEC_PMK WLC_SET_WSEC_PMK \"auth\""), "a refused binary key falls back to the passphrase (%s)", order);
    /* no answer at all */
    {
        struct fm_ap_step one[1]; memset(one, 0, sizeof one); one[0].kind = FMS_CMD_INT; one[0].cmd = WLC_UP; one[0].flags = FMS_FATAL;
        order[0] = 0; run_no = 0; fail_at = 0;
        e = fm_ap_run(one, 1, fake_run, 0, 0, why, sizeof why);
        CHECK(!strcmp(why, "WLC_UP error -23"), "one refused step: %s", why);
    }
    /* open */
    c.pmk = 0; c.pass = 0; c.country = 0;
    n = fm_ap_steps(s, FM_AP_STEPS_MAX, &c);
    order[0] = 0; run_no = 0; fail_at = -1;
    fm_ap_run(s, n, fake_run, 0, 0, why, sizeof why);
    const char *open_ = "\"event_msgs\" \"sup_wpa\" \"mpc\" \"apsta\" WLC_DOWN WLC_SET_BCNPRD WLC_SET_DTIMPRD WLC_SET_INFRA WLC_SET_AP \"chanspec\" "
                        "WLC_UP \"auth\" \"wsec\" \"wpa_auth\" WLC_SET_SSID \"closednet\" ";
    CHECK(!strcmp(order, open_), "the open sequence:\n  got  %s\n  want %s", order, open_);
    for (unsigned i = 0; i < n; i++) if (s[i].kind == FMS_IOV_INT && (!strcmp(s[i].name, "wsec") || !strcmp(s[i].name, "wpa_auth"))) CHECK(s[i].val == 0, "open: %s 0", s[i].name);
    /* d11n chanspec */
    c.d11n = 1; n = fm_ap_steps(s, FM_AP_STEPS_MAX, &c);
    for (unsigned i = 0; i < n; i++) if (s[i].kind == FMS_IOV_INT && !strcmp(s[i].name, "chanspec")) CHECK(s[i].val == 0x2b06, "d11n firmware: chanspec 0x2b06");
    /* unusable */
    c.channel = 14; CHECK(fm_ap_steps(s, FM_AP_STEPS_MAX, &c) == 0, "channel 14 refused");
    c.channel = 6; c.ssid = ""; CHECK(fm_ap_steps(s, FM_AP_STEPS_MAX, &c) == 0, "no SSID refused");
    /* the firmware's feature words */
    CHECK(fm_caps_has("ap sta wme 802.11d idsup idauth mfp", "idauth"), "idauth found");
    CHECK(!fm_caps_has("ap sta wme idauthx", "idauth") && !fm_caps_has("ap sta", "idauth") && fm_caps_has("idauth", "idauth"), "only the whole word idauth");
}

int main(void)
{
    test_dhcp();
    test_dns();
    test_steps();
    printf("apnet_test: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}
