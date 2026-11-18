# Aurora68 (PiStorm-SE): how to build, test and change everything in this repository

This file is for whoever picks the project up, person or Claude. It says what the pieces are,
which tools and versions produce the binaries, how the testbed is run, and how each dependency is
moved to a newer version without losing the project's changes. The documents in `docs/` are the
long form: `docs/00-overview.md` is the story, `docs/08-emu68-port-status.md` the day-by-day
record of what was found and fixed (read it before touching anything that looks odd: it is
usually odd for a reason that is written there), `docs/09-mac-side.md` the Macintosh software,
`docs/07` the mailbox and the Alice68 boards, `docs/10` the second display, `docs/12` the hardware
bring-up plan (classic PiStorm, Pi 3, the SE), `docs/06` the MAME
bridge and the ROM patches.

## What this is
A Raspberry Pi in a Macintosh SE's 68000 socket (a PiStorm) runs Emu68, presents a 68040, boots
Apple's Macintosh IIci ROM (32-bit clean, Slot Manager) instead of the SE's, keeps the SE's own
screen, keyboard, mouse and SCSI, and adds memory, disk images, Ethernet, WiFi, Bluetooth, USB
input and a second colour display through a NuBus card that exists only in software (slot $9, a
mailbox window and a declaration ROM served from Pi RAM). Everything is developed and tested
without the hardware: Emu68 runs under QEMU's Raspberry Pi 3 model, the IIci motherboard runs in
MAME with a "bridge" CPU that forwards every bus cycle to the Emu68 side.

## Layout
| Path | What | Licence |
|---|---|---|
| `emu68/` | the Emu68 fork (kernel8.img), build variant `psmac`. `src/psmac/` is the Macintosh host layer; `src/alice/` the Alice68 service modules (network, Bluetooth, USB, status) with `alice_psmac.c` as their Macintosh glue. Vendored from the fork's own git history (branch `emu68-psmac` of this repository keeps it) | MPL-2.0 (Emu68), GPL-2.0-only (`src/alice/`) |
| `mac/` | the Macintosh side: drivers and programs in GNU as 68k assembly (no resource compiler, so they run under System 6 and 7). `make -C mac` builds all of them and the declaration ROM into `mac/build/` | MPL-2.0 |
| `tools/` | ROM patcher, container builder, testbed launchers, host network peer, QEMU/VideoCore screenshot, debugging helpers | MPL-2.0 |
| `tools/mac/` | resource fork / MacBinary / application / extension / declaration ROM builders, HFS helpers, MacTCP configurator, 'scrn' builder, resource adder | MPL-2.0 |
| `rom/` | ROM patch descriptions (`patches/*.json`) and the Makefile that applies them. ROM images are not in the repository | |
| `mame/` | the MAME testbed's Lua scripts (`shots.lua` takes a screenshot every ten seconds) and configuration; `mame/roms*`, `cfg*`, `nvram*`, `snap*` are local state | |
| `reference/` | `mame-psmac.patch` (the MAME changes: the bridged-CPU IIci machine and the bridge device) and `mame-src/` (unmodified MAME sources kept for reading); everything else under `reference/` is a third-party checkout that is recreated, not committed (see `reference/README.md`) | MAME: BSD-3-Clause/GPL-2.0 |
| `firmware/` | `bcm2710-rpi-3-b.dtb` for QEMU. The WiFi and Bluetooth firmware for real hardware is not included (see "Containers") | |
| `docs/` | the documents, with screenshots in `docs/img/` | |

## Prerequisites (versions this was built and tested with, Ubuntu 24.04)
- Cross toolchains: `gcc-aarch64-linux-gnu` 13.3 (the kernel; Emu68's toolchain file
  `emu68/toolchains/aarch64-linux-gnu-13.cmake` names the 13 series), `gcc-powerpc-linux-gnu`
  (Emu68's PowerPC ROM sub-build, built but unused here), `binutils-m68k-linux-gnu` 2.42 (the
  Macintosh assembly; only `as` and `objcopy` are used, no C compiler and no libc).
- `cmake` 3.28, `device-tree-compiler`, `xxd`, `python3` 3.13 (no third-party Python modules;
  everything in `tools/` is standard library on purpose).
- Emu68 is big-endian AArch64 and the Debian cross toolchain has no big-endian multilib
  headers: create an empty `/usr/aarch64-linux-gnu/include/gnu/stubs-lp64_be.h` once (only
  headers are needed; the kernel links with `-nostdlib`).
