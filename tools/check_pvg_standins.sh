#!/bin/bash
# The tests that build the display device itself (they include devices/pvg_lite.c) stand in for its Metal side
# (devices/pvg_metal.m) with tests/pvg_metal_standin.h. Each hook pvg_lite.c calls needs a stand-in there, or those
# tests stop linking the day a hook is added. This lists the hooks pvg_lite.o calls and fails if the header lacks
# one, if a test builds the device without the header, or if a test that includes the header defines a hook itself
# (a second copy that would drift).
#
# Usage: tools/check_pvg_standins.sh <repo root> <pvg_lite.o>   (exit 1 and what is wrong, if anything)
set -uo pipefail
root=$1 obj=$2
header=$root/tests/pvg_metal_standin.h
hooks=$(nm -u "$obj" | sed -n 's/^_\(hv_pvg_metal_[a-z0-9_]*\)$/\1/p' | sort -u)
if [ -z "$hooks" ]; then
    echo "check_pvg_standins: no hv_pvg_metal_ hook found in $obj (nm failed, or the wrong file)" >&2
    exit 1
fi
# A definition starts a line with its return type; the hook's name is before the line's first "("
defined() { sed -nE 's/^[A-Za-z][^(]*[ *](hv_pvg_metal_[a-z0-9_]+)\(.*/\1/p' "$@" | sort -u; }
bad=0
missing=$(comm -23 <(echo "$hooks") <(defined "$header"))
if [ -n "$missing" ]; then
    echo "check_pvg_standins: pvg_lite.o calls these Metal hooks, with no stand-in in tests/pvg_metal_standin.h:" >&2
    echo "$missing" | sed 's/^/  /' >&2
    bad=1
fi
# The files of tests/ (C or Objective-C) that include or import a file by that name, by whatever path
# (../devices/pvg_lite.c, or pvg_lite.c with -Idevices), on a line of its own: not in a comment
including() { grep -lE '^[[:space:]]*#[[:space:]]*(include|import)[[:space:]]*["<]([^">]*/)?'"${1//./\\.}"'[">]' \
              "$root"/tests/*.c "$root"/tests/*.m 2>/dev/null; }
with_header=$(including pvg_metal_standin.h)
for t in $(including pvg_lite.c); do
    if ! grep -qxF "$t" <<< "$with_header"; then
        echo "check_pvg_standins: ${t#$root/} builds the display device without tests/pvg_metal_standin.h" >&2
        bad=1
    fi
done
for t in $with_header; do
    own=$(defined "$t")
    if [ -n "$own" ]; then
        echo "check_pvg_standins: ${t#$root/} includes tests/pvg_metal_standin.h and defines hooks of its own:" >&2
        echo "$own" | sed 's/^/  /' >&2
        bad=1
    fi
done
[ $bad = 0 ] && echo "check_pvg_standins: all $(echo "$hooks" | wc -l | tr -d ' ') Metal hooks pvg_lite.o calls are stood in for, once"
exit $bad
