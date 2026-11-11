import Foundation

/// Auto-save (the autosave design notes): while the Mac runs on the screen, it is saved whole every so often, its
/// disks with it (APFS clones taken at the same instant), and goes on. If Henge then ends without a Suspend (a crash,
/// iOS ending it, a force-quit, a flat battery, an install), starting the Mac offers to carry on from there.
///
/// One per Mac, in Application Support/AutoSaved/<id>: Mac.hvsnap (the engine's snapshot) and its memory copy beside
/// it, Disk.img and Aux.img (the disks as they were then), Henge Tools.img (a hard link, as Suspend keeps) and
/// autosave.json (what the app needs to build the Mac the same way). Written in <id>.new and swapped in whole, so
/// the one before stays usable until the new one is complete.
enum MacAutoSave {
    /// The trial switch: HENGE_AUTOSAVE=1 in Documents/henge-env.txt, with the running Mac's memory in files (a save
    /// clones the memory files rather than writing the memory) and Suspend on in Settings.
    static var on: Bool {
        getenv("HENGE_AUTOSAVE").map { String(cString: $0) == "1" } == true && GuestMemory.inFiles && MacSuspend.enabled
    }

    /// How often, and how long one may wait for a still moment before it is taken anyway.
    static let interval: TimeInterval = 600
    static let overdue: TimeInterval = 600
    /// Kept only with this much free beyond what it holds; dropped below the second.
    static let spareToTake: Int64 = 4 << 30
    static let dropBelow: Int64 = 3 << 30

