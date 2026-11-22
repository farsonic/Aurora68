// SPDX-License-Identifier: MPL-2.0
//
// Player.swift - playing on this Mac: the queue, AVFoundation, and the system's Now Playing.
//
// Backstory (22 November 2026). On a modern Mac nothing needs converting: the server's /file/PATH sends
// a song as it is, with Range, and AVFoundation plays MP3, AAC and Apple Lossless (M4A) and FLAC and
// seeks in them. So the player asks for the original files: full quality, real seeking, no work for
// the server. AVQueuePlayer holds the playing item and the next one, so songs follow without a gap;
// the queue the owner sees is Aurora68Core's PlayQueue, which says what plays now and what follows.
// MPNowPlayingInfoCenter and MPRemoteCommandCenter put the song in Control Center and make the
// keyboard's media keys work. "Listen here" plays the server's Live channel (/live) instead, and an internet
// radio station (from the Live page, or sent by another player) plays the same way, as a stream.
//
// Seeking (22 November 2026). The owner could not jump to a place in a song on the Mac (on the classic
// Play it works). The position bar was a SwiftUI Slider that seeked only in onEditingChanged(false), which
// macOS does not send for a click on the bar, so a click moved the knob, left it held there, and never
// seeked. The bar is now Scrubber (PlayerViews.swift), which seeks on every click and at the end of every
// drag; and seek(to:) here seeks exactly (zero tolerance, which AVFoundation otherwise widens to the
// nearest convenient point, seconds away in a long MP3), waits for an item that is not ready yet, and
// keeps the periodic clock from showing the old place until the seek has finished.
//
// Music videos (22 November 2026, docs/08 item 109). A video is a Track with `video` set, and plays from the same
// AVQueuePlayer: its item is the server's /video/ID (the file as it is) or /video/ID/index.m3u8 (HLS, for one the
// server remuxes or transcodes: AVFoundation plays no progressive stream without a length), and VideoWindow
// (VideoViews.swift) shows the player's picture. So a video in the queue plays as a video, and the songs after it
// follow it as they follow each other; Now Playing says it is a video and shows its poster.
import AppKit
import AVFoundation
import Combine
import MediaPlayer
import Aurora68Core

@MainActor
final class Player: ObservableObject {
    @Published private(set) var queue = PlayQueue<Track>()
    @Published private(set) var isPlaying = false
    @Published private(set) var elapsed: Double = 0
    @Published private(set) var duration: Double = 0
    @Published private(set) var listeningLive = false          // a stream plays: Live, or a radio station
    @Published private(set) var streamTitle = "Live"
    @Published private(set) var error: String?
    @Published var repeatMode: RepeatMode = .off {
        didSet { queue.repeatMode = repeatMode; refillNext() }
    }
    @Published var volume: Double = UserDefaults.standard.object(forKey: "volume") as? Double ?? 0.8 {
        didSet { av.volume = Float(volume); UserDefaults.standard.set(volume, forKey: "volume") }
    }

    var api: MusicAPI?
    var current: Track? { listeningLive ? nil : queue.current }
    /// what plays is a music video: the video window shows it
    var currentIsVideo: Bool { current?.isVideo ?? false }
    /// the player itself, for the video window's AVPlayerView
    var avPlayer: AVPlayer { av }

    private let av = AVQueuePlayer()
    private var tracks: [ObjectIdentifier: Track] = [:]        // AVPlayerItem -> its song
    private var timeObserver: Any?
    private var bag = Set<AnyCancellable>()
    private var artwork: (key: String, image: NSImage)?
    private var seeking = false                                  // a seek under way: the clock waits for it
    private var pendingSeek: Double?                             // asked for before the item was ready
    private var streamURL: URL?

    init() {
        av.volume = Float(volume)
        av.actionAtItemEnd = .advance
        timeObserver = av.addPeriodicTimeObserver(forInterval: CMTime(seconds: 0.5, preferredTimescale: 600), queue: .main) { [weak self] t in
            Task { @MainActor in self?.tick(t) }
        }
        av.publisher(for: \.timeControlStatus)
            .receive(on: RunLoop.main)
            .sink { [weak self] s in
                self?.isPlaying = (s != .paused)
                self?.updateNowPlaying()
            }
            .store(in: &bag)
        NotificationCenter.default.publisher(for: .AVPlayerItemDidPlayToEndTime)
            .receive(on: RunLoop.main)
            .sink { [weak self] n in self?.itemEnded(n.object as? AVPlayerItem) }
            .store(in: &bag)
        NotificationCenter.default.publisher(for: .AVPlayerItemFailedToPlayToEndTime)
            .receive(on: RunLoop.main)
            .sink { [weak self] n in
                self?.error = (n.userInfo?[AVPlayerItemFailedToPlayToEndTimeErrorKey] as? Error)?.localizedDescription
                self?.skip()
            }
            .store(in: &bag)
        setupRemoteCommands()
    }

