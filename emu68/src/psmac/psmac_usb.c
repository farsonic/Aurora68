/* psmac_usb.c - USB keyboards and mice on a Pi 3 (the solo board and the SE). MPL-2.0.
 *
 * The keyboard and the mouse are Alice68's native HID host, kept apart from any other USB
 * device as Alice68 keeps them (src/alice/usb, copied verbatim from Alice68's
 * frf_usb/native: frf_dwc2.c brings the BCM2837's Synopsys DWC2 controller up as a host,
 * frf_usb_native_hid.c enumerates the root port's device, follows the Pi 3's LAN9514 hub port
 * by port, routes full- and low-speed devices through the hub's transaction translator (split
 * transactions), takes the first HID boot keyboard and the first boot mouse it finds and polls
 * their interrupt endpoints). A 3A+ and a Zero 2 W have no hub on the board: the one port is the
 * root port, with a keyboard, a mouse, a keyboard-and-mouse receiver or a hub of the user's on it,
 * and all of those go the same way (a high-speed hub like the LAN9514, a full-speed one with
 * the device spoken to directly, a device alone with no route at all). Their reports go where the Bluetooth host's go:
 * frf_native_usb_keyboard_boot_report and aga_virtual_mouse_update (alice_psmac.c), into the
 * input ring that .PiInput turns into Mac events. Anything else on the bus is left alone, for a
 * USB stack of its own later.
 *
 * This file is the Pi 3 glue: the controller's power (the firmware's SET_POWER_STATE, device 3),
 * its registers (little-endian, on a big-endian kernel), the timing, a transport, and the place
 * in core 3's loop.
 *
 * Two transports with one interface (frf_dwc2_pio.h: one packet, synchronous, bounded):
 *   DMA (the default): the channel reads and writes a bounce buffer through the VideoCore's bus
 *     alias, with the cache cleaned and invalidated around it. This is what QEMU's DWC2 model
 *     emulates (it has no FIFO mode), so the testbed exercises it, and what Linux and the other
 *     bare-metal hosts use on a Pi 3.
 *   FIFO (usb=pio): Alice68's own frf_dwc2_pio.c, unchanged, compiled under the names
 *     frf_dwc2_fifo_* (CMakeLists.txt).
 * Both follow the same steps for a device behind the hub: start split, on its ACK complete split,
 * complete split again on NYET (up to eight times), NAK = no data now.
 *
 * The start (the controller's power, its reset, the first enumeration: about a second, bounded)
 * runs on core 1, which has nothing else to do, so the mailbox services on core 3 are there for
 * the Mac from its first instruction (docs/08 item 50). Then core 3 runs it next to them, the
 * network and Bluetooth, so nothing may hold the core long: one poll a millisecond, and every
 * two seconds a look at the hub's ports (one control request each); a port that changed (plugged,
 * unplugged) starts a fresh enumeration, with every key released first.
 *
 * USB flash drives (since 20 November 2026) are psmac_usbdisk.c's: after every enumeration, still
 * on core 1 and before core 3 is let back onto the bus, it looks at the hub's ports for sticks and
 * configures them, and between enumerations core 1 runs the sticks' work for the Mac.
 *
 * Kernel options: usb=off (leave the controller alone), usb=pio (Alice68's FIFO transport),
 * mac.usbdisk=off|on|all (psmac_usbdisk.c).
 */
#include <stdint.h>
#include <string.h>
#include "support.h"
#include "mmu.h"
#include "../alice/alice_psmac.h"
#include "../alice/usb/frf_dwc2.h"
#include "../alice/usb/frf_dwc2_pio.h"
#include "../alice/usb/frf_usb_native_hid.h"
#include "psmac_usbdisk.h"

const char *psmac_option(const char *key);
int alice_mbox(uint32_t tag, uint32_t a, uint32_t b, uint32_t *out);
void frf_native_usb_keyboard_force_release(void);
int psmac_board_netchip_on_hub(void);

/* frf_usb_native_hid.c's flags (alice_psmac.c has seen and buttons) */
volatile uint8_t frf_usb_mouse_present;

#define DWC2 (ALICE_PERI + 0x980000ull)         /* BCM2837 0x3F980000 in the kernel's peripheral mapping */

