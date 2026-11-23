// SPDX-License-Identifier: MPL-2.0
//
// Models.swift - the music server's library and Live channel, as the macOS Aurora68 Play holds them.
//
// Backstory (22 November 2026). The music server (tools/music-server) began as stations for the SE's
// Aurora68 Play and grew a control panel, a Live channel and a library interface; the owner then asked
// for "a modern MacOS version of the Player app". The app first read the server's JSON (/api/catalog,
// /api/tracks, /api/now). The same evening the two Plays became one code base (docs/08 item 107) with one
// wire format, the /mac/ text the SE reads: the core (play/core/proto.c) takes the answers apart and these
// types are made from its records. The whole catalogue is every artist (/mac/artists) and every album
// (/mac/albums), page after page as the server sends them (a68_next_from), with full=1 for each album's
// folder (its cover is /art?dir=FOLDER, and "f:FOLDER" plays it on Live or another player).
//
// Music videos (22 November 2026, docs/08 item 109). The owner's library holds music videos beside the albums; the
// server lists them after an album's (or an artist's) tracks as "V" records when asked with videos=1, which this Play
// always asks. A video is a Track here with `video` set (how the server sends it, its size), so the queue, the player
// and the commands from other players take songs and videos alike: a video in the queue plays as a video (the
// server's /video/ID, or its HLS), then the queue goes on.
import Foundation
import PlayCore

/// the whole library
public struct Catalog: Equatable {
    public let name: String
    public let version: String
    public let artists: [Artist]
    public let albums: [Album]

    public init(name: String, version: String, artists: [Artist], albums: [Album]) {
        self.name = name; self.version = version; self.artists = artists; self.albums = albums
    }

    /// the catalogue from the answers: /mac/info, the pages of /mac/artists and of /mac/albums (all of them); an
    /// artist's albums are those that name its id, in the order the server sorts them (by title)
    public static func make(info: Data, artists: [Data], albums: [Data]) throws -> Catalog {
        guard let i = ServerInfo.parse(info) else { throw MusicAPIError.notText }
        var list: [Album] = []
        for page in albums {
            let r = Records(page)
            guard r.head() != nil else { throw MusicAPIError.notText }
            var a = a68_album()
            while a68_next_album(&r.rec, &a) != 0 {
                guard a.full != 0 else { throw MusicAPIError.olderServer }
                list.append(Album(id: text(a.id), title: text(a.title), artist: text(a.artist), artistID: text(a.artist_id),
                                  folder: text(a.folder), tracks: Int(a.tracks), videos: Int(a.videos)))
            }
        }
        var byArtist: [String: [String]] = [:]
        for al in list { byArtist[al.artistID, default: []].append(al.id) }
        var people: [Artist] = []
        for page in artists {
            let r = Records(page)
            guard r.head() != nil else { throw MusicAPIError.notText }
            var a = a68_artist()
            while a68_next_artist(&r.rec, &a) != 0 {
                let id = text(a.id)
                people.append(Artist(id: id, name: text(a.name), albums: byArtist[id] ?? [], videos: Int(a.videos)))
            }
        }
        return Catalog(name: i.name, version: i.version, artists: people, albums: list)
    }
}

public struct Artist: Identifiable, Hashable {
    public let id: String
    public let name: String
    public let albums: [String]          // album ids, in title order
    public let videos: Int               // its music videos (an artist may have videos and no album)

    public init(id: String, name: String, albums: [String], videos: Int = 0) {
        self.id = id; self.name = name; self.albums = albums; self.videos = videos
    }
}

public struct Album: Identifiable, Hashable {
    public let id: String
    public let title: String
    public let artist: String
    public let artistID: String
    public let folder: String            // its folder in the library: /art?dir=FOLDER is its cover
    public let tracks: Int
    public let videos: Int               // the music videos it lists, its own and its songs' (0 from an older server)

    public init(id: String, title: String, artist: String, artistID: String, folder: String, tracks: Int, videos: Int = 0) {
        self.id = id; self.title = title; self.artist = artist; self.artistID = artistID
        self.folder = folder; self.tracks = tracks; self.videos = videos
    }
}

/// how the server sends a music video: the file as it is (Range), remuxed or transcoded (as HLS for AVFoundation)
public enum VideoHow: String, Hashable {
    case direct, remux, transcode, unknown

    public init(_ s: String) { self = VideoHow(rawValue: s) ?? .unknown }

    var core: Int32 {
        switch self {
        case .direct: return Int32(A68_HOW_DIRECT)
        case .remux: return Int32(A68_HOW_REMUX)
        case .transcode: return Int32(A68_HOW_TRANSCODE)
        case .unknown: return Int32(A68_HOW_UNKNOWN)
        }
    }
}

/// what makes a Track a music video
public struct VideoInfo: Hashable {
    public let how: VideoHow
    public let width: Int
    public let height: Int

    public init(how: VideoHow, width: Int = 0, height: Int = 0) {
        self.how = how; self.width = width; self.height = height
    }

