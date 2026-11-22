// swift-tools-version:5.9
// SPDX-License-Identifier: MPL-2.0
//
// Package.swift - Aurora68 Play for macOS: the music server's player on a modern Mac.
//
// Two targets: Aurora68Core, the parts that are only logic (the server's JSON, addresses with IPv6 in
// brackets, the discovery answer, track names, the play queue), tested with XCTest; and Aurora68Play,
// the SwiftUI application around it (AVFoundation, MediaPlayer, the window). make-app.sh wraps the
// built program into "Aurora68 Play.app".
import PackageDescription

let package = Package(
    name: "Aurora68Play",
    platforms: [.macOS(.v13)],
    targets: [
        .target(name: "Aurora68Core"),
        .executableTarget(name: "Aurora68Play", dependencies: ["Aurora68Core"]),
        .testTarget(name: "Aurora68CoreTests", dependencies: ["Aurora68Core"]),
    ]
)
