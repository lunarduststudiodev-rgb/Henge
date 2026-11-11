/* The freeze recorder's thinking part, with no threads or Mach calls in it (tests/test_stallmon.c runs it on made-up
   timelines): when the Mac's screen stalled, judged from its frame count and the person's input, and a summary of what
   each thread was doing meanwhile. host/stallmon.c feeds it from the running Mac and writes what it finds. */
#ifndef HV_STALL_DETECT_H
#define HV_STALL_DETECT_H

#include <stdint.h>
#include <stdio.h>

/* What went wrong. */
enum {
    HV_STALL_NONE,
    HV_STALL_HITCH,    /* frames were coming steadily (a scroll, an animation, typing) and stopped for a while */
    HV_STALL_RESPONSE, /* the person pressed something and the screen took a long time to change */
    HV_STALL_HANG,     /* still waiting after HV_STALL_HANG_US: told now, in case it never ends */
    HV_STALL_APP,      /* Henge's own main thread (its touch and keyboard handling) was stuck */
    HV_STALL_APP_HANG, /* ... and still is after HV_STALL_HANG_US */
    HV_STALL_STARVED,  /* the recorder itself got no time while the Mac ran and Henge's threads did (no samples in it) */
};
const char *hv_stall_kind_name(int kind);

#define HV_STALL_TICK_US 10000       /* the monitor (host/stallmon.c) looks this often */
#define HV_STALL_HITCH_US 120000     /* frames were flowing and stopped this long */
#define HV_STALL_FLOW_FRAMES 4       /* flowing: this many frames ... */
#define HV_STALL_FLOW_US 250000      /* ... within this long before they stopped */
#define HV_STALL_QUIET_US 1000000    /* a stop longer than this with no input during it is the screen just being still */
#define HV_STALL_RESPONSE_US 300000  /* a press waited this long for the screen */
#define HV_STALL_HANG_US 1500000
#define HV_STALL_MAX_US 60000000     /* a press answered later than this was not what changed the screen */
#define HV_STALL_APP_US 250000       /* the main thread took this long to run a tiny task */

typedef struct HvStallEvent {
    int kind;
    uint64_t start_us, end_us; /* the stretch it covers: from the last frame or the press, to the frame that ended it */
    int frames_before;         /* frames in the HV_STALL_FLOW_US before it began */
    int ended_hang;            /* this stretch already went out as a HANG while it lasted */
} HvStallEvent;

#define HV_STALL_FT 16
typedef struct HvStallDetector {
    uint64_t frames;          /* the count last seen */
    uint64_t last_frame_us;   /* when it last changed; 0 before the first look */
    uint64_t ft[HV_STALL_FT]; /* when the latest frames came, newest at (nft - 1) % HV_STALL_FT */
    unsigned nft;
    uint64_t first_press_us;  /* the first press seen since the last frame, 0 if none */
    int hang_reported;
} HvStallDetector;

void hv_stall_reset(HvStallDetector *d);
/* One look, about every 10 ms: now, the frame count, and when the person last gave any input (keys, pointer, scroll,
   touch) and last pressed something (a key, a click, a finger landing), 0 for never. 1 with *ev filled when there is
   something to record. */
int hv_stall_step(HvStallDetector *d, uint64_t now_us, uint64_t frames, uint64_t input_us, uint64_t press_us,
                  HvStallEvent *ev);

/* Whether a stall could be forming, from the detector as its last step left it: the same signals hv_stall_step judges
   by, seen early. Frames were flowing and none came for HV_STALL_WATCH_US, or for HV_STALL_WATCH_LATE_US (one of the
   monitor's ticks) longer than the pace they came at, whichever is longer (a game at 20 fps waits 50 ms for every
   frame), and the stop is not yet the screen just being still. The pace is the longest wait between the flowing
   frames, leaving out waits of HV_STALL_HITCH_US or more; and the start is at most HV_STALL_WATCH_MAX_US, so a stall
   has samples before it is one. The cap matters only where a wait between flowing frames is 80 ms or more as the
   ticks see it: a game at about 12 to 14 fps (slower, fewer than HV_STALL_FLOW_FRAMES come in HV_STALL_FLOW_US, so
   it is not flowing), or one uneven frame; each such wait is watched from 80 ms in, within the budget. Or a press
   after the last frame is unanswered for HV_STALL_WATCH_US, or the main thread's timed task has been waiting that
   long (ping_wait_us, 0 if none is outstanding). frames is the count the step after it will be
   given: a frame that came in since the last step closes the gap (asked with the last step's count, a steady 20 fps
   would look 50 ms late on every frame). host/stallmon.c stops threads to sample them only while this says so:
   samples leave only in the records of stalls, and nothing else needs a thread stopped. */
#define HV_STALL_WATCH_US 50000
#define HV_STALL_WATCH_LATE_US HV_STALL_TICK_US
#define HV_STALL_WATCH_MAX_US 80000 /* four ticks before a gap is a hitch (HV_STALL_HITCH_US) */
/* The watch must start early enough that a gap which becomes a hitch has been sampled for at least three of the
   monitor's ticks by the time it is one. Otherwise its record has too few samples to name a cause; and the timeline
   tests follow the cap, so a cap up to the frame pace they use (110 ms) would still pass them. */
