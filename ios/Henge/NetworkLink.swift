import Foundation
import Network

/// The Mac's Ethernet cable, plugged into the iPad's own connection: when the iPad can't reach anything
/// (airplane mode, Wi-Fi gone with no cellular) macOS sees its cable unplugged, and when the iPad has a network
/// again the cable goes back in and macOS asks for an address at once. The engine debounces it (a few seconds
/// out, a second back in), so a hand-over from Wi-Fi to cellular never shows on the Mac.
enum NetworkLink {
    private static let monitor = NWPathMonitor()
    private static let queue = DispatchQueue(label: "com.henge.network-link", qos: .utility)
    private static var started = false
    private static var last: Bool?

    /// From the app's launch, before any Mac starts, so one started in airplane mode starts unplugged.
    static func start() {
        guard !started else { return }
        started = true
        monitor.pathUpdateHandler = { path in
            let up = path.status == .satisfied
            if last != up {
                last = up
                HengeLog.log("network", "the iPad's network is \(up ? "up" : "down") (\(path.status)): the Mac's cable follows it")
            }
            henge_vm_host_network(up ? 1 : 0)
        }
        monitor.start(queue: queue)
    }
}
