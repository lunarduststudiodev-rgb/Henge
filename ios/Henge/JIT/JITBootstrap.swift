// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import UIKit
import os

/// `csops(2)`. Reading CS_DEBUGGED is how we ask "is a debugger attached right
/// now" without executing anything — unlike the brk probe, which costs a trap.
@_silgen_name("csops")
private func csops(_ pid: Int32, _ ops: Int32,
                   _ useraddr: UnsafeMutableRawPointer?, _ usersize: Int) -> Int32

/// Husk reserves its Android heap around the claim; a Mac guest has nothing to do there.
private func prepareCompressedHeap() {}
private func releaseUnusedCompressedHeap() {}

private let CS_OPS_STATUS = Int32(0)
private let CS_DEBUGGED = UInt32(0x10000000)

/// Gets Henge from "launched normally, no executable memory" to "JIT is live".
///
/// On iOS 27 every supported device enforces TXM, so the app cannot grant itself
/// executable memory — only an attached debugger can. That debugger is either
/// StikDebug, which Henge hands the JIT script inline over its URL scheme, or
/// Henge's own built-in StikJIT helper (JITSetup.swift), which runs the same
/// script from an app extension.
enum JITBootstrap {
    private static let log = Logger(subsystem: "com.henge.app", category: "jit")

    enum State: Equatable {
        case unknown
        case waitingForDebugger
        case live
        case failed(String)
    }

    /// Install the trap guard. Must run before anything can execute a `brk`.
    ///
    /// Without this, a `brk` that StikDebug is not there to service is a fatal
    /// SIGTRAP rather than a failed call — the process simply dies.
    static func installTrapGuard() {
        HengeLog.log("jit", "installing brk trap guard")
        henge_ios_jit_install_trap_handler()
        HengeLog.log("jit", "trap guard installed -- an unserviced brk will now "
                         + "return 0 instead of killing the process")
    }

    /// Size QEMU will ask for. Must match tb-size in the phase 1 command line:
    /// a smaller region here means QEMU allocates a second one, at a point where
    /// StikDebug may be long gone.
    // Back to 256 MiB. Raising this to 512 was one of three changes made at
    // once in v15, and v15 was the first build to die inside qemu_init(). The
    // guest RAM -- the other suspect -- has since been shown to map and write
    // cleanly at 6144 MiB, which leaves this. The region itself allocates and
    // passes its selftest at 512; whatever objects is further in, where TCG
    // carves the buffer into per-vCPU regions.
    //
    // 512 MiB since the native runtime runs Minecraft: its main library alone is a 354 MiB image that has to sit in this
    // region, with the stubs the loader places beside it. A larger prewarm is safe for QEMU, which is handed the
    // prewarmed region whenever it is at least what tb-size asks for.
    /// The one region HengeVM's translator gets for the whole session (it flushes and starts over when full,
    /// never asks for more). 1 GB takes about 1.5 s to prepare, and a first boot through Setup makes that much code; a full flush drops all of it.
    static let codeCacheKey = "henge.perf.codeCacheMB"
    /// 1 GB, or 1.5 GB (Settings > Performance > Code cache; without a choice there, 1.5 GB where the memory limit
    /// has room for it: about 6 GB on 12 GB iPhones). A bigger cache fills and empties less often (macOS 27 filled
    /// 1 GB about once a minute) and costs more memory (its blocks' records come to about 0.75 of its size). Fixed for
    /// the launch: the region is claimed once.
    static let defaultCodeCacheMB: Int = Entitlements.memoryLimitAtLaunch >= 5_500 << 20 ? 1536 : 1024
    static let jitBytes: Int = ((UserDefaults.standard.object(forKey: codeCacheKey) as? Int) ?? defaultCodeCacheMB) == 1536 ? 1536 << 20 : 1024 << 20

    /// True once the region is held. The memory budget needs this: after a
    /// prewarm the JIT is already counted in the footprint, so subtracting it
    /// again charges for it twice and cost the guest 256 MiB.
    nonisolated(unsafe) static var prewarmed = false

