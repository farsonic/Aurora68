# CLAUDE.md: the Aurora68 workspace

This file is for whoever opens a Claude Code session at the root of the workspace on the user's
MacBook, person or Claude. It is the map: what the five repositories are, how each thing is built
and tested, how the testbed is driven, how a test set for the user's SD card is made, what the
hardware on the bench is, and what is open. Each repository has its own CLAUDE.md with its own
rules; they are binding, and Claude Code reads them when work moves into that repository. The
long record of what was found and why is Aurora68's `docs/08-emu68-port-status.md`: read its
latest items before touching anything that looks odd, as it is usually odd for a reason written
there.

The work was done until 19 November 2026 in a cloud session (Ubuntu 24.04, x86). This workspace
reproduces that machine in a container (`pistorm-se/dev/`), so every command below runs as it did
there. This file lives in `pistorm-se/dev/workspace/CLAUDE.md`; the workspace's `CLAUDE.md` is a
link to it, so a change made here is a change to Aurora68 and is committed there.

## The workspace

| Folder | Repository, branch | What it is |
|---|---|---|
| `pistorm-se/` | farsonic/Aurora68, `main` | The hub. Emu68 forked to run the Macintosh IIci ROM on a Raspberry Pi 3: the "solo" board (the Pi alone, the kernel being the logic board) on the bench now, and later the CPU of a Mac SE through a classic PiStorm. `emu68/` the kernel (`kernel8.img`), `mac/` the Mac-side drivers and programs (68k GNU as, and C with the `mac/c` kit: PiLog, PiDepth), `tools/` the testbed, container and ROM tools, `docs/` the record, `dev/` this kit. MPL-2.0 (the Alice68 modules GPL-2.0-only). |
| `riptide/` | farsonic/Riptide, `mac-port` | A TCP/IP stack for the Amiga, ported to the classic Mac OS: a MacTCP-compatible `.IPP` driver, the Riptide DNR, RiptidePrefs, the Riptide Command Terminal, the Browser test. Its tree is also the Mac application kit Lagoon and Octavo build against (`RIPTIDE=../riptide`). GPL-2.0-only. |
| `lagoon/` | farsonic/Lagoon, `mac-port` | Network shares (SMB 2/3, AFP, NFS v3) for the Amiga, with a Mac port: a bridge that shows a share as an AppleShare volume, and Lagoon EtherTalk (AppleTalk over Ethernet for System 7.1). GPL-2.0-only. |
| `quill/` | farsonic/Quill, `mac-port` | Quill, a word processor for the Amiga (MUI), with a classic Mac port called **Octavo** (a Mac program called Quill exists already). GPL-2.0-only. |
| `cornerstone/` | farsonic/cornerstone, `main` | The user's style guide and kit; its classic Mac chapter (`docs/platforms/classic-mac/`) is what Octavo and the Riptide/Lagoon Mac programs are measured against. **Read-only for us.** |
| `scratch/` | none | Logs, testbed runs, built images and test sets. Not a repository; nothing here is committed. |
| `inputs/` | none | What may never be committed: the IIci ROM, System 7.1 images, firmware. |

GitHub moved two of them: Lagoon is at `https://github.com/farsonic/Lagoon.git` and Riptide at
`https://github.com/farsonic/Riptide.git` (`dev/bootstrap.sh` sets those remotes).

## Where commands run

Recommended set-up (pistorm-se/dev/README.md): Claude Code runs **inside the container**
`aurora68` (image `aurora68-dev`, Ubuntu 24.04), with the workspace at **`/work`**, a bind mount
of the Mac's `~/aurora68-work`. Everything below is written for that: plain commands, paths
under `/work`. If instead this session runs on macOS itself, every build and testbed command goes
through the container: `pistorm-se/dev/shell.sh COMMAND...` (it runs COMMAND in the matching
folder under `/work`), and file paths in commands are the `/work` ones.

- The cross compilers (big-endian aarch64 for Emu68, m68k for the Mac side), hfsutils, QEMU 8.2
  and Python 3 with Pillow are in the container. `pistorm-se/dev/setup-linux.sh --check` says
  what is there.
