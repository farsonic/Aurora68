/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/media.h - FLAC, Ogg Vorbis and .m4a files for Aurora68 Play (media.c): what a file is, its
 * title and artist, and the bytes the sound card's decoders want (emu68/src/psmac/psmac_codec.h), from the
 * beginning or from a time to seek to. No Toolbox calls and no allocation: the file is read through the
 * caller's function, so tools/codec_test.c runs the same code on the host against the card's decoders,
 * and the code can be shared with the macOS Aurora68 Play later. Written on 22 November 2026 (docs/08
 * item 105).
 */
#ifndef PIPLAY_MEDIA_H
#define PIPLAY_MEDIA_H

/* the file: read n bytes at off into buf, the number read (fewer at its end), -1 on an error */
struct mf_io {
	long (*read)(void *u, unsigned long off, void *buf, long n);
	void *u;
	unsigned long size;
};

enum { MF_NONE, MF_FLAC, MF_OGG, MF_ALAC,
       MF_AAC, MF_OPUS, MF_OGGFLAC, MF_M4A_OTHER, MF_DAMAGED };   /* known, but not played: mf_why says why */

struct mf_tab { unsigned long off, n; unsigned long cache_at; unsigned long cache[32]; int cache_n; };
struct media {
	int kind;
	unsigned long rate, channels, bits;
	unsigned long frames;                /* in the whole file, 0 if not known */
	unsigned long length_ms;
	char title[100], artist[100], album[100];   /* Mac Roman, C strings */
	/* what the card is sent before the audio: pre (FLAC's STREAMINFO, ALAC's configuration), or
	   the file's own first bytes [hdr_off, hdr_off + hdr_len) (Ogg's three headers) */
	unsigned char pre[48];
	int pre_n;
	unsigned long hdr_off, hdr_len;
	unsigned long audio_off, audio_len;  /* FLAC: the frames; Ogg: the audio pages */
	unsigned long seek_off, seek_n;      /* FLAC: SEEKTABLE's points (18 bytes each) */
	/* ALAC: the sample tables of the 'alac' track */
	unsigned long frame_len;             /* frames per packet (stts' delta) */
	unsigned long npackets, fixed_size;  /* stsz: its count, and the size of every packet if not 0 */
	int co64;
	struct mf_tab stsz, stsc, stco;      /* stsc entries are 3 words: the cache holds words */
};

/* the stream to the card: where it is in the file */
struct mf_stream {
	int phase;                           /* 0 pre, 1 hdr, 2 audio, 3 the end */
	unsigned long at;                    /* bytes of the phase sent */
	unsigned long file_at;               /* audio: the file's offset of the next byte */
	/* ALAC: the packet being sent and where it is in the sample tables */
	unsigned long pkt, pkt_off, pkt_size, pkt_sent;
	unsigned long chunk, in_chunk, stsc_i, spc, next_first;
};

/* what the file is (head: its first got bytes, at least 4 KB of them if the file has them); 1 for
   MF_FLAC, MF_OGG and MF_ALAC; 0 if it is none of the three or one that is not played (m->kind and
   mf_why say which) */
int mf_identify(struct media *m, const struct mf_io *io, const unsigned char *head, long got);
const char *mf_why(const struct media *m);     /* why a known file is not played, in words */
const char *mf_name(const struct media *m);    /* "FLAC", "Ogg Vorbis", "Apple Lossless" */
/* the stream from ms into the file: *start_ms is where it really starts (a FLAC seek point, an ALAC
   packet, an Ogg page), for psmb_stream.start_ms; 0 on an error */
int mf_seek(struct media *m, struct mf_stream *s, const struct mf_io *io, unsigned long ms, unsigned long *start_ms);
/* the next bytes for the card, at most max of them into buf: the number, 0 at the end, -1 on an error */
long mf_next(struct media *m, struct mf_stream *s, const struct mf_io *io, unsigned char *buf, long max);

/* Vorbis comments ("TITLE=..."), as FLAC and Ogg Vorbis carry them: title and artist out of a comment
   block of n bytes (vendor string, count, then the comments), into Mac Roman */
void mf_vorbis_comments(struct media *m, const unsigned char *b, unsigned long n);
#endif
