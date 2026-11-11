import BackgroundTasks
import SwiftUI
import UIKit
import UserNotifications

/// Pausing the running Mac: it stops between two instructions with its clock standing still and its sound
/// off, after macOS has written what it holds for its disks (through Henge Tools) and everything its disks
/// were sent is on the iPad's storage. Resuming carries on from the same instruction; to macOS the pause
/// never happened.
///
/// Henge pauses the Mac by itself when it leaves the screen (Home, another app, the iPad locking), since
/// iOS stops apps it can't see and can close them; it resumes when Henge is back if that pause was its own.
/// All the waiting happens on one queue, in order, never on the main thread.
final class MacPause: ObservableObject {
    static let shared = MacPause()

    static let autoPauseKey = "henge.pause.auto"
    static let autoResumeKey = "henge.pause.autoResume"

    enum Phase: Equatable {
        case running
        case pausing
        /// safe: everything the Mac's disks were sent is on storage. guestSynced: macOS wrote its own
        /// cache first. auto: Henge paused it on leaving the screen, not the person.
        case paused(safe: Bool, guestSynced: Bool, auto: Bool)
        case resuming
    }

    @Published private(set) var phase = Phase.running { didSet { holdPorts() } }
    /// Henge is off the screen (from leaving until it is active again).
    private var away = false
    /// The Mac's ports on the network are open on the iPad only while it runs on the screen: iOS takes a suspended
    /// app's listening sockets back (TN2277), and a paused Mac answers nothing anyway. Closed, a device on the
    /// network is refused at once rather than left waiting; opened again as the Mac goes on.
    private func holdPorts() {
        henge_vm_forwards_hold(away || phase != .running ? 1 : 0)
    }
    /// A Suspend that didn't even freeze the Mac, said once: the Mac went on running.
    @Published var problem: String? {
        didSet { if let problem { MacPause.announce(problem); SuspendAlert.failed(problem, stillRunning: true) } }
    }
    /// The paused Mac is also saved to storage (Suspend): if iOS closes Henge now, it goes on from here next time.
    @Published private(set) var suspended = false {
        didSet {
            if suspended && !oldValue {
                MacPause.announce("Suspended. You can leave Henge.")
                SuspendAlert.clear() // a warning from before is out of date
            }
        }
    }
    /// A save queued or under way (main thread): leaving again meanwhile doesn't start a second, and the
    /// paused screen says "Suspending…".
    @Published private(set) var suspending = false
    /// Why the last save didn't happen, asked for or on leaving the screen (the Mac stayed frozen); cleared by the
    /// next try. The "Not Suspended" screen says it, and coming back to Henge doesn't resume over it.
    @Published private(set) var suspendFailure: String? {
        didSet { if let suspendFailure { MacPause.announce(suspendFailure); SuspendAlert.failed(suspendFailure, stillRunning: false) } }
    }
    /// For VoiceOver, only while Henge is on the screen: a save on leaving ends with another app in front.
    private static func announce(_ text: String) {
        guard UIApplication.shared.applicationState == .active else { return }
        UIAccessibility.post(notification: .announcement, argument: text)
    }
    /// A suspended Mac that can't go on from where it was: the person chooses (ResumeQuestionCard).
    @Published var resumeQuestion: ResumeQuestion?
    /// The Mac whose Suspend the person just gave up on (Start Fresh): it starts anew, so its screen says starting,
    /// not resuming. For that start only (MacSuspend.boot clears it, and the session as it closes).
    @Published var resumeGivenUp: UUID?
    /// Auto-save, for the session's details: when the last was kept, or why none is now. nil: auto-save is off.
    @Published private(set) var autoSaveNote: String?
    /// Paused just as an auto-save finished (it was under way when the pause was asked for, and was let finish with
    /// the Mac left paused): that auto-save is this paused Mac exactly, so a Suspend after it failing loses nothing.
    @Published private(set) var autoSaveCovers = false {
        didSet { SuspendAlert.covered = autoSaveCovers }
    }

    var isPaused: Bool { if case .paused = phase { return true } else { return false } }
    /// Pausing, paused or resuming: the Mac's screen and input are not the person's.
    var isHeld: Bool { phase != .running }

    private let queue = DispatchQueue(label: "henge.pause", qos: .userInitiated)
    private var mac: VirtualMac?
    private var started = Date()
    /// What the Mac was built with: a Suspend records them, and a resume builds it with the same.
    private var ramMB: UInt64 = 0
    private var cpus: Int32 = 0
    /// When macOS last confirmed it wrote its cache, so a pause right after skips asking again.
    private var lastGuestSync: Date?
    /// The queue's own (only touched there): when it last asked macOS to write its cache, and last heard it had.
    private var syncAsked: Date?
    private var syncEnded: Date? // answered or not
    private var syncDone: Date?
    /// The save under way is the one leaving the screen started (not the person's Suspend): coming back calls it off.
    private var savingOnLeave = false
    private var observers: [NSObjectProtocol] = []
    /// Auto-save (AutoSave.swift), main thread: one under way on the queue; when the next is due; the ticks' samples
    /// of frames shown (for a still screen); when the last was taken; the ticks since the room was last checked.
    private var autoSaving = false
    private var autoSaveDue = Date.distantFuture
    private var autoSaveTimer: Timer?
    private var frameSamples: [(at: Date, frames: UInt64)] = []
    private var lastAutoSave = Date()
    private var lastDiskChange: Date?
    private var autoSaveWaitSaid = false
    /// Why the last tick didn't take one (said once in the log, until the reason changes).
    private var autoSaveNotSaid: String?
    /// The running time left until the next is due, kept while the Mac is paused (resume() sets the due time from it).
    private var autoSaveLeft: TimeInterval?
    private let roomWatch = AutoSaveRoomWatch()

