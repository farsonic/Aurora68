/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_buscheck_core.c — the compact Macs' bus check: the tests and the reading of their numbers.
 *
 * Backstory (22 November 2026, docs/08 item 108). A tester's Macintosh 512Ke on a socketed PiStorm
 * showed a right screen with rare single wrong words, while its reads looked unreliable everywhere:
 * the screen read-back found wrong words in 6,011 of 6,605 rows, the mouse's Y1 line chattered, the
 * keyboard gave phantom keys. Writes landing and reads failing is what a read sampled too early
 * looks like, but a bad joint in a socket, a data line, the RAM or the chips could each give some of
 * it. This file measures what tells them apart and says which it is, the same way on every machine:
 *
 *   1  RAM data lines: walking ones and zeros, $0000 $FFFF $AAAA $5555 and random words at 256
 *      addresses, each read back three times as words and once as bytes. Per data bit, how often a 0
 *      read as 1 and a 1 as 0; whether the second read of a wrong word repeats it (systematic) or
 *      not (random); upper and lower byte; what the wrong words were (the address the Pi had just
 *      sent, the word before, $FFFF: a read taken before the board drove the lines); byte writes.
 *   2  RAM address lines: psmac_board_se.c's pairwise test (docs/08 item 68) over the whole RAM, each
 *      read taken as the majority of five, and each pair's result matched against models (a line
 *      stuck, following another, two joined), on reads, writes or both; memory that repeats above a
 *      line is a board with less RAM than assumed.
 *   3  The chips: the VIA's DDRA, DDRB, ACR and PCR written and read back; port A and B's inputs
 *      watched; SCC RR0 on both channels; WR12/WR13 written and read back as RR12/RR13.
 *   4  Timing: every access's duration by kind (min, median, max) and the transport's timeouts.
 *   5  Retries: one read, two that agree, a majority of three: what a software retry would leave.
 *   6  Video: the same reads during vertical blanking and while the video is reading the RAM.
 *   7  Spacing: reads back to back against reads with a pause between them.
 *
 * Every address and value the tests use is safe on all four boards: the RAM below the top 64 KB
 * (where the screen and sound buffers are), the VIA's port bits that are outputs on every board, the
 * SCC's baud-rate constants, which do nothing until WR14 starts the generator.
 */
#include <stdarg.h>
#include "psmac_buscheck.h"

#define VIA(reg)   (0xEFE1FEu + ((uint32_t)(reg) << 9))   /* the VIA on the upper byte, a register every 512 bytes */
#define SCC_RB     0x9FFFF8u                               /* SCC reads (upper byte): B control +0, A control +2 */
#define SCC_WB     0xBFFFF9u                               /* SCC writes (lower byte): B control +0, A control +2 */
#define TOP_KEEP   0x10000u                                /* the top 64 KB: screen buffers ($5900, $D900 below the top) and sound */

/* The VIA values written. Only bits that are outputs on every board are driven, and to levels that do
   nothing: port B 0-2 (the clock chip: data 0, clock 0, enable 1 = deselected) and 7 (sound enable, 1 =
   off); PB3-PB6 stay inputs (the Plus's mouse button, X2, Y2 and H4 are driven by the board). Port A
   0-6: volume 0, PA3 the main sound buffer (Plus), PA4 0 (the Plus's overlay off, the SE's internal
   drive), PA5 head 0, PA6 the main screen; PA7 (SCC W/REQ) stays an input. ACR: shift register mode 3
   (in under CB1's clock, which drives nothing) with T2 counting PB6 pulses; PCR $22: CA2 and CB2 as
   interrupt inputs, CA1 and CB1 on their falling edges. */
#define V_ORB  0x84u
#define V_DDRB 0x87u
#define V_ORA  0x48u
#define V_DDRA 0x7Fu
#define V_ACR  0x2Cu
#define V_PCR  0x22u
/* While the tests run, every word of the main screen holds one mark and every word of the sound buffer
   another (a grey stripe on the glass, silence behind a disabled speaker): a wrong word read that is one
   of them is the video's or the sound's fetch still on the RAM's bus (docs/18 M4). Neither is a pattern
   any test writes. */
#define MARK_SCREEN 0xC3A5u
#define MARK_SOUND  0x96E1u

/* Thresholds, and why */
#define STRAY_PPM      10u     /* up to 10 wrong words in a million (and at least two allowed): a noisy bench, not a fault */
#define STUCK_PCT      95u     /* a bit wrong in 95% of the reads that expected the other level, and right otherwise, is stuck */
#define STUCK_MIN      64u     /* ... with at least that many reads of each kind behind it */
#define RANDOM_PCT     25u     /* under 25% of wrong words repeated by the next read: random; over 75%: systematic */
#define FEW_BITS_PCT   90u     /* 90% of the bit errors on three lines or fewer: those lines, not the timing of all */
#define EARLY_PCT      25u     /* a quarter of the wrong words holding what the Pi last sent: read before the board drove them */
#define CONTEND_X      3u      /* errors three times as frequent while the video reads the RAM: contention */
#define UNDECIDED_PCT  10u     /* a tenth of the address pairs without a majority: the address result is not to be trusted */

struct st {
    const struct bc_bus *bus; const struct bc_config *cfg; struct bc_results *r;
    uint32_t rng, trng;
    uint32_t a[BC_NADDR];       /* the data test's addresses */
    uint32_t lo, hi;            /* the RAM the data tests use */
};

static uint32_t rnd(uint32_t *x) { uint32_t v = *x; v ^= v << 13; v ^= v >> 17; v ^= v << 5; return *x = v ? v : 0x2545f491u; }

/* ---- timed accesses -------------------------------------------------------------------------------- */
static int kind_of(uint32_t a, int size, int w)
{
    if (a < 0x400000u) return size == 1 ? (w ? BC_RAM_WB : BC_RAM_RB) : (w ? BC_RAM_WW : BC_RAM_RW);
    if ((a & 0xf00000u) == 0xe00000u) return w ? BC_VIA_W : BC_VIA_R;
    return w ? BC_SCC_W : BC_SCC_R;
}
static void t_add(struct st *s, int k, uint64_t d64)
{
    struct bc_time *t = &s->r->t[k];
    uint32_t d = d64 > 0xffffffffu ? 0xffffffffu : (uint32_t)d64;
    if (!t->n || d < t->min) t->min = d;
    if (d > t->max) t->max = d;
    t->sum += d;
    if (t->ns < BC_SAMPLES) t->s[t->ns++] = d;
    else { uint32_t j = rnd(&s->trng) % (t->n + 1); if (j < BC_SAMPLES) t->s[j] = d; }   /* a reservoir: an even sample of the whole */
    t->n++;
}
static uint32_t rd(struct st *s, uint32_t a, int size)
{
    uint64_t t0 = s->bus->ticks(s->bus->ctx);
    uint32_t v = s->bus->rd(s->bus->ctx, a, size);
    t_add(s, kind_of(a, size, 0), s->bus->ticks(s->bus->ctx) - t0);
    s->r->cycles++;
    return v & (size == 1 ? 0xffu : 0xffffu);
}
static void wr(struct st *s, uint32_t a, int size, uint32_t v)
{
    uint64_t t0 = s->bus->ticks(s->bus->ctx);
    s->bus->wr(s->bus->ctx, a, size, v & (size == 1 ? 0xffu : 0xffffu));
    t_add(s, kind_of(a, size, 1), s->bus->ticks(s->bus->ctx) - t0);
    s->r->cycles++;
}
static uint32_t median(const struct bc_time *t)
{
    uint32_t v[BC_SAMPLES], n = t->ns;
    if (!n) return 0;
    for (uint32_t i = 0; i < n; i++) v[i] = t->s[i];
    for (uint32_t i = 1; i < n; i++) { uint32_t x = v[i], j = i; while (j && v[j - 1] > x) { v[j] = v[j - 1]; j--; } v[j] = x; }
    return v[n / 2];
}
static void idle(struct st *s) { if (s->bus->idle) s->bus->idle(s->bus->ctx); }
static void stage(struct st *s, int step, const char *what) { if (s->bus->stage) s->bus->stage(s->bus->ctx, step, 7, what); }
static uint64_t now(struct st *s) { return s->bus->ticks(s->bus->ctx); }
static uint64_t us_ticks(struct st *s, uint32_t us) { return s->bus->hz * us / 1000000u; }
static uint32_t ticks_us(const struct bc_results *r, uint64_t t) { return r->hz ? (uint32_t)(t * 1000000u / r->hz) : 0; }
static uint32_t ticks_ns(const struct bc_results *r, uint64_t t) { return r->hz ? (uint32_t)(t * 1000000000u / r->hz) : 0; }

/* the board quiet and the main screen shown: sound off, the clock chip deselected, the overlay off */
static void via_quiet(struct st *s)
{
    wr(s, VIA(0), 1, V_ORB); wr(s, VIA(2), 1, V_DDRB);
    wr(s, VIA(15), 1, V_ORA); wr(s, VIA(3), 1, V_DDRA);
}

/* ---- 1: the data lines ------------------------------------------------------------------------------ */
static uint16_t pattern(struct st *s, int p)
{
    if (p < 16) return (uint16_t)(1u << p);
    if (p < 32) return (uint16_t)~(1u << (p - 16));
    switch (p) { case 32: return 0x0000; case 33: return 0xffff; case 34: return 0xaaaa; case 35: return 0x5555; }
    return (uint16_t)rnd(&s->rng);
}
static void account(struct st *s, uint32_t a, uint16_t want, uint16_t got, uint16_t prev)
{
    struct bc_data *d = &s->r->data;
    d->words++;
    for (int b = 0; b < 16; b++) {
        int eb = want >> b & 1, gb = got >> b & 1;
        if (eb) { d->n1[b]++; if (!gb) d->e10[b]++; } else { d->n0[b]++; if (gb) d->e01[b]++; }
        for (int c = 0; c < 16; c++) {
            if (c == b || eb == (want >> c & 1)) continue;
            if (!eb) { d->n01[b][c]++; if (gb) d->x01[b][c]++; }
            else     { d->n10[b][c]++; if (!gb) d->x10[b][c]++; }
        }
    }
    if (got == want) return;
    d->wrong++;
    if ((got ^ want) & 0xff00) d->up_wrong++;
    if ((got ^ want) & 0x00ff) d->lo_wrong++;
    if (got == (a & 0xffff)) d->eq_addr++;
    else if (got == (0x0200u | (a >> 16 & 0xff))) d->eq_hireg++;   /* the address-high register of a word read: psmac_gpio.c's last write */
    else if (got == prev) d->eq_prev++;
    if (got == 0xffff) { d->eq_ffff++; d->ffff_reads++; }
    else if (got == 0x0000) d->eq_0000++;
    else if (got == MARK_SCREEN) d->eq_screen++;
    else if (got == MARK_SOUND) d->eq_sound++;
}
static uint16_t vote3(uint16_t a, uint16_t b, uint16_t c, int *ok)
{
    *ok = 1;
    if (a == b || a == c) return a;
    if (b == c) return b;
    *ok = 0; return a;
}
/* what the memory at a holds, as far as reads can tell: five reads, the words the Pi had just put on
   the lines (an early read) left out, one word three times or more */
