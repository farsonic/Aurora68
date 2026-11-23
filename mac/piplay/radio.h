/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/piplay/radio.h - PiPlay's internet radio (radio.c): what the window and the menus (piplay.c)
 * need of it. Written on 21 November 2026 (docs/08 item 83).
 */
#ifndef PIPLAY_RADIO_H
#define PIPLAY_RADIO_H
#include "stations.h"

enum { RS_OFF, RS_RESOLVE, RS_OPEN, RS_SEND, RS_HEADERS, RS_PLAYING, RS_RETRY, RS_DRAIN };

struct radio_ui {
	int st;                          /* RS_*: RS_OFF when no station plays */
	int prebuffering;                /* the card is paused until about 3 s are in hand */
	int paused;                      /* Pause: the card is paused, the station goes on into its buffer */
	char station[48];                /* the station's name in the list (or its host) */
	char url[200];                   /* its address */
	char name[64];                   /* what the station calls itself (icy-name) */
	char title[128];                 /* now playing (StreamTitle), Mac Roman */
	unsigned long br;                /* kbit/s, from icy-br or the decoder */
	unsigned long reconnects, underruns;   /* connections lost and made again; times the card ran dry */
	int ipv;                         /* 4 or 6: the IP version of the connection (icy_ip_version), 0 before one */
	unsigned long held_ms;           /* what the card holds and has not played, in ms */
	unsigned long retry_at;          /* Ticks of the next attempt (RS_RETRY) */
	const char *error;               /* a failure to tell the user; the window clears it after showing it */
	unsigned long seq;               /* bumped whenever something above changes */
	/* a track of the music server's library (radio_play_track, 22 November 2026, docs/08 item 99): a stream
	   with an end. When it ends, next() is asked for the next track's address: the card plays on and the
	   next stream follows the last one's audio in its buffer (boundary_ms: ps->pos_ms where the next one
	   will start). When there is none, the card plays out what it holds (RS_DRAIN) and ended is set.
	   A connection lost on the way is made again from where its audio stopped: resume() rewrites the
	   address for the seconds that came. */
	int finite, ended;
	int video;                       /* a music video on the video decoder card (radio_play_video, vcard.c): no next() */
	int (*next)(char *url, int max, unsigned long boundary_ms);
	void (*resume)(char *url, int max, unsigned long secs);
	int (*other_host)(void);         /* 1.8: a connection to the library's server failed: 1 if it moved to another address */
};
extern struct radio_ui radio;

extern struct stations *slist;         /* the station list (stations.h), from the heap */

void radio_init(void);
/* plays the station at url (name for the window); 0 with radio.error set if it cannot even start */
int radio_play(const char *name, const char *url);
int radio_play_track(const char *name, const char *url);   /* the same, as a library track (finite above) */
/* a music video's MPEG-1 stream (/vmpeg/ID) into the video decoder card; without the card, as a track */
int radio_play_video(const char *name, const char *url);
void radio_stop(void);                 /* closes the connection, stops the card */
void radio_pause(int on);
void radio_poll(void);                 /* from the event loop, as often as it can */
void radio_quit(void);                 /* everything given back to MacTCP and the resolver */
const char *radio_status(char *buf);   /* a few words for the window: "Buffering 40%", "Playing" ... */
const char *radio_problem(void);       /* while it waits to try again: why the connection was lost */
/* the station list ("Aurora68 Play Stations", "PiPlay Stations" before 21 November 2026): read (written first from the built-in list if it is not
   there); 1 if it was read again because it changed since the last look */
int stations_load(void);
int stations_check(void);
/* Radio > Add Default Stations: the built-in stations the file lacks added at its end (stations.c says which
   count as there) and the list read again; how many, 0 if it has them all, -1 if the file could not be written */
int stations_add_defaults(void);

/* the family tried first when a name has both (Library or Radio > Connect By, kept in the preferences):
   6 IPv6, 4 IPv4; the other is tried when the first fails. For the radio and the library alike. */
extern int net_prefer;
/* Connect By > IPv4 Only (1.8, docs/08 item 124): a name's IPv6 addresses are passed over, the library's server is
   reached at its IPv4 address alone (net_prefer is 4 with it) */
extern int net_v4only;

/* calls to the network driver that could wait (1.8.1, docs/08 item 127; radio.c): none is synchronous. net_call makes the
   call asynchronously and waits secs seconds at most, the receive chain kept going: its result, or 1 when it has not come
   back (logged: "what did not complete in N s: then"); the block is then still the driver's and goes to net_leave, which
   keeps it, and the buffers the driver may still write into, until the driver gives them back (net_leave_poll, from the
   event loop), releasing a stream left so (release_cs on ref; from_create: the stream a late TCPCreate or UDPCreate makes,
   read from the first block). net_leave_block puts a new cleared block of size bytes in *pp (a pointer to the caller's
   pointer; 0 if there is no memory); net_fresh gives a cleared parameter block of PB_SIZE bytes, a new one when the
   driver still has the old */
struct net_left;
short net_call(void *pb, int secs, const char *what, const char *then);
struct net_left *net_leave(short ref, short release_cs, unsigned long stream, int from_create);
void net_leave_block(struct net_left *l, void *pp, unsigned long size, int is_pb);
void *net_fresh(void *pp, const char *what);
void net_leave_poll(void);
#define NET_PB_SIZE 132                  /* MacTCP's TCPiopb is 104 bytes, .IPP6's TCP6iopb 130 */

/* from piplay.c */
void pi_log(const char *text);
int pi_command(unsigned long c);
void radio_chain_words(char *t);
void radio_keep(void);               /* the receive chain going again if it waits for room (1.8) */     /* the receive chain's state in words, 100 bytes (1.8) */
int pref_place(short *vref, long *dirid);
char *put_str(char *p, const char *s);
char *put_num(char *p, unsigned long v);
#endif
