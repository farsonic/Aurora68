# dev/: the Aurora68 workspace on a MacBook

Everything needed to carry the work on in a local Claude Code session instead of the cloud one it
was done in until 19 November 2026: a build container that reproduces that machine, a script that
clones the five repositories into a workspace, the tools that made the test images and test sets,
the testbed helpers, and `workspace/CLAUDE.md`, the instructions the local session reads first.

| File | What |
|---|---|
| `bootstrap.sh` | Clones or updates the five repositories into `~/aurora68-work`, links the workspace's CLAUDE.md, builds the container image |
| `Dockerfile`, `setup-linux.sh` | The build environment: Ubuntu 24.04 with the cross compilers, QEMU 8.2, hfsutils... (`setup-linux.sh` alone does the same on a plain Ubuntu machine or VM) |
| `shell.sh` | A shell, or one command, in the container (started once and kept) |
| `workspace/CLAUDE.md` | The workspace map for Claude Code: layout, builds, tests, the testbed, test sets, the hardware, open items, the rules |
| `tools/buildall.sh` | Builds every artefact; `--test` runs the native test suites |
| `tools/mkfullimage.sh` | The full test disk: a System 7.1 image with every current build on it |
| `tools/mkpartimg.py` | A small partitioned HFS disk with a base image's driver (an "update disk") |
| `tools/mktestset.sh` | A test set for the user's SD card: kernel at a pushed commit, declaration ROMs, config.txt, cmdline.txt, checks, zip |
| `tools/testbed/` | `boot.sh` (the solo board in QEMU), `keys.py`, `mouse.py`, `shot.sh`, `waitlog.sh`, `stop.sh`, `mkcard.sh` |

## Setting up the MacBook, step by step

Apple Silicon and Intel Macs alike. About 2 GB of downloads and 6 GB of disk.

