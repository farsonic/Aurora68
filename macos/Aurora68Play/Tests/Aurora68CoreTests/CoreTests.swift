// SPDX-License-Identifier: MPL-2.0
//
// CoreTests.swift - Aurora68Core's rules, checked on every build (swift test, GitHub's macOS runner).
//
// Written on 22 November 2026 with the macOS Aurora68 Play: addresses as the owner types them (IPv6 in
// brackets, with and without a port), the discovery answer and the IPv6/IPv4 preference, file paths
// with accents and spaces in URLs, track names as libraries write them, the play queue's rules, and the
// server's JSON as tools/music-server sends it.
import XCTest
@testable import Aurora68Core

final class AddressTests: XCTestCase {
    func testParseTyped() {
        XCTAssertEqual(Address.parse("192.168.1.20:8423")?.host, "192.168.1.20")
        XCTAssertEqual(Address.parse("192.168.1.20:8423")?.port, 8423)
        XCTAssertEqual(Address.parse("192.168.1.20")?.port, Address.defaultPort)
        XCTAssertEqual(Address.parse("[fd00::20]:8423")?.host, "fd00::20")
        XCTAssertEqual(Address.parse("[fd00::20]:8423")?.port, 8423)
        XCTAssertEqual(Address.parse("fd00::20")?.host, "fd00::20")
        XCTAssertEqual(Address.parse("fd00::20")?.port, Address.defaultPort)
        XCTAssertEqual(Address.parse("http://nas.local:8423/live")?.host, "nas.local")
        XCTAssertEqual(Address.parse("http://[2001:db8::1]:8423/")?.host, "2001:db8::1")
        XCTAssertEqual(Address.parse("  nas  ")?.host, "nas")
        XCTAssertNil(Address.parse(""))
        XCTAssertNil(Address.parse("nas:99999"))
        XCTAssertNil(Address.parse("[fd00::20"))
    }

    func testURLsBracketIPv6() {
        XCTAssertEqual(Address.baseURL(host: "fd00::20", port: 8423)?.absoluteString, "http://[fd00::20]:8423")
        XCTAssertEqual(Address.baseURL(host: "[fd00::20]", port: 80)?.absoluteString, "http://[fd00::20]:80")
        XCTAssertEqual(Address.baseURL(host: "192.168.1.20", port: 8423)?.absoluteString, "http://192.168.1.20:8423")
        XCTAssertEqual(Address.urlHost("fe80::1%en0"), "[fe80::1%25en0]")
        XCTAssertTrue(Address.isIPv6("2401:db8:4f02::be24:11ff:fe3f:b27e"))
        XCTAssertFalse(Address.isIPv6("nas.local"))
        XCTAssertTrue(Address.isIPv4("192.168.1.20"))
        XCTAssertFalse(Address.isIPv4("192.168.1.256"))
        XCTAssertFalse(Address.isIPv4("1.2.3"))
    }

    func testPreferenceOrdering() {
        let hosts = ["192.168.1.20", "fd00::20", "nas.local", "192.168.1.20"]
        XCTAssertEqual(Address.ordered(hosts, prefer: .ipv6), ["fd00::20", "192.168.1.20", "nas.local"])
        XCTAssertEqual(Address.ordered(hosts, prefer: .ipv4), ["192.168.1.20", "fd00::20", "nas.local"])
    }

    func testDiscoveryReply() {
        let raw = "AURORA68-MUSIC\t1\tHome Music\t8423\t192.168.1.20\tfd00::20\r"
        let r = DiscoveryReply.parse(raw.data(using: .macOSRoman)!, sender: "192.168.1.20")
        XCTAssertEqual(r?.name, "Home Music")
        XCTAssertEqual(r?.port, 8423)
        XCTAssertEqual(r?.hosts(prefer: .ipv6), ["fd00::20", "192.168.1.20"])
        XCTAssertEqual(r?.hosts(prefer: .ipv4), ["192.168.1.20", "fd00::20"])
        let v4only = DiscoveryReply.parse("AURORA68-MUSIC\t1\tNAS\t8000\t192.168.1.20\t\r".data(using: .macOSRoman)!)
        XCTAssertNil(v4only?.ipv6)
        XCTAssertEqual(v4only?.hosts(prefer: .ipv6), ["192.168.1.20"])
        let caf = "AURORA68-MUSIC\t1\tCaf\u{E9}\t8000\t\t\r".data(using: .macOSRoman)!
        XCTAssertEqual(DiscoveryReply.parse(caf, sender: "10.0.0.5")?.name, "Caf\u{E9}")
        XCTAssertEqual(DiscoveryReply.parse(caf, sender: "10.0.0.5")?.hosts(prefer: .ipv6), ["10.0.0.5"])
        XCTAssertNil(DiscoveryReply.parse("HELLO\t1\tx\t8000".data(using: .utf8)!))
        XCTAssertNil(DiscoveryReply.parse("AURORA68-MUSIC\t1\tx\tport".data(using: .utf8)!))
    }