    private init() {
        UserDefaults.standard.register(defaults: [Self.autoPauseKey: true, Self.autoResumeKey: true, MacSuspend.autoKey: true])
        let centre = NotificationCenter.default
        observers = [
            centre.addObserver(forName: UIApplication.willResignActiveNotification, object: nil, queue: .main) { [weak self] _ in self?.resigning() },
            centre.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) { [weak self] _ in self?.leftScreen() },
            centre.addObserver(forName: UIApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in self?.cameBack() },
        ]
    }

    // MARK: The session

    /// The Mac has started: Henge watches for leaving the screen from now on, and notes that it runs.
    /// ramMB and cpus: what it was built with (0: not known, and then it isn't suspended).
    func attach(_ mac: VirtualMac, ramMB: UInt64 = 0, cpus: Int32 = 0) {
        self.mac = mac
        self.ramMB = ramMB
        self.cpus = cpus
        started = Date()
        phase = .running
        problem = nil
        suspended = false
        suspendFailure = nil
        autoSaveCovers = false
        lastGuestSync = nil
        MemoryWatch.guestRAMMB = ramMB
        SessionMarker.write(mac: mac, started: started, paused: false, safe: false)
        Lab.started()
        if autoPause && MacSuspend.enabled { SuspendAlert.askOnce() } // saves on leaving can fail where no one sees
        startAutoSave()
    }

    /// Shut down: macOS writes its cache, the disks are synced, then the machine stops. The screen can go at
    /// once; this finishes on the pause queue. A paused Mac only stops.
    func shutDown() {
        guard let gone = mac else { return }
        mac = nil
        let saving = autoSaving
        stopAutoSave()
        let wasPaused = isPaused
        phase = .running
        problem = nil
        suspended = false
        // Noted at once, so starting the Mac again asks even if Henge is closed before the queue gets to it. Not with
        // an auto-save still stopping: it may write its notes as this does (the queue notes it once it is done).
        if !saving { MacAutoSave.markForcedOff(gone.id) }
        let task = beginBackgroundTask("Henge shut down")
        queue.async {
            // An auto-save that finished meanwhile (its notes written afresh) is noted too
            let kept = MacAutoSave.exists(gone.id)
            if kept { MacAutoSave.markForcedOff(gone.id) }
            let suspendedToo = wasPaused && MacSuspend.hasSaved(for: gone.id)
            if !wasPaused {
                let synced = henge_share_sync_guest(2500) == 0
                HengeLog.log("pause", "shut down: macOS \(synced ? "wrote its cache" : "was not asked to write its cache (no Henge Tools)")")
                _ = henge_vm_pause(2000) // its disks on storage before the power goes
            }
            else { HengeLog.log("pause", "shut down while paused\(MacSuspend.hasSaved(for: gone.id) ? ": its Suspend is deleted" : "")") }
            henge_vm_stop()
            MacSuspend.forget(gone.id) // a Suspend held the Mac this turns off
            if henge_vm_halted_by_macos() != 0 {
                // macOS turned it off itself just before: as when macOS shuts down (stopped())
                MacAutoSave.forget(gone.id, "macOS shut down or restarted, as Henge turned the Mac off")
            } else if suspendedToo {
                // Its Suspend, newer than any auto-save and the Mac exactly, was just deleted: carrying on from an
                // older auto-save instead would be offered as if it were the Mac as it was. The person turned it off.
                MacAutoSave.forget(gone.id, "the Mac was turned off while suspended: its Suspend is deleted too")
            } else if kept {
                // Henge turned the power off, not macOS: as for any stop macOS didn't make (stopped()), the auto-save
                // is kept. Force Shut Down is for a stuck Mac, and the auto-save may be the only copy of what it had.
                // Starting the Mac again always asks whether to carry on (the person chose to turn it off: marked above).
                HengeLog.log("autosave", "kept: Henge turned the Mac off, not macOS; starting it again asks whether to carry on")
            }
            SessionMarker.remove()
            DispatchQueue.main.async { self.endBackgroundTask(task) }
        }
    }

    /// Returns once nothing is left on the pause queue: a shut down already asked for has finished (Relaunch).
    func settled() async {
        await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in queue.async { done.resume() } }
    }

    /// The machine stopped by itself (macOS shut down or restarted, or it failed). Only macOS turning it off or
    /// restarting it ends what its auto-save stands for; a failure (Henge's, or one the Mac hit) keeps it, so starting
    /// the Mac again offers to carry on from it. (A macOS panic restarts it: that counts as macOS restarting.)
    func stopped() {
        stopAutoSave()
        if let gone = mac {
            let byMacOS = henge_vm_stopped_by_macos() != 0
            let reason = String(cString: henge_vm_exit_reason())
            queue.async {
                MacSuspend.forget(gone.id)
                if byMacOS {
                    MacAutoSave.forget(gone.id, "macOS shut down or restarted (\(reason))")
                } else if MacAutoSave.exists(gone.id) {
                    HengeLog.log("autosave", "kept: the Mac stopped without macOS turning it off (\(reason)); starting it again offers to carry on")
                }
            }
        }
        mac = nil
        phase = .running
        suspended = false
        SessionMarker.remove()
    }

    // MARK: Pause and resume

    /// The one button: Suspend while the Mac runs, Resume once it is held. There is no pause the person chooses
    /// on its own: on iOS a Mac held only in memory is lost if Henge is closed, so the button always saves it whole.
    func toggle() {
        switch phase {
        case .running: suspend()
        case .paused: resume()
        default: break
        }
    }

    /// readAhead: leaving the screen with a save to follow; the screen is read ahead right after the pause.
    /// false if nothing was started (not running).
    @discardableResult
    func pause(auto: Bool, readAhead: ReadAhead? = nil) -> Bool {
        guard let mac, phase == .running, henge_vm_state() == 1 else { return false }
        if autoSaving {
            holdAutoSave("the Mac is being paused")
        } else if autoSaveDue != .distantFuture {
            autoSaveLeft = max(0, autoSaveDue.timeIntervalSinceNow)
        }
        phase = .pausing
        problem = nil
        suspendFailure = nil
        releaseInput()
        let recent = lastGuestSync.map { Date().timeIntervalSince($0) < 3 } ?? false
        queue.async {
            let t0 = Date()
            var guestSynced = recent
            if !recent && henge_vm_paused() != 0 {
                // Paused already: an auto-save under way was let finish and left it paused (holdAutoSave). macOS
                // can't answer while paused, and what it held for its disks is in that auto-save with its memory.
            } else if !recent {
                // Leaving: resigning asked just now (and this queue waited for its answer). Asking again would
                // spend the moments the GPU is still Henge's. Only if that ended within 1.5 s (macOS has written
                // little since): its answer counts, or, if none came (2.5 s waited), it isn't asked again.
                let answered = self.syncAsked.flatMap { asked in self.syncDone.map { $0 >= asked } } ?? false
                if readAhead != nil, let ended = self.syncEnded, Date().timeIntervalSince(ended) < 1.5 {
                    guestSynced = answered
                } else {
                    guestSynced = self.syncGuest()
                }
            }
            let rc = henge_vm_pause(2000)
            if let readAhead { self.readScreenAhead(readAhead) } // first: before the pause's other work
            if rc >= 0 { MacPreview.saveNow(for: mac) } // the paused screen, for the Macs list
            let took = Int(Date().timeIntervalSince(t0) * 1000)
            HengeLog.log("pause", "\(auto ? "auto " : "")pause: \(rc == 0 ? "safe" : rc == 1 ? "paused, disk sync failed" : rc == -1 ? "the CPUs did not stop" : "not running"), "
                         + "macOS cache \(guestSynced ? "written" : "not confirmed"), \(took) ms")
            DispatchQueue.main.async {
                guard self.mac?.id == mac.id, self.phase == .pausing else { return } // shut down meanwhile
                switch rc {
                case 0, 1:
                    self.phase = .paused(safe: rc == 0, guestSynced: guestSynced, auto: auto)
                    UIApplication.shared.isIdleTimerDisabled = false // a paused Mac lets the iPad lock
                    SessionMarker.write(mac: mac, started: self.started, paused: true, safe: rc == 0)
                case -1:
                    self.phase = .running
                    self.problem = "Couldn't suspend: the Mac didn't stop. It kept running."
                default:
                    self.phase = .running
                    self.problem = "Couldn't suspend: the Mac wasn't running."
                }
            }
            // (The Phase 0 measurements no longer run here: in the background they took ~2 s, and by the time the
            // save after them started iOS had taken the GPU away, so the screen couldn't be saved.)
        }
        return true
    }

    /// On the queue: macOS asked to write what it holds for its disks. true if it confirmed.
    private func syncGuest() -> Bool {
        syncAsked = Date()
        defer { syncEnded = Date() }
        guard henge_share_sync_guest(2500) == 0 else { return false }
        syncDone = Date()
        DispatchQueue.main.async { self.lastGuestSync = Date() }
        return true
    }

    /// Leaving the screen with a save to follow: what only the GPU holds (the windows' contents) is read into
    /// memory right after the pause, for the save to write later. iOS lets Henge use the GPU only until it has
    /// finished leaving (MTLCommandBufferErrorNotPermitted after); the save, after the pause and often seconds
    /// later, is past that. leftScreen waits (until `until`) so leaving isn't finished before this is.
    final class ReadAhead {
        let until: Date
        let done = DispatchSemaphore(value: 0)
        init(until: Date) { self.until = until }
    }

    /// On the queue, paused (else nothing): the read-ahead, then the signal leftScreen waits for. Whatever
    /// happens, the save still goes ahead: without the read-ahead it reads the GPU itself.
    private func readScreenAhead(_ ahead: ReadAhead) {
        defer { ahead.done.signal() }
        guard henge_vm_paused() != 0 else { return }
        // Ended a little before leftScreen stops waiting: all of the GPU's work is handed over by then
        let ms = ahead.until.timeIntervalSinceNow * 1000 - 400
        guard ms >= 200 else {
            HengeLog.log("suspend", "no time left to read the screen ahead; the save reads it itself")
            return
        }
        // At most a quarter of what iOS still lets Henge have: it is held, in the background, until the Mac goes on
        let limit = UInt64(henge_available_memory()) / 4
        var why = [CChar](repeating: 0, count: 200)
        let t0 = Date()
        let rc = henge_vm_capture_gpu(limit, UInt32(ms), &why, why.count)
        let took = Int(Date().timeIntervalSince(t0) * 1000)
        HengeLog.log("suspend", rc == 0 ? "the screen read ahead in \(took) ms"
                     : "the screen wasn't read ahead (\(took) ms): \(String(cString: why)); the save reads it itself")
    }

    func resume() {
        guard let mac, isPaused else { return }
        // Asked to go on while a save runs (Resume Now, or coming back to a save leaving started): it is called
        // off at its next step, keeping nothing, and the Mac goes on after that instead of after the whole save
        if suspending {
            HengeLog.log("suspend", "resumed while the save runs: it is called off")
            henge_vm_suspend_cancel()
        }
        phase = .resuming
        autoSaveCovers = false // from its first instruction it moves on from that auto-save
        queue.async {
            henge_vm_resume() // deletes a suspended state first: from the Mac's first instruction it is out of date
            MacSuspend.forget(mac.id)
            henge_lab_resumed()
            DispatchQueue.main.async {
                guard self.mac?.id == mac.id, self.phase == .resuming else { return }
                self.phase = .running
                self.suspended = false
                self.suspendFailure = nil
                self.lastGuestSync = nil
                UIApplication.shared.isIdleTimerDisabled = true
                SessionMarker.write(mac: mac, started: self.started, paused: false, safe: false)
                // The running time that was left (a pause doesn't count), at least a minute
                if self.autoSaveTimer != nil {
                    self.autoSaveDue = Date().addingTimeInterval(max(60, self.autoSaveLeft ?? MacAutoSave.interval))
                }
                self.autoSaveLeft = nil
                // Henge left the screen while this resumed (leftScreen found it resuming, not running): paused
                // and saved again, as leaving would have, rather than running on in the background.
                if UIApplication.shared.applicationState == .background { self.leftScreen() }
            }
        }
    }

    // MARK: Suspend, asked for

    /// Whether Suspend can be asked for now: the Mac runs or is paused, and isn't suspended or on its way.
    var canSuspend: Bool {
        guard mac != nil, ramMB > 0, cpus > 0, !suspended, !suspending else { return false }
        return phase == .running || isPaused
    }

    /// The person's Suspend: the Mac is paused (if it runs), then saved whole to storage, its open apps and windows
    /// included. It stays paused on the screen until the person resumes or closes it, and coming back to Henge
    /// doesn't resume it by itself. Never worse than a pause: if the save fails, the Mac stays paused and the
    /// paused screen says why.
    func suspend() {
        guard canSuspend else { return }
        suspendFailure = nil
        // Known before anything happens: said, and the Mac left as it is (running or paused).
        if let blocker = MainActor.assumeIsolated({ MacDrives.shared.suspendBlocker }) {
            let text = "Couldn't suspend: \(blocker)."
            if isPaused { suspendFailure = text } else { problem = text }
            HengeLog.log("suspend", "Suspend asked for, refused: \(blocker)")
            return
        }
        switch phase {
        case .running:
            pause(auto: false)
        case let .paused(safe, guestSynced, _):
            phase = .paused(safe: safe, guestSynced: guestSynced, auto: false) // the person's now: no resume on coming back
        default:
            return
        }
        HengeLog.log("suspend", "Suspend asked for")
        save(asked: true)
    }

    /// Closes a suspended Mac and keeps it suspended: the machine stops, its saved state stays, and starting this
    /// Mac again carries on from it. (The engine runs one Mac per launch: the next start opens a fresh Henge.)
    /// false (and nothing changes) unless the Mac is suspended now.
    @discardableResult
    func closeSuspended() -> Bool {
        guard mac != nil, suspended, isPaused, !suspending else { return false }
        mac = nil // stopped() then has nothing to forget
        stopAutoSave()
        phase = .running
        problem = nil
        suspended = false
        suspendFailure = nil
        let task = beginBackgroundTask("Henge close")
        queue.async {
            henge_vm_stop()
            HengeLog.log("suspend", "closed, kept suspended")
            SessionMarker.remove()
            DispatchQueue.main.async { self.endBackgroundTask(task) }
        }
        return true
    }

    // MARK: Leaving the screen

    /// Control Center, a notification, the app switcher, or the start of leaving: macOS writes its cache
    /// now, while it still runs, so a pause that follows has less to wait for.
    private func resigning() {
        guard mac != nil, phase == .running, autoPause, henge_vm_state() == 1 else { return }
        // An auto-save under way: it is about to end, and a pause on leaving is let wait for it (and ends with
        // the Mac paused in its state: no sync needed). Asking now would only queue behind it. (Not held here:
        // Control Center and the like come back without leaving.)
        guard !autoSaving else { return }
        queue.async { _ = self.syncGuest() }
    }

    private func leftScreen() {
        away = true
        holdPorts() // before anything else: also when the Mac isn't paused on leaving
        if mac != nil { Lab.leftScreen() }
        releaseInput()
        guard mac != nil, autoPause else { return }
        // iOS gives a little time to finish after leaving; the pause, sync included, fits in it. A save often
        // doesn't: if iOS's time runs out while it is under way, the person is told to come back.
        let t0 = Date()
        let task = beginBackgroundTask("Henge pause") { [weak self] in
            guard let self, self.suspending else { return }
            SuspendAlert.outOfTime()
        }
        // A save follows (as save(asked: false) decides): the screen is read ahead, while the GPU is still Henge's.
        // iOS gives about 5 s here before it ends Henge; 3.5 s at most is spent, the rest left to spare.
        let saving = MacSuspend.enabled && !suspended && !suspending && ramMB > 0 && cpus > 0
            && MainActor.assumeIsolated({ MacDrives.shared.suspendBlocker }) == nil
        let ahead = saving ? ReadAhead(until: t0.addingTimeInterval(3.5)) : nil
        var reading = false
        if phase == .running {
            reading = pause(auto: true, readAhead: ahead) && ahead != nil
        } else if isPaused, let ahead { // paused before leaving (a Suspend that failed): read now
            reading = true
            queue.async { self.readScreenAhead(ahead) }
        }
        save(asked: false)
        if reading, let ahead {
            let late = ahead.done.wait(timeout: .now() + max(0, ahead.until.timeIntervalSinceNow)) == .timedOut
            // iOS's clock for the background time: a finite figure here means it started before the wait ended
            let left = UIApplication.shared.backgroundTimeRemaining
            HengeLog.log("suspend", String(format: "leaving waited %.0f ms for the screen to be read%@; iOS gives %@",
                                           Date().timeIntervalSince(t0) * 1000, late ? ", and it wasn't done" : "",
                                           left > 1e6 ? "no limit yet" : String(format: "%.1f s", left)))
        }
        if suspending { SuspendAlert.outOfTimeLater() } // this save, or one asked for just before leaving
        queue.async { DispatchQueue.main.async { self.endBackgroundTask(task) } } // after the pause and the suspend
    }

    private func cameBack() {
        away = false
        holdPorts() // a Mac that went on running while away (auto-pause off), or one still paused: as its phase says
        Lab.cameBack()
        SuspendAlert.clear() // the person is here: the screen says the rest
        guard mac != nil else { return }
        // Back while the save leaving started is still under way: the Mac goes on at once rather than after it
        // (a minute for a big Mac), so the save is called off; it stops at its next step, keeping nothing. The
        // resume below waits for that, not for the save. The person's own Suspend is never called off.
        // (Not waiting for the pause to say it's done: a quick return lands before it does. Not over a Mac the
        // person had paused, or one whose Suspend failed: that one isn't resumed on return, so calling its save
        // off would leave it unsaved.)
        var leavingPaused = phase == .pausing
        if case .paused(_, _, true) = phase { leavingPaused = true }
        if suspending, savingOnLeave, autoResume, leavingPaused {
            HengeLog.log("suspend", "back while the save on leaving runs: it is called off, and the Mac goes on")
            henge_vm_suspend_cancel()
        }
        // After any pause still in progress, so a quick return resumes what leaving paused.
        queue.async {
            DispatchQueue.main.async {
                // Still on the screen: left again meanwhile, it stays paused (and saved) for the next return.
                // A save on leaving that failed is not resumed over: the person sees "Not Suspended" and why,
                // instead of finding out only when iOS closes Henge and the open apps are gone.
                guard case .paused(_, _, true) = self.phase, self.autoResume, self.suspendFailure == nil,
                      UIApplication.shared.applicationState == .active else { return }
                self.resume()
            }
        }
    }

    /// After the pause on the queue, the paused Mac is saved whole as well, so iOS closing Henge loses nothing:
    /// on leaving the screen (asked: false, if Suspend is on in Settings), or when the person taps Suspend
    /// (asked: true, which also says why if it fails). Never worse than the pause: if the save fails, the Mac
    /// stays paused as it was.
    private func save(asked: Bool) {
        guard let mac, asked || MacSuspend.enabled, !suspended, !suspending, phase == .pausing || isPaused, ramMB > 0, cpus > 0 else { return }
        // Called on the main thread (leftScreen, suspend).
        suspendFailure = nil // a failure from an earlier try is out of date
        if let blocker = MainActor.assumeIsolated({ MacDrives.shared.suspendBlocker }) {
            HengeLog.log("suspend", "not suspended, only paused: \(blocker)")
            suspendFailure = "Couldn't suspend: \(blocker)."
            return
        }
        suspending = true // one save at a time: a second would delete the first before writing it again
        savingOnLeave = !asked
        let ticket = henge_vm_suspend_ticket() // a cancel from now on (coming back) calls this save off
        let plugged = MainActor.assumeIsolated { MacDrives.shared.pluggedForSuspend() }
        let info = MacSuspend.Saved(macID: mac.id, saved: Date(), ramMB: ramMB, cpus: cpus, drives: plugged.drives, tools: plugged.tools != nil)
        let started = self.started
        let leaving = UIApplication.shared.applicationState == .background
        let t0 = Date()
        if leaving { HengeLog.log("suspend", String(format: "saving on leaving the screen, iOS gives %.1f s", UIApplication.shared.backgroundTimeRemaining)) }
        queue.async {
            guard henge_vm_paused() != 0 else { // the pause didn't happen: the Mac runs
                DispatchQueue.main.async { self.suspending = false; SuspendAlert.done() }
                return
            }
            let (ok, why, stopped) = MacSuspend.save(info, tools: plugged.tools, ticket: ticket)
            // Called off, however it ended (a step that failed as it was being called off included)
            let cancelled = stopped || (!ok && henge_vm_suspend_ticket() != ticket)
            SuspendAlert.takeBackOutOfTime() // now, in case iOS stops Henge before the main thread gets to it
            DispatchQueue.main.async {
                if leaving {
                    let left = UIApplication.shared.backgroundTimeRemaining
                    HengeLog.log("suspend", String(format: "the save on leaving %@ after %.1f s, %@", ok ? "finished" : cancelled ? "was called off" : "failed",
                                                   Date().timeIntervalSince(t0), left > 1e6 ? "Henge was back by then" : String(format: "%.1f s of iOS's time left", left)))
                }
                self.suspending = false
                self.savingOnLeave = false
                SuspendAlert.done()
                guard self.mac?.id == mac.id, self.isPaused else { return } // resumed meanwhile: that deleted it
                // Called off because the person came back: not a failure; cameBack resumes the Mac next. Gone
                // again since (the Mac never went on): saved after all, as leaving would have.
                guard !cancelled else {
                    if UIApplication.shared.applicationState == .background { self.saveAgainOnLeaving() }
                    return
                }
                guard ok else {
                    if self.autoSaveCovers && !asked {
                        // Nothing at risk: the auto-save taken as it paused is this paused Mac exactly, and starting it
                        // after iOS closes Henge carries on from there without asking (its disk unchanged since).
                        HengeLog.log("suspend", "not suspended (\(why)), but its auto-save from as it paused holds it exactly: nothing is at risk")
                        return
                    }
                    var reason = why.trimmingCharacters(in: CharacterSet(charactersIn: ". "))
                    // The engine says whole sentences ("The Mac didn't stop in time."); here it follows a colon
                    // ("The Mac…" → "the Mac…"; "Mac", "GPU", "IPad" left alone)
                    let start = Array(reason.prefix(3))
                    if start.count > 1, start[0].isUppercase, start[1].isLowercase, !reason.hasPrefix("Mac") {
                        reason = reason.prefix(1).lowercased() + reason.dropFirst()
                    } else if reason.hasPrefix("IPad") {
                        reason = "i" + reason.dropFirst()
                    }
                    self.suspendFailure = "Couldn't suspend" + (reason.isEmpty ? "." : ": \(reason).")
                        + (self.autoSaveCovers ? " It was auto-saved as it paused, so if Henge closes it carries on from here." : "")
                    return
                }
                self.suspended = true
                SessionMarker.write(mac: mac, started: started, paused: true, safe: true, suspended: true)
            }
            if ok { MacSuspend.changed() } // its auto-save stays: once it resumes, the Suspend goes, and that is what's left
            if !ok && !cancelled { HengeLog.log("suspend", "not suspended, only paused: \(why)") }
            if cancelled { HengeLog.log("suspend", "the save was called off (Henge is back): nothing kept, the Mac goes on") }
        }
    }

    /// The save on leaving was called off by a return, and Henge has left the screen again before the Mac went
    /// on (leaving didn't start a save then: one was still under way). The screen read ahead is still exact.
    private func saveAgainOnLeaving() {
        guard isPaused else { return }
        HengeLog.log("suspend", "left again while the save was being called off: saving it after all")
        let task = beginBackgroundTask("Henge suspend") { [weak self] in
            guard let self, self.suspending else { return }
            SuspendAlert.outOfTime()
        }
        save(asked: false)
        if suspending { SuspendAlert.outOfTimeLater() }
        queue.async { DispatchQueue.main.async { self.endBackgroundTask(task) } }
    }

    // MARK: Auto-save

    private func startAutoSave() {
        stopAutoSave()
        autoSaveNote = nil
        guard MacAutoSave.on, let mac else { return }
        let now = Date()
        // Carried on from its auto-save: that one is the Mac as it is now. Otherwise (started fresh, or from a
        // Suspend: any from before was deleted as it started) the first comes soon, two minutes in.
        let carried = MainActor.assumeIsolated { () -> Bool in
            defer { MacAutoSave.carriedOn = nil }
            return MacAutoSave.carriedOn == mac.id
        }
        let kept = MacAutoSave.saved(for: mac.id)?.saved
        autoSaveDue = now.addingTimeInterval(carried ? MacAutoSave.interval : 120)
        lastAutoSave = carried ? now : .distantPast // (only then is "nothing changed since the last" a fair skip)
        lastDiskChange = diskChanged(mac)
        frameSamples = []
        autoSaveWaitSaid = false
        autoSaveNotSaid = nil
        autoSaveLeft = nil
        autoSaveNote = kept.map { "Last at \($0.formatted(date: .omitted, time: .shortened))" } ?? "None yet"
        autoSaveTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.autoSaveTick() }
        let id = mac.id
        roomWatch.start(id) { [weak self] why in
            // On the pause queue, after any auto-save under way (it may be putting a new one in place)
            self?.queue.async {
                guard MacAutoSave.exists(id) else { return }
                // Not while it is the paused Mac exactly (covering a Suspend that didn't happen): a paused Mac
                // writes nothing, so it needs no room; it is looked at again once the Mac runs.
                guard !(henge_vm_paused() != 0 && SuspendAlert.covered) else { return }
                MacAutoSave.forget(id, why)
                DispatchQueue.main.async {
                    guard let self, self.mac?.id == id else { return }
                    self.autoSaveNote = "Deleted: this \(Device.name) is nearly full"
                    self.autoSaveCovers = false
                }
            }
        }
        HengeLog.log("autosave", "on: every \(Int(MacAutoSave.interval / 60)) minutes of running on the screen, the next in "
                     + "\(Int(autoSaveDue.timeIntervalSince(now))) s\(carried ? " (carried on from the last)" : kept != nil ? " (the one kept is from before)" : "")")
    }

    private func stopAutoSave() {
        autoSaveTimer?.invalidate()
        autoSaveTimer = nil
        autoSaveDue = .distantFuture
        roomWatch.stop()
        cancelAutoSave("the session ended")
    }

    /// Shut Down or Restart was asked of macOS (MacPower): an auto-save under way is called off, as it would finish
    /// with apps quitting. None is taken while macOS quits them (autoSaveTick); the one before is what is kept.
    func powerAsked() {
        cancelAutoSave("the Mac is to shut down or restart")
    }

    /// Calls off one under way (it stops at its next step, keeping nothing; the one before stays), so what comes
    /// next on the queue (a shut down, closing) starts at once rather than after it.
    private func cancelAutoSave(_ why: String) {
        guard autoSaving else { return }
        HengeLog.log("autosave", "called off: \(why)")
        henge_vm_suspend_cancel()
    }

    /// One under way is let finish, and leaves the Mac paused rather than running on to be paused again after it:
    /// a pause asked for meanwhile (leaving the screen, Suspend) waits only for the rest of it (it is mostly done
    /// by the time anyone asks: at most a second or two), and then that auto-save is the paused Mac exactly.
    private func holdAutoSave(_ why: String) {
        guard autoSaving else { return }
        HengeLog.log("autosave", "to finish and leave the Mac paused: \(why)")
        henge_vm_autosave_hold()
    }

    private func diskChanged(_ mac: VirtualMac) -> Date? {
        (try? FileManager.default.attributesOfItem(atPath: mac.disk.path))?[.modificationDate] as? Date
    }

    /// Not taken this time, for a reason that may last: logged once (until another reason), said in the details,
    /// and looked at again in `retry` seconds.
    private func autoSaveNotNow(_ why: String, retry: TimeInterval) {
        if autoSaveNotSaid != why {
            autoSaveNotSaid = why
            HengeLog.log("autosave", "not taken: \(why)")
        }
        autoSaveNote = "Waiting: \(why)"
        autoSaveDue = Date().addingTimeInterval(retry)
    }

    /// iPadOS says memory is critical (MemoryWatch, main thread): with auto-save on, the next one is taken at the
    /// next tick, without waiting for a still moment, unless one is under way or the last is recent. Says what it did.
    func autoSaveForMemory() -> String {
        guard MacAutoSave.on, mac != nil else { return "auto-save is off" }
        if autoSaving { return "an auto-save is under way" }
        let age = Date().timeIntervalSince(lastAutoSave)
        if age < 120 { return "the last auto-save is \(Int(age)) s old" }
        guard phase == .running else { return "the Mac is paused: no auto-save" }
        if MainActor.assumeIsolated({ MacPower.shared.active }) { return "a shut down or restart is under way: no auto-save" }
        autoSaveDue = Date().addingTimeInterval(-MacAutoSave.overdue) // overdue: taken anyway
        return "an auto-save is to be taken now"
    }

    /// Every second on the main thread while a Mac is attached.
    private func autoSaveTick() {
        let now = Date()
        frameSamples.append((now, henge_vm_gpu_frames()))
        frameSamples.removeAll { now.timeIntervalSince($0.at) > 3.5 }
        guard let mac else { return }
        guard now >= autoSaveDue, !autoSaving, phase == .running, !suspending, !suspended, ramMB > 0, cpus > 0,
              henge_vm_state() == 1, henge_vm_paused() == 0,
              UIApplication.shared.applicationState == .active else { return }
        let late = now >= autoSaveDue.addingTimeInterval(MacAutoSave.overdue)
        // Shutting down or restarting (asked of macOS): its apps may be quitting. An auto-save then would replace the
        // one from before with a Mac half shut down, which a Force Shut Down would then carry on from.
        if MainActor.assumeIsolated({ MacPower.shared.active }) {
            return autoSaveNotNow("a shut down or restart is under way", retry: 10)
        }
        // What would surely stop it, first (each looked at again later; none is a failure)
        if henge_vm_autosave_possible() == 0 {
            return autoSaveNotNow("the GPU holds some of the Mac's memory where Henge can't copy it quickly", retry: MacAutoSave.interval)
        }
        if let blocker = MainActor.assumeIsolated({ MacDrives.shared.suspendBlocker }) {
            return autoSaveNotNow(blocker, retry: 60)
        }
        let plugged = MainActor.assumeIsolated { MacDrives.shared.pluggedForSuspend() }
        if let drive = plugged.drives.first(where: { !$0.readOnly }) {
            // Its writes after the auto-save couldn't be put back with it (the engine refuses one whose drive changed)
            return autoSaveNotNow("the USB drive \"\(drive.name)\" is in and can be written to", retry: 60)
        }
        // A hot iPad isn't made hotter (the copy is mostly storage work), unless it is overdue
        if ProcessInfo.processInfo.thermalState.rawValue >= ProcessInfo.ThermalState.serious.rawValue && !late {
            return autoSaveNotNow("this \(Device.name) is hot", retry: 60)
        }
        autoSaveNotSaid = nil
        // Placed where it isn't felt: no input for 3 s and a still screen (at most 6 frames in 3 s), unless overdue
        let idle = henge_vm_input_idle_s()
        let frames = frameSamples.first.map { henge_vm_gpu_frames() &- $0.frames } ?? 0
        let span = frameSamples.first.map { now.timeIntervalSince($0.at) } ?? 0
        let still = span >= 2.5 && frames <= 6
        guard (idle < 0 || idle >= 3) && still || late else {
            if !autoSaveWaitSaid {
                autoSaveWaitSaid = true
                HengeLog.log("autosave", "due: waiting for a still moment (input \(idle < 0 ? "none" : String(format: "%.0f s ago", idle)), \(frames) frames in 3 s)")
            }
            return
        }
        autoSaveWaitSaid = false
        // Nothing new since the last one: no input from the person, and the disk not written
        let changed = diskChanged(mac)
        if idle >= 0 && idle > now.timeIntervalSince(lastAutoSave) && changed == lastDiskChange && MacAutoSave.exists(mac.id) {
            HengeLog.log("autosave", "skipped: nothing changed since the last one")
            autoSaveDue = now.addingTimeInterval(MacAutoSave.interval)
            return
        }
        autoSaving = true
        let ticket = henge_vm_suspend_ticket() // shutting down or closing from now on calls it off
        let hold = henge_vm_autosave_hold_ticket() // a pause from now on lets it finish, and it leaves the Mac paused
        let info = MacSuspend.Saved(macID: mac.id, saved: now, ramMB: ramMB, cpus: cpus, drives: plugged.drives, tools: plugged.tools != nil)
        let why = late ? "overdue, taken anyway" : idle < 0 ? "no input yet, a still screen" : String(format: "no input for %.0f s, a still screen", idle)
        HengeLog.log("autosave", "taking one (\(why))")
        queue.async {
            let t0 = Date()
            let result = henge_vm_paused() == 0 ? MacAutoSave.take(info, tools: plugged.tools, ticket: ticket, hold: hold)
                : MacAutoSave.Result(ok: false, why: "the Mac was paused first", cancelled: true)
            // Left paused (a pause was asked for as it ran): kept, it is the paused Mac exactly
            let held = henge_vm_paused() != 0
            if result.ok && held { SuspendAlert.covered = true } // now, before anything on the main thread
            let took = Int(Date().timeIntervalSince(t0) * 1000)
            HengeLog.log("autosave", result.ok ? "kept, \(took) ms in all\(held ? ", and the Mac stays paused (asked for meanwhile)" : "")"
                         : result.cancelled ? "called off after \(took) ms; the one before is kept"
                         : "not taken (\(took) ms): \(result.why); the one before is kept")
            DispatchQueue.main.async {
                self.autoSaving = false
                guard self.mac?.id == mac.id else { return }
                let done = Date()
                if result.ok {
                    self.lastAutoSave = done
                    self.lastDiskChange = changed
                    self.autoSaveNote = "Last at \(info.saved.formatted(date: .omitted, time: .shortened))"
                    // (Set either way: the queue said so already, and a resume since takes it back)
                    if held { self.autoSaveCovers = self.phase != .running && self.phase != .resuming }
                } else if !result.cancelled {
                    self.autoSaveNote = "The last try didn't work: \(result.why)"
                }
                // A failure is tried again at the next interval, not at once: the same cause would stop it again
                let next = result.cancelled ? 60 : MacAutoSave.interval
                self.autoSaveDue = done.addingTimeInterval(next)
                if self.phase != .running { self.autoSaveLeft = next } // paused meanwhile: counted from the resume
            }
        }
    }

    private var autoPause: Bool { UserDefaults.standard.bool(forKey: Self.autoPauseKey) }
    private var autoResume: Bool { UserDefaults.standard.bool(forKey: Self.autoResumeKey) }

    /// Keys and buttons held down are let go first: a paused Mac must not wake up thinking one still is. What Henge
    /// was still typing goes too: its keys would be lost on the way (a paused Mac takes none), leaving half of it.
    private func releaseInput() {
        MainActor.assumeIsolated { TypingQueue.shared.drop([.dictation, .password, .other]) } // on the main queue, as the phase is
        StickyKeys.shared.releaseAll()
        HardwareKeys.releaseAll()
        HardwareMouse.shared.releaseButtons()
    }

    private func beginBackgroundTask(_ name: String, expiring: (() -> Void)? = nil) -> UIBackgroundTaskIdentifier {
        var task = UIBackgroundTaskIdentifier.invalid
        task = UIApplication.shared.beginBackgroundTask(withName: name) {
            HengeLog.log("pause", "\(name): iOS ended the time it gave")
            expiring?()
            self.endBackgroundTask(task)
        }
        return task
    }

    private func endBackgroundTask(_ task: UIBackgroundTaskIdentifier) {
        guard task != .invalid else { return }
        UIApplication.shared.endBackgroundTask(task)
    }
}

