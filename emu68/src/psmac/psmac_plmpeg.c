/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_plmpeg.c — where the video card's decoder, pl_mpeg, is compiled (docs/16-video-card.md).
 *
 * Backstory (22 November 2026, docs/08 item 98). The owner asked for YouTube on the Mac through a card
 * that exists only in the Pi. The helper on the owner's network turns any video into MPEG-1 with MP2
 * sound; the Mac's program fetches that over its own TCP/IP and copies the bytes into the card; the card
 * decodes them on core 1 with pl_mpeg (third_party/pl_mpeg: MIT, unmodified; README.md there says which
 * commit). This file is only where pl_mpeg's implementation is compiled, once, with what the kernel
 * needs; psmac_video.c calls it.
 *
 * The options, and why:
 *  - PLM_NO_STDIO: the kernel has no FILE; the card's bytes arrive through a load callback.
 *  - PLM_MALLOC, PLM_REALLOC, PLM_FREE: the kernel's TLSF heap, which has its own lock and is used from
 *    core 1 already (psmac_audio.c's stream buffer). pl_mpeg allocates when a stream starts (three
 *    pictures: 345 KB at 320 x 240, 2 MB at 1280 x 720) and grows its byte buffers by doubling when a
 *    packet needs more; nothing in the per-frame path allocates.
 *  - Floating point: pl_mpeg keeps times as doubles and its MP2 synthesis in floats, and it works out
 *    some tables with floats at start. Core 1 runs no JIT and owns its FP registers (psmac_mp3.c says
 *    the same for minimp3), and -ffixed-v19... are honoured here as everywhere.
 *  - Endianness: pl_mpeg reads its bit stream a byte at a time (plm_buffer_read) and keeps pictures as
 *    byte planes, so the big-endian kernel needs nothing special.
 *  - <math.h>: pl_mpeg needs none (checked: no sqrt, cos or pow in the file).
 */
#undef _FORTIFY_SOURCE                 /* the distribution compiler's default; the kernel has no __memcpy_chk */
#include <stdint.h>
#include <stddef.h>
#include "tlsf.h"                      /* not support.h: its __used__ collides with the C library's headers pl_mpeg includes */
extern void *tlsf;                     /* support.h: the kernel's heap */
#define PLM_NO_STDIO
#define PLM_MALLOC(sz)      tlsf_malloc(tlsf, (sz))
#define PLM_REALLOC(p, sz)  tlsf_realloc(tlsf, (p), (sz))
#define PLM_FREE(p)         tlsf_free(tlsf, (p))
#define PL_MPEG_IMPLEMENTATION
#include "third_party/pl_mpeg/pl_mpeg.h"

/* ==== MPEG-1 for QuickTime: the frame decoder's 'mpg1' and the import's MP2 (23 November 2026) =============
 *
 * Backstory (docs/08 item 113, GitHub issue #25). The card's stream channel (psmac_video.c) plays an MPEG-1
 * program stream a program pours into its ring; QuickTime wants something else: a movie whose samples are
 * the pictures, each handed to a decompressor component when QuickTime decides it is time for it. The
 * Aurora68 Video Decoder extension's import component ('eat ', mac/vdec/import.c) makes such a movie out of
 * an MPEG-1 file without copying it: every sample is a byte range of the file itself, from the picture's
 * start (its sequence or GOP header, if any, else its picture start code) to the next picture's start. Its
 * fifth 'imdc' component hands each sample to the card, and this is where it is decoded. pl_mpeg's video
 * decoder is used one picture at a time, below its own plm_video_decode (which finds pictures in a stream
 * and reorders them), through the functions this one compilation of pl_mpeg can see: the sequence header is
 * decoded once, from the ImageDescription, and then each picture with plm_video_decode_picture.
 *
 * In a program stream a sample starts in the middle of a packet and has the packets' headers and the sound's
 * packets inside it. m1_demux takes them out: the bytes up to the first system start code (00 00 01 and a
 * code from B9 up, which the video itself never contains: its start codes end at B8) are the rest of the
 * packet the sample started in; from there on the packets are read by their lengths and only the video
 * stream's payload kept.
 *
 * B-pictures. QuickTime 2.5 has one order of samples, the order they are shown in; MPEG-1 sends a
 * reference picture (I or P) before the B-pictures that are shown before it. The import therefore makes
 * the samples in display order and gives the first B-picture after a reference the reference's bytes in
 * front of its own (mac/vdec/mpgindex.h, mi_order); the reference's own sample comes later, when it is
 * shown. So a sample holds one picture or two (a reference and a B). Here a reference picture is decoded
 * unless it is the one decoded last (recognised by a hash of its bytes): then it is simply shown, the newest
 * reference. A B-picture is decoded against the two references and shown; a sample of a reference and a B
 * shows the B. After a seek QuickTime decodes from the preceding sync sample (an I-picture's own sample) on,
 * so the references are the right ones whenever QuickTime plays and seeks as it does for any temporally
 * compressed movie.
 *
 * The picture shown is converted to 0x00RRGGBB (BT.601, the same coefficients as the stream channel's) into
 * the session's picture; psmac_vframes.c draws it at the destination's depth as for the other formats.
 */
