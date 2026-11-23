// SPDX-License-Identifier: MPL-2.0
/* tools/pistatus_test.c - Aurora68 Status 2.0's numbers on the host (make -C mac check, 23 November 2026).
 *
 * The window can only be looked at in the testbed, and QEMU's Pi has no thermometer, no measured clock and
 * no throttling: those paths of the program are exercised here instead. mac/pistatus/model.c runs as it is
 * on pages made the way the kernel makes them (big-endian, alice_status.h's layout), and every offset
 * model.h writes out is checked against the kernel's header, which the program itself does not include
 * (model.h says why). */
#ifdef A68_FREESTANDING                         /* as a 68020 program under qemu-m68k (tools/pistatus_m68k.sh) */
#include "play_core_shim.h"
#define offsetof(t, f) __builtin_offsetof(t, f)
#else
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#endif
#include "alice_status.h"
#include "../mac/pistatus/model.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define SAME(a, b) do { checks++; if (strcmp((a), (b))) { fails++; printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)

static unsigned char pg[4096];
static void put(unsigned off, unsigned long v) { pg[off] = v >> 24; pg[off + 1] = v >> 16; pg[off + 2] = v >> 8; pg[off + 3] = v; }

#define OFF(f) offsetof(struct alice68_status, f)
static void offsets(void)
{
	CHECK(O_MAGIC == OFF(magic)); CHECK(O_VERSION == OFF(version)); CHECK(O_SIZE == OFF(size));
	CHECK(O_SEQ == OFF(seq)); CHECK(O_UPTIME == OFF(uptime_ms)); CHECK(O_FLAGS == OFF(flags));
	CHECK(O_BUILD == OFF(build)); CHECK(O_CORE == OFF(core)); CHECK(O_TEMP == OFF(temp_mc));
	CHECK(O_ARM_HZ == OFF(arm_hz)); CHECK(O_THROTTLED == OFF(throttled));
	CHECK(O_JIT_TOTAL == OFF(jit_total)); CHECK(O_JIT_FREE == OFF(jit_free)); CHECK(O_NET == OFF(net));
	CHECK(O_ARM_MAX == OFF(arm_max_hz)); CHECK(O_ARM_MEAS == OFF(arm_measured_hz)); CHECK(O_TEMP_MAX == OFF(temp_max_mc));
	CHECK(O_INSN_RATE == OFF(insn_rate)); CHECK(O_HEAP_FREE == OFF(heap_free)); CHECK(O_HEAP_TOTAL == OFF(heap_total));
	CHECK(O_VC_STATE == OFF(vcard_state)); CHECK(O_VC_LOAD == OFF(vcard_load)); CHECK(O_VC_DECODE == OFF(vcard_decode_us));
	CHECK(O_VC_DRAW == OFF(vcard_draw_us)); CHECK(O_VC_BUFFER == OFF(vcard_buffer_ms)); CHECK(O_VC_UNDERRUNS == OFF(vcard_underruns));
	CHECK(O_VC_SHOWN == OFF(vcard_shown)); CHECK(O_VC_DROPPED == OFF(vcard_dropped)); CHECK(O_SND_OUT == OFF(snd_out));
	CHECK(O_SND_PLAYING == OFF(snd_playing)); CHECK(O_SND_UNDERRUNS == OFF(snd_underruns)); CHECK(O_STR_STATE == OFF(str_state));
	CHECK(O_STR_LOAD == OFF(str_load)); CHECK(O_STR_UNDERRUNS == OFF(str_underruns));
	CHECK(ST_PAGE == sizeof(struct alice68_status)); CHECK(ST_MAGIC == ALICE68_STATUS_MAGIC);
	CHECK(ST_UNKNOWN == ALICE68_ST_UNKNOWN); CHECK(STF_SIMULATED == ALICE68_STF_SIMULATED);
	CHECK(OFF(core[1].busy) == O_CORE + 32 + 4); CHECK(OFF(core[2].name) == O_CORE + 64 + 8);
	CHECK(OFF(net[1].rx_bytes) == O_NET + 32 + 16); CHECK(OFF(net[1].tx_bytes) == O_NET + 32 + 20);
}

