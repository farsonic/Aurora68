/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_cinepak.c - Cinepak ('cvid') pictures decoded for the video decoder card (psmac_vcodec.h).
 *
 * Backstory (22 November 2026, docs/08 item 110). Cinepak (SuperMac's "Compact Video", 1991) is the
 * codec of the CD-ROM era: most QuickTime movies made for 68k Macs use it, and a 68030 decodes a 320 x
 * 240 picture in tens of milliseconds. Written here from the public description of the bit stream (Tim
 * Ferguson's "Cinepak codec" notes, 1999, and the MultimediaWiki page that follows them):
 *  - A picture: flags (1 byte), length (3), width, height and the number of strips (2 each), then the
 *    strips, each a band of the picture as wide as it, one after the other from the top.
 *  - A strip: an id ($1000 key, $1100 inter), its length, and a rectangle (12 bytes); then chunks, each an
 *    id and a length (4 bytes) and data.
 *  - Two codebooks per strip, V1 and V4, of 256 entries each. An entry is four luminances (a 2 x 2 patch:
 *    top left, top right, bottom left, bottom right) and two signed colour differences U and V for the
 *    patch; in the grey form (ids with $0400) only the four luminances. Chunk $2000 is a V4 codebook whole,
 *    $2200 V1 whole; with $0100 set ($2100, $2300) it is a partial update: a 32-bit word of flags, high bit
 *    first, says which of the next 32 entries follow. Codebooks are kept from picture to picture, and when
 *    bit 0 of the picture's flags is clear a strip starts from the strip above's (encoders send the first
 *    strip's books once).
 *  - The picture in 4 x 4 blocks, left to right and top to bottom of the strip. $3000 (key): a flag per
 *    block (32 to a word, high bit first) chooses V4 (four bytes: an entry for each 2 x 2 quarter, in the
 *    same order as the luminances) or V1 (one byte: one entry, each luminance doubled to a 2 x 2 patch).
 *    $3100 (inter): first a flag "this block changed" (if not, it stays as it was), then the V4/V1 flag.
 *    $3200: every block V1.
 *  - Colour: R = Y + 2V, G = Y - U/2 - V, B = Y + 2U, each clamped to 0-255 (Cinepak's own approximation,
 *    which its encoder assumed).
 * The entries are turned into four 0x00RRGGBB pixels when they arrive, so a block is copied, not computed.
 */
#include "psmac_vcodec.h"

#define MAX_STRIPS 32                        /* more than any encoder of the period made (SuperMac's: up to 3; FFmpeg's: 3) */

struct cv_state {
    uint32_t v1[MAX_STRIPS][256][4];
    uint32_t v4[MAX_STRIPS][256][4];
};

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint32_t clamp(int v) { return v < 0 ? 0u : v > 255 ? 255u : (uint32_t)v; }

static int cv_open(void *state, struct vc_picture *p)
{
    (void)state;
    return p->depth == 24 || p->depth == 32 || p->depth == 40 || p->depth == 8 ? VC_OK : VC_E_DEPTH;
}

static void entry(uint32_t px[4], const uint8_t *e, int grey)
{
    int u = grey ? 0 : (int8_t)e[4], v = grey ? 0 : (int8_t)e[5];
    for (int i = 0; i < 4; i++) {
        int y = e[i];
        px[i] = clamp(y + 2 * v) << 16 | clamp(y - u / 2 - v) << 8 | clamp(y + 2 * u);
    }
}
/* a codebook chunk into book; whole or partial, colour or grey */
static void codebook(uint32_t book[256][4], uint32_t id, const uint8_t *p, const uint8_t *end)
{
    int grey = (id & 0x0400) != 0, partial = (id & 0x0100) != 0;
    uint32_t esz = grey ? 4 : 6, flags = 0, nbits = 0;
    for (uint32_t i = 0; i < 256; i++) {
        if (partial) {
            if (!nbits) {
                if (p + 4 > end) return;
                flags = be32(p); p += 4; nbits = 32;
            }
            uint32_t bit = flags & 0x80000000u;
            flags <<= 1; nbits--;
            if (!bit) continue;
        }
        if (p + esz > end) return;
        entry(book[i], p, grey);
        p += esz;
    }
}

/* a 4 x 4 block at (x, y) of the picture, clipped to it */
static void put_v1(struct vc_picture *pic, uint32_t x, uint32_t y, const uint32_t px[4])
{
    vc_touch(pic, x, y, x + 4, y + 4);
    for (uint32_t j = 0; j < 4; j++) {
        if (y + j >= pic->h) return;
        uint32_t *row = pic->rgb + (y + j) * pic->w;
        for (uint32_t i = 0; i < 4; i++)
            if (x + i < pic->w) row[x + i] = px[(j >> 1) * 2 + (i >> 1)];
    }
}
static void put_v4(struct vc_picture *pic, uint32_t x, uint32_t y, const uint32_t *q[4])
{
    vc_touch(pic, x, y, x + 4, y + 4);
    for (uint32_t j = 0; j < 4; j++) {
        if (y + j >= pic->h) return;
        uint32_t *row = pic->rgb + (y + j) * pic->w;
        for (uint32_t i = 0; i < 4; i++)
            if (x + i < pic->w) row[x + i] = q[(j >> 1) * 2 + (i >> 1)][(j & 1) * 2 + (i & 1)];
    }
}

/* the blocks of one strip, rows [y0, y1) */
static int vectors(struct cv_state *st, int s, struct vc_picture *pic, uint32_t id, const uint8_t *p, const uint8_t *end,
                   uint32_t y0, uint32_t y1)
{
    uint32_t flags = 0, nbits = 0;
    int inter = id == 0x3100, all_v1 = id == 0x3200;
#define BIT(out) do { if (!nbits) { if (p + 4 > end) return VC_E_DATA; flags = be32(p); p += 4; nbits = 32; } \
                      out = flags >> 31; flags <<= 1; nbits--; } while (0)
    for (uint32_t y = y0; y < y1; y += 4)
        for (uint32_t x = 0; x < pic->w; x += 4) {
            uint32_t b;
            if (inter) {
                BIT(b);
                if (!b) continue;
            }
            if (all_v1) b = 0;
            else BIT(b);
            if (b) {
                if (p + 4 > end) return VC_E_DATA;
                const uint32_t *q[4] = { st->v4[s][p[0]], st->v4[s][p[1]], st->v4[s][p[2]], st->v4[s][p[3]] };
                p += 4;
                put_v4(pic, x, y, q);
            } else {
                if (p + 1 > end) return VC_E_DATA;
                put_v1(pic, x, y, st->v1[s][*p++]);
            }
        }
#undef BIT
    return VC_OK;
}

static int cv_decode(void *state, struct vc_picture *pic, const uint8_t *d, uint32_t len)
{
    struct cv_state *st = state;
    if (len < 10) return VC_E_DATA;
    uint32_t flags = d[0], flen = be24(d + 1), strips = be16(d + 8);
    const uint8_t *end = d + (flen >= 10 && flen < len ? flen : len), *p = d + 10;
    uint32_t y = 0;
    for (uint32_t s = 0; s < strips && s < MAX_STRIPS; s++) {
        if (p + 12 > end) break;
        uint32_t ssize = be16(p + 2), sy1 = be16(p + 4), sy2 = be16(p + 8), sh;
        const uint8_t *send = p + ssize > end || ssize < 12 ? end : p + ssize, *c = p + 12;
        /* the strip's rectangle: relative (top 0, bottom its height) as most encoders write it, or the
           picture's own rows */
        sh = sy1 && sy2 > sy1 ? sy2 - sy1 : sy2;
        if (s && !(flags & 1)) {
            for (int i = 0; i < 256; i++)
                for (int k = 0; k < 4; k++) { st->v1[s][i][k] = st->v1[s - 1][i][k]; st->v4[s][i][k] = st->v4[s - 1][i][k]; }
        }
        uint32_t y1 = y + sh > pic->h ? pic->h : y + sh;
        while (c + 4 <= send) {
            uint32_t cid = be16(c), csize = be16(c + 2);
            const uint8_t *cend = csize < 4 || c + csize > send ? send : c + csize;
            switch (cid & 0xFF00) {
            case 0x2000: case 0x2100: case 0x2400: case 0x2500: codebook(st->v4[s], cid, c + 4, cend); break;
            case 0x2200: case 0x2300: case 0x2600: case 0x2700: codebook(st->v1[s], cid, c + 4, cend); break;
            case 0x3000: case 0x3100: case 0x3200:
                if (vectors(st, (int)s, pic, cid & 0xFF00, c + 4, cend, y, y1) != VC_OK) { /* a short chunk: what came is drawn */ }
                break;
            }
            if (csize < 4) break;
            c += csize;
        }
        y += sh;
        p = send;
    }
    return VC_OK;
}

const struct vc_codec vc_cinepak = { VC_CVID, "Cinepak", sizeof(struct cv_state), 0, cv_open, cv_decode };
