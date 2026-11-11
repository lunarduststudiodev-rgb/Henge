import AVFAudio
import CoreMedia
import Speech
import SwiftUI

/// Speak instead of type. Apple's speech engine (SpeechTranscriber, iOS and iPadOS 26) turns what the microphone
/// hears into text on this device. Its model is the system's: it adds nothing to Henge's size, and it runs outside
/// Henge's memory, which the Mac needs.
///
/// What is said goes straight to the Mac's cursor: each phrase is typed as soon as the engine settles on it, so
/// nothing typed is ever taken back. Typed rather than pasted, so the clipboard stays the person's; Henge types
/// faster than anyone talks. Meanwhile the pill shows the words still being worked out, and a meter of what the
/// microphone hears.
///
/// The engine settles a phrase at a pause of its own: with its quicker settling (.fastResults) that is about a second
/// after the person stops. Henge doesn't hurry it. Asking it to settle sooner (finalize at a short pause) cut
/// phrases mid-sentence, and the engine punctuated each piece as a sentence of its own: stray ". and", ",..." and
/// lone full stops typed into the Mac (tests/test_dictation_rules.swift has what it typed).
///
/// Started and stopped with the microphone key on the keyboard's row of Mac keys (Input.swift's ModifierRow), or on
/// the small control that stands in for the row with a hardware keyboard (CompactMacKeys).
/// While it listens the on-screen letters give way to that row alone, as iPadOS's own dictation does
/// (`lettersAway`); DictationPill sits on the row's right end, with Stop and a keyboard button for the letters.
@MainActor
final class Dictation: ObservableObject {
    static let shared = Dictation()

    enum Phase: Equatable {
        case off
        case preparing(String)
        case listening
        case finishing
        case failed(String)
    }

    @Published private(set) var phase: Phase = .off
    /// The first time dictation is used: a word in the pill that it types a U.S. keyboard's keys, as the password
    /// key's alert says. A Mac set to another layout would get other characters.
    @Published private(set) var layoutNote = false
    /// The on-screen keyboard's letters are put away while dictating, leaving its row of Mac keys with the pill on
    /// it (KeyboardCatcher.CatcherView.inputView). Back when dictation ends, or with the pill's keyboard button.
    @Published private(set) var lettersAway = false
    static let layoutNotedKey = "henge.dictation.layoutNoted"
    static let layoutNoteText = "Dictation types the keys of a U.S. keyboard: keep the Mac's keyboard layout on U.S. or ABC."

    /// The listener on iOS 26 and later (Listener); Any, as the app still opens on iOS 17.
    private var listener: Any?
    /// Each start's number: what an earlier start reports late is not this one's.
    private var run = 0
    /// Spaces between phrases, and each one's closing marks until the next (DictationRules.Joiner).
    private var joiner = DictationRules.Joiner()
    /// The person's press that last moved the Mac's focus (mach_absolute_time's ticks): a phrase said before it was
    /// for where the focus was, though the engine settled it after.
    private var focusMovedAt: UInt64 = 0
    /// When the last phrase typed was said: the closing full stop is for where it went.
    private var lastSaid: UInt64?

    static var isAvailable: Bool {
        if #available(iOS 26, *) { return SpeechTranscriber.isAvailable }
        return false
    }

    var isOn: Bool { phase != .off }

    /// The key's tap: starts listening, or stops.
    func toggle() {
        switch phase {
        case .listening: stop()
        case .off, .failed: start()
        case .preparing: cancel()
        case .finishing: stop()
        }
    }

    func start() {
        guard #available(iOS 26, *) else { return fail("Dictation needs iPadOS 26 or later.") }
        run += 1
        let mine = run
        joiner = DictationRules.Joiner()
        focusMovedAt = 0
        lastSaid = nil
        lettersAway = true
        StickyKeys.shared.releaseAll()
        DictationLive.shared.clear()
        layoutNote = !UserDefaults.standard.bool(forKey: Self.layoutNotedKey)
        phase = .preparing("Getting ready…")
        let listener = Listener()
        self.listener = listener
        HengeLog.log("dictation", "starting")
        Task {
            do {
                try await listener.start(
                    downloading: { Task { @MainActor in
                        if self.run == mine { self.phase = .preparing("Downloading Apple's speech model (once)…") }
                    } },
                    words: { text in Task { @MainActor in
                        if self.run == mine { DictationLive.shared.words = text }
                    } },
                    levels: { levels in Task { @MainActor in
                        if self.run == mine { DictationLive.shared.push(levels) }
                    } },
                    phrase: { text, said in Task { @MainActor in
                        if self.run == mine { self.type(text, said: said) }
                    } })
                guard run == mine else { return await listener.cancel() }
                phase = .listening
                if layoutNote {
                    // Seen once it listens; it stays a little while for the reading.
                    UserDefaults.standard.set(true, forKey: Self.layoutNotedKey)
                    DispatchQueue.main.asyncAfter(deadline: .now() + 12) { if self.run == mine { self.layoutNote = false } }
                }
                HengeLog.log("dictation", "listening (\(listener.localeName))")
            } catch {
                await listener.cancel()
                guard run == mine else { return }
                HengeLog.log("dictation", "couldn't start: \(error)")
                fail((error as? DictationError)?.message ?? "Dictation couldn't start: \(error.localizedDescription)")
            }
        }
    }

