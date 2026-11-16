import UIKit

/// A mouse, a Magic Keyboard's trackpad, or a Mac's pointer brought over with Universal Control, while
/// iPadOS shows its own pointer (not locked). The iPad's pointer is the Mac's: over the Mac's screen the
/// Mac's pointer is exactly where it is, through the touch tablet, so it crosses the screen's edges freely
/// and Universal Control can take it back to the Mac. Buttons and scrolling work where it points.
///
/// While the pointer is locked (zoomed in, or the Mac on another display) iPadOS sends none of this and
/// HardwareMouse moves the Mac's pointer from the raw motion instead. Fingers and the Pencil are DirectTouch's.
final class PointerInput: NSObject, UIGestureRecognizerDelegate {
    private weak var view: UIView?
    /// The Mac's screen in the view's points.
    private var screen: () -> CGRect = { .zero }

    private var held: Set<Int32> = []      // tablet buttons down: 0 left, 1 right, 2 middle
    private var pressing = false           // a press that began over the Mac's screen, until it lets go
    private var pressedOnTablet = false    // which device it went to: let go on the same one
    private var tapCandidate = false       // a press with no physical button down (tap-to-click candidate)
    private var spot = CGPoint.zero        // where the Mac's pointer was put last, 0...1 across the screen
    private var pressSpot = CGPoint.zero   // where it was when the left button went down
    private var lastHover: CGPoint?        // for the relative mouse, before macOS has the tablet
    private var rest = CGPoint.zero        // relative motion not yet sent
    private let scroller = MacScroller()
    private var scrolling = false          // a scroll that began over the Mac's screen, until it ends
    private var scrollSpot = CGPoint.zero  // what is being scrolled, 0...1 across the screen
    private var scrollLast = CGPoint.zero  // the pan's translation so far
    private var hovering = false           // the pointer is over the Mac's screen
    private var logged = 0                 // scrolls not taken, for the log

    private var trackpadScroll: UIPanGestureRecognizer?
    private var wheelScroll: UIPanGestureRecognizer?
    private var pinch: UIPinchGestureRecognizer?
    private var twoFingerTap: UITapGestureRecognizer?
    private var twoFingerDoubleTap: UITapGestureRecognizer?
    private var threeFingerTap: UITapGestureRecognizer?

    private var pinching = false
    private var zoomSteps = 0
    private static let zoomStep: CGFloat = 1.25 // how far apart the fingers go for each ⌘+