_Static_assert(HV_STALL_WATCH_MAX_US + 3 * HV_STALL_TICK_US <= HV_STALL_HITCH_US,
               "the freeze watch's cap leaves under three samples before a gap is a hitch");
int hv_stall_watch(const HvStallDetector *d, uint64_t now_us, uint64_t frames, uint64_t input_us, uint64_t ping_wait_us);

/* How often it may: one credit a tick, HV_STALL_BUDGET_RATE a sampled tick, banked up to HV_STALL_BUDGET_TICKS
   sampled ticks. A long stall is sampled every tick for that many, then every HV_STALL_BUDGET_RATE-th tick. */
#define HV_STALL_BUDGET_RATE 4
#define HV_STALL_BUDGET_TICKS 500
typedef struct HvStallBudget {
    uint32_t credit;
} HvStallBudget;
void hv_stall_budget_fill(HvStallBudget *b); /* banked in full */
/* One tick: 1 if it samples (watching, and the credit is there). */
int hv_stall_budget_take(HvStallBudget *b, int watching);

/* What a thread was doing at one sample. */
enum {
    HV_SAMPLE_GUEST_APP,    /* running macOS's apps (EL0) in translated code */
    HV_SAMPLE_GUEST_KERNEL, /* running macOS's kernel (EL1) in translated code */
    HV_SAMPLE_ENGINE,       /* running Henge's own code for the Mac: translating, the MMU's slow path, devices */
    HV_SAMPLE_WAITING,      /* blocked in a call to the kernel: a lock, the disk, the GPU, another CPU */
    HV_SAMPLE_IDLE,         /* a guest CPU with nothing to do (WFI, off, paused) */
    HV_SAMPLE_GUEST_SPIN,   /* a guest CPU waiting in place for a word another one holds (WFE: macOS's spinlocks) */
    HV_SAMPLE_FAULT,        /* in the kernel uninterruptibly: a page fault (compressed or paged-out memory) or the like */
    HV_SAMPLE_KINDS
};
#define HV_STALL_BT 12
typedef struct HvStallSample {
    uint64_t t_us;
    uint64_t pc;              /* guest code: the block's guest VA; otherwise the host PC */
    uint64_t bt[HV_STALL_BT]; /* host return addresses, innermost first */
    uint8_t thread, what, nbt;
} HvStallSample;

/* Gives an address a name for the record ("app+0x1a2b3 name", "libsystem_kernel.dylib:__psynch_mutexwait"). */
typedef void (*HvStallLabel)(uint64_t addr, char *buf, size_t len);

#define HV_STALL_COUNTERS 12
extern const char *const hv_stall_counter_names[HV_STALL_COUNTERS];
/* The graphics stages' ages (devices/pvg_lite.h HV_PVG_STAGE_*): the most each was held up, in ms, at the counter
   snapshots over the stretch. The guest CPUs can be idle while macOS waits for its GPU work: these say where it was. */
#define HV_STALL_GFX 5
extern const char *const hv_stall_gfx_names[HV_STALL_GFX];
/* How much of [start_us, end_us] a stage held up age_ms at t_us covered: a hold from before the stretch counts only
   from its start, and one seen after its end only up to it. ms. */
uint32_t hv_stall_gfx_overlap(uint64_t t_us, uint32_t age_ms, uint64_t start_us, uint64_t end_us);
/* One counter snapshot's graphics stages: when it was taken, and each stage's age then. */
typedef struct HvStallGfxSnap {
    uint64_t t_us;
    uint32_t ms[HV_STALL_GFX];
} HvStallGfxSnap;
/* The most of [start_us, end_us] each stage was seen held up over (hv_stall_gfx_overlap), from the snapshots among
   s[0..n) (any order) taken in it or up to 50 ms after its end (a hold still going on then reaches back into it). */
void hv_stall_gfx_held(const HvStallGfxSnap *s, unsigned n, uint64_t start_us, uint64_t end_us, uint32_t out[HV_STALL_GFX]);
typedef struct HvStallRecordInfo {
    const HvStallEvent *ev;
    const char *when;            /* wall-clock time, ISO 8601 */
    double uptime_s;
    int thermal;                 /* the device's thermal pressure, 0 nominal .. 4; -1 unknown */
    int64_t avail_mb;            /* memory iOS would still give Henge, -1 unknown */
    const char *image;           /* the app image's UUID and load address, for symbolizing on a Mac */
    uint64_t counters[HV_STALL_COUNTERS]; /* how much the engine counters grew over the stretch */
    uint64_t covered_us;         /* how much of the stretch the samples reach back over */
    uint32_t gfx_ms[HV_STALL_GFX]; /* the most of the stretch each stage was seen held up over (hv_stall_gfx_overlap) */
} HvStallRecordInfo;

/* Writes one JSON line for ev from the samples taken in it (any order), and a sentence for the log into line. Returns
   the verdict ("guest", "guest_spin", "engine", "waiting", "fault", "app", "starved", "graphics": the CPUs idle while
   a graphics stage was held up for at least half the stretch, "idle"). names[i] is thread i's name. */
const char *hv_stall_write(FILE *f, const HvStallRecordInfo *info, const HvStallSample *s, int n, const char *const *names,
                           int nthreads, HvStallLabel label, char *line, size_t line_len);

#endif
