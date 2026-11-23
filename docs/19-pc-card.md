# The PC card: a PC in a window on the Mac's desktop

23 November 2026. The owner asked for an x86 card ("Let's make an x86 card and also integrate
https://libreboot.org/"), then: **"Aurora86 needs to run in a window on the Mac desktop."** The PC itself is
Aurora86, a project of its own beside this repository (`/home/user/aurora86` on the development machine, not
published yet): Bochs 3.1, a Pentium II on an i440FX board, whose firmware is Libreboot's QEMU x86 image (coreboot
with SeaBIOS), built freestanding and big-endian with this kernel's own code-generation flags as `libaurora86.a`.
Its documents (`docs/DESIGN.md`, "The card in Aurora68" and "P3: the window"; `docs/STATUS.md`) are the plan and
the record of the PC; this document is the card's: how the Mac sees the PC, how the kernel runs it, how the
window works. (Docs 17 and 18 were taken when this was written, so the number is 19.)

The owner's decisions (23 November 2026): slot $D; the card "Aurora68 PC Card", the program "Aurora68 PC"; 16 MB
of PC memory by default; Libreboot as published; on the SE's one-bit screen graphics scaled and dithered (1:1 as a
choice), text mode always fitting; core 2 where it is free, core 1 in slices on the SE board;
Control-Option-Command-P (and a click, and a menu item) moving the keyboard and mouse between the Mac and the PC.

## The pieces

| Where | What |
|---|---|
| `emu68/src/psmac/psmac_pc.c`, `psmac_pc.h` | the card: its block, the PC's memory, files, start, slices, drawing, input, commands |
| `emu68/src/psmac/psmac_wclip.c`, `.h` | where a window shows (its visRgn as rectangles), the box around the Mac's pointer, the menu-down test: moved out of the video decoder card (`psmac_video.c`, `psmac_videocard.c`) on the same day, both cards use them |
| `emu68/src/psmac/psmac_pc_declrom.h` | the card's declaration ROM, made by `tools/mac/mkpcdeclrom.py` (`make -C mac pc-declrom`) from `mac/a68pc.s` |
| `emu68/src/psmac/psmac_pc_font.h` | the 6 x 13 and 6 x 12 fonts, made by `tools/mkpcfont.py` from the X Window System's misc-fixed (public domain) |
| `mac/a68pc.s` | `.A68PC`, the card's driver, in its declaration ROM |
| `mac/a68pc/pc.c` | the program **Aurora68 PC** (C, the `mac/c` kit), `mac/build/Aurora68-PC.bin` |
| hooks | `psmac_bus.c` (the card's memory, map and start; core 2's slice; the heartbeat's line), `psmac_mmu.c` (slot $D's pages), `psmac_usb.c` (core 1's slice), `psmac_card.c` (the PC's files on the SD card), `psmb.h` (slot $D's addresses), `psmac_diag.c` (the options' group "PC card"): every one inside `#ifdef PSMAC_PC` |

**Only in a kernel built for it.** `cmake ... -DPSMAC_PC=ON -DAURORA86_DIR=/path/to/aurora86` compiles
`psmac_pc.c` and links `$AURORA86_DIR/build/p2/aarch64be/libaurora86.a` (Aurora86's `make p2`). The default is
off: the normal kernel and CI need nothing from Aurora86, and a normal kernel is the same file it was before the
card (checked: the kernel from the commit before the card and the kernel from the card's tree with `PSMAC_PC` off,
both built outside git with the same `SOURCE_DATE_EPOCH`, are the same `Emu68.img` byte for byte). In a PC kernel
the card is in slot $D with `pc=on` and absent otherwise (`pc=off`, the default until it is proven).

## How the Mac sees it

Slot $D: a 1 MiB window at `$FD000000`, again at `$FDD00000` (the slot's 24-bit megabyte, `$D00000` in 24-bit
mode), the declaration ROM at the slot's top (`$FDFFC000`), mapped by the MMU shim as slot $C's are. The ROM has
the board ("Aurora68 PC Card", BoardId 'PC', vendor "Aurora68 project", part "A68-PC") and one sResource of the
project's vendor category, `{$7050, 4, 1, 'PC'}`, "Aurora68_PC", with the driver `.A68PC` for the Slot Manager to
install and open at start-up, as `.A68Video` is.

The window's block (`psmac_pc.h`, `struct a68p_block`; Aurora86's DESIGN planned the offsets):

