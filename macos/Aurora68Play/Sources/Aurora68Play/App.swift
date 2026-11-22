// SPDX-License-Identifier: MPL-2.0
//
// App.swift - Aurora68 Play for macOS: the application, its menus and its keys.
//
// Backstory (22 November 2026). The owner asked for "a modern MacOS version of the Player app": the
// SE's Aurora68 Play plays the music server's library on a 1987 screen; this one plays it on today's
// Mac, and can send it to the SE through the server's Live channel. SwiftUI, macOS 13 or later.
// Keys: space plays and pauses (unless a text field is being typed in), Command-Left and -Right are
// Previous and Next, Command-F the search field; the keyboard's media keys work through the system's
// Now Playing (Player.swift).
import AppKit
import SwiftUI
import Aurora68Core

final class AppDelegate: NSObject, NSApplicationDelegate {
    var onSpace: (() -> Void)?
    private var keys: Any?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)               // run from Terminal as well as from the .app
        NSApp.activate(ignoringOtherApps: true)
        URLCache.shared = URLCache(memoryCapacity: 64 << 20, diskCapacity: 256 << 20)   // covers, fetched once
        keys = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] e in
            let mods = e.modifierFlags.intersection(.deviceIndependentFlagsMask).subtracting([.capsLock, .numericPad, .function])
            guard e.keyCode == 49, mods.isEmpty else { return e }          // the space bar, alone
            if NSApp.keyWindow?.firstResponder is NSText { return e }      // typing in a field
            self?.onSpace?()
            return nil
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

@main
struct Aurora68PlayApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup("Aurora68 Play") {
            ContentView()
                .environmentObject(model)
                .environmentObject(model.player)
                .environmentObject(model.live)
                .frame(minWidth: 860, minHeight: 540)
                .onAppear {
                    let m = model
                    delegate.onSpace = { Task { @MainActor in m.togglePlay() } }
                    model.start()
                }
        }
        .commands { PlayCommands(model: model) }

        Settings {
            SettingsView().environmentObject(model)
        }
    }
}

struct PlayCommands: Commands {
    let model: AppModel

    var body: some Commands {
        CommandMenu("Controls") {
            Button("Play/Pause") { model.togglePlay() }
            Button("Next") { model.next() }.keyboardShortcut(.rightArrow, modifiers: .command)
            Button("Previous") { model.previous() }.keyboardShortcut(.leftArrow, modifiers: .command)
            Divider()
            Button("Play on This Mac") { model.output = .thisMac }.keyboardShortcut("1", modifiers: [.command, .option])
            Button("Play on Live (Mac SE)") { model.output = .live }.keyboardShortcut("2", modifiers: [.command, .option])
            Divider()
            Button("Reload Library") { model.reload() }.keyboardShortcut("r", modifiers: .command)
        }
        CommandGroup(after: .textEditing) {
            Button("Find") { model.focusSearch() }.keyboardShortcut("f", modifiers: .command)
        }
    }
}
