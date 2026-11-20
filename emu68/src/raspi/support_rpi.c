/*
    Copyright © 2019 Michal Schulz <michal.schulz@gmx.de>
    https://github.com/michalsc

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <stdint.h>
#include <stdarg.h>

#include "devicetree.h"
#include "support_rpi.h"
#include "mmu.h"
#include "tlsf.h"
#include "spinlock.h"

#ifdef PISTORM_ANY_MODEL
#include "ps_protocol.h"
#endif

static int serial_up = 0;

#define ARM_PERIIOBASE ((intptr_t)io_base)

#ifdef PISTORM_ANY_MODEL

uint8_t *q_buffer;
volatile uint64_t q_head;
volatile uint64_t q_tail;
spinlock_t q_lock;
#define Q_SIZE (16*1024*1024)

void q_push(uint8_t data)
{
    spinlock_acquire(&q_lock);

    while(q_tail + Q_SIZE <= q_head)
        __asm__ volatile("yield");

    q_buffer[q_head & (Q_SIZE - 1)] = data;
    __sync_add_and_fetch(&q_head, 1);

    spinlock_release(&q_lock);
}

uint8_t q_pop()
{
    while (q_tail == q_head) {
        __asm__ volatile("wfe");
    }

    uint8_t data = q_buffer[q_tail & (Q_SIZE - 1)];
    __sync_add_and_fetch(&q_tail, 1);
    return data;
}

int redirect = 0;
int fast_serial = 0;

void putByte(void *io_base, char chr)
{
    (void)io_base;

    if (redirect)
    {
        if (chr == '\n')
            q_push('\r');
        q_push(chr);
    }
    else
    {
        if (fast_serial) {
            if (chr == '\n')
                fastSerial_putByte('\r');
            fastSerial_putByte(chr);
        }
        else {
            if (chr == '\n')
                bitbang_putByte('\r');
            bitbang_putByte(chr);
        }
    }
}

void serial_writer()
{
    redirect = 1;

    if (fast_serial) {
        while(1) {
            fastSerial_putByte(q_pop());
        }
    }
    else {
        while(1) {
            bitbang_putByte(q_pop());
        }
    }
}

#else

static inline void waitSerOUT(void *io_base)
{
    while(1)
    {
       if ((rd32le(PL011_0_BASE + PL011_FR) & PL011_FR_TXFF) == 0) break;
    }
}

static inline void putByte(void *io_base, char chr)
{
    if (serial_up)
    {
        waitSerOUT(io_base);

        if (chr == '\n')
        {
            wr32le(PL011_0_BASE + PL011_DR, '\r');
            waitSerOUT(io_base);
        }
        wr32le(PL011_0_BASE + PL011_DR, (uint8_t)chr);
        waitSerOUT(io_base);
    }
}

#endif

#undef ARM_PERIIOBASE
#ifdef PSMAC
#define ARM_PERIIOBASE 0x3f000000
#else
#define ARM_PERIIOBASE 0xf2000000
#endif

volatile unsigned char print_lock = 0;

void __printf_chk(int, const char *restrict format, ...)
{
    va_list v;
    va_start(v, format);

    while (__atomic_test_and_set(&print_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile("yield");

    vkprintf_pc(putByte, (void *)ARM_PERIIOBASE, format, v);

    __atomic_clear(&print_lock, __ATOMIC_RELEASE);

    va_end(v);
}


void __fprintf_chk(void *, int, const char *restrict format, ...)
{
    (void)format;

    va_list v;
    va_start(v, format);

    while (__atomic_test_and_set(&print_lock, __ATOMIC_ACQUIRE))
        __asm__ volatile("yield");

    vkprintf_pc(putByte, (void *)ARM_PERIIOBASE, format, v);

    __atomic_clear(&print_lock, __ATOMIC_RELEASE);

    va_end(v);
}

void kprintf(const char * restrict format, ...)
{
    va_list v;
    va_start(v, format);

    while(__atomic_test_and_set(&print_lock, __ATOMIC_ACQUIRE)) __asm__ volatile("yield");

    vkprintf_pc(putByte, (void*)ARM_PERIIOBASE, format, v);

    __atomic_clear(&print_lock, __ATOMIC_RELEASE);

    va_end(v);
}

void vkprintf(const char * restrict format, va_list args)
{
    while(__atomic_test_and_set(&print_lock, __ATOMIC_ACQUIRE)) __asm__ volatile("yield");

    vkprintf_pc(putByte, (void*)ARM_PERIIOBASE, format, args);

    __atomic_clear(&print_lock, __ATOMIC_RELEASE);
}

/* status register flags */

