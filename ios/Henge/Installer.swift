import Foundation
import UIKit

/// A macOS restore image Apple publishes for virtual Macs (VirtualMac2,1).
struct MacOSRelease: Identifiable, Hashable {
    let id: String        // build
    let name: String
    let url: URL
    let bytes: Int64
    let note: String

    /// The builds offered, oldest first. Apple's catalog (mesu.apple.com) only lists the newest;
    /// these are the same images from Apple's CDN, and Apple still signs them for virtual Macs.
    static let all: [MacOSRelease] = [
        /* macOS Monterey 12.6 (21G115, 2022FallFCS/fullrestores/012-40537/0EC7C669-13E9-49FB-BD64-9EECC1D174B2) installs and boots,
           but its GPU driver (protocol 0x1f) never starts WindowServer's GPU work under Henge yet: not offered until it does. */
        MacOSRelease(id: "22G120", name: "macOS Ventura 13.6",
                     url: URL(string: "https://updates.cdn-apple.com/2023FallFCS/fullrestores/042-55833/C0830847-A2F8-458F-B680-967991820931/UniversalMac_13.6_22G120_Restore.ipsw")!,
                     bytes: 12_297 << 20, note: "The smallest download (and about 35 GB in all), for devices short on storage."),
        MacOSRelease(id: "23G93", name: "macOS Sonoma 14.6.1",
                     url: URL(string: "https://updates.cdn-apple.com/2024SummerFCS/fullrestores/062-52859/932E0A8F-6644-4759-82DA-F8FA8DEA806A/UniversalMac_14.6.1_23G93_Restore.ipsw")!,
                     bytes: 14_106 << 20, note: "A smaller download than Sequoia (and about 40 GB in all)."),
        MacOSRelease(id: "24G90", name: "macOS Sequoia 15.6.1",
                     url: URL(string: "https://updates.cdn-apple.com/2025SummerFCS/fullrestores/093-10809/CFD6DD38-DAF0-40DA-854F-31AAD1294C6F/UniversalMac_15.6.1_24G90_Restore.ipsw")!,
                     bytes: 16_035 << 20, note: "Recommended: the version Henge is tested with."),
        MacOSRelease(id: "25G83", name: "macOS Tahoe 26.6.2",
                     url: URL(string: "https://updates.cdn-apple.com/2026SummerFCS/fullrestores/140-75212/A2A24B94-1FC1-45A3-93F7-C51B02AF1F4D/UniversalMac_26.6.2_25G83_Restore.ipsw")!,
                     bytes: 18_856 << 20, note: "Not tested with Henge yet."),
        MacOSRelease(id: "26B123", name: "macOS 27.2",
                     url: URL(string: "https://updates.cdn-apple.com/2026FallFCS/c9671975-52db-4fb5-9af2-00217169ba8d/UniversalMac_27.2_26B123_Restore.ipsw")!,
                     bytes: 24_409 << 20, note: "Has macOS's own touch, without the touch-sized menus and window buttons."),
        // The first build for virtual Macs with the touchscreen MacBook Pro's AppKit (its 27.2, 26B7124, is for
        // that Mac only): touch-sized menu items and window buttons.
        MacOSRelease(id: "26C5035r", name: "macOS 27.3 beta",
                     url: URL(string: "https://updates.cdn-apple.com/2026FallSeed/53df7654-4865-4259-8dae-30177b76f104/UniversalMac_27.3_26C5035r_Restore.ipsw")!,
                     bytes: 25_946 << 20, note: "Beta. The touchscreen MacBook Pro's touch: bigger menus and window buttons. Not tested with Henge yet."),
    ]

    /// Selected in the Install tab when nothing is downloaded yet: the one Henge is tested with most.
    static let recommended = all.first { $0.id == "24G90" } ?? all[0]

    /// The least memory (MB) the Mac gets while this macOS installs. Sequoia and Tahoe restore with 2 GB. macOS 27's
    /// iBoot panics with 3 GB before the restore starts (#42, 27.2 on an 8 GB iPad) and installs with 4 GB (#26), so it
    /// gets 4 GB, as later builds do. (Apple's manifests ask 4 GB of every virtual Mac, VirtualMachineMinMemorySizeMB, even
    /// 15.6.1's, so that value can't tell them apart.) Builds are numbered one ahead: 24 is macOS 15, 26 is macOS 27.
    var restoreMinMB: Int { (Int(id.prefix(2)) ?? 99) >= 26 ? 4096 : 2048 }
}

/// Downloads a restore image and installs it on a new virtual Mac, on the
/// iPhone: the machine boots its ROM in DFU mode and the restore runs over its
/// USB port, as Virtualization.framework's installer does on a Mac.
@MainActor
final class Installer: NSObject, ObservableObject {
    static let shared = Installer()

