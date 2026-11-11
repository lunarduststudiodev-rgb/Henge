import Combine
import GameController
import SwiftUI
import UIKit

/// Types into the Mac. Two invisible views take turns holding the keyboard: one that asks for the
/// on-screen keyboard, with a row of the Mac's own keys above it, and one that doesn't, so that a
/// hardware keyboard works whether or not that is open.
/// Whether it holds the on-screen keyboard is KeyboardOwner's to say.
struct KeyboardCatcher: UIViewRepresentable {
    /// How much of the window's bottom the keyboard covers while typing, in points: all of a docked
    /// keyboard (its row of keys included). 0 with no keys docked (a floating keyboard, a hardware one): the
    /// row is then a glass bar over the Mac, which keeps its size.
    @Binding var cover: CGFloat
    /// iOS's autocorrect, suggestions and the rest (Henge's Autocorrect switch).
    var autocorrect = true

    /// Posted when the Mac's focus moves: what was typed is somewhere else now, and a new field starts afresh.
    static let focusMoved = Notification.Name("henge.typing.focusMoved")

    func makeUIView(context: Context) -> Holder { Holder() }

    func updateUIView(_ view: Holder, context: Context) {
        view.typer.onHide = { how in KeyboardOwner.shared.userHid(how) }
        view.onCover = { new, animated in
            guard cover != new else { return }
            // Moved with the keyboard's own curve, starting as it starts, so the two move as one.
            if animated { withAnimation(.keyboard) { cover = new } } else { cover = new }
        }
        view.setHelps(autocorrect)
        KeyboardOwner.shared.catcher = view
        DispatchQueue.main.async { view.takeKeyboard() }
    }

    final class Holder: UIView {
        let listener = KeyListener()
        let typer = CatcherView()
        var typing = false {
            didSet {
                guard typing != oldValue else { return }
                // A hardware keyboard puts no keys on the screen: iPadOS gets no row, and Henge draws it (the frames
                // that follow say for sure, and give it back if the on-screen keyboard comes up after all).
                if typing {
                    let hardware = HardwareKeyboard.shared.connected
                    typer.givesRow = !hardware
                    rows.typingStarts(hardwareKeyboard: hardware)
                } else {
                    // The row (or the small control) that showed a held ⌃, ⌥ or ⌘ is gone: so is the key.
                    StickyKeys.shared.releaseAll()
                }
                place()
            }
        }
        var onCover: (CGFloat, Bool) -> Void = { _, _ in }
        private var docked = false // the keyboard's last frame had keys along the bottom
        private var keyboardCover: CGFloat = 0 // what that frame covered, which may leave out the row on top of it
        private var floating: CGRect? // a floating keyboard's last frame, in the window
        private var loggedFloating = CGRect.null
        private var systemBar: CGRect? // iPadOS's bar for a hardware keyboard (or its collapsed button), in the window
        private var rows = RowChoice() // the small control or the whole row
        private var loggedKind = ""
        private var observers: [NSObjectProtocol] = []

        override init(frame: CGRect) {
            super.init(frame: frame)
            addSubview(listener)
            addSubview(typer)
            let center = NotificationCenter.default
            observers = [
                center.addObserver(forName: UIResponder.keyboardWillChangeFrameNotification, object: nil, queue: .main) { [weak self] note in
                    self?.keyboardMoving(note)
                },
                center.addObserver(forName: UIResponder.keyboardDidChangeFrameNotification, object: nil, queue: .main) { [weak self] _ in
                    self?.keyboardSettled()
                },
                center.addObserver(forName: UIResponder.keyboardDidHideNotification, object: nil, queue: .main) { [weak self] _ in
                    self?.keyboardHid()
                },
                center.addObserver(forName: KeyboardCatcher.focusMoved, object: nil, queue: .main) { [weak self] _ in
                    self?.typer.forget(TypedText.fresh)
                },
                // Connected or taken away while typing, with no frame from iPadOS to say so (dictating with the
                // letters away, nothing on the screen changes): the small control or the whole row, again.
                center.addObserver(forName: HardwareKeyboard.changed, object: nil, queue: .main) { [weak self] _ in
                    self?.place()
                },
            ]
        }

        required init?(coder: NSCoder) { fatalError("made in code") }

        deinit {
            observers.forEach(NotificationCenter.default.removeObserver)
        }

        /// Where the keyboard is going is known as it starts to move, so the Mac makes room alongside it,
        /// not after it has arrived. What it is comes from KeyboardLayout: keys docked along the bottom, a
        /// floating keyboard, or no keys at all (a hardware keyboard's bar from iPadOS).
        private func keyboardMoving(_ note: Notification) {
            guard let end = note.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? CGRect, let window,
                  let screen = window.windowScene?.screen else { return }
            let animated = (note.userInfo?[UIResponder.keyboardAnimationDurationUserInfoKey] as? Double ?? 0) > 0
            let frame = end.isNull || end.isEmpty ? CGRect.null : window.convert(end, from: screen.coordinateSpace)
            // Henge's row is in the frame only when it is the keyboard's (not a sheet's or an alert's field).
            let row = typer.isFirstResponder && typer.givesRow ? (typer.inputAccessoryView?.bounds.height ?? 0) : 0
            let kind = KeyboardLayout.read(frame, window: window.bounds, row: row)
            rows.saw(kind, lettersAway: Dictation.shared.lettersAway)
            switch kind {
            case .docked(var cover):
                docked = true
                floating = nil
                systemBar = nil
                // iPadOS sometimes gives a docked keyboard's frame without the row sitting on top of it.
                if cover > 0, typing, let rowAt = rowFrame(), abs(rowAt.maxY - end.minY) < 1 { cover += rowAt.height }
                keyboardCover = cover
                typer.givesRow = true
                logKind("keys docked along the bottom")
                report(cover, "keyboard", animated: animated)
            case .floating(let f):
                docked = false
                floating = f
                systemBar = nil
                typer.givesRow = true
                logKind("a floating keyboard")
                report(0, "floating keyboard", animated: animated)
            case .bar(let f):
                // No keys on the screen: a hardware keyboard (or iPadOS's own keyboard put away). iPadOS's bar, if
                // any, stays as iPadOS lays it out; Henge's row is its own glass bar over the Mac, above that bar,
                // and the Mac keeps its size.
                docked = false
                floating = nil
                let bar = CGRect(x: f.minX, y: f.minY + row, width: f.width, height: f.height - row)
                systemBar = bar.height > 1 ? bar : nil
                typer.givesRow = false
                logKind("no keys on the screen (iPadOS's bar at y \(Int(f.minY + row))-\(Int(f.maxY)), x \(Int(f.minX))-\(Int(f.maxX)))")
                report(0, "no keys on the screen", animated: animated)
            case .none:
                docked = false
                floating = nil
                systemBar = nil
                report(0, "keyboard gone", animated: animated)
            }
            place()
        }

        /// What the keyboard turned out to be, once each time it changes (the row's own frame says the rest).
        private func logKind(_ kind: String) {
            guard kind != loggedKind else { return }
            loggedKind = kind
            HengeLog.log("input", "keyboard: \(kind)")
        }

