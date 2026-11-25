# Prior art (cloned under reference/)
- michalsc/Emu68 (MPL-2.0) — JIT 68040, bare metal; output `Emu68.img` (we rename to kernel8.img). No MMU emulation; Amiga map hardcoded in ps_protocol.c / M68k_Translator.c; AGENTS.md rules make Mac support a fork, not a PR.
- captain-amygdala/pistorm (MIT) — classic CPLD/GPIO protocol (ps_protocol.h): REG_DATA/ADDR_LO/ADDR_HI/STATUS, IPL via status bits, no BR/BG/BGACK, FC lines not handled.
- Frimples/pistorm-macintosh-se — Linux+Musashi SE core; ROM presented at 0 during reset; write-through only for video/sound (3x speed); virtual NCR 5380 target (`macse_scsi.c`, Lido formats, HD SC Setup rejects); 24-bit mask patch for 020/030; latest (Nov 2026, Discord): 68040 + 128 MB + Quadra 900 donor ROM + grey dithering.
- Frimples/MacSE-PiStorm-DirectSocket — EPM240 RTL with bus arbitration + /VPA; hardware validation pending.
- Frimples/MACSE_ROMINATOR — SE ROM with checksum disabled + bbraun ROM-disk driver appended (unfinished).
- Tinkerdifferent threads: "PiStorm on the Macintosh SE" (2085), "ROMINATOR ROM mod for SE" (2109).
- BMOW ROM-inator / bbraun plusrom: ROM disk driver model for a ROM DRVR + boot hook.
- Apple: Guide to Macintosh Family Hardware 2nd ed., Designing Cards and Drivers 2nd ed. (SE PDS pinout, video declaration ROMs, .ENET driver API).
- matthewmrichter/moofport — open-source 68k `.ENET0` Ethernet driver (DaynaPORT emulation on BlueSCSI v2 WiFi): reference implementation of the MacTCP/OT Ethernet driver API for our `.ENET` mailbox driver.
- erichelgeson/BlueSCSI-Toolbox — 68k utility apps; BlueSCSI image conventions (plain HFS or APM images named `HDn_512.hda`) worth mirroring in `sesd`.
- No open-source generic Apple_Driver (SCSI disk driver) found; we will write one (also used by `sesd` to make any image bootable on real SCSI / MAME).


## The accelerators of the compact Macs, 1987-1994

*Written 25 November 2026, from software the owner collected and sent the same day.*

### The problem they all faced

The Macintosh Plus (1986) and SE (1987) run a 68000 at 7.8336 MHz, half the 15.6672 MHz clock that also paces
the screen. Apple wrote their system for exactly that processor, and in several places it is timed by it. The
floppy controller, the IWM, has no clock of its own for the bit stream: the Sony driver counts instructions. Short
delays elsewhere (the gap the SCC serial chip needs between two accesses, for one) are DBRA loops sized by two
low-memory constants, TimeDBRA and TimeSCCDB at $D00 and $D02, that the ROM measures at start-up. And sound has no
chip at all: the SE's video circuitry fetches one word of a 370-word buffer at the top of RAM on every scan line,
342 visible and 28 in vertical blanking, and the high byte sets the width of a pulse that the analog board turns
into sound. Sixty times a second the Sound Driver's VBL task computes the next 370 samples (four waveforms summed
for four-tone sound, a resampled stream for freeform sound) and writes them into that buffer while the video
hardware is playing it. Where in the buffer the task starts writing, relative to where the hardware is reading,
was chosen for a 68000.

A faster processor broke each of these, and an industry grew up to sell faster processors anyway. The Plus's
68000 was soldered or socketed on the logic board, so its accelerators clipped over the chip or replaced it from a
socket; the SE had Apple's 96-pin processor direct slot, which Apple itself documented for "a coprocessor" taking
the bus (docs/22). The six products below are the ones whose software has been read here: their control panels,
INITs and patches, unpacked from BinHex and StuffIt archives, and one from an 800K Disk Copy image, then listed
resource by resource and disassembled. None of that software is in this repository; it is the work of companies
long gone, and is described, not copied.

### GCC HyperCharger 020 (1987-1988)

GCC (General Computer Corporation of Massachusetts, the company behind the arcade Ms. Pac-Man and, for the Mac, the
HyperDrive internal hard disk of 1985) made the HyperCharger, a 68020 board with a 68881 maths coprocessor for the
Plus and the SE. The software read here is "HyperCharger 2.0 (c) 1987, 1988 GCC, Compatible with System 6.0.3",
with a separate "HyperCharger INIT" of 1987. It is the oldest of the set, and the one with the most interesting
answer to the sound problem.

