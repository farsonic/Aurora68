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

## SE accelerators of the time (read 25 November 2026)

The owner sent the software of an early SE accelerator, **Extreme Systems' VANDAL/SE** (a 50 MHz 68030 card for
the SE's processor direct slot, with up to 16 MB and a video board for large external monitors), as a BinHex'd
StuffIt archive. The software is not in this repository (it is someone else's); what it shows is:

- **The control panel "ExSys SE" 1.1a2 (October 1992) is Total Systems' "GemStart"** (the credits in its STR# 128
  name Total Systems of Eugene, Oregon, and Glenn L. Austin; its help strings speak of "the Mercury and Gemini
  accelerators"), rebadged. Compact Virtual 3.00/3.01 is told the VANDAL is a "Gemini Integra". Its switches: FPU
  for SANE ("QuickSANE"), the 68030's instruction and data caches, the startup icon, and "copy ROM to 32-bit RAM at
  startup". Its INIT and 'DPch' patch _ReadXPRam/_WriteXPRam ($A051/$A052) and _SysEnvirons ($A090), and look up
  _Unimplemented to test for traps; its code also comes in 'Rpck' resources marked "PACK ... 1991, TOTAL SYSTEMS".
- **It carries its own Sound Driver, DRVR 3 ".Sound"** (1,068 bytes), replacing the SE ROM's for the faster CPU.
  What its code does at the hardware: the volume into VIA port A bits 0-2 at $EFFFFE with interrupts masked; the
  sound enabled by clearing VIA port B bit 7 ($EFE1FE) when a sound starts; the **square-wave synthesizer by the
  VIA's timer 1 toggling PB7** (ACR bits 7-6 set to $C0, the count into T1C-L/T1C-H at VIA+$800/$A00; ACR's top
  bits cleared again at the end); and **silence by filling the whole sound buffer (SoundBase, $266) with the
  buffer's current first byte**, the level the speaker is resting at, with MOVEP across the 370 words. That last
  is what Aurora68's kernel does between sounds (`snd_quiet` in psmac_board_se.c, docs/08); the T1/PB7 square wave
  is docs/18 section 7's candidate 2 for the compact Macs' whine, used deliberately here.
- **Its release notes list the same troubles Aurora68 has met:** the SE's "Sound Manager may occasionally fail" with
  an "extended or repeating sound"; the 68030 at 50 MHz "tends to generate heat", with crashes after long use until
  the SE is ventilated; and old SCSI disks "too slow to respond to data accesses" from the faster CPU (Syquest,
  Jasmine, Rodime 45/70, Western Digital, OMTI and Adaptec controllers, the HD-20).
- **The video board ("ExSys SE Video" 1.3, April 1995)** loads one of eight Xilinx FPGA configurations ('XLNX'
  resources, about 8.3 KB each, encoded) and a dot clock (50.35 to 100.5 MHz) per monitor, with a 'MONS' resource of
  timings for each of 25 monitors (Apple Two-Page and Portrait, Radius, Sigma Designs, Lapis, Samsung, Triam and
  generic ECL, TTL, analog and composite settings).

The second archive, said to come with an accelerator that solved the sound problems, is **DayStar Digital's Power
Central 1.3** (December 1992), the control panel of DayStar's cache cards: its product list names PowerCache,
PowerCard, FastCache, Equalizer LC, PowerMath LC and FastCache Quadra, cards for 68030 and 68040 Macs (the II family,
the SE/30, the LC, the Quadra), not for the SE's 68000. It has no Sound Driver and no sound patch. Its INIT finds
the card by probing $F8000000 under a bus-error handler with a 68040 transparent-translation register (DTT1)
opened for the probe, measures the processor's speed against the VIA's timer 2 for the "Processor: nn MHz" line,
switches the cache through CACR, and patches SANE (PACK 4 and 5, "PowerMath"), _SysEnvirons and _Gestalt. On those
machines sound comes from the ASC with its own FIFO, which a faster processor or a cache does not disturb, so
whatever DayStar's sound reputation rests on, it is not in this file. The SE accelerator that handled sound well
is still to be found.

DayStar's next disk, **Power Central 2.0 (Disk Release 7, 19 March 1993)**, sent the same day as an 800K Disk Copy
image, corrects the picture: by then DayStar also sold a **PowerCache for the SE and the Classic** (an adapter board,
"P33 or P34" marked, with memory of its own; 68040 support had moved to a separate QuadControl). Its 'mmuc'
resource 0 is the 68030 MMU set-up for those machines: a 24-bit table of sixteen 1 MB pages, $00000019 to
$00400019 (RAM and ROM, cacheable) and $00500059 upwards (cache-inhibit bit set: the SCSI chip, the SCC, the IWM and
the VIA). That is the whole of its SE-specific treatment of the hardware: the I/O space is never cached, RAM and ROM
are. Again no Sound Driver and no sound patch: the SE's sound worked under it because the sound buffer is ordinary
RAM, which the 68030's write-through data cache writes straight to, and the VIA is uncached. Emu68 treats the Mac's
I/O the same way in Aurora68 (accesses to the board are never cached). Its notes add a "Disk Spinup" parameter-RAM
setting (0 to 31 seconds of waiting for SCSI disks at power-up) and PowerDemo 5.4 with SE and Classic support.

