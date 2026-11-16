# Software testbed: MAME maciici + our ROMs/images (findings 2026-11-16)

Setup (all under `mame/`): MAME 0.264 from apt (`/usr/games/mame`, `chdman` from mame-tools).
ROM set: `mame/roms/maciici/` = IIci ROM split into 4 byte lanes (CRCs verified good).
SE ROM: `mame/roms/macse/macse.rom` matches MAME's expected SHA1, but `macse` also needs
the 1 KiB ADB modem MCU ROM `342s0440-b.bin` (not available) → cannot run.
Disk images → CHD: `chdman createhd -i img -o out.chd -c none`; attach `-hard1 file.chd`
(default SCSI ID 6; `-scsi:6 "" -scsi:0 harddisk` to move it).
Headless: `-video none -sound none -nothrottle -seconds_to_run N -snapshot_directory dir`.
Lua (`-autoboot_script`): keep tap/notifier objects in GLOBALS or they are GC'd and vanish.
Memory taps see PHYSICAL addresses. The ROM's 32-bit MMU tables map the I/O block to the
0x5000xxxx alias (A20-A23 ignored by the decoder), so after the MMU is enabled all I/O
traffic appears at 0x50010000 (SCSI), 0x50026000 (RBV) etc., not at 0x50F0xxxx. Tap both,
or just 0x50000000-0x50FFFFFF (mame/mirror.lua, results in mame/mirror.txt).

Runs:
- IIci ROM, 8 MB, 640×480 13" RGB default: boots in ~6 s to the blinking "?" disk icon.
  VIA1, RBV video, ADB, ASC chime all exercised (access census in mame/iolog.txt).
- System 7.1 Zip image (96 MiB, APM: Iomega_DiskInfo, Apple_Driver "IoDr4.2"
  `.SC/IOMEGA`, Apple_HFS "Mac Volume" with valid LK boot blocks, System 7.1/Finder):
  no boot, but the SCSI path itself is fine. Decoded dialogue (mame/scsi_dialog.lua):
  arbitrate (ID 7) → select ID 6 → READ(6) blk 0 (DDR "ER") → READ(6) blk 91 ×4 (Iomega
  loader "IoDr4.2") → READ(6) blk 1..6 (partition map, twice) → the loader reads its full
  driver blk 95..198 one block at a time → INQUIRY on the bus: MAME disk answers
  "SEAGATE", MAME CD-ROM (scsi:3) answers "SONY" → the Iomega driver refuses non-Iomega
  devices → no drive queue entry → ROM falls back to the "?" icon (and keeps retrying,
  hence 80 % of CPU time in the BSY-wait at 0x4080786A). The 8 "unread byte lost"
  messages are the 1-byte INQUIRY probes. Fix = a generic Apple_Driver on the image.
- System 7.0.1 image (10 MiB): a bare HFS volume (LK boot blocks, System 7.0.1, Finder)
  with NO partition map and NO driver → not bootable from SCSI by any Mac ROM as-is.
  Needs wrapping: DDR + APM + Apple_Driver + Apple_HFS. We must obtain or write a generic
  SCSI driver for that (also needed by the `sesd` host tool and for MAME tests).

Conclusions for the project:
1. MAME is a good reference for VIA1/RBV/ASC/ADB behaviour (sources in reference/mame-src)
   but its 5380 path is not trustworthy enough for boot testing with this ROM in 0.264.
   Options: newer MAME build, or our own harness using the Pi glue code + Frimples'
   `macse_scsi.c` virtual target (the glue is what we must write anyway).
2. Virtual disks served by our own ROM-resident `.PiDisk` driver avoid the on-disk driver
   problem entirely; images stay plain HFS (or APM) files on exFAT.
3. For real SCSI disks nothing changes: the IIci ROM's SCSI Manager + the disk's own driver.