- **Sound: Apple's synthesizers, re-tuned for the faster processor.** INIT -4047 runs only on the SE's ROM (the
  ROM version word at ROMBase+8 must be $0276) with a 68020 or better (CPUFlag at $12F of 2 or more). It patches
  _VInstall. When the Sound Driver installs its VBL task (the queue element at $26A) with one of the SE ROM's two
  synthesizer routines as its address, $436E48 (four-tone) or $436F60 (freeform), the patch puts in its own
  versions instead and falls back into the ROM ($436E22, $436E2A, $436F6A) for everything else. Read beside the
  SE ROM's own code (rom/original, disassembled), the replacements compute exactly what Apple's do, from the same
  tables and the same low-memory record at $262. What changes is **where in the buffer the writing starts**:
  Apple's four-tone routine starts at SoundBase+370 bytes (word 185 of 370) and GCC's at word 32; Apple's freeform
  routine starts at word 32 and GCC's at word 16, wrapping at the end of the 740-byte buffer as Apple's does. Both
  pad with the last sample when a sound runs out (the same silence-at-the-resting-level that Aurora68 uses, below).
  Our reading: the starting point only makes sense against the speed of the writer. The hardware is at word 342 when
  the VBL interrupt comes, a 68000 needs a few milliseconds for 370 samples, and a 68020 a fraction of that. The
  writer has to keep a consistent distance ahead of the playback position from frame to frame, and the distance that
  suited a 68000 does not suit a 68020.
- **ROM Copy.** The control panel can copy the ROM into RAM at start-up ("ROM Copy is installed"), so ROM code
  runs at the card's speed rather than the logic board's.
- **Speed test.** INIT -4046 turns the cache off and times a loop against the VIA's timer 1 to decide what it is
  running on.
- **Maths.** 'GCNS' resources are GCC's FP68K and ELEMS68K (SANE through the 68881), installed as 'PACK' 4 and 5.

### Dove MaraThon Racer 030

Dove Computer, known before this for its MacSnap memory upgrades, made the MaraThon Racer, a 68030 accelerator for
the Plus and the SE. The software is "MaraThon Racer 030 Plus/SE INIT/CDEV v2.3".

- **Timing constants for each cache state.** The control panel writes TimeDBRA and TimeSCCDB as one long at $D00:
  $0A3C/$0147 (2,620 and 327 loops per millisecond) with the 68030's caches on, $01EC/$00E7 with them off. Apple's
  short delays then stay the right length at either speed.
- **The floppy driver with the caches off.** 'PROC' 130 copies the .Sony driver (unit 4, refnum -5, found through
  UTableBase at $11C) and wraps its five entry points. Each call saves CACR, turns off and clears both caches,
  waits until the request is finished (ioResult no longer positive) and restores CACR.
- **Card and maths.** 'INIT' 71, "Mercury Cache Enable", finds the card with a MOVES read at $EC398000 in function
  code 3 and times a loop against Ticks; 'PROC' 131 points SANE at the 68882 and fixes _SysEnvirons' FPU field.
  Holding a key at start-up ("KEYDOWN") skips everything.
- **Sound: nothing.** Apple's Sound Driver is left as it is.

### Applied Engineering Warp 030 (1991-1993)

Applied Engineering of Texas built its name on Apple II cards, its TransWarp accelerators among them, before moving
to the Mac. "Warp 030 v1.5, (c) AE Research, Inc. 1991-1993" is the control panel of its 68030 card, and the
software that matches the folk memory that accelerators needed "a control panel or INIT that would modify the sound
code to get the timing correct".

- **Sound: slow down while it plays.** Of its fourteen 'init' patches, "Sound 1300" (158 bytes) installs a VBL task
  that watches SoundActive ($27E) every tick. When a sound starts it saves CACR and writes 0, both on-chip caches
  off; when the sound ends it restores CACR with the clear bits ($0808) set. "Sound/Sony 1340" (three variants, 812
  to 820 bytes) does the same for sound and for the floppy driver together, and also switches off the card's
  external cache, through MOVES in a separate function-code space. Apple's Sound Driver then runs at nearly the speed
  it was written for. The names 1300 and 1340 are the file's own; what they stand for is not stated.
- **The rest.** "Sony" wraps the floppy driver with the caches off as Dove did; "HWPriv" handles the cache-flush
  calls; "ATalk" looks at AppleTalk's state (LocalTalk runs on the SCC); "Gestalt", "Gestalt 68000" and "Shutdown"
  fill gaps in the older systems; Omega SANE comes in FPU and non-FPU forms; a RAM-timing dialog offers 150, 80
  and 60 ns. The help text says why the caches matter: "some drivers use timing variables calculated with [the
  instruction cache] turned on".

### Total Systems Gemini, sold as the Extreme Systems VANDAL/SE (1992-1995)

Total Systems of Eugene, Oregon, made the Gemini and Mercury accelerators; its control panel, GemStart, credits
Glenn L. Austin as its author in a long mock film-credits list in its STR# 128 resource. Extreme Systems sold a 50
MHz 68030 card for the SE's processor direct slot, the VANDAL/SE, with up to 16 MB and a video board for large
monitors, and shipped GemStart under its own name: "ExSys SE" 1.1a2 (October 1992). Connectix's Compact Virtual was
told the VANDAL was a "Gemini Integra".

