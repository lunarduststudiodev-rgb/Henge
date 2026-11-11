#!/usr/bin/env python3
"""Ranks what made the Mac stall, from the freeze recorder's log (host/stallmon.c).

    tools/freeze_report.py henge-freezes.jsonl [more files or folders] [--binary Henge.app/Henge] [--all]

The log comes from the iPad: Settings > Share Freeze Log, or Files > Henge > henge-freezes.jsonl. Henge's own code
appears in it as offsets ("app+0x1a2b3 name+0x..": the name is what the device itself found there). Each record
says which build wrote it (its image UUID); the offsets of the records from a --binary's build are named with atos
(file and line too), and every other record keeps the device's own names, which are right for its build. Give
--binary once for each build in the log (by default the one the last ios/scripts/package_ipa.sh built).
Each stall's cause is the busiest thing during it:
  guest    macOS itself was busy running translated code (the translator's and the MMU's speed)
  guest_spin  macOS's CPUs waited on each other's spinlocks (in place, in WFE): the guest's own contention
  engine   Henge's own code was busy (named: translating, the MMU's slow path, a device)
  waiting  a Mac CPU sat blocked in a call to the kernel (a lock, the disk, the GPU, another CPU): usually a design
           problem, not slow code
  fault    a Mac CPU sat in page faults (guest memory paged out or compressed): memory pressure, not a lock
  app      Henge's own main thread (touch and keyboard handling) was stuck
  graphics the Mac's CPUs were idle while its graphics work was held up (the record's "gfx" says where: commands
           queued for the Metal worker, the one it runs, GPU work with none finished, a wait on another channel, or
           the presenter waiting for a drawable from the screen)
  idle     nothing in the Mac was busy, the graphics side included: the press changed nothing on screen
  starved  the recorder itself got no time while Henge's threads ran (no samples): the device was overloaded
"""
import argparse
import collections
import json
import os
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BINARY = os.path.join(HERE, "..", "ios", "build", "DerivedData", "Build", "Products", "Release-iphoneos",
                              "Henge.app", "Henge")
LOAD = 0x100000000
APP = re.compile(r"app\+0x([0-9a-f]+)")
CPU_KINDS = ("guest_app", "guest_kernel", "guest_spin", "engine", "waiting", "fault")
MEANING = {
    "guest": "macOS itself was busy in translated code: faster translated code (translator, MMU fast path) helps",
    "engine": "Henge's own code was busy: speed up or avoid the named function",
    "guest_spin": "macOS's CPUs waited on each other's spinlocks: the guest's contention (or a CPU of it kept off its core)",
    "waiting": "a Mac CPU sat blocked on the named thing: change what it waits for, not how fast it runs",
    "fault": "a Mac CPU sat in page faults on the named access: memory pressure (compressed or paged-out guest RAM)",
    "app": "Henge's main thread was stuck: move the named work off the main thread",
    "graphics": "the Mac waited on its graphics work: speed up or unblock the named stage",
    "idle": "nothing in the Mac was busy, its graphics included: not a freeze, or before graphics stages were recorded",
    "starved": "the recorder got no time while Henge ran: the device was overloaded (no samples to say by what)",
}


def read_records(paths):
    files = []
    for p in paths:
        if os.path.isdir(p):
            files += [os.path.join(p, f) for f in sorted(os.listdir(p)) if f.startswith("henge-freezes") and f.endswith(".jsonl")]
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