#include "psmac_vcodec.h"

struct mp1 {
    plm_buffer_t *buf;
    plm_video_t *vid;
    uint8_t *es; uint32_t es_cap;             /* a sample's video bytes, out of its packets */
    uint32_t kind, sid;                       /* 1 a video stream, 2 a program stream; its video stream's id (0xE0...) */
    uint32_t h_new; int have_new;             /* the hash of the reference decoded last */
    const plm_frame_t *shown;
    uint32_t decoded, refs_shown_again;
};

static uint32_t fnv(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}
static int32_t m1_ty[256], m1_rv[256], m1_gu[256], m1_gv[256], m1_bu[256];   /* BT.601, x 256 */
static int m1_tables;
static void m1_make_tables(void)
{
    for (int i = 0; i < 256; i++) {
        m1_ty[i] = 298 * (i - 16) + 128;
        m1_rv[i] = 409 * (i - 128); m1_gv[i] = -208 * (i - 128);
        m1_gu[i] = -100 * (i - 128); m1_bu[i] = 516 * (i - 128);
    }
    m1_tables = 1;
}
static inline uint32_t m1_clamp(int32_t v) { return v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v; }
static inline int m1_syscode(const uint8_t *s) { return s[0] == 0 && s[1] == 0 && s[2] == 1 && s[3] >= 0xB9; }

/* the video bytes of a program stream's sample (it starts inside a video packet's payload) */
static uint32_t m1_demux(const uint8_t *s, uint32_t n, uint8_t *out, uint32_t sid)
{
    uint32_t i = 0, o = 0;
    while (i < n && !(i + 3 < n && m1_syscode(s + i)))   /* the rest of the packet the sample started in */
        out[o++] = s[i++];
    while (i + 4 <= n) {
        if (!m1_syscode(s + i)) { i++; continue; }        /* lost: the next system start code */
        uint32_t code = s[i + 3];
        if (code == 0xB9) { i += 4; continue; }
        if (code == 0xBA) {
            if (i + 5 > n) break;
            if ((s[i + 4] & 0xC0) == 0x40) {               /* an MPEG-2 pack */
                if (i + 14 > n) break;
                i += 14 + (s[i + 13] & 7u);
            } else i += 12;
            continue;
        }
        if (i + 6 > n) break;
        uint32_t len = (uint32_t)s[i + 4] << 8 | s[i + 5], p = i + 6, end = i + 6 + len;
        if (end > n) end = n;
        if (code == sid) {
            uint32_t k = 0;
            while (p < end && s[p] == 0xFF && k++ < 16) p++;                  /* stuffing */
            if (p < end && (s[p] & 0xC0) == 0x80) {                           /* an MPEG-2 PES header */
                p = p + 3 <= end ? p + 3 + s[p + 2] : end;
            } else {
                if (p < end && (s[p] & 0xC0) == 0x40) p += 2;                 /* the STD buffer's size */
                if (p < end) {
                    if ((s[p] & 0xF0) == 0x20) p += 5;                        /* PTS */
                    else if ((s[p] & 0xF0) == 0x30) p += 10;                  /* PTS and DTS */
                    else p += 1;                                              /* 0x0F: none */
                }
            }
            while (p < end) out[o++] = s[p++];
        }
        i += 6 + len;
    }
    return o;
}

static int m1_seq_header(struct mp1 *m, const uint8_t *seq, uint32_t len)
{
    static const uint8_t zeros[160];
    if (m->vid->has_sequence_header) return 1;           /* decoded once: pl_mpeg would allocate its pictures again */
    m->buf->bit_index = 0; m->buf->length = 0; m->buf->total_size = 0;
    plm_buffer_write(m->buf, (uint8_t *)seq, len);
    plm_buffer_write(m->buf, (uint8_t *)zeros, sizeof zeros);   /* pl_mpeg wants room for both matrices before it looks */
    m->vid->start_code = -1;
    if (!plm_video_has_header(m->vid)) return 0;
    /* the three pictures black (Y 16, Cb and Cr 128) until the first is decoded */
    size_t luma = (size_t)m->vid->luma_width * m->vid->luma_height, chroma = (size_t)m->vid->chroma_width * m->vid->chroma_height;
    for (int f = 0; f < 3; f++) {
        uint8_t *b = m->vid->frames_data + f * (luma + 2 * chroma);
        memset(b, 16, luma);
        memset(b + luma, 128, 2 * chroma);
    }
    return 1;
}

