/* SPDX-License-Identifier: MPL-2.0
 *
 * buscheck_test.c — the bus check's tests and verdicts (emu68/src/psmac/psmac_buscheck_core.c) against a
 * simulated compact Mac, with the faults a tester's machine might have.
 *
 * Backstory (22 November 2026, docs/08 item 108). The bus check exists to tell a timing problem from a
 * bad line, random noise from a systematic error, RAM from the chips; a verdict that says the wrong one
 * sends a tester to the soldering iron for nothing. So each kind of fault is put on a model board here
 * (RAM, a VIA that keeps its registers and raises CA1 in a 60 Hz vertical blank, an SCC with its
 * register pointer) and the summary must name it: run by `make -C mac check`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psmac_buscheck.h"

#define FRAME_US 16626u     /* the compact Mac's 60.15 Hz */
#define BLANK_US 1260u      /* 28 of 370 lines */

struct board {
    uint8_t *ram; uint32_t size, real;              /* real: the RAM fitted (repeats above it) */
    uint8_t rom[128 << 10];                         /* the ROM at $400000, with Apple's checksum in its first long */
    uint64_t t;                                     /* microseconds * 16 (the clock) */
    uint8_t via[16], ifr, ier; int ca1_was;
    uint8_t scc_ptr, wr12, wr13, wr2; uint64_t scc_t;
    /* faults */
    int stuck_bit, stuck_val;                       /* a data line on reads, -1 none */
    int alias_n, alias_m;                           /* reads: An follows Am (m 0: An stuck at 0) */
    int alias_rw;                                   /* ... on writes as well (the user's A14-A15 bridge, docs/08 item 68) */
    uint32_t rand_pm;                               /* reads corrupted per mille */
    int rand_rom;                                   /* ... the ROM's too (a sampling point too early for everything) */
    int rand_display_only;                          /* only while the video reads the RAM */
    int early;                                      /* corrupted reads return the address's low half */
    int screen_data;                                /* corrupted reads return the screen's word (the video's fetch) */
    int b2b_only;                                   /* corrupted only when the read follows another within 2 us */
    int intermittent_bit;                           /* this bit flips on rand_pm of the reads */
    uint32_t chip_pm;                               /* VIA/SCC reads corrupted per mille */
    int scc_close;                                  /* an SCC read within 2 us of the last SCC access reads DCD wrong half the time */
    uint32_t lcg; uint64_t last_rd;
};
static uint32_t rnd(struct board *b) { b->lcg = b->lcg * 1103515245u + 12345u; return b->lcg >> 8; }
static int in_blank(struct board *b) { return (b->t / 16) % FRAME_US < BLANK_US; }
static void tick(struct board *b, uint32_t us16);
static uint64_t ticks(void *ctx) { struct board *b = ctx; tick(b, 1); return b->t; }   /* reading the clock takes a sixteenth of a microsecond: waits end */
static void tick(struct board *b, uint32_t us16) { b->t += us16; int ca1 = in_blank(b); if (ca1 != b->ca1_was) { if ((b->via[12] & 1) == (ca1 ? 1 : 0)) b->ifr |= 2; b->ca1_was = ca1; } }
static uint32_t ram_rd16(struct board *b, uint32_t a) { a %= b->real; return (uint32_t)b->ram[a & ~1u] << 8 | b->ram[a | 1u]; }
static uint32_t corrupt_word(struct board *b, uint32_t a, uint32_t v)
{
    if (b->intermittent_bit >= 0) return v ^ 1u << b->intermittent_bit;
    if (b->early) return a & 0xffff;
    if (b->screen_data) return ram_rd16(b, b->size - 0x5900 + (rnd(b) % 10944) * 2);
    return v ^ ((rnd(b) & 0xffff) | 1u << (rnd(b) & 15));
}
static uint32_t rd(void *ctx, uint32_t a, int size)
{
    struct board *b = ctx;
    tick(b, 16);                                    /* one microsecond a cycle */
    uint32_t v;
    int gap_short = b->t - b->last_rd < 32;
    b->last_rd = b->t;
    if (a < 0x400000u || (a >= 0x400000u && a < 0x500000u)) {
        int rom = a >= 0x400000u;
        uint32_t ra = a;
        if (!rom && b->alias_n) { uint32_t bit = b->alias_m ? (ra >> b->alias_m & 1) : 0; ra = (ra & ~(1u << b->alias_n)) | bit << b->alias_n; }
        if (rom) { uint32_t o = (a - 0x400000u) % sizeof b->rom & ~1u; v = (uint32_t)b->rom[o] << 8 | b->rom[o + 1]; }
        else v = ram_rd16(b, ra & ~1u);
        int corrupt = b->rand_pm && rnd(b) % 1000 < b->rand_pm;
        if (rom && !b->rand_rom) corrupt = 0;
        if (b->rand_display_only && in_blank(b)) corrupt = 0;
        if (b->b2b_only && !gap_short) corrupt = 0;
        if (corrupt) v = corrupt_word(b, a, v);
        if (b->stuck_bit >= 0) v = b->stuck_val ? v | 1u << b->stuck_bit : v & ~(1u << b->stuck_bit);
        return size == 1 ? (a & 1 ? v & 0xff : v >> 8) : v & 0xffff;
    }
    if (a >= 0xF00000u) return 0x0001;               /* the phase read: D0 = 1 */
    if ((a & 0xf00000u) == 0xe00000u) {
        int reg = (a >> 9) & 15;
        if (reg == 13) v = b->ifr;
        else if (reg == 14) v = 0x80 | b->ier;
        else if (reg == 0) v = (b->via[0] & b->via[2]) | (~b->via[2] & 0x48);
        else if (reg == 15 || reg == 1) v = (b->via[15] & b->via[3]) | (~b->via[3] & 0x80);
        else v = b->via[reg];
    } else {
        int reg = b->scc_ptr; b->scc_ptr = 0;
        v = reg == 12 ? b->wr12 : reg == 13 ? b->wr13 : reg == 2 ? b->wr2 : 0x44;
        if (b->scc_close && b->t - b->scc_t < 32 && (rnd(b) & 1)) v ^= 0x08;
        b->scc_t = b->t;
    }
    if (b->chip_pm && rnd(b) % 1000 < b->chip_pm) v ^= 1u << (rnd(b) & 7);
    if (b->stuck_bit >= 8) v = b->stuck_val ? v | 1u << (b->stuck_bit - 8) : v & ~(1u << (b->stuck_bit - 8));
    return v & 0xff;
}
static void wr(void *ctx, uint32_t a, int size, uint32_t v)
{
    struct board *b = ctx;
    tick(b, 16);
    if (a < 0x400000u) {
        if (b->alias_rw && b->alias_n) { uint32_t bit = b->alias_m ? (a >> b->alias_m & 1) : 0; a = (a & ~(1u << b->alias_n)) | bit << b->alias_n; }
        a %= b->real;
        if (size == 1) b->ram[a] = (uint8_t)v;
        else { b->ram[a & ~1u] = (uint8_t)(v >> 8); b->ram[a | 1u] = (uint8_t)v; }
        return;
    }
    if ((a & 0xf00000u) == 0xe00000u) {
        int reg = (a >> 9) & 15;
        if (reg == 13) b->ifr &= (uint8_t)~(v & 0x7f);
        else if (reg == 14) { if (v & 0x80) b->ier |= v & 0x7f; else b->ier &= (uint8_t)~(v & 0x7f); }
        else b->via[reg == 1 ? 15 : reg] = (uint8_t)v;
        return;
    }
    b->scc_t = b->t;
    if (b->scc_ptr == 0) { b->scc_ptr = v & 15; return; }
    if (b->scc_ptr == 12) b->wr12 = (uint8_t)v;
    if (b->scc_ptr == 13) b->wr13 = (uint8_t)v;
    if (b->scc_ptr == 2) b->wr2 = (uint8_t)v;
    b->scc_ptr = 0;
}