static uint16_t vote5(struct st *s, uint32_t a, int *ok)
{
    uint16_t v[5];
    for (int i = 0; i < 5; i++) v[i] = (uint16_t)rd(s, a, 2);
    for (int i = 0; i < 5; i++) {
        if (v[i] == (a & 0xffffu) || v[i] == (0x0200u | (a >> 16 & 0xffu))) continue;
        int n = 0; for (int j = 0; j < 5; j++) n += v[j] == v[i];
        if (n >= 3) { *ok = 1; return v[i]; }
    }
    *ok = 0; return v[0];
}
static void read_back(struct st *s, uint32_t a, uint16_t v, uint16_t *prev)
{
    struct bc_data *d = &s->r->data;
    uint16_t r1 = (uint16_t)rd(s, a, 2), r2 = (uint16_t)rd(s, a, 2), r3 = (uint16_t)rd(s, a, 2);
    account(s, a, v, r1, *prev); account(s, a, v, r2, r1); account(s, a, v, r3, r2);
    *prev = r3;
    if (r1 != v) { d->first_wrong++; if (r2 == r1) d->repeat++; else if (r2 == v) d->transient++; else d->other++; }
    uint8_t bh = (uint8_t)rd(s, a, 1), bl = (uint8_t)rd(s, a + 1, 1);
    d->bhi++; if (bh != (v >> 8)) d->bhi_wrong++;
    d->blo++; if (bl != (v & 0xff)) d->blo_wrong++;
}
static void data_test(struct st *s)
{
    struct bc_data *d = &s->r->data;
    d->addrs = BC_NADDR; d->patterns = BC_NPAT;
    uint16_t prev = 0;
    for (int p = 0; p < BC_NPAT; p++) {
        if (p < 36) {
            /* the same word everywhere: written to all 256 addresses, then read back. An address fault
               cannot change what is read (test 2's business); the RAM holds it a few milliseconds. */
            uint16_t v = pattern(s, p);
            for (int i = 0; i < BC_NADDR; i++) wr(s, s->a[i], 2, v);
            for (int i = 0; i < BC_NADDR; i++) read_back(s, s->a[i], v, &prev);
        } else {
            /* a word of its own at each address, read back at once (again blind to address faults) */
            for (int i = 0; i < BC_NADDR; i++) { uint16_t v = pattern(s, p); wr(s, s->a[i], 2, v); read_back(s, s->a[i], v, &prev); }
        }
        idle(s);
    }
    /* byte writes: the upper byte, then the lower, each seen through a majority of three word reads,
       so that the writes are measured and not the reads */
    for (int i = 0; i < 64; i++) {
        uint32_t a = s->a[i]; int ok; uint16_t m;
        wr(s, a, 2, 0x0000); wr(s, a, 1, 0xa5);
        m = vote5(s, a, &ok);
        if (ok) { d->bw++; if (m != 0xa500) d->bw_wrong++; }
        wr(s, a + 1, 1, 0x5a);
        m = vote5(s, a, &ok);
        if (ok) { d->bw++; if (m != 0xa55a) d->bw_wrong++; }
    }
    /* word writes, each judged by the commonest of fifteen reads (docs/18 M5): what the RAM holds, as
       far as reads that fail at random can tell */
    for (int i = 64; i < 128; i++) {
        uint32_t a = s->a[i]; uint16_t v = (uint16_t)rnd(&s->rng), r15[15];
        wr(s, a, 2, v);
        for (int k = 0; k < 15; k++) r15[k] = (uint16_t)rd(s, a, 2);
        int best = 0, bn = 0;
        for (int k = 0; k < 15; k++) { int n = 0; for (int j = 0; j < 15; j++) n += r15[j] == r15[k]; if (n > bn) { bn = n; best = k; } }
        d->ww++; if (r15[best] != v) d->ww_wrong++;
    }
    /* one word read 1,000 times: how many values, and is the right one the commonest (M5) */
    {
        uint32_t a = s->a[128]; uint16_t v = 0x5ac3, seen[16]; uint32_t cnt[16], ns = 0;
        wr(s, a, 2, v);
        for (int k = 0; k < 1000; k++) {
            uint16_t x = (uint16_t)rd(s, a, 2); uint32_t j;
            d->one_reads++; if (x == v) d->one_right++;
            for (j = 0; j < ns && seen[j] != x; j++) ;
            if (j == ns) { if (ns < 16) { seen[ns] = x; cnt[ns++] = 1; } d->one_distinct++; } else cnt[j]++;
        }
        uint32_t bj = 0; for (uint32_t j = 1; j < ns; j++) if (cnt[j] > cnt[bj]) bj = j;
        d->one_commonest_right = ns && seen[bj] == v;
    }
    for (int i = 0; i < BC_NADDR; i++) wr(s, s->a[i], 2, 0);
}

/* ---- 2: the address lines --------------------------------------------------------------------------- */
/* combination c of a pair (bit 0: line n, bit 1: line m) under model k: where an access to c really goes */
enum { M_ID, M_N0, M_N1, M_M0, M_M1, M_NFM, M_MFN, M_AND, M_OR, M_MODELS };
static int model(int k, int c)
{
    switch (k) {
    case M_N0: return c & 2;
    case M_N1: return c | 1;
    case M_M0: return c & 1;
    case M_M1: return c | 2;
    case M_NFM: return (c & 2) | (c >> 1 & 1);            /* n follows m */
    case M_MFN: return (c & 1) | (c & 1) << 1;            /* m follows n */
    case M_AND: return c == 3 ? 3 : 0;                    /* joined, a 0 on either wins */
    case M_OR:  return c ? 3 : 0;                         /* joined, a 1 wins */
    default:   return c;
    }
}
/* what the four reads would give under model k with the fault on reads (w 0), both (1) or writes (2):
   the combination whose word is found there, -2 for memory none of them reached */
static void predict(int k, int w, int out[4])
{
    int mem[4] = { -2, -2, -2, -2 };
    for (int c = 0; c < 4; c++) mem[w >= 1 ? model(k, c) : c] = c;
    for (int c = 0; c < 4; c++) out[c] = mem[w <= 1 ? model(k, c) : c];
}
/* The pair test's words: the combination's number (1-4) in every nibble with the nibble's top bit set,
   $9999 $AAAA $BBBB $CCCC. Any two differ in at least four bits and each in eight from the cleared
   $0000, so a word with one bit wrong (a stuck data line) is still known for what it is. */
static uint16_t tag(int c) { return (uint16_t)(0x8888u | 0x1111u * (uint32_t)(c + 1)); }
static int hamming1(uint16_t a, uint16_t b) { uint16_t x = a ^ b; return !(x & (x - 1)); }   /* at most one bit apart */
/* Which of the four words, or the cleared $0000 (-2), is at a: seven reads, each a vote only if it is
   one of those five words (give or take a bit). A read that is none of them (a random error, a read
   taken before the board drove the lines) is no vote, so neither noise nor a systematic early read can
   elect a wrong word. Two votes or more, and more than any other: that one. No votes at all but one
   other word four times or more: memory nobody wrote here (-2). Otherwise undecided (-1). */
static int which(struct st *s, uint32_t a)
{
    uint16_t v[7]; int votes[5] = { 0, 0, 0, 0, 0 }, garbage_max = 0, valid = 0;
    for (int i = 0; i < 7; i++) v[i] = (uint16_t)rd(s, a, 2);
    for (int i = 0; i < 7; i++) {
        int cls = -1;
        if (v[i] == (a & 0xffffu) || v[i] == (0x0200u | (a >> 16 & 0xffu))) continue;   /* what the Pi had just put on the lines: an early read, no vote */
        for (int d = 0; d < 4; d++) if (hamming1(v[i], tag(d))) cls = d;
        if (cls < 0 && hamming1(v[i], 0)) cls = 4;
        if (cls >= 0) { votes[cls]++; valid++; }
        else { int n = 0; for (int j = 0; j < 7; j++) n += v[j] == v[i]; if (n > garbage_max) garbage_max = n; }
    }
    int best = 0;
    for (int k = 1; k < 5; k++) if (votes[k] > votes[best]) best = k;
    int tie = 0;
    for (int k = 0; k < 5; k++) if (k != best && votes[k] == votes[best]) tie = 1;
    if (!tie && votes[best] >= 2) return best == 4 ? -2 : best;
    if (valid <= 1 && garbage_max >= 4) return -2;
    return -1;
}
static int base_for(struct st *s, int n, int m, uint32_t *base)
{
    /* first a base whose four addresses all stay below the top 64 KB (the screen and sound buffers, which
       the video and sound read, and whose words would be the video's fetch); failing that, any in the RAM */
    uint32_t R = s->cfg->ram_size, cand[4] = { (R - 0x12000u) & ~1u, 0x1000u, (R / 2 + 0x1000u) & ~1u, (R / 4 + 0x1000u) & ~1u };
    for (int pass = 0; pass < 2; pass++) {
        uint32_t L = pass == 0 && R >= 0x20000u ? R - TOP_KEEP : R;
        for (int i = 0; i < 4; i++) {
            uint32_t b = cand[i];
            if (b >= L) continue;
            if ((b ^ 1u << n) < L && (b ^ 1u << m) < L && (b ^ 1u << n ^ 1u << m) < L) { *base = b; return 1; }
        }
    }
    return 0;
}
static void add_find(struct bc_addr *A, int kind, int n, int m, int v, int mode)
{
    for (int i = 0; i < A->nfind; i++)
        if (A->find[i].kind == kind && A->find[i].n == n && A->find[i].m == m) return;
    if (A->nfind >= BC_MAXFIND) return;
    struct bc_afind *f = &A->find[A->nfind++];
    f->kind = (uint8_t)kind; f->n = (uint8_t)n; f->m = (uint8_t)m; f->v = (uint8_t)v; f->mode = (uint8_t)mode;
    A->lines_bad |= 1u << n; if (m) A->lines_bad |= 1u << m;
}
static void addr_test(struct st *s)
{
    struct bc_addr *A = &s->r->addr;
    uint32_t R = s->cfg->ram_size;
    int top = 0;
    for (int n = 1; n <= 23 && (1u << n) < R; n++) top = n;
    A->top = top;
    /* votes per line: [n][v][w] a stuck line; explained[n]: named by a join or a follow */
    uint16_t stuck[24][2][3]; uint8_t explained[24], confused[24], tested[24]; uint32_t and1[24], or0[24]; uint16_t excl[24][2];
    for (int n = 0; n < 24; n++) { explained[n] = confused[n] = tested[n] = 0; and1[n] = 0xffffffffu; or0[n] = 0; excl[n][0] = excl[n][1] = 0; for (int v = 0; v < 2; v++) for (int w = 0; w < 3; w++) stuck[n][v][w] = 0; }
    for (int n = 1; n <= top; n++) {
        for (int m = n + 1; m <= top; m++) {
            uint32_t base;
            if (!base_for(s, n, m, &base)) { A->skipped++; continue; }
            A->pairs++; tested[n]++; tested[m]++;
            uint32_t a[4]; int obs[4], und = 0;
            for (int c = 0; c < 4; c++) a[c] = base ^ (c & 1 ? 1u << n : 0) ^ (c & 2 ? 1u << m : 0);
            for (int c = 0; c < 4; c++) wr(s, a[c], 2, 0);
            for (int c = 0; c < 4; c++) wr(s, a[c], 2, tag(c));
            for (int c = 0; c < 4; c++) { obs[c] = which(s, a[c]); if (obs[c] == -1) und = 1; }
            for (int c = 0; c < 4; c++) wr(s, a[c], 2, 0);
            if (und) { A->undecided++; continue; }
            if (obs[0] == 0 && obs[1] == 1 && obs[2] == 2 && obs[3] == 3) continue;
            A->bad++;
            int match[M_MODELS][3], any = 0;
            for (int k = 1; k < M_MODELS; k++)
                for (int w = 0; w < 3; w++) {
                    int p[4]; predict(k, w, p);
                    match[k][w] = p[0] == obs[0] && p[1] == obs[1] && p[2] == obs[2] && p[3] == obs[3];
                    any |= match[k][w];
                }
            if (!any) { confused[n]++; confused[m]++; continue; }
            for (int w = 0; w < 3; w++) {
                if (match[M_AND][w] || match[M_OR][w]) { add_find(A, BC_A_JOINED, n, m, match[M_OR][w], w); explained[n] = explained[m] = 1; break; }
            }
            for (int w = 0; w < 3; w += 2) {                 /* a follow is told from a stuck line only when one direction is right */
                if (match[M_NFM][w] && !match[M_N0][w] && !match[M_N1][w]) { add_find(A, BC_A_FOLLOWS, n, m, 0, w); explained[n] = explained[m] = 1; }
                if (match[M_MFN][w] && !match[M_M0][w] && !match[M_M1][w]) { add_find(A, BC_A_FOLLOWS, m, n, 0, w); explained[m] = explained[n] = 1; }
            }
            for (int w = 0; w < 3; w++) {
                if (match[M_N0][w]) stuck[n][0][w]++;
                if (match[M_N1][w]) stuck[n][1][w]++;
                if (match[M_M0][w]) stuck[m][0][w]++;
                if (match[M_M1][w]) stuck[m][1][w]++;
            }
            /* the bases behind each "stuck at v, reads and writes" vote: a line that follows another reads as
               that line's value in each base, so it seems stuck at 0 in some pairs and at 1 in others */
            if (!match[M_NFM][1] && match[M_N0][1] != match[M_N1][1]) { if (match[M_N0][1]) { or0[n] |= base; excl[n][0]++; } else { and1[n] &= base; excl[n][1]++; } }
            if (!match[M_MFN][1] && match[M_M0][1] != match[M_M1][1]) { if (match[M_M0][1]) { or0[m] |= base; excl[m][0]++; } else { and1[m] &= base; excl[m][1]++; } }
        }
        idle(s);
    }
    /* the lines no join or follow explains: stuck, as most pairs say */
    int first_stuck = 0, n_stuck0b = 0, others = 0;
    for (int n = 1; n <= top; n++) {
        if (explained[n]) continue;
        int bv = -1, bw = 0; uint32_t best = 0;
        for (int v = 0; v < 2; v++) for (int w = 0; w < 3; w++) if (stuck[n][v][w] > best) { best = stuck[n][v][w]; bv = v; bw = w; }
        if (bv >= 0 && excl[n][0] && excl[n][1]) {
            /* "stuck" at both levels: not stuck, following a line that was 1 in every base of the 1 votes
               and 0 in every base of the 0 votes; a neighbour first (a bridge joins neighbouring pins) */
            uint32_t cand = and1[n] & ~or0[n] & ~(1u << n) & (((1u << (top + 1)) - 1u) & ~1u);
            int fm = 0;
            if (n < top && (cand >> (n + 1) & 1)) fm = n + 1;
            else if (n > 1 && (cand >> (n - 1) & 1)) fm = n - 1;
            else if (cand && !(cand & (cand - 1))) fm = __builtin_ctz(cand);
            add_find(A, BC_A_FOLLOWS, n, fm, 0, 1);
            others++;
        } else if (bv >= 0) {
            add_find(A, BC_A_STUCK, n, 0, bv, bw);
            if (bv == 0 && bw == 1) { if (!first_stuck) first_stuck = n; n_stuck0b++; } else others++;
        } else if (confused[n] >= 2 && confused[n] * 2 >= tested[n]) { add_find(A, BC_A_CONFUSED, n, 0, 0, 0); others++; }   /* a line wrong in half its pairs or more; a pair or two that no model fits is a third line's doing or a stray read */
    }
    for (int i = 0; i < A->nfind; i++) if (A->find[i].kind == BC_A_JOINED || A->find[i].kind == BC_A_FOLLOWS) others++;
    /* every line from one up to the top reading and writing as 0 and nothing else wrong: the memory
       repeats, the board has 2^k bytes */
    if (first_stuck && !others && n_stuck0b == top - first_stuck + 1) {
        A->nfind = 0; A->lines_bad = 0;
        add_find(A, BC_A_SIZE, first_stuck, 0, 0, 1);
        A->lines_bad = 0;
    }
}

