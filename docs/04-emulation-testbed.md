# Software testbed: MAME maciici + our ROMs/images (findings 2026-11-16)

Setup (all under `mame/`): MAME 0.264 from apt (`/usr/games/mame`, `chdman` from mame-tools).
ROM set: `mame/roms/maciici/` = IIci ROM split into 4 byte lanes (CRCs verified good).
SE ROM: `mame/roms/macse/macse.rom` matches MAME's expected SHA1, but `macse` also needs
the 1 KiB ADB modem MCU ROM `342s0440-b.bin` (not available) → cannot run.
Disk images → CHD: `chdman createhd -i img -o out.chd -c none`; attach `-hard1 file.chd`
(default SCSI ID 6; `-scsi:6 "" -scsi:0 harddisk` to move it).
Headless: `-video none -sound none -nothrottle -seconds_to_run N -snapshot_directory dir`.
Lua (`-autoboot_script`): keep tap/notifier objects in GLOBALS or they are GC'd and vanish.
Memory taps see only untranslated accesses: after the ROM enables the 68030 MMU, I/O
accesses were not visible on the 0x50F0xxxx alias (see mirror test in mame/mirror.txt).

Runs:
- IIci ROM, 8 MB, 640×480 13" RGB default: boots in ~6 s to the blinking "?" disk icon.
  VIA1, RBV video, ADB, ASC chime all exercised (access census in mame/iolog.txt).
- System 7.1 Zip image (96 MiB, APM: Iomega_DiskInfo, Apple_Driver "IoDr4.2"
  `.SC/IOMEGA`, Apple_HFS "Mac Volume" with valid LK boot blocks, System 7.1/Finder):
  no boot. The ROM's Start Manager selects targets and times out waiting for BSY
  (80 % of CPU time at 0x4080786A); MAME's SCSI helper logged "1 unread byte(s) lost" ×8,
  i.e. the few transactions that did happen failed in the data phase (MAME 0.264 helper
  vs this ROM's pseudo-DMA pattern). The Iomega driver may also refuse a non-Iomega target.
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
