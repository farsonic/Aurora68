// SPDX-License-Identifier: MPL-2.0
//
// Devices.swift - players as devices, internet radio and the library's generation, as the server sends them.
//
// Backstory (22 November 2026). The owner asked, of the macOS Aurora68 Play: that it notice new albums
// without a restart; that Live offer stations to start, not an empty page; that "other devices running
// Aurora Player ... broadcast they are online for redirection"; and that a click on the song's position
// jump there. The music server gained players as devices (tools/music-server/README.md, "Devices"):
// announce, state, the list, a command to a player, its commands by long poll, the radio stations, the
// library's generation.
//
// Since the shared core (docs/08 item 107) both Plays speak the server's /mac/ text, and the rules are the
// core's: play/core/devices.c decides when to announce and when what plays is news (DeviceSession here),
// play/core/proto.c reads the devices, the commands and the stations and writes a command's request
// (Command), the id is made and checked by the core, and a click on the position bar becomes a time there.
// What stays Swift is what SwiftUI shows: these values, the symbol for a kind of player, the radio groups.
import Foundation
import PlayCore

/// /mac/info: the server's name, its version, the library's counts and generation
public struct ServerInfo: Equatable {
    public let name: String
    public let version: String
    public let library: Int
    public let every: Int?
    public let artists: Int
    public let albums: Int
    public let tracks: Int

    public init(name: String, version: String, library: Int, every: Int? = nil, artists: Int = 0, albums: Int = 0, tracks: Int = 0) {
        self.name = name; self.version = version; self.library = library; self.every = every
        self.artists = artists; self.albums = albums; self.tracks = tracks
    }

    public static func parse(_ data: Data) -> ServerInfo? {
        let r = Records(data)
        guard let h = r.head(), h.kind == "info" else { return nil }
        var i = a68_info()
        guard a68_info_of(&r.rec, &i) != 0 else { return nil }
        return ServerInfo(name: text(i.name), version: text(i.version), library: Int(i.generation),
                          every: i.every > 0 ? Int(i.every) : nil, artists: Int(i.artists), albums: Int(i.albums), tracks: Int(i.tracks))
    }
}

/// /mac/radio: one station
public struct RadioStation: Hashable, Identifiable {
    public let group: String
    public let name: String
    public let url: String
    public var id: String { group + "\u{1}" + name + "\u{1}" + url }

    public init(group: String, name: String, url: String) {
        self.group = group; self.name = name; self.url = url
    }
}

public struct RadioList: Equatable {
    public let stations: [RadioStation]

    public init(stations: [RadioStation]) { self.stations = stations }

    /// the server's list (/mac/radio: group, name, url a line)
    public static func parse(_ data: Data) -> RadioList {
        let r = Records(data)
        var s = a68_radio()
        var out: [RadioStation] = []
        while a68_next_radio(&r.rec, &s) != 0 {
            out.append(RadioStation(group: text(s.group), name: text(s.name), url: text(s.url)))
        }
        return RadioList(stations: out)
    }

    /// the classic Play's built-in list itself (play/core/stations.c, read by its own parser): for a server from
    /// before /mac/radio, so every Play still offers the same stations
    public static var builtIn: RadioList {
        var len = 0
        guard let t = stations_default_text(&len) else { return RadioList(stations: []) }
        let l = UnsafeMutablePointer<stations>.allocate(capacity: 1)
        defer { l.deallocate() }
        stations_parse(l, t, len)
        let mac = { (p: UnsafePointer<CChar>) -> String in
            String(bytes: UnsafeRawBufferPointer(start: p, count: strlen(p)), encoding: .macOSRoman) ?? ""
        }
        var groups: [String] = []
        withUnsafeBytes(of: &l.pointee.group) { raw in
            let g = raw.baseAddress!.assumingMemoryBound(to: sgroup.self)     // a C array of structs: a tuple of them
            for i in 0..<Int(l.pointee.ngroups) {
                var name = withUnsafeBytes(of: g[i].name) { mac($0.baseAddress!.assumingMemoryBound(to: CChar.self)) }
                if g[i].parent >= 0 { name = groups[Int(g[i].parent)] + "/" + name }
                groups.append(name)
            }
        }
        var out: [RadioStation] = []
        withUnsafeBytes(of: &l.pointee.station) { raw in
            let st = raw.baseAddress!.assumingMemoryBound(to: station.self)
            for i in 0..<Int(l.pointee.nstations) {
                let name = withUnsafeBytes(of: st[i].name) { mac($0.baseAddress!.assumingMemoryBound(to: CChar.self)) }
                let url = withUnsafeBytes(of: st[i].url) { mac($0.baseAddress!.assumingMemoryBound(to: CChar.self)) }
                out.append(RadioStation(group: st[i].group >= 0 ? groups[Int(st[i].group)] : "", name: name, url: url))
            }
        }
        return RadioList(stations: out)
    }

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

/// what a player plays, as it reports it and as the list shows it
public struct DeviceState: Equatable {
    public var playing: Bool
    public var title: String
    public var artist: String
    public var album: String
    public var folder: String
    public var position: Double
    public var length: Double
    public var volume: Int
    public var at: Double?                 // the server's clock when it was reported (seconds since 1970)
    public var track: String               // a library track's id: the server names it from its catalogue

