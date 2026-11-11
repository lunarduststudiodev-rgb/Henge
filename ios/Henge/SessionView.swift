import SwiftUI
import GameController

/// A running Mac. Upright, its screen sits above a trackpad; on its side the screen fills the device
/// and is its own trackpad. Zoomed in, the screen fills the device either way; a finger works as it does unzoomed (a
/// touch, the pointer or a trackpad), the map in the corner moves the view, and the view follows a pointer a mouse or
/// trackpad moves.
/// A keyboard, trackpad or mouse attached to an iPad works throughout.
struct SessionView: View {
    let mac: VirtualMac
    let close: () -> Void
    /// The screen it had when suspended, if it goes on from there: read from storage once, as the session opens.
    @StateObject private var resumeScreen: Once<URL?>

    init(mac: VirtualMac, close: @escaping () -> Void) {
        self.mac = mac
        self.close = close
        _resumeScreen = StateObject(wrappedValue: Once(MacPreview.resumeScreen(for: mac))) // evaluated once per session
    }

    @StateObject private var display = DisplayRouter()
    @ObservedObject private var pause = MacPause.shared
    @State private var error: String?
    @State private var state: Int32 = 0
    /// Once stopped: who stopped it (henge_vm_halted_by), for the stopped card.
    @State private var haltedBy: Int32 = 0
    @ObservedObject private var relaunch = Relaunch.shared
    @State private var cpusOn: Int32 = 0
    @State private var userCode = false
    @State private var blocksPerSecond: Double = 0
    @State private var framesOut: UInt64 = 0
    @State private var framesIn: UInt64 = 0
    @State private var macPorts = MacPorts()
    @State private var log: [String] = []
    @State private var started = Date()
    @State private var thermalState = ProcessInfo.processInfo.thermalState
    @AppStorage("henge.cpus") private var cpus = 4
    @AppStorage("henge.matchScreenColors") private var matchScreenColors = true
    @AppStorage("henge.display.brightness") private var screenBrightness = 1.0
    @AppStorage("henge.cursor.scale") private var cursorScale = Device.isPad ? 1 : 3
    @AppStorage("henge.cursor.sharp") private var sharpCursor = true
    @AppStorage(ScrollSpeed.key) private var scrollSpeed = ScrollSpeed.standard
    /// On an iPad the Mac's screen is big enough to touch accurately; on a phone, fingers start as a trackpad.
    @AppStorage("henge.touch.direct") private var directTouch = UIDevice.current.userInterfaceIdiom == .pad
    @AppStorage(TouchScreen.enabledKey) private var realTouchscreen = true
    @AppStorage(TouchScreen.alwaysKey) private var alwaysNative = false
    @State private var touchscreenStatus = TouchScreen.Status.none
    @State private var touchNotice: String?
    @State private var graphicsRefused = false // this iPadOS's Metal refused macOS's GPU code (GraphicsSupport.swift)
    @State private var graphicsNoteClosed = false
    @State private var gpuState: Int32 = 0 // the Mac's graphics after a GPU failure: henge_vm_gpu_state (GraphicsStoppedNote)
    @State private var gpuNoteClosed = false

    /// The on-screen keyboard: KeyboardOwner shows and hides it; here it only makes room for it.
    @ObservedObject private var keyboard = KeyboardOwner.shared
    private var typing: Bool { keyboard.isShown }
    /// How much of the window's bottom the keyboard covers, in points: all of a docked one, or only the
    /// row of the Mac's keys that stays along the bottom while an iPad's keyboard floats.
    @State private var keyboardCover: CGFloat = 0
    @State private var askPassword = false
    @State private var passwordDraft = ""
    /// The banner offering to save the Mac's password and the chip that types it (PasswordPrompt): set as the session appears.
    @State private var passwordPrompt = PasswordPrompt(saved: false, neverAsk: true)
    /// What has the Mac's keyboard focus, as Henge Tools reports it: -1 unknown (no helper), 0 nothing to type
    /// into, 1 a text field, 2 a password field.
    private var macFocus: Int32 { keyboard.focus }
    /// Where the focused field is, as fractions of the Mac's screen, when the helper says.
    private var fieldFrame: CGRect? { keyboard.fieldFrame }
    @State private var pointerHiddenAtStart = false
    /// At the Mac's login window: as Henge Tools' system part says, when it is installed (loginCertain). Otherwise as
    /// far as can be told: macOS shows its pointer (the boot screens have none), and the helper, which runs once
    /// someone logs in and has before on this Mac, isn't linked. Not for a while after
    /// Return is pressed, which is someone logging in, until the helper is up on the desktop. Only the system part's
    /// word has the password's Return pressed with no helper to ask (KeyboardOwner.atLoginCertainly): a guess may be
    /// a logged-in Mac whose helper went.
    @State private var atLogin = false
    /// atLogin is what Henge Tools' system part says, not a guess: it runs from the Mac's start (it is installed with
    /// the Mac's password, once) and tells the login window from someone's desktop.
    @State private var loginCertain = false
    @State private var macPointerSeen = false
    @AppStorage("henge.keyboard.auto") private var autoKeyboard = true
    /// A hardware keyboard is connected (iPadOS then hides its own, and text fields bring up nothing): as GameController
    /// says, which knows of one from its first key press until it goes.
    @State private var hardwareKeyboard = HardwareKeyboard.shared.connected
    /// Said once: the first time a hardware keyboard pauses Show for Text Fields.
    @AppStorage("henge.keyboard.pausedToldOnce") private var keyboardPausedTold = false
    @AppStorage("henge.keyboard.autocorrect") private var autocorrect = true
    @ObservedObject private var rowPlacement = RowPlacement.shared
    @State private var zoomed = false
    @State private var showDetails = false
    @State private var showDisks = false
    @State private var showDownloads = false
    @ObservedObject private var downloadWatch = DownloadWatch.shared
    @ObservedObject private var power = MacPower.shared
    @ObservedObject private var dictation = Dictation.shared
    /// Hidden controls stay hidden the next time the Mac opens.
    @AppStorage("henge.controls.hidden") private var controlsHidden = false
    /// While typing the controls tuck away, until shown while typing (TypingBars). Remembered too.
    @AppStorage("henge.controls.hiddenTyping") private var controlsHiddenTyping = true
    /// Said once: the first time the keyboard tucks the controls away, how to bring them back.
    @AppStorage("henge.controls.typingHintShown") private var typingHintShown = false
    /// The reveal hint is for controls the keyboard tucked away, not ones the person hid.
    @State private var revealHintTyping = false
    /// Off: hidden controls leave no handle, and only three fingers on the screen bring them back.
    @AppStorage("henge.controls.handle") private var controlsHandleShown = true
    @State private var showRevealHint = false
    /// Why no on-screen keyboard came up: a hardware keyboard is connected (perhaps a Mac's, by Universal Control).
    @State private var keyboardNotice: String?
    @State private var pointerRefused = false
    @AppStorage("henge.controls.hintsShown") private var revealHintsShown = 0
    /// Once per install: macOS's moving wallpapers and aerial screen saver are video, decoded by the emulated
    /// CPUs (a phone ran about 30% faster with a still one).
    @AppStorage("henge.tips.stillWallpaperShown") private var stillWallpaperTipShown = false
    @State private var showWallpaperTip = false
    @State private var confirmStop = false
    @State private var zoomCentre = CGPoint(x: 0.5, y: 0.5) // where the zoomed view is, 0...1 of the Mac's screen
    @State private var map: UIImage?
    @State private var mapDragging = false  // a finger moves the zoomed view on the map
    @State private var mapLeft = false      // the map's side while it does, so it doesn't jump from under the finger
    @GestureState private var mapTouched = false // the map's drag, which SwiftUI ends even when it's cancelled
    @State private var zoomFollowing: Task<Void, Never>?
    @State private var helperRunning = false
    @State private var touchHelper = 0 // 0 not linked, 1 linked but not allowed, 2 working
    @State private var shown = CGSize(width: 0.5, height: 0.5) // how much of the Mac's screen the zoomed view shows
    @Environment(\.scenePhase) private var scenePhase