    private static var root: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("AutoSaved", isDirectory: true)
    }
    static func folder(_ id: UUID) -> URL { root.appendingPathComponent(id.uuidString, isDirectory: true) }
    private static func newFolder(_ id: UUID) -> URL { root.appendingPathComponent(id.uuidString + ".new", isDirectory: true) }
    private static func stateURL(_ dir: URL) -> URL { dir.appendingPathComponent("Mac.hvsnap") }
    private static func diskURL(_ dir: URL) -> URL { dir.appendingPathComponent("Disk.img") }
    private static func auxURL(_ dir: URL) -> URL { dir.appendingPathComponent("Aux.img") }
    private static func infoURL(_ dir: URL) -> URL { dir.appendingPathComponent("autosave.json") }
    private static func toolsURL(_ dir: URL) -> URL { dir.appendingPathComponent("Henge Tools.img") }

    /// The auto-save to go on from, if there is one: its snapshot and its note are both there.
    static func saved(for id: UUID) -> MacSuspend.Saved? {
        let dir = folder(id)
        guard FileManager.default.fileExists(atPath: stateURL(dir).path),
              let data = try? Data(contentsOf: infoURL(dir)),
              let saved = try? JSONDecoder().decode(MacSuspend.Saved.self, from: data), saved.macID == id else { return nil }
        return saved
    }

    static func exists(_ id: UUID) -> Bool { FileManager.default.fileExists(atPath: folder(id).path) }

    /// Where Carry On keeps the Mac's disk as it was when Henge closed, when that is newer than the auto-save's (the
    /// engine's hv_disk_place): nothing the Mac saved is deleted by going back. Taken, the engine numbers the next
    /// ("Disk before Carry On 2.img", …): one is never replaced, and Henge never deletes one by itself (asideDisks).
    static func asideDisk(_ mac: UUID) -> URL { Machines.directory(for: mac).appendingPathComponent("Disk before Carry On.img") }

    /// The Mac that last carried on from its auto-save in this run of Henge (set as it starts, read by MacPause as it
    /// attaches): only then is the auto-save kept the Mac as it was at its start.
    @MainActor static var carriedOn: UUID?

    /// The Mac being started (MacSuspend.boot, from its first step until it runs or doesn't): its disks are not to be
    /// touched from elsewhere meanwhile (the Mac's settings).
    @MainActor static var starting: UUID?

    /// Where the Mac's auxiliary.bin is kept beside each disk Carry On kept aside, named for that disk's Stamp (a
    /// kept disk keeps its size and time): the two go back in together. Small (the Mac's startup settings).
    nonisolated static func keptAux(_ mac: UUID, for disk: Stamp) -> URL {
        Machines.directory(for: mac).appendingPathComponent("Aux kept by Carry On", isDirectory: true)
            .appendingPathComponent("\(disk.size)-\(disk.sec)-\(disk.nsec).bin")
    }

    /// The Mac's auxiliary.bin, cloned to keptAux for its disk as it is now (before a Carry On that will keep it aside,
    /// or before Use This Disk). false if it couldn't be.
    nonisolated static func keepAux(_ mac: VirtualMac, for disk: Stamp) -> Bool {
        let to = keptAux(mac.id, for: disk)
        try? FileManager.default.createDirectory(at: to.deletingLastPathComponent(), withIntermediateDirectories: true)
        if FileManager.default.fileExists(atPath: to.path) { try? FileManager.default.removeItem(at: to) }
        guard clonefile(mac.aux.path, to.path, 0) == 0 else { return false }
        _ = fullSync(to)
        _ = fullSync(to.deletingLastPathComponent())
        return true
    }

    /// Deletions, off the pause queue: a big clone can take a while to delete, and a pause on leaving mustn't wait.
    private static let tidy = DispatchQueue(label: "henge.autosave.tidy", qos: .utility)

    /// Everything kept for the Mac's auto-save (and any half-written one). Out of sight at once (renamed to a
    /// .trash name, and that rename on storage), then deleted on its own queue, the snapshot first. why: for the log.
    static func forget(_ id: UUID, _ why: String) {
        for dir in [folder(id), newFolder(id)] where FileManager.default.fileExists(atPath: dir.path) {
            throwAway(dir)
            if dir == folder(id) { HengeLog.log("autosave", "auto-save deleted: \(why)") }
        }
    }

    /// A folder of the root out of sight now, deleted soon.
    private static func throwAway(_ dir: URL) {
        let trash = root.appendingPathComponent("\(dir.lastPathComponent).\(UUID().uuidString).trash")
        if rename(dir.path, trash.path) == 0 {
            syncFolder(root)
            tidy.async { delete(trash) }
        } else {
            delete(dir) // here and now, as it can
        }
    }

    /// The snapshot first, its deletion on storage before the rest goes (a folder without one is never used).
    private static func delete(_ dir: URL) {
        if henge_vm_discard_saved(stateURL(dir).path) != 0 { HengeLog.log("autosave", "a snapshot couldn't be deleted (\(dir.lastPathComponent))") }
        try? FileManager.default.removeItem(at: dir)
    }

    /// As forget, and deleted before it returns (only this one: nothing else queued is waited for): for one who counts
    /// free storage right after (a Suspend short of room). On the pause queue.
    static func forgetNow(_ id: UUID, _ why: String) {
        for dir in [folder(id), newFolder(id)] where FileManager.default.fileExists(atPath: dir.path) {
            let trash = root.appendingPathComponent("\(dir.lastPathComponent).\(UUID().uuidString).trash")
            if rename(dir.path, trash.path) == 0 {
                syncFolder(root)
                delete(trash)
            } else {
                delete(dir)
            }
            if dir == folder(id) { HengeLog.log("autosave", "auto-save deleted: \(why)") }
        }
    }

    /// What deleting the auto-save would free: what its files hold that nothing else shares (APFS's private size:
    /// a clone shares what the Mac hasn't written since). A file with other links frees nothing.
    static func frees(_ id: UUID) -> Int64 {
        guard let walk = FileManager.default.enumerator(atPath: folder(id).path) else { return 0 }
        var total: Int64 = 0
        for case let name as String in walk {
            total += privateSize(folder(id).appendingPathComponent(name).path)
        }
        return total
    }

    /// What deleting one file would free: 0 for one with other links or not a file; all its blocks if not known.
    nonisolated static func privateSize(_ path: String) -> Int64 {
        var st = stat()
        guard lstat(path, &st) == 0, st.st_mode & S_IFMT == S_IFREG, st.st_nlink == 1 else { return 0 }
        var list = attrlist()
        list.bitmapcount = u_short(ATTR_BIT_MAP_COUNT)
        list.forkattr = attrgroup_t(ATTR_CMNEXT_PRIVATESIZE)
        var buf = [UInt8](repeating: 0, count: 32)
        guard getattrlist(path, &list, &buf, buf.count, UInt32(FSOPT_ATTR_CMN_EXTENDED | FSOPT_NOFOLLOW)) == 0 else {
            return Int64(st.st_blocks) * 512
        }
        return buf.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: 4, as: Int64.self) }
    }

    /// The person's choice (the Mac's settings): a disk kept aside goes back in the Mac's place, and the disk there
    /// is kept aside in its stead (swapped: nothing deleted). The auto-save goes: it would offer to undo this. Only
    /// with no Mac running in Henge and this one not suspended (its saved memory is of the disk it has).
    /// listed: the disk's Stamp when it was listed (a name may be reused since).
    @MainActor
    static func useAside(_ aside: URL, listed: Stamp, for mac: VirtualMac) -> String? {
        guard henge_vm_state() != 1, starting == nil else { return "Close the Mac that is running first." }
        undoUnfinished(mac)
        guard Stamp.of(aside) == listed else { return "That disk has changed since it was listed: look again." }
        // Startup settings: the ones beside this disk go in with it; the Mac's now are kept beside the disk going out
        if let now = Stamp.of(mac.disk), !keepAux(mac, for: now) {
            return "The Mac's startup settings couldn't be kept with its disk (\(String(cString: strerror(errno)))): nothing was changed."
        }
        guard !SuspendedMacs.shared.ids.contains(mac.id) else {
            return "\(mac.name) is suspended: start it and shut it down first (its saved state goes with the disk it has now)."
        }
        guard renamex_np(aside.path, mac.disk.path, UInt32(RENAME_SWAP)) == 0 else {
            return "It couldn't be swapped in (\(String(cString: strerror(errno))))."
        }
        let aux = keptAux(mac.id, for: listed)
        if FileManager.default.fileExists(atPath: aux.path) {
            if rename(aux.path, mac.aux.path) != 0 {
                HengeLog.log("autosave", "the startup settings kept with that disk couldn't be put in (\(String(cString: strerror(errno))))")
            }
        } else {
            HengeLog.log("autosave", "no startup settings were kept with that disk: the Mac's are kept as they are")
        }
        syncFolder(mac.disk.deletingLastPathComponent())
        HengeLog.log("autosave", "the person put the disk kept as \(aside.lastPathComponent) back in the Mac's place; the one there is kept under that name")
        forget(mac.id, "the person chose a disk kept aside")
        return nil
    }

    /// At launch, before anything runs, whatever else can be read: a half-written one (<id>.new) and one being thrown
    /// away (.trash). Out of the way now (a .new renamed to a .trash name, so an auto-save taken soon after can't
    /// lose its own .new to this), deleted on the tidy queue.
    static func removeHalfDone() {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []
        var renamed = false
        for name in names where name.hasSuffix(".new") || name.hasSuffix(".trash") {
            var url = root.appendingPathComponent(name)
            if name.hasSuffix(".new") {
                let trash = root.appendingPathComponent("\(name).\(UUID().uuidString).trash")
                guard rename(url.path, trash.path) == 0 else {
                    delete(url) // here and now, as it can
                    continue
                }
                url = trash
                renamed = true
            }
            tidy.async { delete(url) }
        }
        if renamed { syncFolder(root) }
    }

    /// At launch, with the list of Macs read: one without its snapshot, and those of Macs that no longer exist. One
    /// whose note can't be read is kept (as a Suspend is): a later Henge may read it, and the Mac's next auto-save
    /// replaces it.
    static func removeLeftovers(known: Set<UUID>) {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []
        for name in names {
            let url = root.appendingPathComponent(name)
            guard let id = UUID(uuidString: name) else { continue }
            if !known.contains(id) { forget(id, "its Mac is gone") }
            else if !FileManager.default.fileExists(atPath: stateURL(url).path) { forget(id, "it has no snapshot") }
        }
    }

    private static func free(at url: URL) -> Int64 {
        (try? url.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?.volumeAvailableCapacityForImportantUsage ?? 0
    }

    /// Free storage as the file system counts it now (statfs): what writes can use this moment. Unlike iOS's
    /// "important usage" figure it leaves out what iOS could free by purging caches, which it does only later.
    /// -1 if it can't be read.
    static func freeNow() -> Int64 {
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        var fs = statfs()
        guard statfs(root.path, &fs) == 0 else { return -1 }
        return Int64(fs.f_bavail) * Int64(fs.f_bsize)
    }

    /// The Mac's disks are as they were at its auto-save: neither written since (same size, and the same time
    /// written to the nanosecond: a clone keeps it, and any write changes it). Then carrying on from it loses nothing.
    static func untouched(_ id: UUID) -> Bool {
        let dir = folder(id), home = Machines.directory(for: id)
        func same(_ a: URL, _ b: URL) -> Bool { Stamp.of(a).map { $0 == Stamp.of(b) } ?? false }
        // (VirtualMac's disk and aux)
        return same(home.appendingPathComponent("disk.img"), diskURL(dir)) && same(home.appendingPathComponent("auxiliary.bin"), auxURL(dir))
    }

    /// A file as it is: its size and when it was last written, to the nanosecond (a clone or a rename keeps both).
    struct Stamp: Codable, Equatable {
        var size: Int64
        var sec: Int
        var nsec: Int
        static func of(_ url: URL) -> Stamp? {
            var st = stat()
            guard stat(url.path, &st) == 0 else { return nil }
            return Stamp(size: Int64(st.st_size), sec: st.st_mtimespec.tv_sec, nsec: st.st_mtimespec.tv_nsec)
        }
        var date: Date { Date(timeIntervalSince1970: TimeInterval(sec) + TimeInterval(nsec) / 1e9) }
    }

    /// Written as Carry On starts, deleted once the Mac runs from it: the Mac's disk as it was before (Stamp). Still
    /// there at the next start, that Carry On didn't finish, and its disk may be the auto-save's already.
    private static func carryingURL(_ dir: URL) -> URL { dir.appendingPathComponent("carrying-on.json") }

    /// The person's choice (the Mac's settings): a disk kept aside deleted, with the startup settings kept beside it.
    /// listed: its Stamp when it was listed. nil, or why not.
    @MainActor
    static func deleteAside(_ aside: URL, listed: Stamp, for mac: VirtualMac) -> String? {
        guard starting != mac.id else { return "\(mac.name) is starting." }
        // First: one waiting to go back in is the Mac's newest disk, not one to delete. (Not while a Mac runs: it may be
        // running on the disk the undo would move.)
        if henge_vm_state() != 1 { undoUnfinished(mac) }
        guard Stamp.of(aside) == listed else { return "That disk has changed since it was listed: look again." }
        do { try FileManager.default.removeItem(at: aside) } catch { return "It couldn't be deleted: \(error.localizedDescription)" }
        try? FileManager.default.removeItem(at: keptAux(mac.id, for: listed))
        HengeLog.log("autosave", "the person deleted the disk kept as \(aside.lastPathComponent)")
        return nil
    }

    /// The disks a Mac's Carry On kept aside: "Disk before Carry On.img", then "… 2.img" and on (host/disk.c).
    static func asideDisks(_ mac: UUID) -> [URL] {
        let home = Machines.directory(for: mac)
        let names = (try? FileManager.default.contentsOfDirectory(atPath: home.path)) ?? []
        return names.filter { $0.hasPrefix("Disk before Carry On") && $0.hasSuffix(".img") }.sorted().map { home.appendingPathComponent($0) }
    }

    /// Before anything else at a start: a Carry On that didn't finish (Henge ended or it failed partway) is undone,
    /// so the Mac's disk is again the one it had when Henge closed, whatever is chosen next. Its disk had been put
    /// aside (or left as disk.img.placing) by the engine; it is swapped back in, and the auto-save's copy it held is
    /// deleted only if it is that copy exactly (the auto-save keeps its own).
    static func undoUnfinished(_ mac: VirtualMac) {
        let dir = folder(mac.id), note = carryingURL(dir)
        // A copy of the auto-save's disk left over (Henge ended before its swap): it holds nothing else, and would hold
        // a whole disk's storage once the auto-save goes
        let placing = URL(fileURLWithPath: mac.disk.path + ".placing")
        if let p = Stamp.of(placing), p == Stamp.of(diskURL(dir)) {
            try? FileManager.default.removeItem(at: placing)
            HengeLog.log("autosave", "a copy of the auto-save's disk left by a Carry On that didn't finish, deleted")
        }
        guard let data = try? Data(contentsOf: note) else { return }
        var tryAgain = false // the next start, if it couldn't be put back now
        defer { if !tryAgain { try? FileManager.default.removeItem(at: note) } }
        guard let before = try? JSONDecoder().decode(Stamp.self, from: data) else {
            HengeLog.log("autosave", "a Carry On didn't finish, and its note can't be read: the disk is left as it is")
            return
        }
        let now = Stamp.of(mac.disk)
        if now == before {
            HengeLog.log("autosave", "a Carry On didn't finish before its disk was changed: nothing to undo")
            return
        }
        // Only a disk that is the auto-save's copy exactly, never written to: if the Mac has run on it since (this
        // note somehow outlived a Carry On that worked), putting the old one back would undo that run.
        guard now != nil, now == Stamp.of(diskURL(dir)) else {
            HengeLog.log("autosave", "a Carry On's note was left, but the Mac's disk isn't the auto-save's copy: left as it is")
            return
        }
        let candidates = [placing] + asideDisks(mac.id).reversed()
        guard let found = candidates.first(where: { Stamp.of($0) == before }) else {
            HengeLog.log("autosave", "a Carry On didn't finish, and the disk from before it isn't among those kept aside: left as it is")
            return
        }
        guard renamex_np(found.path, mac.disk.path, UInt32(RENAME_SWAP)) == 0 else {
            HengeLog.log("autosave", "a Carry On didn't finish, and its disk couldn't be put back (\(String(cString: strerror(errno)))): "
                         + "it is kept as \(found.lastPathComponent), and tried again at the next start")
            tryAgain = true
            return
        }
        syncFolder(mac.disk.deletingLastPathComponent())
        // Its startup settings with it, kept as the Carry On started (the auto-save's may be in place by now)
        let aux = keptAux(mac.id, for: before)
        if FileManager.default.fileExists(atPath: aux.path) {
            if rename(aux.path, mac.aux.path) == 0 { syncFolder(mac.aux.deletingLastPathComponent()) }
            else { HengeLog.log("autosave", "its startup settings couldn't be put back with it (\(String(cString: strerror(errno))))") }
        }
        let copy = Stamp.of(found) != nil && Stamp.of(found) == Stamp.of(diskURL(dir))
        if copy { try? FileManager.default.removeItem(at: found) }
        HengeLog.log("autosave", "a Carry On didn't finish: the disk it had when Henge closed is back in place"
                     + (copy ? "" : " (what it held is kept as \(found.lastPathComponent))"))
    }

    struct Result {
        var ok: Bool
        var why: String
        var cancelled: Bool
        var skipped: Bool = false
    }

    /// On the pause queue, the Mac running. Never worse than not taking it: whatever happens the Mac runs on (or,
    /// held, stays paused as asked), and the auto-save from before stays until this one is complete.
    /// ticket: henge_vm_suspend_ticket() when it was started (shutting down calls it off: nothing kept).
    /// hold: henge_vm_autosave_hold_ticket() then (a pause asked for since lets it finish and leaves the Mac paused).
    static func take(_ info: MacSuspend.Saved, tools: URL?, ticket: UInt64, hold: UInt64) -> Result {
        let fm = FileManager.default
        let id = info.macID
        let dir = newFolder(id)
        if fm.fileExists(atPath: dir.path) { throwAway(dir) }
        do {
            try fm.createDirectory(at: root, withIntermediateDirectories: true,
                                   attributes: [.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication])
            try fm.createDirectory(at: dir, withIntermediateDirectories: false,
                                   attributes: [.protectionKey: FileProtectionType.completeUntilFirstUserAuthentication])
            var excluded = dir
            var values = URLResourceValues()
            values.isExcludedFromBackup = true
            try excluded.setResourceValues(values)
        } catch {
            return Result(ok: false, why: "its folder couldn't be made: \(error.localizedDescription)", cancelled: false)
        }
        // Room: what a Suspend would need now, plus what the clones may come to hold as the live Mac moves on (its
        // memory files diverge as they are written; its disk as it writes), plus spare, so a Suspend after this
        // one always has its room. Without it: not taken (the one from before is kept).
        let room = henge_vm_suspend_room_mb()
        let suspendNeed = Int64((room == UInt64.max ? UInt64(info.ramMB) : room) << 20) + (512 << 20)
        let need = suspendNeed + Int64(info.ramMB << 20) + spareToTake
        // Counted as iOS does for important writes (what it would purge included, as a Suspend counts it), and
        // what is free this moment well clear of where one kept is dropped (so it isn't dropped as soon as it's made).
        let freeImportant = free(at: dir), freeNow = freeNow()
        guard freeImportant >= need, freeNow < 0 || freeNow >= dropBelow + (2 << 30) else {
            try? fm.removeItem(at: dir)
            return Result(ok: false, why: String(format: "not enough free storage (%.1f GB wanted, %.1f GB free, %.1f GB of it free now)",
                                                 Double(need) / 1_073_741_824, Double(freeImportant) / 1_073_741_824,
                                                 Double(max(freeNow, 0)) / 1_073_741_824),
                          cancelled: false, skipped: true)
        }
        if info.tools {
            guard let tools, link(tools.path, toolsURL(dir).path) == 0 else {
                try? fm.removeItem(at: dir)
                return Result(ok: false, why: "the Henge Tools disk couldn't be kept with it", cancelled: false)
            }
        }
        guard let data = try? JSONEncoder().encode(info),
              (try? data.write(to: infoURL(dir), options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])) != nil else {
            try? fm.removeItem(at: dir)
            return Result(ok: false, why: "its note couldn't be written", cancelled: false)
        }
        var why = [CChar](repeating: 0, count: 256)
        let rc = henge_vm_autosave(stateURL(dir).path, diskURL(dir).path, auxURL(dir).path, 2000, ticket, hold, &why, why.count)
        guard rc == 0 else {
            _ = henge_vm_discard_saved(stateURL(dir).path)
            try? fm.removeItem(at: dir)
            return Result(ok: false, why: String(cString: why), cancelled: rc == -3)
        }
        // In place of the one before, at once: swapped (both names always hold a whole one), the old then deleted.
        let live = folder(id)
        var swapped: Int32
        if fm.fileExists(atPath: live.path) {
            swapped = renamex_np(dir.path, live.path, UInt32(RENAME_SWAP))
        } else {
            swapped = rename(dir.path, live.path)
        }
        guard swapped == 0 else {
            let e = String(cString: strerror(errno))
            _ = henge_vm_discard_saved(stateURL(dir).path)
            try? fm.removeItem(at: dir)
            return Result(ok: false, why: "it couldn't be put in place (\(e))", cancelled: false)
        }
        syncFolder(root)
        if fm.fileExists(atPath: dir.path) { throwAway(dir) } // the one before, now under the .new name
        return Result(ok: true, why: "", cancelled: false)
    }

    /// A file (or folder) on storage, F_FULLFSYNC; false if it couldn't be.
    nonisolated private static func fullSync(_ url: URL) -> Bool {
        let fd = open(url.path, O_RDONLY)
        guard fd >= 0 else { return false }
        defer { close(fd) }
        return fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0
    }

    /// A folder's entries on storage (F_FULLFSYNC): a rename in it survives the device losing power.
    private static func syncFolder(_ url: URL) {
        let fd = open(url.path, O_RDONLY)
        guard fd >= 0 else { return }
        if fcntl(fd, F_FULLFSYNC) != 0 { fsync(fd) }
        close(fd)
    }

    // MARK: Going on

    /// Anything in the Shared folder (Sharing) written after date: Carry On doesn't put those back, the Mac's memory
    /// having known them as they were. Looks at no more than a few thousand entries.
    nonisolated private static func sharedChanged(since date: Date) -> Bool {
        let keys: [URLResourceKey] = [.contentModificationDateKey, .attributeModificationDateKey]
        guard let walk = FileManager.default.enumerator(at: Sharing.folder, includingPropertiesForKeys: keys) else { return false }
        func newer(_ url: URL) -> Bool {
            let v = try? url.resourceValues(forKeys: Set(keys))
            return [v?.contentModificationDate, v?.attributeModificationDate].contains { ($0 ?? .distantPast) > date }
        }
        if newer(Sharing.folder) { return true } // (a folder's time changes when something in it is added or deleted)
        var seen = 0
        for case let url as URL in walk {
            seen += 1
            if seen > 5000 { return true } // too many to look through: said, to be safe
            if newer(url) { return true }
        }
        return false
    }

    /// Starts the Mac from its auto-save, if the person wants that; nil if there is none (start as usual).
    /// fresh: starts it as henge_vm_boot does, and deletes the auto-save once it has (MacSuspend.boot): from then it
    /// is older than the Mac, and offering it after a crash would offer to undo this run.
    @MainActor
    /// alwaysAsk: the person chose Start Fresh just before (a Suspend given up on): asked even if it would carry on
    /// without asking, rather than putting back what they chose to leave.
    static func boot(_ mac: VirtualMac, fresh: () -> MacSuspend.Boot, alwaysAsk: Bool = false) async -> MacSuspend.Boot? {
        undoUnfinished(mac)
        guard let saved = saved(for: mac.id) else {
            if exists(mac.id) { HengeLog.log("autosave", "there is an auto-save, but its note can't be read: kept, and not offered") }
            return nil
        }
        let when = Calendar.current.isDateInToday(saved.saved) ? saved.saved.formatted(date: .omitted, time: .shortened)
            : saved.saved.formatted(.dateTime.weekday(.abbreviated).day().month(.abbreviated).hour().minute())
        let ago = RelativeDateTimeFormatter().localizedString(for: saved.saved, relativeTo: Date())
        let still = untouched(mac.id)
        // How long the Mac went on writing its disk after the auto-save: long, and carrying on would undo that much
        // work (kept aside, but out of the Mac), so Start Fresh is the choice offered first.
        let writtenUntil = Stamp.of(mac.disk)?.date
        let after = still ? 0 : max(writtenUntil.map { $0.timeIntervalSince(saved.saved) } ?? 0, 0)
        // Turned off with Force Shut Down: the person chose to turn it off, so starting anew is the choice offered first.
        let forced = saved.forcedOff == true
        let freshFirst = after > 30 * 60 || forced
        HengeLog.log("autosave", "an auto-save from \(saved.saved) (\(saved.ramMB) MB, \(saved.cpus) CPUs, \(saved.drives.count) drives, "
                     + "\(saved.failures) failed before), the disk \(still ? "unchanged since" : String(format: "written for %.0f min after", after / 60))")
        // What would surely refuse it, first: then the person isn't asked to choose something that can't happen
        if let why = cantGoOn(saved, mac) {
            HengeLog.log("autosave", "can't carry on: \(why)")
            let startFresh = await ask(mac, title: "\(mac.name) can't carry on from its auto-save",
                                       message: why + " Start Fresh starts macOS anew on the disk as it was when Henge closed: apps that "
                                           + "were open, and anything not saved in them, are gone, and the auto-save is deleted. Keep for "
                                           + "Later leaves the auto-save, to "
                                           + "try again once that's sorted.",
                                       first: "Start Fresh", second: "Keep for Later")
            guard startFresh == true else { MacDrives.shared.stopped(); return .closed }
            HengeLog.log("autosave", "the person started the Mac fresh")
            return fresh()
        }
        // The disk unchanged since (it was paused as it was auto-saved, or nothing was written after): carrying on
        // loses nothing, and starting fresh would lose the open apps, so it carries on without asking. Not after two
        // tries that didn't finish, nor after a Force Shut Down: the person chooses then.
        if still && saved.failures < 2 && !alwaysAsk && !forced {
            HengeLog.log("autosave", "carrying on without asking: nothing on its disk is newer than the auto-save")
        } else {
            let tried = saved.failures >= 2 ? " Henge has tried twice to carry on from it and couldn't." : ""
            let span = after >= 3600 ? String(format: "%.0f hour%@", (after / 3600).rounded(), (after / 3600).rounded() == 1 ? "" : "s")
                : "\(max(Int(after / 60), 1)) minute\(Int(after / 60) == 1 ? "" : "s")"
            let newer = still ? "" : " Its disk goes back to then too" + (after >= 60 ? ", undoing \(span) of work on it" : "")
                + ": anything saved on the Mac after \(when) won't be on it. The disk as it was when Henge closed is kept "
                + "in the Mac's settings (Disks Kept by Carry On), so nothing is deleted, and it can be put back."
            let since = saved.saved
            let shared = await Task.detached(priority: .userInitiated) { sharedChanged(since: since) }.value
                ? " Files in the Shared folder that changed since then stay as they are now (they aren't put back): an app "
                    + "that had one open may need to open it again." : ""
            let carry = "Carry On from \(when)"
            let answer = await ask(mac, title: forced ? "\(mac.name) was turned off with Force Shut Down" : "\(mac.name) was running when Henge closed",
                                   message: "It was auto-saved \(ago) (\(when)).\(tried) Carry On puts it back exactly as it was then, "
                                       + "with its apps and windows open.\(newer)\(shared) Start Fresh keeps the disk as it was when "
                                       + (forced ? "it was turned off " : "Henge closed ")
                                       + "and starts macOS anew: apps that were open, and anything not saved in them, are gone, and the "
                                       + "auto-save is deleted.",
                                   first: freshFirst ? "Start Fresh" : carry, second: freshFirst ? carry : "Start Fresh")
            guard let answer else { MacDrives.shared.stopped(); return .closed } // the session closed: kept for later
            guard answer != freshFirst else {
                HengeLog.log("autosave", "the person started the Mac fresh")
                return fresh()
            }
        }
        // Noted before anything changes: the disk as it is, so a Carry On that doesn't finish is undone at the next start.
        // And its startup settings beside it, if it will be kept aside (they go back with it).
        if !still, let disk = Stamp.of(mac.disk), !keepAux(mac, for: disk) {
            HengeLog.log("autosave", "its startup settings couldn't be kept with the disk kept aside (\(String(cString: strerror(errno)))): "
                         + "putting that disk back keeps the ones it has then")
        }
        guard let before = Stamp.of(mac.disk), let note = try? JSONEncoder().encode(before),
              (try? note.write(to: carryingURL(folder(mac.id)), options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])) != nil,
              fullSync(carryingURL(folder(mac.id))), fullSync(folder(mac.id))
        else {
            HengeLog.log("autosave", "not carried on: its disk couldn't be noted first")
            return .failed("The Mac couldn't carry on from its auto-save: Henge couldn't note its disk first (is this \(Device.name)'s "
                           + "storage full?). Nothing was changed.")
        }
        var attempt = saved
        attempt.failures += 1 // before it is tried: a crash partway counts
        write(attempt)
        _ = MacDrives.shared.preplug(saved)
        let dir = folder(mac.id)
        if saved.tools { _ = henge_vm_preplug(toolsURL(dir).path, 1, "Henge Tools", MacDrives.toolsSerial) }
        let aside = asideDisk(mac.id)
        let (rc, why): (Int32, String) = await Task.detached(priority: .userInitiated) {
            var buffer = [CChar](repeating: 0, count: 256)
            let rc = henge_vm_boot_autosaved(mac.rom.path, mac.disk.path, mac.aux.path, diskURL(dir).path, auxURL(dir).path,
                                             aside.path, mac.ecid, mac.seed, saved.ramMB, saved.cpus, stateURL(dir).path,
                                             &buffer, buffer.count)
            return (rc, String(cString: buffer))
        }.value
        switch rc {
        case 0:
            try? FileManager.default.removeItem(at: carryingURL(dir)) // it runs from it now: nothing to undo
            var done = saved
            done.failures = 0
            done.forcedOff = nil // it runs again: a later stop is noted anew
            write(done)
            let kept = asideDisks(mac.id).first { Stamp.of($0) == before }
            HengeLog.log("autosave", "the Mac carries on from its auto-save of \(saved.saved)"
                         + (kept.map { "; the disk it had when Henge closed is kept as \($0.lastPathComponent)" } ?? ""))
            if saved.tools, link(toolsURL(dir).path, MacSuspend.runningToolsURL(mac.id).path) != 0 {
                HengeLog.log("autosave", "its Henge Tools file couldn't be kept: a later Suspend won't have that disk")
            }
            carriedOn = mac.id // kept: until the next one, it is the Mac as it is now
            return .started(adopt: saved)
        case -1: // refused, built on the Mac's own disks: it can start fresh here
            write(saved)
            try? FileManager.default.removeItem(at: carryingURL(dir)) // refused before anything changed
            HengeLog.log("autosave", "refused: \(why)")
            let startFresh = await ask(mac, title: "\(mac.name) can't carry on from its auto-save",
                                       message: why + " Start Fresh starts macOS anew on the disk as it is, and deletes the auto-save.",
                                       first: "Start Fresh", second: "Keep for Later")
            if startFresh == true {
                HengeLog.log("autosave", "refused, and the person started the Mac fresh")
                guard henge_vm_start_fresh() == 0 else { return .failed("The Mac couldn't be started.") }
                forget(mac.id, "the Mac started anew") // older than this run from here on
                return .started(adopt: saved)
            }
            henge_vm_drop_built()
            MacDrives.shared.stopped()
            return .closed
        case -4: // refused, and the Mac's own disks couldn't be opened in place of the copies: this process can't build another
            write(saved)
            try? FileManager.default.removeItem(at: carryingURL(dir)) // refused before anything changed
            MacDrives.shared.stopped()
            HengeLog.log("autosave", "refused: \(why)")
            return .failed("\(why) Nothing was changed. Close Henge and open it again, then choose Start Fresh to start the Mac anew.")
        case -3:
            write(saved)
            try? FileManager.default.removeItem(at: carryingURL(dir)) // refused before anything changed
            MacDrives.shared.stopped()
            HengeLog.log("autosave", "not built: \(why)")
            return .failed(why)
        default:
            MacDrives.shared.stopped()
            HengeLog.log("autosave", "failed (\(attempt.failures)): \(why)")
            return .failed("\(why) The auto-save is kept: close Henge and open it again to try once more, or to start fresh.")
        }
    }

    /// Henge turned the Mac off after its auto-save (MacPause.shutDown, on the pause queue): noted with it.
    static func markForcedOff(_ id: UUID) {
        guard var marked = saved(for: id) else { return }
        marked.forcedOff = true
        write(marked)
    }

    private static func write(_ saved: MacSuspend.Saved) {
        guard let data = try? JSONEncoder().encode(saved),
              (try? data.write(to: infoURL(folder(saved.macID)), options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])) != nil
        else { HengeLog.log("autosave", "its note couldn't be written"); return }
    }

    /// Why it surely can't carry on, before anything is built; nil when it may.
    @MainActor
    private static func cantGoOn(_ saved: MacSuspend.Saved, _ mac: VirtualMac) -> String? {
        if let missing = MacDrives.shared.openForResume(saved) {
            return "The USB drive \"\(missing)\" it had in can't be found."
        }
        if saved.tools && !FileManager.default.fileExists(atPath: toolsURL(folder(mac.id)).path) {
            return "The Henge Tools disk it had in is gone."
        }
        let freeNow = free(at: mac.directory)
        let room = freeNow > 0 ? UInt64(max(freeNow - (2 << 30), 0)) >> 20 : UInt64.max
        let reach = henge_largest_mapping_mb()
        if saved.ramMB > min(room, reach > 512 ? reach - 512 : 0) {
            return "There isn't room for its memory as it was (\(saved.ramMB / 1024) GB): free some storage on this \(Device.name), or close other apps."
        }
        return nil
    }

    /// The person chooses: true for the first button, false for the second, nil if the session closed first.
    @MainActor
    private static func ask(_ mac: VirtualMac, title: String, message: String, first: String, second: String) async -> Bool? {
        let question = ResumeQuestion(title: title, message: message, first: first, second: second)
        let answer: Bool = await withTaskCancellationHandler {
            await withCheckedContinuation { (done: CheckedContinuation<Bool, Never>) in
                question.answered = { first in
                    if MacPause.shared.resumeQuestion?.id == question.id { MacPause.shared.resumeQuestion = nil }
                    done.resume(returning: first)
                }
                MacPause.shared.resumeQuestion = question
            }
        } onCancel: {
            Task { @MainActor in question.answer(false) }
        }
        return Task.isCancelled ? nil : answer
    }
}

