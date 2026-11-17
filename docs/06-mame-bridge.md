# MAME `maciici_ps`: the motherboard half of the software testbed

Status 2026-11-16. Implements the bus-bridge side of `docs/05-bridge-protocol.md` inside MAME
0.264 (shallow clone in `reference/mame`). The Macintosh IIci's 68030 is replaced by
`bridge68k_device`, a `cpu_device` that executes nothing: it serves bus cycles that arrive
over a UNIX-domain socket on its 32-bit big-endian program space, so an external CPU
emulator (Emu68 under QEMU) can own CPU/RAM/ROM and use MAME's VIA1, RBV video, ASC, ADB,
SCC, SWIM and 5380 SCSI as its motherboard.

## Files

| path | what |
|---|---|
| `reference/mame/src/mame/apple/bridge68k.h/.cpp` | `bridge68k_device` (`BRIDGE68K`): socket server + bus access + timing model |
| `reference/mame/src/mame/apple/maciici_ps.cpp` | `maciici_ps` machine: copy of `maciici.cpp` (state class `maciici_ps_state`) with the CPU swapped |
| `reference/mame/src/mame/mame.lst` | one added entry (`@source:apple/maciici_ps.cpp` / `maciici_ps`), needed by the SOURCES build |
| `tools/build_mame_psmac.sh` | the build command below |
| `tools/bridge_client.py` | Python client (`read8/16/32`, `write8/16/32` incl. posted, `poll`, `reset_ack`, CLI, `--selftest`) |
| `tools/bridge_e2e.py` | end-to-end test: starts MAME headless and drives it through the socket |

Nothing under `src/devices` or `scripts/src/*.lua` was touched: the device lives next to the
driver (makedep picks `bridge68k.cpp` up automatically because `maciici_ps.cpp` includes
`bridge68k.h`).

## Build

Dependencies (Ubuntu 24.04): `libsdl2-dev libsdl2-ttf-dev libfontconfig-dev qtbase5-dev
libxinerama-dev libxi-dev libxrandr-dev libxcursor-dev libasound2-dev libpulse-dev
libgl1-mesa-dev libutf8proc-dev libflac-dev libjpeg-dev libportmidi-dev libpugixml-dev
rapidjson-dev` (plus the usual gcc/g++/make/python3).

```
cd reference/mame
make SUBTARGET=psmac SOURCES=src/mame/apple/maciici.cpp,src/mame/apple/maciici_ps.cpp \
     REGENIE=1 NOWERROR=1 TOOLS=0 -j4          # = tools/build_mame_psmac.sh
./mamepsmac -listfull
```

`maciici.cpp` is in SOURCES on purpose: `maciici_ps` is declared as a *clone* of `maciici`
(`COMP(1989, maciici_ps, maciici, ...)`) so that it loads the ROM set from
`roms/maciici/`. MAME resolves the parent through the compiled-in driver list, so the parent
must be in the same binary; with `maciici_ps.cpp` alone the machine still builds and runs but
the four ROM files would have to be copied/symlinked to a `maciici_ps/` directory. Having the
stock IIci in the same binary is also handy for A/B comparisons. The build takes
about 25 minutes on 4 cores; use `JOBS=-j2 tools/build_mame_psmac.sh` if memory is
tight. Genie names a SOURCES build after the
subtarget, so the binary is `reference/mame/psmac`; the build script adds the symlink
`reference/mame/mamepsmac -> psmac` so both names work. Incremental rebuilds after editing
`bridge68k.cpp`/`maciici_ps.cpp` take ~1 minute (compile + link; drop `REGENIE=1`).

## Run

```
PSMAC_BRIDGE_SOCK=/tmp/psmac-bridge.sock \
reference/mame/mamepsmac maciici_ps -rompath mame/roms -video none -sound none -nothrottle \
    -hard1 images/chd/hd20.chd -snapshot_directory /tmp/snap -seconds_to_run 20
```

