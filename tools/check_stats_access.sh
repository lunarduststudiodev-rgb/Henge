#!/bin/bash
# The engine's counters are per CPU (core/machine.h HvStats): a CPU thread counts into its own block
# (HV_CPU_COUNT, read with HV_CPU_STAT), other threads into the machine's with HV_STAT_ADD, and readers add every block up
# (HV_STAT_TOTAL, hv_stats_sum). Code that touches a machine's .stats block directly compiles and runs, but it
# reads only what non-CPU threads counted (next to nothing), or races the other CPUs with a plain ++. This
# lists every such access outside core/machine.c and core/machine.h, except the counters that really are the
# machine's: the stop-the-world ones (stops, stop_ticks, stop_wait_ticks, stop_max_ticks), the longest emptying of a zone
# (evict_max_ticks), the reclaimer's longest grace period, tr_mu hold and quick stop (reclaim_gp_max_ticks,
# reclaim_hold_max_ticks, quick_stop_max_ticks: atomic maxima) and host_undefs.
#
# Usage: tools/check_stats_access.sh [repo root]   (exit 1 and the lines, if any)
set -uo pipefail
cd "${1:-$(dirname "$0")/..}" || exit 2
MACHINE_LEVEL='stops|stop_ticks|stop_wait_ticks|stop_max_ticks|evict_max_ticks|reclaim_gp_max_ticks|reclaim_hold_max_ticks|quick_stop_max_ticks|host_undefs'
bad=$(grep -nE '(->|\.)stats\b' core/*.c core/*.h host/*.c host/*.h host/*.m devices/*.c devices/*.h devices/*.m 2>/dev/null |
    grep -vE '^core/machine\.(c|h):' |
    grep -vE '^[^:]+:[0-9]+:\s*(/\*|\*|//)' |
    grep -vE "(->|\.)stats\.($MACHINE_LEVEL)\b" |
    grep -vE 'pc_state->stats|percpu\[[^]]*\]\.stats|(->|\.)stats_')
if [ -n "$bad" ]; then
    echo "check_stats_access: a machine's stats block used directly (use HV_CPU_COUNT, HV_CPU_STAT, HV_STAT_ADD, HV_STAT_TOTAL or hv_stats_sum):" >&2
    echo "$bad" >&2
    exit 1
fi
echo "check_stats_access: the counters are reached only through their macros (machine-level: $MACHINE_LEVEL)"
