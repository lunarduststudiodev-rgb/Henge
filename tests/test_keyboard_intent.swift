import Foundation

// The keyboard's rules (ios/Henge/KeyboardIntent.swift; the keyboard autoshow research notes §6.2 and §7), each as
// the person would meet it. make test-keyboard-intent
@main
struct KeyboardIntentTests {
    static var failures = 0

    static func check(_ ok: Bool, _ what: String, line: Int = #line) {
        if !ok {
            failures += 1
            print("FAIL line \(line): \(what)")
        }
    }

    static var serial: UInt32 = 0
    static func field(_ id: UInt32, _ focus: KeyboardIntent.Focus, generation: UInt32 = 1, kind: Int32 = 0,
                      frame: KeyboardIntent.Box? = nil) -> KeyboardIntent.Field {
        serial += 1
        return KeyboardIntent.Field(serial: serial, id: id, generation: generation, focus: focus, kind: kind, frame: frame)
    }

    /// Linked, with field 1 (nothing to type into) where things stand.
    static func linked() -> KeyboardIntent {
        var k = KeyboardIntent()
        _ = k.focused(field(1, .none), sincePress: 99, now: 0)
        return k
    }

    /// What becomes of what Henge is still typing (dictation's words, the password) as the helper's lines come.
    static func typingAsFocusMoves() {
        typealias K = KeyboardIntent
        let a = field(5, .text), aAgain = field(5, .text)
        check(K.typing(from: nil, to: a) == .stays, "the first line: nothing to compare")
        check(K.typing(from: a, to: aAgain) == .stays, "the same field again")
        check(K.typing(from: a, to: field(6, .text)) == .anotherField, "another text field")
        check(K.typing(from: a, to: field(6, .none)) == .leftText, "nothing to type into: dictation ends")
        check(K.typing(from: a, to: field(6, .secure)) == .leftText, "a password field: dictation ends")
        check(K.typing(from: a, to: field(6, .unknown)) == .leftText, "another app that can't say yet: the field was left")
        check(K.typing(from: a, to: field(5, .unknown)) == .stays, "can't tell, the same field: nothing changes")
        check(K.typing(from: a, to: field(5, .none)) == .leftText, "the same element no longer takes typing")
        check(K.typing(from: field(5, .unknown), to: field(5, .text)) == .stays, "back to telling: the same field")
        check(K.typing(from: a, to: field(6, .text, generation: 2)) == .stays,
              "a new helper's IDs aren't the last one's: the focus may not have moved")
        check(K.typing(from: field(5, .none), to: field(6, .text)) == .moved, "to a field from nothing: it moved")
        check(K.typing(from: field(5, .unknown), to: field(6, .text)) == .moved, "from can't tell: the same")
        check(K.typing(from: field(5, .none), to: field(6, .none)) == .moved, "from a button to another: moved")
        check(K.typing(from: field(5, .secure), to: field(6, .text)) == .anotherField, "a password field for a text field")
        check(K.typing(from: field(5, .secure), to: field(6, .none)) == .leftText, "a password field for nothing")
    }

