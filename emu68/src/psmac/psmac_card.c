/*
 * psmac_card.c — the Pi's own SD card, written by the kernel. MPL-2.0.
 *
 * Two things a bench Pi needs to keep across a power cycle: its Bluetooth pairings, and what the
 * Mac wrote to the disk image it booted from. Both live on the boot card, which the firmware read
 * and then left alone. This file drives the card through the BCM2835's "sdhost" controller on
 * GPIO 48-53 (ALT0), the way Linux does on a Pi 3: the other SD controller, the Arasan, belongs
 * to the WiFi chip on GPIO 34-39 (alice/wifi/sdio.c). The register layout and the reset and
 * clock sequences follow Linux's bcm2835-sdhost driver; the card protocol is the SD Physical
 * Layer Simplified Specification (CMD0, CMD8, ACMD41, CMD2, CMD3, CMD7, ACMD6, CMD17, CMD24,
 * CMD13).
 *
 * The filesystem side is deliberately small. Nothing on the card is ever moved, shrunk or grown
 * except one file of the kernel's own:
 *   pairings.txt   Bluetooth pairings as bt.dev= lines (bt_pi.c reads them at start and hands
 *                  every change back). Created in the root directory, in one cluster, if absent.
 *   aurora68.cpio  the container the firmware loaded (mac.container= for another name). A disk
 *                  image inside it is written back sector by sector at its own offset, so the
 *                  next boot loads what the Mac wrote. Only after the file's size and first
 *                  sector have been checked against the copy in memory.
 *   aurora68.log   the kernel's log, on the Mac's request (PiLog's Save to Card) and after a crash
 *                  record: made once the size of the ring, rewritten in place (below).
 *   cmdline.txt    edited only on the Mac's request (PSMB_INFO_SETOPT: the WiFi menu saves the
 *                  network it joined), and carefully: one line of options, the requested ones
 *                  dropped or replaced, every other byte kept, written only when the result fits
 *                  the file's first cluster, and read back. A mistake here must not stop the Pi
 *                  booting. config.txt is never written.
 *   aurora68-display.txt, aurora68-display.bak
 *                  the HDMI mode PiDisplay chose (psmac_hdmi.c, docs/10 "PiDisplay"): small files of the
 *                  kernel's own, which config.txt reads through its "include aurora68-display.txt" line.
 *                  Made with long names when absent, written in their first cluster, read back.
 *                  config.txt itself is only read.
 *   Mac Exchange/  a folder (mac.exchange=NAME for another, =off for none) whose files the Mac sees as a
 *                  locked HFS volume, read only (21 November 2026, issue #20; exchange_* below): made by
 *                  psmac_hfsview.c from what psmac_fatfs.c reads, a step per pass of core 3, its files'
 *                  blocks read from the card when the Mac asks for them.
 *   *.declrom      read, never written: a declaration ROM in the root is offered to psmac_bus.c
 *                  before the 68k starts, beside the container's, and wins a tie (20 November
 *                  2026, docs/10 "Declaration ROMs from the card"): a new Mac-side driver is then a
 *                  file copied to the card, not a container rebuilt. That one look happens on core 0
 *                  (psmac_card_declroms), the only time this file runs anywhere but core 3.
 * FAT16 and FAT32, 512-byte sectors, the first FAT partition of an MBR card (or a card with no
 * partition table). Options: mac.card=off (do not touch the card), mac.card=ro (read only:
 * pairings are loaded, nothing is written).
 *
 * Everything runs on core 3 (psmac_alice_core), in small steps: one sector per pass at most, the
 * card's programming time waited for between passes, so the Bluetooth UART beside it (no flow
 * control on a Pi 3B) and the second display's tick never wait long. Written 18 November 2026.
 */
#include <stdint.h>
#include "support.h"
#include "tlsf.h"
#include "spinlock.h"
#include "psmac.h"
#include "psmb.h"
#include "psmac_fatfs.h"
#include "psmac_hfsview.h"
#include "psmac_recovery.h"

extern const char *psmac_option(const char *key);
extern int alice_pi_is_2711(void);
extern spinlock_t psmac_gpfsel_lock;
void sprintf(char *buf, const char *format, ...);        /* support.c */

#define PERI     PSMAC_PERIPH_HI
#define GPIO     (PERI + 0x200000u)
#define SDH      (PERI + 0x202000u)
#define SDCMD    0x00
#define SDARG    0x04
#define SDTOUT   0x08
#define SDCDIV   0x0c
#define SDRSP0   0x10
#define SDHSTS   0x20
#define SDVDD    0x30
#define SDEDM    0x34
#define SDHCFG   0x38
#define SDHBCT   0x3c
#define SDDATA   0x40
#define SDHBLC   0x50
#define CMD_NEW      0x8000u
#define CMD_FAIL     0x4000u
#define CMD_BUSY     0x0800u
#define CMD_NORSP    0x0400u
#define CMD_LONG     0x0200u
#define CMD_WRITE    0x0080u
#define CMD_READ     0x0040u
#define HSTS_ERRORS  0x00f8u
#define HSTS_CLEAR   0x07f8u
#define HSTS_CRC7    0x0010u
#define HCFG_BUSY_EN 0x0400u
#define HCFG_SLOW    0x0008u
#define HCFG_WIDE_EXT 0x0004u
#define HCFG_WIDE_INT 0x0002u
#define EDM_FSM(e)   ((e) & 0xfu)
#define EDM_FILL(e)  (((e) >> 4) & 0x1fu)
#define FSM_IDENT    0x0u
#define FSM_DATA     0x1u