/* ---- 3: the chips ----------------------------------------------------------------------------------- */
static void reg_see(struct bc_reg *g, uint8_t v, uint8_t *last, int first)
{
    g->reads++;
    if (v != g->want) g->wrong++;
    if (!first && v != *last) g->changes++;
    *last = v;
    for (int i = 0; i < g->nval; i++) if (g->val[i] == v) { g->cnt[i]++; return; }
    if (g->nval < 6) { g->val[g->nval] = v; g->cnt[g->nval++] = 1; }
}
/* The SCC on the Plus-era boards needs its lines to settle between accesses: "On the Macintosh Plus and
   earlier Macintosh models, it is necessary to let the SCC lines stabilize for 2.2 us between accesses"
   (Guide to the Macintosh Family Hardware, pp. 363-364; docs/18 section 2); the SE's BBU does it in
   hardware. The check keeps 3 us between its SCC accesses everywhere (as the kernel's board layer does
   from 22 November 2026), except where it measures closer spacing on purpose (M8, M9). */
#define SCC_GAP_NS 3000u
static uint64_t scc_last;
static void scc_wait(struct st *s, uint32_t ns)
{
    uint64_t t = scc_last + s->bus->hz * ns / 1000000000u;
    while (now(s) < t) ;
}
static void unspaced(struct st *s, int on) { if (s->bus->scc_unspaced) s->bus->scc_unspaced(s->bus->ctx, on); }
static uint8_t scc_rd(struct st *s, uint32_t a, uint32_t gap_ns) { scc_wait(s, gap_ns); uint8_t v = (uint8_t)rd(s, a, 1); scc_last = now(s); return v; }
static void scc_wr(struct st *s, uint32_t a, uint8_t v, uint32_t gap_ns) { scc_wait(s, gap_ns); wr(s, a, 1, v); scc_last = now(s); }

static void chips_test(struct st *s)
{
    struct bc_chips *C = &s->r->chips;
    uint32_t N = s->cfg->chip_reads ? s->cfg->chip_reads : 10000;
    via_quiet(s);
    /* the registers with no side effects, written to known values (docs/18 M7). T1's latches (6, 7) do not
       start the timer when written; IER written $7F (every enable off) reads $80 */
    static const uint8_t regno[7] = { 3, 2, 6, 7, 11, 12, 14 };
    static const uint8_t want[7] = { V_DDRA, V_DDRB, 0x5a, 0xa5, V_ACR, V_PCR, 0x80 };
    wr(s, VIA(6), 1, 0x5a); wr(s, VIA(7), 1, 0xa5);
    wr(s, VIA(11), 1, V_ACR); wr(s, VIA(12), 1, V_PCR); wr(s, VIA(14), 1, 0x7f);
    uint8_t last[7] = { 0, 0, 0, 0, 0, 0, 0 };
    for (int k = 0; k < 7; k++) C->via[k].want = want[k];
    for (uint32_t i = 0; i < N; i++)
        for (int k = 0; k < 7; k++) {
            uint8_t v = (uint8_t)rd(s, VIA(regno[k]), 1);
            reg_see(&C->via[k], v, &last[k], i == 0);
            for (int b = 0; b < 8; b++) if ((v ^ want[k]) >> b & 1) C->via_bit_wrong[b]++;
        }
    for (int i = 0; i < 2000; i++) { uint8_t a = (uint8_t)rd(s, VIA(13), 1), b = (uint8_t)rd(s, VIA(13), 1); C->ifr_pairs++; if (a != b) C->ifr_differ++; }
    /* the ports: output bits must read what was written, inputs may move (counted per bit) */
    uint8_t pb = 0, pa = 0;
    for (int i = 0; i < 1024; i++) {
        uint8_t b = (uint8_t)rd(s, VIA(0), 1), a = (uint8_t)rd(s, VIA(15), 1);
        C->pb_reads++; C->pa_reads++;
        if ((b ^ V_ORB) & V_DDRB) C->pb_out_wrong++;
        if ((a ^ V_ORA) & V_DDRA) C->pa_out_wrong++;
        if (i) for (int k = 0; k < 8; k++) { if ((b ^ pb) >> k & 1) C->pb_changes[k]++; if ((a ^ pa) >> k & 1) C->pa_changes[k]++; }
        pb = b; pa = a;
    }
    idle(s);
    /* the phase read (docs/18 section 2, the Guide p. 122): on the Plus-era boards a read in $F00000-
       $F7FFFF returns on D0 whether the board's timing signals came up in phase. The IIci ROM never looks
       and nothing corrects it; it is reported so that power-ons can be compared. A read only: the word
       access to the SCC that would shift the phase is never made. */
    if (s->cfg->phase)
        for (int i = 0; i < 16; i++) { C->phase_reads++; if (rd(s, 0xF00000u, 2) & 1) C->phase_d0++; }
    /* the SCC: one control read first, which uses up a register pointer the chip may hold (there is one
       for both channels); from then on every control read is RR0 */
    scc_last = 0;
    (void)scc_rd(s, SCC_RB, SCC_GAP_NS);
    uint8_t l0[2] = { 0, 0 };
    for (int ch = 0; ch < 2; ch++) C->rr0[ch].want = 0;
    for (int i = 0; i < 512; i++)
        for (int ch = 0; ch < 2; ch++) {
            uint8_t v = scc_rd(s, SCC_RB + 2u * (uint32_t)ch, SCC_GAP_NS);
            if (i) { uint8_t x = v ^ l0[ch]; if (x & 0x47) C->rr0_stable_changes[ch]++; if (x & 0xb8) C->rr0_line_changes[ch]++; }
            if (!i) C->rr0[ch].want = v;                  /* RR0 has no written value: the first reading is the reference */
            reg_see(&C->rr0[ch], v, &l0[ch], i == 0);
        }
    /* WR12/WR13 of channel B, the baud-rate constant (nothing runs until WR14 starts the generator),
       read back as RR12 and RR13: a write on the lower byte and a read on the upper through one chip */
    scc_wr(s, SCC_WB, 12, SCC_GAP_NS); scc_wr(s, SCC_WB, 0x5a, SCC_GAP_NS);
    scc_wr(s, SCC_WB, 13, SCC_GAP_NS); scc_wr(s, SCC_WB, 0xa5, SCC_GAP_NS);
    C->rr12.want = 0x5a; C->rr13.want = 0xa5;
    uint8_t l12 = 0, l13 = 0;
    for (int i = 0; i < 64; i++) {
        scc_wr(s, SCC_WB, 12, SCC_GAP_NS); reg_see(&C->rr12, scc_rd(s, SCC_RB, SCC_GAP_NS), &l12, i == 0);
        scc_wr(s, SCC_WB, 13, SCC_GAP_NS); reg_see(&C->rr13, scc_rd(s, SCC_RB, SCC_GAP_NS), &l13, i == 0);
    }
    scc_wr(s, SCC_WB, 12, SCC_GAP_NS); scc_wr(s, SCC_WB, 0, SCC_GAP_NS);
    scc_wr(s, SCC_WB, 13, SCC_GAP_NS); scc_wr(s, SCC_WB, 0, SCC_GAP_NS);
    /* WR2, the interrupt vector (one register for both channels, unused while nothing interrupts), read
       back as channel A's RR2, which returns it as written: with the 3 us and back to back (M9) */
    scc_wr(s, SCC_WB + 2, 2, SCC_GAP_NS); scc_wr(s, SCC_WB + 2, 0xa5, SCC_GAP_NS);
    for (int k = 0; k < 2; k++) {
        uint32_t gap = k ? 0 : SCC_GAP_NS; uint8_t l2 = 0;
        C->rr2[k].want = 0xa5;
        if (k) unspaced(s, 1);
        for (int i = 0; i < 256; i++) { scc_wr(s, SCC_WB + 2, 2, gap); reg_see(&C->rr2[k], scc_rd(s, SCC_RB + 2, gap), &l2, i == 0); }
        if (k) { unspaced(s, 0); scc_wait(s, SCC_GAP_NS); }
    }
    scc_wr(s, SCC_WB + 2, 2, SCC_GAP_NS); scc_wr(s, SCC_WB + 2, 0, SCC_GAP_NS);
    idle(s);
    /* M8: RR0 of A then B, and B then A, at six spacings: the changes of each read's stable bits and DCD
       from one pair to the next. Noise on the second read that goes away above about 2 us is the SCC's
       recovery time (H2 in docs/18). The mouse should be still. */
    static const uint32_t gaps[BC_SGAPS] = { 0, 500, 1000, 2000, 3000, 5000 };
    C->sp_pairs = N;
    unspaced(s, 1);
    for (int g = 0; g < BC_SGAPS; g++) {
        C->sp_ns[g] = gaps[g];
        for (int o = 0; o < 2; o++) {
            uint32_t first = SCC_RB + (o ? 0u : 2u), second = SCC_RB + (o ? 2u : 0u);
            uint8_t p1 = 0, p2 = 0;
            scc_wait(s, SCC_GAP_NS);
            for (uint32_t i = 0; i < N; i++) {
                uint8_t v1 = scc_rd(s, first, SCC_GAP_NS), v2 = scc_rd(s, second, gaps[g]);
                if (i) { if ((v1 ^ p1) & 0x4f) C->sp_first[o][g]++; if ((v2 ^ p2) & 0x4f) C->sp_second[o][g]++; }
                p1 = v1; p2 = v2;
            }
        }
        idle(s);
    }
    scc_wait(s, SCC_GAP_NS);
    unspaced(s, 0);
}