    /// Stops listening; the last words still go to the Mac. Asked again while the engine works them out, it stops
    /// that at once (the engine may never finish): what it already settled is still typed.
    func stop() {
        if phase == .finishing {
            if #available(iOS 26, *), let listener = listener as? Listener {
                HengeLog.log("dictation", "stopped again while finishing: the engine stops now")
                Task { await listener.cancel() }
            }
            return
        }
        guard phase == .listening, #available(iOS 26, *), let listener = listener as? Listener else { return cancel() }
        let mine = run
        phase = .finishing
        Task {
            await listener.finish()
            guard run == mine else { return }
            // The last phrase's full stop, kept back in case a spoken "comma" took its place: said with that phrase,
            // so the focus moving after it drops it too.
            if let end = joiner.end() { enqueue(end, said: lastSaid) }
            await TypingQueue.shared.whenDone(.dictation)
            guard run == mine else { return }
            HengeLog.log("dictation", "stopped")
            close()
        }
    }

    /// Stops at once: what hasn't been typed yet is dropped.
    func cancel() {
        guard phase != .off else { return }
        if #available(iOS 26, *), let listener = listener as? Listener {
            Task { await listener.cancel() }
        }
        HengeLog.log("dictation", "cancelled")
        close()
    }

    /// The pill's keyboard button: the letters come back, and dictation goes on.
    func lettersBack() {
        lettersAway = false
    }

    /// The keyboard went (KeyboardIntent.Hid): put away by the person, the last words still go to the field, which
    /// still has the focus; otherwise (the focus left text, something took the screen) it ends at once.
    func keyboardWent(_ why: KeyboardIntent.Hid) {
        guard isOn else { return }
        if why == .person { stop() } else { cancel() }
    }

    /// The Mac's focus moved (KeyboardIntent.typing), after the person's press at `press`: what they said before it
    /// was for where it was. Queued, KeyboardOwner drops it (TypingQueue); still being worked out, it is dropped as
    /// the engine settles it. The next words start afresh, with no space or full stop before them.
    func focusMoved(at press: UInt64) {
        guard isOn else { return }
        joiner = DictationRules.Joiner()
        lastSaid = nil
        focusMovedAt = max(focusMovedAt, press)
    }

    private func close() {
        // What isn't typed yet would go wherever the focus is now.
        TypingQueue.shared.drop([.dictation])
        run += 1
        listener = nil
        lettersAway = false
        phase = .off
        layoutNote = false
        DictationLive.shared.clear()
    }

    private func fail(_ why: String) {
        listener = nil
        lettersAway = false
        phase = .failed(why)
        layoutNote = false
        DictationLive.shared.clear()
        let mine = run
        DispatchQueue.main.asyncAfter(deadline: .now() + 5) {
            if self.run == mine, case .failed = self.phase { self.phase = .off }
        }
    }

    /// One settled phrase, typed at the Mac's cursor after any still being typed. said: when its audio ended
    /// (mach_absolute_time's ticks), when known.
    private func type(_ phrase: String, said: UInt64?) {
        if let said, said < focusMovedAt {
            HengeLog.log("dictation", "a phrase said before the focus moved is dropped")
            return
        }
        if let text = joiner.add(phrase) {
            lastSaid = said
            enqueue(text, said: said)
        }
    }

    /// After what is still being typed, the password's too (TypingQueue lets go of a latched ⌘ first). Asked for when
    /// it was said, so the focus moving after that drops it (KeyboardOwner).
    private func enqueue(_ text: String, said: UInt64? = nil) {
        TypingQueue.shared.add(text, by: .dictation, asked: said)
    }
}