/// Free storage, watched while a Mac runs (every 5 s, off the main thread): an auto-save kept is dropped as soon as the
/// device nears full, since its blocks are the running Mac's to have (its disk writes fail when there is no room). Near
/// full is 3 GB free plus 30 s at the rate storage has been filling in the last minute, so a fast fill is met early.
/// A disk Carry On kept aside is never deleted here: it is the person's work from after the auto-save.
final class AutoSaveRoomWatch {
    private let queue = DispatchQueue(label: "henge.autosave.room", qos: .utility)
    private var timer: DispatchSourceTimer?
    private var samples: [(at: Date, free: Int64)] = [] // the queue's

    /// drop: called on this watch's queue with why, to delete the auto-save (on the pause queue).
    func start(_ id: UUID, drop: @escaping (String) -> Void) {
        stop()
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 5, repeating: 5, leeway: .seconds(1))
        timer.setEventHandler { [weak self] in self?.check(id, drop) }
        self.timer = timer
        queue.async { self.samples = [] }
        timer.resume()
    }

    func stop() {
        timer?.cancel()
        timer = nil
    }

    private func check(_ id: UUID, _ drop: (String) -> Void) {
        let free = MacAutoSave.freeNow()
        guard free >= 0 else { return }
        let now = Date()
        samples.append((now, free))
        samples.removeAll { now.timeIntervalSince($0.at) > 60 }
        // The fastest it has been filling: between neighbours, and over the minute
        var rate = 0.0
        for (a, b) in zip(samples, samples.dropFirst()) where b.at > a.at {
            rate = max(rate, Double(a.free - b.free) / b.at.timeIntervalSince(a.at))
        }
        if let first = samples.first, now > first.at { rate = max(rate, Double(first.free - free) / now.timeIntervalSince(first.at)) }
        let line = MacAutoSave.dropBelow + Int64(max(rate, 0) * 30)
        guard free < line else { return }
        let gb = { (b: Int64) in String(format: "%.1f GB", Double(b) / 1_073_741_824) }
        // Only the auto-save is ever dropped here. A disk kept aside by Carry On is the user's
        // work from after the auto-save, so it is never deleted without them.
        if MacAutoSave.exists(id) {
            drop("this device is nearly full (\(gb(free)) free, filling at up to \(gb(Int64(rate))) a second)")
        }
    }
}
