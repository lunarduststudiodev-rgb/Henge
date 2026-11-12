import Foundation

/// Engine settings, without a rebuild. Defaults below, then the Performance settings,
/// then Documents/henge-env.txt (KEY=VALUE per line, # comments; {Documents} in a value is that folder) overrides them all.
/// Applied at launch and again before each Mac starts, so changes need no relaunch.
enum EngineTuning {
    static let refreshKey = "henge.perf.hz"
    static let superblocksKey = "henge.perf.superblocks"
    static let wfiSpinKey = "henge.perf.wfiSpin"
    static let fastFlushKey = "henge.perf.fastFlush"
    static let highPriorityKey = "henge.perf.highPriority"
    static let balloonKey = "henge.memory.balloon"

    static let defaults: [String: String] = [
        // 60, not 120: every frame macOS draws is emulated and replayed, so 120 Hz
        // halves the CPU left for apps for animation few will notice.
        "HV_HZ": "60",         // the refresh rate macOS is told its display has
        "HV_WFI_SPIN": "50",   // microseconds an idle CPU watches for work before sleeping (1-cycle cntvct_el0)
        "HV_FLUSH": "2",       // disk flush: F_BARRIERFSYNC, ordered but without a full fsync
        // No HV_STATS: the engine's detailed statistics (a stats block in the log every N seconds, and the extra
        // probes behind it) are for diagnosis, opted into with HV_STATS=10 in henge-env.txt. The counters the
        // freeze and lag records use are counted per CPU without it.
    ]

    static func apply() {
        var env = defaults
        unsetenv("HV_STATS") // only while henge-env.txt asks for it: removing the line turns it off at the next start
        let d = UserDefaults.standard
        if [30, 120].contains(d.integer(forKey: refreshKey)) { env["HV_HZ"] = String(d.integer(forKey: refreshKey)) }
        if d.bool(forKey: superblocksKey) { env["HV_SUPERBLOCKS"] = "1" } else { unsetenv("HV_SUPERBLOCKS") }
        if let spin = d.object(forKey: wfiSpinKey) as? Bool {
            env["HV_WFI_SPIN"] = spin ? "50" : "0"
        } else {
            env["HV_WFI_SPIN"] = "50"
        }
        // The Mac's CPU threads at the user-interactive class instead of user-initiated (host/vm.c hv_vm_cpu_qos).
        if d.bool(forKey: highPriorityKey) { env["HV_QOS"] = "ui" } else { unsetenv("HV_QOS") }
        if d.object(forKey: fastFlushKey) as? Bool == false { env["HV_FLUSH"] = "0" }
        // The Mac gets a memory balloon, asking for nothing yet: MemoryWatch asks when iPadOS runs short.
        if d.bool(forKey: balloonKey) { env["HV_BALLOON"] = "0" } else { unsetenv("HV_BALLOON") }
        let docs = EnvFile.documents
        // GPU command trace: on while Documents/trace-on exists (looked for once a second, so it can be toggled live).
        env["HV_PVG_TRACE_FILE"] = docs.appendingPathComponent("trace-on").path
        env.merge(EnvFile.read()) { _, file in file }
        for (k, v) in env.sorted(by: { $0.key < $1.key }) {
            setenv(k, v, 1)
            HengeLog.log("tuning", "\(k)=\(v)")
        }
    }
}
