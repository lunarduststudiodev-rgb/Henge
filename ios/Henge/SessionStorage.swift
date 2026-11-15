import Foundation

/// Measures and removes suspend, auto-save, and temporary guest memory files on device storage without touching
/// any Mac's virtual disk (`disk.img`) or user files.
enum SessionStorage {
    /// Bytes on this device that removing `macID`'s suspend, auto-save, and temporary memory files would free.
    nonisolated static func bytes(for macID: UUID) -> Int64 {
        let targets = [
            MacSuspend.folder(macID),
            MacSuspend.runningToolsURL(macID),
            MacAutoSave.folder(macID),
            MacAutoSave.newFolder(macID),
            GuestMemory.runFolder(macID),
            Machines.directory(for: macID).appendingPathComponent("ram.tmp")
        ]
        return targets.reduce(0) { $0 + privateBytes(at: $1) }
    }

    /// Total bytes across all Macs and any orphaned suspend/auto-save folders that clearing session data would free.
    nonisolated static func totalBytes() -> Int64 {
        var total = privateBytes(at: MacSuspend.root) + privateBytes(at: MacAutoSave.root)
        let fm = FileManager.default
        if let dirs = try? fm.contentsOfDirectory(at: Machines.root, includingPropertiesForKeys: nil) {
            for dir in dirs {
                total += privateBytes(at: dir.appendingPathComponent("ram.run"))
                total += privateBytes(at: dir.appendingPathComponent("ram.tmp"))
            }
        }
        return total
    }

    /// Deletes `macID`'s suspended state, auto-save, and temporary memory files when no VM is running.
    nonisolated static func clear(for macID: UUID) {
        guard henge_vm_state() != 1 else { return }
        MacSuspend.forget(macID)
        MacSuspend.dropRunningTools(macID)
        MacAutoSave.forgetNow(macID, "removed by user")
        let fm = FileManager.default
        try? fm.removeItem(at: GuestMemory.runFolder(macID))
        try? fm.removeItem(at: Machines.directory(for: macID).appendingPathComponent("ram.tmp"))
        MacSuspend.changed()
        HengeLog.log("storage", "removed suspend and session data for \(macID.uuidString)")
    }

    /// Deletes all suspended states, auto-saves, orphaned session folders, and temporary memory files when no VM is running.
    nonisolated static func clearAll() {
        guard henge_vm_state() != 1 else { return }
        let fm = FileManager.default
        for root in [MacSuspend.root, MacAutoSave.root] {
            guard let items = try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: nil) else { continue }
            for item in items {
                let snap = item.appendingPathComponent("Mac.hvsnap")
                if fm.fileExists(atPath: snap.path) {
                    _ = henge_vm_discard_saved(snap.path)
                }
                try? fm.removeItem(at: item)
            }
        }
        if let dirs = try? fm.contentsOfDirectory(at: Machines.root, includingPropertiesForKeys: nil) {
            for dir in dirs {
                try? fm.removeItem(at: dir.appendingPathComponent("ram.run"))
                try? fm.removeItem(at: dir.appendingPathComponent("ram.tmp"))
            }
        }
        MacSuspend.changed()
        HengeLog.log("storage", "removed all suspend and session data")
    }

    private nonisolated static func privateBytes(at url: URL) -> Int64 {
        let fm = FileManager.default
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: url.path, isDirectory: &isDir) else { return 0 }
        guard isDir.boolValue else { return MacAutoSave.privateSize(url.path) }
        guard let walk = fm.enumerator(atPath: url.path) else { return 0 }
        var total: Int64 = 0
        for case let name as String in walk {
            total += MacAutoSave.privateSize(url.appendingPathComponent(name).path)
        }
        return total
    }
}
