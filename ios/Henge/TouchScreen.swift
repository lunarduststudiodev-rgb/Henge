import UIKit

/// Fingers as a real touchscreen's, on macOS 27 and later: a USB touchscreen the Mac's side plugs in once the
/// helper has turned on macOS's support for one, so macOS takes each finger as a touch of its own and does what
/// it does with touch (taps, holds, drags off the Dock, scrolling, pinching), as on Apple's touch Mac.
///
/// Decided at the start of each gesture, never in the middle of one: when macOS has the touchscreen (the
/// helper says the screen has touch), the gesture's fingers go there; otherwise (older macOS, no helper, the
/// login window, anything not working) DirectTouch imitates touch with the pointer, as before.
///
/// Having it isn't enough: macOS can have the touchscreen and drop every touch (27.2 does when it finds no screen
/// for it). So as each gesture starts the helper is asked whether macOS made anything of it; if it didn't, the
/// touchscreen is pulled out and touch is imitated from the next gesture on, until the Mac restarts. And the person
/// can turn it off (Native Touchscreen, in the session's settings and its controls' touch menu).
///
/// Apple Pencil stays a precise pointer (DirectTouch), as with Sidecar: when it lands, the fingers' touches end
/// as a resting palm does (not as a tap), and the Pencil takes over. Henge's own three-finger tap still shows the
/// session's controls; its touches end the same way, so macOS doesn't act on them too.
final class TouchScreen {
    /// The Mac's screen in the surface's points. If it moves or changes size during a gesture (the iPad turns, the
    /// keyboard comes up, the window is resized), the fingers would jump to other places on the Mac: the gesture
    /// ends there as cancelled, as a resting palm's does, and the next one starts afresh.
    var screen = CGRect(x: 0, y: 0, width: 1, height: 1) {
        didSet { if screen != oldValue, active { giveWay() } }
    }
    /// A gesture is under way here: every finger goes here until the last one lifts.
    var active: Bool { !contacts.isEmpty }

    private struct Contact {
        let touch: UITouch
        let id: Int32
        var at: CGPoint      // 0...1 across the Mac's screen
    }

    private static let tapTime: TimeInterval = 0.35
    private static let slop: CGFloat = 10

    private var contacts: [Contact] = []
    private var startTime: TimeInterval = 0
    private var mostTouches = 0
    private var travelled: CGFloat = 0
    private var startAt: [UITouch: CGPoint] = [:]
    private var over = false               // given way (the Pencil landed): the fingers' touches have ended

    /// The person's switch: Settings' "Native Touchscreen". Off, the Mac never sees a touchscreen, and touch is imitated
    /// with the pointer, as on older macOS.
    static let enabledKey = "henge.touch.screen"
    static var enabled: Bool { UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? true }

    /// Native (Always) rather than Native (Auto): the touchscreen is used whenever macOS has it plugged in, with no
    /// check that macOS responds, and Henge never goes back to the pointer by itself.
    static let alwaysKey = "henge.touch.screen.always"
    static var always: Bool { UserDefaults.standard.bool(forKey: alwaysKey) }
    static func applyAlways() {
        HengeLog.log("touch", always ? "touchscreen: Native (Always): no switching back to the pointer"
                                     : "touchscreen: Native (Auto): back to the pointer if macOS doesn't respond")
        henge_vm_touchscreen_always(always ? 1 : 0)
    }

    /// What touch is doing, for the session to say (host/vm.h's HV_VM_TS_*).
    enum Status: Int32 { case none = 0, off, waiting, trying, works, ignored }
    static var status: Status { Status(rawValue: henge_vm_touchscreen_status()) ?? .none }

    private static var wasUsable: Bool?
    /// macOS has the touchscreen, says so, and hasn't been seen to ignore its touches.
    static var usable: Bool {
        let now = henge_vm_touchscreen_usable() != 0
        if now != wasUsable {
            wasUsable = now
            HengeLog.log("touch", now ? "touchscreen: macOS has Henge's touchscreen, so fingers are real touches"
                                      : "touchscreen: macOS hasn't a touchscreen from Henge, so touch is imitated with the pointer")
        }
        return now
    }