| Offset | Who writes | What |
|---|---|---|
| $0000 | card | `magic` 'A68P', `version`, `done_seq`, `state` (off, starting, running, paused, stopped, failed), `error`, `mips_x10`, `load`, the instruction count, `core`, `ram_mb`, the screen's change count and the last one drawn, `draw_us`, `mode`, `nat_w`/`nat_h` (the natural size), `focus_ack`, `in_tail`, `draws`, `idle_pm` |
| $0080 | Mac | `cmd_seq`, `cmd`, `arg0`, `arg1`; the place: `screen`, `top`, `left`, `bottom`, `right`, `visible`, `geom_seq`; `clip_tag`, `vis_rgn`, `vis_flags` (the window's visRgn, tagged with the geom_seq it goes with, as Play's); `scale`, `scroll_x`, `scroll_y`, `options`; `focus`; `in_head` |
| $0100 | card | `message`, in words |
| $0400 | Mac | the input ring: 64 events of 16 bytes (key down and up as Mac virtual key codes, mouse movement and buttons, a Bochs key code, "let everything go") |
| $1000 | card | the text cells (character, attribute), up to 132 x 60 |
| $4E00 | card | the 16 attribute colours and the text screen's numbers (`struct a68p_tinfo`) |
| $5000 | card | the PC's font (VGA plane 2), $7000 the DAC |
| $8000 | both | disks: the image list and the six places (four IDE, two floppies) |

Commands: power on (the PC is built at the first), power off (it keeps its memory and stops running), reset,
pause, resume, Control-Alt-Delete (the card presses and lets go of the three keys), redraw, attach, detach and
rescan (the last three answer "later" for now: `pc.disk0=` chooses the disk). Options: `A68P_O_PCSENSE` (text in
the PC's own white on black on a one-bit screen), `A68P_O_THRESHOLD` (no dither), `A68P_O_BIGTEXT` (the PC's 8 x 16
font on one bit too), `A68P_O_GROWBOX` (the window's bottom-right 15 x 15 left to the Mac's grow box).

`.A68PC`'s calls (`mac/a68pc.s`): Control 100 a command and its two arguments (waits for the card to take it),
112 the place (six longs), 113 an input event, 114 scale, scroll and options, 115 the focus; Status 100 the
window's address in the caller's addressing mode, the version, state and error, Status 101 the block.

## How the kernel runs it

- **Memory.** The PC needs about 95 MiB beside its RAM (Aurora86's harness: "95 MiB of the heap in use" with 16 MB
  of RAM: Bochs's memory, ROM space, the VGA's memory, the decoded-instruction cache), and the 12 MiB firmware file.
  The kernel's heap is about 52 MB, so the card's memory is carved from the Pi's RAM by `psmac_setup_memory`, like
  the frame buffers and the video card's window: the window, the ROM page and `pc.ram` + 104 MiB as a TLSF pool of
  its own (120 MiB with the default 16 MB). The few allocations Bochs's global objects make in their constructors
  come from the kernel's heap: the kernel runs the library's constructors with its own (`start.c`'s `__init_start`
  list) before the pool exists.
