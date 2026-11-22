// SPDX-License-Identifier: MPL-2.0
//
// PlayerViews.swift - the player bar, the "Play on" choice, the Live page and the settings.
//
// Backstory (22 November 2026). The bar along the bottom is the one place to control what plays: the
// song and its cover, Previous, Play/Pause and Next, shuffle and repeat, the position (click or drag to
// seek), the volume. With "Play on: Live (Mac SE)" chosen, it shows and controls the server's Live
// channel instead, which the SE hears; with another player chosen (another Mac, the SE's own Aurora68
// Play), it shows what that player plays and sends it the commands (DeviceLink.swift).
//
// The same day the owner found the Live page empty ("should there not be a list of stations"): it now
// starts with stations to play, wherever "Play on" says (or, from a station's menu, anywhere else):
// everything shuffled, each artist as a station, and the internet radio stations every Aurora68 Play
// offers (the server's /api/radio, grouped as the classic Play's Radio menu is); then Live's queue and
// listeners as before, and the players on the network.
//
// The position bar is Scrubber, not a Slider: a SwiftUI Slider on macOS does not report the end of an
// edit for a click on its bar, and the seek waited for that, so a click never seeked (Player.swift).
import SwiftUI
import Aurora68Core

struct OutputPicker: View {
    @EnvironmentObject var model: AppModel
    @EnvironmentObject var devices: DeviceLink

    var body: some View {
        Picker(selection: $model.output) {
            ForEach(model.outputs, id: \.self) { o in
                Label(model.label(o), systemImage: model.symbol(o)).tag(o)
            }
        } label: {
            Label("Play on", systemImage: model.symbol(model.output))
        }
        .pickerStyle(.menu)
        .help("Play on this Mac, on the Live channel the Mac SE listens to, or on another Aurora68 Play")
    }
}

/// the song's position: a click jumps there, a drag seeks where it ends
struct Scrubber: View {
    let value: Double
    let duration: Double
    var enabled = true
    @Binding var scrubbing: Double?
    let onSeek: (Double) -> Void
    @State private var hover = false

    var body: some View {
        GeometryReader { g in
            let w = Double(g.size.width)
            let f = Scrub.fraction(scrubbing ?? value, duration: duration)
            let thick: CGFloat = (hover || scrubbing != nil) && enabled ? 6 : 4
            ZStack(alignment: .leading) {
                Capsule().fill(Color.primary.opacity(0.14)).frame(height: thick)
                Capsule().fill(enabled ? Color.accentColor : Color.secondary.opacity(0.6))
                    .frame(width: CGFloat(max(0, w * f)), height: thick)
                if enabled && (hover || scrubbing != nil) {
                    Circle().fill(Color.white).shadow(color: .black.opacity(0.3), radius: 1.5)
                        .frame(width: 12, height: 12)
                        .offset(x: CGFloat(w * f) - 6)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { v in
                        guard enabled else { return }
                        scrubbing = Scrub.time(x: Double(v.location.x), width: w, duration: duration)
                    }
                    .onEnded { v in
                        guard enabled else { return }
                        let t = Scrub.time(x: Double(v.location.x), width: w, duration: duration)
                        scrubbing = nil
                        onSeek(t)
                    }
            )
            .onHover { hover = $0 }
        }
        .frame(height: 14)
        .help(enabled ? "Click or drag to move in the song" : "")
    }
}

struct PlayerBar: View {
    @EnvironmentObject var model: AppModel
    @EnvironmentObject var player: Player
    @Environment(\.openWindow) private var openWindow
    @EnvironmentObject var live: LiveController
    @EnvironmentObject var devices: DeviceLink
    @State private var scrub: Double?

    private var isLive: Bool { model.output == .live }
    private var remote: DeviceState? { model.remoteState }
    private var isRemote: Bool { model.output.deviceID != nil }

