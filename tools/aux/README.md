<!-- SPDX-License-Identifier: MPL-2.0 -->
# tools/aux: working on A/UX without a keyboard

Helpers for the A/UX work (docs/23-mmu.md, docs/24-aux-network.md). They read and change a disk image of A/UX
3.1.1 from the host and take its programs apart. The image, and everything read out of it, is Apple's: none of
it belongs in the repository, and what these tools print about Apple's code (disassembly, symbol lists) stays in
a scratch directory. Run the Python ones with `python3 -I`.

The root file system of the owner's image starts at sector 96 of the disk (a big-endian 4.2BSD file system);
the examples use that.

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
| `ufspatch.py DISK 96 /path NEWFILE` | replaces a small file's contents in place, in the fragments the file already has (it allocates nothing, so the new contents may be at most the old size rounded up to a fragment, 1 KiB). Only on a copy of the image |
| `ufswho.py DISK 96 OFFSET...` | which file holds a byte offset of the image: for a byte pattern found by searching the raw image |
| `mknetimage.py SYSINITRC OUT` | the testbed's `/etc/sysinitrc`: A/UX runs `newconfig` once with the driver's questions answered, restarts, and tests the network at every later start, with its results in files on the image (docs/24 section 7) |
| `run-aux-net.sh SECS LOGDIR KERNEL DISK [options]` | A/UX in the MAME SE with the host's network peer; `stop-aux-net.sh` ends such a run early |
| `coffsyms.py FILE [syms]` | an A/UX COFF file's relocations, or with `syms` its symbols by section and address |
| `coffdis.py OBJECT OUT` | a driver module (`/etc/boot.d/*`) disassembled with its symbols and relocation targets beside the lines (needs `m68k-linux-gnu-objdump`) |
| `unixdis.sh UNIX SYMS VA LEN` | a piece of the kernel file `/unix` disassembled at its virtual address, with names from `coffsyms.py UNIX syms` (`symann.py` adds the names) |
| `etalk_test.sh [BUILD_DIR]` | the A/UX network card's chip model on the host (`etalk_test.c`, with `etalk_stub/` standing in for the kernel's headers): no A/UX needed, a second |

A session from nothing, for the network card:

```
unzip -p AUX.zip AUX_3_1_1GB.dsk | dd of=aux-pristine.dsk conv=sparse bs=1M     # the owner's image, kept as it came
cp --sparse=always aux-pristine.dsk aux-net.dsk                                  # the copy A/UX writes to
python3 tools/aux/ufscat.py aux-pristine.dsk 96 /etc/sysinitrc > sysinitrc.orig
python3 tools/aux/mknetimage.py sysinitrc.orig sysinitrc.new
python3 tools/aux/ufspatch.py aux-net.dsk 96 /etc/sysinitrc sysinitrc.new
tools/aux/run-aux-net.sh 3300 logs/net1 psmac-build/Emu68.img $PWD/aux-net.dsk mac.auxnet=on mac.auxnettrace=300
python3 tools/aux/ufscat.py aux-net.dsk 96 /auxnet.boot; ... /auxnet.newconfig; ... /auxnet.log
grep AUXNET logs/net1/emu68.log; less logs/net1/pinet.log
```

The scripted mouse for the testbed is `mame/act.lua` (`ACT_WAIT=menus` starts its plan when A/UX's Finder shows its
menus).
