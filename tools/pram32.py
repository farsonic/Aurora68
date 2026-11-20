#!/usr/bin/env python3
"""Switch 32-bit addressing on in the testbed's parameter RAM.

    tools/pram32.py            sets the bits (run between testbed runs; MAME saves the PRAM at exit)
    tools/pram32.py --off      clears them

The Macintosh IIci ROM reads extended PRAM byte $8A at start-up (the code at 0x4080CC60 reads one
byte at $8A with _ReadXPRam and tests bits 0 and 2) and boots in 32-bit addressing mode when
both are set; a fresh PRAM has them clear, which is why System 7.1 in the testbed ran in 24-bit
mode with an 8 MiB ceiling until this was found (docs/08, item 26). The Memory control panel sets
the same bits; this does it without the control panel. MAME keeps the IIci's 256-byte PRAM in
mame/nvramFS/maciici_ps/rtc. The SE's clock chip (343-0042-B) has the same 256 bytes, kept in
mame/nvramFS/macse_ps/rtc for the MAME SE; on the SE board the kernel mends byte $8A as the ROM
reads it (bit 4 cleared, 32-bit on above 8 MiB: docs/08 item 63), so that PRAM needs no edit
unless mac.pram8a=keep is given.
"""
import sys
p = 'mame/nvramFS/maciici_ps/rtc'
d = bytearray(open(p, 'rb').read())
before = d[0x8a]
d[0x8a] = (before & ~0x05) if '--off' in sys.argv else (before | 0x05)
open(p, 'wb').write(d)
print(f"PRAM $8A: {before:#04x} -> {d[0x8a]:#04x} ({'24-bit' if '--off' in sys.argv else '32-bit'} addressing at the next boot)")
