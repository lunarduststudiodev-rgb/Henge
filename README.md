<p align="center">
  <img src="docs/icon.png" width="120" height="120" alt="Henge">
</p>

<h1 align="center">Henge</h1>

<p align="center">macOS on your iPhone and iPad.</p>

<p align="center">
  <a href="#install">Install</a>
  ·
  <a href="docs/architecture.md">How it works</a>
  ·
  <a href="#building">Build it yourself</a>
  ·
  <a href="../../releases">Releases</a>
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--2.0--or--later-2E6BFF?style=flat-square&labelColor=111111" alt="GPL-2.0-or-later"></a>
</p>

Download macOS, install it on the device, and use it.

> This is a fork of [Leviidev/Henge](https://github.com/Leviidev/Henge), aimed above all at iPad. It collects fixes
> from the community forks (see [CHANGELOG.md](CHANGELOG.md)) and adds its own engine work: a faster translator,
> a code cache that frees room in the background instead of stopping the Mac, and many graphics and stability fixes.
> Credit for Henge itself goes to its author, Leviidev; the original project's site is [itslevii.dev](https://itslevii.dev).

- **Own emulator** — iOS gives apps no hypervisor, so Henge runs the Mac's arm64 code through its own JIT, with its own MMU and devices. It doesn't use QEMU or Virtualization.framework.
- **Installs on the device** — Henge downloads Apple's restore image and restores it the way a Mac does. No Mac needed, and nothing is decrypted or patched.
- **Graphics** — macOS draws through a virtual GPU, and Henge replays its commands with Metal. Safari and Electron apps like Discord draw on it.
- **Touch** — The screen is a trackpad: hold to drag, two fingers to scroll. Esc, Tab, Control, Option and Command sit above the keyboard.
- **Keyboard and mouse** — On iPad, a Magic Keyboard or a Bluetooth keyboard and mouse drive the Mac directly.
- **External display** — During a Mac session, connect a display with a video-capable USB-C cable or a USB-C/HDMI dock. The mode activates automatically: the monitor shows the Mac, while the iPhone or iPad keeps its trackpad and keyboard controls. Disconnect to return the image to the device. The existing guest resolution is preserved. On iOS 17–26 the same happens through an older iOS route, not yet tried on hardware; outside a Mac session the display mirrors the device. USB-C video is user-confirmed on an iPhone 17 Pro running iOS 27 (development build 0.1/2).
- **Sharing** — Copy and paste between macOS and the device, and a shared folder that shows up in Files and on the Mac.
- **Fits the screen** — On iPad, a new Mac gets a screen the shape of the iPad's, so there are no black bars.

On an iPhone 17 Pro, macOS reaches the lock screen in about 15 seconds. Discord installs, opens in about 15 seconds, logs in and sends messages.

## Install

Sideload `Henge.ipa` from [Releases](../../releases) with SideStore or AltStore, or install `Henge.tipa` with TrollStore or TrollStore Lite. iOS or iPadOS 17 or later, on an A14 or M1 chip or newer: see [What you need](#what-you-need) for which devices and versions work. Upstream is tested on an iPhone 17 Pro; this build is tested on an iPad Pro with M4.

- **JIT** is required: StikDebug, TrollStore (which stops at iOS 17.0), TrollStore Lite (which needs a jailbreak), a jailbreak on its own, or the StikJIT helper built into Henge (iOS 26 or later).
- **Storage:** about 43 GB free for macOS Sequoia. That's 17 GB for the download, which you can delete afterwards, and 26 GB for the Mac.
- **Memory:** a Mac gets 4, 6 or 8 GB. Sign Henge with the increased memory limit entitlement if your certificate has it.

Then open the Install tab, pick macOS Sequoia 15.6.1, and keep Henge open with the device plugged in. Tahoe 26.6.2 and macOS 27.2 are offered too, but they aren't tested yet.

## What you need

### Device

| | Works? |
|---|---|
| **M1, M2, M4, M5 iPads** (iPad Pro, iPad Air) | Yes. The best place to run Henge; this build is tested on an iPad Pro with M4. |
| **A17 Pro and newer** (iPhone 15 Pro and later, iPad mini 7) | Yes. Upstream is tested on an iPhone 17 Pro. |
| **A14, A15, A16** (iPhone 12–14 and 15, iPad Air 4 and 5, iPad 10th and 11th generation, iPad mini 6) | Supported, but slower and hotter than an M chip, and most of these have only 4 or 6 GB of memory, which is often too little (see [Memory](#memory)). |
| **A13 and older** (iPhone 11, iPad 9th generation and older) | No. The Mac's processor needs instructions these chips don't have, so Henge refuses to start a Mac. |
| **Android** | No. Henge's emulator is written for iOS and Apple's chips, and there is no Android version. |

### iOS or iPadOS version

| | Works? |
|---|---|
| **16 and older** | No. Henge needs 17 or later. |
| **17** | Installs, but the Mac's screen may stay black, with only the pointer showing and sound still working: iPadOS 17's Metal can't read the graphics code that macOS 15 and later send. A fix that converts that code is in testing. If the screen stays black, update to iPadOS 18 or later. |
| **18 to 26** | Yes. |
| **27** | Yes. Apple's newer security means only an attached debugger can turn on JIT, so use StikDebug or the built-in StikJIT helper. |

### JIT

Pick a JIT method in Settings, or leave it on **Automatic**.

- **TrollStore** — install `Henge.tipa`. Needs a version TrollStore supports (up to iOS 17.0).
- **Jailbreak** (for example Dopamine) — TrollStore Lite, or the jailbreak on its own.
- **StikDebug** — a separate app that attaches to Henge and turns JIT on. Works on any supported version.
- **Built-in StikJIT** — Henge turns on its own JIT. Needs iOS or iPadOS 26 or later and the LocalDevVPN app. On 27 Henge can pair with the device itself; on 26 it needs a pairing file made on a computer. The first time, it downloads Apple's Developer Disk Image, so it needs a working internet connection.

### Memory

Henge suggests a size for each new Mac from the device's memory: a 4 GB Mac on devices with up to 8 GB, 6 GB on 12 GB devices and 8 GB on 16 GB devices. You can choose from 1 to 16 GB, but the device has to hold the Mac's memory, Henge's JIT and iPadOS at once.

- **4 GB devices** (iPhone 12 and 13, iPhone SE 3, iPad Air 4, iPad 10th generation): iOS lets an app use only about 2.8 GB of them, so installing macOS usually stops partway with a crash. These aren't expected to work.
- **6 GB devices** (iPhone 13 Pro and 14, A16 iPad): use a 3 or 4 GB Mac, and close other apps first.
- **8 GB and more** (every M-chip iPad, iPhone 15 Pro and later): the suggested size is fine.

Sign with a certificate that keeps the *increased memory limit* entitlement, or iOS stops Henge much sooner.

### Free Apple developer account

A free account can sideload Henge with SideStore or AltStore, but:

- the app expires after 7 days and has to be refreshed;
- the *extended virtual addressing* entitlement is left out, and the *increased memory limit* one only takes effect once it is turned on for Henge's App ID. Community tools such as GetMoreRam can do that for a free account.

A paid account, TrollStore, or a jailbreak avoids both.

### If a Mac won't install or boot

- **It crashes during the install** — usually memory. See [Memory](#memory) and pick a smaller Mac.
- **"Failed to download Developer Disk Image"** — the built-in StikJIT couldn't fetch it. Check the internet connection (and that LocalDevVPN is on), tap **Reset Developer Disk Image**, then **Check setup** again. Or use StikDebug instead.
- **macOS says it's online but websites don't load** — fixed in this build: Henge now answers the Mac's DNS lookups itself, and falls back to public DNS servers when the network's own don't answer. On an older build, set the Mac's DNS server to `1.1.1.1` in System Settings › Network.
- **The screen stays black, but the pointer moves** — see iOS version 17 above.
- **Anything else** — in Henge's Settings, tap **Share henge.log** and attach it when you report the problem. It records what the Mac was doing when it stopped.

## Sharing with the Mac

Do this once for each Mac: open Terminal on the Mac, then in Henge open Details and tap **Type the Set Up Command**. Press Return on the Mac, and choose Allow when macOS asks about `henge-agent` and local networks. It needs the Mac's keyboard layout to be U.S.

Then a Henge folder in the Mac's home folder is the same folder as Files › Henge › Shared. What you copy on the Mac is copied on the device too, and the paste button pastes from the device into the Mac.

## Known limits

- Signing in to an Apple Account inside macOS fails with a verification error.
- Graphics are tested on macOS's own apps, Chrome, Firefox, VS Code and Discord. Other apps may draw wrongly.
- For the first few minutes after you log in, macOS is busy starting up in the background, so apps open more slowly.
- Only one Mac can start per launch. Quit Henge and open it again to start another, and to start one again after restarting or shutting down macOS.
- iPadOS keeps some keyboard shortcuts for itself, such as Command-Tab.
- It's emulation, so it runs slower than a real Mac.

## Screenshots

<table>
  <tr>
    <td align="center" colspan="3"><img src="Screenshots/ipad.jpg" alt="Discord on an iPad"><br><sub>iPad</sub></td>
  </tr>
  <tr>
    <td align="center" width="33%"><img src="Screenshots/macs.png" alt="Macs"><br><sub>Macs</sub></td>
    <td align="center" width="33%"><img src="Screenshots/trackpad.png" alt="Trackpad"><br><sub>Trackpad</sub></td>
    <td align="center" width="33%"><img src="Screenshots/install.png" alt="Install macOS"><br><sub>Install macOS</sub></td>
  </tr>
</table>

<sub>Captured on a Mac: the app's own screens with sample data, at iPad and iPhone sizes, showing a frame from Henge's renderer.</sub>

## Building

To use Henge, install the [latest release](../../releases). `main` can be unfinished between releases.

On a Mac with Xcode 27 or later and `xcodegen`. Henge takes StikJIT, on-device pairing and glib from a built [Husk](https://github.com/Leviidev/Husk) checkout at `~/Documents/Coding/Husk` (set `HUSK` to use another path):

```bash
ios/scripts/fetch_deps.sh
tools/build_ios_restore.sh
make ios-lib
ios/scripts/package_ipa.sh
```

That produces `~/Desktop/Henge.ipa`, and `package_ipa.sh --trollstore` writes `Henge.tipa`.

`make` builds the engine for the Mac, `build/hengevm`, and `make test` runs its tests. `make test-gpu` runs the ones that drive the Mac's GPU; run them on a Mac that isn't busy. `make test-cpu` runs the tests without the GPU (with `HV_NO_GPU=1 HV_PVG_NO_METAL=1`).

### Build, sign and install with your own Apple account

Nothing in this repository is signed with anyone's account: you sign your build with yours. The scripts in
[`tools/ios/`](tools/ios) do it from one settings file that stays on your Mac.

1. Copy [`signing.local.env.example`](signing.local.env.example) to `signing.local.env` and fill it in: your Team ID,
   your signing certificate, a bundle id and the provisioning profiles for the app and its JIT helper, and your
   device's UDID. `signing.local.env` is in `.gitignore` and is never committed.
2. Optional, once: `tools/ios/make-tools-identity.sh` makes a self-signed identity, with no name or Apple account in
   it, for the Mac helper tools (`henge-agent` and *Install Henge Tools*). Signed with the same identity every time,
   the helper keeps its Accessibility permission on the Mac across updates. Without it the tools are signed ad hoc and
   each new build has to be allowed again. Keep the `.signing/` folder it makes; it is in `.gitignore`.
3. `tools/ios/build.sh` builds everything (Husk, StikJIT, the restore stack, the engine, the tools and the app) into
   `build/ios/Henge.app`. It needs Xcode, Homebrew and rustup.
4. `tools/ios/sign.sh` signs it into `build/ios/signed/Henge.app` and installs it on your device over USB
   (`--no-install` to only sign). Installing replaces the running app, so a Mac running in Henge stops.

The entitlements are Henge's own ([`ios/Henge/Henge.entitlements`](ios/Henge/Henge.entitlements)), cut down to what
your profile allows; the script says which it left out. With a free (personal) team, set
`HENGE_BASE_ENTITLEMENTS=ios/Henge/Henge.personal.entitlements`. To get the *increased memory limit* and *extended
virtual addressing* entitlements, turn them on for your App ID in your developer account before making the profile.

### Development builds on GitHub

Every push to `main`, and every pull request, runs [`.github/workflows/dev-build.yml`](.github/workflows/dev-build.yml) on GitHub's macOS runners. It builds and tests the engine for the Mac, then builds `Henge.ipa` and `Henge.tipa` with [`ci/build-ipa.sh`](ci/build-ipa.sh), which checks out and builds Husk and StikJIT itself. The two files can be downloaded from the run's page under *Artifacts* for 14 days. They aren't published as releases. The IPA is ad-hoc signed, so the sideloader re-signs it on install. You can also start a run by hand from the Actions tab.

## Licence

GPL-2.0-or-later. Henge compiles in usbmuxd (GPLv2 or GPLv3) and code from Husk (GPL-2.0-or-later), so the shipped binary is a combined GPL work. It cannot go on the App Store. See [docs/licensing.md](docs/licensing.md).

macOS and the Mac's boot ROM are Apple's and aren't part of Henge. Henge downloads both from Apple when you install.
