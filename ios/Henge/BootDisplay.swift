import SwiftUI
import QuartzCore
import CoreGraphics
import ImageIO

/// The Mac's screen: what its GPU draws once macOS is up. Before that, a Mac starting shows the
/// Apple logo where macOS will draw it (BootLogo), so macOS's own logo lands on it and only its
/// progress bar appears, as on a Mac. A Mac going on from a suspend isn't starting: it shows the
/// screen it had (saved as it paused) until macOS draws again, as a Mac waking from sleep does.
/// The firmware's screen (a 640 x 1136 picture at 1x with just a progress bar, nothing like
/// macOS's layout) is shown only to the installer, whose progress nothing else shows.
struct BootDisplay: View {
    let display: DisplayRouter
    let role: DisplayTargetSelection.Role
    /// The id this view's layer connects to the router under.
    var surface = UUID()
    /// The firmware's own screen until macOS's GPU takes over, instead of the boot logo.
    var firmwareScreen = false
    /// The screen the Mac had when it was suspended, if it is going on from there (MacPreview.resumeScreen).
    /// nil again when the person gives up on going on from there (Start Fresh): it starts as any Mac does.
    var resume: URL?
    /// The session is asking the person something (can't go on: Start Fresh or Keep for Later) or saying why it
    /// couldn't start: nothing is being restored meanwhile, so no "Resuming…" over it.
    var waiting = false

    init(display: DisplayRouter, role: DisplayTargetSelection.Role, surface: UUID = UUID(), firmwareScreen: Bool = false,
         resume: URL? = nil, waiting: Bool = false) {
        self.display = display
        self.role = role
        self.surface = surface
        self.firmwareScreen = firmwareScreen
        self.resume = resume
        self.waiting = waiting
        // Going on from a suspend from the very first frame, not from when the task gets going: the screen it had,
        // under "Resuming…", with nothing before it. (These are used only as the view is first made.)
        let resuming = resume != nil && !firmwareScreen && henge_vm_gpu_frames() == 0
        _resuming = State(initialValue: resuming)
        _restoring = State(initialValue: resuming && henge_vm_state() == 0)
        _resumeScreen = State(initialValue: resuming ? resume.flatMap(ResumePicture.cached) : nil)
    }

    @State private var image: CGImage?
    /// Going on from a suspend: no boot logo, ever; the screen it had, or black until macOS draws.
    @State private var resuming = false
    @State private var resumeScreen: CGImage?
    /// The Mac is being put back as it was and isn't in use yet: its memory read from storage, then macOS waking.
    @State private var restoring = false
    /// It was to go on from a suspend and started anew instead: the saved screen is out of date.
    @State private var startedAfresh = false
    @State private var logo: BootLogo.Look?
    @State private var gpuLive = henge_vm_gpu_frames() > 0
    @AppStorage("henge.matchScreenColors") private var matchScreenColors = true
    @AppStorage("henge.display.brightness") private var screenBrightness = 1.0

