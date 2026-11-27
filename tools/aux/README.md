<!-- SPDX-License-Identifier: MPL-2.0 -->
# tools/aux: working on A/UX without a keyboard

Helpers for the A/UX work (docs/23-mmu.md, docs/24-aux-network.md). They read and change a disk image of A/UX
3.1.1 from the host and take its programs apart. The image, and everything read out of it, is Apple's: none of
it belongs in the repository, and what these tools print about Apple's code (disassembly, symbol lists) stays in
a scratch directory. The readers that came first (`ufscat.py`, `coff.py` and others) are added separately.

The root file system of the owner's image starts at sector 96 of the disk (a big-endian 4.2BSD file system);
the examples use that.

| Tool | What it does |
|---|---|
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
