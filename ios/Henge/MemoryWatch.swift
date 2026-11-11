// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import UIKit

/// Low memory on this device. When the whole device runs short, iPadOS closes apps, the biggest last, and with a Mac
/// running Henge is the biggest by far: on 8 Nov 2026 an iPad with 11 GB closed Henge holding about 8 GB, after
/// closing 400 other processes first. iPadOS gives two signs beforehand, a UIKit memory warning and a memory-pressure
/// event (warning, then critical); each is handled here once, however many of them arrive together.
///
/// A warning: what can be rebuilt goes back to iPadOS. That is mostly the engine's translation tables, which a long
/// run leaves written though the code they describe is gone: up to about a gigabyte (core/machine.c,
/// hv_machine_trim_tables). A busy Mac's code cache fills every 30–60 s and then empties one of its eight zones,
/// whose tables go then (hv_machine_trim_zone); a full flush would give back all of them but stops the Mac for
/// seconds while its code is translated again (7–38 s on 8 Nov), so a warning waits for the next zone; only critical
/// asks for a flush at once, not again within `forceEvery`. The JIT region
/// itself (1.5 GB) stays: its pages were
/// prepared one by one by the debugger that made them executable, and pages given back would come back neither
/// prepared nor executable. The Mac's own memory isn't Henge's to give.
///
/// Critical: the same, and then
/// - with auto-save on, one is taken at once if the last is old: it clones the memory files rather than writing the
///   memory, so it is quick, and it is the existing auto-save with its own checks (AutoSave.swift);
/// - Henge doesn't suspend the Mac by itself. A Suspend writes the Mac's whole memory (about 8 GB, much of it
///   compressed by then, so reading it needs more memory) and takes tens of seconds, far longer than iPadOS waits
///   once it is closing the biggest app; and critical often passes: on 8 Nov iPadOS reached it four times in the
///   afternoon and closed Henge once. Stopping the person's Mac each time would cost more than it saves.
/// - The running Mac's session note (SessionMarker) records it, so that if iPadOS closes Henge now, the next start
///   says it was for memory and what helps (a smaller Mac). Back to normal, the note forgets it.
///
/// Each warning is one line in henge.log: what came, what went back and how much, and the footprint before and after.
final class MemoryWatch {
    static let shared = MemoryWatch()

    /// The least time between two flushes asked for at once (critical): each one stops a busy Mac for seconds
    /// while it translates its code again, and gives back only what was written since the last.
    static let forceEvery: TimeInterval = 60
    /// How long a critical moment is kept in the session note without another warning or critical, if iPadOS never
    /// says it is back to normal: a close later than that is not put down to memory.
    static let criticalKept: TimeInterval = 600

    private let queue = DispatchQueue(label: "henge.memory", qos: .userInitiated)
    private var source: DispatchSourceMemoryPressure?
    private var observer: NSObjectProtocol?
    // The queue's own
    private var gathering: (signs: [String], critical: Bool)?
    private var lastForced = Date.distantPast
    private var critical = false
    private var forget: DispatchWorkItem?

    private static let lock = NSLock()
    private static var criticalAtValue: Date?
    private static var guestRAMValue: UInt64 = 0

    /// When iPadOS last said memory was critical while a Mac ran (nil: not lately). SessionMarker writes it.
    static var criticalAt: Date? {
        get { lock.lock(); defer { lock.unlock() }; return criticalAtValue }
        set { lock.lock(); criticalAtValue = newValue; lock.unlock() }
    }

    /// The running Mac's memory, as it was built (MacPause.attach): said after a close for memory.
    static var guestRAMMB: UInt64 {
        get { lock.lock(); defer { lock.unlock() }; return guestRAMValue }
        set { lock.lock(); guestRAMValue = newValue; lock.unlock() }
    }

    private init() {}

    /// As Henge opens (HengeApp.init).
    func start() {
        guard source == nil else { return }
        let source = DispatchSource.makeMemoryPressureSource(eventMask: [.normal, .warning, .critical], queue: queue)
        source.setEventHandler { [weak self, weak source] in
            guard let self, let event = source?.data else { return }
            if event.contains(.critical) { self.sign("pressure critical", critical: true) }
            else if event.contains(.warning) { self.sign("pressure warning", critical: false) }
            else if event.contains(.normal) { self.backToNormal() }
        }
        source.resume()
        self.source = source
        observer = NotificationCenter.default.addObserver(forName: UIApplication.didReceiveMemoryWarningNotification, object: nil,
                                                          queue: nil) { [weak self] _ in
            self?.queue.async { self?.sign("UIKit warning", critical: false) }
        }
    }