    /// Take the JIT region now, while StikDebug is definitely still attached.
    ///
    /// StikDebug lets go after a while, and a first run spends a minute
    /// downloading 1.1 GB of guest image before QEMU starts. By the time
    /// qemu_init() asked for memory the debugger had detached, and there is no
    /// recovering from that in-process -- without a debugger there is no
    /// executable memory at all, and asking again later is precisely what does
    /// not work. So claim it first and hold it.
    ///
    /// `retry`: a debugger may have attached since the last try failed for want of one (Henge asked for it, or came back to
    /// the foreground with one). The native side then asks again, but only when the last probe came back to Henge's own trap
    /// guard: a debugger that is attached and not answering is never trapped into twice.
    @discardableResult
    static func prewarm(retry: Bool = false) -> Bool {
        if holdsRegion { prewarmed = true; return true }
        if retry { henge_ios_jit_prewarm_rearm() }
        // Before iOS 26 Henge can make the region itself where the device allows it (a jailbreak,
        // a debugger that attached and let go such as TrollStore's enable-jit, or dynamic-codesigning),
        // so it does not wait for CS_DEBUGGED there if jailbroken: Dopamine hides that flag from apps unless
        // "Allow JIT in Apps" is on, while still letting them run code they wrote.
        let attached = isDebuggerAttached
        guard attached || (canGrantOwnJIT && !selfGrantTried) else {
            HengeLog.log("jit", "no debugger attached yet; not prewarming")
            return false
        }
        if !attached { selfGrantTried = true }
        // Before iOS 26, unless StikDebug's script is what attached, no brk: TrollStore's enable-jit (and its Open with JIT)
        // attaches for 100 ms and services nothing, and a brk in that window stops Henge for good (TrollStore then says
        // "trollstorehelper returned 3"). A debugged process there makes its own executable memory: MAP_JIT, or plain memory
        // made executable, either way as the two views of one region the translator needs.
        if canGrantOwnJIT && !stikDebugMayServiceTraps {
            selfGrantTried = true
            HengeLog.log("jit", "not trapping for a JIT region (no StikDebug); making it here")
            if claimSelf() { return true }
            lastFailure = selfGrantFailure
            return false
        }
        HengeLog.log("jit", "claiming \(jitBytes / (1024 * 1024)) MiB of JIT memory now, "
                         + "before the guest download -- StikDebug does not stay attached")
        prepareCompressedHeap()
        var ok = henge_ios_jit_prewarm(jitBytes)
        if !ok { releaseUnusedCompressedHeap() }
        if ok {
            prewarmed = true
            lastFailure = nil
            detachIfDone()
        } else if canGrantOwnJIT, claimSelf() {
            // Before iOS 26 a debugged process can make the region itself when the debugger did not hand one out.
            ok = true
        } else {
            lastFailure = unansweredFailure
        }
        HengeLog.log("jit", ok ? "JIT region secured; it will be handed to the Mac later"
                              : "JIT prewarm FAILED -- no debugger answered Henge's JIT request")
        return ok
    }

    /// The region made by Henge itself, before iOS 26 (the native side refuses on 26 and later).
    private static func claimSelf() -> Bool {
        prepareCompressedHeap()
        guard henge_ios_jit_prewarm_self(jitBytes) else {
            releaseUnusedCompressedHeap()
            return false
        }
        prewarmed = true
        lastFailure = nil
        HengeLog.log("jit", "JIT region made by Henge itself (\(henge_ios_jit_self_route().map { String(cString: $0) } ?? "self"))")
        return true
    }

    /// Whether the translator's region is held. The Mac cannot start without it ("Henge has no JIT region"), whatever else
    /// says JIT is on.
    static var holdsRegion: Bool { henge_ios_jit_get_mapping() != nil }

    private static var selfGrantFailure: String {
        "Henge is marked as debugged, but this \(UIDevice.current.model) would not run code Henge wrote. "
            + "Open Henge again with JIT from TrollStore or your jailbreak, or choose StikDebug in Set Up JIT."
    }