MAME prints `bridge68k: listening on /tmp/psmac-bridge.sock` once the machine starts
(the socket is created on the first scheduler slice, not at `-validate`/`-listxml` time).
A client connects and sends 12-byte requests; `bridge68k: client connected / disconnected`
is logged. One client at a time; a new connection replaces the current one. The socket file
is unlinked at exit (and a stale one is unlinked at start).

Quick manual poke:

```
tools/bridge_client.py poll
tools/bridge_client.py read8 0x50F01C00          # VIA1 IER
tools/bridge_client.py write8 0x50F26010 0x00    # RBV: video on, 1bpp
tools/bridge_client.py --posted write32 0 0xFFFFFFFF
tools/bridge_client.py dump 0x50F26000 0x20
```

## How bridge68k works

* `cpu_device` with one address space: `AS_PROGRAM`, `ENDIANNESS_BIG`, 32 data bits, 32
  address bits. State for the debugger: `PC` (= address of the last access), `IPL`, `FLAGS`
  (R reset-requested, B bus-error latched, P reply pending, C client connected).
  Trivial disassembler (`dc.w`).
* `execute_run()`: on first call creates the listening socket (`$PSMAC_BRIDGE_SOCK`, default
  `/tmp/psmac-bridge.sock`). Then, while the slice has cycles: `poll()` the listen + client
  descriptors (timeout 1 ms when idle, 0 when behind wall-clock), accept/receive, and for
  each complete 12-byte request:
  * op 1 READ / op 2 WRITE, size 1/2/4 -> `read_byte/word/dword` / `write_*` on the program
    space (the `_unaligned` variants for odd/misaligned addresses, like the 68030). 4 cycles
    are charged (`m_icount -= 4`). The reply is **deferred** (see below). WRITE with flags
    bit0 (posted) gets no reply.
  * op 3 POLL -> immediate reply, no access. op 4 RESET_ACK -> clears the reset flag, reply.
  * Reply: `status` 0 OK / 1 bus error, `ipl` = highest asserted line 1..7 (0 if none),
    `flags` bit0 reset requested, bit1 HALT, `data` right-aligned.
* `execute_set_input()`: lines 1..7 (`M68K_IRQ_n`) maintain an asserted mask; line 16
  (`M68K_LINE_BUSERROR`, same value as `m68kmusashi.h`) latches a bus error when asserted.
  `execute_input_edge_triggered()` returns true for 16 and 7 so that
  `pulse_input_line(..., attotime::zero)` is accepted, as with the real 68030 core.
  `INPUT_LINE_HALT`/`INPUT_LINE_RESET` are handled by `device_execute_interface`
  (suspend / `device_reset()`); `device_reset()` sets the reset-requested flag.
* Idle time: when no request is pending the device converts elapsed wall-clock time
  (`osd_ticks()`) into cycles at the device clock (25 MHz) and consumes them, capped at 10 ms
  worth (250 000 cycles) per step, so emulated time follows real time even with
  `-nothrottle`. When it is ahead of wall-clock it sleeps in `poll()` (1 ms), so the host CPU
  load is a few percent while idle; while a client is hammering the socket it never sleeps.

### Why the reply is deferred by one scheduler slice

