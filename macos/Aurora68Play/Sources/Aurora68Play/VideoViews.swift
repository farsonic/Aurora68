// SPDX-License-Identifier: MPL-2.0
//
// VideoViews.swift - music videos: their posters in the library, and the window that shows one playing.
//
// Backstory (22 November 2026, docs/08 item 109). The owner's music library also holds music videos, in the same
// folders as the albums, and the music server now lists them (tools/music-server/README.md, "Music videos"). Here
// they appear after an album's songs and after an artist's albums, each as its poster (the server's /vthumb/ID, a
// frame a tenth of the way in), its title and its length. Playing one puts it in the queue like a song (Track with
// `video` set), on This Mac or wherever "Play on" says; the Player plays it from the same AVQueuePlayer, and the
// Video window shows that player's picture in AVKit's AVPlayerView: its own play, pause and position controls, and
// full screen (the green button, or the view's own). The window opens by itself when a video begins (ContentView)
// and from the player bar's film button; closing it leaves the sound playing, as a TV in another room would.
//
// 1.4 (23 November 2026, docs/08 item 115): an artist's videos as a row near the top of its page (VideoRow, See All
// opening VideoListView), and the sidebar's Videos page (AllVideosView: every video, grouped by artist, filtered by
// the search field). An album's videos now include those of its songs that sit in the artist's folder.
import AVKit
import SwiftUI
import Aurora68Core

/// the Video window: the player's picture, or a word when no video plays
struct VideoWindow: View {
    @EnvironmentObject var player: Player

    var body: some View {
        ZStack {
            Color.black
            PlayerPicture(player: player.avPlayer)
            if !player.currentIsVideo {
                VStack(spacing: 8) {
                    Image(systemName: "film").font(.system(size: 40))
                    Text(player.current == nil ? "No video is playing" : "A song is playing; a video shows here when one plays")
                }
                .foregroundStyle(.secondary)
                .allowsHitTesting(false)
            }
        }
        .frame(minWidth: 480, minHeight: 270)
        .navigationTitle(player.currentIsVideo ? player.current.map { "\($0.artist) \u{2014} \($0.title)" } ?? "Video" : "Video")
    }
}

/// the playing video filling the main window's right side (1.5, 23 November 2026: the owner, "expand/shrink the video by
/// pressing the icon in the bottom left"): the player bar's picture switches between this and the library; a video's
/// start expands it, a song shrinks it. The same AVPlayer as the Video window's, so both may show it at once.
struct ExpandedVideo: View {
    @EnvironmentObject var model: AppModel
    @EnvironmentObject var player: Player
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        ZStack(alignment: .topTrailing) {
            Color.black
            PlayerPicture(player: player.avPlayer)
            HStack(spacing: 6) {
                Button { openWindow(id: "video"); model.videoExpanded = false } label: { Image(systemName: "macwindow.on.rectangle") }
                    .help("Show the video in a window of its own")
                Button { model.videoExpanded = false } label: { Image(systemName: "arrow.down.right.and.arrow.up.left") }
                    .help("Shrink the video back into the bar")
                    .keyboardShortcut(.escape, modifiers: [])
            }
            .buttonStyle(.borderless)
            .padding(6)
            .background(.black.opacity(0.5), in: RoundedRectangle(cornerRadius: 6))
            .foregroundStyle(.white)
            .padding(10)
        }
        .navigationTitle(player.current.map { "\($0.artist) \u{2014} \($0.title)" } ?? "Video")
    }
}

/// AVKit's player view over the app's player: floating controls, a full-screen button, the picture fitted
struct PlayerPicture: NSViewRepresentable {
    let player: AVPlayer

    func makeNSView(context: Context) -> AVPlayerView {
        let v = AVPlayerView()
        v.player = player
        v.controlsStyle = .floating
        v.showsFullScreenToggleButton = true
        v.videoGravity = .resizeAspect
        return v
    }

    func updateNSView(_ v: AVPlayerView, context: Context) {
        if v.player !== player { v.player = player }
    }
}

private let videoColumns = [GridItem(.adaptive(minimum: 200, maximum: 300), spacing: 20)]

/// a list of videos as posters: a click plays one (and those after it follow), the menu adds them to the queue
struct VideoGrid: View {
    @EnvironmentObject var model: AppModel
    @EnvironmentObject var player: Player
    let videos: [Track]
    var heading: String? = "Videos"

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let heading { Text(heading).font(.title2.bold()) }
            LazyVGrid(columns: videoColumns, alignment: .leading, spacing: 20) {
                ForEach(Array(videos.enumerated()), id: \.element.id) { i, v in
                    VideoTile(video: v, poster: model.posterURL(v), playing: player.current?.id == v.id && model.output == .thisMac)
                        .onTapGesture { model.play(videos, from: i) }
                        .contextMenu { VideoMenu(model: model, videos: videos, index: i) }
                }
            }
        }
        .padding(.top, 8)
    }
}

