// SPDX-License-Identifier: GPL-2.0-or-later
import Combine
import SwiftUI

/// Downloads as seen from inside a running Mac, where the Install tab can't be reached without shutting it down.
/// Nothing shows while nothing downloads: the toolbar gains a button only while a download is on (its ring is how
/// far it has got), and a short word over the Mac says when one finishes or stops by itself.
@MainActor
final class DownloadWatch: ObservableObject {
    static let shared = DownloadWatch()

    struct Notice: Equatable, Identifiable {
        enum Kind { case done, stopped }
        let id = UUID()
        let kind: Kind
        let title: String
        let detail: String
    }

    /// Finished while this Mac has been on and not yet seen in the panel: the button stays, with a tick, until it is.
    @Published private(set) var finished: [String] = []
    /// What is said over the Mac for a few seconds.
    @Published private(set) var notice: Notice?
    /// Whether the toolbar has the button: a download on, or a finished one not seen. Published only as it changes, so
    /// the Mac's screen (SessionView) isn't redrawn at every bit of progress; the button itself watches the progress.
    @Published private(set) var toolWanted = false

    private var last: [String: ImageDownloads.Status]
    private var watching: AnyCancellable?

    private init() {
        last = ImageDownloads.shared.status
        toolWanted = !last.isEmpty
        // ImageDownloads sets its status on the main thread: handled straight away, in the same update, so the button
        // never drops out for a moment between the last download leaving the list and its tick arriving.
        watching = ImageDownloads.shared.$status
            .sink { [weak self] now in MainActor.assumeIsolated { self?.changed(now) } }
    }

    static func name(_ id: String) -> String { MacOSRelease.all.first { $0.id == id }?.name ?? "macOS \(id)" }

    /// A Mac has started: what finished before it is the Install tab's business, not this Mac's toolbar.
    func sessionStarted() {
        finished.removeAll()
        notice = nil
        last = ImageDownloads.shared.status
        update()
    }

    /// The panel has been seen: the finished ones have been told.
    func seen() {
        finished.removeAll()
        update()
    }

    private func changed(_ now: [String: ImageDownloads.Status]) {
        for (id, before) in last {
            if let after = now[id] {
                if before.problem == nil, let problem = after.problem {
                    say(.stopped, "\(Self.name(id)) download stopped", problem)
                }
            } else if FileManager.default.fileExists(atPath: Installer.imageDirectory.appendingPathComponent("\(id).ipsw").path) {
                // Gone from the list with the image in place: finished. (Gone without it: deleted.)
                if !finished.contains(id) { finished.append(id) }
                say(.done, "\(Self.name(id)) downloaded", Self.afterwards)
            }
        }
        last = now
        update()
    }

    private func update() {
        let wanted = !last.isEmpty || !finished.isEmpty
        if wanted != toolWanted { toolWanted = wanted }
    }

    /// What happens to a finished download while a Mac is on. True both when an install is waiting for this Mac to
    /// shut down (Henge then restarts and installs it) and when it was only a download (Install tab).
    static let afterwards = "Ready to install once this Mac is shut down."

    private func say(_ kind: Notice.Kind, _ title: String, _ detail: String) {
        let said = Notice(kind: kind, title: title, detail: detail)
        notice = said
        UIAccessibility.post(notification: .announcement, argument: "\(title). \(detail)")
        DispatchQueue.main.asyncAfter(deadline: .now() + 6) { [weak self] in
            if self?.notice?.id == said.id { self?.notice = nil }
        }
    }
}

/// The toolbar's download button: there only while a download is on, or one has finished and not been seen.
/// A ring round the arrow is how far it has got; paused, the ring dims; stopped by a problem, it turns red.
struct DownloadTool: View {
    var tint: Color
    @Binding var open: Bool
    @ObservedObject private var downloads = ImageDownloads.shared
    @ObservedObject private var watch = DownloadWatch.shared

    private var active: [ImageDownloads.Status] { Array(downloads.status.values) }
    private var fraction: Double {
        let total = active.reduce(Int64(0)) { $0 + $1.total }
        return total > 0 ? Double(active.reduce(Int64(0)) { $0 + $1.received }) / Double(total) : 1
    }
    private var stopped: Bool { active.contains { $0.problem != nil } }
    private var allPaused: Bool { !active.isEmpty && active.allSatisfy(\.paused) }
    private var done: Bool { active.isEmpty && !watch.finished.isEmpty }

