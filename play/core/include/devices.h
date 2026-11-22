/* SPDX-License-Identifier: MPL-2.0
 *
 * play/core/include/devices.h - an Aurora68 Play as one of the music server's devices (devices.c): it says it is
 * there (/mac/announce, every 20 seconds), what it plays (/mac/state) and takes commands (/mac/commands, a long
 * poll), so the web panel or another player can say "play this on the SE". Also the library's generation,
 * which the announce answer carries: a new one means new albums, and the lists are fetched again.
 *
 * The server's side is tools/music-server (README, "Devices"). No allocation and no calls out: a Play asks
 * dv_want what to send and when, makes the request its own way (the classic Play on its Library's one MacTCP
 * connection, the macOS Play with URLSession) and hands the answer to dv_answer. Written on 22 November 2026
 * for the classic Play (docs/08 item 105) and moved into the core the same day, where the macOS Play uses it
 * too (item 107): the kind, the encoding, the long poll's wait and the refresh while playing are now the
 * Play's to choose (dv_init, and the fields after it).
 */
#ifndef A68_DEVICES_H
#define A68_DEVICES_H
#include "proto.h"

enum { DV_NONE, DV_ANNOUNCE, DV_STATE, DV_COMMANDS };
#define DV_WAIT 15            /* seconds the server may hold a commands request: under MacTCP's 20-second receive timeout */

struct dv_state {             /* what plays, as /mac/state says it */
	int playing;
	unsigned long pos, len;   /* seconds */
	int vol;                  /* 0-100, -1 not known */
	char tid[12];             /* a library track's id (the server names it from its catalogue), or empty */
	char title[100], artist[64];   /* else what plays: a station's title, a file's tags */
};
struct dv {
	char id[65];              /* made once (dv_make_id) and kept in the preferences */
	char name[64];            /* the Mac's name, in the client's encoding */
	char version[8];
	char kind[8];             /* "classic" or "macos" */
	struct a68_client client; /* Mac Roman (the classic Play) or UTF-8 (the macOS one) */
	int wait;                 /* seconds a commands request may be held (DV_WAIT; URLSession waits longer: 20) */
	unsigned long refresh;    /* seconds between reports while playing even without a change (0: none) */
	int on;                   /* the server is known: announce, report, poll */
	int announced;            /* the server answered an announce */
	unsigned long every;      /* seconds between announces (the server's answer) */
	unsigned long gen;        /* the library's generation, 0 while not known */
	int gen_changed;          /* it moved since the last time it was known: the caller refreshes and clears it */
	struct a68_gen watch;
	unsigned long next_announce, next_poll, state_at;     /* in the caller's seconds */
	unsigned long sent_time;  /* when the last state report was taken */
	int state_dirty;
	struct dv_state st, sent;
	int failures;             /* requests in a row that failed: the next ones wait longer */
};

/* kind: "classic" or "macos" */
void dv_init(struct dv *d, const char *id, const char *name, const char *version, const char *kind);
void dv_start(struct dv *d, unsigned long now);       /* a server is known (again): announce at once */
void dv_stop(struct dv *d);
/* the request to make now (DV_NONE: nothing); its path into out (max bytes) */
int dv_want(struct dv *d, unsigned long now);
int dv_path(const struct dv *d, int what, char *out, long max);
/* its answer: status 0 if the request failed (no connection, no answer); 1 if the answer was read. The body is
   taken apart in place (proto.h): a byte after it must be writable */
int dv_answer(struct dv *d, int what, int status, char *body, long len, unsigned long now);
/* what plays now: a change of track, of playing, a seek or a new title makes a state report due */
void dv_set_state(struct dv *d, const struct dv_state *s, unsigned long now);
/* is a state report due now? (for a Play that announces and polls on connections of their own, as the macOS
   Play does, and asks dv_want only about the state) */
int dv_state_due(const struct dv *d, unsigned long now);
/* the library's generation from elsewhere (/mac/info's sixth field) */
void dv_set_generation(struct dv *d, unsigned long gen);

/* a new id: "mac-" and 12 letters and digits from two numbers that differ from Mac to Mac (the clock, the
   tick count, the mouse; a UUID's bytes) */
void dv_make_id(char *id, unsigned long a, unsigned long b);
/* an id the server takes: 1 to 64 letters, digits, '-' and '_', and not "live" */
int dv_valid_id(const char *id);
#endif
