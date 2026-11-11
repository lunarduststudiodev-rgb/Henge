#!/usr/bin/env python3
"""tools/check_signal_safe.py on made-up objects: each crash handler (on_ill, on_stop) fails the check when it,
or anything it reaches, calls a function not known to be safe in a signal handler (fprintf), calls through a
pointer (blr, or a tail call: br), or reads a thread-local; on_stop's own calls through a pointer (the prior
handler) are allowed, and a jump table's br is not a call: only clang's exact sequence (the offset read from a table
and an index, no writeback), with nothing anywhere in the object branching into it, is taken for one. A missing handler is an error. Given the built vm.o,
also: it passes, and the check walked both handlers."""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
CHECK = os.path.join(ROOT, "tools", "check_signal_safe.py")
CLEAN = """
.globl _on_ill, _on_stop
_on_ill:
    bl _write
    adr x10, 1f
    ldrb w11, [x9, x8]
    add x10, x10, x11, lsl #2
    br x10
1:  bl _helper
    b _sigaction
_helper:
    bl _write
    ret
_on_stop:
    bl _hv_pvg_metal_crash_note
    bl _sigaction
    ldr x3, [x1]
    cbz x3, 2f
    blr x3
    ldr x3, [x1, #8]
    br x3
2:  b _raise
"""
PLANTS = [  # (what, the line it goes after, the line planted, what the report must name)
    ("on_stop calls fprintf", "_on_stop:\n", "    bl _fprintf\n", "_on_stop -> _fprintf"),
    ("on_ill calls fprintf through a helper", "_helper:\n", "    bl _fprintf\n", "_on_ill -> _helper -> _fprintf"),
    ("on_ill tail-calls through a pointer", "_on_ill:\n", "    ldr x3, [x1]\n    br x3\n", "_on_ill (calls through a pointer)"),
    ("on_ill calls through a pointer", "_on_ill:\n", "    blr x3\n", "_on_ill (calls through a pointer)"),
    ("on_stop's allowance doesn't reach what it calls", "_on_stop:\n", "    bl _stop_helper\n",
     "_on_stop -> _stop_helper (calls through a pointer)"),
    ("on_stop reads a thread-local", "_on_stop:\n",
     "    adrp x0, _tv@TLVPPAGE\n    ldr x0, [x0, _tv@TLVPPAGEOFF]\n", "_on_stop (reads a thread-local)"),
]
STOP_HELPER = "_stop_helper:\n    blr x4\n    ret\n"
# A br excused as a jump table only when it is exactly clang's sequence, in a row, with nothing branching into it.
# (what, [(text, replaced by)])
TABLE_PLANTS = [
    ("the table register loaded from memory before the br",
     [("    ldrb w11, [x9, x8]\n", "    ldrb w11, [x9, x8]\n    ldr x10, [x1]\n")]),
    ("the table register overwritten by a pair load",
     [("    adr x10, 1f\n", "    adr x10, 1f\n    ldp x2, x10, [x1]\n")]),
    ("the table offset a whole register from memory",
     [("    ldrb w11, [x9, x8]\n    add x10, x10, x11, lsl #2\n", "    ldr x11, [x9]\n    add x10, x10, x11\n")]),
    ("a conditional branch into the sequence with the register loaded from memory",
     [("_on_ill:\n", "_on_ill:\n    ldr x10, [x1]\n    cbnz x0, 4f\n"), ("    br x10\n", "4:  br x10\n")]),
    ("a jump to the br from code that loaded the register",
     [("_on_ill:\n", "_on_ill:\n    cbz x0, 5f\n    ldr x10, [x1]\n    b 4f\n5:\n"), ("    br x10\n", "4:  br x10\n")]),
    ("a branch from another function into the sequence, with the register loaded from memory",
     [("    add x10, x10, x11, lsl #2\n", "6:  add x10, x10, x11, lsl #2\n"),
      ("_helper:\n    bl _write\n", "_helper:\n    bl _write\n    ldr x10, [x1]\n    mov x11, #0\n    b 6b\n")]),
    ("the table read with a pre-index writeback to the table register",
     [("    ldrb w11, [x9, x8]\n", "    ldrb w11, [x10, #0xf0]!\n")]),
    ("the table read into the table register itself",
     [("    ldrb w11, [x9, x8]\n    add x10, x10, x11, lsl #2\n", "    ldrb w10, [x9, x8]\n    add x10, x10, x10, lsl #2\n")]),
    ("the offset read from somewhere not a table and an index",
     [("    ldrb w11, [x9, x8]\n", "    ldrb w11, [x1]\n")]),
    ("the offset read with a post-index writeback",
     [("    ldrb w11, [x9, x8]\n", "    ldrb w11, [x9], #1\n")]),
    ("the offset read through the branch register, not a table",
     [("    ldrb w11, [x9, x8]\n", "    ldrb w11, [x10, x8]\n")]),
    ("the table indexed by the branch register",
     [("    ldrb w11, [x9, x8]\n", "    ldrb w11, [x9, x10]\n")]),
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
    check("safe calls, on_stop's chain to the prior handler and a jump table pass", rc == 0)
    check("both handlers are walked", "on_ill:" in out and "on_stop:" in out)
    if rc:
        print(out)
    for i, (what, after, plant, named) in enumerate(PLANTS):
        rc, out = run(CLEAN.replace(after, after + plant, 1) + STOP_HELPER, d, f"plant{i}")
        check(f"{what}: fails the check and is named", rc == 1 and named in out)
        if rc != 1 or named not in out:
            print(out)
    for i, (what, edits) in enumerate(TABLE_PLANTS):
        src = CLEAN
        for text, by in edits:
            assert src.count(text) == 1, text
            src = src.replace(text, by)
        rc, out = run(src, d, f"table{i}")
        named = "_on_ill (calls through a pointer)"
        check(f"{what}: fails the check and is named", rc == 1 and named in out)
        if rc != 1 or named not in out:
            print(out)
    rc, out = run(CLEAN.replace("ldrb w11, [x9, x8]", "ldrh w11, [x9, x8, lsl #1]"), d, "halfwords")
    check("a table of halfwords passes too", rc == 0)
    if rc:
        print(out)
    rc, out = run(CLEAN.replace("_on_stop", "_on_halt"), d, "nostop")
    check("a missing on_stop is an error", rc == 2 and "_on_stop not in" in out)

if len(sys.argv) > 1:
    r = subprocess.run([sys.executable, CHECK, sys.argv[1]], capture_output=True, text=True)
    out = r.stdout + r.stderr
    check(f"{sys.argv[1]} passes, with both handlers walked",
          r.returncode == 0 and "on_ill:" in out and "on_stop:" in out)
    if r.returncode:
        print(out)

print("test_check_signal_safe FAILED" if failures else "test_check_signal_safe ok")
sys.exit(failures != 0)
