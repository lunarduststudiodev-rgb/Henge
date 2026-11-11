/* The freeze recorder (stallmon.h). One thread, every 10 ms:
   - looks at the screen's frame count and the person's input (stall_detect.c decides what counts as a stall);
   - times a tiny task on the main queue: one that waits long means Henge's own touch and keyboard handling stalled;
   - while either says a stall could be forming (hv_stall_watch: 50 ms in), and no more often than its budget lets
     it (hv_stall_budget_take), samples each guest CPU's thread and the app's main thread: it stops the thread for a
     moment, reads where it is and walks its frame pointers (only inside that thread's own stack, so a guest value in
     x29 can never send it elsewhere; nothing is allocated or locked while a thread is stopped), and keeps the last
     few seconds. Only then: a stopped CPU thread can hold up every other (a spinlock it holds, a TLBI ack another
     waits for), and samples leave only in the records of stalls. Reading a thread's registers stops it whatever
     the call (thread_get_state holds it), so there is no cheaper look at where it is.
   When something is found, the samples from that stretch, the engine's counters over it and the device's heat and
   free memory go out as one line of henge-freezes.jsonl. It stops looking while the Mac is paused and when it was
   itself held up, so neither is taken for a stall; a hold-up while Henge's own threads kept running (the recorder
   starved, not the app suspended) goes out as a "starved" record of its own.
   It runs at the highest QoS class, at or above the guest CPUs': it stops each for a moment to read it, and taken
   off its core in that moment it would leave that CPU stopped, maybe holding a lock every other CPU waits on. */
#include "stallmon.h"

#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if TARGET_OS_IPHONE
#include <os/proc.h>
#endif

#include "../core/cpu.h"
#include "../core/machine.h"
#include "stall_detect.h"
#include "vm.h"

extern void (*hv_prof_register)(void);

#define TICK_US HV_STALL_TICK_US
#define MAX_THREADS 12
#define RING (1u << 13)        /* samples kept: about 7 s of 12 threads at 100 a second */
#define COUNTER_RING 256       /* counter snapshots, one every 50 ms: about 12 s */
#define MAX_RECORDS 400        /* a run that stalls all the time needs no more to show it */
#define FILE_LIMIT (4u << 20)  /* then the file is moved to henge-freezes-old.jsonl and begun again */

/* A place's state. Only the monitor (and hv_stall_cpu_time) moves READY to SAMPLING and back; a thread that ends takes its place from READY
   (waiting out a sample under way), so the monitor never reads a thread's memory once the thread has begun to go. */
enum { SLOT_FREE, SLOT_READY, SLOT_SAMPLING, SLOT_FILLING };

typedef struct {
    thread_act_t port;
    uintptr_t stack_lo, stack_hi;
    HvCpu **self; /* the thread's hv_self (its address on that thread): which guest CPU it runs, once it does */
    char name[16];
    _Atomic int state;
} Thread;

static Thread threads[MAX_THREADS];
static _Atomic int nthreads; /* places ever used: the monitor looks at these */
static void (*prev_register)(void);
static HvStallHooks hooks;
static char log_path[1024], old_path[1024];
static _Atomic int records;
static _Atomic unsigned ticks_judged; /* ticks whose held-up check is done, its record written (hv_stall_ticks) */
static uintptr_t image_base;
static char image_id[96];
static mach_timebase_info_data_t tb;

