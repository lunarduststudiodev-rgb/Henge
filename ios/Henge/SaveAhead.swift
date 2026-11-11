import Foundation

/// Documents/henge-env.txt: KEY=VALUE per line, # comments; {Documents} in a value is that folder. EngineTuning applies
/// it over every setting; a setting that needs to know whether the file has its line reads it here, now.
enum EnvFile {
    static func parse(_ text: String, documents: String) -> [String: String] {
        var out: [String: String] = [:]
        for raw in text.split(whereSeparator: \.isNewline) {
            let line = raw.split(separator: "#", maxSplits: 1, omittingEmptySubsequences: false).first.map(String.init)?
                .trimmingCharacters(in: .whitespaces) ?? ""
            guard let eq = line.firstIndex(of: "="), eq != line.startIndex else { continue }
            // {Documents} stands for this folder: its absolute path changes when Henge is reinstalled.
            out[String(line[..<eq]).trimmingCharacters(in: .whitespaces)] = String(line[line.index(after: eq)...])
                .trimmingCharacters(in: .whitespaces).replacingOccurrences(of: "{Documents}", with: documents)
        }
        return out
    }

    static var documents: URL { FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0] }

    /// The file as it is now (empty without one), so a line added or removed counts without restarting Henge.
    static func read(in docs: URL = documents) -> [String: String] {
        guard let text = try? String(contentsOf: docs.appendingPathComponent("henge-env.txt"), encoding: .utf8) else { return [:] }
        return parse(text, documents: docs.path)
    }
}

/// Whether a start keeps the Mac's memory in files (save-ahead: Settings › Quick Suspend), and what decided it.
///
/// One rule, the one every engine setting follows (EngineTuning): a HENGE_SAVE_AHEAD line in henge-env.txt decides
/// ("1" on, anything else off), and the Settings switch shows it and can't be changed; without the line, the switch
/// decides, off until it is turned on. Above both, a Mac saved with its memory in files (a Suspend or auto-save with a
/// Mac.hvsnap.ram-* copy) goes on with it: that save loads only into memory in files.
struct SaveAheadChoice: Equatable {
    enum Source: Equatable { case savedWithIt, envFile, settings }
    let on: Bool
    let source: Source
    /// What the source that lost says, for the log: the env line's or the switch's value, when it differs.
    let overruled: String?

    static let envKey = "HENGE_SAVE_AHEAD"

    /// The env line's value as a choice: nil without the line.
    static func envValue(_ value: String?) -> Bool? { value.map { $0 == "1" } }

    /// savedInFiles: this Mac's save keeps its memory as files. envLine: HENGE_SAVE_AHEAD's value in henge-env.txt, nil
    /// without the line. setting: the switch's stored value, nil if it was never touched.
    static func decide(savedInFiles: Bool, envLine: String?, setting: Bool?) -> SaveAheadChoice {
        let env = envValue(envLine)
        let chosen = env ?? setting ?? false
        let word = { (b: Bool) in b ? "on" : "off" }
        if savedInFiles {
            let said = env.map { "henge-env.txt says \(word($0))" } ?? "the Settings switch is \(word(setting ?? false))"
            return SaveAheadChoice(on: true, source: .savedWithIt, overruled: chosen ? nil : said)
        }
        if let env {
            let differs = setting.map { $0 != env } ?? false
            return SaveAheadChoice(on: env, source: .envFile,
                                   overruled: differs ? "the Settings switch, \(word(setting ?? false)), is not used" : nil)
        }
        return SaveAheadChoice(on: chosen, source: .settings, overruled: nil)
    }

    /// The log line: what this start does and why.
    var logLine: String {
        let why: String
        switch source {
        case .savedWithIt: why = "the Mac was saved with its memory in files, and that save loads only that way"
        case .envFile: why = "\(Self.envKey)=\(on ? "1" : "0") in henge-env.txt (it overrides the Settings switch)"
        case .settings: why = "the Settings switch (Suspend › Quick Suspend)"
        }
        return "save-ahead \(on ? "on" : "off") for this start: \(why)" + (overruled.map { "; \($0)" } ?? "")
    }
}