#define MBOX_TX_FULL (1UL << 31)
#define MBOX_RX_EMPTY (1UL << 30)
#define MBOX_CHANMASK 0xF

/* VideoCore tags used. */

#define VCTAG_GET_ARM_MEMORY     0x00010005
#define VCTAG_GET_CLOCK_RATE     0x00030002

#define VCCLOCK_PIXEL            9

/*----------------------------------------------------------------------------*/

/* The VideoCore mailbox has one reply FIFO for everyone: this file's calls (CPU0 and CPU2,
   through FBReq), the Macintosh host layer's WiFi and Bluetooth modules on CPU1 (alice_net.c,
   mbox_tag_) and, on the Amiga, the 68k's mailbox.resource. Without a lock, one caller's
   receive loop took the other's answer, which only matched the channel: on the first Pi 3B
   the 68k's RESET remade the VideoCore buffer while the WiFi asked for the board revision,
   psmac_vc_setup read FBReq before the firmware had filled it, and the screen went black for
   good (the firmware then scanned out a buffer nobody drew into). One lock held from send to
   receive, and the reply matched by its buffer address as well as its channel. */
spinlock_t vc_mbox_lock;
spinlock_t psmac_gpfsel_lock;           /* GPFSEL read-modify-writes from different cores: WiFi pins, Bluetooth pins, the SD card's (psmac_card.c) */
static uint32_t mbox_sent;

static uint32_t mbox_recv(uint32_t channel)
{
	volatile uint32_t *mbox_read = (uint32_t*)(ARM_PERIIOBASE + 0xB880);
	volatile uint32_t *mbox_status = (uint32_t*)(ARM_PERIIOBASE + 0xB898);
	uint32_t response, status;

	do
	{
		do
		{
			status = LE32(*mbox_status);
			dsb();
		}
		while (status & MBOX_RX_EMPTY);

		dmb();
		response = LE32(*mbox_read);
		dmb();
	}
	while ((response & MBOX_CHANMASK) != channel || (response & ~MBOX_CHANMASK) != (mbox_sent & ~MBOX_CHANMASK));

	spinlock_release(&vc_mbox_lock);
	return (response & ~MBOX_CHANMASK);
}

/*----------------------------------------------------------------------------*/

static void mbox_send(uint32_t channel, uint32_t data)
{
	volatile uint32_t *mbox_write = (uint32_t*)(ARM_PERIIOBASE + 0xB8A0);
	volatile uint32_t *mbox_status = (uint32_t*)(ARM_PERIIOBASE + 0xB898);
	uint32_t status;

#ifdef PSMAC
	/* The request's address as the VideoCore sees it: through its uncached alias (0xC0000000),
	   as Circle (BUS_ADDRESS) and the Linux firmware driver hand it over on a Pi 2, 3 and 4. A
	   bare physical address is the VideoCore's L1- and L2-cached view of the same memory: the
	   firmware may read a stale copy of the request, or leave its answer in its own cache for a
	   while. QEMU never models that cache, and its GPU bus has the same aliases (20 November 2026,
	   for the HDMI screen and audio: docs/08 item 73). The reply carries the same value back. */
	data = (data & 0x3fffffffu) | 0xC0000000u;
#endif
	data &= ~MBOX_CHANMASK;
	data |= channel & MBOX_CHANMASK;
	spinlock_acquire(&vc_mbox_lock);
	mbox_sent = data;

	do
	{
		status = LE32(*mbox_status);
		dsb();
	}
	while (status & MBOX_TX_FULL);

	dmb();
	*mbox_write = LE32(data);
}

//uint32_t FBReq[128] __attribute__((aligned(16)));
uint32_t *FBReq = (uint32_t *)0xffffff9000001000;

