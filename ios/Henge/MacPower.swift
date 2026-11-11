import SwiftUI
import UIKit

/// Shut Down and Restart as a Mac does them: macOS is asked, the same as choosing Shut Down or Restart in the Apple
/// menu, so every app gets to quit, save, and ask about unsaved changes on the Mac's screen (where the person answers
/// them). Henge waits for macOS to turn the Mac off and says it is shutting down meanwhile; the session then closes
/// (after a restart Henge starts the Mac again, as for a restart chosen on the Mac). An app can stop it (Cancel in its
/// "Save changes?"): the Mac keeps running and Henge says so. Force Shut Down (MacPause.shutDown: macOS writes its
/// cache, then the power goes) is in the banner from the start, for a Mac that is stuck; after macOS has taken long
/// enough the banner also says to use it.
///
/// How macOS is asked: the logged-in user's helper (Henge Tools 32) sends loginwindow the Apple menu's own request.
/// The first time, macOS asks on the Mac's screen whether Henge Tools may. Without that helper (not installed, older,
/// nobody logged in, permission turned down) Henge presses the Mac's power button instead, and macOS shows its own
/// Restart / Sleep / Shut Down choice on the Mac's screen.
@MainActor
final class MacPower: ObservableObject {
    static let shared = MacPower()

    enum Kind: Equatable {
        case shutDown, restart
        var verb: String { self == .restart ? "restart" : "shut down" }
        var doing: String { self == .restart ? "Restarting" : "Shutting down" }
    }

    enum Phase: Equatable {
        case idle
        /// A held Mac (paused or suspended) is going on first, so macOS can be asked.
        case resuming(Kind)
        /// Henge Tools was asked and hasn't answered yet.
        case asking(Kind)
        /// macOS is asking the person, on the Mac's screen, to allow Henge Tools to do this (once only).
        case permission(Kind)
        /// macOS has it in hand: apps are quitting.
        case underWay(Kind)
        /// The power button was pressed: macOS asks on its screen what to do.
        case powerButton(Kind)
    }

    @Published private(set) var phase = Phase.idle
    /// Said once, after it ended without the Mac turning off (an app stopped it, say).
    @Published private(set) var notice: String?
    /// macOS has taken longer than a Mac should: the banner says to use Force Shut Down.
    @Published private(set) var slow = false

    /// Also: no auto-save is taken meanwhile (MacPause), from the moment it is asked for, as macOS may begin quitting
    /// apps at any point after (allowed on its screen, or chosen from its power button's choice). After an app or the
    /// person calls it off on the Mac's screen, Henge knows only once the banner is closed, or from Henge Tools.
    var active: Bool { phase != .idle }
    /// What the person asked for, until the session ends: when macOS then turns the Mac off, the session closes.
    private(set) var asked: Kind?

    private var id: UInt32 = 0
    private var since = Date()
    private var timer: Timer?
    private var noticeTimer: Timer?

    /// How long Henge Tools has to say it has the request before the power button is pressed instead.
    private static let answerWait: TimeInterval = 4
    /// How long a held Mac has to go on again.
    private static let resumeWait: TimeInterval = 30
    /// After this long under way (or with macOS's power choice up), the banner says to use Force Shut Down.
    private static let slowAfter: TimeInterval = 45

    func begin(_ kind: Kind, mac: VirtualMac) {
        guard !active, henge_vm_state() == 1 else { return }
        asked = kind
        slow = false
        say(nil)
        since = Date()
        HengeLog.log("power", "\(kind.verb) asked for")
        let pause = MacPause.shared
        pause.powerAsked() // an auto-save under way would finish with apps quitting
        if pause.isHeld {
            phase = .resuming(kind)
            if pause.isPaused { pause.resume() }
        } else {
            MacPreview.saveNow(for: mac) // the screen it had, for the Macs tab
            ask(kind)
        }
        startTimer()
    }