static uint32_t mmio_read(void *ctx, uint32_t r)
{
    uint32_t v = *(volatile uint32_t *)((uintptr_t)ctx + r);
    __asm__ volatile("dmb oshld" ::: "memory");
    v = __builtin_bswap32(v);
    /* GHWCFG2's high-speed PHY field: the BCM2837 says 1 (UTMI+), QEMU's DWC2 model leaves it 0,
       which frf_dwc2_host_init takes for "no high-speed PHY". Read as the chip's own value. */
    if (r == 0x048 && !(v & (3u << 6)))
        v |= 1u << 6;
    return v;
}
static void mmio_write(void *ctx, uint32_t r, uint32_t v)
{
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t *)((uintptr_t)ctx + r) = __builtin_bswap32(v);
    __asm__ volatile("dsb oshst" ::: "memory");
}
static inline uint64_t now(void) { uint64_t v; __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v)); return v; }
static inline uint64_t hz(void)  { uint64_t v; __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(v)); return v; }
/* A wait of the USB code. On core 1 (the start, and every later enumeration) the sound output is fed
   meanwhile (psmac_audio.c): an enumeration holds core 1 for a second, and most of it is in here. */
extern int psmac_audio_poll(void);
extern int psmac_video_poll(void);
static inline int on_core1(void) { uint64_t m; __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(m)); return (m & 3) == 1; }
static void delay_us(void *ctx, unsigned us)
{
    (void)ctx;
    uint64_t t0 = now(), n = ((uint64_t)us * hz() + 999999u) / 1000000u;
    int feed = us >= 50 && on_core1();
    while (now() - t0 < n)
        if (feed) psmac_audio_poll();
}

static const struct frf_dwc2_ops ops = { mmio_read, mmio_write, delay_us, (void *)(uintptr_t)DWC2 };
static int use_fifo;
/* The root port's speed (HPRT: 0 high, 1 full, 2 low). Split transactions are what a high-speed
   hub's translator needs (the Pi 3's LAN9514); behind a full-speed hub (QEMU's usb-hub) the hub
   repeats full- and low-speed packets as they are, and the channel talks to the device directly. */
static int root_speed;

/* ---- the DMA transport ---- */
enum { GAHBCFG = 0x008, GRSTCTL = 0x010, GINTSTS = 0x014, GINTMSK = 0x018, GRXFSIZ = 0x024, GNPTXFSIZ = 0x028,
       HPTXFSIZ = 0x100, HCFG = 0x400, HAINTMSK = 0x418,
       HCCHAR0 = 0x500, HCSPLT0 = 0x504, HCINT0 = 0x508, HCINTMSK0 = 0x50c, HCTSIZ0 = 0x510, HCDMA0 = 0x514 };
#define B(n) (1u << (n))
static uint8_t dma_buf[64] __attribute__((aligned(64)));

static uint32_t rd(uint32_t r) { return mmio_read(ops.ctx, r); }
static void wr(uint32_t r, uint32_t v) { mmio_write(ops.ctx, r, v); }

static int wait_clear(uint32_t r, uint32_t mask)
{
    for (unsigned i = 0; i < 10000; i++) {
        if (!(rd(r) & mask))
            return 0;
        delay_us(0, 10);
    }
    return -1;
}

int frf_dwc2_fifo_init(const struct frf_dwc2_ops *o);
int frf_dwc2_fifo_packet(const struct frf_dwc2_ops *o, struct frf_dwc2_packet *p, struct frf_dwc2_pio_stats *st);

static int dma_init(void)
{
    wr(GRXFSIZ, 512);
    wr(GNPTXFSIZ, (256u << 16) | 512u);
    wr(HPTXFSIZ, (512u << 16) | 768u);
    wr(HCFG, rd(HCFG) & ~3u);                   /* 30/60 MHz: the high-speed PHY's clock */
    wr(GRSTCTL, B(4) | (0x10u << 6));           /* flush every TX FIFO, then the RX FIFO */
    if (wait_clear(GRSTCTL, B(4)))
        return -2;
    wr(GRSTCTL, B(5));
    if (wait_clear(GRSTCTL, B(5)))
        return -3;
    /* buffer DMA, INCR4 bursts; no interrupts: polled */
    wr(GAHBCFG, (rd(GAHBCFG) & ~(B(0) | (15u << 1))) | B(5) | (3u << 1));
    wr(GINTSTS, 0xffffffffu);
    wr(GINTMSK, 0);
    wr(HAINTMSK, 1u);
    wr(HCINTMSK0, 0x7ffu);
    wr(HCINT0, 0xffffffffu);
    return 0;
}

