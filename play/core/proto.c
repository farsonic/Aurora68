/* SPDX-License-Identifier: MPL-2.0
 *
 * play/core/proto.c - the music server's protocol: requests and answers (proto.h says what and why).
 *
 * Backstory. The classic Play's Library (22 November 2026, docs/08 item 99) read the server's /mac/ answers with
 * libparse.c, a record at a time copied into a small line; its devices (item 105) had their own reader in
 * devices.c; the macOS Play (item 102) read the server's JSON with Swift's Decodable. On the same day the two
 * became one code base (item 107) and one wire format, the text, because a 68000 reads it with a loop and a
 * modern Mac reads it as easily; this file is libparse.c grown into the whole protocol. The records are now
 * split in place rather than copied, so a field has no length of the core's choosing: the macOS Play's paths
 * and UTF-8 names may be long, and a 68000 copies only the fields it keeps.
 *
 * Hard cases kept from libparse_test.c: a record with fewer fields than asked for, an empty field, the last
 * line without its Return, a line feed where a Return should be, an answer that is not the server's.
 *
 * Music videos (22 November 2026, docs/08 item 109): "V" records after an album's tracks, in a search, in /mac/videos and
 * in a command, asked for with videos=1; a "V" is never an id (those are eight hex digits), so the readers of tracks
 * and videos tell them apart by the first field and pass over the other kind.
 */
#include "proto.h"
#include "text.h"

void a68_rec_init(struct a68_rec *r, char *body, long len)
{
	r->b = body;
	r->n = body ? len : 0;
	r->at = 0;
	if (body) body[r->n] = 0;                        /* the caller's buffer has a byte for it (proto.h) */
}
int a68_rec_next(struct a68_rec *r, char **f, int maxf)
{
	long a, k;
	int nf = 0;
	char *line;
	while (r->at < r->n && (r->b[r->at] == '\r' || r->b[r->at] == '\n' || !r->b[r->at])) r->at++;
	if (r->at >= r->n) return -1;
	a = r->at;
	while (r->at < r->n && r->b[r->at] != '\r' && r->b[r->at] != '\n' && r->b[r->at]) r->at++;
	line = r->b + a;
	if (r->at < r->n) r->b[r->at++] = 0;             /* the line's end (or the NUL already at b[n]) */
	f[nf++] = line;
	for (k = 0; line[k]; k++)
		if (line[k] == '\t') {
			line[k] = 0;
			if (nf < maxf) f[nf++] = line + k + 1;
		}
	{
		int i;
		for (i = nf; i < maxf; i++) f[i] = line + k;     /* the fields that are not there: the line's own NUL */
	}
	return nf;
}

int a68_read_head(struct a68_rec *r, struct a68_head *h)
{
	char *f[6];
	h->kind[0] = 0;
	h->total = h->from = h->count = 0;
	if (r->n < 5 || r->b[0] != 'A' || r->b[1] != '6' || r->b[2] != '8' || r->b[3] != 'M' || r->b[4] != '\t') return 0;
	if (a68_rec_next(r, f, 6) < 6 || !a68_same(f[1], "1")) return 0;
	a68_copy(h->kind, f[2], sizeof h->kind);
	h->total = (long)a68_num(f[3]);
	h->from = (long)a68_num(f[4]);
	h->count = (long)a68_num(f[5]);
	return 1;
}
long a68_next_from(const struct a68_head *h)
{
	long next = h->from + h->count;
	return h->count > 0 && next < h->total ? next : -1;
}

