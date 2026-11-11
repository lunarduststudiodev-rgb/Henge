import SwiftUI
import UIKit

/// The one place the iPad's keyboard is shown or hidden. It hears the helper's focus and tap answers the moment they
/// land (henge_share_set_listener), puts them to KeyboardIntent with when the person last pressed something, logs a
/// line for each decision, and applies it to the keyboard catcher itself. SwiftUI only reads `isShown` (and the
/// focus, for the password button) to lay out the Mac. All of it on the main queue: UIKit's touches and presses,
/// GameController's handlers and the link's hop (henge_share_set_listener). What the compiler can hold to that (the
/// password's look, and what reads the helper's lines) is marked as the main actor's; the touch and pointer callers
/// (DirectTouch, TouchScreen, Hardware) aren't marked yet.
final class KeyboardOwner: ObservableObject {
    static let shared = KeyboardOwner()

    /// The keyboard is wanted: the Mac makes room for it.
    @Published private(set) var isShown = false
    /// What has the Mac's focus: -1 unknown (no helper of this version), 0 nothing to type into, 1 a text field,
    /// 2 a password field. Unknown answers from a linked helper leave it as it was.
    @Published private(set) var focus: Int32 = -1
    /// Where the focused field is, as fractions of the Mac's screen, when the helper says.
    @Published private(set) var fieldFrame: CGRect?

    /// The catcher on the screen now (KeyboardCatcher sets it).
    weak var catcher: KeyboardCatcher.Holder? {
        didSet { if let catcher, catcher !== oldValue { catcher.typing = isShown; applyTraits() } }
    }

    /// A sheet, a pause, the app in the background: no keyboard of its own accord.
    var blocked = false {
        didSet { intent.blocked = blocked }
    }

    private var intent = KeyboardIntent()
    private var last = HengeField()
    /// The last line from a helper of this version, for what becomes of the typing as the focus moves.
    private var liveField: KeyboardIntent.Field?
    private var heardOnce = false
    /// Taps the helper is to say the landing of: their ids and when the finger lifted (systemUptime).
    private var taps: [(id: UInt32, lifted: TimeInterval)] = []
    /// The password's Return, waiting for the helper to say what has the focus now (passwordReturnGoes): the look's
    /// id, and who waits.
    @MainActor private var look: (id: UInt32, answer: CheckedContinuation<Bool, Never>)?
    /// The helper takes up to 0.9 s to say what a tap landed in on a slow Mac (henge-agent's tapPatience); then the link.
    private static let tapWait: TimeInterval = 1.2

    private init() {
        // The link's thread: only a hop to the main queue, where everything here happens.
        henge_share_set_listener { DispatchQueue.main.async { KeyboardOwner.shared.heard() } }
    }

    private var now: TimeInterval { ProcessInfo.processInfo.systemUptime }

    private func refresh() {
        intent.auto = UserDefaults.standard.object(forKey: "henge.keyboard.auto") as? Bool ?? true
        intent.hardwareKeyboard = HardwareKeyboard.shared.connected
    }

    /// A session's screen is up: where things stand now (the helper may have spoken before anyone listened).
    @MainActor func start() {
        heardOnce = false
        heard()
    }

    /// The session ended: the next starts with no keyboard and nothing put away.
    @MainActor func reset() {
        intent = KeyboardIntent()
        intent.blocked = blocked
        taps = []
        loginCertain = false
        answerLook(false, "the session ended")
        heardOnce = false
        last = HengeField()
        liveField = nil
        if isShown { isShown = false }
        catcher?.typing = false
        // The next Mac's focus isn't known yet: the password key mustn't press Return after typing there.
        if focus != -1 { focus = -1 }
        if fieldFrame != nil { fieldFrame = nil }
        if RowPlacement.shared.passwordField { RowPlacement.shared.passwordField = false }
    }

    // MARK: What the person does

    /// The toolbar's keyboard button.
    func toggle() {
        refresh()
        apply(isShown ? intent.userHid("the toolbar's keyboard button") : intent.userShowed("the toolbar's keyboard button"))
    }

