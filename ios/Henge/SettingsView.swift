import SwiftUI

struct SettingsView: View {
    /// Opened from a running Mac's Details: the same settings, inside that sheet's navigation, without what would
    /// open another sheet behind the Mac (JIT setup) or does nothing while JIT is on (the method).
    var inSession = false
    @ObservedObject private var jit = JITCoordinator.shared
    @AppStorage(JITCoordinator.autoEnableKey) private var jitAtLaunch = false
    @AppStorage(LaunchRequests.openIntoMacKey) private var openIntoMac = false
    @State private var jitReady = JITBootstrap.ready
    @State private var freezes = FreezeLog.count()
    @AppStorage(HardwareMouse.reverseKey) private var reverseScrolling = false
    @AppStorage(ScrollSpeed.key) private var scrollSpeed = ScrollSpeed.standard
    @AppStorage(EngineTuning.refreshKey) private var refreshRate = 60
    @AppStorage(EngineTuning.fastFlushKey) private var fastFlush = true
    @AppStorage(EngineTuning.wfiSpinKey) private var wfiSpin = false
    @AppStorage(EngineTuning.superblocksKey) private var superblocks = false
    @AppStorage(EngineTuning.highPriorityKey) private var highPriority = false
    @AppStorage(MacPause.autoPauseKey) private var autoPause = true
    @AppStorage(MacPause.autoResumeKey) private var autoResume = true
    @AppStorage(GuestMemory.saveAheadKey) private var quickSuspend = false
    /// HENGE_SAVE_AHEAD in henge-env.txt, which decides over the switch while the file has it (read on appear)
    @State private var quickSuspendFromFile: Bool?
    @ObservedObject private var images = RestoreImages.shared
    @AppStorage(JITBootstrap.codeCacheKey) private var codeCacheMB = JITBootstrap.defaultCodeCacheMB

    var body: some View {
        if inSession {
            form
        } else {
            NavigationStack { form }
        }
    }

