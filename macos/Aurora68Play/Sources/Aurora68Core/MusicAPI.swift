// SPDX-License-Identifier: MPL-2.0
//
// MusicAPI.swift - the music server's HTTP interface, for the macOS Aurora68 Play.
//
// Backstory (22 November 2026). The app first asked the server for JSON (/api/catalog, /api/tracks, /api/now,
// POST /api/live, /api/devices...). The same evening the two Plays became one code base (docs/08 item 107), and
// with it one wire format: the /mac/ text the SE's Aurora68 Play reads, which the shared core takes apart
// (play/core/proto.c) and whose requests it writes. Two parameters make it this Play's: enc=utf8 (UTF-8 both
// ways; the SE's is Mac Roman) and full=1 (the longer records: an album's folder, a track's path, a device's
// whole state). What this file asks (tools/music-server/README.md):
//   GET /mac/info                   the name, the version, the counts, the generation, how often to announce
//   GET /mac/artists, /mac/albums   every artist and album, page after page (500 at a time, the server's most)
//   GET /mac/tracks?album=ID        an album's (or ?artist=ID an artist's) tracks, with the files' paths, then its
//                                   music videos (videos=1, 22 November 2026); /mac/videos?artist=ID an artist's videos
//   GET /mac/now                    what the Live channel plays (the SE listens to it)
//   GET /mac/command?to=ID&...      "play this there": on another player, or on "live" (its queue too)
//   GET /mac/radio, /mac/devices    the radio stations; every player
//   GET /mac/announce, /mac/state, /mac/commands   this Mac as a player (DeviceLink, the core's devices.c)
// and, not text: GET /file/PATH (a file as it is, with Range, for AVFoundation), GET /art?dir=FOLDER (a cover
// picture), GET /live (the Live channel to listen to here). Everything is plain HTTP on the home network; the
// base URL is the server's address as numbers, with IPv6 in brackets (Address.swift), chosen by the owner's
// preference. Requests that change something carry the header X-Aurora: 1 (a web page elsewhere cannot send
// it without asking first); it is sent with every request, as the SE does.
import Foundation
import PlayCore
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

public enum MusicAPIError: Error, LocalizedError, Equatable {
    case status(Int, String)
    case notText
    case olderServer

    public var errorDescription: String? {
        switch self {
        case .status(let code, let text): return text.isEmpty ? "The server answered \(code)." : text
        case .notText: return "The server's answer was not understood."
        case .olderServer:
            // a music server from before this app's text interface (22 November 2026): say what to do
            return "This music server is an older version, without the library this app uses. "
                + "Update it: docker compose pull music, then docker compose up -d --force-recreate music."
        }
    }
}

public final class MusicAPI {
    public let base: URL
    let session: URLSession
    /// the most records a page of the server's holds (MAC_PAGE_MAX)
    public static let page = 500

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

    /// a session for the long poll: the server holds the request up to 20 seconds
    public static func waiting() -> URLSession {
        let c = URLSessionConfiguration.ephemeral
        c.timeoutIntervalForRequest = 45
        c.timeoutIntervalForResource = 60
        return URLSession(configuration: c)
    }

    /// a path (already escaped, as the core writes them) on the server
    public func url(path: String) -> URL {
        var s = base.absoluteString
        while s.hasSuffix("/") { s.removeLast() }
        return URL(string: s + path)!
    }

    public func url(_ path: String, query: [(String, String)] = []) -> URL {
        var p = path
        if !query.isEmpty {
            p += "?" + query.map { "\($0.0)=\(Address.encodeQuery($0.1))" }.joined(separator: "&")
        }
        return url(path: p)
    }

    public func fileURL(_ path: String) -> URL { url("/file/" + Address.encodePath(path)) }
    /// what a Track plays from: a song's file as it is, a video's /video/ID (or its HLS, the core's choice)
    public func playURL(_ t: Track) -> URL {
        guard let v = t.video else { return fileURL(t.path) }
        return url(path: v.path(id: t.id))
    }
    /// a video's poster (a JPEG of the picture a tenth of the way in)
    public func posterURL(_ id: String) -> URL { url(path: built(256) { a68_path_vthumb(id, $0, $1) } ?? "/vthumb/" + id) }
    public func artURL(folder: String) -> URL { url("/art", query: [("dir", folder)]) }
    public var liveURL: URL { url("/live") }
    public var panelURL: URL { url("/") }

    /// one of the server's text answers: its status and its bytes
    public func text(_ path: String) async throws -> (status: Int, body: Data) {
        var req = URLRequest(url: url(path: path))
        req.setValue("1", forHTTPHeaderField: "X-Aurora")
        req.setValue("text/plain", forHTTPHeaderField: "Accept")
        let (data, resp) = try await session.data(for: req)
        return ((resp as? HTTPURLResponse)?.statusCode ?? 200, data)
    }

    /// the same, refused unless it is 200 (the server's words in the error)
    func ok(_ path: String) async throws -> Data {
        let (status, body) = try await text(path)
        guard status == 200 else { throw MusicAPIError.status(status, Records(body).errorText) }
        return body
    }

