#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""mknetimage.py SYSINITRC OUT [--addr A.B.C.D] [--mask M] [--peer P] [--name HOST] - the testbed's /etc/sysinitrc
for an A/UX 3.1.1 disk image copy: A/UX builds its kernel with the network driver and tests the network by itself.

The testbed has no keyboard for A/UX, so what a person would type is put where A/UX runs it anyway (docs/24
section 7). SYSINITRC is the image's own /etc/sysinitrc (ufscat.py IMAGE 96 /etc/sysinitrc); OUT is the new one,
to be written back with ufspatch.py, which can only use the fragments the file already has: the comment lines
are dropped to make room. What is added:

  - around the script's own `autoconfig -a` line, a line in /auxnet.boot for each start and autoconfig's result;
  - at the end, once (until /auxnet.done exists): `newconfig -v` with the answers to the driver's questions on its
    standard input (yes, the host name, no NIS domain, the address, the netmask, yes), its transcript in
    /auxnet.newconfig, then `reboot`;
  - on every later start, in the background: etheraddr, ifconfig ae0, netstat, three pings of the peer, the ARP
    table, a telnet to the peer's echo port, into /auxnet.log.

Read the three files off the image afterwards with ufscat.py. The defaults are the testbed peer's network
(tools/pinet.py: the peer is 192.168.68.1).
"""
import argparse
ap = argparse.ArgumentParser()
ap.add_argument('sysinitrc'); ap.add_argument('out')
ap.add_argument('--addr', default='192.168.68.2'); ap.add_argument('--mask', default='255.255.255.0')
ap.add_argument('--peer', default='192.168.68.1'); ap.add_argument('--name', default='aurora')
a = ap.parse_args()
src = open(a.sysinitrc).read()
lines = [l for i, l in enumerate(src.split('\n')) if not (i > 0 and l.lstrip().startswith('#')) and l.strip() != '']
t = '\n'.join(lines) + '\n'
auto = '/etc/autoconfig -t0 -a -o /unix -S /etc/startup -M /etc/master # system configuration\n'
if auto not in t:
    raise SystemExit('this /etc/sysinitrc has no autoconfig line as A/UX 3.1.1 has it')
t = t.replace(auto, 'echo "boot `/bin/date`" >> /auxnet.boot; sync\n'
                    '/etc/autoconfig -t0 -a -o /unix -S /etc/startup -M /etc/master\n'
                    'echo "autoconfig $?" >> /auxnet.boot; sync\n')
t += f'''if [ ! -f /auxnet.done ]; then
(echo y; echo {a.name}; echo; echo {a.addr}; echo {a.mask}; echo y) | /etc/newconfig -v > /auxnet.newconfig 2>&1
echo "newconfig $? `/bin/date`" >> /auxnet.boot
> /auxnet.done
sync; sync
/etc/reboot
fi
(
echo "auxnet `/bin/date`"
/etc/etheraddr
/etc/ifconfig ae0
/usr/bin/netstat -in
sync
/usr/etc/ping {a.peer} 56 3
echo "ping $?"
/etc/arp -a
sync
(sleep 4; echo aurora68; sleep 6) | /usr/bin/telnet {a.peer} 7
echo "telnet $?"
/usr/bin/netstat -in
sync
) > /auxnet.log 2>&1 &
'''
open(a.out, 'w').write(t)
print(f'{a.out}: {len(t)} bytes (the original has {len(src)})')