    // ---- what the owner does
    func play(_ songs: [Track], from start: Int = 0, shuffled: Bool = false) {
        guard !songs.isEmpty else { return }
        var list = songs
        var first = start
        if shuffled { list.shuffle(); first = 0 }
        listeningLive = false
        queue.replace(with: list, start: first)
        queue.repeatMode = repeatMode
        rebuild()
    }

    func playNext(_ songs: [Track]) {
        let wasEmpty = queue.current == nil
        queue.playNext(songs)
        wasEmpty ? rebuild() : refillNext()
    }

    func add(_ songs: [Track]) {
        let wasEmpty = queue.current == nil
        queue.append(songs)
        wasEmpty ? rebuild() : refillNext()
    }

    func remove(at i: Int) {
        let playing = queue.index
        queue.remove(at: i)
        if i == playing { rebuild() } else { refillNext() }
    }

    func jump(to i: Int) {
        guard queue.jump(to: i) != nil else { return }
        listeningLive = false
        rebuild()
    }

    func shuffleUpcoming() {
        queue.shuffleUpcoming()
        refillNext()
    }

    func clear() {
        queue.clear()
        av.removeAllItems()
        tracks.removeAll()
        elapsed = 0; duration = 0
        updateNowPlaying()
    }

    func togglePlay() {
        if av.currentItem == nil {
            if listeningLive, let u = streamURL { playStream(title: streamTitle, url: u) } else if queue.current != nil { rebuild() }
            return
        }
        av.timeControlStatus == .paused ? av.play() : av.pause()
    }

    func pause() { av.pause() }

    func resume() {
        if av.currentItem == nil { togglePlay() } else { av.play() }
    }

    /// everything stops: the queue is emptied, a stream ends
    func stop() {
        listeningLive = false
        clear()
    }

    func skip() {
        guard !listeningLive else { return }
        queue.skip()
        rebuild()
    }

    func previous() {
        guard !listeningLive else { return }
        let r = queue.previous(elapsed: elapsed)
        if r.restart { seek(to: 0) } else { rebuild() }
    }

    func seek(to seconds: Double) {
        guard !listeningLive else { return }
        var target = max(0, seconds)
        if duration > 1 { target = min(target, duration - 0.5) }
        elapsed = target
        updateNowPlaying()
        guard let item = av.currentItem else { return }
        guard item.status == .readyToPlay else { pendingSeek = target; return }   // done when it is ready (tick)
        seeking = true
        av.seek(to: CMTime(seconds: target, preferredTimescale: 600), toleranceBefore: .zero, toleranceAfter: .zero) { [weak self] _ in
            Task { @MainActor in
                self?.seeking = false
                self?.updateNowPlaying()
            }
        }
    }

    /// play the server's Live channel here (the SE hears the same)
    func listenLive() {
        guard let api else { return }
        playStream(title: "Live", url: api.liveURL)
    }

    /// a stream: Live, a radio station, the server's whole library shuffled. A station's address that is a
    /// playlist (.pls, .m3u) is fetched and its first stream played, as the classic Play does (the shared core's
    /// icy_playlist_url, since 22 November 2026): AVFoundation plays streams, not playlists
    func playStream(title: String, url: URL) {
        guard Playlist.isPlaylist(url) else { return startStream(title: title, url: url) }
        Task { [weak self] in
            var stream = url
            if let r = try? await URLSession.shared.data(from: url), let first = Playlist.firstStream(r.0) { stream = first }
            self?.startStream(title: title, url: stream)
        }
    }

    private func startStream(title: String, url: URL) {
        av.removeAllItems()
        tracks.removeAll()
        listeningLive = true
        streamTitle = title
        streamURL = url
        elapsed = 0; duration = 0
        av.insert(AVPlayerItem(url: url), after: nil)
        av.play()
        error = nil
        updateNowPlaying()
    }

    func stopListeningLive() {
        guard listeningLive else { return }
        listeningLive = false
        av.removeAllItems()
        updateNowPlaying()
    }

    // ---- AVFoundation's queue: the playing item and the next
    private func item(for t: Track) -> AVPlayerItem? {
        guard let api else { return nil }
        let it = AVPlayerItem(url: api.playURL(t))          // a song's file, or a video (the file or its HLS)
        tracks[ObjectIdentifier(it)] = t
        return it
    }

    private func rebuild() {
        pendingSeek = nil
        seeking = false
        av.removeAllItems()
        tracks.removeAll()
        elapsed = 0
        duration = Double(queue.current?.seconds ?? 0)
        guard let cur = queue.current, let first = item(for: cur) else { updateNowPlaying(); return }
        av.insert(first, after: nil)
        if let next = queue.peekNext(), let it = item(for: next) { av.insert(it, after: first) }
        av.play()
        error = nil
        updateNowPlaying()
    }

