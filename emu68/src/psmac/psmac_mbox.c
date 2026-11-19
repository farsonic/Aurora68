/*
 * psmac_mbox.c — Pi-side mailbox services for the Macintosh (PiStorm-SE fork of Emu68). MPL-2.0.
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
static uint8_t *mac_page(uint32_t logical)
{
    int valid; uint32_t ph = psmac_mmu_l2p(logical, &valid);
    if (!valid) return NULL;
    if (ph < ram_size) return (uint8_t *)(KERNEL_ALIAS + ram_phys + ph);
    if (ph >= PSMB_WIN_PHYS && ph < PSMB_WIN_PHYS + PSMB_WIN_SIZE) return win + (ph - PSMB_WIN_PHYS);
    /* the 24-bit alias of the window */
    return NULL;
}
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

/* ---- block devices ---- */
struct blkdev { int64_t fd; uint32_t blocks; int writable; int ram; uint8_t *rambase; char name[48]; };
static struct blkdev bdev[PSMB_MAX_BLKDEV]; static int nbdev;
static uint8_t sector_buf[64 * 1024] __attribute__((aligned(16)));

static void blk_publish(void)
{
    for (int i = 0; i < PSMB_MAX_BLKDEV; i++) {
        volatile uint8_t *d = win + PSMB_OFF_BLKDEV + i * 64;
        wr32(d + 0, i < nbdev ? bdev[i].blocks : 0);
        wr32(d + 4, 512);
        wr32(d + 8, i < nbdev ? (PSMB_BD_PRESENT | (bdev[i].writable ? PSMB_BD_WRITABLE : 0)) : 0);
        wr32(d + 12, 0);
        for (int k = 0; k < 48; k++) ((volatile uint8_t *)d)[16 + k] = i < nbdev ? bdev[i].name[k] : 0;
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
    b->fd = fd; b->blocks = (uint32_t)(len / 512); b->writable = writable; b->ram = 0;
    const char *base = path; for (const char *s = path; *s; s++) if (*s == '/') base = s + 1;
    int k = 0; while (base[k] && k < 47) { b->name[k] = base[k]; k++; } b->name[k] = 0;
    kprintf("[PSMAC:MBOX] block%d: %s, %u blocks (%u MiB)%s\n", nbdev, path, b->blocks, b->blocks / 2048, writable ? "" : " read-only");
    nbdev++;
}
/* An image with an Apple partition map (a driver descriptor 'ER' in block 0, 'PM' entries from block
   1) is served as its first Apple_HFS partition, which is what the File Manager mounts; a bare HFS
   image as it is. PiDisk is one volume per device. */
static void hfs_partition(uint8_t **data, uint32_t *blocks)
{
    const uint8_t *d = *data;
    { extern void psmac_card_need(const void *, uint32_t); psmac_card_need(d, (*blocks < 64 ? *blocks : 64) * 512u); }   /* an image still coming in from the card: its map first */
    if (*blocks < 3 || d[0] != 'E' || d[1] != 'R' || d[512] != 'P' || d[513] != 'M') return;
    uint32_t n = (uint32_t)d[516] << 24 | d[517] << 16 | d[518] << 8 | d[519];
    for (uint32_t i = 1; i <= n && i < 64 && i < *blocks; i++) {
        const uint8_t *e = d + 512 * i;
        if (e[0] != 'P' || e[1] != 'M') break;
        uint32_t start = (uint32_t)e[8] << 24 | e[9] << 16 | e[10] << 8 | e[11], count = (uint32_t)e[12] << 24 | e[13] << 16 | e[14] << 8 | e[15];
        const char *type = (const char *)e + 48, *want = "Apple_HFS";
        int k = 0; while (want[k] && type[k] == want[k]) k++;
        if (want[k] || !count || start + count > *blocks) continue;
        *data += (uint64_t)start * 512; *blocks = count;
        kprintf("[PSMAC:MBOX]   its Apple_HFS partition: blocks %u-%u\n", start, start + count - 1);
        return;
    }
}
static void blk_add_ram(const char *name, uint8_t *data, uint32_t blocks)
{
    if (nbdev >= PSMB_MAX_BLKDEV) return;
    hfs_partition(&data, &blocks);
    struct blkdev *b = &bdev[nbdev];
    b->fd = -1; b->blocks = blocks; b->writable = 1; b->ram = 1; b->rambase = data;
    int k = 0; while (name[k] && k < 47) { b->name[k] = name[k]; k++; } b->name[k] = 0;
    nbdev++;
}
/* a disk image found on the card after the 68k started (psmac_card.c, core 3: this core) */
void psmac_mbox_add_disk(const char *name, uint8_t *data, uint32_t blocks)
{
    blk_add_ram(name, data, blocks);
    blk_publish();
    kprintf("[PSMAC:MBOX] block%d: %s, %u blocks (%u MiB), from the card\n", nbdev - 1, name, blocks, blocks / 2048);
}
static int blk_xfer(int write, struct blkdev *b, uint32_t lba, uint32_t count, uint32_t buf)
{
    if ((uint64_t)lba + count > b->blocks) return PSMB_E_RANGE;
    if (write && !b->writable) return PSMB_E_RO;
    if (b->ram) { extern void psmac_card_need(const void *, uint32_t); psmac_card_need(b->rambase + (uint64_t)lba * 512, count * 512); }   /* still coming in from the card? */
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
            if (psmac_sh_seek(b->fd, (uint64_t)lba * 512) < 0) return PSMB_E_IO;
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

/* ---- services ---- */
static void svc_set(int idx, uint16_t type, uint32_t caps, const uint32_t *param)
{
    volatile uint8_t *s = win + PSMB_OFF_SERVICES + idx * 32;
    wr16(s, type); wr16(s + 2, idx); wr32(s + 4, PSMB_OFF_CMD + idx * 64); wr32(s + 8, caps);
    for (int k = 0; k < 5; k++) wr32(s + 12 + 4 * k, param ? param[k] : 0);
}
static uint32_t stats_cmds, stats_err;

static void run_cmd(int idx, uint16_t type, volatile uint8_t *c)
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
        else status = PSMB_E_BADCMD;
        break;
    case PSMB_SVC_BLOCK: {
        if (cmd == PSMB_BLK_INFO) { blk_publish(); result = nbdev; break; }
        if (a0 >= (uint32_t)nbdev) { status = PSMB_E_BADDEV; break; }
        struct blkdev *b = &bdev[a0];
        if (cmd == PSMB_BLK_READ)       { static int first = 1; if (first) { first = 0; psmac_boot_mark("PiDisk: first block read (device %u, block %u)", a0, a1); }
                                          status = blk_xfer(0, b, a1, a2, buf); if (!status) { xfer = a2 * 512; extern uint32_t psmac_icache_epoch; psmac_icache_epoch++; } }
        else if (cmd == PSMB_BLK_WRITE) { status = blk_xfer(1, b, a1, a2, buf); if (!status) xfer = a2 * 512; }
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
    kprintf("[PSMAC:MBOX] window at Mac 0x%08x (Pi %p): %d block device(s), net %s\n", PSMB_WIN_PHYS, (void *)win_phys, nbdev, net_up ? "up" : "off");
}

/* called from the service core loop; returns 1 if work was done */
int psmac_mbox_poll(void)
{
    if (!win) return 0;
    int did = 0;
    for (int i = 0; i < 3; i++) {
        volatile uint8_t *c = win + PSMB_OFF_CMD + i * 64;
        uint32_t req = __atomic_load_n((volatile uint32_t *)c, __ATOMIC_ACQUIRE);
        uint32_t done = *(volatile uint32_t *)(c + 4);
        if (req == done) continue;
        uint16_t type = rd16(win + PSMB_OFF_SERVICES + i * 32);
        run_cmd(i, type, c);
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
