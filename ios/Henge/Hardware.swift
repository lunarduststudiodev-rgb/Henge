import SwiftUI
import UIKit
import GameController

/// What this device is, for what the app says and suggests.
enum Device {
    static let isPad = UIDevice.current.userInterfaceIdiom == .pad
    static let name = isPad ? "iPad" : "iPhone"

    /// A Mac's screen that fills this device on its side: 1920 wide, as tall as the shape asks. iPads and
    /// squarer phones (iPhone Duo, about 1.46:1) would get bars above and below at 16:9. Phones wider
    /// than 16:9 come out at 1920×1080, as before.
    static var fittingScreen: (width: Int, height: Int) {
        let size = UIScreen.main.bounds.size
        let shape = max(size.width, size.height) / max(min(size.width, size.height), 1)
        return (1920, min(max(Int((1920 / shape / 8).rounded()) * 8, 1080), 1440))
    }

    /// Preserve the iPad's shape while reducing the virtual GPU's pixel work.
    static var performanceScreen: (width: Int, height: Int) {
        let fit = fittingScreen
        return (1600, Int((Double(fit.height) * 5 / 6 / 8).rounded()) * 8)
    }

    /// The panel itself, on its side: every physical pixel, and its real size in mm so that macOS
    /// sees a Retina density and draws at 2x (looks like half the pixels, sharp as the iPad's own UI).
    /// Every iPad with an M chip is 264 ppi.
    static var retinaScreen: (width: Int, height: Int) {
        let n = UIScreen.main.nativeBounds.size
        return (Int(max(n.width, n.height)) / 8 * 8, Int(min(n.width, n.height)) / 8 * 8)
    }
    static var retinaMM: (width: Int, height: Int) {
        let s = retinaScreen
        return (Int((Double(s.width) / 264 * 25.4).rounded()), Int((Double(s.height) / 264 * 25.4).rounded()))
    }

    /// How much memory to suggest for a new Mac: what leaves this device room for iOS.
    static var suggestedRAMGB: Int {
        let mb = ProcessInfo.processInfo.physicalMemory >> 20
        // m4 fork: an 8 GB iPad (7.4 GB usable) thrashes and kills the Mac at 6 GB: 16 GB devices only.
        // A 6 GB device (5.7 GB usable) could not build a 4 GB Mac, and installed a 3 GB one (#70); 4 GB devices, 2.
        return mb >= 14_000 ? 8 : mb >= 10_000 ? 6 : mb >= 7_000 ? 4 : mb >= 5_000 ? 3 : 2
    }

    /// The choices for a Mac's memory, in GB.
    static let memoryChoices = 1...16

    /// What a choice risks on this device, if anything. An install always gets at least 2 GB (Installer.install).
    static func memoryNote(_ gb: Int) -> String? {
        if gb < 4 { return "macOS has little room under 4 GB, so it can be very slow." }
        if memoryTooHigh(gb) {
            let totalMB = ProcessInfo.processInfo.physicalMemory >> 20
            return "Too much for this \(name) (\(totalMB) MiB physical RAM). Allocating \(gb) GB guest RAM plus JIT cache exceeds physical memory, causing severe iPadOS compression thrashing and 10–25s freezes. \(suggestedRAMGB) GB or less is strongly recommended."
        }
        return nil
    }

    /// More than this device can give a Mac without iPadOS closing Henge when memory runs short (8 Nov 2026:
    /// an 8 GB Mac on an iPad with 12 GB was closed four times in a day).
    static func memoryTooHigh(_ gb: Int) -> Bool { gb > suggestedRAMGB }

    /// The virtual CPU offers Armv8.5 instructions (core/sysreg.c), and macOS uses them. They run as
    /// they are, so the chip needs them too: A14, M1 and later, which is iPhone13,x and iPad13,x up.
    static let chipTooOld: Bool = {
        let model = HengeLog.deviceModel
        guard model.hasPrefix("iPhone") || model.hasPrefix("iPad"),
              let major = Int(model.drop { !$0.isNumber }.prefix { $0.isNumber }) else { return false }
        return major < 13
    }()

    static let chipTooOldText = "macOS won't run on this \(name). Henge needs an A14 or M1 chip or newer."
}

/// A keyboard attached to the device. iOS reports each key going down and coming up by its HID usage,
/// which is what the Mac's USB keyboard sends, so every key and modifier goes straight across.
enum HardwareKeys {
    private static var held: Set<UInt8> = []
    private static var lastKey: CFTimeInterval = 0