uint32_t get_clock_rate(uint32_t clock_id)
{
    FBReq[0] = LE32(4*8);       // Length
    FBReq[1] = 0;               // Request
    FBReq[2] = LE32(0x00030002);// GetClockRate
    FBReq[3] = LE32(8);
    FBReq[4] = 0;
    FBReq[5] = LE32(clock_id);
    FBReq[6] = 0;
    FBReq[7] = 0;

    arm_flush_cache((intptr_t)FBReq, 32);
    mbox_send(8, mmu_virt2phys((intptr_t)FBReq));
    mbox_recv(8);

    return LE32(FBReq[6]);
}

uint32_t get_max_clock_rate(uint32_t clock_id)
{
    FBReq[0] = LE32(4*8);       // Length
    FBReq[1] = 0;               // Request
    FBReq[2] = LE32(0x00030004);// GetMaxClockRate
    FBReq[3] = LE32(8);
    FBReq[4] = 0;
    FBReq[5] = LE32(clock_id);
    FBReq[6] = 0;
    FBReq[7] = 0;

    arm_flush_cache((intptr_t)FBReq, 32);
    mbox_send(8, mmu_virt2phys((intptr_t)FBReq));
    mbox_recv(8);

    return LE32(FBReq[6]);
}

uint32_t get_min_clock_rate(uint32_t clock_id)
{
    FBReq[0] = LE32(4*8);       // Length
    FBReq[1] = 0;               // Request
    FBReq[2] = LE32(0x00030007);// GetClockRate
    FBReq[3] = LE32(8);
    FBReq[4] = 0;
    FBReq[5] = LE32(clock_id);
    FBReq[6] = 0;
    FBReq[7] = 0;

    arm_flush_cache((intptr_t)FBReq, 32);
    mbox_send(8, mmu_virt2phys((intptr_t)FBReq));
    mbox_recv(8);

    return LE32(FBReq[6]);
}

uint32_t set_clock_rate(uint32_t clock_id, uint32_t speed)
{
    FBReq[0] = LE32(4*9);       // Length
    FBReq[1] = 0;               // Request
    FBReq[2] = LE32(0x00038002);// SetClockRate
    FBReq[3] = LE32(12);
    FBReq[4] = 0;
    FBReq[5] = LE32(clock_id);
    FBReq[6] = LE32(speed);
    FBReq[7] = 0;
    FBReq[8] = 0;

    arm_flush_cache((intptr_t)FBReq, 36);
    mbox_send(8, mmu_virt2phys((intptr_t)FBReq));
    mbox_recv(8);

    return LE32(FBReq[6]);
}

void get_vc_memory(void **base, uint32_t *size)
{
    FBReq[0] = LE32(4*8);
    FBReq[1] = 0;
    FBReq[2] = LE32(0x00010006);
    FBReq[3] = LE32(8);
    FBReq[4] = 0;
    FBReq[5] = 0;
    FBReq[6] = 0;
    FBReq[7] = 0;

    arm_flush_cache((intptr_t)FBReq, 32);
    mbox_send(8, mmu_virt2phys((intptr_t)FBReq));
    mbox_recv(8);

    if (base) {
        *base = (void *)(intptr_t)LE32(FBReq[5]);
    }

    if (size) {
        *size = LE32(FBReq[6]);
    }
}

struct Size get_display_size()
{
    struct Size sz;

    FBReq[0] = LE32(4*8);
    FBReq[1] = 0;
    FBReq[2] = LE32(0x00040003);
    FBReq[3] = LE32(8);
    FBReq[4] = 0;
    FBReq[5] = 0;
    FBReq[6] = 0;
    FBReq[7] = 0;

    arm_flush_cache((intptr_t)FBReq, 32);
    mbox_send(8, mmu_virt2phys((intptr_t)FBReq));
    mbox_recv(8);

    sz.width = LE32(FBReq[5]);
    sz.height = LE32(FBReq[6]);

    return sz;
}

