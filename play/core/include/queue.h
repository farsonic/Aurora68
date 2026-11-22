/* SPDX-License-Identifier: MPL-2.0
 *
 * play/core/include/queue.h - the play queue both Plays keep: the songs to play, which of them is heard, which
 * is being sent ahead, and the rules for Next, Previous, Play Next, Add, Remove, Shuffle and Repeat.
 *
 * The items are the caller's, of any fixed size (the classic Play's are its songs themselves, 112 bytes each;
 * the macOS Play's are 32-bit handles of Swift values it keeps): the queue only moves them about in the
 * caller's array. Two places in it matter. "cur" is the item heard. "feed" is the one whose stream is being
 * sent: the classic Play pours the next song's stream into the card before the last one has been heard, so an
 * album plays without a gap, and feed runs ahead of cur until the card reaches the boundary
 * (a68q_heard). The macOS Play's AVQueuePlayer does the same work itself, so there feed is always cur.
 * Play Next puts songs after feed, the one being sent, so a song already in the card's buffer is not cut.
 * Written on 22 November 2026 (docs/08 item 107) from the classic Play's library.c and the macOS Play's
 * PlayQueue.swift.
 */
#ifndef A68_QUEUE_H
#define A68_QUEUE_H

enum { A68_REPEAT_OFF, A68_REPEAT_ALL, A68_REPEAT_ONE };

struct a68_queue {
	unsigned char *items;            /* the caller's array: max items of size bytes */
	unsigned long size;
	int max;
	int n;                           /* how many are queued */
	int cur;                         /* the item heard (playing or paused), -1 when none */
	int feed;                        /* the item being sent ahead, -1 when none (cur when nothing runs ahead) */
	int repeat;                      /* A68_REPEAT_* */
};

void a68q_init(struct a68_queue *q, void *items, unsigned long size, int max);
void *a68q_at(const struct a68_queue *q, int i);
/* n items already written at the front of the array are the queue now, start heard and sent first */
void a68q_set(struct a68_queue *q, int n, int start);
void a68q_clear(struct a68_queue *q);
/* item put in at place at (0 to n; anything else: the end); where it went, -1 if the queue is full. A queue
   that played nothing starts at its first item */
int a68q_insert(struct a68_queue *q, int at, const void *item);
/* where Play Next puts songs: after the one being sent (the front of a queue that sends nothing) */
int a68q_next_place(const struct a68_queue *q);
/* take out item i; the one heard moves on to its follower (none if it was the last) */
void a68q_remove(struct a68_queue *q, int i);
/* play item i now; -1 if there is no such item */
int a68q_jump(struct a68_queue *q, int i);
/* what follows item i at its natural end: itself (repeat one), the next, the first again (repeat all), -1 */
int a68q_follow(const struct a68_queue *q, int i);
/* what follows the item heard */
int a68q_peek_next(const struct a68_queue *q);
/* the item heard ended by itself: the next one heard (and sent); -1 when the queue has played */
int a68q_advance(struct a68_queue *q);
/* Next was pressed (repeat one does not hold it): the next one, round again at the end unless repeat is
   off; -1 when there is none */
int a68q_skip(struct a68_queue *q);
/* Previous was pressed: more than three seconds into the song (or on the first), *restart is set and the
   same item stays; else the one before. The item to play */
int a68q_previous(struct a68_queue *q, unsigned long elapsed_secs, int *restart);
/* the stream sent ahead has ended: the next to send (a68q_follow of feed), now feed; -1 if none */
int a68q_feed_next(struct a68_queue *q);
/* the card has reached item i (sent ahead earlier): it is heard now */
void a68q_heard(struct a68_queue *q, int i);
/* the items from place from to the end shuffled (Fisher and Yates), from the caller's seed */
void a68q_shuffle_from(struct a68_queue *q, int from, unsigned long seed);
/* what is still to come shuffled: the items after the one being sent; those heard and sent stay */
void a68q_shuffle_upcoming(struct a68_queue *q, unsigned long seed);
#endif
