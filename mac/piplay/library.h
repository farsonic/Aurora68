/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/library.h - Aurora68 Play's Library (library.c): the music server's artists, albums and
 * tracks in a window of their own, a queue played through the radio's connection, and Now Playing in the
 * player window. Written on 22 November 2026 (docs/08 item 99).
 */
#ifndef PIPLAY_LIBRARY_H
#define PIPLAY_LIBRARY_H
#include "toolbox.h"
struct dv_state;

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
void lib_seek(unsigned long secs);     /* the library's track from secs */

/* the device (devices.c, docs/08 item 105): the id kept in the preferences, the Mac's name; and what a
   library track plays now (1 if one does) */
void lib_device(const char *id, const char *name);
int lib_now(char *tid, unsigned long *pos, unsigned long *len);

/* menus */
int lib_have_server(void);
void lib_connect(void);                /* Connect to Server...: an address, or empty to look again */
void lib_find(void);
void lib_play_all(void);
int lib_can_play_all(void);
void lib_toggle_shuffle(void);
enum { LIB_REPEAT_OFF, LIB_REPEAT_ALL, LIB_REPEAT_ONE };   /* play/core/queue.h's A68_REPEAT_* */
void lib_toggle_repeat(int which);     /* Library > Repeat (1.4): the queue round again; Repeat One (1.7): the song */
int lib_repeat(void);                  /* LIB_REPEAT_* */
int lib_shuffle(void);
void lib_live(void);
void lib_reload(void);
/* 1.7 (docs/08 item 118): the videos, the queue, the server found by itself */
void lib_videos(void);                 /* Library > Videos: every video, by artist */
void lib_up_next(void);                /* Library > Up Next: the queue as a list */
int lib_can_play_video(void);
void lib_play_video(void);             /* Library > Play Video: the selected song's video */
int lib_can_add(void);
void lib_add(int next);                /* Library > Play Next (1) and Add to Queue (0): the selected row */
int lib_instead(void);
void lib_toggle_instead(void);         /* Controls > Play Videos Instead of Songs */
void lib_start(void);                  /* after the preferences: the probe if no server is remembered */
int lib_finding(void);
int lib_servers_gen(void);             /* changes when Library > Servers should be made again */
int lib_servers(void);                 /* its servers: those found, and the one in use */
const char *lib_server_text(int i, char *buf, int *current);   /* "Name (address:port)"; buf: 120 bytes */
void lib_choose_server(int i);
void lib_search_again(void);

/* the preferences file's part */
struct lib_prefs { char host[64]; unsigned short port; short shuffle; Rect r; short shown; short repeat; };   /* repeat: was a pad before 1.4 */
void lib_get_prefs(struct lib_prefs *p);
void lib_set_prefs(const struct lib_prefs *p);
/* the preferences file's tail (1.7): the server chosen, by name; Play Videos Instead of Songs */
struct lib_prefs_ext { char srv_name[48]; short instead; short pad; };
void lib_get_ext(struct lib_prefs_ext *e);
void lib_set_ext(const struct lib_prefs_ext *e);

/* from piplay.c */
void say_error(const char *text);
void message_box(const char *text, int lines_h);
int ask_text(const char *prompt, const char *hint, const char *initial, char *out, int max);
void start_station(const char *name, const char *url);
void player_lib_mode(void);            /* the player window shows the library's track */
void player_refresh(void);             /* its texts, buttons and progress line again */
void *player_window(void);             /* the player window (the video window opens on its screen) */
Rect screen_rect(void);
/* the player, for a device's commands and its state (piplay.c) */
void player_pause(int pause);          /* 1 pause, 0 resume: whatever plays */
void player_stop(void);
void player_seek(unsigned long secs);
void player_volume(int v);             /* 0-100 */
void player_dev_state(struct dv_state *s);
#endif