- The container reaches the user's LAN (the Pi, the SMB share) through Docker's routing; it is
  not on the LAN itself, so the Pi cannot connect *to* it.
- One QEMU at a time (the monitor socket `/tmp/qemu-mon.sock` and pinet's `/tmp/psmac-*` files
  are fixed names). `dev/tools/testbed/boot.sh` refuses to start a second.
- No `rm -rf` or `rm -r`, in scripts or by hand: move things aside (`mv x x.old`).

## Building

All at once, with the native tests: `pistorm-se/dev/tools/buildall.sh --test` (logs in
`scratch/logs/`). Parts: `buildall.sh kernel mac riptide lagoon quill`. By hand:

**The kernel** (Aurora68's CLAUDE.md, "Building each artifact"):
```
cd /work/pistorm-se
cmake -S emu68 -B psmac-build -DCMAKE_TOOLCHAIN_FILE=$PWD/emu68/toolchains/aarch64-linux-gnu-13.cmake \
      -DTARGET=raspi64 -DVARIANT=psmac -DCMAKE_BUILD_TYPE=Release
cmake --build psmac-build -j4 && gunzip -kf psmac-build/Emu68.img.gz
```
`psmac-build/Emu68.img` is what QEMU loads; on the card `kernel8.img` is `Emu68.img.gz` (the Pi
firmware inflates it). Warnings are errors. Emu68's submodules (`emu68/external/capstone`,
`libdeflate`, `tiny-stl`) must be there: `git submodule update --init --depth 1` (bootstrap does it).

