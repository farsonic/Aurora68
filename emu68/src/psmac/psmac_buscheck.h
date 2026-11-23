/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_buscheck.h — the compact Macs' bus check: what is measured, and how it is read.
 *
 * Backstory. On 22 November 2026 a tester's real Macintosh 512Ke showed a screen that was right but
 * for rare single wrong words, while every read of the board looked unreliable: the kernel's screen
 * read-back found 6,011 of 6,605 rows with wrong words (29 of 32 on average), the mouse's Y1 line
 * changed forty times as often as Y2, and the keyboard and button gave tens of thousands of phantom
 * events (docs/08 items 96 and 108). From one log it could not be told whether the reads were taken
 * too early (timing), whether one line was bad, whether the RAM or the chips were at fault, or
 * whether a retry would cure it. So every tester's machine now runs the same measurements and shows
 * the same numbers on one screen: mac.buscheck=1 (psmac_buscheck.c is the kernel's half, this
 * header and psmac_buscheck_core.c the measurements and their reading, which build on the host too:
 * tools/buscheck_test.c runs them against a simulated board with faults, by `make -C mac check`).
 *
 * The core knows nothing of the kernel: it is given a bus (read, write, a clock) and the board's
 * memory size, runs the tests, and writes a report as lines of text no wider than the compact Mac's
 * screen (64 characters of an 8 by 8 font across 512 pixels, 42 lines down 342).
 */
#ifndef PSMAC_BUSCHECK_H
#define PSMAC_BUSCHECK_H
#include <stdint.h>

#define BC_COLS 64                  /* 512 pixels / 8 */
#define BC_ROWS 42                  /* 342 pixels / 8 */
#define BC_NADDR 256                /* the data test's addresses: a few hundred, spread over the RAM */
#define BC_NPAT 40                  /* walking ones (16), walking zeros (16), $0000 $FFFF $AAAA $5555, four random */
#define BC_SAMPLES 255              /* durations kept per kind of access, for the median */
#define BC_MAXFIND 12               /* address-line findings kept */
#define BC_SGAPS 6                  /* SCC read spacings: none, 0.5, 1, 2, 3, 5 us (docs/18 M8) */

/* the bus the check runs on. rd/wr take 24-bit addresses and sizes 1 or 2; ticks is a free-running
   counter at hz. faults, if given, counts bus cycles the transport abandoned (the GPIO timeout). */
struct bc_bus {
    void *ctx;
    uint32_t (*rd)(void *ctx, uint32_t addr, int size);
    void (*wr)(void *ctx, uint32_t addr, int size, uint32_t data);
    uint64_t (*ticks)(void *ctx);
    uint64_t hz;
    uint32_t (*faults)(void *ctx);
    void (*idle)(void *ctx);        /* between tests (may be NULL) */
    void (*scc_unspaced)(void *ctx, int on);   /* the transport's own wait between SCC accesses off (1) and on again (0), for the
                                                  spacing tests (the Plus-era boards' 3 us, psmac_gpio.c); may be NULL */
    void (*stage)(void *ctx, int step, int steps, const char *what);   /* before each test, for a report that grows as the check
                                                  runs (Setup and Recovery's Diagnostics tab, docs/08 item 120); may be NULL */
};

struct bc_config {
    uint32_t ram_size;              /* bytes: the board's RAM as the kernel assumes it */
    const char *board;              /* "Macintosh 512Ke" */
    const char *ram_from;           /* where the size came from: "the board", "mac.seram" */
    const char *link;               /* "GPIO, Pi 3 timings, CPLD 200 MHz" or "bridge (testbed)" */
    const char *version;            /* the kernel's build string, to match photographs to kernels */
    int plus;                       /* the Plus and the original Macs: PB3-PB6 are inputs the board drives, PB6 is H4 */
    uint32_t run;                   /* 1, 2, ... with mac.buscheck=loop */
    uint32_t seed;
    uint32_t rom_kb;                /* the board's ROM at $400000: 64 (128K, 512K), 128 (512Ke, Plus), 256 (SE); 0 none */
    uint32_t fb_base, snd_base;     /* the main screen and sound buffers (filled with marks during the tests) */
    int phase;                      /* the board has the Plus-era phase read at $F00000 (docs/18 section 2) */
    uint32_t chip_reads;            /* reads per VIA register and SCC pairs per spacing: 10,000 on a PiStorm, fewer in the testbed */
    const char *cpld;               /* the CPLD's JTAG usercode and what it is, or why it was not read */
};

