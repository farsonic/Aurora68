/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_vframes.c - the video decoder card's frame decoder: QuickTime pictures from the Mac decoded on
 * core 1 and drawn into the Mac's pixel maps (psmac_video.h, docs/16-video-card.md, GitHub issue #13).
 *
 * Backstory (22 November 2026, docs/08 item 110). The owner: "This video card should leverage the Pi
 * hardware and have no external dependencies. Think of it as an in-box video processing card that
 * integrates into QuickTime and is also available to other programs through traditional means." The
 * stream half of the card (psmac_video.c) plays an MPEG stream a program pours into its ring; this half
 * is the one QuickTime uses. QuickTime asks a decompressor component for each picture of a movie
 * (ImageCodecBandDecompress: the compressed bytes, a pixel map, where in it); the card's component (the
 * Mac's "Aurora68 Video Decoder" extension) passes that on through the card's driver, which writes it into
 * one of the slots below and waits. Here, on core 1 between the sound's turns:
 *   1. the compressed bytes are copied out of the Mac's memory (68k logical addresses, page by page through
 *      the MMU shim's table, psmac_mbox.c), and decoded into the session's own picture (psmac_vcodec.h:
 *      Cinepak, Apple Video, Animation, Graphics);
 *   2. the picture is drawn into the pixel map (only the part it changed, when the map still holds the picture
 *      before: the same place, size, depth and colour table as last time), inside the clip rectangle, scaled by the nearest pixel when
 *      QuickTime asks for another size, at the map's depth: 32 and 16 bits as they are; 8, 4 and 2 bits
 *      through the map's own colour table (a 32,768-entry table of its nearest entries, made again when its
 *      seed changes), an 8-bit image with the same table index for index, other images with an ordered
 *      dither; 1 bit by an 8 x 8 Bayer threshold on the luminance (the SE's own screen).
 * The pixel map may be a screen's frame buffer (the SE's, slot 9's, the HDMI screen's: the screens'
 * machinery then carries the pixels on as for anything the Mac draws) or a GWorld in the Mac's RAM.
 * The 68k waits in the driver meanwhile, so the request's addresses stay good; a picture is decoded and
 * drawn in one or two milliseconds of a Pi core, where the 68k's own decoder would take tens.
 *
 * Nothing here is NuBus either: psmac_videocard.c gives the window, as for the stream.
 */
#include <stdint.h>
#include "support.h"
#include "tlsf.h"
#include "psmac_video.h"
#include "psmac_vcodec.h"

extern uint8_t *psmac_mac_span(uint32_t logical, uint32_t len);
extern int psmac_mac_copy(uint32_t logical, void *buf, uint32_t len, int to_mac);
extern const char *psmac_option(const char *key);

static inline uint64_t now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static inline uint32_t us_of(uint64_t ticks) { return (uint32_t)(ticks * 1000000u / hz()); }

#define DESC_MAX 2200u                        /* an ImageDescription with a 256-entry colour table: 86 + 8 + 2048 */
#define ROW_MAX  8192u                        /* the widest destination row in bytes (2048 pixels at 32 bits) */

static volatile struct a68v_fhdr *hdr;
static volatile struct a68v_freq *slots;
static volatile uint32_t reset_req;

struct sess {
    int used;
    const struct vc_codec *c;
    void *state;
    struct vc_picture pic;
    uint8_t *data; uint32_t data_cap;         /* the compressed picture, copied out of the Mac's memory */
    uint32_t frames, errors;
    /* the destination's colour table, for depths 1-8 */
    uint32_t ct_addr, ct_seed, ct_depth, ct_n;
    uint32_t ct[256];
    uint8_t *nearest;                         /* rgb555 -> the nearest entry of ct */
    int same_pal;                             /* ct is the image's own table: indices go across as they are */
    /* where the last picture was drawn: a picture drawn to the same place again need only redraw what changed,
       as QuickTime's own decompressors draw only the blocks a picture brings (the destination keeps the rest) */
    struct { uint32_t base, rb, depth, ctab, seed; int32_t x, y, w, h, cl, ct, cr, cb; } last;
    int last_ok;
    uint32_t partial, full, skipped;          /* draws of the changed part only, whole draws, pictures that changed nothing */
};
static struct sess sess[A68V_MAX_SESSIONS];
static struct { uint64_t t0, dec, draw; uint32_t ndec, ndraw; } st;
static uint64_t last_req;                     /* when the Mac last asked: core 1 keeps looking while a movie plays */
#define KEEP_LOOKING_MS 250u                  /* this long after a request core 1 does not sleep (wfe) between looks */
static uint8_t desc_buf[DESC_MAX];
static uint8_t row_buf[ROW_MAX];
static uint16_t xmap[2048];
static const uint8_t bayer[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 } };

