# PiStorm-SE: Emu68-based accelerator for the Macintosh SE — architecture & plan

Goal: a PiStorm in the SE's 68000 socket running our own bare-metal `kernel8.img`
(Emu68 fork) that presents a 68040, serves/augments the ROM from Pi memory, adds RAM,
virtual disks from SD-card images and partitions, networking (GbE/WiFi/BT), while keeping
the real NCR 5380 SCSI, display, sound, ADB keyboard/mouse and floppy working.

## 1. Hardware
- SE 68000 is DIP-64, socketed on many boards; PDS also carries all 68000 pins.
  Classic PiStorm (EPM240 CPLD) fits the socket; a PDS variant needs /BR takeover and is
  unproven after reset. Start with the socket.
- Frimples' `MacSE-PiStorm-DirectSocket` CPLD RTL adds /BR,/BG,/BGACK arbitration,
  E-clock + /VPA->/VMA for the VIA (6800-style cycles). Unvalidated on hardware; it is our
  CPLD starting point (classic PiStorm firmware lacks arbitration and FC handling).
- Gigabit Ethernet forces a Pi 4-class SoC: a CM4 carrier in PDS-card form puts the RJ45
  at the SE's rear expansion cutout. Pi Zero 2 W / 3A+ (WiFi only) fit the classic board.
  Decide early: classic board + Pi Zero 2 W (WiFi/BT only) first, CM4 PDS card later.

## 2. Pi-side memory map and bus policy (the core of the Emu68 port)
Emu68's bus layer is `src/pistorm/ps_protocol.c` (function pointers ps_read_*/ps_write_*)
plus Amiga assumptions in `M68k_Translator.c` (Kickstart at $F80000) and boards. We add a
"host = Macintosh SE" map:
| 24-bit range | Served by | Policy |
|---|---|---|
| $000000–RAMtop | Pi RAM | reads from Pi; writes **also forwarded** for the top $E000 (screen, alt screen, sound buffers), which the BBU reads from motherboard DRAM |
| $000000–$3FFFFF while overlay set | ROM image | first fetches after reset; cleared on VIA regA bit 4 write |
| $400000–$57FFFF | Pi ROM image (up to 1.5 MiB) | our patched ROM |
| $5FF000–$5FFFFF | real bus | NCR 5380 incl. pseudo-DMA (DTACK stall = DRQ wait, no timeout) |
| $9FFFF8/$BFFFF9, $DFE1FF, $EFE1FE.. | real bus | SCC, IWM, VIA (VIA via /VPA + E-clock) |
| one unused window (e.g. $F00000–$F7FFFF, undecoded on SE) | Pi | **PiStorm mailbox**: virtual block device, network, RTC, control |
| 32-bit space above 16 MiB | Pi RAM | only usable with a 32-bit-clean ROM (see §3) |
Addresses are masked to 24 bits for the SE map (board property, not CPU property).
RAM size autodetect: probe motherboard DRAM aliasing at boot (1/2/2.5/4 MiB) to place the
write-through window; or `mac.ram=` in cmdline.txt.

## 3. The memory question: 24-bit SE ROM vs 32-bit donor ROM
Facts (docs/01-rom-analysis.md): the SE ROM treats Emu68 as a 68020 and will boot a 68040,
but it has ~264 24-bit-dirty pointer-flag sites. A 32-bit-clean SE ROM is a MODE32-scale
reverse-engineering project; not recommended.