/// A notification when a Suspend fails, or can't finish, while Henge isn't on the screen: the Mac is then held only in
/// memory, and iOS may close Henge at any time, losing its open apps. Tapping it brings Henge back, where the paused
/// screen says why and offers Try Again. On the screen, nothing is posted: the screen and VoiceOver say it.
/// Main thread only (MacPause calls it there), checked.
enum SuspendAlert {
    private static let id = "henge.suspend.failed"
    /// What was posted since the person last saw Henge: the same again is not posted, and only the first sounds.
    nonisolated(unsafe) private static var posted: String?
    nonisolated(unsafe) private static var sounded = false
    /// The "still being suspended" warning is waiting for iOS's time to run out (outOfTimeLater).
    nonisolated(unsafe) private(set) static var outOfTimeWaiting = false
    /// Set by takeBackOutOfTime (on the pause queue, the moment a save ends): the warning isn't set again after it.
    private static let takenBackLock = NSLock()
    nonisolated(unsafe) private static var takenBack = false
    /// The paused Mac is held whole by its auto-save (MacPause.autoSaveCovers): a save after it failing, or not
    /// finishing before iOS ends Henge, loses nothing, so neither is warned of. Any thread.
    nonisolated(unsafe) private static var coveredNow = false
    static var covered: Bool {
        get { takenBackLock.withLock { coveredNow } }
        set {
            takenBackLock.withLock {
                coveredNow = newValue
                if newValue { UNUserNotificationCenter.current().removePendingNotificationRequests(withIdentifiers: [id]) }
            }
        }
    }

