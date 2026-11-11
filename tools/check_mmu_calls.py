#!/usr/bin/env python3
"""What the thunk paths can call, checked on the built mmu.o (Makefile: check-mmu-calls; runtests via make).

hv_tlb_refill and hv_ijc_resolve run from translated code through thunks that save no SIMD register and no
FPSR (host/enter.S). mmu.c is built with -mgeneral-regs-only so that its own code leaves them alone; this checks
that it does (no function reachable from those two names a SIMD or floating-point register, or FPSR/FPCR, as a
build that lost the flag would), and the rest: nothing reachable may call out of mmu.o (libc's string and memory
routines use SIMD; getenv, calloc and the like may) or read a thread-local (the first touch of one on a thread
calls into dyld).

Allowed exits are listed below, each with why it is safe. Usage: check_mmu_calls.py build/mmu.o
"""
import re
import subprocess
import sys

# An operand naming a SIMD or floating-point register (v0.16b, q1, d2, s3, h4, b5) or the FP status/control
# registers. Operands only: b and b.cond are also branch mnemonics.
SIMD_REG = re.compile(r"(?<![\w.])(?:[vqdshb](?:[12]?[0-9]|3[01])(?![\w])|fpsr|fpcr)", re.I)

ROOTS = ["_hv_tlb_refill", "_hv_ijc_resolve"]
# Calls out of mmu.o the thunk paths may make, and why.
ALLOWED = {
    # Atomics helpers the compiler may emit for LSE-less targets (none with -mcpu=apple-a14; listed for clarity).
}
# Functions inside mmu.o that the thunk paths reach only behind a guard that keeps them off it; the guard
# is named so a change to it shows up here. hv_ijc_resolve fills the indirect-jump cache, and resets it only
# when the counts at its top say the cache is full, which the dispatcher (C, SIMD saved) does first.
# Calls inside mmu.o that the thunk paths never make, because ijc_resolve refuses first: it fails when the
# filter (ijc_keyed_count) or the user-code set (ucode_count) is one fill from a reset, so hv_ijc_fill and
# hv_ucode_add never start them over from a thunk. Each is (caller, callee); the guard is mmu.c's
# "ijc_keyed_count >= (1u << 17) || pcs->ucode_count + 1 >= ..." line in ijc_resolve.
GUARDED = {
    ("_hv_ucode_add", "_bzero"), ("_hv_ucode_add", "_hv_ret_clear"),  # ucode_clear, inlined
    ("_hv_ucode_add", "_hv_ijc_reset"), ("_hv_ucode_add", "_hv_ijc_switch"),
    ("_hv_ijc_fill", "_hv_ijc_reset"), ("_hv_ijc_fill", "_bzero"), ("_hv_ijc_fill", "_memset"),
    ("_hv_ijc_fill", "_hv_ret_clear"), ("_hv_ijc_fill", "_hv_ijc_switch"),  # hv_ucode_add inlined into it
    # hv_ijc_reset(cpu, 1) inlined into its filter reset: a kernel generation's run-out zeroes the used kernel tables
    # in machine.c (ijc_clear), as the memset above did when it was in mmu.o
    ("_hv_ijc_fill", "_hv_ijc_tables_clear"),
}
# These follow the compiler's inlining: if it moves a reset call into another function, this check fails and
# the new caller has to be looked at and added, which is the point.


def disassemble(path):
    out = subprocess.run(["xcrun", "objdump", "-d", "-r", "--no-show-raw-insn", path],
                         check=True, capture_output=True, text=True).stdout
    # The function at the start of a section is labelled ltmpN; name it after its real symbol.
    names = {}
    for line in subprocess.run(["nm", "-n", path], check=True, capture_output=True, text=True).stdout.splitlines():
        f = line.split()
        if len(f) == 3 and f[1] in "tT" and not f[2].startswith("ltmp"):
            names.setdefault(int(f[0], 16), f[2])
    funcs, cur, targets = {}, None, set()
    for line in out.splitlines():
        m = re.match(r"^([0-9a-f]+) <(.+)>:$", line)
        if m:
            cur = label = m.group(2)
            if cur.startswith("ltmp"):
                cur = names.get(int(m.group(1), 16), cur)
            funcs[cur] = {"labels": {cur, label}, "calls": set(), "ext": set(), "tlv": False, "indirect": False, "simd": None,
                          "insns": []}
            continue
        if cur is None:
            continue
        f = funcs[cur]
        m = re.match(r"^\s+[0-9a-f]+:\s+(ARM64_RELOC_\w+)\s+(\S+)", line)
        if m:
            kind, sym = m.group(1), m.group(2)
            if kind == "ARM64_RELOC_BRANCH26":
                f["ext"].add(sym)
            elif kind.startswith("ARM64_RELOC_TLVP"):
                f["tlv"] = True
            continue
        m = re.match(r"^\s+([0-9a-f]+):\s+(\S+)\s*(.*)$", line)
        if m:
            op, args = m.group(2), m.group(3)
            if op in ("bl", "b"):
                t = re.search(r"<(_[^+>]+)>", args)  # a direct call to the start of another function here
                if t and t.group(1) != cur:
                    f["calls"].add(t.group(1))
            if op in ("blr", "blraa", "blrab", "blraaz", "blrabz"):
                f["indirect"] = True
            if (op[0] == "b" or op in ("cbz", "cbnz", "tbz", "tbnz")) and (t := re.search(r"0x([0-9a-f]+)\s*<", args)):
                targets.add(int(t.group(1), 16))  # somewhere a branch lands, from any function
            f["insns"].append((int(m.group(1), 16), op, args))
            operands = re.sub(r"<[^>]*>|;.*$|//.*$", "", args)  # not a symbol's name or a comment
            if f["simd"] is None and SIMD_REG.search(operands):  # Apple's movi.2d etc. name a v register too
                f["simd"] = f"{m.group(1)}: {op} {args}".strip()
            continue
    for f in funcs.values():
        for i, (addr, op, args) in enumerate(f["insns"]):
            if op in ("br", "braa", "brab", "braaz", "brabz") and not jump_table(f, i, targets):
                f["indirect"] = True  # a tail call through a pointer
    return funcs


