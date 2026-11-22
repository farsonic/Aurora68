/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_alac.c — Apple Lossless (ALAC) for the stream channel (psmac_codec.h), written for this project.
 *
 * Why not Apple's own decoder (22 November 2026, docs/08 item 103). Apple published its ALAC encoder and
 * decoder in 2011 under the Apache License 2.0, and the plan was to vendor its decoder. The kernel binary
 * also holds the Alice68 modules (src/alice/), which are GPL-2.0-only, and the Free Software Foundation
 * holds Apache-2.0 incompatible with version 2 of the GPL: a kernel with both could not be passed on. So
 * this is a decoder of the format as Apple's release documents it (ALACMagicCookieDescription.txt and the
 * bit stream its decoder reads), written here in the project's style and under its licence (MPL-2.0, which
 * names the GPL as a compatible secondary licence). tools/codec_test.c checks that its output is the
 * same, sample for sample, as ffmpeg's decoder for files ffmpeg encoded (16- and 24-bit, mono and stereo).
 *
 * The format. A packet holds up to frameLength (4096) frames, as a sequence of elements, each starting
 * with a 3-bit tag: a single channel (SCE, 0) or the low-frequency one (LFE, 3), a channel pair (CPE, 1),
 * filler (FIL, 6), data (DSE, 4) and the end (END, 7). A channel element has a 4-bit instance tag, 12
 * unused bits, then 4 bits: a partial frame (the frame count follows in 32 bits), bytesShifted (2 bits:
 * the low bytes of each sample stored raw, apart) and escape (the samples stored raw). Otherwise: the mix
 * (mixBits, mixRes, for a pair), then per channel a predictor mode and its quantization shift (4+4 bits),
 * the history multiplier's factor and the number of coefficients (3+5 bits) and the coefficients (16 bits
 * each); then the shifted-out low bytes, interleaved, if bytesShifted; then each channel's residuals,
 * coded by an adaptive Golomb-Rice scheme with runs of zeros. The predictor runs on the residuals (an
 * adaptive LPC whose coefficients move by one each sample towards less error), and for a pair the mix is
 * undone: left = u + v - ((mixRes * v) >> mixBits), right = left - v.
 *
 * Only the first two channels are decoded (the mixer is stereo); a packet's other elements are skipped
 * with it, as each packet comes with its length.
 */
#include <string.h>
#include "psmac_codec.h"

#define AL_MAXFRAME 4096u                      /* frameLength: Apple's encoder uses 4096, and the format's default */
#define AL_MAXPACKET (1u << 20)                /* a packet larger than this is no ALAC packet */
struct al {
    int have_cfg;
    uint32_t frame_len, max_packet;
    uint8_t bit_depth, pb, mb, kb, nch;
    uint16_t max_run;
    uint8_t *pkt;                              /* the packet being decoded */
    uint32_t pkt_size;
    int32_t ch[2][AL_MAXFRAME];                /* the two channels, and the residuals on their way */
    int32_t res[AL_MAXFRAME];
    uint16_t shift[2 * AL_MAXFRAME];           /* the low bytes stored apart (bytesShifted) */
    int32_t pend_n, pend_at;                   /* frames of the last packet not yet given out */
    int pend_ch;                               /* its channels: 1 or 2 */
    int shiftbits;                             /* 8 * bytesShifted of the channels given out */
};

/* ---- reading bits, most significant first ------------------------------------------------------------ */
struct bits { const uint8_t *b; uint32_t n, at; int bad; };   /* n and at in bits */
static uint32_t bits_get(struct bits *s, int k)   /* k = 0..32 bits */
{
    if (!k) return 0;
    if (s->at + (uint32_t)k > s->n) { s->bad = 1; s->at = s->n; return 0; }
    uint64_t v = 0;
    uint32_t byte = s->at >> 3, have = 0;
    while (have < (uint32_t)k + (s->at & 7)) { v = v << 8 | s->b[byte++]; have += 8; }
    v >>= have - (uint32_t)k - (s->at & 7);
    s->at += (uint32_t)k;
    return (uint32_t)(v & ((k == 32) ? 0xFFFFFFFFu : ((1u << k) - 1)));
}
static int32_t sext(uint32_t v, int bits) { return bits >= 32 ? (int32_t)v : (int32_t)(v << (32 - bits)) >> (32 - bits); }
static int clz32(uint32_t v) { int n = 0; if (!v) return 32; while (!(v & 0x80000000u)) { v <<= 1; n++; } return n; }
static int log2i(uint32_t v) { return 31 - clz32(v); }

/* one Golomb-Rice value: a unary prefix of up to 9 ones; 9 of them mean the value follows raw in
   escape_bits bits; else prefix * (2^k - 1) plus the next k bits less one, those being 2 or more, or
   the prefix alone, one bit fewer taken, when they are 0 or 1 */
