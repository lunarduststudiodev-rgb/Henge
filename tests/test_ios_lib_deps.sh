#!/bin/bash
# The iOS library is checked again (tools/check_engine_isa.sh and the rest of its recipe) when anything a check
# reads changes, not only its objects: each tools/ script its recipe runs, check_mmu_calls.py (check_signal_safe.py
# imports it) and ios/Henge/Info.plist (check_engine_isa.sh reads the A12 floor from it). Read from make's own view
# of the rule (make -p), so nothing is built here.
cd "${1:-$(dirname "$0")/..}" || exit 2
unset MAKEFLAGS MFLAGS MAKELEVEL # when make test runs this, the outer make's flags (-s, -j) aren't this one's
db=$(make -pnq build/ios/libhengevm.a 2>/dev/null)
deps=$(grep -m1 -E '^build/ios/libhengevm\.a:' <<< "$db" | cut -d: -f2- | tr ' ' '\n' | sed '/^$/d')
# The recipe: the lines after the rule, up to the next blank line
recipe=$(awk '/^build\/ios\/libhengevm\.a:/{on=1; next} on && /^$/{exit} on' <<< "$db")
fail=0
[ -n "$deps" ] || { echo "FAIL: no rule for build/ios/libhengevm.a"; fail=1; }
tools=$(grep -oE 'tools/[A-Za-z0-9_]+\.(sh|py)' <<< "$recipe" | sort -u)
[ -n "$tools" ] || { echo "FAIL: the library's recipe runs no checks"; fail=1; }
for f in $(printf "%s\n" $tools tools/check_mmu_calls.py ios/Henge/Info.plist | sort -u); do
    [ -e "$f" ] || { echo "FAIL: $f is missing"; fail=1; }
    grep -qx "$f" <<< "$deps" || { echo "FAIL: changing $f doesn't check build/ios/libhengevm.a again"; fail=1; }
done
# The premise: check_engine_isa.sh does read the plist
grep -q 'ios/Henge/Info.plist' tools/check_engine_isa.sh || { echo "FAIL: check_engine_isa.sh no longer reads Info.plist (update this test)"; fail=1; }

[ "$fail" -eq 0 ] && echo "test_ios_lib_deps ok" || echo "test_ios_lib_deps FAILED"
exit $fail