MAME's `set_input_line()` is synchronised through the scheduler: a `pulse_input_line(
M68K_LINE_BUSERROR)` issued by a handler *during* the access only reaches
`execute_set_input()` after `execute_run()` has returned. The device therefore performs the
access, returns from `execute_run()` (leaving the rest of the slice unused, which MAME
accounts correctly), and sends the reply at the start of the next `execute_run()` call, when
the bus-error latch is valid. Likewise, if the access made the motherboard assert HALT (the
SCSI pseudo-DMA FIFO fill in `macscsi.cpp`), the base class suspends the device and the reply
is delayed until the hold ends: that is the DTACK wait of the protocol document. Cost: one
scheduler round trip per access (microseconds, far below the socket round trip).

## Protocol deviations / notes

1. **HALT flag (reply flags bit1)** is reported from `suspended(SUSPEND_REASON_HALT)` at reply
   time. Because a reply can only be sent while the device is running, it is 0 in practice:
   a hold shows up as reply latency, not as a flag. The CPU side must not rely on bit1.
2. **Malformed requests** (unknown op, size not 1/2/4) get a reply with `status = 2` and are
   logged, instead of being ignored. Not in the protocol document (which does not define the
   case); harmless for conforming clients.
3. **Posted writes that bus-error** are lost silently, as on the real PiStorm (no reply slot).
4. **ROM overlay**: unlike `maciici`, the machine never maps the ROM at address 0. The CPU side
   owns ROM and RAM and only forwards *writes* to 0x00000000-0x0007FFFF, so a write there must
   always land in the RAM the RBV displays; the stock driver would drop them until something
   read 0x40000000 (`rom_switch_r`) through the bridge, which Emu68 never does. Reads of
   0x40000000-0x4FFFFFFF still return ROM data.
5. **Reset flag at start**: `device_reset()` runs at machine start, so the very first reply
   carries flags bit0 = 1. The CPU side should answer with RESET_ACK once (the test does).
6. **Interrupt lines**: `maciici` only drives IRQ 1 (VIA1), 2 (RBV/VIA2) and 4 (SCC) and
   uses `M68K_LINE_BUSERROR` only from the SCSI pseudo-DMA timeout path, so those are the
   values `ipl`/`status` can take on this machine.
7. The machine is flagged without `MACHINE_SUPPORTS_SAVE` (sockets are not save-stateable).
8. `mamepsmac -validate` passes; `-verifyroms maciici_ps` reports `romset maciici_ps [maciici]
   is good`. The IIsi variant of the original driver was not carried over.
9. `-verbose` prints bridge statistics at exit (requests, connections, `execute_run` calls,
   polls, cycles, time spent in the device), useful when tuning the timing model.
10. **Not bridge-related, but worth knowing for the CPU side**: MAME's `ncr5380` re-arms a
    0 ns state timer forever if "start DMA initiator receive" (register 7) is written while no
    target asserts REQ -> emulated time freezes and the bridge stops replying. The stock
    `maciici` hangs identically; the IIci ROM never does this (it selects a target first).
11. The **Average speed** MAME prints is ~100 % by construction (the device pins emulated time
    to wall-clock); with `-nothrottle` the stock `maciici` runs at ~500 % on this host.

## The Macintosh SE machine (17 November, 04:00)
`macse_ps` is the SE parts of MAME's `mac128.cpp` on a `bridge68000_device`: the same bridge
class with a 16-bit data bus and 24-bit addresses (`bridge68k_device`'s protected constructor
takes the widths; `bus_read`/`bus_write` pick the `memory_access` specific for the bus), the
replies held a slice for the SE's SCSI window ($580000-$5FFFFF) instead of the IIci's, and
byte writes on the 16-bit bus smeared onto both lanes as a 68000 does, which the SE's 5380
decoding relies on. The overlay is cleared by an access to the ROM's own space ($400000), as
the BBU does, besides mac128.cpp's "first RAM write". ADB is wired the IIci way (`macadb`
straight on the VIA, `set_mcu_mode(false)`), because the SE's 6805 ADB modem needs a ROM the
testbed does not have and the IIci ROM drives ADB that way regardless. The ROM folder is
`mame/roms/macse_ps` (a link to `macse`): the parent `macse` is not in this subtarget. Run with
`MACHINE=macse_ps tools/run-fullstack.sh ... mac.board=se mac.declrom=.../psmac-512x342.declrom`;
its PRAM is `mame/nvramFS/macse_ps/rtc` (copy the IIci's for 32-bit addressing).

## Tests

### Build / static checks

```
$ ./mamepsmac -listfull | head -4
Name:             Description:
maciici           "Macintosh IIci"
maciici_ps        "Macintosh IIci (psmac bridge CPU)"
maciisi           "Macintosh IIsi"
$ ./mamepsmac -validate            # rc=0, no output
$ ./mamepsmac -verifyroms maciici_ps -rompath ../../mame/roms
romset maciici_ps [maciici] is good
$ python3 tools/bridge_client.py --selftest
bridge_client selftest: OK
```

### End-to-end (`tools/bridge_e2e.py`, 2026-11-16)

Exact run (the script starts MAME itself and uses a private socket path so a concurrently
running Emu68 setup on `/tmp/psmac-bridge.sock` is not disturbed):

```
PSMAC_BRIDGE_SOCK=/tmp/psmac-bridge-e2e.sock reference/mame/mamepsmac maciici_ps \
  -rompath /home/user/pistorm-se/mame/roms -video none -sound none -nothrottle \
  -hard1 /home/user/pistorm-se/images/chd/hd20.chd -snapshot_directory /tmp/psmac-e2e-snap \
  -seconds_to_run 20 -skip_gameinfo -nvram_directory /tmp/psmac-e2e-nvram -cfg_directory /tmp/psmac-e2e-cfg