static uint64_t now_us(void) { return mach_absolute_time() * tb.numer / tb.denom / 1000; }
/* CPU time Henge's threads have used, all of them together. */
static uint64_t process_cpu_us(void) {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    return (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
}
static uint64_t ticks_ms(uint64_t ticks) { return ticks * tb.numer / tb.denom / 1000000; }

/* Darwin's per-thread exit hook for thread-local objects: it runs on the thread as it ends, before the thread's
   thread-local memory goes (pthread key destructors run after, and read it freed: then hv_self is whatever was put
   there next, 0x4 or a port name, and a sample read through it faulted). Exported by libdyld (C++ thread_local
   destructors use it) on macOS and iOS alike. */
extern void _tlv_atexit(void (*fn)(void *), void *arg);

static void thread_gone(void *arg) {
    Thread *t = &threads[(intptr_t)arg];
    for (;;) {
        int want = SLOT_READY;
        if (atomic_compare_exchange_strong(&t->state, &want, SLOT_FILLING)) break;
        usleep(100); /* the monitor is sampling it: a moment */
    }
    mach_port_deallocate(mach_task_self(), t->port);
    t->self = NULL;
    atomic_store_explicit(&t->state, SLOT_FREE, memory_order_release);
}

static int add_thread(HvCpu **self, const char *name) {
    for (int i = 0; i < MAX_THREADS; i++) {
        Thread *t = &threads[i];
        int want = SLOT_FREE;
        if (!atomic_compare_exchange_strong(&t->state, &want, SLOT_FILLING)) continue;
        pthread_t me = pthread_self();
        t->stack_hi = (uintptr_t)pthread_get_stackaddr_np(me);
        t->stack_lo = t->stack_hi - pthread_get_stacksize_np(me);
        t->self = self;
        snprintf(t->name, sizeof t->name, "%s", name);
        t->port = mach_thread_self();
        int n = atomic_load(&nthreads);
        while (n < i + 1 && !atomic_compare_exchange_weak(&nthreads, &n, i + 1)) {
        }
        /* A thread that ends leaves the list first (the main thread never does) */
        if (self) _tlv_atexit(thread_gone, (void *)(intptr_t)i);
        atomic_store_explicit(&t->state, SLOT_READY, memory_order_release);
        return i;
    }
    return -1;
}

/* Called on each thread that will run guest code, before it does (host/vm.c hv_vm_prepare_cpu_thread). */
static void register_cpu_thread(void) {
    if (prev_register) prev_register();
    add_thread(&hv_self, "cpu?");
}

/* ---- sampling ---- */

static HvStallSample ring[RING];
static uint32_t ring_head; /* only the monitor thread touches the ring */

static int in_stack(const Thread *t, uint64_t p, size_t len) {
    return p >= t->stack_lo && p + len <= t->stack_hi && !(p & 7);
}

/* User addresses have no more than 47 bits; anything above is a pointer-authentication signature. */
static uint64_t strip(uint64_t a) { return a & 0x00007fffffffffffull; }

/* Which guest block a PC in translated code is in (hv_block_at: blocks lie in their zone of the JIT region in about
   the order they were made). The arrays have a fixed size, so reading them while another CPU adds to them can only
   give a wrong answer, never a fault. */
static const HvBlock *block_at(HvMachine *m, const uint32_t *pc) { return hv_block_at(m, pc); }

/* Is the instruction before pc a system call (svc)? A thread blocked there waits for something it asked the kernel
   for: a lock, a condition, I/O. Blocked anywhere else, the kernel stopped it at a load or store, to bring the page
   in (from the file, the compressor or swap). Translated code never calls the kernel. Read with vm_read_overwrite,
   which fails rather than faults where nothing is mapped, after the thread runs again: no lock is taken. */
static int after_svc(uint64_t pc, int in_jit) {
    if (in_jit || pc < 4) return 0;
    uint32_t insn = 0;
    vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)(pc - 4), 4, (vm_address_t)&insn, &got) != KERN_SUCCESS ||
        got != 4)
        return 1; /* can't tell: as before, a wait */
    return (insn & 0xffe0001fu) == 0xd4000001u; /* svc #imm */
}

/* A seam for tests/test_stallmon_wake.c, which includes this file: called with 0 between the first look at a thread
   and stopping it, and with 1 once it is let go. Nothing in the app. */
#ifndef HV_STALL_LOOK_HOOK
#define HV_STALL_LOOK_HOOK(where) ((void)0)
#endif

