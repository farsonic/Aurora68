/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_usbdisk.c - USB flash drives plugged into the Pi, as disks on the Mac's desktop.
 *
 * Backstory. The plan for USB on the Mac (20 November 2026) is to keep USB on the Pi and to show
 * the Mac each device as something it already understands, through the slot-9 card. A keyboard
 * and a mouse already go that way (psmac_usb.c: their reports become .PiInput's events). A flash
 * drive is the next: the Mac already has a disk driver on the card, .PiDisk, which serves the
 * card's disk images through the mailbox (psmac_mbox.c, mac/pidisk.s), so a stick becomes one more
 * PiDisk unit. The Mac sees an ejectable disk: it is told a disk was inserted, mounts the HFS volume
 * it finds there, ejects it from the Finder, and is told the drive is empty when the stick is pulled
 * out, the way a Macintosh learns that a floppy was taken out with a paper clip.
 *
 * Which core does what. The 68k's own core (0) never waits for USB. Core 1 runs every enumeration
 * of the bus (psmac_usb.c) and the sound output's feed (psmac_audio.c); it also runs everything in
 * this file, so a stick's work never overlaps an enumeration and core 3, which serves the mailbox,
 * the network and the card, is never held by a slow stick. Core 3 hands core 1 one job at a time (a
 * read or a write of up to 64 KB, or an eject) through the job slot below and copies the Mac's data
 * in and out of the job's buffer itself (psmac_mbox.c). Every wait here feeds the sound output.
 *
 * Sharing the controller. Channel 0 belongs to psmac_usb.c (the keyboard's and mouse's polls and
 * their control requests), channels 1 and 2 to the wired Ethernet's bulk transfers
 * (psmac_usbnet.c). This file uses channels 3 (bulk in), 4 (bulk out) and 5 (control), by DMA
 * through a bounce buffer, the same way psmac_usbnet.c does; the BCM2837's DWC2 has eight. Only
 * after an enumeration, while core 3 keeps off the bus (psmac_usb.c's usb_on is down), does this
 * file speak to devices it does not know yet (claim): it resets the hub ports the keyboard's
 * enumeration did not reach, reads every device's descriptors and configures the sticks. Then it
 * speaks only to its own sticks, and psmac_usbnet.c leaves them alone (psmac_usbdisk_owns).
 *
 * Addresses: Alice68's enumeration gives the hub address 1 and the device on its port N address
 * 1 + N, and leaves a device that is neither a keyboard, a mouse nor a hub addressed but not
 * configured; a port it did not reach is reset here and its device given the same address. A Pi
 * without a hub (a 3A+ or a Zero 2 W with a stick in its one port) has the stick at address 1.
 *
 * Speeds. A high-speed stick behind the Pi 3B's LAN9514 (a high-speed hub) is spoken to directly,
 * whole transfers of up to 64 KB at a time (512-byte packets). A full-speed stick behind a full-
 * speed hub (QEMU's usb-hub, a user's old hub) likewise, with 64-byte packets, 32 KB at a time. A
 * full-speed stick behind a high-speed hub has to go through the hub's transaction translator:
 * split transactions, one packet at a time (start split, complete split, again on NYET, the same
 * steps as psmac_usb.c's dma_packet for the keyboard), 8 KB a job. That path is written but not
 * tested: QEMU has no high-speed hub, and few sticks made since 2005 are full speed.
 *
 * The protocol is the USB Mass Storage Class's Bulk-Only Transport (class 8, subclass 6 SCSI,
 * protocol $50), each command a 31-byte CBW, data, a 13-byte CSW. The recovery follows the
 * specification (section 5.3 and 6.6): a stalled data stage is cleared and the CSW still read; a
 * stalled CSW is cleared and read once more; a CSW that is not a CSW, a phase error, a CBW that
 * would not go, or a transfer that timed out ends in a reset recovery (the class's Bulk-Only Mass
 * Storage Reset, then CLEAR_FEATURE(ENDPOINT_HALT) on both bulk endpoints). Device behaviours
 * taken into account, learnt from reading Neptune's massstorage class (github.com/rondoval/
 * neptune-usb-stack, AROS Public License; no code of it is used here): a GET_MAX_LUN that stalls
 * means one LUN; a CSW a device sends in place of the data is taken as the CSW; a truncated CSW is
 * read again; the residue is not trusted. The SCSI commands: INQUIRY, TEST UNIT READY (with
 * REQUEST SENSE: a unit attention after the reset is normal, "medium not present" is a card reader
 * without a card), READ CAPACITY(10) and (16) beyond 2 TB, MODE SENSE(6) for the write-protect
 * switch, READ(10) and WRITE(10), SYNCHRONIZE CACHE(10) after writes and at an eject. Only 512-byte
 * sectors: PiDisk serves 512-byte blocks, and anything else is refused, with a line in the log.
 *
 * What the Mac is offered. System 7 reads HFS: a stick that is a bare HFS volume (the signature
 * 'BD' in its block 2) is offered whole; one with an Apple partition map, like the card's images,
 * as its first Apple_HFS partition; an MBR or GPT disk whose HFS partition really holds HFS
 * likewise. NTFS, HFS Plus and blank sticks are not offered, so the Finder never asks to initialize
 * a stick of the user's PC; the log says what the stick holds and why it was not offered.
 * mac.usbdisk=all offers them anyway, as the whole stick (its first 2 GB, which is what HFS on System 7
 * reaches): for a Mac with PC Exchange, or to initialize a stick on the Mac. mac.usbdisk=off leaves
 * sticks alone. Since 21 November 2026 (issue #20) a FAT or exFAT stick, what Windows, Linux and a modern
 * Mac write, is read with psmac_fatfs.c: every HFS disk image in its root (.image, .dsk, .hda, .img,
 * .hfv) is a volume of its own, read and written in place in the file's clusters, as the card's images
 * are. Writing inside an image changes no FAT, no directory and no size, so it cannot damage the stick's
 * file system.
 *
 * Writes. Nothing is cached here: a write job ends when the stick's CSW says the WRITE(10) is done,
 * and the Mac's write call returns after that. A stick may keep the data in its own RAM for a
 * moment; SYNCHRONIZE CACHE goes to it half a second after the last write and before an eject
 * (a stick that refuses the command is not asked again).
 *
 * Log lines: [USB] for what was found on the bus and how the stick answered, [PSMAC:DISK] (core 3,
 * psmac_mbox.c) for what the Mac was told. Written 20 November 2026.
 */
#include <stdint.h>
#include <string.h>
#include "support.h"
#include "mmu.h"
#include "../alice/alice_psmac.h"
#include "psmb.h"
#include "tlsf.h"
#include "psmac_usbdisk.h"
#include "psmac_fatfs.h"

const char *psmac_option(const char *key);
extern int psmac_audio_poll(void);
void psmac_usb_ask_enumeration(const char *why);

static inline uint64_t now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static uint32_t ms(void) { return (uint32_t)(now() / (hz() / 1000u)); }
/* every wait on core 1 keeps the sound output fed (psmac_audio.c) */
static void wait_us(unsigned us)
{
    uint64_t t0 = now(), n = ((uint64_t)us * hz() + 999999u) / 1000000u;
    while (now() - t0 < n)
        psmac_audio_poll();
}

/* ---- mac.usbdisk= --------------------------------------------------------------------------------- */
enum { MODE_OFF, MODE_HFS, MODE_ALL };
static int mode = -1;
static int mode_get(void)
{
    if (mode < 0) {
        const char *o = psmac_option("mac.usbdisk");
        mode = !o ? MODE_HFS : !strncmp(o, "off", 3) ? MODE_OFF : !strncmp(o, "all", 3) ? MODE_ALL : MODE_HFS;
        if (o && strncmp(o, "off", 3) && strncmp(o, "all", 3) && strncmp(o, "on", 2))
            kprintf("[USB] mac.usbdisk=%s not understood: on, all or off; on it is\n", o);
        if (mode == MODE_OFF)
            kprintf("[USB] disks: off (mac.usbdisk=off): USB flash drives are left alone\n");
    }
    return mode;
}

/* ---- the controller's channels 3, 4 and 5, by DMA ------------------------------------------------- */
#define DWC2 (ALICE_PERI + 0x980000ull)         /* BCM2837 0x3F980000 in the kernel's peripheral mapping */
static uint32_t rd(uint32_t r)
{
    uint32_t v = *(volatile uint32_t *)(uintptr_t)(DWC2 + r);
    __asm__ volatile("dmb oshld" ::: "memory");
    return __builtin_bswap32(v);
}
static void wr(uint32_t r, uint32_t v)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)(uintptr_t)(DWC2 + r) = __builtin_bswap32(v);
    __asm__ volatile("dsb oshst" ::: "memory");
}
#define HCCHAR(c)   (0x500u + 0x20u * (c))
#define HCSPLT(c)   (0x504u + 0x20u * (c))
#define HCINT(c)    (0x508u + 0x20u * (c))
#define HCINTMSK(c) (0x50cu + 0x20u * (c))
#define HCTSIZ(c)   (0x510u + 0x20u * (c))
#define HCDMA(c)    (0x514u + 0x20u * (c))
#define B(n) (1u << (n))
enum { CH_IN = 3, CH_OUT = 4, CH_CTL = 5 };
/* HCINTn, the DWC2 programming model */
enum { HC_XFERCOMPL = B(0), HC_CHHLTD = B(1), HC_STALL = B(3), HC_NAK = B(4), HC_ACK = B(5), HC_NYET = B(6) };
/* HCTSIZn's PID field */
enum { PID_DATA0 = 0, PID_DATA1 = 2, PID_SETUP = 3 };
/* what a transfer can end in, besides the bytes it moved */
enum { E_STALL = -1, E_TIMEOUT = -2, E_XACT = -3, E_ARGS = -4 };

/* Bounce buffers: the DMA reads and writes them through the VideoCore's uncached alias, with the
   cache cleaned and invalidated around each transfer. Every one starts on a cache line and holds
   whole packets, because an IN transfer is programmed in whole packets (a 13-byte CSW is a 512-byte
   transfer at high speed that the stick ends early). */
