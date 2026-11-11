# Changelog

This fork collects the fixes from the community forks of [Leviidev/Henge](https://github.com/Leviidev/Henge)
into one build, with a few additions, aimed above all at iPad (tested hardware: iPad Pro M4).
Every change keeps its original author in the git history.

## Unreleased

### The iPad's battery on the Mac
- Settings > "Show this iPad's battery on the Mac" gives the Mac Apple's virtual-Mac battery
  (`devices/power_source.c`, PCI 0x106b:0x1a07, driven by macOS's own AppleVirtualPlatformPowerSource),
  fed from the iPad's charge and charger state (`ios/Henge/Battery.swift`). The device and its protocol
  notes are walriyami's (github.com/walriyami/Henge); here it is behind `HV_BATTERY=1`, off by default,
  and takes the PCI slot after every other device. Not yet run on a device.

### Microphone
- Settings > Sound > Microphone gives the Mac this device's microphone: the virtual sound card
  (`devices/virtio_snd.c`) offers an input stream, filled from a RemoteIO input unit
  (`host/audio_mac.c`). Off by default, and then the sound card is output only, as before.
  `HV_MIC=2` in `henge-env.txt` offers a stereo stream instead of mono. Not yet run on a device.

### The screen is a Magic Trackpad 2
- The session's trackpad is no longer a relative mouse: the engine presents it to the Mac as
  a Magic Trackpad 2 (Apple's vendor multitouch format) on the xHCI USB bus, so the guest's
  own multitouch engine does the pointing and gestures: two-finger scroll, three-finger
  swipes for Spaces, Mission Control and App Exposé, two-finger taps for right-click, and so
  on. Settings > Trackpad picks the input mode: the touchscreen itself, or the volume buttons.
- **Touchscreen mode**: a quick tap is a left click, a quick two-finger tap is a right click,
  and touching then holding presses the button down so moving drags.
- **Volume-button mode** (iPhone): up is left click, down is right click (or swap them), and
  the surface only points and gestures. Settings offers three ways to read the buttons:
  *delay hold* (wait to detect a hold, so the click lags), *tap then drag* (click at once,
  then press down if still held), and *camera intercept* (true press/release via the camera;
  it always runs a frame stream, which the capture interaction needs, so the camera stays on).
- The session controls are now shown with a four-finger tap, leaving three fingers free for
  the Mac's own gestures.
- `devices/xhci.c` generalises the xHCI host to two USB devices (the keyboard and the
  trackpad). The report format follows the Linux kernel's `hid-magicmouse` MT2 support and
  VoodooInput's Magic Trackpad II emulation.

## 0.2.0 (100)

Based on upstream `main` at b744a78.

### Display colour
- macOS no longer looks pale: the half-float display surface is shown as WindowServer wrote it
  instead of being sRGB-encoded a second time. Colour probe for diagnosis (`HV_COLOR_PROBE`).
  — [hungbenjamin402](https://github.com/hungbenjamin402/Henge)
- Colour matching for the iPad's P3 panel (the Mac's frames are tagged sRGB), with live
  *Color matching*, *Linear color conversion* and *Brightness* controls in the session's Details.
  — [sonnx0868](https://github.com/sonnx0868/Henge)

### iPad, Magic Keyboard and trackpad
- Magic Keyboard scrolling calibrated, smoother input, and the device's thermal state in Details.
- Smaller, sharper pointer on iPad, with a size and a sharp-edges setting.
- Video planes copied through the IOSurface's real layout.
  — all three: [sonnx0868](https://github.com/sonnx0868/Henge)
- iPadOS's own hide-keyboard key ends typing, so the Mac takes the full screen again.
  — [gavinnolan](https://github.com/gavinnolan/Henge)
- Screen modes for a Mac on iPad: *Retina* (every pixel of the panel, at 2x), *Performance*
  (about 30% fewer pixels), *Fit this iPad* and *16:9*. Retina is opt-in: it costs a lot of
  emulated GPU work.
  — Retina from [AdamLovattDevOps](https://github.com/AdamLovattDevOps/Henge), Performance from sonnx0868
- 120 Hz ProMotion support, 4 GB default memory on 8 GB iPads, sampling profiler and GPU
  trace/shader dump tools. — [AdamLovattDevOps](https://github.com/AdamLovattDevOps/Henge)

### External display
- A Mac on an external display before iOS 27 too (iOS 17–26), the picture coming back to the
  device when the monitor is unplugged, and unplugging one display no longer freezes the others.
  — [hungbenjamin402](https://github.com/hungbenjamin402/Henge)

### JIT
- At launch, LocalDevVPN is connected first when it is off.
  — [braedonsaunders](https://github.com/braedonsaunders/Henge) (upstream PR #3)
- JIT on cellular: explains the failure and retries instead of blaming LocalDevVPN.
- Personal (free) teams can sign the app: `HENGE_ENTITLEMENTS`.
  — both: [hungbenjamin402](https://github.com/hungbenjamin402/Henge)

### New in this fork
- **Performance settings** (Settings → Performance), applied each time a Mac starts:
  - *Mac refresh rate*: 60 Hz by default. Every frame is emulated and replayed, so 120 Hz
    halves the CPU left to apps; 120 Hz stays available.
  - *Faster disk writes* (on): `F_BARRIERFSYNC` instead of a full `fsync`.
  - *Quicker wake from idle* (off): an idle CPU watches for work for 50 µs before sleeping.
  - *Superblocks (experimental)* (off): translated code runs on past conditional branches.
  - `Documents/henge-env.txt` still overrides everything.
- **Hide the session controls**: a button left of Keyboard hides the bar over the Mac; a
  four-finger tap brings it back (a hint says so the first three times). iPadOS's own
  three-finger undo/copy/paste bar is turned off inside the session so it doesn't interfere.
- Merges kept the Xcode 27 build: the built-in StikJIT helper and the Liquid Glass icon stay
  (the m4 fork's Xcode 16 changes, which remove them, are left out).
- Display name back to *Henge*; version 0.2.0 (100) so sideloading stores treat it as an update.

### Not included
- `HV_GROUPS` (grouped memory accesses): upstream measured it no faster and it fills the JIT
  region sooner.

## Building

As upstream (see README): Xcode 27, `xcodegen`, `meson`, `rustup` with `aarch64-apple-ios`,
and a built [Husk](https://github.com/Leviidev/Husk) at `~/Documents/Coding/Husk`. Only Husk's
`build_ios.sh libffi glib libslirp` and `build_rppairing_ios.sh` are needed for Henge.
