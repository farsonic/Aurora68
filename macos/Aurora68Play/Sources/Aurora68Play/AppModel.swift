// SPDX-License-Identifier: MPL-2.0
//
// AppModel.swift - the app's state: which server, its library, what is shown, and where music plays.
//
// Backstory (22 November 2026). Connecting: the address the owner typed (kept), else the first server
// that answers the discovery probe. A name is looked up and its addresses tried as numbers in the
// order the owner prefers (IPv6 first or IPv4 first), each with a four-second limit, then the name
// itself; the first that answers /api/catalog is the server. Playing: "Play on" is This Mac (the
// Player, original files), Live (the server's channel that the Mac SE hears), or, since the same day's
// later requests, any other Aurora68 Play that is online (DeviceLink.swift): every Play, Play Next and
// Add to Queue goes to the one chosen, and the player bar shows and controls it.
//
// New albums without a restart (the owner, the same day): the server counts its library's generation
// and says it in every announce answer (or /api/info, every 20 seconds, on a server without devices);
// when it changes the catalogue is fetched again. What is open stays open: the navigation path holds
// artists and albums by value and the views look them up again by id.
//
// One code base with the SE's Play (the same evening, docs/08 item 107): what is sent to Live or another player
// is a Command, whose request the shared C core writes, and the catalogue comes as the server's text (MusicAPI).
//
// Music videos (22 November 2026, docs/08 item 109): an album's come with its tracks, an artist's from /mac/videos;
// a video plays wherever "Play on" says, as songs do (Track.what names it v:ID): here as a video, on another Mac as a
// video, on the SE and on Live as its sound.
//
// Videos linked to songs (23 November 2026, docs/08 item 115): the server says which song each video is of. A song with
// a video has a film button (its video plays, the album's videos after it, as a poster's click does); "Play Videos
// Instead of Songs" (Settings, and the Controls menu; kept in UserDefaults as "videosInstead") makes every list played
// here or on another Mac take each song's video in its place. Live is left as it is: the SE hears it, and a video's
// sound is the song again. The videos of what has been opened are kept by id (videoIndex) to find a song's.
import AppKit
import Foundation
import Aurora68Core

/// the settings' keys in UserDefaults, which @AppStorage in the views and the menus share with the model
enum Prefs {
    static let videosInstead = "videosInstead"          // "Play Videos Instead of Songs"
}

@MainActor
final class AppModel: ObservableObject {
    enum Status: Equatable {
        case idle, searching, connecting(String), connected, failed(String)
    }
    enum Section: Hashable { case artists, albums, videos, live }
    enum Output: Hashable {
        case thisMac, live, device(String)

        var key: String {
            switch self {
            case .thisMac: return "thisMac"
            case .live: return "live"
            case .device(let id): return "device:" + id
            }
        }

        init(key: String) {
            if key == "live" { self = .live }
            else if key.hasPrefix("device:") { self = .device(String(key.dropFirst(7))) }
            else { self = .thisMac }
        }

        var deviceID: String? { if case .device(let id) = self { return id } else { return nil } }
    }

    @Published private(set) var status: Status = .idle
    @Published private(set) var index: LibraryIndex?
    @Published private(set) var api: MusicAPI?
    @Published private(set) var discovered: [DiscoveryReply] = []
    @Published private(set) var generation = 0             // bumped at each refresh of the library
    @Published private(set) var radio: RadioList?
    @Published var message: String?                         // a refusal from the server, shown in the bar
    @Published var section: Section? = .artists
    @Published var search = ""
    @Published var output: Output = Output(key: UserDefaults.standard.string(forKey: "output") ?? "") {
        didSet { UserDefaults.standard.set(output.key, forKey: "output") }
    }
    @Published var address: String = UserDefaults.standard.string(forKey: "address") ?? ""
    @Published var prefer: IPFamily = IPFamily(rawValue: UserDefaults.standard.string(forKey: "prefer") ?? "") ?? .ipv6 {
        didSet { UserDefaults.standard.set(prefer.rawValue, forKey: "prefer") }
    }

    let player: Player
    let live: LiveController
    let devices: DeviceLink
    private var trackCache: [String: [Track]] = [:]
    private var videoCache: [String: [Track]] = [:]             // an album's videos (its id), an artist's ("a:" and its id)
    private var videoIndex: [String: Track] = [:]               // every video seen so far, by id: a song's is found here
    private var started = false
    private var watch = GenerationWatch()
    private var volumeSend: Task<Void, Never>?
    private var heldPosition: (device: String, position: Double, until: Date)?