    enum Phase: Equatable {
        case idle
        case downloading
        case preparing
        case installing
        /// Downloaded, and Henge restarts to install once the Mac running in this launch is shut down (Relaunch).
        case waitingForMac
        case done(String)
        case failed(String)
    }

    @Published private(set) var phase: Phase = .idle
    @Published private(set) var step: Int = 0
    @Published private(set) var stepProgress: Double = 0
    @Published private(set) var machineName = ""
    /// The build an install is using while it runs, so Downloads doesn't delete it.
    @Published private(set) var imageInUse: String?
    /* Background time, so a moment out of the foreground (a notification, iPhone Mirroring reconnecting)
       does not suspend the restore at once; iOS gives about 30 s. */
    private var grace = UIBackgroundTaskIdentifier.invalid

    var busy: Bool {
        switch phase {
        case .downloading, .preparing, .installing, .waitingForMac: return true
        default: return false
        }
    }

    nonisolated static var imageDirectory: URL { Machines.root.appendingPathComponent("Restore images", isDirectory: true) }
    nonisolated static func imagePath(_ r: MacOSRelease) -> URL { imageDirectory.appendingPathComponent("\(r.id).ipsw") }

    /// The size of the image on this iPhone, if a whole one is there (the listed sizes are rounded).
    static func imageSize(_ r: MacOSRelease) -> Int64? {
        let values = try? imagePath(r).resourceValues(forKeys: [.fileSizeKey])
        guard let size = values?.fileSize, Int64(size) > r.bytes / 2 else { return nil }
        return Int64(size)
    }

    static func haveImage(_ r: MacOSRelease) -> Bool { imageSize(r) != nil }

    /// The whole image, or the part of one downloaded so far.
    static func deleteImage(_ r: MacOSRelease) { deleteImage(r.id) }

    static func deleteImage(_ id: String) {
        ImageDownloads.shared.delete(id)
        try? FileManager.default.removeItem(at: imageDirectory.appendingPathComponent("\(id).ipsw"))
        RestoreImages.shared.refresh()
    }

    /// Free space an install takes: the new Mac's disk, and what is left to download.
    static func spaceNeeded(_ r: MacOSRelease) -> Int64 {
        let left = haveImage(r) ? 0 : max(r.bytes - (ImageDownloads.shared.status[r.id]?.received ?? 0), 0)
        return left + newMacBytes
    }
    private static let newMacBytes: Int64 = 24 << 30

    // MARK: Memory for the restore

    /// About 6 GB or less (6 GB iPhones report ~5.6 GB). There a restore given 4 GB, with the 1 GB JIT region on top,
    /// leaves iOS too little and the phone stalls or closes Henge while macOS is written (#13), so the restore gets
    /// only the least its macOS needs. 8 GB devices report ~7.5 GB.
    private static let lowMemoryDevice = ProcessInfo.processInfo.physicalMemory >> 20 < 7_000

    /// The memory (MB) the Mac gets while it installs, whatever it is set to use afterwards: at least what its macOS needs,
    /// at most 4 GB (the restore system caches the ~15 GB it writes, so it fills all the memory it is given, and 10 GB of
    /// a 12 GB iPhone's RAM in use, outside Henge's footprint so iOS does not stop it, stalls the whole phone two thirds of
    /// the way through), and only the least on a low-memory device.
    static func restoreRAMMB(_ r: MacOSRelease, asked: Int) -> Int {
        let least = r.restoreMinMB
        return lowMemoryDevice ? least : min(max(asked, least), max(4096, least))
    }

    /// Why this device can't give the Mac the memory this macOS needs to install, said before anything is downloaded;
    /// nil when it can, or when that can't be known in this launch (a Mac has run in it: the install happens after a restart).
    /// `serious`: it certainly can't (JIT is on, so the room measured is the room there is); otherwise a warning.
    static func memoryProblem(_ r: MacOSRelease) -> (text: String, serious: Bool)? {
        let need = r.restoreMinMB
        let others = MacOSRelease.all.filter { $0.restoreMinMB < need }.map { $0.name.replacingOccurrences(of: "macOS ", with: "") }
        let instead = others.isEmpty ? "" : " " + ListFormatter.localizedString(byJoining: others) + " need\(others.count == 1 ? "s" : "") only \(MacOSRelease.all.map(\.restoreMinMB).min()! / 1024) GB."
        if !Relaunch.needed {
            // The guest memory is one mapping; the JIT region, once claimed, takes about its own size out of the room for it.
            let jit = JITBootstrap.ready ? 0 : UInt64(JITBootstrap.jitBytes >> 20)
            let reach = henge_largest_mapping_mb()
            let room = reach > 512 + jit ? (reach - 512 - jit) / 512 * 512 : 0
            if room < UInt64(need) {
                let limit = Int((Double(Entitlements.memoryLimitAtLaunch) / Double(1 << 30)).rounded())
                let why = Entitlements.increasedMemory || limit == 0 ? ""
                    : " Henge was signed without the increased-memory entitlement (a free Apple ID can't give it), which keeps it to about \(limit) GB."
                // Before JIT is on this is an estimate (the region's place in memory varies), so it only warns.
                let can = JITBootstrap.ready ? "can get only" : "will likely get only"
                return ("\(r.name) needs \(need / 1024) GB of memory for the Mac while it installs, and Henge \(can) \(String(format: "%.1f", Double(room) / 1024)) GB on this \(Device.name)." + why + instead, JITBootstrap.ready)
            }
        }
        if lowMemoryDevice && need > 2048 {
            let gb = Int((Double(ProcessInfo.processInfo.physicalMemory) / Double(1 << 30)).rounded())
            return ("\(r.name) needs \(need / 1024) GB of memory for the Mac while it installs. On this \(gb) GB \(Device.name) that leaves iOS very little, and it may close Henge partway through." + instead, false)
        }
        return nil
    }

