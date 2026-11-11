/* Suspend, Phase 1 (core/snapshot.h): a machine saved at any moment and loaded back carries on exactly
   as if it had never stopped.

   The guest (snapshot_programs.S) runs two CPUs with the MMU on, EL0 code, timer interrupts, SGIs
   between the CPUs, exclusives and address-space switches. Its result is deterministic, so a reference
   run says what every run must end with. Then, at 1,000 random moments: pause, save, stop the CPU
   threads, wreck everything a snapshot is meant to hold (registers, GIC, RAM, the clock, translated
   code), load the snapshot and let it finish. Each must end exactly as the reference did, and the
   guest's clock must never jump by the time the machine spent stopped.

   Also: a snapshot loaded in a new process, and damaged, cut short, half-written or mismatched
   snapshots refused. */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>

#include "dispatch.h"
#include "machine.h"
#include "snapshot.h"
#include "snapshot_guest.h"
#include "sysreg.h"
#include "vmapple.h"

extern char **environ;
extern const char snapshot_programs_begin[], snapshot_programs_finish[], snap_cpu0[], snap_done[];

#define RAM_SIZE (16ull << 20)
#define CODE VMAPPLE_MEM
#define D (VMAPPLE_MEM + 0x200000)
#define RO_PA 0x60000000ull /* a read-only region the guest never maps: checked, not loaded */
#define RO_SIZE (64u << 10)
#define NCPUS 2
#define MOMENTS 1000

static HvMachine m;
static HvCpu cpus[NCPUS];
static HvVmapple board;
static uint8_t *ro_mem;
static int checks, failures;

static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: %llx, expected %llx\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static uint8_t *gram(uint64_t pa) { return board.ram + (pa - VMAPPLE_MEM); }
static uint64_t code_pa(const char *sym) { return CODE + (uint64_t)(sym - snapshot_programs_begin); }
static double ms(uint64_t ticks) { return ticks / 24000.0; }

/* ---- The machine and its CPU threads ---- */

static pthread_mutex_t kick_mu = PTHREAD_MUTEX_INITIALIZER;
static int kicker_on;
static thread_t main_port, kick_port; /* for the blocked-CPU captures */
static void *kicker(void *unused) {
    (void)unused;
    kick_port = pthread_mach_thread_np(pthread_self());
    for (;;) {
        usleep(1000);
        pthread_mutex_lock(&kick_mu);
        if (kicker_on) hv_machine_kick_all(&m);
        pthread_mutex_unlock(&kick_mu);
    }
    return NULL;
}

static pthread_t threads[NCPUS];
static int running, results[NCPUS], finished, thread_done[NCPUS]; /* thread_done: under fin_mu, set before it ends */
static pthread_mutex_t fin_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fin_cv = PTHREAD_COND_INITIALIZER;

/* ---- What the host took from the guest ----

   The guest reads its clock every 256 loop iterations; the step between two reads is time it saw go
   by. Steps it must never see: the time the machine stood stopped for a save and load, and the engine
   stalling (a long flush, a lock held too long, a CPU thread busy outside the guest). Steps it may see
   on a busy Mac: time its CPU thread was ready to run but had no core. So a sampler at real-time
   priority looks at each CPU thread every 0.25 ms: whether it is runnable (Mach's RUNNING is on a core
   or waiting for one), whether its CPU is parked (asleep in WFI, off, at a barrier: not blocked on a
   lock), and the CPU time it got. Over a step, runnable time less CPU time is time it waited for a
   core. Time a CPU was parked while the other's thread waited for a core (CPU 1 waits in WFI for CPU
   0's SGI) is excused too, and so is time it was blocked on a mutex the other CPU's thread holds while
   that thread waited for a core (the kernel names the owner: waits_on_peer). A CPU blocked on a lock
   any other thread holds is not, nor on the other CPU's while that one runs. Everything else in a step
   is unexplained, and a step with more than 25 ms of it fails, with the reason (and, for a CPU blocked
   in the engine over 25 ms, every thread's stack then).

   macOS brings a thread's CPU time up to date only when the thread enters the kernel or leaves its
   core, so CPU time read mid-step can be ~10 ms behind, and time on a core would count as time waiting
   for one. So each CPU thread enters the kernel at every tick (about every millisecond, at the kicks),
   and a step is judged only once both CPUs' CPU time is known past its end: their threads seen blocked,
   or their CPU time seen to move. Between two such points a thread can only have waited for a core and
   then run (leaving a core would have been one), so its waiting is the start of that stretch: the other
   CPU's parked time is matched against it interval by interval, never against waiting in another part
   of the step. The engine's spin-waits give their core away with sched_yield, which looks the same as
   waiting for one: this test's sched_yield (the engine's calls bind to it) times them, and that time is
   not excused. A gap in the samples counts no more than 0.5 ms runnable: a gap can only make a step look
   worse.

   The record is kept as a function of guest time, so a step that spans a save and load is measured
   too: the guest's clock stands still while the machine is stopped, and so does the record. Stopped
   time leaking into the guest's clock is unexplained. */

#define STEP_LIMIT_MS 25
#define TICKS_MS 24000ull
#define SMP_PERIOD (TICKS_MS / 4)
#define HIST (1u << 16) /* 16 s of samples */
#define PEND 512        /* steps waiting for both CPUs' CPU time, per CPU */
_Static_assert(NCPUS == 2, "the peer excuse is written for two CPUs");

/* Per CPU, cumulative host ticks: runnable, on a core, parked while the other waited for a core (known only once
   the other's CPU time is: added to the records then), excuse carried in from the process that saved (the
   new-process test), and time counted (not while the guest's clock is held). */
/* y: in sched_yield; gap: unsampled; qw: its own waiting, placed by stretch; settled: every CPU stopped for a pause
   while the guest's clock still ran (the machine settled, the pausing thread not yet back) */
typedef struct Took { uint64_t run, cpu, pw, qw, carry, t, y, gap, settled; } Took;
/* At guest time g; over the interval to it, each CPU's runnable time, its parked time while the other was
   runnable, and its time in sched_yield. */
typedef struct Rec { uint64_t g; Took t[NCPUS]; uint64_t ir[NCPUS], ip[NCPUS], iy[NCPUS]; } Rec;
/* A step taken apart: on a core, waiting for one (with what was carried in), blocked, and of that, parked while the
   other CPU waited for a core (excused); un: the rest of the step, unexplained. */
typedef struct Split { uint64_t step, cpu, queued, yielded, blocked, peer, un, gap, settled; } Split;
typedef struct Flag { int cpu, across; uint64_t from; Split s; } Flag; /* from: guest time; across: spans a stop */
typedef struct Pend { uint64_t a, b; int have[NCPUS]; Took e[NCPUS]; } Pend; /* e: each CPU's record at the first
                                                                                 point its CPU time was known past b */

static pthread_mutex_t smp_mu = PTHREAD_MUTEX_INITIALIZER; /* cur and the ports */
static thread_t cpu_port[NCPUS];
static int ports_new;
static uint64_t smp_looks; /* the sampler's looks with the machine running, each with its steps judged */
static int smp_epoch, smp_ack;              /* odd: the machine is stopped (the guest's clock stands) */
static uint64_t smp_tp;                     /* the guest's clock while stopped */
static uint64_t smp_held_bias;              /* hv_time_bias while the machine is still held at resume_machine */
static uint64_t stops_g[64];                /* the guest times of the last stops */
static uint64_t nstops;
static uint64_t stop_excuse[NCPUS];         /* at the last stop: the excuse since each CPU's last read */
static Rec hist[HIST];
static uint64_t hist_n, hist_lo; /* records taken, and the oldest still kept (a stop's cut-back never lowers it) */
static Took cur[NCPUS];
static int committed[NCPUS];                /* CPU c's CPU time is known up to guest time commit_g[c], and then */
static uint64_t commit_g[NCPUS];            /* its record was commit_t[c] */
static Took commit_t[NCPUS];
static Pend pend[NCPUS][PEND];
static uint64_t pend_head[NCPUS], pend_tail[NCPUS];
static uint64_t last_p[NCPUS];
static Flag flags[256];
static int nflags, flags_shown, unmeasured, unsettled;
static uint64_t unm[8][6]; /* the first unmeasured steps: CPU, from, to, the oldest and newest record, records taken */
static uint64_t worst_unexplained, smp_gaps, smp_worst_gap;
static Flag worst;

static uint64_t p_now(int c) { return __atomic_load_n((uint64_t *)gram(D + 0x40020 + 0x10 * (uint64_t)c), __ATOMIC_RELAXED); }
static uint64_t sub(uint64_t a, uint64_t b) { return a > b ? a - b : 0; }

/* Step (guest ticks) of CPU c taken apart, from record a to record e (its CPU time known there), q (the record at
   the step's end, its stretches closed past it; NULL: e is the end) and pw_end (its parked time to the step's end);
   returns the excuse. Waiting is runnable time less CPU time to e; when e is past the step's end, no more than the
   stretches placed before it (q): waiting for a core after the step's last read is not the step's. */
static uint64_t split(const Took *a, const Took *e, const Took *q, uint64_t pw_end, uint64_t step, Split *s) {
    s->step = step;
    s->cpu = sub(e->cpu, a->cpu);
    s->yielded = sub(e->y, a->y);
    s->queued = sub(sub(sub(e->run, a->run), s->cpu), s->yielded) + sub(e->carry, a->carry);
    if (q) {
        uint64_t placed = sub(q->qw, a->qw) + sub(q->carry, a->carry);
        if (placed < s->queued) s->queued = placed;
    }
    s->blocked = sub(sub(e->t, a->t), sub(e->run, a->run));
    s->peer = sub(pw_end, a->pw);
    s->gap = sub(e->gap, a->gap);
    s->settled = sub((q ? q : e)->settled, a->settled);
    uint64_t ex = s->queued + s->peer;
    s->un = sub(step, ex);
    return ex;
}

static void hist_push(uint64_t g, const Took *t, const uint64_t *ir, const uint64_t *ip, const uint64_t *iy) {
    if (hist_n > hist_lo && g < hist[(hist_n - 1) % HIST].g) g = hist[(hist_n - 1) % HIST].g;
    if (hist_n - hist_lo == HIST) hist_lo++; /* its slot is taken over */
    Rec *r = &hist[hist_n % HIST];
    r->g = g;
    memcpy(r->t, t, sizeof r->t);
    for (int c = 0; c < NCPUS; c++) r->ir[c] = ir ? ir[c] : 0, r->ip[c] = ip ? ip[c] : 0, r->iy[c] = iy ? iy[c] : 0;
    hist_n++;
}

/* The record at or before guest time g (no later one: the step's start must not be cut short); 0 when the
   history no longer reaches back. */
static int took_at(uint64_t g, Took *out) {
    uint64_t lo = hist_lo, hi = hist_n;
    if (lo == hi) return 0;
    if (g < hist[lo % HIST].g) {
        if (hist_lo) return 0;
        memcpy(out, hist[lo % HIST].t, sizeof hist[0].t);
        return 1;
    }
    while (hi - lo > 1) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (hist[mid % HIST].g <= g) lo = mid;
        else hi = mid;
    }
    memcpy(out, hist[lo % HIST].t, sizeof hist[0].t);
    return 1;
}

static uint64_t plant_at[256][3]; /* planted stalls (below): guest begin, guest end, host begin */
static uint64_t plant_un[256], plant_ex[256]; /* the most unexplained of any step over each, and that step's excuse */
static int plant_cpu[256];                    /* the CPU each was planted on (its steps judge it), or -1: both */
static int nplants, plant_stray;   /* plant_stray: flagged steps over no planted stall */

