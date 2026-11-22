#!/usr/bin/env python3
"""Configure a MacTCP 2.0.x control panel file without the control panel.

    mactcpcfg.py MacTCP.bin --ip 192.168.68.2 --mask 255.255.255.0 --gateway 192.168.68.1 -o MacTCP-cfg.bin
    mactcpcfg.py MacTCP.bin --server -o MacTCP-cfg.bin            # "Obtain address: Server" (BOOTP)
    mactcpcfg.py MacTCP.bin --show

MacTCP keeps its configuration in a 129-byte resource, 'ipln' 128, inside the control panel
(and a copy in "MacTCP Prep"), with a layout Apple never published. It was read out of the
control panel's own code and the .IPP driver on 16 November 2026 for the Aurora68 project,
because the alternative, driving the control panel's dialogs inside an emulator, was worse:
  +0   long   version, 1 (the driver takes the first 'ipln' whose version is 1)
  +4   long   flags: 0x800 "Manually", 0x348 "Server" (BOOTP/RARP), 0x400 "Dynamically"
  +8   long   IP address
  +12  long   subnet mask
  +16  long   host mask (the complement of the subnet mask)
  +20  long   gateway ("Routing information")
  +40  long   } net and subnet numbers as the control panel shows them; left alone here
  +44  long   }
  +48  str    Pascal string: the interface's name in the control panel's list, "*" = the default
  +83  byte   a checkbox of the "More" dialog, left alone
  +88  long   the link-layer code resource ('iplp') to load: 128 LocalTalk, 129 Ethernet
  +92  long   for the Ethernet link layer: the NuBus slot to use, 0 = the first Ethernet card
  +96  str    Pascal string: a file name when the link layer lives in another file, else empty
The file is MacBinary (hcopy -m); the resource is patched in place, so sizes and the header
CRC do not change.
"""
import argparse, struct, sys

ap = argparse.ArgumentParser()
ap.add_argument('file'); ap.add_argument('-o'); ap.add_argument('--show', action='store_true')
ap.add_argument('--ip'); ap.add_argument('--mask', default='255.255.255.0'); ap.add_argument('--gateway')
ap.add_argument('--server', action='store_true', help='obtain the address from a BOOTP server')
ap.add_argument('--slot', type=int, default=0, help='NuBus slot of the Ethernet card, 0 = first found')
ap.add_argument('--localtalk', action='store_true', help='use the LocalTalk link layer instead of Ethernet')
a = ap.parse_args()

blob = bytearray(open(a.file, 'rb').read())
dlen, rlen = struct.unpack('>II', blob[83:91]); start = 128 + ((dlen + 127) & ~127)
fork = blob[start:start + rlen]
doff, moff, _, mlen = struct.unpack('>IIII', fork[:16]); m = fork[moff:moff + mlen]
tloff, nloff, ntypes = struct.unpack('>HHH', m[24:30]); ntypes = (ntypes + 1) & 0xffff
pos = None
for i in range(ntypes):
    rtype, cnt, rloff = struct.unpack('>4sHH', m[tloff + 2 + i * 8:tloff + 10 + i * 8])
    for j in range(cnt + 1):
        e = m[tloff + rloff + j * 12:tloff + rloff + j * 12 + 12]
        rid, _, attr_off = struct.unpack('>hHI', e[:8])
        if rtype == b'ipln' and rid == 128:
            off = attr_off & 0xffffff
            size = struct.unpack('>I', fork[doff + off:doff + off + 4])[0]
            pos = start + doff + off + 4
            if size < 100: sys.exit(f"'ipln' 128 is {size} bytes: not the layout this tool knows")
if pos is None: sys.exit("no 'ipln' 128 resource: is this MacTCP's control panel?")

def ip(s): return bytes(int(x) for x in s.split('.'))
def show():
    c = blob[pos:pos + 129]
    f, addr, mask, hmask, gw = struct.unpack('>IIIII', c[4:24])
    lap, slot = struct.unpack('>II', c[88:96])
    name = c[49:49 + c[48]].decode('mac_roman', 'replace')
    mode = {0x800: 'Manually', 0x348: 'Server', 0x400: 'Dynamically'}.get(f & 0xfc8, f'flags {f:#x}')
    print(f"address {mode}: {'.'.join(map(str, c[8:12]))} mask {'.'.join(map(str, c[12:16]))} gateway {'.'.join(map(str, c[20:24]))}")
    print(f"link layer 'iplp' {lap} ({'Ethernet' if lap == 129 else 'LocalTalk' if lap == 128 else '?'}), slot {slot}, interface name {name!r}")

if a.show:
    show(); sys.exit(0)
c = blob[pos:pos + 129]
flags = struct.unpack('>I', c[4:8])[0] & ~0xfc8
if a.server:
    flags |= 0x348
else:
    if not a.ip: sys.exit("--ip (or --server) is needed")
    flags |= 0x800
    mask = ip(a.mask); hmask = bytes(255 - b for b in mask)
    c[8:12] = ip(a.ip); c[12:16] = mask; c[16:20] = hmask
    c[20:24] = ip(a.gateway) if a.gateway else b'\0\0\0\0'
c[4:8] = struct.pack('>I', flags)
c[88:92] = struct.pack('>I', 128 if a.localtalk else 129)
c[92:96] = struct.pack('>I', a.slot)
c[48] = 1; c[49] = ord('*')
blob[pos:pos + 129] = c
open(a.o or a.file, 'wb').write(blob)
show()
