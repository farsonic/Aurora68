/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/codec_test.c - Aurora68 Play's FLAC, Ogg Vorbis and Apple Lossless path on the host, both halves:
 * the Mac's (mac/piplay/media.c: what the file is, its tags, the bytes for the card, seeking) and the
 * card's (emu68/src/psmac/psmac_codec.c and the three decoders). tools/codec_test.sh makes the test files
 * with ffmpeg (tones and noise, never music) and runs this on each; `make -C mac check` runs that.
 *
 *     codec_test FILE REF.s16 exact|lossy [TITLE ARTIST]   decode FILE as the card would, compare with REF
 *                                                         (ffmpeg's decoding, 16-bit stereo little-endian):
 *                                                         from the start, then from a seek to a third of it
 *     codec_test FILE - refuse                             the file is known and refused (AAC, Opus)
 *
 * The bytes go as they would on the SE: media.c writes them in pieces of varying size into a ring of the
 * card's size (2 MiB), and the decoder is called between pieces with "all in" false until the last one.
 * exact: every sample the same as ffmpeg's (FLAC and ALAC are lossless; 24-bit cut to 16 the same way);
 * lossy: within 0.2 % of full scale after the best alignment within 4096 frames (two Vorbis decoders differ
 * in their floating point and in how much of the first packet they keep). Written on 22 November 2026
 * (docs/08 item 105).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../mac/piplay/media.h"
#include "../emu68/src/psmac/psmac_codec.h"
#include "../emu68/src/psmac/psmb.h"

static long io_read(void *u, unsigned long off, void *buf, long n)
{
	FILE *f = u;
	if (fseek(f, (long)off, SEEK_SET)) return -1;
	return (long)fread(buf, 1, (size_t)n, f);
}
static void *h_alloc(size_t n) { return malloc(n); }
static void h_free(void *p) { free(p); }
static const struct pc_alloc mem = { h_alloc, h_free };

#define RING (2u << 20)
static uint8_t ring[RING];
static int16_t *out;
static long out_n, out_max;
static unsigned seed = 12345;
static unsigned rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }

/* the whole stream from ms: its frames into out; the codec's base; 0 on an error */
static unsigned long alac_first;
static int run(struct media *m, const struct mf_io *io, unsigned long ms, struct pcodec *c, unsigned long *start_ms)
{
	struct mf_stream st;
	struct pc_src s = { ring, RING - 1, 0, 0, 0 };
	static unsigned char buf[32768];
	int16_t pcm[2 * 1152];
	out_n = 0;
	if (!mf_seek(m, &st, io, ms, start_ms)) { printf("mf_seek failed\n"); return 0; }
	alac_first = st.pkt * m->frame_len;         /* ALAC: the Mac knows the packet exactly, the card does not */
	uint32_t fmt = m->kind == MF_FLAC ? PSMB_STR_FLAC : m->kind == MF_OGG ? PSMB_STR_OGG : PSMB_STR_ALAC;
	if (!pc_open(c, fmt, *start_ms != 0, &mem)) { printf("pc_open failed: %s\n", c->error ? c->error : "?"); return 0; }
	for (;;) {
		uint32_t room = RING - (s.head - s.tail);
		if (!s.all_in && room > sizeof buf) {
			long want = 1 + (long)(rnd() % sizeof buf), n, k;
			n = mf_next(m, &st, io, buf, want);
			if (n < 0) { printf("mf_next failed\n"); return 0; }
			for (k = 0; k < n; k++) ring[(s.head + (uint32_t)k) & (RING - 1)] = buf[k];
			s.head += (uint32_t)n;
			if (st.phase == 3) s.all_in = 1;       /* media.c has sent the last byte */
		}
		int r = pc_decode(c, &s, pcm, 1 + (int)(rnd() % 1152));
		if (r == PC_ERROR) { printf("pc_decode: %s\n", c->error ? c->error : "?"); return 0; }
		if (r == PC_END) break;
		if (r == PC_MORE && s.all_in && pc_avail(&s) == 0) {
			/* nothing left to give: the decoder must say the end on the next call */
		}
		if (r > 0) {
			if (out_n + r > out_max) { out_max = (out_n + r) * 2; out = realloc(out, (size_t)out_max * 4); }
			memcpy(out + 2 * out_n, pcm, (size_t)r * 4);
			out_n += r;
		}
	}
	return 1;
}
static int16_t *ref;
static long ref_n;
/* out against ref from frame at: the number of samples that differ (exact), or the worst difference
   after the best alignment (lossy) */
