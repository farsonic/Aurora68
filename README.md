# Aurora68 — PiStorm-SE

An Emu68-based accelerator and expansion for the Macintosh SE: a Raspberry Pi in the 68000
socket presents a 68040, runs Apple's Macintosh IIci ROM, keeps the SE's own screen, keyboard,
mouse, floppy and SCSI chip working, and adds memory, disk images from the SD card, Ethernet,
WiFi and Bluetooth through a NuBus card that exists only in software.

**Building it:** `CLAUDE.md` lists every tool and version, the build command for each artifact (kernel, Macintosh side, ROM patch, MAME testbed, disk images, containers), how the testbed is run and how each dependency is updated.

Start with `docs/00-overview.md`: it tells the story of the project and points to the working
documents for each part. The day-by-day record of what was found and fixed is `docs/08`.

## Layout
| Directory | Contents |
|---|---|
| `emu68/` | the Emu68 fork (vendored; its own history is the branch `emu68-psmac`): the kernel. `src/psmac/` is the Macintosh host layer (bus bridge, 68030 MMU shim, mailbox window); `src/alice/` are the Alice68 service modules (network, Bluetooth, status, USB) with their Macintosh glue |
| `mac/` | the Macintosh side in 68k assembly: the `.PiDisk` block driver, the `.ENET`-interface Ethernet driver, the `.PiInput` keyboard-and-mouse driver, the `.Display_Video_PiStorm` second-display driver, the PiDisk and PiMach extensions, the PiNet, PiPing, PiScreens and PiColor test applications, the PiWiFi, PiBlue and PiStatus applications, a Makefile (`make check` compares the drivers' structure offsets with the kernel's header) |
| `tools/` | ROM patching, the initramfs container builder (`mkcontainer.py`: ROM, declaration ROM and firmware in one cpio for the Pi), the full-stack testbed launcher, the host network peer, debugging helpers |
| `tools/mac/` | resource fork, MacBinary, application, extension and declaration ROM builders (pure Python) |
| `rom/` | ROM patch descriptions and the patch tool's Makefile (ROM images themselves are not in the repository) |
| `mame/` | Lua scripts for the MAME testbed and the testbed's configuration |
| `reference/` | the MAME testbed patch and instructions to recreate the MAME checkout; third-party checkouts live here but are not committed |
| `docs/` | the documents |

## Building and running the testbed
The kernel: see `docs/08` ("What exists"). The Mac side: `make -C mac`. The whole stack:
`tools/run-fullstack.sh SECONDS [bootargs...]` with `ROM=`, `HD=`, `LOGDIR=` in the environment;
`docs/09` gives the complete workflow with a disk image, the declaration ROM and the network peer.

## Licences
The Emu68 fork is MPL-2.0 like Emu68 itself. The Alice68 service modules under `emu68/src/alice/`
are GPL-2.0-only, as in Alice68. The Macintosh-side drivers, tools and documents of this project
are MPL-2.0. Apple ROM images and disk images are not distributed.
