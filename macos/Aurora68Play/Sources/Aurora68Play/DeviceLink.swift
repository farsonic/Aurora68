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
// this Mac needs no open port. What this Mac plays is reported when it changes and every five seconds
// while it plays. The id is made once and kept in the preferences; the name is the Mac's own computer
// name (System Settings, General, Sharing).
//
// Since the shared core (docs/08 item 107) the rules are the classic Play's: play/core/devices.c (DeviceSession)
// writes the requests (/mac/announce, /mac/state, /mac/commands, in UTF-8) and decides when an announce is due
// and when what plays is news; this file makes the requests, on connections of its own, as URLSession likes.
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
    static let version = "1.4"

    var api: MusicAPI? { didSet { restart() } }
    var onCommand: (@MainActor (DeviceCommand) -> Void)?
    var onGeneration: (@MainActor (Int) -> Void)?
    var currentState: (@MainActor () -> DeviceState)?

    private let session: DeviceSession
    private var loops: [Task<Void, Never>] = []
    private var received = Date()

    init() {
        let kept = UserDefaults.standard.string(forKey: "deviceID")
        id = DeviceIdentity.id(kept: kept)
        if kept != id { UserDefaults.standard.set(id, forKey: "deviceID") }
        // the computer name from System Configuration: Host.current() may wait on the network to look names up
        name = (SCDynamicStoreCopyComputerName(nil, nil) as String?).flatMap { $0.isEmpty ? nil : $0 } ?? "Mac"
        session = DeviceSession(id: id, name: name, version: DeviceLink.version)
    }

    /// the other players that are online: not this Mac, not Live
    var others: [Device] { devices.filter { $0.online && !$0.isLive && $0.id != id } }

    func device(_ id: String) -> Device? { devices.first { $0.id == id } }

    /// a player's state now: its position moved on by the time since it was reported, while it plays
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
        session.end()
        guard let api else { return }
        session.begin()
        let waiting = MusicAPI(base: api.base, session: MusicAPI.waiting())
        loops.append(Task { [weak self] in await self?.announceLoop(api) })
        loops.append(Task { [weak self] in await self?.listLoop(api) })
        loops.append(Task { [weak self] in await self?.commandLoop(waiting) })
        loops.append(Task { [weak self] in await self?.stateLoop(api) })
    }

    private func announceLoop(_ api: MusicAPI) async {
        var older = false                      // a server from before devices: the generation from /mac/info
        var watch = GenerationWatch()
        while !Task.isCancelled {
            if older {
                if let info = try? await api.info(), watch.changed(info.library) { onGeneration?(info.library) }
            } else {
                do {
                    let (status, body) = try await api.text(session.path(.announce))
                    if status == 404 && !session.announced && Records(body).head() != nil {
                        older = true           // "info, artists, ...": this server has no /mac/announce
                        continue
                    }
                    session.answer(.announce, status: status, body: body)
                    if session.takeGenerationChange(), let g = session.generation { onGeneration?(g) }
                    error = status == 200 ? nil : Records(body).errorText
                } catch {
                    session.answer(.announce, status: 0, body: nil)
                    self.error = error.localizedDescription
                }
            }
            try? await Task.sleep(nanoseconds: UInt64(max(5, min(session.every, 60))) * 1_000_000_000)
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
            guard session.announced else {     // the server must know this Mac first
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                continue
            }
            do {
                let (status, commands) = try await api.commands(path: session.path(.commands))
                session.answer(.commands, status: status, body: nil)
                for c in commands { onCommand?(c) }
                if status != 200 { try? await Task.sleep(nanoseconds: 3_000_000_000) }
            } catch {
                if Task.isCancelled { return }
                session.answer(.commands, status: 0, body: nil)
                // the network, or the server gone: try again shortly
                try? await Task.sleep(nanoseconds: 3_000_000_000)
            }
        }
    }

    /// what this Mac plays, to the server: when it changes (a seek counts), and every 5 seconds while playing
    private func stateLoop(_ api: MusicAPI) async {
        while !Task.isCancelled {
            try? await Task.sleep(nanoseconds: 1_000_000_000)
            guard let s = currentState?(), session.setState(s) else { continue }
            if let r = try? await api.text(session.path(.state)) {
                session.answer(.state, status: r.status, body: r.body)
            } else {
                session.answer(.state, status: 0, body: nil)
            }
        }
    }

    /// commands for another player, or for "live": the server's answer in words when it refuses
    func send(_ device: String, _ commands: [Command]) async -> String? {
        guard let api else { return "Not connected." }
        for c in commands {
            do { try await api.command(device, c) } catch {
                self.error = error.localizedDescription
                return error.localizedDescription
            }
        }
        error = nil
        if let d = try? await api.devices() { received = Date(); devices = d }
        return nil
    }
}
