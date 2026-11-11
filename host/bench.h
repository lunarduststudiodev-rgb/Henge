/* The benchmark: the same work inside the Mac on every build, measured the same way, so builds can be compared
   (tools/bench_report.py). The helper inside macOS (tools/henge-agent.m, version 36) runs the steps when asked
   (share.h's hv_share_bench) and says as each one starts and ends; at each of those moments this reads what Henge
   itself counts: the engine's counters (core/machine.h HvStats), how evenly frames came (devices/frame_pace.h), the
   freeze recorder's stalls (stallmon.h), the CPU time the guest's CPUs got and the device's heat. Each step is one
   JSON line in <dir>/henge-bench.jsonl, after a line about the run (build, device, settings) and before one saying
   how it ended. Asked by the autopilot's "bench" (ios/Henge/Autopilot.swift). */
#ifndef HV_BENCH_H
#define HV_BENCH_H

#include <stddef.h>
#include <stdint.h>

#include "../devices/frame_pace.h"

/* Where henge-bench.jsonl goes (Documents); the helper's lines come here from then on. Once. */
void hv_bench_start(const char *dir);
/* Asks the helper to run steps ("all", or names with commas between: idle, video, scroll, apps, cpu, rosetta) as a
   run called label (letters, digits, - . _; may be empty) on the build named build (the app's commit and version,
   for the report). 0 asked; -1 not, with why. A run already under way is refused. */
int hv_bench_run(const char *label, const char *steps, const char *build, char *why, size_t why_len);
/* The last run's state: one of these. */
enum { HV_BENCH_NONE = 0, HV_BENCH_RUNNING = 1, HV_BENCH_DONE = 2, HV_BENCH_FAILED = 3 };
int hv_bench_state(void);
/* Ends a run under way as failed, saying why (the autopilot gave up waiting). */
void hv_bench_give_up(const char *why);

/* ---- what a step's line is made of (bench_json.c, tested on its own) ---- */

typedef struct {
    uint64_t t_ns; /* CLOCK_UPTIME_RAW */
    /* the engine's counters (HvStats) */
    uint64_t blocks, translations, tlbi_all_here, tlb_full_drops, sw_saved, sw_dropped, code_protects;
    /* why code was translated again, and what writes to code threw out (HvStats.retrans_*, lost_races, flush_smc,
       retired, retired_words, rewrite_*): always counted */
    uint64_t retrans_evicted, retrans_written, lost_races, page_drops, retired, retired_words, rewrite_same, rewrite_changed;
    uint64_t entries_written[4], entries_evicted[4]; /* dispatcher entries before death (HvStats.entries_*) */
    int stats_counted;       /* the engine counted its --stats / HV_STATS counters (sw_saved, sw_dropped):
                                0, they stay 0 (core/machine.h HvStats) */
    /* the code cache: whole drops, zones emptied and their blocks; the times every CPU was stopped for either, their
       time together and the longest since the last reading (each reading takes it, so it is the longest in the step) */
    uint64_t cache_drops_all, cache_empties, blocks_dropped, stops;
    uint64_t empties_in_stop; /* of cache_empties, those emptied with every CPU stopped: none was ready (evict_sync) */
    uint64_t reclaim_kicks;   /* the reclaimer's kicks of every CPU in its grace periods (one per zone, more if a CPU is slow) */
    double stop_s, stop_max_s;
    double stop_wait_s;      /* time the guest's CPUs spent held at those stops, together: they wanted no host CPU */
    double sync_s, wfi_s;    /* host time spent waiting for TLBI acks, and asleep in WFI, all CPUs together */
    double cpu_s;            /* time the guest CPU threads had on a host CPU, together */
    int cpu_threads;         /* how many (0: not known) */
    unsigned stalls[6];      /* the freeze recorder's finds by kind (stall_detect.h) */
    uint64_t stall_ms;
    int thermal;             /* 0..4, -1 not known */
    HvPaceCopy mac, shown;
    HvFrameFlowCopy flow;    /* what became of the Mac's frames on their way to the glass */
} HvBenchSnap;

/* One step's line (no newline) into out. run and step are the names; status ok, skip or fail (a skipped step has no
   measures: m is empty), why a word or "";
   target_ms the gap the step's frames should come at (0: none expected); agent the helper's KEY=NUMBER pairs (each
   becomes agent_KEY; others are left out); ncpus the Mac's. The measures from counters the engine counts only with
   HV_STATS are left out of m and named in not_counted when it didn't count them at either reading. Returns the length,
   or -1 if it didn't fit. */
int hv_bench_step_json(char *out, size_t cap, const char *run, const char *step, const char *status, const char *why,
                       double target_ms, const char *agent, const HvBenchSnap *before, const HvBenchSnap *after, int ncpus);
/* s as a JSON string, quotes included, appended at out + *len (cap the room in all). 0, or -1 if it didn't fit. */
int hv_bench_put_string(char *out, size_t cap, size_t *len, const char *s);

#endif