/// a video poster's menu (the model handed over: a menu is not sure to see the window's environment)
struct VideoMenu: View {
    let model: AppModel
    let videos: [Track]
    let index: Int

    var body: some View {
        Button("Play") { model.play(videos, from: index) }
        Button("Play Next") { model.playNext([videos[index]]) }
        Button("Add to Queue") { model.add([videos[index]]) }
    }
}

/// a page of videos (an artist's See All): the value a NavigationLink opens
struct VideoList: Hashable {
    let title: String
    let videos: [Track]
}

struct VideoListView: View {
    let list: VideoList

    var body: some View {
        ScrollView {
            VideoGrid(videos: list.videos, heading: nil).padding(24)
        }
        .navigationTitle(list.title)
    }
}

/// an artist's videos as one row of posters that scrolls sideways (23 November 2026: near the top of its page, where the
/// owner looks first), with See All for the whole grid
struct VideoRow: View {
    @EnvironmentObject var model: AppModel
    @EnvironmentObject var player: Player
    let videos: [Track]
    let all: VideoList

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline) {
                Text("Videos").font(.title2.bold())
                Text("\(videos.count)").foregroundStyle(.secondary)
                Spacer()
                NavigationLink(value: all) { Text("See All") }
                    .buttonStyle(.link)
            }
            .padding(.horizontal, 24)
            ScrollView(.horizontal, showsIndicators: false) {
                LazyHStack(alignment: .top, spacing: 18) {
                    ForEach(Array(videos.enumerated()), id: \.element.id) { i, v in
                        VideoTile(video: v, poster: model.posterURL(v), playing: player.current?.id == v.id && model.output == .thisMac)
                            .frame(width: 220)
                            .onTapGesture { model.play(videos, from: i) }
                            .contextMenu { VideoMenu(model: model, videos: videos, index: i) }
                    }
                }
                .padding(.horizontal, 24)
                .padding(.vertical, 4)
            }
        }
    }
}

/// the sidebar's Videos: every music video of the library, by artist; the search field filters it
struct AllVideosView: View {
    @EnvironmentObject var model: AppModel
    @State private var videos: [Track] = []
    @State private var loading = true

    var body: some View {
        let shelf = VideoShelf(videos, matching: model.search)
        ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                if loading {
                    ProgressView().frame(maxWidth: .infinity)
                } else if shelf.groups.isEmpty {
                    Text(videos.isEmpty ? "There are no music videos in the library." : "No video matches \u{201C}\(model.search)\u{201D}.")
                        .foregroundStyle(.secondary)
                }
                ForEach(shelf.groups) { g in
                    VideoGrid(videos: g.videos, heading: g.artist)
                }
            }
            .padding(24)
        }
        .navigationTitle("Videos")
        .task(id: model.generation) {                     // again when the library changed
            loading = videos.isEmpty
            videos = await model.everyVideo()
            loading = false
        }
    }
}

struct VideoTile: View {
    let video: Track
    let poster: URL?
    let playing: Bool
    @State private var hover = false

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            ZStack {
                RoundedRectangle(cornerRadius: 8).fill(.quaternary)
                Image(systemName: "film").font(.largeTitle).foregroundStyle(.secondary)
                if let poster {
                    AsyncImage(url: poster, transaction: Transaction(animation: .easeIn(duration: 0.2))) { phase in
                        if case .success(let image) = phase { image.resizable().scaledToFill() }
                    }
                }
                Image(systemName: playing ? "speaker.wave.2.fill" : "play.circle.fill")
                    .font(.system(size: 34))
                    .foregroundStyle(.white)
                    .shadow(radius: 4)
                    .opacity(hover || playing ? 1 : 0.0)
                Text(video.seconds > 0 ? TrackName.time(Double(video.seconds)) : "")
                    .font(.caption.monospacedDigit().weight(.semibold))
                    .padding(.horizontal, 5).padding(.vertical, 2)
                    .background(.black.opacity(0.6), in: RoundedRectangle(cornerRadius: 4))
                    .foregroundStyle(.white)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
                    .padding(6)
            }
            .aspectRatio(16.0 / 9.0, contentMode: .fit)
            .clipShape(RoundedRectangle(cornerRadius: 8))
            .shadow(color: .black.opacity(0.15), radius: 4, y: 2)
            Text(video.title).font(.headline).lineLimit(1).foregroundStyle(playing ? Color.accentColor : .primary)
            Text(video.albumTitle.isEmpty ? video.artist : "\(video.artist) \u{2014} \(video.albumTitle)")
                .font(.subheadline).foregroundStyle(.secondary).lineLimit(1)
        }
        .contentShape(Rectangle())
        .onHover { hover = $0 }
        .help(video.video.map { "\($0.width)\u{00D7}\($0.height), \($0.how.rawValue)" } ?? "")
    }
}
