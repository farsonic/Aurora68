/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/library.h - Aurora68 Play's Library (library.c): the music server's artists, albums and
 * tracks in a window of their own, a queue played through the radio's connection, and Now Playing in the
 * player window. Written on 22 November 2026 (docs/08 item 99).
 */
#ifndef PIPLAY_LIBRARY_H
#define PIPLAY_LIBRARY_H
#include "toolbox.h"

void lib_init(int colour);             /* colour: Color QuickDraw is there */
void *lib_window(void);                /* the Library window, 0 before it is first shown */
void lib_show(void);                   /* Library > Show Library: made, shown, brought to the front */
void lib_close(void);                  /* its close box */
void lib_poll(void);                   /* from the event loop: the network, the queue */
void lib_resized(void);

/* events in the Library window */
void lib_update(void);
void lib_activate(int on);
void lib_click(EventRecord *ev);
void lib_key(EventRecord *ev);
int lib_ctl_action(void *ctl, int part);   /* 1 if ctl is the Library's */

/* the player window's part: a library track is the player's (lib_active) */
int lib_active(void);
void lib_leave(void);                  /* a file or a station takes the player: the queue stops */
void lib_draw_now(void *win, short W, short H);
void lib_draw_progress(void *win, short W, short H);
int lib_bar_click(Point local, short W);   /* 1 if it was the progress line: a seek */
int lib_playing(void), lib_can_stop(void), lib_can_skip(int dir);
void lib_play_pause(void), lib_stop(void), lib_skip(int dir);
const char *lib_status(char *buf);

/* menus */
int lib_have_server(void);
void lib_connect(void);                /* Connect to Server...: an address, or empty to look again */
void lib_find(void);
void lib_play_all(void);
int lib_can_play_all(void);
void lib_toggle_shuffle(void);
int lib_shuffle(void);
void lib_live(void);
void lib_reload(void);

/* the preferences file's part */
struct lib_prefs { char host[64]; unsigned short port; short shuffle; Rect r; short shown; short pad; };
void lib_get_prefs(struct lib_prefs *p);
void lib_set_prefs(const struct lib_prefs *p);

/* from piplay.c */
void say_error(const char *text);
void message_box(const char *text, int lines_h);
int ask_text(const char *prompt, const char *hint, const char *initial, char *out, int max);
void start_station(const char *name, const char *url);
void player_lib_mode(void);            /* the player window shows the library's track */
void player_refresh(void);             /* its texts, buttons and progress line again */
Rect screen_rect(void);
#endif
