import SwiftUI

/// iPadOS 17 and the Mac's graphics (GitHub #44, #12, #24).
///
/// macOS 15 and later send their GPU code in a newer library format than iPadOS 17's Metal reads. Henge relabels it for
/// the older Metal and tries again (devices/pvg_metallib.h), but iPadOS 17 may still refuse some of it, and then the
/// Mac's screen stays black after the Apple logo while macOS itself runs (sound, pointer). iPadOS 18 reads it as sent.
enum GraphicsSupport {
    /// iPadOS (or iOS) older than 18: the Mac's graphics may not work here.
    static var olderSystem: Bool { ProcessInfo.processInfo.operatingSystemVersion.majorVersion < 18 }

    static var systemName: String {
        let v = ProcessInfo.processInfo.operatingSystemVersion
        let name = UIDevice.current.userInterfaceIdiom == .pad ? "iPadOS" : "iOS"
        return v.patchVersion > 0 ? "\(name) \(v.majorVersion).\(v.minorVersion).\(v.patchVersion)" : "\(name) \(v.majorVersion).\(v.minorVersion)"
    }

    static var updateName: String { UIDevice.current.userInterfaceIdiom == .pad ? "iPadOS 18" : "iOS 18" }

    /// Before installing: said plainly, without stopping anyone from trying.
    static var installText: String {
        "This \(UIDevice.current.userInterfaceIdiom == .pad ? "iPad" : "iPhone") has \(systemName). macOS can install and run, "
            + "but \(systemName) may not be able to draw the Mac's screen: it can stay black after the Apple logo. "
            + "Henge adapts macOS's graphics for it where it can. For the Mac's screen to work fully, update to \(updateName) or later."
    }

    /// In a session, once this system's Metal has refused GPU code macOS sent.
    static var sessionText: String {
        "\(systemName) can't run some of the graphics code macOS uses, so the Mac's screen may stay black or have parts missing. "
            + "macOS itself is running. To see the Mac's screen, update to \(updateName) or later."
    }
}

/// The note on the install screen, on iPadOS 17.
struct GraphicsSupportInstallNote: View {
    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: "exclamationmark.triangle.fill")
                .font(.system(size: 17, weight: .semibold))
                .foregroundStyle(.yellow)
            Text(GraphicsSupport.installText)
                .font(.system(size: 15))
                .foregroundStyle(Desk.text)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 17)
        .padding(.vertical, 14)
        .deskCard()
        .padding(.top, 12)
        .accessibilityElement(children: .combine)
    }
}

/// The note over the Mac's screen in a session, until it is closed.
struct GraphicsSupportSessionNote: View {
    var close: () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: "display.trianglebadge.exclamationmark")
                .font(.system(size: 20, weight: .semibold))
                .foregroundStyle(.yellow)
            VStack(alignment: .leading, spacing: 6) {
                Text("The Mac's screen can't be drawn here")
                    .font(.system(size: 16, weight: .semibold))
                Text(GraphicsSupport.sessionText)
                    .font(.system(size: 15))
                    .fixedSize(horizontal: false, vertical: true)
            }
            .foregroundStyle(.white)
            Button(action: close) {
                Image(systemName: "xmark")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(.white.opacity(0.8))
                    .frame(width: 32, height: 32)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Close")
        }
        .frame(maxWidth: 520, alignment: .leading)
        .padding(.leading, 18).padding(.trailing, 8).padding(.vertical, 14)
        .deskGlass(in: RoundedRectangle(cornerRadius: 22, style: .continuous))
    }
}

/// The Mac's graphics stopped: the iPad's GPU couldn't finish work macOS sent it (a hang its watchdog ended, or a
/// fault), and Henge stopped running the Mac's graphics so it can't freeze the iPad again and again (the engine's
/// GPU breaker, devices/pvg_metal.m). The Mac itself keeps running; its screen just stops changing. Try Again runs its
/// graphics again: what was missed comes back as macOS draws again. A run allows it twice, for the same work may
/// stall the GPU again; after that only restarting the Mac (a new run) brings them back.
/// state: henge_vm_gpu_state (1 stopped, 2 stopped and the GPU still busy with the Mac's last work, 3 for this run;
/// 3 also in a build made to run none of the Mac's GPU work, HV_PVG_ENCODE_ONLY, which no app build ships).
struct GraphicsStoppedNote: View {
    var state: Int32
    var tryAgain: () -> Void
    var close: () -> Void

    private var text: String {
        if state == 3 {
            return "The iPad's graphics chip kept failing on work from the Mac, so Henge has stopped the Mac's graphics for "
                + "the rest of this session to keep your iPad responsive. The Mac is still running, but its screen won't "
                + "change. Restart the Mac to bring its graphics back."
        }
        return "The iPad's graphics chip couldn't finish work from the Mac, so Henge paused the Mac's graphics to keep your "
            + "iPad responsive. The Mac is still running, but its screen won't change until you try again. If it stops "
            + "again, restart the Mac."
    }

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: "display.trianglebadge.exclamationmark")
                .font(.system(size: 20, weight: .semibold))
                .foregroundStyle(.yellow)
            VStack(alignment: .leading, spacing: 10) {
                Text("The Mac's graphics stopped")
                    .font(.system(size: 16, weight: .semibold))
                Text(text)
                    .font(.system(size: 15))
                    .fixedSize(horizontal: false, vertical: true)
                if state != 3 {
                    Button(action: tryAgain) {
                        Text(state == 2 ? "Waiting for the graphics chip…" : "Try Again")
                            .font(.system(size: 15, weight: .semibold))
                            .padding(.horizontal, 14).padding(.vertical, 8)
                            .background(.white.opacity(state == 2 ? 0.08 : 0.18), in: Capsule())
                    }
                    .buttonStyle(.plain)
                    .disabled(state == 2)
                }
            }
            .foregroundStyle(.white)
            // Not while Try Again can be pressed: it holds the only way back, and the screen behind it isn't changing.
            // While the GPU is still busy (2) it can be closed, as that may last; it comes back once Try Again can be.
            if state != 1 {
                Button(action: close) {
                    Image(systemName: "xmark")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(.white.opacity(0.8))
                        .frame(width: 32, height: 32)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Close")
            }
        }
        .frame(maxWidth: 520, alignment: .leading)
        .padding(.leading, 18).padding(.trailing, 8).padding(.vertical, 14)
        .deskGlass(in: RoundedRectangle(cornerRadius: 22, style: .continuous))
    }
}
