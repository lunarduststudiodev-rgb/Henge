import Foundation

/// Whether the iPad's keyboard should be up, decided from what the person does and what the Mac says has its focus.
/// Pure: inputs in, the wanted visibility and a reason out. KeyboardOwner feeds it and applies what it decides;
/// tests/test_keyboard_intent.swift checks its rules (the keyboard autoshow research notes §6.2):
/// - a tap on something that takes typing shows the keyboard, even one the person put away;
/// - the focus moving to another text field shows it only within `recentPress` of a key, click or tap, so an app
///   that focuses its own search box as it opens brings up nothing;
/// - the field the person put the keyboard away from keeps it away;
/// - the focus leaving text hides it only after something the person did, and only once it has stayed away for
///   `noneHold` (a page re-rendering its field passes through nothing for a moment); "after something the person
///   did" is reckoned from when the focus left the field, so an app that takes a while to say it has nothing focused
///   (one just opening, on a slow Mac) still hides it;
/// - when the helper can't tell, or its link comes and goes, nothing changes;
/// - on a slow Mac the focus is told late (an emulated Mac's apps take a second or more to move it and say so): the
///   focus reaching a text field still follows from a tap when it is the field the tap landed in, or, where the helper
///   couldn't say what the tap landed in and nothing was pressed since, a field it doesn't say the place of
///   (`tapLate`); and from a press when it is the first text field since it (`slowFollow`: ⌘Space, Spotlight's icon);
/// - at the login window (no helper runs there) its password field has the focus: the keyboard comes up, and a tap
///   brings it back after it was put away.
struct KeyboardIntent {
    enum State: Equatable {
        case hidden
        /// Shown for the field with this ID (a tap, or the focus moving to it).
        case shownAuto(UInt32)
        /// Shown because the person asked (the toolbar's keyboard button).
        case shownManual
        /// Put away by the person while the field with this ID had the focus: it stays away for that field.
        case dismissed(UInt32)
    }

    /// What has the Mac's focus, as the helper says (HV_FOCUS_* in host/share.h).
    enum Focus: Int32 {
        case unknown = -1, none = 0, text = 1, secure = 2
        var said: String {
            switch self {
            case .unknown: return "unknown"
            case .none: return "nothing to type into"
            case .text: return "a text field"
            case .secure: return "a password field"
            }
        }
    }

    /// What a tap landed in, as the helper says (HV_TAP_* in host/share.h).
    enum Tap: Int32 { case unknown = -1, no = 0, editable = 1, secure = 2 }

    /// Part of the Mac's screen, as fractions of it from its top left.
    struct Box: Equatable {
        var x = 0.0, y = 0.0, width = 0.0, height = 0.0
        /// With a little room around it: a finger's spot is a guess at where it meant.
        func holds(_ px: Double, _ py: Double) -> Bool {
            let pad = 0.01
            return width > 0 && height > 0 && px >= x - pad && px <= x + width + pad && py >= y - pad && py <= y + height + pad
        }
    }

    /// One "field" line from the helper.
    struct Field: Equatable {
        var serial: UInt32 = 0
        var id: UInt32 = 0
        var generation: UInt32 = 0
        var focus = Focus.unknown
        var kind: Int32 = 0
        /// Where the field is, when the helper says.
        var frame: Box?
    }

    /// The login window's password field, which no helper numbers (none runs before someone logs in).
    static let loginField: UInt32 = .max

    /// Why the keyboard went, for what dictation does then: the person put it away (the last words still go to
    /// the field, which still has the focus), the focus left text, or something took the screen.
    enum Hid: Equatable { case person, focus, closed }

    /// What the helper's line says of the Mac's focus, for what Henge is still typing (dictation's words, the
    /// password): it stays where it was; it moved; it moved from one field to another text field; or it left a field
    /// for something that isn't a text field (nothing, a button, a password field, an app that can't say yet), and
    /// dictation ends. `old` is the last line known for sure (KeyboardOwner keeps it through "can't tell" about the
    /// same field); only lines of one helper run are compared, as its IDs are its run's.
    ///
    /// Whether what is queued goes on isn't decided here but by who moved it (KeyboardOwner, TypingQueue): what was
    /// asked for before the person's last press was for where the focus was. Asked for since, it goes on: the
    /// person tapped a field and pressed the password key before the helper said so (an emulated Mac's apps take
    /// 0.3-0.7 s to), or the app moved the focus by itself (a code's next box).
    enum Typing: Equatable { case stays, moved, anotherField, leftText }

