/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_recovery.h - Aurora68 Recovery (docs/08 item 106): a start of the kernel in which the 68k is never
 * released and the Pi offers its own SD card over the network instead, for a Mac that will not start or a
 * Pi whose card cannot be reached. psmac_recovery.c is the mode (getting in, the screen, the web page, the
 * keyboard menu); psmac_card.c has the card's side (the root directory as a file manager sees it, the boot
 * counter, the request file).
 */
#ifndef PSMAC_RECOVERY_H
#define PSMAC_RECOVERY_H
#include <stdint.h>

/* ---- the mode ---- */
int  psmac_recovery_on(void);                   /* 1 once this start is a recovery start */
void psmac_recovery_phase1(void);               /* core 0, before the mailbox and core 3: the option, the request file, the counter */
void psmac_recovery_phase2(void);               /* core 0, as the 68k would be released: the programmer's switch */
void __attribute__((noreturn)) psmac_recovery_park(void);   /* core 0 for the rest of a recovery start */
void psmac_recovery_poll(void);                 /* core 3, every turn of the service loop (both modes) */
int  psmac_recovery_key(uint32_t usage, int down, uint32_t mods);   /* a USB/Bluetooth key (HID usage): 1 when taken */
void psmac_recovery_adb_key(uint8_t code, int up);                  /* the Plus's and 512K's own keyboard (ADB codes) */
void psmac_recovery_finder(void);               /* the service core: the Finder is running */
int  psmac_recovery_ask(const char *why);       /* a running system asks: recovery.txt, then the Pi restarts. 0 asked */
/* the TCP side, while psmac_recovery_on() (psmac_remote.c hands psmac_ip.c's calls over) */
int      recovery_tcp_accept(int c, uint16_t port, const uint8_t peer[4]);
void     recovery_tcp_recv(int c, const uint8_t *data, uint32_t len);
uint32_t recovery_tcp_room(int c);
uint32_t recovery_tcp_produce(int c, uint8_t *buf, uint32_t room);
void     recovery_tcp_closed(int c);

/* ---- the card (psmac_card.c) ---- */
#define REC_NAME_MAX 100
#define REC_LFN_MAX  20                         /* long-name entries of one name: 20 x 13 = 260 characters */
struct rec_ent {
    char name[REC_NAME_MAX];                    /* the long name (ASCII; others as '?'), else the short one as shown */
    char sname[13];                             /* the 8.3 name, "KERNEL8.IMG" */
    uint32_t size, clus;
    uint8_t attr, nlfn, cut;                    /* FAT attributes; long-name entries; the name was longer than name[] */
    uint32_t lba, off;                          /* the short entry: sector and byte */
    uint32_t lfn_lba[REC_LFN_MAX]; uint16_t lfn_off[REC_LFN_MAX];
};
struct rec_rd { uint32_t clus, size, pos; };
struct rec_wr { int open; uint32_t lba, off, first, last, size, nclus, fill; uint8_t *buf; };
int  psmac_card_rec_ready(void);                /* 1 mounted and writable, 0 not yet looked at, -1 no card, -2 read only */
const char *psmac_card_rec_what(void);          /* "SDHC, FAT32, 32 KB clusters" */
uint32_t psmac_card_rec_cluster(void);          /* bytes */
int  psmac_card_rec_list(int (*cb)(void *ctx, const struct rec_ent *e), void *ctx);
int  psmac_card_rec_find(const char *name, struct rec_ent *e);           /* 0 found */
int  psmac_card_rec_free_kb(uint32_t *kb);       /* 0 known */
void psmac_card_rec_read_open(const struct rec_ent *e, struct rec_rd *r);
int32_t psmac_card_rec_read(struct rec_rd *r, uint8_t *buf, uint32_t max);   /* bytes (0 at the end), -1 a card error */
int  psmac_card_rec_create(const char *name, const char *near, struct rec_wr *w);   /* a new empty file; near: put it beside that one */
int  psmac_card_rec_write(struct rec_wr *w, const uint8_t *d, uint32_t n);
int  psmac_card_rec_close(struct rec_wr *w);
void psmac_card_rec_abandon(struct rec_wr *w);   /* a write that will not finish: its entry and clusters go */
int  psmac_card_rec_delete(const char *name);    /* the entries, and the clusters unless another name starts with them */
int  psmac_card_rec_rename(const char *from, const char *to);
int  psmac_card_rec_replace(const char *target, const char *temp);    /* target's content becomes temp's; temp goes */
int  psmac_card_rec_swap(const char *a, const char *b);              /* the two files' contents change places */
int  psmac_card_rec_get(const char *name, uint8_t *buf, uint32_t max); /* a small file whole: its length, or -1 */
int  psmac_card_rec_put(const char *name, const uint8_t *d, uint32_t n);   /* a small file, through a temporary and replace */
void psmac_card_rec_repair(void);               /* a recovery start: what an interrupted upload or swap left */
const char *psmac_card_rec_error(void);         /* the last operation's reason, for the page */
/* the boot counter and the request file: core 0 before the 68k (phase 1), then core 3 */
int  psmac_card_boot_read(int *count, char *flag, uint32_t flagmax, int *flag_len);   /* 0 the card was read */
void psmac_card_boot_write(int count);
void psmac_card_boot_flag_drop(void);
void psmac_card_boot_reset_later(void);         /* core 3's next card turn writes 0 */
int  psmac_card_ask_recovery(const char *why);  /* queued for core 3; then psmac_card_ask_state */
int  psmac_card_ask_state(void);                /* 0 none, 1 being written, 2 written, 3 failed */

#endif