1. **Docker.** Install [OrbStack](https://orbstack.dev) (lighter and faster for bind mounts) or
   [Docker Desktop](https://www.docker.com/products/docker-desktop/), and start it once. Either
   runs Linux containers natively on Apple Silicon (linux/arm64) and on Intel (linux/amd64), and
   routes a container's outbound connections to the Mac's LAN, so the Pi is reachable from inside.
   With Docker Desktop, give it at least 4 CPUs and 8 GB of memory (Settings, Resources).
2. **git** (Xcode's command line tools: `xcode-select --install`) and, if you like, the GitHub
   CLI (`brew install gh`, `gh auth login`) on the Mac. The repositories are public over HTTPS;
   pushing needs your GitHub login, on the Mac or in the container (`gh auth login` there).
3. **The workspace and the image:**
   ```
   curl -fsSLO https://raw.githubusercontent.com/farsonic/Aurora68/main/dev/bootstrap.sh
   bash bootstrap.sh ~/aurora68-work --extras "--claude"
   ```
   This clones Aurora68 (`main`), Riptide, Lagoon and Quill (`mac-port`) and Cornerstone
   (`main`) into `~/aurora68-work`, fetches Emu68's submodules, links `~/aurora68-work/CLAUDE.md`
   to `pistorm-se/dev/workspace/CLAUDE.md`, and builds the image `aurora68-dev` with Claude Code
   in it (five to ten minutes). Run it again at any time to fast-forward the clean checkouts.
4. **Your own files**, which no repository holds (`~/aurora68-work/inputs/README.txt` lists them):
   copy your Macintosh IIci ROM to `~/aurora68-work/pistorm-se/rom/original/MacIIci_368CADFE.rom`
   (bootstrap makes the folder; git ignores what is in it)
   and your System 7.1 test image (aurora-test3.image is a good one) to `~/aurora68-work/inputs/`.
5. **Check it:** `~/aurora68-work/pistorm-se/dev/shell.sh pistorm-se/dev/tools/buildall.sh --test`
   builds the kernel, the Mac side, Riptide, Lagoon and Octavo and runs the native tests
   (about ten minutes the first time).
6. **Claude Code, in the container:**
   ```
   ~/aurora68-work/pistorm-se/dev/shell.sh
   claude          # in /work; log in once (the login is kept in the volume aurora68-home)
   ```
   Then tell it what you want, for example: "the Mac is at IP 192.168.0.50, get at it".

## Where to run Claude Code: in the container (recommended) or on the Mac

**Recommended: inside the container**, as in step 6. Reasons:
- Every command in the CLAUDE.md files runs as written: the cross compilers, hfsutils, QEMU and
  the Python tools are on the PATH, the paths are the ones the documents use, and the testbed's
  monitor socket and pinet files are local. Nothing has to be remembered to be prefixed.
- It is the environment the work was done in (Ubuntu 24.04, gcc 13, QEMU 8.2.2, binutils 2.42),
  so a result there means the same as the record in docs/08.
- The files stay on the Mac (`~/aurora68-work` is a bind mount): Xcode, BBEdit or Finder see the
  same files, and nothing is lost when the container is rebuilt.
- The Pi is reachable from the container (Docker Desktop and OrbStack route outbound traffic to
  LAN addresses), which the remote-control service will need.

**The other way: Claude Code on macOS**, with the repositories on the Mac and builds through
`dev/shell.sh COMMAND` (it runs `docker exec` in the matching folder under `/work`). It works, and
the Mac's Claude Code keeps its own login and settings, but every build, test and testbed command
must go through `shell.sh`, paths inside commands are the container's (`/work/...`), and the
screenshots and logs the testbed writes are read from `~/aurora68-work/scratch/` on the Mac side.
Choose it if you want Claude Code's Mac integrations more than plain commands.

**Inside a Linux VM or on a Linux machine:** `dev/setup-linux.sh` (as root or with sudo) installs
the same packages, then `bootstrap.sh --no-container` and work directly. On Linux with Docker,
the container's user is uid 1000: run it as that user or give the workspace to uid 1000, or the
builds cannot write.

## QEMU's speed

The testbed's QEMU emulates a Raspberry Pi 3 with TCG (there is no hardware acceleration for the
`raspi3b` machine anywhere), four emulated cores on four host threads. On Apple Silicon the
arm64 container runs QEMU natively, and its TCG on the M-series cores is at least as fast as the
x86 cloud machine the timings in docs/08 come from (System 7.1 to the Finder in 25 to 40 seconds
on the solo board). Do **not** run the amd64 image under Rosetta or QEMU user emulation on Apple
Silicon for the testbed: QEMU's own code generator inside an emulated x86 process is several
times slower. Give the container four cores at least; one QEMU at a time.

## Optional parts

- `--extras "--servers"`: Samba, for Lagoon's SMB integration tests (they skip without it).
- `--extras "--mame-deps"`, then in the container `pistorm-se/dev/setup-linux.sh --mame
  /work/pistorm-se`: MAME 0.264 with Aurora68's patch, for the full IIci and SE testbeds
  (about 25 minutes to build). The solo board does not need it.
- `--extras "--amiga"`: vbcc with vasm, vlink and the AmigaOS target, and the NDK 3.2 headers, in
  `/opt/amiga`, for the Amiga builds of Lagoon, Quill and Riptide. amd64 only (vbcc's Linux
  binaries are x86-64; there is no ARM64 build): on Apple Silicon build a second image for those,
  `docker build --platform linux/amd64 -t aurora68-amiga --build-arg EXTRAS=--amiga pistorm-se/dev`.
  The binaries are vbcc's multi-target release of May 2022, which calls itself "V0.9i pre"; the
  repositories name 0.9h. **The MUI 3.8 SDK** (Quill, Cornerstone) may only be redistributed
  whole, so it is never downloaded or committed here: fetch `dev/mui/mui38dev.lha` from Aminet
  yourself and put its `C/Include` in `/opt/amiga/mui/include` (a bind mount or a `docker cp`).

## Natively on macOS, without the container

Realistic only in part. Homebrew has `qemu` (its `qemu-system-aarch64` has `raspi3b`),
`hfsutils`, `mtools`, `dosfstools`, `cmake`, `dtc`, `python` and `gh`, so the **testbed** can run
on the Mac itself with a kernel built in the container (`brew install qemu hfsutils mtools
dosfstools`, then `tools/run-solo.sh` from the Aurora68 checkout). The **builds** cannot,
without a great deal of work: there is no Homebrew package for the m68k-linux-gnu toolchain the
Mac side is built with (`binutils-m68k-linux-gnu`, `gcc-m68k-linux-gnu` and its libgcc), nor for a
big-endian aarch64 gcc for Emu68; building those cross toolchains from source on macOS is
possible but not something this project has done or tested. Keep the container as the way.

## What was tested, and how (19 November 2026)

On the cloud machine the work was done on (Ubuntu 24.04, x86-64, Docker 29):
- The image, built from this Dockerfile for linux/amd64 (about a minute; 1.8 GB unpacked).
- `bootstrap.sh --no-container` from GitHub into an empty folder (the five clones and Emu68's
  three submodules at their pinned commits, ten seconds), and again over it (fetched, the clean
  checkouts fast-forwarded, the changed one left alone).
- In the container on that workspace, `tools/buildall.sh --test`: the kernel (the size of test set 4's
  kernel from the same commit; only the build stamp differs), `make -C mac` and `make -C mac check`,
  Riptide's two Mac builds and its host tests (40 passed, 18 skipped for want of root and a TUN
  device), Lagoon's Mac build and host tests (Samba's skipped: no root for smbd), Octavo's build and
  Quill's native tests (2679 tests, 0 failed).
- `tools/testbed/boot.sh --net` with the image from `mkfullimage.sh`: the Finder at 24 s, pinet's
  DHCP answered, then `keys.py` typing into Aurora68 Log's filter, `mouse.py` holding the Apple menu open,
  `shot.sh` pictures of both, `stop.sh`.
- `mktestset.sh` with an update disk: the set, its checks and its zip; and its refusals (a key in
  an added file, a password in the notes, the IIci ROM by name and the same ROM renamed).
- `mkfullimage.sh` and `mkpartimg.py` against the real images: the update disk's first 491 blocks
  are byte for byte test set 4's.
- `setup-linux.sh --amiga`, then Lagoon's `make amiga` (Lagoon, LagoonPrefs, lagoon-handler built).
- `shell.sh`: the container started, a command run in the folder matching the current one, stopped.

Not tested: the linux/arm64 image (every package was checked to exist in Ubuntu 24.04's arm64
archive, and the kernel's big-endian build with the native arm64 gcc is expected to work as with
the cross one, but it was not run), Docker Desktop and OrbStack themselves, `--mame` (the MAME
build the recipe comes from was done by hand in November), `--claude` (Anthropic's installer was
reachable but not run), `--servers`. If the kernel does not build in the arm64 image, build it in
an amd64 one (`docker build --platform linux/amd64 ...`, slower under Rosetta but fine for
builds; keep the testbed's QEMU in the arm64 one) and say so in docs/08.
