/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/vcard.h - Aurora68 Play's music videos on the Aurora68 Video Decoder card (vcard.c): the card's
 * driver, its ring for radio.c's receive chain, and the video window. Written on 23 November 2026 (docs/08
 * item 114, docs/16 "Follow-up: Aurora68 Play's music videos").
 */
#ifndef PIPLAY_VCARD_H
#define PIPLAY_VCARD_H

int vc_init(void);                     /* the card's driver ".A68Video" opened as Aurora68 Video opens it: 1 if it is there */
int vc_present(void);

/* the stream: the card's half of radio.c's connection (a /vmpeg/ID answer's body goes into the ring) */
unsigned long vc_room(void);           /* bytes the ring has room for */
void vc_write(const unsigned char *p, unsigned long n);   /* into the ring, head moved after them (interrupt time) */
void vc_begin(void);                   /* before a new stream's bytes: the card stopped (its ring emptied), the end cleared */
void vc_go(void);                      /* the stream's answer has come: its place, volume, then start */
void vc_pause(int on);
void vc_stop(void);
void vc_end(void);                     /* the last byte is in the ring */
int vc_state(void);                    /* A68V_S_* (psmac_video.h); 0 without the card */
unsigned long vc_pos_ms(void);         /* the stream's time of the picture on the screen */
const char *vc_message(void);          /* the card's words for its state */
void vc_volume(int v);                 /* 0-100, Play's volume */
void vc_report(const char *why);       /* the card's numbers into the log */

/* the window: the picture, w x h, the video's title; shown and brought to the front, the first time below near (the
   player window) if the screen has room */
void vc_window_open(const char *title, unsigned w, unsigned h, void *near);
void vc_window_hide(void);             /* its close box: the picture hidden, the sound goes on */
void vc_window_dispose(void);          /* no video any more */
int vc_window_shown(void);
void *vc_window(void);                 /* 0 before the first video */
void vc_update(void);                  /* its update event */
void vc_window_show(void);             /* Controls > Show Video */
/* the window's size (1.7, docs/08 item 120): the picture as the server sends it, twice that, or as large as the screen it
   is on allows (its shape kept; the card scales it, nearest pixel). Controls > Normal Size, Double Size, Fill Screen
   (Command-1, 2, 3) and the window's zoom box (Normal and Fill Screen); kept in the preferences */
enum { VC_NORMAL, VC_DOUBLE, VC_FILL, VC_CUSTOM };   /* VC_CUSTOM (1.8): the size the size box gave (vc_custom_w) */
void vc_set_zoom(int z);
int vc_zoom(void);
void vc_zoom_box(void);                /* the zoom box: Fill Screen, or back to the size it had */
/* 1.8 (docs/08 item 124): the size box, the picture's shape kept; the width it gave, kept in the preferences */
void vc_grow(long where);
long vc_custom_w(void);
void vc_set_custom_w(long w);
void vc_activate(int on);              /* its activate event: the size box */
void vc_strip(const char *t);          /* the words in the strip below the picture (the time) */
/* the size to ask the server for, from the screen the window goes on (the one the player window is on) */
void vc_size_for(void *near_win, unsigned long vw, unsigned long vh, unsigned *w, unsigned *h);
/* hidden from the card while Play's menu is down, a window of Play's is dragged or sized or a dialog is up (counted);
   otherwise the card draws where the window shows (its visRgn, 1.8), in the background too */
void vc_hold(int on);
void vc_poll(int foreground);          /* from the event loop: the window's place and visibility kept current */
#endif
