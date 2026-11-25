# ROM patches: toolkit and patch points

Everything here is reproducible from the untouched originals in `rom/original/`
(`MacIIci_368CADFE.rom`, md5 aaca403bed2b3870b2327b42d298b5a3; `MacSE_B2E362A8.rom`,
md5 9fb38bdcc0d53d9d380897ee53dc1322). `make -C rom` rebuilds `rom/patched/*.rom`.

## 1. Toolkit

| Tool | Purpose |
|---|---|
| `tools/rompatch.py SET.json IN.rom OUT.rom [--dry-run] [-v]` | applies a JSON patch set, recomputes the checksum, prints a table of what changed |
| `tools/romlanes.py ROM OUTDIR` / `--join LANEDIR OUT.rom` | splits an IIci ROM into MAME's four byte-lane files (lane k = bytes k::4: `341-0736.um12`, `341-0735.um11`, `342-0734.um10`, `342-0733.um9`) and back |
| `tools/romfix.py IN OUT` | checksum only (32-bit sum of all 16-bit words from offset 4 to the end, stored at offset 0) |
| `tools/disrange.py FILE.dis lo hi` | print a disassembly range |
| `rom/Makefile` | `all` / `iici` / `se` / `check` (dry-run, verifies every `expect`) / `mame-lanes LANEDIR=dir [ROM=file]` / `clean` |
| `mame/lowmem.lua` | MAME autoboot script: screenshots every 600 frames and dumps the timing globals from physical RAM |

### Patch set format (`rom/patches/*.json`)
```json
{ "name": "...", "base": "0x40800000", "notes": ["..."], "fill": "0x00",
  "include": ["other.json"],                 // optional: apply those sets first
  "symbols": {"calib_store": "0x40800850"},  // ld --defsym for every "asm" item
  "patches": [
    {"name": "...", "at": "0x844", "expect": "45fa 0008", "bytes": "45fa 008c"},
    {"name": "...", "at": "0x8d2", "expect": "0000 ...", "asm": ["not.w %d0", "..."],
     "max_len": 14, "symbols": {"...": "0x..."}},
    {"name": "...", "append": "file.bin"},
    {"name": "...", "append_asm": ["stub: ...", "..."], "align": 65536}
  ] }
```
- `at` is a ROM **file offset**; the table prints offset and VMA (`base` + offset).
- `expect` (hex of the original bytes) is verified for every in-place item before anything
  is written; a mismatch aborts. `bytes` must have the same length as `expect`.
- `asm` is GNU as (MIT syntax) source, assembled with `m68k-linux-gnu-as -m68030`, linked
  with `m68k-linux-gnu-ld` at the patch VMA (linker script with an explicit section address
  so odd 4-byte alignment is honoured), `objcopy -O binary`, checked against `max_len`
  (= `len(expect)`) and padded with NOPs (`4E71`). Targets outside the snippet must be
  **symbols** defined in `symbols` (passed to `ld --defsym`); a literal absolute address in a
  PC-relative instruction fails to assemble because `as` does not know the final address.
  Use `%pc@(sym:w)` to force the 16-bit displacement form of `lea`.
- `append` / `append_asm` add data at the end (optionally `align`ed first); an `append_asm`
  snippet is linked at `base + current_size`, so it can carry absolute `jmp`s. After any
  append the image is padded to a multiple of 64 KiB with `fill` before the checksum is
  computed, so the checksum always covers the whole file.
- `include` lets a set layer on another (used by the test sets below; later the mailbox
  drivers etc. will layer on the timing set).

### Build and test
```
make -C rom                                    # rom/patched/MacIIci_psmac.rom, MacSE_psmac.rom
make -C rom mame-lanes LANEDIR=../mame/roms_patched/maciici
/usr/games/mame maciici -rompath mame/roms_patched -cfg_directory mame/cfgT \
   -nvram_directory mame/nvramT -snapshot_directory mame/snapT -video none -sound none \
   -nothrottle -seconds_to_run 150 -ramsize 8M -hard1 images/chd/hd20.chd \
   -autoboot_script mame/lowmem.lua -skip_gameinfo      # LOWMEM_LOG=file selects the log
```
MAME warns that the lane CRCs differ from its database; that is expected. MAME's `maciici`
ROM region is exactly 0x80000 bytes, so **appended** code cannot be exercised in MAME; the
IIci patches therefore stay in place. (The Pi serves the ROM from its own memory and the
image may grow; the SE set does grow.)

