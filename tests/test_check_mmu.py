#!/usr/bin/env python3
"""tools/check_mmu_calls.py on made-up objects: a function the thunk paths reach (hv_tlb_refill, hv_ijc_resolve)
that names a SIMD or floating-point register (vector, q, d, s, h, b; or FPSR) fails the check; general-register
code (conditional branches, ccmp with a condition, shifts) and a function they never reach do not. Given the built
mmu.o, also: it passes (mmu.c is built with -mgeneral-regs-only)."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
CHECK = os.path.join(ROOT, "tools", "check_mmu_calls.py")
CLEAN = """
.globl _hv_tlb_refill, _hv_ijc_resolve
_hv_tlb_refill:
    cmp x0, #4
    b.hs 1f
    ccmp x1, #0, #4, hs
    lsl x2, x3, #12
    bl _walk
1:  ret
_walk:
    ldr x0, [x1, #8]
    ret
_hv_ijc_resolve:
    ldrb w0, [x1]
    ret
_unreached:
    fadd d0, d1, d2
    movi.2d v0, #0
    ret
"""
DIRTY = [  # (function, the instruction planted in it): each kind of register alone
    ("_walk", "ldr q0, [x1]"),
    ("_hv_ijc_resolve", "mrs x0, fpsr"),
    ("_hv_ijc_resolve", "msr FPCR, x1"),
    ("_new", "fmov s0, w1"),
    ("_new", "fmov d0, xzr"),
    ("_new", "ldr h1, [x0]"),
    ("_new", "ldr b2, [x0]"),
    ("_new", "mov v0.16b, v1.16b"),
    ("_new", "movi.16b v3, #0"),
]

failures = 0


def check(what, ok):
    global failures
    if not ok:
        print(f"FAIL {what}")
        failures += 1


def run(src, d, name):
    s, o = os.path.join(d, name + ".s"), os.path.join(d, name + ".o")
    with open(s, "w") as f:
        f.write(src)
    subprocess.run(["xcrun", "clang", "-c", "-arch", "arm64", s, "-o", o], check=True)
    r = subprocess.run([sys.executable, CHECK, o], capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


with tempfile.TemporaryDirectory() as d:
    rc, out = run(CLEAN, d, "clean")
    check("general-register code, and SIMD only where the thunk paths never go, passes", rc == 0)
    if rc:
        print(out)
    for i, (fn, insn) in enumerate(DIRTY):
        src = CLEAN
        if fn == "_new":  # a new function, reached from hv_tlb_refill
            src = src.replace("    bl _walk\n", f"    bl _walk\n    bl {fn}\n") + f"{fn}:\n    {insn}\n    ret\n"
        else:
            src = src.replace(f"{fn}:\n", f"{fn}:\n    {insn}\n", 1)
        rc, out = run(src, d, f"dirty{i}")
        said = f"{fn} (uses a SIMD/FP register"
        check(f"{insn} in {fn} fails the check, for that", rc == 1 and said in out)
        if rc != 1 or said not in out:
            print(out)

if len(sys.argv) > 1:
    r = subprocess.run([sys.executable, CHECK, sys.argv[1]], capture_output=True, text=True)
    check(f"{sys.argv[1]} passes", r.returncode == 0)
    if r.returncode:
        print(r.stdout + r.stderr)

print("test_check_mmu FAILED" if failures else "test_check_mmu ok")
sys.exit(failures != 0)