static uint8_t jbuf[USBDISK_BUF] __attribute__((aligned(64)));    /* the job's data (core 3 copies the Mac's in and out) */
static uint8_t pbuf[32768] __attribute__((aligned(64)));          /* what the bring-up reads: INQUIRY, capacity, the first 64 blocks */
static uint8_t vbuf[512] __attribute__((aligned(64)));            /* one block looked at on its own (a partition's block 2) */
static uint8_t cbw[64] __attribute__((aligned(64)));
static uint8_t csw[512] __attribute__((aligned(64)));
static uint8_t sense[512] __attribute__((aligned(64)));
static uint8_t setupbuf[64] __attribute__((aligned(64)));
static uint8_t ebuf[512] __attribute__((aligned(64)));            /* a control transfer's data stage */
static uint8_t zbuf[512] __attribute__((aligned(64)));            /* the room a status stage is given */

static uint32_t bus_addr(const void *p) { return (uint32_t)mmu_virt2phys((uintptr_t)p) | 0xC0000000u; }

struct dev {
    uint8_t addr, port, hub, split;         /* hub/port: the translator's route, for a split device */
    uint8_t mps0, ep_in, ep_out, itf, cfg, lun, maxlun;
    uint8_t tog_in, tog_out;                /* the bulk endpoints' data toggles, as HCTSIZ's PID codes */
    uint8_t high;                           /* 1 high speed, 0 full */
    uint16_t mps, vid, pid;
};

static void ch_halt(int c)
{
    if (rd(HCCHAR(c)) & B(31)) {
        wr(HCCHAR(c), rd(HCCHAR(c)) | B(30) | B(31));
        uint64_t end = now() + hz() / 100;          /* 10 ms */
        while (!(rd(HCINT(c)) & HC_CHHLTD) && now() < end)
            psmac_audio_poll();
    }
    wr(HCINT(c), 0xffffffffu);
}

/* one run of a channel: programmed, enabled, waited for until it halts. The interrupt bits it
   halted with, 0 when it did not halt in time (it is then halted here). */
static uint32_t ch_run(int c, uint32_t hcchar, uint32_t hcsplt, uint32_t hctsiz, uint32_t dma, uint32_t timeout_ms)
{
    if (rd(HCCHAR(c)) & B(31))
        ch_halt(c);
    wr(HCINT(c), 0xffffffffu);
    wr(HCINTMSK(c), 0x7ffu);
    wr(HCSPLT(c), hcsplt);
    wr(HCTSIZ(c), hctsiz);
    wr(HCDMA(c), dma);
    wr(HCCHAR(c), hcchar | B(31));
    uint64_t end = now() + hz() / 1000 * timeout_ms;
    for (;;) {
        uint32_t s = rd(HCINT(c));
        if (s & HC_CHHLTD)
            return s;
        if (now() >= end)
            break;
        psmac_audio_poll();
    }
    ch_halt(c);
    return 0;
}

static uint32_t hcchar_of(const struct dev *d, int type, int ep, int in, uint32_t mps)
{
    return (uint32_t)d->addr << 22 | 1u << 20 | (uint32_t)type << 18 | (uint32_t)!!in << 15 | (uint32_t)ep << 11 | mps;
}

/* Through a high-speed hub's translator: one packet at a time. A NAK is the device with nothing yet
   (the same packet again a little later, until the deadline); a NYET on the complete split is the
   translator still waiting for the device. */
static int xfer_split(int c, const struct dev *d, int type, int ep, int in, uint8_t *tog, uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    uint32_t mps = type == 0 ? d->mps0 : d->mps, done = 0;
    uint32_t hcchar = hcchar_of(d, type, ep, in, mps);
    uint32_t splt = B(31) | 3u << 14 | (uint32_t)d->hub << 7 | d->port;       /* xactpos: all */
    uint64_t end = now() + hz() / 1000 * timeout_ms;
    if (mps > 64)
        return E_ARGS;
    do {
        uint32_t n = in ? mps : (len - done > mps ? mps : len - done);
        uint8_t *p = buf + done;
        uint32_t tsz = n | 1u << 19 | (uint32_t)*tog << 29;
        arm_flush_cache((uintptr_t)p, n ? n : 4);
        uint32_t s = ch_run(c, hcchar, splt, tsz, bus_addr(p), 50);
        if (s && (s & HC_ACK) && !(s & HC_XFERCOMPL)) {
            for (unsigned k = 0;; k++) {
                wait_us(125);
                s = ch_run(c, hcchar, splt | B(16), tsz, bus_addr(p), 50);
                if (!s || !(s & HC_NYET) || k >= 64 || now() >= end)
                    break;
            }
        }
        if (!s || (s & HC_NYET))
            return E_TIMEOUT;
        if (s & HC_STALL)
            return E_STALL;
        if (s & HC_NAK) {
            if (now() >= end)
                return E_TIMEOUT;
            wait_us(250);
            continue;
        }
        if (!(s & HC_XFERCOMPL))
            return E_XACT;
        uint32_t got = n - (rd(HCTSIZ(c)) & 0x7ffffu);
        if (got > n)
            got = n;
        *tog = *tog == PID_DATA0 ? PID_DATA1 : PID_DATA0;
        if (in)
            arm_dcache_invalidate((uintptr_t)p, n);
        done += got;
        if (in && got < mps)
            break;                                      /* a short packet ends an IN transfer */
    } while (done < len);
    return (int)(done > len ? len : done);
}

/* A transfer on channel c: OUT len bytes, or IN up to len (buf holds len rounded up to whole
   packets). *tog is the endpoint's data toggle, moved on by the transfer. The bytes moved, or E_*. */
static int xfer(int c, const struct dev *d, int type, int ep, int in, uint8_t *tog, uint8_t *buf, uint32_t len, uint32_t timeout_ms)
{
    uint32_t mps = type == 0 ? d->mps0 : d->mps;
    if (!mps)
        return E_ARGS;
    if (d->split)
        return xfer_split(c, d, type, ep, in, tog, buf, len, timeout_ms);
    /* an IN transfer in whole packets, at least one (Linux's dwc2 does the same); the stick's short
       packet ends it */
    uint32_t pkts = len ? (len + mps - 1) / mps : 1;
    uint32_t size = in ? pkts * mps : len;
    if (pkts > 1023 || size > 0x7ffffu)
        return E_ARGS;
    arm_flush_cache((uintptr_t)buf, size ? size : 4);
    uint32_t s = ch_run(c, hcchar_of(d, type, ep, in, mps), 0, size | pkts << 19 | (uint32_t)*tog << 29, bus_addr(buf), timeout_ms);
    if (!s)
        return E_TIMEOUT;
    uint32_t t = rd(HCTSIZ(c));
    if (s & HC_XFERCOMPL) {
        uint32_t left = t & 0x7ffffu, got = size > left ? size - left : 0;
        *tog = (uint8_t)((t >> 29) & 3u);               /* the toggle the controller expects next */
        if (in) {
            arm_dcache_invalidate((uintptr_t)buf, size);
            if (got > len)
                got = len;
        }
        return (int)got;
    }
    if (s & HC_STALL)
        return E_STALL;
    return E_XACT;
}

/* ---- control transfers (channel 5) ---------------------------------------------------------------- */
static int ctl(const struct dev *d, const uint8_t setup[8], uint8_t *data, uint16_t len)
{
    uint8_t tog = PID_SETUP;
    int in = (setup[0] & 0x80u) != 0, rc;
    uint32_t got = 0;
    if (len > sizeof ebuf)
        return E_ARGS;
    memcpy(setupbuf, setup, 8);
    rc = xfer(CH_CTL, d, 0, 0, 0, &tog, setupbuf, 8, 500);
    if (rc < 0)
        return rc;
    if (len) {
        tog = PID_DATA1;
        if (!in)
            memcpy(ebuf, data, len);
        rc = xfer(CH_CTL, d, 0, 0, in, &tog, ebuf, len, 1000);
        if (rc < 0)
            return rc;
        got = (uint32_t)rc;
        if (in)
            memcpy(data, ebuf, got);
    }
    tog = PID_DATA1;                                    /* the status stage: the other direction, no data */
    rc = xfer(CH_CTL, d, 0, 0, !(len && in), &tog, zbuf, 0, 500);
    return rc < 0 ? rc : (int)got;
}
static int get_desc(const struct dev *d, uint8_t type, uint8_t index, uint16_t lang, uint8_t *buf, uint16_t len)
{
    uint8_t s[8] = { 0x80, 6, index, type, (uint8_t)lang, (uint8_t)(lang >> 8), (uint8_t)len, (uint8_t)(len >> 8) };
    return ctl(d, s, buf, len);
}

/* the hub at address 1 (Alice68's enumeration gives the root device that address); its endpoint 0
   takes 64 bytes, as psmac_usb.c assumes for the same requests */
static const struct dev hubdev = { .addr = 1, .mps0 = 64 };
static int hub_status(unsigned port, uint32_t *st)
{
    uint8_t s[8] = { 0xa3, 0, 0, 0, (uint8_t)port, 0, 4, 0 }, b[4] = { 0 };
    if (ctl(&hubdev, s, b, 4) != 4)
        return -1;
    *st = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 0;
}
static int hub_feature(uint8_t req, uint16_t feature, unsigned port)
{
    uint8_t s[8] = { 0x23, req, (uint8_t)feature, (uint8_t)(feature >> 8), (uint8_t)port, 0, 0, 0 };
    return ctl(&hubdev, s, 0, 0) < 0 ? -1 : 0;
}

/* A port the keyboard's enumeration did not reach (it stops once it has a keyboard and a mouse):
   reset it and give its device the address that enumeration would have, 1 + port. As psmac_usbnet.c's
   address_port, which runs later on core 3 and then finds the port enabled and leaves it be. */