- **Sound: a new Sound Driver.** The control panel carries its own DRVR 3 ".Sound" (1,068 bytes) in place of the SE
  ROM's. It sets the volume through VIA port A bits 0-2 at $EFFFFE with interrupts masked, enables the speaker by
  clearing VIA port B bit 7 at $EFE1FE when a sound starts, makes square waves with the VIA's timer 1 toggling PB7
  (ACR bits 7-6 set to $C0, the count in T1C-L and T1C-H at VIA+$800 and +$A00), and between sounds fills the whole
  buffer with the level the speaker is resting at, with MOVEP across the 370 words.
- **The rest.** Switches for the FPU ("QuickSANE"), the caches and "copy ROM to 32-bit RAM at startup"; patches to
  _ReadXPRam, _WriteXPRam and _SysEnvirons; code packed in 'Rpck' resources marked "1991, TOTAL SYSTEMS".
- **What its release notes record.** The SE's "Sound Manager may occasionally fail" with "an extended or repeating
  sound"; the 68030 at 50 MHz "tends to generate heat", and machines crashed after long use until the SE was
  ventilated; older SCSI disks were "too slow to respond to data accesses" from the faster processor (Syquest,
  Jasmine, Rodime 45 and 70, Western Digital, OMTI and Adaptec controllers, the HD-20).
- **The video board** ("ExSys SE Video" 1.3, April 1995) loads one of eight Xilinx FPGA configurations and a dot
  clock between 50.35 and 100.5 MHz for each of 25 monitors (Apple's Two-Page and Portrait, Radius, Sigma Designs,
  Lapis, Samsung, Triam and generic ECL, TTL, analog and composite settings), from 'MONS' and 'XLNX' resources.

### DayStar Digital PowerCache (1990-1993)

DayStar Digital of Georgia made cache cards and accelerators for 68020 and 68030 Macs, and later multiprocessor Mac
clones. Two versions of its control panel were read. Power Central 1.3 (December 1992) names PowerCache, PowerCard,
FastCache, Equalizer LC, PowerMath LC and FastCache Quadra, all for the II family, the SE/30, the LC and the
Quadra. Power Central 2.0, on "Disk Release 7" (19 March 1993), adds a **PowerCache for the SE and the Classic**: an
adapter board, "P33 or P34" marked, with memory of its own.

- **The SE's hardware: never cache the I/O.** Its 'mmuc' resource 0 is the 68030 MMU set-up for those machines: a
  24-bit table of sixteen 1 MB pages, $00000019 to $00400019 cacheable (RAM and ROM) and $00500059 upwards with the
  cache-inhibit bit set (the SCSI chip, the SCC, the IWM, the VIA).
- **Sound: nothing.** No Sound Driver and no sound patch. The sound buffer is ordinary RAM, which the 68030's
  write-through data cache writes straight to, and the VIA is uncached.
- **The rest.** Card detection by probing $F8000000 under a bus-error handler; a speed measurement against the VIA's
  timer 2 for the "Processor: nn MHz" line; PowerMath (SANE through the 68882) in a normal and an "enhanced" form;
  a hidden configuration dialog; a "Disk Spinup" parameter-RAM setting of 0 to 31 seconds.

### DiiMOCache (1994)

"DiiMOCache Accelerator CDEV v2.2, (c) 1994 DiiMO Technologies, Inc." is the latest of the set: an INIT, a control
panel and a SANE patch. It switches the caches, patches _SysEnvirons and SANE, and has no sound code.

### What the record shows

The compact Macs' sound was handled in three ways, and not at all by the rest:
1. **Re-tune Apple's synthesizers for the faster writer** (GCC, 1987-1988): the same code, started at a different
   place in the buffer.
2. **Replace Apple's Sound Driver** (Total Systems, 1991-1992).
3. **Slow the processor down while Apple's driver plays** (Applied Engineering, 1991-1993).
4. **Leave it alone** (Dove, DayStar, DiiMO), with the rest of the machine kept honest: timing constants per cache
   state (Dove), the floppy driver run with the caches off (Dove, Applied Engineering), the I/O space never cached
   (DayStar).

Aurora68 needs none of them, because Apple's SE Sound Driver never runs under it. The IIci ROM's Sound Manager
writes to an ASC that the kernel emulates, and the kernel fills the SE's buffer itself: half a buffer at a time,
always the half the hardware is not playing, timed from the SE's own vertical blanking (`snd_service` in
psmac_board_se.c; the hardware is at word 342 at the VBL edge, `SND_VBL_WORD`). That is GCC's lesson of 1987 taken
to its end: what matters is where the writer stands relative to the playback position, so place it by the beam,
not by the speed of the processor. Between sounds the kernel holds the buffer at the speaker's resting level
(`snd_quiet`), as Total Systems' driver and both of Apple's routines do.