        /// Once the keyboard has moved, the row is measured too. Docked, iPadOS sometimes gives the
        /// keyboard's frame without the row on top of it, which then covered the bottom of the Mac.
        private func keyboardSettled() {
            (typer.inputAccessoryView as? RowBar)?.remeasure()
            if docked {
                let row = rowFrame().flatMap(rowTop) ?? 0
                report(max(keyboardCover, row), row > keyboardCover ? "keyboard and row" : "keyboard", animated: true)
            } else {
                report(0, floating != nil ? "floating keyboard" : "no keys on the screen", animated: true)
                if typing, let floating, floating != loggedFloating, let window, let screen = window.windowScene?.screen {
                    loggedFloating = floating
                    let row = rowFrame().map { " (row y \(Int($0.minY))-\(Int($0.maxY)))" } ?? ""
                    let onScreen = window.convert(floating, to: screen.coordinateSpace)
                    HengeLog.log("input", "keyboard floats at y \(Int(onScreen.minY))-\(Int(onScreen.maxY)), \(Int(screen.bounds.maxY - onScreen.maxY)) pt above the screen's bottom\(row)")
                }
            }
            place()
        }

        /// iPadOS's own hide-keyboard key closes the keyboard without asking the app, which then went on
        /// keeping the Mac small, with room for a keyboard that was gone. Stop typing as the row's button does.
        /// iPadOS also says the keyboard hid when it starts to float, when the keyboard is rebuilt for another kind
        /// of field (reloadInputViews), when a hardware keyboard's bar is collapsed, and a sheet may take it for a
        /// while (it is handed back when the sheet closes): none of those is the person putting it away.
        private func keyboardHid() {
            guard typing, let window, window.rootViewController?.presentedViewController == nil else { return }
            if ProcessInfo.processInfo.systemUptime - reloadedAt < 0.6 { return keyboardSettled() } // rebuilt
            if typer.isFirstResponder, !typer.givesRow { return keyboardSettled() } // no keys to put away: Henge's row stays
            if !docked, typer.isFirstResponder, floating != nil { return keyboardSettled() } // floating
            if let row = rowFrame(), let screen = window.windowScene?.screen,
               window.convert(row, from: screen.coordinateSpace).intersects(window.bounds) { return keyboardSettled() }
            typer.onHide("iPadOS's hide-keyboard key")
        }

        /// Tells the row (and the session, which draws it when iPadOS has none) where it goes.
        private func place() {
            let placement = RowPlacement.shared
            let alone = !typer.givesRow
            placement.set(alongBottom: !docked)
            placement.set(rowAlone: alone)
            placement.set(keyboardFloats: typing && !docked)
            placement.set(standalone: typing && alone)
            // A hardware keyboard has every key the row has but two: the row is then the small control. Not while
            // dictation has put away on-screen letters the person was typing with: that keeps the whole row.
            placement.set(compact: rows.compact(typing: typing, rowAlone: alone, hardwareKeyboard: HardwareKeyboard.shared.connected))
            placement.set(systemBar: alone ? systemBar : nil)
        }

        private var lastCover: CGFloat = -1
        private func report(_ cover: CGFloat, _ from: String, animated: Bool = false) {
            guard cover != lastCover else { return }
            lastCover = cover
            HengeLog.log("input", "keyboard covers \(Int(cover)) pt at the bottom (\(from))")
            onCover(cover, animated)
        }

        /// How far up from the window's bottom the row reaches, if it spans the window's width (over a docked
        /// keyboard). 0 once it has gone below the window with the keyboard.
        private func rowTop(_ frame: CGRect) -> CGFloat? {
            guard let window, let screen = window.windowScene?.screen, !frame.isEmpty else { return nil }
            let f = window.convert(frame, from: screen.coordinateSpace), b = window.bounds
            guard f.minX <= b.minX + 1, f.maxX >= b.maxX - 1 else { return nil }
            return max(b.maxY - f.minY, 0)
        }

        /// When the keyboard was last rebuilt for new traits (systemUptime): iPadOS may say it hid as it is.
        private var reloadedAt: TimeInterval = -1
        func reloading() { reloadedAt = ProcessInfo.processInfo.systemUptime }

        /// A password field has the Mac's focus: the on-screen keyboard shows no letters or suggestions.
        func setSecure(_ on: Bool) {
            guard typer.isSecureTextEntry != on else { return }
            typer.isSecureTextEntry = on
            retrait()
        }

        /// iOS's help with typing, Henge's Autocorrect switch.
        func setHelps(_ helps: Bool) {
            guard typer.helps != helps else { return }
            typer.helps = helps
            retrait()
        }

        private func retrait() {
            _ = typer.updateTraits()
            typer.forget(TypedText.fresh)
            typer.reload()
        }

        /// The row of the Mac's keys, where it is on the screen now, if it is showing.
        private func rowFrame() -> CGRect? {
            guard typer.isFirstResponder, let row = typer.inputAccessoryView, let screen = window?.windowScene?.screen,
                  row.window != nil, !row.isHidden else { return nil }
            return row.convert(row.bounds, to: screen.coordinateSpace)
        }

        override func didMoveToWindow() {
            super.didMoveToWindow()
            takeKeyboard()
        }

        /// Called often, which also takes the keyboard back after a sheet has had it.
        func takeKeyboard() {
            guard let window, window.rootViewController?.presentedViewController == nil else { return }
            let holder: UIView = typing ? typer : listener
            if !holder.isFirstResponder { holder.becomeFirstResponder() }
        }
    }

    /// Whichever view holds the keyboard hears a hardware keyboard's keys here.
    class KeyListener: UIView {
        override var canBecomeFirstResponder: Bool { true }
        /// Three fingers belong to the session (they show its controls), not to iPadOS's undo/copy/paste bar.
        override var editingInteractionConfiguration: UIEditingInteractionConfiguration { .none }

        override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
            let rest = HardwareKeys.send(presses, down: true)
            if !rest.isEmpty { super.pressesBegan(rest, with: event) }
        }

