#!/usr/bin/env python3
"""Switch 32-bit addressing on in the testbed's parameter RAM.

    tools/pram32.py            sets the bits (run between testbed runs; MAME saves the PRAM at exit)
    tools/pram32.py --off      clears them

The Macintosh IIci ROM reads extended PRAM byte $8A at start-up (the code at 0x4080CC60 reads one
byte at $8A with _ReadXPRam and tests bits 0 and 2) and boots in 32-bit addressing mode when
both are set; a fresh PRAM has them clear, which is why System 7.1 in the testbed ran in 24-bit
mode with an 8 MiB ceiling until this was found (docs/08, item 26). The Memory control panel sets
the same bits; this does it without the control panel. MAME keeps the IIci's 256-byte PRAM in
mame/nvramFS/maciici_ps/rtc. On the real SE the RTC chip has 20 bytes of PRAM and no byte $8A:
the kernel will have to keep the extended PRAM itself (a note for the hardware stage).
"""
import sys
p = 'mame/nvramFS/maciici_ps/rtc'
d = bytearray(open(p, 'rb').read())
before = d[0x8a]
d[0x8a] = (before & ~0x05) if '--off' in sys.argv else (before | 0x05)
open(p, 'wb').write(d)
print(f"PRAM $8A: {before:#04x} -> {d[0x8a]:#04x} ({'24-bit' if '--off' in sys.argv else '32-bit'} addressing at the next boot)")
