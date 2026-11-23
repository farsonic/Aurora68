/* SPDX-License-Identifier: MPL-2.0
 *
 * cmdline_test.c - host tests of psmac_cmdline.c, the word editor Aurora68 Setup and Recovery changes cmdline.txt
 * with (docs/08 item 106). Build and run: tools/cmdline_test.sh
 */
#include <stdio.h>
#include <string.h>
#include "../emu68/src/psmac/psmac_cmdline.h"

static int fails, runs;
static void expect_set(const char *in, const char *key, const char *val, const char *want)
{
    char out[512];
    int n = pcl_set(in, (uint32_t)strlen(in), out, sizeof out, key, val);
    runs++;
    if (n < 0) { if (want) { printf("FAIL set %s in [%s]: %d\n", key, in, n); fails++; } return; }
    out[n] = 0;
    if (!want || strcmp(out, want)) { printf("FAIL set %s=%s in [%s]\n  got  [%s]\n  want [%s]\n", key, val == PCL_FLAG ? "(flag)" : val ? val : "(remove)", in, out, want ? want : "(an error)"); fails++; }
}
static void expect_get(const char *in, const char *key, int want_k, const char *want)
{
    char v[128];
    int k = pcl_get(in, (uint32_t)strlen(in), key, v, sizeof v);
    runs++;
    if (k != want_k || (k == 1 && strcmp(v, want))) { printf("FAIL get %s in [%s]: %d [%s]\n", key, in, k, k ? v : ""); fails++; }
}
static void expect_check(const char *in, int ok)
{
    char why[128];
    int r = pcl_check_line(in, (uint32_t)strlen(in), why, sizeof why);
    runs++;
    if ((r == 0) != ok) { printf("FAIL check [%s]: %s\n", in, r ? why : "accepted"); fails++; }
}
int main(void)
{
    const char *base = "enable_cache debug mac.board=se  wifi.ssid=\"My Network\" wifi.psk=secret12 bt=off\n";
    /* a known option changed in place: everything else byte for byte, the double space kept */
    expect_set(base, "mac.board", "plus", "enable_cache debug mac.board=plus  wifi.ssid=\"My Network\" wifi.psk=secret12 bt=off\n");
    /* a quoted value replaced by one with a space: quoted again */
    expect_set(base, "wifi.ssid", "Office 2", "enable_cache debug mac.board=se  wifi.ssid=\"Office 2\" wifi.psk=secret12 bt=off\n");
    /* a new option at the end, before the newline */
    expect_set(base, "mac.fpu", "none", "enable_cache debug mac.board=se  wifi.ssid=\"My Network\" wifi.psk=secret12 bt=off mac.fpu=none\n");
    /* taken out, with its space */
    expect_set(base, "wifi.psk", 0, "enable_cache debug mac.board=se  wifi.ssid=\"My Network\" bt=off\n");
    expect_set(base, "enable_cache", 0, "debug mac.board=se  wifi.ssid=\"My Network\" wifi.psk=secret12 bt=off\n");
    expect_set(base, "bt", 0, "enable_cache debug mac.board=se  wifi.ssid=\"My Network\" wifi.psk=secret12\n");
    /* a key that is a prefix of another is not confused with it: bt is not bt.pair */
    expect_set("bt.pair=1 bt=off", "bt", "on", "bt.pair=1 bt=on");
    expect_set("mac.recovery=1 mac.recoveryx=2", "mac.recovery", 0, "mac.recoveryx=2");
    /* a bare flag set and removed */
    expect_set("mac.board=se", "mac.console", "hdmi", "mac.board=se mac.console=hdmi");
    expect_set("mac.board=se\r\n", "enable_cache", PCL_FLAG, "mac.board=se enable_cache\r\n");
    expect_set("enable_cache mac.board=se", "enable_cache", PCL_FLAG, "enable_cache mac.board=se");
    /* duplicates: the first keeps its place, the later ones go */
    expect_set("mac.ram=8 x=1 mac.ram=64 y=2", "mac.ram", "128", "mac.ram=128 x=1 y=2");
    expect_set("mac.ram=8 x=1 mac.ram=64 y=2", "mac.ram", 0, "x=1 y=2");
    /* an empty line, a line with no newline, tabs kept */
    expect_set("", "remote", "on", "remote=on");
    expect_set("a=1\tb=2", "c", "3", "a=1\tb=2 c=3");
    expect_set("a=1\tb=2", "a", "9", "a=9\tb=2");
    /* values that cannot be written */
    expect_set(base, "wifi.ssid", "say \"hi\"", 0);
    expect_set(base, "bad key", "1", 0);
    /* reading */
    expect_get(base, "wifi.ssid", 1, "My Network");
    expect_get(base, "mac.board", 1, "se");
    expect_get(base, "debug", 2, "");
    expect_get(base, "mac.fpu", 0, "");
    expect_get("bt.pair=1", "bt", 0, "");
    /* one line */
    expect_check(base, 1);
    expect_check("a=1\r\n", 1);
    expect_check("a=1\nb=2\n", 0);
    expect_check("   \n", 0);
    expect_check("wifi.ssid=\"open", 0);
    expect_check("caf\xc3\xa9=1", 0);
    printf("%d tests, %d failed\n", runs, fails);
    return fails != 0;
}