    static func typing(from old: Field?, to new: Field) -> Typing {
        guard let old, old.generation == new.generation else { return .stays }
        if old.id == new.id {
            return old.focus == .text && (new.focus == .none || new.focus == .secure) ? .leftText : .stays
        }
        guard old.focus == .text || old.focus == .secure else { return .moved }
        return new.focus == .text ? .anotherField : .leftText
    }

    /// What to do: show (true) or hide (false) the keyboard, or leave it as it is (nil); and the line for the log.
    struct Decision: Equatable {
        var show: Bool?
        var reason: String
        /// Set with every hide (show false).
        var hid: Hid? = nil
        /// The Mac's focus is on another field now: what was typed is somewhere else, and the stand-in text starts again.
        var fresh = false
    }

    /// Focus that arrives within this many seconds of a press follows from it.
    static let recentPress: Double = 1.5
    /// The first text field the focus reaches after a press follows from it this long after, on a Mac slow to say.
    static let slowFollow: Double = 4
    /// A tap whose answer didn't say it was a field: the focus reaching a field that holds where it landed, this long
    /// after, follows from it (an app still loading a page, or slow to answer what is at a point). So does one that
    /// doesn't say where it is, if the helper couldn't say what the tap landed in and nothing was pressed since.
    static let tapLate: Double = 6
    /// A press this long after a tap is another (the tap's own touch or click comes just before it is told here).
    static let laterPress: Double = 0.3
    /// Nothing focused must last this long, in seconds, to hide the keyboard.
    static let noneHold: Double = 0.3

    private(set) var state = State.hidden
    var isShown: Bool {
        switch state {
        case .shownAuto, .shownManual: return true
        case .hidden, .dismissed: return false
        }
    }

    /// Henge's "Show for Text Fields" switch: off, the keyboard comes and goes only when the person says.
    var auto = true
    /// A hardware keyboard is connected: iPadOS shows no keyboard of its own then.
    var hardwareKeyboard = false
    /// Something is in front of the Mac (a sheet, a pause, the app in the background): no keyboard of its own accord.
    var blocked = false

    /// The helper's last line.
    private(set) var field: Field?
    /// The field of the last line that wasn't unknown.
    private var knownID: UInt32?
    /// Since when nothing has had the focus, after something the person did, while the keyboard is up.
    private var noneSince: Double?
    private var noneAfter = ""
    /// What the last tap on something that takes no typing landed on, for the log.
    private var tappedOn: String?
    /// The focus left the field the keyboard is up for within `recentPress` of a press, though what has it now
    /// wasn't known yet: when, and what to say of it.
    private var left: (at: Double, said: String)?
    /// How long such a leaving counts for.
    static let leftCounts: Double = 10
    /// The last tap or click the helper didn't answer as a field: where it landed, 0...1 across the Mac's screen.
    /// unsaid: the helper hasn't said what it landed in (couldn't tell, or not in time).
    private var lastTap: (x: Double, y: Double, at: Double, unsaid: Bool)?
    /// When the focus was last on a text field, as the helper said.
    private var lastTextAt: Double?
    /// The Mac is at its login window, and no helper says what has the focus there.
    private(set) var atLogin = false

    /// Waiting to see whether the focus stays off text for `noneHold`: call `tick` then.
    var waitingForNone: Bool { noneSince != nil }

    private var cantShow: String? {
        if !auto { return "Show for Text Fields is off" }
        if hardwareKeyboard { return "a hardware keyboard is connected" }
        if blocked { return "something is in front of the Mac" }
        return nil
    }

    private static func ms(_ seconds: Double) -> Int { Int((max(seconds, 0) * 1000).rounded()) }

    // MARK: Helpers for the owner

    /// Where a click from a pointer (the trackpad's tap, a mouse) lands, to ask the helper about as a tap: the Mac's
    /// pointer as 0...1 across its screen, both in the guest's display pixels. Nil while either isn't known.
    static func clickSpot(pointerX: Int32, pointerY: Int32, screenWidth: Int32, screenHeight: Int32) -> (x: Double, y: Double)? {
        guard screenWidth > 1, screenHeight > 1 else { return nil }
        return (min(max(Double(pointerX) / Double(screenWidth), 0), 1), min(max(Double(pointerY) / Double(screenHeight), 0), 1))
    }

    /// Whether the password key is to treat what has the focus as a password field (and press Return after typing):
    /// only while a helper that says so is linked. Its "can't tell" leaves it as it was; no helper, never.
    static func passwordField(live: Bool, focus: Focus, was: Bool) -> Bool {
        guard live else { return false }
        return focus == .unknown ? was : focus == .secure
    }

