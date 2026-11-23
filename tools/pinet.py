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
                            and the answer logged; a TCP echo server on port 7, an HTTP/1.0
                            server on port 80 (/ a page, /big 256 KB, /slow the page in
                            pieces a second apart, /reset a reset after the headers, /idle
                            the page, then the server's close after 30 idle seconds), and a DNS
                            server on port 53 (peer.testbed and anything.testbed is
                            192.168.68.1, and back; peer.testbed is also fd68::1, and
                            v6only.testbed is fd68::80 with no IPv4 address, where HTTP
                            answers over IPv6; silent.testbed and 192.168.68.66 are never
                            answered, other addresses have no name). Logs every frame (default).
                            PINET_MDNS=NAME: every ten seconds an mDNS query for NAME.local
                            (224.0.0.251:5353, from the Mac's own subnet so the responder does
                            not ignore it as off-link); answers are logged as "mDNS answer".
                            PINET_FORWARD="445=127.0.0.1:4450,548=127.0.0.1:5480": TCP to those
                            ports of the peer is relayed to the host addresses (the Mac's file
                            clients, Lagoon, reach servers on the host: Samba, Netatalk), in
                            order, within the Mac's receive window; the testbed loses nothing,
                            so nothing is sent again. Over IPv6 too (to fd68::1).
                            PINET_UDP="8070=127.0.0.1:18470": UDP to that port, sent to the peer,
                            a broadcast or a multicast group, goes to the host address and its
                            answers come back from the peer (Aurora68 Play's music server probe).
                            PINET_MDNS_RELAY=127.0.0.1:PORT: the Mac's mDNS queries (224.0.0.251:5353)
                            go to that responder on the host (Lagoon's tests/servers/mdns/
                            responder.py --addr 192.168.68.1) and its answers come back to the Mac
                            from the peer, as a legacy unicast answer (RFC 6762 6.7).
                            PINET_ATALK=NAME: an AppleTalk node too (Phase 2, node 104 on the
                            Mac's network): AARP, NBP lookups for NAME:AFPServer, ASP GetStatus,
                            so the Chooser's AppleShare list shows NAME. AppleTalk frames are
                            decoded in the log either way.
                            The kernel's own address (the remote control, docs/13): a DHCP client
                            whose host name starts with "aurora68" gets 192.168.N.3, everyone else
                            192.168.N.2 as before. PINET_KERNEL=PORT: the host reaches the kernel's
                            HTTP server there: a connection to 127.0.0.1:PORT becomes a TCP connection
                            from the peer to the kernel's address on port 80 (PINET_KERNEL_TO=8068
                            for the other port; PINET_KERNEL_IP=A.B.C.D for a static address).
                            PINET_APCLIENT=1 (docs/08 item 117): the peer is a phone that joined the
                            kernel's own access point (Setup and Recovery with mac.wifisim=): it serves
                            no DHCP, it asks the kernel's DHCP server for an address (DISCOVER every two
                            seconds until it has one), takes it as its own, asks the kernel's DNS for a
                            phone's captive-portal check names, and logs every answer; PINET_KERNEL then
                            reaches the page from the leased address (PINET_KERNEL_IP=192.168.4.1).
  pinet.py <base> tap tapN  bridge frames to a Linux TAP device (needs CAP_NET_ADMIN)

Usage in the testbed: tools/run-fullstack.sh 400 mac.net=/tmp/psmac-net   (and run this helper).
"""
import os, sys, time, struct, socket, fcntl

base = sys.argv[1]; mode = sys.argv[2] if len(sys.argv) > 2 else 'peer'
tx_path, rx_path = base + '.tx', base + '.rx'
open(rx_path, 'ab').close(); open(tx_path, 'ab').close()
rx = open(rx_path, 'ab')
# PINET_NET=N: the peer is 192.168.N.1 (02:00:00:00:N:01 in hex digits) and hands out 192.168.N.2, so
# two peers (the wired file pair and the simulated WiFi's) can be two networks; default 68
NET = int(os.environ.get('PINET_NET', '68'))
PEER_MAC = bytes.fromhex('0200000068%02x' % (NET - 67)) if NET != 68 else bytes.fromhex('020000006801')
PEER_IP = bytes([192, 168, NET, 1]); MAC_IP = bytes([192, 168, NET, 2])
PEER_LL6 = bytes.fromhex('fe80000000000000000000fffe006801')      # EUI-64 of PEER_MAC
PEER_GL6 = bytes.fromhex('fd680000000000000000000000000001')
PREFIX6 = bytes.fromhex('fd680000000000000000000000000000')
V6ONLY6 = bytes.fromhex('fd680000000000000000000000000080')   # v6only.testbed: IPv6 only (HTTP on 80)
ALLNODES6 = bytes.fromhex('ff020000000000000000000000000001')
pending_pings = []          # (time, mac, address): the Mac's addresses to ping after their DAD
ping6_seq = 0

# PINET_LOSS=P (testbed): P per cent of the TCP segments each way are lost, as on a busy WiFi network
# (21 November 2026: an upload to the Mac over the owner's WiFi stalled, docs/08 item 95)
import random
LOSS = float(os.environ.get('PINET_LOSS', '0') or 0) / 100.0
def lost(frame):
    return LOSS > 0 and len(frame) >= 34 and frame[12:14] == b'\x08\x00' and frame[23] == 6 and random.random() < LOSS

def send(frame):
    if lost(frame): return
    rx.write(struct.pack('>H', len(frame)) + frame); rx.flush()

def csum(b):
    if len(b) % 2: b += b'\0'
    s = sum(struct.unpack('>%dH' % (len(b) // 2), b))
    while s >> 16: s = (s & 0xffff) + (s >> 16)
    return (~s) & 0xffff

MDNS_NAME = os.environ.get('PINET_MDNS')
mac_ip4 = None
mdns_next = 0
def dns_names(m, p):
    out = []; jumped = False; end = None
    while p < len(m):
        n = m[p]
        if n == 0: p += 1; break
        if n & 0xc0 == 0xc0:
            if end is None: end = p + 2
            p = ((n & 0x3f) << 8) | m[p + 1]; continue
        out.append(m[p + 1:p + 1 + n].decode(errors='replace')); p += 1 + n
    return '.'.join(out), (end if end is not None else p)
def mdns_answer(src, sip, payload):
    m = payload[8:]
    if len(m) < 12: return 'mDNS (short)'
    qd, an = struct.unpack('>HH', m[4:8]); p = 12
    for _ in range(qd):
        _, p = dns_names(m, p); p += 4
    got = []
    for _ in range(an):
        name, p = dns_names(m, p)
        t, c, ttl, l = struct.unpack('>HHIH', m[p:p + 10]); p += 10; d = m[p:p + l]; p += l
        if t == 1 and l == 4: got.append(f"{name} A {'.'.join(map(str, d))}")
        elif t == 28 and l == 16: got.append(f"{name} AAAA {ip6_addr(d)}")
        elif t == 12: got.append(f"{name} PTR {dns_names(m, p - l)[0]}")
        else: got.append(f"{name} type {t}")
    if got: print("mDNS answer from " + '.'.join(map(str, sip)) + ": " + '; '.join(got), flush=True)
    return f'mDNS ({an} answers)'
def mdns_due():
    global mdns_next
    if not MDNS_NAME or time.time() < mdns_next: return
    mdns_next = time.time() + 10
    src_ip = (mac_ip4[:3] + b'\x01') if mac_ip4 else PEER_IP
    q = struct.pack('>HHHHHH', 0, 0, 1, 0, 0, 0)
    for lab in (MDNS_NAME + '.local').split('.'): q += bytes([len(lab)]) + lab.encode()
    q += b'\0' + struct.pack('>HH', 1, 1)
    udp = struct.pack('>HHHH', 5353, 5353, 8 + len(q), 0) + q
    ip = bytearray(struct.pack('>BBHHHBBH', 0x45, 0, 20 + len(udp), 0, 0, 255, 17, 0) + src_ip + bytes([224, 0, 0, 251]))
    ip[10:12] = struct.pack('>H', csum(bytes(ip)))
    send(bytes.fromhex('01005e0000fb') + PEER_MAC + b'\x08\x00' + bytes(ip) + udp)
    print(f"peer -> mDNS query {MDNS_NAME}.local from {'.'.join(map(str, src_ip))}", flush=True)

MDNS_RELAY = None
if os.environ.get('PINET_MDNS_RELAY'):
    h, p_ = os.environ['PINET_MDNS_RELAY'].rsplit(':', 1)
    MDNS_RELAY = (h, int(p_))
def mdns_relay(src, sip, udp):
    sport = struct.unpack('>H', udp[0:2])[0]
    q = udp[8:]
    if len(q) < 12 or q[2] & 0x80:
        return None                                  # an answer, not a query
    s_ = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s_.settimeout(0.5)
    try:
        s_.sendto(q, MDNS_RELAY)
        ans, _ = s_.recvfrom(9000)
    except OSError:
        return 'mDNS query: no answer from the relay'
    finally:
        s_.close()
    out = struct.pack('>HHHH', 5353, sport, 8 + len(ans), 0) + ans
    send4(src, sip, 17, out)
    return f'mDNS query relayed: {len(ans)}-byte answer to port {sport}'

# AppleTalk Phase 2 (Inside AppleTalk, 2nd ed., ch. 2): 802.3 frames whose 802.2 SNAP header names
# AARP (00 00 00 80 F3) or DDP (08 00 07 80 9B); logged, so a connection file's probes and an NBP
# lookup from the Chooser can be seen on the wire
def atalk_addr(b):
    return f"{(b[1] << 8) | b[2]}.{b[3]}"
NBP_FN = {1: 'BrRq', 2: 'LkUp', 3: 'LkUp-Reply', 4: 'FwdReq'}
def nbp_tuple(d, p):
    p += 5                                           # network, node, socket, enumerator
    out = []
    for _ in range(3):
        n = d[p]; out.append(d[p + 1:p + 1 + n].decode('mac_roman', 'replace')); p += 1 + n
    return f"{out[0]}:{out[1]}@{out[2]}", p
def atalk(frame):
    llc = frame[14:22]
    if llc[:3] != b'\xaa\xaa\x03':
        return None
    if llc[3:8] == bytes.fromhex('00000080f3'):
        a = frame[22:]
        if len(a) < 28: return 'AARP (short)'
        fn = struct.unpack('>H', a[6:8])[0]
        what = {1: 'request', 2: 'response', 3: 'probe'}.get(fn, f'function {fn}')
        return f"AARP {what} from {atalk_addr(a[14:18])} ({a[8:14].hex(':')}) for {atalk_addr(a[24:28])}"
    if llc[3:8] == bytes.fromhex('080007809b'):
        d = frame[22:]
        if len(d) < 13: return 'DDP (short)'
        ln = struct.unpack('>H', d[0:2])[0] & 0x3ff
        dn, sn = struct.unpack('>HH', d[4:8])
        s = f"DDP {sn}.{d[9]}:{d[11]} -> {dn}.{d[8]}:{d[10]} type {d[12]} len {ln}"
        if d[12] == 2 and ln > 13:                   # NBP
            fn, cnt = d[13] >> 4, d[13] & 15
            s += f": NBP {NBP_FN.get(fn, fn)} id {d[14]}"
            p = 15
            for _ in range(cnt):
                try:
                    t, p = nbp_tuple(d, p); s += f" '{t}'"
                except IndexError:
                    break
        elif d[12] == 6:
            s += ': ZIP'
        elif d[12] == 1:
            s += ': RTMP'
        return s
    return 'SNAP ' + llc[3:8].hex()

# PINET_ATALK=NAME: a small AppleTalk node on the wire, enough for a Mac's Chooser to list an
# AppleShare server called NAME. Its address is node 104 on whatever network the Mac uses (learnt
# from the Mac's first datagram; until then nothing is answered). It answers AARP requests and
# probes for that address, NBP lookups that match NAME (or =) of type AFPServer (or =), and ASP
# GetStatus (an ATP request with ASP function 3, Inside AppleTalk 11-17) with a server
# information block: no login yet (Inside AppleTalk ch. 11 and 13, AppleTalk Filing Protocol 2.1).
ATALK_NAME = os.environ.get('PINET_ATALK')
ATALK_NODE = 104                 # the peer's node: Aurora68's "68" in hex, out of the way of the Mac's picks
ATALK_SOCKET = 250               # the AFP server's ASP socket (a statically assigned number, 1-127, would do too)
atalk_net = None                 # the network the Mac is on
def snap_frame(dst, snap, body):
    return dst + PEER_MAC + struct.pack('>H', 8 + len(body)) + b'\xaa\xaa\x03' + snap + body
def ddp_send(dst_mac, dnet, dnode, dsock, ssock, dtype, data):
    hdr = struct.pack('>HHHHBBBBB', 13 + len(data), 0, dnet, atalk_net, dnode, ATALK_NODE, dsock, ssock, dtype)
    send(snap_frame(dst_mac, bytes.fromhex('080007809b'), hdr + data))
def pstr(t):
    t = t.encode('mac_roman'); return bytes([len(t)]) + t
def nbp_match(pat, name):
    return pat == '=' or pat.lower() == name.lower()
def server_info():
    # FPGetSrvrInfo's reply (AFP 2.1): offsets, then flags, the name; the strings after
    machine, versions, uams = pstr('Aurora68'), bytes([1]) + pstr('AFPVersion 2.1'), bytes([1]) + pstr('No User Authent')
    fixed = 2 + 2 + 2 + 2 + 2          # offsets: machine type, versions, UAMs, icon; flags
    name = pstr(ATALK_NAME)
    if (fixed + len(name)) & 1: name += b'\0'
    o_mach = fixed + len(name); o_ver = o_mach + len(machine); o_uam = o_ver + len(versions)
    return struct.pack('>HHHHH', o_mach, o_ver, o_uam, 0, 0) + name + machine + versions + uams
atalk_mac = None                 # the Mac's Ethernet and AppleTalk addresses, from its datagrams
atalk_mac_node = None
aep_next = 0; aep_seq = 0
def aep_due():
    # an AppleTalk Echo request (AEP, DDP type 4, socket 4: Inside AppleTalk ch. 5) to the Mac every
    # ten seconds: its .MPP answers by itself, which shows datagrams going up and coming back
    global aep_next, aep_seq
    if not ATALK_NAME or atalk_mac is None or time.time() < aep_next: return
    aep_next = time.time() + 10; aep_seq += 1
    ddp_send(atalk_mac, atalk_net, atalk_mac_node, 4, 253, 4, bytes([1]) + b'aurora68 echo %d' % aep_seq)
    print(f"peer -> AEP echo request {aep_seq} to {atalk_net}.{atalk_mac_node}", flush=True)
def atalk_respond(frame):
    global atalk_net, atalk_mac, atalk_mac_node
    llc, src = frame[14:22], frame[6:12]
    if llc[3:8] == bytes.fromhex('00000080f3'):
        a = frame[22:]
        fn = struct.unpack('>H', a[6:8])[0]
        want = a[24:28]
        if atalk_net is None or (want[1] << 8 | want[2]) != atalk_net or want[3] != ATALK_NODE or fn not in (1, 3):
            return None
        body = struct.pack('>HHBBH', 1, 0x809b, 6, 4, 2) + PEER_MAC + bytes([0, atalk_net >> 8, atalk_net & 255, ATALK_NODE]) + a[8:18]
        send(snap_frame(src, bytes.fromhex('00000080f3'), body))
        return 'AARP response for our address'
    if llc[3:8] != bytes.fromhex('080007809b'):
        return None
    d = frame[22:]
    ln = struct.unpack('>H', d[0:2])[0] & 0x3ff
    dnet, snet = struct.unpack('>HH', d[4:8])
    dnode, snode, dsock, ssock, dtype = d[8], d[9], d[10], d[11], d[12]
    if atalk_net is None and snet:
        atalk_net = snet
        print(f"peer: AppleTalk node {atalk_net}.{ATALK_NODE} (the Mac's network)", flush=True)
    if snet == atalk_net and snode not in (0, 255):
        atalk_mac, atalk_mac_node = src, snode
    if dtype == 4 and dsock == 253 and ln > 13 and d[13] == 2:
        return 'AEP echo REPLY from the Mac: ' + repr(bytes(d[14:ln]))
    if dtype == 2 and dsock == 2 and d[13] >> 4 == 2:          # NBP LkUp
        nid = d[14]
        t, _ = nbp_tuple(d, 15)
        obj, rest = t.split(':', 1); typ, zone = rest.split('@', 1)
        if not (nbp_match(obj, ATALK_NAME) and nbp_match(typ, 'AFPServer')):
            return None
        rnet, rnode, rsock = struct.unpack('>HBB', d[15:19])
        tup = struct.pack('>HBBB', atalk_net, ATALK_NODE, ATALK_SOCKET, 0) + pstr(ATALK_NAME) + pstr('AFPServer') + pstr('*')
        ddp_send(src, rnet, rnode, rsock, 2, 2, bytes([0x31, nid]) + tup)
        return f"NBP LkUp-Reply '{ATALK_NAME}:AFPServer@*' to {rnet}.{rnode}:{rsock}"
    if dtype == 3 and dnode == ATALK_NODE and dsock == ATALK_SOCKET and ln >= 13 + 8:   # ATP
        ctl, bmp, tid = d[13], d[14], struct.unpack('>H', d[15:17])[0]
        user = d[17:21]
        if ctl >> 6 == 1 and user[0] == 3:                     # TReq, ASP GetStatus
            info = server_info()
            resp = bytes([0x90, 0]) + struct.pack('>H', tid) + bytes(4) + info   # TResp, EOM, sequence 0
            ddp_send(src, snet, snode, ssock, ATALK_SOCKET, 3, resp)
            return f"ASP GetStatus: server information ({len(info)} bytes)"
        return f"ATP to the server socket: control {ctl:02x}, ASP function {user[0]} (not answered yet)"
    return None

# PINET_APCLIENT=1: a phone on the kernel's access point (docs/08 item 117)
APCLIENT = os.environ.get('PINET_APCLIENT') == '1'
apc = {'state': 'discover', 'xid': 0x50484f4e, 'next': 0.0, 'server': None, 'offer': None, 'dns': 0}
if APCLIENT:
    PEER_IP = bytes(4)
def apc_dhcp(mtype, extra=b''):
    m = bytearray(300)
    m[0:4] = b'\x01\x01\x06\x00'; m[4:8] = struct.pack('>I', apc['xid']); m[10] = 0x80   # answer by broadcast: no address yet
    m[28:34] = PEER_MAC; m[236:240] = b'\x63\x82\x53\x63'
    o = bytes([53, 1, mtype]) + extra + bytes([12, 11]) + b'pinet-phone' + bytes([55, 3, 1, 3, 6, 255])
    m[240:240 + len(o)] = o
    udp = struct.pack('>HHHH', 68, 67, 8 + len(m), 0) + bytes(m)
    send4(b'\xff' * 6, b'\xff\xff\xff\xff', 17, udp)
def apc_dns(name, qtype):
    q = struct.pack('>HHHHHH', 0x4150 + apc['dns'], 0x0100, 1, 0, 0, 0)
    for part in name.split('.'): q += bytes([len(part)]) + part.encode()
    q += b'\x00' + struct.pack('>HH', qtype, 1)
    apc['dns'] += 1
    send4(kernel['mac'], kernel['ip'], 17, struct.pack('>HHHH', 5300, 53, 8 + len(q), 0) + q)
def apc_opts(m):
    o, k = {}, 240
    while k + 1 < len(m) and m[k] != 255:
        if m[k] == 0: k += 1; continue
        o[m[k]] = m[k + 2:k + 2 + m[k + 1]]; k += 2 + m[k + 1]
    return o
def apclient_in(frame):
    global PEER_IP
    if frame[12:14] != b'\x08\x00' or len(frame) < 42 or frame[23] != 17: return None
    ihl = (frame[14] & 15) * 4; u = frame[14 + ihl:]
    sport, dport = struct.unpack('>HH', u[:4]); body = u[8:]
    ip4 = lambda b: '.'.join(map(str, b))
    if sport == 67 and dport == 68 and len(body) >= 240 and struct.unpack('>I', body[4:8])[0] == apc['xid']:
        o = apc_opts(body); t = o.get(53, b'\0')[0]; yi = body[16:20]
        if t == 2 and apc['state'] == 'discover':
            apc['offer'], apc['server'], apc['state'] = yi, o.get(54), 'request'
            apc_dhcp(3, bytes([50, 4]) + yi + bytes([54, 4]) + o.get(54, bytes(4)))
            return f'AP client: DHCP OFFER of {ip4(yi)} from {ip4(o.get(54, bytes(4)))}: REQUEST sent'
        if t == 5 and apc['state'] == 'request':
            PEER_IP = yi; apc['state'] = 'bound'
            kernel['ip'] = o.get(54, kernel['ip']); kernel['mac'] = frame[6:12]
            apc_dns('connectivitycheck.gstatic.com', 1); apc_dns('captive.apple.com', 28); apc_dns('aurora68.local', 1)
            return (f'AP client: DHCP ACK: {ip4(yi)} mask {ip4(o.get(1, bytes(4)))} router {ip4(o.get(3, bytes(4)))} DNS {ip4(o.get(6, bytes(4)))} '
                    f'lease {struct.unpack(">I", o.get(51, bytes(4)))[0]} s, from {ip4(o.get(54, bytes(4)))} at {frame[6:12].hex(":")}; three DNS queries sent')
        if t == 6:
            apc['state'] = 'discover'; apc['next'] = 0.0
            return 'AP client: DHCP NAK: starting again'
        return f'AP client: DHCP message type {t} ignored'
    if sport == 53 and dport == 5300 and len(body) >= 12:
        an = struct.unpack('>H', body[6:8])[0]; p = 12; name = []
        while p < len(body) and body[p]: name.append(body[p + 1:p + 1 + body[p]].decode('ascii', 'replace')); p += 1 + body[p]
        qtype = struct.unpack('>H', body[p + 1:p + 3])[0]
        a = ip4(body[-4:]) if an else 'no records'
        return f'AP client: DNS {".".join(name)} type {qtype}: rcode {body[3] & 15}, {an} answer(s): {a}'
    return None
def apclient_due():
    if not APCLIENT or apc['state'] == 'bound': return
    now = time.time()
    if now < apc['next']: return
    apc['next'] = now + 2.0
    if apc['state'] == 'request': apc['state'] = 'discover'      # no ACK in two seconds: from the start
    apc['xid'] = (apc['xid'] + 1) & 0xffffffff
    apc_dhcp(1)
    print('AP client: DHCP DISCOVER sent', flush=True)

def peer(frame):
    global mac_ip4
    dst, src, etype = frame[0:6], frame[6:12], struct.unpack('>H', frame[12:14])[0]
    if APCLIENT:
        r = apclient_in(frame)
        if r: return r
        if etype == 0x0806 and len(frame) >= 42 and frame[28:32] == kernel['ip']: kernel['mac'] = frame[22:28]
        if etype == 0x0800 and len(frame) >= 42 and frame[23] == 17: return 'AP client: UDP (not for the phone)'
    if etype == 0x0800 and len(frame) >= 42 and frame[23] == 17 and frame[14 + (frame[14] & 15) * 4 + 2:14 + (frame[14] & 15) * 4 + 4] == b'\x14\xe9':
        if frame[26:30] != bytes(4): mac_ip4 = frame[26:30]
        if MDNS_RELAY:
            r = mdns_relay(src, frame[26:30], frame[14 + (frame[14] & 15) * 4:])
            if r: return r
        return mdns_answer(src, frame[26:30], frame[14 + (frame[14] & 15) * 4:])
    if etype == 0x0800 and len(frame) >= 34 and frame[26:30] != bytes(4): mac_ip4 = frame[26:30]
    if etype == 0x0806 and len(frame) >= 42:                 # ARP
        op = struct.unpack('>H', frame[20:22])[0]; sha, spa, tpa = frame[22:28], frame[28:32], frame[38:42]
        if spa == kernel['ip'] and op in (1, 2):
            kernel['mac'] = sha                                  # its gratuitous ARP, or an answer
        if op == 1 and tpa == PEER_IP:
            arp = struct.pack('>HHBBH', 1, 0x0800, 6, 4, 2) + PEER_MAC + PEER_IP + sha + spa
            send(sha + PEER_MAC + b'\x08\x06' + arp + bytes(18)); return 'ARP reply'
        return 'ARP (ignored)'
    if etype == 0x0800 and len(frame) >= 34:                 # IPv4
        ihl = (frame[14] & 15) * 4; proto = frame[23]; sip, dip = frame[26:30], frame[30:34]
        # PINET_INTERNET=1: the peer is a router to everywhere, and answers an ICMP echo to any
        # address (the testbed's way to see which interface a default route sends a ping out of)
        internet = os.environ.get('PINET_INTERNET') == '1' and proto == 1 and dip[0] not in (127, 224, 255)
        if proto == 17 and UDP_FWD and len(frame) >= 14 + ihl + 8:
            sport_, dport_, ulen_ = struct.unpack('>HHH', frame[14 + ihl:14 + ihl + 6])
            if dport_ in UDP_FWD:
                return udp_relay(src, sip, sport_, dport_, frame[14 + ihl + 8:14 + ihl + ulen_])
        if dip != PEER_IP and dip != b'\xff\xff\xff\xff' and not internet: return 'IP not for us'
        payload = frame[14 + ihl:]
        if proto == 1 and payload[0] == 8:                   # ICMP echo
            icmp = b'\x00\x00' + b'\x00\x00' + payload[4:]
            icmp = icmp[:2] + struct.pack('>H', csum(icmp)) + icmp[4:]
            ip = bytearray(frame[14:14 + ihl]); ip[12:16], ip[16:20] = dip, sip; ip[10:12] = b'\0\0'
            ip[10:12] = struct.pack('>H', csum(bytes(ip)))
            send(src + PEER_MAC + b'\x08\x00' + bytes(ip) + icmp); return 'ICMP echo reply' + ('' if dip == PEER_IP else ' (as the router, from %d.%d.%d.%d)' % tuple(dip))
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
                mtype, k, hostname = 0, 240, b''
                if req[236:240] == bytes([99, 130, 83, 99]):
                    while k + 1 < len(req) and req[k] != 255:
                        if req[k] == 0: k += 1; continue
                        if req[k] == 53: mtype = req[k + 2]
                        if req[k] == 12: hostname = req[k + 2:k + 2 + req[k + 1]]
                        k += 2 + req[k + 1]
                # the Pi's kernel (the remote control, docs/13) asks with its own host name: .3, so that
                # the Mac keeps .2 whichever asks first; on the WiFi both have the same MAC address
                is_kernel = hostname.lower().startswith(b'aurora68')
                if is_kernel:
                    rep[16:20] = kernel['ip']; kernel['mac'] = src
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
                return {0: 'BOOTP reply', 1: 'DHCP OFFER', 3: 'DHCP ACK'}.get(mtype, 'DHCP type %d answered' % mtype) + \
                    (' %d.%d.%d.%d to the kernel (%s)' % (*kernel['ip'], hostname.decode('latin-1')) if is_kernel else ' 192.168.%d.2' % NET)
            if dport == 53 and len(payload) >= 8 + 12:
                return dns_answer(frame, src, sip, sport, payload[8:ulen])
            if dport == 7:
                udp = struct.pack('>HHHH', 7, sport, ulen, 0) + payload[8:]
                ip = bytearray(frame[14:14 + ihl]); ip[12:16], ip[16:20] = PEER_IP, sip; ip[10:12] = b'\0\0'
                ip[10:12] = struct.pack('>H', csum(bytes(ip)))
                send(src + PEER_MAC + b'\x08\x00' + bytes(ip) + udp); return 'UDP echo'
        if proto == 6 and len(payload) >= 20:
            if sip == kernel['ip'] and struct.unpack('>H', payload[2:4])[0] in kclients:
                return kernel_in(frame, payload)
            return tcp_echo(frame, src, sip, payload)
        return f'IP proto {proto}'
    if etype == 0x86dd and len(frame) >= 54:                 # IPv6
        return peer6(frame, src)
    if etype <= 1500 and len(frame) >= 22:                   # 802.3 with 802.2: AppleTalk Phase 2
        r = atalk(frame)
        if r and ATALK_NAME:
            try:
                a = atalk_respond(frame)
                if a: r += ' -> ' + a
            except (IndexError, ValueError, struct.error) as e:
                r += f' (not understood: {e})'
        if r: return r
    return f'type {etype:04x}'

def send4(dstmac, dip, proto, body):
    ip = bytearray(struct.pack('>BBHHHBBH', 0x45, 0, 20 + len(body), 0, 0, 64, proto, 0) + PEER_IP + dip)
    ip[10:12] = struct.pack('>H', csum(bytes(ip)))
    send(dstmac + PEER_MAC + b'\x08\x00' + bytes(ip) + body)

# TCP echo on port 7: one connection per client port, in order only (the testbed loses nothing)
tcp_conns = {}
def tcp_seg(dstmac, dip, sport, dport, seq, ack, flags, data=b''):
    hdr = struct.pack('>HHIIBBHHH', sport, dport, seq & 0xffffffff, ack & 0xffffffff, 5 << 4, flags, 8192, 0, 0)
    pseudo = PEER_IP + dip + struct.pack('>BBH', 0, 6, len(hdr) + len(data))
    c = csum(pseudo + hdr + data)
    send4(dstmac, dip, 6, hdr[:16] + struct.pack('>H', c) + hdr[18:] + data)

def tcp_seg6(dstmac, src6, dst6, sport, dport, seq, ack, flags, data=b''):
    hdr = struct.pack('>HHIIBBHHH', sport, dport, seq & 0xffffffff, ack & 0xffffffff, 5 << 4, flags, 8192, 0, 0)
    pseudo = src6 + dst6 + struct.pack('>I', len(hdr) + len(data)) + b'\0\0\0\x06'
    c = csum(pseudo + hdr + data)
    seg = hdr[:16] + struct.pack('>H', c) + hdr[18:] + data
    send(dstmac + PEER_MAC + b'\x86\xdd' + struct.pack('>IHBB', 0x60000000, len(seg), 6, 64) + src6 + dst6 + seg)

# a segment to the Mac over IPv4, or over IPv6 from me6 (the address of ours it connected to)
def rseg(mac, ip, me6, sport, dport, seq, ack, flags, data=b''):
    if me6:
        tcp_seg6(mac, me6, ip, sport, dport, seq, ack, flags, data)
    else:
        tcp_seg(mac, ip, sport, dport, seq, ack, flags, data)

# UDP relay (PINET_UDP="8070=127.0.0.1:18470", 22 November 2026): the Mac's datagrams to that port, sent to the
# peer, to a broadcast address or to a multicast group (Aurora68 Play's probe for the music server, docs/08
# item 99), go to a host socket; what comes back is sent to the Mac from the peer's address
UDP_FWD = {}
for item in filter(None, os.environ.get('PINET_UDP', '').split(',')):
    port, dest = item.split('=')
    h, hp = dest.rsplit(':', 1)
    UDP_FWD[int(port)] = (h, int(hp))
udp_relays = []             # (host socket, Mac's MAC, Mac's address, its port, our port, until)
def udp_relay(src, sip, sport, dport, data):
    import socket as _s
    hs = _s.socket(_s.AF_INET, _s.SOCK_DGRAM)
    hs.setblocking(False)
    hs.sendto(data, UDP_FWD[dport])
    udp_relays.append((hs, src, sip, sport, dport, time.monotonic() + 5))
    return f'UDP to port {dport} relayed to {UDP_FWD[dport][0]}:{UDP_FWD[dport][1]}'
def udp_due():
    for r in list(udp_relays):
        hs, src, sip, sport, dport, until = r
        try:
            while True:
                d = hs.recv(2048)
                udp = struct.pack('>HHHH', dport, sport, 8 + len(d), 0) + d
                ip = bytearray(struct.pack('>BBHHHBBH', 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0) + PEER_IP + sip)
                ip[10:12] = struct.pack('>H', csum(bytes(ip)))
                send(src + PEER_MAC + b'\x08\x00' + bytes(ip) + udp)
                print(f'pinet: a UDP answer of {len(d)} bytes from the host to the Mac\'s port {sport}', flush=True)
        except BlockingIOError:
            pass
        except OSError:
            until = 0
        if time.monotonic() > until:
            hs.close()
            udp_relays.remove(r)

# TCP relay (PINET_FORWARD): one host socket per connection from the Mac
FORWARD = {}
for item in filter(None, os.environ.get('PINET_FORWARD', '').split(',')):
    port, dest = item.split('=')
    h, hp = dest.rsplit(':', 1)
    FORWARD[int(port)] = (h, int(hp))
relays = {}
def relay_seg(r, flags, data=b''):
    rseg(r['mac'], r['ip'], r.get('me6'), r['lport'], r['rport'], r['snd'], r['rcv'], flags, data)
# HTTP on port 80 (unless PINET_FORWARD takes the port): a small web server for the Mac's MacTCP
# and Open Transport browser tests. HTTP/1.0, one request per connection, then the server closes
# (as the servers of 1997 did for HTTP/1.0 clients):
#   /           a small HTML page          /big       256 KB of text (a line pattern, checkable)
#   /slow       the page in pieces, a second apart (the browser's receive timeouts and ASRs)
#   /reset      the connection reset after the headers (TCPTerminate with remote abort)
#   /idle       the page, kept alive, then closed by the server after 30 s of nothing
#   anything else  404
HTTP_PAGE = (b'<html><head><title>Aurora68 testbed</title></head>\r\n<body><h1>It works</h1>\r\n'
             b'<p>Served by pinet.py to the Mac at 192.168.68.2.</p>\r\n</body></html>\r\n')
def http_big(n=262144):
    out, i = [], 0
    while sum(map(len, out)) < n:
        out.append(b'%06d Riptide on the Mac: line of the large file, 64 bytes long.\r\n'[:60] % i + b'\r\n'); i += 1
    return b''.join(out)[:n]
HTTP_BIG = http_big()
def http_response(req):
    line = req.split(b'\r\n', 1)[0].decode('latin-1', 'replace')
    parts = line.split()
    path = parts[1] if len(parts) > 1 else '/'
    def resp(code, text, body, ctype=b'text/html'):
        conn = b'keep-alive' if path == '/idle' else b'close'
        return (b'HTTP/1.0 %d %s\r\nServer: pinet.py\r\nContent-Type: %s\r\nContent-Length: %d\r\n'
                b'Connection: %s\r\n\r\n' % (code, text, ctype, len(body), conn)) + body, path
    if path in ('/', '/index.html', '/slow', '/reset', '/idle'):
        return resp(200, b'OK', HTTP_PAGE)
    if path == '/big':
        return resp(200, b'OK', HTTP_BIG, b'text/plain')
    return resp(404, b'Not Found', b'<html><body>404: %s</body></html>\r\n' % path.encode('latin-1', 'replace'))
def http_request(r, data):
    r['req'] += data
    if r['out'] or r['fin_out'] or b'\r\n\r\n' not in r['req']:
        return ''
    body, path = http_response(r['req'])
    if path == '/slow':
        r['later'] = [(time.time() + 1 + i, body[k:k + 40]) for i, k in enumerate(range(0, len(body), 40))]
    elif path == '/reset':
        r['out'] = body[:body.index(b'\r\n\r\n') + 4]; r['reset'] = True
    elif path == '/idle':
        r['out'] = body; r['later'] = [(time.time() + 30, b'')]          # the FIN after 30 idle seconds
    else:
        r['out'] = body; r['fin_out'] = True
    return f', HTTP {r["req"].split(chr(13).encode(), 1)[0].decode("latin-1")!r}: {len(body)} bytes'

def relay_tcp(frame, src, sip, t, sport, dport, seq, ack, flags, data, win, me6=None):
    key = (sip, sport, dport)
    if flags & 0x02:                                               # SYN: connect to the host
        if dport in FORWARD:
            try:
                hs = socket.create_connection(FORWARD[dport], timeout=3)
            except OSError as e:
                rseg(src, sip, me6, dport, sport, 0, seq + 1, 0x14)
                return f'TCP relay {dport}: host refused ({e})'
            hs.setblocking(False)
        else:
            hs = None                                              # the HTTP server
        relays[key] = {'sock': hs, 'mac': src, 'ip': sip, 'lport': dport, 'rport': sport, 'snd': 0x68100001,
                       'una': 0x68100001, 'rcv': seq + 1, 'win': win, 'out': b'', 'fin_out': False, 'fin_sent': False,
                       'sent': b'', 'moved': time.time(),
                       'req': b'', 'later': [], 'reset': False, 'me6': me6}
        rseg(src, sip, me6, dport, sport, 0x68100000, seq + 1, 0x12)
        if hs is None:
            return f'TCP {dport} (HTTP{" over IPv6 to " + ip6_addr(me6) if me6 else ""}): SYN-ACK'
        return f'TCP relay {dport} -> {FORWARD[dport][0]}:{FORWARD[dport][1]}: SYN-ACK'
    r = relays.get(key)
    if not r:
        if not flags & 0x04:
            rseg(src, sip, me6, dport, sport, ack, seq + len(data) + (1 if flags & 0x01 else 0), 0x14)
        return 'TCP relay (no connection)'
    if flags & 0x04:
        if r['sock']: r['sock'].close()
        relays.pop(key, None); return 'TCP relay RST'
    if flags & 0x10 and (ack - r['una']) & 0x80000000 == 0 and (r['snd'] - ack) & 0x80000000 == 0:
        if ack != r['una']:
            r['sent'] = r['sent'][(ack - r['una']) & 0xffffffff:]      # what is acknowledged is forgotten
            r['moved'] = time.time()
        r['una'] = ack
    r['win'] = win
    what = 'TCP relay ack'
    if data:
        if seq == r['rcv']:
            r['rcv'] += len(data)
            if r['sock'] is None:
                what = f'TCP {dport} {len(data)} bytes' + http_request(r, data)
            else:
                try:
                    r['sock'].sendall(data)
                except OSError:
                    pass
                what = f'TCP relay {len(data)} bytes to the host'
            relay_seg(r, 0x10)
        else:
            relay_seg(r, 0x10)                                     # out of order: ack what we have
    if flags & 0x01 and seq + len(data) == r['rcv']:
        r['rcv'] += 1
        r['closed_in'] = True
        if r['sock'] is not None:
            try:
                r['sock'].shutdown(socket.SHUT_WR)
            except OSError:
                pass
        relay_seg(r, 0x10)
        what += ', FIN'
    if r['fin_sent'] and r.get('closed_in') and r['una'] == r['snd']:
        relays.pop(key, None)
        what += ' (closed)'
    return what
def relay_due():
    now = time.time()
    for key, r in list(relays.items()):
        if r['sock'] is None:                                      # HTTP: pieces due, a reset
            while r['later'] and r['later'][0][0] <= now:
                r['out'] += r['later'].pop(0)[1]
                if not r['later']: r['fin_out'] = True
            if r['reset'] and not r['out'] and r['una'] == r['snd']:
                relay_seg(r, 0x14); relays.pop(key, None)
                print(f"peer -> TCP {r['lport']}: reset after the headers", flush=True)
                continue
        elif not r['fin_out']:
            try:
                d = r['sock'].recv(65536)
                if d:
                    r['out'] += d
                else:
                    r['fin_out'] = True
            except BlockingIOError:
                pass
            except OSError:
                r['fin_out'] = True
        # Go back N: a frame the Mac's receive ring had no room for is sent again, with what followed it,
        # after a second without an acknowledgement (the Mac keeps acknowledging what it has)
        if r['snd'] != r['una'] and now - r['moved'] > 1.0:
            if r['fin_sent']:
                r['fin_sent'] = False
            r['out'] = r['sent'] + r['out']; r['sent'] = b''
            r['snd'] = r['una']; r['moved'] = now
            print(f"peer -> TCP {r['lport']}: nothing acknowledged for a second, sending again from {r['una']:#x}", flush=True)
        inflight = (r['snd'] - r['una']) & 0xffffffff
        room = max(0, r['win'] - inflight)
        burst = 4                                                  # a few frames a round: the Mac's ring is small
        while r['out'] and room > 0 and burst:
            n = min(1400, room, len(r['out']))
            relay_seg(r, 0x18, r['out'][:n])
            r['sent'] += r['out'][:n]
            r['snd'] += n; r['out'] = r['out'][n:]; room -= n; burst -= 1
        if r['fin_out'] and not r['out'] and not r['fin_sent']:
            relay_seg(r, 0x11)
            r['snd'] += 1; r['fin_sent'] = True
        if r['fin_sent'] and r['una'] == r['snd'] and r['rcv'] and key in relays and r.get('closed_in'):
            if r['sock'] is not None: r['sock'].close()
            relays.pop(key, None)

def tcp_echo(frame, src, sip, t):
    sport, dport, seq, ack, off, flags = struct.unpack('>HHIIBB', t[:14])
    ip_total = struct.unpack('>H', frame[16:18])[0]
    data = t[(off >> 4) * 4:ip_total - 20]
    if dport in FORWARD or dport == 80:
        win = struct.unpack('>H', t[14:16])[0]
        return relay_tcp(frame, src, sip, t, sport, dport, seq, ack, flags, data, win)
    if dport != 7:
        if not flags & 0x04:
            tcp_seg(src, sip, dport, sport, 0, seq + 1, 0x14)      # RST|ACK
        return f'TCP to port {dport}: reset'
    key = (sip, sport)
    if flags & 0x02:                                               # SYN
        tcp_conns[key] = {'snd': 0x68000001, 'rcv': seq + 1}
        tcp_seg(src, sip, 7, sport, 0x68000000, seq + 1, 0x12)     # SYN|ACK
        return 'TCP SYN: SYN-ACK (echo)'
    c = tcp_conns.get(key)
    if not c:
        return 'TCP (no connection)'
    if flags & 0x04:
        tcp_conns.pop(key, None); return 'TCP RST'
    what = 'TCP ack'
    if seq == c['rcv'] and data:
        c['rcv'] += len(data)
        tcp_seg(src, sip, 7, sport, c['snd'], c['rcv'], 0x18, data)    # PSH|ACK: the data back
        c['snd'] += len(data)
        what = 'TCP ECHO %d bytes: %r' % (len(data), bytes(data[:40]))
    if flags & 0x01 and seq + len(data) == c['rcv'] - (0 if data else 0):
        c['rcv'] += 1
        tcp_seg(src, sip, 7, sport, c['snd'], c['rcv'], 0x11)      # FIN|ACK
        c['snd'] += 1
        what += ', FIN answered'
    return what

# DNS: peer.testbed and any name ending in .testbed is the peer; its address points back to peer.testbed
def dns_answer(frame, src, sip, sport, q):
    qid, fl, qd = struct.unpack('>HHH', q[:6])
    p, labels = 12, []
    while p < len(q) and q[p]:
        labels.append(q[p + 1:p + 1 + q[p]].decode('ascii', 'replace')); p += 1 + q[p]
    p += 1
    qtype = struct.unpack('>H', q[p:p + 2])[0]
    name = '.'.join(labels).lower()
    question = q[12:p + 4]
    ans, rcode = b'', 3
    if name in ('silent.testbed', '66.68.168.192.in-addr.arpa'):
        return 'DNS %s: not answered (a name server that does not reply)' % name
    if name == 'many.testbed':
        # NPO's shape (21 November 2026): a CNAME to many-cdn.testbed, which has nine IPv6 addresses that
        # answer nothing (fd68::901 ...) and the peer's IPv4 address. A program that keeps only the first
        # eight addresses in IPv6-first order never gets to IPv4 (docs/08 item 94).
        rcode = 0
        target = bytes([8]) + b'many-cdn' + bytes([7]) + b'testbed' + b'\0'
        ans = b'\xc0\x0c' + struct.pack('>HHIH', 5, 1, 60, len(target)) + target
        tptr = 12 + len(question)                             # where the target name starts in the message
        tref = struct.pack('>H', 0xC000 | (tptr + 12))
        count = 1
        if qtype == 28:
            for k in range(9):
                ans += tref + struct.pack('>HHIH', 28, 1, 60, 16) + bytes.fromhex('fd68000000000000000000000000%04x' % (0x901 + k))
                count += 1
        elif qtype == 1:
            ans += tref + struct.pack('>HHIH', 1, 1, 60, 4) + PEER_IP; count += 1
        hdr = struct.pack('>HHHHHH', qid, 0x8180, 1, count, 0, 0)
        body = hdr + question + ans
        send4(src, sip, 17, struct.pack('>HHHH', 53, sport, 8 + len(body), 0) + body)
        return 'DNS %s many.testbed: a CNAME and %d records' % ({1: 'A', 28: 'AAAA'}.get(qtype, qtype), count - 1)
    if name == 'v6only.testbed':                             # IPv6 only: AAAA, and no A (but the name exists)
        rcode = 0
        if qtype == 28:
            ans = b'\xc0\x0c' + struct.pack('>HHIH', 28, 1, 60, 16) + V6ONLY6
    elif qtype == 28 and name == 'peer.testbed':
        ans = b'\xc0\x0c' + struct.pack('>HHIH', 28, 1, 60, 16) + PEER_GL6; rcode = 0
    elif qtype == 28 and (name == 'testbed' or name.endswith('.testbed')):
        rcode = 0                                            # the other names: IPv4 only
    elif qtype == 1 and (name == 'testbed' or name.endswith('.testbed')):
        ans = b'\xc0\x0c' + struct.pack('>HHIH', 1, 1, 60, 4) + PEER_IP; rcode = 0
    elif qtype == 12 and name == '1.68.168.192.in-addr.arpa':
        rd = bytes([4]) + b'peer' + bytes([7]) + b'testbed' + b'\0'
        ans = b'\xc0\x0c' + struct.pack('>HHIH', 12, 1, 60, len(rd)) + rd; rcode = 0
    hdr = struct.pack('>HHHHHH', qid, 0x8180 | rcode, 1, 1 if ans else 0, 0, 0)
    body = hdr + question + ans
    udp = struct.pack('>HHHH', 53, sport, 8 + len(body), 0) + body
    send4(src, sip, 17, udp)
    return 'DNS %s %s: %s' % ({1: 'A', 12: 'PTR', 28: 'AAAA'}.get(qtype, qtype), name,
                              'answered' if ans else 'no such name' if rcode else 'no record of that type')

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
    ip6 = frame[14:54]; nh = ip6[6]; src6, dst6 = ip6[8:24], ip6[24:40]
    pl = frame[54:54 + struct.unpack('>H', ip6[4:6])[0]]
    if nh == 6 and len(pl) >= 20 and dst6 in (PEER_GL6, PEER_LL6, V6ONLY6):     # TCP over IPv6
        sport, dport, seq, ack, off, flags = struct.unpack('>HHIIBB', pl[:14])
        win = struct.unpack('>H', pl[14:16])[0]
        data = pl[(off >> 4) * 4:]
        if dport == 80 or dport in FORWARD:              # PINET_FORWARD's ports over IPv6 too (22 November 2026)
            return relay_tcp(frame, src, src6, pl, sport, dport, seq, ack, flags, data, win, me6=dst6)
        if not flags & 0x04:
            tcp_seg6(src, dst6, src6, dport, sport, 0, seq + 1, 0x14)
        return f'TCP over IPv6 to port {dport}: reset'
    if nh == 17 and len(pl) >= 8 and dst6 in (PEER_GL6, PEER_LL6, V6ONLY6):    # UDP echo over IPv6
        sport, dport, ulen = struct.unpack('>HHH', pl[:6])
        if dport != 7:
            return f'UDP over IPv6 to port {dport}'
        body = pl[8:ulen]
        udp = struct.pack('>HHHH', 7, sport, 8 + len(body), 0) + body
        pseudo = dst6 + src6 + struct.pack('>I', len(udp)) + b'\0\0\0\x11'
        udp = udp[:6] + struct.pack('>H', csum(pseudo + udp) or 0xffff) + udp[8:]
        send(src + PEER_MAC + b'\x86\xdd' + struct.pack('>IHBB', 0x60000000, len(udp), 17, 64) + dst6 + src6 + udp)
        return 'UDP echo over IPv6 from ' + ip6_addr(dst6)
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
        if target in (PEER_LL6, PEER_GL6, V6ONLY6):
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

# PINET_KERNEL=PORT: the host's way to the kernel's own HTTP server (psmac_remote.c, docs/13). Each
# connection to 127.0.0.1:PORT is a TCP connection from the peer (192.168.N.1) to the kernel's address,
# port 80, through the file bridge, beside the Mac's own traffic. The testbed loses nothing, but the
# kernel's receive queue is small, so the window is kept to what it advertises and a second without
# an acknowledgement sends again from the oldest byte.
kernel = {'mac': None, 'ip': bytes(int(x) for x in os.environ['PINET_KERNEL_IP'].split('.')) if os.environ.get('PINET_KERNEL_IP') else bytes([192, 168, NET, 3])}
KERNEL_PORT = int(os.environ.get('PINET_KERNEL', '0'))
KERNEL_TO = int(os.environ.get('PINET_KERNEL_TO', '80'))
kclients = {}
klisten = None
if KERNEL_PORT:
    klisten = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    klisten.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    klisten.bind(('127.0.0.1', KERNEL_PORT)); klisten.listen(8); klisten.setblocking(False)
kport_next = 41000
def kseg(k, flags, data=b'', seq=None):
    opts = struct.pack('>BBH', 2, 4, 1460) if flags & 0x02 else b''
    hl = 20 + len(opts)
    hdr = struct.pack('>HHIIBBHHH', k['lport'], KERNEL_TO, (k['snd'] if seq is None else seq) & 0xffffffff, k['rcv'] & 0xffffffff,
                      (hl // 4) << 4, flags, 65535, 0, 0) + opts
    c = csum(PEER_IP + kernel['ip'] + struct.pack('>BBH', 0, 6, hl + len(data)) + hdr + data)
    send4(kernel['mac'], kernel['ip'], 6, hdr[:16] + struct.pack('>H', c) + hdr[18:] + data)
def kernel_in(frame, t):
    sport, dport, seq, ack, off, flags = struct.unpack('>HHIIBB', t[:14])
    win = struct.unpack('>H', t[14:16])[0]
    ip_total = struct.unpack('>H', frame[16:18])[0]
    data = t[(off >> 4) * 4:ip_total - 20]
    k = kclients[dport]
    k['win'] = win
    if flags & 0x04:
        k['sock'].close(); del kclients[dport]; return f'kernel TCP {dport}: reset by the kernel'
    if k['state'] == 'syn':
        if flags & 0x12 == 0x12 and ack == k['iss'] + 1:
            k['state'] = 'est'; k['rcv'] = seq + 1; k['una'] = k['snd'] = ack; k['moved'] = time.time()
            kseg(k, 0x10); return f'kernel TCP {dport}: connected'
        return None
    if flags & 0x10 and (ack - k['una']) & 0x80000000 == 0 and (k['snd'] - ack) & 0x80000000 == 0:
        if ack != k['una']:
            n = (ack - k['una']) & 0xffffffff
            k['sent'] = k['sent'][n:]; k['moved'] = time.time()
        k['una'] = ack
    if data or flags & 0x01:
        if seq == k['rcv']:
            if data:
                k['rcv'] += len(data); k['got'] += len(data)
                try:
                    k['sock'].sendall(data)
                except OSError:
                    pass
            if flags & 0x01:
                k['rcv'] += 1; k['fin_in'] = True
                try:
                    k['sock'].shutdown(socket.SHUT_WR)
                except OSError:
                    pass
        kseg(k, 0x10)
    if flags & 0x01:
        return f'kernel TCP {dport}: FIN after {k["got"]} bytes'
    return None
def kernel_due():
    global kport_next
    if klisten is not None:
        try:
            hs, _ = klisten.accept()
            if kernel['mac'] is None:
                hs.close(); print('peer: a host connection for the kernel, but it has no address yet', flush=True)
            else:
                hs.setblocking(False)
                kport_next = 41000 + (kport_next - 41000 + 1) % 20000
                k = {'sock': hs, 'lport': kport_next, 'state': 'syn', 'iss': 0x50490000 + kport_next * 7919, 'rcv': 0,
                     'win': 0, 'out': b'', 'sent': b'', 'fin_out': False, 'fin_sent': False, 'fin_in': False, 'got': 0,
                     'tries': 1, 'moved': time.time(), 'opened': time.time()}
                k['snd'] = k['una'] = k['iss']
                kclients[kport_next] = k
                kseg(k, 0x02, seq=k['iss'])
                print(f"peer -> kernel TCP {kport_next}: SYN to {'.'.join(map(str, kernel['ip']))}:{KERNEL_TO}", flush=True)
        except BlockingIOError:
            pass
    now = time.time()
    for port, k in list(kclients.items()):
        if k['state'] == 'syn':
            if now - k['moved'] > 1.0:
                if k['tries'] >= 6:
                    k['sock'].close(); del kclients[port]; print(f'peer: kernel TCP {port}: no answer', flush=True); continue
                k['tries'] += 1; k['moved'] = now; kseg(k, 0x02, seq=k['iss'])
            continue
        if not k['fin_out']:
            try:
                d = k['sock'].recv(65536)
                if d: k['out'] += d
                else: k['fin_out'] = True
            except BlockingIOError:
                pass
            except OSError:
                k['fin_out'] = True
        if k['snd'] != k['una'] and now - k['moved'] > 1.0:            # go back to the oldest byte
            k['fin_sent'] = False; k['out'] = k['sent'] + k['out']; k['sent'] = b''; k['snd'] = k['una']; k['moved'] = now
        room = max(0, k['win'] - ((k['snd'] - k['una']) & 0xffffffff))
        burst = 4
        while k['out'] and room > 0 and burst:
            n = min(1400, room, len(k['out']))
            kseg(k, 0x18, k['out'][:n]); k['sent'] += k['out'][:n]; k['snd'] += n; k['out'] = k['out'][n:]; room -= n; burst -= 1
        if k['fin_out'] and not k['out'] and not k['fin_sent']:
            kseg(k, 0x11); k['snd'] += 1; k['fin_sent'] = True
        if k['fin_in'] and k['fin_sent'] and k['una'] == k['snd']:
            k['sock'].close(); del kclients[port]
        elif now - k['opened'] > 120:
            kseg(k, 0x14); k['sock'].close(); del kclients[port]; print(f'peer: kernel TCP {port}: given up after two minutes', flush=True)

tap = None
if mode == 'tap':
    TUNSETIFF = 0x400454ca; IFF_TAP = 0x0002; IFF_NO_PI = 0x1000
    tap = os.open('/dev/net/tun', os.O_RDWR | os.O_NONBLOCK)
    fcntl.ioctl(tap, TUNSETIFF, struct.pack('16sH', sys.argv[3].encode(), IFF_TAP | IFF_NO_PI))

off = os.path.getsize(tx_path)
print(f"pinet: {mode} on {base} (tx offset {off})", flush=True)
buf = b''
last_frame = 0.0
while True:
    with open(tx_path, 'rb') as f:
        f.seek(off); data = f.read()
    if data:
        off += len(data); buf += data
        while len(buf) >= 2:
            n = struct.unpack('>H', buf[:2])[0]
            if len(buf) < 2 + n: break
            frame = buf[2:2 + n]; buf = buf[2 + n:]
            if lost(frame): continue
            if tap is not None:
                os.write(tap, frame); what = 'to tap'
            else:
                what = peer(frame) if n >= 14 else f'a {n}-byte runt (the Pi reset in the middle of a frame?)'
            if what is not None:                                       # the kernel's stream of segments is not logged one by one
                print(f"mac -> {frame[0:6].hex(':')} type {frame[12:14].hex()} len {n}: {what}", flush=True)
    if tap is None:
        ping6_due()
        mdns_due()
        aep_due()
        relay_due()
        udp_due()
        kernel_due()
        apclient_due()
    if tap is not None:
        try:
            while True:
                frame = os.read(tap, 2048)
                send(frame)
        except BlockingIOError:
            pass
    # an instant peer while frames flow (so the testbed's round trips measure the Pi and the Mac,
    # not this loop: its 10 ms sleep was most of a ping's time, 19 November), 10 ms when quiet
    if data or kclients: last_frame = time.monotonic()
    time.sleep(0.0005 if time.monotonic() - last_frame < 2.0 else 0.01)
