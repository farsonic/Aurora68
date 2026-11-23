/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/vdec/mpgindex.h - an MPEG-1 file indexed for QuickTime: where each coded picture starts, its type, the
 * sequence header, and the MP2 sound's bytes (docs/16-video-card.md "MPEG-1 in QuickTime", GitHub issue #25).
 *
 * Backstory (23 November 2026, docs/08 item 113). QuickTime 2.5 on a 68k has no MPEG at all. The Aurora68
 * Video Decoder extension's movie import component ('eat ', mac/vdec/import.c) makes a QuickTime movie of an
 * MPEG-1 file without copying it: the video track's samples are references into the file itself, one per
 * picture, decoded by the card ('mpg1', a fifth 'imdc'), and the MP2 sound becomes a sound track. This is the
 * part that reads the file. It is portable C with no Toolbox calls, so the same code runs in the import on
 * the Mac and in the host test (tools/mpg1_test.c, `make -C mac check`), which checks it against FFmpeg.
 *
 * What it does with the bytes it is given, in order, in pieces of any size:
 *  - a program stream (packs, the system header, PES packets: what FFmpeg's "-f mpeg" and every MPEG-1
 *    encoder of the 1990s wrote) is taken apart: the first video stream's payload goes to the video scanner,
 *    the first audio stream's payload to the caller (its MP2 frames, for the card to decode);
 *  - a bare video stream (.m1v, starting with a sequence header) goes to the video scanner as it is;
 *  - the video scanner finds the start codes. A picture's sample begins at the sequence header or group of
 *    pictures header in front of it, if there is one, else at its own picture start code, as a file offset
 *    (in a program stream that is in the middle of a packet: the card's decoder finds the packet's end by
 *    itself, see psmac_plmpeg.c); its type (I, P, B) comes from the two bytes after the start code.
 * mi_order then turns the pictures, which are in coding order, into QuickTime's samples in display order
 * (the B-picture rule below).
 */
#ifndef MPGINDEX_H
#define MPGINDEX_H
#include <stdint.h>

enum { MI_UNKNOWN = 0, MI_ES = 1, MI_PS = 2 };          /* what the file is (also the ImageDescription's) */
enum { MI_OTHER = 0, MI_I = 1, MI_P = 2, MI_B = 3 };     /* a picture's type, as packed in an index entry */

#define MI_SEQ_MAX  140u                                  /* a sequence header with both quantiser matrices: 4 + 8 + 128 */
#define MI_MAX_FILE (1u << 30)                            /* an index entry holds an offset below 1 GB */

struct mi_seq {                     /* the first sequence header */
    int have;
    uint32_t w, h, aspect, rate_code, bitrate;            /* bitrate in units of 400 bit/s, as coded */
    uint32_t len;                                         /* its bytes, from the start code */
    uint8_t bytes[MI_SEQ_MAX];
};
struct mi_audio {                   /* the first MP2 frame's header */
    int have;
    uint32_t layer, rate, channels, kbps;
};

typedef void (*mi_pic_fn)(void *user, uint32_t off, uint32_t type);
typedef void (*mi_aud_fn)(void *user, const uint8_t *p, uint32_t n);

struct mi {
    int kind;
    uint32_t pos;                   /* file offset of the next byte fed */
    mi_pic_fn on_pic; mi_aud_fn on_aud; void *user;
    /* the system layer */
    int st;                         /* the state (mpgindex.c) */
    uint32_t ph;                    /* in a PES header: which part of it */
    uint32_t sr;                    /* the last four bytes, as one word */
    uint8_t hdr[16]; uint32_t hn, need;
    uint32_t left;                  /* bytes left in the current packet */
    uint32_t skip;                  /* header bytes still to skip */
    int cur;                        /* 0 skipped, 1 the video stream, 2 the audio stream */
    int vid_id, aud_id;             /* the streams chosen (the first of each), 0 none yet */
    uint32_t packets, vpackets, apackets;
    /* the video scanner */
    uint32_t vsr, vn;               /* its last four bytes, how many seen */
    uint32_t run_i[4], run_off[4];  /* the last four payload runs: elementary stream index and file offset of their first byte */
    uint32_t unit, have_unit;       /* a sequence or GOP header waiting for its picture */
    uint32_t pic_off, pic_need;     /* the picture whose two header bytes are awaited */
    uint8_t pic_b0;
    uint32_t seq_need;              /* sequence header bytes still to capture */
    uint32_t video_end;             /* file offset just after the last video byte */
    uint32_t npics, ni, np, nb, nother;
    struct mi_seq seq;
    /* the audio */
    uint32_t asr; uint32_t audio_bytes;
    struct mi_audio aud;
};

