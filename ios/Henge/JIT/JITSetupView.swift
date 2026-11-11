// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UniformTypeIdentifiers

/// The JIT walkthrough: pick a way in, follow its numbered steps, end with JIT on.
///
/// Three ways in. Pairing on this iPhone (iOS 27) and a pairing file made on a
/// computer both lead to Built-in StikJIT and share the last two steps,
/// LocalDevVPN and enabling. StikDebug has its own short page. Presented from
/// onboarding, Settings › JIT, and whenever Start finds JIT not set up.
struct JITSetupFlow: View {
    enum Step: Hashable { case pairOnDevice, importFile, connect, enable, stikDebug, trollStore, jailbreak }

    @ObservedObject private var jit = JITCoordinator.shared
    @ObservedObject private var pairing = OnDevicePairing.shared
    @State private var path: [Step] = []
    @State private var importing = false
    @State private var vpnTick = 0
    @Environment(\.dismiss) private var dismiss
    @Environment(\.scenePhase) private var scenePhase

    private var device: String { OnDevicePairing.deviceKind }

    var body: some View {
        NavigationStack(path: $path) {
            choose
                .hengeDestination(for: Step.self) { step in
                    switch step {
                    case .pairOnDevice: pairOnDevice
                    case .importFile: importFile
                    case .connect: connect
                    case .enable: enable
                    case .stikDebug: stikDebug
                    case .trollStore: trollStore
                    case .jailbreak: jailbreak
                    }
                }
        }
        .tint(Theme.accent)
        .fileImporter(isPresented: $importing, allowedContentTypes: [.propertyList, .data]) { result in
            switch result {
            case .success(let url): jit.importPairingFile(url)
            case .failure(let failure):
                HengeLog.log("jit", "pairing import failed: \(failure.localizedDescription)")
            }
        }
        // A new pairing (made here or imported) goes straight on: no Continue to tap.
        .onChange(of: jit.pairingSource) { source in
            guard source != nil, let last = path.last, last == .importFile || last == .pairOnDevice else { return }
            Task {
                try? await Task.sleep(nanoseconds: 900_000_000) // long enough to see the tick
                if path.last == last { path.append(afterPairing) }
            }
        }
        .onChange(of: scenePhase) { phase in if phase == .active { vpnTick += 1 } }
        .onAppear {
            jit.refreshPairingStatus()
            pairing.reset()
            // A failed enable from the Library lands on the step that can fix it.
            if jit.error != nil, jit.hasPairing, jit.resolvedMethod == .builtIn {
                path = [.enable]
            }
            HengeLog.log("ui", "JIT walkthrough opened")
        }
        // Swiping the sheet away, or a copy shown from onboarding, must not
        // leave a request to show it again behind.
        .onDisappear { jit.showSetup = false }
    }

    /// LocalDevVPN's page only when it still has to be installed: Turn On JIT connects an installed one by itself.
    private var afterPairing: Step { LocalDevVPN.isInstalled ? .enable : .connect }

    private func close() {
        jit.showSetup = false
        dismiss()
    }

    // MARK: Choose