int frf_dwc2_pio_init(const struct frf_dwc2_ops *o)
{
    return use_fifo ? frf_dwc2_fifo_init(o) : dma_init();
}

/* one channel run: enable, wait for the halt, the channel's interrupt bits (0: timeout) */
static uint32_t run(uint32_t charval)
{
    wr(HCINT0, 0xffffffffu);
    wr(HCCHAR0, charval | B(31));
    for (unsigned i = 0; i < 5000; i++) {           /* 50 ms */
        uint32_t s = rd(HCINT0);
        if (s & B(1))
            return s;
        delay_us(0, 10);
    }
    wr(HCCHAR0, rd(HCCHAR0) | B(30) | B(31));       /* halt it */
    wait_clear(HCCHAR0, B(31));
    return 0;
}

static int dma_packet(struct frf_dwc2_packet *p, struct frf_dwc2_pio_stats *st)
{
    uint32_t bus, splitval = 0, charval, size, xfer, s = 0;
    unsigned split_retries = 0;
    if (!p || p->address > 127 || p->endpoint > 15 || !p->max_packet || p->max_packet > 64 ||
        p->length > p->max_packet || (p->length && !p->buffer) || p->type > 3 ||
        (p->hub_address && (!p->hub_port || p->hub_address > 127 || p->hub_port > 127)))
        return -1;
    if (st)
        st->attempts++;
    p->received = 0;
    if (rd(HCCHAR0) & B(31)) {
        wr(HCCHAR0, rd(HCCHAR0) | B(30) | B(31));
        if (wait_clear(HCCHAR0, B(31)))
            return -2;
    }
    /* the bounce buffer: the data out, or room for a whole packet in */
    if (!p->in && p->length)
        memcpy(dma_buf, p->buffer, p->length);
    arm_flush_cache((uintptr_t)dma_buf, sizeof dma_buf);
    bus = (uint32_t)mmu_virt2phys((uintptr_t)dma_buf) | 0xC0000000u;     /* the VideoCore's uncached alias */
    xfer = p->in ? p->max_packet : p->length;
    if (p->hub_address && root_speed == 0)
        splitval = B(31) | (3u << 14) | ((uint32_t)p->hub_address << 7) | p->hub_port;   /* xactpos: all */
    charval = ((uint32_t)p->address << 22) | (1u << 20) | ((uint32_t)p->type << 18) | ((uint32_t)!!p->low_speed << 17) |
              ((uint32_t)!!p->in << 15) | ((uint32_t)p->endpoint << 11) | p->max_packet;
    /* an interrupt endpoint goes in the next frame */
    if (p->type == 3 && !(rd(0x408) & 1u))
        charval |= B(29);
    size = xfer | (1u << 19) | ((uint32_t)p->pid << 29);
    wr(HCSPLT0, splitval);
    wr(HCTSIZ0, size);
    wr(HCDMA0, bus);
    s = run(charval);
    if (splitval && s && (s & B(5)) && !(s & B(0))) {
        /* the hub's translator took the start split: now the complete split, again on NYET */
        for (;;) {
            delay_us(0, 125);
            wr(HCSPLT0, splitval | B(16));
            wr(HCTSIZ0, size);
            wr(HCDMA0, bus);
            if (p->type == 3)
                charval = (charval & ~B(29)) | (rd(0x408) & 1u ? 0 : B(29));
            s = run(charval);
            if (s && (s & B(6)) && ++split_retries <= 8)
                continue;
            break;
        }
        if (s & B(6)) {
            if (st)
                st->naks++;
            return -8;
        }
    }
    if (st) {
        st->last_channel_interrupt = s;
        st->last_global_interrupt = rd(GINTSTS);
    }
    if (!s) {
        if (st)
            st->timeouts++;
        return -7;
    }
    if (s & B(3)) {
        if (st)
            st->stalls++;
        return -4;
    }
    if (s & B(4)) {
        if (st)
            st->naks++;
        return -5;
    }
    if (s & (B(2) | B(7) | B(8) | B(9) | B(10))) {
        if (st)
            st->errors++;
        return -6;
    }
    if (!(s & B(0)))
        return -6;
    if (p->in) {
        uint32_t left = rd(HCTSIZ0) & 0x7ffffu, got = xfer > left ? xfer - left : 0;
        if (got > p->length)
            got = p->length;
        arm_dcache_invalidate((uintptr_t)dma_buf, sizeof dma_buf);
        if (got)
            memcpy(p->buffer, dma_buf, got);
        p->received = (uint16_t)got;
        if (st)
            st->rx_bytes += got;
    }
    if (st)
        st->completed++;
    return 0;
}

