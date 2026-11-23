/* SPDX-License-Identifier: MPL-2.0
 *
 * play/core/include/catalog.h - the library as a Play holds it: a page of a list (artists, an artist's albums, an
 * album's tracks, what a search found) as rows, with the lines a window shows around them ("Previous page...",
 * "Next page..."), a row found again by its id after the library changed, and a row found by typing its name.
 *
 * The classic Play keeps one page of 200 rows (it never holds the whole library: a 68000's memory is small and
 * the owner's library is not); the macOS Play keeps every artist and album, which it reads page after page
 * with a68_next_from (proto.h) and the same record readers. A row's texts have fixed sizes, the classic Play's,
 * because a page is an array; the macOS Play reads the records themselves and has no such limits. Written on
 * 22 November 2026 (docs/08 item 107) from the classic Play's library.c. Music videos the same day (item 109): with
 * the client's videos=1 an album's or an artist's tracks are followed by its videos, rows of kind 'V', and a search
 * finds them too; an artist's row counts its videos.
 *
 * Videos linked to songs in the classic Play (1.7, 23 November 2026, docs/08 item 118): a track's row keeps the id of
 * its music video (the tenth field with full=1&videos=1, which the classic Play now sends for an album's tracks), an
 * album's row its count of videos (the eighth), and two more views list videos alone: every one in the library
 * (/mac/videos, sorted by artist) and one artist's (/mac/videos?artist=ID).
 */
#ifndef A68_CATALOG_H
#define A68_CATALOG_H
#include "proto.h"

enum { A68_V_ARTISTS, A68_V_ALBUMS, A68_V_TRACKS, A68_V_SEARCH,     /* what a list holds */
       A68_V_ARTIST,         /* an artist's tracks and videos (/mac/tracks?artist=ID): one with videos and no album */
       A68_V_VIDEOS,         /* every music video of the library, by artist (/mac/videos) */
       A68_V_ARTVIDEOS };    /* an artist's music videos (/mac/videos?artist=ID) */
#define A68_PAGE 200                     /* rows asked for at once: the server's page */

struct a68_row {                         /* one row of a list */
	char id[9], album[9], kind;          /* kind: 'A' artist, 'L' album, 'T' track, 'V' music video */
	unsigned short num, count;           /* a track's number; an artist's albums, an album's tracks */
	unsigned short videos;               /* an artist's videos (videos=1); an album's (full=1&videos=1) */
	unsigned short vw, vh;               /* a video's picture (its V record's width and height; 0 when not said): the
	                                        classic Play 1.6 asks /vmpeg for a size of its shape (23 November 2026) */
	unsigned long secs;
	char text[48], sub[40];              /* the name, title; the artist (an album, a track) */
	char video[9];                       /* a track's music video (full=1&videos=1, servers of 23 November 2026 on); ""
	                                        when it has none or the server did not say */
};
struct a68_list {
	struct a68_row *rows;                /* the caller's array, max rows */
	int max, n;
	long total, from;                    /* the rows the server has for the view; the first one here */
	int kind;                            /* A68_V_* */
};

void a68_list_init(struct a68_list *l, struct a68_row *rows, int max, int kind);
/* the view's request: a page from from (artists; an artist's albums; an album's tracks; a search's words) */
int a68_list_path(const struct a68_client *c, int kind, const char *id, const char *words, long from, char *out, long max);
/* the answer to it: its rows (what does not fit is left out); 1 if it was the server's list, 0 if not. The body is
   taken apart in place */
int a68_list_load(struct a68_list *l, char *body, long len);
/* the row with that id, -1 if none */
int a68_list_find(const struct a68_list *l, const char *id);
/* how many rows are music videos (they come after an album's tracks) */
int a68_list_videos(const struct a68_list *l);
/* the first row whose name starts with what was typed (n bytes, lower case; a "The " in front of a name is left
   out unless what was typed starts "th"), -1 if none */
int a68_list_typed(const struct a68_list *l, const char *typed, int n);
/* the seconds of every track row (not the videos): an album's length */
unsigned long a68_list_secs(const struct a68_list *l);

/* the lines a window shows: "Previous page..." if the page is not the first, the rows, "Next page..." if more
   come after; a line's row (-1 the previous page, -2 the next) and a row's line */
int a68_list_has_prev(const struct a68_list *l);
int a68_list_has_next(const struct a68_list *l);
int a68_list_lines(const struct a68_list *l);
int a68_list_line_row(const struct a68_list *l, int line);
int a68_list_row_line(const struct a68_list *l, int row);
#endif
