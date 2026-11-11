#!/bin/sh
# Builds "Install Henge Tools.app" (tools/henge-tools-installer.m) around the helper, and puts it on the
# Henge Tools disk: build/henge-tools.img.gz, a raw read-only disk Henge attaches to the Mac until Henge Tools
# is in. Compressed in the iPad app, as tools that sign an IPA would try to sign a Mac app lying in it.
# Usage: tools/build-henge-tools.sh <signing identity, or -> <build dir>
set -eu
SIGN="$1"
# Absolute: actool compiles in a shared daemon whose working directory is whichever checkout started it, so a
# relative output folder can land the icon in another worktree's build and leave this one without it.
mkdir -p "$2"
B="$(cd "$2" && pwd)"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$B/henge-tools"
APP="$WORK/Install Henge Tools.app"
rm -rf "$WORK"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Helpers"

run() { env -u SDKROOT -u IPHONEOS_DEPLOYMENT_TARGET "$@"; }

run xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=12.0 -fobjc-arc -Wall -Wextra -Werror -O2 \
    -framework AppKit "$ROOT/tools/henge-tools-installer.m" -o "$APP/Contents/MacOS/Henge Tools"

# Henge's own icon (Assets.car for macOS 26, AppIcon.icns before it).
run xcrun actool --compile "$APP/Contents/Resources" --platform macosx --minimum-deployment-target 12.0 \
    --app-icon AppIcon --output-partial-info-plist "$WORK/icon.plist" "$ROOT/ios/Henge/AppIcon.icon" >/dev/null

VERSION=$(sed -n 's/^#define HV_AGENT_VERSION \([0-9]*\).*/\1/p' "$ROOT/host/share.h")
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>en</string>
	<key>CFBundleDisplayName</key><string>Install Henge Tools</string>
	<key>CFBundleExecutable</key><string>Henge Tools</string>
	<key>CFBundleIconFile</key><string>AppIcon</string>
	<key>CFBundleIconName</key><string>AppIcon</string>
	<key>CFBundleIdentifier</key><string>com.henge.tools-installer</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>Henge Tools</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>$VERSION</string>
	<key>CFBundleVersion</key><string>$VERSION</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.utilities</string>
	<key>LSMinimumSystemVersion</key><string>12.0</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSPrincipalClass</key><string>NSApplication</string>
	<key>NSSupportsAutomaticTermination</key><false/>
</dict>
</plist>
PLIST
plutil -lint "$APP/Contents/Info.plist" >/dev/null

# The helper as built and signed by the Makefile: the same signer every build, so macOS keeps its permission.
cp "$B/henge-agent" "$APP/Contents/Helpers/henge-agent"
chmod 755 "$APP/Contents/Helpers/henge-agent" "$APP/Contents/MacOS/Henge Tools"
codesign -f -s "$SIGN" ${AGENT_KEYCHAIN:+--keychain "$AGENT_KEYCHAIN"} --identifier com.henge.tools-installer "$APP"
codesign --verify --strict "$APP"

# The disk: Henge's icon, and a Finder window of one big icon (tools/henge-tools.DS_Store). HFS+ without a
# journal, as nothing writes to it. Made writable here, then copied out raw for Henge's disk device.
IMG="$WORK/disk"
mkdir -p "$IMG"
hdiutil create -size 16m -fs HFS+ -volname "Henge Tools" -layout GPTSPUD -o "$IMG/rw.dmg" -quiet
# diskarbitrationd now and then turns an attach away for a moment: a few tries.
MNT=
for try in 1 2 3 4 5; do
    OUT=$(hdiutil attach "$IMG/rw.dmg" -nobrowse -noverify -noautoopen -mountrandom "$IMG" 2>&1) || true
    MNT=$(printf '%s\n' "$OUT" | awk -F'\t' '/Apple_HFS/ {print $NF}')
    [ -d "$MNT" ] && break
    sleep 2
done
[ -d "$MNT" ] || { echo "build-henge-tools: could not attach the disk image:"; printf '%s\n' "$OUT" | grep -v deprecated; exit 1; }
trap 'hdiutil detach "$MNT" -force -quiet 2>/dev/null || true' EXIT
ditto --norsrc --noextattr --noqtn "$APP" "$MNT/Install Henge Tools.app"
cp "$ROOT/tools/henge-tools.DS_Store" "$MNT/.DS_Store"
cp "$APP/Contents/Resources/AppIcon.icns" "$MNT/.VolumeIcon.icns"
SetFile -a C "$MNT" # the volume has a custom icon
rm -rf "$MNT/.fseventsd" "$MNT/.Trashes"
codesign --verify --strict "$MNT/Install Henge Tools.app"
# macOS can hold a disk it has just seen for a moment (Spotlight, fseventsd): try again, and force it at last.
for try in 1 2 3 4 5 6 7 8 9 10; do
    hdiutil detach "$MNT" -quiet 2>/dev/null && break
    [ "$try" = 10 ] && hdiutil detach "$MNT" -force -quiet
    sleep 1
done
trap - EXIT
hdiutil convert "$IMG/rw.dmg" -format UDTO -o "$IMG/raw" -quiet
gzip -9 -n -c "$IMG/raw.cdr" > "$B/henge-tools.img.gz.tmp"
mv "$B/henge-tools.img.gz.tmp" "$B/henge-tools.img.gz"
rm -rf "$IMG"
