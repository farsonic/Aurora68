// SPDX-License-Identifier: MPL-2.0
//
// AppModel.swift - the app's state: which server, its library, what is shown, and where music plays.
//
// Backstory (22 November 2026). Connecting: the address the owner typed (kept), else the first server
// that answers the discovery probe. A name is looked up and its addresses tried as numbers in the
// order the owner prefers (IPv6 first or IPv4 first), each with a four-second limit, then the name
// itself; the first that answers /api/catalog is the server. Playing: "Play on" is This Mac (the
// Player, original files) or Live (the server's channel that the Mac SE hears), and every Play, Play
// Next and Add to Queue goes to the one chosen.
import AppKit
import Foundation
import Aurora68Core

@MainActor
final class AppModel: ObservableObject {
    enum Status: Equatable {
        case idle, searching, connecting(String), connected, failed(String)
    }
    enum Section: Hashable { case artists, albums, live }
    enum Output: String, CaseIterable, Identifiable {
        case thisMac, live
        var id: String { rawValue }
        var label: String { self == .thisMac ? "This Mac" : "Live (Mac SE)" }
        var symbol: String { self == .thisMac ? "laptopcomputer" : "dot.radiowaves.left.and.right" }
    }

    @Published private(set) var status: Status = .idle
    @Published private(set) var index: LibraryIndex?
    @Published private(set) var api: MusicAPI?
    @Published private(set) var discovered: [DiscoveryReply] = []
    @Published var section: Section? = .artists
    @Published var search = ""
    @Published var output: Output = Output(rawValue: UserDefaults.standard.string(forKey: "output") ?? "") ?? .thisMac {
        didSet { UserDefaults.standard.set(output.rawValue, forKey: "output") }
    }
    @Published var address: String = UserDefaults.standard.string(forKey: "address") ?? ""
    @Published var prefer: IPFamily = IPFamily(rawValue: UserDefaults.standard.string(forKey: "prefer") ?? "") ?? .ipv6 {
        didSet { UserDefaults.standard.set(prefer.rawValue, forKey: "prefer") }
    }

    let player: Player
    let live: LiveController
    private var trackCache: [String: [Track]] = [:]
    private var started = false

    init() {
        player = Player()
        live = LiveController()
    }

    var serverName: String { index?.catalog.name ?? "" }
    var isConnected: Bool { status == .connected }

    func start() {
        guard !started else { return }
        started = true
        if address.isEmpty { discover(connectToFirst: true) } else { connect(typed: address) }
    }

    // ---- finding and connecting
    func discover(connectToFirst: Bool = false) {
        if connectToFirst { status = .searching }
        Task {
            let found = await Task.detached { Discovery.probe() }.value
            discovered = found
            if connectToFirst {
                if let first = found.first { connect(reply: first) }
                else { status = .failed("No music server answered on this network. Type its address in Settings.") }
            }
        }
    }

    func connect(reply: DiscoveryReply) {
        status = .connecting(reply.name)
        let urls = reply.hosts(prefer: prefer).compactMap { Address.baseURL(host: $0, port: reply.port) }
        Task { await connect(candidates: urls, remember: nil) }
    }

    func connect(typed: String) {
        guard let parsed = Address.parse(typed) else {
            status = .failed("\u{201C}\(typed)\u{201D} is not an address: try 192.168.1.20:8423 or [fd00::20]:8423.")
            return
        }
        let host = parsed.host, port = parsed.port
        status = .connecting(host)
        let prefer = self.prefer
        Task {
            var hosts = [host]
            if !Address.isNumeric(host) {
                let found = await Task.detached { Resolver.addresses(of: host) }.value
                hosts = Address.ordered(found, prefer: prefer) + [host]
            }
            let urls = hosts.compactMap { Address.baseURL(host: $0, port: port) }
            await connect(candidates: urls, remember: typed)
        }
    }

    private func connect(candidates: [URL], remember: String?) async {
        let probing = MusicAPI.probing()
        var lastError = "The server did not answer."
        for base in candidates {
            do {
                let catalog = try await MusicAPI(base: base, session: probing).catalog()
                let api = MusicAPI(base: base)
                self.api = api
                index = LibraryIndex(catalog)
                trackCache = [:]
                player.api = api
                live.api = api
                status = .connected
                if let remember {
                    address = remember
                    UserDefaults.standard.set(remember, forKey: "address")
                }
                return
            } catch {
                lastError = "\(base.host ?? base.absoluteString): \(error.localizedDescription)"
            }
        }
        status = .failed(lastError)
    }

    func reload() {
        guard let api else { return }
        Task {
            if let c = try? await api.catalog() { index = LibraryIndex(c); trackCache = [:] }
        }
    }

    // ---- the library
    func tracks(album: Album) async -> [Track] {
        if let t = trackCache[album.id] { return t }
        guard let api, let t = try? await api.tracks(album: album.id) else { return [] }
        trackCache[album.id] = t
        return t
    }

    func tracks(artist: Artist) async -> [Track] {
        guard let index else { return [] }
        var all: [Track] = []
        for a in index.albums(of: artist) { all += await tracks(album: a) }
        return all
    }

    var filteredArtists: [Artist] { index?.artists(matching: search) ?? [] }
    var filteredAlbums: [Album] { index?.albums(matching: search) ?? [] }

    func albums(of artist: Artist) -> [Album] { index?.albums(of: artist) ?? [] }

    func coverURL(_ album: Album?) -> URL? {
        guard let api, let album else { return nil }
        return api.artURL(folder: album.folder)
    }

    func coverURL(folder: String) -> URL? { api?.artURL(folder: folder) }

    // ---- playing, wherever "Play on" says
    func play(album: Album, shuffled: Bool = false) {
        Task {
            if output == .live { await live.play(album: album, shuffled: shuffled); return }
            player.play(await tracks(album: album), shuffled: shuffled)
        }
    }

    func play(artist: Artist, shuffled: Bool = false) {
        Task {
            if output == .live { await live.play(albums: albums(of: artist), shuffled: shuffled); return }
            player.play(await tracks(artist: artist), shuffled: shuffled)
        }
    }

    func play(_ tracks: [Track], from start: Int) {
        Task {
            if output == .live { await live.play(tracks: tracks, from: start); return }
            player.play(tracks, from: start)
        }
    }

    func playNext(_ tracks: [Track]) {
        Task {
            if output == .live { await live.playNext(tracks) } else { player.playNext(tracks) }
        }
    }

    func add(_ tracks: [Track]) {
        Task {
            if output == .live { await live.add(tracks) } else { player.add(tracks) }
        }
    }

    func add(album: Album) {
        Task {
            if output == .live { await live.add(album: album) } else { player.add(await tracks(album: album)) }
        }
    }

    func togglePlay() {
        if output == .live && !player.listeningLive { Task { await live.togglePlay() } } else { player.togglePlay() }
    }

    func next() {
        if output == .live { Task { await live.skip() } } else { player.skip() }
    }

    func previous() {
        if output == .live { Task { await live.previous() } } else { player.previous() }
    }

    /// Cmd-F: the search field in the toolbar
    func focusSearch() {
        guard let toolbar = NSApp.keyWindow?.toolbar else { return }
        for item in toolbar.items {
            if let s = item as? NSSearchToolbarItem { s.beginSearchInteraction(); return }
        }
    }

    func openSettings() {
        NSApp.sendAction(Selector(("showSettingsWindow:")), to: nil, from: nil)
    }
}
