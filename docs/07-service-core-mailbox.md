# Service core and mailbox: Pi-side services for the Mac

## Roles of the four Pi cores (psmac fork)
| core | upstream Emu68 role | psmac role |
|---|---|---|
| 0 | 68k JIT | 68k JIT (unchanged) |
| 1 | async serial log | async serial log (unchanged) |
| 2 | PiStorm housekeeping (IPL sampling) | **service core**: owns the bus transport (bridge in the testbed, CPLD/GPIO on hardware), polls IPL at 1 kHz, and runs the mailbox services (storage, network, Bluetooth/USB, RTC) |
| 3 | PowerPC | unused by us (PPC disabled) |
Implemented today in `emu68/src/psmac/psmac_bus.c`: `psmac_service_core()` (bus transport owner + IPL poll, request hand-off slot from core 0). The mailbox services below plug into the same loop.

## Mailbox window seen by the 68k (revised: slot $9)
The Pi presents itself as a **NuBus card in slot $9**. The bottom 1 MiB of the slot's super slot
space, physical **0xF9000000**, is ordinary Pi RAM shared with the 68k; the IIci ROM's own tables map
it in both modes (24-bit: logical **0x900000**, 32-bit: 0xF9000000), so drivers never switch MMU
modes. The top 64 KiB (0xF9FF0000) holds a **declaration ROM** the Slot Manager finds at boot; it
carries the Mac-side drivers (`.PiDisk`, `.ENET`) like any NuBus card. The authoritative layout is
`emu68/src/psmac/psmb.h`:
```
+0x0000  header: magic 'PSMB', version, nservices, features, Pi/Mac RAM, uptime, firmware string
+0x0100  service table, 8 x 32 bytes: type (1 info, 2 block, 3 net, 4 hid, 5 rtc), command block offset
+0x1000  one 64-byte command block per service: req_seq, done_seq, cmd, args, buf (68k logical), len, status, result, xfer
+0x2000  block devices, 4 x 64 bytes: blocks, block size, flags, name
+0x4000  net receive ring: head (Pi), tail (Mac), 32 slots x (len + 1536 bytes)
+0x80000 bounce area (512 KiB)
```
Rules (the same ones the Alice68 boards use, so that kernel code ports across): 32-bit fields, a
magic and version first, payload written before the sequence word, ever-increasing counters with
`index % size`, drop when a ring is full, the Pi polls the 68k->Pi direction (the service core loop
runs the poll every iteration, far below a millisecond), no interrupts in v1 (drivers poll from a
VBL task or spin with a timeout). Emu68 is big-endian AArch64 like the 68k: no byte swapping on
either side. Buffer addresses are 68k logical; the service core translates them page by page with
the MMU shim, which also makes the 24-bit flag bits in the high byte harmless.

Implemented: `psmac_mbox.c` (info, block with host-file images `mac.disk0=/path`, net with a host
frame-file pair `mac.net=/path` for the testbed), window and declaration ROM carved from Pi RAM in
`psmac_setup_memory`, mapped by the MMU shim (`pi_phys`, window RW, declaration ROM RO), the
declaration ROM image loaded from `mac.declrom=/path`. Mac side: `mac/pidisk.s` (`.PiDisk` DRVR),
`mac/pidisk_init.s` (extension that installs it and mounts the volumes), packaged by
`tools/mac/mkinit.py` (resource fork + MacBinary, `tools/mac/rsrc.py`), installed with
`tools/mac/hfsput.py`; declaration ROM builder `tools/mac/declrom.py`.

### Relation to Alice68
The user's Alice68 kernel (Amiga PiStorm, same Emu68 base) already runs, on its service core, the
hardware drivers this project needs: VL805 xHCI with USB pass-through, WiFi over SDIO, GENET
Ethernet, Bluetooth HCI with firmware hand-over, a status page and log rings, published to the 68k
through shared-memory "boards" with exactly the pattern above. The plan is to port `alice_net.c`,
the xHCI/USB stack and the Bluetooth code onto the psmac service core behind the slot-$9 window,
keeping the board field names (`an_hdr`, `an_unit`, `an_slot`, `an_bt`, `pt_req`,
`alice68_status`) so the ARM code changes only in how the memory is mapped and how the 68k is
interrupted. The Mac drivers then speak the protocol `alice68net.device` speaks today.
Differences to keep in mind: the Mac has no autoconfig (fixed slot address instead), interrupts
would be injected by Emu68 directly into the JIT's IPL rather than through a virtual CIA, and the
VideoCore firmware mailbox sharing rules apply unchanged (one buffer per client below 1 GiB,
replies matched by address, retries, few calls after boot).

## Services and the Mac-side pieces
| service | Pi side (service core, ARM) | Mac side (68k, in ROM or as INIT) |
|---|---|---|
| block | SD card via Pi SDHC/EMMC; FatFs (exFAT/FAT32) image files `.hda/.img/.dsk`, APM partitions, whole-card passthrough; read/write/flush/eject/info | `.PiDisk` DRVR: drive queue entries per volume, APM aware, bootable (ROM resource + boot hook), Finder-visible icons, Control Panel "PiStorm" for mounting images |
| net | GbE (CM4) / WiFi (brcmfmac-class) / USB Ethernet; frame-level service (send/recv rings, MAC, link state), lwIP optional for Pi-only use | `.ENET` DRVR implementing the Apple Ethernet driver API (ENetAttachPH/DetachPH/Write/Read/GetInfo), so MacTCP and Open Transport work unchanged; MoofPort is the reference |
| hid/bt | Bluetooth HCI (UART to the Pi's radio), USB HID | ADB bridge: synthesize ADB keyboard/mouse events (Pi injects into the real ADB transceiver via VIA on hardware; in the testbed via MAME's macadb) |
| rtc/clock | Pi RTC/NTP time | ROM boot hook sets the Mac clock; control panel |
| console/debug | log ring, Emu68 control (cache, speed), firmware update | `PiStorm` control panel / cdev |

## Testbed mapping (QEMU raspi3b + MAME bridge)
- SD: `-drive file=sd.img,if=sd,format=raw` → Pi SDHOST model; FatFs over it.
- USB: QEMU raspi3b exposes the dwc2 host controller; `-device usb-net` / `usb-kbd` can be attached
  once a bare-metal USB stack exists (large; defer, use GbE/WiFi paths on hardware first).
- Network for development: simplest is a "virtual NIC" service backed by a QEMU-side pipe is NOT
  available without ARM-side code; keep network testing for the first hardware bring-up or use
  the MAME side (MAME has no Ethernet for the IIci either). Decision: develop `.ENET` against a
  loopback implementation of the frame service first.

## Order of work
1. (now) service core as transport owner + IPL poll → ROM boots through the bridge.
2. Mailbox window + `block` service with a RAM-disk backend (no SD code) → `.PiDisk` DRVR in the
   patched ROM boots System 7 from a mailbox volume in the testbed. This proves the whole stack.
3. SD + FatFs backend (bare metal driver from circle/other MPL-compatible source), `sesd` host tool.
4. `.ENET` + frame service (loopback → hardware NIC), then WiFi/BT/USB on the CM4 board.
