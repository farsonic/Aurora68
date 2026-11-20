/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_usbdisk.h - USB flash drives as PiDisk units: what psmac_usbdisk.c (core 1, the USB side)
 * and psmac_mbox.c (core 3, the Mac's block service) tell each other. Written 20 November 2026.
 *
 * Core 1 owns the sticks: it finds them after each enumeration of the bus, speaks bulk-only
 * transport to them and runs one job at a time for the Mac. Core 3 owns the mailbox: it turns a
 * stick into a PiDisk unit in the window's device table, copies the Mac's data to and from the
 * job's buffer and answers the Mac's driver. Between the two there is a published state per stick
 * (read with a sequence count, as a seqlock) and the job slot.
 */
#ifndef PSMAC_USBDISK_H
#define PSMAC_USBDISK_H
#include <stdint.h>

#define USBDISK_UNITS 4                 /* sticks tracked at once: PiDisk's table has four units in all */

/* what the Mac may see of a stick (core 1 writes, core 3 copies) */
struct usbdisk_info {
    uint32_t present;                   /* a medium the Mac may use: found, readable, not ejected */
    uint32_t media;                     /* +1 for every new medium (a stick plugged in, or plugged back in) */
    uint32_t blocks;                    /* what the Mac is given: a whole bare HFS stick, or its HFS partition */
    uint32_t writable;
    char name[48];                      /* for the device table: "USB SanDisk Cruzer Blade" */
};

/* core 3: a change counter (moves whenever any unit's published state moved), then the state of
   one unit; 0 while core 1 is writing it (try again on the next turn) */
uint32_t psmac_usbdisk_changes(void);
int psmac_usbdisk_info(int unit, struct usbdisk_info *out);

/* the job slot: one request of the Mac's at a time, as PiDisk has one command block */
enum { USBDISK_OP_READ = 1, USBDISK_OP_WRITE, USBDISK_OP_EJECT };
uint8_t *psmac_usbdisk_buffer(void);                     /* the job's data, USBDISK_BUF bytes */
#define USBDISK_BUF (64u * 1024u)
uint32_t psmac_usbdisk_max_blocks(int unit);             /* blocks per job for this stick (its bus speed) */
int psmac_usbdisk_post(int unit, uint32_t media, int op, uint32_t lba, uint32_t count);   /* 0 posted, -1 the slot is busy */
int psmac_usbdisk_result(int32_t *status);               /* 1 done (status: 0 or PSMB_E_*, the slot is free again), 0 running, -1 no job */

/* core 1 (psmac_usb.c): after every enumeration, while core 3 keeps off the bus (changed: bit n for a
   hub port n that saw a plug or an unplug, bit 0 the root port); and between enumerations, the
   sticks' work (returns 1 when it did something) */
void psmac_usbdisk_claim(unsigned hub_ports, int root_speed, int root_connected, int root_taken, int dma, uint32_t changed);
uint32_t psmac_usbdisk_seen(void);                       /* sticks found by the last claim (psmac_usb.c's retries leave them out) */
int psmac_usbdisk_core1(void);
void psmac_usbdisk_report(void);                         /* the heartbeat (core 3) */
int psmac_usbdisk_owns(unsigned addr);                   /* psmac_usbnet.c: a device of ours, leave it alone */
#endif
