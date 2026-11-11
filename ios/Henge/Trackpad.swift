import SwiftUI
import UIKit

/// A surface that works like a Mac's trackpad. One finger moves the pointer; a tap clicks; a finger
/// held still for a moment presses the button until it lifts (a ring shows as it does), which is how to
/// drag. Two fingers scroll (through the helper in macOS when it's there, so pages glide and rubber-band
/// as under a real trackpad, and a touch catches a glide), and tapped together they right-click. Pointer
/// speed suits a 1920-wide screen on a small one. A double-tap or squeeze of Apple Pencil right-clicks.
/// Only fingers count here: a real trackpad or mouse goes through PointerInput, or HardwareMouse while
/// iPadOS has the pointer locked.
struct Trackpad: UIViewRepresentable {
    /// Three fingers tapped together: the session shows its hidden controls.
    static let threeFingerTap = Notification.Name("henge.trackpad.threeFingerTap")

    /// A finger is the Mac's own touch or its pointer here now, and when one last was (system uptime): the zoomed view
    /// holds still under it, rather than follow the pointer it moves (macOS moves its pointer to a touch, too).
    private(set) static var fingersOn = false
    private(set) static var fingersAt: TimeInterval = 0
    static var holdsView: Bool { fingersOn || ProcessInfo.processInfo.systemUptime - fingersAt < 1 }

    /// The finger is the pointer (DirectTouch) rather than moving it as a trackpad does, zoomed in as well. Until macOS
    /// has the touch device, it stays a trackpad.
    var direct = false
    /// The view is zoomed in: the screen's size here isn't the size macOS is told the touchscreen has.
    var zoomed = false
    /// For direct touch: the Mac's screen in this view's points, and its size in pixels.
    var screen: CGRect = .zero
    var mac: CGSize = .zero
    var retina = false

    func makeUIView(context: Context) -> Surface { Surface() }
    func updateUIView(_ view: Surface, context: Context) {
        view.direct = direct
        view.directTouch.screen = screen
        if mac.width > 0, mac.height > 0 { view.directTouch.configured = mac }
        view.directTouch.retina = retina
        view.touchScreen.screen = screen
        if !zoomed { TouchScreen.configure(screenPoints: screen.size) }
    }

    final class Surface: UIView, UIPointerInteractionDelegate, UIPencilInteractionDelegate {
        private static let holdTime: TimeInterval = 0.4
        private static let tapTime: TimeInterval = 0.3
        private static let tapDistance: CGFloat = 8
        /// The Mac's points the pointer and a scroll move for each of the pad's.
        private static let scrollScale: CGFloat = 3
        private var fingers: [UITouch] = []
        private var started = Date()
        private var travelled: CGFloat = 0
        private var hadTwoFingers = false
        private var mostFingers = 0
        private var holding = false
        private var holdTimer: Timer?
        private var over = false            // cancelled by iPadOS: nothing more until every finger lifts
        private var caught = false          // the touch stopped a glide: lifting doesn't click
        private let scroller = MacScroller() // two fingers scrolling
        private var restX: CGFloat = 0
        private var restY: CGFloat = 0
        private let buzz = UIImpactFeedbackGenerator(style: .medium)
        var direct = false
        let directTouch = DirectTouch()
        let touchScreen = TouchScreen()      // macOS 27's own touch, when it has Henge's touchscreen
        let pointer = PointerInput()

