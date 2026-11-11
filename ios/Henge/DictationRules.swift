import Foundation

/// Dictation's choices that need neither the microphone nor the screen (Dictation.swift), kept apart so they are
/// checked on the Mac: tests/test_dictation_rules.swift, `make test-dictation`.
enum DictationRules {
    /// The engine's language. English only: the Mac's keyboard types plain ASCII (Keys.type), so French or
    /// Spanish words would lose their accented letters. The person's own English (British, Indian…) when the
    /// iPad is set to one, U.S. English otherwise.
    static func englishLocale(for current: Locale) -> Locale {
        isEnglish(current.identifier) ? current : Locale(identifier: "en_US")
    }

    static func isEnglish(_ identifier: String) -> Bool {
        let language = identifier.lowercased().prefix { $0.isLetter }
        return language == "en"
    }

    /// The marks that end a word: a phrase's closing ones wait in `Joiner.held`.
    static let marks: Set<Character> = [".", ",", ";", ":", "!", "?"]

    /// Joins the engine's settled phrases into what is typed at the Mac's cursor. Nothing typed is taken back, so
    /// each phrase's closing marks wait (`held`) for the next phrase: the engine ends every phrase it settles as a
    /// sentence, and a person who goes on after the pause with "comma" means the comma, not the full stop.
    struct Joiner {
        /// The last character typed; nil before the first phrase.
        private(set) var last: Character?
        /// The last phrase's closing marks, not typed yet.
        private(set) var held = ""

        /// What one settled phrase types now: nil when nothing (yet).
        mutating func add(_ phrase: String) -> String? {
            let text = DictationRules.spokenPunctuation(DictationRules.tidy(DictationRules.typeable(phrase)))
            guard !text.isEmpty else { return nil }
            // Marks it starts with come from a spoken "comma" or "period" after the pause: they replace the held ones.
            let lead = String(text.prefix { DictationRules.marks.contains($0) })
            var rest = String(text.dropFirst(lead.count).drop { $0 == " " })
            guard !rest.isEmpty else {
                held = lead
                return nil
            }
            let tail = String(rest.reversed().prefix { DictationRules.marks.contains($0) }.reversed())
            rest.removeLast(tail.count)
            var out = lead.isEmpty ? held : lead
            held = tail
            if !rest.isEmpty {
                let before = out.last ?? last
                if let before {
                    // The engine starts each phrase as a sentence: a capital only where one starts.
                    if ",;:".contains(before) { rest = DictationRules.lowercasingStart(rest) }
                    if ".!?\n".contains(before) { rest = DictationRules.capitalizingStart(rest) }
                    if !before.isWhitespace, rest.first != "\n" { out += " " }
                }
                out += rest
            }
            guard let end = out.last else { return nil }
            last = end
            return out
        }

        /// What waits when dictation stops: the last phrase's closing marks.
        mutating func end() -> String? {
            let out = held
            held = ""
            guard let end = out.last else { return nil }
            last = end
            return out
        }
    }

    /// The engine's own stray marks. A phrase starts with a word, so marks before it are what's left of a cut
    /// sentence (". and I bought", ",... I came home"); a run of them inside becomes the one that means most, and
    /// a hesitation's "..." goes (at the end, it's a full stop).
    static func tidy(_ text: String) -> String {
        let words = text.split(whereSeparator: \.isWhitespace)
        var pieces: [String] = []
        for (index, word) in words.enumerated() {
            var out = ""
            var run = ""
            func flush(atEnd: Bool) {
                defer { run = "" }
                guard !run.isEmpty else { return }
                if run.count == 1 { out += run }
                else if run.contains("?") { out += "?" }
                else if run.contains("!") { out += "!" }
                else if let pause = run.first(where: { ",;:".contains($0) }) { out.append(pause) }
                else if atEnd { out += "." }
            }
            for c in word {
                if marks.contains(c) { run.append(c); continue }
                flush(atEnd: false)
                out.append(c)
            }
            flush(atEnd: index == words.count - 1)
            if pieces.isEmpty { out = String(out.drop { marks.contains($0) }) }
            if out.isEmpty { continue }
            if out.allSatisfy({ marks.contains($0) }), let before = pieces.popLast() {
                // A run on its own ("so ,.. then"): it joins the word before.
                let merged = DictationRules.tidy(before + out)
                pieces.append(merged)
                continue
            }
            pieces.append(out)
        }
        return pieces.joined(separator: " ")
    }