static int address_port(unsigned port, uint32_t *st)
{
    uint8_t d8[8];
    struct dev z = { .addr = 0, .mps0 = 64 };
    hub_feature(3, 4, port);                            /* SET_FEATURE(PORT_RESET) */
    for (int i = 0; i < 10; i++) {
        wait_us(20000);
        if (!hub_status(port, st) && (*st & 2u))
            break;
    }
    if (!(*st & 2u))
        return -1;
    hub_feature(1, 20, port);                           /* CLEAR_FEATURE(C_PORT_RESET) */
    if (*st & B(9))
        return -2;                                      /* low speed: no mass storage device is one */
    z.high = (*st & B(10)) != 0;
    wait_us(10000);                                     /* reset recovery: 10 ms (USB 2.0, 7.1.7.5) */
    if (get_desc(&z, 1, 0, 0, d8, 8) != 8 || d8[1] != 1)
        return -3;
    z.mps0 = d8[7] ? d8[7] : 8;
    uint8_t sa[8] = { 0, 5, (uint8_t)(1 + port), 0, 0, 0, 0, 0 };      /* SET_ADDRESS */
    if (ctl(&z, sa, 0, 0) < 0)
        return -4;
    wait_us(10000);
    return 0;
}

/* ---- the sticks ------------------------------------------------------------------------------------ */
enum { U_EMPTY, U_CLAIMED, U_NOMEDIUM, U_UP, U_REFUSED, U_EJECTED, U_GONE, U_FAILED };
static const char *const state_name[] = { "empty", "found", "no medium", "on the Mac", "not offered", "ejected", "unplugged", "failed" };
struct unit {
    int state, seen, again, asked_reset;
    struct dev d;
    char vendor[9], product[17], rev[5], mfr[32], prod[32], serial[40];
    uint32_t tag, dev_blocks, first, count, writable;
    uint32_t next_try, last_write;
    int dirty, no_sync, removable, said_nomedium, sync_fails;   /* sync_fails: SYNCHRONIZE CACHEs in a row that failed */
    uint8_t key, asc, ascq;                     /* the last CHECK CONDITION's sense */
    uint32_t reads, writes, rblocks, wblocks, errors, resets;
    uint32_t gen;                               /* moves at each bring-up: its volumes are taken afresh (vol_take) */
    struct fx_vol *fx;                          /* a FAT or exFAT stick: its file system (psmac_fatfs.c) */
};
static struct unit U[USBDISK_UNITS];
static uint32_t seen_last;                      /* sticks found in the last enumeration (psmac_usb.c's retries) */

/* the published state (core 1 writes, core 3 reads with the sequence count) */
static struct pub { volatile uint32_t seq; struct usbdisk_info i; } P[USBDISK_UNITS];
static volatile uint32_t changes;
/* What the Mac is offered of a stick: its volumes (21 November 2026, issue #20). Until then a stick was
   one PiDisk unit, the whole stick or its HFS partition. A FAT or exFAT stick can carry several disk images
   (.image, .dsk, .hda, .img, .hfv, as the card's), each a PiDisk unit of its own, read and written in place
   in the file's clusters. The published slots P[] and the job slot are numbered by volume, so psmac_mbox.c
   sees volumes where it saw sticks; four in all, as PiDisk has four units. */
enum { VK_RAW, VK_IMAGE };
struct run { uint32_t clus, n; };
struct vol {
    int used, unit, kind, present;              /* present: offered and not ejected */
    uint32_t gen, first, count;                 /* RAW: blocks of the stick; IMAGE: blocks of the image file */
    struct run *runs; uint32_t nruns;           /* IMAGE: the file's clusters */
    char name[48];
};
static struct vol V[USBDISK_UNITS];
static void *mem(uint32_t n)
{
    /* the kernel's heap is shared with everything on the Pi: 8 MB are always left to the rest */
    if (tlsf_get_free_size(tlsf) < (uintptr_t)n + (8u << 20))
        return 0;
    return tlsf_malloc_aligned(tlsf, n ? n : 4, 64);
}
static void mem_free(void *p)
{
    if (p)
        tlsf_free(tlsf, p);
}
static void pub_slot(int s, int present, int new_medium)
{
    struct pub *p = &P[s];
    const struct vol *v = &V[s];
    const struct unit *u = &U[v->unit];
    __atomic_store_n(&p->seq, p->seq + 1, __ATOMIC_RELAXED);
    __asm__ volatile("dmb ish" ::: "memory");
    p->i.present = (uint32_t)present;
    p->i.blocks = v->count;
    p->i.writable = u->writable;
    if (new_medium)
        p->i.media++;
    memcpy(p->i.name, v->name, sizeof p->i.name);
    __asm__ volatile("dmb ish" ::: "memory");
    __atomic_store_n(&p->seq, p->seq + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&changes, changes + 1, __ATOMIC_RELEASE);
}
/* every volume of stick n: present (if it is offered and not ejected) or not */
static void publish(int n, int present, int new_medium)
{
    for (int s = 0; s < USBDISK_UNITS; s++)
        if (V[s].used && V[s].unit == n)
            pub_slot(s, present && V[s].present, new_medium);
}
/* a slot for a volume of stick n: one it had before this bring-up first (the Mac keeps the drive), else a
   free one; -1 when PiDisk's units are all taken */
static int vol_take(int n, int kind)
{
    int s = -1;
    for (int k = 0; k < USBDISK_UNITS && s < 0; k++)
        if (V[k].used && V[k].unit == n && V[k].gen != U[n].gen)
            s = k;
    for (int k = 0; k < USBDISK_UNITS && s < 0; k++)
        if (!V[k].used)
            s = k;
    if (s < 0)
        return -1;
    struct vol *v = &V[s];
    mem_free(v->runs);
    memset(v, 0, sizeof *v);
    v->used = 1; v->unit = n; v->kind = kind; v->gen = U[n].gen;
    return s;
}
/* a stick's volumes that this bring-up did not offer again (all: every one, the stick is gone): gone
   from the Mac, their slots free */
