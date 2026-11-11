import CoreGraphics
import Foundation
import QuartzCore

/// Settings' "Scroll speed": how far a page on the Mac moves for the fingers, for all the scrolling Henge does
/// (a trackpad or mouse, and two fingers on the on-screen trackpad). At 1 the page follows the fingers, as the
/// iPad's own pages do.
enum ScrollSpeed {
    static let key = "henge.scroll.speed"
    static let range: ClosedRange<Double> = 0.25...3
    static let standard = 1.0

    static var factor: CGFloat {
        let value = UserDefaults.standard.object(forKey: key) as? Double ?? standard
        return CGFloat(value.isFinite ? min(max(value, range.lowerBound), range.upperBound) : standard)
    }
}

/// The Mac's screen, as scrolling needs it.
enum MacScreen {
    /// In macOS's points (what a page scrolls by).
    static var points: CGSize {
        var w: Int32 = 0, h: Int32 = 0
        if henge_vm_screen_points(&w, &h) == 0, w > 0, h > 0 { return CGSize(width: Int(w), height: Int(h)) }
        if henge_vm_screen_size(&w, &h) == 0, w > 0, h > 0 { return CGSize(width: Int(w), height: Int(h)) }
        return CGSize(width: 1920, height: 1080)
    }

    /// Where the Mac's pointer is, 0...1 across its screen: what a trackpad scrolls, as on a Mac.
    static var pointerSpot: CGPoint {
        var x: Int32 = 0, y: Int32 = 0, w: Int32 = 0, h: Int32 = 0
        guard henge_vm_pointer(&x, &y) == 0, henge_vm_screen_size(&w, &h) == 0, w > 1, h > 1 else { return CGPoint(x: 0.5, y: 0.5) }
        return CGPoint(x: min(max(CGFloat(x) / CGFloat(w - 1), 0), 1), y: min(max(CGFloat(y) / CGFloat(h - 1), 0), 1))
    }
}

/// One scroll of the Mac, the way the iPad scrolls its own pages: the page follows the fingers, a small move moves
/// it a little, and fingers that lift while moving fling it on, slowing as a Mac's page does. Through the helper in
/// macOS when it's there (as a Mac's trackpad: exact, gliding and rubber-banding); otherwise with the scroll wheel,
/// whose units MacWheel works out from the distance.
final class MacScroller {
    private enum Lock { case vertical, horizontal, free }

    /// Slower than this as the fingers lift is a page put down, not flung. The Mac's points a second.
    static let glideSpeed: CGFloat = 600
    private static let topSpeed: CGFloat = 24000

    private var active = false
    private var helper = false
    private var spot = CGPoint.zero
    private var lock: Lock?
    private var total = CGPoint.zero
    private var samples: [(time: TimeInterval, at: CGPoint)] = []
    private let wheel = MacWheel()

    /// Moves the page by d of the Mac's points, d.y > 0 bringing it down, as fingers moving down do. The first move
    /// starts a scroll at `start` (0...1 across the Mac's screen), or where the Mac's pointer is. Mostly one way at
    /// the start is that way only, so a page doesn't drift sideways, or a browser go back.
    func scroll(by d: CGPoint, at time: TimeInterval, from start: CGPoint? = nil) {
        var d = d
        guard d != .zero else { return }
        let began = !active
        if began {
            active = true
            lock = nil
            total = .zero
            samples = []
            wheel.reset()
            helper = DirectTouch.helperReady
            DirectTouch.stopWheelGlide() // the helper's own glide stops as this scroll begins there
            if helper { spot = start ?? MacScreen.pointerSpot }
        }
        if lock == nil {
            lock = abs(d.y) > 1.5 * abs(d.x) ? .vertical : abs(d.x) > 1.5 * abs(d.y) ? .horizontal : .free
        }
        if lock == .vertical { d.x = 0 }
        if lock == .horizontal { d.y = 0 }
        total.x += d.x
        total.y += d.y
        samples.append((time, total))
        samples.removeAll { time - $0.time > 0.1 }
        guard helper else { return wheel.scroll(by: d) }
        let m = MacScreen.points, at = spot
        henge_share_touch_scroll(began ? 1 : 2, at.x, at.y, d.x / m.width, d.y / m.height)
    }

    /// The scroll ends at `time`: through the helper it glides on from the speed the page had then, if it was still
    /// moving, as under a Mac's trackpad. A fling needs `fling` moves in the last 45 ms (a stream of them, not one
    /// wheel notch). Returns that speed, the Mac's points a second.
    @discardableResult
    func end(gliding: Bool, at time: TimeInterval, fling: Int = 2) -> CGPoint {
        finish(gliding ? speed(at: time, fling: fling) : .zero)
    }

    /// The scroll ends at the speed iPadOS measured for it (a pointer's scroll), the Mac's points a second: it
    /// glides on the same as one measured here.
    @discardableResult
    func end(velocity: CGPoint) -> CGPoint {
        var v = velocity
        if lock == .vertical { v.x = 0 }
        if lock == .horizontal { v.y = 0 }
        return finish(Self.glide(v))
    }

    private func finish(_ v: CGPoint) -> CGPoint {
        guard active else { return .zero }
        active = false
        guard helper else { return .zero }
        let m = MacScreen.points, at = spot
        henge_share_touch_scroll(4, at.x, at.y, v.x / m.width, v.y / m.height)
        return v
    }

