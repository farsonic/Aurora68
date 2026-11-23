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

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Videos").font(.title2.bold())
            LazyVGrid(columns: videoColumns, alignment: .leading, spacing: 20) {
                ForEach(Array(videos.enumerated()), id: \.element.id) { i, v in
                    VideoTile(video: v, poster: model.posterURL(v), playing: player.current?.id == v.id && model.output == .thisMac)
                        .onTapGesture { model.play(videos, from: i) }
                        .contextMenu {
                            Button("Play") { model.play(videos, from: i) }
                            Button("Play Next") { model.playNext([v]) }
                            Button("Add to Queue") { model.add([v]) }
                        }
                }
            }
        }
        .padding(.top, 8)
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