    static func main() {
        // A tap on a field shows the keyboard at once, before the focus report.
        do {
            var k = linked()
            let d = k.tapped(.editable, role: "AXTextField", afterLift: 0.04)
            check(d?.show == true && k.isShown, "a tap on a field shows it")
            check(d?.reason.contains("AXTextField") == true && d?.reason.contains("40 ms") == true, "the reason names the role and the time: \(d?.reason ?? "")")
            // Its focus report follows: the same keyboard, the stand-in text starts again.
            let f = k.focused(field(2, .text), sincePress: 0.1, now: 0.1)
            check(f?.show == nil && f?.fresh == true && k.isShown, "the focus following a tap keeps it, fresh")
        }
        // Focus moving to a field just after a press (⌘L, ⌘Space, Rename) shows it; on its own it doesn't (K5).
        do {
            var k = linked()
            let d = k.focused(field(2, .text), sincePress: 0.4, now: 1)
            check(d?.show == true && d?.fresh == true, "focus after a press shows it")
            var quiet = linked()
            let q = quiet.focused(field(2, .text), sincePress: 8, now: 1)
            check(q?.show == nil && !quiet.isShown, "an app focusing its own field brings up nothing")
            check(q?.reason.hasPrefix("not shown") == true, "and says so: \(q?.reason ?? "")")
        }
        // Typing: the caret moving, the same field reported again, a page's field re-rendered, nothing hides it (K2).
        do {
            var k = linked()
            _ = k.tapped(.editable, role: "AXTextArea", afterLift: 0.02)
            _ = k.focused(field(2, .text), sincePress: 0.05, now: 0.1)
            check(k.focused(field(2, .text), sincePress: 0.05, now: 0.5) == nil, "the same field again: nothing")
            let u = k.focused(field(3, .unknown), sincePress: 0.05, now: 0.6)
            check(u?.show == nil && k.isShown && u?.reason.contains("unsure") == true, "unknown keeps it, and says so")
            let r = k.focused(field(3, .text), sincePress: 0.05, now: 0.7)
            check(r?.show == nil && k.isShown, "a re-rendered field keeps it")
            // A web app blurring its field with nothing pressed for a while: kept.
            _ = k.focused(field(4, .none), sincePress: 6, now: 7)
            check(k.isShown && !k.waitingForNone, "a blur with nothing pressed keeps it")
            check(k.tick(now: 9) == nil && k.isShown, "and time passing doesn't hide it")
        }
        // Field A to field B of the same kind keeps the keyboard, through a moment of nothing between them.
        do {
            var k = linked()
            _ = k.tapped(.editable, role: "AXTextField", afterLift: 0.02)
            _ = k.focused(field(2, .text), sincePress: 0.05, now: 0.1)
            _ = k.focused(field(3, .none), sincePress: 0.05, now: 1.0) // Tab: passes through nothing
            check(k.waitingForNone, "nothing after a press is watched")
            let b = k.focused(field(4, .text), sincePress: 0.15, now: 1.1)
            check(b?.fresh == true && k.isShown, "field B keeps it, fresh")
            check(k.tick(now: 1.4) == nil && k.isShown, "no hide once B has the focus")
        }
        // A tap on a button (or a page's empty area) moves the focus off text: hidden once it stays off for 300 ms.
        do {
            var k = linked()
            _ = k.tapped(.editable, role: "AXTextField", afterLift: 0.02)
            _ = k.focused(field(2, .text), sincePress: 0.05, now: 0.1)
            check(k.tapped(.no, role: "AXButton", afterLift: 0.03) == nil, "a tap on a button alone changes nothing")
            _ = k.focused(field(3, .none), sincePress: 0.08, now: 2.0)
            check(k.tick(now: 2.1) == nil && k.isShown, "not before 300 ms")
            let h = k.tick(now: 2.32)
            check(h?.show == false && !k.isShown, "hidden after 300 ms of nothing")
            check(h?.reason == "hide (tap on AXButton; none for 320 ms)", "reason: \(h?.reason ?? "")")
            check(h?.hid == .focus, "hidden as the focus left text: dictation ends")
        }
        // Nothing, then back to text within 300 ms: kept.
        do {
            var k = linked()
            _ = k.userShowed("the toolbar's keyboard button")
            _ = k.focused(field(2, .text), sincePress: 0.05, now: 0.1)
            _ = k.focused(field(3, .none), sincePress: 0.05, now: 1.0)
            _ = k.focused(field(4, .unknown), sincePress: 0.1, now: 1.1)
            check(!k.waitingForNone, "unknown isn't nothing staying")
            check(k.tick(now: 1.5) == nil && k.isShown, "kept")
        }
        // Safari's field, then a tap on another app that takes a while to say it has nothing focused (one opening on a
        // slow Mac): the focus left the field right after the tap, so it hides once the app says so.
        do {
            var k = linked()
            _ = k.tapped(.editable, role: "AXTextField", afterLift: 0.03)
            _ = k.focused(field(2, .text), sincePress: 0.1, now: 0.1)
            _ = k.tapped(.no, role: "AXDockItem", afterLift: 0.03)
            _ = k.focused(field(3, .unknown), sincePress: 0.2, now: 10.2) // another app in front, not answering yet
            check(k.isShown, "unknown keeps it for now")
            _ = k.focused(field(4, .none), sincePress: 3.5, now: 13.5) // it answers, long after the tap
            check(k.waitingForNone, "nothing, once the focus left the field after a press, is watched")
            let h = k.tick(now: 13.8)
            check(h?.show == false && !k.isShown, "and hides it")
            check(h?.reason == "hide (tap on AXDockItem; none for 300 ms)", "reason: \(h?.reason ?? "")")
        }
        // The same without a tap answer (a key, ⌘Tab): the log says the focus left.
        do {
            var k = linked()
            _ = k.userShowed("the toolbar's keyboard button")
            _ = k.focused(field(2, .text), sincePress: 0.1, now: 0.1)
            _ = k.focused(field(3, .unknown), sincePress: 0.4, now: 5.4)
            _ = k.focused(field(4, .none), sincePress: 2.4, now: 7.4)
            let h = k.tick(now: 7.7)
            check(h?.show == false, "hidden")
            check(h?.reason == "hide (focus left field 2 400 ms after a press; none for 300 ms)", "reason: \(h?.reason ?? "")")
        }
        // But the focus leaving on its own (nothing pressed), or the leaving long forgotten, keeps it.
        do {
            var k = linked()
            _ = k.userShowed("the toolbar's keyboard button")
            _ = k.focused(field(2, .text), sincePress: 0.1, now: 0.1)
            _ = k.focused(field(3, .unknown), sincePress: 5, now: 5)
            _ = k.focused(field(4, .none), sincePress: 6, now: 6)
            check(k.isShown && !k.waitingForNone, "left on its own: kept")
            var late = linked()
            _ = late.userShowed("the toolbar's keyboard button")
            _ = late.focused(field(2, .text), sincePress: 0.1, now: 0.1)
            _ = late.focused(field(3, .unknown), sincePress: 0.2, now: 0.2)
            _ = late.focused(field(4, .none), sincePress: 20, now: 20)
            check(late.isShown && !late.waitingForNone, "left after a press, but 20 s before anything was known: kept")
            var back = linked()
            _ = back.userShowed("the toolbar's keyboard button")
            _ = back.focused(field(2, .text), sincePress: 0.1, now: 0.1)
            _ = back.focused(field(3, .unknown), sincePress: 0.2, now: 0.2)
            _ = back.focused(field(4, .text), sincePress: 2, now: 2)
            _ = back.focused(field(5, .none), sincePress: 4, now: 4)
            check(back.isShown && !back.waitingForNone, "back in a field since: that leaving no longer counts")
        }
        // Dismissed, then the same field reported again or relinked: stays hidden (K4). A tap on it: back (K3).
        do {
            var k = linked()
            _ = k.tapped(.editable, role: "AXTextField", afterLift: 0.02)
            _ = k.focused(field(7, .text), sincePress: 0.05, now: 0.1)
            let d = k.userHid("the row's hide button")
            check(d?.show == false && k.state == .dismissed(7), "put away for field 7")
            check(d?.hid == .person, "put away by the person: dictation's last words still go to the field")
            check(k.focused(field(7, .text), sincePress: 0.1, now: 1) == nil && !k.isShown, "the same field: stays hidden")
            let s = k.focused(field(7, .secure), sincePress: 0.1, now: 1.1)
            check(!k.isShown && s?.reason == "stays hidden (put away for field 7)", "and says so: \(s?.reason ?? "")")
            // The helper restarts: a new link names the field afresh (its IDs start again).
            let r = k.focused(field(1, .text, generation: 2), sincePress: 0.2, now: 2)
            check(!k.isShown && k.state == .dismissed(1), "a relink keeps it put away, for the field it names")
            check(r?.show == nil, "a relink changes nothing")
            check(k.focused(field(1, .text, generation: 2), sincePress: 0.2, now: 2.1) == nil && !k.isShown, "still hidden")
            let t = k.tapped(.editable, role: "AXTextField", afterLift: 0.05)
            check(t?.show == true && k.isShown, "a tap on the field brings it back")
            check(t?.reason.contains("put away") == true, "and says it had been: \(t?.reason ?? "")")
        }
        // Dismissed, then another field right after a press: shown (a new field is a new start).
        do {
            var k = linked()
            _ = k.userShowed("the toolbar's keyboard button")
            _ = k.focused(field(2, .text), sincePress: 0.05, now: 0.1)
            _ = k.userHid("iPadOS's hide-keyboard key")
            let d = k.focused(field(3, .text), sincePress: 0.2, now: 2)
            check(d?.show == true, "another field after a press shows it")
        }
        // A link that closes (generation goes up, state unknown) and opens again: nothing changes.
        do {
            var k = linked()
            _ = k.tapped(.editable, role: "AXTextField", afterLift: 0.02)
            _ = k.focused(field(2, .text), sincePress: 0.05, now: 0.1)
            check(k.focused(field(2, .unknown, generation: 2), sincePress: 0.05, now: 0.5) == nil && k.isShown, "the link closing")
            _ = k.focused(field(1, .none, generation: 3), sincePress: 0.05, now: 0.6)
            check(k.isShown && !k.waitingForNone, "a new link saying none doesn't hide it")
            check(k.tick(now: 2) == nil && k.isShown, "even later")
        }
        // A hardware keyboard, Show for Text Fields off, or a sheet: no keyboard of its own accord.
        do {
            var k = linked()
            k.hardwareKeyboard = true
            let d = k.tapped(.editable, role: "AXTextField", afterLift: 0.02)
            check(d?.show == nil && !k.isShown && d?.reason.contains("hardware keyboard") == true, "hardware keyboard: \(d?.reason ?? "")")
            check(k.userShowed("the toolbar's keyboard button")?.show == true, "but the person can still ask")
            var off = linked()
            off.auto = false
            check(off.focused(field(2, .text), sincePress: 0.1, now: 1)?.show == nil && !off.isShown, "Show for Text Fields off")
            var sheet = linked()
            sheet.blocked = true
            check(sheet.tapped(.secure, role: "AXTextField/AXSecureTextField", afterLift: 0.02)?.show == nil, "a sheet in front")
        }
        // Unsure taps change nothing, and say so while hidden.
        do {
            var k = linked()
            let d = k.tapped(.unknown, role: "timed-out", afterLift: 0.15)
            check(d?.show == nil && !k.isShown && d?.reason == "kept (helper unsure what a tap landed in: timed-out)", "unsure tap: \(d?.reason ?? "")")
        }
        // The person's own toggles, and closing.
        do {
            var k = linked()
            check(k.userShowed("the toolbar's keyboard button")?.show == true && k.state == .shownManual, "shown on request")
            _ = k.focused(field(2, .text), sincePress: 0.1, now: 0.1)
            check(k.state == .shownManual, "a field after a manual show keeps it manual")
            let c = k.close("the Mac paused")
            check(c?.show == false && k.state == .hidden && c?.hid == .closed, "closed, not put away")
            check(k.focused(field(2, .text), sincePress: 0.1, now: 0.2) == nil, "the same field again: nothing new")
        }
        // A click from a pointer (trackpad mode's tap, a mouse) is asked about where the Mac's pointer is, and in
        // the note the keyboard was put away from it brings it back, as a finger's tap does.
        do {
            let spot = KeyboardIntent.clickSpot(pointerX: 960, pointerY: 300, screenWidth: 1920, screenHeight: 1200)
            check(spot?.x == 0.5 && spot?.y == 0.25, "the pointer as a fraction of the screen: \(String(describing: spot))")
            let edge = KeyboardIntent.clickSpot(pointerX: 2000, pointerY: -5, screenWidth: 1920, screenHeight: 1200)
            check(edge?.x == 1 && edge?.y == 0, "held to the screen: \(String(describing: edge))")
            check(KeyboardIntent.clickSpot(pointerX: 5, pointerY: 5, screenWidth: 0, screenHeight: 0) == nil, "no screen yet: no ask")
            var k = linked()
            _ = k.tapped(.editable, role: "AXTextArea", afterLift: 0.03)
            _ = k.focused(field(2, .text), sincePress: 0.1, now: 0.1)
            check(k.userHid("iPadOS's hide key")?.show == false && k.state == .dismissed(2), "put away in the note")
            let d = k.tapped(.editable, role: "AXTextArea", afterLift: 0.02)
            check(d?.show == true && k.isShown, "a click in the same note brings it back: \(d?.reason ?? "")")
        }
        // The password key presses Return after the password only while a linked helper says a password field has
        // the focus: not for a Mac without new Henge Tools, nor once the link goes.
        do {
            check(KeyboardIntent.passwordField(live: true, focus: .secure, was: false), "a password field")
            check(!KeyboardIntent.passwordField(live: true, focus: .text, was: true), "a text field after it")
            check(KeyboardIntent.passwordField(live: true, focus: .unknown, was: true), "the helper unsure: as it was")
            check(!KeyboardIntent.passwordField(live: false, focus: .secure, was: true), "no helper of this version: never")
            check(!KeyboardIntent.passwordField(live: false, focus: .unknown, was: true), "the link gone: cleared")
        }

        // The password's Return, from the helper's answer to the look.
        do {
            func goes(_ answer: Int32, sure: Bool = false) -> Bool {
                KeyboardIntent.passwordReturnRefused(look: answer, sure: sure) == nil
            }
            check(goes(2, sure: true), "a password field it is sure of")
            check(!goes(2), "held: a password field only as its app holds secure input (Terminal)")
            check(!goes(1), "a text field")
            check(!goes(0), "nothing to type into")
            check(!goes(-1), "can't tell, or the link closed")
        }
        typingAsFocusMoves()
        slowMac()
        loginWindow()
        passwordPrompt()
        if failures > 0 {
            print("\(failures) failed")
            exit(1)
        }
        print("keyboard intent: all passed")
    }

