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
floppy controller, the IWM, hands over its bytes as fast as the driver reads them, and the Sony driver leans on
the 68000's pace in a few places (the floppy part below says exactly where). Short
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
the bus (docs/22). The products below are the ones whose software has been read here: their control panels,
INITs and patches, unpacked from BinHex and StuffIt archives, and one from an 800K Disk Copy image, then listed
resource by resource and disassembled. None of that software is in this repository; it is the work of companies
long gone, and is described, not copied.

### The three generations of compact Mac

The same sound hardware runs through three generations, and the accelerators had to tell them apart:
- **The original Macintosh, 128K and 512K (1984):** the 64K ROM (version word $0069 at ROMBase+8) and a 400K
  single-sided drive. That drive takes its speed from the **low byte** of each word of the same 370-word buffer whose
  high bytes drive the speaker (the disk-speed PWM), so on these machines the sound buffer is shared between the
  speaker and the floppy.
- **The 512Ke and the Plus (1986):** the 128K ROM ($0075) with the 800K drive, which on Apple's account sets its own
  speed (the *Guide*; not re-checked here against a schematic).
- **The SE (1987):** the 256K ROM ($0276), the processor direct slot, and a second internal drive bay.

All the synthesizers read here, Apple's included, write the samples as **bytes into the high byte of each word**
(MOVE.B or MOVEP at even addresses, two bytes apart), leaving the low bytes alone. On a 400K machine that is what keeps
the drive's speed intact while sound plays. Aurora68's kernel does the same (byte writes in `snd_service` and
`snd_quiet`, psmac_board_se.c, shared by mac.board=128k, 512k, 512ke, plus and se).

Which machines the software read here was written for, from its own checks and notes:

| Product | Machines | How it tells them apart |
|---|---|---|
| GCC HyperCharger 020 | Plus and SE | its sound patch runs only on the SE ROM ($0276) |
| Dove MaraThon Racer 030 | Plus and SE ("Plus/SE INIT/CDEV") | |
| Applied Engineering Warp 030 | Plus and SE | compares the ROM version with $0075 and $0276; HWCfgFlags ($B22) bit 4 |
| Total Systems GemStart / ExSys SE | SE ("made for the Total Systems Gemini accelerators for the Macintosh SE") | |
| MicroMac Performer (GemStart licensed, 1995) | Plus and Classic (Killy-clip), SE (slot) | |
| MicroMac CD306 start-up ROM 3.1 (1993) | Plus (v1 to v3), SE, SE FDHD, Classic | the Mac ROM's checksum, read through the card |
| Novy RailGun / Quik30, Mobius 030 | Plus, 512Ke and SE (README) | ".Sound020" jumps into the Plus ROM ($4179DA...) or the SE ROM ($436CAA...) by HWCfgFlags bit 2 |
| Glenn L. Austin, "Generic Sound Patches" | Plus and SE | compares the ROM version with $0075 and $0276 |
| DayStar PowerCache (Power Central 2.0) | SE and Classic adapters, besides the II family | |
| DiiMOCache 2.2 | not stated in the files | |

None of it is for the 128K and 512K with the 64K ROM; those machines had accelerators of their own, but no software
for them has been read here.

**Apple's own placement, in every compact ROM.** The owner supplied the 64K ROM (the 128K's of January 1984,
checksum $28BA61CE, and the 512K's of October 1984, $28BA4E50, both version $0069) and the Plus's three ROMs (v1
$4D1EEEE1, v2 $4D1EEAE1, v3 $4D1F8172, version $0075) on 25 November 2026; their checksums verify, and they are
kept in rom/original, outside the repository's history. Read beside the SE ROM, all six carry the same two Sound
Driver synthesizers with the same placement: the four-tone routine (at $403646 in the 64K ROM, $417B7A in all three
Plus ROMs, $436E4C in the SE's) starts writing at SoundBase+370 bytes, word 185, and the freeform routine (at
$403712, $417CA2 and $436FA2) at SoundBase+64, word 32. So the placement GCC and Total Systems moved to words 32 and
16 was Apple's on every compact Mac from the first, and their fix applied to all of them. Novy's ".Sound020" relay
jumps into the Plus ROM at $4179DA to $417B58, the same Sound Driver a few hundred bytes before those routines.


### Who made what: a tangled family

The software above often carries one company's name and another's code, and a survey of the period's magazines
explains much of it: Steve Taylor's "68k Accelerators, FPUs and other CPUs" (Steve's Blog, savagetaylor.com,
published June 2022, updated August 2026; the owner sent it as a PDF on 25 November 2026), which lists over a hundred
products with their sources in MacWorld, MacUser, MacWeek, BYTE and others. What follows is taken from it and is
second-hand; where it meets the software read here, that is said.

- **The first accelerators.** The earliest the survey finds are Human Touch Computer Products' Three to One Touch
  Board (MacWorld, January 1986: a new 68000 at 12 MHz and up to 3.5 MB) and GCC's HyperDrive 2000 (March 1986: a
  20 MB disk, 1.5 MB of RAM, a 12 MHz 68000 and a 68881), followed that August by reviews of products from Novy,
  Quesse and Levco.
- **For the original 128K and 512K** the survey lists, among others, Levco's MonsterMac (10 MHz, with a fan, 256K of
  PROM and SCSI, needing changes to case and board), MacMemory's TurboMax (10 MHz, up to 4 MB, its own fan and power
  supply, a difficult installation with soldering), the Three to One Touch Board, the HyperDrive 2000, MacProducts'
  RailGun 020, and Total Systems' **Mercury 030 for the 128K, 512K, Plus and SE**. Its note: accelerators for the
  512Ke and Plus can often be used on a 128K or 512K that has Apple's 800K drive kit and its 128K ROM upgrade.
- **Novy Systems** (Edgewater, Florida; earlier System Technology Corporation) built boards under other names: its
  Mac20MX was sold as Total Systems' Gemini 020, "manufactured for Total Systems Integration of Eugene Oregon, by Novy
  Systems", and as MacProducts' RailGun 020; its Quik30 was sold as MacProducts USA's RailGun (Pro) 030. That is why
  the RailGun software read here is Novy's own (section on Novy, below).
- **Total Systems Integration**'s Mercury 030 was "designed by Quesse Computer Co Inc, and Engineering Solutions Inc.";
  its Gemini Integra (33/50 MHz) was "also sold by Extreme Systems as the Vandal SE" (MacWorld, June 1993), which
  fits the VANDAL's control panel being GemStart under another name.