    /// the address on the server a Mac's player opens (the core's a68_path_video): the file, or HLS
    public func path(id: String) -> String {
        built(256) { a68_path_video(id, how.core, $0, $1) } ?? "/video/" + id
    }
}

/// /mac/tracks?album=ID or ?artist=ID, full=1
public struct Track: Identifiable, Hashable {
    public let id: String
    public let number: Int
    public let title: String
    public let artist: String
    public let seconds: Int
    public let path: String              // the file in the library: /file/PATH plays it as it is
    public let album: String
    public let albumTitle: String
    public let folder: String
    public let video: VideoInfo?         // a music video (nil: a song)
    public let linkedVideo: String?      // a song's music video, its id (the server links them by artist and title)

    public init(id: String, number: Int, title: String, artist: String, seconds: Int, path: String,
                album: String, albumTitle: String, folder: String, video: VideoInfo? = nil, linkedVideo: String? = nil) {
        self.id = id; self.number = number; self.title = title; self.artist = artist; self.seconds = seconds
        self.path = path; self.album = album; self.albumTitle = albumTitle; self.folder = folder; self.video = video
        self.linkedVideo = linkedVideo
    }

    public var isVideo: Bool { video != nil }
    /// a song that has a music video
    public var hasVideo: Bool { !isVideo && linkedVideo != nil }

    /// what the server's "what" names it by: t:PATH for a song, v:ID for a video
    public var what: String { isVideo ? LiveWhat.video(id) : LiveWhat.track(path) }

    /// a page of tracks, and the videos after them (videos=1); the next page's start, if there is one
    public static func page(_ data: Data) throws -> (tracks: [Track], next: Int?, videos: [Track]) {
        let r = Records(data)
        guard let head = r.head() else { throw MusicAPIError.notText }
        var out: [Track] = [], videos: [Track] = []
        var t = a68_track()
        var v = a68_video()
        while true {
            let k = a68_next_item(&r.rec, &t, &v)
            if k == 0 { break }
            if k == Int32(UInt8(ascii: "V")) {
                videos.append(Track.fromVideo(v))
                continue
            }
            guard t.full != 0 else { throw MusicAPIError.olderServer }
            let linked = text(t.video)                     // empty: no video (or a server before 23 November 2026)
            out.append(Track(id: text(t.id), number: Int(t.number), title: text(t.title), artist: text(t.artist),
                             seconds: Int(t.secs), path: text(t.path), album: text(t.album), albumTitle: text(t.album_title),
                             folder: text(t.folder), linkedVideo: linked.isEmpty ? nil : linked))
        }
        return (out, head.next, videos)
    }

    /// a "V" record as a Track that is a video; its number from its file's name, as a song's is
    static func fromVideo(_ v: a68_video) -> Track {
        let path = text(v.path), artist = text(v.artist)
        let number = path.isEmpty ? 0 : TrackName.parse(TrackName.stem(of: path), artist: artist).number
        return Track(id: text(v.id), number: number, title: text(v.title), artist: artist, seconds: Int(v.secs), path: path,
                     album: text(v.album), albumTitle: text(v.album_title), folder: text(v.folder),
                     video: VideoInfo(how: VideoHow(text(v.how)), width: Int(v.width), height: Int(v.height)))
    }

    /// /mac/videos: a page of videos alone
    public static func videoPage(_ data: Data) throws -> (videos: [Track], next: Int?) {
        let r = Records(data)
        guard let head = r.head() else { throw MusicAPIError.notText }
        var out: [Track] = []
        var v = a68_video()
        while a68_next_video(&r.rec, &v) != 0 { out.append(Track.fromVideo(v)) }
        return (out, head.next)
    }
}

/// Videos linked to songs (23 November 2026, docs/08 item 115). The server links a music video to the songs it is of
/// (tools/music-server/README.md, "Videos linked to songs") and says so in each song's record (Track.linkedVideo); an
/// album's videos include those of its songs. The owner asked to "link them to a song to alternate": with "Play Videos
/// Instead of Songs" on, a list played here becomes each song's video where it has one, the song where it has none.
public enum VideoLinks {
    /// videos by their ids, for finding a song's (the first of each id wins)
    public static func index(_ videos: [Track]) -> [String: Track] {
        var out: [String: Track] = [:]
        for v in videos where v.isVideo && out[v.id] == nil { out[v.id] = v }
        return out
    }

    /// the video of a song, if it has one and it is among the videos known
    public static func video(of song: Track, in videos: [String: Track]) -> Track? {
        guard song.hasVideo, let id = song.linkedVideo else { return nil }
        return videos[id]
    }

    /// the list with each song that has a video known replaced by it, one for one (so a place in the list stays the
    /// same place); videos and songs without one stay as they are
    public static func alternate(_ list: [Track], videos: [String: Track]) -> [Track] {
        list.map { video(of: $0, in: videos) ?? $0 }
    }
}