    var body: some View {
        Button { open = true } label: {
            ZStack {
                Circle().stroke(tint.opacity(0.22), lineWidth: 2.5)
                if !done {
                    Circle()
                        .trim(from: 0, to: max(fraction, 0.02))
                        .stroke(ringColor, style: StrokeStyle(lineWidth: 2.5, lineCap: .round))
                        .rotationEffect(.degrees(-90))
                        .animation(.easeInOut(duration: 0.4), value: fraction)
                }
                Image(systemName: symbol)
                    .font(.system(size: 13, weight: .bold))
                    .foregroundStyle(done ? Desk.good : stopped ? Desk.bad : tint)
            }
            .frame(width: 26, height: 26)
            .frame(width: 44, height: 44)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(accessibilityText)
        .accessibilityHint("Shows downloads")
        .popover(isPresented: $open) {
            DownloadsPanel()
                .presentationCompactAdaptation(.popover) // a bubble on iPhone too, as Shut down's is
        }
        .onChange(of: open) { showing in if !showing { watch.seen() } }
        .transition(.opacity.combined(with: .scale(scale: 0.8)))
    }

    private var symbol: String { done ? "checkmark" : stopped ? "exclamationmark" : allPaused ? "pause.fill" : "arrow.down" }
    private var ringColor: Color { stopped ? Desk.bad : allPaused ? tint.opacity(0.5) : Desk.accentLight }
    private var accessibilityText: String {
        if done { return watch.finished.map(DownloadWatch.name).joined(separator: ", ") + " downloaded" }
        if stopped { return "A download stopped" }
        return "Downloads, \(Int(fraction * 100)) percent" + (allPaused ? ", paused" : "")
    }
}

/// What the download button opens: each download with Pause or Resume, and the ones just finished.
struct DownloadsPanel: View {
    @ObservedObject private var downloads = ImageDownloads.shared
    @ObservedObject private var watch = DownloadWatch.shared
    /// Finished ones are kept listed while the panel is open, and the ones that finish while it is (seen() clears them
    /// from the watch as it closes).
    @State private var shownFinished = Set(DownloadWatch.shared.finished)

    private func sortedByName(_ ids: some Sequence<String>) -> [String] { ids.sorted { DownloadWatch.name($0) < DownloadWatch.name($1) } }
    private var finishedIDs: [String] {
        sortedByName(shownFinished.union(watch.finished).filter { downloads.status[$0] == nil })
    }

    var body: some View {
        // Scrolls only when it is too tall for the bubble (two downloads with problems, a phone on its side).
        ViewThatFits(in: .vertical) {
            content
            ScrollView { content }
        }
        .frame(width: 340)
        .preferredColorScheme(.dark)
        .onChange(of: watch.finished) { shownFinished.formUnion($0) }
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Downloads").font(.system(size: 17, weight: .semibold))
            if downloads.status.isEmpty && finishedIDs.isEmpty {
                Text("No downloads right now.").font(.system(size: 15)).foregroundStyle(Desk.dim)
            }
            ForEach(sortedByName(downloads.status.keys), id: \.self) { id in
                if let s = downloads.status[id] { row(id, s) }
            }
            ForEach(finishedIDs, id: \.self) { id in
                VStack(alignment: .leading, spacing: 4) {
                    Label("\(DownloadWatch.name(id)) downloaded", systemImage: "checkmark.circle.fill")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(Desk.good)
                    Text(DownloadWatch.afterwards).font(.system(size: 13)).foregroundStyle(Desk.dim)
                }
            }
            Text("Downloads carry on while the Mac runs, when you switch apps and when the screen locks. To delete one, use the Install tab.")
                .font(.system(size: 12))
                .foregroundStyle(Desk.faint)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(20)
        .frame(width: 340, alignment: .leading)
        .foregroundStyle(Desk.text)
        .animation(.easeInOut(duration: 0.2), value: downloads.status)
    }

    private func row(_ id: String, _ s: ImageDownloads.Status) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline) {
                Text(s.problem != nil ? "\(DownloadWatch.name(id)), stopped" : DownloadWatch.name(id))
                    .font(.system(size: 15, weight: .medium))
                    .lineLimit(1)
                Spacer(minLength: 8)
                Text("\(Int(s.fraction * 100))%").font(.system(size: 15, weight: .semibold)).monospacedDigit()
            }
            DeskBar(fraction: s.fraction, height: 6)
            Text(s.summary).font(.system(size: 13)).foregroundStyle(Desk.dim).monospacedDigit()
            if let problem = s.problem {
                Text(problem).font(.system(size: 13)).foregroundStyle(Desk.bad).fixedSize(horizontal: false, vertical: true)
            }
            Button {
                if s.paused { downloads.resume(id) } else { downloads.pause(id) }
            } label: {
                Label(s.paused ? "Resume" : "Pause", systemImage: s.paused ? "play.fill" : "pause.fill")
                    .font(.system(size: 15, weight: .semibold))
            }
            .buttonStyle(.bordered)
            .buttonBorderShape(.capsule)
            .tint(Desk.accentLight)
        }
    }
}

/// The few seconds' word over the Mac. It takes no touches, so it never gets in the way of the Mac.
struct DownloadNoticeView: View {
    let notice: DownloadWatch.Notice

    var body: some View {
        HStack(alignment: .top, spacing: 11) {
            Image(systemName: notice.kind == .done ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                .font(.system(size: 18))
                .foregroundStyle(notice.kind == .done ? Desk.good : Desk.bad)
            VStack(alignment: .leading, spacing: 2) {
                Text(notice.title).font(.system(size: 15, weight: .semibold))
                Text(notice.detail).font(.system(size: 13)).foregroundStyle(.white.opacity(0.75)).lineLimit(2)
            }
        }
        .foregroundStyle(.white)
        .padding(.horizontal, 18).padding(.vertical, 11)
        .frame(maxWidth: 440, alignment: .leading)
        .fixedSize(horizontal: false, vertical: true)
        .deskGlass(in: RoundedRectangle(cornerRadius: 20, style: .continuous))
        .allowsHitTesting(false)
        .accessibilityElement(children: .combine)
        .transition(.opacity.combined(with: .move(edge: .top)))
    }
}
