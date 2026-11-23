/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_rpza.c - Apple Video ('rpza', "Road Pizza") pictures decoded for the video decoder card
 * (psmac_vcodec.h).
 *
 * Backstory (22 November 2026, docs/08 item 110). Apple Video came with QuickTime 1.0 (1991): 16-bit
 * colour (0RRRRRGGGGGBBBBB) in 4 x 4 blocks, fast to decode on a 68030 at the cost of size. Written here
 * from the format's public description (MultimediaWiki, "Apple RPZA"):
 *  - A chunk: $E1 and a 24-bit length (the 4 bytes included), then opcodes for the blocks, left to right
 *    and top to bottom of the picture.
 *  - An opcode byte with its top bit set: the top three bits say what, the low five a count of blocks less
 *    one. $80: that many blocks unchanged; $A0: a colour, and that many blocks of it; $C0: two colours A and
 *    B, and for each block four bytes, a row each, two bits a pixel (high bits first) choosing among B, two
 *    colours between them (11/32 and 21/32 of the way, per component) and A.
 *  - A byte with its top bit clear is the first byte of a colour A: when the byte after the colour has its
 *    top bit set, that byte begins colour B of one four-colour block as above; otherwise A is the first of
 *    sixteen colours, the block's pixels one by one.
 * The picture is kept as 0x00RRGGBB with each 5-bit component widened as v << 3 | v >> 2, which the
 * 16-bit destinations take back exactly.
 */
#include "psmac_vcodec.h"

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline uint32_t rgb(uint32_t c)
{
    uint32_t r = c >> 10 & 31, g = c >> 5 & 31, b = c & 31;
    return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}
static int rz_open(void *state, struct vc_picture *p) { (void)state; (void)p; return VC_OK; }

static int rz_decode(void *state, struct vc_picture *pic, const uint8_t *d, uint32_t len)
{
    (void)state;
    if (len < 4) return VC_E_DATA;
    uint32_t clen = (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
    const uint8_t *p = d + 4, *end = d + (d[0] == 0xE1 && clen >= 4 && clen < len ? clen : len);
    uint32_t bw = (pic->w + 3) / 4, total = bw * ((pic->h + 3) / 4), bi = 0;
    while (p < end && bi < total) {
        uint32_t op = *p++, n = (op & 0x1F) + 1, a = 0, b, c4[4];
        if (!(op & 0x80)) {
            if (p >= end) break;
            a = op << 8 | *p++;
            op = 0;
            if (p < end && (*p & 0x80)) { op = 0x20; n = 1; }
        }
        if (n > total - bi) n = total - bi;
        switch (op & 0xE0) {
        case 0x80:
            bi += n;
            break;
        case 0xA0: {
            if (p + 2 > end) return VC_E_DATA;
            uint32_t c = rgb(be16(p)); p += 2;
            for (; n; n--, bi++) {
                uint32_t x0 = bi % bw * 4, y0 = bi / bw * 4;
                vc_touch(pic, x0, y0, x0 + 4, y0 + 4);
                for (uint32_t y = y0; y < y0 + 4 && y < pic->h; y++)
                    for (uint32_t x = x0; x < x0 + 4 && x < pic->w; x++) pic->rgb[y * pic->w + x] = c;
            }
            break;
        }
        case 0xC0:
            if (p + 2 > end) return VC_E_DATA;
            a = be16(p); p += 2;
            __attribute__((fallthrough));      /* colour B and the blocks */
        case 0x20: {
            if (p + 2 > end) return VC_E_DATA;
            b = be16(p); p += 2;
            c4[0] = b; c4[3] = a; c4[1] = c4[2] = 0;
            for (int sh = 10; sh >= 0; sh -= 5) {
                uint32_t ta = a >> sh & 31, tb = b >> sh & 31;
                c4[1] |= ((11 * ta + 21 * tb) >> 5) << sh;
                c4[2] |= ((21 * ta + 11 * tb) >> 5) << sh;
            }
            for (int k = 0; k < 4; k++) c4[k] = rgb(c4[k]);
            for (; n; n--, bi++) {
                if (p + 4 > end) return VC_E_DATA;
                uint32_t x0 = bi % bw * 4, y0 = bi / bw * 4;
                vc_touch(pic, x0, y0, x0 + 4, y0 + 4);
                for (uint32_t j = 0; j < 4; j++, p++) {
                    if (y0 + j >= pic->h) continue;
                    for (uint32_t i = 0; i < 4; i++)
                        if (x0 + i < pic->w) pic->rgb[(y0 + j) * pic->w + x0 + i] = c4[*p >> (6 - 2 * i) & 3];
                }
            }
            break;
        }
        case 0x00: {
            if (p + 30 > end) return VC_E_DATA;
            uint32_t x0 = bi % bw * 4, y0 = bi / bw * 4;
            vc_touch(pic, x0, y0, x0 + 4, y0 + 4);
            for (uint32_t k = 0; k < 16; k++) {
                uint32_t c = k ? be16(p) : a;
                if (k) p += 2;
                uint32_t x = x0 + (k & 3), y = y0 + (k >> 2);
                if (x < pic->w && y < pic->h) pic->rgb[y * pic->w + x] = rgb(c);
            }
            bi++;
            break;
        }
        default:
            return VC_E_DATA;
        }
    }
    return VC_OK;
}

const struct vc_codec vc_rpza = { VC_RPZA, "Apple Video", 0, 0, rz_open, rz_decode };
