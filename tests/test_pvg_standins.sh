#!/bin/bash
# tools/check_pvg_standins.sh passes on the tree as it is, and fails, naming it, when a hook pvg_lite.o calls has
# no stand-in, when a test builds the device without the header (included or imported, by any path, not in a
# comment), or when a test (C or Objective-C) keeps a stand-in of its own.
# Usage: tests/test_pvg_standins.sh <repo root> <pvg_lite.o>
set -uo pipefail
root=$1 obj=$2
check=$root/tools/check_pvg_standins.sh
fails=0
say() { echo "$1"; if [ "${2:-}" = fail ]; then fails=$((fails + 1)); fi; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/pvg_standins.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

out=$("$check" "$root" "$obj" 2>&1) && say "ok: the tree passes" || say "FAIL: the tree fails: $out" fail

# A copy of the tests with one hook's stand-in taken out (as when pvg_lite.c gains a hook nobody stood in for)
mkdir -p "$tmp/gone/tests"
cp "$root"/tests/*.c "$tmp/gone/tests/"
grep -v '^void hv_pvg_metal_hide_pointer(' "$root/tests/pvg_metal_standin.h" > "$tmp/gone/tests/pvg_metal_standin.h"
out=$("$check" "$tmp/gone" "$obj" 2>&1)
rc=$?
[ $rc = 1 ] && echo "$out" | grep -q '^  hv_pvg_metal_hide_pointer$' &&
    say "ok: a hook with no stand-in fails, named" || say "FAIL: a hook with no stand-in: rc $rc: $out" fail

# A test that includes the header and keeps one of its own as well
mkdir -p "$tmp/own/tests"
cp "$root"/tests/*.c "$root/tests/pvg_metal_standin.h" "$tmp/own/tests/"
printf '#include "pvg_metal_standin.h"\nint hv_pvg_metal_pointer_shown2(HvPvgMetal *m) { (void)m; return 0; }\n' > "$tmp/own/tests/test_planted.c"
out=$("$check" "$tmp/own" "$obj" 2>&1)
rc=$?
[ $rc = 1 ] && echo "$out" | grep -q 'tests/test_planted.c includes' &&
    say "ok: a test with a stand-in of its own fails, named" || say "FAIL: a test's own stand-in: rc $rc: $out" fail

# A test that builds the device with stand-ins of its own, not the header's
mkdir -p "$tmp/apart/tests"
cp "$root"/tests/*.c "$root/tests/pvg_metal_standin.h" "$tmp/apart/tests/"
printf '#include "../devices/pvg_lite.c"\n' > "$tmp/apart/tests/test_planted.c"
out=$("$check" "$tmp/apart" "$obj" 2>&1)
rc=$?
[ $rc = 1 ] && echo "$out" | grep -q 'tests/test_planted.c builds the display device without' &&
    say "ok: a test building the device without the header fails, named" || say "FAIL: a test without the header: rc $rc: $out" fail

# The same, included as pvg_lite.c (built with -Idevices)
mkdir -p "$tmp/short/tests"
cp "$root"/tests/*.c "$root/tests/pvg_metal_standin.h" "$tmp/short/tests/"
printf '#include "pvg_lite.c"\n' > "$tmp/short/tests/test_planted.c"
out=$("$check" "$tmp/short" "$obj" 2>&1)
rc=$?
[ $rc = 1 ] && echo "$out" | grep -q 'tests/test_planted.c builds the display device without' &&
    say "ok: a test including pvg_lite.c by a short path without the header fails, named" ||
    say "FAIL: a short-path include without the header: rc $rc: $out" fail

# An Objective-C test that includes the header and keeps a stand-in of its own
mkdir -p "$tmp/objc/tests"
cp "$root"/tests/*.c "$root/tests/pvg_metal_standin.h" "$tmp/objc/tests/"
printf '#include "pvg_metal_standin.h"\nint hv_pvg_metal_pointer_shown2(HvPvgMetal *m) { (void)m; return 0; }\n' > "$tmp/objc/tests/test_planted.m"
out=$("$check" "$tmp/objc" "$obj" 2>&1)
rc=$?
[ $rc = 1 ] && echo "$out" | grep -q 'tests/test_planted.m includes' &&
    say "ok: an Objective-C test with a stand-in of its own fails, named" || say "FAIL: an .m test's own stand-in: rc $rc: $out" fail

# Plants that each fail the check, and what it must say: one per line, the planted file's name, its text, the message
plant() { # <name> <file> <text> <message>
    rm -rf "$tmp/p" && mkdir -p "$tmp/p/tests" && cp "$root"/tests/*.c "$root/tests/pvg_metal_standin.h" "$tmp/p/tests/"
    printf '%b' "$3" > "$tmp/p/tests/$2"
    out=$("$check" "$tmp/p" "$obj" 2>&1)
    rc=$?
    [ $rc = 1 ] && echo "$out" | grep -qF "tests/$2 $4" && say "ok: $1 fails, named" || say "FAIL: $1: rc $rc: $out" fail
}
plant "an Objective-C test importing the device without the header" test_planted.m '#import "../devices/pvg_lite.c"\n' \
    "builds the display device without"
plant "a test whose include of the header is commented out" test_planted.c \
    '#include "../devices/pvg_lite.c"\n// #include "pvg_metal_standin.h"\n' "builds the display device without"
plant "a test including the header by a longer path, with a hook of its own" test_planted.c \
    '#include "../tests/pvg_metal_standin.h"\nint hv_pvg_metal_pointer_shown2(HvPvgMetal *m) { (void)m; return 0; }\n' \
    "includes tests/pvg_metal_standin.h and defines hooks of its own"
# and one that passes: a comment naming the device's source is no include
rm -rf "$tmp/p" && mkdir -p "$tmp/p/tests" && cp "$root"/tests/*.c "$root/tests/pvg_metal_standin.h" "$tmp/p/tests/"
printf '/* like ../devices/pvg_lite.c, but not it */\n// #include "../devices/pvg_lite.c"\n' > "$tmp/p/tests/test_planted.c"
out=$("$check" "$tmp/p" "$obj" 2>&1) && say "ok: a comment naming pvg_lite.c passes" || say "FAIL: a comment naming pvg_lite.c: $out" fail

[ $fails = 0 ] && echo "test_pvg_standins ok" || echo "test_pvg_standins FAILED"
exit $((fails != 0))