    /// Why the password's Return isn't pressed, from the helper's answer to the look asked once the password was
    /// typed (as HengeField's state: -1 it couldn't tell, or the link closed first; 0 nothing to type into; 1 a text
    /// field; 2 a password field; sure: one it is sure of, which the field says it is or macOS's own prompt has the
    /// keys of). Nil: it goes. (A press since the password was asked for is TypingQueue's to refuse.)
    static func passwordReturnRefused(look answer: Int32, sure: Bool) -> String? {
        switch answer {
        case 2: return sure ? nil : "a password field only as its app holds secure input"
        case -1: return "the helper couldn't tell what has the focus"
        default: return "no password field has the focus"
        }
    }

    // MARK: Inputs

    /// A tap or click went to the Mac at x, y (0...1 across its screen), before anything says what it landed in.
    mutating func tapLanded(x: Double, y: Double, now: Double) {
        lastTap = (x, y, now, true)
    }

    /// A tap's answer from the helper: what it landed in, its role, and how long after the finger lifted it came.
    mutating func tapped(_ answer: Tap, role: String, afterLift: Double) -> Decision? {
        let what = role.isEmpty ? "something" : role
        switch answer {
        case .editable, .secure:
            tappedOn = nil
            lastTap = nil
            noneSince = nil
            left = nil
            if isShown { return nil }
            if let why = cantShow {
                return Decision(show: nil, reason: "not shown (tap on \(what), \(Self.ms(afterLift)) ms after lift; \(why))")
            }
            let away: String
            if case .dismissed = state { away = "; it had been put away" } else { away = "" }
            state = .shownAuto(knownID ?? 0)
            return Decision(show: true, reason: "show (tap on \(what), \(Self.ms(afterLift)) ms after lift\(away))")
        case .no:
            tappedOn = what // whether the focus left the field, the focus says
            lastTap?.unsaid = false // a field may still come under it (a page loading): only one that holds it counts
            return nil
        case .unknown:
            tappedOn = nil
            return isShown ? nil : Decision(show: nil, reason: "kept (helper unsure what a tap landed in\(role.isEmpty ? "" : ": \(role)"))")
        }
    }

    /// A field line. sincePress: seconds since the person last pressed a key, clicked or touched the Mac.
    mutating func focused(_ new: Field, sincePress: Double, now: Double) -> Decision? {
        let old = field
        field = new
        // A link that opens or closes says where things stand, not that anything moved: the field the helper names
        // first is taken as the one the keyboard was for (a new helper numbers its fields afresh).
        if old == nil || old?.generation != new.generation {
            noneSince = nil
            left = nil
            guard new.focus != .unknown else { return nil }
            knownID = new.id
            switch state {
            case .shownAuto: state = .shownAuto(new.id)
            case .dismissed: state = .dismissed(new.id)
            case .hidden, .shownManual: break
            }
            return old == nil ? nil : Decision(show: nil, reason: "kept (helper linked: focus on \(new.focus.said))")
        }
        let recent = sincePress <= Self.recentPress
        guard new.focus != .unknown else {
            noneSince = nil // not seen staying off text
            guard isShown else { return nil }
            if new.id != knownID, recent, left == nil {
                left = (now, "focus left field \(knownID ?? 0) \(Self.ms(sincePress)) ms after a press")
            }
            return Decision(show: nil, reason: "kept (helper unsure what has the focus)")
        }
        let another = new.id != knownID
        knownID = new.id
        switch new.focus {
        case .text, .secure:
            noneSince = nil
            left = nil
            // Whether a press since the focus was last on text led here, however long the Mac took to say.
            let firstSincePress = sincePress <= Self.slowFollow && (lastTextAt.map { now - sincePress > $0 } ?? true)
            lastTextAt = now
            // A press after the tap is another thing the person did, which the tap no longer speaks for.
            let tap = lastTap.flatMap { t in now - t.at <= Self.tapLate && now - sincePress <= t.at + Self.laterPress ? t : nil }
            let tapIn = tap.flatMap { t in new.frame?.holds(t.x, t.y) == true ? now - t.at : nil }
            let tapUnsaid = tap.flatMap { t in t.unsaid && new.frame == nil ? now - t.at : nil }
            let kind = new.focus.said
            if isShown {
                guard another else { return nil }
                if case .shownAuto = state { state = .shownAuto(new.id) }
                return Decision(show: nil, reason: "kept (focus moved to \(kind), field \(new.id))", fresh: true)
            }
            if case .dismissed(let id) = state, id == new.id {
                return old?.focus != new.focus ? Decision(show: nil, reason: "stays hidden (put away for field \(id))") : nil
            }
            guard another else { return nil }
            let why: String
            if let tapIn {
                why = "where a tap landed \(Self.ms(tapIn)) ms before"
            } else if let tapUnsaid {
                why = "\(Self.ms(tapUnsaid)) ms after a tap the helper couldn't say the landing of"
            } else if recent {
                why = "\(Self.ms(sincePress)) ms after a press"
            } else if firstSincePress {
                why = "\(Self.ms(sincePress)) ms after a press, the first field since it"
            } else {
                return Decision(show: nil, reason: "not shown (focus moved to \(kind) on its own, \(Self.ms(sincePress)) ms after the last press)",
                                fresh: true)
            }
            lastTap = nil
            if let cant = cantShow { return Decision(show: nil, reason: "not shown (focus on \(kind); \(cant))", fresh: true) }
            state = .shownAuto(new.id)
            return Decision(show: true, reason: "show (focus moved to \(kind), \(why))", fresh: true)
        case .none:
            guard isShown else { return nil }
            guard auto else { return Decision(show: nil, reason: "kept (focus left text; Show for Text Fields is off)") }
            let leftEarlier = left.flatMap { now - $0.at <= Self.leftCounts ? $0.said : nil }
            guard recent || leftEarlier != nil else { return Decision(show: nil, reason: "kept (focus left text with nothing pressed)") }
            if noneSince == nil {
                noneSince = now
                noneAfter = tappedOn.map { "tap on \($0)" } ?? leftEarlier ?? "focus left text \(Self.ms(sincePress)) ms after a press"
            }
            return nil
        case .unknown:
            return nil
        }
    }