static void sess_free(struct sess *s)
{
    if (s->state) tlsf_free(tlsf, s->state);
    if (s->pic.rgb) tlsf_free(tlsf, s->pic.rgb);
    if (s->pic.idx) tlsf_free(tlsf, s->pic.idx);
    if (s->data) tlsf_free(tlsf, s->data);
    if (s->nearest) tlsf_free(tlsf, s->nearest);
    uint8_t *p = (uint8_t *)s;
    for (uint32_t i = 0; i < sizeof *s; i++) p[i] = 0;
}

static uint32_t op_open(volatile struct a68v_freq *r)
{
    const struct vc_codec *c = vc_find(r->codec);
    uint32_t i;
    if (!c) return A68V_FE_CODEC;
    if (!r->width || !r->height || r->width > VC_MAX_W || r->height > VC_MAX_H) return A68V_FE_SIZE;
    for (i = 0; i < A68V_MAX_SESSIONS && sess[i].used; i++) { }
    if (i == A68V_MAX_SESSIONS) return A68V_FE_SESSION;
    struct sess *s = &sess[i];
    s->c = c;
    s->pic.w = r->width; s->pic.h = r->height; s->pic.depth = r->depth;
    uint32_t px = r->width * r->height;
    s->pic.rgb = tlsf_malloc(tlsf, px * 4);
    if (vc_wants_idx(c, r->depth)) s->pic.idx = tlsf_malloc(tlsf, px);
    if (c->state_size) s->state = tlsf_malloc(tlsf, c->state_size);
    if (!s->pic.rgb || (vc_wants_idx(c, r->depth) && !s->pic.idx) || (c->state_size && !s->state)) { sess_free(s); return A68V_FE_MEM; }
    for (uint32_t k = 0; k < px; k++) s->pic.rgb[k] = 0;
    if (s->pic.idx) for (uint32_t k = 0; k < px; k++) s->pic.idx[k] = 0;
    if (s->state) { uint8_t *p = s->state; for (uint32_t k = 0; k < c->state_size; k++) p[k] = 0; }
    uint32_t dl = r->desc_len > DESC_MAX ? DESC_MAX : r->desc_len;
    if (r->desc && dl >= 86 && !psmac_mac_copy(r->desc, desc_buf, dl, 0)) vc_palette_from_desc(&s->pic, desc_buf, dl);
    else vc_palette_from_desc(&s->pic, desc_buf, 0);
    if (c->open(s->state, &s->pic) != VC_OK) { sess_free(s); return A68V_FE_DEPTH; }
    s->used = 1;
    r->session = i + 1;
    hdr->sessions = hdr->sessions + 1;
    kprintf("[VIDEO] frames: session %u, %s %ux%u at depth %u%s\n", i + 1, c->name, r->width, r->height, r->depth,
            s->pic.npal ? ", with its colour table" : "");
    return A68V_FE_OK;
}
static struct sess *session_of(volatile struct a68v_freq *r)
{
    uint32_t i = r->session;
    return i >= 1 && i <= A68V_MAX_SESSIONS && sess[i - 1].used ? &sess[i - 1] : 0;
}

