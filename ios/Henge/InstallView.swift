import SwiftUI

/// Download macOS (or use a copy already on this device) and install it on a new Mac.
struct InstallView: View {
    @ObservedObject private var installer = Installer.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @ObservedObject private var downloads = ImageDownloads.shared
    @ObservedObject private var images = RestoreImages.shared
    @State private var downloadError: String?
    @State private var jitReady = JITBootstrap.ready
    @StateObject private var display = DisplayRouter()
    @State private var release = MacOSRelease.all.first(where: Installer.haveImage) ?? MacOSRelease.recommended
    @State private var name = "Mac"
    @State private var ramGB = Device.suggestedRAMGB
    @State private var deleteAfter = false
    @State private var refresh = 0 // redraws the list when a download is deleted
    @State private var details = false
    @State private var stageStarted = Date()
    @State private var stageSeen = -1
    @State private var confirmDeleteDownload: MacOSRelease?
    /// Why this device can't (or may not) give the Mac the memory the chosen macOS needs to install.
    @State private var memoryProblem: (text: String, serious: Bool)?
    @AppStorage("henge.cpus") private var cpus = 4

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 0) {
                    switch installer.phase {
                    case .idle: chooser
                    case .downloading, .preparing, .installing: progress
                    case .waitingForMac: waiting
                    case .done(let text): finished(text)
                    case .failed(let text): failed(text)
                    }
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 24)
                .deskColumn()
            }
            .background(Desk.page)
            .navigationTitle(installer.busy ? "Installing" : "Install macOS")
            .onAppear { images.refresh() }
            .onChange(of: downloads.status) { _ in images.refresh() }
            .onChange(of: release) { memoryProblem = Installer.memoryProblem($0) }
            .task {
                while !Task.isCancelled { // the region may be claimed after a hand-off to StikDebug
                    jitReady = JITBootstrap.ready
                    if !installer.busy { memoryProblem = Installer.memoryProblem(release) } // the room changes as JIT turns on and apps close
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                }
            }
        }
    }

    // MARK: Choosing

    @ViewBuilder private var chooser: some View {
        if GraphicsSupport.olderSystem { GraphicsSupportInstallNote() }
        DeskHeading("Version")
        VStack(spacing: 0) {
            ForEach(MacOSRelease.all) { r in
                HStack(spacing: 0) {
                    Button { release = r } label: { versionRow(r) }
                        .buttonStyle(.plain)
                    if (Installer.haveImage(r) || downloads.status[r.id] != nil) && installer.imageInUse != r.id { // restore images are 15-25 GB: deleting one, or a download under way, should be easy to find; not the one an install is using
                        Button { confirmDeleteDownload = r } label: {
                            Image(systemName: "trash")
                                .font(.system(size: 15, weight: .medium))
                                .foregroundStyle(Desk.dim)
                                .frame(width: 44, height: 44)
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel("Delete the \(r.name) download")
                        .padding(.trailing, 6)
                    }
                }
                    .contextMenu {
                        if Installer.haveImage(r) || downloads.status[r.id] != nil {
                            Button("Delete Download", role: .destructive) { confirmDeleteDownload = r }
                        }
                    }
                if r.id != MacOSRelease.all.last?.id { Rectangle().fill(Desk.hairline).frame(height: 0.5) }
            }
        }
        .deskCard()
        .id(refresh)
        .confirmationDialog("Delete this download?", isPresented: Binding(get: { confirmDeleteDownload != nil }, set: { if !$0 { confirmDeleteDownload = nil } }),
                            titleVisibility: .visible) {
            Button("Delete Download", role: .destructive) {
                if let r = confirmDeleteDownload { Installer.deleteImage(r) }
                refresh += 1
            }
        }
        if let s = downloads.status[release.id] {
            downloadCard(release, s, deletable: true).padding(.top, 12)
        }
        if !images.items.isEmpty {
            NavigationLink {
                DownloadsView()
            } label: {
                HStack {
                    Label("Manage Downloads", systemImage: "arrow.down.circle")
                    Spacer()
                    Text(ByteText.string(images.totalBytes)).foregroundStyle(Desk.dim)
                    Image(systemName: "chevron.right").font(.system(size: 13, weight: .semibold)).foregroundStyle(Desk.faint)
                }
                .font(.system(size: 16))
                .foregroundStyle(Desk.text)
                .padding(.horizontal, 17)
                .padding(.vertical, 14)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .deskCard()
            .padding(.top, 12)
        }

        DeskHeading("New Mac")
        VStack(spacing: 0) {
            TextField("Name", text: $name)
                .font(.system(size: 16))
                .padding(.horizontal, 17)
                .padding(.vertical, 14)
            Rectangle().fill(Desk.hairline).frame(height: 0.5)
            Stepper(value: $ramGB, in: Device.selectableMemoryChoices) {
                HStack {
                    Text("Memory").font(.system(size: 16))
                    Spacer()
                    Text("\(ramGB) GB").font(.system(size: 16, weight: .semibold))
                }
            }
            .padding(.horizontal, 17)
            .padding(.vertical, 10)
            if Device.memoryNote(ramGB) != nil {
                MemoryNote(gb: ramGB)
                    .padding(.horizontal, 17)
                    .padding(.bottom, 10)
            }
            if Device.isPad && Device.physicalMemoryMB >= 7_000 && !Device.supports6GBGuest {
                Text("6 GB requires the increased-memory and extended-virtual-addressing entitlements.")
                    .font(.system(size: 13)).foregroundStyle(Desk.dim)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 17)
                    .padding(.bottom, 10)
            }
            Rectangle().fill(Desk.hairline).frame(height: 0.5)
            Toggle("Delete the download after", isOn: $deleteAfter)
                .font(.system(size: 16))
                .padding(.horizontal, 17)
                .padding(.vertical, 10)
        }
        .deskCard()

        // Before any Mac exists, this is the first thing that needs JIT (#23): the button turns it on, then installs.
        NeedsJITButton(title: Installer.haveImage(release) ? "Install"
                              : downloads.status[release.id] != nil ? "Install When Downloaded" : "Download and Install",
                       enabled: !Device.chipTooOld,
                       takeOver: {
                           // A Mac has run in this launch: no JIT here. The download happens here, then Henge restarts to install.
                           guard Relaunch.needed else { return false }
                           startInstall()
                           return true
                       }) {
            startInstall()
        }
        .padding(.top, 20)
        if !Installer.haveImage(release) && downloads.status[release.id] == nil {
            // No JIT for this: the install can come later.
            Button("Download Only") {
                Task {
                    do { try await downloads.begin(release) } catch { downloadError = error.localizedDescription }
                }
            }
            .font(.system(size: 16, weight: .semibold))
            .foregroundStyle(Desk.accentLight)
            .padding(.top, 14)
            .alert("Couldn't start the download", isPresented: Binding(get: { downloadError != nil }, set: { if !$0 { downloadError = nil } })) {
                Button("OK", role: .cancel) { downloadError = nil }
            } message: {
                Text(downloadError ?? "")
            }
        }
        Text(Device.chipTooOld ? Device.chipTooOldText
             : (jitReady ? "" : (JITBootstrap.lastFailure.map { $0 + " " } ?? "Henge turns on JIT first. "))
               + (Installer.haveImage(release) ? "" : Self.downloadNote)
               + "The install needs Henge open and the \(Device.name) plugged in. Needs about \(ByteText.string(UInt64(Installer.spaceNeeded(release)))) free.")
            .font(.system(size: 13))
            .foregroundStyle(Desk.dim)
            .multilineTextAlignment(.center)
            .padding(.top, 10)
        if let problem = memoryProblem {
            Text(problem.text)
                .font(.system(size: 13))
                .foregroundStyle(problem.serious ? Desk.bad : Desk.warn)
                .multilineTextAlignment(.center)
                .padding(.top, 8)
        }
    }

    private func versionRow(_ r: MacOSRelease) -> some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(r.name).font(.system(size: 16, weight: .medium)).foregroundStyle(Desk.text)
                if let size = Installer.imageSize(r) {
                    Text("Downloaded, \(ByteText.string(UInt64(size)))").font(.system(size: 13)).foregroundStyle(Desk.good)
                } else if let s = downloads.status[r.id] {
                    Text(s.problem != nil ? "Download stopped at \(Int(s.fraction * 100))%"
                         : s.paused ? "Download paused at \(Int(s.fraction * 100))%" : "Downloading, \(Int(s.fraction * 100))%")
                        .font(.system(size: 13)).foregroundStyle(s.problem != nil ? Desk.bad : Desk.accentLight)
                } else {
                    Text("\(ByteText.string(UInt64(r.bytes))) download").font(.system(size: 13)).foregroundStyle(Desk.dim)
                }
                Text(r.note).font(.system(size: 13)).foregroundStyle(Desk.dim)
            }
            Spacer()
            if release == r { Image(systemName: "checkmark").font(.system(size: 15, weight: .bold)).foregroundStyle(Desk.accentLight) }
        }
        .padding(.horizontal, 17)
        .padding(.vertical, 12)
        .contentShape(Rectangle())
    }

    private static let downloadNote = "The download keeps going when you switch apps or lock the screen, and you can pause it. Swiping Henge away in the app switcher pauses it until you open Henge again. "

    /// How far a download is, with Pause or Resume.
    private func downloadCard(_ r: MacOSRelease, _ s: ImageDownloads.Status, deletable: Bool) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(s.problem != nil ? "Download stopped" : s.paused ? "Download paused" : "Downloading \(r.name)")
                    .font(.system(size: 16, weight: .semibold))
                Spacer()
                Text("\(Int(s.fraction * 100))%").font(.system(size: 16, weight: .semibold)).monospacedDigit()
            }
            DeskBar(fraction: s.fraction)
            Text(s.summary).font(.system(size: 13)).foregroundStyle(Desk.dim).monospacedDigit()
            if let problem = s.problem {
                Text(problem).font(.system(size: 13)).foregroundStyle(Desk.bad)
            }
            HStack {
                Button {
                    if s.paused { downloads.resume(r.id) } else { downloads.pause(r.id) }
                } label: {
                    Label(s.paused ? "Resume" : "Pause", systemImage: s.paused ? "play.fill" : "pause.fill")
                }
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(Desk.accentLight)
                Spacer()
                if deletable {
                    Button("Delete") { confirmDeleteDownload = r }
                        .font(.system(size: 15))
                        .foregroundStyle(Desk.bad)
                }
            }
            .padding(.top, 2)
        }
        .padding(17)
        .frame(maxWidth: .infinity, alignment: .leading)
        .deskCard()
    }

    // MARK: Going

    private static let stages = ["Download", "Start installer", "Write macOS", "Check macOS", "Finish"]

    private var stage: Int {
        switch installer.phase {
        case .downloading: return 0
        case .preparing: return 1
        default:
            switch installer.step {
            case 0, 1: return 1
            case 2: return 2
            case 3: return 3
            default: return 4
            }
        }
    }

    private var fraction: Double {
        if case .downloading = installer.phase {
            return downloads.status[release.id]?.fraction ?? (Installer.haveImage(release) ? 1 : 0)
        }
        return installer.stepProgress
    }

    /// From how this step has gone so far; nothing until there is enough to go on.
    private var timeLeft: String? {
        let elapsed = Date().timeIntervalSince(stageStarted)
        guard stageSeen == stage, fraction > 0.03, fraction < 1, elapsed > 10 else { return nil }
        let minutes = Int((elapsed / fraction * (1 - fraction) / 60).rounded(.up))
        return minutes <= 1 ? "About a minute left in this step" : "About \(minutes) minutes left in this step"
    }

    @ViewBuilder private var progress: some View {
        Text(stage == 0 ? "Downloading \(release.name). It keeps going when you switch apps; the install starts once it's done and Henge is open."
                        : "\(release.name) on this \(Device.name). Keep Henge open.")
            .font(.system(size: 16))
            .foregroundStyle(Desk.dim)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 4)
            .padding(.bottom, 16)

        TimelineView(.periodic(from: .now, by: 1)) { _ in
            VStack(alignment: .leading, spacing: 0) {
                Text("\(Int(fraction * 100))%")
                    .font(.system(size: 66, weight: .bold))
                    .monospacedDigit()
                Text(stage == 0 ? (downloads.status[release.id]?.summary ?? "Starting the download…")
                                : timeLeft ?? "Step \(stage + 1) of \(Self.stages.count)")
                    .font(.system(size: 15))
                    .foregroundStyle(Desk.dim)
                    .padding(.top, 2)
                    .padding(.bottom, 16)
                DeskBar(fraction: fraction, height: 11)
                if stage == 0, let s = downloads.status[release.id] {
                    Button {
                        if s.paused { downloads.resume(release.id) } else { downloads.pause(release.id) }
                    } label: {
                        Label(s.paused ? "Resume Download" : "Pause Download", systemImage: s.paused ? "play.fill" : "pause.fill")
                    }
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(Desk.accentLight)
                    .padding(.top, 16)
                    if let problem = s.problem {
                        Text(problem).font(.system(size: 13)).foregroundStyle(Desk.bad).padding(.top, 8)
                    }
                }
            }
            .padding(.horizontal, 20)
            .padding(.vertical, 24)
            .frame(maxWidth: .infinity, alignment: .leading)
            .deskCard(radius: 30)
        }
        .onChange(of: stage) { value in
            stageSeen = value
            stageStarted = Date()
        }
        .onAppear {
            stageSeen = stage
            stageStarted = Date()
        }

        DeskHeading("Steps")
        VStack(spacing: 0) {
            ForEach(Array(Self.stages.enumerated()), id: \.offset) { i, title in
                HStack {
                    Text(title)
                        .font(.system(size: 16, weight: i == stage ? .semibold : .regular))
                        .foregroundStyle(i > stage ? Desk.faint : Desk.text)
                    Spacer()
                    if i < stage {
                        Image(systemName: "checkmark").font(.system(size: 15, weight: .bold)).foregroundStyle(Desk.good)
                    } else if i == stage {
                        ProgressView().controlSize(.small)
                    }
                }
                .padding(.horizontal, 17)
                .padding(.vertical, 13)
                if i < Self.stages.count - 1 { Rectangle().fill(Desk.hairline).frame(height: 0.5) }
            }
        }
        .deskCard()

        Button(details ? "Hide details" : "Show details") { withAnimation { details.toggle() } }
            .font(.system(size: 15, weight: .semibold))
            .foregroundStyle(Desk.accentLight)
            .padding(.top, 14)
        if details {
            VStack(spacing: 12) {
                if case .installing = installer.phase {
                    BootDisplay(display: display, role: .device, firmwareScreen: true)
                        .aspectRatio(16.0 / 10.0, contentMode: .fit)
                        .clipShape(RoundedRectangle(cornerRadius: 15, style: .continuous))
                }
                LogTail()
            }
            .padding(14)
            .deskCard()
            .padding(.top, 10)
        }
    }

    private func startInstall() {
        installer.start(release, name: name.isEmpty ? "Mac" : name, ramMB: ramGB * 1024, cpus: cpus, keepImage: !deleteAfter)
    }

    private var waiting: some View {
        VStack(spacing: 14) {
            Image(systemName: "arrow.down.circle.fill").font(.system(size: 54)).foregroundStyle(Desk.good).padding(.top, 8)
            Text("Downloaded").font(.system(size: 24, weight: .bold))
            Text("\(release.name) is ready to install. Shut down the Mac that's on, and Henge restarts and installs it by itself.")
                .font(.system(size: 15)).foregroundStyle(Desk.dim).multilineTextAlignment(.center)
        }
        .padding(20)
        .frame(maxWidth: .infinity)
        .deskCard(radius: 30)
        .padding(.top, 8)
    }

    private func finished(_ text: String) -> some View {
        VStack(spacing: 14) {
            Image(systemName: "checkmark.circle.fill").font(.system(size: 54)).foregroundStyle(Desk.good).padding(.top, 8)
            Text("Installed").font(.system(size: 24, weight: .bold))
            Text(text).font(.system(size: 15)).foregroundStyle(Desk.dim).multilineTextAlignment(.center)
        }
        .padding(20)
        .frame(maxWidth: .infinity)
        .deskCard(radius: 30)
        .padding(.top, 8)
    }

    @ViewBuilder private func failed(_ text: String) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("It did not work").font(.system(size: 22, weight: .bold))
            Text(text).font(.system(size: 15))
            Button("Back") { installer.reset() }.buttonStyle(DeskButtonStyle()).padding(.top, 6)
        }
        .padding(18)
        .deskCard(radius: 30)
        .padding(.top, 8)
        DeskHeading("Log")
        LogTail().padding(14).frame(maxWidth: .infinity, alignment: .leading).deskCard()
    }
}

/// The last lines of Henge's log.
private struct LogTail: View {
    @State private var lines: [String] = []

    var body: some View {
        VStack(alignment: .leading, spacing: 1) {
            ForEach(Array(lines.enumerated()), id: \.offset) { _, line in
                Text(line).font(.system(size: 9, design: .monospaced)).foregroundStyle(.secondary).lineLimit(2)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .task {
            while !Task.isCancelled {
                lines = Array(HengeLog.recentLines(14))
                try? await Task.sleep(nanoseconds: 1_000_000_000)
            }
        }
    }
}