    private var form: some View {
        Form {
            Section {
                Toggle("Open straight into my Mac", isOn: $openIntoMac)
            } header: {
                Text("Opening Henge")
            } footer: {
                Text("When Henge opens, it turns on JIT and starts the Mac you used last, with nothing to tap. A suspended Mac carries on where it was.")
            }
            .listRowBackground(Desk.card)
            Section {
                jitStatusRow
                Toggle("Turn on JIT when Henge opens", isOn: $jitAtLaunch)
                if !inSession {
                    Picker("Method", selection: $jit.method) {
                        ForEach(JITMethod.allCases) { Text($0.title).tag($0) }
                    }
                    Button("Set Up JIT…") { jit.showSetup = true }
                }
            } header: {
                Text("JIT")
            } footer: {
                Text(inSession
                     ? "JIT stays on while Henge is open. The method and setup are in Henge's Settings tab, once this Mac is shut down."
                     : (jit.method == .automatic ? jit.automaticDescription : "Henge uses \(jit.method.title).")
                        + (jitAtLaunch ? " When Henge opens it connects LocalDevVPN first if it is off, and comes back." : "")
                        + " Starting a Mac turns JIT on by itself, so you rarely need these.")
            }
            .listRowBackground(Desk.card)
            Section("Memory") {
                LabeledContent("Extra memory", value: Entitlements.increasedMemory ? "On" : "Off")
                LabeledContent("Henge's limit", value: ByteText.string(UInt64(Entitlements.memoryLimitAtLaunch)))
                LabeledContent("Free for Henge", value: ByteText.string(UInt64(henge_available_memory())))
                Text(Entitlements.increasedMemory
                     ? "A Mac's memory is kept out of this limit once JIT is on. If iOS refuses that, it uses free storage instead."
                     : "This install isn't signed with the increased memory limit entitlement. Reinstall Henge with a certificate that has it. Export your Mac first if the reinstall replaces the app's data.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            .listRowBackground(Desk.card)
            Section("Mouse and Trackpad") {
                VStack(alignment: .leading, spacing: 4) {
                    LabeledContent("Scroll speed", value: String(format: "%.2g×", scrollSpeed))
                    Slider(value: $scrollSpeed, in: ScrollSpeed.range)
                }
                if Device.isPad {
                    Toggle("Reverse scrolling", isOn: $reverseScrolling)
                }
                Text(Device.isPad
                     ? "Scroll speed is how far a Mac's page moves for your fingers, on a trackpad, with a mouse's wheel, or with two fingers on the on-screen trackpad. At 1× it follows them, as the iPad's own pages do. Most of the time the iPad's pointer is the Mac's, so the iPad's own settings (Settings › General › Trackpad & Mouse) set its speed and scroll direction. While Henge holds the pointer (zoomed in, or with the Mac on another screen), Reverse scrolling applies, and the Mac's own System Settings › Mouse › Tracking speed sets how fast the pointer moves."
                     : "Scroll speed is how far a Mac's page moves when two fingers scroll on the on-screen trackpad. At 1× it follows them, as the iPhone's own pages do.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            .listRowBackground(Desk.card)
            Section {
                Toggle("Suspend when Henge leaves the screen", isOn: $autoPause)
                Toggle("Resume when Henge comes back", isOn: $autoResume)
                    .disabled(!autoPause)
                Toggle("Quick Suspend (experimental)", isOn: quickSuspendFromFile.map { .constant($0) } ?? $quickSuspend)
                    .disabled(quickSuspendFromFile != nil)
                    .onAppear { quickSuspendFromFile = GuestMemory.saveAheadFromFile }
            } header: {
                Text("Suspend")
            } footer: {
                Text((autoPause
                     ? "Going Home, switching apps or locking the \(Device.name) freezes the Mac and starts saving it whole. iOS gives Henge only a few seconds once it leaves the screen, which may not be enough to finish. To be sure, tap Suspend and wait for it to say Suspended before you leave Henge."
                     : "The Mac keeps running when Henge leaves the screen, until iOS stops Henge. If iOS closes Henge, the Mac is turned off without saving. Tap Suspend and wait for it to say Suspended before you leave Henge.")
                     + "\n\nQuick Suspend keeps the Mac's memory saved on storage as it runs, so suspending takes seconds instead of half a minute, but the Mac can run slower while it waits for storage. It applies the next time a Mac starts. A Mac suspended with it on always resumes with it on."
                     + (quickSuspendFromFile == nil ? "" : " On this \(Device.name) the \(SaveAheadChoice.envKey) line in henge-env.txt decides it: remove that line to choose here."))
            }
            .listRowBackground(Desk.card)
            Section("Performance") {
                Picker("Mac refresh rate", selection: $refreshRate) {
                    Text("60 Hz").tag(60)
                    Text("120 Hz").tag(120)
                }
                .pickerStyle(.segmented)
                Toggle("Faster disk writes", isOn: $fastFlush)
                Toggle("Quicker wake from idle", isOn: $wfiSpin)
                Toggle("Superblocks (experimental)", isOn: $superblocks)
                Toggle("High-priority CPUs (experimental)", isOn: $highPriority)
                Picker("Code cache", selection: $codeCacheMB) {
                    Text("1 GB").tag(1024)
                    Text("1.5 GB").tag(1536)
                }
                Text("A bigger code cache empties less often and takes more memory; Henge picks 1.5 GB where there is room for it. Applies the next time Henge opens. If JIT stops turning on, choose 1 GB.")
                    .font(.footnote).foregroundStyle(.secondary)
                Text("60 Hz leaves more of the emulated CPU to apps. Faster disk writes order them without waiting for a full sync; a sudden power loss could lose the last ones. Quicker wake keeps an idle CPU watching for work for 50 µs, at some battery cost. Superblocks let translated code run past conditional branches. Changes apply the next time a Mac starts.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            .listRowBackground(Desk.card)
            Section {
                NavigationLink {
                    DownloadsView()
                } label: {
                    LabeledContent {
                        Text(images.items.isEmpty ? "None" : ByteText.string(images.totalBytes))
                    } label: {
                        Label("macOS Downloads", systemImage: "arrow.down.circle")
                    }
                }
            } header: {
                Text("Storage")
            } footer: {
                Text("The macOS downloads on this \(Device.name). Delete the ones you no longer need to free space; your Macs aren't affected.")
            }
            .listRowBackground(Desk.card)
            Section {
                ShareLink(item: HengeLog.logFileURL) { Label("Share henge.log", systemImage: "square.and.arrow.up") }
                if freezes > 0 {
                    ShareLink(item: FreezeLog.url) {
                        Label("Share Freeze Log (\(freezes))", systemImage: "square.and.arrow.up")
                    }
                }
            } header: {
                Text("Log")
            } footer: {
                Text("While a Mac runs, Henge notes each moment its screen stalls and what it was doing then. The freeze log is in Files › Henge too.")
            }
            .listRowBackground(Desk.card)
        }
        .scrollContentBackground(.hidden)
        .background(Desk.page)
        .navigationTitle(inSession ? "Henge Settings" : "Settings")
        .onAppear { images.refresh(); freezes = FreezeLog.count() }
        .task {
            while !Task.isCancelled {
                jitReady = JITBootstrap.ready
                try? await Task.sleep(nanoseconds: 1_000_000_000)
            }
        }
    }

    /// JIT's state, and while it is off the one-tap way to turn it on: no walkthrough when Henge is already set up.
    @ViewBuilder private var jitStatusRow: some View {
        if inSession {
            // Only what is so: turning it on from here could open the setup sheet behind the Mac.
            Label(jitReady ? "JIT is on" : "JIT is off", systemImage: "bolt.fill").foregroundStyle(jitReady ? Desk.good : Desk.dim)
        } else if jitReady {
            Label("JIT is on", systemImage: "bolt.fill").foregroundStyle(Desk.good)
        } else {
            Button { jit.turnOn() } label: {
                HStack(spacing: 10) {
                    if jit.busy {
                        ProgressView()
                    } else {
                        Image(systemName: "bolt.fill")
                    }
                    Text(jit.busy ? (jit.status ?? "Turning on JIT…") : "Turn On JIT")
                    Spacer()
                    Text("Off").foregroundStyle(.secondary)
                }
            }
            .disabled(jit.busy)
            if let error = jit.error, !jit.busy {
                Text(error).font(.footnote).foregroundStyle(Desk.bad)
            }
        }
    }
}

/// The freeze recorder's file (host/stallmon.c): one line per stall of the Mac's screen, read by tools/freeze_report.py.
enum FreezeLog {
    static var url: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("henge-freezes.jsonl")
    }

    /// How many stalls the file holds (0 when there is none).
    static func count() -> Int {
        guard let data = try? Data(contentsOf: url) else { return 0 }
        return data.reduce(0) { $0 + ($1 == UInt8(ascii: "\n") ? 1 : 0) }
    }
}
