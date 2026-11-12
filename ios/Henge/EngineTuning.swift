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
    static let writeCacheKey = "henge.perf.writeCache"
    static let wfeSpinKey = "henge.perf.wfeSpin"
    static let tlbtKey = "henge.perf.tlbt"
    static let lightUIKey = "henge.perf.lightUI"

    static let defaults: [String: String] = [
        // 60, not 120: every frame macOS draws is emulated and replayed, so 120 Hz
        // halves the CPU left for apps for animation few will notice.
        "HV_HZ": "60",         // the refresh rate macOS is told its display has
        "HV_WFI_SPIN": "50",   // microseconds an idle CPU watches for work before sleeping (1-cycle cntvct_el0)
        "HV_FLUSH": "2",       // disk flush: F_BARRIERFSYNC, ordered but without a full fsync
        "HV_WRITE_CACHE": "1", // host disk write caching: avoids 2-3x F_NOCACHE penalty
        "HV_TLBT": "1",        // table-major TLB layout: 9-20% faster in benchmarks
        "HV_QOS": "ui",        // user-interactive CPU thread QoS for performance core scheduling
        "HV_SUPERBLOCKS": "1", // superblocks: forward conditional branches fall through in-block without ending the block
        // No HV_STATS: the engine's detailed statistics (a stats block in the log every N seconds, and the extra
        // probes behind it) are for diagnosis, opted into with HV_STATS=10 in henge-env.txt. The counters the
        // freeze and lag records use are counted per CPU without it.
    ]

    static func apply() {
        var env = defaults
        unsetenv("HV_STATS") // only while henge-env.txt asks for it: removing the line turns it off at the next start
        let d = UserDefaults.standard
        if [30, 120].contains(d.integer(forKey: refreshKey)) { env["HV_HZ"] = String(d.integer(forKey: refreshKey)) }
        if d.object(forKey: superblocksKey) as? Bool == false { unsetenv("HV_SUPERBLOCKS") } else { env["HV_SUPERBLOCKS"] = "1" }
        if let spin = d.object(forKey: wfiSpinKey) as? Bool {
            env["HV_WFI_SPIN"] = spin ? "50" : "0"
        } else {
            env["HV_WFI_SPIN"] = "50"
        }
        // The Mac's CPU threads at the user-interactive class instead of user-initiated (host/vm.c hv_vm_cpu_qos).
        if d.object(forKey: highPriorityKey) as? Bool == false { unsetenv("HV_QOS") } else { env["HV_QOS"] = "ui" }
        if d.object(forKey: fastFlushKey) as? Bool == false { env["HV_FLUSH"] = "0" }
        if d.object(forKey: writeCacheKey) as? Bool == false { env["HV_WRITE_CACHE"] = "0" }
        if d.bool(forKey: wfeSpinKey) { env["HV_WFE_SPIN"] = "1" } else { unsetenv("HV_WFE_SPIN") }
        if d.object(forKey: tlbtKey) as? Bool == false { env["HV_TLBT"] = "0" }
        if d.object(forKey: lightUIKey) as? Bool == false { unsetenv("HV_LIGHT_UI") } else { env["HV_LIGHT_UI"] = "1" }
        // The Mac gets a memory balloon, asking for nothing yet: MemoryWatch asks when iPadOS runs short.
        if d.bool(forKey: balloonKey) { env["HV_BALLOON"] = "0" } else { unsetenv("HV_BALLOON") }
        let docs = EnvFile.documents
        // GPU command trace: on while Documents/trace-on exists (looked for once a second, so it can be toggled live).
        env["HV_PVG_TRACE_FILE"] = docs.appendingPathComponent("trace-on").path
        env.merge(EnvFile.read()) { _, file in file }
        // Thermal throttle prevention: if the user or env configured > 60 Hz (e.g. 120 Hz)
        // but the device is already hot/critical, clamp refresh rate to protect M1 CPU/GPU performance.
        if let hzVal = Int(env["HV_HZ"] ?? "60"), hzVal > 60 {
            let heat = ProcessInfo.processInfo.thermalState
            if heat == .serious || heat == .critical {
                let clamped = heat == .critical ? "30" : "60"
                env["HV_HZ"] = clamped
                HengeLog.log("tuning", "thermal state is \(heat == .critical ? "Critical" : "Hot"): clamped HV_HZ from \(hzVal) to \(clamped)")
            }
        }
        for (k, v) in env.sorted(by: { $0.key < $1.key }) {
            setenv(k, v, 1)
            HengeLog.log("tuning", "\(k)=\(v)")
        }
    }
}
