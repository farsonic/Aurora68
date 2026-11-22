// SPDX-License-Identifier: MPL-2.0
//
// Models.swift - the music server's library and Live channel, as the macOS Aurora68 Play reads them.
//
// Backstory (22 November 2026). The music server (tools/music-server) began as stations for the SE's
// Aurora68 Play and grew a control panel, a Live channel and a library interface; the owner then asked
// for "a modern MacOS version of the Player app". A modern Mac reads JSON without effort, so the server
// gained /api/catalog (every artist and album at once) and /api/tracks (an album's or artist's tracks
// with their titles, lengths and files); these types are those answers, and /api/now's.
import Foundation

/// GET /api/catalog: the whole library
public struct Catalog: Decodable, Equatable {
    public let name: String
    public let version: String
    public let artists: [Artist]
    public let albums: [Album]

    public init(name: String, version: String, artists: [Artist], albums: [Album]) {
        self.name = name; self.version = version; self.artists = artists; self.albums = albums
    }
}

public struct Artist: Decodable, Identifiable, Hashable {
    public let id: String
    public let name: String
    public let albums: [String]          // album ids, in title order

    public init(id: String, name: String, albums: [String]) {
        self.id = id; self.name = name; self.albums = albums
    }
}

public struct Album: Decodable, Identifiable, Hashable {
    public let id: String
    public let title: String
    public let artist: String
    public let artistID: String
    public let folder: String            // its folder in the library: /art?dir=FOLDER is its cover
    public let tracks: Int

    enum CodingKeys: String, CodingKey { case id, title, artist, artistID = "artist_id", folder, tracks }

    public init(id: String, title: String, artist: String, artistID: String, folder: String, tracks: Int) {
        self.id = id; self.title = title; self.artist = artist; self.artistID = artistID
        self.folder = folder; self.tracks = tracks
    }
}

/// GET /api/tracks?album=ID or ?artist=ID
public struct Track: Decodable, Identifiable, Hashable {
    public let id: String
    public let number: Int
    public let title: String
    public let artist: String
    public let seconds: Int
    public let path: String              // the file in the library: /file/PATH plays it as it is
    public let album: String
    public let albumTitle: String
    public let folder: String

    enum CodingKeys: String, CodingKey {
        case id, number, title, artist, seconds, path, album, albumTitle = "album_title", folder
    }

    public init(id: String, number: Int, title: String, artist: String, seconds: Int, path: String,
                album: String, albumTitle: String, folder: String) {
        self.id = id; self.number = number; self.title = title; self.artist = artist; self.seconds = seconds
        self.path = path; self.album = album; self.albumTitle = albumTitle; self.folder = folder
    }
}

public struct TrackList: Decodable {
    public let tracks: [Track]
}

/// GET /api/now: what the Live channel plays (what is heard, not what was sent ahead)
public struct LiveNow: Decodable, Equatable {
    public struct Current: Decodable, Equatable {
        public let path: String
        public let folder: String
        public let title: String
        public let elapsed: Double
        public let length: Double
    }
    public struct Item: Decodable, Equatable {
        public let path: String
        public let title: String
    }
    public struct Listener: Decodable, Equatable {
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
}

/// The catalogue indexed for the views: albums by id, an artist's albums, filtering by words
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
