// SPDX-License-Identifier: MPL-2.0
/* mac/pistatus/model.h - Aurora68 Status 2.0's numbers, apart from its window (23 November 2026).
 *
 * The status page (the kernel's emu68/src/alice/alice_status.h, at slot-$9 window + 0x50000) is
 * read as bytes, big-endian, at the offsets below, so the same code runs on the 68k and in the host
 * test (tools/pistatus_test.c), which also checks every offset here against the kernel's header:
 * that header is GPL-2.0-only and this program MPL-2.0, so the offsets are written out rather than
 * the header included, and the test keeps them honest. No Toolbox calls in model.c. */
#ifndef PISTATUS_MODEL_H
#define PISTATUS_MODEL_H

typedef unsigned int u32;              /* 32 bits on the 68k and on the host alike */
#define ST_UNKNOWN 0xFFFFFFFFU                 /* "not measured on this machine" */
#define ST_MAGIC   0x41363853U                 /* 'A68S' */
#define ST_PAGE    0x200                        /* the bytes read: the page as of 23 November 2026 */

/* offsets in the page */
#define O_MAGIC        0x000
#define O_VERSION      0x004
#define O_SIZE         0x008
#define O_SEQ          0x00C
#define O_UPTIME       0x010
#define O_FLAGS        0x018
#define O_BUILD        0x020                    /* 96 bytes */
#define O_CORE         0x080                    /* 4 x 32: role, busy, name[24] */
#define O_TEMP         0x100
#define O_ARM_HZ       0x104
#define O_THROTTLED    0x10C
#define O_JIT_TOTAL    0x120
#define O_JIT_FREE     0x124
#define O_NET          0x140                    /* 2 x 32: kind, link, rx_pkts, tx_pkts, rx_bytes, tx_bytes, rx_drop, tx_err */
#define O_ARM_MAX      0x1A0
#define O_ARM_MEAS     0x1A4
#define O_TEMP_MAX     0x1A8
#define O_INSN_RATE    0x1AC
#define O_HEAP_FREE    0x1B0
#define O_HEAP_TOTAL   0x1B4
#define O_VC_STATE     0x1B8
#define O_VC_LOAD      0x1BC
#define O_VC_DECODE    0x1C0
#define O_VC_DRAW      0x1C4
#define O_VC_BUFFER    0x1C8
#define O_VC_UNDERRUNS 0x1CC
#define O_VC_SHOWN     0x1D0
#define O_VC_DROPPED   0x1D4
#define O_SND_OUT      0x1D8
#define O_SND_PLAYING  0x1DC
#define O_SND_UNDERRUNS 0x1E0
#define O_STR_STATE    0x1E4
#define O_STR_LOAD     0x1E8
#define O_STR_UNDERRUNS 0x1EC
#define STF_SIMULATED  2U

/* one reading of the page, in plain numbers (ST_UNKNOWN where not measured, or past an older page's size) */
struct st_now {
	int ok;                                 /* a page was read */
	u32 size, seq, uptime_ms, flags;
	char build[97];
	struct { u32 busy; char name[25]; } core[4];
	u32 temp_mc, temp_max_mc, arm_hz, arm_meas_hz, arm_max_hz, throttled;
	u32 jit_total, jit_free, heap_free, heap_total, insn_rate;
	struct { u32 kind, link, rx_bytes, tx_bytes; } net[2];
	u32 vc_state, vc_load, vc_decode_us, vc_draw_us, vc_buffer_ms, vc_underruns, vc_shown, vc_dropped;
	u32 snd_out, snd_playing, snd_underruns, str_state, str_load, str_underruns;
};

/* what the window shows, kept between readings: rates, and the history the strip charts draw */
#define HIST_N 512                              /* points, one every HIST_EVERY seconds: 17 minutes */
#define HIST_EVERY 2
struct st_model {
	struct st_now now;
	int have_prev;
	u32 prev_up, prev_rx[2], prev_tx[2];
	u32 rx_bps[2], tx_bps[2];                /* bytes a second, ST_UNKNOWN until two readings */
	u32 mips_peak;                          /* the highest MIPS seen, for the dial's and the chart's scale */
	u32 last_hist_s;
	int hist_head, hist_n;                  /* the newest point is hist[(hist_head - 1) mod HIST_N] */
	u32 h_temp[HIST_N], h_mhz[HIST_N], h_mips[HIST_N];   /* thousandths of a degree, MHz, MIPS x 10; ST_UNKNOWN: none */
};

/* the page's bytes (a copy) as numbers; 0 when they are not a status page */
int st_parse(const unsigned char *page, struct st_now *n);
/* a new reading: rates and history */
void st_update(struct st_model *m, const struct st_now *n);
/* the i-th newest point of a history (0 the newest), ST_UNKNOWN past the start */
u32 st_hist(const struct st_model *m, const u32 *h, int i);

/* numbers as text, into b (at least 24 bytes); "not measured" for ST_UNKNOWN. Mac Roman: 0xA1 is the degree sign */
char *fmt_temp(char *b, u32 mc);                /* "61.2\xA1C" */
char *fmt_mhz(char *b, u32 hz);                 /* "1200 MHz" */
char *fmt_mips(char *b, u32 ips);               /* "169.3 MIPS" */
char *fmt_rate(char *b, u32 bps);               /* "12.3 KB/s", "1.2 MB/s" */
char *fmt_mb(char *b, u32 bytes);               /* "41.2 MB" */
char *fmt_pct(char *b, u32 hundredths);         /* "12%" */
char *fmt_num(char *b, u32 v);
char *fmt_secs(char *b, u32 ms);                /* "8.2 s" */
char *cat(char *d, const char *s);              /* s at the end of d; returns d */

/* the throttled word: bit 0-3 now, 16-19 since boot (under-voltage, frequency capped, throttled, soft limit) */
#define TH_NOW(t, i)  (((t) >> (i)) & 1U)
#define TH_PAST(t, i) (((t) >> (16 + (i))) & 1U)

/* a dial's needle: v on lo..hi as -120..120 degrees (0 at the top, clockwise), clamped */
int dial_angle(u32 v, u32 lo, u32 hi);
/* sine and cosine of a whole number of degrees, times 16384 */
int isin(int deg);
int icos(int deg);
/* the MIPS dial's top: 50, 100, 200, 300 ... above what has been seen */
u32 mips_scale(u32 peak_ips);

#endif