static void vol_drop(int n, int all)
{
    for (int s = 0; s < USBDISK_UNITS; s++) {
        struct vol *v = &V[s];
        if (!v->used || v->unit != n || (!all && v->gen == U[n].gen))
            continue;
        if (P[s].i.present)
            pub_slot(s, 0, 0);
        mem_free(v->runs);
        memset(v, 0, sizeof *v);
    }
}
static void vol_name(struct vol *v, const char *a, const char *b)
{
    unsigned k = 0;
    while (*a && k < sizeof v->name - 1)
        v->name[k++] = *a++;
    while (b && *b && k < sizeof v->name - 1)
        v->name[k++] = *b++;
    v->name[k] = 0;
}
uint32_t psmac_usbdisk_changes(void) { return __atomic_load_n(&changes, __ATOMIC_ACQUIRE); }
int psmac_usbdisk_info(int n, struct usbdisk_info *out)
{
    if (n < 0 || n >= USBDISK_UNITS)
        return 0;
    uint32_t s1 = __atomic_load_n(&P[n].seq, __ATOMIC_ACQUIRE);
    if (s1 & 1u)
        return 0;
    memcpy(out, (const void *)&P[n].i, sizeof *out);
    __asm__ volatile("dmb ish" ::: "memory");
    return __atomic_load_n(&P[n].seq, __ATOMIC_ACQUIRE) == s1;
}
static const char *uname(const struct unit *u);
int psmac_usbdisk_owns(unsigned addr)
{
    for (int n = 0; n < USBDISK_UNITS; n++)
        if (U[n].state != U_EMPTY && U[n].state != U_GONE && U[n].d.addr == addr)
            return 1;
    return 0;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
/* an INQUIRY field without its padding */
static void field(char *dst, const uint8_t *src, unsigned n)
{
    unsigned k;
    for (k = 0; k < n; k++)
        dst[k] = (src[k] >= 32 && src[k] < 127) ? (char)src[k] : ' ';
    while (k && dst[k - 1] == ' ')
        k--;
    dst[k] = 0;
}
/* a USB string descriptor (UTF-16LE), as ASCII */
static void usb_string(const struct dev *d, uint8_t index, uint16_t lang, char *dst, unsigned max)
{
    uint8_t b[256];
    dst[0] = 0;
    if (!index)
        return;
    int n = get_desc(d, 3, index, lang, b, 255);
    if (n < 4 || b[1] != 3)
        return;
    if (b[0] < n)
        n = b[0];
    unsigned k = 0;
    for (int i = 2; i + 1 < n && k < max - 1; i += 2)
        dst[k++] = (b[i] >= 32 && b[i] < 127 && !b[i + 1]) ? (char)b[i] : '?';
    while (k && dst[k - 1] == ' ')
        k--;
    dst[k] = 0;
}
static const char *uname(const struct unit *u)
{
    return u->prod[0] ? u->prod : u->product[0] ? u->product : "USB disk";
}

/* ---- bulk-only transport --------------------------------------------------------------------------- */
static int clear_halt(struct unit *u, int in)
{
    struct dev *d = &u->d;
    uint8_t ep = in ? (uint8_t)(d->ep_in | 0x80u) : d->ep_out;
    uint8_t s[8] = { 0x02, 1, 0, 0, ep, 0, 0, 0 };    /* CLEAR_FEATURE(ENDPOINT_HALT) */
    if (in)
        d->tog_in = PID_DATA0;
    else
        d->tog_out = PID_DATA0;
    return ctl(d, s, 0, 0) < 0 ? -1 : 0;
}
static void reset_recovery(struct unit *u, const char *why)
{
    struct dev *d = &u->d;
    uint8_t s[8] = { 0x21, 0xff, 0, 0, d->itf, 0, 0, 0 };   /* Bulk-Only Mass Storage Reset */
    int r = ctl(d, s, 0, 0);
    int a = clear_halt(u, 1), b = clear_halt(u, 0);
    u->resets++;
    if (u->resets <= 5)
        kprintf("[USB] disk %d: %s: reset recovery (reset %s, endpoints %s)%s\n", (int)(u - U), why, r < 0 ? "refused" : "taken",
                a || b ? "not cleared" : "cleared", u->resets == 5 ? "; further ones are counted, not logged" : "");
}

/* One command: 0 GOOD, 1 CHECK CONDITION (or FAILED: the sense is then fetched by scsi()), negative:
   the transport failed, and has been recovered as far as it could be. in: data from the stick. */
static int bot(struct unit *u, const uint8_t *cdb, int cdblen, int in, uint8_t *data, uint32_t len, uint32_t timeout_ms)
{
    struct dev *d = &u->d;
    uint32_t tag = ++u->tag;
    int r;
    memset(cbw, 0, 31);
    put32le(cbw, 0x43425355u);                          /* 'USBC' */
    put32le(cbw + 4, tag);
    put32le(cbw + 8, len);
    cbw[12] = in ? 0x80 : 0;
    cbw[13] = d->lun;
    cbw[14] = (uint8_t)cdblen;
    memcpy(cbw + 15, cdb, (unsigned)cdblen);
    r = xfer(CH_OUT, d, 2, d->ep_out, 0, &d->tog_out, cbw, 31, 2000);
    if (r == E_STALL) {
        clear_halt(u, 1);
        clear_halt(u, 0);
        r = xfer(CH_OUT, d, 2, d->ep_out, 0, &d->tog_out, cbw, 31, 2000);
    }
    if (r != 31) {
        reset_recovery(u, "the command block did not go");
        return -1;
    }
    if (len) {
        r = in ? xfer(CH_IN, d, 2, d->ep_in, 1, &d->tog_in, data, len, timeout_ms)
               : xfer(CH_OUT, d, 2, d->ep_out, 0, &d->tog_out, data, len, timeout_ms);
        if (r == E_STALL)
            clear_halt(u, in);                          /* the stick refused the data: its CSW says why */
        else if (r < 0) {
            reset_recovery(u, r == E_TIMEOUT ? "no data in time" : "the data stage failed");
            return -2;
        } else if (in && r == 13 && len != 13 && le32(data) == 0x53425355u && le32(data + 4) == tag) {
            /* the stick sent its CSW in place of the data */
            return data[12] == 0 ? 0 : data[12] == 1 ? 1 : (reset_recovery(u, "phase error"), -3);
        }
    }
    for (int k = 0; k < 2; k++) {
        r = xfer(CH_IN, d, 2, d->ep_in, 1, &d->tog_in, csw, 13, timeout_ms > 2000 ? timeout_ms : 2000);
        if (r == E_STALL) {
            clear_halt(u, 1);
            continue;
        }
        if (r >= 0 && r < 13)
            continue;                                   /* a truncated CSW: read it again */
        break;
    }
    if (r != 13 || le32(csw) != 0x53425355u || le32(csw + 4) != tag) {
        reset_recovery(u, r < 0 ? "no status" : "a status that is not one");
        return -4;
    }
    if (csw[12] == 0)
        return 0;
    if (csw[12] == 1)
        return 1;
    reset_recovery(u, "phase error");
    return -5;
}
/* bot() and, on CHECK CONDITION, REQUEST SENSE: u->key, asc, ascq */
static int scsi(struct unit *u, const uint8_t *cdb, int cdblen, int in, uint8_t *data, uint32_t len, uint32_t timeout_ms)
{
    int r = bot(u, cdb, cdblen, in, data, len, timeout_ms);
    if (r != 1)
        return r;
    static const uint8_t rs[6] = { 0x03, 0, 0, 0, 18, 0 };
    memset(sense, 0, 18);
    u->key = u->asc = u->ascq = 0xff;
    if (bot(u, rs, 6, 1, sense, 18, 1000) == 0) {
        u->key = sense[2] & 15u;
        u->asc = sense[12];
        u->ascq = sense[13];
    }
    return 1;
}
static int rw10(struct unit *u, int write, uint32_t lba, uint32_t count, uint8_t *buf, uint32_t timeout_ms)
{
    uint8_t c[10] = { write ? 0x2a : 0x28, 0 };
    put32be(c + 2, lba);
    c[7] = (uint8_t)(count >> 8);
    c[8] = (uint8_t)count;
    return scsi(u, c, 10, !write, buf, count * 512u, timeout_ms);
}
/* The stick's own cache onto its flash: 0 when done (or when the stick has no cache to flush), else -1.
   Until 21 November 2026 a failure was only logged: the eject then said "safe to unplug" and the
   background flush forgot the writes were still in the stick's cache (an outside review, docs/08 item 92). */
static int sync_cache(struct unit *u, const char *when)
{
    static const uint8_t c[10] = { 0x35, 0 };
    if (u->no_sync)
        return 0;
    int r = scsi(u, c, 10, 0, 0, 0, 10000);
    if (r == 1 && u->key == 5) {
        u->no_sync = 1;                                 /* ILLEGAL REQUEST: a stick without a cache to flush */
        kprintf("[USB] disk %d: it does not take SYNCHRONIZE CACHE (%s): its writes are done when it says so\n", (int)(u - U), when);
        return 0;
    }
    if (!r) { u->sync_fails = 0; return 0; }
    if (u->sync_fails++ < 3)
        kprintf("[USB] disk %d: SYNCHRONIZE CACHE %s: %s (sense %x/%02x/%02x): its last writes may not be on the stick yet\n",
                (int)(u - U), when, r < 0 ? "no answer" : "refused", u->key, u->asc, u->ascq);
    return -1;
}

/* ---- finding the sticks (after an enumeration; core 3 keeps off the bus meanwhile) ------------------ */
static int count_on_bus;
static int free_unit(void)
{
    for (int n = 0; n < USBDISK_UNITS; n++)
        if (U[n].state == U_EMPTY)
            return n;
    for (int n = 0; n < USBDISK_UNITS; n++)
        if (U[n].state == U_GONE)
            return n;                                   /* a stick that went: its PiDisk unit takes the new one */
    return -1;
}

static void try_device(uint8_t addr, unsigned port, int high, int split, int changed)
{
    struct dev d = { .addr = addr, .port = (uint8_t)port, .hub = (uint8_t)(port ? 1 : 0), .split = (uint8_t)split, .mps0 = 64, .high = (uint8_t)high };
    uint8_t dd[18], c[512];
    char where[16] = "the root port";
    if (port) {
        unsigned k = 9;
        memcpy(where, "hub port ", 9);
        if (port >= 10)
            where[k++] = (char)('0' + port / 10);
        where[k++] = (char)('0' + port % 10);
        where[k] = 0;
    }
    /* the first eight bytes say how large endpoint 0's packets are (a full-speed device may take 8) */
    if (get_desc(&d, 1, 0, 0, dd, 8) != 8 || dd[1] != 1)
        return;
    d.mps0 = dd[7];
    if (d.mps0 != 8 && d.mps0 != 16 && d.mps0 != 32 && d.mps0 != 64)
        return;
    if (get_desc(&d, 1, 0, 0, dd, 18) != 18)
        return;
    d.vid = (uint16_t)(dd[8] | dd[9] << 8);
    d.pid = (uint16_t)(dd[10] | dd[11] << 8);
    if (dd[4] != 0 && dd[4] != 8)
        return;                                         /* a hub, the board's network chip (class $FF), ... */
    if (get_desc(&d, 2, 0, 0, c, 9) != 9 || c[1] != 2)
        return;
    unsigned total = (unsigned)(c[2] | c[3] << 8);
    if (total > sizeof c)
        total = sizeof c;
    if (get_desc(&d, 2, 0, 0, c, (uint16_t)total) != (int)total)
        return;
    int found = 0;
    uint8_t sub = 0, proto = 0, in = 0, out = 0;
    uint16_t mpsi = 0, mpso = 0;
    for (unsigned p = 0, cls = 0, alt = 0; p + 2 <= total && c[p] >= 2 && p + c[p] <= total; p += c[p]) {
        if (c[p + 1] == 4 && c[p] >= 9) {
            if (found && in && out)
                break;
            cls = c[p + 5]; alt = c[p + 3];
            if (cls == 8 && alt == 0 && !found) {
                found = 1; d.itf = c[p + 2]; sub = c[p + 6]; proto = c[p + 7];
            } else if (found)
                break;
        } else if (c[p + 1] == 5 && c[p] >= 7 && found && cls == 8 && alt == 0 && (c[p + 3] & 3u) == 2u) {
            if (c[p + 2] & 0x80u) { in = c[p + 2] & 15u; mpsi = (uint16_t)((c[p + 4] | c[p + 5] << 8) & 0x7ff); }
            else { out = c[p + 2] & 15u; mpso = (uint16_t)((c[p + 4] | c[p + 5] << 8) & 0x7ff); }
        }
    }
    if (!found)
        return;
    if (sub != 6 || proto != 0x50) {
        kprintf("[USB] %s: a mass storage device %04x:%04x of subclass %u, protocol $%02x: only SCSI over bulk-only transport (subclass 6, protocol $50) is spoken here; left alone\n",
                where, d.vid, d.pid, sub, proto);
        return;
    }
    if (!in || !out || !mpsi || mpsi != mpso || mpsi > 512 || (split && mpsi > 64)) {
        kprintf("[USB] %s: a USB disk %04x:%04x whose bulk endpoints this kernel cannot use (in %u, out %u, %u/%u bytes); left alone\n",
                where, d.vid, d.pid, in, out, mpsi, mpso);
        return;
    }
    d.ep_in = in; d.ep_out = out; d.mps = mpsi; d.cfg = c[5];
    count_on_bus++;
    /* who it is: the same stick again keeps its unit, and with it its PiDisk drive on the Mac */
    char mfr[32], prod[32], serial[40];
    uint8_t l[4] = { 0 };
    uint16_t lang = 0x0409;
    if (get_desc(&d, 3, 0, 0, l, 4) >= 4 && l[1] == 3)
        lang = (uint16_t)(l[2] | l[3] << 8);
    usb_string(&d, dd[14], lang, mfr, sizeof mfr);
    usb_string(&d, dd[15], lang, prod, sizeof prod);
    usb_string(&d, dd[16], lang, serial, sizeof serial);
    int n = -1;
    for (int k = 0; k < USBDISK_UNITS && n < 0; k++) {
        struct unit *u = &U[k];
        if (u->state != U_EMPTY && !u->seen && u->d.vid == d.vid && u->d.pid == d.pid && !strcmp(u->serial, serial) &&
            (serial[0] || u->d.port == port))
            n = k;
    }
    int same = n >= 0;
    if (!same && (n = free_unit()) < 0) {
        kprintf("[USB] %s: a USB disk %04x:%04x, but %d are in use already; left alone\n", where, d.vid, d.pid, USBDISK_UNITS);
        return;
    }
    struct unit *u = &U[n];
    int prev = same ? u->state : U_EMPTY;
    if (!same) {
        memset(u, 0, sizeof *u);
        memcpy(u->mfr, mfr, sizeof mfr);
        memcpy(u->prod, prod, sizeof prod);
        memcpy(u->serial, serial, sizeof serial);
    }
    u->d = d;
    u->seen = 1;
    uint8_t sc[8] = { 0, 9, d.cfg, 0, 0, 0, 0, 0 };     /* SET_CONFIGURATION */
    if (ctl(&d, sc, 0, 0) < 0) {
        kprintf("[USB] %s: USB disk %04x:%04x refused its configuration; left alone\n", where, d.vid, d.pid);
        u->state = U_FAILED;
        return;
    }
    /* GET_MAX_LUN: a stall means one LUN (the class specification allows it, many sticks do it) */
    uint8_t gm[8] = { 0xa1, 0xfe, 0, 0, d.itf, 0, 1, 0 }, ml = 0;
    if (ctl(&d, gm, &ml, 1) != 1 || ml > 15)
        ml = 0;
    u->d.maxlun = ml;
    u->d.lun = 0;
    u->d.tog_in = u->d.tog_out = PID_DATA0;
    /* what happens to it now: a stick the Mac has is checked again (a bus reset is no new disk);
       one that was ejected or refused stays so unless its port saw a plug (it was taken out and put
       back); anything else is brought up as new */
    u->again = 0;
    if (same && prev == U_UP && !changed)
        u->again = 1, u->state = U_CLAIMED;
    else if (same && (prev == U_EJECTED || prev == U_REFUSED || prev == U_FAILED) && !changed)
        u->state = prev;
    else
        u->state = U_CLAIMED;
    if (u->state == U_CLAIMED && !u->again)
        kprintf("[USB] disk %d on %s: %s%s%s (%04x:%04x), %s speed%s, LUN 0 of %u\n", n, where, mfr, mfr[0] ? " " : "", prod[0] ? prod : "(no name)",
                d.vid, d.pid, high ? "high" : "full", split ? " through the hub's translator (split transactions)" : "", ml + 1u);
    else if (u->state == U_EJECTED)
        kprintf("[USB] disk %d (%s) is still plugged in after the Mac ejected it: unplug it, and plug it in again to use it\n", n, uname(u));
}

static void gone(int n)
{
    struct unit *u = &U[n];
    if (u->state == U_UP || (u->state == U_CLAIMED && u->again)) {
        kprintf("[USB] disk %d (%s): unplugged while the Mac had it (%u blocks read, %u written)%s\n", n, uname(u), u->rblocks, u->wblocks,
                u->dirty ? "; its last writes may not have reached the flash" : "");
        publish(n, 0, 0);
    } else if (u->state == U_EJECTED)
        kprintf("[USB] disk %d (%s): unplugged after the Mac ejected it\n", n, uname(u));
    else if (u->state != U_GONE && u->state != U_EMPTY)
        kprintf("[USB] disk %d (%s): unplugged\n", n, uname(u));
    if (u->state != U_EMPTY)
        u->state = U_GONE;
    u->dirty = 0;
    u->again = 0;
    vol_drop(n, 1);
    mem_free(u->fx);
    u->fx = 0;
}

/* psmac_usb.c, core 1, after every enumeration (and once at start), before core 3 is let back onto
   the bus. changed: the hub ports that saw a plug or an unplug (bit n for port n; bit 0 the root
   port), all of them at the first enumeration. */
static int fifo_said;
void psmac_usbdisk_claim(unsigned hub_ports, int root_speed, int root_connected, int root_taken, int dma, uint32_t changed)
{
    if (mode_get() == MODE_OFF)
        return;
    if (!dma) {
        if (!fifo_said++)
            kprintf("[USB] disks: they need the DMA transport, and usb=pio has none: USB flash drives are left alone\n");
        return;
    }
    for (int n = 0; n < USBDISK_UNITS; n++)
        U[n].seen = 0;
    count_on_bus = 0;
    if (!hub_ports) {
        if (root_connected && !root_taken)
            try_device(1, 0, root_speed == 0, 0, (changed & 1u) != 0);
    } else {
        for (unsigned port = 1; port <= hub_ports && port < 32; port++) {
            uint32_t st = 0;
            if (hub_status(port, &st) || !(st & 1u) || (st & B(9)))
                continue;                               /* nothing there, or low speed */
            if (!(st & 2u) && address_port(port, &st) < 0)
                continue;
            int high = (st & B(10)) != 0;
            try_device((uint8_t)(1 + port), port, high, root_speed == 0 && !high, (changed >> port) & 1u);
        }
    }
    for (int n = 0; n < USBDISK_UNITS; n++)
        if (!U[n].seen && U[n].state != U_EMPTY && U[n].state != U_GONE)
            gone(n);
    seen_last = (uint32_t)count_on_bus;
}
uint32_t psmac_usbdisk_seen(void) { return seen_last; }

/* ---- bringing a stick up, and what it holds -------------------------------------------------------- */
static const char *mbr_type(uint8_t t)
{
    switch (t) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0e: return "FAT16";
    case 0x07: return "exFAT or NTFS";
    case 0x0b: case 0x0c: return "FAT32";
    case 0x0f: case 0x05: return "an extended partition";
    case 0x83: return "Linux";
    case 0xaf: return "Apple HFS or HFS Plus";
    case 0xa8: return "Apple UFS";
    case 0xee: return "GPT";
    default: return "a type this kernel does not know";
    }
}
static const char *gpt_type(uint32_t first_word)
{
    switch (first_word) {
    case 0xebd0a0a2u: return "Microsoft basic data (FAT, exFAT or NTFS)";
    case 0x48465300u: return "Apple HFS or HFS Plus";
    case 0x7c3457efu: return "APFS";
    case 0x0fc63dafu: return "Linux";
    case 0xc12a7328u: return "EFI system (FAT)";
    case 0: return "none";
    default: return "a type this kernel does not know";
    }
}
/* the volume signature in a volume's block 2: 'BD' HFS (System 7's), 'H+'/'HX' HFS Plus (Mac OS 8.1
   and later only); the volume's name from an HFS master directory block (drVN, offset 36) */