/* Step p of CPU c. Judged once both CPUs' CPU time is known past its end, or, when that cannot wait (the CPU
   threads replaced, too many waiting), on what is known: the rest of the step is then unexplained. */
static void judge(int c, const Pend *p) {
    Took a[NCPUS], b[NCPUS];
    if (!took_at(p->a, a) || !took_at(p->b, b)) {
        if (unmeasured < 8) {
            uint64_t lo = hist_lo;
            uint64_t *u = unm[unmeasured];
            u[0] = (uint64_t)c, u[1] = p->a, u[2] = p->b, u[3] = hist_n > lo ? hist[lo % HIST].g : 0;
            u[4] = hist_n > lo ? hist[(hist_n - 1) % HIST].g : 0, u[5] = hist_n;
        }
        unmeasured++;
        return;
    }
    Took e = p->have[c] ? p->e[c] : committed[c] ? commit_t[c] : a[c];
    unsettled += !p->have[0] || !p->have[1];
    Split s;
    uint64_t ex = split(&a[c], &e, &b[c], b[c].pw, p->b - p->a, &s);
    int across = 0;
    for (uint64_t k = nstops > 64 ? nstops - 64 : 0; k < nstops; k++) across |= stops_g[k % 64] >= p->a && stops_g[k % 64] < p->b;
    Flag f = {c, across, p->a, s};
    if (s.un > worst_unexplained) worst_unexplained = s.un, worst = f;
    int over_plant = 0;
    for (int i = 0, n = __atomic_load_n(&nplants, __ATOMIC_ACQUIRE); i < n; i++)
        if (p->a < plant_at[i][1] && p->b > plant_at[i][0]) {
            over_plant = 1;
            if ((plant_cpu[i] < 0 || plant_cpu[i] == c) && s.un >= plant_un[i]) plant_un[i] = s.un, plant_ex[i] = ex;
        }
    if (s.un <= STEP_LIMIT_MS * TICKS_MS) return;
    plant_stray += !over_plant;
    int n = __atomic_load_n(&nflags, __ATOMIC_RELAXED);
    if (n < (int)(sizeof flags / sizeof *flags)) flags[n] = f;
    __atomic_store_n(&nflags, n + 1, __ATOMIC_RELEASE);
}

static void judge_waiting(int all) {
    for (int c = 0; c < NCPUS; c++)
        for (; pend_head[c] != pend_tail[c]; pend_head[c]++) {
            const Pend *p = &pend[c][pend_head[c] % PEND];
            if (!all && !(p->have[0] && p->have[1])) break;
            judge(c, p);
        }
}

/* After a look: each waiting step takes each CPU's first record known past its end. */
static void settle(void) {
    for (int c = 0; c < NCPUS; c++)
        for (uint64_t k = pend_head[c]; k != pend_tail[c]; k++) {
            Pend *p = &pend[c][k % PEND];
            for (int o = 0; o < NCPUS; o++)
                if (!p->have[o] && committed[o] && commit_g[o] >= p->b) p->have[o] = 1, p->e[o] = commit_t[o];
        }
}

static void see_reads(void) {
    for (int c = 0; c < NCPUS; c++) {
        uint64_t p = p_now(c);
        if (p == last_p[c]) continue;
        if (last_p[c] && p > last_p[c]) {
            if (pend_tail[c] - pend_head[c] == PEND) judge(c, &pend[c][pend_head[c]++ % PEND]);
            Pend *s = &pend[c][pend_tail[c]++ % PEND];
            memset(s, 0, sizeof *s);
            s->a = last_p[c], s->b = p;
        }
        last_p[c] = p;
    }
}

static int thread_state(thread_t t, int *running, uint64_t *cpu_us) {
    thread_basic_info_data_t bi;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (t == MACH_PORT_NULL || thread_info(t, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) != KERN_SUCCESS) return -1;
    *running = bi.run_state == TH_STATE_RUNNING;
    *cpu_us = (uint64_t)(bi.user_time.seconds + bi.system_time.seconds) * 1000000 + bi.user_time.microseconds +
              bi.system_time.microseconds;
    return 0;
}

/* The sampler's own: the ports it looks at and the last look. */
static thread_t s_port[NCPUS];
static int s_have, s_run[NCPUS], s_park[NCPUS], s_settled; /* s_park: 1 parked, 2 waiting on the other CPU's lock */
static uint64_t s_cpu[NCPUS], s_y[NCPUS], s_now, s_g, s_tid[NCPUS], peer_lock_looks;

/* CPU c's thread, blocked, waits for a mutex the other CPU's thread holds. Blocked in psynch_mutexwait (syscall
   301), its saved x0 is the mutex and x3 the owner's thread id as it went to sleep, the one the kernel pushes
   priority to; x3 is not brought up to date if a third waiter takes the lock first, so the mutex's own owner field
   (libpthread keeps the tid at offset 24, cleared at unlock) must name the other CPU too: what the other CPU then
   waits for a core, CPU c waits for too. */
static int waits_on_peer(int c) {
    arm_thread_state64_t st;
    mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    if (!s_tid[c ^ 1] || thread_get_state(s_port[c], ARM_THREAD_STATE64, (thread_state_t)&st, &n) != KERN_SUCCESS) return 0;
    if (st.__x[16] != 301 || st.__x[3] != s_tid[c ^ 1]) return 0;
    /* x0 is the mutex only while the call is under way: just back from it, x0 is its result and x16 still 301. So
       read the mutex the safe way, and only one with a mutex's signature ("MUT" and a kind) counts. */
    uint64_t mu[4];
    vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)st.__x[0], sizeof mu, (vm_address_t)mu, &got) != KERN_SUCCESS ||
        got != sizeof mu)
        return 0;
    return (mu[0] >> 8) == 0x4D5554 && mu[3] == s_tid[c ^ 1];
}

/* When a CPU's thread has been blocked in the engine (not parked, not on the other CPU's lock) for 25, 37 and 49 ms:
   each of the machine's threads' state and the top of its stack, so a flagged step names the lock it waited on and
   what the holder was doing. Taking a running thread off its core can lengthen the wait; past 25 ms of time never
   excused, the step fails anyway. */
#define CAP_FRAMES 8
typedef struct CapThread { int state, n; uint64_t pc[CAP_FRAMES]; } CapThread;
typedef struct Cap { int cpu; uint64_t g, blocked; CapThread th[NCPUS + 2]; } Cap; /* th: the CPUs, main, the kicker */
static Cap caps[64];
static int ncaps;
/* main_port, kick_port: declared with the kicker */
static uint64_t bl_since[NCPUS];
static int bl_caps[NCPUS];

static void cap_thread(thread_t t, CapThread *o) {
    o->state = -1, o->n = 0;
    if (t == MACH_PORT_NULL) return;
    thread_basic_info_data_t bi;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(t, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) == KERN_SUCCESS) o->state = bi.run_state;
    pthread_t p = pthread_from_mach_thread_np(t); /* before the suspend: it takes the thread list's lock */
    uint64_t hi = p ? (uint64_t)pthread_get_stackaddr_np(p) : 0, lo = p ? hi - pthread_get_stacksize_np(p) : 0;
    if (thread_suspend(t) != KERN_SUCCESS) return;
    arm_thread_state64_t st;
    mach_msg_type_number_t sn = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(t, ARM_THREAD_STATE64, (thread_state_t)&st, &sn) == KERN_SUCCESS) {
        o->pc[o->n++] = (uint64_t)arm_thread_state64_get_pc(st);
        o->pc[o->n++] = (uint64_t)arm_thread_state64_get_lr(st);
        for (uint64_t fp = (uint64_t)arm_thread_state64_get_fp(st); o->n < CAP_FRAMES && fp >= lo && fp + 16 <= hi && !(fp & 15);) {
            uint64_t f[2]; /* read the safe way: a thread ending frees its stack before it is gone */
            vm_size_t got = 0;
            if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof f, (vm_address_t)f, &got) != KERN_SUCCESS || got != sizeof f)
                break;
            o->pc[o->n++] = f[1] & 0x0000ffffffffffffull;
            if (f[0] <= fp) break;
            fp = f[0];
        }
    }
    thread_resume(t);
}

static void cap_blocked(int c, uint64_t g, uint64_t blocked) {
    Cap *k = &caps[ncaps % 64];
    k->cpu = c, k->g = g, k->blocked = blocked;
    for (int i = 0; i < NCPUS; i++) cap_thread(s_port[i], &k->th[i]);
    cap_thread(main_port, &k->th[NCPUS]);
    cap_thread(kick_port, &k->th[NCPUS + 1]);
    __atomic_store_n(&ncaps, ncaps + 1, __ATOMIC_RELEASE);
}

static void show_frame(uint64_t a) {
    Dl_info di;
    if (dladdr((void *)a, &di) && di.dli_sname)
        printf(" %s+%llu", di.dli_sname, (unsigned long long)(a - (uint64_t)di.dli_saddr));
    else
        printf(" 0x%llx", (unsigned long long)(a - (uint64_t)_dyld_get_image_vmaddr_slide(0)));
}

static void show_caps(int c, uint64_t from, uint64_t to) {
    static const char *who[] = {"CPU 0", "CPU 1", "main", "kicker"};
    static const char *state[] = {"?", "runnable", "stopped", "waiting", "waiting (uninterruptible)", "halted"};
    int n = __atomic_load_n(&ncaps, __ATOMIC_ACQUIRE);
    for (int i = n > 64 ? n - 64 : 0; i < n; i++) {
        const Cap *k = &caps[i % 64];
        if (k->cpu != c || k->g < from || k->g > to) continue;
        printf("    CPU %d blocked %.0f ms in the engine; then:\n", c, ms(k->blocked));
        for (int t = 0; t < NCPUS + 2; t++) {
            printf("      %s, %s:", who[t], k->th[t].state >= 0 && k->th[t].state <= 5 ? state[k->th[t].state] : "?");
            for (int j = 0; j < k->th[t].n; j++) show_frame(k->th[t].pc[j]);
            printf("\n");
        }
    }
}

/* Host ticks each CPU thread spent in sched_yield: its core given away, not waited for. The engine's spin-waits
   (CPU_ON's, the code-protect wait, a stop's) bind to this one at link time; it yields as the system's does. */
static uint64_t yield_ticks[NCPUS], yield_since[NCPUS]; /* yield_since: when the one in progress began, or 0 */
static __thread int my_cpu = -1;
extern boolean_t swtch_pri(int pri);
int sched_yield(void) {
    int c = my_cpu;
    uint64_t t = mach_absolute_time();
    if (c >= 0) __atomic_store_n(&yield_since[c], t, __ATOMIC_SEQ_CST);
    swtch_pri(0);
    if (c >= 0) { /* ended before counted: a look between sees it neither in progress nor counted, never both */
        uint64_t e = mach_absolute_time();
        __atomic_store_n(&yield_since[c], 0, __ATOMIC_SEQ_CST);
        __atomic_add_fetch(&yield_ticks[c], e - t, __ATOMIC_SEQ_CST);
    }
    return 0;
}

/* CPU c's time in sched_yield to host time now, the one in progress included: a yield over many looks is
   counted in each, not as time waiting for a core until it returns. Never less than last seen. */
static uint64_t yielded_to(int c, uint64_t now, uint64_t last) {
    uint64_t t1, s, t2;
    do {
        t1 = __atomic_load_n(&yield_ticks[c], __ATOMIC_SEQ_CST);
        s = __atomic_load_n(&yield_since[c], __ATOMIC_SEQ_CST);
        t2 = __atomic_load_n(&yield_ticks[c], __ATOMIC_SEQ_CST);
    } while (t1 != t2);
    uint64_t y = t1 + (s && s < now ? now - s : 0);
    return y > last ? y : last;
}

