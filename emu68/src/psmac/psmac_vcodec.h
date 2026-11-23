/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_vcodec.h - the video decoder card's frame decoders: QuickTime's own compressed formats of the
 * 1990s decoded one picture at a time, in portable C (docs/16-video-card.md, GitHub issue #13).
 *
 * Backstory (22 November 2026, docs/08 item 110). The owner asked for the video card to be "an in-box
 * video processing card that integrates into QuickTime", with nothing outside the Mac and the Pi. A 68000
 * decodes Cinepak at a few frames a second; a Pi core decodes it in a millisecond or two. QuickTime has
 * the extension point hardware cards of the time used: a decompressor component ('imdc') per compression
 * type, called with one compressed picture and a destination. So the Mac's component hands each picture
 * to the card, and these decoders turn it into pixels:
 *   'cvid' Cinepak (SuperMac/Radius, 1991): vector quantisation of 4 x 4 blocks, codebooks kept between
 *          pictures, a picture in horizontal strips;
 *   'rpza' Apple Video ("Road Pizza", 1991): 16-bit colour, 4 x 4 blocks, runs, skips, four-colour blocks;
 *   'rle ' Animation (1991): run lengths per line at 1 to 32 bits a pixel, lines that did not change left;
 *   'smc ' Graphics (1991): 8 bits a pixel, 4 x 4 blocks, caches of recent two-, four- and eight-colour
 *          sets.
 * Each was written here from the formats' public descriptions (the MultimediaWiki pages and the
 * Cinepak description by Tim Ferguson); none comes from another decoder's code. FFmpeg is used on the
 * host only, to make test movies and to decode them for comparison (tools/vcodec_test.sh).
 *
 * The decoders know nothing of the Mac, the kernel or the card. They decode into a picture of their own
 * (32-bit 0x00RRGGBB, and for the 8-bit formats the colour indices too), kept from one picture to the
 * next: these formats leave unchanged blocks and lines out, and the picture they were left out of is
 * here, whatever the Mac did with the last one (a window moved, a frame decoded and not drawn). The card
 * (psmac_vframes.c) then copies the picture into the Mac's pixel map at that map's depth.
 *
 * Nothing here allocates or includes a header beyond stdint; the caller gives the memory.
 */
#ifndef PSMAC_VCODEC_H
#define PSMAC_VCODEC_H
#include <stdint.h>

#define VC_FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define VC_CVID VC_FOURCC('c', 'v', 'i', 'd')
#define VC_RPZA VC_FOURCC('r', 'p', 'z', 'a')
#define VC_RLE  VC_FOURCC('r', 'l', 'e', ' ')
#define VC_SMC  VC_FOURCC('s', 'm', 'c', ' ')

#define VC_MAX_W 1024u                       /* QuickTime of the period: 640 x 480 is large; the card's buffers */
#define VC_MAX_H 768u

enum { VC_OK = 0, VC_E_DATA = 1, VC_E_DEPTH = 2, VC_E_SIZE = 3 };

/* the picture a session decodes into, kept between pictures */
struct vc_picture {
    uint32_t w, h;                           /* the image's size (its ImageDescription) */
    uint32_t *rgb;                           /* w x h, 0x00RRGGBB, row after row */
    uint8_t *idx;                            /* w x h colour indices (smc, 8-bit and lower rle), else 0 */
    uint32_t pal[256];                       /* the image's colour table, 0x00RRGGBB */
    uint32_t npal;                           /* its entries (0 for direct colour) */
    uint32_t depth;                          /* the ImageDescription's depth: 1-32, 33-40 grey */
    /* what the last picture changed, [left, right) x [top, bottom): the caller empties it (left = w, top = h,
       right = bottom = 0) before a picture, the decoder widens it over every block or run it writes, and the
       card then redraws only that part when the destination still holds the picture before */
    uint32_t changed_left, changed_top, changed_right, changed_bottom;
};
static inline void vc_touch(struct vc_picture *p, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (x1 > p->w) x1 = p->w;
    if (y1 > p->h) y1 = p->h;
    if (x0 >= x1 || y0 >= y1) return;
    if (x0 < p->changed_left) p->changed_left = x0;
    if (y0 < p->changed_top) p->changed_top = y0;
    if (x1 > p->changed_right) p->changed_right = x1;
    if (y1 > p->changed_bottom) p->changed_bottom = y1;
}
static inline void vc_untouched(struct vc_picture *p)
{
    p->changed_left = p->w; p->changed_top = p->h; p->changed_right = p->changed_bottom = 0;
}

struct vc_codec {
    uint32_t fourcc;
    const char *name;
    uint32_t state_size;                     /* bytes of state the caller provides, zeroed */
    int needs_idx;                           /* the picture's idx plane is wanted (the depth decides for rle) */
    int (*open)(void *state, struct vc_picture *p);     /* VC_OK, or VC_E_DEPTH for a depth it does not do */
    int (*decode)(void *state, struct vc_picture *p, const uint8_t *data, uint32_t len);
};

const struct vc_codec *vc_find(uint32_t fourcc);
/* the FourCCs decoded here, 0-terminated (for the card's list) */
extern const uint32_t vc_fourccs[];
/* does this codec want the idx plane at this depth? */
int vc_wants_idx(const struct vc_codec *c, uint32_t depth);

/* the colour table of an image: from an ImageDescription (big-endian, as QuickTime keeps it: 86 bytes and,
   when its clutID is 0, a ColorTable after them), or QuickTime's standard table for the depth (1, 2, 4 and
   8 bits; 33-40 the grey ramps). Returns the entries set. */
uint32_t vc_palette_from_desc(struct vc_picture *p, const uint8_t *desc, uint32_t len);
uint32_t vc_standard_palette(uint32_t *pal, uint32_t depth);

#endif
