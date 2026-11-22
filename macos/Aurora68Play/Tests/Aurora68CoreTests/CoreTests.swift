// SPDX-License-Identifier: MPL-2.0
//
// CoreTests.swift - Aurora68Core's rules, checked on every build (swift test, GitHub's macOS runner).
//
// Written on 22 November 2026 with the macOS Aurora68 Play: addresses as the owner types them (IPv6 in
// brackets, with and without a port), the discovery answer and the IPv6/IPv4 preference, file paths
// with accents and spaces in URLs, track names as libraries write them, the play queue's rules. Since the
// same evening (docs/08 item 107) these rules are the shared C core's (play/core), which the classic Play on the
// SE compiles too, and the server speaks to both in its /mac/ text: so these tests check the Swift wrappers
// over the core, and the core's reading of the server's answers written here as tools/music-server writes
// them (enc=utf8&full=1). tools/play_core_test.c checks the same core on Linux and as a 68020 program.
import XCTest
import PlayCore
@testable import Aurora68Core

/// an answer as the server writes it: Return-ended lines, tab-separated fields
func answer(_ lines: [String]) -> Data { Data((lines.joined(separator: "\r") + "\r").utf8) }

final class AddressTests: XCTestCase {
    func testParseTyped() {
        XCTAssertEqual(Address.parse("192.168.1.20:8423")?.host, "192.168.1.20")
        XCTAssertEqual(Address.parse("192.168.1.20:8423")?.port, 8423)
        XCTAssertEqual(Address.parse("192.168.1.20")?.port, Address.defaultPort)
        XCTAssertEqual(Address.defaultPort, 8000)
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
        XCTAssertTrue(Address.isIPv6("2001:db8:4f02::21a:2bff:fe3c:4d5e"))
        XCTAssertFalse(Address.isIPv6("nas.local"))
        XCTAssertTrue(Address.isIPv4("192.168.1.20"))
        XCTAssertFalse(Address.isIPv4("192.168.1.256"))
        XCTAssertFalse(Address.isIPv4("1.2.3"))
        XCTAssertEqual(Address.family(of: "fd00::20"), .ipv6)
        XCTAssertNil(Address.family(of: "nas.local"))
    }

    func testPreferenceOrdering() {
        let hosts = ["192.168.1.20", "fd00::20", "nas.local", "192.168.1.20"]
        XCTAssertEqual(Address.ordered(hosts, prefer: .ipv6), ["fd00::20", "192.168.1.20", "nas.local"])
        XCTAssertEqual(Address.ordered(hosts, prefer: .ipv4), ["192.168.1.20", "fd00::20", "nas.local"])
        XCTAssertEqual(Address.ordered([], prefer: .ipv4), [])
    }

    func testDiscoveryReply() {
        let raw = "AURORA68-MUSIC\t1\tHome Music\t8423\t192.168.1.20\tfd00::20\r"
        let r = DiscoveryReply.parse(raw.data(using: .macOSRoman)!, sender: "192.168.1.20")
        XCTAssertEqual(r?.name, "Home Music")
        XCTAssertEqual(r?.port, 8423)
        XCTAssertEqual(r?.hosts(prefer: .ipv6), ["fd00::20", "192.168.1.20"])
        XCTAssertEqual(r?.hosts(prefer: .ipv4), ["192.168.1.20", "fd00::20"])
        // the server in a container names its container's address: the one the answer came from goes first
        let boxed = DiscoveryReply.parse(raw.data(using: .macOSRoman)!, sender: "10.0.0.7")
        XCTAssertEqual(boxed?.hosts(prefer: .ipv4), ["10.0.0.7", "192.168.1.20", "fd00::20"])
        let v4only = DiscoveryReply.parse("AURORA68-MUSIC\t1\tNAS\t8000\t192.168.1.20\t\r".data(using: .macOSRoman)!)
        XCTAssertNil(v4only?.ipv6)
        XCTAssertEqual(v4only?.hosts(prefer: .ipv6), ["192.168.1.20"])
        let caf = "AURORA68-MUSIC\t1\tCaf\u{E9}\t8000\t\t\r".data(using: .macOSRoman)!
        XCTAssertEqual(DiscoveryReply.parse(caf, sender: "10.0.0.5")?.name, "Caf\u{E9}")
        XCTAssertEqual(DiscoveryReply.parse(caf, sender: "10.0.0.5")?.hosts(prefer: .ipv6), ["10.0.0.5"])
        XCTAssertNil(DiscoveryReply.parse("HELLO\t1\tx\t8000".data(using: .utf8)!))
        XCTAssertNil(DiscoveryReply.parse("AURORA68-MUSIC\t1\tx\tport".data(using: .utf8)!))
        XCTAssertNil(DiscoveryReply.parse(Data()))
        XCTAssertEqual(Address.probe, "AURORA68-MUSIC?")
        XCTAssertEqual(Address.discoveryPort, 8070)
        XCTAssertEqual(Address.discoveryGroup, "239.255.68.70")
    }