    var body: some View {
        ZStack {
            Color.black
            if !gpuLive {
                if resuming {
                    if let resumeScreen {
                        Image(decorative: resumeScreen, scale: 1).resizable().interpolation(.high).aspectRatio(contentMode: .fit)
                            .opacity(GPUScreen.alpha(screenBrightness))
                            .allowsHitTesting(false)
                    }
                } else if !firmwareScreen {
                    if let logo {
                        BootLogo(look: logo).opacity(GPUScreen.alpha(screenBrightness)) // as dim as the screen will be
                    }
                } else if let image {
                    Image(decorative: image, scale: 1).resizable().interpolation(.medium).aspectRatio(contentMode: .fit)
                    if image.height > image.width {
                        Image(systemName: "apple.logo")
                            .font(.system(size: 44))
                            .foregroundStyle(.white)
                    }
                } else {
                    Image(systemName: "apple.logo")
                        .font(.system(size: 44))
                        .foregroundStyle(.white)
                }
            }
            GPUScreen(display: display, role: role, surface: surface,
                      matchScreenColors: matchScreenColors, brightness: screenBrightness)
                .allowsHitTesting(false)
            if restoring && !waiting { // over macOS's own frames too: they show before it answers. As the paused screen says it.
                ResumingOverlay().allowsHitTesting(false).transition(.opacity)
            }
        }
        .onChange(of: resume) { _, now in
            guard now == nil, resuming, !gpuLive else { return }
            resuming = false // Start Fresh: the boot logo from here, as for any start
            restoring = false
            resumeScreen = nil
            startedAfresh = true
            ResumePicture.forget()
        }
        .task {
            if let resume, resuming, resumeScreen == nil { // not read ahead (ResumePicture.preload): read now
                resumeScreen = await ResumePicture.load(resume)
            }
            // Once macOS draws with the GPU the frames go straight to GPUScreen:
            // nothing to copy here any more.
            var waking = Waking()
            while !Task.isCancelled && (!gpuLive || restoring) {
                if restoring, let why = waking.done() {
                    if !why.isEmpty { HengeLog.log("display", "resumed: in use \(why)") }
                    withAnimation(.easeOut(duration: 0.25)) { restoring = false }
                }
                if gpuLive {
                    try? await Task.sleep(nanoseconds: 100_000_000)
                    continue
                }
                if henge_vm_gpu_frames() > 0 {
                    gpuLive = true
                    image = nil
                    continue
                }
                if !resuming && !firmwareScreen && henge_vm_resumed() != 0 { // another view of a Mac going on (an external display)
                    resuming = true
                    logo = nil
                }
                if resuming && henge_vm_state() == 1 && henge_vm_resumed() == 0 { // it couldn't go on: a start like any other
                    resuming = false
                    resumeScreen = nil
                    startedAfresh = true
                }
                if firmwareScreen {
                    image = Self.snapshot() ?? image
                } else if !resuming, let mode = BootLogo.Mode.current(), mode != logo?.mode {
                    logo = BootLogo.Look.load(mode) // the session has just named the screen
                }
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
            // The saved screen, once shown or out of date; kept if the Mac stays suspended (the session closed first).
            if let resume, gpuLive || startedAfresh {
                resumeScreen = nil
                try? FileManager.default.removeItem(at: resume)
            }
            if resume != nil { ResumePicture.forget() }
            // What macOS drew, for the next start: only the view on the device, and only at a start this view watched.
            if gpuLive && !firmwareScreen && !resuming && role == .device, let mode = logo?.mode { await BootLogo.learn(mode) }
        }
    }

    /// When a Mac going on from a suspend is in use, measured by the engine, not by asking macOS: it runs in a new
    /// process, so all the code macOS runs is translated afresh, and while that goes on fast macOS barely answers
    /// (taps and keys wait their turn). Once translating has stayed slow a while, macOS has woken. Henge's helper
    /// linking again is macOS saying so itself (what clears the password button too), so it ends the wait sooner when
    /// it comes; without it (none installed, or it failed) the engine's measure does. Half a minute at most: the
    /// overlay never takes a touch, so even then the Mac was never out of reach.
    private struct Waking {
        static let busy = 20_000.0  // translations a second: tens of thousands waking, a few thousand once woken
        static let calm = 2.0       // seconds slow before it counts
        static let cap = 30.0
        var running: Date?
        var samples: [(at: Date, count: UInt64)] = []
        var calmSince: Date?
        var lastLog: Date?
        var idleTicks: UInt64 = 0, idleAt: UInt64 = 0, idle = -1.0

        /// nil while it is still waking; else why it's in use ("" when it stopped or started afresh: nothing to say).
        mutating func done() -> String? {
            let state = henge_vm_state()
            if state == 2 { return "" }
            guard state == 1 else { return nil } // its memory is still being read back
            if henge_vm_resumed() == 0 { return "" }
            let now = Date()
            if running == nil { running = now }
            let up = now.timeIntervalSince(running!)
            let count = henge_vm_translations()
            samples.append((now, count))
            samples.removeAll { now.timeIntervalSince($0.at) > 1.0 }
            var rate = Double.infinity
            if let first = samples.first, now.timeIntervalSince(first.at) >= 0.8 {
                rate = Double(count &- first.count) / now.timeIntervalSince(first.at)
            }
            if rate < Self.busy { calmSince = calmSince ?? now } else { calmSince = nil }
            if lastLog == nil || now.timeIntervalSince(lastLog!) >= 1 { // how waking went, to tune the numbers by
                idle = henge_vm_idle(&idleTicks, &idleAt)
                if lastLog != nil {
                    HengeLog.log("display", String(format: "resuming: %.0f s, translating %@/s, CPUs %@ idle", up,
                                                   rate.isFinite ? String(Int(rate)) : "?",
                                                   idle < 0 ? "?" : "\(Int(idle * 100))%"))
                }
                lastLog = now
            }
            let linked = henge_share_touch_linked() != 0 || henge_share_focus() != -1 || henge_share_session() >= 0
            let at = String(format: "%.1f s after it runs", up)
            if linked { return "\(at), as its helper linked" }
            if let calmSince, now.timeIntervalSince(calmSince) >= Self.calm {
                return "\(at), as translating slowed to \(Int(rate))/s\(linked ? " (helper linked too)" : "")"
            }
            if up > Self.cap { return "\(at): still translating \(rate.isFinite ? String(Int(rate)) : "?")/s, gave up waiting" }
            return nil
        }
    }

    /// The saved screen, decoded, in the colours GPUScreen shows: sRGB matched to the display, or its values as
    /// the display's own (a Metal layer without a colour space is shown unmatched, in the panel's P3).
    nonisolated fileprivate static func picture(_ url: URL, srgb: Bool) -> CGImage? {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let image = CGImageSourceCreateImageAtIndex(source, 0, [kCGImageSourceShouldCacheImmediately: true] as CFDictionary),
              let space = CGColorSpace(name: srgb ? CGColorSpace.sRGB : CGColorSpace.displayP3) else { return nil }
        return image.copy(colorSpace: space) ?? image
    }

    static func snapshot() -> CGImage? {
        var w: Int32 = 0, h: Int32 = 0, stride: Int32 = 0
        var data = Data()
        var rc: Int32 = 1
        for _ in 0..<2 where rc == 1 { // a copy, taken by the engine: the frame it reads may be freed when its size changes
            if stride > 0 { data = Data(count: Int(stride) * Int(h)) }
            rc = data.withUnsafeMutableBytes { henge_vm_boot_display($0.bindMemory(to: UInt8.self).baseAddress, $0.count, &w, &h, &stride) }
        }
        guard rc == 0 else { return nil }
        guard let provider = CGDataProvider(data: data as CFData) else { return nil }
        let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        return CGImage(width: Int(w), height: Int(h), bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: Int(stride),
                       space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: info, provider: provider, decode: nil,
                       shouldInterpolate: true, intent: .defaultIntent)
    }
}

/// A layer the engine draws the Mac's frames into, with the GPU, without the main thread.
private struct GPUScreen: UIViewRepresentable {
    let display: DisplayRouter
    let role: DisplayTargetSelection.Role
    let surface: UUID
    let matchScreenColors: Bool
    let brightness: Double
    private static let srgb = CGColorSpace(name: CGColorSpace.sRGB)