static int m1_open(void *state, struct vc_picture *p)
{
    struct mp1 *m = state;
    if (!m1_tables) m1_make_tables();
    m->kind = 2; m->sid = 0xE0;
    const uint8_t *seq = 0;
    uint32_t seql = 0;
    if (p->desc && p->desc_len >= 86 + 12) {           /* the import's extension atom: 'mpgh' */
        const uint8_t *a = p->desc + 86;
        uint32_t asz = (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
        if (a[4] == 'm' && a[5] == 'p' && a[6] == 'g' && a[7] == 'h' && asz >= 12 && 86 + asz <= p->desc_len) {
            m->kind = a[9]; m->sid = a[10] ? a[10] : 0xE0;
            seq = a + 12; seql = asz - 12;
        }
    }
    m->buf = plm_buffer_create_with_capacity(64u << 10);
    if (!m->buf) return VC_E_SIZE;
    m->vid = plm_video_create_with_buffer(m->buf, 0);
    if (seql >= 12 && seq[3] == 0xB3) {
        if (!m1_seq_header(m, seq, seql)) return VC_E_DATA;
        if ((uint32_t)m->vid->width != p->w || (uint32_t)m->vid->height != p->h) return VC_E_SIZE;
    }
    return VC_OK;
}
static void m1_close(void *state)
{
    struct mp1 *m = state;
    if (m->vid) plm_video_destroy(m->vid);
    if (m->buf) plm_buffer_destroy(m->buf);
    if (m->es) PLM_FREE(m->es);
    m->vid = 0; m->buf = 0; m->es = 0;
}
static void m1_picture(struct mp1 *m, const uint8_t *pic, uint32_t n)
{
    static const uint8_t tail[8] = { 0, 0, 1, 0xB7, 0, 0, 0, 0 };   /* a sequence end code: the last slice ends there */
    m->buf->bit_index = 0; m->buf->length = 0; m->buf->total_size = 0;
    plm_buffer_write(m->buf, (uint8_t *)pic + 4, n - 4);          /* after the picture start code, where pl_mpeg starts */
    plm_buffer_write(m->buf, (uint8_t *)tail, sizeof tail);
    m->vid->start_code = PLM_START_PICTURE;
    plm_video_decode_picture(m->vid);
    m->decoded++;
}
static int m1_start(const uint8_t *s, int any)   /* a start code that ends a picture (any: or a sequence header begins) */
{
    if (!(s[0] == 0 && s[1] == 0 && s[2] == 1)) return 0;
    return s[3] == 0x00 || s[3] == 0xB3 || (any && (s[3] == 0xB8 || s[3] == 0xB7));
}
static int m1_decode(void *state, struct vc_picture *p, const uint8_t *data, uint32_t len)
{
    struct mp1 *m = state;
    const uint8_t *es = data;
    uint32_t n = len;
    if (m->kind == 2) {
        if (len > m->es_cap) {
            if (m->es) PLM_FREE(m->es);
            m->es_cap = (len + 0xFFFFu) & ~0xFFFFu;
            if (!(m->es = PLM_MALLOC(m->es_cap))) { m->es_cap = 0; return VC_E_SIZE; }
        }
        n = m1_demux(data, len, m->es, m->sid);
        es = m->es;
    }
    /* the pictures in the sample, each from its picture start code to the next start code outside a picture */
    const plm_frame_t *shown = 0;
    int bad = 0;
    uint32_t i = 0, nn = 0;
    for (;;) {
        while (i + 3 < n && !m1_start(es + i, 0)) i++;
        if (i + 3 >= n) break;
        uint32_t e = i + 4;
        while (e + 3 < n && !m1_start(es + e, 1)) e++;
        if (e + 3 >= n) e = n;
        if (es[i + 3] == 0xB3) {                          /* the first sequence header, if the description had none */
            if (!m1_seq_header(m, es + i, e - i)) return VC_E_DATA;
            if ((uint32_t)m->vid->width != p->w || (uint32_t)m->vid->height != p->h) return VC_E_SIZE;
            i = e;
            continue;
        }
        if (e - i < 8 || !m->vid->has_sequence_header) { bad = 1; i = e; continue; }
        uint32_t type = (es[i + 5] >> 3) & 7;
        if (type == 1 || type == 2) {
            uint32_t h = fnv(es + i, e - i);
            int again = m->have_new && h == m->h_new;
            if (!again) { m1_picture(m, es + i, e - i); m->h_new = h; m->have_new = 1; }
            else m->refs_shown_again++;
            if (nn < sizeof p->note - 3) p->note[nn++] = type == 1 ? (again ? 'i' : 'I') : (again ? 'p' : 'P');
            shown = &m->vid->frame_backward;
        } else if (type == 3) {
            m1_picture(m, es + i, e - i);
            if (nn < sizeof p->note - 3) p->note[nn++] = 'B';
            shown = &m->vid->frame_current;
        }
        i = e;
    }
    p->note[nn] = 0;
    if (!shown) return bad ? VC_E_DATA : VC_OK;          /* nothing to show: the picture before stays */
    m->shown = shown;
    uint32_t w = p->w, h = p->h;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *yr = shown->y.data + y * shown->y.width;
        const uint8_t *cb = shown->cb.data + (y >> 1) * shown->cb.width, *cr = shown->cr.data + (y >> 1) * shown->cr.width;
        uint32_t *o = p->rgb + y * w;
        for (uint32_t x = 0; x < w; x++) {
            int32_t c = m1_ty[yr[x]], u = cb[x >> 1], v = cr[x >> 1];
            o[x] = m1_clamp((c + m1_rv[v]) >> 8) << 16 | m1_clamp((c + m1_gu[u] + m1_gv[v]) >> 8) << 8 | m1_clamp((c + m1_bu[u]) >> 8);
        }
    }
    vc_touch(p, 0, 0, w, h);
    return bad ? VC_E_DATA : VC_OK;
}
int vc_mpg1_planes(void *state, const uint8_t **y, const uint8_t **cb, const uint8_t **cr, uint32_t *ystride, uint32_t *cstride)
{
    struct mp1 *m = state;
    if (!m->shown) return 0;
    *y = m->shown->y.data; *cb = m->shown->cb.data; *cr = m->shown->cr.data;
    *ystride = m->shown->y.width; *cstride = m->shown->cb.width;
    return 1;
}
const struct vc_codec vc_mpg1 = { VC_MPG1, "MPEG-1", sizeof(struct mp1), 0, m1_open, m1_decode, m1_close };