static int read_one(struct unit *u, uint32_t lba)
{
    if (lba >= u->dev_blocks)
        return -1;
    return rw10(u, 0, lba, 1, vbuf, 3000) == 0 ? 0 : -1;
}
static void hfs_name(const uint8_t *mdb, char *out)
{
    unsigned n = mdb[36] > 27 ? 27 : mdb[36], k;
    for (k = 0; k < n; k++)
        out[k] = (mdb[37 + k] >= 32 && mdb[37 + k] < 127) ? (char)mdb[37 + k] : '?';
    out[k] = 0;
}
#define HFS_MAX_BLOCKS 4194304u                 /* 2 GB: the largest HFS volume System 7 knows */

/* ---- FAT and exFAT sticks (issue #20) -------------------------------------------------------------- */
static int32_t phys_rw(struct unit *u, int n, int write, uint32_t lba, uint32_t count, uint8_t *buf);
static uint32_t dev_max_blocks(const struct dev *d);
/* psmac_fatfs.c's sectors: through phys_rw, in pieces the stick's bus takes, into the caller's buffer
   (64-byte aligned, as the DMA wants; any other through pbuf) */
static int usb_fx_read(void *ctx, uint32_t lba, uint32_t cnt, uint8_t *buf)
{
    struct unit *u = ctx;
    int n = (int)(u - U);
    uint32_t max = dev_max_blocks(&u->d);
    while (cnt) {
        uint32_t k = cnt < max ? cnt : max;
        uint8_t *dst = ((uintptr_t)buf & 63u) ? pbuf : buf;
        if (dst == pbuf && k > sizeof pbuf / 512u)
            k = sizeof pbuf / 512u;
        if (phys_rw(u, n, 0, lba, k, dst))
            return -1;
        if (dst != buf)
            memcpy(buf, pbuf, k * 512u);
        buf += k * 512u;
        lba += k;
        cnt -= k;
    }
    return 0;
}
/* blocks [b, b + count) of a file whose clusters are runs, read or written in place */
static int32_t runs_rw(struct unit *u, int n, const struct run *runs, uint32_t nruns, int write, uint32_t b, uint32_t count, uint8_t *buf)
{
    uint32_t spc = u->fx->spc;
    while (count) {
        uint32_t ci = b / spc, in = b % spc, k, plba = 0, avail = 0;
        for (k = 0; k < nruns; k++) {
            if (ci < runs[k].n) {
                plba = fx_clus_lba(u->fx, runs[k].clus + ci) + in;
                avail = (runs[k].n - ci) * spc - in;
                break;
            }
            ci -= runs[k].n;
        }
        if (k == nruns)
            return PSMB_E_RANGE;
        uint32_t m = count < avail ? count : avail;
        int32_t r = phys_rw(u, n, write, plba, m, buf);
        if (r)
            return r;
        buf += m * 512u;
        b += m;
        count -= m;
    }
    return 0;
}
static void ascii16(char *out, unsigned max, const uint16_t *s, int n)
{
    unsigned k = 0;
    for (int i = 0; i < n && k < max - 1; i++)
        out[k++] = s[i] >= 32 && s[i] < 127 ? (char)s[i] : '?';
    out[k] = 0;
}
/* the stick's FAT or exFAT volume, found and mounted: 1, or 0 when it has none */
static int fat_mount(struct unit *u, int n)
{
    uint32_t lba;
    int type;
    const char *what;
    if (!u->fx)
        u->fx = mem(sizeof *u->fx);
    if (!u->fx) {
        kprintf("[USB] disk %d: no memory to read its FAT\n", n);
        return 0;
    }
    if (!fx_probe(usb_fx_read, u, u->dev_blocks, u->fx, &lba, &type, &what)) {
        mem_free(u->fx);
        u->fx = 0;
        return 0;
    }
    int e = fx_mount(u->fx, usb_fx_read, u, lba);
    if (e) {
        kprintf("[USB] disk %d: %s %s at block %u, but its boot sector does not make sense (%d)\n", n, fx_type_name(type), what, lba, e);
        mem_free(u->fx);
        u->fx = 0;
        return 0;
    }
    char lab[16];
    ascii16(lab, sizeof lab, u->fx->label, u->fx->label_len);
    kprintf("[USB] disk %d: %s %s at block %u, %u clusters of %u KB%s%s%s\n", n, fx_type_name(u->fx->type), what, lba, u->fx->clusters,
            u->fx->spc / 2u, lab[0] ? ", label \"" : "", lab, lab[0] ? "\"" : "");
    return 1;
}
static int ends16(const struct fx_dirent *e, const char *x)
{
    int m = (int)strlen(x);
    if (e->nlen <= m)
        return 0;
    for (int i = 0; i < m; i++)
        if ((e->name[e->nlen - m + i] | 0x20) != (uint8_t)x[i])
            return 0;
    return 1;
}
static int is_image16(const struct fx_dirent *e)
{
    if (e->attr & FX_ATTR_DIR || e->size < 65536u || e->size >= 0x100000000ull || (e->nlen > 1 && e->name[0] == '.'))
        return 0;
    return ends16(e, ".image") || ends16(e, ".dsk") || ends16(e, ".hda") || ends16(e, ".img") || ends16(e, ".hfv");
}
static struct run *runs_tmp; static uint32_t runs_tmp_n;
static int runs_add(void *arg, uint32_t c, uint32_t k)
{
    (void)arg;
    if (runs_tmp_n >= 256)
        return -1;                              /* more pieces than a disk image should be in */
    runs_tmp[runs_tmp_n].clus = c;
    runs_tmp[runs_tmp_n++].n = k;
    return 0;
}
/* an image file on the stick, if it holds HFS: what of it the Mac is given (a bare HFS image whole, an
   Apple-partitioned one its first Apple_HFS partition, as the card's images): 1, or 0 */
