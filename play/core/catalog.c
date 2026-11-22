/* SPDX-License-Identifier: MPL-2.0
 *
 * play/core/catalog.c - a page of the library as rows (catalog.h says what and why).
 *
 * Backstory (22 November 2026, docs/08 items 99 and 107). The classic Play's Library window had this inside
 * library.c beside its drawing: which request a view makes, how each kind of answer becomes rows, the lines
 * of the previous and next pages, the row selected again after a refresh (item 105), and typing to select
 * ("c" finds The Cure, as record shops file them). It is the model of the window, not the window, so it is
 * here, and the host tests check it against the server's own answers.
 */
#include "catalog.h"
#include "text.h"

void a68_list_init(struct a68_list *l, struct a68_row *rows, int max, int kind)
{
	l->rows = rows; l->max = max; l->n = 0;
	l->total = l->from = 0;
	l->kind = kind;
}
int a68_list_path(const struct a68_client *c, int kind, const char *id, const char *words, long from, char *out, long max)
{
	switch (kind) {
	case A68_V_ARTISTS: return a68_path_artists(c, from, A68_PAGE, out, max);
	case A68_V_ALBUMS: return a68_path_albums(c, id, from, A68_PAGE, out, max);
	case A68_V_TRACKS: return a68_path_tracks(c, id, 0, from, A68_PAGE, out, max);
	default: return a68_path_search(c, words, from, A68_PAGE, out, max);
	}
}
static void clear_row(struct a68_row *r)
{
	unsigned char *z = (unsigned char *)r;
	unsigned long i;
	for (i = 0; i < sizeof *r; i++) z[i] = 0;
}
int a68_list_load(struct a68_list *l, char *body, long len)
{
	struct a68_rec r;
	struct a68_head h;
	a68_rec_init(&r, body, len);
	l->n = 0;
	if (!a68_read_head(&r, &h)) return 0;
	l->total = h.total;
	l->from = h.from;
	while (l->n < l->max) {
		struct a68_row *w = &l->rows[l->n];
		clear_row(w);
		if (l->kind == A68_V_ARTISTS) {
			struct a68_artist a;
			if (!a68_next_artist(&r, &a)) break;
			w->kind = 'A'; a68_copy(w->id, a.id, sizeof w->id); a68_copy(w->text, a.name, sizeof w->text);
			w->count = (unsigned short)a.albums; w->videos = (unsigned short)a.videos;
		} else if (l->kind == A68_V_ALBUMS) {
			struct a68_album a;
			if (!a68_next_album(&r, &a)) break;
			w->kind = 'L'; a68_copy(w->id, a.id, sizeof w->id); a68_copy(w->text, a.title, sizeof w->text);
			a68_copy(w->sub, a.artist, sizeof w->sub);
			w->count = (unsigned short)a.tracks; w->secs = a.secs;
		} else if (l->kind == A68_V_TRACKS) {
			struct a68_track t;
			struct a68_video v;
			int k = a68_next_item(&r, &t, &v);
			if (!k) break;
			if (k == 'V') {                  /* a music video after the tracks: its title, artist, length */
				w->kind = 'V'; a68_copy(w->id, v.id, sizeof w->id);
				a68_copy(w->text, v.title, sizeof w->text); a68_copy(w->sub, v.artist, sizeof w->sub);
				w->secs = v.secs; a68_copy(w->album, v.album, sizeof w->album);
			} else {
				w->kind = 'T'; a68_copy(w->id, t.id, sizeof w->id); w->num = (unsigned short)t.number;
				a68_copy(w->text, t.title, sizeof w->text); a68_copy(w->sub, t.artist, sizeof w->sub);
				w->secs = t.secs; a68_copy(w->album, t.album, sizeof w->album);
			}
		} else {
			struct a68_hit t;
			if (!a68_next_hit(&r, &t)) break;
			w->kind = t.kind; a68_copy(w->id, t.id, sizeof w->id); a68_copy(w->text, t.text, sizeof w->text);
			a68_copy(w->sub, t.sub, sizeof w->sub);
			a68_copy(w->album, t.album, sizeof w->album); w->secs = t.secs;
		}
		l->n++;
	}
	return 1;
}
int a68_list_find(const struct a68_list *l, const char *id)
{
	int i;
	for (i = 0; i < l->n; i++)
		if (id[0] && a68_same(l->rows[i].id, id)) return i;
	return -1;
}
int a68_list_typed(const struct a68_list *l, const char *typed, int n)
{
	int i, k;
	for (i = 0; i < l->n; i++) {
		const char *s = l->rows[i].text;
		if (!(n > 1 && typed[0] == 't' && typed[1] == 'h')) s = a68_sort_name(s);
		for (k = 0; k < n && a68_lower((unsigned char)s[k]) == typed[k]; k++) { }
		if (k == n) return i;
	}
	return -1;
}
unsigned long a68_list_secs(const struct a68_list *l)
{
	unsigned long s = 0;
	int i;
	for (i = 0; i < l->n; i++)
		if (l->rows[i].kind != 'V') s += l->rows[i].secs;
	return s;
}
int a68_list_videos(const struct a68_list *l)
{
	int i, n = 0;
	for (i = 0; i < l->n; i++)
		if (l->rows[i].kind == 'V') n++;
	return n;
}
int a68_list_has_prev(const struct a68_list *l) { return l->from > 0; }
int a68_list_has_next(const struct a68_list *l) { return l->from + l->n < l->total; }
int a68_list_lines(const struct a68_list *l) { return l->n + a68_list_has_prev(l) + a68_list_has_next(l); }
int a68_list_line_row(const struct a68_list *l, int i)
{
	if (a68_list_has_prev(l)) { if (i == 0) return -1; i--; }
	return i < l->n ? i : -2;
}
int a68_list_row_line(const struct a68_list *l, int row) { return row + a68_list_has_prev(l); }
