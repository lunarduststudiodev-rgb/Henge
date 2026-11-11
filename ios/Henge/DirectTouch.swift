import UIKit

/// The finger is the pointer, as on Apple's touch Mac and in iPhone Mirroring. It works through the
/// Mac's absolute pointer (the engine's touch tablet), so the pointer is exactly where the finger is,
/// at once, with nothing to steer, and the pointer itself stays out of sight while a finger is on it.
///
/// One finger: a tap clicks where it lands, and a second tap there double-clicks. A button that shows
/// itself pressed (a dialog's, a checkbox, a window's close button) is pressed as the finger lands, and
/// clicks when it lifts, unless the finger slid off it first, as with a mouse. A drag on a title bar, a
/// slider, a divider or a menu takes hold of it at once; a drag anywhere else scrolls it, and a flick keeps
/// it gliding until a touch catches it (a touch that catches a glide only stops it, as on iPad).
///
/// Touch and hold: a ring shows around the finger (and an iPhone buzzes) and it right-clicks there and
/// then, so the menu is up while the finger is still down. Keep holding and move to drag what was held
/// instead: the menu closes first, and the drag starts once it has gone, so a Dock icon can be dragged
/// off the Dock, a file moved, or text selected. Without the helper (below) there's no telling whether a
/// menu opened, so a hold right-clicks when it lifts instead, and moving drags.
///
/// What is under the finger, whether a menu is showing, and scrolling as a trackpad scrolls (by the
/// pixel, rubber-banding at the ends, gliding as apps expect) come from the helper inside macOS
/// (tools/henge-agent.m), which needs macOS's Accessibility permission. Without it a swipe scrolls with
/// the wheel, and only a hold drags.
///
/// Two fingers: tapped together they right-click, moved together they scroll, and spread or pinched they
/// zoom (⌘+ and ⌘−). Three fingers tapped together show the session's controls. A one-finger drag or
/// scroll follows that finger only: another finger or a palm landing changes nothing, and the gesture ends
/// when the finger that started it lifts.
///
/// Apple Pencil is a precise mouse: it presses as it touches, so it clicks, drags and selects straight
/// away, and hovering moves the pointer. A double-tap or squeeze of the Pencil right-clicks where it points.
final class DirectTouch {
    /// The Mac's screen in the surface's points.
    var screen = CGRect(x: 0, y: 0, width: 1, height: 1)
    /// The machine's configured screen in pixels, used until the running Mac reports its own.
    var configured = CGSize(width: 1920, height: 1080)
    /// The Mac's screen in pixels as it runs now, which follows a size picked in Displays.
    var mac: CGSize {
        var w: Int32 = 0, h: Int32 = 0
        guard henge_vm_screen_size(&w, &h) == 0, w > 0, h > 0 else { return configured }
        return CGSize(width: Int(w), height: Int(h))
    }
    /// A Retina Mac draws two pixels for each of its points.
    var retina = false
    /// The Mac's screen in its own points, as it scrolls and moves things.
    private var macPoints: CGSize {
        var w: Int32 = 0, h: Int32 = 0
        if henge_vm_screen_points(&w, &h) == 0, w > 0, h > 0 { return CGSize(width: Int(w), height: Int(h)) }
        let m = mac, k: CGFloat = retina ? 2 : 1
        return CGSize(width: m.width / k, height: m.height / k)
    }
    /// A gesture is under way: every touch goes here until the last one lifts.
    var active: Bool { !touches.isEmpty }

    private enum Gesture {
        case pending      // one finger, not yet a tap, a swipe or a hold
        case pressing     // one finger on a button that shows itself pressed: the left button is down
        case held         // held still until it right-clicked (the helper will say if a menu opened): move to drag
        case holding      // held still, without the helper: right-clicks when it lifts, drags if it moves
        case leaving      // moved on from a hold: the menu closes, then the drag starts
        case deciding     // moved past the slop: a moment for the helper to say what is under the finger
        case dragging     // the left button is down and follows the finger or Pencil
        case scrolling
        case twoFingers   // two fingers, not yet a tap, a scroll or a pinch
        case zooming
        case threeFingers
        case over         // finished or cancelled: wait for every finger to lift
    }
    private enum Lock { case vertical, horizontal, free }

    /// Moving on from a hold: wait for the right-click's menu to have shown, see whether one did (against how
    /// many were showing as the finger landed), close it with its own Cancel action through the helper (as
    /// VoiceOver does: no key is pressed, so nothing else can take it), or with a guarded Escape where the
    /// helper can't, and wait for it to be gone (the Dock's fades out over about 0.18 s, and a press before
    /// then is swallowed), then press where the finger held.
    private enum Leave {
        case settling                                              // giving the menu time to show
        case counting(id: UInt32, asked: TimeInterval)             // how many menus are showing now
        case cancelling(id: UInt32, asked: TimeInterval)           // the helper asked to cancel it: did it?
        case closing(id: UInt32, asked: TimeInterval, since: TimeInterval) // closed: until it has gone
        case gone(at: TimeInterval)                                // a moment for the Mac to settle
        case pressed(at: TimeInterval)                             // down where it held: then to the finger
    }

