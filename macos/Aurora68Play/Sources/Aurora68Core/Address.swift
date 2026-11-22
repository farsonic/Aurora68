// SPDX-License-Identifier: MPL-2.0
//
// Address.swift - where the music server is: addresses as typed, URLs with IPv6 in brackets, the
// discovery answer, and the owner's choice of IPv6 or IPv4.
//
// Backstory (22 November 2026). The owner wants IPv6 to work and to choose it over IPv4 or the other
// way round (on their network a name resolves to both, and an IPv6 address of the server reached the
// MacBook when the documentation-range one did not). So the app never leaves the choice to the
// system's resolver: a typed name is looked up, its addresses ordered by the preference and tried in
// that order as numbers; the discovery answer carries both of the server's addresses and is ordered
// the same way. An IPv6 address in a URL goes in brackets (RFC 3986), and a file's path is
// percent-encoded segment by segment, ASCII letters and digits only left as they are, so names with
// accents, spaces, apostrophes or "#" reach the server exactly.
//
// Since the same evening (docs/08 item 107) the rules are the shared core's (play/core/address.c), the same
// the classic Play on the SE uses: these are thin wrappers. One rule changed in the move: after a probe, the
// address the answer came from is tried before the IPv4 address the server names for itself (which, inside a
// container, may be the container's), as the classic Play did.
import Foundation
import PlayCore

public enum IPFamily: String, Codable, CaseIterable, Identifiable {
    case ipv6, ipv4
    public var id: String { rawValue }
    public var label: String { self == .ipv6 ? "IPv6 first" : "IPv4 first" }
    public var other: IPFamily { self == .ipv6 ? .ipv4 : .ipv6 }
    var core: Int32 { self == .ipv6 ? Int32(A68_IPV6) : Int32(A68_IPV4) }
}

public enum Address {
    /// the music server's port when none is typed (the server's own default)
    public static let defaultPort = Int(A68_DEFAULT_PORT)
    /// the UDP port and multicast group the server answers "AURORA68-MUSIC?" on
    public static let discoveryPort = UInt16(A68_DISCOVERY_PORT)
    public static let discoveryGroup = A68_DISCOVERY_GROUP_TEXT
    public static let probe = A68_PROBE

    public static func isIPv6(_ host: String) -> Bool { a68_is_ipv6(host) != 0 }
    public static func isIPv4(_ host: String) -> Bool { a68_is_ipv4(host) != 0 }
    public static func isNumeric(_ host: String) -> Bool { isIPv4(host) || isIPv6(host) }

    public static func family(of host: String) -> IPFamily? {
        switch Int(a68_family(host)) {
        case Int(A68_IPV6): return .ipv6
        case Int(A68_IPV4): return .ipv4
        default: return nil
        }
    }

    /// a host as it goes into a URL: IPv6 in brackets (a zone's "%" written "%25")
    public static func urlHost(_ host: String) -> String {
        let size = host.utf8.count * 3 + 8
        var buf = [CChar](repeating: 0, count: size)
        buf.withUnsafeMutableBufferPointer { b in
            let end = a68_put_url_host(b.baseAddress!, b.baseAddress! + (size - 1), host)
            end?.pointee = 0
        }
        return String(cString: buf)
    }

    public static func baseURL(host: String, port: Int) -> URL? {
        builtLength(host.utf8.count * 3 + 32) { a68_url($0, $1, host, UInt32(clamping: port), nil) }.flatMap { URL(string: $0) }
    }

    /// what the owner typed, as (host, port): "192.168.1.20:8423", "[fd00::20]:8423", "fd00::20",
    /// "nas.local", "http://nas.local:8423/live" (scheme and path dropped); nil if it is no address
    public static func parse(_ typed: String, defaultPort: Int = Address.defaultPort) -> (host: String, port: Int)? {
        var host = [CChar](repeating: 0, count: Int(A68_HOST_MAX))
        var port: UInt32 = 0
        let ok = host.withUnsafeMutableBufferPointer { b in
            a68_parse_address(typed, UInt32(clamping: defaultPort), b.baseAddress!, b.count, &port)
        }
        return ok != 0 ? (String(cString: host), Int(port)) : nil
    }