/* ---- the board's ROM (docs/18 M6) -------------------------------------------------------------------- */
/* The ROM answers at once (no video interleave, a /DTACK straight from the address), so clean ROM reads
   beside bad RAM reads put the fault in the RAM's timing, and bad ROM reads too in where the PiStorm
   samples. Each of 512 words is read four times; and the whole ROM is summed for the checksum Apple's
   ROMs carry in their first long (the sum of the words after it), over the size this board's ROM has. */
static void rom_test(struct st *s)
{
    struct bc_rom *M = &s->r->rom;
    uint32_t size = s->cfg->rom_kb << 10;
    if (!size) { M->verdict = BC_SKIP; return; }
    M->verdict = BC_INFO;                                 /* decided in analyse() */
    for (uint32_t i = 0; i < 512; i++) {
        uint32_t a = 0x400000u + ((i * (size / 512)) & ~1u);
        uint16_t v[4];
        for (int k = 0; k < 4; k++) v[k] = (uint16_t)rd(s, a, 2);
        int ok; uint16_t m = vote3(v[0], v[1], v[2], &ok);
        if (!ok) m = v[3];
        M->addrs++;
        for (int k = 0; k < 4; k++) { M->reads++; if (v[k] != m) M->disagree++; }
    }
    idle(s);
    M->stored = (uint32_t)rd(s, 0x400000u, 2) << 16 | rd(s, 0x400002u, 2);
    uint32_t sum = 0;
    for (uint32_t a = 4; a < size; a += 2) sum += rd(s, 0x400000u + a, 2);
    M->sum = sum; M->sum_kb = s->cfg->rom_kb; M->sum_ok = sum == M->stored;
    idle(s);
}

/* ---- 5: retries ------------------------------------------------------------------------------------- */
static void retry_test(struct st *s)
{
    struct bc_retry *T = &s->r->retry;
    for (int p = 0; p < 8; p++) {
        for (int i = 0; i < BC_NADDR; i++) {
            uint32_t a = s->a[i]; uint16_t v = (uint16_t)rnd(&s->rng);
            wr(s, a, 2, v);
            T->values++;
            if ((uint16_t)rd(s, a, 2) != v) T->one_wrong++;
            uint16_t x = 0, y = 1; int k;
            for (k = 0; k < 4; k++) { x = (uint16_t)rd(s, a, 2); y = (uint16_t)rd(s, a, 2); T->two_reads += 2; if (x == y) break; }
            if (k == 4) T->two_unresolved++;
            if (y != v) T->two_wrong++;
            int ok; uint16_t m = vote3((uint16_t)rd(s, a, 2), (uint16_t)rd(s, a, 2), (uint16_t)rd(s, a, 2), &ok);
            if (!ok) T->three_nomajority++;
            if (m != v) T->three_wrong++;
        }
        idle(s);
    }
    for (int i = 0; i < BC_NADDR; i++) wr(s, s->a[i], 2, 0);
}

/* ---- 6: vertical blanking against the video's reads ------------------------------------------------ */
/* CA1 is the vertical blanking signal on every compact Mac; the flag (IFR bit 1) is set on the edge
   PCR bit 0 chooses. 2 s without one is no VBL (a board without its video running, or the testbed's) */
static int wait_ca1(struct st *s, int rising, uint64_t *t)
{
    wr(s, VIA(12), 1, (V_PCR & ~1u) | (uint32_t)rising);
    wr(s, VIA(13), 1, 0x02);
    uint64_t t0 = now(s), lim = us_ticks(s, 2000000);
    for (;;) {
        if (rd(s, VIA(13), 1) & 0x02) { *t = now(s); return 1; }
        if (now(s) - t0 > lim) return 0;
    }
}
static void video_test(struct st *s)
{
    struct bc_video *V = &s->r->video;
    uint64_t ta, tb, tc;
    if (!wait_ca1(s, 1, &ta) || !wait_ca1(s, 0, &tb) || !wait_ca1(s, 1, &tc)) { wr(s, VIA(12), 1, V_PCR); return; }
    uint64_t d1 = tb - ta, d2 = tc - tb, blank = d1 < d2 ? d1 : d2, frame = d1 + d2;
    V->ok = 1; V->blank_edge = d1 < d2;
    V->frame_us = ticks_us(s->r, frame); V->blank_us = ticks_us(s->r, blank);
    /* a compact Mac's frame is 16.6 ms and its blank 1.3: edges far from that are the VIA's reads going wrong (CA1's flag read
       as set when it is not), and blank against display would mean nothing */
    if (V->frame_us < 5000u || V->frame_us > 100000u || V->blank_us * 4u > V->frame_us) { V->ok = 2; wr(s, VIA(12), 1, V_PCR); return; }
    uint32_t rt = median(&s->r->t[BC_RAM_RW]); if (!rt) rt = 1;
    uint64_t k = blank * 6 / 10 / rt;                     /* reads that fit in 60% of the blank */
    V->burst = k < 8 ? 8 : k > 256 ? 256 : (uint32_t)k;
    uint16_t v[64];
    for (int i = 0; i < 64; i++) { v[i] = (uint16_t)rnd(&s->rng); wr(s, s->a[i], 2, v[i]); }
    for (int f = 0; f < 16; f++) {
        uint64_t t0;
        if (!wait_ca1(s, V->blank_edge, &t0)) break;
        V->frames++;
        for (uint32_t j = 0; j < V->burst; j++) { V->blank_reads++; if ((uint16_t)rd(s, s->a[j & 63], 2) != v[j & 63]) V->blank_wrong++; }
        if (now(s) - t0 > blank) V->overran++;
        uint64_t mid = t0 + blank + (frame - blank) / 3, lim = t0 + frame;
        while (now(s) < mid && now(s) < lim) ;
        for (uint32_t j = 0; j < V->burst; j++) { V->disp_reads++; if ((uint16_t)rd(s, s->a[j & 63], 2) != v[j & 63]) V->disp_wrong++; }
        /* every other frame: reads through the whole frame, each counted in the eighth of it it fell in
           (bin 0 holds the blank), so that errors can be placed against the beam (docs/18 M2) */
        if (f & 1) {
            uint64_t t1;
            if (!wait_ca1(s, V->blank_edge, &t1)) break;
            for (uint32_t j = 0; j < 4096; j++) {
                uint64_t e = now(s) - t1;
                if (e >= frame - frame / 16) break;
                uint32_t bin = (uint32_t)(e * 8 / frame);
                V->bin_reads[bin]++;
                if ((uint16_t)rd(s, s->a[j & 63], 2) != v[j & 63]) V->bin_wrong[bin]++;
            }
        }
    }
    wr(s, VIA(12), 1, V_PCR);
    for (int i = 0; i < 64; i++) wr(s, s->a[i], 2, 0);
}

/* ---- 7: back to back against spaced ---------------------------------------------------------------- */
static void space_test(struct st *s)
{
    struct bc_space *S = &s->r->space;
    static const uint32_t gap[BC_GAPS] = { 0, 5, 50 };   /* microseconds: none, a few bus cycles' worth, well clear of any settling */
    uint16_t v[64];
    for (int i = 0; i < 64; i++) { v[i] = (uint16_t)rnd(&s->rng); wr(s, s->a[i], 2, v[i]); }
    for (int g = 0; g < BC_GAPS; g++) {
        S->gap_us[g] = gap[g];
        uint64_t w = us_ticks(s, gap[g]);
        for (int j = 0; j < 1024; j++) {
            if (w) { uint64_t t = now(s) + w; while (now(s) < t) ; }
            S->reads[g]++;
            if ((uint16_t)rd(s, s->a[j & 63], 2) != v[j & 63]) S->wrong[g]++;
        }
        idle(s);
    }
    for (int i = 0; i < 64; i++) wr(s, s->a[i], 2, 0);
}

/* ---- the analysis ----------------------------------------------------------------------------------- */
static uint32_t ppm(uint32_t n, uint32_t of) { return of ? (uint32_t)((uint64_t)n * 1000000u / of) : 0; }
/* a rate for people: "0", "37 ppm" below a tenth of a percent, "4.2%" above */
static const char *rate(char *o, uint32_t n, uint32_t of)
{
    uint32_t p = ppm(n, of);
    if (!n) bc_fmt(o, 12, "0");
    else if (p < 1000u) bc_fmt(o, 12, "%u ppm", p ? p : 1);
    else bc_fmt(o, 12, "%u.%u%%", p / 10000u, p / 1000u % 10u);
    return o;
}
static int is_stray(uint32_t n, uint32_t of) { uint32_t lim = (uint32_t)((uint64_t)of * STRAY_PPM / 1000000u); return n <= (lim > 2 ? lim : 2); }
static int worst(int a, int b) { return a > b ? a : b; }
static int bit_stuck(const struct bc_data *d, int b)   /* 0 none, 1 reads 0, 2 reads 1 */
{
    if (d->n0[b] < STUCK_MIN || d->n1[b] < STUCK_MIN) return 0;
    if (d->e10[b] * 100u >= d->n1[b] * STUCK_PCT && d->e01[b] * 100u <= d->n0[b] * (100u - STUCK_PCT)) return 1;
    if (d->e01[b] * 100u >= d->n0[b] * STUCK_PCT && d->e10[b] * 100u <= d->n1[b] * (100u - STUCK_PCT)) return 2;
    return 0;
}
static int near_all(uint32_t x, uint32_t n) { return n >= STUCK_MIN && x * 100u >= n * STUCK_PCT; }
static int near_none(uint32_t x, uint32_t n) { return x * 100u <= n * (100u - STUCK_PCT); }
/* b follows c: wrong exactly where they differ, the way c is */
static int bit_follows(const struct bc_data *d, int b, int c)
{
    return near_all(d->x01[b][c], d->n01[b][c]) && near_all(d->x10[b][c], d->n10[b][c]);
}
/* b and c joined: v 0 (a 0 wins) or 1 */
static int bits_joined(const struct bc_data *d, int b, int c, int *v)
{
    if (near_all(d->x10[b][c], d->n10[b][c]) && near_all(d->x10[c][b], d->n10[c][b]) && near_none(d->x01[b][c], d->n01[b][c]) && near_none(d->x01[c][b], d->n01[c][b])) { *v = 0; return 1; }
    if (near_all(d->x01[b][c], d->n01[b][c]) && near_all(d->x01[c][b], d->n01[c][b]) && near_none(d->x10[b][c], d->n10[b][c]) && near_none(d->x10[c][b], d->n10[c][b])) { *v = 1; return 1; }
    return 0;
}
static uint32_t bit_err(const struct bc_data *d, int b) { return d->e01[b] + d->e10[b]; }