static inline uint32_t r(uint32_t o) { return rd32le(SDH + o); }
static inline void w(uint32_t o, uint32_t v) { wr32le(SDH + o, v); }
static inline uint64_t cnt(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t frq(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static uint32_t ms(void) { return (uint32_t)(cnt() / (frq() / 1000u)); }
static void udelay(uint32_t us) { uint64_t e = cnt() + frq() * us / 1000000u; while (cnt() < e) ; }

/* ==== the controller and the card ============================================================ */
static struct {
    int state;                      /* 0 not started, 1 ready, -1 no card / failed, -2 off */
    uint32_t rca, ccs, last_hsts;   /* the card's address; block addressing (SDHC/SDXC) */
    uint32_t errors, reads, writes;
    int ro;
} sd;

static int sd_cmd(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t *rsp)
{
    uint64_t t = cnt();
    while (r(SDCMD) & CMD_NEW) if (cnt() - t > frq() / 20) { sd.errors++; return -1; }
    uint32_t h = r(SDHSTS);
    if (h & HSTS_CLEAR) w(SDHSTS, h & HSTS_CLEAR);
    w(SDARG, arg);
    w(SDCMD, (idx & 0x3fu) | flags | CMD_NEW);
    t = cnt();
    uint64_t lim = (flags & CMD_BUSY) ? frq() : frq() / 10;
    while (r(SDCMD) & CMD_NEW) if (cnt() - t > lim) { sd.errors++; w(SDCMD, 0); return -2; }
    if (r(SDCMD) & CMD_FAIL) {
        h = r(SDHSTS); sd.last_hsts = h;
        w(SDHSTS, h & HSTS_CLEAR);
        /* R3 (ACMD41) and R2 carry no CRC7 the controller can check: that "error" is expected */
        if (!((h & HSTS_ERRORS) == HSTS_CRC7 && (idx == 41 || (flags & CMD_LONG)))) { sd.errors++; return -3; }
    }
    if (rsp) { rsp[0] = r(SDRSP0); if (flags & CMD_LONG) { rsp[1] = r(SDRSP0 + 4); rsp[2] = r(SDRSP0 + 8); rsp[3] = r(SDRSP0 + 12); } }
    return 0;
}
static int sd_acmd(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t *rsp)
{
    uint32_t r1;
    if (sd_cmd(55, sd.rca << 16, 0, &r1)) return -1;
    return sd_cmd(idx, arg, flags, rsp);
}
static void sd_pins(void)
{
    spinlock_acquire(&psmac_gpfsel_lock);
    uint32_t f4 = rd32le(GPIO + 0x10), f5 = rd32le(GPIO + 0x14);
    f4 = (f4 & ~(077u << 24)) | (044u << 24);                  /* GPIO 48, 49: ALT0 (SD0 CLK, CMD) */
    f5 = (f5 & ~07777u) | 04444u;                              /* GPIO 50-53: ALT0 (SD0 DAT0-3) */
    wr32le(GPIO + 0x10, f4); wr32le(GPIO + 0x14, f5);
    spinlock_release(&psmac_gpfsel_lock);
}
/* The core clock (the sdhost's input), asked once and kept. support_rpi.c's get_clock_rate waits for
   the shared mailbox lock and for the answer without a limit; on core 3 a wait like that is a wait of
   the whole service loop. Asked here through alice_net.c's single bounded call instead (-9: another
   core holds the mailbox for a moment, so again a little later). Without an answer, 400 MHz, a Pi 3's
   core_freq: a slower card clock if the truth is 250 MHz, never a faster one. */
static uint32_t core_hz;
static uint32_t sd_core_clock(void)
{
    if (core_hz) return core_hz;
    extern int alice_mbox_once(uint32_t tag, uint32_t a, uint32_t b, uint32_t out[2]);
    uint32_t o[2] = { 0, 0 };
    int e = -9;
    for (int i = 0; i < 20 && e == -9; i++) {
        e = alice_mbox_once(0x00030002u, 4u, 0u, o);    /* GET_CLOCK_RATE: CORE */
        if (e == -9) udelay(5000);
    }
    if (!e && o[1] >= 100000000u) core_hz = o[1];
    else { core_hz = 400000000u; kprintf("[CARD] the firmware did not give the core clock (%d): 400 MHz assumed\n", e); }
    return core_hz;
}
static void sd_clock(uint32_t hz)
{
    uint32_t core = sd_core_clock();
    uint32_t div = core / hz; div = div > 2 ? div - 2 : 0;
    if (core / (div + 2) > hz) div++;
    if (div > 0x7ff) div = 0x7ff;
    w(SDCDIV, div);
}
/* mac.cardfail=early|all (testbed): the card does not answer, as a missing or broken one does (the same
   1.5 s of ACMD41 first): "early" only for the early look on core 0, which card_start then retries,
   "all" for every start. The boot must go on without the card's files. */
static int early_phase;                     /* psmac_card_declroms is running (core 0, before core 3 starts) */
static int card_fail_wanted(void)
{
    const char *o = psmac_option("mac.cardfail");
    if (!o) return 0;
    if (o[0] == 'a') return 1;
    return o[0] == 'e' && early_phase;
}
static int sd_init(void)
{
    if (card_fail_wanted()) {
        kprintf("[CARD] mac.cardfail=%s: the card does not answer (as a missing one: 1.5 s)\n", psmac_option("mac.cardfail"));
        udelay(1500000);
        return -1;
    }
    sd_pins();
    w(SDVDD, 0); w(SDCMD, 0); w(SDARG, 0); w(SDTOUT, 0xf00000u); w(SDCDIV, 0);
    w(SDHSTS, HSTS_CLEAR); w(SDHCFG, 0); w(SDHBCT, 0); w(SDHBLC, 0);
    uint32_t e = r(SDEDM);
    e &= ~((0x1fu << 9) | (0x1fu << 14)); e |= (4u << 9) | (4u << 14);   /* FIFO thresholds, as Linux */
    w(SDEDM, e);
    udelay(20000);
    w(SDVDD, 1);
    udelay(20000);
    w(SDHCFG, HCFG_WIDE_INT | HCFG_SLOW | HCFG_BUSY_EN);
    sd_clock(400000);
    udelay(2000);
    uint32_t rsp[4];
    sd.rca = 0;
    sd_cmd(0, 0, CMD_NORSP, 0);
    udelay(2000);
    int v2 = sd_cmd(8, 0x1aa, 0, rsp) == 0 && (rsp[0] & 0xfff) == 0x1aa;
    uint32_t t0 = ms();
    for (;;) {
        int e = sd_acmd(41, v2 ? 0x40ff8000u : 0x00ff8000u, 0, rsp);
        if (!e && (rsp[0] & 0x80000000u)) break;                      /* powered up */
        if (ms() - t0 > 1500) return -1;
        udelay(10000);
    }
    sd.ccs = (rsp[0] >> 30) & 1;
    if (sd_cmd(2, 0, CMD_LONG, rsp)) return -2;
    if (sd_cmd(3, 0, 0, rsp)) return -3;
    sd.rca = rsp[0] >> 16;
    if (sd_cmd(7, sd.rca << 16, CMD_BUSY, rsp)) return -4;
    if (sd_acmd(6, 2, 0, rsp) == 0) w(SDHCFG, HCFG_WIDE_INT | HCFG_SLOW | HCFG_BUSY_EN | HCFG_WIDE_EXT);
    if (!sd.ccs) sd_cmd(16, 512, 0, rsp);
    sd_clock(25000000);
    return 0;
}
/* the card's state from CMD13: 4 = transfer (idle), 7 = programming; -1 if it does not answer */
static int sd_status(void)
{
    uint32_t rsp;
    if (sd_cmd(13, sd.rca << 16, 0, &rsp)) return -1;
    return (int)((rsp >> 9) & 0xf);
}
static int fsm_wait(uint32_t us)
{
    uint64_t t = cnt(), lim = frq() * us / 1000000u;
    for (;;) { uint32_t f = EDM_FSM(r(SDEDM)); if (f == FSM_DATA || f == FSM_IDENT) return 0; if (cnt() - t > lim) return -1; }
}
static int sd_read(uint32_t lba, uint8_t *buf)
{
    if (sd.state != 1) return -1;
    w(SDHBCT, 512); w(SDHBLC, 1);
    if (sd_cmd(17, sd.ccs ? lba : lba * 512u, CMD_READ, 0)) return -2;
    uint32_t *p = (uint32_t *)buf;
    for (int i = 0; i < 128; i++) {
        uint64_t t = cnt();
        while (!EDM_FILL(r(SDEDM)) && !(r(SDHSTS) & 1u)) if (cnt() - t > frq() / 10) { sd.errors++; return -3; }
        p[i] = *(volatile uint32_t *)(uintptr_t)(SDH + SDDATA);    /* raw: the bytes keep their order */
    }
    if (fsm_wait(100000) || (r(SDHSTS) & HSTS_ERRORS)) { uint32_t h = r(SDHSTS); sd.last_hsts = h; w(SDHSTS, h & HSTS_CLEAR); sd.errors++; return -4; }
    sd.reads++;
    return 0;
}
/* n consecutive sectors in one command (CMD18, stopped with CMD12): the disk image loader's read.
   A card answers a run far faster than the same sectors one CMD17 at a time, since each command
   costs it an access time of its own. */
static int sd_read_multi(uint32_t lba, uint8_t *buf, uint32_t n)
{
    if (n == 1) return sd_read(lba, buf);
    if (sd.state != 1) return -1;
    w(SDHBCT, 512); w(SDHBLC, n);
    if (sd_cmd(18, sd.ccs ? lba : lba * 512u, CMD_READ, 0)) return -2;
    uint32_t *p = (uint32_t *)buf;
    int err = 0;
    for (uint32_t i = 0; i < n * 128u; i++) {
        uint64_t t = cnt();
        while (!EDM_FILL(r(SDEDM)) && !(r(SDHSTS) & 1u)) if (cnt() - t > frq() / 10) { err = -3; break; }
        if (err) break;
        p[i] = *(volatile uint32_t *)(uintptr_t)(SDH + SDDATA);
    }
    if (sd_cmd(12, 0, CMD_BUSY, 0) && !err) err = -5;                  /* STOP_TRANSMISSION, R1b */
    if (!err && (fsm_wait(100000) || (r(SDHSTS) & HSTS_ERRORS))) err = -4;
    if (err) { uint32_t h = r(SDHSTS); sd.last_hsts = h; w(SDHSTS, h & HSTS_CLEAR); sd.errors++; return err; }
    sd.reads += n;
    return 0;
}
/* a write in two halves: the data goes out now, the card's programming is waited for by polling */
static int sd_write_start(uint32_t lba, const uint8_t *buf)
{
    if (sd.state != 1 || sd.ro) return -1;
    w(SDHBCT, 512); w(SDHBLC, 1);
    if (sd_cmd(24, sd.ccs ? lba : lba * 512u, CMD_WRITE, 0)) return -2;
    const uint32_t *p = (const uint32_t *)buf;
    for (int i = 0; i < 128; i++) {
        uint64_t t = cnt();
        while (EDM_FILL(r(SDEDM)) >= 15) if (cnt() - t > frq() / 10) { sd.errors++; return -3; }
        *(volatile uint32_t *)(uintptr_t)(SDH + SDDATA) = p[i];
    }
    return 0;
}
/* 1 done, 0 still programming, < 0 failed */
static int sd_write_poll(void)
{
    uint32_t f = EDM_FSM(r(SDEDM));
    if (f != FSM_DATA && f != FSM_IDENT) return 0;
    uint32_t h = r(SDHSTS);
    if (h & HSTS_ERRORS) { sd.last_hsts = h; w(SDHSTS, h & HSTS_CLEAR); sd.errors++; return -1; }
    int s = sd_status();
    if (s < 0) return -2;
    if (s != 4) return 0;
    sd.writes++;
    return 1;
}
static int sd_write(uint32_t lba, const uint8_t *buf)                 /* the small writes: directory, FAT, pairings */
{
    if (sd_write_start(lba, buf)) return -1;
    uint32_t t = ms();
    for (;;) { int k = sd_write_poll(); if (k == 1) return 0; if (k < 0 || ms() - t > 1000) return -2; }
}

/* ==== FAT ===================================================================================== */
static struct {
    int ok, fat32;
    uint32_t part, spc, fat_lba, nfats, fatsz, root_lba, rootsecs, data_lba, clusters, root_clus, fsinfo;
} fs;
static uint8_t sec[512] __attribute__((aligned(16)));
static uint8_t fsec[512] __attribute__((aligned(16))); static uint32_t fsec_lba = 0xffffffffu;
static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t clus_lba(uint32_t c) { return fs.data_lba + (c - 2) * fs.spc; }
static int is_fat_vbr(const uint8_t *b) { return b[510] == 0x55 && b[511] == 0xaa && (b[0] == 0xeb || b[0] == 0xe9) && le16(b + 11) == 512 && b[13] && b[16]; }
static int fs_mount(void)
{
    if (sd_read(0, sec)) return -1;
    uint32_t part = 0;
    if (!is_fat_vbr(sec)) {
        if (sec[510] != 0x55 || sec[511] != 0xaa) return -2;
        for (int i = 0; i < 4 && !part; i++) {
            const uint8_t *e = sec + 0x1be + 16 * i;
            uint8_t t = e[4];
            if ((t == 0x0b || t == 0x0c || t == 0x0e || t == 0x06 || t == 0x04) && le32(e + 12)) part = le32(e + 8);
        }
        if (!part || sd_read(part, sec) || !is_fat_vbr(sec)) return -3;
    }
    fs.part = part; fs.spc = sec[13]; fs.nfats = sec[16];
    uint32_t rsvd = le16(sec + 14), rootent = le16(sec + 17), tot = le16(sec + 19) ? le16(sec + 19) : le32(sec + 32);
    fs.fatsz = le16(sec + 22) ? le16(sec + 22) : le32(sec + 36);
    fs.rootsecs = (rootent * 32u + 511u) / 512u;
    fs.fat_lba = part + rsvd;
    fs.root_lba = fs.fat_lba + fs.nfats * fs.fatsz;
    fs.data_lba = fs.root_lba + fs.rootsecs;
    fs.clusters = (tot - (rsvd + fs.nfats * fs.fatsz + fs.rootsecs)) / fs.spc;
    fs.fat32 = fs.clusters >= 65525u;
    if (!fs.fat32 && fs.clusters < 4085u) return -4;                   /* FAT12: a floppy, not a boot card */
    fs.root_clus = fs.fat32 ? le32(sec + 44) : 0;
    fs.fsinfo = fs.fat32 ? part + le16(sec + 48) : 0;
    fs.ok = 1;
    return 0;
}
static uint32_t fat_get(uint32_t c)
{
    uint32_t off = c * (fs.fat32 ? 4u : 2u), lba = fs.fat_lba + off / 512u;
    if (lba != fsec_lba) { if (sd_read(lba, fsec)) return 0x0fffffffu; fsec_lba = lba; }
    return fs.fat32 ? le32(fsec + off % 512u) & 0x0fffffffu : le16(fsec + off % 512u);
}
static int fat_set(uint32_t c, uint32_t v)                             /* in every copy of the FAT */
{
    uint32_t off = c * (fs.fat32 ? 4u : 2u);
    for (uint32_t i = 0; i < fs.nfats; i++) {
        uint32_t lba = fs.fat_lba + i * fs.fatsz + off / 512u;
        if (sd_read(lba, sec)) return -1;
        if (fs.fat32) put32(sec + off % 512u, (le32(sec + off % 512u) & 0xf0000000u) | (v & 0x0fffffffu));
        else put16(sec + off % 512u, v);
        if (sd_write(lba, sec)) return -2;
    }
    fsec_lba = 0xffffffffu;
    return 0;
}
static int eoc(uint32_t v) { return fs.fat32 ? v >= 0x0ffffff8u : v >= 0xfff8u; }

/* a file in the root directory: where its entry is, its first cluster and size */
struct cfile { uint32_t dir_lba, dir_off, clus, size; };
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int name_eq(const char *a, const char *b) { while (*a && *b) if (lower(*a++) != lower(*b++)) return 0; return !*a && !*b; }
/* calls f for every root directory sector; it returns 1 to stop */
static int root_walk(int (*f)(uint32_t lba, void *ctx), void *ctx)
{
    if (!fs.fat32) {
        for (uint32_t i = 0; i < fs.rootsecs; i++) { int k = f(fs.root_lba + i, ctx); if (k) return k; }
        return 0;
    }
    uint32_t c = fs.root_clus;
    for (int guard = 0; c >= 2 && !eoc(c) && guard < 4096; guard++) {
        for (uint32_t i = 0; i < fs.spc; i++) { int k = f(clus_lba(c) + i, ctx); if (k) return k; }
        c = fat_get(c);
    }
    return 0;
}
struct find_ctx { const char *name; struct cfile *out; char lfn[128]; int lfn_ok; char found[48]; };
/* a disk image on the card by its name: what the container's own images end in (psmac_bus.c), less
   the Pi's own kernel*.img, macOS's ._ shadow files and anything under a megabyte */
static int ends_ci(const char *s, const char *suf)
{
    unsigned n = 0, m = 0; while (s[n]) n++; while (suf[m]) m++;
    if (m > n) return 0;
    for (unsigned i = 0; i < m; i++) if (lower(s[n - m + i]) != suf[i]) return 0;
    return 1;
}
static int image_taken(const char *name);                   /* below: already one of the Mac's disks */
#ifdef PSMAC_PC
/* the PC card's disks (pc.disk0= ... pc.disk3=, pc.fda=, pc.fdb=, pc.cd=, psmac_pc.c) are the PC's, not the Mac's: a
   freedos.img in the root was taken as a Mac disk image too, loaded into the Mac's memory and offered to the Mac, whose
   Finder asks to initialise a disk it cannot read (23 November 2026) */
static int pc_owns(const char *s)
{
    static const char *const keys[] = { "pc.disk0", "pc.disk1", "pc.disk2", "pc.disk3", "pc.fda", "pc.fdb", "pc.cd" };
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const char *v = psmac_option(keys[i]);
        if (!v || !v[0]) continue;
        char t[64]; unsigned n = 0;
        while ((unsigned char)v[n] > ' ' && n < sizeof t - 1) { t[n] = v[n]; n++; }   /* a CR of a Windows line end is not the name's */
        t[n] = 0;
        if (name_eq(t, s)) return 1;
    }
    return 0;
}
#endif
static int is_image_name(const char *s, uint32_t size)
{
    if (size < (1u << 20) || (s[0] == '.' && s[1] == '_')) return 0;
#ifdef PSMAC_PC
    if (pc_owns(s)) return 0;
#endif
    if (lower(s[0]) == 'k' && lower(s[1]) == 'e' && lower(s[2]) == 'r' && lower(s[3]) == 'n' && lower(s[4]) == 'e' && lower(s[5]) == 'l') return 0;
    return ends_ci(s, ".img") || ends_ci(s, ".hda") || ends_ci(s, ".image") || ends_ci(s, ".hfv") || ends_ci(s, ".dsk");
}
static int find_sector(uint32_t lba, void *vctx)
{
    struct find_ctx *x = vctx;
    if (sd_read(lba, sec)) return -1;
    for (int i = 0; i < 16; i++) {
        const uint8_t *e = sec + 32 * i;
        if (e[0] == 0) return 2;                                       /* the end of the directory */
        if (e[0] == 0xe5) { x->lfn_ok = 0; continue; }
        if (e[11] == 0x0f) {                                           /* a long-name piece */
            unsigned seq = e[0] & 0x1f, base = (seq - 1) * 13;
            if (e[0] & 0x40) { for (unsigned k = 0; k < sizeof x->lfn; k++) x->lfn[k] = 0; x->lfn_ok = 1; }
            static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (unsigned k = 0; k < 13; k++) { unsigned ch = le16(e + at[k]); if (base + k < sizeof x->lfn - 1) x->lfn[base + k] = (char)(ch == 0xffff ? 0 : ch); }
            continue;
        }
        if (e[11] & 0x18) { x->lfn_ok = 0; continue; }                 /* volume label, directory */
        char sn[13]; unsigned n = 0;
        for (int k = 0; k < 8 && e[k] != ' '; k++) sn[n++] = (char)e[k];
        if (e[8] != ' ') { sn[n++] = '.'; for (int k = 8; k < 11 && e[k] != ' '; k++) sn[n++] = (char)e[k]; }
        sn[n] = 0;
        const char *shown = x->lfn_ok ? x->lfn : sn;
        int hit = x->name ? name_eq(sn, x->name) || (x->lfn_ok && name_eq(x->lfn, x->name)) : is_image_name(shown, le32(e + 28)) && !image_taken(shown);
        if (hit && !x->name) { unsigned k = 0; while (shown[k] && k < sizeof x->found - 1) { x->found[k] = shown[k]; k++; } x->found[k] = 0; }
        x->lfn_ok = 0;
        if (hit) {
            x->out->dir_lba = lba; x->out->dir_off = 32u * (unsigned)i;
            x->out->clus = (fs.fat32 ? (uint32_t)le16(e + 20) << 16 : 0) | le16(e + 26);
            x->out->size = le32(e + 28);
            return 1;
        }
    }
    return 0;
}
static int file_find(const char *name, struct cfile *f)
{
    struct find_ctx x; x.name = name; x.out = f; x.lfn_ok = 0;
    return root_walk(find_sector, &x) == 1 ? 0 : -1;
}
struct free_ctx { uint32_t lba, off; };
static int free_sector(uint32_t lba, void *vctx)
{
    struct free_ctx *x = vctx;
    if (sd_read(lba, sec)) return -1;
    for (int i = 0; i < 16; i++) if (sec[32 * i] == 0 || sec[32 * i] == 0xe5) { x->lba = lba; x->off = 32u * (unsigned)i; return 1; }
    return 0;
}
static uint32_t clus_alloc(void)
{
    uint32_t start = 2;
    if (fs.fsinfo && !sd_read(fs.fsinfo, sec) && le32(sec) == 0x41615252u && le32(sec + 484) == 0x61417272u) {
        uint32_t hint = le32(sec + 492);
        if (hint >= 2 && hint < fs.clusters + 2) start = hint;
    }
    for (uint32_t n = 0; n < fs.clusters; n++) {
        uint32_t c = 2 + (start - 2 + n) % fs.clusters;
        if (fat_get(c) == 0) {
            if (fat_set(c, fs.fat32 ? 0x0fffffffu : 0xffffu)) return 0;
            if (fs.fsinfo && !sd_read(fs.fsinfo, sec) && le32(sec) == 0x41615252u) {
                uint32_t fc = le32(sec + 488); if (fc != 0xffffffffu && fc) put32(sec + 488, fc - 1);
                put32(sec + 492, c + 1); sd_write(fs.fsinfo, sec);
            }
            return c;
        }
    }
    return 0;
}
/* a new file, 8.3 name (stored upper case, shown lower case), one cluster, empty */
static int file_create(const char *base8, const char *ext3, struct cfile *f)
{
    struct free_ctx x;
    if (root_walk(free_sector, &x) != 1) return -1;
    uint32_t c = clus_alloc();
    if (!c) return -2;
    if (sd_read(x.lba, sec)) return -3;
    uint8_t *e = sec + x.off;
    for (int k = 0; k < 32; k++) e[k] = 0;
    for (int k = 0; k < 8; k++) e[k] = (uint8_t)(base8[k] ? base8[k] : ' ');
    for (int k = 0; k < 3; k++) e[8 + k] = (uint8_t)(ext3[k] ? ext3[k] : ' ');
    e[11] = 0x20; e[12] = 0x18;
    uint32_t date = (uint32_t)((2026 - 1980) << 9 | 11 << 5 | 18);
    put16(e + 14, 0); put16(e + 16, date); put16(e + 18, date); put16(e + 22, 0); put16(e + 24, date);
    put16(e + 20, c >> 16); put16(e + 26, c & 0xffff); put32(e + 28, 0);
    if (sd_write(x.lba, sec)) return -4;
    f->dir_lba = x.lba; f->dir_off = x.off; f->clus = c; f->size = 0;
    return 0;
}

/* ==== pairings.txt ============================================================================ */
static char pair_text[4096]; static uint32_t pair_len; static int pair_state;   /* 0 pending, 1 read, -1 none */
static char pair_new[4096]; static volatile uint32_t pair_new_len; static volatile int pair_dirty;
static struct cfile pair_file; static int pair_have;
static void pairings_load(void)
{
    pair_have = file_find("pairings.txt", &pair_file) == 0;
    pair_len = 0;
    if (pair_have && pair_file.clus >= 2) {
        uint32_t n = pair_file.size < sizeof pair_text - 1 ? pair_file.size : sizeof pair_text - 1, c = pair_file.clus, got = 0;
        while (got < n && c >= 2 && !eoc(c)) {
            for (uint32_t i = 0; i < fs.spc && got < n; i++) {
                if (sd_read(clus_lba(c) + i, sec)) { n = got; break; }
                uint32_t k = n - got < 512 ? n - got : 512;
                for (uint32_t j = 0; j < k; j++) pair_text[got + j] = (char)sec[j];
                got += k;
            }
            c = fat_get(c);
        }
        pair_len = got;
    }
    pair_text[pair_len] = 0;
    pair_state = 1;
    kprintf("[CARD] pairings.txt: %s%u bytes\n", pair_have ? "" : "not on the card yet, ", pair_len);
}
static void pairings_write(void)
{
    uint32_t n = pair_new_len;
    if (!pair_have) {
        if (file_create("PAIRINGS", "TXT", &pair_file)) { kprintf("[CARD] could not create pairings.txt (root directory full?)\n"); pair_dirty = 0; return; }
        pair_have = 1;
        kprintf("[CARD] pairings.txt created\n");
    }
    uint32_t cap = fs.spc * 512u;
    if (n > cap) { while (n > cap) n--; while (n && pair_new[n - 1] != '\n') n--; kprintf("[CARD] pairings.txt: only %u bytes fit in one cluster\n", n); }
    for (uint32_t i = 0; i * 512u < n || i == 0; i++) {
        for (uint32_t j = 0; j < 512; j++) sec[j] = i * 512u + j < n ? (uint8_t)pair_new[i * 512u + j] : 0;
        if (sd_write(clus_lba(pair_file.clus) + i, sec)) { kprintf("[CARD] pairings.txt: write failed\n"); return; }
    }
    if (sd_read(pair_file.dir_lba, sec)) return;
    put32(sec + pair_file.dir_off + 28, n);
    if (sd_write(pair_file.dir_lba, sec)) return;
    pair_file.size = n;
    kprintf("[CARD] pairings.txt: %u bytes written\n", n);
}
/* bt_pi.c: the pairings as the card had them at start (state 0 pending, 1 ready, -1 no card) */
const char *psmac_card_pairings(int *state) { *state = sd.state < 0 ? -1 : pair_state; return pair_state == 1 ? pair_text : 0; }
void psmac_card_save_pairings(const char *text, uint32_t len)
{
    if (len > sizeof pair_new) len = sizeof pair_new;
    for (uint32_t i = 0; i < len; i++) pair_new[i] = text[i];
    pair_new_len = len;
    __atomic_store_n(&pair_dirty, 1, __ATOMIC_RELEASE);
}

/* ==== pram.bin: the parameter RAM of the solo board and of the SE board's stand-in ============
   A real Mac keeps its parameter RAM in a chip with a battery: AppleTalk active (the Chooser), the
   Network control panel's connection (extended PRAM $E0), Monitors' depth, the startup disk, the
   sound. The solo board's chip (psmac_board_solo.c) started from its defaults at every power-on.
   pram.bin in the card's root keeps the 256 bytes and then the clock (4 bytes, the Mac's seconds
   since 1904, big-endian, as it was at the save), in one cluster, created the first time it is
   written. Read at the card's first look; written when the board hands over a change, which it does
   a second after the Mac's last one and never during the boot. mac.card=ro reads it only.
   Since 20 November the SE board uses the same file for the parameter RAM and clock it answers with
   when the SE's own clock chip does not answer (psmac_board_se.c, xp_card_load and
   psmac_se_pram_take: at most every ten seconds, and every ten minutes for the clock); with a chip
   that answers the file is neither read nor written. One card moved between a bench Pi and the SE
   keeps one set of settings. */
static uint8_t pram_text[512]; static uint32_t pram_len; static int pram_state;   /* 0 pending, 1 read, -1 none */
static struct cfile pram_file; static int pram_have;
static void pram_load(void)
{
    pram_have = file_find("pram.bin", &pram_file) == 0;
    pram_len = 0;
    if (pram_have && pram_file.clus >= 2 && pram_file.size >= 256 && !sd_read(clus_lba(pram_file.clus), sec)) {
        pram_len = pram_file.size < 260 ? pram_file.size : 260;
        for (uint32_t i = 0; i < pram_len; i++) pram_text[i] = sec[i];
    }
    __atomic_store_n(&pram_state, pram_len ? 1 : -1, __ATOMIC_RELEASE);
    kprintf("[CARD] pram.bin: %s\n", pram_len ? "read" : pram_have ? "too short, not used" : "not on the card yet");
}
static void pram_write(const uint8_t *p)
{
    if (!pram_have) {
        if (file_create("PRAM", "BIN", &pram_file)) { kprintf("[CARD] could not create pram.bin (root directory full?)\n"); return; }
        pram_have = 1;
    }
    for (uint32_t j = 0; j < 512; j++) sec[j] = j < 260 ? p[j] : 0;
    if (sd_write(clus_lba(pram_file.clus), sec)) { kprintf("[CARD] pram.bin: write failed\n"); return; }
    if (pram_file.size != 260) {
        if (sd_read(pram_file.dir_lba, sec)) return;
        put32(sec + pram_file.dir_off + 28, 260);
        if (sd_write(pram_file.dir_lba, sec)) return;
        pram_file.size = 260;
    }
    kprintf("[CARD] pram.bin: parameter RAM saved\n");
}
/* psmac_board_solo.c and psmac_board_se.c: pram.bin as the card had it at start (state 0 pending, 1 read, -1 none) */
const uint8_t *psmac_card_pram(int *state, uint32_t *len)
{
    *state = sd.state < 0 ? -1 : __atomic_load_n(&pram_state, __ATOMIC_ACQUIRE);
    *len = pram_len;
    return *state == 1 ? pram_text : 0;
}

/* ==== aurora68.log: the kernel's log, for the user to hand on =================================
   The bench user photographed PiLog's window to report a problem; the log is better as text. On
   the Mac's request (PSMB_INFO_SAVELOG: PiLog's "Save to Card"), and by itself right after a
   [CRASH] record (psmac_crash.c), the whole 64 KiB ring goes into aurora68.log in the card's root,
   overwritten each time, so it can be read on another computer and survives a power cut.
   The file is made once, the size the ring needs, and never grows afterwards: its first cluster as
   for pairings.txt, then the rest one at a time, each taken (marked end of chain) before the one
   before it points to it, so an interruption leaves at worst a lost cluster, never a broken chain.
   A file already there with a shorter chain is lengthened the same way. The text is a snapshot of
   the ring taken when the writing starts, with the values of options that are secrets (wifi.psk
   and anything named psk, pass, key, secret or token (the remote control's remote.token=, docs/13),
   the password in mac.wifisim, the link key in bt.dev) replaced by ***. One sector per pass of core 3, like everything here (logf_step). */
static volatile int logf_req;                  /* asked; 2: after a crash record; 3: on its own (mac.logsave) */
static volatile int logf_state;                /* 0 nothing asked, 1 being written, 2 saved, 3 failed */
static uint8_t *logf_buf; static uint32_t logf_len, logf_done, logf_clus, logf_ci;
static struct cfile logf_file; static int logf_have;
static int secret_name(const char *s, uint32_t n)
{
    uint32_t b = n;
    while (b && s[b - 1] != '.') b--;                  /* the last part of a dotted name */
    static const char *const names[] = { "psk", "pass", "passwd", "password", "passphrase", "passkey", "key", "secret", "token", "recoverypin" };
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        uint32_t m = 0, j;
        while (names[i][m]) m++;
        if (n - b < m) continue;
        j = 0;
        while (j < m && lower(s[n - m + j]) == names[i][j]) j++;
        /* the whole part, or its end after _ or - (wpa_passphrase, wifi-key) */
        if (j == m && (n - m == b || s[n - m - 1] == '_' || s[n - m - 1] == '-')) return 1;
    }
    return 0;
}
static int name_is(const char *s, uint32_t n, const char *k) { uint32_t j = 0; while (j < n && k[j] && lower(s[j]) == k[j]) j++; return j == n && !k[j]; }
/* in into out with secret values as ***; returns the bytes in out */
static uint32_t log_redact(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t cap)
{
    uint32_t o = 0, ns = 0;
    for (uint32_t i = 0; i < n && o < cap; i++) {
        uint8_t c = in[i];
        out[o++] = c;
        int namec = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (c != '=') { if (!namec) ns = o; continue; }
        const char *nm = (const char *)out + ns; uint32_t nl = o - 1 - ns;
        uint32_t keep = 0;                             /* value bytes kept before the secret part */
        int hide = secret_name(nm, nl), skip_to = 0;
        if (!hide && name_is(nm, nl, "mac.wifisim")) { hide = 1; skip_to = 1; }    /* SSID:PASSWORD */
        if (!hide && name_is(nm, nl, "bt.dev")) { hide = 1; skip_to = 1; }         /* ADDRESS:KEY:FLAGS */
        if (!hide) { ns = o; continue; }
        uint32_t v = i + 1, q = v < n && in[v] == '"';
        if (skip_to) { while (v + keep < n && in[v + keep] != ':' && in[v + keep] != ' ' && in[v + keep] != '\n') keep++; if (v + keep < n && in[v + keep] == ':') keep++; }
        for (uint32_t k = 0; k < keep && o < cap; k++) out[o++] = in[v + k];
        uint32_t e = v + keep;
        if (q && !keep) { e++; while (e < n && in[e] != '"' && in[e] != '\n') e++; if (e < n && in[e] == '"') e++; }
        else while (e < n && in[e] != ' ' && in[e] != '\t' && in[e] != '\n' && in[e] != ')' && in[e] != ',' && !(skip_to && in[e] == ':')) e++;
        for (const char *r = "***"; *r && o < cap; r++) out[o++] = (uint8_t)*r;
        i = e - 1; ns = o;
    }
    return o;
}
/* the same rule for the kernel's own lines that repeat cmdline.txt (psmac_boot.c): out is
   NUL-terminated, cut short if it is too small */