    var body: some View {
        GeometryReader { geo in
            let insets = geo.safeAreaInsets
            let full = CGSize(width: geo.size.width + insets.leading + insets.trailing,
                              height: geo.size.height + insets.top + insets.bottom)
            let landscape = full.width > full.height
            let layout = Layout(full: full, insets: insets, landscape: landscape, zoomed: zoomed, keyboard: macKeyboardCover(fills: landscape || zoomed), centre: zoomCentre, aspect: mac.aspect)
            ZStack(alignment: .topLeading) {
                (landscape || zoomed ? Color.black : Desk.page)
                // One view in every layout: it holds the layer the engine draws into.
                BootDisplay(display: display, role: .device,
                            resume: pause.resumeGivenUp == mac.id ? nil : resumeScreen.value,
                            waiting: pause.resumeQuestion != nil || error != nil)
                    .opacity(display.isExternal ? 0 : 1)
                    .overlay {
                        if display.isExternal {
                            VStack(spacing: 8) {
                                Image(systemName: "display").font(.largeTitle)
                                Text("Mac on external display").font(.headline)
                                Text("Use the trackpad, keyboard or a connected mouse.")
                                    .font(.subheadline)
                            }
                            .foregroundStyle(.white)
                            .multilineTextAlignment(.center)
                            .padding()
                            .accessibilityElement(children: .combine)
                        }
                    }
                    .frame(width: layout.screen.width, height: layout.screen.height)
                    .clipShape(RoundedRectangle(cornerRadius: layout.fillsPhone ? 0 : 15, style: .continuous))
                    .overlay {
                        if !layout.fillsPhone {
                            RoundedRectangle(cornerRadius: 15, style: .continuous).stroke(Color.white.opacity(0.16), lineWidth: 0.5)
                        }
                    }
                    .position(x: layout.screen.midX, y: layout.screen.midY)
                // Over the Mac's screen a finger is macOS's own touch where it has the touchscreen, otherwise the pointer;
                // zoomed in too, where the rest of the screen is past the view's edges and the map in the corner moves
                // the view.
                Trackpad(direct: directTouch && !display.isExternal,
                         zoomed: zoomed,
                         screen: layout.screen.offsetBy(dx: -layout.touch.minX, dy: -layout.touch.minY),
                         mac: CGSize(width: mac.screen.width, height: mac.screen.height),
                         retina: mac.isRetina)
                    .frame(width: layout.touch.width, height: layout.touch.height)
                    .position(x: layout.touch.midX, y: layout.touch.midY)
                // Under the controls, which keep Resume and Shut down where they always are.
                if state == 1 && pause.isHeld {
                    pausedScreen(layout)
                }
                if !layout.fillsPhone {
                    uprightControls(layout)
                } else {
                    floatingControls(layout)
                }
                if state == 1 && !pause.isHeld {
                    ToolsGuide(mac: mac, screen: layout.screen)
                        .frame(width: full.width, height: full.height)
                }
                if pointerRefused && !showRevealHint {
                    Label("Make Henge full screen to use the trackpad or mouse as the Mac's", systemImage: "cursorarrow.rays")
                        .font(.system(size: 15, weight: .medium))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 18).padding(.vertical, 12)
                        .deskGlass(in: Capsule())
                        .position(x: full.width / 2, y: insets.top + 40)
                        .transition(.opacity)
                        .allowsHitTesting(false)
                }
                if showRevealHint {
                    Label(revealHintText, systemImage: "hand.tap")
                        .font(.system(size: 15, weight: .medium))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 18).padding(.vertical, 12)
                        .deskGlass(in: Capsule())
                        .position(x: full.width / 2, y: insets.top + 64) // under the handle
                        .transition(.opacity)
                        .allowsHitTesting(false)
                }
                if let touchNotice, !showRevealHint, keyboardNotice == nil {
                    Label(touchNotice, systemImage: "hand.point.up.left")
                        .font(.system(size: 15, weight: .medium))
                        .foregroundStyle(.white)
                        .multilineTextAlignment(.leading)
                        .frame(maxWidth: 560)
                        .padding(.horizontal, 18).padding(.vertical, 12)
                        .deskGlass(in: RoundedRectangle(cornerRadius: 22, style: .continuous))
                        .position(x: full.width / 2, y: insets.top + 64)
                        .transition(.opacity)
                        .allowsHitTesting(false)
                }
                if let keyboardNotice, !showRevealHint {
                    Label(keyboardNotice, systemImage: "keyboard")
                        .font(.system(size: 15, weight: .medium))
                        .foregroundStyle(.white)
                        .multilineTextAlignment(.leading)
                        .frame(maxWidth: 560)
                        .padding(.horizontal, 18).padding(.vertical, 12)
                        .deskGlass(in: RoundedRectangle(cornerRadius: 22, style: .continuous))
                        .position(x: full.width / 2, y: insets.top + 64)
                        .transition(.opacity)
                        .allowsHitTesting(false)
                }
                if passwordPrompt.offering && !showRevealHint {
                    PasswordOfferView(macName: mac.name, save: {
                        answerPasswordOffer(.save)
                        // Asked for, then typed if a password field or the login window is still in front; Return only
                        // where the helper (asked once it is typed) or the system part says one is there.
                        MacPassword.typeOrAsk(enter: true)
                    }, notNow: {
                        answerPasswordOffer(.notNow)
                    }, neverAsk: {
                        answerPasswordOffer(.neverAsk)
                        PasswordOffer.neverAsk(mac.id)
                    })
                    .padding(.horizontal, 16)
                    .position(x: full.width / 2, y: insets.top + (keyboardNotice != nil || touchNotice != nil ? 150 : 72))
                    .transition(.opacity)
                }
                if let problem = pause.problem {
                    Label(problem, systemImage: "exclamationmark.triangle.fill")
                        .font(.system(size: 15, weight: .medium))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 18).padding(.vertical, 12)
                        .deskGlass(in: Capsule())
                        .position(x: full.width / 2, y: insets.top + 40)
                        .transition(.opacity)
                        .allowsHitTesting(false)
                }
                if power.active || power.notice != nil {
                    PowerBanner(power: power, force: forceShutDown)
                        .padding(.horizontal, 16)
                        .position(x: full.width / 2, y: noticeY(layout))
                }
                if let notice = downloadWatch.notice {
                    DownloadNoticeView(notice: notice)
                        .padding(.horizontal, 16)
                        .position(x: full.width / 2, y: noticeY(layout) + (power.active || power.notice != nil ? 100 : 0))
                }
                if showPasswordChip { passwordChip(layout, full: full) }
                if showWallpaperTip && !showRevealHint && !pointerRefused && touchNotice == nil && keyboardNotice == nil && pause.problem == nil {
                    Label("For more speed, give this Mac a still wallpaper and set its screen saver to Never in System Settings.", systemImage: "speedometer")
                        .font(.system(size: 15, weight: .medium))
                        .foregroundStyle(.white)
                        .multilineTextAlignment(.leading)
                        .frame(maxWidth: min(full.width - 40, 520))
                        .padding(.horizontal, 18).padding(.vertical, 12)
                        .deskGlass(in: RoundedRectangle(cornerRadius: 22, style: .continuous))
                        .position(x: full.width / 2, y: insets.top + 56)
                        .transition(.opacity)
                        .allowsHitTesting(false)
                }
                if dictation.isOn {
                    // Its bottom right corner on the row of Mac keys' right end, by the mic key.
                    let corner = dictationCorner(layout, full: full)
                    DictationPill(width: min(380, full.width - 24))
                        .frame(width: corner.x, height: corner.y, alignment: .bottomTrailing)
                        .position(x: corner.x / 2, y: corner.y / 2)
                        .transition(.opacity)
                }
                if gpuState != 0 && !gpuNoteClosed && state == 1 && !pause.isHeld {
                    GraphicsStoppedNote(state: gpuState, tryAgain: {
                        if henge_vm_gpu_try_again() == 0 {
                            HengeLog.log("session", "Try Again: the Mac's graphics run again")
                            gpuState = 0
                        } else {
                            gpuState = henge_vm_gpu_state()
                        }
                    }, close: { gpuNoteClosed = true })
                        .padding(.horizontal, 16)
                        .position(x: layout.screen.midX, y: layout.screen.midY)
                        .transition(.opacity)
                } else if graphicsRefused && !graphicsNoteClosed && state == 1 && !pause.isHeld {
                    GraphicsSupportSessionNote { graphicsNoteClosed = true }
                        .padding(.horizontal, 16)
                        .position(x: layout.screen.midX, y: layout.screen.midY)
                        .transition(.opacity)
                }
                if let error { errorCard(error, layout) }
                if state == 2 && error == nil { stoppedCard(layout, full: full) }
                if let question = pause.resumeQuestion {
                    ResumeQuestionCard(question: question)
                        .frame(width: min(layout.touch.width, 390) - 32, alignment: .leading)
                        .position(x: layout.fillsPhone ? layout.touch.midX : layout.pad.midX, y: layout.fillsPhone ? layout.touch.midY : layout.pad.midY)
                }
                KeyboardCatcher(cover: $keyboardCover, autocorrect: autocorrect).frame(width: 0, height: 0)
                if typing && rowPlacement.standalone && rowPlacement.compact, let spot = rowPlacement.glassFrame(in: full) {
                    // A hardware keyboard: only what it has no key for, small, in the corner clear of the Mac's Dock.
                    // Its right end at the spot's: it grows to the left while a modifier is held from its menu. The
                    // rest of the frame is empty, and touches there are the Mac's.
                    CompactMacKeys(hide: { KeyboardOwner.shared.userHid("the small control's Hide") })
                        .frame(width: spot.maxX, height: spot.height, alignment: .trailing)
                        .position(x: spot.maxX / 2, y: spot.midY)
                        .transition(.opacity)
                } else if typing && rowPlacement.standalone, let glass = rowPlacement.glassFrame(in: full) {
                    // No keys on the screen (dictating with the letters away): the row is Henge's own glass bar.
                    ModifierRow(hide: { KeyboardOwner.shared.userHid("the row's hide button") })
                        .frame(width: full.width, height: glass.height)
                        .position(x: full.width / 2, y: glass.midY)
                        .transition(.opacity)
                }
            }
            .frame(width: full.width, height: full.height)
            .ignoresSafeArea()
            .animation(.easeInOut(duration: 0.25), value: zoomed)
            .animation(.keyboard, value: typing) // the Mac grows back as the keyboard goes
            .animation(.easeInOut(duration: 0.25), value: controlsHidden)
            .animation(.easeInOut(duration: 0.25), value: controlsHiddenTyping)
            .animation(.keyboard, value: rowPlacement.foldedOverDocked) // the Mac grows behind the folded row
            .animation(.easeInOut(duration: 0.2), value: rowPlacement.compact) // the small control or the whole row
            .animation(.keyboard, value: rowPlacement.systemBar) // above iPadOS's bar as it comes, goes or collapses
            .animation(.easeInOut(duration: 0.25), value: showRevealHint)
            .animation(.easeInOut(duration: 0.2), value: pause.phase)
            .animation(.easeInOut(duration: 0.2), value: pause.suspending)
            .animation(.easeInOut(duration: 0.2), value: pause.suspended)
            .animation(.easeInOut(duration: 0.25), value: pause.problem)
            .animation(.easeInOut(duration: 0.2), value: showPasswordChip)
            .animation(.easeInOut(duration: 0.25), value: downloadWatch.notice)
            .animation(.easeInOut(duration: 0.25), value: power.phase)
            .animation(.easeInOut(duration: 0.25), value: power.notice)
            .animation(.easeInOut(duration: 0.25), value: downloadToolShown)
            .animation(.easeInOut(duration: 0.25), value: showWallpaperTip)
            .onReceive(NotificationCenter.default.publisher(for: UIPointerLockState.didChangeNotification)) { _ in
                pointerRefused = PointerLock.refused
            }
            .onReceive(NotificationCenter.default.publisher(for: .GCMouseDidConnect)) { _ in
                DispatchQueue.main.asyncAfter(deadline: .now() + 1) { pointerRefused = PointerLock.refused }
            }
            .onReceive(NotificationCenter.default.publisher(for: HardwareKeyboard.changed)) { _ in hardwareKeyboardChanged() }
            .onReceive(NotificationCenter.default.publisher(for: Trackpad.threeFingerTap)) { _ in showControls() }
            .onAppear {
                shown = layout.shown
                // Opened with the controls hidden and no handle: say once more how to get them back.
                if controlsHidden && !controlsHandleShown { remindReveal() }
            }
            .onChange(of: layout.shown) { shown = $0 }
            .onChange(of: typing) { on in
                if on { explainMissingKeyboard() } else { keyboardNotice = nil }
                if on { tellTypingTuck(layout) } else if revealHintTyping { showRevealHint = false }
            }
            .animation(.easeInOut(duration: 0.25), value: keyboardNotice)
            .animation(.easeInOut(duration: 0.25), value: touchNotice)
            .animation(.easeInOut(duration: 0.25), value: graphicsRefused)
            .animation(.easeInOut(duration: 0.25), value: gpuState)
            .onChange(of: realTouchscreen) { _ in TouchScreen.applyEnabled() }
            .onChange(of: alwaysNative) { _ in TouchScreen.applyAlways() }
            // The toolbar moves between the bar and the floating pill: a bubble left open would point at nothing.
            .onChange(of: layout.fillsPhone) { _ in showDownloads = false; confirmStop = false }
        }
        .statusBarHidden(zoomed || Device.isPad)
        .persistentSystemOverlays(.hidden)
        // The Mac's menu bar runs along the top edge: a touch there is the Mac's first, Notification Center's on a
        // second swipe.
        .defersSystemGestures(on: .top)
        .preferredColorScheme(.dark)
        .sheet(isPresented: $showDetails) { details }
        .sheet(isPresented: $showDisks) { DisksView() }
        .onReceive(MacDrives.shared.$problem) { problem in
            // The installer wanted with every port taken: Disks, where the person can eject one, says so.
            if case .noPortForTools? = problem, !showDisks {
                showDetails = false
                showDownloads = false
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { showDisks = true }
            }
        }
        .alert("\(mac.name)'s password", isPresented: $askPassword) {
            SecureField("Password", text: $passwordDraft)
            Button("Save") {
                MacPassword.save(passwordDraft, for: mac.id)
                passwordDraft = ""
                passwordSavedChanged()
                // Asked for by a tap, to type it: type it now, once the alert has gone. With Return asked (the
                // banner's Save) only into a password field or the login window still in front: not into whatever
                // has the focus now, which might show it. The Return then goes only where the helper, asked once it
                // is typed, or Henge Tools' system part says a password field is there.
                if let enter = MacPassword.typeOnceSaved, passwordPrompt.saved {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) {
                        if !enter || passwordPlace { MacPassword.typeOrAsk(enter: enter) }
                        else { HengeLog.log("input", "password saved; not typed: no password field or login window in front now") }
                    }
                }
                MacPassword.typeOnceSaved = nil
            }
            if passwordPrompt.saved {
                Button("Forget", role: .destructive) {
                    MacPassword.forget(mac.id); passwordDraft = ""; MacPassword.typeOnceSaved = nil
                    passwordPrompt.forgotten()
                    HengeLog.log("input", "password forgotten (not offered again this run)")
                }
            }
            Button("Cancel", role: .cancel) { passwordDraft = ""; MacPassword.typeOnceSaved = nil }
        } message: {
            Text("Kept in this \(Device.name)'s keychain. The key button above the keyboard types it into the Mac; hold it to change it. It types the keys of a U.S. keyboard.")
        }
        .onReceive(NotificationCenter.default.publisher(for: MacPassword.wanted)) { _ in
            passwordDraft = ""
            askPassword = true
        }
        .onChange(of: zoomed) { on in
            zoomFollowing?.cancel()
            zoomFollowing = on ? Task { await followPointer() } : nil
        }
        .onChange(of: wantsPointerLock) { PointerLock.set($0) }
        .onChange(of: scenePhase) { phase in
            guard phase != .active else {
                display.refresh()
                return
            }
            HardwareKeys.releaseAll()
            HardwareMouse.shared.releaseButtons()
            if phase == .background { dictation.cancel() }
        }
        .onChange(of: showDetails) { showing in
            if showing { HardwareKeys.releaseAll(); StickyKeys.shared.releaseAll(); log = HengeLog.recentLines(30) }
        }
        .onChange(of: showDisks) { showing in
            if showing { HardwareKeys.releaseAll(); StickyKeys.shared.releaseAll() }
        }
        .onChange(of: showDownloads) { showing in
            if showing { HardwareKeys.releaseAll(); StickyKeys.shared.releaseAll(); KeyboardOwner.shared.close("the downloads sheet opened") }
        }
        .onChange(of: downloadToolShown) { shown in if !shown { showDownloads = false } }
        // Offered at a password field or the login window; it stays up until answered, even if the field goes.
        .onChange(of: passwordPlace) { offerPassword($0) }
        .animation(.easeInOut(duration: 0.25), value: passwordPrompt.offering)
        // The login window's password field has the focus, which no helper there says: the keyboard comes up for it.
        .onChange(of: atLogin) { on in
            KeyboardOwner.shared.loginCertain = loginCertain
            KeyboardOwner.shared.loginWindow(on)
        }
        .onChange(of: loginCertain) { KeyboardOwner.shared.loginCertain = $0 }
        .onChange(of: askPassword) { showing in
            if showing { HardwareKeys.releaseAll(); StickyKeys.shared.releaseAll() }
        }
        .onChange(of: confirmStop) { showing in
            if showing { HardwareKeys.releaseAll(); StickyKeys.shared.releaseAll() }
        }
        .onChange(of: pause.isHeld) { held in
            if held { KeyboardOwner.shared.close("the Mac paused") }
        }
        .onChange(of: pause.problem) { problem in
            guard problem != nil else { return }
            DispatchQueue.main.asyncAfter(deadline: .now() + 4) { if pause.problem == problem { pause.problem = nil } }
        }
        .onChange(of: keyboardBlocked) { KeyboardOwner.shared.blocked = $0 }
        // The Mac paused, suspended or stopped, or a sheet or alert is over it: what is said isn't typed into it.
        .onChange(of: dictationBlocked) { if $0 { dictation.cancel() } }
        .onAppear {
            KeyboardOwner.shared.blocked = keyboardBlocked
            KeyboardOwner.shared.start()
            UIApplication.shared.isIdleTimerDisabled = true
            DownloadWatch.shared.sessionStarted()
            MacPassword.mac = mac.id
            passwordPrompt = PasswordPrompt(saved: MacPassword.read(mac.id) != nil, neverAsk: PasswordOffer.declined(mac.id))
            offerPassword(passwordPlace)
            HardwareMouse.shared.start()
            PointerLock.set(wantsPointerLock)
        }
        .onDisappear {
            power.reset()
            if pause.resumeGivenUp == mac.id { pause.resumeGivenUp = nil }
            KeyboardOwner.shared.reset()
            dictation.cancel()
            UIApplication.shared.isIdleTimerDisabled = false
            if MacPassword.mac == mac.id { MacPassword.mac = nil }
            zoomFollowing?.cancel()
            StickyKeys.shared.releaseAll()
            HardwareKeys.releaseAll()
            HardwareMouse.shared.stop()
        }
        .modifier(ExternalMacDisplayModifier(presentation: MacPresentation(display: display, aspect: mac.aspect)))
        .task { await run() }
        .task { await Sharing.followMacClipboard() }
    }

    // MARK: Following the Mac's focus

    /// The keyboard comes up when a text field on the Mac gets the focus after something the person did (a tap on it,
    /// ⌘L), and goes when a tap or key moves the focus off text; KeyboardOwner decides and applies it. A password
    /// field also gets a button that types the saved password and presses Return.
    /// Not of its own accord while something is in front of the Mac.
    /// When dictation ends at once. keyboardBlocked without the app going inactive: iOS's own microphone question
    /// makes it inactive while dictation starts.
    private var dictationBlocked: Bool {
        state != 1 || pause.isHeld || showDetails || showDisks || showDownloads || askPassword || confirmStop
    }

    private var keyboardBlocked: Bool {
        state != 1 || pause.isHeld || scenePhase != .active || showDetails || showDisks || showDownloads || askPassword || confirmStop
    }

    /// Offers to save the Mac's password, at a password field or the login window, if PasswordPrompt says to.
    private func offerPassword(_ wanted: Bool) {
        guard passwordPrompt.update(wanted: wanted) else { return }
        HengeLog.log("input", "password offer: shown (\(macFocus == 2 ? "a password field" : "the login window"), no password saved); stays until answered")
    }

    private func answerPasswordOffer(_ a: PasswordPrompt.Answer) {
        passwordPrompt.answer(a)
        HengeLog.log("input", "password offer: \(a.rawValue)" + (a == .notNow ? " (offered again the next time this Mac runs)" : ""))
    }

    /// A password was saved (or not, the alert's field left empty): the chip and the offer follow.
    private func passwordSavedChanged() {
        passwordPrompt.saved = MacPassword.read(mac.id) != nil
        offerPassword(passwordPlace)
    }

    /// A password field on the Mac, or its login window, with nothing over the Mac.
    private var passwordPlace: Bool {
        state == 1 && !pause.isHeld && !showDetails && !showDisks && !showDownloads
            && (macFocus == 2 || (atLogin && (macFocus == -1 || loginCertain)))
    }

    /// The chip that types the saved password: only with one saved (PasswordPrompt).
    private var showPasswordChip: Bool { passwordPrompt.chip(wanted: passwordPlace) }

    /// Beside the field (under it, or over it if there is no room below) when the helper says where it is.
    /// Otherwise (the login window, before the helper runs) nothing on the screen can be read, so not a guess at
    /// where the field might be: the bottom-left corner of the Mac's screen, which macOS leaves empty there.
    private func passwordChipSpot(_ layout: Layout, full: CGSize) -> CGPoint {
        let screen = layout.screen
        let bottom = full.height - (keyboardTakesRoom ? keyboardCover : 0)
        let seen = screen.intersection(layout.touch).intersection(CGRect(x: 0, y: 0, width: full.width, height: bottom))
        let half: CGFloat = 22, gap: CGFloat = 8, halfWidth: CGFloat = 80
        if macFocus == 2, let f = fieldFrame {
            let field = CGRect(x: screen.minX + f.minX * screen.width, y: screen.minY + f.minY * screen.height,
                               width: f.width * screen.width, height: f.height * screen.height)
            let below = field.maxY + gap + half
            var spot = CGPoint(x: field.midX, y: below + half <= seen.maxY ? below : field.minY - gap - half)
            guard !seen.isNull, seen.width > 2 * halfWidth, seen.height > 2 * half else { return spot }
            spot.x = min(max(spot.x, seen.minX + halfWidth), seen.maxX - halfWidth)
            spot.y = min(max(spot.y, seen.minY + half), seen.maxY - half)
            return spot
        }
        let corner = seen.isNull ? screen : seen
        // Above iPadOS's bar for a hardware keyboard, then above the row of keys where that is along the bottom over
        // the Mac, wherever either would be under it (not the small control, in the other corner).
        var chip = CGRect(x: corner.minX + 16, y: corner.maxY - 16 - 2 * half, width: 2 * halfWidth, height: 2 * half)
        if typing { chip = KeyboardLayout.chipClear(chip, systemBar: rowPlacement.systemBar, row: rowPlacement.glassFrame(in: full)) }
        return CGPoint(x: chip.midX, y: chip.midY)
    }

    private func passwordChip(_ layout: Layout, full: CGSize) -> some View {
        // Return only where something says a password field is there: the system part (the login window), or the
        // helper (asked again once it is typed). A guessed login window (the helper gone a while) may be a logged-in
        // Mac whose helper went: the password alone, and the chip stays for what comes next.
        let login = atLogin && loginCertain
        let enter = login || macFocus == 2
        return Button {
            MacPassword.typeOrAsk(enter: enter)
            // Gone at once: the Mac is logging in (or the dialog closing), and the helper isn't up to say so yet.
            if login, MacPassword.mac.flatMap(MacPassword.read) != nil { atLogin = false }
        } label: {
            Label(login ? "Log In" : "Enter Password", systemImage: "key.fill")
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(.white)
                .padding(.horizontal, 18).padding(.vertical, 11)
                .background(Desk.accent, in: Capsule()) // filled: it stands out on any wallpaper
                .shadow(color: .black.opacity(0.35), radius: 8, y: 2)
        }
        .accessibilityHint("Types the password saved on this \(Device.name)\(enter ? ", then Return" : "")")
        .contextMenu {
            Button("Change Password", systemImage: "key") {
                MacPassword.typeOnceSaved = nil
                NotificationCenter.default.post(name: MacPassword.wanted, object: nil)
            }
        }
        .position(passwordChipSpot(layout, full: full))
        .transition(.opacity)
    }

    /// Where dictation's pill has its bottom right corner: just above the row of Mac keys, at the row's right end
    /// where the mic key is, so it reads as the key's and keeps off the middle of the Mac's screen. With the
    /// keyboard put away, where that row would be.
    private func dictationCorner(_ layout: Layout, full: CGSize) -> CGPoint {
        var rowTop = full.height - (keyboardTakesRoom ? keyboardCover
                                    : layout.insets.bottom + ModifierRow.height + ModifierRow.floatingGap)
        var right = full.width - (rowPlacement.alongBottom ? 18 : 6)
        if typing, let glass = rowPlacement.glassFrame(in: full) {
            // The glass bar (or its folded button) over the Mac, above iPadOS's bar for a hardware keyboard.
            right = glass.maxX
            rowTop = glass.minY
        }
        return CGPoint(x: max(right, 1), y: max(rowTop - 8, 1))
    }

    /// The Mac is made smaller, and the trackpad shorter, for what covers the bottom of the screen while
    /// typing: a docked keyboard, or only the row of keys under a floating one.
    private var keyboardTakesRoom: Bool { typing && keyboardCover > 0 }

    /// What the keyboard takes from the Mac: all it covers, but where the screen is filled, not the folded row's
    /// strip on top of docked keys, which is see-through but for its ⌘ button (TypingBars.macCover).
    private func macKeyboardCover(fills: Bool) -> CGFloat {
        guard keyboardTakesRoom else { return 0 }
        return TypingBars.macCover(keyboardCover, dockedRowFolded: rowPlacement.foldedOverDocked, fills: fills)
    }

    // MARK: The controls while typing

    private var typingBars: TypingBars {
        get { TypingBars(controlsHidden: controlsHidden, controlsHiddenTyping: controlsHiddenTyping) }
        nonmutating set {
            if controlsHidden != newValue.controlsHidden { controlsHidden = newValue.controlsHidden }
            if controlsHiddenTyping != newValue.controlsHiddenTyping { controlsHiddenTyping = newValue.controlsHiddenTyping }
        }
    }

    /// The controls are tucked away, leaving their handle: hidden by their button, or out of the way while typing.
    private var controlsAway: Bool { typingBars.controlsAway(typing: typing) }

    /// The handle, three fingers, or Show Controls: the controls come back without touching the keyboard.
    private func showControls() {
        typingBars.showControls(typing: typing)
        showRevealHint = false
        HengeLog.log("session", "controls shown\(typing ? " while typing: they stay while typing" : "")")
    }

    private var revealHintText: String {
        if revealHintTyping {
            return controlsHandleShown ? "Controls are tucked away while you type. Tap the handle to show them."
                                       : "Controls are tucked away while you type. Tap with three fingers to show them."
        }
        return controlsHandleShown ? "Tap the handle, or with three fingers, to show the controls"
                                   : "Tap with three fingers to show the controls"
    }

    /// The first time the keyboard tucks the controls away, says where they went.
    private func tellTypingTuck(_ layout: Layout) {
        guard layout.fillsPhone, !typingHintShown, controlsHiddenTyping, !controlsHidden else { return }
        typingHintShown = true
        revealHintTyping = true
        showRevealHint = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 4) {
            if revealHintTyping { showRevealHint = false; revealHintTyping = false }
        }
    }

    /// When there is something of the app's to point at, iPadOS gets its own pointer back.
    private var pointerNeeded: Bool { state == 2 || showDetails || showDisks || showDownloads || confirmStop || askPassword || error != nil || pause.isHeld }

    /// The download button is in the toolbar only while a download is on, or one has just finished (SessionDownloads).
    private var downloadToolShown: Bool { downloadWatch.toolWanted }

    /// Where a download's word goes: at the top, under the controls when they float there, and under any other
    /// word already showing there (a pause that failed, the trackpad hint, how to get the controls back).
    private func noticeY(_ layout: Layout) -> CGFloat {
        var top = layout.insets.top + 50
        if pause.problem != nil || pointerRefused || showRevealHint { top += 70 }
        guard layout.fillsPhone, !controlsAway else { return top }
        if let barY = layout.barY { return max(top, barY + 25 + 40) }
        let landscape = layout.touch.width > layout.touch.height
        if landscape || keyboardTakesRoom { return layout.insets.top + 8 + 50 + 40 }
        return top
    }
    /// The iPad's pointer is the Mac's wherever it shows the whole Mac, so it stays free to go back to the
    /// Mac it came from with Universal Control. Zoomed in, or with the Mac on another display, it can't
    /// reach every part of the Mac's screen, so iPadOS is asked to lock it and the raw motion moves the Mac's.
    private var wantsPointerLock: Bool { (zoomed || display.isExternal) && !pointerNeeded }

    // MARK: Layout

    /// Where everything goes, in points from the device's top left corner.
    private struct Layout {
        let screen: CGRect
        let touch: CGRect     // what is a trackpad besides the pad itself
        let fillsPhone: Bool
        let toolsY: CGFloat
        let pad: CGRect
        let insets: EdgeInsets
        var barY: CGFloat?    // where the controls go in the space beside the screen, if it has room for them

        var shown: CGSize {
            CGSize(width: min(touch.width / max(screen.width, 1), 1), height: min(touch.height / max(screen.height, 1), 1))
        }

        /// keyboard: the points the keyboard covers at the bottom while typing; 0 when not typing.
        init(full: CGSize, insets: EdgeInsets, landscape: Bool, zoomed: Bool, keyboard: CGFloat, centre: CGPoint, aspect: CGFloat) {
            self.insets = insets
            let typing = keyboard > 0
            let big = full.height >= 600 // an iPad on its side, or a window on one
            if zoomed {
                // Upright, as tall as the device. On its side, bigger than it fits: a big screen needs less.
                let height = landscape ? full.height * (big ? 1.5 : 2) : max(full.height - insets.bottom, full.width / aspect)
                let width = height * aspect
                // Typing, the view pans within what the keyboard leaves showing, so the pointer stays above it.
                let seen = full.height - keyboard
                let x = min(0, max(full.width - width, full.width / 2 - centre.x * width))
                let y = min(0, max(seen - height, seen / 2 - centre.y * height))
                screen = CGRect(x: x, y: y, width: width, height: height)
                touch = CGRect(x: 0, y: 0, width: full.width, height: seen)
                fillsPhone = true
                toolsY = 0
                pad = .zero
            } else if landscape, big {
                // The Mac above the on-screen keyboard; the controls float over it if there is no room beside it.
                let room = full.height - keyboard
                // An iPad keeps clear of its side insets; a big iPhone (iPhone Duo) fills edge to edge.
                let side = Device.isPad ? 2 * max(insets.leading, insets.trailing) : 0
                let height = max(min(room, (full.width - side) / aspect), 200)
                let width = height * aspect
                let y = max((room - height) / 2, 0)
                screen = CGRect(x: (full.width - width) / 2, y: y, width: width, height: height)
                touch = CGRect(origin: .zero, size: full)
                fillsPhone = true
                toolsY = 0
                pad = .zero
                if y - insets.top >= 54 { barY = insets.top + (y - insets.top) / 2 }
            } else if landscape {
                // Edge to edge. Phones wider than the Mac fill the height with room to spare beside the
                // Dynamic Island; a squarer phone (iPhone Duo) fills all of it, island and corners included.
                let height = min(full.height, full.width / aspect)
                let width = height * aspect
                screen = CGRect(x: (full.width - width) / 2, y: (full.height - height) / 2, width: width, height: height)
                touch = CGRect(origin: .zero, size: full)
                fillsPhone = true
                toolsY = 0
                pad = .zero
            } else {
                let width = full.width - 16
                let height = width / aspect
                screen = CGRect(x: 8, y: insets.top + 6, width: width, height: height)
                touch = screen
                fillsPhone = false
                toolsY = screen.maxY + 10
                let top = typing ? screen.maxY + 8 : toolsY + 54 + 10
                let bottom = full.height - (typing ? keyboard + 8 : insets.bottom)
                pad = CGRect(x: 8, y: top, width: width, height: max(bottom - top, 60))
            }
        }
    }

    // MARK: Upright

    @ViewBuilder private func uprightControls(_ layout: Layout) -> some View {
        if !keyboardTakesRoom && controlsHidden {
            controlsHandle(down: true).position(x: layout.pad.midX, y: layout.toolsY + 14)
        }
        if !keyboardTakesRoom && !controlsHidden {
            HStack {
                tool("eye.slash", "Hide controls") { hideControls() }
                Spacer()
                keyboardTool(tint: Desk.text)
                Spacer()
                pasteTool
                Spacer()
                tool("plus.magnifyingglass", "Zoom in") { zoomed = true }
                Spacer()
                tool("externaldrive", "Disks") { showDisks = true }
                Spacer()
                touchTool(tint: Desk.text)
                Spacer()
                tool("info.circle", "Details") { showDetails = true }
                Spacer()
                if downloadToolShown {
                    DownloadTool(tint: Desk.text, open: $showDownloads)
                    Spacer()
                }
                suspendTool(tint: Desk.text)
                Spacer()
                stopTool
            }
            .padding(.horizontal, 22)
            .frame(width: layout.pad.width, height: 54)
            .background(Desk.card, in: Capsule())
            .overlay(Capsule().stroke(Desk.hairline, lineWidth: 0.5))
            .position(x: layout.pad.midX, y: layout.toolsY + 27)
            .transition(.opacity)
        }
        trackpadPanel(layout)
    }

    private func trackpadPanel(_ layout: Layout) -> some View {
        let compact = layout.pad.height < 260
        return VStack(spacing: 0) {
            ZStack {
                Trackpad()
                if compact {
                    Text("Trackpad").font(.system(size: 14)).foregroundStyle(Desk.faint).allowsHitTesting(false)
                } else {
                    VStack(spacing: 6) {
                        hint("Drag", "to move")
                        hint("Tap", "to click")
                        hint("Hold, then drag", "to grab")
                        hint("Two fingers", "to scroll")
                    }
                    .allowsHitTesting(false)
                }
            }
            Rectangle().fill(Desk.hairline).frame(height: 0.5)
            HStack(spacing: 0) {
                MouseButton(title: "Click", button: 0)
                Rectangle().fill(Desk.hairline).frame(width: 0.5)
                MouseButton(title: "Right-click", button: 1)
            }
            .frame(height: compact ? 50 : 70)
        }
        .background(Desk.card)
        .clipShape(RoundedRectangle(cornerRadius: 30, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 30, style: .continuous).stroke(Desk.hairline, lineWidth: 0.5))
        .frame(width: layout.pad.width, height: layout.pad.height)
        .position(x: layout.pad.midX, y: layout.pad.midY)
    }

    private func hint(_ strong: String, _ rest: String) -> some View {
        (Text(strong).foregroundColor(Desk.dim).fontWeight(.semibold) + Text(" \(rest)").foregroundColor(Desk.faint))
            .font(.system(size: 15))
    }

    /// The system's paste button: iOS asks no permission for it, as it would if the app read the clipboard itself.
    private var pasteTool: some View {
        PasteButton(payloadType: String.self) { texts in
            if let text = texts.first { Sharing.paste(text) }
        }
        .labelStyle(.iconOnly)
        .buttonBorderShape(.circle)
        .tint(Desk.raised)
    }

    /// Where the hidden controls were: a small glass handle, faint over the Mac, that brings them back. (So do three
    /// fingers on the screen.)
    @ViewBuilder private func controlsHandle(down: Bool) -> some View {
        if controlsHandleShown {
        Button { showControls() } label: {
            Image(systemName: down ? "chevron.compact.down" : "chevron.compact.up")
                .font(.system(size: 17, weight: .semibold))
                .foregroundStyle(.white.opacity(0.85))
                .frame(width: 54, height: 22)
                .deskGlass(in: Capsule())
                .opacity(0.8)
                .frame(width: 88, height: 44) // a finger's worth to hit
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Show controls")
        .contextMenu {
            Button("Show Controls", systemImage: "chevron.down") { showControls() }
            Button("Hide Handle", systemImage: "eye.slash") {
                controlsHandleShown = false
                revealHintsShown = 0
                remindReveal()
            }
        }
        .transition(.opacity)
        }
    }

    /// Asked to type with a hardware keyboard connected, iPadOS shows no keyboard of its own and there is no way to
    /// make it. Says so, and which keyboard: Universal Control's (a Mac's keyboard and trackpad) is easily missed.
    private func explainMissingKeyboard() {
        guard let keyboard = GCKeyboard.coalesced else { return }
        let names = [keyboard.vendorName] + GCMouse.mice().map(\.vendorName)
        let universal = names.contains { name in
            guard let name else { return false }
            return name.hasPrefix("V-") || name.contains("UC Automouse")
        }
        // iPadOS keeps its keyboard hidden then, and no app can bring it up; the person can, from the keyboard control
        // iPadOS shows in the bottom corner while Henge has the focus (as it does now), and it stays until put away.
        let how = "To use the on-screen keyboard anyway, touch and hold iPadOS's keyboard icon in the bottom corner of the screen, then choose Show Keyboard."
        let text = universal
            ? "Your Mac's keyboard is connected to this \(Device.name) through Universal Control, so iPadOS hides its on-screen keyboard. \(how)"
            : "A keyboard is connected to this \(Device.name), so iPadOS hides its on-screen keyboard. \(how)"
        HengeLog.log("input", "no on-screen keyboard: a hardware keyboard is connected (\(keyboard.vendorName ?? "?"))\(universal ? ", Universal Control" : ""); said how to show iPadOS's")
        keyboardNotice = text
        DispatchQueue.main.asyncAfter(deadline: .now() + 10) { if keyboardNotice == text { keyboardNotice = nil } }
    }

    /// Hides the session's controls, leaving a small handle; three fingers on the screen bring them back too. The
    /// first few times say so. The keyboard stays as it is: hiding the controls is to see more of the Mac, typing or
    /// not, and they stay hidden while typing too until shown again.
    private func hideControls() {
        typingBars.hideControls()
        HengeLog.log("session", "controls hidden\(typing ? " while typing" : ""); the keyboard stays as it is")
        remindReveal()
    }

    /// The first few times the controls go (or open hidden without a handle), says how to bring them back.
    private func remindReveal() {
        revealHintTyping = false
        guard revealHintsShown < 3 else { return }
        revealHintsShown += 1
        showRevealHint = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) { showRevealHint = false }
    }

    private func tool(_ symbol: String, _ label: String, tint: Color = Desk.text, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 19, weight: .medium))
                .foregroundStyle(tint)
                .frame(width: 44, height: 44)
                .contentShape(Rectangle())
        }
        .accessibilityLabel(label)
    }

    /// The keyboard button: a tap shows or hides the keyboard for now (Show for Text Fields stays as it is); touched and
    /// held, a menu with Show for Text Fields (Details' switch, the same setting). Its look says how that stands: on
    /// (a small A, as automatic), off (no mark), or paused by a hardware keyboard (a pause mark).
    private func keyboardTool(tint: Color) -> some View {
        let paused = autoKeyboard && hardwareKeyboard
        let state = !autoKeyboard ? "off" : paused ? "paused while a keyboard is connected" : "on"
        return Menu {
            if paused {
                Section("Paused: a keyboard is connected to this \(Device.name).") {
                    Toggle("Show for Text Fields", isOn: $autoKeyboard)
                }
            } else {
                Toggle("Show for Text Fields", isOn: $autoKeyboard)
            }
        } label: {
            Image(systemName: "keyboard")
                .font(.system(size: 19, weight: .medium))
                .foregroundStyle(tint)
                .overlay(alignment: .topTrailing) { keyboardBadge(paused: paused).offset(x: 8, y: -7) }
                .frame(width: 44, height: 44)
                .contentShape(Rectangle())
        } primaryAction: {
            KeyboardOwner.shared.toggle()
        }
        .accessibilityLabel("Keyboard")
        .accessibilityValue("Show for Text Fields \(state)")
        .accessibilityHint("Shows or hides the keyboard. Touch and hold for Show for Text Fields.")
    }

    @ViewBuilder private func keyboardBadge(paused: Bool) -> some View {
        if paused {
            Image(systemName: "pause.circle.fill")
                .font(.system(size: 11, weight: .bold))
                .foregroundStyle(.white, Desk.dim)
        } else if autoKeyboard {
            Text("A")
                .font(.system(size: 9, weight: .heavy, design: .rounded))
                .foregroundStyle(.white)
                .frame(width: 13, height: 13)
                .background(Desk.accent, in: Circle())
        }
    }

    /// The first time a hardware keyboard pauses Show for Text Fields, says so, and where the switch is.
    private func hardwareKeyboardChanged() {
        let now = HardwareKeyboard.shared.connected
        guard now != hardwareKeyboard else { return }
        hardwareKeyboard = now
        HengeLog.log("input", now ? "a hardware keyboard is connected: Show for Text Fields pauses" : "no hardware keyboard now: Show for Text Fields goes on again")
        guard now, autoKeyboard, !keyboardPausedTold, keyboardNotice == nil else { return }
        keyboardPausedTold = true
        let text = "A keyboard is connected, so text fields don't bring up the on-screen keyboard. Touch and hold Henge's keyboard button in the controls for Show for Text Fields."
        keyboardNotice = text
        DispatchQueue.main.asyncAfter(deadline: .now() + 6) { if keyboardNotice == text { keyboardNotice = nil } }
    }

    /// Shut down, asking first in a bubble that points at the button (not a sheet along the bottom of the screen).
    /// Shut Down and Restart ask macOS, as the Apple menu does (MacPower): apps quit and can save first. A suspended
    /// Mac is offered Close for Now first, which keeps it suspended; a running or paused one, Suspend. While macOS is
    /// already shutting down, the bubble offers Force Shut Down instead.
    private var stopTool: some View {
        tool("power", pause.suspended ? "Close or shut down" : "Shut down", tint: Desk.bad) { stopTapped() }
            .disabled(pause.suspending) // its save finishes first
            .popover(isPresented: $confirmStop) {
                VStack(spacing: 14) {
                    if power.active {
                        Text(power.title).font(.system(size: 17, weight: .semibold)).multilineTextAlignment(.center)
                        Text("macOS is still on it: an app may be asking something on the Mac's screen. Force Shut Down turns "
                             + "the Mac off now, like holding a Mac's power button: anything not saved in its apps is lost."
                             + (MacAutoSave.on ? " Its last auto-save, if it has one, is kept, and starting the Mac again asks "
                                + "whether to carry on from it." : ""))
                            .font(.system(size: 14))
                            .foregroundStyle(Desk.dim)
                            .multilineTextAlignment(.center)
                            .fixedSize(horizontal: false, vertical: true)
                        HStack(spacing: 10) {
                            Button("Keep Waiting") { confirmStop = false }
                                .buttonStyle(.borderedProminent)
                                .tint(Desk.accent)
                                .keyboardShortcut(.defaultAction)
                            Button("Force Shut Down", role: .destructive) {
                                confirmStop = false
                                forceShutDown()
                            }
                            .buttonStyle(.bordered)
                            .tint(Desk.bad)
                        }
                    } else {
                        if pause.suspended {
                            Text("Close \(mac.name)?").font(.system(size: 17, weight: .semibold))
                            Text("It is suspended. Close for Now keeps it that way: next time it carries on from here, apps open. "
                                 + "Shut Down wakes it and asks macOS to shut down, so its apps can save first.")
                                .font(.system(size: 14))
                                .foregroundStyle(Desk.dim)
                                .multilineTextAlignment(.center)
                                .fixedSize(horizontal: false, vertical: true)
                            Button {
                                confirmStop = false
                                closeSuspended()
                            } label: {
                                Text("Close for Now").frame(maxWidth: .infinity)
                            }
                            .buttonStyle(.borderedProminent)
                            .tint(Desk.accent)
                            .keyboardShortcut(.defaultAction)
                        } else {
                            Text("Shut down \(mac.name)?").font(.system(size: 17, weight: .semibold))
                            Text("As on a Mac: apps quit first and can save, and any with unsaved changes asks you on the Mac's screen."
                                 + (pause.canSuspend ? " To stop for now and carry on later, suspend instead: the Mac is saved with its apps open." : ""))
                                .font(.system(size: 14))
                                .foregroundStyle(Desk.dim)
                                .multilineTextAlignment(.center)
                                .fixedSize(horizontal: false, vertical: true)
                            if pause.canSuspend {
                                Button {
                                    confirmStop = false
                                    pause.suspend()
                                } label: {
                                    Label("Suspend Instead", systemImage: "moon.zzz.fill").frame(maxWidth: .infinity)
                                }
                                .buttonStyle(.borderedProminent)
                                .tint(Desk.accent)
                                .keyboardShortcut(.defaultAction)
                            }
                        }
                        HStack(spacing: 10) {
                            Button("Restart") {
                                confirmStop = false
                                power.begin(.restart, mac: mac)
                            }
                            .buttonStyle(.bordered)
                            .tint(Desk.text)
                            Button("Shut Down", role: .destructive) {
                                confirmStop = false
                                power.begin(.shutDown, mac: mac)
                            }
                            .buttonStyle(.bordered)
                            .tint(Desk.bad)
                        }
                        Button("Cancel") { confirmStop = false }
                            .tint(Desk.dim)
                            .keyboardShortcut(.cancelAction)
                    }
                }
                .controlSize(.large)
                .buttonBorderShape(.capsule)
                .padding(20)
                .frame(width: 300)
                .foregroundStyle(Desk.text)
                .presentationCompactAdaptation(.popover) // a bubble on iPhone too, not a sheet
                .preferredColorScheme(.dark)
            }
    }

    /// Which way touch reaches the Mac, in the controls: what is in use now ("Native" while macOS takes the fingers
    /// as a touchscreen's, "Pointer" while the finger moves the pointer, "Trackpad"), and a menu to switch, with why
    /// the one chosen may not be the one in use.
    private func touchTool(tint: Color) -> some View {
        let native = directTouch && (touchscreenStatus == .works || touchscreenStatus == .trying)
        let name = !directTouch ? "Trackpad" : native ? "Native" : "Pointer"
        let symbol = !directTouch ? "rectangle.and.hand.point.up.left" : native ? "hand.tap.fill" : "cursorarrow.rays"
        let choice = Binding<Int>(
            get: { !directTouch ? 2 : !realTouchscreen ? 1 : alwaysNative ? 3 : 0 },
            set: { pick in
                let again = pick == 0 && realTouchscreen && directTouch && touchscreenStatus == .ignored
                directTouch = pick != 2
                if pick != 2 { realTouchscreen = pick == 0 || pick == 3 }
                if pick == 0 || pick == 3 { alwaysNative = pick == 3 }
                if again { TouchScreen.retry() }
                HengeLog.log("touch", "touch method chosen in the controls: \(["native touchscreen (auto)", "pointer", "trackpad", "native touchscreen (always)"][pick])")
            })
        return Menu {
            Section(touchInUseText) {
                Picker("Touch", selection: choice) {
                    Label("Native (Auto)", systemImage: "hand.tap").tag(0)
                    Label("Native (Always)", systemImage: "hand.tap.fill").tag(3)
                    Label("Pointer", systemImage: "cursorarrow.rays").tag(1)
                    Label("Trackpad", systemImage: "rectangle.and.hand.point.up.left").tag(2)
                }
            }
            if directTouch, realTouchscreen, !alwaysNative, touchscreenStatus == .ignored {
                Button("Try Native Touchscreen Again", systemImage: "arrow.clockwise") { TouchScreen.retry() }
            }
        } label: {
            HStack(spacing: 5) {
                Image(systemName: symbol).font(.system(size: 15, weight: .semibold))
                Text(name).font(.system(size: 13, weight: .semibold))
            }
            .foregroundStyle(native ? Desk.accentLight : tint)
            .padding(.horizontal, 6)
            .frame(height: 44)
            .contentShape(Rectangle())
        }
        .accessibilityLabel("Touch: \(name)")
    }

    /// What touch is doing now, for the touch menu's heading.
    private var touchInUseText: String {
        if !directTouch { return "In use: trackpad. The screen moves the pointer like a trackpad." }
        switch touchscreenStatus {
        case .works: return "In use: native touchscreen. macOS takes your fingers as touches."
        case .trying: return alwaysNative ? "In use: native touchscreen, always. Henge won't switch to the pointer by itself."
                                          : "In use: native touchscreen. macOS has it; checking it responds."
        case .waiting: return realTouchscreen ? "In use: pointer. Waiting for macOS to take the touchscreen." : "In use: pointer."
        case .ignored: return "In use: pointer. macOS didn't respond to the touchscreen. Pick Native (Auto) to try again, or Native (Always) to keep it."
        case .off: return "In use: pointer. Your finger moves the Mac's pointer."
        case .none: return realTouchscreen ? "In use: pointer. The native touchscreen needs macOS 27 and Henge Tools." : "In use: pointer."
        }
    }

    /// The one button for stopping for now: Suspend while the Mac runs (it is frozen and saved whole, open apps
    /// included), Resume once it is held. No pause on its own: on iOS a Mac held only in memory is lost if Henge closes.
    private func suspendTool(tint: Color) -> some View {
        let held = pause.isPaused
        return tool(held ? "play.fill" : "moon.zzz", held ? "Resume" : "Suspend", tint: held ? Desk.accentLight : tint) {
            pause.toggle()
        }
        .accessibilityHint(held ? "Carries on from where the Mac was." : "Saves the whole Mac, open apps included, so you can leave Henge.")
        .disabled(state != 1 || pause.phase == .pausing || pause.phase == .resuming
                  || (pause.phase == .running && !pause.canSuspend)) // Resume while suspending calls the save off
    }

    /// Over the Mac's screen while it is paused or suspended, or on its way in or out: which it is, what that
    /// means for the person's work, and what they can do next.
    private func pausedScreen(_ layout: Layout) -> some View {
        let seen = layout.screen.intersection(layout.touch) // zoomed in, only part of the screen shows
        let small = seen.height < 300
        let title = Font.system(size: small ? 19 : 26, weight: .semibold)
        let note = Font.system(size: small ? 13 : 15)
        return ZStack {
            Color.black.opacity(0.6)
            VStack(spacing: small ? 8 : 14) {
                switch pause.phase {
                case .pausing:
                    ProgressView().tint(.white).controlSize(small ? .regular : .large)
                    Text(pause.suspending ? "Suspending…" : "One moment…").font(.system(size: small ? 17 : 22, weight: .semibold))
                    if !small {
                        Text(pause.suspending ? "Keep Henge open until it says Suspended." : "Saving the Mac's disk to this \(Device.name)")
                            .font(.system(size: 15)).foregroundStyle(.white.opacity(0.7))
                    }
                case .resuming:
                    ProgressView().tint(.white).controlSize(small ? .regular : .large)
                    Text("Resuming…").font(.system(size: small ? 17 : 22, weight: .semibold))
                case let .paused(safe, guestSynced, _):
                    if pause.suspending {
                        ProgressView().tint(.white).controlSize(small ? .regular : .large)
                        Text("Suspending…").font(.system(size: small ? 17 : 22, weight: .semibold))
                        if !small {
                            Text("Saving the whole Mac to this \(Device.name), open apps and windows included."
                                 + " Keep Henge open until it says Suspended.")
                                .font(.system(size: 15)).foregroundStyle(.white.opacity(0.7))
                                .multilineTextAlignment(.center)
                                .frame(maxWidth: 420)
                        }
                        // Don't want to wait: the save is called off and the Mac carries on at once
                        pausedButton("Resume Now", systemImage: "play.fill", small: small) { pause.resume() }
                            .accessibilityHint("Stops saving and carries on with the Mac. It won't be suspended.")
                            .padding(.top, small ? 2 : 6)
                    } else if pause.suspended {
                        Image(systemName: "moon.zzz.fill")
                            .font(.system(size: small ? 30 : 46))
                            .foregroundStyle(Desk.accentLight)
                        Text("Suspended").font(title)
                        Text("The whole Mac is saved to this \(Device.name), open apps and windows included. You can leave Henge, "
                             + "close it, or turn off the \(Device.name): this Mac carries on from here.")
                            .font(note)
                            .foregroundStyle(.white.opacity(0.75))
                            .multilineTextAlignment(.center)
                            .lineLimit(small ? 4 : nil)
                            .frame(maxWidth: 440)
                        if !small {
                            Text("If an update to Henge can't carry it on, Henge asks before starting it afresh.")
                                .font(.system(size: 13))
                                .foregroundStyle(.white.opacity(0.55))
                                .multilineTextAlignment(.center)
                                .frame(maxWidth: 440)
                        }
                        HStack(spacing: 12) {
                            resumeButton(small: small)
                            pausedButton("Close for Now", systemImage: "moon.zzz", small: small) { closeSuspended() }
                                .accessibilityHint("Closes this Mac. It stays suspended, and Resume on the Macs screen carries on from here.")
                        }
                        .padding(.top, small ? 2 : 6)
                    } else {
                        // Held, but not saved whole: a Suspend that failed, or leaving Henge without one. Said as a
                        // warning, never as a calm state: closing Henge now loses the open apps.
                        Image(systemName: "exclamationmark.triangle.fill")
                            .font(.system(size: small ? 30 : 46))
                            .foregroundStyle(Desk.warn)
                        Text("Not Suspended").font(title)
                        Text(notSavedNote(safe: safe, guestSynced: guestSynced))
                            .font(note)
                            .foregroundStyle(Desk.warn)
                            .multilineTextAlignment(.center)
                            .lineLimit(small ? 4 : nil)
                            .minimumScaleFactor(0.85)
                            .frame(maxWidth: 440)
                        HStack(spacing: 12) {
                            resumeButton(small: small)
                            if pause.canSuspend {
                                pausedButton("Try Again", systemImage: "moon.zzz.fill", small: small) { pause.suspend() }
                                    .accessibilityHint("Tries again to save the whole Mac, open apps included.")
                            }
                        }
                        .padding(.top, small ? 2 : 6)
                    }
                case .running:
                    EmptyView()
                }
            }
            .foregroundStyle(.white)
            .padding(.horizontal, 24)
        }
        .frame(width: seen.width, height: seen.height)
        .clipShape(RoundedRectangle(cornerRadius: layout.fillsPhone ? 0 : 15, style: .continuous))
        .contentShape(Rectangle()) // the Mac's screen takes no touches while it is paused
        .position(x: seen.midX, y: seen.midY)
        .accessibilityElement(children: .contain)
        .transition(.opacity)
    }

    private func resumeButton(small: Bool) -> some View {
        Button {
            pause.resume()
        } label: {
            Label("Resume", systemImage: "play.fill")
                .font(.system(size: small ? 15 : 17, weight: .semibold))
                .padding(.horizontal, small ? 18 : 26).padding(.vertical, small ? 8 : 12)
        }
        .buttonStyle(.borderedProminent)
        .buttonBorderShape(.capsule)
        .tint(Desk.accent)
        .keyboardShortcut(.defaultAction)
    }

    /// The other button on the paused screen, beside Resume and quieter than it.
    private func pausedButton(_ title: String, systemImage: String, small: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(title, systemImage: systemImage)
                .font(.system(size: small ? 15 : 17, weight: .semibold))
                .padding(.horizontal, small ? 18 : 26).padding(.vertical, small ? 8 : 12)
        }
        .buttonStyle(.bordered)
        .buttonBorderShape(.capsule)
        .tint(.white)
    }

    /// Why a held Mac isn't saved whole, and what that means. The Mac is frozen either way.
    private func notSavedNote(safe: Bool, guestSynced: Bool) -> String {
        let why = pause.suspendFailure ?? "Henge couldn't save the whole Mac."
        if !safe {
            return why + " Henge also couldn't confirm the Mac's disk reached this \(Device.name)'s storage. Resume and save your work before closing Henge."
        }
        let files = guestSynced ? "Your files are saved" : "The Mac's disk is saved"
        return why + " \(files), but open apps and windows are only in memory: if Henge closes, they are lost. "
            + (pause.canSuspend ? "Resume, or try again." : "Resume, and save your work in the Mac.")
    }

    /// Close, keeping the Mac suspended: the Macs list then shows it Suspended, and Start carries on from here.
    private func closeSuspended() {
        if pause.closeSuspended() { close() }
    }

    // MARK: Filling the phone

    @ViewBuilder private func floatingControls(_ layout: Layout) -> some View {
        let landscape = layout.touch.width > layout.touch.height
        let top = layout.insets.top + (landscape ? 8 : 6)
        let pill = HStack(spacing: 4) {
            tool("eye.slash", "Hide controls", tint: .white) { hideControls() }
            keyboardTool(tint: .white)
            pasteTool
            tool(zoomed ? "minus.magnifyingglass" : "plus.magnifyingglass", zoomed ? "Zoom out" : "Zoom in", tint: zoomed ? Desk.accentLight : .white) { zoomed.toggle() }
            tool("externaldrive", "Disks", tint: .white) { showDisks = true }
            touchTool(tint: .white)
            tool("info.circle", "Details", tint: .white) { showDetails = true }
            if downloadToolShown { DownloadTool(tint: .white, open: $showDownloads) }
            Rectangle().fill(Color.white.opacity(0.3)).frame(width: 1, height: 22).padding(.horizontal, 4)
            suspendTool(tint: .white)
            stopTool
        }
        .padding(.horizontal, 8)
        .frame(height: 50)
        .deskGlass(in: Capsule())
        if controlsAway {
            if let barY = layout.barY {
                controlsHandle(down: true).position(x: layout.touch.midX, y: barY)
            } else if landscape || keyboardTakesRoom {
                controlsHandle(down: true).position(x: layout.touch.midX, y: layout.insets.top + 14)
            } else {
                controlsHandle(down: false).position(x: layout.touch.midX, y: layout.touch.height - layout.insets.bottom - 14)
            }
        } else if let barY = layout.barY {
            pill.position(x: layout.touch.midX, y: barY)
        } else if landscape || keyboardTakesRoom {
            pill.position(x: layout.touch.midX, y: top + 25)
        } else {
            pill.position(x: layout.touch.midX, y: layout.touch.height - layout.insets.bottom - 34)
        }
        if zoomed { zoomMap(layout) }
    }

    /// The whole screen, small, with where the zoomed view is on it. Touch or drag it to move the view there: with
    /// native touch nothing else does (the view follows the pointer, and fingers don't move one). It sits at the top
    /// right, or the top left while the view is at the screen's right edge, so it never hides the Mac's own top-right
    /// corner (Control Center, the clock).
    private func zoomMap(_ layout: Layout) -> some View {
        let width: CGFloat = 112, height = width / mac.aspect
        let atRightEdge = layout.screen.maxX < layout.touch.maxX + 1 && layout.screen.minX < layout.touch.minX - 1
        let onLeft = mapDragging ? mapLeft : atRightEdge
        let shown = layout.shown
        let viewW = min(layout.touch.width / layout.screen.width, 1) * width
        let viewH = min(layout.touch.height / layout.screen.height, 1) * height
        let left = (-layout.screen.minX / layout.screen.width) * width
        let top = (-layout.screen.minY / layout.screen.height) * height
        return ZStack(alignment: .topLeading) {
            Color.black
            if let map { Image(uiImage: map).resizable() }
            RoundedRectangle(cornerRadius: 3).stroke(Color.white, lineWidth: 1.5)
                .frame(width: viewW, height: viewH)
                .offset(x: left, y: top)
        }
        .frame(width: width, height: height)
        .contentShape(Rectangle())
        .gesture(DragGesture(minimumDistance: 0).onChanged { drag in
            if !mapDragging { mapLeft = atRightEdge; mapDragging = true }
            let c = CGPoint(x: min(max(drag.location.x / width, shown.width / 2), 1 - shown.width / 2),
                            y: min(max(drag.location.y / height, shown.height / 2), 1 - shown.height / 2))
            if c != zoomCentre { zoomCentre = c }
        }.updating($mapTouched) { _, touched, _ in touched = true }
         .onEnded { _ in mapDragging = false })
        // A drag cancelled (Control Center, the app switcher, a call) never reaches onEnded: without this the view
        // would stop following the pointer and the map stay on its side until the next drag.
        .onChange(of: mapTouched) { touched in if !touched { mapDragging = false } }
        .clipShape(RoundedRectangle(cornerRadius: 9, style: .continuous))
        .padding(3)
        .deskGlass(in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .position(x: onLeft ? width / 2 + 15 + layout.insets.leading : layout.touch.width - width / 2 - 15 - layout.insets.trailing,
                  y: layout.insets.top + height / 2 + 20)
        .accessibilityElement()
        .accessibilityLabel("Zoomed view")
        .accessibilityHint("Touch or drag to move the view there")
    }

    /// Keeps the pointer in view: the view moves when the pointer nears its edge. Only as the pointer moves, so the view
    /// stays where the map put it; and not for a finger that is the touch or the pointer, which is in view already, and
    /// under which the view must hold still.
    private func followPointer() async {
        var tick = 0
        var was: (Int32, Int32)?
        while !Task.isCancelled {
            var x: Int32 = 0, y: Int32 = 0
            /* The pointer is in the pixels of the size macOS shows now, which its Displays settings can change. */
            var w: Int32 = 0, h: Int32 = 0
            if henge_vm_screen_size(&w, &h) != 0 { (w, h) = (Int32(mac.screen.width), Int32(mac.screen.height)) }
            if henge_vm_pointer(&x, &y) == 0, w > 0, h > 0 {
                let moved = was.map { $0 != (x, y) } ?? true // just zoomed in: to the pointer
                was = (x, y)
                if moved, !Trackpad.holdsView, !mapDragging {
                    moveZoom(toward: CGPoint(x: CGFloat(x) / CGFloat(w), y: CGFloat(y) / CGFloat(h)))
                }
            }
            if tick % 60 == 0, let image = MacPreview.capture(width: 224, height: Int(224 / mac.aspect)) { map = UIImage(cgImage: image) }
            tick += 1
            try? await Task.sleep(nanoseconds: 33_000_000)
        }
    }

    private func moveZoom(toward p: CGPoint) {
        // How much of the screen the view shows, and a margin inside that.
        let shownX = shown.width, shownY = shown.height
        let reachX = shownX * 0.3, reachY = shownY * 0.3
        var c = zoomCentre
        if p.x < c.x - reachX { c.x = p.x + reachX } else if p.x > c.x + reachX { c.x = p.x - reachX }
        if p.y < c.y - reachY { c.y = p.y + reachY } else if p.y > c.y + reachY { c.y = p.y - reachY }
        c.x = min(max(c.x, shownX / 2), 1 - shownX / 2)
        c.y = min(max(c.y, shownY / 2), 1 - shownY / 2)
        guard c != zoomCentre else { return }
        withAnimation(.linear(duration: 0.08)) { zoomCentre = c }
    }

    // MARK: Status

    private func errorCard(_ text: String, _ layout: Layout) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("This Mac could not start").font(.system(size: 17, weight: .semibold))
            Text(text).font(.system(size: 14)).foregroundStyle(Desk.dim)
            Button("Close") { close() }.font(.system(size: 15, weight: .semibold)).foregroundStyle(Desk.accentLight)
        }
        .padding(18)
        .frame(width: min(layout.touch.width, 390) - 32, alignment: .leading)
        .background(Desk.raised, in: RoundedRectangle(cornerRadius: 22, style: .continuous))
        .position(x: (layout.fillsPhone ? layout.touch.midX : layout.pad.midX), y: layout.fillsPhone ? layout.touch.midY : layout.pad.midY)
    }

    private var phase: String {
        switch state {
        case 0: return error == nil ? "Starting…" : "Not started"
        case 1: return pause.suspended ? "Suspended" : pause.isPaused ? (pause.autoSaveCovers ? "Paused, and auto-saved as it paused" : "Not suspended: frozen, open apps not saved") : userCode ? "macOS is running" : "Booting"
        default: return "Stopped (\(String(cString: henge_vm_exit_reason())))"
        }
    }

    private var details: some View {
        NavigationStack {
            List {
                Section {
                    NavigationLink {
                        SettingsView(inSession: true)
                    } label: {
                        Label("Henge Settings", systemImage: "gearshape")
                    }
                } footer: {
                    Text("Henge's own settings, as in its Settings tab, without leaving this Mac.")
                }
                Section(phase) {
                    if let note = pause.autoSaveNote { LabeledContent("Auto-save", value: note) }
                    LabeledContent("CPUs on", value: "\(cpusOn) of \(cpus)")
                    LabeledContent("Temperature", value: Self.thermalLabel(thermalState))
                    LabeledContent("Translated blocks", value: String(format: "%.1f M/s", blocksPerSecond / 1e6))
                    LabeledContent("Network frames", value: "\(framesOut) out, \(framesIn) in")
                    LabeledContent("Up", value: "\(Int(Date().timeIntervalSince(started))) s")
                }
                onTheNetwork
                Section("Display") {
                    Toggle("Color matching", isOn: $matchScreenColors)
                    LabeledContent("Brightness", value: "\(Int(screenBrightness * 100))%")
                    Slider(value: $screenBrightness, in: 0.4...1)
                    Text("Brightness dims only the Mac's screen.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("Keyboard") {
                    Toggle("Show for Text Fields", isOn: $autoKeyboard)
                    Text("The keyboard comes up when you tap a text field on the Mac, and goes when you tap somewhere else. A password field also shows Enter Password, which types the password saved on this \(Device.name) and presses Return when Henge Tools on the Mac says a password field has the focus. Needs Henge Tools on the Mac.")
                        .font(.footnote).foregroundStyle(.secondary)
                    Toggle("Autocorrect", isOn: $autocorrect)
                    Text("Autocorrect, suggestions, capitals and the rest work as this \(Device.name)'s keyboard settings have them, for what you type with the on-screen keyboard. Touch and hold the space bar to move the Mac's text cursor. The ⌘ button above the keyboard opens the Mac's keys (Esc, Tab, Control, Option, Command, arrows); the arrow at the row's left end folds them away again.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("Controls") {
                    Toggle("Show Handle When Hidden", isOn: $controlsHandleShown)
                        .onChange(of: controlsHandleShown) { _ in revealHintsShown = 0 }
                    Text("While you type, the controls tuck away under a small handle; show them while typing and they stay. Hidden controls stay hidden the next time you open the Mac. The handle brings them back; without it, tap the screen with three fingers.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("Touch") {
                    Picker("Touching the Mac's screen", selection: $directTouch) {
                        Text("Direct").tag(true)
                        Text("Trackpad").tag(false)
                    }
                    .pickerStyle(.segmented)
                    Text(directTouch
                         ? "Your finger is the pointer. Tap to click, tap twice to double-click. Drag a window by its title bar, or a slider, a divider or a menu, and it moves at once; swipe anywhere else to scroll, and flick to glide (touch to stop it). Touch and hold until a ring shows to right-click; keep holding and move to drag instead, such as a file, selected text or an icon off the Dock. Two fingers: tap to right-click, pinch to zoom. Apple Pencil clicks and drags as it touches; double-tap or squeeze it to right-click. Zoomed in it works the same; touch or drag the map in the corner to move the view."
                         : "The screen works as a trackpad: drag to move the pointer, tap to click, hold until a ring shows then drag to grab, two fingers to scroll or tap to right-click.")
                        .font(.footnote).foregroundStyle(.secondary)
                    if directTouch {
                        Toggle("Native Touchscreen", isOn: $realTouchscreen)
                        if realTouchscreen {
                            Toggle("Always Native", isOn: $alwaysNative)
                        }
                        Text(touchscreenText)
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                    if directTouch {
                        LabeledContent("Mac helper", value: touchHelper == 2 ? "On" : touchHelper == 1 ? "Needs permission" : "Not set up")
                        if touchHelper == 1 {
                            Button("Open Accessibility Settings on the Mac") { henge_share_touch_ask() }
                        } else if touchHelper == 0 {
                            Button("Use Terminal Instead") {
                                showDetails = false
                                Task {
                                    try? await Task.sleep(nanoseconds: 600_000_000)
                                    await Sharing.typeSetUpCommand()
                                }
                            }
                        }
                        Text(touchHelper == 2
                             ? "The helper in the Mac tells title bars from pages, and scrolls the way a trackpad does."
                             : touchHelper == 1
                             ? "On the Mac, open System Settings, Privacy & Security, Accessibility, and turn on henge-agent. It lets the helper see what is under your finger and scroll smoothly. Until then, hold to drag a window."
                             : "Title bars that drag at once and trackpad-smooth scrolling need Henge Tools in the Mac, set up once. On the Mac, open the Henge Tools disk (on the desktop, or in Finder's sidebar) and double-click Install Henge Tools. Or open Terminal on the Mac, tap Use Terminal Instead, then press Return.")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                }
                Section("Pointer") {
                    Picker("Size", selection: $cursorScale) {
                        Text("1×").tag(1)
                        Text("2×").tag(2)
                        Text("3×").tag(3)
                    }
                    .pickerStyle(.segmented)
                    Toggle("Sharp edges", isOn: $sharpCursor)
                    LabeledContent("Scroll speed", value: String(format: "%.2g×", scrollSpeed))
                    Slider(value: $scrollSpeed, in: ScrollSpeed.range)
                    Text("The Mac supplies a small pointer image. Enlarging it can look blurry; 1× is the sharpest on an iPad. Scroll speed is how far a page moves for your fingers: at 1× it follows them.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
                Section("Sharing") {
                    LabeledContent("Clipboard", value: helperRunning ? "On" : "Not set up")
                    if !helperRunning {
                        Button("Use Terminal Instead") {
                            showDetails = false
                            Task {
                                try? await Task.sleep(nanoseconds: 600_000_000)
                                await Sharing.typeSetUpCommand()
                            }
                        }
                    }
                    Text("Files in Files, Henge, Shared are on the Mac too: in Finder, under My Shared Files, \(UIDevice.current.model).")
                        .font(.footnote).foregroundStyle(.secondary)
                    Text(helperRunning
                         ? "What you copy on the Mac is copied here too, and the paste button pastes from here into the Mac. The shared files are also in the Henge folder in the Mac's home folder."
                         : "To share the clipboard, install Henge Tools once on each Mac: tap Open the Henge Tools Installer below. Terminal works too: open it on the Mac, tap Use Terminal Instead, then press Return. That types \(Sharing.setUpCommand) and needs the Mac's keyboard layout to be U.S.")
                        .font(.footnote).foregroundStyle(.secondary)
                    if Sharing.toolsDisk != nil {
                        Button("Open the Henge Tools Installer") {
                            showDetails = false
                            Task {
                                try? await Task.sleep(nanoseconds: 600_000_000)
                                await Sharing.openToolsInstaller()
                            }
                        }
                        Text("To install, update or remove Henge Tools. Henge plugs the Henge Tools disk into the Mac and opens the installer; the disk is ejected again once it's done.")
                            .font(.footnote).foregroundStyle(.secondary)
                    }
                }
                Section("Log") {
                    VStack(alignment: .leading, spacing: 1) {
                        ForEach(Array(log.suffix(30).enumerated()), id: \.offset) { _, line in
                            Text(line).font(.system(size: 10, design: .monospaced)).foregroundStyle(.secondary)
                        }
                    }
                }
            }
            .navigationTitle("Details")
            .navigationBarTitleDisplayMode(.inline)
        }
        .preferredColorScheme(.dark)
    }

    // MARK: Running

    /// The iPad's clock on the perf line, so a moment someone noticed ("YouTube failed at about 9:41") can be found.
    private static let perfClock: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "HH:mm:ss"
        return f
    }()

    /// The Mac's ports on the network: open on this device's own address, as they would be on a MacBook's. No
    /// switch: the Mac's own Sharing settings and firewall decide what answers.
    private var onTheNetwork: some View {
        Section {
            if let address = macPorts.address {
                LabeledContent("Address", value: address)
            } else {
                LabeledContent("Address", value: "Not on Wi-Fi or Ethernet")
            }
            if !macPorts.name.isEmpty {
                LabeledContent("Name", value: macPorts.taken ? "\(macPorts.name) (another device's)" : macPorts.name)
            }
            if macPorts.denied {
                Text("Henge's Local Network access is off, so the Mac's name and services aren't on the network: other devices reach it only by address. Turn it on in Settings › Privacy & Security › Local Network.")
                    .foregroundStyle(.secondary)
            } else if macPorts.taken {
                Text("Another device on the network is already called this. The Mac takes another name as soon as it sees that, as any Mac would; until then other devices reach it by address.")
                    .foregroundStyle(.secondary)
            }
            if macPorts.unlisted > 0 && !macPorts.denied {
                Text(macPorts.unlisted == 1
                     ? "One of the Mac's services isn't on the network by name: iOS lets Henge announce only the usual kinds. Its port is still open."
                     : "\(macPorts.unlisted) of the Mac's services aren't on the network by name: iOS lets Henge announce only the usual kinds. Their ports are still open.")
                    .foregroundStyle(.secondary)
            }
            if !macPorts.held && !macPorts.denied {
                if macPorts.seen.isEmpty {
                    LabeledContent("Seen from the Mac", value: "No devices yet")
                } else {
                    DisclosureGroup("Seen from the Mac: \(macPorts.seen.count) device\(macPorts.seen.count == 1 ? "" : "s")") {
                        ForEach(Array(macPorts.seen.enumerated()), id: \.offset) { Text($0.element) }
                    }
                }
            }
            if macPorts.held {
                Text("The Mac's ports are closed while it is paused or Henge is off the screen, and open again as the Mac goes on.")
                    .foregroundStyle(.secondary)
            } else if macPorts.total == 0 {
                Text(macPorts.fromHelper
                     ? "The Mac isn't listening for anything from the network. Turn on a service in the Mac's System Settings › General › Sharing, or start a program that listens, and it shows here."
                     : "Opening the Mac's ports…")
                    .foregroundStyle(.secondary)
            } else {
                ForEach(macPorts.ports) { p in
                    LabeledContent {
                        Text(MacPorts.state(p))
                    } label: {
                        Text("\(p.number)\(p.udp ? " UDP" : "")")
                        if let service = MacPorts.service(p) { Text(service) }
                    }
                }
                if macPorts.total > macPorts.ports.count {
                    Text("and \(macPorts.total - macPorts.ports.count) more").foregroundStyle(.secondary)
                }
            }
        } header: {
            Text("On the network")
        } footer: {
            Text((macPorts.address.map { "Other devices on your network reach the Mac at \($0), the \(Device.name)'s own address, on these ports, as they would a MacBook" + (macPorts.name.isEmpty ? ". " : ", and by its name, \(macPorts.name), where its services show in Finder and other apps. ") } ?? "")
                 + "The Mac's own Sharing settings and firewall decide what answers."
                 + (macPorts.fromHelper ? ""
                    : " Henge Tools isn't running in the Mac, so only the usual sharing ports are open: a port a program opens for itself, like a dev server's, isn't."))
        }
    }

    private static func thermalLabel(_ state: ProcessInfo.ThermalState) -> String {
        switch state {
        case .nominal: return "Normal"
        case .fair: return "Warm"
        case .serious: return "Hot — performance reduced"
        case .critical: return "Critical"
        @unknown default: return "Unknown"
        }
    }

    private func run() async {
        let ram = GuestMemory.prepare(mac, saveAhead: GuestMemory.saveAhead(for: mac))
        HengeLog.log("session", "available memory \(henge_available_memory() >> 20) MB")
        // macOS stops at its very first step (an iBoot panic) with too little memory: say why instead. Mostly it is the
        // iPad's free storage, which GuestMemory keeps the Mac's memory within.
        guard ram >= UInt64(min(mac.ramMB, GuestMemory.minimumMB)) else {
            error = GuestMemory.tooLittle(for: mac, got: ram)
            HengeLog.log("session", "not starting: \(ram) MB of memory is too little for macOS")
            return
        }
        JITBootstrap.releaseLingeringDebugger()
        Sharing.prepare(mac)
        MacSpeaker.shared.prepare()
        EngineTuning.apply()
        henge_vm_set_display(UInt32(mac.screen.width), UInt32(mac.screen.height))
        if mac.isRetina {
            henge_vm_set_display_mm(UInt32(Device.retinaMM.width), UInt32(Device.retinaMM.height))
        } else {
            henge_vm_set_display_mm(0, 0)
        }
        HengeLog.log("session", "screen \(mac.screen.width)x\(mac.screen.height) retina \(mac.isRetina)")
        // A fresh start takes the setting; a suspended Mac goes on in the mode it was suspended in.
        henge_vm_set_rosetta(mac.rosetta == true ? 1 : 0)
        if mac.rosetta == true { HengeLog.log("session", "experimental Rosetta mode chosen for a fresh start") }
        // From where it was suspended, if it was (Suspend, Pause.swift); else afresh.
        let adopt: MacSuspend.Saved?
        switch await MacSuspend.boot(mac, ram: ram, cpus: Int32(cpus)) {
        case .started(let saved): adopt = saved
        case .failed(let why):
            error = why
            return
        case .closed:
            close()
            return
        }
        started = Date()
        // Disks plug in as USB drives once the Mac is built: the ones it had, and Henge Tools' if it may need it.
        // A resumed Mac has its drives in already, in the ports they were in: they are taken as they are.
        MacDrives.shared.start(mac, adopting: adopt)
        henge_vm_touchscreen_config(TouchScreen.enabled ? 1 : 0, 0, 0) // the switch, before the Mac's side plugs it in
        henge_vm_touchscreen_always(TouchScreen.always ? 1 : 0)
        defer { MacDrives.shared.stopped() }
        // The Mac's speaker plays here with the iPad's volume as its own (MacSpeaker.swift).
        MacSpeaker.shared.start()
        defer { MacSpeaker.shared.stop() }
        // Touch-first: no arrow in the corner of the login window, as after a finger. Asked before macOS draws,
        // so it never shows; a mouse or trackpad brings it back as it moves.
        // And where it starts: macOS takes it from the touch device, which says the top-left corner until touched.
        // Touch-only, the bottom-left corner, where the arrow falls off the screen (no hot corner there by
        // default, and clear of the Dock); with a mouse, mid-screen, as on a Mac.
        if directTouch, GCMouse.mice().isEmpty, !display.isExternal {
            henge_vm_hide_pointer(1)
            henge_vm_park_pointer(0, 1)
        } else {
            henge_vm_park_pointer(0.5, 0.5)
        }
        MacPause.shared.attach(mac, ramMB: adopt?.ramMB ?? ram, cpus: adopt?.cpus ?? Int32(cpus))
        var countedAsUsed = false // once macOS itself runs: a start that fails never makes a Mac the one used last
        var lastBlocks = henge_vm_blocks()
        var lastTime = Date()
        var lastGPUFrames = henge_vm_gpu_frames()
        var netLost: UInt64 = 0 // frames the Mac's network lost so far, as last logged
        var tick = 0
        var unlinked = 0 // seconds the helper hasn't been linked
        var linkedThisRun = false
        var systemThisRun = false // Henge Tools' system part has spoken this run
        var systemSaid: Int32 = -1, systemSaidAt = Date.distantPast // what it last said, and when
        var loginSaidAt: Date? // when it started saying the login window is up
        var loginSaidFrames: UInt64 = 0 // and how many frames the Mac's GPU had drawn by then
        while !Task.isCancelled {
            state = henge_vm_state()
            cpusOn = henge_vm_cpus_on()
            userCode = userCode || henge_vm_guest_el0() != 0
            if userCode && !countedAsUsed, var used = Machines.shared.all.first(where: { $0.id == mac.id }) {
                countedAsUsed = true
                used.lastUsed = started
                Machines.shared.update(used)
            }
            let now = Date(), blocks = henge_vm_blocks()
            blocksPerSecond = Double(blocks &- lastBlocks) / max(now.timeIntervalSince(lastTime), 0.001)
            lastBlocks = blocks
            lastTime = now
            framesOut = henge_vm_net_frames(1)
            framesIn = henge_vm_net_frames(0)
            let ports = MacPorts.now()
            if ports != macPorts { macPorts = ports }
            var lostToMac: UInt64 = 0, lostFromMac: UInt64 = 0, tooLong: UInt64 = 0, resets: UInt64 = 0
            henge_vm_net_losses(&lostToMac, &lostFromMac, &tooLong, &resets)
            let lost = lostToMac &+ lostFromMac &+ tooLong
            if lost != netLost {
                // Rare, and the moment matters: a web page or video failing can be lined up with it.
                HengeLog.log("net", "\(Self.perfClock.string(from: now)) \(lost &- netLost) frames dropped in the last second "
                             + "(so far: to the Mac \(lostToMac), from it \(lostFromMac), too long \(tooLong)); resets to the Mac \(resets)")
                netLost = lost
            }
            helperRunning = Sharing.helperRunning
            touchHelper = henge_share_touch_ready() != 0 ? 2 : henge_share_touch_linked() != 0 ? 1 : 0
            let ts = TouchScreen.status
            if ts != touchscreenStatus {
                HengeLog.log("touch", "touchscreen status: \(ts)")
                if ts == .ignored {
                    let text = "macOS didn't respond to touch, so Henge moves the pointer with your finger instead."
                    touchNotice = text
                    DispatchQueue.main.asyncAfter(deadline: .now() + 8) { if touchNotice == text { touchNotice = nil } }
                }
                if directTouch, ts == .trying || ts == .works, touchscreenStatus != .trying, touchscreenStatus != .works {
                    let text = "Touch: native touchscreen. macOS takes your fingers as touches."
                    touchNotice = text
                    DispatchQueue.main.asyncAfter(deadline: .now() + 4) { if touchNotice == text { touchNotice = nil } }
                }
                touchscreenStatus = ts
            }
            // Again as macOS first draws, in case the display wasn't ready to be told at the start.
            if !pointerHiddenAtStart, henge_vm_gpu_frames() > 0 {
                pointerHiddenAtStart = true
                if directTouch, GCMouse.mice().isEmpty, !display.isExternal { henge_vm_hide_pointer(1) }
            }
            if touchHelper != 0 {
                if !UserDefaults.standard.bool(forKey: helperSeenKey) { UserDefaults.standard.set(true, forKey: helperSeenKey) }
                unlinked = 0
                linkedThisRun = true
            } else {
                unlinked += 1
            }
            let gpuNow = henge_vm_gpu_state()
            if gpuNow != gpuState {
                // The Mac's graphics after a GPU failure (GraphicsStoppedNote). Shown again when they stop (anew, or for
                // good), and once Try Again can be pressed (closed while the GPU was still busy).
                if gpuState == 0 || gpuNow != 2 { gpuNoteClosed = false }
                if gpuState == 0 { HengeLog.log("session", "the iPad's GPU failed work the Mac sent it: the Mac's graphics are stopped (state \(gpuNow))") }
                if gpuNow == 3 { HengeLog.log("session", "the Mac's graphics are stopped for this run: restarting the Mac brings them back") }
                gpuState = gpuNow
            }
            if !graphicsRefused, henge_vm_gpu_code_refused() != 0 {
                graphicsRefused = true
                HengeLog.log("session", "\(GraphicsSupport.systemName)'s Metal refused GPU code macOS sent: the Mac's screen may stay black (\(GraphicsSupport.updateName) or later reads it)")
            }
            // The helper comes up seconds after someone logs in; before that, a Mac that has had it is at the login
            // window once macOS shows its pointer. (If the pointer can't be told, after a while, as before.)
            if !macPointerSeen, henge_vm_pointer_shown() != 0 {
                macPointerSeen = true
                HengeLog.log("session", "macOS shows its pointer \(Int(Date().timeIntervalSince(started))) s in: the login window is up")
            }
            let session = henge_share_session()
            if session >= 0 { systemSaid = session; systemSaidAt = Date() }
            if session >= 0, !systemThisRun {
                systemThisRun = true
                HengeLog.log("session", "Henge Tools' system part is linked: it says when the Mac is at its login window")
            }
            let login: Bool
            if systemThisRun {
                // For certain. Return just pressed there is someone logging in: hidden a moment, back if the password was wrong.
                // While its link drops for a moment (it is updated, or restarts), what it last said stands.
                let said = session >= 0 ? session : Date().timeIntervalSince(systemSaidAt) < 30 ? systemSaid : -1
                if said != 1 { loginSaidAt = nil } else if loginSaidAt == nil { loginSaidAt = Date(); loginSaidFrames = henge_vm_gpu_frames() }
                // It says so seconds before the login window is drawn: once macOS shows its pointer, which it may not
                // while the touchscreen is in; or once the GPU has drawn for a moment since (the boot screens before
                // it aren't the GPU's); or, should neither be told, a while after.
                let saidFor = loginSaidAt.map { Date().timeIntervalSince($0) } ?? 0
                let drawing = saidFor > 1.5 && henge_vm_gpu_frames() &- loginSaidFrames >= 45
                let drawn = macPointerSeen || drawing || saidFor > 8
                login = said == 1 && drawn && Date().timeIntervalSince(Keys.lastEnter) > 8
            } else {
                let loggingIn = Date().timeIntervalSince(Keys.lastEnter) < 45
                // Once the helper has been up this run, only a long gap is a logout: it drops for a moment as it updates.
                let wait = linkedThisRun ? 20 : macPointerSeen ? 1 : 30
                login = userCode && unlinked >= wait && !loggingIn
                    && UserDefaults.standard.bool(forKey: helperSeenKey)
            }
            if systemThisRun != loginCertain { loginCertain = systemThisRun }
            if login != atLogin { atLogin = login }
            // Only shown in Details: copying it each second otherwise was main-thread work for nothing.
            if showDetails { log = HengeLog.recentLines(30) }
            MacDrives.shared.tick()
            let heat = ProcessInfo.processInfo.thermalState
            if heat.rawValue != thermalState.rawValue {
                thermalState = heat
                HengeLog.log("session", "thermal state: \(Self.thermalLabel(heat))")
            }
            if state == 2 {
                MacPause.shared.stopped()
                macStopped()
                break
            }
            tick += 1
            if tick % 10 == 0 {
                JITBootstrap.releaseLingeringDebugger()
                let gpuFrames = henge_vm_gpu_frames()
                let lostText = lost > 0 ? ", dropped \(lost)" : ""
                HengeLog.log("perf", "\(Self.perfClock.string(from: now)) blocks/s \(String(format: "%.0f", blocksPerSecond)), "
                             + "GPU frames/10s \(gpuFrames &- lastGPUFrames), network tx \(framesOut), rx \(framesIn), "
                             + "resets to the Mac \(resets)\(lostText), "
                             + "thermal \(Self.thermalLabel(heat))")
                lastGPUFrames = gpuFrames
                // The network over the same 10 s, while anything moves: speed each way, how quickly macOS
                // acknowledges, and frames waiting or lost (a slow network shows here before anywhere else).
                var line = [CChar](repeating: 0, count: 512)
                if henge_vm_net_report(&line, line.count) != 0 {
                    HengeLog.log("net", String(cString: line))
                }
            }
            // About when the desktop is up after a login; not while the Mac is still at its login window or paused.
            if tick >= 60 && !stillWallpaperTipShown && !atLogin && !pause.isHeld {
                stillWallpaperTipShown = true
                showWallpaperTip = true
                DispatchQueue.main.asyncAfter(deadline: .now() + 10) { showWallpaperTip = false }
            }
            // How close to the limit, for a memory kill leaves no crash report. Not on the main thread: native code
            // writes it to stderr, a file, synchronously. Not utility either: iOS throttles that I/O, with stderr's lock held.
            if tick % 30 == 0 { DispatchQueue.global(qos: .userInitiated).async { HengeLog.logFootprint("session") } }
            if tick % 60 == 0 { MacPreview.save(for: mac) }
            try? await Task.sleep(nanoseconds: 1_000_000_000)
        }
    }

    /// What Native Touchscreen is doing, in a sentence or two.
    private var touchscreenText: String {
        switch touchscreenStatus {
        case .off, .none:
            return "Off. Your finger moves the Mac's pointer, as on macOS before 27. Turn it on to have macOS take your fingers as touches."
        case .waiting:
            return "macOS 27 can take your fingers as touches on a touchscreen. Waiting for it to take Henge's; until then your finger moves the pointer."
        case .trying:
            return alwaysNative
                ? "On, always: your fingers go to macOS as touches whenever it has the touchscreen, and Henge never switches to the pointer by itself. Turn off Always Native to have it switch back when macOS doesn't respond. Zoomed in too: touch or drag the map in the corner to move the view."
                : "On: your fingers go to macOS as touches. If macOS doesn't respond to a touch, Henge goes back to moving the pointer with your finger by itself. Zoomed in too: touch or drag the map in the corner to move the view."
        case .works:
            return "On: macOS takes your fingers as touches. If it stops responding, Henge goes back to moving the pointer with your finger by itself. Zoomed in too: touch or drag the map in the corner to move the view."
        case .ignored:
            return "macOS didn't respond to touches, so your finger moves the pointer instead. To try the touchscreen again, turn this off and on, or restart the Mac."
        }
    }

    private var helperSeenKey: String { "henge.helperSeen.\(mac.id.uuidString)" }

    /// The Mac has stopped by itself (macOS shut down or restarted, a panic, an error) and the session stays up to
    /// say so: nothing is held down on it any more, the iPad may sleep again, and if macOS asked to restart, Henge
    /// starts it again (one Mac per launch of Henge: Relaunch). After Henge's own Shut down the session has closed.
    private func macStopped() {
        haltedBy = henge_vm_halted_by()
        let reason = String(cString: henge_vm_exit_reason())
        HengeLog.log("session", "the Mac stopped: \(Self.haltedByLabel(haltedBy)), exit \(reason)")
        KeyboardOwner.shared.close("the Mac stopped")
        showDetails = false
        confirmStop = false
        zoomFollowing?.cancel()
        StickyKeys.shared.releaseAll()
        HardwareKeys.releaseAll()
        HardwareMouse.shared.releaseButtons()
        UIApplication.shared.isIdleTimerDisabled = false
        let asked = power.asked
        power.reset()
        if haltedBy == 2 {
            HengeLog.log("session", "macOS restarted: starting the Mac again")
            Relaunch.shared.start(mac)
        } else if haltedBy == 1 && asked != nil {
            // The Shut Down asked for in Henge has happened: the session ends, as it would have at once before.
            HengeLog.log("session", "macOS shut down as asked: closing the session")
            close()
        }
    }

    private static func haltedByLabel(_ by: Int32) -> String {
        switch by {
        case 1: return "macOS shut down"
        case 2: return "macOS restarted"
        case 3: return "stopped by Henge or an error"
        case 4: return "macOS panicked"
        default: return "reason unknown"
        }
    }

    private func stoppedCard(_ layout: Layout, full: CGSize) -> some View {
        ZStack {
            Color.black.opacity(0.55) // over the last frame macOS drew, and the trackpad: nothing there answers now
                .frame(width: full.width, height: full.height)
                .position(x: full.width / 2, y: full.height / 2)
                .contentShape(Rectangle())
            StoppedMacView(haltedBy: haltedBy, reason: String(cString: henge_vm_exit_reason()),
                           restarting: relaunch.restarting != nil,
                           start: { Relaunch.shared.start(mac) }, close: close)
                .frame(width: min(layout.touch.width, 390) - 32)
                .position(x: layout.fillsPhone ? layout.touch.midX : layout.pad.midX,
                          y: layout.fillsPhone ? layout.touch.midY : layout.pad.midY)
        }
        .transition(.opacity)
    }

    private func stopTapped() {
        if state == 1 { confirmStop = true } else { close() }
    }

    /// Force Shut Down: the power goes now (macOS writes its cache first, if it can), as holding a Mac's power
    /// button does. Only from the bubble or the banner, once a Shut Down or Restart asked of macOS is under way.
    private func forceShutDown() {
        HengeLog.log("power", "Force Shut Down")
        power.reset()
        if state == 1 {
            if !pause.isPaused { MacPreview.saveNow(for: mac) } // a paused Mac's screen is saved already
            pause.shutDown()
        }
        close()
    }
}

/// The Mac's ports on the network, for the Details sheet: what host/net_forward.c has open on this device, read
/// once a second. Each is the same port on the device's own address, as a MacBook's would be on its own.
struct MacPorts: Equatable {
    struct Port: Equatable, Identifiable {
        let number: UInt16
        let udp: Bool
        let err: Int32 // 0: open
        var id: Int { Int(number) * 2 + (udp ? 1 : 0) }
    }
    var ports: [Port] = []
    var total = 0
    var fromHelper = false
    var held = false
    var address: String?
    var name = ""
    var taken = false
    var denied = false
    var unlisted = 0
    var seen: [String] = []

    static func now() -> MacPorts {
        let max = 64
        var numbers = [UInt16](repeating: 0, count: max), udp = [UInt8](repeating: 0, count: max)
        var errs = [Int32](repeating: 0, count: max)
        var helper: Int32 = 0, held: Int32 = 0
        let n = Int(henge_vm_forward_status(&numbers, &udp, &errs, Int32(max), &helper, &held))
        var m = MacPorts(total: n, fromHelper: helper != 0, held: held != 0, address: wifiAddress())
        m.ports = (0..<min(n, max)).map { Port(number: numbers[$0], udp: udp[$0] != 0, err: errs[$0]) }
        var name = [CChar](repeating: 0, count: 260)
        var taken: Int32 = 0, services: Int32 = 0, denied: Int32 = 0, unlisted: Int32 = 0, seen: Int32 = 0
        henge_vm_mdns_status(&name, Int32(name.count), &taken, &services, &denied, &unlisted, &seen)
        m.name = String(cString: name)
        m.taken = taken != 0
        m.denied = denied != 0
        m.unlisted = Int(unlisted)
        if seen > 0 {
            var list = [CChar](repeating: 0, count: 2048)
            _ = henge_vm_mdns_seen(&list, Int32(list.count))
            m.seen = String(cString: list).split(separator: "\n").map(String.init)
        }
        return m
    }

    /// What the port is for, when it is one macOS's sharing uses.
    static func service(_ p: Port) -> String? {
        switch (p.number, p.udp) {
        case (22, false): return "Remote Login (SSH)"
        case (88, _): return "Kerberos (Screen and File Sharing logins)"
        case (445, false): return "File Sharing"
        case (548, false): return "File Sharing (AFP)"
        case (631, false): return "Printer Sharing"
        case (3283, _): return "Remote Management"
        case (5900, false): return "Screen Sharing"
        case (7000, false): return "AirPlay Receiver"
        default: return nil
        }
    }

    /// Open, or why it can't be the Mac's.
    static func state(_ p: Port) -> String {
        switch p.err {
        case 0: return "Open"
        case EADDRINUSE: return "The \(Device.name) itself uses it"
        case EACCES, EPERM: return "iOS keeps it for its own"
        default: return String(cString: strerror(p.err))
        }
    }

    /// The device's own IPv4 address on Wi-Fi or Ethernet, as other devices on the network reach it.
    private static func wifiAddress() -> String? {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return nil }
        defer { freeifaddrs(list) }
        for p in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let ifa = p.pointee
            let name = String(cString: ifa.ifa_name)
            guard name.hasPrefix("en"), (ifa.ifa_flags & UInt32(IFF_UP)) != 0, (ifa.ifa_flags & UInt32(IFF_LOOPBACK)) == 0,
                  let sa = ifa.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET) else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            guard getnameinfo(sa, socklen_t(sa.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST) == 0 else { continue }
            return String(cString: host)
        }
        return nil
    }
}
