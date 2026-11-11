#!/usr/bin/env python3
"""tools/check_cpu_thread_fp.py on a made-up object: floating-point arithmetic, scalar or vector (fmla.4s,
scvtf.2d in objdump's Apple syntax), is found and fails the check; moves and abs, scalar or vector, are not.
Given the Mac's pvg_mac.o, also: its MMIO handlers, which run on a CPU thread, do no floating point of their own
(HV_PVG_TRACE's timing runs in trace_fp, under the host's FPCR). Given ramfile.o too: it passes the check (the balloon's
give-back, on a vCPU, logs its hold time in integers)."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SRC = """
_vector_fp:
    fmla.4s v0, v1, v2
    ret
_vector_convert:
    scvtf.2d v0, v1
    ret
_scalar_fp:
    fadd s0, s1, s2
    ret
_moves_only:
    fmov.4s v0, #1.0
    fabs.4s v1, v2
    fneg d0, d1
    fmov d2, x3
    ret
"""

failures = 0


def check(what, ok):
    global failures
    if not ok:
        print(f"FAIL {what}")
        failures += 1


with tempfile.TemporaryDirectory() as d:
    s, o = os.path.join(d, "made_up.s"), os.path.join(d, "made_up.o")
    with open(s, "w") as f:
        f.write(SRC)
    subprocess.run(["xcrun", "clang", "-c", "-arch", "arm64", s, "-o", o], check=True)
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "check_cpu_thread_fp.py"), o],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    check("the check fails on an object with unlisted floating point", r.returncode != 0)
    check("vector arithmetic (fmla.4s) is found", "_vector_fp" in out)
    check("a vector conversion (scvtf.2d) is found", "_vector_convert" in out)
    check("scalar arithmetic (fadd) is found", "_scalar_fp" in out)
    check("moves, abs and neg are not floating-point arithmetic", "_moves_only" not in out)
    if failures:
        print(out)

if len(sys.argv) > 1:
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "check_cpu_thread_fp.py"), sys.argv[1]],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    for fn in ("_gfx_read", "_gfx_write", "_iosfc_read", "_iosfc_write"):
        check(f"pvg_mac.o {fn} (a CPU thread) does no floating point outside a bracket", f" {fn}:" not in out)
    check("pvg_mac.o: the trace's timing is found where it is bracketed (trace_fp)", " _trace_fp:" in out)
    if failures:
        print(out)

if len(sys.argv) > 2:
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "check_cpu_thread_fp.py"), sys.argv[2]],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    # hv_ramfile_discard is the balloon's give-back, on a vCPU (virtio_balloon.c): its hold line is in integers
    check("ramfile.o has no floating point outside the functions listed (hv_ramfile_discard runs on a vCPU)", r.returncode == 0)
    if r.returncode != 0:
        print(out)

print("test_check_fp FAILED" if failures else "test_check_fp ok")
sys.exit(failures != 0)
