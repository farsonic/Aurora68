/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_cmdline.c - cmdline.txt as words: read one option, change one, keep everything else.
 *
 * Backstory. Aurora68 Setup and Recovery (22 November 2026, docs/08 item 106) lets the owner change the Pi's options
 * from a phone or from the recovery screen's keyboard menu when the Mac will not start: the WiFi network,
 * mac.board=, mac.fpu= and the like. cmdline.txt is one line that the Pi's firmware hands to the kernel, and
 * a card whose line has been mangled may not start at all, so a change must touch exactly the word it is
 * about. The kernel's card writer had one edit of this kind already (psmac_card.c's cmdline_apply, for the
 * Wi-Fi menu), but it rebuilt the line with single spaces; this one copies every byte it does not change,
 * so a line written by hand keeps its spacing, its order and the options nobody here knows. A word is what
 * the kernel and Alice68's bootarg_val take for one: characters up to a space, a tab or the end of the line,
 * a "quoted part" counting as one (wifi.ssid="My Network"). The key of a word is what comes before its
 * first '='; a word without one is a bare flag (enable_cache, debug).
 *
 * No kernel headers: tools/cmdline_test.c builds this file on the host.
 */
#include "psmac_cmdline.h"

static int sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static uint32_t slen_(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }

/* the next word at or after *p: its start, its length in *wl; 0 at the end */
static const char *word_at(const char *s, uint32_t n, uint32_t *p, uint32_t *wl)
{
    uint32_t i = *p;
    while (i < n && sp(s[i])) i++;
    if (i >= n) { *p = n; return 0; }
    uint32_t w = i; int q = 0;
    while (i < n && (q || !sp(s[i]))) { if (s[i] == '"') q = !q; i++; }
    *p = i; *wl = i - w;
    return s + w;
}
/* the word is key=... (1) or the bare word key (2), else 0 */
static int word_is(const char *w, uint32_t wl, const char *key)
{
    uint32_t kl = slen_(key), i = 0;
    if (wl < kl) return 0;
    for (; i < kl; i++) if (w[i] != key[i]) return 0;
    if (wl == kl) return 2;
    return w[kl] == '=' ? 1 : 0;
}

int pcl_get(const char *line, uint32_t n, const char *key, char *out, uint32_t max)
{
    uint32_t p = 0, wl;
    const char *w;
    while ((w = word_at(line, n, &p, &wl))) {
        int k = word_is(w, wl, key);
        if (!k) continue;
        uint32_t o = 0;
        if (k == 1)
            for (uint32_t i = slen_(key) + 1; i < wl && o + 1 < max; i++)
                if (w[i] != '"') out[o++] = w[i];
        if (max) out[o] = 0;
        return k;
    }
    return 0;
}

static int put(char *out, uint32_t cap, uint32_t *o, const char *s, uint32_t k)
{
    if (*o + k > cap) return -1;
    for (uint32_t i = 0; i < k; i++) out[*o + i] = s[i];
    *o += k;
    return 0;
}
/* key=value as it goes into the line: quoted when the value has a space or a tab */
static int put_word(char *out, uint32_t cap, uint32_t *o, const char *key, const char *value)
{
    if (put(out, cap, o, key, slen_(key))) return -1;
    if (value == PCL_FLAG) return 0;
    uint32_t vl = slen_(value); int q = 0;
    for (uint32_t i = 0; i < vl; i++) if (value[i] == ' ' || value[i] == '\t') q = 1;
    if (put(out, cap, o, "=", 1)) return -1;
    if (q && put(out, cap, o, "\"", 1)) return -1;
    if (put(out, cap, o, value, vl)) return -1;
    if (q && put(out, cap, o, "\"", 1)) return -1;
    return 0;
}

int pcl_set(const char *in, uint32_t n, char *out, uint32_t cap, const char *key, const char *value)
{
    if (!key[0]) return -2;
    for (const char *k = key; *k; k++) if (sp(*k) || *k == '=' || *k == '"' || (unsigned char)*k < 32) return -2;
    if (value && value != PCL_FLAG)
        for (const char *v = value; *v; v++) if (*v == '"' || (unsigned char)*v < 32 || *v == 127) return -2;
    uint32_t o = 0, p = 0, wl, last = 0;
    int done = 0;
    const char *w;
    /* the line's end: what follows its last word (a newline, usually) is kept after any new word */
    uint32_t tail = n;
    while (tail > 0 && sp(in[tail - 1])) tail--;
    while ((w = word_at(in, n, &p, &wl))) {
        uint32_t ws = (uint32_t)(w - in);
        if (!word_is(w, wl, key)) continue;
        /* the bytes before this word as they are */
        if (put(out, cap, &o, in + last, ws - last)) return -1;
        if (!done && value) {
            if (put_word(out, cap, &o, key, value)) return -1;
            done = 1;
            last = ws + wl;
            continue;
        }
        /* taken out: with the spaces in front of it, or after it if it is the first word */
        while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) o--;
        last = ws + wl;
        if (o == 0) while (last < n && (in[last] == ' ' || in[last] == '\t')) last++;
    }
    if (!done && value) {
        if (put(out, cap, &o, in + last, tail > last ? tail - last : 0)) return -1;
        if (o > 0 && put(out, cap, &o, " ", 1)) return -1;
        if (put_word(out, cap, &o, key, value)) return -1;
        last = tail > last ? tail : last;
    }
    if (put(out, cap, &o, in + last, n - last)) return -1;
    return (int)o;
}

int pcl_check_line(const char *t, uint32_t n, char *why, uint32_t whymax)
{
    const char *bad = 0;
    uint32_t e = n;
    if (e && t[e - 1] == '\n') e--;
    if (e && t[e - 1] == '\r') e--;
    uint32_t words = 0, p = 0, wl;
    int q = 0;
    for (uint32_t i = 0; i < e && !bad; i++) {
        unsigned char c = (unsigned char)t[i];
        if (c == '\n' || c == '\r') bad = "more than one line: cmdline.txt must be a single line of options";
        else if (c == '"') q = !q;
        else if ((c < 32 && c != '\t') || c > 126) bad = "a character that is not plain ASCII text";
    }
    if (!bad && q) bad = "a quote that is not closed";
    while (!bad && word_at(t, e, &p, &wl)) words++;
    if (!bad && !words) bad = "the line is empty: the kernel would start with none of its options";
    if (!bad && e > 1024) bad = "longer than 1024 characters";
    if (!bad) return 0;
    uint32_t i = 0;
    for (; bad[i] && i + 1 < whymax; i++) why[i] = bad[i];
    if (whymax) why[i] = 0;
    return -1;
}

void pcl_words(const char *line, uint32_t n, void (*f)(void *ctx, const char *w, uint32_t wl), void *ctx)
{
    uint32_t p = 0, wl;
    const char *w;
    while ((w = word_at(line, n, &p, &wl))) f(ctx, w, wl);
}