int frf_dwc2_pio_packet(const struct frf_dwc2_ops *o, struct frf_dwc2_packet *p, struct frf_dwc2_pio_stats *st)
{
    /* A low-speed keyboard or mouse on the root port itself: a 3A+'s one USB socket, a Zero 2 W's
       OTG adapter, no hub between. The channel must be told it talks to a low-speed device
       (HCCHAR's LSpdDev) as much as for one behind a hub, but Alice68's enumeration marks only the
       latter: on the Pi 3B's LAN9514 nothing else ever reached the root port. A low-speed device
       cannot be a hub, so with the root port at low speed every packet without a hub route is its. */
    if (p && !p->hub_address && root_speed == 2)
        p->low_speed = 1;
    return use_fifo ? frf_dwc2_fifo_packet(o, p, st) : dma_packet(p, st);
}

/* ---- bring-up, polling, plug and unplug ---- */
static struct frf_native_hid hid;
static int usb_on, root_connected;
static uint64_t next_poll, next_check;
static uint32_t enumerations;

static void hub_clear(unsigned port, unsigned feature);

static void report(const char *what, int rc)
{
    kprintf("[USB] %s rc=%d: %u keyboard%s, %u %s (hub ports %u, devices seen %u, last port error %d)\n", what, rc,
            hid.have_keyboard, hid.have_keyboard == 1 ? "" : "s", hid.have_mouse, hid.have_mouse == 1 ? "mouse" : "mice",
            hid.root_hub_ports, hid.ports_seen, (int)hid.port_error);
}

/* the bus's generation for psmac_usbnet.c (the Pi 3's wired Ethernet): one more after every
   enumeration, which resets the root port and every device behind the hub with it */
static uint32_t bus_gen;
void psmac_usbnet_bus_reset(void);

/* the ports that saw a plug or an unplug since the last enumeration (bit n: hub port n, bit 0: the
   root port), for psmac_usbdisk.c: a stick taken out and put back between two looks at the ports is
   a new disk, not the old one after a bus reset. Every port counts as changed at the start. */
static uint32_t port_changes = ~0u;

static void enumerate(const char *why)
{
    struct frf_dwc2_state s;
    int rc;
    uint32_t changed = port_changes;
    port_changes = 0;
    enumerations++;
    rc = frf_dwc2_port_reset(&ops, &s);
    if (rc) {
        kprintf("[USB] %s: root port rc=%d (HPRT %08x, connected %u)\n", why, rc, s.port_status, s.port_connected);
        memset(&hid, 0, sizeof hid);
        root_connected = s.port_connected != 0;
        psmac_usbdisk_claim(0, 0, 0, 0, !use_fifo, changed);      /* nothing on the bus: every stick is gone */
        __atomic_store_n(&bus_gen, bus_gen + 1, __ATOMIC_RELEASE);
        return;
    }
    root_connected = 1;
    root_speed = (int)s.port_speed;
    rc = frf_native_hid_enumerate(&ops, &hid, (unsigned)root_speed);
    report(why, rc);
    /* the hub saw its ports' devices arrive as their power came on: those changes are this
       enumeration's, not the next plug */
    for (unsigned k = 1; k <= hid.root_hub_ports; k++)
        hub_clear(k, 16);                       /* C_PORT_CONNECTION */
    /* USB flash drives (psmac_usbdisk.c), while core 3 still keeps off the bus */
    psmac_usbdisk_claim(hid.root_hub_ports, root_speed, root_connected, hid.have_keyboard || hid.have_mouse, !use_fifo, changed);
    __atomic_store_n(&bus_gen, bus_gen + 1, __ATOMIC_RELEASE);
}