    /// candidates ordered by the preference: the preferred family's addresses, then the other's, then names
    public static func ordered(_ hosts: [String], prefer: IPFamily) -> [String] {
        guard !hosts.isEmpty else { return [] }
        let copies = hosts.map { strdup($0) }
        defer { copies.forEach { free($0) } }
        let pointers: [UnsafePointer<CChar>?] = copies.map { $0.map { UnsafePointer($0) } }
        var order = [Int32](repeating: 0, count: hosts.count)
        let n = a68_order(pointers, Int32(hosts.count), prefer.core, &order)
        return order[0..<Int(n)].map { hosts[Int($0)] }
    }

    /// a library path for a URL: each segment percent-encoded as UTF-8, the slashes kept
    public static func encodePath(_ path: String) -> String { escaped(path, keepSlash: true) }

    public static func encodeQuery(_ value: String) -> String { escaped(value, keepSlash: false) }

    static func escaped(_ s: String, keepSlash: Bool) -> String {
        let size = s.utf8.count * 3 + 1
        var buf = [CChar](repeating: 0, count: size)
        buf.withUnsafeMutableBufferPointer { b in
            let end = a68_put_escaped(b.baseAddress!, b.baseAddress! + (size - 1), s, keepSlash ? 1 : 0)
            end?.pointee = 0
        }
        return String(cString: buf)
    }
}

/// the server's answer to "AURORA68-MUSIC?": "AURORA68-MUSIC <tab> 1 <tab> NAME <tab> PORT <tab> IPV4 <tab> IPV6",
/// in Mac Roman (it is written for the SE's Aurora68 Play as well); either address may be empty
public struct DiscoveryReply: Equatable, Hashable, Identifiable {
    public let name: String
    public let port: Int
    public let ipv4: String?
    public let ipv6: String?
    public var sender: String?                 // where the answer came from
    public var id: String { "\(name)|\(port)|\(ipv4 ?? "")|\(ipv6 ?? "")" }

    public init(name: String, port: Int, ipv4: String?, ipv6: String?, sender: String? = nil) {
        self.name = name; self.port = port; self.ipv4 = ipv4; self.ipv6 = ipv6; self.sender = sender
    }

    public static func parse(_ data: Data, sender: String? = nil) -> DiscoveryReply? {
        var r = a68_disc()
        let ok = data.withUnsafeBytes { raw in
            a68_disc_parse(raw.bindMemory(to: UInt8.self).baseAddress, raw.count, &r)
        }
        guard ok != 0 else { return nil }
        let name = withUnsafeBytes(of: r.name) { raw in
            String(bytes: raw.prefix(while: { $0 != 0 }), encoding: .macOSRoman) ?? ""
        }
        let v4 = text(tuple: r.v4), v6 = text(tuple: r.v6)
        return DiscoveryReply(name: name, port: Int(r.port), ipv4: v4.isEmpty ? nil : v4, ipv6: v6.isEmpty ? nil : v6, sender: sender)
    }

    /// the hosts to try, best first (play/core/address.c's a68_disc_hosts): the preferred family first; for IPv4
    /// the address the answer came from before the one the server names
    public func hosts(prefer: IPFamily) -> [String] {
        var r = a68_disc()
        setText(&r.v4, ipv4 ?? "")
        setText(&r.v6, ipv6 ?? "")
        r.port = UInt32(clamping: port)
        return withUnsafeMutablePointer(to: &r) { rp in      // the answers point into r and the sender: read while both live
            (sender ?? "").withCString { s in
                var out = [UnsafePointer<CChar>?](repeating: nil, count: 3)
                let n = a68_disc_hosts(rp, s, prefer.core, 1, &out)
                return out[0..<Int(n)].map { text($0) }
            }
        }
    }
}