/* A thread's run state and the CPU time it has used, user and system, in microseconds. */
static int thread_now(thread_t port, int *running, uint64_t *cpu_us) {
    thread_basic_info_data_t ti;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO, (thread_info_t)&ti, &n) != KERN_SUCCESS) return 0;
    if (running) *running = ti.run_state == TH_STATE_RUNNING; /* or ready to: taken off its core */
    *cpu_us = ((uint64_t)ti.user_time.seconds + (uint64_t)ti.system_time.seconds) * 1000000u +
              (uint64_t)ti.user_time.microseconds + (uint64_t)ti.system_time.microseconds;
    return 1;
}

/* One look at a thread: whether it was running (or ready to) when first looked at, its PC and host frames read while
   it was stopped, and, if it was blocked, whether it stayed blocked until the stop or woke in between. 0 if it
   couldn't be read.

   A thread seen blocked may wake before it is stopped, and its PC is then wherever it got to, not where it was
   blocked: in its own code, which is how a page fault looks. Its run state can't tell (a stopped thread reads as
   waiting, running or not, and once let go it may have blocked again), but its CPU time can: it runs only if it
   woke, and waking alone costs it a few microseconds of system time, more than the 1 us the time is counted in.
   Unchanged between the first look and the stop, it never woke, and the PC is where it blocked. The time moves in
   1 us steps (THREAD_EXTENDED_INFO's nanoseconds too), so a wake that cost under 1 us and crossed no step would
   still read as blocked, and its own code as a fault: the probe behind this never saw one in ~710 wakes, and the
   shortest it saw moved the time by 1 us. Changed, it woke: sample() looks again. */
typedef struct {
    int running, blocked, woke, spinning, in_jit, nbt;
    uint64_t pc, bt[HV_STALL_BT];
} Look;

static _Atomic unsigned skipped; /* samples not taken: the thread woke during both looks (sample()); for the tests */
static _Atomic unsigned suspends; /* threads stopped to be read, ever: for the tests (tests/test_stallmon_quiet.c) */

static int look(Thread *t, HvMachine *m, HvCpu *cpu, Look *l) {
    memset(l, 0, sizeof *l);
    uint64_t before, after;
    if (!thread_now(t->port, &l->running, &before)) return 0;
    HV_STALL_LOOK_HOOK(0);

    arm_thread_state64_t st;
    mach_msg_type_number_t sn = ARM_THREAD_STATE64_COUNT;
    if (thread_suspend(t->port) != KERN_SUCCESS) return 0;
    atomic_fetch_add_explicit(&suspends, 1, memory_order_relaxed);
    /* Returns once the thread is off its core, so its CPU time is then counted up to the stop */
    kern_return_t kr = thread_get_state(t->port, ARM_THREAD_STATE64, (thread_state_t)&st, &sn);
    /* Read while it is stopped, so it is where the PC is: the in-place WFE wait (dispatch.c wfe) */
    l->spinning = cpu && cpu->pc_state && __atomic_load_n(&cpu->pc_state->wfe_waiting, __ATOMIC_RELAXED);
    if (kr == KERN_SUCCESS && !thread_now(t->port, NULL, &after)) kr = KERN_FAILURE;
    if (kr == KERN_SUCCESS) {
        l->woke = !l->running && after != before;
        l->blocked = !l->running && !l->woke;
        l->pc = strip((uint64_t)arm_thread_state64_get_pc(st));
        uintptr_t jlo = (uintptr_t)m->jit.base, jhi = jlo + m->jit.capacity * 4;
        l->in_jit = jlo && l->pc >= jlo && l->pc < jhi;
        if (!l->in_jit) { /* translated code keeps the guest's x29 and x30: no host frames to read there */
            uint64_t lr = strip((uint64_t)arm_thread_state64_get_lr(st));
            uint64_t fp = (uint64_t)arm_thread_state64_get_fp(st);
            if (lr) l->bt[l->nbt++] = lr;
            while (l->nbt < HV_STALL_BT && in_stack(t, fp, 16)) {
                const uint64_t *frame = (const uint64_t *)(uintptr_t)fp;
                uint64_t next = frame[0], ret = strip(frame[1]);
                if (!ret) break;
                if (!(l->nbt == 1 && ret == l->bt[0])) l->bt[l->nbt++] = ret; /* lr is often the first frame's own return */
                if (next <= fp) break;
                fp = next;
            }
        }
    }
    thread_resume(t->port);
    HV_STALL_LOOK_HOOK(1);
    return kr == KERN_SUCCESS;
}

