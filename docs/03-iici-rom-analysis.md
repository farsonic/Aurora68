# Macintosh IIci ROM 368CADFE — what the Pi must provide (donor-ROM path)

Source `rom/original/MacIIci_368CADFE.rom` (512 KiB, md5 aaca403bed2b3870b2327b42d298b5a3).
Checksum verified; the four byte-lane splits match MAME's `maciici` set CRCs exactly
(341-0736/0735, 342-0734/0733), so the dump is byte-perfect.
Disassembly `rom/original/MacIIci.dis` (68030, VMA 0x40800000). Tools: `tools/univres.py`
(Universal ROM resource list), `tools/declrom.py` (declaration ROM), `tools/disrange.py`.

## Why this ROM
32-bit clean in ROM (no MODE32), NCR 5380 SCSI Manager (same chip as the SE → real SCSI
coexists natively), VIA1-based ADB like the SE, and a built-in video driver that already
has **512×342** modes. CPU is detected as 68030; Emu68 presents a 68040 (see MMU below).

## Hardware the ROM expects (DecoderInfo table, ROM+0x3494, "MDU" decoder)
| Device | 32-bit phys | 24-bit logical | Pi strategy on an SE |
|---|---|---|---|
| RAM bank A / B | 0x00000000 / 0x04000000 | 0x000000-0x7FFFFF | Pi RAM; frame buffer pages write-through to SE DRAM |
| ROM | 0x40800000 (mirrors 0x40000000-0x4FFFFFFF) | 0x800000 | Pi memory (our patched image) |
| VIA1 | 0x50F00000 (regs at +n*0x200) | 0xF00000 | Pi-emulated VIA1 front end; Pi drives the real SE VIA for ADB/RTC/sound/video bits |
| VIA2 | 0x50F02000 (absent on IIci; probe only) | | return open-bus / not present |
| SCC | 0x50F04000 | 0xF04000 | forward to SE SCC ($9FFFF8 rd / $BFFFF9 wr) with register translation |
| SCSI 5380 | 0x50F10000 regs (+reg*16), pDMA 0x50F12000, handshake 0x50F06000 | 0xF10000 | forward to SE 5380 at $5FF000/$5FF001 (+reg*16), pDMA/handshake to $5FF200 |
| ASC sound | 0x50F14000 | 0xF14000 | Pi-emulated ASC (FIFO mode 22.25 kHz) → SE sound buffer |
| SWIM floppy | 0x50F16000 | 0xF16000 | forward to SE IWM ($DFE1FF); IWM-only SEs may lose 1.44 MB modes |
| VDAC | 0x50F24000 | | Pi-emulated (palette; ignore for 1-bit) |
| RBV | 0x50F26000 (VIA2-compatible IFR/IER + video control + monitor sense) | 0xF26000 | Pi-emulated: interrupts, slot-E video, sense bits |
| Slots | 0xF9000000-0xFEFFFFFF (24-bit 0x900000-0xEFFFFF) | | empty; optional pseudo-slot for our devices |
Note: the IIci decoder ignores A20-A23 in the I/O block, so 0x500xxxxx..0x50Fxxxxx alias.
The ROM also probes 0x50F21C00 / 0x50F02000 at boot to tell decoders apart (must read as
"not there" for MDU detection; MAME returns open bus).

## MMU usage (the shim Emu68 needs)
Only 22 `pmove` (TC/CRP), 3 `pflush(a)`, 1 `ptest`; plus 103 `movec` (CACR/VBR/USP only).
- Boot: `pmove (a0),crp / pmove 8(a0),tc` at 0x40800076-7A, tables built around
  0x40804B0A-0x40804C1E (MMU32bit flag $CB2, table pointers $CB4/$CB8, MMUType $CB1).
- SwapMMUMode: `pmove tc,-(sp); bclr #31; pmove (sp)+,tc` (0x40842C14, 0x40843C76,
  0x40843F80) → toggles translation enable; 24/32-bit mode switch = swapping CRP/TC
  between the two prebuilt tables (0x40804BB0 / 0x40804BFC).
- Plan: Emu68 fork traps PMOVE/PFLUSH/PTEST (F-line, coprocessor 0), keeps shadow TC/CRP,
  walks the 68030 page tables once per TC/CRP load and re-programs its own 68k→Pi mapping;
  TC bit 31 clear = identity. No VM support (System 7 Virtual Memory must stay off).
- CPU type: ROM sets CPUFlag from its own probes; Emu68 must behave as 68030 for these
  (full 68882 FPU instruction set is already provided by Emu68, so SANE/FPU paths are fine).

## Resource list (51 resources, `tools/univres.py`)
Linked list via ROM+0x1A → 0x7EB90 (last entry), 24-byte entries, 12-byte fake 32-bit
Memory Manager block headers. DRVRs: .Sony 4, .Sound 3, .MPP 9 / .ATP 10 (two sets,
flags 20000000 vs 40000000 = AppleTalk v1/v2 selection), .XPP 40, SERD 0/60/61.
Also 'iopc' (IOP code for IIfx — unused on IIci), gama/clut/cctb/wctb (Color QD).
Appending our ROM DRVRs = add 24-byte entries to the list (we own the ROM image).

## Built-in video declaration ROM (ROM+0x7EC1A..0x7FFFF, parsed by `tools/declrom.py`)
Board sRsrc 1 "Macintosh II Built-In Video"; functional sRsrcs 'Display_Video_Apple_RBV1'
numbered **0x80 + sense code** (table at ROM+0x7EEDC): 0x81 portrait 640×870, 0x82 12" RGB
512×384, 0x86 13" RGB 640×480, **0x87 = 512×342 (sense 7), modes 80..83 = 1/2/4/8 bpp,
rowBytes 64/128/256/512**. Variants 0x8x/0x9x restrict depths by available RAM.
=> Pi presents RBV monitor sense 7 and the stock driver runs 512×342. In 1-bpp the frame
buffer layout equals the SE screen (64 bytes/row) → straight write-through; 8-bpp grey
can later be dithered by the Pi (as Frimples does). Frame buffer lives in bank A RAM at a
base the driver sets via RBV; Pi marks those pages write-through.

## Timing
TimeDBRA/TimeSCCDB/TimeSCSIDB ($D00/$D02/$B24) are calibrated with dbf loops
(TimeSCSIDB loop at 0x408008A4 reads the 5380 status register) and used in the SCSI
Manager (e.g. 0x40807854 `TimeDBRA>>7`, 0x40807860 `mulu TimeSCSIDB`). Same overflow
hazard as the SE ROM at JIT speed → patch or clamp (Emu68 can also slow these loops by
making the calibration run in interpreter mode).

## Boot flow landmarks
0x4080008C start → 0x40802E00/2F18 decoder detection (presence checks 0x4080306C..) →
UnivInfoPtr $DD8 / BoxFlag $CB3 (0x4080016A) → MMU tables (0x40804B0A) → memory test
→ 0x40844xxx SCSI self-test (RST/ACK/BSY/SEL/ATN loopback on the 5380 at 0x40844FD2) →
Start Manager scans floppy and SCSI; selection routine 0x40807714 (ICR=04, ODR=IDs,
ICR=0D, release BSY, wait BSY at 0x40807860 with TimeSCSIDB timeout).
