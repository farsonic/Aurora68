// SPDX-License-Identifier: MPL-2.0
//
// PlayerViews.swift - the player bar, the "Play on" choice, the Live view and the settings.
//
// Backstory (22 November 2026). The bar along the bottom is the one place to control what plays: the
// song and its cover, Previous, Play/Pause and Next, shuffle and repeat, the position (drag to seek),
// the volume. With "Play on: Live (Mac SE)" chosen, it shows and controls the server's Live channel
// instead, which the SE hears; the Live view in the sidebar shows that channel's queue and listeners.
import SwiftUI
import Aurora68Core

struct OutputPicker: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        Picker(selection: $model.output) {
            ForEach(AppModel.Output.allCases) { o in
                Label(o.label, systemImage: o.symbol).tag(o)
            }
        } label: {
            Label("Play on", systemImage: model.output.symbol)
        }
        .pickerStyle(.menu)
        .help("Play on this Mac, or on the Live channel the Mac SE listens to")
    }
}

struct PlayerBar: View {
    @EnvironmentObject var model: AppModel
    @EnvironmentObject var player: Player
    @EnvironmentObject var live: LiveController
    @State private var scrub: Double?

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
                if model.output == .live {
                    Button { player.listeningLive ? player.stopListeningLive() : player.listenLive() } label: {
                        Image(systemName: player.listeningLive ? "headphones.circle.fill" : "headphones")
                    }
                    .buttonStyle(.borderless)
                    .help(player.listeningLive ? "Stop listening here" : "Listen to Live here as well")
                }
                Image(systemName: "speaker.fill").foregroundStyle(.secondary)
                Slider(value: $player.volume, in: 0...1).frame(width: 100)
            }
            .frame(width: 180, alignment: .trailing)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
        .background(.bar)
    }

    // ---- left: the song
    @ViewBuilder private var nowPlaying: some View {
        HStack(spacing: 10) {
            Cover(url: coverURL, corner: 5).frame(width: 48, height: 48)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.callout.weight(.semibold)).lineLimit(1)
                Text(subtitle).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                if model.output == .live {
                    Label(liveListeners, systemImage: "dot.radiowaves.left.and.right")
                        .font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                }
            }
        }
    }

    private var coverURL: URL? {
        if model.output == .live { return live.now?.current.flatMap { model.coverURL(folder: $0.folder) } }
        return player.current.flatMap { model.coverURL(folder: $0.folder) }
    }

    private var title: String {
        if model.output == .live { return live.now?.current?.title ?? "Live: nothing queued" }
        if player.listeningLive { return "Live" }
        return player.current?.title ?? "Not playing"
    }

    private var subtitle: String {
        if model.output == .live {
            guard let c = live.now?.current else { return live.error ?? "Choose an album to play on the Mac SE" }
            return c.folder.split(separator: "/").last.map(String.init) ?? ""
        }
        guard let t = player.current else { return player.error ?? "" }
        return "\(t.artist) \u{2014} \(t.albumTitle)"
    }

    private var liveListeners: String {
        let n = live.now?.listeners.count ?? 0
        return n == 0 ? "No one listening" : n == 1 ? "1 listening" : "\(n) listening"
    }

    // ---- middle: the buttons and the position
    private var playing: Bool {
        if model.output == .live && !player.listeningLive { return (live.now?.playing ?? false) }
        return player.isPlaying
    }

    @ViewBuilder private var controls: some View {
        HStack(spacing: 18) {
            Button { shuffle() } label: { Image(systemName: "shuffle") }
                .help(model.output == .live ? "Shuffle what is still to come on Live" : "Shuffle what is still to come")
            Button { model.previous() } label: { Image(systemName: "backward.fill") }
            Button { model.togglePlay() } label: {
                Image(systemName: playing ? "pause.circle.fill" : "play.circle.fill").font(.system(size: 30))
            }
            Button { model.next() } label: { Image(systemName: "forward.fill") }
            Button { cycleRepeat() } label: {
                Image(systemName: repeatSymbol).foregroundStyle(repeatOn ? Color.accentColor : Color.primary)
            }
            .help("Repeat")
        }
        .buttonStyle(.borderless)
        .font(.system(size: 14))
    }

    private var repeatOn: Bool {
        model.output == .live ? (live.now?.`repeat` ?? false) : player.repeatMode != .off
    }

    private var repeatSymbol: String {
        model.output == .thisMac && player.repeatMode == .one ? "repeat.1" : "repeat"
    }

    private func shuffle() {
        if model.output == .live { Task { await live.shuffleRest() } } else { player.shuffleUpcoming() }
    }

    private func cycleRepeat() {
        if model.output == .live { Task { await live.setRepeat(!(live.now?.`repeat` ?? false)) } }
        else { player.repeatMode = player.repeatMode.next }
    }

    @ViewBuilder private var position: some View {
        let now = times.0
        let total = times.1
        HStack(spacing: 8) {
            Text(TrackName.time(scrub ?? now)).frame(width: 44, alignment: .trailing)
            if model.output == .live {
                ProgressView(value: total > 0 ? min(now, total) : 0, total: max(total, 1))
            } else {
                Slider(value: Binding(get: { scrub ?? now }, set: { scrub = $0 }), in: 0...max(total, 1),
                       onEditingChanged: { editing in
                           if !editing, let s = scrub { player.seek(to: s); scrub = nil }
                       })
                .disabled(player.current == nil)
            }
            Text(total > 0 ? TrackName.time(total) : "--:--").frame(width: 44, alignment: .leading)
        }
        .font(.caption.monospacedDigit())
        .foregroundStyle(.secondary)
        .controlSize(.small)
    }

    private var times: (Double, Double) {
        if model.output == .live {
            let c = live.now?.current
            return (c?.elapsed ?? 0, c?.length ?? 0)
        }
        return (player.elapsed, player.duration)
    }
}

