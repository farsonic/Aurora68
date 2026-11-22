// SPDX-License-Identifier: MPL-2.0
//
// MusicAPI.swift - the music server's HTTP interface, for the macOS Aurora68 Play.
//
// Backstory (22 November 2026). What the app asks of the server (tools/music-server/README.md):
//   GET  /api/catalog            the artists and albums
//   GET  /api/tracks?album=ID    an album's (or ?artist=ID an artist's) tracks, with titles, lengths, files
//   GET  /file/PATH              a file as it is, with Range: AVFoundation plays it and seeks in it
//   GET  /art?dir=FOLDER         an album's cover picture
//   GET  /api/now                what the Live channel plays (the SE listens to it)
//   POST /api/live               change what Live plays (JSON; the header X-Aurora: 1, which a web page
//                                from elsewhere cannot send, so no other site can change it)
//   GET  /live                   the Live channel itself, to listen to here
// Everything is plain HTTP on the home network; the base URL is the server's address as numbers, with
// IPv6 in brackets (Address.swift), chosen by the owner's preference.
import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

public enum MusicAPIError: Error, LocalizedError, Equatable {
    case status(Int, String)
    case notJSON

    public var errorDescription: String? {
        switch self {
        case .status(let code, let text): return text.isEmpty ? "The server answered \(code)." : text
        case .notJSON: return "The server's answer was not understood."
        }
    }
}

public final class MusicAPI {
    public let base: URL
    let session: URLSession

    public init(base: URL, session: URLSession = .shared) {
        self.base = base
        self.session = session
    }

    /// a session that gives up quickly: for trying an address while connecting
    public static func probing() -> URLSession {
        let c = URLSessionConfiguration.ephemeral
        c.timeoutIntervalForRequest = 4
        c.timeoutIntervalForResource = 8
        return URLSession(configuration: c)
    }

    public func url(_ path: String, query: [(String, String)] = []) -> URL {
        var s = base.absoluteString
        while s.hasSuffix("/") { s.removeLast() }
        s += path
        if !query.isEmpty {
            s += "?" + query.map { "\($0.0)=\(Address.encodeQuery($0.1))" }.joined(separator: "&")
        }
        return URL(string: s)!
    }

    public func fileURL(_ path: String) -> URL { url("/file/" + Address.encodePath(path)) }
    public func artURL(folder: String) -> URL { url("/art", query: [("dir", folder)]) }
    public var liveURL: URL { url("/live") }
    public var panelURL: URL { url("/") }

    func get<T: Decodable>(_ type: T.Type, _ url: URL) async throws -> T {
        var req = URLRequest(url: url)
        req.setValue("application/json", forHTTPHeaderField: "Accept")
        let (data, resp) = try await session.data(for: req)
        try MusicAPI.check(resp, data)
        do { return try JSONDecoder().decode(T.self, from: data) } catch { throw MusicAPIError.notJSON }
    }

    static func check(_ resp: URLResponse, _ data: Data) throws {
        guard let h = resp as? HTTPURLResponse else { return }
        guard (200..<300).contains(h.statusCode) else {
            let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
            throw MusicAPIError.status(h.statusCode, (obj?["error"] as? String) ?? "")
        }
    }

    public func catalog() async throws -> Catalog { try await get(Catalog.self, url("/api/catalog")) }

    public func tracks(album: String) async throws -> [Track] {
        try await get(TrackList.self, url("/api/tracks", query: [("album", album)])).tracks
    }

    public func tracks(artist: String) async throws -> [Track] {
        try await get(TrackList.self, url("/api/tracks", query: [("artist", artist)])).tracks
    }

    public func now() async throws -> LiveNow { try await get(LiveNow.self, url("/api/now")) }

    /// one change to the Live channel: ["action": "play", "what": "f:FOLDER", "shuffle": 1] and so on
    @discardableResult
    public func live(_ body: [String: Any]) async throws -> LiveNow {
        var req = URLRequest(url: url("/api/live"))
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.setValue("1", forHTTPHeaderField: "X-Aurora")
        req.httpBody = try JSONSerialization.data(withJSONObject: body)
        let (data, resp) = try await session.data(for: req)
        try MusicAPI.check(resp, data)
        do { return try JSONDecoder().decode(LiveNow.self, from: data) } catch { throw MusicAPIError.notJSON }
    }
}

/// what the Live channel is told to play: a whole folder (an album), or one file
public enum LiveWhat {
    public static func folder(_ folder: String) -> String { "f:" + folder }
    public static func track(_ path: String) -> String { "t:" + path }
}
