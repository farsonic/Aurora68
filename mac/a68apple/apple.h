/* SPDX-License-Identifier: MPL-2.0
 *
 * mac/a68apple/apple.h - what apple.c (the window, the keys, the menus) and disks.c (the disks from the Mac) share.
 * 24 November 2026, docs/20-apple-ii-card.md "Disks from the Mac".
 */
#ifndef A68APPLE_APPLE_H
#define A68APPLE_APPLE_H
#include "toolbox.h"
#include "psmac_a2.h"

/* apple.c */
extern volatile unsigned char *card;          /* the card's window (24- or 32-bit address), 0 without the card */
extern volatile struct a68a_block *blk;
extern unsigned char pbuf[256];
extern void *m_disks;
void command(unsigned long c, unsigned long a0, unsigned long a1);
void notice(const volatile char *text);
void beat(void);

/* disks.c */
#define M_DISKS_ID   132                       /* the Disks menu (apple.c's M_DISKS) */
#define DK_INSERT    1                         /* its items: Insert Disk..., a line, the six units, a line, the folder's */
#define DK_UNIT1     3
#define DK_FOLDER    10
#define DK_SHOW      11
#define DK_AGAIN     12
#define DK_CARD1     14                        /* the card's own images (the SD card's root) from here */
void disks_init(void);                         /* after the card is open: the session, the folder, the Finder's files */
void disks_menu_build(void);                   /* the Disks menu's fixed items (once) */
void disks_menu(void);                         /* brought up to date before the menu is shown */
void disks_chosen(short menu, short item);     /* the Disks menu or one of the folder's submenus */
int disks_serve(void);                         /* the card's requests answered (every turn of every loop): how many */
long disks_sleep(long dflt);                   /* WaitNextEvent's sleep while disks are served */
void disks_resume(void);                       /* the program in front again: the folder looked at again */
void disks_hle(EventRecord *ev);               /* a high-level event (an Apple event) */
int disks_quit_asked(void);                    /* the Finder's Quit event came */
void disks_quit(void);                         /* every disk back to the Mac, then the files closed */
int disks_is_menu(short menu);

#endif