        override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
            let rest = HardwareKeys.send(presses, down: false)
            if !rest.isEmpty { super.pressesEnded(rest, with: event) }
        }

        override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
            let rest = HardwareKeys.send(presses, down: false)
            if !rest.isEmpty { super.pressesCancelled(rest, with: event) }
        }
    }

    /// The view the on-screen keyboard types into: a stand-in text box holding what has been typed into the Mac
    /// (TypedText), so iOS can autocorrect it, suggest words, capitalize, put in a double space's period, swipe-type
    /// and use text replacements, all of which reach the Mac as keys. Holding the space bar moves the Mac's caret
    /// with arrow keys. Whenever the Mac's text may have changed some other way (a click, another key, the focus
    /// moving), the stand-in starts again with nothing iOS may change.
    final class CatcherView: KeyListener, UITextInput {
        /// The person put the keyboard away, and how.
        var onHide: (String) -> Void = { _ in }
        private lazy var row: RowBar = RowBar(ModifierRow(hide: { [weak self] in self?.onHide("the row's hide button") }))

        /// iPadOS's own place for an app's keys: on top of a docked keyboard, and along the bottom of the screen
        /// while it floats, moved and animated with the keyboard by iPadOS itself.
        /// Not with no keys on the screen (a hardware keyboard): iPadOS then sets the row into its own shortcuts and
        /// predictions bar, screen-wide and starting partway across, so the row lost its glass and half of it (and of
        /// iPadOS's bar) went off the right edge. The session draws the row itself then (RowPlacement.standalone).
        override var inputAccessoryView: UIView? { givesRow ? row : nil }
        var givesRow = true {
            didSet { if givesRow != oldValue { reload() } }
        }

        /// While dictating the letters give way to the row alone (Dictation.lettersAway), as iPadOS's own dictation
        /// does. Read here, so a rebuild for the Mac's next field (reload, on a focus change) keeps them away.
        override var inputView: UIView? { Dictation.shared.lettersAway ? noLetters : nil }
        private let noLetters = UIView(frame: .zero)
        private var lettersWatch: AnyCancellable?

        override func didMoveToWindow() {
            super.didMoveToWindow()
            guard window != nil, lettersWatch == nil else { return }
            lettersWatch = Dictation.shared.$lettersAway.dropFirst().removeDuplicates().sink { [weak self] _ in
                // Sent before the value changes: rebuilt once it has.
                DispatchQueue.main.async { self?.reload() }
            }
        }

        // MARK: Traits

        var isSecureTextEntry = false
        /// iOS's help with typing (autocorrect and the rest, as Settings › General › Keyboard has them). Henge's
        /// Autocorrect switch turns it all off; a password field never has it.
        var helps = true
        var autocorrectionType: UITextAutocorrectionType = .default
        var autocapitalizationType: UITextAutocapitalizationType = .sentences
        var spellCheckingType: UITextSpellCheckingType = .default
        // Curly quotes and long dashes aren't on the Mac's keyboard; iOS 17's grey inline prediction would be
        // invisible here, so a space would take a word nobody saw.
        var smartQuotesType: UITextSmartQuotesType = .no
        var smartDashesType: UITextSmartDashesType = .no
        var smartInsertDeleteType: UITextSmartInsertDeleteType = .no
        var inlinePredictionType: UITextInlinePredictionType = .no
        var keyboardType: UIKeyboardType = .default

        /// What kind of field has the Mac's focus, as the helper tells it (HV_FIELD_* in share.h): words get iOS's
        /// help; code, a terminal, an address bar, an email address or user name don't, as they wouldn't on iOS.
        enum Field: Int32 { case prose = 0, code = 1, address = 2, plain = 3 }
        private(set) var field = Field.prose

        /// Sets the traits from `helps`, `isSecureTextEntry` and `field`; true if any changed.
        func updateTraits() -> Bool {
            let on = helps && !isSecureTextEntry
            let words = on && field == .prose
            let was = (autocorrectionType, autocapitalizationType, spellCheckingType, keyboardType)
            autocorrectionType = words ? .default : .no
            autocapitalizationType = words ? .sentences : .none
            spellCheckingType = words ? .default : .no
            keyboardType = field == .address ? .webSearch : .default
            return was != (autocorrectionType, autocapitalizationType, spellCheckingType, keyboardType)
        }

        /// The kind of field the Mac's focus is on, when a word being composed held it back.
        private var fieldWaiting: Field?

        /// The Mac's focus moved to another kind of field (KeyboardOwner, with the focus line): the keyboard changes to
        /// suit it; not while iOS is composing a word, which a new keyboard would lose, but once the word is done.
        func setField(_ new: Field) {
            guard doc.marked == nil else {
                fieldWaiting = new == field ? nil : new
                return
            }
            fieldWaiting = nil
            guard new != field else { return }
            HengeLog.log("input", "keyboard's field is now \(new)")
            field = new
            if updateTraits() { reload() }
        }

        /// A word was composed: the kind of field that waited for it now.
        private func fieldNoLongerWaits() {
            if let waiting = fieldWaiting, doc.marked == nil { setField(waiting) }
        }

        /// Rebuilds the keyboard for new traits; iPadOS may say it hid meanwhile, which isn't the person putting it away.
        func reload() {
            guard isFirstResponder else { return }
            (superview as? Holder)?.reloading()
            reloadInputViews()
        }

        // MARK: The stand-in text

        private var doc = TypedText()
        /// The Mac's key and click counts after Henge's own last keys: anything more came from elsewhere.
        private var stamp: (UInt32, UInt32) = (0, 0)
        private var watch: Timer?

        private func restamp() { stamp = (henge_vm_keys_pressed(), henge_vm_clicks()) }
        private var stampMoved: Bool {
            let now = (henge_vm_keys_pressed(), henge_vm_clicks())
            return now != stamp
        }

        /// Starts again: `TypedText.fresh` at a field's start, `midText` somewhere in text Henge can't see. The Mac's
        /// own text takes its place once the helper has said what it is (lookAtMac).
        func forget(_ seed: String = TypedText.midText, why: String? = nil) {
            if let why, !doc.bare { HengeLog.log("input", "keyboard lost track of the Mac's text: \(why)") }
            changedAt = Date()
            looked = 0 // what the helper last said is looked at again: the caret may not have moved
            if doc.bare, doc.text == seed { return restamp() } // nothing typed yet
            set(TypedText(seed: seed))
        }

        private func set(_ new: TypedText) {
            doc = new
            restamp()
            // Told after the keyboard's own call has returned, so it reads the text afresh.
            DispatchQueue.main.async { [weak self] in
                guard let self, let d = self.inputDelegate else { return }
                d.selectionWillChange(self); d.textWillChange(self)
                d.textDidChange(self); d.selectionDidChange(self)
            }
        }

        // MARK: The Mac's own text

        /// When the Mac's text last changed by something else, or by these keys.
        private var changedAt = Date.distantPast, sentAt = Date.distantPast
        /// The helper's last word on the text around the caret, when it came, and the last one looked at.
        private var heard: UInt32 = 0, heardAt = Date.distantPast, looked: UInt32 = 0

        /// What the helper says is around the Mac's caret replaces the stand-in, so iOS suggests and corrects with the
        /// words already there; and if it disagrees with what was typed here (the Mac corrected a word itself, say), it
        /// wins. Only once things have settled: the helper looks four times a second, and what it says may be from
        /// before the last keys or click.
        private func lookAtMac() {
            var before = [CChar](repeating: 0, count: 1024), after = [CChar](repeating: 0, count: 1024)
            var selected: Int32 = 0, known: Int32 = 0, kind: Int32 = 0
            let seq = henge_share_context(&before, 1024, &after, 1024, &selected, &known, &kind)
            let now = Date()
            if seq != heard { heard = seq; heardAt = now }
            guard seq != 0, seq != looked, now.timeIntervalSince(heardAt) >= 0.3,
                  now.timeIntervalSince(changedAt) >= 0.6, now.timeIntervalSince(sentAt) >= 0.6,
                  cursor == nil, doc.marked == nil, !isSecureTextEntry else { return }
            looked = seq
            guard known != 0 else { return }
            func text(_ b64: [CChar]) -> String {
                Data(base64Encoded: String(cString: b64)).flatMap { String(data: $0, encoding: .utf8) } ?? ""
            }
            let b = text(before), a = text(after), n = Int(selected)
            // What the Mac has selected (a word picked out to correct, an address bar's completion) is selected here
            // too, so iOS offers its corrections for it; unless it runs past the line or holds what can't be typed.
            guard TypedText.follows(before: b, after: a, selected: n) else {
                return doc.bare ? () : forget(why: "the Mac selected more than one line of plain text")
            }
            if !doc.bare, doc.agrees(before: b, after: a, selected: n) { return }
            let real = TypedText(before: b, after: a, selected: n)
            let picked = real.macSelection.map { ", \($0.count) of them selected" } ?? ""
            HengeLog.log("input", doc.bare ? "keyboard knows the Mac's text: \(real.macCaret) characters before the caret, \(real.count - real.macCaret) after\(picked)"
                                          : "keyboard's text didn't match the Mac's: the Mac's taken\(picked)")
            set(real)
        }

        /// Before each change from the keyboard: what was typed is only known if nothing else went to the Mac since.
        private func catchUp() {
            if stampMoved { forget(why: "a key or click reached the Mac some other way") }
        }

        private func send(_ keys: [TypedText.Key]) {
            if !keys.isEmpty { sentAt = Date() }
            for key in keys {
                switch key {
                case .left: Keys.press(Keys.left)
                case .right: Keys.press(Keys.right)
                case .backspace: Keys.press(Keys.backspace)
                case .type(let c): Keys.type(c)
                }
            }
            restamp()
        }

        override func becomeFirstResponder() -> Bool {
            let became = super.becomeFirstResponder()
            if became {
                forget(TypedText.fresh)
                watch?.invalidate()
                watch = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
                    guard let self, self.cursor == nil else { return }
                    if self.stampMoved { self.forget(why: "a key or click reached the Mac some other way") }
                    self.lookAtMac()
                }
            }
            return became
        }

        override func resignFirstResponder() -> Bool {
            watch?.invalidate()
            watch = nil
            return super.resignFirstResponder()
        }

        // MARK: Typing

        var hasText: Bool { true }

        func insertText(_ text: String) {
            guard !HardwareKeys.keyJustPressed else { return }
            if fieldWaiting != nil { DispatchQueue.main.async { [weak self] in self?.fieldNoLongerWaits() } }
            catchUp()
            defer { StickyKeys.shared.releaseAll() }
            // A shortcut (⌘ and a letter) or a password: typed as it is, and not kept.
            if !StickyKeys.shared.down.isEmpty || isSecureTextEntry {
                for c in text { Keys.type(c) }
                return forget(isSecureTextEntry ? TypedText.fresh : TypedText.midText)
            }
            if let keys = doc.insert(text) { return send(keys) }
            // Return, Tab, or what the Mac's keyboard can't type: typed as it is (what it can), and after
            // Return or Tab the next line or field starts afresh.
            // What is selected only here goes first; what the Mac itself has selected is the Mac's to deal with (Return
            // in an address bar takes its completion, it doesn't delete it).
            if !doc.selection.isEmpty, doc.selection != doc.macSelection, let keys = doc.deleteBackward() { send(keys) }
            for c in text { Keys.type(c) }
            forget(text.contains { $0 == "\n" || $0 == "\t" } ? TypedText.fresh : TypedText.midText)
        }

        func deleteBackward() {
            guard !HardwareKeys.keyJustPressed else { return }
            catchUp()
            defer { StickyKeys.shared.releaseAll() }
            if !isSecureTextEntry, let keys = doc.deleteBackward() { return send(keys) }
            Keys.press(Keys.backspace) // into text Henge can't see
            forget(isSecureTextEntry ? TypedText.fresh : TypedText.midText)
        }

        func text(in range: UITextRange) -> String? {
            span(range).map(doc.text)
        }

        func replace(_ range: UITextRange, withText text: String) {
            guard let r = span(range) else { return }
            catchUp()
            if let keys = doc.replace(r, text) {
                HengeLog.log("input", "keyboard replaced \(r.count) typed characters with \(text.count)")
                send(keys)
            } else {
                forget(why: "the keyboard changed \(r) to \(text.count) characters, out of reach") // the Mac keeps what was typed
            }
        }

        var selectedTextRange: UITextRange? {
            get { TextSpan(doc.selection) }
            set {
                guard let new = newValue.flatMap(span), new != doc.selection else { return }
                // The space bar's cursor has moved the Mac's caret itself.
                guard cursor == nil, Date() >= quietUntil else { return }
                catchUp()
                if let keys = doc.select(new) { send(keys) } else { forget(why: "the keyboard moved to \(new), out of reach") }
            }
        }

        var markedTextRange: UITextRange? { doc.marked.map(TextSpan.init) }
        var markedTextStyle: [NSAttributedString.Key: Any]?

        func setMarkedText(_ markedText: String?, selectedRange: NSRange) {
            catchUp()
            let s = markedText ?? ""
            let at = selectedRange.location == NSNotFound ? s.count : selectedRange.location
            if let keys = doc.setMarked(s, selected: at..<(at + selectedRange.length)) { send(keys) }
            else { forget(why: "the keyboard couldn't take the selection off the Mac") }
        }

        func unmarkText() {
            guard let m = doc.marked else { return }
            defer { DispatchQueue.main.async { [weak self] in self?.fieldNoLongerWaits() } }
            let composed = doc.text(m)
            if let keys = doc.commit() { return send(keys) }
            for c in composed where TypedText.typeable(c) { Keys.type(c) }
            forget(why: "the keyboard composed what the Mac can't type")
        }

        weak var inputDelegate: UITextInputDelegate?
        lazy var tokenizer: UITextInputTokenizer = UITextInputStringTokenizer(textInput: self)

        // MARK: Positions

        private func span(_ range: UITextRange) -> Range<Int>? {
            guard let a = (range.start as? TextSpot)?.at, let b = (range.end as? TextSpot)?.at else { return nil }
            let lo = min(max(min(a, b), 0), doc.count), hi = min(max(max(a, b), 0), doc.count)
            return lo..<hi
        }

        private func spot(_ p: UITextPosition) -> Int { min(max((p as? TextSpot)?.at ?? 0, 0), doc.count) }

        var beginningOfDocument: UITextPosition { TextSpot(0) }
        var endOfDocument: UITextPosition { TextSpot(doc.count) }

        func textRange(from fromPosition: UITextPosition, to toPosition: UITextPosition) -> UITextRange? {
            let a = spot(fromPosition), b = spot(toPosition)
            return TextSpan(min(a, b)..<max(a, b))
        }

        func position(from position: UITextPosition, offset: Int) -> UITextPosition? {
            let p = spot(position) + offset
            return (0...doc.count).contains(p) ? TextSpot(p) : nil
        }

        func position(from position: UITextPosition, in direction: UITextLayoutDirection, offset: Int) -> UITextPosition? {
            self.position(from: position, offset: direction == .left || direction == .up ? -offset : offset)
        }

        func compare(_ position: UITextPosition, to other: UITextPosition) -> ComparisonResult {
            let a = spot(position), b = spot(other)
            return a < b ? .orderedAscending : a > b ? .orderedDescending : .orderedSame
        }

        func offset(from: UITextPosition, to toPosition: UITextPosition) -> Int { spot(toPosition) - spot(from) }

        func position(within range: UITextRange, farthestIn direction: UITextLayoutDirection) -> UITextPosition? {
            direction == .left || direction == .up ? range.start : range.end
        }

        func characterRange(byExtending position: UITextPosition, in direction: UITextLayoutDirection) -> UITextRange? {
            let p = spot(position)
            return direction == .left || direction == .up ? TextSpan(0..<p) : TextSpan(p..<doc.count)
        }

        func baseWritingDirection(for position: UITextPosition, in direction: UITextStorageDirection) -> NSWritingDirection { .leftToRight }
        func setBaseWritingDirection(_ writingDirection: NSWritingDirection, for range: UITextRange) {}

        // Nothing is drawn: the Mac shows the text.
        private var caretBox: CGRect { CGRect(x: 0, y: 0, width: 2, height: Self.lineHeight) }
        func firstRect(for range: UITextRange) -> CGRect { caretBox }
        func caretRect(for position: UITextPosition) -> CGRect { caretBox }
        func selectionRects(for range: UITextRange) -> [UITextSelectionRect] { [] }
        func closestPosition(to point: CGPoint) -> UITextPosition? { TextSpot(doc.selection.lowerBound) }
        func closestPosition(to point: CGPoint, within range: UITextRange) -> UITextPosition? { range.start }
        func characterRange(at point: CGPoint) -> UITextRange? { nil }
        var textInputView: UIView { self }

        // MARK: The space bar's cursor

        /// How far a finger moves on the keyboard for one arrow press, across and up or down.
        static let charWidth: CGFloat = 9, lineHeight: CGFloat = 30
        private var cursor: (start: CGPoint, x: Int, y: Int, lost: Bool)?
        private var quietUntil = Date.distantPast

        func beginFloatingCursor(at point: CGPoint) {
            catchUp()
            cursor = (point, 0, 0, false)
        }

        func updateFloatingCursor(at point: CGPoint) {
            guard var c = cursor else { return }
            let x = Int(((point.x - c.start.x) / Self.charWidth).rounded(.towardZero))
            let y = Int(((point.y - c.start.y) / Self.lineHeight).rounded(.towardZero))
            if x != c.x {
                for _ in 0..<abs(x - c.x) { Keys.press(x > c.x ? Keys.right : Keys.left) }
                if !c.lost, !doc.macMoved(by: x - c.x) { c.lost = true }
            }
            if y != c.y {
                for _ in 0..<abs(y - c.y) { Keys.press(y > c.y ? Keys.down : Keys.up) }
                c.lost = true // lines Henge can't see
            }
            if x != c.x || y != c.y { sentAt = Date() }
            c.x = x; c.y = y
            cursor = c
            restamp()
        }

        func endFloatingCursor() {
            guard let c = cursor else { return }
            cursor = nil
            quietUntil = Date().addingTimeInterval(0.4) // iOS may set the selection as it lets go
            HengeLog.log("input", "space bar moved the Mac's caret \(c.x) across, \(c.y) down")
            if c.lost { return forget(why: "the space bar took the caret past what the keyboard knows") }
            restamp()
            DispatchQueue.main.async { [weak self] in
                guard let self, let d = self.inputDelegate else { return }
                d.selectionWillChange(self); d.selectionDidChange(self)
            }
        }
    }
}

