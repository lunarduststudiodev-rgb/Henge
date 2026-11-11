import SwiftUI
import UniformTypeIdentifiers

/// A disk the Mac gets as a USB drive, plugged in and ejected while it runs, as a USB stick or an external SSD on
/// a real Mac: a disk image from Files, used where it is (on this device, a USB drive plugged into it, or a file
/// provider), or a new disk made here, in Files, Henge, Disks. The engine has four USB ports for them
/// (devices/xhci.c, usb_storage.c); the Henge Tools disk uses one while it's in.
struct MacDrive: Codable, Identifiable, Hashable {
    var id = UUID()
    var name: String
    /// A file picked in Files: a bookmark to it, so it's used where it is, and again after Henge restarts.
    var bookmark: Data?
    /// A disk made here: its file's name in Files, Henge, Disks.
    var made: String?
    var readOnly = false
    /// In the Mac when it last ran: it goes back in as the Mac starts, as a drive left plugged into a Mac would.
    var inMac = false
    /// Its USB serial number, which names it to Henge Tools on the Mac.
    var serial = "HENGE-" + UUID().uuidString.prefix(8).uppercased()
    /// Made here, and not yet a Mac disk: Henge Tools makes it one as it first arrives (it comes as FAT32).
    var fresh = false
}

@MainActor final class MacDrives: ObservableObject {
    static let shared = MacDrives()

    enum Phase: Equatable { case arriving, ready, settingUp, ejecting }

    /// A problem to put to the person, each with the thing to do about it.
    enum Problem: Identifiable {
        case inUse(MacDrive)          // Henge Tools asked macOS to eject it, and macOS wouldn't
        case ejectOnMac(MacDrive)     // no Henge Tools to eject it: Finder's eject button, or remove it anyway
        case macFormat(URL)           // a .dmg or .sparseimage: the Mac opens those itself, from the Shared folder
        case otherFormat(String, String) // a virtual machine disk of another kind: name, kind
        case cantOpen(MacDrive)       // its file is gone, or not downloaded
        case cantMake(String)
        case noPortForTools           // four disks in, and the installer wanted
        case suspended                // a disk put in or taken out of a suspended Mac: its saved state would no longer fit
        var id: String {
            switch self {
            case .inUse(let d): return "use-\(d.id)"
            case .ejectOnMac(let d): return "mac-\(d.id)"
            case .macFormat(let u): return "fmt-\(u.path)"
            case .otherFormat(let n, _): return "other-\(n)"
            case .cantOpen(let d): return "open-\(d.id)"
            case .cantMake(let n): return "make-\(n)"
            case .noPortForTools: return "tools"
            case .suspended: return "suspended"
            }
        }
    }

    static let ports = 4

    @Published private(set) var drives: [MacDrive] = []
    @Published private(set) var phases: [UUID: Phase] = [:]
    @Published var problem: Problem?
    /// The Henge Tools disk's port while it's in the Mac.
    @Published private(set) var toolsPort: Int32 = 0
    /// The Henge Tools file in the Mac: this launch's (Sharing.toolsDisk, unpacked anew each launch), or for a Mac
    /// resumed from Suspend the very file it had (MacSuspend.runningToolsURL). A Suspend keeps that one.
    private var toolsFile: URL?

    private var mac: VirtualMac?
    private var running = false
    private var ports: [UUID: Int32] = [:]
    private var opened: [UUID: URL] = [:] // security-scoped files being used, to let go of when they leave
    private var asked: [UUID: Date] = [:] // when Henge Tools was asked to eject or set up a drive

    var inUse: Int { ports.count + (toolsPort > 0 ? 1 : 0) }
    var portsFree: Bool { inUse < Self.ports }