static uint64_t st_start[NCPUS], st_cpu[NCPUS]; /* each CPU's stretch since its CPU time last moved: its first record, less
                                                  one, and its CPU time then */

/* CPU o's CPU time is known to here: its stretch closes. The thread waited for a core and then ran, so its CPU time
   is the stretch's last runnable time and the rest is waiting. The other CPU's parked time in each interval is
   matched against that interval's waiting, and added to its records from there on; so is o's own waiting.
   moved: closed because its CPU time moved (still runnable). That CPU time is known only to some point in the last
   interval, so the whole of that interval counts as on a core: the waiting placed is never more than the real one
   (the rest of that interval's CPU time, counted again in the next stretch, only takes from that one's waiting). */
static void close_stretch(int o, int moved) {
    int c = o ^ 1;
    uint64_t lo = st_start[o] + 1, hi = hist_n, oncore = sub(cur[o].cpu, st_cpu[o]), w_last = 0, k, acc = 0, accq = 0;
    if (moved && hi) oncore += hist[(hi - 1) % HIST].ir[o];
    if (lo < hist_lo) lo = hist_lo;
    for (k = hi; k > lo; k--) { /* records k.. on a core; k-1 waited w_last of its runnable time; those before, all */
        uint64_t r = hist[(k - 1) % HIST].ir[o];
        if (r >= oncore) {
            w_last = r - oncore;
            break;
        }
        oncore -= r;
    }
    for (uint64_t i = lo; i < hi; i++) {
        Rec *r = &hist[i % HIST];
        uint64_t w = sub(i + 1 < k ? r->ir[o] : i + 1 == k ? w_last : 0, r->iy[o]);
        acc += w < r->ip[c] ? w : r->ip[c];
        accq += w;
        r->t[c].pw += acc;
        r->t[o].qw += accq;
    }
    cur[c].pw += acc;
    cur[o].qw += accq;
    st_start[o] = hi - 1, st_cpu[o] = cur[o].cpu;
}

static void commit(int c, uint64_t g, int moved) {
    committed[c] = 1, commit_g[c] = g, commit_t[c] = cur[c];
    close_stretch(c, moved);
}

/* After the history is cut back: every stretch starts afresh at its last record. */
static void restart_stretches(void) {
    for (int c = 0; c < NCPUS; c++) st_start[c] = hist_n - 1, st_cpu[c] = cur[c].cpu;
}

/* One look at the CPU threads at host time now, guest time g (under smp_mu). held: the guest's clock stands,
   so nothing is counted; only the baseline moves. A thread not runnable has all its CPU time counted (it
   entered the kernel to block); a runnable one whose CPU time moved had it counted at some point since the
   last look. A thread gone is as one blocked. */
static void look(uint64_t now, uint64_t g, int held) {
    int r[NCPUS], k[NCPUS], settled = 1;
    uint64_t c2[NCPUS], y2[NCPUS];
    for (int c = 0; c < NCPUS; c++) {
        y2[c] = yielded_to(c, now, s_y[c]);
        if (thread_state(s_port[c], &r[c], &c2[c]) != 0) r[c] = 0, c2[c] = s_cpu[c];
        k[c] = !r[c] && !__atomic_load_n(&cpus[c].pc_state->active, __ATOMIC_SEQ_CST);
        HvPerCpu *pc = &m.percpu[c];
        settled &= __atomic_load_n(&pc->at_gate, __ATOMIC_SEQ_CST) || __atomic_load_n(&pc->exited, __ATOMIC_SEQ_CST) ||
                   (__atomic_load_n(&cpus[c].off, __ATOMIC_SEQ_CST) && __atomic_load_n(&cpus[c].parked_off, __ATOMIC_SEQ_CST));
    }
    settled &= __atomic_load_n(&m.pause_req, __ATOMIC_SEQ_CST) != 0;
    for (int c = 0; c < NCPUS; c++)
        if (!r[c] && !k[c] && waits_on_peer(c)) k[c] = 2, peer_lock_looks++;
    for (int c = 0; c < NCPUS; c++) {
        if (r[c] || k[c]) {
            bl_since[c] = 0, bl_caps[c] = 0;
            continue;
        }
        if (!bl_since[c]) bl_since[c] = now;
        if (bl_caps[c] < 3 && now - bl_since[c] >= (STEP_LIMIT_MS + 12 * (uint64_t)bl_caps[c]) * TICKS_MS)
            cap_blocked(c, g, now - bl_since[c]), bl_caps[c]++;
    }
    uint64_t ir[NCPUS] = {0}, ip[NCPUS] = {0}, iy[NCPUS] = {0};
    if (s_have && !held) {
        uint64_t dt = now - s_now, span = dt < 2 * SMP_PERIOD ? dt : 2 * SMP_PERIOD;
        for (int c = 0; c < NCPUS; c++) {
            ir[c] = span * (uint64_t)(s_run[c] + r[c]) / 2;
            ip[c] = span * (uint64_t)(!!s_park[c] * s_run[c ^ 1] + !!k[c] * r[c ^ 1]) / 2;
            iy[c] = y2[c] - s_y[c];
            cur[c].run += ir[c];
            cur[c].cpu += (c2[c] - s_cpu[c]) * 24;
            cur[c].y += iy[c];
            cur[c].t += dt;
            cur[c].gap += dt - span;
            cur[c].settled += span * (uint64_t)(s_settled + settled) / 2;
        }
    }
    hist_push(g, cur, ir, ip, iy);
    for (int c = 0; c < NCPUS; c++)
        if (!r[c]) commit(c, g, 0);
        else if (s_have && c2[c] != s_cpu[c]) commit(c, s_g, 1);
    memcpy(s_run, r, sizeof r), memcpy(s_park, k, sizeof k), memcpy(s_cpu, c2, sizeof c2), memcpy(s_y, y2, sizeof y2);
    s_now = now, s_g = g, s_have = 1, s_settled = settled;
}

/* New CPU threads (under smp_mu): the steps still waiting are judged on what is known, then the new ones looked at. */
static void take_ports(void) {
    if (!ports_new) return;
    settle();
    judge_waiting(1);
    memcpy(s_port, cpu_port, sizeof s_port);
    for (int c = 0; c < NCPUS; c++) {
        thread_identifier_info_data_t ti;
        mach_msg_type_number_t n = THREAD_IDENTIFIER_INFO_COUNT;
        s_tid[c] = thread_info(s_port[c], THREAD_IDENTIFIER_INFO, (thread_info_t)&ti, &n) == KERN_SUCCESS ? ti.thread_id : 0;
    }
    ports_new = 0, s_have = 0;
}

static void *sampler(void *unused) {
    (void)unused;
    thread_time_constraint_policy_data_t pol = {(uint32_t)SMP_PERIOD, (uint32_t)SMP_PERIOD / 2, (uint32_t)SMP_PERIOD, 1};
    thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    int seen = __atomic_load_n(&smp_epoch, __ATOMIC_ACQUIRE);
    uint64_t prev = 0, next = mach_absolute_time();
    for (;;) {
        next += SMP_PERIOD;
        uint64_t now = mach_absolute_time();
        if (next < now) next = now;
        mach_wait_until(next);
        now = mach_absolute_time();
        if (prev && now - prev > 2 * TICKS_MS) smp_gaps++;
        if (prev && now - prev > smp_worst_gap) smp_worst_gap = now - prev;
        prev = now;
        int ep = __atomic_load_n(&smp_epoch, __ATOMIC_ACQUIRE);
        if (ep != seen) {
            if (ep & 1) {
                /* Stopped at guest time smp_tp, every CPU parked: a last look once their threads are seen blocked
                   (their CPU time all counted; at most 10 ms). Samples taken after the stop settled are dropped,
                   the reads made before it judged, and what each CPU's last read is owed kept. */
                uint64_t tp = smp_tp;
                pthread_mutex_lock(&smp_mu);
                take_ports();
                for (int tries = 0;; tries++) {
                    look(now, tp, 0);
                    if (!s_run[0] && !s_run[1]) break;
                    if (tries == 40) break;
                    mach_wait_until(now + SMP_PERIOD);
                    now = mach_absolute_time();
                }
                pthread_mutex_unlock(&smp_mu);
                while (hist_n > hist_lo && hist[(hist_n - 1) % HIST].g > tp) hist_n--;
                hist_push(tp, cur, NULL, NULL, NULL);
                restart_stretches();
                for (int c = 0; c < NCPUS; c++) /* looks between the stop and now saw the guest's clock still moving */
                    if (commit_g[c] > tp) commit_g[c] = tp;
                stops_g[nstops++ % 64] = tp;
                see_reads();
                settle();
                judge_waiting(1);
                for (int c = 0; c < NCPUS; c++) {
                    Took a[NCPUS];
                    Split s;
                    stop_excuse[c] = last_p[c] && took_at(last_p[c], a) ? split(&a[c], &cur[c], NULL, cur[c].pw, 0, &s) : 0;
                }
            }
            s_have = 0;
            seen = ep;
        }
        if (ep & 1) {
            __atomic_store_n(&smp_ack, ep, __ATOMIC_RELEASE);
            continue;
        }
        pthread_mutex_lock(&smp_mu); /* the ports stay ours while looked at */
        take_ports();
        /* The guest's time now. Until resume_machine's hv_machine_resume it is still held at smp_tp: the epoch turns
           first, so this sampler's baseline is from before the CPUs run, and none of their time is missed however
           long the resuming thread waits for a core in between. (The resume moves the bias before pause_req drops.) */
        int held = __atomic_load_n(&m.pause_req, __ATOMIC_SEQ_CST) &&
                   __atomic_load_n(&hv_time_bias, __ATOMIC_SEQ_CST) == __atomic_load_n(&smp_held_bias, __ATOMIC_ACQUIRE);
        uint64_t g = held ? smp_tp : now - __atomic_load_n(&hv_time_bias, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&smp_epoch, __ATOMIC_ACQUIRE) != ep) { /* stopped meanwhile: not this one */
            pthread_mutex_unlock(&smp_mu);
            continue;
        }
        look(now, g, held);
        pthread_mutex_unlock(&smp_mu);
        see_reads();
        settle();
        judge_waiting(0);
        __atomic_add_fetch(&smp_looks, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&smp_ack, ep, __ATOMIC_RELEASE);
    }
    return NULL;
}

/* After the CPU threads ended: a few of the sampler's looks, so it has seen them gone and judged every step. */
static void drain_sampler(void) {
    uint64_t n = __atomic_load_n(&smp_looks, __ATOMIC_ACQUIRE);
    for (int i = 0; i < 1000 && __atomic_load_n(&smp_looks, __ATOMIC_ACQUIRE) < n + 3; i++) usleep(1000);
}

static void smp_set(int ep) {
    __atomic_store_n(&smp_epoch, ep, __ATOMIC_RELEASE);
    while (__atomic_load_n(&smp_ack, __ATOMIC_ACQUIRE) != ep) usleep(100);
}

/* The CPU threads' ports, for the sampler: ours (a reference of our own) until the next ones. */
static void publish_ports(void) {
    pthread_mutex_lock(&smp_mu);
    for (int i = 0; i < NCPUS; i++) {
        thread_t t = pthread_mach_thread_np(threads[i]);
        mach_port_mod_refs(mach_task_self(), t, MACH_PORT_RIGHT_SEND, 1);
        if (cpu_port[i]) mach_port_deallocate(mach_task_self(), cpu_port[i]);
        cpu_port[i] = t;
    }
    ports_new = 1;
    pthread_mutex_unlock(&smp_mu);
}

typedef struct Since { Took t[NCPUS]; } Since;

