/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_scsidev.h - virtual SCSI devices and the NCR 5380 the Mac drives them through (psmac_scsidev.c).
 *
 * Used by the solo board (psmac_board_solo.c: its disk images are targets here, the 5380 is answered here
 * entirely), by the boards with a real SCSI bus behind them (psmac_board_se.c for the SE, the Plus and the
 * original Macs: a connection with a virtual target is answered here, every other one goes to the real chip),
 * by the card layer (psmac_card.c: the image files scsi.cd= and scsi.disk= name, served on demand) and by the
 * remote control (psmac_remote.c: the CD changed while the Mac runs). Built natively too, for the command
 * layer's tests (tools/scsidev_test.c, -DPSMAC_SCSIDEV_HOST).
 */
#ifndef PSMAC_SCSIDEV_H
#define PSMAC_SCSIDEV_H
#include <stdint.h>

#define PSMAC_SCSI_TYPE_DISK 0                 /* INQUIRY's peripheral device types */
#define PSMAC_SCSI_TYPE_CDROM 5

/* ---- set up ---- */
void psmac_scsidev_init(int solo);             /* the options (scsi.cd=, scsi.disk= ...), once, before the 68k starts */
void psmac_scsidev_reset(void);                /* the 68k's RESET or the SCSI bus's: the chip as at power on, the targets' modes reset */
/* a disk image of the solo board as the target at ID id (psmac_board_solo.c): in memory (ram), a host file (fd >= 0)
   or served from the card (card = its handle + 1) */
void psmac_scsidev_add_image(int id, const char *name, uint8_t *ram, int64_t fd, int card, uint64_t bytes, int writable);
int psmac_scsidev_id_taken(int id);            /* an ID a scsi.cd= or scsi.disk= device holds */

/* ---- the 5380 as the Mac sees it ---- */
uint32_t psmac_scsidev_read(uint32_t reg);     /* registers 0-7 */
void psmac_scsidev_write(uint32_t reg, uint8_t d);
uint8_t psmac_scsidev_pdma_read(void);         /* a byte through a pseudo-DMA window */
void psmac_scsidev_pdma_write(uint8_t d);
int psmac_scsidev_irq(void);                   /* the 5380's interrupt line, when this file is the chip the Mac sees */

/* ---- the boards with a real bus (psmac_board_se.c) ---- */
struct psmac_scsi_wire { uint32_t (*rd)(uint32_t reg); void (*wr)(uint32_t reg, uint8_t v); };
void psmac_scsidev_bus(const struct psmac_scsi_wire *w, int empty);   /* the real chip's registers; empty: no real bus at all */
void psmac_scsidev_bus_empty(int empty);       /* the probe's answer changed (mac.sescsi=auto after a RESET) */
int psmac_scsidev_bus_write(uint32_t reg, uint8_t d);   /* every write of the Mac's: 1 answered here, 0 to be sent to the real chip */
int psmac_scsidev_local(void);                 /* reads and pseudo-DMA answered here (a virtual target's connection, or no bus) */
int psmac_scsidev_configured(void);            /* any virtual device */
int psmac_scsidev_cd_id(void);                 /* the (first) CD-ROM drive's ID, or -1 */

/* ---- the card (psmac_card.c, core 3) ---- */
/* the i-th device that wants a file from the card: its name, and whether it may be written; 0 past the last */
const char *psmac_scsidev_card_want(int i, int *writable);
void psmac_scsidev_card_ready(int i, int handle, uint64_t bytes, int writable);   /* found and served from the card */
void psmac_scsidev_card_missing(int i);        /* no such file */
/* a CD change asked for (the remote control): the CD's index and the new name ("" ejects); 1 when one waits */
int psmac_scsidev_card_change(int *i, char *name, unsigned max);

/* ---- the remote control and the heartbeat ---- */
int psmac_scsidev_cd_request(const char *name, int id, int force);   /* a new CD in the drive at id (-1: the first CD drive); "" ejects;
                                                           force: even if the Mac holds it (the paperclip); 0 asked, <0 why not */
int psmac_scsidev_json(char *out, unsigned max);           /* the devices as JSON */
void psmac_scsidev_report(void);

/* ---- what the module needs from the kernel (stubbed by the native test) ---- */
const char *psmac_option(const char *key);
int psmac_card_img_io(int h, int write, uint32_t lba, uint32_t n, void *buf, int wait);
#endif