    /// Why a debugger was attached and still no region came: what answered (or did not) depends on how Henge was opened.
    private static var unansweredFailure: String {
        if isInLiveContainer {
            return "LiveContainer marked Henge as debugged, but no debugger answered Henge's JIT request. In LiveContainer's "
                + "settings choose StikDebug as the JIT enabler, then open Henge with Launch with JIT. Or tap Turn On JIT: "
                + "Henge asks StikDebug to attach with Henge's own script."
        }
        return "A debugger attached, but it did not answer Henge's JIT request, so no code region could be claimed. This "
            + "happens when Henge is opened from StikDebug's app list without a script. Tap Turn On JIT, or in StikDebug "
            + "long-press Henge, choose Assign Script and pick universal.js once: then opening Henge from StikDebug's list "
            + "turns JIT on by itself."
    }

    /// Whether to leave StikDebug attached after the region is held.
    ///
    /// While a debugger is attached, every stop event (a `brk`, a signal, a
    /// Mach exception) halts the whole process until StikDebug answers. iOS
    /// suspends StikDebug soon after it hands the foreground back, so the next
    /// stop freezes Henge with nothing in the log. On iOS 26 that looks like
    /// Android hanging on its boot screen. The RX pages stay valid after detach
    /// (AetherPS4 runs this way on TXM devices), so detaching once the region is
    /// held removes the debugger from the picture.
    static var keepDebuggerAttached: Bool {
        get { UserDefaults.standard.bool(forKey: "henge.keepDebuggerAttached") }
        set { UserDefaults.standard.set(newValue, forKey: "henge.keepDebuggerAttached") }
    }

    /// Whether StikDebug's script may be the debugger: the only one that answers a brk.
    private static var stikDebugMayServiceTraps: Bool {
        let method = UserDefaults.standard.string(forKey: "henge.jitMethod") ?? JITMethod.automatic.rawValue
        return method == JITMethod.stikDebug.rawValue || (method == JITMethod.automatic.rawValue && isStikDebugInstalled)
    }

    /// The one try at making the region without a debugger has been made (it is not repeated: the answer will not change).
    nonisolated(unsafe) static private var selfGrantTried = false

    /// Whether Henge has JIT now: the region the Mac runs in is held. Before iOS 26 this tries, once, to make the region
    /// itself, which is all TrollStore and jailbroken devices need. Cheap after the first call.
    static var ready: Bool {
        if prewarmed || holdsRegion { return true }
        // CS_DEBUGGED alone is not JIT: it is also what opening Henge from StikDebug's app list without a script gives, or
        // LiveContainer, and a Mac then stopped at "Henge has no JIT region" (#23, #49). On a TXM device only a debugger that
        // answers the brk hands out the region, and the claim happens at launch or after an attach Henge asked for, never
        // here: views ask this every second, and a brk into a debugger that does not answer stops the app. Without TXM a
        // debugged process can be given the region in one try, made here once.
        if debuggedFlag {
            guard !deviceEnforcesTXM, !debuggedClaimTried else { return false }
            debuggedClaimTried = true
            return prewarm()
        }
        guard canGrantOwnJIT, !selfGrantTried else { return false }
        return prewarm()
    }

    /// `ready`'s one claim on a device without TXM has been made.
    nonisolated(unsafe) static private var debuggedClaimTried = false

    /// True once this process has told StikDebug to let go.
    nonisolated(unsafe) static private(set) var detached = false

    /// Henge's built-in helper (StikJIT's script, which answers the detach brk) was started this launch.
    nonisolated(unsafe) static var helperMayBeAttached = false
    nonisolated(unsafe) static private var lingeringTries = 0

    /// A debugger still on after the one that gave the region was let go: another helper that attached once the first
    /// detached. With one attached every trap stops Henge until it answers (a phone run went 1.5-2x slower). Told to let
    /// go too, if it is the built-in helper's (StikJIT's script answers the detach brk; a debugger that does not, Xcode's
    /// say, would stop at it). Cheap: one sysctl. Called before a Mac starts and every 10 s while one runs; three tries.
    static func releaseLingeringDebugger() {
        guard detached, !keepDebuggerAttached, helperMayBeAttached, lingeringTries < 3, isTraced else { return }
        lingeringTries += 1
        HengeLog.log("jit", "a debugger is still attached after the JIT region was held (a second helper?); detaching it")
        henge_ios_jit_detach()
        HengeLog.log("jit", isTraced ? "still traced after the detach request" : "debugger detached")
    }