/* a page as the kernel writes it: QEMU's (no sensors) or a hot Pi 3B's */
static void page(unsigned size, int hot, unsigned long up_ms, unsigned long rx)
{
	memset(pg, 0, sizeof pg);
	put(O_MAGIC, ST_MAGIC); put(O_VERSION, 1); put(O_SIZE, size); put(O_SEQ, 42); put(O_UPTIME, up_ms);
	memcpy(pg + O_BUILD, "Emu68 1.1 psmac", 15);
	static const char *names[4] = { "68k JIT", "USB, sound, video card", "Mac bus", "services" };
	for (int i = 0; i < 4; i++) { put(O_CORE + 32 * i + 4, i ? 1234UL * i : ST_UNKNOWN); strcpy((char *)pg + O_CORE + 32 * i + 8, names[i]); }
	put(O_TEMP, hot ? 81234 : ST_UNKNOWN); put(O_ARM_HZ, hot ? 600000000UL : 700000000UL);
	put(O_THROTTLED, hot ? 0x000E000EUL : ST_UNKNOWN);
	put(O_JIT_TOTAL, 16UL << 20); put(O_JIT_FREE, 5UL << 20);
	put(O_NET + 32, 2); put(O_NET + 32 + 4, 1); put(O_NET + 32 + 16, rx); put(O_NET + 32 + 20, rx / 10);
	for (unsigned o = 0x1A0; o < 0x1F0; o += 4) put(o, ST_UNKNOWN);
	put(O_ARM_MAX, hot ? 1200000000UL : 700000000UL);
	put(O_ARM_MEAS, hot ? 600123456UL : ST_UNKNOWN);
	put(O_TEMP_MAX, hot ? 85000 : ST_UNKNOWN);
	put(O_INSN_RATE, hot ? 31000000UL : 169300000UL);
	put(O_HEAP_FREE, 43201234UL); put(O_HEAP_TOTAL, 64UL << 20);
	if (hot) { put(O_VC_STATE, 2); put(O_VC_LOAD, 734); put(O_VC_BUFFER, 8200); put(O_SND_OUT, 1); put(O_STR_STATE, 0); }
	if (size < ST_PAGE) memset(pg + size, 0xAA, ST_PAGE - size);   /* past an older page: whatever lies there */
}