    /// what follows the playing item changed: the next item is made again
    private func refillNext() {
        guard !listeningLive, let playing = av.currentItem else { return }
        for it in av.items() where it !== playing {
            av.remove(it)
            tracks[ObjectIdentifier(it)] = nil
        }
        if let next = queue.peekNext(), let it = item(for: next) { av.insert(it, after: playing) }
    }

    private func itemEnded(_ ended: AVPlayerItem?) {
        guard let ended, !listeningLive, tracks[ObjectIdentifier(ended)] != nil else { return }
        tracks[ObjectIdentifier(ended)] = nil
        queue.advance()                                    // AVQueuePlayer has moved on to the next item
        if queue.current == nil { av.removeAllItems(); elapsed = 0; updateNowPlaying(); return }
        duration = Double(queue.current?.seconds ?? 0)
        if let playing = av.currentItem, av.items().count < 2, let next = queue.peekNext(), let it = item(for: next) {
            av.insert(it, after: playing)
        } else if av.currentItem == nil {
            rebuild()
        }
        updateNowPlaying()
    }

    private func tick(_ t: CMTime) {
        if let p = pendingSeek, av.currentItem?.status == .readyToPlay {
            pendingSeek = nil
            seek(to: p)
            return
        }
        guard !seeking, pendingSeek == nil else { return }
        elapsed = t.seconds.isFinite ? t.seconds : 0
        if !listeningLive, let d = av.currentItem?.duration.seconds, d.isFinite, d > 0 { duration = d }
    }

    // ---- the system's Now Playing and media keys
    private func setupRemoteCommands() {
        let c = MPRemoteCommandCenter.shared()
        c.playCommand.addTarget { [weak self] _ in Task { @MainActor in self?.av.play() }; return .success }
        c.pauseCommand.addTarget { [weak self] _ in Task { @MainActor in self?.av.pause() }; return .success }
        c.togglePlayPauseCommand.addTarget { [weak self] _ in Task { @MainActor in self?.togglePlay() }; return .success }
        c.nextTrackCommand.addTarget { [weak self] _ in Task { @MainActor in self?.skip() }; return .success }
        c.previousTrackCommand.addTarget { [weak self] _ in Task { @MainActor in self?.previous() }; return .success }
        c.changePlaybackPositionCommand.addTarget { [weak self] e in
            guard let e = e as? MPChangePlaybackPositionCommandEvent else { return .commandFailed }
            let pos = e.positionTime
            Task { @MainActor in self?.seek(to: pos) }
            return .success
        }
    }

    private func updateNowPlaying() {
        let center = MPNowPlayingInfoCenter.default()
        var info: [String: Any] = [:]
        if listeningLive {
            info[MPMediaItemPropertyTitle] = streamTitle
            info[MPNowPlayingInfoPropertyIsLiveStream] = true
        } else if let t = queue.current {
            info[MPMediaItemPropertyTitle] = t.title
            info[MPMediaItemPropertyArtist] = t.artist
            info[MPMediaItemPropertyAlbumTitle] = t.albumTitle
            info[MPMediaItemPropertyPlaybackDuration] = duration
            info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = elapsed
            info[MPNowPlayingInfoPropertyMediaType] = t.isVideo ? MPNowPlayingInfoMediaType.video.rawValue
                                                                : MPNowPlayingInfoMediaType.audio.rawValue
            let key = t.isVideo ? "video:" + t.id : t.folder         // a video's poster, else the album's cover
            if let art = artwork, art.key == key {
                let image = art.image
                info[MPMediaItemPropertyArtwork] = MPMediaItemArtwork(boundsSize: image.size) { _ in image }
            } else if let api {
                loadArtwork(key: key, url: t.isVideo ? api.posterURL(t.id) : api.artURL(folder: t.folder))
            }
        } else {
            center.nowPlayingInfo = nil
            center.playbackState = .stopped
            return
        }
        info[MPNowPlayingInfoPropertyPlaybackRate] = isPlaying ? 1.0 : 0.0
        center.nowPlayingInfo = info
        center.playbackState = isPlaying ? .playing : .paused
    }

    private func loadArtwork(key: String, url: URL) {
        Task { [weak self] in
            guard let result = try? await URLSession.shared.data(from: url) else { return }
            let (data, resp) = result
            guard (resp as? HTTPURLResponse)?.statusCode == 200, let image = NSImage(data: data) else { return }
            await MainActor.run {
                self?.artwork = (key, image)
                self?.updateNowPlaying()
            }
        }
    }
}
