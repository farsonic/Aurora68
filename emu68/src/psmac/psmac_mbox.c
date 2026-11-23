/*
 * psmac_mbox.c — Pi-side mailbox services for the Macintosh (Aurora68 fork of Emu68). MPL-2.0.
 *
 * The window (see psmb.h) is Pi RAM shared with the 68k at slot $9. The service core calls
 * psmac_mbox_poll() from its main loop; each service has one command block and is executed
 * synchronously. Backends in the testbed are host files reached through QEMU semihosting
 * (block device image, network frame files); on hardware they become SD/exFAT, GENET/WiFi, etc.
 */
#include <stdint.h>
#include <stdbool.h>
#include "support.h"
#include "psmac.h"
#include "psmb.h"
#include "../alice/alice_psmac.h"
#include "psmac_usbdisk.h"

#define KERNEL_ALIAS 0xffffff9000000000ull

/* semihosting primitives exported by psmac_bus.c (testbed only) */
int64_t psmac_sh_open(const char *name, uint64_t mode);
int     psmac_sh_read(int64_t fd, void *buf, uint64_t len);
int     psmac_sh_write(int64_t fd, const void *buf, uint64_t len);
int64_t psmac_sh_seek(int64_t fd, uint64_t pos);
int64_t psmac_sh_flen(int64_t fd);
int     psmac_sh_available(void);
const char *psmac_option(const char *key);     /* value of key= in bootargs, or NULL */
uint32_t psmac_mac_ram_mb(void);

/* Emu68 is built big-endian (like the 68k): shared fields need no byte swapping */
static inline uint32_t rd32(const volatile void *p) { return *(const volatile uint32_t *)p; }
static inline uint16_t rd16(const volatile void *p) { return *(const volatile uint16_t *)p; }
static inline void wr32(volatile void *p, uint32_t v) { *(volatile uint32_t *)p = v; }
static inline void wr16(volatile void *p, uint16_t v) { *(volatile uint16_t *)p = v; }

static uint8_t *win;               /* kernel alias of the window */
static uintptr_t win_phys, ram_phys; static uint32_t ram_size;
static uint64_t t0;

