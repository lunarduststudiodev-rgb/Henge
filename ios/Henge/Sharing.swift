import SwiftUI
import UIKit

/// What a running Mac shares with this device: the Shared folder in Files, and the clipboard. The Mac mounts
/// the folder and follows the clipboard through a small helper (host/share.c, tools/henge-agent.m).
enum Sharing {
    static var folder: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("Shared", isDirectory: true)
    }

    static let setUpCommand = "curl -s http://10.0.2.4/.henge/install | sh"

    /// Before a Mac starts.
    static func prepare(_ mac: VirtualMac) {
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        // A Unix socket's path can't be long, so the short temporary folder.
        let socket = (NSTemporaryDirectory() as NSString).appendingPathComponent("s.sock")
        // This Mac's pinned system key, in its own folder: it goes with the Mac, and stays when macOS in it is put in again.
        henge_vm_system_pin(mac.directory.appendingPathComponent("system-key-pin").path)
        henge_vm_share(folder.path, socket, Bundle.main.path(forResource: "henge-agent", ofType: "gz"))
        henge_vm_bench_video(Bundle.main.path(forResource: "henge-bench", ofType: "mp4")) // the benchmark's (Autopilot "bench")
        // The same folder as macOS's own shared-folder device: it appears in Finder by itself, under My Shared
        // Files, with nothing to install.
        henge_vm_folders(UIDevice.current.model, folder.path, nil)
        // The Henge Tools disk, ready to plug in as a USB drive while the Mac runs (MacDrives): as it starts if
        // Henge Tools wasn't running there last time, and whenever the installer is wanted. Henge Tools ejects it
        // once it isn't needed.
        toolsDisk = unpackToolsDisk()
        if toolsDisk == nil { HengeLog.log("tools", "the Henge Tools disk couldn't be unpacked: no disk") }
    }

    /// The Henge Tools disk, unpacked for this session; nil if it couldn't be.
    private(set) static var toolsDisk: URL?

    /// The Henge Tools disk, fresh from this build each time: a 16 MB raw disk image, packed in the app gzipped.
    private static func unpackToolsDisk() -> URL? {
        guard let packed = Bundle.main.path(forResource: "henge-tools.img", ofType: "gz"),
              let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
        else { return nil }
        try? FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
        try? FileManager.default.removeItem(at: support.appendingPathComponent("Henge Tools", isDirectory: true)) // the folder before
        let disk = support.appendingPathComponent("Henge Tools.img")
        return henge_unpack(packed, disk.path) == 0 ? disk : nil
    }

    // Whether a Mac had this Henge's Tools, for the log. An older helper updates itself, so the Tools are current
    // once any helper of this version has linked, until a session runs without one.
    private static func key(_ what: String, _ id: UUID) -> String { "henge.tools.\(what).\(id.uuidString)" }

    static func toolsAreCurrent(for id: UUID) -> Bool { UserDefaults.standard.bool(forKey: key("current", id)) }

    static func noteTools(current: Bool, for id: UUID) {
        guard toolsAreCurrent(for: id) != current else { return }
        UserDefaults.standard.set(current, forKey: key("current", id))
        HengeLog.log("tools", current ? "Henge Tools is installed and current" : "Henge Tools isn't running on this Mac")
    }

    /// Opens Install Henge Tools on the Mac: to install, update or remove Henge Tools. The Henge Tools disk plugs
    /// in as a USB drive. With Henge Tools running, it opens the installer itself as the disk arrives (it's told
    /// first, so it doesn't eject the disk); without, Henge opens it from the Mac's keyboard once macOS has it.
    @MainActor static func openToolsInstaller() async {
        if henge_share_can_eject() != 0 { // an out-of-date Henge Tools opens it too: the way to update by hand
            HengeLog.log("tools", "asking Henge Tools to open its installer")
            henge_share_tools_open()
            MacDrives.shared.plugTools()
        } else {
            let wasIn = MacDrives.shared.toolsPort > 0
            guard MacDrives.shared.plugTools() else {
                HengeLog.log("tools", "the Henge Tools disk couldn't be plugged in")
                return
            }
            if !wasIn { await MacDrives.shared.waitForTools(seconds: 20) }
            await openToolsInstallerFromKeyboard()
        }
    }

    /// Opens Install Henge Tools on the Mac from its keyboard, the way a person could: a Finder window (⌥⌘Space),
    /// Computer (⇧⌘C), the Henge Tools disk picked by typing its name and opened (⌘↓), then the installer the
    /// same way. Spotlight can't help: macOS doesn't index a read-only disk. Nothing here submits or closes
    /// anything, so if Finder didn't come forward the worst is a few letters typed where the keyboard was.
    /// The letters are the same on the common Latin layouts (AZERTY and QWERTZ too). Tried on macOS 27.
    @MainActor static func openToolsInstallerFromKeyboard() async {
        HengeLog.log("tools", "opening the installer from the Mac's keyboard")
        func pause(_ seconds: Double) async { try? await Task.sleep(nanoseconds: UInt64(seconds * 1e9)) }
        // Each one whole, between anything else Henge is typing (TypingQueue): dictation's letters typed while ⌘⌥
        // were down would be shortcuts.
        func shortcut(_ modifiers: [UInt16], _ key: UInt16) async {
            await TypingQueue.shared.type(shortcut: modifiers, key, by: .other)
        }
        await shortcut([Keys.command, Keys.option], 57) // space: a Finder search window, Finder in front
        await pause(2.0)
        await shortcut([Keys.command, Keys.shift], 46) // C: Computer, the Mac's disks
        await pause(2.0)
        await Keys.typeSlowly("henge")
        await pause(1.0)
        await shortcut([Keys.command], Keys.down) // open the disk
        await pause(2.0)
        await Keys.typeSlowly("inst")
        await pause(1.0)
        await shortcut([Keys.command], Keys.down) // open the installer
    }

    static func neverAskAboutTools(for id: UUID) -> Bool { UserDefaults.standard.bool(forKey: key("neverAsk", id)) }

    static func setNeverAskAboutTools(_ never: Bool, for id: UUID) { UserDefaults.standard.set(never, forKey: key("neverAsk", id)) }

    /// The helper asks the server for news at most 25 s apart.
    static var helperRunning: Bool {
        let seen = henge_share_agent_seen()
        return seen >= 0 && seen < 40
    }

    /// What is copied on the Mac is copied here too. The other way needs the system's paste button: iOS
    /// asks for permission whenever an app reads the clipboard on its own.
    static func followMacClipboard() async {
        while !Task.isCancelled {
            if let text = henge_share_take_clipboard() {
                UIPasteboard.general.string = String(cString: text)
                free(text)
            }
            try? await Task.sleep(nanoseconds: 500_000_000)
        }
    }

    /// Text from this device, pasted at the Mac's cursor. Without the helper it is typed, as far as the keyboard can.
    static func paste(_ text: String) {
        Task { @MainActor in
            if helperRunning {
                henge_share_set_clipboard(text)
                try? await Task.sleep(nanoseconds: 800_000_000) // for the helper to put it on the Mac's clipboard
                if let v = Keys.code("v") { TypingQueue.shared.add(shortcut: [Keys.command], v, by: .other) }
            } else {
                await Keys.typeSlowly(text)
            }
        }
    }

    /// Types the set up command at the Mac's cursor, and stops there: the person using the Mac presses Return.
    /// Henge can't see what has the keyboard, so it must not submit anything by itself. Opening Terminal for them
    /// through Spotlight was tried: when Spotlight was slow the text went to a web search, and it could as easily
    /// have gone to a chat.
    static func typeSetUpCommand() async {
        await Keys.typeSlowly(setUpCommand)
    }
}