        override init(frame: CGRect) {
            super.init(frame: frame)
            isMultipleTouchEnabled = true
            backgroundColor = .clear
            addInteraction(UIPointerInteraction(delegate: self))
            let pencil = UIPencilInteraction()
            pencil.delegate = self
            addInteraction(pencil)
            let hover = UIHoverGestureRecognizer(target: self, action: #selector(hovered(_:)))
            hover.cancelsTouchesInView = false
            addGestureRecognizer(hover)
            pointer.attach(to: self) { [weak self] in self?.directTouch.screen ?? .zero }
        }

        /// iPadOS watches the screen's edges for its own swipes (Notification Center, Control Center) and holds
        /// a touch there back until the finger lifts, so a tap on the Mac's menu bar reached macOS as a press
        /// and release a few milliseconds apart. Touches go to the Mac as they land; the swipes still work.
        override func didMoveToWindow() {
            super.didMoveToWindow()
            letEdgeTouchesThrough()
        }

        /// Leaving the screen (the session closes) with fingers on the touchscreen: they end there as cancelled, so
        /// macOS isn't left holding a touch no finger will lift.
        override func willMove(toWindow newWindow: UIWindow?) {
            super.willMove(toWindow: newWindow)
            if newWindow == nil { touchScreen.giveWay() }
        }

        override func layoutSubviews() {
            super.layoutSubviews()
            letEdgeTouchesThrough()
        }

        private func letEdgeTouchesThrough() {
            for recognizer in window?.gestureRecognizers ?? [] where recognizer.delaysTouchesBegan {
                recognizer.delaysTouchesBegan = false
            }
        }

        /// Apple Pencil above the glass moves the pointer. A mouse or trackpad hovers too, with no height:
        /// that goes through PointerInput.
        @objc private func hovered(_ hover: UIHoverGestureRecognizer) {
            guard direct, hover.state == .began || hover.state == .changed, hover.zOffset > 0, DirectTouch.ready, !touchScreen.active else { return }
            directTouch.hover(hover.location(in: self))
        }

        /// Over the Mac there is only the Mac's pointer, if iPadOS hasn't locked its own away.
        /// Over the Mac's screen the Mac draws its own pointer, so the iPad's hides; elsewhere (the trackpad
        /// area below an upright screen) the iPad's pointer shows as usual.
        func pointerInteraction(_ interaction: UIPointerInteraction, regionFor request: UIPointerRegionRequest, defaultRegion: UIPointerRegion) -> UIPointerRegion? {
            directTouch.screen.contains(request.location) ? UIPointerRegion(rect: directTouch.screen) : nil
        }

        func pointerInteraction(_ interaction: UIPointerInteraction, styleFor region: UIPointerRegion) -> UIPointerStyle? { .hidden() }

        // MARK: Apple Pencil

        /// A double-tap of the Pencil right-clicks, as its second button, unless the iPad's Pencil settings say
        /// to ignore it or to run a shortcut (iPadOS does that itself).
        @available(iOS 17.5, *)
        func pencilInteraction(_ interaction: UIPencilInteraction, didReceiveTap tap: UIPencilInteraction.Tap) {
            guard Self.rightClicks(UIPencilInteraction.preferredTapAction) else { return }
            pencilRightClick(at: tap.hoverPose?.location)
        }

        /// The same before iPadOS 17.5 (which calls the one above instead): no hover position, so at the pointer.
        func pencilInteractionDidTap(_ interaction: UIPencilInteraction) {
            guard Self.rightClicks(UIPencilInteraction.preferredTapAction) else { return }
            pencilRightClick(at: nil)
        }

        /// A squeeze (Apple Pencil Pro) right-clicks as it lets go, with the same exceptions.
        @available(iOS 17.5, *)
        func pencilInteraction(_ interaction: UIPencilInteraction, didReceiveSqueeze squeeze: UIPencilInteraction.Squeeze) {
            guard squeeze.phase == .ended, Self.rightClicks(UIPencilInteraction.preferredSqueezeAction) else { return }
            pencilRightClick(at: squeeze.hoverPose?.location)
        }

        private static func rightClicks(_ action: UIPencilPreferredAction) -> Bool {
            if action == .ignore { return false }
            if #available(iOS 17.5, *), action == .runSystemShortcut { return false }
            return true
        }

        /// Where the Pencil points (hovering) or last was, when the finger is the pointer; at the pointer otherwise.
        private func pencilRightClick(at point: CGPoint?) {
            if direct, DirectTouch.ready {
                directTouch.pencilRightClick(at: point)
            } else if fingers.isEmpty, !directTouch.active {
                henge_vm_mouse_button(1, 1)
                henge_vm_mouse_button(1, 0)
                StickyKeys.shared.releaseAll()
            }
        }

        /// A full-size iPad has more room to move a finger across than a phone does.
        private var speed: CGFloat { traitCollection.horizontalSizeClass == .regular ? 2 : 3 }

        required init?(coder: NSCoder) { fatalError("made in code") }

        // MARK: Fingers

        /// Notes whether a finger is the Mac's touch or pointer, as each touch event comes and once it's handled.
        private func markFingers() {
            guard direct else { return }
            let on = touchScreen.active || directTouch.active
            if on || Trackpad.fingersOn { Trackpad.fingersAt = ProcessInfo.processInfo.systemUptime }
            Trackpad.fingersOn = on
        }

        override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
            markFingers(); defer { markFingers() }
            // A gesture is macOS's own touch or imitated, decided as it starts and kept until every finger lifts.
            if touchScreen.active {
                let pens = touches.filter { $0.type == .pencil }
                guard !pens.isEmpty else { return touchScreen.began(touches, in: self) }
                touchScreen.giveWay() // the Pencil is a pointer: the fingers' touches end as a resting palm's
                if direct, DirectTouch.ready { directTouch.began(pens, in: self) }
                return
            }
            if !directTouch.active, direct, fingers.isEmpty, TouchScreen.takes(touches) { return touchScreen.began(touches, in: self) }
            if directTouch.active || (direct && fingers.isEmpty && DirectTouch.ready) { return directTouch.began(touches, in: self) }
            let touches = touches.filter { $0.type != .indirectPointer }
            guard let first = touches.first else { return }
            if fingers.isEmpty {
                started = Date()
                travelled = 0
                hadTwoFingers = false
                mostFingers = 0
                over = false
                scroller.end(gliding: false, at: first.timestamp) // none should still be going; if one is, it stops here
                restX = 0
                restY = 0
                buzz.prepare()
                // A touch catches a page still gliding, as on a Mac's trackpad: it stops, and the tap doesn't click.
                caught = false
                if DirectTouch.helperReady {
                    caught = DirectTouch.glideCatches(at: first.timestamp)
                    DirectTouch.catchUntil = 0
                }
                MacScroller.stopGlide() // early: a tap's click at the lift waits for what is left of it (the input fence)
            }
            fingers.append(contentsOf: touches)
            mostFingers = max(mostFingers, fingers.count)
            guard !over else { return }
            if fingers.count >= 2 {
                hadTwoFingers = true
                cancelHold()
            } else if !holding {
                armHold()
            }
        }