    var body: some View {
        HStack(spacing: 16) {
            nowPlaying.frame(width: 260, alignment: .leading)
            Spacer(minLength: 8)
            VStack(spacing: 4) {
                controls
                position
            }
            .frame(maxWidth: 520)
            Spacer(minLength: 8)
            HStack(spacing: 8) {
                if !isLive && !isRemote && player.currentIsVideo {
                    Button { openWindow(id: "video") } label: { Image(systemName: "film") }
                        .buttonStyle(.borderless)
                        .help("Show the video")
                }
                if isLive {
                    Button { player.listeningLive ? player.stopListeningLive() : player.listenLive() } label: {
                        Image(systemName: player.listeningLive ? "headphones.circle.fill" : "headphones")
                    }
                    .buttonStyle(.borderless)
                    .help(player.listeningLive ? "Stop listening here" : "Listen to Live here as well")
                }
                Image(systemName: "speaker.fill").foregroundStyle(.secondary)
                if isRemote {
                    Slider(value: Binding(get: { Double(remote?.volume ?? 100) / 100 }, set: { model.setRemoteVolume($0) }), in: 0...1)
                        .frame(width: 100)
                        .help("The volume of \(model.label(model.output))")
                } else {
                    Slider(value: $player.volume, in: 0...1).frame(width: 100)
                }
            }
            .frame(width: 180, alignment: .trailing)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
        .background(.bar)
        .onReceive(clock) { _ in if isRemote { tick &+= 1 } }      // another player's position moves on
    }

    private let clock = Timer.publish(every: 1, on: .main, in: .common).autoconnect()
    @State private var tick = 0

    // ---- left: the song
    @ViewBuilder private var nowPlaying: some View {
        HStack(spacing: 10) {
            Cover(url: coverURL, corner: 5).frame(width: 48, height: 48)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.callout.weight(.semibold)).lineLimit(1)
                Text(subtitle).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                if isLive {
                    Label(liveListeners, systemImage: "dot.radiowaves.left.and.right")
                        .font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                } else if isRemote {
                    Label("On \(model.label(model.output))", systemImage: model.symbol(model.output))
                        .font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                }
            }
        }
    }

    private var coverURL: URL? {
        if isLive { return live.now?.current.flatMap { model.coverURL(folder: $0.folder) } }
        if isRemote { return remote.flatMap { $0.folder.isEmpty ? nil : model.coverURL(folder: $0.folder) } }
        if let t = player.current, t.isVideo { return model.posterURL(t) }
        return player.current.flatMap { model.coverURL(folder: $0.folder) }
    }

    private var title: String {
        if isLive { return live.now?.current?.title ?? "Live: nothing queued" }
        if isRemote { return remote.map { $0.title.isEmpty ? "Not playing" : $0.title } ?? "Not online" }
        if player.listeningLive { return player.streamTitle }
        return player.current?.title ?? "Not playing"
    }

    private var subtitle: String {
        if let m = model.message { return m }
        if isLive {
            guard let c = live.now?.current else { return live.error ?? "Choose an album to play on the Mac SE" }
            return c.folder.split(separator: "/").last.map(String.init) ?? ""
        }
        if isRemote {
            guard let s = remote else { return "Choose another player in Play on" }
            return [s.artist, s.album].filter { !$0.isEmpty }.joined(separator: " \u{2014} ")
        }
        if player.listeningLive { return "Internet radio" }
        guard let t = player.current else { return player.error ?? "" }
        return "\(t.artist) \u{2014} \(t.albumTitle)"
    }

    private var liveListeners: String {
        let n = live.now?.listeners.count ?? 0
        return n == 0 ? "No one listening" : n == 1 ? "1 listening" : "\(n) listening"
    }

    // ---- middle: the buttons and the position
    private var playing: Bool {
        if isLive && !player.listeningLive { return (live.now?.playing ?? false) }
        if isRemote { return remote?.playing ?? false }
        return player.isPlaying
    }

    @ViewBuilder private var controls: some View {
        HStack(spacing: 18) {
            Button { shuffle() } label: { Image(systemName: "shuffle") }
                .help(isLive ? "Shuffle what is still to come on Live" : "Shuffle what is still to come")
                .disabled(isRemote)
            Button { model.previous() } label: { Image(systemName: "backward.fill") }
            Button { model.togglePlay() } label: {
                Image(systemName: playing ? "pause.circle.fill" : "play.circle.fill").font(.system(size: 30))
            }
            Button { model.next() } label: { Image(systemName: "forward.fill") }
            Button { cycleRepeat() } label: {
                Image(systemName: repeatSymbol).foregroundStyle(repeatOn ? Color.accentColor : Color.primary)
            }
            .help("Repeat")
            .disabled(isRemote)
        }
        .buttonStyle(.borderless)
        .font(.system(size: 14))
    }

