/* SPDX-License-Identifier: MPL-2.0
 *
 * play/core/devices.c - an Aurora68 Play as one of the music server's devices: announce, state, commands.
 *
 * Backstory (22 November 2026, docs/08 item 105). The owner asked that the players know of each other, so
 * that one can send music to another; the music server (tools/music-server) keeps the list, and every player
 * tells it that it is there, what it plays, and asks for commands. No player has to accept a connection, which
 * suits a Macintosh SE behind MacTCP: the classic Play makes the same short GET requests its Library makes, one
 * at a time on the Library's connection (fetch.c), between the lists and the covers. The commands come by long
 * poll: the server holds the request until a command arrives or the wait passes, so a command reaches the SE
 * within a second or two without polling hard.
 *
 * The rhythm: an announce at once and then every "every" seconds (the server's answer, 20; a device not heard
 * for 60 is offline), a state report with each announce and whenever what plays changes (another track, play
 * or pause, a seek of more than three seconds, the volume; not the seconds going by, which the server can
 * count), and a commands request whenever the connection has nothing else to do. A failure waits five seconds
 * more each time, up to a minute; an answer "announce first" (404: the server restarted) announces again.
 *
 * In the core since the same day (item 107): the macOS Play's DeviceLink.swift had the same rules in Swift
 * (its JSON, a report every five seconds while playing, a three-second tolerance where this file had four);
 * now both use this one, the macOS Play with refresh 5 and a 20-second wait, the classic one with none and 15.
 * The commands and the device list are read by proto.c. No Toolbox calls: tools/devices_test.c runs this on
 * the host against the server's own answers.
 */
#include "devices.h"
#include "text.h"

static char *put_n(char *p, unsigned long v, char *end) { return a68_put_num(p, end, v); }

void dv_make_id(char *id, unsigned long a, unsigned long b)
{
	static const char set[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	/* masked to 32 bits: a 64-bit long (a Mac of today) makes the same id from the same seeds as a 68000 */
	unsigned long x = (a ^ 0x9E3779B9ul) & 0xFFFFFFFFul, y = (b * 2654435761ul + 0x7F4A7C15ul) & 0xFFFFFFFFul;
	int i;
	a68_copy(id, "mac-", 5);
	for (i = 0; i < 12; i++) {
		x = (x * 1103515245ul + 12345ul + y) & 0xFFFFFFFFul;
		y = ((y << 7 | y >> 25) ^ x) & 0xFFFFFFFFul;
		id[4 + i] = set[(x >> 11) % 36];
	}
	id[16] = 0;
}
int dv_valid_id(const char *id)
{
	int i;
	if (!id || !id[0] || a68_same(id, "live")) return 0;
	for (i = 0; id[i]; i++) {
		char c = id[i];
		if (i >= 64 || !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_')) return 0;
	}
	return 1;
}

void dv_init(struct dv *d, const char *id, const char *name, const char *version, const char *kind)
{
	unsigned char *z = (unsigned char *)d;
	unsigned long i;
	for (i = 0; i < sizeof *d; i++) z[i] = 0;
	a68_copy(d->id, id, sizeof d->id);
	a68_copy(d->name, name, sizeof d->name);
	a68_copy(d->version, version, sizeof d->version);
	a68_copy(d->kind, kind && kind[0] ? kind : "classic", sizeof d->kind);
	d->every = 20;
	d->wait = DV_WAIT;
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
	if (a68_gen_changed(&d->watch, gen)) d->gen_changed = 1;
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
int dv_state_due(const struct dv *d, unsigned long now)
{
	return d->on && d->announced && d->state_dirty && (long)(now - d->state_at) >= 0;
}
/* the encoding the client asked for, at the end of a path */
static char *tail(const struct dv *d, char *p, char *end)
{
	if (d->client.utf8) p = a68_put(p, end, "&enc=utf8");
	if (d->client.full) p = a68_put(p, end, "&full=1");
	if (d->client.videos) p = a68_put(p, end, "&videos=1");   /* a command's videos as V lines */
	return p;
}
int dv_path(const struct dv *d, int what, char *out, long max)
{
	char *p = out, *end = out + max - 1;
	if (what == DV_ANNOUNCE) {
		p = a68_put(p, end, "/mac/announce?id="); p = a68_put_escaped(p, end, d->id, 0);
		p = a68_put(p, end, "&name="); p = a68_put_escaped(p, end - 24, d->name, 0);
		p = a68_put(p, end, "&kind="); p = a68_put_escaped(p, end, d->kind, 0);
		p = a68_put(p, end, "&v="); p = a68_put_escaped(p, end, d->version, 0);
		if (d->video) p = a68_put(p, end, "&video=1");    /* it shows music videos (the macOS Play) */
	} else if (what == DV_STATE) {
		const struct dv_state *s = &d->st;
		p = a68_put(p, end, "/mac/state?id="); p = a68_put_escaped(p, end, d->id, 0);
		p = a68_put(p, end, s->playing ? "&p=1&pos=" : "&p=0&pos="); p = put_n(p, s->pos, end);
		p = a68_put(p, end, "&len="); p = put_n(p, s->len, end);
		if (s->vol >= 0) { p = a68_put(p, end, "&vol="); p = put_n(p, (unsigned long)s->vol, end); }
		if (s->tid[0]) { p = a68_put(p, end, "&t="); p = a68_put_escaped(p, end, s->tid, 0); }
		else {
			if (s->title[0]) { p = a68_put(p, end, "&title="); p = a68_put_escaped(p, p + (end - p) / 2, s->title, 0); }
			if (s->artist[0]) { p = a68_put(p, end, "&artist="); p = a68_put_escaped(p, end - 20, s->artist, 0); }
		}
	} else if (what == DV_COMMANDS) {
		p = a68_put(p, end, "/mac/commands?id="); p = a68_put_escaped(p, end, d->id, 0);
		p = a68_put(p, end, "&wait="); p = put_n(p, (unsigned long)d->wait, end);
	} else return 0;
	p = tail(d, p, end);
	*p = 0;
	return 1;
}

int dv_answer(struct dv *d, int what, int status, char *body, long len, unsigned long now)
{
	struct a68_rec r;
	char *f[1];
	a68_rec_init(&r, body, body ? len : 0);
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
		if (status != 200 || a68_rec_next(&r, f, 1) < 1 || !a68_same(f[0], "ok")) {
			d->failures++;
			d->announced = 0; d->next_announce = now + (backoff(d) > 30 ? backoff(d) : 30);   /* refused: not soon again */
			return 0;
		}
		d->failures = 0;
		d->announced = 1;
		if (a68_rec_next(&r, f, 1) >= 1 && a68_num(f[0])) {
			d->every = a68_num(f[0]);
			if (d->every < 5) d->every = 5;
			if (d->every > 60) d->every = 60;
		}
		if (a68_rec_next(&r, f, 1) >= 1) dv_set_generation(d, a68_num(f[0]));
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
	d->st = *s;
	if (o->playing && s->playing) expect = o->pos + (now - d->sent_time);   /* the seconds since it was sent */
	if (s->playing != o->playing || !a68_same(s->tid, o->tid) || !a68_same(s->title, o->title) || !a68_same(s->artist, o->artist) ||
	    s->vol != o->vol || s->len != o->len || (s->pos > expect ? s->pos - expect : expect - s->pos) > 3 ||
	    (d->refresh && s->playing && now - d->sent_time >= d->refresh))
		d->state_dirty = 1;
}
