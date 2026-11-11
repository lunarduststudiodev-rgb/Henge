#!/usr/bin/env python3
"""What the crash handlers can call, checked on the built vm.o (Makefile: ios-lib, check-signal-safe, make test).

on_ill (SIGILL, SIGBUS, SIGSEGV) and on_stop (SIGTRAP, SIGABRT) in host/vm.c run in a signal handler on a thread
that crashed or stopped, maybe while it or another thread held stderr's lock, the malloc lock or the device lock.
Anything they reach outside vm.o must be safe there: stdio would block for good and the crash would become a hang
that never reaches the prior handler. They write through sig_say (host/sig_say.h), which formats on the stack and
calls write(2). A call through a pointer could go anywhere, so it fails the check too (a tail call, br, as well as
blr), except where a handler is listed in CHAINS: on_stop passes the signal on to the handler that had it before.

Usage: check_signal_safe.py build/vm.o
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from check_mmu_calls import disassemble  # noqa: E402

ROOTS = ["_on_ill", "_on_stop"]
# Handlers that call through a pointer by design, and to what. Only the handler itself: a function it reaches that
# calls through a pointer still fails.
CHAINS = {
    "_on_stop": "the handler that had SIGTRAP / SIGABRT before (prior_trap, prior_abrt): passing the signal on is "
                "what it is for (the JIT's trap guard, the log's abort writer), and that handler's safety is its own",
}
ALLOWED = {
    "_write": "async-signal-safe (POSIX)",
    "_sigaction": "async-signal-safe (POSIX)",
    "_raise": "async-signal-safe (POSIX)",
    "___stack_chk_fail": "the stack protector on a smashed stack: abort(), async-signal-safe",
    "_hv_jit_sigill": "core/dispatch.c: reads the code cache's tables and the faulting context, takes no lock and calls nothing that does",
    "_hv_pvg_metal_crash_note": "devices/pvg_metal.m: an atomic load, pthread_self (a register read), strnlen and write(2); no lock",
}


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    funcs = disassemble(sys.argv[1])
    for root in ROOTS:
        if root not in funcs:
            print(f"check_signal_safe: {root} not in {sys.argv[1]}", file=sys.stderr)
            return 2
    bad, said = [], []
    for root in ROOTS:
        seen, todo = set(), [(root, [root])]
        while todo:
            fn, path = todo.pop()
            if fn in seen:
                continue
            seen.add(fn)
            f = funcs.get(fn)
            if f is None:
                if fn not in ALLOWED:
                    bad.append(" -> ".join(path))
                continue
            if f["tlv"]:
                bad.append(" -> ".join(path) + " (reads a thread-local)")
            if f["indirect"] and not (fn == root and fn in CHAINS):
                bad.append(" -> ".join(path) + " (calls through a pointer)")
            for c in sorted(f["calls"] | f["ext"]):
                todo.append((c, path + [c]))
        reached = sorted(f.lstrip("_") for f in seen if f in ALLOWED)
        chain = " and the prior handler" if root in CHAINS and funcs[root]["indirect"] else ""
        said.append(f"{root.lstrip('_')}: {', '.join(reached)}{chain}")
    if bad:
        print("check_signal_safe: a crash handler reaches calls that may take a lock:", file=sys.stderr)
        for b in sorted(set(bad)):
            print("  " + b, file=sys.stderr)
        return 1
    print("check_signal_safe: outside vm.o, " + "; ".join(said))
    return 0


if __name__ == "__main__":
    sys.exit(main())
