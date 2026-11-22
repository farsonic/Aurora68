/* SPDX-License-Identifier: MPL-2.0
 *
 * play/core/include/proto.h - the music server's protocol as both Plays speak it: the requests (their paths)
 * and the answers taken apart.
 *
 * One wire format (22 November 2026, docs/08 item 107): the server's /mac/ text interface
 * (tools/music-server/README.md, "Aurora68 Play's Library" and "Devices"), which was made for a 68000 and is
 * read here with a loop: one record a line ended by a Return, fields separated by tabs, lists with a first
 * line "A68M <tab> 1 <tab> KIND <tab> TOTAL <tab> FROM <tab> COUNT" and ?from=N&count=M for a page. Two
 * parameters make it serve the macOS Play as well, and the classic Play sends neither, so its answers are
 * what they were:
 *   enc=utf8   the answer in UTF-8 and the request's words read as UTF-8 (else Mac Roman both ways). The core
 *              only moves bytes; each Play turns them into its own text at its edge.
 *   full=1     the longer records: an album's artist id and folder, a track's path (the original file, which
 *              the macOS Play plays through /file/), album title and folder, a device's whole state, the
 *              path and album of a command's tracks, how often to announce.
 * The answers are taken apart in place: the body's tabs and Returns become NULs and each field is a pointer
 * into it, so a record needs no copy and has no length the core decides (a path may be long); the body must
 * be writable, with one more byte after its end (a NUL goes there), and the fields last as long as it does.
 * Missing fields are empty strings, never null.
 */
#ifndef A68_PROTO_H
#define A68_PROTO_H

/* what a Play asks for: its text encoding and whether it wants the longer records */
struct a68_client { int utf8; int full; };

/* ---- records --------------------------------------------------------------------------------------------- */
struct a68_rec { char *b; long n, at; };
void a68_rec_init(struct a68_rec *r, char *body, long len);
/* the next record's fields, in place: f[0] .. f[maxf - 1] (those not there point at ""); how many there were,
   -1 at the end. Empty lines are passed over */
int a68_rec_next(struct a68_rec *r, char **f, int maxf);

/* a list's first line: 1 if it is the server's ("A68M", version 1), its kind, total, from and count */
struct a68_head { char kind[12]; long total, from, count; };
int a68_read_head(struct a68_rec *r, struct a68_head *h);
/* the next page's "from" after this one, -1 if this was the last */
long a68_next_from(const struct a68_head *h);

/* /mac/info: name, version, artists, albums, tracks, generation (servers of 22 November 2026 on), every (full=1) */
struct a68_info { const char *name, *version; unsigned long artists, albums, tracks, generation, every; };
/* /mac/artists: id, name, albums, tracks */
struct a68_artist { const char *id, *name; unsigned long albums, tracks; };
/* /mac/albums: id, title, artist, tracks, seconds; full=1: artist_id, folder (empty from an older server) */
struct a68_album { const char *id, *title, *artist; unsigned long tracks, secs; const char *artist_id, *folder; int full; };
/* /mac/tracks: id, number, title, artist, seconds, album; full=1: path, album_title, folder */
struct a68_track { const char *id; unsigned long number; const char *title, *artist; unsigned long secs; const char *album;
                   const char *path, *album_title, *folder; int full; };
/* /mac/search: kind ('A' artist, 'L' album, 'T' track), id, text, sub; a track also album and seconds */
struct a68_hit { char kind; const char *id, *text, *sub, *album; unsigned long secs; };
/* /mac/radio: group ("By country/Austria": a submenu inside a submenu), name, url */
struct a68_radio { const char *group, *name, *url; };
/* /mac/devices: id, name, kind, online, title, artist; full=1 the rest of the state (positions in ms; at and
   last_seen the server's clock in seconds) */
struct a68_device {
	const char *id, *name, *kind; int online; const char *title, *artist;
	int full, playing; unsigned long position_ms, length_ms; int volume; const char *album, *folder;
	unsigned long at, last_seen; int is_live; const char *version; unsigned long listeners;
};
/* /mac/commands: 'C' a command (n, action, position, volume: -1 when not given, shuffle), then its 'T' tracks
   (tid, title, artist, seconds; full=1: path, album) and an 'R' radio station (name, url) */
struct a68_cmd {
	int kind;
	long n, position; int volume, shuffle; const char *action;
	const char *tid, *title, *artist; unsigned long secs; const char *path, *album;
	const char *name, *url;
};
/* /mac/now, the Live channel: 'N' (name, playing, paused, repeat, pos, changes, listeners), 'C' the song heard
   (path, folder, title, elapsed and length in ms), 'Q' each queued song (path, title), 'L' each listener (who,
   since: the server's clock) */
