// SPDX-License-Identifier: MPL-2.0
//
// LibraryViews.swift - the window: a sidebar, the library as covers, an album's songs.
//
// Backstory (22 November 2026). The layout of a modern music app, as the owner asked ("Spotify-like"):
// a sidebar (Artists, Albums, Live, and the server at its foot), a grid of covers, an artist's albums,
// an album with its cover and songs; a search field in the toolbar filters artists and albums as one
// types. Covers come from the server's /art (the folder's cover.jpg, or the picture inside its first
// song) through URLSession's cache, so a grid of a hundred albums is fetched once.
import SwiftUI
import Aurora68Core

struct ContentView: View {
    @EnvironmentObject var model: AppModel
    @EnvironmentObject var player: Player
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(spacing: 0) {
            NavigationSplitView {
                Sidebar()
            } detail: {
                if model.isConnected { DetailRoot() } else { ConnectView() }
            }
            Divider()
            PlayerBar()
        }
        .searchable(text: $model.search, placement: .toolbar, prompt: "Artists and albums")
        .toolbar {
            ToolbarItem(placement: .primaryAction) { OutputPicker() }
        }
        .navigationTitle(model.serverName.isEmpty ? "Aurora68 Play" : model.serverName)
        .onChange(of: player.current?.id) { _ in               // a video begins: its window opens
            if player.currentIsVideo { openWindow(id: "video") }
        }
    }
}

struct Sidebar: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        List(selection: $model.section) {
            Section("Library") {
                Label("Artists", systemImage: "music.mic").tag(AppModel.Section.artists)
                Label("Albums", systemImage: "square.stack").tag(AppModel.Section.albums)
            }
            Section("Stations") {
                Label("Live and Radio", systemImage: "dot.radiowaves.left.and.right").tag(AppModel.Section.live)
            }
        }
        .safeAreaInset(edge: .bottom) { ServerStatus().padding(10) }
        .navigationSplitViewColumnWidth(min: 170, ideal: 200)
    }
}

struct ServerStatus: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        Button { model.openSettings() } label: {
            HStack(spacing: 8) {
                Circle().fill(colour).frame(width: 8, height: 8)
                VStack(alignment: .leading, spacing: 1) {
                    Text(title).font(.callout).lineLimit(1)
                    if let host = model.api?.base.host {
                        Text(host).font(.caption2).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                    }
                }
                Spacer(minLength: 0)
            }
        }
        .buttonStyle(.plain)
        .help("The music server: click to change it")
    }

    private var colour: Color {
        switch model.status {
        case .connected: return .green
        case .failed: return .red
        default: return .orange
        }
    }

    private var title: String {
        switch model.status {
        case .idle: return "Not connected"
        case .searching: return "Looking for a server\u{2026}"
        case .connecting(let n): return "Connecting to \(n)\u{2026}"
        case .connected: return model.serverName
        case .failed: return "Not connected"
        }
    }
}

struct DetailRoot: View {
    @EnvironmentObject var model: AppModel
    @State private var path = NavigationPath()

    var body: some View {
        NavigationStack(path: $path) {
            Group {
                switch model.section ?? .artists {
                case .artists: ArtistsGrid()
                case .albums: AlbumsGrid(albums: model.filteredAlbums, showArtist: true)
                case .live: LiveView()
                }
            }
            .navigationDestination(for: Artist.self) { ArtistView(artist: $0) }
            .navigationDestination(for: Album.self) { AlbumView(album: $0) }
        }
        .onChange(of: model.section) { _ in path = NavigationPath() }
    }
}

struct Cover: View {
    let url: URL?
    var corner: CGFloat = 8

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: corner).fill(.quaternary)
            Image(systemName: "music.note").font(.largeTitle).foregroundStyle(.secondary)
            if let url {
                AsyncImage(url: url, transaction: Transaction(animation: .easeIn(duration: 0.2))) { phase in
                    if case .success(let image) = phase {
                        image.resizable().scaledToFill()
                    }
                }
            }
        }
        .aspectRatio(1, contentMode: .fit)
        .clipShape(RoundedRectangle(cornerRadius: corner))
    }
}

struct Tile: View {
    let cover: URL?
    let title: String
    let subtitle: String

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Cover(url: cover).shadow(color: .black.opacity(0.15), radius: 4, y: 2)
            Text(title).font(.headline).lineLimit(1)
            Text(subtitle).font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
        }
        .contentShape(Rectangle())
    }
}

