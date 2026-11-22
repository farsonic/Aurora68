// SPDX-License-Identifier: MPL-2.0
//
// Network.swift - finding the music server on the home network, and looking up a typed name with the
// owner's IPv6/IPv4 preference.
//
// Backstory (22 November 2026). The server answers "AURORA68-MUSIC?" on UDP port 8070, sent as a
// broadcast or to the group 239.255.68.70 (the SE's Aurora68 Play sends both: Riptide refuses
// broadcasts, so the server joined the group). The answer names the server, its TCP port and both of
// its addresses, so the app picks the family the owner prefers. A typed name goes through getaddrinfo
// and is tried address by address in the preferred order, as numbers: the system's own choice
// (Happy Eyeballs) would not honour the preference. Plain BSD sockets: a few dozen lines, and they need
// nothing beyond the Local Network permission macOS asks the owner for once.
import Foundation
import Darwin
import Aurora68Core

enum Discovery {
    /// send the probe (broadcast and multicast) and collect answers for `timeout` seconds
    static func probe(timeout: Double = 1.5) -> [DiscoveryReply] {
        let fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { return [] }
        defer { close(fd) }
        var on: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, socklen_t(MemoryLayout<Int32>.size))
        var ttl: UInt8 = 1
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, socklen_t(MemoryLayout<UInt8>.size))
        let message = Array(Address.probe.utf8)
        for dest in ["255.255.255.255", Address.discoveryGroup] {
            var a = sockaddr_in()
            a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            a.sin_family = sa_family_t(AF_INET)
            a.sin_port = Address.discoveryPort.bigEndian
            inet_pton(AF_INET, dest, &a.sin_addr)
            _ = withUnsafePointer(to: &a) { p in
                p.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    sendto(fd, message, message.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
        var found: [DiscoveryReply] = []
        let deadline = Date().addingTimeInterval(timeout)
        while true {
            let left = deadline.timeIntervalSinceNow
            if left <= 0 { break }
            var p = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
            if poll(&p, 1, Int32(left * 1000)) <= 0 { break }
            var buf = [UInt8](repeating: 0, count: 600)
            var from = sockaddr_in()
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            let n = withUnsafeMutablePointer(to: &from) { fp in
                fp.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(fd, &buf, buf.count, 0, $0, &len) }
            }
            guard n > 0 else { continue }
            var ip = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
            inet_ntop(AF_INET, &from.sin_addr, &ip, socklen_t(INET_ADDRSTRLEN))
            if let r = DiscoveryReply.parse(Data(buf[0..<n]), sender: String(cString: ip)),
               !found.contains(where: { $0.name == r.name && $0.port == r.port }) {
                found.append(r)
            }
        }
        return found
    }
}

enum Resolver {
    /// a name's addresses as numbers (link-local ones left out: they need a zone a URL rarely carries)
    static func addresses(of host: String) -> [String] {
        var hints = addrinfo()
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = SOCK_STREAM
        var res: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host, nil, &hints, &res) == 0, let first = res else { return [] }
        defer { freeaddrinfo(first) }
        var out: [String] = []
        var p: UnsafeMutablePointer<addrinfo>? = first
        while let ai = p {
            var name = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(ai.pointee.ai_addr, ai.pointee.ai_addrlen, &name, socklen_t(name.count), nil, 0, NI_NUMERICHOST) == 0 {
                let s = String(cString: name)
                if !s.lowercased().hasPrefix("fe80") && !out.contains(s) { out.append(s) }
            }
            p = ai.pointee.ai_next
        }
        return out
    }
}
