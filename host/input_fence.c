/* The person's input held behind a press until the helper's glide has stopped (input_fence.h). */
#include "input_fence.h"
#include "share.h"

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

#define HELD_MAX 512 /* a hold lasts one helper round trip: moves at 120 Hz and keys fit many times over */

/* Recursive: delivering an event can fail a link send in share.c, which tells the listener (here) at once. */
static pthread_mutex_t mu;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static HvFenceEvent held[HELD_MAX];
static int held_n;
static unsigned hold; /* the stop held events wait for; 0 when nothing is held */
static unsigned hold_serial; /* counts holds: a hold's time limit is for it only (the same stop may be waited for again) */
static uint64_t hold_since_ns;
static unsigned hold_stops; /* the Mac's stop count as the hold began (hv_fence_set_stops) */
static unsigned (*_Atomic mac_stops)(void);
static _Atomic unsigned limit_ms = HV_FENCE_LIMIT_MS;
static unsigned holds, holds_long; /* for the log: how often, and how often it took long */

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

/* Sends held events in order from the first, until one is a press that must wait again (a fling sent since: it holds
   what remains). Under mu. */
static void go_on_l(void) {
    int i = 0;
    while (i < held_n) {
        HvFenceEvent *e = &held[i];
        if (e->press && (hold = hv_share_glide_fence()) != 0) break;
        i++;
        e->deliver(e);
    }
    for (int j = i; j < held_n; j++) held[j - i] = held[j];
    held_n -= i;
}

static void timed_out(void *ctx);

static unsigned stops_now(void) {
    unsigned (*stops)(void) = atomic_load(&mac_stops);
    return stops ? stops() : 0;
}

static void start_hold_l(void) {
    hold_since_ns = now_ns();
    hold_stops = stops_now();
    holds++;
    hold_serial++;
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, (int64_t)atomic_load(&limit_ms) * 1000000),
                     dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), (void *)(uintptr_t)hold_serial, timed_out);
}

/* The hold ends: what it held goes on. */
static void ended_l(const char *how) {
    double ms = (double)(now_ns() - hold_since_ns) / 1e6;
    if (ms >= 100 || holds <= 3) {
        if (ms >= 100) holds_long++;
        fprintf(stderr, "input: a click waited %.0f ms for the helper's glide to stop (%s; %d events held; %u holds, %u long)\n",
                ms, how, held_n, holds, holds_long);
    }
    hold = 0;
    go_on_l();
    if (hold) start_hold_l();
}

static void timed_out(void *ctx) {
    unsigned serial = (unsigned)(uintptr_t)ctx;
    pthread_mutex_lock(&mu);
    if (hold && serial == hold_serial) {
        if (!(hold_stops & 1) && stops_now() == hold_stops) {
            hv_share_touch_late(hold); /* not waited on again until it answers */
            ended_l("no answer in time: it isn't waited on again until it answers");
        } else {
            /* The Mac stood still meanwhile (stopped for an auto-save): the helper couldn't answer. The press waits
               again, for a whole limit from now (a pause sends what is held first: hv_fence_flush). */
            ended_l("no answer yet, the Mac having stood still: waiting on");
        }
    }
    pthread_mutex_unlock(&mu);
}

/* share.c: every stop up to `id` is over (answered, or its link went). One before the stop held for doesn't end it. */
static void answered(unsigned id, int gone) {
    pthread_mutex_lock(&mu);
    if (hold && (int)(id - hold) >= 0) ended_l(gone ? "the helper's link went" : "it answered");
    pthread_mutex_unlock(&mu);
}

static void setup(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mu, &a);
    pthread_mutexattr_destroy(&a);
    hv_share_set_glide_listener(answered);
}

void hv_fence_send(const HvFenceEvent *e) {
    pthread_once(&once, setup);
    pthread_mutex_lock(&mu);
    if (!hold && !held_n && !(e->press && (hold = hv_share_glide_fence()) != 0)) {
        e->deliver(e);
    } else {
        if (held_n == HELD_MAX) { /* never in a round trip's time: rather out of order than lost */
            fprintf(stderr, "input: %d events held: sent without waiting for the helper\n", held_n);
            int n = held_n;
            hold = 0, held_n = 0; /* first: a delivery can end the hold (the link failing), which mustn't send them again */
            for (int i = 0; i < n; i++) held[i].deliver(&held[i]);
        }
        held[held_n++] = *e;
        if (!hold) {
            go_on_l(); /* nothing held before it: it goes, unless it is a press that must wait */
            if (hold) start_hold_l();
        } else if (held_n == 1) {
            start_hold_l(); /* the press that holds */
        }
    }
    pthread_mutex_unlock(&mu);
}

void hv_fence_flush(const char *why) {
    pthread_once(&once, setup);
    pthread_mutex_lock(&mu);
    if (held_n) fprintf(stderr, "input: %d events held for the helper sent now: %s\n", held_n, why);
    hold = 0;
    for (int i = 0; i < held_n; i++) held[i].deliver(&held[i]);
    held_n = 0;
    pthread_mutex_unlock(&mu);
}

void hv_fence_set_stops(unsigned (*stops)(void)) { atomic_store(&mac_stops, stops); }
void hv_fence_set_limit_ms(unsigned ms) { atomic_store(&limit_ms, ms); }

int hv_fence_held(void) {
    pthread_once(&once, setup);
    pthread_mutex_lock(&mu);
    int n = held_n;
    pthread_mutex_unlock(&mu);
    return n;
}
