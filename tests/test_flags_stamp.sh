#!/bin/bash
# Every object the Mac build compiles with CFLAGS depends on the flags stamp (the Makefile's FLAGS_STAMP), so a
# change of chip, page size, configuration or sanitizer rebuilds it rather than mixing it with objects built
# otherwise. enter.o is assembled with no flags. Read from make's own rules (make -p -n), so nothing is built.
cd "${1:-$(dirname "$0")/..}" || exit 2
unset MAKEFLAGS MFLAGS MAKELEVEL
db=$(make -p -n B=build 2>/dev/null)
objs=$(grep -E '^build/[a-z0-9_]+\.o:' <<< "$db" | sed 's/:.*//' | sort -u)
stamped=$(grep -E '^build/[a-z0-9_]+\.o: .*build/\.flags-' <<< "$db" | sed 's/:.*//' | sort -u)
missing=$(comm -23 <(echo "$objs") <(echo "$stamped") | grep -vx 'build/enter.o')
[ "$(wc -l <<< "$objs")" -gt 20 ] || { echo "FAIL: found only $(wc -l <<< "$objs") objects in make's rules"; echo "test_flags_stamp FAILED"; exit 1; }
if [ -n "$missing" ]; then
    echo "FAIL: not rebuilt when the flags change (add them to the flags stamp's list in the Makefile):"; echo "$missing"
    echo "test_flags_stamp FAILED"; exit 1
fi
echo "test_flags_stamp ok"