    /// The session ended (closed, or the Mac stopped): nothing is waited for any more.
    func reset() {
        timer?.invalidate()
        timer = nil
        phase = .idle
        slow = false
        asked = nil
        id = 0
    }

    /// The person stops waiting for it, from the banner: Henge stops saying it, macOS goes on with whatever it was
    /// doing (an app's question stays on the Mac's screen), and the session still closes if the Mac turns off.
    func hide() {
        guard active else { return }
        HengeLog.log("power", "stopped waiting in Henge (\(String(describing: phase)))")
        timer?.invalidate()
        timer = nil
        phase = .idle
        slow = false
    }

    private func ask(_ kind: Kind) {
        id = henge_share_power(kind == .restart ? 1 : 0)
        if id != 0 {
            phase = .asking(kind)
            since = Date()
        } else {
            pressPowerButton(kind, why: "Henge Tools can't do it here")
        }
    }

    private func pressPowerButton(_ kind: Kind, why: String) {
        HengeLog.log("power", "pressing the Mac's power button: \(why)")
        id = 0
        henge_vm_power_button()
        phase = .powerButton(kind)
        since = Date()
    }

    private func startTimer() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
    }

    private func tick() {
        guard henge_vm_state() == 1 else { return } // the session sees it stop, and resets this
        let waited = Date().timeIntervalSince(since)
        switch phase {
        case .idle:
            timer?.invalidate()
            timer = nil
        case let .resuming(kind):
            let pause = MacPause.shared
            if pause.phase == .running {
                ask(kind)
            } else if waited > Self.resumeWait && !slow {
                // Still waiting (it may yet go on), and Force Shut Down is offered meanwhile.
                HengeLog.log("power", "the Mac hasn't gone on in \(Int(Self.resumeWait)) s: macOS isn't asked yet")
                slow = true
            } else if pause.isPaused {
                pause.resume() // still held (the resume asked for didn't take): asked again
            }
        case let .asking(kind):
            switch henge_share_power_answer(id) {
            case 1: phase = .underWay(kind); since = Date(); HengeLog.log("power", "macOS is quitting apps to \(kind.verb)")
            case 3: phase = .permission(kind); since = Date(); HengeLog.log("power", "macOS asks on its screen to allow Henge Tools")
            case 2: cancelled(kind)
            case 0: pressPowerButton(kind, why: "Henge Tools couldn't ask macOS")
            default: if waited > Self.answerWait { pressPowerButton(kind, why: "Henge Tools didn't answer") }
            }
        case let .permission(kind):
            switch henge_share_power_answer(id) {
            case 1: phase = .underWay(kind); since = Date(); HengeLog.log("power", "allowed: macOS is quitting apps to \(kind.verb)")
            case 2: cancelled(kind)
            case 0: pressPowerButton(kind, why: "Henge Tools wasn't allowed")
            default: break // the person answers on the Mac's screen, as long as they like
            }
        case let .underWay(kind):
            if henge_share_power_answer(id) == 2 { cancelled(kind); return }
            if waited > Self.slowAfter && !slow { slow = true; HengeLog.log("power", "macOS hasn't finished after \(Int(Self.slowAfter)) s") }
        case .powerButton:
            if waited > Self.slowAfter && !slow { slow = true }
        }
    }

    private func cancelled(_ kind: Kind) {
        HengeLog.log("power", "an app on the Mac stopped the \(kind == .restart ? "restart" : "shutdown")")
        end("macOS didn't \(kind.verb): an app stopped it, usually to ask about unsaved changes. The Mac is still running.")
    }

    private func end(_ text: String) {
        phase = .idle
        asked = nil
        id = 0
        slow = false
        timer?.invalidate()
        timer = nil
        say(text)
    }

    private func say(_ text: String?) {
        noticeTimer?.invalidate()
        notice = text
        guard let text else { return }
        if UIApplication.shared.applicationState == .active {
            UIAccessibility.post(notification: .announcement, argument: text)
        }
        noticeTimer = Timer.scheduledTimer(withTimeInterval: 9, repeats: false) { [weak self] _ in
            MainActor.assumeIsolated { if self?.notice == text { self?.notice = nil } }
        }
    }

    /// What the banner says while it is under way.
    var title: String {
        switch phase {
        case .idle: return ""
        case let .resuming(kind), let .asking(kind), let .underWay(kind): return "\(kind.doing)…"
        case let .permission(kind): return "Allow Henge Tools to \(kind.verb) the Mac"
        case let .powerButton(kind): return "Choose \(kind == .restart ? "Restart" : "Shut Down") on the Mac"
        }
    }

    var detail: String {
        switch phase {
        case .idle: return ""
        case .resuming: return slow ? "The Mac hasn't gone on yet. Force Shut Down turns it off now; anything not saved is lost."
                                    : "The Mac goes on first, so its apps can save."
        case .asking: return "Asking macOS."
        case .permission: return "macOS asks on the Mac's screen. Choose Allow (only this once), and next time it just happens."
        case .underWay: return slow ? "An app may be waiting for an answer on the Mac's screen. Force Shut Down loses unsaved work."
                                    : "Apps are quitting. If one asks about unsaved changes, answer it on the Mac."
        case let .powerButton(kind):
            return "macOS asks what to do. Choose \(kind == .restart ? "Restart" : "Shut Down") there; apps can save first."
        }
    }
}