void psmac_usb_start(void)
{
    struct frf_dwc2_state s;
    uint32_t state = 0;
    int rc;
    const char *opt = psmac_option("usb");
    if (opt && !strncmp(opt, "off", 3)) {
        kprintf("[USB] off (usb=off)\n");
        return;
    }
    use_fifo = opt && !strncmp(opt, "pio", 3);
    for (int i = 0; i < 50; i++) {                      /* -9: the WiFi has the mailbox for a moment */
        rc = alice_mbox(0x00028001u, 3, 3, &state);     /* SET_POWER_STATE: USB HCD on, wait */
        if (rc != -9)
            break;
        delay_us(0, 20000);
    }
    if (rc || !(state & 1u))
        kprintf("[USB] firmware power request rc=%d state %08x (trying anyway)\n", rc, state);
    delay_us(0, 100000);
    rc = frf_dwc2_probe(&ops, &s);
    if (rc) {
        kprintf("[USB] no DWC2 controller (rc=%d, id %08x): USB off\n", rc, s.core_id);
        return;
    }
    rc = frf_dwc2_host_init(&ops);
    if (!rc)
        rc = frf_dwc2_pio_init(&ops);
    kprintf("[USB] DWC2 %08x (hwcfg2 %08x): host init rc=%d, %s transport\n", s.core_id, s.hardware_config, rc,
            use_fifo ? "FIFO (Alice68)" : "DMA");
    if (rc)
        return;
    enumerate("start");
    next_poll = now();
    next_check = now() + 2 * hz();
    __atomic_store_n(&usb_on, 1, __ATOMIC_RELEASE);     /* core 3's polls begin (start runs on core 1) */
}

/* Every later enumeration runs on core 1 as well. Until 20 November 2026 core 3 ran them itself,
   in the middle of its loop: on the first SE boots (a Pi 3B with no keyboard or mouse) each retry
   below held the loop for 360 ms (the root port's reset, the LAN9514's five ports powered, reset
   and waited for), every six seconds of the boot's first half minute, and the mailbox, the card
   and the network waited behind it. Now core 3 only stops its own users of the controller (the
   key state, the network chip's bulk channels), takes usb_on down so that neither its polls nor
   psmac_usbnet.c touch the controller, and hands the reason to core 1, which enumerates and puts
   usb_on back. A Mac without USB input loses nothing; one with it has no keyboard or mouse for the
   enumeration's second, as before. */
static const char *volatile enum_why;
static void enumerate_elsewhere(const char *why)       /* core 3 */
{
    frf_native_usb_keyboard_force_release();
    psmac_usbnet_bus_reset();                   /* the network chip's bulk channels stop before the reset takes it */
    __atomic_store_n(&usb_on, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&enum_why, why, __ATOMIC_RELEASE);
    __asm__ volatile("dsb ish; sev" ::: "memory");
}
/* Core 1 waits here between enumerations, feeding the sound output (psmac_audio.c) as it goes. Its
   wfe wakes on core 3's sev and, since 20 November 2026, on the generic timer's event stream as well,
   whenever bit 13 of the counter goes from 0 to 1 (every 0.85 ms at the Pi 3's 19.2 MHz), so the
   output's ring is looked at a thousand times a second without core 1 spinning. Before an enumeration the ring is filled with
   silence: the DMA goes on playing it while core 1 is busy, and repeats nothing stale. */
extern void psmac_audio_pause(void);
void __attribute__((noreturn)) psmac_usb_core1(void)  /* core 1, after psmac_usb_start, for ever */
{
    uint64_t k;
    __asm__ volatile("mrs %0, CNTKCTL_EL1" : "=r"(k));
    k = (k & ~0xf0ull) | 13u << 4 | 1u << 2;                    /* EVNTI = 13, EVNTEN */
    __asm__ volatile("msr CNTKCTL_EL1, %0; isb" :: "r"(k));
    for (;;) {
        const char *why;
        while (!(why = __atomic_load_n(&enum_why, __ATOMIC_ACQUIRE))) {
            if (psmac_usbdisk_core1())          /* a USB stick's work for the Mac (psmac_usbdisk.c) */
                continue;
            int a = psmac_audio_poll();
            if (!psmac_video_poll() && !a)      /* the video card's decoder (psmac_video.c) between the sound's turns */
                __asm__ volatile("wfe");
        }
        psmac_audio_pause();
        enumerate(why);
        next_poll = now();
        next_check = now() + 2 * hz();
        __atomic_store_n(&enum_why, (const char *)0, __ATOMIC_RELAXED);
        __atomic_store_n(&usb_on, 1, __ATOMIC_RELEASE);
    }
}