static void sample(int i, HvMachine *m, uint64_t t_us) {
    Thread *t = &threads[i];
    HvCpu *cpu = t->self ? __atomic_load_n(t->self, __ATOMIC_RELAXED) : NULL;
    if (cpu && t->name[3] == '?') snprintf(t->name, sizeof t->name, "cpu%u", cpu->index);
    if (t->self && !cpu) return; /* a CPU thread that hasn't started its CPU yet */

    HvStallSample *s = &ring[ring_head % RING];
    memset(s, 0, sizeof *s);
    s->t_us = t_us;
    s->thread = (uint8_t)i;
    if (cpu && cpu->pc_state && !__atomic_load_n(&cpu->pc_state->active, __ATOMIC_RELAXED)) {
        /* asleep in WFI, off, or held at a pause or flush: where it sleeps tells nothing */
        s->what = HV_SAMPLE_IDLE;
        ring_head++;
        return;
    }

    Look l;
    if (!look(t, m, cpu, &l)) return;
    /* Woke between the first look and the stop: where it got to is its own code, or a new block (a call to the
       kernel, or a page fault, which looks like its own code). One fresh look tells, as it is now. */
    if (l.woke && !look(t, m, cpu, &l)) return;
    int running = l.running, spinning = l.spinning, in_jit = l.in_jit, nbt = l.nbt;
    uint64_t pc = l.pc;
    const uint64_t *bt = l.bt;
    /* Stopped in a call to the kernel, or elsewhere. Asked only of a thread that was blocked, as before. */
    int svc = (l.blocked || l.woke) && after_svc(pc, in_jit);
    /* Woken again during the fresh look, and not in a call to the kernel: running Henge's own code or translated code
       (which never calls the kernel, so svc is 0 there), or blocked in a new page fault in either, and nothing tells
       which. No sample, rather than one that may be wrong (engine or guest for a fault); skipped counts them. */
    if (l.woke && !svc && !spinning) {
        atomic_fetch_add_explicit(&skipped, 1, memory_order_relaxed);
        return;
    }

    s->nbt = (uint8_t)nbt;
    memcpy(s->bt, bt, (size_t)nbt * sizeof bt[0]);
    if (spinning) { /* macOS's spinlocks: guest CPUs waiting on each other, not Henge's code */
        s->what = HV_SAMPLE_GUEST_SPIN;
        s->pc = pc;
    } else if (l.blocked && !svc) { /* blocked all along, but not in a call to the kernel: a fault */
        s->what = HV_SAMPLE_FAULT;
        s->pc = pc;
    } else if (in_jit) {
        const HvBlock *b = block_at(m, (const uint32_t *)(uintptr_t)pc);
        int el1 = b ? (b->flags & HV_BLOCK_EL1) != 0 : cpu && cpu->el > 0;
        s->what = el1 ? HV_SAMPLE_GUEST_KERNEL : HV_SAMPLE_GUEST_APP;
        s->pc = b ? b->va : 0;
    } else {
        /* Running, it is in its own code; else blocked in a call to the kernel (woke, and went back to one): waiting */
        s->what = running ? HV_SAMPLE_ENGINE : HV_SAMPLE_WAITING;
        s->pc = pc;
    }
    ring_head++;
}

