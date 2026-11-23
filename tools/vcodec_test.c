/* SPDX-License-Identifier: MPL-2.0
 *
 * tools/vcodec_test.c - the video decoder card's frame decoders on the host (tools/vcodec_test.sh,
 * docs/16-video-card.md, docs/08 item 110).
 *
 *     vcodec_test desc.bin frames.bin out.rgb
 *
 * desc.bin and frames.bin come from tools/movsamples.py; every picture is decoded with the card's own
 * code (emu68/src/psmac/psmac_vcodec.c and the decoders) and written to out.rgb as 24-bit RGB, which the
 * script compares with FFmpeg's decoding of the same movie. The decoders run on the Pi, not the 68k, so
 * this is built for the host only (with the sanitizers, in `make -C mac check`).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "psmac_vcodec.h"

void *tlsf;                                     /* psmac_plmpeg.c's allocator, the C library's here (tools/mpg1_host/tlsf.h) */

static unsigned char *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)*n + 1);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) { perror(path); exit(2); }
    fclose(f);
    return b;
}
static unsigned be(const unsigned char *p, int n) { unsigned v = 0; while (n--) v = v << 8 | *p++; return v; }

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: vcodec_test desc.bin frames.bin out.rgb\n"); return 2; }
    long dn, fn;
    unsigned char *desc = slurp(argv[1], &dn), *frames = slurp(argv[2], &fn);
    if (dn < 86) { fprintf(stderr, "%s: not an ImageDescription\n", argv[1]); return 2; }
    const struct vc_codec *c = vc_find(be(desc + 4, 4));
    if (!c) { fprintf(stderr, "no decoder for '%.4s'\n", (char *)desc + 4); return 1; }
    struct vc_picture pic;
    memset(&pic, 0, sizeof pic);
    pic.w = be(desc + 32, 2); pic.h = be(desc + 34, 2); pic.depth = be(desc + 82, 2);
    if (!pic.w || !pic.h || pic.w > VC_MAX_W || pic.h > VC_MAX_H) { fprintf(stderr, "size %ux%u\n", pic.w, pic.h); return 1; }
    pic.rgb = calloc((size_t)pic.w * pic.h, 4);
    if (vc_wants_idx(c, pic.depth)) pic.idx = calloc((size_t)pic.w * pic.h, 1);
    vc_palette_from_desc(&pic, desc, (unsigned)dn);
    void *state = c->state_size ? calloc(1, c->state_size) : 0;
    if (c->open(state, &pic) != VC_OK) { fprintf(stderr, "%s: depth %u not decoded\n", c->name, pic.depth); return 1; }
    FILE *out = fopen(argv[3], "wb");
    unsigned char *row = malloc((size_t)pic.w * 3);
    long off = 0, nf = 0, bad = 0;
    clock_t t0 = clock();
    while (off + 4 <= fn) {
        unsigned n = be(frames + off, 4);
        off += 4;
        if (off + n > fn) break;
        if (c->decode(state, &pic, frames + off, n) != VC_OK) bad++;
        off += n; nf++;
        for (unsigned y = 0; y < pic.h; y++) {
            for (unsigned x = 0; x < pic.w; x++) {
                unsigned v = pic.rgb[y * pic.w + x];
                row[x * 3] = (unsigned char)(v >> 16); row[x * 3 + 1] = (unsigned char)(v >> 8); row[x * 3 + 2] = (unsigned char)v;
            }
            fwrite(row, 3, pic.w, out);
        }
    }
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    fclose(out);
    printf("%s %ux%u depth %u: %ld pictures (%ld with damaged data), %.3f ms a picture on this host with the conversion\n",
           c->name, pic.w, pic.h, pic.depth, nf, bad, nf ? ms / (double)nf : 0.0);
    free(state); free(pic.rgb); free(pic.idx); free(row); free(desc); free(frames);
    return bad ? 1 : 0;
}
