// swift-tools-version:5.9
// SPDX-License-Identifier: MPL-2.0
//
// Package.swift - Aurora68 Play for macOS: the music server's player on a modern Mac, and the C core it shares
// with the classic Aurora68 Play on the SE.
//
// Why it is at the top of the repository (22 November 2026, docs/08 item 107): the two Plays share one C core,
// play/core, which the classic Play's mac/Makefile compiles for the 68000, and a SwiftPM target must lie inside
// its package's folder. So the package is the repository, and its targets name their folders; nothing else in
// the repository is part of it. make-app.sh (macos/Aurora68Play) and the workflow (.github/workflows/
// macos-player.yml) build it from here.
//
// Four targets: PlayCore, the shared C core (play/core: its include/ is the module's headers, PlayCore.h the
// umbrella); Aurora68Core, the Swift side of it and the rest of the logic (the types SwiftUI shows, tested with
// XCTest); Aurora68Play, the SwiftUI application (AVFoundation, MediaPlayer, the window); and the tests.
import PackageDescription

let package = Package(
    name: "Aurora68Play",
    platforms: [.macOS(.v13)],
    targets: [
        .target(name: "PlayCore", path: "play/core", exclude: ["README.md"], publicHeadersPath: "include",
                cSettings: [.unsafeFlags(["-Wall", "-Wextra", "-Wno-unused-parameter"])]),
        .target(name: "Aurora68Core", dependencies: ["PlayCore"], path: "macos/Aurora68Play/Sources/Aurora68Core"),
        .executableTarget(name: "Aurora68Play", dependencies: ["Aurora68Core"], path: "macos/Aurora68Play/Sources/Aurora68Play"),
        .testTarget(name: "Aurora68CoreTests", dependencies: ["Aurora68Core", "PlayCore"],
                    path: "macos/Aurora68Play/Tests/Aurora68CoreTests"),
    ],
    cLanguageStandard: .c99
)