```

Output:

```
starting: /home/user/pistorm-se/reference/mame/mamepsmac maciici_ps -rompath /home/user/pistorm-se/mame/roms -video none -sound none -nothrottle -hard1 /home/user/pistorm-se/images/chd/hd20.chd -snapshot_directory /tmp/psmac-e2e-snap -seconds_to_run 20 -skip_gameinfo -nvram_directory /tmp/psmac-e2e-nvram -cfg_directory /tmp/psmac-e2e-cfg
connected to /tmp/psmac-bridge-e2e.sock after 0.2 s
  [PASS] initial POLL reports reset requested (flags bit0) -- flags=01 ipl=0
  [PASS] RESET_ACK clears the reset flag -- flags=00
  VIA1 @50F00000: ORB=48 ORA=C7 DDRB=00 DDRA=00 T1CL=51 T1CH=DD T1LL=F3 T1LH=B5 T2CL=4D T2CH=DD SR=00 ACR=00 PCR=00 IFR=00 IER=80 ORA_NH=C7
  [PASS] VIA1 alias 0x50000000 matches 0x50F00000
  [PASS] VIA1 alias 0x50F40000 matches 0x50F00000
  [PASS] VIA1 IER reads 0x80 (no sources enabled) after reset -- IER=80
  [PASS] VIA1 port A reads 0xC7 pattern (IIci PA6|PA2|PA1 + outputs) -- ORA=C7
  [PASS] VIA1 16-bit read duplicates the byte on both lanes -- 8080
  [PASS] VIA1 T1 counter changes over 20 ms (emulated time follows wall clock) -- B760 -> 77AC
  RBV  @50F26000: 4F 06 7F 00 00 00 00 00 00 00 00 00 00 00 00 00 70 00 00 00
  [PASS] RBV alias 0x50026000 matches 0x50F26000
  [PASS] RBV reg 0x10 = video off | montype 6 (0x70) -- 70
  [PASS] RBV reg 0 reset value 0x4F -- 4F
  RBV IER=00 IFR=00
  [PASS] RBV extended IER/IFR readable -- IER=00 IFR=00
  [PASS] ipl is 0 with all VIA1 sources disabled -- ipl=0
  [PASS] VIA1 CA1 (60 Hz) interrupt raises ipl 1 -- ipl=1 IFR=82
  [PASS] VIA1 IFR shows IRQ|CA1 (0x82) -- IFR=82
  [PASS] ipl drops to 0 after disabling/acknowledging -- ipl=0
  [PASS] RBV video enabled, 1bpp (reg 0x10 reads 0x30 = montype only) -- 30
  [PASS] posted writes landed (0x00000000/0x3FC = FFFFFFFF, 0x400 untouched) -- FFFFFFFF FFFFFFFF 00000000 (256 posted writes in 0.8 ms)
  [PASS] byte/word lanes big-endian in RAM -- 00AABEEF
  [PASS] unaligned 32-bit read -- FFFF0000
  [PASS] SCSI pseudo-DMA underflow returns status 1 (bus error) -- status=1 data=BB
  [PASS] access after the bus error is OK again -- mode=00 status=0
  [PASS] ROM readable at 0x40000000 (first long of IIci ROM = checksum 368CADFE) -- 368CADFE