    /// Asked once, when a Mac starts with saving on leaving turned on: the moment the person can see why.
    static func askOnce() {
        let centre = UNUserNotificationCenter.current()
        centre.getNotificationSettings { settings in
            guard settings.authorizationStatus == .notDetermined else { return }
            centre.requestAuthorization(options: [.alert, .sound]) { granted, _ in
                HengeLog.log("suspend", "notifications for a failed Suspend: \(granted ? "allowed" : "not allowed")")
            }
        }
    }

    /// A save (or the pause before it) failed with Henge off the screen. stillRunning: the pause itself failed, so
    /// the Mac goes on running; said only if saving on leaving is on (otherwise nothing was promised).
    static func failed(_ reason: String, stillRunning: Bool) {
        dispatchPrecondition(condition: .onQueue(.main))
        guard UIApplication.shared.applicationState != .active, !stillRunning || MacSuspend.enabled, stillRunning || !covered else { return }
        post(title: stillRunning ? "Your Mac wasn't suspended" : "Your Mac wasn't saved",
             body: stillRunning
                ? "\(reason) Open Henge to check on it."
                : "\(reason) It's paused in memory only, and iOS may close Henge at any time. Open Henge and tap Try Again, or resume and save your work.")
    }

    /// A save under way when Henge leaves the screen: if iOS's time runs out first, Henge is stopped and can't
    /// post anything then, so the warning is handed to iOS now, to show a moment after that time is up. The end
    /// of the save (done) takes it back. Without a known limit, the end of iOS's time posts it (outOfTime).
    static func outOfTimeLater() {
        dispatchPrecondition(condition: .onQueue(.main))
        guard UIApplication.shared.applicationState != .active else { return } // on the screen: no limit
        guard !covered else {
            HengeLog.log("suspend", "no out-of-time warning: the auto-save taken as it paused holds the Mac")
            return
        }
        let left = UIApplication.shared.backgroundTimeRemaining
        // Just after leaving, iOS often doesn't say yet ("no limit"): about 30 s is what it gives, so the
        // warning is set for then, and set again for the time iOS says as soon as it says it.
        let known = left < 3600
        let after = known ? max(1, left + 1) : Self.usualTime
        outOfTimeWaiting = true
        takenBackLock.withLock { takenBack = false }
        let (title, body) = outOfTimeText
        posted = nil
        schedule(title: title, body: body, sound: !sounded, after: after)
        HengeLog.log("suspend", String(format: "the out-of-time warning is set for %.1f s from now%@", after, known ? "" : " (iOS hasn't said yet)"))
        if !known { DispatchQueue.main.asyncAfter(deadline: .now() + 1) { outOfTimeAgain(tries: 10) } }
    }