    func testPathsAndQueriesEncoded() {
        XCTAssertEqual(Address.encodePath("Bj\u{F6}rk - Post/01. Bj\u{F6}rk - Army of Me.m4a"),
                       "Bj%C3%B6rk%20-%20Post/01.%20Bj%C3%B6rk%20-%20Army%20of%20Me.m4a")
        XCTAssertEqual(Address.encodePath("Jane's Addiction - Strays/#1.mp3"), "Jane%27s%20Addiction%20-%20Strays/%231.mp3")
        let api = MusicAPI(base: URL(string: "http://[fd00::20]:8423")!)
        XCTAssertEqual(api.fileURL("A B/c.flac").absoluteString, "http://[fd00::20]:8423/file/A%20B/c.flac")
        XCTAssertEqual(api.artURL(folder: "Weezer - OK Human").absoluteString,
                       "http://[fd00::20]:8423/art?dir=Weezer%20-%20OK%20Human")
        XCTAssertEqual(api.liveURL.absoluteString, "http://[fd00::20]:8423/live")
    }
}

final class TrackNameTests: XCTestCase {
    func testNames() {
        XCTAssertEqual(TrackName.parse("01. Semisonic - Closing Time", artist: "Semisonic").title, "Closing Time")
        XCTAssertEqual(TrackName.parse("01. Semisonic - Closing Time", artist: "Semisonic").number, 1)
        XCTAssertEqual(TrackName.parse("07 Song Two").number, 7)
        XCTAssertEqual(TrackName.parse("07 Song Two").title, "Song Two")
        XCTAssertEqual(TrackName.parse("1-03 Third").number, 3)
        XCTAssertEqual(TrackName.parse("1-03 Third").title, "Third")
        XCTAssertEqual(TrackName.parse("12 - Twelve").title, "Twelve")
        XCTAssertEqual(TrackName.parse("2001 A Space Odyssey").number, 0)
        XCTAssertEqual(TrackName.parse("2001 A Space Odyssey").title, "2001 A Space Odyssey")
        XCTAssertEqual(TrackName.parse("Weezer - Buddy Holly", artist: "Weezer").title, "Buddy Holly")
        XCTAssertEqual(TrackName.stem(of: "Weezer - Pinkerton (Explicit)/03. Weezer - Across the Sea.m4a"),
                       "03. Weezer - Across the Sea")
        XCTAssertEqual(TrackName.time(187), "3:07")
        XCTAssertEqual(TrackName.time(3723), "1:02:03")
    }
}

final class PlayQueueTests: XCTestCase {
    func testPlayThroughAndRepeat() {
        var q = PlayQueue<Int>()
        q.replace(with: [1, 2, 3])
        XCTAssertEqual(q.current, 1)
        XCTAssertEqual(q.peekNext(), 2)
        XCTAssertEqual(q.advance(), 2)
        XCTAssertEqual(q.advance(), 3)
        XCTAssertNil(q.peekNext())
        XCTAssertNil(q.advance())
        q.replace(with: [1, 2], start: 1)
        q.repeatMode = .all
        XCTAssertEqual(q.peekNext(), 1)
        XCTAssertEqual(q.advance(), 1)
        q.repeatMode = .one
        XCTAssertEqual(q.peekNext(), 1)
        XCTAssertEqual(q.advance(), 1)
        XCTAssertEqual(q.skip(), 2)                   // Next is not held by repeat one
    }

    func testNextAddRemoveJump() {
        var q = PlayQueue<String>()
        q.append(["a", "b"])
        XCTAssertEqual(q.current, "a")
        q.playNext(["x"])
        XCTAssertEqual(q.items, ["a", "x", "b"])
        q.remove(at: 0)                               // the playing one goes: its follower plays
        XCTAssertEqual(q.current, "x")
        XCTAssertEqual(q.jump(to: 1), "b")
        q.remove(at: 1)                               // the last, playing: nothing plays
        XCTAssertNil(q.current)
        q.clear()
        XCTAssertTrue(q.isEmpty)
    }

