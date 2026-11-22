/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/devices.c - Aurora68 Play as one of the music server's devices: announce, state, commands.
 *
 * Backstory (22 November 2026, docs/08 item 103). The owner asked that the players know of each other, so
 * that one can send music to another; the music server (tools/music-server, branch play-devices) keeps the
 * list, and every player tells it that it is there, what it plays, and asks for commands. No player has to
 * accept a connection, which suits a Macintosh SE behind MacTCP: the classic Play makes the same short GET
 * requests its Library makes, one at a time on the Library's connection (fetch.c), between the lists and
 * the covers. The commands come by long poll: the server holds the request until a command arrives or
 * DV_WAIT seconds pass, so a command reaches the SE within a second or two without polling hard.
 *
 * The rhythm: an announce at once and then every "every" seconds (the server's answer, 20; a device not heard
 * for 60 is offline), a state report with each announce and whenever what plays changes (another track, play
 * or pause, a seek, the volume; not the seconds going by, which the server can count), and a commands request
 * whenever the connection has nothing else to do. A failure waits five seconds more each time, up to a
 * minute; an answer "announce first" (404: the server restarted) announces again.
 *
 * The answers are the classic interface's plain lines (Mac Roman, ended by a Return, fields by tabs). No
 * Toolbox calls: tools/devices_test.c runs this on the host against the server's own answers.
 */
#include "devices.h"

