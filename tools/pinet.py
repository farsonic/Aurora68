#!/usr/bin/env python3
"""Host side of the testbed network service: a tiny Ethernet peer for the Mac.

Emu68's net service appends {u16 length, frame} records to <base>.tx and reads the same records
from <base>.rx (QEMU semihosting, so they are plain files). This helper tails .tx and writes
replies to .rx. Two modes:

  pinet.py <base> peer      built-in peer at 192.168.68.1 / 02:00:00:00:68:01: answers ARP,
                            ICMP echo, UDP echo on port 7, BOOTP and DHCP (the Mac gets
                            192.168.68.2), and IPv6: neighbour discovery for fe80::ff:fe00:6801
                            and fd68::1, a router advertisement of fd68::/64, ICMPv6 echo; each
                            address the Mac checks for duplicates (DAD) is pinged a second later
                            and the answer logged. Logs every frame (default)
  pinet.py <base> tap tapN  bridge frames to a Linux TAP device (needs CAP_NET_ADMIN)

Usage in the testbed: tools/run-fullstack.sh 400 mac.net=/tmp/psmac-net   (and run this helper).
"""
import os, sys, time, struct, socket, fcntl

base = sys.argv[1]; mode = sys.argv[2] if len(sys.argv) > 2 else 'peer'
tx_path, rx_path = base + '.tx', base + '.rx'
open(rx_path, 'ab').close(); open(tx_path, 'ab').close()
rx = open(rx_path, 'ab')
PEER_MAC = bytes.fromhex('020000006801'); PEER_IP = bytes([192, 168, 68, 1]); MAC_IP = bytes([192, 168, 68, 2])
PEER_LL6 = bytes.fromhex('fe80000000000000000000fffe006801')      # EUI-64 of PEER_MAC
PEER_GL6 = bytes.fromhex('fd680000000000000000000000000001')
PREFIX6 = bytes.fromhex('fd680000000000000000000000000000')
ALLNODES6 = bytes.fromhex('ff020000000000000000000000000001')
pending_pings = []          # (time, mac, address): the Mac's addresses to ping after their DAD
ping6_seq = 0

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
                # DHCP (RFC 2131) when the request carries a message type: DISCOVER gets an OFFER,
                # REQUEST an ACK, with the server, a lease, the mask, the router and a name server
                mtype, k = 0, 240
                if req[236:240] == bytes([99, 130, 83, 99]):
                    while k + 1 < len(req) and req[k] != 255:
                        if req[k] == 0: k += 1; continue
                        if req[k] == 53: mtype = req[k + 2]
                        k += 2 + req[k + 1]
                vend = bytes([99, 130, 83, 99])
                if mtype:
                    vend += bytes([53, 1, 2 if mtype == 1 else 5]) + bytes([54, 4]) + PEER_IP + bytes([51, 4]) + struct.pack('>I', 3600)
                    vend += bytes([6, 4]) + PEER_IP + bytes([15, 7]) + b'testbed'
                vend += bytes([1, 4]) + bytes([255, 255, 255, 0]) + bytes([3, 4]) + PEER_IP + bytes([255])
                body = bytes(rep) + vend + b'\0' * max(0, 64 - len(vend))
                udp = struct.pack('>HHHH', 67, 68, 8 + len(body), 0) + body
                ip = bytearray(frame[14:14 + ihl]); ip[12:16], ip[16:20] = PEER_IP, b'\xff\xff\xff\xff'; ip[10:12] = b'\0\0'
                ip[2:4] = struct.pack('>H', ihl + len(udp))
                ip[10:12] = struct.pack('>H', csum(bytes(ip)))
                send(b'\xff' * 6 + PEER_MAC + b'\x08\x00' + bytes(ip) + udp)
                return {0: 'BOOTP reply', 1: 'DHCP OFFER', 3: 'DHCP ACK'}.get(mtype, 'DHCP type %d answered' % mtype) + ' 192.168.68.2'
            if dport == 7:
                udp = struct.pack('>HHHH', 7, sport, ulen, 0) + payload[8:]
                ip = bytearray(frame[14:14 + ihl]); ip[12:16], ip[16:20] = PEER_IP, sip; ip[10:12] = b'\0\0'
                ip[10:12] = struct.pack('>H', csum(bytes(ip)))
                send(src + PEER_MAC + b'\x08\x00' + bytes(ip) + udp); return 'UDP echo'
        return f'IP proto {proto}'
    if etype == 0x86dd and len(frame) >= 54:                 # IPv6
        return peer6(frame, src)
    return f'type {etype:04x}'

