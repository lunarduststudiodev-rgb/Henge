// SPDX-License-Identifier: GPL-2.0-or-later
import AppIntents
import SwiftUI

/// What a shortcut asked Henge to do when it opened; RootView carries it out.
@MainActor final class LaunchRequests: ObservableObject {
    static let shared = LaunchRequests()
    @Published var startLastMac = false
    /// Settings' "Open straight into my Mac": each time Henge opens fresh, it starts the Mac used last, as the shortcut does.
    static let openIntoMacKey = "henge.openIntoMac"
}

/// "Turn on JIT in Henge": from Siri, Spotlight, the Shortcuts app or the Action button.
struct TurnOnJITIntent: AppIntent {
    static var title: LocalizedStringResource { "Turn On JIT" }
    static var description: IntentDescription { IntentDescription("Opens Henge and turns on JIT.") }
    static var openAppWhenRun: Bool { true }

    @MainActor func perform() async throws -> some IntentResult {
        HengeLog.log("jit", "shortcut: turn on JIT")
        JITCoordinator.shared.turnOn()
        return .result()
    }
}

/// "Start my Mac in Henge": opens Henge, turns on JIT if needed and starts the Mac used last.
struct StartMacIntent: AppIntent {
    static var title: LocalizedStringResource { "Start Mac" }
    static var description: IntentDescription { IntentDescription("Opens Henge, turns on JIT if needed and starts the Mac you used last.") }
    static var openAppWhenRun: Bool { true }

    @MainActor func perform() async throws -> some IntentResult {
        HengeLog.log("jit", "shortcut: start the last Mac")
        LaunchRequests.shared.startLastMac = true
        return .result()
    }
}

struct HengeShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(intent: StartMacIntent(),
                    phrases: ["Start my Mac in \(.applicationName)", "Start \(.applicationName)"],
                    shortTitle: "Start Mac", systemImageName: "play.fill")
        AppShortcut(intent: TurnOnJITIntent(),
                    phrases: ["Turn on JIT in \(.applicationName)", "Enable JIT in \(.applicationName)"],
                    shortTitle: "Turn On JIT", systemImageName: "bolt.fill")
    }
}
