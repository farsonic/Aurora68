/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_codec.c — which decoder a stream of the stream channel gets (psmac_codec.h). The decoders are
 * psmac_flac.c (dr_flac), psmac_ogg.c (stb_vorbis) and psmac_alac.c (this project's own); MPEG audio and
 * PCM stay in psmac_audio.c, as they were. No kernel calls here or in the three: tools/codec_test.c
 * builds them on the host and checks their output against ffmpeg's (docs/08 item 103).
 */
#include "psmac_codec.h"
#include "psmb.h"

uint32_t pc_read(struct pc_src *s, void *dst, uint32_t n)
{
    uint32_t a = pc_avail(s), k = 0;
    uint8_t *d = dst;
    if (n > a) n = a;
    while (k < n) {                           /* in at most two pieces: up to the ring's end, then from its start */
        uint32_t o = (s->tail + k) & s->mask, c = n - k;
        if (c > s->mask + 1 - o) c = s->mask + 1 - o;
        for (uint32_t i = 0; i < c; i++)
            d[k + i] = s->buf[o + i];
        k += c;
    }
    s->tail += n;
    return n;
}

int pc_open(struct pcodec *c, uint32_t format, int resume, const struct pc_alloc *mem)
{
    pc_close(c);
    c->format = format; c->resume = resume; c->mem = mem;
    c->rate = c->channels = c->bits = 0; c->total = 0; c->base = resume ? -1 : 0; c->out = 0; c->error = 0;
    switch (format) {
    case PSMB_STR_FLAC: return pc_flac_open(c);
    case PSMB_STR_OGG: return pc_ogg_open(c);
    case PSMB_STR_ALAC: return pc_alac_open(c);
    }
    c->format = 0;
    return 0;
}
int pc_decode(struct pcodec *c, struct pc_src *s, int16_t *out, int max)
{
    int n = PC_ERROR;
    switch (c->format) {
    case PSMB_STR_FLAC: n = pc_flac_decode(c, s, out, max); break;
    case PSMB_STR_OGG: n = pc_ogg_decode(c, s, out, max); break;
    case PSMB_STR_ALAC: n = pc_alac_decode(c, s, out, max); break;
    default: c->error = "no stream";
    }
    if (n > 0) c->out += (uint64_t)n;
    return n;
}
void pc_close(struct pcodec *c)
{
    switch (c->format) {
    case PSMB_STR_FLAC: pc_flac_close(c); break;
    case PSMB_STR_OGG: pc_ogg_close(c); break;
    case PSMB_STR_ALAC: pc_alac_close(c); break;
    }
    c->format = 0;
    c->priv = 0;
}
