// SPDX-License-Identifier: MPL-2.0
//
// PlayQueue.swift - the songs to play on this Mac, and which of them is playing.
//
// Backstory (22 November 2026). The player keeps AVFoundation's queue only one song ahead (the playing
// item and the next, so that the next starts without a gap); this is the queue the owner sees and
// changes: play an album, play a song next, add to the end, remove, jump, shuffle what is still to
// come, repeat all or one. It is a value with no AVFoundation in it, so the rules are tested on their
// own; the player asks it what plays now and what follows.
//
// Since the shared core (docs/08 item 107) the rules are play/core/queue.c's, which the classic Play on the SE
// uses for its Library: this is a wrapper. The core moves fixed-size items about in an array its caller owns;
// here the items are 32-bit handles of the Swift values, kept in `pool`. One rule changed in the move: songs
// added to a queue that has played to its end are the ones that play next (the classic Play's rule), where
// this queue used to start again from its first song.
import Foundation
import PlayCore

public enum RepeatMode: String, Codable, CaseIterable {
    case off, all, one
    public var next: RepeatMode { self == .off ? .all : self == .all ? .one : .off }
    var core: Int32 {
        switch self {
        case .off: return Int32(A68_REPEAT_OFF)
        case .all: return Int32(A68_REPEAT_ALL)
        case .one: return Int32(A68_REPEAT_ONE)
        }
    }
}

public struct PlayQueue<Item: Equatable>: Equatable {
    private var pool: [Item] = []                     // the values; the core's items are their places in it
    private var handles: [Int32] = [0]                // the core's array (never empty: its address is lent out)
    private var n: Int32 = 0
    private var cur: Int32 = -1
    private var feed: Int32 = -1
    public var repeatMode: RepeatMode = .off

    public init() {}

    public static func == (a: PlayQueue, b: PlayQueue) -> Bool {
        a.items == b.items && a.index == b.index && a.repeatMode == b.repeatMode
    }

    public var items: [Item] { handles[0..<Int(n)].map { pool[Int($0)] } }
    public var index: Int? { cur >= 0 ? Int(cur) : nil }               // the playing (or paused) item
    public var current: Item? { index.map { pool[Int(handles[$0])] } }
    public var isEmpty: Bool { n == 0 }
    /// the items after the playing one
    public var upcoming: ArraySlice<Item> { items[((index ?? -1) + 1)...] }

    /// one of the core's operations on this queue (room: items it may add)
    private mutating func run<R>(room: Int = 0, _ body: (UnsafeMutablePointer<a68_queue>) -> R) -> R {
        if handles.count < Int(n) + room { handles += [Int32](repeating: 0, count: Int(n) + room - handles.count) }
        let rep = repeatMode.core
        var (nn, cc, ff) = (n, cur, feed)
        let r: R = handles.withUnsafeMutableBufferPointer { buf in
            var q = a68_queue()
            a68q_init(&q, buf.baseAddress!, UInt(MemoryLayout<Int32>.stride), Int32(buf.count))
            q.n = nn; q.cur = cc; q.feed = ff; q.`repeat` = rep
            let out = body(&q)
            (nn, cc, ff) = (q.n, q.cur, q.feed)
            return out
        }
        (n, cur, feed) = (nn, cc, ff)
        return r
    }

    private func item(_ i: Int32) -> Item? { i >= 0 && i < n ? pool[Int(handles[Int(i)])] : nil }

    private mutating func insert(_ new: Item, at place: Int32) {
        var h = Int32(pool.count)
        pool.append(new)
        _ = run(room: 1) { a68q_insert($0, place, &h) }
    }

    /// a new queue, playing from `start`
    public mutating func replace(with new: [Item], start: Int = 0) {
        pool = new
        handles = (0..<Int32(new.count)).map { $0 } + [0]
        n = 0; cur = -1; feed = -1
        run { a68q_set($0, Int32(new.count), Int32(clamping: start)) }
    }

    /// after the playing item (or at the front of a queue that plays nothing)
    public mutating func playNext(_ new: [Item]) {
        let place = run { a68q_next_place($0) }
        for (k, it) in new.enumerated() { insert(it, at: place + Int32(k)) }
    }

    public mutating func append(_ new: [Item]) {
        for it in new { insert(it, at: -1) }
    }

    /// take an item out; the one playing moves on to its follower (nil if it was the last)
    public mutating func remove(at i: Int) {
        run { a68q_remove($0, Int32(clamping: i)) }
    }

    public mutating func jump(to i: Int) -> Item? {
        let j = run { a68q_jump($0, Int32(clamping: i)) }
        return item(j)
    }

    public mutating func clear() {
        pool = []; handles = [0]
        n = 0; cur = -1; feed = -1
    }

    /// what follows the playing item at its natural end (repeat one: itself; repeat all: round again)
    public func peekNext() -> Item? {
        var copy = self
        return item(copy.run { a68q_peek_next($0) })
    }

    /// the playing item ended by itself
    @discardableResult
    public mutating func advance() -> Item? { item(run { a68q_advance($0) }) }

    /// the owner pressed Next: repeat one does not hold them
    @discardableResult
    public mutating func skip() -> Item? { item(run { a68q_skip($0) }) }

    /// the owner pressed Previous: more than three seconds in, the song starts again; else the one before
    public mutating func previous(elapsed: Double) -> (item: Item?, restart: Bool) {
        var restart: Int32 = 0
        let secs = elapsed.isFinite && elapsed > 0 ? UInt(elapsed.rounded(.up)) : 0    // "more than three": 3.2 s counts
        let i = run { a68q_previous($0, secs, &restart) }
        return (item(i), restart != 0)
    }

    /// shuffle what is still to come; the playing item stays where it is
    public mutating func shuffleUpcoming<G: RandomNumberGenerator>(using g: inout G) {
        let seed = UInt(truncatingIfNeeded: g.next())
        run { a68q_shuffle_upcoming($0, seed) }
    }

    public mutating func shuffleUpcoming() {
        var g = SystemRandomNumberGenerator()
        shuffleUpcoming(using: &g)
    }
}
