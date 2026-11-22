// SPDX-License-Identifier: MPL-2.0
//
// Devices.swift - players as devices, internet radio and the library's generation, as the server sends them.
//
// Backstory (22 November 2026). The owner asked, of the macOS Aurora68 Play: that it notice new albums
// without a restart; that Live offer stations to start, not an empty page; that "other devices running
// Aurora Player ... broadcast they are online for redirection"; and that a click on the song's position
// jump there. The music server gained (tools/music-server/README.md, "Devices"):
//   GET  /api/info                        the library's generation, bumped when a rescan finds a change
//   GET  /api/radio                       the internet radio stations every player offers
//   POST /api/devices/announce            "I am here" (every 20 seconds); the answer carries the generation
//   POST /api/devices/ID/state            what this player plays
//   GET  /api/devices                     every player, online or not, and the Live channel as "live"
//   POST /api/devices/ID/command          "play this there"
//   GET  /api/devices/ID/commands?wait=20 this player's commands, waiting up to 20 seconds for one
// These are those answers, and the rules that are only logic (an id kept in the preferences, a change of
// generation, a command as what the player does, a click on the scrubber as a time), tested with XCTest.
import Foundation

/// GET /api/info
public struct ServerInfo: Decodable, Equatable {
    public let name: String
    public let version: String
    public let library: Int
    public let every: Int?

    public init(name: String, version: String, library: Int, every: Int? = nil) {
        self.name = name; self.version = version; self.library = library; self.every = every
    }
}

/// POST /api/devices/announce's answer
public struct AnnounceReply: Decodable, Equatable {
    public let ok: Bool
    public let every: Int
    public let library: Int?
}

/// GET /api/radio: one station
public struct RadioStation: Decodable, Hashable, Identifiable {
    public let group: String
    public let name: String
    public let url: String
    public var id: String { group + "\u{1}" + name + "\u{1}" + url }

    public init(group: String, name: String, url: String) {
        self.group = group; self.name = name; self.url = url
    }
}

public struct RadioList: Decodable {
    public let stations: [RadioStation]

    public init(stations: [RadioStation]) { self.stations = stations }

    /// the stations by group, in the order the server gives them, each with its index (what "r:N" names)
    public var groups: [RadioGroup] {
        var out: [RadioGroup] = []
        for (i, s) in stations.enumerated() {
            let item = IndexedStation(index: i, station: s)
            if let last = out.indices.last, out[last].group == s.group {
                out[last].stations.append(item)
            } else {
                out.append(RadioGroup(group: s.group, stations: [item]))
            }
        }
        return out
    }
}

public struct IndexedStation: Identifiable, Equatable {
    public let index: Int
    public let station: RadioStation
    public var id: Int { index }
}

public struct RadioGroup: Identifiable, Equatable {
    public let group: String
    public var stations: [IndexedStation]
    public var id: String { group + "\u{1}\(stations.first?.index ?? 0)" }
}

/// what a player plays, as it posts it and as the list shows it
public struct DeviceState: Codable, Equatable {
    public var playing: Bool
    public var title: String
    public var artist: String
    public var album: String
    public var folder: String
    public var position: Double
    public var length: Double
    public var volume: Int
    public var at: Double?                 // the server's clock when it was posted (seconds since 1970)

    public init(playing: Bool = false, title: String = "", artist: String = "", album: String = "", folder: String = "",
                position: Double = 0, length: Double = 0, volume: Int = 100, at: Double? = nil) {
        self.playing = playing; self.title = title; self.artist = artist; self.album = album; self.folder = folder
        self.position = position; self.length = length; self.volume = volume; self.at = at
    }

    enum CodingKeys: String, CodingKey { case playing, title, artist, album, folder, position, length, volume, at }

