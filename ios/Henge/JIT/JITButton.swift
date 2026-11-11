// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

/// The button for anything that needs JIT (Start, Install). There is no separate "Turn On JIT" step: with JIT off,
/// a tap turns it on and the action follows by itself. While JIT turns on, the button shows what is happening and
/// takes no more taps.
struct NeedsJITButton: View {
    let title: String
    var systemImage: String?
    var enabled = true
    /// Asked first: true when it has taken the tap over, and JIT is not turned on here. Henge restarting for the next
    /// Mac does this: JIT belongs to one launch of Henge, so the new one turns it on.
    var takeOver: (() -> Bool)?
    let action: () -> Void
    @ObservedObject private var jit = JITCoordinator.shared

    var body: some View {
        VStack(spacing: 10) {
            Button {
                if takeOver?() == true { return }
                jit.turnOn(then: action)
            } label: {
                HStack(spacing: 10) {
                    if jit.busy {
                        ProgressView().tint(.white)
                    } else if let systemImage {
                        Image(systemName: systemImage)
                    }
                    Text(jit.busy ? (jit.status ?? "Turning on JIT…") : title)
                        .lineLimit(1)
                        .minimumScaleFactor(0.7)
                        .contentTransition(.opacity)
                }
                .padding(.horizontal, 18)
                .animation(.easeInOut(duration: 0.2), value: jit.busy)
                .animation(.easeInOut(duration: 0.2), value: jit.status)
            }
            .buttonStyle(DeskButtonStyle(enabled: enabled && !jit.busy))
            .disabled(!enabled || jit.busy)
            .accessibilityHint(JITBootstrap.ready ? "" : "Turns on JIT first.")
            if let error = jit.error, !jit.busy {
                Label(error, systemImage: "exclamationmark.triangle.fill")
                    .font(.system(size: 13))
                    .foregroundStyle(Desk.bad)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 5)
                    .transition(.opacity)
            }
        }
        .animation(.easeInOut(duration: 0.2), value: jit.error)
    }
}

/// The JIT state as a capsule: on, turning on, or off.
struct JITBadge: View {
    let ready: Bool
    @ObservedObject private var jit = JITCoordinator.shared

    var body: some View {
        let tint = ready ? Desk.good : jit.busy ? Desk.accentLight : Desk.warn
        HStack(spacing: 5) {
            if jit.busy && !ready { ProgressView().controlSize(.mini).tint(tint) }
            Text(ready ? "JIT on" : jit.busy ? "Turning on" : "JIT off")
        }
        .font(.system(size: 13, weight: .semibold))
        .foregroundStyle(tint)
        .padding(.horizontal, 11)
        .padding(.vertical, 5)
        .background(tint.opacity(0.13), in: Capsule())
        .animation(.easeInOut(duration: 0.2), value: jit.busy)
        .animation(.easeInOut(duration: 0.2), value: ready)
    }
}
