/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/mpg1_test.c - MPEG-1 in QuickTime, on the host: the import's indexer and the card's decoders together
 * (tools/mpg1_test.sh, `make -C mac check`; docs/16-video-card.md "MPEG-1 in QuickTime", docs/08 item 113).
 *
 *     mpg1_test film.mpg out.yuv out.pcm
 *
 * Does on the host what the Mac and the Pi do between them when MoviePlayer opens an MPEG-1 file:
 *  1. the file is read in 32 KB pieces into the import's indexer (mac/vdec/mpgindex.c, the same file the
 *     68k compiles): the pictures' starts and types, the sequence header, the MP2 sound's bytes;
 *  2. mi_order makes QuickTime's samples, in display order, and mi_image_description the 'mpg1' description;
 *  3. every sample, in that order, is decoded by the card's 'mpg1' (emu68/src/psmac/psmac_plmpeg.c with
 *     pl_mpeg) as QuickTime would hand it to the decompressor: the sample's bytes of the file, nothing else.
 *     The pictures go to out.yuv (planar 4:2:0, as FFmpeg's rawvideo yuv420p), which the script compares with
 *     FFmpeg's own decoding of the file;
 *  4. seeking: for every sync sample after the first, the same session (holding whatever pictures it held)
 *     and a new one each decode from that sync sample on for a GOP's worth of samples, and every picture must
 *     be the one the straight run gave; and QuickTime 2.5's own way of jumping, which hands over the sample of
 *     the time asked for alone: for every sample, from a session left anywhere else, the decompressor's catch-up
 *     (mi_catchup: the samples from the sync sample before it, B-pictures alone left out) and then the sample,
 *     which must again give the straight run's picture;
 *  5. the sound: the MP2 bytes in 4 KB pieces through the card's MP2 session, its PCM (16-bit big-endian
 *     stereo) to out.pcm, which the script compares with FFmpeg's decoding.
 * Prints one line of numbers for the script. Exit status 0 when every step worked.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "psmac_vcodec.h"
#include "../mac/vdec/mpgindex.h"

void *tlsf;                                     /* psmac_plmpeg.c's allocator (tools/mpg1_host/tlsf.h: the C library's) */

struct film { uint32_t *pics; uint32_t n, cap; uint8_t *aud; uint32_t an, acap; };
static void on_pic(void *u, uint32_t off, uint32_t type)
{
    struct film *f = u;
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 256; f->pics = realloc(f->pics, f->cap * sizeof *f->pics); }
    f->pics[f->n++] = off << 2 | type;
}
static void on_aud(void *u, const uint8_t *p, uint32_t n)
{
    struct film *f = u;
    if (f->an + n > f->acap) { while (f->an + n > f->acap) f->acap = f->acap ? f->acap * 2 : 65536; f->aud = realloc(f->aud, f->acap); }
    memcpy(f->aud + f->an, p, n); f->an += n;
}
struct sample { uint32_t off, size; int sync; };
static struct sample *smp; static uint32_t nsmp;
static void emit(void *u, uint32_t off, uint32_t size, int flags) { (void)u; smp[nsmp].off = off; smp[nsmp].size = size; smp[nsmp].sync = flags & MI_SYNC; nsmp++; }

static uint8_t *file; static long flen;
static uint8_t desc[MI_DESC_MAX]; static uint32_t desc_len;
static uint32_t W, H;