- `qemu-system-aarch64` 8.2.2 (`qemu-system-arm` package): the `raspi3b` machine, with
  semihosting; the VideoCore frame buffer it emulates is the second display in the testbed.
- MAME 0.264 built from source with the patch (below); its `chdman` makes the disk images.
- `hfsutils` 3.2.6 (`hmount`, `hcopy -m`, `hls`, `hdel`, `humount`, `hformat`): puts the built
  Macintosh files onto HFS images.
- Inputs that are not in the repository and must be supplied: the Macintosh IIci ROM
  (368CADFE, 512 KB) and, for the SE experiments, the SE ROM (B2E362A8); System 6 or 7.1 disk
  images; for hardware, Broadcom's WiFi/Bluetooth firmware files.

## Building each artifact
### The kernel (Emu68, variant psmac)
```
cmake -S emu68 -B psmac-build -DCMAKE_TOOLCHAIN_FILE=emu68/toolchains/aarch64-linux-gnu-13.cmake \
      -DTARGET=raspi64 -DVARIANT=psmac -DCMAKE_BUILD_TYPE=Release
cmake --build psmac-build -j4 && gunzip -kf psmac-build/Emu68.img.gz
```
`psmac-build/Emu68.img` is what QEMU loads and what goes on an SD card as `kernel8.img`.
Warnings are errors in this tree (`-Werror=misleading-indentation` among them): write
single-statement loops on their own lines. The fork's own sources are `src/psmac/*.c`,
`src/alice/alice_psmac.*`, `src/psmac/psmac_display.c`, plus small `#ifdef PSMAC` islands in
`src/alice/alice_net.c`, `src/raspi/start_rpi64.c` and `src/aarch64/*`; `emu68/AGENTS.md`
explains the fork's relationship to upstream.

### The Macintosh side
```
make -C mac            # drivers (.PiDisk, .ENET x2, .PiInput, .Display_Video_PiStorm), extensions
                       # (PiDisk, PiMach), applications (PiNet, PiWiFi, PiBlue, PiStatus, PiPing,
                       # PiScreens, PiColor), the declaration ROMs psmac.declrom / psmac_wifi.declrom
                       # and one per second-screen size (psmac-1024x768.declrom ...; RESES in the Makefile)
make -C mac check      # the drivers' structure offsets against the kernel's alice_net.h (cc + python)
make -C mac install IMG=../images/sys710pi.image [PART=1]   # PiDisk and PiMach into the System Folder
```
Everything is assembled with `m68k-linux-gnu-as -m68020` in GNU MIT syntax and packaged by the
Python tools (`mkapp.py`, `mkinit.py`, `mkdeclrom.py`). Conventions that cost time when broken
(all recorded in docs/08): Toolbox traps clobber D0-D2/A0-A1, keep state in D3-D7/A2-A6; QuickDraw
traps clobber A0, advance the pen before DrawString; a driver's Open runs in 24-bit mode, strip
master pointers; `SetRect` takes left, top, right, bottom; `divull` is the 32-bit divide; apps
must take every event (mask $FFFF) so disk-inserted events mount volumes; a slot interrupt
handler claims only what its board flagged, and a catch-all at priority 0 returns 1.

### The ROM patch
Put the original ROMs in `rom/original/` as `MacIIci_368CADFE.rom` and `MacSE_B2E362A8.rom`, then
```
make -C rom check      # dry run: every patch's "expect" bytes must still match
make -C rom            # rom/patched/MacIIci_psmac.rom (and the SE one)
make -C rom mame-lanes LANEDIR=mame/roms_patched/maciici    # the four byte-lane files MAME wants
```
`rom/patches/iici.json` is the patch description (docs/06-rom-patches.md): today it holds the
timing-calibration stub that reads the bus-loop constants from the mailbox window. Each patch
has an `expect` field; the tool refuses to apply a patch whose bytes have moved.