void psmac_redact(const char *in, char *out, uint32_t cap)
{
    uint32_t n = 0; while (in[n]) n++;
    out[log_redact((const uint8_t *)in, n, (uint8_t *)out, cap - 1)] = 0;
}
/* psmac_mbox.c (PiLog) and psmac_crash.c: 0 asked, -1 no writable card */
int psmac_card_save_log(int after_crash)
{
    if (sd.state < 0 || sd.ro || psmac_recovery_on()) return -1;   /* in recovery aurora68.log is the last start's, kept */
    logf_req = after_crash ? 2 : 1;
    __atomic_store_n(&logf_state, 1, __ATOMIC_RELEASE);
    return 0;
}
int psmac_card_log_state(void) { return logf_state; }
static int logf_phase;
static void logf_fail(const char *why) { kprintf("[CARD] aurora68.log: %s\n", why); logf_phase = 0; logf_state = 3; }
/* One step per pass of core 3: 0 the snapshot; 1 the file found or made (its directory entry and
   first cluster, as file_create does for pairings.txt), its chain walked; 2 one more cluster on the
   chain, until it holds the text; 3 one sector of the text; 4 the size in the directory entry. */
static uint32_t logf_tail, logf_nclus;
/* mac.logsave=S: the log saved by itself every S seconds, 0 never. In the SE there is no serial
   line (GPIO 14/15 are the PiStorm's data lines) and the HDMI console scrolls the early lines
   away, so a boot that stops before the Mac's screen comes up left nothing to read but a
   photograph of the last screenful (the first SE boot, 20 November 2026). The SE board saves
   every 30 seconds unless told otherwise; the file is overwritten in place, never grows. */
static uint32_t logsave_s, logsave_last, logf_saves; static int logf_quiet;
static void logf_step(void)
{
    extern uint32_t psmac_log_copy(uint8_t *out, uint32_t max);
    static uint8_t *raw;
    if (logf_req) {                                    /* a new request, or one during a save: from the start */
        int why = logf_req;
        logf_req = 0;
        if (!logf_buf) logf_buf = tlsf_malloc(tlsf, PSMB_LOG_SIZE + 8192);
        if (!raw) raw = tlsf_malloc(tlsf, PSMB_LOG_SIZE);
        if (!logf_buf || !raw) { logf_fail("no memory for the copy"); return; }
        uint32_t n = psmac_log_copy(raw, PSMB_LOG_SIZE), s = 0;
        if (n == PSMB_LOG_SIZE) { while (s < n && raw[s] != '\n') s++; if (s < n) s++; }   /* the oldest line is cut: from the next */
        const char *hd = why == 2 ? "Aurora68 kernel log, saved after a crash record\n"
                       : why == 3 ? "Aurora68 kernel log, saved on its own (mac.logsave)\n" : "Aurora68 kernel log, saved from the Mac\n";
        logf_quiet = why == 3 && logf_saves > 0;       /* a timed save says so the first time only */
        uint32_t h = 0; while (hd[h]) { logf_buf[h] = (uint8_t)hd[h]; h++; }
        logf_len = h + log_redact(raw + s, n - s, logf_buf + h, PSMB_LOG_SIZE + 8192 - h);
        logf_phase = 1;
        return;
    }
    if (logf_phase == 1) {
        if (!logf_have) {
            if (file_find("aurora68.log", &logf_file) == 0) logf_have = 1;
            else if (file_create("AURORA68", "LOG", &logf_file)) { logf_fail("could not create it (root directory or card full?)"); return; }
            else { logf_have = 1; kprintf("[CARD] aurora68.log created\n"); }
        }
        if (logf_file.clus < 2) {                      /* an empty file made elsewhere: no cluster yet */
            uint32_t c = clus_alloc();
            if (!c || sd_read(logf_file.dir_lba, sec)) { logf_fail("no free cluster"); return; }
            put16(sec + logf_file.dir_off + 20, c >> 16); put16(sec + logf_file.dir_off + 26, c & 0xffff);
            if (sd_write(logf_file.dir_lba, sec)) { logf_fail("directory write failed"); return; }
            logf_file.clus = c;
        }
        uint32_t c = logf_file.clus, n = 1;
        for (uint32_t nx = fat_get(c); nx >= 2 && !eoc(nx) && n < 65536; nx = fat_get(c)) { c = nx; n++; }
        logf_tail = c; logf_nclus = n;
        logf_phase = 2;
        return;
    }
    if (logf_phase == 2) {
        if (logf_nclus * fs.spc * 512u < logf_len) {
            uint32_t nc = clus_alloc();                /* taken, marked end of chain... */
            if (!nc) { logf_fail("the card is full"); return; }
            if (fat_set(logf_tail, nc)) { logf_fail("FAT write failed"); return; }    /* ...then joined on */
            logf_tail = nc; logf_nclus++;
            return;
        }
        logf_done = 0; logf_clus = logf_file.clus; logf_ci = 0;
        logf_phase = 3;
    }
    if (logf_phase == 3) {
        for (uint32_t j = 0; j < 512; j++) sec[j] = logf_done + j < logf_len ? logf_buf[logf_done + j] : 0;
        if (sd_write(clus_lba(logf_clus) + logf_ci, sec)) { logf_fail("write failed"); return; }
        logf_done += 512;
        if (++logf_ci == fs.spc && logf_done < logf_len) {
            logf_ci = 0; logf_clus = fat_get(logf_clus);
            if (logf_clus < 2 || eoc(logf_clus)) { logf_fail("its chain ended early"); return; }
        }
        if (logf_done >= logf_len) logf_phase = 4;
        return;
    }
    if (logf_phase != 4) { logf_fail("lost its place"); return; }
    if (sd_read(logf_file.dir_lba, sec)) { logf_fail("directory read failed"); return; }
    put32(sec + logf_file.dir_off + 28, logf_len);
    if (sd_write(logf_file.dir_lba, sec)) { logf_fail("directory write failed"); return; }
    logf_file.size = logf_len;
    logf_phase = 0;
    logf_state = 2;
    if (!logf_quiet) kprintf("[CARD] aurora68.log: %u bytes of the log written%s\n", logf_len, logsave_s ? " (and again every mac.logsave period)" : "");
    logf_saves++;
}

/* ==== cmdline.txt: the options the Mac asks to change ========================================
   The edit list is Alice68's (alice_net.h, struct an_setup): lines "-NAME" (drop the option NAME)
   and "+NAME=VALUE" (replace NAME, or add it at the end). Options are the line's words; a
   quoted value ("wifi-office", "My Network") stays one word. */
static char opt_edit[512]; static volatile uint32_t opt_edit_len; static volatile int opt_state;   /* 0 none, 1 pending, 2 saved, 3 failed */
static char opt_old[2048], opt_out[2048];
static int opt_key_eq(const char *w, uint32_t wl, const char *k, uint32_t kl)
{
    uint32_t n = 0;
    while (n < wl && w[n] != '=') n++;
    if (n != kl) return 0;
    for (uint32_t i = 0; i < n; i++) if (w[i] != k[i]) return 0;
    return 1;
}
static void cmdline_apply(void)
{
    struct cfile f;
    uint32_t cap = fs.spc * 512u, n, o = 0;
    if (file_find("cmdline.txt", &f) || f.clus < 2 || f.size == 0 || f.size >= sizeof opt_old || f.size > cap) {
        kprintf("[CARD] cmdline.txt: not found or too large to edit\n"); opt_state = 3; return;
    }
    n = f.size;
    for (uint32_t i = 0; i * 512u < n; i++) {
        if (sd_read(clus_lba(f.clus) + i, sec)) { opt_state = 3; return; }
        for (uint32_t j = 0; j < 512 && i * 512u + j < n; j++) opt_old[i * 512u + j] = (char)sec[j];
    }
    int nl = n && (opt_old[n - 1] == '\n' || opt_old[n - 1] == '\r');
    /* the words of the old line, those the edit list keeps */
    uint32_t p = 0;
    while (p < n) {
        while (p < n && (opt_old[p] == ' ' || opt_old[p] == '\t' || opt_old[p] == '\n' || opt_old[p] == '\r')) p++;
        if (p >= n) break;
        uint32_t w = p; int q = 0;
        while (p < n && (q || (opt_old[p] != ' ' && opt_old[p] != '\t' && opt_old[p] != '\n' && opt_old[p] != '\r'))) { if (opt_old[p] == '"') q = !q; p++; }
        int drop = 0;
        for (uint32_t e = 0; e < opt_edit_len; ) {
            uint32_t le = e; while (le < opt_edit_len && opt_edit[le] != '\n') le++;
            if (le > e + 1 && (opt_edit[e] == '-' || opt_edit[e] == '+')) {
                uint32_t kl = 0; while (e + 1 + kl < le && opt_edit[e + 1 + kl] != '=') kl++;
                if (opt_key_eq(opt_old + w, p - w, opt_edit + e + 1, kl)) drop = 1;
            }
            e = le + 1;
        }
        if (!drop) {
            if (o + (p - w) + 2 >= sizeof opt_out) { opt_state = 3; return; }
            if (o) opt_out[o++] = ' ';
            for (uint32_t i = w; i < p; i++) opt_out[o++] = opt_old[i];
        }
    }
    /* the new words */
    for (uint32_t e = 0; e < opt_edit_len; ) {
        uint32_t le = e; while (le < opt_edit_len && opt_edit[le] != '\n') le++;
        if (le > e + 1 && opt_edit[e] == '+') {
            if (o + (le - e) + 2 >= sizeof opt_out) { opt_state = 3; return; }
            if (o) opt_out[o++] = ' ';
            for (uint32_t i = e + 1; i < le; i++) opt_out[o++] = opt_edit[i];
        }
        e = le + 1;
    }
    if (nl) opt_out[o++] = '\n';
    if (!o || o > cap) { kprintf("[CARD] cmdline.txt: the new line does not fit\n"); opt_state = 3; return; }
    for (uint32_t i = 0; i * 512u < o; i++) {
        for (uint32_t j = 0; j < 512; j++) sec[j] = i * 512u + j < o ? (uint8_t)opt_out[i * 512u + j] : 0;
        if (sd_write(clus_lba(f.clus) + i, sec)) { kprintf("[CARD] cmdline.txt: write failed\n"); opt_state = 3; return; }
    }
    if (sd_read(f.dir_lba, sec)) { opt_state = 3; return; }
    put32(sec + f.dir_off + 28, o);
    if (sd_write(f.dir_lba, sec)) { opt_state = 3; return; }
    /* read back */
    for (uint32_t i = 0; i * 512u < o; i++) {
        if (sd_read(clus_lba(f.clus) + i, sec)) { opt_state = 3; return; }
        for (uint32_t j = 0; j < 512 && i * 512u + j < o; j++)
            if (sec[j] != (uint8_t)opt_out[i * 512u + j]) { kprintf("[CARD] cmdline.txt: read back differs\n"); opt_state = 3; return; }
    }
    kprintf("[CARD] cmdline.txt: %u bytes written (was %u)\n", o, n);
    opt_state = 2;
}
/* the mailbox (PSMB_INFO_SETOPT): queue an edit list; the answer comes from psmac_card_options_state */
int psmac_card_edit_options(const char *text, uint32_t len)
{
    if (sd.state < 0 || sd.ro) return -1;
    if (opt_state == 1) return -2;
    if (len >= sizeof opt_edit) return -3;
    for (uint32_t i = 0; i < len; i++) opt_edit[i] = text[i];
    opt_edit_len = len;
    __atomic_store_n(&opt_state, 1, __ATOMIC_RELEASE);
    return 0;
}
int psmac_card_options_state(void) { return opt_state; }

/* ==== disk images: inside the container, or files of their own ===============================
   An image the container brought was loaded by the Pi firmware with everything else, and its
   changes go back into the container at the image's offset. A container of 100 MB costs the
   firmware seconds before the kernel even starts (docs/08 item 50), so an image can also sit on
   the card as a file of its own (MyMac.hda, or mac.image=NAME): core 3 finds it when it mounts the
   card, a few milliseconds after the 68k has been released, and reads it into memory in the
   background while the Mac boots. Blocks the Mac asks for before they are in are read first
   (psmac_card_need); its changes go back into the same file, in place. */
#define CHUNK 4096u                    /* the unit of the Mac's changes waiting for the card */
#define LCHUNK 16384u                  /* the loader's unit: 32 card sectors */
#define LSLICE 4u                      /* card sectors read per pass of core 3's loop: about 170 us at
                                          25 MHz, well inside the 32-byte receive FIFO of the
                                          Bluetooth UART beside it (921600 baud, no flow control) */
struct ext { uint32_t clus, n; };
struct img {
    const uint8_t *ram; uint32_t off, len;       /* the image in memory; its offset in its file, its length */
    const uint8_t *file; uint32_t file_len;      /* the file it lives in, in memory: the container, or the image itself */
    struct ext *ext; int next, ok;               /* that file's extents on the card; changes are written back */
    volatile uint32_t *dirty; uint32_t nbits;    /* CHUNKs the Mac changed */
    volatile uint32_t *loaded; uint32_t nload, nloaded;   /* LCHUNKs read from the card (images of their own) */
    volatile uint32_t *failed;                   /* LCHUNKs the card did not give: the Mac gets an I/O error for them */
    uint64_t load_t0; char name[48]; uint32_t written;
};
static struct img imgs[4]; static int nimg;
static const uint8_t *cont_ram; static uint32_t cont_len;
static struct ext cont_ext[256]; static int cont_next;          /* the container file's extents */
static volatile uint64_t last_dirty_t, first_dirty_t;
static volatile int scan_done;                 /* the card has been looked at for images (or there is none) */
void psmac_card_container(const uint8_t *ram, uint32_t len) { cont_ram = ram; cont_len = len; }
static struct img *img_new(const char *name, const uint8_t *ram, uint32_t len)
{
    if (nimg >= 4) return 0;
    struct img *m = &imgs[nimg];
    m->ram = ram; m->len = len; m->nbits = (len + CHUNK - 1) / CHUNK;
    m->dirty = tlsf_malloc(tlsf, ((m->nbits + 31) / 32) * 4);
    if (!m->dirty) return 0;
    for (uint32_t i = 0; i < (m->nbits + 31) / 32; i++) m->dirty[i] = 0;
    int k = 0; while (name[k] && k < 47) { m->name[k] = name[k]; k++; } m->name[k] = 0;
    return m;
}
/* an image inside the container (psmac_bus.c walk_container, before the 68k starts) */
void psmac_card_track(const char *name, const uint8_t *ram, uint32_t off, uint32_t len)
{
    struct img *m = img_new(name, ram, len);
    if (!m) return;
    m->off = off; m->file = 0; m->ext = cont_ext;        /* file and extents once the card is checked */
    nimg++;
}
/* the Mac wrote [p, p+len) of an image in memory (psmac_board_solo.c, psmac_mbox.c) */
static volatile uint32_t dirty_calls, dirty_miss; static const void *dirty_last;
void psmac_card_dirty(const void *p, uint32_t len)
{
    const uint8_t *b = p;
    dirty_calls++; dirty_last = p;
    for (int i = 0; i < nimg; i++) {
        struct img *m = &imgs[i];
        if (b < m->ram || b >= m->ram + m->len) continue;
        uint32_t s = (uint32_t)(b - m->ram) / CHUNK, e = ((uint32_t)(b - m->ram) + len - 1) / CHUNK;
        for (uint32_t c = s; c <= e && c < m->nbits; c++) __atomic_fetch_or(&m->dirty[c / 32], 1u << (c % 32), __ATOMIC_RELEASE);
        uint64_t now = cnt();
        if (!first_dirty_t) first_dirty_t = now;
        last_dirty_t = now;
        return;
    }
    dirty_miss++;
}
static int file_extents(const struct cfile *f, struct ext *ext, int max)
{
    int n = 0;
    uint32_t c = f->clus, guard = 0;
    while (c >= 2 && !eoc(c) && guard++ < fs.clusters) {
        if (n && ext[n - 1].clus + ext[n - 1].n == c) ext[n - 1].n++;
        else { if (n >= max) return -1; ext[n].clus = c; ext[n].n = 1; n++; }
        c = fat_get(c);
    }
    return n;
}
/* the card sector holding byte off of an image's file, and how many sectors follow it in the same extent */
static uint32_t file_lba(const struct img *m, uint32_t off, uint32_t *run)
{
    uint32_t cl = off / (fs.spc * 512u);
    for (int i = 0; i < m->next; i++) {
        if (cl < m->ext[i].n) {
            uint32_t in = (off / 512u) % fs.spc;
            if (run) *run = (m->ext[i].n - cl) * fs.spc - in;
            return clus_lba(m->ext[i].clus + cl) + in;
        }
        cl -= m->ext[i].n;
    }
    return 0;
}
static void container_check(void)
{
    const char *name = psmac_option("mac.container");
    if (!name) name = "aurora68.cpio";
    struct cfile f;
    if (!nimg) return;
    if (!cont_ram || file_find(name, &f)) { kprintf("[CARD] %s not found in the card's root: the disk image's changes stay in memory\n", name); return; }
    if (f.size != cont_len) { kprintf("[CARD] %s on the card is %u bytes, the one loaded %u: not the same file, nothing written to it\n", name, f.size, cont_len); return; }
    cont_next = file_extents(&f, cont_ext, 256);
    if (cont_next < 0) { cont_next = 0; kprintf("[CARD] %s is in more than 256 pieces: nothing written to it\n", name); return; }
    for (int i = 0; i < nimg; i++) { imgs[i].file = cont_ram; imgs[i].file_len = cont_len; imgs[i].next = cont_next; }
    uint32_t l = file_lba(&imgs[0], 0, 0);
    if (!l || sd_read(l, sec)) return;
    for (int i = 0; i < 512; i++) if (sec[i] != cont_ram[i]) { kprintf("[CARD] %s's first sector differs from the one loaded: nothing written to it\n", name); return; }
    for (int i = 0; i < nimg; i++) imgs[i].ok = 1;
    kprintf("[CARD] %s checked (%u bytes, %d piece%s): the disk image's changes go back into it\n", name, f.size, cont_next, cont_next == 1 ? "" : "s");
}

/* memory for an image of its own: from the kernel's list of free RAM, block 0, which
   psmac_setup_memory carved the Mac's RAM from before the 68k started. Nothing takes from it
   after that but this, on core 3. PSMAC_PI_SLACK_MB (16 MB) stay free for whatever comes later.
   How much there is was decided with the Mac's RAM (psmac_setup_memory logs it). */
static uint32_t memory_left_mb(void)
{
    uintptr_t base = (sys_memory[0].mb_Base + 0xfffu) & ~(uintptr_t)0xfffu, end = sys_memory[0].mb_Base + sys_memory[0].mb_Size;
    return end > base + ((uintptr_t)PSMAC_PI_SLACK_MB << 20) ? (uint32_t)((end - base) >> 20) - PSMAC_PI_SLACK_MB : 0;
}
static uint8_t *take_memory(uint32_t len)
{
    uintptr_t base = (sys_memory[0].mb_Base + 0xfffu) & ~(uintptr_t)0xfffu, end = sys_memory[0].mb_Base + sys_memory[0].mb_Size;
    if (base + len + ((uintptr_t)PSMAC_PI_SLACK_MB << 20) > end) return 0;
    sys_memory[0].mb_Size = end - (base + len);
    sys_memory[0].mb_Base = base + len;
    return (uint8_t *)(0xffffff9000000000ull + base);
}
/* at mount: images of their own, when the container brought none. Every disk image file in the
   card's root in directory order (the order they were copied there: the first is SCSI ID 0 on the
   solo board), or the names in mac.image=NAME[,NAME...], up to four */
