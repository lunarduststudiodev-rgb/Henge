import SwiftUI
import UIKit
import CoreGraphics
import ImageIO

/// The Apple logo macOS draws when its GPU takes over, drawn before then at the same size in the same
/// place, so that macOS's own logo lands exactly on it and only its progress bar appears, as on a Mac.
///
/// The look is what macOS drew the last time it started in this mode, saved from its first GPU frame
/// (`learn`). Before it has ever started in a mode: Apple's symbol where macOS puts its logo, centred
/// and 76 x 92 points (measured on macOS 26: 152 x 184 pixels on a 2752 x 2064 Retina mode).
/// Either way the screen fits the picture to the view the way GPUScreen does: aspect fit, centred.
struct BootLogo: View {
    let look: Look

    var body: some View {
        GeometryReader { geo in
            let picture = look.picture, logo = look.logo
            let fit = min(geo.size.width / picture.width, geo.size.height / picture.height)
            let left = (geo.size.width - picture.width * fit) / 2, top = (geo.size.height - picture.height * fit) / 2
            Image(decorative: look.art, scale: 1)
                .resizable()
                .interpolation(.high)
                .aspectRatio(contentMode: .fit)
                .frame(width: logo.width * fit, height: logo.height * fit)
                .position(x: left + logo.midX * fit, y: top + logo.midY * fit)
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    /// The mode macOS starts in, which decides where its logo is.
    struct Mode: Equatable {
        let width: Int, height: Int, scale: Int

        static func current() -> Mode? {
            var w: UInt32 = 0, h: UInt32 = 0, k: UInt32 = 0
            guard henge_vm_start_mode(&w, &h, &k) == 0, w > 0, h > 0 else { return nil }
            return Mode(width: Int(w), height: Int(h), scale: max(Int(k), 1))
        }

        fileprivate var name: String { "\(width)x\(height)@\(scale)" }
    }

    /// The picture macOS shows while it starts, in its own pixels, the logo's box in it, and the logo itself.
    struct Look {
        let mode: Mode
        let picture: CGSize
        let logo: CGRect
        let art: CGImage

        /// What macOS drew the last time it started in this mode, else where it is known to draw its logo.
        static func load(_ mode: Mode) -> Look? { saved(mode) ?? predicted(mode) }

        private static func predicted(_ mode: Mode) -> Look? {
            guard let art = symbol else { return nil }
            let k = CGFloat(mode.scale)
            let size = CGSize(width: 76 * k, height: 92 * k)
            // The logo's box sits 1.5 points left of the centre and 0.5 below: its bite and leaf.
            let centre = CGPoint(x: CGFloat(mode.width) / 2 - 1.5 * k, y: CGFloat(mode.height) / 2 + 0.5 * k)
            return Look(mode: mode, picture: CGSize(width: mode.width, height: mode.height),
                        logo: CGRect(x: centre.x - size.width / 2, y: centre.y - size.height / 2, width: size.width, height: size.height),
                        art: art)
        }

        /// Apple's logo symbol in white, cropped to its ink so its box is the logo's.
        private static let symbol: CGImage? = {
            let config = UIImage.SymbolConfiguration(pointSize: 400)
            guard let glyph = UIImage(systemName: "apple.logo", withConfiguration: config)?
                .withTintColor(.white, renderingMode: .alwaysOriginal) else { return nil }
            let format = UIGraphicsImageRendererFormat()
            format.scale = 1
            format.opaque = false
            guard let full = UIGraphicsImageRenderer(size: glyph.size, format: format).image(actions: { _ in glyph.draw(at: .zero) }).cgImage
            else { return nil }
            let w = full.width, h = full.height
            var alpha = [UInt8](repeating: 0, count: w * h)
            guard let ctx = CGContext(data: &alpha, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w,
                                      space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.alphaOnly.rawValue) else { return nil }
            ctx.draw(full, in: CGRect(x: 0, y: 0, width: w, height: h))
            var x0 = w, x1 = -1, y0 = h, y1 = -1
            for y in 0..<h {
                for x in 0..<w where alpha[y * w + x] > 127 {
                    x0 = min(x0, x); x1 = max(x1, x); y0 = min(y0, y); y1 = max(y1, y)
                }
            }
            guard x1 >= x0, y1 >= y0 else { return nil }
            return full.cropping(to: CGRect(x: x0, y: y0, width: x1 - x0 + 1, height: y1 - y0 + 1))
        }()

        // MARK: What macOS drew, kept in Application Support/BootLogo/<mode>.png and .plist

        private static var folder: URL {
            FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("BootLogo", isDirectory: true)
        }

        private static func saved(_ mode: Mode) -> Look? {
            let base = folder.appendingPathComponent(mode.name)
            guard let data = try? Data(contentsOf: base.appendingPathExtension("plist")),
                  let v = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Double],
                  let pw = v["pictureWidth"], let ph = v["pictureHeight"], let x = v["x"], let y = v["y"], let w = v["width"], let h = v["height"],
                  pw > 0, ph > 0, w > 0, h > 0,
                  let source = CGImageSourceCreateWithURL(base.appendingPathExtension("png") as CFURL, nil),
                  let art = CGImageSourceCreateImageAtIndex(source, 0, nil) else { return nil }
            return Look(mode: mode, picture: CGSize(width: pw, height: ph), logo: CGRect(x: x, y: y, width: w, height: h), art: art)
        }

        fileprivate func save() {
            let base = Self.folder.appendingPathComponent(mode.name)
            try? FileManager.default.createDirectory(at: Self.folder, withIntermediateDirectories: true)
            guard let dest = CGImageDestinationCreateWithURL(base.appendingPathExtension("png") as CFURL, "public.png" as CFString, 1, nil) else { return }
            CGImageDestinationAddImage(dest, art, nil)
            guard CGImageDestinationFinalize(dest) else { return }
            let v: [String: Double] = ["pictureWidth": picture.width, "pictureHeight": picture.height,
                                       "x": logo.minX, "y": logo.minY, "width": logo.width, "height": logo.height]
            if let data = try? PropertyListSerialization.data(fromPropertyList: v, format: .binary, options: 0) {
                try? data.write(to: base.appendingPathExtension("plist"), options: .atomic)
            }
        }
    }

    // MARK: Learning the look from macOS's first frames

    private static var learning = false

    /// Looks for macOS's boot logo in its first GPU frames and keeps it for the next start in this mode.
    static func learn(_ mode: Mode) async {
        guard !learning else { return }
        learning = true
        defer { learning = false }
        for _ in 0..<12 { // its first 3 s: the logo shows for several before the login window
            if let look = await Task.detached(priority: .utility, operation: { find(mode) }).value {
                look.save()
                HengeLog.log("display", "boot logo \(Int(look.logo.width))x\(Int(look.logo.height)) at \(Int(look.logo.minX)),\(Int(look.logo.minY)) "
                             + "in \(Int(look.picture.width))x\(Int(look.picture.height)), kept for \(mode.name)")
                return
            }
            try? await Task.sleep(nanoseconds: 250_000_000)
        }
        HengeLog.log("display", "boot logo: not found in macOS's first frames")
    }

    /// The logo in what macOS shows now: on a black screen, the lit shape across its middle row.
    private static func find(_ mode: Mode) -> Look? {
        var w32: Int32 = 0, h32: Int32 = 0
        guard henge_vm_picture_size(&w32, &h32) == 0 else { return nil }
        let w = Int(w32), h = Int(h32)
        guard w >= 320, h >= 200, w <= 8192, h <= 8192 else { return nil }
        var px = [UInt8](repeating: 0, count: w * h * 4)
        guard henge_vm_snapshot(&px, w32, h32) == 0 else { return nil }

        // Rows with anything lit, in runs; and how much is lit at all (a boot screen is nearly all black).
        var lit = 0
        var runs: [(top: Int, bottom: Int, left: Int, right: Int)] = []
        var lastRow = -10
        px.withUnsafeBufferPointer { p in
            for y in 0..<h {
                var left = Int.max, right = -1
                let row = y * w * 4
                for x in 0..<w {
                    let i = row + x * 4
                    if Int(p[i]) + Int(p[i + 1]) + Int(p[i + 2]) > 96 {
                        lit += 1
                        if left == Int.max { left = x }
                        right = x
                    }
                }
                guard right >= 0 else { continue }
                if y - lastRow > 3 || runs.isEmpty {
                    runs.append((y, y, left, right))
                } else {
                    runs[runs.count - 1].bottom = y
                    runs[runs.count - 1].left = min(runs[runs.count - 1].left, left)
                    runs[runs.count - 1].right = max(runs[runs.count - 1].right, right)
                }
                lastRow = y
            }
        }
        guard lit < w * h / 50 else { return nil } // the login window or a desktop, not the boot screen
        guard let logo = runs.first(where: { $0.top <= h / 2 && $0.bottom >= h / 2 }) else { return nil }
        let lw = logo.right - logo.left + 1, lh = logo.bottom - logo.top + 1
        let aspect = Double(lw) / Double(lh)
        guard lh >= h / 50, lh <= h / 3, aspect > 0.6, aspect < 1.1, logo.left <= w / 2, logo.right >= w / 2 else { return nil }

        // Two black pixels round it, so its soft edge is whole.
        let box = CGRect(x: logo.left - 2, y: logo.top - 2, width: lw + 4, height: lh + 4)
            .intersection(CGRect(x: 0, y: 0, width: w, height: h))
        guard let provider = CGDataProvider(data: Data(px) as CFData),
              let frame = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
                                  provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent),
              let art = frame.cropping(to: box) else { return nil }
        return Look(mode: mode, picture: CGSize(width: w, height: h), logo: box, art: art)
    }
}
