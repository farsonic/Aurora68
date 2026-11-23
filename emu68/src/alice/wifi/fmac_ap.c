/* SPDX-License-Identifier: GPL-2.0-only
 * fmac_ap.c - the FullMAC firmware as an access point (see fmac_ap.h). Aurora68's addition, 23 November
 * 2026 (docs/08 item 118). Host-independent, like fmac.c.
 *
 * The order is Linux brcmfmac's brcmf_cfg80211_start_ap for one interface (no "mbss", no RSDB), read
 * from cfg80211.c and written here from that reading:
 *   the event mask (an access point's own events added: a station associated, left);
 *   "mpc" 0 (the firmware's minimum power consumption mode would switch the radio off with no
 *   association of its own), "apsta" 0, the beacon period and the DTIM period;
 *   WLC_DOWN, WLC_SET_INFRA 1, WLC_SET_AP 1, the channel ("chanspec"; Linux notes that firmware 10.x
 *   wants it after the AP role and before up), WLC_UP;
 *   WPA2-PSK: WLC_SET_WSEC_PMK with the 32-byte key (brcmf_set_pmk: key_len 32, flags 0), then "auth" 0,
 *   "wsec" AES, "wpa_auth" WPA2_AUTH_PSK, "mfp" none (brcmf_configure_wpaie); open: "wsec" 0 and
 *   "wpa_auth" 0 (brcmf_configure_opensecurity);
 *   WLC_SET_SSID with join parameters that hold only the SSID (Linux clears the rest): the network
 *   starts; "closednet" 0, the SSID in the beacons.
 * Fall-backs that Linux does not need but a firmware we have not seen might: WLC_SET_CHANNEL with the
 * plain number when "chanspec" is refused, and the passphrase form of the key (flags 1, what the
 * station side's older firmware took) when the binary key is refused.
 */
#include "fmac.h"
#include "fmac_ap.h"

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);

static unsigned sl(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }

uint16_t fm_chanspec_2g(unsigned ch, int d11n)
{
    /* d11ac: band 2G 0x0000, bandwidth 20 0x1000; d11n: band 2G 0x2000, bandwidth 20 0x1000, no sideband 0x0300 */
    return (uint16_t)((ch & 0xff) | (d11n ? 0x2b00u : 0x1000u));
}

static struct fm_ap_step *add(struct fm_ap_step *s, unsigned *n, unsigned max, uint8_t kind, uint8_t flags, const char *what)
{
    if (*n >= max) return 0;
    struct fm_ap_step *t = &s[(*n)++];
    memset(t, 0, sizeof *t);
    t->kind = kind; t->flags = flags; t->what = what;
    return t;
}
#define CMD_INT(c, v, f, w) do { struct fm_ap_step *t_ = add(s, &n, max, FMS_CMD_INT, f, w); if (!t_) return 0; t_->cmd = c; t_->val = v; } while (0)
#define IOV_INT(nm, v, f, w) do { struct fm_ap_step *t_ = add(s, &n, max, FMS_IOV_INT, f, w); if (!t_) return 0; t_->name = nm; t_->val = v; } while (0)

unsigned fm_ap_steps(struct fm_ap_step *s, unsigned max, const struct fm_ap_cfg *c)
{
    unsigned n = 0, ssl = sl(c->ssid);
    if (!ssl || ssl > 32 || c->channel < 1 || c->channel > 13) return 0;
    struct fm_ap_step *t;
    /* the events: the station's set and an access point's */
    if (!(t = add(s, &n, max, FMS_IOV_DATA, 0, "the events an access point is told about"))) return 0;
    t->name = "event_msgs"; t->len = EVENT_MASK_LEN;
    static const uint8_t evs[] = { E_SET_SSID, E_LINK, E_AUTH, E_AUTH_IND, E_DEAUTH, E_DEAUTH_IND, E_ASSOC, E_ASSOC_IND,
                                   E_REASSOC_IND, E_DISASSOC, E_DISASSOC_IND, E_PSK_SUP, E_ESCAN_RESULT, E_IF };
    for (unsigned i = 0; i < sizeof evs; i++) fm_event_mask_set(t->data, evs[i]);
    IOV_INT("sup_wpa", 0, 0, "the station's own supplicant off");
    IOV_INT("mpc", 0, 0, "no minimum-power mode: the radio stays on with no association of its own");
    IOV_INT("apsta", 0, 0, "an access point only, no station beside it");
    CMD_INT(WLC_DOWN, 1, FMS_FATAL, "down, to change the role");
    if (c->country && c->country[0] && c->country[1]) {
        if (!(t = add(s, &n, max, FMS_IOV_DATA, 0, "the country (its channels and power)"))) return 0;
        t->name = "country"; t->len = (uint16_t)fm_country(t->data, c->country);
    }
    CMD_INT(WLC_SET_BCNPRD, 100, 0, "a beacon every 100 TU");
    CMD_INT(WLC_SET_DTIMPRD, 2, 0, "DTIM every second beacon");
    CMD_INT(WLC_SET_INFRA, 1, FMS_FATAL, "infrastructure mode");
    CMD_INT(WLC_SET_AP, 1, FMS_FATAL, "the access point role");
    IOV_INT("chanspec", fm_chanspec_2g(c->channel, c->d11n), FMS_FATAL, "the channel");
    CMD_INT(WLC_SET_CHANNEL, c->channel, FMS_FATAL | FMS_ALT, "the channel, the older way");
    CMD_INT(WLC_UP, 1, FMS_FATAL, "up");
    if (c->pmk) {
        if (!(t = add(s, &n, max, FMS_CMD_DATA, FMS_FATAL, "the WPA2 key (the firmware is the authenticator)"))) return 0;
        t->cmd = WLC_SET_WSEC_PMK; t->len = 132;
        fm_put16(t->data, 32); fm_put16(t->data + 2, 0);          /* key_len 32, flags 0: the key itself */
        memcpy(t->data + 4, c->pmk, 32);
        if (c->pass && sl(c->pass) >= 8 && sl(c->pass) <= 63) {
            if (!(t = add(s, &n, max, FMS_CMD_DATA, FMS_FATAL | FMS_ALT, "the WPA2 key as a passphrase"))) return 0;
            t->cmd = WLC_SET_WSEC_PMK; t->len = (uint16_t)fm_pmk(t->data, c->pass, sl(c->pass), 0);
        }
        IOV_INT("auth", 0, 0, "open system authentication (WPA2 does the rest)");
        IOV_INT("wsec", WSEC_AES, FMS_FATAL, "AES (CCMP)");
        IOV_INT("wpa_auth", WPA2_AUTH_PSK, FMS_FATAL, "WPA2-PSK");
        IOV_INT("mfp", MFP_NONE, 0, "no management frame protection");
    } else {
        IOV_INT("auth", 0, 0, "open system authentication");
        IOV_INT("wsec", WSEC_NONE, FMS_FATAL, "no encryption");
        IOV_INT("wpa_auth", WPA_AUTH_DISABLED, FMS_FATAL, "no WPA");
    }
    if (!(t = add(s, &n, max, FMS_CMD_DATA, FMS_FATAL, "the network's name: the access point starts"))) return 0;
    t->cmd = WLC_SET_SSID;
    {
        static const uint8_t none[6];
        t->len = (uint16_t)fm_ssid_params(t->data, c->ssid, ssl, none);   /* only the SSID, as Linux */
    }
    IOV_INT("closednet", 0, 0, "the name in the beacons");
    return n;
}