static inline uint64_t cnt_now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0":"=r"(v)); return v; }
static inline uint64_t cnt_frq(void) { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0":"=r"(v)); return v; }

/* ---- 68k logical buffer access (page by page through the MMU shim) ---- */
/* a screen's frame buffer, by the 68k address of its pixels (the video decoder card draws into them,
   psmac_vframes.c): its kernel view, and how many bytes from there are the screen's */
extern int psmac_display_info(int i, uint32_t *w, uint32_t *h, uint32_t *depth, uint32_t *base);
extern int psmac_display_surface(int i, uint8_t **pix, uint32_t *w, uint32_t *h, uint32_t *rb, uint32_t *depth,
                                 const volatile uint32_t **clut, uint32_t *clut_seq, int *gray);
static uint8_t *screen_at(uint32_t ph, uint32_t *left)
{
    for (int i = 0; i < 2; i++) {
        uint32_t w, h, d, base, rb, seq; uint8_t *pix; const volatile uint32_t *clut; int gray;
        if (!psmac_display_info(i, &w, &h, &d, &base) || !psmac_display_surface(i, &pix, &w, &h, &rb, &d, &clut, &seq, &gray)) continue;
        if (ph >= base && ph - base < rb * h) { *left = rb * h - (ph - base); return pix + (ph - base); }
    }
    return NULL;
}
extern uintptr_t psmac_mmu_video_phys(uint32_t p, uint32_t *left);
static uint8_t *mac_page(uint32_t logical)
{
    int valid; uint32_t ph = psmac_mmu_l2p(logical, &valid), left;
    if (!valid) return NULL;
    if (ph < ram_size) return (uint8_t *)(KERNEL_ALIAS + ram_phys + ph);
    if (ph >= PSMB_WIN_PHYS && ph < PSMB_WIN_PHYS + PSMB_WIN_SIZE) return win + (ph - PSMB_WIN_PHYS);
    uintptr_t card = psmac_mmu_video_phys(ph, &left);   /* the video decoder card's window: its ring takes disk reads */
    if (card) return (uint8_t *)(KERNEL_ALIAS + card);
    return screen_at(ph, &left);              /* (the 24-bit alias of the window is not served) */
}
/* For the video decoder card (psmac_vframes.c, 22 November 2026): [logical, logical + len) of the 68k's
   memory as one run in the kernel's view (RAM whose pages follow one another, or a screen), else NULL; and
   the page-by-page copy for anything else. Logical addresses, as everywhere in this file. */
uint8_t *psmac_mac_span(uint32_t logical, uint32_t len)
{
    int valid; uint32_t ph = psmac_mmu_l2p(logical, &valid), left;
    if (!valid || !len) return NULL;
    if (ph < ram_size) {
        if (len > ram_size - ph) return NULL;
        for (uint32_t a = (logical & ~0xfffu) + 0x1000u; a - logical < len; a += 0x1000u) {
            int v2; uint32_t p2 = psmac_mmu_l2p(a, &v2);
            if (!v2 || p2 != ph + (a - logical)) return NULL;
        }
        return (uint8_t *)(KERNEL_ALIAS + ram_phys + ph);
    }
    uint8_t *s = screen_at(ph, &left);
    return s && len <= left ? s : NULL;
}
int psmac_mac_copy(uint32_t logical, void *buf, uint32_t len, int to_mac);
static int mac_copy(uint32_t logical, void *dst, const void *src, uint32_t len, int to_mac)
{
    while (len) {
        uint32_t chunk = 0x1000 - (logical & 0xfff); if (chunk > len) chunk = len;
        uint8_t *p = mac_page(logical);
        if (!p) return PSMB_E_BADADDR;
        if (to_mac) memcpy(p, src, chunk); else memcpy(dst, p, chunk);
        logical += chunk; len -= chunk;
        if (to_mac) src = (const uint8_t *)src + chunk; else dst = (uint8_t *)dst + chunk;
    }
    return 0;
}
int psmac_mac_copy(uint32_t logical, void *buf, uint32_t len, int to_mac) { return mac_copy(logical, buf, buf, len, to_mac); }

/* ---- block devices ---- */
struct blkdev { int64_t fd; uint32_t blocks, first; int writable; int ram; uint8_t *rambase; char name[48];   /* first: a host file's partition, in blocks */
                int usb;                           /* a USB stick's unit (psmac_usbdisk.c): its number + 1, 0 for an image */
                int present;                       /* USB: a disk the Mac may use is in the unit */
                uint32_t media, umedia;            /* the count the Mac's driver sees (psmb_blkdev.media), and psmac_usbdisk.c's */
                int view;                          /* the card's exchange folder as a locked HFS volume (psmac_card.c, issue #20) */
                int said_read; };
static struct blkdev bdev[PSMB_MAX_BLKDEV]; static int nbdev;
static int nimg;                                   /* the disk images among them (not the USB sticks): the startup disk's business */
static uint8_t sector_buf[64 * 1024] __attribute__((aligned(64)));   /* 64: a USB or view read may go straight in */
static int apm_hfs(const uint8_t *d, uint32_t have, uint32_t blocks, uint32_t *start, uint32_t *count);

/* the device table the Mac's driver reads (its VBL task too, since 20 November 2026: a USB stick
   comes and goes while the Mac runs). Size and name first, then the flags and the media count, which
   are what the driver acts on. */
static void blk_publish(void)
{
    for (int i = 0; i < PSMB_MAX_BLKDEV; i++) {
        volatile uint8_t *d = win + PSMB_OFF_BLKDEV + i * 64;
        const struct blkdev *b = &bdev[i];
        int in = i < nbdev && (!b->usb || b->present);
        wr32(d + 0, i < nbdev ? b->blocks : 0);
        wr32(d + 4, 512);
        for (int k = 0; k < 48; k++) ((volatile uint8_t *)d)[16 + k] = i < nbdev ? b->name[k] : 0;
        __asm__ volatile("dmb ish" ::: "memory");
        wr32(d + 12, i < nbdev ? b->media : 0);
        wr32(d + 8, (in ? PSMB_BD_PRESENT : 0) | (in && b->writable ? PSMB_BD_WRITABLE : 0) | (i < nbdev && b->usb ? PSMB_BD_REMOVABLE : 0));
    }
}
static void blk_add_file(const char *path, int writable)
{
    if (nbdev >= PSMB_MAX_BLKDEV || !psmac_sh_available()) return;
    int64_t fd = psmac_sh_open(path, writable ? 3 /* r+b */ : 1 /* rb */);
    if (fd < 0 && writable) { fd = psmac_sh_open(path, 1); writable = 0; }
    if (fd < 0) { kprintf("[PSMAC:MBOX] cannot open block image %s\n", path); return; }
    int64_t len = psmac_sh_flen(fd);
    struct blkdev *b = &bdev[nbdev];
    b->fd = fd; b->blocks = (uint32_t)(len / 512); b->writable = writable; b->ram = 0; b->first = 0;
    const char *base = path; for (const char *s = path; *s; s++) if (*s == '/') base = s + 1;
    int k = 0; while (base[k] && k < 47) { b->name[k] = base[k]; k++; } b->name[k] = 0;
    kprintf("[PSMAC:MBOX] block%d: %s, %u blocks (%u MiB)%s\n", nbdev, path, b->blocks, b->blocks / 2048, writable ? "" : " read-only");
    nimg++;
    /* a partitioned image as its HFS partition, as the card's images are served (until 20 November
       the testbed served the whole file, map and all, which the File Manager cannot mount) */
    uint32_t have = b->blocks < 64 ? b->blocks : 64, st, ct;
    if (have && psmac_sh_seek(fd, 0) >= 0 && psmac_sh_read(fd, sector_buf, have * 512) && apm_hfs(sector_buf, have, b->blocks, &st, &ct)) { b->first = st; b->blocks = ct; }
    nbdev++;
}
/* An image with an Apple partition map (a driver descriptor 'ER' in block 0, 'PM' entries from block
   1) is served as its first Apple_HFS partition, which is what the File Manager mounts and where the
   Start Manager looks for boot blocks; a bare HFS image as it is. PiDisk is one volume per device.
   d holds the image's first blocks (have of them); returns 1 with the partition in *start, *count. */
static int apm_hfs(const uint8_t *d, uint32_t have, uint32_t blocks, uint32_t *start, uint32_t *count)
{
    if (have < 3 || d[0] != 'E' || d[1] != 'R' || d[512] != 'P' || d[513] != 'M') return 0;
    uint32_t n = (uint32_t)d[516] << 24 | d[517] << 16 | d[518] << 8 | d[519];
    for (uint32_t i = 1; i <= n && i < have; i++) {
        const uint8_t *e = d + 512 * i;
        if (e[0] != 'P' || e[1] != 'M') break;
        uint32_t st = (uint32_t)e[8] << 24 | e[9] << 16 | e[10] << 8 | e[11], ct = (uint32_t)e[12] << 24 | e[13] << 16 | e[14] << 8 | e[15];
        const char *type = (const char *)e + 48, *want = "Apple_HFS";
        int k = 0; while (want[k] && type[k] == want[k]) k++;
        if (want[k] || !ct || st + ct > blocks) continue;
        kprintf("[PSMAC:MBOX]   its Apple_HFS partition: blocks %u-%u\n", st, st + ct - 1);
        *start = st; *count = ct;
        return 1;
    }
    return 0;
}
static void hfs_partition(uint8_t **data, uint32_t *blocks)
{
    uint32_t have = *blocks < 64 ? *blocks : 64, st, ct;
    { extern int psmac_card_need(const void *, uint32_t); psmac_card_need(*data, have * 512u); }   /* an image still coming in from the card: its map first */
    if (apm_hfs(*data, have, *blocks, &st, &ct)) { *data += (uint64_t)st * 512; *blocks = ct; }
}
static void blk_add_ram(const char *name, uint8_t *data, uint32_t blocks)
{
    if (nbdev >= PSMB_MAX_BLKDEV) return;
    hfs_partition(&data, &blocks);
    struct blkdev *b = &bdev[nbdev];
    b->fd = -1; b->blocks = blocks; b->writable = 1; b->ram = 1; b->rambase = data;
    int k = 0; while (name[k] && k < 47) { b->name[k] = name[k]; k++; } b->name[k] = 0;
    nbdev++; nimg++;
}
/* a disk image found on the card after the 68k started (psmac_card.c, core 3: this core) */
void psmac_mbox_add_disk(const char *name, uint8_t *data, uint32_t blocks)
{
    blk_add_ram(name, data, blocks);
    blk_publish();
    kprintf("[PSMAC:MBOX] block%d: %s, %u blocks (%u MiB), from the card\n", nbdev - 1, name, blocks, blocks / 2048);
}
/* the card's exchange folder, once psmac_card.c has made its HFS volume (core 3: this core). Not one of the
   card's images, so not a startup disk; a fixed disk, locked */
void psmac_mbox_add_view(const char *name, uint32_t blocks)
{
    if (nbdev >= PSMB_MAX_BLKDEV) { kprintf("[PSMAC:DISK] %s: all %d PiDisk units are in use: the Mac is not told about it\n", name, PSMB_MAX_BLKDEV); return; }
    struct blkdev *b = &bdev[nbdev];
    *b = (struct blkdev){ 0 };
    b->fd = -1; b->blocks = blocks; b->writable = 0; b->view = 1;
    int k = 0; while (name[k] && k < 47) { b->name[k] = name[k]; k++; } b->name[k] = 0;
    nbdev++;
    blk_publish();
    kprintf("[PSMAC:DISK] PiDisk unit %d is the card's folder \"%s\": %u blocks (%u MB), locked; the Mac's driver adds its drive\n", nbdev - 1, name, blocks, blocks / 2048);
}
/* The BLOCK command in hand while blk_xfer runs, and its progress word moved before each step that may
   wait: a host file's semihosting call, a chunk of an image still to come from the card (psmac_card.c
   calls this between its card reads). PiDisk spins a fixed number of turns on the done word and then
   waits on only while the progress word moves (psmac_usbdisk.c's commands always moved it); the other
   commands never did, and on 22 November 2026 one semihosting read of a host file not yet in the host's
   page cache (the first testbed run after a power cut) took 0.7 s, longer than the spin, and the
   Macintosh 512Ke's start ended in "prime error -036" and the question-mark disk (docs/08 item 96). */
static volatile uint8_t *blk_cmd_busy;
void psmac_mbox_blk_progress(void)
{
    volatile uint8_t *c = blk_cmd_busy;
    if (!c) return;
    wr32(c + 48, rd32(c + 48) + 1);
    __asm__ volatile("dmb ish" ::: "memory");
}
static int blk_xfer(int write, struct blkdev *b, uint32_t lba, uint32_t count, uint32_t buf)
{
    if ((uint64_t)lba + count > b->blocks) return PSMB_E_RANGE;
    if (write && !b->writable) return PSMB_E_RO;
    if (b->view) {                               /* made of the card's FAT, read when the Mac asks */
        extern int psmac_card_view_read(uint32_t, uint32_t, uint8_t *);
        while (count) {
            uint32_t n = count > sizeof(sector_buf) / 512 ? sizeof(sector_buf) / 512 : count;
            if (psmac_card_view_read(lba, n, sector_buf)) return PSMB_E_IO;
            int r = mac_copy(buf, NULL, sector_buf, n * 512, 1); if (r) return r;
            lba += n; count -= n; buf += n * 512;
        }
        return 0;
    }
    if (b->ram) {                                /* still coming in from the card? Blocks the card did not give: an I/O error */
        extern int psmac_card_need(const void *, uint32_t);
        psmac_mbox_blk_progress();
        if (psmac_card_need(b->rambase + (uint64_t)lba * 512, count * 512)) return PSMB_E_IO;
    }
    while (count) {
        uint32_t n = count > sizeof(sector_buf) / 512 ? sizeof(sector_buf) / 512 : count;
        if (b->ram) {
            /* from the Mac into the image: the destination is the image (until 19 November the two were
               swapped, and the first write to a PiDisk volume held in memory stopped core 3 with a
               store to address 0: docs/08 item 52) */
            if (write) { int r = mac_copy(buf, b->rambase + (uint64_t)lba * 512, NULL, n * 512, 0); if (r) return r;
                         extern void psmac_card_dirty(const void *, uint32_t); psmac_card_dirty(b->rambase + (uint64_t)lba * 512, n * 512); }
            else       { int r = mac_copy(buf, NULL, b->rambase + (uint64_t)lba * 512, n * 512, 1); if (r) return r; }
        } else {
            psmac_mbox_blk_progress();
            if (psmac_sh_seek(b->fd, ((uint64_t)b->first + lba) * 512) < 0) return PSMB_E_IO;
            if (write) {
                int r = mac_copy(buf, sector_buf, NULL, n * 512, 0); if (r) return r;
                if (!psmac_sh_write(b->fd, sector_buf, n * 512)) return PSMB_E_IO;
            } else {
                if (!psmac_sh_read(b->fd, sector_buf, n * 512)) return PSMB_E_IO;
                int r = mac_copy(buf, NULL, sector_buf, n * 512, 1); if (r) return r;
            }
        }
        lba += n; count -= n; buf += n * 512;
    }
    return 0;
}

/* ---- the startup disk: a disk image on the card as the Mac's startup disk (docs/08 item 67) ----
   The IIci ROM starts from a NuBus card only when the default startup device in parameter RAM
   ($78-$7B) names a slot and an sResource; it then runs that sResource's boot record, which opens
   the card's driver and says which driver's drives to try first (mac/pidisk_boot.s). PiDisk's
   driver is otherwise opened only after a System has started from somewhere else, so with no SCSI
   disk and no floppy the Mac stopped at the question-mark disk.
   So from each RESET until the boot record has asked which disk goes first (PSMB_BLK_BOOT), the
   Mac's view of those four bytes is the record for slot 9's PiDisk, sResource $80, whenever an image
   is published and mac.boot= is not scsi. Its own writes there in that time are kept in that view
   and go no further: the ROM's parameter RAM test reads every byte, writes a pattern, reads it back
   and writes back what it read (0x40841DA2), and a view that ignored writes failed that test at $78
   and had the ROM store the slot-9 record in the real parameter RAM. Once the boot record has asked,
   the bytes are the parameter RAM's own again, so the Startup Disk control panel sees and sets the
   real choice. A byte the Mac writes there is given back only to a read of that same byte that comes
   next, with no other access to $78-$7B between; every other read is given slot 9. The test writes a
   pattern into one byte and reads that byte at once, then the next byte; when the ROM resets a
   parameter RAM it does not trust it writes zeros to the four bytes, then ff ff ff df (SCSI ID 0, its
   default startup device), and reads the four back. A view that kept every write showed the Mac that
   SCSI default, the card's boot record never ran, and the first start of a Macintosh 512Ke whose clock
   chip held no IIci parameter RAM ended at the question-mark disk (22 November 2026, docs/08 item 96).
   The board layers serve the view (psmac_board_se.c for the SE's clock chip or its
   stand-in, psmac_board_solo.c) and report what the parameter RAM really holds. The answer to the
   boot record follows mac.boot=:
     auto (the default)  the SCSI disk the parameter RAM names (Apple's default, ID 0, when it names
                         none or another card) first, then the card's image, then the other SCSI
                         disks: the image starts the Mac when the usual startup disk is not there;
     image               the card's image first, the SCSI disks after it;
     scsi                parameter RAM as it is, the image only if it names the card (the Startup Disk
                         control panel): the kernel changes nothing.
   A floppy with a System is taken first in every case, as on any Mac. */
static const uint8_t startup_slot9[4] = { 0, 0, 9, 0x80 };   /* sdExtDevID, sdPartition, sdSlotNum, sdSRsrcID */
static uint8_t startup_real[4] = { 0xff, 0xff, 0xff, 0xdf };   /* the parameter RAM's own bytes, as last read */
static uint8_t startup_known;                                  /* bit i: startup_real[i] was read from the parameter RAM this session */
static uint8_t startup_view[4];                                /* bytes the Mac wrote while the view is up */
static int startup_last = -1;                                  /* the byte the Mac wrote last, until its next access to $78-$7B */
static volatile int startup_window = 1;                        /* the view is up: from each RESET until the boot record asks */
static int startup_mode = -1;                                  /* 0 auto, 1 image, 2 scsi */
static int startup_said;
static int startup_mode_get(void)
{
    if (startup_mode < 0) {
        const char *o = psmac_option("mac.boot");
        startup_mode = !o ? 0 : o[0] == 'i' ? 1 : o[0] == 's' ? 2 : 0;
        if (o && o[0] != 'i' && o[0] != 's' && o[0] != 'a') kprintf("[PSMAC:MBOX] mac.boot=%s not understood: auto, image or scsi; auto it is\n", o);
        for (int i = 0; i < 4; i++) startup_view[i] = startup_slot9[i];
    }
    return startup_mode;
}
/* the 68k's RESET (psmac_bus.c): a new start, the view up again. A USB disk's command still in
   progress then has nobody waiting for it (usb_req_step). */
static volatile uint32_t mbox_resets;
void psmac_startup_reset(void)
{
    __atomic_store_n(&mbox_resets, mbox_resets + 1, __ATOMIC_RELEASE);
    startup_mode_get();
    for (int i = 0; i < 4; i++) startup_view[i] = startup_slot9[i];
    startup_last = -1;
    startup_said = 0;
    __atomic_store_n(&startup_window, 1, __ATOMIC_RELEASE);
}
/* For the board layers. psmac_startup_card: is the view up (an image published, mac.boot= not
   scsi, the boot record has not yet asked)? psmac_startup_seen: a byte $78-$7B as the parameter
   RAM really holds it. psmac_startup_view: what the Mac is given while the view is up.
   psmac_startup_read: all three, for a board layer that has the byte before the Mac reads it.
   psmac_startup_write: the Mac wrote a byte there; 1 when it went into the view, and must go no
   further. psmac_startup_real: the parameter RAM's byte, if read this session (for the SE's chip,
   whose writes the board layer can only rewrite, not hold back). */
int psmac_startup_card(void) { return startup_window && startup_mode_get() != 2 && nimg > 0; }
void psmac_startup_seen(int addr, uint8_t real)
{
    int i = addr - 0x78;
    if (i < 0 || i > 3) return;
    startup_real[i] = real; startup_known |= (uint8_t)(1u << i);
    if (i == 3 && !startup_said && psmac_startup_card()) {
        startup_said = 1;
        kprintf("[PSMAC:MBOX] startup disk: parameter RAM $78-$7B %02x %02x %02x %02x, shown to the Mac as 00 00 09 80, slot 9's PiDisk, until its boot record asks (mac.boot=%s)\n",
                startup_real[0], startup_real[1], startup_real[2], startup_real[3], startup_mode == 1 ? "image" : "auto");
    }
}
uint8_t psmac_startup_view(int addr)
{
    int i = (addr - 0x78) & 3, last = startup_last;
    startup_last = -1;
    return last == i ? startup_view[i] : startup_slot9[i];
}
uint8_t psmac_startup_read(int addr, uint8_t real)
{
    if (addr < 0x78 || addr > 0x7b) return real;
    psmac_startup_seen(addr, real);
    return psmac_startup_card() ? psmac_startup_view(addr) : real;
}
int psmac_startup_write(int addr, uint8_t v)
{
    if (addr < 0x78 || addr > 0x7b || !psmac_startup_card()) return 0;
    startup_view[addr - 0x78] = v;
    startup_last = addr - 0x78;
    return 1;
}
int psmac_startup_real(int addr, uint8_t *v)
{
    int i = addr - 0x78;
    if (i < 0 || i > 3 || !(startup_known & (1u << i))) return 0;
    *v = startup_real[i];
    return 1;
}
/* PSMB_BLK_BOOT: 0 = the image first, else the reference number of the SCSI driver to try first.
   The view comes down: the bytes are the parameter RAM's own from now on. */
static uint32_t startup_first(void)
{
    const char *why;
    uint32_t r = 0;
    int16_t ref = (int16_t)(startup_real[2] << 8 | startup_real[3]);
    if (startup_mode_get() != 0) why = startup_mode == 1 ? "the card's image first (mac.boot=image)" : "the card's image (the parameter RAM names it)";
    else if (startup_real[2] == 9 && startup_real[3] == 0x80) why = "the card's image first (the parameter RAM names it)";
    else if (ref < 0) { r = (uint16_t)ref; why = "the SCSI disk the parameter RAM names first, then the card's image (mac.boot=auto)"; }
    else { r = 0xffdf; why = "SCSI ID 0 first (the parameter RAM names none), then the card's image (mac.boot=auto)"; }
    __atomic_store_n(&startup_window, 0, __ATOMIC_RELEASE);
    if (r >= 0xffd9 && r <= 0xffdf) kprintf("[PSMAC:MBOX] startup disk: %s: SCSI ID %u, refNum %d; %d image(s) on the card\n", why, 0xffdf - r, (int16_t)r, nimg);
    else kprintf("[PSMAC:MBOX] startup disk: %s; %d image(s) on the card\n", why, nimg);
    return r;
}

/* ---- network: frames exchanged with a host helper through two append-only files ----
   tx: we append {u16 len, frame}; rx: the helper appends the same records, we read from our offset.
   There are two such pairs. Pair 0 (mac.net=BASE) is the testbed's Ethernet: the NET mailbox
   service below and, through the primitives at the end, unit 1 of the Alice68 network board.
   Pair 1 (mac.wifinet=BASE) stands in for the WiFi chip's air interface: unit 0's frames go
   through it while the simulated radio (alice_psmac.c, mac.wifisim=) says it is connected, so the
   Mac's WiFi driver and MacTCP over it can be exercised in QEMU, which has no WiFi chip. */
struct netfile { int64_t tx_fd, rx_fd; uint64_t rx_off; int up; };
static struct netfile nf[2] = { { -1, -1, 0, 0 }, { -1, -1, 0, 0 } };
#define net_up (nf[0].up)
static uint8_t net_mac[6] = { 0x02, 0x50, 0x53, 0x4D, 0x41, 0x43 };   /* locally administered "PSMAC" */
static uint8_t net_buf[PSMB_NETRX_FRAME + 2];
static uint32_t net_filter;
static int mac_eq(const uint8_t *a, const uint8_t *b) { for (int i = 0; i < 6; i++) if (a[i] != b[i]) return 0; return 1; }

static void netfile_open(struct netfile *f, const char *key)
{
    const char *base = psmac_option(key);
    if (!base || !psmac_sh_available()) return;
    char tx[128], rx[128]; int n = 0; while (base[n] && n < 100 && base[n] != ' ') { tx[n] = rx[n] = base[n]; n++; }
    memcpy(tx + n, ".tx", 4); memcpy(rx + n, ".rx", 4);
    f->tx_fd = psmac_sh_open(tx, 9 /* ab */);
    f->rx_fd = psmac_sh_open(rx, 1 /* rb */);
    f->up = f->tx_fd >= 0 && f->rx_fd >= 0;
    if (f->up) f->rx_off = (uint64_t)psmac_sh_flen(f->rx_fd);   /* start from the current end */
    kprintf("[PSMAC:MBOX] %s: %s (%s.tx/.rx) %s\n", key, base, base, f->up ? "up" : "unavailable");
}
static void net_init(void) { netfile_open(&nf[0], "mac.net"); netfile_open(&nf[1], "mac.wifinet"); }
/* one record from a pair's rx file into buf: the frame's length, 0 if none (or none that fits) */
static int netfile_recv(struct netfile *f, uint8_t *buf, uint32_t max)
{
    if (!f->up) return 0;
    int64_t len = psmac_sh_flen(f->rx_fd);
    if (len < 0 || (uint64_t)len < f->rx_off + 2) return 0;
    if (psmac_sh_seek(f->rx_fd, f->rx_off) < 0) return 0;
    uint8_t hdr[2]; if (!psmac_sh_read(f->rx_fd, hdr, 2)) return 0;
    uint32_t flen = (hdr[0] << 8) | hdr[1];
    if (flen > max || (uint64_t)len < f->rx_off + 2 + flen) { if (flen > max) f->rx_off += 2 + flen; return 0; }
    if (!psmac_sh_read(f->rx_fd, buf, flen)) return 0;
    f->rx_off += 2 + flen;
    return (int)flen;
}
static int netfile_send(struct netfile *f, const uint8_t *frame, uint32_t len)
{
    if (!f->up || len > PSMB_NETRX_FRAME) return 0;
    uint8_t hdr[2] = { len >> 8, len & 0xff };
    return psmac_sh_write(f->tx_fd, hdr, 2) && psmac_sh_write(f->tx_fd, frame, len);
}
static void net_poll_rx(void)
{
    if (!net_up) return;
    volatile uint8_t *ring = win + PSMB_OFF_NETRX;
    for (int budget = 0; budget < 8; budget++) {
        uint32_t head = rd32(ring), tail = rd32(ring + 4);
        uint32_t next = (head + 1) % PSMB_NETRX_SLOTS;
        if (next == tail) { return; }                 /* ring full: leave the frame in the file */
        int flen = netfile_recv(&nf[0], net_buf, PSMB_NETRX_FRAME);
        if (flen <= 0) return;
        /* destination filter: ours, broadcast, multicast (if enabled) or promiscuous */
        int mine = (net_filter & PSMB_NETF_PROMISC) || mac_eq(net_buf, net_mac) ||
                   (net_buf[0] == 0xff && net_buf[1] == 0xff && net_buf[2] == 0xff && net_buf[3] == 0xff && net_buf[4] == 0xff && net_buf[5] == 0xff) ||
                   ((net_buf[0] & 1) && (net_filter & PSMB_NETF_MULTICAST));
        if (!mine) continue;
        volatile uint8_t *slot = ring + 16 + head * (8 + PSMB_NETRX_FRAME);
        memcpy((void *)(slot + 8), net_buf, (uint32_t)flen);
        wr32(slot, (uint32_t)flen);
        __asm__ volatile("dmb ish" ::: "memory");
        wr32(ring, next);
    }
}
/* primitives for alice_psmac.c's testbed network units: which = 0 Ethernet (mac.net), 1 WiFi (mac.wifinet) */
int psmac_netfile_up(int which) { return nf[which & 1].up; }
int psmac_netfile_send(int which, const uint8_t *frame, uint32_t len) { return netfile_send(&nf[which & 1], frame, len); }
int psmac_netfile_recv(int which, uint8_t *buf, uint32_t max) { return netfile_recv(&nf[which & 1], buf, max); }
static int net_send(uint32_t buf, uint32_t len)
{
    if (!net_up) return PSMB_E_IO;
    if (len < 14 || len > PSMB_NETRX_FRAME) return PSMB_E_RANGE;
    int r = mac_copy(buf, net_buf + 2, NULL, len, 0); if (r) return r;
    net_buf[0] = len >> 8; net_buf[1] = len & 0xff;
    return psmac_sh_write(nf[0].tx_fd, net_buf, len + 2) ? 0 : PSMB_E_IO;
}

/* ---- USB disks (psmac_usbdisk.c, core 1) as PiDisk units (20 November 2026) ----
   A stick psmac_usbdisk.c offers becomes a unit of the device table the first time it is present and
   keeps that unit for the session, so the Mac's driver keeps one drive for it: a stick pulled out and
   put back is a disk put back into its drive, which is how the File Manager finds the volume it still
   has (offline) and puts it back online. Each new disk moves the unit's media count, which is what
   the driver's VBL task watches for: it then posts the disk-inserted event. A unit whose disk is
   gone (ejected, pulled out) is published without PSMB_BD_PRESENT; the driver marks its drive empty
   and answers the File Manager offLinErr, as for a floppy taken out.
   The Mac's reads and writes go to core 1 as jobs of at most 64 KB (psmac_usbdisk_post); the command
   stays unanswered meanwhile, its progress word moving so that the driver goes on waiting, and core 3
   copies the data between the Mac's buffer and the job's. */
static uint32_t stats_cmds, stats_err;            /* (the services' counters, below) */
static int usb_slot[USBDISK_UNITS] = { -1, -1, -1, -1 };
static uint32_t usb_changes_seen;
static int usb_full_said;
static void usb_sync(void)
{
    uint32_t ch = psmac_usbdisk_changes();
    int retry = 0;
    if (ch == usb_changes_seen) return;
    for (int u = 0; u < USBDISK_UNITS; u++) {
        struct usbdisk_info in;
        if (!psmac_usbdisk_info(u, &in)) { retry = 1; continue; }
        int s = usb_slot[u];
        if (s < 0) {
            if (!in.present) continue;
            if (nbdev >= PSMB_MAX_BLKDEV) {
                if (!usb_full_said++) kprintf("[PSMAC:DISK] USB disk %d: all %d PiDisk units are in use (disk images and sticks): the Mac is not told about it\n", u, PSMB_MAX_BLKDEV);
                continue;
            }
            s = usb_slot[u] = nbdev++;
            bdev[s] = (struct blkdev){ 0 };
            bdev[s].fd = -1; bdev[s].usb = u + 1;
        }
        struct blkdev *b = &bdev[s];
        int k = 0; while (in.name[k] && k < 47) { b->name[k] = in.name[k]; k++; } b->name[k] = 0;
        b->blocks = in.blocks; b->writable = (int)in.writable;
        if (in.present && (!b->present || in.media != b->umedia)) {
            b->media++; b->umedia = in.media; b->said_read = 0;
            kprintf("[PSMAC:DISK] PiDisk unit %d is USB disk %d, %s: %u blocks (%u MB), %s; the Mac's driver is told a disk was inserted\n",
                    s, u, b->name, b->blocks, b->blocks / 2048, b->writable ? "writable" : "read-only");
        } else if (!in.present && b->present)
            kprintf("[PSMAC:DISK] PiDisk unit %d (USB disk %d): no disk in it now; the Mac's driver is told its drive is empty\n", s, u);
        b->present = (int)in.present;
    }
    if (!retry) usb_changes_seen = ch;
    blk_publish();
}

/* the Mac's command in progress on a USB unit */
static struct { int active; int bdev, op, abandoned; uint32_t lba, left, buf, done, n, req, resets; uint64_t t0; int waiting; } ur;
static void usb_finish(volatile uint8_t *c, int32_t status)
{
    if (!status && ur.op == USBDISK_OP_READ) { extern uint32_t psmac_icache_epoch; psmac_icache_epoch++; }
    stats_cmds++; if (status) stats_err++;
    wr32(c + 36, (uint32_t)status); wr32(c + 40, 0); wr32(c + 44, ur.done * 512);
    __asm__ volatile("dmb ish" ::: "memory");
    __atomic_store_n((volatile uint32_t *)(c + 4), ur.req, __ATOMIC_RELEASE);
    ur.active = 0;
}
/* the next part of the command to core 1: 0 posted (or waiting for the job slot), else a status */
static int32_t usb_next(void)
{
    struct blkdev *b = &bdev[ur.bdev];
    int u = b->usb - 1;
    int32_t old;
    ur.waiting = 1;
    if (psmac_usbdisk_result(&old) == 0) return 0;         /* a job the Mac gave up on is still running: wait for it */
    if (!b->present) return PSMB_E_NODATA;
    ur.n = ur.op == USBDISK_OP_EJECT ? 0 : ur.left;
    { uint32_t m = psmac_usbdisk_max_blocks(u); if (ur.n > m) ur.n = m; }
    if (ur.op == USBDISK_OP_WRITE) { int r = mac_copy(ur.buf, psmac_usbdisk_buffer(), NULL, ur.n * 512, 0); if (r) return r; }
    if (psmac_usbdisk_post(u, b->umedia, ur.op, ur.lba, ur.n)) return 0;
    ur.waiting = 0;
    return 0;
}
/* a READ, WRITE or EJECT on a USB unit: 1 when it goes on in the background (usb_req_step answers it) */
static int usb_begin(volatile uint8_t *c, int bi, uint16_t cmd, uint32_t lba, uint32_t count, uint32_t buf, uint32_t req, int32_t *status)
{
    struct blkdev *b = &bdev[bi];
    int op = cmd == PSMB_BLK_READ ? USBDISK_OP_READ : cmd == PSMB_BLK_WRITE ? USBDISK_OP_WRITE : USBDISK_OP_EJECT;
    (void)c;
    if (!b->present) { *status = op == USBDISK_OP_EJECT ? 0 : PSMB_E_NODATA; return 0; }
    if (op != USBDISK_OP_EJECT) {
        if ((uint64_t)lba + count > b->blocks) { *status = PSMB_E_RANGE; return 0; }
        if (op == USBDISK_OP_WRITE && !b->writable) { *status = PSMB_E_RO; return 0; }
        if (!count) { *status = 0; return 0; }
        if (op == USBDISK_OP_READ && !b->said_read) { b->said_read = 1; kprintf("[PSMAC:DISK] PiDisk unit %d (USB disk %d): the Mac's first read, block %u: it is mounting the disk\n", bi, b->usb - 1, lba); }
    } else kprintf("[PSMAC:DISK] PiDisk unit %d (USB disk %d): the Mac ejects it\n", bi, b->usb - 1);
    ur.active = 1; ur.bdev = bi; ur.op = op; ur.abandoned = 0; ur.lba = lba; ur.left = count; ur.buf = buf; ur.done = 0;
    ur.req = req; ur.resets = __atomic_load_n(&mbox_resets, __ATOMIC_ACQUIRE); ur.t0 = cnt_now();
    int32_t r = usb_next();
    if (r) { ur.active = 0; *status = r; return 0; }
    return 1;
}
static void usb_req_step(volatile uint8_t *c)
{
    int32_t st;
    wr32(c + 48, rd32(c + 48) + 1);                       /* progress: the driver goes on waiting */
    if (!ur.abandoned && (rd32(c) != ur.req || __atomic_load_n(&mbox_resets, __ATOMIC_ACQUIRE) != ur.resets)) {
        ur.abandoned = 1;
        kprintf("[PSMAC:DISK] PiDisk unit %d: the Mac stopped waiting for its %s (%s); it is finished and dropped\n", ur.bdev,
                ur.op == USBDISK_OP_READ ? "read" : ur.op == USBDISK_OP_WRITE ? "write" : "eject", rd32(c) != ur.req ? "a new command" : "the 68k was reset");
    }
    if (ur.waiting) {
        if (ur.abandoned) { if (psmac_usbdisk_result(&st) != 0) ur.active = 0; return; }
        int32_t r = usb_next();
        if (r) usb_finish(c, r);
        return;
    }
    int r = psmac_usbdisk_result(&st);
    if (r == 0) {
        /* core 1 bounds every transfer, so a job ends; a minute is far beyond any of them */
        if (!ur.abandoned && cnt_now() - ur.t0 > 60 * cnt_frq()) {
            kprintf("[PSMAC:DISK] PiDisk unit %d: no answer from the USB disk in a minute: an I/O error for the Mac\n", ur.bdev);
            ur.abandoned = 1;
            ur.waiting = 1;                                /* the slot is drained before the next command */
            wr32(c + 36, (uint32_t)PSMB_E_IO); wr32(c + 44, 0);
            __asm__ volatile("dmb ish" ::: "memory");
            __atomic_store_n((volatile uint32_t *)(c + 4), ur.req, __ATOMIC_RELEASE);
        }
        return;
    }
    if (r < 0) st = PSMB_E_IO;                             /* (no job: cannot happen) */
    if (ur.abandoned) {
        /* after a RESET nobody posts this command again: retire it, without touching the Mac's memory */
        if (rd32(c) == ur.req && rd32(c + 4) != ur.req) {
            wr32(c + 36, (uint32_t)PSMB_E_IO);
            __asm__ volatile("dmb ish" ::: "memory");
            __atomic_store_n((volatile uint32_t *)(c + 4), ur.req, __ATOMIC_RELEASE);
        }
        ur.active = 0;
        return;
    }
    if (st) { usb_finish(c, st); return; }
    if (ur.op == USBDISK_OP_READ) { int e = mac_copy(ur.buf, NULL, psmac_usbdisk_buffer(), ur.n * 512, 1); if (e) { usb_finish(c, e); return; } }
    ur.done += ur.n; ur.left -= ur.n; ur.lba += ur.n; ur.buf += ur.n * 512;
    if (!ur.left || ur.op == USBDISK_OP_EJECT) { usb_finish(c, 0); return; }
    int32_t e = usb_next();
    if (e) usb_finish(c, e);
}

/* ---- services ---- */
static void svc_set(int idx, uint16_t type, uint32_t caps, const uint32_t *param)
{
    volatile uint8_t *s = win + PSMB_OFF_SERVICES + idx * 32;
    wr16(s, type); wr16(s + 2, idx); wr32(s + 4, PSMB_OFF_CMD + idx * 64); wr32(s + 8, caps);
    for (int k = 0; k < 5; k++) wr32(s + 12 + 4 * k, param ? param[k] : 0);
}
static uint32_t stats_cmds, stats_err;

static int run_cmd(int idx, uint16_t type, volatile uint8_t *c, uint32_t req)   /* 1: answered later (usb_req_step) */
{
    uint16_t cmd = rd16(c + 8); uint32_t a0 = rd32(c + 12), a1 = rd32(c + 16), a2 = rd32(c + 20);
    uint32_t buf = rd32(c + 28), len = rd32(c + 32);
    int32_t status = 0; uint32_t result = 0, xfer = 0;
    (void)idx;
    switch (type) {
    case PSMB_SVC_INFO:
        if (cmd == PSMB_INFO_PING) result = ~a0;
        else if (cmd == PSMB_INFO_LOG) {
            char tmp[128]; uint32_t n = len > 127 ? 127 : len;
            if (mac_copy(buf, tmp, NULL, n, 0) == 0) { tmp[n] = 0; kprintf("[MAC] %s\n", tmp); psmac_boot_mac_line((const char *)tmp); xfer = n; } else status = PSMB_E_BADADDR;
        } else if (cmd == PSMB_INFO_TIME) result = 0;
        else if (cmd == PSMB_INFO_SETOPT) {
            extern int psmac_card_edit_options(const char *, uint32_t);
            char tmp[512]; uint32_t n = len > 511 ? 511 : len;
            if (mac_copy(buf, tmp, NULL, n, 0)) status = PSMB_E_BADADDR;
            else { int r = psmac_card_edit_options(tmp, n); status = r == -1 ? PSMB_E_RO : r == -2 ? PSMB_E_BUSY : r ? PSMB_E_RANGE : 0; if (!r) { tmp[n] = 0; kprintf("[MAC] cmdline.txt edit asked: %u bytes\n", n); } }
        } else if (cmd == PSMB_INFO_OPTSTATE) { extern int psmac_card_options_state(void); result = (uint32_t)psmac_card_options_state(); }
        else if (cmd == PSMB_INFO_SAVELOG) { extern int psmac_card_save_log(int); if (psmac_card_save_log(0)) status = PSMB_E_RO; else kprintf("[MAC] the log to the card asked\n"); }
        else if (cmd == PSMB_INFO_LOGSTATE) { extern int psmac_card_log_state(void); result = (uint32_t)psmac_card_log_state(); }
        else if (cmd >= PSMB_INFO_DISPINFO && cmd <= PSMB_INFO_DISPRESTART) {      /* PiDisplay (psmac_hdmi.c) */
            extern int psmac_hdmi_cmd(uint16_t, uint32_t, uint32_t, uint32_t, uint32_t *, const void **, uint32_t *);
            const void *out; uint32_t n;
            status = psmac_hdmi_cmd(cmd, a0, a1, a2, &result, &out, &n);
            if (!status && out) { if (n > len) n = len; if (mac_copy(buf, NULL, out, n, 1)) status = PSMB_E_BADADDR; else xfer = n; }
        }
        else if (cmd == PSMB_INFO_SANE) {                                         /* A68 Machine (psmac_sane.c) */
            extern int psmac_sane_get(uint32_t, const uint8_t **, uint32_t *);
            const uint8_t *p; uint32_t n;
            status = psmac_sane_get(a0, &p, &n);
            if (!status) {
                result = n;
                if (len) { if (len > n) len = n; if (mac_copy(buf, NULL, p, len, 1)) status = PSMB_E_BADADDR; else xfer = len; }
            }
        }
        else status = PSMB_E_BADCMD;
        break;
    case PSMB_SVC_BLOCK: {
        if (cmd == PSMB_BLK_INFO) { blk_publish(); result = nbdev; break; }
        if (cmd == PSMB_BLK_BOOT) { extern int psmac_card_scanned(void); if (!psmac_card_scanned()) status = PSMB_E_BUSY; else result = startup_first(); break; }
        if (a0 >= (uint32_t)nbdev) { status = PSMB_E_BADDEV; break; }
        struct blkdev *b = &bdev[a0];
        if (b->usb && (cmd == PSMB_BLK_READ || cmd == PSMB_BLK_WRITE || cmd == PSMB_BLK_EJECT)) {
            if (usb_begin(c, (int)a0, cmd, a1, a2, buf, req, &status)) return 1;
            if (!status && cmd != PSMB_BLK_EJECT) xfer = a2 * 512;
            break;
        }
        if (cmd == PSMB_BLK_READ)       { static int first = 1; if (first) { first = 0; psmac_boot_mark("PiDisk: first block read (device %u, block %u)", a0, a1); psmac_splash_status("Starting from the disk image on the SD card..."); }
                                          blk_cmd_busy = c; status = blk_xfer(0, b, a1, a2, buf); blk_cmd_busy = NULL; if (!status) { xfer = a2 * 512; extern uint32_t psmac_icache_epoch; psmac_icache_epoch++; } }
        else if (cmd == PSMB_BLK_WRITE) { blk_cmd_busy = c; status = blk_xfer(1, b, a1, a2, buf); blk_cmd_busy = NULL; if (!status) xfer = a2 * 512; }
        else if (cmd == PSMB_BLK_FLUSH || cmd == PSMB_BLK_EJECT) status = 0;
        else status = PSMB_E_BADCMD;
        break; }
    case PSMB_SVC_NET:
        if (cmd == PSMB_NET_SEND) { status = net_send(buf, len); if (!status) xfer = len; }
        else if (cmd == PSMB_NET_GETADDR) { result = (net_mac[0] << 24) | (net_mac[1] << 16) | (net_mac[2] << 8) | net_mac[3]; xfer = (net_mac[4] << 8) | net_mac[5]; }
        else if (cmd == PSMB_NET_SETFILTER) net_filter = a0;
        else if (cmd == PSMB_NET_LINK) result = net_up;
        else status = PSMB_E_BADCMD;
        break;
    default:
        status = PSMB_E_BADCMD;
    }
    stats_cmds++; if (status) stats_err++;
    wr32(c + 36, (uint32_t)status); wr32(c + 40, result); wr32(c + 44, xfer);
    return 0;
}

/* ---- public ---- */
void psmac_mbox_init(uintptr_t window_phys, uintptr_t ramp, uint32_t rams)
{
    win_phys = window_phys; ram_phys = ramp; ram_size = rams;
    win = (uint8_t *)(KERNEL_ALIAS + win_phys);
    for (uint32_t i = 0; i < PSMB_OFF_BOUNCE; i += 8) *(uint64_t *)(win + i) = 0;
    t0 = cnt_now();

    /* services: 0 info, 1 block, 2 net */
    const char *d0 = psmac_option("mac.disk0"), *d1 = psmac_option("mac.disk1");
    if (d0) blk_add_file(d0, 1);
    if (d1) blk_add_file(d1, 1);
    {   /* disk images from the container: PiDisk volumes here, the solo board's SCSI disks there */
        extern int psmac_board_solo; extern int psmac_image_count(void); extern int psmac_image_get(int, const char **, uint8_t **, uint32_t *);
        extern int psmac_image_is_scsi(int);       /* the solo board's first image is its SCSI disk, the rest are PiDisk volumes */
        for (int i = 0; i < psmac_image_count(); i++) { const char *nm; uint8_t *dp; uint32_t bl; if (!(psmac_board_solo && psmac_image_is_scsi(i)) && psmac_image_get(i, &nm, &dp, &bl)) blk_add_ram(nm, dp, bl); }
    }
    blk_publish();
    net_init();

    /* the Alice68 boards inside the window (alice_psmac.h): network board, status page, input ring */
    { extern void psmac_alice_init(uint8_t *win); psmac_alice_init(win); }
    { extern void psmac_display_init(uint8_t *win); psmac_display_init(win); }
    { extern void psmac_log_attach(uint8_t *win); psmac_log_attach(win); }   /* the kernel's log for PiLog (psmb.h) */   /* the second display (psmac_display.c) */
    uint32_t pa[5] = { ALICE_NET_WINDOW_OFF, ALICE_STATUS_WINDOW_OFF, ALICE_INPUT_WINDOW_OFF, 0, 0 };
    svc_set(3, PSMB_SVC_ALICE, 0, pa);
    svc_set(0, PSMB_SVC_INFO, 0, NULL);
    uint32_t pb[5] = { (uint32_t)nbdev, 0, 0, 0, 0 };
    svc_set(1, PSMB_SVC_BLOCK, 0, pb);
    uint32_t pn[5] = { (net_mac[0] << 24) | (net_mac[1] << 16) | (net_mac[2] << 8) | net_mac[3], (net_mac[4] << 8) | net_mac[5], PSMB_OFF_NETRX, PSMB_NETRX_SLOTS, PSMB_NETRX_FRAME };
    svc_set(2, PSMB_SVC_NET, net_up ? 1 : 0, pn);

    volatile uint8_t *h = win;
    wr32(h + 0, PSMB_MAGIC); wr16(h + 4, PSMB_VERSION); wr16(h + 6, 4);
    wr32(h + 8, PSMB_FEAT_BLOCK | (net_up ? PSMB_FEAT_NET : 0));
    wr32(h + 12, 0); wr32(h + 16, psmac_mac_ram_mb()); wr32(h + 20, 0);
    const char *ver = "Aurora68 psmac " __DATE__;   /* the build string PiNet shows */ int k = 0; while (ver[k] && k < 31) { ((volatile uint8_t *)h)[24 + k] = ver[k]; k++; }
    { extern int psmac_board_se, psmac_board_solo, psmac_board_plus, psmac_board_orig;   /* which logic board the Mac is in (psmb.h, struct psmb_header.board) */
      static const uint32_t orig[] = { PSMB_BOARD_PLUS, PSMB_BOARD_128K, PSMB_BOARD_512K, PSMB_BOARD_512KE };
      wr32(h + 56, psmac_board_plus ? orig[psmac_board_orig & 3] : psmac_board_solo ? PSMB_BOARD_SOLO : psmac_board_se ? PSMB_BOARD_SE : PSMB_BOARD_IICI); }
    kprintf("[PSMAC:MBOX] window at Mac 0x%08x (Pi %p): %d block device(s), net %s\n", PSMB_WIN_PHYS, (void *)win_phys, nbdev, net_up ? "up" : "off");
}

/* called from the service core loop; returns 1 if work was done */
int psmac_mbox_poll(void)
{
    if (!win) return 0;
    int did = 0;
    usb_sync();                                            /* USB sticks come and go (psmac_usbdisk.c) */
    for (int i = 0; i < 3; i++) {
        volatile uint8_t *c = win + PSMB_OFF_CMD + i * 64;
        if (i == 1 && ur.active) { usb_req_step(c); did = 1; continue; }   /* the block service waits for a USB disk */
        uint32_t req = __atomic_load_n((volatile uint32_t *)c, __ATOMIC_ACQUIRE);
        uint32_t done = *(volatile uint32_t *)(c + 4);
        if (req == done) continue;
        uint16_t type = rd16(win + PSMB_OFF_SERVICES + i * 32);
        if (run_cmd(i, type, c, req)) { did = 1; continue; }
        __asm__ volatile("dmb ish" ::: "memory");
        __atomic_store_n((volatile uint32_t *)(c + 4), req, __ATOMIC_RELEASE);
        did = 1;
    }
    /* frames for the old PSMB net service: only when nobody else consumes the host file */
    { extern int psmac_alice_owns_net(void); if (!psmac_alice_owns_net()) net_poll_rx(); }
    return did;
}

/* The IIci ROM's timing calibration (TimeDBRA, TimeSCCDB, TimeSCSIDB, TimeVIADB: loop iterations
   per millisecond) is meaningless under a JIT: the CPU loop overflows 16 bits and the bus loops
   measure the bridge. The patched ROM takes the values from here instead. The bus loops get the
   real bridge rate so that SCSI and VIA timeouts, which are counted in bus accesses, keep their
   intended duration.
   TimeDBRA was 0x7FFF, "large", until 19 November. But every self-looping dbf costs one read of the
   system timer per turn since docs/08 item 27 (M68k_LINE5.c, mac.dbfslow), so a delay the ROM
   counts in TimeDBRA turns lasted 0x7FFF reads per millisecond asked: the half second the SCSI
   Manager waits after resetting the bus took 23 s under QEMU and, at a tenth of a microsecond or
   more per read, one and a half seconds or more on a Pi 3 (docs/08 item 50). The rate is measured
   now, a few thousand reads of the same counter, so those delays last what the ROM meant. */
static uint32_t dbf_turns_per_ms(void)
{
    extern uint32_t psmac_dbf_slow;
    if (!psmac_dbf_slow) return 0x7fff;                  /* mac.dbfslow=0: the loops are free, "large" it is */
    volatile uint32_t *clo = (volatile uint32_t *)(uintptr_t)(PSMAC_PERIPH_HI + 0x3004);
    uint32_t sink = 0;
    uint64_t t0 = cnt_now();
    for (int i = 0; i < 4096; i++) sink += *clo;
    uint64_t dt = cnt_now() - t0;
    (void)sink;
    uint64_t per_ms = dt ? (uint64_t)4096 * (cnt_frq() / 1000) / (dt * psmac_dbf_slow) : 0x7fff;
    if (per_ms < 64) per_ms = 64;
    if (per_ms > 0x7fff) per_ms = 0x7fff;
    return (uint32_t)per_ms;
}
void psmac_mbox_set_calib(uint32_t bus_latency_us)
{
    if (!win) return;
    uint32_t per_ms = bus_latency_us ? 1000 / bus_latency_us : 2000;
    if (per_ms < 16) per_ms = 16;
    if (per_ms > 0xffff) per_ms = 0xffff;
    uint32_t dbra = dbf_turns_per_ms();
    volatile uint8_t *t = win + PSMB_OFF_CALIB;
    const uint16_t addr[4] = { 0xd00, 0xd02, 0xb24, 0xcea };
    const uint16_t val[4]  = { (uint16_t)dbra, (uint16_t)per_ms, (uint16_t)per_ms, (uint16_t)per_ms };
    for (int i = 0; i < 4; i++) { wr16(t + 4 * i, addr[i]); wr16(t + 4 * i + 2, val[i]); }
    kprintf("[PSMAC:MBOX] calibration table: TimeDBRA %u (dbf turns per ms, measured), bus loops %u per ms (%u us per access)\n", dbra, per_ms, bus_latency_us);
}

void psmac_mbox_heartbeat(void)
{
    if (!win) return;
    wr32(win + 20, (uint32_t)((cnt_now() - t0) * 1000 / cnt_frq()));
    kprintf("[PSMAC:MBOX] %u commands (%u errors), req/done seq info %u/%u block %u/%u net %u/%u, net rx head %u tail %u\n", stats_cmds, stats_err,
            rd32(win + PSMB_OFF_CMD), rd32(win + PSMB_OFF_CMD + 4), rd32(win + PSMB_OFF_CMD + 64), rd32(win + PSMB_OFF_CMD + 68),
            rd32(win + PSMB_OFF_CMD + 128), rd32(win + PSMB_OFF_CMD + 132), rd32(win + PSMB_OFF_NETRX), rd32(win + PSMB_OFF_NETRX + 4));
}
