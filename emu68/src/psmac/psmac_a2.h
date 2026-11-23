/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_a2.h — the Aurora68 Apple II Card's window: the block the Mac and the card share (docs/20-apple-ii-card.md).
 *
 * Backstory (23 November 2026). Aurora65 (a project of its own beside this one, like Aurora86 for the PC card) is an
 * 8-bit Apple II (II, II+, IIe, enhanced IIe, IIc), built as a library for this kernel (libaurora65.a, aurora65.h).
 * This card is how the Mac sees it, as the Apple IIe Card saw the Macintosh LC's: a NuBus card in slot $E, "Aurora68
 * Apple II Card", whose driver ".A68A2" lives in its declaration ROM, and a program, "Aurora68 Apple II", that shows
 * the Apple's screen in an ordinary window, passes the keyboard to it and says where its window is. The card draws
 * the Apple's screen into the Mac's own frame buffer itself, inside that window's visible region (psmac_wclip.c, as
 * the PC card and the video decoder card do); the 68k never touches a pixel.
 *
 * The window is 1 MiB at $FE000000 (32-bit) and $FEE00000 (24-bit: the slot's $E00000), the declaration ROM in its
 * top 16 KiB; every word big-endian. The block's layout is the PC card's (psmac_pc.h) wherever the two have the same
 * thing, so that the drivers (mac/a68pc.s, mac/a68apple.s) are the same code and the programs share their habits:
 * the card's half and the Mac's are apart, the card never writes the Mac's half, the Mac never the card's.
 *
 * How the Mac uses it:
 *  1. Find the card (Slot Manager: the sResource of type {$7050 'pP', 5, 1, 'A2'}) or open ".A68A2"; check magic.
 *  2. The place: screen (A68A_SCREEN_*), the window's content rectangle in that screen's pixels, visible, and (from
 *     the program itself) clip_tag = A68A_CLIP_TAG + the new geom_seq with vis_rgn = the window's visRgn handle; then
 *     geom_seq + 1. Again whenever the window moves, is resized, shown, hidden or brought forward.
 *  3. front: 1 while the window is the Mac's front window (activate events), 0 otherwise: the guest scheduler
 *     (psmac_guest.c) gives the front window's machine the shared core first.
 *  4. Commands: cmd and arg0/arg1, then cmd_seq + 1; done_seq follows when the card has carried it out.
 *  5. Input while the window is in front: events into the ring at in_head (the card takes them at in_tail): Mac key
 *     downs (the character code and the virtual key code, with the modifiers), key ups, modifier changes, the
 *     mouse as the joystick.
 *  6. Status: state, model, message, speed, load; mode, nat_w, nat_h (the picture's natural size, for the zoom box).
 */
#ifndef PSMAC_A2_H
#define PSMAC_A2_H
#include <stdint.h>

#define A68A_MAGIC        0x41363541u        /* 'A65A' (Aurora65's DESIGN, "The card in Aurora68") */
#define A68A_VERSION      1u
#define A68A_WIN_SIZE     0x00100000u        /* 1 MiB, the declaration ROM in its top 16 KiB */
#define A68A_OFF_BLOCK    0x00000u
#define A68A_OFF_MESSAGE  0x00100u
#define A68A_OFF_INPUT    0x00400u           /* A68A_IN_SLOTS x struct a68a_in */
#define A68A_OFF_TEXT     0x01000u           /* 80 x 24 cells: the ASCII character in the low byte, A68A_ATTR_* in the high */
#define A68A_OFF_DISKS    0x08000u           /* struct a68a_disks */
#define A68A_OFF_ROM      0xFC000u
#define A68A_ROM_SIZE     0x04000u
#define A68A_IN_SLOTS     64u

/* commands (cmd, then cmd_seq + 1) */
#define A68A_C_POWER_ON   1u                 /* the machine on (built at the first), or on again (a cold start) */
#define A68A_C_POWER_OFF  2u                 /* it stops and keeps its memory; every disk written back first */
#define A68A_C_RESET      3u                 /* arg0 0: Control-Reset (warm); 1: power cycled (cold, as Control-Open-Apple-Reset) */
#define A68A_C_PAUSE      4u
#define A68A_C_RESUME     5u
#define A68A_C_REDRAW     7u                 /* the whole window again (the program's update event) */
#define A68A_C_ATTACH     8u                 /* arg0 = image (index in the list), arg1 = unit (A68A_UNIT_*), or
                                                A68A_UNIT_AUTO: the first free unit of the image's kind */
#define A68A_C_DETACH     9u                 /* arg1 = unit, or A68A_UNIT_AUTO and arg0 = the image: ejected (written back) */
#define A68A_C_RESCAN     10u                /* the image list again (the SD card's root) */
#define A68A_C_MODEL      11u                /* arg0 = A68A_MODEL_*: the machine built again as that model, from its ROM */
#define A68A_C_NOWINDOW   12u                /* the program's window is gone (it quits): nothing drawn from now on, every key and
                                                button let go; done_seq says the card has stopped drawing (the PC card's
                                                A68P_C_NOWINDOW, 23 November 2026) */
#define A68A_UNIT_D1      0u                 /* the units (aurora65.h's A65_UNIT_*): the Disk II's drives (slot 6) */
#define A68A_UNIT_D2      1u
#define A68A_UNIT_HD1     2u                 /* Aurora65's block-device cards: slot 7's two units ... */
#define A68A_UNIT_HD2     3u
#define A68A_UNIT_HD3     4u                 /* ... and slot 5's */
#define A68A_UNIT_HD4     5u
#define A68A_UNITS        6u
#define A68A_UNIT_AUTO    0xFFFFFFFFu

#define A68A_MODEL_II     0u                 /* aurora65.h's A65_APPLE_* */
#define A68A_MODEL_IIPLUS 1u
#define A68A_MODEL_IIE    2u
#define A68A_MODEL_IIEE   3u                 /* the enhanced IIe: the default */
#define A68A_MODEL_IIC    4u
#define A68A_MODELS       5u

/* state */
#define A68A_S_OFF        0u                 /* not started (a2=on starts it with the Mac), or powered off */
#define A68A_S_STARTING   1u                 /* reading the ROM, building the machine */
#define A68A_S_RUNNING    2u
#define A68A_S_PAUSED     3u
#define A68A_S_HALTED     4u                 /* the processor stopped (a JAM opcode): Reset starts it again */
#define A68A_S_FAILED     5u                 /* error and message say why */

#define A68A_E_NOROM      2u                 /* no ROM for the model on the card (the owner's own dump: docs/20) */
#define A68A_E_NOMEM      3u                 /* no memory for the machine */
#define A68A_E_BUILD      4u                 /* the library refused the machine (a ROM of the wrong size ...): message says why */

#define A68A_SCR_TEXT     1u                 /* mode: aurora65.h's A65_SCR_* */
#define A68A_SCR_LORES    2u
#define A68A_SCR_HIRES    3u
#define A68A_SCR_DHIRES   4u

#define A68A_SCREEN_FIRST 0u                 /* the Mac's first screen: the SE's own on the SE board */
#define A68A_SCREEN_HDMI  1u                 /* the HDMI screen (slot $A) on the SE board */
#define A68A_CLIP_TAG     0x41320000u        /* 'A2' << 16, plus the geom_seq vis_rgn goes with */
/* prog, alive and nodraw: as the PC card's (psmac_pc.h, A68P_PROG_*, A68P_ALIVE_MS, A68P_ND_*; 23 November 2026, the owner's
   SE: the PC card drew through a quit program's freed visRgn handle; this card was made after its pattern) */
#define A68A_PROG_NONE    0u                 /* nothing said (an older program, a2.testwin): no heartbeat watched */
#define A68A_PROG_LIVE    1u                 /* the program runs and adds 1 to alive at every turn of its event loop */
#define A68A_PROG_GONE    2u                 /* the program has quit (or its ExitToShell patch ran): nothing drawn */
#define A68A_ALIVE_MS     1000u
#define A68A_ND_DRAWS     0u
#define A68A_ND_HIDDEN    1u
#define A68A_ND_GONE      2u
#define A68A_ND_STALE     3u
#define A68A_ND_RGN       4u
#define A68A_ND_NORGN     5u
#define A68A_VF_24BIT     1u                 /* vis_flags: the program runs in 24-bit mode */

#define A68A_SCALE_1      0u                 /* 1:1 (the default, the owner's): 280 x 192 for 40 columns and hires on one bit */
#define A68A_SCALE_FIT    1u                 /* scaled to the window (one bit: by the nearest pixel, dithered; deeper
                                                screens: by the largest whole number that fits) */
#define A68A_O_APPLESENSE 1u                 /* options: white on black, as the Apple draws it (default on one bit: black on white) */
#define A68A_O_COLOUR     2u                 /* options: hires drawn in colour and dithered on one bit (default: as a
                                                monochrome monitor shows it, the exact pixels) */
