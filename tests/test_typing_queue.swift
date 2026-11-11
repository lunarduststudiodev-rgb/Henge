import Foundation

// Everything Henge types at the Mac's cursor (ios/Henge/TypingQueue.swift): in order, never mixed, a dropped piece
// stopping before its next key (and letting go of a shortcut's), Return only after all of the password, and what was
// asked for before the person's last press dropped as the focus moves. Drops happen at a given key, from the recorder
// itself, on a clock the test sets, so each run is the same. make test-typing-queue
@MainActor
final class Recorder: TypingSink {
    var keys: [String] = []
    var strokes = 0
    /// The queue's clock.
    var now: UInt64 = 10
    /// When the person last pressed something, on that clock.
    var lastPress: UInt64 = 0
    /// Run just after the Nth character or key going down or up (counting from 1, over the whole run).
    var after: [Int: () -> Void] = [:]

    func begin() { keys.append("|") }
    func type(_ c: Character) {
        keys.append(String(c))
        stroke()
    }
    /// A key down is "<code", up ">": a press reads "<28>", ⌘V "<125<47>>".
    func key(_ code: UInt16, down: Bool) {
        keys.append(down ? "<\(code)" : ">")
        stroke()
    }
    func press(_ code: UInt16) {
        keys.append("<\(code)>")
        stroke()
    }
    /// Not pressed: "(28 why)".
    func notPressed(_ code: UInt16, _ why: String) { keys.append("(\(code) \(why))") }
    func gap() async { await Task.yield() }

    private func stroke() {
        strokes += 1
        after.removeValue(forKey: strokes)?()
    }

    var said: String { keys.joined() }
}

@main
struct TypingQueueTests {
    static var failures = 0

    static func check(_ ok: Bool, _ what: String, line: Int = #line) {
        if !ok {
            failures += 1
            print("FAIL line \(line): \(what)")
        }
    }

    static let enter: UInt16 = 28
    enum Keys {
        static let command: UInt16 = 125
        static let option: UInt16 = 56
        static let v: UInt16 = 47
    }

    @MainActor static func fresh() -> (TypingQueue, Recorder) {
        let r = Recorder()
        return (TypingQueue(sink: r, clock: { r.now }, lastPress: { r.lastPress }), r)
    }

    /// Until nothing of anyone's is left (a piece may be added while another is typed).
    @MainActor static func settle(_ q: TypingQueue) async {
        let all: [TypingQueue.Who] = [.dictation, .password, .other]
        repeat {
            for who in all { await q.whenDone(who) }
        } while all.contains(where: q.busy)
    }

