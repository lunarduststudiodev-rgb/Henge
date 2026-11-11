/* Frame gaps for the benchmark (frame_pace.h). */
#include "frame_pace.h"

#include <os/lock.h>
#include <stdatomic.h>
#include <string.h>

HvPace hv_pace_mac, hv_pace_shown;
HvFrameFlow hv_frame_flow;
_Atomic int hv_frame_watch;

void hv_frame_watch_set(int on) {
    if (on) {
        atomic_store_explicit(&hv_pace_mac.last_ns, 0, memory_order_relaxed);
        atomic_store_explicit(&hv_pace_shown.last_ns, 0, memory_order_relaxed);
    }
    atomic_store_explicit(&hv_frame_watch, on, memory_order_relaxed);
}

void hv_pace_note(HvPace *p, uint64_t now_ns) {
    uint64_t last = atomic_exchange_explicit(&p->last_ns, now_ns, memory_order_relaxed);
    if (last && now_ns > last) {
        uint64_t gap = now_ns - last, ms = gap / 1000000;
        atomic_fetch_add_explicit(&p->gaps[ms < HV_PACE_BUCKETS - 1 ? ms : HV_PACE_BUCKETS - 1], 1, memory_order_relaxed);
        if (gap > atomic_load_explicit(&p->longest_ns, memory_order_relaxed))
            atomic_store_explicit(&p->longest_ns, gap, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&p->frames, 1, memory_order_release);
}

void hv_pace_copy(HvPace *p, HvPaceCopy *out) {
    out->frames = atomic_load_explicit(&p->frames, memory_order_acquire);
    out->longest_ns = atomic_load_explicit(&p->longest_ns, memory_order_relaxed);
    for (int i = 0; i < HV_PACE_BUCKETS; i++) out->gaps[i] = atomic_load_explicit(&p->gaps[i], memory_order_relaxed);
}

/* The gap length (ms) that a share q of the gaps is no longer than. */
static double quantile_of(const uint32_t *d, int buckets, uint64_t n, double q) {
    uint64_t want = (uint64_t)(q * (double)n + 0.999999), seen = 0;
    if (!want) want = 1;
    for (int i = 0; i < buckets; i++) {
        seen += d[i];
        if (seen >= want) return i;
    }
    return buckets - 1;
}
static double quantile(const uint32_t *d, uint64_t n, double q) { return quantile_of(d, HV_PACE_BUCKETS, n, q); }

void hv_pace_summary(const HvPaceCopy *a, const HvPaceCopy *b, double seconds, double target_ms, HvPaceSummary *s) {
    memset(s, 0, sizeof *s);
    uint32_t d[HV_PACE_BUCKETS];
    uint64_t n = 0, late = 0, stalled = 0;
    double dropped = 0;
    int longest = -1;
    for (int i = 0; i < HV_PACE_BUCKETS; i++) {
        d[i] = b->gaps[i] - a->gaps[i];
        if (!d[i]) continue;
        n += d[i];
        longest = i;
        if (target_ms <= 0) continue;
        /* bucket i holds gaps of i..i+1 ms: past a limit when all of it is */
        if (i >= 1.25 * target_ms) late += d[i];
        if (i >= 3.5 * target_ms) stalled += d[i];
        double gap = i == HV_PACE_BUCKETS - 1 && b->longest_ns > a->longest_ns ? b->longest_ns / 1e6 : i + 0.5;
        double missing = (double)(int64_t)(gap / target_ms + 0.25) - 1;
        if (missing > 0) dropped += missing * d[i];
    }
    s->frames = b->frames - a->frames;
    s->gaps = n;
    s->fps = seconds > 0 ? (double)s->frames / seconds : 0;
    if (!n) return;
    s->p50_ms = quantile(d, n, 0.5);
    s->p90_ms = quantile(d, n, 0.9);
    s->p99_ms = quantile(d, n, 0.99);
    s->max_ms = longest == HV_PACE_BUCKETS - 1 && b->longest_ns > a->longest_ns ? b->longest_ns / 1e6 : longest;
    s->late = (double)late / (double)n;
    s->stalled = (double)stalled / (double)n;
    s->dropped_per_s = seconds > 0 ? dropped / seconds : 0;
}

void hv_frame_replaced(void) { atomic_fetch_add_explicit(&hv_frame_flow.replaced, 1, memory_order_relaxed); }

void hv_frame_presented(uint64_t shown_ns, uint64_t swap_ns) {
    if (!shown_ns) {
        atomic_fetch_add_explicit(&hv_frame_flow.not_shown, 1, memory_order_relaxed);
        return;
    }
    /* Metal's presented handlers may come on more than one thread: one note at a time, as hv_pace_note wants */
    static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    os_unfair_lock_lock(&lock);
    hv_pace_note(&hv_pace_shown, shown_ns);
    if (swap_ns && shown_ns > swap_ns) {
        uint64_t d = shown_ns - swap_ns, ms = d / 1000000;
        atomic_fetch_add_explicit(&hv_frame_flow.delay[ms < HV_PACE_BUCKETS - 1 ? ms : HV_PACE_BUCKETS - 1], 1, memory_order_relaxed);
        if (d > atomic_load_explicit(&hv_frame_flow.longest_delay_ns, memory_order_relaxed))
            atomic_store_explicit(&hv_frame_flow.longest_delay_ns, d, memory_order_relaxed);
        atomic_fetch_add_explicit(&hv_frame_flow.delays, 1, memory_order_release);
    }
    os_unfair_lock_unlock(&lock);
}

static void span_note(HvSpans *s, uint64_t ns) {
    uint64_t b = ns / 100000;
    atomic_fetch_add_explicit(&s->span[b < HV_SPAN_BUCKETS - 1 ? b : HV_SPAN_BUCKETS - 1], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->ns, ns, memory_order_relaxed);
    uint64_t longest = atomic_load_explicit(&s->longest_ns, memory_order_relaxed);
    while (ns > longest &&
           !atomic_compare_exchange_weak_explicit(&s->longest_ns, &longest, ns, memory_order_relaxed, memory_order_relaxed)) {
    }
    atomic_fetch_add_explicit(&s->n, 1, memory_order_release);
}
void hv_frame_held(uint64_t ns) { span_note(&hv_frame_flow.held, ns); }
void hv_frame_guest_waited(uint64_t ns) { span_note(&hv_frame_flow.guest_waited, ns); }

static void span_copy(HvSpans *s, HvSpansCopy *out) {
    out->n = atomic_load_explicit(&s->n, memory_order_acquire);
    out->ns = atomic_load_explicit(&s->ns, memory_order_relaxed);
    out->longest_ns = atomic_load_explicit(&s->longest_ns, memory_order_relaxed);
    for (int i = 0; i < HV_SPAN_BUCKETS; i++) out->span[i] = atomic_load_explicit(&s->span[i], memory_order_relaxed);
}

static void span_summary(const HvSpansCopy *a, const HvSpansCopy *b, HvSpansSummary *s) {
    uint32_t d[HV_SPAN_BUCKETS];
    uint64_t n = 0;
    int longest = -1;
    for (int i = 0; i < HV_SPAN_BUCKETS; i++) {
        d[i] = b->span[i] - a->span[i];
        if (d[i]) n += d[i], longest = i;
    }
    s->n = n;
    if (!n) return;
    s->mean_ms = (double)(b->ns - a->ns) / (double)n / 1e6;
    s->p99_ms = quantile_of(d, HV_SPAN_BUCKETS, n, 0.99) / 10;
    s->max_ms = longest == HV_SPAN_BUCKETS - 1 && b->longest_ns > a->longest_ns ? b->longest_ns / 1e6 : longest / 10.0;
}

void hv_frame_flow_copy(HvFrameFlow *f, HvFrameFlowCopy *out) {
    out->delays = atomic_load_explicit(&f->delays, memory_order_acquire);
    out->replaced = atomic_load_explicit(&f->replaced, memory_order_relaxed);
    out->not_shown = atomic_load_explicit(&f->not_shown, memory_order_relaxed);
    out->longest_delay_ns = atomic_load_explicit(&f->longest_delay_ns, memory_order_relaxed);
    for (int i = 0; i < HV_PACE_BUCKETS; i++) out->delay[i] = atomic_load_explicit(&f->delay[i], memory_order_relaxed);
    span_copy(&f->held, &out->held);
    span_copy(&f->guest_waited, &out->guest_waited);
}

void hv_frame_flow_summary(const HvFrameFlowCopy *a, const HvFrameFlowCopy *b, HvFrameFlowSummary *s) {
    memset(s, 0, sizeof *s);
    s->replaced = b->replaced - a->replaced;
    s->not_shown = b->not_shown - a->not_shown;
    uint32_t d[HV_PACE_BUCKETS];
    uint64_t n = 0;
    int longest = -1;
    for (int i = 0; i < HV_PACE_BUCKETS; i++) {
        d[i] = b->delay[i] - a->delay[i];
        if (d[i]) n += d[i], longest = i;
    }
    s->delays = n;
    if (n) {
        s->delay_p50_ms = quantile(d, n, 0.5);
        s->delay_p99_ms = quantile(d, n, 0.99);
        s->delay_max_ms = longest == HV_PACE_BUCKETS - 1 && b->longest_delay_ns > a->longest_delay_ns ? b->longest_delay_ns / 1e6 : longest;
    }
    span_summary(&a->held, &b->held, &s->held);
    span_summary(&a->guest_waited, &b->guest_waited, &s->guest_waited);
}
