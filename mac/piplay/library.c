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
 *
 * Music videos (Aurora68 Play 1.5, 22 November 2026, docs/08 item 109). The owner's library holds music videos
 * beside the albums; the server lists them after an album's tracks when asked with videos=1, which this Play now
 * sends. A video is a row marked "Video:"; playing one plays its sound, as MP3 from the server's /vaudio/ID, exactly
 * as a track plays from /t/ID (a stream with an end, into the card). The picture is for later: the kernel's video
 * card (docs/16) and its program will show it through /vmpeg/ID. The device says it shows no video (its announce
 * carries no video=1), and a video sent to it from another player comes as a V line and plays the same way. An
 * artist with videos and no album opens on its videos (catalog.c's A68_V_ARTIST).
 *
 * Music videos shown (Aurora68 Play 1.6, 23 November 2026, docs/08 item 114). With the Aurora68 Video Decoder card
 * in the Mac (vcard.c finds its driver), a video plays as itself: /vmpeg/ID?w=&h=&start= (play/core/proto.c's
 * path, a size of the video's own shape for the screen the player window is on: a68_vmpeg_size), its MPEG-1 stream
 * into the card through the radio's connection (radio_play_video), its picture in a video window. The queue goes
 * on when a video has played (the card says "ended"), but never gaplessly: a video is a card stream of its own, so
 * the song or video after it, or a video after a song, starts as a new stream once the one before has played out.
 * A seek is a new /vmpeg request with start=S (the card stopped, a new stream started). The device says it shows
 * video (dv.video, the announce's video=1), so the server sends v:ID to it as a video rather than its sound.
 * Without the card everything is as in 1.5.
 *
 * Parity with the macOS Play 1.5 (Aurora68 Play 1.7, 23 November 2026, docs/08 item 120). The owner asked for "the classic
 * mac version up to parity" and for it to find the music servers by itself. What came here:
 *  - Finding the server without being asked: at the first start (no server remembered) and whenever the remembered one
 *    does not answer, the UDP probe runs at once, with the Library window shown or not; every server that answers is
 *    kept (fetch.c), the remembered one (by name) or the only one is taken, and with several and none remembered the
 *    list area names them and Library > Servers lists them to choose from. The choice is kept in the preferences, the
 *    server's name with it (piplay.c's preferences tail), so a server that moved to another address is found again.
 *  - Videos linked to songs: an album's tracks are asked for with full=1&videos=1 (still Mac Roman), whose records end
 *    with the song's video (catalog.c's row.video) and an album's with its count of videos. A song with a video has a
 *    film mark in its row, and Library > Play Video (Command-M) plays the selected song's video, the album's videos after
 *    it, as the macOS Play's film button does. A list too long for the 48 KB answer with the longer records is asked
 *    again without them (fetch.too_long), as 1.6 asked it.
 *  - Controls > Play Videos Instead of Songs: with the video decoder card, a song that has a video is replaced by it, one
 *    for one, in whatever is queued from a list, an album, an artist or Play Next and Add to Queue (instead_on); without
 *    the card the songs play.
 *  - Library > Videos: every video of the library, by artist (/mac/videos); an artist's view begins with a line "Videos"
 *    that opens the artist's (/mac/videos?artist=ID); artists and albums with videos have the film mark.
 *  - Library > Play Next and Add to Queue for the selected song, video, album or artist; Library > Up Next, the queue as
 *    a list (Return plays from that place); Library > Repeat One beside Repeat (the core had it).
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
#include "vcard.h"

extern volatile struct psmb_stream *ps;
static unsigned char pb[256];
static const struct a68_client cl = { 0, 0, 1 };    /* Mac Roman, the short records (what the classic Play always asked),
                                                       and the music videos (1.5: V rows after an album's tracks) */
/* 1.7: an artist's albums and an album's tracks (and a queue fetched with Play Videos Instead of Songs on) with the longer
   records too, still in Mac Roman: their last fields are a song's video and an album's count of videos. The core reads
   records by their fields, so an older server's (shorter) records read as no links */
static const struct a68_client cl_links = { 0, 1, 1 };
static int links_off;                            /* this view's list was too long with them: asked again without */

/* ---- what the library holds now: a page of the view's list (play/core/catalog.c) -------------------------- */
enum { V_ARTISTS = A68_V_ARTISTS, V_ALBUMS = A68_V_ALBUMS, V_TRACKS = A68_V_TRACKS, V_SEARCH = A68_V_SEARCH,
       V_ARTIST = A68_V_ARTIST,             /* an artist with videos and no album: its videos */
       V_VIDEOS = A68_V_VIDEOS, V_ARTVIDEOS = A68_V_ARTVIDEOS,   /* every video; an artist's (1.7) */
       V_QUEUE = 20 };                      /* Up Next: the queue itself, made here, not asked of the server */
#define MAX_ROWS A68_PAGE
#define PAGE     A68_PAGE
static struct a68_row *rows;                     /* list.rows */
static struct a68_list list;
struct view { int kind; char id[9]; char title[48], sub[40]; char q[40]; int sel, top; long from;
              int nvid; };                       /* an artist's albums: its videos, the "Videos" line above them */
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
static int srv_mpeg1;                            /* it makes MPEG-1 for the video card (/vmpeg/, "mpeg1" in its capabilities) */
static int finding;                              /* the UDP probe is out */
static int auto_find;                            /* it is out because the server remembered did not answer */
static int auto_tried;                           /* that was done once for this server (until one answers) */
static int user_find;                            /* Library > Servers > Search Again while a server is known */
static char want_name[48];                       /* the server chosen last (its name): taken again among those found */
static struct disc_found servers[D_MAX];         /* what the last probe found, for Library > Servers */
static int nservers, servers_gen;
static char alt_host[3][64];                     /* the server's other addresses (discovery): tried in turn if this one fails */
static int nalt;
/* 1.8 (docs/08 item 124): the server's addresses by family, from the probe's answer and kept in the preferences, so that
   a remembered server is reached at its IPv6 address first again (Connect By: IPv6 First) and at its IPv4 address when
   that does not answer, and srv_host is the one that answered, for the rest of the session. The owner's SE said "IPv4"
   with IPv6 there: 1.7 kept only the address that had answered, and a server first reached by its IPv4 address (typed,
   or found by the probe before it named an IPv6 address) stayed IPv4 for good */
static char srv_v6[48], srv_v4[2][20];
static int addr_learned;                         /* the probe answered for this server: its addresses are known */
static int want_learn, learn_find;               /* a quiet probe for them (the remembered server answered over IPv4) */
static char v6_bad[48];                          /* an IPv6 address of the server's that did not answer this session */
static int relearned;                            /* the probe asked again after that, once */
static const char *problem;                      /* the list area says this instead of rows */
static char problem_buf[360];                    /* 1.7: the servers found, or a server that does not answer, named in it */

/* ---- the queue (play/core/queue.c): the songs, the one heard (Q.cur), the one whose stream is sent (Q.feed) ---- */
struct qitem { char id[9], album[9]; unsigned long secs; char title[48], artist[40]; char video; unsigned short vw, vh;
               char link[9]; };                  /* 1.7: a song's music video, when the server said */
/* video: a music video, shown on the video decoder card (/vmpeg/, its picture vw x vh, 0 when not known) or without
   the card its sound (/vaudio/) */
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
static int instead;                              /* Controls > Play Videos Instead of Songs (1.7) */
static char album_title[48];                     /* the album of what is heard, for Now Playing */
/* an artist's name as shown (1.8, docs/08 item 124): a name with no letter or digit ("_", a downloader's "nobody", which
   the owner's videos carried as their artist) is no name: "" where something else can stand instead (Now Playing, the
   video window's title, the Library's strip), "Unknown artist" where a name must stand (the heads of the Videos list's
   groups, an artist's row). Mac Roman's letters above $7F (accented ones) count as letters */
static int named(const char *a)
{
	for (; *a; a++) {
		unsigned char c = (unsigned char)*a;
		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0x80 && c <= 0xD8 && c != 0xD0 && c != 0xD1)) return 1;
	}
	return 0;
}
static const char *artist_or(const char *a, const char *instead) { return named(a) ? a : instead; }
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
static int job_links;                            /* it asked with cl_links */
static int want_info, want_list;
static int want_queue;                           /* a queue to fetch: an album or an artist's tracks */
static char q_album[9], q_artist[9], q_start[9];
enum { Q_PLAY, Q_NEXT, Q_ADD };
static int q_mode;                               /* what the fetched tracks do: play, Play Next, Add to Queue (1.7) */
static int q_nolinks;                            /* the queue's answer was too long with the links */
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
static void queue_changed(void);
static int instead_on(void);
static void view_changed(void);
static void queue_rows(void);
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
int lib_repeat(void) { return Q.repeat; }

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
/* the probe (fetch.c): automatic, when no server is remembered or the remembered one did not answer (the address is kept
   meanwhile, so that Quit keeps it in the preferences); or Library > Servers > Search Again. Nothing is asked of a
   server while it is out */
