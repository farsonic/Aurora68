// SPDX-License-Identifier: MPL-2.0
//
// TrackName.swift - a song's number and title from its file's name.
//
// Backstory (22 November 2026). The server gives each track its title from the file's tags once the
// album has been listed, and from its name before that; the Live channel's queue only names files.
// The owner's library names them "01. Semisonic - Closing Time.m4a", others "01 Closing Time.mp3",
// "1-01 Closing Time.flac" (disc and track) or "Closing Time.mp3". So: an optional disc number, a track
// number of up to three digits, a separator; then "Artist - " taken off the front when it is the
// album's artist. A name that only starts with a number ("2001 A Space Odyssey") keeps it.
import Foundation

public enum TrackName {
    private static let pattern = try! NSRegularExpression(
        pattern: #"^\s*(?:\d{1,2}[-.])?(\d{1,3})(?:\s*[-.)_]\s*|\s+)(.+)$"#)

    /// (number or 0, title) of a file name without its extension
    public static func parse(_ stem: String, artist: String? = nil) -> (number: Int, title: String) {
        var number = 0
        var title = stem.trimmingCharacters(in: .whitespaces)
        let ns = title as NSString
        if let m = pattern.firstMatch(in: title, range: NSRange(location: 0, length: ns.length)),
           let n = Int(ns.substring(with: m.range(at: 1))) {
            number = n
            title = ns.substring(with: m.range(at: 2)).trimmingCharacters(in: .whitespaces)
        }
        if let artist, !artist.isEmpty {
            let prefix = artist + " - "
            if title.lowercased().hasPrefix(prefix.lowercased()) && title.count > prefix.count {
                title = String(title.dropFirst(prefix.count))
            }
        }
        return (number, title)
    }

    /// the stem of a path: its last part without the extension
    public static func stem(of path: String) -> String {
        let last = path.split(separator: "/").last.map(String.init) ?? path
        if let dot = last.lastIndex(of: "."), dot != last.startIndex {
            return String(last[..<dot])
        }
        return last
    }

    /// "3:07", "1:02:03"
    public static func time(_ seconds: Double) -> String {
        guard seconds.isFinite, seconds >= 0 else { return "--:--" }
        let s = Int(seconds.rounded(.down))
        return s >= 3600 ? String(format: "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60)
                         : String(format: "%d:%02d", s / 60, s % 60)
    }
}