    /// With the on-screen keyboard's view holding the keyboard, iOS may also offer a hardware key as
    /// text, and again as it repeats. The Mac has the key already, and repeats it itself.
    static var keyJustPressed: Bool {
        held.contains { $0 < 0xe0 } || CACurrentMediaTime() - lastKey < 0.1
    }

    /// Sends the presses that are keys, and returns the rest.
    static func send(_ presses: Set<UIPress>, down: Bool) -> Set<UIPress> {
        var rest = Set<UIPress>()
        // A chord can arrive in one unordered Set. Press modifiers first and
        // release them last so the guest sees the intended shortcut.
        let ordered = presses.sorted { a, b in
            let aCode = a.key?.keyCode.rawValue ?? 0
            let bCode = b.key?.keyCode.rawValue ?? 0
            let aModifier = (0xe0...0xe7).contains(aCode)
            let bModifier = (0xe0...0xe7).contains(bCode)
            if aModifier != bModifier { return down ? aModifier : !aModifier }
            return aCode < bCode
        }
        for press in ordered {
            guard let raw = press.key?.keyCode.rawValue, raw >= 0x04, raw <= 0xe7 else {
                rest.insert(press)
                continue
            }
            let usage = UInt8(raw)
            henge_vm_key_hid(usage, down ? 1 : 0)
            if usage < 0xe0 { lastKey = CACurrentMediaTime() }
            if down, usage == 0x28 || usage == 0x58 { Keys.lastEnter = Date() } // Return, or Enter on a keypad
            if down {
                held.insert(usage)
            } else {
                held.remove(usage)
                if usage < 0xe0 { StickyKeys.shared.releaseAll() } // one key after a tap on ⌃, ⌥ or ⌘
            }
        }
        return rest
    }

    /// iPadOS can keep a key's release when it takes over, as it does for ⌘-Tab: the Mac must not
    /// go on thinking the key is down.
    static func releaseAll() {
        for usage in held { henge_vm_key_hid(usage, 0) }
        held.removeAll()
    }
}

/// Asks iPadOS to hand the pointer over, so a trackpad or mouse drives only the Mac's pointer. It does
/// that only for an app filling the screen. SwiftUI has no way to ask, so the answer is added to the
/// class of the window's root view controller, which is who iPadOS asks.
/// A locked pointer can't leave the app, so Universal Control can't take it back to the Mac it came from:
/// the session asks only where the iPad's pointer can't stand for the Mac's (zoomed in, another display).
enum PointerLock {
    /// iPadOS has granted it. It may not have yet (another app was in front when asked, or no pointer).
    static var isLocked: Bool {
        UIApplication.shared.connectedScenes.contains { $0.pointerLockState?.isLocked ?? false }
    }

    /// Asks again, as when a mouse arrives after the session started.
    static func refresh() { set(wanted) }

    private static var wanted = false
    private static var patched: Set<ObjectIdentifier> = []

    static func set(_ on: Bool) {
        wanted = on
        let windows = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.flatMap(\.windows)
        for window in windows {
            guard let root = window.rootViewController else { continue }
            let kind: AnyClass = type(of: root)
            if patched.insert(ObjectIdentifier(kind)).inserted {
                let answer: @convention(block) (UIViewController) -> Bool = { _ in wanted }
                class_replaceMethod(kind, #selector(getter: UIViewController.prefersPointerLocked), imp_implementationWithBlock(answer), "B@:")
            }
            root.setNeedsUpdateOfPrefersPointerLocked()
        }
    }

    /// iPadOS only locks the pointer to an app that fills the screen: in a window (Stage Manager, or iPadOS 26's windows)
    /// the request is ignored and the system pointer stays separate from the Mac's (#18). An iPhone never locks it, and
    /// its apps are always full screen, so the hint is for iPad only.
    static var refused: Bool {
        guard Device.isPad, wanted, GCMouse.current != nil else { return false }
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.filter { $0.activationState == .foregroundActive }
        return !scenes.isEmpty && !scenes.contains { $0.pointerLockState?.isLocked == true }
    }
}

/// A Magic Keyboard's trackpad or a mouse, while a Mac is running and iPadOS has the pointer locked to Henge.
/// GameController gives the raw motion, which macOS then accelerates as it does for its own mouse. Henge doesn't
/// scale it first: macOS's acceleration isn't linear, and a speed applied before it made the pointer jump (#20).
/// macOS's own Tracking speed (System Settings › Mouse) sets how fast it goes, as on a real Mac.
final class HardwareMouse {
    static let shared = HardwareMouse()
    static let reverseKey = "henge.mouse.reverse"