struct session { void *state; struct vc_picture pic; };
static int sess_open(struct session *s)
{
    memset(s, 0, sizeof *s);
    s->pic.w = W; s->pic.h = H; s->pic.depth = 24;
    s->pic.rgb = calloc((size_t)W * H, 4);
    s->state = calloc(1, vc_mpg1.state_size);
    s->pic.desc = desc; s->pic.desc_len = desc_len;
    int e = vc_mpg1.open(s->state, &s->pic);
    s->pic.desc = 0; s->pic.desc_len = 0;
    return e;
}
static void sess_close(struct session *s) { vc_mpg1.close(s->state); free(s->state); free(s->pic.rgb); }
/* one sample decoded; its picture as yuv420p into out (W*H*3/2 bytes) */
static int decode(struct session *s, uint32_t k, uint8_t *out)
{
    vc_untouched(&s->pic);
    int e = vc_mpg1.decode(s->state, &s->pic, file + smp[k].off, smp[k].size);
    const uint8_t *y, *cb, *cr; uint32_t ys, cs;
    if (!vc_mpg1_planes(s->state, &y, &cb, &cr, &ys, &cs)) return -1;
    for (uint32_t r = 0; r < H; r++) memcpy(out + r * W, y + r * ys, W);
    uint8_t *u = out + W * H, *v = u + (W / 2) * (H / 2);
    for (uint32_t r = 0; r < H / 2; r++) { memcpy(u + r * (W / 2), cb + r * cs, W / 2); memcpy(v + r * (W / 2), cr + r * cs, W / 2); }
    return e;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: mpg1_test film.mpg out.yuv out.pcm\n"); return 2; }
    FILE *fi = fopen(argv[1], "rb");
    if (!fi) { perror(argv[1]); return 2; }
    fseek(fi, 0, SEEK_END); flen = ftell(fi); fseek(fi, 0, SEEK_SET);
    file = malloc((size_t)flen + 1);
    if (fread(file, 1, (size_t)flen, fi) != (size_t)flen) { perror(argv[1]); return 2; }
    fclose(fi);

    /* 1. the indexer, in the 68k's pieces */
    struct film f = { 0 };
    struct mi m;
    mi_init(&m, on_pic, on_aud, &f);
    for (long o = 0; o < flen; o += 32768) mi_feed(&m, file + o, (uint32_t)(flen - o < 32768 ? flen - o : 32768));
    if (!m.seq.have || !f.n) { printf("mpg1_test: no MPEG-1 video found (kind %d, %u pictures)\n", m.kind, f.n); return 1; }
    W = m.seq.w; H = m.seq.h;
    uint32_t num, den;
    mi_rate(m.seq.rate_code, &num, &den);

    /* 2. QuickTime's samples and description */
    smp = calloc(f.n + 1, sizeof *smp);
    uint32_t ns = mi_order(f.pics, f.n, m.video_end, emit, 0), nsync = 0;
    for (uint32_t k = 0; k < nsmp; k++) nsync += smp[k].sync;
    desc_len = mi_image_description(desc, sizeof desc, &m);
    if (ns != f.n || !desc_len) { printf("mpg1_test: %u samples for %u pictures, description %u bytes\n", ns, f.n, desc_len); return 1; }
    {   /* the decompressor's index, as the import appends it, found again; every sample where mi_order put it */
        uint8_t spec[70] = { 0 }, *x = malloc(desc_len + MI_INDEX_HDR + 4 * f.n);
        memcpy(x, desc, desc_len);
        uint32_t xl = desc_len + mi_index_atom(x + desc_len, MI_INDEX_HDR + 4 * f.n, spec, (uint32_t)flen, m.video_end, f.pics, f.n);
        const uint8_t *fs_, *pb; uint32_t fsz, end, n, off, size; int fl, ok = mi_index_find(x, xl, &fs_, &fsz, &end, &pb, &n);
        ok = ok && n == f.n && end == m.video_end && fsz == (uint32_t)flen;
        for (uint32_t i = 0; ok && i < n; i++) ok = ((uint32_t)pb[4 * i] << 24 | (uint32_t)pb[4 * i + 1] << 16 | (uint32_t)pb[4 * i + 2] << 8 | pb[4 * i + 3]) == f.pics[i];
        for (uint32_t k = 0; ok && k < nsmp; k++) ok = mi_sample_at(f.pics, f.n, m.video_end, k, &off, &size, &fl) && off == smp[k].off && size == smp[k].size && (fl & MI_SYNC) == smp[k].sync;
        free(x);
        if (!ok) { printf("mpg1_test: the decompressor's index did not come back as it went in\n"); return 1; }
    }

    /* 3. every sample in display order */
    size_t fs = (size_t)W * H * 3 / 2;
    uint8_t *all = malloc(fs * nsmp), *tmp = malloc(fs);
    struct session s;
    if (sess_open(&s) != VC_OK) { printf("mpg1_test: the card's 'mpg1' did not open\n"); return 1; }
    int bad = 0;
    clock_t t0 = clock();
    for (uint32_t k = 0; k < nsmp; k++)
        if (decode(&s, k, all + k * fs) != VC_OK) bad++;
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / nsmp;
    FILE *fo = fopen(argv[2], "wb");
    fwrite(all, fs, nsmp, fo);
    fclose(fo);

    /* 4. seeks: from every sync sample, with the old session and with a new one */
    uint32_t seeks = 0, seek_bad = 0;
    for (uint32_t k = 1; k < nsmp; k++) {
        if (!smp[k].sync) continue;
        struct session fresh;
        sess_open(&fresh);
        for (uint32_t j = k; j < nsmp && j < k + 16; j++) {
            decode(&s, j, tmp);
            if (memcmp(tmp, all + j * fs, fs)) seek_bad++;
            decode(&fresh, j, tmp);
            if (memcmp(tmp, all + j * fs, fs)) seek_bad++;
        }
        sess_close(&fresh);
        seeks++;
    }
    /* QuickTime's jumps: sample k alone, after the 'mpg1' decompressor's catch-up, from a session that was elsewhere */
    uint32_t jumps = 0, jump_bad = 0, caught = 0;
    struct mi_sample *cu = calloc(f.n + 1, sizeof *cu);
    for (uint32_t k = 0; k < nsmp; k += 1) {
        uint32_t from = (k * 7 + 31) % nsmp;           /* where the session was left: anywhere */
        decode(&s, from, tmp);
        uint32_t nc = mi_catchup(f.pics, f.n, m.video_end, k, cu, f.n);
        for (uint32_t c = 0; c < nc; c++) {
            if (cu[c].off != smp[cu[c].index].off || cu[c].size != smp[cu[c].index].size) jump_bad++;
            decode(&s, cu[c].index, tmp);
        }
        caught += nc;
        decode(&s, k, tmp);
        if (memcmp(tmp, all + k * fs, fs)) jump_bad++;
        jumps++;
    }
    free(cu);
    /* and the redraws QuickTime makes of a picture it already showed: the same sample twice */
    uint32_t again_bad = 0;
    struct session r2;
    sess_open(&r2);
    for (uint32_t k = 0; k < nsmp; k++) {
        decode(&r2, k, tmp);
        decode(&r2, k, tmp);
        if (memcmp(tmp, all + k * fs, fs)) again_bad++;
    }
    sess_close(&r2);
    sess_close(&s);

    /* 5. the sound */
    uint32_t pcm_bytes = 0, rate = 0;
    if (f.an) {
        void *a = vc_mp2_open();
        FILE *fp = fopen(argv[3], "wb");
        static uint8_t out[65536];
        for (uint32_t o = 0; o <= f.an; o += 4096) {
            uint32_t n = f.an - o < 4096 ? f.an - o : 4096, more = 1;
            uint32_t w = vc_mp2_decode(a, f.aud + o, n, out, sizeof out, &more, &rate);
            fwrite(out, 1, w, fp); pcm_bytes += w;
            while (more) { w = vc_mp2_decode(a, 0, 0, out, 1000, &more, &rate); fwrite(out, 1, w, fp); pcm_bytes += w; }
            if (!n) break;
        }
        fclose(fp);
        vc_mp2_close(a);
    } else { FILE *fp = fopen(argv[3], "wb"); fclose(fp); }

    printf("%s %ux%u at %u/%u frames a second, %u pictures (%u I, %u P, %u B), %u samples (%u sync), description %u bytes; "
           "%.3f ms a picture on this host; %u seeks, %u pictures wrong after them; %u jumps caught up with %u samples, %u wrong; %u wrong when decoded twice; "
           "sound: %u bytes of layer %u at %u Hz (%u kbit/s) into %u bytes of PCM at %u Hz; %d damaged\n",
           m.kind == MI_PS ? "program stream" : "video stream", W, H, num, den, f.n, m.ni, m.np, m.nb, nsmp, nsync, desc_len,
           ms, seeks, seek_bad, jumps, caught, jump_bad, again_bad, f.an, m.aud.layer, m.aud.rate, m.aud.kbps, pcm_bytes, rate, bad);
    free(all); free(tmp); free(smp); free(f.pics); free(f.aud); free(file);
    return bad || seek_bad || jump_bad || again_bad ? 1 : 0;
}