/* control requests to the hub at address 1 (Alice68's enumeration gives the root device that
   address); its endpoint 0 takes 64 bytes, as every high-speed hub's must */
static int hub_request(const uint8_t setup[8], uint8_t *data, uint16_t len)
{
    struct frf_dwc2_packet p = { 1, 0, 0, 0, 0, FRF_USB_PID_SETUP, 64, 8, 0, (uint8_t *)setup, 0, 0 };
    if (frf_dwc2_pio_packet(&ops, &p, NULL))
        return -1;
    if (len) {
        p = (struct frf_dwc2_packet){ 1, 0, 1, 0, 0, FRF_USB_PID_DATA1, 64, len, 0, data, 0, 0 };
        if (frf_dwc2_pio_packet(&ops, &p, NULL) || p.received != len)
            return -1;
    }
    /* the status stage: the other direction, no data, DATA1 */
    p = (struct frf_dwc2_packet){ 1, 0, len ? 0 : 1, 0, 0, FRF_USB_PID_DATA1, 64, 0, 0, NULL, 0, 0 };
    return frf_dwc2_pio_packet(&ops, &p, NULL) ? -1 : 0;
}

static int hub_port_status(unsigned port, uint32_t *status)
{
    uint8_t setup[8] = { 0xa3, 0, 0, 0, (uint8_t)port, 0, 4, 0 }, data[4] = { 0 };
    if (hub_request(setup, data, 4))
        return -1;
    *status = (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
    return 0;
}

static void hub_clear(unsigned port, unsigned feature)
{
    uint8_t setup[8] = { 0x23, 1, (uint8_t)feature, 0, (uint8_t)port, 0, 0, 0 };     /* CLEAR_FEATURE */
    hub_request(setup, NULL, 0);
}

static int ports_changed(void)
{
    /* the root port's own connection: nothing, a device on it, or a hub. On a Pi 3B the hub is the
       LAN9514, soldered to the board; on a 3A+ or Zero 2 W it is whatever is plugged into the one
       port, and pulling it out is a change like any other */
    int c = (rd(0x440) & 1u) != 0;                  /* HPRT: connect status */
    if (c != root_connected || !hid.root_hub_ports) {
        if (c != root_connected && hid.root_hub_ports)
            kprintf("[USB] root port: the hub was %s\n", c ? "plugged in" : "unplugged");
        if (c != root_connected)
            port_changes |= 1u;
        return c != root_connected;
    }
    /* every port, so that psmac_usbdisk.c learns of each that changed (until 20 November 2026 the
       first change ended the look) */
    int any = 0;
    for (unsigned k = 1; k <= hid.root_hub_ports && k < 32; k++) {
        uint32_t st;
        if (hub_port_status(k, &st) == 0 && (st & (1u << 16))) {      /* C_PORT_CONNECTION */
            kprintf("[USB] hub port %u: %s\n", k, st & 1u ? "connected" : "disconnected");
            port_changes |= 1u << k;
            any = 1;
        }
    }
    return any;
}

/* A keyboard plugged in at power-on did not work on the user's Pi 3 until it was unplugged and
   plugged back in (18 November): the enumeration at boot, a second or two after the firmware
   handed over, can catch a device that is not ready, and a device that fails enumeration makes no
   port change for ports_changed() to see. So the start is retried: while no keyboard or no mouse
   was found, four times six seconds apart (a Pi with only a keyboard just ends up with the
   keyboard); and at any time, a device whose polls have all failed for two seconds
   (no report, no NAK) is enumerated again. */
static unsigned retries;
static uint32_t last_ok[2], last_bad[2];
static int needs_retry(void)
{
    struct frf_native_hid_ep *ep[2] = { &hid.keyboard, &hid.mouse };
    uint8_t have[2] = { hid.have_keyboard, hid.have_mouse };
    int dead = 0;
    for (int i = 0; i < 2; i++) {
        uint32_t ok = ep[i]->success + ep[i]->nak, bad = ep[i]->faults;
        if (have[i] && ok == last_ok[i] && bad - last_bad[i] > 20)
            dead = 1;
        last_ok[i] = ok;
        last_bad[i] = bad;
    }
    if (dead) {
        kprintf("[USB] a device stopped answering: enumerating again\n");
        return 1;
    }
    /* four tries, six seconds apart, in the boot's first half minute, and only when a port of the
       hub had a device on it that did not come out as the keyboard or the mouse: with nothing
       plugged in (the first SE boots: "devices seen 1", the LAN9514's own network chip on its port
       1) another enumeration finds nothing more, and each one resets the network chip as well */
    unsigned seen = hid.root_hub_ports ? hid.ports_seen : (unsigned)root_connected;   /* no hub: the device on the root port */
    unsigned others = hid.root_hub_ports ? (unsigned)psmac_board_netchip_on_hub() : 0;
    others += psmac_usbdisk_seen();             /* USB flash drives found (psmac_usbdisk.c) are not a keyboard that failed */
    if ((!hid.have_keyboard || !hid.have_mouse) && seen > hid.have_keyboard + hid.have_mouse + others
        && retries < 12 && ++retries % 3 == 0)
        return 1;
    return 0;
}

/* core 1 (psmac_usbdisk.c): a stick stopped answering; core 3 starts a fresh enumeration, which
   resets every device on the bus, at its next turn */
static const char *volatile asked_why;
void psmac_usb_ask_enumeration(const char *why) { __atomic_store_n(&asked_why, why, __ATOMIC_RELEASE); }

void psmac_usb_poll(void)
{
    uint64_t t;
    const char *asked;
    if (!__atomic_load_n(&usb_on, __ATOMIC_ACQUIRE))
        return;
    if ((asked = __atomic_exchange_n(&asked_why, (const char *)0, __ATOMIC_ACQ_REL))) {
        kprintf("[USB] %s: enumerating again\n", asked);
        enumerate_elsewhere(asked);
        return;
    }
    t = now();
    if (t >= next_check) {
        next_check = t + 2 * hz();
        if (ports_changed()) {
            retries = 0;
            enumerate_elsewhere("plug or unplug");
            return;
        }
        if (needs_retry()) {
            enumerate_elsewhere("retry");
            return;
        }
    }
    /* frf_native_hid_poll counts its calls as milliseconds */
    if (t < next_poll)
        return;
    if (t - next_poll > hz() / 10)
        next_poll = t;                          /* far behind (enumeration, a long pass): no burst */
    next_poll += hz() / 1000;
    frf_native_hid_poll(&ops, &hid);
}

/* for the heartbeat (every five seconds): a line every thirty */
void psmac_usb_report(void)
{
    static unsigned beat;
    psmac_usbdisk_report();
    if (!usb_on || beat++ % 6)
        return;
    kprintf("[USB]   %s, %u enumeration%s, kb %u (ok %u nak %u err %u), mouse %u (ok %u nak %u err %u), packets %u timeouts %u\n",
            use_fifo ? "FIFO" : "DMA", enumerations, enumerations == 1 ? "" : "s", hid.have_keyboard, hid.keyboard.success,
            hid.keyboard.nak, hid.keyboard.faults, hid.have_mouse, hid.mouse.success, hid.mouse.nak, hid.mouse.faults,
            hid.stats.attempts, hid.stats.timeouts);
}

/* for psmac_usbnet.c: 0 until the bus is up, else its generation, with the root hub's port count,
   the root port's speed (0 high, 1 full) and whether the DMA transport runs (the bulk path needs it) */
uint32_t psmac_usb_bus(unsigned *hub_ports, int *speed, int *dma)
{
    if (!__atomic_load_n(&usb_on, __ATOMIC_ACQUIRE))
        return 0;
    *hub_ports = hid.root_hub_ports;
    *speed = root_speed;
    *dma = !use_fifo;
    return __atomic_load_n(&bus_gen, __ATOMIC_ACQUIRE);
}
const struct frf_dwc2_ops *psmac_usb_ops(void) { return &ops; }
