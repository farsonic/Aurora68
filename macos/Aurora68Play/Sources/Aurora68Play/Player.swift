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
// keyboard's media keys work. "Listen here" plays the server's Live channel (/live) instead.
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
    @Published private(set) var listeningLive = false
    @Published private(set) var error: String?
    @Published var repeatMode: RepeatMode = .off {
        didSet { queue.repeatMode = repeatMode; refillNext() }
    }
    @Published var volume: Double = UserDefaults.standard.object(forKey: "volume") as? Double ?? 0.8 {
        didSet { av.volume = Float(volume); UserDefaults.standard.set(volume, forKey: "volume") }
    }

    var api: MusicAPI?
    var current: Track? { listeningLive ? nil : queue.current }

    private let av = AVQueuePlayer()
    private var tracks: [ObjectIdentifier: Track] = [:]        // AVPlayerItem -> its song
    private var timeObserver: Any?
    private var bag = Set<AnyCancellable>()
    private var artwork: (folder: String, image: NSImage)?

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
            if listeningLive { listenLive() } else if queue.current != nil { rebuild() }
            return
        }
        av.timeControlStatus == .paused ? av.play() : av.pause()
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
        av.seek(to: CMTime(seconds: max(0, seconds), preferredTimescale: 600))
        elapsed = seconds
        updateNowPlaying()
    }

    /// play the server's Live channel here (the SE hears the same)
    func listenLive() {
        guard let api else { return }
        av.removeAllItems()
        tracks.removeAll()
        listeningLive = true
        av.insert(AVPlayerItem(url: api.liveURL), after: nil)
        av.play()
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
        let it = AVPlayerItem(url: api.fileURL(t.path))
        tracks[ObjectIdentifier(it)] = t
        return it
    }

    private func rebuild() {
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
        elapsed = t.seconds.isFinite ? t.seconds : 0
        if let d = av.currentItem?.duration.seconds, d.isFinite, d > 0 { duration = d }
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
            info[MPMediaItemPropertyTitle] = "Live"
            info[MPNowPlayingInfoPropertyIsLiveStream] = true
        } else if let t = queue.current {
            info[MPMediaItemPropertyTitle] = t.title
            info[MPMediaItemPropertyArtist] = t.artist
            info[MPMediaItemPropertyAlbumTitle] = t.albumTitle
            info[MPMediaItemPropertyPlaybackDuration] = duration
            info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = elapsed
            if let art = artwork, art.folder == t.folder {
                let image = art.image
                info[MPMediaItemPropertyArtwork] = MPMediaItemArtwork(boundsSize: image.size) { _ in image }
            } else {
                loadArtwork(folder: t.folder)
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

    private func loadArtwork(folder: String) {
        guard let api else { return }
        let url = api.artURL(folder: folder)
        Task { [weak self] in
            guard let result = try? await URLSession.shared.data(from: url) else { return }
            let (data, resp) = result
            guard (resp as? HTTPURLResponse)?.statusCode == 200, let image = NSImage(data: data) else { return }
            await MainActor.run {
                self?.artwork = (folder, image)
                self?.updateNowPlaying()
            }
        }
    }
}
