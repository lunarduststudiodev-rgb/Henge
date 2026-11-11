#!/bin/bash
# A sanitizer build (make SAN_CFLAGS=-fsanitize=...) compiles the engine with the shipped build's flags (its chip,
# -O2) and the sanitizer added, linked with it too; mmu.o gets the chip but no sanitizer (its runtime uses SIMD: the
# Makefile's mmu.o rule). Objects a plain build made aren't reused for it.
# Read from what make would run (make -n into an empty build directory), so nothing is built here.
cd "${1:-$(dirname "$0")/..}" || exit 2
unset MAKEFLAGS MFLAGS MAKELEVEL # when make test runs this, the outer make's flags aren't this one's
d=$(mktemp -d) || exit 1
trap 'rm -rf "$d"' EXIT
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
fail=0
has() { grep -q -- "$2" <<< "$1" || { echo "FAIL: $3 lacks $2: ${1:0:160}"; fail=1; }; }
lacks() { grep -q -- "$2" <<< "$1" && { echo "FAIL: $3 has $2: ${1:0:160}"; fail=1; }; }
line() { make -n B="$d" SAN_CFLAGS="$SAN" "$d/$1" 2>/dev/null | grep -- " -c .*$1\$"; }

shipped=$(make -n B="$d" "$d/dispatch.o" 2>/dev/null | grep -- "-c ")
dispatch=$(line dispatch.o)
squeeze() { tr -s ' ' <<< "$1"; }
[ "$(squeeze "${dispatch/$SAN/}")" = "$(squeeze "$shipped")" ] || { echo "FAIL: dispatch.o's sanitizer line isn't the shipped one with the sanitizer added:"; echo "  shipped: $shipped"; echo "  san:     $dispatch"; fail=1; }
for f in -mcpu=apple-a12 -mtune=apple-a14 -O2 -fsanitize=address; do has "$dispatch" "$f" dispatch.o; done
mmu=$(line mmu.o)
for f in -mcpu=apple-a12 -mgeneral-regs-only; do has "$mmu" "$f" mmu.o; done
lacks "$mmu" -fsanitize mmu.o

link=$(make -n B="$d" SAN_CFLAGS="$SAN" "$d/test_mmu" 2>/dev/null | grep -- "-o $d/test_mmu\$")
has "$link" -fsanitize=address "test_mmu's link"

# Objects built without the sanitizer (or with another) aren't taken for a sanitizer build's: the flags stamp differs
stamp=$(make -n B="$d" "$d/dispatch.o" 2>/dev/null | sed -n "s|^touch $d/\(\.flags-[^ ]*\)$|\1|p")
if [ -z "$stamp" ]; then echo "FAIL: no flags stamp for $d/dispatch.o"; fail=1; else
    touch "$d/$stamp" "$d/dispatch.o"
    make -q B="$d" "$d/dispatch.o" 2>/dev/null || { echo "FAIL: a plain dispatch.o isn't up to date for a plain build"; fail=1; }
    make -q B="$d" SAN_CFLAGS="$SAN" "$d/dispatch.o" 2>/dev/null && { echo "FAIL: a plain dispatch.o is taken for a sanitizer build"; fail=1; }
fi

[ "$fail" -eq 0 ] && echo "test_san_flags ok" || echo "test_san_flags FAILED"
exit $fail
