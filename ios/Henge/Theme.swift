import SwiftUI
import UIKit
import ImageIO

/// The Desk: a dark, quiet page, cards on it with a hairline edge, and one blue for what you press.
/// A Mac's screen is the only colourful thing here.
enum Desk {
    static let page = Color(red: 11 / 255, green: 12 / 255, blue: 16 / 255)
    static let card = Color(red: 22 / 255, green: 24 / 255, blue: 31 / 255)
    static let raised = Color(red: 35 / 255, green: 38 / 255, blue: 47 / 255)
    static let text = Color(red: 242 / 255, green: 243 / 255, blue: 247 / 255)
    static let dim = Color(red: 144 / 255, green: 149 / 255, blue: 164 / 255)
    static let faint = Color(red: 106 / 255, green: 111 / 255, blue: 126 / 255)
    static let accent = Color(red: 46 / 255, green: 107 / 255, blue: 1)
    static let accentLight = Color(red: 91 / 255, green: 141 / 255, blue: 1)
    static let good = Color(red: 57 / 255, green: 217 / 255, blue: 138 / 255)
    static let warn = Color(red: 1, green: 188 / 255, blue: 46 / 255)
    static let bad = Color(red: 1, green: 122 / 255, blue: 112 / 255)
    static let hairline = Color.white.opacity(0.1)
}

extension View {
    /// Liquid Glass where iOS has it, for controls floating over a Mac's screen.
    @ViewBuilder func deskGlass<S: Shape>(in shape: S) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            glassEffect(.regular, in: shape)
        } else {
            background(.ultraThinMaterial, in: shape)
        }
        #else
        background(.ultraThinMaterial, in: shape)
        #endif
    }

    /// Keeps a page's content a readable width on a big screen.
    func deskColumn() -> some View {
        frame(maxWidth: 680).frame(maxWidth: .infinity)
    }

    /// A card on the page.
    func deskCard(radius: CGFloat = 22) -> some View {
        background(Desk.card, in: RoundedRectangle(cornerRadius: radius, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: radius, style: .continuous).stroke(Desk.hairline, lineWidth: 0.5))
    }
}

/// The one action a screen is for.
struct DeskButtonStyle: ButtonStyle {
    var enabled = true
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 19, weight: .bold))
            .foregroundStyle(enabled ? Color.white : Desk.dim)
            .frame(maxWidth: .infinity)
            .frame(height: 58)
            .background(enabled ? Desk.accent : Desk.raised, in: Capsule())
            .opacity(configuration.isPressed ? 0.85 : 1)
            .scaleEffect(configuration.isPressed ? 0.985 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}

/// Labelled the way the page's other headings are.
struct DeskHeading: View {
    let title: String
    init(_ title: String) { self.title = title }
    var body: some View {
        Text(title)
            .font(.system(size: 14, weight: .semibold))
            .foregroundStyle(Desk.dim)
            .padding(.horizontal, 18)
            .padding(.top, 18)
            .padding(.bottom, 8)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

/// A row of a card.
struct DeskBar: View {
    let fraction: Double
    var height: CGFloat = 8
    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(Desk.raised)
                Capsule().fill(Desk.accent).frame(width: max(height, geo.size.width * min(max(fraction, 0), 1)))
            }
        }
        .frame(height: height)
    }
}

/// The picture of a Mac as it was last seen, kept beside its disk.
enum MacPreview {
    static func file(for mac: VirtualMac) -> URL { mac.directory.appendingPathComponent("preview.jpg") }