    /// An emulated Mac is slow to say things (the 9 Nov test: focus lines 1.8-2.7 s after the tap, tap answers given up).
    static func slowMac() {
        let searchBox = KeyboardIntent.Box(x: 0.3, y: 0.1, width: 0.4, height: 0.05)
        // A tap on a search box the helper couldn't answer in time; the focus says so 2.7 s later: shown.
        do {
            var k = linked()
            k.tapLanded(x: 0.5, y: 0.12, now: 10)
            check(k.tapped(.unknown, role: "", afterLift: 1.2)?.show == nil && !k.isShown, "an unanswered tap keeps things as they are")
            let d = k.focused(field(2, .text, frame: searchBox), sincePress: 2.7, now: 12.7)
            check(d?.show == true && k.isShown, "the focus late in the field the tap landed in shows it")
            check(d?.reason.contains("where a tap landed 2700 ms before") == true, "says why: \(d?.reason ?? "")")
        }
        // A tap before the page has loaded (YouTube's search box): the field comes under it 5 s later and gets the focus.
        do {
            var k = linked()
            k.tapLanded(x: 0.5, y: 0.12, now: 10)
            _ = k.tapped(.no, role: "AXWebArea", afterLift: 0.3)
            let d = k.focused(field(2, .text, frame: searchBox), sincePress: 5, now: 15)
            check(d?.show == true, "a field that the tap landed in, focused late, shows it")
        }
        // A field somewhere else gets the focus late after a tap: not the tap's, and too late after it for a press.
        do {
            var k = linked()
            k.tapLanded(x: 0.9, y: 0.9, now: 10)
            _ = k.tapped(.unknown, role: "", afterLift: 1.2)
            let d = k.focused(field(2, .text, frame: searchBox), sincePress: 5, now: 15)
            check(d?.show == nil && !k.isShown, "a field away from the tap, 5 s on: not shown")
        }
        // Too long after the tap (a page that focuses its field by itself much later): not shown.
        do {
            var k = linked()
            k.tapLanded(x: 0.5, y: 0.12, now: 10)
            let d = k.focused(field(2, .text, frame: searchBox), sincePress: 9, now: 19)
            check(d?.show == nil, "9 s after the tap: on its own")
        }
        // ⌘Space (Spotlight, opened from the menu bar's icon too): its field said 2.7 s after the press, with no
        // frame. The first text field since the press: shown.
        do {
            var k = linked()
            let d = k.focused(field(2, .text), sincePress: 2.7, now: 20)
            check(d?.show == true, "the first field 2.7 s after a press shows it: \(d?.reason ?? "")")
            check(d?.reason.contains("the first field since it") == true, "says why: \(d?.reason ?? "")")
        }
        // A second text field after the same press (a page moving its focus about) doesn't count as the press's.
        do {
            var k = linked()
            _ = k.focused(field(2, .text), sincePress: 2, now: 20)
            _ = k.userHid("the row's hide button")
            _ = k.focused(field(1, .none), sincePress: 2.5, now: 20.5)
            let d = k.focused(field(3, .text), sincePress: 3, now: 21)
            check(d?.show == nil && !k.isShown, "a second field after the same press: not shown")
        }
        // Past slowFollow, with no tap: on its own (K5 still holds).
        do {
            var k = linked()
            let d = k.focused(field(2, .text), sincePress: 4.5, now: 30)
            check(d?.show == nil && !k.isShown, "4.5 s after a press, no tap: not shown")
        }
        // A hardware keyboard: a late field the tap landed in still shows nothing of iPadOS's.
        do {
            var k = linked()
            k.hardwareKeyboard = true
            k.tapLanded(x: 0.5, y: 0.12, now: 10)
            let d = k.focused(field(2, .text, frame: searchBox), sincePress: 2, now: 12)
            check(d?.show == nil && !k.isShown && d?.reason.contains("hardware keyboard") == true, "a hardware keyboard: not shown")
        }
        // The helper couldn't say what a tap landed in (timed out, or the owner's wait ran out), and the focus line
        // says no place: a text field 2.7 s on, nothing pressed since, shows it; 5 s on too (past slowFollow).
        do {
            var k = linked()
            k.tapLanded(x: 0.5, y: 0.12, now: 10)
            _ = k.tapped(.unknown, role: "timed-out", afterLift: 0.9)
            let d = k.focused(field(2, .text), sincePress: 5, now: 15)
            check(d?.show == true && d?.reason.contains("couldn't say") == true, "an unsaid tap, then a field 5 s on: \(d?.reason ?? "")")
        }
        // The same, but another press came after the tap: the tap no longer speaks for the focus (and 4.5 s after
        // that press is too late for it).
        do {
            var k = linked()
            k.tapLanded(x: 0.5, y: 0.12, now: 10)
            _ = k.tapped(.unknown, role: "", afterLift: 0.9)
            let d = k.focused(field(2, .text), sincePress: 4.5, now: 15) // pressed at 10.5
            check(d?.show == nil && !k.isShown, "an unsaid tap, another press, then a field: not shown")
        }
        // A tap the helper said was on something else (a link): a field elsewhere, later than slowFollow, doesn't count.
        do {
            var k = linked()
            k.tapLanded(x: 0.5, y: 0.5, now: 10)
            _ = k.tapped(.no, role: "AXLink", afterLift: 0.3)
            let d = k.focused(field(2, .text), sincePress: 5, now: 15)
            check(d?.show == nil, "a tap on a link, a page's field 5 s on: not shown")
        }
        // The owner's wait ran out with no answer at all (tapped never called): the tap still counts as unsaid.
        do {
            var k = linked()
            k.tapLanded(x: 0.5, y: 0.12, now: 10)
            check(k.focused(field(2, .text), sincePress: 2.7, now: 12.7)?.show == true, "no answer, a field 2.7 s on: shown")
        }
        check(searchBox.holds(0.5, 0.12) && !searchBox.holds(0.5, 0.3), "a box holds what is in it")
    }