    /// What iOS usually gives after leaving, when it hasn't said: a little over its ~30 s.
    private static let usualTime: TimeInterval = 31

    /// The warning set for iOS's usual time, set again for the time it says, once it says (every second, a few times).
    private static func outOfTimeAgain(tries: Int) {
        guard outOfTimeWaiting, UIApplication.shared.applicationState != .active else { return }
        let left = UIApplication.shared.backgroundTimeRemaining
        guard left < 3600 else {
            if tries > 1 { DispatchQueue.main.asyncAfter(deadline: .now() + 1) { outOfTimeAgain(tries: tries - 1) } }
            return
        }
        let (title, body) = outOfTimeText
        // Under the lock: a save that ends meanwhile takes this one back too
        takenBackLock.withLock {
            guard !takenBack, !coveredNow else { return }
            schedule(title: title, body: body, sound: !sounded, after: max(1, left + 1)) // the same id: it replaces the first
        }
        HengeLog.log("suspend", String(format: "the out-of-time warning is set again, for %.1f s from now (as iOS says)", max(1, left + 1)))
    }

    /// iOS's time after leaving ran out with the save under way, and no warning was set beforehand.
    static func outOfTime() {
        dispatchPrecondition(condition: .onQueue(.main))
        guard UIApplication.shared.applicationState != .active, !outOfTimeWaiting, !covered else { return }
        let (title, body) = outOfTimeText
        post(title: title, body: body)
    }

    private static var outOfTimeText: (String, String) {
        ("Your Mac is still being suspended",
         "iOS stopped Henge before the save finished, so it's paused in memory only. Open Henge to check on it; you may need to tap Try Again.")
    }

    /// From any thread, as soon as a save ends: the warning set for when iOS's time runs out is taken back
    /// (done() then tidies up on the main thread).
    static func takeBackOutOfTime() {
        takenBackLock.withLock {
            takenBack = true
            UNUserNotificationCenter.current().removePendingNotificationRequests(withIdentifiers: [id])
        }
    }

    /// The save ended (saved or not): a warning set for when iOS's time runs out is out of date.
    static func done() {
        dispatchPrecondition(condition: .onQueue(.main))
        guard outOfTimeWaiting else { return }
        outOfTimeWaiting = false
        UNUserNotificationCenter.current().removePendingNotificationRequests(withIdentifiers: [id])
    }