static void excuse_now(Since *s) {
    pthread_mutex_lock(&smp_mu);
    memcpy(s->t, cur, sizeof s->t);
    pthread_mutex_unlock(&smp_mu);
}

/* Pausing: CPU c still keeps the pause from settling (not at the gate, not parked off, its hv_run not returned). */
static int holds_pause(int c) {
    HvPerCpu *pc = &m.percpu[c];
    return !__atomic_load_n(&pc->at_gate, __ATOMIC_SEQ_CST) && !__atomic_load_n(&pc->exited, __ATOMIC_SEQ_CST) &&
           !(__atomic_load_n(&cpus[c].off, __ATOMIC_SEQ_CST) && __atomic_load_n(&cpus[c].parked_off, __ATOMIC_SEQ_CST));
}

/* Over limit_ms that the host does not explain, or over 5 times that in all: says where each CPU's time went.
   Explained: the least any CPU still holding it up was (each of them must get there), as for a step; when none
   is seen holding it any more (it ended meanwhile), the most any was. pausing: holding = holds_pause, else not
   finished. */
static int too_long(uint64_t t0, const Since *s, unsigned limit_ms, const char *what, const char *took, int pausing) {
    Took now[NCPUS];
    Split sp[NCPUS];
    pthread_mutex_lock(&smp_mu);
    memcpy(now, cur, sizeof now);
    pthread_mutex_unlock(&smp_mu);
    uint64_t wall = mach_absolute_time() - t0, ex = 0, ex_hold = UINT64_MAX;
    for (int c = 0; c < NCPUS; c++) {
        uint64_t e = split(&s->t[c], &now[c], NULL, now[c].pw, wall, &sp[c]);
        if (e > ex) ex = e;
        int hold = pausing ? holds_pause(c) : !__atomic_load_n(&thread_done[c], __ATOMIC_SEQ_CST);
        if (hold && e < ex_hold) ex_hold = e;
    }
    if (ex_hold != UINT64_MAX) ex = ex_hold;
    uint64_t un = sub(wall, ex), limit = limit_ms * TICKS_MS;
    if (un <= limit && wall <= 5 * limit) return 0;
    printf("  %s: %s %.0f ms, %.0f ms of it explained by the host: %.0f ms unexplained (limit %u ms%s)\n", what, took,
           ms(wall), ms(ex), ms(un), limit_ms, un <= limit ? ", or 5 times that in all" : "");
    for (int c = 0; c < NCPUS; c++)
        printf("    CPU %d: %.0f ms on a core, %.0f ms waiting for one, %.0f ms yielded it, %.0f ms blocked (%.0f ms of it parked "
               "or on the other CPU's lock while the other waited for a core; the sampler late %.0f ms); pc %llx el%llu%s%s%s%s\n",
               c, ms(sp[c].cpu), ms(sp[c].queued), ms(sp[c].yielded), ms(sp[c].blocked), ms(sp[c].peer), ms(sp[c].gap),
               (unsigned long long)cpus[c].pc,
               (unsigned long long)cpus[c].el, cpus[c].off ? ", off" : "", cpus[c].parked_off ? " (parked)" : "",
               __atomic_load_n(&cpus[c].pc_state->at_gate, __ATOMIC_SEQ_CST) ? ", at the gate" : ", not at the gate",
               cpus[c].halted ? ", halted" : "");
    return 1;
}

/* Planted stalls (HV_SNAP_PLANT_STALL_MS), of a kind (HV_SNAP_PLANT_KIND), while both CPUs run their loops:
     0  the GIC's lock held that long by another thread, so both CPUs stop at their next interrupt;
     1  one CPU's thread doing that much work on its core (CPU time), outside the guest (a signal handler that spins);
     2  one CPU's thread blocked that long on a lock another thread holds, the other CPU running on.
   Guest times each began and ended (the machine runs throughout: no pause can come while plant_mu is held),
   matched to the steps in guest time, since a step the guest only sees after a pause is seen by the host that
   much later. */
static unsigned plant_ms, plant_kind;
static int plant_go; /* plant_go: not in the reference runs, which set how long a run takes */
static pthread_mutex_t plant_mu = PTHREAD_MUTEX_INITIALIZER; /* held by whoever pauses the machine */
static pthread_mutex_t plant_lock = PTHREAD_MUTEX_INITIALIZER; /* kind 2's */
static int plant_armed, plant_started, plant_done;

static void plant_began(void) {
    plant_cpu[nplants] = my_cpu; /* -1 off the CPU threads (kind 0) */
    plant_at[nplants][2] = mach_absolute_time();
    plant_at[nplants][0] = plant_at[nplants][2] - hv_time_bias;
}
static void plant_ended(void) {
    plant_at[nplants][1] = mach_absolute_time() - hv_time_bias;
    __atomic_store_n(&nplants, nplants + 1, __ATOMIC_RELEASE); /* before any CPU can step past it */
}

static void on_plant(int sig) { /* on the CPU thread, kinds 1 and 2 */
    (void)sig;
    if (!__atomic_exchange_n(&plant_armed, 0, __ATOMIC_ACQ_REL)) return; /* the planter gave up on it */
    plant_began();
    __atomic_store_n(&plant_started, 1, __ATOMIC_RELEASE);
    if (plant_kind == 1) { /* that much CPU time: the engine's work, however long the host keeps it off a core */
        /* No kernel entry but a look at its CPU time every 10 ms of wall time, as an engine stall that makes no system
           call (a long translation): its CPU time lags as that one's would. */
        uint64_t t0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID), w = mach_absolute_time();
        for (;;) {
            if (mach_absolute_time() - w < 10 * TICKS_MS) continue;
            if (clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - t0 >= (uint64_t)plant_ms * 1000000) break;
            w = mach_absolute_time();
        }
    } else {
        pthread_mutex_lock(&plant_lock);
        pthread_mutex_unlock(&plant_lock);
    }
    plant_ended();
    __atomic_store_n(&plant_done, 1, __ATOMIC_RELEASE);
}

static void *planter(void *unused) {
    (void)unused;
    for (;;) {
        usleep(50000 + arc4random_uniform(400000));
        pthread_mutex_lock(&plant_mu);
        uint64_t p0 = p_now(0), p1 = p_now(1);
        usleep(1000);
        if (__atomic_load_n(&plant_go, __ATOMIC_ACQUIRE) && !(__atomic_load_n(&smp_epoch, __ATOMIC_ACQUIRE) & 1) && running && p0 && p1 && p_now(0) != p0 && p_now(1) != p1 &&
            nplants < 256) { /* both in their loops */
            if (plant_kind == 0) {
                pthread_mutex_lock(&board.gic->mu);
                plant_began();
                usleep(plant_ms * 1000);
                plant_ended();
                pthread_mutex_unlock(&board.gic->mu);
            } else {
                if (plant_kind == 2) pthread_mutex_lock(&plant_lock);
                plant_started = plant_done = 0;
                __atomic_store_n(&plant_armed, 1, __ATOMIC_RELEASE);
                int t = (int)arc4random_uniform(NCPUS), sent = 0;
                pthread_mutex_lock(&fin_mu); /* the thread cannot end (or be joined) while this is held */
                if (running && !thread_done[t]) sent = pthread_kill(threads[t], SIGUSR1) == 0;
                pthread_mutex_unlock(&fin_mu);
                /* Begun, or given up on: one not begun in a second (its thread was ending) is not planted. */
                for (uint64_t t0 = mach_absolute_time(); !__atomic_load_n(&plant_started, __ATOMIC_ACQUIRE); usleep(200))
                    if ((!sent || ms(mach_absolute_time() - t0) > 1000) && __atomic_exchange_n(&plant_armed, 0, __ATOMIC_ACQ_REL))
                        break;
                if (plant_kind == 2) {
                    if (__atomic_load_n(&plant_started, __ATOMIC_ACQUIRE)) usleep(plant_ms * 1000); /* that long from its start */
                    pthread_mutex_unlock(&plant_lock);
                }
                while (__atomic_load_n(&plant_started, __ATOMIC_ACQUIRE) && !__atomic_load_n(&plant_done, __ATOMIC_ACQUIRE)) usleep(200);
            }
        }
        pthread_mutex_unlock(&plant_mu);
    }
    return NULL;
}

static void show_flags(const char *where) {
    int n = __atomic_load_n(&nflags, __ATOMIC_ACQUIRE);
    if (n > (int)(sizeof flags / sizeof *flags)) n = (int)(sizeof flags / sizeof *flags);
    for (; flags_shown < n; flags_shown++) {
        const Flag *f = &flags[flags_shown];
        printf("  %s: CPU %d's clock stepped %.1f ms%s: %.1f ms unexplained (limit %d ms); %.1f ms on a core, %.1f ms "
               "waiting for one, %.1f ms yielded it, %.1f ms blocked (%.1f ms of it parked or on the other CPU's lock while "
               "the other CPU waited for a core; %.1f ms with both stopped for a pause, the guest's clock still running); "
               "the sampler late %.1f ms of it\n",
               where, f->cpu, ms(f->s.step), f->across ? " across a stop" : "", ms(f->s.un), STEP_LIMIT_MS, ms(f->s.cpu),
               ms(f->s.queued), ms(f->s.yielded), ms(f->s.blocked), ms(f->s.peer), ms(f->s.settled), ms(f->s.gap));
        if (!plant_ms || flags_shown < 2) show_caps(f->cpu, f->from, f->from + f->s.step);
    }
}

static char alt_stack[NCPUS][1 << 16];

static void *cpu_thread(void *arg) {
    HvCpu *c = arg;
    my_cpu = c->index;
    if (plant_kind) { /* the planted stalls' signal handler: off the guest's stack */
        stack_t ss = {.ss_sp = alt_stack[c->index], .ss_size = sizeof alt_stack[0]};
        sigaltstack(&ss, NULL);
    }
    int r = hv_run(c);
    usleep(1000); /* the sampler sees this thread blocked: its CPU time to the end is counted */
    pthread_mutex_lock(&fin_mu);
    results[c->index] = r;
    thread_done[c->index] = 1;
    finished++;
    pthread_cond_broadcast(&fin_cv);
    pthread_mutex_unlock(&fin_mu);
    return NULL;
}

static void start_cpus(void) {
    finished = 0;
    memset(thread_done, 0, sizeof thread_done);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8u << 20);
    for (int i = 0; i < NCPUS; i++) pthread_create(&threads[i], &a, cpu_thread, &cpus[i]);
    running = 1;
    publish_ports();
}

/* Stops whatever still runs (at the gate, off or in the guest) and joins the threads. */
static void stop_cpus(void) {
    if (!running) return;
    for (int i = 0; i < NCPUS; i++) __atomic_store_n(&cpus[i].halted, 3, __ATOMIC_SEQ_CST);
    for (int i = 0; i < NCPUS; i++) hv_cpu_wake(&cpus[i]);
    hv_machine_kick_all(&m);
    hv_machine_pause_poke(&m);
    for (int i = 0; i < NCPUS; i++) pthread_join(threads[i], NULL);
    running = 0;
}

/* Both CPUs at snap_done, or -1 after limit_ms that the host's load does not explain. */
static int wait_finished(unsigned limit_ms, const char *what) {
    uint64_t t0 = mach_absolute_time();
    Since x0;
    excuse_now(&x0);
    pthread_mutex_lock(&fin_mu);
    int ok;
    while (!(ok = finished == NCPUS)) {
        struct timespec ts = {0, 100000000};
        pthread_cond_timedwait_relative_np(&fin_cv, &fin_mu, &ts);
        if (finished < NCPUS && too_long(t0, &x0, limit_ms, what, "the run went on", 0)) break;
    }
    ok = finished == NCPUS;
    pthread_mutex_unlock(&fin_mu);
    if (!ok) return -1;
    for (int i = 0; i < NCPUS; i++) pthread_join(threads[i], NULL);
    running = 0;
    return 0;
}