uint32_t enable_unicam_domain()
{
    FBReq[0] = LE32(4 * 8);      // Length
    FBReq[1] = 0;                // Request
    FBReq[2] = LE32(0x00038030); // SetClockRate
    FBReq[3] = LE32(8);
    FBReq[4] = 0;
    FBReq[5] = LE32(14); // unicam1
    FBReq[6] = LE32(1);
    FBReq[7] = 0;

    arm_flush_cache((intptr_t)FBReq, 32);
    mbox_send(8, mmu_virt2phys((intptr_t)FBReq));
    mbox_recv(8);

    return LE32(FBReq[6]);
}

#ifdef PSMAC
/* The second display (psmac/psmac_display.c): the VideoCore frame buffer made again at the Macintosh
   screen's size and depth.

   How the buffer is asked for, compared with the two references (20 November 2026, docs/08 item
   73). Upstream Emu68's init_display sends physical size, virtual size, depth 16, "allocate buffer"
   (alignment 64) and "get pitch" in one message, through the request's physical address. Circle's
   CBcmFrameBuffer::Initialize sends physical size, virtual size, depth, virtual offset (0, 0),
   "allocate buffer" and "get pitch" in one message, through the VideoCore's uncached alias, and
   writes 32-bit pixels with an alpha byte. This one follows Circle's order, adds "set alpha mode:
   ignored" for a 32-bit buffer (the kernel's pixels carry 0 in that byte; the firmware's default
   for the byte is a config.txt matter, framebuffer_ignore_alpha) and asks for the pixel order.
   Then a second message that only reads (psmac_vc_query) says what the firmware actually has, so
   the log shows the buffer the HVS scans out and not just the answer to the request. */
uint32_t psmac_vc_pixel_order = 1;          /* what the firmware says the byte order is: 1 RGB, 0 BGR (tag 0x40006) */
uint32_t psmac_vc_alpha = ~0u;              /* the answer to "set alpha mode" (2: ignored), ~0 if not asked */
uint32_t psmac_vc_setup(uint32_t w, uint32_t h, uint32_t depth, void **fb, uint32_t *pitch_out)
{
    int c = 1, pos_base = 0, pos_pitch = 0, pos_depth = 0, pos_order = 0, pos_alpha = 0;
    FBReq[c++] = 0;
    FBReq[c++] = LE32(0x48003); FBReq[c++] = LE32(8); FBReq[c++] = 0; FBReq[c++] = LE32(w); FBReq[c++] = LE32(h);
    FBReq[c++] = LE32(0x48004); FBReq[c++] = LE32(8); FBReq[c++] = 0; FBReq[c++] = LE32(w); FBReq[c++] = LE32(h);
    FBReq[c++] = LE32(0x48005); FBReq[c++] = LE32(4); FBReq[c++] = 0; pos_depth = c; FBReq[c++] = LE32(depth);
    FBReq[c++] = LE32(0x48009); FBReq[c++] = LE32(8); FBReq[c++] = 0; FBReq[c++] = 0; FBReq[c++] = 0;       /* virtual offset 0, 0 */
    if (depth == 32) { FBReq[c++] = LE32(0x48007); FBReq[c++] = LE32(4); FBReq[c++] = 0; pos_alpha = c; FBReq[c++] = LE32(2); }
    FBReq[c++] = LE32(0x40001); FBReq[c++] = LE32(8); FBReq[c++] = 0; pos_base = c; FBReq[c++] = LE32(64); FBReq[c++] = 0;
    FBReq[c++] = LE32(0x40008); FBReq[c++] = LE32(4); FBReq[c++] = 0; pos_pitch = c; FBReq[c++] = 0;
    FBReq[c++] = LE32(0x40006); FBReq[c++] = LE32(4); FBReq[c++] = 0; pos_order = c; FBReq[c++] = 0;
    FBReq[c++] = 0;
    FBReq[0] = LE32(c << 2);
    arm_flush_cache((intptr_t)FBReq, c * 4);
    mbox_send(8, mmu_virt2phys((intptr_t)FBReq));
    mbox_recv(8);
    arm_dcache_invalidate((intptr_t)FBReq, c * 4);
    *fb = (void *)(intptr_t)(LE32(FBReq[pos_base]) & 0x3fffffff);
    *pitch_out = LE32(FBReq[pos_pitch]);
    psmac_vc_pixel_order = LE32(FBReq[pos_order]) & 1;
    psmac_vc_alpha = pos_alpha ? LE32(FBReq[pos_alpha]) : ~0u;
    return LE32(FBReq[pos_depth]);              /* the depth the firmware (or QEMU) actually set */
}
/* What the firmware has now, asked without changing anything: physical and virtual size, depth,
   pitch, virtual offset and pixel order. Returns 0 if the firmware did not answer. */
