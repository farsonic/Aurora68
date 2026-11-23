/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_vcodec.c - the frame decoders' list and the colour tables they share (psmac_vcodec.h, docs/16).
 *
 * Backstory (22 November 2026, docs/08 item 110). The decoders are one file each (psmac_cinepak.c,
 * psmac_rpza.c, psmac_qtrle.c, psmac_smc.c); this is what they have in common: which FourCC is which
 * decoder, and the colour tables of indexed images. An indexed QuickTime image (Graphics, and Animation
 * at 1 to 8 bits) carries its table in its ImageDescription (clutID 0: a ColorTable after the 86 bytes)
 * or names one of the Mac's standard tables by its depth. Those tables are the System's 'clut' resources
 * 1, 2, 4 and 8, which every colour Mac has; they are written out below from what they are (white to black,
 * the 6 x 6 x 6 cube and its ramps), not copied from Apple's files. The grey depths 33 to 40 are 1, 2, 4 and
 * 8 bits of grey, white at index 0 as everywhere on the Mac.
 */
#include "psmac_vcodec.h"

extern const struct vc_codec vc_cinepak, vc_rpza, vc_qtrle, vc_smc;
static const struct vc_codec *const codecs[] = { &vc_cinepak, &vc_rpza, &vc_qtrle, &vc_smc, &vc_mpg1 };
const uint32_t vc_fourccs[] = { VC_CVID, VC_RPZA, VC_RLE, VC_SMC, VC_MPG1, VC_MP2, 0 };

const struct vc_codec *vc_find(uint32_t fourcc)
{
    for (unsigned i = 0; i < sizeof codecs / sizeof codecs[0]; i++)
        if (codecs[i]->fourcc == fourcc) return codecs[i];
    return 0;
}
int vc_wants_idx(const struct vc_codec *c, uint32_t depth)
{
    if (c->needs_idx) return 1;
    return c->fourcc == VC_RLE && (depth <= 8 || (depth >= 33 && depth <= 40));
}

/* the Mac's standard 16 colours ('clut' 4), as the 4-bit screens of every colour Mac show them */
static const uint32_t std16[16] = {
    0xFFFFFF, 0xFCF305, 0xFF6402, 0xDD0806, 0xF20884, 0x4600A5, 0x0000D4, 0x02ABEA,
    0x1FB714, 0x006411, 0x562C05, 0x90713A, 0xC0C0C0, 0x808080, 0x404040, 0x000000 };
/* the ramps of the 256-colour table: after the cube, ten reds, greens, blues and greys, each without the
   cube's own levels (00 33 66 99 CC FF) */
static const uint8_t ramp[10] = { 0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11 };

static uint32_t grey(uint32_t v) { return v << 16 | v << 8 | v; }
uint32_t vc_standard_palette(uint32_t *pal, uint32_t depth)
{
    uint32_t i;
    switch (depth) {
    case 1: case 33: pal[0] = 0xFFFFFF; pal[1] = 0; return 2;
    case 2: pal[0] = 0xFFFFFF; pal[1] = 0xACACAC; pal[2] = 0x555555; pal[3] = 0; return 4;
    case 34: for (i = 0; i < 4; i++) pal[i] = grey(255 - i * 85); return 4;
    case 4: for (i = 0; i < 16; i++) pal[i] = std16[i]; return 16;
    case 36: for (i = 0; i < 16; i++) pal[i] = grey(255 - i * 17); return 16;
    case 40: for (i = 0; i < 256; i++) pal[i] = grey(255 - i); return 256;
    case 8:
        for (i = 0; i < 215; i++) {
            uint32_t r = 5 - i / 36, g = 5 - i / 6 % 6, b = 5 - i % 6;
            pal[i] = (r * 0x33) << 16 | (g * 0x33) << 8 | b * 0x33;
        }
        for (i = 0; i < 10; i++) {
            pal[215 + i] = (uint32_t)ramp[i] << 16;
            pal[225 + i] = (uint32_t)ramp[i] << 8;
            pal[235 + i] = ramp[i];
            pal[245 + i] = grey(ramp[i]);
        }
        pal[255] = 0;
        return 256;
    }
    return 0;
}

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

/* ImageDescription: idSize at 0, depth at 82, clutID at 84; a ColorTable at 86 when clutID is 0 (ctSeed,
   ctFlags, ctSize = entries - 1, then { value, red, green, blue } of 16 bits each) */
uint32_t vc_palette_from_desc(struct vc_picture *p, const uint8_t *d, uint32_t len)
{
    uint32_t depth = len >= 86 ? be16(d + 82) : p->depth, n = 0, i;
    for (i = 0; i < 256; i++) p->pal[i] = 0;
    if (len >= 86 + 8 && be16(d + 84) == 0) {
        uint32_t size = be16(d + 86 + 6) + 1, flags = be16(d + 86 + 4);
        const uint8_t *e = d + 86 + 8;
        if (size > 256) size = 256;
        for (i = 0; i < size && e + 8 <= d + len; i++, e += 8) {
            uint32_t slot = (flags & 0x8000) ? i : be16(e);   /* ctFlags bit 15: a device table, values are not indices */
            if (slot < 256) p->pal[slot] = (be16(e + 2) >> 8) << 16 | (be16(e + 4) >> 8) << 8 | be16(e + 6) >> 8;
            if (slot + 1 > n) n = slot + 1;
        }
    }
    if (!n) n = vc_standard_palette(p->pal, depth);
    p->npal = n;
    return n;
}