    func testPrevious() {
        var q = PlayQueue<Int>()
        q.replace(with: [1, 2, 3], start: 2)
        let a = q.previous(elapsed: 10)
        XCTAssertTrue(a.restart)
        XCTAssertEqual(a.item, 3)
        let b = q.previous(elapsed: 1)
        XCTAssertFalse(b.restart)
        XCTAssertEqual(b.item, 2)
        XCTAssertEqual(q.current, 2)
    }

    func testShuffleKeepsThePlayingOne() {
        var q = PlayQueue<Int>()
        q.replace(with: Array(1...20), start: 4)
        var g = SeededGenerator(seed: 7)
        q.shuffleUpcoming(using: &g)
        XCTAssertEqual(q.current, 5)
        XCTAssertEqual(Array(q.items[0...4]), [1, 2, 3, 4, 5])
        XCTAssertEqual(Set(q.upcoming), Set(6...20))
        XCTAssertNotEqual(Array(q.upcoming), Array(6...20))
    }
}

final class ModelTests: XCTestCase {
    func testDecodeServerJSON() throws {
        let cat = """
        {"name": "Home Music", "version": "3",
         "artists": [{"id": "a1", "name": "The Cure", "albums": ["l1", "l2"]}, {"id": "a2", "name": "Weezer", "albums": ["l3"]}],
         "albums": [{"id": "l1", "title": "Disintegration", "artist": "The Cure", "artist_id": "a1", "folder": "The Cure - Disintegration", "tracks": 12},
                    {"id": "l2", "title": "Songs Of A Lost World", "artist": "The Cure", "artist_id": "a1", "folder": "The Cure - Songs Of A Lost World", "tracks": 8},
                    {"id": "l3", "title": "OK Human", "artist": "Weezer", "artist_id": "a2", "folder": "Weezer - OK Human", "tracks": 12}]}
        """
        let c = try JSONDecoder().decode(Catalog.self, from: Data(cat.utf8))
        let ix = LibraryIndex(c)
        XCTAssertEqual(ix.albums(of: c.artists[0]).map(\.title), ["Disintegration", "Songs Of A Lost World"])
        XCTAssertEqual(ix.coverAlbum(of: c.artists[1])?.folder, "Weezer - OK Human")
        XCTAssertEqual(ix.artists(matching: "lost").map(\.name), ["The Cure"])
        XCTAssertEqual(ix.albums(matching: "weezer human").map(\.id), ["l3"])
        XCTAssertEqual(ix.artists(matching: "").count, 2)

        let tr = """
        {"tracks": [{"id": "t1", "number": 1, "title": "Closing Time", "artist": "Semisonic", "seconds": 273,
                     "path": "Semisonic - Feeling Strangely Fine/01. Semisonic - Closing Time.m4a", "album": "l9",
                     "album_title": "Feeling Strangely Fine", "folder": "Semisonic - Feeling Strangely Fine"}]}
        """
        let t = try JSONDecoder().decode(TrackList.self, from: Data(tr.utf8)).tracks
        XCTAssertEqual(t.first?.albumTitle, "Feeling Strangely Fine")
        XCTAssertEqual(t.first?.seconds, 273)

        let now = """
        {"name": "Home Music", "playing": true, "paused": false, "repeat": false, "pos": 0, "changes": 3,
         "current": {"path": "a/b.m4a", "folder": "a", "title": "Gamma - Closing Time", "elapsed": 12.5, "length": 200.0},
         "queue": [{"path": "a/b.m4a", "title": "01. b"}], "listeners": [{"who": "fd00::68", "since": 1700000000}]}
        """
        let n = try JSONDecoder().decode(LiveNow.self, from: Data(now.utf8))
        XCTAssertEqual(n.current?.title, "Gamma - Closing Time")
        XCTAssertEqual(n.listeners.first?.who, "fd00::68")
        let idle = """
        {"name": "Home Music", "playing": false, "paused": false, "repeat": true, "pos": 0, "changes": 0,
         "current": null, "queue": [], "listeners": []}
        """
        let i = try JSONDecoder().decode(LiveNow.self, from: Data(idle.utf8))
        XCTAssertNil(i.current)
        XCTAssertTrue(i.`repeat`)
    }
}

/// a repeatable generator, so the shuffle test sees the same order each time
struct SeededGenerator: RandomNumberGenerator {
    var state: UInt64
    init(seed: UInt64) { state = seed &* 0x9E3779B97F4A7C15 | 1 }
    mutating func next() -> UInt64 {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17
        return state
    }
}
