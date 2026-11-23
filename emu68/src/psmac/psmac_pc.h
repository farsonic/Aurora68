/* SPDX-License-Identifier: MPL-2.0
 *
 * psmac_pc.h — the Aurora68 PC Card's window: the block the Mac and the card share (docs/19-pc-card.md).
 *
 * Backstory (23 November 2026). The owner: "Aurora86 needs to run in a window on the Mac desktop." Aurora86 (a
 * project of its own beside this one) is a PC, Bochs with Libreboot's firmware, built as a library for this
 * kernel (libaurora86.a). This card is how the Mac sees it, the way the DOS Compatibility Card and OrangePC were
 * seen: a NuBus card in slot $D, "Aurora68 PC Card", whose driver ".A68PC" lives in its declaration ROM, and a
 * program, "Aurora68 PC", that shows the PC's screen in an ordinary window, passes the keyboard and mouse to it
 * and says where its window is. The card draws the PC's screen into the Mac's own frame buffer itself, inside
 * that window's visible region (psmac_wclip.c, as the video decoder card does); the 68k never touches a pixel.
 *
 * The window is 1 MiB at $FD000000 (32-bit) and $FDD00000 (24-bit: the slot's $D00000), the declaration ROM in
 * its top 16 KiB; every word big-endian (the 68k's order and this kernel's). Aurora86's docs/DESIGN.md ("The card's
 * block") planned these offsets; this header is the authority now. The card's half and the Mac's are apart, as in
 * struct a68v_block: the card never writes the Mac's half, the Mac never the card's.
 *
 * How the Mac uses it:
 *  1. Find the card (Slot Manager: the sResource of type {$7050 'pP', 4, 1, 'PC'}) or open ".A68PC"; check magic.
 *  2. The place: screen (A68P_SCREEN_*), the window's content rectangle in that screen's pixels, visible, and
 *     (from the program itself) clip_tag = A68P_CLIP_TAG + the new geom_seq with vis_rgn = the window's visRgn
 *     handle; then geom_seq + 1. Again whenever the window moves, is resized, shown, hidden or brought forward.
 *  3. Commands: cmd and arg0/arg1, then cmd_seq + 1; done_seq follows when the card has carried it out.
 *  4. Input while the PC has the keyboard (focus = 1): events into the ring at in_head (the card takes them at
 *     in_tail): Mac virtual key codes down and up, mouse movement and buttons.
 *  5. Status: state, message, mips_x10, load; mode, nat_w, nat_h (the picture's natural size, for the zoom box).
 */
#ifndef PSMAC_PC_H
#define PSMAC_PC_H
#include <stdint.h>

#define A68P_MAGIC        0x41363850u        /* 'A68P' */
#define A68P_VERSION      1u
#define A68P_WIN_SIZE     0x00100000u        /* 1 MiB, the declaration ROM in its top 16 KiB */
#define A68P_OFF_BLOCK    0x00000u
#define A68P_OFF_MESSAGE  0x00100u
#define A68P_OFF_INPUT    0x00400u           /* A68P_IN_SLOTS x struct a68p_in */
#define A68P_OFF_TEXT     0x01000u           /* cols x rows cells: character in the low byte, attribute in the high */
#define A68P_OFF_TINFO    0x04E00u           /* struct a68p_tinfo: the 16 colours and the text screen's numbers */
#define A68P_OFF_FONT     0x05000u           /* 256 glyphs x 32 bytes, the PC's own (VGA plane 2) */
#define A68P_OFF_DAC      0x07000u           /* 256 x 0x00RRGGBB */
#define A68P_OFF_DISKS    0x08000u           /* struct a68p_disks */
#define A68P_OFF_PICTURE  0x10000u           /* reserved: the picture for programs that draw it themselves (not yet) */
#define A68P_OFF_ROM      0xFC000u
#define A68P_ROM_SIZE     0x04000u
#define A68P_IN_SLOTS     64u
#define A68P_TEXT_MAX     (132u * 60u)

