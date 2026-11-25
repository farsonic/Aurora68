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
| Novy RailGun / Quik30, Mobius 030 | Plus, 512Ke and SE (README) | ".Sound020" jumps into the Plus ROM ($4179DA...) or the SE ROM ($436CAA...) by HWCfgFlags bit 2 |
| Glenn L. Austin, "Generic Sound Patches" | Plus and SE | compares the ROM version with $0075 and $0276 |
| DayStar PowerCache (Power Central 2.0) | SE and Classic adapters, besides the II family | |
| DiiMOCache 2.2 | not stated in the files | |

None of it is for the 128K and 512K with the 64K ROM. Those machines had accelerators of their own, but no software
for them has been read here, and the 64K ROM is not in rom/original to compare against, so the original Macintosh
appears in this history only through the hardware it shares with its successors.

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
1. **Move where the synthesizers start writing in the buffer, for a faster writer.** GCC patched Apple's routines in
   1987-1988; Total Systems shipped a whole driver in 1991 with the same two numbers (four-tone from word 32,
   freeform from word 16, against Apple's 185 and 32); Glenn L. Austin's later "Generic Sound Patches" did the same
   job for the Sound Manager's synthesizers. Novy's own relay into the ROM, with Apple's placement left as it was,
   is the case the fix was written for: "the distorted sound problem that otherwise accompanies the Novy boards".
2. **Slow the processor down while Apple's driver plays** (Applied Engineering, 1991-1993).
3. **Leave it alone** (Dove, DayStar, DiiMO), with the rest of the machine kept honest: timing constants per cache
   state (Dove), the floppy driver run with the caches off (Dove, Applied Engineering), the I/O space never cached
   (DayStar).

Aurora68 needs none of them, because Apple's SE Sound Driver never runs under it. The IIci ROM's Sound Manager
writes to an ASC that the kernel emulates, and the kernel fills the SE's buffer itself: half a buffer at a time,
always the half the hardware is not playing, timed from the SE's own vertical blanking (`snd_service` in
psmac_board_se.c; the hardware is at word 342 at the VBL edge, `SND_VBL_WORD`). That is GCC's lesson of 1987 taken
to its end: what matters is where the writer stands relative to the playback position, so place it by the beam,
not by the speed of the processor. Between sounds the kernel holds the buffer at the speaker's resting level
(`snd_quiet`), as Total Systems' driver and both of Apple's routines do.