static int image_taken(const char *name)
{
    for (int i = 0; i < nimg; i++) if (name_eq(imgs[i].name, name)) return 1;
    return 0;
}
static int image_take(const char *name, const struct cfile *f)
{
    extern int psmac_board_solo;
    if (f->clus < 2 || f->size < 1024) return 0;
    uint8_t *ram = take_memory(f->size);
    if (!ram) {
        kprintf("[CARD] MEMORY: %s (%u MB) is larger than the memory the Pi has left (%u MB for disk images): not loaded. "
                "A smaller image, or a smaller mac.ram= (the Mac has %u MB)\n", name, f->size >> 20, memory_left_mb(), psmac_mac_ram_mb());
        return 0;
    }
    struct img *m = img_new(name, ram, f->size);
    if (!m) return 0;
    m->ext = tlsf_malloc(tlsf, 256 * sizeof(struct ext));
    m->nload = (f->size + LCHUNK - 1) / LCHUNK;
    m->loaded = tlsf_malloc(tlsf, ((m->nload + 31) / 32) * 4);
    m->failed = tlsf_malloc(tlsf, ((m->nload + 31) / 32) * 4);
    if (!m->ext || !m->loaded || !m->failed) return 0;
    m->next = file_extents(f, m->ext, 256);
    if (m->next <= 0) { kprintf("[CARD] %s is in more than 256 pieces: copy it to a freshly formatted card\n", name); return 0; }
    for (uint32_t i = 0; i < (m->nload + 31) / 32; i++) m->loaded[i] = m->failed[i] = 0;
    m->off = 0; m->file = ram; m->file_len = f->size; m->ok = !sd.ro; m->nloaded = 0; m->load_t0 = cnt();
    __atomic_store_n(&nimg, nimg + 1, __ATOMIC_RELEASE);
    kprintf("[CARD] disk image %s, %u MB in %d piece%s: read in the background%s\n", name, f->size >> 20, m->next, m->next == 1 ? "" : "s", m->ok ? ", changes written back to it" : "");
    psmac_boot_mark("card: disk image %s found (%u MB), loading in the background", name, f->size >> 20);
    psmac_splash_status("Loading the disk image %s...", name);         /* the HDMI splash (psmac_display.c) */
    /* the Mac's view: a SCSI disk on the solo board, a PiDisk volume on the others */
    extern void psmac_image_add(const char *, uint8_t *, uint32_t); extern int psmac_image_count(void), psmac_image_is_scsi(int);
    extern void psmac_mbox_add_disk(const char *, uint8_t *, uint32_t);
    if (psmac_board_solo) psmac_image_add(m->name, ram, f->size);           /* the first is SCSI ID 0 */
    if (!psmac_board_solo || !psmac_image_is_scsi(psmac_image_count() - 1)) psmac_mbox_add_disk(m->name, ram, f->size / 512u);
    return 1;
}
static void image_scan(void)
{
    if (nimg) return;
    const char *opt = psmac_option("mac.image");
    if (opt && name_eq(opt, "off")) return;
    struct cfile f; struct find_ctx x; x.out = &f; x.lfn_ok = 0;
    if (opt) {
        char list[128]; unsigned n = 0;
        while (opt[n] && n < sizeof list - 1) { list[n] = opt[n]; n++; }
        list[n] = 0;
        for (char *w = list; *w && nimg < 4; ) {
            char *e = w; while (*e && *e != ',') e++;
            char c = *e; *e = 0;
            if (file_find(w, &f)) kprintf("[CARD] mac.image=%s: no such file in the card's root\n", w);
            else if (!image_taken(w)) image_take(w, &f);
            if (!c) break;
            w = e + 1;
        }
        return;
    }
    while (nimg < 4) {
        x.name = 0; x.found[0] = 0; x.lfn_ok = 0;
        if (root_walk(find_sector, &x) != 1 || !image_take(x.found, &f)) break;
    }
}

/* the loader: one LCHUNK at a time, LSLICE sectors per pass; a chunk the Mac waits for first */
static struct { int active, img; uint32_t chunk, done, cursor; } ld;
static volatile int want_img = -1; static volatile uint32_t want_chunk;
/* One owner of the card at a time. Core 3 holds it for each of its passes (psmac_card_poll) and for
   PiDisk's blocks; core 0 takes it to read a chunk the Mac is waiting for itself when core 3 has not
   come round to it (chunk_load_direct). Before 20 November core 0 only asked and waited, and gave up
   after ten seconds: on the user's Pi 3B core 3 was busy elsewhere (the WiFi's turn joining the
   network) and the solo board's SCSI disk served blocks that were not yet in memory, zeros, while
   the Finder loaded: "Error type 41", and a Restart, with the image complete by then, worked
   (docs/08 item 54). */
static spinlock_t card_lock;
static volatile int card_lock_core3;        /* core 3 holds card_lock: its card pass, which may reach PiDisk's
                                               block reads through a new image's registration (hfs_partition) */
static uint32_t card_slow_us;               /* mac.cardslow=N: N us more per card sector read, the testbed's slow card */
static uint32_t card_fail_block = 0xffffffffu;   /* mac.cardfail=B: the first read covering block B of an image fails (testbed) */
/* the testbed's failing card: one read error, the first time block B's sectors are read, then good */
static int card_fail_now(uint32_t first, uint32_t n)
{
    if (card_fail_block == 0xffffffffu || card_fail_block < first || card_fail_block >= first + n) return 0;
    kprintf("[CARD] mac.cardfail=%u: this read fails (the testbed's failing card)\n", card_fail_block);
    card_fail_block = 0xffffffffu;
    return 1;
}
static void chunk_load_direct(struct img *m, uint32_t c);
static int chunk_retry(struct img *m, uint32_t c);
static void flush_finish(void);
static int chunk_in(const struct img *m, uint32_t c) { return (__atomic_load_n(&m->loaded[c / 32], __ATOMIC_ACQUIRE) >> (c % 32)) & 1; }
/* A chunk the card failed to give is marked loaded, so the loader goes on with the rest, and failed, so
   that the Mac is told: until 21 November 2026 it was handed the zeros in its place as good data (an
   outside review, docs/08 item 92). */
static int chunk_failed(const struct img *m, uint32_t c) { return m->failed && ((__atomic_load_n(&m->failed[c / 32], __ATOMIC_ACQUIRE) >> (c % 32)) & 1); }
static void mark_failed(struct img *m, uint32_t c) { __atomic_fetch_or(&m->failed[c / 32], 1u << (c % 32), __ATOMIC_RELEASE); }
static int load_pick(void)
{
    int wi = __atomic_load_n(&want_img, __ATOMIC_ACQUIRE);
    if (wi >= 0) {
        uint32_t c = want_chunk;
        if (!chunk_in(&imgs[wi], c)) { ld.active = 1; ld.img = wi; ld.chunk = c; ld.done = 0; return 1; }
    }
    /* the smallest image first: a second disk is a PiDisk volume the Finder mounts at once, and
       PiDisk's driver waits only milliseconds for a block; the startup disk's blocks come first
       anyway, as the Mac asks for them (want_img above) */
    int order[4], no = 0;
    for (int i = 0; i < nimg; i++) {
        if (!imgs[i].loaded || imgs[i].nloaded >= imgs[i].nload) continue;
        int j = no++;
        while (j > 0 && imgs[order[j - 1]].len > imgs[i].len) { order[j] = order[j - 1]; j--; }
        order[j] = i;
    }
    for (int o = 0; o < no; o++) {
        int i = order[o];
        struct img *m = &imgs[i];
        for (uint32_t k = 0; k < m->nload; k++) {
            uint32_t c = (ld.cursor + k) % m->nload;
            if (!chunk_in(m, c)) { ld.active = 1; ld.img = i; ld.chunk = c; ld.done = 0; ld.cursor = c + 1; return 1; }
        }
    }
    return 0;
}
static uint8_t lbuf[LSLICE * 512] __attribute__((aligned(16)));
/* one slice of the current chunk; 0 when there is nothing left to load */
static int load_step(void)
{
    if (psmac_recovery_on()) return 0;                 /* recovery entered after the images were found (the switch) */
    if (!ld.active && !load_pick()) return 0;
    struct img *m = &imgs[ld.img];
    uint32_t first = ld.chunk * (LCHUNK / 512u) + ld.done, secs = (m->len + 511u) / 512u;
    uint32_t end = (ld.chunk + 1) * (LCHUNK / 512u); if (end > secs) end = secs;
    uint32_t run = 0, lba = file_lba(m, first * 512u, &run), n = end - first;
    if (n > LSLICE) n = LSLICE;
    if (n > run) n = run;
    int e = lba && n ? -9 : -8;
    for (int tries = 0; tries < 3 && e; tries++) e = lba && n ? sd_read_multi(lba, lbuf, n) : -8;
    if (!e && card_fail_now(first, n)) e = -7;
    if (card_slow_us) udelay(card_slow_us * n);
    if (chunk_in(m, ld.chunk)) { ld.active = 0; return 1; }   /* core 0 read this chunk itself: its copy, and the Mac's writes since, stay */
    if (e) {
        /* the card failed under the image: the chunk is marked failed (the Mac gets an I/O error for
           it, and it is tried again when the Mac next reads it), and nothing of this image is written
           back, so the file on the card is not made worse */
        kprintf("[CARD] %s: read error %d at sector %u (status %x): blocks %u-%u give the Mac an I/O error; nothing is written back to the card\n",
                m->name, e, first, sd.last_hsts, ld.chunk * (LCHUNK / 512u), end - 1);
        mark_failed(m, ld.chunk);
        m->ok = 0; n = end - first;
    } else {
        uint32_t bytes = n * 512u; if (first * 512u + bytes > m->len) bytes = m->len - first * 512u;
        memcpy((uint8_t *)m->ram + first * 512u, lbuf, bytes);
    }
    ld.done += n;
    if (first + n >= end) {
        __atomic_fetch_or(&m->loaded[ld.chunk / 32], 1u << (ld.chunk % 32), __ATOMIC_RELEASE);
        ld.active = 0;
        if (__atomic_add_fetch(&m->nloaded, 1, __ATOMIC_RELAXED) == m->nload) {
            uint32_t ms_ = (uint32_t)((cnt() - m->load_t0) * 1000u / frq());
            kprintf("[CARD] %s: all %u MB read from the card in %u ms (%u KB/s)\n", m->name, m->len >> 20, ms_, ms_ ? (uint32_t)((uint64_t)m->len / ms_) : 0);
            psmac_boot_mark("card: disk image %s loaded (%u MB, %u ms)", m->name, m->len >> 20, ms_);
        }
    }
    return 1;
}
/* [p, p+len) of an image is about to be read or written by the Mac: make sure it is in memory; 0, or -1
   when part of it could not be read from the card (the Mac is to get an I/O error: a read of it would be
   the zeros in its place, a write of part of it would be saved over blocks nobody has seen). On
   core 3 (PiDisk's block service) the chunks are read here and now; from core 0 (the solo board's
   SCSI disk) core 3 is asked and this waits, as the Mac would wait on a disk. */
static inline int on_core3(void) { uint64_t id; __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(id)); return (id & 3) == 3; }
int psmac_card_need(const void *p, uint32_t len)
{
    const uint8_t *b = p;
    int n = __atomic_load_n(&nimg, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        struct img *m = &imgs[i];
        if (!m->loaded || b < m->ram || b >= m->ram + m->len || !len) continue;
        uint32_t c0 = (uint32_t)(b - m->ram) / LCHUNK, c1 = ((uint32_t)(b - m->ram) + len - 1) / LCHUNK;
        int bad = 0;
        for (uint32_t c = c0; c <= c1 && c < m->nload; c++) {
            if (chunk_in(m, c)) {
                if (!chunk_failed(m, c)) continue;
                /* the card failed here before: once more, when the card is free, else an I/O error */
                int mine = on_core3() && card_lock_core3;
                if (mine || spinlock_try_acquire(&card_lock)) {
                    if (chunk_failed(m, c) && chunk_retry(m, c)) bad = 1;
                    if (!mine) spinlock_release(&card_lock);
                } else bad = 1;
                continue;
            }
            if (on_core3()) {
                /* already held when this comes from core 3's own card pass: a second image's
                   registration reads its partition map here (447febf took the lock again and
                   stopped core 3 for good on the user's Pi with two images on the card) */
                int mine = card_lock_core3;
                if (!mine) { spinlock_acquire(&card_lock); card_lock_core3 = 1; }
                flush_finish();                             /* PiDisk's block service comes between passes */
                extern void psmac_mbox_blk_progress(void);   /* PiDisk waits on while this moves (psmac_mbox.c) */
                if (ld.active && !(ld.img == i && ld.chunk == c)) while (ld.active) { psmac_mbox_blk_progress(); load_step(); }   /* the chunk in hand first */
                if (!chunk_in(m, c)) { if (!ld.active) { ld.active = 1; ld.img = i; ld.chunk = c; ld.done = 0; } while (ld.active) { psmac_mbox_blk_progress(); load_step(); } }
                if (!mine) { card_lock_core3 = 0; spinlock_release(&card_lock); }
                if (chunk_failed(m, c)) bad = 1;
                continue;
            }
            /* core 0 (the solo board's SCSI disk): core 3 is asked first, and given two milliseconds;
               then this core reads the chunk itself whenever the card is free. It never serves a
               block that is not in memory while the card answers. */
            want_chunk = c;
            __atomic_store_n(&want_img, i, __ATOMIC_RELEASE);
            uint64_t t0 = cnt(); int said = 0;
            while (!chunk_in(m, c) && sd.state == 1) {
                if (cnt() - t0 > frq() / 500 && spinlock_try_acquire(&card_lock)) {
                    if (!chunk_in(m, c)) chunk_load_direct(m, c);
                    spinlock_release(&card_lock);
                }
                if (!said && cnt() - t0 > frq()) { said = 1; kprintf("[CARD] %s: the Mac waits for block %u from the card\n", m->name, c * (LCHUNK / 512u)); }
                __asm__ volatile("yield");
            }
            __atomic_store_n(&want_img, -1, __ATOMIC_RELEASE);
            if (!chunk_in(m, c)) { kprintf("[CARD] %s: the card stopped answering; block %u is not in memory\n", m->name, c * (LCHUNK / 512u)); return -1; }
            if (chunk_failed(m, c)) bad = 1;
        }
        return bad ? -1 : 0;
    }
    return 0;
}
/* PiDisk's boot record asks for the startup disk only once the card has been looked at (psmac_mbox.c) */
int psmac_card_scanned(void) { return __atomic_load_n(&scan_done, __ATOMIC_ACQUIRE); }
/* the solo board's first SCSI selection waits for the card's first look (a few tens of ms after
   the 68k starts), so a disk image on the card is there when the ROM looks for a startup disk */
void psmac_card_scan_wait(void)
{
    uint64_t t0 = cnt();
    while (!__atomic_load_n(&scan_done, __ATOMIC_ACQUIRE) && cnt() - t0 < 3 * frq()) __asm__ volatile("yield");
}

/* the flush: one sector in flight at a time. cpio aligns a file's data to 4 bytes, not 512, so an
   image inside the container straddles card sectors: whole card sectors are written, copied from
   the file in memory, whose bytes around the image are the unchanged cpio headers. */
static struct { int busy; int img; uint32_t sec, last; } fl;
static uint8_t wbuf[512] __attribute__((aligned(16)));
static void flush_poll(void)
{
    if (sd.ro) return;
    if (fl.busy) {
        int k = sd_write_poll();
        if (k == 0) return;
        fl.busy = 0;
        if (k < 0) { kprintf("[CARD] write failed (status %x): %s's writes stop here\n", sd.last_hsts, imgs[fl.img].name); imgs[fl.img].ok = 0; fl.img = -1; return; }
        imgs[fl.img].written++;
        if (++fl.sec <= fl.last) goto next_sector;
        fl.img = -1;
    }
    if (fl.img < 0 || fl.img >= nimg) {
        /* a new chunk: only once the Mac has been quiet a second, or five seconds after the first change */
        uint64_t now = cnt(), q = last_dirty_t, f0 = first_dirty_t;
        if (!f0 || (now - q < frq() && now - f0 < 5 * frq())) return;
        for (int i = 0; i < nimg; i++) {
            struct img *m = &imgs[i];
            if (!m->ok) continue;
            for (uint32_t wd = 0; wd < (m->nbits + 31) / 32; wd++) {
                uint32_t v = m->dirty[wd];
                if (!v) continue;
                uint32_t b = (uint32_t)__builtin_ctz(v), c = wd * 32 + b;
                __atomic_fetch_and(&m->dirty[wd], ~(1u << b), __ATOMIC_ACQUIRE);   /* cleared before the copy: a write after it marks it again */
                uint32_t s = m->off + c * CHUNK, e = m->off + (c * CHUNK + CHUNK < m->len ? c * CHUNK + CHUNK : m->len);
                fl.img = i; fl.sec = s / 512u; fl.last = (e - 1) / 512u;
                goto next_sector;
            }
        }
        first_dirty_t = 0;                             /* everything is on the card */
        return;
    }
next_sector:
    {
        struct img *m = &imgs[fl.img];
        uint32_t at = fl.sec * 512u;                       /* a card sector of the image's file */
        for (uint32_t j = 0; j < 512; j++) wbuf[j] = at + j < m->file_len ? m->file[at + j] : 0;
        uint32_t lba = file_lba(m, at, 0);
        if (!lba) { kprintf("[CARD] %s: offset %u outside the file: writes stop\n", m->name, at); m->ok = 0; fl.img = -1; return; }
        if (sd_write_start(lba, wbuf)) { kprintf("[CARD] write could not start (status %x): %s's writes stop\n", sd.last_hsts, m->name); m->ok = 0; fl.img = -1; return; }
        fl.busy = 1;
    }
}

/* core 0's read of one chunk, the card lock held: a sector being programmed is finished first (the
   flush goes on from the next one on core 3's next pass), then the chunk in one command per extent
   run. Core 3's loader leaves a chunk alone once its bit is set (load_step). */
static uint8_t dbuf[LCHUNK] __attribute__((aligned(16)));
/* a sector still being programmed is finished before anything else is read from the card */
static void flush_finish(void)
{
    while (fl.busy) {
        int k = sd_write_poll();
        if (k == 0) continue;
        fl.busy = 0;
        if (k < 0) { kprintf("[CARD] write failed (status %x): %s's writes stop here\n", sd.last_hsts, imgs[fl.img].name); imgs[fl.img].ok = 0; fl.img = -1; break; }
        imgs[fl.img].written++;
        if (++fl.sec > fl.last) fl.img = -1;
    }
}
/* chunk c of the image read from the card into memory, whole: 0, or -1 if the card failed under it (the
   failure logged). The caller holds the card lock. */
static int chunk_read(struct img *m, uint32_t c)
{
    flush_finish();
    uint32_t first = c * (LCHUNK / 512u), secs = (m->len + 511u) / 512u, end = first + LCHUNK / 512u;
    if (end > secs) end = secs;
    int bad = 0;
    for (uint32_t s = first; s < end; ) {
        uint32_t run = 0, lba = file_lba(m, s * 512u, &run), n = end - s;
        if (n > run) n = run;
        int e = lba && n ? -9 : -8;
        for (int tries = 0; tries < 3 && e; tries++) e = lba && n ? sd_read_multi(lba, dbuf, n) : -8;
        if (!e && card_fail_now(s, n)) e = -7;
        if (card_slow_us) udelay(card_slow_us * n);
        if (e) {
            kprintf("[CARD] %s: read error %d at sector %u (status %x): blocks %u-%u give the Mac an I/O error; nothing is written back to the card\n",
                    m->name, e, s, sd.last_hsts, first, end - 1);
            bad = 1; if (!n) n = end - s;
        }
        else { uint32_t bytes = n * 512u; if (s * 512u + bytes > m->len) bytes = m->len - s * 512u; memcpy((uint8_t *)m->ram + s * 512u, dbuf, bytes); }
        s += n;
    }
    return bad ? -1 : 0;
}
static void chunk_load_direct(struct img *m, uint32_t c)
{
    if (chunk_read(m, c)) { mark_failed(m, c); m->ok = 0; }
    __atomic_fetch_or(&m->loaded[c / 32], 1u << (c % 32), __ATOMIC_RELEASE);
    if (__atomic_add_fetch(&m->nloaded, 1, __ATOMIC_RELAXED) == m->nload)
        kprintf("[CARD] %s: all %u MB read from the card\n", m->name, m->len >> 20);
}
/* A failed chunk tried again when the Mac reads it: unless the Mac has written into it since (its writes
   are in memory and a fresh copy from the card would lose them), the card is read once more, and if it
   answers now the chunk is good. 0 when the chunk is good. The caller holds the card lock. */
static int chunk_retry(struct img *m, uint32_t c)
{
    uint32_t d0 = c * (LCHUNK / CHUNK), d1 = d0 + LCHUNK / CHUNK;
    for (uint32_t d = d0; d < d1 && d < m->nbits; d++)
        if ((m->dirty[d / 32] >> (d % 32)) & 1) return -1;
    if (chunk_read(m, c)) return -1;
    __atomic_fetch_and(&m->failed[c / 32], ~(1u << (c % 32)), __ATOMIC_RELEASE);
    kprintf("[CARD] %s: blocks %u-%u read from the card on a second try\n", m->name, c * (LCHUNK / 512u), (c + 1) * (LCHUNK / 512u) - 1);
    return 0;
}

