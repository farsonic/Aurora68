# Aurora68

[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20me%20a%20coffee-support%20Aurora68-FFDD00?logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/farsonic)

A Raspberry Pi on a PiStorm in a compact Macintosh's 68000 socket. It runs Emu68 and boots Apple's Macintosh
IIci ROM, so the SE becomes a 68040-class Mac with up to 128 MB of memory, while its own screen, keyboard, mouse,
floppy and SCSI keep working. On top it adds a colour second screen and stereo sound over HDMI, disk images on the
SD card, WiFi, Ethernet, Bluetooth and USB keyboards and mice, USB sticks and a remote control, through a NuBus
card that exists only in software.

- **Macs:** the SE; the Plus, 512Ke, 512K and 128K with `mac.board=plus|512ke|512k|128k`.
- **Systems:** System 7.1 and System 6.0.8 tested. A/UX 3.1.1 runs on a real SE with the experimental 68030 MMU
  (branches, see `docs/25-aux-handover.md`).
- **Hardware:** a classic PiStorm (the DIP-64 Amiga board) with a Raspberry Pi 3A+ or Zero 2 W (they fit), or a
  3B/3B+ (overhangs), and a microSD card with a FAT32 first partition.
- **You supply:** your own Macintosh IIci ROM (368CADFE, 512 KB) and Mac system software. Nothing of Apple's is
  included.

## Quick start

1. Unzip the latest release from the Releases page onto the card's root.
2. Copy your IIci ROM there as `MacIIci_368CADFE.rom`.
3. In the card's root (macOS or Linux, needs `curl` and `python3`), run this one line. It fetches the Pi's boot
   files and every Pi model's WiFi and Bluetooth firmware, checks them, and builds the container `aurora68.cpio`:

   ```bash
   sh get-firmware.sh --container MacIIci_368CADFE.rom --declrom aurora68-se.declrom
   ```

4. Put the card in the Pi, the PiStorm in the 68000 socket (pin 1 at the notch end), plug in an HDMI monitor if
   you have one, and switch on. `cmdline.txt` as shipped is ready for an SE:

   ```
   enable_cache debug mac.board=se bt.pair=1 mac.console=hdmi
   ```

The Mac starts from its own SCSI disk (or a BlueSCSI), a floppy, or a disk image (`.img`, `.hda`, `.dsk`) in the
card's root. The Aurora68 programs are in the release's `Mac` folder; the drivers need no installing.
`READ-ME-FIRST.txt` in the zip says what the first start should look like.

## More

- `docs/guide.md`: the full guide. Every `cmdline.txt` option, `config.txt` and HDMI sizes, disks and SCSI images,
  the other Macs, the remote control, Setup and Recovery, larger startup disks, the hardware table.
- `RELEASE-NOTES.md`: what is new, and what has been checked on real hardware.
- `docs/00-overview.md`: the story of the project, with links to the working documents.
- `CLAUDE.md`: building everything from source and running the QEMU/MAME testbed.

## Licences

The Emu68 fork is MPL-2.0, the Alice68 modules in `emu68/src/alice/` are GPL-2.0-only, and the Macintosh side,
tools and documents are MPL-2.0. Third-party code compiled into the kernel (minimp3, dr_flac, stb_vorbis) and the
radio stations Aurora68 Play lists are credited in `docs/guide.md`, "Licences". Apple ROMs, system software and
Broadcom's firmware are not distributed.
