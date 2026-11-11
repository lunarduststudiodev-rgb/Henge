#!/bin/bash
# The tests that are scripts run through a small wrapper the Makefile writes into the build directory. Each such
# wrapper whose rule needs nothing but scripts builds into a build directory that doesn't exist yet (the rule makes
# it), as on a fresh checkout. The wrappers are found in make's rules (make -p -n), so a new one is checked too. And
# every test in TESTS makes the build directory before anything is written into it.
cd "${1:-$(dirname "$0")/..}" || exit 2
unset MAKEFLAGS MFLAGS MAKELEVEL
d=$(mktemp -d) || exit 1
trap 'rm -rf "$d"' EXIT
fail=0 n=0
for t in $(make -p -n B=build 2>/dev/null | grep -E '^build/test_[a-z0-9_]+: ' |
           awk '{ ok = 1; for (i = 2; i <= NF; i++) if ($i != "|" && $i !~ /^(tests|tools)\// && $i != "build") ok = 0; if (ok) print $1 }' |
           sed 's|^build/||; s|:$||' | sort -u); do
    n=$((n + 1))
    rm -rf "$d/b"
    make -s B="$d/b" "$d/b/$t" > "$d/log" 2>&1 && [ -x "$d/b/$t" ] || { echo "FAIL: $t doesn't build into a new build directory:"; cat "$d/log"; fail=1; }
done
[ "$n" -ge 3 ] || { echo "FAIL: found only $n wrapper tests"; fail=1; }

# Every test, wrapper or not: as make would build it into a build directory that doesn't exist yet (make -n), the
# first command that names the directory makes it, not one that writes into it (a compiler's -o fails there). A rule
# that names it first harmlessly (an rm -f, a test -d) fails this too: put the mkdir before it.
printf 'print-tests:\n\t@echo $(TESTS)\n' > "$d/print.mk"
all=$(make -s -f Makefile -f "$d/print.mk" B=build print-tests)
m=0
for t in $all; do
    t=${t#build/}
    m=$((m + 1))
    rm -rf "$d/b"
    first=$(make -n B="$d/b" "$d/b/$t" 2>&1 | grep -m1 -F "$d/b")
    case "$first" in
    mkdir*) ;;
    *) echo "FAIL: $t writes into the build directory before it is made: $first"; fail=1 ;;
    esac
done
[ "$m" -ge 50 ] || { echo "FAIL: found only $m tests"; fail=1; }
[ "$fail" -eq 0 ] && echo "test_wrapper_dirs ok ($n wrappers built; $m tests make the directory first)" || echo "test_wrapper_dirs FAILED"
exit $fail
