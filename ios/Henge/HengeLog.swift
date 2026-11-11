// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Darwin
import UIKit
import os

/// Everything Henge knows, in one chronological file.
///
/// The problem this solves: QEMU and Henge's own C code write diagnostics with
/// `fprintf(stderr, ...)`, and on iOS stderr goes nowhere a user can reach. So
/// before anything else runs, stdout and stderr are redirected into a pipe that a
/// reader thread drains, writing every line to three places at once:
///
///   * `Documents/henge.log`, retrievable over the Files app or the share sheet
///   * `os_log`, so Console.app shows it live with the device attached
///   * an in-memory ring buffer the app can display without leaving the device
///
/// Because QEMU's output and Henge's Swift logging both land in the same stream,
/// the ordering between "the JIT region was allocated" and "QEMU started
/// translating" is preserved, which is exactly the ordering that matters when
/// something goes wrong.
enum HengeLog {
    private static let osLog = Logger(subsystem: "com.henge.app", category: "henge")

    private static var logFD: Int32 = -1
    private static var pipeReadFD: Int32 = -1
    private static var pipeWriteFD: Int32 = -1
    /// HV_LOG_CONSOLE: a copy of the original stderr, so `devicectl device process launch --console` (and Xcode's
    /// console) can follow the log live. `colorprobe` (any text) mirrors only lines containing it; `all` mirrors everything.
    private static var consoleFD: Int32 = -1
    private static var consoleFilter: String?
    private static var started = false
    private static let startTime = Date()

    /// Recent lines, for the in-app viewer. Bounded so a long session cannot grow
    /// without limit.
    private static let ringLimit = 4000
    private static var ring: [String] = []
    private static let ringLock = NSLock()