static struct board *mk(uint32_t size)
{
    struct board *b = calloc(1, sizeof *b);
    b->size = b->real = size; b->ram = calloc(1, size);
    b->stuck_bit = -1; b->intermittent_bit = -1; b->lcg = 12345;
    uint32_t sum = 0;
    for (uint32_t i = 4; i < sizeof b->rom; i++) { b->rom[i] = (uint8_t)(i * 7 + (i >> 9)); if (i & 1) sum += (uint32_t)b->rom[i - 1] << 8 | b->rom[i]; }
    b->rom[0] = (uint8_t)(sum >> 24); b->rom[1] = (uint8_t)(sum >> 16); b->rom[2] = (uint8_t)(sum >> 8); b->rom[3] = (uint8_t)sum;
    return b;
}
static int fails;
static void run(const char *name, struct board *b, int plus, const char *expect1, const char *expect2, const char *never)
{
    struct bc_bus bus = { b, rd, wr, ticks, 16000000u, NULL, NULL, NULL };
    struct bc_config cfg = { b->size, "Macintosh 512Ke", "the board", "host simulation", "test", plus, 1, 0x1234, 128, b->size - 0x5900, b->size - 0x300, plus, 2000, "usercode 00185866 (test)" };
    static struct bc_results r;
    static char lines[BC_ROWS][BC_COLS + 1]; static char det[4096];
    bc_run(&bus, &cfg, &r);
    extern int bc_clipped; bc_clipped = 0;
    int n = bc_report(&cfg, &r, lines, det, sizeof det);
    extern char bc_clipped_text[];
    const char *sum = bc_summary(&cfg, &r);
    int ok = strstr(sum, expect1) != NULL && (!expect2 || strstr(sum, expect2) != NULL) && (!never || !strstr(sum, never));
    for (int i = 0; i < n; i++) if (strlen(lines[i]) > BC_COLS) ok = 0;
    if (bc_clipped) { ok = 0; printf("  %d screen lines cut at the right margin (%s)\n", bc_clipped, bc_clipped_text); }   /* every number must be readable on the photograph */
    printf("%-28s %s  %s\n", name, ok ? "ok  " : "FAIL", bc_headline(&cfg, &r));
    if (!ok || getenv("BC_SHOW")) { for (int i = 0; i < n; i++) printf("  |%s\n", lines[i]); if (!ok) printf("  (wanted \"%s\"%s%s%s%s; got: %s)\n", expect1, expect2 ? " and " : "", expect2 ? expect2 : "", never ? ", never " : "", never ? never : "", sum); }
    fails += !ok;
    free(b->ram); free(b);
}