int main(void)
{
	char b[64];
	struct st_now n;
	static struct st_model m;

	offsets();

	/* QEMU's page: the sensors not measured */
	page(ST_PAGE, 0, 100000, 1000);
	CHECK(st_parse(pg, &n) == 1);
	CHECK(n.temp_mc == ST_UNKNOWN); SAME(fmt_temp(b, n.temp_mc), "not measured");
	SAME(fmt_mhz(b, n.arm_hz), "700 MHz"); SAME(fmt_mhz(b, n.arm_meas_hz), "not measured");
	SAME(fmt_mips(b, n.insn_rate), "169.3 MIPS");
	CHECK(n.throttled == ST_UNKNOWN);
	SAME(n.core[1].name, "USB, sound, video card"); CHECK(n.core[0].busy == ST_UNKNOWN); CHECK(n.core[2].busy == 2468);
	SAME(fmt_pct(b, n.core[2].busy), "25%"); SAME(fmt_pct(b, n.core[0].busy), "--");
	SAME(fmt_mb(b, n.heap_free), "41.2 MB"); SAME(fmt_mb(b, n.jit_free), "5.0 MB");
	CHECK(n.vc_state == ST_UNKNOWN && n.snd_out == ST_UNKNOWN);
	SAME(n.build, "Emu68 1.1 psmac");

	/* a hot Pi 3B, frequency capped: the owner's case */
	page(ST_PAGE, 1, 100000, 1000);
	CHECK(st_parse(pg, &n) == 1);
	SAME(fmt_temp(b, n.temp_mc), "81.2\xA1" "C"); SAME(fmt_temp(b, n.temp_max_mc), "85.0\xA1" "C");
	SAME(fmt_mhz(b, n.arm_meas_hz), "600 MHz"); SAME(fmt_mhz(b, n.arm_max_hz), "1200 MHz");
	SAME(fmt_mips(b, n.insn_rate), "31.0 MIPS");
	CHECK(!TH_NOW(n.throttled, 0) && TH_NOW(n.throttled, 1) && TH_NOW(n.throttled, 2) && TH_NOW(n.throttled, 3));
	CHECK(!TH_PAST(n.throttled, 0) && TH_PAST(n.throttled, 1));
	CHECK(n.vc_state == 2 && n.vc_load == 734); SAME(fmt_secs(b, n.vc_buffer_ms), "8.2 s");

	/* an older kernel's page (size 0x1a0): the new fields not measured, whatever lies past the size */
	page(0x1A0, 1, 100000, 1000);
	CHECK(st_parse(pg, &n) == 1);
	CHECK(n.arm_max_hz == ST_UNKNOWN && n.insn_rate == ST_UNKNOWN && n.vc_state == ST_UNKNOWN && n.str_underruns == ST_UNKNOWN);
	CHECK(n.temp_mc == 81234);

	/* not a page */
	memset(pg, 0, sizeof pg);
	CHECK(st_parse(pg, &n) == 0 && !n.ok);
	put(O_MAGIC, ST_MAGIC); put(O_SIZE, 0x40);
	CHECK(st_parse(pg, &n) == 0);

	/* rates and history: one reading a second; rx grows 12.5 KB a second */
	memset(&m, 0, sizeof m);
	for (int s = 0; s < 10; s++) {
		page(ST_PAGE, s >= 5, 100000UL + 1000UL * s, 1000UL + 12800UL * s);
		st_parse(pg, &n);
		st_update(&m, &n);
	}
	CHECK(m.rx_bps[1] == 12800); SAME(fmt_rate(b, m.rx_bps[1]), "12.5 KB/s"); SAME(fmt_rate(b, m.tx_bps[1]), "1.3 KB/s");
	CHECK(m.hist_n == 5);                                     /* a point every two seconds */
	CHECK(st_hist(&m, m.h_temp, 0) == 81234 && st_hist(&m, m.h_mhz, 0) == 600);
	CHECK(st_hist(&m, m.h_temp, 4) == ST_UNKNOWN && st_hist(&m, m.h_mhz, 4) == 700);   /* QEMU's first seconds */
	CHECK(st_hist(&m, m.h_mips, 0) == 310 && st_hist(&m, m.h_mips, 4) == 1693);
	CHECK(st_hist(&m, m.h_temp, 5) == ST_UNKNOWN);
	CHECK(m.mips_peak == 169300000UL && mips_scale(m.mips_peak) == 200);
	SAME(fmt_rate(b, 3UL << 20), "3.0 MB/s");
	/* the Pi restarts: uptime goes back, the history starts again */
	page(ST_PAGE, 0, 2000, 0);
	st_parse(pg, &n); st_update(&m, &n);
	CHECK(m.hist_n == 1 && m.rx_bps[1] == ST_UNKNOWN);
	/* the history ring wraps */
	for (int s = 0; s < 2 * HIST_N + 7; s++) { page(ST_PAGE, 0, 4000UL + 2000UL * s, 0); st_parse(pg, &n); st_update(&m, &n); }
	CHECK(m.hist_n == HIST_N && st_hist(&m, m.h_mhz, HIST_N - 1) == 700 && st_hist(&m, m.h_mhz, HIST_N) == ST_UNKNOWN);

	/* dials */
	CHECK(dial_angle(ST_UNKNOWN, 0, 100) == -120); CHECK(dial_angle(0, 0, 100) == -120);
	CHECK(dial_angle(50, 0, 100) == 0); CHECK(dial_angle(1000, 0, 100) == 120);
	CHECK(dial_angle(600000000UL, 0, 1200000000UL) == 0); CHECK(dial_angle(1200000000UL, 0, 1400000000UL) == 85);
	CHECK(dial_angle(60000, 30000, 90000) == 0); CHECK(dial_angle(80000, 30000, 90000) == 80);
	CHECK(isin(0) == 0 && isin(90) == 16384 && isin(-90) == -16384 && isin(210) == -8192 && icos(0) == 16384 && icos(120) == -8192);
	{   /* a needle 34 pixels long at -110 degrees (8.8 MIPS on a 200 MIPS dial): left and a little down */
		volatile int a = dial_angle(8800000U, 0, 200000000U), r = 34;
		CHECK(a == -110 && isin(a) == -15396 && icos(a) == -5604);
		CHECK(isin(a) * r / 16384 == -31 && -icos(a) * r / 16384 == 11);
		CHECK(isin(-76) < 0 && icos(-76) > 0 && isin(-359) == 286 && isin(719) == -286);
	}
	CHECK(mips_scale(0) == 50 && mips_scale(49999999UL) == 50 && mips_scale(50000000UL) == 100 && mips_scale(250000000UL) == 300);

	printf("pistatus_test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