/* ---- counters ---- */

typedef struct {
    uint64_t t_us, c[HV_STALL_COUNTERS];
} Counters;
_Static_assert(HV_STALL_GFX == 5, "hv_vm_gfx_stages fills five");
static Counters counter_ring[COUNTER_RING];
static HvStallGfxSnap gfx_ring[COUNTER_RING]; /* the graphics stages' ages at the same snapshots (hv_vm_gfx_stages) */
static uint32_t counter_head;

static void snap_counters(HvMachine *m, uint64_t t_us) {
    HvStallGfxSnap *g = &gfx_ring[counter_head % COUNTER_RING];
    Counters *c = &counter_ring[counter_head++ % COUNTER_RING];
    c->t_us = g->t_us = t_us;
    /* Each CPU counts into its own (HvPerCpu.stats): add them up. All of these are counted without --stats.
       "refills" are the light refills (every TLB miss in translated code); records before 9 Nov had "tlb_misses",
       counted only with --stats. */
#define T(f) HV_STAT_TOTAL(m, f)
    uint64_t v[HV_STALL_COUNTERS] = {
        T(translations), T(block_flushes), T(flush_full), T(flush_smc), T(syncs), ticks_ms(T(sync_ticks)),
        T(dev_waits), ticks_ms(T(dev_wait_ticks)), T(tr_waits), ticks_ms(T(tr_wait_ticks)), T(code_protects),
        T(refills),
    };
#undef T
    memcpy(c->c, v, sizeof v);
    hv_vm_gfx_stages(g->ms);
}

/* How much each counter grew from the snapshot nearest before start to the newest one. */
static void counter_growth(uint64_t start_us, uint64_t *out) {
    memset(out, 0, HV_STALL_COUNTERS * sizeof *out);
    if (!counter_head) return;
    const Counters *last = &counter_ring[(counter_head - 1) % COUNTER_RING], *from = NULL;
    uint32_t n = counter_head < COUNTER_RING ? counter_head : COUNTER_RING;
    for (uint32_t k = 1; k <= n; k++) {
        const Counters *c = &counter_ring[(counter_head - k) % COUNTER_RING];
        from = c;
        if (c->t_us <= start_us) break;
    }
    for (int k = 0; k < HV_STALL_COUNTERS; k++) out[k] = last->c[k] >= from->c[k] ? last->c[k] - from->c[k] : 0;
}

/* ---- the main thread ---- */

static _Atomic uint64_t ping_sent, ping_done;
static _Atomic int main_added;

static void ping(void *unused) {
    (void)unused;
    if (!atomic_exchange(&main_added, 1)) add_thread(NULL, "main");
    atomic_store(&ping_done, now_us());
}

/* ---- writing ---- */

static void label(uint64_t addr, char *buf, size_t len) {
    Dl_info di;
    if (!dladdr((void *)(uintptr_t)addr, &di)) {
        snprintf(buf, len, "0x%llx", (unsigned long long)addr);
        return;
    }
    if ((uintptr_t)di.dli_fbase == image_base) { /* the offset is what tools/freeze_report.py symbolizes */
        int n = snprintf(buf, len, "app+0x%llx", (unsigned long long)(addr - image_base));
        if (di.dli_sname && di.dli_saddr && n > 0 && (size_t)n < len && addr - (uintptr_t)di.dli_saddr < 0x10000)
            snprintf(buf + n, len - (size_t)n, " %s+0x%llx", di.dli_sname,
                     (unsigned long long)(addr - (uintptr_t)di.dli_saddr));
        return;
    }
    const char *lib = di.dli_fname ? strrchr(di.dli_fname, '/') : NULL;
    lib = lib ? lib + 1 : di.dli_fname ? di.dli_fname : "?";
    if (di.dli_sname && di.dli_saddr)
        snprintf(buf, len, "%s:%s+0x%llx", lib, di.dli_sname, (unsigned long long)(addr - (uintptr_t)di.dli_saddr));
    else snprintf(buf, len, "%s+0x%llx", lib, (unsigned long long)(addr - (uintptr_t)di.dli_fbase));
}

