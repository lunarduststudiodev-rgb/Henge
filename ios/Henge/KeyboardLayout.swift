import CoreGraphics

/// What iPadOS's keyboard frame says is on the screen, and where Henge's row of Mac keys goes for it. Kept free of
/// UIKit so it is checked on the Mac (tests/test_keyboard_layout.swift, make test-keyboard-layout).
///
/// iPadOS gives one frame for whatever its keyboard shows (UIResponder.keyboardFrameEndUserInfoKey), with the app's
/// accessory (Henge's row) included when the app has one:
/// - docked: the keys along the bottom, the whole width of the screen;
/// - floating: a small keyboard somewhere over the app (the accessory goes along the screen's bottom on its own);
/// - a hardware keyboard (a Magic Keyboard, Universal Control's): no keys at all, only iPadOS's shortcuts and
///   predictions bar along the bottom, a small button in a corner when that bar is collapsed, or nothing.
/// Henge used to tell these apart by height alone, taking anything taller than its own row for a docked keyboard;
/// so iPadOS's bar with the row on it (about 100 points) made the Mac shrink for keys that weren't there.
enum KeyboardLayout {
    /// The row of Mac keys (ModifierRow).
    static let rowHeight: CGFloat = 40
    /// Over the Mac (no docked keyboard under it), the row is a glass bar kept this far off the screen's bottom edge
    /// and off iPadOS's own bar.
    static let gap: CGFloat = 10
    /// A frame with less than this of its own, Henge's row aside, has no keys: iPadOS's bar for a hardware keyboard
    /// (about 55 to 70 points), or the row alone. The smallest keyboard with keys, an iPhone's in landscape with no
    /// suggestions, is about 160; an iPad's floating one about 250.
    static let keysMinimum: CGFloat = 120

    enum Keyboard: Equatable {
        /// Nothing of the keyboard on the window: hidden, or gone below it.
        case none
        /// Keys along the bottom: the window's bottom this many points up is under them (Henge's row included).
        case docked(cover: CGFloat)
        /// A floating keyboard with keys, at this frame in the window.
        case floating(CGRect)
        /// No keys: iPadOS's bar for a hardware keyboard, or its collapsed button, at this frame in the window. It
        /// covers the Mac as any glass bar does; the Mac keeps its size.
        case bar(CGRect)
    }

    /// What a keyboard frame is. frame: in the window's coordinates; window: the window's bounds; row: the height of
    /// Henge's row inside that frame (0 when Henge gave iPadOS no row).
    static func read(_ frame: CGRect, window: CGRect, row: CGFloat) -> Keyboard {
        guard !frame.isNull, !frame.isEmpty, frame.intersects(window) else { return .none }
        let keys = frame.height - row
        guard keys >= keysMinimum else { return .bar(frame) }
        // Docked keys span the screen; in a Stage Manager window they span the window too (and may start below its
        // top or below the window altogether). A floating keyboard never spans a window this app can have.
        let spans = frame.minX <= window.minX + 1 && frame.maxX >= window.maxX - 1
        guard spans else { return .floating(frame) }
        return .docked(cover: max(window.maxY - frame.minY, 0))
    }

    /// Where the row's glass bar (or, folded, its one button) is drawn in a strip `size` wide, its top at 0: centred,
    /// and the folded button clear of the screen's rounded corner.
    static func glass(in size: CGSize, folded: Bool) -> CGRect {
        if folded { return CGRect(x: size.width - 20 - rowHeight, y: 0, width: rowHeight, height: rowHeight) }
        let width = min(520, size.width - 32)
        return CGRect(x: (size.width - width) / 2, y: 0, width: width, height: rowHeight)
    }

    /// With a hardware keyboard the row is a small control: Dictation and Type Mac Password, the two things a
    /// keyboard has no key for (the rest are a touch and hold away on it). Each of its buttons, and the glass around them.
    static let compactKey: CGFloat = 40
    static let compactPadding: CGFloat = 4
    /// Off the window's right edge: clear of the screen's rounded corner, as the folded button is.
    static let cornerInset: CGFloat = 20

    static func compactWidth(keys: Int) -> CGFloat {
        CGFloat(max(keys, 1)) * compactKey + CGFloat(max(keys, 1) + 1) * compactPadding
    }

    /// Where the small control goes with a hardware keyboard: the bottom right corner, never over the middle of the
    /// bottom edge where the Mac's Dock is when it is at the bottom (centred, it grows from the middle out). A Dock on
    /// the left is on the other side; one on the right is centred down that edge, above the corner. It sits above
    /// iPadOS's own bar for a hardware keyboard, or its collapsed button in that same corner, as the glass bar does.
    /// Henge can't see where the Mac's Dock is (Henge Tools doesn't say), so the corner is the place clear of all three.
    static func compactInWindow(_ window: CGRect, bar: CGRect?, keys: Int) -> CGRect {
        let width = compactWidth(keys: keys)
        var frame = above(CGRect(x: window.maxX - cornerInset - width, y: window.maxY - gap - rowHeight, width: width, height: rowHeight),
                          bar)
        frame.origin.y = max(frame.origin.y, window.minY)
        frame.origin.x = max(frame.origin.x, window.minX)
        return frame
    }