#define A68A_O_GROWBOX    8u                 /* options: the window's bottom right 15 x 15 (its grow box) is left to the Mac */

/* the input ring's events */
#define A68A_IN_KEYDOWN   1u                 /* code: the Mac's virtual key code, dx: its character code, modifiers */
#define A68A_IN_KEYUP     2u                 /* the key is let go (the IIe's "any key down") */
#define A68A_IN_MODS      3u                 /* modifiers changed without a key (Command and Option: Open-Apple, Solid-Apple) */
#define A68A_IN_JOY       4u                 /* dx, dy: paddles 0 and 1 (0-255); buttons bit 0: the mouse button */
#define A68A_IN_RELEASE   5u                 /* every key and button let go (the window went to the back) */

struct a68a_in {                             /* 16 bytes, as struct a68p_in */
    uint16_t type, code;
    uint16_t modifiers;                      /* the Mac event's modifiers */
    int16_t  dx, dy;
    uint16_t buttons;
    uint32_t pad;
};

struct a68a_block {
    /* the card's half: 0x00-0x7F */
    uint32_t magic, version, win_size;       /* A68A_MAGIC, A68A_VERSION (written last), A68A_WIN_SIZE */
    uint32_t done_seq;                       /* the last cmd_seq carried out */
    uint32_t state, error;                   /* A68A_S_*, A68A_E_* */
    uint32_t speed_pm;                       /* the Apple's time run per real time over the last second, per mille (1000: its own speed) */
    uint32_t load;                           /* per mille of its core's time in the Apple over the last second */
    uint32_t cycles_hi, cycles_lo;           /* the Apple's cycles since power on */
    uint32_t core;                           /* the Pi core it runs on: 1 or 2 */
    uint32_t model;                          /* A68A_MODEL_* */
    uint32_t screen_seq, drawn_seq;          /* the Apple's screen's change count, and the last one drawn */
    uint32_t draw_us;                        /* drawing, average over the last second */
    uint32_t mode, nat_w, nat_h;             /* A68A_SCR_*; the picture's natural size in the window (pixels) */
    uint32_t cols;                           /* 40 or 80 */
    uint32_t idle;                           /* 1 while the Apple waits for a key (Aurora65's idle) */
    uint32_t disk_busy;                      /* the Apple's disk writes in the last second */
    uint32_t in_tail;                        /* input events taken (the same place as the PC card's: the drivers share code) */
    uint32_t draws;                          /* pictures drawn since the start */
    uint32_t idle_pm;                        /* per mille of the last second the Apple's frames were skipped as idle */
    uint32_t disk_dirty;                     /* units with changes not yet back in their image files */
    uint32_t motor;                          /* bit n: unit n's drive turning (the Disk II's light) */
    uint32_t rom_known;                      /* 1: the ROM is one romcheck.py knows, 2: Aurora65's test ROM, 0: unknown */
    uint32_t nodraw;                         /* A68A_ND_*: why nothing is drawn now (0: the card draws) */
    uint32_t stalls;                         /* times drawing stopped because the program's alive stood still */
    uint32_t pad0[3];
    /* the Mac's half: 0x80-0xFF */
    uint32_t cmd_seq, cmd, arg0, arg1;       /* cmd and its arguments, then cmd_seq + 1 */
    uint32_t screen;                         /* A68A_SCREEN_* */
    int32_t  top, left, bottom, right;       /* the window's content in that screen's pixels, QuickDraw's order */
    uint32_t visible;                        /* 0: draw nothing (hidden, being dragged) */
    uint32_t geom_seq;                       /* + 1 after any of the above, clip_tag/vis_rgn/vis_flags, scale, options */
    uint32_t clip_tag, vis_rgn, vis_flags;   /* the window's visRgn handle (A68A_CLIP_TAG + geom_seq it goes with) */
    uint32_t scale;                          /* A68A_SCALE_* */
    int32_t  scroll_x, scroll_y;             /* 1:1: the picture's pixel at the window's top left */
    uint32_t options;                        /* A68A_O_* */
    uint32_t focus;                          /* 1: the window takes the keyboard (it is in front) */
    uint32_t in_head;                        /* input events put in */
    uint32_t front;                          /* 1: the program's window is the Mac's front window */
    uint32_t prog;                           /* A68A_PROG_* */
    uint32_t alive;                          /* + 1 at every turn of the program's event loop (A68A_PROG_LIVE) */
    uint32_t pad1[9];
    /* the card again: 0x100 */
    char     message[128];                   /* what the Apple is doing, or why it stopped, in words (ASCII) */
};
_Static_assert(sizeof(struct a68a_block) == 0x180, "a68a_block");
_Static_assert(__builtin_offsetof(struct a68a_block, in_tail) == 0x54, "a68a_block in_tail");
_Static_assert(__builtin_offsetof(struct a68a_block, cmd_seq) == 0x80, "a68a_block Mac half");
_Static_assert(__builtin_offsetof(struct a68a_block, screen) == 0x90, "a68a_block screen");
_Static_assert(__builtin_offsetof(struct a68a_block, geom_seq) == 0xA8, "a68a_block geom_seq");
_Static_assert(__builtin_offsetof(struct a68a_block, scale) == 0xB8, "a68a_block scale");
_Static_assert(__builtin_offsetof(struct a68a_block, focus) == 0xC8, "a68a_block focus");
_Static_assert(__builtin_offsetof(struct a68a_block, in_head) == 0xCC, "a68a_block in_head");
_Static_assert(__builtin_offsetof(struct a68a_block, front) == 0xD0, "a68a_block front");
_Static_assert(__builtin_offsetof(struct a68a_block, prog) == 0xD4 && __builtin_offsetof(struct a68a_block, alive) == 0xD8, "a68a_block prog");
_Static_assert(__builtin_offsetof(struct a68a_block, message) == A68A_OFF_MESSAGE, "a68a_block message");
_Static_assert(sizeof(struct a68a_in) == 16, "a68a_in");

