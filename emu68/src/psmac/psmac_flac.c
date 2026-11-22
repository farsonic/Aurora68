/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_flac.c — FLAC for the stream channel (psmac_codec.h): dr_flac (third_party/dr_flac, public domain or
 * MIT-0, unmodified; its README.md says which release) compiled once here, and the glue that feeds it.
 *
 * Written on 22 November 2026 (docs/08 item 103). dr_flac pulls its bytes through a read callback, and
 * a callback cannot wait for the Mac: a short read means the end of the file to it. So a frame is decoded
 * only when the Pi's buffer holds more than the largest frame can take (STREAMINFO's maximum frame size,
 * or the size of a block of samples stored raw when the encoder left that out) and the decoder's own
 * read-ahead, or when the Mac has sent everything. The Mac sends STREAMINFO alone as the metadata
 * (mac/piplay/media.c), so dr_flac never needs to seek. dr_flac finds the next frame's sync code and
 * checks its header's CRC-8 itself, so a stream that starts at a seek's guess of a byte goes on from the
 * first whole frame after it; the frame's header says which sample it starts at, and that is the stream's
 * base (the position the Mac shows).
 *
 * Options: DR_FLAC_NO_SIMD (its NEON code is written for little-endian lanes, as minimp3's is:
 * psmac_mp3.c), no stdio, no wide characters, no Ogg-FLAC (FLAC in Ogg is rare, and Aurora68 Play tells
 * an Ogg FLAC file from an Ogg Vorbis one and says so). dr_flac does integer arithmetic only.
 */
#undef _FORTIFY_SOURCE                 /* the distribution compiler's default; the kernel has no __memcpy_chk */
#include <string.h>
#include "psmac_codec.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_WCHAR
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_OGG
#define DRFLAC_ASSERT(expression) ((void)0)
static void *fl_none(size_t sz) { (void)sz; return 0; }   /* dr_flac's defaults: everything goes through the callbacks below */
static void *fl_none2(void *p, size_t sz) { (void)p; (void)sz; return 0; }
static void fl_none3(void *p) { (void)p; }
#define DRFLAC_MALLOC(sz) fl_none(sz)
#define DRFLAC_REALLOC(p, sz) fl_none2((p), (sz))
#define DRFLAC_FREE(p) fl_none3(p)
#define DRFLAC_COPY_MEMORY(dst, src, sz) memcpy((dst), (src), (sz))
#define DRFLAC_ZERO_MEMORY(p, sz) memset((p), 0, (sz))
#include "third_party/dr_flac/dr_flac.h"

#define FL_CHUNK 1152u                         /* frames per slice: what psmac_audio.c's MPEG path gives too */
struct fl {
    drflac *d;
    struct pc_src *src;                        /* during a call into dr_flac */
    uint32_t need;                             /* bytes buffered before a frame is decoded, unless all are in */
    int first;                                 /* no frame decoded yet: the base still to be learnt */
    drflac_allocation_callbacks cb;
    int16_t pcm[FL_CHUNK * 8];
};

static size_t fl_read(void *u, void *out, size_t n)
{
    struct fl *f = u;
    return pc_read(f->src, out, (uint32_t)n);
}
static drflac_bool32 fl_seek(void *u, int offset, drflac_seek_origin origin)
{
    struct fl *f = u;                          /* only forward over bytes that are here: the Mac sends no blocks to skip */
    if (origin != DRFLAC_SEEK_CUR || offset < 0 || (uint32_t)offset > pc_avail(f->src)) return DRFLAC_FALSE;
    f->src->tail += (uint32_t)offset;
    return DRFLAC_TRUE;
}
static void *fl_malloc(size_t n, void *u) { const struct pc_alloc *m = u; return m->alloc(n); }
static void fl_free(void *p, void *u) { const struct pc_alloc *m = u; if (p) m->release(p); }

int pc_flac_open(struct pcodec *c)
{
    struct fl *f = c->mem->alloc(sizeof *f);
    if (!f) { c->error = "no memory for the FLAC decoder"; return 0; }
    memset(f, 0, sizeof *f);
    f->cb.pUserData = (void *)c->mem;
    f->cb.onMalloc = fl_malloc;
    f->cb.onFree = fl_free;
    f->first = 1;
    c->priv = f;
    return 1;
}
void pc_flac_close(struct pcodec *c)
{
    struct fl *f = c->priv;
    if (!f) return;
    if (f->d) drflac_close(f->d);
    c->mem->release(f);
}
int pc_flac_decode(struct pcodec *c, struct pc_src *s, int16_t *out, int max)
{
    struct fl *f = c->priv;
    if (!f->d) {
        /* "fLaC", then STREAMINFO: block sizes 16+16, frame sizes 24+24, rate 20, channels - 1 3, bits - 1 5,
           total samples 36, MD5 128 bits */
        if (pc_avail(s) < 42) { if (s->all_in) { c->error = "the FLAC stream ended in its header"; return PC_ERROR; } return PC_MORE; }
        uint8_t h[42];
        for (int i = 0; i < 42; i++)
            h[i] = pc_peek(s, (uint32_t)i);
        if (h[0] != 'f' || h[1] != 'L' || h[2] != 'a' || h[3] != 'C' || (h[4] & 0x7F) != 0) { c->error = "no FLAC header"; return PC_ERROR; }
        uint32_t maxblock = (uint32_t)h[10] << 8 | h[11], maxframe = (uint32_t)h[15] << 16 | (uint32_t)h[16] << 8 | h[17];
        uint32_t ch = ((h[20] >> 1) & 7) + 1, bits = ((uint32_t)(h[20] & 1) << 4 | h[21] >> 4) + 1;
        uint32_t raw = (maxblock ? maxblock : 65535) * ch * (bits + 1) / 8 + 64;   /* a block stored verbatim */
        f->need = (maxframe && maxframe < raw ? maxframe : raw) + 2 * DR_FLAC_BUFFER_SIZE;
        if (f->need > (s->mask + 1) / 2) f->need = (s->mask + 1) / 2;    /* half the Pi's buffer at most (1 MiB) */
        f->src = s;
        f->d = drflac_open(fl_read, fl_seek, 0, f, &f->cb);
        if (!f->d) { c->error = "dr_flac did not take the FLAC header"; return PC_ERROR; }
        c->rate = f->d->sampleRate; c->channels = f->d->channels; c->bits = f->d->bitsPerSample;
        c->total = f->d->totalPCMFrameCount;
    }
    if (pc_avail(s) < f->need && !s->all_in) return PC_MORE;
    if (max > (int)FL_CHUNK) max = (int)FL_CHUNK;
    f->src = s;
    drflac_uint64 n = drflac_read_pcm_frames_s16(f->d, (drflac_uint64)max, f->pcm);
    if (!n) {
        if (s->all_in) return PC_END;
        c->error = "a FLAC frame that dr_flac could not read";
        return PC_ERROR;
    }
    if (f->first) {                            /* where in the file this is: the frame's own number */
        drflac_uint64 lo = 0, hi = 0;
        drflac__get_pcm_frame_range_of_current_flac_frame(f->d, &lo, &hi);
        uint64_t used = (hi - lo + 1) - f->d->currentFLACFrame.pcmFramesRemaining;
        c->base = (int64_t)(lo + used - n);
        f->first = 0;
    }
    uint32_t ch = f->d->channels;
    for (drflac_uint64 i = 0; i < n; i++) {
        out[2 * i] = f->pcm[i * ch];
        out[2 * i + 1] = f->pcm[i * ch + (ch > 1)];
    }
    return (int)n;
}