    /// The row's hide button, or iPadOS's own hide-keyboard key.
    func userHid(_ how: String) {
        apply(intent.userHid(how))
    }

    /// Something took the screen from typing: the keyboard goes (not held away from the field).
    func close(_ why: String) {
        apply(intent.close(why))
    }

    /// A tap went to the Mac: id is the hit or tap the helper was asked about (0: no helper of this version to ask),
    /// lifted when the finger came up (UITouch's timestamp, systemUptime), at where it landed (0...1 across the Mac's
    /// screen; nil: where the Mac has its pointer, which the finger moved).
    func tapped(_ id: UInt32, lifted: TimeInterval, at: CGPoint? = nil) {
        if let spot = at ?? pointerSpot() { intent.tapLanded(x: Double(spot.x), y: Double(spot.y), now: now) }
        guard id != 0 else {
            // The login window: no helper runs there to ask, and its password field has the focus.
            if intent.atLogin { refresh(); apply(intent.tapped(.secure, role: "the login window", afterLift: now - lifted)) }
            return
        }
        taps.append((id, lifted))
        if taps.count > 4 { taps.removeFirst(taps.count - 4) }
        checkTaps()
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.tapWait) { [weak self] in self?.checkTaps() }
    }

    /// A left click from a pointer went to the Mac (the trackpad's tap, a mouse, an iPad trackpad's click): what is
    /// under the Mac's pointer is asked of the helper as a tap, so a click in the field the keyboard was put away
    /// from brings it back, as a finger's tap does. spot: where the click went, 0...1 across the Mac's screen, when
    /// the caller put it there; otherwise where the Mac has its pointer.
    func clicked(at spot: CGPoint? = nil) {
        guard let at = spot ?? pointerSpot() else { return }
        tapped(henge_share_tap(Double(at.x), Double(at.y)), lifted: now, at: at)
    }

    /// The Mac's pointer, 0...1 across its screen.
    private func pointerSpot() -> CGPoint? {
        var x: Int32 = 0, y: Int32 = 0, w: Int32 = 0, h: Int32 = 0
        guard henge_vm_pointer(&x, &y) == 0, henge_vm_screen_size(&w, &h) == 0,
              let at = KeyboardIntent.clickSpot(pointerX: x, pointerY: y, screenWidth: w, screenHeight: h) else { return nil }
        return CGPoint(x: at.x, y: at.y)
    }

    /// The Mac's login window is up (or went), as the session tells from Henge Tools' system part. Its password field
    /// has the focus there, which no helper says: the user's helper runs only once someone has logged in.
    func loginWindow(_ on: Bool) {
        refresh()
        apply(intent.loginWindow(on))
    }

    /// The login window is told by Henge Tools' system part, not guessed (SessionView's loginCertain). Guessed (the
    /// helper unlinked a while), it may be a logged-in Mac whose helper went: Return would go into whatever is in front.
    var loginCertain = false

    /// Whether the system part says the Mac is at its login window now: for the password's Return with no helper to
    /// ask, read as the password is asked for (the Log In chip goes, and says the login window went, as it types).
    var atLoginCertainly: Bool { intent.atLogin && loginCertain }

    // MARK: What the helper says

    @MainActor private func heard() {
        checkTaps()
        checkLook()
        var f = HengeField()
        let live = henge_share_field(&f) != 0
        let changed = !heardOnce || f.serial != last.serial || f.generation != last.generation || f.id != last.id || f.state != last.state
        if heardOnce && !changed { return }
        heardOnce = true
        let was = last
        last = f
        // For the password button: unknown while no helper of this version is linked; a linked helper's "can't tell"
        // leaves it as it was.
        let shownFocus: Int32 = !live ? -1 : f.state == -1 ? focus : f.state
        if shownFocus != focus { focus = shownFocus }
        if live && f.state != -1 {
            let frame = f.width > 0 ? CGRect(x: CGFloat(f.x), y: CGFloat(f.y), width: CGFloat(f.width), height: CGFloat(f.height)) : nil
            if frame != fieldFrame { fieldFrame = frame }
        } else if !live, fieldFrame != nil {
            fieldFrame = nil
        }
        // No helper of this version (one linked that is older, or none): nothing says a password field has the
        // focus, so the password key types the password without Return.
        let password = KeyboardIntent.passwordField(live: live, focus: KeyboardIntent.Focus(rawValue: f.state) ?? .unknown,
                                                    was: RowPlacement.shared.passwordField)
        if RowPlacement.shared.passwordField != password { RowPlacement.shared.passwordField = password }
        if live, f.state != -1, f.state != was.state || f.id != was.id {
            HengeLog.log("input", "Mac's focus: \(KeyboardIntent.Focus(rawValue: f.state)?.said ?? "?") (field \(f.id))")
        }
        refresh()
        let box = f.width > 0 ? KeyboardIntent.Box(x: Double(f.x), y: Double(f.y), width: Double(f.width), height: Double(f.height)) : nil
        let field = KeyboardIntent.Field(serial: f.serial, id: f.id, generation: f.generation,
                                         focus: KeyboardIntent.Focus(rawValue: f.state) ?? .unknown, kind: f.kind, frame: box)
        if live {
            let typing = KeyboardIntent.typing(from: liveField, to: field)
            typingMoved(typing)
            // "Can't tell" about the same field keeps what was known of it.
            if field.focus != .unknown || field.id != liveField?.id || field.generation != liveField?.generation { liveField = field }
        }
        apply(intent.focused(field, sincePress: henge_vm_press_idle_s(), now: now))
        if live && f.state != -1 { applyTraits() }
        if intent.waitingForNone {
            DispatchQueue.main.asyncAfter(deadline: .now() + KeyboardIntent.noneHold) { [weak self] in
                guard let self else { return }
                self.apply(self.intent.tick(now: self.now))
            }
        }
    }

    /// What Henge was asked to type before the person's last press (a tap, a click, a key: what moved the focus) was
    /// for where the focus was, and is dropped as it moves: typed on, it would land in whatever has it now. Asked for
    /// since, it goes on (KeyboardIntent.typing). Dictation hears it too: leaving text ends it.
    @MainActor private func typingMoved(_ typing: KeyboardIntent.Typing) {
        guard typing != .stays else { return }
        let press = henge_vm_last_press()
        let dropped = TypingQueue.shared.drop([.dictation, .password], askedBefore: press)
        if dropped > 0 { HengeLog.log("input", "the focus moved after a press: \(dropped) piece(s) still to type for where it was dropped") }
        switch typing {
        case .stays: return
        case .moved, .anotherField: Dictation.shared.focusMoved(at: press)
        case .leftText:
            if Dictation.shared.isOn { HengeLog.log("input", "focus left text: dictation ends") }
            Dictation.shared.cancel()
        }
    }

    /// Whether the password key's Return goes, now that the password is typed (MacPassword; TypingQueue has already
    /// refused it for a press since). With a helper of this version linked, it is asked what has the focus once every
    /// key and click before has reached the Mac's keyboard and pointer (henge_share_look), and the Return goes only if
    /// it says a password field it is sure of (the field says so, or macOS's own prompt has the keys:
    /// KeyboardIntent.passwordReturnRefused). A field taken only from its app holding secure input (Terminal: its
    /// Secure Keyboard Entry looks the same as sudo asking), a helper before 34 (which can't be asked), no answer
    /// within tapWait, the link closing, or the typing cut (the task cancelled, TypingQueue.drop): the password
    /// without Return. With no helper linked, only the login window has the Return (no helper runs there, and its
    /// password field has the focus): atLogin, as the system part said it as the password was asked for. A helper
    /// relaunching after an update, or one that went, can't say what is in front.
    @MainActor func passwordReturnGoes(atLogin: Bool) async -> Bool {
        var f = HengeField()
        guard henge_share_field(&f) != 0 else {
            if atLogin { return true }
            HengeLog.log("input", "password typed without Return: no helper is linked to say what has the focus")
            return false
        }
        guard !Task.isCancelled else { return false }
        let id = henge_share_look()
        guard id != 0 else {
            HengeLog.log("input", "password typed without Return: the Mac's helper is older than 34 and can't be asked what has the focus")
            return false
        }
        answerLook(false, "another was asked") // one at a time (TypingQueue types one piece at a time)
        return await withTaskCancellationHandler {
            await withCheckedContinuation { (go: CheckedContinuation<Bool, Never>) in
                look = (id, go)
                checkLook()
                // The wait ends this look, whatever the clock says then.
                DispatchQueue.main.asyncAfter(deadline: .now() + Self.tapWait) { [weak self] in
                    self?.endLook(id, "the helper didn't say what has the focus within \(Int(Self.tapWait * 1000)) ms")
                }
            }
        } onCancel: {
            // Cancelled before the look was stored, this still runs after it is: the main actor is busy until then.
            Task { @MainActor in KeyboardOwner.shared.endLook(id, "the typing was cut") }
        }
    }

    /// The helper's answer to the look, once it is in (or the link closed).
    @MainActor private func checkLook() {
        guard let look else { return }
        var f = HengeField()
        var sure: Int32 = 0
        let answer = henge_share_field(&f) != 0 ? henge_share_look_answer(look.id, &sure) : -1
        guard answer != -2 else { return } // not yet: the answer, the link closing or the wait ends it
        let why = KeyboardIntent.passwordReturnRefused(look: answer, sure: sure != 0)
        answerLook(why == nil, why ?? "")
    }

    /// Ends look id, if it is still the one waiting, without Return.
    @MainActor private func endLook(_ id: UInt32, _ why: String) {
        if look?.id == id { answerLook(false, why) }
    }

    @MainActor private func answerLook(_ goes: Bool, _ why: String) {
        guard let look else { return }
        self.look = nil
        if !goes { HengeLog.log("input", "password typed without Return: \(why)") }
        look.answer.resume(returning: goes)
    }

    private func checkTaps() {
        guard !taps.isEmpty else { return }
        let t = now
        var waiting: [(id: UInt32, lifted: TimeInterval)] = []
        for tap in taps {
            var x: Float = 0, y: Float = 0, w: Float = 0, h: Float = 0
            var role = [CChar](repeating: 0, count: 48)
            let answer = henge_share_tap_answer(tap.id, &x, &y, &w, &h, &role, Int32(role.count))
            if answer == -2 { // not yet
                if t - tap.lifted < Self.tapWait { waiting.append(tap) }
                else { HengeLog.log("input", "keyboard: kept (helper didn't say what a tap landed in within \(Int(Self.tapWait * 1000)) ms)") }
                continue
            }
            refresh()
            apply(intent.tapped(KeyboardIntent.Tap(rawValue: answer) ?? .unknown, role: String(cString: role), afterLift: t - tap.lifted))
        }
        taps = waiting
    }

    // MARK: Applying it

    private func apply(_ decision: KeyboardIntent.Decision?) {
        guard let decision else { return }
        HengeLog.log("input", "keyboard: \(decision.reason)")
        if decision.fresh { NotificationCenter.default.post(name: KeyboardCatcher.focusMoved, object: nil) }
        let show = intent.isShown
        guard show != isShown else { return }
        // Dictation's key went with the keyboard: what becomes of it depends on why (KeyboardIntent.Hid).
        // On the main queue, as all of KeyboardOwner is: UIKit's touches and presses, GameController's handlers.
        if !show, let hid = decision.hid { MainActor.assumeIsolated { Dictation.shared.keyboardWent(hid) } }
        isShown = show
        catcher?.typing = show
        catcher?.takeKeyboard()
    }

    /// The keyboard suits the field (a password's, code's, an address bar's) as the focus moves to it, not a moment
    /// later; not while iOS is composing a word, which a new keyboard would lose (it waits for the word).
    private func applyTraits() {
        guard let catcher, last.state != -1 else { return }
        catcher.setSecure(last.state == 2)
        catcher.typer.setField(KeyboardCatcher.CatcherView.Field(rawValue: last.kind) ?? .prose)
    }
}
