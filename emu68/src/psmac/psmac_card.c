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

extern uint32_t get_clock_rate(uint32_t clock_id);
extern const char *psmac_option(const char *key);
extern int alice_pi_is_2711(void);
extern spinlock_t psmac_gpfsel_lock;

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
static void sd_clock(uint32_t hz)
{
    uint32_t core = get_clock_rate(4);
    if (core < 100000000u) core = 250000000u;
    uint32_t div = core / hz; div = div > 2 ? div - 2 : 0;
    if (core / (div + 2) > hz) div++;
    if (div > 0x7ff) div = 0x7ff;
    w(SDCDIV, div);
}
static int sd_init(void)
{
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
static int is_image_name(const char *s, uint32_t size)
{
    if (size < (1u << 20) || (s[0] == '.' && s[1] == '_')) return 0;
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
    static const char *const names[] = { "psk", "pass", "passwd", "password", "passphrase", "passkey", "key", "secret", "token" };
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
    if (sd.state < 0 || sd.ro) return -1;
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
    if (!m->ext || !m->loaded) return 0;
    m->next = file_extents(f, m->ext, 256);
    if (m->next <= 0) { kprintf("[CARD] %s is in more than 256 pieces: copy it to a freshly formatted card\n", name); return 0; }
    for (uint32_t i = 0; i < (m->nload + 31) / 32; i++) m->loaded[i] = 0;
    m->off = 0; m->file = ram; m->file_len = f->size; m->ok = !sd.ro; m->nloaded = 0; m->load_t0 = cnt();
    __atomic_store_n(&nimg, nimg + 1, __ATOMIC_RELEASE);
    kprintf("[CARD] disk image %s, %u MB in %d piece%s: read in the background%s\n", name, f->size >> 20, m->next, m->next == 1 ? "" : "s", m->ok ? ", changes written back to it" : "");
    psmac_boot_mark("card: disk image %s found (%u MB), loading in the background", name, f->size >> 20);
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
static void chunk_load_direct(struct img *m, uint32_t c);
static void flush_finish(void);
static int chunk_in(const struct img *m, uint32_t c) { return (__atomic_load_n(&m->loaded[c / 32], __ATOMIC_ACQUIRE) >> (c % 32)) & 1; }
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
    if (!ld.active && !load_pick()) return 0;
    struct img *m = &imgs[ld.img];
    uint32_t first = ld.chunk * (LCHUNK / 512u) + ld.done, secs = (m->len + 511u) / 512u;
    uint32_t end = (ld.chunk + 1) * (LCHUNK / 512u); if (end > secs) end = secs;
    uint32_t run = 0, lba = file_lba(m, first * 512u, &run), n = end - first;
    if (n > LSLICE) n = LSLICE;
    if (n > run) n = run;
    int e = lba && n ? -9 : -8;
    for (int tries = 0; tries < 3 && e; tries++) e = lba && n ? sd_read_multi(lba, lbuf, n) : -8;
    if (card_slow_us) udelay(card_slow_us * n);
    if (chunk_in(m, ld.chunk)) { ld.active = 0; return 1; }   /* core 0 read this chunk itself: its copy, and the Mac's writes since, stay */
    if (e) {
        /* the card failed under the image: what is missing stays zero, and nothing of this image
           is written back, so the file on the card is not made worse */
        kprintf("[CARD] %s: read error %d at sector %u (status %x): the image is incomplete, its changes stay in memory\n", m->name, e, first, sd.last_hsts);
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
/* [p, p+len) of an image is about to be read or written by the Mac: make sure it is in memory. On
   core 3 (PiDisk's block service) the chunks are read here and now; from core 0 (the solo board's
   SCSI disk) core 3 is asked and this waits, as the Mac would wait on a disk. */
static inline int on_core3(void) { uint64_t id; __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(id)); return (id & 3) == 3; }
void psmac_card_need(const void *p, uint32_t len)
{
    const uint8_t *b = p;
    int n = __atomic_load_n(&nimg, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        struct img *m = &imgs[i];
        if (!m->loaded || b < m->ram || b >= m->ram + m->len || !len) continue;
        if (m->nloaded >= m->nload) return;
        uint32_t c0 = (uint32_t)(b - m->ram) / LCHUNK, c1 = ((uint32_t)(b - m->ram) + len - 1) / LCHUNK;
        for (uint32_t c = c0; c <= c1 && c < m->nload; c++) {
            if (chunk_in(m, c)) continue;
            if (on_core3()) {
                /* already held when this comes from core 3's own card pass: a second image's
                   registration reads its partition map here (447febf took the lock again and
                   stopped core 3 for good on the user's Pi with two images on the card) */
                int mine = card_lock_core3;
                if (!mine) { spinlock_acquire(&card_lock); card_lock_core3 = 1; }
                flush_finish();                             /* PiDisk's block service comes between passes */
                if (ld.active && !(ld.img == i && ld.chunk == c)) while (ld.active) load_step();   /* the chunk in hand first */
                if (!chunk_in(m, c)) { if (!ld.active) { ld.active = 1; ld.img = i; ld.chunk = c; ld.done = 0; } while (ld.active) load_step(); }
                if (!mine) { card_lock_core3 = 0; spinlock_release(&card_lock); }
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
            if (!chunk_in(m, c)) { kprintf("[CARD] %s: the card stopped answering; block %u is not in memory\n", m->name, c * (LCHUNK / 512u)); return; }
        }
        return;
    }
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
static void chunk_load_direct(struct img *m, uint32_t c)
{
    flush_finish();
    uint32_t first = c * (LCHUNK / 512u), secs = (m->len + 511u) / 512u, end = first + LCHUNK / 512u;
    if (end > secs) end = secs;
    for (uint32_t s = first; s < end; ) {
        uint32_t run = 0, lba = file_lba(m, s * 512u, &run), n = end - s;
        if (n > run) n = run;
        int e = lba && n ? -9 : -8;
        for (int tries = 0; tries < 3 && e; tries++) e = lba && n ? sd_read_multi(lba, dbuf, n) : -8;
        if (card_slow_us) udelay(card_slow_us * n);
        if (e) { kprintf("[CARD] %s: read error %d at sector %u (status %x): the image is incomplete, its changes stay in memory\n", m->name, e, s, sd.last_hsts); m->ok = 0; if (!n) n = end - s; }
        else { uint32_t bytes = n * 512u; if (s * 512u + bytes > m->len) bytes = m->len - s * 512u; memcpy((uint8_t *)m->ram + s * 512u, dbuf, bytes); }
        s += n;
    }
    __atomic_fetch_or(&m->loaded[c / 32], 1u << (c % 32), __ATOMIC_RELEASE);
    if (__atomic_add_fetch(&m->nloaded, 1, __ATOMIC_RELAXED) == m->nload)
        kprintf("[CARD] %s: all %u MB read from the card\n", m->name, m->len >> 20);
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
static void card_poll_locked(void)
{
    if (sd.state == 0) {
        card_start();
        __atomic_store_n(&scan_done, 1, __ATOMIC_RELEASE);
        return;
    }
    if (sd.state != 1) return;
    if (fl.busy) { flush_poll(); return; }             /* a sector is being programmed: nothing else touches the card */
    if (__atomic_load_n(&pair_dirty, __ATOMIC_ACQUIRE) && !sd.ro) { pair_dirty = 0; pairings_write(); return; }
    if (__atomic_load_n(&opt_state, __ATOMIC_ACQUIRE) == 1 && !sd.ro) { cmdline_apply(); return; }
    { extern int psmac_solo_pram_take(uint8_t out[260]), psmac_se_pram_take(uint8_t out[260]); static uint8_t p[260];
      if (!sd.ro && (psmac_solo_pram_take(p) || psmac_se_pram_take(p))) { pram_write(p); return; } }
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
    if ((++turn & 1) && first_dirty_t) { flush_poll(); if (fl.busy) return; }
    if (load_step()) return;
    flush_poll();
}
/* the card's first look, on core 3's first pass: the card, its FAT, pairings.txt, the container, an image */
static void card_start(void)
{
    if (!start_at) start_at = ms() ? ms() : 1;
    { const char *o = psmac_option("mac.cardslow"); if (o) { uint32_t v = 0; while (*o >= '0' && *o <= '9') v = v * 10 + (uint32_t)(*o++ - '0'); card_slow_us = v; kprintf("[CARD] mac.cardslow=%u: %u us more per sector read (a slow card, for the testbed)\n", v, v); } }
    const char *o = psmac_option("mac.card");
    if (o && o[0] == 'o' && o[1] == 'f') { sd.state = -2; pair_state = -1; kprintf("[CARD] off (mac.card=off)\n"); return; }
    if (alice_pi_is_2711()) { sd.state = -1; pair_state = -1; kprintf("[CARD] the Pi 4's card controller is not supported yet: nothing saved\n"); return; }
    sd.ro = o && o[0] == 'r' && o[1] == 'o';
    int e = sd_init();
    if (e) { sd.state = -1; pair_state = -1; kprintf("[CARD] the SD card does not answer on the sdhost controller (%d, status %x)\n", e, sd.last_hsts); return; }
    sd.state = 1;
    e = fs_mount();
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
    container_check();
    image_scan();
    psmac_boot_mark("card: mounted, pairings read, disk images found");
    fl.img = -1;
    { static const char test[] = "# mac.cardtest\nbt.dev=001122334455:00112233445566778899aabbccddeeff:2\n";
      const char *t = psmac_option("mac.cardtest"); if (t && t[0] == '1') psmac_card_save_pairings(test, sizeof test - 1); }
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