    private var repeatOn: Bool {
        isLive ? (live.now?.`repeat` ?? false) : player.repeatMode != .off
    }

    private var repeatSymbol: String {
        model.output == .thisMac && player.repeatMode == .one ? "repeat.1" : "repeat"
    }

    private func shuffle() {
        if isLive { Task { await live.shuffleRest() } } else { player.shuffleUpcoming() }
    }

    private func cycleRepeat() {
        if isLive { Task { await live.setRepeat(!(live.now?.`repeat` ?? false)) } }
        else { player.repeatMode = player.repeatMode.next }
    }

    @ViewBuilder private var position: some View {
        let _ = tick
        let now = times.0
        let total = times.1
        HStack(spacing: 8) {
            Text(TrackName.time(scrub ?? now)).frame(width: 44, alignment: .trailing)
            if isLive {
                ProgressView(value: total > 0 ? min(now, total) : 0, total: max(total, 1))
            } else {
                Scrubber(value: now, duration: total, enabled: canSeek && total > 0, scrubbing: $scrub) { model.seek(to: $0) }
            }
            Text(total > 0 ? TrackName.time(total) : "--:--").frame(width: 44, alignment: .leading)
        }
        .font(.caption.monospacedDigit())
        .foregroundStyle(.secondary)
        .controlSize(.small)
    }

    private var canSeek: Bool {
        if isRemote { return !(remote?.title.isEmpty ?? true) }
        return player.current != nil && !player.listeningLive
    }

    private var times: (Double, Double) {
        if isLive {
            let c = live.now?.current
            return (c?.elapsed ?? 0, c?.length ?? 0)
        }
        if isRemote { return (remote?.position ?? 0, remote?.length ?? 0) }
        return (player.elapsed, player.duration)
    }
}

/// a station's play button and its menu of other places to play it
struct StationRow: View {
    @EnvironmentObject var model: AppModel
    let symbol: String
    let title: String
    let subtitle: String
    var radio = false
    let play: (AppModel.Output?) -> Void

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: symbol).frame(width: 22).foregroundStyle(Color.accentColor)
            VStack(alignment: .leading, spacing: 1) {
                Text(title).lineLimit(1)
                if !subtitle.isEmpty { Text(subtitle).font(.caption).foregroundStyle(.secondary).lineLimit(1) }
            }
            Spacer()
            Button { play(nil) } label: { Image(systemName: "play.fill") }
                .buttonStyle(.borderless)
                .disabled(radio && !model.canPlayRadio(on: model.output))
                .help(radio && !model.canPlayRadio(on: model.output) ? "Live plays the library: choose another place in Play on"
                                                                     : "Play on \(model.label(model.output))")
            Menu {
                ForEach(model.outputs, id: \.self) { o in
                    Button { play(o) } label: { Label(model.label(o), systemImage: model.symbol(o)) }
                        .disabled(radio && !model.canPlayRadio(on: o))
                }
            } label: { Image(systemName: "ellipsis.circle") }
                .menuStyle(.borderlessButton)
                .menuIndicator(.hidden)
                .fixedSize()
                .help("Play it somewhere else")
        }
        .font(.callout)
        .padding(.vertical, 5)
        .contentShape(Rectangle())
        .onTapGesture(count: 2) {
            if !(radio && !model.canPlayRadio(on: model.output)) { play(nil) }
        }
    }
}

