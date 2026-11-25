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

A second accelerator's software, one the owner remembers working well with sound, is to come; its Sound Driver is
the thing to compare with this one.