static int image_hfs(struct unit *u, int n, const struct run *r, uint32_t nr, uint32_t blocks, uint32_t *first, uint32_t *count, char *vname)
{
    uint32_t have = blocks < 64 ? blocks : 64;
    if (runs_rw(u, n, r, nr, 0, 0, have, pbuf))
        return 0;
    const uint8_t *b = pbuf;
    *first = 0;
    *count = blocks;
    if (have >= 3 && b[0] == 'E' && b[1] == 'R' && b[512] == 'P' && b[513] == 'M') {
        uint32_t entries = be32(b + 516), found = 0;
        for (uint32_t i = 1; i <= entries && i < have && i <= 16 && !found; i++) {
            const uint8_t *e = b + 512 * i;
            char t[33];
            if (e[0] != 'P' || e[1] != 'M')
                break;
            field(t, e + 48, 32);
            uint32_t s0 = be32(e + 8), c0 = be32(e + 12);
            if (!strcmp(t, "Apple_HFS") && c0 && s0 + c0 <= blocks) {
                *first = s0;
                *count = c0;
                found = 1;
            }
        }
        if (!found)
            return 0;
    }
    if (runs_rw(u, n, r, nr, 0, *first + 2, 1, vbuf) || be16(vbuf) != 0x4244)
        return 0;
    hfs_name(vbuf, vname);
    return 1;
}
/* A FAT or exFAT stick: every disk image in its root that holds HFS becomes a volume of its own, in
   directory order, as many as PiDisk has units for. 1 when the Mac was offered something. */
static int fat_offer(int n)
{
    struct unit *u = &U[n];
    struct fx_dir *d = mem(sizeof *d);
    struct fx_dirent *e = mem(sizeof *e);
    int offered = 0;
    runs_tmp = mem(256 * sizeof *runs_tmp);
    if (!d || !e || !runs_tmp) {
        kprintf("[USB] disk %d: no memory to read its root folder\n", n);
        goto out;
    }
    fx_dir_open(d, u->fx, 0);
    int r;
    while ((r = fx_dir_next(d, e)) == 1) {
        if (!is_image16(e))
            continue;
        char fname[40], vname[32];
        uint32_t nc, first, count, blocks = (uint32_t)(e->size / 512u);
        ascii16(fname, sizeof fname, e->name, e->nlen);
        runs_tmp_n = 0;
        if (fx_runs(u->fx, e->clus, e->size, e->contig, runs_add, 0, &nc)) {
            kprintf("[USB] disk %d: disk image %s is in more than 256 pieces, or its chain is broken: not offered\n", n, fname);
            continue;
        }
        if (!image_hfs(u, n, runs_tmp, runs_tmp_n, blocks, &first, &count, vname)) {
            kprintf("[USB] disk %d: %s is not an HFS disk image: not offered\n", n, fname);
            continue;
        }
        int s = vol_take(n, VK_IMAGE);
        if (s < 0) {
            kprintf("[USB] disk %d: disk image %s: all %d PiDisk units are taken\n", n, fname, USBDISK_UNITS);
            break;
        }
        struct vol *v = &V[s];
        v->runs = mem(runs_tmp_n * sizeof *v->runs);
        if (!v->runs) {
            kprintf("[USB] disk %d: no memory for disk image %s\n", n, fname);
            vol_drop(n, 0);
            v->used = 0;
            break;
        }
        memcpy(v->runs, runs_tmp, runs_tmp_n * sizeof *v->runs);
        v->nruns = runs_tmp_n;
        v->first = first;
        v->count = count;
        v->present = 1;
        vol_name(v, "USB ", fname);
        kprintf("[USB] disk %d: disk image %s (%u MB, %u piece%s) holds the HFS volume \"%s\": offered to the Mac, %s\n", n, fname,
                blocks / 2048u, runs_tmp_n, runs_tmp_n == 1 ? "" : "s", vname, u->writable ? "writable" : "read-only");
        offered++;
    }
    if (r < 0)
        kprintf("[USB] disk %d: its root folder could not be read to the end\n", n);
out:
    mem_free(d);
    mem_free(e);
    mem_free(runs_tmp);
    runs_tmp = 0;
    if (!offered)
        kprintf("[USB] disk %d: no HFS disk image (.image, .dsk, .hda, .img, .hfv) in its root: nothing offered to the Mac\n", n);
    return offered;
}

/* what the stick holds (its first 64 blocks are in pbuf); sets u->first and u->count; 1 when the
   Mac is to be offered it, 2 for a FAT or exFAT stick (mounted in u->fx: its disk images are offered) */