struct LiveView: View {
    @EnvironmentObject var model: AppModel
    @EnvironmentObject var live: LiveController
    @EnvironmentObject var player: Player
    @EnvironmentObject var devices: DeviceLink
    @State private var showAllArtists = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                header
                stations
                radio
                queue
                players
                if let e = live.error { Text(e).foregroundStyle(.red).font(.callout) }
            }
            .padding(24)
        }
        .navigationTitle("Live")
        .onAppear { if model.radio == nil { model.loadRadio() } }
    }

    private var header: some View {
        HStack(alignment: .bottom, spacing: 24) {
            Cover(url: live.now?.current.flatMap { model.coverURL(folder: $0.folder) }, corner: 10)
                .frame(width: 160)
            VStack(alignment: .leading, spacing: 6) {
                Text("LIVE \u{2014} WHAT THE MAC SE HEARS").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                Text(live.now?.current?.title ?? "Nothing queued").font(.largeTitle.bold()).lineLimit(2)
                Text(live.now?.paused == true ? "Paused" : (live.now?.current == nil ? "Start a station below, or play an album with \u{201C}Play on: Live\u{201D}" : "Playing"))
                    .foregroundStyle(.secondary)
                HStack {
                    OutputPicker().frame(width: 240)
                    Button { player.listeningLive ? player.stopListeningLive() : player.listenLive() } label: {
                        Label(player.listeningLive ? "Stop Listening" : "Listen Here", systemImage: "headphones")
                    }
                    Button("Clear Queue") { Task { await live.clear() } }.disabled(live.now?.queue.isEmpty ?? true)
                }
                .padding(.top, 8)
            }
        }
    }

    // ---- stations from the library
    private var stations: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("Stations").font(.title3.bold())
            Text("Play on \(model.label(model.output)); the \u{2026} menu plays a station somewhere else.")
                .font(.caption).foregroundStyle(.secondary).padding(.bottom, 4)
            StationRow(symbol: "shuffle", title: "Everything, shuffled",
                       subtitle: "\(model.index?.catalog.albums.count ?? 0) albums") { model.playEverything(on: $0) }
            Divider().opacity(0.4)
            let artists = model.index?.catalog.artists ?? []
            ForEach(showAllArtists ? artists : Array(artists.prefix(12))) { a in
                StationRow(symbol: "music.mic", title: a.name,
                           subtitle: a.albums.count == 1 ? "1 album, shuffled" : "\(a.albums.count) albums, shuffled") { model.playStation(a, on: $0) }
                Divider().opacity(0.4)
            }
            if artists.count > 12 {
                Button(showAllArtists ? "Fewer Artists" : "All \(artists.count) Artists") { showAllArtists.toggle() }
                    .buttonStyle(.link).padding(.top, 4)
            }
        }
    }

    // ---- internet radio
    @ViewBuilder private var radio: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("Internet Radio").font(.title3.bold())
            if let r = model.radio {
                ForEach(r.groups) { g in
                    Text(g.group.isEmpty ? "Stations" : g.group.replacingOccurrences(of: "/", with: " \u{203A} "))
                        .font(.caption.weight(.semibold)).foregroundStyle(.secondary).padding(.top, 10)
                    ForEach(g.stations) { item in
                        StationRow(symbol: "antenna.radiowaves.left.and.right", title: item.station.name,
                                   subtitle: URL(string: item.station.url)?.host ?? "", radio: true) { model.playRadio(item.index, on: $0) }
                        Divider().opacity(0.4)
                    }
                }
            } else {
                Text("The server sends its list of stations (a music server from before 22 November 2026 has none).")
                    .foregroundStyle(.secondary)
            }
        }
    }

    // ---- Live's queue and listeners
    private var queue: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Live Queue").font(.title3.bold())
            if let q = live.now?.queue, !q.isEmpty {
                ForEach(Array(q.enumerated()), id: \.offset) { i, item in
                    HStack {
                        Text("\(i + 1)").frame(width: 28, alignment: .trailing).foregroundStyle(.secondary)
                        Text(item.title).fontWeight(i == live.now?.pos ? .semibold : .regular)
                            .foregroundStyle(i == live.now?.pos ? Color.accentColor : Color.primary)
                            .lineLimit(1)
                        Spacer()
                        Button { Task { await live.jump(to: i) } } label: { Image(systemName: "play.fill") }
                            .buttonStyle(.borderless).help("Play this now")
                        Button { Task { await live.remove(at: i) } } label: { Image(systemName: "xmark") }
                            .buttonStyle(.borderless).help("Take it out of the queue")
                    }
                    .font(.callout)
                    .padding(.vertical, 4)
                    Divider().opacity(0.4)
                }
            } else {
                Text("Empty. Start a station above with Play on: Live (Mac SE), or play an album or a song there.")
                    .foregroundStyle(.secondary)
            }
            Text("Listening").font(.headline).padding(.top, 12)
            let who = live.now?.listeners ?? []
            if who.isEmpty {
                Text("No one yet. On the Mac SE: Aurora68 Play, Library \u{203A} Listen Live, or Radio \u{203A} Open URL\u{2026} with \(model.api?.liveURL.absoluteString ?? "the server\u{2019}s /live").")
                    .foregroundStyle(.secondary).textSelection(.enabled)
            } else {
                ForEach(Array(who.enumerated()), id: \.offset) { _, l in
                    Label(l.who, systemImage: "hifispeaker").font(.callout)
                }
            }
        }
    }

    // ---- the players on the network
    private var players: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Players").font(.title3.bold())
            let list = devices.devices.filter { !$0.isLive }
            if list.isEmpty {
                Text("Only this Mac. Every Aurora68 Play (on another Mac, or the SE's) shows up here when it runs.")
                    .foregroundStyle(.secondary)
            }
            ForEach(list) { d in
                HStack(spacing: 10) {
                    Circle().fill(d.online ? Color.green : Color.secondary.opacity(0.5)).frame(width: 8, height: 8)
                    Image(systemName: d.symbol).frame(width: 22)
                    VStack(alignment: .leading, spacing: 1) {
                        Text(d.id == devices.id ? "\(d.name) (this Mac)" : d.name)
                        Text(d.state.title.isEmpty ? (d.online ? "Not playing" : "Offline")
                                                   : [d.state.title, d.state.artist].filter { !$0.isEmpty }.joined(separator: " \u{2014} ")
                                                     + (d.state.playing ? "" : " (paused)"))
                            .font(.caption).foregroundStyle(.secondary).lineLimit(1)
                    }
                    Spacer()
                    if d.online && d.id != devices.id {
                        Button("Play on This") { model.output = .device(d.id) }
                            .disabled(model.output == .device(d.id))
                    }
                }
                .font(.callout)
                .padding(.vertical, 4)
                Divider().opacity(0.4)
            }
        }
    }
}