    private static let holdTime: TimeInterval = 0.45
    private static let slop: CGFloat = 10                 // points a finger may wander and still tap or hold
    private static let tapTime: TimeInterval = 0.35       // fingers tapped together
    private static let doubleTapTime: TimeInterval = 0.5
    private static let doubleTapDistance: CGFloat = 30
    private static let zoomStep: CGFloat = 1.25           // how far apart the fingers go for each ⌘+
    /// How long a finger that has started moving waits for the helper's answer before it scrolls. Long
    /// enough for a busy Mac, short enough not to be felt.
    private static let decideWait: TimeInterval = 0.12
    /// A right-click's menu shows within this on a Mac that isn't struggling.
    private static let menuSettle: TimeInterval = 0.25
    /// The longest a drag from a hold waits for the menu to close before it presses anyway.
    private static let menuCloseWait: TimeInterval = 1.0
    /// A glide at least this fast (the Mac's points a second) is caught by a touch rather than tapped through.
    /// The helper says when its glide is (version 16); this is for the wheel's own glide, and its twin is in the helper.
    static let catchSpeed: CGFloat = 250
    /// After a flick, a touch before this (the touch's clock) is caught, even if the helper's "glide 1" hasn't
    /// come over the link yet. Set by the zoomed trackpad too.
    static var catchUntil: TimeInterval = 0

    private var gesture = Gesture.pending
    private var touches: [UITouch] = []
    private var primary: UITouch?             // the finger a one-finger gesture follows
    private var byPrimary = true              // the gesture follows the primary finger, not two fingers' middle
    private weak var surface: UIView?
    private var pencil = false
    private var start = CGPoint.zero          // where the gesture began, in points
    private var origin = CGPoint.zero         // where the tracked point was when this phase began
    private var startTime: TimeInterval = 0
    private var spot = CGPoint.zero           // where clicks go: 0...1 across the Mac's screen
    private var last = CGPoint.zero           // the tracked point (one finger, or two fingers' middle)
    private var travelled: CGFloat = 0
    private var mostTouches = 0
    private var startSpan: CGFloat = 1        // two fingers: how far apart they began
    private var zoomSteps = 0
    private var lock: Lock?
    private var wheel = MacWheel()            // without the helper
    private var samples: [(time: TimeInterval, at: CGPoint)] = []
    private var lastTap: (spot: CGPoint, at: CGPoint, time: TimeInterval)?
    private var holdTimer: Timer?
    private var heldAt = CGPoint.zero         // where the finger was when the hold registered
    private var rightClickedAt: TimeInterval = 0
    private var menuBase: UInt32 = 0          // the helper's count of menus as the finger landed (an id to ask about)
    private var menusBefore: Int32 = 0        // that count, once known (while a menu is being closed)
    private var leave = Leave.settling
    private var leaveTimer: Timer?
    private var pressTimer: Timer?            // waits on the helper to say whether a button is under the finger
    private var pressFrame = CGRect.zero      // that button, 0...1 across the screen
    private var pressAt = CGPoint.zero        // where the pointer is while pressing, 0...1
    private var glide: CADisplayLink?
    private var glideTime: CFTimeInterval = 0
    private var velocity = CGPoint.zero       // points a second
    private let buzz = UIImpactFeedbackGenerator(style: .medium)
    private var probes = 5                    // taps whose landing is checked in the log
    private var hitsLogged = 0                // the helper's first answers, and how long each took, go in the log
    private var hitAsked: TimeInterval = 0
    private var native = false                // the helper scrolls this gesture, as a trackpad
    private var hit: UInt32 = 0               // the helper's question about what is under the finger
    private var movedAt: TimeInterval = 0     // when the finger passed the slop
    private var decideTimer: Timer?
    private var scrollSpot = CGPoint.zero     // what is being scrolled, 0...1 across the screen
    private var caught = false                // the touch stopped a glide: lifting doesn't click
    private var pencilAt: CGPoint?            // where the Pencil last pointed (hovering or touching), in points

    private var uptime: TimeInterval { ProcessInfo.processInfo.systemUptime }

    // MARK: The device

    private static var wasReady: Bool?
    /// macOS has taken the touch device. Before it has (recoveryOS, or early in startup) the finger
    /// works as a trackpad instead.
    static var ready: Bool {
        let now = henge_vm_touch_ready() != 0
        if now != wasReady {
            wasReady = now
            HengeLog.log("touch", now ? "direct touch: the Mac has the touch device" : "direct touch: the Mac has no touch device yet, so touch is a trackpad")
        }
        return now
    }

