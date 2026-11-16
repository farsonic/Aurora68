# Service core and mailbox: Pi-side services for the Mac

## Roles of the four Pi cores (psmac fork)
| core | upstream Emu68 role | psmac role |
|---|---|---|
| 0 | 68k JIT | 68k JIT (unchanged) |
| 1 | async serial log | async serial log (unchanged) |
| 2 | PiStorm housekeeping (IPL sampling) | **service core**: owns the bus transport (bridge in the testbed, CPLD/GPIO on hardware), polls IPL at 1 kHz, and runs the mailbox services (storage, network, Bluetooth/USB, RTC) |
| 3 | PowerPC | unused by us (PPC disabled) |
Implemented today in `emu68/src/psmac/psmac_bus.c`: `psmac_service_core()` (bus transport owner + IPL poll, request hand-off slot from core 0). The mailbox services below plug into the same loop.

## Mailbox window seen by the 68k
A 64 KiB window at a fixed physical address the IIci decoder leaves free and the donor ROM never
touches: **0x51000000** (32-bit mode only; drivers switch with SwapMMUMode when called from 24-bit
mode, exactly as 32-bit-clean NuBus drivers do). It is ordinary Pi RAM mapped into the 68k space
(cached, coherent on the Pi), except the last page which is the doorbell and traps to the service core.
```
0x51000000  +0x0000  magic 'PSMB', version, feature bits, service count     (RO for the Mac)
            +0x0100  service descriptor table: {type, id, ring offset, caps}  (RO)
            +0x1000  per-service rings: request ring (Mac->Pi) / completion ring (Pi->Mac),
                     16-byte descriptors {cmd, flags, tag, len, bufaddr(phys), lba/arg}
            +0x8000  bounce buffers (for drivers that cannot hand over 68k physical addresses)
0x5100F000  doorbell page: write = kick service core (traps to psmac_bus_write)
0x5100F004  interrupt ack / status (Pi raises a 68k level-2 "slot" interrupt via the RBV path or,
            simpler, drivers poll the completion ring from their VBL task)
```
Data transfers: the Pi reads/writes the Mac buffer directly (it is Pi RAM) using the physical
address the driver provides; drivers obtain it with `LockMemory`/`GetPhysical` under VM, or run in
32-bit mode with VM off (System 6/7 without VM: logical = physical for RAM, see MMU shim notes).

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
