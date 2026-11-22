/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/devices.h - Aurora68 Play as one of the music server's devices (devices.c): it says it is there
 * (/mac/announce, every 20 seconds), what it plays (/mac/state) and takes commands (/mac/commands, a long
 * poll), so the web panel or another player can say "play this on the SE". Also the library's generation,
 * which the announce answer carries: a new one means new albums, and the Library's lists are fetched again.
 * The server's side is tools/music-server (README, "Devices"). No Toolbox calls and no allocation: the
 * requests go out through library.c's one connection, and tools/devices_test.c runs this on the host; the
 * same code can later go into the macOS Aurora68 Play. Written on 22 November 2026 (docs/08 item 103).
 */
#ifndef PIPLAY_DEVICES_H
#define PIPLAY_DEVICES_H

enum { DV_NONE, DV_ANNOUNCE, DV_STATE, DV_COMMANDS };
#define DV_WAIT 15            /* seconds the server may hold a commands request: under MacTCP's 20-second receive timeout */

struct dv_state {             /* what plays, as /mac/state says it */
	int playing;
	unsigned long pos, len;   /* seconds */
	int vol;                  /* 0-100 */
	char tid[9];              /* a library track's id, or empty */
	char title[48], artist[40];
};
struct dv {
	char id[17];              /* made once (dv_make_id) and kept in the preferences */
	char name[64];            /* the Mac's name, Mac Roman */
	char version[8];
	int on;                   /* the server is known: announce, report, poll */
	int announced;            /* the server answered an announce */
	unsigned long every;      /* seconds between announces (the server's answer) */
	unsigned long gen;        /* the library's generation, 0 while not known */
	int gen_changed;          /* it moved since the last time it was known: the caller refreshes and clears it */
	unsigned long next_announce, next_poll, state_at;     /* in the caller's seconds */
	unsigned long sent_time;  /* when the last state report was taken */
	int state_dirty;
	struct dv_state st, sent;
	int failures;             /* requests in a row that failed: the next ones wait longer */
};

void dv_init(struct dv *d, const char *id, const char *name, const char *version);
void dv_start(struct dv *d, unsigned long now);       /* a server is known (again): announce at once */
void dv_stop(struct dv *d);
/* the request to make now (DV_NONE: nothing); its path into out (max bytes) */
int dv_want(struct dv *d, unsigned long now);
int dv_path(const struct dv *d, int what, char *out, int max);
/* its answer: status 0 if the request failed (no connection, no answer); 1 if the answer was read */
int dv_answer(struct dv *d, int what, int status, const char *body, long len, unsigned long now);
/* what plays now: a change of track, of playing, a seek or a new title makes a state report due */
void dv_set_state(struct dv *d, const struct dv_state *s, unsigned long now);
/* the library's generation from elsewhere (/mac/info's sixth field) */
void dv_set_generation(struct dv *d, unsigned long gen);

/* a commands answer, item by item: 'C' a command (n, action, position, volume: -1 when not given), then
   its 'T' tracks (tid, title, artist, secs) and an 'R' radio station (name, url) */
struct dv_item {
	int kind;
	long n, position;
	int volume;
	char action[12];
	char tid[9], title[48], artist[40];
	unsigned long secs;
	char name[48], url[200];
};
struct dv_iter { const char *b; long n, at; };
void dv_iter_init(struct dv_iter *it, const char *body, long len);
int dv_next_item(struct dv_iter *it, struct dv_item *out);      /* 1 while there are items */
/* the actions, by name */
enum { DA_UNKNOWN, DA_PLAY, DA_ADD, DA_NEXT, DA_PAUSE, DA_RESUME, DA_SKIP, DA_PREVIOUS, DA_SEEK, DA_VOLUME, DA_STOP };
int dv_action(const char *name);

/* /mac/devices, line by line */
struct dv_device { char id[65], name[48], kind[10]; int online; char title[48], artist[40]; };
int dv_next_device(struct dv_iter *it, struct dv_device *out);

/* a new id: "mac-" and 12 letters and digits from two numbers that differ from Mac to Mac (the clock, the
   tick count, the mouse) */
void dv_make_id(char *id, unsigned long a, unsigned long b);
/* s into a URL's query, Mac Roman bytes as %XX; the end of what was written */
char *dv_escape(char *p, const char *s, int max);
#endif