    /// Stops a page still gliding, as a finger on a Mac's trackpad does: whichever hand flung it (a finger on the
    /// screen, the trackpad, a mouse), as the Mac has one glide. Without the helper a finger's flick glides on this
    /// side (DirectTouch) and stops here and now. The helper's glide stops on its own queue: a click after this waits
    /// until it says so (the input fence in the engine, input_fence.h), so it lands where the page stopped. Said
    /// early, as a press begins, so the answer is on its way.
    static func stopGlide() {
        DirectTouch.stopWheelGlide()
        if DirectTouch.helperReady { henge_share_touch_stop() }
    }

    private func speed(at time: TimeInterval, fling: Int) -> CGPoint {
        guard let latest = samples.last, time - latest.time < 0.05 else { return .zero }
        let recent = samples.filter { latest.time - $0.time <= 0.045 }
        guard recent.count >= fling, let first = recent.first else { return .zero }
        let dt = latest.time - first.time
        guard dt > 0.008 else { return .zero }
        return Self.glide(CGPoint(x: (latest.at.x - first.at.x) / dt, y: (latest.at.y - first.at.y) / dt))
    }

    /// A speed to glide on at: none if it was too slow to be a fling, and no faster than a page can go.
    private static func glide(_ v: CGPoint) -> CGPoint {
        guard v.x.isFinite, v.y.isFinite, hypot(v.x, v.y) > glideSpeed else { return .zero }
        return CGPoint(x: max(min(v.x, topSpeed), -topSpeed), y: max(min(v.y, topSpeed), -topSpeed))
    }
}

/// The Mac's scroll wheel, for scrolling that should move a page a set distance (when the helper in the
/// Mac isn't there to scroll as a trackpad does). macOS speeds the wheel up hard, by the square of an
/// event's units and by how close together its recent events came (IOHIDFamily's IOHIDScrollAccelerator:
/// an event more than 150 ms after the one before counts as on its own; events a 60th of a second apart
/// count as a stream, and macOS's sense of their speed is about 800 times as high). On its own, a page moves
/// about 0.08 d² points for an event of d units (measured: 8 units 4 points, 100 units 970); in a stream
/// about a dozen times as far (#38: a swipe through the wheel, at 0.08, flew past Setup Assistant's list;
/// PR #14's tuning on a Mac came to about a twenty-fifth of that). So each event sends as many units as the
/// distance so far covers at the rate its timing gets, and what they fall short by waits for the next. At
/// most 60 events a second go, the distance in between gathered into the next (PR #14's finding).
final class MacWheel {
    /// The Mac's points for an event of d units, over d², on its own and in a stream.
    private static let aloneSquared: CGFloat = 0.08
    private static let streamSquared: CGFloat = 1.0
    private static let interval: CFTimeInterval = 1.0 / 60
    private var rest = CGPoint.zero
    private var lastSent: CFTimeInterval = 0
    private var flushPending = false
    private var generation = 0

    func reset() {
        rest = .zero
        generation += 1 // a send still waiting belongs to the scroll before
        flushPending = false
    }

    /// Moves the page by d of the Mac's points, the way it follows a finger: d.y > 0 brings it down.
    func scroll(by d: CGPoint) {
        rest.x += d.x
        rest.y += d.y
        send()
    }

    private func send() {
        let now = CACurrentMediaTime()
        let wait = lastSent + Self.interval - now
        guard wait <= 0 else {
            // The distance waits for the next event, or goes on its own once the interval is up.
            guard !flushPending else { return }
            flushPending = true
            let scroll = generation
            DispatchQueue.main.asyncAfter(deadline: .now() + wait) { [weak self] in
                guard let self, self.generation == scroll else { return }
                self.flushPending = false
                self.send()
            }
            return
        }
        let k = Self.squared(after: now - lastSent)
        let ux = Self.units(rest.x, k), uy = Self.units(rest.y, k)
        guard ux != 0 || uy != 0 else { return }
        rest.x -= Self.points(ux, k)
        rest.y -= Self.points(uy, k)
        lastSent = now
        henge_vm_mouse_scroll(-ux, uy)
    }

    /// Points over units² for an event this long after the one before. macOS's speed for a wheel goes with
    /// (A t² − B t + C) for t milliseconds between events (IOHIDScrollAccelerator: 0 at 150 ms and past it,
    /// 1.27 at a 60th of a second); the rate here goes from on its own to a stream with it.
    static func squared(after gap: CFTimeInterval) -> CGFloat {
        func pace(_ seconds: CFTimeInterval) -> CGFloat {
            let t = CGFloat(min(max(seconds * 1000, 1), 150))
            let a: CGFloat = 2.0 / 65536, b: CGFloat = 0x3bb / 65536.0, c: CGFloat = 0x18041 / 65536.0
            return max(a * t * t - b * t + c, 0)
        }
        let share = min(pace(gap) / pace(interval), 1)
        return aloneSquared + (streamSquared - aloneSquared) * share
    }

    private static func units(_ points: CGFloat, _ k: CGFloat) -> Int32 {
        let units = (abs(points) / k).squareRoot().rounded(.down)
        return Int32(points < 0 ? -units : units)
    }

    private static func points(_ units: Int32, _ k: CGFloat) -> CGFloat {
        let u = CGFloat(units)
        return (u < 0 ? -1 : 1) * k * u * u
    }
}