    /// Whether these touches start a touchscreen gesture: fingers (not the Pencil), and macOS has it.
    static func takes(_ touches: Set<UITouch>) -> Bool {
        touches.contains { $0.type == .direct } && !touches.contains { $0.type == .pencil } && enabled && usable
    }

    /// Before the session starts, or as the screen changes size: what macOS is told the screen measures.
    static func configure(screenPoints: CGSize) {
        // An iPad shows about 132 points to the inch (264 pixels at 2x; the mini more, which only makes the Mac
        // think its screen a little larger than it is).
        let mm = { (points: CGFloat) in Int32((points / 132 * 25.4).rounded()) }
        guard screenPoints.width > 0, screenPoints.height > 0 else { return }
        size = (mm(screenPoints.width), mm(screenPoints.height))
        henge_vm_touchscreen_config(enabled ? 1 : 0, size.w, size.h)
    }

    /// The switch changed: the Mac's side plugs the touchscreen in or pulls it out (a gesture under way on it ends there).
    static func applyEnabled() {
        HengeLog.log("touch", enabled ? "touchscreen: turned on in settings" : "touchscreen: turned off in settings: touch is imitated with the pointer")
        henge_vm_touchscreen_config(enabled ? 1 : 0, size.w, size.h)
    }
    /// Picked again after macOS was seen to ignore it: given another chance, as turning it off and on does.
    static func retry() {
        guard enabled else { return }
        HengeLog.log("touch", "touchscreen: tried again from the touch menu")
        henge_vm_touchscreen_config(0, size.w, size.h)
        henge_vm_touchscreen_config(1, size.w, size.h)
    }
    private static var size: (w: Int32, h: Int32) = (0, 0) // 0: the size the Mac's side has

    // MARK: Touches

    func began(_ touches: Set<UITouch>, in view: UIView) {
        let first = contacts.isEmpty
        if first {
            startTime = touches.map(\.timestamp).min() ?? ProcessInfo.processInfo.systemUptime
            mostTouches = 0
            travelled = 0
            startAt = [:]
            over = false
            henge_vm_hide_pointer(1) // the finger is the pointer: no arrow under it
        }
        guard !over else { return }
        if pausedNow() { return }
        // The helper checks macOS does something with the gesture (once it has ended as a touch: not one cancelled);
        // if not, the next is imitated with the pointer. Not one the Mac is paused for: nothing reaches macOS.
        if first { henge_vm_touchscreen_watch() }
        var changed = false
        for touch in touches where touch.type == .direct {
            guard let id = freeID() else { break } // ten fingers at most, as the touchscreen says
            let p = touch.location(in: view)
            contacts.append(Contact(touch: touch, id: id, at: unit(p)))
            startAt[touch] = p
            changed = true
        }
        mostTouches = max(mostTouches, contacts.count)
        if changed { send(lifted: [], cancelled: false) }
    }

    func moved(_ touches: Set<UITouch>, in view: UIView) {
        guard !over, !pausedNow() else { return }
        var changed = false
        for i in contacts.indices where touches.contains(contacts[i].touch) {
            let p = contacts[i].touch.location(in: view)
            let at = unit(p)
            if at != contacts[i].at {
                contacts[i].at = at
                changed = true
            }
            if let s = startAt[contacts[i].touch] { travelled = max(travelled, hypot(p.x - s.x, p.y - s.y)) }
        }
        if changed { send(lifted: [], cancelled: false) }
    }

    func ended(_ touches: Set<UITouch>, in view: UIView) {
        lift(touches, in: view, cancelled: false)
    }

    /// iPadOS took the touches (a swipe from the edge, say): they end as cancelled, not as touches.
    func cancelled(_ touches: Set<UITouch>, in view: UIView) {
        lift(touches, in: view, cancelled: true)
    }

