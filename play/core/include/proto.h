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
 *
 * Music videos (22 November 2026, docs/08 item 109): a third parameter, videos=1, which the classic Play 1.4 and the
 * macOS Play 1.2 do not send, so their answers stay what they were. With it an album's or an artist's tracks are
 * followed by its videos as "V" records, an artist has a fifth field (its videos, and an artist with videos alone is
 * listed), a search finds videos, /mac/info adds every, the videos and what the server can do, a device says whether
 * it shows videos, and a command's videos come as "V" lines. /mac/videos lists them alone.
 *
 * Videos linked to songs (23 November 2026): with full=1 and videos=1 both (the macOS Play's; the classic Play sends
 * neither) a track's record ends with the id of its music video and an album's with how many videos it lists. Both
 * fields are last, so a reader that does not know them passes over them, and a record without them (an older server)
 * reads as none.
 */
#ifndef A68_PROTO_H
#define A68_PROTO_H

/* what a Play asks for: its text encoding and whether it wants the longer records */
struct a68_client { int utf8; int full; int videos; };

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

/* /mac/info: name, version, artists, albums, tracks, generation (servers of 22 November 2026 on), every (full=1 or
   videos=1); videos=1 also the videos and what the server does with them ("videos,remux,hls,transcode,mpeg1") */
struct a68_info { const char *name, *version; unsigned long artists, albums, tracks, generation, every, videos; const char *caps; };
/* /mac/artists: id, name, albums, tracks; videos=1 also videos */
struct a68_artist { const char *id, *name; unsigned long albums, tracks, videos; };
/* /mac/albums: id, title, artist, tracks, seconds; full=1: artist_id, folder (empty from an older server); full=1 and
   videos=1 (servers of 23 November 2026 on): the videos the album lists, its own and its songs' (0 from older ones) */
struct a68_album { const char *id, *title, *artist; unsigned long tracks, secs; const char *artist_id, *folder; int full;
                   unsigned long videos; };
/* /mac/tracks: id, number, title, artist, seconds, album; full=1: path, album_title, folder; full=1 and videos=1
   (servers of 23 November 2026 on): video, the id of the song's music video ("" when it has none, or from an older
   server, whose record ends at folder) */
struct a68_track { const char *id; unsigned long number; const char *title, *artist; unsigned long secs; const char *album;
                   const char *path, *album_title, *folder; int full; const char *video; };
/* a music video (videos=1): "V", id, title, artist, seconds, album (its id, or empty), width, height; full=1 also path
   (for /file/), album_title, folder and how the server sends it ("direct", "remux", "transcode") */
struct a68_video { const char *id, *title, *artist; unsigned long secs; const char *album; unsigned long width, height;
                   const char *path, *album_title, *folder, *how; int full; };
/* /mac/search: kind ('A' artist, 'L' album, 'T' track, 'V' video), id, text, sub; a track or a video also album and
   seconds */
struct a68_hit { char kind; const char *id, *text, *sub, *album; unsigned long secs; };
/* /mac/radio: group ("By country/Austria": a submenu inside a submenu), name, url */
struct a68_radio { const char *group, *name, *url; };
/* /mac/devices: id, name, kind, online, title, artist; full=1 the rest of the state (positions in ms; at and
   last_seen the server's clock in seconds) */
struct a68_device {
	const char *id, *name, *kind; int online; const char *title, *artist;
	int full, playing; unsigned long position_ms, length_ms; int volume; const char *album, *folder;
	unsigned long at, last_seen; int is_live; const char *version; unsigned long listeners;
	int video;                /* videos=1: it shows videos (the macOS Play); 0: their sound only (the classic Play) */
};
/* /mac/commands: 'C' a command (n, action, position, volume: -1 when not given, shuffle), then its 'T' tracks
   (tid, title, artist, seconds; full=1: path, album), 'V' videos with videos=1 (the same, and full=1 how, width,
   height) and an 'R' radio station (name, url) */
struct a68_cmd {
	int kind;
	long n, position; int volume, shuffle; const char *action;
	const char *tid, *title, *artist; unsigned long secs; const char *path, *album;
	const char *name, *url;
	const char *how; unsigned long width, height;
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
int a68_next_track(struct a68_rec *r, struct a68_track *o);    /* passes over "V" records */
int a68_next_video(struct a68_rec *r, struct a68_video *o);    /* passes over everything but "V" records */
/* a tracks answer with videos=1: 'T' (into t), 'V' (into v), 0 at the end */
int a68_next_item(struct a68_rec *r, struct a68_track *t, struct a68_video *v);
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

/* how a video is sent (a68_video.how) */
enum { A68_HOW_UNKNOWN, A68_HOW_DIRECT, A68_HOW_REMUX, A68_HOW_TRANSCODE };
int a68_video_how(const char *how);

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
/* music videos: an artist's or an album's (both empty: every one), /mac/videos */
int a68_path_videos(const struct a68_client *c, const char *artist, const char *album, long from, long count, char *out, long max);
/* a video's sound as MP3 from start seconds (/vaudio/ID?start=S: what the classic Play plays), its poster (/vthumb/ID),
   and the address a Mac's player opens: /video/ID for one sent as it is (the file, with Range), else
   /video/ID/index.m3u8 (HLS: AVFoundation plays no stream without a length) */
int a68_path_vaudio(const char *vid, unsigned long start, char *out, long max);
int a68_path_vthumb(const char *vid, char *out, long max);
int a68_path_video(const char *vid, int how, char *out, long max);
/* the video as an MPEG-1 program stream at w x h from start seconds, for the Aurora68 Video Decoder card (the
   classic Play 1.6, 23 November 2026, docs/08 item 114): /vmpeg/ID?w=W&h=H&rate=R&start=S. The server fits the
   picture into w x h and pads it black; its sizes are even, 64 to 1280 x 720 (a68_vmpeg_size makes them so). rate:
   kbit/s of video (servers of 23 November 2026 on; 0 leaves it out: the server's own, up to 2,500) */
int a68_path_vmpeg(const char *vid, unsigned w, unsigned h, unsigned rate, unsigned long start, char *out, long max);
/* the rate the classic Play asks for: about 0.2 bit a pixel a picture at 25 a second (w x h / 192 kbit/s: 400 at
   320 x 240, 300 at 320 x 180, 1,600 at 640 x 480), 150 to 2,500. The server's own is some seven times that at 320 x
   240, which the testbed's MacTCP did not carry in time (docs/08 item 114); on the SE's one-bit screen the dither
   hides what the lower rate loses */
unsigned a68_vmpeg_rate(unsigned w, unsigned h);
/* the size to ask /vmpeg for: the video's shape (vw x vh; 0 x 0 when not known: 4:3) fitted into a box that suits
   a screen of sw x sh pixels (320 x 240 on the SE's 512 x 342 and on any screen up to 640 wide, 512 x 384 below
   1280, 640 x 480 from 1280 up; never more than room_w x room_h, the room the window has there), then made what the
   server takes: even, at least 64, at most 1280 x 720. Into *w and *h */
void a68_vmpeg_size(unsigned long vw, unsigned long vh, unsigned sw, unsigned sh, unsigned room_w, unsigned room_h,
                    unsigned *w, unsigned *h);
/* the server's "what": f:FOLDER (an album; "" the whole library), t:PATH (a song), r:N (radio station N) */
int a68_what_folder(const char *folder, char *out, long max);
int a68_what_track(const char *path, char *out, long max);
int a68_what_radio(unsigned long n, char *out, long max);
int a68_what_video(const char *vid, char *out, long max);      /* v:ID, a music video */

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
