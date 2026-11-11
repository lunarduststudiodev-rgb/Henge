#!/bin/zsh
# Copies the prebuilt iOS pieces Henge links into ios/deps (not in git):
#   StikJIT.xcframework   Built-in StikJIT (MPL-2.0), as Husk vendors it
#   libhusk_rppairing.a   on-device pairing (Husk's src/rppairing-ios, scripts/build_rppairing_ios.sh)
#   glib, intl, pcre2 (arm64 iOS static libraries, Husk's scripts/build_ios.sh)
# libslirp is built from third_party/libslirp against third_party/glib-ios, the headers of this same glib;
# the copy stops if Husk's glib isn't the one those headers are for.
set -e
HUSK=${HUSK:-$HOME/Documents/Coding/Husk}
SYSROOT="$HUSK/build/ios-arm64/sysroot"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="$ROOT/ios/deps"
if [ ! -d "$SYSROOT/lib" ]; then
    echo "error: no built Husk at $HUSK (set HUSK to its checkout; Husk's scripts/build_ios.sh libffi glib builds it)" >&2
    exit 1
fi
"$ROOT/tools/glib_ios_headers.sh" --check "$SYSROOT"
mkdir -p "$DEPS/lib"
rm -rf "$DEPS/StikJIT.xcframework"
ditto "$HUSK/src/app/Frameworks/StikJIT.xcframework" "$DEPS/StikJIT.xcframework"
cp "$HUSK/build/ios-arm64/lib/libhusk_rppairing.a" "$DEPS/lib/"
for l in libglib-2.0 libintl libpcre2-8; do cp "$SYSROOT/lib/$l.a" "$DEPS/lib/"; done
# What earlier versions copied for Husk's libslirp, which the app no longer links
rm -f "$DEPS/lib/libslirp.a"
rm -rf "$DEPS/include/slirp"
echo "deps in $DEPS"
