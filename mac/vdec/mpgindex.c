/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/vdec/mpgindex.c - an MPEG-1 file indexed for QuickTime (mpgindex.h; docs/16-video-card.md, GitHub
 * issue #25). Written on 23 November 2026 from ISO/IEC 11172-1 (the system layer: packs, the system header,
 * packets) and 11172-2 (the video's start codes, the sequence and picture headers), as the standard and the
 * period's books describe them; no other decoder's code. Portable C: the import component on the 68k and the
 * host test (tools/mpg1_test.c) compile this one file.
 *
 * The 68k reads a film in 32 KB pieces and hands each to mi_feed, so everything here is a state machine that
 * may stop between any two bytes: in the middle of a pack header, a packet's length, a PES header's time
 * stamps, a start code. The video scanner looks at every byte of the video's payload; the common case (a byte
 * above 1, not after 00 00 01) costs a shift and two compares, so a film of 10 MB is indexed in about a second
 * by an Emu68 68k on a Pi.
 */
#include "mpgindex.h"

enum { ST_SYNC, ST_HDR, ST_PESH, ST_PAY, ST_SKIP };
enum { PH_FIRST, PH_AFTER_STD, PH_M2FLAGS, PH_M2LEN, PH_SKIP };

void mi_init(struct mi *m, mi_pic_fn on_pic, mi_aud_fn on_aud, void *user)
{
    uint8_t *b = (uint8_t *)m;
    for (uint32_t i = 0; i < sizeof *m; i++) b[i] = 0;
    m->on_pic = on_pic; m->on_aud = on_aud; m->user = user;
    m->sr = m->vsr = m->asr = 0xFFFFFFFFu;
}

void mi_rate(uint32_t code, uint32_t *num, uint32_t *den)
{
    static const uint16_t n[9] = { 0, 24000, 24, 25, 30000, 30, 50, 60000, 60 };
    static const uint16_t d[9] = { 1, 1001, 1, 1, 1001, 1, 1, 1001, 1 };
    if (code > 8) code = 0;
    *num = n[code]; *den = d[code];
}

/* ---- the video scanner ----------------------------------------------------------------------- */
static void run_begin(struct mi *m, uint32_t off)
{
    for (int k = 0; k < 3; k++) { m->run_i[k] = m->run_i[k + 1]; m->run_off[k] = m->run_off[k + 1]; }
    m->run_i[3] = m->vn; m->run_off[3] = off;
}
/* the file offset of the video's idx-th byte (one of the last few: a start code is four bytes, and a packet
   boundary may fall between any two of them) */