    /// Whether a debugger traces this process right now (P_TRACED; CS_DEBUGGED stays set after a detach).
    static var isTraced: Bool {
        var info = kinfo_proc()
        var size = MemoryLayout<kinfo_proc>.stride
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
        guard sysctl(&mib, 4, &info, &size, nil, 0) == 0 else { return false }
        return (info.kp_proc.p_flag & P_TRACED) != 0
    }

    /// Release the debugger once the JIT region is held. It is no longer
    /// needed for anything, and leaving it attached is what froze the app.
    private static func detachIfDone() {
        guard prewarmed, !detached, !keepDebuggerAttached else { return }
        HengeLog.log("jit", "JIT region held; detaching StikDebug so a suspended "
                         + "debugger cannot stall the process later (traced: \(isTraced ? "yes" : "no"))")
        henge_ios_jit_detach()
        detached = true
        HengeLog.log("jit", "after the detach request, traced: \(isTraced ? "yes" : "no")")
    }

    /// Whether this device enforces TXM, so that only a trap servicer can grant
    /// executable memory. Uses the same rule as the "TXM expected" line in the
    /// log banner, so the two can never disagree.
    static let deviceEnforcesTXM: Bool = HengeLog.expectsTXM(model: HengeLog.deviceModel)

    /// Why the last prewarm failed, for the UI to show.
    ///
    /// CS_DEBUGGED being set is not the same as the debugger servicing traps.
    /// A build running inside LiveContainer reports itself debugged, answers no
    /// brk, and gets no executable memory -- and Henge started QEMU anyway,
    /// which segfaulted inside qemu_init() with a perfectly healthy 4 GB of
    /// headroom. The crash looked like memory pressure and was nothing of the
    /// kind.
    nonisolated(unsafe) static var lastFailure: String?

    /// The actual precondition for starting the guest: StikDebug has attached.
    ///
    /// Checked with csops/CS_DEBUGGED rather than the `brk #0x69` probe, because
    /// this costs nothing and is safe to call repeatedly — the brk probe traps
    /// every time, and every trap is a chance to hit a moment when StikDebug is
    /// not listening. The brk probe still runs once, inside the allocator, where
    /// its answer is immediately acted on.
    static var isDebuggerAttached: Bool {
        guard let flags = csStatus() else {
            HengeLog.log("jit", "csops failed (errno \(errno)); assuming no debugger")
            return false
        }
        let attached = (flags & CS_DEBUGGED) != 0
        HengeLog.log("jit", String(format: "csops status = 0x%08x, CS_DEBUGGED = %@",
                                  flags, attached ? "set" : "clear"))
        return attached
    }

    /// `isDebuggerAttached` without the log line, for polling while the
    /// built-in helper attaches.
    static var debuggedFlag: Bool {
        (csStatus() ?? 0) & CS_DEBUGGED != 0
    }

    private static func csStatus() -> UInt32? {
        var flags: UInt32 = 0
        let rc = withUnsafeMutableBytes(of: &flags) { buf in
            csops(getpid(), CS_OPS_STATUS, buf.baseAddress, buf.count)
        }
        return rc == 0 ? flags : nil
    }

    static var isStikDebugInstalled: Bool {
        URL(string: "stikjit://").map(UIApplication.shared.canOpenURL) ?? false
    }