/// A place in the stand-in text.
private final class TextSpot: UITextPosition {
    let at: Int
    init(_ at: Int) { self.at = at }
}

private final class TextSpan: UITextRange {
    let range: Range<Int>
    init(_ range: Range<Int>) { self.range = range }
    override var start: UITextPosition { TextSpot(range.lowerBound) }
    override var end: UITextPosition { TextSpot(range.upperBound) }
    override var isEmpty: Bool { range.isEmpty }
}

/// The keyboard's accessory: the row of the Mac's keys. While the keyboard floats, iPadOS leaves it along the
/// whole bottom of the screen, but only the glass bar in its middle (or the folded button in its corner) is
/// drawn, and touches anywhere else go through to the Mac under it. Folded on top of docked keys, likewise only
/// its ⌘ button is drawn, at the right end of the strip.
final class RowBar: UIView {
    private let host: UIHostingController<ModifierRow>
    private var watch: AnyCancellable?
    private var floats = false

    init(_ row: ModifierRow) {
        host = UIHostingController(rootView: row)
        super.init(frame: CGRect(x: 0, y: 0, width: UIScreen.main.bounds.width, height: ModifierRow.height))
        host.sizingOptions = []
        // Along the bottom of the screen the row's safe area left its keys too little room, and they
        // spilled out of its top over the Mac. The keys use the whole row; the home indicator is hidden.
        host.safeAreaRegions = []
        host.view.backgroundColor = .clear
        host.view.frame = bounds
        backgroundColor = .clear
        // Its height is its own (below), as iPadOS asks it.
        autoresizingMask = .flexibleHeight
        addSubview(host.view)
        watch = RowPlacement.shared.$keyboardFloats.removeDuplicates().sink { [weak self] floats in
            guard let self, self.floats != floats else { return }
            self.floats = floats
            self.invalidateIntrinsicContentSize()
        }
    }