enum bc_kind { BC_RAM_RW, BC_RAM_WW, BC_RAM_RB, BC_RAM_WB, BC_VIA_R, BC_VIA_W, BC_SCC_R, BC_SCC_W, BC_KINDS };
enum bc_verdict { BC_SKIP, BC_CLEAN, BC_INFO, BC_STRAY, BC_UNSURE, BC_FAIL };

struct bc_time { uint32_t n, min, max, ns; uint64_t sum; uint32_t s[BC_SAMPLES]; };   /* in ticks */

/* test 1: the data lines, through the RAM */
struct bc_data {
    uint32_t addrs, patterns;
    uint32_t words, wrong;                          /* word reads (three per address and pattern) */
    uint32_t up_wrong, lo_wrong;                    /* word reads with the upper (D8-D15) / lower byte wrong */
    uint32_t bhi, bhi_wrong, blo, blo_wrong;        /* byte reads at even (upper byte) and odd (lower byte) addresses */
    uint32_t bw, bw_wrong;                          /* byte writes, read back as words */
    uint32_t n0[16], n1[16], e01[16], e10[16];      /* per bit: reads expecting 0 / 1; read 1 for a 0, 0 for a 1 */
    uint32_t n01[16][16], x01[16][16];              /* [b][c]: expected b=0 c=1; of those, b read 1 */
    uint32_t n10[16][16], x10[16][16];              /* [b][c]: expected b=1 c=0; of those, b read 0 */
    uint32_t first_wrong, repeat, transient, other; /* a wrong first read; the second read: the same word, right, another */
    uint32_t eq_addr, eq_hireg, eq_prev, eq_ffff, eq_0000;   /* wrong words that were the address's low half, the Pi's last word to the
                                                       CPLD (the address-high register), the word read before, $FFFF, $0000 */
    uint32_t ffff_reads;                            /* reads of $FFFF where something else was expected: a board that does not answer */
    uint32_t eq_screen, eq_sound;                   /* wrong words that were the screen's or the sound buffer's mark: the video's or sound's fetch */
    uint32_t one_reads, one_distinct, one_right, one_commonest_right;   /* one word read 1,000 times (docs/18 M5) */
    uint32_t ww, ww_wrong;                          /* word writes, each judged by a majority of fifteen reads */
    int verdict;
};

/* test 2: the address lines */
struct bc_afind { uint8_t kind, n, m, v, mode; };  /* kind: BC_A_* ; mode: 0 reads only, 1 reads and writes, 2 writes only */
enum { BC_A_STUCK = 1, BC_A_FOLLOWS, BC_A_JOINED, BC_A_SIZE, BC_A_CONFUSED };
struct bc_addr {
    int top;                                        /* the highest line with an address one line away inside the RAM */
    uint32_t pairs, skipped, undecided, bad;
    uint32_t lines_bad;                             /* bit n: An named in a finding */
    int nfind;
    struct bc_afind find[BC_MAXFIND];
    int verdict;
};

