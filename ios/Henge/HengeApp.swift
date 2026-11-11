import SwiftUI
import AVFAudio

@main
struct HengeApp: App {
    /// Answers for external display scenes before iOS 27 (LegacyExternalDisplay.swift).
    @UIApplicationDelegateAdaptor(HengeAppDelegate.self) private var appDelegate
    @Environment(\.scenePhase) private var scenePhase

    init() {
        // Order matters: HengeLog takes over stderr, so the engine's and the JIT
        // allocator's first lines land in Documents/henge.log too.
        HengeLog.start()
        HengeLog.log("boot", "address space: \(henge_largest_reservation_gb()) GB can be reserved in one piece")
        EngineTuning.apply()
        HengeLog.logFootprint("app-launch")
        MemoryWatch.shared.start() // low-memory warnings: give back what can be rebuilt, note a critical moment
        JITBootstrap.noteLaunchState()
        HengeLog.log("app", "increased memory limit: \(Entitlements.increasedMemory ? "yes" : "no"); extended addressing: \(Entitlements.extendedAddressing ? "yes" : "no"); limit \(Entitlements.memoryLimitAtLaunch >> 20) MB")
        // Without the guard, a brk no debugger answers kills the app instead of failing.
        JITBootstrap.installTrapGuard()
        try? AVAudioSession.sharedInstance().setCategory(.playback, options: [.mixWithOthers])
        Microphone.install() // records only while the Mac does
        Machines.shared.collectImports()
        if let left = SessionMarker.atLaunch {
            HengeLog.log("app", "Henge closed last time with \(left.name) \(left.paused ? "paused" : "running")"
                         + (left.memoryCritical.map { ", \(Int(Date().timeIntervalSince($0))) s after iPadOS said memory was critical: most likely closed for memory" } ?? ""))
        }
        if let request = Relaunch.atLaunch { HengeLog.log("relaunch", "opened again by \(request.route.rawValue) to carry on: \(request.action)") }
        // The display question: does this iOS have Apple's paravirtualized graphics at all?
        for path in ["/System/Library/Frameworks/ParavirtualizedGraphics.framework/ParavirtualizedGraphics",
                     "/System/Library/PrivateFrameworks/ParavirtualizedGraphics.framework/ParavirtualizedGraphics"] {
            HengeLog.log("app", "\(path): \(dlopen(path, RTLD_LAZY) != nil ? "present" : "absent")")
        }
    }

    var body: some Scene {
        WindowGroup {
            RootView()
                .onOpenURL { url in HengeLog.log("app", "opened with \(url.absoluteString)") } // henge:// back from LocalDevVPN
        }
    }
}

struct RootView: View {
    enum Tab: Hashable { case macs, install, settings }

    @ObservedObject private var machines = Machines.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @ObservedObject private var requests = LaunchRequests.shared
    @ObservedObject private var relaunch = Relaunch.shared
    @Environment(\.scenePhase) private var scenePhase
    @State private var running: VirtualMac?
    @State private var tab = Tab.macs
    /// A Mac that was on when Henge last closed: said once, at launch.
    @State private var leftover = SessionMarker.atLaunch
    /// "Starting <Mac>…" over everything while JIT turns on for a Mac started at launch, so the Macs tab never flashes by.
    @State private var opening: String?
    /// With it, for a suspended Mac: its screen under "Resuming…" instead (ResumingCover).
    @State private var openingResume: URL?
    /// The session opens from behind that screen: it fades in where it will be, rather than growing out of the page.
    @State private var fromCover = false
    /// Opening straight into the Mac happens once per launch, not each time this view appears.
    private static var openedIntoMac = false

