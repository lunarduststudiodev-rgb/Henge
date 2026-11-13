import Foundation

/// Resolves the host iPad/iPhone hardware identifier (`uname().machine`) into concise
/// marketing model and Apple Silicon chip strings passed into `VMAppleCfg` before VM creation.
/// `VMAppleCfg.model` / `soc_name` hold at most 31 UTF-8 bytes; the guest's `VirtualMac2,1`
/// `model` and `compatible` DeviceTree properties remain intact for XNU kernel driver matching.
extension Device {
    struct HostIdentity {
        let modelName: String
        let socName: String
    }

    static let hostIdentity: HostIdentity = resolveHostIdentity(identifier: HengeLog.deviceModel)

    static func resolveHostIdentity(identifier id: String) -> HostIdentity {
        if let known = knownModels[id] { return known }
        let fallbackModel: String
        if id.isEmpty || id == "arm64" || id == "x86_64" {
            fallbackModel = name
        } else {
            let tagged = "\(name) (\(id))"
            fallbackModel = tagged.utf8.count <= 31 ? tagged : String(id.prefix(31))
        }
        return HostIdentity(modelName: fallbackModel, socName: "Apple Silicon")
    }

    /// Pushes the host device identity to `VMAppleCfg` before booting or installing a Mac.
    static func applyHostIdentity() {
        let id = hostIdentity
        henge_vm_set_device_info(id.modelName, id.socName)
    }

    // MARK: - Guest memory limits

    /// Physical RAM reported by iOS in MiB (~7400–7600 MiB on an 8 GB iPad).
    static var physicalMemoryMB: UInt64 { ProcessInfo.processInfo.physicalMemory >> 20 }

    /// Whether this device can select and run a 6 GB (6144 MiB) guest without immediate jetsam:
    /// requires an iPad with >= 8 GB physical RAM (>= 7000 MiB usable) and both memory entitlements.
    static var supports6GBGuest: Bool {
        supports6GBGuest(
            isPad: isPad,
            physicalMemoryMB: physicalMemoryMB,
            increasedMemory: Entitlements.increasedMemory,
            extendedAddressing: Entitlements.extendedAddressing
        )
    }

    static func supports6GBGuest(isPad: Bool, physicalMemoryMB: UInt64,
                                 increasedMemory: Bool, extendedAddressing: Bool) -> Bool {
        isPad && physicalMemoryMB >= 7_000 && increasedMemory && extendedAddressing
    }

    /// Maximum guest RAM (in GB) selectable on this device without risking guaranteed jetsam.
    static var maxSelectableRAMGB: Int {
        maxSelectableRAMGB(
            isPad: isPad,
            physicalMemoryMB: physicalMemoryMB,
            suggestedRAMGB: suggestedRAMGB,
            increasedMemory: Entitlements.increasedMemory,
            extendedAddressing: Entitlements.extendedAddressing
        )
    }

    static func maxSelectableRAMGB(isPad: Bool, physicalMemoryMB: UInt64, suggestedRAMGB: Int,
                                   increasedMemory: Bool, extendedAddressing: Bool) -> Int {
        guard increasedMemory && extendedAddressing else { return min(suggestedRAMGB, 4) }
        if physicalMemoryMB >= 14_000 { return memoryChoices.upperBound }
        if physicalMemoryMB >= 10_000 { return isPad ? 8 : 6 }
        if isPad && physicalMemoryMB >= 7_000 { return 6 }
        return suggestedRAMGB
    }

    /// Suggested RAM clamped to what this device and its signed entitlements can actually select.
    static var clampedSuggestedRAMGB: Int {
        min(suggestedRAMGB, maxSelectableRAMGB)
    }

    /// Selectable guest RAM choices in GB for this device and its signed entitlements.
    static var selectableMemoryChoices: ClosedRange<Int> {
        1...max(1, maxSelectableRAMGB)
    }

