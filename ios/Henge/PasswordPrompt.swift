import Foundation

/// When Henge offers to save a Mac's password, and when the chip that types it shows. One whole, as Safari's and
/// iPadOS's password prompts are: an offer that waits for an answer, and help only once there is something to help with.
///
/// - At a password field (or the login window) with no password saved for this Mac, the banner offers to save it,
///   once per run of the Mac. It stays until the person answers: nothing takes it away on a timer, and it doesn't go
///   when the field does (a field that loses the focus for a moment would take it before it could be read).
/// - Save: the password is asked for, then typed with Return if a password field or the login window is still in
///   front. Not Now (or the banner's close button): not offered again this run. Don't Ask Again: never offered for
///   this Mac again.
/// - The chip ("Log In", "Enter Password") shows only with a password saved, at a password field or the login
///   window. With none saved there is nothing for it to type, so it doesn't show (it was a nag); the key button above
///   the keyboard, held, saves one at any time.
/// - One prompt at a time: no chip while the banner is up.
struct PasswordPrompt: Equatable {
    /// A password is kept for this Mac.
    var saved: Bool
    /// Don't Ask Again was chosen for this Mac.
    var neverAsk: Bool
    /// Answered this run (Save, Not Now or the close button): not offered again until the Mac runs again.
    private(set) var answered = false
    /// The banner is up.
    private(set) var offering = false

    enum Answer: String { case save = "save", notNow = "not now", neverAsk = "don't ask again" }

    init(saved: Bool, neverAsk: Bool) {
        self.saved = saved
        self.neverAsk = neverAsk
    }

    /// What is in front changed: wanted is a password field or the login window. True when the banner comes up now.
    mutating func update(wanted: Bool) -> Bool {
        if saved { offering = false } // saved some other way (the key button, held): nothing left to offer
        guard wanted, !offering, !saved, !neverAsk, !answered else { return false }
        offering = true
        return true
    }

    /// The person answered the banner.
    mutating func answer(_ a: Answer) {
        offering = false
        answered = true
        if a == .neverAsk { neverAsk = true }
    }

    /// The saved password was forgotten (the password alert's Forget): no chip, and not offered again this run.
    mutating func forgotten() {
        saved = false
        offering = false
        answered = true
    }

    /// Whether the chip shows, given what is in front.
    func chip(wanted: Bool) -> Bool { wanted && saved && !offering }
}