    private static var helperWasReady: Bool?
    /// The helper in macOS is linked and allowed to control the Mac.
    static var helperReady: Bool {
        let now = henge_share_touch_ready() != 0
        if now != helperWasReady {
            helperWasReady = now
            HengeLog.log("touch", now ? "direct touch: the Mac's helper scrolls and finds title bars" : "direct touch: no helper in the Mac, so a swipe scrolls with the wheel")
        }
        return now
    }

    /// A touch landing now catches a glide (and so doesn't click): the helper says its glide is still quick, or
    /// a flick has only just been sent to it.
    static func glideCatches(at time: TimeInterval) -> Bool {
        henge_share_touch_gliding() != 0 || time < catchUntil
    }

    // MARK: Touches

    func began(_ touches: Set<UITouch>, in view: UIView) {
        surface = view
        var new = touches.filter { $0.type == .direct || $0.type == .pencil }
        // While the Pencil is down only the Pencil counts: a hand resting on the glass changes nothing.
        if !self.touches.isEmpty, pencil { new = new.filter { $0.type == .pencil } }
        // A hand that was already resting there, and hasn't done anything yet, gives way to the Pencil.
        if let pen = new.first(where: { $0.type == .pencil }), !pencil, !self.touches.isEmpty,
           [.pending, .held, .holding, .twoFingers].contains(gesture) {
            stopTimers()
            self.touches.removeAll()
            new = [pen]
        }
        guard let first = new.first else { return }
        if self.touches.isEmpty { begin(first, in: view) }
        self.touches.append(contentsOf: new)
        mostTouches = max(mostTouches, self.touches.count)
        if pencil { pencilAt = first.location(in: view) }
        guard !pencil, self.touches.count >= 2 else { return }
        switch gesture {
        case .pending, .deciding:
            // A second finger before the first did anything: a two- or three-finger gesture, from their middle.
            stopTimers()
            byPrimary = false
            gesture = self.touches.count >= 3 ? .threeFingers : .twoFingers
            last = tracked(in: view)
            origin = last
            samples = [(first.timestamp, last)]
            startSpan = max(span(in: view), 1)
        case .twoFingers where self.touches.count >= 3, .zooming where self.touches.count >= 3:
            gesture = .threeFingers
        default:
            // A one-finger press, hold, drag or scroll carries on under its own finger: another finger or a
            // palm landing beside it changes nothing.
            break
        }
    }

    func moved(_ touches: Set<UITouch>, in view: UIView) {
        if pencil, let pen = touches.first(where: { $0.type == .pencil }) { pencilAt = pen.location(in: view) }
        if byPrimary {
            guard let primary, touches.contains(primary) else { return }
        } else {
            guard touches.contains(where: self.touches.contains) else { return }
        }
        let time = (byPrimary ? primary : self.touches.first)?.timestamp ?? uptime
        let now = tracked(in: view)
        let delta = CGPoint(x: now.x - last.x, y: now.y - last.y)
        guard delta != .zero else { return }
        travelled += hypot(delta.x, delta.y)
        sample(now, time)
        switch gesture {
        case .pending:
            guard distance(now, origin) > Self.slop else { break }
            stopTimers()
            last = now
            movedAt = time
            gesture = .deciding
            decide()
            return
        case .deciding:
            last = now
            decide()
            return
        case .pressing:
            // Held down on a button, the pointer goes with the finger once it really moves: off the button,
            // macOS shows it unpressed, and letting go there doesn't click it, as with a mouse.
            if distance(now, start) > Self.slop {
                pressAt = unit(now)
                henge_vm_touch(pressAt.x, pressAt.y)
            }
        case .held:
            // The right-click's menu is up. Moving on closes it and drags what was held, as an iPad's context
            // menu gives way when what was held is dragged off.
            guard distance(now, heldAt) > Self.slop else { break }
            startLeaving()
        case .holding:
            // No right-click yet (no helper to say if a menu opens): a hold that moves is a drag from where it held.
            guard distance(now, heldAt) > Self.slop else { break }
            last = now
            startDrag()
            return
        case .leaving:
            break // the pointer stays where it held until the drag starts, then catches up with the finger
        case .dragging:
            henge_vm_touch(unit(now).x, unit(now).y)
        case .scrolling:
            scroll(delta)
        case .twoFingers:
            let ratio = span(in: view) / startSpan
            if self.touches.count >= 2, abs(log(ratio)) > 0.15, distance(now, origin) < 40 {
                gesture = .zooming
            } else if distance(now, origin) > Self.slop {
                last = now
                startScroll()
                return
            }
        case .zooming:
            guard self.touches.count >= 2 else { break } // one finger left: the zoom stays as it is
            zoom(to: Int((log(span(in: view) / startSpan) / log(Self.zoomStep)).rounded(.towardZero)))
        case .threeFingers, .over:
            break
        }
        last = now
    }