    public init(playing: Bool = false, title: String = "", artist: String = "", album: String = "", folder: String = "",
                position: Double = 0, length: Double = 0, volume: Int = 100, at: Double? = nil, track: String = "") {
        self.playing = playing; self.title = title; self.artist = artist; self.album = album; self.folder = folder
        self.position = position; self.length = length; self.volume = volume; self.at = at; self.track = track
    }

    /// where the song is now: the reported position, moved on by the time since, while it plays (the core's rule)
    public func estimatedPosition(at now: Double) -> Double {
        guard let at, now.isFinite, at.isFinite, at > 0 else { return position }
        let ms = a68_estimate_ms(UInt(max(0, position) * 1000), UInt(max(0, length) * 1000), playing ? 1 : 0,
                                 UInt(at), UInt(max(0, now)))
        return Double(ms) / 1000
    }
}

/// /mac/devices?full=1: one player (or the Live channel)
public struct Device: Identifiable, Equatable {
    public let id: String
    public let name: String
    public let kind: String
    public let online: Bool
    public let lastSeen: Int
    public let state: DeviceState
    public let isLive: Bool

    public init(id: String, name: String, kind: String, online: Bool, lastSeen: Int = 0, state: DeviceState = DeviceState(),
                isLive: Bool = false) {
        self.id = id; self.name = name; self.kind = kind; self.online = online; self.lastSeen = lastSeen
        self.state = state; self.isLive = isLive
    }