    static var madeFolder: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("Disks", isDirectory: true)
    }

    private static func key(_ id: UUID) -> String { "henge.drives.\(id.uuidString)" }

    // MARK: The session

    /// Once the Mac is built (henge_vm_boot): the drives it had plug back in, and the Henge Tools disk if the Mac
    /// may need it (Henge Tools wasn't running there last time).
    /// adopting: the drives a resumed Mac (Suspend) had, plugged in before it was built (preplug), which the
    /// restore put back in the ports they were in: they are taken as they are, not plugged in a second time.
    func start(_ mac: VirtualMac, adopting saved: MacSuspend.Saved? = nil) {
        self.mac = mac
        running = true
        ports = [:]
        phases = [:]
        asked = [:]
        toolsPort = 0
        toolsFile = nil
        drives = Self.load(mac.id)
        if let saved {
            for kept in saved.drives where drives.contains(where: { $0.id == kept.id }) {
                let port = henge_vm_disk_port(kept.serial)
                guard port > 0 else { continue }
                ports[kept.id] = port
                phases[kept.id] = .arriving
                HengeLog.log("drives", "\(kept.name) is back in port \(port), as it was")
            }
            if saved.tools {
                let port = henge_vm_disk_port(Self.toolsSerial)
                if port > 0 {
                    toolsPort = port
                    let kept = MacSuspend.runningToolsURL(mac.id)
                    if FileManager.default.fileExists(atPath: kept.path) { toolsFile = kept }
                    else { HengeLog.log("suspend", "the Henge Tools disk is in, but its file wasn't kept: this session can pause, not suspend") }
                }
            }
        }
        if !Sharing.toolsAreCurrent(for: mac.id) { plugTools() }
        for drive in drives where drive.inMac && ports[drive.id] == nil { plug(drive, quietly: true) }
    }

    private static func load(_ id: UUID) -> [MacDrive] {
        UserDefaults.standard.data(forKey: key(id)).flatMap { try? JSONDecoder().decode([MacDrive].self, from: $0) } ?? []
    }

    static let toolsSerial = "HENGE-TOOLS"

    // MARK: Suspend

    /// What is plugged in now, for a Suspend to note: each drive, and the Henge Tools file if that disk is in.
    func pluggedForSuspend() -> (drives: [MacSuspend.Saved.Drive], tools: URL?) {
        let list = drives.compactMap { d in
            ports[d.id] == nil ? nil : MacSuspend.Saved.Drive(id: d.id, name: d.name, serial: d.serial, readOnly: d.readOnly)
        }
        return (list, toolsPort > 0 ? toolsFile : nil)
    }

    /// Why the Mac can't be suspended with the disks it has in; nil when it can. A resumed Mac whose Henge Tools
    /// file wasn't kept has that disk in with nothing to keep it by, and a resume would be refused.
    var suspendBlocker: String? {
        toolsPort > 0 && toolsFile == nil
            ? "Henge lost track of the Henge Tools disk this Mac has in. Shut the Mac down and start it again to be able to suspend it"
            : nil
    }

    /// Suspended, or being saved: a disk going in or out now would make the saved Mac not fit what it has in.
    private var suspendedNow: Bool { MacPause.shared.suspended || MacPause.shared.suspending }

    /// The Henge Tools disk is out: a resumed Mac's own copy of it isn't needed any more.
    private func dropToolsFile() {
        if let file = toolsFile, file.path != Sharing.toolsDisk?.path { try? FileManager.default.removeItem(at: file) }
        toolsFile = nil
    }

    /// Before a suspended Mac is built: each drive it had, opened where it is. The name of one that can't be
    /// found, or nil when all are here.
    func openForResume(_ saved: MacSuspend.Saved) -> String? {
        drives = Self.load(saved.macID)
        for kept in saved.drives {
            guard let drive = drives.first(where: { $0.id == kept.id }), open(drive) != nil else { return kept.name }
        }
        return nil
    }

    /// Plugs the drives openForResume found into the machine about to be built (henge_vm_preplug). false if one
    /// couldn't be queued, which the restore then refuses as a drive not there.
    func preplug(_ saved: MacSuspend.Saved) -> Bool {
        var all = true
        for kept in saved.drives {
            guard let drive = drives.first(where: { $0.id == kept.id }), let url = open(drive),
                  henge_vm_preplug(url.path, kept.readOnly ? 1 : 0, Self.deviceName(drive.name), kept.serial) == 0
            else { all = false; continue }
        }
        return all
    }

    /// The Mac has stopped: the drives went with it, still marked as in it, to go back in next time.
    func stopped() {
        running = false
        for url in opened.values { url.stopAccessingSecurityScopedResource() }
        opened = [:]
        ports = [:]
        phases = [:]
        toolsPort = 0
        dropToolsFile()
    }

    private func save() {
        guard let mac, let data = try? JSONEncoder().encode(drives) else { return }
        UserDefaults.standard.set(data, forKey: Self.key(mac.id))
    }

    private func update(_ drive: MacDrive) {
        if let i = drives.firstIndex(where: { $0.id == drive.id }) { drives[i] = drive } else { drives.append(drive) }
        save()
    }

    func isIn(_ drive: MacDrive) -> Bool { ports[drive.id] != nil }
    func phase(_ drive: MacDrive) -> Phase? { phases[drive.id] }

    // MARK: Plugging in

    /// The drive's file, opened for use where it is. nil if it can't be found.
    private func open(_ drive: MacDrive) -> URL? {
        if let url = opened[drive.id] { return url }
        if let made = drive.made {
            let url = Self.madeFolder.appendingPathComponent(made)
            return FileManager.default.fileExists(atPath: url.path) ? url : nil
        }
        guard let bookmark = drive.bookmark else { return nil }
        var stale = false
        guard let url = try? URL(resolvingBookmarkData: bookmark, bookmarkDataIsStale: &stale) else { return nil }
        guard url.startAccessingSecurityScopedResource() else { return nil }
        if stale, let fresh = try? url.bookmarkData(options: .minimalBookmark, includingResourceValuesForKeys: nil, relativeTo: nil) {
            var renewed = drive
            renewed.bookmark = fresh
            update(renewed)
        }
        opened[drive.id] = url
        return url
    }

    private func letGo(_ drive: MacDrive) {
        opened.removeValue(forKey: drive.id)?.stopAccessingSecurityScopedResource()
    }

    /// What the Mac calls the device: ASCII, as USB descriptors are here.
    private static func deviceName(_ name: String) -> String {
        let ascii = String(name.unicodeScalars.filter { $0.isASCII && $0.value >= 0x20 && $0 != "\u{7f}" }.map(Character.init))
        return ascii.trimmingCharacters(in: .whitespaces).isEmpty ? "Disk" : ascii
    }

    func plug(_ drive: MacDrive, quietly: Bool = false) {
        guard running, ports[drive.id] == nil else { return }
        if suspendedNow { if !quietly { problem = .suspended }; return }
        guard portsFree else { return }
        guard let url = open(drive) else {
            HengeLog.log("drives", "can't open \(drive.name)")
            if !quietly { problem = .cantOpen(drive) }
            var gone = drive
            gone.inMac = false
            update(gone)
            return
        }
        let port = henge_vm_plug_disk(url.path, drive.readOnly ? 1 : 0, Self.deviceName(drive.name), drive.serial)
        guard port > 0 else {
            HengeLog.log("drives", "couldn't plug \(drive.name) in (\(port))")
            letGo(drive)
            if !quietly && port == -1 { problem = .cantOpen(drive) }
            return
        }
        ports[drive.id] = port
        phases[drive.id] = .arriving
        var now = drive
        now.inMac = true
        if henge_vm_disk_readonly(port) != 0 { now.readOnly = true } // a file that can't be written
        update(now)
        HengeLog.log("drives", "plugged \(drive.name) in (port \(port)\(now.readOnly ? ", read-only" : ""))")
    }

    /// The Henge Tools disk, in the Mac (or on its way): true if it is.
    @discardableResult func plugTools() -> Bool {
        guard running else { return false }
        if toolsPort > 0 { return true }
        if suspendedNow { problem = .suspended; return false }
        guard let disk = Sharing.toolsDisk else { return false }
        guard portsFree else {
            // Four disks in: the person ejects one. Henge doesn't pull a disk out of the Mac for this.
            HengeLog.log("tools", "no USB port free for the Henge Tools disk")
            problem = .noPortForTools
            return false
        }
        let port = henge_vm_plug_disk(disk.path, 1, "Henge Tools", "HENGE-TOOLS")
        guard port > 0 else {
            HengeLog.log("tools", "couldn't plug the Henge Tools disk in (\(port))")
            return false
        }
        toolsPort = port
        toolsFile = disk
        HengeLog.log("tools", "the Henge Tools disk is plugged in (port \(port))")
        return true
    }

    /// Until macOS has taken the Henge Tools disk (and a moment to mount it), up to `seconds`.
    func waitForTools(seconds: Double) async {
        let until = Date().addingTimeInterval(seconds)
        while toolsPort > 0 && henge_vm_disk_state(toolsPort) != 2 && Date() < until {
            try? await Task.sleep(nanoseconds: 250_000_000)
        }
        try? await Task.sleep(nanoseconds: 3_000_000_000) // macOS mounts it a moment after
    }

    // MARK: Adding

    /// A file picked in Files: checked, remembered, and plugged in.
    func add(_ url: URL) {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let name = url.deletingPathExtension().lastPathComponent
        if let kind = Self.foreignKind(url) {
            HengeLog.log("drives", "\(url.lastPathComponent) is \(kind)")
            if kind == "mac" { problem = .macFormat(url) } else { problem = .otherFormat(name, kind) }
            return
        }
        if let same = drives.first(where: { Self.resolve($0)?.standardizedFileURL == url.standardizedFileURL }) {
            plug(same)
            return
        }
        guard let bookmark = try? url.bookmarkData(options: .minimalBookmark, includingResourceValuesForKeys: nil, relativeTo: nil) else {
            problem = .cantOpen(MacDrive(name: name))
            return
        }
        let iso = ["iso", "cdr"].contains(url.pathExtension.lowercased())
        let writable = FileManager.default.isWritableFile(atPath: url.path)
        let drive = MacDrive(name: name, bookmark: bookmark, readOnly: iso || !writable)
        update(drive)
        HengeLog.log("drives", "added \(url.lastPathComponent)")
        plug(drive)
    }

    private static func resolve(_ drive: MacDrive) -> URL? {
        guard let bookmark = drive.bookmark else { return nil }
        var stale = false
        return try? URL(resolvingBookmarkData: bookmark, bookmarkDataIsStale: &stale)
    }

    /// Not a raw disk image: "mac" for the Mac's own formats (the Mac opens those as files), another VM's kind,
    /// or nil for a raw image (or one this can't tell, which the Mac then makes of what it can).
    private static func foreignKind(_ url: URL) -> String? {
        guard let h = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? h.close() }
        let head = (try? h.read(upToCount: 8)) ?? Data()
        let size = (try? h.seekToEnd()) ?? 0
        if size < 512 { return "too small to be a disk" }
        if head.starts(with: Data("sprs".utf8)) { return "mac" } // .sparseimage
        if head.starts(with: [0x51, 0x46, 0x49, 0xfb]) { return "a QEMU disk (qcow2)" }
        if head.starts(with: Data("KDMV".utf8)) { return "a VMware disk (vmdk)" }
        if head.starts(with: Data("vhdxfile".utf8)) { return "a Hyper-V disk (vhdx)" }
        if (try? h.seek(toOffset: size - 512)) != nil, let tail = try? h.read(upToCount: 4), tail == Data("koly".utf8) {
            return "mac" // .dmg: compressed, or not, it has its own layout
        }
        return nil
    }

    /// A .dmg into the Shared folder, where the Mac finds it under My Shared Files and opens it itself.
    func copyToShared(_ url: URL) {
        Task.detached(priority: .userInitiated) {
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            var dest = Sharing.folder.appendingPathComponent(url.lastPathComponent)
            var n = 2
            while FileManager.default.fileExists(atPath: dest.path) {
                dest = Sharing.folder.appendingPathComponent("\(url.deletingPathExtension().lastPathComponent) \(n).\(url.pathExtension)")
                n += 1
            }
            do {
                try FileManager.default.copyItem(at: url, to: dest)
                HengeLog.log("drives", "copied \(url.lastPathComponent) to the Shared folder")
            } catch {
                HengeLog.log("drives", "couldn't copy \(url.lastPathComponent) to the Shared folder: \(error.localizedDescription)")
                await MainActor.run { MacDrives.shared.problem = .cantMake("Henge couldn't copy \(url.lastPathComponent): \(error.localizedDescription)") }
            }
        }
    }

    /// A new disk, in Files, Henge, Disks, plugged in. It takes room only as the Mac fills it.
    func make(name: String, bytes: UInt64) {
        let clean = name.trimmingCharacters(in: .whitespacesAndNewlines).replacingOccurrences(of: "/", with: "-")
        let title = clean.isEmpty ? "Disk" : clean
        try? FileManager.default.createDirectory(at: Self.madeFolder, withIntermediateDirectories: true)
        var file = "\(title).img"
        var n = 2
        while FileManager.default.fileExists(atPath: Self.madeFolder.appendingPathComponent(file).path) {
            file = "\(title) \(n).img"
            n += 1
        }
        let path = Self.madeFolder.appendingPathComponent(file).path
        guard henge_make_disk(path, bytes, title) == 0 else {
            problem = .cantMake("Henge couldn't make \(title). There may not be enough room on this \(Device.name).")
            return
        }
        let drive = MacDrive(name: title, made: file, fresh: true)
        update(drive)
        HengeLog.log("drives", "made \(file), \(bytes >> 30) GB")
        plug(drive)
    }

    // MARK: Ejecting

    /// Henge Tools can eject: an out-of-date one can too, while it updates itself.
    var toolsCanEject: Bool { henge_share_can_eject() != 0 }

    func eject(_ drive: MacDrive) {
        guard ports[drive.id] != nil else { return }
        if suspendedNow { problem = .suspended; return }
        if henge_vm_disk_state(ports[drive.id]!) == 3 { return } // ejected already: it leaves at the next look
        guard toolsCanEject else {
            problem = .ejectOnMac(drive)
            return
        }
        henge_share_eject(drive.serial)
        phases[drive.id] = .ejecting
        asked[drive.id] = Date()
        HengeLog.log("drives", "asked Henge Tools to eject \(drive.name)")
    }

    /// Out at once, without macOS letting go first: what it hadn't written yet is lost.
    func removeNow(_ drive: MacDrive) {
        guard let port = ports[drive.id] else { return }
        if suspendedNow { problem = .suspended; return }
        henge_vm_unplug_disk(port)
        left(drive, "removed \(drive.name) without the Mac ejecting it")
    }

    private func left(_ drive: MacDrive, _ why: String) {
        ports.removeValue(forKey: drive.id)
        phases.removeValue(forKey: drive.id)
        asked.removeValue(forKey: drive.id)
        letGo(drive)
        if var now = drives.first(where: { $0.id == drive.id }) {
            now.inMac = false
            update(now)
        }
        HengeLog.log("drives", why)
    }

    func forget(_ drive: MacDrive) {
        guard ports[drive.id] == nil else { return }
        drives.removeAll { $0.id == drive.id }
        save()
    }

    func setReadOnly(_ drive: MacDrive, _ on: Bool) {
        guard ports[drive.id] == nil else { return }
        var now = drive
        now.readOnly = on
        update(now)
    }

    func size(_ drive: MacDrive) -> Int64? {
        let url: URL?
        if let made = drive.made { url = Self.madeFolder.appendingPathComponent(made) } else { url = opened[drive.id] }
        guard let url, let attrs = try? FileManager.default.attributesOfItem(atPath: url.path) else { return nil }
        return (attrs[.size] as? NSNumber)?.int64Value
    }

    // MARK: Following along

    /// Once a second while the Mac runs.
    func tick() {
        guard running else { return }
        if toolsPort > 0 {
            let st = henge_vm_disk_state(toolsPort)
            if st == 3 || henge_share_eject_answer("HENGE-TOOLS") == 1 {
                henge_vm_unplug_disk(toolsPort)
                HengeLog.log("tools", "the Mac ejected the Henge Tools disk; unplugged it")
                toolsPort = 0
                dropToolsFile()
            } else if st == 0 {
                toolsPort = 0
                dropToolsFile()
            }
        }
        for drive in drives {
            guard let port = ports[drive.id] else { continue }
            let st = henge_vm_disk_state(port)
            let since = asked[drive.id].map { Date().timeIntervalSince($0) } ?? 0
            switch st {
            case 0:
                left(drive, "\(drive.name) is out")
                continue
            case 3:
                henge_vm_unplug_disk(port)
                left(drive, "the Mac ejected \(drive.name); unplugged it")
                continue
            default: break
            }
            switch phases[drive.id] {
            case .ejecting:
                let answer = henge_share_eject_answer(drive.serial)
                if answer == 1 {
                    henge_vm_unplug_disk(port) // macOS let go of it: what it wrote is in the file
                    left(drive, "Henge Tools ejected \(drive.name); unplugged it")
                } else if answer == 0 || since > 30 {
                    phases[drive.id] = .ready
                    asked.removeValue(forKey: drive.id)
                    if problem == nil { problem = .inUse(drive) }
                }
            case .settingUp:
                let answer = henge_share_format_answer(drive.serial)
                if answer >= 0 || since > 90 {
                    phases[drive.id] = .ready
                    asked.removeValue(forKey: drive.id)
                    var now = drive
                    now.fresh = false
                    update(now)
                    HengeLog.log("drives", answer == 1 ? "\(drive.name) is a Mac disk now" : "\(drive.name) stays as it came (FAT32)")
                }
            default:
                if st == 2 {
                    if drive.fresh && !drive.readOnly && henge_share_can_format() != 0 {
                        henge_share_format(drive.serial, drive.name)
                        phases[drive.id] = .settingUp
                        asked[drive.id] = Date()
                    } else {
                        if drive.fresh {
                            var now = drive
                            now.fresh = false // no Henge Tools: it stays FAT32, which the Mac reads and writes
                            update(now)
                        }
                        phases[drive.id] = .ready
                    }
                }
            }
        }
    }
}