static void analyse(struct bc_results *r)
{
    struct bc_data *d = &r->data;
    uint32_t bad = d->wrong + d->bhi_wrong + d->blo_wrong + d->bw_wrong + d->ww_wrong, all = d->words + d->bhi + d->blo + d->bw + d->ww;
    d->verdict = !bad ? BC_CLEAN : is_stray(bad, all) ? BC_STRAY : BC_FAIL;
    struct bc_addr *A = &r->addr;
    A->verdict = A->nfind ? BC_FAIL : BC_CLEAN;
    if (A->pairs && A->undecided * 100u >= A->pairs * UNDECIDED_PCT) A->verdict = A->nfind ? BC_FAIL : BC_UNSURE;
    else if (!A->nfind && A->undecided) A->verdict = BC_STRAY;
    struct bc_chips *C = &r->chips;
    uint32_t vw = 0, vr = 0;
    for (int k = 0; k < 7; k++) { vw += C->via[k].wrong; vr += C->via[k].reads; }
    vw += C->pb_out_wrong + C->pa_out_wrong; vr += C->pb_reads + C->pa_reads;
    C->via_verdict = !vw ? BC_CLEAN : is_stray(vw, vr) ? BC_STRAY : BC_FAIL;
    uint32_t sw = C->rr0_stable_changes[0] + C->rr0_stable_changes[1] + C->rr12.wrong + C->rr13.wrong + C->rr2[0].wrong,
             sr = C->rr0[0].reads + C->rr0[1].reads + C->rr12.reads + C->rr13.reads + C->rr2[0].reads;
    struct bc_rom *M = &r->rom;
    if (M->verdict != BC_SKIP) M->verdict = M->disagree ? (is_stray(M->disagree, M->reads) ? BC_STRAY : BC_FAIL) : M->sum_ok ? BC_CLEAN : BC_INFO;
    C->scc_verdict = !sw ? BC_CLEAN : is_stray(sw, sr) ? BC_STRAY : BC_FAIL;
    C->in_verdict = BC_INFO;
    struct bc_retry *T = &r->retry;
    T->verdict = !T->one_wrong ? BC_CLEAN : (T->three_wrong * 10u <= T->one_wrong || T->two_wrong * 10u <= T->one_wrong) ? BC_INFO : BC_FAIL;
    struct bc_video *V = &r->video;
    V->verdict = V->ok != 1 ? BC_SKIP : (V->blank_wrong + V->disp_wrong) ? BC_INFO : BC_CLEAN;
    struct bc_space *S = &r->space;
    S->verdict = (S->wrong[0] + S->wrong[1] + S->wrong[2]) ? BC_INFO : BC_CLEAN;
    r->verdict = worst(worst(d->verdict, A->verdict), worst(C->via_verdict, C->scc_verdict));
    if (M->verdict == BC_FAIL || M->verdict == BC_STRAY) r->verdict = worst(r->verdict, M->verdict);
    if (r->timeouts) r->verdict = BC_FAIL;
}

void bc_run(const struct bc_bus *bus, const struct bc_config *cfg, struct bc_results *r)
{
    struct st s;
    uint8_t *p = (uint8_t *)r;
    for (uint32_t i = 0; i < sizeof *r; i++) p[i] = 0;
    s.bus = bus; s.cfg = cfg; s.r = r; r->hz = bus->hz;
    s.rng = cfg->seed ? cfg->seed : 0x1d872b41u; s.trng = 0x9e3779b9u;
    uint32_t f0 = bus->faults ? bus->faults(bus->ctx) : 0;
    uint64_t t0 = now(&s);
    via_quiet(&s);
    if (cfg->fb_base) for (uint32_t i = 0; i < 342 * 64; i += 2) wr(&s, cfg->fb_base + i, 2, MARK_SCREEN);
    if (cfg->snd_base) for (uint32_t i = 0; i < 370 * 2; i += 2) wr(&s, cfg->snd_base + i, 2, MARK_SOUND);
    /* The address lines first (test 2): its words survive a bad data line and noise. Then the data tests'
       addresses are chosen around what it found, so that they measure the data lines alone: a line stuck
       at v gets v in every address, a line following another the other's value, two joined lines the
       winning value, and RAM that repeats is used below where it repeats. */
    stage(&s, 1, "the address lines and how much RAM answers");
    addr_test(&s);
    uint32_t R = cfg->ram_size, force1 = 0, force0 = 0, same_n[24], same_m[24], nsame = 0;
    s.lo = 0x1000; s.hi = R >= 0x20000u ? R - TOP_KEEP : R / 2;
    for (int i = 0; i < r->addr.nfind; i++) {
        const struct bc_afind *f = &r->addr.find[i];
        if (f->kind == BC_A_SIZE && (1u << f->n) >= 0x20000u) s.hi = (1u << f->n) - TOP_KEEP;
        else if (f->kind == BC_A_SIZE) s.hi = (1u << f->n) / 2;
        else if (f->kind == BC_A_STUCK) { if (f->v) force1 |= 1u << f->n; else force0 |= 1u << f->n; }
        else if (f->kind == BC_A_JOINED) { uint32_t m2 = 1u << f->n | 1u << f->m; if (f->v) force1 |= m2; else force0 |= m2; }
        else if (f->kind == BC_A_FOLLOWS && nsame < 24) { same_n[nsame] = f->n; same_m[nsame++] = f->m; }
    }
    if (s.hi <= s.lo + 2 * BC_NADDR) s.hi = s.lo + 4 * BC_NADDR;
    uint32_t stride = ((s.hi - s.lo) / BC_NADDR) & ~1u;
    for (int i = 0; i < BC_NADDR; i++) {
        uint32_t a = 0;
        for (int tries = 0; tries < 64; tries++) {
            a = s.lo + (uint32_t)i * stride + ((rnd(&s.rng) % (stride / 2 ? stride / 2 : 1)) & ~1u);
            a = (a | force1) & ~force0;
            for (uint32_t k = 0; k < nsame; k++) a = (a & ~(1u << same_n[k])) | ((a >> same_m[k] & 1u) << same_n[k]);
            if (a >= s.lo && a < s.hi) break;
        }
        s.a[i] = a;
    }
    stage(&s, 2, "the data lines, through the RAM");
    data_test(&s);
    stage(&s, 3, "the VIA's and the SCC's registers");
    chips_test(&s);
    stage(&s, 4, "the board's ROM");
    rom_test(&s);
    stage(&s, 5, "what a retry would cure");
    retry_test(&s);
    stage(&s, 6, "reads back to back and spaced apart");
    space_test(&s);
    stage(&s, 7, "reads in the blank and while the video reads");
    video_test(&s);
    if (cfg->snd_base) for (uint32_t i = 0; i < 370 * 2; i += 2) wr(&s, cfg->snd_base + i, 2, 0x8000);   /* the sound buffer at the middle level */
    via_quiet(&s);
    r->ticks = now(&s) - t0;
    r->timeouts = bus->faults ? bus->faults(bus->ctx) - f0 : 0;
    analyse(r);
}

/* ---- text ------------------------------------------------------------------------------------------- */
static int put_num(char *o, uint32_t cap, uint32_t at, uint64_t v, int base, int upper, int width, int left, int zero, int neg)
{
    char t[24]; int n = 0;
    do { int dg = (int)(v % (uint64_t)base); t[n++] = (char)(dg < 10 ? '0' + dg : (upper ? 'A' : 'a') + dg - 10); v /= (uint64_t)base; } while (v);
    if (neg) t[n++] = '-';
    int pad = width > n ? width - n : 0, k = 0;
    if (!left) for (; k < pad; k++) if (at + (uint32_t)k + 1 < cap) o[at + (uint32_t)k] = zero ? '0' : ' ';
    for (int i = n - 1; i >= 0; i--, k++) if (at + (uint32_t)k + 1 < cap) o[at + (uint32_t)k] = t[i];
    if (left) for (int j = 0; j < pad; j++, k++) if (at + (uint32_t)k + 1 < cap) o[at + (uint32_t)k] = ' ';
    return k;
}
static int vfmt(char *o, uint32_t cap, const char *f, va_list ap)
{
    uint32_t n = 0;
    for (; *f; f++) {
        if (*f != '%') { if (n + 1 < cap) o[n] = *f; n++; continue; }
        f++;
        int left = 0, zero = 0, width = 0, lng = 0;
        if (*f == '-') { left = 1; f++; }
        if (*f == '0') { zero = 1; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == 'l') { lng = 1; f++; }
        switch (*f) {
        case 'd': { long long v = lng ? (long long)va_arg(ap, int64_t) : va_arg(ap, int); n += (uint32_t)put_num(o, cap, n, (uint64_t)(v < 0 ? -v : v), 10, 0, width, left, zero, v < 0); break; }
        case 'u': n += (uint32_t)put_num(o, cap, n, lng ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t), 10, 0, width, left, zero, 0); break;
        case 'x': case 'X': n += (uint32_t)put_num(o, cap, n, lng ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t), 16, *f == 'X', width, left, zero, 0); break;
        case 'c': { char c = (char)va_arg(ap, int); if (n + 1 < cap) o[n] = c; n++; break; }
        case 's': {
            const char *str = va_arg(ap, const char *); if (!str) str = "";
            int len = 0; while (str[len]) len++;
            int pad = width > len ? width - len : 0;
            if (!left) for (int i = 0; i < pad; i++) { if (n + 1 < cap) o[n] = ' '; n++; }
            for (int i = 0; i < len; i++) { if (n + 1 < cap) o[n] = str[i]; n++; }
            if (left) for (int i = 0; i < pad; i++) { if (n + 1 < cap) o[n] = ' '; n++; }
            break;
        }
        case '%': if (n + 1 < cap) o[n] = '%'; n++; break;
        default: break;
        }
    }
    if (cap) o[n < cap ? n : cap - 1] = 0;
    return (int)n;
}
int bc_fmt(char *out, uint32_t cap, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); int n = vfmt(out, cap, fmt, ap); va_end(ap); return n;
}