struct LiveView: View {
    @EnvironmentObject var model: AppModel
    @EnvironmentObject var live: LiveController
    @EnvironmentObject var player: Player

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                HStack(alignment: .bottom, spacing: 24) {
                    Cover(url: live.now?.current.flatMap { model.coverURL(folder: $0.folder) }, corner: 10)
                        .frame(width: 200)
                    VStack(alignment: .leading, spacing: 6) {
                        Text("LIVE \u{2014} WHAT THE MAC SE HEARS").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                        Text(live.now?.current?.title ?? "Nothing queued").font(.largeTitle.bold()).lineLimit(2)
                        Text(live.now?.paused == true ? "Paused" : (live.now?.current == nil ? "Choose an album with \u{201C}Play on: Live\u{201D}" : "Playing"))
                            .foregroundStyle(.secondary)
                        HStack {
                            Picker("Play on", selection: $model.output) {
                                ForEach(AppModel.Output.allCases) { Text($0.label).tag($0) }
                            }
                            .frame(width: 220)
                            Button { player.listeningLive ? player.stopListeningLive() : player.listenLive() } label: {
                                Label(player.listeningLive ? "Stop Listening" : "Listen Here", systemImage: "headphones")
                            }
                            Button("Clear Queue") { Task { await live.clear() } }.disabled(live.now?.queue.isEmpty ?? true)
                        }
                        .padding(.top, 8)
                    }
                }
                VStack(alignment: .leading, spacing: 6) {
                    Text("Queue").font(.title3.bold())
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
                        Text("Empty. Choose \u{201C}Play on: Live (Mac SE)\u{201D} in the toolbar, then play an album or a song.")
                            .foregroundStyle(.secondary)
                    }
                }
                VStack(alignment: .leading, spacing: 6) {
                    Text("Listening").font(.title3.bold())
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
                if let e = live.error { Text(e).foregroundStyle(.red).font(.callout) }
            }
            .padding(24)
        }
        .navigationTitle("Live")
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