    final class Screen: UIView {
        override class var layerClass: AnyClass { CAMetalLayer.self }

        private let id: UUID
        private let display: DisplayRouter
        private let role: DisplayTargetSelection.Role
        private var attached = false
        private var lastSize: CGSize = .zero

        init(display: DisplayRouter, role: DisplayTargetSelection.Role, surface: UUID) {
            self.id = surface
            self.display = display
            self.role = role
            super.init(frame: .zero)
            backgroundColor = .clear
            isOpaque = false
            layer.contentsGravity = .resizeAspect
            layer.isOpaque = false
        }

        required init?(coder: NSCoder) { fatalError("made in code") }

        override func didMoveToWindow() {
            super.didMoveToWindow()
            if window == nil { detach() } else { attachIfReady() }
        }

        override func layoutSubviews() {
            super.layoutSubviews()
            attachIfReady()
        }

        private func attachIfReady() {
            guard let window, bounds.width > 0, bounds.height > 0,
                  let metal = layer as? CAMetalLayer else { return }
            // A Metal layer starts at 1x: the presenter would fit the Mac to the view's points and
            // Core Animation would stretch that to the screen. At the screen's own scale every pixel shows.
            let scale = window.screen.scale
            let scaleChanged = contentScaleFactor != scale
            if scaleChanged { contentScaleFactor = scale }
            if !attached {
                attached = true
                lastSize = bounds.size
                display.connect(id, layer: metal, role: role)
            } else if lastSize != bounds.size || scaleChanged {
                lastSize = bounds.size
                display.refresh(id)
            }
        }

        func detach() {
            guard attached else { return }
            attached = false
            display.disconnect(id)
        }
    }

    private func setColorSpace(_ view: Screen) {
        let metalLayer = view.layer as! CAMetalLayer
        // The guest's display profile is sRGB, and the presenter writes sRGB
        // values. A nil colorspace skips matching on wide-gamut iPad screens.
        metalLayer.colorspace = matchScreenColors ? Self.srgb : nil
        view.alpha = CGFloat(Self.alpha(brightness))
    }

    /// The screen's brightness setting as the view's opacity over black.
    static func alpha(_ brightness: Double) -> Double { min(max(brightness, 0.4), 1) }