    /// every page of a list, from the first
    func pages(_ make: (Int) -> String) async throws -> [Data] {
        var out: [Data] = []
        var from = 0
        while true {
            let body = try await ok(make(from))
            out.append(body)
            guard let next = Records(body).head()?.next, next > from, out.count < 1000 else { break }
            from = next
        }
        return out
    }

    public func catalog() async throws -> Catalog {
        let info: Data
        do {
            info = try await ok(request { a68_path_info($0, $1, $2) })
        } catch MusicAPIError.status(404, _) {
            throw MusicAPIError.olderServer
        }
        let artists = try await pages { from in request { a68_path_artists($0, from, MusicAPI.page, $1, $2) } }
        let albums = try await pages { from in request { a68_path_albums($0, nil, from, MusicAPI.page, $1, $2) } }
        return try Catalog.make(info: info, artists: artists, albums: albums)
    }

    public func tracks(album: String) async throws -> [Track] {
        try await tracks { from in request { a68_path_tracks($0, album, nil, from, MusicAPI.page, $1, $2) } }
    }

    public func tracks(artist: String) async throws -> [Track] {
        try await tracks { from in request { a68_path_tracks($0, nil, artist, from, MusicAPI.page, $1, $2) } }
    }

    func tracks(_ make: (Int) -> String) async throws -> [Track] {
        try await media(make).tracks
    }

    /// an album's songs and its music videos, in one list from the server (the videos after the songs)
    public func media(album: String) async throws -> (tracks: [Track], videos: [Track]) {
        try await media { from in request { a68_path_tracks($0, album, nil, from, MusicAPI.page, $1, $2) } }
    }

    func media(_ make: (Int) -> String) async throws -> (tracks: [Track], videos: [Track]) {
        var tracks: [Track] = [], videos: [Track] = []
        for page in try await pages(make) {
            let p = try Track.page(page)
            tracks += p.tracks
            videos += p.videos
        }
        return (tracks, videos)
    }

    /// an artist's music videos (those in its albums and those beside them)
    public func videos(artist: String) async throws -> [Track] {
        var out: [Track] = []
        for page in try await pages({ from in request { a68_path_videos($0, artist, nil, from, MusicAPI.page, $1, $2) } }) {
            out += try Track.videoPage(page).videos
        }
        return out
    }

    public func now() async throws -> LiveNow {
        guard let n = LiveNow.parse(try await ok(request { a68_path_now($0, $1, $2) })) else { throw MusicAPIError.notText }
        return n
    }

    /// one change to the Live channel, then what it plays now
    @discardableResult
    public func live(_ c: Command) async throws -> LiveNow {
        try await command("live", c)
        return try await now()
    }

    public func info() async throws -> ServerInfo {
        guard let i = ServerInfo.parse(try await ok(request { a68_path_info($0, $1, $2) })) else { throw MusicAPIError.notText }
        return i
    }

    /// the internet radio stations every Aurora68 Play offers; a server without them: the classic Play's own list
    public func radio() async throws -> RadioList {
        let (status, body) = try await text(request { a68_path_radio($0, $1, $2) })
        if status == 404 { return RadioList.builtIn }
        guard status == 200 else { throw MusicAPIError.status(status, Records(body).errorText) }
        return RadioList.parse(body)
    }

    public func devices() async throws -> [Device] {
        Device.list(try await ok(request { a68_path_devices($0, $1, $2) }))
    }

    /// a command for another player (or "live"): refused in the server's words
    public func command(_ device: String, _ c: Command) async throws {
        _ = try await ok(c.path(to: device))
    }

    /// this player's commands (the path from DeviceSession, with its wait; use a session from waiting())
    public func commands(path: String) async throws -> (status: Int, commands: [DeviceCommand]) {
        let (status, body) = try await text(path)
        return (status, status == 200 ? DeviceCommand.list(body) : [])
    }
}

/// a station's address that is a playlist (.pls, .m3u: AVFoundation plays streams, not playlists): its first
/// stream, as the classic Play takes it (play/core/icy.c's icy_playlist_url)
public enum Playlist {
    /// does the address look like a playlist's? (.m3u8 is HLS, which AVFoundation plays itself)
    public static func isPlaylist(_ url: URL) -> Bool {
        let e = url.pathExtension.lowercased()
        return e == "pls" || e == "m3u"
    }

    /// the first http:// stream in a playlist's text; nil if none
    public static func firstStream(_ data: Data) -> URL? {
        var out = [CChar](repeating: 0, count: 1024)
        var https: Int32 = 0
        let found = data.withUnsafeBytes { raw -> Int32 in
            guard let p = raw.bindMemory(to: UInt8.self).baseAddress else { return 0 }
            return out.withUnsafeMutableBufferPointer { b in
                icy_playlist_url(p, UInt32(clamping: min(raw.count, Int(ICY_PLAYLIST))), b.baseAddress!, Int32(b.count), &https)
            }
        }
        return found != 0 ? URL(string: String(cString: out)) : nil
    }
}
