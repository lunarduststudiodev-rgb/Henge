import SwiftUI

/// The macOS restore images on this device: finished ones and downloads still going, from Henge's "Restore images" folder.
/// Kept here so the Install tab and the Downloads screen both redraw when one is deleted.
@MainActor
final class RestoreImages: ObservableObject {
    static let shared = RestoreImages()

    struct Item: Identifiable, Equatable {
        let id: String        // build
        let name: String
        /// Bytes on this device: the whole image, or what a download has so far.
        let bytes: Int64
        /// A download not finished yet (ImageDownloads has its progress).
        let partial: Bool
    }

    @Published private(set) var items: [Item] = []
    @Published private(set) var freeBytes: Int64 = 0

    var totalBytes: Int64 { items.reduce(0) { $0 + $1.bytes } }

    private init() { refresh() }

    static func name(_ id: String) -> String { MacOSRelease.all.first { $0.id == id }?.name ?? "macOS (build \(id))" }

    /// Looks at the folder again. Cheap: a handful of files.
    func refresh() {
        let fm = FileManager.default
        let dir = Installer.imageDirectory
        var found: [String: Item] = [:]
        for name in (try? fm.contentsOfDirectory(atPath: dir.path)) ?? [] where name.hasSuffix(".ipsw") {
            let id = String(name.dropLast(".ipsw".count))
            let size = (try? dir.appendingPathComponent(name).resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0
            found[id] = Item(id: id, name: Self.name(id), bytes: Int64(size), partial: false)
        }
        // A download that is still going (an older finished copy, if any, is replaced when it ends).
        for (id, s) in ImageDownloads.shared.status where found[id] == nil {
            found[id] = Item(id: id, name: Self.name(id), bytes: s.received, partial: true)
        }
        // Newest release first, then anything Henge no longer offers.
        let order = Array(MacOSRelease.all.map(\.id).reversed())
        let sorted = found.values.sorted { a, b in
            let ia = order.firstIndex(of: a.id) ?? Int.max, ib = order.firstIndex(of: b.id) ?? Int.max
            return ia != ib ? ia < ib : a.id > b.id
        }
        if sorted != items { items = sorted }
        freeBytes = (try? URL(fileURLWithPath: NSHomeDirectory()).resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?
            .volumeAvailableCapacityForImportantUsage ?? 0
    }

    /// Whether an install is using it right now (deleting it would break the install).
    func inUse(_ id: String) -> Bool { Installer.shared.busy && Installer.shared.imageInUse == id }

    /// The whole image, or the part of a download so far.
    func delete(_ id: String) {
        guard !inUse(id) else { return }
        Installer.deleteImage(id)
        HengeLog.log("download", "\(id): deleted from Downloads")
        items.removeAll { $0.id == id }
        // ImageDownloads deletes the partial download on its own queue; look again once it has.
        Task {
            try? await Task.sleep(nanoseconds: 500_000_000)
            refresh()
        }
    }
}

/// Every macOS download on this device, with how much space each takes, and a way to pause, resume or delete them.
struct DownloadsView: View {
    @ObservedObject private var images = RestoreImages.shared
    @ObservedObject private var downloads = ImageDownloads.shared
    @ObservedObject private var installer = Installer.shared
    @State private var confirmDelete: RestoreImages.Item?
    @State private var confirmDeleteAll = false

    var body: some View {
        Form {
            if images.items.isEmpty {
                Section {
                    VStack(spacing: 10) {
                        Image(systemName: "arrow.down.circle").font(.system(size: 40)).foregroundStyle(Desk.dim)
                        Text("No downloads").font(.system(size: 17, weight: .semibold))
                        Text("macOS downloads from the Install tab show here, so you can delete them when you no longer need them.")
                            .font(.footnote).foregroundStyle(.secondary).multilineTextAlignment(.center)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 18)
                }
                .listRowBackground(Desk.card)
            } else {
                Section {
                    ForEach(images.items) { item in
                        row(item)
                            .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                                if !images.inUse(item.id) {
                                    Button(role: .destructive) { confirmDelete = item } label: { Label("Delete", systemImage: "trash") }
                                }
                            }
                    }
                } header: {
                    Text("macOS Downloads")
                } footer: {
                    Text("A Mac that is already installed doesn't need its download: you can delete it to free space, and download it again from the Install tab to make another Mac.")
                }
                .listRowBackground(Desk.card)

                Section {
                    LabeledContent("Downloads use", value: ByteText.string(images.totalBytes))
                    LabeledContent("Free on this \(Device.name)", value: ByteText.string(images.freeBytes))
                    if images.items.count > 1 {
                        Button("Delete All Downloads", role: .destructive) { confirmDeleteAll = true }
                            .disabled(images.items.allSatisfy { images.inUse($0.id) })
                    }
                } header: {
                    Text("Space")
                }
                .listRowBackground(Desk.card)
            }
        }
        .scrollContentBackground(.hidden)
        .background(Desk.page)
        .navigationTitle("Downloads")
        .confirmationDialog(confirmDelete.map { "Delete \($0.name)?" } ?? "",
                            isPresented: Binding(get: { confirmDelete != nil }, set: { if !$0 { confirmDelete = nil } }),
                            titleVisibility: .visible) {
            Button("Delete Download", role: .destructive) {
                if let item = confirmDelete { images.delete(item.id) }
                confirmDelete = nil
            }
        } message: {
            if let item = confirmDelete {
                Text("This frees \(ByteText.string(item.bytes)). Your Macs aren't affected. To install it again, Henge downloads it again.")
            }
        }
        .confirmationDialog("Delete all downloads?", isPresented: $confirmDeleteAll, titleVisibility: .visible) {
            Button("Delete All", role: .destructive) {
                for item in images.items where !images.inUse(item.id) { images.delete(item.id) }
            }
        } message: {
            Text("This frees about \(ByteText.string(images.items.filter { !images.inUse($0.id) }.reduce(0) { $0 + $1.bytes })). Your Macs aren't affected.")
        }
        .onAppear { images.refresh() }
        .onChange(of: downloads.status) { _ in images.refresh() }
    }

    private func row(_ item: RestoreImages.Item) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .firstTextBaseline) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(item.name).font(.system(size: 16, weight: .medium))
                    Text(subtitle(item)).font(.system(size: 13)).foregroundStyle(color(item))
                }
                Spacer()
                Text(ByteText.string(item.bytes)).font(.system(size: 15, weight: .semibold)).monospacedDigit()
            }
            if item.partial, let s = downloads.status[item.id] {
                DeskBar(fraction: s.fraction, height: 6)
                Text(s.summary).font(.system(size: 12)).foregroundStyle(.secondary).monospacedDigit()
                if let problem = s.problem {
                    Text(problem).font(.system(size: 12)).foregroundStyle(Desk.bad)
                }
            }
            HStack(spacing: 20) {
                if item.partial, let s = downloads.status[item.id] { // an install waits through a pause
                    Button {
                        if s.paused { downloads.resume(item.id) } else { downloads.pause(item.id) }
                    } label: {
                        Label(s.paused ? "Resume" : "Pause", systemImage: s.paused ? "play.fill" : "pause.fill")
                    }
                    .foregroundStyle(Desk.accentLight)
                }
                Spacer()
                if images.inUse(item.id) {
                    Text("In use by the install").font(.system(size: 13)).foregroundStyle(.secondary)
                } else {
                    Button(role: .destructive) { confirmDelete = item } label: { Label("Delete", systemImage: "trash") }
                        .foregroundStyle(Desk.bad)
                }
            }
            .font(.system(size: 15, weight: .semibold))
            .buttonStyle(.borderless) // each button on its own, not the whole row
        }
        .padding(.vertical, 4)
    }

    private func subtitle(_ item: RestoreImages.Item) -> String {
        guard item.partial else {
            return MacOSRelease.all.contains { $0.id == item.id } ? "Downloaded, ready to install" : "Downloaded; Henge no longer offers this version"
        }
        guard let s = downloads.status[item.id] else { return "Downloading" }
        let percent = Int(s.fraction * 100)
        return s.problem != nil ? "Stopped at \(percent)%" : s.paused ? "Paused at \(percent)%" : "Downloading, \(percent)%"
    }

    private func color(_ item: RestoreImages.Item) -> Color {
        guard item.partial else { return Desk.good }
        return downloads.status[item.id]?.problem != nil ? Desk.bad : Desk.accentLight
    }
}
