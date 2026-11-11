import Foundation

// Dictation's rules (ios/Henge/DictationRules.swift), each as the person would meet it. make test-dictation
@main
struct DictationRulesTests {
    static var failures = 0

    static func check(_ ok: Bool, _ what: String, line: Int = #line) {
        if !ok {
            failures += 1
            print("FAIL line \(line): \(what)")
        }
    }

    /// What dictating these phrases, one after another, types at the Mac's cursor.
    static func typed(_ phrases: [String]) -> String {
        var joiner = DictationRules.Joiner()
        var out = phrases.compactMap { joiner.add($0) }.joined()
        if let end = joiner.end() { out += end }
        return out
    }

    static func spoken(_ phrase: String) -> String {
        DictationRules.spokenPunctuation(DictationRules.tidy(phrase))
    }

    static func main() {
        // English only: an iPad set to French, Arabic or Japanese still dictates U.S. English, as the Mac's keyboard
        // can't type their letters.
        for other in ["fr_FR", "ar_OM", "ja_JP", "es", "de-DE", "pt_BR"] {
            let l = DictationRules.englishLocale(for: Locale(identifier: other)).identifier
            check(l == "en_US", "\(other) dictates in \(l), not U.S. English")
        }
        // The person's own English is kept.
        for english in ["en_GB", "en_IN", "en_AU", "en_US", "en", "en-GB", "en_OM"] {
            let l = DictationRules.englishLocale(for: Locale(identifier: english)).identifier
            check(l == Locale(identifier: english).identifier, "\(english) became \(l)")
        }
        check(!DictationRules.isEnglish("eng"), "\"eng\" isn't en")
        check(!DictationRules.isEnglish(""), "nothing isn't English")

        // Phrases join with one space; the first has no leading space.
        check(typed(["Hello there."]) == "Hello there.", "the first phrase as it is")
        check(typed(["Hello there.", "How are you?"]) == "Hello there. How are you?", "a space after a sentence")
        check(typed(["  spaced  "]) == "spaced", "the engine's spaces trimmed")
        check(typed(["   "]) == "", "nothing to type")
        var joiner = DictationRules.Joiner()
        check(joiner.add("Hello.") == "Hello", "a phrase's full stop waits for the next")
        check(joiner.held == ".", "and is held")
        check(joiner.add("") == nil, "an empty phrase types nothing")
        check(joiner.end() == ".", "it's typed when dictation stops")
        check(joiner.end() == nil, "once")

        // Junk punctuation. Henge used to ask the engine to settle at every short pause; it cut sentences there and
        // punctuated each piece as one. Its phrases for "I went to the store. And I bought some apples. Then I came
        // home and made a pie. Hello comma how are you question mark", spoken with pauses (macOS 27, en_US):
        let nudged = ["I went to the store.", ". and I bought some apples.", ",... I came home and made a pie.",
                      ",.., how are you?", "."]
        check(typed(nudged) == "I went to the store. And I bought some apples. I came home and made a pie. How are you?",
              "no stray marks from cut phrases: \(typed(nudged))")
        // Left to settle on its own (now), the engine gives:
        let plain = ["I went to the store.", " And I bought some apples.", " Then I came home and made a pie.",
                     " Hello, how are you question mark?"]
        check(typed(plain) == "I went to the store. And I bought some apples. Then I came home and made a pie. Hello, how are you?",
              "the engine's own phrases: \(typed(plain))")
        check(DictationRules.tidy("So,... I came home") == "So, I came home", "a comma and a hesitation: the comma")
        check(DictationRules.tidy("I went to the... store") == "I went to the store", "a hesitation's dots go")
        check(DictationRules.tidy("wait ... what") == "wait what", "dots on their own go")
        check(DictationRules.tidy("I wonder...") == "I wonder.", "at the end, a full stop")
        check(DictationRules.tidy("Really?!") == "Really?", "a question and an exclamation: the question")
        check(DictationRules.tidy("It costs 3.50, or 1,000 in the U.S. today") == "It costs 3.50, or 1,000 in the U.S. today",
              "numbers and abbreviations untouched")

        // Spoken punctuation, as iPadOS's dictation takes it. The engine spells some out and writes others itself.
        check(spoken(" I am fine, period.") == "I am fine.", "\"period\" after the engine's comma")
        check(spoken("That is great exclamation point.") == "That is great!", "exclamation point")
        check(spoken("Is it ready question mark") == "Is it ready?", "question mark")
        check(spoken("The list colon apples, comma pairs, semicolon, and plums full stop.") == "The list: apples, pairs; and plums.",
              "colon, comma, semicolon, full stop: \(spoken("The list colon apples, comma pairs, semicolon, and plums full stop."))")
        check(spoken("I am done period see you soon") == "I am done. See you soon", "a capital after a spoken full stop")
        check(spoken("Dear Sam comma new line thanks for the apples") == "Dear Sam,\nThanks for the apples", "new line types Return")
        check(spoken("Add a semi-colon here") == "Add a semi-colon here", "after \"a\", the word")
        check(spoken("It took a period of time.") == "It took a period of time.", "a period of time")
        check(spoken("Put the comma here, then a question mark.") == "Put the comma here, then a question mark.", "the comma, a question mark")
        check(spoken("Each period of the game, every colon") == "Each period of the game, every colon", "each, every")
        check(spoken("Hello semi-colon goodbye") == "Hello; goodbye", "semi-colon in one word")
        check(spoken("He said, quote, yes, end quote, maybe.") == "He said, quote, yes, end quote, maybe.", "quotes aren't taken")
        // Said after a pause, a mark takes the place of the full stop the engine ended the phrase before with.
        check(typed(["Hello.", "Comma.", "How are you?"]) == "Hello, how are you?", "comma alone after a pause")
        check(typed(["Hello.", "Comma how are you?"]) == "Hello, how are you?", "comma starting the next phrase")
        check(typed(["I'm here.", "Question mark."]) == "I'm here?", "question mark after a pause")
        check(typed(["Wait.", "Comma.", "Period."]) == "Wait.", "the last one said wins")
        check(typed(["Hello comma.", "I think so."]) == "Hello, I think so.", "\"I\" keeps its capital after a comma")
        check(typed(["We use comma.", "NASA data."]) == "We use, NASA data.", "and so does NASA")
        check(typed(["New line.", "Next"]) == "\nNext", "a new line has no space after it")
        check(typed(["Done.", "New line next"]) == "Done.\nNext", "the full stop before a new line is kept")

        // What the Mac's U.S. keyboard can type.
        check(DictationRules.typeable("It\u{2019}s \u{201C}fine\u{201D} \u{2014} really\u{2026}") == "It's \"fine\" - really...",
              "curly quotes, a long dash and an ellipsis become ASCII")
        check(DictationRules.typeable("a\u{00A0}b") == "a b", "a non-breaking space becomes a space")
        let ascii = "Plain text, 123!"
        check(DictationRules.typeable(ascii) == ascii, "plain ASCII unchanged")

        if failures > 0 {
            print("\(failures) dictation rule(s) failed")
            exit(1)
        }
        print("dictation rules: all passed")
    }
}