- **MicroMac Technology** "has incorporated accelerators originally developed by such firms as DiiMO, Dove, Harris,
  and Mobius into its product line" (MacUser, July 1997). The survey says the Dove MaraThon Racer 030 (Plus, SE,
  Classic) was rebranded as the MicroMac Performer when Dove stopped making accelerators; the Performer's own
  software, read below, is Total Systems' GemStart with advice to choose "Mercury 030", and the modern rebuild of the
  board calls it logically identical to the Mercury. The record is not settled here.
- **DiiMO Technologies** (Austin, Texas; formerly Teknovation) sold a cache card first developed by Logica Research as
  the LogiCache, later sold by MicroMac.
- **Applied Engineering** bought SiClone Sales & Engineering, another accelerator maker, in the autumn of 1991, and
  sold TransWarp boards for the SE (16 to 40 MHz, with video and Ethernet options), the Classic and the LC.
- **The end.** System 7.5 changed the Memory Manager in ways most of these boards could not follow; with the move to
  PowerPC, that ended the market. The survey quotes DayStar's website of January 1998: its 030/040/601 upgrades "are
  only certified through System 7.5.1", and DayStar stopped making them in the autumn of 1996.
- **Today.** Several of these boards have been reverse-engineered and remade by members of the 68kMLA forum. The
  survey names Bolle's 25/33 MHz 68030 for the SE, sold by MacEffects as a clone of "the MicroMac Performer SE / Dove
  Marathon Racer SE", with its design files on GitHub; ZigZagJoe's remakes of SE/30 cards; and luRaichu's Plus board
  (below).

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

- **Sound: a new Sound Driver, with GCC's placement.** The control panel carries its own DRVR 3 ".Sound" (1,068
  bytes) in place of the SE ROM's; GemStart 2.1 ("(c) 1991, TOTAL SYSTEMS", sent separately with a note of its own)
  carries the same code byte for byte but for one padding byte. Its four-tone synthesizer starts writing at
  SoundBase+64 (word 32) for 338 words and wraps for 32, and its freeform synthesizer starts at a word offset kept in
  the driver itself, 16 in both copies read: **the same two numbers as GCC's patch**, against Apple's 185 and 32.
  The note that came with GemStart says what it was for: "It also works with Novy Quik30 accelerators, and fixes the
  distorted sound problem that otherwise accompanies the Novy boards." The driver also sets the volume through VIA port A bits 0-2 at $EFFFFE with interrupts masked, enables the speaker by
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

The same software lived on after Total Systems. **MicroMac Technology** of Aliso Viejo, California, sold the
**Performer** (called the "Universal" until February 1995), a 16 MHz 68030 with an optional 25 MHz 68882 for the
Plus and the Classic, clipped over the 68000 with a "Killy-clip", and for the SE in the processor direct slot. Its
control panel, "MM Performer 2.2.1 ... Licensed by MicroMac" (February 1995, creator 'TotS'), is GemStart again: its
credits read "Written by Glenn L. Austin @TS", its control panel, 'DPch' and 'DMgr' are byte for byte GemStart 2.1's,
and its DRVR 3 ".Sound" is the same 1,068 bytes, with the same four-tone and freeform placement. Its notes tell owners
to choose "Mercury 030" in Compact Virtual, which suggests the board is Total Systems' Mercury under licence, and its
SE installation steps include "Connect the audio cable" without saying more. Its release notes also record the
caveats of the age: software that wants "a 68030 processor" but not "a Compact Mac with an 030 accelerator", because
"some other resources are located in the ROMs for original 020/030 Macs which the compact Mac doesn't have".

**The Performer's second life (2025-2026).** The board itself has been reverse-engineered by members of the 68kMLA
forums (credited as Bolle and Trash80toHP_Mini, who worked out the Mercury 030 and the Performer), converted to KiCad
as "Performer-SE-PL-CL" (github.com/alxlab-zone66x), its five GAL16V8 chips dumped, and rebuilt as an open board for
the Plus, **Polyphloisboisterous '030** (github.com/luRaichu/polyphloisboisterous030, Rev. 0 November 2025, Rev. 1
December 2025; PCB files under CC BY-NC-SA). It is a 16 MHz 68030 with no FPU, piggybacked on the Plus's own 68000
through a DIP socket soldered on top of it, five GALs in PLCC sockets and through-hole-friendly parts, "50% smaller and
77% cheaper to manufacture over the original Performer". Its schematic names the signals of the arrangement this
history keeps coming back to: /BR_00 to the logic board's 68000 and /BGACK_30 from the card (the card takes the bus
rather than replacing the processor), /RESET_00 and /RESET_30 kept apart, C8M and C16M from the board, E, /VMA and
/VPA, /EXT.DTK, and the 68030's dynamic bus sizing (/DSACK0, /DSACK1) and /STERM. Its README tells the owner to install
**GemStart** ("identify your accelerator as a 'Total Systems Mercury'") and notes that "these accelerators affect the
Macintosh's Sound Manager. Audio playback may be garbled or cut in and out rapidly. I don't know why." The software
read above suggests why: the fix lived in GemStart's own Sound Driver (the Sound Driver's VBL synthesizers, placed
for a faster writer) and, for the System 7 Sound Manager's 'snth' synthesizers, in Glenn L. Austin's separate "Generic
Sound Patches"; a GemStart version or system that leaves either out leaves Apple's 68000 placement in charge. That is
a reading of the code, not a test on that board. It also records a disable switch the hardware itself provides: hold
the Interrupt switch after the chime and press Reset, and the board stays out of the way until the next start.

### MicroMac CD306 start-up ROM 3.1 (1993)

The card's firmware survives for one MicroMac board: a 32 KB start-up ROM (dumped twice over in a 64 KB file, named
by its owner for the MicroMac MultiSpeed) whose header reads "NewLife Computer Corp.", "MicroMac Technology", "CD306
Accelerator Start Up ROM", "Version 3.1", "14 July 1993", and, at $100, "BENJAMIN". The 68030 starts from it at
$F80200 (reset vector $00F80200, stack $003EFFF0); about 10 KB is code. Read in full, it shows how a 68030 card of
1993 took over a compact Mac without any software on the disk:

- **It sets the card up through a private function code.** Before touching the Mac, the ROM sets DFC to 3, a
  function code the 68030 never uses for itself, and writes card registers with MOVES at $03000000, $04000010,
  $05000010, $06000010 and $07000000, the setting carried in the address rather than the data. The settings come
  from three words in the ROM at $180, $190 and $1A0 ($0009, $0010, $0014); on our reading these are the speed and
  wait-state choices that gave the board its name (inference).
- **It tests the card and draws its own error screens.** Walking-bit tests of the card's registers and RAM, with
  the SE's screen at $3FA700 painted black and a code drawn on it for each failure, before the Mac's ROM runs.
- **It copies the Mac's ROM into its own RAM, patches the copy, and boots from it.** The ROM checks the Mac ROM's
  checksum through the card's window at $04400000, copies 128 KB (Plus) or 256 KB (SE, Classic) of it from $400000
  into card RAM, verifies the copy, and recognises five ROMs by checksum: the Plus v1 ($4D1EEEE1) and v3
  ($4D1F8172), the SE ($B2E362A8), the SE FDHD ($B306E171) and the Classic ($A49F9914). Any other 128 KB ROM, in
  practice the Plus v2, is treated like the v1. It then clears both caches, turns on the instruction and data caches
  with burst fill and write allocate (CACR $3919), and jumps to the copy's reset vector.
- **On the Plus it upgrades the ROM to v3 first.** On a v1 copy it makes the two one-byte changes Apple made in the
  v2 (at $1A4A a BEQ to BRA, at $7D44 a BEQ to BPL); on a v1 or v2 copy it writes Apple's v3 change (the SCSIReset
  moved out of the boot's SCSI routine at $7D40 into a new routine at $1FFC0, called from $8AC before it) and
  stamps the copy with the v3 checksum. Each
  of these is byte for byte what the v2 and v3 ROMs have (compared against the three Plus ROMs).
- **Then it fixes what a fast processor breaks on the Plus: the SCSI Manager.** Ten routines of the Plus ROM's
  SCSI Manager ($417294 to $417566) and the boot code's SCSI read at $7DCC are pointed at replacements in the card
  ROM ($F8259C to $F82878). Apple's versions time the 5380's reset and settle delays and its handshake timeouts with
  counted DBRA loops written for a 7.8 MHz 68000; the card's are the same routines with NOPs in the loops and new
  counts, one timeout raised in place (the $00017528 at $4173AE becomes $000507AC), and a blind read that gives up
  with an error when the target leaves the data phase instead of waiting forever. The SE and Classic get none of this: their ROMs calibrate their SCSI timing at start-up
  (TimeSCSIDB, inference), so their loops scale with the processor by themselves.
- **It tells the system what it is.** The ROM's processor test is replaced so that CPUFlag reads 3, a 68030 (Plus:
  `moveq #3,d1` at $590 and its MOVEC removed; SE and Classic: `moveq #3,d1; move.b d1,d7; rts` at $3EE); on the
  Plus, HWCfgFlags is set unconditionally ($3F0); it gets bit 3 always and bit 4 when
  a 68882 answers a probe through the F-line vector (Plus $C0 becomes $C8 or $D8; SE and Classic $C400 becomes $CC00
  or $DC00), the bit Applied Engineering's software also tests. The ROM's own checksum test is disabled (Plus $D9A,
  SE and Classic $26EA), since the copy no longer matches, and the Plus ROM's probe for a test ROM at $F80000 (at
  $F6CA) is pointed at $400000, because the card's own ROM sits at exactly that address.
- **The rest, on the Plus only.** A start-up hook at $66 that quietens the 5380 and the VIA before the ROM's own
  start-up, and a hook on system error 12 (unimplemented trap, $806) that saves every register just past the end of
  the screen buffer ($3FFC80), writes them in hex at the bottom of the screen, and then hands over to the ROM's
  system error.

**Nothing in it touches sound or the floppy.** No patch lands in the Sound Driver, the Sony driver or the IWM's
address space, no code reads SoundBase or the sound buffer, and the data cache is left on for good. Whatever kept
sound and floppy right on this board was either in its hardware (the I/O space kept out of the cache, inference) or
in MicroMac's software on the disk, the GemStart-derived Performer control panel being the one read here. The ROM is
also the earliest example in this history of the method Aurora68 uses: the Mac's ROM copied into the accelerator's
memory and patched there, never changed in its socket.

### Novy Systems RailGun and Quik30, also sold as the Mobius 030 (1992-1993)

Novy Systems made the RailGun and Quik30 68030 accelerators for the Plus, the 512Ke and the SE, with the ImagePro
board for large monitors. Its support software, "RailGun v5.42, (c) Novy Systems Inc. 1993", has a long version
history in its README (1992-1993): monitor after monitor for the ImagePro, 40 and 50 MHz boards, a DualScreen mode,
"Patched the operating system to support PLUS extended keyboards at 50Mhz", and "Fixed sound crash with System 7 and
512KE when using SysBeep with autopop bit set. This happens in Microsoft Word 5.1." Mobius Technologies sold the
same board as the Mobius 030: its "Mobius 030" control panel and "MobiusInit" are Novy's own Quik30Init 4.99 ("(c)
Novy Systems Inc. 1992") under another name, beside a "Mobius Display" panel for the video card ("(c) smal
productions 1992") and a small "Mobius 7.01 Fix".

- **Options**, from its 'XOPT' resources: CACHE, SOUND, SANE, COPY (the ROM into RAM), RAMD (a RAM disk, DRVR 11
  ".RamDisk") and ATALK.
- **Its Sound Driver is only a relay.** DRVR 3 ".Sound020" is 136 bytes: each entry point jumps straight into the
  ROM's own Sound Driver, the Plus ROM's ($4179DA, $417A7A, $417B58, $4179F4) or the SE ROM's ($436CAA, $436D4C,
  $436E2A, $436CC4), chosen by bit 2 of HWCfgFlags ($B22). So the ROM's synthesizers, with Apple's placement in the
  buffer, run at the card's speed, which fits the "distorted sound" that GemStart's note says Novy owners heard.
- **"Mobius Sound", the fix that came later.** The Mobius set also has an INIT called "Generic Sound Patches",
  "Copyright 1992, Glenn L. Austin", the author of Total Systems' GemStart. It patches _SysBeep and _GetResource,
  catches the Sound Manager loading its 'snth' synthesizer resources on the Plus (ROM $0075) and the SE (ROM $0276),
  and supplies synthesizers of its own that write the buffer in halves of 185 words and in runs of 150 and 337.

