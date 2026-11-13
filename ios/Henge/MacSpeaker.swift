import AVFAudio
import MediaPlayer
import UIKit

/// The Mac's speaker as a Bluetooth speaker: macOS sees a USB speaker named for this iPad (host/audio_usb.c) and sends
/// its sound at full strength, and the volume is the iPad's own.
/// - macOS's volume (its slider, Control Center, the volume keys) sets the iPad's system volume, through a hidden
///   MPVolumeView's slider (the one way an app may set it). Mute on the Mac is silence here; the iPad's volume stays.
/// - The iPad's volume buttons step the Mac's volume as a USB speaker's buttons do (HID volume keys), so macOS moves
///   its slider and shows its own volume display; any other change (Control Center's slider, a connected device)
///   moves the Mac's slider to it.
/// The volume is read from the shared audio session whatever its category, so it goes on working if the app turns
/// to play-and-record for the microphone.
@MainActor
final class MacSpeaker {
    static let shared = MacSpeaker()

    private var observation: NSKeyValueObservation?
    private var interruption: NSObjectProtocol?
    private var volumeView: MPVolumeView?
    /// The volume this side set last, and when: its change coming back is not the person's.
    private var setByMac: (level: Float, at: Date)?
    /// The volume this side sent to the Mac last, and when: ignore echoes while macOS settles to it.
    private var setByIPad: (level: Float, at: Date)?
    private var pendingVolumeTask: Task<Void, Never>?
    private var pendingVolumeTarget: Float?
    private var running = false

    /// Before the Mac is built: what macOS calls the speaker.
    func prepare() {
        henge_vm_speaker_name(UIDevice.current.model)
    }

    /// Once the Mac runs.
    func start() {
        guard !running else { return }
        running = true
        let session = AVAudioSession.sharedInstance()
        try? session.setActive(true) // outputVolume changes are only seen while the session is active
        observation = session.observe(\.outputVolume, options: [.old, .new]) { _, change in
            guard let old = change.oldValue, let new = change.newValue else { return }
            Task { @MainActor in MacSpeaker.shared.iPadVolumeChanged(from: old, to: new) }
        }
        interruption = NotificationCenter.default.addObserver(forName: AVAudioSession.interruptionNotification, object: session,
                                                              queue: .main) { note in
            let type = (note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt).flatMap(AVAudioSession.InterruptionType.init)
            if type == .ended { try? AVAudioSession.sharedInstance().setActive(true) }
        }
        attachVolumeView()
        henge_vm_speaker_listen({ _, level, mute in
            DispatchQueue.main.async { MacSpeaker.shared.macVolumeChanged(level: level, mute: mute != 0) }
        }, nil)
        // The Mac takes the iPad's volume: a Mac going on from where it was suspended moves its slider to it.
        setByIPad = (session.outputVolume, Date())
        henge_vm_speaker_host_volume(Double(session.outputVolume))
        HengeLog.log("speaker", "the Mac's speaker follows the iPad's volume (now \(Int(session.outputVolume * 100))%)")
    }

    /// Once the Mac has stopped.
    func stop() {
        guard running else { return }
        running = false
        pendingVolumeTask?.cancel()
        pendingVolumeTask = nil
        pendingVolumeTarget = nil
        henge_vm_speaker_listen(nil, nil)
        observation?.invalidate()
        observation = nil
        if let interruption { NotificationCenter.default.removeObserver(interruption) }
        interruption = nil
        volumeView?.removeFromSuperview()
        volumeView = nil
        setByMac = nil
        setByIPad = nil
    }

    /// The system volume slider, out of sight. In a window, it also keeps iOS's own volume display away while the Mac
    /// shows its own.
    private func attachVolumeView() {
        guard volumeView == nil else { return }
        let window = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.flatMap(\.windows)
            .first { $0.isKeyWindow } ?? UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
            .flatMap(\.windows).first
        guard let window else { return }
        let view = MPVolumeView(frame: CGRect(x: -2000, y: -2000, width: 100, height: 40))
        view.alpha = 0.01
        view.isUserInteractionEnabled = false
        window.addSubview(view)
        volumeView = view
    }

    private var slider: UISlider? {
        if volumeView?.window == nil { volumeView?.removeFromSuperview(); volumeView = nil; attachVolumeView() }
        return volumeView?.subviews.compactMap { $0 as? UISlider }.first
    }

    /// macOS set the speaker's volume: the iPad's goes there. Muted, the sound is silent and the iPad's volume stays.
    private func macVolumeChanged(level: Double, mute: Bool) {
        guard running, !mute else { return }
        let target = Float(min(max(level, 0), 1))
        if let sent = setByIPad {
            let age = Date().timeIntervalSince(sent.at)
            if age < 0.6 || (age < 1.5 && abs(target - sent.level) < 0.015) {
                return // echo from an iPad-initiated volume change while macOS settles
            }
            setByIPad = nil
        }
        if abs(AVAudioSession.sharedInstance().outputVolume - target) < 0.005 { return }
        guard let slider else {
            HengeLog.log("speaker", "no volume slider: the iPad's volume can't follow the Mac's")
            return
        }
        pendingVolumeTask?.cancel()
        pendingVolumeTask = nil
        pendingVolumeTarget = nil
        setByMac = (target, Date())
        slider.setValue(target, animated: false)
        slider.sendActions(for: .valueChanged)
    }

    /// The iPad's volume changed.
    private func iPadVolumeChanged(from old: Float, to new: Float) {
        guard running, old != new else { return }
        if let set = setByMac, Date().timeIntervalSince(set.at) < 1.5, abs(new - set.level) < 0.01 {
            setByMac = nil // the Mac's own change, coming back
            return
        }
        pendingVolumeTarget = new
        pendingVolumeTask?.cancel()
        pendingVolumeTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 30_000_000) // coalesce rapid/intermediate KVO updates
            guard let self, !Task.isCancelled, self.running, let target = self.pendingVolumeTarget else { return }
            self.pendingVolumeTarget = nil
            self.pendingVolumeTask = nil
            if let set = self.setByMac, Date().timeIntervalSince(set.at) < 1.5, abs(target - set.level) < 0.01 {
                self.setByMac = nil
                return
            }
            self.setByIPad = (target, Date())
            // Send only the absolute volume level rather than pairing HID volume keys with host_volume
            // (which double-steps and fights with CoreAudio). When henge-agent is linked, it coordinates
            // the native macOS Volume HUD and 1:1 CoreAudio level smoothly.
            henge_vm_speaker_host_volume(Double(target))
        }
    }
}