    func attach(to view: UIView, screen: @escaping () -> CGRect) {
        self.view = view
        self.screen = screen
        let hover = UIHoverGestureRecognizer(target: self, action: #selector(hovered(_:)))
        let buttons = PointerButtons(owner: self)
        // A trackpad's two-finger scroll glides when it lets go; a wheel's notches don't.
        let trackpadScroll = UIPanGestureRecognizer(target: self, action: #selector(scrolled(_:)))
        trackpadScroll.allowedScrollTypesMask = .continuous
        let wheelScroll = UIPanGestureRecognizer(target: self, action: #selector(scrolled(_:)))
        wheelScroll.allowedScrollTypesMask = .discrete
        for scroll in [trackpadScroll, wheelScroll] {
            scroll.allowedTouchTypes = [] // scrolling only: no touch drags it
        }

        let pinch = UIPinchGestureRecognizer(target: self, action: #selector(pinched(_:)))
        pinch.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]

        let twoFingerTap = UITapGestureRecognizer(target: self, action: #selector(twoFingerTapped(_:)))
        twoFingerTap.numberOfTouchesRequired = 2
        twoFingerTap.numberOfTapsRequired = 1
        twoFingerTap.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]

        let twoFingerDoubleTap = UITapGestureRecognizer(target: self, action: #selector(twoFingerDoubleTapped(_:)))
        twoFingerDoubleTap.numberOfTouchesRequired = 2
        twoFingerDoubleTap.numberOfTapsRequired = 2
        twoFingerDoubleTap.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]

        let threeFingerTap = UITapGestureRecognizer(target: self, action: #selector(threeFingerTapped(_:)))
        threeFingerTap.numberOfTouchesRequired = 3
        threeFingerTap.numberOfTapsRequired = 1
        threeFingerTap.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]

        twoFingerTap.require(toFail: twoFingerDoubleTap)

        self.trackpadScroll = trackpadScroll
        self.wheelScroll = wheelScroll
        self.pinch = pinch
        self.twoFingerTap = twoFingerTap
        self.twoFingerDoubleTap = twoFingerDoubleTap
        self.threeFingerTap = threeFingerTap

        for recognizer in [hover, buttons, trackpadScroll, wheelScroll, pinch] {
            recognizer.cancelsTouchesInView = false
            recognizer.delaysTouchesBegan = false
            recognizer.delaysTouchesEnded = false
            recognizer.delegate = self
            view.addGestureRecognizer(recognizer)
        }
        for tap in [twoFingerTap, twoFingerDoubleTap, threeFingerTap] {
            tap.delegate = self
            view.addGestureRecognizer(tap)
        }
    }

    // MARK: Gesture delegate arbitration

    func gestureRecognizer(_ a: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith b: UIGestureRecognizer) -> Bool {
        // Hover and PointerButtons (which only inspects touches without recognizing) can run with anything
        if a is UIHoverGestureRecognizer || b is UIHoverGestureRecognizer { return true }
        if a is PointerButtons || b is PointerButtons { return true }

        // Never allow scrolling to run simultaneously with pinch or multi-touch taps
        if (a is UIPinchGestureRecognizer && b is UIPanGestureRecognizer) ||
           (a is UIPanGestureRecognizer && b is UIPinchGestureRecognizer) {
            return false
        }
        if a is UITapGestureRecognizer || b is UITapGestureRecognizer {
            return false
        }
        if a is UIPinchGestureRecognizer || b is UIPinchGestureRecognizer {
            return false
        }
        return false
    }

    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, shouldRequireFailureOf otherGestureRecognizer: UIGestureRecognizer) -> Bool {
        // Single 2-finger tap waits for 2-finger double tap to fail
        if gestureRecognizer === twoFingerTap && otherGestureRecognizer === twoFingerDoubleTap {
            return true
        }
        // Multi-touch taps should not fire if trackpad scrolling or pinch is recognized
        if gestureRecognizer === twoFingerTap || gestureRecognizer === twoFingerDoubleTap {
            if otherGestureRecognizer === trackpadScroll || otherGestureRecognizer === pinch {
                return true
            }
        }
        if gestureRecognizer === threeFingerTap && (otherGestureRecognizer === trackpadScroll || otherGestureRecognizer === pinch) {
            return true
        }
        return false
    }

    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, shouldBeRequiredToFailBy otherGestureRecognizer: UIGestureRecognizer) -> Bool {
        if gestureRecognizer === twoFingerDoubleTap && otherGestureRecognizer === twoFingerTap {
            return true
        }
        if (gestureRecognizer === trackpadScroll || gestureRecognizer === pinch) &&
           (otherGestureRecognizer === twoFingerTap || otherGestureRecognizer === twoFingerDoubleTap || otherGestureRecognizer === threeFingerTap) {
            return true
        }
        return false
    }

    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, canPrevent otherGestureRecognizer: UIGestureRecognizer) -> Bool {
        // Trackpad scroll must NEVER prevent pinch
        if gestureRecognizer === trackpadScroll && otherGestureRecognizer === pinch {
            return false
        }
        if gestureRecognizer === pinch && otherGestureRecognizer === trackpadScroll {
            return true
        }
        return true
    }

    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, canBePreventedBy otherGestureRecognizer: UIGestureRecognizer) -> Bool {
        // Trackpad scroll CAN be prevented by pinch
        if gestureRecognizer === trackpadScroll && otherGestureRecognizer === pinch {
            return true
        }
        // Pinch must NEVER be prevented by trackpad scroll
        if gestureRecognizer === pinch && otherGestureRecognizer === trackpadScroll {
            return false
        }
        return true
    }

    // MARK: Where it points

    private var locked: Bool { view?.window?.windowScene?.pointerLockState?.isLocked ?? false }

    /// macOS has taken the tablet; until then (recoveryOS, early in startup) the pointer moves as a mouse.
    private var absolute: Bool { henge_vm_touch_ready() != 0 }

    private func over(_ p: CGPoint) -> Bool { screen().contains(p) }

    /// A point as 0...1 across the Mac's screen. A drag that leaves it holds at its edge.
    private func unit(_ p: CGPoint) -> CGPoint {
        let s = screen()
        return CGPoint(x: min(max((p.x - s.minX) / max(s.width, 1), 0), 1),
                       y: min(max((p.y - s.minY) / max(s.height, 1), 0), 1))
    }

    /// The Mac's points (what its pages scroll by) for each of the view's points.
    private var macPointsPerPoint: CGFloat {
        var w: Int32 = 0, h: Int32 = 0
        guard henge_vm_screen_points(&w, &h) == 0, w > 0 else { return 1 }
        return CGFloat(w) / max(screen().width, 1)
    }

    private func point(at p: CGPoint) {
        guard absolute else { return nudge(to: p) }
        spot = unit(p)
        henge_vm_hide_pointer(0) // a finger may have hidden it
        henge_vm_touch(spot.x, spot.y)
    }

    /// Before the tablet: the pointer moves by as much as the iPad's did (macOS then accelerates it).
    private func nudge(to p: CGPoint) {
        defer { lastHover = p }
        guard let from = lastHover else { return }
        let scale = macPointsPerPoint
        rest.x += (p.x - from.x) * scale
        rest.y += (p.y - from.y) * scale
        let x = rest.x.rounded(), y = rest.y.rounded()
        guard x != 0 || y != 0 else { return }
        rest.x -= x
        rest.y -= y
        henge_vm_mouse_move(Int32(x), Int32(y))
    }

    @objc private func hovered(_ hover: UIHoverGestureRecognizer) {
        guard let view, !locked else { return }
        if hover.zOffset > 0 { return } // the Pencil above the glass: DirectTouch's
        let p = hover.location(in: view)
        switch hover.state {
        case .began where over(p), .changed where over(p):
            hovering = true
            point(at: p)
        default:
            hovering = false
            lastHover = nil
        }
    }

    // MARK: Buttons

    /// The buttons a press holds now, as the tablet numbers them. A tap on a trackpad clicks with none.
    fileprivate func pressed(_ touch: UITouch, _ mask: UIEvent.ButtonMask, touchCount: Int, began: Bool) {
        guard let view, !locked, !pinching else { return cancelled() }
        let p = touch.location(in: view)
        if mask.isEmpty && (touchCount > 1 || pinching) {
            cancelled()
            return
        }
        if began, !pressing {
            guard over(p) else { return }
            pressing = true
            pressedOnTablet = absolute
            point(at: p)
            pressSpot = spot
            if mask.isEmpty {
                tapCandidate = true
            } else {
                tapCandidate = false
                MacScroller.stopGlide() // the click waits for it (the input fence), so it lands where the page stopped
            }
        }
        guard pressing else { return }
        var now: Set<Int32> = []
        if mask.contains(.primary) { now.insert(0) }
        if mask.contains(.secondary) { now.insert(1) }
        if mask.contains(.button(3)) { now.insert(2) }
        point(at: p)
        for button in held.subtracting(now).sorted() { send(button, down: false) }
        for button in now.subtracting(held).sorted() { send(button, down: true) }
        held = now
    }

    fileprivate func moved(_ touch: UITouch) {
        guard pressing, let view, !locked else { return }
        let p = touch.location(in: view)
        point(at: p)
        if tapCandidate && (abs(spot.x - pressSpot.x) + abs(spot.y - pressSpot.y) > 0.01) {
            tapCandidate = false
        }
    }

    fileprivate func lifted(_ touch: UITouch?) {
        guard pressing else { return }
        if let touch, let view, !locked, pressedOnTablet { point(at: touch.location(in: view)) }
        release()
    }

    fileprivate func cancelled() {
        guard pressing || !held.isEmpty || tapCandidate else { return }
        for button in held.sorted() { send(button, down: false) }
        held.removeAll()
        pressing = false
        tapCandidate = false
        StickyKeys.shared.releaseAll()
    }

    private func release() {
        guard pressing else { return }
        let isTap = tapCandidate && abs(spot.x - pressSpot.x) + abs(spot.y - pressSpot.y) < 0.01
        let leftClick = held.contains(0) && abs(spot.x - pressSpot.x) + abs(spot.y - pressSpot.y) < 0.01

        if isTap {
            MacScroller.stopGlide()
            send(0, down: true)
            send(0, down: false)
            KeyboardOwner.shared.clicked(at: pressedOnTablet ? spot : nil)
        } else {
            for button in held.sorted() { send(button, down: false) }
            if leftClick { KeyboardOwner.shared.clicked(at: pressedOnTablet ? spot : nil) }
        }

        held.removeAll()
        pressing = false
        tapCandidate = false
        StickyKeys.shared.releaseAll() // as a mouse's click ends a held ⌘ or ⇧ from the on-screen keys
    }

    /// At the spot the pointer was just put, on the device the press began on.
    private func send(_ button: Int32, down: Bool) {
        if pressedOnTablet {
            henge_vm_touch_button(spot.x, spot.y, button, down ? 1 : 0)
        } else {
            henge_vm_mouse_button(button, down ? 1 : 0)
        }
    }

    // MARK: Pinch to zoom

    @objc private func pinched(_ pinch: UIPinchGestureRecognizer) {
        guard let view, !locked else {
            pinching = false
            zoomSteps = 0
            return
        }
        let at = pinch.location(in: view)
        switch pinch.state {
        case .began:
            guard hovering || over(at) else { return }
            cancelled()
            pinching = true
            zoomSteps = 0
            MacScroller.stopGlide()
            if scrolling {
                scrolling = false
                scroller.end(gliding: false, at: 0)
            }
            if over(at) { point(at: at) }
        case .changed:
            guard pinching else { return }
            guard pinch.scale > 0 else { return }
            let steps = Int((log(pinch.scale) / log(Self.zoomStep)).rounded(.towardZero))
            zoom(to: steps)
        case .ended, .cancelled, .failed:
            guard pinching else { return }
            pinching = false
            zoomSteps = 0
        default:
            break
        }
    }

    /// ⌘+ or ⌘− for each step the fingers spread or pinch, as most Mac apps zoom.
    private func zoom(to steps: Int) {
        while zoomSteps != steps {
            let zoomIn = steps > zoomSteps
            zoomSteps += zoomIn ? 1 : -1
            henge_vm_key(Keys.command, 1)
            henge_vm_key(zoomIn ? 13 : 12, 1) // = and -
            henge_vm_key(zoomIn ? 13 : 12, 0)
            henge_vm_key(Keys.command, 0)
        }
    }

    // MARK: Multi-touch taps

    @objc private func twoFingerTapped(_ tap: UITapGestureRecognizer) {
        guard let view, !locked else { return }
        let p = tap.location(in: view)
        guard hovering || over(p) else { return }
        cancelled()
        MacScroller.stopGlide()
        if over(p) { point(at: p) }
        if absolute {
            henge_vm_touch_button(spot.x, spot.y, 1, 1)
            henge_vm_touch_button(spot.x, spot.y, 1, 0)
        } else {
            henge_vm_mouse_button(1, 1)
            henge_vm_mouse_button(1, 0)
        }
        StickyKeys.shared.releaseAll()
    }

    @objc private func twoFingerDoubleTapped(_ tap: UITapGestureRecognizer) {
        guard let view, !locked else { return }
        let p = tap.location(in: view)
        guard hovering || over(p) else { return }
        cancelled()
        MacScroller.stopGlide()
        if over(p) { point(at: p) }
        NotificationCenter.default.post(name: Trackpad.smartZoom, object: nil)
    }

    @objc private func threeFingerTapped(_ tap: UITapGestureRecognizer) {
        guard !locked else { return }
        cancelled()
        NotificationCenter.default.post(name: Trackpad.threeFingerTap, object: nil)
    }

    // MARK: Scrolling

    /// As the iPad's own pages scroll with this trackpad or wheel: the page follows the fingers (the iPad's
    /// scroll direction setting already applies), times Settings' Scroll speed, where the Mac's pointer is.
    /// MacScroller does the rest, as it does for the on-screen trackpad and a locked pointer. The pointer is the
    /// iPad's throughout: iPadOS doesn't move it while two fingers scroll, and whatever it does move it by is a real
    /// move of the pointer, shown on the Mac as it is on the iPad.
    @objc private func scrolled(_ pan: UIPanGestureRecognizer) {
        guard let view, !pinching else { return }
        let scale = macPointsPerPoint * ScrollSpeed.factor
        switch pan.state {
        case .began:
            let at = pan.location(in: view)
            guard !locked, hovering || over(at) else {
                note("pointer scroll not taken: \(locked ? "the pointer is locked" : "not over the Mac's screen")")
                return
            }
            scrolling = true
            scrollSpot = hovering && absolute ? spot : unit(at)
            scrollLast = .zero
            scroller.end(gliding: false, at: 0) // one not ended (it shouldn't be) stops here
            // A page still gliding stops as this scroll begins, as under a trackpad (MacScroller.scroll)
            fallthrough
        case .changed:
            guard scrolling, !pinching else { return }
            let t = pan.translation(in: view)
            let d = CGPoint(x: (t.x - scrollLast.x) * scale, y: (t.y - scrollLast.y) * scale)
            scrollLast = t
            scroller.scroll(by: d, at: CACurrentMediaTime(), from: scrollSpot)
        case .ended, .cancelled, .failed:
            guard scrolling else { return }
            scrolling = false
            // Only a trackpad that let go moving glides on; a wheel's notches don't.
            let v = pan.allowedScrollTypesMask == .continuous && pan.state == .ended ? pan.velocity(in: view) : .zero
            scroller.end(velocity: CGPoint(x: v.x * scale, y: v.y * scale))
        default:
            break
        }
    }

    /// The first few scrolls not taken, to see on a device why.
    private func note(_ line: @autoclosure () -> String) {
        guard logged < 30 else { return }
        logged += 1
        HengeLog.log("input", line())
    }
}

