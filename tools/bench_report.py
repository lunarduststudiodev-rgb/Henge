#!/usr/bin/env python3
"""Compares benchmark runs (host/bench.h) build against build, step by step.

    tools/bench_report.py henge-bench.jsonl [more files or folders] [--base BUILD] [--each] [--list]

Each run is the same work inside the Mac (idle, a video, a scrolling page, opening apps, CPU work, Rosetta) with
Henge's own measures for each step. Runs of the same build and label are repeats: their spread is the noise, and a
change between builds counts only when it is bigger than that (with no repeats, 15% and a floor per measure stand
in, and the report says so; run a build twice to measure the noise). The first build given (or --base, a commit
prefix, label or code-cache size) is what the others are compared with. Runs at different code-cache sizes (Settings)
are kept apart like labels. A flagged change says whether it is worse (REGRESSION) or
better; measures that are neither in themselves (how much the engine translated, say) are shown without a flag.
A run made hotter (the device's thermal state) is pointed out: heat slows the iPad down whatever the build.

Some measures also have a limit (LIMITS), flagged OVER in any run past it, the base build's too.

Exit status: 0, or 1 with --fail-on-regression when something got worse or went over a limit.
"""
import argparse
import collections
import json
import os
import re
import statistics
import sys

STEP_ORDER = ["idle", "video", "scroll", "apps", "cpu", "rosetta"]