    func reset() { if !busy { phase = .idle } }

    private func endGrace() {
        guard grace != .invalid else { return }
        UIApplication.shared.endBackgroundTask(grace)
        grace = .invalid
    }

    func start(_ release: MacOSRelease, name: String, ramMB: Int, cpus: Int, keepImage: Bool) {
        guard !busy else { return }
        machineName = name
        imageInUse = release.id
        UIApplication.shared.isIdleTimerDisabled = true // the download goes on without Henge, but the restore needs it open
        grace = UIApplication.shared.beginBackgroundTask(withName: "Henge install") {
            MainActor.assumeIsolated { Installer.shared.endGrace() }
        }
        Task {
            defer {
                UIApplication.shared.isIdleTimerDisabled = false
                imageInUse = nil
                RestoreImages.shared.refresh()
                endGrace()
            }
            var made: VirtualMac?
            do {
                try checkSpace(release)
                if let problem = Self.memoryProblem(release), problem.serious { throw message(problem.text) } // before the long download
                let rom = try await BootROM.file() // before the long download, so a problem shows up at once
                HengeLog.log("install", "boot ROM ready")
                let ipsw = try await download(release)
                if Relaunch.needed {
                    // A Mac has run in this launch: the install runs in a fresh Henge, which finds the download there.
                    // A Mac still on is never cut short: Henge restarts once it has shut down.
                    if henge_vm_state() == 1 {
                        phase = .waitingForMac
                        HengeLog.log("install", "downloaded; waiting for the running Mac to shut down before restarting")
                        while henge_vm_state() == 1 { try await Task.sleep(nanoseconds: 1_000_000_000) }
                    }
                    phase = .idle
                    Relaunch.shared.install(release, name: name, ramMB: ramMB, cpus: cpus, keepImage: keepImage)
                    return
                }
                phase = .preparing
                var mac = try prepare(name: name, ramMB: ramMB, rom: rom)
                mac.osName = release.name
                made = mac
                phase = .installing
                try await install(mac, release: release, ipsw: ipsw, cpus: cpus)
                if !keepImage { try? FileManager.default.removeItem(at: ipsw) }
                HengeLog.log("install", "\(name): installed \(release.name)")
                phase = .done("\(release.name) is installed on \(name). Start it from the Macs tab.")
            } catch {
                HengeLog.log("install", "failed: \(error.localizedDescription)")
                // A half-installed Mac is no use, and its disk can be many gigabytes. The download stays.
                if let made, henge_vm_state() != 1 { try? FileManager.default.removeItem(at: made.directory) }
                phase = .failed(error.localizedDescription)
            }
        }
    }

