#!/usr/bin/env python3
"""Host side of the testbed network service: a tiny Ethernet peer for the Mac.

Emu68's net service appends {u16 length, frame} records to <base>.tx and reads the same records
from <base>.rx (QEMU semihosting, so they are plain files). This helper tails .tx and writes
replies to .rx. Two modes:

  pinet.py <base> peer      built-in peer at 192.168.68.1 / 02:00:00:00:68:01: answers ARP,
                            ICMP echo, UDP echo on port 7 and BOOTP (the Mac gets 192.168.68.2),
                            logs every frame (default)
  pinet.py <base> tap tapN  bridge frames to a Linux TAP device (needs CAP_NET_ADMIN)

Usage in the testbed: tools/run-fullstack.sh 400 mac.net=/tmp/psmac-net   (and run this helper).
"""
import os, sys, time, struct, socket, fcntl

base = sys.argv[1]; mode = sys.argv[2] if len(sys.argv) > 2 else 'peer'
tx_path, rx_path = base + '.tx', base + '.rx'
open(rx_path, 'ab').close(); open(tx_path, 'ab').close()
rx = open(rx_path, 'ab')
PEER_MAC = bytes.fromhex('020000006801'); PEER_IP = bytes([192, 168, 68, 1]); MAC_IP = bytes([192, 168, 68, 2])

def send(frame):
    rx.write(struct.pack('>H', len(frame)) + frame); rx.flush()

def csum(b):
    if len(b) % 2: b += b'\0'
    s = sum(struct.unpack('>%dH' % (len(b) // 2), b))
    while s >> 16: s = (s & 0xffff) + (s >> 16)
    return (~s) & 0xffff

def peer(frame):
    dst, src, etype = frame[0:6], frame[6:12], struct.unpack('>H', frame[12:14])[0]
    if etype == 0x0806 and len(frame) >= 42:                 # ARP
        op = struct.unpack('>H', frame[20:22])[0]; sha, spa, tpa = frame[22:28], frame[28:32], frame[38:42]
        if op == 1 and tpa == PEER_IP:
            arp = struct.pack('>HHBBH', 1, 0x0800, 6, 4, 2) + PEER_MAC + PEER_IP + sha + spa
            send(sha + PEER_MAC + b'\x08\x06' + arp + bytes(18)); return 'ARP reply'
        return 'ARP (ignored)'
    if etype == 0x0800 and len(frame) >= 34:                 # IPv4
        ihl = (frame[14] & 15) * 4; proto = frame[23]; sip, dip = frame[26:30], frame[30:34]
        if dip != PEER_IP and dip != b'\xff\xff\xff\xff': return 'IP not for us'
        payload = frame[14 + ihl:]
        if proto == 1 and payload[0] == 8:                   # ICMP echo
            icmp = b'\x00\x00' + b'\x00\x00' + payload[4:]
            icmp = icmp[:2] + struct.pack('>H', csum(icmp)) + icmp[4:]
            ip = bytearray(frame[14:14 + ihl]); ip[12:16], ip[16:20] = PEER_IP, sip; ip[10:12] = b'\0\0'
            ip[10:12] = struct.pack('>H', csum(bytes(ip)))
            send(src + PEER_MAC + b'\x08\x00' + bytes(ip) + icmp); return 'ICMP echo reply'
        if proto == 17 and len(payload) >= 8:                # UDP echo on port 7, BOOTP on 67
            sport, dport, ulen = struct.unpack('>HHH', payload[0:6])
            if dport == 67 and len(payload) >= 8 + 236 and payload[8] == 1:
                # BOOTP (RFC 951), what MacTCP's "Server" setting asks for: hand the Mac 192.168.68.2,
                # ourselves as the gateway, a /24 mask, through the vendor area's RFC 1048 tags
                req = payload[8:]
                xid, chaddr = req[4:8], req[28:44]
                rep = bytearray(req[:236]); rep[0] = 2                      # BOOTREPLY
                rep[16:20] = MAC_IP                                         # yiaddr
                rep[20:24] = PEER_IP                                        # siaddr
                rep[24:28] = PEER_IP                                        # giaddr: the peer routes
                rep[44:108] = b'\0' * 64; rep[108:236] = b'\0' * 128
                vend = bytes([99, 130, 83, 99]) + bytes([1, 4]) + bytes([255, 255, 255, 0]) + bytes([3, 4]) + PEER_IP + bytes([255])
                body = bytes(rep) + vend + b'\0' * (64 - len(vend))
                udp = struct.pack('>HHHH', 67, 68, 8 + len(body), 0) + body
                ip = bytearray(frame[14:14 + ihl]); ip[12:16], ip[16:20] = PEER_IP, b'\xff\xff\xff\xff'; ip[10:12] = b'\0\0'
                ip[2:4] = struct.pack('>H', ihl + len(udp))
                ip[10:12] = struct.pack('>H', csum(bytes(ip)))
                send(b'\xff' * 6 + PEER_MAC + b'\x08\x00' + bytes(ip) + udp); return 'BOOTP reply 192.168.68.2'
            if dport == 7:
                udp = struct.pack('>HHHH', 7, sport, ulen, 0) + payload[8:]
                ip = bytearray(frame[14:14 + ihl]); ip[12:16], ip[16:20] = PEER_IP, sip; ip[10:12] = b'\0\0'
                ip[10:12] = struct.pack('>H', csum(bytes(ip)))
                send(src + PEER_MAC + b'\x08\x00' + bytes(ip) + udp); return 'UDP echo'
        return f'IP proto {proto}'
    return f'type {etype:04x}'

tap = None
if mode == 'tap':
    TUNSETIFF = 0x400454ca; IFF_TAP = 0x0002; IFF_NO_PI = 0x1000
    tap = os.open('/dev/net/tun', os.O_RDWR | os.O_NONBLOCK)
    fcntl.ioctl(tap, TUNSETIFF, struct.pack('16sH', sys.argv[3].encode(), IFF_TAP | IFF_NO_PI))

off = os.path.getsize(tx_path)
print(f"pinet: {mode} on {base} (tx offset {off})", flush=True)
buf = b''
while True:
    with open(tx_path, 'rb') as f:
        f.seek(off); data = f.read()
    if data:
        off += len(data); buf += data
        while len(buf) >= 2:
            n = struct.unpack('>H', buf[:2])[0]
            if len(buf) < 2 + n: break
            frame = buf[2:2 + n]; buf = buf[2 + n:]
            if tap is not None:
                os.write(tap, frame); what = 'to tap'
            else:
                what = peer(frame)
            print(f"mac -> {frame[0:6].hex(':')} type {frame[12:14].hex()} len {n}: {what}", flush=True)
    if tap is not None:
        try:
            while True:
                frame = os.read(tap, 2048)
                send(frame)
        except BlockingIOError:
            pass
    time.sleep(0.01)