static long compare(long at, int exact, long *lag_out)
{
	if (exact) {
		long bad = 0, i;
		if (at + out_n > ref_n) { printf("  %ld frames decoded from %ld, the reference has %ld\n", out_n, at, ref_n); return 1; }
		for (i = 0; i < 2 * out_n; i++)
			if (out[i] != ref[2 * at + i]) { if (bad < 4) printf("  frame %ld channel %ld: %d, ffmpeg %d\n", at + i / 2, i % 2, out[i], ref[2 * at + i]); bad++; }
		*lag_out = 0;
		return bad;
	}
	long best = -1, best_lag = 0, lag;
	for (lag = -4096; lag <= 4096; lag += 1) {
		long n = 0, i, worst = 0;
		double err = 0;
		for (i = 8192; i < out_n - 8192 && i < 8192 + 44100; i += 7) {
			long r = at + i + lag;
			if (r < 0 || r >= ref_n) continue;
			long d = labs((long)out[2 * i] - ref[2 * r]) + labs((long)out[2 * i + 1] - ref[2 * r + 1]);
			err += (double)d;
			if (d > worst) worst = d;
			n++;
		}
		if (n < 1000) continue;
		if (best < 0 || err / n < best) { best = (long)(err / n); best_lag = lag; }
	}
	*lag_out = best_lag;
	{   /* the worst sample at the best alignment */
		long i, worst = 0;
		for (i = 8192; i < out_n - 8192; i++) {
			long r = at + i + best_lag;
			if (r < 0 || r >= ref_n) continue;
			long d = labs((long)out[2 * i] - ref[2 * r]);
			if (labs((long)out[2 * i + 1] - ref[2 * r + 1]) > d) d = labs((long)out[2 * i + 1] - ref[2 * r + 1]);
			if (d > worst) worst = d;
		}
		return worst;
	}
}

int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "codec_test FILE REF.s16|- exact|lossy|refuse [TITLE ARTIST]\n"); return 2; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 2; }
	fseek(f, 0, SEEK_END);
	struct mf_io io = { io_read, f, (unsigned long)ftell(f) };
	static unsigned char head[32768];
	long got = io_read(f, 0, head, sizeof head);
	static struct media m;
	int ok = mf_identify(&m, &io, head, got);
	const char *name = strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];
	if (!strcmp(argv[3], "refuse")) {
		int pass = !ok && m.kind != MF_NONE && mf_why(&m)[0];
		printf("%s %s: refused (%d): %s\n", pass ? "PASS" : "FAIL", name, m.kind, mf_why(&m));
		return !pass;
	}
	if (!ok) { printf("FAIL %s: not identified (%d) %s\n", name, m.kind, mf_why(&m)); return 1; }
	int exact = !strcmp(argv[3], "exact"), fails = 0;
	printf("%s: %s, %lu Hz, %lu channels, %lu bits, %lu frames, %lu.%03lu s; \"%s\" by \"%s\"\n", name, mf_name(&m), m.rate, m.channels,
	       m.bits, m.frames, m.length_ms / 1000, m.length_ms % 1000, m.title, m.artist);
	if (!strcmp(argv[3], "tags")) {
		int pass = argc >= 6 && !strcmp(m.title, argv[4]) && !strcmp(m.artist, argv[5]);
		printf("%s %s: title \"%s\", artist \"%s\"\n", pass ? "PASS" : "FAIL", name, m.title, m.artist);
		return !pass;
	}
	FILE *r = fopen(argv[2], "rb");
	if (!r) { perror(argv[2]); return 2; }
	fseek(r, 0, SEEK_END);
	ref_n = ftell(r) / 4;
	ref = malloc((size_t)ref_n * 4 + 4);
	fseek(r, 0, SEEK_SET);
	if (fread(ref, 4, (size_t)ref_n, r) != (size_t)ref_n) return 2;
	fclose(r);
	struct pcodec c;
	memset(&c, 0, sizeof c);
	unsigned long start = 0;
	long lag = 0, d;
	/* from the start */
	if (!run(&m, &io, 0, &c, &start)) { printf("FAIL %s: from the start\n", name); return 1; }
	d = compare(0, exact, &lag);
	int good = exact ? d == 0 && out_n == ref_n : d < 140 && labs(out_n - ref_n) < 4096;
	printf("%s %s: from the start, %ld frames (ffmpeg %ld), %s %ld%s\n", good ? "PASS" : "FAIL", name, out_n, ref_n,
	       exact ? "samples differing" : "worst difference", d, exact ? "" : " (of 32768)");
	if (!exact) printf("  aligned at %ld frames\n", lag);
	fails += !good;
	if (c.rate != m.rate) { printf("FAIL %s: the card says %u Hz, the Mac %lu\n", name, c.rate, m.rate); fails++; }
	pc_close(&c);
	/* from a seek to a third */
	unsigned long ms = m.length_ms / 3;
	if (!run(&m, &io, ms, &c, &start)) { printf("FAIL %s: the seek\n", name); return 1; }
	if (c.base < 0 && m.kind == MF_ALAC) c.base = (int64_t)alac_first;   /* what the kernel does with start_ms */
	if (c.base < 0) { printf("FAIL %s: after the seek the card does not know where it is\n", name); fails++; }
	else {
		long base_ms = (long)(c.base * 1000 / c.rate);
		d = compare((long)c.base, exact, &lag);
		good = (exact ? d == 0 : d < 140 && labs(lag) < 4096) && labs(base_ms - (long)ms) < 2500 && labs((long)start - base_ms) < 2500;
		printf("%s %s: seek to %lu ms: the Mac starts at %lu ms, the card finds frame %lld (%ld ms); %ld frames, %s %ld\n",
		       good ? "PASS" : "FAIL", name, ms, start, (long long)c.base, base_ms, out_n, exact ? "samples differing" : "worst difference", d);
		fails += !good;
	}
	pc_close(&c);
	fclose(f);
	return fails != 0;
}