/* commands (cmd, then cmd_seq + 1) */
#define A68P_C_POWER_ON   1u                 /* start the PC (built at the first), or power it on again */
#define A68P_C_POWER_OFF  2u                 /* it stops running and keeps its memory; the window shows "off" */
#define A68P_C_RESET      3u                 /* the reset button */
#define A68P_C_PAUSE      4u
#define A68P_C_RESUME     5u
#define A68P_C_CAD        6u                 /* Control-Alt-Delete: the card presses and lets go of the three keys */
#define A68P_C_REDRAW     7u                 /* the whole window again (the program's update event) */
#define A68P_C_ATTACH     8u                 /* arg0 = image (index in the list), arg1 = place (A68P_PLACE_*) */
#define A68P_C_DETACH     9u                 /* arg1 = place */
#define A68P_C_RESCAN     10u                /* the image list again */

/* state */
#define A68P_S_OFF        0u                 /* not started (pc=on starts it with the Mac), or powered off */
#define A68P_S_STARTING   1u                 /* loading the firmware, building the PC */
#define A68P_S_RUNNING    2u
#define A68P_S_PAUSED     3u
#define A68P_S_STOPPED    4u                 /* the PC switched itself off (Power On starts it again) */
#define A68P_S_FAILED     5u                 /* error and message say why */

#define A68P_E_NOLIB      1u                 /* this kernel was built without the PC (PSMAC_PC=OFF) */
#define A68P_E_NOROM      2u                 /* the firmware (pc.rom=) could not be read */
#define A68P_E_NOMEM      3u                 /* no memory for the PC */
#define A68P_E_BOCHS      4u                 /* the PC itself failed (the kernel log has Bochs's words) */

#define A68P_MODE_NONE    0u
#define A68P_MODE_TEXT    1u
#define A68P_MODE_GRAPHICS 2u

#define A68P_SCREEN_FIRST 0u                 /* the Mac's first screen: the SE's own on the SE board */
#define A68P_SCREEN_HDMI  1u                 /* the HDMI screen (slot $A) on the SE board */
#define A68P_CLIP_TAG     0x50430000u        /* 'PC' << 16, plus the geom_seq vis_rgn goes with */
#define A68P_VF_24BIT     1u                 /* vis_flags: the program runs in 24-bit mode */

#define A68P_SCALE_FIT    0u                 /* graphics scaled to the window (text is always drawn from its cells) */
#define A68P_SCALE_1      1u                 /* 1:1, from scroll_x, scroll_y */
#define A68P_O_PCSENSE    1u                 /* options: text white on black as the PC draws it (default: black on white on 1 bit) */
#define A68P_O_THRESHOLD  2u                 /* options: graphics on 1 bit by a threshold, no dither */
#define A68P_O_BIGTEXT    4u                 /* options: text in the PC's own 8 x 16 font also on a 1-bit screen */
#define A68P_O_GROWBOX    8u                 /* options: the window's bottom right 15 x 15 (its grow box) is left to the Mac */

#define A68P_LED_SCROLL   1u
#define A68P_LED_NUM      2u
#define A68P_LED_CAPS     4u

/* the input ring's events */
#define A68P_IN_KEYDOWN   1u                 /* code: a Mac virtual key code (the ADB extended keyboard's) */
#define A68P_IN_KEYUP     2u
#define A68P_IN_MOUSE     3u                 /* dx, dy: the movement (y down); buttons: bit 0 left, 1 right, 2 middle */
#define A68P_IN_BXKEY     4u                 /* code: an A86_KEY_* (aurora86.h), modifiers bit 0: released */
#define A68P_IN_RELEASE   5u                 /* every key the PC holds down is let go (focus went back to the Mac) */

struct a68p_in {                             /* 16 bytes */
    uint16_t type, code;
    uint16_t modifiers;                      /* the Mac event's modifiers */
    int16_t  dx, dy;
    uint16_t buttons;
    uint32_t pad;
};