- **The core.** The solo board's core 2 is the service core, but light there (the heartbeat, the VIA's time), so
  the PC runs on it in slices of about 2 ms between the loop's turns. On the SE board core 2 is the SE's bus, and on
  the IIci testbed it carries every I/O cycle over the bridge (found while writing this: the design had counted the
  testbed's core 2 as free), so there the PC runs on core 1 in slices of about 1 ms between the sound output's turns
  and the video decoder's. `pc.core=1|2` chooses. A slice's length in instructions follows the measured rate.
- **Idle.** Bochs paces the PC's clock (`clock: sync=both`: its timers in real time, and its time never more than a
  second ahead of the Pi's): while the PC halts (the firmware waiting, DOS idle with HLT) it sleeps a millisecond at
  a time, and the library ends the slice there and says how long (`a86_pc_idle_us`); the core does other work
  meanwhile.
- **Files.** `pc.rom=` (Libreboot's 12 MiB image), `pc.vgarom=` (SeaVGABIOS for Bochs's VGA) and `pc.disk0=` (a hard
  disk image, 16 heads and 63 sectors a track), in the SD card's root, read on the PC's core under the card's lock
  (`psmac_card_pc_open`/`_read`: a file's clusters listed once, then any part read a cluster at a time), or in the
  testbed host files through semihosting. The firmware is read a megabyte per slice; the disk on demand, through
  `a86_disk()`'s callbacks. What the PC writes stays in its memory (an overlay by sector, lost at power-off): the
  image on the card is not written until writing back is built and tested.

## The picture

The card draws the PC's screen itself, into the frame buffer of the Mac's screen the window is on (screen 0: the
SE's own on the SE board, the HDMI screen on the solo board; screen 1: the SE board's HDMI screen), at that screen's
depth, 30 times a second at most and only the part that changed: the PC's dirty rectangle, the window's whole content
after it moved, was resized or uncovered (the program's update event asks for a redraw), the cursor's cell at each
blink (266 ms). It draws only inside the window's visRgn (read through the Mac's memory before each picture), never in
the box around the Mac's pointer (redrawn once the pointer has moved on), never while any program's menu is down, and
not while the program says the window is being dragged or resized.

- **Text** is drawn from the cells. On a one-bit screen in a 6 x 13 font (the X Window System's misc-fixed, the
  PC's code page 437 with its box drawing), 80 x 25 in 480 x 325; but **the SE's screen is too short for that**: 342
  lines less the menu bar (20) and a document window's title bar and frame (21) leave 301, so on a screen under 366
  lines the card uses misc-fixed's 6 x 12, 80 x 25 in 480 x 300, which fits (Aurora86's DESIGN had 6 x 13 fitting
  "with the window's frame": it does not, by 24 lines). A cell is black on white: the lighter of its two colours is
  the paper, so DOS's grey on black is black on white and a highlighted bar white on black (`View > White on Black`
  for the PC's own sense). On deeper screens the PC's own font (8 or 9 x 16) in its 16 colours, through the screen's
  colour table at 2 to 8 bits.
- **Graphics** (VGA and Bochs VBE modes, the 32-bit picture the library keeps) are scaled to the window by the nearest
  pixel, ordered-dithered (8 x 8) on one bit, through a 4096-entry table of the screen's colours at 2 to 8 bits; `View
  > Actual Size` shows them 1:1.

## Aurora68 PC

`mac/a68pc/pc.c`, a C program with the `mac/c` kit, in `Aurora68:Aurora68 PC` (`tools/mac/aurora68-layout.py`). A
document window with grow and zoom boxes, titled with the PC's state ("Aurora68 PC (off)", "... (starting)", and
": keyboard and mouse to the PC" while captured); 480 x 300 at first, which fits the SE's screen; zoomed, the
picture's natural size. Menus: **PC** (Power On, Power Off, Reset, Send Control-Alt-Delete, Pause), **Disks** (the
card's image list, attached ones checked), **View** (Scaled to the Window, Actual Size, Large Text (8 x 16), White on
Black), **Keyboard** (Capture Keyboard and Mouse (Control-Option-Command-P)). It powers the PC on when it starts if
the PC is off.

The program draws nothing in the window but the grow box: it tells the card the window's content rectangle in the
screen's pixels and hands over its visRgn handle (tagged, as Aurora68 Play 1.8 does), again whenever the window moves,
is resized, zoomed, activated, or the program is suspended or resumed; while it drags or resizes the window it says
"not visible". **Capture**: a click in the window, the menu item or Control-Option-Command-P while the window is in
front gives the PC the keyboard and mouse; the same key, a Command-click, or the window going to the back gives them
back (the card lets go of every key the PC holds). While captured the program adds keyUpMask to SysEvtMask ($144) so
that key releases arrive, passes key-downs and key-ups as Mac virtual key codes (auto-key events are not passed: the
PC repeats keys itself), the modifier keys as presses of their own from the events' modifiers (Command as the
Windows key, Option as Alt, Control as Control), hides the pointer and reads the mouse's movement at each null event
from the window's middle, putting the pointer back there (MTemp, RawMouse and CrsrNew, the trick games of the
period used). The card maps the ADB extended keyboard's codes onto Bochs's (`mac2pc` in `psmac_pc.c`).

## Options

| Option | What |
|---|---|
| `pc=on` | the card in slot $D and the PC started with the Mac (off by default; only in a kernel built with `-DPSMAC_PC=ON`) |
| `pc.ram=MB` | the PC's memory, 16 by default (4 to 256); the card takes this plus 104 MiB of the Pi's RAM |
| `pc.core=1\|2` | the Pi core: by default 2 on the solo board, 1 elsewhere |
| `pc.rom=NAME`, `pc.vgarom=NAME` | the firmware and the VGA BIOS in the card's root: `aurora86.rom` and `aurora86-vga.bin` by default (host paths in the testbed) |
| `pc.disk0=NAME` | a hard disk image in the card's root for the first IDE disk |
| `pc.testwin=S,L,T,R,B` | testbed only: the window's place set by the kernel (no program needed), the whole rectangle drawn |
| `pc.textlog=1` | testbed only: every changed line of the PC's text screen into the log, `[PC] \| text` |

## What the owner copies to the SD card

From an Aurora86 checkout after `make firmware p2` (firmware never committed anywhere):
`build/firmware/pc.rom` (the 12 MiB Libreboot image) as **`aurora86.rom`**, `build/firmware/vgabios-bochs-stdvga.bin`
as **`aurora86-vga.bin`**, and a disk image (for instance FreeDOS 1.4's `FD14LITE.img`, which Aurora86's
`make test-freedos` fetches) as, say, `freedos.img` with `pc.disk0=freedos.img`; a kernel built with `-DPSMAC_PC=ON`
as `kernel8.img`; `pc=on` in `cmdline.txt`; Aurora68 PC on the Mac.
