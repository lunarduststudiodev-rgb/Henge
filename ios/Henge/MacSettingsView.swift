import SwiftUI

/// One Mac: what it has, and what can be done with it.
struct MacSettingsView: View {
    @ObservedObject private var machines = Machines.shared
    @Environment(\.dismiss) private var dismiss
    @State private var mac: VirtualMac
    @State private var confirmDelete = false
    @State private var movingExport = false
    @AppStorage("henge.cpus") private var cpus = 4
    private let rosettaSupported = henge_vm_rosetta_supported() != 0

    init(mac: VirtualMac) { _mac = State(initialValue: mac) }

    var body: some View {
        ScrollView {
            VStack(spacing: 0) {
                DeskHeading("Name")
                TextField("Name", text: $mac.name)
                    .font(.system(size: 16))
                    .submitLabel(.done)
                    .padding(.horizontal, 17)
                    .padding(.vertical, 14)
                    .deskCard()
                    .onSubmit(save)

                DeskHeading("Memory")
                VStack(alignment: .leading, spacing: 12) {
                    HStack(alignment: .firstTextBaseline) {
                        Text("\(mac.ramMB / 1024) GB").font(.system(size: 26, weight: .bold))
                        Spacer()
                        Text("Up to \(Device.suggestedRAMGB) GB suggested on this \(Device.name)")
                            .font(.system(size: 13)).foregroundStyle(Desk.dim).multilineTextAlignment(.trailing)
                    }
                    Stepper("Memory", value: Binding(get: { mac.ramMB / 1024 }, set: { mac.ramMB = $0 * 1024; save() }), in: Device.memoryChoices)
                        .font(.system(size: 16))
                    MemoryNote(gb: mac.ramMB / 1024)
                }
                .padding(17)
                .deskCard()

                DeskHeading("Processors")
                Stepper(value: $cpus, in: 1...6) {
                    HStack {
                        Text("CPUs").font(.system(size: 16))
                        Spacer()
                        Text("\(cpus)").font(.system(size: 16, weight: .semibold))
                    }
                }
                .padding(.horizontal, 17)
                .padding(.vertical, 10)
                .deskCard()

                if Device.fittingScreen.height != 1080 {
                    DeskHeading("Screen")
                    VStack(alignment: .leading, spacing: 12) {
                        Picker("Screen", selection: Binding(get: {
                            if mac.isRetina { return "retina" }
                            if mac.screen.width == Device.performanceScreen.width,
                               mac.screen.height == Device.performanceScreen.height { return "performance" }
                            if mac.screen.width == 1920, mac.screen.height == 1080 { return "wide" }
                            return "fit"
                        }, set: { choice in
                            mac.retina = choice == "retina"
                            switch choice {
                            case "retina": break
                            case "performance": (mac.screenWidth, mac.screenHeight) = Device.performanceScreen
                            case "wide": (mac.screenWidth, mac.screenHeight) = (1920, 1080)
                            default: (mac.screenWidth, mac.screenHeight) = Device.fittingScreen
                            }
                            save()
                        })) {
                            if Device.isPad {
                                Text("Retina").tag("retina")
                                Text("Performance").tag("performance")
                            }
                            Text("Fit this \(Device.name)").tag("fit")
                            Text("16:9").tag("wide")
                        }
                        .pickerStyle(.segmented)
                        Text(mac.isRetina
                             ? "This \(Device.name)'s own \(mac.screen.width) × \(mac.screen.height) pixels, as sharp as its own apps. To make things larger or fit more on screen, choose a size in macOS System Settings › Displays; it changes straight away. Changes here apply the next time this Mac starts."
                             : "\(mac.screen.width) × \(mac.screen.height). \(Device.isPad ? "Performance renders about 30% fewer pixels. " : "")Changes apply the next time this Mac starts.")
                            .font(.system(size: 13)).foregroundStyle(Desk.dim)
                    }
                    .padding(17)
                    .deskCard()
                }

                DeskHeading("Experimental")
                VStack(alignment: .leading, spacing: 10) {
                    Toggle(isOn: Binding(get: { rosettaSupported && mac.rosetta == true },
                                         set: { mac.rosetta = $0 ? true : nil; save() })) {
                        Text("Rosetta for Intel apps").font(.system(size: 16))
                    }
                    .disabled(!rosettaSupported)
                    Text(rosettaSupported
                         ? "Lets this Mac run apps made for Intel Macs. Experimental: those apps are slower and some may not work. It applies the next time this Mac starts afresh; a suspended Mac carries on as it was."
                         : "Not available on this \(Device.name): its chip lacks a feature this needs (M2 and later have it).")
                        .font(.system(size: 13)).foregroundStyle(Desk.dim)
                }
                .padding(17)
                .deskCard()

                DeskHeading("Storage")
                VStack(spacing: 11) {
                    HStack {
                        Text("\(ByteText.string(UInt64(max(mac.bytesUsed, 0)))) used").font(.system(size: 16))
                        Spacer()
                        Text("of \(ByteText.string(UInt64(max(mac.diskBytes, 0))))").font(.system(size: 16)).foregroundStyle(Desk.dim)
                    }
                    DeskBar(fraction: Double(mac.bytesUsed) / Double(max(mac.diskBytes, 1)))
                }
                .padding(17)
                .deskCard()

                KeptDisks(mac: mac)

                DeskHeading("Manage")
                manage
            }
            .padding(.horizontal, 16)
            .padding(.bottom, 24)
            .deskColumn()
        }
        .background(Desk.page)
        .navigationTitle(mac.name)
        .navigationBarTitleDisplayMode(.inline)
        .onDisappear(perform: save)
        .fileMover(isPresented: $movingExport, file: machines.lastExport) { result in
            switch result {
            case .success: machines.lastExport = nil
            case .failure(let error): machines.lastError = error.localizedDescription
            }
        }
        .confirmationDialog("Delete \(mac.name)?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                machines.delete(mac.id)
                dismiss()
            }
        } message: {
            Text("Its disk is removed from this \(Device.name).")
        }
    }

    private var manage: some View {
        VStack(spacing: 0) {
            if let name = machines.exporting {
                VStack(alignment: .leading, spacing: 9) {
                    Text("Exporting \(name)… keep Henge open").font(.system(size: 15))
                    DeskBar(fraction: machines.exported)
                }
                .padding(17)
            } else {
                row("Export", symbol: "square.and.arrow.up", tint: Desk.accentLight) { machines.export(mac) }
            }
            if let folder = machines.lastExport {
                divider
                VStack(alignment: .leading, spacing: 8) {
                    Text("Saved in Files, under Henge, Exports, \(folder.lastPathComponent). Deleting Henge deletes it too, so move it somewhere else first.")
                        .font(.system(size: 13)).foregroundStyle(Desk.dim)
                    Button("Move Out of Henge…") { movingExport = true }
                        .font(.system(size: 15, weight: .semibold)).foregroundStyle(Desk.accentLight)
                }
                .padding(17)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            divider
            row("Delete this Mac", symbol: nil, tint: Desk.bad) { confirmDelete = true }
        }
        .deskCard()
    }

    private var divider: some View { Rectangle().fill(Desk.hairline).frame(height: 0.5) }

    private func row(_ title: String, symbol: String?, tint: Color, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack {
                Text(title).font(.system(size: 16)).foregroundStyle(tint)
                Spacer()
                if let symbol { Image(systemName: symbol).foregroundStyle(tint) }
            }
            .padding(.horizontal, 17)
            .padding(.vertical, 14)
            .contentShape(Rectangle())
        }
    }

    private func save() {
        if mac.name.isEmpty { mac.name = "Mac" }
        machines.update(mac)
    }
}