MAME exited with 0; output:
    bridge68k: listening on /tmp/psmac-bridge-e2e.sock
    bridge68k: client connected
    bridge68k: client disconnected
    Average speed: 100.00% (19 seconds)
  snapshot /tmp/psmac-e2e-snap/maciici_ps/0000.png: 640x480, black pixels per row 0..11: [640, 640, 640, 640, 640, 640, 640, 640, 640, 640, 640, 640], white pixels in row 40: 640
  [PASS] final snapshot: rows 0-11 all black (0x400 bytes of 0xFF @ 80 bytes/row), row 40 white

24 checks, 0 failed
```

What the checks show:

* **Protocol**: first reply has flags bit0 (reset requested), RESET_ACK clears it; POLL carries
  ipl; posted writes get no reply and are ordered before the next request (256 posted dword
  writes in ~1.5 ms); malformed requests are rejected (client self-test).
* **VIA1** at 0x50F00000 (registers every 0x200) reads sane reset values (ORA = C7 = the IIci
  PA6|PA2|PA1 ID, IER = 80, T1 counting); the aliases 0x50000000 and 0x50F40000 return the same
  registers; 16-bit reads return the byte on both lanes as in `via_r`.
* **RBV** at 0x50F26000 and 0x50026000 are identical (pseudo-VIA reset values 4F 06 7F ...,
  reg 0x10 = 0x70 = video off | 13" RGB); the extended IER/IFR at +0x1C00/+0x1A00 are reachable.
* **Interrupts -> ipl**: enabling CA1 in VIA1 IER makes the RBV's 60.15 Hz tick raise the VIA
  IRQ, `field_interrupts` asserts line 1 and POLL reports ipl = 1 with IFR = 82; disabling and
  acknowledging drops it to 0.
* **Frame buffer**: with the CLUT programmed (pen FE white, FF black) and video enabled,
  0x400 bytes of 0xFF written to RAM 0..0x3FF with posted writes show up in the final
  snapshot as 12 fully black rows + 512 black pixels in row 12 (640x480, 80 bytes/row); row 40
  is white (`docs/img/maciici_ps-bridge-e2e.png`). Big-endian byte/word lanes and unaligned
  32-bit reads behave as on the 68030.
* **Bus error**: a SCSI pseudo-DMA read underflow (`macscsi.cpp` -> `scsi_berr_w` ->
  `pulse_input_line(M68K_LINE_BUSERROR)`) is reported as status = 1 on exactly that access;
  the next access is status 0 again.
* **Time**: emulated time follows wall-clock (`Average speed: 100.00% (19 seconds)` with
  `-nothrottle`), so VIA timers and the 60 Hz tick run at real rate while the CPU side is idle.

### Performance (same host, 4 cores)

| | |
|---|---|
| idle (no client), 8 s run, `-nothrottle` | 0.34 s user + 0.05 s sys = ~5 % of one core; 1841 slices, 14769 polls (7236 timed) |
| synchronous `read8` round trip (Python client) | 15.5 us -> 64 k requests/s |
| posted `write32` | ~196 k requests/s |
| `Average speed` with a client hammering the socket | 100.26 % (emulated time stays pinned to wall-clock) |

A first version of the idle loop skipped the 1 ms sleep whenever *any* wall-clock credit was
pending; wall-clock time kept accruing credit while it spun, so it busy-waited (18.6 M polls
in 5 s, ~98 % of a core). The sleep is now skipped only when the device is >= 1 ms behind
wall-clock.

### Stock `maciici` in the same binary

`./mamepsmac maciici -rompath ... -seconds_to_run 10` runs the IIci ROM at ~510 % speed and
produces the usual black screen with the "?" disk icon after 10 s, i.e. the extra source does
not disturb the reference machine.

