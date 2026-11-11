// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * henge.log's queue and writer thread (henge-logq.h says why).
 *
 * The queue is one ring of bytes, taken at start, with two counters that only grow: `head`, the bytes ever queued,
 * and `tail`, the bytes ever written. What is waiting is [tail, head). Producers append under `mu` and publish by
 * storing `head` after the copy; the one writer thread reads [tail, head) without the lock and stores `tail` after
 * each write(2). A producer only fills space below tail + CAP, so it never touches bytes not yet written.
 *
 * The fatal-signal handler takes no lock: it reads `tail` and `head` and writes what lies between. A line a producer
 * was still copying is not in it (head moves after the copy); a write the writer had in flight may come out twice.
 * Twice is better than never: those are the lines that say why the process died.
 */
#include "henge-logq.h"

#include <errno.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#define CAP ((uint64_t)1 << 20) /* 1 MiB, some 10,000 lines: seconds of the busiest boot logging */

static char *ring;
static int log_fd = -1;
static int native_fd = -1;
static _Atomic uint64_t head, tail;
static _Atomic int crashed; /* the fatal handler has drained: the writer writes no more, pushes write directly */
static _Atomic uint64_t drained_to; /* how far the fatal handler wrote from the queue; 0 until it has */
static _Atomic int crash_done;      /* the first fatal handler has written everything it will */
static pthread_t crash_thread;      /* which thread that is (set before crash_done could be awaited) */
static _Atomic uint64_t lost_unmarked; /* bytes the writer gave up on (the file could not be written), not yet said */
static _Atomic int held;    /* tests */
static _Atomic uint64_t dropped_total;
static uint64_t dropped_unmarked; /* under mu: dropped since the last "lines dropped" line */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t more = PTHREAD_COND_INITIALIZER; /* to the writer: something queued */
static pthread_cond_t done = PTHREAD_COND_INITIALIZER; /* from the writer: tail moved */
static int started, threaded; /* under mu */
static pthread_t writer;

/* Taken before a signal handler could need them: nothing in one may allocate. */
static char crash_buf[8192];
static void *crash_frames[64];

static void write_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        p += w;
        n -= (size_t)w;
    }
}

static void copy_in(const char *p, size_t n) {
    uint64_t h = atomic_load_explicit(&head, memory_order_relaxed);
    size_t off = (size_t)(h % CAP), first = n < CAP - off ? n : (size_t)(CAP - off);
    memcpy(ring + off, p, first);
    memcpy(ring, p + first, n - first);
    atomic_store_explicit(&head, h + n, memory_order_release);
}

static void *write_loop(void *arg) {
    (void)arg;
#ifdef __APPLE__
    pthread_setname_np("com.henge.log.writer");
    /* As the log reader and the guest's CPUs: below the UI, and not utility, whose I/O iOS throttles. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
    pthread_mutex_lock(&mu);
    for (;;) {
        while ((atomic_load(&head) == atomic_load(&tail) || atomic_load(&held)) && !atomic_load(&crashed))
            pthread_cond_wait(&more, &mu);
        if (atomic_load(&crashed)) break;
        uint64_t h = atomic_load_explicit(&head, memory_order_acquire), t = atomic_load(&tail);
        pthread_mutex_unlock(&mu);
        while (t < h && !atomic_load(&crashed)) {
            size_t off = (size_t)(t % CAP), n = (size_t)(h - t < CAP - off ? h - t : CAP - off);
            ssize_t w = write(log_fd, ring + off, n);
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) atomic_fetch_add(&lost_unmarked, h - t); /* a failing file (full storage): give the bytes */
            t = w > 0 ? t + (uint64_t)w : h;                      /* up rather than spin, and say so (push) */
            atomic_store_explicit(&tail, t, memory_order_release);
        }
        pthread_mutex_lock(&mu);
        pthread_cond_broadcast(&done);
    }
    pthread_cond_broadcast(&done);
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* exit() waits a moment for the queue, and no fsync: what is in the kernel survives the process. It must be quick:
   Henge's self-restart (Relaunch.swift) needs this process gone before the app it opened calls it back, about 1 s. */
static void at_exit(void) { henge_logq_flush(0.3); }

