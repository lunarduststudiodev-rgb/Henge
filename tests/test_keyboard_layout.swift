import CoreGraphics
import Foundation

// How Henge reads iPadOS's keyboard frames and where its row of Mac keys goes (ios/Henge/KeyboardLayout.swift),
// with frames as iPadOS gives them in each case. make test-keyboard-layout
@main
struct KeyboardLayoutTests {
    static var failures = 0

    static func check(_ ok: Bool, _ what: String, line: Int = #line) {
        if !ok {
            failures += 1
            print("FAIL line \(line): \(what)")
        }
    }

    static func main() {
        typealias K = KeyboardLayout
        let pad = CGRect(x: 0, y: 0, width: 1376, height: 1032)     // iPad Pro 13", landscape, full screen
        let rowFloat = K.rowHeight + K.gap                          // the row's height while it floats

        // Docked: keys along the bottom, Henge's row on top. The Mac makes room for all of it.
        check(K.read(CGRect(x: 0, y: 1032 - 420, width: 1376, height: 420), window: pad, row: K.rowHeight) == .docked(cover: 420),
              "a docked keyboard covers its height")
        // Docked, without Henge's row (a sheet's field had it): still keys.
        check(K.read(CGRect(x: 0, y: 1032 - 380, width: 1376, height: 380), window: pad, row: 0) == .docked(cover: 380),
              "a docked keyboard without the row")

        // Floating: as the test log had it (y 462-837), the row along the bottom on its own.
        let floating = CGRect(x: 520, y: 462, width: 320, height: 375)
        check(K.read(floating, window: pad, row: 0) == .floating(floating), "a floating keyboard is floating")
        check(K.read(floating, window: pad, row: rowFloat) == .floating(floating), "a floating keyboard, row given")

        // A hardware keyboard with iPadOS's shortcuts bar: about 55-70 points along the bottom, Henge's row on it
        // (what made the Mac shrink before: about 100 points was taken for keys).
        let shortcuts = CGRect(x: 0, y: 1032 - 69, width: 1376, height: 69)
        let withRow = CGRect(x: 0, y: shortcuts.minY - rowFloat, width: 1376, height: 69 + rowFloat)
        check(K.read(withRow, window: pad, row: rowFloat) == .bar(withRow), "shortcuts bar plus the row: no keys")
        check(K.read(shortcuts, window: pad, row: 0) == .bar(shortcuts), "shortcuts bar alone: no keys")
        // The same, laid out partway across as iPadOS did with the row inside it (half off the right edge).
        let offCentre = CGRect(x: 688, y: 1032 - 69, width: 1376, height: 69)
        check(K.read(offCentre, window: pad, row: 0) == .bar(offCentre), "an off-centre bar still has no keys")
        // Collapsed: a small button in the bottom right corner.
        let collapsed = CGRect(x: 1376 - 80, y: 1032 - 60, width: 60, height: 44)
        check(K.read(collapsed, window: pad, row: 0) == .bar(collapsed), "the collapsed bar: no keys")
        // The row alone along the bottom (iPadOS's keyboard put away with the row left).
        check(K.read(CGRect(x: 0, y: 1032 - 40, width: 1376, height: 40), window: pad, row: K.rowHeight) != .docked(cover: 40),
              "the row alone is not a docked keyboard")

        // Gone: off the bottom, or nothing.
        check(K.read(CGRect(x: 0, y: 1032, width: 1376, height: 420), window: pad, row: 0) == .none, "below the window: gone")
        check(K.read(.null, window: pad, row: 0) == .none, "no frame: gone")
        check(K.read(.zero, window: pad, row: 0) == .none, "an empty frame: gone")

        // Stage Manager: a window 900 x 700 at (200, 100) of the screen; frames converted to the window's
        // coordinates. A docked keyboard spans the screen, so the window too, and covers its bottom 120 points.
        let stage = CGRect(x: 0, y: 0, width: 900, height: 700)
        check(K.read(CGRect(x: -200, y: 580, width: 1376, height: 420), window: stage, row: K.rowHeight) == .docked(cover: 120),
              "Stage Manager: a docked keyboard covers the part of the window over it")
        // The window ends above the keyboard: nothing of it on the window.
        check(K.read(CGRect(x: -200, y: 720, width: 1376, height: 300), window: stage, row: 0) == .none,
              "Stage Manager: a keyboard below the window is not on it")
        // A hardware keyboard's bar, crossing the window's bottom.
        check(K.read(CGRect(x: -200, y: 650, width: 1376, height: 69), window: stage, row: 0) == .bar(CGRect(x: -200, y: 650, width: 1376, height: 69)),
              "Stage Manager: the shortcuts bar has no keys")

        // iPhone: docked keys, portrait and landscape (no suggestions, a password field: the shortest there is).
        let phone = CGRect(x: 0, y: 0, width: 393, height: 852)
        check(K.read(CGRect(x: 0, y: 852 - 336, width: 393, height: 336), window: phone, row: K.rowHeight) == .docked(cover: 336),
              "iPhone portrait: docked")
        let phoneLand = CGRect(x: 0, y: 0, width: 852, height: 393)
        check(K.read(CGRect(x: 0, y: 393 - 200, width: 852, height: 200), window: phoneLand, row: K.rowHeight) == .docked(cover: 200),
              "iPhone landscape: docked")

        // Where the glass bar goes with no keys: centred, along the bottom...
        let alone = K.glassInWindow(pad, bar: nil, folded: false)
        check(abs(alone.midX - pad.midX) < 0.5, "the glass bar is centred")
        check(alone.maxY == pad.maxY - K.gap && alone.height == K.rowHeight, "the glass bar sits the gap off the bottom")
        // ...above iPadOS's shortcuts bar...
        let above = K.glassInWindow(pad, bar: shortcuts, folded: false)
        check(above.maxY == shortcuts.minY - K.gap, "the glass bar sits above iPadOS's bar")
        check(abs(above.midX - pad.midX) < 0.5, "and stays centred")
        // ...and where the collapsed button is in the corner, the centred bar isn't moved for it...
        check(K.glassInWindow(pad, bar: collapsed, folded: false) == alone, "the collapsed bar's corner doesn't lift the row")
        // ...but the folded button, in that corner too, is.
        let foldedAbove = K.glassInWindow(pad, bar: collapsed, folded: true)
        check(foldedAbove.maxY <= collapsed.minY - K.gap, "the folded button keeps off iPadOS's collapsed bar")
        // Never above the window's top.
        check(K.glassInWindow(CGRect(x: 0, y: 0, width: 400, height: 60), bar: CGRect(x: 0, y: 10, width: 400, height: 50), folded: false).minY == 0,
              "the glass bar stays on the window")
        // On an iPhone the bar fits the width, with a margin.
        let phoneGlass = K.glassInWindow(phone, bar: nil, folded: false)
        check(phoneGlass.minX >= 16 && phoneGlass.maxX <= phone.maxX - 16, "the glass bar fits an iPhone's width")

        // With a hardware keyboard the row is a small control (Dictation, Type Mac Password) in the bottom right
        // corner: never over the middle of the bottom edge, where a bottom Dock is.
        func clearOfDock(_ f: CGRect, _ window: CGRect, _ what: String, line: Int = #line) {
            check(f.minX >= window.minX + window.width * 0.7, "\(what): keeps to the right, off the middle of the bottom edge (x \(f.minX))", line: line)
            check(f.maxX <= window.maxX - 16, "\(what): clear of the rounded corner", line: line)
            check(window.contains(f), "\(what): on the window", line: line)
            check(f.width < 120 && f.height == K.rowHeight, "\(what): small (\(f.width) wide)", line: line)
        }
        let small = K.compactInWindow(pad, bar: nil, keys: 2)
        clearOfDock(small, pad, "iPad, nothing of iPadOS's")
        check(small.maxY == pad.maxY - K.gap, "the small control sits the gap off the bottom")
        // With one button (no dictation) its right end, so the password key, stays where it is.
        let one = K.compactInWindow(pad, bar: nil, keys: 1)
        check(one.maxX == small.maxX && one.width < small.width, "one button: the same right end, narrower")
        // Above iPadOS's shortcuts bar, and above its collapsed button in the same corner.
        let overShortcuts = K.compactInWindow(pad, bar: shortcuts, keys: 2)
        clearOfDock(overShortcuts, pad, "above iPadOS's bar")
        check(overShortcuts.maxY <= shortcuts.minY - K.gap, "the small control keeps off iPadOS's bar")
        let overCollapsed = K.compactInWindow(pad, bar: collapsed, keys: 2)
        clearOfDock(overCollapsed, pad, "above iPadOS's collapsed button")
        check(overCollapsed.maxY <= collapsed.minY - K.gap, "the small control keeps off the collapsed button")
        // A bar only at the far left doesn't lift it.
        check(K.compactInWindow(pad, bar: CGRect(x: 20, y: 1032 - 60, width: 60, height: 44), keys: 2) == small,
              "a button far from the corner leaves it where it is")
        // Stage Manager (a window's own coordinates), and the iPhone both ways, iPad upright.
        clearOfDock(K.compactInWindow(stage, bar: nil, keys: 2), stage, "Stage Manager")
        clearOfDock(K.compactInWindow(phone, bar: nil, keys: 2), phone, "iPhone portrait")
        clearOfDock(K.compactInWindow(phoneLand, bar: nil, keys: 2), phoneLand, "iPhone landscape")
        let upright = CGRect(x: 0, y: 0, width: 1032, height: 1376)
        clearOfDock(K.compactInWindow(upright, bar: nil, keys: 2), upright, "iPad upright")

        // The password chip (bottom left, 160 x 44) keeps off the row only where the row would be under it.
        let chip = CGRect(x: 16, y: 1032 - 16 - 44, width: 160, height: 44)
        check(K.above(chip, small) == chip, "the small control in the other corner leaves the chip where it is")
        check(K.above(chip, alone) == chip, "the centred bar, beside it on an iPad, leaves it too")
        check(K.above(chip, nil) == chip, "no row, no move")
        let phoneChip = CGRect(x: 16, y: 852 - 16 - 44, width: 160, height: 44)
        let phoneRow = K.glassInWindow(phone, bar: nil, folded: false)
        check(K.above(phoneChip, phoneRow).maxY == phoneRow.minY - K.gap, "on an iPhone the bar is under it: it goes above")
        check(K.above(chip, CGRect(x: 0, y: 100, width: 1376, height: 40)) == chip, "a row well above it doesn't move it down or up")
        // With a hardware keyboard, iPadOS's shortcuts bar runs the screen's width: the chip goes above it, and above
        // Henge's whole row where that sits on the bar over it; the small control in the other corner leaves it there.
        let bar = CGRect(x: 0, y: 1032 - 69, width: 1376, height: 69)
        let overBar = K.chipClear(chip, systemBar: bar, row: K.compactInWindow(pad, bar: bar, keys: 2))
        check(overBar.maxY == bar.minY - K.gap && overBar.minX == chip.minX, "the chip goes above the shortcuts bar")
        let wideRow = CGRect(x: 0, y: bar.minY - K.gap - 40, width: 1376, height: 40)
        let overRow = K.chipClear(chip, systemBar: bar, row: wideRow)
        check(overRow.maxY == wideRow.minY - K.gap, "and above a row along the bar's top that would be under it")
        check(K.chipClear(chip, systemBar: nil, row: small) == chip, "no bar, the small control: it stays in its corner")
        let button = CGRect(x: 1376 - 80, y: 1032 - 60, width: 60, height: 44)
        check(K.chipClear(chip, systemBar: button, row: nil) == chip, "the collapsed button at the far right leaves it there")

        // Which row: the small control with a hardware keyboard, the whole row with on-screen keys, and dictation
        // (letters away, which reads as a bar) keeps whichever it was.
        let docked420 = K.Keyboard.docked(cover: 420)
        var rows = RowChoice()
        rows.typingStarts(hardwareKeyboard: false)
        rows.saw(docked420, lettersAway: false)
        check(!rows.compact(typing: true, rowAlone: false, hardwareKeyboard: false), "on-screen keys: the whole row")
        rows.saw(.bar(shortcuts), lettersAway: true)
        check(!rows.compact(typing: true, rowAlone: true, hardwareKeyboard: false), "dictating, no hardware keyboard: the whole row")
        // A hardware keyboard from the start: iPadOS's bar, then dictation from the small control.
        rows = RowChoice()
        rows.typingStarts(hardwareKeyboard: true)
        check(rows.compact(typing: true, rowAlone: true, hardwareKeyboard: true), "hardware keyboard, before any frame: the small control")
        rows.saw(.bar(shortcuts), lettersAway: false)
        check(rows.compact(typing: true, rowAlone: true, hardwareKeyboard: true), "hardware keyboard: the small control")
        rows.saw(.bar(collapsed), lettersAway: true)
        check(rows.compact(typing: true, rowAlone: true, hardwareKeyboard: true), "dictating from the small control keeps it")
        // Taken away mid-dictation (nothing on the screen changes): the whole row, with the letters-back button.
        check(!rows.compact(typing: true, rowAlone: true, hardwareKeyboard: false), "hardware keyboard taken away: the whole row")
        check(!rows.compact(typing: false, rowAlone: true, hardwareKeyboard: true), "not typing: nothing")
        // On-screen keys shown with a hardware keyboard connected, then dictation puts the letters away: the whole row.
        rows = RowChoice()
        rows.typingStarts(hardwareKeyboard: true)
        rows.saw(.bar(shortcuts), lettersAway: false)
        rows.saw(.floating(floating), lettersAway: false)
        rows.saw(.bar(shortcuts), lettersAway: true)
        check(!rows.compact(typing: true, rowAlone: true, hardwareKeyboard: true), "keys shown on purpose, then dictation: the whole row")
        // The keyboard going (.none) says nothing; the on-screen keys put away by iPadOS's key, then: the small control.
        rows.saw(.none, lettersAway: false)
        check(!rows.compact(typing: true, rowAlone: true, hardwareKeyboard: true), "the keyboard going changes nothing")
        rows.saw(.bar(shortcuts), lettersAway: false)
        check(rows.compact(typing: true, rowAlone: true, hardwareKeyboard: true), "on-screen keys put away: the small control")

        // The bars while typing. Out of the box: the controls show, and tuck away while the keyboard is up.
        var bars = TypingBars()
        check(!bars.controlsAway(typing: false), "the controls show at first")
        check(bars.controlsAway(typing: true), "typing, the controls are out of the way")
        check(TypingBars.rowFoldedByDefault, "the row of Mac keys starts folded")
        // Shown while typing: they stay, while typing and after.
        bars.showControls(typing: true)
        check(!bars.controlsAway(typing: true) && !bars.controlsAway(typing: false), "shown while typing, they stay")
        // Hidden while typing: hidden whether typing or not, until shown (the keyboard isn't touched: nothing to check
        // here, as nothing in TypingBars can reach it).
        bars.hideControls()
        check(bars.controlsAway(typing: true) && bars.controlsAway(typing: false), "hidden while typing, they stay hidden")
        // Shown while not typing: the keyboard still tucks them away when it comes up.
        bars.showControls(typing: false)
        check(!bars.controlsAway(typing: false) && bars.controlsAway(typing: true), "shown while not typing, typing still tucks them away")
        // Hidden while not typing: hidden while typing too.
        bars.hideControls()
        check(bars.controlsAway(typing: true), "hidden while not typing, hidden while typing")
        // What the folded row leaves the Mac over docked keys.
        check(TypingBars.macCover(420, dockedRowFolded: true, fills: true) == 420 - K.rowHeight, "the folded row's strip is the Mac's")
        check(TypingBars.macCover(420, dockedRowFolded: false, fills: true) == 420, "the unfolded row is not")
        check(TypingBars.macCover(420, dockedRowFolded: true, fills: false) == 420, "upright, the trackpad stays clear of the ⌘ button")
        check(TypingBars.macCover(0, dockedRowFolded: true, fills: true) == 0, "no keyboard, nothing taken")
        // The folded button sits at the strip's right end, inside it.
        let folded = K.glass(in: CGSize(width: 1376, height: K.rowHeight), folded: true)
        check(folded.maxX <= 1376 - 16 && folded.minY == 0 && folded.height == K.rowHeight, "the folded button is in the strip's corner")

        if failures > 0 {
            print("\(failures) failed")
            exit(1)
        }
        print("keyboard layout: all passed")
    }
}