    public static func list(_ data: Data) -> [Device] {
        let r = Records(data)
        var d = a68_device()
        var out: [Device] = []
        while a68_next_device(&r.rec, &d) != 0 {
            let st = DeviceState(playing: d.playing != 0, title: text(d.title), artist: text(d.artist), album: text(d.album),
                                 folder: text(d.folder), position: Double(d.position_ms) / 1000, length: Double(d.length_ms) / 1000,
                                 volume: Int(d.volume), at: d.at > 0 ? Double(d.at) : nil)
            let name = text(d.name)
            out.append(Device(id: text(d.id), name: name.isEmpty ? text(d.id) : name, kind: text(d.kind), online: d.online != 0,
                              lastSeen: Int(d.last_seen), state: st, isLive: d.is_live != 0 || text(d.kind) == "live"))
        }
        return out
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

/// a command for this player (/mac/commands?full=1: a command line, then its tracks and its station)
public struct DeviceCommand: Equatable {
    public struct Song: Equatable {
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
    public struct Radio: Equatable {
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

    public init(id: Int, action: String, tracks: [Song] = [], radio: Radio? = nil, position: Double? = nil,
                volume: Int? = nil, shuffle: Int? = nil) {
        self.id = id; self.action = action; self.tracks = tracks; self.radio = radio
        self.position = position; self.volume = volume; self.shuffle = shuffle
    }

    /// the commands of an answer, each with the tracks and the station after its line
    public static func list(_ data: Data) -> [DeviceCommand] {
        let r = Records(data)
        var c = a68_cmd()
        var out: [DeviceCommand] = []
        var cur: (id: Int, action: String, position: Double?, volume: Int?, shuffle: Int?)?
        var songs: [Song] = []
        var radio: Radio?
        func finish() {
            if let x = cur {
                out.append(DeviceCommand(id: x.id, action: x.action, tracks: songs, radio: radio, position: x.position,
                                         volume: x.volume, shuffle: x.shuffle))
            }
            songs = []; radio = nil; cur = nil
        }
        while a68_next_cmd(&r.rec, &c) != 0 {
            switch UInt8(truncatingIfNeeded: c.kind) {
            case UInt8(ascii: "C"):
                finish()
                let act = text(c.action)
                cur = (Int(c.n), act, act == "seek" || c.position > 0 ? Double(c.position) : nil,
                       c.volume >= 0 ? Int(c.volume) : nil, c.shuffle != 0 ? 1 : nil)
            case UInt8(ascii: "T"):
                songs.append(Song(path: text(c.path), tid: text(c.tid), title: text(c.title), artist: text(c.artist),
                                  album: text(c.album), seconds: Int(c.secs)))
            case UInt8(ascii: "R"):
                radio = Radio(name: text(c.name), url: text(c.url))
            default:
                break
            }
        }
        finish()
        return out
    }

    /// what the player does for it (the server has already shuffled a list it was asked to shuffle)
    public var playerAction: PlayerAction? {
        switch Int(a68_action(action)) {
        case Int(A68_ACT_PLAY):
            if let r = radio, let u = URL(string: r.url) { return .radio(name: r.name, url: u) }
            return tracks.isEmpty ? nil : .play(tracks.map(\.track), from: position ?? 0)
        case Int(A68_ACT_ADD): return tracks.isEmpty ? nil : .add(tracks.map(\.track))
        case Int(A68_ACT_NEXT): return tracks.isEmpty ? nil : .playNext(tracks.map(\.track))
        case Int(A68_ACT_PAUSE): return .pause
        case Int(A68_ACT_RESUME): return .resume
        case Int(A68_ACT_SKIP): return .skip
        case Int(A68_ACT_PREVIOUS): return .previous
        case Int(A68_ACT_SEEK): return position.map { .seek($0) }
        case Int(A68_ACT_VOLUME): return volume.map { .volume(Double(max(0, min(100, $0))) / 100) }
        case Int(A68_ACT_STOP): return .stop
        default: return nil
        }
    }
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

/// a command sent to another player, or to "live" (/mac/command): the core writes its request
public struct Command: Equatable {
    public enum Action: String {
        case play, add, next, pause, resume, skip, previous, seek, volume, stop
        case jump, remove, clear, shuffle, `repeat`            // the Live channel's queue
    }
    public var action: Action
    public var what: String?
    public var shuffle: Bool
    public var position: Double?
    public var volume: Int?
    public var index: Int?
    public var on: Bool?

    public init(_ action: Action, what: String? = nil, shuffle: Bool = false, position: Double? = nil, volume: Int? = nil,
                index: Int? = nil, on: Bool? = nil) {
        self.action = action; self.what = what; self.shuffle = shuffle; self.position = position; self.volume = volume
        self.index = index; self.on = on
    }

    /// its request to the device `to`
    public func path(to device: String) -> String {
        let ms = position.map { $0.isFinite ? Int(max(0, $0) * 1000) : 0 } ?? -1
        let act = a68_action(action.rawValue)
        let vol = Int32(clamping: volume ?? -1), at = index ?? -1, onOff: Int32 = on.map { $0 ? 1 : 0 } ?? -1
        let mix: Int32 = shuffle ? 1 : 0
        return request { c, out, size in
            guard let what else { return a68_path_command(c, device, act, nil, mix, ms, vol, at, onOff, out, size) }
            return what.withCString { w in a68_path_command(c, device, act, w, mix, ms, vol, at, onOff, out, size) }
        }
    }
}

/// this Mac as a device: an id kept in the preferences, made once
public enum DeviceIdentity {
    public static func isValid(_ id: String) -> Bool { dv_valid_id(id) != 0 }

    /// the kept id when it is a good one, else a new one ("mac-" and 12 letters and digits, the core's
    /// dv_make_id, seeded from a UUID's bytes)
    public static func id(kept: String?, make: () -> UUID = { UUID() }) -> String {
        if let kept, isValid(kept) { return kept }
        let u = make().uuid
        let a = UInt(u.0) << 24 | UInt(u.1) << 16 | UInt(u.2) << 8 | UInt(u.3)
        let b = UInt(u.4) << 24 | UInt(u.5) << 16 | UInt(u.6) << 8 | UInt(u.7)
        return built(20) { out, _ in dv_make_id(out, a ^ UInt(u.8) << 8 ^ UInt(u.9), b ^ UInt(u.10) << 16 ^ UInt(u.15)); return 1 } ?? "mac-000000000000"
    }
}

/// the library's generation: a change after the first one seen means "refresh the lists" (the core's a68_gen)
public struct GenerationWatch: Equatable {
    private var w = a68_gen()

    public init() {}

    public var seen: Int? { w.seen > 0 ? Int(w.seen) : nil }

    public static func == (a: GenerationWatch, b: GenerationWatch) -> Bool { a.w.seen == b.w.seen }

    /// true when g differs from the generation seen before (never for the first)
    public mutating func changed(_ g: Int?) -> Bool {
        guard let g, g > 0 else { return false }
        return a68_gen_changed(&w, UInt(g)) != 0
    }
}

/// a click or drag on the position bar as a time in the song (the core's a68_bar_time, in thousandths)
public enum Scrub {
    public static func time(x: Double, width: Double, duration: Double) -> Double {
        guard width > 0, duration > 0, x.isFinite, width.isFinite, duration.isFinite else { return 0 }
        let ms = a68_bar_time(Int(min(max(x, -1), width + 1) * 1000), Int(width * 1000), UInt(duration * 1000))
        return Double(ms) / 1000
    }

    /// the place of a time on the bar, 0...1
    public static func fraction(_ t: Double, duration: Double) -> Double {
        guard duration > 0, t.isFinite else { return 0 }
        return min(max(t / duration, 0), 1)
    }
}

/// this Mac as one of the server's devices: play/core/devices.c's rules (when to announce, when what plays is
/// news, what to do with an answer) over the paths it writes. DeviceLink makes the requests, all from the main
/// actor, so the core's struct is only ever touched by one at a time.
public final class DeviceSession {
    private let d: UnsafeMutablePointer<dv>
    private let start = Date()

    public init(id: String, name: String, version: String) {
        d = .allocate(capacity: 1)
        dv_init(d, id, name, version, "macos")
        d.pointee.client = client                // UTF-8, the whole state
        d.pointee.wait = 20                      // URLSession waits longer than MacTCP: the server's longest hold
        d.pointee.refresh = 5                    // what plays, every five seconds while it plays (for the others' bars)
    }

    deinit { d.deallocate() }

    /// the caller's clock: whole seconds since this session began
    public var now: UInt { UInt(max(0, Date().timeIntervalSince(start))) }

    public func begin() { dv_start(d, now) }
    public func end() { dv_stop(d) }

    public var announced: Bool { d.pointee.announced != 0 }
    public var every: Int { Int(d.pointee.every) }
    public var generation: Int? { d.pointee.gen > 0 ? Int(d.pointee.gen) : nil }
    /// the generation moved since it was last asked (read once)
    public func takeGenerationChange() -> Bool {
        defer { d.pointee.gen_changed = 0 }
        return d.pointee.gen_changed != 0
    }

    public enum Request: Int32 { case announce = 1, state = 2, commands = 3 }

    public func path(_ what: Request) -> String {
        built(1024) { dv_path(d, what.rawValue, $0, $1) } ?? "/"
    }

    /// a request's answer: status 0 when there was none; true if it was taken
    @discardableResult
    public func answer(_ what: Request, status: Int, body: Data?) -> Bool {
        let n = now
        guard let body else { return dv_answer(d, what.rawValue, Int32(clamping: status), nil, 0, n) != 0 }
        let r = Records(body)
        return dv_answer(d, what.rawValue, Int32(clamping: status), r.buf, r.count, n) != 0
    }

    /// what plays now; true if it is news (a report is due)
    @discardableResult
    public func setState(_ s: DeviceState) -> Bool {
        var st = dv_state()
        st.playing = s.playing ? 1 : 0
        st.pos = UInt(max(0, s.position.isFinite ? s.position : 0))
        st.len = UInt(max(0, s.length.isFinite ? s.length : 0))
        st.vol = Int32(max(0, min(100, s.volume)))
        setText(&st.tid, s.track)
        setText(&st.title, s.title)
        setText(&st.artist, s.artist)
        let n = now
        dv_set_state(d, &st, n)
        return dv_state_due(d, n) != 0
    }
}

/// what "what" the server takes names: everything, a folder, a song, a radio station (the core writes them)
public enum LiveWhat {
    public static func folder(_ folder: String) -> String { built(folder.utf8.count + 8) { a68_what_folder(folder, $0, $1) } ?? "f:" }
    public static func track(_ path: String) -> String { built(path.utf8.count + 8) { a68_what_track(path, $0, $1) } ?? "t:" }
    public static var everything: String { folder("") }
    public static func radio(_ index: Int) -> String { built(24) { a68_what_radio(UInt(max(0, index)), $0, $1) } ?? "r:0" }
}
