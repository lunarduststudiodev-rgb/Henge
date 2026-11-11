import Foundation

/// Where the queue's keys go: the Mac's keyboard in the app (MacKeyboard), a recorder in tests/test_typing_queue.swift.
@MainActor
protocol TypingSink: AnyObject {
    /// Before a piece's first key (the app lets go of a ⌃, ⌥ or ⌘ held on the row or a hardware keyboard, or "hello"
    /// would be ⌘H; it does so before each character too, for one pressed meanwhile).
    func begin()
    func type(_ c: Character)
    func key(_ code: UInt16, down: Bool)
    /// The key pressed after a piece (Return): as the person's own press, since what it does (a login, a message
    /// sent) moves the focus as theirs would.
    func press(_ code: UInt16)
    /// That key isn't pressed, and why (the app logs it). Not said when pressIf said no: it says why itself.
    func notPressed(_ code: UInt16, _ why: String)
    /// Between two keys, so the Mac keeps up (the app: 25 ms).
    func gap() async
}

/// Everything Henge types at the Mac's cursor, one piece at a time, in the order asked: dictation's phrases, the Mac's
/// password, and the rest (a paste and its ⌘V, the Henge Tools installer's shortcuts and setup command, autopilot).
/// Two pieces never mix their keys, and a Return asked for after a piece goes only when all of it went, the person
/// pressed nothing since it was asked for (a tap on a chat box, maybe not yet acted on in the Mac, would get it), and
/// its pressIf says so: looked at again just before it goes.
///
/// What is still to type can be dropped: dictation's when it is cancelled, what was asked for before the person moved
/// the Mac's focus (typed on, it would land in whatever they moved it to), and all of it as the Mac pauses. A piece
/// being typed stops before its next key, letting go of any it holds down; one waiting never starts.
@MainActor
final class TypingQueue {
    enum Who: Hashable {
        case dictation, password, other
    }

    typealias PressIf = @MainActor () async -> Bool

    enum Stroke: Equatable {
        case char(Character)
        case down(UInt16)
        case up(UInt16)
    }

    private struct Piece {
        let who: Who
        let strokes: [Stroke]
        let thenPress: UInt16?
        /// Asked as it is due: whether `thenPress` still goes (the password's Return: whether a password field has the
        /// focus now, MacPassword). The piece is still being typed while it is asked, so a drop meanwhile cuts it, and
        /// cancels the asking task: no Return.
        let pressIf: PressIf?
        /// When it was asked for, on the clock: for a phrase, when it was said.
        let asked: UInt64
        let done: ((Bool) -> Void)?
    }

    var sink: TypingSink
    /// The person's presses are on the same clock (henge_vm_last_press: mach_absolute_time).
    private let clock: () -> UInt64
    /// When the person last pressed something on the Mac (a key, a button, a finger), on the clock.
    private let lastPress: () -> UInt64
    private var waiting: [Piece] = []
    private var typing: Piece?
    private var cut = false // the piece being typed was dropped
    private var holding: [UInt16] = [] // the keys it holds down now (a shortcut's ⌘), let go as it is cut
    private var asking: Task<Bool, Never>? // the piece's pressIf, while it is asked: cancelled as the piece is cut
    private var working = false
    private var idle: [(who: Who, resume: () -> Void)] = []

    init(sink: TypingSink, clock: @escaping () -> UInt64, lastPress: @escaping () -> UInt64) {
        self.sink = sink
        self.clock = clock
        self.lastPress = lastPress
    }

    /// Typed after what is already waiting; `thenPress` (Return) only if all of it went, and `pressIf` says so then.
    /// `done` says whether all of it went. asked: when it was asked for, if not now (a phrase: when it was said).
    func add(_ text: String, by who: Who, thenPress: UInt16? = nil, pressIf: PressIf? = nil, asked: UInt64? = nil,
             done: ((Bool) -> Void)? = nil) {
        add(text.map { .char($0) }, by: who, thenPress: thenPress, pressIf: pressIf, asked: asked, done: done)
    }

    /// A shortcut: the modifiers held down for the key (⌘V), as one piece.
    func add(shortcut modifiers: [UInt16], _ key: UInt16, by who: Who, done: ((Bool) -> Void)? = nil) {
        add(modifiers.map { .down($0) } + [.down(key), .up(key)] + modifiers.reversed().map { .up($0) }, by: who, done: done)
    }