### The MAME testbed
```
git clone --branch mame0264 --depth 1 https://github.com/mamedev/mame reference/mame
git -C reference/mame apply ../mame-psmac.patch
cd reference/mame && make SUBTARGET=psmac SOURCES=src/mame/apple/maciici.cpp,src/mame/apple/maciici_ps.cpp,src/mame/apple/macse_ps.cpp \
     REGENIE=1 NOWERROR=1 TOOLS=1 -j4 && ln -sf psmac mamepsmac        # about 25 minutes on 4 cores
```
Dependencies are the usual MAME ones (docs/06-mame-bridge.md lists the Ubuntu packages). The
patch adds `src/mame/apple/maciici_ps.cpp` (the IIci with a bridged CPU), `bridge68k.cpp/.h`
(the bridge device: a CPU whose bus cycles arrive over two FIFOs) and one line in `mame.lst`.
`TOOLS=1` builds `chdman`. The ROM set goes in `mame/roms/maciici/` as the four lane files
(`make -C rom mame-lanes`, or the original lanes for an unpatched machine).

### Disk images
HFS images are plain files (`hformat -l Name image`), `.hda` files may carry an Apple partition
map (hfsutils takes a partition number). MAME reads them as CHDs: `chdman createhd -i x.image -o
x.chd -c none -f`, and **`-c none` matters**: the testbed machine serves compressed CHDs as raw
files (docs/08 item on "MComprHD"). The Pi-side disk (`mac.disk0=`) is a bare HFS image, 20 MB is
plenty. System software is Apple's and is not in the repository; `docs/09` describes the two
images used (System 7.1 and System 6.0.8), what was installed in them and in which folders.

### Containers for hardware
`tools/mkcontainer.py -o psmac.cpio rom/patched/MacIIci_psmac.rom mac/build/psmac.declrom
firmware/brcmfmac43455-sdio.bin ...` builds the initramfs the Pi firmware loads next to
`kernel8.img` (`config.txt`: `initramfs psmac.cpio followkernel`). The kernel also accepts a bare
ROM as initramfs, which is what the testbed does.

