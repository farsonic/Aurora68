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
                                  folder: text(a.folder), tracks: Int(a.tracks)))
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
                people.append(Artist(id: id, name: text(a.name), albums: byArtist[id] ?? []))
            }
        }
        return Catalog(name: i.name, version: i.version, artists: people, albums: list)
    }
}

public struct Artist: Identifiable, Hashable {
    public let id: String
    public let name: String
    public let albums: [String]          // album ids, in title order

    public init(id: String, name: String, albums: [String]) {
        self.id = id; self.name = name; self.albums = albums
    }
}

public struct Album: Identifiable, Hashable {
    public let id: String
    public let title: String
    public let artist: String
    public let artistID: String
    public let folder: String            // its folder in the library: /art?dir=FOLDER is its cover
    public let tracks: Int

    public init(id: String, title: String, artist: String, artistID: String, folder: String, tracks: Int) {
        self.id = id; self.title = title; self.artist = artist; self.artistID = artistID
        self.folder = folder; self.tracks = tracks
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

    public init(id: String, number: Int, title: String, artist: String, seconds: Int, path: String,
                album: String, albumTitle: String, folder: String) {
        self.id = id; self.number = number; self.title = title; self.artist = artist; self.seconds = seconds
        self.path = path; self.album = album; self.albumTitle = albumTitle; self.folder = folder
    }

    /// a page of tracks; the next page's start, if there is one
    public static func page(_ data: Data) throws -> (tracks: [Track], next: Int?) {
        let r = Records(data)
        guard let head = r.head() else { throw MusicAPIError.notText }
        var out: [Track] = []
        var t = a68_track()
        while a68_next_track(&r.rec, &t) != 0 {
            guard t.full != 0 else { throw MusicAPIError.olderServer }
            out.append(Track(id: text(t.id), number: Int(t.number), title: text(t.title), artist: text(t.artist),
                             seconds: Int(t.secs), path: text(t.path), album: text(t.album), albumTitle: text(t.album_title),
                             folder: text(t.folder)))
        }
        return (out, head.next)
    }
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