    func ended(_ touches: Set<UITouch>, in view: UIView) {
        let lifted = touches.filter(self.touches.contains)
        guard !lifted.isEmpty else { return }
        let liftTime = lifted.map(\.timestamp).max() ?? startTime
        let primaryLifted = primary.map(lifted.contains) ?? false
        if byPrimary, primaryLifted, let p = primary { last = p.location(in: view) }
        self.touches.removeAll(where: lifted.contains)
        if !self.touches.isEmpty {
            if byPrimary {
                // The finger the gesture followed has lifted, the others were only resting: it ends here, and
                // they do nothing more until they lift too.
                if primaryLifted, gesture != .over {
                    finish(liftTime)
                    gesture = .over
                }
            } else {
                // The others carry on from where they are, without a jump from the lifted finger.
                last = tracked(in: view)
                origin = last
                samples = [(liftTime, last)]
            }
            return
        }
        finish(liftTime)
    }

    /// The gesture's end, as its finger (or the last of its fingers) lifts.
    private func finish(_ liftTime: TimeInterval) {
        stopTimers()
        let quick = liftTime - startTime < Self.tapTime * (mostTouches >= 3 ? 2 : 1)
        switch gesture {
        case .pending:
            if caught { break } // the touch only stopped a glide, as on iPad
            click(0)
            lastTap = (spot, start, liftTime)
            KeyboardOwner.shared.tapped(hit, lifted: liftTime, at: spot) // the helper said what is there with the hit
            check(spot)
            return
        case .pressing:
            releasePress()
            if travelled <= Self.slop {
                lastTap = (spot, start, liftTime)
                KeyboardOwner.shared.tapped(hit, lifted: liftTime, at: spot)
                check(spot)
                return
            }
        case .deciding:
            // A quick flick, gone before the helper answered: what it has said by now, or a scroll.
            if henge_share_touch_answer(hit) == 1 {
                startDrag()
                release()
            } else {
                startScroll()
                endScroll(liftTime)
            }
        case .held:
            break // it right-clicked as the hold registered: the menu stays open to pick from
        case .holding:
            click(1) // held without moving, and no helper to say whether a menu would open: right-click now
        case .leaving:
            // Lifted before the drag began: what is done is done (the menu may have closed); a press lets go.
            if case .pressed = leave { release() }
        case .dragging:
            if pencil, travelled <= Self.slop {
                // A Pencil tap: let go where it pressed, so it's a click, and the next one can double.
                henge_vm_touch_button(spot.x, spot.y, 0, 0)
                StickyKeys.shared.releaseAll()
                lastTap = (spot, start, liftTime)
                return
            }
            release()
        case .scrolling:
            endScroll(liftTime)
        case .twoFingers:
            if quick, travelled < Self.slop * 2 { click(1) }
        case .threeFingers:
            if quick, travelled < Self.slop * 3 { NotificationCenter.default.post(name: Trackpad.threeFingerTap, object: nil) }
        case .zooming, .over:
            break
        }
        lastTap = nil
    }

    /// iPadOS took the touches (a swipe from the edge, say): nothing clicks, a pressed button lets go without
    /// clicking, and a drag drops where it is.
    func cancelled(_ touches: Set<UITouch>) {
        let lifted = touches.filter(self.touches.contains)
        guard !lifted.isEmpty else { return }
        self.touches.removeAll(where: lifted.contains)
        stopTimers()
        switch gesture {
        case .dragging:
            release()
        case .pressing:
            cancelPress()
        case .leaving:
            if case .pressed = leave { release() }
        case .scrolling:
            if native { henge_share_touch_scroll(4, scrollSpot.x, scrollSpot.y, 0, 0) }
        default:
            break
        }
        gesture = .over
        lastTap = nil
    }

    /// Apple Pencil hovering: the pointer follows it, so things under it show they can be clicked.
    func hover(_ at: CGPoint) {
        pencilAt = at
        guard touches.isEmpty else { return }
        let p = unit(at)
        henge_vm_hide_pointer(0) // a Pencil points precisely: the pointer shows where
        henge_vm_touch(p.x, p.y)
    }

    /// The Pencil was double-tapped or squeezed: a right-click where it points (hovering), or where it last was.
    func pencilRightClick(at point: CGPoint?) {
        guard touches.isEmpty, let at = point ?? pencilAt else { return }
        pencilAt = at
        let p = unit(at)
        henge_vm_hide_pointer(0)
        henge_vm_touch(p.x, p.y)
        henge_vm_touch_button(p.x, p.y, 1, 1)
        henge_vm_touch_button(p.x, p.y, 1, 0)
        StickyKeys.shared.releaseAll()
        lastTap = nil
    }

