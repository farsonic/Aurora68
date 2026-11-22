/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_codec.h — the stream channel's decoders for FLAC, Ogg Vorbis and Apple Lossless (ALAC), behind
 * one small interface that psmac_audio.c calls on core 1 and tools/codec_test.c calls on the host.
 *
 * Backstory (22 November 2026, docs/08 item 103). Aurora68 Play played MP3 (minimp3), AIFF and WAV on the
 * sound card's stream channel; the owner's music is mostly FLAC and .m4a, and they asked for those. The
 * Macintosh hands the card bytes through the stream ring (psmb.h) and the card decodes them, as it does
 * MP3. What the Mac sends, per format (Aurora68 Play's half is mac/piplay/media.c):
 *
 *  - PSMB_STR_FLAC: "fLaC" and the STREAMINFO block alone (marked the last), then the file's frames from
 *    where play starts (its first frame, or the one a seek chose). The Mac drops the other metadata
 *    blocks, which may hold a cover picture of megabytes. Decoded by dr_flac (third_party/dr_flac).
 *  - PSMB_STR_OGG: the file's first pages up to the first audio page (the identification, comment and
 *    setup headers), then the pages from where play starts. Decoded by stb_vorbis's pushdata interface
 *    (third_party/stb_vorbis); a stream that starts later than the beginning (psmb_stream.start_ms not 0)
 *    is flushed after its headers, so the decoder looks for the next whole page itself.
 *  - PSMB_STR_ALAC: "alac", a 32-bit length and the decoder's 24-byte configuration (the ALACSpecificConfig
 *    of the file's 'alac' sample description), then each packet as a 32-bit length and its bytes. The Mac
 *    reads the MP4 container (its sample tables may be at the end of the file, where the card's sequential
 *    stream cannot look). Decoded by psmac_alac.c, written for this project.
 *
 * All numbers in these streams are big-endian. Output is 16-bit stereo at the stream's own rate: mono is
 * doubled, more than two channels give the first two, and deeper samples are cut to 16 bits (the mixer
 * is 16-bit). Memory comes from the caller's allocator (the kernel's TLSF on the card).
 */
#ifndef PSMAC_CODEC_H
#define PSMAC_CODEC_H
#include <stdint.h>
#include <stddef.h>

/* the bytes the Mac has sent: a ring of a power of two, positions counted for ever */
struct pc_src {
    const uint8_t *buf;
    uint32_t mask;                            /* the ring's size less one */
    uint32_t head, tail;                      /* the decoder moves tail */
    int all_in;                               /* the Mac has sent everything: no more bytes will come */
};
struct pc_alloc {
    void *(*alloc)(size_t n);
    void (*release)(void *p);
};
enum { PC_MORE = 0, PC_END = -1, PC_ERROR = -2 };

struct pcodec {
    uint32_t format;                          /* PSMB_STR_FLAC, _OGG or _ALAC */
    int resume;                               /* the stream starts after the beginning (a seek) */
    uint32_t rate, channels, bits;            /* the stream's, once known (rate 0 before) */
    uint64_t total;                           /* frames in the whole file, 0 if not known */
    int64_t base;                             /* the file's frame number of the first frame given out, -1 if not known */
    uint64_t out;                             /* frames given out */
    const char *error;                        /* PC_ERROR: why, for the log */
    const struct pc_alloc *mem;
    void *priv;                               /* the decoder's own */
};

/* a new stream of this format (psmb.h's numbers); 0 if the format is not one of these */
int pc_open(struct pcodec *c, uint32_t format, int resume, const struct pc_alloc *mem);
/* one slice of work: up to max frames of 16-bit stereo into out (2 * max int16s). The number of frames,
   PC_MORE (0) if more bytes are needed first, PC_END at the end, PC_ERROR (c->error says why) */
int pc_decode(struct pcodec *c, struct pc_src *s, int16_t *out, int max);
void pc_close(struct pcodec *c);

/* the decoders, for pc_open and pc_decode */
int pc_flac_open(struct pcodec *c);
int pc_flac_decode(struct pcodec *c, struct pc_src *s, int16_t *out, int max);
void pc_flac_close(struct pcodec *c);
int pc_ogg_open(struct pcodec *c);
int pc_ogg_decode(struct pcodec *c, struct pc_src *s, int16_t *out, int max);
void pc_ogg_close(struct pcodec *c);
int pc_alac_open(struct pcodec *c);
int pc_alac_decode(struct pcodec *c, struct pc_src *s, int16_t *out, int max);
void pc_alac_close(struct pcodec *c);

/* helpers shared by the three */
static inline uint32_t pc_avail(const struct pc_src *s) { return s->head - s->tail; }
static inline uint8_t pc_peek(const struct pc_src *s, uint32_t k) { return s->buf[(s->tail + k) & s->mask]; }
uint32_t pc_read(struct pc_src *s, void *dst, uint32_t n);    /* up to n bytes taken; the number */
static inline int16_t pc_clip16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
#endif