/* Why the last save, load or pause failed: the step, and what it returned (printed with the moment it cost) */
static char why_failed[512];
#define FAILED(...) (snprintf(why_failed, sizeof why_failed, __VA_ARGS__), -1)

/* A pause that didn't settle: where each CPU was (read as it runs: a hint, not a state) */
static int pause_failed(void) {
    int n = snprintf(why_failed, sizeof why_failed, "the machine did not pause (2 s of time the host didn't take):");
    for (int c = 0; c < NCPUS && n > 0 && (size_t)n < sizeof why_failed; c++)
        n += snprintf(why_failed + n, sizeof why_failed - (size_t)n, " CPU %d at pc %llx, EL%d%s", c,
                      (unsigned long long)__atomic_load_n(&cpus[c].pc, __ATOMIC_RELAXED), (int)__atomic_load_n(&cpus[c].el, __ATOMIC_RELAXED),
                      __atomic_load_n(&cpus[c].off, __ATOMIC_RELAXED) ? " (off)" : "");
    return -1;
}

/* Pauses the machine (no planted stall then) and stops the sampler's clock at the guest's. Why it failed: why_failed. */
static int pause_machine(const char *what) {
    pthread_mutex_lock(&plant_mu);
    hv_machine_pause(&m);
    uint64_t t0 = mach_absolute_time(), tp;
    Since x0;
    excuse_now(&x0);
    int r;
    while ((r = hv_machine_pause_wait(&m, 50)) != 0 && !too_long(t0, &x0, 2000, what, "the machine did not pause in", 1)) {
    }
    if (r != 0)
        pause_failed();
    else if (hv_machine_paused_counter(&m, &tp) == 0) {
        smp_tp = tp;
        smp_set(smp_epoch + 1);
    } else {
        /* the wait saw every CPU stopped, the counter's read just after did not: one started again while paused */
        int n = snprintf(why_failed, sizeof why_failed, "paused, then not when the guest's counter was read:");
        for (int c = 0; c < NCPUS && n > 0 && (size_t)n < sizeof why_failed; c++)
            n += snprintf(why_failed + n, sizeof why_failed - (size_t)n, " CPU %d at pc %llx%s%s%s%s;", c,
                          (unsigned long long)__atomic_load_n(&cpus[c].pc, __ATOMIC_RELAXED),
                          __atomic_load_n(&cpus[c].pc_state->at_gate, __ATOMIC_SEQ_CST) ? " at the gate" : " not at the gate",
                          __atomic_load_n(&cpus[c].pc_state->exited, __ATOMIC_SEQ_CST) ? ", exited" : "",
                          __atomic_load_n(&cpus[c].off, __ATOMIC_SEQ_CST) ? ", off" : "",
                          __atomic_load_n(&cpus[c].parked_off, __ATOMIC_SEQ_CST) ? " (parked)" : "");
        r = -1;
    }
    pthread_mutex_unlock(&plant_mu);
    return r;
}

static void resume_machine(void) {
    if (smp_epoch & 1) {
        __atomic_store_n(&smp_held_bias, __atomic_load_n(&hv_time_bias, __ATOMIC_SEQ_CST), __ATOMIC_RELEASE);
        smp_set(smp_epoch + 1);
    }
    hv_machine_resume(&m);
}

static void flush_code(void) { hv_flush_blocks(&m); /* takes tr_mu itself */ }

static uint8_t gic_fresh[sizeof(HvGic)];
static size_t gic_state_off, gic_state_len;

/* The board's tick, entering the kernel first: macOS counts a running thread's CPU time only there, and the
   sampler needs it no more than about a millisecond behind (a tick comes at every kick). */
static uint64_t (*board_tick)(HvMachine *, HvCpu *);
static uint64_t tick_counted(HvMachine *mm, HvCpu *c) {
    (void)clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
    return board_tick(mm, c);
}