void mi_init(struct mi *m, mi_pic_fn on_pic, mi_aud_fn on_aud, void *user);
void mi_feed(struct mi *m, const uint8_t *p, uint32_t n);
/* the frame rate of a sequence header's code as a ratio: num / den frames a second (0 if unknown) */
void mi_rate(uint32_t code, uint32_t *num, uint32_t *den);

/* QuickTime's samples in display order from the pictures in coding order. Each entry of pics is the
   picture's sample start (file offset) << 2 | its type; end is the file offset after the last picture.
   For every picture one sample is given to emit, in display order, as a byte range of the file:
     an I- or P-picture's own range, when it is shown (after the B-pictures that follow it in the file);
     the first B-picture after a reference picture as the reference's range and its own together (the
     reference must be decoded before the B, and QuickTime 2.5 shows samples in the order it is given them);
     any other B-picture's own range.
   A reference picture's range is thus given twice: once in front of the first B after it, and once as its own
   sample, which the card's decoder recognises as the picture it already decoded and simply shows (it is
   decoded only if it is not: after a seek to it, a key frame). I-pictures' own samples are the sync samples.
   emit's flags: MI_SYNC for a sync sample, MI_BONLY for a sample of a B-picture alone (nothing else needs it).
   Returns the samples emitted. */
#define MI_SYNC  1
#define MI_BONLY 2
typedef void (*mi_emit_fn)(void *user, uint32_t off, uint32_t size, int flags);
uint32_t mi_order(const uint32_t *pics, uint32_t n, uint32_t end, mi_emit_fn emit, void *user);

/* QuickTime 2.5 does not decode from the preceding key frame when it jumps (a click in the controller, a frame
   it drops while playing): it hands the decompressor the sample of the time asked for and nothing else. So the
   'mpg1' decompressor catches the card up itself: mi_catchup gives the samples (by index, offset and size) that
   must be decoded, without being drawn, before sample k (0-based, display order) when the card holds none of
   them: from the sync sample at or before k up to k - 1, leaving out the B-pictures alone. Returns how many it
   gave; none when k is a sync sample. */
struct mi_sample { uint32_t index, off, size; };
uint32_t mi_catchup(const uint32_t *pics, uint32_t n, uint32_t end, uint32_t k, struct mi_sample *out, uint32_t max);
/* sample k's range and flags (0 if there is no such sample) */
int mi_sample_at(const uint32_t *pics, uint32_t n, uint32_t end, uint32_t k, uint32_t *off, uint32_t *size, int *flags);

/* the 'mpg1' ImageDescription for the movie's video track, with the card's extension atom ('mpgh': the
   version, the file's kind, the video stream id, the sequence header); returns its size (idSize), 0 if
   cap is too small */
uint32_t mi_image_description(uint8_t *d, uint32_t cap, const struct mi *m);
#define MI_DESC_MAX (86u + 12u + MI_SEQ_MAX)

/* the decompressor's index, a second extension atom ('mpgx') appended to the description by the import: where
   the film is (an FSSpec: vRefNum, parID, name; 70 bytes), its size, the end of its last picture, and the
   pictures in coding order as mi_order takes them (big-endian, the 68k's own order). At most MI_INDEX_MAX
   pictures (beyond, no index: QuickTime's jumps then show what the card makes of the sample alone). */
#define MI_INDEX_MAX 32768u
#define MI_INDEX_HDR (8u + 70u + 2u + 12u)
uint32_t mi_index_atom(uint8_t *d, uint32_t cap, const uint8_t *fsspec, uint32_t file_size, uint32_t end, const uint32_t *pics, uint32_t n);
/* the index in a description, 0 if it has none: *fsspec (70 bytes), *pics (big-endian, n of them) */
int mi_index_find(const uint8_t *desc, uint32_t len, const uint8_t **fsspec, uint32_t *file_size, uint32_t *end,
                  const uint8_t **pics, uint32_t *n);

#endif