    init() {
        player = Player()
        live = LiveController()
        devices = DeviceLink()
        devices.onCommand = { [weak self] c in self?.obey(c) }
        devices.onGeneration = { [weak self] g in self?.libraryGeneration(g) }
        devices.currentState = { [weak self] in self?.localState() ?? DeviceState() }
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
                videoCache = [:]
                videoIndex = [:]
                watch = GenerationWatch()
                radio = nil
                player.api = api
                live.api = api
                devices.api = api
                status = .connected
                if let remember {
                    address = remember
                    UserDefaults.standard.set(remember, forKey: "address")
                }
                loadRadio()
                return
            } catch {
                lastError = "\(base.host ?? base.absoluteString): \(error.localizedDescription)"
            }
        }
        status = .failed(lastError)
    }

    /// the catalogue again (Command-R, or a new generation): what is open stays open
    func reload() {
        guard let api else { return }
        Task {
            if let c = try? await api.catalog() {
                index = LibraryIndex(c)
                trackCache = [:]
                videoCache = [:]
                videoIndex = [:]
                generation += 1
            }
        }
    }

    private func libraryGeneration(_ g: Int) {
        if watch.changed(g) { reload() }
    }

    func loadRadio() {
        guard let api else { return }
        Task { if let r = try? await api.radio() { radio = r } }
    }

    // ---- the library
    func tracks(album: Album) async -> [Track] {
        await media(album: album).tracks
    }

    /// an album's songs and its music videos (one answer of the server's)
    func media(album: Album) async -> (tracks: [Track], videos: [Track]) {
        if let t = trackCache[album.id] { return (t, videoCache[album.id] ?? []) }
        guard let api, let m = try? await api.media(album: album.id) else { return ([], []) }
        trackCache[album.id] = m.tracks
        videoCache[album.id] = m.videos
        remember(m.videos)
        return m
    }

    /// an artist's music videos: those in its albums, those beside them, those of its songs
    func videos(artist: Artist) async -> [Track] {
        let key = "a:" + artist.id
        if let v = videoCache[key] { return v }
        guard let api, artist.videos > 0, let v = try? await api.videos(artist: artist.id) else { return [] }
        videoCache[key] = v
        remember(v)
        return v
    }

    /// every video of the library, sorted by artist (the Videos page)
    func everyVideo() async -> [Track] {
        if let v = videoCache["*"] { return v }
        guard let api, let v = try? await api.allVideos() else { return [] }
        videoCache["*"] = v
        remember(v)
        return v
    }

    private func remember(_ videos: [Track]) {
        for (id, v) in VideoLinks.index(videos) where videoIndex[id] == nil { videoIndex[id] = v }
    }

    /// a song's music video, when the server linked one and it has been listed (its album's videos list it)
    func video(of song: Track) -> Track? { VideoLinks.video(of: song, in: videoIndex) }

    /// the film button of a song: its video plays as a video, the list's videos after it (a poster's click does the same)
    func playVideo(of song: Track, among videos: [Track]) {
        guard let v = video(of: song) else { return }
        if let i = videos.firstIndex(where: { $0.id == v.id }) { play(videos, from: i) } else { play([v], from: 0) }
    }

    // ---- "Play Videos Instead of Songs": each song's video in its place, here and on another Mac (not on Live, which the
    // SE hears: a video's sound is the song again)
    var videosInstead: Bool { UserDefaults.standard.bool(forKey: Prefs.videosInstead) }

    private func alternate(_ list: [Track], on o: Output? = nil) -> [Track] {
        guard videosInstead, (o ?? output) != .live else { return list }
        return VideoLinks.alternate(list, videos: videoIndex)
    }

    /// a whole list, shuffled or not, as its tracks (with videos in place of songs)
    private func playList(_ list: [Track], shuffled: Bool, on o: Output) {
        switch o {
        case .live: Task { await live.play(tracks: shuffled ? list.shuffled() : list, from: 0) }
        case .device(let id):
            let l = shuffled ? list.shuffled() : list
            guard let first = l.first else { return }
            toDevice(id, [Command(.play, what: first.what)] + l.dropFirst().map { Command(.add, what: $0.what) })
        case .thisMac: player.play(list, shuffled: shuffled)
        }
    }

    func posterURL(_ video: Track) -> URL? { api?.posterURL(video.id) }

    func tracks(artist: Artist) async -> [Track] {
        guard let index else { return [] }
        var all: [Track] = []
        for a in index.albums(of: artist) { all += await tracks(album: a) }
        return all
    }

    var filteredArtists: [Artist] { index?.artists(matching: search) ?? [] }
    var filteredAlbums: [Album] { index?.albums(matching: search) ?? [] }

    func albums(of artist: Artist) -> [Album] { index?.albums(of: artist) ?? [] }

    /// the artist as the library has it now (its albums may have changed since it was opened)
    func current(_ artist: Artist) -> Artist {
        index?.catalog.artists.first { $0.id == artist.id } ?? artist
    }

    func current(_ album: Album) -> Album { index?.albumsByID[album.id] ?? album }

    func coverURL(_ album: Album?) -> URL? {
        guard let api, let album else { return nil }
        return api.artURL(folder: album.folder)
    }

    func coverURL(folder: String) -> URL? { api?.artURL(folder: folder) }

    // ---- where music plays: This Mac, Live, or another player
    var outputs: [Output] {
        var out: [Output] = [.thisMac, .live] + devices.others.map { .device($0.id) }
        if !out.contains(output) { out.append(output) }    // a chosen player that went offline stays shown
        return out
    }

    func label(_ o: Output) -> String {
        switch o {
        case .thisMac: return "This Mac"
        case .live: return "Live (Mac SE)"
        case .device(let id):
            guard let d = devices.device(id) else { return "A player that is not online" }
            return d.online ? d.name : d.name + " (offline)"
        }
    }

    func symbol(_ o: Output) -> String {
        switch o {
        case .thisMac: return "laptopcomputer"
        case .live: return "dot.radiowaves.left.and.right"
        case .device(let id): return devices.device(id)?.symbol ?? "hifispeaker"
        }
    }

    /// the state of the chosen player, when it is another one
    var remoteState: DeviceState? {
        guard let id = output.deviceID, var s = devices.state(of: id) else { return nil }
        if let h = heldPosition, h.device == id, Date() < h.until { s.position = h.position }
        return s
    }

    private func toDevice(_ id: String, _ commands: [Command]) {
        Task {
            message = await devices.send(id, commands)
        }
    }

    func play(album: Album, shuffled: Bool = false) {
        if videosInstead && output != .live && album.videos > 0 {     // the album's songs, each as its video where it has one
            let o = output
            Task {
                let songs = await tracks(album: album)                 // its videos are known once its list is
                playList(alternate(songs, on: o), shuffled: shuffled, on: o)
            }
            return
        }
        switch output {
        case .live: Task { await live.play(album: album, shuffled: shuffled) }
        case .device(let id): toDevice(id, [Command(.play, what: LiveWhat.folder(album.folder), shuffle: shuffled)])
        case .thisMac: Task { player.play(await tracks(album: album), shuffled: shuffled) }
        }
    }

    func play(artist: Artist, shuffled: Bool = false, on o: Output? = nil) {
        let albums = albums(of: current(artist))
        let place = o ?? output
        if videosInstead && place != .live && current(artist).videos > 0 {
            let a = current(artist)
            Task {
                let songs = await tracks(artist: a)
                playList(alternate(songs, on: place), shuffled: shuffled, on: place)
            }
            return
        }
        switch place {
        case .live: Task { await live.play(albums: albums, shuffled: shuffled) }
        case .device(let id):
            guard let first = albums.first else { return }
            var commands = [Command(.play, what: LiveWhat.folder(first.folder), shuffle: shuffled)]
            for a in albums.dropFirst() { commands.append(Command(.add, what: LiveWhat.folder(a.folder), shuffle: shuffled)) }
            toDevice(id, commands)
        case .thisMac: Task { player.play(await tracks(artist: current(artist)), shuffled: shuffled) }
        }
    }

    func play(_ songs: [Track], from start: Int) {
        let tracks = alternate(songs)                      // one for one: the place clicked is still the place played
        switch output {
        case .live: Task { await live.play(tracks: tracks, from: start) }
        case .device(let id):
            guard tracks.indices.contains(start) else { return }
            var commands = [Command(.play, what: tracks[start].what)]
            for t in tracks[(start + 1)...] { commands.append(Command(.add, what: t.what)) }
            toDevice(id, commands)
        case .thisMac: player.play(tracks, from: start)
        }
    }

    func playNext(_ songs: [Track]) {
        let tracks = alternate(songs)
        switch output {
        case .live: Task { await live.playNext(tracks) }
        case .device(let id): toDevice(id, tracks.reversed().map { Command(.next, what: $0.what) })
        case .thisMac: player.playNext(tracks)
        }
    }

    func add(_ songs: [Track]) {
        let tracks = alternate(songs)
        switch output {
        case .live: Task { await live.add(tracks) }
        case .device(let id): toDevice(id, tracks.map { Command(.add, what: $0.what) })
        case .thisMac: player.add(tracks)
        }
    }

    func add(album: Album) {
        if videosInstead && output != .live && album.videos > 0 {
            Task {
                let songs = await tracks(album: album)
                add(songs)
            }
            return
        }
        switch output {
        case .live: Task { await live.add(album: album) }
        case .device(let id): toDevice(id, [Command(.add, what: LiveWhat.folder(album.folder))])
        case .thisMac: Task { player.add(await tracks(album: album)) }
        }
    }

    // ---- stations (the Live page): everything shuffled, an artist, an internet radio station
    func playEverything(on o: Output? = nil) {
        switch o ?? output {
        case .live: Task { await live.act(Command(.play, what: LiveWhat.everything, shuffle: true)) }
        case .device(let id): toDevice(id, [Command(.play, what: LiveWhat.everything, shuffle: true)])
        case .thisMac:                     // the server's own station of the whole library, shuffled, as a stream
            guard let api else { return }
            player.playStream(title: "Everything, shuffled", url: api.url("/radio/", query: [("shuffle", "1")]))
        }
    }

    func playStation(_ artist: Artist, on o: Output? = nil) {
        play(artist: artist, shuffled: true, on: o)
    }

    /// whether a radio station can play there: Live plays the library only
    func canPlayRadio(on o: Output) -> Bool { o != .live }

    func playRadio(_ index: Int, on o: Output? = nil) {
        guard let st = radio?.stations, st.indices.contains(index) else { return }
        switch o ?? output {
        case .live: message = "Live plays the library; choose This Mac or another player for internet radio."
        case .device(let id): toDevice(id, [Command(.play, what: LiveWhat.radio(index))])
        case .thisMac:
            guard let url = URL(string: st[index].url) else { return }
            player.playStream(title: st[index].name, url: url)
        }
    }

    // ---- the player bar's buttons, wherever "Play on" says
    func togglePlay() {
        switch output {
        case .live where !player.listeningLive: Task { await live.togglePlay() }
        case .device(let id): toDevice(id, [Command((remoteState?.playing ?? false) ? .pause : .resume)])
        default: player.togglePlay()
        }
    }

    func next() {
        switch output {
        case .live: Task { await live.skip() }
        case .device(let id): toDevice(id, [Command(.skip)])
        case .thisMac: player.skip()
        }
    }

    func previous() {
        switch output {
        case .live: Task { await live.previous() }
        case .device(let id): toDevice(id, [Command(.previous)])
        case .thisMac: player.previous()
        }
    }

    func seek(to seconds: Double) {
        switch output {
        case .live: break                                      // the Live channel cannot seek
        case .device(let id):
            heldPosition = (id, seconds, Date().addingTimeInterval(4))   // shown until the player says where it is
            toDevice(id, [Command(.seek, position: seconds)])
        case .thisMac: player.seek(to: seconds)
        }
    }

    /// the volume slider for another player: sent when the slider rests for a moment
    func setRemoteVolume(_ v: Double) {
        guard let id = output.deviceID else { return }
        volumeSend?.cancel()
        volumeSend = Task {
            try? await Task.sleep(nanoseconds: 250_000_000)
            if Task.isCancelled { return }
            message = await devices.send(id, [Command(.volume, volume: Int((v * 100).rounded()))])
        }
    }

    // ---- this Mac as a player for the others
    func obey(_ c: DeviceCommand) {
        guard let a = c.playerAction else { return }
        switch a {
        case .play(let t, let from):
            player.play(t)
            if from > 0 { player.seek(to: from) }
        case .add(let t): player.add(t)
        case .playNext(let t): player.playNext(t)
        case .radio(let name, let url): player.playStream(title: name, url: url)
        case .pause: player.pause()
        case .resume: player.resume()
        case .skip: player.skip()
        case .previous: player.previous()
        case .stop: player.stop()
        case .seek(let s): player.seek(to: s)
        case .volume(let v): player.volume = v
        }
    }

    func localState() -> DeviceState {
        let vol = Int((player.volume * 100).rounded())
        if player.listeningLive {
            return DeviceState(playing: player.isPlaying, title: player.streamTitle, volume: vol)
        }
        guard let t = player.current else { return DeviceState(volume: vol) }
        return DeviceState(playing: player.isPlaying, title: t.title, artist: t.artist, album: t.albumTitle, folder: t.folder,
                           position: player.elapsed, length: player.duration, volume: vol, track: t.id)
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