    /// Time passing: once nothing has had the focus for `noneHold`, the keyboard goes.
    mutating func tick(now: Double) -> Decision? {
        guard let since = noneSince, now - since >= Self.noneHold - 0.005 else { return nil }
        noneSince = nil
        guard isShown, field?.focus == Focus.none else { return nil }
        left = nil
        state = .hidden
        return Decision(show: false, reason: "hide (\(noneAfter); none for \(Self.ms(now - since)) ms)", hid: .focus)
    }

    /// The person asked for the keyboard.
    mutating func userShowed(_ how: String) -> Decision? {
        noneSince = nil
        left = nil
        guard !isShown else { return nil }
        state = .shownManual
        return Decision(show: true, reason: "show (\(how))")
    }

    /// The Mac's login window came (on) or went (off), as Henge Tools' system part says, while no helper says what has
    /// the focus: its password field has it. The keyboard comes up for it as for a field the focus moved to; it isn't
    /// held for the desktop that follows.
    mutating func loginWindow(_ on: Bool) -> Decision? {
        guard on != atLogin else { return nil }
        atLogin = on
        noneSince = nil
        left = nil
        lastTap = nil
        if !on {
            knownID = field?.focus == .unknown ? knownID : field?.id
            switch state {
            case .shownAuto(Self.loginField):
                state = .hidden
                return Decision(show: false, reason: "hide (the login window went)", hid: .focus)
            case .dismissed(Self.loginField):
                state = .hidden
                return nil
            default:
                return nil
            }
        }
        knownID = Self.loginField
        switch state {
        case .shownAuto: state = .shownAuto(Self.loginField); return nil
        case .shownManual: return nil
        case .dismissed, .hidden: break
        }
        if let why = cantShow { return Decision(show: nil, reason: "not shown (the login window's password field; \(why))") }
        state = .shownAuto(Self.loginField)
        return Decision(show: true, reason: "show (the login window: its password field has the focus)", fresh: true)
    }

    /// The person put the keyboard away: it stays away from this field until a tap on a field.
    mutating func userHid(_ how: String) -> Decision? {
        noneSince = nil
        left = nil
        guard isShown else { return nil }
        let id = knownID ?? 0
        state = .dismissed(id)
        return Decision(show: false, reason: "hide (\(how)); stays hidden for field \(id) until a tap on a field", hid: .person)
    }

    /// Something took the screen from typing (the Mac paused, the downloads sheet, the controls hidden): the keyboard
    /// goes, and isn't held away from the field.
    mutating func close(_ why: String) -> Decision? {
        noneSince = nil
        left = nil
        guard isShown else { return nil }
        state = .hidden
        return Decision(show: false, reason: "hide (\(why))", hid: .closed)
    }
}
