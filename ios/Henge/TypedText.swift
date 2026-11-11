/// What the Mac's text looks like around its caret, as far as Henge knows: the characters typed since the
/// keyboard came up (or since the Mac's text last changed some other way). iOS's keyboard reads it to
/// autocorrect, suggest words, capitalize and so on, and its changes go to the Mac as keys: arrows to the
/// place, backspaces over what goes, then the new text typed. Henge can't see the Mac's text, so whatever
/// came before is a stand-in (the seed): iOS may read it, but never change it.
///
/// Positions count characters, which are all ASCII (the only ones the Mac's keyboard can type), so they are
/// UTF-16 offsets too, as iOS counts.
struct TypedText {
    enum Key: Equatable { case left, right, backspace, type(Character) }

    /// Stand-ins for text Henge can't see. Empty at a field's start (iOS then capitalizes), and a comma and space
    /// somewhere in the middle of text (it doesn't).
    static let fresh = "", midText = ", "
    /// The most keys one change may take: the Mac's keyboard holds 256 reports, and a key takes two to four.
    static let maxKeys = 60
    /// iOS only ever changes the word being typed, or the one just before it.
    static let reach = 48
    static let cap = 2000

    private(set) var chars: [Character]
    private(set) var seedCount: Int
    /// iOS's selection; empty, it is the caret.
    private(set) var selection: Range<Int>
    /// Text iOS is still composing (marked), here only: it goes to the Mac when it is committed.
    private(set) var marked: Range<Int>?
    /// Where the Mac's caret is, in this text: the selection's start, or the start of marked text.
    private(set) var macCaret: Int
    /// What the Mac itself has selected, when it has (a word picked out to correct, say): typing replaces it, a
    /// backspace deletes it, and a left or right arrow only ends it at its start or end. macCaret is its start.
    private(set) var macSelection: Range<Int>?

    init(seed: String = fresh) {
        chars = Array(seed)
        seedCount = chars.count
        selection = seedCount..<seedCount
        macCaret = seedCount
    }

    /// The Mac's own text around its caret, as the helper read it: all of it known, and iOS may change it. Only the
    /// part on the caret's line the Mac's keyboard could have typed (ASCII), so positions match the Mac's arrows.
    /// `after` starts where the Mac's selection does, and its first `selected` characters are what is selected; iOS
    /// gets the same selection, so it offers its corrections for a selected word.
    init(before: String, after: String, selected: Int = 0) {
        let b = before.lastIndex { !Self.typeable($0) }.map { String(before[before.index(after: $0)...]) } ?? before
        let a = after.firstIndex { !Self.typeable($0) }.map { String(after[..<$0]) } ?? after
        chars = Array(b + a)
        seedCount = 0
        selection = b.count..<b.count
        macCaret = b.count
        if selected > 0, selected <= a.count {
            selection = b.count..<(b.count + selected)
            macSelection = selection
        }
    }

    /// Whether the Mac's selection, as the helper said it, is all here: false when it reaches past the caret's line
    /// or holds what the Mac's keyboard can't type, and Henge can't say where its keys would go.
    static func follows(before: String, after: String, selected: Int) -> Bool {
        selected <= 0 || TypedText(before: before, after: after, selected: selected).macSelection != nil
    }

    /// Whether the Mac's text, as the helper read it, agrees with what is known here around the Mac's caret (and
    /// what it has selected). Either side may reach further than the other.
    func agrees(before: String, after: String, selected: Int = 0) -> Bool {
        let real = TypedText(before: before, after: after, selected: selected)
        guard (real.macSelection?.count ?? 0) == (macSelection?.count ?? 0) else { return false }
        let realBefore = real.text(0..<real.macCaret), realAfter = real.text(real.macCaret..<real.count)
        let knownBefore = text(seedCount..<macCaret), knownAfter = text(macCaret..<count)
        return (realBefore.hasSuffix(knownBefore) || knownBefore.hasSuffix(realBefore))
            && (realAfter.hasPrefix(knownAfter) || knownAfter.hasPrefix(realAfter))
    }

    /// Only a stand-in so far: nothing known.
    var bare: Bool { count == seedCount }

    var count: Int { chars.count }
    var text: String { String(chars) }
    func text(_ range: Range<Int>) -> String {
        let r = range.clamped(to: 0..<count)
        return String(chars[r])
    }

    /// What the Mac's keyboard types (US layout): printable ASCII.
    static func typeable(_ c: Character) -> Bool {
        guard let a = c.asciiValue else { return false }
        return a >= 0x20 && a < 0x7f
    }

    // MARK: What iOS does

    /// Typed text. Nil when the text can't be followed (a character the Mac can't type, Return or Tab):
    /// the caller types it as it is and starts again.
    mutating func insert(_ s: String) -> [Key]? {
        if marked != nil {
            _ = setMarked(s, selected: s.count..<s.count)
            return commit()
        }
        return edit(selection, s)
    }

    /// Nil when there is nothing known before the caret: the caller sends a backspace and starts again.
    mutating func deleteBackward() -> [Key]? {
        if let m = marked {
            let at = selection.isEmpty ? selection.lowerBound - 1 : selection.lowerBound
            guard at >= m.lowerBound else { return nil }
            let gone = at..<max(selection.upperBound, at + 1)
            chars.removeSubrange(gone)
            marked = m.lowerBound..<(m.upperBound - gone.count)
            selection = at..<at
            if marked!.isEmpty { marked = nil }
            return []
        }
        if !selection.isEmpty { return edit(selection, "") }
        let c = selection.lowerBound
        guard c > seedCount else { return nil }
        return edit(c - 1..<c, "")
    }

