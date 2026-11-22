// SPDX-License-Identifier: MPL-2.0
//
// PlayQueue.swift - the songs to play on this Mac, and which of them is playing.
//
// Backstory (22 November 2026). The player keeps AVFoundation's queue only one song ahead (the playing
// item and the next, so that the next starts without a gap); this is the queue the owner sees and
// changes: play an album, play a song next, add to the end, remove, jump, shuffle what is still to
// come, repeat all or one. It is a value with no AVFoundation in it, so the rules are tested on their
// own; the player asks it what plays now and what follows.
import Foundation

public enum RepeatMode: String, Codable, CaseIterable {
    case off, all, one
    public var next: RepeatMode { self == .off ? .all : self == .all ? .one : .off }
}

public struct PlayQueue<Item: Equatable>: Equatable {
    public private(set) var items: [Item] = []
    public private(set) var index: Int?               // the playing (or paused) item
    public var repeatMode: RepeatMode = .off

    public init() {}

    public var current: Item? { index.map { items[$0] } }
    public var isEmpty: Bool { items.isEmpty }
    /// the items after the playing one
    public var upcoming: ArraySlice<Item> { items[((index ?? -1) + 1)...] }

    /// a new queue, playing from `start`
    public mutating func replace(with new: [Item], start: Int = 0) {
        items = new
        index = new.isEmpty ? nil : min(max(start, 0), new.count - 1)
    }

    /// after the playing item (or at the front of a queue that plays nothing)
    public mutating func playNext(_ new: [Item]) {
        let at = index.map { $0 + 1 } ?? 0
        items.insert(contentsOf: new, at: at)
        if index == nil && !items.isEmpty { index = 0 }
    }

    public mutating func append(_ new: [Item]) {
        items.append(contentsOf: new)
        if index == nil && !items.isEmpty { index = 0 }
    }

    /// take an item out; the one playing moves on to its follower (nil if it was the last)
    public mutating func remove(at i: Int) {
        guard items.indices.contains(i) else { return }
        items.remove(at: i)
        guard let cur = index else { return }
        if items.isEmpty { index = nil }
        else if i < cur { index = cur - 1 }
        else if i == cur && cur >= items.count { index = nil }
    }

    public mutating func jump(to i: Int) -> Item? {
        guard items.indices.contains(i) else { return nil }
        index = i
        return items[i]
    }

    public mutating func clear() {
        items = []; index = nil
    }

    /// what follows the playing item at its natural end (repeat one: itself; repeat all: round again)
    public func peekNext() -> Item? {
        guard let cur = index else { return nil }
        if repeatMode == .one { return items[cur] }
        if cur + 1 < items.count { return items[cur + 1] }
        return repeatMode == .all && !items.isEmpty ? items[0] : nil
    }

    /// the playing item ended by itself
    @discardableResult
    public mutating func advance() -> Item? {
        guard let cur = index else { return nil }
        if repeatMode == .one { return items[cur] }
        if cur + 1 < items.count { index = cur + 1 }
        else if repeatMode == .all && !items.isEmpty { index = 0 }
        else { index = nil }
        return current
    }

    /// the owner pressed Next: repeat one does not hold them
    @discardableResult
    public mutating func skip() -> Item? {
        guard let cur = index else { return nil }
        if cur + 1 < items.count { index = cur + 1 }
        else if repeatMode != .off && !items.isEmpty { index = 0 }
        else { index = nil }
        return current
    }

    /// the owner pressed Previous: more than three seconds in, the song starts again; else the one before
    public mutating func previous(elapsed: Double) -> (item: Item?, restart: Bool) {
        guard let cur = index else { return (nil, false) }
        if elapsed > 3 || cur == 0 { return (items[cur], true) }
        index = cur - 1
        return (items[cur - 1], false)
    }

    /// shuffle what is still to come; the playing item stays where it is
    public mutating func shuffleUpcoming<G: RandomNumberGenerator>(using g: inout G) {
        let from = (index ?? -1) + 1
        guard from < items.count else { return }
        var rest = Array(items[from...])
        rest.shuffle(using: &g)
        items.replaceSubrange(from..., with: rest)
    }

    public mutating func shuffleUpcoming() {
        var g = SystemRandomNumberGenerator()
        shuffleUpcoming(using: &g)
    }
}