    public init(from d: Decoder) throws {
        let c = try d.container(keyedBy: CodingKeys.self)
        playing = try c.decodeIfPresent(Bool.self, forKey: .playing) ?? false
        title = try c.decodeIfPresent(String.self, forKey: .title) ?? ""
        artist = try c.decodeIfPresent(String.self, forKey: .artist) ?? ""
        album = try c.decodeIfPresent(String.self, forKey: .album) ?? ""
        folder = try c.decodeIfPresent(String.self, forKey: .folder) ?? ""
        position = try c.decodeIfPresent(Double.self, forKey: .position) ?? 0
        length = try c.decodeIfPresent(Double.self, forKey: .length) ?? 0
        volume = try c.decodeIfPresent(Int.self, forKey: .volume) ?? 100
        at = try c.decodeIfPresent(Double.self, forKey: .at)
    }

    /// where the song is now: the posted position, moved on by the time since, while it plays
    public func estimatedPosition(at now: Double) -> Double {
        guard playing, let at else { return position }
        let p = position + max(0, now - at)
        return length > 0 ? min(p, length) : p
    }
}

/// GET /api/devices: one player (or the Live channel)
public struct Device: Decodable, Identifiable, Equatable {
    public let id: String
    public let name: String
    public let kind: String
    public let online: Bool
    public let lastSeen: Int
    public let state: DeviceState
    public let isLive: Bool

    enum CodingKeys: String, CodingKey { case id, name, kind, online, lastSeen = "last_seen", state, isLive = "is_live" }

    public init(id: String, name: String, kind: String, online: Bool, lastSeen: Int = 0, state: DeviceState = DeviceState(),
                isLive: Bool = false) {
        self.id = id; self.name = name; self.kind = kind; self.online = online; self.lastSeen = lastSeen
        self.state = state; self.isLive = isLive
    }

    public init(from d: Decoder) throws {
        let c = try d.container(keyedBy: CodingKeys.self)
        id = try c.decode(String.self, forKey: .id)
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? id
        kind = try c.decodeIfPresent(String.self, forKey: .kind) ?? ""
        online = try c.decodeIfPresent(Bool.self, forKey: .online) ?? false
        lastSeen = try c.decodeIfPresent(Int.self, forKey: .lastSeen) ?? 0
        state = try c.decodeIfPresent(DeviceState.self, forKey: .state) ?? DeviceState()
        isLive = try c.decodeIfPresent(Bool.self, forKey: .isLive) ?? false
    }

    /// the SF Symbol for its kind
    public var symbol: String {
        switch kind {
        case "live": return "dot.radiowaves.left.and.right"
        case "classic": return "desktopcomputer"
        case "browser": return "globe"
        default: return "laptopcomputer"
        }
    }
}

public struct DeviceList: Decodable {
    public let devices: [Device]
}

/// a command for this player (GET /api/devices/ID/commands)
public struct DeviceCommand: Decodable, Equatable {
    public struct Song: Decodable, Equatable {
        public let path: String
        public let tid: String
        public let title: String
        public let artist: String
        public let album: String
        public let seconds: Int

        public init(path: String, tid: String, title: String, artist: String, album: String, seconds: Int) {
            self.path = path; self.tid = tid; self.title = title; self.artist = artist; self.album = album; self.seconds = seconds
        }

        /// the song as the player's queue holds it (an album id is not needed: the cover comes by folder)
        public var track: Track {
            let folder = path.split(separator: "/").dropLast().joined(separator: "/")
            let name = TrackName.parse(TrackName.stem(of: path), artist: artist)
            return Track(id: tid, number: name.number, title: title.isEmpty ? name.title : title, artist: artist,
                         seconds: seconds, path: path, album: "", albumTitle: album, folder: folder)
        }
    }
    public struct Radio: Decodable, Equatable {
        public let name: String
        public let url: String
    }

    public let id: Int
    public let action: String
    public let tracks: [Song]
    public let radio: Radio?
    public let position: Double?
    public let volume: Int?
    public let shuffle: Int?

    enum CodingKeys: String, CodingKey { case id, action, tracks, radio, position, volume, shuffle }