    private static let knownModels: [String: HostIdentity] = [
        // iPad Pro (M1, 2021)
        "iPad13,4": HostIdentity(modelName: "iPad Pro (11-inch, M1)", socName: "Apple M1"),
        "iPad13,5": HostIdentity(modelName: "iPad Pro (11-inch, M1)", socName: "Apple M1"),
        "iPad13,6": HostIdentity(modelName: "iPad Pro (11-inch, M1)", socName: "Apple M1"),
        "iPad13,7": HostIdentity(modelName: "iPad Pro (11-inch, M1)", socName: "Apple M1"),
        "iPad13,8": HostIdentity(modelName: "iPad Pro (12.9-inch, M1)", socName: "Apple M1"),
        "iPad13,9": HostIdentity(modelName: "iPad Pro (12.9-inch, M1)", socName: "Apple M1"),
        "iPad13,10": HostIdentity(modelName: "iPad Pro (12.9-inch, M1)", socName: "Apple M1"),
        "iPad13,11": HostIdentity(modelName: "iPad Pro (12.9-inch, M1)", socName: "Apple M1"),
        // iPad Air (M1, 2022) & iPad (10th gen)
        "iPad13,16": HostIdentity(modelName: "iPad Air (5th gen, M1)", socName: "Apple M1"),
        "iPad13,17": HostIdentity(modelName: "iPad Air (5th gen, M1)", socName: "Apple M1"),
        "iPad13,18": HostIdentity(modelName: "iPad (10th gen)", socName: "Apple A14 Bionic"),
        "iPad13,19": HostIdentity(modelName: "iPad (10th gen)", socName: "Apple A14 Bionic"),
        // iPad mini (6th gen)
        "iPad14,1": HostIdentity(modelName: "iPad mini (6th gen)", socName: "Apple A15 Bionic"),
        "iPad14,2": HostIdentity(modelName: "iPad mini (6th gen)", socName: "Apple A15 Bionic"),
        // iPad Pro (M2, 2022)
        "iPad14,3": HostIdentity(modelName: "iPad Pro (11-inch, M2)", socName: "Apple M2"),
        "iPad14,4": HostIdentity(modelName: "iPad Pro (11-inch, M2)", socName: "Apple M2"),
        "iPad14,5": HostIdentity(modelName: "iPad Pro (12.9-inch, M2)", socName: "Apple M2"),
        "iPad14,6": HostIdentity(modelName: "iPad Pro (12.9-inch, M2)", socName: "Apple M2"),
        // iPad Air (M2, 2024)
        "iPad14,8": HostIdentity(modelName: "iPad Air (11-inch, M2)", socName: "Apple M2"),
        "iPad14,9": HostIdentity(modelName: "iPad Air (11-inch, M2)", socName: "Apple M2"),
        "iPad14,10": HostIdentity(modelName: "iPad Air (13-inch, M2)", socName: "Apple M2"),
        "iPad14,11": HostIdentity(modelName: "iPad Air (13-inch, M2)", socName: "Apple M2"),
        // iPad Air (M3, 2025) & iPad (A16)
        "iPad15,3": HostIdentity(modelName: "iPad Air (11-inch, M3)", socName: "Apple M3"),
        "iPad15,4": HostIdentity(modelName: "iPad Air (11-inch, M3)", socName: "Apple M3"),
        "iPad15,5": HostIdentity(modelName: "iPad Air (13-inch, M3)", socName: "Apple M3"),
        "iPad15,6": HostIdentity(modelName: "iPad Air (13-inch, M3)", socName: "Apple M3"),
        "iPad15,7": HostIdentity(modelName: "iPad (A16)", socName: "Apple A16"),
        "iPad15,8": HostIdentity(modelName: "iPad (A16)", socName: "Apple A16"),
        // iPad mini (A17 Pro), iPad Pro (M4, 2024), iPad Air (M4, 2026)
        "iPad16,1": HostIdentity(modelName: "iPad mini (A17 Pro)", socName: "Apple A17 Pro"),
        "iPad16,2": HostIdentity(modelName: "iPad mini (A17 Pro)", socName: "Apple A17 Pro"),
        "iPad16,3": HostIdentity(modelName: "iPad Pro (11-inch, M4)", socName: "Apple M4"),
        "iPad16,4": HostIdentity(modelName: "iPad Pro (11-inch, M4)", socName: "Apple M4"),
        "iPad16,5": HostIdentity(modelName: "iPad Pro (13-inch, M4)", socName: "Apple M4"),
        "iPad16,6": HostIdentity(modelName: "iPad Pro (13-inch, M4)", socName: "Apple M4"),
        "iPad16,8": HostIdentity(modelName: "iPad Air (11-inch, M4)", socName: "Apple M4"),
        "iPad16,9": HostIdentity(modelName: "iPad Air (11-inch, M4)", socName: "Apple M4"),
        "iPad16,10": HostIdentity(modelName: "iPad Air (13-inch, M4)", socName: "Apple M4"),
        "iPad16,11": HostIdentity(modelName: "iPad Air (13-inch, M4)", socName: "Apple M4"),
        // iPad Pro (M5, 2025)
        "iPad17,1": HostIdentity(modelName: "iPad Pro (11-inch, M5)", socName: "Apple M5"),
        "iPad17,2": HostIdentity(modelName: "iPad Pro (11-inch, M5)", socName: "Apple M5"),
        "iPad17,3": HostIdentity(modelName: "iPad Pro (13-inch, M5)", socName: "Apple M5"),
        "iPad17,4": HostIdentity(modelName: "iPad Pro (13-inch, M5)", socName: "Apple M5"),
        // iPhones (A14–A19)
        "iPhone13,1": HostIdentity(modelName: "iPhone 12 mini", socName: "Apple A14 Bionic"),
        "iPhone13,2": HostIdentity(modelName: "iPhone 12", socName: "Apple A14 Bionic"),
        "iPhone13,3": HostIdentity(modelName: "iPhone 12 Pro", socName: "Apple A14 Bionic"),
        "iPhone13,4": HostIdentity(modelName: "iPhone 12 Pro Max", socName: "Apple A14 Bionic"),
        "iPhone14,2": HostIdentity(modelName: "iPhone 13 Pro", socName: "Apple A15 Bionic"),
        "iPhone14,3": HostIdentity(modelName: "iPhone 13 Pro Max", socName: "Apple A15 Bionic"),
        "iPhone14,4": HostIdentity(modelName: "iPhone 13 mini", socName: "Apple A15 Bionic"),
        "iPhone14,5": HostIdentity(modelName: "iPhone 13", socName: "Apple A15 Bionic"),
        "iPhone14,6": HostIdentity(modelName: "iPhone SE (3rd gen)", socName: "Apple A15 Bionic"),
        "iPhone14,7": HostIdentity(modelName: "iPhone 14", socName: "Apple A15 Bionic"),
        "iPhone14,8": HostIdentity(modelName: "iPhone 14 Plus", socName: "Apple A15 Bionic"),
        "iPhone15,2": HostIdentity(modelName: "iPhone 14 Pro", socName: "Apple A16 Bionic"),
        "iPhone15,3": HostIdentity(modelName: "iPhone 14 Pro Max", socName: "Apple A16 Bionic"),
        "iPhone15,4": HostIdentity(modelName: "iPhone 15", socName: "Apple A16 Bionic"),
        "iPhone15,5": HostIdentity(modelName: "iPhone 15 Plus", socName: "Apple A16 Bionic"),
        "iPhone16,1": HostIdentity(modelName: "iPhone 15 Pro", socName: "Apple A17 Pro"),
        "iPhone16,2": HostIdentity(modelName: "iPhone 15 Pro Max", socName: "Apple A17 Pro"),
        "iPhone17,1": HostIdentity(modelName: "iPhone 16 Pro", socName: "Apple A18 Pro"),
        "iPhone17,2": HostIdentity(modelName: "iPhone 16 Pro Max", socName: "Apple A18 Pro"),
        "iPhone17,3": HostIdentity(modelName: "iPhone 16", socName: "Apple A18"),
        "iPhone17,4": HostIdentity(modelName: "iPhone 16 Plus", socName: "Apple A18"),
        "iPhone17,5": HostIdentity(modelName: "iPhone 16e", socName: "Apple A18"),
        "iPhone18,1": HostIdentity(modelName: "iPhone 17 Pro", socName: "Apple A19 Pro"),
        "iPhone18,2": HostIdentity(modelName: "iPhone 17 Pro Max", socName: "Apple A19 Pro"),
        "iPhone18,3": HostIdentity(modelName: "iPhone 17", socName: "Apple A19"),
        "iPhone18,4": HostIdentity(modelName: "iPhone Air", socName: "Apple A19 Pro"),
    ]
}
