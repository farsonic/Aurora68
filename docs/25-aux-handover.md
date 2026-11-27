<!-- SPDX-License-Identifier: MPL-2.0 -->
# 25. A/UX on the SE: where the work stands and how to take it over

*27 November 2026.* This note is for whoever picks the A/UX work up next, in particular the networking, starting from
a fresh clone with none of the scratch files of the sessions that did the work so far. It says what runs, which
branch holds what, what is with the owner for testing, what to do next and in which order, and how to get the test
set-up back. The long record is `docs/23-mmu.md` (the emulated 68030 MMU and A/UX's road to its Finder on the real
SE, sections 12 to 23) and, on branch `aux-net`, `docs/24-aux-network.md` (the network card). Read `CLAUDE.md` first.

## 1. What runs today

| What | State | Where |
|---|---|---|
| A/UX 3.1.1 (the owner's own disk image, the Apple Workgroup Server 95 edition) to its Finder and CommandShell on the owner's real SE | works (test kernel 20) | branch `mmu-experimental`, commit b863149 |
| Bluetooth and USB keyboards and mice under A/UX, as two virtual ADB devices beside the SE's own (`mac.adb=auto`, the default) | Bluetooth keyboard and mouse work on the real SE (test kernel 21); USB and the SE's own devices beside them not yet reported; the pointer was "a bit fast", so the default is now half the counts (`mac.adbspeed=50`) | `mmu-experimental`, commits c7b5307, be999f5, 77a047c, 85e575a (regression: System 7.1 on the SE and solo boards and the MMU test ROM unchanged); docs/23 section 23, docs/08 item 151 |
| Networking under A/UX over the Pi's WiFi: a virtual card of the kind A/UX's own `ae6` driver drives (`mac.auxnet=on`) | **works on the owner's real SE** (test kernel 22, 27 November 2026): `/etc/etheraddr` shows the Pi's WiFi address, `newconfig` linked `ae6`, `ae0` came up and another machine on the WiFi network pings A/UX (about 4 ms once ARP is done); with `route add default ROUTER 1` A/UX pings a public address on the internet, five of five answered. Not yet seen in the testbed; default route, DNS and the merge with the input work still to do | branch `aux-net` (from b863149), `docs/24-aux-network.md` there |
| The HDMI screen under A/UX | not started; switching the HDMI screen on locked the SE up under A/UX, so the owner runs with `mac.hdmi=log` | |

`aux-net` now also holds the input work (merged from `mmu-experimental` at 85e575a, without conflicts): its head is test kernel 23, input and network together, sent to the owner on 27 November 2026; the HDMI work continues on `aux-hdmi`, branched from it. None of this is merged into `main`. `main` has the tools (`tools/aux/`, `mame/act.lua`) and this note.

**The owner's order of work (27 November 2026):** Bluetooth, WiFi and USB under A/UX first; then the HDMI screen
under A/UX. On hold by the owner's word: restyling the kernel to `emu68/CODING_STYLE.md` (CLAUDE.md, Conventions),
Card P, the Octavo tree, any new port.

## 2. Networking: what there is, and what to do next

**The idea.** A/UX does not use Mac OS slot drivers, so the Pi's own network card (its `.ENET` driver, docs/07) is
of no use to it, and the kernel drops that card when A/UX's page tables come on (`psmac_card_handoff()`). The owner's
image carries Apple's `ae6` driver module (`/etc/boot.d/ae6`; `/etc/master.d/ae6` says `id 8`, the board ID of the
Apple EtherTalk NB card, a DP8390 with buffer RAM and its Ethernet address in ROM). A/UX links a driver into its
kernel when `autoconfig` (run by `newconfig`) finds a board with a matching ID in a slot. So the kernel presents
such a card: a declaration ROM of our own with board ID 8, a register-level DP8390, the buffer, a slot interrupt,
and the frames passed to and from the Pi's network board (WiFi or USB Ethernet). The details the driver depends on
are in `docs/24-aux-network.md` on `aux-net`.

**Tested by the owner: test kernel 22** (`aux-net` at the time, no ADB work in it, built without the PC and Apple II
cards). The steps the owner was given:
1. cmdline: the A/UX line plus `mac.auxnet=on mac.auxnettrace=300`.
2. In A/UX as root: `/etc/etheraddr` must print the Pi's Ethernet address (if not, A/UX does not see the card).
3. `cp /unix /unix.old`, then `newconfig -v`: it builds a new kernel (look for "Found board ID 8 in slot 13") and
   asks for a host name, an address and a netmask. The same disk image must be kept across the restart.