    /// Autocorrect, a suggestion, a text replacement, a double space's period.
    mutating func replace(_ range: Range<Int>, _ s: String) -> [Key]? {
        if let m = marked, range.lowerBound >= m.lowerBound, range.upperBound <= m.upperBound {
            chars.replaceSubrange(range, with: Array(s))
            marked = m.lowerBound..<(m.upperBound - range.count + s.count)
            selection = shifted(selection, by: range, to: s.count)
            return []
        }
        var keys: [Key] = []
        if marked != nil { guard let k = commit() else { return nil }; keys = k }
        guard range.lowerBound >= max(seedCount, selection.lowerBound - Self.reach), range.upperBound <= count else { return nil }
        // A replaced selection leaves the caret after the new text, as a text view's does.
        let end = range.lowerBound + s.count
        let after = range == selection ? end..<end : shifted(selection, by: range, to: s.count)
        guard let k = mac(range, s) else { return nil }
        keys += k
        selection = after
        keys += moveMac(to: after.lowerBound)
        return keys.count <= Self.maxKeys ? keys : nil
    }

    /// iOS moved the caret (or selected). Nil when it went where Henge can't follow.
    mutating func select(_ range: Range<Int>) -> [Key]? {
        let r = range.clamped(to: 0..<count)
        if marked != nil { selection = r; return [] }
        guard r.lowerBound >= seedCount else { return nil }
        selection = r
        if r == macSelection { return [] }
        // Henge can't select on the Mac: its caret waits at the selection's start until something replaces it.
        let keys = moveMac(to: r.lowerBound)
        return keys.count <= Self.maxKeys ? keys : nil
    }

    /// Nil when what was selected couldn't be taken off the Mac.
    mutating func setMarked(_ s: String, selected: Range<Int>) -> [Key]? {
        let range: Range<Int>
        var keys: [Key] = []
        if let m = marked { range = m } else {
            // What is selected goes first, from the Mac too.
            if !selection.isEmpty {
                guard let k = edit(selection, "") else { return nil }
                keys = k
            }
            range = selection.lowerBound..<selection.lowerBound
        }
        chars.replaceSubrange(range, with: Array(s))
        let m = range.lowerBound..<(range.lowerBound + s.count)
        marked = m
        let sel = selected.clamped(to: 0..<s.count)
        selection = (m.lowerBound + sel.lowerBound)..<(m.lowerBound + sel.upperBound)
        return keys
    }

    /// Composed text goes to the Mac. Nil when it has characters the Mac can't type.
    mutating func commit() -> [Key]? {
        guard let m = marked else { return [] }
        marked = nil
        let s = text(m)
        chars.removeSubrange(m)
        let caret = selection.lowerBound
        macCaret = m.lowerBound
        selection = m.lowerBound..<m.lowerBound
        guard var keys = edit(selection, s) else { return nil }
        let target = min(max(caret, seedCount), count)
        selection = target..<target
        keys += moveMac(to: target)
        return keys
    }

    /// The Mac's caret moved by arrows (the space bar's cursor). False when it left what Henge knows.
    mutating func macMoved(by n: Int) -> Bool {
        guard marked == nil else { return false }
        var to = macCaret + n
        // The first arrow only ends what the Mac has selected, at that end.
        if let m = macSelection, n != 0 { to = n < 0 ? m.lowerBound + n + 1 : m.upperBound + n - 1 }
        guard to >= seedCount, to <= count else { return false }
        macSelection = nil
        macCaret = to
        selection = to..<to
        return true
    }

    // MARK: To the Mac

    /// Replaces a range on the Mac and here, leaving both carets after the new text.
    private mutating func edit(_ range: Range<Int>, _ s: String) -> [Key]? {
        guard let keys = mac(range, s) else { return nil }
        selection = macCaret..<macCaret
        return keys
    }

    private mutating func mac(_ range: Range<Int>, _ s: String) -> [Key]? {
        guard marked == nil, range.lowerBound >= seedCount, range.upperBound <= count,
              s.allSatisfy(Self.typeable), count - range.count + s.count <= Self.cap else { return nil }
        var keys: [Key]
        if range == macSelection {
            // What the Mac has selected: typing replaces it, and a backspace deletes it.
            keys = s.isEmpty ? [.backspace] : s.map { .type($0) }
        } else {
            // Anything else the Mac has selected is ended first: a left arrow leaves its caret at its start.
            var from = macCaret
            keys = []
            if let m = macSelection { keys = [.left]; from = m.lowerBound }
            let n = range.upperBound - from
            keys += Array(repeating: n > 0 ? .right : .left, count: abs(n))
            keys += Array(repeating: .backspace, count: range.count)
            keys += s.map { .type($0) }
        }
        guard keys.count <= Self.maxKeys else { return nil }
        chars.replaceSubrange(range, with: Array(s))
        macSelection = nil
        macCaret = range.lowerBound + s.count
        return keys
    }

    private mutating func moveMac(to target: Int) -> [Key] {
        var keys: [Key] = []
        if let m = macSelection {
            // An arrow ends what the Mac has selected at that end, and goes no further.
            keys = [target >= m.upperBound ? .right : .left]
            macCaret = target >= m.upperBound ? m.upperBound : m.lowerBound
            macSelection = nil
        }
        let n = target - macCaret
        macCaret = target
        return keys + Array(repeating: n > 0 ? .right : .left, count: abs(n))
    }

    /// Where a range ends up after another is replaced: after it, it moves with it; inside it, to its end.
    private func shifted(_ r: Range<Int>, by range: Range<Int>, to length: Int) -> Range<Int> {
        func at(_ p: Int) -> Int {
            if p >= range.upperBound { return p - range.count + length }
            if p > range.lowerBound { return range.lowerBound + length }
            return p
        }
        return at(r.lowerBound)..<max(at(r.lowerBound), at(r.upperBound))
    }
}