// MARK: - The Disks sheet

struct DisksView: View {
    @ObservedObject private var drives = MacDrives.shared
    @Environment(\.dismiss) private var dismiss
    @State private var picking = false
    @State private var making = false
    @State private var newName = "New Disk"
    @State private var newSize: UInt64 = 0
    @State private var removing: MacDrive?

    var body: some View {
        NavigationStack {
            List {
                Section {
                    if drives.drives.isEmpty {
                        VStack(alignment: .leading, spacing: 6) {
                            Label("No disks yet", systemImage: "externaldrive")
                                .font(.headline)
                            Text("Add a disk image from Files, or make a new disk. The Mac sees it arrive as a USB drive, right away.")
                                .font(.subheadline).foregroundStyle(.secondary)
                        }
                        .padding(.vertical, 6)
                    }
                    ForEach(drives.drives) { drive in row(drive) }
                } footer: {
                    if !drives.portsFree {
                        Text("All four USB ports are in use\(drives.toolsPort > 0 ? " (the Henge Tools disk has one)" : ""). Eject a disk to plug in another.")
                    }
                }
                Section {
                    Button { picking = true } label: {
                        Label("Add a Disk Image…", systemImage: "plus.circle")
                    }
                    .disabled(!drives.portsFree)
                    Button { newSize = sizes.first ?? 0; making = true } label: {
                        Label("New Disk…", systemImage: "externaldrive.badge.plus")
                    }
                    .disabled(!drives.portsFree || sizes.isEmpty)
                } footer: {
                    Text("Disk images (.img, .iso and other raw images) are used where they are: on this \(Device.name), in iCloud Drive, or on a USB drive plugged into it. Eject a disk here or in the Mac's Finder before you take it away. A USB drive itself can't go to the Mac: iPadOS gives apps its files, not the drive.")
                }
            }
            .navigationTitle("Disks")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } }
            }
            .fileImporter(isPresented: $picking, allowedContentTypes: [.data, .diskImage]) { result in
                if case .success(let url) = result { drives.add(url) }
            }
            .sheet(isPresented: $making) { newDisk }
            .alert(item: $drives.problem) { problem in alertFor(problem) }
            .confirmationDialog(removing.map { "Remove \($0.name) without ejecting it?" } ?? "", isPresented: Binding(get: { removing != nil }, set: { if !$0 { removing = nil } }), titleVisibility: .visible) {
                if let drive = removing {
                    Button("Remove Now", role: .destructive) { drives.removeNow(drive) }
                }
            } message: {
                Text("Anything the Mac hasn't finished writing to it is lost, and the Mac warns that it wasn't ejected properly.")
            }
        }
        .preferredColorScheme(.dark)
    }

    @ViewBuilder private func row(_ drive: MacDrive) -> some View {
        let phase = drives.phase(drive)
        let inMac = drives.isIn(drive)
        HStack(spacing: 14) {
            Image(systemName: inMac ? "externaldrive.fill.badge.checkmark" : "externaldrive")
                .font(.system(size: 22))
                .foregroundStyle(inMac ? Desk.good : Desk.dim)
                .frame(width: 32)
            VStack(alignment: .leading, spacing: 2) {
                Text(drive.name).font(.body.weight(.medium))
                Text(status(drive, phase: phase, inMac: inMac)).font(.footnote).foregroundStyle(.secondary)
            }
            Spacer()
            if phase == .arriving || phase == .ejecting || phase == .settingUp {
                ProgressView()
            } else if inMac {
                Button("Eject") { drives.eject(drive) }
                    .buttonStyle(.bordered)
            } else {
                Button("Plug In") { drives.plug(drive) }
                    .buttonStyle(.borderedProminent)
                    .disabled(!drives.portsFree)
            }
        }
        .padding(.vertical, 4)
        .contextMenu {
            if inMac {
                Button { drives.eject(drive) } label: { Label("Eject", systemImage: "eject") }
                Button(role: .destructive) { removing = drive } label: { Label("Remove Without Ejecting", systemImage: "bolt.horizontal") }
            } else {
                Button { drives.plug(drive) } label: { Label("Plug In", systemImage: "cable.connector") }
                Toggle(isOn: Binding(get: { drive.readOnly }, set: { drives.setReadOnly(drive, $0) })) {
                    Label("Read-Only", systemImage: "lock")
                }
                Button(role: .destructive) { drives.forget(drive) } label: { Label("Remove from List", systemImage: "minus.circle") }
            }
        }
        .swipeActions {
            if !inMac {
                Button(role: .destructive) { drives.forget(drive) } label: { Label("Remove", systemImage: "minus.circle") }
            }
        }
    }

    private func status(_ drive: MacDrive, phase: MacDrives.Phase?, inMac: Bool) -> String {
        var parts: [String] = []
        switch phase {
        case .arriving: parts.append("Plugging in…")
        case .settingUp: parts.append("Making it a Mac disk…")
        case .ejecting: parts.append("Ejecting…")
        case .ready: parts.append("In the Mac")
        case nil: parts.append(inMac ? "In the Mac" : "Not plugged in")
        }
        if drive.readOnly { parts.append("read-only") }
        if let bytes = drives.size(drive) { parts.append(ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)) }
        if drive.made != nil && !inMac { parts.append("in Files, Henge, Disks") }
        return parts.joined(separator: " · ")
    }

    private func alertFor(_ problem: MacDrives.Problem) -> Alert {
        switch problem {
        case .inUse(let drive):
            return Alert(title: Text("\(drive.name) is in use"),
                         message: Text("Something on the Mac still has it open. Quit the app using it, then eject again."),
                         primaryButton: .default(Text("Try Again")) { drives.eject(drive) },
                         secondaryButton: .cancel(Text("Not Now")))
        case .ejectOnMac(let drive):
            return Alert(title: Text("Eject \(drive.name) on the Mac"),
                         message: Text("In Finder, click ⏏ next to \(drive.name). Henge unplugs it as soon as the Mac lets go. With Henge Tools on the Mac, the Eject button here does this for you."),
                         primaryButton: .default(Text("OK")),
                         secondaryButton: .destructive(Text("Remove Now")) { removing = drive })
        case .macFormat(let url):
            return Alert(title: Text("The Mac opens this kind of disk itself"),
                         message: Text("\(url.lastPathComponent) is a Mac disk image. Henge can put it in the Shared folder; on the Mac, double-click it under My Shared Files to open it."),
                         primaryButton: .default(Text("Put It in the Shared Folder")) { drives.copyToShared(url) },
                         secondaryButton: .cancel())
        case .otherFormat(let name, let kind):
            return Alert(title: Text("\(name) is \(kind)"),
                         message: Text("Henge plugs in raw disk images (.img, .iso). Convert it to a raw image on a computer first, or pick another."),
                         primaryButton: .default(Text("Pick Another")) { DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { picking = true } },
                         secondaryButton: .cancel())
        case .cantOpen(let drive):
            return Alert(title: Text("Henge can't open \(drive.name)"),
                         message: Text("It may have moved, be on a USB drive that isn't plugged in, or be in iCloud and not downloaded yet. Pick it again in Files."),
                         primaryButton: .default(Text("Pick It in Files")) {
                             drives.forget(drive)
                             DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { picking = true }
                         },
                         secondaryButton: .cancel())
        case .cantMake(let text):
            return Alert(title: Text("Something went wrong"), message: Text(text), dismissButton: .default(Text("OK")))
        case .suspended:
            return Alert(title: Text("Resume the Mac first"),
                         message: Text("This Mac is suspended with the disks it has in. Disks can't go in or come out until it is resumed."),
                         dismissButton: .default(Text("OK")))
        case .noPortForTools:
            return Alert(title: Text("Eject a disk first"),
                         message: Text("All four USB ports are in use, and the Henge Tools disk needs one. Eject a disk here, then open the installer again."),
                         dismissButton: .default(Text("OK")))
        }
    }

    // MARK: New disk

    /// Sizes there's room for on this device (with some to spare): the disk takes room as the Mac fills it.
    private var sizes: [UInt64] {
        let home = URL(fileURLWithPath: NSHomeDirectory())
        let free = (try? home.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]).volumeAvailableCapacityForImportantUsage) ?? 0
        let room = UInt64(max(free, 0)) * 9 / 10
        return [8, 16, 32, 64, 128, 256, 512].map { UInt64($0) << 30 }.filter { $0 <= room }
    }

    private var newDisk: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Name", text: $newName)
                    Picker("Size", selection: $newSize) {
                        ForEach(sizes, id: \.self) { s in Text(ByteCountFormatter.string(fromByteCount: Int64(s), countStyle: .file)).tag(s) }
                    }
                } footer: {
                    Text("It takes room on this \(Device.name) only as the Mac fills it, and it's kept in Files, Henge, Disks. With Henge Tools on the Mac, it's set up as a Mac disk (APFS) as it arrives; without, it works as a FAT32 drive.")
                }
            }
            .navigationTitle("New Disk")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { making = false } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Make and Plug In") {
                        making = false
                        drives.make(name: newName, bytes: newSize)
                    }
                    .disabled(newSize == 0 || newName.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
        }
        .presentationDetents([.medium])
        .preferredColorScheme(.dark)
    }
}
