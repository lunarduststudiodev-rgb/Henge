#!/bin/bash
# tools/run_tests.sh runs every test even after one fails, lists each one's result, and exits non-zero if any
# failed (a skip, exit 77, isn't a failure).
run="$(dirname "$0")/../tools/run_tests.sh"
d=$(mktemp -d) || exit 1
trap 'rm -rf "$d"' EXIT
mk() { printf '#!/bin/sh\necho ran >> "%s/ran"\nexit %s\n' "$d" "$2" > "$d/$1" && chmod +x "$d/$1"; }
mk first 0; mk breaks 1; mk skips 77; mk last 0
fail=0
check() { "$@" || { echo "FAIL: $*"; fail=1; }; }

out=$("$run" "$d/first" "$d/breaks" "$d/skips" "$d/last" 2>&1); rc=$?
check [ "$rc" -ne 0 ]
check [ "$(wc -l < "$d/ran" | tr -d ' ')" = 4 ] # the tests after the failing one ran too
check grep -q "^FAILED  $d/breaks (exit 1)$" <<< "$out"
check grep -q "^skipped $d/skips$" <<< "$out"
check grep -q "^ok      $d/last$" <<< "$out"
check grep -q "^== 3 of 4 passed or skipped, 1 failed$" <<< "$out"

rm -f "$d/ran"
"$run" "$d/first" "$d/skips" "$d/last" > /dev/null 2>&1
check [ $? -eq 0 ] # skips aren't failures
[ "$fail" -eq 0 ] && echo "test_run_tests ok" || { echo "$out"; echo "test_run_tests FAILED"; }
exit $fail
