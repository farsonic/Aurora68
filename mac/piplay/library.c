/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/library.c - Aurora68 Play's Library: the music server's artists, albums and tracks, with
 * their covers, a queue, and Now Playing.
 *
 * Backstory (22 November 2026, docs/08 item 99). The owner put their music (some 120 albums, M4A, in
 * "Artist - Album" folders) on a NAS behind the music server (tools/music-server), and Aurora68 Play
 * already played its folders as radio stations. What they asked for next was a player of the library
 * itself, "more like a Spotify type app": artists, their albums with the album art, the tracks, play
 * any of them, next and previous, while the radio stays as it was.
 *
 * How the work is shared. The server does all that a 68k should not: it reads the tags and lengths,
 * converts M4A and FLAC to MP3, scales and dithers each cover to a bitmap of the size and depth asked
 * for (Atkinson's dithering for the SE's one bit, the System's own 256 colours for a colour screen), and
 * answers in lines of text a C loop can split. The Mac shows lists and pictures and plays an MP3 stream,
 * which it already knew how to do: a track is a radio stream with an end (radio.c's radio_play_track),
 * and when one ends the next track's stream follows it into the same card stream, so an album plays
 * without gaps. The card's position (ps->pos_ms) is where the boundaries are: radio.c says where the next
 * track will begin (the audio the card holds at the moment the last stream ended), and the player's
 * display moves on when the card gets there.
 *
 * The shared core (22 November 2026, docs/08 item 107). What this file knew that is not the window went
 * into play/core, which the macOS Play compiles too: the requests and the answers (proto.c), a page of a
 * list as rows with its "previous page" and "next page" lines, a row found again after a refresh and found
 * by typing (catalog.c), the queue with the stream sent ahead (queue.c, which brought Library > Repeat),
 * the device (devices.c), a typed address and a URL with IPv6 in brackets (address.c). This file keeps the
 * window, the views one above the other, the covers, and what the network does when.
 *
 * The window. One window after the Human Interface Guidelines of 1992, as large as the SE's screen
 * allows (500 x 292 at first; any size from 360 x 200): a header with the place in the library ("Artists",
 * an artist's name, an album's title), Back, Shuffle and Play; the list, one view at a time, with a
 * scroll bar (Artists, then an artist's albums with their covers, then an album's tracks under its
 * cover); a bar along the bottom with what plays, its cover, the time, and Previous, Play and Next.
 * Double-click or Return opens a row or plays a track, Delete or the left arrow goes back, typing selects
 * by name. Lists come a page (200 rows) at a time: Aurora68 Play never holds the whole library.
 *
 * Finding the server: the address in the preferences, else a UDP probe (fetch.c); Library > Connect to
 * Server... takes an address. Everything the network does is asynchronous and driven from the event
 * loop (lib_poll), one request at a time, the list first, then covers.
 */
#include "toolbox.h"
#include "psmb.h"
#include "radio.h"
#include "fetch.h"
#include "text.h"
#include "address.h"
#include "proto.h"
#include "catalog.h"
#include "queue.h"
#include "devices.h"
#include "library.h"

extern volatile struct psmb_stream *ps;
static unsigned char pb[256];
static const struct a68_client cl = { 0, 0 };    /* Mac Roman, the short records: what the classic Play always asked */

/* ---- what the library holds now: a page of the view's list (play/core/catalog.c) -------------------------- */
enum { V_ARTISTS = A68_V_ARTISTS, V_ALBUMS = A68_V_ALBUMS, V_TRACKS = A68_V_TRACKS, V_SEARCH = A68_V_SEARCH };
#define MAX_ROWS A68_PAGE
#define PAGE     A68_PAGE
static struct a68_row *rows;                     /* list.rows */
static struct a68_list list;
struct view { int kind; char id[9]; char title[48], sub[40]; char q[40]; int sel, top; long from; };
static struct view stack[6];
static int depth;                                /* stack[depth - 1] is shown */
#define CUR (&stack[depth - 1])

#define THUMB    32                              /* a row's cover: 32 x 32, one bit, 4 bytes a row */
#define THUMB_RB 4
#define THUMB_BYTES (THUMB_RB * THUMB)
static unsigned char *thumbs, thumb_state[MAX_ROWS];      /* 0 not asked, 1 asked, 2 there, 3 none */

#define ART 64                                   /* the large cover: 64 x 64 */
struct art { char album[9]; int depth, state, rb, size; unsigned char *bits; };  /* state: 0 none, 1 asked, 2 there, 3 none there */
/* the album's cover above its tracks, Now Playing's, and the bar's: a cover of its own at 32 x 32, because a
   dithered 64 x 64 one halved by CopyBits keeps every other pixel of a regular pattern and comes out solid */
static struct art view_art, now_art, bar_art;

/* the server */
static char srv_host[64], srv_name[48] = "Library";
static unsigned short srv_port = A68_DEFAULT_PORT;
static int srv_known;                            /* /mac/info answered */
static int finding;                              /* the UDP probe is out */
static char alt_host[2][64];                     /* the server's other addresses (discovery): tried in turn if this one fails */
static int nalt;
static const char *problem;                      /* the list area says this instead of rows */
static char problem_buf[160];

/* ---- the queue (play/core/queue.c): the songs, the one heard (Q.cur), the one whose stream is sent (Q.feed) ---- */
struct qitem { char id[9], album[9]; unsigned long secs; char title[48], artist[40]; };
#define MAX_Q 250
static struct qitem *qitems;
static struct a68_queue Q;
static unsigned long base_ms, offset_s;          /* ps->pos_ms where Q.cur began; seconds into it at that point */
static struct { unsigned long at; int idx; } bounds[8];
static int nb;
static int mode;                                 /* a library track is the player's */
enum { L_STOPPED, L_PLAYING, L_ENDED };
static int lstate;
static int shuffle;
static char album_title[48];                     /* the album of what is heard, for Now Playing */
static struct qitem *qat(int i) { return (struct qitem *)a68q_at(&Q, i); }

/* ---- the window ------------------------------------------------------------------------------------ */
static void *lw, *back_btn, *play_btn, *shuf_cb, *sbar, *prev_btn, *pp_btn, *next_btn;
static short LW, LH;
static int colour, active = 1, shown;
static void *clut8;                              /* the System's 256 colours, for 8-bit covers */
static void *pwin;                               /* the player window (lib_draw_now), for its cover's depth */
static Rect saved_r;
static int saved_shown, have_saved;
#define HEAD_H  26
#define ALBUM_H 74                               /* the album's cover and names above its tracks */
#define BAR_H   40
#define R_LINE  16
#define R_ALBUM 36

/* ---- jobs for the network, one at a time ---------------------------------------------------------- */
enum { J_NONE, J_INFO, J_LIST, J_QUEUE, J_NOWART, J_VIEWART, J_THUMBS, J_BARART, J_DEV };
static int job;                                  /* the one the fetch is for */
static int want_info, want_list;
static int want_queue;                           /* a queue to fetch: an album or an artist's tracks */
static char q_album[9], q_artist[9], q_start[9];
static char thumb_ids[16][9];
static int thumb_rows[16], nthumb;
/* the device (Aurora68 Play 1.3, docs/08 item 105): devices.c says what to ask the server and when; its requests
   take the same connection as the lists, the long poll for commands only when nothing else waits, and it is
   given up (before any byte of its answer has come) when the user asks the Library for something */
static struct dv dev;
static int dev_what;                             /* the device request J_DEV is for */
static int dev_ready;                            /* lib_device: an id and a name */
static int user_waiting;                         /* a click or a key in the Library since the last job */
static char keep_sel[9];                         /* a refresh: the row to select again */
static unsigned long secs_now(void) { return LM_TICKS / 60; }

static void run_commands(char *b, long n);
static unsigned long pos_s(void);
static void lib_refresh(void);
static void copy(char *d, const char *s, int max) { a68_copy(d, s, max); }
static int same_id(const char *a, const char *b) { int i; for (i = 0; i < 8; i++) if (a[i] != b[i]) return 0; return a[0] != 0; }
static int lower(int c) { return a68_lower(c); }
static char *put_time(char *p, unsigned long s) { return a68_put_time(p, p + 12, s); }
static void logs(const char *a, const char *b)
{
	char t[128], *p = put_str(t, "library: ");
	for (; a && *a && p < t + 126; a++) *p++ = *a;
	for (; b && *b && p < t + 126; b++) *p++ = *b;
	*p = 0;
	pi_log(t);
}

void lib_init(int c)
{
	colour = c;
	rows = mac_newptr(sizeof(struct a68_row) * MAX_ROWS);
	a68_list_init(&list, rows, rows ? MAX_ROWS : 0, V_ARTISTS);
	thumbs = mac_newptr(THUMB_BYTES * MAX_ROWS);
	qitems = mac_newptr(sizeof(struct qitem) * MAX_Q);
	a68q_init(&Q, qitems, sizeof(struct qitem), qitems ? MAX_Q : 0);
	view_art.bits = mac_newptr(ART * ART); view_art.size = ART;
	now_art.bits = mac_newptr(ART * ART); now_art.size = ART;
	bar_art.bits = mac_newptr(THUMB * THUMB); bar_art.size = THUMB;
	if (colour) clut8 = (void *)GetCTable(8);
	fetch_init();
	depth = 1;
	stack[0].kind = V_ARTISTS;
	copy(stack[0].title, "Artists", sizeof stack[0].title);
}
void *lib_window(void) { return lw; }
int lib_have_server(void) { return srv_host[0] != 0; }
int lib_shuffle(void) { return shuffle; }
int lib_repeat(void) { return Q.repeat != A68_REPEAT_OFF; }

/* a screen's depth where a rectangle of the window is: 8 or more on a colour screen asks for colour covers */
static int depth_at(void *w, const Rect *local)
{
	void *port;
	if (!colour || !w) return 1;
	Point a = { local->top, local->left }, b = { local->bottom, local->right };
	GetPort(&port);
	SetPort(w);
	LocalToGlobal(&a); LocalToGlobal(&b);
	SetPort(port);
	Rect g = { a.v, a.h, b.v, b.h };
	void **gd = (void **)GetMaxDevice(&g);
	if (!gd || !*gd) return 1;
	void **pm = *(void ***)((char *)*gd + 22);      /* gdPMap */
	if (!pm || !*pm) return 1;
	return *(short *)((char *)*pm + 32) >= 8 ? 8 : 1;   /* pixelSize */
}

/* ---- the server's address ------------------------------------------------------------------------- */
static void set_problem(const char *t) { problem = t; if (lw) { SetPort(lw); InvalRect(PORT_RECT(lw)); } }
static void set_title(void)
{
	char t[80], *p = put_str(t, "Library");
	if (srv_known && srv_name[0]) { p = put_str(p, " \xD1 "); p = put_str(p, srv_name); }   /* an em dash */
	*p = 0;
	if (lw) SetWTitle(lw, pstr(pb, t));
}
static void use_server(const char *host, unsigned short port)
{
	copy(srv_host, host, sizeof srv_host);
	srv_port = port ? port : A68_DEFAULT_PORT;
	srv_known = 0;
	fetch_cancel();
	fetch_forget_host();
	job = J_NONE;
	want_info = 1;
	want_list = 1;
	list.n = 0; list.total = 0;
	set_problem("Asking the music server\xC9");     /* an ellipsis */
	logs("server ", host);
}
static void find_server(void)
{
	srv_host[0] = 0;
	srv_known = 0;
	if (!net_open() || !discover_start()) {
		set_problem("Aurora68 Play could not look for a music server: MacTCP (or Riptide) is not there.");
		return;
	}
	finding = 1;
	set_problem("Looking for a music server on the network\xC9");
}

/* ---- the list's views -------------------------------------------------------------------------------- */
static void view_changed(void)
{
	a68_list_init(&list, rows, list.max, CUR->kind);
	list.from = CUR->from;
	memset(thumb_state, 0, sizeof thumb_state);
	want_list = 1;
	if (job == J_LIST || job == J_THUMBS || job == J_VIEWART) { fetch_cancel(); job = J_NONE; }
	view_art.state = 0;
	problem = 0;
	if (lw) { SetPort(lw); lib_resized(); InvalRect(PORT_RECT(lw)); }
}
static void push_view(int kind, const char *id, const char *title, const char *sub)
{
	if (depth >= 6) { memmove(stack, stack + 1, sizeof stack[0] * 5); depth--; }
	struct view *v = &stack[depth++];
	memset(v, 0, sizeof *v);
	v->kind = kind;
	copy(v->id, id, sizeof v->id);
	copy(v->title, title, sizeof v->title);
	copy(v->sub, sub ? sub : "", sizeof v->sub);
	view_changed();
}
static void go_back(void)
{
	if (depth <= 1) { SysBeep(1); return; }
	depth--;
	view_changed();
}

/* the lines of the list: a "previous page" line, the rows, a "next page" line (play/core/catalog.c) */
static int has_prev(void) { return a68_list_has_prev(&list); }
static int nlines(void) { return a68_list_lines(&list); }
static int line_row(int i) { return a68_list_line_row(&list, i); }   /* -1 the previous page, -2 the next */
static short row_h(void) { return CUR->kind == V_ALBUMS ? R_ALBUM : R_LINE; }
static Rect list_rect(void)
{
	Rect r = { (short)(HEAD_H + 1 + (CUR->kind == V_TRACKS ? ALBUM_H : 0)), 0, (short)(LH - BAR_H - 1), (short)(LW - 15) };
	return r;
}
static int visible(void) { Rect r = list_rect(); int n = (r.bottom - r.top) / row_h(); return n < 1 ? 1 : n; }
static void fix_scroll(void)
{
	int max = nlines() - visible();
	if (max < 0) max = 0;
	if (CUR->top > max) CUR->top = max;
	if (CUR->top < 0) CUR->top = 0;
	if (!sbar) return;
	SetCtlMax(sbar, max);
	SetCtlValue(sbar, CUR->top);
	HiliteControl(sbar, active && max ? 0 : 255);
}

/* ---- the server's answers --------------------------------------------------------------------------- */
static void fail_text(const char *why)
{
	char *p = put_str(problem_buf, why);
	if (srv_host[0]) { p = put_str(p, " (The server: "); p = put_str(p, srv_host); *p++ = ':'; p = put_num(p, srv_port); *p++ = ')'; }
	*p = 0;
	set_problem(problem_buf);
}
static void got_info(void)
{
	struct a68_rec r;
	struct a68_head h;
	struct a68_info inf;
	a68_rec_init(&r, fetch.body, fetch.len);
	if (!a68_head(&r, &h) || !a68_info_of(&r, &inf)) {
		dv_stop(&dev);
		fail_text("That address answers, but not as Aurora68's music server.");
		want_list = 0;
		return;
	}
	copy(srv_name, inf.name, sizeof srv_name);
	srv_known = 1;
	set_title();
	dv_set_generation(&dev, inf.generation);       /* the library's generation (servers of 22 November 2026 on) */
	dev.gen_changed = 0;
	if (dev_ready) dv_start(&dev, secs_now());     /* this server (perhaps another one): announce now */
	char t[120], *p = put_str(t, "the server is \"");
	p = put_str(p, srv_name); p = put_str(p, "\": "); p = put_num(p, inf.artists); p = put_str(p, " artists, ");
	p = put_num(p, inf.albums); p = put_str(p, " albums, "); p = put_num(p, inf.tracks); p = put_str(p, " tracks");
	*p = 0;
	logs(t, fetch.ipv == 6 ? " (IPv6)" : " (IPv4)");
}
static void got_list(void)
{
	int k;
	if (fetch.status != 200) {                   /* "error" and its words */
		const char *t = a68_error_text(fetch.body, fetch.len);
		if (t[0]) { copy(problem_buf, t, sizeof problem_buf); set_problem(problem_buf); }
		else fail_text("The music server could not give that list.");
		return;
	}
	list.kind = CUR->kind;
	if (!a68_list_load(&list, fetch.body, fetch.len)) { fail_text("The music server's answer could not be read."); return; }
	problem = list.n || list.total ? 0 : CUR->kind == V_SEARCH ? "Nothing in the library matches." : "There is nothing here.";
	if (keep_sel[0]) {                           /* a refresh: the same row selected, wherever it is now */
		k = a68_list_find(&list, keep_sel);
		if (k >= 0) CUR->sel = a68_list_row_line(&list, k);
		keep_sel[0] = 0;
	}
	if (CUR->sel >= nlines()) CUR->sel = 0;
	if (lw) { SetPort(lw); fix_scroll(); InvalRect(PORT_RECT(lw)); }
}
/* a cover: "A68ART 1 W H DEPTH ROWBYTES", then the bits */
static void got_art(struct art *a)
{
	long v[4], at, n = a->size;                  /* W, H, depth, rowBytes */
	if (fetch.status != 200 || !a68_art(fetch.body, fetch.len, "A68ART", v, 4, &at) || v[0] != n || v[1] != n ||
	    fetch.len - at < v[3] * n || v[3] * n > n * n) { a->state = 3; return; }
	a->depth = (int)v[2]; a->rb = (int)v[3];
	memcpy(a->bits, fetch.body + at, (unsigned long)(v[3] * n));
	a->state = 2;
}
static void got_thumbs(void)
{
	long v[5], at;                               /* N, W, H, depth, rowBytes */
	int i;
	if (fetch.status != 200 || !a68_art(fetch.body, fetch.len, "A68ARTS", v, 5, &at) || v[1] != THUMB || v[4] != THUMB_RB) {
		for (i = 0; i < nthumb; i++) thumb_state[thumb_rows[i]] = 3;
		return;
	}
	for (i = 0; i < nthumb && i < v[0]; i++) {
		long o = at + (long)i * (1 + THUMB_BYTES);
		int r = thumb_rows[i];
		if (o + 1 + THUMB_BYTES > fetch.len || !same_id(rows[r].id, thumb_ids[i])) continue;
		if (fetch.body[o]) { memcpy(thumbs + r * THUMB_BYTES, fetch.body + o + 1, THUMB_BYTES); thumb_state[r] = 2; }
		else thumb_state[r] = 3;
	}
}

/* ---- playing ------------------------------------------------------------------------------------------ */
/* a track's stream: http://HOST:PORT/t/ID?start=S (an IPv6 address in brackets) */
static void track_url(char *u, long max, int i, unsigned long start)
{
	char path[40];
	struct qitem *q = qat(i);
	a68_path_stream(q ? q->id : "", start, path, sizeof path);
	a68_url(u, max, srv_host, srv_port, path);
}
static void want_now_art(void)
{
	struct qitem *q = qat(Q.cur);
	if (!q) return;
	if (!same_id(now_art.album, q->album)) { copy(now_art.album, q->album, 9); now_art.state = 0; }
	if (!same_id(bar_art.album, q->album)) { copy(bar_art.album, q->album, 9); bar_art.state = 0; }
}
/* radio.c, when a track's connection was lost: from where its audio stopped */
static unsigned long feed_start;                 /* the seconds into Q.feed its connection began at */
/* radio.c asks, when a track's stream has ended: the next track's address (the queue's follower of the one sent:
   the next, or with Library > Repeat the first again), and where it will start */
static int next_url(char *url, int max, unsigned long at)
{
	int f;
	if (!mode || (f = a68q_feed_next(&Q)) < 0) return 0;
	feed_start = 0;                              /* the new stream from its start (a lost one resumes from here) */
	track_url(url, max, f, 0);
	if (nb < 8) { bounds[nb].at = at; bounds[nb].idx = f; nb++; }
	logs("next in the stream: ", qat(f)->title);
	return 1;
}
static void resume_url(char *url, int max, unsigned long secs)
{
	feed_start += secs;
	track_url(url, max, Q.feed, feed_start);
}
static void play_at(int i, unsigned long start)
{
	char u[200], name[60], *p;
	struct qitem *q = qat(i);
	if (!q) return;
	player_lib_mode();
	mode = 1;
	a68q_jump(&Q, i);
	nb = 0;
	base_ms = 0;
	offset_s = feed_start = start;
	radio.next = next_url;
	radio.resume = resume_url;
	track_url(u, sizeof u, i, start);
	p = put_str(name, q->artist);
	p = put_str(p, " - ");
	copy(p, q->title, (int)(sizeof name - (unsigned long)(p - name)));
	want_now_art();
	if (!radio_play_track(name, u)) {
		const char *e = radio.error;
		radio.error = 0;
		lstate = L_STOPPED;
		say_error(e ? e : "The track could not be played.");
	} else lstate = L_PLAYING;
	player_refresh();
	if (lw) { SetPort(lw); InvalRect(PORT_RECT(lw)); }
}
static unsigned long seed_now(void) { return LM_TICKS * 2654435761ul + 1; }
/* the queue's song at at moved to the front (the one chosen, before a shuffle of the rest) */
static void to_front(int at)
{
	struct qitem t;
	if (at <= 0 || !qat(at)) return;
	t = *qat(at);
	a68q_remove(&Q, at);
	a68q_insert(&Q, 0, &t);
}
static void fill_item(struct qitem *q, const char *id, const char *album, const char *title, const char *artist, unsigned long secs)
{
	memset(q, 0, sizeof *q);
	copy(q->id, id, 9); copy(q->album, album, 9);
	copy(q->title, title, sizeof q->title); copy(q->artist, artist, sizeof q->artist);
	q->secs = secs;
}
/* the queue from the rows of a tracks view (start at row s, -1 for Play); in order from the chosen one, or the
   chosen one first and the rest shuffled */
static void queue_from_rows(int s)
{
	int i, n = 0, at = 0;
	for (i = 0; i < list.n && n < MAX_Q; i++) {
		if (rows[i].kind != 'T') continue;
		if (i == s) at = n;
		fill_item(&qitems[n++], rows[i].id, rows[i].album, rows[i].text, rows[i].sub, rows[i].secs);
	}
	if (!n || !qitems) return;
	a68q_set(&Q, n, 0);
	copy(album_title, CUR->kind == V_TRACKS ? CUR->title : "", sizeof album_title);
	if (shuffle) {
		if (s >= 0) to_front(at);
		a68q_shuffle_from(&Q, s >= 0 ? 1 : 0, seed_now());
		at = 0;
	}
	play_at(at, 0);
}
static void ask_queue(const char *album, const char *artist, const char *start)
{
	copy(q_album, album ? album : "", 9);
	copy(q_artist, artist ? artist : "", 9);
	copy(q_start, start ? start : "", 9);
	want_queue = 1;
	if (job == J_THUMBS || job == J_VIEWART) { fetch_cancel(); job = J_NONE; }
}
static void got_queue(void)
{
	struct a68_rec r;
	struct a68_head h;
	struct a68_track t;
	int at = 0, n = 0;
	a68_rec_init(&r, fetch.body, fetch.len);
	if (fetch.status != 200 || !a68_head(&r, &h)) { say_error("The music server could not give those tracks."); return; }
	while (n < MAX_Q && qitems && a68_next_track(&r, &t)) {
		if (q_start[0] && same_id(t.id, q_start)) at = n;
		fill_item(&qitems[n++], t.id, t.album, t.title, t.artist, t.secs);
	}
	if (!n) return;
	a68q_set(&Q, n, 0);
	if (shuffle) {
		to_front(at);
		a68q_shuffle_from(&Q, q_start[0] ? 1 : 0, seed_now());
		at = 0;
	}
	album_title[0] = 0;
	play_at(at, 0);
}

/* ---- the device's commands (play/core/proto.c reads them; this carries them out) --------------------------- */
struct pending { int act; long pos; int vol, first, n, radio; char rname[48], rurl[200]; };
static void finish_command(struct pending *c)
{
	switch (c->act) {
	case A68_ACT_PLAY:
		if (c->radio) { lib_leave(); start_station(c->rname, c->rurl); break; }
		if (c->n) { album_title[0] = 0; play_at(0, (unsigned long)c->pos); }
		break;
	case A68_ACT_ADD: case A68_ACT_NEXT:         /* into the queue; playing it if nothing plays */
		if (c->n && (!mode || lstate != L_PLAYING)) play_at(c->first, 0);
		else player_refresh();
		break;
	case A68_ACT_PAUSE: player_pause(1); break;
	case A68_ACT_RESUME: player_pause(0); break;
	case A68_ACT_STOP: player_stop(); break;
	case A68_ACT_SKIP: if (mode) lib_skip(1); break;
	case A68_ACT_PREVIOUS: if (mode) lib_skip(-1); break;
	case A68_ACT_SEEK: player_seek((unsigned long)c->pos); break;
	case A68_ACT_VOLUME: if (c->vol >= 0) player_volume(c->vol); break;
	}
	if (lw) { SetPort(lw); InvalRect(PORT_RECT(lw)); }
}
static void run_commands(char *b, long n)
{
	struct a68_rec r;
	struct a68_cmd c;
	struct pending pc;
	struct qitem item;
	int more;
	memset(&pc, 0, sizeof pc);
	a68_rec_init(&r, b, n);
	do {
		more = a68_next_cmd(&r, &c);
		if (!more || c.kind == 'C') {
			if (pc.act) finish_command(&pc);
			memset(&pc, 0, sizeof pc);
			if (!more) break;
			pc.act = a68_action(c.action); pc.pos = c.position; pc.vol = c.volume;
			if (pc.act > A68_ACT_STOP) pc.act = A68_ACT_UNKNOWN;     /* the Live channel's own: not for a player */
			logs("a command from the server: ", a68_action_name(pc.act));
			continue;
		}
		if (c.kind == 'T' && (pc.act == A68_ACT_PLAY || pc.act == A68_ACT_ADD || pc.act == A68_ACT_NEXT) && qitems) {
			int at;
			if (pc.act == A68_ACT_PLAY && !pc.n) { lib_leave(); a68q_clear(&Q); }   /* the queue replaced (once its first track is here) */
			/* "next": after what is being received (the stream may already be on the track after the one heard) */
			at = pc.act == A68_ACT_NEXT && mode ? a68q_next_place(&Q) + pc.n : pc.act == A68_ACT_PLAY ? pc.n : Q.n;
			if (!pc.n) pc.first = at > Q.n ? Q.n : at;
			fill_item(&item, c.tid, "", c.title, c.artist, c.secs);   /* no album id in a command's tracks: no cover for them */
			if (a68q_insert(&Q, at, &item) >= 0) pc.n++;
		} else if (c.kind == 'R') { copy(pc.rname, c.name, sizeof pc.rname); copy(pc.rurl, c.url, sizeof pc.rurl); pc.radio = 1; }
	} while (more);
}
/* the library's generation moved: the list in view again, its selected row kept */
static void lib_refresh(void)
{
	int r;
	if (!lw || !srv_known) return;
	r = line_row(CUR->sel);
	keep_sel[0] = 0;
	if (r >= 0 && r < list.n) copy(keep_sel, rows[r].id, 9);
	view_changed();
}
/* the device's part of the player: an id kept in the preferences and the Mac's name */
void lib_device(const char *id, const char *name)
{
	dv_init(&dev, id, name, "1.4", "classic");
	dev_ready = id[0] != 0;
	if (dev_ready && srv_known) dv_start(&dev, secs_now());
}
/* what a library track is, for the device's state: 1 if one plays (or is paused) */
int lib_now(char *tid, unsigned long *pos, unsigned long *len)
{
	struct qitem *q = qat(Q.cur);
	if (!mode || lstate != L_PLAYING || !q) return 0;
	copy(tid, q->id, 9);
	*pos = pos_s();
	*len = q->secs;
	return 1;
}
void lib_seek(unsigned long s)
{
	struct qitem *q = qat(Q.cur);
	if (!mode || !q) return;
	if (q->secs && s >= q->secs) s = q->secs - 1;
	play_at(Q.cur, s);
}

/* ---- the network, from the event loop ------------------------------------------------------------- */
static void start_job(int j, const char *path)
{
	if (fetch_start(srv_host, srv_port, path)) job = j;
}
static void next_job(void)
{
	char path[220];
	int i;
	if (!srv_host[0]) return;
	if (want_info) { want_info = 0; a68_path_info(&cl, path, sizeof path); start_job(J_INFO, path); return; }
	if (want_list && srv_known) {
		struct view *v = CUR;
		want_list = 0;
		a68_list_path(&cl, v->kind, v->id, v->q, v->from, path, sizeof path);
		start_job(J_LIST, path);
		return;
	}
	if (!srv_known) return;
	{   /* the device's announce and state: short, before the covers */
		int w = dv_want(&dev, secs_now());
		if (w == DV_ANNOUNCE || w == DV_STATE) {
			dv_path(&dev, w, path, sizeof path);
			dev_what = w;
			start_job(J_DEV, path);
			return;
		}
	}
	if (want_queue) {
		want_queue = 0;
		a68_path_tracks(&cl, q_album, q_artist, 0, MAX_Q, path, sizeof path);
		start_job(J_QUEUE, path);
		return;
	}
	struct art *a = 0;
	if (mode && now_art.album[0] && !now_art.state) a = &now_art;
	else if (mode && lw && bar_art.album[0] && !bar_art.state) a = &bar_art;
	else if (CUR->kind == V_TRACKS && lw && !view_art.state) {
		copy(view_art.album, CUR->id, 9);
		Rect vr = { HEAD_H + 6, 10, HEAD_H + 6 + ART, 10 + ART };
		if (now_art.state == 2 && same_id(now_art.album, view_art.album) && now_art.depth == depth_at(lw, &vr)) {   /* the same cover: no need to ask */
			memcpy(view_art.bits, now_art.bits, ART * ART); view_art.depth = now_art.depth; view_art.rb = now_art.rb; view_art.state = 2;
			SetPort(lw); InvalRect(PORT_RECT(lw));
			return;
		}
		a = &view_art;
	}
	if (a) {
		/* one bit or eight: what the screen under the cover shows (an SE's screen and a colour HDMI screen side
		   by side differ); a window moved to the other screen asks again (lib_draw_now, draw_album_head) */
		Rect pr = { 5, 9, 5 + ART, 9 + ART }, vr = { HEAD_H + 6, 10, HEAD_H + 6 + ART, 10 + ART };
		Rect br = { (short)(LH - 36), 8, (short)(LH - 4), 40 };
		a->depth = a == &now_art ? depth_at(pwin ? pwin : lw, &pr) : depth_at(lw, a == &bar_art ? &br : &vr);
		a->state = 1;
		a68_path_art(&cl, a->album, a->size, a->depth, path, sizeof path);
		start_job(a == &now_art ? J_NOWART : a == &bar_art ? J_BARART : J_VIEWART, path);
		return;
	}
	if (CUR->kind == V_ALBUMS && lw) {           /* the covers of the albums in view, sixteen at a time */
		int first = CUR->top - has_prev(), last = first + visible();
		const char *ids[16];
		if (first < 0) first = 0;
		nthumb = 0;
		for (i = first; i <= last && i < list.n && nthumb < 16; i++)
			if (!thumb_state[i]) {
				copy(thumb_ids[nthumb], rows[i].id, 9);
				ids[nthumb] = thumb_ids[nthumb];
				thumb_rows[nthumb++] = i;
				thumb_state[i] = 1;
			}
		if (nthumb) { a68_path_arts(&cl, ids, nthumb, THUMB, path, sizeof path); start_job(J_THUMBS, path); return; }
	}
	if (dv_want(&dev, secs_now()) == DV_COMMANDS) {   /* nothing else to do: wait for a command */
		dv_path(&dev, DV_COMMANDS, path, sizeof path);
		dev_what = DV_COMMANDS;
		start_job(J_DEV, path);
		if (job == J_DEV) fetch.cut_ok = 1;          /* a long list of tracks: the first ones are enough */
	}
}
static void draw_bar(void);
static void draw_list(void);
static void draw_album_head(void);
void lib_poll(void)
{
	static unsigned long last_draw;
	if (finding) {
		discover_poll();
		if (disc.st == D_FOUND) {
			/* the addresses to try, best first (play/core/address.c): the family Connect By names first; for IPv4 the
			   address the answer came from, which is known to reach this Mac, before the one the server names (which
			   may be its container's); IPv6 only through Riptide's .IPP6. The others are kept in case it fails */
			const char *c[3];
			int n = a68_disc_hosts(&disc.d, disc.host, net_prefer == 4 ? A68_IPV4 : A68_IPV6, net_is_v6(), c), k;
			char first[64];
			finding = 0;
			if (!n) { set_problem("The music server's answer named no address this Mac can use."); return; }
			copy(first, c[0], sizeof first);
			for (nalt = 0, k = 1; k < n && nalt < 2; k++) copy(alt_host[nalt++], c[k], sizeof alt_host[0]);
			use_server(first, (unsigned short)disc.d.port);
			srv_known = 0;
			copy(srv_name, disc.d.name, sizeof srv_name);
		} else if (disc.st == D_NONE) {
			finding = 0;
			set_problem("No music server answered on this network. Choose Library > Connect to Server\xC9 and type its "
			            "address, such as 192.168.1.20:8423 (the music server's web page shows it).");
		}
	}
	fetch_poll();
	/* a command's long poll gives way to what the user asked for, if nothing of its answer has come */
	if (job == J_DEV && dev_what == DV_COMMANDS && (want_info || want_list || want_queue || user_waiting) &&
	    fetch.st != FS_DONE && fetch.st != FS_FAILED && !fetch_received()) {
		fetch_cancel();
		job = J_NONE;
		fetch.st = FS_IDLE;
	}
	user_waiting = 0;
	if (dev.gen_changed) {                       /* new albums (or fewer): the list again, the same row selected */
		dev.gen_changed = 0;
		logs("the library changed: its lists again", 0);
		lib_refresh();
	}
	if (dev.on) {                                /* what plays, for the server (devices.c decides when it is news) */
		static unsigned long last;
		if (LM_TICKS - last >= 60) {
			struct dv_state st;
			last = LM_TICKS;
			player_dev_state(&st);
			dv_set_state(&dev, &st, secs_now());
		}
	}
	if (job && (fetch.st == FS_DONE || fetch.st == FS_FAILED)) {
		int j = job;
		job = J_NONE;
		if (fetch.st == FS_FAILED) {
			if (j == J_INFO && nalt) {               /* the server's other addresses, each once */
				char h[64];
				int k;
				copy(h, alt_host[0], sizeof h);
				for (k = 1; k < nalt; k++) copy(alt_host[k - 1], alt_host[k], sizeof alt_host[0]);
				nalt--;
				logs("trying the server's other address, ", h);
				use_server(h, srv_port);
				fetch.st = FS_IDLE;
				return;
			}
			if (j == J_INFO || j == J_LIST) { fail_text(fetch.error); if (j == J_INFO) want_list = 1; }
			else if (j == J_QUEUE) say_error(fetch.error);
			else if (j == J_NOWART) now_art.state = 3;
			else if (j == J_BARART) bar_art.state = 3;
			else if (j == J_VIEWART) view_art.state = 3;
			else if (j == J_THUMBS) { int i; for (i = 0; i < nthumb; i++) thumb_state[thumb_rows[i]] = 3; }
			else if (j == J_DEV) dv_answer(&dev, dev_what, 0, 0, 0, secs_now());
		} else {
			switch (j) {
			case J_INFO: got_info(); if (lw) { SetPort(lw); InvalRect(PORT_RECT(lw)); } break;
			case J_LIST: got_list(); break;
			case J_QUEUE: got_queue(); break;
			case J_NOWART: got_art(&now_art); player_refresh(); if (lw) { SetPort(lw); draw_bar(); if (CUR->kind == V_TRACKS) { view_art.state = 0; } } break;
			case J_VIEWART: got_art(&view_art); if (lw) { SetPort(lw); draw_album_head(); } break;
			case J_BARART: got_art(&bar_art); if (lw) { SetPort(lw); draw_bar(); } break;
			case J_THUMBS: got_thumbs(); if (lw) { SetPort(lw); draw_list(); } break;
			case J_DEV:
				/* answers are read in place, once: a commands answer's commands here (devices.c wants only its status),
				   an announce's lines by devices.c */
				if (dev_what == DV_COMMANDS) {
					if (fetch.status == 200 && fetch.len) run_commands(fetch.body, fetch.len);
					dv_answer(&dev, dev_what, fetch.status, 0, 0, secs_now());
				} else dv_answer(&dev, dev_what, fetch.status, fetch.body, fetch.len, secs_now());
				break;
			}
		}
		fetch.st = FS_IDLE;
	}
	if (!job) next_job();
	/* what is heard: the next track when the card has reached it */
	if (mode && ps) {
		while (nb && ps->pos_ms >= bounds[0].at) {
			a68q_heard(&Q, bounds[0].idx);
			base_ms = bounds[0].at;
			offset_s = 0;
			nb--;
			memmove(bounds, bounds + 1, sizeof bounds[0] * (unsigned long)nb);
			logs("now playing ", qat(Q.cur) ? qat(Q.cur)->title : "");
			want_now_art();
			player_refresh();
			if (lw) { SetPort(lw); draw_bar(); if (CUR->kind == V_TRACKS) draw_list(); }
		}
		if (radio.ended && lstate == L_PLAYING) {
			radio.ended = 0;
			lstate = L_ENDED;
			logs("the queue has played", 0);
			player_refresh();
			if (lw) { SetPort(lw); draw_bar(); }
		}
		if (radio.error) {                       /* told once, by the player's window */
			const char *e = radio.error;
			radio.error = 0;
			lstate = L_STOPPED;
			player_refresh();
			say_error(e);
		}
		if (lw && LM_TICKS - last_draw >= 30) { last_draw = LM_TICKS; SetPort(lw); draw_bar(); }
	}
}

/* ---- drawing ----------------------------------------------------------------------------------------- */
static void clip_text(const char *s, int max_w)
{
	int n = (int)strlen(s);
	if (TextWidth(s, 0, n) <= max_w) { DrawText(s, 0, n); return; }
	while (n > 0 && TextWidth(s, 0, n) + 10 > max_w) n--;
	DrawText(s, 0, n);
	DrawText("\xC9", 0, 1);
}
static void draw_right(const char *s, short right, short v)
{
	int n = (int)strlen(s);
	MoveTo((short)(right - TextWidth(s, 0, n)), v);
	DrawText(s, 0, n);
}
struct pixmap { void *base; short rb; Rect bounds; short ver, pack; long packsize, hres, vres; short ptype, psize, ccount, csize; long planes; void *table; long res; };
_Static_assert(sizeof(struct pixmap) == 50, "PixMap");
/* a cover at (left, top): the bits as they came, one bit or the System's 256 colours; a frame for none */
static void draw_bits(void *w, const unsigned char *bits, int bdepth, int rb, short size, short left, short top)
{
	Rect src = { 0, 0, size, size }, dst = { top, left, (short)(top + size), (short)(left + size) };
	ForeColor(33); BackColor(30);                /* black and white: CopyBits colours a one-bit source with them */
	if (bdepth == 8 && clut8) {
		struct pixmap pm = { (void *)bits, (short)(rb | 0x8000), { 0, 0, size, size }, 0, 0, 0, 72L << 16, 72L << 16, 0, 8, 1, 8, 0, clut8, 0 };
		CopyBits(&pm, (char *)w + 2, &src, &dst, 0, 0);
	} else {
		struct { const void *base; short rb; Rect bounds; } bm = { bits, (short)rb, { 0, 0, size, size } };
		CopyBits(&bm, (char *)w + 2, &src, &dst, 0, 0);
	}
}
static void draw_cover(void *w, struct art *a, short size, short left, short top)
{
	Rect r = { top, left, (short)(top + size), (short)(left + size) };
	if (a && a->state == 2) draw_bits(w, a->bits, a->depth, a->rb, size, left, top);
	else {                                       /* no cover (yet): a grey square with a note in it */
		static const unsigned char grey[8] = { 0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22 };
		FillRect(&r, grey);
		Rect n = { (short)(top + size / 2 + 2), (short)(left + size / 2 - 7), (short)(top + size / 2 + 8), (short)(left + size / 2 - 1) };
		PaintRect(&n);
		Rect s = { (short)(top + size / 4), (short)(left + size / 2 - 2), (short)(top + size / 2 + 6), (short)(left + size / 2 - 1) };
		PaintRect(&s);
	}
	InsetRect(&r, -1, -1);
	FrameRect(&r);
}
static void row_texts(int i, char *right)
{
	struct a68_row *r = &rows[i];
	char *p = right;
	switch (r->kind) {
	case 'A': p = put_num(p, r->count); p = put_str(p, r->count == 1 ? " album" : " albums"); break;
	case 'L': p = put_num(p, r->count); p = put_str(p, r->count == 1 ? " track" : " tracks"); break;
	case 'T': if (r->secs) p = put_time(p, r->secs); break;
	}
	*p = 0;
}
static int playing_row(int i)
{
	return mode && lstate != L_STOPPED && Q.cur >= 0 && Q.cur < Q.n && rows[i].kind == 'T' && same_id(rows[i].id, qat(Q.cur)->id);
}
static void draw_line(int li, Rect *lr)
{
	short h = row_h(), y = (short)(lr->top + (li - CUR->top) * h);
	Rect r = { y, lr->left, (short)(y + h), lr->right };
	char right[24];
	int i = line_row(li);
	EraseRect(&r);
	TextFont(3); TextSize(9); TextFace(0);
	if (i < 0) {
		MoveTo(30, (short)(y + 12));
		DrawText(i == -1 ? "Previous page\xC9" : "Next page\xC9", 0, i == -1 ? 14 : 10);
	} else if (rows[i].kind == 'L' && CUR->kind == V_ALBUMS) {
		if (thumb_state[i] == 2) draw_bits(lw, thumbs + i * THUMB_BYTES, 1, THUMB_RB, THUMB, 8, (short)(y + 2));
		else draw_cover(lw, 0, THUMB, 8, (short)(y + 2));
		if (thumb_state[i] == 2) { Rect f = { (short)(y + 1), 7, (short)(y + 3 + THUMB), 9 + THUMB }; FrameRect(&f); }
		TextFace(1);
		MoveTo(50, (short)(y + 15));
		clip_text(rows[i].text, lr->right - 60);
		TextFace(0);
		row_texts(i, right);
		MoveTo(50, (short)(y + 28));
		clip_text(right, lr->right - 60);
	} else {
		short x = 8;
		row_texts(i, right);
		short rw = (short)TextWidth(right, 0, (int)strlen(right));
		if (rows[i].kind == 'T' && CUR->kind == V_TRACKS) {
			char n[8];
			put_num(n, rows[i].num ? rows[i].num : (unsigned long)i + 1)[0] = 0;
			if (playing_row(i)) put_str(n, "\xA5")[0] = 0;   /* a bullet by the track that plays */
			draw_right(n, 22, (short)(y + 12));
			x = 30;
		}
		if (playing_row(i) && CUR->kind != V_TRACKS) { MoveTo(x, (short)(y + 12)); DrawText("\xA5 ", 0, 2); x += 10; }
		MoveTo(x, (short)(y + 12));
		if (CUR->kind == V_SEARCH) {
			char t[100], *p = t;
			p = put_str(p, rows[i].kind == 'A' ? "Artist: " : rows[i].kind == 'L' ? "Album: " : "");
			p = put_str(p, rows[i].text);
			if (rows[i].sub[0] && rows[i].kind != 'A') { p = put_str(p, " \xD1 "); p = put_str(p, rows[i].sub); }
			*p = 0;
			clip_text(t, lr->right - x - rw - 16);
		} else clip_text(rows[i].text, lr->right - x - rw - 16);
		draw_right(right, (short)(lr->right - 8), (short)(y + 12));
	}
	if (li == CUR->sel && active) InvertRect(&r);
}
static void draw_list(void)
{
	Rect lr = list_rect();
	int li, n = nlines(), v = visible();
	if (!lw) return;
	if (problem || !n) {
		EraseRect(&lr);
		Rect tr = lr;
		InsetRect(&tr, 24, 20);
		TextFont(3); TextSize(9); TextFace(0);
		const char *t = problem ? problem : want_list || job == J_LIST ? "Loading\xC9" : "";
		TextBox(t, (long)strlen(t), &tr, 0);
		return;
	}
	for (li = CUR->top; li < CUR->top + v + 1 && li < n; li++) draw_line(li, &lr);
	short y = (short)(lr.top + (n - CUR->top) * row_h());
	if (y < lr.bottom) { Rect rest = { y, lr.left, lr.bottom, lr.right }; EraseRect(&rest); }
}
static void draw_album_head(void)
{
	if (!lw || CUR->kind != V_TRACKS) return;
	Rect r = { HEAD_H + 1, 0, (short)(HEAD_H + 1 + ALBUM_H), (short)LW };
	char t[60], *p;
	unsigned long secs = 0;
	int i;
	EraseRect(&r);
	Rect vr = { HEAD_H + 6, 10, HEAD_H + 6 + ART, 10 + ART };
	if (view_art.state == 2 && view_art.depth != depth_at(lw, &vr)) view_art.state = 0;   /* moved to another screen */
	draw_cover(lw, &view_art, ART, 10, HEAD_H + 6);
	TextFont(0); TextSize(12); TextFace(0);
	MoveTo(86, HEAD_H + 22);
	clip_text(CUR->title, LW - 96);
	TextFont(3); TextSize(9);
	MoveTo(86, HEAD_H + 36);
	clip_text(CUR->sub, LW - 96);
	for (i = 0; i < list.n; i++) secs += rows[i].secs;
	p = put_num(t, (unsigned long)list.total); p = put_str(p, list.total == 1 ? " track" : " tracks");
	if (secs) { p = put_str(p, ", "); p = put_time(p, secs); }
	*p = 0;
	MoveTo(86, HEAD_H + 50);
	DrawText(t, 0, (int)strlen(t));
	MoveTo(0, (short)(HEAD_H + ALBUM_H)); LineTo((short)LW, (short)(HEAD_H + ALBUM_H));
}
static unsigned long pos_s(void)
{
	if (!mode || !ps || lstate == L_STOPPED) return 0;
	unsigned long at = ps->pos_ms > base_ms ? ps->pos_ms - base_ms : 0;
	return offset_s + at / 1000;
}
static void draw_bar(void)
{
	if (!lw) return;
	Rect r = { (short)(LH - BAR_H + 1), 0, LH, (short)(LW - 150) };     /* the text, not the buttons beside it */
	char t[120], *p;
	EraseRect(&r);
	MoveTo(0, (short)(LH - BAR_H)); LineTo((short)LW, (short)(LH - BAR_H));
	TextFont(3); TextSize(9);
	short tx = 8, right = (short)(LW - 150);
	if (mode && Q.cur >= 0 && Q.cur < Q.n) {
		struct qitem *q = qat(Q.cur);
		draw_cover(lw, bar_art.state == 2 && same_id(bar_art.album, q->album) ? &bar_art : 0, THUMB, 8, (short)(LH - 36));
		tx = 48;
		TextFace(1);
		MoveTo(tx, (short)(LH - 24));
		clip_text(q->title, right - tx - 6);
		TextFace(0);
		p = put_str(t, q->artist);
		p = put_str(p, " \xD1 ");
		p = put_time(p, pos_s());
		if (q->secs) { p = put_str(p, " of "); p = put_time(p, q->secs); }
		if (lstate == L_ENDED) p = put_str(p, " \xD1 finished");
		else if (radio.paused) p = put_str(p, " \xD1 paused");
		*p = 0;
		MoveTo(tx, (short)(LH - 11));
		clip_text(t, right - tx - 6);
	} else {
		MoveTo(tx, (short)(LH - 17));
		clip_text(srv_host[0] ? "Double-click a track to play it, or choose Play for an album or an artist." : "", right - tx - 6);
	}
	if (pp_btn) {
		static int said = -1;
		int pl = lib_playing();
		if (pl != said) { said = pl; SetCTitle(pp_btn, pstr(pb, pl ? "Pause" : "Play")); }
		HiliteControl(pp_btn, active && (mode || list.n) ? 0 : 255);
		HiliteControl(prev_btn, active && lib_can_skip(-1) ? 0 : 255);
		HiliteControl(next_btn, active && lib_can_skip(1) ? 0 : 255);
	}
}
static void draw_head(void)
{
	Rect r = { 0, 0, HEAD_H, (short)LW };
	EraseRect(&r);
	TextFont(0); TextSize(12); TextFace(0);
	short x = depth > 1 ? 62 : 10, lim = (short)(LW - (play_btn && CUR->kind != V_ARTISTS && CUR->kind != V_SEARCH ? 150 : 90));
	MoveTo(x, 18);
	clip_text(CUR->title, lim - x);
	MoveTo(0, HEAD_H); LineTo((short)LW, HEAD_H);
}
static void grow_box(void)
{
	Rect c = { (short)(LH - 15), (short)(LW - 15), LH, LW }, all = { -32000, -32000, 32000, 32000 };
	ClipRect(&c);
	DrawGrowIcon(lw);
	ClipRect(&all);
}
static void place_controls(void)
{
	Rect lr = list_rect();
	MoveControl(back_btn, 6, 4);
	if (depth > 1) ShowControl(back_btn); else HideControl(back_btn);
	MoveControl(shuf_cb, (short)(LW - 142), 5);
	MoveControl(play_btn, (short)(LW - 62), 4);
	if (CUR->kind == V_ALBUMS || CUR->kind == V_TRACKS) ShowControl(play_btn); else HideControl(play_btn);
	MoveControl(sbar, (short)(LW - 15), (short)(lr.top - 1));
	SizeControl(sbar, 16, (short)(lr.bottom - lr.top + 2));
	MoveControl(prev_btn, (short)(LW - 146), (short)(LH - 30));
	MoveControl(pp_btn, (short)(LW - 112), (short)(LH - 30));
	MoveControl(next_btn, (short)(LW - 54), (short)(LH - 30));
}
void lib_resized(void)
{
	if (!lw) return;
	Rect *pr = PORT_RECT(lw);
	LW = pr->right - pr->left; LH = pr->bottom - pr->top;
	place_controls();
	fix_scroll();
}
void lib_update(void)
{
	SetPort(lw);
	EraseRect(PORT_RECT(lw));
	draw_head();
	draw_album_head();
	draw_list();
	draw_bar();
	DrawControls(lw);
	grow_box();
}
void lib_activate(int on)
{
	active = on;
	if (!lw) return;
	SetPort(lw);
	HiliteControl(back_btn, on ? 0 : 255);
	HiliteControl(play_btn, on ? 0 : 255);
	HiliteControl(shuf_cb, on ? 0 : 255);
	fix_scroll();
	draw_list();
	draw_bar();
	grow_box();
}

/* ---- making and showing the window --------------------------------------------------------------------- */
void lib_show(void)
{
	if (!lw) {
		Rect s = screen_rect(), b;
		short w = s.right - s.left >= 640 ? 540 : 500, h = s.bottom - s.top >= 480 ? 360 : 292;
		b.top = 44; b.left = (short)((s.right - s.left - w) / 2); b.bottom = (short)(b.top + h); b.right = (short)(b.left + w);
		if (have_saved && saved_r.right - saved_r.left >= 360 && saved_r.bottom - saved_r.top >= 200) {
			Point a = { (short)(saved_r.top - 10), (short)(saved_r.left + 24) };
			if (PtInRgn(pt_long(a), LM_GRAYRGN)) b = saved_r;
		}
		lw = colour ? (void *)NewCWindow(0, &b, pstr(pb, "Library"), 0, 8 /* zoomDocProc */, (void *)-1, 1, 0)
		            : (void *)NewWindow(0, &b, pstr(pb, "Library"), 0, 8, (void *)-1, 1, 0);
		if (!lw) { SysBeep(5); return; }
		SetPort(lw);
		Rect r0 = { 0, 0, 18, 50 }, r1 = { 0, 0, 18, 56 }, r2 = { 0, 0, 18, 76 }, r3 = { 0, 0, 100, 16 }, r4 = { 0, 0, 18, 30 };
		back_btn = NewControl(lw, &r0, pstr(pb, "Back"), 0, 0, 0, 1, pushButProc, 0);
		play_btn = NewControl(lw, &r1, pstr(pb, "Play"), 0, 0, 0, 1, pushButProc, 0);
		shuf_cb = NewControl(lw, &r2, pstr(pb, "Shuffle"), 1, shuffle, 0, 1, checkBoxProc, 0);
		sbar = NewControl(lw, &r3, pstr(pb, ""), 1, 0, 0, 0, scrollBarProc, 0);
		prev_btn = NewControl(lw, &r4, pstr(pb, "<<"), 1, 0, 0, 1, pushButProc, 0);
		r1.right = 54;
		pp_btn = NewControl(lw, &r1, pstr(pb, "Play"), 1, 0, 0, 1, pushButProc, 0);
		next_btn = NewControl(lw, &r4, pstr(pb, ">>"), 1, 0, 0, 1, pushButProc, 0);
		lib_resized();
		set_title();
		if (!srv_host[0] && !finding) find_server();
		else if (srv_host[0] && !srv_known && !job) { want_info = 1; want_list = 1; }
	}
	ShowWindow(lw);
	SelectWindow(lw);
	shown = 1;
	SetPort(lw);
	InvalRect(PORT_RECT(lw));
}
void lib_close(void)
{
	if (!lw) return;
	HideWindow(lw);
	shown = 0;
}

/* ---- clicks and keys --------------------------------------------------------------------------------- */
static void open_line(int li)
{
	int i = line_row(li);
	if (li < 0 || li >= nlines()) return;
	if (i == -1) { CUR->from -= PAGE; if (CUR->from < 0) CUR->from = 0; CUR->sel = CUR->top = 0; view_changed(); return; }
	if (i == -2) { CUR->from += PAGE; CUR->sel = CUR->top = 0; view_changed(); return; }
	struct a68_row *r = &rows[i];
	switch (r->kind) {
	case 'A': push_view(V_ALBUMS, r->id, r->text, 0); break;
	case 'L': push_view(V_TRACKS, r->id, r->text, r->sub[0] ? r->sub : CUR->title); break;
	case 'T':
		if (CUR->kind == V_TRACKS) queue_from_rows(i);
		else ask_queue(r->album, 0, r->id);      /* from a search: its album, from it */
		break;
	}
}
static void select_line(int li)
{
	int n = nlines(), v = visible();
	if (!n) return;
	if (li < 0) li = 0;
	if (li >= n) li = n - 1;
	CUR->sel = li;
	if (li < CUR->top) CUR->top = li;
	if (li >= CUR->top + v) CUR->top = li - v + 1;
	fix_scroll();
	draw_list();
}
static void scroll_to(int top)
{
	CUR->top = top;
	fix_scroll();
	draw_list();
	if (CUR->kind == V_ALBUMS) want_list = want_list;   /* the thumbs of what came into view: next_job asks */
}
int lib_ctl_action(void *ctl, int part)
{
	if (!lw || ctl != sbar) return 0;
	int v = visible(), t = CUR->top;
	switch (part) {
	case inUpButton: t--; break;
	case inDownButton: t++; break;
	case inPageUp: t -= v - 1; break;
	case inPageDown: t += v - 1; break;
	}
	scroll_to(t);
	return 1;
}
void lib_click(EventRecord *ev)
{
	user_waiting = 1;
	static unsigned long last_t;
	static int last_li = -1;
	Point p = ev->where;
	void *ctl = 0;
	SetPort(lw);
	GlobalToLocal(&p);
	int part = FindControl(pt_long(p), lw, &ctl);
	if (ctl) {
		if (ctl == sbar) {
			if (part == inThumb) { TrackControl(ctl, pt_long(p), 0); scroll_to(GetCtlValue(sbar)); }
			else TrackControl(ctl, pt_long(p), (void *)pas_ctl_action);
			return;
		}
		if (!TrackControl(ctl, pt_long(p), 0)) return;
		if (ctl == back_btn) go_back();
		else if (ctl == shuf_cb) lib_toggle_shuffle();
		else if (ctl == play_btn) lib_play_all();
		else if (ctl == pp_btn) lib_play_pause();
		else if (ctl == prev_btn) lib_skip(-1);
		else if (ctl == next_btn) lib_skip(1);
		return;
	}
	Rect lr = list_rect();
	if (PtInRect(pt_long(p), &lr) && !problem) {
		int li = CUR->top + (p.v - lr.top) / row_h();
		if (li >= nlines()) return;
		int dbl = li == last_li && ev->when - last_t <= *(volatile unsigned long *)0x2F0;   /* DoubleTime */
		last_li = li; last_t = ev->when;
		select_line(li);
		if (dbl) { last_li = -1; open_line(li); }
	}
}
void lib_key(EventRecord *ev)
{
	user_waiting = 1;
	static char typed[16];
	static int ntyped;
	static unsigned long typed_t;
	int c = (int)(ev->message & charCodeMask), i;
	switch (c) {
	case 0x1E: select_line(CUR->sel - 1); return;        /* up arrow */
	case 0x1F: select_line(CUR->sel + 1); return;        /* down arrow */
	case 0x0D: case 0x03: case 0x1D: open_line(CUR->sel); return;   /* Return, Enter, right arrow */
	case 0x08: case 0x1C: go_back(); return;              /* Delete, left arrow */
	case ' ': lib_play_pause(); return;
	case 0x0B: select_line(CUR->sel - visible()); return; /* page up */
	case 0x0C: select_line(CUR->sel + visible()); return; /* page down */
	}
	if (c < ' ' || c == 0x7F) { SysBeep(1); return; }
	/* typing selects: the first row whose name starts with what was typed (a "The " in front left out) */
	if (ev->when - typed_t > 60) ntyped = 0;
	typed_t = ev->when;
	if (ntyped < 15) typed[ntyped++] = (char)lower(c);
	i = a68_list_typed(&list, typed, ntyped);
	if (i >= 0) select_line(a68_list_row_line(&list, i));
}

/* ---- the player window's part ------------------------------------------------------------------------- */
int lib_active(void) { return mode; }
int lib_playing(void) { return mode && lstate == L_PLAYING && radio.st != RS_OFF && !radio.paused; }
int lib_can_stop(void) { return mode && lstate == L_PLAYING; }
int lib_can_skip(int dir) { return mode && Q.n && (dir < 0 ? Q.cur > 0 || pos_s() > 3 : Q.cur + 1 < Q.n || Q.repeat != A68_REPEAT_OFF); }
void lib_leave(void)
{
	if (!mode) return;
	if (lstate == L_PLAYING) radio_stop();
	mode = 0;
	lstate = L_STOPPED;
	radio.next = 0; radio.resume = 0;
	if (lw) { SetPort(lw); draw_bar(); draw_list(); }
}
void lib_play_pause(void)
{
	if (!mode) { if (CUR->kind == V_TRACKS || CUR->kind == V_ALBUMS) lib_play_all(); else if (list.n) open_line(CUR->sel); return; }
	if (lstate != L_PLAYING) { play_at(lstate == L_ENDED ? 0 : Q.cur, 0); return; }
	radio_pause(!radio.paused);
	player_refresh();
	if (lw) { SetPort(lw); draw_bar(); }
}
void lib_stop(void)
{
	if (!mode || lstate != L_PLAYING) return;
	radio_stop();
	lstate = L_STOPPED;
	nb = 0;
	logs("stopped", 0);
	player_refresh();
	if (lw) { SetPort(lw); draw_bar(); draw_list(); }
}
/* Previous: the start of this track more than three seconds in, or on the first; else the one before. Next: the one
   after, round to the first with Repeat; a beep on the last without it (play/core/queue.c's rules) */
void lib_skip(int dir)
{
	int i, restart;
	if (!mode || !Q.n) return;
	if (dir < 0) i = a68q_previous(&Q, pos_s(), &restart);
	else {
		if (Q.cur + 1 >= Q.n && Q.repeat == A68_REPEAT_OFF) { SysBeep(1); return; }
		i = a68q_skip(&Q);
	}
	play_at(i, 0);
}
const char *lib_status(char *b)
{
	if (lstate == L_ENDED) { put_str(b, "Finished")[0] = 0; return b; }
	if (lstate == L_STOPPED) { put_str(b, "Stopped")[0] = 0; return b; }
	return radio_status(b);
}
/* Now Playing in the player window: the cover (64 x 64) at the left, the track, the artist and album, the
   place in the queue; the progress line, which a click moves in the track */
void lib_draw_now(void *win, short W, short H)
{
	char t[160], *p;
	(void)H;
	Rect r = { 0, 0, 72, W }, cr = { 5, 9, 5 + ART, 9 + ART };
	pwin = win;
	SetPort(win);
	EraseRect(&r);
	struct qitem *q = Q.cur >= 0 && Q.cur < Q.n ? qat(Q.cur) : 0;
	if (now_art.state == 2 && now_art.depth != depth_at(win, &cr)) now_art.state = 0;   /* moved to another screen */
	draw_cover(win, q && same_id(now_art.album, q->album) ? &now_art : 0, ART, 9, 5);
	TextFont(0); TextSize(12); TextFace(0);
	MoveTo(84, 18);
	clip_text(q ? q->title : "", W - 94);
	TextFont(3); TextSize(9);
	p = t;
	if (q) {
		p = put_str(p, q->artist);
		if (album_title[0] && Q.cur >= 0) { p = put_str(p, " \xD1 "); p = put_str(p, album_title); }
	}
	*p = 0;
	MoveTo(84, 30);
	clip_text(t, W - 94);
	p = t;
	if (q) {
		p = put_str(p, "Track "); p = put_num(p, (unsigned long)Q.cur + 1); p = put_str(p, " of "); p = put_num(p, (unsigned long)Q.n);
		p = put_str(p, shuffle ? ", shuffled" : "");
		p = put_str(p, Q.repeat != A68_REPEAT_OFF ? ", repeated" : "");
		if (ps && ps->rate && lstate == L_PLAYING) { p = put_str(p, " \xD1 MP3 "); p = put_num(p, ps->bitrate ? ps->bitrate : radio.br); p = put_str(p, " kbit/s"); }
		if (radio.ipv) p = put_str(p, radio.ipv == 6 ? ", IPv6" : ", IPv4");
	}
	*p = 0;
	MoveTo(84, 41);
	clip_text(t, W - 94);
}
void lib_draw_progress(void *win, short W, short H)
{
	char t[60], st[40], *p = t;
	(void)H;
	SetPort(win);
	Rect r = { 46, 84, 54, (short)(W - 10) };
	struct qitem *q = Q.cur >= 0 && Q.cur < Q.n ? qat(Q.cur) : 0;
	unsigned long pos = pos_s(), len = q ? q->secs : 0;
	FrameRect(&r);
	InsetRect(&r, 1, 1);
	short w = r.right - r.left, fillw = len ? (short)((pos > len ? len : pos) * (unsigned long)w / len) : 0;
	if (lstate == L_ENDED) fillw = w;
	Rect a = r, b = r;
	a.right = r.left + fillw; b.left = r.left + fillw;
	static const unsigned char grey[8] = { 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 };
	if (fillw) FillRect(&a, grey);
	if (b.left < b.right) EraseRect(&b);
	Rect tr = { 56, 72, 70, W };
	EraseRect(&tr);
	TextFont(3); TextSize(9);
	if (q) { p = put_time(p, pos); if (len) { p = put_str(p, " of "); p = put_time(p, len); } }
	*p = 0;
	MoveTo(84, 66);
	DrawText(t, 0, (int)strlen(t));
	lib_status(st);
	draw_right(st, (short)(W - 10), 66);
}
int lib_bar_click(Point p, short W)
{
	Rect r = { 42, 84, 58, (short)(W - 10) };
	if (!mode || !PtInRect(pt_long(p), &r) || !qat(Q.cur) || !qat(Q.cur)->secs) return 0;
	unsigned long s = a68_bar_time(p.h - 85, W - 96, qat(Q.cur)->secs);
	if (s >= qat(Q.cur)->secs) s = qat(Q.cur)->secs - 1;
	char t[40];
	put_num(put_str(t, "seek to "), s)[0] = 0;
	logs(t, " s");
	play_at(Q.cur, s);
	return 1;
}

/* ---- menus ----------------------------------------------------------------------------------------------- */
void lib_connect(void)
{
	char a[80], cur[80], *p = cur;
	if (srv_host[0]) {
		p = a68_put_url_host(p, cur + 70, srv_host);
		*p++ = ':'; p = put_num(p, srv_port);
	}
	*p = 0;
	if (!ask_text("Connect to the music server at this address:",
	              "Such as 192.168.1.20:8423 or [fd00::20]:8423. Empty: look again.", cur, a, sizeof a))
		return;                                  /* Cancel */
	nalt = 0;
	lib_show();
	{
		const char *s = a;
		while (*s == ' ') s++;
		if (!*s) { find_server(); return; }
	}
	/* host[:port], [ipv6]:port, ipv6 alone, a URL (play/core/address.c); no port: the server's own, 8000 */
	char h[64];
	unsigned port;
	if (!a68_parse_address(a, A68_DEFAULT_PORT, h, sizeof h, &port)) {
		say_error("That is not an address Aurora68 Play understands. Type a name or number and a port, such as 192.168.1.20:8423.");
		return;
	}
	if (finding) { discover_stop(); finding = 0; }
	depth = 1;
	CUR->sel = CUR->top = 0; CUR->from = 0;
	use_server(h, (unsigned short)port);
	view_changed();
}
void lib_find(void)
{
	char q[40];
	static char last[40];
	if (!srv_host[0]) { lib_show(); return; }
	if (!ask_text("Find in the library:", "Words of an artist's name, an album's title or a track's.", last, q, sizeof q) || !q[0]) return;
	copy(last, q, sizeof last);
	lib_show();
	push_view(V_SEARCH, "", "", 0);
	char *p = put_str(CUR->title, "Found: ");
	copy(p, q, (int)(sizeof CUR->title - 7));
	copy(CUR->q, q, sizeof CUR->q);
	view_changed();
}
int lib_can_play_all(void) { return lw && srv_known && (CUR->kind == V_ALBUMS || CUR->kind == V_TRACKS); }
void lib_play_all(void)
{
	if (CUR->kind == V_TRACKS && list.n) queue_from_rows(-1);
	else if (CUR->kind == V_ALBUMS) ask_queue(0, CUR->id, 0);
	else SysBeep(1);
}
void lib_toggle_shuffle(void)
{
	shuffle = !shuffle;
	if (shuf_cb) SetCtlValue(shuf_cb, shuffle);
	if (mode && Q.n > 1 && shuffle && Q.cur + 1 < Q.n)   /* what is still to come, shuffled; the stream in flight stays */
		a68q_shuffle_upcoming(&Q, seed_now() + 7);
	player_refresh();
}
/* Library > Repeat (Aurora68 Play 1.4, with the shared queue): the queue again from its first song when it ends,
   gaplessly as from one song to the next */
void lib_toggle_repeat(void)
{
	Q.repeat = Q.repeat == A68_REPEAT_OFF ? A68_REPEAT_ALL : A68_REPEAT_OFF;
	logs(Q.repeat ? "repeat: on" : "repeat: off", 0);
	player_refresh();
}
void lib_live(void)
{
	char u[120], n[60];
	if (!srv_host[0]) { lib_show(); return; }
	a68_url(u, sizeof u, srv_host, srv_port, "/live");
	put_str(put_str(n, "Live: "), srv_known ? srv_name : srv_host)[0] = 0;
	lib_leave();
	start_station(n, u);
}
void lib_reload(void)
{
	if (!srv_host[0]) { find_server(); return; }
	if (!srv_known) want_info = 1;
	view_changed();
}

/* ---- the preferences ----------------------------------------------------------------------------------- */
void lib_get_prefs(struct lib_prefs *p)
{
	memset(p, 0, sizeof *p);
	copy(p->host, srv_host, sizeof p->host);
	p->port = srv_port;
	p->shuffle = (short)shuffle;
	p->shown = (short)shown;
	p->repeat = (short)Q.repeat;
	if (lw) {
		Point tl = { 0, 0 };
		SetPort(lw);
		LocalToGlobal(&tl);
		p->r.top = tl.v; p->r.left = tl.h; p->r.bottom = (short)(tl.v + LH); p->r.right = (short)(tl.h + LW);
	} else p->r = saved_r;
}
void lib_set_prefs(const struct lib_prefs *p)
{
	if (p->host[0]) { copy(srv_host, p->host, sizeof srv_host); srv_port = p->port ? p->port : A68_DEFAULT_PORT; want_info = 1; want_list = 1; }
	shuffle = p->shuffle != 0;
	Q.repeat = p->repeat == A68_REPEAT_ALL ? A68_REPEAT_ALL : A68_REPEAT_OFF;   /* the pad of files before 1.4: 0, off */
	saved_r = p->r;
	saved_shown = p->shown;
	have_saved = 1;
	if (saved_shown) lib_show();
}