    private var choose: some View {
        page(symbol: "bolt.fill", title: "Turn on JIT",
             subtitle: "A Mac needs memory it can write and then run, which on iOS only an attached "
                     + "debugger can grant. Choose how your \(device) gets one.") {
            VStack(spacing: 10) {
                if JITBootstrap.signedWithoutDebugging {
                    notice(JITBootstrap.withoutDebuggingFailure, symbol: "exclamationmark.triangle.fill", warning: true)
                } else if JITBootstrap.isInLiveContainer {
                    notice("Henge is running inside LiveContainer. In LiveContainer's settings choose StikDebug as the JIT "
                           + "enabler and open Henge with Launch with JIT, or use StikDebug below: Henge asks it to attach "
                           + "with Henge's own script.", symbol: "shippingbox")
                }
                let builtIn = HengeBuiltInJIT.unavailableReason
                way("Pair on this \(device)", symbol: "iphone.radiowaves.left.and.right",
                    detail: builtIn ?? (!OnDevicePairing.isSupported ? "Needs iOS 27 or later."
                        : jit.pairingSource == .onDevice ? "Paired on this \(device)."
                        : "No computer needed. Pairs from Settings in a minute."),
                    done: jit.pairingSource == .onDevice,
                    enabled: builtIn == nil && OnDevicePairing.isSupported) { path.append(.pairOnDevice) }
                way("Use a pairing file", symbol: "doc.badge.plus",
                    detail: builtIn ?? (jit.pairingSource == .imported ? "Pairing file imported."
                        : "Import a pairing file made on a computer."),
                    done: jit.pairingSource == .imported,
                    enabled: builtIn == nil) { path.append(.importFile) }
                way("Use StikDebug", symbol: "ant",
                    detail: JITBootstrap.isStikDebugInstalled ? "StikDebug is installed."
                        : JITBootstrap.isInLiveContainer ? "The way in from LiveContainer."
                        : "Enable JIT through the StikDebug app.",
                    done: jit.method == .stikDebug) { path.append(.stikDebug) }
                way("Use TrollStore", symbol: "sparkles",
                    detail: JITBootstrap.isTrollStoreInstalled ? "TrollStore is installed."
                        : "For a Henge installed through TrollStore.",
                    done: jit.method == .trollStore) { path.append(.trollStore) }
                way("Use a jailbreak", symbol: "lock.open",
                    detail: JITBootstrap.debuggedAtLaunch ? "JIT was already on when Henge opened."
                        : JITBootstrap.isJailbroken ? "A jailbreak was found. Turn on Allow JIT in Apps."
                        : "Dopamine can give every app JIT with one setting.",
                    done: jit.method == .jailbreak || JITBootstrap.debuggedAtLaunch) { path.append(.jailbreak) }
            }
        } actions: {
            if jit.hasPairing && HengeBuiltInJIT.isAvailable {
                Button { path.append(afterPairing) } label: {
                    Label("Turn On JIT", systemImage: "bolt.fill")
                }
                .buttonStyle(PrimaryButtonStyle())
            }
            Button("Not now") { close() }
                .font(.system(size: 15, weight: .medium))
                .foregroundStyle(Theme.textDim)
        }
    }

