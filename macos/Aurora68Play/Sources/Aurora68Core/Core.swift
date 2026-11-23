// SPDX-License-Identifier: MPL-2.0
//
// Core.swift - the bridge to Aurora68 Play's shared C core (play/core, the Swift module PlayCore).
//
// Backstory (22 November 2026, docs/08 item 107). The owner asked for "a common code base between the two": the
// classic Play on the SE and this one. Everything that is not interface is now one portable C core that both
// compile (play/core/README.md): the server's protocol and its text answers, the catalogue's records, the queue,
// addresses and discovery, the devices' rules, track names, ICY, the stations. Aurora68Core is the thin Swift
// side of it: the types the SwiftUI views use, made from what the core reads. The core works on bytes and on
// buffers its caller owns; this file is how Swift lends it them:
//   - Records: an answer of the server's, copied once into memory that stays put while the core takes it apart
//     in place (its tabs and Returns become NULs, each field a pointer into it);
//   - text(_:) and cText(_:): C strings to Swift and back, as UTF-8 (the macOS Play asks the server for UTF-8;
//     the classic Play asks for Mac Roman, which only the classic Play turns into anything);
//   - built(_:): a core function that writes a path or a URL into a buffer, as a String.
import Foundation
import PlayCore

/// a C string as a Swift String (UTF-8; a byte that is not UTF-8 becomes U+FFFD)
func text(_ p: UnsafePointer<CChar>?) -> String {
    guard let p else { return "" }
    return String(cString: p)
}

/// a C char array (imported as a tuple) as a Swift String
func text<T>(tuple: T) -> String {
    withUnsafeBytes(of: tuple) { raw in
        String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
    }
}

/// a Swift String into a C char array (a tuple of `size` bytes), cut at a character's boundary, NUL-ended
func setText<T>(_ tuple: inout T, _ s: String) {
    withUnsafeMutableBytes(of: &tuple) { raw in
        let room = raw.count - 1
        var bytes = Array(s.utf8)
        if bytes.count > room {
            bytes = Array(bytes[0..<room])
            while let last = bytes.last, last & 0xC0 == 0x80 { bytes.removeLast() }     // a sequence cut short
            if let last = bytes.last, last >= 0xC0 { bytes.removeLast() }                // its lead byte
        }
        for (i, b) in bytes.enumerated() { raw[i] = b }
        raw[bytes.count] = 0
    }
}

/// what a core function writes into a buffer (a path, a URL), as a String; nil if it did not fit
func built(_ size: Int = 4096, _ f: (UnsafeMutablePointer<CChar>, Int) -> Int32) -> String? {
    var buf = [CChar](repeating: 0, count: size)
    let ok = buf.withUnsafeMutableBufferPointer { f($0.baseAddress!, size) }
    return ok != 0 ? String(cString: buf) : nil
}

/// what a core function writes into a buffer, returning its length (0: nothing)
func builtLength(_ size: Int = 4096, _ f: (UnsafeMutablePointer<CChar>, Int) -> Int) -> String? {
    var buf = [CChar](repeating: 0, count: size)
    let n = buf.withUnsafeMutableBufferPointer { f($0.baseAddress!, size) }
    return n > 0 ? String(cString: buf) : nil
}

/// one of the server's answers, held where the core can take it apart in place
public final class Records {
    let buf: UnsafeMutablePointer<CChar>
    let count: Int
    var rec = a68_rec()

    public init(_ data: Data) {
        count = data.count
        buf = .allocate(capacity: count + 1)          // the core writes a NUL after the end
        buf.withMemoryRebound(to: UInt8.self, capacity: count + 1) { p in
            data.copyBytes(to: p, count: count)
            p[count] = 0
        }
        a68_rec_init(&rec, buf, count)
    }

    public convenience init(_ s: String) { self.init(Data(s.utf8)) }

    deinit { buf.deallocate() }

    /// the first line of a list ("A68M 1 KIND TOTAL FROM COUNT"); nil if the answer is not the server's
    public func head() -> Page? {
        var h = a68_head()
        guard a68_read_head(&rec, &h) != 0 else { return nil }
        let next = a68_next_from(&h)
        return Page(kind: text(tuple: h.kind), total: h.total, from: h.from, count: h.count, next: next >= 0 ? next : nil)
    }

    /// the words of an error answer (the server's own, in its first record)
    public var errorText: String {
        text(a68_error_text(buf, count))
    }
}

/// a list's first line: what it is, how many the server has, which of them came, where the next page starts
public struct Page: Equatable {
    public let kind: String
    public let total: Int
    public let from: Int
    public let count: Int
    public let next: Int?
}

/// what the macOS Play asks the server for: UTF-8, the longer records (paths, folders, whole states), and music
/// videos (22 November 2026: an album's and an artist's after their tracks, a command's as V lines; an older server
/// passes over the parameter and sends none)
let client: a68_client = {
    var c = a68_client()
    c.utf8 = 1
    c.full = 1
    c.videos = 1
    return c
}()

/// a request's path, made by the core with the macOS Play's client (enc=utf8&full=1)
func request(_ f: (UnsafePointer<a68_client>, UnsafeMutablePointer<CChar>, Int) -> Int32) -> String {
    withUnsafePointer(to: client) { c in built { f(c, $0, $1) } ?? "/" }
}