A third, **Dove Computer's MaraThon Racer 030 for the Plus and SE** ("MaraThon® Racer® 030 Plus/SE INIT/CDEV v2.3"),
said to be the one that handled sound well, holds no sound code either. What it does:
- **Re-times the ROM's delay loops when the cache changes.** The control panel writes the low-memory timing constants
  at $D00 (TimeDBRA, then TimeSCCDB at $D02) as one long: $0A3C/$0147 with the 68030's caches on, $01EC/$00E7 with them
  off. That is 2,620 or 492 DBRA loops per millisecond, against the 68000's few hundred. Apple's ROM and drivers time
  short waits (the SCC's access spacing among them) from these, so they stay right at any speed. Aurora68 does the
  same job differently: its ROM patch gives the IIci ROM the real bus figures from the mailbox (docs/06-rom-patches,
  docs/08 item 14).
- **Runs the floppy driver with the caches off.** 'PROC' 130 copies the .Sony driver (unit 4, refnum -5, found
  through UTableBase at $11C) and wraps its five entry points. Each call saves CACR, turns off and clears both caches
  (bits 8 and 0 cleared, 11 and 3 set), waits for the request to finish (ioResult no longer positive) and puts
  CACR back. The IWM's software timing is the reason.
- **Card and maths.** 'INIT' 71, "Mercury Cache Enable", finds the card through a MOVES read at $EC398000 with
  SFC 3 and measures the loop speed against Ticks; 'PROC' 131 points SANE (PACK 4 and 5) at the 68882 and fixes
  _SysEnvirons' FPU field. Holding a key at startup ("KEYDOWN") skips it.
So none of these accelerators carries special sound code beyond Extreme Systems' replacement driver. If the Racer
sounded better, the reason is in its hardware or in leaving Apple's own Sound Driver alone with correct timing
constants, not in a sound patch.

**Applied Engineering's Warp 030** (control panel "Warp 030 v1.5", (c) AE Research 1991-1993; with 'AECA' and
'USER' resources, a SANE-for-68882 'Icod' and Omega SANE for FPU and non-FPU machines) is the one that **does carry
sound code**, and it confirms the old advice that "there was usually a control panel or INIT that would modify the
sound code to get the timing correct". It has fourteen small 'init' patches, installed by machine: "Gestalt",
"Gestalt 68000", "HWPriv", "Sony", "ATalk", "Shutdown", "Sound 1300" and three variants of "Sound/Sony 1340" (the
numbers are the file's names for its variants; what they stand for is not stated). What the sound ones do:
- **"Sound 1300"** (158 bytes) copies itself into the system heap and installs a VBL task (_VInstall) that runs
  every tick and watches the low-memory byte SoundActive ($27E). When a sound starts it saves the 68030's CACR and
  writes 0: **both on-chip caches off for as long as a sound plays**. When the sound ends it puts the saved value
  back with the clear bits ($0808) set. So Apple's own Sound Driver, written for a 68000, runs at something close to
  the speed it was timed for.
- **"Sound/Sony 1340"** (812-820 bytes) does the same for sound and for the floppy driver (the .Sony DCE through
  UTableBase): it watches both, and while either is busy it turns off the on-chip caches and also the card's own
  external cache. It writes that through MOVES in a separate function-code space, with values like $78 and $40 and a
  comparison against $19.
- **"Sony"** wraps the floppy driver with the caches off as Dove did; **"HWPriv"** handles the cache-flushing calls;
  **"ATalk"** looks at AppleTalk's state in low memory (SCC-timed LocalTalk); the control panel's help also warns
  that "some drivers use timing variables calculated with [the instruction cache] turned on".

So the accelerator software of the time handled the compact Macs' sound in one of two ways: replace Apple's Sound
Driver with one written for the fast processor (Extreme Systems / Total Systems), or slow the processor down while
Apple's driver plays (Applied Engineering). Aurora68 needs neither, because Apple's SE Sound Driver never runs under it:
the IIci ROM's Sound Manager writes to an ASC that the kernel emulates, and the kernel fills the SE's PWM buffer
itself, two halves a frame, timed from the SE's own vertical blanking (`snd_service` in psmac_board_se.c), not from
the 68k side's speed.