static uint32_t es_off(const struct mi *m, uint32_t idx)
{
    for (int k = 3; k > 0; k--)
        if (m->run_i[k] <= idx) return m->run_off[k] + (idx - m->run_i[k]);
    return m->run_off[0] + (idx - m->run_i[0]);
}
static void seq_done(struct mi *m)
{
    const uint8_t *s = m->seq.bytes;
    m->seq.w = (uint32_t)s[4] << 4 | s[5] >> 4;
    m->seq.h = (uint32_t)(s[5] & 15) << 8 | s[6];
    m->seq.aspect = s[7] >> 4; m->seq.rate_code = s[7] & 15;
    m->seq.bitrate = (uint32_t)s[8] << 10 | (uint32_t)s[9] << 2 | s[10] >> 6;
    m->seq.have = 1;
}
static void es_slow(struct mi *m, uint8_t b)
{
    uint32_t idx = m->vn++;
    if (m->seq_need) {                      /* the first sequence header, captured whole for the ImageDescription */
        m->seq.bytes[m->seq.len++] = b;
        if (!--m->seq_need) {
            const uint8_t *s = m->seq.bytes;
            if (m->seq.len == 12) {         /* load_intra_quantiser_matrix, else load_non_intra's flag */
                if (s[11] & 2) m->seq_need = 64;
                else if (s[11] & 1) m->seq_need = 64;
            } else if (m->seq.len == 76 && (s[11] & 2) && (s[75] & 1)) m->seq_need = 64;
            if (!m->seq_need) seq_done(m);
        }
    }
    if (m->pic_need) {                      /* the picture header: temporal_reference (10 bits), picture_coding_type (3) */
        if (m->pic_need == 2) m->pic_b0 = b;
        else {
            uint32_t t = (uint32_t)(b >> 3) & 7, k = t == 1 ? MI_I : t == 2 ? MI_P : t == 3 ? MI_B : MI_OTHER;
            m->npics++;
            if (k == MI_I) m->ni++; else if (k == MI_P) m->np++; else if (k == MI_B) m->nb++; else m->nother++;
            if (m->on_pic) m->on_pic(m->user, m->pic_off, k);
        }
        m->pic_need--;
    }
    m->vsr = m->vsr << 8 | b;
    if ((m->vsr & 0xFFFFFF00u) != 0x100u || idx < 3) return;
    uint32_t start = es_off(m, idx - 3);
    switch (b) {
    case 0xB3:                              /* sequence header */
        if (!m->seq.have && !m->seq_need) {
            m->seq.bytes[0] = 0; m->seq.bytes[1] = 0; m->seq.bytes[2] = 1; m->seq.bytes[3] = 0xB3;
            m->seq.len = 4; m->seq_need = 8;
        }
        /* fall through */
    case 0xB8:                              /* group of pictures */
        if (!m->have_unit) { m->unit = start; m->have_unit = 1; }
        break;
    case 0x00:                              /* picture */
        m->pic_off = m->have_unit ? m->unit : start;
        m->have_unit = 0;
        m->pic_need = 2;
        break;
    }
}
static void video_run(struct mi *m, const uint8_t *p, uint32_t n, uint32_t off)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (b > 1 && !m->seq_need && !m->pic_need && (m->vsr & 0xFFFFFFu) != 1u) {   /* cannot end or begin a start code */
            m->vsr = m->vsr << 8 | b;
            m->vn++;
            continue;
        }
        es_slow(m, b);
    }
    m->video_end = off + n;
}

/* ---- the audio --------------------------------------------------------------------------------- */
static void audio_run(struct mi *m, const uint8_t *p, uint32_t n)
{
    static const uint16_t kbps2[15] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 };
    static const uint16_t rates[3] = { 44100, 48000, 32000 };
    for (uint32_t i = 0; i < n && !m->aud.have; i++) {
        m->asr = m->asr << 8 | p[i];
        uint32_t h = m->asr, ver = h >> 19 & 3, lay = h >> 17 & 3, br = h >> 12 & 15, sr = h >> 10 & 3;
        if ((h >> 21) == 0x7FF && ver == 3 && lay && br && br != 15 && sr != 3) {
            m->aud.have = 1; m->aud.layer = 4 - lay; m->aud.rate = rates[sr];
            m->aud.channels = (h >> 6 & 3) == 3 ? 1 : 2;
            m->aud.kbps = m->aud.layer == 2 ? kbps2[br] : 0;
        }
    }
    m->audio_bytes += n;
    if (m->on_aud) m->on_aud(m->user, p, n);
}

