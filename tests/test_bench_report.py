#!/usr/bin/env python3
"""tools/bench_report.py: reading runs, grouping repeats, the noise, and what gets flagged.

    python3 tests/test_bench_report.py
"""
import io
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import bench_report as br  # noqa: E402


def run_lines(rid, commit, label, steps, status="ok", env=None, thermal=0, cache_mb=None):
    out = [{"type": "run", "run": rid, "label": label, "build": f"{commit} 1.0 (5)", "device": "iPad16,3", "when": "x",
            "steps": "all", "ncpus": 4, "rosetta": 0, "agent": 36, "env": env or {}}]
    if cache_mb:
        out[0]["code_cache_mb"] = cache_mb
    for name, target, m in steps:
        out.append({"type": "step", "run": rid, "step": name, "status": "ok", "why": "", "target_ms": target,
                    "thermal": [0, thermal], "m": m})
    if status:
        out.append({"type": "done", "run": rid, "status": status, "why": "" if status == "ok" else "gone", "wall_s": 300})
    return out


def write(dirpath, name, lines):
    path = os.path.join(dirpath, name)
    with open(path, "w") as f:
        for rec in lines:
            f.write(json.dumps(rec) + "\n")
    return path


class Report(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="bench-report-")

    def report(self, paths, base=None):
        groups = br.pick_base(br.group_runs(br.read_runs(paths)), base)
        out = io.StringIO()
        regs = br.report(groups, out)
        return regs, out.getvalue()

    def test_reads_runs_and_skips_damage(self):
        lines = run_lines("r1", "aaaaaaaaaaaa", "", [("idle", 0, {"wall_s": 20})])
        path = write(self.dir, "henge-bench.jsonl", lines)
        with open(path, "a") as f:
            f.write("{not json\n")
            f.write(json.dumps({"type": "step", "run": "nobody", "step": "idle", "m": {}}) + "\n")  # no run line
        runs = br.read_runs([self.dir])  # a folder: its henge-bench*.jsonl
        self.assertEqual(len(runs), 1)
        self.assertEqual(runs[0].commit, "aaaaaaaaaaaa")
        self.assertEqual(runs[0].status, "ok")
        self.assertEqual(list(runs[0].steps), ["idle"])

    def test_regression_beyond_noise(self):
        # the old build twice (noise 2 late-pct points), the new one once: +10 points is worse, +1 isn't
        old1 = run_lines("o1", "old000000000", "", [("video", 33.3, {"mac_late_pct": 10.0, "mac_fps": 29.0, "tlb_full_drops_per_s": 14})])
        old2 = run_lines("o2", "old000000000", "", [("video", 33.3, {"mac_late_pct": 12.0, "mac_fps": 28.5, "tlb_full_drops_per_s": 15})])
        new = run_lines("n1", "new000000000", "", [("video", 33.3, {"mac_late_pct": 21.0, "mac_fps": 28.9, "tlb_full_drops_per_s": 102})])
        p = write(self.dir, "a.jsonl", old1 + old2 + new)
        regs, text = self.report([p])
        names = {(r[1], r[2]) for r in regs}
        self.assertIn(("video", "mac_late_pct"), names)
        self.assertIn(("video", "tlb_full_drops_per_s"), names)
        self.assertNotIn(("video", "mac_fps"), names)
        self.assertIn("Compared with old000000", text)
        self.assertIn("(noise 2.00)", text)
        self.assertNotIn("guessed", text)

    def test_no_repeats_uses_share_and_floor(self):
        a = run_lines("a", "aaaaaaaaaaaa", "", [("apps", 0, {"agent_launch_ms_median": 1000, "hitches": 1})])
        b = run_lines("b", "bbbbbbbbbbbb", "", [("apps", 0, {"agent_launch_ms_median": 1100, "hitches": 2})])
        regs, text = self.report([write(self.dir, "x.jsonl", a + b)])
        self.assertEqual(regs, [])  # 10% is within the guessed 15%; one more hitch is under the floor of 2
        self.assertIn("guessed", text)
        c = run_lines("c", "cccccccccccc", "", [("apps", 0, {"agent_launch_ms_median": 1300, "hitches": 5})])
        regs, _ = self.report([write(self.dir, "y.jsonl", a + c)])
        self.assertEqual({r[2] for r in regs}, {"agent_launch_ms_median", "hitches"})

    def test_better_and_neutral(self):
        self.assertEqual(br.judge("agent_work_s", 10.0, 7.0, None), "better")
        self.assertEqual(br.judge("agent_work_s", 10.0, 13.0, None), "worse")
        self.assertIsNone(br.judge("blocks_m_per_s", 10.0, 30.0, None))  # neither better nor worse in itself
        self.assertEqual(br.judge("mac_fps", 30.0, 20.0, None, 33.3), "worse")
        self.assertIsNone(br.judge("mac_fps", 30.0, 20.0, None, 0))  # idle: fewer frames is no worse
        self.assertIsNone(br.judge("mac_late_pct", None, 5.0, None))
        self.assertIsNone(br.judge("unknown_measure", 1.0, 100.0, None))

    def test_labels_split_one_build(self):
        # the same build with and without a setting: two groups, compared
        a = run_lines("a", "669c1b0aaaaa", "flush0", [("idle", 0, {"tlbi_wait_pct": 7.0})], env={"HV_SWITCH_FLUSH": "0"})
        b = run_lines("b", "669c1b0aaaaa", "flush2", [("idle", 0, {"tlbi_wait_pct": 20.0})], env={"HV_SWITCH_FLUSH": "2"})
        regs, text = self.report([write(self.dir, "z.jsonl", a + b)], base="flush0")
        self.assertEqual([(r[0], r[2]) for r in regs], [("669c1b0aa flush2", "tlbi_wait_pct")])
        regs, _ = self.report([write(self.dir, "z.jsonl", a + b)], base="flush2")
        self.assertEqual(regs, [])  # the other way round it got better

    def test_code_cache_sizes_split_one_build(self):
        # the same build at 1536 and 1024 MB (Settings): two groups even with the same label, each size its repeats
        big = [run_lines(f"b{i}", "669c1b0aaaaa", "", [("apps", 0, {"cache_empties": e, "stop_max_ms": 12.0})], cache_mb=1536)
               for i, e in enumerate((0, 1))]
        small = [run_lines(f"s{i}", "669c1b0aaaaa", "", [("apps", 0, {"cache_empties": e, "stop_max_ms": 60.0})], cache_mb=1024)
                 for i, e in enumerate((9, 11))]
        p = write(self.dir, "cc.jsonl", sum(big + small, []))
        groups = br.group_runs(br.read_runs([p]))
        self.assertEqual([(g.key, len(g.runs)) for g in groups], [("669c1b0aa 1536MB", 2), ("669c1b0aa 1024MB", 2)])
        regs, text = self.report([p], base="1536MB")
        self.assertEqual({(r[0], r[2]) for r in regs}, {("669c1b0aa 1024MB", "cache_empties"), ("669c1b0aa 1024MB", "stop_max_ms")})
        self.assertIn("Compared with 669c1b0aa 1536MB", text)
        regs, _ = self.report([p], base="1024MB")
        self.assertEqual(regs, [])
        # a run from before the size was said: no size in its key
        old = br.read_runs([write(self.dir, "old.jsonl", run_lines("o", "669c1b0aaaaa", "x", []))])[0]
        self.assertEqual(old.key, "669c1b0aa x")

    def test_unfinished_heat_and_mixed_settings(self):
        a = run_lines("a", "aaaaaaaaaaaa", "", [("cpu", 0, {"agent_work_s": 10})], thermal=0)
        b = run_lines("b", "bbbbbbbbbbbb", "", [("cpu", 0, {"agent_work_s": 10})], status=None, thermal=2)
        c = run_lines("c", "bbbbbbbbbbbb", "", [("cpu", 0, {"agent_work_s": 10})], env={"HV_X": "1"})
        _, text = self.report([write(self.dir, "w.jsonl", a + b + c)])
        self.assertIn("NOT FINISHED: b cut short", text)
        self.assertIn("HEAT DIFFERS", text)
        self.assertIn("different settings", text)

    def test_failed_step_left_out(self):
        a = run_lines("a", "aaaaaaaaaaaa", "", [("video", 33.3, {"mac_late_pct": 5.0})])
        b = run_lines("b", "bbbbbbbbbbbb", "", [])
        b.insert(1, {"type": "step", "run": "b", "step": "video", "status": "fail", "why": "noplay", "target_ms": 0, "thermal": [0, 0], "m": {}})
        regs, text = self.report([write(self.dir, "v.jsonl", a + b)])
        self.assertEqual(regs, [])
        self.assertIn("fail:noplay", text)

    def test_limits_whatever_the_base(self):
        # the present's hold past 1 ms (p99) or 2 ms (longest) is flagged in every build, the base's too, even unchanged
        a = run_lines("a", "aaaaaaaaaaaa", "", [("video", 33.3, {"present_hold_p99_ms": 1.4, "present_hold_max_ms": 1.5})])
        b = run_lines("b", "bbbbbbbbbbbb", "", [("video", 33.3, {"present_hold_p99_ms": 1.45, "present_hold_max_ms": 2.5})])
        p = write(self.dir, "h.jsonl", a + b)
        regs, text = self.report([p])
        self.assertEqual(regs, [])  # no change beyond the noise
        over = br.over_limits(br.group_runs(br.read_runs([p])))
        self.assertEqual({(k, n) for k, _, n, _, _ in over},
                         {("aaaaaaaaa", "present_hold_p99_ms"), ("bbbbbbbbb", "present_hold_p99_ms"), ("bbbbbbbbb", "present_hold_max_ms")})
        self.assertIn("OVER THE LIMIT: aaaaaaaaa: video present_hold_p99_ms 1.40 (limit 1.00)", text)
        self.assertIn("1.45 +4% OVER", text)  # past the limit, though no change worth flagging
        # a lone run, nothing to compare with: still flagged, and --fail-on-regression fails on it
        lone = write(self.dir, "l.jsonl", a)
        _, text = self.report([lone])
        self.assertIn("OVER THE LIMIT", text)
        out, old = io.StringIO(), sys.stdout
        sys.stdout = out
        try:
            self.assertEqual(br.main([lone, "--fail-on-regression"]), 1)
            under = write(self.dir, "u.jsonl", run_lines("u", "uuuuuuuuuuuu", "", [("video", 33.3, {"present_hold_p99_ms": 0.6, "present_hold_max_ms": 1.9})]))
            self.assertEqual(br.main([under, "--fail-on-regression"]), 0)
        finally:
            sys.stdout = old
        self.assertNotIn("OVER", out.getvalue().split("Compared with uuuuuuuuu")[1])
        # one run of three past it, though the build's median isn't: flagged, with that run's value
        runs = sum((run_lines(f"r{i}", "rrrrrrrrrrrr", "", [("video", 33.3, {"present_hold_max_ms": m})]) for i, m in enumerate((1.0, 3.0, 1.2))), [])
        over = br.over_limits(br.group_runs(br.read_runs([write(self.dir, "r.jsonl", runs)])))
        self.assertEqual([(n, v) for _, _, n, _, v in over], [("present_hold_max_ms", 3.0)])

    def test_not_counted(self):
        # The three measures the engine counts only with HV_STATS. Builds 119979b..e54ae63^ gave them as 0 with it off,
        # saying nothing; later ones name them in not_counted. Neither is a measurement: no verdict, said as such.
        stats = {"again_pct": 6.0, "switches_saved_per_s": 40.0, "switches_dropped_per_s": 9.0, "hitches": 1}
        zeros = {"again_pct": 0.0, "switches_saved_per_s": 0.0, "switches_dropped_per_s": 0.0, "hitches": 1}
        on = {"HV_STATS": "10"}
        old_zeros = run_lines("o", "119979b00000", "", [("apps", 0, zeros)])  # HV_STATS off, nothing said
        new_on = run_lines("n", "e54ae6300000", "", [("apps", 0, stats)], env=on)
        regs, text = self.report([write(self.dir, "nc1.jsonl", old_zeros + new_on)])
        self.assertEqual(regs, [])  # was: again_pct 0 -> 6 and switches_dropped_per_s 0 -> 9 WORSE
        self.assertIn("not counted", text)
        self.assertNotIn("WORSE", text)
        # HV_STATS=0 is off too; and the other way round, a counted base against a run that didn't count
        zero_env = run_lines("z", "119979b00000", "", [("apps", 0, zeros)], env={"HV_STATS": "0"})
        regs, _ = self.report([write(self.dir, "nc2.jsonl", new_on + zero_env)])
        self.assertEqual(regs, [])
        # a later build names them in not_counted and leaves them out of m
        named = run_lines("m", "7b035ed00000", "", [("apps", 0, {"hitches": 1})])
        named[1]["not_counted"] = list(br.NEEDS_STATS)
        regs, text = self.report([write(self.dir, "nc3.jsonl", new_on + named)])
        self.assertEqual(regs, [])
        self.assertEqual(text.count("not counted"), 3)
        # counted on both sides, a real change is still flagged; a measure the statistics don't gate is read as before
        worse = run_lines("w", "d40c6f200000", "", [("apps", 0, dict(stats, again_pct=20.0, hitches=9))], env=on)
        regs, _ = self.report([write(self.dir, "nc4.jsonl", new_on + worse)])
        self.assertEqual({r[2] for r in regs}, {"again_pct", "hitches"})
        regs, _ = self.report([write(self.dir, "nc5.jsonl", old_zeros + worse)])
        self.assertEqual({r[2] for r in regs}, {"hitches"})

    def test_cli(self):
        p = write(self.dir, "c.jsonl", run_lines("a", "aaaaaaaaaaaa", "", [("idle", 0, {"wall_s": 20})])
                  + run_lines("b", "bbbbbbbbbbbb", "", [("idle", 0, {"wall_s": 20})]))
        out = io.StringIO()
        old = sys.stdout
        sys.stdout = out
        try:
            self.assertEqual(br.main([p, "--list"]), 0)
            self.assertEqual(br.main([p, "--fail-on-regression"]), 0)
        finally:
            sys.stdout = old
        self.assertIn("iPad16,3", out.getvalue())
        with self.assertRaises(SystemExit):
            br.main([p, "--base", "nosuch"])


if __name__ == "__main__":
    unittest.main()