    func testPathsAndQueriesEncoded() {
        XCTAssertEqual(Address.encodePath("Bj\u{F6}rk - Post/01. Bj\u{F6}rk - Army of Me.m4a"),
                       "Bj%C3%B6rk%20-%20Post/01.%20Bj%C3%B6rk%20-%20Army%20of%20Me.m4a")
        XCTAssertEqual(Address.encodePath("Jane's Addiction - Strays/#1.mp3"), "Jane%27s%20Addiction%20-%20Strays/%231.mp3")
        XCTAssertEqual(Address.encodeQuery("a/b c"), "a%2Fb%20c")
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
        XCTAssertEqual(TrackName.parse("03. Bj\u{F6}rk - Army of Me", artist: "Bj\u{F6}rk").title, "Army of Me")
        XCTAssertEqual(TrackName.stem(of: "Weezer - Pinkerton (Explicit)/03. Weezer - Across the Sea.m4a"),
                       "03. Weezer - Across the Sea")
        XCTAssertEqual(TrackName.time(187), "3:07")
        XCTAssertEqual(TrackName.time(3723), "1:02:03")
        XCTAssertEqual(TrackName.time(.nan), "--:--")
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
        q.playNext(["x", "y"])
        XCTAssertEqual(q.items, ["a", "x", "y", "b"])
        q.remove(at: 0)                               // the playing one goes: its follower plays
        XCTAssertEqual(q.current, "x")
        XCTAssertEqual(q.jump(to: 2), "b")
        q.remove(at: 2)                               // the last, playing: nothing plays
        XCTAssertNil(q.current)
        q.append(["z"])                               // added after the end: the new song plays, not "x" again
        XCTAssertEqual(q.current, "z")
        XCTAssertEqual(q.upcoming.count, 0)
        q.clear()
        XCTAssertTrue(q.isEmpty)
        XCTAssertNil(q.jump(to: 0))
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
        XCTAssertTrue(q.previous(elapsed: 3.2).restart)
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
        var a = PlayQueue<Int>(), b = PlayQueue<Int>()
        a.replace(with: Array(1...20)); b.replace(with: Array(1...20))
        var g1 = SeededGenerator(seed: 9), g2 = SeededGenerator(seed: 9)
        a.shuffleUpcoming(using: &g1); b.shuffleUpcoming(using: &g2)
        XCTAssertEqual(a, b)                          // the core's generator: the same order from the same seed
    }
}