def binary_uuids(binary):
    try:
        text = subprocess.run(["dwarfdump", "--uuid", binary], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return set()
    return {u.upper() for u in re.findall(r"UUID: ([0-9A-Fa-f-]+)", text)}


def record_uuid(r):
    return r.get("image", "").split("@")[0].upper()


class Symbols:
    """Names "app+0x..." offsets, each with the binary of its own record's build. An offset in a record from any
    other build keeps the recorder's label: naming it with another build's binary gives some other function."""

    def __init__(self):
        self.by_uuid = {}  # image UUID -> {offset: name}

    def name(self, label, r):
        m = APP.search(label)
        names = self.by_uuid.get(record_uuid(r)) if m else None
        if names is not None and int(m.group(1), 16) in names:
            return names[int(m.group(1), 16)]
        return label


def symbolizer(records, binaries):
    """Symbols for the records whose build is one of the binaries, and notes on what stays unnamed."""
    syms, notes = Symbols(), []
    images = {record_uuid(r) for r in records} - {"", "?"}
    for binary in binaries:
        if not binary or not os.path.exists(binary):
            notes.append(f"no Henge binary at {binary} (--binary)")
            continue
        have = binary_uuids(binary)
        if not have:
            notes.append(f"couldn't read the build UUID of {binary}: not used")
            continue
        mine = [r for r in records if record_uuid(r) in have]
        if not mine:
            notes.append(f"{binary} is not the build of any record")
            continue
        offsets = sorted({int(m, 16) for r in mine for m in APP.findall(json.dumps(r))})
        names = {}
        for i in range(0, len(offsets), 500):
            chunk = offsets[i:i + 500]
            try:
                res = subprocess.run(["atos", "-o", binary, "-arch", "arm64", "-l", hex(LOAD)] + [hex(LOAD + o) for o in chunk],
                                     capture_output=True, text=True, check=True).stdout.splitlines()
            except (OSError, subprocess.CalledProcessError):
                notes.append(f"atos failed on {binary}")
                names = None
                break
            for o, name in zip(chunk, res):
                name = re.sub(r" \(in [^)]*\)", "", name).strip()
                if not re.fullmatch(r"0x[0-9a-fA-F]+", name):  # atos gives the bare address where it has no name:
                    names[o] = name                            # keep the device's label there
        if names is not None:
            for u in have & images:
                syms.by_uuid[u] = names
    rest = [r for r in records if record_uuid(r) not in syms.by_uuid]
    if rest:
        builds = sorted({record_uuid(r) or "unknown" for r in rest})
        notes.append(f"{len(rest)} of {len(records)} records are from builds no --binary is ({', '.join(builds)}): "
                     "showing the names the device gave")
    return syms, "; ".join(notes)


def first_app_frame(stack, syms, r):
    for a in stack.get("at", []):
        if a.startswith("app+"):
            return syms.name(a, r)
    return stack["at"][0] if stack.get("at") else "?"


def cause(r, syms):
    """What one stall comes down to: its verdict and, where it says something, the place."""
    v = r.get("verdict", "?")
    threads = r.get("threads", [])
    if v == "app":
        main = next((t for t in threads if t["name"] == "main"), None)
        top = main["stacks"][0] if main and main.get("stacks") else None
        return v, first_app_frame(top, syms, r) if top else "main thread"
    if v == "guest":
        app = sum(t.get("guest_app", 0) for t in threads if t["name"].startswith("cpu"))
        kern = sum(t.get("guest_kernel", 0) for t in threads if t["name"].startswith("cpu"))
        return v, "macOS kernel code" if kern >= app else "macOS app code"
    if v == "guest_spin":
        return v, "in-place WFE wait"
    if v == "graphics":
        gfx = r.get("gfx", {})
        worst = max(gfx, key=lambda k: gfx[k], default=None)
        return v, f"{worst[:-3]} held up {ms(gfx[worst])}" if worst and gfx[worst] else ""
    if v in ("engine", "waiting", "fault"):
        best, where = 0, "?"
        for t in threads:
            if not t["name"].startswith("cpu"):
                continue
            for s in t.get("stacks", []):
                if s["what"] == v and s["n"] > best:
                    best, where = s["n"], first_app_frame(s, syms, r)
        return v, where
    return v, ""


def ms(x):
    return f"{x / 1000:.1f} s" if x >= 1000 else f"{x:.0f} ms"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+", help="henge-freezes.jsonl files, or folders holding them")
    ap.add_argument("--binary", action="append", help="a Henge executable of a build in the log (again for more builds)")
    ap.add_argument("--all", action="store_true", help="also list the idle ones and every place, not just the top 15")
    ap.add_argument("--stacks", type=int, default=0, help="show the full stacks of the N worst stalls")
    a = ap.parse_args()

    # a hang (whole Mac or app's main thread) comes again as its end, so only the end is counted
    records = [r for r in read_records(a.logs) if r.get("kind") not in ("hang", "app_hang")]
    hangs_open = [r for r in read_records(a.logs) if r.get("kind") in ("hang", "app_hang")]
    if not records and not hangs_open:
        print("No stalls in the log.")
        return
    syms, note = symbolizer(records + hangs_open, a.binary or [DEFAULT_BINARY])
    if note:
        print(f"({note})\n")

    if not records:
        print(f"No finished stalls in the log, only {len(hangs_open)} notice(s) of a stall still going (Henge was likely closed during it).")
        return
    total = sum(r["ms"] for r in records)
    print(f"{len(records)} stalls, {ms(total)} in all, from {min(r['when'] for r in records)} to {max(r['when'] for r in records)}")
    kinds = collections.Counter(r["kind"] for r in records)
    print("  " + ", ".join(f"{k} {n}" for k, n in kinds.most_common()))
    # Older records kept the notification's tens (10 moderate, 20 heavy); newer ones 0..4.
    heat = [(lambda t: t // 10 if t >= 10 else t)(r.get("thermal", 0)) for r in records]
    hot = sum(1 for t in heat if t >= 2)
    warm = sum(1 for t in heat if t == 1)
    low = sum(1 for r in records if 0 <= r.get("avail_mb", -1) < 512)
    if hot:
        print(f"  {hot} happened while the device was hot (iOS slows it down a lot then)")
    if warm:
        print(f"  {warm} happened while the device was warm (iOS slows it down a little)")
    if low:
        print(f"  {low} happened with under 512 MB of memory left for Henge")
    unended = [r for r in hangs_open if r["kind"] == "app_hang"]
    if unended:
        print(f"  {len(unended)} times Henge's main thread was stuck for over 1.5 s")

    print("\nWhere the stalled time went:")
    by_verdict = collections.defaultdict(list)
    for r in records:
        by_verdict[r.get("verdict", "?")].append(r)
    for v, rs in sorted(by_verdict.items(), key=lambda kv: -sum(r["ms"] for r in kv[1])):
        t = sum(r["ms"] for r in rs)
        print(f"  {v:8} {100 * t / total:5.1f}%  {len(rs):4} stalls  {MEANING.get(v, '')}")

    groups = collections.defaultdict(list)
    for r in records:
        if r.get("verdict") == "idle" and not a.all:
            continue
        groups[cause(r, syms)].append(r)
    ranked = sorted(groups.items(), key=lambda kv: -sum(r["ms"] for r in kv[1]))
    print("\nCauses, worst first (total stalled time):")
    for (v, where), rs in ranked[: None if a.all else 15]:
        times = [r["ms"] for r in rs]
        c = {k: statistics.median(r["counters"].get(k, 0) for r in rs) for k in rs[0].get("counters", {})}
        busy = ", ".join(f"{k} {c[k]:g}" for k in ("translations", "block_flushes", "sync_ms", "dev_wait_ms", "tr_wait_ms")
                         if c.get(k))
        print(f"  {ms(sum(times)):>8}  {len(rs):4}x  median {ms(statistics.median(times))}, worst {ms(max(times))}  "
              f"{v}: {where or '-'}")
        if busy:
            print(f"            counters over a typical stall: {busy}")

    if a.stacks:
        print(f"\nThe {a.stacks} worst stalls:")
        for r in sorted(records, key=lambda r: -r["ms"])[: a.stacks]:
            print(f"\n  {r['when']}  {r['kind']} {ms(r['ms'])}, {r.get('verdict')}  (thermal {r.get('thermal')}, "
                  f"{r.get('avail_mb')} MB free)")
            gfx = {k[:-3]: v for k, v in r.get("gfx", {}).items() if v}
            if gfx:
                print("    graphics held up: " + ", ".join(f"{k} {ms(v)}" for k, v in gfx.items()))
            for t in r.get("threads", []):
                share = ", ".join(f"{k} {t.get(k, 0)}" for k in CPU_KINDS + ("idle",) if t.get(k))
                print(f"    {t['name']}: {t['samples']} samples: {share}")
                for s in t.get("stacks", []):
                    print(f"      {s['what']} x{s['n']}")
                    for f in s.get("at", []):
                        print(f"        {syms.name(f, r)}")


if __name__ == "__main__":
    main()
