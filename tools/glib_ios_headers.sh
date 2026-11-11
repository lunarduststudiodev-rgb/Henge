#!/bin/bash
# third_party/glib-ios: the public headers of the glib the iPad app links (glib 2.83.0, built by Husk's
# scripts/build_ios.sh for arm64 iOS), which the Makefile compiles libslirp against.
#
#   tools/glib_ios_headers.sh                regenerate them: download glib, configure it for arm64 iOS with
#                                            Husk's cross file and options, copy what `meson install` would
#                                            put in include/glib-2.0 and lib/glib-2.0/include for glib itself
#   tools/glib_ios_headers.sh --check DIR    fail unless they are the same as those of the glib installed in
#                                            DIR (Husk's build/ios-arm64/sysroot); ci/build-ipa.sh runs it
#
# When Husk moves to another glib, change GLIB_VERSION and GLIB_SHA256 here and run it.
set -euo pipefail
GLIB_VERSION=2.83.0
GLIB_SHA256=a07d9e1a57a4279c5ece71c26dc44eea12bd518ea9ff695d53e722997032b614
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/third_party/glib-ios"

if [ "${1:-}" = "--check" ]; then
    SYS="${2:?usage: tools/glib_ios_headers.sh --check <sysroot>}"
    have="$(PKG_CONFIG_LIBDIR="$SYS/lib/pkgconfig" pkg-config --modversion glib-2.0 2>/dev/null || true)"
    if [ "$have" != "$GLIB_VERSION" ]; then
        echo "error: the iPad links glib '$have' from $SYS, but third_party/glib-ios holds glib $GLIB_VERSION's headers." >&2
        echo "       Regenerate them for that version (tools/glib_ios_headers.sh) so libslirp compiles against the glib it links." >&2
        exit 1
    fi
    bad=0
    while IFS= read -r f; do
        rel="${f#"$OUT"/}"
        if ! cmp -s "$f" "$SYS/$rel"; then
            echo "error: $rel differs from the one Husk installed in $SYS" >&2
            bad=1
        fi
    done < <(find "$OUT/include" "$OUT/lib" -type f)
    [ $bad = 0 ] || { echo "       Regenerate third_party/glib-ios (tools/glib_ios_headers.sh)." >&2; exit 1; }
    echo "third_party/glib-ios matches the glib $GLIB_VERSION in $SYS"
    exit 0
fi

command -v meson >/dev/null && command -v ninja >/dev/null || { echo "needs meson and ninja (brew install meson ninja)" >&2; exit 1; }
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
curl -fL --retry 3 -o "$WORK/glib.tar.xz" "https://download.gnome.org/sources/glib/${GLIB_VERSION%.*}/glib-$GLIB_VERSION.tar.xz"
echo "$GLIB_SHA256  $WORK/glib.tar.xz" | shasum -a 256 -c -
tar -xf "$WORK/glib.tar.xz" -C "$WORK"
SRC="$WORK/glib-$GLIB_VERSION"

# Husk's scripts/build_ios.sh: its toolchain, cross file and glib options
ARCH=arm64 SDK=iphoneos SDKMINVER=15.5
SDKROOT="$(xcrun --sdk $SDK --show-sdk-path)"
T="-target $ARCH-apple-ios$SDKMINVER"
PREFIX="$WORK/sysroot"
CFLAGS="-arch $ARCH -isysroot $SDKROOT -I$PREFIX/include $T -O2"
LDFLAGS="-arch $ARCH -isysroot $SDKROOT -L$PREFIX/lib $T"
q() { echo "'$(echo $* | sed "s/ /','/g")'"; }
cat > "$WORK/cross-ios.meson" <<EOF
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = [$(q $CFLAGS)]
cpp_args = [$(q $CFLAGS)]
objc_args = [$(q $CFLAGS)]
c_link_args = [$(q $LDFLAGS)]
cpp_link_args = [$(q $LDFLAGS)]
objc_link_args = [$(q $LDFLAGS)]
[binaries]
c = [$(q "$(xcrun --sdk $SDK --find clang)" $T)]
cpp = [$(q "$(xcrun --sdk $SDK --find clang++)" $T)]
objc = ['$(xcrun --sdk $SDK --find clang)']
ar = ['$(xcrun --sdk $SDK --find ar)']
nm = ['$(xcrun --sdk $SDK --find nm)']
ranlib = ['$(xcrun --sdk $SDK --find ranlib)']
strip = ['$(xcrun --sdk $SDK --find strip)', '-x']
pkg-config = ['$(command -v pkg-config)']
python = ['$(command -v python3)']
[host_machine]
system = 'ios'
kernel = 'xnu'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
unset PKG_CONFIG_PATH
meson setup "$WORK/build" "$SRC" --cross-file "$WORK/cross-ios.meson" --prefix="$PREFIX" \
    --buildtype=release --default-library=static \
    -Dtests=false -Dnls=disabled -Dintrospection=disabled -Dselinux=disabled -Dlibmount=disabled \
    -Ddtrace=disabled -Dman-pages=disabled -Dglib_debug=disabled -Dxattr=false >"$WORK/setup.log" ||
    { cat "$WORK/setup.log" >&2; exit 1; }
ninja -C "$WORK/build" glib/glib-visibility.h glib/gversionmacros.h >/dev/null

# What `meson install` puts in include/glib-2.0 and lib/glib-2.0/include for glib itself (not gio, gobject,
# gmodule or girepository, which libslirp doesn't use)
rm -rf "$OUT/include" "$OUT/lib"
meson introspect "$WORK/build" --installed | python3 -I -c '
import json, sys
for src, dst in json.load(sys.stdin).items():
    rel = dst.split("/sysroot/", 1)[1]
    name = rel.split("/")[2] if rel.startswith("include/glib-2.0/") else ""
    if rel.startswith("lib/glib-2.0/include/") or (name and not name.startswith(("gio", "gobject", "gmodule", "girepository"))):
        print(src + "\t" + rel)
' | while IFS=$'\t' read -r src rel; do
    mkdir -p "$OUT/$(dirname "$rel")"
    cp "$src" "$OUT/$rel"
done
cp "$SRC/COPYING" "$OUT/COPYING"
echo "third_party/glib-ios: glib $GLIB_VERSION's headers for arm64 iOS ($(find "$OUT/include" "$OUT/lib" -type f | wc -l | tr -d ' ') files)"
