import SwiftUI
import UniformTypeIdentifiers

/// Your Mac as you left it, and one big button.
struct HomeView: View {
    @ObservedObject private var machines = Machines.shared
    @ObservedObject private var jit = JITCoordinator.shared
    @ObservedObject private var installer = Installer.shared
    @ObservedObject private var downloads = ImageDownloads.shared
    @ObservedObject private var suspendedMacs = SuspendedMacs.shared
    @Binding var running: VirtualMac?
    @Binding var tab: RootView.Tab
    @State private var picking = false
    @State private var jitReady = JITBootstrap.ready
    @State private var chosen: UUID?

    /// The Mac on show: the one picked, or the one used last.
    private var mac: VirtualMac? {
        if let chosen, let mac = machines.all.first(where: { $0.id == chosen }) { return mac }
        return machines.all.max { ($0.lastUsed ?? $0.added) < ($1.lastUsed ?? $1.added) }
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 14) {
                    notices
                    if let mac {
                        MacHero(mac: mac, jitReady: jitReady, start: { running = mac },
                                jitNote: jitReady || jit.error != nil ? nil : JITBootstrap.lastFailure)
                        HStack(spacing: 10) {
                            FactTile(title: "Disk", value: ByteText.string(UInt64(max(mac.bytesUsed, 0))))
                            FactTile(title: "Last used", value: mac.lastUsedText)
                        }
                        others(besides: mac)
                    } else {
                        firstMac
                    }
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 24)
                .deskColumn()
            }
            .background(Desk.page)
            .navigationTitle("Henge")
            .navigationDestination(for: VirtualMac.self) { MacSettingsView(mac: $0) }
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Menu {
                        Button("Install macOS…") { tab = .install }
                        Button("Bring in a Mac from a computer…") { picking = true }
                    } label: {
                        Image(systemName: "plus")
                    }
                    .accessibilityLabel("Add a Mac")
                }
            }
            .fileImporter(isPresented: $picking, allowedContentTypes: [.folder]) { result in
                if case let .success(url) = result { machines.importFolder(url) }
            }
            .task {
                while !Task.isCancelled { // the region may be claimed after a hand-off to StikDebug
                    jitReady = JITBootstrap.ready
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                }
            }
        }
    }

    @ViewBuilder private var notices: some View {
        if Device.chipTooOld {
            NoticeRow(symbol: "exclamationmark.triangle.fill", text: Device.chipTooOldText, tint: Desk.bad)
        }
        if installer.busy {
            Button { tab = .install } label: {
                NoticeRow(symbol: "arrow.down.circle.fill", text: "Installing \(installer.machineName)…", trailing: "See")
            }
        }
        if !installer.busy {
            ForEach(MacOSRelease.all.filter { downloads.status[$0.id] != nil }) { r in
                let s = downloads.status[r.id]!
                Button { tab = .install } label: {
                    NoticeRow(symbol: s.paused ? "pause.circle.fill" : "arrow.down.circle.fill",
                              text: "\(r.name): \(s.paused ? "download paused at" : "downloading,") \(Int(s.fraction * 100))%", trailing: "See")
                }
            }
        }
        if let name = machines.importing {
            NoticeRow(symbol: "square.and.arrow.down.fill",
                      text: machines.unpacked > 0 ? String(format: "Unpacking %@'s disk… %.0f%%", name, machines.unpacked * 100) : "Bringing in \(name)…")
        }
        if let error = machines.lastError {
            NoticeRow(symbol: "exclamationmark.triangle.fill", text: error, tint: Desk.bad)
        }
    }

    private var firstMac: some View {
        VStack(spacing: 14) {
            Image(systemName: "macwindow.on.rectangle")
                .font(.system(size: 54, weight: .light))
                .foregroundStyle(Desk.accentLight)
                .frame(maxWidth: .infinity)
                .frame(height: 190)
                .background(Desk.raised.opacity(0.5), in: RoundedRectangle(cornerRadius: 18, style: .continuous))
            Text("No Mac yet").font(.system(size: 24, weight: .bold))
            Text("Install macOS on this \(Device.name), or bring in a Mac made on a computer.")
                .font(.system(size: 15)).foregroundStyle(Desk.dim).multilineTextAlignment(.center)
            Button("Install macOS") { tab = .install }.buttonStyle(DeskButtonStyle())
            Button("Bring in a Mac") { picking = true }
                .font(.system(size: 15, weight: .semibold)).foregroundStyle(Desk.accentLight)
        }
        .padding(14)
        .deskCard(radius: 30)
    }

    @ViewBuilder private func others(besides shown: VirtualMac) -> some View {
        let rest = machines.all.filter { $0.id != shown.id }
        if !rest.isEmpty {
            DeskHeading("Other Macs")
            VStack(spacing: 0) {
                ForEach(rest) { other in
                    Button { chosen = other.id } label: {
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(other.name).font(.system(size: 16, weight: .medium)).foregroundStyle(Desk.text)
                                Text(suspendedMacs.ids.contains(other.id) ? "Suspended · \(other.summary)" : other.summary)
                                    .font(.system(size: 13)).foregroundStyle(Desk.dim)
                            }
                            Spacer()
                            Image(systemName: "chevron.right").font(.system(size: 13, weight: .semibold)).foregroundStyle(Desk.faint)
                        }
                        .padding(.horizontal, 17)
                        .padding(.vertical, 12)
                        .contentShape(Rectangle())
                    }
                    if other.id != rest.last?.id { Rectangle().fill(Desk.hairline).frame(height: 0.5) }
                }
            }
            .deskCard()
        }
    }
}