4. After the restart: `ifconfig ae0`, `netstat -in`, `/usr/etc/ping ROUTER 56 3`, `/usr/etc/route add default ROUTER 1`.

**How to read what comes back.**
- `/etc/etheraddr` prints nothing: the Slot Manager or A/UX's slot call does not show the card. Look at the
  declaration ROM (sResource list, board ID entry, the Ethernet address entry) and at which slots are asked.
- `newconfig` does not find board ID 8: the same, on autoconfig's side (it asks about slots 9 to D only).
- The new kernel hangs, panics or prints `ae0:` errors: the chip model is wrong somewhere. The log's
  `[AUXNET] W/R <register> <value>` trace lines (the first 300 accesses) say where the driver was; compare with the
  driver's code in docs/24. Any `irqguard` line means the card's interrupt was not taken (docs/23 section 16).
- `ifconfig ae0` is fine but ping fails: follow one frame: "the first frame from A/UX's driver went to the network
  board", then "the first frame for A/UX's driver is in the card's ring", then the 30-second counter line. On WiFi
  the card must use the WiFi's own Ethernet address (an access point drops other source addresses), and A/UX's IP
  address must differ from the kernel's own (`remote=on` shares that Ethernet address).

**Next steps, in order.**
1. Done on 27 November 2026: test kernel 22 on the real SE gave `ae0` and answered pings. Still wanted from the owner:
   the `aurora68.log` of that session (the card's counters) and DNS. The default route by hand works (CommandShell, 27 November 2026); A/UX's ping prints 0 ms for answers faster than its clock's tick, which is not a measurement. The real SE is the
   faster test: A/UX boots there in minutes, in the testbed in 20 to 40.
2. In the testbed: a run where the relinked kernel attaches `ae0` and exchanges ARP or a ping with the host peer
   (`mac.net=` and `tools/pinet.py`, CLAUDE.md). The kernel has to be relinked without typing: do what `newconfig`
   does from an rc script on a private copy of the image (`tools/aux/mknetimage.py` on `aux-net` writes that script
   into the image; docs/24 says how far it got). The chip model also has a host test, `tools/aux/etalk_test.sh`.
3. Done (27 November 2026, test kernel 23): `aux-net` merged with `mmu-experimental`; the slot rule for `pc=on` is in docs/24 section 4. What follows is the note written before it, kept for the reasoning. Merge `aux-net` and `mmu-experimental` into one kernel with input and network. Both touch
   `psmac_board_se.c`, `psmac_bus.c`, `psmac_diag.c`, `alice_psmac.c`, `emu68/CMakeLists.txt` and
   `tools/diag_test.c`, each with small hooks; the bulk is in files of their own (`psmac_adb.c`, `psmac_etalk.c`).
   Build it the way the owner's kernels are built (the PC and Apple II cards compiled in, docs/19 and docs/20):
   with `pc=on` the PC card takes slot $D and the network card would move to $E, which autoconfig does not ask
   about; decide what gives way.
4. What the owner will want once ping works: a default route that survives a restart (a line at the end of
   `/etc/sysinitrc`, or `/etc/gateways` with `net 0.0.0.0 gateway ROUTER metric 1 passive` for `in.routed`, which
   A/UX 3.1.1 starts when `newconfig bnet` is used and which otherwise waits for RIP that no home router sends),
   `/etc/resolv.conf`, and `newconfig bnet` or `nfs` for the daemons. There is no DHCP client in A/UX 3.1.1.
   AppleTalk over the same card (EtherTalk) and MacTCP in A/UX's Macintosh environment have not been looked at.
5. Then regression (section 4), the docs, and the merge of `mmu-experimental` into `main` when the owner asks.

## 3. Other things known and open

- **Two network cards, by the owner's choice for now (27 November 2026).** Mac OS uses the Pi's own card in slot $9
  (`.ENET`, MacTCP, Riptide, the WiFi menu); A/UX uses the board ID 8 card in slot $D (`ae6`). Under Mac OS the slot
  $D card shows as an unused Apple Ethernet card. Offered and left for later: hiding it from Mac OS, or one card for
  both (our `.ENET` in the slot $D card's ROM, the slot $9 network service retired).

- **A bus error in A/UX's Macintosh environment at start, in the testbed only so far.** Two runs stopped about 13
  emulated minutes in with `[CRASH] System error 1 (bus error) called at PC 002f99fa in "A/UX Startup"`; the read
  that fails is a data read in user mode at PC $2FCE8E through a handle holding $08F90007. Intermittent, cause not
  known, not seen on the real SE. In those runs the crash recorder then killed a core with a read past the end of
  memory (`psmac_mac_rd32`); that second fault is fixed on `aux-net`.
- **`/bin/sh: NNN Memory fault - core dumped`** when CommandShell opens root's login shell on the real SE, after
  `/.profile`'s last group (`date; /bin/news; pwd; ls -aFC`) has printed. The shell goes on working and the owner
  takes it for a red herring. Not reproduced in the testbed. A lead, not checked: A/UX's `sh` grows its memory by
  taking a SIGSEGV and running the instruction again (`fault`, `setbrk`, `brkincr` in its symbols).
- **A/UX counts 192 MB on a Mac given 128**: use `mac.ram=64`.
- **The TLB self-check line** (`[PSMAC:MMU] TLB self-check`, docs/23 section 19) has never been read from a real Pi's
  log: the log file keeps only the last 64 KiB.
- **"Macintosh IIci" in About This Macintosh under A/UX**: Aurora68 Machine is not in A/UX's System Folder. Untried.
- **The slot route on a real SE** (a PiStorm on the processor direct slot, the 68000 left in): docs/22 section 11.

## 4. The test set-up, from nothing

1. The prerequisites and the MAME testbed as in `CLAUDE.md` (the patch builds `macse_ps`, the bridged SE). The
   Macintosh IIci ROM is not in the repository and must be supplied.
2. **The A/UX disk image is the owner's and is not in the repository.** Ask the owner for it again
   (`AUX_3_1_1GB.dsk`, 1,048,576,000 bytes, a Shoebill-style image with the root file system at sector 96). Keep
   it only in a scratch directory, never commit or upload it or anything extracted from it, and delete every copy
   when the A/UX work is done. A/UX writes to its disk: give each run a fresh copy unless the run is meant to
   build on the last.
3. A kernel from the branch under test, built as in `CLAUDE.md` (variant `psmac`). A failed build leaves the old
   image in place without saying so: check the image's time.
4. `tools/aux/runse.sh SECS LOGDIR KERNEL mac.mmu=full mac.ram=64 [options]` with `PSMAC_RAM=64` runs A/UX in the
   MAME SE. One emulator at a time (the script holds a lock). A/UX's boot dialog comes after about four emulated
   minutes, its Finder after 12 to 20; look at the snapshots, not the clock. `mame/act.lua` with `ACT_WAIT=menus`
   clicks once the Finder's menus are up. There is no keyboard in the testbed other than MAME's own ADB devices
   driven from Lua, so make A/UX do things from files on the image where possible.
5. `tools/aux/README.md` lists the helpers for reading the image and the dumps. Regression before anything goes to
   the owner: `tools/diag_test.sh`, `make -C mac check`, the test ROM (`mac/mmutest.s`, docs/23 section 12.6: all
   passed, none failed, with `mac.mmu=full`), System 7.1 to the Finder on the SE board with default options and on
   the solo board with and without `mac.mmu=full`.

## 5. Working with the owner

- The owner tests on the real SE and sends photographs of the screen and `aurora68.log` (the last 64 KiB, saved
  every ten seconds with `mac.logsave=10`). A test kernel goes out as a zip of `kernel8.img` and a `README.txt`
  that says what changed, the exact `cmdline.txt` change, what to try in which order, what to look for, the risks
  and the way back. Number them on from 22.
- The working command line for A/UX on the SE, with placeholders:
  `enable_cache mac.board=se mac.mmu=full mac.ram=64 scsi.disk=AUX.dsk:0 mac.boot=scsi mac.hdmi=log eth=on bt=on
  remote=on remote.wifi=shared remote.token=YOURTOKEN wifi.country=XX wifi.ssid="YOURSSID" wifi.psk=PASSWORD
  mac.logsave=10`
- Before a package leaves: search it for certificate or key material, passwords and e-mail addresses, and refuse
  if any is found. Logs and photographs from the owner can hold the WiFi's name and password, a token, addresses
  and host names: none of that goes into code, documents, commit messages or packages; write placeholders.
- Commits in this repository are made as `Aurora68 <aurora68@localhost>` with no trailers. No pull requests
  unless asked; the owner makes the releases.
- No recursive deletes on the work machine: move things aside.
