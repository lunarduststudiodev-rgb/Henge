/* The benchmark's host side (bench.h): asks the helper for a run, reads Henge's own counters as each step starts and
   ends, and writes henge-bench.jsonl. */
#include "bench.h"

#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>

#include "../core/machine.h"
#include "share.h"
#include "stallmon.h"
#include "vm.h"

extern char **environ;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char path[1024];
static _Atomic int state = HV_BENCH_NONE;
static char run_id[64];
static uint64_t run_began_ns;
static unsigned run_link; /* the helper's link the run was asked on (share.h): only its lines count */
/* the step under way: its name, target and the reading at its start */
static char step[32];
static double step_target;
static HvBenchSnap before;

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static double ticks_s(uint64_t ticks) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)ticks * tb.numer / tb.denom / 1e9;
}

static void snap(HvBenchSnap *s) {
    memset(s, 0, sizeof *s);
    s->t_ns = now_ns();
    {
        /* Each CPU counts into its own block (HvPerCpu.stats), other threads into the machine's: add them all up.
           The machine's own block alone holds next to nothing a CPU counts. */
        HvMachine *m = hv_vm_machine();
        HvStats st;
        hv_stats_sum(m, &st);
        s->blocks = st.blocks;
        s->translations = st.translations;
        s->retrans_evicted = st.retrans_evicted; /* code thrown out before, made again */
        s->retrans_written = st.retrans_written;
        s->lost_races = st.lost_races;
        s->page_drops = st.flush_smc; /* pages whose code a write dropped */
        s->retired = st.retired;
        s->retired_words = st.retired_words;
        s->rewrite_same = st.rewrite_same;
        s->rewrite_changed = st.rewrite_changed;
        for (int k = 0; k < 4; k++) s->entries_written[k] = st.entries_written[k], s->entries_evicted[k] = st.entries_evicted[k];
        s->tlbi_all_here = st.flush_why[0];
        s->tlb_full_drops = st.reset_why[0];
        s->sw_saved = st.stash_saves;
        s->sw_dropped = st.stash_stale;
        s->code_protects = st.code_protects;
        s->stats_counted = m->stats_interval != 0;
        s->sync_s = ticks_s(st.sync_ticks);
        s->wfi_s = ticks_s(st.wfi_ticks);
        s->cache_drops_all = st.block_flushes;
        s->cache_empties = st.zone_evictions;
        s->empties_in_stop = st.evict_sync;
        s->reclaim_kicks = st.reclaim_kicks;
        s->blocks_dropped = st.evicted_blocks;
        s->stops = st.stops; /* the stops are the machine's (core/dispatch.c stop_the_world), in the sum too */
        s->stop_s = ticks_s(st.stop_ticks);
        s->stop_wait_s = ticks_s(st.stop_wait_ticks);
        /* the longest stop since the last reading: a machine-level maximum, taken and reset here */
        s->stop_max_s = ticks_s(__atomic_exchange_n(&m->stats.stop_max_ticks, 0, __ATOMIC_RELAXED));
    }
    s->cpu_threads = hv_stall_cpu_time(&s->cpu_s);
    hv_stall_found(s->stalls, &s->stall_ms);
    s->thermal = hv_stall_thermal();
    hv_pace_copy(&hv_pace_mac, &s->mac);
    hv_pace_copy(&hv_pace_shown, &s->shown);
    hv_frame_flow_copy(&hv_frame_flow, &s->flow);
}

/* One line onto the file (under mu). */
static void write_line(const char *line) {
    FILE *f = fopen(path, "a");
    if (!f) {
        fprintf(stderr, "bench: can't write %s\n", path);
        return;
    }
    fprintf(f, "%s\n", line);
    fclose(f);
}

/* {"type":"done"...}: the run is over (under mu). */
static void finish(int ok, const char *why) {
    char line[512];
    size_t len = 0;
    snprintf(line, sizeof line, "{\"type\":\"done\",\"run\":");
    len = strlen(line);
    hv_bench_put_string(line, sizeof line, &len, run_id);
    snprintf(line + len, sizeof line - len, ",\"status\":\"%s\",\"why\":", ok ? "ok" : "failed");
    len = strlen(line);
    hv_bench_put_string(line, sizeof line, &len, why);
    snprintf(line + len, sizeof line - len, ",\"wall_s\":%.1f}", (double)(now_ns() - run_began_ns) / 1e9);
    write_line(line);
    step[0] = 0;
    hv_frame_watch_set(0);
    atomic_store(&state, ok ? HV_BENCH_DONE : HV_BENCH_FAILED);
    fprintf(stderr, "bench: run %s %s%s%s\n", run_id, ok ? "done" : "failed", *why ? ": " : "", why);
}