/* ==== the exchange folder: a folder of the card as an HFS volume for the Mac (issue #20) =========
   A folder in the card's root, "Mac Exchange" unless mac.exchange= names another (off: none), is shown to
   the Mac as a disk of its own: a locked HFS volume psmac_hfsview.c makes of the folder's files, with the
   resource forks, types and creators of the AppleDouble "._" files a modern Mac writes beside them. Only
   the volume's catalog lives in memory; its files' blocks are read from the card when the Mac reads them
   (psmac_card_view_read, from PiDisk's block service on this core). The volume is made a step at a time,
   every other pass of core 3 (each step reads at most about 30 KB), so a folder of thousands of files
   holds up neither the mailbox nor a disk image coming in. The card's own FAT code above stays the writer
   of the kernel's files; this is psmac_fatfs.c reading the same partition, nothing is written. */
static struct { int state; struct fx_vol *fx; struct hv *hv; uint32_t t0; char tag[48]; } xv;   /* state 0 none, 1 making, 2 ready, -1 failed */
static void *xv_mem(uint32_t n)
{
    if (tlsf_get_free_size(tlsf) < (uintptr_t)n + (8u << 20))
        return 0;                                      /* 8 MB of the kernel's heap are always left to the rest */
    return tlsf_malloc_aligned(tlsf, n ? n : 4, 64);
}
static void xv_free(void *p) { if (p) tlsf_free(tlsf, p); }
/* psmac_fatfs.c's sectors from the card: on core 3, the card lock taken unless this core's card pass holds it */
static int card_fx_read(void *ctx, uint32_t lba, uint32_t n, uint8_t *buf)
{
    (void)ctx;
    int mine = card_lock_core3, e = 0;
    if (!mine) { spinlock_acquire(&card_lock); card_lock_core3 = 1; }
    flush_finish();
    while (n && !e) {
        uint32_t k = n > 64 ? 64 : n;
        e = -1;
        for (int t = 0; t < 3 && e; t++)
            e = sd_read_multi(lba, buf, k);
        lba += k; buf += k * 512u; n -= k;
    }
    if (!mine) { card_lock_core3 = 0; spinlock_release(&card_lock); }
    return e ? -1 : 0;
}
static void exchange_start(void)
{
    const char *name = psmac_option("mac.exchange"), *given = name;
    if (!name)
        name = "Mac Exchange";
    if (name_eq(name, "off"))
        return;
    struct fx_dir *d = xv_mem(sizeof *d);
    struct fx_dirent *e = xv_mem(sizeof *e);
    int found = 0;
    xv.fx = xv_mem(sizeof *xv.fx);
    if (!d || !e || !xv.fx || fx_mount(xv.fx, card_fx_read, 0, fs.part)) {
        kprintf("[CARD] %s: the card's FAT could not be read for it (no memory, or a boot sector psmac_fatfs.c does not take)\n", name);
        goto out;
    }
    fx_dir_open(d, xv.fx, 0);
    while (!found && fx_dir_next(d, e) == 1) {
        if (!(e->attr & FX_ATTR_DIR))
            continue;
        int i = 0;
        while (i < e->nlen && name[i] && e->name[i] < 128 && lower(e->name[i]) == lower(name[i]))
            i++;
        found = i == e->nlen && !name[i];
    }
    if (!found) {
        kprintf("[CARD] no folder \"%s\" in the card's root: %s\n", name,
                given ? "no exchange volume for the Mac" : "make one for files to show on the Mac (mac.exchange=NAME for another name)");
        goto out;
    }
    unsigned k = 0;
    for (const char *a = "[CARD] "; *a; ) xv.tag[k++] = *a++;
    for (int i = 0; i < e->nlen && k < sizeof xv.tag - 1; i++) xv.tag[k++] = (char)e->name[i];
    xv.tag[k] = 0;
    xv.hv = xv_mem(sizeof *xv.hv);
    if (!xv.hv) {
        kprintf("%s: no memory to show it to the Mac\n", xv.tag);
        goto out;
    }
    hv_init(xv.hv, xv.fx, xv_mem, xv_free, kprintf, xv.tag);
    hv_set_root(xv.hv, e);
    xv.state = 1;
    xv.t0 = ms();
    kprintf("%s: the folder is being read, to be shown to the Mac as a locked HFS volume\n", xv.tag);
out:
    xv_free(d);
    xv_free(e);
    if (xv.state != 1) { xv_free(xv.fx); xv.fx = 0; }
}
static void exchange_step(void)
{
    int r = hv_step(xv.hv);
    if (!r)
        return;
    if (r < 0) {
        kprintf("%s: cannot be shown to the Mac\n", xv.tag);
        hv_release(xv.hv);
        xv_free(xv.hv); xv.hv = 0;
        xv.state = -1;
        return;
    }
    xv.state = 2;
    hv_report(xv.hv, ms() - xv.t0);
    extern void psmac_mbox_add_view(const char *name, uint32_t blocks);
    psmac_mbox_add_view(xv.hv->vname_c, xv.hv->total);
}
/* PiDisk's block service (psmac_mbox.c, core 3): blocks of the exchange volume. 0, or -1 */
int psmac_card_view_read(uint32_t lba, uint32_t n, uint8_t *buf)
{
    if (xv.state != 2 || sd.state != 1)
        return -1;
    return hv_read(xv.hv, lba, n, buf);
}

/* ==== core 3 ================================================================================== */
static uint32_t start_at;
static void card_start(void);
static void card_poll_locked(void);
void psmac_card_poll(void)
{
    if (!spinlock_try_acquire(&card_lock)) return;       /* core 0 is reading a chunk the Mac waits for */
    card_lock_core3 = 1;
    card_poll_locked();
    card_lock_core3 = 0;
    spinlock_release(&card_lock);
    /* mac.cardstall=S (testbed): once, as soon as an image is coming in, keep core 3 away from the
       card for S seconds, as the WiFi's turn did on the user's Pi; the Mac must still get its blocks */
    static int stall = -1;
    if (stall < 0) { const char *o = psmac_option("mac.cardstall"); stall = 0; if (o) while (*o >= '0' && *o <= '9') stall = stall * 10 + (*o++ - '0'); }
    if (stall > 0 && nimg && imgs[0].loaded && imgs[0].nloaded) {
        kprintf("[CARD] mac.cardstall=%d: core 3 kept away from the card for %d s\n", stall, stall);
        udelay((uint32_t)stall * 1000000u); stall = 0;
    }
}
/* the early look for declaration ROMs (below): the card was initialised and mounted on core 0 before
   core 3 began, and card_start takes its results rather than resetting the card a second time */
static int early_done, early_init_rc, early_mount_rc;
static volatile int sf_pending; static void sfile_step(void);     /* small files of the kernel's own (below) */
static int boot_jobs(void);                                       /* boot.txt and recovery.txt (Aurora68 Setup and Recovery, at the end) */
static void card_poll_locked(void)
{
    if (sd.state == 0 || early_done == 1) {
        /* card_start takes the early look's results only when there was one (early_done 1); set to 2
           before the call (as at first), a card with no early look (a testbed run whose initrd is a bare
           ROM) was never initialised or mounted: "FAT16 at sector 0, 0-byte clusters", no disk image */
        card_start();
        early_done = 2;
        __atomic_store_n(&scan_done, 1, __ATOMIC_RELEASE);
        return;
    }
    if (sd.state != 1) return;
    if (fl.busy) { flush_poll(); return; }             /* a sector is being programmed: nothing else touches the card */
    if (boot_jobs()) return;
    if (psmac_recovery_on()) return;                   /* a recovery start: the card is the recovery's alone (psmac_recovery.c) */
    if (__atomic_load_n(&pair_dirty, __ATOMIC_ACQUIRE) && !sd.ro) { pair_dirty = 0; pairings_write(); return; }
    if (__atomic_load_n(&opt_state, __ATOMIC_ACQUIRE) == 1 && !sd.ro) { cmdline_apply(); return; }
    { extern int psmac_solo_pram_take(uint8_t out[260]), psmac_se_pram_take(uint8_t out[260]); static uint8_t p[260];
      if (!sd.ro && (psmac_solo_pram_take(p) || psmac_se_pram_take(p))) { pram_write(p); return; } }
    if (__atomic_load_n(&sf_pending, __ATOMIC_ACQUIRE) && !sd.ro) { sfile_step(); return; }
    if (__atomic_load_n(&logf_state, __ATOMIC_ACQUIRE) == 1 && !sd.ro) { logf_step(); return; }
    if (logsave_s && !sd.ro && ms() - logsave_last >= logsave_s * 1000u) {
        logsave_last = ms();
        logf_req = 3;
        __atomic_store_n(&logf_state, 1, __ATOMIC_RELEASE);
        return;
    }
    /* a disk image still coming in from the card, and the Mac's changes going back: every other pass
       each, so writes made while a large image loads are on the card long before it is complete */
    static unsigned turn;
    static unsigned xturn;
    if (xv.state == 1 && (++xturn & 1)) { exchange_step(); return; }   /* the exchange folder, every other pass */
    if ((++turn & 1) && first_dirty_t) { flush_poll(); if (fl.busy) return; }
    if (load_step()) return;
    flush_poll();
}
/* the card's first look, on core 3's first pass: the card, its FAT, pairings.txt, the container, an image */
static void card_start(void)
{
    if (!start_at) start_at = ms() ? ms() : 1;
    { const char *o = psmac_option("mac.cardfail"); if (o) { uint32_t v = 0; while (*o >= '0' && *o <= '9') v = v * 10 + (uint32_t)(*o++ - '0'); card_fail_block = v; kprintf("[CARD] mac.cardfail=%u: the first read of block %u fails (the testbed's failing card)\n", v, v); } }
    { const char *o = psmac_option("mac.cardslow"); if (o) { uint32_t v = 0; while (*o >= '0' && *o <= '9') v = v * 10 + (uint32_t)(*o++ - '0'); card_slow_us = v; kprintf("[CARD] mac.cardslow=%u: %u us more per sector read (a slow card, for the testbed)\n", v, v); } }
    const char *o = psmac_option("mac.card");
    if (o && o[0] == 'o' && o[1] == 'f') { sd.state = -2; pair_state = -1; kprintf("[CARD] off (mac.card=off)\n"); return; }
    if (alice_pi_is_2711()) { sd.state = -1; pair_state = -1; kprintf("[CARD] the Pi 4's card controller is not supported yet: nothing saved\n"); return; }
    sd.ro = o && o[0] == 'r' && o[1] == 'o';
    int e = early_done ? early_init_rc : sd_init();
    if (e) { sd.state = -1; pair_state = -1; kprintf("[CARD] the SD card does not answer on the sdhost controller (%d, status %x)\n", e, sd.last_hsts); return; }
    sd.state = 1;
    e = early_done ? early_mount_rc : fs_mount();
    if (e) { sd.state = -1; pair_state = -1; kprintf("[CARD] no FAT16/FAT32 partition found (%d)\n", e); return; }
    kprintf("[CARD] %s card, FAT%d at sector %u, %u-byte clusters%s\n", sd.ccs ? "SDHC/SDXC" : "SDSC", fs.fat32 ? 32 : 16, fs.part, fs.spc * 512u, sd.ro ? ", read only (mac.card=ro)" : "");
    { extern int psmac_board_se, psmac_board_solo;
      const char *ls = psmac_option("mac.logsave");
      logsave_s = psmac_board_se && !psmac_board_solo ? 30 : 0;
      if (ls) { logsave_s = 0; while (*ls >= '0' && *ls <= '9') logsave_s = logsave_s * 10 + (uint32_t)(*ls++ - '0'); }
      logsave_last = ms();
      if (logsave_s && !sd.ro) kprintf("[CARD] the log goes into aurora68.log every %u s (mac.logsave=%u; 0 stops it)\n", logsave_s, logsave_s); }
    pairings_load();
    pram_load();
    if (psmac_recovery_on()) {
        /* Aurora68 Setup and Recovery: no disk image loaded, nothing of the Mac's written, and aurora68.log left as
           the last start wrote it, since the recovery page shows it */
        logsave_s = 0;
        fl.img = -1;
        kprintf("[CARD] a recovery start: no disk images loaded, aurora68.log kept as the last start left it\n");
        return;
    }
    container_check();
    image_scan();
    exchange_start();
    psmac_boot_mark("card: mounted, pairings read, disk images found");
    fl.img = -1;
    { static const char test[] = "# mac.cardtest\nbt.dev=001122334455:00112233445566778899aabbccddeeff:2\n";
      const char *t = psmac_option("mac.cardtest"); if (t && t[0] == '1') psmac_card_save_pairings(test, sizeof test - 1); }
}
/* ==== small files of the kernel's own, with long names: aurora68-display.txt and .bak ===========
   PiDisplay's HDMI settings (psmac_hdmi.c) go into aurora68-display.txt, which config.txt includes, and
   the file before into aurora68-display.bak. Both names are longer than 8.3, and the firmware finds
   config.txt's include by its long name, so a file that is not there yet is made with VFAT long-name
   entries: the pieces of the name (13 UTF-16 characters each, the last first, the 0x40 bit on it), each
   carrying the checksum of the short name, then the short entry itself (AURORA~1.TXT, the first number
   no other file has), all in one directory sector, which is written in one go. A file is at most its
   first cluster (the texts are under 1 KB, buscheck.txt under 4 KB, clusters 4 KB or more on any card the firmware boots), so
   nothing but the directory and that one cluster is ever written; the text is read back. Jobs are
   queued from any core (psmac_card_put_file) and done one per pass of core 3, in order. */
struct sfjob { char name[32]; char text[4096]; uint32_t len; };   /* 4 KB: the bus check's buscheck.txt (psmac_buscheck.c), one cluster of any card the firmware boots */
static struct sfjob sf_q[3]; static volatile int sf_head, sf_n; static volatile int sf_result;   /* 0 none, 1 busy, 2 done, 3 failed */
static uint8_t lfn_sum(const uint8_t *sn) { uint8_t c = 0; for (int i = 0; i < 11; i++) c = (uint8_t)(((c & 1) << 7) + (c >> 1) + sn[i]); return c; }
struct run_ctx { uint32_t want, lba, off; };
static int run_sector(uint32_t lba, void *vctx)
{
    struct run_ctx *x = vctx;
    if (sd_read(lba, sec)) return -1;
    uint32_t run = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t c = sec[32 * i];
        if (c == 0 || c == 0xe5) { if (++run == x->want) { x->lba = lba; x->off = 32u * (uint32_t)(i + 1 - (int)x->want); return 1; } }
        else run = 0;
    }
    return 0;
}
static int lfn_create(const char *name, struct cfile *f)
{
    uint32_t n = 0; while (name[n]) n++;
    uint32_t dot = n; while (dot && name[dot - 1] != '.') dot--;
    if (!dot || n > 64) return -1;
    /* the short name: the first six letters of the long one, ~N, and its extension, upper case */
    uint8_t sn[11]; char probe[13];
    for (int i = 0; i < 11; i++) sn[i] = ' ';
    int k = 0;
    for (uint32_t i = 0; i < dot - 1 && k < 6; i++) { char c = name[i]; if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) sn[k++] = (uint8_t)(c >= 'a' && c <= 'z' ? c - 32 : c); }
    for (uint32_t i = dot, j = 8; i < n && j < 11; i++, j++) sn[j] = (uint8_t)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
    int num = 1;
    for (; num < 10; num++) {
        int p = 0;
        for (int i = 0; i < k; i++) probe[p++] = (char)sn[i];
        probe[p++] = '~'; probe[p++] = (char)('0' + num); probe[p++] = '.';
        for (int i = 8; i < 11 && sn[i] != ' '; i++) probe[p++] = (char)sn[i];
        probe[p] = 0;
        struct cfile t;
        if (file_find(probe, &t)) break;                               /* nobody has this short name */
    }
    if (num >= 10) return -2;
    sn[k] = '~'; sn[k + 1] = (uint8_t)('0' + num);
    uint32_t pieces = (n + 12) / 13;
    struct run_ctx x; x.want = pieces + 1;
    if (root_walk(run_sector, &x) != 1) return -3;                     /* no room in one sector of the root directory */
    uint32_t c = clus_alloc();
    if (!c) return -4;
    if (sd_read(x.lba, sec)) return -5;
    uint8_t sum = lfn_sum(sn);
    static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (uint32_t p = 0; p < pieces; p++) {
        uint32_t seq = pieces - p;                                     /* the last piece first */
        uint8_t *e = sec + x.off + 32 * p;
        for (int i = 0; i < 32; i++) e[i] = 0;
        e[0] = (uint8_t)(seq | (p == 0 ? 0x40 : 0)); e[11] = 0x0f; e[13] = sum;
        for (int i = 0; i < 13; i++) {
            uint32_t ci = (seq - 1) * 13 + (uint32_t)i, ch = ci < n ? (uint8_t)name[ci] : ci == n ? 0 : 0xffff;
            put16(e + at[i], ch);
        }
    }
    uint8_t *e = sec + x.off + 32 * pieces;
    for (int i = 0; i < 32; i++) e[i] = 0;
    for (int i = 0; i < 11; i++) e[i] = sn[i];
    e[11] = 0x20;
    uint32_t date = (uint32_t)((2026 - 1980) << 9 | 11 << 5 | 21);
    put16(e + 16, date); put16(e + 18, date); put16(e + 24, date);
    put16(e + 20, c >> 16); put16(e + 26, c & 0xffff);
    if (sd_write(x.lba, sec)) return -6;
    f->dir_lba = x.lba; f->dir_off = x.off + 32 * pieces; f->clus = c; f->size = 0;
    kprintf("[CARD] %s created (short name %.8s.%.3s)\n", name, (const char *)sn, (const char *)sn + 8);
    return 0;
}
static void sfile_step(void)
{
    struct sfjob *j = &sf_q[sf_head];
    struct cfile f;
    int e = 0;
    uint32_t cap = fs.spc * 512u;
    if (j->len > cap) e = -10;
    else if (file_find(j->name, &f)) e = lfn_create(j->name, &f);
    if (!e && f.clus < 2) {                                            /* an empty file made elsewhere: no cluster yet */
        uint32_t c = clus_alloc();
        if (!c || sd_read(f.dir_lba, sec)) e = -11;
        else { put16(sec + f.dir_off + 20, c >> 16); put16(sec + f.dir_off + 26, c & 0xffff); if (sd_write(f.dir_lba, sec)) e = -12; else f.clus = c; }
    }
    for (uint32_t i = 0; !e && (i * 512u < j->len || i == 0); i++) {
        for (uint32_t b = 0; b < 512; b++) sec[b] = i * 512u + b < j->len ? (uint8_t)j->text[i * 512u + b] : 0;
        if (sd_write(clus_lba(f.clus) + i, sec)) e = -13;
    }
    if (!e) { if (sd_read(f.dir_lba, sec)) e = -14; else { put32(sec + f.dir_off + 28, j->len); if (sd_write(f.dir_lba, sec)) e = -15; } }
    for (uint32_t i = 0; !e && i * 512u < j->len; i++) {               /* read back */
        if (sd_read(clus_lba(f.clus) + i, sec)) { e = -16; break; }
        for (uint32_t b = 0; b < 512 && i * 512u + b < j->len; b++) if (sec[b] != (uint8_t)j->text[i * 512u + b]) { e = -17; break; }
    }
    if (e) kprintf("[CARD] %s: not written (%d%s)\n", j->name, e, e == -3 ? ": no room in the root directory" : e == -10 ? ": longer than a cluster" : "");
    else kprintf("[CARD] %s: %u bytes written and read back\n", j->name, j->len);
    sf_head = (sf_head + 1) % 3;
    if (e) sf_result = 3;
    if (--sf_n == 0) { if (sf_result != 3) sf_result = 2; __atomic_store_n(&sf_pending, 0, __ATOMIC_RELEASE); }
}
/* queue a small file (psmac_hdmi.c): 0 queued, -1 no writable card, -2 the queue is full, -3 too long.
   Jobs queued together are reported together by psmac_card_files_state. */