/* ---- the destination's colour table (depths 1 to 8) --------------------------------------------- */
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static int dest_table(struct sess *s, volatile struct a68v_freq *r)
{
    uint32_t depth = r->dst_depth;
    if (s->nearest && s->ct_addr == r->ctab && s->ct_seed == r->ctab_seed && s->ct_depth == depth) return 1;
    uint32_t n = 0;
    for (uint32_t i = 0; i < 256; i++) s->ct[i] = 0;
    if (r->ctab) {
        uint8_t h8[8];
        if (psmac_mac_copy(r->ctab, h8, 8, 0)) return 0;
        uint32_t size = rd16(h8 + 6) + 1, flags = rd16(h8 + 4);
        if (size > 256) size = 256;
        if (psmac_mac_copy(r->ctab + 8, desc_buf, size * 8, 0)) return 0;
        for (uint32_t i = 0; i < size; i++) {
            const uint8_t *e = desc_buf + i * 8;
            uint32_t slot = (flags & 0x8000) ? i : rd16(e);   /* a device's table: entries in order */
            if (slot < 256) s->ct[slot] = (rd16(e + 2) >> 8) << 16 | (rd16(e + 4) >> 8) << 8 | rd16(e + 6) >> 8;
            if (slot + 1 > n) n = slot + 1;
        }
    } else n = vc_standard_palette(s->ct, depth);
    if (n > (1u << depth)) n = 1u << depth;
    s->ct_n = n;
    if (!s->nearest && !(s->nearest = tlsf_malloc(tlsf, 32768))) return 0;
    for (uint32_t k = 0; k < 32768; k++) {
        int r5 = (int)((k >> 10) & 31) * 255 / 31, g5 = (int)((k >> 5) & 31) * 255 / 31, b5 = (int)(k & 31) * 255 / 31;
        uint32_t best = 0, bd = ~0u;
        for (uint32_t i = 0; i < n; i++) {
            int dr = r5 - (int)(s->ct[i] >> 16 & 255), dg = g5 - (int)(s->ct[i] >> 8 & 255), db = b5 - (int)(s->ct[i] & 255);
            uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
            if (d < bd) { bd = d; best = i; }
        }
        s->nearest[k] = (uint8_t)best;
    }
    s->same_pal = s->pic.idx && s->pic.npal && s->pic.npal <= n;
    for (uint32_t i = 0; s->same_pal && i < s->pic.npal; i++)
        if ((s->pic.pal[i] & 0xFFFFFF) != (s->ct[i] & 0xFFFFFF)) s->same_pal = 0;
    s->ct_addr = r->ctab; s->ct_seed = r->ctab_seed; s->ct_depth = depth;
    return 1;
}