static int classify(struct unit *u, int n, uint32_t have)
{
    const uint8_t *b = pbuf;
    char nm[32];
    u->first = 0;
    u->count = 0;
    if (have >= 3 && b[0] == 'E' && b[1] == 'R' && b[512] == 'P' && b[513] == 'M') {
        uint32_t entries = be32(b + 516);
        int hfs = -1, plus = 0;
        uint32_t st = 0, ct = 0;
        char list[160];
        unsigned k = 0;
        for (uint32_t i = 1; i <= entries && i < have && i <= 16; i++) {
            const uint8_t *e = b + 512 * i;
            if (e[0] != 'P' || e[1] != 'M')
                break;
            char t[33];
            field(t, e + 48, 32);
            for (const char *s = t; *s && k < sizeof list - 3; s++)
                list[k++] = *s;
            if (k < sizeof list - 3) { list[k++] = ','; list[k++] = ' '; }
            if (hfs < 0 && !strcmp(t, "Apple_HFS")) {
                uint32_t s0 = be32(e + 8), c0 = be32(e + 12);
                if (c0 && s0 + c0 <= u->dev_blocks) { hfs = (int)i; st = s0; ct = c0; }
            }
        }
        if (k >= 2)
            k -= 2;
        list[k] = 0;
        kprintf("[USB] disk %d: an Apple partition map, %u entries: %s\n", n, entries, list);
        if (hfs > 0) {
            if (read_one(u, st + 2) == 0 && be16(vbuf) == 0x4244) {
                hfs_name(vbuf, nm);
                kprintf("[USB] disk %d: its Apple_HFS partition (blocks %u-%u) holds the HFS volume \"%s\"\n", n, st, st + ct - 1, nm);
                u->first = st; u->count = ct;
                return 1;
            }
            plus = be16(vbuf) == 0x482b || be16(vbuf) == 0x4858;
            kprintf("[USB] disk %d: its Apple_HFS partition (blocks %u-%u) holds %s\n", n, st, st + ct - 1,
                    plus ? "an HFS Plus volume: Mac OS 8.1 and later read those, System 7 does not" : "no HFS volume");
            if (mode == MODE_ALL) {
                u->first = st; u->count = ct;
                return 1;
            }
            return 0;
        }
        kprintf("[USB] disk %d: no Apple_HFS partition in the map\n", n);
    } else if (have >= 3 && be16(b + 1024) == 0x4244) {
        hfs_name(b + 1024, nm);
        kprintf("[USB] disk %d: a bare HFS volume \"%s\" (no partition map)\n", n, nm);
        u->count = u->dev_blocks;
        return 1;
    } else if (have >= 3 && (be16(b + 1024) == 0x482b || be16(b + 1024) == 0x4858)) {
        kprintf("[USB] disk %d: a bare HFS Plus volume: Mac OS 8.1 and later read those, System 7 does not\n", n);
    } else if (b[510] == 0x55 && b[511] == 0xaa && (b[0] == 0xeb || b[0] == 0xe9) &&
               (!memcmp(b + 3, "EXFAT   ", 8) || !memcmp(b + 3, "NTFS    ", 8) || !memcmp(b + 54, "FAT", 3) || !memcmp(b + 82, "FAT32", 5))) {
        kprintf("[USB] disk %d: a %s volume with no partition table\n", n,
                !memcmp(b + 3, "EXFAT   ", 8) ? "exFAT" : !memcmp(b + 3, "NTFS    ", 8) ? "NTFS" : !memcmp(b + 82, "FAT32", 5) ? "FAT32" : "FAT12 or FAT16");
    } else if (b[510] == 0x55 && b[511] == 0xaa) {
        const uint8_t *e = 0;
        for (int i = 0; i < 4 && !e; i++)
            if (b[446 + 16 * i + 4])
                e = b + 446 + 16 * i;
        if (!e)
            kprintf("[USB] disk %d: an MBR partition table with no partitions\n", n);
        else if (e[4] == 0xee && have >= 2 && !memcmp(b + 512, "EFI PART", 8)) {
            uint32_t elba = le32(b + 512 + 72), esz = le32(b + 512 + 84);
            const uint8_t *g = (elba < have && esz >= 128 && esz <= 512) ? b + 512 * elba : 0;
            kprintf("[USB] disk %d: a GPT partition table, first partition %s%s%s\n", n, g ? gpt_type(le32(g)) : "(not read)",
                    g && le32(g + esz) ? ", second " : "", g && le32(g + esz) ? gpt_type(le32(g + esz)) : "");
            for (int i = 0; g && i < 4; i++) {
                const uint8_t *p = g + esz * (unsigned)i;
                if ((uintptr_t)(p + 48 - pbuf) > have * 512u || le32(p) != 0x48465300u)
                    continue;
                uint32_t st = le32(p + 32), last = le32(p + 40);
                if (le32(p + 36) || le32(p + 44) || last < st)
                    continue;                           /* beyond 2 TB */
                if (read_one(u, st + 2) == 0 && be16(vbuf) == 0x4244) {
                    hfs_name(vbuf, nm);
                    kprintf("[USB] disk %d: its partition %d (blocks %u-%u) holds the HFS volume \"%s\"\n", n, i + 1, st, last, nm);
                    u->first = st; u->count = last - st + 1;
                    return 1;
                }
                if (be16(vbuf) == 0x482b || be16(vbuf) == 0x4858)
                    kprintf("[USB] disk %d: its partition %d holds an HFS Plus volume: Mac OS 8.1 and later read those, System 7 does not\n", n, i + 1);
            }
        } else {
            uint32_t st = le32(e + 8), ct = le32(e + 12);
            kprintf("[USB] disk %d: an MBR partition table, first partition %s (type $%02x, blocks %u-%u)\n", n, mbr_type(e[4]), e[4], st, st + ct - 1);
            if (e[4] == 0xaf && ct && st + ct <= u->dev_blocks && read_one(u, st + 2) == 0) {
                if (be16(vbuf) == 0x4244) {
                    hfs_name(vbuf, nm);
                    kprintf("[USB] disk %d: it holds the HFS volume \"%s\"\n", n, nm);
                    u->first = st; u->count = ct;
                    return 1;
                }
                if (be16(vbuf) == 0x482b || be16(vbuf) == 0x4858)
                    kprintf("[USB] disk %d: it holds an HFS Plus volume: Mac OS 8.1 and later read those, System 7 does not\n", n);
            }
        }
    } else {
        int blank = 1;
        for (uint32_t i = 0; i < have * 512u && blank; i++)
            if (b[i])
                blank = 0;
        kprintf("[USB] disk %d: %s\n", n, blank ? "blank (its first blocks are all zero: no partition table, no volume)" :
                                                   "no partition table and no volume this kernel knows");
    }
    if (mode != MODE_ALL && fat_mount(u, n))
        return 2;                               /* FAT or exFAT: its disk images (fat_offer) */
    if (mode == MODE_ALL) {
        u->count = u->dev_blocks > HFS_MAX_BLOCKS ? HFS_MAX_BLOCKS : u->dev_blocks;
        kprintf("[USB] disk %d: offered to the Mac all the same (mac.usbdisk=all), as %s: the Finder may offer to initialize it, which erases it\n",
                n, u->count < u->dev_blocks ? "its first 2 GB" : "the whole stick");
        return 1;
    }
    kprintf("[USB] disk %d: not offered to the Mac: System 7 mounts HFS disks only (FAT needs PC Exchange); mac.usbdisk=all offers it anyway\n", n);
    return 0;
}

static void bring_up(int n)
{
    struct unit *u = &U[n];
    static const uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 }, tur[6] = { 0 };
    int r = -1, again = u->again;
    u->again = 0;
    if (!again) {
        u->gen++;                               /* its volumes are taken afresh (the same slots first) */
        mem_free(u->fx);
        u->fx = 0;
    }
    for (int k = 0; k < 3 && r; k++) {
        r = scsi(u, inq, 6, 1, pbuf, 36, 2000);
        if (r)
            wait_us(100000);
    }
    if (r) {
        kprintf("[USB] disk %d: INQUIRY %s: left alone\n", n, r < 0 ? "not answered" : "refused");
        u->state = U_FAILED;
        if (again)
            publish(n, 0, 0);
        return;
    }
    field(u->vendor, pbuf + 8, 8);
    field(u->product, pbuf + 16, 16);
    field(u->rev, pbuf + 32, 4);
    u->removable = (pbuf[1] & 0x80u) != 0;
    if ((pbuf[0] >> 5) != 0 || ((pbuf[0] & 31u) != 0 && (pbuf[0] & 31u) != 0x0e)) {
        static const char *const what[] = { "a disk", "a tape", "a printer", "a processor", "a write-once drive", "a CD-ROM or DVD drive", "a scanner", "an optical drive" };
        unsigned t = pbuf[0] & 31u;
        kprintf("[USB] disk %d: \"%s %s\" is %s (SCSI peripheral type %u), not a disk: left alone\n", n, u->vendor, u->product,
                t < 8 ? what[t] : "not a disk", t);
        u->state = U_REFUSED;
        return;
    }
    /* ready? a unit attention after the reset is normal; "becoming ready" is waited for; "medium not
       present" is a card reader without a card, looked at again every two seconds */
    uint32_t t0 = ms();
    for (;;) {
        r = scsi(u, tur, 6, 0, 0, 0, 2000);
        if (r == 0)
            break;
        if (r == 1 && u->key == 2 && u->asc == 0x3a) {
            if (!u->said_nomedium++)
                kprintf("[USB] disk %d (%s %s): no medium in it; looked into every two seconds%s\n", n, u->vendor, u->product, again ? "; the Mac is told the disk is gone" : "");
            u->state = U_NOMEDIUM;
            u->next_try = ms() + 2000;
            if (again)
                publish(n, 0, 0);
            return;
        }
        if (ms() - t0 > 5000) {
            kprintf("[USB] disk %d (%s %s): not ready after 5 s (sense %x/%02x/%02x); looked at again in 2 s\n", n, u->vendor, u->product, u->key, u->asc, u->ascq);
            u->state = U_NOMEDIUM;
            u->next_try = ms() + 2000;
            if (again)
                publish(n, 0, 0);
            return;
        }
        wait_us(r == 1 && u->key == 6 ? 10000 : 200000);
    }
    /* how large, in blocks of what size */
    static const uint8_t rc10[10] = { 0x25, 0 };
    r = -1;
    for (int k = 0; k < 3 && r; k++)
        r = scsi(u, rc10, 10, 1, pbuf, 8, 2000);
    if (r) {
        kprintf("[USB] disk %d (%s %s): READ CAPACITY %s: left alone\n", n, u->vendor, u->product, r < 0 ? "not answered" : "refused");
        u->state = U_FAILED;
        if (again)
            publish(n, 0, 0);
        return;
    }
    uint32_t last = be32(pbuf), bs = be32(pbuf + 4), blocks;
    if (last == 0xffffffffu) {
        uint8_t rc16[16] = { 0x9e, 0x10, 0 };
        rc16[13] = 32;
        if (scsi(u, rc16, 16, 1, pbuf, 32, 2000) == 0) {
            bs = be32(pbuf + 8);
            last = be32(pbuf) ? 0xfffffffeu : be32(pbuf + 4);
            if (be32(pbuf))
                kprintf("[USB] disk %d: larger than 2 TB: its first 2 TB are used\n", n);
        }
    }
    blocks = last + 1;
    if (bs != 512) {
        kprintf("[USB] disk %d (%s %s): %u-byte sectors: PiDisk serves 512-byte blocks only, so it is not offered to the Mac\n", n, u->vendor, u->product, bs);
        u->state = U_REFUSED;
        if (again)
            publish(n, 0, 0);
        return;
    }
    /* the write-protect switch (MODE SENSE's device-specific byte, bit 7); a stick that refuses the
       command is taken as writable, and a write it refuses later says so */
    static const uint8_t ms6[6] = { 0x1a, 0, 0x3f, 0, 192, 0 };
    int wp = 0;
    if (scsi(u, ms6, 6, 1, pbuf, 192, 2000) == 0)
        wp = (pbuf[2] & 0x80u) != 0;
    if (again && blocks == u->dev_blocks && (uint32_t)!wp == u->writable) {
        u->state = U_UP;
        kprintf("[USB] disk %d (%s): answers again after the bus was reset; the Mac's disk carries on\n", n, uname(u));
        return;
    }
    u->dev_blocks = blocks;
    u->writable = !wp;
    {
        uint32_t mb = blocks / 2048u;
        char size[16];
        unsigned k = 0, v = mb < 1024u ? mb : mb / 1024u;
        char digits[8];
        unsigned nd = 0;
        do {
            digits[nd++] = (char)('0' + v % 10u);
            v /= 10u;
        } while (v && nd < sizeof digits);
        while (nd)
            size[k++] = digits[--nd];
        if (mb >= 1024u) {
            size[k++] = '.';
            size[k++] = (char)('0' + mb % 1024u * 10u / 1024u);
        }
        memcpy(size + k, mb < 1024u ? " MB" : " GB", 4);
        kprintf("[USB] disk %d: \"%s %s %s\", %u blocks of 512 bytes (%s)%s%s\n", n, u->vendor, u->product, u->rev, blocks, size,
                wp ? ", write-protected" : ", writable", u->removable ? ", removable medium" : "");
    }
    uint32_t have = blocks < 64 ? blocks : 64;
    if (rw10(u, 0, 0, have, pbuf, 5000)) {
        kprintf("[USB] disk %d: its first blocks could not be read (sense %x/%02x/%02x): left alone\n", n, u->key, u->asc, u->ascq);
        u->state = U_FAILED;
        if (again)
            publish(n, 0, 0);
        return;
    }
    int kind = classify(u, n, have);
    if (!kind) {
        u->state = U_REFUSED;
        if (again)
            publish(n, 0, 0);
        vol_drop(n, 0);
        return;
    }
    if (kind == 2) {
        u->said_nomedium = 0;
        u->asked_reset = 0;
        u->state = fat_offer(n) ? U_UP : U_REFUSED;
        vol_drop(n, 0);
        publish(n, 1, 1);
        return;
    }
    u->state = U_UP;
    u->said_nomedium = 0;
    u->asked_reset = 0;
    int s = vol_take(n, VK_RAW);
    if (s < 0) {
        kprintf("[USB] disk %d: all %d PiDisk units are taken: not offered to the Mac\n", n, USBDISK_UNITS);
        u->state = U_REFUSED;
        return;
    }
    V[s].first = u->first;
    V[s].count = u->count;
    V[s].present = 1;
    vol_name(&V[s], "USB ", uname(u));
    vol_drop(n, 0);
    kprintf("[USB] disk %d: offered to the Mac: %u blocks (%u MB) from block %u, %s\n", n, u->count, u->count / 2048u, u->first,
            u->writable ? "writable" : "read-only");
    pub_slot(s, 1, 1);
}