static void find_image(void) {
    Dl_info di;
    if (!dladdr((void *)hv_stall_start, &di)) return;
    image_base = (uintptr_t)di.dli_fbase;
    const struct mach_header_64 *h = di.dli_fbase;
    const uint8_t *lc = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; h->magic == MH_MAGIC_64 && i < h->ncmds; i++) {
        const struct load_command *c = (const void *)lc;
        if (c->cmd == LC_UUID) {
            const uint8_t *u = ((const struct uuid_command *)c)->uuid;
            snprintf(image_id, sizeof image_id,
                     "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X@0x%lx", u[0], u[1], u[2],
                     u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15],
                     (unsigned long)image_base);
            return;
        }
        lc += c->cmdsize;
    }
    snprintf(image_id, sizeof image_id, "?@0x%lx", (unsigned long)image_base);
}

int hv_stall_thermal(void) {
    static int token = -1, tried;
    if (!tried) {
        tried = 1;
        if (notify_register_check("com.apple.system.thermalpressurelevel", &token) != NOTIFY_STATUS_OK) token = -1;
    }
    uint64_t v = 0;
    if (token < 0 || notify_get_state(token, &v) != NOTIFY_STATUS_OK) return -1;
    /* The notification's state counts in tens (10 moderate, 20 heavy, 30 trapping, 40 sleeping), as an iPad Pro
       logged it; the record keeps OSThermalPressureLevel's 0..4. */
    return (int)(v >= 10 ? v / 10 : v);
}

static uint64_t started_us;
static HvStallSample window[RING];

/* Every stall found, for the benchmark: those too close to the last of their kind to be recorded too. */
static _Atomic unsigned found[6];
static _Atomic uint64_t found_ms;

static void count(const HvStallEvent *ev) {
    if (ev->kind >= 0 && ev->kind < 6) atomic_fetch_add(&found[ev->kind], 1);
    if (ev->kind != HV_STALL_HANG && ev->kind != HV_STALL_APP_HANG) /* a hang is told again, whole, as it ends */
        atomic_fetch_add(&found_ms, (ev->end_us - ev->start_us) / 1000);
}

static void record(const HvStallEvent *ev) {
    if (atomic_load(&records) >= MAX_RECORDS) return;
    /* The stretch's samples, from a little before it began. */
    uint64_t from = ev->start_us > 100000 ? ev->start_us - 100000 : 0, oldest = UINT64_MAX;
    int n = 0;
    uint32_t have = ring_head < RING ? ring_head : RING;
    for (uint32_t k = 1; k <= have; k++) {
        const HvStallSample *s = &ring[(ring_head - k) % RING];
        if (s->t_us < from) break;
        if (s->t_us <= ev->end_us) window[n++] = *s;
        if (s->t_us < oldest) oldest = s->t_us;
    }
    const char *names[MAX_THREADS];
    int nt = atomic_load(&nthreads);
    if (nt > MAX_THREADS) nt = MAX_THREADS;
    for (int i = 0; i < nt; i++) names[i] = threads[i].name;

    HvStallRecordInfo info = {.ev = ev, .image = image_id, .thermal = hv_stall_thermal(), .avail_mb = -1};
#if TARGET_OS_IPHONE
    info.avail_mb = (int64_t)(os_proc_available_memory() >> 20);
#endif
    info.uptime_s = (double)(ev->end_us - started_us) / 1e6;
    if (n) info.covered_us = ev->end_us - (oldest > ev->start_us ? oldest : ev->start_us);
    counter_growth(ev->start_us, info.counters);
    hv_stall_gfx_held(gfx_ring, counter_head < COUNTER_RING ? counter_head : COUNTER_RING, ev->start_us, ev->end_us, info.gfx_ms);
    char when[40];
    time_t wall = time(NULL) - (time_t)((now_us() - ev->end_us) / 1000000);
    struct tm tm;
    gmtime_r(&wall, &tm);
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tm);
    info.when = when;

    struct stat sb;
    if (stat(log_path, &sb) == 0 && sb.st_size > (off_t)FILE_LIMIT) rename(log_path, old_path);
    FILE *f = fopen(log_path, "a");
    if (!f) return;
    char line[512];
    hv_stall_write(f, &info, window, n, names, nt, label, line, sizeof line);
    fclose(f);
    atomic_fetch_add(&records, 1);
    fprintf(stderr, "henge %s\n", line);
}

