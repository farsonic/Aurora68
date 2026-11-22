/* SPDX-License-Identifier: MPL-2.0
 *
 * play/core/queue.c - the play queue (queue.h says what and why).
 *
 * Backstory (22 November 2026, docs/08 item 107). The macOS Play's PlayQueue.swift had the rules (Play Next,
 * Add, Remove, Jump, Repeat all and one, Previous restarting a song more than three seconds in, Shuffle of
 * what is still to come); the classic Play's library.c had its own queue with the stream sent ahead and no
 * repeat. One queue now serves both, with what each lacked:
 *   - the classic Play gains Repeat (Library > Repeat) and Previous on the first song restarting it;
 *   - songs added to a queue that has played to its end are the ones that play (the classic rule), where the
 *     macOS Play started the old queue again from its first song.
 * The shuffle draws from a68_random, so a test with a seed gets the same order on a 68000 and on a Mac.
 * Items are moved byte by byte: no memmove in the classic Play's kit beyond its own, and an item is small.
 */
#include "queue.h"
#include "text.h"

void a68q_init(struct a68_queue *q, void *items, unsigned long size, int max)
{
	q->items = (unsigned char *)items;
	q->size = size;
	q->max = max;
	q->n = 0;
	q->cur = q->feed = -1;
	q->repeat = A68_REPEAT_OFF;
}
void *a68q_at(const struct a68_queue *q, int i)
{
	return i >= 0 && i < q->n ? q->items + (unsigned long)i * q->size : 0;
}
static void move_items(struct a68_queue *q, int to, int from, int count)
{
	unsigned char *d = q->items + (unsigned long)to * q->size, *s = q->items + (unsigned long)from * q->size;
	unsigned long k = (unsigned long)count * q->size;
	if (d < s) while (k--) *d++ = *s++;
	else { d += k; s += k; while (k--) *--d = *--s; }
}
static void swap_items(struct a68_queue *q, int a, int b)
{
	unsigned char *x = q->items + (unsigned long)a * q->size, *y = q->items + (unsigned long)b * q->size, t;
	unsigned long k;
	for (k = 0; k < q->size; k++) { t = x[k]; x[k] = y[k]; y[k] = t; }
}

void a68q_set(struct a68_queue *q, int n, int start)
{
	q->n = n < 0 ? 0 : n > q->max ? q->max : n;
	if (!q->n) { q->cur = q->feed = -1; return; }
	q->cur = q->feed = start < 0 ? 0 : start >= q->n ? q->n - 1 : start;
}
void a68q_clear(struct a68_queue *q) { q->n = 0; q->cur = q->feed = -1; }

int a68q_insert(struct a68_queue *q, int at, const void *item)
{
	const unsigned char *s = (const unsigned char *)item;
	unsigned char *d;
	unsigned long k;
	if (q->n >= q->max) return -1;
	if (at < 0 || at > q->n) at = q->n;
	move_items(q, at + 1, at, q->n - at);
	d = q->items + (unsigned long)at * q->size;
	for (k = 0; k < q->size; k++) d[k] = s[k];
	q->n++;
	if (q->cur < 0) q->cur = q->feed = at;           /* nothing played: the new songs do */
	else {
		if (at <= q->cur) q->cur++;
		if (q->feed >= 0 && at <= q->feed) q->feed++;
	}
	return at;
}
int a68q_next_place(const struct a68_queue *q)
{
	int f = q->feed >= 0 ? q->feed : q->cur;
	return f >= 0 ? f + 1 : 0;
}
void a68q_remove(struct a68_queue *q, int i)
{
	if (i < 0 || i >= q->n) return;
	move_items(q, i, i + 1, q->n - i - 1);
	q->n--;
	if (q->cur < 0) return;
	if (!q->n) { q->cur = q->feed = -1; return; }
	if (i < q->cur) q->cur--;
	else if (i == q->cur && q->cur >= q->n) q->cur = -1;
	if (q->feed >= 0) {
		if (i < q->feed) q->feed--;
		else if (i == q->feed && q->feed >= q->n) q->feed = -1;
	}
	if (q->cur < 0 || q->feed < q->cur) q->feed = q->cur;
}
int a68q_jump(struct a68_queue *q, int i)
{
	if (i < 0 || i >= q->n) return -1;
	q->cur = q->feed = i;
	return i;
}
int a68q_follow(const struct a68_queue *q, int i)
{
	if (i < 0 || i >= q->n) return -1;
	if (q->repeat == A68_REPEAT_ONE) return i;
	if (i + 1 < q->n) return i + 1;
	return q->repeat == A68_REPEAT_ALL ? 0 : -1;
}
int a68q_peek_next(const struct a68_queue *q) { return a68q_follow(q, q->cur); }
int a68q_advance(struct a68_queue *q)
{
	q->cur = q->feed = a68q_follow(q, q->cur);
	return q->cur;
}
int a68q_skip(struct a68_queue *q)
{
	int c = q->cur;
	if (c < 0) return -1;
	if (c + 1 < q->n) c++;
	else c = q->repeat != A68_REPEAT_OFF && q->n ? 0 : -1;
	q->cur = q->feed = c;
	return c;
}
int a68q_previous(struct a68_queue *q, unsigned long elapsed, int *restart)
{
	*restart = 0;
	if (q->cur < 0) return -1;
	if (elapsed > 3 || q->cur == 0) { *restart = 1; q->feed = q->cur; return q->cur; }
	q->cur = q->feed = q->cur - 1;
	return q->cur;
}
int a68q_feed_next(struct a68_queue *q)
{
	int f = a68q_follow(q, q->feed >= 0 ? q->feed : q->cur);
	if (f >= 0) q->feed = f;
	return f;
}
void a68q_heard(struct a68_queue *q, int i)
{
	if (i >= 0 && i < q->n) q->cur = i;
	if (q->feed < q->cur) q->feed = q->cur;
}
void a68q_shuffle_from(struct a68_queue *q, int from, unsigned long seed)
{
	int i;
	if (from < 0) from = 0;
	seed &= 0xFFFFFFFFul;
	for (i = q->n - 1; i > from; i--) {
		int j = from + (int)(a68_random(&seed) % (unsigned long)(i - from + 1));
		if (j != i) {
			swap_items(q, i, j);
			if (q->cur == i) q->cur = j; else if (q->cur == j) q->cur = i;
			if (q->feed == i) q->feed = j; else if (q->feed == j) q->feed = i;
		}
	}
}
void a68q_shuffle_upcoming(struct a68_queue *q, unsigned long seed)
{
	a68q_shuffle_from(q, a68q_next_place(q), seed);
}