## 2. Patch set `iici.json`: timing calibration saturation (IIci ROM 368CADFE)

### The code (ROM offset 0x800, VMA 0x40800800, called by `bsr` from 0x4080019E)
```
40800800  movel 0x64,-(sp)            save vector $64 (level-1 autovector = VIA1)
40800804  movew sr,-(sp) ; oriw #$700,sr
4080080a  moveal 0x1d4,a1             VIA1 base
4080080e  bclr #5,$1600(a1)           ACR: T2 one-shot
40800814  moveb #$ff,$1200(a1)        T2C-H
4080081a  moveb #$a0,$1c00(a1)        IER: enable T2 interrupt
40800820  moveq #12,d1 ; moveq #3,d2  T2 reload = $030C ticks of 783.36 kHz = 0.996 ms
40800824  moveal sp,a3
40800826  lea 0x40800866,a0 ; movel a0,0x64      install handler
4080082e  lea 0x4080086e,a4                       table: {loop offset, lowmem addr} x4, 0
40800832  moveal a4,a0 ; addaw (a4),a0 ; moveq #2,d0 ; lea 0x4080083e,a2 ; jmp (a0)
                                                  warm-up pass (3 iterations)
4080083e  moveal a4,a0 ; addaw (a4)+,a0 ; moveq #-1,d0
40800844  lea 0x4080084e,a2        <-- PATCH 1
40800848  andiw #$f8ff,sr ; jmp (a0)              interrupts on, run the loop
4080084e  notw d0                  <-- continuation: count = ~d0.w
40800850  moveaw (a4)+,a0 ; movew d0,(a0)         store to $D00 / $D02 / $B24 / $CEA
40800854  tstw (a4) ; bnes 0x40800832             next entry
40800858  moveb #$20,$1c00(a1) ; movew (sp)+,sr ; movel (sp)+,0x64 ; rts
40800866  tstb $1000(a1) ; moveal a3,sp ; jmp (a2)   T2 handler: ack, drop frame, continue
4080086e  .word 0x12,$D00, 0x1C,$D02, 0x2E,$B24, 0x42,$CEA, 0                 table
40800880  moveb d1,$1000(a1) ; moveb d2,$1200(a1) ; dbf d0,. ; jmp (a2)       TimeDBRA
4080088e  moveal 0x1d8,a0 ; (start T2) ; btst #0,(a0) ; dbf d0,.-4 ; jmp (a2) TimeSCCDB (SCC read)
408008a4  moveal 0xc00,a0 ; (start T2) ; btst #0,$40(a0) ; dbf ; jmp (a2)     TimeSCSIDB (5380 status)
408008bc  lea $1c00(a1),a0 ; (start T2) ; btst #0,(a0) ; dbf ; jmp (a2)       TimeVIADB (VIA IER read)
408008d2  14 bytes of 00 padding   <-- PATCH 2 (nothing references them; the next routine
408008e0  ... starts here, entered by braw from 0x408000B4)
```
`dbf` decrements `d0.w` and exits when it becomes -1. Starting from 0xFFFF, after k
iterations `d0.w = 0xFFFF - k`; the handler's `notw` yields k. If the timer has not fired
after 65536 iterations the loop falls through with `d0.w = 0xFFFF` and `jmp (a2)` reaches the
same `notw`, which produces **0**. This is the failure mode at JIT speed: a CPU doing more
than 65536 iterations per millisecond stores TimeDBRA/TimeSCCDB/TimeSCSIDB/TimeVIADB = 0 and
every delay derived from them (e.g. 0x40807854 `move.w TimeDBRA,d0 ; lsr.w #7 ; dbf`, 0x40807860
`mulu TimeSCSIDB,d1` in the SCSI selection BSY wait, 0x408688D8/EE, 0x40875410/26) collapses
to nothing. (Wrap-around proper never happens: `dbf` cannot run past -1.)

