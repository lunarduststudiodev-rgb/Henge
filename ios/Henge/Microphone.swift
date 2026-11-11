import AVFAudio

/// The Mac's microphone is the iPad's (devices/virtio_snd.c's input stream, host/audio_mac.c). The app's audio
/// session plays only, mixing with other apps, until something records: the Mac, or Henge's own dictation
/// (Dictation.swift). Then it plays and records, and goes back to playing only once neither does. iOS shows its
/// microphone indicator only meanwhile.
enum Microphone {
    private static let lock = NSLock()
    private static var macRecords = false
    /// Dictation's listeners recording now: a cancelled one may stop after the next has started (Dictation.swift).
    private static var listeners = 0

    static func install() {
        henge_vm_set_mic_hook { on in
            // On the engine's microphone queue, before its input starts (on) or after it stopped: it may block.
            if on != 0 && AVAudioApplication.shared.recordPermission == .undetermined {
                // The first time the Mac records: iOS asks the person. Without it the Mac hears silence.
                let answered = DispatchSemaphore(value: 0)
                AVAudioApplication.requestRecordPermission { granted in
                    HengeLog.log("microphone", "permission \(granted ? "given" : "refused")")
                    answered.signal()
                }
                answered.wait()
            }
            Microphone.set(mac: on != 0) // named in full: a C callback can't capture Self
        }
    }

    /// One of dictation's listeners records (true) or has stopped (false), each once. Before it reads the
    /// microphone: the session must record first. The session records while any listener does.
    static func setDictating(_ on: Bool) { set(dictation: on) }

    private static func set(mac: Bool? = nil, dictation: Bool? = nil) {
        lock.lock()
        defer { lock.unlock() }
        let before = macRecords || listeners > 0
        if let mac { macRecords = mac }
        if let dictation { listeners = max(listeners + (dictation ? 1 : -1), 0) }
        let dictating = listeners > 0
        let records = macRecords || dictating
        guard records != before else { return }
        let who = macRecords && dictating ? "the Mac and dictation" : macRecords ? "the Mac" : "dictation"
        let session = AVAudioSession.sharedInstance()
        if records {
            do {
                try session.setCategory(.playAndRecord, options: [.mixWithOthers, .defaultToSpeaker, .allowBluetoothA2DP])
                try session.setActive(true)
                HengeLog.log("microphone", "\(who) records: the audio session records too")
            } catch {
                HengeLog.log("microphone", "the audio session can't record: \(error.localizedDescription)")
            }
        } else {
            try? session.setCategory(.playback, options: [.mixWithOthers])
            HengeLog.log("microphone", "nothing records: the audio session plays only")
        }
    }
}