    /// Whether TrollStore is on this device. It answers enable-jit on the `apple-magnifier` scheme, but that scheme is also the
    /// Magnifier app's, on every iPhone, so whether it opens says nothing. A Henge TrollStore installed knows from its marker file;
    /// otherwise SpringBoard is asked to open TrollStore, suspended, which an ordinary app is refused (9) when it exists and told
    /// it does not (7) when it does not. TrollStore installs on iOS 14.0 to 17.0 only.
    static let isTrollStoreInstalled: Bool = {
        if isInstalledWithTrollStore { return true }
        if #available(iOS 17.0.1, *) { return false }
        typealias Launch = @convention(c) (NSString, NSURL?, NSDictionary?, NSDictionary?, Bool) -> Int32
        guard let lib = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_NOW),
              let sym = dlsym(lib, "SBSLaunchApplicationWithIdentifierAndURLAndLaunchOptions") else { return false }
        let launch = unsafeBitCast(sym, to: Launch.self)
        for id in ["com.opa334.TrollStore", "com.opa334.TrollStoreLite"] {
            let r = launch(id as NSString, nil, nil, nil, true)
            HengeLog.log("jit", "SpringBoard on opening \(id): \(r)")
            if r == 9 { return true }
        }
        return false
    }()

    /// Whether this copy of Henge was installed by TrollStore (or TrollStore Lite): it leaves a marker file next to the app in
    /// its bundle container (TrollStore's `TS_MARKER`).
    static var isInstalledWithTrollStore: Bool {
        let container = Bundle.main.bundleURL.deletingLastPathComponent()
        return ["_TrollStore", "_TrollStoreLite"].contains {
            FileManager.default.fileExists(atPath: container.appendingPathComponent($0).path)
        }
    }

    /// Whether Henge runs as a guest inside LiveContainer, which sets this for its guests (LCBootstrap.m). LiveContainer asks
    /// StikDebug for JIT for its own process, with the universal script on iOS 26 and later; Henge's requests work there by
    /// pid. It does not declare the stikjit scheme, so isStikDebugInstalled is always false inside it.
    static var isInLiveContainer: Bool { getenv("LC_HOME_PATH") != nil }

    /// Signed without get-task-allow: no debugger may attach to this copy, so StikDebug and the built-in helper can never turn
    /// JIT on (#19). Some signing tools and services leave it out (iOS App Signer's "No get-task-allow"); SideStore and
    /// AltStore keep it. TrollStore and a jailbreak do not need it. Unknown signatures are not reported.
    static var signedWithoutDebugging: Bool {
        Entitlements.debuggable == false && !isInstalledWithTrollStore && !isJailbroken && !debuggedAtLaunch
    }

    static let withoutDebuggingFailure = "This copy of Henge was signed without the get-task-allow entitlement, so no debugger "
        + "is allowed to attach to it and JIT cannot turn on. Install Henge again with SideStore or AltStore (AltServer runs "
        + "on Windows too), or with a signing tool that keeps get-task-allow (in iOS App Signer, turn off No get-task-allow)."

    /// A rootless jailbreak (Dopamine and its kin) puts its files in /var/jb and lets apps see it.
    static var isJailbroken: Bool { FileManager.default.fileExists(atPath: "/var/jb") }

    /// Whether Henge was already marked as debugged when it was opened, before it asked anything of anyone. That is what a
    /// jailbreak that allows JIT in apps does to every app it launches (Dopamine's "Allow JIT in Apps" setting), and it needs
    /// no hand-off at all. Read once, early (HengeApp.init), so a later attach is not mistaken for it.
    nonisolated(unsafe) static var debuggedAtLaunch = false
    static func noteLaunchState() {
        debuggedAtLaunch = (csStatus() ?? 0) & CS_DEBUGGED != 0
        HengeLog.log("jit", "debugged at launch: \(debuggedAtLaunch ? "yes" : "no"); get-task-allow: "
                         + (Entitlements.debuggable.map { $0 ? "yes" : "NO" } ?? "unknown")
                         + (isInLiveContainer ? "; running inside LiveContainer" : ""))
        if signedWithoutDebugging { lastFailure = withoutDebuggingFailure }
    }

    /// Whether this iOS lets a process that is marked as debugged make executable memory for itself, as opposed to needing a
    /// debugger to hand it out: true before iOS 26, which is when the Trusted Execution Monitor arrived. It is what makes
    /// TrollStore and jailbreaks enough, without StikDebug.
    static var canGrantOwnJIT: Bool {
        if #available(iOS 26, *) { return false }
        return true
    }

    /// henge-jit.js as standard base64, which Built-in StikJIT's custom script takes.
    static var scriptBase64: String? {
        loadScript()?.data(using: .utf8)?.base64EncodedString()
    }

    /// Ask StikDebug to attach to us and run the JIT script.
    ///
    /// This backgrounds Henge — iOS switches to StikDebug, which attaches over the
    /// debugserver protocol, runs the script, and relaunches us. Everything after
    /// this point happens in a *new* foreground pass of the app.
    @MainActor
    static func requestTrollStoreAttach() -> Bool {
        HengeLog.log("jit", "requestTrollStoreAttach() -- handing off to TrollStore")
        guard let bundleID = Bundle.main.bundleIdentifier else { return false }
        
        let url = "apple-magnifier://enable-jit?bundle-id=\(bundleID)"
        guard isTrollStoreInstalled, let launchURL = URL(string: url) else {
            HengeLog.log("jit", "FAIL: TrollStore is not installed")
            return false
        }
        
        HengeLog.log("jit", "opening apple-magnifier:// for bundle \(bundleID)")
        UIApplication.shared.open(launchURL)
        return true
    }

    @MainActor
    static func requestAttach() -> Bool {
        HengeLog.log("jit", "requestAttach() -- handing off to StikDebug")

        guard let bundleID = Bundle.main.bundleIdentifier else {
            HengeLog.log("jit", "FAIL: no bundle identifier")
            return false
        }

        // pid, not just bundle-id: StikDebug's bundle-id path launches that id via installd.
        // A LiveContainer guest has no such record; only the container is installed. The pid
        // path attaches to this already-running process. bundle-id stays as the fallback.
        var url = "stikjit://enable-jit?bundle-id=\(bundleID)&pid=\(getpid())"

        // Sending the script inline makes Henge self-contained. StikDebug accepts
        // script-data unconditionally (HomeView.handleExternalURL), so we do not
        // depend on the user having assigned a script to Henge in StikDebug's UI.
        if let script = loadScript(), let encoded = base64URLEncode(script) {
            HengeLog.log("jit", "loaded henge-jit.js (\(script.count) chars, "
                             + "\(encoded.count) chars encoded); sending inline")
            url += "&script-data=\(encoded)"
        } else {
            HengeLog.log("jit", "WARNING: could not load henge-jit.js from the bundle. "
                             + "Falling back to whatever script StikDebug has assigned "
                             + "to this bundle id -- if none, JIT will not be enabled.")
        }

        guard let launchURL = URL(string: url) else {
            HengeLog.log("jit", "FAIL: malformed StikDebug URL (\(url.count) chars)")
            return false
        }

        // Not canOpenURL first: that needs LSApplicationQueriesSchemes for 'stikjit', which is
        // checked against whichever app iOS actually installed — a container app hosting Henge
        // fails the query even when StikDebug is installed. open() has no such restriction.
        HengeLog.log("jit", "opening stikjit:// for bundle \(bundleID), pid \(getpid()); "
                         + "Henge will be backgrounded and relaunched after attach")
        UIApplication.shared.open(launchURL, options: [:]) { success in
            if !success {
                HengeLog.log("jit", "FAIL: open(stikjit://) did not succeed -- StikDebug is not installed")
            }
        }
        return true
    }

    /// StikDebug's enable-jit with no pid: StikDebug launches Henge itself, under its debugger, with Henge's script. For a
    /// restart (Relaunch), when StikDebug is what opens Henge again; the pid path attaches to a Henge that is running.
    static func stikDebugLaunchURL() -> URL? {
        guard let bundleID = Bundle.main.bundleIdentifier else { return nil }
        var url = "stikjit://enable-jit?bundle-id=\(bundleID)"
        if let script = loadScript(), let encoded = base64URLEncode(script) { url += "&script-data=\(encoded)" }
        return URL(string: url)
    }

    private static func loadScript() -> String? {
        // Accept either bundle layout: the build copies henge-jit.js to the bundle
        // root, but a folder-reference build phase would put it under Resources/.
        let candidates = [
            Bundle.main.path(forResource: "henge-jit", ofType: "js"),
            Bundle.main.path(forResource: "henge-jit", ofType: "js", inDirectory: "Resources"),
        ]
        for case let path? in candidates {
            if let s = try? String(contentsOfFile: path, encoding: .utf8) { return s }
        }
        return nil
    }

    /// StikDebug expects base64url (`-`/`_`, no padding) and percent-encodes it
    /// into the query string.
    private static func base64URLEncode(_ s: String) -> String? {
        guard let data = s.data(using: .utf8) else { return nil }
        let b64 = data.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .trimmingCharacters(in: CharacterSet(charactersIn: "="))
        return b64.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed)
    }
}