    /// Henge is back, or the Mac was saved after all: any warning is out of date.
    static func clear() {
        dispatchPrecondition(condition: .onQueue(.main))
        posted = nil
        sounded = false
        outOfTimeWaiting = false
        let centre = UNUserNotificationCenter.current()
        centre.removeDeliveredNotifications(withIdentifiers: [id])
        centre.removePendingNotificationRequests(withIdentifiers: [id])
    }

    private static func post(title: String, body: String) {
        let key = title + "\n" + body
        guard posted != key else { return } // already said
        posted = key
        outOfTimeWaiting = false // replaced by this one
        schedule(title: title, body: body, sound: !sounded, after: 0)
        sounded = true
    }

    private static func schedule(title: String, body: String, sound: Bool, after: TimeInterval) {
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        if sound { content.sound = .default }
        content.interruptionLevel = .active
        let trigger = after > 0 ? UNTimeIntervalNotificationTrigger(timeInterval: after, repeats: false) : nil
        // One at a time: a newer one replaces the last
        UNUserNotificationCenter.current().add(UNNotificationRequest(identifier: id, content: content, trigger: trigger)) { error in
            HengeLog.log("suspend", "notification \"\(title)\": \(error.map { "not posted (\($0.localizedDescription))" } ?? (after > 0 ? "set" : "posted"))")
        }
    }
}

/// Suspend lab (Phase 0): measurements for the Suspend plan, logged as "lab" (henge.log) and "lab:" lines
/// (henge-native.log). Nothing here changes what the Mac sees or how pausing works.
enum Lab {
    /// Main thread only: a background task held to see how long iOS really gives, and when it was taken.
    nonisolated(unsafe) private static var probe = UIBackgroundTaskIdentifier.invalid
    nonisolated(unsafe) private static var probeStart = Date()
    nonisolated(unsafe) private static var probeTimer: Timer?
    nonisolated(unsafe) private static var left: Date?

    static func started() {
        var gpu = "unknown"
        if #available(iOS 26.0, *) { gpu = BGTaskScheduler.supportedResources.contains(.gpu) ? "yes" : "no" }
        let info = ProcessInfo.processInfo
        HengeLog.log("lab", "on: background GPU work offered by iOS (BGContinuedProcessingTask .gpu): \(gpu); thermal \(info.thermalState.rawValue); "
                     + "low power \(info.isLowPowerModeEnabled); free memory \(henge_available_memory() >> 20) MB")
    }

    /// Henge left the screen: hold a background task to its end to see how long iOS gives, logging as it goes.
    static func leftScreen() {
        left = Date()
        guard probe == .invalid else { return }
        probeStart = Date()
        probe = UIApplication.shared.beginBackgroundTask(withName: "Henge lab: background time") {
            HengeLog.log("lab", String(format: "background: iOS ended the time it gave after %.1f s", Date().timeIntervalSince(probeStart)))
            endProbe()
        }
        HengeLog.log("lab", "background: left the screen, iOS says \(remaining())")
        probeTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { _ in
            HengeLog.log("lab", String(format: "background: %.0f s in, %@", Date().timeIntervalSince(probeStart), remaining()))
        }
    }

    /// The background time iOS says is left. While Henge is still in the foreground (the moment it starts to leave,
    /// or back) iOS says the largest number there is: no limit.
    private static func remaining() -> String {
        let left = UIApplication.shared.backgroundTimeRemaining
        return left > 1e6 ? "no limit (in the foreground)" : String(format: "%.1f s remain", left)
    }

    static func cameBack() {
        if let left {
            HengeLog.log("lab", String(format: "background: back after %.1f s away%@", Date().timeIntervalSince(left),
                                       probe == .invalid ? "" : ", the time iOS gave had not run out"))
        }
        left = nil
        endProbe()
    }

    private static func endProbe() {
        probeTimer?.invalidate()
        probeTimer = nil
        guard probe != .invalid else { return }
        UIApplication.shared.endBackgroundTask(probe)
        probe = .invalid
    }
}

/// A note that a Mac is running, kept while it does: if Henge opens and finds one, iOS closed Henge (or it
/// failed) with the Mac on, and the person is told what that means for their work.
struct SessionMarker: Codable {
    var macID: UUID
    var name: String
    var started: Date
    var paused: Bool
    var safe: Bool
    /// Saved whole (Suspend): it goes on from where it was. Optional: markers from before Suspend lack it.
    var suspended: Bool?
    /// When iPadOS last said memory was critical while it ran (MemoryWatch), and the Mac's memory then: a close
    /// soon after was most likely iPadOS freeing memory. Optional: older markers lack them.
    var memoryCritical: Date?
    var ramMB: UInt64?

    private static var url: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("running-session.json")
    }

    static func write(mac: VirtualMac, started: Date, paused: Bool, safe: Bool, suspended: Bool = false) {
        let marker = SessionMarker(macID: mac.id, name: mac.name, started: started, paused: paused, safe: safe, suspended: suspended)
        marker.save()
    }

    private func save() {
        var marker = self
        marker.memoryCritical = MemoryWatch.criticalAt
        marker.ramMB = MemoryWatch.criticalAt != nil ? MemoryWatch.guestRAMMB : nil
        guard let data = try? JSONEncoder().encode(marker) else { return }
        try? FileManager.default.createDirectory(at: Self.url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? data.write(to: Self.url, options: .atomic)
    }

    /// MemoryWatch.criticalAt changed: the running Mac's note (if one is written) takes it.
    static func noteMemory() {
        guard let data = try? Data(contentsOf: url), let marker = try? JSONDecoder().decode(SessionMarker.self, from: data) else { return }
        marker.save()
    }

    static func remove() { try? FileManager.default.removeItem(at: url) }

    /// The marker the last run of Henge left. Read once, as Henge opens (HengeApp.init), and removed then:
    /// a view that asks again later gets the same answer, never a Mac running now.
    static let atLaunch = takeLeftover()

    private static func takeLeftover() -> SessionMarker? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        remove()
        return try? JSONDecoder().decode(SessionMarker.self, from: data)
    }

    /// Suspended, and the saved state is still there to go on from.
    var resumable: Bool { suspended == true && MacSuspend.hasSaved(for: macID) }

    /// Not suspended, but auto-saved: starting it offers to carry on from then.
    var autoSaved: Date? { resumable ? nil : MacAutoSave.saved(for: macID)?.saved }

    /// Closed while iPadOS was short of memory (and not suspended, which a close then costs nothing).
    var closedForMemory: Bool { memoryCritical != nil && !resumable }

    var title: String {
        closedForMemory ? "iPadOS closed Henge to free memory" : resumable ? "\(name) is suspended" : autoSaved != nil ? "\(name) was running when Henge closed" : paused ? "\(name) wasn't suspended when Henge closed" : "\(name) was running when Henge closed"
    }

    var message: String {
        guard closedForMemory else { return stateMessage }
        let gb = Int((ramMB ?? 0) / 1024), most = Device.suggestedRAMGB
        var why = "This \(Device.name) ran out of memory while \(name) was running, and iPadOS closed Henge to get it back. "
        if gb > most {
            why += "\(name) has \(gb) GB, more than the \(most) GB Henge suggests for this \(Device.name). Giving it \(most) GB in its "
                + "settings (shut it down first) makes this much less likely. "
        } else {
            why += "Closing other apps before starting \(name), or giving it less memory in its settings, makes this less likely. "
        }
        return why + "\n\n" + stateMessage
    }

    private var stateMessage: String {
        if let autoSaved {
            let when = autoSaved.formatted(date: .omitted, time: .shortened)
            if MacAutoSave.untouched(macID) {
                return "Nothing is lost: it was auto-saved at \(when) and its disk hasn't changed since. Start it and it carries on "
                    + "exactly from there, with its apps and windows open."
            }
            return "Henge closed while the Mac was running. It was auto-saved at \(when): start it and choose whether to carry on "
                + "from then, with its apps and windows open (anything saved on the Mac after \(when) is gone), or to start it fresh "
                + "with its disk as it was when Henge closed."
        }
        if resumable {
            return "iOS closed Henge while the Mac was suspended. Nothing is lost: start it and it carries on exactly where it was, "
                + "with its apps and windows as you left them."
        }
        if paused && safe {
            return "iOS closed Henge before the whole Mac was saved. Its files are on this \(Device.name), but it wasn't suspended. "
                + "Start it again: macOS starts fresh, and apps that were open, and anything not saved in them, are gone. "
                + "Next time, tap Suspend and wait for it to say Suspended before leaving Henge."
        }
        if paused {
            return "iOS closed Henge before the whole Mac was saved, and Henge couldn't confirm all of its disk writes reached storage. "
                + "Start it again and check your recent work. macOS may check its disk as it starts."
        }
        return "Henge closed while the Mac was running, so it was turned off without shutting down. Start it again: "
            + "macOS checks its disk as it starts, and work that wasn't saved may be lost. "
            + "Tap Suspend and wait for it to say Suspended before leaving Henge."
    }
}

// MARK: - Suspend

/// Suspend: when Henge leaves the screen with the Mac paused, the whole Mac (its memory, processors, devices and
/// what only the GPU holds) is saved to this device's storage as well. If iOS then closes Henge, starting the Mac
/// again carries on from exactly where it was, apps and windows open. Coming back while Henge is still open just
/// resumes, and the saved copy is deleted then: from the Mac's first instruction it is out of date, and loading it
/// later would wreck the disk. The engine deletes it as it resumes or restores; this tidies what goes with it.
///
/// One folder per Mac, in Application Support/Suspended/<id>: Mac.hvsnap (the engine's snapshot, written aside and
/// renamed into place, so it is all there or not at all), suspended.json (what the app needs to build the Mac the
/// same way: memory, processors, the drives it had) and, if it was in, the Henge Tools disk as it was (a hard link:
/// a newer Henge unpacks a new one beside it, and the suspended Mac needs the very file it had).
enum MacSuspend {
    static let autoKey = "henge.suspend.auto"
    static var enabled: Bool { UserDefaults.standard.bool(forKey: autoKey) }