    private func begin(_ touch: UITouch, in view: UIView) {
        stopTimers()
        native = Self.helperReady
        // A touch catches a page that is still gliding, as on iPad: the glide stops, and nothing is clicked. Only
        // a glide quick enough to be seen moving: one that has all but stopped lets the tap through.
        let wheelGlide = (Self.wheelGliding?.stopGlide() ?? 0) * macPoints.height / max(screen.height, 1)
        caught = touch.type != .pencil && (wheelGlide >= Self.catchSpeed || (native && Self.glideCatches(at: touch.timestamp)))
        Self.catchUntil = 0
        if native { henge_share_touch_stop() } // early: a tap's click at the lift waits for what is left of it (the input fence)
        pencil = touch.type == .pencil
        primary = touch
        byPrimary = true
        start = touch.location(in: view)
        startTime = touch.timestamp
        origin = start
        last = start
        samples = [(touch.timestamp, start)]
        travelled = 0
        mostTouches = 0
        zoomSteps = 0
        lock = nil
        wheel.reset()
        leave = .settling
        menuBase = 0
        menusBefore = 0
        spot = unit(start)
        // A second tap goes where the first did, so the Mac counts a double click.
        if let tap = lastTap, touch.timestamp - tap.time < Self.doubleTapTime, distance(tap.at, start) < Self.doubleTapDistance {
            spot = tap.spot
        }
        hit = 0
        if pencil {
            henge_vm_hide_pointer(0)
            gesture = .dragging
            henge_vm_touch_button(spot.x, spot.y, 0, 1)
        } else {
            henge_vm_hide_pointer(1) // the finger is the pointer: no arrow under it, as on a touch Mac
            gesture = .pending
            henge_vm_touch(spot.x, spot.y) // the pointer is there at once, so what's under it lights up
            if native { // asked now, answered by the time the finger moves
                hit = henge_share_touch_hit(spot.x, spot.y)
                hitAsked = uptime
                if hit != 0, !caught { watchForPress() }
            }
            // How many menus are already up, so a hold can tell whether its right-click opened one (0 without a
            // helper of this version: a hold then waits for the lift to right-click).
            menuBase = henge_share_menu_ask()
            buzz.prepare()
            armHold()
        }
    }

    // MARK: Deciding

    /// A finger that set off: drag what it is on if the helper says that drags (a title bar, a slider, a
    /// divider, a menu), or scroll. Waits a moment for the answer if it hasn't come yet.
    private func decide() {
        let answer = henge_share_touch_answer(hit)
        if answer >= 0, hitsLogged < 20 {
            hitsLogged += 1
            let ms = Int((uptime - hitAsked) * 1000)
            HengeLog.log("touch", "the helper said \(answer == 1 ? "drag" : "scroll") by \(ms) ms after touching down")
        }
        if answer == 1 {
            stopDeciding()
            startDrag()
        } else if answer == 0 || hit == 0 || uptime - movedAt > Self.decideWait {
            stopDeciding()
            if answer < 0, hit != 0 { HengeLog.log("touch", "the helper was slow to say what was under a finger: scrolling") }
            startScroll()
        } else if decideTimer == nil {
            decideTimer = repeating(0.008) { [weak self] in
                guard let self, self.gesture == .deciding else { return }
                self.decide()
            }
        }
    }

    private func stopDeciding() {
        decideTimer?.invalidate()
        decideTimer = nil
    }

    /// Takes hold where the finger came down, and brings it to where the finger is now.
    private func startDrag() {
        gesture = .dragging
        henge_vm_touch_button(spot.x, spot.y, 0, 1)
        henge_vm_touch(unit(last).x, unit(last).y)
    }

    private func release() {
        henge_vm_touch_button(unit(last).x, unit(last).y, 0, 0)
        StickyKeys.shared.releaseAll()
    }

    // MARK: Buttons pressed as they're touched

    /// Until the helper answers (or the finger moves on, or holds): if what is under the finger is a button that
    /// shows itself pressed, it is pressed now, as a finger on a touch screen would, and clicks when it lifts.
    private func watchForPress() {
        pressTimer = repeating(0.008) { [weak self] in
            guard let self else { return }
            guard self.gesture == .pending, self.touches.count == 1, self.uptime - self.hitAsked < Self.holdTime else {
                return self.stopWatchingForPress()
            }
            guard henge_share_touch_answer(self.hit) >= 0 else { return } // not answered yet
            self.stopWatchingForPress()
            var x: Float = 0, y: Float = 0, w: Float = 0, h: Float = 0
            guard henge_share_touch_press(self.hit, &x, &y, &w, &h) != 0 else { return }
            let frame = CGRect(x: CGFloat(x), y: CGFloat(y), width: CGFloat(w), height: CGFloat(h))
            guard frame.contains(self.spot) else { return } // a double tap's second, moved to the first: not this button
            self.cancelHold()
            self.pressFrame = frame
            self.pressAt = self.spot
            self.gesture = .pressing
            henge_vm_touch_button(self.spot.x, self.spot.y, 0, 1)
        }
    }

    private func stopWatchingForPress() {
        pressTimer?.invalidate()
        pressTimer = nil
    }

    /// Lets go where the pointer is: on the button that's a click; off it, nothing, as a Mac's buttons do.
    private func releasePress() {
        henge_vm_touch_button(pressAt.x, pressAt.y, 0, 0)
        StickyKeys.shared.releaseAll()
    }