    /// The login window: no helper there to say its password field has the focus.
    static func loginWindow() {
        do {
            var k = KeyboardIntent()
            let d = k.loginWindow(true)
            check(d?.show == true && k.isShown && k.atLogin, "the login window shows the keyboard")
            check(k.loginWindow(true) == nil, "said again: nothing")
            let off = k.loginWindow(false)
            check(off?.show == false && !k.isShown && !k.atLogin, "gone (someone logged in): it goes")
        }
        // Put away there, a tap on the field (no helper to ask: the owner taps .secure) brings it back.
        do {
            var k = KeyboardIntent()
            _ = k.loginWindow(true)
            _ = k.userHid("iPadOS's hide-keyboard key")
            check(!k.isShown, "put away")
            let d = k.tapped(.secure, role: "the login window", afterLift: 0)
            check(d?.show == true && k.isShown, "a tap at the login window brings it back")
            let off = k.loginWindow(false)
            check(off?.show == false && off?.hid == .focus, "and it goes with the login window")
        }
        // Shown by the person (the toolbar button) before: it stays theirs, also once the login window goes.
        do {
            var k = KeyboardIntent()
            _ = k.userShowed("the toolbar's keyboard button")
            check(k.loginWindow(true) == nil && k.isShown, "already up: kept")
            check(k.loginWindow(false) == nil && k.isShown, "the person's: not taken away")
        }
        // A hardware keyboard, or Show for Text Fields off: not shown by itself.
        do {
            var k = KeyboardIntent()
            k.hardwareKeyboard = true
            let d = k.loginWindow(true)
            check(d?.show == nil && !k.isShown, "a hardware keyboard: not shown at the login window")
            k.hardwareKeyboard = false
            k.auto = false
            check(k.tapped(.secure, role: "the login window", afterLift: 0)?.show == nil, "auto off: a tap doesn't either")
        }
        // Logged in, the helper links and names its fields: the login window's keyboard is gone, the desktop's rules apply.
        do {
            var k = KeyboardIntent()
            _ = k.loginWindow(true)
            _ = k.loginWindow(false)
            _ = k.focused(field(1, .none), sincePress: 99, now: 0)
            check(!k.isShown, "the desktop: nothing up")
        }
    }

