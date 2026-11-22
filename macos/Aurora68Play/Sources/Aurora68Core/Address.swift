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
import Foundation

public enum IPFamily: String, Codable, CaseIterable, Identifiable {
    case ipv6, ipv4
    public var id: String { rawValue }
    public var label: String { self == .ipv6 ? "IPv6 first" : "IPv4 first" }
    public var other: IPFamily { self == .ipv6 ? .ipv4 : .ipv6 }
}

public enum Address {
    /// the music server's port when none is typed (the server's own default)
    public static let defaultPort = 8000
    /// the UDP port and multicast group the server answers "AURORA68-MUSIC?" on
    public static let discoveryPort: UInt16 = 8070
    public static let discoveryGroup = "239.255.68.70"
    public static let probe = "AURORA68-MUSIC?"

    public static func isIPv6(_ host: String) -> Bool {
        let h = host.trimmingCharacters(in: CharacterSet(charactersIn: "[]"))
        return h.contains(":") && h.allSatisfy { $0.isHexDigit || $0 == ":" || $0 == "." || $0 == "%" }
    }

    public static func isIPv4(_ host: String) -> Bool {
        let parts = host.split(separator: ".", omittingEmptySubsequences: false)
        return parts.count == 4 && parts.allSatisfy { p in
            !p.isEmpty && p.count <= 3 && p.allSatisfy(\.isNumber) && (Int(p) ?? 999) <= 255
        }
    }

    public static func isNumeric(_ host: String) -> Bool { isIPv4(host) || isIPv6(host) }

    public static func family(of host: String) -> IPFamily? {
        isIPv6(host) ? .ipv6 : isIPv4(host) ? .ipv4 : nil
    }

    /// a host as it goes into a URL: IPv6 in brackets (a zone's "%" written "%25")
    public static func urlHost(_ host: String) -> String {
        let h = host.trimmingCharacters(in: CharacterSet(charactersIn: "[]"))
        if isIPv6(h) { return "[" + h.replacingOccurrences(of: "%", with: "%25") + "]" }
        return h
    }

    public static func baseURL(host: String, port: Int) -> URL? {
        URL(string: "http://\(urlHost(host)):\(port)")
    }

    /// what the owner typed, as (host, port): "192.168.1.20:8423", "[fd00::20]:8423", "fd00::20",
    /// "nas.local", "http://nas.local:8423/live" (scheme and path dropped); nil if it is no address
    public static func parse(_ typed: String, defaultPort: Int = Address.defaultPort) -> (host: String, port: Int)? {
        var s = typed.trimmingCharacters(in: .whitespacesAndNewlines)
        for scheme in ["http://", "https://"] where s.lowercased().hasPrefix(scheme) {
            s = String(s.dropFirst(scheme.count))
        }
        var host = "", rest = Substring("")
        if s.hasPrefix("[") {
            guard let close = s.firstIndex(of: "]") else { return nil }
            host = String(s[s.index(after: s.startIndex)..<close])
            rest = s[s.index(after: close)...]
        } else if s.filter({ $0 == ":" }).count > 1 && !s.contains("/") {
            host = s                                   // a bare IPv6 address: no port can follow
        } else {
            let end = s.firstIndex(of: "/") ?? s.endIndex
            let hp = s[..<end]
            if let colon = hp.firstIndex(of: ":") {
                host = String(hp[..<colon]); rest = hp[colon...]
            } else {
                host = String(hp)
            }
        }
        var port = defaultPort
        if rest.hasPrefix(":") {
            let digits = rest.dropFirst().prefix(while: \.isNumber)
            guard let p = Int(digits), (1...65535).contains(p) else { return nil }
            port = p
        }
        guard !host.isEmpty, !host.contains(" ") else { return nil }
        return (host, port)
    }

    /// candidates ordered by the preference: the preferred family's addresses, then the other's
    public static func ordered(_ hosts: [String], prefer: IPFamily) -> [String] {
        var seen = Set<String>()
        let unique = hosts.filter { !$0.isEmpty && seen.insert($0).inserted }
        let first = unique.filter { family(of: $0) == prefer }
        let second = unique.filter { family(of: $0) == prefer.other }
        let names = unique.filter { family(of: $0) == nil }
        return first + second + names
    }

    private static let unreserved = CharacterSet(charactersIn:
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~")

    /// a library path for a URL: each segment percent-encoded as UTF-8, the slashes kept
    public static func encodePath(_ path: String) -> String {
        path.split(separator: "/", omittingEmptySubsequences: false)
            .map { String($0).addingPercentEncoding(withAllowedCharacters: unreserved) ?? "" }
            .joined(separator: "/")
    }

    public static func encodeQuery(_ value: String) -> String {
        value.addingPercentEncoding(withAllowedCharacters: unreserved) ?? ""
    }
}

/// the server's answer to "AURORA68-MUSIC?": "AURORA68-MUSIC <tab> 1 <tab> NAME <tab> PORT <tab> IPV4 <tab> IPV6",
/// in Mac Roman (it is written for the SE's Aurora68 Play as well); either address may be empty
public struct DiscoveryReply: Equatable, Hashable, Identifiable {
    public let name: String
    public let port: Int
    public let ipv4: String?
    public let ipv6: String?
    public var sender: String?                 // where the answer came from, if the addresses are missing
    public var id: String { "\(name)|\(port)|\(ipv4 ?? "")|\(ipv6 ?? "")" }

    public init(name: String, port: Int, ipv4: String?, ipv6: String?, sender: String? = nil) {
        self.name = name; self.port = port; self.ipv4 = ipv4; self.ipv6 = ipv6; self.sender = sender
    }

    public static func parse(_ data: Data, sender: String? = nil) -> DiscoveryReply? {
        guard let text = String(data: data, encoding: .macOSRoman) else { return nil }
        let f = text.trimmingCharacters(in: .whitespacesAndNewlines)
            .split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
        guard f.count >= 4, f[0] == "AURORA68-MUSIC", let port = Int(f[3]), (1...65535).contains(port) else { return nil }
        let v4 = f.count > 4 && Address.isIPv4(f[4]) ? f[4] : nil
        let v6 = f.count > 5 && Address.isIPv6(f[5]) ? f[5] : nil
        return DiscoveryReply(name: f[2], port: port, ipv4: v4, ipv6: v6, sender: sender)
    }

    /// the hosts to try, the preferred family first, the sender's address last
    public func hosts(prefer: IPFamily) -> [String] {
        Address.ordered([ipv4, ipv6, sender].compactMap { $0 }, prefer: prefer)
    }
}
