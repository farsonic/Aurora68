# Mac-side software: drivers, extension, test application, declaration ROM

Everything the Macintosh runs to talk to the Pi lives in `mac/` (68k assembly, GNU `as` MIT
syntax, `binutils-m68k-linux-gnu`) and is packaged by small Python tools in `tools/mac/`
(no Apple tools needed). `make -C mac` builds all of it into `mac/build/`.

| Piece | Source | Output | What it is |
|---|---|---|---|
| `.PiDisk` | `mac/pidisk.s` | `pidisk.bin.raw` (DRVR image) | Block device driver over the mailbox BLOCK service: Open adds a drive queue entry per Pi block device and posts a disk-inserted event, Prime reads/writes 512-byte blocks synchronously, Control answers KillIO/verify/format/eject/icon/drive-info, Status answers drive status. |
| PiDisk extension | `mac/pidisk_init.s` + the DRVR | `PiDisk.bin` (MacBinary, type `INIT`) | System 6/7 extension: installs the DRVR from its own resource fork (`_DrvrInstall`), opens it, mounts every drive it added (`_MountVol`). Used until the declaration ROM path loads the driver from ROM. |
| `.Network_EtherNet_PiStorm` | `mac/enet.s` | `enet.bin.raw` | Apple `.ENET`-interface Ethernet driver over the NET service: EAttachPH/EDetachPH, ERead (default handler: queued parameter blocks completed from a VBL task), EWrite (WDS gather, source address inserted, 60-byte padding), EGetInfo (address + 3 counters), EAddMulti/EDelMulti, ESetGeneral, ERdCancel; real protocol handlers are called with the A3/A4/D1 convention and ReadPacket/ReadRest. |
| Declaration ROM | `tools/mac/mkdeclrom.py` | `psmac.declrom` | Slot $9 card: board sResource (BoardId 0x5053, vendor info), `PiDisk` functional sResource with the DRVR in its sDriverDir (opened at start by the Start Manager), `Network_EtherNet_PiStorm` sResource (catNetwork/typeEtherNet, not opened at start: the `.ENET` shell opens it through OpenSlot when MacTCP or EtherTalk ask). Served by Emu68 at 0xF9FF0000 (`mac.declrom=` in the testbed). |
| PiNet | `mac/pinet.s` | `PiNet.bin` (application) | Test application: shows the mailbox header, PiDisk 0, finds the Ethernet card through `SGetTypeSRsrc`, opens `.ENET` with `OpenSlot`, prints the address from EGetInfo, sends an ARP request for 192.168.68.1 and waits for the reply through ERead. Pair with `tools/pinet.py` on the host. |

## Tools
- `tools/mac/rsrc.py`: resource fork writer (type list, reference lists, name list; 28-byte map header) and MacBinary II writer. `hcopy -m` understands MacBinary, so files land on HFS images with their resource forks.
- `tools/mac/mkinit.py`: DRVR + INIT code image → extension file. `tools/mac/mkapp.py`: flat code image → application (CODE 0 jump table, CODE 1, SIZE). `tools/mac/hfsput.py`: copy into a folder of an image (hfsutils).
- `tools/mac/declrom.py`: declaration ROM builder (format block at the top, sResource directory, entries with self-relative 24-bit offsets, sBlocks, sExec blocks, CRC = rotate-left-and-add verified against the IIci ROM's own built-in declaration ROM). `tools/declrom.py` parses the result.
- `tools/pinet.py`: host peer for the testbed net service (answers ARP, ICMP echo and UDP echo on port 7, or bridges to a TAP).

## Workflow in the testbed
```
make -C mac                                        # drivers, extension, app, declaration ROM
cp images/incoming/sys710_patched.image images/sys710pi.image
make -C mac install IMG=../images/sys710pi.image   # PiDisk into System Folder:Extensions, PiNet in the root
chdman createhd -i images/sys710pi.image -o images/chd/sys710pi.chd -f
dd if=/dev/zero of=images/pidisk/pidisk.img bs=1M count=20 && hformat -l PiDisk images/pidisk/pidisk.img
ROM=rom/patched/MacIIci_psmac.rom HD=images/chd/sys710pi.chd tools/run-fullstack.sh 480 \
    mac.disk0=$PWD/images/pidisk/pidisk.img mac.declrom=$PWD/mac/build/psmac.declrom mac.net=/tmp/psmac-net
python3 tools/pinet.py /tmp/psmac-net peer        # in another shell
```
68k code can log to the Pi console through the INFO service (`log "text"` macro in the sources);
the lines appear as `[MAC] ...` in `emu68.log`.

## Facts the drivers rely on (from Inside Macintosh and Designing Cards and Drivers)
- Driver header: flags, delay, event mask, menu, five offsets, Pascal name, then a word-aligned
  version word (the `.ENET` shell and the Start Manager compare versions).
- Slot Manager: the format block is probed at $FsFFFFFF (byte lanes $0F for a 32-bit ROM), test
  pattern $5A932BC7, format 1, revision 1, CRC over the whole image; the board sResource is found by
  type (1/0/0/0) and must carry a BoardId; sResources without sRsrcFlags are opened at start.
- Ethernet clients find cards with `SGetTypeSRsrc` (category 4, type 1, mask 3) and `OpenSlot
  ".ENET"`; protocol type 0 means 802.3 (length field ≤ 1500) frames and goes to the LAP Manager;
  `ERead` returns the whole frame including the 14-byte header; the driver inserts the source address
  and pads to 60 bytes.
- Buffer addresses handed to the Pi are logical; in 24-bit mode the driver strips the high byte.
