#!/usr/bin/env bash
# Builds Henge's unsigned, ad-hoc-signed IPA (and the TrollStore .tipa) on a clean macOS/Xcode machine:
# GitHub Actions (.github/workflows/dev-build.yml) and Bitrise (ci/build-bitrise.sh) both run this.
# SideStore, AltStore or another sideloader re-signs the IPA with the user's own certificate on install.
#
# Usage: ci/build-ipa.sh <work folder>
# Husk (glib and the on-device pairing library, built for iOS) and StikJIT are checked out and built in the
# work folder. Either is skipped when the work folder already holds it built at the pinned revision, so a CI
# cache of the work folder saves the long part. libslirp is Henge's own (third_party/libslirp), which make
# ios-lib builds against third_party/glib-ios, the headers of Husk's glib; fetch_deps.sh stops the build if
# they don't match what Husk built.
# Writes ~/Desktop/Henge.ipa and ~/Desktop/Henge.tipa.
set -euo pipefail
export HOMEBREW_NO_AUTO_UPDATE=1

HENGE_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${1:?usage: ci/build-ipa.sh <work folder>}"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
HUSK_ROOT="$WORK/Husk"
STIKJIT_ROOT="$WORK/StikJIT"
HUSK_REV=afe83d45b31194c4cc9f7b3db7ec2c757db8346e

missing=()
for tool in meson ninja pkg-config xcodegen cmake; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if ((${#missing[@]})); then
    brew install "${missing[@]}"
fi

export HUSK="$HUSK_ROOT"
IOS_DEPS="$HUSK_ROOT/build/ios-arm64/sysroot"
husk_built() {
    [ "$(git -C "$HUSK_ROOT" rev-parse HEAD 2>/dev/null)" = "$HUSK_REV" ] || return 1
    test -s "$HUSK_ROOT/build/ios-arm64/lib/libhusk_rppairing.a" || return 1
    for dep in libglib-2.0.a libintl.a libpcre2-8.a pkgconfig/glib-2.0.pc; do test -s "$IOS_DEPS/lib/$dep" || return 1; done
}
if husk_built; then
    echo "==> Husk $HUSK_REV: already built"
else
    if ! command -v rustup >/dev/null 2>&1; then
        brew install rustup
        export PATH="$(brew --prefix rustup)/bin:$PATH"
    fi
    export PATH="$HOME/.cargo/bin:$PATH"
    rustup default stable
    rustup target add aarch64-apple-ios
    if [ "$(git -C "$HUSK_ROOT" rev-parse HEAD 2>/dev/null)" != "$HUSK_REV" ]; then
        rm -rf "$HUSK_ROOT"
        git clone --depth 1 https://github.com/Leviidev/Husk.git "$HUSK_ROOT"
        git -C "$HUSK_ROOT" fetch --depth 1 origin "$HUSK_REV"
        git -C "$HUSK_ROOT" checkout --detach "$HUSK_REV"
    fi
    (
        cd "$HUSK_ROOT"
        ./scripts/fetch_sources.sh
        # Only what Henge links: libffi (for glib) and glib (with pcre2 and intl).
        ./scripts/build_ios.sh libffi glib
        ./scripts/build_rppairing_ios.sh
    )
    husk_built || {
        echo "Husk's iOS libraries are missing after the build" >&2
        exit 1
    }
fi

cd "$HENGE_ROOT"
./ios/scripts/fetch_deps.sh
# StikJIT 1.9.0 with full-width JIT addresses (ci/build-stikjit.sh), in place of the one Husk vendors.
if [ ! -d "$STIKJIT_ROOT/StikJIT.xcframework" ]; then
    rm -rf "$STIKJIT_ROOT"
    bash ci/build-stikjit.sh "$STIKJIT_ROOT"
fi
rm -rf "$HENGE_ROOT/ios/deps/StikJIT.xcframework"
ditto "$STIKJIT_ROOT/StikJIT.xcframework" "$HENGE_ROOT/ios/deps/StikJIT.xcframework"
./tools/build_ios_restore.sh
make ios-lib
mkdir -p "$HOME/Desktop"
./ios/scripts/package_ipa.sh
./ios/scripts/package_ipa.sh --trollstore