struct txt { char (*lines)[BC_COLS + 1]; int n; char *det; uint32_t cap, len; };
static void det_add(struct txt *t, const char *s)
{
    uint32_t n = 0; while (s[n]) n++;
    while (n && s[n - 1] == ' ') n--;                    /* the screen's padding is not kept */
    for (uint32_t i = 0; i < n && t->len + 2 < t->cap; i++) t->det[t->len++] = s[i];
    if (t->len + 1 < t->cap) t->det[t->len++] = '\n';
    t->det[t->len < t->cap ? t->len : t->cap - 1] = 0;
}
/* a screen line (also the first part of the detail text) */
int bc_clipped; char bc_clipped_text[160];                                     /* screen lines cut at the right margin (the host test wants none) */
static void line(struct txt *t, const char *fmt, ...)
{
    char b[160]; va_list ap; va_start(ap, fmt); vfmt(b, sizeof b, fmt, ap); va_end(ap);
    if (t->n < BC_ROWS) { int i = 0; for (; i < BC_COLS && b[i]; i++) t->lines[t->n][i] = b[i]; t->lines[t->n][i] = 0; t->n++; if (b[i]) { bc_clipped++; bc_fmt(bc_clipped_text, sizeof bc_clipped_text, "%s", b); } }
    det_add(t, b);
}
/* a detail line only (the log and the card) */
static void det(struct txt *t, const char *fmt, ...)
{
    char b[200]; va_list ap; va_start(ap, fmt); vfmt(b, sizeof b, fmt, ap); va_end(ap);
    det_add(t, b);
}
static const char *vname(int v)
{
    static const char *const n[] = { "SKIP", "CLEAN", "INFO", "STRAY", "UNSURE", "FAIL" };
    return n[v >= 0 && v <= BC_FAIL ? v : 0];
}
/* a test's line: the text, then the verdict at the right margin */
static void tline(struct txt *t, int v, const char *fmt, ...)
{
    char b[160]; va_list ap; va_start(ap, fmt); vfmt(b, sizeof b, fmt, ap); va_end(ap);
    char o[BC_COLS + 1]; int i = 0;
    for (; b[i] && i < BC_COLS - 7; i++) o[i] = b[i];
    if (b[i]) { bc_clipped++; bc_fmt(bc_clipped_text, sizeof bc_clipped_text, "%s", b); }
    for (; i < BC_COLS - 6; i++) o[i] = ' ';
    const char *n = vname(v); int k = 0;
    for (; n[k] && i < BC_COLS; k++) o[i++] = n[k];
    o[i] = 0;
    line(t, "%s", o);
}
/* a count in three characters, for the per-bit table: "  .", "  7", "999", " 4k", "99k", "12M" */
static void cell3(char *o, uint32_t v)
{
    if (!v) bc_fmt(o, 4, "  .");
    else if (v < 1000) bc_fmt(o, 4, "%3u", v);
    else if (v < 100000) bc_fmt(o, 4, "%2uk", v / 1000);
    else if (v < 1000000) bc_fmt(o, 4, ".%uM", v / 100000);
    else bc_fmt(o, 4, "%2uM", v / 1000000 > 99 ? 99 : v / 1000000);
}
static void kn(char *o, uint32_t v) { if (v < 10000) bc_fmt(o, 8, "%u", v); else bc_fmt(o, 8, "%uk", v / 1000); }   /* a count in five characters at most */
static void us1(char *o, uint32_t ns)            /* microseconds in four characters at most: "1.2", "44.4", "6427", "15ms" */
{
    if (ns < 100000u) bc_fmt(o, 12, "%u.%u", ns / 1000, ns % 1000 / 100);
    else if (ns < 10000000u) bc_fmt(o, 12, "%u", ns / 1000);
    else bc_fmt(o, 12, "%ums", ns / 1000000 > 999 ? 999 : ns / 1000000);
}
static int dpin(int b) { return b <= 4 ? 5 - b : 69 - b; }            /* 68000 DIP-64: D0-D4 on pins 5-1, D5-D15 on 64-54 */
static int apin(int n) { return n <= 20 ? 28 + n : 29 + n; }          /* A1-A20 on pins 29-48, A21-A23 on 50-52 */
static const char *modename(int w) { return w == 0 ? "reading" : w == 2 ? "writing" : "reading and writing"; }

/* The summary in plain words, as sentences into buf. The order is the order of what to look at first:
   a board that does not answer; a data line that is broken (stuck, following another, joined); an
   address line; reads that fail on many lines (timing, or noise) and what makes them better or worse;
   the chips; the transport's timeouts; or all clean. */
static void summary(const struct bc_config *cfg, const struct bc_results *r, char *buf, uint32_t cap)
{
    const struct bc_data *d = &r->data; const struct bc_addr *A = &r->addr; const struct bc_chips *C = &r->chips;
    uint32_t n = 0;
#define SAY(...) do { n += (uint32_t)bc_fmt(buf + n, cap > n ? cap - n : 0, __VA_ARGS__); } while (0)
    buf[0] = 0;
    uint32_t reads = d->words + d->bhi + d->blo;
    if (d->words && d->ffff_reads * 10u >= d->words * 9u) {
        SAY("No answer from the board: %u of %u word reads came back $FFFF%s. Is the PiStorm seated and the Mac switched on? ", d->ffff_reads, d->words,
            r->timeouts ? " and cycles timed out" : "");
        return;
    }
    int reads_bad = d->verdict == BC_FAIL && ppm(d->wrong, d->words) >= 10000u;   /* one word in a hundred */
    int said = 0;
    /* data lines: stuck, following, joined */
    int lines_named = 0; uint32_t named = 0;
    for (int b = 15; b >= 0; b--) {
        int k = bit_stuck(d, b);
        if (k) { SAY("D%d always reads %d: a stuck data line (socket pin %d, the Pi's GPIO %d). ", b, k - 1, dpin(b), b + 8); lines_named++; named |= 1u << b; }
    }
    for (int b = 15; b >= 0; b--)
        for (int c = 15; c >= 0; c--) {
            int v;
            if (c == b || (named >> b & 1)) continue;
            if (c < b && !(named >> c & 1) && bits_joined(d, b, c, &v)) { SAY("D%d and D%d are joined (a %d wins): socket pins %d and %d. ", b, c, v, dpin(b), dpin(c)); lines_named++; named |= 1u << b | 1u << c; }
            else if (bit_follows(d, b, c)) { SAY("D%d follows D%d: a short (socket pins %d, %d). ", b, c, dpin(b), dpin(c)); lines_named++; named |= 1u << b; }
        }
    said |= lines_named;
    /* with a data line named, the words read wrong because of it explain the retries, the spacing, the blank,
       the word writes and the chips' upper byte as well: those are said only if errors remain on other lines */
    int explained = 0;
    if (lines_named) {
        uint32_t rest = 0, all = 0;
        for (int b = 0; b < 16; b++) { all += bit_err(d, b); if (!(named >> b & 1)) rest += bit_err(d, b); }
        uint32_t vrest = 0; for (int b = 0; b < 8; b++) if (!(named >> (b + 8) & 1)) vrest += C->via_bit_wrong[b];
        explained = rest * 100u <= all && !vrest;
        if (explained) SAY("Everything else that read wrong follows from %s. ", lines_named > 1 ? "those lines" : "that line");
    }
    /* address lines */
    for (int i = 0; i < A->nfind; i++) {
        const struct bc_afind *f = &A->find[i];
        if (f->kind == BC_A_SIZE) {
            uint32_t kb = (1u << f->n) >> 10;
            SAY("The memory repeats every %u KB: lines A%u up are not decoded, so the board has %u KB, not the %u KB assumed (%s). ", kb, f->n, kb, cfg->ram_size >> 10,
                cfg->plus ? "mac.board=" : "mac.seram=");
        } else if (f->kind == BC_A_JOINED)
            SAY("A%u and A%u are joined (a %u wins) when %s: look for a bridge between socket pins %d and %d. ", f->n, f->m, f->v, modename(f->mode), apin(f->n), apin(f->m));
        else if (f->kind == BC_A_FOLLOWS)
            { if (f->m) SAY("A%u follows A%u when %s: a short between them or at the latch (socket pins %d, %d). ", f->n, f->m, modename(f->mode), apin(f->n), apin(f->m));
              else SAY("A%u follows another line when %s (it reads as 0 at some addresses and 1 at others): a short to a neighbour (socket pin %d). ", f->n, modename(f->mode), apin(f->n)); }
        else if (f->kind == BC_A_STUCK)
            { if (f->mode == 1) SAY("A%u does not reach the RAM as both 0 and 1, reading and writing: stuck, not connected, or following another line (from the Pi's side the three look the same; docs/08 item 68) (socket pin %d). ", f->n, apin(f->n));
              else SAY("A%u is stuck at %u when %s (socket pin %d). ", f->n, f->v, modename(f->mode), apin(f->n)); }
        else
            SAY("A%u gives results no single fault explains. ", f->n);
        said = 1;
    }
    if (A->nfind && reads_bad) SAY("(With reads this unreliable the address findings are uncertain.) ");
    /* reads that fail without a named line */
    uint32_t berr = 0, nb = 0, top3 = 0, e[16];
    for (int b = 0; b < 16; b++) { e[b] = bit_err(d, b); berr += e[b]; if (e[b] && e[b] * 1000u >= (d->n0[b] + d->n1[b])) nb++; }
    {
        uint32_t x[16]; for (int b = 0; b < 16; b++) x[b] = e[b];
        for (int k = 0; k < 3; k++) { int bi = 0; for (int b = 1; b < 16; b++) if (x[b] > x[bi]) bi = b; top3 += x[bi]; x[bi] = 0; }
    }
    if (d->verdict == BC_FAIL && !lines_named) {
        uint32_t fw = d->first_wrong ? d->first_wrong : 1, rep = d->repeat * 100u / fw;
        if (berr && top3 * 100u >= berr * FEW_BITS_PCT) {
            /* the errors on one to three lines, not stuck: an intermittent contact */
            int sh = 0;
            SAY("Reads fail mostly on");
            uint32_t x[16]; for (int b = 0; b < 16; b++) x[b] = e[b];
            for (int k = 0; k < 3; k++) { int bi = 0; for (int b = 1; b < 16; b++) if (x[b] > x[bi]) bi = b; if (!x[bi] || x[bi] * 10u < top3) break; SAY("%s D%d (pin %d)", sh ? "," : "", bi, dpin(bi)); sh = 1; x[bi] = 0; }
            SAY(", the other lines clean, and are right at other times: an intermittent line (a socket joint, a contact), not timing. ");
        } else {
            uint32_t p = ppm(d->wrong, d->words);
            SAY("Reads of RAM fail on %u of 16 bits, %u.%u%% of words, ", nb, p / 10000u, p / 1000u % 10u);
            if (rep < RANDOM_PCT) SAY("mostly at random (a second read repeats the wrong word %u%% of the time): a timing problem, not a broken line. ", rep);
            else if (rep > 100u - RANDOM_PCT) SAY("the same way each time (repeated %u%%): a fault in the read path or the memory, not noise. ", rep);
            else SAY("partly at random (repeated %u%%): a marginal read, most likely timing. ", rep);
            uint32_t ew = d->eq_addr + d->eq_hireg;
            if (d->wrong && ew * 100u >= d->wrong * EARLY_PCT)
                SAY("%u%% of the wrong words were what the Pi had just sent: the data was read before the board drove it. ", ew * 100u / d->wrong);
            if (berr >= 20) {
                uint32_t up = 0; for (int b = 8; b < 16; b++) up += e[b];
                if (up * 100u >= berr * FEW_BITS_PCT) SAY("Mostly the upper byte (D8-D15, UDS). ");
                else if ((berr - up) * 100u >= berr * FEW_BITS_PCT) SAY("Mostly the lower byte (D0-D7, LDS). ");
            }
        }
        said = 1;
    } else if (d->verdict == BC_FAIL && d->wrong && !explained) {
        SAY("Other reads fail as well (%u wrong words in %u). ", d->wrong, d->words);
    }
    if (A->verdict == BC_UNSURE) { SAY("The address lines could not be judged: %u of %u pairs had no clear answer. ", A->undecided, A->pairs); said = 1; }
    if (d->bw_wrong && d->bw) { SAY("%u of %u byte writes did not land as written. ", d->bw_wrong, d->bw); said = 1; }
    /* retries, spacing, video: only when there is something to cure */
    const struct bc_retry *T = &r->retry;
    if (T->one_wrong >= 3 && !explained) {
        uint32_t p1 = ppm(T->one_wrong, T->values), p2 = ppm(T->two_wrong, T->values), p3 = ppm(T->three_wrong, T->values), best = p2 < p3 ? p2 : p3;
        char q1[12], q2[12], q3[12];
        rate(q1, T->one_wrong, T->values); rate(q2, T->two_wrong, T->values); rate(q3, T->three_wrong, T->values);
        if (best * 10u <= p1 && best <= 1000u) SAY("A retry would help: one read %s wrong, two that agree %s, three voting %s. ", q1, q2, q3);
        else if (best * 3u <= p1) SAY("A retry would help but not cure it: one read %s wrong, two that agree %s, three voting %s. ", q1, q2, q3);
        else SAY("A retry would not cure it: one read %s wrong, two that agree %s, three voting %s. ", q1, q2, q3);
        said = 1;
    }
    const struct bc_space *S = &r->space;
    if (S->wrong[0] >= 10 && !explained) {
        uint32_t p0 = ppm(S->wrong[0], S->reads[0]), pg = ppm(S->wrong[BC_GAPS - 1], S->reads[BC_GAPS - 1]);
        char q0[12], qg[12];
        rate(q0, S->wrong[0], S->reads[0]); rate(qg, S->wrong[BC_GAPS - 1], S->reads[BC_GAPS - 1]);
        if (pg * 3u <= p0) SAY("Reads %u us apart fail far less (%s against %s back to back): a cycle started too soon after the last. ", S->gap_us[BC_GAPS - 1], qg, q0);
        else SAY("Spacing reads %u us apart does not help (%s against %s back to back). ", S->gap_us[BC_GAPS - 1], qg, q0);
    }
    const struct bc_video *V = &r->video;
    if (V->ok == 1 && V->disp_wrong + V->blank_wrong >= 20 && !explained) {
        uint32_t pd = ppm(V->disp_wrong, V->disp_reads), pb = ppm(V->blank_wrong, V->blank_reads);
        char qd[12], qb[12];
        rate(qd, V->disp_wrong, V->disp_reads); rate(qb, V->blank_wrong, V->blank_reads);
        if (pd >= pb * CONTEND_X) SAY("Reads fail more while the video reads the RAM (%s, %s in the blank): the read ends too early when the board delays it for the video. ", qd, qb);
        else SAY("The blank does not help (%s in it, %s while the video runs): not the video's memory cycles. ", qb, qd);
    }
    /* what the wrong words were: the video's or the sound's fetch */
    if (d->wrong >= 20 && !explained) {
        if (d->eq_screen * 100u >= d->wrong * EARLY_PCT) SAY("%u%% of the wrong words were the screen's mark: the video's fetch was still on the RAM's bus. ", d->eq_screen * 100u / d->wrong);
        if (d->eq_sound * 100u >= d->wrong * EARLY_PCT) SAY("%u%% of the wrong words were the sound buffer's mark: the sound fetch's data. ", d->eq_sound * 100u / d->wrong);
    }
    if (d->ww_wrong && d->ww && !explained) { SAY("%u of %u word writes read back wrong even by the commonest of fifteen reads: the memory holds them wrong (a write fault). ", d->ww_wrong, d->ww); said = 1; }
    /* the ROM against the RAM: no interleave there */
    const struct bc_rom *M = &r->rom;
    if (M->verdict != BC_SKIP && d->verdict == BC_FAIL && !lines_named) {
        if (!M->disagree) SAY("The board's ROM reads clean (%u reads%s) while RAM reads fail: the fault is in the RAM's cycles (its /DTACK, the video's share), not only in where the PiStorm samples. ",
                              M->reads, M->sum_ok ? ", its checksum right" : "");
        else SAY("The board's ROM reads fail too (%u of %u): the PiStorm takes the data too early for this board, whatever answers (docs/18 H1). ", M->disagree, M->reads);
    } else if (M->verdict == BC_FAIL && d->verdict != BC_FAIL) { SAY("The ROM's reads disagree (%u of %u) though RAM reads are clean. ", M->disagree, M->reads); said = 1; }
    /* the SCC's spacing (M8, M9) */
    {
        uint32_t c0 = C->sp_second[0][0] + C->sp_second[1][0], c5 = C->sp_second[0][BC_SGAPS - 1] + C->sp_second[1][BC_SGAPS - 1];
        if (c0 >= 10 && c0 >= 3 * c5 + 3) {
            int g = 1; while (g < BC_SGAPS - 1 && C->sp_second[0][g] + C->sp_second[1][g] > c0 / 3) g++;
            SAY("SCC reads soon after another one disturb it (%u jumps back to back, %u at %u us; calm from %u ns): the SCC needs time between accesses on this board (docs/18 H2). ",
                c0, c5, C->sp_ns[BC_SGAPS - 1] / 1000, C->sp_ns[g]);
            said = 1;
        }
        if (C->rr2[1].wrong && !C->rr2[0].wrong) { SAY("RR2 reads back wrong back to back (%u of %u) and right 3 us apart. ", C->rr2[1].wrong, C->rr2[1].reads); said = 1; }
    }
    /* the chips */
    if ((C->via_verdict == BC_FAIL || C->scc_verdict == BC_FAIL) && !explained) {
        if (d->verdict == BC_CLEAN) SAY("RAM reads are clean but the chips' are not:");
        else SAY("The chips' reads are wrong too:");
        if (C->via_verdict == BC_FAIL) { uint32_t w = 0, n2 = 0; for (int k = 0; k < 7; k++) { w += C->via[k].wrong; n2 += C->via[k].reads; } SAY(" the VIA's registers %u of %u%s", w, n2, C->scc_verdict == BC_FAIL ? ";" : ""); }
        if (C->scc_verdict == BC_FAIL) SAY(" the SCC %u RR0 jumps, RR12/13 %u of %u wrong", C->rr0_stable_changes[0] + C->rr0_stable_changes[1], C->rr12.wrong + C->rr13.wrong, C->rr12.reads + C->rr13.reads);
        SAY(". ");
        said = 1;
    }
    if (r->timeouts) { SAY("%u bus cycles timed out (the CPLD never finished them). ", r->timeouts); said = 1; }
    if (reads_bad && !A->nfind && !lines_named) SAY("This screen was written by the Pi: if it reads cleanly, writes work and only reads fail. ");
    if (!said) {
        if (r->verdict == BC_CLEAN) SAY("All clean: %u reads of RAM, the VIA and the SCC, no errors. The bus is good. ", reads + C->pb_reads + C->pa_reads + C->rr0[0].reads + C->rr0[1].reads);
        else SAY("A few stray errors (%u in %u reads): within what a noisy bus gives; mac.buscheck=loop runs it again and again. ", d->wrong + d->bhi_wrong + d->blo_wrong, reads);
    }
    if (C->phase_reads) SAY("Phase read D0 %u: note it with each power-on (docs/18 M10). ", C->phase_d0 * 2 >= C->phase_reads);
#undef SAY
}