struct a68p_block {
    /* the card's half: 0x00-0x7F */
    uint32_t magic, version, win_size;       /* A68P_MAGIC, A68P_VERSION (written last), A68P_WIN_SIZE */
    uint32_t done_seq;                       /* the last cmd_seq carried out */
    uint32_t state, error;                   /* A68P_S_*, A68P_E_* */
    uint32_t mips_x10;                       /* guest instructions per second over the last second, / 100,000 */
    uint32_t load;                           /* per mille of its core's time in the PC over the last second */
    uint32_t icount_hi, icount_lo;           /* guest instructions since the start */
    uint32_t core;                           /* the Pi core it runs on: 1 or 2 */
    uint32_t ram_mb;                         /* the PC's memory */
    uint32_t screen_seq, drawn_seq;          /* the PC's screen's change count, and the last one drawn */
    uint32_t draw_us;                        /* drawing, average over the last second */
    uint32_t mode, nat_w, nat_h;             /* A68P_MODE_*; the picture's natural size in the window (pixels) */
    uint32_t focus_ack;                      /* the focus the card acts on */
    uint32_t leds;                           /* A68P_LED_* */
    uint32_t disk_busy;                      /* disk reads and writes in the last second */
    uint32_t in_tail;                        /* input events taken */
    uint32_t draws;                          /* pictures drawn since the start */
    uint32_t idle_pm;                        /* per mille of the last second the PC was idle (halted, waiting) */
    uint32_t pad0[8];
    /* the Mac's half: 0x80-0xFF */
    uint32_t cmd_seq, cmd, arg0, arg1;       /* cmd and its arguments, then cmd_seq + 1 */
    uint32_t screen;                         /* A68P_SCREEN_* */
    int32_t  top, left, bottom, right;       /* the window's content in that screen's pixels, QuickDraw's order */
    uint32_t visible;                        /* 0: draw nothing (hidden, being dragged) */
    uint32_t geom_seq;                       /* + 1 after any of the above, clip_tag/vis_rgn/vis_flags, scale, scroll */
    uint32_t clip_tag, vis_rgn, vis_flags;   /* the window's visRgn handle (A68P_CLIP_TAG + geom_seq it goes with) */
    uint32_t scale;                          /* A68P_SCALE_* */
    int32_t  scroll_x, scroll_y;             /* 1:1: the picture's pixel at the window's top left */
    uint32_t options;                        /* A68P_O_* */
    uint32_t focus;                          /* 1: the PC has the keyboard and mouse */
    uint32_t in_head;                        /* input events put in */
    uint32_t pad1[12];
    /* the card again: 0x100 */
    char     message[128];                   /* what the PC is doing, or why it stopped, in words (ASCII) */
};
_Static_assert(sizeof(struct a68p_block) == 0x180, "a68p_block");
_Static_assert(__builtin_offsetof(struct a68p_block, cmd_seq) == 0x80, "a68p_block Mac half");
_Static_assert(__builtin_offsetof(struct a68p_block, in_head) == 0xCC, "a68p_block in_head");
_Static_assert(__builtin_offsetof(struct a68p_block, message) == A68P_OFF_MESSAGE, "a68p_block message");
_Static_assert(sizeof(struct a68p_in) == 16, "a68p_in");

struct a68p_tinfo {                          /* at A68P_OFF_TINFO */
    uint32_t colours[16];                    /* the 16 attribute colours, 0x00RRGGBB */
    uint32_t cols, rows, cursor_x, cursor_y, cursor_start, cursor_end;
    uint32_t char_w, char_h, width, height, bpp, seq;
};

#define A68P_DISKS_MAX    32u
#define A68P_PLACES       6u                 /* IDE 0 master, 0 slave, 1 master, 1 slave; floppy A, B */
#define A68P_KIND_HD      1u
#define A68P_KIND_FLOPPY  2u
#define A68P_KIND_CD      3u
#define A68P_DF_RO        1u
struct a68p_disk { char name[64]; uint32_t sectors, kind, flags; int32_t place; };   /* place: -1, or where it is attached */
struct a68p_disks {                          /* at A68P_OFF_DISKS */
    uint32_t count, seq;
    struct a68p_disk d[A68P_DISKS_MAX];
    int32_t  place[A68P_PLACES];             /* the image attached at each place, -1 for none */
};
_Static_assert(sizeof(struct a68p_disks) <= 0x1000, "a68p_disks");

/* ---- the kernel's side (psmac_pc.c) ---- */
uint32_t psmac_pccard_size(void);            /* the Pi memory the card needs (its window and the PC's own), 0 with pc=off */
void psmac_pccard_map(uintptr_t phys);       /* that memory, carved by psmac_bus.c beside the other cards' */
void psmac_pccard_init(void);                /* its ROM and block, once the screens are known */
int  psmac_pc_poll(int core);                /* one slice of the PC and its drawing, from that core's loop; 1 if it ran */
void psmac_pc_report(void);                  /* the heartbeat's line */

#endif