/* ---- the system layer ------------------------------------------------------------------------- */
static void start_header(struct mi *m, uint8_t code)
{
    m->hdr[0] = 0; m->hdr[1] = 0; m->hdr[2] = 1; m->hdr[3] = code;
    m->hn = 4;
    m->need = code == 0xBA ? 12 : code == 0xB9 ? 4 : 6;   /* a pack (MPEG-1: 12 bytes), the end code, a packet's code and length */
    m->st = m->need == 4 ? ST_SYNC : ST_HDR;
    if (m->st == ST_SYNC) m->sr = 0xFFFFFFFFu;
}
static void header_done(struct mi *m)
{
    uint8_t code = m->hdr[3];
    m->sr = 0xFFFFFFFFu;
    if (code == 0xBA) { m->st = ST_SYNC; return; }
    m->left = (uint32_t)m->hdr[4] << 8 | m->hdr[5];
    m->packets++;
    m->cur = 0;
    if (code >= 0xE0 && code <= 0xEF && (!m->vid_id || m->vid_id == code)) { m->vid_id = code; m->cur = 1; m->vpackets++; }
    else if (code >= 0xC0 && code <= 0xDF && (!m->aud_id || m->aud_id == code)) { m->aud_id = code; m->cur = 2; m->apackets++; }
    if (!m->left) { m->st = ST_SYNC; return; }
    if (m->cur) { m->st = ST_PESH; m->ph = PH_FIRST; m->skip = 0; }
    else m->st = ST_SKIP;
}
static void payload_begins(struct mi *m)
{
    m->st = m->left ? ST_PAY : ST_SYNC;
    if (m->st == ST_PAY && m->cur == 1) run_begin(m, m->pos);
}

void mi_feed(struct mi *m, const uint8_t *p, uint32_t n)
{
    if (m->kind == MI_UNKNOWN) {
        if (m->pos == 0 && n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 1 && p[3] == 0xB3) {
            m->kind = MI_ES;
            run_begin(m, 0);
        } else m->kind = MI_PS;
    }
    if (m->kind == MI_ES) {
        if (m->pos + n > MI_MAX_FILE) n = m->pos < MI_MAX_FILE ? MI_MAX_FILE - m->pos : 0;
        video_run(m, p, n, m->pos);
        m->pos += n;
        return;
    }
    while (n) {
        if (m->pos >= MI_MAX_FILE) return;
        uint32_t k;
        switch (m->st) {
        case ST_SYNC: {
            uint8_t b = *p++; n--; m->pos++;
            m->sr = m->sr << 8 | b;
            if ((m->sr & 0xFFFFFF00u) == 0x100u && b >= 0xB9) start_header(m, b);
            break;
        }
        case ST_HDR: {
            uint8_t b = *p++; n--; m->pos++;
            if (m->hn < sizeof m->hdr) m->hdr[m->hn] = b;
            m->hn++;
            if (m->hdr[3] == 0xBA && m->hn == 5 && (b & 0xC0) == 0x40) m->need = 14;      /* an MPEG-2 pack */
            if (m->hdr[3] == 0xBA && m->need >= 14 && m->hn == 14) m->need = 14 + (b & 7); /* and its stuffing */
            if (m->hn == m->need) header_done(m);
            break;
        }
        case ST_PESH: {
            if (!m->left) { m->st = ST_SYNC; break; }
            uint8_t b = *p++; n--; m->pos++; m->left--;
            switch (m->ph) {
            case PH_FIRST:
                if (b == 0xFF) break;                                    /* stuffing */
                if ((b & 0xC0) == 0x40) { m->ph = PH_AFTER_STD; m->skip = 1; break; }   /* STD buffer: one more byte */
                if ((b & 0xC0) == 0x80) { m->ph = PH_M2FLAGS; break; }     /* an MPEG-2 PES header */
                /* fall through */
            case PH_AFTER_STD:
                if (m->skip) { m->skip--; break; }
                if ((b & 0xF0) == 0x20) { m->ph = PH_SKIP; m->skip = 4; }  /* PTS */
                else if ((b & 0xF0) == 0x30) { m->ph = PH_SKIP; m->skip = 9; }   /* PTS and DTS */
                else { m->ph = PH_SKIP; m->skip = 0; }                    /* 0x0F: no time stamps */
                if (!m->skip) payload_begins(m);
                break;
            case PH_M2FLAGS: m->ph = PH_M2LEN; break;
            case PH_M2LEN: m->ph = PH_SKIP; m->skip = b; if (!b) payload_begins(m); break;
            case PH_SKIP:
                if (m->skip && !--m->skip) payload_begins(m);
                break;
            }
            if (m->st == ST_PESH && !m->left) m->st = ST_SYNC;
            break;
        }
        case ST_PAY:
            k = m->left < n ? m->left : n;
            if (m->pos + k > MI_MAX_FILE) k = MI_MAX_FILE - m->pos;
            if (m->cur == 1) video_run(m, p, k, m->pos); else audio_run(m, p, k);
            p += k; n -= k; m->pos += k; m->left -= k;
            if (!m->left) { m->st = ST_SYNC; m->sr = 0xFFFFFFFFu; }
            break;
        case ST_SKIP:
            k = m->left < n ? m->left : n;
            p += k; n -= k; m->pos += k; m->left -= k;
            if (!m->left) { m->st = ST_SYNC; m->sr = 0xFFFFFFFFu; }
            break;
        }
    }
}