private let gridColumns = [GridItem(.adaptive(minimum: 150, maximum: 220), spacing: 20)]

struct ArtistsGrid: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        ScrollView {
            LazyVGrid(columns: gridColumns, alignment: .leading, spacing: 24) {
                ForEach(model.filteredArtists) { artist in
                    NavigationLink(value: artist) {
                        Tile(cover: model.coverURL(model.index?.coverAlbum(of: artist)), title: artist.name,
                             subtitle: artistSummary(artist))
                    }
                    .buttonStyle(.plain)
                    .contextMenu {
                        Button("Play") { model.play(artist: artist) }
                        Button("Shuffle") { model.play(artist: artist, shuffled: true) }
                    }
                }
            }
            .padding(24)
        }
        .navigationTitle("Artists")
    }
}

/// "3 albums", "2 albums, 1 video", "4 videos"
func artistSummary(_ a: Artist) -> String {
    var parts: [String] = []
    if !a.albums.isEmpty || a.videos == 0 { parts.append(a.albums.count == 1 ? "1 album" : "\(a.albums.count) albums") }
    if a.videos > 0 { parts.append(a.videos == 1 ? "1 video" : "\(a.videos) videos") }
    return parts.joined(separator: ", ")
}

struct AlbumsGrid: View {
    @EnvironmentObject var model: AppModel
    let albums: [Album]
    var showArtist = false
    var videos: [Track] = []                           // an artist's music videos, after its albums

    var body: some View {
        ScrollView {
            LazyVGrid(columns: gridColumns, alignment: .leading, spacing: 24) {
                ForEach(albums) { album in
                    NavigationLink(value: album) {
                        Tile(cover: model.coverURL(album), title: album.title,
                             subtitle: showArtist ? album.artist : (album.tracks == 1 ? "1 song" : "\(album.tracks) songs"))
                    }
                    .buttonStyle(.plain)
                    .contextMenu {
                        Button("Play") { model.play(album: album) }
                        Button("Shuffle") { model.play(album: album, shuffled: true) }
                        Button("Add to Queue") { model.add(album: album) }
                    }
                }
            }
            .padding(24)
            if !videos.isEmpty { VideoGrid(videos: videos).padding([.horizontal, .bottom], 24) }
        }
        .navigationTitle("Albums")
    }
}

struct ArtistView: View {
    @EnvironmentObject var model: AppModel
    let artist: Artist
    @State private var videos: [Track] = []

    var body: some View {
        let artist = model.current(self.artist)            // its albums as they are now (new ones appear)
        VStack(alignment: .leading, spacing: 0) {
            HStack(alignment: .firstTextBaseline) {
                VStack(alignment: .leading) {
                    Text(artist.name).font(.largeTitle.bold())
                    Text(artistSummary(artist)).foregroundStyle(.secondary)
                }
                Spacer()
                Button { model.play(artist: artist) } label: { Label("Play", systemImage: "play.fill") }
                    .buttonStyle(.borderedProminent)
                Button { model.play(artist: artist, shuffled: true) } label: { Label("Shuffle", systemImage: "shuffle") }
            }
            .padding([.horizontal, .top], 24)
            AlbumsGrid(albums: model.albums(of: artist), videos: videos)
        }
        .navigationTitle(artist.name)
        .task(id: "\(artist.id) \(model.generation)") { videos = await model.videos(artist: model.current(self.artist)) }
    }
}

