# Macintosh SE ROM B2E362A8 — structural analysis

Source: `rom/original/MacSE_B2E362A8.rom` (256 KiB, md5 9fb38bdcc0d53d9d380897ee53dc1322).
Disassembly: `rom/original/MacSE.dis` (m68k-linux-gnu-objdump, VMA 0x400000).
Tools: `tools/romscan.py` (header/strings/HW literals), `tools/romres.py` (resource map).

## Header
| Offset | Value | Meaning |
|---|---|---|
| 0x00 | B2E362A8 | checksum = 16-bit word sum of bytes 4..end, mod 2^32 (verified OK) |
| 0x04 | 0040002A | reset PC |
| 0x08 | 0276 | ROM version (SE) |
| 0x1A | 0001AF1C | ROM-relative pointer to resource map prefix; real 28-byte map header at +4 (0x41AF20) |

The checksum routine must be patched or the checksum recomputed after every change (trivial; `tools/` will get `romfix.py`).

## CPU detection (0x4003EE–0x40041A) — matters for the 68040
```
4003fe  moveq #2,d1          ; assume 68020+
400400  moveq #1,d0
400402  movec d0,CACR        ; illegal on 68000 -> vector 0x10
...     handler: d1 = 1 if frame vector-offset word == 0x10 (68010+), else 0 (68000)
400418  move.b d1,d7  ->  stored to CPUFlag ($12F) at 0x4000A0
```
Emu68 (68040) executes `movec` without trapping, so **CPUFlag = 2 (68020)** and the ROM
installs the 68020 A-trap dispatcher at 0x402CB6 (vector 0x28 set at 0x400762). That
dispatcher uses `movec CACR` with bit 3 (68020 "clear I-cache") — harmless on an 040.
There is no 68040-specific code anywhere (no CINV/CPUSH); none is needed.

## 24-bit dirtiness
- 264 direct `tst.b/btst/bset/bclr #5..7,(An)` on pointer high bytes; 1 StripAddress-style mask.
- Conclusion: the SE ROM cannot be made 32-bit clean by spot patching. Anything beyond the
  24-bit map needs a 32-bit-clean donor ROM (see 00-overview.md).

## Hardware addresses baked into the ROM (24-bit SE map)
| Device | Address(es) | Refs | Notes |
|---|---|---|---|
| SCSI (NCR 5380) base | $5FF000 rd / $5FF001 wr; regs at +reg*16 | lit. at 0x4004B4, 0x4004CE, 0x401C74 | stored to SCSIBase $C00; pseudo-DMA/handshake $5FF200 -> $C04/$C08 |
| VIA | $EFE1FE (+ $200*n register stride) | 17 | ADB, overlay (regA bit 4), sound/video page bits, timers |
| IWM | $DFE1FF | 4 | floppy; stored to IWM lo-mem $1E0 |
| SCC | $9FFFF8 rd / $BFFFF9 wr | 8 / 7 | serial / LocalTalk |
| ROM | $400000 | 27 | ROMBase ($2AE) set at 0x400262 |
| RAM top | $400000 (4 MiB) / $800000 | 14 | memory sizing |
| Overlay RAM | $600000 | 7 | RAM alias while overlay set |

Everything the Pi must forward to the real bus is in this table; everything else can be
served from Pi memory.

## Timing calibration (0x40041C–0x4004B2) — must be patched
- TimeDBRA ($D00) and TimeSCCDB ($D02) are measured with `dbf` loops against VIA timer 1
  and stored as 16-bit words. At JIT speeds the count overflows 16 bits.
- Used by the SCSI Manager (0x432510, 0x433CA0, 0x433CBA `mulu TimeDBRA`) and others for
  delays. Wrong values -> SCSI phase timeouts fire early -> "no devices" (matches AlexCL's
  2022 symptom of SCSI failing only with PiStorm installed).
- Fix: patch the calibration to saturate, or better, replace delay loops with a VIA timer
  (or Emu68 CNTVAL control register) based wait. Candidates: all readers of $D00/$D02.

## Resource map (13 types, 28 resources, 0x41B122–0x43CBB0)
Ref entries are 12 bytes: ID(2) nameOff(2) attrs(1)=0x58 dataOff(3, ROM-relative) + 4 reserved.
Each resource datum is preceded by an 8-byte fake Memory Manager block header
(`C0 | size24` , `relHandle`), e.g. .Sony: `C0 002610 00000074`.

| Type | ID | Name | Data | Size |
|---|---|---|---|---|
| DRVR | 3 | .Sound | 436C90 | 920 |
| DRVR | 4 | .Sony | 434680 | 9744 |
| DRVR | 9 | .MPP | 43312A | 5462 |
| DRVR | 10 | .ATP | 4324AA | 3200 |
| DRVR | 40 | .XPP | 437028 | 2568 |
| SERD | 0 | (serial drivers .AIn/.AOut/.BIn/.BOut) | 431BAE | 2300 |
| PACK | 4,5,7 | SANE / Int utils | 41B122.. | |
| bbmc | 0 | (82 570 bytes — ROM "picture" easter egg + misc) | 41D924 | 82570 |
| CDEF/MDEF/WDEF/MBDF/KCHR/KMAP/CURS/FONT/tcsl | | | | |

Adding a ROM DRVR = append `blockhdr + DRVR` data, add a 12-byte ref entry + 8-byte type
count bump (or a new type entry), extend the map, fix checksum. Because the ROM is served
from Pi memory it can grow past 256 KiB (window $400000–$57FFFF = 1.5 MiB).
Boot code that opens ROM drivers by ID: 0x402FEA–0x403114 (`GetResource('DRVR', id)`
followed by DrvrInstall/Open); this is the hook to open our drivers early enough to boot.

## Exception frames
Bus-error handler installed from the SCSI code at 0x41A534 (vector 0x8) expects the
68010+ frame layout to read/restore; with CPUFlag = 2 the 68020 paths are used. 68040
bus-error (access fault, format 7) frames differ from 68020 (format A/B) — Emu68 produces
its own frame format; verify against the SCSI bus-error recovery path on hardware.
