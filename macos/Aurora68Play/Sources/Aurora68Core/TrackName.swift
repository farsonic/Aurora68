// SPDX-License-Identifier: MPL-2.0
//
// TrackName.swift - a song's number and title from its file's name, and a time as a player shows it.
//
// Backstory (22 November 2026). The server gives each track its title from the file's tags once the
// album has been listed, and from its name before that; the Live channel's queue only names files.
// The owner's library names them "01. Semisonic - Closing Time.m4a", others "01 Closing Time.mp3",
// "1-01 Closing Time.flac" (disc and track) or "Closing Time.mp3". So: an optional disc number, a track
// number of up to three digits, a separator; then "Artist - " taken off the front when it is the
// album's artist. A name that only starts with a number ("2001 A Space Odyssey") keeps it.
//
// This was a regular expression here; since the shared core (docs/08 item 107) the rule is written out in
// play/core/names.c, which the classic Play compiles too, and these are its wrappers. One difference: the
// artist is compared with the title's front in ASCII letters of either case (the core has no Unicode tables),
// where Swift's lowercased() also matched "É" with "é".
import Foundation
import PlayCore

public enum TrackName {
    /// (number or 0, title) of a file name without its extension
    public static func parse(_ stem: String, artist: String? = nil) -> (number: Int, title: String) {
        let size = stem.utf8.count + 1
        var out = [CChar](repeating: 0, count: size)
        let n = out.withUnsafeMutableBufferPointer { b in a68_track_name(stem, artist ?? "", b.baseAddress!, size) }
        return (Int(n), String(cString: out))
    }

    /// the stem of a path: its last part without the extension
    public static func stem(of path: String) -> String {
        let size = path.utf8.count + 1
        var out = [CChar](repeating: 0, count: size)
        _ = out.withUnsafeMutableBufferPointer { b in a68_stem(path, b.baseAddress!, size) }
        return String(cString: out)
    }

    /// "3:07", "1:02:03"
    public static func time(_ seconds: Double) -> String {
        guard seconds.isFinite, seconds >= 0 else { return "--:--" }
        var buf = [CChar](repeating: 0, count: 24)
        buf.withUnsafeMutableBufferPointer { b in
            let end = a68_put_time(b.baseAddress!, b.baseAddress! + 23, UInt(seconds.rounded(.down)))
            end?.pointee = 0
        }
        return String(cString: buf)
    }
}