int a68_info_of(struct a68_rec *r, struct a68_info *o)
{
	char *f[9];
	int n = a68_rec_next(r, f, 9);
	if (n < 1) return 0;
	o->name = f[0]; o->version = f[1];
	o->artists = a68_num(f[2]); o->albums = a68_num(f[3]); o->tracks = a68_num(f[4]);
	o->generation = a68_num(f[5]);
	o->every = n >= 7 ? a68_num(f[6]) : 0;
	o->videos = n >= 8 ? a68_num(f[7]) : 0;
	o->caps = f[8];
	return 1;
}
int a68_next_artist(struct a68_rec *r, struct a68_artist *o)
{
	char *f[5];
	while (a68_rec_next(r, f, 5) >= 0) {
		if (!f[0][0]) continue;
		o->id = f[0]; o->name = f[1]; o->albums = a68_num(f[2]); o->tracks = a68_num(f[3]); o->videos = a68_num(f[4]);
		return 1;
	}
	return 0;
}
int a68_next_album(struct a68_rec *r, struct a68_album *o)
{
	char *f[7];
	int n;
	while ((n = a68_rec_next(r, f, 7)) >= 0) {
		if (!f[0][0]) continue;
		o->id = f[0]; o->title = f[1]; o->artist = f[2]; o->tracks = a68_num(f[3]); o->secs = a68_num(f[4]);
		o->artist_id = f[5]; o->folder = f[6]; o->full = n >= 7;
		return 1;
	}
	return 0;
}
static void track_of(char **f, int n, struct a68_track *o)
{
	o->id = f[0]; o->number = a68_num(f[1]); o->title = f[2]; o->artist = f[3]; o->secs = a68_num(f[4]);
	o->album = f[5]; o->path = f[6]; o->album_title = f[7]; o->folder = f[8]; o->full = n >= 9;
}
static void video_of(char **f, int n, struct a68_video *o)
{
	o->id = f[1]; o->title = f[2]; o->artist = f[3]; o->secs = a68_num(f[4]); o->album = f[5];
	o->width = a68_num(f[6]); o->height = a68_num(f[7]);
	o->path = f[8]; o->album_title = f[9]; o->folder = f[10]; o->how = f[11]; o->full = n >= 12;
}
int a68_next_track(struct a68_rec *r, struct a68_track *o)
{
	char *f[12];
	int n;
	while ((n = a68_rec_next(r, f, 12)) >= 0) {
		if (!f[0][0] || a68_same(f[0], "V")) continue;
		track_of(f, n, o);
		return 1;
	}
	return 0;
}
int a68_next_video(struct a68_rec *r, struct a68_video *o)
{
	char *f[12];
	int n;
	while ((n = a68_rec_next(r, f, 12)) >= 0) {
		if (!a68_same(f[0], "V") || !f[1][0]) continue;
		video_of(f, n, o);
		return 1;
	}
	return 0;
}
int a68_next_item(struct a68_rec *r, struct a68_track *t, struct a68_video *v)
{
	char *f[12];
	int n;
	while ((n = a68_rec_next(r, f, 12)) >= 0) {
		if (!f[0][0]) continue;
		if (a68_same(f[0], "V")) {
			if (!f[1][0]) continue;
			video_of(f, n, v);
			return 'V';
		}
		track_of(f, n, t);
		return 'T';
	}
	return 0;
}
int a68_video_how(const char *how)
{
	if (a68_same(how, "direct")) return A68_HOW_DIRECT;
	if (a68_same(how, "remux")) return A68_HOW_REMUX;
	if (a68_same(how, "transcode")) return A68_HOW_TRANSCODE;
	return A68_HOW_UNKNOWN;
}
int a68_next_hit(struct a68_rec *r, struct a68_hit *o)
{
	char *f[6];
	while (a68_rec_next(r, f, 6) >= 0) {
		if (!f[0][0] || !f[1][0]) continue;
		o->kind = f[0][0]; o->id = f[1]; o->text = f[2]; o->sub = f[3];
		o->album = o->kind == 'T' || o->kind == 'V' ? f[4] : f[4] + a68_len(f[4]);
		o->secs = o->kind == 'T' || o->kind == 'V' ? a68_num(f[5]) : 0;
		return 1;
	}
	return 0;
}
int a68_next_radio(struct a68_rec *r, struct a68_radio *o)
{
	char *f[3];
	int n;
	while ((n = a68_rec_next(r, f, 3)) >= 0) {
		if (n < 3 || !f[2][0]) continue;
		o->group = f[0]; o->name = f[1]; o->url = f[2];
		return 1;
	}
	return 0;
}
int a68_next_device(struct a68_rec *r, struct a68_device *o)
{
	char *f[18];
	int n;
	while ((n = a68_rec_next(r, f, 18)) >= 0) {
		if (n < 3 || !f[0][0]) continue;
		o->id = f[0]; o->name = f[1]; o->kind = f[2]; o->online = f[3][0] == '1';
		o->title = f[4]; o->artist = f[5];
		o->full = n >= 16;
		o->playing = f[6][0] == '1';
		o->position_ms = a68_num_ms(f[7]); o->length_ms = a68_num_ms(f[8]);
		o->volume = f[9][0] ? (int)a68_num(f[9]) : 100;
		o->album = f[10]; o->folder = f[11];
		o->at = a68_num(f[12]); o->last_seen = a68_num(f[13]);
		o->is_live = f[14][0] == '1';
		o->version = f[15];
		o->listeners = a68_num(f[16]);
		o->video = f[17][0] == '1';
		return 1;
	}
	return 0;
}
int a68_next_cmd(struct a68_rec *r, struct a68_cmd *o)
{
	char *f[10];
	int n;
	while ((n = a68_rec_next(r, f, 10)) >= 0) {
		char *none = f[0] + a68_len(f[0]);
		o->kind = 0; o->n = 0; o->position = 0; o->volume = -1; o->shuffle = 0;
		o->action = o->tid = o->title = o->artist = o->path = o->album = o->name = o->url = o->how = none;
		o->secs = o->width = o->height = 0;
		if ((a68_same(f[0], "T") || a68_same(f[0], "V")) && n >= 2) {
			o->kind = f[0][0]; o->tid = f[1]; o->title = f[2]; o->artist = f[3]; o->secs = a68_num(f[4]);
			o->path = f[5]; o->album = f[6];
			if (o->kind == 'V') { o->how = f[7]; o->width = a68_num(f[8]); o->height = a68_num(f[9]); }
			return 1;
		}
		if (a68_same(f[0], "R") && n >= 3) {
			o->kind = 'R'; o->name = f[1]; o->url = f[2];
			return 1;
		}
		if (f[0][0] >= '0' && f[0][0] <= '9' && n >= 2) {
			o->kind = 'C'; o->n = (long)a68_num(f[0]); o->action = f[1];
			o->position = (long)a68_num(f[2]);
			o->volume = f[3][0] >= '0' && f[3][0] <= '9' ? (int)a68_num(f[3]) : -1;
			o->shuffle = f[4][0] == '1' || a68_same_ci(f[4], "true");
			return 1;
		}
		/* anything else (an error's words, a line of a later version): passed over */
	}
	return 0;
}
int a68_next_now(struct a68_rec *r, struct a68_now *o)
{
	char *f[8];
	int n;
	while ((n = a68_rec_next(r, f, 8)) >= 0) {
		char *none = f[0] + a68_len(f[0]);
		o->name = o->path = o->folder = o->title = o->who = none;
		o->playing = o->paused = o->repeat = 0; o->pos = 0; o->changes = o->listeners = 0;
		o->elapsed_ms = o->length_ms = 0; o->since = 0;
		o->kind = f[0][0] && !f[0][1] ? f[0][0] : 0;
		switch (o->kind) {
		case 'N':
			o->name = f[1]; o->playing = f[2][0] == '1'; o->paused = f[3][0] == '1'; o->repeat = f[4][0] == '1';
			o->pos = (long)a68_num(f[5]); o->changes = a68_num(f[6]); o->listeners = a68_num(f[7]);
			return 1;
		case 'C':
			o->path = f[1]; o->folder = f[2]; o->title = f[3]; o->elapsed_ms = a68_num_ms(f[4]); o->length_ms = a68_num_ms(f[5]);
			return 1;
		case 'Q':
			o->path = f[1]; o->title = f[2];
			return 1;
		case 'L':
			o->who = f[1]; o->since = a68_num(f[2]);
			return 1;
		}
	}
	return 0;
}
const char *a68_error_text(char *body, long len)
{
	struct a68_rec r;
	struct a68_head h;
	char *f[1];
	a68_rec_init(&r, body, len);
	if (len >= 5 && body[0] == 'A' && body[1] == '6' && body[2] == '8' && body[3] == 'M') {
		if (!a68_read_head(&r, &h)) return "";
	}
	return a68_rec_next(&r, f, 1) >= 1 ? f[0] : "";
}

