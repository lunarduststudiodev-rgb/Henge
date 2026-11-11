import SwiftUI

/// Tells the person about Henge Tools while a Mac runs, as other virtual machine apps do about theirs. Henge can't
/// see whether the Mac is at its desktop or still in Setup Assistant, so it never puts a card over the Mac to ask:
/// a small "Set Up Henge Tools" button waits in a corner until the person is ready, and opens the card when tapped.
/// After installing, the card asks for the Accessibility permission (the person is at the desktop by then), then
/// says it's done and goes. Touches outside the button and card still reach the Mac.
struct ToolsGuide: View {
    let mac: VirtualMac
    /// Where the Mac's screen is: the card sits in its top-left corner, below the menu bar, away from the disks.
    let screen: CGRect

    private enum Stage: Hashable { case none, install, permission, done }

    @State private var stage = Stage.none
    @State private var expanded = false // the card, rather than the small button

    var body: some View {
        // Clear and out of the way of touches, so it is there (and follows along) with or without a card.
        Color.clear
            .allowsHitTesting(false)
            .overlay(alignment: .topLeading) {
                Group {
                    if stage == .done || (stage != .none && expanded) {
                        card.transition(.scale(scale: 0.85, anchor: .topLeading).combined(with: .opacity))
                    } else if stage != .none {
                        chip.transition(.scale(scale: 0.85, anchor: .topLeading).combined(with: .opacity))
                    }
                }
                .padding(.leading, max(screen.minX + 16, 12))
                .padding(.top, max(screen.minY + 40, 12))
            }
            .animation(.spring(response: 0.4, dampingFraction: 0.85), value: stage)
            .animation(.spring(response: 0.4, dampingFraction: 0.85), value: expanded)
            .task { await follow() }
    }

    // MARK: The small button

    /// Waits in the corner until the person is ready: tapping it opens the card.
    private var chip: some View {
        Button { expanded = true } label: {
            HStack(spacing: 8) {
                Image(systemName: symbol).font(.system(size: 14, weight: .semibold)).foregroundStyle(Color.accentColor)
                Text(stage == .permission ? "Allow Henge Tools" : "Set Up Henge Tools").font(.subheadline.weight(.semibold))
            }
            .padding(.horizontal, 14)
            .frame(height: 36)
            .background(.regularMaterial, in: Capsule())
            .overlay(Capsule().strokeBorder(.white.opacity(0.12)))
            .shadow(color: .black.opacity(0.3), radius: 10, y: 3)
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .environment(\.colorScheme, .dark)
        .contextMenu {
            if stage == .install {
                Button("Don't Show for This Mac", systemImage: "eye.slash") { neverAsk() }
            }
        }
        .accessibilityHint("Shows how to install Henge Tools on this Mac")
    }

    private func neverAsk() {
        Sharing.setNeverAskAboutTools(true, for: mac.id)
        HengeLog.log("tools", "asked not to show Henge Tools for this Mac")
        stage = .none
    }

    // MARK: The card

    @State private var opening = false // typing on the Mac to open the installer
    @State private var opened = false // did that once: what to do in the installer, and what if it didn't open

    @ViewBuilder private var card: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .center, spacing: 12) {
                Image(systemName: symbol)
                    .font(.system(size: 22, weight: .semibold))
                    .foregroundStyle(stage == .done ? .green : .accentColor)
                    .frame(width: 30)
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).font(.headline)
                    Text(subtitle).font(.subheadline).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            switch stage {
            case .install:
                if opened {
                    step(1, "Click **Install** in the window that opened on the Mac.")
                    step(2, "Type your Mac's password when macOS asks. It's the one you log in with, and it's asked for once, so Henge Tools can start at the login window.")
                    step(3, "Turn on **henge-agent** when the Mac asks.")
                    note("Nothing opened? Double-click the **Henge Tools** disk on the Mac's desktop, then **Install Henge Tools**.")
                } else {
                    note("Henge opens the installer on the Mac for you. Then click **Install**, type your Mac's password once when macOS asks, and turn on **henge-agent**. It takes a minute.")
                    callout("Do this from the Mac's desktop. Still setting up macOS? Finish that first: this button waits up here until you're ready.")
                }
                actions {
                    Button {
                        opening = true
                        Task {
                            await Sharing.openToolsInstaller()
                            try? await Task.sleep(nanoseconds: 1_500_000_000)
                            opening = false
                            opened = true
                        }
                    } label: {
                        if opening {
                            HStack(spacing: 8) { ProgressView().controlSize(.small); Text("Opening…") }
                        } else {
                            Text(opened ? "Open It Again" : "Open the Installer")
                        }
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(opening)
                }
            case .permission:
                note("Henge Tools is installed. Turn on **henge-agent** in the Accessibility list, so it can see what's under your finger.")
                actions {
                    Button("Open Settings on the Mac") { henge_share_touch_ask() }
                        .buttonStyle(.borderedProminent)
                }
            default:
                EmptyView()
            }
        }
        .padding(16)
        .frame(width: 360, alignment: .leading)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).strokeBorder(.white.opacity(0.12)))
        .shadow(color: .black.opacity(0.35), radius: 18, y: 6)
        .environment(\.colorScheme, .dark)
        .accessibilityElement(children: .contain)
    }

    /// The card's one thing to do, then Not Now, which folds it back into the small button; for installing, a quiet
    /// way to stop showing it for this Mac (Details still has the installer).
    @ViewBuilder private func actions<Primary: View>(@ViewBuilder _ primary: () -> Primary) -> some View {
        HStack(spacing: 10) {
            primary()
            Button("Not Now") { expanded = false }
                .buttonStyle(.bordered)
        }
        .controlSize(.regular)
        .padding(.top, 2)
        if stage == .install {
            Button("Don't show this for this Mac") { neverAsk() }
                .font(.footnote)
                .foregroundStyle(.secondary)
                .buttonStyle(.plain)
                .accessibilityHint("You can still install Henge Tools from Details")
        }
    }

    /// Something to know before pressing the button.
    private func callout(_ text: LocalizedStringKey) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Image(systemName: "info.circle").foregroundStyle(Desk.warn)
            Text(text).font(.subheadline)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Desk.warn.opacity(0.12), in: RoundedRectangle(cornerRadius: 10, style: .continuous))
    }

    private func note(_ text: LocalizedStringKey) -> some View {
        Text(text).font(.subheadline).foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }

    private func step(_ n: Int, _ text: LocalizedStringKey) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Text("\(n)")
                .font(.caption.weight(.bold))
                .foregroundStyle(.white)
                .frame(width: 20, height: 20)
                .background(Circle().fill(Color.accentColor))
            Text(text).font(.subheadline)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var symbol: String {
        switch stage {
        case .install: return "wrench.and.screwdriver"
        case .permission: return "lock.shield"
        default: return "checkmark.circle.fill"
        }
    }

    private var title: String {
        switch stage {
        case .install: return "Install Henge Tools"
        case .permission: return "Allow Henge Tools"
        default: return "Henge Tools Is On"
        }
    }

    private var subtitle: String {
        switch stage {
        case .install: return "Windows drag at once, pages scroll smoothly, and the clipboard is shared with this \(Device.name)."
        case .permission: return "One last step: macOS needs your OK before Henge Tools can make touch feel native."
        default: return "Touch, the clipboard and your files now work as they should."
        }
    }

    // MARK: Following the helper

    private func follow() async {
        var asked = false // the card asked for something this session, so it says when it's done
        var watch = ToolsWatch()
        let began = Date()
        while !Task.isCancelled {
            let linked = henge_share_touch_linked() != 0
            let current = henge_share_agent_current() != 0
            if linked && current { Sharing.noteTools(current: true, for: mac.id) }
            let next: Stage
            switch watch.look(at: Date().timeIntervalSince(began), keys: henge_vm_keys_pressed() > 0, clicks: henge_vm_clicks() > 0,
                              linked: linked, current: current, ready: henge_share_touch_ready() != 0) {
            case .ready: next = asked ? .done : .none
            case .permission: next = .permission
            case .missing:
                // No helper by now: removed, or never installed. Install plugs the Henge Tools disk in and opens its
                // installer, unless the disk couldn't be unpacked: then there's nothing to offer.
                Sharing.noteTools(current: false, for: mac.id)
                next = Sharing.neverAskAboutTools(for: mac.id) || Sharing.toolsDisk == nil ? .none : .install
            case .wait: next = .none
            }
            if next != stage {
                if next == .install || next == .permission { asked = true }
                // To install, the small button: the Mac may still be in Setup Assistant. The permission comes
                // right after installing, at the desktop, so its card opens by itself.
                expanded = next == .permission
                if next == .install { HengeLog.log("tools", "the Set Up Henge Tools button is showing") }
                if next == .permission { HengeLog.log("tools", "the card asks for the Accessibility permission") }
                stage = next
                if next == .done {
                    HengeLog.log("tools", "Henge Tools is on")
                    try? await Task.sleep(nanoseconds: 5_000_000_000)
                    stage = .none
                    asked = false
                    continue
                }
            }
            try? await Task.sleep(nanoseconds: 1_000_000_000)
        }
    }
}