static void find_server(int automatic)
{
	if (finding) return;
	if (!net_open() || !discover_start()) {
		set_problem("Aurora68 Play could not look for a music server: MacTCP (or Riptide) is not there.");
		return;
	}
	finding = 1;
	learn_find = automatic == 2;
	auto_find = automatic == 1;
	if (learn_find) { logs("asking the probe for the server's other addresses (Connect By: IPv6 First)", 0); return; }
	if (!srv_known) set_problem(automatic && srv_host[0] ? problem_buf : "Looking for music servers on the network\xC9");
	logs(automatic ? "looking for music servers (the probe)" : "looking for music servers again (Search Again)", 0);
}
static int same_text(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int colon(const char *s) { while (*s) if (*s++ == ':') return 1; return 0; }
static const char *connect_by(void)
{
	return net_v4only ? "IPv4 Only" : !net_is_v6() ? "IPv4: no .IPP6 here" : net_prefer == 4 ? "IPv4 First" : "IPv6 First";
}
/* the server's addresses in the order to try them (1.8): Connect By's family first; for IPv4 the address the answer
   came from, which is known to reach this Mac, before the one the server names (which may be its container's); IPv6
   only through Riptide's .IPP6 and not with IPv4 Only */
static int addr_order(const char **out)
{
	int n = 0, i, v6ok = srv_v6[0] && !net_v4only && net_is_v6();
	if (v6ok && net_prefer != 4) out[n++] = srv_v6;
	for (i = 0; i < 2; i++) if (srv_v4[i][0]) out[n++] = srv_v4[i];
	if (v6ok && net_prefer == 4) out[n++] = srv_v6;
	return n;
}
/* the others after the first, as the ones to try if it does not answer; the log says the order */
static void addr_alts(const char **c, int n, int from)
{
	char t[160], *p;
	int i;
	for (nalt = 0, i = from; i < n && nalt < 3; i++) copy(alt_host[nalt++], c[i], sizeof alt_host[0]);
	p = put_str(put_str(t, "the server's addresses ("), connect_by());
	p = put_str(p, "): "); p = put_str(p, c[0]);
	for (i = 0; i < nalt; i++) { p = put_str(p, i ? ", then " : " first, then "); p = put_str(p, alt_host[i]); }
	*p = 0;
	logs(t, 0);
}
static void learn_addrs(const struct disc_found *f)
{
	copy(srv_v6, f->d.v6, sizeof srv_v6);
	copy(srv_v4[0], f->host, sizeof srv_v4[0]);
	srv_v4[1][0] = 0;
	if (f->d.v4[0] && !same_text(f->d.v4, f->host)) copy(srv_v4[1], f->d.v4, sizeof srv_v4[1]);
	addr_learned = 1;
}
/* server k of those found: its addresses in the order to try them, the others kept in case the first fails; the views
   start again at Artists when it is another server than the one in use */
static void connect_found(int k)
{
	const char *c[3];
	struct disc_found *f = &servers[k];
	int n, same_srv = srv_known && same_text(f->d.name, srv_name) && f->d.port == srv_port;
	char first[64];
	learn_addrs(f);
	n = addr_order(c);
	if (!n) { set_problem("The music server's answer named no address this Mac can use."); return; }
	copy(first, c[0], sizeof first);
	addr_alts(c, n, 1);
	if (!same_srv && (!same_text(first, srv_host) || f->d.port != srv_port)) {
		depth = 1;
		CUR->sel = CUR->top = 0; CUR->from = 0;
	}
	use_server(first, (unsigned short)f->d.port);
	copy(srv_name, f->d.name, sizeof srv_name);
	copy(want_name, f->d.name, sizeof want_name);
	view_changed();
}
/* is server k the one in use? (its address is one of those it named, and the port) */
static int found_is_current(int k)
{
	const char *c[3];
	int n = a68_disc_hosts(&servers[k].d, servers[k].host, A68_IPV4, 1, c), i;
	if (servers[k].d.port != srv_port) return 0;
	for (i = 0; i < n; i++) if (same_text(c[i], srv_host)) return 1;
	return 0;
}
static char *put_server(char *p, int k)
{
	p = put_str(p, servers[k].d.name[0] ? servers[k].d.name : "Music server");
	p = put_str(p, " ("); p = put_str(p, servers[k].host); *p++ = ':'; p = put_num(p, servers[k].d.port);
	return put_str(p, ")");
}
/* the probe has ended: take the server remembered (by name), or the only one; with several, the owner chooses */
static void probe_done(void)
{
	int k, pick = -1;
	int was_auto = auto_find, was_user = user_find, was_learn = learn_find;
	finding = 0; auto_find = 0; user_find = 0; learn_find = 0;
	if (disc.st == D_FOUND) {
		nservers = disc.n;
		memcpy(servers, disc.found, sizeof servers[0] * (unsigned long)nservers);
	} else nservers = 0;
	servers_gen++;
	if (was_learn) {                             /* 1.8: the server in use, reached over IPv4: has it an IPv6 address? */
		addr_learned = 1;                        /* asked once; Search Again asks again */
		for (k = 0; k < nservers; k++)
			if (servers[k].d.port == srv_port && (same_text(servers[k].d.name, srv_name) || found_is_current(k))) break;
		if (k == nservers) { logs("the probe did not hear from the server in use: its address stays ", srv_host); return; }
		learn_addrs(&servers[k]);
		if (srv_v6[0] && same_text(srv_v6, v6_bad)) logs("the server names the IPv6 address that did not answer: IPv4 for this session, ", srv_host);
		else if (srv_v6[0] && net_is_v6() && !net_v4only && net_prefer != 4 && !colon(srv_host)) {
			const char *c[3];
			int n = addr_order(c);
			logs("the server also has the IPv6 address ", srv_v6);
			addr_alts(c, n, 1);
			copy(srv_host, c[0], sizeof srv_host);
			fetch_forget_host();
			want_info = 1;                       /* asked over IPv6 now; if that does not answer, the IPv4 address again */
		} else logs("the server names no IPv6 address: it stays at ", srv_host);
		return;
	}
	if (!nservers) {
		if (was_user && srv_known) { message_box("No music server answered on this network. The one in use stays.", 32); return; }
		if (was_auto && srv_host[0]) {
			char *p = put_str(problem_buf, "The music server at ");
			p = a68_put_url_host(p, problem_buf + 80, srv_host); *p++ = ':'; p = put_num(p, srv_port);
			p = put_str(p, " does not answer, and no other music server answered on this network. Is it running? "
			               "Library > Servers > Search Again looks again; Reload tries this one again.");
			*p = 0;
			set_problem(problem_buf);
		} else
			set_problem("No music server answered on this network. Choose Library > Servers > Connect to Server\xC9 and type "
			            "its address, such as 192.168.1.20:8423 (the music server's web page shows it), or Search Again.");
		return;
	}
	for (k = 0; k < nservers && pick < 0; k++)
		if (want_name[0] && same_text(servers[k].d.name, want_name)) pick = k;   /* the one chosen before */
	if (was_user && srv_known) {                 /* asked while connected: the one in use stays unless it is the only other */
		for (k = 0; k < nservers; k++) if (found_is_current(k)) return;
		if (nservers > 1) { message_box("More than one music server answered: choose one in Library > Servers.", 32); return; }
		pick = 0;
	}
	if (pick < 0 && nservers == 1) pick = 0;
	if (pick >= 0) {
		if (was_auto && srv_host[0] && !found_is_current(pick)) logs("the remembered server did not answer: taking the one found, ", servers[pick].d.name);
		connect_found(pick);
		return;
	}
	{   /* several, none chosen before: named in the list area, chosen from the menu */
		char *p = put_str(problem_buf, "Music servers on this network: ");
		for (k = 0; k < nservers && p < problem_buf + 100; k++) { if (k) p = put_str(p, ", "); p = put_server(p, k); }
		p = put_str(p, ". Choose one in Library > Servers.");
		*p = 0;
		srv_known = 0;
		set_problem(problem_buf);
		lib_show();
	}
}
/* Library > Servers: those found, the one in use checked (and listed first if the probe did not find it) */
int lib_servers_gen(void) { return servers_gen + (srv_known ? 1000 : 0); }
int lib_servers(void) { int k; for (k = 0; k < nservers; k++) if (found_is_current(k)) return nservers; return nservers + (srv_known ? 1 : 0); }
const char *lib_server_text(int i, char *buf, int *current)
{
	int k, extra = srv_known;
	char *p = buf;
	for (k = 0; k < nservers; k++) if (found_is_current(k)) extra = 0;
	*current = 0;
	if (extra && i == 0) {
		p = put_str(p, srv_name[0] ? srv_name : "Music server");
		p = put_str(p, " ("); p = a68_put_url_host(p, buf + 70, srv_host); *p++ = ':'; p = put_num(p, srv_port); p = put_str(p, ")");
		*current = 1;
	} else if (i - extra >= 0 && i - extra < nservers) {
		p = put_server(p, i - extra);
		*current = srv_known && found_is_current(i - extra);
	}
	*p = 0;
	return buf;
}
void lib_choose_server(int i)
{
	int k, extra = srv_known;
	for (k = 0; k < nservers; k++) if (found_is_current(k)) extra = 0;
	if (extra && i == 0) { lib_show(); return; }
	i -= extra;
	if (i < 0 || i >= nservers) return;
	lib_show();
	if (finding) { discover_stop(); finding = 0; }
	auto_tried = 0;
	logs("server chosen: ", servers[i].d.name);
	connect_found(i);
}
void lib_search_again(void)
{
	lib_show();
	if (finding) return;
	user_find = 1;
	find_server(0);
}
int lib_finding(void) { return finding; }

/* ---- the list's views -------------------------------------------------------------------------------- */
static void view_changed(void)
{
	a68_list_init(&list, rows, list.max, CUR->kind);
	list.from = CUR->from;
	memset(thumb_state, 0, sizeof thumb_state);
	want_list = 1;
	links_off = 0;
	if (job == J_LIST || job == J_THUMBS || job == J_VIEWART) { fetch_cancel(); job = J_NONE; }
	view_art.state = 0;
	problem = 0;
	if (CUR->kind == V_QUEUE) { want_list = 0; queue_rows(); }   /* Up Next: the queue itself */
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

/* the lines of the list: a "previous page" line, the rows, a "next page" line (play/core/catalog.c); in an artist's
   albums with videos (1.7) a first line "Videos" before them, which opens the artist's videos */
static int vline(void) { return CUR->kind == V_ALBUMS && CUR->nvid && !list.from && !problem; }
static int has_prev(void) { return a68_list_has_prev(&list); }
static int nlines(void) { return a68_list_lines(&list) + vline(); }
static int line_row(int i)                       /* -1 the previous page, -2 the next, -3 the artist's videos */
{
	if (vline()) { if (i == 0) return -3; i--; }
	return a68_list_line_row(&list, i);
}
static int row_line(int r) { return a68_list_row_line(&list, r) + vline(); }
static short row_h(void) { return CUR->kind == V_ALBUMS ? R_ALBUM : R_LINE; }
static Rect list_rect(void)
{
	Rect r = { (short)(HEAD_H + 1 + (CUR->kind == V_TRACKS ? ALBUM_H : 0)), 0, (short)(LH - BAR_H - 1), (short)(LW - 15) };
	return r;
}
static int visible(void) { Rect r = list_rect(); int n = (r.bottom - r.top) / row_h(); return n < 1 ? 1 : n; }
static int tracking;                             /* the scroll bar's arrows or page areas held (1.8) */
static void fix_scroll(void)
{
	int max = nlines() - visible();
	if (max < 0) max = 0;
	if (CUR->top > max) CUR->top = max;
	if (CUR->top < 0) CUR->top = 0;
	if (!sbar) return;
	SetCtlMax(sbar, max);
	SetCtlValue(sbar, CUR->top);
	/* not while the Control Manager tracks it (1.8): it has the arrow highlighted, and this unhighlighted it and drew the
	   whole bar again at every step */
	if (!tracking) HiliteControl(sbar, active && max ? 0 : 255);
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
	if (!a68_read_head(&r, &h) || !a68_info_of(&r, &inf)) {
		dv_stop(&dev);
		fail_text("That address answers, but not as Aurora68's music server.");
		want_list = 0;
		return;
	}
	copy(srv_name, inf.name, sizeof srv_name);
	copy(want_name, inf.name, sizeof want_name);  /* the one chosen, by name: found again if its address changes */
	srv_known = 1;
	auto_tried = 0;
	servers_gen++;
	{   /* "mpeg1" among what the server can do (servers of 22 November 2026 on): its /vmpeg/ for the video card. Without
	       it a video plays its sound, as in 1.5, and the device does not say it shows video */
		const char *c = inf.caps ? inf.caps : "";
		int k;
		srv_mpeg1 = 0;
		for (; *c; c++) {
			for (k = 0; "mpeg1"[k] && c[k] == "mpeg1"[k]; k++) { }
			if (!"mpeg1"[k] && (c[k] == ',' || !c[k]) && (c == inf.caps || c[-1] == ',')) srv_mpeg1 = 1;
		}
		dev.video = vc_present() && srv_mpeg1;
	}
	set_title();
	dv_set_generation(&dev, inf.generation);       /* the library's generation (servers of 22 November 2026 on) */
	dev.gen_changed = 0;
	if (dev_ready) dv_start(&dev, secs_now());     /* this server (perhaps another one): announce now */
	char t[120], *p = put_str(t, "the server is \"");
	p = put_str(p, srv_name); p = put_str(p, "\": "); p = put_num(p, inf.artists); p = put_str(p, " artists, ");
	p = put_num(p, inf.albums); p = put_str(p, " albums, "); p = put_num(p, inf.tracks); p = put_str(p, " tracks");
	*p = 0;
	logs(t, fetch.ipv == 6 ? " (IPv6)" : " (IPv4)");
	p = put_str(put_str(t, "the server answers at "), srv_host);
	p = put_str(p, fetch.ipv == 6 ? " (IPv6): " : " (IPv4): "); p = put_str(p, "that address is used from now on (");
	p = put_str(p, connect_by()); *p++ = ')'; *p = 0;
	logs(t, 0);
	/* the one that answered is kept for the session; the ones not tried stay, for a later failure (the other family) */
	/* reached over IPv4 with IPv6 wanted and the server's IPv6 address not known: ask the probe for it, once (1.8) */
	if (fetch.ipv == 4 && (!addr_learned || (v6_bad[0] && !relearned)) && net_is_v6() && !net_v4only && net_prefer != 4) {
		want_learn = 1;
		if (v6_bad[0]) relearned = 1;            /* once a session */
	}
	/* (and when its IPv6 address did not answer: the server may have another now, which the probe's answer names) */
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
		if (k >= 0) CUR->sel = row_line(k);
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
/* a track's stream: http://HOST:PORT/t/ID?start=S (an IPv6 address in brackets); a video's sound /vaudio/ID?start=S */
static unsigned vm_w, vm_h;                      /* the size the video in the card's window is asked at */
static int shows_video(const struct qitem *q) { return q && q->video && vc_present() && srv_mpeg1; }
static void track_url(char *u, long max, int i, unsigned long start)
{
	char path[60];
	struct qitem *q = qat(i);
	if (shows_video(q)) a68_path_vmpeg(q->id, vm_w, vm_h, a68_vmpeg_rate(vm_w, vm_h), start, path, sizeof path);
	else if (q && q->video) a68_path_vaudio(q->id, start, path, sizeof path);
	else a68_path_stream(q ? q->id : "", start, path, sizeof path);
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
	if (!mode) return 0;
	f = a68q_follow(&Q, Q.feed);
	if (f >= 0 && shows_video(qat(f))) { logs("next: a video, once this song has played", 0); return 0; }   /* lib_poll starts it */
	if ((f = a68q_feed_next(&Q)) < 0) return 0;
	feed_start = 0;                              /* the new stream from its start (a lost one resumes from here) */
	track_url(url, max, f, 0);
	if (nb < 8) { bounds[nb].at = at; bounds[nb].idx = f; nb++; }
	logs("next in the stream: ", qat(f)->title);
	return 1;
}
static void resume_url(char *url, int max, unsigned long secs)
{
	feed_start += secs;
	if (shows_video(qat(Q.feed))) offset_s = feed_start;   /* a video's new stream counts from there */
	track_url(url, max, Q.feed, feed_start);
}
/* the stream's connection to the server failed (radio.c, 1.8): the server's next address for the stream's next try, and
   for the lists from then on */
static int other_host(void)
{
	char t[140], *p;
	int k;
	if (!nalt) return 0;
	p = put_str(put_str(put_str(t, "the stream could not reach "), srv_host), ": the server's other address, ");
	copy(srv_host, alt_host[0], sizeof srv_host);
	for (k = 1; k < nalt; k++) copy(alt_host[k - 1], alt_host[k], sizeof alt_host[0]);
	nalt--;
	p = put_str(p, srv_host);
	*p = 0;
	logs(t, 0);
	fetch_forget_host();
	return 1;
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
	radio.other_host = other_host;
	p = name;
	if (named(q->artist)) { p = put_str(p, q->artist); p = put_str(p, " - "); }
	copy(p, q->title, (int)(sizeof name - (unsigned long)(p - name)));
	if (shows_video(q)) {
		/* the picture at a size of the video's shape for the screen the player window is on; the same video again (a
		   seek, Previous on it) keeps its window where it is */
		static char shown_id[9];
		if (!same_id(shown_id, q->id) || !vc_window()) {
			vc_size_for(player_window(), q->vw, q->vh, &vm_w, &vm_h);
			copy(shown_id, q->id, 9);
			{   /* its title the artist, an em dash, the title (1.8: the owner, "add the artist name into the window also") */
				char wt[100], *w = wt;
				if (named(q->artist)) { w = put_str(w, q->artist); w = put_str(w, " \xD1 "); }
				copy(w, q->title, (int)(sizeof wt - (unsigned long)(w - wt)));
				vc_window_open(wt, vm_w, vm_h, player_window());
			}
		}
	} else vc_window_dispose();
	track_url(u, sizeof u, i, start);
	want_now_art();
	if (!(shows_video(q) ? radio_play_video(name, u) : radio_play_track(name, u))) {
		const char *e = radio.error;
		radio.error = 0;
		lstate = L_STOPPED;
		say_error(e ? e : "The track could not be played.");
	} else lstate = L_PLAYING;
	player_refresh();
	queue_changed();
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
/* Play Videos Instead of Songs (1.7): on, the video decoder card there and a server that makes MPEG-1; a song with a video
   is then queued as its video, one for one, so that the place chosen in a list is the place played */
static int instead_on(void) { return instead && vc_present() && srv_mpeg1; }
int lib_instead(void) { return instead; }
void lib_toggle_instead(void)
{
	instead = !instead;
	logs(instead ? "videos instead of songs: on" : "videos instead of songs: off", vc_present() ? 0 : " (no video decoder card: the songs play)");
}
/* a song's item turned into its video: v is the video's row when the list has it (its title, length and shape), else
   only its id is known */
static void to_video(struct qitem *q, const struct a68_row *v)
{
	char album[9];
	copy(album, q->album, 9);                    /* the album's cover stays */
	if (v) { fill_item(q, v->id, v->album[0] ? v->album : album, v->text, v->sub, v->secs); q->vw = v->vw; q->vh = v->vh; }
	else { copy(q->id, q->link, 9); q->secs = 0; q->vw = q->vh = 0; }
	q->video = 1;
	q->link[0] = 0;
}
/* a row of the list as a queue item (a song, or with instead_on its video when it has one) */
static void row_item(struct qitem *q, int i)
{
	struct a68_row *r = &rows[i];
	fill_item(q, r->id, r->album, r->text, r->sub, r->secs);
	q->vw = r->vw; q->vh = r->vh;
	q->video = r->kind == 'V';
	copy(q->link, r->video, 9);
	if (q->link[0] && instead_on()) { int k = a68_list_find(&list, q->link); to_video(q, k >= 0 ? &rows[k] : 0); }
}
/* the queue from the rows of a tracks view (start at row s, -1 for Play); in order from the chosen one, or the
   chosen one first and the rest shuffled. An album's tracks, without its videos; a video chosen, or an artist's
   view of videos: the videos (their sound) */
static void queue_from_rows(int s)
{
	int i, n = 0, at = 0;
	char want = CUR->kind == V_ARTIST || CUR->kind == V_VIDEOS || CUR->kind == V_ARTVIDEOS ||
	            (s >= 0 && s < list.n && rows[s].kind == 'V') ? 'V' : 'T';
	if (!qitems) return;
	for (i = 0; i < list.n && n < MAX_Q; i++) {
		if (rows[i].kind != want) continue;
		if (i == s) at = n;
		row_item(&qitems[n++], i);
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
	q_mode = Q_PLAY;
	copy(q_album, album ? album : "", 9);
	copy(q_artist, artist ? artist : "", 9);
	copy(q_start, start ? start : "", 9);
	want_queue = 1;
	if (job == J_THUMBS || job == J_VIEWART) { fetch_cancel(); job = J_NONE; }
}
/* the songs' videos of a queue's answer (V records after the tracks), for Play Videos Instead of Songs */
#define MAX_QV 24
static struct a68_row *qv;                       /* MAX_QV rows, made at the first need */
static int nqv;
static void got_queue(void)
{
	struct a68_rec r;
	struct a68_head h;
	struct a68_track t;
	struct a68_video v;
	int at = 0, n = 0, k, first = 0, place = 0, i;
	int adding = q_mode != Q_PLAY;
	a68_rec_init(&r, fetch.body, fetch.len);
	if (fetch.status != 200 || !a68_read_head(&r, &h)) { say_error("The music server could not give those tracks."); return; }
	if (!qitems) return;
	if (adding && !mode) a68q_clear(&Q);         /* nothing of the library plays: the queue is made of these */
	if (adding) place = first = q_mode == Q_NEXT && mode ? a68q_next_place(&Q) : Q.n;
	if (instead_on() && !qv) qv = mac_newptr(sizeof(struct a68_row) * MAX_QV);
	nqv = 0;
	while ((k = a68_next_item(&r, &t, &v))) {
		struct qitem item;
		if (k == 'V') {                          /* a song's video, kept to replace it */
			if (qv && nqv < MAX_QV) {
				struct a68_row *w = &qv[nqv++];
				memset(w, 0, sizeof *w);
				w->kind = 'V'; copy(w->id, v.id, 9); copy(w->text, v.title, sizeof w->text); copy(w->sub, v.artist, sizeof w->sub);
				copy(w->album, v.album, 9); w->secs = v.secs;
				w->vw = (unsigned short)(v.width & 0xFFFF); w->vh = (unsigned short)(v.height & 0xFFFF);
			}
			continue;
		}
		fill_item(&item, t.id, t.album, t.title, t.artist, t.secs);
		copy(item.link, t.video, 9);
		if (adding) { if (a68q_insert(&Q, place, &item) < 0) break; place++; n++; continue; }
		if (n >= MAX_Q) break;
		if (q_start[0] && same_id(t.id, q_start)) at = n;
		qitems[n++] = item;
	}
	if (!n) return;
	if (instead_on())                            /* each song with a video as its video, one for one */
		for (i = adding ? first : 0; i < (adding ? first + n : n); i++) {
			struct qitem *q = adding ? qat(i) : &qitems[i];
			const struct a68_row *vr = 0;
			if (!q || !q->link[0]) continue;
			for (k = 0; k < nqv; k++) if (same_id(qv[k].id, q->link)) vr = &qv[k];
			to_video(q, vr);
		}
	if (adding) {                                /* Play Next, Add to Queue: played at once if nothing plays */
		char tt[40];
		put_str(put_num(tt, (unsigned long)n), q_mode == Q_NEXT ? " to play next" : " added to the queue")[0] = 0;
		logs(tt, 0);
		if (!mode || lstate != L_PLAYING) play_at(first, 0);
		else { player_refresh(); queue_changed(); }
		return;
	}
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
		else { player_refresh(); queue_changed(); }
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
		if ((c.kind == 'T' || c.kind == 'V') && (pc.act == A68_ACT_PLAY || pc.act == A68_ACT_ADD || pc.act == A68_ACT_NEXT) && qitems) {
			int at;
			if (pc.act == A68_ACT_PLAY && !pc.n) { lib_leave(); a68q_clear(&Q); }   /* the queue replaced (once its first track is here) */
			/* "next": after what is being received (the stream may already be on the track after the one heard) */
			at = pc.act == A68_ACT_NEXT && mode ? a68q_next_place(&Q) + pc.n : pc.act == A68_ACT_PLAY ? pc.n : Q.n;
			if (!pc.n) pc.first = at > Q.n ? Q.n : at;
			fill_item(&item, c.tid, "", c.title, c.artist, c.secs);   /* no album id in a command's tracks: no cover for them */
			item.video = c.kind == 'V';              /* a music video: shown with the card, else its sound */
			item.vw = (unsigned short)(c.width & 0xFFFF); item.vh = (unsigned short)(c.height & 0xFFFF);   /* full=1 only: 0, 4:3 */
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
	dv_init(&dev, id, name, "1.8", "classic");
	dev.client = cl;                             /* videos=1: a video sent here comes as a V line */
	dev.video = vc_present() && srv_mpeg1;       /* 1.6: with the video decoder card it shows them (the announce's video=1) */
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
	fetch_next_quick = nalt > 0;                 /* 1.8: another address to try: 5 s for this one */
	if (fetch_start(srv_host, srv_port, path)) job = j;
}
static void next_job(void)
{
	char path[220];
	int i;
	if (!srv_host[0] || finding) return;
	job_links = 0;
	if (want_learn && srv_known && !want_info) { want_learn = 0; find_server(2); return; }
	if (want_info) { want_info = 0; a68_path_info(&cl, path, sizeof path); start_job(J_INFO, path); return; }
	if (want_list && srv_known) {
		struct view *v = CUR;
		want_list = 0;
		job_links = !links_off && (v->kind == V_ALBUMS || v->kind == V_TRACKS);   /* film marks: an album's videos, a song's */
		a68_list_path(job_links ? &cl_links : &cl, v->kind, v->id, v->q, v->from, path, sizeof path);
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
		job_links = instead_on() && !q_nolinks;      /* the songs' videos, to play instead */
		a68_path_tracks(job_links ? &cl_links : &cl, q_album, q_artist, 0, MAX_Q, path, sizeof path);
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
		int first = CUR->top - has_prev() - vline(), last = first + visible();
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
		/* the addresses of the server taken are tried best first (play/core/address.c, connect_found): the family
		   Connect By names first; for IPv4 the address the answer came from, which is known to reach this Mac, before
		   the one the server names (which may be its container's); IPv6 only through Riptide's .IPP6 */
		discover_poll();
		if (disc.st == D_FOUND || disc.st == D_NONE) probe_done();
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
			if ((j == J_INFO || fetch.unreachable) && nalt) {   /* the server's other addresses, each once */
				char h[64], t[160], *p;
				int k;
				copy(h, alt_host[0], sizeof h);
				for (k = 1; k < nalt; k++) copy(alt_host[k - 1], alt_host[k], sizeof alt_host[0]);
				nalt--;
				if (colon(srv_host)) copy(v6_bad, srv_host, sizeof v6_bad);
				p = put_str(put_str(put_str(t, "no answer from "), srv_host), " (the line above says why): trying ");
				p = put_str(p, h);
				*p = 0;
				logs(t, 0);
				if (j == J_INFO) use_server(h, srv_port);
				else {                               /* 1.8: a list or a cover: the same request at the other address */
					copy(srv_host, h, sizeof srv_host);
					fetch_forget_host();
					if (j == J_LIST) want_list = 1;
					else if (j == J_QUEUE) want_queue = 1;
					else if (j == J_NOWART) now_art.state = 0;
					else if (j == J_BARART) bar_art.state = 0;
					else if (j == J_VIEWART) view_art.state = 0;
					else if (j == J_THUMBS) { for (k = 0; k < nthumb; k++) thumb_state[thumb_rows[k]] = 0; }
				}
				fetch.st = FS_IDLE;
				return;
			}
			if (j == J_INFO && !auto_tried) {       /* the server remembered does not answer: look for one (1.7) */
				char *p = put_str(problem_buf, "The music server at ");
				auto_tried = 1;
				p = a68_put_url_host(p, problem_buf + 80, srv_host); *p++ = ':'; p = put_num(p, srv_port);
				p = put_str(p, " does not answer. Looking for music servers on the network\xC9");
				*p = 0;
				logs("the server does not answer: ", fetch.error);
				want_list = 1;
				fetch.st = FS_IDLE;
				find_server(1);
				return;
			}
			if (j == J_LIST && job_links && fetch.too_long) { links_off = 1; want_list = 1; logs("the list again without the links (too long with them)", 0); }
			else if (j == J_QUEUE && job_links && fetch.too_long) { q_nolinks = 1; want_queue = 1; }
			else if (j == J_INFO || j == J_LIST) { fail_text(fetch.error); if (j == J_INFO) want_list = 1; }
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
			case J_QUEUE: got_queue(); q_nolinks = 0; break;
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
			if (lw) { SetPort(lw); draw_bar(); if (CUR->kind == V_TRACKS || CUR->kind == V_QUEUE) draw_list(); }
		}
		if (radio.ended && lstate == L_PLAYING) {
			/* a video has played, or a song before a video (next_url sends no stream into or out of a video): what
			   follows starts as a stream of its own. So does a song put in after the last stream had come whole (Play
			   Next or Add to Queue near a song's end, 1.7: seen in the testbed, where such a song was never played) */
			int f = a68q_follow(&Q, Q.feed);
			radio.ended = 0;
			if (f >= 0) {
				logs(shows_video(qat(Q.feed)) || shows_video(qat(f)) ? "next after the video's stream: " : "next, as a stream of its own: ",
				     qat(f)->title);
				play_at(f, 0);
				return;
			}
			vc_window_dispose();
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
		if (LM_TICKS - last_draw >= 30) {
			last_draw = LM_TICKS;
			if (lw) { SetPort(lw); draw_bar(); }
			if (vc_window_shown() && Q.cur >= 0 && Q.cur < Q.n) {   /* the video window's strip: the time (1.8) */
				char t[40], *p = put_time(t, pos_s());
				if (qat(Q.cur)->secs) { p = put_str(p, " of "); p = put_time(p, qat(Q.cur)->secs); }
				if (radio.paused) p = put_str(p, ", paused");
				*p = 0;
				vc_strip(t);
			}
		}
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
	case 'A':
		if (r->count || !r->videos) { p = put_num(p, r->count); p = put_str(p, r->count == 1 ? " album" : " albums"); }
		if (r->videos) { if (r->count) p = put_str(p, ", "); p = put_num(p, r->videos); p = put_str(p, r->videos == 1 ? " video" : " videos"); }
		break;
	case 'L':
		p = put_num(p, r->count); p = put_str(p, r->count == 1 ? " track" : " tracks");
		if (r->videos) { p = put_str(p, ", "); p = put_num(p, r->videos); p = put_str(p, r->videos == 1 ? " video" : " videos"); }
		break;
	case 'T': case 'V': if (r->secs) p = put_time(p, r->secs); break;
	}
	*p = 0;
}
static int playing_row(int i)
{
	if (CUR->kind == V_QUEUE) return mode && lstate != L_STOPPED && i == Q.cur;   /* Up Next: by its place */
	return mode && lstate != L_STOPPED && Q.cur >= 0 && Q.cur < Q.n && (rows[i].kind == 'T' || rows[i].kind == 'V') &&
	       same_id(rows[i].id, qat(Q.cur)->id);
}
/* the film mark (1.7): a song with a music video, an artist or an album with videos, the videos' lists; 11 x 8, its
   bottom on the text's base line */
static void draw_film(void *w, short x, short base)
{
	static const unsigned short film[8] = { 0xFFE0, 0xAAA0, 0xFFE0, 0x8020, 0x8020, 0xFFE0, 0xAAA0, 0xFFE0 };
	struct { const void *base; short rb; Rect bounds; } bm = { film, 2, { 0, 0, 8, 16 } };
	Rect src = { 0, 0, 8, 11 }, dst = { (short)(base - 8), x, base, (short)(x + 11) };
	ForeColor(33); BackColor(30);
	CopyBits(&bm, (char *)w + 2, &src, &dst, 1 /* srcOr */, 0);
}
#define FILM_W 15                                /* the mark and the space after it */
static void draw_line(int li, Rect *lr)
{
	short h = row_h(), y = (short)(lr->top + (li - CUR->top) * h);
	Rect r = { y, lr->left, (short)(y + h), lr->right };
	char right[24];
	int i = line_row(li);
	EraseRect(&r);
	TextFont(3); TextSize(9); TextFace(0);
	if (i == -3) {                               /* an artist's videos, above its albums */
		char t[40], *p;
		draw_film(lw, 18, (short)(y + 22));
		TextFace(1);
		MoveTo(50, (short)(y + 15));
		DrawText("Videos", 0, 6);
		TextFace(0);
		p = put_num(t, (unsigned long)CUR->nvid); p = put_str(p, CUR->nvid == 1 ? " music video" : " music videos");
		*p = 0;
		MoveTo(50, (short)(y + 28));
		DrawText(t, 0, (int)strlen(t));
	} else if (i < 0) {
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
		clip_text(right, lr->right - 60 - FILM_W);
		if (rows[i].videos) draw_film(lw, (short)(50 + TextWidth(right, 0, (int)strlen(right)) + 5), (short)(y + 28));
	} else {
		short x = 8;
		int film = (rows[i].kind == 'T' && rows[i].video[0]) || (rows[i].kind == 'A' && rows[i].videos);
		int vids = CUR->kind == V_VIDEOS || CUR->kind == V_ARTVIDEOS;
		row_texts(i, right);
		short rw = (short)TextWidth(right, 0, (int)strlen(right)), room;
		if ((rows[i].kind == 'T' || rows[i].kind == 'V') && (CUR->kind == V_TRACKS || CUR->kind == V_QUEUE)) {
			char n[8];
			if (CUR->kind == V_QUEUE) put_num(n, (unsigned long)i + 1)[0] = 0;   /* Up Next: the place in the queue */
			else if (rows[i].kind == 'V') n[0] = 0;  /* a video: no number, "Video:" before its title */
			else put_num(n, rows[i].num ? rows[i].num : (unsigned long)i + 1)[0] = 0;
			if (playing_row(i)) put_str(n, "\xA5")[0] = 0;   /* a bullet by the track that plays */
			draw_right(n, 22, (short)(y + 12));
			x = 30;
		}
		if (playing_row(i) && CUR->kind != V_TRACKS && CUR->kind != V_QUEUE) { MoveTo(x, (short)(y + 12)); DrawText("\xA5 ", 0, 2); x += 10; }
		room = (short)(lr->right - rw - 16 - (film ? FILM_W : 0));
		if (vids) {                              /* the videos' lists: the mark, and (every video) the artist once a group */
			x = 18;                              /* the columns the same with the bullet or without */
			draw_film(lw, x, (short)(y + 11));
			x += FILM_W;
			if (CUR->kind == V_VIDEOS) {
				short col = (short)((lr->right - x) / 3);
				if (i == 0 || !same_text(rows[i - 1].sub, rows[i].sub)) {
					TextFace(1);
					MoveTo(x, (short)(y + 12));
					clip_text(artist_or(rows[i].sub, "Unknown artist"), col - 8);
					TextFace(0);
				}
				x += col;
			}
			MoveTo(x, (short)(y + 12));
			clip_text(rows[i].text, room - x);
		} else if (CUR->kind == V_SEARCH || CUR->kind == V_QUEUE || rows[i].kind == 'V') {
			char t[100], *p = t;
			MoveTo(x, (short)(y + 12));
			p = put_str(p, rows[i].kind == 'A' ? "Artist: " : rows[i].kind == 'L' ? "Album: " : rows[i].kind == 'V' ? "Video: " : "");
			p = put_str(p, rows[i].kind == 'A' ? artist_or(rows[i].text, "Unknown artist") : rows[i].text);
			if (named(rows[i].sub) && rows[i].kind != 'A' && (CUR->kind == V_SEARCH || CUR->kind == V_QUEUE)) { p = put_str(p, " \xD1 "); p = put_str(p, rows[i].sub); }
			*p = 0;
			clip_text(t, room - x);
		} else { MoveTo(x, (short)(y + 12)); clip_text(rows[i].kind == 'A' ? artist_or(rows[i].text, "Unknown artist") : rows[i].text, room - x); }
		if (film) draw_film(lw, (short)(lr->right - 8 - rw - FILM_W - 1), (short)(y + 11));
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
	secs = a68_list_secs(&list);                 /* the tracks' (a video's length is its own) */
	i = a68_list_videos(&list);
	p = put_num(t, (unsigned long)(list.total - i)); p = put_str(p, list.total - i == 1 ? " track" : " tracks");
	if (secs) { p = put_str(p, ", "); p = put_time(p, secs); }
	if (i) { p = put_str(p, ", "); p = put_num(p, (unsigned long)i); p = put_str(p, i == 1 ? " video" : " videos"); }
	*p = 0;
	MoveTo(86, HEAD_H + 50);
	DrawText(t, 0, (int)strlen(t));
	MoveTo(0, (short)(HEAD_H + ALBUM_H)); LineTo((short)LW, (short)(HEAD_H + ALBUM_H));
}
static unsigned long pos_s(void)
{
	if (!mode || !ps || lstate == L_STOPPED) return 0;
	if (radio.video) {                           /* the card's picture: the server's streams start at 0.5 s (-muxdelay) */
		unsigned long v = vc_pos_ms();
		return offset_s + (v > 500 ? v - 500 : 0) / 1000;
	}
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
		p = t;
		if (named(q->artist)) { p = put_str(p, q->artist); p = put_str(p, " \xD1 "); }   /* 1.8: "_" no more */
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
	short x = depth > 1 ? 62 : 10, lim = (short)(LW - (play_btn && CUR->kind != V_ARTISTS && CUR->kind != V_SEARCH && CUR->kind != V_QUEUE ? 150 : 90));
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
	if (lib_can_play_all() || (CUR->kind == V_VIDEOS || CUR->kind == V_ARTVIDEOS)) ShowControl(play_btn); else HideControl(play_btn);
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
			/* both ends of the title bar on a screen, and no taller than the screens (1.7: a place kept from a 640 x 480
			   screen was taken on the SE's 512 x 342 by its left end alone, the rest of the window off the screen) */
			Point a = { (short)(saved_r.top - 10), (short)(saved_r.left + 24) }, c = { (short)(saved_r.top - 10), (short)(saved_r.right - 24) };
			if (PtInRgn(pt_long(a), LM_GRAYRGN) && PtInRgn(pt_long(c), LM_GRAYRGN) && saved_r.bottom <= RGN_BBOX(LM_GRAYRGN)->bottom) b = saved_r;
			else pi_log("library: the remembered window place is on no screen now: the window goes on the main screen");
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
		if (!srv_host[0] && !finding && !nservers) find_server(1);   /* several found and none chosen: the owner chooses */
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
	if (i == -3) {                               /* the artist's videos (1.7) */
		char t[48], *p = put_str(t, "Videos: ");
		copy(p, CUR->title, (int)(sizeof t - 8));
		push_view(V_ARTVIDEOS, CUR->id, t, CUR->title);
		return;
	}
	if (CUR->kind == V_QUEUE) { if (i < Q.n) play_at(i, 0); return; }   /* Up Next: from that place */
	struct a68_row *r = &rows[i];
	switch (r->kind) {
	case 'A':
		{
			int nv = r->videos;
			push_view(r->count || !r->videos ? V_ALBUMS : V_ARTIST, r->id, r->text, 0);
			CUR->nvid = CUR->kind == V_ALBUMS ? nv : 0;  /* a line "Videos" above the albums */
		}
		break;
	case 'L': push_view(V_TRACKS, r->id, r->text, r->sub[0] ? r->sub : CUR->title); break;
	case 'T':
		if (CUR->kind == V_TRACKS) queue_from_rows(i);
		else ask_queue(r->album, 0, r->id);      /* from a search: its album, from it */
		break;
	case 'V':                                    /* a music video: its sound, as a track */
		if (CUR->kind == V_TRACKS || CUR->kind == V_ARTIST || CUR->kind == V_VIDEOS || CUR->kind == V_ARTVIDEOS) queue_from_rows(i);
		else if (qitems) {                       /* from a search: that video alone */
			fill_item(&qitems[0], r->id, r->album, r->text, r->sub, r->secs);
			qitems[0].video = 1;
			qitems[0].vw = r->vw; qitems[0].vh = r->vh;
			a68q_set(&Q, 1, 0);
			album_title[0] = 0;
			play_at(0, 0);
		}
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
	radio_keep();                                /* 1.8: the stream kept coming while the arrow is held */
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
			else { tracking = 1; TrackControl(ctl, pt_long(p), (void *)pas_ctl_action); tracking = 0; fix_scroll(); }
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
	if (i >= 0) select_line(row_line(i));
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
	vc_window_dispose();
	if (lw) { SetPort(lw); draw_bar(); draw_list(); }
}
void lib_play_pause(void)
{
	if (!mode) { if (lib_can_play_all()) lib_play_all(); else if (list.n) open_line(CUR->sel); return; }
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
	vc_window_dispose();
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
	if (q) {                                     /* the artist, the album; either alone when the other is not there (1.8) */
		p = put_str(p, artist_or(q->artist, ""));
		if (album_title[0] && Q.cur >= 0) { if (p > t) p = put_str(p, " \xD1 "); p = put_str(p, album_title); }
	}
	*p = 0;
	MoveTo(84, 30);
	clip_text(t, W - 94);
	p = t;
	if (q) {
		p = put_str(p, q->video ? "Video " : "Track "); p = put_num(p, (unsigned long)Q.cur + 1); p = put_str(p, " of "); p = put_num(p, (unsigned long)Q.n);
		p = put_str(p, shuffle ? ", shuffled" : "");
		p = put_str(p, Q.repeat != A68_REPEAT_OFF ? ", repeated" : "");
		if (radio.video && lstate == L_PLAYING) {   /* the picture the card shows */
			p = put_str(p, " \xD1 MPEG-1 "); p = put_num(p, vm_w); *p++ = 'x'; p = put_num(p, vm_h);
		} else if (q->video && lstate == L_PLAYING) p = put_str(p, " \xD1 its sound");
		else if (ps && ps->rate && lstate == L_PLAYING) { p = put_str(p, " \xD1 MP3 "); p = put_num(p, ps->bitrate ? ps->bitrate : radio.br); p = put_str(p, " kbit/s"); }
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
		if (!*s) { user_find = srv_known; find_server(0); return; }
	}
	/* host[:port], [ipv6]:port, ipv6 alone, a URL (play/core/address.c); no port: the server's own, 8000 */
	char h[64];
	unsigned port;
	if (!a68_parse_address(a, A68_DEFAULT_PORT, h, sizeof h, &port)) {
		say_error("That is not an address Aurora68 Play understands. Type a name or number and a port, such as 192.168.1.20:8423.");
		return;
	}
	if (finding) { discover_stop(); finding = 0; auto_find = user_find = 0; }
	depth = 1;
	CUR->sel = CUR->top = 0; CUR->from = 0;
	auto_tried = 0;
	want_name[0] = 0;                            /* the name it answers with is kept (got_info) */
	/* 1.8: the typed address is the one known; a name is looked up by the driver (IPv6 and IPv4 addresses, in Connect
	   By's order, each tried in turn); a typed IPv4 address with IPv6 First asks the probe for the server's IPv6 address
	   once it has answered */
	srv_v6[0] = srv_v4[0][0] = srv_v4[1][0] = 0;
	addr_learned = 0;
	if (colon(h)) copy(srv_v6, h, sizeof srv_v6);
	else if (a68_is_ipv4(h)) copy(srv_v4[0], h, sizeof srv_v4[0]);
	{
		char t[120], *p = put_str(put_str(t, "connecting to the typed address "), h);
		p = put_str(put_str(p, colon(h) ? " (IPv6" : a68_is_ipv4(h) ? " (IPv4" : " (a name: its addresses in the order of "), connect_by());
		p = put_str(p, colon(h) || a68_is_ipv4(h) ? ")" : ")");
		*p = 0;
		logs(t, 0);
	}
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
int lib_can_play_all(void)
{
	return lw && srv_known && (CUR->kind == V_ALBUMS || CUR->kind == V_TRACKS || CUR->kind == V_ARTIST || CUR->kind == V_VIDEOS ||
	                           CUR->kind == V_ARTVIDEOS);
}
void lib_play_all(void)
{
	if ((CUR->kind == V_TRACKS || CUR->kind == V_ARTIST || CUR->kind == V_VIDEOS || CUR->kind == V_ARTVIDEOS) && list.n) queue_from_rows(-1);
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
   gaplessly as from one song to the next; Repeat One (1.7): the song again (play/core/queue.c's a68q_follow) */
void lib_toggle_repeat(int which)
{
	Q.repeat = Q.repeat == which ? A68_REPEAT_OFF : which;
	logs(Q.repeat == A68_REPEAT_ONE ? "repeat: one" : Q.repeat ? "repeat: on" : "repeat: off", 0);
	player_refresh();
}
/* ---- 1.7: the videos' lists, Up Next, Play Video, Play Next and Add to Queue ---------------------------------------- */
void lib_videos(void)
{
	lib_show();
	if (!srv_host[0] || CUR->kind == V_VIDEOS) return;
	push_view(V_VIDEOS, "", "Videos", 0);
}
void lib_up_next(void)
{
	lib_show();
	if (CUR->kind == V_QUEUE) return;
	push_view(V_QUEUE, "", "Up Next", 0);
	if (mode && Q.cur >= 0) { CUR->sel = Q.cur; select_line(Q.cur); }
}
static int sel_row(void) { int i = lw && list.n ? line_row(CUR->sel) : -1; return i >= 0 && i < list.n ? i : -1; }
int lib_can_play_video(void) { int i = sel_row(); return i >= 0 && srv_known && (rows[i].kind == 'V' || (rows[i].kind == 'T' && rows[i].video[0])); }
/* the selected song's video (and the album's videos after it, as the macOS Play's film button), or the selected video;
   without the card its sound, as a video's row plays */
void lib_play_video(void)
{
	int i = sel_row(), k;
	if (!lib_can_play_video()) { SysBeep(1); return; }
	if (rows[i].kind == 'V') { open_line(CUR->sel); return; }
	k = CUR->kind == V_QUEUE ? -1 : a68_list_find(&list, rows[i].video);
	if (k >= 0 && rows[k].kind == 'V') { queue_from_rows(k); return; }
	if (!qitems) return;
	row_item(&qitems[0], i);
	if (!qitems[0].video) to_video(&qitems[0], 0);
	a68q_set(&Q, 1, 0);
	album_title[0] = 0;
	play_at(0, 0);
}
int lib_can_add(void)
{
	int i = sel_row();
	if (i < 0 || !srv_known || CUR->kind == V_QUEUE) return 0;
	return rows[i].kind == 'T' || rows[i].kind == 'V' || rows[i].kind == 'L' || (rows[i].kind == 'A' && rows[i].count);
}
/* Play Next (after the song being sent, so that what the card holds is not cut) or Add to Queue (at its end): a song or a
   video at once, an album or an artist once its tracks have come (got_queue). Nothing from the library playing: the queue
   is made of it and plays */
void lib_add(int next)
{
	int i = sel_row(), at;
	struct qitem item;
	if (!lib_can_add()) { SysBeep(1); return; }
	if (rows[i].kind == 'L' || rows[i].kind == 'A') {
		ask_queue(rows[i].kind == 'L' ? rows[i].id : 0, rows[i].kind == 'A' ? rows[i].id : 0, 0);
		q_mode = next ? Q_NEXT : Q_ADD;
		return;
	}
	row_item(&item, i);
	if (!mode) a68q_clear(&Q);
	at = next && mode ? a68q_next_place(&Q) : Q.n;
	if (a68q_insert(&Q, at, &item) < 0) { SysBeep(1); return; }
	logs(next ? "to play next: " : "added to the queue: ", item.title);
	if (!mode || lstate != L_PLAYING) { album_title[0] = 0; play_at(at, 0); }
	else { player_refresh(); queue_changed(); }
}
/* Up Next's rows: the queue's items, as a list made here */
static void queue_rows(void)
{
	int i, n = Q.n < list.max ? Q.n : list.max;
	for (i = 0; i < n; i++) {
		struct qitem *q = qat(i);
		struct a68_row *w = &rows[i];
		memset(w, 0, sizeof *w);
		w->kind = q->video ? 'V' : 'T';
		copy(w->id, q->id, 9); copy(w->album, q->album, 9); copy(w->video, q->link, 9);
		copy(w->text, q->title, sizeof w->text); copy(w->sub, q->artist, sizeof w->sub);
		w->secs = q->secs;
	}
	list.n = n; list.total = n; list.from = 0;
	problem = n ? 0 : "Nothing is queued. Play an album, or choose Play Next or Add to Queue in the Library menu for a song.";
}
static void queue_changed(void)
{
	if (!lw || CUR->kind != V_QUEUE) return;
	queue_rows();
	if (CUR->sel >= nlines()) CUR->sel = nlines() ? nlines() - 1 : 0;
	SetPort(lw);
	fix_scroll();
	InvalRect(PORT_RECT(lw));
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
	if (!srv_host[0]) { find_server(1); return; }
	auto_tried = 0;                              /* a server that does not answer: the probe again after this try */
	if (!srv_known) want_info = 1;
	view_changed();
}
/* at the start (1.7): no server remembered, so the probe at once, the Library window shown or not */
void lib_start(void) { if (!srv_host[0] && !finding) find_server(1); }
void lib_get_ext(struct lib_prefs_ext *e)
{
	memset(e, 0, sizeof *e);
	copy(e->srv_name, want_name, sizeof e->srv_name);
	e->instead = (short)instead;
}
void lib_set_ext(const struct lib_prefs_ext *e)
{
	copy(want_name, e->srv_name, sizeof want_name);
	instead = e->instead != 0;
}
/* Connect By changed (1.8): the server's addresses in the new order; asked again at the first if that is another */
void lib_net_changed(void)
{
	const char *c[3];
	int n;
	if (!srv_host[0]) return;
	n = addr_order(c);
	if (!n || same_text(c[0], srv_host)) { if (n) addr_alts(c, n, 1); return; }
	copy(srv_host, c[0], sizeof srv_host);
	addr_alts(c, n, 1);
	fetch_forget_host();
	want_info = 1;
}
/* the preferences' 1.8 tail: the server's addresses by family (after lib_set_prefs, which gave the one that answered
   last): the order Connect By names again, the one that answered last among them */
void lib_get_addrs(struct lib_prefs_addrs *a)
{
	memset(a, 0, sizeof *a);
	copy(a->v6, srv_v6, sizeof a->v6);
	copy(a->v4[0], srv_v4[0], sizeof a->v4[0]);
	copy(a->v4[1], srv_v4[1], sizeof a->v4[1]);
	a->learned = (short)addr_learned;
}
void lib_set_addrs(const struct lib_prefs_addrs *a)
{
	const char *c[3];
	int n, i;
	copy(srv_v6, a->v6, sizeof srv_v6);
	copy(srv_v4[0], a->v4[0], sizeof srv_v4[0]);
	copy(srv_v4[1], a->v4[1], sizeof srv_v4[1]);
	if (!a68_is_ipv6(srv_v6)) srv_v6[0] = 0;
	for (i = 0; i < 2; i++) if (!a68_is_ipv4(srv_v4[i])) srv_v4[i][0] = 0;
	addr_learned = a->learned != 0;
	if (!srv_host[0] || !net_open()) return;
	n = addr_order(c);
	for (i = 0; i < n && !same_text(c[i], srv_host); i++) { }
	if (!n || i == n) { nalt = 0; return; }       /* the remembered address is none of them: it alone, as before */
	copy(srv_host, c[0], sizeof srv_host);
	addr_alts(c, n, 1);
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
	Q.repeat = p->repeat == A68_REPEAT_ALL || p->repeat == A68_REPEAT_ONE ? p->repeat : A68_REPEAT_OFF;   /* the pad of files before 1.4: 0, off */
	saved_r = p->r;
	saved_shown = p->shown;
	have_saved = 1;
	if (saved_shown) lib_show();
}