/// What changes many times a second while dictating, kept apart from Dictation so only the pill redraws for it
/// (SessionView watches Dictation).
@MainActor
final class DictationLive: ObservableObject {
    static let shared = DictationLive()
    static let bars = 18

    /// The words the engine is still working out: not typed yet, and they may still change.
    @Published var words = ""
    /// The microphone's loudness, newest last, 0 (quiet) to 1, one per 30 ms.
    @Published private(set) var levels = [Float](repeating: 0, count: DictationLive.bars)

    func push(_ new: [Float]) {
        guard !new.isEmpty else { return }
        levels = Array((levels + new).suffix(Self.bars))
    }

    func clear() {
        words = ""
        levels = [Float](repeating: 0, count: Self.bars)
    }
}

struct DictationError: Error {
    let message: String
}

/// The microphone, through Apple's speech engine. Not on the main actor: the microphone's buffers come on an audio thread.
@available(iOS 26, *)
private final class Listener: @unchecked Sendable {
    // start, finish and cancel run on the main actor, one at a time between their waits: a second Stop while it
    // finishes (or a cancel while it starts) can't take the microphone down on two threads at once. The microphone's
    // buffers come on the audio thread: what they read of this is behind the lock, or set before the tap is.
    private let engine = AVAudioEngine()
    private var analyzer: SpeechAnalyzer?
    private var input: AsyncStream<AnalyzerInput>.Continuation? // behind the lock
    private var results: Task<Void, Never>?
    private var converter: AVAudioConverter?
    private var format: AVAudioFormat?
    private var recording = false
    private var tapped = false
    private var levels: (@Sendable ([Float]) -> Void)?
    private(set) var localeName = ""

    // Shared by the audio thread and the results task.
    private let lock = NSLock()
    /// Seconds of audio given to the engine so far: its clock.
    private var fed = 0.0
    /// The engine's clock on the host's (mach_absolute_time's ticks), from the microphone's buffers' host times: from
    /// `fed` seconds on, audio at `s` seconds was heard at host + (s - fed). A new one only where the microphone's
    /// clock and the engine's part (audio lost between buffers), so what was heard before keeps its own.
    private var anchors: [(fed: Double, host: UInt64)] = []

    @MainActor func start(downloading: @escaping @Sendable () -> Void,
               words: @escaping @Sendable (String) -> Void,
               levels: @escaping @Sendable ([Float]) -> Void,
               phrase: @escaping @Sendable (String, UInt64?) -> Void) async throws {
        guard await AVAudioApplication.requestRecordPermission() else {
            throw DictationError(message: "Henge can't use the microphone. Allow it in Settings › Apps › Henge › Microphone.")
        }
        self.levels = levels
        let locale = await SpeechTranscriber.supportedLocale(equivalentTo: DictationRules.englishLocale(for: .current))
            ?? Locale(identifier: "en_US")
        localeName = locale.identifier
        // Words as they are worked out (volatile, for the pill only), and the engine's quicker settling.
        let transcriber = SpeechTranscriber(locale: locale, transcriptionOptions: [],
                                            reportingOptions: [.volatileResults, .fastResults], attributeOptions: [.audioTimeRange])
        if let request = try await AssetInventory.assetInstallationRequest(supporting: [transcriber]) {
            downloading()
            try await request.downloadAndInstall()
        }
        guard let format = await SpeechAnalyzer.bestAvailableAudioFormat(compatibleWith: [transcriber]) else {
            throw DictationError(message: "Apple's speech engine isn't available on this device.")
        }
        self.format = format
        let analyzer = SpeechAnalyzer(modules: [transcriber])
        self.analyzer = analyzer
        try await analyzer.prepareToAnalyze(in: format)

        results = Task.detached { [weak self] in
            do {
                for try await result in transcriber.results {
                    guard let self else { return }
                    let text = String(result.text.characters)
                    if result.isFinal {
                        // Where its last word ends: the result's range runs on through the silence after it, to
                        // where the engine settled, which may be after the person tapped somewhere else.
                        let end = Self.lastWordEnd(result.text) ?? result.range.end.seconds
                        self.settled(text, through: end)
                        words("")
                        phrase(text, self.hostTime(of: end))
                    } else {
                        words(text)
                    }
                }
            } catch {
                HengeLog.log("dictation", "results ended: \(error)")
            }
        }

        // The session records before the microphone is touched: until then the input has no channels.
        Microphone.setDictating(true)
        recording = true
        let node = engine.inputNode
        let micFormat = node.outputFormat(forBus: 0)
        guard micFormat.channelCount > 0, micFormat.sampleRate > 0 else {
            throw DictationError(message: "No microphone is available.")
        }
        converter = AVAudioConverter(from: micFormat, to: format)
        let (stream, continuation) = AsyncStream<AnalyzerInput>.makeStream()
        lock.withLock { input = continuation }
        node.installTap(onBus: 0, bufferSize: 2048, format: micFormat) { [weak self] buffer, when in
            self?.feed(buffer, at: when)
        }
        tapped = true
        engine.prepare()
        try engine.start()
        try await analyzer.start(inputSequence: stream)
    }