/* ---- the job slot ---------------------------------------------------------------------------------- */
enum { J_IDLE, J_POSTED, J_RUNNING, J_DONE };
static struct { volatile int state; int unit, op; uint32_t media, lba, count; int32_t status; } J;
uint8_t *psmac_usbdisk_buffer(void) { return jbuf; }
static uint32_t dev_max_blocks(const struct dev *d)
{
    if (d->split)
        return 16;                                      /* 128 packets, one at a time: 8 KB */
    return d->mps >= 512 ? USBDISK_BUF / 512u : 64;     /* high speed: 128 packets; full: 512 (the channel takes 1023) */
}
uint32_t psmac_usbdisk_max_blocks(int s)
{
    if (s < 0 || s >= USBDISK_UNITS || !V[s].used)
        return 1;
    return dev_max_blocks(&U[V[s].unit].d);
}
int psmac_usbdisk_post(int n, uint32_t media, int op, uint32_t lba, uint32_t count)
{
    if (__atomic_load_n(&J.state, __ATOMIC_ACQUIRE) != J_IDLE)
        return -1;
    J.unit = n; J.media = media; J.op = op; J.lba = lba; J.count = count; J.status = 0;
    __atomic_store_n(&J.state, J_POSTED, __ATOMIC_RELEASE);
    __asm__ volatile("dsb ish; sev" ::: "memory");      /* core 1 waits in wfe between jobs */
    return 0;
}
int psmac_usbdisk_result(int32_t *status)
{
    int s = __atomic_load_n(&J.state, __ATOMIC_ACQUIRE);
    if (s == J_IDLE)
        return -1;
    if (s != J_DONE)
        return 0;
    *status = J.status;
    __atomic_store_n(&J.state, J_IDLE, __ATOMIC_RELEASE);
    return 1;
}

/* count blocks at the stick's block lba, with the class's recovery: 0 or PSMB_E_* */
static int32_t phys_rw(struct unit *u, int n, int write, uint32_t lba, uint32_t count, uint8_t *buf)
{
    int transport = 0;
    if ((uint64_t)lba + count > u->dev_blocks)
        return PSMB_E_RANGE;
    if (write && !u->writable)
        return PSMB_E_RO;
    for (int attempt = 0; attempt < 3; attempt++) {
        int r = rw10(u, write, lba, count, buf, 10000);
        if (r == 0) {
            if (write) { u->writes++; u->wblocks += count; u->dirty = 1; u->last_write = ms(); }
            else { u->reads++; u->rblocks += count; }
            return 0;
        }
        if (r < 0) {
            transport++;
            continue;
        }
        if (u->key == 6)
            continue;                                   /* UNIT ATTENTION: once more */
        if (u->key == 2 && u->asc == 0x3a) {
            kprintf("[USB] disk %d (%s): the medium is gone; the Mac is told the disk is gone\n", n, uname(u));
            u->state = U_NOMEDIUM;
            u->next_try = ms() + 2000;
            publish(n, 0, 0);
            return PSMB_E_NODATA;
        }
        if (u->key == 2 && u->asc == 0x04) {
            wait_us(200000);                            /* becoming ready */
            continue;
        }
        if (u->key == 7) {
            kprintf("[USB] disk %d (%s): write-protected (the switch on the stick?): the write was refused\n", n, uname(u));
            u->writable = 0;
            publish(n, 1, 0);
            return PSMB_E_RO;
        }
        break;
    }
    u->errors++;
    if (u->errors <= 10)
        kprintf("[USB] disk %d: %s of %u blocks at %u failed (%s, sense %x/%02x/%02x)\n", n, write ? "write" : "read", count, lba,
                transport ? "the stick did not answer" : "refused", u->key, u->asc, u->ascq);
    /* a stick that stopped answering altogether: one fresh enumeration of the bus, which resets it */
    if (transport >= 3 && !u->asked_reset) {
        u->asked_reset = 1;
        psmac_usb_ask_enumeration("a USB disk stopped answering");
    }
    return PSMB_E_IO;
}
/* the Mac's read or write of volume s, in jbuf */
static int32_t vol_rw(int s, int write)
{
    struct vol *v = &V[s];
    struct unit *u = &U[v->unit];
    uint32_t lba = J.lba, count = J.count;
    if (count > dev_max_blocks(&u->d) || (uint64_t)lba + count > v->count)
        return PSMB_E_RANGE;
    if (write && !u->writable)
        return PSMB_E_RO;
    if (v->kind == VK_IMAGE)
        return runs_rw(u, v->unit, v->runs, v->nruns, write, v->first + lba, count, jbuf);
    return phys_rw(u, v->unit, write, v->first + lba, count, jbuf);
}
static void run_job(void)
{
    int s = J.unit;
    int32_t st;
    __atomic_store_n(&J.state, J_RUNNING, __ATOMIC_RELAXED);
    struct vol *v = (s >= 0 && s < USBDISK_UNITS && V[s].used) ? &V[s] : 0;
    int n = v ? v->unit : 0;
    struct unit *u = v ? &U[n] : 0;
    if (!u || u->state != U_UP || !v->present || P[s].i.media != J.media)
        st = PSMB_E_NODATA;                             /* the stick went (or another is in its place) */
    else if (J.op == USBDISK_OP_READ)
        st = vol_rw(s, 0);
    else if (J.op == USBDISK_OP_WRITE)
        st = vol_rw(s, 1);
    else if (J.op == USBDISK_OP_EJECT) {
        /* a stick whose cache would not flush stays mounted, and the Mac's eject fails: its writes may
           still be only in the stick's cache, so it is not safe to unplug */
        if (u->dirty && sync_cache(u, "at the eject")) {
            kprintf("[USB] disk %d (%s): not ejected: the stick did not confirm its writes; NOT safe to unplug\n", n, uname(u));
            st = PSMB_E_IO;
        } else {
            /* one volume of the stick ejected; the stick is safe to unplug once none is left on the Mac */
            int left = 0;
            u->dirty = 0;
            v->present = 0;
            pub_slot(s, 0, 0);
            for (int k = 0; k < USBDISK_UNITS; k++)
                if (V[k].used && V[k].unit == n && V[k].present)
                    left++;
            if (!left) {
                u->state = U_EJECTED;
                kprintf("[USB] disk %d (%s): ejected by the Mac (%u blocks read, %u written): safe to unplug\n", n, uname(u), u->rblocks, u->wblocks);
            } else
                kprintf("[USB] disk %d (%s): %s ejected by the Mac; %d more of its volumes are on the Mac: not yet safe to unplug\n", n, uname(u), v->name, left);
            st = 0;
        }
    } else
        st = PSMB_E_BADCMD;
    J.status = st;
    __atomic_store_n(&J.state, J_DONE, __ATOMIC_RELEASE);
}

/* core 1, between enumerations: a stick to bring up, the Mac's job, a cache to flush, a card reader
   to look into again. 1 when something was done. */
int psmac_usbdisk_core1(void)
{
    if (mode_get() == MODE_OFF)
        return 0;
    for (int n = 0; n < USBDISK_UNITS; n++)
        if (U[n].state == U_CLAIMED) {
            bring_up(n);
            return 1;
        }
    if (__atomic_load_n(&J.state, __ATOMIC_ACQUIRE) == J_POSTED) {
        run_job();
        return 1;
    }
    uint32_t t = ms();
    for (int n = 0; n < USBDISK_UNITS; n++) {
        struct unit *u = &U[n];
        if (u->state == U_UP && u->dirty && t - u->last_write >= 500) {
            if (sync_cache(u, "after writes"))
                u->last_write = t;                      /* still dirty: tried again half a second on */
            else
                u->dirty = 0;
            return 1;
        }
        if (u->state == U_NOMEDIUM && (int32_t)(t - u->next_try) >= 0) {
            static const uint8_t tur[6] = { 0 };
            u->next_try = t + 2000;
            if (scsi(u, tur, 6, 0, 0, 0, 2000) == 0 || (u->key == 6 && scsi(u, tur, 6, 0, 0, 0, 2000) == 0))
                u->state = U_CLAIMED;                   /* a card went in: bring it up on the next turn */
            return 1;
        }
    }
    return 0;
}

/* the heartbeat (core 3, every five seconds): a line per stick every thirty */
void psmac_usbdisk_report(void)
{
    static unsigned beat;
    if (beat++ % 6)
        return;
    for (int n = 0; n < USBDISK_UNITS; n++) {
        const struct unit *u = &U[n];
        if (u->state == U_EMPTY)
            continue;
        kprintf("[USB]   disk %d (%s): %s, %u reads (%u blocks), %u writes (%u blocks), %u errors, %u resets\n", n, uname(u),
                state_name[u->state], u->reads, u->rblocks, u->writes, u->wblocks, u->errors, u->resets);
    }
}