/* test 3: the chips' registers that should not change */
struct bc_reg { uint32_t reads, wrong, changes; uint8_t want; uint8_t nval; uint8_t val[6]; uint32_t cnt[6]; };
struct bc_chips {
    struct bc_reg via[7];                           /* DDRA, DDRB, T1 latch low and high, ACR, PCR, IER: cfg->chip_reads each (docs/18 M7) */
    uint32_t via_bit_wrong[8];                      /* those reads' wrong bits by position (D8-D15) */
    uint32_t ifr_pairs, ifr_differ;                 /* the IFR read twice in a row */
    uint32_t pb_reads, pb_changes[8], pb_out_wrong; /* port B: changes per input bit; output bits that read back wrong */
    uint32_t pa_reads, pa_changes[8], pa_out_wrong; /* port A (register 15, no handshake) */
    struct bc_reg rr0[2];                           /* SCC RR0, channel B and A */
    uint32_t rr0_stable_changes[2], rr0_line_changes[2];   /* bits 0,1,2,6 (should not move) / 3,4,5,7 (the pins) */
    struct bc_reg rr12, rr13;                       /* WR12/WR13 written on channel B, read back as RR12/RR13 */
    struct bc_reg rr2[2];                           /* WR2 (the vector) read back as channel A's RR2: 3 us apart, back to back (M9) */
    uint32_t sp_pairs;                              /* RR0 pairs per spacing and order (M8) */
    uint32_t sp_ns[BC_SGAPS];                       /* the spacings */
    uint32_t sp_first[2][BC_SGAPS], sp_second[2][BC_SGAPS];   /* [order 0 A then B, 1 B then A]: changes of the first / second read's stable bits and DCD */
    uint32_t phase_reads, phase_d0;                 /* the phase read at $F00000: reads, and how many had D0 = 1 */
    int via_verdict, scc_verdict, in_verdict;
};

/* the board's ROM: no interleave, an immediate /DTACK (docs/18 M6) */
struct bc_rom {
    uint32_t addrs, reads, disagree;                /* words read four times each, reads unlike their majority */
    uint32_t stored, sum, sum_kb;                   /* the checksum at 0 and the sum of the words after it, over sum_kb */
    int sum_ok, verdict;
};

/* test 5: the retry experiment */
struct bc_retry {
    uint32_t values;
    uint32_t one_wrong;                             /* (a) one read */
    uint32_t two_wrong, two_reads, two_unresolved;  /* (b) read pairs until two agree (four pairs at most) */
    uint32_t three_wrong, three_nomajority;         /* (c) majority of three */
    int verdict;
};

/* test 6: during vertical blanking and while the video reads the RAM */
struct bc_video {
    int ok;                                         /* edges seen: 0 none (skipped) */
    int blank_edge;                                 /* 1: CA1's rising edge starts the blank, 0: the falling */
    uint32_t frame_us, blank_us, frames, burst;
    uint32_t blank_reads, blank_wrong, disp_reads, disp_wrong, overran;
    uint32_t bin_reads[8], bin_wrong[8];            /* reads by place in the frame: bin 0 begins at the blank, eight to a frame (M2) */
    int verdict;
};

/* test 7: reads back to back against reads spaced apart (the coordinator's question of 22 November 2026:
   a socket joint shows as one line, a timing fault may depend on how soon one cycle follows another) */
#define BC_GAPS 3
struct bc_space {
    uint32_t gap_us[BC_GAPS];                       /* 0 (back to back), then two waits between reads */
    uint32_t reads[BC_GAPS], wrong[BC_GAPS];
    int verdict;
};

struct bc_results {
    struct bc_data data;
    struct bc_addr addr;
    struct bc_chips chips;
    struct bc_time t[BC_KINDS];
    uint32_t timeouts, cycles;
    struct bc_retry retry;
    struct bc_video video;
    struct bc_space space;
    struct bc_rom rom;
    uint64_t ticks;                                 /* the whole check's duration */
    uint64_t hz;
    int verdict;                                    /* the worst of the tests */
};

/* run every test; r is filled (and cleared first) */
void bc_run(const struct bc_bus *bus, const struct bc_config *cfg, struct bc_results *r);

/* the report: up to BC_ROWS lines of at most BC_COLS characters for the screen (lines[i], NUL ended),
   and a longer text for the log and the card (detail, up to cap bytes: the screen's lines first, then
   the details). Returns the number of screen lines. */
int bc_report(const struct bc_config *cfg, const struct bc_results *r, char lines[BC_ROWS][BC_COLS + 1], char *detail, uint32_t cap);

/* the summary's first sentence, for a log line and the host test */
const char *bc_headline(const struct bc_config *cfg, const struct bc_results *r);
const char *bc_summary(const struct bc_config *cfg, const struct bc_results *r);   /* all of it, unwrapped */

/* a small formatter (no libc in the kernel): %d %u %x %X %s %c with a width, '-' to the left, '0' to
   pad, %lu for uint64_t */
int bc_fmt(char *out, uint32_t cap, const char *fmt, ...);

#endif
