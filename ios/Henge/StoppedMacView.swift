// SPDX-License-Identifier: GPL-2.0-or-later
// Based on PR #50 by liukaixiang817.
import SwiftUI

/// What the session shows once the Mac has stopped by itself: macOS shut it down (or restarted it, while Henge gets
/// ready to start it again), it panicked, or it hit an error. Start Mac starts it again (in a fresh Henge: Relaunch);
/// Close goes back to the list of Macs.
struct StoppedMacView: View {
    /// henge_vm_halted_by: 1 shut down, 2 restarted, 3 stopped by Henge or an error, 4 panic.
    let haltedBy: Int32
    /// henge_vm_exit_reason: "halt" unless an error stopped it.
    let reason: String
    /// Henge is restarting itself to start the Mac again (macOS asked to restart).
    let restarting: Bool
    let start: () -> Void
    let close: () -> Void

    private var title: String {
        switch haltedBy {
        case 1: return "This Mac has shut down"
        case 2: return restarting ? "This Mac is restarting" : "This Mac has restarted"
        case 4: return "This Mac stopped after a problem in macOS"
        default: return reason == "halt" ? "This Mac has stopped" : "This Mac stopped unexpectedly"
        }
    }

    private var detail: String {
        switch haltedBy {
        case 1: return "Everything it wrote is saved. Start it again, or close it."
        case 2: return restarting ? "Henge starts it again in a moment." : "Start it again, or close it."
        case 4: return "macOS reported a panic. Start it again, or close it."
        default:
            return reason == "halt" ? "Start it again, or close it."
                : "It stopped with an error (\(reason)). Start it again, or close it."
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Label(title, systemImage: haltedBy == 2 ? "arrow.clockwise" : "power")
                .font(.system(size: 17, weight: .semibold))
            Text(detail).font(.system(size: 14)).foregroundStyle(Desk.dim)
            if !restarting {
                HStack(spacing: 22) {
                    Button("Start Mac", action: start).foregroundStyle(Desk.accentLight)
                    Button("Close", action: close).foregroundStyle(Desk.dim)
                }
                .font(.system(size: 15, weight: .semibold))
                .padding(.top, 4)
            } else {
                ProgressView().tint(.white).padding(.top, 4)
            }
        }
        .foregroundStyle(.white)
        .padding(18)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Desk.raised, in: RoundedRectangle(cornerRadius: 22, style: .continuous))
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("stopped-mac")
    }
}