static int build_machine(void) {
    if (hv_machine_init(&m, 16u << 20) != 0) return -1;
    HvCfgParams cfg = {.nr_cpus = NCPUS, .ecid = 0x5a5a};
    if (hv_vmapple_create(&board, &m, cpus, RAM_SIZE, NULL, NULL, &cfg, NULL) != 0) return -1;
    board_tick = m.tick;
    m.tick = tick_counted;
    hv_vmapple_reset_cpu(&board);
    ro_mem = mmap(NULL, RO_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    for (uint32_t i = 0; i < RO_SIZE; i++) ro_mem[i] = (uint8_t)(i * 7 + (i >> 9));
    hv_machine_add_ram(&m, RO_PA, RO_SIZE, ro_mem, 1);
    m.stop_pc = code_pa(snap_done);
    /* The GIC's state is from ctlr on, through the CPUs this machine has. */
    gic_state_off = offsetof(HvGic, ctlr);
    gic_state_len = offsetof(HvGic, cpu) + NCPUS * sizeof(HvGicCpu) - gic_state_off;
    memcpy(gic_fresh, board.gic, sizeof(HvGic));
    pthread_t t;
    main_port = pthread_mach_thread_np(pthread_self());
    pthread_create(&t, NULL, kicker, NULL);
    pthread_detach(t);
    pthread_create(&t, NULL, sampler, NULL);
    pthread_detach(t);
    pthread_mutex_lock(&kick_mu);
    kicker_on = 1;
    pthread_mutex_unlock(&kick_mu);
    return 0;
}

/* Power on: zeroed RAM with the program at its start, a GIC as built, CPU 0 at the program. */
static void fresh_start(void) {
    pthread_mutex_lock(&kick_mu);
    memset(board.ram, 0, RAM_SIZE);
    memcpy(gram(CODE), snapshot_programs_begin, (size_t)(snapshot_programs_finish - snapshot_programs_begin));
    pthread_mutex_lock(&board.gic->mu);
    memcpy((char *)board.gic + gic_state_off, gic_fresh + gic_state_off, gic_state_len);
    pthread_mutex_unlock(&board.gic->mu);
    hv_vmapple_reset_cpu(&board);
    cpus[0].pc = code_pa(snap_cpu0);
    for (int i = 0; i < NCPUS; i++) m.percpu[i].irq_line = m.percpu[i].fiq_line = 0;
    flush_code();
    pthread_mutex_unlock(&kick_mu);
    start_cpus();
}

/* ---- What a run ends with ---- */

typedef struct CpuEnd {
    uint64_t x[31], sp, nzcv, fpcr, fpsr, el, pc, ttbr0, tpidr_el1, sp_el0, sp_el1, sctlr, spsel, daif, vbar, off;
    __uint128_t v[32];
} CpuEnd;
#define DET_A 0x21000   /* D: control words and both buffers */
#define DET_B 0x3000    /* D + 0x80000: the page tables */
typedef struct End {
    CpuEnd cpu[NCPUS];
    uint8_t det_a[DET_A], det_b[DET_B];
    uint64_t steps[NCPUS], ticks[NCPUS];
} End;

static void capture(End *e) {
    memset(e, 0, sizeof *e);
    for (int i = 0; i < NCPUS; i++) {
        HvCpu *c = &cpus[i];
        CpuEnd *k = &e->cpu[i];
        memcpy(k->x, c->x, sizeof k->x);
        memcpy(k->v, c->v, sizeof k->v);
        k->sp = c->sp, k->nzcv = c->nzcv & 0xf0000000, k->fpcr = c->fpcr, k->fpsr = c->fpsr, k->el = c->el, k->pc = c->pc;
        k->ttbr0 = c->ttbr0_el1, k->tpidr_el1 = c->tpidr_el1, k->sp_el0 = c->sp_el0, k->sp_el1 = c->sp_el1;
        k->sctlr = c->sctlr_el1, k->spsel = c->spsel, k->daif = c->daif, k->vbar = c->vbar_el1, k->off = c->off;
    }
    memcpy(e->det_a, gram(D), DET_A);
    memcpy(e->det_b, gram(D + 0x80000), DET_B);
    for (int i = 0; i < NCPUS; i++) {
        e->steps[i] = *(uint64_t *)gram(D + 0x40028 + 0x10 * (uint64_t)i);
        e->ticks[i] = *(uint64_t *)gram(D + 0x40000 + 8 * (uint64_t)i);
    }
}

/* Differences from the reference, printed (the first few). */
static int compare(const End *want, const End *got, const char *what) {
    int diffs = 0;
#define DIFF(fmt, ...) do { if (diffs++ < 6) printf("  %s: " fmt "\n", what, __VA_ARGS__); } while (0)
    for (int i = 0; i < NCPUS; i++) {
        const CpuEnd *a = &want->cpu[i], *b = &got->cpu[i];
        for (int r = 0; r < 31; r++)
            if (a->x[r] != b->x[r]) DIFF("cpu%d x%d %llx, expected %llx", i, r, (unsigned long long)b->x[r], (unsigned long long)a->x[r]);
        for (int r = 0; r < 32; r++)
            if (a->v[r] != b->v[r]) DIFF("cpu%d q%d differs", i, r);
#define F(f) if (a->f != b->f) DIFF("cpu%d " #f " %llx, expected %llx", i, (unsigned long long)b->f, (unsigned long long)a->f);
        F(sp) F(nzcv) F(fpcr) F(fpsr) F(el) F(pc) F(ttbr0) F(tpidr_el1) F(sp_el0) F(sp_el1) F(sctlr) F(spsel) F(daif) F(vbar) F(off)
#undef F
    }
    for (size_t o = 0; o < DET_A; o += 8)
        if (memcmp(want->det_a + o, got->det_a + o, 8)) {
            DIFF("RAM D+%#zx %llx, expected %llx", o, *(unsigned long long *)(got->det_a + o), *(unsigned long long *)(want->det_a + o));
            break;
        }
    if (memcmp(want->det_b, got->det_b, DET_B)) DIFF("%s", "the page tables differ");
#undef DIFF
    return diffs;
}

static void print_end(const End *e) {
    const uint64_t *w = (const uint64_t *)e->det_a;
    printf("  counter %llu, pings %llu/%llu, errors %llx, sums %llx %llx, ticks %llu %llu, biggest clock steps %.2f %.2f ms\n",
           (unsigned long long)w[1], (unsigned long long)w[3], (unsigned long long)w[2], (unsigned long long)w[4],
           (unsigned long long)w[5], (unsigned long long)w[6], (unsigned long long)e->ticks[0], (unsigned long long)e->ticks[1],
           ms(e->steps[0]), ms(e->steps[1]));
    if (w[4]) printf("  fault: esr %llx elr %llx far %llx vector %llx\n", (unsigned long long)w[7], (unsigned long long)w[8],
                     (unsigned long long)w[9], (unsigned long long)w[10]);
}

/* ---- Save and load ---- */

static char dir[1024], snap_path[1100];


static int save(const char *path) {
    int rc, e;
    why_failed[0] = 0;
    errno = 0;
    HvSnapWriter *w = hv_snap_begin(path, HV_SNAP_NO_SYNC);
    if (!w) return e = errno, FAILED("the save could not start at %s (%s)", path, e ? strerror(e) : "no reason given");
    const char *step = (rc = hv_snap_put_ram(w, &m, 4)) != 0       ? "RAM"
                       : (rc = hv_snap_put_cpus(w, &m)) != 0         ? "CPUs"
                       : (rc = hv_gicv3_snap_save(board.gic, w)) != 0 ? "GIC"
                                                                      : NULL;
    if (step) {
        hv_snap_abort(w);
        return FAILED("the save of the %s failed (%d)", step, rc);
    }
    errno = 0;
    if ((rc = hv_snap_commit(w)) != 0) return e = errno, FAILED("the save's commit failed (%d, %s)", rc, e ? strerror(e) : "no errno");
    return 0;
}

static int load(const char *path, unsigned ram_flags) {
    char why[256] = "";
    why_failed[0] = 0;
    HvSnapReader *r = hv_snap_open(path, why, sizeof why);
    if (!r) return FAILED("the snapshot did not open: %s", why[0] ? why : "no reason given");
    int rc;
    const char *step = (rc = hv_snap_get_ram(r, &m, 4, ram_flags)) != 0 ? "RAM"
                       : (rc = hv_snap_get_cpus(r, &m)) != 0         ? "CPUs"
                       : (rc = hv_gicv3_snap_load(board.gic, r)) != 0 ? "GIC"
                                                                      : NULL;
    hv_snap_close(r);
    flush_code(); /* RAM changed under the JIT: as in a new process, nothing translated is kept */
    return step ? FAILED("the load of the %s failed (%d)", step, rc) : 0;
}

static uint64_t rng = 0x853c49e6748fea9bull;
static uint64_t rnd(void) {
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return rng;
}

/* Everything a snapshot holds, made wrong. */
static void wreck(void) {
    for (int i = 0; i < NCPUS; i++) {
        HvCpu *c = &cpus[i];
        HvMachine *keep_m = c->m;
        HvPerCpu *keep_p = c->pc_state;
        uint64_t keep_mpidr = c->mpidr; /* the board's, set when it is made: a load checks it, never takes it */
        memset(c, 0xa5, sizeof *c);
        c->m = keep_m, c->pc_state = keep_p, c->mpidr = keep_mpidr;
        m.percpu[i].irq_line = 1;
    }
    pthread_mutex_lock(&board.gic->mu);
    memset((char *)board.gic + gic_state_off, 0x5a, gic_state_len);
    pthread_mutex_unlock(&board.gic->mu);
    memset(board.ram, 0xa5, RAM_SIZE);
    hv_time_bias = rnd();
    flush_code();
}

/* ---- The test ---- */

static End ref;
static double ref_ms;

static int run_reference(End *e, double *took) {
    fresh_start();
    uint64_t t0 = mach_absolute_time();
    if (wait_finished(20000, "reference") != 0) {
        stop_cpus();
        printf("  the program did not finish\n");
        return -1;
    }
    *took = ms(mach_absolute_time() - t0);
    capture(e);
    for (int i = 0; i < NCPUS; i++)
        if (results[i] != HV_EXIT_HALT) {
            printf("  cpu%d stopped with %d at pc %llx\n", i, results[i], (unsigned long long)cpus[i].pc);
            return -1;
        }
    return 0;
}

static struct {
    int total, mid_run, cpu1_off, cpu1_pinging, el0[NCPUS], one_done;
    double worst_step, save_ms, load_ms, late_ms;
} st;

/* Set: the first moment waits for CPU 1 to be off (it is off only briefly, so random moments rarely land there). */
static int aim_off;

/* One run, stopped and loaded back at `k` moments. */
static int trial(int k) {
    uint64_t at[3];
    for (int i = 0; i < k; i++) at[i] = (uint64_t)(rnd() % (uint64_t)(ref_ms * 900)); /* us, within the run */
    for (int i = 0; i < k; i++)
        for (int j = i + 1; j < k; j++)
            if (at[j] < at[i]) { uint64_t t = at[i]; at[i] = at[j]; at[j] = t; }
    fresh_start();
    uint64_t ran = 0;
    for (int i = 0; i < k; i++) {
        uint64_t t = mach_absolute_time();
        if (aim_off && i == 0) {
            while (!__atomic_load_n(&cpus[1].off, __ATOMIC_RELAXED) && ms(mach_absolute_time() - t) < ref_ms * 2) {
            }
            at[i] = ran + (uint64_t)(ms(mach_absolute_time() - t) * 1000);
            for (int j = 1; j < k; j++)
                if (at[j] < at[i]) at[j] = at[i];
        } else if (at[i] > ran)
            usleep((useconds_t)(at[i] - ran));
        st.late_ms += ms(mach_absolute_time() - t) - (at[i] - ran) / 1000.0;
        ran = at[i];
        char what[64];
        snprintf(what, sizeof what, "moment %d", st.total + 1);
        if (pause_machine(what) != 0) {
            printf("  %s: %s\n", what, why_failed);
            stop_cpus();
            hv_machine_resume(&m);
            return -1;
        }
        st.total++;
        uint64_t done = code_pa(snap_done);
        int c0_done = cpus[0].pc == done, c1_done = cpus[1].pc == done;
        if (!c0_done || !c1_done) st.mid_run++;
        /* Ended already: this run took no more than `ran` (the host less busy now than in the reference runs).
           The moments after aim within that, so they land mid-run however the load moves. */
        else if (ran / 1000.0 < ref_ms) ref_ms = ran / 1000.0;
        if (c0_done != c1_done) st.one_done++;
        if (cpus[1].off) st.cpu1_off++;
        else if (!c1_done && cpus[1].el == 1 && *(uint64_t *)gram(D + 0x10) < PINGS) st.cpu1_pinging++;
        for (int c = 0; c < NCPUS; c++) st.el0[c] += cpus[c].el == 0;

        t = mach_absolute_time();
        if (save(snap_path) != 0) {
            printf("  moment %d: %s\n", st.total, why_failed);
            stop_cpus();
            resume_machine();
            return -1;
        }
        st.save_ms += ms(mach_absolute_time() - t);
        stop_cpus();
        hv_machine_resume(&m);
        pthread_mutex_lock(&kick_mu);
        wreck();
        if (st.total % 10 == 0) usleep(60000); /* time the guest must not see */
        t = mach_absolute_time();
        int rc = load(snap_path, HV_SNAP_DIRTY_RAM);
        st.load_ms += ms(mach_absolute_time() - t);
        pthread_mutex_unlock(&kick_mu);
        if (rc != 0) {
            printf("  moment %d: %s\n", st.total, why_failed);
            resume_machine();
            return -1;
        }
        start_cpus();
        resume_machine();
    }
    char what[64];
    snprintf(what, sizeof what, "moment %d", st.total);
    if (wait_finished(20000, what) != 0) {
        stop_cpus();
        End e;
        capture(&e);
        printf("  moment %d: the restored program did not finish (pc %llx el%llu, %llx el%llu)\n", st.total,
               (unsigned long long)cpus[0].pc, (unsigned long long)cpus[0].el, (unsigned long long)cpus[1].pc,
               (unsigned long long)cpus[1].el);
        print_end(&e);
        return -1;
    }
    End e;
    capture(&e);
    show_flags(what);
    if (compare(&ref, &e, what)) {
        print_end(&e);
        return -1;
    }
    for (int c = 0; c < NCPUS; c++)
        if (ms(e.steps[c]) > st.worst_step) st.worst_step = ms(e.steps[c]);
    return 0;
}

/* The child: a new process, a new machine, the snapshot loaded into fresh (zeroed) RAM; then the end
   is compared with the reference the parent wrote. The parent stopped the guest's clock at tp, when
   the host had excused each CPU's step so far by ex[c]: the steps across the new process are judged
   as in the parent. */
static int child(const char *snap, const char *ref_path, uint64_t tp, const uint64_t *ex) {
    smp_epoch = smp_ack = 1; /* stopped until the machine runs */
    if (build_machine() != 0) {
        printf("  in the new process: the machine could not be built\n");
        return 2;
    }
    End want;
    int fd = open(ref_path, O_RDONLY);
    ssize_t got = fd < 0 ? -1 : read(fd, &want, sizeof want);
    if (got != (ssize_t)sizeof want) {
        printf("  in the new process: the reference %s could not be read (%s)\n", ref_path,
               fd < 0 || got < 0 ? strerror(errno) : "too short");
        return 2;
    }
    close(fd);
    if (load(snap, 0) != 0) {
        printf("  in the new process: %s\n", why_failed);
        return 3;
    }
    smp_tp = tp;
    for (int c = 0; c < NCPUS; c++) last_p[c] = p_now(c);
    hist_push(tp - 1, cur, NULL, NULL, NULL);
    for (int c = 0; c < NCPUS; c++) cur[c].carry = ex[c];
    hist_push(tp, cur, NULL, NULL, NULL);
    restart_stretches();
    start_cpus();
    resume_machine();
    if (wait_finished(20000, "new process") != 0) {
        stop_cpus();
        printf("  new process: the restored program did not finish\n");
        return 4;
    }
    End e;
    capture(&e);
    if (compare(&want, &e, "new process")) {
        print_end(&e);
        return 5;
    }
    drain_sampler();
    show_flags("new process");
    /* With planting on, the stall planted last before the save (HV_SNAP_PLANTED: its guest begin and end) is seen
       again here, by the step the save fell in: that one is the parent's, not a new one. */
    uint64_t pb = 0, pe = 0;
    if (getenv("HV_SNAP_PLANTED")) sscanf(getenv("HV_SNAP_PLANTED"), "%llu,%llu", (unsigned long long *)&pb, (unsigned long long *)&pe);
    int own = 0;
    for (int j = 0; j < nflags && j < (int)(sizeof flags / sizeof *flags); j++) own += !(flags[j].from < pe && flags[j].from + flags[j].s.step > pb);
    if (own < nflags) printf("  new process: %d step(s) over the stall planted before the save\n", nflags - own);
    return own || nflags > (int)(sizeof flags / sizeof *flags) ? 6 : 0;
}

static int spawn_child(const char *self, const char *snap, const char *ref_path) {
    pid_t pid;
    char tp[32], e0[32], e1[32];
    snprintf(tp, sizeof tp, "%llu", (unsigned long long)smp_tp);
    snprintf(e0, sizeof e0, "%llu", (unsigned long long)stop_excuse[0]);
    snprintf(e1, sizeof e1, "%llu", (unsigned long long)stop_excuse[1]);
    char *argv[] = {(char *)self, "--load", (char *)snap, (char *)ref_path, tp, e0, e1, NULL};
    if (posix_spawn(&pid, self, NULL, NULL, argv, environ) != 0) return -1;
    int status;
    if (waitpid(pid, &status, 0) != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 100;
}

static int copy_file(const char *from, const char *to) {
    char cmd[4200];
    snprintf(cmd, sizeof cmd, "cp '%s' '%s'", from, to);
    return system(cmd);
}

static void flip(const char *path, off_t off) {
    int fd = open(path, O_RDWR);
    uint8_t b = 0;
    pread(fd, &b, 1, off);
    b ^= 0x10;
    pwrite(fd, &b, 1, off);
    close(fd);
}

static off_t file_size(const char *path) {
    struct stat s;
    return stat(path, &s) == 0 ? s.st_size : -1;
}

static int opens(const char *path) {
    HvSnapReader *r = hv_snap_open(path, NULL, 0);
    if (r) hv_snap_close(r);
    return r != NULL;
}

/* (gate r2 should 1) A moment skipped or failed says why: the step and its value, not only that it failed */
static void test_reasons(void) {
    char p[1200];
    snprintf(p, sizeof p, "%s/no-such-folder/x.snap", dir);
    check("reasons: a save that can't start fails", (uint64_t)save(p), (uint64_t)-1);
    check("reasons: and says where and why", strstr(why_failed, "could not start") && strstr(why_failed, "no-such-folder") &&
          strstr(why_failed, strerror(ENOENT)) != NULL, 1);
    check("reasons: a load that can't open fails", (uint64_t)load(p, 0), (uint64_t)-1);
    check("reasons: and says why", strstr(why_failed, "did not open: ") && !strstr(why_failed, "no reason given"), 1);
    pause_failed();
    check("reasons: a pause that didn't settle names each CPU's place", strstr(why_failed, "CPU 0 at pc ") && strstr(why_failed, "CPU 1 at pc "), 1);
}

/* With the machine paused somewhere in the middle of a run. */
static void test_refusals(const char *self) {
    char p[1200], q[1200];
    snprintf(p, sizeof p, "%s/good.snap", dir);
    check("refusals: a paused machine saves", save(p), 0);
    check("refusals: no .partial is left", access((snprintf(q, sizeof q, "%s.partial", p), q), F_OK), (uint64_t)-1);
    check("refusals: the saved file opens", opens(p), 1);

    /* Saving again but failing half way leaves the old file as it was. */
    HvSnapWriter *w = hv_snap_begin(p, HV_SNAP_NO_SYNC);
    hv_snap_put_ram(w, &m, 4);
    check("refusals: a half-written save is in .partial", access(q, F_OK), 0);
    hv_snap_abort(w);
    check("refusals: an abandoned save leaves no .partial", access(q, F_OK), (uint64_t)-1);
    check("refusals: and the previous snapshot still opens", opens(p), 1);
    /* A crash before the rename: the .partial is there, complete or not, and never loaded. */
    copy_file(p, q);
    unlink(p);
    check("refusals: a .partial is not a snapshot", opens(p), 0);
    rename(q, p);

    off_t size = file_size(p);
    snprintf(q, sizeof q, "%s/bad.snap", dir);
    copy_file(p, q);
    flip(q, 100);
    check("refusals: a damaged header", opens(q), 0);
    copy_file(p, q);
    flip(q, size - 20);
    check("refusals: a damaged section", opens(q), 0);
    copy_file(p, q);
    truncate(q, size / 2);
    check("refusals: a file cut short", opens(q), 0);
    copy_file(p, q);
    truncate(q, 0);
    check("refusals: an empty file", opens(q), 0);
    check("refusals: no file", opens("/nonexistent/henge.snap"), 0);

    /* Damaged RAM: the ROM's (empty) 1 MB slot first, then RAM, whose first chunk holds the program. */
    copy_file(p, q);
    flip(q, 65536 + (1 << 20) + 300);
    HvSnapReader *r = hv_snap_open(q, NULL, 0);
    check("refusals: damaged RAM still opens (it is checked as it loads)", r != NULL, 1);
    if (r) {
        static uint8_t keep[RAM_SIZE];
        memcpy(keep, board.ram, RAM_SIZE);
        check("refusals: damaged RAM is refused", hv_snap_get_ram(r, &m, 4, HV_SNAP_DIRTY_RAM), (uint64_t)-1);
        memcpy(board.ram, keep, RAM_SIZE);
        hv_snap_close(r);
    }

    /* A machine that is not the one saved. */
    r = hv_snap_open(p, NULL, 0);
    if (r) {
        ro_mem[1000] ^= 1;
        check("refusals: read-only memory that changed", hv_snap_get_ram(r, &m, 4, HV_SNAP_DIRTY_RAM), (uint64_t)-1);
        ro_mem[1000] ^= 1;
        HvRam keep = m.ram[1];
        m.ram[1].size /= 2;
        check("refusals: RAM of another size", hv_snap_get_ram(r, &m, 4, HV_SNAP_DIRTY_RAM), (uint64_t)-1);
        m.ram[1] = keep;
        hv_snap_close(r);
    }
    r = hv_snap_open(p, NULL, 0);
    if (r) {
        static HvCpu keep[NCPUS];
        memcpy(keep, cpus, sizeof keep);
        cpus[1].mpidr ^= 1; /* a board whose second CPU has another id: PSCI would wake the wrong one */
        check("refusals: CPUs with other ids", hv_snap_get_cpus(r, &m), (uint64_t)-1);
        memcpy(cpus, keep, sizeof keep);
        hv_snap_close(r);
    }
    (void)self;
}

/* Sections too big to build in memory, streamed to the file before the RAM: read back whole, the big ones mapped
   from the file; checked like any other; refused where they would overlap the RAM. */
static void test_streams(void) {
    char p[1200], q[1200];
    snprintf(p, sizeof p, "%s/stream.snap", dir);
    const size_t big = (20u << 20) + 12345; /* over the size that is mapped, and not a whole number of pages */
    uint8_t *want = malloc(big);
    for (size_t i = 0; i < big; i++) want[i] = (uint8_t)(i * 131 + (i >> 13));
    const uint32_t BIG = HV_SNAP_TAG('S', 'T', 'R', 'M'), SMALL = HV_SNAP_TAG('S', 'T', 'R', 'S'), GONE = HV_SNAP_TAG('S', 'T', 'R', 'X');
    HvSnapWriter *w = hv_snap_begin(p, HV_SNAP_NO_SYNC);
    HvSnapStream *s = w ? hv_snap_stream_begin(w, BIG, 3) : NULL;
    int ok = s != NULL;
    for (size_t at = 0, n = 1; ok && at < big; at += n, n = n * 3 + 7) { /* writes of every size, some past its buffer */
        if (n > big - at) n = big - at;
        ok = hv_snap_stream_write(s, want + at, n) == 0;
    }
    check("streams: written", ok && hv_snap_stream_end(s, 1) == 0, 1);
    s = w ? hv_snap_stream_begin(w, SMALL, 1) : NULL;
    ok = s && hv_snap_stream_write(s, "tiny", 4) == 0 && hv_snap_stream_end(s, 1) == 0;
    check("streams: a small one", ok, 1);
    s = w ? hv_snap_stream_begin(w, GONE, 1) : NULL;
    if (s) hv_snap_stream_write(s, "junk", 4);
    check("streams: one dropped", s && hv_snap_stream_end(s, 0) != 0, 1);
    ok = w && hv_snap_put_ram(w, &m, 4) == 0 && hv_snap_put_cpus(w, &m) == 0 && hv_gicv3_snap_save(board.gic, w) == 0;
    check("streams: after the RAM, none", w && hv_snap_stream_begin(w, HV_SNAP_TAG('S', 'T', 'R', 'L'), 1) == NULL, 1);
    if (w) hv_snap_abort(w); /* that failed the writer: done again without */
    w = hv_snap_begin(p, HV_SNAP_NO_SYNC);
    s = w ? hv_snap_stream_begin(w, BIG, 3) : NULL;
    ok = s && hv_snap_stream_write(s, want, big) == 0 && hv_snap_stream_end(s, 1) == 0;
    s = ok ? hv_snap_stream_begin(w, SMALL, 1) : NULL;
    ok = s && hv_snap_stream_write(s, "tiny", 4) == 0 && hv_snap_stream_end(s, 1) == 0;
    ok = ok && hv_snap_put_ram(w, &m, 4) == 0 && hv_snap_put_cpus(w, &m) == 0 && hv_gicv3_snap_save(board.gic, w) == 0;
    check("streams: saved", ok && hv_snap_commit(w) == 0, 1);
    if (!ok && w) hv_snap_abort(w);

    HvSnapReader *r = hv_snap_open(p, NULL, 0);
    check("streams: opens", r != NULL, 1);
    if (r) {
        HvSnapIn in;
        check("streams: the small one", hv_snap_get(r, SMALL, 1, &in) == 0 && in.len == 4 && !memcmp(in.p, "tiny", 4), 1);
        check("streams: the dropped one isn't there", hv_snap_get(r, GONE, 1, &in), (uint64_t)-1);
        check("streams: has the big one", hv_snap_has(r, BIG), 1);
        check("streams: hasn't the dropped one", hv_snap_has(r, GONE), 0);
        check("streams: another version refused", hv_snap_get(r, BIG, 1, &in), (uint64_t)-1);
        check("streams: the big one", hv_snap_get(r, BIG, 3, &in) == 0 && in.len == big && !memcmp(in.p, want, big), 1);
        HvSnapBuf b = {0};
        check("streams: taken", hv_snap_take(r, BIG, 3, &b), 0);
        check("streams: mapped from the file", b.mapped, 1);
        check("streams: taken whole", b.len == big && !memcmp(b.p, want, big), 1);
        check("streams: once taken, gone from the reader", hv_snap_get(r, BIG, 3, &in), (uint64_t)-1);
        check("streams: the RAM after them", hv_snap_get_ram(r, &m, 4, HV_SNAP_DIRTY_RAM), 0);
        hv_snap_close(r);
        check("streams: what was taken outlives the reader", !memcmp(b.p, want, big), 1);
        hv_snap_buf_free(&b);
        HvSnapBuf small = {0};
        r = hv_snap_open(p, NULL, 0);
        check("streams: a small one taken is in memory", r && hv_snap_take(r, SMALL, 1, &small) == 0 && !small.mapped && small.len == 4, 1);
        if (r) hv_snap_close(r);
        hv_snap_buf_free(&small);
    }
    /* A section put under the streamed one's tag while it streams: the stream's end refuses it. In a file of its own */
    snprintf(q, sizeof q, "%s/stream-twice.snap", dir);
    w = hv_snap_begin(q, HV_SNAP_NO_SYNC);
    check("streams: a second writer begins", w != NULL, 1);
    s = w ? hv_snap_stream_begin(w, BIG, 1) : NULL;
    HvSnapOut o = {0};
    uint32_t v = 7;
    HV_SNAP_OUT(&o, v);
    ok = s && hv_snap_stream_write(s, "x", 1) == 0 && hv_snap_put(w, BIG, 1, &o) == 0;
    check("streams: the same tag twice, refused at the end", ok && hv_snap_stream_end(s, 1) != 0, 1);
    if (!ok && s) hv_snap_stream_end(s, 0);
    if (w) hv_snap_abort(w);
    /* A byte of the big one damaged: the file is refused as it opens */
    snprintf(q, sizeof q, "%s/stream-bad.snap", dir);
    copy_file(p, q);
    flip(q, 65536 + big / 2);
    r = hv_snap_open(q, NULL, 0);
    check("streams: damaged, refused", r == NULL, 1);
    if (r) hv_snap_close(r);
    /* Called off (the person came back): the RAM's write stops, says so, and nothing is kept */
    {
        uint64_t count = 5;
        char q[1200]; /* a name nothing else used */
        snprintf(q, sizeof q, "%s/cancelled.snap", dir);
        w = hv_snap_begin(q, HV_SNAP_NO_SYNC);
        hv_snap_set_cancel(w, &count, 5);
        check("cancel: not yet", hv_snap_cancelled(w), 0);
        count = 6;
        check("cancel: the RAM's write stops", w && hv_snap_put_ram(w, &m, 4) != 0, 1);
        check("cancel: and says it was called off", hv_snap_cancelled(w), 1);
        if (w) hv_snap_abort(w);
        check("cancel: nothing kept", access(q, F_OK), (uint64_t)-1);
        char partial[1300];
        snprintf(partial, sizeof partial, "%s.partial", q);
        check("cancel: no .partial", access(partial, F_OK), (uint64_t)-1);
        /* A ticket still current: the save goes through */
        w = hv_snap_begin(q, HV_SNAP_NO_SYNC);
        hv_snap_set_cancel(w, &count, 6);
        check("cancel: a current ticket saves", w && hv_snap_put_ram(w, &m, 4) == 0 && !hv_snap_cancelled(w), 1);
        /* Called off after the RAM: the commit stops before the header, and nothing is kept */
        count = 7;
        check("cancel: the commit stops", w && hv_snap_commit(w) != 0, 1);
        w = NULL;
        check("cancel: after the commit, nothing kept", access(q, F_OK), (uint64_t)-1);
        check("cancel: after the commit, no .partial", access(partial, F_OK), (uint64_t)-1);
        /* Called off while a section streams: its writes stop */
        w = hv_snap_begin(q, HV_SNAP_NO_SYNC);
        hv_snap_set_cancel(w, &count, 7);
        HvSnapStream *cs = w ? hv_snap_stream_begin(w, 0x54534554u, 1) : NULL;
        static uint8_t chunk[4096];
        check("cancel: streaming, before", cs && hv_snap_stream_write(cs, chunk, sizeof chunk) == 0, 1);
        count = 8;
        check("cancel: streaming stops", cs && hv_snap_stream_write(cs, chunk, sizeof chunk) != 0, 1);
        if (cs) hv_snap_stream_end(cs, 0);
        if (w) hv_snap_abort(w);
        /* Deleting where there is no snapshot (a save called off): fine, and nothing there after */
        check("cancel: discarding nothing", hv_snap_discard(q), 0);
        check("cancel: discarded, nothing there", access(q, F_OK), (uint64_t)-1);
    }
    w = hv_snap_begin(q, HV_SNAP_NO_SYNC);
    uint64_t free_bytes = hv_snap_free_bytes(w);
    check("streams: free storage is told", free_bytes > 0 && free_bytes != UINT64_MAX, 1);
    if (w) hv_snap_abort(w);
    free(want);
    unlink(p), unlink(q);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("HV_RAM_NO_LEDGER", "1", 1); /* plain memory: a test binary may not leave the footprint */
    if (argc == 7 && !strcmp(argv[1], "--load")) {
        uint64_t ex[NCPUS] = {strtoull(argv[5], NULL, 0), strtoull(argv[6], NULL, 0)};
        return child(argv[2], argv[3], strtoull(argv[4], NULL, 0), ex);
    }
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    snprintf(dir, sizeof dir, "%s/henge-snap.XXXXXX", tmp);
    if (!mkdtemp(dir) || build_machine() != 0) {
        printf("cannot set up\n");
        return 1;
    }
    snprintf(snap_path, sizeof snap_path, "%s/moment.snap", dir);
    if (argc > 1 && !strcmp(argv[1], "reasons")) { /* the reasons' test alone: no moments */
        test_reasons();
        char rm[1200];
        snprintf(rm, sizeof rm, "rm -rf '%s'", dir);
        system(rm);
        printf("%d checks, %d failures\n", checks, failures);
        return failures != 0;
    }
    if (getenv("HV_SNAP_PLANT_STALL_MS")) { /* proves the clock check: these stalls must all be flagged */
        plant_ms = (unsigned)strtoul(getenv("HV_SNAP_PLANT_STALL_MS"), NULL, 0);
        plant_kind = getenv("HV_SNAP_PLANT_KIND") ? (unsigned)strtoul(getenv("HV_SNAP_PLANT_KIND"), NULL, 0) : 0;
        if (plant_kind) {
            struct sigaction sa = {.sa_handler = on_plant, .sa_flags = SA_ONSTACK};
            sigaction(SIGUSR1, &sa, NULL);
        }
        pthread_t t;
        pthread_create(&t, NULL, planter, NULL);
        pthread_detach(t);
    }
    /* Different moments each time; HV_SNAP_SEED repeats a run's. */
    rng = getenv("HV_SNAP_SEED") ? strtoull(getenv("HV_SNAP_SEED"), NULL, 0) : mach_absolute_time() | 1;
    printf("seed %#llx\n", (unsigned long long)rng);
    printf("RAM regions:");
    for (int i = 0; i < m.nram; i++) printf(" %llx+%llx%s", (unsigned long long)m.ram[i].pa, (unsigned long long)m.ram[i].size, m.ram[i].readonly ? " (ro)" : "");
    printf("\n");

    /* The reference, three times: the program must be deterministic for any of this to mean anything. */
    if (run_reference(&ref, &ref_ms) != 0) {
        End e;
        capture(&e);
        print_end(&e);
        return 1;
    }
    print_end(&ref);
    const uint64_t *w = (const uint64_t *)ref.det_a;
    check("reference: no errors", w[4], 0);
    check("reference: every ping answered", w[3], PINGS);
    check("reference: every pong seen", w[2], PINGS);
    check("reference: the shared counter", w[1], ITERS0 / 16 + ITERS1 / 16 + PINGS * 3);
    check("reference: timer interrupts on both CPUs", ref.ticks[0] > 10 && ref.ticks[1] > 10, 1);
    for (int i = 0; i < 2; i++) {
        End again;
        double t;
        if (run_reference(&again, &t) != 0 || compare(&ref, &again, "reference again")) {
            printf("  the program is not deterministic\n");
            return 1;
        }
        if (t < ref_ms) ref_ms = t;
    }
    printf("  a run takes %.1f ms\n", ref_ms);
    __atomic_store_n(&plant_go, 1, __ATOMIC_RELEASE);

    /* 1,000 moments. */
    uint64_t t0 = mach_absolute_time();
    int bad = 0;
    while (st.total < MOMENTS && bad < 5) {
        int k = 1 + (int)(rnd() % 3);
        if (k > MOMENTS - st.total) k = MOMENTS - st.total;
        if (trial(k) != 0) bad++, failures++;
    }
    int taken = st.total, random_off = st.cpu1_off;
    /* then moments aimed at CPU 1 being off, so that case is always covered, however busy this Mac is */
    aim_off = 1;
    for (int n = 0; n < 200 && st.cpu1_off < 5 && bad < 5; n++)
        if (trial(1) != 0) bad++, failures++;
    aim_off = 0;
    checks++;
    printf("  %d moments in %.1f s: %d mid-run (%d with one CPU done), CPU 1 off at %d, ping-pong at %d, "
           "at EL0 %d and %d; save %.2f ms, load %.2f ms on average\n",
           st.total, ms(mach_absolute_time() - t0) / 1000, st.mid_run, st.one_done, st.cpu1_off, st.cpu1_pinging, st.el0[0],
           st.el0[1], st.save_ms / st.total, st.load_ms / st.total);
    printf("  %d random moments, %d of them with CPU 1 off; %d aimed ones after\n", taken, random_off, st.total - taken);
    printf("  the moments aimed within %.1f ms of a run's start, the fastest run seen\n", ref_ms * 0.9);
    printf("  the guest's biggest clock step: %.2f ms; the pauses came %.2f ms late on average\n", st.worst_step,
           st.late_ms / st.total);
    check("moments: every one ended as the reference", bad, 0);
    check("moments: all 1000 taken", taken, MOMENTS);
    check("moments: at least 90% mid-run", st.mid_run * 10 >= st.total * 9, 1);
    check("moments: some with CPU 1 off, pinging, and at EL0", st.cpu1_off > 0 && st.cpu1_pinging > 0 && st.el0[0] > 0 && st.el0[1] > 0, 1);

    /* In a new process, 10 moments. */
    char ref_path[1200];
    snprintf(ref_path, sizeof ref_path, "%s/reference", dir);
    int fd = open(ref_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    write(fd, &ref, sizeof ref);
    close(fd);
    int child_ok = 0;
    for (int i = 0; i < 10; i++) {
        fresh_start();
        usleep((useconds_t)(rnd() % (uint64_t)(ref_ms * 900)));
        char what[64];
        snprintf(what, sizeof what, "new process %d", i);
        if (pause_machine(what) != 0 || save(snap_path) != 0) {
            printf("  new process %d: not started, %s\n", i, why_failed);
            stop_cpus();
            resume_machine();
            continue;
        }
        if (nplants) { /* the last one before this save (no stall is planted while paused) */
            char pl[64];
            snprintf(pl, sizeof pl, "%llu,%llu", (unsigned long long)plant_at[nplants - 1][0], (unsigned long long)plant_at[nplants - 1][1]);
            setenv("HV_SNAP_PLANTED", pl, 1);
        }
        stop_cpus();
        resume_machine();
        int rc = spawn_child(argv[0], snap_path, ref_path);
        if (rc != 0) printf("  new process %d: exit %d\n", i, rc);
        else child_ok++;
    }
    check("new process: 10 of 10 loaded and ended as the reference", child_ok, 10);

    /* Refusals, with a machine paused mid-run. */
    fresh_start();
    usleep((useconds_t)(ref_ms * 300));
    if (pause_machine("refusals") == 0) test_refusals(argv[0]), test_streams(), test_reasons();
    else printf("  refusals: %s\n", why_failed), check("refusals: the machine paused", 0, 1);
    stop_cpus();
    resume_machine();

    /* The guest's clock: every step it saw, less what the host took (waiting for a core), within 25 ms. */
    show_flags("after");
    /* Each planted stall by the worst step over it (the list of flags keeps only the first 256). One no CPU stepped
       over (the run ended in it), or whose steps the host held off a core long enough to bring it under the limit,
       proves nothing either way: left out, but at most 1 in 10, so a check that excuses everything still fails. */
    int caught = 0, left_out = 0;
    for (int i = 0; i < nplants; i++) {
        uint64_t limit = STEP_LIMIT_MS * TICKS_MS;
        if (plant_un[i] > limit) {
            caught++;
            continue;
        }
        const char *why = plant_un[i] + plant_ex[i] == 0      ? "its CPU did not step over it (its run ended in it)"
                          : plant_un[i] + plant_ex[i] > limit ? "left out: the host also kept the CPU off a core"
                                                              : "NOT FLAGGED";
        left_out += why[0] != 'N';
        printf("  planted stall %d (%.1f ms of guest time): the worst step over it %.2f ms unexplained, %.2f ms excused: %s\n",
               i, ms(plant_at[i][1] - plant_at[i][0]), ms(plant_un[i]), ms(plant_ex[i]), why);
    }
    printf("  the worst step: CPU %d's clock stepped %.2f ms%s: %.2f ms unexplained; %.2f ms on a core, %.2f ms waiting for "
           "one, %.2f ms yielded it, %.2f ms blocked (%.2f ms of it parked or on the other CPU's lock while the other "
           "waited for a core; %.2f ms with both stopped for a pause, the guest's clock still running); the sampler "
           "late %.2f ms of it\n",
           worst.cpu, ms(worst.s.step), worst.across ? " across a stop" : "", ms(worst.s.un), ms(worst.s.cpu),
           ms(worst.s.queued), ms(worst.s.yielded), ms(worst.s.blocked), ms(worst.s.peer), ms(worst.s.settled),
           ms(worst.s.gap));
    printf("  the sampler: %d steps it could not measure, %d judged before both CPUs' time was known past their end, "
           "%llu gaps over 2 ms (the longest %.1f ms); a CPU seen blocked on the other's lock %llu times\n",
           unmeasured, unsettled, (unsigned long long)smp_gaps, ms(smp_worst_gap), (unsigned long long)peer_lock_looks);
    for (int i = 0; i < unmeasured && i < 8; i++)
        printf("  unmeasured: CPU %llu's step from %.2f to %.2f ms of guest time; the samples reached %.2f to %.2f ms "
               "(%llu taken)\n",
               (unsigned long long)unm[i][0], ms(unm[i][1]), ms(unm[i][2]), ms(unm[i][3]), ms(unm[i][4]),
               (unsigned long long)unm[i][5]);
    check("the sampler measured every step", unmeasured, 0);
    if (plant_ms) {
        printf("  planted %d stalls of %u ms (kind %u): %d of them flagged, %d left out; %d other steps flagged\n", nplants,
               plant_ms, plant_kind, caught, left_out, plant_stray);
        check("planted stalls: some were planted", nplants > 0, 1);
        check("planted stalls: every one the guest could see flagged", caught, nplants - left_out);
        check("planted stalls: at most 1 in 10 left out", left_out * 10 <= nplants, 1);
        check("planted stalls: no other step flagged", plant_stray, 0);
    } else
        check("the guest never saw time the host did not take (no step over 25 ms unexplained)", nflags, 0);

    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    system(cmd);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
