import SwiftUI

/// The banner offering to save a Mac's password (when it shows, and what each answer does: PasswordPrompt). "Don't
/// Ask Again" is kept for that Mac; the key button above the keyboard, held, still saves one.
enum PasswordOffer {
    private static func declinedKey(_ mac: UUID) -> String { "henge.macPassword.offerDeclined.\(mac.uuidString)" }

    /// "Don't Ask Again" was chosen for this Mac.
    static func declined(_ mac: UUID) -> Bool { UserDefaults.standard.bool(forKey: declinedKey(mac)) }

    /// "Don't Ask Again": never offered for this Mac again, and no chip until a password is saved with the key button.
    static func neverAsk(_ mac: UUID) {
        UserDefaults.standard.set(true, forKey: declinedKey(mac))
    }
}

struct PasswordOfferView: View {
    let macName: String
    let save: () -> Void
    let notNow: () -> Void
    let neverAsk: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top, spacing: 14) {
                Image(systemName: "key.fill")
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(Desk.accentLight)
                    .padding(.top, 1)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Save \(macName)'s password?")
                        .font(.system(size: 15, weight: .semibold))
                    Text("Henge types it in now, and for you from then on at the login window, the lock screen and password prompts.")
                        .font(.system(size: 13))
                        .foregroundStyle(.white.opacity(0.75))
                        .fixedSize(horizontal: false, vertical: true)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                Button(action: notNow) {
                    Image(systemName: "xmark")
                        .font(.system(size: 13, weight: .bold))
                        .foregroundStyle(.white.opacity(0.7))
                        .frame(width: 30, height: 30)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Not Now")
            }
            HStack(spacing: 18) {
                Spacer(minLength: 0)
                Button("Don't Ask Again", action: neverAsk)
                    .font(.system(size: 15, weight: .medium))
                    .foregroundStyle(.white.opacity(0.7))
                    .buttonStyle(.plain)
                Button("Not Now", action: notNow)
                    .font(.system(size: 15, weight: .medium))
                    .foregroundStyle(.white.opacity(0.85))
                    .buttonStyle(.plain)
                Button(action: save) {
                    Text("Save…")
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 16).padding(.vertical, 8)
                        .background(Desk.accent, in: Capsule())
                }
                .buttonStyle(.plain)
            }
        }
        .foregroundStyle(.white)
        .padding(.horizontal, 18).padding(.vertical, 12)
        .frame(maxWidth: 600)
        .deskGlass(in: RoundedRectangle(cornerRadius: 22, style: .continuous))
        .accessibilityElement(children: .contain)
    }
}
