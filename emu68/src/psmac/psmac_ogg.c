/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_ogg.c — Ogg Vorbis for the stream channel (psmac_codec.h): stb_vorbis (third_party/stb_vorbis,
 * public domain or MIT, unmodified; its README.md says which commit) compiled once here, and the glue.
 *
 * Written on 22 November 2026 (docs/08 item 105). stb_vorbis's pushdata interface takes whatever bytes it
 * is given and says how many it used, so it suits a stream that arrives in pieces: the bytes go from the
 * Pi's buffer into a window of this file's own, the decoder takes whole packets from its front, and a
 * packet's samples (up to 4096 frames) are given out across as many slices as the caller has room for.
 * The three headers must be in the window together before stb_vorbis_open_pushdata succeeds; a comment
 * header can carry a cover picture of hundreds of kilobytes, so the window grows (to OG_WIN_MAX) until
 * they fit, and shrinks back once they are read.
 *
 * Memory: stb_vorbis allocates from one buffer given to it (stb_vorbis_alloc), its setup tables at the
 * bottom and each packet's scratch at the top, so it never calls malloc and needs no alloca on core 1's
 * 64 KiB stack. Floating point: stb_vorbis works in single precision, on core 1 only (psmac_mp3.c says
 * why that is safe). Its setup needs sin, cos, exp, log, pow and floor, which the kernel has (src/math,
 * musl's), and ldexp, which is scalbn under another name. memcmp is this file's own (the kernel has none).
 * Its asserts are compiled out.
 */
#undef _FORTIFY_SOURCE
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "psmac_codec.h"

static int og_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (size_t i = 0; i < n; i++)
        if (p[i] != q[i]) return p[i] < q[i] ? -1 : 1;
    return 0;
}
static double og_ldexp(double x, int e) { return scalbn(x, e); }
__attribute__((unused)) static void *og_nomalloc(size_t n) { (void)n; return 0; }
__attribute__((unused)) static void *og_norealloc(void *p, size_t n) { (void)p; (void)n; return 0; }
__attribute__((unused)) static void og_nofree(void *p) { (void)p; }
#define memcmp og_memcmp
#define ldexp og_ldexp
#define malloc og_nomalloc
#define realloc og_norealloc
#define free og_nofree
#ifndef NDEBUG
#define NDEBUG
#endif
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PULLDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "third_party/stb_vorbis/stb_vorbis.c"
#undef memcmp
#undef ldexp
#undef malloc
#undef realloc
#undef free

#define OG_WIN       (64u << 10)               /* the window: an Ogg page is at most 65,307 bytes */
#define OG_WIN_MAX   (1536u << 10)             /* for the headers, a cover picture in the comments included */
#define OG_MEM       (512u << 10)              /* stb_vorbis's buffer; tried again at OG_MEM_MAX if too small */
#define OG_MEM_MAX   (2048u << 10)
struct og {
    stb_vorbis *v;
    stb_vorbis_alloc mem;
    uint8_t *win;
    uint32_t wsize, wn;
    float **pend;                              /* a packet's samples not yet given out */
    int pn, pat, pch;
};

