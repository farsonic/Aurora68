/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_usbnet.c - the wired Ethernet of a Pi 3B and 3B+: the network chip on the board's USB bus,
 * driven over the DWC2 host that psmac_usb.c already runs for keyboards and mice.
 *
 * Backstory. The Mac's wired card (sResource $81 of slot 9, the .ENET driver .Network_EtherNet_PiStorm)
 * talks to unit 1 of the Alice68 network board (alice_net.h). On a Pi 4 that unit is the GENET,
 * which Alice68's alice_net.c drives. A Pi 3 has no GENET: its RJ45 socket hangs off the USB bus
 * (docs/08 item 49):
 *   Pi 3B (and 2B v1.2)   root port -> LAN9514 hub (address 1), its port 1 -> the LAN9514's Ethernet
 *                         function, the same register set as a LAN9512 (USB 0424:ec00, "smsc95xx")
 *   Pi 3B+                root port -> LAN7515 hub, its port 1 -> a second hub, its port 1 -> the
 *                         LAN7800 Ethernet function (0424:7800, "lan78xx")
 *   Pi 3A+, Zero 2 W      no wired Ethernet at all (WiFi only)
 * The kernel learnt in November 2026 that a dead card confuses everybody (e5ff17d hid $81 on a
 * Pi 3); this file makes the card live instead, so the Mac gets both its network cards.
 *
 * What it does. When psmac_usb.c has enumerated the bus (Alice68's HID host gives the hub address
 * 1 and the device on hub port N address N+1, and leaves anything that is not a keyboard, mouse or
 * hub unconfigured), this file finds the network chip, configures it, and moves frames between its
 * bulk endpoints and unit 1's rings, exactly where alice_net.c's GENET code would. The Mac's
 * drivers, the board protocol and its offsets do not change. It also drives a USB
 * Communications-Class Ethernet device (CDC-ECM), which is what QEMU's usb-net is: the testbed has no
 * LAN9514, so that is how the bulk path, the chip-independent half of this file, is exercised there.
 *
 * Sharing the bus with the keyboard and the mouse:
 *   - Channel 0 stays psmac_usb.c's (interrupt polls of the HID devices, every control request);
 *     this file's register accesses use it too, through the same frf_dwc2_pio_packet, from the same
 *     core 3 loop, so they never overlap. Bulk data goes on channels 1 (in) and 2 (out), which run
 *     by DMA while the loop does other things: started on one turn, looked at on a later one.
 *   - The chip answers an empty bulk-in with a zero-length packet (HW_CFG/USB_CFG0's BIR bit
 *     clear), so a receive transfer always ends by itself: nothing sits on the bus waiting, and
 *     the interrupt endpoints' periodic slots come first anyway (USB 2.0 section 5.4).
 *   - Every wait (resets, the PHY's autonegotiation) is a state with a deadline, not a loop: a poll
 *     does at most a handful of control transfers (well under a millisecond each), so neither the
 *     HID polls nor the mailbox services next to it (the Mac's disks) are held up.
 *   - A fresh HID enumeration (a keyboard plugged in) resets the root port, and with it the
 *     network chip: psmac_usb.c tells this file first (psmac_usbnet_bus_reset), the bulk channels
 *     stop, and the chip is found and set up again afterwards; the Mac sees the link go down and
 *     come back.
 * The FIFO transport (usb=pio) has no bulk path; the wired port then says so and stays down.
 *
 * Which board gets what (psmac_board_wired, also used by psmac_bus.c to decide whether the Mac sees
 * the $81 card at all) comes from the device tree the Pi's firmware passed: the ethernet0 alias's
 * "compatible" (brcm,genet-v5 on a Pi 4; usb424,ec00 on a 3B; usb424,7800 on a 3B+), so it is known
 * before the 68k starts, long before the USB bus is enumerated.
 *
 * Kernel log lines for the bench ([ETH], also in the network board's log that PiLog shows):
 *   [ETH] wired port: LAN9514 (smsc95xx) on the USB bus, the Pi 3B's ...    at boot, from the device tree
 *   [ETH] LAN9514 found at USB address 2 (0424:ec00), chip id ec00 rev 0002
 *   [ETH] MAC b8:27:eb:xx:xx:xx (the Pi's own)
 *   [ETH] link up, 100 Mbit/s full duplex   /   [ETH] no link: is the cable in?
 *   [ETH]   LAN9514: link 100 full, rx 1234 frames (56789 bytes) tx ..., errors ...   every 15 s
 */
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "support.h"
#include "mmu.h"
#include "devicetree.h"
#include "../alice/alice_psmac.h"
#include "../alice/alice_net.h"
#include "../alice/usb/frf_dwc2_pio.h"
#include "usbnet_frame.h"
#include "psmac_ip.h"

const char *psmac_option(const char *key);
int alice_mbox2(uint32_t tag, uint32_t a, uint32_t b, uint32_t out[2]);
void alice_net_ring(const char *s, unsigned n);
/* psmac_usb.c: 0 while the bus is not up, else a count that moves on with every enumeration;
   with the root hub's port count, the root port's speed (0 high) and whether the DMA transport runs */
uint32_t psmac_usb_bus(unsigned *hub_ports, int *root_speed, int *dma);
const struct frf_dwc2_ops *psmac_usb_ops(void);
void psmac_netprobe_tx(int unit, const volatile uint8_t *f, uint32_t len, int stage);
void psmac_netprobe_rx(int unit, const volatile uint8_t *f, uint32_t len, uint32_t slot, volatile uint32_t *rx_tail);

static inline uint64_t now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
static uint32_t now_ms(void) { return (uint32_t)(now() / (hz() / 1000u)); }

/* ---- the board table --------------------------------------------------------------------------- */
enum { WIRED_NONE = 0, WIRED_GENET, WIRED_SMSC95XX, WIRED_LAN78XX };
static int dt_has(of_property_t *p, const char *what)
{
    unsigned n = (unsigned)strlen(what);
    const char *c = p ? (const char *)p->op_value : 0;
    for (uint32_t i = 0; c && i + n <= p->op_length; i++)
        if (!memcmp(c + i, what, n))
            return 1;
    return 0;
}
/* the Pi's wired Ethernet, from the device tree: ethernet0's compatible, else the board's model */
int psmac_board_wired(void)
{
    static int cached = -1;
    if (cached >= 0)
        return cached;
    of_node_t *al = dt_find_node("/aliases");
    of_property_t *ap = al ? dt_find_property(al, "ethernet0") : 0;
    of_node_t *eth = (ap && ap->op_value) ? dt_find_node((char *)ap->op_value) : 0;
    of_property_t *c = eth ? dt_find_property(eth, "compatible") : 0;
    of_node_t *root = dt_find_node("/");
    of_property_t *model = root ? dt_find_property(root, "compatible") : 0;
    if (dt_has(c, "genet"))
        cached = WIRED_GENET;
    else if (dt_has(c, "usb424,ec00") || dt_has(c, "usb424,9500") || dt_has(c, "usb424,9e00"))
        cached = WIRED_SMSC95XX;
    else if (dt_has(c, "usb424,7800"))
        cached = WIRED_LAN78XX;
    /* a device tree without the alias (an old one, or a hand-made one): by the board's name */
    else if (dt_has(model, "raspberrypi,3-model-b-plus"))
        cached = WIRED_LAN78XX;
    else if (dt_has(model, "raspberrypi,3-model-b") || dt_has(model, "raspberrypi,2-model-b"))
        cached = WIRED_SMSC95XX;
    else if (dt_has(model, "raspberrypi,4-model-b") || dt_has(model, "raspberrypi,400") || dt_has(model, "raspberrypi,4-compute-module"))
        cached = WIRED_GENET;
    else
        cached = WIRED_NONE;                    /* 3A+, Zero 2 W, CM3: WiFi only */
    return cached;
}
/* the Bluetooth chip's firmware family, for bt_pi.c: 1 BCM43430A1 (3B), 2 BCM43430B0 (Zero 2 W),
   3 BCM4345C5 (400, CM4), 0 BCM4345C0 (3B+, 3A+, 4B, and anything else) */
int psmac_board_bt_chip(void)
{
    of_node_t *root = dt_find_node("/");
    of_property_t *model = root ? dt_find_property(root, "compatible") : 0;
    if (dt_has(model, "3-model-b-plus") || dt_has(model, "3-model-a-plus"))
        return 0;
    if (dt_has(model, "raspberrypi,3-model-b"))
        return 1;
    if (dt_has(model, "model-zero-2-w"))
        return 2;
    if (dt_has(model, "raspberrypi,400") || dt_has(model, "4-compute-module"))
        return 3;
    return 0;
}
const char *psmac_board_wired_name(void)
{
    static const char *const n[] = { "none (WiFi only)", "GENET (the Pi 4's own)", "LAN9514 (smsc95xx) on the USB bus", "LAN7800 (lan78xx) on the USB bus" };
    return n[psmac_board_wired()];
}

/* ---- logging: the kernel log and the network board's ring (PiLog) ---------------------------------- */
struct lb { char s[160]; unsigned n; };
static void lb_c(void *d, char c) { struct lb *b = d; if (b->n < sizeof b->s - 2) b->s[b->n++] = c; }
static void elog(const char *fmt, ...)
{
    struct lb b;
    va_list ap;
    b.n = 0;
    for (const char *p = "[ETH] "; *p; p++)
        b.s[b.n++] = *p;
    va_start(ap, fmt);
    vkprintf_pc(lb_c, &b, fmt, ap);
    va_end(ap);
    if (!b.n || b.s[b.n - 1] != '\n')
        b.s[b.n++] = '\n';
    b.s[b.n] = 0;
    kprintf("%s", b.s);
    alice_net_ring(b.s, b.n);
}

/* ---- the DWC2's bulk channels (1: in, 2: out), by DMA, started and looked at separately ---------- */
#define DWC2 (ALICE_PERI + 0x980000ull)
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
enum { CH_IN = 1, CH_OUT = 2 };
#define RXBUF 16384u                            /* one transfer: a frame, or a burst of them */
static uint8_t rxbuf[RXBUF] __attribute__((aligned(64)));
static uint8_t txbuf[2048] __attribute__((aligned(64)));

struct chan { int busy; uint32_t len, started_ms; uint8_t pid; };
static struct chan ch[3];

static uint32_t bus_addr(void *p) { return (uint32_t)mmu_virt2phys((uintptr_t)p) | 0xC0000000u; }   /* the VideoCore's uncached alias */

static void ch_halt(int c)
{
    if (rd(HCCHAR(c)) & B(31)) {
        wr(HCCHAR(c), rd(HCCHAR(c)) | B(30) | B(31));
        for (int i = 0; i < 1000 && (rd(HCCHAR(c)) & B(31)); i++) {
            uint64_t t = now() + hz() / 100000;
            while (now() < t)
                ;
        }
    }
    wr(HCINT(c), 0xffffffffu);
    ch[c].busy = 0;
}

static struct dev {
    enum usbnet_chip chip;
    uint8_t addr, mps0, ep_in, ep_out, cfg, ctl_itf, data_itf, data_alt;
    uint16_t mps, vid, pid;
    uint32_t chip_id;
} D;

static void ch_start(int c, void *buf, uint32_t len)
{
    int in = c == CH_IN;
    uint32_t pkts = len ? (len + D.mps - 1) / D.mps : 1;
    if (in)
        arm_flush_cache((uintptr_t)buf, len);           /* nothing of ours may be written back over the DMA's bytes */
    else
        arm_flush_cache((uintptr_t)buf, len ? len : 4);
    wr(HCINT(c), 0xffffffffu);
    wr(HCINTMSK(c), 0x7ffu);
    wr(HCSPLT(c), 0);
    wr(HCTSIZ(c), len | pkts << 19 | (uint32_t)ch[c].pid << 29);
    wr(HCDMA(c), bus_addr(buf));
    wr(HCCHAR(c), (uint32_t)D.addr << 22 | 1u << 20 | 2u << 18 | (uint32_t)in << 15 |
                  (uint32_t)(in ? D.ep_in : D.ep_out) << 11 | D.mps | B(31));
    ch[c].busy = 1;
    ch[c].len = len;
    ch[c].started_ms = now_ms();
}
/* a channel's outcome: 1 still running, 0 done (*got: bytes in), -1 no data (NAK), -2 an error */
static int ch_check(int c, uint32_t *got)
{
    uint32_t s = rd(HCINT(c));
    if (!(s & B(1)))
        return 1;
    uint32_t t = rd(HCTSIZ(c));
    ch[c].busy = 0;
    ch[c].pid = (uint8_t)((t >> 29) & 3u);              /* the toggle the core expects next */
    wr(HCINT(c), 0xffffffffu);
    if (s & B(0)) {
        uint32_t left = t & 0x7ffffu;
        *got = ch[c].len > left ? ch[c].len - left : 0;
        if (c == CH_IN)
            arm_dcache_invalidate((uintptr_t)rxbuf, (*got + 63u) & ~63u);
        return 0;
    }
    if (s & B(4))
        return -1;
    if (s & B(3))
        ch[c].pid = 0;                                  /* a stall resets the endpoint's toggle */
    return -2;
}

/* ---- control transfers on channel 0, through psmac_usb.c's transport ------------------------------ */
static int ctl(uint8_t addr, uint8_t mps0, const uint8_t setup[8], uint8_t *data, uint16_t len)
{
    const struct frf_dwc2_ops *o = psmac_usb_ops();
    uint8_t s[8];
    int in = (setup[0] & 0x80u) != 0, rc = 0;
    uint16_t done = 0;
    uint8_t pid = FRF_USB_PID_DATA1;
    memcpy(s, setup, 8);
    struct frf_dwc2_packet p = { addr, 0, 0, 0, 0, FRF_USB_PID_SETUP, mps0, 8, 0, s, 0, 0 };
    for (int k = 0; k < 4 && (rc = frf_dwc2_pio_packet(o, &p, 0)) == -5; k++)
        ;
    if (rc)
        return rc;
    while (done < len) {
        uint16_t n = len - done > mps0 ? mps0 : len - done;
        p = (struct frf_dwc2_packet){ addr, 0, (uint8_t)in, 0, 0, pid, mps0, n, 0, data + done, 0, 0 };
        for (int k = 0; k < 20 && (rc = frf_dwc2_pio_packet(o, &p, 0)) == -5; k++)
            ;
        if (rc)
            return rc;
        done += in ? p.received : n;
        if (in && p.received < n)
            break;
        pid = pid == FRF_USB_PID_DATA1 ? FRF_USB_PID_DATA0 : FRF_USB_PID_DATA1;
    }
    p = (struct frf_dwc2_packet){ addr, 0, (uint8_t)!in, 0, 0, FRF_USB_PID_DATA1, mps0, 0, 0, 0, 0, 0 };
    for (int k = 0; k < 20 && (rc = frf_dwc2_pio_packet(o, &p, 0)) == -5; k++)
        ;
    return rc ? rc : (int)done;
}
static int std_req(uint8_t addr, uint8_t bm, uint8_t req, uint16_t value, uint16_t index, uint8_t *data, uint16_t len)
{
    uint8_t s[8] = { bm, req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)len, (uint8_t)(len >> 8) };
    return ctl(addr, D.mps0 ? D.mps0 : 64, s, data, len);
}
/* the chips' vendor requests: 0xA1 reads a 32-bit register, 0xA0 writes one (both little-endian) */
static uint32_t reg_errors;
static int reg_rd(uint16_t reg, uint32_t *v)
{
    uint8_t b[4] = { 0 };
    int rc = std_req(D.addr, 0xc0, 0xa1, 0, reg, b, 4);
    if (rc != 4) { reg_errors++; return -1; }
    *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 0;
}
static int reg_wr(uint16_t reg, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    if (std_req(D.addr, 0x40, 0xa0, 0, reg, b, 4) != 4) { reg_errors++; return -1; }
    return 0;
}

/* ---- the chips' registers (Microchip LAN9512/9514 and LAN7800 datasheets) ---------------------------- */
/* LAN9514 (smsc95xx) */
#define S_ID_REV      0x000
#define S_INT_STS     0x008
#define S_TX_CFG      0x010
#define S_HW_CFG      0x014
#define S_PM_CTRL     0x020
#define S_LED_GPIO    0x024
#define S_AFC_CFG     0x02c
#define S_BURST_CAP   0x038
#define S_BULK_IN_DLY 0x06c
#define S_MAC_CR      0x100
#define S_ADDRH       0x104
#define S_ADDRL       0x108
#define S_HASHH       0x10c
#define S_HASHL       0x110
#define S_MII_ADDR    0x114
#define S_MII_DATA    0x118
#define S_FLOW        0x11c
#define S_COE_CR      0x130
#define S_HW_LRST     B(3)
#define S_HW_BCE      B(1)
#define S_HW_MEF      B(5)
#define S_HW_BIR      B(12)
#define S_HW_RXDOFF   (3u << 9)
#define S_PM_PHY_RST  B(4)
#define S_MAC_RXEN    B(2)
#define S_MAC_TXEN    B(3)
#define S_MAC_PRMS    B(18)
#define S_MAC_MCPAS   B(19)
#define S_MAC_HPFILT  B(13)
#define S_MAC_FDPX    B(20)
/* LAN7800 (lan78xx) */
#define L_ID_REV      0x000
#define L_INT_STS     0x00c
#define L_HW_CFG      0x010
#define L_PMT_CTL     0x014
#define L_DP_SEL      0x024
#define L_DP_CMD      0x028
#define L_DP_ADDR     0x02c
#define L_DP_DATA     0x030
#define L_USB_CFG0    0x080
#define L_BURST_CAP   0x090
#define L_BULK_IN_DLY 0x094
#define L_RFE_CTL     0x0b0
#define L_FCT_RX_CTL  0x0c0
#define L_FCT_TX_CTL  0x0c4
#define L_FCT_RX_END  0x0c8
#define L_FCT_TX_END  0x0cc
#define L_FCT_FLOW    0x0d0
#define L_MAC_CR      0x100
#define L_MAC_RX      0x104
#define L_MAC_TX      0x108
#define L_FLOW        0x10c
#define L_RX_ADDRH    0x118
#define L_RX_ADDRL    0x11c
#define L_MII_ACC     0x120
#define L_MII_DATA    0x124
#define L_MAF_HI0     0x400
#define L_MAF_LO0     0x404
#define L_MAF_HI1     0x408               /* the perfect filter's second slot: the remote control's address (docs/13) */
#define L_MAF_LO1     0x40c
#define L_HW_LRST     B(1)
#define L_HW_MEF      B(4)
#define L_HW_CLKS     (B(25) | B(24))                   /* CLK125_EN, REFCLK25_EN, as the chip's reference design runs */
#define L_PMT_PHY_RST B(4)
#define L_PMT_READY   B(7)
#define L_USB_BIR     B(6)
#define L_USB_BCE     B(5)
#define L_RFE_BCAST   B(10)
#define L_RFE_MCAST   B(9)
#define L_RFE_UCAST   B(8)
#define L_RFE_MHASH   B(3)
#define L_RFE_PERFECT B(1)
#define L_MAC_AUTO    (B(12) | B(11))                   /* AUTO_DUPLEX, AUTO_SPEED: the MAC follows the PHY */
#define L_EN          B(31)
#define PHY_ADDR      1                                 /* both chips' internal PHY */

/* ---- unit 1 ------------------------------------------------------------------------------------- */
enum { S_IDLE, S_PROBE, S_INIT, S_PHY, S_RUN, S_ABSENT, S_FAILED };
static struct {
    int owner;                                          /* -1 not decided, 0 not ours, 1 ours */
    int state, step, link, speed, full, promisc, allmulti, dma, ready;
    uint32_t gen, t_step, t_next, t_probe, mii_phase, bmsr_last;
    uint8_t mac[6], fmac[6];
    uint32_t hash[16];
    uint32_t rx_frames, rx_bytes, tx_frames, tx_bytes, rx_bad, rx_held, tx_err, bulk_err, naks, inits;
    unsigned hub_ports;
    int root_speed;
} N;

static const char *chip_name(void)
{
    switch (D.chip) {
    case USBNET_SMSC95XX: return D.chip_id == 0xec00 ? "LAN9514" : "LAN95xx";
    case USBNET_LAN78XX:  return "LAN7800";
    case USBNET_CDC_ECM:  return "CDC-ECM";
    default:              return "USB Ethernet";
    }
}

static volatile struct an_hdr *hdr_of(volatile struct an_unit *u) { return (volatile struct an_hdr *)((volatile uint8_t *)u - AN_OFF_UNIT(1)); }
static void raise_irq(volatile struct an_unit *u) { __asm__ volatile("dmb ish" ::: "memory"); hdr_of(u)->irq = 1; }
static void ustate(volatile struct an_unit *u, uint32_t st, const char *msg)
{
    unsigned i = 0;
    for (; msg[i] && i < 95; i++)
        u->msg[i] = msg[i];
    u->msg[i] = 0;
    u->state = st;
    __asm__ volatile("dmb ish" ::: "memory");
    u->state_seq++;
    raise_irq(u);
    elog("%s", msg);
}
static void link_down(volatile struct an_unit *u, const char *why)
{
    if (N.link) {
        u->link_downs++;
        N.link = 0;
    }
    u->link = 0;
    u->speed_mbps = 0;
    ustate(u, AN_ST_IDLE, why);
}

/* the Pi's own address: the firmware's (mailbox), else the device tree's, else a made-up local one */
static void learn_mac(volatile struct an_unit *u)
{
    uint32_t w[2] = { 0, 0 };
    if (alice_mbox2(0x00010003u, 0, 0, w) == 0) {       /* GET_BOARD_MAC_ADDRESS */
        N.fmac[0] = (uint8_t)w[0]; N.fmac[1] = (uint8_t)(w[0] >> 8); N.fmac[2] = (uint8_t)(w[0] >> 16);
        N.fmac[3] = (uint8_t)(w[0] >> 24); N.fmac[4] = (uint8_t)w[1]; N.fmac[5] = (uint8_t)(w[1] >> 8);
    }
    if (!(N.fmac[0] | N.fmac[1] | N.fmac[2] | N.fmac[3] | N.fmac[4] | N.fmac[5])) {
        of_node_t *al = dt_find_node("/aliases");
        of_property_t *ap = al ? dt_find_property(al, "ethernet0") : 0;
        of_node_t *e = (ap && ap->op_value) ? dt_find_node((char *)ap->op_value) : 0;
        of_property_t *m = e ? dt_find_property(e, "local-mac-address") : 0;
        if (m && m->op_length >= 6)
            memcpy(N.fmac, m->op_value, 6);
    }
    if (!(N.fmac[0] | N.fmac[1] | N.fmac[2] | N.fmac[3] | N.fmac[4] | N.fmac[5]) || (N.fmac[0] & 1)) {
        static const uint8_t local[6] = { 0x02, 0x50, 0x53, 0x55, 0x53, 0x42 };     /* "PSUSB", locally administered */
        memcpy(N.fmac, local, 6);
    }
    memcpy(N.mac, N.fmac, 6);
    for (int i = 0; i < 6; i++) {
        u->mac[i] = N.mac[i];
        u->factory_mac[i] = N.fmac[i];
    }
}

/* ---- finding the chip -------------------------------------------------------------------------- */
static int hub_status(uint8_t hub, uint8_t port, uint32_t *st)
{
    uint8_t b[4] = { 0 };
    uint8_t s[8] = { 0xa3, 0, 0, 0, port, 0, 4, 0 };
    if (ctl(hub, 64, s, b, 4) != 4)
        return -1;
    *st = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 0;
}
static int hub_feature(uint8_t hub, uint8_t req, uint16_t feature, uint8_t port)
{
    uint8_t s[8] = { 0x23, req, (uint8_t)feature, (uint8_t)(feature >> 8), port, 0, 0, 0 };
    return ctl(hub, 64, s, 0, 0) < 0 ? -1 : 0;
}
static void spin_ms(unsigned ms) { uint64_t t = now() + hz() * ms / 1000; while (now() < t) ; }

/* the device descriptor at addr (18 bytes); the endpoint-0 size is learnt from its first 8 */
static int dev_desc(uint8_t addr, uint8_t *d)
{
    uint8_t s[8] = { 0x80, 6, 0, 1, 0, 0, 18, 0 };
    memset(d, 0, 18);
    int rc = ctl(addr, 64, s, d, 18);
    if (rc >= 8 && d[1] == 1 && d[7] && d[7] < 64 && rc < 18)
        rc = ctl(addr, d[7], s, d, 18);                 /* a full-speed device with a smaller endpoint 0 */
    return rc >= 8 && d[1] == 1 ? rc : -1;
}

/* The 3B+'s Ethernet sits one hub further down than Alice68's enumeration goes: its second hub
   (address 2, configured by Alice68 as a hub) has the LAN7800 on port 1. Power, reset, address it.
   About 150 ms, once per enumeration of the bus. */
#define INNER_ADDR 20
static int enumerate_inner(uint8_t hub)
{
    uint8_t hd[9] = { 0 }, s[8] = { 0xa0, 6, 0, 0x29, 0, 0, 9, 0 }, d[18];
    uint32_t st = 0;
    if (ctl(hub, 64, s, hd, 9) != 9 || hd[1] != 0x29)
        return -1;
    hub_feature(hub, 3, 8, 1);                          /* SET_FEATURE(PORT_POWER), port 1 */
    spin_ms((unsigned)hd[5] * 2u + 20u);
    if (hub_status(hub, 1, &st) || !(st & 1u))
        return -2;                                      /* nothing on port 1 */
    hub_feature(hub, 3, 4, 1);                          /* SET_FEATURE(PORT_RESET) */
    for (int i = 0; i < 10; i++) {
        spin_ms(20);
        if (!hub_status(hub, 1, &st) && (st & 2u))
            break;
    }
    if (!(st & 2u))
        return -3;
    hub_feature(hub, 1, 20, 1);                         /* CLEAR_FEATURE(C_PORT_RESET) */
    if (!(st & (1u << 10)))
        return -4;                                      /* not high speed: no split transactions here */
    spin_ms(10);
    uint8_t s8[8] = { 0x80, 6, 0, 1, 0, 0, 8, 0 };
    if (ctl(0, 64, s8, d, 8) != 8 || d[1] != 1)
        return -5;
    uint8_t sa[8] = { 0, 5, INNER_ADDR, 0, 0, 0, 0, 0 };     /* SET_ADDRESS */
    if (ctl(0, d[7] ? d[7] : 64, sa, 0, 0) < 0)
        return -6;
    spin_ms(10);
    return INNER_ADDR;
}

/* a configuration's interfaces: the bulk pair, and for CDC-ECM the data interface's setting */
static int parse_config(const uint8_t *c, unsigned total, int ecm)
{
    uint8_t itf = 0, alt = 0, cls = 0, sub = 0, in = 0, out = 0;
    uint16_t mpsi = 0;
    int ecm_seen = 0;
    D.ep_in = D.ep_out = 0;
    for (unsigned p = 0; p + 2 <= total && c[p] >= 2 && p + c[p] <= total; p += c[p]) {
        if (c[p + 1] == 4 && c[p] >= 9) {
            itf = c[p + 2]; alt = c[p + 3]; cls = c[p + 5]; sub = c[p + 6];
            in = out = 0;
            if (cls == 2 && sub == 6) {
                ecm_seen = 1;
                D.ctl_itf = itf;
            }
        } else if (c[p + 1] == 5 && c[p] >= 7 && (c[p + 3] & 3u) == 2u) {
            if (c[p + 2] & 0x80u) { in = c[p + 2] & 15u; mpsi = (uint16_t)(c[p + 4] | c[p + 5] << 8); }
            else out = c[p + 2] & 15u;
            if (in && out && !D.ep_in && (!ecm || (ecm_seen && cls == 10))) {
                D.ep_in = in; D.ep_out = out; D.mps = mpsi & 0x7ffu;
                D.data_itf = itf; D.data_alt = alt;
            }
        }
    }
    if (ecm && !ecm_seen)
        return -1;
    return D.ep_in && D.ep_out && D.mps ? 0 : -1;
}

/* is the device at addr one of ours? fills D and configures it */
static int try_device(uint8_t addr)
{
    uint8_t d[18], c[512];
    if (dev_desc(addr, d) < 18)
        return -1;
    memset(&D, 0, sizeof D);
    D.addr = addr;
    D.mps0 = d[7];
    D.vid = (uint16_t)(d[8] | d[9] << 8);
    D.pid = (uint16_t)(d[10] | d[11] << 8);
    if (D.vid == 0x0424 && (D.pid == 0xec00 || D.pid == 0x9500 || D.pid == 0x9e00 || D.pid == 0xec01))
        D.chip = USBNET_SMSC95XX;
    else if (D.vid == 0x0424 && (D.pid == 0x7800 || D.pid == 0x7850))
        D.chip = USBNET_LAN78XX;
    for (unsigned k = 0; k < d[17] && k < 4; k++) {
        uint8_t s[8] = { 0x80, 6, (uint8_t)k, 2, 0, 0, 9, 0 };
        if (ctl(addr, D.mps0, s, c, 9) != 9 || c[1] != 2)
            continue;
        unsigned total = (unsigned)(c[2] | c[3] << 8);
        if (total > sizeof c)
            total = sizeof c;
        s[6] = (uint8_t)total; s[7] = (uint8_t)(total >> 8);
        if (ctl(addr, D.mps0, s, c, (uint16_t)total) != (int)total)
            continue;
        int ecm = D.chip == USBNET_NONE;
        if (parse_config(c, total, ecm))
            continue;
        if (ecm)
            D.chip = USBNET_CDC_ECM;
        D.cfg = c[5];
        break;
    }
    if (D.chip == USBNET_NONE || !D.cfg)
        return -1;
    if (std_req(addr, 0, 9, D.cfg, 0, 0, 0) < 0)              /* SET_CONFIGURATION */
        return -2;
    if (D.chip == USBNET_CDC_ECM && D.data_alt && std_req(addr, 1, 11, D.data_alt, D.data_itf, 0, 0) < 0)   /* SET_INTERFACE */
        return -3;
    return 0;
}

static int probe(void)
{
    uint8_t d[18];
    if (!N.hub_ports)
        return try_device(1);                           /* a device on the root port itself (QEMU without a hub) */
    for (unsigned port = 1; port <= N.hub_ports; port++) {
        uint8_t addr = (uint8_t)(1 + port);
        uint32_t st = 0;
        /* only a port with a device on it (QEMU's DWC2 model stops the whole emulator on a transfer
           to an address nobody has); behind a high-speed hub only high-speed devices, as the bulk
           path has no split transactions */
        if (hub_status(1, (uint8_t)port, &st) || !(st & 1u) || !(st & 2u))
            continue;
        if (N.root_speed == 0 && !(st & (1u << 10)))
            continue;
        if (try_device(addr) == 0)
            return 0;
        /* the 3B+: a hub on port 1 with the LAN7800 behind it */
        if (port == 1 && dev_desc(addr, d) >= 18 && d[4] == 9) {
            int a = enumerate_inner(addr);
            if (a > 0 && try_device((uint8_t)a) == 0)
                return 0;
            if (a <= 0)
                elog("the hub on port 1 (address %u): nothing usable on its port 1 (%d)", addr, a);
        }
    }
    return -1;
}

/* ---- setting the chip up: one step per poll, a deadline for every wait ------------------------------ */
static int mii_wait(uint16_t acc)
{
    uint32_t v = 1;
    for (int i = 0; i < 20 && (v & 1u); i++)
        if (reg_rd(acc, &v))
            return -1;
    return (v & 1u) ? -1 : 0;
}
static int mii_rd(int reg)
{
    uint16_t acc = D.chip == USBNET_LAN78XX ? L_MII_ACC : S_MII_ADDR, dat = acc + 4;
    uint32_t v;
    if (mii_wait(acc) || reg_wr(acc, (uint32_t)PHY_ADDR << 11 | (uint32_t)reg << 6 | 1u) || mii_wait(acc) || reg_rd(dat, &v))
        return -1;
    return (int)(v & 0xffffu);
}
static int mii_wr(int reg, uint16_t val)
{
    uint16_t acc = D.chip == USBNET_LAN78XX ? L_MII_ACC : S_MII_ADDR, dat = acc + 4;
    if (mii_wait(acc) || reg_wr(dat, val) || reg_wr(acc, (uint32_t)PHY_ADDR << 11 | (uint32_t)reg << 6 | 2u | 1u))
        return -1;
    return mii_wait(acc);
}

/* the receive filter as the Mac asked (AN_CMD_FILTER) */
static void apply_filter(void)
{
    int all = N.allmulti || N.promisc;
    for (int i = 0; i < 16; i++)
        N.hash[i] = all ? 0xffffffffu : 0;
    if (D.chip == USBNET_SMSC95XX) {
        uint32_t cr = 0;
        reg_rd(S_MAC_CR, &cr);
        cr &= ~(S_MAC_PRMS | S_MAC_MCPAS | S_MAC_HPFILT);
        /* one address register: while the remote control has an address of its own (psmac_ip.c)
           the chip takes every frame and the kernel sorts them (psmac_ip_rx) */
        cr |= (N.promisc || psmac_ip_promisc(1) ? S_MAC_PRMS : 0) | (N.allmulti ? S_MAC_MCPAS : 0) | S_MAC_HPFILT;
        reg_wr(S_HASHH, N.hash[1]);
        reg_wr(S_HASHL, N.hash[0]);
        reg_wr(S_MAC_CR, cr);
    } else if (D.chip == USBNET_LAN78XX) {
        /* the 512-bit table through the data port (VLAN/DA RAM, after the 128 VLAN words) */
        uint32_t v = 0;
        reg_rd(L_DP_SEL, &v);
        reg_wr(L_DP_SEL, (v & ~15u) | 1u);
        for (int i = 0; i < 16; i++) {
            reg_wr(L_DP_ADDR, 128u + (uint32_t)i);
            reg_wr(L_DP_DATA, N.hash[i]);
            reg_wr(L_DP_CMD, 1);
            for (int k = 0; k < 10 && !reg_rd(L_DP_SEL, &v) && !(v & B(31)); k++)
                ;
        }
        reg_wr(L_RFE_CTL, L_RFE_BCAST | L_RFE_PERFECT | L_RFE_MHASH | (all ? L_RFE_MCAST : 0) | (N.promisc ? L_RFE_UCAST : 0));
    } else if (D.chip == USBNET_CDC_ECM) {
        /* SET_ETHERNET_PACKET_FILTER: directed, broadcast, all multicast (and promiscuous); the
           exact filter is applied here, frame by frame (usbnet_filter_pass) */
        std_req(D.addr, 0x21, 0x43, (uint16_t)(0x0e | (N.promisc || psmac_ip_promisc(1) ? 1 : 0)), D.ctl_itf, 0, 0);
    }
}
static void apply_mac(void)
{
    uint32_t lo = (uint32_t)N.mac[0] | (uint32_t)N.mac[1] << 8 | (uint32_t)N.mac[2] << 16 | (uint32_t)N.mac[3] << 24;
    uint32_t hi = (uint32_t)N.mac[4] | (uint32_t)N.mac[5] << 8;
    if (D.chip == USBNET_SMSC95XX) {
        reg_wr(S_ADDRL, lo);
        reg_wr(S_ADDRH, hi);
    } else if (D.chip == USBNET_LAN78XX) {
        reg_wr(L_RX_ADDRL, lo);
        reg_wr(L_RX_ADDRH, hi);
        reg_wr(L_MAF_HI0, 0);
        reg_wr(L_MAF_LO0, lo);
        reg_wr(L_MAF_HI0, hi | B(31));                  /* perfect filter 0: our own address, valid */
        uint8_t k[6];
        if (psmac_ip_hwaddr(1, k)) {                    /* perfect filter 1: the remote control's */
            reg_wr(L_MAF_HI1, 0);
            reg_wr(L_MAF_LO1, (uint32_t)k[0] | (uint32_t)k[1] << 8 | (uint32_t)k[2] << 16 | (uint32_t)k[3] << 24);
            reg_wr(L_MAF_HI1, ((uint32_t)k[4] | (uint32_t)k[5] << 8) | B(31));
        }
    }
}
static void apply_phy(volatile struct an_unit *u)
{
    uint32_t a = u->cfg_adv, f = u->cfg_flags;
    if (D.chip == USBNET_CDC_ECM)
        return;
    if (f & AN_EC_AUTONEG) {
        int pause = (f & AN_EC_PAUSE) && D.chip == USBNET_SMSC95XX;     /* flow control on the LAN9514 only */
        mii_wr(4, (uint16_t)(0x0001 | ((a & AN_LM_10H) ? 0x020 : 0) | ((a & AN_LM_10F) ? 0x040 : 0) |
                             ((a & AN_LM_100H) ? 0x080 : 0) | ((a & AN_LM_100F) ? 0x100 : 0) | (pause ? 0x400 : 0)));
        if (D.chip == USBNET_LAN78XX)
            mii_wr(9, (uint16_t)(((a & AN_LM_1000H) ? 0x100 : 0) | ((a & AN_LM_1000F) ? 0x200 : 0)));
        mii_wr(0, 0x1200);                              /* autonegotiation on, restart */
    } else {
        uint32_t sp = u->cfg_speed;
        if (sp == 1000 && D.chip != USBNET_LAN78XX)
            sp = 100;
        mii_wr(0, (uint16_t)((sp == 1000 ? 0x0040 : sp == 100 ? 0x2000 : 0) | ((f & AN_EC_FULL) ? 0x0100 : 0)));
    }
}

/* returns 1 when the step is done, 0 to be called again (a wait), -1 failed */
static int init_step(volatile struct an_unit *u)
{
    uint32_t v = 0, t = now_ms() - N.t_step;
    if (D.chip == USBNET_CDC_ECM) {
        elog("%s found at USB address %u (%04x:%04x), bulk endpoints in %u out %u, %u-byte packets", chip_name(), D.addr, D.vid, D.pid, D.ep_in, D.ep_out, D.mps);
        apply_filter();
        return 2;                                       /* nothing more: frames flow */
    }
    if (D.chip == USBNET_SMSC95XX) {
        switch (N.step) {
        case 0: return reg_wr(S_HW_CFG, S_HW_LRST) ? -1 : 1;           /* lite reset */
        case 1: if (reg_rd(S_HW_CFG, &v)) return -1;
                if (v & S_HW_LRST) return t > 1000 ? -1 : 0;
                return 1;
        case 2: return reg_wr(S_PM_CTRL, S_PM_PHY_RST) ? -1 : 1;       /* the PHY's reset */
        case 3: if (reg_rd(S_PM_CTRL, &v)) return -1;
                if (v & S_PM_PHY_RST) return t > 1000 ? -1 : 0;
                return 1;
        case 4: if (reg_rd(S_ID_REV, &v)) return -1;
                D.chip_id = v >> 16;
                elog("%s found at USB address %u (%04x:%04x), chip id %04x rev %04x", chip_name(), D.addr, D.vid, D.pid, v >> 16, v & 0xffffu);
                apply_mac();
                return 1;
        case 5: /* one frame per transfer, an empty bulk-in answered with a zero-length packet (not a NAK),
                   no padding before the frame */
                if (reg_rd(S_HW_CFG, &v)) return -1;
                v &= ~(S_HW_BIR | S_HW_MEF | S_HW_BCE | S_HW_RXDOFF);
                if (reg_wr(S_HW_CFG, v) || reg_wr(S_BURST_CAP, 0) || reg_wr(S_BULK_IN_DLY, 0x2000)) return -1;
                return 1;
        case 6: reg_wr(S_INT_STS, 0xffffffffu);
                if (!reg_rd(S_LED_GPIO, &v))
                    reg_wr(S_LED_GPIO, v | 0x01110000u);    /* GPIOs as the speed, link and duplex LEDs: the socket's lights */
                reg_wr(S_FLOW, 0);
                reg_wr(S_AFC_CFG, 0x00f830a1u);             /* the datasheet's default flow-control levels */
                reg_wr(S_COE_CR, 0);                        /* no checksum offload: nothing appended to frames */
                apply_filter();
                return 1;
        case 7: if (reg_rd(S_MAC_CR, &v)) return -1;
                if (reg_wr(S_MAC_CR, v | S_MAC_TXEN) || reg_wr(S_TX_CFG, B(2)) || reg_wr(S_MAC_CR, v | S_MAC_TXEN | S_MAC_RXEN)) return -1;
                return 1;
        case 8: return mii_wr(0, 0x8000) ? -1 : 1;                     /* BMCR: reset */
        case 9: { int b = mii_rd(0); if (b < 0) return -1; if (b & 0x8000) return t > 1000 ? -1 : 0; }
                apply_phy(u);
                return 2;
        }
    } else {
        switch (N.step) {
        case 0: if (reg_rd(L_HW_CFG, &v) || reg_wr(L_HW_CFG, v | L_HW_LRST)) return -1;
                return 1;
        case 1: if (reg_rd(L_HW_CFG, &v)) return -1;
                if (v & L_HW_LRST) return t > 1000 ? -1 : 0;
                return 1;
        case 2: if (reg_rd(L_ID_REV, &v)) return -1;
                D.chip_id = v >> 16;
                elog("%s found at USB address %u (%04x:%04x), chip id %04x rev %04x", chip_name(), D.addr, D.vid, D.pid, v >> 16, v & 0xffffu);
                apply_mac();
                return 1;
        case 3: if (reg_rd(L_USB_CFG0, &v) || reg_wr(L_USB_CFG0, v & ~(L_USB_BIR | L_USB_BCE))) return -1;
                if (reg_wr(L_BURST_CAP, 0) || reg_wr(L_BULK_IN_DLY, 0x800)) return -1;
                if (reg_rd(L_HW_CFG, &v) || reg_wr(L_HW_CFG, (v & ~L_HW_MEF) | L_HW_CLKS)) return -1;
                return 1;
        case 4: reg_wr(L_FCT_RX_END, (12288u - 512u) / 512u);          /* the 12 KB FIFOs, as the datasheet sizes them */
                reg_wr(L_FCT_TX_END, (12288u - 512u) / 512u);
                reg_wr(L_INT_STS, 0xffffffffu);
                reg_wr(L_FLOW, 0);
                reg_wr(L_FCT_FLOW, 0);
                apply_filter();
                return 1;
        case 5: if (reg_rd(L_PMT_CTL, &v) || reg_wr(L_PMT_CTL, v | L_PMT_PHY_RST)) return -1;
                return 1;
        case 6: if (reg_rd(L_PMT_CTL, &v)) return -1;
                if ((v & L_PMT_PHY_RST) || !(v & L_PMT_READY)) return t > 1000 ? -1 : 0;
                return 1;
        case 7: if (reg_rd(0x100, &v) || reg_wr(L_MAC_CR, v | L_MAC_AUTO)) return -1;
                if (reg_wr(L_MAC_RX, (1518u + 4u) << 16) || reg_wr(L_MAC_RX, (1518u + 4u) << 16 | 1u)) return -1;
                if (reg_wr(L_MAC_TX, 1) || reg_wr(L_FCT_TX_CTL, L_EN) || reg_wr(L_FCT_RX_CTL, L_EN)) return -1;
                return 1;
        case 8: return mii_wr(0, 0x8000) ? -1 : 1;
        case 9: { int b = mii_rd(0); if (b < 0) return -1; if (b & 0x8000) return t > 1000 ? -1 : 0; }
                apply_phy(u);
                return 2;
        }
    }
    return -1;
}

/* the link from the PHY: BMSR in two halves (address, then data) on consecutive polls, so a poll
   never does more than one control transfer for it; the rest only when it changed */
static void link_poll(volatile struct an_unit *u)
{
    uint16_t acc = D.chip == USBNET_LAN78XX ? L_MII_ACC : S_MII_ADDR;
    uint32_t v;
    if (D.chip == USBNET_CDC_ECM) {
        if (!N.link) {
            N.link = 1; N.speed = 100; N.full = 1;
            u->link = 1; u->speed_mbps = 100; u->link_flags = AN_LF_FULL; u->link_up_s = now_ms() / 1000;
            ustate(u, AN_ST_CONNECTED, "link up (CDC-ECM: the device reports none; taken as 100 Mbit/s full duplex)");
        }
        return;
    }
    if ((int32_t)(now_ms() - N.t_next) < 0)
        return;
    if (N.mii_phase == 0) {
        reg_wr(acc, (uint32_t)PHY_ADDR << 11 | 1u << 6 | 1u);         /* read BMSR */
        N.mii_phase = 1;
        return;
    }
    N.mii_phase = 0;
    N.t_next = now_ms() + 500;
    if (reg_rd(acc + 4, &v))
        return;
    uint32_t bmsr = v & 0xffffu;
    int up = (bmsr & 4u) != 0;
    if (up == N.link && bmsr == N.bmsr_last)
        return;
    N.bmsr_last = bmsr;
    if (up && !N.link) {
        int adv = mii_rd(4), lpa = mii_rd(5), c1000 = 0, s1000 = 0;
        if (D.chip == USBNET_LAN78XX) { c1000 = mii_rd(9); s1000 = mii_rd(10); }
        if (adv < 0 || lpa < 0 || c1000 < 0 || s1000 < 0)
            return;
        struct usbnet_link l;
        usbnet_link_decode(bmsr, (unsigned)adv, (unsigned)lpa, (unsigned)c1000, (unsigned)s1000, &l);
        if (!(u->cfg_flags & AN_EC_AUTONEG)) { l.speed = u->cfg_speed > 100 && D.chip != USBNET_LAN78XX ? 100 : u->cfg_speed; l.full = (u->cfg_flags & AN_EC_FULL) != 0; }
        static const uint16_t bits[6] = { 0x020, 0x040, 0x080, 0x100, 0x400, 0x800 };
        static const uint8_t flag[6] = { AN_LM_10H, AN_LM_10F, AN_LM_100H, AN_LM_100F, AN_LM_PAUSE, AN_LM_ASYM };
        unsigned la = 0, lp = 0;
        for (int i = 0; i < 6; i++) {
            if (adv & bits[i]) la |= flag[i];
            if (lpa & bits[i]) lp |= flag[i];
        }
        la |= ((c1000 & 0x100) ? AN_LM_1000H : 0) | ((c1000 & 0x200) ? AN_LM_1000F : 0);
        lp |= ((s1000 & 0x400) ? AN_LM_1000H : 0) | ((s1000 & 0x800) ? AN_LM_1000F : 0);
        int pause = D.chip == USBNET_SMSC95XX && l.full && (adv & lpa & 0x400) && (u->cfg_flags & AN_EC_PAUSE);
        if (D.chip == USBNET_SMSC95XX) {
            uint32_t cr = 0, afc = 0;
            reg_rd(S_MAC_CR, &cr);
            reg_wr(S_MAC_CR, l.full ? cr | S_MAC_FDPX : cr & ~S_MAC_FDPX);
            reg_wr(S_FLOW, pause ? 0xffff0002u : 0);
            if (!reg_rd(S_AFC_CFG, &afc))
                reg_wr(S_AFC_CFG, pause ? afc | 0xfu : afc & ~0xfu);
        }
        N.link = 1; N.speed = (int)l.speed; N.full = (int)l.full;
        u->link_adv = la; u->link_lpa = lp;
        u->link_flags = (l.autoneg_done && (u->cfg_flags & AN_EC_AUTONEG) ? AN_LF_AUTONEG : 0) | (l.full ? AN_LF_FULL : 0) |
                        (pause ? AN_LF_RXPAUSE | AN_LF_TXPAUSE : 0);
        u->link_up_s = now_ms() / 1000;
        u->link = 1;
        u->speed_mbps = l.speed;
        char m[64];
        unsigned k = 0;
        for (const char *p = "link up, "; *p; p++) m[k++] = *p;
        { char dg[6]; int nd = 0; unsigned s = l.speed; do { dg[nd++] = (char)('0' + s % 10); s /= 10; } while (s); while (nd) m[k++] = dg[--nd]; }
        for (const char *p = l.full ? " Mbit/s full duplex" : " Mbit/s half duplex"; *p; p++) m[k++] = *p;
        m[k] = 0;
        ustate(u, AN_ST_CONNECTED, m);
    } else if (!up && N.link) {
        link_down(u, "no link: is the cable in?");
    }
}

/* ---- frames ------------------------------------------------------------------------------------ */
static uint32_t rx_have, rx_pos;                        /* a received transfer not yet all handed over */
static void rx_poll(volatile struct an_unit *u)
{
    volatile uint8_t *board = (volatile uint8_t *)hdr_of(u);
    for (int budget = 0; budget < 4; budget++) {
        if (!rx_have) {
            uint32_t got = 0;
            int r;
            if (!ch[CH_IN].busy) {
                ch_start(CH_IN, rxbuf, RXBUF);
                return;
            }
            r = ch_check(CH_IN, &got);
            if (r == 1)
                return;                                 /* still waiting */
            if (r == -1) { N.naks++; ch_start(CH_IN, rxbuf, RXBUF); return; }
            if (r == -2) { N.bulk_err++; ch_start(CH_IN, rxbuf, RXBUF); return; }
            if (!got) { ch_start(CH_IN, rxbuf, RXBUF); return; }      /* the zero-length "nothing" */
            rx_have = got;
            rx_pos = 0;
        }
        const uint8_t *f;
        unsigned len;
        for (;;) {
            unsigned pos = rx_pos;
            int r = usbnet_rx_next(D.chip, rxbuf, rx_have, &pos, &f, &len);
            if (r < 0)
                break;
            if (r == 0) { N.rx_bad++; u->rx_drop++; rx_pos = pos; continue; }
            if (psmac_ip_active) {                      /* the remote control's frames first (psmac_ip.c) */
                int k = psmac_ip_rx(1, f, len, N.mac, N.promisc);
                if (k != IPK_TO_MAC) { rx_pos = pos; continue; }
            }
            if (D.chip == USBNET_CDC_ECM && !usbnet_filter_pass(D.chip, f, N.mac, N.hash, N.promisc, N.allmulti)) { rx_pos = pos; continue; }
            if (!u->rx_on || u->admin_off) { u->rx_drop++; rx_pos = pos; continue; }
            if (u->rx_head - u->rx_tail >= AN_RX_SLOTS) {
                N.rx_held++;                            /* the Mac's 16 slots are full: keep it, and stop reading the chip */
                return;
            }
            volatile struct an_slot *s = (volatile struct an_slot *)(board + AN_OFF_RX(1) + (u->rx_head % AN_RX_SLOTS) * AN_SLOT_BYTES);
            for (unsigned i = 0; i < len; i++)
                s->data[i] = f[i];
            s->len = len;
            s->flags = (f[0] & 1) ? (f[0] == 0xff && f[1] == 0xff ? AN_SF_BCAST : AN_SF_MCAST) : 0;
            psmac_netprobe_rx(1, f, len, u->rx_head, &u->rx_tail);
            __asm__ volatile("dmb ish" ::: "memory");
            u->rx_head++;
            u->rx_pkts++; u->rx_bytes += len;
            N.rx_frames++; N.rx_bytes += len;
            raise_irq(u);
            rx_pos = pos;
        }
        rx_have = 0;
        ch_start(CH_IN, rxbuf, RXBUF);
    }
}
static unsigned tx_hdr, tx_len;                       /* the frame in txbuf, for the echo probe */
static void tx_poll(volatile struct an_unit *u)
{
    volatile uint8_t *board = (volatile uint8_t *)hdr_of(u);
    if (ch[CH_OUT].busy) {
        uint32_t got;
        int r = ch_check(CH_OUT, &got);
        if (r == 0 && tx_len)
            psmac_netprobe_tx(1, txbuf + tx_hdr, tx_len, 1);
        if (r == 1) {
            if (now_ms() - ch[CH_OUT].started_ms > 200) { ch_halt(CH_OUT); N.tx_err++; u->tx_err++; }
            return;
        }
        if (r == -1) { ch_start(CH_OUT, txbuf, ch[CH_OUT].len); return; }        /* the chip's FIFO was full: again */
        if (r == -2) { N.tx_err++; u->tx_err++; }
        /* a transfer that filled its last packet exactly ends with a zero-length one */
        if (r == 0 && ch[CH_OUT].len && !(ch[CH_OUT].len % D.mps)) { tx_len = 0; ch_start(CH_OUT, txbuf, 0); return; }
    }
    if (u->tx_tail == u->tx_head) {
        /* the Mac has nothing to send: one of the remote control's frames, if it has any */
        uint32_t n;
        if (psmac_ip_active && N.link && !u->admin_off && (n = psmac_ip_tx_take(1, txbuf + USBNET_TX_HDR_MAX, USBNET_FRAME_MAX))) {
            unsigned h = usbnet_tx_header(D.chip, txbuf, n);
            for (uint32_t i = 0; i < n; i++)
                txbuf[h + i] = txbuf[USBNET_TX_HDR_MAX + i];
            tx_hdr = h; tx_len = 0;
            ch_start(CH_OUT, txbuf, h + n);
        }
        return;
    }
    __asm__ volatile("dmb ish" ::: "memory");
    volatile struct an_slot *s = (volatile struct an_slot *)(board + AN_OFF_TX(1) + (u->tx_tail % AN_TX_SLOTS) * AN_SLOT_BYTES);
    uint32_t len = s->len;
    if (len < 14 || len > USBNET_FRAME_MAX || !N.link || u->admin_off) {
        u->tx_err++;
    } else {
        unsigned h, pad = len < 60 ? 60 : len;          /* the shortest Ethernet frame */
        h = usbnet_tx_header(D.chip, txbuf, pad);
        for (uint32_t i = 0; i < len; i++)
            txbuf[h + i] = s->data[i];
        psmac_ip_mac_tx(1, txbuf + h, len);
        for (uint32_t i = len; i < pad; i++)
            txbuf[h + i] = 0;
        u->tx_pkts++; u->tx_bytes += len;
        N.tx_frames++; N.tx_bytes += len;
        tx_hdr = h; tx_len = len;
        psmac_netprobe_tx(1, txbuf + h, len, 0);
        ch_start(CH_OUT, txbuf, h + pad);
    }
    __asm__ volatile("dmb ish" ::: "memory");
    u->tx_tail++;
    raise_irq(u);
}

/* ---- the Mac's commands ----------------------------------------------------------------------------- */
static uint32_t arg32(volatile struct an_unit *u, unsigned o) { return (uint32_t)u->arg[o] << 24 | (uint32_t)u->arg[o + 1] << 16 | (uint32_t)u->arg[o + 2] << 8 | u->arg[o + 3]; }
static void commands(volatile struct an_unit *u)
{
    uint32_t seq = u->cmd_seq;
    int r = 0, live = N.state == S_RUN || N.state == S_PHY;
    if (seq == u->cmd_done)
        return;
    __asm__ volatile("dmb ish" ::: "memory");
    switch (u->cmd) {
    case AN_CMD_START:  u->admin_off = 0; break;
    case AN_CMD_OFF:    u->admin_off = 1; break;
    case AN_CMD_ENABLE: u->admin_off = arg32(u, 0) ? 0 : 1; elog("switched %s", u->admin_off ? "off" : "on"); u->state_seq++; break;
    case AN_CMD_RXON:   u->rx_on = 1; break;
    case AN_CMD_RXOFF:  u->rx_on = 0; break;
    case AN_CMD_FILTER: {
        uint32_t f = arg32(u, 0) | u->arg[3];
        N.promisc = (f & AN_FILT_PROMISC) != 0;
        N.allmulti = (f & AN_FILT_ALLMULTI) != 0;
        if (live)
            apply_filter();
        break; }
    case AN_CMD_SETMAC: {
        uint8_t m[6];
        int any = 0;
        for (int i = 0; i < 6; i++) { m[i] = u->arg[i]; any |= m[i]; }
        if (any && (m[0] & 1)) { r = -2; break; }
        for (int i = 0; i < 6; i++)
            N.mac[i] = any ? m[i] : N.fmac[i];
        if (u->arg[6] == 1)
            memcpy(N.mac + 3, N.fmac + 3, 3);
        for (int i = 0; i < 6; i++)
            u->mac[i] = N.mac[i];
        if (live)
            apply_mac();
        elog("MAC address now %02x:%02x:%02x:%02x:%02x:%02x%s", N.mac[0], N.mac[1], N.mac[2], N.mac[3], N.mac[4], N.mac[5], any ? "" : " (the Pi's own)");
        u->state_seq++;
        break; }
    case AN_CMD_ETHCFG: {
        uint32_t a = arg32(u, 0) & 0x3fu, f = arg32(u, 4) & (AN_EC_AUTONEG | AN_EC_FULL | AN_EC_PAUSE), sp = arg32(u, 8);
        if ((f & AN_EC_AUTONEG) ? !a : (sp != 10 && sp != 100 && sp != 1000)) { r = -2; break; }
        u->cfg_adv = a; u->cfg_flags = f; u->cfg_speed = sp ? sp : 100;
        if (live)
            apply_phy(u);
        u->state_seq++;
        break; }
    case AN_CMD_SCAN: case AN_CMD_JOIN: case AN_CMD_DISCONNECT: case AN_CMD_WIFICFG:
        break;
    default:
        r = -3;
    }
    u->cmd_result = r;
    __asm__ volatile("dmb ish" ::: "memory");
    u->cmd_done = seq;
    raise_irq(u);
}

/* ---- the bus around us ------------------------------------------------------------------------------ */
/* psmac_usb.c, before it resets the root port (and with it our chip): the bulk channels stop */
/* A bulk-in the device keeps answering with NAK (a CDC-ECM device with nothing to say) is retried
   by QEMU's DWC2 model even after the channel is disabled, and after the bus reset the address
   is nobody's and the emulator stops on an assertion (hw/usb/core.c usb_ep_get, 19 November). So
   the channel is given one last transfer that ends at once, a SETUP to the device's endpoint 0,
   which replaces the pending one. On the real controller the disable is enough and this costs a
   few microseconds before a reset that undoes it anyway. */
static void ch_retire(int c)
{
    static uint8_t setup[8] __attribute__((aligned(64))) = { 0x80, 0, 0, 0, 0, 0, 2, 0 };    /* GET_STATUS */
    if (!D.addr)
        return;
    arm_flush_cache((uintptr_t)setup, sizeof setup);
    wr(HCINT(c), 0xffffffffu);
    wr(HCSPLT(c), 0);
    wr(HCTSIZ(c), 8u | 1u << 19 | 3u << 29);           /* 8 bytes, one packet, SETUP */
    wr(HCDMA(c), bus_addr(setup));
    wr(HCCHAR(c), (uint32_t)D.addr << 22 | 1u << 20 | (D.mps0 ? D.mps0 : 64) | B(31));
    for (int i = 0; i < 1000 && !(rd(HCINT(c)) & B(1)); i++) {
        uint64_t t = now() + hz() / 100000;
        while (now() < t)
            ;
    }
    ch_halt(c);
}
void psmac_usbnet_bus_reset(void)
{
    if (N.owner != 1 || !N.gen)
        return;                                         /* the first enumeration (core 1): nothing of ours runs yet */
    ch_halt(CH_IN);
    ch_halt(CH_OUT);
    ch_retire(CH_IN);
    ch[CH_IN].pid = ch[CH_OUT].pid = 0;
    rx_have = 0;
    N.state = S_IDLE;
}

static void start_owner(volatile struct an_unit *u)
{
    const char *opt = psmac_option("eth");
    int w = psmac_board_wired();
    N.owner = 0;
    if (w != WIRED_SMSC95XX && w != WIRED_LAN78XX)
        return;                                         /* a Pi 4 (alice_net.c's GENET) or no wired port */
    N.owner = 1;
    u->kind = AN_KIND_ETHER;
    u->cfg_adv = AN_LM_10H | AN_LM_10F | AN_LM_100H | AN_LM_100F | (w == WIRED_LAN78XX ? AN_LM_1000H | AN_LM_1000F : 0);
    u->cfg_flags = AN_EC_AUTONEG | AN_EC_FULL;
    u->cfg_speed = w == WIRED_LAN78XX ? 1000 : 100;
    learn_mac(u);
    if (opt && !strncmp(opt, "off", 3)) {
        u->kind = AN_KIND_NONE;
        N.state = S_FAILED;
        elog("off (eth=off in cmdline.txt)");
        return;
    }
    elog("wired port: %s; MAC %02x:%02x:%02x:%02x:%02x:%02x (the Pi's own)", psmac_board_wired_name(),
         N.mac[0], N.mac[1], N.mac[2], N.mac[3], N.mac[4], N.mac[5]);
    ustate(u, AN_ST_STARTING, "looking for the network chip on the USB bus");
    N.state = S_IDLE;
}

/* alice_psmac.c (from alice_net.c's Ethernet turn, core 3): 1 = this file owns unit 1 */
int psmac_usbnet_poll(volatile struct an_unit *u)
{
    if (!N.ready) {
        N.ready = 1;
        start_owner(u);
    }
    if (N.owner != 1)
        return 0;
    if (N.state == S_FAILED && u->kind == AN_KIND_NONE) {     /* eth=off: answer commands, nothing else */
        if (u->cmd_seq != u->cmd_done) { u->cmd_result = -1; __asm__ volatile("dmb ish" ::: "memory"); u->cmd_done = u->cmd_seq; raise_irq(u); }
        return 1;
    }
    commands(u);
    unsigned ports = 0;
    int speed = 0, dma = 0;
    uint32_t gen = psmac_usb_bus(&ports, &speed, &dma);
    if (gen != N.gen) {
        /* a new enumeration of the bus (or the first): find the chip again */
        if (N.link)
            link_down(u, "the USB bus was enumerated again: the network chip restarts");
        N.gen = gen;
        N.state = gen ? S_PROBE : S_IDLE;
        N.hub_ports = ports;
        N.root_speed = speed;
        N.dma = dma;
        N.t_probe = now_ms();
        ch[CH_IN].pid = ch[CH_OUT].pid = 0;
        ch[CH_IN].busy = ch[CH_OUT].busy = 0;
        rx_have = 0;
    }
    switch (N.state) {
    case S_PROBE:
        if (!N.dma) {
            N.state = S_FAILED;
            ustate(u, AN_ST_FAILED, "the wired port needs the USB DMA transport (usb=pio has no bulk transfers)");
            break;
        }
        if (probe() == 0) {
            u->kind = AN_KIND_ETHER;
            N.state = S_INIT;
            N.step = 0;
            N.t_step = now_ms();
            N.inits++;
            reg_errors = 0;
            break;
        }
        N.state = S_ABSENT;
        N.t_probe = now_ms();
        /* The card stays in the declaration ROM (that was decided before the 68k started), but
           a driver that opens it now finds no unit (kind 0) and fails its Open, as with eth=off:
           Riptide then carries on with the WiFi, and EtherTalk falls back to the next card. */
        u->kind = AN_KIND_NONE;
        ustate(u, AN_ST_FAILED, psmac_board_wired() == WIRED_LAN78XX ? "no LAN7800 found behind the 3B+'s hubs" : "no network chip found on the USB bus");
        break;
    case S_ABSENT:
        /* look again now and then: cheap, and the chip may have been slow to wake */
        if (now_ms() - N.t_probe > 10000)
            N.state = S_PROBE;
        break;
    case S_INIT: {
        int r = init_step(u);
        if (r < 0) {
            N.state = S_FAILED;
            elog("%s: setting it up failed at step %d (%u register errors)", chip_name(), N.step, reg_errors);
            ustate(u, AN_ST_FAILED, "the wired port's chip did not answer as expected");
            N.t_probe = now_ms();
        } else if (r == 1) {
            N.step++;
            N.t_step = now_ms();
        } else if (r == 2) {
            N.state = S_PHY;
            N.t_next = now_ms() + 200;
            N.mii_phase = 0;
            N.bmsr_last = 0;
            N.link = 0;
            ustate(u, AN_ST_IDLE, "waiting for the link");
            ch_start(CH_IN, rxbuf, RXBUF);
        }
        break; }
    case S_PHY:
    case S_RUN:
        link_poll(u);
        N.state = N.link ? S_RUN : S_PHY;
        rx_poll(u);
        if (N.link)
            tx_poll(u);
        else
            while (u->tx_tail != u->tx_head) { u->tx_tail++; u->tx_err++; }
        break;
    case S_FAILED:
        /* a chip that failed its set-up: try again from the probe after ten seconds */
        if (now_ms() - N.t_probe > 10000 && N.gen)
            N.state = S_PROBE;
        while (u->tx_tail != u->tx_head) { u->tx_tail++; u->tx_err++; }
        break;
    default:
        while (u->tx_tail != u->tx_head) { u->tx_tail++; u->tx_err++; }
        break;
    }
    return 1;
}

/* the heartbeat (every five seconds): a line every fifteen while the port is ours */
void psmac_usbnet_report(void)
{
    static unsigned beat;
    if (N.owner != 1 || beat++ % 3)
        return;
    static const char *const st[] = { "waiting for the USB bus", "probing", "setting up", "no link", "running", "not found", "failed" };
    kprintf("[ETH]   %s: %s, link %u%s, rx %u frames (%u bytes) tx %u (%u bytes), bad %u held %u tx errors %u bulk errors %u\n",
            D.chip ? chip_name() : psmac_board_wired_name(), st[N.state], N.link ? N.speed : 0, N.link ? (N.full ? " full" : " half") : "",
            N.rx_frames, N.rx_bytes, N.tx_frames, N.tx_bytes, N.rx_bad, N.rx_held, N.tx_err, N.bulk_err);
}
int psmac_usbnet_owns(void) { return N.owner == 1; }
