#!/bin/bash
# The engine's instruction set, checked on the built objects (Makefile: ios-lib and check-isa run it).
#
# 1. Atomics are LSE (CAS, LDADD, SWP): no compiler-made LDXR/STXR retry loops. Without -mcpu clang builds
#    for the A7 and every __atomic is such a loop, which a contended line (the code cache's locks, the
#    TLBI waits) can keep failing. The engine's own inline LDAXR (the in-place WFE wait) has no store.
# 2. Nothing the A12 can't run. The A12 is the oldest device the app installs on: ios/Henge/Info.plist requires
#    iphone-ipad-minimum-performance-a12 (iPadOS 17 alone would admit the A10 iPads, which have no LSE). The app
#    calls into this library at launch on every device it installs on, the A12 too, before Hardware.swift's
#    chipTooOld says why it won't start a Mac (A13 and older); an instruction newer than the A12's would crash it
#    instead. The objects are disassembled as for the A12, which shows every such
#    instruction as <unknown> (A13's UDOT/SDOT, SHA3, FMLAL; Armv8.4's LDAPUR, STLUR, FRINT32/64), and the
#    flag-manipulation ones (CFINV, AXFLAG, XAFLAG), which it shows as a raw MSR S0_..., are named as well.
# 3. The premise of 2: ios/Henge/Info.plist does require the A12.
#
# Usage: tools/check_engine_isa.sh <library or objects...>
set -euo pipefail
[ $# -gt 0 ] || { echo "usage: $0 <lib.a | file.o ...>" >&2; exit 2; }
ops=$(xcrun objdump -d --no-show-raw-insn "$@" | awk 'NF >= 2 && $1 ~ /^[0-9a-f]+:$/ { print $2 }' | sort | uniq -c)
[ -n "$ops" ] || { echo "check_engine_isa: no instructions in $*" >&2; exit 2; }
bad=0
llsc=$(echo "$ops" | awk '$2 ~ /^st(l)?x(r|p)(b|h)?$/')
if [ -n "$llsc" ]; then
    echo "check_engine_isa: exclusive stores (LL/SC atomics; is the Makefile's ENGINE_CPU missing?):" >&2
    echo "$llsc" >&2
    bad=1
fi
new=$(echo "$ops" | awk '$2 ~ /^(ldapur|stlur)/ || $2 ~ /^(cfinv|rmif|setf8|setf16|axflag|xaflag)$/ || $2 ~ /^frint(32|64)/')
a12=$(xcrun objdump -d --no-show-raw-insn --mcpu=apple-a12 "$@" |
    awk '/^[0-9a-f]+ <.*>:$/ { fn = substr($0, index($0, "<")) } NF >= 2 && $1 ~ /^[0-9a-f]+:$/ && ($2 == "<unknown>" || $0 ~ /[ \t]S0_/) { print fn }' |
    sort | uniq -c)
if [ -n "$new$a12" ]; then
    echo "check_engine_isa: instructions an A12 can't run at app launch (is ENGINE_CPU newer than -mcpu=apple-a12?):" >&2
    [ -n "$new" ] && echo "$new" >&2
    [ -n "$a12" ] && echo "$a12" | sed 's/^/    in /' >&2
    bad=1
fi
# 3. The A12 floor itself: without the plist's requirement the A10 iPads install the app and crash at launch.
plist="$(dirname "$0")/../ios/Henge/Info.plist"
if ! plutil -extract UIRequiredDeviceCapabilities xml1 -o - "$plist" 2>/dev/null |
    grep -q '<string>iphone-ipad-minimum-performance-a12</string>'; then
    echo "check_engine_isa: ios/Henge/Info.plist's UIRequiredDeviceCapabilities lacks iphone-ipad-minimum-performance-a12:" >&2
    echo "    an A10 iPad (iPadOS 17, no LSE) would install the app and crash at launch" >&2
    bad=1
fi
[ $bad -eq 0 ] && echo "check_engine_isa: LSE atomics only, nothing the A12 lacks ($(echo "$ops" | wc -l | tr -d ' ') opcodes)"
exit $bad