static const char *const act_names[] = { "?", "play", "add", "next", "pause", "resume", "skip", "previous", "seek", "volume", "stop",
                                         "jump", "remove", "clear", "shuffle", "repeat" };
int a68_action(const char *a)
{
	int i;
	for (i = 1; i <= A68_ACT_REPEAT; i++)
		if (a68_same(a, act_names[i])) return i;
	return A68_ACT_UNKNOWN;
}
const char *a68_action_name(int act) { return act > 0 && act <= A68_ACT_REPEAT ? act_names[act] : act_names[0]; }

int a68_art(const char *b, long n, const char *magic, long *v, int nv, long *data_at)
{
	long i = 0;
	int k;
	while (magic[i]) { if (i >= n || b[i] != magic[i]) return 0; i++; }
	if (i >= n || b[i] != '\t') return 0;
	i++;
	if (i + 1 >= n || b[i] != '1' || b[i + 1] != '\t') return 0;   /* version 1 */
	i += 2;
	for (k = 0; k < nv; k++) {
		long x = 0;
		if (i >= n || b[i] < '0' || b[i] > '9') return 0;
		while (i < n && b[i] >= '0' && b[i] <= '9') x = x * 10 + (b[i++] - '0');
		v[k] = x;
		if (i < n && b[i] == '\t') i++;
	}
	while (i < n && b[i] != '\r') i++;
	if (i >= n) return 0;
	*data_at = i + 1;
	return 1;
}