def ip6_addr(a):
    return socket.inet_ntop(socket.AF_INET6, a)

def icmp6(src6, dst6, body):
    # body with a zero checksum; the pseudo header: addresses, length, next header 58
    pseudo = src6 + dst6 + struct.pack('>I', len(body)) + b'\0\0\0\x3a'
    c = csum(pseudo + body)
    return body[:2] + struct.pack('>H', c) + body[4:]

def send6(dstmac, src6, dst6, body):
    pkt = icmp6(src6, dst6, body)
    ip6 = struct.pack('>IHBB', 0x60000000, len(pkt), 58, 255) + src6 + dst6
    send(dstmac + PEER_MAC + b'\x86\xdd' + ip6 + pkt)

def mcast_mac(a6):
    return b'\x33\x33' + a6[12:16]

def peer6(frame, src):
    global ping6_seq
    ip6 = frame[14:54]; nh = ip6[6]; src6, dst6 = ip6[8:24], ip6[24:40]; pl = frame[54:]
    if nh != 58 or len(pl) < 4:
        return f'IPv6 next header {nh}'
    t = pl[0]
    me = PEER_GL6 if dst6[:2] == b'\xfd\x68' else PEER_LL6
    if t == 133:                                             # router solicitation -> advertisement
        body = struct.pack('>BBHBBHII', 134, 0, 0, 64, 0, 1800, 0, 0)
        body += bytes([1, 1]) + PEER_MAC                                   # source link-layer address
        body += bytes([3, 4, 64, 0xC0]) + struct.pack('>III', 86400, 14400, 0) + PREFIX6
        body += bytes([5, 1, 0, 0]) + struct.pack('>I', 1500)               # MTU
        send6(mcast_mac(ALLNODES6), PEER_LL6, ALLNODES6, body)
        return 'IPv6 RS: router advertisement fd68::/64'
    if t == 135 and len(pl) >= 24:                           # neighbour solicitation
        target = pl[8:24]
        if src6 == bytes(16):
            pending_pings.append((time.time() + 1.5, src, target))
            return 'IPv6 DAD for ' + ip6_addr(target)
        if target in (PEER_LL6, PEER_GL6):
            body = struct.pack('>BBHI', 136, 0, 0, 0xE0000000) + target + bytes([2, 1]) + PEER_MAC
            send6(src, target, src6, body)
            return 'IPv6 NS: advertisement for ' + ip6_addr(target)
        return 'IPv6 NS for ' + ip6_addr(target)
    if t == 128:                                             # echo request -> reply
        body = bytes([129, 0, 0, 0]) + pl[4:]
        send6(src, dst6 if dst6[0] != 0xff else me, src6, body)
        return 'IPv6 echo reply to ' + ip6_addr(src6)
    if t == 129:
        return 'IPv6 ECHO REPLY from ' + ip6_addr(src6) + ' (the Mac answers on IPv6)'
    if t == 136:
        return 'IPv6 NA for ' + ip6_addr(pl[8:24])
    return f'ICMPv6 type {t}'

def ping6_due():
    global ping6_seq
    now = time.time()
    for p in list(pending_pings):
        if p[0] <= now:
            pending_pings.remove(p)
            _, mac, a6 = p
            ping6_seq += 1
            src6 = PEER_GL6 if a6[:2] == b'\xfd\x68' else PEER_LL6
            body = struct.pack('>BBHHH', 128, 0, 0, 0x6801, ping6_seq) + b'aurora68'
            send6(mac, src6, a6, body)
            print(f"peer -> ping6 {ip6_addr(a6)}", flush=True)

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
    if tap is None:
        ping6_due()
    if tap is not None:
        try:
            while True:
                frame = os.read(tap, 2048)
                send(frame)
        except BlockingIOError:
            pass
    time.sleep(0.01)