int henge_logq_start(int fd) {
    pthread_mutex_lock(&mu);
    if (started) {
        pthread_mutex_unlock(&mu);
        return threaded ? 0 : -1;
    }
    started = 1;
    log_fd = fd;
    ring = malloc(CAP);
    if (ring) {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        threaded = pthread_create(&writer, &a, write_loop, NULL) == 0;
        pthread_attr_destroy(&a);
    }
    pthread_mutex_unlock(&mu);
    atexit(at_exit);
    return threaded ? 0 : -1;
}

int henge_logq_push(const char *p, size_t n) {
    if (!n) return 0;
    pthread_mutex_lock(&mu);
    /* Not started, or no thread could be had: as before, straight to the file. And once the fatal handler has
       drained the queue: the log reader may mirror QEMU's last words a moment after, and nothing writes the queue
       now. */
    if (!threaded || atomic_load(&crashed)) {
        if (log_fd >= 0) write_all(log_fd, p, n);
        pthread_mutex_unlock(&mu);
        return 0;
    }
    char marker[200];
    size_t m = 0;
    uint64_t lost = atomic_load(&lost_unmarked);
    if (dropped_unmarked || lost) {
        int k = snprintf(marker, sizeof marker, "[log]%s%.0llu%s%s%.0llu%s\n",
                         dropped_unmarked ? " " : "", (unsigned long long)dropped_unmarked,
                         dropped_unmarked ? " lines dropped here: the log file fell behind" : "",
                         lost ? " " : "", (unsigned long long)lost,
                         lost ? " bytes lost here: henge.log could not be written" : "");
        m = k > 0 && (size_t)k < sizeof marker ? (size_t)k : 0;
    }
    /* A line longer than a quarter of the queue (none is) is cut there rather than never fitting. */
    size_t keep = n > CAP / 4 ? (size_t)(CAP / 4) - 1 : n, cut = keep < n;
    uint64_t h = atomic_load_explicit(&head, memory_order_relaxed), was_empty = h == atomic_load(&tail);
    uint64_t room = CAP - (h - atomic_load(&tail));
    if (keep + cut + m > room) {
        dropped_unmarked++;
        atomic_fetch_add(&dropped_total, 1);
        pthread_mutex_unlock(&mu);
        return -1;
    }
    if (m) {
        copy_in(marker, m);
        dropped_unmarked = 0;
        atomic_fetch_sub(&lost_unmarked, lost);
    }
    copy_in(p, keep);
    if (cut) copy_in("\n", 1);
    pthread_mutex_unlock(&mu);
    /* Only an idle writer needs waking: a busy one looks at head again, under the lock, before it waits. */
    if (was_empty) pthread_cond_signal(&more);
    return 0;
}

int henge_logq_flush(double timeout_s) {
    struct timeval now;
    gettimeofday(&now, NULL);
    double end = (double)now.tv_sec + now.tv_usec / 1e6 + (timeout_s > 0 ? timeout_s : 0);
    struct timespec until = {.tv_sec = (time_t)end, .tv_nsec = (long)((end - (double)(time_t)end) * 1e9)};
    /* The lock within the timeout too: exit() must never hang here, whatever another thread was doing. */
    while (pthread_mutex_trylock(&mu) != 0) {
        gettimeofday(&now, NULL);
        if ((double)now.tv_sec + now.tv_usec / 1e6 >= end) return 0;
        usleep(1000);
    }
    if (!threaded || pthread_equal(pthread_self(), writer)) {
        pthread_mutex_unlock(&mu);
        return !threaded;
    }
    uint64_t target = atomic_load(&head);
    pthread_cond_signal(&more);
    while (atomic_load(&tail) < target && !atomic_load(&crashed))
        if (pthread_cond_timedwait(&done, &mu, &until) == ETIMEDOUT) break;
    int ok = atomic_load(&tail) >= target;
    pthread_mutex_unlock(&mu);
    return ok;
}

void henge_logq_sync(double timeout_s) {
    henge_logq_flush(timeout_s);
    if (log_fd >= 0) fsync(log_fd);
}

uint64_t henge_logq_dropped(void) { return atomic_load(&dropped_total); }

uint64_t henge_logq_pending(void) {
    uint64_t t = atomic_load(&tail); /* first: tail never passes head, so this cannot go below zero */
    return atomic_load(&head) - t;
}

