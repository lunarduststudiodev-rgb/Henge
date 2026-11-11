/* A benchmark step's line (bench.h): the difference between the readings at its start and end. */
#include "bench.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int put(char *out, size_t cap, size_t *len, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int put(char *out, size_t cap, size_t *len, const char *fmt, ...) {
    if (*len >= cap) return -1;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *len) return -1;
    *len += (size_t)n;
    return 0;
}

int hv_bench_put_string(char *out, size_t cap, size_t *len, const char *s) {
    if (put(out, cap, len, "\"") != 0) return -1;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        int rc = c == '"' || c == '\\' ? put(out, cap, len, "\\%c", c) : c < 0x20 ? put(out, cap, len, "\\u%04x", c) : put(out, cap, len, "%c", c);
        if (rc != 0) return -1;
    }
    return put(out, cap, len, "\"");
}

/* A number: JSON has no NaN or infinity, and 3 decimals say all a benchmark needs. */
static int metric(char *out, size_t cap, size_t *len, int *first, const char *name, double v) {
    if (v != v || v > 1e15 || v < -1e15) return 0;
    if (put(out, cap, len, "%s\"%s\":", *first ? "" : ",", name) != 0) return -1;
    *first = 0;
    double r = (double)(long long)(v * 1000 + (v < 0 ? -0.5 : 0.5)) / 1000;
    return put(out, cap, len, "%.15g", r);
}

static int pace(char *out, size_t cap, size_t *len, int *first, const char *who, const HvPaceCopy *a, const HvPaceCopy *b,
                double secs, double target_ms, int full) {
    HvPaceSummary s;
    hv_pace_summary(a, b, secs, target_ms, &s);
    char name[48];
#define M(key, v)                                                                                                       \
    do {                                                                                                                \
        snprintf(name, sizeof name, "%s_%s", who, key);                                                                 \
        if (metric(out, cap, len, first, name, v) != 0) return -1;                                                      \
    } while (0)
    M("fps", s.fps);
    if (s.gaps) {
        M("p99_ms", s.p99_ms);
        if (full) {
            M("p50_ms", s.p50_ms);
            M("p90_ms", s.p90_ms);
            M("max_ms", s.max_ms);
        }
        if (target_ms > 0) {
            M("late_pct", 100 * s.late);
            M("dropped_per_s", s.dropped_per_s);
            if (full) M("stalled_pct", 100 * s.stalled);
        }
    }
#undef M
    return 0;
}