# What each measure means, and which way is better: -1 lower, +1 higher, 0 neither. The floor is the smallest
# change worth flagging whatever the noise (in the measure's own units).
MEASURES = [
    # how evenly the Mac's frames came (devices/frame_pace.h), and Henge's on the iPad's screen
    ("mac_fps", +1, 1.0, "frames the Mac drew a second"),
    ("mac_late_pct", -1, 1.0, "% of the Mac's frames a refresh late or more"),
    ("mac_stalled_pct", -1, 0.5, "% of the Mac's frames 3 or more refreshes late"),
    ("mac_dropped_per_s", -1, 0.5, "frames missing a second"),
    ("mac_p50_ms", 0, 2.0, "median gap between the Mac's frames (ms)"),
    ("mac_p90_ms", -1, 4.0, "90th percentile gap (ms)"),
    ("mac_p99_ms", -1, 8.0, "99th percentile gap (ms)"),
    ("mac_max_ms", -1, 50.0, "longest gap (ms)"),
    ("shown_fps", +1, 1.0, "frames shown on the iPad a second"),
    ("shown_late_pct", -1, 1.0, "% of the iPad's frames a refresh late or more"),
    ("shown_dropped_per_s", -1, 0.5, "iPad frames missing a second"),
    ("shown_p99_ms", -1, 8.0, "99th percentile gap on the iPad (ms)"),
    ("frames_replaced", -1, 5.0, "Mac frames replaced by a newer one before the iPad drew them"),
    ("presents_not_shown", -1, 2.0, "frames the iPad drew that never reached the screen"),
    ("swap_to_glass_p50_ms", -1, 4.0, "median time from the Mac's frame to the iPad's screen (ms)"),
    ("swap_to_glass_p99_ms", -1, 8.0, "99th percentile time from the Mac's frame to the screen (ms)"),
    ("swap_to_glass_max_ms", -1, 50.0, "longest time from the Mac's frame to the screen (ms)"),
    ("present_hold_mean_ms", -1, 0.25, "mean time each iPad frame held the Mac's GPU work behind it (ms)"),
    ("present_hold_p99_ms", -1, 0.5, "99th percentile of that hold (ms)"),
    ("present_hold_max_ms", -1, 2.0, "longest hold (ms)"),
    ("mac_gpu_wait_mean_ms", -1, 0.25, "mean wait of the Mac's GPU work from its commit to being scheduled (ms)"),
    ("mac_gpu_wait_p99_ms", -1, 1.0, "99th percentile of that wait (ms)"),
    ("mac_gpu_wait_max_ms", -1, 4.0, "longest wait (ms)"),
    # the freeze recorder (host/stallmon.c)
    ("hitches", -1, 2.0, "screen hitches"),
    ("slow_responses", -1, 2.0, "slow responses to input"),
    ("hangs", -1, 1.0, "hangs (1.5 s or more)"),
    ("app_stalls", -1, 1.0, "Henge's own main thread stuck"),
    ("stall_ms", -1, 250.0, "time stalled in all (ms)"),
    # the engine
    ("blocks_m_per_s", 0, 1.0, "millions of blocks run a second"),
    ("translated_per_s", 0, 50.0, "blocks translated a second"),
    ("retrans_evicted_pct", -1, 2.0, "% of translations of code an emptied zone held, its page unchanged since (the cache too small for what runs, or the wrong zone emptied); code emptied more than about three 650K-block zones ago goes uncounted"),
    ("retrans_written_pct", -1, 2.0, "% of translations of code thrown out before whose page was written since"),
    ("lost_races_per_s", -1, 20.0, "translations thrown away a second: another CPU placed the same block first"),
    ("page_drops_per_s", -1, 5.0, "pages whose translated code a write dropped, a second"),
    ("blocks_per_page_drop", 0, 2.0, "blocks each such write threw out"),
    ("kb_per_page_drop", 0, 2.0, "translated code each such write threw out (KB)"),
    ("rewrites_same_pct", 0, 5.0, "% of those writes (where known) that left the page's bytes as they were"),
    ("entries_before_write_1_pct", 0, 5.0, "% of blocks a code write retired that the dispatcher entered once (dispatcher entries before death, not runs)"),
    ("entries_before_write_2_8_pct", 0, 5.0, "... 2-8 times"),
    ("entries_before_write_9_64_pct", 0, 5.0, "... 9-64 times"),
    ("entries_before_write_65up_pct", 0, 5.0, "... more"),
    ("entries_before_evict_1_pct", 0, 5.0, "% of an emptied zone's live blocks the dispatcher entered once (dispatcher entries before death, not runs)"),
    ("entries_before_evict_2_8_pct", 0, 5.0, "... 2-8 times"),
    ("entries_before_evict_9_64_pct", 0, 5.0, "... 9-64 times"),
    ("entries_before_evict_65up_pct", 0, 5.0, "... more"),
    ("again_pct", -1, 2.0, "older builds: % of translations of code thrown out before (now retrans_evicted_pct + retrans_written_pct; before claude/evict-lists a hashed guess, about 28% too high)"),
    ("tlbi_all_per_s", 0, 5.0, "whole-TLB flushes here a second"),
    ("tlb_full_drops_per_s", -1, 5.0, "whole-TLB flushes that also cleared the branch caches a second (the stats line's 'cache drops full')"),
    ("switches_saved_per_s", 0, 5.0, "process switches that kept their TLB a second"),
    ("switches_dropped_per_s", -1, 5.0, "process switches whose kept TLB a flush dropped, a second"),
    ("new_code_pages_per_s", 0, 5.0, "pages becoming code a second"),
    # the code cache running out of room (its size is the run's code_cache_mb)
    ("cache_drops_all", -1, 1.0, "times the whole code cache was dropped"),
    ("cache_empties", -1, 2.0, "code-cache zones emptied to make room"),
    ("evict_sync", -1, 1.0, "of those, emptied with every CPU stopped (none emptied ahead was ready)"),
    ("reclaim_kicks", -1, 5.0, "kicks of every CPU while one emptied ahead waited for them (one per zone; more: a CPU slow to answer)"),
    ("blocks_dropped", -1, 5000.0, "translated blocks thrown away with them"),
    ("stops", -1, 2.0, "times every CPU was stopped for the code cache"),
    ("stop_ms", -1, 20.0, "time every CPU was stopped for it, in all (ms)"),
    ("stop_max_ms", -1, 10.0, "longest such stop (ms)"),
    ("tlbi_wait_pct", -1, 2.0, "time waiting for other CPUs' flushes (% of one CPU)"),
    ("idle_pct", 0, 3.0, "% of the Mac's CPU time idle"),
    ("cpu_got_pct", +1, 3.0, "% of the time the Mac's CPUs wanted (not idle, not held at a code-cache stop) that they ran on the iPad's"),
    # what the helper in the Mac measured
    ("agent_raf_fps", +1, 2.0, "the page's own frames a second"),
    ("agent_launch_ms_median", -1, 100.0, "median app launch to first window (ms)"),
    ("agent_launch_ms_max", -1, 300.0, "slowest app launch (ms)"),
    ("agent_work_s", -1, 0.5, "seconds the CPU work took"),
    ("wall_s", 0, 1.0, "the step's length (s)"),
]
MEASURE = {name: (better, floor, about) for name, better, floor, about in MEASURES}
# The measures the engine counts only with HV_STATS on (host/bench_json.c): a step that didn't count them names them in
# not_counted, but builds before that said nothing and gave 0 (119979b..e54ae63^). A run whose env has no HV_STATS, or
# HV_STATS=0, didn't count them either way, so its zeros are not measurements.
NEEDS_STATS = ("again_pct", "switches_saved_per_s", "switches_dropped_per_s")  # again_pct: older builds
# Limits flagged in any run, whatever the build compared with: a build already past one is never flagged by change
# alone. The present's hold: past 1 ms at the 99th percentile (or 2 ms at its longest) the Mac's GPU work waits behind
# the iPad's frames enough to matter, and the presenter should encode before the pick (gfx-fixes item 1, S1-2).
LIMITS = {
    "present_hold_p99_ms": 1.0,
    "present_hold_max_ms": 2.0,
}
NO_REPEATS_SHARE = 0.15  # without repeats, the change that counts
REPEATS_SHARE = 0.05     # and with them, never less than this