void henge_logq_test_hold(int hold) {
    pthread_mutex_lock(&mu);
    atomic_store(&held, hold);
    pthread_cond_signal(&more);
    pthread_mutex_unlock(&mu);
}

/* ---- the crash path: async-signal-safe only (write, read, fsync, backtrace, atomics; no locks, no allocation) ---- */

static void drain_from(uint64_t t) {
    uint64_t h = atomic_load_explicit(&head, memory_order_acquire);
    while (t < h) {
        size_t off = (size_t)(t % CAP), n = (size_t)(h - t < CAP - off ? h - t : CAP - off);
        write_all(log_fd, ring + off, n);
        t += n;
    }
    atomic_store(&drained_to, h);
}

static void put(const char *s) { write_all(log_fd, s, strlen(s)); }

static void put_u64(uint64_t v) {
    char num[24], *d = num + sizeof num;
    *--d = 0;
    do *--d = (char)('0' + v % 10); while ((v /= 10) && d > num);
    put(d);
}

void henge_logq_crash_write(int sig) {
    pthread_t self = pthread_self();
    if (atomic_exchange(&crashed, 1)) {
        /* Another thread is dying too and is writing the queue out. Its raise() ends the process; ours must not
           end it first, mid-write: wait for it (at most 3 s), then add this thread's marker and backtrace after.
           Not when it is this thread, faulting again inside its own handler: nothing would set the flag. */
        struct timespec step = {0, 10 * 1000 * 1000};
        if (!pthread_equal(self, crash_thread))
            for (int i = 0; i < 300 && !atomic_load(&crash_done); i++) nanosleep(&step, NULL);
    } else {
        crash_thread = self;
        /* What was queued came before the signal. */
        if (ring && log_fd >= 0) drain_from(atomic_load_explicit(&tail, memory_order_acquire));
    }
    if (log_fd < 0) {
        atomic_store(&crash_done, 1);
        return;
    }
    put("\n*** FATAL SIGNAL ");
    put_u64(sig < 0 ? 0 : (uint64_t)sig);
    put(" -- process is dying ***\n");
    /* A gap the queue had not marked yet (no line came after it to carry the marker): said here instead. */
    uint64_t dropped = atomic_load(&dropped_total), lost = atomic_load(&lost_unmarked);
    if (dropped) {
        put("[log] ");
        put_u64(dropped);
        put(" lines were dropped in all since start: the log file fell behind\n");
    }
    if (lost) {
        put("[log] ");
        put_u64(lost);
        put(" bytes lost since the last such line: henge.log could not be written\n");
    }
    /* What native code wrote to stderr (a file) that the tailer has not mirrored yet: QEMU's assertion text, an
       error_report. Reading a file cannot block; what the tailer has not reached is still there at its offset. The
       buffer is taken up front: abort() can come with the allocator's lock held, and a malloc here deadlocked once. */
    if (native_fd >= 0)
        for (;;) {
            ssize_t n = read(native_fd, crash_buf, sizeof crash_buf);
            if (n <= 0) break;
            write_all(log_fd, crash_buf, (size_t)n);
        }
    /* Where it died. backtrace_symbols_fd writes straight to a descriptor, and is documented async-signal-safe. */
    put("\n--- backtrace ---\n");
    int frames = backtrace(crash_frames, 64);
    backtrace_symbols_fd(crash_frames, frames, log_fd);
    /* A push that was copying in as the queue was drained: its line, late rather than lost. */
    uint64_t upto = atomic_load(&drained_to); /* 0: nothing was ever queued (from 0 would rewrite old lines) */
    if (ring && upto) drain_from(upto);
    fsync(log_fd);
    atomic_store(&crash_done, 1);
}

static void on_fatal(int sig) {
    henge_logq_crash_write(sig);
    signal(sig, SIG_DFL);
    raise(sig);
}

void henge_logq_install_crash_handlers(int fd) {
    native_fd = fd;
    /* Not SIGTRAP or SIGBUS: the JIT trap guard (henge-ios-jit.c) steps over an unserviced brk with them. SIGPIPE is
       ignored (HengeLog.start). */
    static const int fatal[] = {SIGSEGV, SIGABRT, SIGILL, SIGFPE, SIGSYS};
    for (size_t i = 0; i < sizeof fatal / sizeof *fatal; i++) signal(fatal[i], on_fatal);
}
