#!/usr/bin/env python3
"""tools/freeze_report.py names each record's offsets with the binary of that record's own build only.

Two records, both stalled in hv_run: one written by the build of the binary given (its UUID), one by another build,
where hv_run sat at the offset this binary has main at. The report must name both hv_run: the first through atos,
the second with the name the device wrote. Naming the second with this binary gives "main", which is what put
engine stalls under SwiftUI functions in the freeze reports of Nov 2026. A third, of this build, at an offset atos
has no name for (it answers with the bare address), keeps the device's name too.

Usage: test_freeze_report.py <a built engine binary with symbols, e.g. build/hengevm>
"""
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPORT = os.path.join(HERE, "..", "tools", "freeze_report.py")


def offset(binary, sym):
    out = subprocess.run(["nm", binary], capture_output=True, text=True, check=True).stdout
    m = re.search(r"^([0-9a-f]+) T " + re.escape(sym) + "$", out, re.M)
    return int(m.group(1), 16) - 0x100000000


def record(image, at):
    return {"v": 1, "kind": "hitch", "when": "2026-11-09T10:00:00Z", "uptime_s": 1.0, "ms": 500, "ended_hang": 0,
            "frames_before": 4, "verdict": "engine", "thermal": 0, "avail_mb": 1000, "image": image + "@0x100000000",
            "counters": {}, "threads": [{"name": "cpu0", "samples": 5, "engine": 5,
                                         "stacks": [{"what": "engine", "n": 5, "at": [at]}]}]}


def main():
    binary = sys.argv[1]
    text = subprocess.run(["dwarfdump", "--uuid", binary], capture_output=True, text=True, check=True).stdout
    uuid = re.search(r"UUID: ([0-9A-Fa-f-]+)", text).group(1).upper()
    run, main_at = offset(binary, "_hv_run"), offset(binary, "_main")
    with tempfile.TemporaryDirectory() as d:
        log = os.path.join(d, "henge-freezes.jsonl")
        with open(log, "w") as f:
            f.write(json.dumps(record(uuid, f"app+0x{run + 8:x} hv_run+0x8")) + "\n")
            f.write(json.dumps(record("00000000-1111-2222-3333-444444444444", f"app+0x{main_at + 8:x} hv_run+0x8")) + "\n")
            f.write(json.dumps(record(uuid, "app+0x7ffff000 hv_run+0x10")) + "\n")
        out = subprocess.run([sys.executable, REPORT, log, "--binary", binary, "--all"], capture_output=True, text=True).stdout
    causes = out.split("Causes, worst first", 1)[-1].splitlines()[1:]
    causes = [c for c in causes if "engine:" in c]
    fails = []
    if len(causes) != 3:
        fails.append(f"expected 3 causes, got {len(causes)}")
    for c in causes:
        if "hv_run" not in c:
            fails.append(f"a cause names something other than hv_run: {c.strip()}")
    if not any("hv_run (" in c for c in causes):
        fails.append("the record of the binary's own build was not named through atos (file and line)")
    if "from builds no --binary is" not in out:
        fails.append("the report doesn't say which records stay unnamed")
    if fails:
        print(out)
        for x in fails:
            print("FAIL " + x)
        return 1
    print("test_freeze_report: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