    /// Lets go off the button, so it doesn't click: just beside it, on whichever side the screen has room.
    private func cancelPress() {
        let f = pressFrame, gap: CGFloat = 0.01
        let candidates = [CGPoint(x: f.minX - gap, y: f.midY), CGPoint(x: f.maxX + gap, y: f.midY),
                          CGPoint(x: f.midX, y: f.minY - gap), CGPoint(x: f.midX, y: f.maxY + gap)]
        let away = candidates.first { $0.x >= 0 && $0.x <= 1 && $0.y >= 0 && $0.y <= 1 } ?? CGPoint(x: 0.5, y: 0.5)
        pressAt = away
        henge_vm_touch(away.x, away.y)
        releasePress()
    }

    // MARK: Touch and hold

    private func armHold() {
        holdTimer = once(Self.holdTime) { [weak self] in
            guard let self, self.gesture == .pending, self.touches.count == 1 else { return }
            self.heldAt = self.last
            self.buzz.impactOccurred()
            HoldRing.show(at: self.last, in: self.surface)
            if self.menuBase != 0 {
                // Right-click now, so the menu is there while the finger is still down.
                self.gesture = .held
                self.rightClickedAt = self.uptime
                self.click(1)
            } else {
                self.gesture = .holding
            }
        }
    }

    private func cancelHold() {
        holdTimer?.invalidate()
        holdTimer = nil
    }

    private func startLeaving() {
        gesture = .leaving
        leave = .settling
        leaveTimer?.invalidate()
        leaveTimer = repeating(0.01) { [weak self] in self?.leaveStep() }
        leaveStep()
    }

    private func stopLeaving() {
        leaveTimer?.invalidate()
        leaveTimer = nil
    }

    private func leaveStep() {
        guard gesture == .leaving else { return stopLeaving() }
        let t = uptime
        switch leave {
        case .settling:
            guard t - rightClickedAt >= Self.menuSettle else { return }
            let id = henge_share_menu_ask()
            if id == 0 { return pressFromHold() } // the helper has gone: don't risk an Escape with nothing to close
            leave = .counting(id: id, asked: t)
        case let .counting(id, asked):
            let showing = henge_share_menu_answer(id)
            guard showing >= 0 else {
                if t - asked > 0.3 { pressFromHold() } // no answer: a press is safe, an Escape might not be
                return
            }
            // Only a menu that the right-click opened is closed: one that was already showing as the finger
            // landed is the user's, and with none showing, an Escape would go to the window instead (and cancel
            // a dialog, leave full screen or undo a rename).
            let before = henge_share_menu_answer(menuBase)
            guard showing > 0, before < 0 || showing > before else { return pressFromHold() }
            menusBefore = max(before, 0) // kept: the answer is gone from the helper's few once more are asked
            let close = henge_share_menu_close()
            guard close != 0 else { return escapeMenu(at: t) }
            leave = .cancelling(id: close, asked: t)
        case let .cancelling(id, asked):
            let closed = henge_share_menu_answer(id)
            if closed > 0 {
                leave = .closing(id: henge_share_menu_ask(), asked: t, since: t)
            } else if closed == 0 {
                escapeMenu(at: t) // it couldn't (no Accessibility permission): the menu is known to be showing
            } else if t - asked > 1.0 {
                // No answer yet: it may still be closing it, so no Escape (one after the menu went would reach the
                // window). Watch for it to go; past menuCloseWait, the press is made as it is.
                leave = .closing(id: henge_share_menu_ask(), asked: t, since: t)
            }
        case let .closing(id, asked, since):
            if t - since > Self.menuCloseWait { return pressFromHold() }
            let showing = henge_share_menu_answer(id)
            if showing >= 0, showing <= menusBefore {
                leave = .gone(at: t)
            } else if (showing >= 0 && t - asked >= 0.04) || t - asked > 0.3 {
                leave = .closing(id: henge_share_menu_ask(), asked: t, since: since) // still fading: look again
            }
        case let .gone(at):
            if t - at >= 0.04 { pressFromHold() }
        case let .pressed(at):
            // A moment after the press, the pointer moves to the finger, as a mouse would: the drag begins.
            guard t - at >= 0.03 else { return }
            stopLeaving()
            gesture = .dragging
            henge_vm_touch(unit(last).x, unit(last).y)
        }
    }

    /// The menu the hold opened is showing, and the helper couldn't cancel it: Escape closes it.
    private func escapeMenu(at t: TimeInterval) {
        henge_vm_key(Keys.escape, 1)
        henge_vm_key(Keys.escape, 0)
        leave = .closing(id: henge_share_menu_ask(), asked: t, since: t)
    }

    /// Presses where the finger held, for the drag that follows.
    private func pressFromHold() {
        henge_vm_touch_button(spot.x, spot.y, 0, 1)
        leave = .pressed(at: uptime)
    }

    private func stopTimers() {
        cancelHold()
        stopDeciding()
        stopWatchingForPress()
        stopLeaving()
    }

    // MARK: Pointing

