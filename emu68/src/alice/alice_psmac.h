/* SPDX-License-Identifier: GPL-2.0-only
 *
 * alice_psmac.h — what the Alice68 service modules need to know about the Macintosh build.
 *
 * History. Alice68 is the user's Raspberry Pi 4 Amiga kernel: Emu68 plus a virtual AGA chipset,
 * with the Pi's WiFi, Ethernet, Bluetooth and USB driven on a spare ARM core and handed to the
 * 68k through "boards" in shared memory. PiStorm-SE ("Aurora68", the Macintosh variant of the same
 * kernel) reuses those modules as they are: the hardware does not care which 68k OS consumes the
 * frames. What differs is the plumbing, and this header plus alice_psmac.c carry all of it:
 *   - the Pi peripherals are reached through a kernel-space mapping (Alice68 maps them at
 *     0xF2000000, the Mac build at 0x3F000000 for the 68k and high up for the kernel's cores,
 *     see src/raspi/start_rpi64.c and psmac/psmac.h);
 *   - there is no Zorro autoconfig: the boards are fixed regions inside the NuBus slot-$9 window
 *     (psmac/psmb.h), announced to the Mac through the window's service table;
 *   - the 68k is interrupted by setting the board's irq flag; the Mac drivers poll from VBL
 *     tasks until the virtual slot interrupt exists (docs/07);
 *   - keyboard and mouse reports become events in an input ring the Mac side injects as ADB-style
 *     events, instead of the Amiga's virtual CIA/JOYDAT;
 *   - firmware files are preloaded by the kernel instead of handed over by the 68k.
 */
#ifndef ALICE_PSMAC_H
#define ALICE_PSMAC_H
#include <stdint.h>

/* A register address the device tree hands out (start_rpi64.c rewrites the ranges to the low
   window) moved to the kernel-space twin of that window; anything else is returned as it is. */
static inline unsigned long alice_periph_hi(unsigned long low_va)
{
    return (low_va >= 0x3f000000ul && low_va < 0x4f000000ul) ? low_va + (0xffffffe800000000ul - 0x3f000000ul) : low_va;
}
#define ALICE_PERI              0xffffffe800000000ull   /* the kernel-space mapping of the Pi peripherals (psmac/psmac.h: PSMAC_PERIPH_HI);
                                                           the low one at 0x3F000000 vanishes for a moment at every Mac MMU switch */

/* where the Alice68 boards live inside the slot-$9 window (psmb.h: 0x20000..0x7FFFF is free) */
#define ALICE_NET_WINDOW_OFF    0x20000u        /* 128 KB network board (alice_net.h layout) */
#define ALICE_USB_WINDOW_OFF    0x40000u        /* 64 KB USB pass-through board (alice68_usbpt.h) */
#define ALICE_STATUS_WINDOW_OFF 0x50000u        /* 4 KB status page (alice_status.h) */
#define ALICE_INPUT_WINDOW_OFF  0x51000u        /* 4 KB input event ring (below) */

/* Input ring: keyboard and mouse reports from USB and Bluetooth, for the Mac side to inject.
   Written by the service core, read by the Mac's input driver; head/tail count up for ever. */
#define ALICE_IN_MAGIC          0x41364949u     /* 'A6II' */
#define ALICE_IN_SLOTS          64
#define ALICE_IN_KEY            1               /* a = USB usage page/usage (boot report byte), b = 1 down / 0 up, c = modifiers */
#define ALICE_IN_MOUSE          2               /* a = dx, b = dy (signed), c = buttons */
#define ALICE_IN_WHEEL          3               /* a = wheel, b = pan */
#define ALICE_IN_MOUSEABS       4               /* a = x, b = y (global, the main screen at 0,0), c = buttons or -1: the remote (docs/13) */
#define ALICE_IN_MACKEY         5               /* a = Mac virtual key code, b = 1 down / 0 up, c = modifiers: the remote */
struct alice_input_event { uint32_t type; int32_t a, b, c; };
struct alice_input_ring {
    uint32_t magic, version, slots;
    uint32_t attached;                          /* Mac: 1 once .PiInput has opened and will take events */
    uint32_t head;                              /* service core: events put */
    uint32_t tail;                              /* Mac: events taken */
    uint32_t dropped, pad;
    struct alice_input_event ev[ALICE_IN_SLOTS];
};

void alice_net_attach(void *base);              /* alice_net.c: use this memory as the board */
void alice_net_reset(void);                     /* alice_net.c: the 68k was reset */
int  alice_fw_preload(const char *name, const uint8_t *data, uint32_t len);
void alice_status_attach(void *page);           /* alice_status.c */
void alice_net_poll(void);
void psmac_alice_input_test(uint32_t seconds);  /* alice_psmac.c: scripted mouse and keys for the testbed */
void psmac_input_put(uint32_t type, int32_t a, int32_t b, int32_t c);   /* alice_psmac.c: one event into the ring, as given (the remote) */
int  psmac_input_attached(void);                /* .PiInput has opened and takes events */
uint32_t psmac_input_backlog(void);             /* events in the ring .PiInput has not taken */

#endif