    /// A phrase the engine settled: logs how long after its last word it came, which is the wait the person sees.
    private func settled(_ text: String, through end: Double) {
        let now = lock.withLock { fed }
        let words = text.split(whereSeparator: \.isWhitespace).count
        HengeLog.log("dictation", String(format: "settled %d words %.2f s after the audio they end at", words, now - end))
    }

    /// Where the last word with a time ends, on the engine's clock (attributeOptions: .audioTimeRange).
    private static func lastWordEnd(_ text: AttributedString) -> Double? {
        var end: Double?
        for run in text.runs {
            if let range = run.audioTimeRange, range.end.isNumeric { end = range.end.seconds }
        }
        return end
    }

    /// When the engine's clock read `seconds`, on the host's clock.
    private func hostTime(of seconds: Double) -> UInt64? {
        guard seconds.isFinite, seconds >= 0 else { return nil }
        return lock.withLock {
            guard let anchor = anchors.last(where: { $0.fed <= seconds }) ?? anchors.first else { return nil }
            let after = seconds - anchor.fed
            return after >= 0 ? anchor.host &+ AVAudioTime.hostTime(forSeconds: after)
                : anchor.host &- min(anchor.host, AVAudioTime.hostTime(forSeconds: -after))
        }
    }

    /// The buffer heard at `when` starts at `fed` on the engine's clock.
    private func anchor(_ when: AVAudioTime) {
        guard when.isHostTimeValid else { return }
        lock.withLock {
            if let last = anchors.last {
                let expected = last.host &+ AVAudioTime.hostTime(forSeconds: fed - last.fed)
                let apart = expected > when.hostTime ? expected - when.hostTime : when.hostTime - expected
                if apart < AVAudioTime.hostTime(forSeconds: 0.005) { return } // the same clock on
            }
            anchors.append((fed, when.hostTime))
            if anchors.count > 256 { anchors.removeFirst(anchors.count - 256) }
        }
    }

    /// The engine's clock moves on by what it was given.
    private func gave(_ buffer: AVAudioPCMBuffer) {
        let seconds = Double(buffer.frameLength) / buffer.format.sampleRate
        lock.withLock { fed += seconds }
    }

    /// One buffer from the microphone, in the engine's format.
    private func feed(_ buffer: AVAudioPCMBuffer, at when: AVAudioTime) {
        guard let input = lock.withLock({ self.input }), let format else { return }
        anchor(when)
        listen(buffer)
        guard let converter else {
            input.yield(AnalyzerInput(buffer: buffer))
            gave(buffer)
            return
        }
        let ratio = format.sampleRate / buffer.format.sampleRate
        let capacity = AVAudioFrameCount((Double(buffer.frameLength) * ratio).rounded(.up)) + 1
        guard let out = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: capacity) else { return }
        var given = false
        var error: NSError?
        converter.convert(to: out, error: &error) { _, status in
            if given {
                status.pointee = .noDataNow
                return nil
            }
            given = true
            status.pointee = .haveData
            return buffer
        }
        if error == nil, out.frameLength > 0 {
            input.yield(AnalyzerInput(buffer: out))
            gave(out)
        }
    }

    /// How loud the buffer is, 30 ms at a time, for the pill's meter.
    private func listen(_ buffer: AVAudioPCMBuffer) {
        guard let samples = buffer.floatChannelData?[0], buffer.frameLength > 0 else { return }
        let rate = buffer.format.sampleRate
        let frames = Int(buffer.frameLength)
        let step = max(1, Int(rate * 0.03))
        var loudness: [Float] = []
        var start = 0
        while start < frames {
            let end = min(frames, start + step)
            var sum: Float = 0
            for i in start..<end { sum += samples[i] * samples[i] }
            let rms = (sum / Float(end - start)).squareRoot()
            loudness.append(20 * log10(max(rms, 1e-6)))
            start = end
        }
        levels?(loudness.map { min(max(($0 + 55) / 40, 0), 1) })
    }

    @MainActor private func stopMicrophone() {
        if tapped {
            tapped = false
            engine.inputNode.removeTap(onBus: 0)
            engine.stop()
        }
        let input = lock.withLock {
            defer { self.input = nil }
            return self.input
        }
        input?.finish()
        if recording {
            recording = false
            Microphone.setDictating(false)
        }
    }

    /// Stops the microphone and waits for the engine's last words.
    @MainActor func finish() async {
        stopMicrophone()
        try? await analyzer?.finalizeAndFinishThroughEndOfInput()
        await results?.value
    }

    @MainActor func cancel() async {
        stopMicrophone()
        await analyzer?.cancelAndFinishNow()
        results?.cancel()
    }
}

