import Foundation

/// What this install was actually signed with: the entitlements in the project are only a
/// request, and a signing certificate without them leaves them out.
enum Entitlements {
    static let increasedMemory = has("com.apple.developer.kernel.increased-memory-limit")
    static let extendedAddressing = has("com.apple.developer.kernel.extended-virtual-addressing")
    /// What iOS allowed when the app opened, before the JIT region took its share.
    static let memoryLimitAtLaunch = henge_available_memory()

    private typealias CreateFromSelf = @convention(c) (CFAllocator?) -> CFTypeRef?
    private typealias CopyValue = @convention(c) (CFTypeRef, CFString, UnsafeMutablePointer<Unmanaged<CFError>?>?) -> CFTypeRef?

    /// get-task-allow: whether a debugger may attach, which every JIT method but TrollStore and a jailbreak needs. Nil when
    /// the signature could not be read.
    static let debuggable = value("get-task-allow")

    static func has(_ name: String) -> Bool { value(name) ?? false }

    /// The entitlement's yes or no, nil when the signature could not be read at all (a missing entitlement is a no).
    private static func value(_ name: String) -> Bool? {
        let handle = UnsafeMutableRawPointer(bitPattern: -2) // RTLD_DEFAULT
        guard let create = dlsym(handle, "SecTaskCreateFromSelf"), let copy = dlsym(handle, "SecTaskCopyValueForEntitlement") else { return nil }
        guard let task = unsafeBitCast(create, to: CreateFromSelf.self)(nil) else { return nil }
        return (unsafeBitCast(copy, to: CopyValue.self)(task, name as CFString, nil) as? NSNumber)?.boolValue ?? false
    }
}