    /// The Pencil landed: every finger's touch ends as a resting palm's does, and nothing more goes to the
    /// touchscreen until they have lifted. The Pencil goes to DirectTouch.
    func giveWay() {
        guard !over, !contacts.isEmpty else { return }
        send(lifted: contacts, cancelled: true, only: true)
        over = true
    }

    private func lift(_ touches: Set<UITouch>, in view: UIView, cancelled: Bool) {
        let gone = contacts.filter { touches.contains($0.touch) }
        guard !gone.isEmpty else { return }
        _ = pausedNow()
        contacts.removeAll { touches.contains($0.touch) }
        for c in gone { startAt[c.touch] = nil }
        if !over {
            var lifted = gone
            for i in lifted.indices { lifted[i].at = unit(lifted[i].touch.location(in: view)) }
            // The last of three or more fingers tapped together: Henge's controls, and macOS's touches end as
            // cancelled. (Earlier lifts of the same tap went as touches; macOS does nothing with three fingers
            // that lift without moving.)
            let liftTime = touches.map(\.timestamp).max() ?? startTime
            let threeFingerTap = contacts.isEmpty && mostTouches >= 3 && travelled < Self.slop * 3
                && liftTime - startTime < Self.tapTime * 2
            send(lifted: lifted, cancelled: cancelled || threeFingerTap)
            if threeFingerTap { NotificationCenter.default.post(name: Trackpad.threeFingerTap, object: nil) }
            // One finger, quick and still: a tap, which macOS takes as a click. What it landed in is asked of the
            // helper, so the keyboard can come up for a field before the Mac's focus has moved.
            if contacts.isEmpty, !cancelled, mostTouches == 1, travelled < Self.slop, liftTime - startTime < Self.tapTime,
               let at = lifted.first?.at {
                KeyboardOwner.shared.tapped(henge_share_tap(Double(at.x), Double(at.y)), lifted: liftTime, at: at)
            }
        }
        if contacts.isEmpty {
            over = false
            StickyKeys.shared.releaseAll() // a held modifier (on-screen keyboard) goes with one gesture, as with a click
        }
    }

    /// The Mac is paused: it can't take touches now, and they would all come at once when it goes on, so the
    /// gesture ends here, as cancelled where macOS last had its fingers, and the rest of it is left out.
    private func pausedNow() -> Bool {
        guard henge_vm_paused() != 0 else { return false }
        if over { return true }
        if contacts.isEmpty { over = true } else { giveWay() }
        return true
    }

    // MARK: Sending

    /// One frame: every finger down, and those lifted since the last frame (once). only: just the lifted ones
    /// (giving way: every finger is lifted).
    private func send(lifted: [Contact], cancelled: Bool, only: Bool = false) {
        var ids: [Int32] = [], down: [Int32] = [], cancel: [Int32] = [], xs: [Double] = [], ys: [Double] = []
        func add(_ c: Contact, _ isDown: Bool) {
            ids.append(c.id)
            down.append(isDown ? 1 : 0)
            cancel.append(!isDown && cancelled ? 1 : 0)
            xs.append(Double(c.at.x))
            ys.append(Double(c.at.y))
        }
        if !only { for c in contacts { add(c, true) } }
        for c in lifted { add(c, false) }
        guard !ids.isEmpty else { return }
        _ = henge_vm_touchscreen_frame(Int32(ids.count), ids, down, cancel, xs, ys)
    }

    private func freeID() -> Int32? {
        (0..<10).map(Int32.init).first { id in !contacts.contains { $0.id == id } }
    }

    /// A point on the surface as 0...1 across the Mac's screen. Beside the screen counts as its edge.
    private func unit(_ p: CGPoint) -> CGPoint {
        CGPoint(x: min(max((p.x - screen.minX) / max(screen.width, 1), 0), 1),
                y: min(max((p.y - screen.minY) / max(screen.height, 1), 0), 1))
    }
}
