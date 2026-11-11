# Debug and measurement costs in the shipped engine

What the iOS build does by default that exists for debugging or measuring, and what each costs. Checked by
reading the code (core/, devices/, host/, ios/Henge/Native/); the rates are from a stats sample of macOS
starting apps on an iPad Pro M4, and the costs are estimates from those rates, not timings.

## Off by default

| Switch | What it does | Why it is off |
|---|---|---|
| `HV_REWRITE_HASH` | Hashes each 16 KB page as it becomes code and again when a write drops that code, for `HvStats.rewrite_same` / `rewrite_changed` (and the benchmark's `rewrites_same_pct`). | About 1 µs a hash, 4,000 a second while apps start: ~4–8 ms of CPU a second. Turned on for a benchmark run by `host/bench.c`. |
| `HV_STATS` | The per-block and per-instruction statistics and the periodic stats line. | Off in the app (`EngineTuning.swift`); honoured only from `henge-env.txt`. |
| `HV_JIT_POISON` | `gen_checks`: every translated block checks the cache generation it was made under. | A check per block. |
| `HV_SLOW_MEM`, `HV_BIG_LOCK`, `HV_EXIT_CHECKS`, `HV_BLOCK_DUMP`, `HV_KVA_HIST`, `HV_PROF_IOS`, `HV_COLOR_PROBE` | Debugging aids (every access through the helper, one CPU at a time, exit checks, block dumps, histograms, profiler, colour probe). | Debugging only. |
| `m->log >= 2` | Per-exception, per-system-register, per-TLBI and per-MMIO logging. | The app runs at log level 1. |

The iOS engine is built `-O2`, with no sanitizers, and has no `assert()` calls on the hot paths.

## On by default, and kept

| What | Where | When it runs | Cost |
|---|---|---|---|
| Retranslation ledger (`retrans_evicted`, `retrans_written`) | `core/machine.h` `hv_ghost_note` / `hv_ghost_take` | Once per translation, and per block dropped from the code cache | A lookup in a 4 MB table under the translation lock: est. 5–15 ms a second at peak translation rates. Kept: the code-cache eviction work is measured with it. |
| Freeze recorder | `host/stallmon.c` | Wakes every 10 ms (a `getrusage`); during a freeze it samples the CPU threads' stacks | Small when idle; while a freeze is being watched it briefly stops the threads it samples. `HV_FREEZES=0` turns it off (and with it the benchmark's freeze counts). |
| Stop and sync timing (`stop_max`, `evict_sync`, `reclaim_kicks`, TLBI sync waits) | `core/dispatch.c`, `core/machine.c` | Per stop, per TLBI sync (≈25k/s) | Two clock reads per wait: est. ~2 ms a second, next to the waits themselves. |
| Trace rings, block entry counts, per-CPU counters | `core/dispatch.c`, `core/exception.c`, `core/mmu.c` | Per dispatcher entry, exception, refill | Plain stores to the CPU's own structure: negligible. |
| Suspend measurement hooks | `ios/Henge/Native/henge-vm.c` `lab_install` | Per network frame (~300/s), per RTC read | An atomic add and a clock read: negligible. |
| Graphics trace poll | `devices/pvg_lite.c` `hv_pvg_tracing` | Per GPU command | One clock read (~30 ns); the file check is once a second. |
| Per-frame pacing counters, `NSUserDefaults` cursor reads | `devices/frame_pace.c`, `devices/pvg_metal.m` | ~60 a second | Negligible. |
| Log-gated `getenv` calls | `devices/aes.c`, `devices/pcie.c`, `devices/virtio_input.c` | Per AES command, PCI config read, input notify | ~0.3 µs each; these paths are rare. |
| Rare-path warnings (unknown registers, undefined instructions) | `devices/vmapple.c`, `core/sysreg.c`, `core/memop.c`, `core/exception.c` | Rare | Negligible unless a guest hits one in a loop. |

Log lines go through a queue (`ios/Henge/Native/henge-logq.c`) written by its own thread, never the main thread.