static void cpy(char *d, const char *s, int max) { int i = 0; while (s && s[i] && i < max - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static int same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static unsigned long num(const char *s) { unsigned long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned long)(*s++ - '0'); return v; }
static char *put(char *p, const char *s, char *end) { while (*s && p < end) *p++ = *s++; return p; }
static char *put_n(char *p, unsigned long v, char *end)
{
	char t[12];
	int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
	while (n && p < end) *p++ = t[--n];
	return p;
}

char *dv_escape(char *p, const char *s, int max)
{
	static const char hx[] = "0123456789ABCDEF";
	int i;
	for (i = 0; s[i] && max >= 3; i++) {
		unsigned char c = (unsigned char)s[i];
		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') { *p++ = (char)c; max--; }
		else { *p++ = '%'; *p++ = hx[c >> 4]; *p++ = hx[c & 15]; max -= 3; }
	}
	return p;
}
void dv_make_id(char *id, unsigned long a, unsigned long b)
{
	static const char set[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	unsigned long x = a ^ 0x9E3779B9ul, y = b * 2654435761ul + 0x7F4A7C15ul;
	int i;
	cpy(id, "mac-", 5);
	for (i = 0; i < 12; i++) {
		x = x * 1103515245ul + 12345ul + y;
		y = (y << 7 | y >> 25) ^ x;
		id[4 + i] = set[(x >> 11) % 36];
	}
	id[16] = 0;
}

void dv_init(struct dv *d, const char *id, const char *name, const char *version)
{
	unsigned char *z = (unsigned char *)d;
	unsigned long i;
	for (i = 0; i < sizeof *d; i++) z[i] = 0;
	cpy(d->id, id, sizeof d->id);
	cpy(d->name, name, sizeof d->name);
	cpy(d->version, version, sizeof d->version);
	d->every = 20;
	d->st.vol = d->sent.vol = -1;
}
void dv_start(struct dv *d, unsigned long now)
{
	d->on = 1; d->announced = 0; d->failures = 0;
	d->next_announce = d->next_poll = d->state_at = now;
	d->state_dirty = 1;
}
void dv_stop(struct dv *d) { d->on = 0; d->announced = 0; }
void dv_set_generation(struct dv *d, unsigned long gen)
{
	if (!gen) return;
	if (d->gen && gen != d->gen) d->gen_changed = 1;
	d->gen = gen;
}

static unsigned long backoff(struct dv *d) { unsigned long s = 5ul * (unsigned long)d->failures; return s > 60 ? 60 : s; }
int dv_want(struct dv *d, unsigned long now)
{
	if (!d->on || !d->id[0]) return DV_NONE;
	if (!d->announced || (long)(now - d->next_announce) >= 0) return (long)(now - d->next_announce) >= 0 ? DV_ANNOUNCE : DV_NONE;
	if (d->state_dirty && (long)(now - d->state_at) >= 0) return DV_STATE;
	if ((long)(now - d->next_poll) >= 0) return DV_COMMANDS;
	return DV_NONE;
}
int dv_path(const struct dv *d, int what, char *out, int max)
{
	char *p = out, *end = out + max - 1;
	if (what == DV_ANNOUNCE) {
		p = put(p, "/mac/announce?id=", end); p = dv_escape(p, d->id, (int)(end - p));
		p = put(p, "&name=", end); p = dv_escape(p, d->name, (int)(end - p) - 24);
		p = put(p, "&kind=classic&v=", end); p = dv_escape(p, d->version, (int)(end - p));
	} else if (what == DV_STATE) {
		const struct dv_state *s = &d->st;
		p = put(p, "/mac/state?id=", end); p = dv_escape(p, d->id, (int)(end - p));
		p = put(p, s->playing ? "&p=1&pos=" : "&p=0&pos=", end); p = put_n(p, s->pos, end);
		p = put(p, "&len=", end); p = put_n(p, s->len, end);
		if (s->vol >= 0) { p = put(p, "&vol=", end); p = put_n(p, (unsigned long)s->vol, end); }
		if (s->tid[0]) { p = put(p, "&t=", end); p = dv_escape(p, s->tid, (int)(end - p)); }
		else {
			if (s->title[0]) { p = put(p, "&title=", end); p = dv_escape(p, s->title, (int)(end - p) / 2); }
			if (s->artist[0]) { p = put(p, "&artist=", end); p = dv_escape(p, s->artist, (int)(end - p)); }
		}
	} else if (what == DV_COMMANDS) {
		p = put(p, "/mac/commands?id=", end); p = dv_escape(p, d->id, (int)(end - p));
		p = put(p, "&wait=", end); p = put_n(p, DV_WAIT, end);
	} else return 0;
	*p = 0;
	return 1;
}

/* ---- the answers: lines and their tabs ---------------------------------------------------------------- */
void dv_iter_init(struct dv_iter *it, const char *body, long len) { it->b = body; it->n = body ? len : 0; it->at = 0; }
/* the next line's fields, copied into line and split: f[0] .. f[n-1]; the number, -1 at the end */
static int fields(struct dv_iter *it, char *line, int max, char **f, int maxf)
{
	int o = 0, nf = 0, k;
	while (it->at < it->n && (it->b[it->at] == '\r' || it->b[it->at] == '\n')) it->at++;
	if (it->at >= it->n) return -1;
	while (it->at < it->n && it->b[it->at] != '\r' && it->b[it->at] != '\n') {
		if (o < max - 1) line[o++] = it->b[it->at];
		it->at++;
	}
	line[o] = 0;
	f[nf++] = line;
	for (k = 0; k < o; k++)
		if (line[k] == '\t') { line[k] = 0; if (nf < maxf) f[nf++] = line + k + 1; }
	for (k = nf; k < maxf; k++) f[k] = line + o;             /* fields that are not there: empty */
	return nf;
}
int dv_answer(struct dv *d, int what, int status, const char *body, long len, unsigned long now)
{
	struct dv_iter it;
	char line[160], *f[2];
	dv_iter_init(&it, body, len);
	if (!status) {                                         /* no answer at all */
		d->failures++;
		if (what == DV_ANNOUNCE) { d->announced = 0; d->next_announce = now + backoff(d); }
		else if (what == DV_STATE) d->state_at = now + backoff(d);
		else d->next_poll = now + backoff(d);
		return 0;
	}
	if (status == 404 && what != DV_ANNOUNCE) {           /* "announce first": the server forgot this device */
		d->announced = 0; d->next_announce = now;
		return 0;
	}
	if (what == DV_ANNOUNCE) {
		if (status != 200 || fields(&it, line, sizeof line, f, 1) < 1 || !same(f[0], "ok")) {
			d->failures++;
			d->announced = 0; d->next_announce = now + (backoff(d) > 30 ? backoff(d) : 30);   /* refused: not soon again */
			return 0;
		}
		d->failures = 0;
		d->announced = 1;
		if (fields(&it, line, sizeof line, f, 1) >= 1 && num(f[0])) {
			d->every = num(f[0]);
			if (d->every < 5) d->every = 5;
			if (d->every > 60) d->every = 60;
		}
		if (fields(&it, line, sizeof line, f, 1) >= 1) dv_set_generation(d, num(f[0]));
		d->next_announce = now + d->every;
		d->state_dirty = 1; d->state_at = now;            /* the state goes with each announce */
		return 1;
	}
	if (status != 200) {                                   /* refused: say it again later */
		d->failures++;
		if (what == DV_STATE) d->state_at = now + backoff(d);
		else d->next_poll = now + backoff(d);
		return 0;
	}
	d->failures = 0;
	if (what == DV_STATE) { d->sent = d->st; d->sent_time = now; d->state_dirty = 0; d->state_at = now + 2; }   /* at most one report in two seconds */
	if (what == DV_COMMANDS) d->next_poll = now;           /* and the next one at once */
	return 1;
}
void dv_set_state(struct dv *d, const struct dv_state *s, unsigned long now)
{
	const struct dv_state *o = &d->sent;
	unsigned long expect = o->pos;
	(void)now;
	d->st = *s;
	if (o->playing && s->playing) expect = o->pos + (now - d->sent_time);   /* the seconds since it was sent */
	if (s->playing != o->playing || !same(s->tid, o->tid) || !same(s->title, o->title) || !same(s->artist, o->artist) ||
	    s->vol != o->vol || s->len != o->len || (s->pos > expect ? s->pos - expect : expect - s->pos) > 4)
		d->state_dirty = 1;
}

int dv_action(const char *a)
{
	static const char *const names[] = { "play", "add", "next", "pause", "resume", "skip", "previous", "seek", "volume", "stop" };
	int i;
	for (i = 0; i < 10; i++)
		if (same(a, names[i])) return DA_PLAY + i;
	return DA_UNKNOWN;
}
int dv_next_item(struct dv_iter *it, struct dv_item *o)
{
	char line[400], *f[5];
	int n;
	while ((n = fields(it, line, sizeof line, f, 5)) >= 0) {
		unsigned char *z = (unsigned char *)o;
		unsigned long i;
		for (i = 0; i < sizeof *o; i++) z[i] = 0;
		if (same(f[0], "T") && n >= 2) {
			o->kind = 'T'; cpy(o->tid, f[1], sizeof o->tid); cpy(o->title, f[2], sizeof o->title);
			cpy(o->artist, f[3], sizeof o->artist); o->secs = num(f[4]);
			return 1;
		}
		if (same(f[0], "R") && n >= 3) {
			o->kind = 'R'; cpy(o->name, f[1], sizeof o->name); cpy(o->url, f[2], sizeof o->url);
			return 1;
		}
		if (f[0][0] >= '0' && f[0][0] <= '9' && n >= 2) {
			o->kind = 'C'; o->n = (long)num(f[0]); cpy(o->action, f[1], sizeof o->action);
			o->position = (long)num(f[2]);
			o->volume = f[3][0] >= '0' && f[3][0] <= '9' ? (int)num(f[3]) : -1;
			return 1;
		}
		/* anything else (an error's words, a line of a later version): passed over */
	}
	return 0;
}
int dv_next_device(struct dv_iter *it, struct dv_device *o)
{
	char line[400], *f[6];
	int n;
	while ((n = fields(it, line, sizeof line, f, 6)) >= 0) {
		if (n < 3 || !f[0][0]) continue;
		cpy(o->id, f[0], sizeof o->id); cpy(o->name, f[1], sizeof o->name); cpy(o->kind, f[2], sizeof o->kind);
		o->online = f[3][0] == '1';
		cpy(o->title, f[4], sizeof o->title); cpy(o->artist, f[5], sizeof o->artist);
		return 1;
	}
	return 0;
}