final class ModelTests: XCTestCase {
    func testCatalogFromText() throws {
        let info = answer(["A68M\t1\tinfo\t1\t0\t1", "Home Music\t3\t2\t3\t30\t7\t20"])
        let artists = answer(["A68M\t1\tartists\t2\t0\t2", "a1\tThe Cure\t2\t20", "a2\tWeezer\t1\t12"])
        let albums = answer(["A68M\t1\talbums\t3\t0\t3",
                             "l1\tDisintegration\tThe Cure\t12\t3600\ta1\tThe Cure - Disintegration",
                             "l2\tSongs Of A Lost World\tThe Cure\t8\t2940\ta1\tThe Cure - Songs Of A Lost World",
                             "l3\tOK Human\tWeezer\t12\t1800\ta2\tWeezer - OK Human"])
        let c = try Catalog.make(info: info, artists: [artists], albums: [albums])
        XCTAssertEqual(c.name, "Home Music")
        let ix = LibraryIndex(c)
        XCTAssertEqual(ix.albums(of: c.artists[0]).map(\.title), ["Disintegration", "Songs Of A Lost World"])
        XCTAssertEqual(ix.coverAlbum(of: c.artists[1])?.folder, "Weezer - OK Human")
        XCTAssertEqual(ix.artists(matching: "lost").map(\.name), ["The Cure"])
        XCTAssertEqual(ix.albums(matching: "weezer human").map(\.id), ["l3"])
        XCTAssertEqual(ix.artists(matching: "").count, 2)
        // pages: the same list in two answers
        let p1 = answer(["A68M\t1\tartists\t2\t0\t1", "a1\tThe Cure\t2\t20"])
        let p2 = answer(["A68M\t1\tartists\t2\t1\t1", "a2\tWeezer\t1\t12"])
        XCTAssertEqual(Records(p1).head()?.next, 1)
        XCTAssertNil(Records(p2).head()?.next)
        XCTAssertEqual(try Catalog.make(info: info, artists: [p1, p2], albums: [albums]).artists, c.artists)
        // an older server's albums have no folder: said in words
        let old = answer(["A68M\t1\talbums\t1\t0\t1", "l1\tDisintegration\tThe Cure\t12\t3600"])
        XCTAssertThrowsError(try Catalog.make(info: info, artists: [artists], albums: [old])) { e in
            XCTAssertEqual(e as? MusicAPIError, .olderServer)
        }
        XCTAssertEqual(ServerInfo.parse(info), ServerInfo(name: "Home Music", version: "3", library: 7, every: 20, artists: 2, albums: 3, tracks: 30))
        XCTAssertNil(ServerInfo.parse(Data("HTTP/1.0 404".utf8)))
    }