    required init?(coder: NSCoder) { fatalError("made in code") }

    /// Floating, the bar keeps clear of the screen's bottom edge.
    override var intrinsicContentSize: CGSize {
        CGSize(width: UIView.noIntrinsicMetric, height: floats ? ModifierRow.height + ModifierRow.floatingGap : ModifierRow.height)
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let span = onScreen()
        if host.view.frame != span { host.view.frame = span }
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        setNeedsLayout()
    }

    /// Lays the keys out again where the bar is now: iPadOS moves it without resizing it.
    func remeasure() {
        setNeedsLayout()
        layoutIfNeeded()
    }

    private var loggedSpan = CGRect.null
    /// The part of the bar on the screen, should iPadOS put it partway off (as it did inside a hardware keyboard's bar,
    /// before the row stopped being iPadOS's there: CatcherView.givesRow).
    private func onScreen() -> CGRect {
        guard let screen = window?.windowScene?.screen else { return bounds }
        let at = convert(bounds, to: screen.coordinateSpace)
        let left = max(0, screen.bounds.minX - at.minX), right = min(bounds.width, screen.bounds.maxX - at.minX)
        guard right - left >= 120 else { return bounds }
        let span = CGRect(x: left, y: 0, width: right - left, height: bounds.height)
        if span != bounds, span != loggedSpan {
            loggedSpan = span
            HengeLog.log("input", "the row is at x \(Int(at.minX))-\(Int(at.maxX)) of a \(Int(screen.bounds.width)) pt screen: keys kept to \(Int(left))-\(Int(right))")
        }
        return span
    }

    override func point(inside point: CGPoint, with event: UIEvent?) -> Bool {
        let span = host.view.frame
        // Folded, only the ⌘ button is the row's (over docked keys too, where the rest of the strip is the Mac's).
        let folded = RowPlacement.shared.folded
        guard floats || folded else { return span.contains(point) }
        return ModifierRow.floatingFrame(in: span.size, folded: folded)
            .offsetBy(dx: span.minX, dy: 0).insetBy(dx: -4, dy: -4).contains(point)
    }
}

/// Linux input key codes, which the Mac's keyboard device takes.
enum Keys {
    static let backspace: UInt16 = 14
    static let enter: UInt16 = 28
    static let shift: UInt16 = 42
    static let escape: UInt16 = 1
    static let tab: UInt16 = 15
    static let control: UInt16 = 29
    static let option: UInt16 = 56
    static let command: UInt16 = 125
    static let up: UInt16 = 103
    static let left: UInt16 = 105
    static let right: UInt16 = 106
    static let down: UInt16 = 108
    private static let letters: [Character: UInt16] = [
        "q": 16, "w": 17, "e": 18, "r": 19, "t": 20, "y": 21, "u": 22, "i": 23, "o": 24, "p": 25,
        "a": 30, "s": 31, "d": 32, "f": 33, "g": 34, "h": 35, "j": 36, "k": 37, "l": 38,
        "z": 44, "x": 45, "c": 46, "v": 47, "b": 48, "n": 49, "m": 50,
    ]
    private static let plain: [Character: UInt16] = [
        "1": 2, "2": 3, "3": 4, "4": 5, "5": 6, "6": 7, "7": 8, "8": 9, "9": 10, "0": 11,
        "-": 12, "=": 13, "\t": 15, "[": 26, "]": 27, "\n": 28, ";": 39, "'": 40, "`": 41, "\\": 43,
        ",": 51, ".": 52, "/": 53, " ": 57,
    ]
    private static let shifted: [Character: UInt16] = [
        "!": 2, "@": 3, "#": 4, "$": 5, "%": 6, "^": 7, "&": 8, "*": 9, "(": 10, ")": 11,
        "_": 12, "+": 13, "{": 26, "}": 27, ":": 39, "\"": 40, "~": 41, "|": 43,
        "<": 51, ">": 52, "?": 53,
    ]

    /// When Return last went to the Mac: at the login window, someone logging in.
    static var lastEnter = Date.distantPast

    /// byHenge: typed for the person (TypingQueue), so not a press of theirs (henge_vm_key_typed).
    static func press(_ code: UInt16, shifted: Bool = false, byHenge: Bool = false) {
        if shifted { key(shift, down: true, byHenge: byHenge) }
        key(code, down: true, byHenge: byHenge)
        key(code, down: false, byHenge: byHenge)
        if shifted { key(shift, down: false, byHenge: byHenge) }
    }

    static func key(_ code: UInt16, down: Bool, byHenge: Bool = false) {
        if down, code == enter { lastEnter = Date() }
        if byHenge { henge_vm_key_typed(code, down ? 1 : 0) } else { henge_vm_key(code, down ? 1 : 0) }
    }