    public init(id: Int, action: String, tracks: [Song] = [], radio: Radio? = nil, position: Double? = nil,
                volume: Int? = nil, shuffle: Int? = nil) {
        self.id = id; self.action = action; self.tracks = tracks; self.radio = radio
        self.position = position; self.volume = volume; self.shuffle = shuffle
    }

    public init(from d: Decoder) throws {
        let c = try d.container(keyedBy: CodingKeys.self)
        id = try c.decode(Int.self, forKey: .id)
        action = try c.decode(String.self, forKey: .action)
        tracks = try c.decodeIfPresent([Song].self, forKey: .tracks) ?? []
        radio = try c.decodeIfPresent(Radio.self, forKey: .radio)
        position = try c.decodeIfPresent(Double.self, forKey: .position)
        volume = try c.decodeIfPresent(Int.self, forKey: .volume)
        shuffle = try c.decodeIfPresent(Int.self, forKey: .shuffle)
    }

    /// what the player does for it (the server has already shuffled a list it was asked to shuffle)
    public var playerAction: PlayerAction? {
        switch action {
        case "play":
            if let r = radio, let u = URL(string: r.url) { return .radio(name: r.name, url: u) }
            return tracks.isEmpty ? nil : .play(tracks.map(\.track), from: position ?? 0)
        case "add": return tracks.isEmpty ? nil : .add(tracks.map(\.track))
        case "next": return tracks.isEmpty ? nil : .playNext(tracks.map(\.track))
        case "pause": return .pause
        case "resume": return .resume
        case "skip": return .skip
        case "previous": return .previous
        case "seek": return position.map { .seek($0) }
        case "volume": return volume.map { .volume(Double(max(0, min(100, $0))) / 100) }
        case "stop": return .stop
        default: return nil
        }
    }
}

public struct CommandList: Decodable {
    public let commands: [DeviceCommand]
}

public enum PlayerAction: Equatable {
    case play([Track], from: Double)
    case add([Track])
    case playNext([Track])
    case radio(name: String, url: URL)
    case pause, resume, skip, previous, stop
    case seek(Double)
    case volume(Double)                    // 0...1
}

/// this Mac as a device: an id kept in the preferences, made once
public enum DeviceIdentity {
    public static func isValid(_ id: String) -> Bool {
        !id.isEmpty && id.count <= 64 && id != "live" &&
            id.unicodeScalars.allSatisfy { CharacterSet.alphanumerics.contains($0) && $0.isASCII || $0 == "-" || $0 == "_" }
    }

    /// the kept id when it is a good one, else a new one ("mac-" and 12 hexadecimal digits)
    public static func id(kept: String?, make: () -> UUID = { UUID() }) -> String {
        if let kept, isValid(kept) { return kept }
        let hex = make().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        return "mac-" + hex.prefix(12)
    }
}

/// the library's generation: a change after the first one seen means "refresh the lists"
public struct GenerationWatch: Equatable {
    public private(set) var seen: Int?

    public init() {}

    /// true when g differs from the generation seen before (never for the first)
    public mutating func changed(_ g: Int?) -> Bool {
        guard let g else { return false }
        defer { seen = g }
        return seen != nil && seen != g
    }
}

/// a click or drag on the position bar as a time in the song
public enum Scrub {
    public static func time(x: Double, width: Double, duration: Double) -> Double {
        guard width > 0, duration > 0, x.isFinite else { return 0 }
        return min(max(x / width, 0), 1) * duration
    }

    /// the place of a time on the bar, 0...1
    public static func fraction(_ t: Double, duration: Double) -> Double {
        guard duration > 0, t.isFinite else { return 0 }
        return min(max(t / duration, 0), 1)
    }
}

/// what "what" the server takes names: everything, a folder, a song, a radio station
public extension LiveWhat {
    static var everything: String { "f:" }
    static func radio(_ index: Int) -> String { "r:\(index)" }
}
