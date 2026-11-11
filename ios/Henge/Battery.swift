import UIKit

/// The iPad's battery, as the Mac's own: the engine gives macOS Apple's virtual-Mac battery
/// (henge_vm_set_power), and macOS shows it in the menu bar and Battery settings, warns when it runs low,
/// and runs "only on battery" settings (Low Power Mode among them) by whether the iPad is on its charger.
/// iOS's own Low Power Mode can't be passed on: the device has no place for it.
@MainActor
enum MacBattery {
    private static var watching = false

    /// Before a Mac starts (and harmless after): sends the battery as it is now, then each change.
    static func start() {
        let device = UIDevice.current
        device.isBatteryMonitoringEnabled = true
        if !watching {
            watching = true
            let center = NotificationCenter.default
            for name in [UIDevice.batteryLevelDidChangeNotification, UIDevice.batteryStateDidChangeNotification] {
                center.addObserver(forName: name, object: nil, queue: .main) { _ in
                    MainActor.assumeIsolated { send() }
                }
            }
        }
        send()
    }

    private static var last: (Int, Int32, Int32, Int32)?

    private static func send() {
        let device = UIDevice.current
        let level = device.batteryLevel // 0...1, or -1 when iOS doesn't know (the Simulator)
        let charger: Int32, charging: Int32, full: Int32
        switch device.batteryState {
        case .unplugged: (charger, charging, full) = (0, 0, 0)
        case .charging: (charger, charging, full) = (1, 1, 0)
        case .full: (charger, charging, full) = (1, 0, 1)
        default: return // unknown: the Mac keeps what it has (full, on a charger, until told otherwise)
        }
        guard level >= 0 else { return }
        let percent = Double(level) * 100
        let now = (Int(percent.rounded()), charger, charging, full)
        if let last, last == now { return }
        last = now
        henge_vm_set_power(percent, charger, charging, full)
        HengeLog.log("battery", "\(now.0)%" + (charger == 1 ? (full == 1 ? ", full on the charger" : charging == 1 ? ", charging" : ", on the charger") : ""))
    }
}
