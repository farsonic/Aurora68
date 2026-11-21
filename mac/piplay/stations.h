/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/stations.h - Aurora68 Play's station list ("Aurora68 Play Stations", a TeachText file):
 * the built-in list, the file's lines taken apart into stations and the groups of the Radio menu's
 * submenus, and the stations of the built-in list that a user's file lacks (Radio > Add Default
 * Stations).
 *
 * Portable C with no Toolbox calls, like icy.c: the same file runs in Aurora68 Play (radio.c reads and
 * writes the file, piplay.c builds the menus) and in a host test (tools/stations_test.c, `make -C mac
 * check`). Written on 21 November 2026 (docs/08 item 87).
 *
 * The file, one line at a time (Return, line feed or both end a line; spaces around anything are
 * ignored):
 *     Name = http://host[:port]/path      a station
 *     http://host/path                    a station named after its host
 *     # a note                            (also ; a note) ignored
 *     [Group]                             the stations below are a submenu of the Radio menu
 *     [Group/Sub]                         ... a submenu of Group's submenu (one level only: the rest
 *                                         of the line, slashes and all, is Sub's name)
 *     []                                  the stations below stand in the Radio menu itself again
 * Stations above the first heading stand in the Radio menu itself, so a file of the first kind (before
 * 21 November 2026), which has no headings, makes the same flat menu as before. A heading seen again
 * adds to the group it named the first time. Items come in the order the file first names them; a group
 * with no station in it, or in a submenu of it, is left out of the menus.
 */
#ifndef PIPLAY_STATIONS_H
#define PIPLAY_STATIONS_H

#define MAX_STATIONS 128                 /* the built-in list has 40; each station takes 250 bytes */
#define MAX_GROUPS   48                  /* submenu IDs 140 to 187: an application's submenus must be 1 to 235 */
#define MAX_ITEMS    (MAX_STATIONS + MAX_GROUPS)
#define ST_TOP       (-1)                /* a station's or a group's "group": the Radio menu itself */

struct station { char name[48]; char url[200]; short group; };        /* group: ST_TOP or an index in group[] */
struct sgroup { char name[32]; short parent; short used; };            /* used: holds a station, here or below */

struct stations {
	struct station station[MAX_STATIONS];
	struct sgroup group[MAX_GROUPS];
	short order[MAX_ITEMS];              /* every station and group in the order the file names them:  */
	int nstations, ngroups, norder;      /* n >= 0 is station[n], n < 0 is group[-1 - n] */
};

extern const char stations_builtin[];    /* the list written at the first start (Mac Roman, Return-ended lines) */
extern const long stations_builtin_len;

/* the file's text into l (everything in it replaced) */
void stations_parse(struct stations *l, const char *text, long n);
/* the items of one menu (group ST_TOP: the Radio menu's own) in their order, each as in order[]; how many */
int stations_items(const struct stations *l, int group, short *items, int max);
/* the lines of defaults (a station list of the same form) whose stations l does not have yet, with their
   headings and the notes under those headings, ready to be added at the end of the file; a station counts
   as there when l has its address in the same group or in the Radio menu itself (so the flat list of the
   first kind, SomaFM's channels at the top, does not get them again in a submenu). The text's length;
   *added says how many stations it holds. Never more than max bytes: what does not fit is left out. */
long stations_missing(const struct stations *l, const char *defaults, long n, char *out, long max, int *added);

#endif