### The patch
| Offset | VMA | Old | New | Why |
|---|---|---|---|---|
| 0x844 | 0x40800844 | `45FA 0008` `lea 0x4080084e(pc),a2` | `45FA 008C` `lea 0x408008d2(pc),a2` | continuation (both the T2 handler's `jmp (a2)` and the loops' fall-through `jmp (a2)`) goes to the stub |
| 0x8D2 | 0x408008D2 | 14 x `00` (padding) | `4640` `not.w d0` / `6602` `bne.s +2` / `5340` `subq.w #1,d0` / `6000 FF76` `bra.w 0x40800850` / `4E71 4E71` | count = ~d0.w; a zero result (fall-through) becomes 0xFFFF = saturated; continue at the store |

Properties: the normal path (timer interrupt before 65536 iterations) produces exactly the
original value; the warm-up pass (a2 = 0x4080083E) is untouched; the stub lives in bytes that
no code or table references; the ROM size is unchanged so the MAME testbed can run it.
Checksum 368CADFE -> 368FAA5C.

Residual (pre-existing) issue: after a fall-through, T2 is still running and its interrupt
is still enabled until the next loop restarts the timer (or 0x40800858 disables it); if T2
expired exactly in that window the handler would run once more and store a second value /
skip a table entry. The window is ~20 instructions wide against a ~1 ms timer, and at JIT
speed the fall-through occurs long before the timer expires, so this is not addressed here.

### What saturation does and does not fix
With the stored counts at 0xFFFF, a derived delay is `N x 65535` iterations instead of the
intended `N x (true iterations per ms)`. On a CPU 10x faster than the 65536/ms limit the
delays are still 10x too short, but they are no longer zero (a zero TimeSCSIDB makes the
5380 phase waits time out immediately: "no SCSI devices"). Robust fixes, planned:
- Emu68 runs the four calibration loops (0x40800880..0x408008D1) in interpreter mode / with
  a per-iteration stall so the measured count is in the 68030 range, or
- the delay sites are re-pointed to a VIA1/Pi-timer based wait (see §4).

