// SPDX-License-Identifier: MPL-2.0
//
// LiveController.swift - the server's Live channel, which the Mac SE listens to, driven from here.
//
// Backstory (22 November 2026). The server's Live channel is one stream every listener hears at once;
// the SE's Aurora68 Play tunes to it, and the control panel (a web page) chooses what it plays. The
// owner wanted the macOS app to be "Spotify-like", so it has Spotify Connect's idea: a "Play on" choice
// of This Mac or Live (Mac SE). With Live chosen, Play and the queue buttons send their changes to the
// server (POST /api/live) and the player bar shows what Live plays (GET /api/now, every two seconds).
import Foundation
import Aurora68Core

@MainActor
final class LiveController: ObservableObject {
    @Published private(set) var now: LiveNow?
    @Published private(set) var error: String?

    var api: MusicAPI? {
        didSet { now = nil; startPolling() }
    }
    private var polling: Task<Void, Never>?

    func startPolling() {
        polling?.cancel()
        guard api != nil else { return }
        polling = Task { [weak self] in
            while !Task.isCancelled {
                await self?.refresh()
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
        }
    }

    func refresh() async {
        guard let api else { return }
        do {
            let n = try await api.now()
            if n != now { now = n }
            error = nil
        } catch {
            self.error = error.localizedDescription
        }
    }

    /// one change; the answer is the new state, shown at once
    func act(_ body: [String: Any]) async {
        guard let api else { return }
        do {
            now = try await api.live(body)
            error = nil
        } catch {
            self.error = error.localizedDescription
        }
    }

    // ---- what the owner asks of Live
    func play(album: Album, shuffled: Bool = false) async {
        await act(["action": "play", "what": LiveWhat.folder(album.folder), "shuffle": shuffled ? 1 : 0])
    }

    func add(album: Album) async {
        await act(["action": "add", "what": LiveWhat.folder(album.folder)])
    }

    /// an artist: the first album played, the others added after it, shuffled together if asked
    func play(albums: [Album], shuffled: Bool = false) async {
        guard let first = albums.first else { return }
        await act(["action": "play", "what": LiveWhat.folder(first.folder)])
        for a in albums.dropFirst() { await act(["action": "add", "what": LiveWhat.folder(a.folder)]) }
        if shuffled { await act(["action": "jump", "index": 0]); await act(["action": "shuffle"]) }
    }

    /// songs from a list: the chosen one plays now, the ones after it follow
    func play(tracks: [Track], from start: Int) async {
        guard tracks.indices.contains(start) else { return }
        await act(["action": "play", "what": LiveWhat.track(tracks[start].path)])
        for t in tracks[(start + 1)...] { await act(["action": "add", "what": LiveWhat.track(t.path)]) }
    }

    func playNext(_ tracks: [Track]) async {
        for t in tracks.reversed() { await act(["action": "next", "what": LiveWhat.track(t.path)]) }
    }

    func add(_ tracks: [Track]) async {
        for t in tracks { await act(["action": "add", "what": LiveWhat.track(t.path)]) }
    }

    func togglePlay() async {
        guard let n = now else { return }
        await act(["action": n.paused ? "resume" : "pause"])
    }

    func skip() async { await act(["action": "skip"]) }
    func previous() async { await act(["action": "previous"]) }
    func jump(to i: Int) async { await act(["action": "jump", "index": i]) }
    func remove(at i: Int) async { await act(["action": "remove", "index": i]) }
    func clear() async { await act(["action": "clear"]) }
    func shuffleRest() async { await act(["action": "shuffle"]) }
    func setRepeat(_ on: Bool) async { await act(["action": "repeat", "on": on]) }
}