    /// Punctuation said aloud, as iPadOS's dictation takes it. Apple's engine writes some of it and spells out
    /// the rest ("I am fine, period."), so Henge does it all: the mark replaces any the engine put there, and
    /// after a full stop, question or exclamation mark or new line, a capital. Not after "a", "the" and the
    /// like: "a period of time" and "add the comma" mean the word.
    static func spokenPunctuation(_ text: String) -> String {
        let words = text.split(whereSeparator: \.isWhitespace).map(String.init)
        var out: [String] = []
        var capital = false
        var i = 0
        while i < words.count {
            if let (mark, length) = spokenMark(at: i, in: words),
               !meansTheWord(after: out.last, before: i + length < words.count ? words[i + length] : nil) {
                if var word = out.popLast() {
                    if mark.first != "\n" { while let c = word.last, marks.contains(c) { word.removeLast() } }
                    out.append(word + mark)
                } else {
                    out.append(mark)
                }
                if let m = mark.last, ".!?\n".contains(m) { capital = true }
                i += length
                continue
            }
            out.append(capital ? capitalizingStart(words[i]) : words[i])
            capital = false
            i += 1
        }
        var joined = ""
        for word in out {
            if let end = joined.last, end != "\n" { joined += " " }
            joined += word
        }
        return joined
    }

    /// The spoken forms, longest first. Return types a new line: in a chat, it sends.
    private static let spoken: [([String], String)] = [
        (["question", "mark"], "?"), (["exclamation", "point"], "!"), (["exclamation", "mark"], "!"),
        (["full", "stop"], "."), (["semi", "colon"], ";"), (["new", "line"], "\n"), (["new", "paragraph"], "\n\n"),
        (["comma"], ","), (["period"], "."), (["colon"], ":"), (["semicolon"], ";"),
    ]

    /// Words before which "period" or "comma" is a word: "a period", "the colon", "every comma".
    private static let determiners: Set<String> = [
        "a", "an", "the", "this", "that", "these", "those", "each", "every", "any", "one", "another", "per", "no",
        "its", "his", "her", "their", "our", "my", "your", "which", "what", "same",
    ]

    private static func bare(_ word: String) -> String {
        String(word.lowercased().drop { !$0.isLetter }.reversed().drop { !$0.isLetter }.reversed())
    }

    private static func spokenMark(at i: Int, in words: [String]) -> (String, Int)? {
        for (form, mark) in spoken {
            if bare(words[i]) == form.joined(separator: "-") { return (mark, 1) }
            guard i + form.count <= words.count else { continue }
            if zip(form, words[i...]).allSatisfy({ bare($1) == $0 }) { return (mark, form.count) }
        }
        return nil
    }

    private static func meansTheWord(after previous: String?, before next: String?) -> Bool {
        if let next, bare(next) == "of" { return true }
        guard let previous, let end = previous.last, !marks.contains(end) else { return false }
        return determiners.contains(bare(previous))
    }

    static func capitalizingStart(_ text: String) -> String {
        guard let i = text.firstIndex(where: \.isLetter) else { return text }
        return text.replacingCharacters(in: i...i, with: text[i].uppercased())
    }

    /// Back to lowercase after a comma, but not "I" or "NASA".
    static func lowercasingStart(_ text: String) -> String {
        guard let first = text.first, first.isUppercase else { return text }
        let word = text.prefix { $0.isLetter }
        if word == "I" || (word.count > 1 && word.allSatisfy(\.isUppercase)) { return text }
        return first.lowercased() + text.dropFirst()
    }

    /// The Mac's keyboard types plain ASCII: curly quotes, long dashes and the like become what it has.
    static func typeable(_ s: String) -> String {
        var out = ""
        for c in s {
            switch c {
            case "\u{2018}", "\u{2019}", "\u{02BC}": out.append("'")
            case "\u{201C}", "\u{201D}": out.append("\"")
            case "\u{2013}", "\u{2014}": out.append("-")
            case "\u{2026}": out.append("...")
            case "\u{00A0}", "\u{2009}", "\u{202F}": out.append(" ")
            default: out.append(c)
            }
        }
        return out
    }
}