int psmac_vc_query(uint32_t out[8])
{
    static const uint32_t tags[6] = { 0x40003, 0x40004, 0x40005, 0x40008, 0x40009, 0x40006 };
    static const uint8_t words[6] = { 2, 2, 1, 1, 2, 1 };
    int c = 1, pos[6];
    FBReq[c++] = 0;
    for (int i = 0; i < 6; i++) {
        FBReq[c++] = LE32(tags[i]); FBReq[c++] = LE32(4u * words[i]); FBReq[c++] = 0;
        pos[i] = c;
        for (int k = 0; k < words[i]; k++) FBReq[c++] = 0;
    }
    FBReq[c++] = 0;
    FBReq[0] = LE32(c << 2);
    arm_flush_cache((intptr_t)FBReq, c * 4);
    mbox_send(8, mmu_virt2phys((intptr_t)FBReq));
    mbox_recv(8);
    arm_dcache_invalidate((intptr_t)FBReq, c * 4);
    if (LE32(FBReq[1]) != 0x80000000u) return 0;
    out[0] = LE32(FBReq[pos[0]]); out[1] = LE32(FBReq[pos[0] + 1]);    /* physical width, height */
    out[2] = LE32(FBReq[pos[1]]); out[3] = LE32(FBReq[pos[1] + 1]);    /* virtual width, height */
    out[4] = LE32(FBReq[pos[2]]);                                       /* depth */
    out[5] = LE32(FBReq[pos[3]]);                                       /* pitch */
    out[6] = LE32(FBReq[pos[4]]) | LE32(FBReq[pos[4] + 1]) << 16;      /* virtual offset x | y << 16 */
    out[7] = LE32(FBReq[pos[5]]);                                       /* pixel order */
    return 1;
}
#endif

void init_display(struct Size dimensions, void **framebuffer, uint32_t *pitch)
{
    int c = 1;
    int pos_buffer_base = 0;
    int pos_buffer_pitch = 0;

    FBReq[c++] = 0;                 // Request
    FBReq[c++] = LE32(0x48003);     // SET_RESOLUTION
    FBReq[c++] = LE32(8);
    FBReq[c++] = 0;
    FBReq[c++] = LE32(dimensions.width);
    FBReq[c++] = LE32(dimensions.height);

    FBReq[c++] = LE32(0x48004);          // Virtual resolution: duplicate physical size...
    FBReq[c++] = LE32(8);
    FBReq[c++] = 0;
    FBReq[c++] = LE32(dimensions.width);
    FBReq[c++] = LE32(dimensions.height);

    FBReq[c++] = LE32(0x48005);   // Set depth
    FBReq[c++] = LE32(4);
    FBReq[c++] = LE32(0);
    FBReq[c++] = LE32(16);

    FBReq[c++] = LE32(0x40001); // Allocate buffer
    FBReq[c++] = LE32(8);
    FBReq[c++] = LE32(0);
    pos_buffer_base = c;
    FBReq[c++] = LE32(64);
    FBReq[c++] = LE32(0);

    FBReq[c++] = LE32(0x40008); // Get pitch
    FBReq[c++] = LE32(4);
    FBReq[c++] = LE32(0);
    pos_buffer_pitch = c;
    FBReq[c++] = LE32(0);

    FBReq[c++] = 0;

    FBReq[0] = LE32(c << 2);

    arm_flush_cache((intptr_t)FBReq, c * 4);
    mbox_send(8, mmu_virt2phys((intptr_t)FBReq));
    mbox_recv(8);
    arm_dcache_invalidate((intptr_t)FBReq, c*4);

    uint32_t _base = LE32(FBReq[pos_buffer_base]);
    uint32_t _pitch = LE32(FBReq[pos_buffer_pitch]);

    if ((_base & 0xc0000000) == 0x40000000)
    {
        // Cached buffer
    }
    else if ((_base & 0xc0000000) == 0xc0000000)
    {
        // Uncached buffer
    }
    _base &= ~0xc0000000;

    if (framebuffer)
        *framebuffer = (void*)(intptr_t)_base;

    if (pitch)
        *pitch = _pitch;
}