static uint32_t rice(struct bits *s, int k, int escape_bits)
{
    uint32_t pre = 0;
    while (pre < 9 && bits_get(s, 1)) pre++;
    if (s->bad) return 0;
    if (pre >= 9) return bits_get(s, escape_bits);
    if (k <= 1) return pre;
    uint32_t x = pre * ((1u << k) - 1);
    uint32_t extra = bits_get(s, k - 1);       /* the k bits, read as k - 1 and then, maybe, one more */
    if (extra) {
        extra = extra << 1 | bits_get(s, 1);
        return x + extra - 1;
    }
    return x;                                  /* the k bits would be 0 or 1: only k - 1 of them were the value's */
}
/* a channel's residuals: the adaptive Golomb-Rice coding with its history (mean) and runs of zeros */
static int residuals(struct al *a, struct bits *s, int32_t *out, uint32_t n, int chan_bits, uint32_t pb_factor)
{
    uint32_t hist = a->mb, mult = (uint32_t)a->pb * pb_factor / 4, sign_mod = 0;
    for (uint32_t i = 0; i < n; i++) {
        int k = log2i((hist >> 9) + 3);
        if (k > a->kb) k = a->kb;
        uint32_t x = rice(s, k, chan_bits) + sign_mod;
        sign_mod = 0;
        out[i] = (int32_t)((x >> 1) ^ (0u - (x & 1)));     /* 0, -1, 1, -2, 2, ... */
        if (x > 0xFFFF) hist = 0xFFFF;
        else hist = hist + x * mult - ((hist * mult) >> 9);
        if (hist < 128 && i + 1 < n) {          /* quiet: a run of zeros may follow */
            int kz = clz32(hist) - 24 + (int)((hist + 16) >> 6);
            if (kz > a->kb) kz = a->kb;
            uint32_t run = rice(s, kz, 16);
            if (run >= n - i) return 0;
            for (uint32_t j = 0; j < run; j++)
                out[++i] = 0;
            if (run < 0xFFFF) sign_mod = 1;
            hist = 0;
        }
        if (s->bad) return 0;
    }
    return 1;
}
/* the adaptive predictor: out from the residuals (res and out may be the same array only for order 31) */
static void predict(const int32_t *res, int32_t *out, uint32_t n, int16_t *coef, int order, int chan_bits, int shift)
{
    if (!n) return;
    out[0] = res[0];
    if (order == 0) { if (out != res) memcpy(out + 1, res + 1, (n - 1) * sizeof *out); return; }
    if (order == 31) {                         /* the first difference only */
        for (uint32_t j = 1; j < n; j++)
            out[j] = sext((uint32_t)res[j] + (uint32_t)out[j - 1], chan_bits);
        return;
    }
    uint32_t j;
    for (j = 1; j <= (uint32_t)order && j < n; j++)
        out[j] = sext((uint32_t)res[j] + (uint32_t)out[j - 1], chan_bits);
    int32_t half = shift ? 1 << (shift - 1) : 0;
    for (; j < n; j++) {
        /* coef[0] goes with the newest sample, out[j-1]; all of them against the one before the window, top */
        /* in 32 bits that wrap, as the format's own decoder sums (deep samples can overflow them) */
        int32_t top = out[j - (uint32_t)order - 1];
        uint32_t sum = 0;
        for (int k = 0; k < order; k++)
            sum += (uint32_t)((int64_t)coef[k] * ((int64_t)out[j - 1 - (uint32_t)k] - top));
        int32_t e = res[j];
        out[j] = sext((uint32_t)e + (uint32_t)top + (uint32_t)((int32_t)(sum + (uint32_t)half) >> shift), chan_bits);
        if (e > 0) {                           /* nudge the coefficients, oldest sample first, until the error is spent */
            for (int k = order - 1; k >= 0 && e > 0; k--) {
                int32_t d = top - out[j - 1 - (uint32_t)k], sg = (d > 0) - (d < 0);
                coef[k] -= (int16_t)sg;
                e -= (order - k) * ((sg * d) >> shift);
            }
        } else if (e < 0) {
            for (int k = order - 1; k >= 0 && e < 0; k--) {
                int32_t d = top - out[j - 1 - (uint32_t)k], sg = (d > 0) - (d < 0);
                coef[k] += (int16_t)sg;
                e -= (order - k) * ((-sg * d) >> shift);
            }
        }
    }
}