/* ---- drawing ------------------------------------------------------------------------------------ */
static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
/* the picture into the request's pixel map; only [lx0, lx1) x [ly0, ly1) of the map when given (the changed part) */
static uint32_t draw(struct sess *s, volatile struct a68v_freq *r, int32_t lx0, int32_t ly0, int32_t lx1, int32_t ly1)
{
    uint32_t depth = r->dst_depth, rb = r->dst_rowbytes & 0x3FFF;
    int32_t x0 = r->dst_x, y0 = r->dst_y, w = r->dst_w > 0 ? r->dst_w : (int32_t)s->pic.w, h = r->dst_h > 0 ? r->dst_h : (int32_t)s->pic.h;
    int32_t cx0 = r->clip_left > x0 ? r->clip_left : x0, cy0 = r->clip_top > y0 ? r->clip_top : y0;
    int32_t cx1 = r->clip_right < x0 + w ? r->clip_right : x0 + w, cy1 = r->clip_bottom < y0 + h ? r->clip_bottom : y0 + h;
    if (lx0 > cx0) cx0 = lx0;
    if (ly0 > cy0) cy0 = ly0;
    if (lx1 < cx1) cx1 = lx1;
    if (ly1 < cy1) cy1 = ly1;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16 && depth != 32) return A68V_FE_DEPTH;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx0 >= cx1 || cy0 >= cy1) return A68V_FE_OK;
    if (w > 2048 || (uint32_t)cx1 * depth > rb * 8) return A68V_FE_SIZE;
    if (depth <= 8 && !dest_table(s, r)) return A68V_FE_ADDR;
    for (int32_t x = cx0; x < cx1; x++) xmap[x - cx0] = (uint16_t)((uint32_t)(x - x0) * s->pic.w / (uint32_t)w);
    int dither = !(r->flags & A68V_FF_NODITHER) && !s->same_pal;
    int amp = depth == 8 ? 40 : depth == 4 ? 64 : 96;
    uint32_t b0 = (uint32_t)cx0 * depth / 8, b1 = ((uint32_t)cx1 * depth + 7) / 8, nb = b1 - b0;
    for (int32_t y = cy0; y < cy1; y++) {
        uint32_t sy = (uint32_t)(y - y0) * s->pic.h / (uint32_t)h;
        const uint32_t *src = s->pic.rgb + sy * s->pic.w;
        const uint8_t *sidx = s->pic.idx ? s->pic.idx + sy * s->pic.w : 0;
        uint32_t addr = r->dst_base + (uint32_t)y * rb + b0;
        uint8_t *row = psmac_mac_span(addr, nb), *out;
        if (row) out = row - b0;
        else {
            if (nb > ROW_MAX) return A68V_FE_SIZE;
            if (depth < 8 && psmac_mac_copy(addr, row_buf, nb, 0)) return A68V_FE_ADDR;   /* the bytes shared with pixels outside */
            out = row_buf - b0;
        }
        const uint8_t *bay = bayer[y & 7];
        switch (depth) {
        case 32:
            for (int32_t x = cx0; x < cx1; x++) {
                uint32_t v = src[xmap[x - cx0]];
                uint8_t *o = out + x * 4;
                o[0] = 0; o[1] = (uint8_t)(v >> 16); o[2] = (uint8_t)(v >> 8); o[3] = (uint8_t)v;
            }
            break;
        case 16:
            for (int32_t x = cx0; x < cx1; x++) {
                uint32_t v = src[xmap[x - cx0]], c = (v >> 9 & 0x7C00) | (v >> 6 & 0x03E0) | (v >> 3 & 0x1F);
                out[x * 2] = (uint8_t)(c >> 8); out[x * 2 + 1] = (uint8_t)c;
            }
            break;
        case 1: {
            for (int32_t x = cx0; x < cx1; x++) {
                uint32_t v = src[xmap[x - cx0]];
                uint32_t l = ((v >> 16 & 255) * 77 + (v >> 8 & 255) * 151 + (v & 255) * 28) >> 8;
                uint8_t bit = (uint8_t)(0x80u >> (x & 7));
                if (l < (uint32_t)bay[x & 7] * 4 + 2) out[x >> 3] |= bit; else out[x >> 3] &= (uint8_t)~bit;
            }
            break;
        }
        default: {                                                     /* 2, 4, 8 through the table */
            uint32_t per = 8 / depth, mask = (1u << depth) - 1;
            for (int32_t x = cx0; x < cx1; x++) {
                uint32_t sx = xmap[x - cx0], idx;
                if (s->same_pal) idx = sidx[sx];
                else {
                    uint32_t v = src[sx];
                    int rr = v >> 16 & 255, gg = v >> 8 & 255, bb = v & 255;
                    if (dither) {
                        int off = ((int)bay[x & 7] - 32) * amp / 64;
                        rr = clamp8(rr + off); gg = clamp8(gg + off); bb = clamp8(bb + off);
                    }
                    idx = s->nearest[(uint32_t)(rr >> 3) << 10 | (uint32_t)(gg >> 3) << 5 | (uint32_t)(bb >> 3)];
                }
                if (depth == 8) { out[x] = (uint8_t)idx; continue; }
                uint32_t bi = (uint32_t)x / per, sh = 8 - depth - ((uint32_t)x % per) * depth;
                out[bi] = (uint8_t)((out[bi] & ~(mask << sh)) | (idx & mask) << sh);
            }
            break;
        }
        }
        if (!row && psmac_mac_copy(addr, row_buf, nb, 1)) return A68V_FE_ADDR;
    }
    __asm__ volatile("dmb ish" ::: "memory");
    return A68V_FE_OK;
}

