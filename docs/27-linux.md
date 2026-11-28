<!-- SPDX-License-Identifier: MPL-2.0 -->
# 27. Debian m68k Linux on Aurora68

*28 November 2026.* The owner wants a modern Debian m68k on the SE, built on his MacBook with one script and copied
to the SD card. A/UX 3.1.1 had shown the week before that the emulated 68030 MMU (`mac.mmu=full`, docs/23 sections 11
and 12) and the board layer's IIci chips (VIA, RBV, SCC, NCR 5380, ADB through the VIA, docs/25) carry a Unix that
drives the hardware itself. Linux/m68k has a Macintosh port that knows the IIci. This is the record of the two
pieces made that day: the kernel starting Linux directly, without Mac OS and without Penguin (`mac.linux=`), and the
build script (`tools/linux/`). Where something is our reading and not an observation it says *inference*.

## 1. What Linux wants from a Macintosh booter

Read in Linux 6.18.65 (`arch/m68k/kernel/head.S`, `arch/m68k/mac/config.c`, `arch/m68k/include/uapi/asm/bootinfo.h`
and `bootinfo-mac.h`, `arch/m68k/kernel/setup_mm.c`), the source tree the build uses:

- **Where.** The kernel is linked at 0 (`PAGE_OFFSET` 0 on a Motorola MMU); the ELF's first segment starts at 0 with
  the ELF header in the page below `_stext` ($1000), the entry is `_start` ($2000), one page above it. head.S runs
  position-independent with the MMU off, takes the physical address of `_stext` as the kernel's start, and maps
  `PAGE_OFFSET` to it: loading the segments at their addresses with the memory chunk at 0 is what Penguin does.
- **The bootinfo** is a list of records (`tag`, `size`, data) **right after `_end`** (head.S's `get_bi_record` starts
  at `lea %pc@(_end)`), ended by a zero tag; head.S puts its first page tables in the page after the list. The bss
  is the booter's to clear. The machine-independent records: `BI_MACHTYPE` (3, Macintosh), `BI_CPUTYPE`,
  `BI_FPUTYPE`, `BI_MMUTYPE` (2 each: 68030, 68882, 68030), `BI_MEMCHUNK` (address, size), `BI_RAMDISK`,
  `BI_COMMAND_LINE`. The Macintosh ones Linux reads (`mac_parse_bootinfo`): model (the Gestalt ID, 11 for the IIci),
  the screen's physical address, depth, row bytes and dimensions (`height << 16 | width`), its logical address, the
  SCC's base, boot time and GMT bias (printed only: Linux reads the clock chip itself), memory size, CPU id (used
  only when the model is missing) and the ROM's base. The kernel says which version it speaks at `_stext` (a branch,
  `BIV^Z`, then machine and version pairs): Macintosh 2.0.
