// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UIKit

/// What Henge does as it opens again after restarting itself.
struct RelaunchRequest: Codable {
    enum Action: Codable, Equatable {
        case startMac(UUID)
        case install(release: String, name: String, ramMB: Int, cpus: Int, keepImage: Bool)
    }

    /// What opens Henge again. It decides what the new Henge waits for before it asks for JIT.
    enum Route: String, Codable {
        /// TrollStore's enable-jit: opens Henge and makes it debugged.
        case trollStore
        /// LocalDevVPN's enable?scheme=henge: connects (Built-in StikJIT and StikDebug go through it) and opens henge://.
        case localDevVPN
        /// StikDebug's enable-jit with no pid: launches Henge under its debugger and runs Henge's script.
        case stikDebug
        /// Nothing here can open Henge: the person opens it.
        case byHand
    }

    var action: Action
    var route: Route
    var made: Date
}

/// Henge's engine runs one Mac per launch of the app: once a Mac, or an install, has run, the next needs a fresh
/// process. iOS has no call for an app to restart itself, so Henge writes down what was asked, has an app that can
/// open Henge do so (TrollStore, LocalDevVPN or StikDebug, whichever is here), and closes; the new Henge finds the
/// note and carries on with it. Nothing is cut short: a Mac shutting down finishes first, and a download or an
/// install in progress stops the restart.
@MainActor
final class Relaunch: ObservableObject {
    static let shared = Relaunch()

    /// What the restart screen says, in the Henge that closes and again in the one that opens; nil the rest of the time.
    @Published private(set) var restarting: String?
    /// With it, for a suspended Mac: its screen as it was, under "Resuming…" (ResumingCover), not a start's page.
    @Published private(set) var resumeScreen: URL?
    /// Why Henge didn't restart, or that it needs opening by hand.
    @Published var notice: Notice?

    struct Notice: Identifiable {
        let id = UUID()
        let title: String
        let message: String
        /// The notice offers to close Henge, which the person then opens again.
        var closes = false
    }

    /// Whether the next Mac needs a fresh Henge: one has run, or is running, in this launch.
    static var needed: Bool { henge_vm_state() != 0 }

    private var leaving = false

    // MARK: Asking

    /// Start `mac` in a fresh Henge.
    func start(_ mac: VirtualMac) {
        let installer = Installer.shared
        // A download carries on through the restart (ImageDownloads); its install is then started from the Install tab.
        if installer.busy && installer.phase != .downloading {
            notice = Notice(title: "Henge is installing macOS",
                            message: "Starting \(mac.name) needs Henge to restart, which would stop the install of \(installer.machineName). Start it when the install is done.")
            return
        }
        let screen = ResumePicture.screen(for: mac)
        ResumePicture.preload(screen)
        restart(.startMac(mac.id), saying: "Starting \(mac.name)…", after: "\(mac.name) starts by itself", screen: screen)
    }

    /// Install `release` in a fresh Henge, which finds the download already there.
    func install(_ release: MacOSRelease, name: String, ramMB: Int, cpus: Int, keepImage: Bool) {
        restart(.install(release: release.id, name: name, ramMB: ramMB, cpus: cpus, keepImage: keepImage),
                saying: "Getting ready to install \(release.name)…", after: "the install carries on by itself")
    }

    /// The person chose to close Henge (nothing here can open it again): to the Home Screen as if swiped away, then gone.
    func closeNow() {
        HengeLog.log("relaunch", "closing; the person opens Henge again")
        HengeLog.flushNow()
        UIApplication.shared.perform(NSSelectorFromString("suspend"))
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { exit(0) }
    }

    /// The person kept Henge open instead: the request is dropped.
    func stayOpen() {
        Self.clear()
        HengeLog.log("relaunch", "the person kept Henge open")
    }

    // MARK: Closing

    private func restart(_ action: RelaunchRequest.Action, saying text: String, after outcome: String, screen: URL? = nil) {
        guard restarting == nil, !leaving else { return }
        restarting = text
        resumeScreen = screen
        HengeLog.log("relaunch", "a Mac has run in this launch; restarting Henge for \(action)")
        Task {
            // A Mac shutting down finishes first: the pause queue runs in order, so this comes after its shut down.
            await MacPause.shared.settled()
            // Then the engine: its CPUs stop at their next block.
            let deadline = Date().addingTimeInterval(15)
            while henge_vm_state() == 1, Date() < deadline { try? await Task.sleep(nanoseconds: 100_000_000) }
            guard henge_vm_state() != 1 else {
                HengeLog.log("relaunch", "the machine has not stopped; not restarting")
                restarting = nil
                resumeScreen = nil
                notice = Notice(title: "A Mac is still on", message: "Henge can restart once it has shut down. Try again in a moment.")
                return
            }
            let route = Self.route
            Self.save(RelaunchRequest(action: action, route: route, made: Date()))
            try? await Task.sleep(nanoseconds: 350_000_000) // the screen is seen, not a flash
            leave(route, outcome: outcome)
        }
    }

    /// Whichever app here can open Henge again. LocalDevVPN before StikDebug: it answers after a fixed second,
    /// so Henge is gone by then, and Built-in StikJIT and StikDebug both need it connected anyway.
    private static var route: RelaunchRequest.Route {
        // Inside LiveContainer those apps would open the container, not this copy of Henge.
        if getenv("LC_HOME_PATH") != nil { return .byHand }
        if JITCoordinator.shared.resolvedMethod == .trollStore, JITBootstrap.isTrollStoreInstalled { return .trollStore }
        if LocalDevVPN.isInstalled { return .localDevVPN }
        if JITBootstrap.isStikDebugInstalled { return .stikDebug }
        return .byHand
    }