/* one channel element (SCE/LFE: pair 0; CPE: pair 1) into a->ch[first ...]; the frames, 0 if damaged */
static uint32_t element(struct al *a, struct bits *s, int pair, int first)
{
    int nc = pair ? 2 : 1;
    bits_get(s, 4);                            /* the element's instance tag */
    if (bits_get(s, 12)) return 0;             /* unused, zero */
    uint32_t hb = bits_get(s, 4);
    int partial = (hb >> 3) & 1, bshift = (hb >> 1) & 3, escape = hb & 1;
    uint32_t n = a->frame_len;
    if (partial) n = bits_get(s, 32);
    if (!n || n > a->frame_len || s->bad) return 0;
    int32_t *dst[2] = { a->ch[first], a->ch[first + 1 < 2 ? first + 1 : 1] };
    a->shiftbits = 0;
    if (!escape) {
        int chan_bits = a->bit_depth - 8 * bshift + (pair ? 1 : 0);
        uint32_t mix_bits = bits_get(s, 8);
        int32_t mix_res = (int8_t)bits_get(s, 8);
        int mode[2], qshift[2], order[2];
        uint32_t pbf[2];
        int16_t coef[2][32];
        for (int c = 0; c < nc; c++) {
            uint32_t x = bits_get(s, 8);
            mode[c] = (int)(x >> 4); qshift[c] = (int)(x & 15);
            x = bits_get(s, 8);
            pbf[c] = x >> 5; order[c] = (int)(x & 31);
            for (int k = 0; k < order[c]; k++)
                coef[c][k] = (int16_t)bits_get(s, 16);
        }
        if (bshift) {                          /* the low bytes, sample by sample, channel by channel */
            for (uint32_t i = 0; i < n; i++)
                for (int c = 0; c < nc; c++)
                    a->shift[i * 2 + (uint32_t)c] = (uint16_t)bits_get(s, 8 * bshift);
            a->shiftbits = 8 * bshift;
        }
        if (s->bad || chan_bits > 32) return 0;
        for (int c = 0; c < nc; c++) {
            if (!residuals(a, s, a->res, n, chan_bits, pbf[c])) return 0;
            if (mode[c]) predict(a->res, a->res, n, 0, 31, chan_bits, 0);
            predict(a->res, dst[c], n, coef[c], order[c], chan_bits, qshift[c]);
        }
        if (pair && mix_res) {
            for (uint32_t i = 0; i < n; i++) {
                int32_t u = dst[0][i], v = dst[1][i];
                int32_t l = u + v - (int32_t)(((int64_t)mix_res * v) >> mix_bits);
                dst[0][i] = l; dst[1][i] = l - v;
            }
        }
    } else {                                   /* stored: each sample of each channel, bit_depth bits */
        for (uint32_t i = 0; i < n; i++)
            for (int c = 0; c < nc; c++)
                dst[c][i] = sext(bits_get(s, a->bit_depth), a->bit_depth);
        if (s->bad) return 0;
    }
    return n;
}

int pc_alac_open(struct pcodec *c)
{
    struct al *a = c->mem->alloc(sizeof *a);
    if (!a) { c->error = "no memory for the ALAC decoder"; return 0; }
    memset(a, 0, sizeof *a);
    c->priv = a;
    return 1;
}
void pc_alac_close(struct pcodec *c)
{
    struct al *a = c->priv;
    if (!a) return;
    if (a->pkt) c->mem->release(a->pkt);
    c->mem->release(a);
}
static uint32_t be32p(const struct pc_src *s, uint32_t k)
{
    return (uint32_t)pc_peek(s, k) << 24 | (uint32_t)pc_peek(s, k + 1) << 16 | (uint32_t)pc_peek(s, k + 2) << 8 | pc_peek(s, k + 3);
}
/* "alac", the configuration's length, then ALACSpecificConfig: frameLength 32, compatibleVersion 8,
   bitDepth 8, pb 8, mb 8, kb 8, numChannels 8, maxRun 16, maxFrameBytes 32, avgBitRate 32, sampleRate 32 */