/// The disks Carry On kept aside for this Mac (MacAutoSave.asideDisk): the person's own work from after an auto-save,
/// never deleted by Henge. Shown only when there are any: when each was last written, what deleting it would free,
/// and Use This Disk (swapped with the Mac's) or Delete.
private struct KeptDisks: View {
    let mac: VirtualMac
    @State private var disks: [(url: URL, stamp: MacAutoSave.Stamp, frees: Int64)] = []
    @State private var deleting: (URL, MacAutoSave.Stamp)?
    @State private var using: (URL, MacAutoSave.Stamp)?
    @State private var problem: String?

    var body: some View {
        Group {
            if !disks.isEmpty {
                DeskHeading("Disks Kept by Carry On")
                VStack(alignment: .leading, spacing: 0) {
                    Text("When \(mac.name) carried on from an auto-save, the disk it had then was kept here, so nothing on it was lost. "
                         + "Use This Disk puts one back (the disk it has now is kept here instead).")
                        .font(.system(size: 13)).foregroundStyle(Desk.dim)
                        .padding(17)
                    ForEach(disks, id: \.url) { disk in
                        let written = disk.stamp.date
                        Rectangle().fill(Desk.hairline).frame(height: 0.5)
                        VStack(alignment: .leading, spacing: 10) {
                            HStack(alignment: .firstTextBaseline) {
                                Text("Last written \(written.formatted(date: .abbreviated, time: .shortened))").font(.system(size: 16))
                                Spacer()
                                Text("\(ByteText.string(UInt64(max(disk.frees, 0)))) of storage")
                                    .font(.system(size: 13)).foregroundStyle(Desk.dim)
                            }
                            HStack(spacing: 22) {
                                Button("Use This Disk") { using = (disk.url, disk.stamp) }
                                    .foregroundStyle(Desk.accentLight)
                                Button("Delete") { deleting = (disk.url, disk.stamp) }
                                    .foregroundStyle(Desk.bad)
                            }
                            .font(.system(size: 15, weight: .semibold))
                            .buttonStyle(.borderless)
                        }
                        .padding(17)
                    }
                    if let problem {
                        Text(problem).font(.system(size: 13)).foregroundStyle(Desk.bad).padding([.horizontal, .bottom], 17)
                    }
                }
                .deskCard()
            }
        }
        .task { await load() }
        .confirmationDialog("Delete this disk?", isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } }),
                            titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                if let (url, stamp) = deleting { problem = MacAutoSave.deleteAside(url, listed: stamp, for: mac) }
                deleting = nil
                Task { await load() }
            }
        } message: {
            Text("Anything saved on it that isn't on \(mac.name)'s disk now is gone for good.")
        }
        .confirmationDialog("Use this disk?", isPresented: Binding(get: { using != nil }, set: { if !$0 { using = nil } }),
                            titleVisibility: .visible) {
            Button("Use This Disk") {
                if let (url, stamp) = using { problem = MacAutoSave.useAside(url, listed: stamp, for: mac) }
                using = nil
                Task { await load() }
            }
        } message: {
            Text("\(mac.name) starts from this disk next time. The disk it has now is kept here in its place, and its auto-save, if it has one, is deleted.")
        }
    }

    private func load() async {
        let mac = mac
        // A Carry On that didn't finish is undone first: until then a disk listed here may be the Mac's newest
        if MacAutoSave.starting == nil, henge_vm_state() != 1 { MacAutoSave.undoUnfinished(mac) }
        disks = await Task.detached(priority: .utility) {
            MacAutoSave.asideDisks(mac.id).compactMap { url in
                guard let stamp = MacAutoSave.Stamp.of(url) else { return nil }
                return (url, stamp, MacAutoSave.privateSize(url.path))
            }.sorted { $0.stamp.date > $1.stamp.date }
        }.value
    }
}
