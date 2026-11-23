/* SPDX-License-Identifier: MPL-2.0
 *
 * diag_test.c - Setup and Recovery's options table, its validator, the one-shot debug start's merge and the
 * report bundle's scrubbing (emu68/src/psmac/psmac_diag.c, docs/08 item 120), on the host. Run by
 * tools/diag_test.sh from `make -C mac check`.
 *
 * Every option: each of its plain values accepted (in any case), its example accepted, nonsense refused, at
 * least one board using it, a group and a line of text, no key twice, the testbed's options absent. The
 * board filter: the counts per board and group printed (the page's), and the options that must or must not
 * show on each board checked. The one-shot words: allowed, refused for a key that is not a debug option, a
 * bare word, a bad value, two lines. The merge: a key replaced in place, new ones at the end, bad words
 * skipped and named. The scrub: every kind of secret as "(hidden)", literal copies too, the rest untouched.
 */
#include <stdio.h>
#include <string.h>
#include "psmac_diag.h"

static int fails, checks;
static void ok(int c, const char *what) { checks++; if (!c) { fails++; printf("FAIL: %s\n", what); } }

static uint32_t L(const char *s) { return (uint32_t)strlen(s); }
static int board_has(const char *key, int board) { return pdg_uses(pdg_find(key), board); }