#define A68A_ATTR_INVERSE   0x01             /* the cells' attributes (aurora65.h's A65_ATTR_*) */
#define A68A_ATTR_FLASH     0x02
#define A68A_ATTR_MOUSETEXT 0x04

#define A68A_DISKS_MAX    32u
#define A68A_KIND_525     1u                 /* a 5.25" image: the Disk II's drives */
#define A68A_KIND_BLOCK   2u                 /* a block image (3.5", hard disk): the block cards' units */
#define A68A_DF_RO        1u                 /* its file is never written (a2.d1ro=on ..., or the card is read only) */
struct a68a_disk { char name[64]; uint32_t size, kind, flags; int32_t unit; };   /* unit: -1, or where it is attached */
struct a68a_disks {                          /* at A68A_OFF_DISKS */
    uint32_t count, seq;
    struct a68a_disk d[A68A_DISKS_MAX];
    int32_t  unit[A68A_UNITS];               /* the image in each unit, -1 for none */
};
_Static_assert(sizeof(struct a68a_disks) <= 0x1000, "a68a_disks");

/* ---- the kernel's side (psmac_a2.c) ---- */
uint32_t psmac_a2card_size(void);            /* the Pi memory the card needs (its window, ROM page and pool), 0 with a2=off */
void psmac_a2card_map(uintptr_t phys);       /* that memory, carved by psmac_bus.c beside the other cards' */
void psmac_a2card_init(void);                /* its ROM and block, once the screens are known */
void psmac_a2_report(void);                  /* the heartbeat's line */

#endif