#define PL011_ICR_FLAGS (PL011_ICR_RXIC|PL011_ICR_TXIC|PL011_ICR_RTIC|PL011_ICR_FEIC|PL011_ICR_PEIC|PL011_ICR_BEIC|PL011_ICR_OEIC|PL011_ICR_RIMIC|PL011_ICR_CTSMIC|PL011_ICR_DSRMIC|PL011_ICR_DCDMIC)

#define DEF_BAUD 115200

#define PL011_DIVCLOCK(baud, clock)     ((clock * 4) / baud)
#define PL011_BAUDINT(baud, clock)      ((PL011_DIVCLOCK(baud, clock) & 0xFFFFFFC0) >> 6)
#define PL011_BAUDFRAC(baud, clock)     ((PL011_DIVCLOCK(baud, clock) & 0x0000003F) >> 0)

#define GPIO_BASE                                       (ARM_PERIIOBASE + 0x200000)
#define GPFSEL1                                         (GPIO_BASE + 0x4)
#define GPPUD                                           (GPIO_BASE + 0x94)
#define GPPUDCLK0                                       (GPIO_BASE + 0x98)

#ifdef PISTORM_ANY_MODEL

void setup_serial()
{
    of_node_t *e = NULL;
    int async = 0;

    serial_up = 1;

    e = dt_find_node("/emu68/diag");
    if (e)
    {
        if (dt_find_property(e, "fast-serial")) {
            fast_serial = 1;
            fastSerial_init();
        }
        else {
            fast_serial = 0;
        }

        if (dt_find_property(e, "async-log")) {
            async = 1;
        }
    }

    if (async) {
        q_buffer = tlsf_malloc(tlsf, Q_SIZE);
        q_head = 0;
        q_tail = 0;
    }
}

#else

void setup_serial()
{
    unsigned int        uartvar;
    unsigned const int uartbaud = 115200;
    unsigned int uartclock;
    unsigned int uartdivint;
    unsigned int uartdivfrac;

    uartclock = get_clock_rate(2);

    wr32le(PL011_0_BASE + PL011_CR, 0);

    uartvar = rd32le(GPFSEL1);
    uartvar &= ~(7<<12);                        // TX on GPIO14
    uartvar |= 4<<12;                           // alt0
    uartvar &= ~(7<<15);                        // RX on GPIO15
    uartvar |= 4<<15;                           // alt0
    wr32le(GPFSEL1, uartvar);

    /* Disable pull-ups and pull-downs on rs232 lines */
    wr32le(GPPUD, 0);

    for (uartvar = 0; uartvar < 150; uartvar++) __asm__ volatile ("nop");

    wr32le(GPPUDCLK0, (1 << 14)|(1 << 15));

    for (uartvar = 0; uartvar < 150; uartvar++) __asm__ volatile ("nop");

    wr32le(GPPUDCLK0, 0);

    wr32le(PL011_0_BASE + PL011_ICR, PL011_ICR_FLAGS);
    uartdivint = PL011_BAUDINT(uartbaud, uartclock);
    wr32le(PL011_0_BASE + PL011_IBRD, uartdivint);
    uartdivfrac = PL011_BAUDFRAC(uartbaud, uartclock);
    wr32le(PL011_0_BASE + PL011_FBRD, uartdivfrac);
    wr32le(PL011_0_BASE + PL011_LCRH, PL011_LCRH_WLEN8|PL011_LCRH_FEN);           // 8N1, Fifo enabled
    wr32le(PL011_0_BASE + PL011_CR, PL011_CR_UARTEN|PL011_CR_TXE|PL011_CR_RXE);   // enable the uart, tx and rx

    for (uartvar = 0; uartvar < 150; uartvar++) __asm__ volatile ("nop");

    serial_up = 1;
}

#endif

bool is_serial_up()
{
    return serial_up;
}