int hv_bench_step_json(char *out, size_t cap, const char *run, const char *step, const char *status, const char *why,
                       double target_ms, const char *agent, const HvBenchSnap *a, const HvBenchSnap *b, int ncpus) {
    size_t len = 0;
    double secs = (double)(b->t_ns - a->t_ns) / 1e9;
    if (secs <= 0) secs = 1e-9;
    if (put(out, cap, &len, "{\"type\":\"step\",\"run\":") || hv_bench_put_string(out, cap, &len, run) ||
        put(out, cap, &len, ",\"step\":") || hv_bench_put_string(out, cap, &len, step) || put(out, cap, &len, ",\"status\":") ||
        hv_bench_put_string(out, cap, &len, status) || put(out, cap, &len, ",\"why\":") || hv_bench_put_string(out, cap, &len, why) ||
        put(out, cap, &len, ",\"target_ms\":%.1f,\"thermal\":[%d,%d],", target_ms, a->thermal, b->thermal))
        return -1;
    /* Counted only with HV_STATS (core/machine.h HvStats): without it they read 0, which would look measured. */
    int counted = a->stats_counted && b->stats_counted;
    if (!counted && put(out, cap, &len, "\"not_counted\":[\"switches_saved_per_s\",\"switches_dropped_per_s\"],"))
        return -1;
    if (put(out, cap, &len, "\"m\":{")) return -1;
    if (!strcmp(status, "skip")) return put(out, cap, &len, "}}") != 0 ? -1 : (int)len;
    int first = 1;
#define M(key, v)                                                                                                       \
    do {                                                                                                                \
        if (metric(out, cap, &len, &first, key, v) != 0) return -1;                                                     \
    } while (0)
#define RATE(f) ((double)(b->f - a->f) / secs)
    M("wall_s", secs);
    /* How smoothly the Mac drew, and how smoothly Henge showed it. */
    if (pace(out, cap, &len, &first, "mac", &a->mac, &b->mac, secs, target_ms, 1) ||
        pace(out, cap, &len, &first, "shown", &a->shown, &b->shown, secs, target_ms, 0))
        return -1;
    /* Frames the Mac swapped that Henge replaced with a newer one before drawing, presents the screen never showed,
       and the time from the Mac's swap to the glass (frame_pace.h). */
    HvFrameFlowSummary fl;
    hv_frame_flow_summary(&a->flow, &b->flow, &fl);
    M("frames_replaced", (double)fl.replaced);
    M("presents_not_shown", (double)fl.not_shown);
    if (fl.delays) {
        M("swap_to_glass_p50_ms", fl.delay_p50_ms);
        M("swap_to_glass_p99_ms", fl.delay_p99_ms);
        M("swap_to_glass_max_ms", fl.delay_max_ms);
    }
    /* How long each present held the Mac's GPU work up behind it (from its place in the GPU's order to its commit), and
       how long the Mac's GPU batches waited from their commit to being scheduled, which a hold makes longer */
    if (fl.held.n) {
        M("present_hold_mean_ms", fl.held.mean_ms);
        M("present_hold_p99_ms", fl.held.p99_ms);
        M("present_hold_max_ms", fl.held.max_ms);
    }
    if (fl.guest_waited.n) {
        M("mac_gpu_wait_mean_ms", fl.guest_waited.mean_ms);
        M("mac_gpu_wait_p99_ms", fl.guest_waited.p99_ms);
        M("mac_gpu_wait_max_ms", fl.guest_waited.max_ms);
    }
    /* The freeze recorder's finds (stall_detect.h: 1 hitch, 2 slow response, 3 hang, 4 Henge's own main thread). A
       hang is told as it passes 1.5 s and again, as a hitch or slow response, when it ends: hangs are the ones of those
       that got that long. Kind 5 is kind 4's hang the same way, so it isn't added. */
    M("hitches", b->stalls[1] - a->stalls[1]);
    M("slow_responses", b->stalls[2] - a->stalls[2]);
    M("hangs", b->stalls[3] - a->stalls[3]);
    M("app_stalls", b->stalls[4] - a->stalls[4]);
    M("stall_ms", (double)(b->stall_ms - a->stall_ms));
    /* The engine. */
    M("blocks_m_per_s", RATE(blocks) / 1e6);
    M("translated_per_s", RATE(translations));
    /* Why code was translated again: thrown out by emptying its zone with its page unchanged since (the cache too small
       for what runs, or the wrong zone emptied), or its page written since; and translations another CPU's copy beat. */
    if (b->translations > a->translations) {
        double tr = (double)(b->translations - a->translations);
        M("retrans_evicted_pct", 100.0 * (double)(b->retrans_evicted - a->retrans_evicted) / tr);
        M("retrans_written_pct", 100.0 * (double)(b->retrans_written - a->retrans_written) / tr);
    }
    M("lost_races_per_s", RATE(lost_races));
    /* Writes to code: pages whose code they dropped, what each threw out, and how many left the bytes as they were. */
    M("page_drops_per_s", RATE(page_drops));
    if (b->page_drops > a->page_drops) {
        double drops = (double)(b->page_drops - a->page_drops);
        M("blocks_per_page_drop", (double)(b->retired - a->retired) / drops);
        M("kb_per_page_drop", (double)(b->retired_words - a->retired_words) * 4 / 1024 / drops);
    }
    uint64_t known = b->rewrite_same - a->rewrite_same + b->rewrite_changed - a->rewrite_changed;
    if (known) M("rewrites_same_pct", 100.0 * (double)(b->rewrite_same - a->rewrite_same) / (double)known);
    /* How often the dispatcher entered blocks before they were thrown out (not runs: chained runs leave no trace), as
       % of the blocks retired by a write, and of the live blocks of emptied zones: 1, 2-8, 9-64, more. */
    static const char *const bucket[4] = {"1", "2_8", "9_64", "65up"};
    for (int h = 0; h < 2; h++) {
        const uint64_t *x = h ? a->entries_evicted : a->entries_written, *y = h ? b->entries_evicted : b->entries_written;
        uint64_t n = 0;
        for (int k = 0; k < 4; k++) n += y[k] - x[k];
        for (int k = 0; n && k < 4; k++) {
            char key[48];
            snprintf(key, sizeof key, "entries_before_%s_%s_pct", h ? "evict" : "write", bucket[k]);
            M(key, 100.0 * (double)(y[k] - x[k]) / (double)n);
        }
    }
    M("tlbi_all_per_s", RATE(tlbi_all_here));
    M("tlb_full_drops_per_s", RATE(tlb_full_drops));
    if (counted) {
        M("switches_saved_per_s", RATE(sw_saved));
        M("switches_dropped_per_s", RATE(sw_dropped));
    }
    M("new_code_pages_per_s", RATE(code_protects));
    /* The code cache running out of room: whole drops, zones emptied and the blocks in them, and every CPU stopped
       meanwhile (in all, and the longest). */
    M("cache_drops_all", (double)(b->cache_drops_all - a->cache_drops_all));
    M("cache_empties", (double)(b->cache_empties - a->cache_empties));
    M("evict_sync", (double)(b->empties_in_stop - a->empties_in_stop));
    M("reclaim_kicks", (double)(b->reclaim_kicks - a->reclaim_kicks));
    M("blocks_dropped", (double)(b->blocks_dropped - a->blocks_dropped));
    M("stops", (double)(b->stops - a->stops));
    M("stop_ms", 1e3 * (b->stop_s - a->stop_s));
    M("stop_max_ms", 1e3 * b->stop_max_s);
    M("tlbi_wait_pct", 100 * (b->sync_s - a->sync_s) / secs); /* of one CPU, as the log's stats say it */
    if (ncpus > 0) M("idle_pct", 100 * (b->wfi_s - a->wfi_s) / (secs * ncpus));
    /* The share of the time the guest's CPUs wanted that the host gave them: all of it but WFI and being held at a
       code-cache stop (a CPU that is off, or waits on a lock, still counts as wanting it). */
    double wanted = secs * a->cpu_threads - (b->wfi_s - a->wfi_s) - (b->stop_wait_s - a->stop_wait_s);
    if (a->cpu_threads > 0 && a->cpu_threads == b->cpu_threads && wanted > 0.01 * secs) M("cpu_got_pct", 100 * (b->cpu_s - a->cpu_s) / wanted);
#undef RATE
    /* What the helper measured itself: KEY=NUMBER, each key lower case letters, digits and _. */
    for (const char *p = agent ? agent : ""; *p;) {
        while (*p == ' ') p++;
        const char *eq = strchr(p, '='), *end = strchr(p, ' ');
        if (!end) end = p + strlen(p);
        if (eq && eq < end && eq > p && eq - p <= 32) {
            char key[48] = "agent_", num[40];
            int ok = (size_t)(end - eq - 1) < sizeof num;
            for (const char *k = p; ok && k < eq; k++) ok = islower((unsigned char)*k) || isdigit((unsigned char)*k) || *k == '_';
            if (ok) {
                memcpy(key + 6, p, (size_t)(eq - p));
                key[6 + (eq - p)] = 0;
                memcpy(num, eq + 1, (size_t)(end - eq - 1));
                num[end - eq - 1] = 0;
                char *stop;
                double v = strtod(num, &stop);
                if (*num && !*stop) M(key, v);
            }
        }
        p = end;
    }
#undef M
    if (put(out, cap, &len, "}}") != 0) return -1;
    return (int)len;
}