int main(void)
{
    struct board *b;
    run("clean 512 KB", mk(512 << 10), 1, "All clean", NULL, NULL);
    run("clean 128 KB", mk(128 << 10), 1, "All clean", NULL, NULL);
    run("clean 4 MB", mk(4 << 20), 0, "All clean", NULL, NULL);
    b = mk(512 << 10); b->stuck_bit = 11; b->stuck_val = 0; run("D11 stuck at 0", b, 1, "D11 always reads 0", "pin 58", "timing");
    b = mk(512 << 10); b->stuck_bit = 3; b->stuck_val = 1; run("D3 stuck at 1", b, 1, "D3 always reads 1", NULL, "A1 ");
    b = mk(512 << 10); b->alias_n = 14; b->alias_m = 15; run("A14 follows A15 (reads)", b, 1, "A14 follows A15 when reading", NULL, "Reads of RAM fail");
    b = mk(4 << 20); b->alias_n = 14; b->alias_m = 15; b->alias_rw = 1; run("A14 follows A15 (both)", b, 0, "A14 does not reach the RAM as both 0 and 1", "following another line", "Reads of RAM fail");
    b = mk(512 << 10); b->alias_n = 9; run("A9 stuck at 0 (reads)", b, 1, "A9 is stuck at 0 when reading", NULL, "Reads of RAM fail");
    b = mk(4 << 20); b->real = 512 << 10; run("512K board taken for 4 MB", b, 0, "repeats every 512 KB", "not the 4096 KB", "Reads of RAM fail");
    b = mk(512 << 10); b->rand_pm = 500; run("random 50%", b, 1, "mostly at random", "timing problem", "stuck");
    b = mk(512 << 10); b->rand_pm = 20; run("random 2%: retry cures", b, 1, "A retry would help:", "timing problem", "stuck");
    b = mk(512 << 10); b->rand_pm = 300; b->early = 1; run("early sampling", b, 1, "read before the board drove it", NULL, "stuck");
    b = mk(512 << 10); b->rand_pm = 300; b->rand_display_only = 1; run("video contention", b, 1, "while the video reads the RAM", NULL, "fail far less");
    b = mk(512 << 10); b->rand_pm = 300; b->b2b_only = 1; run("back to back only", b, 1, "fail far less", NULL, "while the video reads");
    b = mk(512 << 10); b->rand_pm = 30; b->intermittent_bit = 6; run("intermittent D6", b, 1, "intermittent line", "D6", "timing problem");
    b = mk(512 << 10); b->chip_pm = 50; run("chips only", b, 1, "RAM reads are clean but the chips", NULL, "Reads of RAM");
    b = mk(512 << 10); b->rand_pm = 100; run("RAM only, ROM clean", b, 1, "ROM reads clean", "RAM's cycles", "ROM reads fail");
    b = mk(512 << 10); b->rand_pm = 100; b->rand_rom = 1; run("RAM and ROM: sampling", b, 1, "ROM reads fail too", "too early", "ROM reads clean");
    b = mk(512 << 10); b->rand_pm = 200; b->screen_data = 1; run("video fetch's data", b, 1, "screen's mark", NULL, NULL);
    b = mk(512 << 10); b->rand_pm = 300; b->rand_rom = 1; b->chip_pm = 300; run("everything noisy (a 512Ke?)", b, 1, "timing problem", "chips' reads are wrong too", "stuck");
    b = mk(512 << 10); b->scc_close = 1; run("SCC needs spacing", b, 1, "SCC reads soon after another", "calm from", "Reads of RAM");
    printf("%s\n", fails ? "bus check test: FAILED" : "bus check test: all verdicts as expected");
    return fails != 0;
}