/// The word at the top of the screen while macOS shuts down or restarts, and after it didn't. It takes no room from the
/// Mac and lets touches through around it: the person may need to answer an app's question on the Mac meanwhile.
struct PowerBanner: View {
    @ObservedObject var power: MacPower
    let force: () -> Void

    var body: some View {
        if power.active {
            HStack(alignment: .center, spacing: 12) {
                if case .permission = power.phase {
                    Image(systemName: "hand.raised.fill").font(.system(size: 18)).foregroundStyle(Desk.accentLight)
                } else if case .powerButton = power.phase {
                    Image(systemName: "power").font(.system(size: 18)).foregroundStyle(Desk.accentLight)
                } else {
                    ProgressView().tint(.white)
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text(power.title).font(.system(size: 15, weight: .semibold))
                    Text(power.detail).font(.system(size: 13)).foregroundStyle(.white.opacity(0.75))
                        .fixedSize(horizontal: false, vertical: true)
                    // Offered from the start: a stuck Mac may never answer, and the person shouldn't have to wait
                    // to find out it's there. `slow` only changes what the banner says about it.
                    Button("Force Shut Down", role: .destructive, action: force)
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(Desk.bad)
                        .padding(.top, 4)
                }
                Spacer(minLength: 0)
                Button { power.hide() } label: {
                    Image(systemName: "xmark").font(.system(size: 13, weight: .bold)).foregroundStyle(.white.opacity(0.7))
                        .frame(width: 30, height: 30).contentShape(Rectangle())
                }
                .accessibilityLabel("Stop waiting")
                .accessibilityHint("The Mac goes on as macOS decides; Henge stops showing this")
            }
            .foregroundStyle(.white)
            .padding(.leading, 18).padding(.trailing, 8).padding(.vertical, 11)
            .frame(maxWidth: 460, alignment: .leading)
            .fixedSize(horizontal: false, vertical: true)
            .deskGlass(in: RoundedRectangle(cornerRadius: 20, style: .continuous))
            .transition(.opacity.combined(with: .move(edge: .top)))
        } else if let notice = power.notice {
            Label(notice, systemImage: "exclamationmark.triangle.fill")
                .font(.system(size: 15, weight: .medium))
                .foregroundStyle(.white)
                .padding(.horizontal, 18).padding(.vertical, 11)
                .frame(maxWidth: 460, alignment: .leading)
                .fixedSize(horizontal: false, vertical: true)
                .deskGlass(in: RoundedRectangle(cornerRadius: 20, style: .continuous))
                .allowsHitTesting(false)
                .transition(.opacity.combined(with: .move(edge: .top)))
        }
    }
}