const char *bc_summary(const struct bc_config *cfg, const struct bc_results *r)
{
    static char b[1600];
    summary(cfg, r, b, sizeof b);
    return b;
}
const char *bc_headline(const struct bc_config *cfg, const struct bc_results *r)
{
    static char b[1600];
    summary(cfg, r, b, sizeof b);
    for (int i = 0; b[i]; i++)
        if (b[i] == '.' && b[i + 1] == ' ' && b[i + 2] >= 'A' && b[i + 2] <= 'Z') { b[i + 1] = 0; break; }   /* the first sentence */
    return b;
}

int bc_report(const struct bc_config *cfg, const struct bc_results *r, char lines[BC_ROWS][BC_COLS + 1], char *detail, uint32_t cap)
{
    struct txt t = { lines, 0, detail, cap, 0 };
    if (cap) detail[0] = 0;
    const struct bc_data *d = &r->data; const struct bc_addr *A = &r->addr; const struct bc_chips *C = &r->chips;
    char a[12], b[12], c[12];
    line(&t, "AURORA68 BUS CHECK  run %u  %s", cfg->run, cfg->version);
    line(&t, "%s  RAM %u KB (%s)", cfg->board, cfg->ram_size >> 10, cfg->ram_from);
    line(&t, "%s", cfg->link);
    line(&t, "CPLD %s", cfg->cpld ? cfg->cpld : "usercode not read");
    line(&t, "%u.%u s, %u bus cycles, %u timeouts", ticks_us(r, r->ticks) / 1000000u, ticks_us(r, r->ticks) / 100000u % 10u, r->cycles, r->timeouts);
    line(&t, "----------------------------------------------------------------");
    tline(&t, d->verdict, "1 RAM DATA  %ux%u addr: %u/%u word reads wrong", d->patterns, d->addrs, d->wrong, d->words);
    line(&t, "  bytes hi %u/%u lo %u/%u, writes b %u/%u w %u/%u", d->bhi_wrong, d->bhi, d->blo_wrong, d->blo, d->bw_wrong, d->bw, d->ww_wrong, d->ww);
    line(&t, "  2nd read of a wrong word: same %u, right %u, other %u", d->repeat, d->transient, d->other);
    {
        char k1[8], k2[8], k3[8], k4[8], k5[8], k6[8], k7[8];
        kn(k1, d->eq_addr); kn(k2, d->eq_hireg); kn(k3, d->eq_prev); kn(k4, d->eq_ffff); kn(k5, d->eq_0000); kn(k6, d->eq_screen); kn(k7, d->eq_sound);
        line(&t, "  wrong were: adr %s hi %s prev %s FFFF %s 0 %s scr %s snd %s", k1, k2, k3, k4, k5, k6, k7);
    }
    line(&t, "  one word read %ux: %u%s values, %u right%s", d->one_reads, d->one_distinct > 15 ? 16 : d->one_distinct, d->one_distinct > 15 ? "+" : "",
         d->one_right, d->one_commonest_right ? "" : ", not the commonest");
    {
        char f[64]; uint32_t k = 0; f[0] = 0;
        for (int i = 0; i < A->nfind && k < 40; i++) {
            const struct bc_afind *x = &A->find[i];
            if (x->kind == BC_A_SIZE) k += (uint32_t)bc_fmt(f + k, sizeof f - k, "repeats at A%u ", x->n);
            else if (x->kind == BC_A_JOINED) k += (uint32_t)bc_fmt(f + k, sizeof f - k, "A%u&A%u ", x->n, x->m);
            else if (x->kind == BC_A_FOLLOWS) k += (uint32_t)(x->m ? bc_fmt(f + k, sizeof f - k, "A%u=A%u ", x->n, x->m) : bc_fmt(f + k, sizeof f - k, "A%u=A? ", x->n));
            else if (x->kind == BC_A_STUCK) k += (uint32_t)bc_fmt(f + k, sizeof f - k, "A%u=%u ", x->n, x->v);
            else k += (uint32_t)bc_fmt(f + k, sizeof f - k, "A%u? ", x->n);
        }
        if (!A->nfind) tline(&t, A->verdict, "2 RAM ADDR  A1-A%d: %u pairs good, %u undecided", A->top, A->pairs - A->bad - A->undecided, A->undecided);
        else tline(&t, A->verdict, "2 RAM ADDR  A1-A%d: %s", A->top, f);
    }
    tline(&t, C->via_verdict, "3 VIA REGS x%u wrong: %u %u %u %u %u %u %u", C->via[0].reads, C->via[0].wrong, C->via[1].wrong, C->via[2].wrong, C->via[3].wrong,
          C->via[4].wrong, C->via[5].wrong, C->via[6].wrong);
    {
        char ph[24];
        if (C->phase_reads) bc_fmt(ph, sizeof ph, "D0 %u (%u/%u)", C->phase_d0 * 2 >= C->phase_reads, C->phase_d0, C->phase_reads);
        else bc_fmt(ph, sizeof ph, "no phase");
        line(&t, "  outs bad PB %u PA %u /%u  IFR 2x %u/%u  %s", C->pb_out_wrong, C->pa_out_wrong, C->pb_reads, C->ifr_differ, C->ifr_pairs, ph);
    }
    {
        char k3[8], k4[8], k5[8], k6[8], k7[8];
        kn(k3, C->pb_changes[3]); kn(k4, C->pb_changes[4]); kn(k5, C->pb_changes[5]); kn(k6, C->pb_changes[6]); kn(k7, C->pa_changes[7]);
        tline(&t, C->in_verdict, "  in changes PB3 %s PB4 %s PB5 %s PB6%s %s PA7 %s", k3, k4, k5, cfg->plus ? "/H4" : "", k6, k7);
    }
    tline(&t, C->scc_verdict, "  SCC RR0 values B %u A %u, jumps %u %u, pins %u %u", C->rr0[0].nval, C->rr0[1].nval, C->rr0_stable_changes[0], C->rr0_stable_changes[1],
          C->rr0_line_changes[0], C->rr0_line_changes[1]);
    line(&t, "  SCC RR12 %u RR13 %u /%u, RR2 3us %u b2b %u /%u wrong", C->rr12.wrong, C->rr13.wrong, C->rr12.reads, C->rr2[0].wrong, C->rr2[1].wrong, C->rr2[0].reads);
    {
        char row[80]; uint32_t q = (uint32_t)bc_fmt(row, sizeof row, "  SCC pairs x%u, 2nd read jumps:", C->sp_pairs);
        for (int g = 0; g < BC_SGAPS; g++) q += (uint32_t)bc_fmt(row + q, sizeof row - q, " %u", C->sp_second[0][g] + C->sp_second[1][g]);
        line(&t, "%s", row);
    }
    {
        const struct bc_rom *M = &r->rom;
        if (M->verdict == BC_SKIP) tline(&t, BC_SKIP, "  ROM none");
        else tline(&t, M->verdict, "  ROM %u reads %u odd, %u KB sum %s", M->reads, M->disagree, M->sum_kb, M->sum_ok ? "= checksum" : "<> checksum");
    }
    line(&t, "4 TIMING us   min  med  max     n |         min  med  max");
    {
        static const char *const nm[BC_KINDS] = { "RAM rd w", "RAM wr w", "RAM rd b", "RAM wr b", "VIA rd", "VIA wr", "SCC rd", "SCC wr" };
        for (int k = 0; k < 4; k++) {
            const struct bc_time *x = &r->t[k], *y = &r->t[k + 4];
            char a2[12], b2[12], c2[12];
            us1(a, ticks_ns(r, x->min)); us1(b, ticks_ns(r, median(x))); us1(c, ticks_ns(r, x->max));
            us1(a2, ticks_ns(r, y->min)); us1(b2, ticks_ns(r, median(y))); us1(c2, ticks_ns(r, y->max));
            line(&t, "  %-8s %5s%5s%5s %6u | %-6s %5s%5s%5s", nm[k], a, b, c, x->n, nm[k + 4], a2, b2, c2);
        }
    }
    {
        const struct bc_retry *T = &r->retry;
        uint32_t two10 = T->values ? T->two_reads * 10u / T->values : 0;
        char q1[12], q2[12], q3[12];
        tline(&t, T->verdict, "5 RETRY 1 rd %s, 2 agree %s (%u.%u rd), 3 vote %s", rate(q1, T->one_wrong, T->values), rate(q2, T->two_wrong, T->values),
              two10 / 10, two10 % 10, rate(q3, T->three_wrong, T->values));
    }
    {
        const struct bc_video *V = &r->video;
        char q1[12], q2[12];
        if (V->ok == 1) tline(&t, V->verdict, "6 VIDEO wrong: blank %s display %s of %u, %u.%u ms", rate(q1, V->blank_wrong, V->blank_reads), rate(q2, V->disp_wrong, V->disp_reads),
                         V->disp_reads, V->frame_us / 1000, V->frame_us / 100 % 10);
        else if (V->ok == 2) tline(&t, BC_SKIP, "6 VIDEO CA1 implausible: frame %u us, blank %u", V->frame_us, V->blank_us);
        else tline(&t, BC_SKIP, "6 VIDEO no vertical blanking seen on VIA CA1");
    }
    {
        const struct bc_space *S = &r->space;
        char q0[12], q1[12], q2[12];
        tline(&t, S->verdict, "7 SPACING wrong: b2b %s, %uus %s, %uus %s", rate(q0, S->wrong[0], S->reads[0]), S->gap_us[1], rate(q1, S->wrong[1], S->reads[1]),
              S->gap_us[2], rate(q2, S->wrong[2], S->reads[2]));
    }
    /* the per-bit table: 16 columns */
    {
        char row[BC_COLS + 1]; uint32_t k;
        k = (uint32_t)bc_fmt(row, sizeof row, "BIT   ");
        for (int bi = 15; bi >= 0; bi--) k += (uint32_t)bc_fmt(row + k, sizeof row - k, "%3d", bi);
        line(&t, "%s", row);
        k = (uint32_t)bc_fmt(row, sizeof row, "0->1  ");
        for (int bi = 15; bi >= 0; bi--) { cell3(a, d->e01[bi]); k += (uint32_t)bc_fmt(row + k, sizeof row - k, "%s", a); }
        line(&t, "%s", row);
        k = (uint32_t)bc_fmt(row, sizeof row, "1->0  ");
        for (int bi = 15; bi >= 0; bi--) { cell3(a, d->e10[bi]); k += (uint32_t)bc_fmt(row + k, sizeof row - k, "%s", a); }
        line(&t, "%s", row);
        k = (uint32_t)bc_fmt(row, sizeof row, "ERR%%  ");
        for (int bi = 15; bi >= 0; bi--) {
            uint32_t n2 = d->n0[bi] + d->n1[bi], e = bit_err(d, bi);
            if (!e) bc_fmt(a, 4, "  .");
            else if (e * 100u < n2) bc_fmt(a, 4, " <1");
            else bc_fmt(a, 4, "%3u", e * 100u / (n2 ? n2 : 1));
            k += (uint32_t)bc_fmt(row + k, sizeof row - k, "%s", a);
        }
        line(&t, "%s", row);
    }
    line(&t, "----------------------------------------------------------------");
    tline(&t, r->verdict, "SUMMARY");
    {
        static char sbuf[1600];
        summary(cfg, r, sbuf, sizeof sbuf);
        /* word-wrapped to the screen, keeping two lines at the foot */
        const char *p = sbuf;
        while (*p) {
            while (*p == ' ') p++;
            if (!*p) break;
            int len = 0, cut = 0;
            while (p[len] && len < BC_COLS) { if (p[len] == ' ') cut = len; len++; }
            if (p[len] && cut) len = cut;
            char o[BC_COLS + 1]; for (int i = 0; i < len; i++) o[i] = p[i]; o[len] = 0;
            if (t.n < BC_ROWS - 2) line(&t, "%s", o); else det(&t, "%s", o);
            p += len;
        }
    }
    while (t.n < BC_ROWS - 2) t.lines[t.n++][0] = 0;            /* blank screen lines: not in the text */
    line(&t, "Photograph this screen. A copy is in buscheck.txt on the card.");
    line(&t, "Power off when done (docs: READ-ME-FIRST.txt, the bus check).");
    /* details for the log and the card */
    det(&t, "DETAILS");
    {
        static const char *const what[4] = { "reads expecting 0", "reads expecting 1", "read 1 for a 0", "read 0 for a 1" };
        for (int k = 0; k < 4; k++) {
            char row[200]; uint32_t q = (uint32_t)bc_fmt(row, sizeof row, "D15..D0 %s:", what[k]);
            for (int bi = 15; bi >= 0; bi--) q += (uint32_t)bc_fmt(row + q, sizeof row - q, " %u", k == 0 ? d->n0[bi] : k == 1 ? d->n1[bi] : k == 2 ? d->e01[bi] : d->e10[bi]);
            det(&t, "%s", row);
        }
    }
    det(&t, "word reads with the upper byte wrong %u, the lower %u", d->up_wrong, d->lo_wrong);
    det(&t, "address lines: %u pairs, %u skipped (outside the RAM), %u undecided, %u not as written", A->pairs, A->skipped, A->undecided, A->bad);
    for (int i = 0; i < A->nfind; i++)
        det(&t, "  finding %u: kind %u A%u A%u value %u when %s", i + 1, A->find[i].kind, A->find[i].n, A->find[i].m, A->find[i].v, modename(A->find[i].mode));
    {
        static const char *const vn[13] = { "DDRA", "DDRB", "T1LL", "T1LH", "ACR", "PCR", "IER", "SCC RR0 B", "SCC RR0 A", "SCC RR12", "SCC RR13", "RR2 3us", "RR2 b2b" };
        const struct bc_reg *gs[13] = { &C->via[0], &C->via[1], &C->via[2], &C->via[3], &C->via[4], &C->via[5], &C->via[6], &C->rr0[0], &C->rr0[1], &C->rr12, &C->rr13, &C->rr2[0], &C->rr2[1] };
        for (int k = 0; k < 13; k++) {
            const struct bc_reg *g = gs[k];
            char v6[64]; uint32_t q = 0; v6[0] = 0;
            for (int i = 0; i < g->nval; i++) q += (uint32_t)bc_fmt(v6 + q, sizeof v6 - q, " %02X:%u", g->val[i], g->cnt[i]);
            det(&t, "%s want %02X: %u reads %u wrong %u changes, values%s", vn[k], g->want, g->reads, g->wrong, g->changes, v6);
        }
        det(&t, "VIA changes per bit 0-7, port B: %u %u %u %u %u %u %u %u, port A: %u %u %u %u %u %u %u %u",
            C->pb_changes[0], C->pb_changes[1], C->pb_changes[2], C->pb_changes[3], C->pb_changes[4], C->pb_changes[5], C->pb_changes[6], C->pb_changes[7],
            C->pa_changes[0], C->pa_changes[1], C->pa_changes[2], C->pa_changes[3], C->pa_changes[4], C->pa_changes[5], C->pa_changes[6], C->pa_changes[7]);
    }
    {
        const struct bc_retry *T = &r->retry;
        det(&t, "retry: %u values; one read %u wrong; pairs %u wrong (%u reads, %u never agreed); three %u wrong (%u no majority)",
            T->values, T->one_wrong, T->two_wrong, T->two_reads, T->two_unresolved, T->three_wrong, T->three_nomajority);
        const struct bc_video *V = &r->video;
        if (V->ok == 1) det(&t, "video: frame %u us, blank %u us (from CA1's %s edge), %u frames, bursts of %u reads, %u overran",
                       V->frame_us, V->blank_us, V->blank_edge ? "rising" : "falling", V->frames, V->burst, V->overran);
        if (V->ok == 1) {
            char vb[200]; uint32_t qv = (uint32_t)bc_fmt(vb, sizeof vb, "frame by eighths from the blank, wrong/reads:");
            for (int k = 0; k < 8; k++) qv += (uint32_t)bc_fmt(vb + qv, sizeof vb - qv, " %u/%u", V->bin_wrong[k], V->bin_reads[k]);
            det(&t, "%s", vb);
        }
        for (int o = 0; o < 2; o++) {
            char sb[200]; uint32_t qs = (uint32_t)bc_fmt(sb, sizeof sb, "SCC RR0 %s, changes first/second at", o ? "B then A" : "A then B");
            for (int g = 0; g < BC_SGAPS; g++) qs += (uint32_t)bc_fmt(sb + qs, sizeof sb - qs, " %uns %u/%u", C->sp_ns[g], C->sp_first[o][g], C->sp_second[o][g]);
            det(&t, "%s", sb);
        }
        det(&t, "VIA wrong bits D8..D15: %u %u %u %u %u %u %u %u (registers DDRA DDRB T1LL T1LH ACR PCR IER)", C->via_bit_wrong[0], C->via_bit_wrong[1], C->via_bit_wrong[2],
            C->via_bit_wrong[3], C->via_bit_wrong[4], C->via_bit_wrong[5], C->via_bit_wrong[6], C->via_bit_wrong[7]);
        det(&t, "ROM: %u words x4, %u reads unlike their majority; first long %08X, sum of the words after it over %u KB %08X", r->rom.addrs, r->rom.disagree, r->rom.stored, r->rom.sum_kb, r->rom.sum);
        char row[200]; uint32_t q = (uint32_t)bc_fmt(row, sizeof row, "mean ns (RAM rw ww rb wb, VIA r w, SCC r w):");
        for (int k = 0; k < BC_KINDS; k++) q += (uint32_t)bc_fmt(row + q, sizeof row - q, " %u", r->t[k].n ? ticks_ns(r, r->t[k].sum / r->t[k].n) : 0);
        det(&t, "%s", row);
    }
    return t.n;
}