    /// A point on the surface as 0...1 across the Mac's screen. Beside the screen counts as its edge,
    /// so the Dock and the menu bar are easy to reach.
    private func unit(_ p: CGPoint) -> CGPoint {
        CGPoint(x: min(max((p.x - screen.minX) / max(screen.width, 1), 0), 1),
                y: min(max((p.y - screen.minY) / max(screen.height, 1), 0), 1))
    }

    /// The finger a one-finger gesture follows, or the middle of the first two.
    private func tracked(in view: UIView) -> CGPoint {
        if byPrimary {
            guard let primary, touches.contains(primary) else { return last }
            return primary.location(in: view)
        }
        guard let a = touches.first?.location(in: view) else { return last }
        guard touches.count >= 2 else { return a }
        let b = touches[1].location(in: view)
        return CGPoint(x: (a.x + b.x) / 2, y: (a.y + b.y) / 2)
    }

    private func span(in view: UIView) -> CGFloat {
        guard touches.count >= 2 else { return startSpan }
        return distance(touches[0].location(in: view), touches[1].location(in: view))
    }

    private func distance(_ a: CGPoint, _ b: CGPoint) -> CGFloat { hypot(a.x - b.x, a.y - b.y) }

    private func click(_ button: Int32) {
        henge_vm_touch_button(spot.x, spot.y, button, 1)
        henge_vm_touch_button(spot.x, spot.y, button, 0)
        StickyKeys.shared.releaseAll()
    }

    /// The log says where the first few taps landed, against where they were meant to.
    private func check(_ spot: CGPoint) {
        guard probes > 0 else { return }
        probes -= 1
        let want = CGPoint(x: (spot.x * (mac.width - 1)).rounded(), y: (spot.y * (mac.height - 1)).rounded())
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) {
            var x: Int32 = 0, y: Int32 = 0
            let got = henge_vm_pointer(&x, &y) == 0 ? "\(x),\(y)" : "not shown"
            HengeLog.log("touch", "tap meant for \(Int(want.x)),\(Int(want.y)): pointer at \(got)")
        }
    }

    // MARK: Timers

    /// In the run loop's common modes, so they keep time while a scroll view or a menu tracks elsewhere in the app.
    private func once(_ after: TimeInterval, _ body: @escaping () -> Void) -> Timer {
        let timer = Timer(timeInterval: after, repeats: false) { _ in body() }
        RunLoop.main.add(timer, forMode: .common)
        return timer
    }

    private func repeating(_ every: TimeInterval, _ body: @escaping () -> Void) -> Timer {
        let timer = Timer(timeInterval: every, repeats: true) { _ in body() }
        RunLoop.main.add(timer, forMode: .common)
        return timer
    }

    // MARK: Zooming

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

    // MARK: Scrolling

    /// From where the finger (or the middle of two) set off: all of the way so far, so the page keeps up.
    private func startScroll() {
        gesture = .scrolling
        scrollSpot = unit(origin)
        scroll(CGPoint(x: last.x - origin.x, y: last.y - origin.y), began: true)
    }

    /// The page follows the finger, pixel for pixel. Mostly one way at the start is that way only, as
    /// on iPad, so a page doesn't drift sideways and a browser doesn't go back.
    private func scroll(_ d: CGPoint, began: Bool = false) {
        if lock == nil {
            lock = abs(d.y) > 1.5 * abs(d.x) ? .vertical : abs(d.x) > 1.5 * abs(d.y) ? .horizontal : .free
        }
        if native {
            // The helper scrolls as a trackpad: phases, so apps rubber-band at the ends and glide after.
            let dx = lock == .vertical ? 0 : d.x / max(screen.width, 1)
            let dy = lock == .horizontal ? 0 : d.y / max(screen.height, 1)
            henge_share_touch_scroll(began ? 1 : 2, scrollSpot.x, scrollSpot.y, dx, dy)
            return
        }
        let scale = macPoints.height / max(screen.height, 1)
        wheel.scroll(by: CGPoint(x: lock == .vertical ? 0 : d.x * scale, y: lock == .horizontal ? 0 : d.y * scale))
    }

    private func sample(_ at: CGPoint, _ time: TimeInterval) {
        samples.append((time, at))
        samples.removeAll { time - $0.time > 0.1 }
    }

    /// How fast the finger was going as it lifted, in points a second: zero if it had stopped first.
    private func liftSpeed(_ liftTime: TimeInterval) -> CGPoint {
        guard let latest = samples.last, liftTime - latest.time < 0.05 else { return .zero }
        // A flick speeds up as it goes: its last few hundredths of a second are its speed, not the average since it set off.
        let first = samples.first { latest.time - $0.time <= 0.045 } ?? latest
        let dt = latest.time - first.time
        guard dt > 0.008 else { return .zero }
        var v = CGPoint(x: (latest.at.x - first.at.x) / dt, y: (latest.at.y - first.at.y) / dt)
        guard hypot(v.x, v.y) > 200 else { return .zero }
        v.x = lock == .vertical ? 0 : max(min(v.x, 8000), -8000)
        v.y = lock == .horizontal ? 0 : max(min(v.y, 8000), -8000)
        return v
    }

    /// A finger that left moving keeps the page going; one that stopped first doesn't.
    private func endScroll(_ liftTime: TimeInterval) {
        let v = liftSpeed(liftTime)
        guard native else { return flick(v) }
        henge_share_touch_scroll(4, scrollSpot.x, scrollSpot.y, v.x / max(screen.width, 1), v.y / max(screen.height, 1))
        // The helper says "glide 1" once it is gliding quickly enough to catch a touch; until that has had time
        // to come back over the link, a touch is caught on this side.
        let speed = hypot(v.x, v.y) * macPoints.height / max(screen.height, 1)
        if speed >= Self.catchSpeed { Self.catchUntil = liftTime + 0.12 }
    }

    private func flick(_ v: CGPoint) {
        guard v != .zero else { return }
        velocity = v
        Self.stopWheelGlide() // the Mac has one glide
        glideTime = CACurrentMediaTime()
        let link = CADisplayLink(target: GlideStep(self), selector: #selector(GlideStep.step))
        link.add(to: .main, forMode: .common)
        glide = link
        Self.wheelGliding = self
    }

    /// Whose wheel glide is going, without the helper: a mouse's or trackpad's press or scroll stops it as a finger's
    /// does (MacScroller.stopGlide). Its steps go to the Mac's mouse on this thread, as a click does, so a click
    /// sent after this lands where the page is.
    private static weak var wheelGliding: DirectTouch?
    static func stopWheelGlide() { wheelGliding?.stopGlide() }

    /// Slows as an iPad's own pages do (UIScrollView's normal rate, 0.998 a millisecond).
    fileprivate func glideStep() {
        let now = CACurrentMediaTime()
        let dt = min(now - glideTime, 0.05)
        glideTime = now
        scroll(CGPoint(x: velocity.x * dt, y: velocity.y * dt))
        let slow = pow(0.998, dt * 1000)
        velocity = CGPoint(x: velocity.x * slow, y: velocity.y * slow)
        if hypot(velocity.x, velocity.y) < 20 { stopGlide() }
    }

    /// Stops the wheel's own glide, if one is going: how fast it was (the surface's points a second), else 0.
    @discardableResult
    private func stopGlide() -> CGFloat {
        defer { glide = nil }
        guard let glide else { return 0 }
        if Self.wheelGliding === self { Self.wheelGliding = nil }
        glide.invalidate()
        return hypot(velocity.x, velocity.y)
    }
}