/* ---- the loop ---- */

static void *monitor(void *unused) {
    (void)unused;
    pthread_setname_np("henge.freezes");
    HvStallDetector det;
    hv_stall_reset(&det);
    HvStallBudget budget;
    hv_stall_budget_fill(&budget);
    uint64_t last_tick = 0, last_cpu = 0, last_record[HV_STALL_STARVED + 1] = {0};
    int app_open = 0, app_hang_told = 0, was_live = 0;
    uint32_t tick = 0;
    for (;;) {
        usleep(TICK_US);
        uint64_t now = now_us(), cpu_used = process_cpu_us();
        int held_up = last_tick && now - last_tick > 50 * TICK_US; /* iOS stopped the app, or starved this thread */
        HvMachine *m = hv_vm_machine();
        int live = hooks.running() && !hv_vm_paused() && m && m->jit.base;
        /* Held up with the Mac running before and after, while Henge's threads used at least a quarter of a core
           over it: not the app suspended (then nothing in it runs) but this thread kept off its core. That is a
           freeze too, maybe one it caused by holding a CPU stopped (see the top), and its samples are missing. */
        if (held_up && was_live && live && cpu_used - last_cpu >= (now - last_tick) / 4) {
            HvStallEvent ev = {.kind = HV_STALL_STARVED, .start_us = last_tick, .end_us = now};
            if (now - last_record[ev.kind] >= 1000000) last_record[ev.kind] = now, record(&ev);
        }
        last_tick = now, last_cpu = cpu_used, was_live = live;
        atomic_fetch_add_explicit(&ticks_judged, 1, memory_order_release);
        if (held_up || !live) {
            hv_stall_reset(&det);
            atomic_store(&ping_sent, 0), atomic_store(&ping_done, 0);
            app_open = app_hang_told = 0;
            continue;
        }
        double in = hooks.input_idle_s(), pr = hooks.press_idle_s();
        uint64_t input_at = in >= 0 && in < 1e6 ? now - (uint64_t)(in * 1e6) : 0;
        uint64_t press_at = pr >= 0 && pr < 1e6 ? now - (uint64_t)(pr * 1e6) : 0;
        uint64_t sent = atomic_load(&ping_sent), done = atomic_load(&ping_done);
        uint64_t ping_wait = sent && done < sent && now > sent ? now - sent : 0;
        uint64_t frames = hv_vm_gpu_frames(); /* the watch and the step see the same */
        if (hv_stall_budget_take(&budget, hv_stall_watch(&det, now, frames, input_at, ping_wait))) {
            int nt = atomic_load(&nthreads);
            for (int i = 0; i < nt && i < MAX_THREADS; i++) {
                int want = SLOT_READY;
                if (atomic_compare_exchange_strong(&threads[i].state, &want, SLOT_SAMPLING)) {
                    sample(i, m, now);
                    atomic_store_explicit(&threads[i].state, SLOT_READY, memory_order_release);
                }
            }
        }
        if (tick++ % 5 == 0) snap_counters(m, now);

        HvStallEvent ev;
        int found = 0;
        /* Henge's main thread: a tiny task on its queue every 50 ms, timed. */
        if (done >= sent) {
            if (app_open) {
                ev = (HvStallEvent){.kind = HV_STALL_APP, .start_us = sent, .end_us = done, .ended_hang = app_hang_told};
                found = 1;
            }
            app_open = app_hang_told = 0;
            if (now - sent >= 50000) {
                atomic_store(&ping_sent, now);
                dispatch_async_f(dispatch_get_main_queue(), NULL, ping);
            }
        } else if (now - sent >= HV_STALL_APP_US) {
            app_open = 1;
            if (!app_hang_told && now - sent >= HV_STALL_HANG_US) {
                app_hang_told = 1;
                ev = (HvStallEvent){.kind = HV_STALL_APP_HANG, .start_us = sent, .end_us = now};
                found = 1;
            }
        }
        if (found) count(&ev);
        if (found && now - last_record[ev.kind] >= 1000000) last_record[ev.kind] = now, record(&ev);

        if (hv_stall_step(&det, now, frames, input_at, press_at, &ev) && (count(&ev), 1) &&
            (ev.kind == HV_STALL_HANG || now - last_record[ev.kind] >= 1000000)) {
            last_record[ev.kind] = now;
            record(&ev);
        }
    }
    return NULL;
}