### DayStar Digital PowerCache (1990-1993)

DayStar Digital of Georgia made cache cards and accelerators for 68020 and 68030 Macs, and later multiprocessor Mac
clones. Two versions of its control panel were read. Power Central 1.3 (December 1992) names PowerCache, PowerCard,
FastCache, Equalizer LC, PowerMath LC and FastCache Quadra, all for the II family, the SE/30, the LC and the
Quadra. Power Central 2.0, on "Disk Release 7" (19 March 1993), adds a **PowerCache for the SE and the Classic**: an
adapter board, "P33 or P34" marked, with memory of its own.

- **Its MMU table is the Mac II's, not the SE's.** 'mmuc' resource 0 is loaded only when Gestalt 'mach' is 6 (the
  Mac II; Power Central 2.0's INIT at $276-$296, 1.3's at $238): sixteen 256 MB pages under a TC of $80F04D00,
  $X0000019, cache-inhibited from $50000000, the Mac II's 32-bit I/O space. (An earlier reading here took it for a
  24-bit SE table of 1 MB pages; the floppy part below corrects it.)
- **The SE/Classic adapter's options.** Power Central 2.0's "Classic/SE PowerCache Startup Configuration" dialog
  (DITL 131) offers "Sound Patch #1" to "#4", "Patch Floppy Driver", "Map ROM to RAM", "Protect ROM" and "System
  patch #1", kept as bits of a word in parameter RAM at $FE-$FF. No code in the INIT, the control panel or the
  'mmuc' resources applies them; they may be carried out by the adapter's own firmware (inference). So the SE
  PowerCache did have sound and floppy patches, of unknown content, and they are not in the files read here.
- **The rest.** Card detection by probing $F8000000 under a bus-error handler; a speed measurement against the VIA's
  timer 2 for the "Processor: nn MHz" line; PowerMath (SANE through the 68882) in a normal and an "enhanced" form;
  a hidden configuration dialog; a "Disk Spinup" parameter-RAM setting of 0 to 31 seconds.

### DiiMOCache (1994)

"DiiMOCache Accelerator CDEV v2.2, (c) 1994 DiiMO Technologies, Inc." is the latest of the set: an INIT, a control
panel and a SANE patch. It switches the caches, patches _SysEnvirons and SANE, and has no sound code.

### What the record shows

The compact Macs' sound was handled in three ways, and not at all by the rest:
1. **Move where the synthesizers start writing in the buffer, for a faster writer.** GCC patched Apple's routines in
   1987-1988; Total Systems shipped a whole driver in 1991 with the same two numbers (four-tone from word 32,
   freeform from word 16, against Apple's 185 and 32); Glenn L. Austin's later "Generic Sound Patches" did the same
   job for the Sound Manager's synthesizers. Novy's own relay into the ROM, with Apple's placement left as it was,
   is the case the fix was written for: "the distorted sound problem that otherwise accompanies the Novy boards".
2. **Slow the processor down while Apple's driver plays** (Applied Engineering, 1991-1993).
3. **Leave it alone** (Dove, DiiMO, and the MicroMac CD306's start-up ROM, which patches the Plus's SCSI Manager
   but nothing of sound), with the rest of the machine kept honest: timing constants per cache state
   (Dove), the floppy driver run with the caches off (Dove, Applied Engineering). DayStar's SE adapter offered four
   "Sound Patch" options, whose code is not in its control panel or INIT and may be in the adapter's firmware.

Aurora68 needs none of them, because Apple's SE Sound Driver never runs under it. The IIci ROM's Sound Manager
writes to an ASC that the kernel emulates, and the kernel fills the SE's buffer itself: half a buffer at a time,
always the half the hardware is not playing, timed from the SE's own vertical blanking (`snd_service` in
psmac_board_se.c; the hardware is at word 342 at the VBL edge, `SND_VBL_WORD`). That is GCC's lesson of 1987 taken
to its end: what matters is where the writer stands relative to the playback position, so place it by the beam,
not by the speed of the processor. Between sounds the kernel holds the buffer at the speaker's resting level
(`snd_quiet`), as Total Systems' driver and both of Apple's routines do.

## Floppy read and write on the accelerated compact Macs

*Written 25 November 2026, from the same software, read against the SE ROM ($0276) and the IIci ROM in rom/original,
both disassembled. The 64K ROM of the 128K and 512K and the 128K ROM of the 512Ke and Plus are not there: what is said
of those machines' drivers is inferred from the SE ROM's code, which carries their drives' support, from docs/18 and
from the accelerators' own checks, and is marked so.*

### The problem as Apple built it

The opening of this history says that the Sony driver "counts instructions". Read closely, the SE ROM's driver
counts surprisingly little. The IWM shifts the bits itself, from its own clock; the driver's part is to take each
byte (a GCR "nibble", one every 16 us, docs/08 item 93) before the next one replaces it, and on a write to hand over
each byte before the chip runs dry. Every long delay goes through a timer. What does depend on the 68000 is a short
list, and most of it is the read and write loops themselves.