    var body: some View {
        ZStack {
            TabView(selection: $tab) {
                HomeView(running: $running, tab: $tab)
                    .tabItem { Label("Macs", systemImage: "macwindow") }
                    .tag(Tab.macs)
                InstallView()
                    .tabItem { Label("Install", systemImage: "arrow.down.circle") }
                    .tag(Tab.install)
                SettingsView()
                    .tabItem { Label("Settings", systemImage: "gearshape") }
                    .tag(Tab.settings)
            }
            .tint(Desk.accentLight)
            if let running {
                SessionView(mac: running) { self.running = nil }
                    // The Mac's screen opens out of the page, as an app does, and sinks back into it as it closes.
                    .transition(.asymmetric(insertion: fromCover ? .opacity : .opacity.combined(with: .scale(scale: 0.92)),
                                            removal: .opacity.combined(with: .scale(scale: 0.96))))
                    .zIndex(1) // over the page while it goes, not behind it
            }
            if let text = relaunch.restarting {
                RestartScreen(text: text, resume: relaunch.resumeScreen).transition(.opacity)
            } else if let text = opening, running == nil {
                RestartScreen(text: text, detail: jit.busy ? jit.status : nil, cancel: { opening = nil }, resume: openingResume)
                    .transition(.opacity)
            }
        }
        .animation(running != nil ? .spring(response: 0.38, dampingFraction: 0.9) : .easeIn(duration: 0.22), value: running?.id)
        .animation(.easeInOut(duration: 0.2), value: relaunch.restarting)
        .animation(.easeInOut(duration: 0.2), value: opening)
        .preferredColorScheme(.dark)
        .sheet(isPresented: $jit.showSetup) { JITSetupFlow() }
        .alert(leftover?.title ?? "", isPresented: Binding(get: { leftover != nil }, set: { if !$0 { leftover = nil } })) {
            Button("OK", role: .cancel) { leftover = nil }
        } message: {
            Text(leftover?.message ?? "")
        }
        .alert(item: $relaunch.notice) { notice in
            if notice.closes {
                return Alert(title: Text(notice.title), message: Text(notice.message),
                             primaryButton: .default(Text("Close Henge")) { relaunch.closeNow() },
                             secondaryButton: .cancel(Text("Not Now")) { relaunch.stayOpen() })
            }
            return Alert(title: Text(notice.title), message: Text(notice.message), dismissButton: .cancel(Text("OK")))
        }
        // Each time Henge is in the foreground, the first appearance included (onChange misses that one: opened from
        // StikDebug's app list or inside LiveContainer, Henge never claimed the region and said JIT was off). Based on PR #46.
        .task(id: scenePhase) {
            guard scenePhase == .active else { return }
            machines.collectImports()
            jit.enableAtLaunchIfAsked()
            // Back from StikDebug or TrollStore, or opened under a debugger: claim the JIT region while it is there.
            await jit.claimExternalJIT()
        }
        .onChange(of: jit.attachGeneration) { _ in JITBootstrap.prewarm() } // the built-in helper attached
        .onChange(of: running?.id) { _, id in if id == nil { fromCover = false } }
        .onChange(of: requests.startLastMac) { _ in startLastMacIfAsked() }
        .onChange(of: jit.busy) { busy in if !busy, !JITBootstrap.ready { opening = nil } } // JIT failed: its error shows on Start
        .onAppear {
            carryOnAfterRestart()
            openIntoMacIfSet()
            startLastMacIfAsked()
        }
        .task { // testing only: HENGE_AUTOPILOT
            await Autopilot.run { mac in autopilotStart(mac) }
        }
    }

    /// The autopilot's `mac`. The plan is what starts a Mac in a launch it follows (openIntoMacIfSet starts none), and
    /// one Mac per launch: a session already open is left alone, and a Mac that has run in this launch can't be started
    /// again in it (Relaunch), so both are said, not tried. Closing an open session to start another (as this did
    /// before) left the first Mac running with no session while the second's boot was refused (the 10 Nov bench run:
    /// "This Mac could not start", then "A Mac is still on").
    private func autopilotStart(_ mac: VirtualMac) {
        if let open = running {
            HengeLog.log("autopilot", open.id == mac.id
                         ? "\(mac.name)'s session is open already: not started again"
                         : "\(open.name)'s session is open: not starting \(mac.name) (one Mac per launch)")
            return
        }
        guard !Relaunch.needed else {
            HengeLog.log("autopilot", "a Mac has run in this launch: \(mac.name) needs a fresh launch of Henge")
            return
        }
        // A start still waiting for JIT (the Start Mac shortcut): this one is the plan's, and that one then does nothing.
        fromCover = opening != nil
        opening = nil
        running = mac
    }