| | A. SE ROM (24-bit) | B. 32-bit-clean donor ROM + Pi glue |
|---|---|---|
| CPU seen by OS | 68020 (040 behaviour) | 68030/68040 |
| RAM | 4 MiB (8 MiB only by relocating ROM+I/O windows: large patch) | up to 128 MiB+ with System 7.1–7.5.5 |
| Real 5380 SCSI | native | native **only** with a 5380-based donor (IIci/IIsi 368CADFE is 32-bit clean in ROM; SE/30 needs MODE32); Quadra 900 (Frimples' choice) has 53C96 → Pi must bridge 53C96→5380 |
| Display | native | Pi emulates a pseudo-slot 1-bit 512×342 video card whose frame buffer is the SE screen RAM |
| Sound | native | Pi converts ASC FIFO (22.25 kHz) → SE 370-byte sound buffer |
| Floppy | native IWM | donor expects SWIM; IWM-only SEs may lose floppy |
| MMU | none needed | donor's PMOVE/MOVEC-MMU code → Emu68 has no MMU; patch donor's SwapMMUMode/MMU setup to a Pi "address-mode" register, or emulate the subset |
| Risk | low | high but with precedent (Frimples: 68040 + 128 MB + Quadra 900 ROM on Musashi) |

Recommendation: build A first as Milestone 1 (it exercises 100% of the hardware, bus,
ROM-serving, driver and SD infrastructure), then B as Milestone 2 reusing all of it.
For B, prefer the **IIci/IIsi ROM** (32-bit clean, 5380 SCSI Manager, VIA1 ADB like the
SE) unless we accept bridging SCSI for the Quadra ROM's colour/040-native advantages.
**Decision needed from you.**

## 4. ROM patches (Milestone 1, SE ROM)
1. Checksum: recompute (not disable) after patching.
2. TimeDBRA/TimeSCCDB calibration (0x40041C): saturate + replace SCSI delay loops with
   timer-based waits. This is the probable cause of "SCSI chip warm, no devices" in 2022.
3. Add ROM DRVRs (new resource-map entries) and open them in the boot driver loop
   (0x402FEA): `.PiDisk` block driver (multiple drive-queue entries, APM-aware, bootable),
   `.ENET`-compatible Ethernet driver (MacTCP/OT), `.PiCtl` (RTC/time sync, config).
   All talk to the Pi through the mailbox window; no 68k code touches Pi hardware.
4. Memory sizing: trust Pi-reported size (4 MiB) regardless of installed SIMMs.
5. Optional: ROM-disk volume appended to the >256 KiB ROM (ROM-inator style) for recovery.

## 5. Pi-side services (ARM, in kernel8.img)
- Storage: SD card via Pi SDHC/EMMC; FatFs (exFAT+FAT32) for image files
  (`.dsk/.img/.hda`, DiskCopy 4.2, BlueSCSI naming); raw Apple-Partition-Map partitions
  on the card also exposed; whole-card passthrough device for Mac-side formatting tools.
- Partition/format management (your requirement): two layers —
  (a) host tool `sesd` (Python CLI, later GUI): lays out the SD card (FAT32 boot with
  kernel8.img/ROM/cmdline, exFAT data), creates/resizes APM + HFS volumes (machfs/libhfs),
  converts image formats, injects files; (b) Mac-side: Lido / patched HD SC Setup / our own
  formatter work against the passthrough or image devices. Emu68 never needs to understand HFS.
- Network: lwIP + bare-metal drivers (Circle project has GENET GbE, BCM43xx WLAN, BT HCI
  drivers; GPLv3 vs Emu68 MPL-2.0 — licence check before adopting). 68k side sees a plain
  Ethernet driver; WiFi config via cmdline.txt/control panel.
- This is a fork: Emu68's AGENTS.md forbids ARM-side virtual devices and host-specific
  maps upstream. Keep the JIT core unmodified and isolate everything under `src/mac/`.

## 6. Milestones
M0 (now): toolchain, ROM tools (`romscan`, `romres`, `romfix`, patch assembler via
   m68k-linux-gnu-as), disassembly annotations. ✔ started
M1 Bring-up: Emu68 fork boots SE ROM from Pi memory in a Mini vMac-like test harness
   (qemu raspi3 + bus simulator), then on hardware: grey screen → Happy Mac from floppy/SCSI.
M2 Timing + SCSI coexistence: calibration patch; real SCSI disk boots. Floppy writes.
M3 Virtual disk: mailbox protocol, `.PiDisk`, exFAT images, APM partitions, boot from image.
M4 `sesd` host tool: partitioning, HFS create, image conversion.
M5 Network: `.ENET` driver + lwIP + WiFi (Zero 2 W); later GbE on CM4 PDS card.
M6 32-bit machine (donor ROM) per §3 decision: pseudo-slot video, VIA2, ASC, MMU shim.
M7 Bluetooth (ADB keyboard/mouse bridge or serial), polish, release images.