/* ---- MP2 for the import's sound track ---------------------------------------------------------------- */
struct mp2 {
    plm_buffer_t *buf;
    plm_audio_t *aud;
    uint8_t pcm[PLM_AUDIO_SAMPLES_PER_FRAME * 4];   /* one frame as 16-bit big-endian stereo */
    uint32_t pcm_n, pcm_at;                         /* its bytes, and those already given out */
};
void *vc_mp2_open(void)
{
    struct mp2 *s = PLM_MALLOC(sizeof *s);
    if (!s) return 0;
    memset(s, 0, sizeof *s);
    s->buf = plm_buffer_create_with_capacity(16u << 10);
    if (s->buf) s->aud = plm_audio_create_with_buffer(s->buf, 1);
    if (!s->aud) {
        if (s->buf) plm_buffer_destroy(s->buf);
        PLM_FREE(s);
        return 0;
    }
    return s;
}
void vc_mp2_close(void *h)
{
    struct mp2 *s = h;
    if (!s) return;
    plm_audio_destroy(s->aud);                      /* and its buffer */
    PLM_FREE(s);
}
uint32_t vc_mp2_decode(void *h, const uint8_t *in, uint32_t n, uint8_t *out, uint32_t room, uint32_t *more, uint32_t *rate)
{
    struct mp2 *s = h;
    uint32_t o = 0;
    if (n) plm_buffer_write(s->buf, (uint8_t *)in, n);
    room &= ~3u;
    for (;;) {
        if (s->pcm_at < s->pcm_n) {
            uint32_t k = s->pcm_n - s->pcm_at;
            if (k > room - o) k = room - o;
            memcpy(out + o, s->pcm + s->pcm_at, k);
            o += k; s->pcm_at += k;
            if (s->pcm_at < s->pcm_n) break;        /* no more room */
        }
        plm_samples_t *sm = plm_audio_decode(s->aud);
        if (!sm) break;
        uint8_t *q = s->pcm;
        for (uint32_t i = 0; i < sm->count * 2; i++) {
            float f = sm->interleaved[i] * 32767.0f;
            int v = f > 32767.0f ? 32767 : f < -32768.0f ? -32768 : (int)f;
            *q++ = (uint8_t)((uint32_t)v >> 8);
            *q++ = (uint8_t)v;
        }
        s->pcm_n = (uint32_t)(q - s->pcm); s->pcm_at = 0;
    }
    *more = s->pcm_at < s->pcm_n;
    *rate = (uint32_t)plm_audio_get_samplerate(s->aud);
    return o;
}
