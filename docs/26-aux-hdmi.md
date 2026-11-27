<!-- SPDX-License-Identifier: MPL-2.0 -->
# 26. The HDMI screen under A/UX

*27 November 2026.* Under Mac OS the SE has two screens: its own, served by slot 9's card, and the Pi's HDMI
output, a second virtual NuBus card in slot $A with a declaration ROM and a video driver of ours (docs/10, "Two
screens on the SE"). Under A/UX the owner had to run with `mac.hdmi=log`: with the HDMI card present, "enabling
external HDMI locked up the Mac". docs/23 section 16 had seen one side of it (a level-2 storm at A/UX Startup's
launch, and the storm guard as a second line), and nobody had looked further. This is the record of what locked
up, what A/UX does with NuBus video cards, and what was changed. Where something is our reading and not an
observation it says *inference*. A/UX's code was read from the owner's disk image in the scratch area (the kernel
`/unix` with its symbols); none of it is in the repository.

## 1. How A/UX finds and drives a video card

From the kernel's own code (its symbols name the functions):

- **A/UX opens every video card's Mac OS driver itself, in its kernel.** At start-up `video_init` calls
  `video_find`, which asks the Slot Manager (the Mac ROM's, through the kernel's `slotmanager`) about slots 0, 9,
  $A, $B, $C, $D and $E in turn: the next video sResource (category 3, type 1) of that slot, its device base, the
  mode with the fewest bits per pixel and the largest frame (its VPBlock), and the driver (`SGetDriver`). Then, for
  each card found, `video_init` registers the kernel's `video_intr` as the handler of that card's slot interrupt
  and opens the card's driver (`callOpen`). The driver runs in the kernel, with a small set of Toolbox traps the
  kernel answers itself (`aNewPtr`, `aSIntInstall`, ...; an unknown trap is "Attempt to use unsupported A-line
  trap" and a panic, docs/23 section 15.1). This is why our cards' drivers say "open" twice in a boot that goes on
  to A/UX: once under Mac OS, once under A/UX's tables. No driver of A/UX's own is needed for a video card, and
  nothing in `/etc/master.d` or `/etc/boot.d` is about video.
- **One slot interrupt handler per slot.** The kernel's `SIntInstall` (`cSIntInstall`) keeps one queue element per
  slot. A second one for the same slot prints "The slot is %x" and the list of video cards, then
  `panic: Multiple interrupt handlers for slot %x`. `video_intr` calls the slot's one handler (`callSlotInt`), and a
  handler that returns 0 is `panic: Polling routine didn't handle interrupt`. Mac OS keeps a queue per slot, sorted
  by priority, and asks each in turn; A/UX does not.
- **A card is filed under the slot of its frame address as well.** `video_find` stores each card in a table
  indexed by its slot (`video_index[slot]`) and once more under `addr2slot(frame address)`: the top nibble of the
  address, or the next one when the top is $F (so $F9988000 and $90000000 are slot 9, $FAA88000 is slot $A). The
  console is the card of the slot parameter RAM names as the main screen (`check_video_depth`, extended PRAM byte
  $80), if that card is at one bit; the kernel draws the pointer itself (`ui_drawcursor`) on the card it finds,
  again by slot, from the frame address of the Macintosh environment's main GDevice (`ui_useGDevice`).
- **The Macintosh environment draws into the cards' memory directly.** `UI_phys_screens` maps each card's slot space
  into the Macintosh environment's process: the card's 16 MiB ($Fs000000) in 32-bit mode, its megabyte in 24-bit
  mode; not the super slot space ($s0000000). Video driver Control and Status calls of the Macintosh environment
  (Monitors, QuickDraw's mode and colour changes) go to the kernel's copy of the driver (`callControlStatus`).
- **What A/UX's documents say.** The owner's image carries A/UX's manuals as DocViewer files (a Documentation
  folder in the root directory).
  "Basic Skills" has a section "Configuring multiple monitors": the Monitors control panel in the Macintosh
  environment sets their positions and the main one, as under Mac OS. "Server Admin" says that `autoconfig`, run
  at every start, builds a new kernel and asks for a restart when the hardware changed, "for example, if you have
  moved or removed a video card".

## 2. What locked up

The HDMI card's driver (`mac/pivideo.s` assembled with `SCREEN2`) installed its slot interrupt handler on slot 9,
beside the SE screen driver's: the Pi raised slot 9's line for both screens' VBLs, and each handler claimed only its
own block's tick, which is how Mac OS shares a slot. Under A/UX the second `SIntInstall` on slot 9 is the panic
above.

The owner's log (real SE, test kernel 23, `mac.hdmi=screen`, the HDMI at 2560x1440) shows it from the side:
after "A/UX's tables on", `PiHDMI: open`, `PiHDMI: VBL handler installed`, then `PiVideo: open` and nothing more
from PiVideo; display 0 (the SE's) stayed "vbl off" (PiVideo turns its VBLs on only after its `SIntInstall`
returns). Then the storm guard tripped on level 4 once and on level 2 four times at PC $548E0, and from there every
heartbeat showed levels 1 and 2 each taken about two million times in five seconds, "taken again at once", no access
to any chip, the 68k in locore's idle loop ($548E4-$54912) with every autovector on the masking stub
`ORI.W #$0700,(SP); RTE` at $548C8. That is what the kernel is left with after its panic during driver
initialisation: the stub clears no source, so the HDMI's VBL (pending on slot 9, its driver's handler never to run)
and the VIA's clock come back after every `STOP #$2000`. Level 1 storms too for that reason, not because of the SE's
own VBL. The storm is the aftermath; holding the HDMI's VBL back would not have helped. (At "Launching..." the A/UX
kernel itself is already running, A/UX Startup's dialog still on the screen; what docs/23 sections 14 and 16 called
the standalone shell is the kernel's early start-up, as section 15 found.)