    @MainActor static func main() async {
        // In the order asked, one piece at a time, each from its own start (a latched ⌘ let go).
        do {
            let (q, r) = fresh()
            q.add("ab", by: .dictation)
            q.add("xy", by: .password, thenPress: enter)
            q.add("z", by: .other)
            await settle(q)
            check(r.said == "|ab|xy<28>|z", "in order, each piece whole: \(r.said)")
        }

        // The password asked for while a phrase is being typed waits for it: their letters don't mix.
        do {
            let (q, r) = fresh()
            r.after[2] = { q.add("pw", by: .password, thenPress: enter) }
            q.add("hello", by: .dictation)
            await settle(q)
            check(r.said == "|hello|pw<28>", "the password after the phrase, not inside it: \(r.said)")
        }

        // Cancelling dictation stops what was already waiting, and the phrase being typed before its next letter.
        do {
            let (q, r) = fresh()
            var results: [Bool] = []
            r.after[2] = { q.drop([.dictation]) }
            q.add("hello", by: .dictation, done: { results.append($0) })
            q.add(" world", by: .dictation, done: { results.append($0) })
            await settle(q)
            check(r.said == "|he", "nothing after the cancel: \(r.said)")
            check(results == [false, false], "both phrases said dropped: \(results)")
            check(!q.busy(.dictation), "nothing of dictation's left")
        }

        // The focus moving mid-password: the rest isn't typed and Return isn't pressed.
        do {
            let (q, r) = fresh()
            r.after[3] = { q.drop([.dictation, .password]) }
            let whole = await q.type("secret", by: .password, thenPress: enter)
            check(r.said == "|sec", "no more letters and no Return: \(r.said)")
            check(!whole, "said dropped")
        }

        // Dropped after its last letter, before Return: still no Return.
        do {
            let (q, r) = fresh()
            r.after[2] = { q.drop([.password]) }
            let whole = await q.type("pw", by: .password, thenPress: enter)
            check(r.said == "|pw", "no Return after a dropped password: \(r.said)")
            check(!whole, "said dropped")
        }

        // A password typed whole says so.
        do {
            let (q, r) = fresh()
            let whole = await q.type("pw", by: .password, thenPress: enter)
            check(whole && r.said == "|pw<28>", "typed whole with Return: \(r.said)")
        }

        // Dropping dictation's leaves the rest: a paste waiting after the phrase is still typed.
        do {
            let (q, r) = fresh()
            r.after[1] = { q.drop([.dictation]) }
            q.add("ab", by: .dictation)
            q.add("cd", by: .other)
            await settle(q)
            check(r.said == "|a|cd", "the paste still goes: \(r.said)")
        }

        // The password key while dictating (MacPassword.typeOrAsk): dictation's words are dropped first, then the
        // password goes whole, and none of dictation's letters after it.
        do {
            let (q, r) = fresh()
            r.after[3] = {
                q.drop([.dictation])
                q.add("pw", by: .password, thenPress: enter)
            }
            q.add("hello", by: .dictation)
            q.add(" world", by: .dictation)
            await settle(q)
            check(r.said == "|hel|pw<28>", "the password alone after the cut: \(r.said)")
        }

        // The Mac pausing drops everything, the setup command too.
        do {
            let (q, r) = fresh()
            r.after[1] = { q.drop([.dictation, .password, .other]) }
            q.add("ab", by: .other)
            q.add("cd", by: .dictation)
            q.add("ef", by: .password, thenPress: enter)
            await settle(q)
            check(r.said == "|a", "nothing after the pause: \(r.said)")
        }

        // Waiting for dictation's typing (Dictation.stop): back at once with nothing to type; after the last phrase
        // otherwise; and back as soon as it is dropped.
        do {
            let (q, r) = fresh()
            await q.whenDone(.dictation)
            check(r.said == "", "nothing to wait for")
            q.add("ab", by: .dictation)
            q.add("cd", by: .other)
            await q.whenDone(.dictation)
            check(r.said.hasPrefix("|ab") && !q.busy(.dictation), "back once the phrase is typed: \(r.said)")
            await settle(q)

            let (q2, r2) = fresh()
            r2.after[1] = { q2.drop([.dictation]) }
            q2.add("abc", by: .dictation)
            await q2.whenDone(.dictation)
            check(r2.said == "|a", "back once dropped: \(r2.said)")
        }

        // A piece added after the queue went idle starts by itself.
        do {
            let (q, r) = fresh()
            await q.type("a", by: .other)
            await q.type("b", by: .other)
            check(r.said == "|a|b", "again after idle: \(r.said)")
        }

        // Who moved the focus. The person tapped the next field (a press at 20) and pressed the password key (asked
        // at 21) before the helper said the focus moved there: it goes on, Return and all.
        do {
            let (q, r) = fresh()
            r.now = 21
            r.after[2] = { q.drop([.dictation, .password], askedBefore: 20) }
            let whole = await q.type("secret", by: .password, thenPress: enter)
            check(whole && r.said == "|secret<28>", "asked after the press that moved the focus: typed whole: \(r.said)")
        }

        // The key pressed with nothing focused (asked at 10), then a tap on a chat box (a press at 20) mid-password:
        // the rest isn't typed there.
        do {
            let (q, r) = fresh()
            r.after[2] = { q.drop([.dictation, .password], askedBefore: 20) }
            let whole = await q.type("secret", by: .password, thenPress: enter)
            check(!whole && r.said == "|se", "asked before the press: dropped: \(r.said)")
        }

        // A phrase said before the press (asked: when it was said), queued after it: dropped; one said after, kept;
        // and the count says so.
        do {
            let (q, r) = fresh()
            r.now = 30
            var count = -1
            q.add("for the last field", by: .dictation, asked: 15)
            q.add("for this one", by: .dictation, asked: 25)
            r.after[1] = { count = q.drop([.dictation, .password], askedBefore: 20) }
            await settle(q)
            check(r.said == "|f|for this one", "only what was said after the press: \(r.said)")
            check(count == 1, "one piece dropped (cut short): \(count)")
        }

        // A shortcut is one piece: ⌘ down, V, ⌘ up, never with dictation's letters between.
        do {
            let (q, r) = fresh()
            r.after[1] = { q.add("hi", by: .dictation) }
            q.add(shortcut: [Keys.command], Keys.v, by: .other)
            await settle(q)
            check(r.said == "|<125<47>>|hi", "the paste whole, then the phrase: \(r.said)")
        }

        // Cut mid-shortcut (the Mac pausing): the held ⌘ is let go, the key never pressed.
        do {
            let (q, r) = fresh()
            r.after[1] = { q.drop([.dictation, .password, .other]) }
            let whole = await q.type(shortcut: [Keys.command, Keys.option], 57, by: .other)
            check(!whole && r.said == "|<125>", "nothing stays down: \(r.said)")
        }

        // Return is decided as it is due, not as the password is asked for: the focus known to have left the
        // password field meanwhile (a tap on a chat box), the password is typed and Return isn't pressed.
        do {
            let (q, r) = fresh()
            var stillPassword = true
            r.after[1] = { stillPassword = false }
            let whole = await q.type("pw", by: .password, thenPress: enter, pressIf: { stillPassword })
            check(whole && r.said == "|pw", "typed, no Return: \(r.said)")
            let (q2, r2) = fresh()
            await q2.type("pw", by: .password, thenPress: enter, pressIf: { true })
            check(r2.said == "|pw<28>", "Return while it still is: \(r2.said)")
        }

        // Dropped while it is asked whether Return goes (the person moved the focus as the helper looked): no Return.
        do {
            let (q, r) = fresh()
            let whole = await q.type("pw", by: .password, thenPress: enter, pressIf: {
                await Task.yield()
                q.drop([.password])
                return true
            })
            check(!whole && r.said == "|pw", "cut as it is asked, no Return: \(whole) \(r.said)")
        }

        // Cut while the answer is still to come: the asking is cancelled and the queue goes on at once, not when the
        // answer (or the wait for it) is over.
        do {
            let (q, r) = fresh()
            var asked = false
            var cancelled = false
            let start = ContinuousClock.now
            let cutter = Task { @MainActor in
                while !asked { await Task.yield() }
                q.drop([.password])
            }
            let whole = await q.type("pw", by: .password, thenPress: enter, pressIf: {
                asked = true
                try? await Task.sleep(for: .seconds(5))
                cancelled = Task.isCancelled
                return true
            })
            await cutter.value
            let waited = ContinuousClock.now - start
            check(!whole && r.said == "|pw" && cancelled && waited < .seconds(2),
                  "cut while asked: cancelled, no Return, no wait: \(whole) \(r.said) \(cancelled) \(waited)")
            await q.type("ok", by: .other)
            check(r.said == "|pw|ok", "the next piece goes: \(r.said)")
        }

        // A press after the piece was asked for (a tap on a chat box as the password is typed, which the Mac may not
        // have acted on yet): no Return, whatever pressIf would say; not even asked when it came first. One before
        // it was asked for (what put the focus in the field) doesn't count.
        do {
            let (q, r) = fresh()
            r.now = 50
            r.lastPress = 49
            var asks = 0
            r.after[1] = { r.lastPress = 51 }
            let whole = await q.type("pw", by: .password, thenPress: enter, pressIf: { asks += 1; return true })
            check(whole && asks == 0 && r.said == "|pw(28 something was pressed after it was asked for)",
                  "pressed as it was typed: not asked, no Return: \(asks) \(r.said)")
            let (q2, r2) = fresh()
            r2.now = 50
            r2.lastPress = 49
            await q2.type("pw", by: .password, thenPress: enter, pressIf: { r2.lastPress = 52; return true })
            check(r2.said == "|pw(28 something was pressed after it was asked for)", "pressed as it was asked: no Return: \(r2.said)")
            let (q3, r3) = fresh()
            r3.now = 50
            r3.lastPress = 49
            await q3.type("pw", by: .password, thenPress: enter, pressIf: { true })
            check(r3.said == "|pw<28>", "a press before it was asked for: Return: \(r3.said)")
        }

        // Cut mid-shortcut: the held keys are let go at once, as it is cut, not after the next gap.
        do {
            let (q, r) = fresh()
            var atCut = ""
            r.after[1] = {
                q.drop([.other])
                atCut = r.said
            }
            await q.type(shortcut: [Keys.command], Keys.v, by: .other)
            check(atCut == "|<125>" && r.said == "|<125>", "let go as it is cut: \(atCut), then \(r.said)")
        }

        if failures > 0 {
            print("\(failures) failed")
            exit(1)
        }
        print("typing queue: all passed")
    }
}