    struct Saved: Codable {
        struct Drive: Codable {
            var id: UUID
            var name: String
            var serial: String
            var readOnly: Bool
        }
        var macID: UUID
        var saved: Date
        var ramMB: UInt64
        var cpus: Int32
        var drives: [Drive]
        var tools: Bool
        /// Restores begun that didn't finish (counted before each, so a crash counts too): after two the person is asked.
        var failures = 0
        /// Henge turned the Mac off (Force Shut Down) after this auto-save: carrying on from it is always asked, as
        /// the person had chosen to turn the Mac off. Missing in notes from before it (nil).
        var forcedOff: Bool? = nil
    }

    private static var root: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("Suspended", isDirectory: true)
    }
    static func folder(_ id: UUID) -> URL { root.appendingPathComponent(id.uuidString, isDirectory: true) }
    static func stateURL(_ id: UUID) -> URL { folder(id).appendingPathComponent("Mac.hvsnap") }
    private static func infoURL(_ id: UUID) -> URL { folder(id).appendingPathComponent("suspended.json") }
    private static func toolsURL(_ id: UUID) -> URL { folder(id).appendingPathComponent("Henge Tools.img") }
    /// The Henge Tools disk a resumed Mac has in, kept by name while it runs: the very file it had (same inode and
    /// time), which a later Suspend keeps again. Henge unpacks a new one each launch, which a restore wouldn't take.
    static func runningToolsURL(_ id: UUID) -> URL { root.appendingPathComponent("\(id.uuidString) Henge Tools.img") }
    /// A resumed Mac's Henge Tools file, once nothing runs from it (MacDrives lets it go when the disk comes out).
    static func dropRunningTools(_ id: UUID) { try? FileManager.default.removeItem(at: runningToolsURL(id)) }

    /// The suspended Mac to go on from, if there is one: its snapshot and its note are both there.
    static func saved(for id: UUID) -> Saved? {
        guard FileManager.default.fileExists(atPath: stateURL(id).path),
              let data = try? Data(contentsOf: infoURL(id)),
              let saved = try? JSONDecoder().decode(Saved.self, from: data), saved.macID == id else { return nil }
        return saved
    }

    static func hasSaved(for id: UUID) -> Bool { saved(for: id) != nil }

    /// Everything kept for the Mac: the snapshot first, its deletion on storage before the rest goes.
    static func forget(_ id: UUID) {
        let state = stateURL(id).path
        if henge_vm_discard_saved(state) != 0 { HengeLog.log("suspend", "the saved state couldn't be deleted") }
        guard FileManager.default.fileExists(atPath: folder(id).path) else { return }
        try? FileManager.default.removeItem(at: folder(id))
        changed()
    }

    /// At launch, before anything runs: a suspend Henge was closed in the middle of has no snapshot (it is renamed into
    /// place only once complete), and can hold a copy of the Mac's memory as large as the memory: deleted. A folder
    /// with a snapshot is never deleted here, even if its note can't be read (a later Henge may read it).
    static func removeUnfinished() {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []
        for id in names.compactMap(UUID.init(uuidString:)) {
            if FileManager.default.fileExists(atPath: stateURL(id).path) {
                if !hasSaved(for: id) { HengeLog.log("suspend", "a suspended Mac's note can't be read: kept (\(id))") }
                continue
            }
            HengeLog.log("suspend", "removing a suspend that didn't finish (\(id))")
            forget(id)
        }
    }

    /// The Macs list reads again which Macs are suspended (SuspendedMacs): after a save and after a forget.
    static func changed() {
        Task { @MainActor in SuspendedMacs.shared.refresh() }
    }

    /// Every Mac with a suspended state to go on from.
    static func allSaved() -> Set<UUID> {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []
        return Set(names.compactMap(UUID.init(uuidString:)).filter(hasSaved(for:)))
    }

    private static func write(_ saved: Saved) -> Bool {
        guard let data = try? JSONEncoder().encode(saved) else { return false }
        return (try? data.write(to: infoURL(saved.macID), options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])) != nil
    }

    /// On the pause queue, the Mac paused. Never worse than the pause: on any failure the Mac stays paused and
    /// nothing is left behind. (true, "") once the snapshot is complete and on storage, else (false, why).
    /// ticket: henge_vm_suspend_ticket() when the save was asked for (a henge_vm_suspend_cancel since calls it off:
    /// cancelled, and nothing is kept).
    static func save(_ info: Saved, tools: URL?, ticket: UInt64) -> (ok: Bool, why: String, cancelled: Bool) {
        let fm = FileManager.default
        let id = info.macID
        forget(id) // whatever an earlier one left
        let dir = folder(id)
        do {
            // Readable while the device is locked (once it has been unlocked since it started): iOS may close
            // Henge in the background, and this is written there.
            try fm.createDirectory(at: dir, withIntermediateDirectories: true,
                                   attributes: [.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication])
            var excluded = dir
            var values = URLResourceValues()
            values.isExcludedFromBackup = true // gigabytes, and of no use on another device
            try excluded.setResourceValues(values)
        } catch {
            return (false, "its folder couldn't be made: \(error.localizedDescription)", false)
        }
        var free = (try? dir.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?.volumeAvailableCapacityForImportantUsage ?? 0
        // The memory: whole, compressed (mostly less, but it can't be counted on); kept in files, only what changed
        // since they were last written. Never fill the device.
        let room = henge_vm_suspend_room_mb()
        let need = Int64((room == UInt64.max ? UInt64(info.ramMB) : room) << 20) + (512 << 20) // max: no Mac
        // Short of room with an auto-save kept: it goes first (this Suspend is newer), and the room is counted again
        // (Not one that holds this paused Mac exactly: this Suspend would only be the same again.)
        // Only if deleting it frees enough: its files are mostly clones sharing storage with the Mac's own, so it may
        // free far less than its size, and an auto-save kept beats a Suspend that fails anyway.
        if free < need, MacAutoSave.exists(id), !SuspendAlert.covered {
            let gain = MacAutoSave.frees(id)
            let gb = { (b: Int64) in String(format: "%.1f GB", Double(b) / 1_073_741_824) }
            if free + gain >= need {
                let counted0 = free, before = MacAutoSave.freeNow()
                MacAutoSave.forgetNow(id, "a Suspend needed its room (it frees \(gb(gain)))") // gone before the room is counted again
                // What the file system freed, added to the count from before (iOS's own figure may not show it yet). It
                // may free a large file's blocks a moment after it is deleted: looked at for a few seconds at most.
                func recount() {
                    let counted = (try? dir.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?.volumeAvailableCapacityForImportantUsage ?? 0
                    let now = MacAutoSave.freeNow()
                    free = max(counted, before >= 0 && now >= 0 ? counted0 + max(now - before, 0) : counted)
                }
                recount()
                var waited = 0
                while free < need && waited < 12 {
                    usleep(250_000)
                    waited += 1
                    recount()
                }
                if waited > 0 { HengeLog.log("suspend", "waited \(waited * 250) ms for the auto-save's room to show") }
            } else {
                HengeLog.log("suspend", "the auto-save is kept: deleting it would free only \(gb(gain)), and \(gb(need - free)) more is needed")
            }
        }
        guard free >= need else {
            forget(id)
            return (false, String(format: "there isn't enough free storage on the iPad (%.1f GB needed, %.1f GB free)",
                                  Double(need) / 1_073_741_824, Double(free) / 1_073_741_824), false)
        }
        if info.tools {
            guard let tools, link(tools.path, toolsURL(id).path) == 0 else {
                forget(id)
                return (false, "the Henge Tools disk couldn't be kept with it", false)
            }
        }
        guard write(info) else {
            forget(id)
            return (false, "its note couldn't be written", false)
        }
        var why = [CChar](repeating: 0, count: 256)
        let t0 = Date()
        let rc = henge_vm_suspend(stateURL(id).path, 2000, ticket, &why, why.count)
        guard rc == 0 else {
            forget(id)
            return (false, String(cString: why), rc == -3)
        }
        let size = ((try? fm.attributesOfItem(atPath: stateURL(id).path))?[.size] as? Int64 ?? 0) >> 20
        HengeLog.log("suspend", "suspended in \(Int(Date().timeIntervalSince(t0) * 1000)) ms, \(size) MB")
        return (true, "", false)
    }

    // MARK: Going on

    enum Boot {
        /// Running. adopt: the drives a resumed Mac had, already in it (MacDrives.start(_:adopting:)).
        case started(adopt: Saved?)
        /// Not started: a sentence for the person.
        case failed(String)
        /// The person kept the suspended Mac for later: nothing runs, the session closes.
        case closed
    }

    /// Starts the Mac: from where it was suspended if it was, else afresh as henge_vm_boot does. ram and cpus:
    /// what this session would give it; a resumed Mac gets what it had. The display settings and shared folders
    /// are set first, as for henge_vm_boot.
    @MainActor
    static func boot(_ mac: VirtualMac, ram: UInt64, cpus: Int32) async -> Boot {
        MacBattery.start() // the iPad's battery is the Mac's, from its first instruction
        MacPause.shared.resumeGivenUp = nil
        MacAutoSave.starting = mac.id
        defer { MacAutoSave.starting = nil }
        func fresh() -> Boot {
            var buffer = [CChar](repeating: 0, count: 256)
            let rc = henge_vm_boot(mac.rom.path, mac.disk.path, mac.aux.path, mac.ecid, mac.seed, ram, cpus, &buffer, buffer.count)
            guard rc == 0 else { return .failed(String(cString: buffer)) }
            forgetAutoSave(mac.id, "the Mac started anew")
            return .started(adopt: nil)
        }
        // An auto-save from before this run, once it runs: older than the Mac from then on, so not offered after a crash
        // (that would offer to undo this run). One whose note can't be read stays (the Mac's next one replaces it).
        func forgetAutoSave(_ id: UUID, _ why: String) {
            if MacAutoSave.saved(for: id) != nil { MacAutoSave.forget(id, why) }
        }
        MacAutoSave.undoUnfinished(mac) // (MacAutoSave.boot does it too: here for a start from a Suspend)
        // The Suspend given up on, before anything is built: an auto-save, if there is one, is offered first (it may
        // be this paused Mac exactly, by another way in), else it starts fresh.
        func freshOrAutoSaved() async -> Boot {
            if let boot = await MacAutoSave.boot(mac, fresh: fresh, alwaysAsk: true) { return boot }
            return fresh()
        }
        dropRunningTools(mac.id) // an earlier run's, if Henge was ended while it ran: no Mac runs from it now
        guard let saved = saved(for: mac.id) else {
            // No Suspend: an auto-save, if Henge ended while the Mac ran (asked; nil when there is none)
            if let boot = await MacAutoSave.boot(mac, fresh: fresh) { return boot }
            return fresh()
        }
        // A Suspend is newer than any auto-save, so it is what goes on; the auto-save is deleted once it does.
        HengeLog.log("suspend", "going on from the Mac suspended \(saved.saved) (\(saved.ramMB) MB, \(saved.cpus) CPUs, \(saved.drives.count) drives, \(saved.failures) failed before)")

        // Tried twice and it didn't work (or Henge was ended partway, twice): the person chooses, rather than a
        // third try that would likely end the same way.
        if saved.failures >= 2 {
            let why = "Henge tried twice to carry on from where it was suspended, and couldn't."
            guard await ask(mac, why) else {
                var again = saved
                again.failures = 1 // kept for later: the next start tries once more
                _ = write(again)
                MacDrives.shared.stopped()
                return .closed
            }
            forget(mac.id)
            return await freshOrAutoSaved()
        }
        // What would refuse it for certain, asked before anything is built, while either answer is free.
        if let why = cantResume(saved, mac) {
            HengeLog.log("suspend", "can't go on: \(why)")
            guard await ask(mac, why) else { MacDrives.shared.stopped(); return .closed }
            forget(mac.id)
            return await freshOrAutoSaved()
        }

        // Counted before it is tried: if iOS ends Henge partway (memory) or it crashes, the next start knows.
        var attempt = saved
        attempt.failures += 1
        if !write(attempt) { HengeLog.log("suspend", "the attempt couldn't be noted") }
        _ = MacDrives.shared.preplug(saved)
        if saved.tools { _ = henge_vm_preplug(toolsURL(mac.id).path, 1, "Henge Tools", MacDrives.toolsSerial) }
        let state = stateURL(mac.id).path
        let (rc, why): (Int32, String) = await Task.detached(priority: .userInitiated) {
            var buffer = [CChar](repeating: 0, count: 256)
            let rc = henge_vm_boot_saved(mac.rom.path, mac.disk.path, mac.aux.path, mac.ecid, mac.seed, saved.ramMB, saved.cpus,
                                         state, &buffer, buffer.count)
            return (rc, String(cString: buffer))
        }.value
        switch rc {
        case 0:
            HengeLog.log("suspend", "the Mac goes on from where it was suspended")
            keepRunningTools(saved)
            forget(mac.id) // the engine deleted the snapshot already; the rest goes with it
            forgetAutoSave(mac.id, "the Mac went on from its Suspend")
            return .started(adopt: saved)
        case -1:
            _ = write(saved) // refused, not failed: the count as it was
            HengeLog.log("suspend", "refused: \(why)")
            if await ask(mac, why, built: true) {
                keepRunningTools(saved) // plugged in already: the drives are adopted as they are
                forget(mac.id)
                guard henge_vm_start_fresh() == 0 else { return .failed("The Mac couldn't be started.") }
                forgetAutoSave(mac.id, "the Mac started anew")
                return .started(adopt: saved)
            }
            henge_vm_drop_built()
            MacDrives.shared.stopped()
            return .closed
        case -3:
            _ = write(saved) // not tried
            MacDrives.shared.stopped()
            HengeLog.log("suspend", "not built: \(why)")
            return .failed(why)
        default:
            MacDrives.shared.stopped()
            HengeLog.log("suspend", "failed (\(attempt.failures)): \(why)")
            if attempt.failures >= 2 {
                return .failed("\(why) Henge couldn't carry on from where this Mac was suspended, twice. Close Henge and open it "
                               + "again to choose whether to start it afresh.")
            }
            return .failed("\(why) The suspended Mac is kept: close Henge and open it again to try once more.")
        }
    }

    /// The Henge Tools file plugged in for a resume, kept by name before its folder goes: a later Suspend keeps
    /// that same file (MacDrives.start(_:adopting:) takes it as the one in the Mac).
    private static func keepRunningTools(_ saved: Saved) {
        guard saved.tools, link(toolsURL(saved.macID).path, runningToolsURL(saved.macID).path) != 0 else { return }
        HengeLog.log("suspend", "its Henge Tools file couldn't be kept: a later Suspend won't have that disk")
    }

    /// Why this Mac surely can't go on from where it was, before anything is built; nil when it may.
    @MainActor
    private static func cantResume(_ saved: Saved, _ mac: VirtualMac) -> String? {
        if let missing = MacDrives.shared.openForResume(saved) {
            return "The USB drive \"\(missing)\" it had in can't be found."
        }
        if saved.tools && !FileManager.default.fileExists(atPath: toolsURL(mac.id).path) {
            return "The Henge Tools disk it had in is gone."
        }
        // Its memory as it was: on storage (the RAM file) and in one mapping, as GuestMemory.prepare gives.
        let free = (try? mac.directory.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?
            .volumeAvailableCapacityForImportantUsage ?? 0
        let room = free > 0 ? UInt64(max(free - (2 << 30), 0)) >> 20 : UInt64.max
        let reach = henge_largest_mapping_mb()
        if saved.ramMB > min(room, reach > 512 ? reach - 512 : 0) {
            return "There isn't room for its memory as it was (\(saved.ramMB / 1024) GB): free some storage on this \(Device.name), or close other apps."
        }
        return nil
    }

    /// The person chooses: true to start the Mac afresh, false to keep the suspended Mac for later.
    @MainActor
    /// built: the Mac is built already (refused as it was put back), so an auto-save can't be offered in this run.
    private static func ask(_ mac: VirtualMac, _ why: String, built: Bool = false) async -> Bool {
        let auto = MacAutoSave.saved(for: mac.id)?.saved.formatted(date: .abbreviated, time: .shortened)
        let then = auto.map { built ? " Its auto-save from \($0) is deleted too." : " If it has an auto-save (from \($0)), Henge offers that next." } ?? ""
        let question = ResumeQuestion(
            title: "\(mac.name) can't carry on where it was",
            message: why + " Start Fresh turns the Mac on anew: its disk is as it was when suspended, but apps that were open, "
                + "and anything not saved in them, are gone.\(then) Keep for Later leaves it suspended, to try again once that's sorted.")
        return await withTaskCancellationHandler {
            await withCheckedContinuation { (done: CheckedContinuation<Bool, Never>) in
                question.answered = { fresh in
                    if MacPause.shared.resumeQuestion?.id == question.id { MacPause.shared.resumeQuestion = nil }
                    if fresh { MacPause.shared.resumeGivenUp = mac.id }
                    done.resume(returning: fresh)
                }
                MacPause.shared.resumeQuestion = question
            }
        } onCancel: {
            Task { @MainActor in question.answer(false) } // the session closed: keep it
        }
    }
}

/// Which Macs are suspended, for the Macs list to show and to say Resume rather than Start. Read from storage at
/// first, and again whenever a Mac is suspended or its saved state is deleted (MacSuspend.changed).
@MainActor
final class SuspendedMacs: ObservableObject {
    static let shared = SuspendedMacs()
    @Published private(set) var ids: Set<UUID> = []

    private init() { refresh() }

    func refresh() {
        let now = MacSuspend.allSaved()
        if now != ids { ids = now }
    }
}

/// A suspended Mac that can't go on from where it was, put to the person (ResumeQuestionCard).
final class ResumeQuestion: Identifiable {
    let id = UUID()
    let title: String
    let message: String
    /// The two buttons: true is the first. A Suspend's: Start Fresh, Keep for Later.
    let first: String
    let second: String
    var answered: ((Bool) -> Void)?

    init(title: String, message: String, first: String = "Start Fresh", second: String = "Keep for Later") {
        self.title = title
        self.message = message
        self.first = first
        self.second = second
    }

    /// Once: true for the first button (a Suspend's: start the Mac afresh), false for the second (keep it).
    @MainActor func answer(_ fresh: Bool) {
        guard let answered else { return }
        self.answered = nil
        answered(fresh)
    }
}

struct ResumeQuestionCard: View {
    let question: ResumeQuestion

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(question.title).font(.system(size: 17, weight: .semibold))
            Text(question.message).font(.system(size: 14)).foregroundStyle(Desk.dim)
            HStack(spacing: 22) {
                Button(question.first) { question.answer(true) }.foregroundStyle(Desk.accentLight)
                Button(question.second) { question.answer(false) }.foregroundStyle(Desk.dim)
            }
            .font(.system(size: 15, weight: .semibold))
        }
        .padding(18)
        .background(Desk.raised, in: RoundedRectangle(cornerRadius: 22, style: .continuous))
    }
}