def jump_table(f, i, targets):
    """Is the br at f["insns"][i] the end of one of clang's jump tables, a branch inside the function and nowhere else?
    Exactly adr xN, <this function>; ldrb/ldrh wM, [xT, xI(, lsl #1)] (M not N, no writeback); add xN, xN, wM(, lsl/sxtw/
    uxtw #2); br xN, in a row, with nothing in the object (targets: every branch's) branching into the middle: entered
    there, xN could hold anything."""
    if i < 3:
        return False
    (_, op0, a0), (a1, op1, r1), (a2, op2, r2), (a3, _, r3) = f["insns"][i - 3:i + 1]
    regs = lambda a: [x.strip() for x in re.sub(r"<[^>]*>|;.*$|//.*$", "", a).split(",")]
    adr, ldr, add, br = regs(a0), regs(r1), regs(r2), regs(r3)
    t = re.search(r"<([^+>]+)(\+0x[0-9a-f]+)?>", a0)
    n = br[0]
    table = re.fullmatch(r"w(\d+), \[x(\d+), x(\d+)(, lsl #(0x)?1)?\]", re.sub(r";.*$|//.*$", "", r1).strip())
    return (op0 == "adr" and t is not None and t.group(1) in f["labels"] and adr[0] == n and n.startswith("x")
            and op1 in ("ldrb", "ldrh", "ldrsb", "ldrsh") and table is not None and "x" + table.group(1) != n
            and n[1:] not in (table.group(2), table.group(3))
            and op2 == "add" and len(add) >= 3 and add[0] == n and add[1] == n and add[2][1:] == ldr[0][1:] and add[2][:1] in "wx"
            and (len(add) == 3 or re.fullmatch(r"(lsl|sxtw|uxtw) #(0x)?2", add[3]) is not None)
            and not ({a1, a2, a3} & targets))


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    funcs = disassemble(sys.argv[1])
    for r in ROOTS:
        if r not in funcs:
            print(f"check_mmu_calls: {r} not in {sys.argv[1]}", file=sys.stderr)
            return 2
    bad = []
    for root in ROOTS:
        seen, todo = set(), [(root, [root])]
        while todo:
            fn, path = todo.pop()
            if fn in seen:
                continue
            seen.add(fn)
            f = funcs.get(fn)
            if f is None:  # outside mmu.o
                if fn not in ALLOWED:
                    bad.append(" -> ".join(path))
                continue
            if f["tlv"]:
                bad.append(" -> ".join(path) + " (reads a thread-local)")
            if f["indirect"]:
                bad.append(" -> ".join(path) + " (calls through a pointer)")
            if f["simd"]:
                bad.append(" -> ".join(path) + f" (uses a SIMD/FP register: {f['simd']})")
            for c in sorted(f["calls"] | f["ext"]):
                if (fn, c) not in GUARDED:
                    todo.append((c, path + [c]))
    if bad:
        print("check_mmu_calls: the no-SIMD-save thunk paths reach SIMD/FP registers or code outside mmu.o:",
              file=sys.stderr)
        for b in sorted(set(bad)):
            print("  " + b, file=sys.stderr)
        return 1
    print("check_mmu_calls: hv_tlb_refill and hv_ijc_resolve stay inside mmu.o and use no SIMD/FP register")
    return 0


if __name__ == "__main__":
    sys.exit(main())