/* ---- the requests ----------------------------------------------------------------------------------------- */
struct pb { char *out, *p, *end; int q; };
static void pb_begin(struct pb *b, char *out, long max, const char *path)
{
	b->out = out; b->p = out; b->end = out + (max > 0 ? max - 1 : 0); b->q = 0;
	b->p = a68_put(b->p, b->end, path);
}
static void arg(struct pb *b, const char *name)
{
	if (b->p < b->end) *b->p++ = b->q ? '&' : '?';
	b->q = 1;
	b->p = a68_put(b->p, b->end, name);
	if (b->p < b->end) *b->p++ = '=';
}
static void arg_s(struct pb *b, const char *name, const char *v) { arg(b, name); b->p = a68_put_escaped(b->p, b->end, v, 0); }
static void arg_n(struct pb *b, const char *name, unsigned long v) { arg(b, name); b->p = a68_put_num(b->p, b->end, v); }
static int pb_end(struct pb *b, const struct a68_client *c)
{
	if (c && c->utf8) arg_s(b, "enc", "utf8");
	if (c && c->full) arg_n(b, "full", 1);
	if (c && c->videos) arg_n(b, "videos", 1);
	if (b->p >= b->end) { if (b->end >= b->out) b->out[0] = 0; return 0; }
	*b->p = 0;
	return 1;
}
static void page(struct pb *b, long from, long count)
{
	if (count > 0) arg_n(b, "count", (unsigned long)count);
	if (from > 0) arg_n(b, "from", (unsigned long)from);
}