/// A display link holds its target strongly; this keeps it from holding the touch handler.
private final class GlideStep: NSObject {
    weak var owner: DirectTouch?
    init(_ owner: DirectTouch) { self.owner = owner }
    @objc func step() { owner?.glideStep() }
}

/// A ring that shows around a finger as a touch-and-hold registers. An iPhone buzzes as well, but an iPad has
/// nothing to feel, and the finger hides what is under it: this says the hold has taken.
enum HoldRing {
    private static let radius: CGFloat = 32

    static func show(at point: CGPoint, in view: UIView?) {
        guard let view else { return }
        let r = radius
        let ring = CAShapeLayer()
        ring.path = UIBezierPath(ovalIn: CGRect(x: -r, y: -r, width: 2 * r, height: 2 * r)).cgPath
        ring.bounds = CGRect(x: -r, y: -r, width: 2 * r, height: 2 * r)
        ring.position = point
        ring.zPosition = 1000
        ring.fillColor = UIColor.white.withAlphaComponent(0.14).cgColor
        ring.strokeColor = UIColor.white.withAlphaComponent(0.92).cgColor
        ring.lineWidth = 2.5
        // Dark around the light, so it shows on a white window and a dark one alike.
        ring.shadowColor = UIColor.black.cgColor
        ring.shadowOpacity = 0.5
        ring.shadowRadius = 2.5
        ring.shadowOffset = .zero
        ring.opacity = 0
        view.layer.addSublayer(ring)

        let fade = CAKeyframeAnimation(keyPath: "opacity")
        fade.values = [0, 1, 1, 0]
        fade.keyTimes = [0, 0.15, 0.6, 1]
        var animations: [CAAnimation] = [fade]
        if !UIAccessibility.isReduceMotionEnabled {
            let grow = CABasicAnimation(keyPath: "transform.scale")
            grow.fromValue = 0.55
            grow.toValue = 1
            grow.duration = 0.2
            grow.timingFunction = CAMediaTimingFunction(name: .easeOut)
            animations.append(grow)
        }
        let group = CAAnimationGroup()
        group.animations = animations
        group.duration = 0.6
        CATransaction.begin()
        CATransaction.setCompletionBlock { ring.removeFromSuperlayer() }
        ring.add(group, forKey: "hold")
        CATransaction.commit()
    }
}