def read_lines(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in sorted(os.walk(p)):
                files += [os.path.join(root, n) for n in sorted(names) if n.startswith("henge-bench") and n.endswith(".jsonl")]
        else:
            files.append(p)
    out = []
    for f in files:
        with open(f, encoding="utf-8", errors="replace") as fh:
            for n, line in enumerate(fh, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    out.append(json.loads(line))
                except json.JSONDecodeError:
                    print(f"{f}:{n}: skipped a damaged line", file=sys.stderr)
    return out


class Run:
    def __init__(self, head):
        self.id = head.get("run", "?")
        self.label = head.get("label", "")
        build = head.get("build", "").split()
        self.commit = build[0] if build else "?"
        self.build = head.get("build", "")
        self.env = head.get("env", {})
        self.code_cache_mb = head.get("code_cache_mb")  # the code cache's size (Settings), None before it was said
        self.head = head
        self.steps = collections.OrderedDict()
        self.notes = []
        self.status = None  # "ok", "failed", or None: never ended (Henge quit, or the file was copied mid-run)
        self.why = ""

    @property
    def key(self):
        """One build, label and code-cache size: runs with the same key are repeats."""
        size = f"{self.code_cache_mb}MB" if self.code_cache_mb else ""
        return " ".join(p for p in (self.commit[:9], self.label, size) if p)

    def hottest(self):
        return max((max(s.get("thermal", [0, 0])) for s in self.steps.values()), default=-1)

    def stats_off(self):
        """The engine's statistics were off for the whole run (HV_STATS missing, or 0 as the engine reads it: atoi)."""
        v = re.match(r"\s*([+-]?\d+)", self.env.get("HV_STATS", ""))
        return not v or int(v.group(1)) == 0

    def counted(self, step, measure):
        """False when the step didn't count this measure: it says so (not_counted), or the statistics were off."""
        rec = self.steps.get(step, {})
        if measure in rec.get("not_counted", ()):
            return False
        return not (measure in NEEDS_STATS and self.stats_off())


def read_runs(paths):
    """The runs in the files, in the order they began; lines for a run whose start isn't there are left out."""
    runs = collections.OrderedDict()
    for rec in read_lines(paths):
        kind, rid = rec.get("type"), rec.get("run")
        if kind == "run":
            runs[rid] = Run(rec)
        elif rid in runs:
            run = runs[rid]
            if kind == "step":
                run.steps[rec.get("step", "?")] = rec
            elif kind == "note":
                run.notes.append(rec.get("text", ""))
            elif kind == "done":
                run.status, run.why = rec.get("status"), rec.get("why", "")
    return list(runs.values())


class Group:
    """Runs of one build and label: repeats of the same thing."""

    def __init__(self, key):
        self.key = key
        self.runs = []

    def values(self, step, measure):
        out = []
        for run in self.runs:
            rec = run.steps.get(step)
            if rec and rec.get("status") == "ok" and measure in rec.get("m", {}) and run.counted(step, measure):
                out.append(rec["m"][measure])
        return out

    def not_counted(self, step, measure):
        """No value, because every run that finished the step didn't count it"""
        ok = [run for run in self.runs if run.steps.get(step, {}).get("status") == "ok"]
        return bool(ok) and not self.values(step, measure) and all(not run.counted(step, measure) for run in ok)

    def value(self, step, measure):
        v = self.values(step, measure)
        return statistics.median(v) if v else None

    def spread(self, step, measure):
        v = self.values(step, measure)
        return max(v) - min(v) if len(v) >= 2 else None

    def target(self, step):
        return next((r.steps[step].get("target_ms", 0) for r in self.runs if step in r.steps), 0)

    def statuses(self, step):
        return [run.steps[step].get("status", "?") + (":" + run.steps[step]["why"] if run.steps[step].get("why") else "")
                if step in run.steps else "missing" for run in self.runs]


def group_runs(runs, each=False):
    groups = collections.OrderedDict()
    for run in runs:
        key = run.id if each else run.key
        groups.setdefault(key, Group(key)).runs.append(run)
    return list(groups.values())


def noise(groups, step, measure):
    """The largest spread between repeats of one build, or None with no repeats."""
    spreads = [s for s in (g.spread(step, measure) for g in groups) if s is not None]
    return max(spreads) if spreads else None


FRAME_RATES = ("mac_fps", "shown_fps")


def judge(measure, base, other, spread, target_ms=0):
    """None (not a change that counts), "worse" or "better". A frame rate counts only in a step that has frames to
    keep up with (target_ms): more frames while idle is no better."""
    better, floor, _ = MEASURE.get(measure, (0, 0.0, ""))
    if measure in FRAME_RATES and not target_ms:
        better = 0
    if base is None or other is None:
        return None
    delta = other - base
    share = REPEATS_SHARE if spread is not None else NO_REPEATS_SHARE
    limit = max(floor, share * abs(base), spread or 0.0)
    if abs(delta) <= limit or better == 0:
        return None
    return "better" if delta * better > 0 else "worse"


def over_limits(groups):
    """Measures past their LIMITS in any one run, base included, not only in a build's median: (group, step, measure,
    limit, the highest of its runs' values)."""
    out = []
    for g in groups:
        for step in step_names(g.runs):
            for name, limit in LIMITS.items():
                v = g.values(step, name)
                if v and max(v) > limit:
                    out.append((g.key, step, name, limit, max(v)))
    return out


def fmt(v, not_counted=False):
    if v is None:
        return "not counted" if not_counted else "-"
    if abs(v) >= 1000:
        return f"{v:.0f}"
    if abs(v) >= 100:
        return f"{v:.1f}"
    return f"{v:.2f}"


def change(base, other):
    if base is None or other is None:
        return ""
    if base == 0:
        return "" if other == 0 else "(new)"
    return f"{(other - base) / abs(base) * 100:+.0f}%"


def step_names(runs):
    seen = []
    for run in runs:
        for s in run.steps:
            if s not in seen:
                seen.append(s)
    return sorted(seen, key=lambda s: (STEP_ORDER.index(s) if s in STEP_ORDER else len(STEP_ORDER), s))


def measure_names(groups, step):
    seen = set()
    for g in groups:
        for run in g.runs:
            seen.update(run.steps.get(step, {}).get("m", {}))
    known = [name for name, *_ in MEASURES if name in seen]
    return known + sorted(seen - set(known))


def report(groups, out=None):
    """Prints the comparison; returns the regressions as (group, step, measure, base, other)."""
    out = out or sys.stdout
    base = groups[0]
    regressions = []
    over = {(key, step, name) for key, step, name, _, _ in over_limits(groups)}
    any_repeats = any(len(g.runs) >= 2 for g in groups)
    print(f"Compared with {base.key} ({len(base.runs)} run{'s' if len(base.runs) != 1 else ''})", file=out)
    for g in groups:
        failed = [r for r in g.runs if r.status != "ok"]
        print(f"  {g.key}: {len(g.runs)} run(s): {', '.join(r.id for r in g.runs)}"
              + (f"; NOT FINISHED: {', '.join(r.id + ' ' + (r.status or 'cut short') + (' ' + r.why if r.why else '') for r in failed)}" if failed else ""),
              file=out)
    for g in groups:
        if len({json.dumps(r.env, sort_keys=True) for r in g.runs}) > 1:
            print(f"  {g.key}: its runs had different settings (env): give each setting a label of its own", file=out)
    if not any_repeats:
        print("  No build was run twice, so the noise is guessed (15%): run one build twice to measure it.", file=out)
    hot = {g.key: max(r.hottest() for r in g.runs) for g in groups}
    if len(set(hot.values())) > 1:
        print("  HEAT DIFFERS (0 nominal .. 4): " + ", ".join(f"{k} {v}" for k, v in hot.items())
              + ": a hotter iPad runs slower whatever the build", file=out)
    for step in step_names([r for g in groups for r in g.runs]):
        print(f"\n== {step}", file=out)
        for g in groups:
            st = g.statuses(step)
            if any(s != "ok" for s in st):
                print(f"   {g.key}: {', '.join(st)}", file=out)
        names = measure_names(groups, step)
        if not names:
            continue
        width = max(len(n) for n in names)
        cols = [g.key[:24] for g in groups]
        print(f"   {'':{width}}  " + "  ".join(f"{c:>24}" for c in cols), file=out)
        for name in names:
            spread = noise(groups, step, name)
            b = base.value(step, name)
            cells = [f"{(fmt(b, base.not_counted(step, name)) + (' OVER' if (base.key, step, name) in over else '')).strip():>24}"]
            for g in groups[1:]:
                v = g.value(step, name)
                verdict = judge(name, b, v, spread, base.target(step) or g.target(step))
                mark = {"worse": " WORSE", "better": " better"}.get(verdict, "") + (" OVER" if (g.key, step, name) in over else "")
                if verdict == "worse":
                    regressions.append((g.key, step, name, b, v))
                cells.append(f"{(fmt(v, g.not_counted(step, name)) + ' ' + change(b, v) + mark).strip():>24}")
            tail = f"  (noise {fmt(spread)})" if spread is not None else ""
            print(f"   {name:{width}}  " + "  ".join(cells) + tail, file=out)
    print("", file=out)
    if regressions:
        print("REGRESSIONS (worse by more than the noise):", file=out)
        for key, step, name, b, v in regressions:
            print(f"  {key}: {step} {name} {fmt(b)} -> {fmt(v)} ({change(b, v)}): {MEASURE.get(name, (0, 0, name))[2]}", file=out)
    elif len(groups) > 1:
        print("No regressions beyond the noise.", file=out)
    for key, step, name, limit, v in over_limits(groups):
        print(f"OVER THE LIMIT: {key}: {step} {name} {fmt(v)} (limit {fmt(limit)}): {MEASURE.get(name, (0, 0, name))[2]}", file=out)
    return regressions


def pick_base(groups, base):
    if not base:
        return groups
    for i, g in enumerate(groups):
        run = g.runs[0]
        if g.key == base or run.commit.startswith(base) or run.label == base or (run.code_cache_mb and f"{run.code_cache_mb}MB" == base):
            return [g] + groups[:i] + groups[i + 1:]
    raise SystemExit(f"no runs of {base}: there are {', '.join(g.key for g in groups)}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help="henge-bench.jsonl files, or folders holding them")
    ap.add_argument("--base", help="the build to compare with: a commit prefix, a label, a code-cache size (1024MB) or the whole key")
    ap.add_argument("--each", action="store_true", help="every run on its own, not grouped by build and label")
    ap.add_argument("--list", action="store_true", help="only list the runs found")
    ap.add_argument("--fail-on-regression", action="store_true", help="exit 1 if something got worse or went over a limit")
    args = ap.parse_args(argv)
    runs = read_runs(args.paths)
    if not runs:
        raise SystemExit("no benchmark runs in " + ", ".join(args.paths))
    if args.list:
        for r in runs:
            env = " ".join(f"{k}={v}" for k, v in sorted(r.env.items()))
            print(f"{r.id}  {r.build}  {r.head.get('device', '?')} {r.head.get('ncpus', '?')} CPUs  "
                  f"{str(r.code_cache_mb) + ' MB code cache  ' if r.code_cache_mb else ''}"
                  f"{r.status or 'not finished'}  steps {','.join(r.steps)}  {env}")
        return 0
    groups = pick_base(group_runs(runs, args.each), args.base)
    regressions = report(groups)
    return 1 if (regressions or over_limits(groups)) and args.fail_on_regression else 0


if __name__ == "__main__":
    sys.exit(main())