extension VirtualMac {
    var bytesUsed: Int64 {
        Int64((try? disk.resourceValues(forKeys: [.totalFileAllocatedSizeKey]))?.totalFileAllocatedSize ?? 0)
    }

    var lastUsedText: String {
        guard let lastUsed else { return "Never" }
        if Calendar.current.isDateInToday(lastUsed) { return "Today" }
        if Calendar.current.isDateInYesterday(lastUsed) { return "Yesterday" }
        return lastUsed.formatted(.dateTime.month(.abbreviated).day())
    }

    var summary: String {
        var parts: [String] = []
        if let osName { parts.append(osName) }
        parts.append("\(ramMB / 1024) GB")
        return parts.joined(separator: " · ")
    }
}

/// The picture of the Mac, its name, and Start.
private struct MacHero: View {
    let mac: VirtualMac
    let jitReady: Bool
    /// Runs once JIT is on: NeedsJITButton turns it on first when it is off.
    let start: () -> Void
    let jitNote: String?
    @AppStorage("henge.cpus") private var cpus = 4

    @ObservedObject private var suspendedMacs = SuspendedMacs.shared
    /// Suspended: Start carries on from where it was (MacSuspend.boot), so it says Resume.
    private var suspended: Bool { suspendedMacs.ids.contains(mac.id) }

    var body: some View {
        VStack(spacing: 0) {
            NavigationLink(value: mac) {
                VStack(spacing: 0) {
                    preview
                    HStack {
                        Text(mac.name).font(.system(size: 24, weight: .bold)).foregroundStyle(Desk.text).lineLimit(1)
                        Spacer()
                        JITBadge(ready: jitReady)
                        Image(systemName: "chevron.right").font(.system(size: 13, weight: .semibold)).foregroundStyle(Desk.faint)
                    }
                    .padding(.top, 15)
                    .padding(.horizontal, 5)
                    Text("\(mac.summary) · \(cpus) CPUs")
                        .font(.system(size: 15))
                        .foregroundStyle(Desk.dim)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 5)
                        .padding(.top, 3)
                }
            }
            .buttonStyle(.plain)
            if suspended {
                Label("Suspended. Resume carries on where you left off, with its apps and windows open.", systemImage: "moon.zzz.fill")
                    .font(.system(size: 14, weight: .medium))
                    .foregroundStyle(Desk.accentLight)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 5)
                    .padding(.top, 10)
            }
            if let jitNote {
                Text(jitNote).font(.system(size: 13)).foregroundStyle(Desk.dim)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 5)
                    .padding(.top, 10)
            }
            NeedsJITButton(title: suspended ? "Resume" : "Start", systemImage: "play.fill", takeOver: {
                if suspended { ResumePicture.preload(ResumePicture.screen(for: mac)) } // ready for the session's first frame
                guard Relaunch.needed else { return false }
                Relaunch.shared.start(mac) // a Mac has run in this launch: a fresh Henge starts this one
                return true
            }, action: start)
                .padding(.top, 15)
        }
        .padding(14)
        .deskCard(radius: 30)
    }

    /// What the Mac's screen shows now, as a Mac in a dock would: a suspended Mac's screen as it was, frozen;
    /// a Mac that is off, a dark screen. Its last desktop would say it is still on.
    @ViewBuilder private var preview: some View {
        ZStack {
            if suspended, let image = UIImage(contentsOfFile: MacPreview.file(for: mac).path) {
                Image(uiImage: image).resizable().aspectRatio(contentMode: .fill)
            } else {
                Color.black
                VStack(spacing: 8) {
                    Image(systemName: "power").font(.system(size: 34, weight: .light))
                    Text(mac.lastUsed == nil ? "Never Started" : "Shut Down").font(.system(size: 14, weight: .medium))
                }
                .foregroundStyle(Color.white.opacity(0.3))
            }
        }
        .aspectRatio(mac.aspect, contentMode: .fit)
        .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous).stroke(Color.white.opacity(0.14), lineWidth: 0.5))
        .accessibilityHidden(true)
    }
}

private struct FactTile: View {
    let title: String
    let value: String
    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(.system(size: 13)).foregroundStyle(Desk.dim)
            Text(value).font(.system(size: 21, weight: .bold)).foregroundStyle(Desk.text).lineLimit(1).minimumScaleFactor(0.7)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(15)
        .deskCard()
    }
}

struct NoticeRow: View {
    let symbol: String
    let text: String
    var trailing: String?
    var tint: Color = Desk.accentLight

    var body: some View {
        HStack(spacing: 11) {
            Image(systemName: symbol).foregroundStyle(tint)
            Text(text).font(.system(size: 15)).foregroundStyle(Desk.text).multilineTextAlignment(.leading)
            Spacer(minLength: 4)
            if let trailing { Text(trailing).font(.system(size: 15, weight: .semibold)).foregroundStyle(Desk.accentLight) }
        }
        .padding(14)
        .deskCard()
    }
}
