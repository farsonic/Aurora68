/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/icy.h - the answer of an internet radio station, taken apart: the HTTP or ICY status line
 * and headers, the body undone from HTTP/1.1's chunks if the server sent it so, the ICY metadata
 * blocks taken out of the audio, and the station's "now playing" (StreamTitle) in Mac Roman.
 *
 * Portable C with no Toolbox calls, so the same file runs in PiPlay on the Mac (fed from MacTCP's
 * TCPRcv completion routine, at interrupt time: no allocation, no traps) and in a host test
 * (tools/icy_test.c, `make -C mac check`). Written on 21 November 2026 (docs/08 item 83).
 */
#ifndef PIPLAY_ICY_H
#define PIPLAY_ICY_H

enum { ICY_STATUS, ICY_HEADERS, ICY_BODY, ICY_DONE };          /* struct icy's state */
enum { ICY_K_AUDIO = 1, ICY_K_PLAYLIST, ICY_K_OTHER };         /* what the body is, from Content-Type */
#define ICY_EV_HEADERS 1u            /* icy_feed: the headers are complete (status, kind, ... are set) */
#define ICY_EV_TITLE   2u            /* a new StreamTitle is in title (title_seq was bumped) */
#define ICY_EV_ERROR   4u            /* the answer is not HTTP at all, or its chunks are damaged */
#define ICY_PLAYLIST   2048          /* the first bytes of a playlist (.pls, .m3u) kept for the caller */

struct icy {
	int state;
	int status;                      /* 200, 302 ...; 0 until the status line has come */
	int icy;                         /* the status line was "ICY 200 OK" (Shoutcast 1) */
	int kind;                        /* ICY_K_*, once the headers are complete */
	int chunked;                     /* Transfer-Encoding: chunked */
	unsigned long metaint;           /* icy-metaint: audio bytes between metadata blocks, 0 if none */
	unsigned long br;                /* icy-br: kbit/s, 0 if not said */
	char reason[40];                 /* the status line's words ("OK", "Found") */
	char name[64];                   /* icy-name, Mac Roman */
	char ctype[40];                  /* Content-Type, lower case, without its parameters */
	char location[200];              /* Location (a redirection) */
	/* the body */
	unsigned long audio_left;        /* audio bytes before the next metadata length byte */
	int meta_state;                  /* 0 audio, 1 in a metadata block */
	unsigned meta_len, meta_got;     /* the block's length (16 x its length byte), what has come */
	char meta[512];                  /* the block's start, where StreamTitle is */
	int ch_state;                    /* chunks: 0 the size line, 1 data, 2 the CRLF after the data, 3 the end */
	unsigned long ch_left;
	unsigned char pl[ICY_PLAYLIST];  /* a playlist's text */
	unsigned pl_len;
	/* what came */
	char title[128];                 /* the last StreamTitle, Mac Roman */
	unsigned long title_seq;         /* bumped for each new title */
	unsigned long meta_blocks;       /* metadata blocks with something in them */
	unsigned long audio_bytes;       /* audio handed on */
	unsigned long body_bytes;        /* body bytes before metadata and chunks were taken out */
	/* the header line being read */
	unsigned linelen;
	char line[256];
};

/* a sink for the audio: the bytes in order, metadata and chunk framing taken out */
typedef void (*icy_audio_fn)(const unsigned char *p, unsigned long n);

void icy_init(struct icy *s);
/* the next bytes from the connection; returns ICY_EV_* bits. Audio goes to audio() only when the
   status is 2xx and the body is audio; a playlist's text collects in pl. After the headers of an
   answer that is not 2xx, nothing more is looked at (state ICY_DONE). */
unsigned icy_feed(struct icy *s, const unsigned char *p, unsigned long n, icy_audio_fn audio);
/* a character of Unicode (or Latin-1) as a Mac Roman byte: a table for Latin-1, a few marks of
   punctuation, a question mark for the rest */
unsigned char icy_mac_char(unsigned long u);
/* text in UTF-8, or Latin-1 when it is not valid UTF-8, into Mac Roman (at most max - 1 bytes) */
void icy_to_mac(const unsigned char *in, unsigned long n, char *out, int max);
/* the first plain-http URL in a playlist's text (.pls "File1=", .m3u lines); 0 if none; *https set
   when the playlist names only https streams */
int icy_playlist_url(const unsigned char *t, unsigned n, char *out, int max, int *https);

#endif
