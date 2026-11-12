/* How evenly frames come: a count of the gaps between them by length, for the benchmark (host/bench.c) to read before
   and after each of its steps. Two are kept: the Mac's own frames (each time macOS hands the display a new picture,
   pvg_metal.m's swap) and the frames that reached the iPad's screen (the time Core Animation says each of Henge's
   presents was on the glass, the pointer's included). The Mac's say whether macOS itself kept up; the iPad's whether
   Henge showed them on time. And what became of the Mac's pictures on the way (HvFrameFlow): replaced by a newer one
   before Henge could show it, a present the screen never showed, and how long a picture took to reach the glass. And
   how Henge's presents and the Mac's own GPU work wait on each other on the one GPU queue (HvSpans): how long each
   present held the work queued after it (its hold: from taking its place in the queue's order, with the screen's
   texture, to its commit), and how long each of the Mac's own GPU batches waited from its commit to being scheduled
   (Metal's word: let go to the GPU, which may still be busy with earlier work; not when the GPU starts it). A batch
   can't be scheduled ahead of a present enqueued before it and not yet committed, so a hold makes this longer. */
#ifndef HV_FRAME_PACE_H
#define HV_FRAME_PACE_H

#include <stdatomic.h>
#include <stdint.h>

#define HV_PACE_BUCKETS 1001 /* gaps of 0..999 ms, 1 ms each; the last is 1000 ms and longer */
#define HV_SPAN_BUCKETS 201  /* spans of 0..19.9 ms, 0.1 ms each; the last is 20 ms and longer */

typedef struct {
    _Atomic uint64_t frames;
    _Atomic uint64_t last_ns;
    _Atomic uint64_t longest_ns; /* the longest gap ever, in full: the last bucket doesn't say */
    _Atomic uint32_t gaps[HV_PACE_BUCKETS];
} HvPace;

/* A plain copy, to subtract one from another. */
typedef struct {
    uint64_t frames, longest_ns;
    uint32_t gaps[HV_PACE_BUCKETS];
} HvPaceCopy;

typedef struct {
    uint64_t frames, gaps;  /* frames in the stretch, and the gaps measured between them */
    double fps;             /* frames over the stretch's length */
    double p50_ms, p90_ms, p99_ms, max_ms; /* gap lengths (to the bucket: 1 ms) */
    /* Against the step's target gap T (a 60 Hz screen's 16.7 ms, a 30 fps video's 33.3 ms). Gaps come in whole
       refreshes, so the limits sit between them: */
    double late;            /* the share of gaps of 1.25 T or more: a frame came at least one refresh late, 0..1 */
    double stalled;         /* ... of 3.5 T or more: three frames or more missing, a visible stutter */
    double dropped_per_s;   /* frames missing: a gap of about k T is k - 1 of them (from 1.75 T, 2.75 T, ...) */
} HvPaceSummary;

extern HvPace hv_pace_mac, hv_pace_shown;

/* Short spans of time (holds, waits): how many, in all, and each to the 0.1 ms */
typedef struct {
    _Atomic uint64_t n, ns;
    _Atomic uint32_t span[HV_SPAN_BUCKETS];
    _Atomic uint64_t longest_ns;
} HvSpans;
typedef struct {
    uint64_t n, ns, longest_ns;
    uint32_t span[HV_SPAN_BUCKETS];
} HvSpansCopy;
typedef struct {
    uint64_t n;
    double mean_ms, p99_ms, max_ms; /* p99 to the bucket (0.1 ms); max to the bucket, or the true longest past them */
} HvSpansSummary;

typedef struct {
    _Atomic uint64_t replaced;  /* the Mac's pictures replaced by its next before Henge took them to show */
    _Atomic uint64_t not_shown; /* presents the screen dropped (presentedTime 0) */
    _Atomic uint64_t delays;    /* pictures timed from the Mac's swap to the glass */
    _Atomic uint32_t delay[HV_PACE_BUCKETS]; /* those times, 1 ms each */
    _Atomic uint64_t longest_delay_ns;
    HvSpans held, guest_waited; /* hv_frame_held, hv_frame_guest_waited */
} HvFrameFlow;

typedef struct {
    uint64_t replaced, not_shown, delays, longest_delay_ns;
    uint32_t delay[HV_PACE_BUCKETS];
    HvSpansCopy held, guest_waited;
} HvFrameFlowCopy;

typedef struct {
    uint64_t replaced, not_shown, delays;
    double delay_p50_ms, delay_p99_ms, delay_max_ms;
    HvSpansSummary held, guest_waited;
} HvFrameFlowSummary;

extern HvFrameFlow hv_frame_flow;

/* A frame now (any thread; one at a time for each HvPace: each is noted from one queue). */
void hv_pace_note(HvPace *p, uint64_t now_ns);
void hv_pace_copy(HvPace *p, HvPaceCopy *out);
/* The frames between two copies, over seconds, gaps judged against target_ms (0: late, stalled and dropped_per_s are
   0). A gap belongs to the stretch its ending frame is in. max_ms is the longest gap's bucket, or for a gap of
   1000 ms or more its full length when it is the longest ever seen (else 1000). */
void hv_pace_summary(const HvPaceCopy *before, const HvPaceCopy *after, double seconds, double target_ms, HvPaceSummary *s);

/* A picture from the Mac replaced before it was taken to the screen (pvg_metal.m's presenter). */
void hv_frame_replaced(void);
/* A present Core Animation has finished with: on the glass at shown_ns (CLOCK_UPTIME_RAW; 0: never shown), carrying
   the Mac's picture swapped at swap_ns (0: none new, the pointer moved, say). Notes hv_pace_shown. Any thread. */
void hv_frame_presented(uint64_t shown_ns, uint64_t swap_ns);
/* A present's hold, in ns: how long the Mac's GPU work queued after it waited for Henge to finish encoding it. And one
   of the Mac's GPU batches' wait, in ns, from its commit to being scheduled. Any thread. */
void hv_frame_held(uint64_t ns);
void hv_frame_guest_waited(uint64_t ns);
void hv_frame_flow_copy(HvFrameFlow *f, HvFrameFlowCopy *out);
void hv_frame_flow_summary(const HvFrameFlowCopy *before, const HvFrameFlowCopy *after, HvFrameFlowSummary *s);

/* Whether the display notes any of this (pvg_metal.m): only while a benchmark runs (host/bench.c), so drawing costs
   nothing more the rest of the time. Turned on, the gaps start afresh: the time it was off is no gap. */
extern _Atomic int hv_frame_watch;
static inline int hv_frame_watching(void) { return atomic_load_explicit(&hv_frame_watch, memory_order_relaxed); }
void hv_frame_watch_set(int on);

/* Dynamic thermal state tracking (0: nominal, 1: fair/warm, 2: serious/hot, 3: critical).
   Enables frame pacing adaptation and refresh clamping to prevent thermal throttling. */
void hv_thermal_set_state(int state);
int hv_thermal_get_state(void);

#endif