/* ---- display order ------------------------------------------------------------------------------ */
uint32_t mi_order(const uint32_t *pics, uint32_t n, uint32_t end, mi_emit_fn emit, void *user)
{
    uint32_t out = 0;
    int32_t pending = -1;                   /* the last reference picture, not yet shown */
    int carried = 0;                        /* its bytes have gone out in front of a B-picture */
#define OFF(i)  (pics[i] >> 2)
#define TYPE(i) (pics[i] & 3)
#define NEXT(i) ((uint32_t)(i) + 1 < n ? OFF((uint32_t)(i) + 1) : end)
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t = TYPE(i);
        if (t == MI_I || t == MI_P) {
            if (pending >= 0) { emit(user, OFF(pending), NEXT(pending) - OFF(pending), TYPE(pending) == MI_I ? MI_SYNC : 0); out++; }
            pending = (int32_t)i; carried = 0;
        } else if (t == MI_B && pending >= 0 && i == (uint32_t)pending + 1 && !carried) {
            emit(user, OFF(pending), NEXT(i) - OFF(pending), 0); out++;
            carried = 1;
        } else { emit(user, OFF(i), NEXT(i) - OFF(i), t == MI_B ? MI_BONLY : 0); out++; }
    }
    if (pending >= 0) { emit(user, OFF(pending), NEXT(pending) - OFF(pending), TYPE(pending) == MI_I ? MI_SYNC : 0); out++; }
#undef OFF
#undef TYPE
#undef NEXT
    return out;
}

/* ---- the ImageDescription ------------------------------------------------------------------------ */
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
uint32_t mi_image_description(uint8_t *d, uint32_t cap, const struct mi *m)
{
    uint32_t sl = m->seq.have ? m->seq.len : 0, size = 86 + 12 + sl;
    static const char name[] = "MPEG-1 (Aurora68)";
    if (cap < size) return 0;
    for (uint32_t i = 0; i < size; i++) d[i] = 0;
    be32(d, size);
    d[4] = 'm'; d[5] = 'p'; d[6] = 'g'; d[7] = '1';               /* cType */
    be16(d + 16, 1); be16(d + 18, 1);                              /* version, revisionLevel */
    d[20] = 'A'; d[21] = '6'; d[22] = '8'; d[23] = 'v';           /* vendor */
    be32(d + 28, 0x200);                                           /* spatialQuality: codecNormalQuality */
    be16(d + 32, m->seq.w); be16(d + 34, m->seq.h);
    be32(d + 36, 72u << 16); be32(d + 40, 72u << 16);              /* 72 dpi */
    be16(d + 48, 1);                                               /* frameCount */
    d[50] = (uint8_t)(sizeof name - 1);
    for (uint32_t i = 0; i + 1 < sizeof name; i++) d[51 + i] = (uint8_t)name[i];
    be16(d + 82, 24); be16(d + 84, 0xFFFF);                        /* millions of colours, no colour table */
    be32(d + 86, 12 + sl);                                         /* the card's extension: 'mpgh' */
    d[90] = 'm'; d[91] = 'p'; d[92] = 'g'; d[93] = 'h';
    d[94] = 1; d[95] = (uint8_t)m->kind; d[96] = (uint8_t)m->vid_id; d[97] = 0;
    for (uint32_t i = 0; i < sl; i++) d[98 + i] = m->seq.bytes[i];
    return size;
}