    func makeUIView(context: Context) -> Screen {
        if ProcessInfo.processInfo.environment["HV_COLOR_PROBE"] != nil {
            // What the screen itself is, for the colour probe (the engine logs the layer and the frames).
            let s = UIScreen.main
            let gamut = s.traitCollection.displayGamut == .P3 ? "P3" : s.traitCollection.displayGamut == .SRGB ? "sRGB" : "unspecified"
            HengeLog.log("colorprobe", "colorprobe: v=1 src=swift event=screen gamut=\(gamut) scale=\(s.scale) nativeScale=\(s.nativeScale) "
                       + "bounds=\(Int(s.bounds.width))x\(Int(s.bounds.height)) maxEDR=\(s.potentialEDRHeadroom) currentEDR=\(s.currentEDRHeadroom) "
                       + "brightness=\(String(format: "%.2f", s.brightness)) refreshHz=\(s.maximumFramesPerSecond)")
        }
        let view = Screen(display: display, role: role, surface: surface)
        setColorSpace(view)
        return view
    }
    func updateUIView(_ view: Screen, context: Context) { setColorSpace(view) }
    static func dismantleUIView(_ view: Screen, coordinator: ()) { view.detach() }
}

/// A value worked out once, when a view is made, and kept for as long as it lasts (@StateObject makes it once).
final class Once<Value>: ObservableObject {
    let value: Value
    init(_ value: Value) { self.value = value }
}

/// "Resuming…" over the Mac's screen, as it is put back as it was. The same in the session and on the screen before
/// it (ResumingCover), so going from one to the other changes nothing.
struct ResumingOverlay: View {
    /// What is happening underneath, in smaller type (JIT turning on).
    var detail: String?
    var cancel: (() -> Void)?

    var body: some View {
        ZStack {
            Color.black.opacity(0.6)
            VStack(spacing: 14) {
                ProgressView().tint(.white).controlSize(.large)
                Text("Resuming…").font(.system(size: 22, weight: .semibold)).foregroundStyle(.white)
                if let detail {
                    Text(detail).font(.system(size: 14)).foregroundStyle(.white.opacity(0.7)).multilineTextAlignment(.center)
                        .contentTransition(.opacity)
                }
                if let cancel {
                    Button("Not Now", action: cancel)
                        .font(.system(size: 15, weight: .semibold)).foregroundStyle(Desk.accentLight)
                        .padding(.top, 8)
                }
            }
            .padding(24)
            .animation(.easeInOut(duration: 0.2), value: detail)
        }
    }
}

/// What shows from the tap on Resume until the session does (Henge restarting for it, JIT coming on): the Mac's screen
/// as it was suspended under "Resuming…", as the session then shows it, so a resume never looks like a start.
struct ResumingCover: View {
    let screen: URL
    var detail: String?
    var cancel: (() -> Void)?
    @State private var picture: CGImage?
    @AppStorage("henge.display.brightness") private var screenBrightness = 1.0

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if let picture = picture ?? ResumePicture.cached(screen) {
                Image(decorative: picture, scale: 1).resizable().interpolation(.high).aspectRatio(contentMode: .fit)
                    .opacity(GPUScreen.alpha(screenBrightness))
                    .ignoresSafeArea()
            }
            ResumingOverlay(detail: detail, cancel: cancel).ignoresSafeArea()
        }
        .task { picture = await ResumePicture.load(screen) }
    }
}

/// The screen a suspended Mac had, decoded once for whichever shows it first (the cover before the session, then the
/// session's first frame), and let go once the session has its own frames. About 20 MB, so only the one.
@MainActor
enum ResumePicture {
    private static var kept: (url: URL, srgb: Bool, image: CGImage)?
    private static var loading: (url: URL, task: Task<CGImage?, Never>)?

    /// The session's colour setting, as BootDisplay reads it.
    private static var srgb: Bool { UserDefaults.standard.object(forKey: "henge.matchScreenColors") as? Bool ?? true }

    /// The screen to show while `mac` goes on from its Suspend; nil when it would start afresh.
    static func screen(for mac: VirtualMac) -> URL? {
        guard let url = MacPreview.resumeScreen(for: mac), FileManager.default.fileExists(atPath: url.path) else { return nil }
        return url
    }

    static func cached(_ url: URL) -> CGImage? {
        guard let kept, kept.url == url, kept.srgb == srgb else { return nil }
        return kept.image
    }

    /// Starts reading it, so it is there by the time it is shown.
    static func preload(_ url: URL?) {
        guard let url else { return }
        Task { _ = await load(url) }
    }

    static func load(_ url: URL) async -> CGImage? {
        if let image = cached(url) { return image }
        let srgb = srgb
        let task: Task<CGImage?, Never>
        if let loading, loading.url == url {
            task = loading.task
        } else {
            task = Task.detached(priority: .userInitiated) { BootDisplay.picture(url, srgb: srgb) }
            loading = (url, task)
        }
        let image = await task.value
        if loading?.url == url { loading = nil }
        if let image, kept?.url != url || kept?.srgb != srgb { kept = (url, srgb, image) }
        return image
    }

    static func forget() {
        kept = nil
        loading = nil
    }
}
