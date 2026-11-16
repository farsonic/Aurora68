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
static int blk_xfer(int write, struct blkdev *b, uint32_t lba, uint32_t count, uint32_t buf)
{
    if ((uint64_t)lba + count > b->blocks) return PSMB_E_RANGE;
    if (write && !b->writable) return PSMB_E_RO;
    while (count) {
        uint32_t n = count > sizeof(sector_buf) / 512 ? sizeof(sector_buf) / 512 : count;
        if (b->ram) {
            if (write) { int r = mac_copy(buf, NULL, b->rambase + (uint64_t)lba * 512, n * 512, 0); if (r) return r; }
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
   tx: we append {u16 len, frame}; rx: the helper appends the same records, we read from our offset. */
static int64_t net_tx_fd = -1, net_rx_fd = -1; static uint64_t net_rx_off; static int net_up;
static uint8_t net_mac[6] = { 0x02, 0x50, 0x53, 0x4D, 0x41, 0x43 };   /* locally administered "PSMAC" */
static uint8_t net_buf[PSMB_NETRX_FRAME + 2];
static uint32_t net_filter;
static int mac_eq(const uint8_t *a, const uint8_t *b) { for (int i = 0; i < 6; i++) if (a[i] != b[i]) return 0; return 1; }

static void net_init(void)
{
    const char *base = psmac_option("mac.net");
    if (!base || !psmac_sh_available()) return;
    char tx[128], rx[128]; int n = 0; while (base[n] && n < 100 && base[n] != ' ') { tx[n] = rx[n] = base[n]; n++; }
    memcpy(tx + n, ".tx", 4); memcpy(rx + n, ".rx", 4);
    net_tx_fd = psmac_sh_open(tx, 9 /* ab */);
    net_rx_fd = psmac_sh_open(rx, 1 /* rb */);
    net_up = net_tx_fd >= 0 && net_rx_fd >= 0;
    if (net_up) net_rx_off = (uint64_t)psmac_sh_flen(net_rx_fd);   /* start from the current end */
    kprintf("[PSMAC:MBOX] net: %s (%s.tx/.rx) %s\n", base, base, net_up ? "up" : "unavailable");
}
static void net_poll_rx(void)
{
    if (!net_up) return;
    volatile uint8_t *ring = win + PSMB_OFF_NETRX;
    for (int budget = 0; budget < 8; budget++) {
        int64_t len = psmac_sh_flen(net_rx_fd);
        if (len < 0 || (uint64_t)len < net_rx_off + 2) return;
        uint32_t head = rd32(ring), tail = rd32(ring + 4);
        uint32_t next = (head + 1) % PSMB_NETRX_SLOTS;
        if (next == tail) { return; }                 /* ring full: leave the frame in the file */
        if (psmac_sh_seek(net_rx_fd, net_rx_off) < 0) return;
        uint8_t hdr[2]; if (!psmac_sh_read(net_rx_fd, hdr, 2)) return;
        uint32_t flen = (hdr[0] << 8) | hdr[1];
        if (flen > PSMB_NETRX_FRAME || (uint64_t)len < net_rx_off + 2 + flen) return;
        if (!psmac_sh_read(net_rx_fd, net_buf, flen)) return;
        net_rx_off += 2 + flen;
        /* destination filter: ours, broadcast, multicast (if enabled) or promiscuous */
        int mine = (net_filter & PSMB_NETF_PROMISC) || mac_eq(net_buf, net_mac) ||
                   (net_buf[0] == 0xff && net_buf[1] == 0xff && net_buf[2] == 0xff && net_buf[3] == 0xff && net_buf[4] == 0xff && net_buf[5] == 0xff) ||
                   ((net_buf[0] & 1) && (net_filter & PSMB_NETF_MULTICAST));
        if (!mine) continue;
        volatile uint8_t *slot = ring + 16 + head * (8 + PSMB_NETRX_FRAME);
        memcpy((void *)(slot + 8), net_buf, flen);
        wr32(slot, flen);
        __asm__ volatile("dmb ish" ::: "memory");
        wr32(ring, next);
    }
}
/* primitives for alice_psmac.c's testbed Ethernet (the same host file pair) */
int psmac_netfile_up(void) { return net_up; }
int psmac_netfile_send(const uint8_t *frame, uint32_t len)
{
    if (!net_up || len > PSMB_NETRX_FRAME) return 0;
    uint8_t hdr[2] = { len >> 8, len & 0xff };
    return psmac_sh_write(net_tx_fd, hdr, 2) && psmac_sh_write(net_tx_fd, frame, len);
}
int psmac_netfile_recv(uint8_t *buf, uint32_t max)
{
    if (!net_up) return 0;
    int64_t len = psmac_sh_flen(net_rx_fd);
    if (len < 0 || (uint64_t)len < net_rx_off + 2) return 0;
    if (psmac_sh_seek(net_rx_fd, net_rx_off) < 0) return 0;
    uint8_t hdr[2]; if (!psmac_sh_read(net_rx_fd, hdr, 2)) return 0;
    uint32_t flen = (hdr[0] << 8) | hdr[1];
    if (flen > max || (uint64_t)len < net_rx_off + 2 + flen) { if (flen > max) net_rx_off += 2 + flen; return 0; }
    if (!psmac_sh_read(net_rx_fd, buf, flen)) return 0;
    net_rx_off += 2 + flen;
    return (int)flen;
}
static int net_send(uint32_t buf, uint32_t len)
{
    if (!net_up) return PSMB_E_IO;
    if (len < 14 || len > PSMB_NETRX_FRAME) return PSMB_E_RANGE;
    int r = mac_copy(buf, net_buf + 2, NULL, len, 0); if (r) return r;
    net_buf[0] = len >> 8; net_buf[1] = len & 0xff;
    return psmac_sh_write(net_tx_fd, net_buf, len + 2) ? 0 : PSMB_E_IO;
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
            if (mac_copy(buf, tmp, NULL, n, 0) == 0) { tmp[n] = 0; kprintf("[MAC] %s\n", tmp); xfer = n; } else status = PSMB_E_BADADDR;
        } else if (cmd == PSMB_INFO_TIME) result = 0;
        else status = PSMB_E_BADCMD;
        break;
    case PSMB_SVC_BLOCK: {
        if (cmd == PSMB_BLK_INFO) { blk_publish(); result = nbdev; break; }
        if (a0 >= (uint32_t)nbdev) { status = PSMB_E_BADDEV; break; }
        struct blkdev *b = &bdev[a0];
        if (cmd == PSMB_BLK_READ)       { status = blk_xfer(0, b, a1, a2, buf); if (!status) { xfer = a2 * 512; extern uint32_t psmac_icache_epoch; psmac_icache_epoch++; } }
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
    blk_publish();
    net_init();

    /* the Alice68 boards inside the window (alice_psmac.h): network board, status page, input ring */
    { extern void psmac_alice_init(uint8_t *win); psmac_alice_init(win); }
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
    const char *ver = "psmac " __DATE__; int k = 0; while (ver[k] && k < 31) { ((volatile uint8_t *)h)[24 + k] = ver[k]; k++; }
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

void psmac_mbox_heartbeat(void)
{
    if (!win) return;
    wr32(win + 20, (uint32_t)((cnt_now() - t0) * 1000 / cnt_frq()));
    kprintf("[PSMAC:MBOX] %u commands (%u errors), req/done seq info %u/%u block %u/%u net %u/%u, net rx head %u tail %u\n", stats_cmds, stats_err,
            rd32(win + PSMB_OFF_CMD), rd32(win + PSMB_OFF_CMD + 4), rd32(win + PSMB_OFF_CMD + 64), rd32(win + PSMB_OFF_CMD + 68),
            rd32(win + PSMB_OFF_CMD + 128), rd32(win + PSMB_OFF_CMD + 132), rd32(win + PSMB_OFF_NETRX), rd32(win + PSMB_OFF_NETRX + 4));
}
