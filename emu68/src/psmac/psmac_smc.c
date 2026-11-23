/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_smc.c - Graphics ('smc ') pictures decoded for the video decoder card (psmac_vcodec.h).
 *
 * Backstory (22 November 2026, docs/08 item 110). QuickTime's Graphics compressor (1991) is for 8-bit
 * pictures with few colours (drawings, slides, the 256-colour animations of early CD-ROMs). Written here
 * from the format's public description (MultimediaWiki, "Apple SMC"):
 *  - A chunk: a byte, a 24-bit length (the 4 bytes included), then opcodes for 4 x 4 blocks, left to right
 *    and top to bottom.
 *  - The opcode's high nibble says what, its low nibble a count less one; for the odd nibbles of the first
 *    eight the count less one is the next byte instead:
 *      0, 1  that many blocks left as they were
 *      2, 3  the block before repeated that many times
 *      4, 5  the two blocks before repeated that many times (as a pair)
 *      6, 7  a colour index, and that many blocks of it
 *      8     two colours (kept in a cache of 256 pairs, filled round from entry 0 in each picture), then
 *            each block's 16 one-bit choices
 *      9     a pair from the cache by its index, then the blocks
 *      A, B  the same with four colours and 2 bits a pixel (a row a byte)
 *      C, D  the same with eight colours and 3 bits a pixel: six bytes a block as three 16-bit words a, b
 *            and c, whose top 12 bits run on for the first two rows (a, then b) and whose last nibbles
 *            with c's top 12 bits make the last two rows (c's 12 bits, then the nibbles of a, b and c)
 *      E     sixteen colour indices, the block as it is
 *  - The pixels are indices into the image's colour table (its ImageDescription's).
 */
#include "psmac_vcodec.h"

struct smc_state {
    uint8_t pairs[256][2], quads[256][4], octets[256][8];
    uint32_t np, nq, no;
};

static int sm_open(void *state, struct vc_picture *p) { (void)state; return p->depth == 8 || p->depth == 40 ? VC_OK : VC_E_DEPTH; }

static void put(struct vc_picture *pic, uint32_t bi, uint32_t bw, const uint8_t px[16])
{
    uint32_t x0 = bi % bw * 4, y0 = bi / bw * 4;
    vc_touch(pic, x0, y0, x0 + 4, y0 + 4);
    for (uint32_t j = 0; j < 4; j++) {
        if (y0 + j >= pic->h) return;
        for (uint32_t i = 0; i < 4; i++)
            if (x0 + i < pic->w) {
                uint32_t o = (y0 + j) * pic->w + x0 + i;
                pic->idx[o] = px[j * 4 + i];
                pic->rgb[o] = pic->pal[px[j * 4 + i]];
            }
    }
}
static void get(const struct vc_picture *pic, uint32_t bi, uint32_t bw, uint8_t px[16])
{
    uint32_t x0 = bi % bw * 4, y0 = bi / bw * 4;
    for (uint32_t j = 0; j < 4; j++)
        for (uint32_t i = 0; i < 4; i++)
            px[j * 4 + i] = y0 + j < pic->h && x0 + i < pic->w ? pic->idx[(y0 + j) * pic->w + x0 + i] : 0;
}

static int sm_decode(void *state, struct vc_picture *pic, const uint8_t *d, uint32_t len)
{
    struct smc_state *st = state;
    if (len < 4 || !pic->idx) return VC_E_DATA;
    uint32_t clen = (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
    const uint8_t *p = d + 4, *end = d + (clen >= 4 && clen < len ? clen : len);
    uint32_t bw = (pic->w + 3) / 4, total = bw * ((pic->h + 3) / 4), bi = 0;
    uint8_t px[16], px2[16];
    st->np = st->nq = st->no = 0;              /* each picture fills its caches from the first entry (their contents stay) */
#define NEED(k) do { if (p + (k) > end) return VC_E_DATA; } while (0)
    while (p < end && bi < total) {
        uint32_t op = *p++, hi = op >> 4, n = (op & 15) + 1;
        if (hi < 8 && (hi & 1)) { NEED(1); n = *p++ + 1u; }
        switch (hi) {
        case 0: case 1:
            bi += n;
            break;
        case 2: case 3:
            if (!bi) return VC_E_DATA;
            get(pic, bi - 1, bw, px);
            for (; n && bi < total; n--) put(pic, bi++, bw, px);
            break;
        case 4: case 5:
            if (bi < 2) return VC_E_DATA;
            get(pic, bi - 2, bw, px); get(pic, bi - 1, bw, px2);
            for (; n && bi < total; n--) { put(pic, bi++, bw, px); if (bi < total) put(pic, bi++, bw, px2); }
            break;
        case 6: case 7:
            NEED(1);
            for (int k = 0; k < 16; k++) px[k] = *p;
            p++;
            for (; n && bi < total; n--) put(pic, bi++, bw, px);
            break;
        case 8: case 9: {
            const uint8_t *c;
            if (hi == 8) { NEED(2); c = st->pairs[st->np]; st->pairs[st->np][0] = p[0]; st->pairs[st->np][1] = p[1]; st->np = (st->np + 1) & 255; p += 2; }
            else { NEED(1); c = st->pairs[*p++]; }
            for (; n && bi < total; n--) {
                NEED(2);
                uint32_t f = (uint32_t)p[0] << 8 | p[1];
                p += 2;
                for (int k = 0; k < 16; k++) px[k] = c[f >> (15 - k) & 1];
                put(pic, bi++, bw, px);
            }
            break;
        }
        case 10: case 11: {
            const uint8_t *c;
            if (hi == 10) { NEED(4); c = st->quads[st->nq]; for (int k = 0; k < 4; k++) st->quads[st->nq][k] = p[k]; st->nq = (st->nq + 1) & 255; p += 4; }
            else { NEED(1); c = st->quads[*p++]; }
            for (; n && bi < total; n--) {
                NEED(4);
                for (int k = 0; k < 16; k++) px[k] = c[p[k >> 2] >> (6 - 2 * (k & 3)) & 3];
                p += 4;
                put(pic, bi++, bw, px);
            }
            break;
        }
        case 12: case 13: {
            const uint8_t *c;
            if (hi == 12) { NEED(8); c = st->octets[st->no]; for (int k = 0; k < 8; k++) st->octets[st->no][k] = p[k]; st->no = (st->no + 1) & 255; p += 8; }
            else { NEED(1); c = st->octets[*p++]; }
            for (; n && bi < total; n--) {
                NEED(6);
                uint32_t a = (uint32_t)p[0] << 8 | p[1], b = (uint32_t)p[2] << 8 | p[3], cc = (uint32_t)p[4] << 8 | p[5];
                p += 6;
                uint32_t top = (a & 0xFFF0) << 8 | b >> 4;
                uint32_t bot = (cc & 0xFFF0) << 8 | (a & 15) << 8 | (b & 15) << 4 | (cc & 15);
                for (int k = 0; k < 8; k++) px[k] = c[top >> (21 - 3 * k) & 7];
                for (int k = 0; k < 8; k++) px[8 + k] = c[bot >> (21 - 3 * k) & 7];
                put(pic, bi++, bw, px);
            }
            break;
        }
        case 14:
            for (; n && bi < total; n--) {
                NEED(16);
                for (int k = 0; k < 16; k++) px[k] = p[k];
                p += 16;
                put(pic, bi++, bw, px);
            }
            break;
        default:
            return VC_E_DATA;
        }
    }
#undef NEED
    return VC_OK;
}

const struct vc_codec vc_smc = { VC_SMC, "Graphics", sizeof(struct smc_state), 1, sm_open, sm_decode };