The driver is ROM resource DRVR ".Sony" at $434680 (Open $4346EE, Prime $434A12, Control $43487C, Status
$4349BE, Close $434698). At Open it copies fifteen routine addresses from a table at $43469C into the low-memory
vectors $222-$25A (FigTrkSpd, DiskPrime, RdAddr, RdData, WrData, Seek, SetUpPoll, Recal, Control, WakeUp, ReSeek,
MakeSpdTbl, AdrDisk, SetSpeed and the nibble table, in Inside Macintosh's order) and three more into $B40, and
32 bytes of timing parameters from $4346C0 into its variables (SonyVars, $134, from offset 42). The IWM is reached
through IWMBase ($1E0, $DFE1FF on these machines, a register every $200), the VIA through VIA ($1D4).

- **The handshakes.** Reading, the driver spins on the data register (IWMBase+$1800 with Q6 and Q7 low) until bit 7
  is set, `move.b (a4),d5 ; bpl` (RdAddr $435B04, RdData $435BCC); the chip is put in mode $17 at $435268 (latch
  mode and asynchronous handshake among its bits, by MAME's iwm.cpp; up to $20000 tries, else -77 initIWMErr). Writing
  (WrData $435E24), it spins on bit 7 of the handshake register before each byte and looks at the underrun bit only
  once, at the end: bit 6 clear at $435F5A is -74 wrUnderrun. An underrun anywhere in the sector is latched and
  reported after it.
- **The search limits.** Only one is counted in loop passes: RdAddr gives the drive **128 passes** ($435B06,
  `moveq #127,d2`) to deliver three nibbles of any kind, else -66 noNybErr. Each pass reads the VIA as well as the
  IWM, so on a 68000 it lasts a few microseconds and 128 of them some hundreds, against the 48 us that three nibbles
  need **(inference: our estimate of the cycle counts)**. The rest are counted in **nibbles received**, whatever the
  processor: the address mark must come within 1,500 nibbles (1,468 for the self-regulating drive, $435B2E), else -67
  noAdrMkErr; the data mark within 49 nibbles of the address field ($435BD0), else -71 noDtaMkErr.
- **Short waits made of instructions.** The step strobe is `tst.b` of phase 3 high, two NOPs, `tst.b` of phase 3
  low ($434FE6-$434FEE). After each step the 800K drive's step line is polled up to **81 passes** with no timer
  ($435158), and on running out the driver simply goes on. The sequence that addresses devices chained on the
  external port (HD-20s, on our reading) spaces its phase-line changes with `move.l (sp),(sp)` ($435398-$4353BA).
- **The long waits are all timed by the Time Manager.** WakeUp ($4351A0) saves the driver's registers, puts its
  return address in $124, primes a Time Manager task through the trap table entry of _PrimeTime ($568, at $4351EE)
  for the delay given in units of 100 us, and returns to the caller; the task ($4351FA) resumes the driver later. A
  disk request therefore finishes in interrupt time, after the call that started it has returned. From the
  parameter table: 30 ms settle after a seek, 150-400 ms after a speed change (32 units for each unit of speed),
  400 ms motor on, 12 ms a step (half for the 800K drive in Recal), 10 ms between sectors, 750 ms to eject a 400K
  disk. The 800K drive is not waited on blindly: its READY line is read every millisecond, up to 1,001 times
  ($435178-$435198); a 400K drive gets the fixed waits. A VBL task every 30 ticks ($435542) turns the motor off.
  **Neither TimeDBRA nor TimeSCCDB appears anywhere in the driver.**
- **The 400K drive's speed.** SetSpeed ($435012) takes the zone (track / 16) and writes the drive's PWM into the
  **low bytes** of the sound buffer: 370 bytes, two apart, through the pointers at $B0A and $312, which the start-up
  code ($40024A) sets both to SoundBase + 1. Each byte is a state of a 6-bit shift-register sequence (its place in
  the sequence is the pulse length), dithered between two neighbouring states by a pattern table at $4350D2. The
  table of speeds is measured, not fixed: MakeSpdTbl ($4353DE) sets two trial speeds, waits 400 ms for each, and
  FigTrkSpd ($435484) times the tachometer (drive sense line 14) against **VIA timer 2** (T2C at VIA+$1000/$1200,
  the flag at VIA+$1A00), with interrupts masked; the five zones are interpolated toward targets at $4353C4. ReSeek
  ($434C80) measures again when more than 14,400 ticks (four minutes) have passed. A drive whose sense line 15
  reads high at Open (the flag at offset 13 of its record, which we take to be the 800K drive's) gets no speed
  control: MakeSpdTbl only waits 800 ms. Even so the ROM writes the PWM bytes once on such a machine, on the first
  SetSpeed, while the current speed at $138 still holds the -1 the start-up code ($40023C) put there. Which machine
  does what: the 128K and 512K have the 400K drive and need all of this; the 512Ke and Plus brought the 800K drive,
  which sets its own speed (the three generations above); the SE ROM still carries the whole 400K path. That the
  64K and Plus ROMs do the same is **inferred** from the SE ROM, not read.
- **Interrupts.** Prime raises the mask to level 1 at once (`ori #$0100,sr` at $434A12: no VIA interrupts, the VBL
  included, for the whole call), and SetUpPoll ($435ACC) raises it to level 3 for each sector's I/O. AppleTalk
  bytes arriving at the SCC meanwhile are not lost: inside every loop above, `tst.b (a5) ; bmi` reads VIA port A at
  $EFFFFE (bit 7, the SCC's wait/request line) and, when a byte waits, `move.b (a6),-(sp)` takes it from the SCC's
  data register at $9FFFFE onto the stack; the poll procedure at $13E replays them afterwards ($434EAA). Sense reads
  and strobes mask to level 3 as well ($434FC8, $434FE2).
- **The drives.** The driver numbers up to five positions ($434F3A): two internal ones told apart by VIA port A bit 4
  (both on the IWM's ENABLE1; the routine behind $B40, $435316), the external one on ENABLE2, and two more on the
  external port, reached by the phase-line sequence at $43537E (HD-20s, on our reading). The SE can carry the second
  internal drive; the 128K, 512K, 512Ke and Plus use port A bit 4 for the ROM overlay, and their ROMs, not read,
  presumably know one internal and one external drive. The ExSys SE release notes add the last
  variant: "The Macintosh SE has two ROM versions currently sold. The HD version allows a high density floppy drive
  to be used" (the SE FDHD, whose SWIM replaces the IWM; that ROM is not in rom/original).

So a faster processor endangers this driver in fewer places than the folk memory suggests: the 128-pass search, the
81-pass step poll, the strobe width and the external port's spacers. And one more that is not in the driver's code at
all: the time between two reads of the IWM. In the IWM's asynchronous mode a read that finds bit 7 set clears the data
register
**14 clocks later** (MAME's iwm.cpp, `m_async_update = m_last_sync + 14`; about 1.8 us at 7.8336 MHz), and until then
a second read returns the same nibble. On a 68000 the shortest path through RdData between two reads of the
register ($435C12-$435C20) is some forty clocks, five microseconds; a cached 68030 fetches those instructions in a
fraction of that, and the second read can come inside the window. Nothing in the driver would notice: the same
nibble taken twice is a checksum error (-72 badDCksum) or a bad address field (-69, -70), and a retry.
**(inference: MAME's model of the IWM and our count of the loop; the chip's specification was not to hand.)** A
68030's data cache, unless the card inhibits it for the I/O space, would also hand the polling loops the same byte
for ever.

### What each accelerator did about it

In date order, as the sound part above. Every wrapper below finds the driver the same way, through the unit table:
UTableBase ($11C) + 16 is unit 4, refnum -5, and its DCE's first long is the driver.

- **GCC HyperCharger 020 (1987-1988): nothing.** No resource of HyperCharger 2.0 or of its INIT touches the unit
  table, IWMBase or the Sony vectors. The 68020 has an instruction cache and no data cache, so the IWM's registers
  are never cached. Whether its loops ever came inside the IWM's read window is not known.
- **Dove MaraThon Racer 030 (Plus/SE): every call with the caches off, until it is finished.** 'PROC' 130 copies
  the driver's header (from the ROM, or from its handle when dCtlFlags bit 6 says the driver is in RAM) into a
  block of its own and points the DCE at it. Each of the five entry points saves CACR, clears its enable bits (EI,
  ED) and sets the clear bits (CI, CD), calls Apple's routine, then **waits while ioResult is positive** (`tst.w
  16(a1) ; bgt`, at $11A) and gives back only the two enable bits it took away (`andi.l #$101`). Waiting on
  ioResult is what makes it work with WakeUp: the request is not over when Apple's routine returns. There is no
  machine check in 'PROC' 130. Dove's other answer, a TimeDBRA and TimeSCCDB for each cache state (the Dove section
  above), does not concern the floppy, which uses neither.
- **Applied Engineering Warp 030 (1991-1993): two patches, of two strengths.** Both replace the DCE's driver
  pointer with a header of their own (and assume the driver is in ROM: no dCtlFlags test).
  - **"Sony"** ('init' 4, 218 bytes, the driver named ".AESony") passes Open, Prime, Status and Close straight
    through and turns the on-chip caches off (CACR without EI and ED, then the clear bits $0808 on the way back) only
    for **Control csCode 6, the format call**.
  - **"Sound/Sony 1340"** ('init' 8, 10 and 12) wraps Prime and Status (caches off for at least 64 ticks) and Control
    csCodes 5, 6 and 7, verify, format and eject (at least 120 ticks); every other Control call passes through. For an
    asynchronous call (ioTrap bit 10) it **puts its own completion routine** in ioCompletion, keeps the caller's in
    the parameter block's offset 18, and counts the requests outstanding, so the caches stay off until the last one
    has completed. Off means CACR 0 and the card's external cache off too, by MOVES byte writes with DFC set to 0 to
    addresses 0 and 2 ($2C2). A VBL task every tick counts down, and once the floppy is idle and SoundActive ($27E) is
    clear it turns the external cache on (addresses 0 and 3, or 0, 2 and 1, or 0, 3 and 1: the only difference
    between the three variants, presumably three cards) and restores CACR. The patches have no ROM test of their own;
    AE's INIT and control panel compare the ROM version with $0075 and $0276 (INIT -4064 at $13E8 and $13F0), which
    code they choose by it was not followed.
- **Total Systems GemStart 2.1, ExSys SE 1.1a2, MicroMac Performer 2.2.1 (1991-1995): a wrapper that is not installed
  on the SE, and a shutdown procedure for the SE alone.** 'ptch' 131 (392 bytes) and 'ptch' 129 (284 bytes) are byte
  for byte the same in all three. Neither the control panel, the 'DPch' (_ReadXPRam, _WriteXPRam, _SysEnvirons), the
  'DMgr' (by its strings, a loader for MacsBug), the other 'ptch' resources nor Performer's new 'ptch' 136 (SE ROM
  only, Resource Manager work) touches the floppy; the 'Rpck' resources, by their IDs the packed SANE packages 4 and
  5, were not unpacked.
  - **'ptch' 131** asks Gestalt for 'TotS' (the card's own selector: a type in the low byte, which must be below 4)
    and for 'mach', and **returns without installing on machine type 5, the SE**. Elsewhere (the Plus, the 512Ke, the
    Classic) it copies the header (ROM or RAM driver alike) and wraps all five entry points: CACR without EI and ED
    and, when the 'TotS' value has type 1 or 3 and bit 22 (which 'ptch' 129 also takes as "the MMU is on"), **the
    MMU switched off** (`pmove` of 0 into TC, the old TC kept) for the call; both come back when Apple's routine
    returns ($15E). It does not wait for
    ioResult, so the end of an asynchronous request runs with them on again. **Our reading of the TC switch:** with
    "copy ROM to 32-bit RAM" the ROM's code runs from the card's RAM through the MMU; with the MMU off the Sony
    driver runs from the logic board's ROM, at the logic board's speed, as Apple wrote it to.
  - **'ptch' 129** installs a shutdown procedure (_ShutDown, sdInstall). On the SE it runs, with interrupts masked to
    level 3, the IWM-to-ISM switch ($57, $17, $57, $57 to the mode register) and the phase-register test ($F5, $F6,
    $F7) that finds a **SWIM**, and if one answers writes $F8 to its mode-clear register: the same sequence, byte for
    byte, as the IIci ROM's probe at $4086EA88 (below), which the SE ROM ($0276) does not contain. On the other
    machines its procedure writes $7F to the VIA's interrupt-enable register. Why the SE FDHD's SWIM needed resetting
    at shutdown under GemStart is not stated anywhere in the files.
- **Novy RailGun and Quik30, Mobius 030 (1992-1993): the data cache off, the instruction cache by machine, until it is
  finished.** Every Novy file read carries the same 198-byte ".Sony" wrapper (Quik30Init 4.99 and the "Mobius 030"
  control panel with the header at $F4A and the installer at $1010, the "Mobius 7.01 Fix", RailGun 5.42's INIT in
  'CCOD' -2013, RailGunCdev and Patch701). Each entry point saves CACR and the parameter block, writes CACR without EI
  and ED, with the clear bits, and **ORed with a mask set at installation: 1 (the instruction cache left on) when bit
  2 of the byte at $B22 is set**, the bit by which Novy's Sound Driver tells the SE from the Plus, and 0 otherwise. So
  on the SE only the data cache is off; on the Plus and 512Ke both. After Apple's routine it **waits while ioResult is
  positive** ($FE4), as Dove's does, and restores CACR. It assumes the driver is in ROM.
- **DayStar PowerCache (Power Central 2.0, 1993): a checkbox whose code is elsewhere.** Its "Classic/SE PowerCache
  Startup Configuration" dialog (DITL 131, "Classic Configuration") offers "Map ROM to RAM", "Protect ROM", **"Patch
  Floppy Driver"**, "Sound Patch #1" to "#4" and "System patch #1", kept as bits of a word in parameter RAM at $FE-$FF
  (_ReadXPRam and _WriteXPRam with $000200FE, control panel $2470 and $2482). No code in Power Central's INIT,
  control panel or MMU resources touches the unit table, the Sony vectors or IWMBase, and the INIT never reads that
  word. Where the patch was applied is not in these files; the adapter board, with memory of its own, may well have
  had firmware of its own **(inference)**. Two corrections to the DayStar entry above follow from the same files: the
  SE/Classic adapter **did** have sound patches, four of them, of unknown content; and 'mmuc' 0 is not the SE's table.
  The INIT loads it only when Gestalt 'mach' is 6, the Mac II ($276-$296; Power Central 1.3 the same at $238), and its
  descriptors are $00000019, $10000019 ... $F0000059: sixteen 256 MB pages under a TC of $80F04D00 (TIA 4, TIB 13,
  32 KB pages), cacheable below $50000000 and cache-inhibited from it, the Mac II's 32-bit I/O space. How the SE
  adapter kept the IWM out of its cache is not shown by the software.
- **DiiMOCache 2.2 (1994): nothing.** No floppy code.
- **MicroMac CD306 start-up ROM 3.1 (1993): nothing.** It patches the Mac's ROM in card RAM before boot (see
  above), but none of the patches is in the Sony driver, and it turns the data cache on and leaves it on.

| | Calls wrapped | Caches during the call | Until | Machine paths |
|---|---|---|---|---|
| GCC HyperCharger 020 | none | (68020: no data cache) | | |
| Dove MaraThon Racer 030 | all five | both off | ioResult no longer positive | none |
| AE "Sony" | Control 6 (format) | both off | the call returns | none of its own |
| AE "Sound/Sony 1340" | Prime, Status, Control 5/6/7 | both off, and the card's | 64 or 120 ticks, and the last completion | none of its own |
| GemStart / ExSys SE / Performer 'ptch' 131 | all five | both off, and the MMU off (when the ROM is remapped, on our reading) | the call returns | not on the SE ('mach' 5) |
| GemStart / ExSys SE / Performer 'ptch' 129 | (a shutdown procedure) | | | SWIM reset on the SE only |
| Novy RailGun / Quik30 / Mobius | all five | data off; instruction off on the Plus/512Ke, on on the SE | ioResult no longer positive | $B22 bit 2 |
| DayStar Power Central 2.0 | "Patch Floppy Driver", code not found | | | Classic/SE adapter |
| DiiMOCache 2.2 | none | | | |
| MicroMac CD306 ROM 3.1 | none (data cache on throughout) | | | |

### What the IIci ROM changed

The IIci ROM's ".Sony" (a ROM resource at $4086C3E0, 16 KB to $40870550) is the SE's driver grown up, and Aurora68
runs it against the SE's IWM. Its entry points choose between two drivers by bit 0 of the byte at $DD1, which is
clear on the IIci (on our reading the bit that says a SWIM IOP is present, as on the IIfx); the IIci's own are Open
$4086CA38, Prime $4086CE94, Control $4086CBBA, Status $4086CD78 and Close $4086C9DC. Its timing parameters at
$4086CA0A are the SE's 32 bytes unchanged, and its vectors at $222-$25A point at routines that are, for GCR, mostly
the SE's. What Apple changed is precisely the list above:
- the no-nibbles search runs **576 passes** ($4086E18E, `move.w #575,d2`), and in RdAddr every wait for a nibble is
  preceded by `move.b (a5),(a5)` ($4086E1A0 and seven more), a read **and a write back** of VIA port A where the SE
  only read it: each pass and each nibble of the address field now costs two VIA accesses, which are paced by the
  VIA's own slow clock, not by the processor;
- the step strobe's two NOPs became the same VIA read and write ($4086D482-$4086D48C);
- after the 81-pass step poll comes a wait of TimeDBRA / 64 turns of a `dbf` on itself, about 16 us
  ($4086D530-$4086D536), the driver's first use of the calibrated constant;
- the IWM mode loop allows $80000 tries ($4086D652); sector I/O and register access mask interrupts to **level 7**
  ($4086E154, $4086D45C) instead of 3;
- the **400K speed control is gone**: FigTrkSpd ($4086D7F0) is `moveq #-1,d0 ; rts`, MakeSpdTbl ($4086D7E2) waits
  800 ms, SetSpeed ($4086CB60) is an `rts`, and nothing in the driver touches $B0A, $312 or the sound buffer. The
  400K drive's fixed waits (drives without the offset-13 flag) are still there;
- a **SWIM probe** at Open ($4086EA88: the $57/$17/$57/$57 switch, then $F5/$F6/$F7 through the ISM's phase register)
  sets SonyVars+312 when a SWIM answers, and the MFM paths it enables use TimeVIADB ($CEA: 18,808 x TimeVIADB /
  65,536 VIA polls, about 0.29 ms, at $4086EFF2) and TimeDBRA (3/4 ms at $4086F0A0, and at $4086F7C6). An IWM fails
  the probe and the driver keeps to the IWM paths, as the testbed showed (docs/08 item 93).

Unchanged: RdData's and WrData's loops (data mark within 49 nibbles at $4086E280, the underrun bit tested once at
$4086E616), the address-mark limits of 1,500 and 1,468 nibbles ($4086E1B8), and WakeUp through _PrimeTime
($4086D5C0). In RdData there is still nothing but a few instructions between two reads of the data register
($4086E2C2-$4086E2D0).

### What the record shows

The accelerators handled the floppy in four ways:
1. **Run Apple's driver with the caches off, until the request is really finished** (Dove; Novy, with the
   instruction cache left on for the SE; Applied Engineering's Sound/Sony, which also stops the card's own cache
   and follows asynchronous requests to their completion). Waiting for ioResult, or for the completion routine,
   matters because the Sony driver returns before it is done (WakeUp).
2. **Run it with the caches off for the call alone** (GemStart's 'ptch' 131, Applied Engineering's simpler "Sony"
   for formatting only), and in GemStart's case from the logic board's ROM with the MMU off.
3. **Leave it alone** (GCC with a 68020 and no data cache; DiiMO), or patch it by means not in the software (DayStar).
4. **Look after the chip, not the driver**: GemStart's shutdown procedure resets an SE FDHD's SWIM.

None of them changes a search limit or a timing constant of the driver: there was hardly any to change. What all the
wrappers have in common is that Apple's code runs uncached, which both keeps the IWM's registers out of a data cache
and puts time back between two bus accesses; on our reading the second is the dependency the driver never states, the
time between two reads of the IWM. Apple's own changes in the IIci ROM point the same way: VIA accesses in the loops
where the processor's speed had mattered, timed by a chip instead of by instructions. Nobody touched the 400K drive's
speed: every product read here was for the 512Ke, the Plus, the SE and the Classic, whose internal drives regulate
themselves, and the SE ROM's speed control kept working beneath them.

### What it means for Aurora68

Under Aurora68 the SE ROM's driver never runs; the IIci ROM's does, at JIT speed, and every IWM access is a cycle on
the SE's bus through the GPIO (psmac_board_se.c, the IWM window at $16000-$18000 of the IIci's I/O space, register n
to $DFE1FF + n x $200; `mac.board=se`, `plus`, `512ke`, `512k` and `128k` share it). There is no cache to turn off:
every access in the loops reaches the wire. What carries over from 1987:
- **The search limits hold.** Every limit the IIci driver counts in passes is paced by accesses to the real IWM and
  VIA, which are as slow on the PiStorm as on a 68000 or slower: 576 passes of a VIA read, a VIA write and an IWM read
  are milliseconds, against 48 us for three nibbles; the 81-pass step poll is of the same order as the SE's own
  **(inference)**. The long waits are the Time Manager's, which counts on a timer of the SE's real VIA, clocked at
  783 kHz as the IIci's is. The waits counted in TimeDBRA turns last what they should because the kernel measures
  TimeDBRA for self-looping `dbf`s (psmac_mbox.c `dbf_turns_per_ms`, with `mac.dbfslow`, docs/08 items 14, 27 and 50);
  with `mac.dbfslow=0` the 16 us after a step would vanish, which the step poll's own bus time makes harmless
  **(inference)**.
- **The read window is the open question.** In RdData two reads of the data register are separated only by the wire:
  the instructions between them take nanoseconds. If a read on the GPIO costs about a microsecond (docs/08 items 71
  and 93 put a byte write at a microsecond or two; psmac_bus.c a status register read of the PiStorm at one), the
  second read can come within the IWM's 14 clocks and take the same nibble twice; at two microseconds it cannot. The
  testbed cannot show it either way, its bridged cycles being 45-450 us apart (docs/08 item 93). **Recommendation:**
  measure on the SE how far apart two back-to-back IWM reads are on the wire; if under 2 us, let the board layer hold
  an IWM read until 2 us after a read of the data register that returned bit 7 set (a timestamp and a spin on the Pi's
  counter in the IWM window). The cost is at most 2 us a nibble against the 16 us it has. This is the PiStorm's
  version of what Dove and Novy did with CACR.
- **The deadline is per byte.** A nibble must be taken within 16 us of the last and, writing, handed over within the
  same time, or the sector is lost (a checksum error, retried) or the write underruns (-74, seen only at the end).
  The service core's own errands in front of a Mac cycle were up to 174 cycles before docs/08 item 93's fix and are
  at most one in front of an IWM access since (`psmac_serve_waiting`, psmac_bus.c); that figure is in cycles, from
  the testbed. On the SE, `mac.buswait=1` reports the longest wait in microseconds in front of an IWM access (docs/12,
  "The floppy and the modem port"): anything over about 10 us there is a lost nibble. The heartbeat and the screen
  rows are the errands to watch **(inference: their microseconds on the GPIO are not measured)**. docs/18's H1, the
  CPLD taking read data early, applies to IWM reads on the 128K-to-Plus boards ("floppy reads on a Plus will need
  it").
- **The 400K drive has no speed control under Aurora68.** On `mac.board=128k` and `512k` (and a 400K drive on the
  external port of any board) the IIci driver writes no PWM, and the kernel writes only the high bytes of the sound
  buffer (`snd_service`, `snd_quiet`), so the drive turns at whatever speed the low bytes happen to hold. MAME's model
  in the testbed (`macplus_ps_state::pwm_push`, reference/mame-psmac.patch) maps the PWM's duty cycle linearly onto
  the speed (342.5 rpm at 9.4 %, 702.5 rpm at 91 %), and low bytes of zero, by the same formula, onto about 301 rpm; a
  fixed speed can suit at most one of the five zones. README's "Their floppy drives (400K on the 128K and 512K) have
  not been tried yet" stands, with this as the expected result **(inference, not run)**. **Recommendation:** have the
  kernel write the PWM into the odd bytes, between the Mac's cycles as the speaker does (`psmac_serve_waiting`), for
  the zone of the track the driver seeks to. The track is best told by the driver itself: a ROM patch on the IIci
  driver's Seek (vector $236, $4086D4BA) calling the kernel through the mailbox, rather than the kernel guessing from
  the IWM's phase lines. The speeds would be the kernel's own, calibrated against the drive's tachometer as the SE ROM
  did, or measured once on the testbed's model; Apple's code is not to be copied.
- **The VIA write-backs reach the SE.** The IIci RdAddr's `move.b (a5),(a5)` is a write of port A on every pass. With
  the speaker on, `psmac_se_wire_write` replaces bits 0-3 with the speaker's; with `mac.sesound=off` the write puts
  back what `via_read_fix` returned, bits 0-2 set, so the volume lines go to 7 while an address field is read
  **(inference from the code; harmless while the speaker is off)**.
- **The SE FDHD.** Its SWIM will answer the IIci driver's probe, and the driver would then run the SuperDrive, MFM
  included, through the same register window. Not tried. Its MFM write path is the one user of TimeVIADB, which the
  kernel supplies as 1000 / the bridge's measured latency in whole microseconds (`psmac_mbox_set_calib`): a 1.4 us
  VIA access becomes 1,000 a millisecond, 40 % too many. Working in nanoseconds there would cost nothing.
- **The drives reachable.** One internal drive (VIA port A bit 4 driven low on the SE, `via_data`, docs/08 item 93)
  and the external one; the SE's second internal drive is outside what the IIci ROM knows.