static int config(struct pcodec *c, struct al *a, struct pc_src *s)
{
    if (pc_avail(s) < 8) return s->all_in ? PC_ERROR : PC_MORE;
    if (pc_peek(s, 0) != 'a' || pc_peek(s, 1) != 'l' || pc_peek(s, 2) != 'a' || pc_peek(s, 3) != 'c') { c->error = "no ALAC header"; return PC_ERROR; }
    uint32_t len = be32p(s, 4);
    if (len < 24 || len > 256) { c->error = "an ALAC configuration of a size it cannot have"; return PC_ERROR; }
    if (pc_avail(s) < 8 + len) return s->all_in ? PC_ERROR : PC_MORE;
    s->tail += 8;
    uint8_t k[24];
    pc_read(s, k, 24);
    s->tail += len - 24;
    a->frame_len = (uint32_t)k[0] << 24 | (uint32_t)k[1] << 16 | (uint32_t)k[2] << 8 | k[3];
    a->bit_depth = k[5]; a->pb = k[6]; a->mb = k[7]; a->kb = k[8]; a->nch = k[9];
    a->max_run = (uint16_t)(k[10] << 8 | k[11]);
    a->max_packet = (uint32_t)k[12] << 24 | (uint32_t)k[13] << 16 | (uint32_t)k[14] << 8 | k[15];
    c->rate = (uint32_t)k[20] << 24 | (uint32_t)k[21] << 16 | (uint32_t)k[22] << 8 | k[23];
    c->channels = a->nch; c->bits = a->bit_depth;
    if (k[4] != 0 || !a->frame_len || a->frame_len > AL_MAXFRAME || !a->nch ||
        (a->bit_depth != 16 && a->bit_depth != 20 && a->bit_depth != 24 && a->bit_depth != 32) || !c->rate) {
        c->error = "an ALAC configuration this decoder does not take (version, frame length, depth)";
        return PC_ERROR;
    }
    a->have_cfg = 1;
    return 1;
}
int pc_alac_decode(struct pcodec *c, struct pc_src *s, int16_t *out, int max)
{
    struct al *a = c->priv;
    if (!a->have_cfg) {
        int r = config(c, a, s);
        if (r <= 0) { if (r == PC_ERROR && !c->error) c->error = "the ALAC stream ended in its header"; return r; }
    }
    if (!a->pend_n) {
        if (pc_avail(s) < 4) return s->all_in ? PC_END : PC_MORE;
        uint32_t len = be32p(s, 0);
        if (!len || len > AL_MAXPACKET) { c->error = "an ALAC packet of a size it cannot have"; return PC_ERROR; }
        if (pc_avail(s) < 4 + len) return s->all_in ? PC_END : PC_MORE;
        if (len > a->pkt_size) {
            if (a->pkt) c->mem->release(a->pkt);
            a->pkt_size = len < 16384 ? 16384 : len;
            a->pkt = c->mem->alloc(a->pkt_size);
            if (!a->pkt) { a->pkt_size = 0; c->error = "no memory for an ALAC packet"; return PC_ERROR; }
        }
        s->tail += 4;
        pc_read(s, a->pkt, len);
        struct bits b = { a->pkt, len * 8, 0, 0 };
        int got = 0;                           /* channels decoded */
        uint32_t frames = 0;
        for (;;) {
            uint32_t tag = bits_get(&b, 3);
            if (b.bad || tag == 7) break;      /* END */
            if (tag == 0 || tag == 3 || tag == 1) {
                if (got >= 2) break;           /* the channels after the first two: not wanted */
                int pair = tag == 1;
                if (!pair && !got && a->nch >= 3) {    /* 3 channels and more begin with the centre: the pair after it is wanted */
                    if (!element(a, &b, 0, 0)) { c->error = "a damaged ALAC packet"; return PC_ERROR; }
                    continue;
                }
                if (pair && got) break;       /* a pair after a single channel: the third and fourth */
                uint32_t n = element(a, &b, pair, got);
                if (!n) { c->error = "a damaged ALAC packet"; return PC_ERROR; }
                frames = n;
                got += pair ? 2 : 1;
                if (got == 2) break;
                continue;
            }
            if (tag == 6) {                    /* FIL: count 4 bits (15: + 8 more less one) bytes */
                uint32_t cnt = bits_get(&b, 4);
                if (cnt == 15) cnt += bits_get(&b, 8) - 1;
                b.at += cnt * 8;
                continue;
            }
            if (tag == 4) {                    /* DSE: tag 4, align 1, count 8 (255: + 8 more), then bytes */
                bits_get(&b, 4);
                uint32_t align = bits_get(&b, 1), cnt = bits_get(&b, 8);
                if (cnt == 255) cnt += bits_get(&b, 8);
                if (align) b.at = (b.at + 7) & ~7u;
                b.at += cnt * 8;
                continue;
            }
            c->error = "an ALAC element this decoder does not know";
            return PC_ERROR;
        }
        if (!got) return PC_MORE;              /* a packet without audio: on to the next */
        a->pend_n = (int32_t)frames; a->pend_at = 0; a->pend_ch = got;
    }
    /* out: the full sample (the shifted-out low bytes put back) cut to its top 16 bits */
    int k = a->pend_n - a->pend_at < max ? a->pend_n - a->pend_at : max;
    int down = a->bit_depth - 16, sb = a->shiftbits;
    for (int i = 0; i < k; i++) {
        uint32_t f = (uint32_t)(a->pend_at + i);
        for (int c2 = 0; c2 < 2; c2++) {
            int src = a->pend_ch > 1 ? c2 : 0;
            int64_t v = a->ch[src][f];
            if (sb) v = v * (1 << sb) + a->shift[f * 2 + (uint32_t)src];
            out[2 * i + c2] = (int16_t)(v >> down);
        }
    }
    a->pend_at += k;
    if (a->pend_at >= a->pend_n) a->pend_n = 0;
    return k;
}
