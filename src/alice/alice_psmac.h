/* SPDX-License-Identifier: GPL-2.0-only
 *
 * alice_psmac.h — what the Alice68 service modules need to know about the Macintosh build.
 *
 * History. Alice68 is the user's Raspberry Pi 4 Amiga kernel: Emu68 plus a virtual AGA chipset,
 * with the Pi's WiFi, Ethernet, Bluetooth and USB driven on a spare ARM core and handed to the
 * 68k through "boards" in shared memory. PiStorm-SE ("Aurora68", the Macintosh variant of the same
 * kernel) reuses those modules as they are: the hardware does not care which 68k OS consumes the
 * frames. What differs is the plumbing, and this header plus alice_psmac.c carry all of it:
 *   - the Pi peripherals sit at 0x3F000000 in Emu68's map on the Mac build (Alice68 maps them at
 *     0xF2000000, see src/raspi/start_rpi64.c);
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

#define ALICE_PERI              0x3f000000u     /* Pi peripheral window in the psmac Emu68 map */

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
struct alice_input_event { uint32_t type; int32_t a, b, c; };
struct alice_input_ring {
    uint32_t magic, version, slots, reserved;
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

#endif