int a68_path_info(const struct a68_client *c, char *out, long max)
{
	struct pb b;
	pb_begin(&b, out, max, "/mac/info");
	return pb_end(&b, c);
}
int a68_path_artists(const struct a68_client *c, long from, long count, char *out, long max)
{
	struct pb b;
	pb_begin(&b, out, max, "/mac/artists");
	page(&b, from, count);
	return pb_end(&b, c);
}
int a68_path_albums(const struct a68_client *c, const char *artist, long from, long count, char *out, long max)
{
	struct pb b;
	pb_begin(&b, out, max, "/mac/albums");
	if (artist && artist[0]) arg_s(&b, "artist", artist);
	page(&b, from, count);
	return pb_end(&b, c);
}
int a68_path_tracks(const struct a68_client *c, const char *album, const char *artist, long from, long count, char *out, long max)
{
	struct pb b;
	pb_begin(&b, out, max, "/mac/tracks");
	if (album && album[0]) arg_s(&b, "album", album);
	else arg_s(&b, "artist", artist ? artist : "");
	page(&b, from, count);
	return pb_end(&b, c);
}
int a68_path_search(const struct a68_client *c, const char *words, long from, long count, char *out, long max)
{
	struct pb b;
	pb_begin(&b, out, max, "/mac/search");
	arg_s(&b, "q", words);
	page(&b, from, count);
	return pb_end(&b, c);
}
int a68_path_art(const struct a68_client *c, const char *album, int size, int depth, char *out, long max)
{
	struct pb b;
	pb_begin(&b, out, max, "/mac/art");
	arg_n(&b, "w", (unsigned long)size);
	arg_n(&b, "h", (unsigned long)size);
	arg_s(&b, "album", album);
	arg_n(&b, "depth", depth == 8 ? 8 : 1);
	return pb_end(&b, c);
}
int a68_path_arts(const struct a68_client *c, const char *const *ids, int n, int size, char *out, long max)
{
	struct pb b;
	int i;
	pb_begin(&b, out, max, "/mac/arts");
	arg_n(&b, "w", (unsigned long)size);
	arg_n(&b, "h", (unsigned long)size);
	arg(&b, "albums");
	for (i = 0; i < n; i++) {                        /* the ids as they are, a comma between (the server splits at it) */
		if (i && b.p < b.end) *b.p++ = ',';
		b.p = a68_put_escaped(b.p, b.end, ids[i], 0);
	}
	return pb_end(&b, c);
}
int a68_path_radio(const struct a68_client *c, char *out, long max)
{
	struct pb b;
	pb_begin(&b, out, max, "/mac/radio");
	return pb_end(&b, c);
}
int a68_path_devices(const struct a68_client *c, char *out, long max)
{
	struct pb b;
	pb_begin(&b, out, max, "/mac/devices");
	return pb_end(&b, c);
}
int a68_path_now(const struct a68_client *c, char *out, long max)
{
	struct pb b;
	pb_begin(&b, out, max, "/mac/now");
	return pb_end(&b, c);
}
int a68_path_command(const struct a68_client *c, const char *to, int action, const char *what, int shuffle,
                     long position_ms, int volume, long index, int on, char *out, long max)
{
	struct pb b;
	pb_begin(&b, out, max, "/mac/command");
	arg_s(&b, "to", to);
	arg_s(&b, "action", a68_action_name(action));
	if (what) arg_s(&b, "what", what);
	if (shuffle) arg_n(&b, "shuffle", 1);
	if (position_ms >= 0) {                          /* seconds, to the thousandth */
		unsigned long ms = (unsigned long)position_ms;
		arg_n(&b, "position", ms / 1000);
		if (ms % 1000) {
			if (b.p < b.end) *b.p++ = '.';
			if (b.p < b.end) *b.p++ = (char)('0' + ms / 100 % 10);
			if (b.p < b.end) *b.p++ = (char)('0' + ms / 10 % 10);
			if (b.p < b.end) *b.p++ = (char)('0' + ms % 10);
		}
	}
	if (volume >= 0) arg_n(&b, "volume", (unsigned long)(volume > 100 ? 100 : volume));
	if (index >= 0) arg_n(&b, "index", (unsigned long)index);
	if (on >= 0) arg_n(&b, "on", on ? 1 : 0);
	return pb_end(&b, c);
}
int a68_path_stream(const char *tid, unsigned long start, char *out, long max)
{
	struct pb b;
	pb_begin(&b, out, max, "/t/");
	b.p = a68_put_escaped(b.p, b.end, tid, 0);
	if (start) arg_n(&b, "start", start);
	return pb_end(&b, 0);
}
int a68_path_videos(const struct a68_client *c, const char *artist, const char *album, long from, long count, char *out, long max)
{
	struct pb b;
	pb_begin(&b, out, max, "/mac/videos");
	if (album && album[0]) arg_s(&b, "album", album);
	else if (artist && artist[0]) arg_s(&b, "artist", artist);
	page(&b, from, count);
	return pb_end(&b, c);
}
static int video_path(const char *head, const char *vid, const char *tail, char *out, long max)
{
	struct pb b;
	pb_begin(&b, out, max, head);
	b.p = a68_put_escaped(b.p, b.end, vid, 0);
	if (tail) b.p = a68_put(b.p, b.end, tail);
	return pb_end(&b, 0);
}
int a68_path_vaudio(const char *vid, unsigned long start, char *out, long max)
{
	struct pb b;
	pb_begin(&b, out, max, "/vaudio/");
	b.p = a68_put_escaped(b.p, b.end, vid, 0);
	if (start) arg_n(&b, "start", start);
	return pb_end(&b, 0);
}
int a68_path_vmpeg(const char *vid, unsigned w, unsigned h, unsigned long start, char *out, long max)
{
	struct pb b;
	pb_begin(&b, out, max, "/vmpeg/");
	b.p = a68_put_escaped(b.p, b.end, vid, 0);
	arg_n(&b, "w", w);
	arg_n(&b, "h", h);
	if (start) arg_n(&b, "start", start);
	return pb_end(&b, 0);
}
/* the server's rule for a size (mpeg_size in aurora68_music_server.py): 64 to top, even */
static unsigned vm_even(unsigned long v, unsigned top)
{
	if (v > top) v = top;
	if (v < 64) v = 64;
	return (unsigned)(v - v % 2);
}
void a68_vmpeg_size(unsigned long vw, unsigned long vh, unsigned sw, unsigned sh, unsigned room_w, unsigned room_h,
                    unsigned *w, unsigned *h)
{
	unsigned long bw = sw <= 640 ? 320 : sw < 1280 ? 512 : 640, bh = sw <= 640 ? 240 : sw < 1280 ? 384 : 480;
	unsigned long fw, fh;
	(void)sh;
	if (room_w && bw > room_w) bw = room_w;
	if (room_h && bh > room_h) bh = room_h;
	if (bw > 1280) bw = 1280;
	if (bh > 720) bh = 720;
	if (!vw || !vh) { vw = 4; vh = 3; }
	vw &= 0xFFFFul; vh &= 0xFFFFul;                  /* a 32-bit product below: sizes of a video, not more */
	if (vw * bh <= vh * bw) { fh = bh; fw = vw * bh / vh; }   /* taller than the box: its height fills it */
	else { fw = bw; fh = vh * bw / vw; }
	*w = vm_even(fw, 1280);
	*h = vm_even(fh, 720);
}
int a68_path_vthumb(const char *vid, char *out, long max) { return video_path("/vthumb/", vid, 0, out, max); }
int a68_path_video(const char *vid, int how, char *out, long max)
{
	return video_path("/video/", vid, how == A68_HOW_DIRECT ? 0 : "/index.m3u8", out, max);
}
static int what(const char *kind, const char *s, char *out, long max)
{
	char *p = out, *end = out + max - 1;
	if (max < 3) return 0;
	p = a68_put(p, end, kind);
	p = a68_put(p, end, s);
	if (a68_len(s) > (long)(p - out) - 2) { out[0] = 0; return 0; }
	*p = 0;
	return 1;
}
int a68_what_folder(const char *folder, char *out, long max) { return what("f:", folder, out, max); }
int a68_what_track(const char *path, char *out, long max) { return what("t:", path, out, max); }
int a68_what_video(const char *vid, char *out, long max) { return what("v:", vid, out, max); }
int a68_what_radio(unsigned long n, char *out, long max)
{
	char *p = out, *end = out + max - 1;
	if (max < 4) return 0;
	p = a68_put(p, end, "r:");
	p = a68_put_num(p, end, n);
	*p = 0;
	return 1;
}

int a68_gen_changed(struct a68_gen *w, unsigned long g)
{
	int changed;
	if (!g) return 0;
	changed = w->seen && g != w->seen;
	w->seen = g;
	return changed;
}
unsigned long a68_bar_time(long x, long w, unsigned long len)
{
	if (w <= 0 || x <= 0) return 0;
	if (x >= w) return len;
	return a68_muldiv(len, (unsigned long)x, (unsigned long)w);
}
unsigned long a68_estimate_ms(unsigned long pos, unsigned long len, int playing, unsigned long at, unsigned long now)
{
	if (playing && at && now > at) pos += (now - at) * 1000ul;
	if (len && pos > len) pos = len;
	return pos;
}