        override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
            markFingers(); defer { markFingers() }
            let onTouchScreen = touchScreen.active
            if onTouchScreen { touchScreen.moved(touches, in: self) }
            if directTouch.active { return directTouch.moved(touches, in: self) }
            if onTouchScreen { return }
            guard !over, let finger = fingers.first, touches.contains(finger) || (fingers.count >= 2 && touches.contains(where: fingers.contains)) else { return }
            let now = finger.location(in: self), before = finger.previousLocation(in: self)
            let dx = now.x - before.x, dy = now.y - before.y
            travelled += abs(dx) + abs(dy)
            if fingers.count >= 2 {
                scroll(dx, dy, at: finger.timestamp)
            } else {
                if travelled >= Self.tapDistance, !holding { cancelHold() }
                restX += dx * speed
                restY += dy * speed
                let moveX = restX.rounded(), moveY = restY.rounded()
                guard moveX != 0 || moveY != 0 else { return }
                restX -= moveX
                restY -= moveY
                henge_vm_mouse_move(Int32(moveX), Int32(moveY))
            }
        }

        override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
            markFingers(); defer { markFingers() }
            let onTouchScreen = touchScreen.active
            if onTouchScreen { touchScreen.ended(touches, in: self) }
            if directTouch.active { return directTouch.ended(touches, in: self) }
            if onTouchScreen { return }
            lift(touches)
        }

        /// iPadOS took the touches (a swipe from the edge, say): nothing clicks, a held button lets go where the
        /// pointer is, and a scroll ends without gliding.
        override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
            markFingers(); defer { markFingers() }
            let onTouchScreen = touchScreen.active
            if onTouchScreen { touchScreen.cancelled(touches, in: self) }
            if directTouch.active { return directTouch.cancelled(touches) }
            if onTouchScreen { return }
            // Only this gesture's fingers: one that began elsewhere (an eleventh on the touchscreen, one after
            // the Pencil took over) ends here too once that gesture is over, and must not end this one.
            let touches = touches.filter { fingers.contains($0) }
            guard !touches.isEmpty else { return }
            fingers.removeAll { touches.contains($0) }
            cancelHold()
            endScroll(gliding: false)
            if holding {
                holding = false
                henge_vm_mouse_button(0, 0)
                StickyKeys.shared.releaseAll()
            }
            over = true
        }

        private func lift(_ touches: Set<UITouch>) {
            let touches = touches.filter { fingers.contains($0) } // this gesture's fingers only, as when cancelled
            guard !touches.isEmpty else { return }
            let liftTime = touches.map(\.timestamp).max() ?? 0
            fingers.removeAll { touches.contains($0) }
            // Two fingers that were scrolling, down to one (or none): the scroll ends here, gliding on if they
            // were still moving, and a finger left behind moves the pointer again.
            if fingers.count < 2 { endScroll(gliding: true, liftTime: liftTime) }
            guard fingers.isEmpty else { return }
            cancelHold()
            if holding {
                holding = false
                henge_vm_mouse_button(0, 0)
                StickyKeys.shared.releaseAll()
                return
            }
            if over || caught { return }
            if mostFingers >= 3 {
                if travelled < Self.tapDistance * 3, Date().timeIntervalSince(started) < Self.tapTime * 2 {
                    NotificationCenter.default.post(name: Trackpad.threeFingerTap, object: nil)
                }
                return
            }
            guard travelled < Self.tapDistance, Date().timeIntervalSince(started) < Self.tapTime else { return }
            let button: Int32 = hadTwoFingers ? 1 : 0
            henge_vm_mouse_button(button, 1)
            henge_vm_mouse_button(button, 0)
            StickyKeys.shared.releaseAll()
            if button == 0 { KeyboardOwner.shared.clicked() } // a click in a field brings the keyboard back
        }

        // MARK: Holding

        /// In the run loop's common modes, so it keeps time while something elsewhere in the app tracks a touch.
        private func armHold() {
            holdTimer?.invalidate()
            let timer = Timer(timeInterval: Self.holdTime, repeats: false) { [weak self] _ in
                guard let self, !self.over, self.fingers.count == 1, self.travelled < Self.tapDistance else { return }
                self.holding = true
                henge_vm_mouse_button(0, 1)
                self.buzz.impactOccurred()
                HoldRing.show(at: self.fingers[0].location(in: self), in: self)
            }
            RunLoop.main.add(timer, forMode: .common)
            holdTimer = timer
        }

        private func cancelHold() {
            holdTimer?.invalidate()
            holdTimer = nil
        }

        // MARK: Scrolling

        /// As on a Mac's trackpad, the page follows the fingers: about three of the Mac's points for each of the
        /// pad's, the same as the pointer, times Settings' Scroll speed.
        private func scroll(_ dx: CGFloat, _ dy: CGFloat, at time: TimeInterval) {
            let scale = Self.scrollScale * ScrollSpeed.factor
            scroller.scroll(by: CGPoint(x: dx * scale, y: dy * scale), at: time)
        }

        /// The scroll ends: through the helper it glides on from the fingers' speed as they lifted, if they were
        /// still moving, as under a Mac's trackpad.
        private func endScroll(gliding: Bool, liftTime: TimeInterval = 0) {
            let v = scroller.end(gliding: gliding, at: liftTime)
            if hypot(v.x, v.y) >= DirectTouch.catchSpeed { DirectTouch.catchUntil = liftTime + 0.12 }
        }
    }
}

/// One mouse button you can hold: down when touched, up when let go.
struct MouseButton: View {
    let title: String
    let button: Int32
    @State private var down = false

    var body: some View {
        Text(title)
            .font(.system(size: 15, weight: .semibold))
            .foregroundStyle(down ? Desk.text : Desk.dim)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(down ? Desk.raised : Color.clear)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        guard !down else { return }
                        down = true
                        MacScroller.stopGlide() // the click waits for it (the input fence), so it lands where the page stopped
                        henge_vm_mouse_button(button, 1)
                        UIImpactFeedbackGenerator(style: .light).impactOccurred()
                    }
                    .onEnded { _ in
                        down = false
                        henge_vm_mouse_button(button, 0)
                        StickyKeys.shared.releaseAll()
                    }
            )
            .accessibilityLabel(title)
    }
}
