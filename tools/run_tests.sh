#!/bin/bash
# Runs every test it is given, one at a time, whatever the ones before did: a failing test doesn't hide those after
# it (make test, make test-cpu / test-shipped). A test that exits 77 skipped itself (no FEAT_AFP, say). Ends with
# every test's result and exits 1 if any failed.
#
# Usage: tools/run_tests.sh test...
ulimit -s 65520 # a test keeps a whole HvCpu (2 MB of TLB and more) on its stack
results=() failed=0
for t in "$@"; do
    echo "== $t"
    "$t"
    rc=$?
    case $rc in
    0) results+=("ok      $t") ;;
    77) results+=("skipped $t") ;;
    *) results+=("FAILED  $t (exit $rc)") failed=$((failed + 1)) ;;
    esac
done
echo "== results"
printf '%s\n' "${results[@]}"
echo "== $(($# - failed)) of $# passed or skipped, $failed failed"
[ "$failed" -eq 0 ]