The HDMI driver opened first on the owner's SE because of the filing by frame address: the HDMI card's frames were
at slot 9's addresses ($F9988000, $90000000), so `video_index[9]` became the HDMI card and the console choice
(parameter RAM's main slot, 9) moved it to the front. In the testbed, at 640x480, PiVideo happened to open first,
and the panic would have come at PiHDMI's open instead.

## 3. The changes

1. **The HDMI driver takes its own slot's interrupt** (commit 27ec856). `pivideo.s` with `SCREEN2` installs its
   handler on slot $A and writes the slot into its display block (`irq_slot`, the former padding at offset 56 of
   `struct psmb_display`, psmb.h) before it turns VBLs on. The kernel raises slot $A's bit of the RBV's slot
   register for that screen's VBL instead of slot 9's line (`psmac_display_slot_irq`, beside the A/UX network
   card's `psmac_etalk_slot_irq` in `psmac_board_se.c`'s `rbv_recalc` and slot register read). A driver from before
   writes 0 there and keeps slot 9; at the hand-over to A/UX's tables the kernel then says that A/UX will stop
   ("the HDMI screen's driver takes slot 9's interrupt ..."). Under Mac OS nothing else changes: the IIci ROM
   dispatches slot $A's bit to slot $A's queue, where the driver's handler is now alone.
2. **The HDMI card's frames at slot $A's own addresses** (commit df044fe, then the layout of item 3). With the
   interrupt fixed, A/UX reached its Finder on the owner's SE (test kernel 24), and its Macintosh environment's
   pointer was drawn on the HDMI monitor, in a small rectangle at its top left that it could not leave, with old
   watch cursors left behind there; the HDMI monitor was otherwise plain grey. The testbed showed the same (run 1: a
   watch cursor at the HDMI screen's top left while A/UX's console text was on the SE's). That is the filing by frame
   address: the SE screen's card and the HDMI card both "slot 9", the later one (the HDMI) won the table entry, and
   the kernel drew the SE screen's pointer, with the SE screen's coordinates, into the HDMI card's frame. So the same
   Pi memory now has slot $A addresses as well, and the HDMI card's modes use them (run 2: the watch cursor on the
   SE's screen, the HDMI screen a clean grey).
3. **Millions under A/UX, and nothing taken from Mac OS.** With kernel 25 the owner set the HDMI screen to millions
   in A/UX's Monitors (it works under Mac OS, and has since 20 November). A/UX's Macintosh environment then crashed
   and was restarted over and over (the SE's screen flashing, about 25,000 privileged instructions trapped and 30
   bus errors every five seconds, no system error logged): millions at 2560x1440 was a frame at $90000000, in slot
   9's super slot space, which A/UX never maps for the Macintosh environment. And our driver had let the mode be
   set: its "24-bit mode reaches only the slot's megabyte" test reads MMU32Bit, and under A/UX it runs in A/UX's
   kernel, whose low memory is not the environment's. The owner's requirement, rightly: Mac OS keeps every depth it
   has; a limit only under A/UX and only where a frame really cannot be reached. What was done:
   - The HDMI card's big region is at $FA000000, in slot $A's own 16 MiB, for as much as fits below the card's ROM
     (16 MiB less 16 KiB; millions at 2560x1440 is 14.06 MiB), the same Pi memory as $90000000. A/UX maps that 16
     MiB for a 32-bit Macintosh environment. Only a frame larger than that (millions at 3840x2160) stays at
     $90000000, for a 32-bit Mac OS.
   - The small modes a 24-bit Mac reaches are at $FAA88000 (24-bit $A88000), in slot $A's 24-bit megabyte: the
     window's frame half while the big region is at most 10 MiB; a larger big region (sizes above about 2048x1152)
     covers that megabyte, and the small modes then use the big region's memory at offset $A88000.
   - While A/UX's tables are on, the kernel sets `aux` in the display blocks (psmb.h, the last padding word). The
     HDMI driver's check (`reach`, pivideo.s) then reads the calling Macintosh environment's own MMU32Bit with MOVES
     from user space and refuses (paramErr) a mode outside the card's megabyte in 24-bit mode, outside its 16 MiB
     in 32-bit mode. A mode left at $90000000 also leaves the HDMI card's live declaration ROM and its block's table
     under A/UX, so that A/UX's Monitors does not offer it. The 68k's RESET gives all of it back for Mac OS.
   - The HDMI parts of the driver are assembled only with `SCREEN2`: every declaration ROM other than the SE's
     two-screen files is byte for byte what the commit before this work builds, and so is slot 9's part of the SE's
     files.

Both changes need the new declaration ROM (`aurora68-se.declrom`) as well as the new kernel.

## 4. What the testbed showed

The MAME SE full stack (`MACHINE=macse_ps`), the owner's A/UX disk as a virtual SCSI disk at ID 0 (a fresh copy
each run), `mac.mmu=full mac.ram=64`, the two-screen declaration ROM, QEMU's VideoCore at 640x480 unless said
otherwise; the VideoCore photographed every 100 s (`tools/vcshot.py`). The host was slow that night.

- **Run 1** (commit 27ec856: the interrupt on slot $A, frames still at slot 9's addresses). After "A/UX's tables
  on": `PiVideo: open`, `PiVideo: VBL handler installed`, `PiHDMI: open`, `PiHDMI: VBL handler installed`, then
  `PiVideo: first VBL interrupt` and `PiHDMI: first VBL interrupt`; level 2 about 50 times a second from then on,
  "taken again at once" a few dozen to 258 times in five seconds, no storm guard line, init's programs running
  (356 bus errors delivered). A/UX's console text on the SE's screen, and a watch cursor at the top left of the HDMI
  screen: the pointer in the wrong card's frame (section 3, item 2).
- **Run 2** (commit df044fe: frames at slot $A's addresses). The log: "the HDMI card's frames at slot $A's own
  addresses"; the same course; the watch cursor on the SE's screen, the HDMI screen a clean grey.
- In both, and in a baseline with `mac.hdmi=log` (run 3, the same kernel), A/UX did not reach its Macintosh
  environment within 30 minutes: init's verbose console stayed on the SE's screen, no start-up dialog; runs 1 and
  2 ended with "Error output from startmac: The toolbox environment already exists" at about 1,620 s, run 3 had not
  got that far at 1,800 s. On earlier nights the Finder came at 700 to 1,400 s (docs/23 section 21). So the HDMI
  screen as an active screen under A/UX was not seen in the testbed; the owner's SE is that test.
- **System 7.1 on the SE board** (a copy of the 7.1 image with Aurora68's extensions installed, Aurora68 Screen
  Setup arranging the screens, Aurora68 Colour in Startup Items; commit fd9049f):
with 16 MB (32-bit addressing on) and the HDMI at 2560x1440 (QEMU's VideoCore started at that size,
  `QEMU_EXTRA`), Aurora68 Screen Setup wrote its arrangement and restarted once, and the HDMI screen came up in
  millions (`depth 32 rowbytes 10240`), the log saying its frames are at $FAA88000 and $FA000000, Aurora68 Colour's
  hues on it. The same with the declaration ROM from before this work (5d7f50f): frames at $F9988000 and
  $90000000, and the VideoCore's picture byte for byte the same as with the new ROM. At 640x480, 32-bit: millions.
  With 8 MB and 32-bit addressing off (parameter RAM byte $8A cleared, `mac.pram8a=keep`): at 2560x1440 the HDMI
  screen in one bit, its pixels in the big region's memory at offset $A88000 (the new path of section 3, item 3), the
  Finder's pattern and Aurora68 Colour's window drawn on it; at 640x480, one bit at first: Aurora68 Screen Setup
  counted only $F99xxxxx as reachable in 24-bit mode, so it no longer found the HDMI's indexed modes, now at
  $FAAxxxxx. It counts both since commit 55bd631; with it, the same 24-bit start at 640x480 gave the HDMI screen 256 colours (`depth 8 rowbytes 640`)
  at $FAA88000.
- The host tests: `tools/diag_test.sh` 7,689 checks, 0 failed; `tools/adb_test.sh` 86, 0 failed;
  `tools/aux/etalk_test.sh` 44,539, 0 failed; `make -C mac check` passed. Every declaration ROM `make -C mac` builds
  other than the `psmac-se*` files is byte for byte the build of 5d7f50f, and so is slot 9's part of the
  `psmac-se*` files (compared with `cmp`).

## 5. On the owner's SE

- Test kernel 24 (commit 27ec856 and its ROM): past "Launching..." to A/UX's Finder with `mac.hdmi=screen`; the
  pointer stuck in the HDMI monitor's top left (section 3, item 2).
- Test kernel 25 (df044fe): the pointer on the SE; Monitors in A/UX's Macintosh environment arranged two screens
  with millions for the HDMI, and after the restart the environment crashed and restarted over and over (section 3,
  item 3). The way out: `mac.hdmi=log` (the arrangement names two screens, so with one it is not used), then
  `/mac/bin/mac32` before going back to `mac.hdmi=screen`.
- Test kernel 26 (fd9049f and 55bd631, with the new `aurora68-se.declrom` and Aurora68 Screen Setup): **two
  screens under A/UX on the owner's real SE** (27 November 2026): A/UX's Macintosh environment runs with the SE's
  screen and the HDMI monitor (2560x1440) side by side. The depth used and the addressing mode were not reported.

## 6. What is not settled

- The HDMI screen as an active screen of A/UX's Macintosh environment works on the owner's SE (section 5); which
  depths and which addressing mode were tried is not yet known. In particular what the Mac ROM's 'scrn' loader in a 24-bit environment does when the driver refuses the
  saved depth (paramErr) is not known; if it builds the screen from that mode anyway, the environment will still
  fail, and the answer is `mac32` or a smaller saved depth.
- The pointer on the HDMI screen under A/UX: A/UX's kernel draws it (section 1) on the card it finds by the main
  GDevice's frame address; how it follows the pointer onto a second screen was not read.
- `MOVES` from the driver reads the calling environment's low memory. A mode change asked for in a context with
  no Macintosh environment (none was seen; A/UX's own calls go through the environment's `ioctl`) and outside the
  card's megabyte would fault in A/UX's kernel.
- A/UX's `autoconfig` and a video card that comes and goes (`mac.hdmi=screen` and `log` in turn): its manual says
  it builds a new kernel and asks for a restart when a video card was moved or removed; not seen.
- Frames over 16 MiB less 16 KiB (millions at 3840x2160) stay at $90000000 and are left out under A/UX.