    func testTracksAndLive() throws {
        let tr = answer(["A68M\t1\ttracks\t1\t0\t1",
                         "t1\t1\tClosing Time\tSemisonic\t273\tl9\tSemisonic - Feeling Strangely Fine/01. Semisonic - Closing Time.m4a\tFeeling Strangely Fine\tSemisonic - Feeling Strangely Fine"])
        let t = try Track.page(tr)
        XCTAssertEqual(t.tracks.first?.albumTitle, "Feeling Strangely Fine")
        XCTAssertEqual(t.tracks.first?.seconds, 273)
        XCTAssertEqual(t.tracks.first?.path, "Semisonic - Feeling Strangely Fine/01. Semisonic - Closing Time.m4a")
        XCTAssertNil(t.next)

        let now = answer(["A68M\t1\tnow\t1\t0\t4", "N\tHome Music\t1\t0\t0\t0\t3\t1",
                          "C\ta/b.m4a\ta\tGamma - Closing Time\t12.5\t200.0", "Q\ta/b.m4a\t01. b", "L\tfd00::68\t1700000000"])
        let n = try XCTUnwrap(LiveNow.parse(now))
        XCTAssertEqual(n.current?.title, "Gamma - Closing Time")
        XCTAssertEqual(n.current?.elapsed, 12.5)
        XCTAssertEqual(n.queue.map(\.title), ["01. b"])
        XCTAssertEqual(n.listeners.first?.who, "fd00::68")
        XCTAssertEqual(n.changes, 3)
        let idle = try XCTUnwrap(LiveNow.parse(answer(["A68M\t1\tnow\t0\t0\t1", "N\tHome Music\t0\t0\t1\t0\t0\t0"])))
        XCTAssertNil(idle.current)
        XCTAssertTrue(idle.`repeat`)
        XCTAssertNil(LiveNow.parse(answer(["A68M\t1\terror\t1\t0\t1", "no"])))
        XCTAssertEqual(Records(answer(["A68M\t1\terror\t1\t0\t1", "There is no such album in the library now."])).errorText,
                       "There is no such album in the library now.")
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

/// players as devices, internet radio, the library's generation, the position bar
final class DeviceTests: XCTestCase {
    func testDevicesFromText() {
        let list = answer(["live\tHome Music Live\tlive\t1\tClosing Time\tSemisonic\t1\t12.5\t273.0\t100\tFeeling Strangely Fine\tSemisonic - Feeling Strangely Fine\t0\t1795336800\t1\t3\t1",
                           "mac-0123456789ab\tStudy iMac\tmacos\t1\t\t\t0\t0\t0\t100\t\t\t0\t1795336790\t0\t1.2\t0",
                           "se1\tCaf\u{e9} SE\tclassic\t0\tLossless\tGamma\t0\t3.0\t2.0\t80\tConverted\tGamma - Converted\t1795330000\t1795330000\t0\t1.4\t0"])
        let d = Device.list(list)
        XCTAssertEqual(d.map(\.id), ["live", "mac-0123456789ab", "se1"])
        XCTAssertTrue(d[0].isLive)
        XCTAssertEqual(d[0].state.title, "Closing Time")
        XCTAssertEqual(d[0].state.position, 12.5)
        XCTAssertNil(d[0].state.at)
        XCTAssertEqual(d[1].state, DeviceState())                      // nothing playing: the defaults
        XCTAssertEqual(d[2].name, "Caf\u{e9} SE")
        XCTAssertEqual(d[2].symbol, "desktopcomputer")
        XCTAssertFalse(d[2].online)
        XCTAssertEqual(d[2].state.volume, 80)
        XCTAssertEqual(d[2].state.at, 1795330000)
        XCTAssertEqual(d[2].lastSeen, 1795330000)
    }

    func testCommandsBecomePlayerActions() {
        let text = answer([
            "1\tplay\t0\t\t1",
            "T\tabcd1234\tClosing Time\tSemisonic\t273\tSemisonic - Feeling Strangely Fine/01. Semisonic - Closing Time.m4a\tFeeling Strangely Fine",
            "2\tplay\t0\t\t0", "R\tGroove Salad\thttp://ice2.somafm.com/groovesalad-128-mp3",
            "3\tseek\t42\t\t0",
            "4\tvolume\t0\t100\t0",
            "5\tpause\t0\t\t0",
            "6\tdance\t0\t\t0",
            "7\tadd\t0\t\t0", "T\tt2\t\t\t0\tLoose.mp3\t"])
        let c = DeviceCommand.list(text)
        XCTAssertEqual(c.map(\.id), [1, 2, 3, 4, 5, 6, 7])
        guard case .play(let tracks, let from)? = c[0].playerAction else { return XCTFail("play") }
        XCTAssertEqual(from, 0)
        XCTAssertEqual(c[0].shuffle, 1)
        XCTAssertEqual(tracks.first?.id, "abcd1234")
        XCTAssertEqual(tracks.first?.number, 1)
        XCTAssertEqual(tracks.first?.folder, "Semisonic - Feeling Strangely Fine")
        XCTAssertEqual(tracks.first?.albumTitle, "Feeling Strangely Fine")
        XCTAssertEqual(c[1].playerAction, .radio(name: "Groove Salad", url: URL(string: "http://ice2.somafm.com/groovesalad-128-mp3")!))
        XCTAssertEqual(c[2].playerAction, .seek(42))
        XCTAssertEqual(c[3].playerAction, .volume(1.0))
        XCTAssertEqual(c[4].playerAction, .pause)
        XCTAssertNil(c[5].playerAction)
        guard case .add(let added)? = c[6].playerAction else { return XCTFail("add") }
        XCTAssertEqual(added.first?.title, "Loose")                    // no title: from the file's name
        XCTAssertEqual(added.first?.folder, "")
    }

    func testCommandsAsRequests() {
        XCTAssertEqual(Command(.play, what: LiveWhat.folder("Caf\u{e9} Sessions"), shuffle: true).path(to: "live"),
                       "/mac/command?to=live&action=play&what=f%3ACaf%C3%A9%20Sessions&shuffle=1&enc=utf8&full=1")
        XCTAssertEqual(Command(.seek, position: 42.5).path(to: "mac-1"), "/mac/command?to=mac-1&action=seek&position=42.500&enc=utf8&full=1")
        XCTAssertEqual(Command(.volume, volume: 150).path(to: "se1"), "/mac/command?to=se1&action=volume&volume=100&enc=utf8&full=1")
        XCTAssertEqual(Command(.jump, index: 0).path(to: "live"), "/mac/command?to=live&action=jump&index=0&enc=utf8&full=1")
        XCTAssertEqual(Command(.`repeat`, on: false).path(to: "live"), "/mac/command?to=live&action=repeat&on=0&enc=utf8&full=1")
    }

    func testRadio() {
        let text = answer(["SomaFM\tGroove Salad\thttp://ice2.somafm.com/groovesalad-128-mp3",
                           "SomaFM\tDrone Zone\thttp://ice2.somafm.com/dronezone-128-mp3",
                           "By country/Austria\t\u{d6}1\thttp://ors-sn05.ors-shoutcast.at/oe1-q2a",
                           "SomaFM\tLush\thttp://ice2.somafm.com/lush-128-mp3"])
        let r = RadioList.parse(text)
        let g = r.groups
        XCTAssertEqual(g.map(\.group), ["SomaFM", "By country/Austria", "SomaFM"])
        XCTAssertEqual(g[0].stations.map(\.index), [0, 1])
        XCTAssertEqual(g[2].stations.first?.index, 3)                   // "r:3" plays Lush
        XCTAssertNotEqual(g[0].id, g[2].id)
        XCTAssertEqual(LiveWhat.radio(3), "r:3")
        XCTAssertEqual(LiveWhat.everything, "f:")
        // a server without /mac/radio: the classic Play's own list, read by its own parser
        let b = RadioList.builtIn
        XCTAssertGreaterThanOrEqual(b.stations.count, 40)
        XCTAssertEqual(b.stations.first, RadioStation(group: "SomaFM", name: "Groove Salad", url: "http://ice2.somafm.com/groovesalad-128-mp3"))
        XCTAssertTrue(b.stations.contains(RadioStation(group: "By country/Austria", name: "\u{d6}1", url: "http://ors-sn05.ors-shoutcast.at/oe1-q2a")))
        // a station that is a playlist: its first plain stream
        XCTAssertTrue(Playlist.isPlaylist(URL(string: "http://somafm.com/groovesalad.pls")!))
        XCTAssertFalse(Playlist.isPlaylist(URL(string: "http://x/live.m3u8")!))
        XCTAssertEqual(Playlist.firstStream(Data("[playlist]\nFile1=https://a/b\nFile2=http://ice1.somafm.com/groovesalad-128-mp3\n".utf8)),
                       URL(string: "http://ice1.somafm.com/groovesalad-128-mp3"))
        XCTAssertNil(Playlist.firstStream(Data("nothing here".utf8)))
    }

    func testIdentityAndGeneration() {
        XCTAssertEqual(DeviceIdentity.id(kept: "mac-0123456789ab"), "mac-0123456789ab")
        let uuid = UUID(uuidString: "E621E1F8-C36C-495A-93FC-0C247A3E6E5F")!
        let made = DeviceIdentity.id(kept: nil, make: { uuid })
        XCTAssertEqual(made, DeviceIdentity.id(kept: nil, make: { uuid }))    // the same seed, the same id
        XCTAssertTrue(made.hasPrefix("mac-"))
        XCTAssertEqual(made.count, 16)
        XCTAssertTrue(DeviceIdentity.isValid(made))
        XCTAssertNotEqual(DeviceIdentity.id(kept: "../etc"), "../etc")
        XCTAssertNotEqual(DeviceIdentity.id(kept: "live"), "live")
        XCTAssertFalse(DeviceIdentity.isValid(String(repeating: "a", count: 65)))
        XCTAssertFalse(DeviceIdentity.isValid("caf\u{e9}"))

        var w = GenerationWatch()
        XCTAssertFalse(w.changed(3))                                   // the first one seen: nothing to refresh
        XCTAssertFalse(w.changed(3))
        XCTAssertFalse(w.changed(nil))
        XCTAssertTrue(w.changed(4))                                    // albums were added
        XCTAssertFalse(w.changed(4))
        XCTAssertTrue(w.changed(1))                                    // the server restarted
    }

    func testScrubAndPosition() {
        XCTAssertEqual(Scrub.time(x: 50, width: 200, duration: 240), 60)        // a click a quarter along
        XCTAssertEqual(Scrub.time(x: -10, width: 200, duration: 240), 0)
        XCTAssertEqual(Scrub.time(x: 500, width: 200, duration: 240), 240)
        XCTAssertEqual(Scrub.time(x: 50, width: 0, duration: 240), 0)
        XCTAssertEqual(Scrub.time(x: 50, width: 200, duration: 0), 0)
        XCTAssertEqual(Scrub.fraction(60, duration: 240), 0.25)
        XCTAssertEqual(Scrub.fraction(.nan, duration: 240), 0)

        let s = DeviceState(playing: true, title: "x", position: 10, length: 100, at: 1000)
        XCTAssertEqual(s.estimatedPosition(at: 1005), 15)
        XCTAssertEqual(s.estimatedPosition(at: 2000), 100)
        var paused = s
        paused.playing = false
        XCTAssertEqual(paused.estimatedPosition(at: 1005), 10)
    }

    /// this Mac as a device: the core's rules through DeviceSession
    func testDeviceSession() {
        let s = DeviceSession(id: "mac-0123456789ab", name: "Caf\u{e9} Mac", version: "1.2")
        s.begin()
        XCTAssertEqual(s.path(.announce), "/mac/announce?id=mac-0123456789ab&name=Caf%C3%A9%20Mac&kind=macos&v=1.2&enc=utf8&full=1")
        XCTAssertFalse(s.announced)
        XCTAssertTrue(s.answer(.announce, status: 200, body: Data("ok\r20\r7\r".utf8)))
        XCTAssertTrue(s.announced)
        XCTAssertEqual(s.every, 20)
        XCTAssertEqual(s.generation, 7)
        XCTAssertFalse(s.takeGenerationChange())                       // the first generation known
        XCTAssertTrue(s.setState(DeviceState(playing: true, title: "Closing Time", position: 3, length: 273, volume: 80, track: "t1")))
        XCTAssertEqual(s.path(.state), "/mac/state?id=mac-0123456789ab&p=1&pos=3&len=273&vol=80&t=t1&enc=utf8&full=1")
        XCTAssertTrue(s.answer(.state, status: 200, body: Data("ok\r".utf8)))
        XCTAssertEqual(s.path(.commands), "/mac/commands?id=mac-0123456789ab&wait=20&enc=utf8&full=1")
        s.answer(.announce, status: 200, body: Data("ok\r20\r8\r".utf8))
        XCTAssertTrue(s.takeGenerationChange())                        // new albums
        XCTAssertFalse(s.takeGenerationChange())
        s.answer(.commands, status: 404, body: nil)                     // the server forgot this Mac: announce again
        XCTAssertFalse(s.announced)
        let r = DeviceSession(id: "mac-1", name: "x", version: "1.2")
        r.begin()
        r.answer(.announce, status: 200, body: Data("ok\r20\r1\r".utf8))
        XCTAssertTrue(r.setState(DeviceState(title: "A station \u{2014} with a long name that does not fit into the hundred bytes the core keeps for a title, cut at a character")))
        XCTAssertTrue(r.path(.state).contains("&title="))
    }
}