static int og_grow(struct pcodec *c, struct og *o, uint32_t size)
{
    uint8_t *w = c->mem->alloc(size);
    if (!w) return 0;
    memcpy(w, o->win, o->wn);
    c->mem->release(o->win);
    o->win = w; o->wsize = size;
    return 1;
}
int pc_ogg_open(struct pcodec *c)
{
    struct og *o = c->mem->alloc(sizeof *o);
    if (!o) { c->error = "no memory for the Vorbis decoder"; return 0; }
    memset(o, 0, sizeof *o);
    o->win = c->mem->alloc(OG_WIN);
    if (!o->win) { c->mem->release(o); c->error = "no memory for the Vorbis decoder"; return 0; }
    o->wsize = OG_WIN;
    c->priv = o;
    return 1;
}
void pc_ogg_close(struct pcodec *c)
{
    struct og *o = c->priv;
    if (!o) return;
    if (o->v) stb_vorbis_close(o->v);
    if (o->mem.alloc_buffer) c->mem->release(o->mem.alloc_buffer);
    if (o->win) c->mem->release(o->win);
    c->mem->release(o);
}
static void og_fill(struct og *o, struct pc_src *s)
{
    o->wn += pc_read(s, o->win + o->wn, o->wsize - o->wn);
}
static void og_used(struct og *o, int used)
{
    if (used <= 0) return;
    if ((uint32_t)used > o->wn) used = (int)o->wn;
    memmove(o->win, o->win + used, o->wn - (uint32_t)used);
    o->wn -= (uint32_t)used;
}
static int og_headers(struct pcodec *c, struct og *o, struct pc_src *s)
{
    for (;;) {
        og_fill(o, s);
        int used = 0, err = 0;
        if (!o->mem.alloc_buffer) {
            o->mem.alloc_buffer_length_in_bytes = (int)OG_MEM;
            o->mem.alloc_buffer = c->mem->alloc(OG_MEM);
            if (!o->mem.alloc_buffer) { c->error = "no memory for the Vorbis decoder's tables"; return PC_ERROR; }
        }
        o->v = stb_vorbis_open_pushdata(o->win, (int)o->wn, &used, &err, &o->mem);
        if (o->v) {
            og_used(o, used);
            stb_vorbis_info in = stb_vorbis_get_info(o->v);
            c->rate = in.sample_rate; c->channels = (uint32_t)in.channels; c->bits = 16;
            if (c->resume) stb_vorbis_flush_pushdata(o->v);   /* not the file's first audio page: find a whole one */
            if (o->wsize > OG_WIN && o->wn <= OG_WIN) {    /* the window back to its size */
                uint8_t *w = c->mem->alloc(OG_WIN);
                if (w) { memcpy(w, o->win, o->wn); c->mem->release(o->win); o->win = w; o->wsize = OG_WIN; }
            }
            return 1;
        }
        if (err == VORBIS_outofmem && o->mem.alloc_buffer_length_in_bytes < (int)OG_MEM_MAX) {
            c->mem->release(o->mem.alloc_buffer);
            o->mem.alloc_buffer_length_in_bytes = (int)OG_MEM_MAX;
            o->mem.alloc_buffer = c->mem->alloc(OG_MEM_MAX);
            if (!o->mem.alloc_buffer) { c->error = "no memory for the Vorbis decoder's tables"; return PC_ERROR; }
            continue;
        }
        if (err != VORBIS_need_more_data) { c->error = "not an Ogg Vorbis stream stb_vorbis can read"; return PC_ERROR; }
        if (o->wn == o->wsize) {
            if (o->wsize >= OG_WIN_MAX || !og_grow(c, o, o->wsize * 2 > OG_WIN_MAX ? OG_WIN_MAX : o->wsize * 2)) {
                c->error = "the Ogg headers are larger than the window";
                return PC_ERROR;
            }
            continue;
        }
        if (s->all_in && !pc_avail(s)) { c->error = "the Ogg stream ended in its headers"; return PC_ERROR; }
        return PC_MORE;
    }
}
int pc_ogg_decode(struct pcodec *c, struct pc_src *s, int16_t *out, int max)
{
    struct og *o = c->priv;
    if (!o->v) {
        int r = og_headers(c, o, s);
        if (r <= 0) return r;
    }
    for (int tries = 0; !o->pn && tries < 16; tries++) {
        og_fill(o, s);
        int ch = 0, n = 0;
        float **pcm = 0;
        int used = stb_vorbis_decode_frame_pushdata(o->v, o->win, (int)o->wn, &ch, &pcm, &n);
        if (!used && !n) {
            if (s->all_in && !pc_avail(s)) return PC_END;     /* what is left is no whole packet */
            if (o->wn == o->wsize) {           /* a page larger than the window: only a damaged one is */
                if (o->wsize >= 4 * OG_WIN || !og_grow(c, o, o->wsize * 2)) og_used(o, (int)o->wn);
                continue;
            }
            return PC_MORE;
        }
        og_used(o, used);
        if (n > 0) {
            o->pend = pcm; o->pn = n; o->pat = 0; o->pch = ch;
            if (c->base < 0) {                 /* after a seek: the file's sample number, once a page has said it */
                int at = stb_vorbis_get_sample_offset(o->v);
                if (at >= 0) c->base = (int64_t)at - (int64_t)c->out;
            }
        }
    }
    if (!o->pn) return PC_MORE;
    int k = o->pn - o->pat < max ? o->pn - o->pat : max;
    const float *l = o->pend[0] + o->pat, *r = o->pend[o->pch > 1 ? 1 : 0] + o->pat;
    for (int i = 0; i < k; i++) {
        out[2 * i] = pc_clip16((int32_t)(l[i] * 32768.0f));
        out[2 * i + 1] = pc_clip16((int32_t)(r[i] * 32768.0f));
    }
    o->pat += k;
    if (o->pat >= o->pn) o->pn = 0;
    return k;
}
