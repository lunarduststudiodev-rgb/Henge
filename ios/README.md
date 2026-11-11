# Henge for iOS

The Henge app on HengeVM's engine. Build: `ios/scripts/package_ipa.sh` (writes
`~/Desktop/Henge.ipa`; `--trollstore` writes `Henge.tipa`, for TrollStore and
TrollStore Lite, without the entitlements TrollStore bans). Needs Xcode 27 or later, xcodegen,
and the prebuilt pieces
`ios/scripts/fetch_deps.sh` copies from Husk (StikJIT, the pairing library,
and glib for iOS; libslirp is built from `third_party/libslirp` against that glib's headers in
`third_party/glib-ios`). `HENGE_ENTITLEMENTS` picks the app's entitlements file
(default `Henge/Henge.entitlements`). A free personal team uses
`Henge/Henge.personal.entitlements`: pass it to xcodebuild as a build setting
(`xcodebuild ... HENGE_ENTITLEMENTS=Henge/Henge.personal.entitlements`, since
`project.yml` sets it and an environment variable does not override that), and to
the packager as an environment variable (`HENGE_ENTITLEMENTS=... ios/scripts/package_ipa.sh`).

- **JIT**: on iOS 26+ (TXM) only an attached debugger can grant executable
  memory. Henge uses the same machinery as Husk: StikDebug, TrollStore, a
  jailbreak, or the built-in StikJIT helper (`HengeJITHelper.appex`, with
  on-device pairing on iOS 27). The app claims one 1 GB region per launch
  (`JITBootstrap.jitBytes`) and the engine writes through a `vm_remap`'d alias
  (`host/jit_region.c`, split mode).
- **Memory**: `increased-memory-limit` is recommended, not required. Once a
  debugger has attached, guest RAM is kept out of the app's memory limit, and if
  iOS refuses that it comes from a file on free storage instead.
- **Machines** are installed on the iPhone from Apple's restore image
  (`Installer.swift`, [docs/architecture.md](../docs/architecture.md)). A machine
  can also be exported from the app, or made on a Mac with
  `tools/export_ios.sh DISK AUX ECID [NAME]` (disk packed with its holes left out,
  see `host/hvsparse.h`), and dropped on Henge in Finder or picked in Files.
- **Boot ROM**: fetched from Apple at install, not shipped (`Henge/BootROM.swift`,
  [docs/architecture.md](../docs/architecture.md)).
- **Display**: iOS has no ParavirtualizedGraphics framework, so Henge replays the
  guest's GPU commands with Metal (`devices/pvg_lite.c`, `devices/pvg_metal.m`).
- **External display**: on iOS 27, `SessionView` registers a SwiftUI
  `ExternalNonInteractiveAccessory` during a Mac session. On iOS 17–26 iOS offers
  the display scene to the app delegate instead (`HengeAppDelegate`), and
  `LegacyExternalDisplay` shows the session there as `ExternalScreenPlan` decides;
  outside a session the display mirrors the phone. `DisplayRouter` selects
  one live `CAMetalLayer`, preferring the external surface and returning to the
  device on disconnect. The phone's session owns the VM and all input; the
  accessory only presents pixels. The existing guest resolution is unchanged
  (1920×1080 by default on iPhone), fitted to the monitor without stretching.
  USB-C video and a hub's keyboard/mouse can be used together when iOS recognizes
  the peripherals. AirPlay may also provide an external scene, with
  transport-dependent latency. Run `make test-display-routing`
  for routing lifecycle checks; `ios/scripts/check_display.sh` also compiles all
  app Swift sources and the iOS/macOS renderer without linking external libraries.
  USB-C video is user-confirmed on an iPhone 17 Pro running iOS 27 with
  development build 0.1/2; activation is automatic during a Mac session. The
  iOS 17–26 path compiles and its plan is tested, but has not run on hardware yet.
  Remaining acceptance checks: connect before/during boot, unplug/replug with an
  idle desktop, rotate the phone, use keyboard/mouse through the dock, and end
  the session while connected. These individual lifecycle cases, other devices
  and AirPlay have not been separately verified.
- **Sharing** (`Henge/Sharing.swift`, `host/share.c`,
  [docs/architecture.md](../docs/architecture.md)): the Shared folder in Files and
  the clipboard. The Mac's clipboard follows along; going the other way uses the
  system paste button, since iOS asks permission when an app reads it on its own.
- **Hardware input** (`Henge/Hardware.swift`): a keyboard's keys go to the Mac's
  USB keyboard by HID usage, which is how iOS reports them. A trackpad or mouse
  goes through GameController with the pointer locked, which iPadOS allows only
  for an app filling the screen. The first scroll values are logged to
  `henge.log`, to tune scrolling from a real device.
- **Chips**: the virtual CPU offers Armv8.5 instructions (`core/sysreg.c`), which
  macOS uses and which run as they are, so Henge needs an A14 or M1 or newer.

Code from Husk (GPL-2.0-or-later): `Henge/JIT/*`, `Henge/HengeLog.swift`,
`Henge/Native/henge-ios-jit.*`, `henge-brk.S`, `HengeJITHelper/`. StikJIT is
MPL-2.0 and idevice MIT (`Henge/Resources/legal/`); `tcg-apple-jit.h` is QEMU's
(LGPL-2.1). See [docs/licensing.md](../docs/licensing.md).
