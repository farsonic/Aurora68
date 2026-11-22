// SPDX-License-Identifier: MPL-2.0
//
// LiveController.swift - the server's Live channel, which the Mac SE listens to, driven from here.
//
// Backstory (22 November 2026). The server's Live channel is one stream every listener hears at once;
// the SE's Aurora68 Play tunes to it, and the control panel (a web page) chooses what it plays. The
// owner wanted the macOS app to be "Spotify-like", so it has Spotify Connect's idea: a "Play on" choice
// of This Mac or Live (Mac SE). With Live chosen, Play and the queue buttons send their changes to the
// server and the player bar shows what Live plays, every two seconds. Since the shared core (docs/08 item 107)
// in the server's text, as the SE reads it: GET /mac/command?to=live&... (the core writes it) and /mac/now.
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

    /// one change; then the new state, shown at once
    func act(_ c: Command) async {
        guard let api else { return }
        do {
            now = try await api.live(c)
            error = nil
        } catch {
            self.error = error.localizedDescription
        }
    }

    // ---- what the owner asks of Live
    func play(album: Album, shuffled: Bool = false) async {
        await act(Command(.play, what: LiveWhat.folder(album.folder), shuffle: shuffled))
    }

    func add(album: Album) async {
        await act(Command(.add, what: LiveWhat.folder(album.folder)))
    }

    /// an artist: the first album played, the others added after it, shuffled together if asked
    func play(albums: [Album], shuffled: Bool = false) async {
        guard let first = albums.first else { return }
        await act(Command(.play, what: LiveWhat.folder(first.folder)))
        for a in albums.dropFirst() { await act(Command(.add, what: LiveWhat.folder(a.folder))) }
        if shuffled { await act(Command(.jump, index: 0)); await act(Command(.shuffle)) }
    }

    /// songs from a list: the chosen one plays now, the ones after it follow
    func play(tracks: [Track], from start: Int) async {
        guard tracks.indices.contains(start) else { return }
        await act(Command(.play, what: tracks[start].what))        // a video: Live plays its sound
        for t in tracks[(start + 1)...] { await act(Command(.add, what: t.what)) }
    }

    func playNext(_ tracks: [Track]) async {
        for t in tracks.reversed() { await act(Command(.next, what: t.what)) }
    }

    func add(_ tracks: [Track]) async {
        for t in tracks { await act(Command(.add, what: t.what)) }
    }

    func togglePlay() async {
        guard let n = now else { return }
        await act(Command(n.paused ? .resume : .pause))
    }

    func skip() async { await act(Command(.skip)) }
    func previous() async { await act(Command(.previous)) }
    func jump(to i: Int) async { await act(Command(.jump, index: i)) }
    func remove(at i: Int) async { await act(Command(.remove, index: i)) }
    func clear() async { await act(Command(.clear)) }
    func shuffleRest() async { await act(Command(.shuffle)) }
    func setRepeat(_ on: Bool) async { await act(Command(.`repeat`, on: on)) }
}