static uint32_t op_decode(volatile struct a68v_freq *r)
{
    struct sess *s = session_of(r);
    if (!s) return A68V_FE_SESSION;
    uint32_t n = r->src_len, res = A68V_FE_OK;
    if (n > A68V_MAX_DATA) return A68V_FE_SIZE;
    if (n > s->data_cap) {
        uint32_t cap = (n + 0xFFFFu) & ~0xFFFFu;
        if (s->data) tlsf_free(tlsf, s->data);
        s->data_cap = 0;
        if (!(s->data = tlsf_malloc(tlsf, cap))) return A68V_FE_MEM;
        s->data_cap = cap;
    }
    uint64_t t0 = now();
    if (n && psmac_mac_copy(r->src, s->data, n, 0)) return A68V_FE_ADDR;
    vc_untouched(&s->pic);
    if (s->c->decode(s->state, &s->pic, s->data, n) != VC_OK) { res = A68V_FE_DATA; s->errors++; }
    uint64_t t1 = now();
    s->frames++;
    hdr->frames = hdr->frames + 1;
    st.dec += t1 - t0; st.ndec++;
    r->decode_us = us_of(t1 - t0);
    if (!(r->flags & A68V_FF_NODRAW)) {
        int32_t w = r->dst_w > 0 ? r->dst_w : (int32_t)s->pic.w, h = r->dst_h > 0 ? r->dst_h : (int32_t)s->pic.h;
        int same = s->last_ok && s->last.base == r->dst_base && s->last.rb == r->dst_rowbytes && s->last.depth == r->dst_depth &&
                   s->last.ctab == r->ctab && s->last.seed == r->ctab_seed && s->last.x == r->dst_x && s->last.y == r->dst_y &&
                   s->last.w == w && s->last.h == h && s->last.cl == r->clip_left && s->last.ct == r->clip_top &&
                   s->last.cr == r->clip_right && s->last.cb == r->clip_bottom;
        uint32_t d;
        if (!same) { d = draw(s, r, INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX); s->full++; }
        else if (s->pic.changed_left >= s->pic.changed_right) { d = A68V_FE_OK; s->skipped++; }
        else {                                 /* the changed part, widened to whole destination pixels when scaled */
            uint32_t pw = s->pic.w, ph = s->pic.h;
            int32_t lx0 = r->dst_x + (int32_t)(s->pic.changed_left * (uint32_t)w / pw), lx1 = r->dst_x + (int32_t)((s->pic.changed_right * (uint32_t)w + pw - 1) / pw);
            int32_t ly0 = r->dst_y + (int32_t)(s->pic.changed_top * (uint32_t)h / ph), ly1 = r->dst_y + (int32_t)((s->pic.changed_bottom * (uint32_t)h + ph - 1) / ph);
            d = draw(s, r, lx0, ly0, lx1, ly1);
            s->partial++;
        }
        s->last.base = r->dst_base; s->last.rb = r->dst_rowbytes; s->last.depth = r->dst_depth; s->last.ctab = r->ctab;
        s->last.seed = r->ctab_seed; s->last.x = r->dst_x; s->last.y = r->dst_y; s->last.w = w; s->last.h = h;
        s->last.cl = r->clip_left; s->last.ct = r->clip_top; s->last.cr = r->clip_right; s->last.cb = r->clip_bottom;
        s->last_ok = d == A68V_FE_OK;
        uint64_t t2 = now();
        r->draw_us = us_of(t2 - t1);
        st.draw += t2 - t1; st.ndraw++;
        hdr->drawn = hdr->drawn + 1;
        if (d) res = d;
    } else {
        r->draw_us = 0;
        s->last_ok = 0;                       /* the destination did not get this picture: the next one is drawn whole */
    }
    return res;
}