/* A word from the helper's line: up to the next space, at most n - 1 characters. Returns what follows it. */
static const char *word(const char *p, char *out, size_t n) {
    while (*p == ' ') p++;
    size_t i = 0;
    for (; *p && *p != ' '; p++)
        if (i + 1 < n) out[i++] = *p;
    out[i] = 0;
    while (*p == ' ') p++;
    return p;
}

/* What the helper says as the run goes (share.c's link thread), on the link `link`. */
static void heard(unsigned link, const char *line) {
    pthread_mutex_lock(&mu);
    if (atomic_load(&state) != HV_BENCH_RUNNING || link != run_link) {
        pthread_mutex_unlock(&mu);
        return;
    }
    char what[16], name[32];
    const char *rest = word(line, what, sizeof what);
    if (!strcmp(what, "begin")) {
        char target[16];
        word(word(rest, name, sizeof name), target, sizeof target);
        snprintf(step, sizeof step, "%s", name);
        step_target = atof(target);
        if (step_target < 0 || step_target > 1000) step_target = 0;
        snap(&before);
        fprintf(stderr, "bench: %s began\n", step);
    } else if (!strcmp(what, "end")) {
        char status[8], why[48] = "";
        rest = word(word(rest, name, sizeof name), status, sizeof status);
        const char *next = word(rest, why, sizeof why); /* a why, if the next word isn't KEY=NUMBER */
        if (strchr(why, '=')) why[0] = 0;
        else rest = next;
        if (strcmp(status, "ok") && strcmp(status, "skip")) snprintf(status, sizeof status, "fail");
        HvBenchSnap after;
        snap(&after);
        int begun = step[0] && !strcmp(step, name);
        if (!begun && strcmp(status, "skip")) { /* an end with no begin: nothing to measure it by */
            snprintf(status, sizeof status, "fail");
            if (!why[0]) snprintf(why, sizeof why, "no-begin");
        }
        static char out[8192];
        int n = hv_bench_step_json(out, sizeof out, run_id, name, status, why, begun ? step_target : 0, rest,
                                   begun ? &before : &after, &after, hv_vm_ncpus());
        if (n > 0) write_line(out);
        else fprintf(stderr, "bench: step %s's line didn't fit\n", name);
        fprintf(stderr, "bench: %s %s%s%s\n", name, status, why[0] ? " " : "", why);
        step[0] = 0;
    } else if (!strcmp(what, "note")) {
        char out[512];
        size_t len = 0;
        snprintf(out, sizeof out, "{\"type\":\"note\",\"run\":");
        len = strlen(out);
        if (hv_bench_put_string(out, sizeof out, &len, run_id) == 0 && snprintf(out + len, sizeof out - len, ",\"text\":") > 0 &&
            (len = strlen(out), hv_bench_put_string(out, sizeof out, &len, rest) == 0) && len + 2 < sizeof out) {
            strcat(out, "}");
            write_line(out);
        }
    } else if (!strcmp(what, "done")) {
        finish(1, "");
    } else if (!strcmp(what, "failed")) {
        finish(0, *rest ? rest : "helper");
    }
    pthread_mutex_unlock(&mu);
}

void hv_bench_start(const char *dir) {
    pthread_mutex_lock(&mu);
    if (!path[0] && dir) {
        snprintf(path, sizeof path, "%s/henge-bench.jsonl", dir);
        hv_share_set_bench_listener(heard);
    }
    pthread_mutex_unlock(&mu);
}

int hv_bench_state(void) { return atomic_load(&state); }

void hv_bench_give_up(const char *why) {
    pthread_mutex_lock(&mu);
    if (atomic_load(&state) == HV_BENCH_RUNNING) finish(0, why && *why ? why : "gave-up");
    pthread_mutex_unlock(&mu);
}

