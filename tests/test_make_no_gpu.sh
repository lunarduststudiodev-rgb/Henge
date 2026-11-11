#!/bin/bash
# make test (and test-cpu / test-shipped) never runs a test that drives the host's real GPU, and runs the rest with
# no way to reach it (HV_NO_GPU, HV_PVG_NO_METAL); make test-gpu runs exactly those. Read from what make would run
# (make -n), so nothing is built or run here.
cd "${1:-$(dirname "$0")/..}" || exit 2
unset MAKEFLAGS MFLAGS MAKELEVEL # when make test runs this, the outer make's flags (-s, -j) aren't this one's
GPU="test_pvg_snap test_metallib test_pvg_fuzz test_pvg_present test_pvg_validate"
fail=0
check() { "$@" || { echo "FAIL: $*"; fail=1; }; }
runs() { make -n "$1" 2>/dev/null | grep -E 'tools/run_tests\.sh|test_rosetta reference'; }
words() { tr ' ' '\n' | sed -n 's|^.*/\(test_[a-z0-9_]*\)$|\1|p' | sort -u; }

for target in test test-cpu; do
    lines=$(runs $target)
    check [ -n "$lines" ]
    while IFS= read -r l; do
        case "$l" in *"HV_NO_GPU=1 HV_PVG_NO_METAL=1"*) ;; *) echo "FAIL: make $target runs without HV_NO_GPU/HV_PVG_NO_METAL: ${l:0:120}"; fail=1 ;; esac
    done <<< "$lines"
    ran=$(words <<< "$lines")
    for g in $GPU; do
        grep -qx "$g" <<< "$ran" && { echo "FAIL: make $target runs $g (it drives the host's GPU)"; fail=1; }
    done
    for t in test_pvg_queue test_pvg_present_nogpu test_pvg_present_app test_classify; do # no GPU in these: they stay in
        check grep -qx "$t" <<< "$ran"
    done
done
check [ "$(runs test-gpu | words | tr '\n' ' ')" = "$(tr ' ' '\n' <<< "$GPU" | sort | tr '\n' ' ')" ]

[ "$fail" -eq 0 ] && echo "test_make_no_gpu ok" || echo "test_make_no_gpu FAILED"
exit $fail