    private static func url(for route: RelaunchRequest.Route) -> URL? {
        let id = Bundle.main.bundleIdentifier ?? ""
        switch route {
        case .trollStore: return URL(string: "apple-magnifier://enable-jit?bundle-id=\(id)")
        case .localDevVPN: return LocalDevVPN.connect
        case .stikDebug: return JITBootstrap.stikDebugLaunchURL()
        case .byHand: return nil
        }
    }

    private func leave(_ route: RelaunchRequest.Route, outcome: String) {
        guard let url = Self.url(for: route) else {
            askToCloseByHand(outcome: outcome)
            return
        }
        leaving = true
        HengeLog.log("relaunch", "closing; \(route.rawValue) opens Henge again")
        HengeLog.flushNow()
        // Gone before the other app opens Henge: were this Henge still here, iOS would bring it back instead of a new one.
        // Whichever comes first: the other app has opened, or this one has left the screen.
        let gone = NotificationCenter.default.addObserver(forName: UIApplication.didEnterBackgroundNotification,
                                                          object: nil, queue: .main) { _ in exit(0) }
        UIApplication.shared.open(url, options: [:]) { opened in
            if opened { exit(0) }
            Task { @MainActor in
                NotificationCenter.default.removeObserver(gone)
                HengeLog.log("relaunch", "\(route.rawValue) did not open")
                self.leaving = false
                self.askToCloseByHand(outcome: outcome)
            }
        }
    }

    private func askToCloseByHand(outcome: String) {
        restarting = nil
        resumeScreen = nil
        notice = Notice(title: "Henge needs to restart",
                        message: "Only one Mac can run each time Henge is opened. Close Henge, then open it again: \(outcome).",
                        closes: true)
    }

    // MARK: Opening again

    private static var file: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("relaunch-request.json")
    }

    private static func save(_ request: RelaunchRequest) {
        guard let data = try? JSONEncoder().encode(request) else { return }
        try? FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? data.write(to: file, options: .atomic)
    }

    private static func clear() { try? FileManager.default.removeItem(at: file) }

    /// The request the last Henge left as it restarted. Read and removed as this one opens (HengeApp.init). Only a recent
    /// one counts, so opening Henge long afterwards never starts a Mac by surprise.
    static let atLaunch: RelaunchRequest? = {
        guard let data = try? Data(contentsOf: file) else { return nil }
        clear()
        guard let request = try? JSONDecoder().decode(RelaunchRequest.self, from: data),
              Date().timeIntervalSince(request.made) < 10 * 60 else { return nil }
        return request
    }()

    private var handedOut = false

    /// The request to carry out, once.
    func takeRequest() -> RelaunchRequest? {
        guard !handedOut else { return nil }
        handedOut = true
        return Self.atLaunch
    }

    /// The app that opened Henge may be turning JIT on as it does (TrollStore, StikDebug), or have just been asked to
    /// connect LocalDevVPN, which Built-in StikJIT needs: a few seconds for that before `then` asks for JIT, so it
    /// isn't asked for twice. The restart screen stays up meanwhile.
    func awaitJIT(after route: RelaunchRequest.Route, saying text: String, screen: URL? = nil, then go: @escaping () -> Void) {
        restarting = text
        resumeScreen = screen
        Task {
            let wait: TimeInterval = switch route {
            case .trollStore, .stikDebug: 10
            case .localDevVPN: 5
            case .byHand: 0
            }
            let deadline = Date().addingTimeInterval(wait)
            while Date() < deadline {
                if JITBootstrap.debuggedFlag {
                    JITBootstrap.prewarm() // the debugger is here: claim the region while it is
                    break
                }
                if route == .localDevVPN, LocalDevVPN.isConnected { break }
                try? await Task.sleep(nanoseconds: 250_000_000)
            }
            HengeLog.log("relaunch", "opened again by \(route.rawValue); JIT \(JITBootstrap.ready ? "on" : "not on yet")")
            restarting = nil
            resumeScreen = nil
            go()
        }
    }
}

/// What shows while Henge restarts, or turns JIT on for a Mac it is opening into.
struct RestartScreen: View {
    let text: String
    /// What is happening underneath (JIT's own status), in smaller type.
    var detail: String?
    /// Offered as Not Now: back to the Macs tab.
    var cancel: (() -> Void)?
    /// A suspended Mac going on: its screen as it was, under "Resuming…", as its session then shows it.
    var resume: URL?

    var body: some View {
        if let resume {
            ResumingCover(screen: resume, detail: detail, cancel: cancel)
        } else {
            starting
        }
    }

    private var starting: some View {
        ZStack {
            Desk.page.ignoresSafeArea()
            VStack(spacing: 16) {
                ProgressView().controlSize(.large).tint(Desk.accentLight)
                Text(text).font(.system(size: 17, weight: .semibold)).foregroundStyle(Desk.text)
                    .multilineTextAlignment(.center)
                if let detail {
                    Text(detail).font(.system(size: 14)).foregroundStyle(Desk.dim).multilineTextAlignment(.center)
                        .contentTransition(.opacity)
                }
                if let cancel {
                    Button("Not Now", action: cancel)
                        .font(.system(size: 15, weight: .semibold)).foregroundStyle(Desk.accentLight)
                        .padding(.top, 8)
                }
            }
            .padding(24)
            .animation(.easeInOut(duration: 0.2), value: detail)
        }
    }
}