/// A small pill on the keyboard's row while dictation is on: what the microphone hears, the words being worked
/// out (or why it can't listen), and Stop.
struct DictationPill: View {
    let width: CGFloat
    @ObservedObject var dictation = Dictation.shared
    @ObservedObject var live = DictationLive.shared
    @ObservedObject var placement = RowPlacement.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 10) {
                if hearing {
                    DictationMeter(levels: live.levels)
                } else {
                    Image(systemName: failed ? "exclamationmark.triangle.fill" : "mic.fill")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(failed ? Desk.bad : Desk.dim)
                }
                Text(status)
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(hearing && live.words.isEmpty ? Desk.dim : Desk.text)
                    .lineLimit(hearing ? 1 : 3)
                    .truncationMode(.head) // the newest words
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .animation(nil, value: status)
                // Not with a hardware keyboard's small control: there were no letters on the screen to bring back.
                if hearing && dictation.lettersAway && !placement.compact {
                    // As iPadOS's dictation has it: the letters back, still listening.
                    Button { dictation.lettersBack() } label: {
                        Image(systemName: "keyboard")
                            .font(.system(size: 15, weight: .semibold))
                            .foregroundStyle(Desk.text)
                            .frame(width: 36, height: 30)
                            .contentShape(Rectangle())
                    }
                    .accessibilityLabel("Show the keyboard")
                }
                if hearing {
                    Button { dictation.stop() } label: {
                        Text("Stop")
                            .font(.system(size: 14, weight: .semibold))
                            .foregroundStyle(.white)
                            .padding(.horizontal, 14)
                            .padding(.vertical, 6)
                            .background(Color.red.opacity(0.85), in: Capsule())
                    }
                }
            }
            if dictation.layoutNote {
                Text(Dictation.layoutNoteText)
                    .font(.system(size: 12, weight: .medium))
                    .foregroundStyle(Desk.dim)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.trailing, 10)
                    .padding(.bottom, 4)
            }
        }
        .padding(.leading, 14)
        .padding(.trailing, hearing ? 6 : 16)
        .padding(.vertical, 6)
        .frame(width: width)
        .frame(minHeight: 40)
        .deskGlass(in: dictation.layoutNote ? AnyShape(RoundedRectangle(cornerRadius: 20, style: .continuous)) : AnyShape(Capsule()))
        .shadow(color: .black.opacity(0.25), radius: 8, y: 2)
        .animation(.easeInOut(duration: 0.2), value: dictation.layoutNote)
        .accessibilityElement(children: .contain)
    }

    private var hearing: Bool { dictation.phase == .listening || dictation.phase == .finishing }

    private var failed: Bool {
        if case .failed = dictation.phase { return true }
        return false
    }

    private var status: String {
        switch dictation.phase {
        case .off: return ""
        case .preparing(let what): return what
        case .listening: return live.words.isEmpty ? "Listening…" : live.words
        case .finishing: return live.words.isEmpty ? "Finishing…" : live.words
        case .failed(let why): return why
        }
    }
}

/// Bars of the microphone's loudness, newest on the right: that it hears the person.
private struct DictationMeter: View {
    let levels: [Float]

    var body: some View {
        HStack(alignment: .center, spacing: 2) {
            ForEach(levels.indices, id: \.self) { i in
                Capsule()
                    .fill(levels[i] > 0.2 ? Color.red : Desk.dim.opacity(0.6))
                    .frame(width: 2.5, height: 3 + CGFloat(levels[i]) * 19)
            }
        }
        .frame(height: 22)
        .animation(.linear(duration: 0.06), value: levels)
        .accessibilityHidden(true)
    }
}