    private func checkSpace(_ r: MacOSRelease) throws {
        let free = (try? URL(fileURLWithPath: NSHomeDirectory()).resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?
            .volumeAvailableCapacityForImportantUsage ?? 0
        let needed = Self.spaceNeeded(r)
        if free > 0 && free < needed {
            throw message("This install needs about \(ByteText.string(UInt64(needed))) free; this \(Device.name) has \(ByteText.string(UInt64(free))).")
        }
    }

    // MARK: Download

    /// The image, once ImageDownloads has all of it. It downloads in the background and can be paused;
    /// this waits through a pause.
    private func download(_ r: MacOSRelease) async throws -> URL {
        let dest = Self.imagePath(r)
        if Self.haveImage(r) {
            HengeLog.log("install", "using the downloaded \(r.name)")
            return dest
        }
        phase = .downloading
        try await ImageDownloads.shared.download(r)
        return dest
    }

    // MARK: The new machine

    private func prepare(name: String, ramMB: Int, rom: URL) throws -> VirtualMac {
        // A new machine identity, as VZMacMachineIdentifier makes one: a random ECID.
        let ecid = UInt64.random(in: 1 << 40 ..< 1 << 62)
        var mac = VirtualMac(name: name, ecid: ecid, seed: 0, ramMB: ramMB, diskBytes: 64 << 30)
        (mac.screenWidth, mac.screenHeight) = Device.fittingScreen
        mac.retina = true // on an iPad; elsewhere the fitting screen
        let fm = FileManager.default
        try fm.createDirectory(at: mac.directory, withIntermediateDirectories: true)
        Machines.excludeFromBackup(Machines.root)
        try? fm.removeItem(at: mac.rom)
        try fm.copyItem(at: rom, to: mac.rom)
        guard fm.createFile(atPath: mac.disk.path, contents: nil) else { throw message("Cannot create the disk.") }
        let h = try FileHandle(forWritingTo: mac.disk)
        try h.truncate(atOffset: UInt64(64) << 30) // sparse: blocks are only used as macOS writes them
        try h.close()
        guard henge_make_aux(mac.aux.path) == 0 else { throw message("Cannot create the auxiliary storage.") }
        let info: [String: Any] = ["name": name, "ecid": ecid, "seed": 0, "ramMB": ramMB]
        try JSONSerialization.data(withJSONObject: info).write(to: mac.directory.appendingPathComponent("machine.json"))
        mac.diskBytes = 64 << 30
        HengeLog.log("install", "new machine \(name): ECID \(ecid), 64 GB disk")
        return mac
    }

    // MARK: The restore

    private func install(_ mac: VirtualMac, release: MacOSRelease, ipsw: URL, cpus: Int) async throws {
        guard JITBootstrap.ready else { throw message("Turn on JIT first: the installer runs the Mac's own restore system, which needs it.") }
        var restoring = mac
        restoring.ramMB = Self.restoreRAMMB(release, asked: mac.ramMB)
        let ram = GuestMemory.prepare(restoring)
        guard ram >= UInt64(release.restoreMinMB) else {
            // Less than its macOS needs: 27's iBoot panics before the restore starts (#42).
            throw message(Self.memoryProblem(release).flatMap { $0.serious ? $0.text : nil }
                          ?? "Installing needs a few more GB of free storage for the Mac's memory.")
        }
        setenv("HV_WRITE_CACHE", "0", 1) // the ~15 GB the restore writes goes straight to storage (host/disk.c); this process runs no other Mac
        let work = mac.directory.appendingPathComponent("restore", isDirectory: true)
        try FileManager.default.createDirectory(at: work, withIntermediateDirectories: true)
        var buffer = [CChar](repeating: 0, count: 256)
        let rc = henge_vm_install(mac.rom.path, mac.disk.path, mac.aux.path, mac.ecid, ipsw.path, work.path, ram, Int32(cpus),
                                  &buffer, buffer.count)
        if rc != 0 { throw message(String(cString: buffer)) }
        while true {
            try await Task.sleep(nanoseconds: 1_000_000_000)
            step = Int(henge_install_step())
            stepProgress = henge_install_progress()
            if step == 100 { break }
            if step == 101 { throw message("The restore failed. The log (Settings, or Documents/henge.log) says why.") }
            if henge_vm_state() == 2 && step < 100 {
                // The restore ends with the machine restarting, and idevicerestore finishes a
                // little after; a stop with no result after that is a failure.
                for _ in 0..<90 where henge_install_step() < 100 { try await Task.sleep(nanoseconds: 1_000_000_000) }
                if henge_install_step() == 100 { break }
                throw message("The machine stopped during the restore (\(String(cString: henge_vm_exit_reason()))).")
            }
        }
        // In the list before anything else: stopping the restore system can take a while, and if Henge is
        // stopped meanwhile (the phone locked) a finished Mac must not be left on the disk unlisted.
        try? (mac.osName ?? "").write(to: mac.directory.appendingPathComponent(Machines.installedMarker), atomically: true, encoding: .utf8)
        Machines.shared.add(mac)
        HengeLog.log("install", "\(mac.name): restore finished, added to the Macs")
        if henge_vm_state() == 1 { henge_vm_stop() }
        try? FileManager.default.removeItem(at: work)
    }

    static func stepName(_ step: Int) -> String {
        switch step {
        case 0: return "Finding the Mac"
        case 1: return "Starting the installer"
        case 2: return "Writing macOS"
        case 3: return "Checking macOS"
        case 4: return "Installing firmware"
        case 6, 7: return "Installing system files"
        default: return "Installing"
        }
    }

    private func message(_ text: String) -> NSError {
        NSError(domain: "Henge", code: 2, userInfo: [NSLocalizedDescriptionKey: text])
    }
}