(Later items in the same set: the calibration values taken from the Pi, docs/08 item 14; the
cold-start RAM test, the chime's length and the SCSI selection timeout, docs/08 item 50; the
Memory Manager's 24-bit retry, §2b.)

### How the patches reach a real Pi
The SD card holds Apple's ROM as it is: the patched image is never distributed. `tools/mkdelta.py`
turns `rom/original` and `rom/patched` into `rom/patches/iici-368CADFE.delta.json` (the byte runs
that differ, with both checksum words) and `emu68/src/psmac/psmac_rom_delta.h` (the same runs as
a C table). At start the kernel copies the ROM out of the container and, when its checksum word
is 368CADFE, writes the runs over it and checks the sum of the result (`psmac_bus.c`: "stock IIci
ROM ... patched in memory, checksum word ... (verified)"). `tools/mkcontainer.py` applies the same
delta when it packs a stock ROM, so a container it made holds the patched image of its day; since
21 November 2026 the kernel also brings such an image up to date: the delta is a superset of every
earlier patch set (checked against each version of `iici.json` in the history: 368FAA5C, 369A8139,
369A5A97, and since 25 November 2026 36AC1C28), so it is written over any 512 KiB ROM that is not already the current one, and kept
only if the result's sum is the current checksum; anything else is copied back as it came. After
changing `iici.json`: `make -C rom check && make -C rom iici && python3 tools/mkdelta.py`, then
rebuild the kernel.

## 2b. The Memory Manager's 24-bit retry (21 November 2026, docs/08 item 78)

The IIci ROM carries one Memory Manager for both block layouts: 24-bit zones (8-byte block
headers, the zone's routines in the table at $1E00) and 32-bit zones (12-byte headers, the table
at $1F00). Low memory $1EFC holds its flags, read from parameter RAM byte $8A at start
($4080CC64): bit 2 says the Mac starts in 32-bit mode, bit 0 which layout the Memory Manager
uses now. A call that takes a pointer (DisposPtr and three others, through the vector at $1EE4)
or a handle (through $1EEC) goes through a dispatcher that finds the block's zone first:

```
4080dc2e  movea.l (sp)+,a1          a1 = the call's offset in the zone's table ($44: DisposPtr)
          movem.l d3-d7/a2-a6,-(sp) ; move.b $1EFC,-(sp) ; movea.l a1,a4 ; clr.w -(sp)   retry flag 0
4080dc3e  suba.l a1,a1 ; bsr $4080DB9C    check the block: the header 12 bytes (32-bit) or 8 bytes
                                          (24-bit, the pointer masked with Lo3Bytes) below it; its
                                          tag byte (0 free: -111), its zone, and the zone's first
                                          block's back pointer (zone+60 or +56) must be the zone (-113)
4080dc44  bmi.s $4080DC5C                 the check failed
          addq.l #2,sp ; move.l d2,d0 ; move.l 32(a6),d3 ; bne.s 1f ; move.l #$1E00,d3
        1:adda.l d3,a4 ; move.l (a4),-(sp) ; rts      jump through the zone's table (zone+32)
4080dc5c  tst.w (sp)+ ; bne.w $4080DDE0   failed after the retry: MemErr = d0, return
          bchg #0,$1EFC ; move.w #1,-(sp) ; bra.s $4080DC3E     check again in the other layout
```

The handle dispatcher at $4080DC6E is the same around $4080DB6A (which reads the master pointer
through the handle, masked in 24-bit mode) and retries at $4080DCCE. Neither knows how much memory
there is. In 32-bit mode with more than 16 MB the 24-bit retry checks a different address, the
pointer's low 24 bits, which is other RAM, and a uniform fill of any byte from $40 to $7F there
passes every test: the tag says nonrelocatable, the zone pointer is the fill masked to 24 bits
(more RAM made of the same fill), and that zone's back pointer, the fill masked again, is the
zone. The dispatcher then adds the fill to the table offset and jumps through it. On the user's SE
(128 MB, a $55 fill) Speedometer 3.23 disposing a pointer twice ended at $FFFFFFFF.

The patch (four items in `iici.json`): the two retries jump to stubs in the zero padding at
0x62A40 and 0x62A80, which do what the ROM did, except that when the 68k is in 32-bit mode
(MMU32Bit $CB2 not zero), the check that failed used the 32-bit layout and the pointer (or the
handle) is above $00FFFFFF, they return the 32-bit check's error instead of retrying:

| Offset | VMA | Old | New |
|---|---|---|---|
| 0xDC5C | 0x4080DC5C | `4A5F 6600 0080 0878 0000 1EFC 3F3C 0001 60D0` | `4EF9 4086 2A40` `jmp` the pointer stub, NOPs |
| 0xDCCE | 0x4080DCCE | `4A5F 660E 2049 0878 0000 1EFC 3F3C 0001 609E` | `4EF9 4086 2A80` `jmp` the handle stub, NOPs |
| 0x62A40 | 0x40862A40 | 48 x `00` | `tst.w (sp)+; bne 9f; tst.b $CB2; beq 1f; btst #0,$1EFC; beq 1f; cmpa.l #$00FFFFFF,a0; bhi 9f; 1: bchg #0,$1EFC; move.w #1,-(sp); jmp $4080DC3E; 9: jmp $4080DDE0` |
| 0x62A80 | 0x40862A80 | 56 x `00` | the same with a1 (the handle), `movea.l a1,a0` before the retry, `jmp $4080DC7E` |

Why it is safe. A 24-bit zone's blocks lie below 16 MB, and in 32-bit mode the 68k uses an address
as it is, so a pointer or handle with a non-zero high byte cannot be in one; the retry can only
find a block that is not there. Everything else takes the ROM's own path: 24-bit mode (MMU32Bit
0, as on every Mac with 8 MB and 32-bit addressing off, and while a driver runs in 24-bit mode),
a first check made in the 24-bit layout, addresses below 16 MB (where the mask changes nothing),
and every call whose first check passes, which is every legitimate one. The error returned is
what the 32-bit check found (memWZErr -111 for a free block, memAZErr -113 for a block whose zone
does not check), the kind of error a Mac with 16 MB or less returns for the same call: there the
retry looks at the same address in the other layout and fails too. Checksum 369A5A97 -> 36AC1C28.

## 2c. The 400K drive's speed through the seek wrapper (25 November 2026, docs/08 item 146)

The 400K single-sided drive of the 128K and 512K turns at the speed the Mac writes into the low bytes of
the sound buffer, one zone of 16 tracks at a time; the SE ROM's Sony driver does it in SetSpeed
($435012), called from a wrapper that every seek goes through ($4350DE: `bclr #11,d6; bsr SetSpeed;
move.l JSeek,-(sp); rts`). The IIci ROM's driver kept the wrapper and dropped the call:

```
4086d4b0  bclr #11,d6 ; move.l $236,-(sp) ; rts     called from $4086CC98, $4086CCA8 (eject), $4086D144 (ReSeek,
                                                    before every sector's I/O) and $4086EA68; Seek is $4086D4BA
```

Seek still waits out SonyVars+20 for a drive without the 800K's ready line ($4086D512: `move.w 20(a1),d0;
moveq #0,d2; bsr $4086D54A`), the speed-change wait SetSpeed used to put there. The patch (two items in
`iici.json`, "flop:") sends the wrapper to a stub in the zero padding at 0x62AC0:

| Offset | VMA | Old | New |
|---|---|---|---|
| 0x6D4B0 | 0x4086D4B0 | `0886 000B 2F38 0236 4E75` | `4EF9 4086 2AC0` `jmp` the stub, NOPs |
| 0x62AC0 | 0x40862AC0 | 112 x `00` | the wrapper's `bclr #11,d6`; the window at $900000 (24-bit) or $F9000000 (MMU32Bit $CB2 set); if it says 'PSMB' and its word at +$90 says 'SP': the track to +$92, one more seek counted at +$94, and when the zone (track / 16) differs from the one at +$96, that wait (+$98, 100 us units) times the zones moved, at most SonyVars+56 (400 ms), into SonyVars+20 unless a longer one is due; then `move.l JSeek,-(sp); rts` |

The kernel writes 'SP' only on a board whose drive needs it (`mac.board=128k`, `512k`, or `mac.floppyspeed=on`;
psmb.h `struct psmb_floppy`), so on every other board, the IIci testbed and the solo board included, the stub
does the wrapper's two instructions and reads the window once. Checksum 36AC1C28 -> 36BA4653 (the kernel's
startup chime and SCSI-timeout adjustments, psmac_board_se.c, then change it further in memory, as before).

## 3. Patch set `se.json`: the same for the SE ROM B2E362A8

Code 0x40041C-0x4004B2 (docs/01). Two loops (TimeDBRA at 0x40044C..0x40045A, TimeSCCDB at
0x400482..0x400494, the latter also reads the SCC at $9FFFF8), each with the handler/fall-
through label `tst.b $1000(a1) ; not.w d0 ; move.w d0,$D00|$D02`. The SE ROM has no padding
near the code, so the first six bytes of each label become an absolute `jmp` to a stub that
is appended at 0x440000 (the image grows 256 -> 320 KiB; the Pi's ROM window is
$400000-$57FFFF, see docs/00 §2).

| Offset | VMA | Old | New |
|---|---|---|---|
| 0x460 | 0x400460 | `4A29 1000 4640` `tst.b $1000(a1); not.w d0` | `4EF9 0044 0000` `jmp 0x440000` |
| 0x49A | 0x40049A | `4A29 1000 4640` | `4EF9 0044 0010` `jmp 0x440010` |
| 0x40000 | 0x440000 | (appended) | `tst.b $1000(a1); not.w d0; bne.s +2; subq.w #1,d0; jmp 0x400466` |
| 0x40010 | 0x440010 | (appended) | `tst.b $1000(a1); not.w d0; bne.s +2; subq.w #1,d0; jmp 0x4004A0` |

Checksum B2E362A8 -> B2E61B2E, 327680 bytes. Not testable in MAME (`macse` needs the ADB
modem MCU ROM we do not have); verified by disassembly only. The two absolute jumps add
24 cycles (68000) per calibration after the loop has ended, so the stored count is unaffected.

## 4. Verification (MAME 0.264 `maciici`, 8 MB, `images/chd/hd20.chd`, 150 emulated seconds)

Low memory is not identity-mapped on the IIci; `mame/lowmem.lua` finds the page by scanning
physical RAM for ROMBase ($2AE) == 0x40800000 (it is at physical 0x50000 with 8 MB) and
reads the globals from there.