    static func capture(width: Int, height: Int) -> CGImage? {
        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        guard henge_vm_snapshot(&pixels, Int32(width), Int32(height)) == 0 else { return nil }
        guard let provider = CGDataProvider(data: Data(pixels) as CFData) else { return nil }
        let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
                       space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: info, provider: provider, decode: nil,
                       shouldInterpolate: true, intent: .defaultIntent)
    }

    /// Off the main thread.
    static func save(for mac: VirtualMac) {
        let url = file(for: mac), height = Int(640 / mac.aspect)
        Task.detached(priority: .utility) { write(to: url, height: height) }
    }

    /// Before the machine stops, while there is still a screen to read. Paused, the whole screen too, for a resume.
    static func saveNow(for mac: VirtualMac) {
        write(to: file(for: mac), height: Int(640 / mac.aspect))
        if henge_vm_paused() != 0 { keepScreen(for: mac) }
    }

    /// The Mac's whole screen as it was paused, pixel for pixel: what a Mac going on from a suspend shows
    /// until macOS draws again (BootDisplay), as a Mac waking from sleep shows what it had.
    static func screenFile(for mac: VirtualMac) -> URL { mac.directory.appendingPathComponent("screen.png") }

    /// The screen to show while this Mac goes on from where it was suspended; nil when it starts afresh.
    static func resumeScreen(for mac: VirtualMac) -> URL? {
        MacSuspend.hasSaved(for: mac.id) ? screenFile(for: mac) : nil
    }

    private static func keepScreen(for mac: VirtualMac) {
        let url = screenFile(for: mac)
        var w32: Int32 = 0, h32: Int32 = 0
        guard henge_vm_picture_size(&w32, &h32) == 0, w32 > 0, h32 > 0, let image = capture(width: Int(w32), height: Int(h32)) else {
            try? FileManager.default.removeItem(at: url) // an older one would be wrong
            return
        }
        Task.detached(priority: .utility) { // the pixels are read; only the encoding is left
            guard let dest = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil) else { return }
            CGImageDestinationAddImage(dest, image, nil)
            if !CGImageDestinationFinalize(dest) { try? FileManager.default.removeItem(at: url) }
        }
    }

    private static func write(to url: URL, height: Int) {
        guard let image = capture(width: 640, height: height), let data = UIImage(cgImage: image).jpegData(compressionQuality: 0.8) else { return }
        try? data.write(to: url, options: .atomic)
    }
}

enum ByteText {
    static func string(_ bytes: Int64) -> String {
        let formatter = ByteCountFormatter()
        formatter.countStyle = .file
        formatter.allowedUnits = [.useGB, .useMB]
        formatter.includesUnit = true
        return formatter.string(fromByteCount: bytes)
    }

    static func string(_ bytes: UInt64) -> String {
        string(Int64(clamping: bytes))
    }
}

/// The names the JIT setup screens (from Husk) use, in Henge's colours.
enum Theme {
    static let accent = Desk.accent
    static let accentSoft = Desk.accent.opacity(0.18)
    static let backdrop = Desk.page
    static let text = Desk.text
    static let textDim = Desk.dim
}

extension View {
    /// high: a card raised above the others (a result, an outcome).
    func hengeCard(high: Bool = false) -> some View {
        background(high ? Desk.raised : Desk.card, in: RoundedRectangle(cornerRadius: 22, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 22, style: .continuous).stroke(Desk.hairline, lineWidth: 1))
    }
}

/// The one action a screen is for (the JIT setup screens use it).
struct PrimaryButtonStyle: ButtonStyle {
    var enabled = true

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(.system(size: 16, weight: .semibold))
            .foregroundStyle(enabled ? Color.white : Desk.dim)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 15)
            .background(enabled ? Desk.accent : Desk.raised, in: RoundedRectangle(cornerRadius: 15, style: .continuous))
            .opacity(configuration.isPressed ? 0.85 : 1)
            .scaleEffect(configuration.isPressed ? 0.985 : 1)
            .animation(.easeOut(duration: 0.12), value: configuration.isPressed)
    }
}

/// A card that is also a button: it moves a little under the finger.
struct CardButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.96 : 1)
            .opacity(configuration.isPressed ? 0.9 : 1)
            .animation(.easeOut(duration: 0.14), value: configuration.isPressed)
    }
}