    /// Henge restarted itself for this (Relaunch): start the Mac, or install, with JIT turned on first.
    private func carryOnAfterRestart() {
        guard let request = relaunch.takeRequest() else { return }
        switch request.action {
        case .startMac(let id):
            guard running == nil, let mac = machines.all.first(where: { $0.id == id }) else { return }
            tab = .macs
            let text = "Starting \(mac.name)…", screen = ResumePicture.screen(for: mac)
            ResumePicture.preload(screen)
            // The restart screen until JIT is asked for, then the same while it comes on: never the Macs tab between.
            relaunch.awaitJIT(after: request.route, saying: text, screen: screen) { openSession(mac, saying: text, resume: screen, covered: true) }
        case let .install(releaseID, name, ramMB, cpus, keepImage):
            guard let release = MacOSRelease.all.first(where: { $0.id == releaseID }) else { return }
            tab = .install
            relaunch.awaitJIT(after: request.route, saying: "Getting ready to install \(release.name)…") {
                jit.turnOn { Installer.shared.start(release, name: name, ramMB: ramMB, cpus: cpus, keepImage: keepImage) }
            }
        }
    }

    /// Settings' "Open straight into my Mac": a fresh Henge starts the Mac used last, as the Start Mac shortcut does.
    /// Not when Henge restarted itself for something else (carryOnAfterRestart has that in hand), nor when the autopilot
    /// is following a plan.
    private func openIntoMacIfSet() {
        guard !Self.openedIntoMac else { return }
        Self.openedIntoMac = true
        guard UserDefaults.standard.bool(forKey: LaunchRequests.openIntoMacKey), Relaunch.atLaunch == nil,
              !machines.all.isEmpty else { return }
        // A launch the autopilot follows: its plan says which Mac starts, and when (Autopilot.swift, `mac`). Both
        // starting raced, and the loser either closed the other's session or benched the wrong Mac.
        guard !Autopilot.enabled else {
            HengeLog.log("app", "Open straight into my Mac: not in a launch the autopilot follows (its plan starts the Mac)")
            return
        }
        HengeLog.log("app", "opening straight into the Mac used last")
        leftover = nil // it starts again now; the note would only cover it
        requests.startLastMac = true
    }

    /// The Start Mac shortcut: the Mac used last, with JIT turned on first when it is off.
    private func startLastMacIfAsked() {
        guard requests.startLastMac else { return }
        requests.startLastMac = false
        guard running == nil,
              let mac = machines.all.max(by: { ($0.lastUsed ?? $0.added) < ($1.lastUsed ?? $1.added) }) else { return }
        tab = .macs
        if Relaunch.needed { Relaunch.shared.start(mac); return }
        let screen = ResumePicture.screen(for: mac)
        ResumePicture.preload(screen)
        openSession(mac, saying: "Starting \(mac.name)…", resume: screen)
    }

    /// Opens `mac`'s session once JIT is on, with "Starting <Mac>…" (or a suspended Mac's screen under "Resuming…")
    /// over everything while it comes on.
    /// covered: the restart screen was up until now.
    private func openSession(_ mac: VirtualMac, saying text: String, resume: URL?, covered: Bool = false) {
        if JITBootstrap.ready {
            fromCover = covered
            running = mac
            return
        }
        opening = text
        openingResume = resume
        jit.turnOn {
            guard opening != nil else { return } // the person chose Not Now while JIT came on
            fromCover = true
            opening = nil
            running = mac
        }
        if !jit.busy, !JITBootstrap.ready { opening = nil } // JIT could not even start (the walkthrough opens)
    }
}

extension View {
    /// Husk's screens are written against an iOS 15 shim; Henge needs iOS 17, so this is the real thing.
    func hengeDestination<D: Hashable, C: View>(for data: D.Type, @ViewBuilder destination: @escaping (D) -> C) -> some View {
        navigationDestination(for: data, destination: destination)
    }
}