static const char *cmd_name(uint32_t c)
{
    switch (c) {
    case WLC_UP: return "WLC_UP";
    case WLC_DOWN: return "WLC_DOWN";
    case WLC_SET_INFRA: return "WLC_SET_INFRA";
    case WLC_SET_SSID: return "WLC_SET_SSID";
    case WLC_SET_CHANNEL: return "WLC_SET_CHANNEL";
    case WLC_SET_BCNPRD: return "WLC_SET_BCNPRD";
    case WLC_SET_DTIMPRD: return "WLC_SET_DTIMPRD";
    case WLC_SET_AP: return "WLC_SET_AP";
    case WLC_SET_WSEC_PMK: return "WLC_SET_WSEC_PMK";
    default: return "WLC_?";
    }
}
void fm_ap_step_name(const struct fm_ap_step *s, char *out, unsigned max)
{
    unsigned k = 0;
    if (!max) return;
    if (s->kind == FMS_CMD_INT || s->kind == FMS_CMD_DATA) {
        for (const char *p = cmd_name(s->cmd); *p && k + 1 < max; p++) out[k++] = *p;
    } else {
        if (k + 1 < max) out[k++] = '"';
        for (const char *p = s->name; p && *p && k + 1 < max; p++) out[k++] = *p;
        if (k + 1 < max) out[k++] = '"';
    }
    out[k] = 0;
}

static unsigned put_int(char *o, unsigned k, unsigned max, int32_t v)
{
    char d[12]; unsigned n = 0; uint32_t u = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    if (v < 0 && k + 1 < max) o[k++] = '-';
    do { d[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < sizeof d);
    while (n && k + 1 < max) o[k++] = d[--n];
    return k;
}
int32_t fm_ap_run(const struct fm_ap_step *s, unsigned n, int32_t (*run)(void *, const struct fm_ap_step *),
                  void (*log)(void *, const struct fm_ap_step *, int32_t, int), void *ctx, char *why, unsigned whymax)
{
    int32_t prev = 0;
    if (why && whymax) why[0] = 0;
    for (unsigned i = 0; i < n; i++) {
        if ((s[i].flags & FMS_ALT) && !prev) { if (log) log(ctx, &s[i], 0, 1); continue; }   /* the step before was taken */
        int32_t e = run(ctx, &s[i]);
        if (log) log(ctx, &s[i], e, 0);
        int has_alt = i + 1 < n && (s[i + 1].flags & FMS_ALT);
        if (e && (s[i].flags & FMS_FATAL) && !has_alt) {
            if (why && whymax) {
                fm_ap_step_name(&s[i], why, whymax);
                unsigned k = sl(why);
                const char *t = e <= -100 ? ": no answer from the chip (" : " error ";
                while (*t && k + 1 < whymax) why[k++] = *t++;
                k = put_int(why, k, whymax, e);
                if (e <= -100 && k + 1 < whymax) why[k++] = ')';
                why[k] = 0;
            }
            return e;
        }
        prev = e;
    }
    return 0;
}

int fm_caps_has(const char *caps, const char *word)
{
    unsigned wl = sl(word);
    for (const char *p = caps; p && *p; p++) {
        if (p != caps && p[-1] != ' ') continue;
        unsigned k = 0; while (k < wl && p[k] == word[k]) k++;
        if (k == wl && (p[k] == ' ' || p[k] == 0 || p[k] == '\n')) return 1;
    }
    return 0;
}