**The Mac side:** `make -C mac` (everything into `mac/build/`: drivers, extensions, programs, the
declaration ROMs `psmac*.declrom`, one per screen size), `make -C mac check` (structure offsets
against the kernel's headers, and 53 native checks of the USB network framing). The binaries in
`mac/build/*.bin` and `*.declrom` are committed: rebuild and commit them with the sources. Two
things make a rebuild touch more than you changed: every MacBinary file carries its build date
(bytes 91-98), so `git status` lists every `.bin` after `make -C mac`; and PiPing's target is a
make variable, `PEER` (default 192.168.68.1, the testbed's peer), while the committed PiPing pings
the bench's router, `make -C mac PEER=192.168.0.1`. Commit only the binaries whose sources
changed and `git checkout` the rest.

**Riptide (Mac):** in `riptide/`, `make -f Makefile.mac` (build/mac) and
`make -f Makefile.mac AURORA68=1 BUILD=build/mac-a68` (the logging build: its log also goes to the
Pi's kernel log, which PiLog shows); `test` adds MacTCPTest.bin and BrowserTest.bin. Host tests:
`make check`.

**Lagoon:** host tests `make test` (unit tests always; the Samba, Netatalk and nfs-ganesha
integration tests skip with status 77 when the servers are missing: `setup-linux.sh --servers`
adds Samba). Mac: `make -f Makefile.mac AURORA68=1 B=build/mac-a68` (Lagoon.bin,
LagoonEtherTalk.bin).

**Quill and Octavo:** native tests from `quill/` (expect "N tests, 0 failed"):
```
cc -std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
   core/*.c tests/coretest.c tests/test_wp*.c -o /tmp/quill_coretest && /tmp/quill_coretest | tail -3
```
Octavo: `make -f Makefile.mac AURORA68=1 B=build/mac-a68` gives `build/mac-a68/Octavo.bin`
(needs Riptide's tree beside it). `quill/docs/mac.md` and `quill/NEXT.md` are its record.

**Amiga builds** (Lagoon, Quill, Riptide's Amiga side, Cornerstone's Jotter) need vbcc and the
NDK: `setup-linux.sh --amiga` (amd64 containers only) installs vbcc's multi-target binaries and
NDK 3.2 in `/opt/amiga`; the MUI 3.8 SDK is not redistributable and the user supplies it
(`/opt/amiga/mui/include`, from Aminet `dev/mui/mui38dev.lha`). The Mac work does not need them.

**MAME** (the full IIci/SE testbed, Aurora68's CLAUDE.md "The MAME testbed"):
`setup-linux.sh --mame /work/pistorm-se` clones MAME 0.264, applies `reference/mame-psmac.patch`
and builds (about 25 minutes). The solo board needs no MAME; most work since 17 November used
the solo board only.

## The testbed: the solo board in QEMU

QEMU's `raspi3b` runs the kernel with `mac.board=solo`: the Pi alone is the Mac, its HDMI screen
the Mac's screen. The disk image and the declaration ROM come in through semihosting; the IIci
ROM is `pistorm-se/rom/original/MacIIci_368CADFE.rom` (the user's; not in git). Helpers in
`pistorm-se/dev/tools/testbed/` (each prints its usage with `--help` or no arguments):

```
T=/work/pistorm-se/dev/tools/testbed
$T/boot.sh --bg --secs 900 --name try1 --net /work/scratch/full.image      # back once QEMU is up
$T/waitlog.sh try1 '\[BOOT\].*Finder' 300        # the desktop is up (about 25-40 s of QEMU time)
$T/shot.sh /work/scratch/runs/try1/desk.png       # look at it with the Read tool
$T/mouse.py click 200 4                           # the pointer to (200, 4), click (a menu title)
$T/keys.py @cmd-o "word" @ret                     # Command-O, type, Return
$T/stop.sh try1                                   # or let --secs run out
```
- `boot.sh` options: `--screen WxH` (default 1024x768, with `mac/build/psmac-WxH.declrom`),
  `--ram MB` (32), `--net` (the wired card on `tools/pinet.py`, a host peer at 192.168.68.1 with
  DHCP, ARP, ping, UDP echo; `PINET_INTERNET=1` answers pings to anywhere, `PINET_MDNS=NAME` asks
  mDNS, `PINET_ATALK=NAME` is an AppleTalk node and AFP server stub), `--wifi` (the simulated WiFi
  radio on a second peer, 192.168.69.1, network "Aurora68"), `--card FILE` (an SD card image from
  `mkcard.sh`, to run the card code: images on the card, `pram.bin`, `aurora68.log`),
  `--container FILE` (boot from a cpio as the Pi does), `--shot N` (a screenshot every N s), and
  kernel options after the image (`mac.inputtest=`, `mac.peek=ADDR,LEN`, `mac.cardstall=S`,
  `mac.cardslow=N`, `mac.netdown=A,B`; docs/09 and docs/08 have them all).
- The run's folder is `scratch/runs/NAME`: `emu68.log` (the kernel log: `[MAC]` lines are the Mac
  programs, `[PSMAC]` the heartbeat every 5 s, `[BOOT]` the timeline, `[CRASH]` a system error's
  record), `final.png`, `pinet-*.log`.
- Input reaches the Mac only through QEMU's USB keyboard and mouse once the Finder is up. The
  mouse is relative: `mouse.py at X Y` pushes the pointer to the corner first, then moves in
  8-pixel steps, so X and Y are the screenshot's pixels.
- Under QEMU every trapped hardware access costs 10 to 20 times what it does on a Pi 3 (docs/08
  item 50), and the emulated 68k is slow: wait generously (an application's first window can
  take a minute). Timings from the testbed are QEMU's, not the Pi's.

**Disk images.** `dev/tools/mkfullimage.sh --base inputs/aurora-test3.image --out
scratch/full.image` puts every current build on a copy of a System 7.1 image (layout in its
header; riptide.conf for both cards and the "Test Disk Read Me" from `dev/tools/disk/`).
`dev/tools/mkpartimg.py BASE OUT.img --label "Riptide Update" --put riptide/build/mac-a68/Riptide.bin`
makes a small partitioned disk that reuses the base's Iomega driver (how test set 4's update disk
was made). `mkcard.sh card.img cmdline.txt full.image update.img` makes an SD card image for
`--card`; keep a pristine copy, as runs write to it. hfsutils (`hmount`, `hls`, `hcopy -m` for
MacBinary, `hcopy -t` for text, `humount`) reads and changes images between runs; never while
QEMU has the image open.

## A test set for the user's card

`dev/tools/mktestset.sh --out scratch/Aurora68-0.12-test5 --notes notes.txt` builds the kernel
from a clean worktree at a pushed commit (`--commit REF`, default HEAD), takes that commit's
declaration ROMs, writes config.txt (the release's, `hdmi_mode=69` for the bench's 1920x1200) and
cmdline.txt (one line, placeholders for the network), optionally an update disk
(`--update-disk riptide-update.img --update-base inputs/aurora-test3.image --update-put FILE.bin`),
a TEST-BUILD.txt (the commits, what is in the set, how to set up the card, then your notes:
what is new, what to try, what to photograph), refuses key material, ROMs, firmware and
passwords, and zips it. `--kernel-only` for a kernel update that leaves the card as it is.
Commit and push first: a set must name a commit anyone can find.

**The card**, as the bench has it (FAT32, the root):
- `kernel8.img` (gzip is fine), `config.txt` (`kernel=kernel8.img`, `arm_64bit=1`,
  `initramfs aurora68.cpio 0x10000000`, `boot_delay=0`, `enable_uart=1`, `hdmi_group=2`,
  `hdmi_mode=69` for 1920x1200, `[pi3] gpu_mem=64`), the Pi's boot files (`bootcode.bin`,
  `start.elf`, `fixup.dat`, the device trees; `get-firmware.sh` fetches them, and `start4.elf`,
  `fixup4.dat` for a Pi 4 on the same card).
- `cmdline.txt`, one line, each option once, for example
  `enable_cache mac.board=solo mac.screen=1920x1200 mac.image=aurora-test3.image,riptide-update.img wifi.ssid=IOT wifi.psk=... wifi.country=AU`.
  The WiFi password is in the user's own copy only: never in a file of ours, a commit, a log
  we keep, or a message.
- `aurora68.cpio`, the container, about 1 MB: the IIci ROM, the declaration ROM of the screen's
  size, the WiFi and Bluetooth firmware. `python3 make-container.py -o aurora68.cpio
  MacIIci_368CADFE.rom aurora68-1920x1200.declrom cyfmac43430-sdio.bin brcmfmac43430-sdio.txt
  cyfmac43430-sdio.clm_blob BCM43430A1.hcd` for the Pi 3B, or `sh get-firmware.sh --container
  MacIIci_368CADFE.rom --declrom aurora68-1920x1200.declrom` (every board's firmware, checked).
  The kernel patches a stock ROM itself.
- Disk images as files in the root (`*.image`, `*.img`, `*.hda`, `*.hfv`, `*.dsk` over 1 MB),
  named by `mac.image=A.image,B.img`: the first is the startup disk (SCSI ID 0), the others
  PiDisk volumes. The card code writes `pram.bin` (parameter RAM), `pairings.txt` (Bluetooth)
  and `aurora68.log` (PiLog's Save to Card, and after a crash record) beside them.

**Gotchas met so far:**
- A second `mac.image=` in cmdline.txt is ignored: the kernel takes the first. One line, each
  option once (mktestset.sh refuses duplicates).
- The declaration ROM's size must match `mac.screen=`. The kernel now falls back to the plain
  or the sized ROM it finds and gives the screen that ROM's size, saying so in the log; before
  that, a mismatch left slot 9 empty (no screen, no PiDisk, no input).
- Firmware fetched from RPi-Distro/firmware-nonfree's `brcm/` names is a 31-byte text file
  holding a link target, not firmware: use `get-firmware.sh`, which fetches the real paths at
  pinned commits and checks SHA-256.
- An image inside the container wins over the card's, is read whole by the firmware before the
  kernel starts (5-10 s per 100 MB) and lives in RAM (writes lost at power-off). On the card it
  is read in the background as the Mac asks, and writes go back to the file. Keep images out of
  the container.
- A Pi 4 on the same card: the kernel does not drive the Pi 4's card controller yet, so nothing
  is read from or written to the card there (no card images, no `pram.bin`, no pairings, no
  `aurora68.log`); images must go in the container, and Bluetooth pairings do not move between
  boards.
- Replace the declaration ROMs together with `kernel8.img` when the notes say so (the frame
  buffer and the interrupt rules moved more than once).

## The user's hardware

- A Raspberry Pi 3B (v1.2) on the bench, running the solo board, with its HDMI to a Dell monitor
  at 1920x1200 (`hdmi_mode=69`, `mac.screen=1920x1200`, millions of colours).
- WiFi on the network called **IOT** (BCM43438, 2.4 GHz; the password stays in the user's
  cmdline.txt only). The wired port works too (the LAN9514, since test set 3). Riptide uses both:
  en0 wired, en1 WiFi, DHCP on each.
- A Pi 4 is there for testing by moving the card (see the Pi 4 gotcha).
- USB keyboard and mouse on the Pi's ports; Bluetooth keyboards and mice pair from the menu bar.
- The eventual target: a Macintosh SE with a classic PiStorm (docs/12), the Pi as its CPU.
- On the same LAN: the user's MacBook (this machine) and an SMB share (Lagoon's test target).
  The Mac's host name is **MacSE** (`MacSE.local` by mDNS, from riptide.conf).
- No serial cable is assumed: what the Pi says comes back as PiLog screenshots, PiLog's Save to
  Card (`aurora68.log`) or, soon, the remote-control service below.

## Remote control: "the Mac is at IP x.x.x.x"

**Pending: see `pistorm-se/docs/13-remote-control.md` when it lands.** Another worker is
building a remote-control service into the kernel (its own small IP stack and an HTTP server on
the Pi, an MCP server `pistorm-se/tools/aurora68-mcp.py`). The intended workflow, when the user
says "the Mac is at IP x.x.x.x":
1. The card's cmdline.txt has `remote=on remote.token=TOKEN` (the token is a secret like the
   WiFi password: the user's card and an environment variable, never a file of ours).
2. The Pi's address: the one the user gives, else the router's DHCP list, else PiLog (the
   service logs where it listens).
3. Add the MCP server to Claude Code, in the container:
   `claude mcp add aurora68 -- python3 /work/pistorm-se/tools/aurora68-mcp.py --host x.x.x.x`
   with `AURORA68_TOKEN` set in the environment (check docs/13 for the real arguments).
4. Then: screenshot the Mac's screen, click and type, read the kernel log, read the state
   (heartbeat, network, disks), restart; build, put the new kernel or image on the card when the
   service can, test, fix, repeat.
Until it lands the fallbacks are PiLog's Save Log and Save to Card (being added by the PiLog
worker: `aurora68.log` on the card, passwords replaced by `***`), and the card itself: the user
moves it to the MacBook, and `aurora68.log`, `pram.bin` and the images can be read there
(hfsutils for the images).

## Open items and known issues (19 November 2026)

To pick up, roughly in order. Check `git log` and docs/08's newest items first: others were
working on several of these when this was written.
1. **Two images on the card deadlock the boot.** With kernel 447febf (test set 4) and
   `mac.image=A,B` both on the card, the boot stops after the MMU's set-up, before the first
   heartbeat (the testbed reproduces it: two images on a card image, `--card`). A worker was
   changing `psmac_card.c` and `start.c` for it on 19-20 November. Until fixed: one image on the
   card, or test set 3's kernel (1df6f2a), which mounted two.
2. **15 s of early kernel boot** on the real Pi before the 68k starts: moving the kernel and
   waking the other cores (`start.c`, the `[BOOT] t=` lines). Same worker. docs/08 item 50 has
   the timeline and the target (under three seconds from power-on to the 68k).
3. **The WiFi password is echoed in the log**: `[PSMAC] options: ... (bootargs: ...)`
   (`psmac_bus.c`) prints the whole command line, `wifi.psk=` included, to the serial log,
   PiLog and anything saved from it. Redact as `psmac_card.c`'s `log_redact` does for
   `aurora68.log` (psk, pass, key, secret, the password in `mac.wifisim`, the key in `bt.dev`).
4. **Ping latency**: about 20 ms wired and 80 ms over the WiFi on the Pi 3B. Two fixes are in
   test set 4 and not yet confirmed on the hardware: the WiFi's power saving switched off after
   every join (`[WIFI] power save: PM ...`) and Riptide waking a waiting program at once
   (WakeUpProcess). The `[NET] echo` lines and the service-loop line will say where the time
   goes (docs/08 item 49).
5. **Bluetooth reconnect**: paired keyboards and mice do not always come back after a power
   cycle (pairings in `pairings.txt` on the card). Ask the user for the log lines around it.
6. **The USB keyboard's '.' key** misbehaves (reported; which key, main or keypad, and in which
   program is to be asked). `.PiInput` (`mac/piinput.s`) maps HID usages to Mac key codes; docs/08
   item 42 notes the keypad '.' is Word's own menu key.
7. **Open Transport**: designed (`riptide/docs/MAC-OT.md`, route (a): Riptide's own OT libraries,
   in four stages) and on hold; Internet Explorer 4.01 works over the MacTCP interface without it.
8. **IPv6 stand-ins for MacTCP programs** (Riptide 64c09a0: an IPv6-only host gets a stand-in
   IPv4 address from 198.18.0.0/15): passes the Browser test in the testbed, untested on hardware.
9. **The remote-control service** (above): being built; review and test it when it lands.
10. Not proved on hardware: the LAN7800 (Pi 3B+), the Pi 4's GENET with the WiFi, the boot
    timeline's firmware settings (`gpu_mem=16`, start_cd.elf), the SE itself (the PiStorm GPIO
    transport, docs/12). Lagoon's bridge serves one share at a time, guest only, volumes at most
    2 GB. Octavo's list of what is not done is `quill/docs/mac.md`, "Not done yet".

## Rules of every repository

- **Commit identities.** Aurora68: `git -c user.name=Aurora68 -c user.email=aurora68@localhost
  commit`, no trailers, no AI names or session identifiers anywhere (code, documents, commit
  messages). Riptide, Lagoon and Quill: the author `Claude <noreply@anthropic.com>` and the trailer
  `Co-Authored-By: Claude <model> <noreply@anthropic.com>`, as earlier commits do (look at
  `git log -3` there); no session links. Cornerstone: read-only.
- Never put a personal email address in commits, code or documents.
- `git pull --rebase` before every push; add only your own paths (`git add PATH`, never `-A`
  in a tree others may have touched).
- No `rm -rf` or `rm -r`: move things aside.
- Interface strings in five languages (English, German, French, Spanish, Italian) where the
  repository requires it: Quill (`tools/lang/`, Mac words in `tools/lang/mac.py`, then
  `python3 tools/langcheck.py`), Lagoon (catalogs, `tools/mkcatalog.py`), Cornerstone's kit.
- Packages and test sets are searched for key material (`BEGIN CERTIFICATE`, `PRIVATE KEY`,
  `*.crt`, `*.pem`, `*.key`) and refused if any is found; `mktestset.sh` does it, and refuses
  ROMs, firmware and passwords too.
- No ROMs, Apple software (System images, MacTCP, Open Transport), disk images, firmware or other
  downloaded binaries in git, in any repository. Aurora68's `.gitignore` enforces part of it;
  look before `git add`.
- Secrets: the WiFi password and the remote-control token live on the user's card and in the
  user's environment, never in files, commits, logs kept here, or messages. Ask for redacted logs.
- Each repository's own CLAUDE.md adds its own rules (C99 that vbcc compiles, no floating point
  in Riptide's core, speed measured before and after on the 68k, the window fitting 640x480,
  British spelling and plain words, a historian's record with dates).

## Reporting back

So that the next session, here or in the cloud, knows what happened:
- **Aurora68:** a numbered item at the end of `docs/08-emu68-port-status.md` for each finding
  (what was reported, what the testbed or the Pi showed, what was changed, what is not yet
  proved), and a line in `RELEASE-NOTES.md` under "Unreleased" in the user's words. Screenshots
  that prove something go in `docs/img/`.
- **Riptide:** `docs/MAC.md` (and `MAC-OT.md` for Open Transport). **Lagoon:** `docs/mac.md`.
  **Quill:** `docs/mac.md` and `NEXT.md` (where work stopped).
- **This file:** when an open item above is closed or a new one appears, change the list and
  commit it with Aurora68's identity.
- Results from the user's hardware are the ones that count: say which were proved on the Pi
  and which only in the testbed.