/// What the card makes of the helper, looked at once a second: when to ask to install, and when for the permission.
/// Apart from the view so it can be tried on its own, with times in seconds since the Mac started.
struct ToolsWatch {
    enum Verdict: Equatable { case wait, missing, permission, ready }

    /// The helper starts as someone logs in, and links within half a minute or so. Henge can't see the login
    /// window, so it goes by what the person does: from the first key (a password, most likely), this long; with
    /// no key (a Mac that logs in by itself), this long from the first click or tap. Nothing at all, and there's
    /// no card: a Mac left at its login window once had one over it, asking for what no one could do yet.
    static let missingAfterFirstKey: TimeInterval = 75
    static let missingAfterFirstClick: TimeInterval = 120
    /// The helper asks macOS for the permission as it starts; a moment for the person to answer that first.
    static let permissionAfter: TimeInterval = 20

    private var firstKey: TimeInterval?
    private var firstClick: TimeInterval?
    private var everLinked = false // a helper was here this session: a link that drops is it updating or restarting
    private var untrustedSince: TimeInterval?

    mutating func look(at t: TimeInterval, keys: Bool, clicks: Bool, linked: Bool, current: Bool, ready: Bool) -> Verdict {
        if keys && firstKey == nil { firstKey = t }
        if clicks && firstClick == nil { firstClick = t }
        if linked { everLinked = true }
        if linked && current && !ready { untrustedSince = untrustedSince ?? t } else { untrustedSince = nil }
        if ready { return .ready }
        if let since = untrustedSince, t - since >= Self.permissionAfter { return .permission }
        if everLinked { return .wait } // starting, or an older helper updating itself
        if let key = firstKey { return t - key >= Self.missingAfterFirstKey ? .missing : .wait }
        if let click = firstClick { return t - click >= Self.missingAfterFirstClick ? .missing : .wait }
        return .wait
    }
}
