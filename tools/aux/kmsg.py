#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# kmsg.py DUMP [TAIL]: the A/UX 3.1.1 kernel's message buffer in a RAM dump (VA $120199F4, index at VA $11005B1C, as found
# under the standalone loader's tables: physical $1189F4 and $F2B1C)
import sys, struct
d = open(sys.argv[1], 'rb').read()
n = struct.unpack('>I', d[0xf2b1c:0xf2b20])[0]
txt = d[0x1189f4:0x1189f4 + min(n, 10000)].decode('latin1')
t = int(sys.argv[2]) if len(sys.argv) > 2 else 4000
print('index', n); print(txt[-t:])
