# psmac bus bridge protocol (Emu68 ⇄ MAME "motherboard")

Topology (mirrors the real PiStorm): Emu68 owns CPU, RAM and ROM. Every access the Pi would
put on the 68000 socket pins goes to a *bridge* instead of the CPLD. In the software testbed
the bridge is a MAME `maciici_ps` machine whose CPU is replaced by a `bridge68k` device that
serves bus requests over a UNIX-domain stream socket.

Socket: path from env `PSMAC_BRIDGE_SOCK` (default `/tmp/psmac-bridge.sock`). MAME listens;
the CPU side connects. All integers little-endian (host order on both ends today).

Request, 12 bytes:
| off | size | field | meaning |
|---|---|---|---|
| 0 | u8 | op | 1 READ, 2 WRITE, 3 POLL (no access; reply carries IPL/flags), 4 RESET_ACK |
| 1 | u8 | size | 1, 2 or 4 bytes (READ/WRITE) |
| 2 | u8 | flags | bit0 POSTED (WRITE only: no reply is sent) |
| 3 | u8 | pad | 0 |
| 4 | u32 | addr | 32-bit physical 68k address |
| 8 | u32 | data | WRITE data, right-aligned |

Reply, 8 bytes (for every non-posted request):
| off | size | field | meaning |
|---|---|---|---|
| 0 | u8 | status | 0 OK, 1 BUS ERROR (MAME pulsed M68K_LINE_BUSERROR during the access) |
| 1 | u8 | ipl | current interrupt priority level 0..7 (highest asserted M68K_IRQ_n line) |
| 2 | u8 | flags | bit0 RESET requested by the motherboard, bit1 HALT asserted |
| 3 | u8 | pad | 0 |
| 4 | u32 | data | READ data, right-aligned |

Semantics: READ/WRITE are performed on the bridge CPU's big-endian program space exactly as
the 68030 would (byte lanes, mirrors, side effects). A request issued while the CPU is held
(HALT, e.g. SCSI pseudo-DMA FIFO fill) is served when the hold ends: that is the DTACK wait.
Time: the bridge device charges 4 cycles @ 25 MHz per access and, when idle, lets MAME time
follow wall-clock time so VIA timers, 60 Hz video and ADB keep running.

What Emu68 forwards (IIci map): all of 0x50000000-0x50FFFFFF (I/O), slot space
0xF9000000-0xFEFFFFFF, and *writes* to physical 0x00000000-0x0007FFFF (frame buffer at the
start of bank A; reads are served from Emu68's own RAM). Everything else is local to Emu68.