/* ---- catching up after a jump --------------------------------------------------------------------- */
struct cu { uint32_t k, at, sync, n, max; struct mi_sample *out; int pass; };
static void cu_emit(void *u, uint32_t off, uint32_t size, int flags)
{
    struct cu *c = u;
    uint32_t i = c->at++;
    if (c->pass == 0) { if (i <= c->k && (flags & MI_SYNC)) c->sync = i; return; }
    if (i < c->sync || i >= c->k || (flags & MI_BONLY) || c->n == c->max) return;
    c->out[c->n].index = i; c->out[c->n].off = off; c->out[c->n].size = size; c->n++;
}
uint32_t mi_catchup(const uint32_t *pics, uint32_t n, uint32_t end, uint32_t k, struct mi_sample *out, uint32_t max)
{
    struct cu c = { k, 0, 0xFFFFFFFFu, 0, max, out, 0 };
    mi_order(pics, n, end, cu_emit, &c);
    if (c.sync == 0xFFFFFFFFu || c.sync == k) return 0;
    c.at = 0; c.pass = 1;
    mi_order(pics, n, end, cu_emit, &c);
    return c.n;
}

struct sa { uint32_t k, at, off, size; int flags, found; };
static void sa_emit(void *u, uint32_t off, uint32_t size, int flags)
{
    struct sa *a = u;
    if (a->at++ == a->k) { a->off = off; a->size = size; a->flags = flags; a->found = 1; }
}
int mi_sample_at(const uint32_t *pics, uint32_t n, uint32_t end, uint32_t k, uint32_t *off, uint32_t *size, int *flags)
{
    struct sa a = { k, 0, 0, 0, 0, 0 };
    mi_order(pics, n, end, sa_emit, &a);
    *off = a.off; *size = a.size; *flags = a.flags;
    return a.found;
}

/* ---- the decompressor's index ------------------------------------------------------------------- */
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint32_t mi_index_atom(uint8_t *d, uint32_t cap, const uint8_t *fsspec, uint32_t file_size, uint32_t end, const uint32_t *pics, uint32_t n)
{
    uint32_t size = MI_INDEX_HDR + 4 * n;
    if (n > MI_INDEX_MAX || cap < size) return 0;
    be32(d, size);
    d[4] = 'm'; d[5] = 'p'; d[6] = 'g'; d[7] = 'x';
    for (uint32_t i = 0; i < 70; i++) d[8 + i] = fsspec[i];
    d[78] = d[79] = 0;
    be32(d + 80, file_size); be32(d + 84, end); be32(d + 88, n);
    for (uint32_t i = 0; i < n; i++) be32(d + MI_INDEX_HDR + 4 * i, pics[i]);
    return size;
}
int mi_index_find(const uint8_t *desc, uint32_t len, const uint8_t **fsspec, uint32_t *file_size, uint32_t *end,
                  const uint8_t **pics, uint32_t *n)
{
    uint32_t at = 86;
    while (at + 8 <= len) {
        uint32_t size = rd32(desc + at);
        if (size < 8 || at + size > len) return 0;
        if (desc[at + 4] == 'm' && desc[at + 5] == 'p' && desc[at + 6] == 'g' && desc[at + 7] == 'x' && size >= MI_INDEX_HDR) {
            const uint8_t *a = desc + at;
            *fsspec = a + 8; *file_size = rd32(a + 80); *end = rd32(a + 84); *n = rd32(a + 88);
            if (MI_INDEX_HDR + 4 * *n > size) return 0;
            *pics = a + MI_INDEX_HDR;
            return 1;
        }
        at += size;
    }
    return 0;
}
