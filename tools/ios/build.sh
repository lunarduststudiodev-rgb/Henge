#!/usr/bin/env bash
# Builds Henge for iPhone and iPad on this Mac, the way ci/build-ipa.sh does: Husk's iOS libraries, the patched
# StikJIT, the restore stack, the engine, the Mac helper tools and the app (unsigned). The result is
# $HENGE_WORK/Henge.app (default build/ios/Henge.app), ready for tools/ios/sign.sh.
# Needs Xcode, Homebrew (meson, xcodegen) and rustup. Settings come from signing.local.env (see the .example).
set -euo pipefail
. "$(dirname "$0")/env.sh"
export HOMEBREW_NO_AUTO_UPDATE=1
WORK="$HENGE_WORK"
HUSK_ROOT="$WORK/Husk"
STIKJIT_ROOT="$WORK/StikJIT"
HUSK_REV=$(cat "$ROOT/ci/build-ipa.sh" "$ROOT/ci/build-bitrise.sh" 2>/dev/null | sed -n 's/^HUSK_REV=//p' | head -1)
[ -n "$HUSK_REV" ] || { echo "no HUSK_REV in ci/build-ipa.sh" >&2; exit 1; }
mkdir -p "$WORK"
step() { echo; echo "==== [$(date +%T)] $*"; }

step "tools"
command -v meson >/dev/null || brew install meson
command -v xcodegen >/dev/null || brew install xcodegen
export PATH="/opt/homebrew/opt/rustup/bin:$HOME/.cargo/bin:$PATH"
rustup show active-toolchain >/dev/null 2>&1 || rustup default stable
rustup target add aarch64-apple-ios

step "Husk $HUSK_REV"
[ -d "$HUSK_ROOT/.git" ] || git clone --depth 1 https://github.com/Leviidev/Husk.git "$HUSK_ROOT"
git -C "$HUSK_ROOT" fetch --depth 1 origin "$HUSK_REV"
git -C "$HUSK_ROOT" checkout --detach "$HUSK_REV"
cd "$HUSK_ROOT"
if [ ! -s build/ios-arm64/sysroot/lib/libslirp.a ]; then
    ./scripts/fetch_sources.sh
    ./scripts/build_ios.sh libffi glib libslirp
fi
[ -s build/ios-arm64/lib/libhusk_rppairing.a ] || ./scripts/build_rppairing_ios.sh

step "deps into ios/deps"
cd "$ROOT"
export HUSK="$HUSK_ROOT"
export IOS_DEPS="$HUSK_ROOT/build/ios-arm64/sysroot"
./ios/scripts/fetch_deps.sh

step "StikJIT (high-address patch)"
[ -d "$STIKJIT_ROOT/StikJIT.xcframework" ] || { rm -rf "$STIKJIT_ROOT"; bash ci/build-stikjit.sh "$STIKJIT_ROOT"; }
rm -rf ios/deps/StikJIT.xcframework
ditto "$STIKJIT_ROOT/StikJIT.xcframework" ios/deps/StikJIT.xcframework

step "restore stack + engine"
./tools/build_ios_restore.sh
make ios-lib

step "Mac helper tools"
KC="$HENGE_TOOLS_DIR/henge-tools.keychain-db"
if [ -f "$KC" ]; then
    security unlock-keychain -p "$(cat "$HENGE_TOOLS_DIR/henge-tools.password")" "$KC"
    SIGN=("AGENT_SIGN=Henge Tools (self-signed)" "AGENT_KEYCHAIN=$KC")
else
    echo "no $KC: the tools are signed ad hoc (tools/ios/make-tools-identity.sh makes a lasting identity)"
    SIGN=("AGENT_SIGN=-")
fi
rm -f build/henge-agent build/henge-agent.gz build/henge-tools.img.gz
make build/henge-agent.gz build/henge-tools.img.gz "${SIGN[@]}"
codesign -dv build/henge-agent 2>&1 | grep -E "^Identifier|^Authority|^Signature"

step "app"
cd ios
xcodegen generate >/dev/null
DD="$WORK/DerivedData"
BID=(); [ -n "${HENGE_BUNDLE_ID:-}" ] && BID=("HENGE_BUNDLE_IDENTIFIER=$HENGE_BUNDLE_ID")
xcodebuild -project Henge.xcodeproj -scheme Henge -configuration Release -sdk iphoneos \
    -destination 'generic/platform=iOS' -derivedDataPath "$DD" ${BID[@]+"${BID[@]}"} \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" build > "$WORK/app-build.log" 2>&1 || {
    grep -E "error:|BUILD" "$WORK/app-build.log" | sort -u | head -40
    echo "APP BUILD FAILED (log: $WORK/app-build.log)"; exit 1; }
rm -rf "$WORK/Henge.app"
ditto "$DD/Build/Products/Release-iphoneos/Henge.app" "$WORK/Henge.app"
rm -f "$WORK/Henge.app/AVPBooter.vmapple2.bin" "$WORK/Henge.app/henge-tools.tar.gz"
plutil -insert HengeBuildCommit -string "$(git -C "$ROOT" rev-parse --short=12 HEAD)" "$WORK/Henge.app/Info.plist" 2>/dev/null || true
step "BUILD DONE: $WORK/Henge.app (next: tools/ios/sign.sh)"