int psmac_vframes_poll(void)
{
    if (!hdr) return 0;
    if (__atomic_load_n(&reset_req, __ATOMIC_ACQUIRE)) {
        reset_req = 0;
        uint32_t open = 0;
        for (uint32_t i = 0; i < A68V_MAX_SESSIONS; i++)
            if (sess[i].used) { sess_free(&sess[i]); open++; }
        if (open) kprintf("[VIDEO] frames: the computer was reset: %u session%s closed\n", open, open == 1 ? "" : "s");
        hdr->sessions = 0;
        for (uint32_t i = 0; i < A68V_FSLOTS; i++) slots[i].done = slots[i].seq;
        return 1;
    }
    int did = 0;
    for (uint32_t i = 0; i < A68V_FSLOTS; i++) {
        volatile struct a68v_freq *r = &slots[i];
        uint32_t seq = r->seq;
        if (seq == r->done) continue;
        __asm__ volatile("dmb ish" ::: "memory");
        uint32_t res;
        if (!hdr->enabled) res = A68V_FE_OFF;
        else switch (r->op) {
        case A68V_OP_OPEN: res = op_open(r); break;
        case A68V_OP_DECODE: res = op_decode(r); break;
        case A68V_OP_DRAW: {
            struct sess *s = session_of(r);
            res = s ? draw(s, r, INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX) : A68V_FE_SESSION;
            if (s) s->last_ok = 0;
            break;
        }
        case A68V_OP_CLOSE: {
            struct sess *s = session_of(r);
            if (!s) { res = A68V_FE_SESSION; break; }
            kprintf("[VIDEO] frames: session %u closed after %u pictures (%u damaged): %u drawn whole, %u only where they changed, %u changed nothing\n",
                    r->session, s->frames, s->errors, s->full, s->partial, s->skipped);
            sess_free(s);
            hdr->sessions = hdr->sessions ? hdr->sessions - 1 : 0;
            res = A68V_FE_OK;
            break;
        }
        default: res = A68V_FE_OP;
        }
        if (res && res != A68V_FE_DATA) hdr->errors = hdr->errors + 1;
        r->result = res;
        __asm__ volatile("dmb ish" ::: "memory");
        r->done = seq;
        did = 1;
        last_req = now();
    }
    /* While a movie plays, core 1 does not wait for an event between looks: in the testbed its wfe slept until the
       next interrupt, and the 68k waited about 0.8 ms a picture for a decode of 0.3 ms (22 November 2026); on a Pi
       the timer's event stream would wake it within 0.43 ms. A movie is a few dozen requests a second, so core 1
       keeps looking for a quarter of a second after each and sleeps again once the movie stops or pauses. */
    if (hdr->sessions && now() - last_req < hz() / 1000u * KEEP_LOOKING_MS) did = 1;
    uint64_t t = now();
    if (t - st.t0 >= hz()) {
        hdr->decode_us = st.ndec ? us_of(st.dec / st.ndec) : 0;
        hdr->draw_us = st.ndraw ? us_of(st.draw / st.ndraw) : 0;
        st.dec = st.draw = 0; st.ndec = st.ndraw = 0; st.t0 = t;
    }
    return did;
}

void psmac_vframes_reset(void) { __atomic_store_n(&reset_req, 1, __ATOMIC_RELEASE); }

void psmac_vframes_init(uint8_t *win)
{
    hdr = (volatile struct a68v_fhdr *)(win + A68V_OFF_FRAMES);
    slots = (volatile struct a68v_freq *)(win + A68V_OFF_FSLOTS);
    for (uint32_t i = 0; i < sizeof(struct a68v_fhdr) / 4; i++) ((volatile uint32_t *)hdr)[i] = 0;
    for (uint32_t i = 0; i < A68V_FSLOTS * sizeof(struct a68v_freq) / 4; i++) ((volatile uint32_t *)slots)[i] = 0;
    hdr->slots = A68V_FSLOTS; hdr->slot_off = A68V_OFF_FSLOTS; hdr->slot_size = sizeof(struct a68v_freq);
    hdr->max_w = VC_MAX_W; hdr->max_h = VC_MAX_H;
    for (uint32_t i = 0; vc_fourccs[i] && i < 15; i++) hdr->codecs[i] = vc_fourccs[i];
    const char *o = psmac_option("mac.vdecode");
    hdr->enabled = !(o && o[0] == 'o' && o[1] == 'f');
    st.t0 = now();
    __asm__ volatile("dmb ish" ::: "memory");
    hdr->version = A68V_FVERSION; hdr->magic = A68V_FMAGIC;
    kprintf("[VIDEO] frames: Cinepak, Apple Video, Animation and Graphics decoded for QuickTime%s\n",
            hdr->enabled ? "" : " (switched off: mac.vdecode=off)");
}

void psmac_vframes_report(void)
{
    static uint32_t last;
    if (!hdr || hdr->frames == last) return;
    last = hdr->frames;
    kprintf("[PSMAC]   frames: %u decoded, %u drawn, %u refused; %u session%s open; decode %u us, draw %u us a picture\n",
            hdr->frames, hdr->drawn, hdr->errors, hdr->sessions, hdr->sessions == 1 ? "" : "s", hdr->decode_us, hdr->draw_us);
}