void hv_stall_start(const char *dir, const HvStallHooks *h) {
    static int started;
    if (started++) return;
    const char *off = getenv("HV_FREEZES");
    if (off && !strcmp(off, "0")) return;
    if (!dir || !h || !h->running || !h->input_idle_s || !h->press_idle_s) return;
    mach_timebase_info(&tb);
    hooks = *h;
    snprintf(log_path, sizeof log_path, "%s/henge-freezes.jsonl", dir);
    snprintf(old_path, sizeof old_path, "%s/henge-freezes-old.jsonl", dir);
    find_image();
    started_us = now_us();
    prev_register = hv_prof_register;
    hv_prof_register = register_cpu_thread;
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0); /* at or above the guest CPUs (see the top) */
    if (pthread_create(&t, &a, monitor, NULL) == 0) pthread_detach(t);
    pthread_attr_destroy(&a);
    fprintf(stderr, "henge: freeze recorder on (%s)\n", log_path);
}

int hv_stall_records(void) { return atomic_load(&records); }
unsigned hv_stall_ticks(void) { return atomic_load_explicit(&ticks_judged, memory_order_acquire); }

int hv_stall_threads(void) {
    int n = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        int st = atomic_load(&threads[i].state);
        n += st == SLOT_READY || st == SLOT_SAMPLING;
    }
    return n;
}

void hv_stall_found(unsigned counts[6], uint64_t *total_ms) {
    for (int k = 0; k < 6; k++) counts[k] = atomic_load(&found[k]);
    *total_ms = atomic_load(&found_ms);
}

int hv_stall_cpu_time(double *seconds) {
    int n = 0;
    *seconds = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        Thread *t = &threads[i];
        int want;
        /* Held as the monitor holds a thread it samples (waiting out a sample under way), so it can't end meanwhile. */
        while ((want = SLOT_READY), !atomic_compare_exchange_strong(&t->state, &want, SLOT_SAMPLING) && want == SLOT_SAMPLING) usleep(50);
        if (want != SLOT_READY) continue; /* free, or being filled or emptied */
        thread_basic_info_data_t ti;
        mach_msg_type_number_t ni = THREAD_BASIC_INFO_COUNT;
        if (t->self && thread_info(t->port, THREAD_BASIC_INFO, (thread_info_t)&ti, &ni) == KERN_SUCCESS) {
            *seconds += ti.user_time.seconds + ti.user_time.microseconds / 1e6 + ti.system_time.seconds + ti.system_time.microseconds / 1e6;
            n++;
        }
        atomic_store_explicit(&t->state, SLOT_READY, memory_order_release);
    }
    return n;
}