| Run | TimeSCSIDB $B24 | TimeVIADB $CEA | TimeDBRA $D00 | TimeSCCDB $D02 | Boot result (150 s) |
|---|---|---|---|---|---|
| original ROM (`mame/roms`) | 0782 | 030E | 1866 | 0822 | System 6 Finder |
| patched `iici.json` (`mame/roms_patched`) | 0782 | 030E | 1866 | 0822 | System 6 Finder |
| original + T2 = 0xFFFF (test) | 0000 | 0000 | 0000 | 0000 | System 6 Finder (see note) |
| patched + T2 = 0xFFFF (test) | FFFF | FFFF | FFFF | FFFF | System 6 Finder |

Logs: `mame/lowmem_orig.txt`, `mame/lowmem_patched.txt`, `mame/lowmem_test_*.txt` (values
read at frames 600/3000/6000 and at stop; identical each time). The patched ROM stores
exactly the original values in the normal case, as designed.

The two "T2 = 0xFFFF" rows use `rom/patches/iici-test-slowtimer*.json` (`make -C rom test`;
test-only: the calibration timer period is stretched from 0x030C to 0xFFFF ticks, so even
MAME's 25 MHz 68030 runs each loop past 65536 iterations). They exercise the fall-through
path exactly as a very fast CPU does: the unpatched ROM stores 0, the patched ROM stores
0xFFFF. Note: MAME's 5380/VIA/SCC models are not timing-sensitive, so the unpatched ROM
with all-zero counts still boots there; on real hardware zero delays break the SCSI phase
waits. (A mis-linked intermediate build whose stub branched back to `notw` stored
`0xFFFF - k`, E799 for TimeDBRA; the dump catches such mistakes immediately.)

Screenshots: `mame/snap{O,T,TP,TU}/maciici/0014.png` (frame 8400 of 8992). All four are
pixel-identical (0.00 % differing pixels, 640x480 RGB) to
`docs/img/iici-hd20-system6-finder.png`: menu bar, "BlueSCSI Mac Plus" volume icon, Trash.

## 5. Planned ROM patches (from docs/00 §4 and docs/03)

1. **Mailbox driver resources** (IIci: 24-byte resource-list entries, list head via ROM+0x1A
   -> 0x7EB90, each datum preceded by a 12-byte fake 32-bit Memory Manager block header;
   SE: 12-byte ref entries in the classic resource map at 0x41AF20). Appended with
   `append`/`append_asm`, list/map entries patched in place: `DRVR .PiDisk` (block device,
   several drive-queue entries, APM-aware, bootable), `DRVR .ENET` (Ethernet for MacTCP/OT),
   `DRVR .PiCtl` (RTC/time sync, configuration). All talk to the Pi through the mailbox
   window (docs/00 §2, docs/05); no 68k code touches Pi hardware.
2. **Boot hook**: open the new drivers early enough to boot from them. SE: the boot driver
   loop 0x402FEA-0x403114 (`GetResource('DRVR', id)` + DrvrInstall/Open). IIci: the
   corresponding Start Manager driver-open loop still has to be located (docs/03 boot flow
   landmarks: 0x40844xxx SCSI self-test -> Start Manager floppy/SCSI scan, selection routine
   0x40807714); alternative is a `.PiDisk` that installs itself as a SCSI-Manager-visible
   device so the stock boot scan finds it.
3. **Memory sizing**: make the ROM trust the Pi-reported RAM size instead of probing bank A/B
   (IIci: memory test after the MMU table setup at 0x40804B0A; SE: RAM-top probing against
   $400000/$800000, docs/01 hardware table). Needed because Emu68 RAM is not visible on the
   SE bus and the frame buffer must stay inside the write-through window.
4. **Timing, phase 2**: re-point the `TimeDBRA`/`TimeSCSIDB` delay sites (0x40807854,
   0x40807860, 0x40808B9A, 0x408688D8/EE, 0x40875410/26 in the IIci ROM; 0x432510,
   0x433CA0, 0x433CBA in the SE ROM) to a timer-based wait, or have Emu68 stall the
   calibration loops. §2 explains why saturation alone is not sufficient at JIT speed.
5. **MMU shim support (IIci)**: no ROM patch planned; Emu68 traps PMOVE/PFLUSH/PTEST
   (docs/03). If that proves impractical, SwapMMUMode (0x40842C14, 0x40843C76, 0x40843F80)
   and the table setup (0x40804B0A-0x40804C1E) are the patch points.
6. Optional: ROM-disk volume appended to the image (ROM-inator style) for recovery.