int psmac_card_put_file(const char *name, const char *text, uint32_t len)
{
    if (sd.state < 0 || sd.ro) return -1;
    if (sf_n >= 3) return -2;
    if (len > sizeof sf_q[0].text) return -3;
    struct sfjob *j = &sf_q[(sf_head + sf_n) % 3];
    uint32_t i = 0; while (name[i] && i + 1 < sizeof j->name) { j->name[i] = name[i]; i++; } j->name[i] = 0;
    for (uint32_t k = 0; k < len; k++) j->text[k] = text[k];
    j->len = len;
    if (!sf_pending) sf_result = 1;
    sf_n++;
    __atomic_store_n(&sf_pending, 1, __ATOMIC_RELEASE);
    return 0;
}
int psmac_card_files_state(void) { return sf_result; }              /* 0 none, 1 being written, 2 written, 3 failed */
int psmac_card_writable(void) { return sd.state == 1 && !sd.ro; }
uint32_t psmac_card_cluster_bytes(void) { return sd.state == 1 ? fs.spc * 512u : 0; }   /* a small file's limit (psmac_buscheck.c), 0 while the card is not mounted */
/* nothing waits to go onto the card: no small file, no change of a disk image, no sector being
   programmed, no log or pairings (the Pi is about to restart, psmac_hdmi.c) */
int psmac_card_idle(void)
{
    return !sf_pending && !first_dirty_t && !fl.busy && logf_state != 1 && !pair_dirty && opt_state != 1;
}

/* ---- declaration ROMs in the card's root ------------------------------------------------------
   psmac_bus.c asks once, on core 0 in psmac_setup_memory, before the Slot Manager can read slot 9 and
   before core 3 starts (psmac_alice_init comes after): the card is initialised and mounted here, and
   card_start later takes that over. Only for a Pi that booted from a container (on hardware it always
   does; psmac_bus.c decides), so that a testbed run with no card does not wait for a card that is not
   there. list gets each root file whose name ends in .declrom (its long name when it has one), at
   most 64 KiB; read then fetches the one psmac_bus.c chose. */
struct dr_ctx { void (*cb)(const char *name, uint32_t size); char lfn[128]; int lfn_ok; uint64_t t0; int late; };
static int declrom_sector(uint32_t lba, void *vctx)
{
    struct dr_ctx *x = vctx;
    if (cnt() - x->t0 > frq()) { x->late = 1; return 2; }    /* a second for the whole look */
    if (sd_read(lba, sec)) return -1;
    for (int i = 0; i < 16; i++) {
        const uint8_t *e = sec + 32 * i;
        if (e[0] == 0) return 2;                                       /* the end of the directory */
        if (e[0] == 0xe5) { x->lfn_ok = 0; continue; }
        if (e[11] == 0x0f) {                                           /* a long-name piece, as in find_sector */
            unsigned seq = e[0] & 0x1f, base = (seq - 1) * 13;
            if (e[0] & 0x40) { for (unsigned k = 0; k < sizeof x->lfn; k++) x->lfn[k] = 0; x->lfn_ok = 1; }
            static const uint8_t at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            for (unsigned k = 0; k < 13; k++) { unsigned ch = le16(e + at[k]); if (base + k < sizeof x->lfn - 1) x->lfn[base + k] = (char)(ch == 0xffff ? 0 : ch); }
            continue;
        }
        if (e[11] & 0x18) { x->lfn_ok = 0; continue; }                 /* volume label, directory */
        char sn[13]; unsigned n = 0;
        for (int k = 0; k < 8 && e[k] != ' '; k++) sn[n++] = (char)e[k];
        if (e[8] != ' ') { sn[n++] = '.'; for (int k = 8; k < 11 && e[k] != ' '; k++) sn[n++] = (char)e[k]; }
        sn[n] = 0;
        const char *shown = x->lfn_ok ? x->lfn : sn;
        uint32_t size = le32(e + 28);
        x->lfn_ok = 0;
        if (ends_ci(shown, ".declrom") && !(shown[0] == '.' && shown[1] == '_') && size >= 20 && size <= PSMB_DECLROM_SIZE)
            x->cb(shown, size);
    }
    return 0;
}
/* The early look is bounded (21 November 2026, docs/08 item 79): each card command has its limit
   already, and the root's walk stops after a second. Whatever fails, the card is left as if it had
   never been touched (early_done 0, sd.state 0), so card_start on core 3 starts it afresh with its
   own reset, and the container's declaration ROM is used meanwhile. */
static uint64_t early_t0;
static int early_failed;                    /* tried once and failed: not tried again before core 3's start */
static void early_undo(const char *why, uint64_t t0)
{
    kprintf("[CARD] the card's first look, before the 68k starts (%u ms): %s; the container's declaration ROM is used, and the card is started again later\n",
            (uint32_t)((cnt() - t0) * 1000u / frq()), why);
    early_done = 0; early_init_rc = 0; early_mount_rc = 0;
    sd.state = 0;
    early_failed = 1;
}
/* the card initialised and mounted on core 0, once, before core 3 starts: for the declaration ROMs below
   and for psmac_hdmi.c's look at config.txt and aurora68-display.txt (PiDisplay). 0 when it is mounted */
int psmac_card_early(void)
{
    if (early_done) return early_init_rc || early_mount_rc ? -1 : 0;
    if (early_failed) return -1;
    const char *o = psmac_option("mac.card");
    if ((o && o[0] == 'o' && o[1] == 'f') || alice_pi_is_2711()) return -1;   /* card_start says why */
    uint64_t t0 = early_t0 = cnt();
    early_phase = 1;
    early_init_rc = sd_init();
    early_phase = 0;
    if (early_init_rc) { early_undo("the card does not answer", t0); return -1; }
    sd.state = 1;
    early_mount_rc = fs_mount();
    if (early_mount_rc) { early_undo("no FAT partition", t0); return -1; }
    early_done = 1;
    kprintf("[CARD] the card's first look, before the 68k starts (%u ms): the card is mounted\n", (uint32_t)((cnt() - t0) * 1000u / frq()));
    return 0;
}
int psmac_card_declroms(void (*cb)(const char *name, uint32_t size))
{
    if (psmac_card_early()) return -1;
    uint64_t t0 = early_t0;
    struct dr_ctx x; x.cb = cb; x.lfn_ok = 0; x.t0 = cnt(); x.late = 0;     /* a second for the walk itself */
    root_walk(declrom_sector, &x);
    if (x.late)
        kprintf("[CARD] the card's root took more than a second to read: the declaration ROMs found so far are offered\n");
    kprintf("[CARD] looked for declaration ROMs in the card's root (%u ms): the card is mounted\n", (uint32_t)((cnt() - t0) * 1000u / frq()));
    return 0;
}
/* the file's bytes into buf (at most max): its length, or -1 */
int32_t psmac_card_read_file(const char *name, uint8_t *buf, uint32_t max)
{
    struct cfile f;
    if (early_init_rc || early_mount_rc || sd.state != 1 || file_find(name, &f) || f.clus < 2 || f.size > max) return -1;
    uint32_t c = f.clus, got = 0;
    while (got < f.size && c >= 2 && !eoc(c)) {
        for (uint32_t i = 0; i < fs.spc && got < f.size; i++) {
            if (sd_read(clus_lba(c) + i, sec)) return -1;
            uint32_t k = f.size - got < 512 ? f.size - got : 512;
            for (uint32_t j = 0; j < k; j++) buf[got + j] = sec[j];
            got += k;
        }
        c = fat_get(c);
    }
    return got == f.size ? (int32_t)got : -1;
}

void psmac_card_report(void)
{
    if (sd.state == 0 || sd.state == -2) return;
    if (sd.state < 0) return;
    kprintf("[CARD]   %u reads, %u writes, %u errors; marks %u (%u outside the images, last %p, image 0 at %p)", sd.reads, sd.writes, sd.errors,
            dirty_calls, dirty_miss, dirty_last, nimg ? (const void *)imgs[0].ram : 0);
    for (int i = 0; i < nimg; i++) {
        uint32_t d = 0; for (uint32_t w2 = 0; w2 < (imgs[i].nbits + 31) / 32; w2++) d += (uint32_t)__builtin_popcount(imgs[i].dirty[w2]);
        kprintf(" | %s: %u KB written, %u KB waiting%s", imgs[i].name, imgs[i].written / 2, d * (CHUNK / 1024), imgs[i].ok ? "" : ", writes off");
        if (imgs[i].loaded && imgs[i].nloaded < imgs[i].nload) kprintf(", %u%% read from the card", imgs[i].nloaded * 100u / imgs[i].nload);
    }
    kprintf("\n");
}

/* ==== Aurora68 Setup and Recovery: the card's root as a file manager sees it (22 November 2026) ============
   The recovery start (psmac_recovery.c, docs/08 item 106) lists, reads, writes, renames and deletes any
   file in the card's root, of any size: a new kernel8.img, a disk image of a hundred megabytes, a
   declaration ROM, cmdline.txt. Everything above this point only ever rewrote files of its own in place;
   this part makes and removes directory entries and cluster chains, so it is written around one rule:
   a power cut at any moment leaves a card the firmware still boots. That comes down to the order of
   the writes, each of which is one 512-byte sector (an SD card programs a sector whole or not at all, as
   far as a file system can rely on anything):
   - a new file's directory entry is written first, empty (size 0, no cluster); its data, then its chain
     in the FAT (each cluster marked the end of the chain before the one in front is joined to it), and
     only then is its entry given the first cluster and the size. Cut before that, it is an empty file and
     a few clusters marked used that nothing owns: space lost, nothing broken.
   - a file's content is replaced by changing its own short entry's first cluster and size, one sector
     write. The name never goes away, so kernel8.img is on the card at every instant. Where that change
     and its partner's (the temporary file, or kernel8.old) are in two sectors, there is a moment between
     the two writes when both names start with the same cluster: psmac_card_rec_repair, at the next
     recovery start, finds such a pair among the temporary names and takes the temporary's entry away
     without touching its clusters; and a delete never frees a chain that another name starts with.
   FAT16 and FAT32 (the root's fixed area or its chain, which grows by a cluster when full); long names
   read and written (VFAT), a name that fits 8.3 written as 8.3 with the lower-case bits Windows uses.
   Every function here runs on core 3 (the recovery's service loop) or on core 0 before core 3 starts,
   and takes the card lock for its whole length. */
static uint8_t rsec[512] __attribute__((aligned(16)));         /* the directory walk's sector */
static uint8_t dsec[512] __attribute__((aligned(16)));         /* a directory sector being changed */
static uint8_t fwbuf[512] __attribute__((aligned(16)));        /* the FAT sector being changed (write-back) */
static uint32_t fw_lba = 0xffffffffu; static int fw_dirty;
static uint32_t alloc_hint;
static int multi_bad;                                          /* CMD25 failed once: single-sector writes from then on */
static char rec_err[120];
const char *psmac_card_rec_error(void) { return rec_err; }
static void rerr(const char *s) { unsigned i = 0; for (; s[i] && i < sizeof rec_err - 1; i++) rec_err[i] = s[i]; rec_err[i] = 0; }
/* In a normal start the kernel's other writers (clus_alloc, fat_set) change the FAT between these
   operations, so the FAT sector kept here is read again each time; in recovery nothing else writes */
static void rec_lock(void)
{
    spinlock_acquire(&card_lock); card_lock_core3 = 1; flush_finish();
    if (!fw_dirty && !psmac_recovery_on()) fw_lba = 0xffffffffu;
}
static void rec_unlock(void) { card_lock_core3 = 0; spinlock_release(&card_lock); }

/* n sectors in one command (CMD25, then CMD12 and the card's programming waited for): an upload's
   cluster. Written after the Linux driver's sequence, as sd_read_multi was; tried under QEMU only, so
   the first failure switches to sd_write per sector for the rest of the start, and says so. */
static int sd_write_multi(uint32_t lba, const uint8_t *buf, uint32_t n)
{
    if (sd.state != 1 || sd.ro) return -1;
    w(SDHBCT, 512); w(SDHBLC, n);
    if (sd_cmd(25, sd.ccs ? lba : lba * 512u, CMD_WRITE, 0)) return -2;
    const uint32_t *p = (const uint32_t *)buf;
    int err = 0;
    for (uint32_t i = 0; i < n * 128u && !err; i++) {
        uint64_t t = cnt();
        while (EDM_FILL(r(SDEDM)) >= 15) if (cnt() - t > frq() / 10) { err = -3; break; }
        if (!err) *(volatile uint32_t *)(uintptr_t)(SDH + SDDATA) = p[i];
    }
    if (!err && fsm_wait(500000)) err = -4;                           /* the last block's CRC answered */
    if (sd_cmd(12, 0, CMD_BUSY, 0) && !err) err = -5;
    uint32_t t0 = ms();
    while (!err) { int s = sd_status(); if (s == 4) break; if (s < 0 || ms() - t0 > 2000) err = -6; }
    if (!err && (r(SDHSTS) & HSTS_ERRORS)) err = -7;
    if (err) { uint32_t h = r(SDHSTS); sd.last_hsts = h; w(SDHSTS, h & HSTS_CLEAR); sd.errors++; return err; }
    sd.writes += n;
    return 0;
}
static int sd_write_run(uint32_t lba, const uint8_t *buf, uint32_t n)
{
    if (n > 1 && !multi_bad) {
        int e = sd_write_multi(lba, buf, n);
        if (!e) return 0;
        multi_bad = 1;
        kprintf("[CARD] a multi-sector write failed (%d, status %x): one sector at a time from now on\n", e, sd.last_hsts);
    }
    for (uint32_t i = 0; i < n; i++) if (sd_write(lba + i, buf + 512u * i)) return -1;
    return 0;
}

/* the FAT through one sector of write-back: every copy of the FAT written when it is flushed */
static int fw_flush(void)
{
    if (!fw_dirty) return 0;
    for (uint32_t i = 0; i < fs.nfats; i++) if (sd_write(fw_lba + i * fs.fatsz, fwbuf)) { rerr("a FAT write failed"); return -1; }
    fw_dirty = 0; fsec_lba = 0xffffffffu;
    return 0;
}
static int fw_load(uint32_t lba)
{
    if (fw_lba == lba) return 0;
    if (fw_flush()) return -1;
    if (sd_read(lba, fwbuf)) { fw_lba = 0xffffffffu; rerr("a FAT read failed"); return -1; }
    fw_lba = lba;
    return 0;
}
static uint32_t fw_get(uint32_t c)
{
    uint32_t off = c * (fs.fat32 ? 4u : 2u);
    if (fw_load(fs.fat_lba + off / 512u)) return 0x0fffffffu;
    return fs.fat32 ? le32(fwbuf + off % 512u) & 0x0fffffffu : le16(fwbuf + off % 512u);
}
static int fw_set(uint32_t c, uint32_t v)
{
    uint32_t off = c * (fs.fat32 ? 4u : 2u);
    if (fw_load(fs.fat_lba + off / 512u)) return -1;
    if (fs.fat32) put32(fwbuf + off % 512u, (le32(fwbuf + off % 512u) & 0xf0000000u) | (v & 0x0fffffffu));
    else put16(fwbuf + off % 512u, v);
    fw_dirty = 1;
    return 0;
}
static int rec_eoc(uint32_t v) { return v < 2 || (fs.fat32 ? v >= 0x0ffffff7u : v >= 0xfff7u); }
static uint32_t eoc_mark(void) { return fs.fat32 ? 0x0fffffffu : 0xffffu; }
/* FAT32's FSInfo: the free count and the next-free hint, kept roughly right (both are only hints) */
static void fsinfo_adjust(int32_t delta, uint32_t hint)
{
    if (!fs.fsinfo || sd_read(fs.fsinfo, dsec) || le32(dsec) != 0x41615252u || le32(dsec + 484) != 0x61417272u) return;
    uint32_t fc = le32(dsec + 488);
    if (fc != 0xffffffffu) put32(dsec + 488, (uint32_t)((int32_t)fc + delta));
    if (hint >= 2) put32(dsec + 492, hint);
    sd_write(fs.fsinfo, dsec);
}
/* a free cluster at or after the hint, taken (end of chain); 0 when the card is full */
static uint32_t rec_alloc(void)
{
    if (alloc_hint < 2) {
        alloc_hint = 2;
        if (fs.fsinfo && !sd_read(fs.fsinfo, dsec) && le32(dsec) == 0x41615252u) { uint32_t h = le32(dsec + 492); if (h >= 2 && h < fs.clusters + 2) alloc_hint = h; }
    }
    for (uint32_t n = 0; n < fs.clusters; n++) {
        uint32_t c = 2 + (alloc_hint - 2 + n) % fs.clusters;
        uint32_t v = fw_get(c);
        if (fw_lba == 0xffffffffu) return 0;
        if (v == 0) { if (fw_set(c, eoc_mark())) return 0; alloc_hint = c + 1; return c; }
    }
    rerr("the card is full");
    return 0;
}