    /// Something about this install that decides how, or whether, JIT can come on.
    private func notice(_ text: String, symbol: String, warning: Bool = false) -> some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: symbol)
                .font(.system(size: 17, weight: .semibold))
                .foregroundStyle(warning ? Color.orange : Theme.accent)
            Text(text).font(.system(size: 13)).foregroundStyle(Theme.text)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(14)
        .hengeCard()
    }

    private func way(_ title: String, symbol: String, detail: String, done: Bool,
                     enabled: Bool = true, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: 14) {
                Image(systemName: symbol)
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(Theme.accent)
                    .frame(width: 34, height: 34)
                    .background(Theme.accentSoft, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
                VStack(alignment: .leading, spacing: 3) {
                    Text(title).font(.system(size: 16, weight: .semibold)).foregroundStyle(Theme.text)
                    Text(detail).font(.system(size: 13)).foregroundStyle(Theme.textDim)
                        .fixedSize(horizontal: false, vertical: true)
                        .multilineTextAlignment(.leading)
                }
                Spacer(minLength: 8)
                Image(systemName: done ? "checkmark.circle.fill" : "chevron.right")
                    .font(.system(size: done ? 18 : 13, weight: .semibold))
                    .foregroundStyle(done ? .green : Theme.textDim.opacity(0.7))
            }
            .padding(14)
            .contentShape(Rectangle())
            .hengeCard()
        }
        .buttonStyle(CardButtonStyle())
        .disabled(!enabled)
        .opacity(enabled ? 1 : 0.5)
    }

    // MARK: Pair on this device

    private var pairedOnDevice: Bool {
        if case .paired = pairing.phase { return true }
        return false
    }

    private var pairOnDevice: some View {
        page(symbol: "iphone.radiowaves.left.and.right", title: "Pair on this \(device)",
             subtitle: "Henge pretends to be a computer on your Wi-Fi, and your \(device) pairs with it "
                     + "the way it would with a Mac.") {
            VStack(alignment: .leading, spacing: 14) {
                point(1, "Turn on Wi-Fi, tap **Start pairing** below and allow Local Network access.",
                      done: pairing.phase != .idle)
                point(2, "Open **Settings › Privacy & Security › Developer Mode**, scroll down and tap "
                       + "**Pair with \(OnDevicePairing.hostName)**.",
                      done: pairing.isShowingPin || pairedOnDevice)
                point(3, "Settings first asks for **your \(device)'s own passcode** (the one you unlock it with). "
                       + "Enter it and stay in Settings: closing that prompt cancels the pairing.",
                      done: pairing.isShowingPin || pairedOnDevice)
                point(4, "Then enter the 6-digit code Henge shows. It also appears at the top of the screen, "
                       + "so you don't have to switch back.",
                      done: pairedOnDevice)
                point(5, "Come back to Henge.", done: pairedOnDevice)
            }
            .padding(16).hengeCard()
            pairingStatus
        } actions: {
            switch pairing.phase {
            case .idle, .failed:
                Button { pairing.start() } label: {
                    Label("Start pairing", systemImage: "dot.radiowaves.left.and.right")
                }
                .buttonStyle(PrimaryButtonStyle())
            case .waiting, .pin:
                Button("Cancel pairing", role: .cancel) { pairing.cancel() }
                    .font(.system(size: 15, weight: .medium))
                    .foregroundStyle(Theme.textDim)
            case .paired:
                Button("Continue") { path.append(afterPairing) }
                    .buttonStyle(PrimaryButtonStyle())
            }
        }
    }

    @ViewBuilder private var pairingStatus: some View {
        switch pairing.phase {
        case .idle:
            EmptyView()
        case .waiting:
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 10) {
                    ProgressView().tint(Theme.accent)
                    Text("Waiting for your \(device)…").font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(Theme.text)
                }
                Button("Open Settings") {
                    if let url = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(url) }
                }
                .font(.system(size: 14, weight: .semibold))
                if pairing.waitedLong {
                    Text("No pairing request yet. If Settings asked for your \(device) passcode and you closed it, "
                       + "tap Pair with \(OnDevicePairing.hostName) again and enter the passcode.")
                        .font(.system(size: 13)).foregroundStyle(Theme.textDim)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if pairing.backgroundLimited {
                    Text("This installation can only wait about 30 seconds in the background, "
                       + "so go to Settings straight away.")
                        .font(.system(size: 13)).foregroundStyle(.orange)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(16).hengeCard()
        case .pin(let pin):
            VStack(spacing: 8) {
                Text("Enter this code on your \(device)").font(.system(size: 14))
                    .foregroundStyle(Theme.textDim)
                Text(pin).font(.system(size: 40, weight: .bold, design: .monospaced)).tracking(6)
                    .foregroundStyle(Theme.text)
                    .textSelection(.enabled)
                    .accessibilityLabel("Pairing code \(pin.map(String.init).joined(separator: " "))")
            }
            .frame(maxWidth: .infinity)
            .padding(18).hengeCard(high: true)
        case .paired(let name):
            outcome("Paired with \(name)", ok: true)
        case .failed(let message):
            outcome(message, ok: false)
        }
    }

    // MARK: Import a pairing file

    private var importFile: some View {
        page(symbol: "doc.badge.plus", title: "Use a pairing file",
             subtitle: "A pairing file made on a computer lets Henge talk to this \(device) the same way.") {
            VStack(alignment: .leading, spacing: 14) {
                point(1, "On a computer, make this \(device)'s pairing file with the "
                       + "[StikDebug pairing-file guide](https://github.com/StikDebug/StikDebug-Guide/blob/main/pairing_file.md).",
                      done: jit.pairingSource == .imported)
                point(2, "Save it to Files, or AirDrop it to this \(device).",
                      done: jit.pairingSource == .imported)
                point(3, "Tap **Choose pairing file** and pick it.", done: jit.pairingSource == .imported)
            }
            .padding(16).hengeCard()
            Label("The pairing file stays in Henge's Documents folder and is only sent to Henge's own helper.",
                  systemImage: "lock.fill")
                .font(.system(size: 13)).foregroundStyle(Theme.textDim)
            if let error = jit.error { outcome(error, ok: false) }
        } actions: {
            if jit.pairingSource == .imported {
                Button("Continue") { path.append(afterPairing) }.buttonStyle(PrimaryButtonStyle())
                Button("Choose another file") { importing = true }
                    .font(.system(size: 15, weight: .medium))
            } else {
                Button { importing = true } label: { Label("Choose pairing file", systemImage: "folder") }
                    .buttonStyle(PrimaryButtonStyle())
            }
        }
    }

    // MARK: LocalDevVPN

    private var connect: some View {
        page(symbol: "network.badge.shield.half.filled", title: "Connect LocalDevVPN",
             subtitle: "Henge's helper reaches this \(device)'s debugging service through a local VPN. "
                     + "Nothing leaves your \(device).") {
            VStack(alignment: .leading, spacing: 14) {
                point(1, "Install [LocalDevVPN](\(LocalDevVPN.appStore.absoluteString)) from the App Store.",
                      done: LocalDevVPN.isInstalled)
                point(2, "Come back to Henge. From then on Henge connects it by itself whenever it turns JIT on.",
                      done: LocalDevVPN.isConnected)
            }
            .padding(16).hengeCard()
            .id(vpnTick) // re-checked each time Henge comes back to the front
        } actions: {
            if LocalDevVPN.isInstalled {
                Button { path.append(.enable) } label: { Label("Turn On JIT", systemImage: "bolt.fill") }
                    .buttonStyle(PrimaryButtonStyle())
            } else {
                Button("Get LocalDevVPN") { LocalDevVPN.open() }
                    .buttonStyle(PrimaryButtonStyle())
                Button("Continue without checking") { path.append(.enable) }
                    .font(.system(size: 15, weight: .medium))
            }
        }
        .id(vpnTick)
    }

    // MARK: Enable

    private var attached: Bool { jit.attachGeneration > 0 || JITBootstrap.debuggedFlag }

    private var enable: some View {
        page(symbol: "bolt.badge.checkmark", title: "Turn on JIT",
             subtitle: "Henge connects LocalDevVPN if it is off, prepares Apple's Developer Disk Image and attaches "
                     + "its helper. The first time can take a minute; after that it takes seconds, and starting a Mac "
                     + "does it by itself.") {

            if jit.busy {
                HStack(spacing: 11) {
                    ProgressView().tint(Theme.accent)
                    Text(jit.status ?? "Working…").font(.system(size: 14)).foregroundStyle(Theme.text)
                    Spacer(minLength: 0)
                }
                .padding(16).hengeCard(high: true)
            } else if attached {
                outcome("JIT is on. A Mac can start.", ok: true)
            } else if let error = jit.error {
                VStack(alignment: .leading, spacing: 10) {
                    outcome(error, ok: false)
                    if jit.advice?.actions.contains(.pairAgain) == true {
                        Button(OnDevicePairing.isSupported ? "Pair again" : "Import a new pairing file") {
                            path = [OnDevicePairing.isSupported ? .pairOnDevice : .importFile]
                        }
                        .font(.system(size: 14, weight: .semibold))
                    }
                    if jit.advice?.actions.contains(.connectLocalDevVPN) == true {
                        Button(LocalDevVPN.actionTitle) { LocalDevVPN.open() }
                            .font(.system(size: 14, weight: .semibold))
                    }
                }
            } else if let status = jit.status {
                outcome(status, ok: true)
            }

            if !attached {
                Button("Reset Developer Disk Image", role: .destructive) { jit.resetDDI() }
                    .font(.system(size: 13, weight: .medium))
                    .disabled(jit.busy)
                    .padding(.top, 4)
            }
        } actions: {
            if attached {
                Button("Done") { close() }.buttonStyle(PrimaryButtonStyle())
            } else {
                Button { jit.turnOnBuiltIn() } label: {
                    Label(jit.error == nil ? "Turn On JIT" : "Try Again", systemImage: "bolt.fill")
                }
                .buttonStyle(PrimaryButtonStyle(enabled: !jit.busy))
                .disabled(jit.busy)
            }
        }
        // Arriving here is the request: start at once, unless the last try failed (then the advice shows first).
        .onAppear { if !attached && !jit.busy && jit.error == nil { jit.turnOnBuiltIn() } }
        .onChange(of: jit.attachGeneration) { _ in
            Task {
                try? await Task.sleep(nanoseconds: 1_200_000_000)
                close()
            }
        }
    }

    // MARK: StikDebug

    private var stikDebug: some View {
        page(symbol: "ant", title: "Use StikDebug",
             subtitle: "StikDebug is a separate app that attaches to Henge. Henge sends it the JIT script "
                     + "itself, so nothing needs configuring for Henge inside StikDebug.") {
            VStack(alignment: .leading, spacing: 14) {
                point(1, "Install [StikDebug](https://github.com/StikDebug/StikDebug/releases/latest).",
                      done: JITBootstrap.isStikDebugInstalled)
                point(2, "Import this \(device)'s pairing file into StikDebug.")
                point(3, "Install and connect [LocalDevVPN](\(LocalDevVPN.appStore.absoluteString)).")
                point(4, "Whenever you start a Mac, Henge opens StikDebug, which attaches and comes back.")
            }
            .padding(16).hengeCard()
            Label("To open Henge from StikDebug's app list instead, assign it a script once: in StikDebug long-press Henge, "
                  + "choose Assign Script and pick universal.js. Opened from the list without one, JIT stays off.",
                  systemImage: "info.circle")
                .font(.system(size: 13)).foregroundStyle(Theme.textDim)
                .fixedSize(horizontal: false, vertical: true)
        } actions: {
            Button("Use StikDebug") {
                jit.method = .stikDebug
                HengeLog.log("ui", "JIT method set to StikDebug")
                close()
            }
            .buttonStyle(PrimaryButtonStyle())
        }
    }

    // MARK: TrollStore

    private var trollStore: some View {
        page(symbol: "sparkles", title: "Use TrollStore",
             subtitle: "TrollStore can enable JIT for apps it installed, with no pairing file, "
                     + "VPN or computer.") {
            VStack(alignment: .leading, spacing: 14) {
                point(1, "Install Henge.ipa through TrollStore, on an iOS version TrollStore supports. It carries the "
                       + "entitlements TrollStore keeps, so the same file works there and in a sideloader.",
                      done: JITBootstrap.isInstalledWithTrollStore)
                point(2, "In TrollStore's Settings, turn on URL Scheme. TrollStore ignores enable-jit requests without it.")
                point(3, "Whenever a Mac starts, Henge asks TrollStore to enable JIT. TrollStore opens Henge, "
                       + "attaches to it for a moment and lets go, which leaves it allowed to run code it wrote.")
            }
            .padding(16).hengeCard()
            if !JITBootstrap.isTrollStoreInstalled {
                Label("TrollStore was not found on this \(device). It installs on iOS 14.0 to 17.0.", systemImage: "info.circle")
                    .font(.system(size: 13)).foregroundStyle(Theme.textDim)
                    .fixedSize(horizontal: false, vertical: true)
            }
        } actions: {
            Button("Use TrollStore") {
                jit.method = .trollStore
                HengeLog.log("ui", "JIT method set to TrollStore")
                close()
            }
            .buttonStyle(PrimaryButtonStyle())
        }
    }

    // MARK: Jailbreak

    private var jailbreak: some View {
        page(symbol: "lock.open", title: "Use a jailbreak",
             subtitle: "On a device jailbroken with Dopamine, JIT is a setting: the jailbreak marks every app as "
                     + "debugged when it opens, and a debugged app may run code it wrote. Henge needs nothing else.") {
            VStack(alignment: .leading, spacing: 14) {
                point(1, "Open Dopamine and go to its Settings.", done: JITBootstrap.isJailbroken)
                point(2, "Turn on Allow JIT in Apps.")
                point(3, "Close Henge completely and open it again. The setting applies to apps as they start.",
                      done: JITBootstrap.debuggedAtLaunch)
            }
            .padding(16).hengeCard()
            if JITBootstrap.debuggedAtLaunch {
                Label("Henge was marked as debugged when it opened, so JIT is on.", systemImage: "checkmark.circle.fill")
                    .font(.system(size: 13)).foregroundStyle(.green)
            } else if !JITBootstrap.isJailbroken {
                Label("No jailbreak was found on this \(device).", systemImage: "info.circle")
                    .font(.system(size: 13)).foregroundStyle(Theme.textDim)
            }
        } actions: {
            Button("Use the jailbreak") {
                jit.method = .jailbreak
                HengeLog.log("ui", "JIT method set to jailbreak")
                close()
            }
            .buttonStyle(PrimaryButtonStyle())
        }
    }

    // MARK: Pieces

    private func page<Content: View, Actions: View>(
        symbol: String, title: String, subtitle: String,
        @ViewBuilder content: () -> Content,
        @ViewBuilder actions: () -> Actions) -> some View {
        ZStack {
            Theme.backdrop
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    Image(systemName: symbol)
                        .font(.system(size: 26, weight: .semibold))
                        .foregroundStyle(Theme.accent)
                        .frame(width: 56, height: 56)
                        .background(Theme.accentSoft, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
                    Text(title).font(.system(size: 28, weight: .semibold)).foregroundStyle(Theme.text)
                    Text(subtitle).font(.system(size: 15)).foregroundStyle(Theme.textDim)
                        .fixedSize(horizontal: false, vertical: true)
                    content()
                }
                .padding(.horizontal, 22).padding(.top, 8).padding(.bottom, 24)
            }
        }
        .safeAreaInset(edge: .bottom, spacing: 0) {
            VStack(spacing: 12) { actions() }
                .padding(.horizontal, 22).padding(.top, 10).padding(.bottom, 16)
                .background(Theme.backdrop)
        }
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) {
                Button("Close") { close() }
            }
        }
    }

    private func point(_ number: Int, _ text: String, done: Bool = false) -> some View {
        HStack(alignment: .top, spacing: 12) {
            ZStack {
                Circle().fill(done ? Color.green.opacity(0.18) : Theme.accentSoft)
                if done {
                    Image(systemName: "checkmark").font(.system(size: 12, weight: .bold)).foregroundStyle(.green)
                } else {
                    Text("\(number)").font(.system(size: 13, weight: .bold)).foregroundStyle(Theme.accent)
                }
            }
            .frame(width: 26, height: 26)
            Text(.init(text))
                .font(.system(size: 15))
                .foregroundStyle(Theme.text)
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
        }
    }

    private func outcome(_ text: String, ok: Bool) -> some View {
        Label(text, systemImage: ok ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
            .font(.system(size: 14, weight: .medium))
            .foregroundStyle(ok ? .green : .red)
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(14).hengeCard()
    }
}