    /// On the queue. Signs within half a second are one warning (UIKit's and the pressure source's come together).
    private func sign(_ what: String, critical: Bool) {
        if var g = gathering {
            g.signs.append(what)
            g.critical = g.critical || critical
            gathering = g
            return
        }
        gathering = ([what], critical)
        queue.asyncAfter(deadline: .now() + 0.5) { [weak self] in self?.handle() }
    }

    private func handle() {
        guard let g = gathering else { return }
        gathering = nil
        let before = Self.footprint()
        var gave: [String] = []

        let cached = URLCache.shared.currentMemoryUsage
        URLCache.shared.removeAllCachedResponses()
        if cached > 0 { gave.append("download cache \(Self.mb(UInt64(cached)))") }

        let running = henge_vm_state() == 1
        if running {
            let since = Date().timeIntervalSince(lastForced)
            let now = g.critical && since >= Self.forceEvery
            let done = henge_vm_trim_request(now ? 1 : 0)
            if done >= 0 && now {
                lastForced = Date()
                // The flush comes at a CPU's next block; a paused Mac does it as it resumes.
                var released: UInt64 = 0
                let deadline = Date().addingTimeInterval(3)
                while henge_vm_trims(&released) == done, Date() < deadline { usleep(50_000) }
                if henge_vm_trims(&released) > done {
                    gave.append("translation tables \(Self.mb(released)) (flushed now)")
                } else {
                    gave.append("translation tables: at the Mac's next step (\(henge_vm_paused() != 0 ? "it is paused" : "not yet in 3 s"))")
                }
            } else if done >= 0 {
                let why = g.critical ? "a flush was forced \(Int(since)) s ago" : "not critical"
                gave.append("translation tables: as the code cache next makes room (\(why); \(done) given back before)")
            }
        }
        let after = Self.footprint()
        let left = henge_ios_available_memory()

        var line = "iPadOS says memory is \(g.critical ? "CRITICAL" : "low") (\(g.signs.joined(separator: ", "))). "
        line += gave.isEmpty ? "Nothing to give back" : "Gave back: " + gave.joined(separator: "; ")
        line += ". Footprint \(Self.mb(before)) -> \(Self.mb(after)), \(Self.mb(UInt64(left))) more before this app's own limit"
        line += running ? ", the Mac has \(Self.guestRAMMB / 1024) GB" : ", no Mac running"
        if g.critical {
            critical = true
            if running {
                Self.criticalAt = Date()
                SessionMarker.noteMemory()
                line += ". If iPadOS closes Henge now, the next start says why"
                line += "; " + DispatchQueue.main.sync { MacPause.shared.autoSaveForMemory() }
            }
            line += "; not suspending: a Suspend writes all the Mac's memory, too slow and too much memory at this point"
        }
        if critical { keepCritical() }
        HengeLog.log("memory", line)
    }

    /// The note keeps the critical moment for `criticalKept` after the last sign.
    private func keepCritical() {
        forget?.cancel()
        let item = DispatchWorkItem { [weak self] in self?.clearCritical("no sign for \(Int(Self.criticalKept / 60)) minutes") }
        forget = item
        queue.asyncAfter(deadline: .now() + Self.criticalKept, execute: item)
    }

    private func backToNormal() {
        guard critical else { return }
        clearCritical("iPadOS says it is back to normal")
    }

    private func clearCritical(_ why: String) {
        critical = false
        forget?.cancel()
        forget = nil
        guard Self.criticalAt != nil else { return }
        Self.criticalAt = nil
        SessionMarker.noteMemory()
        HengeLog.log("memory", "memory is no longer critical (\(why)); Henge closing from now on isn't put down to memory")
    }

    static func footprint() -> UInt64 {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
        let kr = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count) }
        }
        return kr == KERN_SUCCESS ? info.phys_footprint : 0
    }

    static func mb(_ bytes: UInt64) -> String {
        bytes >= 1 << 30 ? String(format: "%.1f GB", Double(bytes) / Double(1 << 30)) : "\(bytes >> 20) MB"
    }
}