/* ---- the root, entry by entry ---- */
typedef int (*rec_cb)(void *ctx, struct rec_ent *e);
static const uint8_t lfn_at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
/* "KERNEL8.IMG" (case 0), or with the lower-case bits Windows sets (kernel8.img) */
static void short_shown(const uint8_t *e, char *out, int with_case)
{
    unsigned n = 0;
    for (int k = 0; k < 8 && e[k] != ' '; k++) { char c = (char)(k == 0 && e[0] == 0x05 ? 0xe5 : e[k]); out[n++] = with_case && (e[12] & 0x08) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
    if (e[8] != ' ') { out[n++] = '.'; for (int k = 8; k < 11 && e[k] != ' '; k++) { char c = (char)e[k]; out[n++] = with_case && (e[12] & 0x10) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; } }
    out[n] = 0;
}
static int rec_walk(rec_cb cb, void *ctx)
{
    static struct rec_ent e;
    static char lfn[264];
    int lfn_ok = 0; unsigned lfn_n = 0, lfn_expect = 0; uint8_t lfn_sum = 0;
    uint32_t c = fs.fat32 ? fs.root_clus : 0, si = 0;
    for (uint32_t guard = 0; guard < 1u << 20; guard++) {
        uint32_t lba;
        if (!fs.fat32) { if (si >= fs.rootsecs) return 0; lba = fs.root_lba + si++; }
        else {
            if (si == fs.spc) { c = fw_get(c); si = 0; }
            if (rec_eoc(c)) return 0;
            lba = clus_lba(c) + si++;
        }
        if (sd_read(lba, rsec)) { rerr("the root directory could not be read"); return -1; }
        for (int i = 0; i < 16; i++) {
            const uint8_t *d = rsec + 32 * i;
            if (d[0] == 0) return 0;
            if (d[0] == 0xe5) { lfn_ok = 0; continue; }
            if (d[11] == 0x0f) {
                unsigned seq = d[0] & 0x1f;
                if (d[0] & 0x40) { lfn_ok = seq >= 1 && seq <= REC_LFN_MAX; lfn_expect = seq; lfn_n = 0; lfn_sum = d[13]; for (unsigned k = 0; k < sizeof lfn; k++) lfn[k] = 0; }
                else if (!lfn_ok || d[13] != lfn_sum || seq != lfn_expect - lfn_n) lfn_ok = 0;
                if (lfn_ok && seq >= 1) {
                    for (unsigned k = 0; k < 13; k++) {
                        unsigned ch = le16(d + lfn_at[k]), at = (seq - 1) * 13 + k;
                        if (at < sizeof lfn - 1) lfn[at] = (char)(ch == 0xffff || ch == 0 ? 0 : ch < 128 ? ch : '?');
                    }
                    e.lfn_lba[lfn_n] = lba; e.lfn_off[lfn_n] = (uint16_t)(32 * i); lfn_n++;
                }
                continue;
            }
            if (d[11] & 0x08) { lfn_ok = 0; continue; }            /* the volume label */
            uint8_t sum = 0; for (int k = 0; k < 11; k++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + d[k]);
            int use_lfn = lfn_ok && lfn_n == lfn_expect && sum == lfn_sum && lfn[0];
            short_shown(d, e.sname, 0);
            char tmp[13];
            const char *nm = lfn;
            if (!use_lfn) { short_shown(d, tmp, 1); nm = tmp; }
            unsigned k = 0; for (; nm[k] && k < REC_NAME_MAX - 1; k++) e.name[k] = nm[k];
            e.name[k] = 0; e.cut = nm[k] != 0;
            e.size = le32(d + 28); e.clus = (fs.fat32 ? (uint32_t)le16(d + 20) << 16 : 0) | le16(d + 26);
            e.attr = d[11]; e.nlfn = (uint8_t)(use_lfn ? lfn_n : 0); e.lba = lba; e.off = 32u * (uint32_t)i;
            e.wtime = le16(d + 22); e.wdate = le16(d + 24);
            lfn_ok = 0;
            int rr = cb(ctx, &e);
            if (rr) return rr;
        }
    }
    return 0;
}
int psmac_card_rec_ready(void)
{
    if (sd.state == 0 || !__atomic_load_n(&scan_done, __ATOMIC_ACQUIRE)) return 0;
    if (sd.state != 1 || !fs.ok) return -1;
    return sd.ro ? -2 : 1;
}
const char *psmac_card_rec_what(void)
{
    static char b[64];
    if (sd.state != 1) return "no card";
    sprintf(b, "%s, FAT%d, %u KB clusters", sd.ccs ? "SDHC/SDXC" : "SDSC", fs.fat32 ? 32 : 16, fs.spc / 2);
    return b;
}
uint32_t psmac_card_rec_cluster(void) { return fs.spc * 512u; }
static int readable(void) { int k = psmac_card_rec_ready(); return k == 1 || k == -2; }
struct rec_list_ctx { int (*cb)(void *, const struct rec_ent *); void *ctx; };
static int rec_list_cb(void *vc, struct rec_ent *e) { struct rec_list_ctx *x = vc; return x->cb(x->ctx, e); }
int psmac_card_rec_list(int (*cb)(void *ctx, const struct rec_ent *e), void *ctx)
{
    if (!readable()) { rerr("no card"); return -1; }
    struct rec_list_ctx x = { cb, ctx };
    rec_lock(); int rr = rec_walk(rec_list_cb, &x); rec_unlock();
    return rr < 0 ? -1 : 0;
}
struct rec_find_ctx { const char *name; struct rec_ent *out; uint32_t not_lba, not_off, clus; };
static int rec_find_cb(void *vc, struct rec_ent *e)
{
    struct rec_find_ctx *x = vc;
    if (x->name) { if (!name_eq(e->name, x->name) && !name_eq(e->sname, x->name)) return 0; }
    else if (e->clus != x->clus || (e->lba == x->not_lba && e->off == x->not_off)) return 0;   /* another name for this chain */
    if (x->out) *x->out = *e;
    return 1;
}
static int rec_find_locked(const char *name, struct rec_ent *e)
{
    struct rec_find_ctx x = { name, e, 0, 0, 0 };
    return rec_walk(rec_find_cb, &x) == 1 ? 0 : -1;
}
int psmac_card_rec_find(const char *name, struct rec_ent *e)
{
    if (!readable()) return -1;
    rec_lock(); int rr = rec_find_locked(name, e); rec_unlock();
    return rr;
}
static int chain_shared(const struct rec_ent *e)               /* another root entry starts with e's first cluster */
{
    if (e->clus < 2) return 0;
    struct rec_find_ctx x = { 0, 0, e->lba, e->off, e->clus };
    return rec_walk(rec_find_cb, &x) == 1;
}
int psmac_card_rec_free_kb(uint32_t *kb)
{
    if (!readable()) return -1;
    uint32_t free_c = 0xffffffffu;
    rec_lock();
    if (fs.fsinfo && !sd_read(fs.fsinfo, dsec) && le32(dsec) == 0x41615252u) free_c = le32(dsec + 488);
    if (free_c == 0xffffffffu || free_c > fs.clusters) {            /* FAT16, or a count nobody keeps: counted */
        free_c = 0;
        for (uint32_t c = 2; c < fs.clusters + 2; c++) { uint32_t v = fw_get(c); if (fw_lba == 0xffffffffu) break; if (!v) free_c++; }
    }
    rec_unlock();
    *kb = free_c * (fs.spc / 2);
    return 0;
}

/* ---- reading ---- */
static uint8_t rdbuf[64 * 512] __attribute__((aligned(16)));
void psmac_card_rec_read_open(const struct rec_ent *e, struct rec_rd *rd) { rd->clus = e->clus; rd->size = e->size; rd->pos = 0; }
int32_t psmac_card_rec_read(struct rec_rd *rd, uint8_t *buf, uint32_t max)
{
    if (rd->pos >= rd->size) return 0;
    uint32_t cb = fs.spc * 512u, in = rd->pos % cb;
    if (rec_eoc(rd->clus)) { rerr("the file's chain ends before its size"); return -1; }
    uint32_t want = rd->size - rd->pos; if (want > max) want = max;
    if (want > cb - in) want = cb - in;
    uint32_t skip = in % 512u;
    if (skip + want > sizeof rdbuf) want = (uint32_t)sizeof rdbuf - skip;
    uint32_t s0 = in / 512u, s1 = (in + want + 511u) / 512u;
    rec_lock();
    int e = -1;
    for (int t = 0; t < 3 && e; t++) e = sd_read_multi(clus_lba(rd->clus) + s0, rdbuf, s1 - s0);
    if (!e) memcpy(buf, rdbuf + skip, want);
    if (!e && in + want == cb) rd->clus = fw_get(rd->clus);
    rec_unlock();
    if (e) { rerr("a card read failed"); return -1; }
    rd->pos += want;
    return (int32_t)want;
}
int psmac_card_rec_get(const char *name, uint8_t *buf, uint32_t max)
{
    struct rec_ent e; struct rec_rd rd;
    if (psmac_card_rec_find(name, &e)) return -1;
    if (e.size > max || (e.attr & 0x10)) return -2;
    psmac_card_rec_read_open(&e, &rd);
    uint32_t got = 0;
    while (got < e.size) { int32_t k = psmac_card_rec_read(&rd, buf + got, e.size - got); if (k <= 0) return -1; got += (uint32_t)k; }
    return (int)got;
}

/* ---- names ---- */
static int sfn_char(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '(' || c == ')' || c == '@' || c == '^' || c == '{' || c == '}' || c == '~' || c == '\''; }
static char upc(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
/* the name as an 8.3 entry, if it is one: its 11 bytes and the case bits; 0 if it needs a long name */
static int as_83(const char *n, uint8_t sn[11], uint8_t *nt)
{
    unsigned l = 0, dot = 0, dots = 0;
    while (n[l]) { if (n[l] == '.') { dot = l; dots++; } l++; }
    if (!l || dots > 1 || (dots && (dot == 0 || dot > 8 || l - dot - 1 > 3 || l - dot - 1 == 0)) || (!dots && l > 8)) return 0;
    unsigned be = dots ? dot : l;
    int lo[2] = { 0, 0 }, hi[2] = { 0, 0 };
    for (int k = 0; k < 11; k++) sn[k] = ' ';
    for (unsigned i = 0; i < l; i++) {
        if (i == be) continue;
        char c = n[i]; int part = i > be;
        if (c >= 'a' && c <= 'z') lo[part] = 1; else if (c >= 'A' && c <= 'Z') hi[part] = 1;
        if (!sfn_char(upc(c))) return 0;
        sn[part ? 8 + (i - be - 1) : i] = (uint8_t)upc(c);
    }
    if ((lo[0] && hi[0]) || (lo[1] && hi[1])) return 0;
    *nt = (uint8_t)((lo[0] ? 0x08 : 0) | (lo[1] ? 0x10 : 0));
    return 1;
}
static int name_ok(const char *n)
{
    unsigned l = 0;
    for (; n[l]; l++) {
        unsigned char c = (unsigned char)n[l];
        if (c < 32 || c > 126 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return 0;
    }
    return l > 0 && l < REC_NAME_MAX && n[0] != ' ' && n[l - 1] != ' ' && n[l - 1] != '.' && !(l <= 2 && n[0] == '.' && (l == 1 || n[1] == '.'));
}
struct sn_ctx { const uint8_t *sn; };
static int sn_cb(void *vc, struct rec_ent *e)
{
    struct sn_ctx *x = vc; char want[13];
    short_shown(x->sn, want, 0);
    return name_eq(e->sname, want);
}
static int short_taken(const uint8_t sn[11]) { uint8_t t[13]; for (int k = 0; k < 11; k++) t[k] = sn[k]; t[11] = t[12] = 0; struct sn_ctx x = { t }; return rec_walk(sn_cb, &x) == 1; }
/* a free run of k entries in one root sector, near's sector first; the FAT32 root grows by a cluster when there is none */
struct run_find { uint32_t want, near, lba, off; int found; };
static int run_in(uint32_t lba, struct run_find *x)
{
    if (sd_read(lba, dsec)) return -1;
    uint32_t run = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t c = dsec[32 * i];
        if (c == 0 || c == 0xe5) { if (++run == x->want) { x->lba = lba; x->off = 32u * (uint32_t)(i + 1 - (int)x->want); x->found = 1; return 1; } }
        else run = 0;
    }
    return 0;
}
static int root_run(struct run_find *x)
{
    x->found = 0;
    if (x->near && run_in(x->near, x) == 1) return 0;
    uint32_t c = fs.fat32 ? fs.root_clus : 0, last = c;
    if (!fs.fat32) { for (uint32_t i = 0; i < fs.rootsecs; i++) if (run_in(fs.root_lba + i, x) == 1) return 0; rerr("the root directory is full (FAT16 has a fixed number of entries)"); return -1; }
    for (uint32_t g = 0; !rec_eoc(c) && g < 65536; g++) {
        for (uint32_t i = 0; i < fs.spc; i++) if (run_in(clus_lba(c) + i, x) == 1) return 0;
        last = c; c = fw_get(c);
    }
    uint32_t n = rec_alloc();                                     /* a new cluster of empty entries on the root's chain */
    if (!n) return -1;
    for (uint32_t j = 0; j < 512; j++) dsec[j] = 0;
    for (uint32_t i = 0; i < fs.spc; i++) if (sd_write(clus_lba(n) + i, dsec)) { rerr("a write failed"); return -1; }
    if (fw_set(last, n) || fw_flush()) return -1;
    fsinfo_adjust(-1, n + 1);
    kprintf("[CARD] the root directory grew by a cluster\n");
    x->lba = clus_lba(n); x->off = 0; x->found = 1;
    return 0;
}
/* the entries of a name (its long-name pieces, then the short one) written into a free run, with the
   first cluster, size and attributes given: 0, and where the short entry went */
static int entries_write(const char *name, uint32_t clus, uint32_t size, uint8_t attr, uint32_t near, uint32_t *lba, uint32_t *off)
{
    uint8_t sn[11], nt = 0;
    int is83 = as_83(name, sn, &nt);
    if (is83 && short_taken(sn)) { rerr("that name is taken"); return -1; }
    uint32_t n = 0; while (name[n]) n++;
    uint32_t pieces = 0;
    if (!is83) {
        uint32_t dot = n; while (dot && name[dot - 1] != '.') dot--;
        for (int k = 0; k < 11; k++) sn[k] = ' ';
        int k = 0;
        for (uint32_t i = 0; i < (dot ? dot - 1 : n) && k < 6; i++) { char c = upc(name[i]); if (sfn_char(c)) sn[k++] = (uint8_t)c; }
        if (!k) sn[k++] = '_';
        for (uint32_t i = dot, j = 8; dot && i < n && j < 11; i++) { char c = upc(name[i]); if (sfn_char(c)) sn[j++] = (uint8_t)c; }
        int num = 1;
        for (; num < 10; num++) { sn[k] = '~'; sn[k + 1] = (uint8_t)('0' + num); if (!short_taken(sn)) break; }
        if (num >= 10) { rerr("too many names alike for a short name"); return -1; }
        pieces = (n + 12) / 13;
    }
    struct run_find x = { pieces + 1, near, 0, 0, 0 };
    if (root_run(&x)) return -1;
    if (sd_read(x.lba, dsec)) { rerr("a directory read failed"); return -1; }
    uint8_t sum = 0; for (int k = 0; k < 11; k++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sn[k]);
    for (uint32_t p = 0; p < pieces; p++) {
        uint32_t seq = pieces - p;
        uint8_t *e = dsec + x.off + 32 * p;
        for (int i = 0; i < 32; i++) e[i] = 0;
        e[0] = (uint8_t)(seq | (p == 0 ? 0x40 : 0)); e[11] = 0x0f; e[13] = sum;
        for (int i = 0; i < 13; i++) { uint32_t ci = (seq - 1) * 13 + (uint32_t)i, ch = ci < n ? (uint8_t)name[ci] : ci == n ? 0 : 0xffff; put16(e + lfn_at[i], ch); }
    }
    uint8_t *e = dsec + x.off + 32 * pieces;
    for (int i = 0; i < 32; i++) e[i] = 0;
    for (int i = 0; i < 11; i++) e[i] = sn[i];
    e[11] = attr; e[12] = nt;
    uint32_t date = (uint32_t)((2026 - 1980) << 9 | 11 << 5 | 22);
    put16(e + 16, date); put16(e + 18, date); put16(e + 24, date);
    put16(e + 20, clus >> 16); put16(e + 26, clus & 0xffff); put32(e + 28, size);
    if (sd_write(x.lba, dsec)) { rerr("a directory write failed"); return -1; }
    *lba = x.lba; *off = x.off + 32 * pieces;
    return 0;
}
/* a name's entries marked deleted, sector by sector (its long-name pieces may be in the sector before) */
static int entries_remove(const struct rec_ent *e)
{
    uint32_t lbas[REC_LFN_MAX + 1], offs[REC_LFN_MAX + 1]; unsigned n = 0;
    for (unsigned i = 0; i < e->nlfn; i++) { lbas[n] = e->lfn_lba[i]; offs[n] = e->lfn_off[i]; n++; }
    lbas[n] = e->lba; offs[n] = e->off; n++;
    for (unsigned i = 0; i < n; i++) {
        if (!lbas[i]) continue;
        uint32_t l = lbas[i];
        if (sd_read(l, dsec)) { rerr("a directory read failed"); return -1; }
        for (unsigned j = i; j < n; j++) if (lbas[j] == l) { dsec[offs[j]] = 0xe5; lbas[j] = 0; }
        if (sd_write(l, dsec)) { rerr("a directory write failed"); return -1; }
    }
    return 0;
}
/* the short entry at lba/off given a first cluster and a size, and (a rename in place) new name bytes */
static int entry_point(uint32_t lba, uint32_t off, uint32_t clus, uint32_t size, const uint8_t *sn, uint8_t nt)
{
    if (sd_read(lba, dsec)) { rerr("a directory read failed"); return -1; }
    uint8_t *e = dsec + off;
    put16(e + 20, clus >> 16); put16(e + 26, clus & 0xffff); put32(e + 28, size);
    if (sn) { for (int i = 0; i < 11; i++) e[i] = sn[i]; e[12] = nt; }
    if (sd_write(lba, dsec)) { rerr("a directory write failed"); return -1; }
    return 0;
}
static uint32_t chain_free(uint32_t c)
{
    uint32_t n = 0;
    while (!rec_eoc(c) && n < fs.clusters) { uint32_t nx = fw_get(c); if (fw_lba == 0xffffffffu) break; fw_set(c, 0); n++; c = nx; }
    fw_flush();
    if (n) fsinfo_adjust((int32_t)n, 0);
    return n;
}
static int is_kernel(const char *n) { return name_eq(n, "kernel8.img"); }

/* ---- writing ---- */
int psmac_card_rec_create(const char *name, const char *near, struct rec_wr *wr)
{
    wr->open = 0;
    if (psmac_card_rec_ready() != 1) { rerr(sd.ro ? "the card is read only (mac.card=ro)" : "no card"); return -1; }
    if (!name_ok(name)) { rerr("not a name for a file in the card's root"); return -1; }
    struct rec_ent ne, old;
    rec_lock();
    int rr = -1;
    uint32_t nl = 0;
    if (!rec_find_locked(name, &old)) rerr("a file of that name is there already");
    else {
        if (near && !rec_find_locked(near, &ne)) nl = ne.lba;
        if (!entries_write(name, 0, 0, 0x20, nl, &wr->lba, &wr->off)) rr = 0;
    }
    rec_unlock();
    if (rr) return -1;
    wr->buf = tlsf_malloc(tlsf, fs.spc * 512u);
    if (!wr->buf) { struct rec_ent e; if (!psmac_card_rec_find(name, &e)) { rec_lock(); entries_remove(&e); rec_unlock(); } rerr("no memory for a cluster"); return -1; }
    wr->first = wr->last = 0; wr->size = 0; wr->nclus = 0; wr->fill = 0; wr->open = 1;
    return 0;
}
static int wr_cluster(struct rec_wr *wr)
{
    uint32_t c = rec_alloc();
    if (!c) return -1;
    if (wr->last && fw_set(wr->last, c)) return -1;                   /* the new one is the end; then joined on */
    if (!wr->first) wr->first = c;
    wr->last = c; wr->nclus++;
    uint32_t secs = (wr->fill + 511u) / 512u;
    for (uint32_t j = wr->fill; j < secs * 512u; j++) wr->buf[j] = 0;
    if (sd_write_run(clus_lba(c), wr->buf, secs)) { rerr("a write to the card failed"); return -1; }
    wr->fill = 0;
    return 0;
}
int psmac_card_rec_write(struct rec_wr *wr, const uint8_t *d, uint32_t n)
{
    if (!wr->open) return -1;
    uint32_t cb = fs.spc * 512u;
    int rr = 0;
    while (n && !rr) {
        uint32_t k = cb - wr->fill; if (k > n) k = n;
        memcpy(wr->buf + wr->fill, d, k);
        wr->fill += k; wr->size += k; d += k; n -= k;
        if (wr->fill == cb) { rec_lock(); rr = wr_cluster(wr); rec_unlock(); }
    }
    return rr;
}
int psmac_card_rec_close(struct rec_wr *wr)
{
    if (!wr->open) return -1;
    rec_lock();
    int rr = 0;
    if (wr->fill) rr = wr_cluster(wr);
    if (!rr) rr = fw_flush();
    if (!rr) rr = entry_point(wr->lba, wr->off, wr->first, wr->size, 0, 0);
    if (!rr && wr->nclus) fsinfo_adjust(-(int32_t)wr->nclus, alloc_hint);
    rec_unlock();
    tlsf_free(tlsf, wr->buf); wr->buf = 0; wr->open = 0;
    return rr;
}
void psmac_card_rec_abandon(struct rec_wr *wr)
{
    if (!wr->open) return;
    rec_lock();
    fw_flush();
    if (wr->first) chain_free(wr->first);
    if (!sd_read(wr->lba, dsec)) {                                  /* its entries: the short one and the pieces before it */
        uint32_t off = wr->off;
        dsec[off] = 0xe5;
        while (off >= 32 && dsec[off - 32 + 11] == 0x0f && dsec[off - 32] != 0xe5) { off -= 32; int last = dsec[off] & 0x40; dsec[off] = 0xe5; if (last) break; }
        sd_write(wr->lba, dsec);
    }
    rec_unlock();
    tlsf_free(tlsf, wr->buf); wr->buf = 0; wr->open = 0;
}

/* ---- delete, rename, replace, swap ---- */
static int rec_delete_locked(const char *name, int refuse_kernel)
{
    struct rec_ent e;
    if (rec_find_locked(name, &e)) { rerr("no such file"); return -1; }
    if (e.attr & 0x10) { rerr("a folder: only files are deleted here"); return -1; }
    if (refuse_kernel && is_kernel(e.name)) { rerr("kernel8.img is never deleted: the Pi would not start"); return -1; }
    int shared = chain_shared(&e);
    if (entries_remove(&e)) return -1;
    if (!shared && e.clus >= 2) chain_free(e.clus);
    else if (shared) kprintf("[CARD] %s: its clusters are another name's too, so they stay\n", e.name);
    return 0;
}
int psmac_card_rec_delete(const char *name)
{
    if (psmac_card_rec_ready() != 1) { rerr("no writable card"); return -1; }
    rec_lock(); int rr = rec_delete_locked(name, 1); rec_unlock();
    return rr;
}
static int rename_locked(const char *from, const char *to)
{
    struct rec_ent e, t;
    int rr = -1;
    uint8_t sn[11], nt;
    if (rec_find_locked(from, &e)) rerr("no such file");
    else if (!rec_find_locked(to, &t) && !(t.lba == e.lba && t.off == e.off)) rerr("a file of that name is there already");
    else if (!e.nlfn && as_83(to, sn, &nt) && (!short_taken(sn) || name_eq(e.sname, to))) rr = entry_point(e.lba, e.off, e.clus, e.size, sn, nt);   /* in place: one write */
    else {
        uint32_t l, o;
        if (!entries_write(to, e.clus, e.size, e.attr, e.lba, &l, &o)) rr = entries_remove(&e);
    }
    return rr;
}
int psmac_card_rec_rename(const char *from, const char *to)
{
    if (psmac_card_rec_ready() != 1) { rerr("no writable card"); return -1; }
    if (!name_ok(to)) { rerr("not a name for a file in the card's root"); return -1; }
    if (is_kernel(from) || is_kernel(to)) { rerr("kernel8.img is not renamed here: upload a kernel (it is checked) or roll back"); return -1; }
    rec_lock();
    int rr = rename_locked(from, to);
    rec_unlock();
    return rr;
}
/* the two files' first clusters and sizes change places: one sector write when their short entries share
   a sector, else two, a's first (between the two both start with b's chain, which repair sorts out) */
static int swap_locked(struct rec_ent *a, struct rec_ent *b)
{
    if (a->lba == b->lba) {
        if (sd_read(a->lba, dsec)) { rerr("a directory read failed"); return -1; }
        uint8_t *x = dsec + a->off, *y = dsec + b->off;
        put16(x + 20, b->clus >> 16); put16(x + 26, b->clus & 0xffff); put32(x + 28, b->size);
        put16(y + 20, a->clus >> 16); put16(y + 26, a->clus & 0xffff); put32(y + 28, a->size);
        if (sd_write(a->lba, dsec)) { rerr("a directory write failed"); return -1; }
        return 0;
    }
    if (entry_point(a->lba, a->off, b->clus, b->size, 0, 0)) return -1;
    return entry_point(b->lba, b->off, a->clus, a->size, 0, 0);
}
int psmac_card_rec_swap(const char *an, const char *bn)
{
    if (psmac_card_rec_ready() != 1) { rerr("no writable card"); return -1; }
    struct rec_ent a, b;
    rec_lock();
    int rr = -1;
    if (rec_find_locked(an, &a) || rec_find_locked(bn, &b)) rerr("no such file");
    else rr = swap_locked(&a, &b);
    rec_unlock();
    return rr;
}
/* target's content becomes temp's: an existing target's short entry is pointed at temp's chain (its name
   never goes away), then temp's entries go and the old chain is freed; a new target is temp renamed */
int psmac_card_rec_replace(const char *target, const char *temp)
{
    if (psmac_card_rec_ready() != 1) { rerr("no writable card"); return -1; }
    struct rec_ent t, u;
    rec_lock();
    int rr = -1;
    if (rec_find_locked(temp, &u)) rerr("the uploaded file has gone");
    else if (rec_find_locked(target, &t)) {
        uint8_t sn[11], nt;
        if (!u.nlfn && as_83(target, sn, &nt) && !short_taken(sn)) rr = entry_point(u.lba, u.off, u.clus, u.size, sn, nt);
        else { uint32_t l, o; if (!entries_write(target, u.clus, u.size, 0x20, u.lba, &l, &o)) rr = entries_remove(&u); }
    } else if (t.attr & 0x10) rerr("that name is a folder");
    else {
        uint32_t old = t.clus;
        rr = swap_locked(&t, &u);                                   /* target -> the new chain, temp -> the old one */
        if (!rr && !rec_find_locked(temp, &u)) { rr = entries_remove(&u); if (!rr && old >= 2) chain_free(old); }
    }
    rec_unlock();
    return rr;
}
int psmac_card_rec_put(const char *name, const uint8_t *d, uint32_t n)
{
    struct rec_wr wr;
    static const char tmp[] = "aurora68.tmp";
    struct rec_ent e;
    if (!psmac_card_rec_find(tmp, &e)) psmac_card_rec_delete(tmp);
    if (psmac_card_rec_create(tmp, name, &wr)) return -1;
    if (psmac_card_rec_write(&wr, d, n) || psmac_card_rec_close(&wr)) { psmac_card_rec_abandon(&wr); return -1; }
    return psmac_card_rec_replace(name, tmp);
}
/* What an interrupted upload or swap leaves, put right at a recovery start: a temporary name that starts
   with another name's first cluster is a swap cut between its two writes (its entries go, the clusters are
   the other name's); a temporary on its own is an upload that did not finish (deleted, clusters freed).
   kernel8.old sharing kernel8.img's chain is a rollback cut the same way. */
void psmac_card_rec_repair(void)
{
    static const char *const temps[] = { "kernel8.new", "aurora68.up", "aurora68.tmp", "kernel8.old" };
    if (psmac_card_rec_ready() != 1) return;
    rec_lock();
    for (unsigned i = 0; i < sizeof temps / sizeof temps[0]; i++) {
        struct rec_ent e;
        if (rec_find_locked(temps[i], &e)) continue;
        if (chain_shared(&e)) { entries_remove(&e); kprintf("[RECOVERY] %s shared its first cluster with another file (a swap cut short): its name taken away, the data kept\n", e.name); }
        else if (i < 3) { rec_delete_locked(temps[i], 0); kprintf("[RECOVERY] %s, left by an upload that did not finish, deleted\n", e.name); }
    }
    rec_unlock();
}

/* ---- the boot counter and the request file ---------------------------------------------------
   boot.txt in the card's root holds the number of starts in a row that have not reached the Finder
   (psmac_recovery.c decides on it). It is made once, with one cluster and a text of fixed length, and from
   then on only its first data sector is written: no directory sector and no FAT, so a power cut in the
   middle of the write can damage at most the number itself, which then reads as 0. recovery.txt is the
   request "start in recovery next time": made by the kernel when a running system asks (its one line says
   who asked), or put there by hand; taken away when recovery starts. */
#define BOOT_TEXT_LEN 160u
static struct rec_ent boot_ent; static int boot_have;
static volatile int boot_reset_pending;
static void boot_text(int n, uint8_t *out)
{
    static const char head[] = "Aurora68 boot counter: ";
    static const char tail[] = "\nStarts in a row that did not reach the Finder; Aurora68 Setup and Recovery starts at mac.bootcount (3). 00 once the Finder runs.\n";
    unsigned o = 0;
    for (unsigned i = 0; head[i]; i++) out[o++] = (uint8_t)head[i];
    if (n < 0) n = 0;
    if (n > 99) n = 99;
    out[o++] = (uint8_t)('0' + n / 10); out[o++] = (uint8_t)('0' + n % 10);
    for (unsigned i = 0; tail[i] && o < BOOT_TEXT_LEN; i++) out[o++] = (uint8_t)tail[i];
    while (o < BOOT_TEXT_LEN) out[o++] = ' ';
    out[BOOT_TEXT_LEN - 1] = '\n';
}
static int boot_card_ok(void) { return sd.state == 1 && fs.ok && !sd.ro; }
int psmac_card_boot_read(int *count, char *flag, uint32_t flagmax, int *flag_len)
{
    *count = -1; *flag_len = -1;
    if (sd.state != 1 || !fs.ok) return -1;
    rec_lock();
    boot_have = !rec_find_locked("boot.txt", &boot_ent) && boot_ent.clus >= 2 && boot_ent.size;
    if (boot_have && !sd_read(clus_lba(boot_ent.clus), dsec)) {
        /* the number after the first ':' ("Aurora68 boot counter: 02"), or a file that is just a number */
        int n = -1;
        unsigned i = 0, lim = boot_ent.size < 511 ? boot_ent.size : 511;
        while (i < lim && dsec[i] != ':') i++;
        if (i >= lim) i = 0;
        while (i < lim && !(dsec[i] >= '0' && dsec[i] <= '9')) i++;
        if (i < lim) { n = 0; while (i < lim && dsec[i] >= '0' && dsec[i] <= '9' && n < 1000) n = n * 10 + (dsec[i++] - '0'); }
        *count = n < 0 ? 0 : n;
    }
    struct rec_ent f;
    if (!rec_find_locked("recovery.txt", &f)) {
        uint32_t n = 0;
        if (f.clus >= 2 && f.size && !sd_read(clus_lba(f.clus), dsec))
            for (; n < f.size && n < 512 && n + 1 < flagmax && dsec[n] != '\n' && dsec[n] != '\r'; n++) flag[n] = (char)(dsec[n] >= 32 && dsec[n] < 127 ? dsec[n] : '?');
        if (flagmax) flag[n] = 0;
        *flag_len = (int)n;
    }
    rec_unlock();
    return 0;
}
/* The one-shot debug start (23 November 2026, docs/08 item 122): core 0, before the boot arguments are read, with
   the card mounted by its first look (psmac_card_early). debug-once.txt's one line into buf (at most max - 1 bytes):
   its length, or -1 when there is none (or no card). With take, the file is renamed debug-once.old at once (the old
   .old goes first), so that it happens once: a start that then hangs is followed by a normal one, counted by the
   boot counter as before. A card that cannot be written (mac.card=ro) gives -2: the start would repeat for ever. */
int psmac_card_once_take(char *buf, uint32_t max, int take)
{
    if (sd.state != 1 || !fs.ok || !max) return -1;
    struct rec_ent f, o;
    int n = -1;
    rec_lock();
    if (!rec_find_locked("debug-once.txt", &f) && !(f.attr & 0x10)) {
        n = 0;
        if (f.clus >= 2 && f.size && !sd_read(clus_lba(f.clus), dsec))
            for (; (uint32_t)n < f.size && n < 512 && (uint32_t)n + 1 < max; n++) buf[n] = (char)(dsec[n] >= 32 && dsec[n] < 127 ? dsec[n] : dsec[n] == '\n' || dsec[n] == '\r' ? '\n' : '?');
        buf[n] = 0;
        if (take && sd.ro) n = -2;
        else if (take) {
            if (!rec_find_locked("debug-once.old", &o)) rec_delete_locked("debug-once.old", 1);
            if (rename_locked("debug-once.txt", "debug-once.old")) { kprintf("[CARD] debug-once.txt could not be renamed debug-once.old (%s): not used, so that it does not happen at every start\n", rec_err); n = -2; }
        }
    }
    rec_unlock();
    return n;
}
/* the start was a recovery start after all (the programmer's switch, phase 2): debug-once.old back as debug-once.txt,
   for the next normal start. Core 3 */
int psmac_card_once_rearm(void)
{
    if (psmac_card_rec_ready() != 1) return -1;
    struct rec_ent f;
    rec_lock();
    int rr = -1;
    if (rec_find_locked("debug-once.txt", &f)) rr = rename_locked("debug-once.old", "debug-once.txt");
    rec_unlock();
    return rr;
}
static void boot_write_locked(int count)
{
    uint8_t t[BOOT_TEXT_LEN];
    boot_text(count, t);
    if (!boot_have || boot_ent.size != BOOT_TEXT_LEN) {            /* made, or remade at its fixed length */
        if (boot_have) rec_delete_locked("boot.txt", 0);
        uint32_t c = rec_alloc(), l, o;
        if (!c || fw_flush()) return;
        for (uint32_t j = 0; j < 512; j++) dsec[j] = j < BOOT_TEXT_LEN ? t[j] : 0;
        if (sd_write(clus_lba(c), dsec)) return;
        if (entries_write("boot.txt", c, BOOT_TEXT_LEN, 0x20, 0, &l, &o)) return;
        fsinfo_adjust(-1, alloc_hint);
        boot_have = !rec_find_locked("boot.txt", &boot_ent);
        return;
    }
    for (uint32_t j = 0; j < 512; j++) dsec[j] = j < BOOT_TEXT_LEN ? t[j] : 0;
    sd_write(clus_lba(boot_ent.clus), dsec);
}
void psmac_card_boot_write(int count)
{
    if (!boot_card_ok()) return;
    rec_lock(); boot_write_locked(count); rec_unlock();
}
void psmac_card_boot_flag_drop(void)
{
    if (!boot_card_ok()) return;
    rec_lock(); rec_delete_locked("recovery.txt", 0); rec_unlock();
}
void psmac_card_boot_reset_later(void) { __atomic_store_n(&boot_reset_pending, 1, __ATOMIC_RELEASE); }
static char ask_text[160]; static volatile int ask_state;          /* 0 none, 1 pending, 2 written, 3 failed */
int psmac_card_ask_recovery(const char *why)
{
    if (!boot_card_ok()) return -1;
    unsigned i = 0; for (; why[i] && i < sizeof ask_text - 2; i++) ask_text[i] = why[i];
    ask_text[i++] = '\n'; ask_text[i] = 0;
    __atomic_store_n(&ask_state, 1, __ATOMIC_RELEASE);
    return 0;
}
int psmac_card_ask_state(void) { return ask_state; }
/* core 3's card turn, in either mode: the jobs above (1 when one was done) */
static int boot_jobs(void)
{
    if (__atomic_load_n(&boot_reset_pending, __ATOMIC_ACQUIRE) && boot_card_ok()) {
        boot_reset_pending = 0;
        if (!boot_have) boot_have = !rec_find_locked("boot.txt", &boot_ent) && boot_ent.clus >= 2 && boot_ent.size;
        boot_write_locked(0);
        fw_flush();
        kprintf("[CARD] boot.txt: 00 (the Finder was reached, or recovery started)\n");
        return 1;
    }
    if (__atomic_load_n(&ask_state, __ATOMIC_ACQUIRE) == 1) {
        int ok = 0;
        if (boot_card_ok()) {
            uint32_t n = 0; while (ask_text[n]) n++;
            struct rec_ent f;
            if (!rec_find_locked("recovery.txt", &f)) rec_delete_locked("recovery.txt", 0);
            uint32_t c = rec_alloc(), l, o;
            if (c && !fw_flush()) {
                for (uint32_t j = 0; j < 512; j++) dsec[j] = j < n ? (uint8_t)ask_text[j] : 0;
                if (!sd_write(clus_lba(c), dsec) && !entries_write("recovery.txt", c, n, 0x20, 0, &l, &o)) { ok = 1; fsinfo_adjust(-1, alloc_hint); }
            }
        }
        kprintf("[CARD] recovery.txt %s\n", ok ? "written: the next start is Aurora68 Setup and Recovery" : "could not be written");
        __atomic_store_n(&ask_state, ok ? 2 : 3, __ATOMIC_RELEASE);
        return 1;
    }
    return 0;
}

#ifdef PSMAC_PC
/* ==== the PC card's files (psmac_pc.c, docs/19-pc-card.md; a kernel built with -DPSMAC_PC=ON) ======================
   23 November 2026. The PC's firmware, its VGA BIOS and its disk image are files in the card's root, which the PC's
   core (core 2 on the solo board, core 1 elsewhere) reads while core 3 goes on with its own work: a file's clusters
   are listed once (the FAT read under the card's lock, as the recovery functions above do), then any part of it is
   read with the cluster's sectors in one command, also under the lock, a cluster at most at a time. Written back
   the same way since the second round (psmac_card_pc_write, below). */
#define PCF_MAX 36                 /* the firmware, the VGA BIOS and the 32 images of the PC's list */
static struct { uint32_t size, n, *clus; } pcf[PCF_MAX];
static int npcf;
/* the file's handle, or why not (psmac_pc.c says it in words, in the log and in Aurora68 PC's window since 23 November
   2026, when the owner's pc.disk0= was not attached and nothing said why): -1 no file of that name (long or 8.3, any
   case) in the root, -2 the card not looked at yet (try again), -3 a folder, -4 empty, -5 its clusters end before its
   size, -6 no memory for its cluster list, -7 too many files open, -8 no card, or one that cannot be read */
int psmac_card_pc_open(const char *name, uint32_t *size)
{
    struct rec_ent e;
    int k = psmac_card_rec_ready();
    if (k == 0) return -2;
    if (k != 1 && k != -2) return -8;
    if (npcf >= PCF_MAX) return -7;
    if (psmac_card_rec_find(name, &e)) return -1;
    if (e.attr & 0x10) return -3;
    if (e.clus < 2 || !e.size) return -4;
    uint32_t cb = fs.spc * 512u, n = (e.size + cb - 1) / cb, i = 0, c = e.clus;
    uint32_t *list = tlsf_malloc(tlsf, n * 4u);
    if (!list) return -6;
    rec_lock();
    for (; i < n && !rec_eoc(c); i++) { list[i] = c; c = fw_get(c); }
    rec_unlock();
    if (i < n) { tlsf_free(tlsf, list); kprintf("[CARD] %s: its cluster chain ends after %u of its %u clusters\n", name, i, n); return -5; }
    pcf[npcf].size = e.size; pcf[npcf].n = n; pcf[npcf].clus = list;
    *size = e.size;
    return npcf++;
}
int psmac_card_pc_read(int file, uint32_t off, void *buf, uint32_t n)
{
    if (file < 0 || file >= npcf || off > pcf[file].size || n > pcf[file].size - off) return -1;
    uint8_t *b = buf;
    uint32_t cb = fs.spc * 512u;
    while (n) {
        uint32_t ci = off / cb, in = off % cb, s0 = in / 512u, skip = in % 512u;
        uint32_t k = cb - in < n ? cb - in : n;
        if (skip + k > sizeof rdbuf) k = (uint32_t)sizeof rdbuf - skip;
        uint32_t s1 = (in + k + 511u) / 512u;
        rec_lock();
        int e = -1;
        for (int t = 0; t < 3 && e; t++) e = sd_read_multi(clus_lba(pcf[file].clus[ci]) + s0, rdbuf, s1 - s0);
        if (!e) memcpy(b, rdbuf + skip, k);
        rec_unlock();
        if (e) return -1;
        b += k; off += k; n -= k;
    }
    return 0;
}
/* Writing back (23 November 2026, docs/19-pc-card.md "Disks"): whole sectors at a sector's offset, inside the file's own
   clusters (listed at its open), as PiDisk's images are written: the file never changes size and no directory entry or
   FAT sector is touched, so the card's file system is the same before and after, whatever happens in between. A cluster's
   sectors at most per command (sd_write_run: CMD25, or one at a time once that has failed), under the card's lock, as the
   reads above are. 0, -1 for a bad request or a write that failed, -2 when the card is not writable (mac.card=ro, none) */
int psmac_card_pc_write(int file, uint32_t off, const void *buf, uint32_t n)
{
    if (file < 0 || file >= npcf || ((off | n) & 511u) || off > pcf[file].size || n > pcf[file].size - off) return -1;
    if (psmac_card_rec_ready() != 1) return -2;
    const uint8_t *b = buf;
    uint32_t cb = fs.spc * 512u;
    while (n) {
        uint32_t ci = off / cb, in = off % cb;
        uint32_t k = cb - in < n ? cb - in : n;
        if (k > sizeof rdbuf) k = (uint32_t)sizeof rdbuf;
        rec_lock();
        memcpy(rdbuf, b, k);                                       /* the controller's FIFO takes aligned words */
        int e = sd_write_run(clus_lba(pcf[file].clus[ci]) + in / 512u, rdbuf, k / 512u);
        rec_unlock();
        if (e) return -1;
        b += k; off += k; n -= k;
    }
    return 0;
}
int psmac_card_pc_writable(void) { return psmac_card_rec_ready() == 1; }
/* the root's files (not folders) for the PC's image list, from the PC's core: -1 when the card cannot be read now */
struct pc_list_ctx { void (*cb)(void *, const char *, uint32_t); void *ctx; };
static int pc_list_cb(void *vc, const struct rec_ent *e)
{
    const struct pc_list_ctx *x = vc;
    if (!(e->attr & 0x18) && !e->cut && e->clus >= 2) x->cb(x->ctx, e->name, e->size);
    return 0;
}
int psmac_card_pc_list(void (*cb)(void *ctx, const char *name, uint32_t size), void *ctx)
{
    struct pc_list_ctx x = { cb, ctx };
    return psmac_card_rec_list(pc_list_cb, &x);
}
#endif