    static var logFileURL: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("henge.log")
    }

    static func recentLines(_ n: Int = 400) -> [String] {
        ringLock.lock(); defer { ringLock.unlock() }
        return Array(ring.suffix(n))
    }

    // MARK: - Startup

    static func start() {
        guard !started else { return }
        started = true

        // A write to a pipe whose read end has closed raises SIGPIPE, and the
        // default action for SIGPIPE is to terminate the process. Since stdout and
        // stderr are about to become that pipe, ignore it outright -- a logging
        // failure must never be able to kill the app it is trying to diagnose.
        signal(SIGPIPE, SIG_IGN)

        // Fresh file each launch. StikDebug relaunches us after attaching, so the
        // run that matters is always the most recent one; keeping the previous
        // run's noise would make the log harder to read, not easier.
        // ...but the previous run is kept as henge-prev.log, because when a game took Henge down with it, that run's log is
        // the crash report (CrashReport.swift).
        let url = logFileURL
        let prev = url.deletingLastPathComponent().appendingPathComponent("henge-prev.log")
        try? FileManager.default.removeItem(at: prev)
        try? FileManager.default.moveItem(at: url, to: prev)
        logFD = open(url.path, O_CREAT | O_WRONLY | O_TRUNC, 0o644)
        // The file is written by a thread of its own (henge-logq.c): a write(2) to it took up to 0.65 s on the main
        // thread while the iPad's storage was busy (freeze recorder, 8 Nov 2026). Logging now only copies the line
        // into memory; the crash handler and exit() still get what is queued out.
        if logFD >= 0, henge_logq_start(logFD) != 0 {
            log("boot", "WARNING: no log writer thread; henge.log is written by whoever logs")
        }

        redirectStdio()
        installCrashHandlers()
        flushWithTheApp()
        logBanner()
        replayPreviousNativeLog()
    }

    /// What is queued for the file is written before iOS can stop the app: on going to the background (it may be
    /// suspended and then killed without a word, which loses what is still in memory), on a memory warning (a
    /// memory kill gives no warning of its own), and as it is closed.
    private static func flushWithTheApp() {
        let centre = NotificationCenter.default
        centre.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main) { _ in
            // Not on the main thread: a background task keeps the app running until the writer has caught up.
            var task = UIBackgroundTaskIdentifier.invalid
            task = UIApplication.shared.beginBackgroundTask(withName: "henge.log") {
                UIApplication.shared.endBackgroundTask(task)
                task = .invalid
            }
            DispatchQueue.global(qos: .userInitiated).async {
                henge_logq_sync(5)
                DispatchQueue.main.async {
                    if task != .invalid { UIApplication.shared.endBackgroundTask(task); task = .invalid }
                }
            }
        }
        centre.addObserver(forName: UIApplication.didReceiveMemoryWarningNotification, object: nil, queue: nil) { _ in
            // The writer is already on it; this only says how far behind it was, and nudges it.
            let behind = henge_logq_pending(), dropped = henge_logq_dropped()
            if behind > 64 * 1024 || dropped > 0 {
                log("log", "memory warning: \(behind / 1024) KB of log not yet written, \(dropped) lines dropped so far")
            }
            DispatchQueue.global(qos: .userInitiated).async { henge_logq_flush(2) }
        }
        centre.addObserver(forName: UIApplication.willTerminateNotification, object: nil, queue: .main) { _ in
            flushNow()
        }
    }

    /// Put the last run's native output into this run's log.
    ///
    /// When QEMU aborts, its reason lands in henge-native.log and the process is
    /// gone before anything mirrors it -- and that file was not in the share
    /// sheet either, so the one artefact explaining a crash was the one nobody
    /// could retrieve. Replaying it here puts it in henge.log, which is the file
    /// people already know how to send.
    private static func replayPreviousNativeLog() {
        guard let data = try? Data(contentsOf: previousNativeLogURL),
              !data.isEmpty,
              let text = String(data: data.suffix(8 * 1024), encoding: .utf8)
        else { return }

        let lines = text.split(separator: "\n", omittingEmptySubsequences: true).suffix(40)
        guard !lines.isEmpty else { return }
        log("prev", "---- last \(lines.count) lines from the previous run "
                  + "(the one that may have crashed) ----")
        for line in lines { log("prev", String(line)) }
        log("prev", "---- end of the previous run ----")
    }

    /// QEMU's own stderr, this run and the one before it.
    static var nativeLogURL: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("henge-native.log")
    }
    static var previousNativeLogURL: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("henge-native-prev.log")
    }

    /// Everything logged so far to the file, and the file to storage: before Henge closes itself. Waits at most
    /// half a second for the writer thread: Henge's self-restart (Relaunch.swift) must be gone before the app it
    /// opened calls it back, about 1 s later. exit() waits for the queue briefly too (henge-logq.c).
    static func flushNow() {
        henge_logq_sync(0.5)
    }

    /// Point stdout and stderr at a pipe we drain ourselves.
    ///
    /// Uses raw pipe(2) descriptors held in statics rather than Foundation's Pipe.
    /// Pipe owns its FileHandles and closes their descriptors when it deallocates,
    /// so a locally-scoped Pipe tears down the read end the moment this function
    /// returns -- after which every write to stderr hits a reader-less pipe. These
    /// descriptors belong to no object and are never closed.
    /// A file, not a pipe.
    ///
    /// A pipe loses everything in it when the process dies: QEMU writes an
    /// assertion message, aborts before the reader thread is scheduled, and the
    /// log shows a fatal signal with nothing before it. A file cannot lose
    /// anything -- stderr is unbuffered, so every byte is on disk as it is
    /// written, whatever happens next.
    ///
    /// The previous run's copy is kept rather than overwritten, because the run
    /// that crashed is exactly the one worth reading, and it is replayed into
    /// the next run's henge.log by replayPreviousNativeLog().
    private static func redirectStdio() {
        let dir = FileManager.default.urls(for: .documentDirectory,
                                           in: .userDomainMask)[0]
        let cURL = dir.appendingPathComponent("henge-native.log")
        let prevURL = dir.appendingPathComponent("henge-native-prev.log")
        try? FileManager.default.removeItem(at: prevURL)
        try? FileManager.default.moveItem(at: cURL, to: prevURL)

        pipeWriteFD = open(cURL.path, O_CREAT | O_WRONLY | O_TRUNC, 0o644)
        pipeReadFD = open(cURL.path, O_RDONLY)
        guard pipeWriteFD >= 0, pipeReadFD >= 0 else {
            log("boot", "WARNING: could not open henge-native.log (errno \(errno)); "
                      + "C-side stderr will not be captured")
            return
        }
        log("boot", "native stderr -> henge-native.log (mirrored here)")

        if let mode = ProcessInfo.processInfo.environment["HV_LOG_CONSOLE"], !mode.isEmpty {
            consoleFD = dup(STDERR_FILENO)
            consoleFilter = (mode == "all" || mode == "1") ? nil : mode
        }
        setvbuf(stdout, nil, _IOLBF, 0)
        setvbuf(stderr, nil, _IONBF, 0)
        dup2(pipeWriteFD, STDOUT_FILENO)
        dup2(pipeWriteFD, STDERR_FILENO)

        let t = Thread {
            var pending = Data()
            var buf = [UInt8](repeating: 0, count: 16 * 1024)
            while true {
                let n = read(pipeReadFD, &buf, buf.count)
                if n < 0 {
                    if errno == EINTR { continue }
                    break
                }
                // End of file, not end of stream: the writer is still open and
                // more will arrive. A pipe blocked here; a file returns 0, so
                // this is where the tail waits.
                if n == 0 {
                    usleep(40_000)
                    continue
                }
                pending.append(contentsOf: buf[0..<n])

                // Emit complete lines only, so a partial write never splits a
                // message across two log entries.
                while let nl = pending.firstIndex(of: 0x0A) {
                    let lineData = pending.subdata(in: pending.startIndex..<nl)
                    pending.removeSubrange(pending.startIndex...nl)
                    if let line = String(data: lineData, encoding: .utf8) {
                        emit(line, fromStream: true)
                    }
                }
            }
        }
        t.name = "com.henge.log.reader"
        t.qualityOfService = .userInitiated
        t.start()
    }

    /// Turn "the app vanished with no crash log" into a labelled last line.
    ///
    /// The handler is C (henge-logq.c), so that it allocates nothing and takes no lock: what is still queued for
    /// henge.log, a FATAL SIGNAL marker, whatever native code wrote to henge-native.log that the reader thread had not
    /// reached (QEMU's assertion text, which once cost a whole diagnosis), a backtrace, fsync, then the signal's
    /// default action.
    ///
    /// Deliberately does NOT touch SIGTRAP or SIGBUS: those belong to the JIT trap
    /// guard in henge-ios-jit.c, which needs to step over an unserviced brk rather
    /// than treat it as fatal. Stealing them here would break JIT detection.
    /// SIGPIPE is already set to SIG_IGN in start() and must stay that way.
    private static func installCrashHandlers() {
        henge_logq_install_crash_handlers(pipeReadFD)
        NSSetUncaughtExceptionHandler { ex in
            HengeLog.log("crash", "uncaught exception: \(ex.name.rawValue) -- \(ex.reason ?? "")")
            HengeLog.log("crash", ex.callStackSymbols.prefix(20).joined(separator: " | "))
            // No flush: abort() follows, and the SIGABRT handler writes the queue out without waiting on a thread.
        }
        log("boot", "crash handlers installed (SEGV/ABRT/ILL/FPE/SYS; "
                  + "TRAP+BUS left to the JIT guard; SIGPIPE ignored)")
    }

    // MARK: - Emitting

    /// - Parameter fromStream: true when the line came out of the captured pipe,
    ///   meaning the C side already sent it to os_log itself.
    private static func emit(_ line: String, fromStream: Bool) {
        ringLock.lock()
        ring.append(line)
        // Trimmed 500 at a time: removeFirst shifts every line left, and once per line was 4,000 moves a line.
        if ring.count > ringLimit + 500 { ring.removeFirst(ring.count - ringLimit) }
        ringLock.unlock()

        // The C side (henge-ios-jit.c, henge-display.c) writes to os_log itself as
        // well as stderr, because those are the lines most worth surviving a hard
        // crash. Mirroring them again here would double every one of them in
        // Console.app.
        if !fromStream || (!line.contains("[henge-jit]") && !line.contains("[henge-dpy]")) {
            osLog.log("\(line, privacy: .public)")
        }

        if consoleFD >= 0, consoleFilter == nil || line.contains(consoleFilter!),
           let console = (line + "\n").data(using: .utf8) {
            console.withUnsafeBytes { raw in _ = write(consoleFD, raw.baseAddress, raw.count) }
        }

        // Queued, not written here: the writer thread (henge-logq.c) writes it, in order. A write(2) here once took
        // 0.65 s on the main thread. What a crash would otherwise lose, the fatal-signal handler writes from the
        // queue itself, and exit() waits for the queue; a full queue drops the line (counted, and marked in the file)
        // rather than wait.
        guard logFD >= 0 else { return }
        var bytes = line + "\n"
        bytes.withUTF8 { raw in _ = henge_logq_push(raw.baseAddress, raw.count) }
    }

    /// Log from Swift. Lands in the same stream as QEMU's own output.
    static func log(_ category: String, _ message: String) {
        let t = Date().timeIntervalSince(startTime) * 1000
        emit(String(format: "[%9.2fms][%@] %@", t, category, message), fromStream: false)
    }

    // MARK: - Context

    /// The hardware identifier, e.g. "iPhone18,1". Which JIT routes exist on
    /// this device is decided from it and the iOS version.
    static var deviceModel: String {
        var sysinfo = utsname(); uname(&sysinfo)
        return withUnsafePointer(to: &sysinfo.machine) {
            $0.withMemoryRebound(to: CChar.self, capacity: 1) { String(cString: $0) }
        }
    }

    private static func logBanner() {
        let d = UIDevice.current
        let model = deviceModel
        let mem = ProcessInfo.processInfo.physicalMemory / (1024 * 1024)

        log("boot", "================ Henge starting ================")
        log("boot", "device      : \(model)")
        log("boot", "system      : \(d.systemName) \(d.systemVersion)")
        log("boot", "physical RAM: \(mem) MiB")
        log("boot", "processors  : \(ProcessInfo.processInfo.processorCount) "
                  + "(active \(ProcessInfo.processInfo.activeProcessorCount))")
        log("boot", "bundle      : \(Bundle.main.bundleIdentifier ?? "?")")
        // Which build this is, in the log itself.
        //
        // Without this a log cannot be told apart from one produced by an older
        // install, and a stale app on the phone reads exactly like a fix that
        // did not work -- which has now cost a debugging round trip. The commit
        // and date are stamped into Info.plist by package_ipa.sh.
        let info = Bundle.main.infoDictionary ?? [:]
        let version = info["CFBundleShortVersionString"] as? String ?? "?"
        let build   = info["CFBundleVersion"] as? String ?? "?"
        let commit  = info["HengeBuildCommit"] as? String ?? "unstamped"
        let built   = info["HengeBuildDate"] as? String ?? "?"
        log("boot", "build       : \(version) (\(build)) \(commit) built \(built)")
        log("boot", "pid         : \(getpid())")
        log("boot", "log file    : \(logFileURL.path)")
        // Every iOS 27 device except iPad8,11/8,12 enforces TXM, which is what makes
        // the debugger-assisted JIT path mandatory rather than optional.
        log("boot", "TXM expected: \(Self.expectsTXM(model: model) ? "YES" : "no")")
        log("boot", "===============================================")
    }

    static func expectsTXM(model: String) -> Bool {
        if #available(iOS 27.0, *) {
            return model != "iPad8,11" && model != "iPad8,12"
        }
        if #available(iOS 26.0, *) {
            // A14-era and newer: iPhone13,x is A14, so HW major >= 13 for phones.
            let digits = model.drop { !$0.isNumber }.prefix { $0.isNumber }
            if let major = Int(digits) {
                return model.hasPrefix("iPhone") ? major >= 13 : major >= 14
            }
        }
        return false
    }

    /// Snapshot of process memory, the figure jetsam kills on.
    static func logFootprint(_ tag: String) {
        henge_ios_jit_log_footprint(tag)
    }
}