struct AlbumView: View {
    @EnvironmentObject var model: AppModel
    @EnvironmentObject var player: Player
    let album: Album
    @State private var tracks: [Track] = []
    @State private var videos: [Track] = []
    @State private var loading = true

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                HStack(alignment: .bottom, spacing: 24) {
                    Cover(url: model.coverURL(album), corner: 10)
                        .frame(width: 220)
                        .shadow(color: .black.opacity(0.25), radius: 10, y: 4)
                    VStack(alignment: .leading, spacing: 6) {
                        Text("ALBUM").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                        Text(album.title).font(.largeTitle.bold()).lineLimit(3)
                        Text(album.artist).font(.title3)
                        Text(summary).font(.callout).foregroundStyle(.secondary)
                        HStack {
                            Button { model.play(album: album) } label: { Label("Play", systemImage: "play.fill") }
                                .buttonStyle(.borderedProminent)
                                .keyboardShortcut(.defaultAction)
                            Button { model.play(album: album, shuffled: true) } label: { Label("Shuffle", systemImage: "shuffle") }
                            Button { model.add(tracks) } label: { Label("Add to Queue", systemImage: "text.badge.plus") }
                                .disabled(tracks.isEmpty)
                        }
                        .padding(.top, 8)
                    }
                }
                if loading {
                    ProgressView().frame(maxWidth: .infinity)
                } else {
                    VStack(spacing: 0) {
                        ForEach(Array(tracks.enumerated()), id: \.element.id) { i, t in
                            TrackRow(track: t, position: i + 1, playing: player.current?.id == t.id && model.output == .thisMac,
                                     differentArtist: t.artist != album.artist)
                                .onTapGesture(count: 2) { model.play(tracks, from: i) }
                                .contextMenu {
                                    Button("Play") { model.play(tracks, from: i) }
                                    Button("Play Next") { model.playNext([t]) }
                                    Button("Add to Queue") { model.add([t]) }
                                }
                            Divider().opacity(0.4)
                        }
                    }
                    if !videos.isEmpty { VideoGrid(videos: videos) }
                }
            }
            .padding(24)
        }
        .navigationTitle(album.title)
        .task(id: "\(album.id) \(model.generation)") {     // again when the library changed
            loading = tracks.isEmpty
            let m = await model.media(album: album)
            tracks = m.tracks
            videos = m.videos
            loading = false
        }
    }

    private var summary: String {
        let total = tracks.reduce(0) { $0 + $1.seconds }
        let n = tracks.isEmpty ? album.tracks : tracks.count
        return "\(n) song\(n == 1 ? "" : "s")" + (total > 0 ? ", \(total / 60) min" : "")
    }
}

struct TrackRow: View {
    let track: Track
    let position: Int
    let playing: Bool
    let differentArtist: Bool
    @State private var hover = false

    var body: some View {
        HStack(spacing: 12) {
            Group {
                if playing { Image(systemName: "speaker.wave.2.fill").foregroundStyle(Color.accentColor) }
                else { Text("\(track.number > 0 ? track.number : position)").foregroundStyle(.secondary) }
            }
            .frame(width: 28, alignment: .trailing)
            .font(.callout.monospacedDigit())
            VStack(alignment: .leading, spacing: 1) {
                Text(track.title).fontWeight(playing ? .semibold : .regular).lineLimit(1)
                if differentArtist { Text(track.artist).font(.caption).foregroundStyle(.secondary) }
            }
            Spacer()
            Text(track.seconds > 0 ? TrackName.time(Double(track.seconds)) : "")
                .font(.callout.monospacedDigit()).foregroundStyle(.secondary)
        }
        .padding(.vertical, 7)
        .padding(.horizontal, 8)
        .background(RoundedRectangle(cornerRadius: 6).fill(hover ? Color.primary.opacity(0.06) : .clear))
        .contentShape(Rectangle())
        .onHover { hover = $0 }
    }
}

struct ConnectView: View {
    @EnvironmentObject var model: AppModel
    @State private var typed = ""

    var body: some View {
        VStack(spacing: 16) {
            Image(systemName: "hifispeaker.2").font(.system(size: 48)).foregroundStyle(.secondary)
            Text("Aurora68 Play").font(.title.bold())
            Group {
                switch model.status {
                case .searching: Text("Looking for your music server\u{2026}")
                case .connecting(let n): Text("Connecting to \(n)\u{2026}")
                case .failed(let why): Text(why).foregroundStyle(.red)
                default: Text("Find the music server on your network, or type its address.")
                }
            }
            .multilineTextAlignment(.center)
            .frame(maxWidth: 420)
            HStack {
                TextField("192.168.1.20:8423 or [fd00::20]:8423", text: $typed)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 280)
                    .onSubmit { model.connect(typed: typed) }
                Button("Connect") { model.connect(typed: typed) }.disabled(typed.isEmpty)
            }
            Button("Find on Network") { model.discover(connectToFirst: true) }
            if !model.discovered.isEmpty {
                ForEach(model.discovered) { r in
                    Button { model.connect(reply: r) } label: {
                        Label("\(r.name)  \(r.hosts(prefer: model.prefer).first ?? "")", systemImage: "server.rack")
                    }
                }
            }
        }
        .padding(40)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .onAppear { typed = model.address }
    }
}
