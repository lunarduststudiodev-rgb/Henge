#!/bin/zsh
# Builds the installer's libraries for iOS arm64 into build/ios/libhengerestore.a:
# libplist, libimobiledevice-glue, libusbmuxd, libimobiledevice, libtatsu,
# libirecovery, usbmuxd, idevicerestore (all third_party, vendored sources),
# mbedtls (only so libimobiledevice compiles: a restore never opens TLS),
# libzip (cmake), and HengeVM's libusb stand-in, curl stand-in and install
# driver. The Mac build (make) uses Homebrew's copies of the same libraries.
set -e
# "tools/build_ios_restore.sh mac" builds the very same sources for the Mac into
# build/mac-restore, so tests on the Mac run the restore code iOS ships.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
if [ "$1" = mac ]; then
    BUILD="$ROOT/build/mac-restore"
    SDKNAME=macosx
    CC=(xcrun --sdk macosx clang -arch arm64 -mmacosx-version-min=14.0)
    CMAKE_TARGET=(-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0)
else
    BUILD="$ROOT/build/ios"
    SDKNAME=iphoneos
    CC=(xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0)
    CMAKE_TARGET=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0)
fi
OUT="$BUILD/restore"
DEPS="$BUILD/deps"
SDK="$(xcrun --sdk $SDKNAME --show-sdk-path)"
CC+=(-isysroot "$SDK")
mkdir -p "$OUT"
COMMON=(-O2 -w -DHAVE_CONFIG_H -I"$TP/restore_config" -I"$TP" -I"$TP/libusb-1.0" -I"$TP/curl_include"
        -I"$TP/libplist/include" -I"$TP/libimobiledevice-glue/include" -I"$TP/libusbmuxd/include"
        -I"$TP/libimobiledevice/include" -I"$TP/libimobiledevice" -I"$TP/libtatsu/include" -I"$TP/libirecovery/include"
        -I"$TP/libzip/lib" -I"$DEPS/libzip" -I"$TP/mbedtls/include")
OBJS=()

# The newest header in a source's folder and in its library's include folder: an object older than that is
# built again too (objects don't record which headers they read, and a changed struct or prototype in a
# header, like img4.h's img4_envelope, must not leave stale objects linked against the new ones).
typeset -A NEWEST_H
newest_header() { # source file -> $REPLY
    local d=${1:h}
    if [ -z "${NEWEST_H[$d]+x}" ]; then
        local hs=("$d"/*.h(N.om) "${d:h}"/include/*.h(N.om) "${d:h}"/include/*/*.h(N.om))
        NEWEST_H[$d]=${${(@)hs}[1]:-}
        local h
        for h in "${hs[@]}"; do [ "$h" -nt "${NEWEST_H[$d]}" ] && NEWEST_H[$d]=$h; done
    fi
    REPLY=${NEWEST_H[$d]}
}

compile() { # name extra-flags... -- files...
    local name=$1; shift
    local flags=()
    while [ "$1" != "--" ]; do flags+=("$1"); shift; done
    shift
    mkdir -p "$OUT/$name"
    for f in "$@"; do
        local o="$OUT/$name/$(basename "${f%.*}").o"
        newest_header "$f"
        if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || { [ -n "$REPLY" ] && [ "$REPLY" -nt "$o" ]; }; then
            "${CC[@]}" "${COMMON[@]}" -I"${f:h}" "${flags[@]}" -c "$f" -o "$o" || { echo "FAILED: $f" >&2; return 1; }
        fi
        OBJS+=("$o")
    done
}

# libzip: cmake makes its config headers and the library.
if [ ! -f "$DEPS/libzip/lib/libzip.a" ]; then
    cmake -S "$TP/libzip" -B "$DEPS/libzip" "${CMAKE_TARGET[@]}" -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
        -DZLIB_INCLUDE_DIR="$SDK/usr/include" -DZLIB_LIBRARY="$SDK/usr/lib/libz.tbd" -DENABLE_COMMONCRYPTO=OFF \
        -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF -DENABLE_WINDOWS_CRYPTO=OFF -DENABLE_BZIP2=OFF \
        -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF \
        -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF -DLIBZIP_DO_INSTALL=OFF >/dev/null
    cmake --build "$DEPS/libzip" -j4 >/dev/null
fi

compile mbedtls -I"$TP/mbedtls/library" -- "$TP"/mbedtls/library/*.c
compile plist -I"$TP/libplist/libcnary/include" -DPACKAGE_VERSION='"2.8.0-henge"' -- \
    "$TP"/libplist/src/*.c "$TP"/libplist/libcnary/*.c
compile glue -DPACKAGE_VERSION='"1.3.3-henge"' -- "$TP"/libimobiledevice-glue/src/*.c
compile usbmuxdc -DPACKAGE_VERSION='"2.1.1-henge"' -DPACKAGE_STRING='"libusbmuxd"' -- "$TP"/libusbmuxd/src/*.c
compile imd -DHAVE_MBEDTLS -DPACKAGE_VERSION='"1.4.0-henge"' -DPACKAGE_STRING='"libimobiledevice"' \
    -I"$TP/libimobiledevice/src" -I"$TP/libimobiledevice/common" -- \
    "$TP"/libimobiledevice/src/*.c "$TP"/libimobiledevice/common/*.c
compile tatsu -DPACKAGE_VERSION='"1.0.5-henge"' -- "$TP"/libtatsu/src/*.c
compile irecv -DPACKAGE_VERSION='"1.3.1-henge"' -- "$TP"/libirecovery/src/libirecovery.c
compile muxd -DPACKAGE_NAME='"usbmuxd"' -DPACKAGE_VERSION='"1.1.1-henge"' -Dmain=usbmuxd_main -Dlog_level=usbmuxd_log_level -- \
    "$TP"/usbmuxd/src/*.c
compile idr -I"$TP/idevicerestore/src" -DIDEVICERESTORE_NOMAIN -DPACKAGE_NAME='"idevicerestore"' -DPACKAGE_VERSION='"1.0.0-henge"' -- \
    "$TP"/idevicerestore/src/*.c
compile henge -std=c11 -I"$ROOT/core" -I"$ROOT/host" -I"$ROOT/devices" -- \
    "$ROOT/host/libusb_shim.c" "$ROOT/host/install.c"
compile hengeobjc -fobjc-arc -- "$ROOT/host/curl_shim.m"

rm -f "$BUILD/libhengerestore.a"
xcrun --sdk $SDKNAME libtool -static -no_warning_for_no_symbols -o "$BUILD/libhengerestore.a" \
    "${OBJS[@]}" "$DEPS/libzip/lib/libzip.a" 2>&1 | grep -v 'same member name' || true
ls -la "$BUILD/libhengerestore.a"
