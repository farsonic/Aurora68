/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_qtrle.c - Animation ('rle ') pictures decoded for the video decoder card (psmac_vcodec.h).
 *
 * Backstory (22 November 2026, docs/08 item 110). QuickTime's Animation compressor (1991) is run-length
 * coding by lines, lossless at its best quality, used for screen recordings, title sequences and
 * cartoons, at every depth a Mac has. Written here from the format's public description (MultimediaWiki,
 * "Apple QuickTime RLE"):
 *  - A chunk: its length (4 bytes), a header word; with bit 3 of the header set, the first line that
 *    changed and how many lines did (2 bytes each, each followed by 2 unused), otherwise every line.
 *  - Each line: a byte, the pixels to leave as they were plus one; then codes, each a signed byte: -1 ends
 *    the line, 0 is followed by another skip byte (again plus one), a positive n is n pixels as they are,
 *    a negative -n is one pixel repeated n times.
 *  - A "pixel" of a code is a pixel at 16 (0RRRRRGGGGGBBBBB), 24 (R, G, B) and 32 bits (A, R, G, B: the
 *    alpha byte not used); at 2, 4 and 8 bits it is a group of four bytes (16, 8 or 4 pixels), and skips
 *    count groups. At 1 bit a group is two bytes (16 pixels). The grey depths 33 to 40 are 1 to 8 bits.
 * The indexed depths keep the colour indices too (the idx plane), so an 8-bit destination with the same
 * table is written index for index.
 */
#include "psmac_vcodec.h"

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

static int rl_open(void *state, struct vc_picture *p)
{
    (void)state;
    uint32_t d = p->depth > 32 ? p->depth - 32 : p->depth;
    return d == 1 || d == 2 || d == 4 || d == 8 || d == 16 || d == 24 || d == 32 ? VC_OK : VC_E_DEPTH;
}

static int rl_decode(void *state, struct vc_picture *pic, const uint8_t *d, uint32_t len)
{
    (void)state;
    if (len < 8) return VC_OK;                   /* a picture that changed nothing */
    uint32_t depth = pic->depth > 32 ? pic->depth - 32 : pic->depth;
    uint32_t hdr = be16(d + 4), row = 0, lines = pic->h;
    const uint8_t *p = d + 6, *end = d + len;
    if (hdr & 0x0008) {
        if (len < 14) return VC_E_DATA;
        row = be16(d + 6); lines = be16(d + 10);
        p = d + 14;
    }
    if (row > pic->h) return VC_E_DATA;
    if (lines > pic->h - row) lines = pic->h - row;
    int indexed = depth <= 8;
    uint32_t unit = indexed ? (depth == 1 ? 2 : 4) : depth / 8;      /* bytes of a code's "pixel" */
    uint32_t ppu = indexed ? unit * 8 / depth : 1;                    /* pixels in it */
    uint32_t w = pic->w;
    for (; lines; lines--, row++) {
        if (p >= end) return VC_E_DATA;
        uint32_t x = (uint32_t)(*p++ - 1) * ppu;
        uint32_t *rrow = pic->rgb + row * w;
        uint8_t *irow = pic->idx ? pic->idx + row * w : 0;
        for (;;) {
            if (p >= end) return VC_E_DATA;
            int code = (int8_t)*p++;
            if (code == -1) break;
            if (code == 0) {
                if (p >= end) return VC_E_DATA;
                x += (uint32_t)(*p++ - 1) * ppu;
                continue;
            }
            uint32_t n = (uint32_t)(code < 0 ? -code : code);
            int rep = code < 0;
            const uint8_t *src = p;
            if (p + (rep ? unit : unit * n) > end) return VC_E_DATA;
            vc_touch(pic, x, row, x + n * ppu, row + 1);
            p += rep ? unit : unit * n;
            for (uint32_t k = 0; k < n; k++) {
                const uint8_t *u = rep ? src : src + k * unit;
                if (indexed) {
                    for (uint32_t i = 0; i < ppu; i++, x++) {
                        uint32_t bit = i * depth, v = u[bit >> 3] >> (8 - depth - (bit & 7)) & ((1u << depth) - 1);
                        if (x < w) { rrow[x] = pic->pal[v]; if (irow) irow[x] = (uint8_t)v; }
                    }
                } else {
                    uint32_t c;
                    if (depth == 16) {
                        uint32_t v = be16(u), r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
                        c = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
                    } else if (depth == 24) c = (uint32_t)u[0] << 16 | (uint32_t)u[1] << 8 | u[2];
                    else c = (uint32_t)u[1] << 16 | (uint32_t)u[2] << 8 | u[3];
                    if (x < w) rrow[x] = c;
                    x++;
                }
            }
        }
    }
    return VC_OK;
}

const struct vc_codec vc_qtrle = { VC_RLE, "Animation", 0, 0, rl_open, rl_decode, 0 };