- **The state.** Supervisor mode, interrupts masked, the MMU off. head.S sets its own stack, the VBR, the caches
  and the 68030's tables (TT1 over the NuBus space $F8000000-$FFFFFFFF, the 4 MiB around the screen at
  $F0000000, the ROM and the I/O block one to one). Linux's `via_init` turns off and sets up both VIAs' interrupts and
  timers and port B's clock chip lines, `macii_init` port B's ADB lines and the shift register, the SCC driver its
  chip. The one thing it takes as the ROM leaves it is **VIA1 port A's data direction**, which on the SE selects the
  screen buffer and the sound buffer (docs/08 item 93): the kernel writes DDRA $2F and port A $08 before the
  68k starts (`$6F` reached the SE's wire under the IIci ROM, the board layer adding PA6, *inference* for the rest).

## 2. `mac.linux=` (`emu68/src/psmac/psmac_linux.c`)

Options: `mac.linux=NAME` (the ELF, `vmlinux`, in the card's root; a name with a `/` is a host file in the testbed,
through semihosting), `mac.linuxargs=...` (Linux's command line, quotes around spaces; default
`root=/dev/sda rw console=tty0`), `mac.linuxinitrd=NAME` (an initramfs, put at the top of memory and named in
`BI_RAMDISK`), `mac.linuxscreen=se|hdmi`, `mac.scclog=on|off` (below). `mac.mmu=full` is required, and the SE or solo
board: otherwise one `[LINUX]` line says why and the IIci ROM starts as usual.

1. In `psmac_setup_memory`, while core 0 still has the card (before the mailbox and core 3): the file is read into the
   upper half of the Mac's RAM and checked (32-bit, big-endian, `EM_68K`), the initramfs into the top quarter.
2. At the end of `psmac_setup_memory`, once the screens exist: the `PT_LOAD` segments are moved to their addresses
   and their bss cleared, `_end` is taken from the symbol table (the highest segment end for a stripped file), the
   version table is checked, a screen is switched on (below), the bootinfo is written at `_end`, and the reset
   vectors at 0, which the generic start-up reads, become Linux's: SP `_stext`, PC the entry.
3. In `M68K_StartEmu`, as the 68k is released: VIA1 port A as above, and on the SE board the Pi's keyboards and mice
   as ADB devices at once (`psmac_adb_linux`), so that Linux's first scan of the bus finds them beside the SE's own.
   The boot timeline says `68k released: Linux's first instruction ($00002000)`.

**The screen.** No Mac OS driver opens the slot 9 card, so `psmac_display_linux` does what the driver would: the
display block's depth and row bytes, the colour table, `PSMB_DISP_ENABLED`. The SE's screen is one bit, 512 by 342 at
$F9980000 (Linux's MONO01 is the Mac's order: 1 is black); an HDMI screen with an eight-bit mode gets eight bits and
the Linux console's sixteen colours in the table. Linux's `macfb` finds the address inside slot 9's space, calls the
card "Generic NuBus" and writes no palette (the logo's colours are the grey ramp's). On the SE board the board
layer copies the frame to the SE's own screen as for Mac OS (`psmac_se_mark`).

**The serial ports in the log.** What the 68k writes to the SCC's data registers goes into the kernel's log a line at
a time, `[SCC A] ...` and `[SCC B] ...` (`psmac_linux_scc`, on the solo board's SCC model and on the SE board's way to
the real chip). head.S's early console prints to both channels, the serial console to channel A, so the testbed reads
Linux's messages without a screen, and on the SE `aurora68.log` shows what a cable on the modem port would.

**The boot counter** (docs/08 item 106) is not counted for a Linux start: no Finder would put it back to 0, and the
fourth start would have been a recovery start.

## 3. The first runs, on the solo board

The kernel: Linux 6.18.65 (the newest longterm release that day), `mac_defconfig` with every module turned off and
`tools/linux/aurora68.config` over it, cross-built here with Ubuntu's `m68k-linux-gnu-gcc` 13.3: a 7 MB ELF, 6 MB
loaded. Two things about the build were found first:

- **6.18.65 does not build for m68k as released**: `include/vdso/math64.h` uses `OPTIMIZER_HIDE_VAR` without including
  `<linux/compiler.h>`, which other architectures get through their own headers ("implicit declaration of function
  'OPTIMIZER_HIDE_VAR'" in `init/calibrate.c` and `arch/m68k/mac/via.c`). `tools/linux/patches/0001-...` adds the
  include; the build script applies every patch there.
- **Modules off makes every `=m` a `=y`.** The first build (merge of the fragment with `CONFIG_MODULES=n` only) was a
  16 MB kernel with mac_defconfig's test modules built in (`CONFIG_TEST_DHRY` and thirty more). The `=m` lines are
  now turned off before the fragment is merged: 507 options, a 6 MB kernel.

Run by run (`tools/run-solo.sh`, QEMU, `mac.mmu=full`, 64 MiB, the testbed's kernel image; logs in the scratch area):

1. **The kernel ran at the first try** (`mac.linuxargs="console=ttyS0 earlyprintk debug"`, a blank disk): head.S's
   letters `ABCFGHIJK` on both SCC channels, `Linux version 6.18.65-aurora68`, `Saving 178 bytes of bootinfo`,
   `Detected Macintosh model: 11`, the bootinfo's screen and SCC echoed back, `Apple Macintosh IIci`, 58468K of 65536K
   available, the VIA1 clocksource, `macfb: framebuffer at 0xf9980000 ... mode is 640x480x8`, the SCC's two ports as
   ttyS0 and ttyS1, `adb: Mac II ADB Driver`, `scsi host0: Macintosh NCR5380 SCSI`, the disk as sda, and the panic
   that a missing root gives ("VFS: Unable to mount root fs"), about ten seconds after the 68k's release. The screen
   showed the penguin.
2. **User space** from an initramfs (`mac.linuxinitrd=`, a static m68k program of ours as `/init`): `/proc/cpuinfo`
   says `CPU: 68030`, `MMU: 68030`, `FPU: 68882`; the timer interrupt at 100 Hz (934 in 9 s); the ext4 disk mounted
   read-only. One SCSI message on the way: `PDMA fixup: !REQ timeout`, then `switching to slow handshake`.
3. **Root on the virtual SCSI disk** (`root=/dev/sda rw`): mounted, `/sbin/init` started, and then nothing for the
   rest of the run: every sector came through the polled path, a register access per byte, each an abort (3 M
   instructions in 5 s). **The cause** was the 5380 model, not Linux. With `FLAG_DMA_FIXUP` Linux's NCR5380 driver
   moves one byte fewer than asked through the pseudo-DMA window, waits for DRQ, waits for REQ to go *false*, reads
   the last byte from the input data register and leaves DMA mode: on the chip, a REQ in a matching input phase is
   answered with ACK at once in DMA mode, the target drops REQ, and leaving DMA mode drops ACK. The model kept REQ
   up. Now, while Linux is started only (`psmac_scsidev.c`, `dma_latched`), REQ reads 0 in that state and leaving DMA
   mode after the input register was read completes the byte. Only then, because Mac OS and A/UX were never seen to
   look at REQ there, and A/UX's disk on the owner's SE is one of these virtual devices, which cannot be tried
   against here. After the change no `PDMA` line, and the program on the disk ran as init and printed to ttyS0.
4. **A shell and the network card** (`mac.auxnet=on mac.net=...`, `tools/pinet.py ... peer` as the other end, and
   Debian's own `busybox-static` m68k package on the disk). Two faults first:
   - `*** ILLEGAL INSTRUCTION ***` in `setup_frame`, init killed: `4e7b 8802`, `MOVEC A0,CAAR`. The 68030's cache
     address register, which Linux writes with CACR's clear-entry bit around a signal frame (`push_cache`), was not
     in the translator's MOVEC (a 68040 has none). Under PSMAC it is now accepted: the write keeps nothing (the
     clear-entry bit already bumps the code cache's epoch, `check_cacr`), the read gives 0 (`M68k_LINE4.c`).
   - `mac8390 slot.D: Don't know how to access card memory`: the A/UX network card's buffer did not answer. Linux's
     `mac8390` takes the card's base as `slot_addr | slot << 20` ("this is how the Apple driver does it", $FDD00000)
     and the buffer at MinorBaseOS from there, $FDDD0000; the heartbeat's busiest emulated pages showed the probe
     there, while the card answered only at $FD0D0000, where A/UX's `ae6` looks. A real card decodes fewer address
     lines (*inference*: both drivers work on Apple's card); ours now decodes A0-A19 for its buffer and chip, the ROM
     page aside (`psmac_etalk.c`, `etalk_decode`).

   Then: `mac8390 slot.D: Aurora68 Ethernet for A/UX (type apple)`, `32 KB shared memory at 0xfddd0000, 32-bit
   access`; busybox's `udhcpc` got 192.168.68.2 from the peer, IPv6 configured itself from the peer's router
   advertisement, and `ping -c 3 192.168.68.1`: 3 of 3 answered (99, 18, 17 ms). Interrupt 60, "8390 Ethernet", in
   `/proc/interrupts` beside the SCC (4), ADB (10), the VIA timer (14) and the 5380 (19).

## 4. The MAME SE full stack

`tools/aux/runse.sh` with `DECLROM=mac/build/psmac-se.declrom`, `mac.mmu=full mac.ram=64`, the busybox disk as
`scsi.disk=...:0` on the SE's 5380 (a virtual device beside MAME's SCSI bus), 600 s. Linux started at the same
address, `screen: 512x342 at 1 bit, 64 bytes a row, at $f9980000 (the SE's screen)`; `macfb: mode is 512x342x1`;
MAME's own ADB keyboard and mouse found (`Detected ADB keyboard, type ANSI`, `input: ADB mouse`), `scsi 0:0:0:0:
Direct-Access AURORA68 Virtual Disk`, root mounted, init and busybox's shell about a minute after the release (four
heartbeats to init). MAME's screen of the SE shows the penguin and the kernel's messages in the 6x11 font (snapshot
62 of that run). That run was made before the ADB devices were added at the release (section 2): its log shows them
appearing only when Linux's first user process gave the modes separate views (the A/UX hand-over), after Linux's bus
scan.

**The network card on the MAME SE, with the ADB devices at the release** (the final kernel, `tools/aux/run-aux-net.sh`
with `mac.auxnet=on` and `tools/pinet.py` as the peer, 420 s): `[PSMAC:ADB] a virtual ADB keyboard ... Linux is
started`, and Linux found two keyboards and two mice (`input0`-`input3`: MAME's and the Pi's, each given its own
address by Linux's scan); `mac8390 slot.D: Aurora68 Ethernet for A/UX (type apple)`, `udhcpc: lease of 192.168.68.2
obtained from 192.168.68.1`, and 3 of 3 pings answered (334, 23, 68 ms over the bridge).

**Regression** (the final kernel): `make -C mac check` passes, with `diag_test.sh` (7689 checks), `adb_test.sh` (86),
`aux/etalk_test.sh` (44539, the A/UX network card's chip model after the address decoding change) and the 5380
model's host test (solo, se, empty; the Linux-only REQ change is off there, as for Mac OS); System 7.1 on the solo
board without `mac.linux=` reaches the Finder at 21.9 s.

## 5. The build script (`tools/linux/`)

`build-debian-image.sh [OUTPUT]` runs on the Mac with nothing but a Docker-compatible engine: OrbStack's `docker`
(the owner's) or Docker Desktop, Apple Silicon or Intel. It checks that `docker info` answers ("Start OrbStack (or
Docker Desktop) first" otherwise), asks for root's password twice (or takes `ROOT_PASSWORD`), builds the image from
`tools/linux/Dockerfile` (Debian 13 with `gcc-m68k-linux-gnu`, `debootstrap`, `debian-ports-archive-keyring`,
`qemu-user-static`, `e2fsprogs`; the package names were checked in Debian 13's arm64 and amd64 indexes) and runs
`build-in-container.sh` in a privileged container, the password on its standard input:

1. the kernel: the pinned tarball from cdn.kernel.org, checked against its SHA-256, the patches, the configuration as
   in section 3, a check that the options the machine needs are `=y`, `make vmlinux`;
2. qemu-m68k through binfmt_misc with the fix-binary flag (an existing registration without it is replaced);
3. `debootstrap --foreign --arch=m68k --variant=minbase` from `https://deb.debian.org/debian-ports`, `unstable`, with
   sysvinit-core, openssh-server, ifupdown, dhcpcd-base, ca-certificates, nano, iproute2, netbase, procps, less; then
   the second stage in the chroot;
4. `configure-rootfs.sh`: host name, fstab (the root by UUID), getty on tty1 and ttyS0 (the other consoles off),
   eth0 by DHCP, apt's source, root's password as a SHA-512 hash (`openssl passwd -6`, never in clear on disk or in a
   log), `PermitRootLogin yes` unless `AUTHORIZED_KEYS` gave keys;
5. `debian.img`: a sparse file of `IMAGE_SIZE` (2G by default, at most 4095M, the FAT32 limit of the card), ext4 on the
   whole disk made with `mkfs.ext4 -d` from the tree, no loop device.

Its `README.txt` names the files and the `cmdline.txt` line. A Debian machine of OrbStack's (`orb create debian`)
can run the same `build-in-container.sh` as root (the script's header says how); the Docker way is the one meant.

**Two dependency findings while writing it, from Debian Ports' index of the day:** isc-dhcp-client is gone from
unstable (dhcpcd-base is there, and ifupdown uses it); ifupdown, dhcpcd-base and openssh-server depend on
`systemd | systemd-standalone-sysusers | systemd-sysusers`, and debootstrap takes the first alternative: a first stage
run here put systemd in its list, beside systemd-standalone-sysusers, which conflicts with it. The script takes
systemd and its private library out of debootstrap's lists between the two stages (`debootstrap --print-debs` without
those three packages lists no systemd at all).

**What was run here and what was not.** This machine has no Docker engine and no binfmt_misc, so the script was not
run end to end. Run here: `build-in-container.sh` itself, with its folders pointed into the scratch area and Ubuntu's
GCC 13 instead of the container's 14, through step 1 (the tarball's SHA-256 "OK", the configuration checked, a
7,143,084-byte vmlinux with the same configuration as the hand-made one) until step 2 stopped it as it should ("qemu-m68k
is not statically linked": this machine's is dynamic); the host script's checks (no engine: its message; the size rules,
for bash 3.2's sake written without Python); and debootstrap's first stage with the step's options (Debian's debootstrap
1.0.145 and the Ports keyring, unpacked in the scratch area), whose list showed the systemd problem below. The second stage could
not run in a chroot here; section 6 is how it was tried instead.

## 6. Debian's second stage, on the emulated SE itself

Without binfmt_misc on the host, the m68k programs of the second stage can run on the machine they are for: the first
stage's tree (with debian-ports-archive-keyring added and systemd taken out of its lists as the script does) as a 2 GB
ext4 image, booted on the solo board (128 MiB) with `init=` a script that runs `debootstrap --second-stage`, then
`configure-rootfs.sh` with `ROOT=/`. Two tries, both stopped before the end:

- The first (the kernel with a log line at every context switch, below) stopped after about five minutes: no level-1
  interrupt taken any more (`interrupts taken: level 1 0` from one heartbeat on, the VIA's timer flag and enable both
  set), no disk command after 33,758 sectors read, the sampled PC nearly always in `mac_read_clk` (the VIA clocksource)
  called from `ktime_get`, with the interrupt mask at 7. The seqcount Linux's `ktime_get` waits on was even (the saved
  D2 on the stack), so `ktime_get` itself returned; something called it over and over with interrupts masked, and with
  the mask up the VIA clocksource cannot advance past its tick. Who, was not found: the second try, with the same Linux
  and the context-switch line limited, ran 22 minutes without it (to debootstrap's "Installing core packages", 50,670
  sectors read and 44,264 written) and was stopped there on the owner's word: the image is to be built on the Mac,
  where this stage runs under qemu-user far faster. *Not settled:* whether the first stop was the log flood's timing
  or a fault that the second run did not meet.
- **The line at every context switch.** Linux's 68030-only context switch is `PMOVEFD` to CRP followed by
  `PFLUSH #0,#4` (`switch_mm_0230`, `CPU_M68030_ONLY`), and the MMU said "PFLUSH applies deferred CRP change" every
  time: thousands of lines a minute, each through the UART. It is said the first eight times and every 4096th now.
- **Where the time goes** (the second try's counters after 21 minutes): 25.9 million emulated accesses, 11.4 million
  of them writes to the 5380's pseudo-DMA window (one abort each, every long of every sector); 69,091 address-space
  switches at about a millisecond each, nearly all of it revalidating what the process had mapped (770 million pages
  kept by the PFLUSH revalidation, 71.6 s); 10.5 million MOVES; and the code cache's checksums after each switch,
  because Linux's switch also clears the 68030's caches through CACR (`FLUSH_I_AND_D`), which bumps the code cache's
  epoch. The 68k averaged 3 to 5 MIPS. Each is a place to win speed back; none was changed.

## 7. Open

- The second stage's speed, and the disk's: every pseudo-DMA access of the 5380 is an abort (section 6); not measured
  on a Pi. The stop of section 6's first try.
- `[PSMAC:MMU] fault at ... not delivered as a bus error: not translated code` lines while user programs run: the code
  cache's checksum (`CalcCRC32`, after Linux's cache clears bump the epoch) reading a block's page that Linux has
  since unmapped. The block then fails its check and is dropped, which is what should happen; the lines are noise,
  but a check that reads through the walk instead of faulting would be cheaper.
- `mac8390 slot.9: MAC address resource not found`, twice: slot 9's card has two network sResources for Mac OS's
  `.ENET` drivers (the wired port's, drHw 'PS', and the WiFi's, 'PT'), both `catNetwork, typeEtherNet, drSwApple`,
  which `mac8390_ident` takes for Apple 8390 cards. For those the driver reads the MAC address resource ($80) first,
  finds none, and gives the function up before touching the card (`mac8390_rsrc_init` returns false), so slot 9 gets no
  interface and eth0 is slot $D's card (section 3, run 4: `ip link` lists lo and eth0 only). Harmless; changing slot
  9's sResource types would touch what Mac OS's drivers and programs look for, so it was left.
- Linux's palette writes go nowhere ("Generic NuBus" has no palette routine): the logo is grey on an eight-bit HDMI
  screen. The console's colours are right.
- Restart: Linux's `reboot` jumps into the IIci ROM, which starts Mac OS; `mac.linux=` takes effect at the next start
  of the Pi.
- Not tried on the real SE, nor with a USB or Bluetooth keyboard as an ADB device under Linux, nor the WiFi path for
  the network card (the testbed used the wired port's semihosted network).
- The root device's name depends on the SCSI IDs: Linux names disks in ID order, so with the SE's own disk at ID 0 the
  Debian disk is `sdb`. A partition table and `root=PARTUUID=` would not care; the owner asked for ext4 on the whole
  disk.