    /// A letter (or space) with a modifier held.
    static func chord(_ modifier: UInt16, _ c: Character) {
        henge_vm_key(modifier, 1)
        type(c)
        henge_vm_key(modifier, 0)
    }

    /// One character at a time, for what the Mac has to keep up with, after anything else Henge is typing
    /// (TypingQueue). Dropped only as the Mac pauses.
    @MainActor static func typeSlowly(_ text: String) async {
        await TypingQueue.shared.type(text, by: .other)
    }

    static func type(_ c: Character, byHenge: Bool = false) {
        if let code = letters[c] { press(code, byHenge: byHenge) }
        else if let lower = c.lowercased().first, let code = letters[lower] { press(code, shifted: true, byHenge: byHenge) }
        else if let code = plain[c] { press(code, byHenge: byHenge) }
        else if let code = shifted[c] { press(code, shifted: true, byHenge: byHenge) }
    }

    /// The key a character is typed with, unshifted (a shortcut's letter: ⌘V).
    static func code(_ c: Character) -> UInt16? { letters[c] ?? plain[c] }
}

/// Whether a hardware keyboard is connected: one answer for the whole app, from GameController's own connect and
/// disconnect notices, taken as they come. (Read on the disconnect notice, GCKeyboard.coalesced still has the keyboard
/// it is about.) GameController knows of a keyboard from its first key press until it goes.
final class HardwareKeyboard {
    static let shared = HardwareKeyboard()
    /// Posted on the main queue once `connected` has changed.
    static let changed = Notification.Name("henge.hardwareKeyboard.changed")
    private(set) var connected = GCKeyboard.coalesced != nil
    private var observers: [NSObjectProtocol] = []

    private init() {
        let center = NotificationCenter.default
        observers = [
            center.addObserver(forName: .GCKeyboardDidConnect, object: nil, queue: .main) { [weak self] _ in self?.set(true) },
            center.addObserver(forName: .GCKeyboardDidDisconnect, object: nil, queue: .main) { [weak self] _ in self?.set(false) },
        ]
    }

    private func set(_ now: Bool) {
        guard now != connected else { return }
        connected = now
        NotificationCenter.default.post(name: Self.changed, object: nil)
    }
}

/// TypingQueue's keys, to the Mac's keyboard, typed for the person: none counts as a press of theirs, but for the
/// Return after a piece (what it does moves the focus as theirs would).
@MainActor
final class MacKeyboard: TypingSink {
    /// A ⌘ latched on the row, or held on a hardware keyboard, would make "hello" ⌘H, a held Shift would change the
    /// password, and ⌘ would make the password's Return ⌘Return: let go on the Mac, whoever holds them, before each
    /// character, a shortcut and that Return. What the iPad
    /// knows of a hardware keyboard's keys stays: its release when the person lets go is sent as ever.
    private func letGo() {
        StickyKeys.shared.releaseAll()
        henge_vm_modifiers_up()
    }
    func begin() { letGo() }
    func type(_ c: Character) {
        letGo()
        Keys.type(c, byHenge: true)
    }
    func key(_ code: UInt16, down: Bool) { Keys.key(code, down: down, byHenge: true) }
    func press(_ code: UInt16) {
        letGo()
        Keys.press(code)
    }
    func notPressed(_ code: UInt16, _ why: String) {
        HengeLog.log("input", "typed without \(code == Keys.enter ? "Return" : "key \(code)") after it: \(why)")
    }
    func gap() async { try? await Task.sleep(nanoseconds: 25_000_000) }
}

extension TypingQueue {
    static let shared = TypingQueue(sink: MacKeyboard(), clock: { mach_absolute_time() }, lastPress: { henge_vm_last_press() })
}

/// ⌃, ⌥ and ⌘ stay down from a tap until the next key or click, so a shortcut takes one finger.
final class StickyKeys: ObservableObject {
    static let shared = StickyKeys()
    @Published private(set) var down: Set<UInt16> = []

    func toggle(_ code: UInt16) {
        if down.contains(code) {
            henge_vm_key(code, 0)
            down.remove(code)
        } else {
            henge_vm_key(code, 1)
            down.insert(code)
        }
    }

    func releaseAll() {
        guard !down.isEmpty else { return }
        for code in down { henge_vm_key(code, 0) }
        down.removeAll()
    }
}

/// The keys an on-screen keyboard doesn't have, in the slot above it.
struct ModifierRow: View {
    /// Short, so the Mac moves little to make room for it.
    static let height: CGFloat = KeyboardLayout.rowHeight
    /// While the keyboard floats: the row is a glass bar over the Mac, kept this far off the screen's bottom edge.
    static let floatingGap: CGFloat = KeyboardLayout.gap
    /// Where the glass bar (or the folded button) is drawn in the row's place along the bottom.
    static func floatingFrame(in size: CGSize, folded: Bool) -> CGRect { KeyboardLayout.glass(in: size, folded: folded) }
    let hide: () -> Void
    @ObservedObject private var sticky = StickyKeys.shared
    @ObservedObject private var placement = RowPlacement.shared
    /// Folded away to one ⌘ button (RowPlacement.folded), on top of docked keys or over the Mac.
    private var folded: Bool { placement.folded }
    private var floating: Bool { placement.keyboardFloats }

    var body: some View {
        if floating || folded {
            GeometryReader { space in
                let frame = Self.floatingFrame(in: CGSize(width: space.size.width, height: Self.height), folded: folded)
                floatingBar
                    .frame(width: frame.width, height: frame.height)
                    .position(x: frame.midX, y: frame.midY)
                    .animation(.snappy(duration: 0.25), value: folded)
            }
            .ignoresSafeArea()
            .transition(.opacity)
        } else {
            keys
                // Along the bottom of the screen its rounded corners would clip the end keys.
                .padding(.horizontal, placement.alongBottom ? 18 : 6)
                .padding(.vertical, 4)
                .frame(height: Self.height)
                .ignoresSafeArea()
        }
    }

    @ViewBuilder private var floatingBar: some View {
            if folded {
                Button { placement.setFolded(false) } label: {
                    Image(systemName: "command")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(placement.passwordField ? Desk.accentLight : Desk.text)
                        .frame(width: Self.height, height: Self.height)
                        .deskGlass(in: Circle())
                        .shadow(color: .black.opacity(0.25), radius: 6, y: 2)
                }
                .accessibilityLabel("Show the Mac's keys")
                .accessibilityHint("Esc, Tab, Control, Option, Command, the arrows, the password and dictation")
                // The row's own keys, a touch and hold away while it is folded (a floating keyboard has no key of
                // its own to put itself away).
                .contextMenu {
                    Button("Show the Mac's Keys", systemImage: "command") { placement.setFolded(false) }
                    Button("Type Mac Password", systemImage: "key.fill") { MacPassword.typeOrAsk(enter: placement.passwordField) }
                    Button("Hide Keyboard", systemImage: "keyboard.chevron.compact.down", action: hide)
                }
            } else {
                keys
                    .padding(.vertical, 4).padding(.horizontal, 6)
                    .frame(height: Self.height)
                    .deskGlass(in: Capsule())
                    .shadow(color: .black.opacity(0.25), radius: 6, y: 2)
            }
    }