/* The run's first line: what it was run on, so runs can be told apart and compared (under mu). */
static void write_header(const char *label, const char *steps, const char *build) {
    static char out[16384];
    size_t len = 0;
    char device[64] = "";
    size_t dn = sizeof device - 1;
    if (sysctlbyname("hw.machine", device, &dn, NULL, 0) != 0) device[0] = 0;
    time_t t = time(NULL);
    char when[32];
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S%z", localtime(&t));
    HvMachine *m = hv_vm_machine();
    int ok = snprintf(out, sizeof out, "{\"type\":\"run\",\"run\":") > 0;
    len = strlen(out);
#define STR(key, v) (ok = ok && snprintf(out + len, sizeof out - len, ",\"%s\":", key) > 0 && (len = strlen(out), 1) && \
                          hv_bench_put_string(out, sizeof out, &len, v) == 0)
    ok = ok && hv_bench_put_string(out, sizeof out, &len, run_id) == 0;
    STR("label", label);
    STR("build", build);
    STR("device", device);
    STR("when", when);
    STR("steps", steps);
    if (ok) {
        snprintf(out + len, sizeof out - len, ",\"ncpus\":%d,\"rosetta\":%d,\"agent\":%d,\"code_cache_mb\":%llu,\"env\":{",
                 hv_vm_ncpus(), m->rosetta, hv_share_agent_version(), 
                 (unsigned long long)((m->jit.capacity * sizeof(uint32_t) + (1u << 19)) >> 20)); /* capacity is in words */
        len = strlen(out);
    }
    /* the settings that change how Henge runs (henge-env.txt puts them here) */
    int first = 1;
    for (char **e = environ; ok && *e; e++) {
        if (strncmp(*e, "HV_", 3) && strncmp(*e, "ROSETTA_", 8) && strncmp(*e, "HENGE_", 6)) continue;
        const char *eq = strchr(*e, '=');
        if (!eq || eq - *e > 63) continue;
        char key[64];
        memcpy(key, *e, (size_t)(eq - *e));
        key[eq - *e] = 0;
        ok = snprintf(out + len, sizeof out - len, "%s", first ? "" : ",") >= 0 && (len = strlen(out), 1) &&
             hv_bench_put_string(out, sizeof out, &len, key) == 0 && len + 1 < sizeof out && (out[len++] = ':', out[len] = 0, 1) &&
             hv_bench_put_string(out, sizeof out, &len, eq + 1) == 0;
        first = 0;
    }
#undef STR
    if (ok && len + 3 < sizeof out) {
        strcat(out, "}}");
        write_line(out);
    } else {
        fprintf(stderr, "bench: the run's line didn't fit\n");
    }
}

int hv_bench_run(const char *label, const char *steps, const char *build, char *why, size_t why_len) {
    if (!label) label = "";
    for (const char *p = label; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-._", *p)) ||
            p - label >= 40) {
            snprintf(why, why_len, "a label is up to 40 letters, digits, - . or _");
            return -1;
        }
    pthread_mutex_lock(&mu);
    int rc = -1;
    if (!path[0]) snprintf(why, why_len, "the benchmark has nowhere to write (hv_bench_start)");
    else if (atomic_load(&state) == HV_BENCH_RUNNING) snprintf(why, why_len, "a run is already under way");
    else if (hv_vm_ncpus() <= 0) snprintf(why, why_len, "no Mac is running");
    else if (hv_share_screen_away() == 1) snprintf(why, why_len, "the Mac's screen is locked or asleep");
    else {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        char stamp[24];
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
        snprintf(run_id, sizeof run_id, "%s%s%s", stamp, *label ? "-" : "", label);
        unsigned link = hv_share_bench(steps);
        if (!link) {
            snprintf(why, why_len, "no helper that can run it is linked (version %d or later), or its link went", HV_AGENT_SINCE_BENCH);
        } else {
            run_link = link;
            /* The run's rewrites_same_pct: pages hashed as they become code from now (off outside a run). */
            HvMachine *bm = hv_vm_machine();
            if (bm) __atomic_store_n(&bm->rewrite_hash, 1, __ATOMIC_RELAXED);
            run_began_ns = now_ns();
            step[0] = 0;
            hv_frame_watch_set(1); /* the display notes its frames from now on (frame_pace.h) */
            write_header(label, steps, build ? build : "");
            atomic_store(&state, HV_BENCH_RUNNING);
            fprintf(stderr, "bench: run %s asked for (%s)\n", run_id, steps);
            rc = 0;
        }
    }
    pthread_mutex_unlock(&mu);
    return rc;
}