    private var observers: [NSObjectProtocol] = []
    private var mice: [GCMouse] = []
    private var buttonsDown: Set<Int32> = []
    private var restX: Float = 0, restY: Float = 0
    private var movedPressed: Float = 0 // how far the left button has been dragged since it went down
    private var scrollsLogged = 0 // the first few, to see what a device's raw values look like
    private var lastScrollFirst: Float = 0, lastScrollSecond: Float = 0
    private let scroller = MacScroller()
    private var scrollEnd: DispatchWorkItem?

    func start() {
        lastScrollFirst = 0
        lastScrollSecond = 0
        GCMouse.mice().forEach(attach)
        observers.append(NotificationCenter.default.addObserver(forName: .GCMouseDidConnect, object: nil, queue: .main) { [weak self] note in
            if let mouse = note.object as? GCMouse { self?.attach(mouse) }
            PointerLock.refresh()
        })
        observers.append(NotificationCenter.default.addObserver(forName: .GCMouseDidDisconnect, object: nil, queue: .main) { [weak self] note in
            guard let self, let mouse = note.object as? GCMouse else { return }
            self.releaseButtons()
            self.mice.removeAll { $0 === mouse }
        })
        observers.append(NotificationCenter.default.addObserver(forName: UIPointerLockState.didChangeNotification, object: nil, queue: .main) { [weak self] note in
            // The scene that changed comes in the user info: the notification's object isn't it (PR #35).
            let scene = note.userInfo?[UIPointerLockState.sceneUserInfoKey] as? UIScene
            let locked = scene?.pointerLockState?.isLocked ?? PointerLock.isLocked
            HengeLog.log("input", "pointer lock: \(locked ? "on" : "off")")
            self?.releaseButtons() // whatever was pressed went to the other path
            self?.stopScrolling()
        })
    }

    func stop() {
        PointerLock.set(false)
        releaseButtons()
        stopScrolling()
        observers.forEach(NotificationCenter.default.removeObserver)
        observers.removeAll()
        for mouse in mice {
            guard let input = mouse.mouseInput else { continue }
            input.mouseMovedHandler = nil
            input.leftButton.pressedChangedHandler = nil
            input.rightButton?.pressedChangedHandler = nil
            input.middleButton?.pressedChangedHandler = nil
            input.scroll.valueChangedHandler = nil
        }
        mice.removeAll()
    }

    func releaseButtons() {
        for button in buttonsDown { henge_vm_mouse_button(button, 0) }
        buttonsDown.removeAll()
    }

    private func attach(_ mouse: GCMouse) {
        guard let input = mouse.mouseInput, !mice.contains(where: { $0 === mouse }) else { return }
        mice.append(mouse)
        HengeLog.log("input", "mouse: \(mouse.vendorName ?? "?"), right button \(input.rightButton != nil), middle \(input.middleButton != nil)")
        input.mouseMovedHandler = { [weak self] _, dx, dy in self?.moved(dx, dy) }
        input.leftButton.pressedChangedHandler = { [weak self] _, _, pressed in self?.button(0, pressed) }
        input.rightButton?.pressedChangedHandler = { [weak self] _, _, pressed in self?.button(1, pressed) }
        input.middleButton?.pressedChangedHandler = { [weak self] _, _, pressed in self?.button(2, pressed) }
        input.scroll.valueChangedHandler = { [weak self] _, first, second in self?.scrolled(first, second) }
    }

    /// GameController's Y runs upward. What doesn't make a whole step yet waits for the next move.
    /// Only while the pointer is locked: otherwise the iPad's own pointer is the Mac's (PointerInput).
    private func moved(_ dx: Float, _ dy: Float) {
        guard PointerLock.isLocked else { return }
        if buttonsDown.contains(0) { movedPressed += abs(dx) + abs(dy) }
        restX += dx
        restY -= dy
        let x = restX.rounded(), y = restY.rounded()
        guard x != 0 || y != 0 else { return }
        restX -= x
        restY -= y
        henge_vm_mouse_move(Int32(x), Int32(y))
    }