    func add(_ strokes: [Stroke], by who: Who, thenPress: UInt16? = nil, pressIf: PressIf? = nil, asked: UInt64? = nil,
             done: ((Bool) -> Void)? = nil) {
        waiting.append(Piece(who: who, strokes: strokes, thenPress: thenPress, pressIf: pressIf, asked: asked ?? clock(), done: done))
        guard !working else { return }
        working = true
        Task { await work() }
    }

    /// As `add`, and back once it is typed (true) or dropped (false).
    @discardableResult
    func type(_ text: String, by who: Who, thenPress: UInt16? = nil, pressIf: PressIf? = nil) async -> Bool {
        await withCheckedContinuation { (go: CheckedContinuation<Bool, Never>) in
            add(text, by: who, thenPress: thenPress, pressIf: pressIf) { go.resume(returning: $0) }
        }
    }

    @discardableResult
    func type(shortcut modifiers: [UInt16], _ key: UInt16, by who: Who) async -> Bool {
        await withCheckedContinuation { (go: CheckedContinuation<Bool, Never>) in
            add(shortcut: modifiers, key, by: who) { go.resume(returning: $0) }
        }
    }

    /// Drops what these have still to type, or only what was asked for before `askedBefore` (the person's last
    /// press, as the focus moves): the piece being typed stops before its next key, and lets go of any it holds now.
    /// Says how many pieces it dropped, counting one cut short.
    @discardableResult
    func drop(_ whose: Set<Who>, askedBefore: UInt64? = nil) -> Int {
        func goes(_ piece: Piece) -> Bool { whose.contains(piece.who) && piece.asked < (askedBefore ?? .max) }
        let gone = waiting.filter(goes)
        waiting.removeAll(where: goes)
        var count = gone.count
        if let typing, !cut, goes(typing) {
            cut = true
            count += 1
            for code in holding.reversed() { sink.key(code, down: false) }
            holding = []
            asking?.cancel() // no Return now, whatever it would say: the queue needn't wait for its answer
        }
        for piece in gone { piece.done?(false) }
        wake()
        return count
    }

    /// Whether any of `who`'s pieces is waiting or being typed.
    func busy(_ who: Who) -> Bool {
        typing?.who == who || waiting.contains { $0.who == who }
    }

    /// Back once nothing of `who`'s is waiting or being typed.
    func whenDone(_ who: Who) async {
        guard busy(who) else { return }
        await withCheckedContinuation { (go: CheckedContinuation<Void, Never>) in
            idle.append((who, { go.resume() }))
        }
    }

    private func work() async {
        while !waiting.isEmpty {
            let piece = waiting.removeFirst()
            typing = piece
            cut = false
            sink.begin()
            for stroke in piece.strokes {
                if cut { break }
                switch stroke {
                case .char(let c): sink.type(c)
                case .down(let code):
                    holding.append(code)
                    sink.key(code, down: true)
                case .up(let code):
                    holding.removeAll { $0 == code }
                    sink.key(code, down: false)
                }
                await sink.gap()
            }
            // A piece that didn't let go of all it held (a shortcut asked that way): nothing stays down.
            for code in holding.reversed() { sink.key(code, down: false) }
            holding = []
            if !cut, let key = piece.thenPress {
                let pressed = "something was pressed after it was asked for"
                if lastPress() > piece.asked {
                    sink.notPressed(key, pressed)
                } else if let pressIf = piece.pressIf {
                    let ask = Task { await pressIf() }
                    asking = ask
                    let goes = await ask.value
                    asking = nil
                    // On the main actor from here to the press: nothing the person does comes in between.
                    if goes && !cut {
                        if lastPress() > piece.asked { sink.notPressed(key, pressed) } else { sink.press(key) }
                    }
                } else {
                    sink.press(key)
                }
            }
            let whole = !cut
            typing = nil
            cut = false
            piece.done?(whole)
            wake()
        }
        working = false
    }

    private func wake() {
        let ready = idle.filter { !busy($0.who) }
        idle.removeAll { !busy($0.who) }
        for waiter in ready { waiter.resume() }
    }
}
