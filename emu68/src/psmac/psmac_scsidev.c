/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_scsidev.c - virtual SCSI devices: a CD-ROM drive and hard disks served from image files, answered by
 * the kernel on the Mac's SCSI bus, and the NCR 5380 the Mac reaches them through.
 * Copyright (c) 2026 Aurora68 project.
 *
 * Backstory (25 November 2026). The owner will install A/UX 3.x, from CD images (.iso, .cdr, .toast). A/UX's
 * kernel drives the 5380 itself; it cannot use PiDisk (a Mac OS driver over the mailbox), so its installation CD
 * and the disk it installs onto must be SCSI devices. Two kinds of board need them:
 *   - the solo board (psmac_board_solo.c), which has no Macintosh behind it: its 5380 and its disk images were
 *     answered in that file since 17 November (docs/08 item 35). That 5380 is here now, with the targets, and
 *     the solo board's disk images are targets like the others;
 *   - the boards with a real SCSI bus behind them (the SE, the Plus; psmac_board_se.c), where every access of
 *     the 68k's to the 5380 goes to the real chip. There a virtual device owns an ID: when the Mac selects that
 *     ID, the access that would put the selection on the real bus is kept back, the real chip is let go (its
 *     SEL, BSY and data lines released: the bus is free) and the connection is answered here, every phase of
 *     it, the registers, DRQ and the pseudo-DMA windows, the interrupt (the RBV's flag, as the solo board has
 *     it). After the bus is free again the Mac's accesses stay here until it starts the next arbitration (or
 *     resets the bus, or selects a real ID without arbitrating): then the real chip is given the registers the
 *     Mac last wrote (data, target command, initiator command, mode) and everything goes to it again. So the
 *     real bus only ever sees whole connections with real targets.
 *   The empty bus of item 70 (no terminator, mac.sescsi=empty, the 128K and 512K) was a 5380 with no target
 *   answered in psmac_board_se.c; it is this 5380 now, with the virtual devices on it if there are any.
 *
 * The rule for IDs. A virtual device must not share an ID with a real one. The first selection of each virtual
 * ID in a session goes to the real bus as the Mac made it; if a real target answers (BSY within PROBE_MS), the
 * virtual device at that ID is turned off for the session with a warning in the log and on the HDMI screen, and
 * the connection is the real device's. scsi.probe=off leaves the check out. ID 7 is the Mac's.
 *
 * The devices.
 *   CD-ROM (peripheral type 5, removable), read only, from an image of 2048-byte sectors (.iso, .cdr, a .toast
 *   that is a plain image; an Apple hybrid CD with a partition map and HFS or A/UX partitions is the same kind of
 *   file). Its INQUIRY says SONY CD-ROM CDU-8003A 1.9a (the AppleCD 300, whose Apple firmware Apple's CD-ROM
 *   driver and A/UX accept; scsi.cdvendor=, scsi.cdproduct=, scsi.cdrev= change it). Its logical blocks are 512
 *   bytes at power on and after every reset, as on Apple's drives: the ROM reads block 0 and a CD's driver
 *   partition in 512-byte blocks, so a CD can be the startup disk; MODE SELECT's block descriptor switches it to
 *   2048 (what Apple's driver and A/UX do) and back. scsi.cdblock=2048 starts at 2048 (a drive without Apple's
 *   firmware). Commands: TEST UNIT READY, REQUEST SENSE, INQUIRY, READ CAPACITY, READ(6), READ(10), READ(12),
 *   MODE SENSE(6) and (10) (pages 1, 2, $0D, $0E and Apple's $30), MODE SELECT(6) and (10), READ TOC (formats 0
 *   and 1, LBA and MSF), READ HEADER, READ SUB-CHANNEL, START STOP UNIT (eject), PREVENT ALLOW MEDIUM REMOVAL,
 *   SEEK, REZERO, VERIFY, RESERVE, RELEASE, SEND DIAGNOSTIC, and Apple's own $C0 (eject), $C1 (READ TOC, in BCD),
 *   $C2, $CC (audio status) and the audio controls, as MAME's Apple CD-ROM answers them; the audio play commands
 *   answer ILLEGAL MODE FOR THIS TRACK (a data CD); anything else CHECK CONDITION, ILLEGAL REQUEST, logged once
 *   per command.
 *   Hard disk (type 0), from an image file, written back: READ and WRITE (6) and (10), WRITE AND VERIFY,
 *   VERIFY, READ CAPACITY, MODE SENSE (pages 1, 2, 3, 4, 8, $0A and Apple's $30, which HD SC Setup looks for),
 *   MODE SELECT, FORMAT UNIT (no work: an image has no defects), REASSIGN BLOCKS, READ DEFECT DATA, SYNCHRONIZE
 *   CACHE, SEEK, REZERO, START STOP, RESERVE, RELEASE, SEND and RECEIVE DIAGNOSTIC, READ and WRITE BUFFER, and
 *   Iomega's "non-sense" $06 for the Zip driver on the release's images (item 52).
 *   The image is in memory (the solo board's, item 54), a host file through semihosting (the testbed: a name
 *   with a '/'), or served from the card as the Mac asks through the card's cache (psmac_card.c, item 142):
 *   never read whole, so a 650 MB CD costs the Pi a few cluster lists.
 *
 * Options: scsi.cd=NAME[:ID] (ID 3 by default; NAME "empty" for a drive with no CD; more drives with more of them),
 * scsi.disk=NAME:ID (any number of them, or several separated by commas), scsi.cdvendor=, scsi.cdproduct=, scsi.cdrev=, scsi.cdblock=,
 * scsi.hdvendor=, scsi.hdproduct=, scsi.probe=off, scsi.trace=N (the first N commands in the log; mac.solotrace=
 * as before on the solo board). The CD is changed while the Mac runs through the remote control (POST /scsi/cd,
 * tools/aurora68.py cd NAME | --eject): the old CD goes, the drive is NOT READY until the new one is in, then
 * UNIT ATTENTION (medium changed), as a drive with a new disc.
 *
 * Everything here runs on core 0 (the 68k's accesses), except psmac_scsidev_card_* (core 3, the card) and the
 * request and JSON calls (the remote control's core): those touch only the fields named there, published with
 * release stores.
 */
#ifdef PSMAC_SCSIDEV_HOST
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
void kprintf(const char *fmt, ...);
uint64_t psmac_scsidev_host_ticks(void);           /* the native test's clock: 1 MHz */
#else
#include "support.h"
#include "psmac.h"
#endif
#include "psmac_scsidev.h"

/* the kernel's side (stubbed by tools/scsidev_test.c) */
int64_t psmac_sh_open(const char *name, uint64_t mode);
int     psmac_sh_read(int64_t fd, void *buf, uint64_t len);
int     psmac_sh_write(int64_t fd, const void *buf, uint64_t len);
int64_t psmac_sh_seek(int64_t fd, uint64_t pos);
int64_t psmac_sh_flen(int64_t fd);
int     psmac_sh_available(void);
int     psmac_card_need(const void *p, uint32_t len);
void    psmac_card_dirty(const void *p, uint32_t len);
void    psmac_card_scan_wait(void);
const char *psmac_option_nth(const char *key, int n);
extern uint32_t psmac_icache_epoch;

#ifdef PSMAC_SCSIDEV_HOST
static uint64_t ticks(void) { return psmac_scsidev_host_ticks(); }
static uint64_t ticks_hz(void) { return 1000000u; }
#define BOOT_MARK(...) do { } while (0)
#define SPLASH(...) do { } while (0)
#define ALERT(...) do { } while (0)
#else
static uint64_t ticks(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static uint64_t ticks_hz(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
#define BOOT_MARK(...) psmac_boot_mark(__VA_ARGS__)
#define SPLASH(...) psmac_splash_status(__VA_ARGS__)
#define ALERT(...) psmac_splash_alert(0, __VA_ARGS__)
#endif

#define NID 8                                   /* SCSI IDs 0-7; 7 is the Mac's */
#define PROBE_MS 20u                            /* the real bus's answer to a selection: a target asserts BSY within
                                                   microseconds; 20 ms and at least PROBE_READS reads (the testbed's
                                                   MAME bus advances only as it is read) */
#define PROBE_READS 2000u

/* ==== the devices ============================================================================ */
struct sdev {
    volatile int present;                       /* a target answers at this ID */
    int type;                                   /* PSMAC_SCSI_TYPE_DISK or _CDROM */
    int cfg;                                    /* from scsi.cd= or scsi.disk= (else one of the solo board's images) */
    char name[128];                             /* the image file (a host path in the testbed can be long) */
    uint8_t *ram; int64_t fd; int card;         /* in memory; a host file; served from the card (handle + 1) */
    uint64_t bytes;                             /* the image's size */
    int writable;
    volatile int want_card, waiting;            /* the card is to find name; it has not answered yet */
    volatile int medium;                        /* a CD in the drive */
    volatile int ua;                            /* UNIT ATTENTION waits: 1 medium changed */
    int prevent;                                /* PREVENT MEDIUM REMOVAL */
    uint32_t bsize, def_bsize;                  /* the logical block size, now and at reset */
    char vendor[9], product[17], rev[5];
    uint8_t key, asc, ascq;                     /* the sense data REQUEST SENSE returns next */
    uint32_t info; int info_ok;
    int disabled, probed;                       /* a real device answers this ID; the check has been made */
    uint32_t cmds, reads, writes, sel, unknown; /* reads and writes in 512-byte sectors */
    uint8_t op_said[32];                        /* unsupported commands already logged */
    volatile int change;                        /* a new CD asked for (the remote control) */
    char change_name[128];
};
static struct sdev dev[NID];
static int solo_board, probe_off, inited, any_cfg;
static uint32_t trace_left;
static uint32_t sel_seen[NID];
#define TRACE(...) do { if (trace_left) { trace_left--; kprintf("[SCSI] " __VA_ARGS__); } } while (0)

static void scopy(char *d, const char *s, unsigned max) { unsigned k = 0; while (s && s[k] && k + 1 < max) { d[k] = s[k]; k++; } d[k] = 0; }
/* an INQUIRY name, padded with spaces; '_' is a space (cmdline.txt's words cannot hold one: scsi.cdproduct=CD-ROM_CDU-8003A) */
static void spad(char *d, const char *s, unsigned n) { unsigned k = 0; while (s && s[k] && k < n) { d[k] = s[k] == '_' ? ' ' : s[k]; k++; } while (k < n) d[k++] = ' '; d[n] = 0; }
static const char *kind(const struct sdev *d) { return d->type == PSMAC_SCSI_TYPE_CDROM ? "CD-ROM" : "disk"; }
static uint32_t num(const char *s) { uint32_t v = 0; while (s && *s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0'); return v; }

int psmac_scsidev_id_taken(int id) { return id >= 0 && id < NID && dev[id].cfg; }
int psmac_scsidev_configured(void) { for (int i = 0; i < NID; i++) if (dev[i].cfg) return 1; return 0; }
int psmac_scsidev_cd_id(void) { for (int i = 0; i < NID; i++) if (dev[i].cfg && dev[i].type == PSMAC_SCSI_TYPE_CDROM) return i; return -1; }

/* one NAME[:ID] word; the ID is the digits after the last ':' */
static void parse_word(const char *w, unsigned len, int type, int def_id)
{
    char nm[128]; unsigned n = len < sizeof nm - 1 ? len : sizeof nm - 1;
    for (unsigned i = 0; i < n; i++) nm[i] = w[i];
    nm[n] = 0;
    int id = def_id;
    for (int i = (int)n - 1; i > 0; i--)
        if (nm[i] == ':' && nm[i + 1] >= '0' && nm[i + 1] <= '9') { id = (int)num(nm + i + 1); nm[i] = 0; break; }
    const char *what = type == PSMAC_SCSI_TYPE_CDROM ? "scsi.cd" : "scsi.disk";
    if (id < 0) { kprintf("[SCSI] %s=%s: which SCSI ID? (NAME:ID, 0 to 6)\n", what, nm); return; }
    if (id > 6) { kprintf("[SCSI] %s=%s:%d: ID %d is not free (7 is the Mac's): not used\n", what, nm, id, id); return; }
    if (!nm[0]) { kprintf("[SCSI] %s: no file named\n", what); return; }
    struct sdev *d = &dev[id];
    if (d->cfg) { kprintf("[SCSI] %s=%s:%d: ID %d already holds %s: not used\n", what, nm, id, id, d->name); return; }
    d->cfg = 1; d->type = type; d->fd = -1; d->ram = 0; d->card = 0;
    scopy(d->name, nm, sizeof d->name);
    if (type == PSMAC_SCSI_TYPE_CDROM) {
        uint32_t bs = num(psmac_option("scsi.cdblock"));
        d->def_bsize = bs == 2048 ? 2048 : 512;
        const char *v = psmac_option("scsi.cdvendor"), *p = psmac_option("scsi.cdproduct"), *r = psmac_option("scsi.cdrev");
        spad(d->vendor, v ? v : "SONY", 8); spad(d->product, p ? p : "CD-ROM CDU-8003A", 16); spad(d->rev, r ? r : "1.9a", 4);
        d->present = 1;                         /* the drive is there even before (or without) its CD */
    } else {
        d->def_bsize = 512;
        const char *v = psmac_option("scsi.hdvendor"), *p = psmac_option("scsi.hdproduct");
        spad(d->vendor, v ? v : "AURORA68", 8); spad(d->product, p ? p : "Virtual Disk", 16); spad(d->rev, "1.0", 4);
    }
    d->bsize = d->def_bsize;
    int empty = type == PSMAC_SCSI_TYPE_CDROM && (!nm[0] || (nm[0] == 'e' && nm[1] == 'm' && nm[2] == 'p' && nm[3] == 't' && nm[4] == 'y' && !nm[5]));
    if (empty) { d->name[0] = 0; kprintf("[SCSI] ID %d: a CD-ROM drive with no CD (scsi.cd=empty)\n", id); return; }
    int host = 0; for (const char *s = nm; *s; s++) if (*s == '/') host = 1;
    if (host && psmac_sh_available()) {         /* the testbed: a host file through semihosting */
        int64_t fd = type == PSMAC_SCSI_TYPE_DISK ? psmac_sh_open(nm, 3) : -1;
        d->writable = fd >= 0;
        if (fd < 0) fd = psmac_sh_open(nm, 1);
        if (fd < 0) { kprintf("[SCSI] ID %d: cannot open the host file %s\n", id, nm); if (type == PSMAC_SCSI_TYPE_DISK) d->cfg = 0; return; }
        d->fd = fd; d->bytes = (uint64_t)psmac_sh_flen(fd);
        if (type == PSMAC_SCSI_TYPE_CDROM) { d->writable = 0; d->medium = 1; }
        __atomic_store_n(&d->present, 1, __ATOMIC_RELEASE);
        kprintf("[SCSI] ID %d: %s %s, a host file, %u MB%s\n", id, kind(d), nm, (uint32_t)(d->bytes >> 20), d->writable ? ", written back" : ", read only");
        return;
    }
    d->want_card = 1; d->waiting = 1;           /* psmac_card.c finds it at its first look */
    kprintf("[SCSI] ID %d: %s %s, from the card when it has been looked at\n", id, kind(d), nm);
}
static void parse_list(const char *v, int type, int def_id)
{
    while (v && *v) {
        const char *e = v; while (*e && *e != ',') e++;
        if (e > v) parse_word(v, (unsigned)(e - v), type, def_id);
        v = *e ? e + 1 : e;
    }
}
void psmac_scsidev_init(int solo)
{
    if (inited) return;
    inited = 1;
    solo_board = solo;
    for (int i = 0; i < NID; i++) { dev[i].fd = -1; dev[i].bsize = dev[i].def_bsize = 512; }
    const char *o = psmac_option("scsi.probe");
    probe_off = o && o[0] == 'o' && o[1] == 'f';
    trace_left = num(psmac_option("scsi.trace"));
    if (solo) trace_left += num(psmac_option("mac.solotrace"));
    for (int k = 0; k < 8 && (o = psmac_option_nth("scsi.cd", k)); k++) parse_list(o, PSMAC_SCSI_TYPE_CDROM, 3);
    for (int k = 0; k < 16 && (o = psmac_option_nth("scsi.disk", k)); k++) parse_list(o, PSMAC_SCSI_TYPE_DISK, -1);
    psmac_scsidev_reset();                      /* the chip and the target as at power on */
    any_cfg = psmac_scsidev_configured();
}
void psmac_scsidev_add_image(int id, const char *name, uint8_t *ram, int64_t fd, int card, uint64_t bytes, int writable)
{
    if (id < 0 || id > 6) return;
    struct sdev *d = &dev[id];
    if (d->cfg) { kprintf("[SCSI] the solo board's disk image %s would be ID %d, which %s holds: not a SCSI disk this session\n", name, id, d->name[0] ? d->name : "the CD-ROM drive"); return; }
    scopy(d->name, name, sizeof d->name);
    d->type = PSMAC_SCSI_TYPE_DISK; d->ram = ram; d->fd = fd; d->card = card; d->bytes = bytes; d->writable = writable;
    d->bsize = d->def_bsize = 512;
    spad(d->vendor, "AURORA68", 8); spad(d->product, "PiDisk SCSI", 16); spad(d->rev, "0.4", 4);
    __atomic_store_n(&d->present, 1, __ATOMIC_RELEASE);   /* complete before it is seen: a card image arrives from core 3 */
}

/* ---- the card (core 3) ---- */
const char *psmac_scsidev_card_want(int i, int *writable)
{
    if (i < 0 || i >= NID || !dev[i].cfg || !dev[i].want_card) return 0;
    *writable = dev[i].type == PSMAC_SCSI_TYPE_DISK;
    return dev[i].name;
}
void psmac_scsidev_card_ready(int i, int handle, uint64_t bytes, int writable)
{
    if (i < 0 || i >= NID) return;
    struct sdev *d = &dev[i];
    d->card = handle + 1; d->bytes = bytes; d->ram = 0; d->fd = -1;
    d->writable = d->type == PSMAC_SCSI_TYPE_DISK && writable;
    if (d->type == PSMAC_SCSI_TYPE_CDROM) {
        int had = d->waiting == 0;              /* a change, not the first look */
        __atomic_store_n(&d->medium, 1, __ATOMIC_RELEASE);
        if (had) __atomic_store_n(&d->ua, 1, __ATOMIC_RELEASE);
    }
    d->want_card = 0;
    __atomic_store_n(&d->present, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&d->waiting, 0, __ATOMIC_RELEASE);
    kprintf("[SCSI] ID %d: %s %s, %u MB, served from the card%s%s\n", i, kind(d), d->name, (uint32_t)(bytes >> 20),
            d->type == PSMAC_SCSI_TYPE_CDROM ? "" : d->writable ? ", written back" : ", locked",
            d->type == PSMAC_SCSI_TYPE_DISK && (bytes & 511) ? " (its size is not a multiple of 512: the last bytes are not used)" : "");
}
void psmac_scsidev_card_missing(int i)
{
    if (i < 0 || i >= NID) return;
    struct sdev *d = &dev[i];
    d->want_card = 0;
    if (d->type == PSMAC_SCSI_TYPE_CDROM) kprintf("[SCSI] ID %d: no file %s in the card's root: the CD-ROM drive has no CD\n", i, d->name);
    else { kprintf("[SCSI] ID %d: no file %s in the card's root: no disk at ID %d (make one with tools/mac/mkblank.py)\n", i, d->name, i); d->cfg = 0; }
    __atomic_store_n(&d->waiting, 0, __ATOMIC_RELEASE);
}
int psmac_scsidev_card_change(int *i, char *name, unsigned max)
{
    for (int k = 0; k < NID; k++)
        if (__atomic_load_n(&dev[k].change, __ATOMIC_ACQUIRE)) {
            scopy(name, dev[k].change_name, max);
            *i = k;
            __atomic_store_n(&dev[k].change, 0, __ATOMIC_RELEASE);
            return 1;
        }
    return 0;
}
int psmac_scsidev_cd_request(const char *name, int id, int force)
{
    if (id < 0) for (int k = 0; k < NID && id < 0; k++) if (dev[k].cfg && dev[k].type == PSMAC_SCSI_TYPE_CDROM) id = k;
    if (id < 0 || id >= NID || !dev[id].cfg || dev[id].type != PSMAC_SCSI_TYPE_CDROM) return -1;   /* no CD-ROM drive */
    struct sdev *d = &dev[id];
    if (d->prevent && d->medium && !force) return -2;   /* the Mac holds the CD (PREVENT MEDIUM REMOVAL) */
    if (d->prevent && d->medium) kprintf("[SCSI] ID %d: the CD taken out although the Mac holds it (force, the paperclip)\n", id);
    __atomic_store_n(&d->medium, 0, __ATOMIC_RELEASE);   /* the old CD out first */
    d->waiting = 0;
    if (!name || !name[0]) { kprintf("[SCSI] ID %d: the CD ejected (remote control)\n", id); d->name[0] = 0; return 0; }
    int host = 0; for (const char *s = name; *s; s++) if (*s == '/') host = 1;
    if (host && psmac_sh_available()) {
        int64_t fd = psmac_sh_open(name, 1);
        if (fd < 0) { kprintf("[SCSI] ID %d: cannot open the host file %s\n", id, name); return -3; }
        scopy(d->name, name, sizeof d->name);
        d->card = 0; d->ram = 0; d->fd = fd; d->bytes = (uint64_t)psmac_sh_flen(fd);
        __atomic_store_n(&d->medium, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&d->ua, 1, __ATOMIC_RELEASE);
        kprintf("[SCSI] ID %d: CD %s in the drive (a host file, %u MB)\n", id, name, (uint32_t)(d->bytes >> 20));
        return 0;
    }
    scopy(d->change_name, name, sizeof d->change_name);
    scopy(d->name, name, sizeof d->name);
    __atomic_store_n(&d->change, 1, __ATOMIC_RELEASE);   /* psmac_card.c's next pass */
    kprintf("[SCSI] ID %d: CD %s asked for (remote control): from the card at its next pass\n", id, name);
    return 0;
}

/* ---- the images' sectors ---- */
static uint8_t sector_buf[64 * 1024] __attribute__((aligned(64)));
static int store_io(struct sdev *d, int write, uint64_t sec, uint32_t n, uint8_t *buf)
{
    if (write) d->writes += n; else d->reads += n;
    if (d->ram) {
        uint8_t *p = d->ram + sec * 512u;
        if (write) { if (p != buf) memcpy(p, buf, n * 512u); psmac_card_dirty(p, n * 512u); }
        else memcpy(buf, p, n * 512u);
        return 1;
    }
    if (d->card) return psmac_card_img_io(d->card - 1, write, (uint32_t)sec, n, buf, 1) == (int)n;
    if (d->fd < 0 || psmac_sh_seek(d->fd, sec * 512u) < 0) return 0;
    return write ? psmac_sh_write(d->fd, buf, n * 512u) : psmac_sh_read(d->fd, buf, n * 512u);
}

/* ==== the 5380 and the target behind it ======================================================= */
enum { PH_DATAOUT = 0, PH_DATAIN = 1, PH_CMD = 2, PH_STATUS = 3, PH_MSGOUT = 6, PH_MSGIN = 7, PH_FREE = 8 };
enum { DOUT_STORE, DOUT_MODESEL6, DOUT_MODESEL10, DOUT_FORMAT, DOUT_REASSIGN, DOUT_DISCARD };
static struct {
    uint8_t odr, icr, mr, tcr, ser;
    int dma, irq, busy_err, aip;
    int latched;                                /* Linux's last byte of a DMA receive read from the input register (below) */
} n;
static struct {
    int phase, req;
    struct sdev *d; int id;
    uint8_t lun; int identified, abort;         /* IDENTIFY's LUN; ABORT (1) or BUS DEVICE RESET (2) came */
    uint8_t cdb[16]; int cdb_n, cdb_len;
    uint8_t *buf; uint32_t buf_len, buf_pos;    /* the data phase's current chunk */
    uint32_t left; uint64_t sec;                /* bytes still to move after this chunk, the store's next sector */
    int chunked, dout;                          /* chunks through sector_buf; what a data out phase's bytes are for */
    uint32_t par_len;                           /* the bytes of a parameter list kept in small (the rest dropped) */
    uint8_t *wr_ram; uint32_t wr_len;
    uint8_t status, msgin;
    uint8_t small[512];
    uint32_t cmds, mismatches, underflows;
    struct { uint8_t id, cdb[10], status; uint32_t n; } ring[12]; uint32_t ring_n;
} t;
static int bus;                                 /* where the Mac's accesses go (below) */
enum { BUS_SOLO, BUS_REAL, BUS_LOCAL, BUS_EMPTY };
static const struct psmac_scsi_wire *wire;
static uint32_t diverted, returned, probes;
static const char *phase_name(int p) { static const char *const nm[] = { "data out", "data in", "command", "status", "?", "?", "message out", "message in", "free" }; return nm[p & 15]; }

static void target_free(void)
{
    if (t.phase != PH_FREE && n.dma) { n.irq = 1; n.dma = 0; t.mismatches++; }
    t.phase = PH_FREE; t.req = 0; t.d = 0;
    if (n.mr & 0x04) { n.busy_err = 1; n.irq = 1; }
}
static void set_phase(int p)
{
    if (p == PH_STATUS && t.ring_n) t.ring[(t.ring_n - 1) % 12].status = t.status;
    if (t.phase == PH_DATAIN && p == PH_STATUS) {
        /* A read has put new bytes in the Mac's memory, perhaps code over code the JIT has translated. The bus
           layer tells the translator so for every SCSI read that goes over the wire (psmac_bus.c,
           psmac_icache_epoch: RAM blocks are checked again on their next entry); this chip never goes over the
           wire, so it says so itself, once per transfer (docs/08 item 53). */
        psmac_icache_epoch++;
    }
    if (p != t.phase && n.dma) { n.irq = 1; n.dma = 0; t.mismatches++; }   /* a phase change ends a DMA transfer with an interrupt */
    t.phase = p; t.req = 1;
}
static void sense(struct sdev *d, uint8_t key, uint8_t asc, uint8_t ascq) { d->key = key; d->asc = asc; d->ascq = ascq; d->info_ok = 0; }
static void check(uint8_t key, uint8_t asc) { t.status = 2; sense(t.d, key, asc, 0); set_phase(PH_STATUS); }
static void good(void) { set_phase(PH_STATUS); }
/* the small answer in t.small, at most alloc bytes (alloc 0: all of it, as the solo board always took a 6-byte
   command's 0, unless zero_none, for the 10-byte commands whose 0 means none) */
static void data_in(uint32_t len, uint32_t alloc, int zero_none)
{
    if (alloc || zero_none) { if (alloc < len) len = alloc; }
    if (len > sizeof t.small) len = sizeof t.small;
    t.buf = t.small; t.buf_len = len; t.buf_pos = 0; t.left = 0; t.chunked = 0;
    set_phase(len ? PH_DATAIN : PH_STATUS);
}
/* a parameter list to come (MODE SELECT and the like): into t.small, anything beyond it dropped */
static void data_out(uint32_t len, int what)
{
    if (!len) { good(); return; }
    uint32_t k = len > sizeof t.small ? sizeof t.small : len;
    t.buf = t.small; t.buf_len = k; t.buf_pos = 0; t.left = len - k; t.chunked = 0; t.dout = what; t.par_len = k;
    set_phase(PH_DATAOUT);
}
static void next_chunk(void)
{
    uint32_t nb = t.left / 512; if (nb > sizeof sector_buf / 512) nb = sizeof sector_buf / 512;
    if (!store_io(t.d, 0, t.sec, nb, sector_buf)) { check(3, 0x11); return; }
    t.buf = sector_buf; t.buf_len = nb * 512; t.buf_pos = 0; t.left -= nb * 512; t.sec += nb;
}

/* ---- the commands ---- */
static uint32_t be(const uint8_t *p, int n) { uint32_t v = 0; for (int i = 0; i < n; i++) v = v << 8 | p[i]; return v; }
static void put(uint8_t *p, uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }
static uint32_t blocks_of(const struct sdev *d) { uint64_t b = d->bytes / d->bsize; return b > 0xffffffffu ? 0xffffffffu : (uint32_t)b; }
static uint32_t frames_of(const struct sdev *d) { return (uint32_t)(d->bytes / 2048u); }
static uint8_t bcd(uint8_t v) { return (uint8_t)(v / 10 << 4 | v % 10); }
static void msf(uint8_t *p, uint32_t frame) { frame += 150; p[0] = 0; p[1] = (uint8_t)(frame / (75 * 60)); p[2] = (uint8_t)((frame / 75) % 60); p[3] = (uint8_t)(frame % 75); }

/* the mode pages, page 0x3f for all; the length, or -1 for a page this device does not have */
static int mode_pages(const struct sdev *d, int page, int pc, uint8_t *p)
{
    int k = 0, any = 0;
    int cd = d->type == PSMAC_SCSI_TYPE_CDROM, ch = pc == 1;   /* changeable values: none */
#define PAGE(code, len) if (page == (code) || page == 0x3f) { for (int i = 0; i < (len) + 2; i++) p[k + i] = 0; p[k] = (code); p[k + 1] = (len); any = 1;
#define END(len) k += (len) + 2; }
    PAGE(0x01, cd ? 6 : 10) if (!ch) p[k + 3] = 1; END(cd ? 6 : 10)                       /* error recovery: one retry */
    PAGE(0x02, 14) END(14)                                                                  /* disconnect-reconnect: never disconnects */
    if (!cd) {
        PAGE(0x03, 22) if (!ch) { p[k + 11] = 32; p[k + 12] = 2; p[k + 20] = 0x40; } END(22)   /* format: 32 sectors a track, 512 bytes (the solo board's) */
        PAGE(0x04, 22) if (!ch) { uint32_t cyl = (uint32_t)(d->bytes / 512u / (16 * 32)); put(p + k + 2, cyl, 3); p[k + 5] = 16; p[k + 20] = 0x1c; p[k + 21] = 0x20; } END(22)   /* geometry: 16 heads, 7200 rpm */
        PAGE(0x08, 18) END(18)                                                              /* caching: none */
        PAGE(0x0a, 10) END(10)                                                              /* control */
    } else {
        PAGE(0x0d, 6) if (!ch) { p[k + 5] = 60; p[k + 7] = 75; } END(6)                    /* CD-ROM: 60 S per M, 75 F per S */
        PAGE(0x0e, 14) if (!ch) { p[k + 2] = 0x04; p[k + 8] = 1; p[k + 9] = 0xff; p[k + 10] = 2; p[k + 11] = 0xff; } END(14)   /* audio control: both channels, full volume */
    }
    /* Apple's page, which HD SC Setup asks a disk for before it will touch it, and Apple's drives (CD-ROM ones too) carry:
       $B0 (saveable), 22 bytes, "APPLE COMPUTER, INC" and three spaces, as MAME's hard disk and CD-ROM answer it
       (until 25 November 2026 the solo board answered "APPLE COMPUTER, INC." with a full stop and page code $30) */
    PAGE(0x30, 22) p[k] = 0xb0; if (!ch) { static const char apple[] = "APPLE COMPUTER, INC   "; for (int i = 0; i < 22; i++) p[k + 2 + i] = (uint8_t)apple[i]; } END(22)
#undef PAGE
#undef END
    return any || page == 0 ? k : -1;
}
static void mode_sense(int ten)
{
    struct sdev *d = t.d;
    int page = t.cdb[2] & 0x3f, pc = t.cdb[2] >> 6, dbd = t.cdb[1] & 0x08;
    uint32_t alloc = ten ? be(t.cdb + 7, 2) : t.cdb[4];
    int hl = ten ? 8 : 4, bdl = dbd ? 0 : 8;
    for (unsigned i = 0; i < sizeof t.small; i++) t.small[i] = 0;
    int pl = mode_pages(d, page, pc, t.small + hl + bdl);
    if (pl < 0) { kprintf("[SCSI] ID %d (%s): MODE SENSE of page $%02x, which it does not have: CHECK CONDITION, ILLEGAL REQUEST\n", t.id, kind(d), page); check(5, 0x24); return; }
    uint8_t *h = t.small;
    uint8_t medium = d->type == PSMAC_SCSI_TYPE_CDROM ? (d->medium ? 0x01 : 0x70) : 0;   /* a 120 mm data CD; no disc */
    uint8_t spec = d->type == PSMAC_SCSI_TYPE_CDROM || !d->writable ? 0x80 : 0;              /* write protected (a CD always: see cd_readonly) */
    if (ten) { h[2] = medium; h[3] = spec; h[7] = (uint8_t)bdl; }
    else { h[1] = medium; h[2] = spec; h[3] = (uint8_t)bdl; }
    if (bdl) {
        uint8_t *b = t.small + hl;
        uint32_t nb = d->medium || d->type == PSMAC_SCSI_TYPE_DISK ? blocks_of(d) : 0;
        put(b + 1, nb > 0xffffff ? 0 : nb, 3);
        put(b + 5, pc == 1 ? 0 : d->bsize, 3);
    }
    uint32_t total = (uint32_t)(hl + bdl + pl);
    if (ten) put(h, total - 2, 2); else h[0] = (uint8_t)(total - 1);
    data_in(total, alloc, ten);
}
/* MODE SELECT's parameter list: the block descriptor's length is the one thing used */
static void mode_select_done(int ten)
{
    struct sdev *d = t.d;
    uint32_t got = t.par_len, hl = ten ? 8 : 4;
    uint32_t bdl = got >= hl ? (ten ? be(t.small + 6, 2) : t.small[3]) : 0;
    if (bdl >= 8 && got >= hl + 8) {
        uint32_t bs = be(t.small + hl + 5, 3);
        if (bs && bs != d->bsize) {
            int ok = bs == 512 || (bs == 2048 && d->type == PSMAC_SCSI_TYPE_CDROM);
            if (!ok) { kprintf("[SCSI] ID %d (%s): MODE SELECT asks for %u-byte blocks: CHECK CONDITION (512%s only)\n", t.id, kind(d), bs, d->type == PSMAC_SCSI_TYPE_CDROM ? " and 2048" : ""); check(5, 0x26); return; }
            kprintf("[SCSI] ID %d (%s): logical blocks of %u bytes (MODE SELECT; they were %u)\n", t.id, kind(d), bs, d->bsize);
            d->bsize = bs;
        }
    }
    good();
}
static void unsupported(const char *why)
{
    struct sdev *d = t.d;
    uint8_t op = t.cdb[0];
    d->unknown++;
    if (!(d->op_said[op >> 3] & (1u << (op & 7)))) {
        d->op_said[op >> 3] |= (uint8_t)(1u << (op & 7));
        kprintf("[SCSI] ID %d (%s): command $%02x (%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x) %s: CHECK CONDITION, ILLEGAL REQUEST\n",
                t.id, kind(d), op, t.cdb[0], t.cdb[1], t.cdb[2], t.cdb[3], t.cdb[4], t.cdb[5], t.cdb[6], t.cdb[7], t.cdb[8], t.cdb[9], why);
    }
    check(5, 0x20);
}
/* A write to the CD. The SCSI-2 CD-ROM has no write commands (ILLEGAL REQUEST), but a driver that does not know CDs
   takes that for a passing error. In the testbed's System 7.1 the Iomega driver (patched to take any disk, docs/08
   item 52; the release's images carry it too) found the HFS test CD, a removable drive with a partition map, mounted
   it as a writable volume and retried the write of its master directory block for ever, and the Mac went no further
   (25 November 2026). So the CD is write-protected wherever such a driver looks: DATA PROTECT for a write (the
   File Manager's "locked disk"), the write-protect bit in MODE SENSE, and Iomega's vendor command $06 answered as a
   write-protected Zip disk. Apple's CD-ROM driver and A/UX never write to a CD. */
static void cd_readonly(void)
{
    struct sdev *d = t.d;
    static int said;
    if (!said) { said = 1; kprintf("[SCSI] ID %d (CD-ROM): a write (command $%02x): DATA PROTECT, the CD is read only (a driver that is not a CD driver?)\n", t.id, t.cdb[0]); }
    d->unknown++;
    t.status = 2; sense(d, 7, 0x27, 0); set_phase(PH_STATUS);
}
static void transfer(uint32_t lba, uint32_t cnt, int write)
{
    struct sdev *d = t.d;
    if (d->type == PSMAC_SCSI_TYPE_CDROM && write) { cd_readonly(); return; }
    if ((uint64_t)lba + cnt > blocks_of(d)) { check(5, 0x21); return; }
    if (!cnt) { good(); return; }
    if (write && !d->writable) { check(7, 0x27); return; }
    uint32_t spb = d->bsize / 512u;
    uint64_t sec = (uint64_t)lba * spb; uint32_t nsec = cnt * spb;
    { static int first = 1; if (first && !write) { first = 0; BOOT_MARK("SCSI: first read, ID %d block %u (%u blocks): the disk's driver or boot blocks", t.id, lba, cnt); SPLASH("Starting from the disk..."); } }
    if (d->ram) {                               /* an image in memory, perhaps still coming in from the card: blocks it did not give are a medium error */
        if (psmac_card_need(d->ram + sec * 512u, nsec * 512u)) { check(3, write ? 0x0c : 0x11); return; }
        t.buf = d->ram + sec * 512u; t.buf_len = nsec * 512u; t.buf_pos = 0; t.left = 0; t.chunked = 0;
        if (write) { d->writes += nsec; t.wr_ram = t.buf; t.wr_len = nsec * 512u; } else d->reads += nsec;
        t.dout = DOUT_STORE;
        set_phase(write ? PH_DATAOUT : PH_DATAIN);
        return;
    }
    t.left = nsec * 512u; t.sec = sec; t.chunked = 1; t.dout = DOUT_STORE;
    if (write) {
        uint32_t nb = nsec > sizeof sector_buf / 512 ? (uint32_t)(sizeof sector_buf / 512) : nsec;
        t.buf = sector_buf; t.buf_len = nb * 512; t.buf_pos = 0; t.left -= nb * 512;
        set_phase(PH_DATAOUT);
        return;
    }
    next_chunk();
    if (t.phase != PH_STATUS) set_phase(PH_DATAIN);
}
/* the commands every device takes the same way, 1 when op was one of them */
static int common(uint8_t op)
{
    struct sdev *d = t.d;
    switch (op) {
    case 0x03: {                                /* REQUEST SENSE */
        t.small[0] = 0x70; t.small[2] = d->key; t.small[7] = 10; t.small[12] = d->asc; t.small[13] = d->ascq;
        if (d->info_ok) { t.small[0] |= 0x80; put(t.small + 3, d->info, 4); }
        if (!d->key && d->type == PSMAC_SCSI_TYPE_CDROM && !d->medium) { t.small[2] = 2; t.small[12] = 0x3a; }   /* still no CD */
        sense(d, 0, 0, 0);
        data_in(18, t.cdb[4], 0); return 1; }
    case 0x12: {                                /* INQUIRY */
        int lun = t.identified ? t.lun : t.cdb[1] >> 5;
        if (t.cdb[1] & 1) {                     /* vital product data: page 0, the list of pages, which is itself */
            if (t.cdb[2]) { check(5, 0x24); return 1; }
            t.small[0] = (uint8_t)d->type; t.small[3] = 1; t.small[4] = 0;
            data_in(5, t.cdb[4], 0); return 1;
        }
        t.small[0] = lun ? 0x7f : (uint8_t)d->type;
        t.small[1] = d->type == PSMAC_SCSI_TYPE_CDROM ? 0x80 : 0;   /* removable */
        t.small[2] = 0x02; t.small[3] = 0x02; t.small[4] = 31;
        for (int i = 0; i < 8; i++) t.small[8 + i] = (uint8_t)d->vendor[i];
        for (int i = 0; i < 16; i++) t.small[16 + i] = (uint8_t)d->product[i];
        for (int i = 0; i < 4; i++) t.small[32 + i] = (uint8_t)d->rev[i];
        data_in(36, t.cdb[4], 0); return 1; }
    case 0x1a: mode_sense(0); return 1;
    case 0x5a: mode_sense(1); return 1;
    case 0x15: data_out(t.cdb[4], DOUT_MODESEL6); return 1;
    case 0x55: data_out(be(t.cdb + 7, 2), DOUT_MODESEL10); return 1;
    case 0x16: case 0x17: good(); return 1;     /* RESERVE, RELEASE */
    case 0x1d: data_out(be(t.cdb + 3, 2), DOUT_DISCARD); return 1;   /* SEND DIAGNOSTIC: the self-test passes */
    case 0x1e: d->prevent = t.cdb[4] & 1; good(); return 1;           /* PREVENT ALLOW MEDIUM REMOVAL */
    }
    return 0;
}
static int needs_medium(uint8_t op)
{
    switch (op) {
    case 0x00: case 0x01: case 0x08: case 0x0b: case 0x25: case 0x28: case 0xa8: case 0x2b: case 0x2f: case 0x42: case 0x43: case 0x44:
        return 1;
    }
    return 0;
}
static void cdrom(uint8_t op)
{
    struct sdev *d = t.d;
    switch (op) {
    case 0x00: case 0x01: case 0x0b: case 0x2b: case 0x2f: good(); return;   /* TEST UNIT READY, REZERO, SEEK(6), SEEK(10), VERIFY */
    case 0x08: transfer(be(t.cdb + 1, 3) & 0x1fffff, t.cdb[4] ? t.cdb[4] : 256, 0); return;
    case 0x28: transfer(be(t.cdb + 2, 4), be(t.cdb + 7, 2), 0); return;
    case 0xa8: transfer(be(t.cdb + 2, 4), be(t.cdb + 6, 4), 0); return;
    case 0x0a: case 0x2a: case 0xaa: case 0x2e: case 0x04: cd_readonly(); return;
    case 0x06: {                                /* Iomega's "non-sense": a Zip disk that is write-protected (cd_readonly) */
        t.small[21] = 2;
        data_in(t.cdb[4] ? t.cdb[4] : 255, 0, 0); return; }
    case 0x25: {                                /* READ CAPACITY */
        put(t.small, blocks_of(d) - 1, 4); put(t.small + 4, d->bsize, 4);
        data_in(8, 0, 0); return; }
    case 0x1b: {                                /* START STOP UNIT: LoEj with Start 0 ejects, with Start 1 loads */
        int loej = t.cdb[4] & 2, start = t.cdb[4] & 1;
        if (loej && !start) {
            if (d->prevent) { t.status = 2; sense(d, 5, 0x53, 0x02); set_phase(PH_STATUS); return; }   /* medium removal prevented */
            if (d->medium) kprintf("[SCSI] ID %d: the Mac ejects the CD %s (START STOP UNIT); tools/aurora68.py cd NAME puts in another\n", t.id, d->name);
            __atomic_store_n(&d->medium, 0, __ATOMIC_RELEASE);
        } else if (loej && start && !d->medium && (d->card || d->fd >= 0)) {
            __atomic_store_n(&d->medium, 1, __ATOMIC_RELEASE);   /* the tray closes on the same CD */
            kprintf("[SCSI] ID %d: the CD %s loaded again (START STOP UNIT)\n", t.id, d->name);
        }
        good(); return; }
    case 0x43: {                                /* READ TOC: one data track, then the lead-out */
        int m = t.cdb[1] & 2, fmt = t.cdb[2] & 0x0f, track = t.cdb[6];
        if (!fmt) fmt = t.cdb[9] >> 6;          /* SCSI-2's drives kept the format in the control byte's vendor bits */
        uint32_t alloc = be(t.cdb + 7, 2), k = 4;
        if (fmt == 1) {                         /* the sessions: one, starting at track 1 */
            t.small[2] = 1; t.small[3] = 1;
            t.small[5] = 0x14; t.small[6] = 1;
            if (m) msf(t.small + 8, 0);
            k = 12;
        } else if (fmt == 0) {
            if (track > 1 && track != 0xaa) { if (track > 0xaa) { check(5, 0x24); return; } }
            t.small[2] = 1; t.small[3] = 1;
            if (track <= 1) { t.small[k + 1] = 0x14; t.small[k + 2] = 1; if (m) msf(t.small + k + 4, 0); k += 8; }
            t.small[k + 1] = 0x14; t.small[k + 2] = 0xaa;
            if (m) msf(t.small + k + 4, frames_of(d)); else put(t.small + k + 4, frames_of(d), 4);
            k += 8;
        } else { unsupported("(READ TOC of a format other than 0 or 1)"); return; }
        put(t.small, k - 2, 2);
        data_in(k, alloc, 1); return; }
    case 0x44: {                                /* READ HEADER: every sector is mode 1 */
        uint32_t lba = be(t.cdb + 2, 4), frame = (uint32_t)((uint64_t)lba * d->bsize / 2048u);
        t.small[0] = 1;
        if (t.cdb[1] & 2) msf(t.small + 4, frame); else put(t.small + 4, frame, 4);
        data_in(8, be(t.cdb + 7, 2), 1); return; }
    case 0x42: {                                /* READ SUB-CHANNEL: no audio playing, the head on track 1 */
        uint32_t alloc = be(t.cdb + 7, 2), k = 4;
        t.small[1] = 0x15;                      /* no current audio status */
        if (t.cdb[2] & 0x40) {
            int fmt = t.cdb[3];
            if (fmt == 1) { t.small[4] = 1; t.small[5] = 0x14; t.small[6] = 1; t.small[7] = 1; if (t.cdb[1] & 2) { msf(t.small + 8, 0); msf(t.small + 12, 0); } k = 16; }
            else k = 4;
        }
        put(t.small + 2, k - 4, 2);
        data_in(k, alloc, 1); return; }
    /* Apple's own commands, from the AppleCD SC's firmware (Sony's CDU-8002, which Apple's CD-ROM driver also sends to
       later drives it knows): MAME's Apple CD-ROM (src/devices/bus/nscsi/cd.cpp) is the model. Addresses in BCD. */
    case 0xc0:                                  /* APPLE EJECT */
        if (d->prevent) { t.status = 2; sense(d, 5, 0x53, 0x02); set_phase(PH_STATUS); return; }
        if (d->medium) kprintf("[SCSI] ID %d: the Mac ejects the CD %s (Apple's eject command)\n", t.id, d->name);
        __atomic_store_n(&d->medium, 0, __ATOMIC_RELEASE);
        good(); return;
    case 0xc1: {                                /* APPLE READ TOC */
        uint32_t alloc = be(t.cdb + 7, 2), k = 4;
        uint8_t m[4];
        if (t.cdb[9] == 0x80) {                 /* every track from cdb[5] on, four bytes each: the one data track, again */
            msf(m, 0); k = alloc / 4 * 4; if (k > 64) k = 64;
            for (uint32_t i = 0; i < k; i += 4) { t.small[i] = 0x14; t.small[i + 1] = bcd(m[1]); t.small[i + 2] = bcd(m[2]); t.small[i + 3] = bcd(m[3]); }
        } else if (t.cdb[9] == 0x40 || t.cdb[5] == 0x40) {   /* the lead-out's address */
            msf(m, frames_of(d)); t.small[0] = bcd(m[1]); t.small[1] = bcd(m[2]); t.small[2] = bcd(m[3]);
        } else if (t.cdb[5] == 0x80) {          /* one track's control and address */
            msf(m, 0); t.small[0] = 0x14; t.small[1] = bcd(m[1]); t.small[2] = bcd(m[2]); t.small[3] = bcd(m[3]);
        } else { t.small[0] = 1; t.small[1] = 1; }   /* the first and last tracks */
        data_in(k, alloc ? alloc : k, 1); return; }
    case 0xc2: data_in(9, be(t.cdb + 7, 2), 0); return;   /* APPLE READ SUB-CHANNEL: no audio, all zeros */
    case 0xcc: {                                /* APPLE AUDIO STATUS: idle, at the start of track 1 */
        uint8_t m[4]; msf(m, 0);
        t.small[0] = 5; t.small[2] = 0x14; t.small[3] = bcd(m[1]); t.small[4] = bcd(m[2]); t.small[5] = bcd(m[3]);
        data_in(6, 0, 0); return; }
    case 0xca: case 0xcb: case 0xce: good(); return;   /* APPLE AUDIO PAUSE, STOP, CONTROL: nothing plays */
    case 0xc8: case 0xc9: case 0xcd:                  /* APPLE AUDIO PLAY, PLAY TRACK, SCAN: a data CD */
    case 0x45: case 0x47: case 0x48: case 0x49: case 0xa5: case 0x4b: case 0x4e:   /* the audio commands: a data CD */
        { static int said; if (!said) { said = 1; kprintf("[SCSI] ID %d (CD-ROM): an audio command ($%02x) for a data CD: ILLEGAL MODE FOR THIS TRACK\n", t.id, op); } }
        t.status = 2; sense(d, 5, 0x64, 0); set_phase(PH_STATUS); return;
    }
    unsupported("not supported");
}
static void disk(uint8_t op)
{
    struct sdev *d = t.d;
    switch (op) {
    case 0x00: case 0x01: case 0x0b: case 0x2b: case 0x1b: case 0x35: good(); return;   /* TUR, REZERO, SEEKs, START STOP, SYNCHRONIZE CACHE */
    case 0x08: case 0x0a: transfer(be(t.cdb + 1, 3) & 0x1fffff, t.cdb[4] ? t.cdb[4] : 256, op == 0x0a); return;
    case 0x28: case 0x2a: case 0x2e: transfer(be(t.cdb + 2, 4), be(t.cdb + 7, 2), op != 0x28); return;
    case 0x2f:                                  /* VERIFY: with BytChk the blocks come and are taken as they are */
        if (t.cdb[1] & 2) { uint64_t b = (uint64_t)be(t.cdb + 7, 2) * d->bsize; data_out(b > 0xffffffffu ? 0xffffffffu : (uint32_t)b, DOUT_DISCARD); }
        else good();
        return;
    case 0x04:                                  /* FORMAT UNIT: an image has nothing to format; a defect list is taken and dropped */
        kprintf("[SCSI] ID %d: FORMAT UNIT (nothing to do for an image: its blocks are kept as they are)\n", t.id);
        if (t.cdb[1] & 0x10) data_out(4, DOUT_FORMAT); else good();
        return;
    case 0x07: data_out(4, DOUT_REASSIGN); return;   /* REASSIGN BLOCKS: no bad blocks to move */
    case 0x25: put(t.small, blocks_of(d) - 1, 4); put(t.small + 4, d->bsize, 4); data_in(8, 0, 0); return;
    case 0x37: t.small[1] = t.cdb[2] & 0x1f; data_in(4, be(t.cdb + 7, 2), 1); return;   /* READ DEFECT DATA: an empty list */
    case 0x1c: data_in(4, be(t.cdb + 3, 2), 1); return;   /* RECEIVE DIAGNOSTIC RESULTS: nothing to say */
    case 0x3b: data_out(be(t.cdb + 6, 3), DOUT_DISCARD); return;   /* WRITE BUFFER */
    case 0x3c: data_in(4, be(t.cdb + 6, 3), 1); return;   /* READ BUFFER: a buffer of 0 bytes */
    case 0x06: {                                /* Iomega's "non-sense" (vendor unique), as a Zip drive answers it */
        /* The Iomega driver on users' images (the full image's Apple_Driver, Zip 4.2, patched to take any disk by
           tools/iomega_patch.py) asks every drive for page 2 of it, 22 bytes, and later 63, and reads the disk's
           protection mode from byte 21 (0 none, 2 write-protected, 3 and 5 with a password ...) and a flag from
           byte 62. A CHECK CONDITION here left a second disk's drive without its disk-inserted event, so its
           volume never mounted (docs/08 item 52). All zeros: a disk in place, not protected. Drivers that send
           no vendor commands (Apple's HD SC Setup driver) never see it. */
        data_in(t.cdb[4] ? t.cdb[4] : 255, 0, 0); return; }
    }
    unsupported("not supported");
}
static void execute(void)
{
    struct sdev *d = t.d;
    uint8_t op = t.cdb[0];
    t.status = 0; t.cmds++; t.wr_ram = 0; d->cmds++;
    { unsigned k = t.ring_n++ % 12; t.ring[k].id = (uint8_t)t.id; for (int i = 0; i < 10; i++) t.ring[k].cdb[i] = t.cdb[i]; t.ring[k].n = t.cmds; t.ring[k].status = 0xff; }
    TRACE("ID %d: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x (%u-byte blocks)\n", t.id, t.cdb[0], t.cdb[1], t.cdb[2], t.cdb[3], t.cdb[4], t.cdb[5], t.cdb[6], t.cdb[7], t.cdb[8], t.cdb[9], d->bsize);
    for (unsigned i = 0; i < 64; i++) t.small[i] = 0;
    int lun = t.identified ? t.lun : t.cdb[1] >> 5;
    if (lun && op != 0x12 && op != 0x03) { check(5, 0x25); return; }          /* LOGICAL UNIT NOT SUPPORTED */
    if (op != 0x12 && op != 0x03 && __atomic_load_n(&d->ua, __ATOMIC_ACQUIRE)) {   /* UNIT ATTENTION: the CD was changed */
        __atomic_store_n(&d->ua, 0, __ATOMIC_RELEASE);
        if (__atomic_load_n(&d->medium, __ATOMIC_ACQUIRE)) { t.status = 2; sense(d, 6, 0x28, 0); set_phase(PH_STATUS); return; }
    }
    if (d->type == PSMAC_SCSI_TYPE_CDROM && needs_medium(op) && !__atomic_load_n(&d->medium, __ATOMIC_ACQUIRE)) { check(2, 0x3a); return; }   /* MEDIUM NOT PRESENT */
    if (common(op)) return;
    if (d->type == PSMAC_SCSI_TYPE_CDROM) cdrom(op); else disk(op);
}
/* a data out phase's last byte is in */
static void dout_done(void)
{
    switch (t.dout) {
    case DOUT_MODESEL6: mode_select_done(0); return;
    case DOUT_MODESEL10: mode_select_done(1); return;
    case DOUT_FORMAT: {                         /* the defect list header: the list itself, if any, to come and be dropped */
        uint32_t len = be(t.small + 2, 2);
        if (len) { t.dout = DOUT_DISCARD; t.left = len; t.buf = sector_buf; t.buf_len = len > sizeof sector_buf ? (uint32_t)sizeof sector_buf : len; t.buf_pos = 0; t.left -= t.buf_len; t.req = 1; return; }
        good(); return; }
    case DOUT_REASSIGN: {
        uint32_t len = be(t.small + 2, 2);
        if (len) { t.dout = DOUT_DISCARD; t.left = len; t.buf = sector_buf; t.buf_len = len > sizeof sector_buf ? (uint32_t)sizeof sector_buf : len; t.buf_pos = 0; t.left -= t.buf_len; t.req = 1; return; }
        good(); return; }
    default: good(); return;
    }
}
/* the byte on the bus, as the initiator reads it */
static uint8_t cur_data(void)
{
    if (t.phase == PH_DATAIN && t.buf_pos < t.buf_len) return t.buf[t.buf_pos];
    if (t.phase == PH_STATUS) return t.status;
    if (t.phase == PH_MSGIN) return t.msgin;
    if ((n.icr & 0x01) || (n.mr & 0x01)) return n.odr;
    return 0;
}
/* the initiator's ACK: the byte is taken or given, REQ drops */
static void ack_byte(void)
{
    if (!t.req) return;
    switch (t.phase) {
    case PH_CMD:
        if (t.cdb_n < 16) t.cdb[t.cdb_n] = n.odr;
        if (t.cdb_n == 0) {
            uint8_t g = n.odr >> 5;
            t.cdb_len = g == 0 ? 6 : (g == 1 || g == 2) ? 10 : g == 5 ? 12 : (g >= 6 && t.d && t.d->type == PSMAC_SCSI_TYPE_CDROM) ? 10 : 6;   /* a CD drive's vendor commands are ten bytes */
        }
        t.cdb_n++;
        break;
    case PH_MSGOUT:
        if (n.odr & 0x80) { t.lun = n.odr & 7; t.identified = 1; }   /* IDENTIFY */
        else if (n.odr == 0x06) t.abort = 1;                          /* ABORT */
        else if (n.odr == 0x0c) t.abort = 2;                          /* BUS DEVICE RESET */
        break;
    case PH_DATAOUT: if (t.buf_pos < t.buf_len) t.buf[t.buf_pos++] = n.odr; break;
    case PH_DATAIN:  if (t.buf_pos < t.buf_len) t.buf_pos++; break;
    default: break;
    }
    t.req = 0;
}
/* ACK released: the target moves on */
static void advance(void)
{
    switch (t.phase) {
    case PH_CMD:
        if (t.cdb_n < t.cdb_len) t.req = 1; else execute();
        break;
    case PH_MSGOUT:
        if (n.icr & 0x02) t.req = 1;
        else if (t.abort) { if (t.abort == 2 && t.d) { t.d->bsize = t.d->def_bsize; t.d->prevent = 0; } target_free(); }
        else { t.cdb_n = 0; t.cdb_len = 6; set_phase(PH_CMD); }
        break;
    case PH_DATAIN:
        if (t.buf_pos < t.buf_len) t.req = 1;
        else if (t.left) { next_chunk(); if (t.phase == PH_DATAIN) t.req = 1; }
        else set_phase(PH_STATUS);
        break;
    case PH_DATAOUT:
        if (t.buf_pos < t.buf_len) { t.req = 1; break; }
        if (t.dout == DOUT_STORE) {
            if (t.chunked) {
                uint32_t nb = t.buf_len / 512;
                if (!store_io(t.d, 1, t.sec, nb, sector_buf)) { check(3, 0x03); break; }
                t.sec += nb;
                if (t.left) { nb = t.left / 512; if (nb > sizeof sector_buf / 512) nb = sizeof sector_buf / 512; t.buf_len = nb * 512; t.buf_pos = 0; t.left -= nb * 512; t.req = 1; break; }
            }
            if (t.wr_ram) { psmac_card_dirty(t.wr_ram, t.wr_len); t.wr_ram = 0; }
            good();
            break;
        }
        if (t.left) {                           /* the rest of a parameter list longer than t.small: taken and dropped */
            uint32_t k = t.left > sizeof sector_buf ? (uint32_t)sizeof sector_buf : t.left;
            t.buf = sector_buf; t.buf_len = k; t.buf_pos = 0; t.left -= k; t.req = 1;
            break;
        }
        dout_done();
        break;
    case PH_STATUS: t.msgin = 0; set_phase(PH_MSGIN); break;   /* COMMAND COMPLETE */
    case PH_MSGIN:  target_free(); break;
    default: break;
    }
}
static int sel_cond(uint8_t icr) { return (icr & 0x04) && !(icr & 0x08) && (icr & 0x01) && !(icr & 0x80); }   /* SEL, data bus, no BSY, no RST */
static void connect(int i)
{
    t.d = &dev[i]; t.id = i; t.cdb_n = 0; t.cdb_len = 6; t.status = 0; t.lun = 0; t.identified = 0; t.abort = 0;
    dev[i].sel++;
    n.aip = 0;
    if (n.icr & 0x02) set_phase(PH_MSGOUT); else set_phase(PH_CMD);
}
static void first_wait(void)
{
    static int done;
    if (done) return;
    done = 1;
    int want = 0, any = 0;
    for (int i = 0; i < NID; i++) { want |= __atomic_load_n(&dev[i].waiting, __ATOMIC_ACQUIRE); any |= dev[i].present; }
    if (want || (solo_board && !any)) psmac_card_scan_wait();   /* a disk image on the card: there when the ROM looks for a startup disk */
}
static void try_select(void)
{
    if (t.phase != PH_FREE || !sel_cond(n.icr)) return;
    for (int i = 0; i < 7; i++) if (n.odr & (1u << i)) sel_seen[i]++;
    first_wait();
    for (int i = 0; i < 7; i++) {
        if (!(n.odr & (1u << i)) || !__atomic_load_n(&dev[i].present, __ATOMIC_ACQUIRE) || dev[i].disabled) continue;
        connect(i);
        return;
    }
}
static int phase_match(void) { return t.phase != PH_FREE && (n.tcr & 7) == (t.phase & 7); }
/* The last byte of a DMA receive, as Linux's NCR5380 driver takes it (28 November 2026, docs/27): with FLAG_DMA_FIXUP it
   moves one byte fewer through the pseudo-DMA window, waits for DRQ, then for REQ to go false, reads the byte from the
   input data register and leaves DMA mode. On the chip, in DMA receive mode a REQ of the matching input phase is
   answered with ACK at once (the byte latched, DRQ up), so the target has dropped REQ; leaving DMA mode drops ACK and
   the byte is done. Until then this model kept REQ up and Linux gave up with "PDMA fixup: !REQ timeout" and polled every
   byte from then on. Only while Linux is started (mac.linux=): Mac OS and A/UX were never seen to look at REQ there,
   and A/UX's disk is one of these devices on the owner's SE, which this cannot be tried against here. */
#ifdef PSMAC_SCSIDEV_HOST
static int psmac_linux_wanted(void) { return 0; }    /* the native test (tools/scsidev_test.c) is Mac OS's and A/UX's use */
#else
extern int psmac_linux_wanted(void);
#endif
static int dma_latched(void)
{
    return n.dma && t.req && phase_match() && (t.phase == PH_DATAIN || t.phase == PH_STATUS || t.phase == PH_MSGIN) &&
           psmac_linux_wanted();
}
uint32_t psmac_scsidev_read(uint32_t reg)
{
    switch (reg & 7) {
    case 0: return cur_data();
    case 1: return (n.icr & 0x9f) | (n.aip ? 0x40 : 0);
    case 2: return n.mr;
    case 3: return n.tcr;
    case 4: {
        uint32_t v = 0;
        if (n.icr & 0x80) v |= 0x80;
        if (t.phase != PH_FREE || (n.icr & 0x08)) v |= 0x40;
        if (t.req && !dma_latched()) v |= 0x20;
        if (t.phase != PH_FREE) v |= (uint32_t)(t.phase & 7) << 2;
        if (n.icr & 0x04) v |= 0x02;
        return v; }
    case 5: {
        uint32_t v = 0;
        if (n.dma && t.req && phase_match()) v |= 0x40;
        if (n.irq) v |= 0x10;
        if (phase_match()) v |= 0x08;
        if (n.busy_err) v |= 0x04;
        if (n.icr & 0x02) v |= 0x02;
        if (n.icr & 0x10) v |= 0x01;
        return v; }
    case 6: if (dma_latched()) n.latched = 1; return cur_data();
    default: n.irq = 0; n.busy_err = 0; return 0;
    }
}
static void reset_modes(void) { for (int i = 0; i < NID; i++) { dev[i].bsize = dev[i].def_bsize; dev[i].prevent = 0; } }
void psmac_scsidev_write(uint32_t reg, uint8_t d)
{
    switch (reg & 7) {
    case 0: n.odr = d; break;
    case 1: {
        uint8_t was = n.icr;
        n.icr = d & 0x9f;
        if (d & 0x80) { if (!(was & 0x80)) { target_free(); n.irq = 1; reset_modes(); } break; }   /* a bus reset: the targets as at power on */
        try_select();
        int ack = (n.icr & 0x10) != 0, was_ack = (was & 0x10) != 0;
        if (ack && !was_ack && !n.dma) ack_byte();
        if (!ack && was_ack && !n.dma) advance();
        break; }
    case 2:
        if (!(d & 0x02) && n.latched && dma_latched()) { ack_byte(); advance(); }   /* ACK dropped: the latched byte is done */
        n.latched = 0;
        n.mr = d; if (d & 0x01) n.aip = 1; if (!(d & 0x02)) n.dma = 0; break;   /* arbitration: the bus is free at once */
    case 3: n.tcr = d & 0x0f; break;
    case 4: n.ser = d; break;
    case 5: if (n.mr & 0x02) n.dma = 1; break;  /* start DMA send */
    case 6: break;
    default: if (n.mr & 0x02) n.dma = 1; break; /* start DMA initiator receive */
    }
}
/* A pseudo-DMA read takes the byte of whichever input phase the target is in, as the 5380's DMA handshake does once
   the phase matches TCR: data in, and also status and message in. A/UX 3's SCSI driver reads the status byte that way
   (DMA mode, start DMA initiator receive, wait for DRQ in the bus and status register, then one read of the pseudo-DMA
   window); until 26 November 2026 only data in was answered, the status read $FF with no ACK, and A/UX retried the
   same command for ever with the target held in the status phase (docs/23 section 15). */
/* a pseudo-DMA access with no byte to take or give: logged the first eight times, with where the target was (a driver
   that moves more bytes than the phase has, or a model that dropped REQ, either of which shifts the data) */
static void underflow(int write)
{
    if (++t.underflows > 8) return;
    kprintf("[SCSI] pseudo-DMA %s with nothing to %s (#%u): target %s, REQ %d, byte %u of %u, command $%02x, ID %d\n",
            write ? "write" : "read", write ? "take" : "give", t.underflows, phase_name(t.phase), t.req, t.buf_pos, t.buf_len,
            t.cdb[0], t.id);
}
/* The pseudo-DMA window moves a byte only in DMA mode (MR bit 1 and a start DMA write, until a phase change or MR
   ends it): the 5380's DACK does nothing else. Outside it a write only loads the output data register and a read
   returns the bus. A/UX 3's SCSI driver writes the window once before it starts a transfer, in both directions; taken
   as data, that byte went to the disk in front of the sector, and the root file system's superblock, which A/UX
   rewrites at mount, came back one byte late ("BAD SUPER BLOCK: MAGIC NUMBER WRONG" at the next start; 26 November
   2026, docs/23 section 15). The Mac OS SCSI Manager sets DMA mode first, as the 5380's data sheet asks. */
static uint32_t pdma_outside;
static void outside_dma(int write, uint8_t v)
{
    if (++pdma_outside > 4) return;
    kprintf("[SCSI] pseudo-DMA %s outside DMA mode (#%u): %s, target %s, MR $%02x, command $%02x: not a transfer\n",
            write ? "write" : "read", pdma_outside, write ? "the output data register loaded" : "the bus read", phase_name(t.phase),
            n.mr, t.cdb[0]);
    (void)v;
}
uint8_t psmac_scsidev_pdma_read(void)
{
    if (!n.dma) { uint8_t v = cur_data(); outside_dma(0, v); return v; }
    if (!(t.req && (t.phase == PH_DATAIN || t.phase == PH_STATUS || t.phase == PH_MSGIN))) { underflow(0); return 0xff; }
    n.latched = 0;
    uint8_t v = cur_data(); ack_byte(); advance(); return v;
}
void psmac_scsidev_pdma_write(uint8_t d)
{
    if (!n.dma) { n.odr = d; outside_dma(1, d); return; }
    if (!(t.req && (t.phase == PH_DATAOUT || t.phase == PH_CMD || t.phase == PH_MSGOUT))) { underflow(1); return; }
    n.odr = d; ack_byte(); advance();
}
void psmac_scsidev_reset(void)
{
    n.odr = n.icr = n.mr = n.tcr = n.ser = 0; n.dma = n.irq = n.busy_err = n.aip = 0;
    t.phase = PH_FREE; t.req = 0; t.d = 0;
    reset_modes();
    if (bus == BUS_LOCAL) bus = BUS_REAL;       /* the reset line has reset the real chip too */
}

/* ==== the boards with a real bus ================================================================ */
/* The interrupt line: the solo board's always; on a board with a real bus only while this file is the chip the Mac
   sees and a virtual device is configured (the empty bus alone never raised it, before 25 November or since) */
int psmac_scsidev_irq(void) { return bus == BUS_SOLO ? n.irq : bus != BUS_REAL && any_cfg ? n.irq : 0; }
int psmac_scsidev_local(void) { return bus != BUS_REAL; }
void psmac_scsidev_bus(const struct psmac_scsi_wire *w, int empty) { wire = w; bus = empty ? BUS_EMPTY : BUS_REAL; }
void psmac_scsidev_bus_empty(int empty)
{
    if (!wire) return;
    psmac_scsidev_reset();
    bus = empty ? BUS_EMPTY : BUS_REAL;
}
/* the virtual ID the data lines select (ODR, the Mac's own bit 7 aside), or -1 */
static int virtual_target(void)
{
    for (int i = 0; i < 7; i++)
        if ((n.odr & (1u << i)) && (dev[i].present || dev[i].waiting) && !dev[i].disabled) return i;
    return -1;
}
/* the registers as the Mac writes them, without anything happening: the real chip does the work */
static void shadow(uint32_t reg, uint8_t d)
{
    switch (reg & 7) {
    case 0: n.odr = d; break;
    case 1: n.icr = d & 0x9f; break;
    case 2: n.mr = d; if (!(d & 0x02)) n.dma = 0; break;
    case 3: n.tcr = d & 0x0f; break;
    case 4: n.ser = d; break;
    default: break;
    }
}
/* back to the real chip: given the registers the Mac last wrote, the bus left to it */
static void to_real(void)
{
    wire->wr(0, n.odr); wire->wr(3, n.tcr);
    wire->wr(1, n.icr & 0x0f & ~0x08);          /* data bus, ATN, SEL as the Mac has them; never ACK, BSY or RST left over */
    wire->wr(2, n.mr & ~0x01);
    n.irq = 0; n.busy_err = 0; n.dma = 0; n.aip = 0;
    bus = BUS_REAL; returned++;
}
/* The Mac's selection of a virtual ID, on the real bus as it stands: its arbitration won, SEL, the data lines and
   (perhaps) ATN asserted, BSY just released. 1: the selection is answered here (or a real device took it: the
   write has gone to the real chip); 0: not virtual after all, the write is to go to the real chip. */
static int divert(int id)
{
    struct sdev *d = &dev[id];
    if (!d->present) {                          /* waiting for the card's first look */
        psmac_card_scan_wait();
        if (!__atomic_load_n(&d->present, __ATOMIC_ACQUIRE)) return 0;
    }
    if (!d->probed) {
        d->probed = 1;
        if (!probe_off) {
            probes++;
            if (n.mr & 0x01) wire->wr(2, n.mr & ~0x01);   /* arbitration over: the chip's own BSY off */
            wire->wr(1, n.icr);                 /* the selection itself, on the real bus */
            uint64_t t0 = ticks(), lim = ticks_hz() / 1000u * PROBE_MS;
            int answered = 0;
            for (uint32_t k = 0; !answered && (k < PROBE_READS || ticks() - t0 < lim); k++)
                if (wire->rd(4) & 0x40) answered = 1;
            if (answered) {
                d->disabled = 1;
                kprintf("[SCSI] WARNING: a real device on the SCSI bus answers ID %d: the virtual %s %s is turned off for this session, "
                        "the ID is the real device's (choose a free ID: scsi.%s=NAME:ID)\n", id, kind(d), d->name, d->type == PSMAC_SCSI_TYPE_CDROM ? "cd" : "disk");
                ALERT("SCSI ID %d is a real device's: the virtual %s is off (choose another ID)", id, kind(d));
                return 1;                       /* the real device's connection: the write has gone to the chip */
            }
            kprintf("[SCSI] ID %d: no real device answers it on the SCSI bus: the virtual %s %s answers here\n", id, kind(d), d->name);
        }
    }
    wire->wr(1, 0);                             /* the real bus let go: SEL, the data lines and ATN released */
    if (n.mr & 0x01) wire->wr(2, n.mr & ~0x01);
    bus = BUS_LOCAL; diverted++;
    n.aip = 0; n.irq = 0; n.busy_err = 0;
    t.phase = PH_FREE;
    try_select();
    return 1;
}
int psmac_scsidev_bus_write(uint32_t reg, uint8_t d)
{
    if (bus == BUS_SOLO || bus == BUS_EMPTY) { psmac_scsidev_write(reg, d); return 1; }
    reg &= 7;
    if (reg == 1 && (d & 0x80) && !(n.icr & 0x80)) reset_modes();   /* the Mac resets the bus: the virtual targets see it too */
    if (bus == BUS_LOCAL) {
        /* the bus is free again after a virtual connection: answered here until the Mac starts something the real
           bus must see (an arbitration, a reset, a selection of a real ID) */
        int to_wire = t.phase == PH_FREE &&
            ((reg == 2 && (d & 0x01)) || (reg == 1 && (d & 0x80)) || (reg == 1 && sel_cond(d) && virtual_target() < 0));
        if (!to_wire) { psmac_scsidev_write(reg, d); return 1; }
        shadow(reg, d);
        to_real();
        return 0;
    }
    uint8_t was = n.icr;
    shadow(reg, d);
    if (reg == 1 && sel_cond(n.icr) && !sel_cond(was)) {
        int id = virtual_target();
        if (id >= 0 && divert(id)) return 1;    /* (try_select counted it) */
        for (int i = 0; i < 7; i++) if (n.odr & (1u << i)) sel_seen[i]++;
    }
    return 0;
}

/* ==== the heartbeat and the remote control ====================================================== */
void psmac_scsidev_report(void)
{
    static const char *const bn[] = { "solo", "real chip", "a virtual device's connection", "the empty bus answered here" };
    kprintf("[SCSI] the 5380: %s, target %s req %d, %u commands, %u phase interrupts, %u underflows | selections by ID 0-6: %u %u %u %u %u %u %u",
            bn[bus & 3], phase_name(t.phase), t.req, t.cmds, t.mismatches, t.underflows, sel_seen[0], sel_seen[1], sel_seen[2], sel_seen[3], sel_seen[4], sel_seen[5], sel_seen[6]);
    if (wire) kprintf(" | %u connections answered here, %u back to the real chip, %u IDs checked on the real bus", diverted, returned, probes);
    kprintf("\n");
    for (int i = 0; i < NID; i++) {
        struct sdev *d = &dev[i];
        if (!d->present && !d->cfg) continue;
        kprintf("[SCSI]   ID %d %s %s%s: %u selections, %u commands (%u not supported), %u sectors read, %u written, %u-byte blocks%s%s\n", i, kind(d), d->name[0] ? d->name : "(no CD)",
                d->disabled ? " (OFF: a real device has this ID)" : "", d->sel, d->cmds, d->unknown, d->reads, d->writes, d->bsize,
                d->type == PSMAC_SCSI_TYPE_CDROM ? (d->medium ? ", CD in" : ", no CD") : "", d->waiting ? ", waiting for the card" : "");
    }
    static uint32_t shown;
    if (t.ring_n != shown) {
        shown = t.ring_n;
        kprintf("[SCSI]   last commands (n id: cdb -> status):");
        unsigned first = t.ring_n > 12 ? t.ring_n - 12 : 0;
        for (unsigned i = first; i < t.ring_n; i++) { unsigned k = i % 12; kprintf(" %u %u:%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x->%x", t.ring[k].n, t.ring[k].id, t.ring[k].cdb[0], t.ring[k].cdb[1], t.ring[k].cdb[2], t.ring[k].cdb[3], t.ring[k].cdb[4], t.ring[k].cdb[5], t.ring[k].cdb[6], t.ring[k].cdb[7], t.ring[k].cdb[8], t.ring[k].cdb[9], t.ring[k].status); }
        kprintf("\n");
    }
}
static unsigned jput(char *o, unsigned at, unsigned max, const char *s) { while (*s && at + 1 < max) o[at++] = *s++; o[at < max ? at : max - 1] = 0; return at; }
static unsigned jnum(char *o, unsigned at, unsigned max, uint64_t v) { char b[24]; int k = 0; do { b[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k && at + 1 < max) o[at++] = b[--k]; o[at < max ? at : max - 1] = 0; return at; }
static unsigned jstr(char *o, unsigned at, unsigned max, const char *s)
{
    at = jput(o, at, max, "\"");
    for (; *s && at + 2 < max; s++) { if (*s == '"' || *s == '\\') o[at++] = '\\'; o[at++] = (unsigned char)*s < ' ' ? '?' : *s; }
    return jput(o, at, max, "\"");
}
int psmac_scsidev_json(char *o, unsigned max)
{
    unsigned k = jput(o, 0, max, "{\"devices\": [");
    int first = 1;
    for (int i = 0; i < NID; i++) {
        struct sdev *d = &dev[i];
        if (!d->present && !d->cfg) continue;
        if (!first) k = jput(o, k, max, ", ");
        first = 0;
        k = jput(o, k, max, "{\"id\": "); k = jnum(o, k, max, (uint64_t)i);
        k = jput(o, k, max, ", \"type\": "); k = jstr(o, k, max, d->type == PSMAC_SCSI_TYPE_CDROM ? "cdrom" : "disk");
        k = jput(o, k, max, ", \"name\": "); k = jstr(o, k, max, d->name);
        k = jput(o, k, max, ", \"bytes\": "); k = jnum(o, k, max, d->bytes);
        k = jput(o, k, max, ", \"block\": "); k = jnum(o, k, max, d->bsize);
        if (d->type == PSMAC_SCSI_TYPE_CDROM) { k = jput(o, k, max, ", \"medium\": "); k = jput(o, k, max, d->medium ? "true" : "false"); }
        k = jput(o, k, max, ", \"writable\": "); k = jput(o, k, max, d->writable ? "true" : "false");
        k = jput(o, k, max, ", \"off\": "); k = jput(o, k, max, d->disabled ? "true" : "false");
        k = jput(o, k, max, ", \"commands\": "); k = jnum(o, k, max, d->cmds);
        k = jput(o, k, max, ", \"sectors_read\": "); k = jnum(o, k, max, d->reads);
        k = jput(o, k, max, ", \"sectors_written\": "); k = jnum(o, k, max, d->writes);
        k = jput(o, k, max, "}");
    }
    k = jput(o, k, max, "]}\n");
    return (int)k;
}
