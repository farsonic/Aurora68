<!-- SPDX-License-Identifier: MPL-2.0 -->
# Tools for the A/UX work (docs/23, docs/25)

Small host-side helpers written while getting A/UX 3.1.1 to run with `mac.mmu=full`. They read the owner's A/UX disk
image and the testbed's dumps; none of them contains anything of Apple's, and no part of the image may be committed.
Run the Python ones with `python3 -I`.

| Tool | What it does |
|---|---|
| `runse.sh SECS LOGDIR KERNEL [options]` | A/UX in the MAME SE full stack with the disk image at SCSI ID 0; takes the one-emulator lock. `PSMAC_RAM=64 ... mac.mmu=full mac.ram=64`. A/UX's Finder comes after 700 to 1200 emulated seconds, depending on the host. |
| `ufscat.py DISK 96 /path [ls]` | prints a file (or lists a directory) of the image's root file system (big-endian UFS, partition start sector 96 on the owner's image). Read only. |
| `coff.py FILE [PREFIX]` | lists an A/UX COFF file's sections; with PREFIX writes each to PREFIX.name.bin, for `m68k-linux-gnu-objdump -D -b binary -m m68k:68030`. |
| `crossers.py LISTING` | instructions of an objdump listing that run from one 4 KiB page into the next (docs/23 section 21). |
| `utrace.py FILE [N]` | the tail of a `mac.usertrace=` ring: the last user-mode blocks and bus errors before a program died. |
| `vread.py DUMP ROOT ADDR LEN` | reads a logical range from a RAM dump through A/UX's 68030 tables (the u-area is at $12FFF000 under the supervisor root). |
| `walk030.py DUMP TC ROOTHI ROOTLO ADDR...` | walks a 68030 translation tree in a RAM dump. |
| `kmsg.py DUMP` | A/UX 3.1.1's kernel message buffer from a RAM dump. |
| `kdis.sh DUMP VA LEN` | disassembles kernel code from a RAM dump (kernel text: logical $10000000 = physical $5C000). |

The scripted mouse for the testbed is `mame/act.lua` (`ACT_WAIT=menus` starts its plan when A/UX's Finder shows its
menus).