    private var keys: some View {
        HStack(spacing: 5) {
            Button { placement.setFolded(true) } label: {
                Image(systemName: "chevron.right")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(Desk.dim)
                    .frame(width: 22)
                    .frame(maxHeight: .infinity)
            }
            .accessibilityLabel("Fold away the Mac's keys")
            HeldKey(label: "esc", code: Keys.escape)
            HeldKey(label: "tab", code: Keys.tab)
            modifier("⌃", Keys.control, "Control")
            modifier("⌥", Keys.option, "Option")
            modifier("⌘", Keys.command, "Command")
            HeldKey(label: "←", code: Keys.left)
            HeldKey(label: "↑", code: Keys.up)
            HeldKey(label: "↓", code: Keys.down)
            HeldKey(label: "→", code: Keys.right)
            PasswordKey()
            if Dictation.isAvailable { DictateKey() }
            Button(action: hide) {
                Image(systemName: "keyboard.chevron.compact.down")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(Desk.text)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(Desk.raised, in: RoundedRectangle(cornerRadius: 7, style: .continuous))
            }
            .accessibilityLabel("Hide keyboard")
        }
    }

    private func modifier(_ symbol: String, _ code: UInt16, _ name: String) -> some View {
        let on = sticky.down.contains(code)
        return Button {
            sticky.toggle(code)
            UIImpactFeedbackGenerator(style: .light).impactOccurred()
        } label: {
            Text(symbol)
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(on ? Color.white : Desk.text)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(on ? Desk.accent : Desk.raised, in: RoundedRectangle(cornerRadius: 7, style: .continuous))
        }
        .accessibilityLabel(name)
    }
}

/// The row with a hardware keyboard: only Dictation and Type Mac Password, the keyboard has the rest. A small glass
/// control in the bottom right corner (KeyboardLayout.compactInWindow), clear of the Mac's Dock. Touched and held, it
/// has everything else the row has (Esc, Tab, the arrows, Control, Option and Command held for the next key, changing
/// the password, hiding it), so nothing is lost. Drawn by the session at its own frame: touches anywhere else are the
/// Mac's.
struct CompactMacKeys: View {
    let hide: () -> Void
    @ObservedObject private var sticky = StickyKeys.shared
    @ObservedObject private var placement = RowPlacement.shared
    @ObservedObject private var dictation = Dictation.shared

    @MainActor static var keys: Int { Dictation.isAvailable ? 2 : 1 }
    /// Control, Option and Command as the Mac's menus write them, in that order.
    private static let modifiers: [(code: UInt16, symbol: String, name: String)] = [
        (Keys.control, "⌃", "Control"), (Keys.option, "⌥", "Option"), (Keys.command, "⌘", "Command"),
    ]
    private static let more = "Touch and hold for Esc, Tab, the arrow keys and the rest of the Mac's keys"

    var body: some View {
        HStack(spacing: KeyboardLayout.compactPadding) {
            // Held from the menu for the next key: shown, as the row shows its held keys, until that key (or a tap here).
            let held = Self.modifiers.filter { sticky.down.contains($0.code) }
            if !held.isEmpty {
                Button { sticky.releaseAll() } label: {
                    Text(held.map(\.symbol).joined())
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(Color.white)
                        .padding(.horizontal, 10)
                        .frame(minWidth: KeyboardLayout.compactKey)
                        .frame(height: KeyboardLayout.compactKey - 2 * KeyboardLayout.compactPadding)
                        .background(Desk.accent, in: Capsule())
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("\(held.map(\.name).joined(separator: ", ")) held for the next key")
                .accessibilityHint("Lets go")
            }
            if Dictation.isAvailable {
                let on = dictation.isOn
                Button {
                    UIImpactFeedbackGenerator(style: .light).impactOccurred()
                    dictation.toggle()
                } label: {
                    Image(systemName: on ? "mic.fill" : "mic")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(on ? Color.white : Desk.text)
                        .frame(width: KeyboardLayout.compactKey, height: KeyboardLayout.compactKey - 2 * KeyboardLayout.compactPadding)
                        .background(on ? Color.red.opacity(0.85) : Color.clear, in: Capsule())
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(on ? "Stop dictating" : "Dictate")
                .accessibilityHint("Speak instead of typing; your words go where the Mac's cursor is. " + Self.more)
            }
            let field = placement.passwordField
            Button { MacPassword.typeOrAsk(enter: field) } label: {
                Image(systemName: "key.fill")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(field ? Color.white : Desk.text)
                    .frame(width: KeyboardLayout.compactKey, height: KeyboardLayout.compactKey - 2 * KeyboardLayout.compactPadding)
                    .background(field ? Desk.accent : Color.clear, in: Capsule())
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Type Mac password")
            .accessibilityHint((field ? "Types it, then presses Return. " : "Types it where the Mac's cursor is. ") + Self.more)
        }
        .padding(.horizontal, KeyboardLayout.compactPadding)
        .frame(height: KeyboardLayout.rowHeight)
        .deskGlass(in: Capsule())
        .shadow(color: .black.opacity(0.25), radius: 6, y: 2)
        .contentShape(.contextMenuPreview, Capsule())
        .contextMenu { menu }
    }

    @ViewBuilder private var menu: some View {
        Section("Mac Keys") {
            Button("Esc") { press(Keys.escape) }
            Button("Tab") { press(Keys.tab) }
            Menu("Arrow Keys", systemImage: "arrow.up.and.down.and.arrow.left.and.right") {
                Button("Left", systemImage: "arrow.left") { press(Keys.left) }
                Button("Up", systemImage: "arrow.up") { press(Keys.up) }
                Button("Down", systemImage: "arrow.down") { press(Keys.down) }
                Button("Right", systemImage: "arrow.right") { press(Keys.right) }
            }
        }
        Section("Hold for the Next Key") {
            ForEach(Self.modifiers, id: \.code) { modifier($0.name, $0.name.lowercased(), $0.code) }
        }
        Section {
            Button("Change Mac Password", systemImage: "key") {
                MacPassword.typeOnceSaved = nil
                NotificationCenter.default.post(name: MacPassword.wanted, object: nil)
            }
            Button("Hide", systemImage: "keyboard.chevron.compact.down", action: hide)
        }
    }

    /// As the row's modifier keys: down until the next key or click (StickyKeys).
    private func modifier(_ name: String, _ symbol: String, _ code: UInt16) -> some View {
        Toggle(isOn: Binding(get: { sticky.down.contains(code) },
                             set: { on in if on != sticky.down.contains(code) { sticky.toggle(code) } })) {
            Label(name, systemImage: symbol)
        }
    }

    private func press(_ code: UInt16) {
        Keys.press(code)
        StickyKeys.shared.releaseAll()
    }
}

/// A key that goes down when touched and up when let go, so holding it repeats as on a Mac.
private struct HeldKey: View {
    let label: String
    let code: UInt16
    @State private var down = false

    var body: some View {
        Text(label)
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(Desk.text)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(down ? Desk.accent : Desk.raised, in: RoundedRectangle(cornerRadius: 7, style: .continuous))
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        guard !down else { return }
                        down = true
                        henge_vm_key(code, 1)
                        UIImpactFeedbackGenerator(style: .light).impactOccurred()
                    }
                    .onEnded { _ in
                        down = false
                        henge_vm_key(code, 0)
                        StickyKeys.shared.releaseAll()
                    }
            )
            .accessibilityLabel(label)
    }
}

/// Dictation (Dictation.swift): the first tap listens, and what is said is typed at the Mac's cursor; the next stops.
private struct DictateKey: View {
    @ObservedObject private var dictation = Dictation.shared

    var body: some View {
        let on = dictation.isOn
        Image(systemName: on ? "mic.fill" : "mic")
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(on ? Color.white : Desk.text)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(on ? Color.red.opacity(0.85) : Desk.raised, in: RoundedRectangle(cornerRadius: 7, style: .continuous))
            .contentShape(Rectangle())
            .onTapGesture {
                UIImpactFeedbackGenerator(style: .light).impactOccurred()
                dictation.toggle()
            }
            .accessibilityLabel(on ? "Stop dictating" : "Dictate")
            .accessibilityHint("Speak instead of typing; your words go where the Mac's cursor is")
    }
}

/// Types the Mac's password, saved in this device's keychain, into whatever has the Mac's focus. The first
/// tap asks for it; holding the key changes or forgets it.
private struct PasswordKey: View {
    @ObservedObject private var placement = RowPlacement.shared