int main(void)
{
    char why[200], out[4096];
    /* ---- the table ---- */
    for (unsigned i = 0; i < pdg_nopts; i++) {
        const struct pdg_opt *o = &pdg_opts[i];
        char t[300];
        snprintf(t, sizeof t, "%s: a group, boards, a text", o->key);
        ok(o->group < PDG_GROUPS && o->boards && o->text && strlen(o->text) > 10 && o->values && o->values[0], t);
        for (unsigned j = 0; j < i; j++) { snprintf(t, sizeof t, "%s only once", o->key); ok(strcmp(o->key, pdg_opts[j].key) != 0, t); }
        snprintf(t, sizeof t, "%s found", o->key); ok(pdg_find(o->key) == o, t);
        /* each plain alternative, and in capitals */
        const char *a = o->values;
        while (*a) {
            const char *e = a; while (*e && *e != '|') e++;
            char v[64]; unsigned n = (unsigned)(e - a); if (n > 63) n = 63;
            memcpy(v, a, n); v[n] = 0;
            if (!strpbrk(v, "#%*")) {
                snprintf(t, sizeof t, "%s=%s accepted", o->key, v); ok(pdg_value_ok(o, v, why, sizeof why), t);
                for (unsigned k = 0; k < n; k++) if (v[k] >= 'a' && v[k] <= 'z') v[k] = (char)(v[k] - 32);
                snprintf(t, sizeof t, "%s=%s accepted", o->key, v); ok(pdg_value_ok(o, v, why, sizeof why), t);
            }
            a = *e ? e + 1 : e;
        }
        if (o->example && !strstr(o->example, "A.B.C.D") && !strstr(o->example, "XX")) { snprintf(t, sizeof t, "%s's example %s accepted", o->key, o->example); ok(pdg_value_ok(o, o->example, why, sizeof why), t); }
        if (!strchr(o->values, '*')) {
            snprintf(t, sizeof t, "%s=nonsense? refused", o->key);
            ok(!pdg_value_ok(o, "nonsense?", why, sizeof why) && strstr(why, o->key) == why, t);
            ok(!pdg_value_ok(o, "", why, sizeof why), "an empty value refused");
        }
        if (o->flags & PDG_F_ONCE) { snprintf(t, sizeof t, "%s is a debug-start option of the Debug, Display or Sound group", o->key); ok(o->group == PDG_G_DEBUG || o->group == PDG_G_DISPLAY || o->group == PDG_G_SOUND, t); }
    }
    static const char *const testbed[] = { "mac.remotefile", "mac.wifisim", "mac.switchsim", "mac.hdmisim", "mac.resetsim", "mac.recoverypin",
                                           "mac.recoveryscreen", "mac.wifisimap", "mac.edidtest", "mac.cardearly", "mac.busfault", "mac.selinefault",
                                           "mac.cardslow", "mac.cardstall", "mac.cardfail", "mac.cardtest", "mac.deaduart", "mac.audiodump", "mac.videotest",
                                           "mac.semihost", "mac.declrom", "mac.disk0", "mac.net", "mac.btsim", "mac.mboxstress", "mac.netdown", "mac.inputtest",
                                           "mac.rtcsim", "mac.sepa4", "mac.sefloat", "mac.vcslow", "mac.bridge" };
    for (unsigned i = 0; i < sizeof testbed / sizeof testbed[0]; i++) { char t[100]; snprintf(t, sizeof t, "%s (testbed only) not offered", testbed[i]); ok(!pdg_find(testbed[i]), t); }

    /* ---- values ---- */
    const struct pdg_opt *o;
    o = pdg_find("MAC.FPU"); ok(o && pdg_value_ok(o, "exact", why, sizeof why) && !pdg_value_ok(o, "fast", why, sizeof why), "mac.fpu: exact yes, fast no");
    ok(!strcmp(why, "mac.fpu: fast is not one of exact|on|double|none"), "the reason names the values");
    o = pdg_find("mac.screen"); ok(pdg_value_ok(o, "1024x768", why, sizeof why) && pdg_value_ok(o, "2560X1440", why, sizeof why) && !pdg_value_ok(o, "1024x", why, sizeof why) && !pdg_value_ok(o, "x768", why, sizeof why), "mac.screen WxH");
    o = pdg_find("mac.seram"); ok(pdg_value_ok(o, "2.5", why, sizeof why) && !pdg_value_ok(o, "3", why, sizeof why), "mac.seram 2.5 yes, 3 no");
    o = pdg_find("mac.peek"); ok(pdg_value_ok(o, "910,32", why, sizeof why) && pdg_value_ok(o, "0x910", why, sizeof why) && !pdg_value_ok(o, "910,", why, sizeof why) && !pdg_value_ok(o, "g10", why, sizeof why), "mac.peek HEX[,N]");
    o = pdg_find("mac.setrace"); ok(pdg_value_ok(o, "200", why, sizeof why) && pdg_value_ok(o, "200,VIA", why, sizeof why) && !pdg_value_ok(o, "VIA", why, sizeof why), "mac.setrace N[,DEV]");
    o = pdg_find("mac.buscheck"); ok(pdg_value_ok(o, "force,loop", why, sizeof why) && !pdg_value_ok(o, "2", why, sizeof why), "mac.buscheck");
    o = pdg_find("remote.ip"); ok(pdg_value_ok(o, "10.0.0.5/24,10.0.0.1", why, sizeof why) && pdg_value_ok(o, "10.0.0.5/24", why, sizeof why) && !pdg_value_ok(o, "10.0.0/24", why, sizeof why), "remote.ip");
    o = pdg_find("remote.mac"); ok(pdg_value_ok(o, "02:00:5e:10:00:01", why, sizeof why) && !pdg_value_ok(o, "02:00:5e:10:00", why, sizeof why), "remote.mac");
    o = pdg_find("mac.exchange"); ok(pdg_value_ok(o, "Mac Exchange", why, sizeof why) && !pdg_value_ok(o, "a\"b", why, sizeof why), "mac.exchange takes a space, not a quote");
    o = pdg_find("recovery.apchannel"); ok(pdg_value_ok(o, "13", why, sizeof why) && !pdg_value_ok(o, "14", why, sizeof why) && !pdg_value_ok(o, "0", why, sizeof why), "the channel 1 to 13");
    ok(pdg_choices(pdg_find("mac.fpu")) && !pdg_choices(pdg_find("mac.ram")), "menus and fields");

    /* ---- boards ---- */
    ok(pdg_board("se") == PDG_SE && pdg_board("512KE") == PDG_512KE && pdg_board("") == PDG_IICI && pdg_board(0) == PDG_IICI && pdg_board("amiga") == PDG_IICI, "mac.board's values");
    ok(!strcmp(pdg_board_word(PDG_128K), "128k"), "board words");
    static const char *const bnames[] = { "se", "plus", "512ke", "512k", "128k", "solo", "iici" };
    printf("options per board (on the Options tab, the WiFi tab's three left out; one-shot debug options):\n");
    for (int b = 0; b < 7; b++) {
        int n = 0, g[PDG_GROUPS] = { 0 }, once = 0;
        for (unsigned i = 0; i < pdg_nopts; i++) if (pdg_uses(&pdg_opts[i], 1 << b) && !(pdg_opts[i].flags & PDG_F_WIFITAB)) { n++; g[pdg_opts[i].group]++; if (pdg_opts[i].flags & PDG_F_ONCE) once++; }
        printf("  %-6s %2d:", bnames[b], n);
        for (int k = 0; k < PDG_GROUPS; k++) printf(" %s %d", pdg_group_name[k], g[k]);
        printf("; one-shot %d\n", once);
    }
    for (int b = 1; b <= PDG_128K; b <<= 1) { char t[80]; snprintf(t, sizeof t, "the bus check and JTAG on %s", pdg_board_word(b)); ok(board_has("mac.buscheck", b) && board_has("mac.jtag", b) && board_has("mac.screencheck", b), t); }
    ok(!board_has("mac.buscheck", PDG_SOLO) && !board_has("mac.buscheck", PDG_IICI) && !board_has("mac.bustrace", PDG_SOLO), "no bus check, JTAG or bus trace on the solo board or the IIci");
    ok(board_has("mac.plustrace", PDG_PLUS) && board_has("mac.plustrace", PDG_512KE) && board_has("mac.plustrace", PDG_128K) && !board_has("mac.plustrace", PDG_SE), "the Plus family's ADB trace, not the SE's");
    ok(board_has("mac.sescsi", PDG_SE) && !board_has("mac.sescsi", PDG_PLUS) && board_has("mac.scsitrace", PDG_SE) && !board_has("mac.scsitrace", PDG_512K), "the SE's SCSI; no SCSI trace on the 512K");
    ok(board_has("mac.solotrace", PDG_SOLO) && !board_has("mac.solotrace", PDG_SE) && board_has("mac.logsave", PDG_SOLO), "the solo board's trace and log save");

    /* ---- the one-shot debug start ---- */
    ok(!pdg_once_check("mac.logsave=10 mac.synclog=1 mac.buscheck=1", L("mac.logsave=10 mac.synclog=1 mac.buscheck=1"), why, sizeof why), "a one-shot line");
    ok(!pdg_once_check("mac.audiotest=3\n", L("mac.audiotest=3\n"), why, sizeof why), "with an editor's newline");
    ok(pdg_once_check("mac.fpu=none", L("mac.fpu=none"), why, sizeof why) && strstr(why, "not a debug option"), "mac.fpu is no one-shot option");
    ok(pdg_once_check("mac.synclog", L("mac.synclog"), why, sizeof why) && strstr(why, "key=value"), "a bare word refused");
    ok(pdg_once_check("mac.logsave=soon", L("mac.logsave=soon"), why, sizeof why) && strstr(why, "mac.logsave"), "a bad value refused");
    ok(pdg_once_check("mac.synclog=1\nmac.logsave=5", L("mac.synclog=1\nmac.logsave=5"), why, sizeof why), "two lines refused");
    ok(pdg_once_check("", L(""), why, sizeof why), "nothing refused");
    ok(pdg_once_check("wifi.psk=x", L("wifi.psk=x"), why, sizeof why), "a secret is no one-shot option");
    int m = 0, s = 0;
    const char *args = "enable_cache mac.board=se  mac.logsave=0 wifi.psk=PASSWORD";
    int r = pdg_once_merge(args, (uint32_t)strlen(args), "mac.logsave=10 mac.synclog=1 mac.fpu=none mac.scsitrace=200\n", L("mac.logsave=10 mac.synclog=1 mac.fpu=none mac.scsitrace=200\n"), out, sizeof out, &m, &s, why, sizeof why);
    ok(r > 0 && !strcmp(out, "enable_cache mac.board=se  mac.logsave=10 wifi.psk=PASSWORD mac.synclog=1 mac.scsitrace=200"), "the merge: replaced in place, added at the end, spacing kept");
    ok(m == 3 && s == 1 && strstr(why, "mac.fpu"), "three merged, mac.fpu skipped and named");
    r = pdg_once_merge(args, (uint32_t)strlen(args), "mac.logsave=5 mac.logsave=20", L("mac.logsave=5 mac.logsave=20"), out, sizeof out, &m, &s, why, sizeof why);
    ok(r > 0 && strstr(out, "mac.logsave=20") && !strstr(out, "mac.logsave=5") && !strstr(out, "mac.logsave=0") && m == 2, "the last word of a key wins");
    r = pdg_once_merge(args, (uint32_t)strlen(args), "mac.synclog=1", L("mac.synclog=1"), out, 20, &m, &s, why, sizeof why);
    ok(r == -1, "too small an output refused");
    r = pdg_once_merge("", 0, "mac.buscheck=force", L("mac.buscheck=force"), out, sizeof out, &m, &s, why, sizeof why);
    ok(r > 0 && !strcmp(out, "mac.buscheck=force") && m == 1, "into empty boot arguments");

    /* ---- the scrub ---- */
    const char *secrets[] = { "PASSWORD", "YOURTOKEN", "abcdefghjk", "1234", 0 };
    const char *in = "[BOOT] bootargs=mac.board=plus wifi.ssid=Home wifi.psk=PASSWORD remote.token=YOURTOKEN mac.recoverypin=1234 "
                     "mac.wifisim=Net:hunter22 bt.dev=001122334455:00112233445566778899aabbccddeeff:2 wifi.psk=\"pass word\" x.key=\n"
                     "[WIFI] joining Home with PASSWORD (no, the log never says it) wifi.psk=*** ap abcdefghjk\nPIN 1234 at 12:34\n";
    uint32_t n = pdg_scrub(in, (uint32_t)strlen(in), out, sizeof out, secrets, 4);
    ok(n == strlen(out), "the length");
    ok(!strstr(out, "PASSWORD") && !strstr(out, "YOURTOKEN") && !strstr(out, "hunter22") && !strstr(out, "00112233445566778899") && !strstr(out, "pass word")
       && !strstr(out, "abcdefghjk") && !strstr(out, "***"), "no secret left");
    ok(strstr(out, "wifi.psk=(hidden) remote.token=(hidden) mac.recoverypin=(hidden) mac.wifisim=Net:(hidden) bt.dev=001122334455:(hidden):2 wifi.psk=(hidden) x.key=\n") != 0, "each in its place");
    ok(strstr(out, "joining Home with (hidden) (no") && strstr(out, "wifi.psk=(hidden) ap (hidden)") && strstr(out, "mac.board=plus wifi.ssid=Home"), "literal copies hidden, the rest kept");
    ok(strstr(out, "PIN 1234 at 12:34") != 0, "a short secret (the PIN) only where a key names it: other numbers stay");
    n = pdg_scrub(in, (uint32_t)strlen(in), out, 40, secrets, 4);
    ok(n == 39 && out[39] == 0, "cut at the buffer's end");

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
