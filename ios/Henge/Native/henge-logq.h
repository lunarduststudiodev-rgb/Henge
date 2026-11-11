// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * henge.log's writer: lines are queued in memory and written to the file by one thread of their own, so the thread
 * that logs (the main thread, mostly) never waits on the file. A write(2) to henge.log took 0.3-0.65 s on the main
 * thread on 8 Nov 2026 (freeze recorder), with the iPad's storage busy.
 *
 * What the old synchronous write gave, kept:
 *   - order: one queue, one writer; lines reach the file in the order they were queued.
 *   - a crash still leaves the last lines: the fatal-signal handler (henge_logq_install_crash_handlers) writes what
 *     is still queued straight from the queue's memory before its own marker. It takes no lock and allocates nothing.
 *   - exit(): an atexit handler waits (up to 0.3 s, no fsync: Henge's self-restart needs the process gone quickly)
     for the queue to reach the kernel. After a crash, later lines go straight to the file.
 * Not kept: a line used to be in the kernel (safe from a kill) as soon as it was logged. Now a SIGKILL (iOS's memory
 * limit, the watchdog) loses what is still queued, usually nothing, but up to a stalled write's worth when storage
 * is busy. A memory warning flushes the queue (HengeLog.swift), which narrows that for memory kills.
 * A full queue never blocks: the line is dropped and counted, and a "lines dropped" line marks the gap. A line the
 * writer had in flight as the gap began may appear once more after that marker; harmless, and rare.
 */
#ifndef HENGE_LOGQ_H
#define HENGE_LOGQ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the writer thread on `fd` (henge.log, opened by the caller and never closed). Once; later calls do nothing.
   Returns 0, or -1 if the thread or the queue's memory could not be had (then henge_logq_push writes synchronously). */
int henge_logq_start(int fd);

/* Queues `len` bytes (a whole line with its newline; one over 256 KiB is cut there). Never waits on the file. Returns 0 if queued (or written, before
   start), -1 if dropped: the queue was full. Safe from any thread; not from a signal handler. */
int henge_logq_push(const char *bytes, size_t len);

/* Waits up to `timeout_s` for everything queued before the call to be written. 1: written; 0: timed out.
   Never call from the writer thread or a signal handler. Waiting for the lock counts against the timeout. */
int henge_logq_flush(double timeout_s);

/* henge_logq_flush, then fsync. */
void henge_logq_sync(double timeout_s);

/* Lines dropped because the queue was full, since start. */
uint64_t henge_logq_dropped(void);

/* Bytes queued but not yet written. */
uint64_t henge_logq_pending(void);

/* Fatal signals (SEGV ABRT ILL FPE SYS; TRAP and BUS stay the JIT guard's): what is still queued, a marker, what
   native code wrote to `native_fd`'s file that the tailer had not reached yet (-1: none), a backtrace, fsync, then
   the signal's default action. Async-signal-safe throughout. The writer thread writes no more once it has run; a
   second thread dying meanwhile waits (up to 3 s) for the first to finish before adding its own. */
void henge_logq_install_crash_handlers(int native_fd);

/* The fatal handler's work without dying, for tests. */
void henge_logq_crash_write(int sig);

/* Tests: hold the writer (it writes nothing while held), to fill the queue or leave lines in it. */
void henge_logq_test_hold(int hold);

#ifdef __cplusplus
}
#endif

#endif
