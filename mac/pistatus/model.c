// SPDX-License-Identifier: MPL-2.0
/* mac/pistatus/model.c - Aurora68 Status 2.0's numbers: the status page parsed, the rates worked out
 * from two readings, the strip charts' history, and the numbers as text. No Toolbox calls: the host
 * test (tools/pistatus_test.c) runs this file as it is, on pages it makes. model.h says why. */
#include "model.h"

static u32 be32(const unsigned char *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void cstr(char *d, const unsigned char *s, int max)
{
	int i = 0;
	for (; i < max && s[i]; i++) d[i] = (char)(s[i] >= 32 && s[i] < 127 ? s[i] : '?');
	d[i] = 0;
}

int st_parse(const unsigned char *pg, struct st_now *n)
{
	u32 size;
	int i;
	n->ok = 0;
	if (be32(pg + O_MAGIC) != ST_MAGIC) return 0;
	size = be32(pg + O_SIZE);
	if (size < 0x1A0 || size > 4096) return 0;
	n->size = size;
	n->seq = be32(pg + O_SEQ);
	n->uptime_ms = be32(pg + O_UPTIME);
	n->flags = be32(pg + O_FLAGS);
	cstr(n->build, pg + O_BUILD, 96);
	for (i = 0; i < 4; i++) {
		n->core[i].busy = be32(pg + O_CORE + 32 * i + 4);
		cstr(n->core[i].name, pg + O_CORE + 32 * i + 8, 24);
	}
	n->temp_mc = be32(pg + O_TEMP);
	n->arm_hz = be32(pg + O_ARM_HZ);
	n->throttled = be32(pg + O_THROTTLED);
	n->jit_total = be32(pg + O_JIT_TOTAL);
	n->jit_free = be32(pg + O_JIT_FREE);
	for (i = 0; i < 2; i++) {
		const unsigned char *u = pg + O_NET + 32 * i;
		n->net[i].kind = be32(u);
		n->net[i].link = be32(u + 4);
		n->net[i].rx_bytes = be32(u + 16);
		n->net[i].tx_bytes = be32(u + 20);
	}
	/* the fields added on 23 November 2026, where the page has them */
#define NEW(f, o) n->f = size >= (o) + 4 ? be32(pg + (o)) : ST_UNKNOWN
	NEW(arm_max_hz, O_ARM_MAX); NEW(arm_meas_hz, O_ARM_MEAS); NEW(temp_max_mc, O_TEMP_MAX);
	NEW(insn_rate, O_INSN_RATE); NEW(heap_free, O_HEAP_FREE); NEW(heap_total, O_HEAP_TOTAL);
	NEW(vc_state, O_VC_STATE); NEW(vc_load, O_VC_LOAD); NEW(vc_decode_us, O_VC_DECODE);
	NEW(vc_draw_us, O_VC_DRAW); NEW(vc_buffer_ms, O_VC_BUFFER); NEW(vc_underruns, O_VC_UNDERRUNS);
	NEW(vc_shown, O_VC_SHOWN); NEW(vc_dropped, O_VC_DROPPED);
	NEW(snd_out, O_SND_OUT); NEW(snd_playing, O_SND_PLAYING); NEW(snd_underruns, O_SND_UNDERRUNS);
	NEW(str_state, O_STR_STATE); NEW(str_load, O_STR_LOAD); NEW(str_underruns, O_STR_UNDERRUNS);
#undef NEW
	n->ok = 1;
	return 1;
}

static void hist_push(u32 *h, int head, u32 v) { h[head] = v; }

void st_update(struct st_model *m, const struct st_now *n)
{
	int i;
	m->now = *n;
	if (!n->ok) { m->have_prev = 0; return; }
	/* rates: the counters' change over the uptime's (32-bit counters wrap; a restart is a smaller uptime) */
	if (m->have_prev && n->uptime_ms > m->prev_up) {
		u32 dt = n->uptime_ms - m->prev_up;
		for (i = 0; i < 2; i++) {
			u32 dr = n->net[i].rx_bytes - m->prev_rx[i], dx = n->net[i].tx_bytes - m->prev_tx[i];
			/* bytes x 1000 / ms without overflow for any rate a Pi reaches: whole kilobytes first */
			m->rx_bps[i] = dr < 4000000U ? dr * 1000U / dt : dr / dt * 1000U;
			m->tx_bps[i] = dx < 4000000U ? dx * 1000U / dt : dx / dt * 1000U;
		}
	} else if (!m->have_prev || n->uptime_ms < m->prev_up) {
		for (i = 0; i < 2; i++) m->rx_bps[i] = m->tx_bps[i] = ST_UNKNOWN;
		if (m->have_prev) { m->hist_n = 0; m->hist_head = 0; m->mips_peak = 0; }   /* the Pi restarted */
	}
	m->prev_up = n->uptime_ms;
	for (i = 0; i < 2; i++) { m->prev_rx[i] = n->net[i].rx_bytes; m->prev_tx[i] = n->net[i].tx_bytes; }
	m->have_prev = 1;
	if (n->insn_rate != ST_UNKNOWN && n->insn_rate > m->mips_peak) m->mips_peak = n->insn_rate;
	/* the history: one point every HIST_EVERY seconds of the Pi's uptime */
	{
		u32 s = n->uptime_ms / 1000U;
		if (m->hist_n && s / HIST_EVERY == m->last_hist_s / HIST_EVERY) return;
		m->last_hist_s = s;
		hist_push(m->h_temp, m->hist_head, n->temp_mc);
		hist_push(m->h_mhz, m->hist_head, n->arm_meas_hz != ST_UNKNOWN ? n->arm_meas_hz / 1000000U :
		          n->arm_hz != ST_UNKNOWN ? n->arm_hz / 1000000U : ST_UNKNOWN);
		hist_push(m->h_mips, m->hist_head, n->insn_rate != ST_UNKNOWN ? n->insn_rate / 100000U : ST_UNKNOWN);
		m->hist_head = (m->hist_head + 1) % HIST_N;
		if (m->hist_n < HIST_N) m->hist_n++;
	}
}

u32 st_hist(const struct st_model *m, const u32 *h, int i)
{
	if (i < 0 || i >= m->hist_n) return ST_UNKNOWN;
	return h[(m->hist_head - 1 - i + 2 * HIST_N) % HIST_N];
}

/* ---- text ---- */
char *cat(char *d, const char *s)
{
	char *p = d;
	while (*p) p++;
	while (*s) *p++ = *s++;
	*p = 0;
	return d;
}
char *fmt_num(char *b, u32 v)
{
	char t[12];
	int i = 0, k = 0;
	do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (i) b[k++] = t[--i];
	b[k] = 0;
	return b;
}
/* v / div with one decimal, rounded */
static char *one_dec(char *b, u32 v, u32 div)
{
	u32 tenths = v < 400000000U ? (v * 10U + div / 2) / div : v / div * 10U;
	char d[2] = { (char)('0' + tenths % 10), 0 };
	fmt_num(b, tenths / 10);
	cat(b, ".");
	return cat(b, d);
}
static const char not_measured[] = "not measured";
char *fmt_temp(char *b, u32 mc)
{
	b[0] = 0;
	if (mc == ST_UNKNOWN) return cat(b, not_measured);
	one_dec(b, mc, 1000);
	return cat(b, "\xA1" "C");
}
char *fmt_mhz(char *b, u32 hz)
{
	b[0] = 0;
	if (hz == ST_UNKNOWN) return cat(b, not_measured);
	fmt_num(b, (hz + 500000U) / 1000000U);
	return cat(b, " MHz");
}
char *fmt_mips(char *b, u32 ips)
{
	b[0] = 0;
	if (ips == ST_UNKNOWN) return cat(b, not_measured);
	one_dec(b, ips, 1000000U);
	return cat(b, " MIPS");
}
char *fmt_rate(char *b, u32 bps)
{
	b[0] = 0;
	if (bps == ST_UNKNOWN) return cat(b, "--");
	if (bps < 1024U * 1000U) { one_dec(b, bps, 1024); return cat(b, " KB/s"); }
	one_dec(b, bps / 1024U, 1024);
	return cat(b, " MB/s");
}
char *fmt_mb(char *b, u32 bytes)
{
	b[0] = 0;
	if (bytes == ST_UNKNOWN) return cat(b, not_measured);
	if (bytes < 1024U * 1024U) { fmt_num(b, (bytes + 512) / 1024); return cat(b, " KB"); }
	one_dec(b, bytes / 1024U, 1024);
	return cat(b, " MB");
}
char *fmt_pct(char *b, u32 h)
{
	b[0] = 0;
	if (h == ST_UNKNOWN) return cat(b, "--");
	fmt_num(b, (h + 50) / 100);
	return cat(b, "%");
}
char *fmt_secs(char *b, u32 ms)
{
	b[0] = 0;
	if (ms == ST_UNKNOWN) return cat(b, "--");
	one_dec(b, ms, 1000);
	return cat(b, " s");
}

/* ---- dials ---- */
int dial_angle(u32 v, u32 lo, u32 hi)
{
	if (v == ST_UNKNOWN || hi <= lo || v <= lo) return -120;
	if (v >= hi) return 120;
	{
		u32 x = v - lo, r = hi - lo;
		while (r > 0xFFFFFFUL) { r >>= 4; x >>= 4; }     /* x * 240 must stay below 2^32 */
		return -120 + (int)(x * 240U / r);
	}
}
static const short sine[91] = {
	0, 286, 572, 857, 1143, 1428, 1713, 1997, 2280, 2563, 2845, 3126, 3406,
	3686, 3964, 4240, 4516, 4790, 5063, 5334, 5604, 5872, 6138, 6402, 6664, 6924,
	7182, 7438, 7692, 7943, 8192, 8438, 8682, 8923, 9162, 9397, 9630, 9860, 10087,
	10311, 10531, 10749, 10963, 11174, 11381, 11585, 11786, 11982, 12176, 12365, 12551, 12733,
	12911, 13085, 13255, 13421, 13583, 13741, 13894, 14044, 14189, 14330, 14466, 14598, 14726,
	14849, 14968, 15082, 15191, 15296, 15396, 15491, 15582, 15668, 15749, 15826, 15897, 15964,
	16026, 16083, 16135, 16182, 16225, 16262, 16294, 16322, 16344, 16362, 16374, 16382, 16384,
};
int isin(int d)
{
	d %= 360;
	if (d < 0) d += 360;
	if (d <= 90) return sine[d];
	if (d <= 180) return sine[180 - d];
	if (d <= 270) return -sine[d - 180];
	return -sine[360 - d];
}
int icos(int d) { return isin(d + 90); }
u32 mips_scale(u32 peak)
{
	u32 m = peak / 1000000U, top = 50;
	while (top <= m && top < 4000) top = top < 100 ? 100 : top + 100;
	return top;
}