## Running the testbed
Two testbeds. The solo board needs no MAME: `tools/run-solo.sh 240 [options]` (QEMU with
`mac.board=solo`, the disk image and declaration ROM through semihosting, `SHOT=20` for periodic
screenshots in `$LOGDIR`, `CONTAINER=` to boot from a cpio); still one QEMU at a time (the
monitor socket is shared). The full stack with MAME:
```
ROM=rom/patched/MacIIci_psmac.rom HD=images/chd/sys710pi.chd tools/run-fullstack.sh 400 \
    mac.disk0=$PWD/images/pidisk/pidisk.img mac.declrom=$PWD/mac/build/psmac.declrom \
    mac.net=/tmp/psmac-net bt=off wifi=off
python3 tools/pinet.py /tmp/psmac-net peer      # the host side of the Ethernet, in another shell
```
`run-fullstack.sh SECONDS [kernel options]` starts MAME (`mamepsmac maciici_ps`, FIFO transport;
`MACHINE=macse_ps` for the bridged Macintosh SE, with `mac.board=se` and the 512x342 declaration
ROM on the kernel side; its PRAM lives in `mame/nvramFS/macse_ps/`)
and QEMU (one run at a time: the bridge socket, MAME's nvram and snapshot directories are shared,
and two runs side by side corrupt each other's results silently), writes `emu68.log` and `mame.log` to `$LOGDIR` (default `/tmp/psmac-logs`), and MAME
takes a screenshot every ten seconds into `mame/snapFS/maciici_ps/`. `PSMAC_RAM` (MiB, default 16)
is the Mac's memory. Useful kernel options (all in `docs/09`): `mac.inputtest=`, `mac.inputabout=`,
`mac.inputwifi=`, `mac.inputdrag=` (scripted mouse and keyboard, there is no real input in QEMU),
`mac.wifisim=SSID:PASSWORD` with `mac.wifinet=BASE` (a simulated WiFi radio), `mac.peek=ADDR,LEN`
(dump Mac memory at each heartbeat), `mac.scsitrace=`, `mac.screen=WxH` with the matching
`mac.declrom=$PWD/mac/build/psmac-WxH.declrom` (a larger second screen; the 'scrn' resource's
rectangle must agree, docs/09 "Two screens"), `mac.vcswap=0|1` (red/blue order on the Pi's screen).
`tools/vcshot.py out.png` photographs the
Pi's own screen (the second display) through QEMU's monitor socket. `tools/pram32.py` switches
32-bit addressing on in MAME's saved PRAM (needed for more than 8 MiB and for the second screen's
configuration; see docs/08 item 26). A System 7.1 boot reaches the Finder in about four minutes
on four cores; System 6 in three. `bt=off wifi=off` keep the real drivers from touching QEMU's
console UART and the missing WiFi chip.

The kernel log is the first place to look: `[MAC]` lines are the Macintosh drivers and programs
logging through the mailbox's INFO service (`log "text"` macro in the sources), `[PSMAC]`
heartbeats every five seconds show bus traffic, the 68k's PC, the slot interrupt line, the
display state and the network board. A run that stops printing mid-line is a core that died
with the console lock held (docs/08 item 21 explains the one time that happened).

## Tests
- `make -C rom check`, `make -C mac check` (static).
- MAME bridge end to end: `tools/bridge_e2e.py` (docs/06, "Tests").
- The scripted sessions above are the integration tests; the screenshots in `docs/img/` are
  their expected results (System 6 and 7.1 to the Finder with the Pi disk mounted; PiNet's ARP
  round trip; MacTCP pinging over Ethernet and over the simulated WiFi; PiWiFi's scan and join;
  About This Macintosh saying Macintosh SE; the two-screen desktop; PiColor's colours on the
  Pi's screen).

## Updating the dependencies
- **Emu68 (upstream michalsc/Emu68).** The fork's base is upstream's 1.1 line (the merge of
  `1_1_wip`, upstream commit 61a61f7). The project's code sits almost entirely in files upstream
  does not have; the islands in shared files are marked `#ifdef PSMAC`. The branch `emu68-psmac`
  of this repository holds the fork's own commits on top of a snapshot of upstream at that
  commit (its root commit is that snapshot, not upstream's history, which the development clone
  never had). To move to a newer upstream: check out `emu68-psmac`, add upstream as a remote
  and fetch it, reconnect the histories with `git replace --graft <root commit> 61a61f7`
  (git then treats the snapshot as that upstream commit), `git merge upstream/master`, resolve
  the few islands, rebuild with the command above, then copy the tree into `emu68/` on `main`. `docs/08` lists the
  upstream behaviours the fork depends on (the low-4-GiB identity map and its shadows, the
  semihosting path, the JIT's instruction cache model for addresses below 16 MiB).
- **MAME.** Clone the new release tag, `git apply reference/mame-psmac.patch`, fix what moved
  (the patch touches only the three new files and `mame.lst`; `maciici.cpp`'s device
  constructors are what `maciici_ps.cpp` inherits from, so compare it against
  `reference/mame-src/maciici.cpp`, the 0.264 copy kept for that purpose), rebuild, then
  regenerate the patch: `git -C reference/mame diff mame0XXX..HEAD > reference/mame-psmac.patch`.
- **QEMU.** Nothing in the project depends on a QEMU version beyond `raspi3b`, semihosting
  (`-semihosting-config enable=on,target=native`) and the `screendump` monitor command.
- **Alice68 modules** (`emu68/src/alice/`, from github.com/farsonic/Alice68). They are copied
  verbatim except `alice_psmac.*` (the Macintosh glue), `alice_net.c`'s `#ifdef PSMAC` hooks and
  the peripheral base (`ALICE_PERI`, now the kernel-space mapping). To take a newer Alice68,
  diff its `src/alice/` against this tree's and re-apply the hooks; the board protocol headers
  (`alice_net.h`, `alice_status.h`) are shared with the Macintosh drivers, and `make -C mac
  check` tells you if an offset moved. `src/alice/usb/` is Alice68's
  `third_party/Emu68/src/frf_usb/native/` (frf_dwc2*, frf_usb_native_hid*), also verbatim: the Pi 3's
  USB keyboard and mouse, with `src/psmac/psmac_usb.c` as its glue (docs/08 item 40).
- **Toolchains.** A newer aarch64 GCC needs a matching `emu68/toolchains/*.cmake` (copy the
  13 one and change the version). Nothing else is version-sensitive.

## Conventions
- Code is written to be read: a backstory at the top of every file, a reason beside every
  constant that was found rather than designed. The documents are written like a historian's
  record: what was tried, what it showed, what was concluded, with dates. Keep that up.
- The kernel presents a 68040 and the MAME testbed stays the reference environment even once
  hardware exists; both are the user's standing decisions.
- Never commit ROM images, Apple system software, disk images, firmware blobs, or the
  third-party checkouts under `reference/` (the `.gitignore` enforces it). Never put model
  names or session identifiers of AI assistants into code, documents or commit messages.
- Licences: the Emu68 fork stays MPL-2.0, the Alice68 modules GPL-2.0-only, the Macintosh side
  and tools MPL-2.0; new files carry an SPDX line.