    var body: some View {
        let field = placement.passwordField
        Image(systemName: "key.fill")
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(field ? Color.white : Desk.text)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(field ? Desk.accent : Desk.raised, in: RoundedRectangle(cornerRadius: 7, style: .continuous))
            .contentShape(Rectangle())
            .onTapGesture { MacPassword.typeOrAsk(enter: field) }
            .onLongPressGesture(minimumDuration: 0.5) {
                UIImpactFeedbackGenerator(style: .medium).impactOccurred()
                MacPassword.typeOnceSaved = nil
                NotificationCenter.default.post(name: MacPassword.wanted, object: nil)
            }
            .accessibilityLabel("Type Mac password")
            .accessibilityHint("Hold to change or forget the saved password")
    }
}

/// The running Mac's password, kept in this device's keychain under the Mac's id, never in the app's files.
enum MacPassword {
    /// Posted when the password should be asked for: the session shows a secure field for it.
    static let wanted = Notification.Name("henge.macPassword.wanted")
    /// The Mac whose password the key types, set by the session showing it.
    static var mac: UUID?

    /// Set when a tap asked for the password because none was saved: type it once it is, and whether to press
    /// Return after it. Nil when it is only being changed.
    static var typeOnceSaved: Bool?

    /// Types the password (and Return, to log in at once), or asks for it first if none is saved.
    @MainActor static func typeOrAsk(enter: Bool = false) {
        guard let mac, let password = read(mac) else {
            typeOnceSaved = enter
            NotificationCenter.default.post(name: wanted, object: nil)
            return
        }
        // A second tap while it is typed would type it twice, Return and all.
        guard !TypingQueue.shared.busy(.password) else { return }
        UIImpactFeedbackGenerator(style: .light).impactOccurred()
        // Dictation's words still to type would go into the password field, and the next ones after it: it ends.
        Dictation.shared.cancel()
        // Return only if all of it went (dropped as the focus moves, it would log in or submit half a password),
        // nothing was pressed since (TypingQueue), and the helper, asked once it is typed, says a password field has
        // the focus then: not as the key was pressed (a tap on a chat box just before, which the helper hadn't said
        // yet, would send the password). With no helper to ask, whether Henge Tools' system part said the login window
        // was up as it was asked for: the Log In chip says it went as it types.
        let atLogin = KeyboardOwner.shared.atLoginCertainly
        TypingQueue.shared.add(password, by: .password, thenPress: enter ? Keys.enter : nil,
                               pressIf: { await KeyboardOwner.shared.passwordReturnGoes(atLogin: atLogin) })
    }

    private static func query(_ mac: UUID) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: "henge.mac-password",
         kSecAttrAccount as String: mac.uuidString]
    }

    static func read(_ mac: UUID) -> String? {
        var q = query(mac)
        q[kSecReturnData as String] = true
        q[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let data = out as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func save(_ password: String, for mac: UUID) {
        forget(mac)
        guard !password.isEmpty else { return }
        var q = query(mac)
        q[kSecValueData as String] = Data(password.utf8)
        q[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
        let status = SecItemAdd(q as CFDictionary, nil)
        if status != errSecSuccess { HengeLog.log("input", "couldn't save the Mac's password: \(status)") }
    }

    static func forget(_ mac: UUID) {
        SecItemDelete(query(mac) as CFDictionary)
    }
}

extension Animation {
    /// The curve the on-screen keyboard slides with (UIKit's private keyboard curve, as a spring).
    static let keyboard = Animation.interpolatingSpring(mass: 3, stiffness: 1000, damping: 500, initialVelocity: 0)
}

/// Whether the row of keys sits along the bottom of the screen (keyboard floating, or a hardware
/// keyboard) rather than on top of a docked keyboard.
final class RowPlacement: ObservableObject {
    static let shared = RowPlacement()
    @Published private(set) var alongBottom = false
    /// A password field has the Mac's focus: the password key stands out, and presses Return after the password.
    @Published var passwordField = false
    /// While typing with no docked keyboard (it floats, is put away, or is a hardware one): the row is the glass
    /// bar along the bottom, over the Mac.
    @Published private(set) var keyboardFloats = false
    /// The row with no on-screen keyboard: a hardware keyboard is connected (or iPadOS's own keyboard is put away).
    @Published private(set) var rowAlone = false

    /// The row folded away to one ⌘ button, as last left, starting folded (TypingBars.rowFoldedByDefault). The same
    /// over docked keys, along the bottom with a floating keyboard, and alone while dictation has the letters away.
    /// (A hardware keyboard has no row to fold: the small control, CompactMacKeys.) Folding never shows or hides the
    /// keyboard.
    @Published private(set) var folded = UserDefaults.standard.object(forKey: RowPlacement.foldedKey) as? Bool ?? TypingBars.rowFoldedByDefault
    /// A new name: the old one (henge.keyboard.rowFolded, floating only) started unfolded.
    static let foldedKey = "henge.keyboard.keysFolded"
    /// Folded on top of docked keys: its strip is see-through but for the ⌘ button, and the Mac may go behind it.
    var foldedOverDocked: Bool { !alongBottom && !rowAlone && folded }

    func setFolded(_ on: Bool) {
        guard folded != on else { return }
        folded = on
        UserDefaults.standard.set(on, forKey: Self.foldedKey)
        HengeLog.log("input", "the row of Mac keys \(on ? "folded to its ⌘ button" : "unfolded")")
    }

    func set(keyboardFloats new: Bool) {
        if keyboardFloats != new { keyboardFloats = new }
    }

    func set(rowAlone new: Bool) {
        if rowAlone != new { rowAlone = new }
    }

    func set(alongBottom new: Bool) {
        if alongBottom != new { alongBottom = new }
    }

    /// Typing with no keys on the screen (a hardware keyboard, or dictation's letters away): iPadOS is given no row,
    /// and the session draws it over the Mac at glassFrame: the glass bar, or with a hardware keyboard the small
    /// control (`compact`).
    @Published private(set) var standalone = false
    /// iPadOS's own bar for a hardware keyboard (or its collapsed button), in the window, while the row is standalone.
    @Published private(set) var systemBar: CGRect?

    /// Typing with a hardware keyboard: the row is the small control in the corner (CompactMacKeys), not the bar.
    @Published private(set) var compact = false

    func set(compact new: Bool) {
        if compact != new { compact = new }
    }

    func set(standalone new: Bool) {
        if standalone != new { standalone = new }
    }

    func set(systemBar new: CGRect?) {
        if systemBar != new { systemBar = new }
    }

    /// Where the row's glass bar (or its folded button) is in a window this size, while it is over the Mac (no
    /// docked keyboard under it): along the bottom, above iPadOS's bar if that is there. Nil while it sits on top of
    /// a docked keyboard. For whatever goes just above the row (dictation, the password button). With a hardware
    /// keyboard, the small control in the corner.
    @MainActor func glassFrame(in window: CGSize) -> CGRect? {
        guard keyboardFloats else { return nil }
        if standalone && compact {
            return KeyboardLayout.compactInWindow(CGRect(origin: .zero, size: window), bar: systemBar,
                                                  keys: CompactMacKeys.keys)
        }
        return KeyboardLayout.glassInWindow(CGRect(origin: .zero, size: window), bar: standalone ? systemBar : nil,
                                            folded: folded)
    }
}