struct SettingsView: View {
    @EnvironmentObject var model: AppModel
    @State private var typed = ""

    var body: some View {
        Form {
            Section("Music server") {
                HStack {
                    TextField("Address", text: $typed, prompt: Text("192.168.1.20:8423 or [fd00::20]:8423"))
                        .onSubmit { model.connect(typed: typed) }
                    Button("Connect") { model.connect(typed: typed) }.disabled(typed.isEmpty)
                }
                Text(statusText).font(.caption).foregroundStyle(.secondary)
                HStack {
                    Button("Find on Network") { model.discover() }
                    Spacer()
                }
                ForEach(model.discovered) { r in
                    HStack {
                        VStack(alignment: .leading) {
                            Text(r.name)
                            Text(r.hosts(prefer: model.prefer).map { Address.urlHost($0) }.joined(separator: "  ") + "  port \(r.port)")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button("Use") { model.connect(reply: r); typed = "" }
                    }
                }
            }
            Section("Network") {
                Picker("Connect by", selection: $model.prefer) {
                    ForEach(IPFamily.allCases) { Text($0.label).tag($0) }
                }
                Text("When the server has both an IPv6 and an IPv4 address, the preferred one is tried first, then the other.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("This Mac as a player") {
                LabeledContent("Name", value: model.devices.name)
                Text("Other Aurora68 Plays and the server's web panel list this Mac by its computer name (System Settings \u{203A} General \u{203A} Sharing) and can play music on it.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .frame(width: 480)
        .padding(.vertical, 8)
        .onAppear { typed = model.address }
    }

    private var statusText: String {
        switch model.status {
        case .connected: return "Connected to \(model.serverName) at \(model.api?.base.absoluteString ?? "")"
        case .connecting(let n): return "Connecting to \(n)\u{2026}"
        case .searching: return "Looking on the network\u{2026}"
        case .failed(let why): return why
        case .idle: return "Not connected"
        }
    }
}