/// Watches the pointer's presses (touches of type .indirectPointer) without ever taking them: it never
/// recognizes, so nothing else on the view is held up or cancelled.
private final class PointerButtons: UIGestureRecognizer {
    private weak var owner: PointerInput?
    private var touch: UITouch?

    init(owner: PointerInput) {
        self.owner = owner
        super.init(target: nil, action: nil)
        allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent) {
        let indirectTouches = event.allTouches?.filter { $0.type == .indirectPointer } ?? touches
        if event.buttonMask.isEmpty && (indirectTouches.count > 1 || touches.count > 1) {
            cancel()
            return
        }
        guard let first = touches.first else { return }
        touch = touch ?? first
        owner?.pressed(first, event.buttonMask, touchCount: indirectTouches.count, began: true)
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent) {
        let indirectTouches = event.allTouches?.filter { $0.type == .indirectPointer } ?? touches
        if event.buttonMask.isEmpty && (indirectTouches.count > 1 || touches.count > 1) {
            cancel()
            return
        }
        guard let touch, touches.contains(touch) else { return }
        owner?.pressed(touch, event.buttonMask, touchCount: indirectTouches.count, began: false) // a second button pressed or let go mid-drag
        owner?.moved(touch)
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent) { finish(touches) }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent) { cancel() }

    private func cancel() {
        owner?.cancelled()
        self.touch = nil
        state = .failed
    }

    private func finish(_ touches: Set<UITouch>) {
        guard let touch, touches.contains(touch) else { return }
        owner?.lifted(touch)
        self.touch = nil
        state = .failed // back to .possible for the next press
    }

    override func reset() {
        if touch != nil { owner?.cancelled() }
        touch = nil
    }
}
