// Save-ahead's source of truth (ios/Henge/SaveAhead.swift): henge-env.txt's HENGE_SAVE_AHEAD line decides over the
// Settings switch, the switch decides without it, a Mac saved with its memory in files always goes on with it, the log
// line names what decided, and the file is read as it is when a Mac starts.
import Foundation

@main
enum TestSaveAhead {
    static var checks = 0, failures = 0
    static func check(_ ok: Bool, _ what: String, line: Int = #line) {
        checks += 1
        if !ok { failures += 1; print("  FAIL line \(line): \(what)") }
    }

    static func expect(_ saved: Bool, _ env: String?, _ setting: Bool?, on: Bool, _ source: SaveAheadChoice.Source,
                overruled: Bool, logs: String, line: Int = #line) {
        let c = SaveAheadChoice.decide(savedInFiles: saved, envLine: env, setting: setting)
        let what = "saved \(saved) env \(env ?? "none") switch \(setting.map { "\($0)" } ?? "never set")"
        check(c.on == on, "\(what): on \(c.on), wanted \(on)", line: line)
        check(c.source == source, "\(what): source \(c.source), wanted \(source)", line: line)
        check((c.overruled != nil) == overruled, "\(what): overruled \(c.overruled ?? "nil")", line: line)
        check(c.logLine.contains(logs), "\(what): log \"\(c.logLine)\" lacks \"\(logs)\"", line: line)
    }

    static func main() {
        print("the switch decides without the env line")
        expect(false, nil, nil, on: false, .settings, overruled: false, logs: "save-ahead off for this start: the Settings switch")
        expect(false, nil, false, on: false, .settings, overruled: false, logs: "the Settings switch")
        expect(false, nil, true, on: true, .settings, overruled: false, logs: "save-ahead on for this start: the Settings switch")

        print("the env line decides over the switch, and the log says the switch lost")
        // The iPad's case on 9 Nov: HENGE_SAVE_AHEAD=1 in the file, the switch turned off. The switch used to win and every
        // start logged only "save-ahead off".
        expect(false, "1", false, on: true, .envFile, overruled: true, logs: "save-ahead on for this start: HENGE_SAVE_AHEAD=1 in henge-env.txt")
        expect(false, "0", true, on: false, .envFile, overruled: true, logs: "HENGE_SAVE_AHEAD=0 in henge-env.txt")
        expect(false, "1", nil, on: true, .envFile, overruled: false, logs: "HENGE_SAVE_AHEAD=1")
        expect(false, "1", true, on: true, .envFile, overruled: false, logs: "HENGE_SAVE_AHEAD=1")
        expect(false, "", true, on: false, .envFile, overruled: true, logs: "HENGE_SAVE_AHEAD=0") // any value but 1 is off
        check(SaveAheadChoice.decide(savedInFiles: false, envLine: "1", setting: false).logLine.contains("the Settings switch, off, is not used"),
              "the log names the switch's losing value")

        print("a Mac saved with its memory in files goes on with it, whatever the others say")
        expect(true, "0", true, on: true, .savedWithIt, overruled: true, logs: "save-ahead on for this start: the Mac was saved with its memory in files")
        expect(true, nil, false, on: true, .savedWithIt, overruled: true, logs: "the Settings switch is off")
        expect(true, "0", nil, on: true, .savedWithIt, overruled: true, logs: "henge-env.txt says off")
        expect(true, "1", false, on: true, .savedWithIt, overruled: false, logs: "the Mac was saved")
        expect(true, nil, true, on: true, .savedWithIt, overruled: false, logs: "the Mac was saved")

        print("the file is read the way EngineTuning reads it, as it is now")
        let parsed = EnvFile.parse("""
            # comment
            HENGE_SAVE_AHEAD = 1   # trial
            HV_TRACE={Documents}/t.log
            =nokey
            no equals
            EMPTY=
            """, documents: "/docs")
        check(parsed["HENGE_SAVE_AHEAD"] == "1", "spaces and a trailing comment: \(parsed["HENGE_SAVE_AHEAD"] ?? "nil")")
        check(parsed["HV_TRACE"] == "/docs/t.log", "{Documents}: \(parsed["HV_TRACE"] ?? "nil")")
        check(parsed["EMPTY"] == "" && parsed.count == 3, "lines without a key are skipped: \(parsed)")

        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("test_save_ahead-\(getpid())")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let file = dir.appendingPathComponent("henge-env.txt")
        check(EnvFile.read(in: dir)[SaveAheadChoice.envKey] == nil, "no file: no line")
        try! "HENGE_SAVE_AHEAD=1\n".write(to: file, atomically: true, encoding: .utf8)
        check(EnvFile.read(in: dir)[SaveAheadChoice.envKey] == "1", "the line, once written")
        try! "# HENGE_SAVE_AHEAD=1\n".write(to: file, atomically: true, encoding: .utf8)
        check(EnvFile.read(in: dir)[SaveAheadChoice.envKey] == nil, "the line commented out counts at once (no relaunch)")

        print("\(checks) checks, \(failures) failures")
        exit(failures == 0 ? 0 : 1)
    }
}