struct a68_now {
	int kind;
	const char *name; int playing, paused, repeat; long pos; unsigned long changes, listeners;
	const char *path, *folder, *title; unsigned long elapsed_ms, length_ms;
	const char *who; unsigned long since;
};
int a68_info_of(struct a68_rec *r, struct a68_info *o);       /* 1 if there was one */
int a68_next_artist(struct a68_rec *r, struct a68_artist *o);  /* 1 while there are records with an id */
int a68_next_album(struct a68_rec *r, struct a68_album *o);
int a68_next_track(struct a68_rec *r, struct a68_track *o);
int a68_next_hit(struct a68_rec *r, struct a68_hit *o);
int a68_next_radio(struct a68_rec *r, struct a68_radio *o);
int a68_next_device(struct a68_rec *r, struct a68_device *o);
int a68_next_cmd(struct a68_rec *r, struct a68_cmd *o);
int a68_next_now(struct a68_rec *r, struct a68_now *o);
/* an answer that is an error: its words (the first field of the first record after an "error" head, or of the
   first line of the devices' plain answers) */
const char *a68_error_text(char *body, long len);

/* the commands' and devices' actions, by name */
enum { A68_ACT_UNKNOWN, A68_ACT_PLAY, A68_ACT_ADD, A68_ACT_NEXT, A68_ACT_PAUSE, A68_ACT_RESUME, A68_ACT_SKIP,
       A68_ACT_PREVIOUS, A68_ACT_SEEK, A68_ACT_VOLUME, A68_ACT_STOP,
       /* the Live channel's queue only */
       A68_ACT_JUMP, A68_ACT_REMOVE, A68_ACT_CLEAR, A68_ACT_SHUFFLE, A68_ACT_REPEAT };
int a68_action(const char *name);
const char *a68_action_name(int act);

/* a cover: "A68ART 1 W H DEPTH ROWBYTES" (or "A68ARTS 1 N W H DEPTH ROWBYTES"), then the bits: its numbers into
   v (nv of them) and where the bits start; 1 if it is one */
int a68_art(const char *b, long n, const char *magic, long *v, int nv, long *data_at);

/* ---- the requests: paths, with enc and full as the client asks; 1 if the path fitted in out --------------- */
int a68_path_info(const struct a68_client *c, char *out, long max);
int a68_path_artists(const struct a68_client *c, long from, long count, char *out, long max);
/* artist "" or 0: every album */
int a68_path_albums(const struct a68_client *c, const char *artist, long from, long count, char *out, long max);
/* an album's tracks, or (album empty) an artist's */
int a68_path_tracks(const struct a68_client *c, const char *album, const char *artist, long from, long count, char *out, long max);
int a68_path_search(const struct a68_client *c, const char *words, long from, long count, char *out, long max);
int a68_path_art(const struct a68_client *c, const char *album, int size, int depth, char *out, long max);
/* the one-bit covers of n albums at once (the albums in view): /mac/arts?w=S&h=S&albums=ID,ID,... */
int a68_path_arts(const struct a68_client *c, const char *const *ids, int n, int size, char *out, long max);
int a68_path_radio(const struct a68_client *c, char *out, long max);
int a68_path_devices(const struct a68_client *c, char *out, long max);
int a68_path_now(const struct a68_client *c, char *out, long max);
/* "play this there" (to: a device's id, or "live"): what may be 0, the numbers -1 when not given */
int a68_path_command(const struct a68_client *c, const char *to, int action, const char *what, int shuffle,
                     long position, int volume, long index, int on, char *out, long max);
/* one track as MP3 from start seconds, as the classic Play plays it: /t/ID?start=S */
int a68_path_stream(const char *tid, unsigned long start, char *out, long max);
/* the server's "what": f:FOLDER (an album; "" the whole library), t:PATH (a song), r:N (radio station N) */
int a68_what_folder(const char *folder, char *out, long max);
int a68_what_track(const char *path, char *out, long max);
int a68_what_radio(unsigned long n, char *out, long max);

/* ---- the library's generation: a change means new albums (or fewer), so the lists are fetched again ----- */
struct a68_gen { unsigned long seen; };
/* 1 when g differs from the generation seen before (never for the first one known; 0 is "not known") */
int a68_gen_changed(struct a68_gen *w, unsigned long g);

/* a position on a bar: x of w pixels (from its left) as a time in a song of len (any unit) */
unsigned long a68_bar_time(long x, long w, unsigned long len);
/* where another player's song is now: its position, moved on by the seconds since the server heard it while
   it plays, never past its length (0: not known) */
unsigned long a68_estimate_ms(unsigned long position_ms, unsigned long length_ms, int playing, unsigned long at,
                              unsigned long now);
#endif