    /// The password banner and chip, as one (ios/Henge/PasswordPrompt.swift).
    static func passwordPrompt() {
        // Nothing saved: at a password field the banner comes, once, and stays until answered.
        do {
            var p = PasswordPrompt(saved: false, neverAsk: false)
            check(!p.update(wanted: false) && !p.offering, "nothing in front: no banner")
            check(p.update(wanted: true) && p.offering, "a password field: the banner")
            check(!p.chip(wanted: true), "no chip with nothing saved (it nagged)")
            check(!p.update(wanted: false) && p.offering, "the field goes: the banner stays until answered")
            p.answer(.notNow)
            check(!p.offering && !p.update(wanted: true), "Not Now: not again this run")
            check(!p.chip(wanted: true), "and no chip")
        }
        // Save: the banner goes, and once the password is saved the chip types it at password fields.
        do {
            var p = PasswordPrompt(saved: false, neverAsk: false)
            _ = p.update(wanted: true)
            p.answer(.save)
            p.saved = true
            check(!p.offering && p.chip(wanted: true) && !p.chip(wanted: false), "saved: the chip at a password field only")
        }
        // Save, then the alert cancelled: nothing saved, not offered again this run, no chip.
        do {
            var p = PasswordPrompt(saved: false, neverAsk: false)
            _ = p.update(wanted: true)
            p.answer(.save)
            check(!p.update(wanted: true) && !p.chip(wanted: true), "cancelled: nothing more this run")
        }
        // Don't Ask Again: no banner, no chip, the next run too; saved later with the key button, the chip comes.
        do {
            var p = PasswordPrompt(saved: false, neverAsk: false)
            _ = p.update(wanted: true)
            p.answer(.neverAsk)
            check(p.neverAsk && !p.offering, "don't ask again")
            var next = PasswordPrompt(saved: false, neverAsk: true)
            check(!next.update(wanted: true) && !next.chip(wanted: true), "the next run: no banner, no chip")
            next.saved = true
            check(next.chip(wanted: true), "saved with the key button: the chip")
        }
        // Saved some other way while the banner is up (the key button, held): the banner goes, the chip comes.
        do {
            var p = PasswordPrompt(saved: false, neverAsk: false)
            _ = p.update(wanted: true)
            p.saved = true
            check(!p.update(wanted: true) && !p.offering && p.chip(wanted: true), "saved meanwhile: chip, no banner")
        }
        // A password saved: never the banner.
        do {
            var p = PasswordPrompt(saved: true, neverAsk: false)
            check(!p.update(wanted: true) && p.chip(wanted: true), "saved: chip, no banner")
            p.forgotten()
            check(!p.chip(wanted: true) && !p.update(wanted: true), "forgotten: no chip, not offered again this run")
        }
    }
}