    private func button(_ button: Int32, _ pressed: Bool) {
        guard PointerLock.isLocked || !pressed else { return }
        guard pressed || buttonsDown.contains(button) else { return }
        if pressed {
            stopScrolling() // a scroll still waiting to end doesn't fling after the click
            MacScroller.stopGlide() // the click waits for it (the input fence), so it lands where the page stopped
        }
        henge_vm_mouse_button(button, pressed ? 1 : 0)
        if pressed {
            buttonsDown.insert(button)
            if button == 0 { movedPressed = 0 }
        } else {
            buttonsDown.remove(button)
            StickyKeys.shared.releaseAll()
            // A click, not a drag: in a field, it brings the keyboard back.
            if button == 0, movedPressed < 8 { KeyboardOwner.shared.clicked() }
        }
    }

    // MARK: Scrolling

    /// A scroll ends when no more comes for this long: a trackpad's two fingers send 60 to 120 a second.
    private static let scrollPause: TimeInterval = 0.06

    /// The Mac's points a page moves for each of GameController's scroll units, at Scroll speed 1: a unit is about
    /// one of the iPad's points of finger travel, and the Mac's screen fills the iPad, so the page follows the
    /// fingers as an iPad page does. A small move moves a little, a fast one far, and lifting the fingers while
    /// they move flings the page on (#38: a square root of each event's size, as wheel units, made every small
    /// move a jump, and macOS then sped those units up again).
    private var pointsPerUnit: CGFloat {
        let ipad = UIScreen.main.bounds.size
        let mac = MacScreen.points
        return mac.width / max(max(ipad.width, ipad.height), 1)
    }

    /// GameController puts vertical motion on the first axis and horizontal on the second, as the fingers move
    /// (natural scrolling: the page follows them). A gesture is a stream of these with no end marked, so it ends
    /// once they pause.
    private func scrolled(_ first: Float, _ second: Float) {
        guard PointerLock.isLocked else { return }
        // GameController reports both axis states when either one changes. On a
        // Magic Keyboard it calls us twice per movement, with one stale axis in
        // each call; sending both values doubles the scroll and adds sideways drift.
        let verticalChanged = first != lastScrollFirst
        let horizontalChanged = second != lastScrollSecond
        lastScrollFirst = first
        lastScrollSecond = second
        let mostlyVertical = abs(first) > 2 * abs(second)
        let mostlyHorizontal = abs(second) > 2 * abs(first)
        let vertical = verticalChanged && !mostlyHorizontal && first.isFinite ? CGFloat(-first) : 0
        let horizontal = horizontalChanged && !mostlyVertical && second.isFinite ? CGFloat(-second) : 0
        let way: CGFloat = UserDefaults.standard.bool(forKey: Self.reverseKey) ? -1 : 1
        let scale = pointsPerUnit * ScrollSpeed.factor * way
        let d = CGPoint(x: horizontal * scale, y: vertical * scale)
        if scrollsLogged < 24 {
            scrollsLogged += 1
            HengeLog.log("input", "scroll raw: \(first), \(second); Mac points: \(Int(d.x.rounded())), \(Int(d.y.rounded()))")
        }
        guard d != .zero else { return }
        let now = CACurrentMediaTime()
        scroller.scroll(by: d, at: now) // a page still gliding stops as it begins, as under a trackpad
        scrollEnd?.cancel()
        let end = DispatchWorkItem { [weak self] in
            // A trackpad's stream of moves flings the page on; a wheel's notch never does.
            self?.scroller.end(gliding: true, at: now, fling: 3)
        }
        scrollEnd = end
        DispatchQueue.main.asyncAfter(deadline: .now() + Self.scrollPause, execute: end)
    }

    private func stopScrolling() {
        scrollEnd?.cancel()
        scrollEnd = nil
        scroller.end(gliding: false, at: CACurrentMediaTime())
    }
}

/// Under a Mac's memory choice: what it risks, a warning in its colour when it is too much for this device.
struct MemoryNote: View {
    let gb: Int

    var body: some View {
        if let note = Device.memoryNote(gb) {
            let high = Device.memoryTooHigh(gb)
            HStack(alignment: .firstTextBaseline, spacing: 6) {
                if high { Image(systemName: "exclamationmark.triangle.fill") }
                Text(note).fixedSize(horizontal: false, vertical: true)
            }
            .font(.system(size: 13, weight: high ? .medium : .regular))
            .foregroundStyle(high ? Desk.warn : Desk.dim)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}
