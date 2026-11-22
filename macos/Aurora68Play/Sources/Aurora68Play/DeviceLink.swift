// SPDX-License-Identifier: MPL-2.0
//
// DeviceLink.swift - this Mac as one of the music server's players, and the other players it can play on.
//
// Backstory (22 November 2026). The owner: "Perhaps other devices running Aurora Player need to broadcast
// they are online for redirection". Every Aurora68 Play (this app on any Mac, and the classic Play on the
// SE) tells the music server it is there, every 20 seconds, and what it plays; the server lists them all,
// and any of them (or the web panel) can send another a command: "play this album there". So "Play on" in
// the toolbar lists This Mac, Live and every other player that is online, Spotify Connect's idea.
//
// Three loops while connected: the announce (every 20 seconds; its answer carries the library's
// generation, which the app watches to refresh its lists when albums are added), the list of players
// (every two seconds, for the picker and the player bar), and the commands for this Mac, fetched by a
// long poll (the server holds the request up to 20 seconds and answers as soon as a command arrives), so
// this Mac needs no open port. What this Mac plays is posted when it changes and every five seconds
// while it plays. The id is made once and kept in the preferences; the name is the Mac's own computer
// name (System Settings, General, Sharing).
import AppKit
import Foundation
import SystemConfiguration
import Aurora68Core

@MainActor
final class DeviceLink: ObservableObject {
    @Published private(set) var devices: [Device] = []
    @Published private(set) var error: String?

    let id: String
    let name: String
    static let version = "1.1"

    var api: MusicAPI? { didSet { restart() } }
    var onCommand: (@MainActor (DeviceCommand) -> Void)?
    var onGeneration: (@MainActor (Int) -> Void)?
    var currentState: (@MainActor () -> DeviceState)?

    private var loops: [Task<Void, Never>] = []
    private var lastPosted: DeviceState?
    private var lastPostTime = Date.distantPast
    private var received = Date()

    init() {
        let kept = UserDefaults.standard.string(forKey: "deviceID")
        id = DeviceIdentity.id(kept: kept)
        if kept != id { UserDefaults.standard.set(id, forKey: "deviceID") }
        // the computer name from System Configuration: Host.current() may wait on the network to look names up
        name = (SCDynamicStoreCopyComputerName(nil, nil) as String?).flatMap { $0.isEmpty ? nil : $0 } ?? "Mac"
    }

    /// the other players that are online: not this Mac, not Live
    var others: [Device] { devices.filter { $0.online && !$0.isLive && $0.id != id } }

    func device(_ id: String) -> Device? { devices.first { $0.id == id } }

    /// a player's state now: its position moved on by the time since it was posted, while it plays
    func state(of id: String) -> DeviceState? {
        guard var s = device(id)?.state else { return nil }
        if s.at != nil {
            s.position = s.estimatedPosition(at: Date().timeIntervalSince1970)
        } else if s.playing {                  // the Live channel's: as of the list
            let p = s.position + Date().timeIntervalSince(received)
            s.position = s.length > 0 ? min(p, s.length) : p
        }
        return s
    }

    private func restart() {
        loops.forEach { $0.cancel() }
        loops = []
        devices = []
        lastPosted = nil
        guard let api else { return }
        let waiting = MusicAPI(base: api.base, session: MusicAPI.waiting())
        loops.append(Task { [weak self] in await self?.announceLoop(api) })
        loops.append(Task { [weak self] in await self?.listLoop(api) })
        loops.append(Task { [weak self] in await self?.commandLoop(waiting) })
        loops.append(Task { [weak self] in await self?.stateLoop() })
    }

    private func announceLoop(_ api: MusicAPI) async {
        var every = 20
        var older = false                      // a server from before devices: the generation from /api/info
        while !Task.isCancelled {
            if older {
                if let info = try? await api.info() { onGeneration?(info.library) }
            } else {
                do {
                    let r = try await api.announce(id: id, name: name, version: DeviceLink.version)
                    every = max(5, min(r.every, 60))
                    if let g = r.library { onGeneration?(g) }
                    error = nil
                    await postState(force: true)
                } catch MusicAPIError.status(404, _) {
                    older = true
                    continue
                } catch {
                    self.error = error.localizedDescription
                }
            }
            try? await Task.sleep(nanoseconds: UInt64(every) * 1_000_000_000)
        }
    }

    private func listLoop(_ api: MusicAPI) async {
        while !Task.isCancelled {
            if let d = try? await api.devices() {
                received = Date()
                if d != devices { devices = d }
            }
            try? await Task.sleep(nanoseconds: 2_000_000_000)
        }
    }

    private func commandLoop(_ api: MusicAPI) async {
        while !Task.isCancelled {
            do {
                let commands = try await api.commands(id: id, wait: 20)
                for c in commands { onCommand?(c) }
                if !commands.isEmpty { await postState(force: true) }
            } catch {
                if Task.isCancelled { return }
                // not announced yet, an older server, or the network: try again shortly
                try? await Task.sleep(nanoseconds: 3_000_000_000)
            }
        }
    }

    private func stateLoop() async {
        while !Task.isCancelled {
            try? await Task.sleep(nanoseconds: 1_000_000_000)
            await postState()
        }
    }

    /// what this Mac plays, to the server: when it changes (a seek counts), and every 5 seconds while playing
    func postState(force: Bool = false) async {
        guard let api, let s = currentState?() else { return }
        let now = Date()
        var post = force || lastPosted == nil
        if !post, let p = lastPosted {
            var a = p, b = s
            a.position = 0
            b.position = 0
            let expected = p.playing ? p.position + now.timeIntervalSince(lastPostTime) : p.position
            post = a != b || abs(s.position - expected) > 3 || (s.playing && now.timeIntervalSince(lastPostTime) >= 5)
        }
        guard post else { return }
        lastPosted = s
        lastPostTime = now
        do { try await api.postState(id: id, s) } catch MusicAPIError.status(404, _) { lastPosted = nil } catch {}
    }

    /// a command for another player, or for "live": the server's answer in words when it refuses
    func send(_ device: String, _ bodies: [[String: Any]]) async -> String? {
        guard let api else { return "Not connected." }
        for b in bodies {
            do { try await api.command(device, b) } catch {
                self.error = error.localizedDescription
                return error.localizedDescription
            }
        }
        error = nil
        if let d = try? await api.devices() { received = Date(); devices = d }
        return nil
    }
}