/// every music video of the library (/mac/videos, which the server sorts by artist), grouped by artist for the
/// Videos page; filtered as the artists and albums are (every word, case and accents ignored)
public struct VideoShelf: Equatable {
    public struct Group: Equatable, Identifiable {
        public let artist: String
        public let videos: [Track]
        public var id: String { artist }
    }
    public let groups: [Group]

    public init(_ videos: [Track], matching text: String = "") {
        let words = LibraryIndex.words(text)
        var groups: [Group] = []
        var seen = Set<String>()
        for v in videos where !seen.contains(v.id) {
            seen.insert(v.id)
            if !words.isEmpty && !LibraryIndex.matches(v.title + " " + v.artist + " " + v.albumTitle, words) { continue }
            if let last = groups.last, LibraryIndex.fold(last.artist) == LibraryIndex.fold(v.artist) {
                groups[groups.count - 1] = Group(artist: last.artist, videos: last.videos + [v])
            } else {
                groups.append(Group(artist: v.artist, videos: [v]))
            }
        }
        self.groups = groups
    }

    public var count: Int { groups.reduce(0) { $0 + $1.videos.count } }
}

/// /mac/now: what the Live channel plays (what is heard, not what was sent ahead)
public struct LiveNow: Equatable {
    public struct Current: Equatable {
        public let path: String
        public let folder: String
        public let title: String
        public let elapsed: Double
        public let length: Double
    }
    public struct Item: Equatable {
        public let path: String
        public let title: String
    }
    public struct Listener: Equatable {
        public let who: String
        public let since: Int
    }
    public let name: String
    public let playing: Bool
    public let paused: Bool
    public let `repeat`: Bool
    public let pos: Int
    public let changes: Int
    public let current: Current?
    public let queue: [Item]
    public let listeners: [Listener]

    public static func parse(_ data: Data) -> LiveNow? {
        let r = Records(data)
        guard let h = r.head(), h.kind == "now" else { return nil }
        var name = "", playing = false, paused = false, rep = false, pos = 0, changes = 0
        var current: Current?
        var queue: [Item] = [], listeners: [Listener] = []
        var o = a68_now()
        while a68_next_now(&r.rec, &o) != 0 {
            switch UInt8(truncatingIfNeeded: o.kind) {
            case UInt8(ascii: "N"):
                name = text(o.name); playing = o.playing != 0; paused = o.paused != 0; rep = o.`repeat` != 0
                pos = Int(o.pos); changes = Int(o.changes)
            case UInt8(ascii: "C"):
                current = Current(path: text(o.path), folder: text(o.folder), title: text(o.title),
                                  elapsed: Double(o.elapsed_ms) / 1000, length: Double(o.length_ms) / 1000)
            case UInt8(ascii: "Q"):
                queue.append(Item(path: text(o.path), title: text(o.title)))
            case UInt8(ascii: "L"):
                listeners.append(Listener(who: text(o.who), since: Int(o.since)))
            default:
                break
            }
        }
        return LiveNow(name: name, playing: playing, paused: paused, repeat: rep, pos: pos, changes: changes,
                       current: current, queue: queue, listeners: listeners)
    }
}

/// The catalogue indexed for the views: albums by id, an artist's albums, filtering by words. The filter folds
/// case and accents with Foundation's Unicode tables, which the C core has not: text at this Play's edge
public struct LibraryIndex {
    public let catalog: Catalog
    public let albumsByID: [String: Album]

    public init(_ catalog: Catalog) {
        self.catalog = catalog
        var byID: [String: Album] = [:]
        for a in catalog.albums { byID[a.id] = a }
        albumsByID = byID
    }

    public func albums(of artist: Artist) -> [Album] {
        artist.albums.compactMap { albumsByID[$0] }
    }

    /// the album whose cover stands for the artist: their first
    public func coverAlbum(of artist: Artist) -> Album? {
        artist.albums.first.flatMap { albumsByID[$0] }
    }

    /// every word must appear (case and accents ignored) in the artist's name or one of their albums
    public func artists(matching text: String) -> [Artist] {
        let words = LibraryIndex.words(text)
        if words.isEmpty { return catalog.artists }
        return catalog.artists.filter { ar in
            let hay = ([ar.name] + albums(of: ar).map(\.title)).joined(separator: " ")
            return LibraryIndex.matches(hay, words)
        }
    }

    public func albums(matching text: String) -> [Album] {
        let words = LibraryIndex.words(text)
        if words.isEmpty { return catalog.albums }
        return catalog.albums.filter { LibraryIndex.matches($0.title + " " + $0.artist, words) }
    }

    static func fold(_ s: String) -> String {
        s.folding(options: [.caseInsensitive, .diacriticInsensitive], locale: nil)
    }

    static func words(_ text: String) -> [String] {
        fold(text).split(whereSeparator: { $0.isWhitespace }).map(String.init)
    }

    static func matches(_ hay: String, _ words: [String]) -> Bool {
        let h = fold(hay)
        return words.allSatisfy { h.contains($0) }
    }
}