    /// `frame`, moved up to sit `gap` above `other` if it would be on it or closer than that; left where it is if
    /// `other` is beside it or above it. How the row keeps off iPadOS's bar, and the password chip off the row.
    static func above(_ frame: CGRect, _ other: CGRect?) -> CGRect {
        guard let other, !other.isEmpty, other.minX < frame.maxX + gap, other.maxX > frame.minX - gap,
              other.minY < frame.maxY + gap, other.maxY > frame.minY - gap else { return frame }
        var moved = frame
        moved.origin.y = min(frame.minY, other.minY - gap - frame.height)
        return moved
    }

    /// The password chip in its corner while typing: above iPadOS's bar for a hardware keyboard (screen-wide, or its
    /// collapsed button) where it would be under it, then above Henge's row (which is above that bar) likewise.
    static func chipClear(_ chip: CGRect, systemBar: CGRect?, row: CGRect?) -> CGRect {
        above(above(chip, systemBar), row)
    }

    /// Where the glass bar goes in the window when Henge draws the row itself (no keys on the screen): along the
    /// bottom, and above iPadOS's bar where that would be under it. bar: iPadOS's bar or collapsed button, if shown.
    static func glassInWindow(_ window: CGRect, bar: CGRect?, folded: Bool) -> CGRect {
        var frame = above(glass(in: window.size, folded: folded).offsetBy(dx: window.minX, dy: window.maxY - gap - rowHeight), bar)
        frame.origin.y = max(frame.origin.y, window.minY)
        return frame
    }
}

/// Which row of Mac keys goes with the keyboard there is while typing: the small control (CompactMacKeys: Dictation,
/// Type Mac Password) when a hardware keyboard is why no keys are on the screen, the whole row otherwise. Dictation
/// puts the on-screen letters away too (Dictation.lettersAway), which reads the same as a hardware keyboard's bar, so
/// what was on the screen before it is what counts: the person typing on the on-screen keys (shown with a hardware
/// keyboard connected, too) keeps the whole row, and someone with a hardware keyboard who starts dictating from the
/// small control keeps that. Kept free of UIKit so it is checked on the Mac (tests/test_keyboard_layout.swift).
struct RowChoice {
    /// On-screen keys were the last thing seen on the screen, dictation's no-letters aside.
    private(set) var keysOnScreen = true

    /// Typing starts: keys come up unless a hardware keyboard is connected (the frames that follow say for sure).
    mutating func typingStarts(hardwareKeyboard: Bool) { keysOnScreen = !hardwareKeyboard }

    /// What a keyboard frame turned out to be (KeyboardLayout.read), and whether dictation had the letters away.
    mutating func saw(_ keyboard: KeyboardLayout.Keyboard, lettersAway: Bool) {
        guard !lettersAway else { return }
        switch keyboard {
        case .docked, .floating: keysOnScreen = true
        case .bar: keysOnScreen = false
        case .none: break // put away, or not typing: says nothing about which keys
        }
    }

    /// The small control rather than the whole row. rowAlone: iPadOS was given no row (no keys on the screen).
    func compact(typing: Bool, rowAlone: Bool, hardwareKeyboard: Bool) -> Bool {
        typing && rowAlone && hardwareKeyboard && !keysOnScreen
    }
}

/// Henge's two bars while the Mac is typed into: the controls along the top, and the row of Mac keys over the
/// keyboard. Typing, both keep out of the way unless brought back, and each is remembered as it was left (across the
/// keyboard coming and going, and across launches). Neither ever shows or hides the keyboard. Kept free of UIKit so it
/// is checked on the Mac (tests/test_keyboard_layout.swift).
struct TypingBars: Equatable {
    /// The controls are hidden while not typing (henge.controls.hidden).
    var controlsHidden = false
    /// The controls are hidden while typing (henge.controls.hiddenTyping): out of the way until shown while typing.
    var controlsHiddenTyping = true

    /// Whether the controls are tucked away (only their handle shows), typing or not.
    func controlsAway(typing: Bool) -> Bool { typing ? controlsHiddenTyping : controlsHidden }

    /// Hidden by their own button: away whether typing or not, until shown again.
    mutating func hideControls() {
        controlsHidden = true
        controlsHiddenTyping = true
    }

    /// Shown by their handle (or three fingers). Typing, they then stay for typing too; not typing, the keyboard
    /// still tucks them away when it next comes up, as it does unless they were shown while typing.
    mutating func showControls(typing: Bool) {
        controlsHidden = false
        if typing { controlsHiddenTyping = false }
    }

    /// The row of Mac keys starts folded to its one ⌘ button while there are on-screen keys: most typing needs none
    /// of them. (With a hardware keyboard there is no row to fold: it is the small control, CompactMacKeys.)
    static let rowFoldedByDefault = true

    /// How much of the keyboard's cover the Mac may still use: on top of docked keys the folded row's strip is
    /// see-through but for its ⌘ button at the right end, so the Mac goes down behind it. Not on an upright screen
    /// (fills: false), where that end is the trackpad's Right-click button.
    static func macCover(_ cover: CGFloat, dockedRowFolded: Bool, fills: Bool) -> CGFloat {
        guard dockedRowFolded, fills, cover > KeyboardLayout.rowHeight else { return cover }
        return cover - KeyboardLayout.rowHeight
    }
}
