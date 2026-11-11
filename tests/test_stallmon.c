/* The freeze recorder's judgement (host/stall_detect.c), on made-up timelines looked at every 10 ms as the recorder
   does: steady frames that stop (a hitch), a press the screen answers late (a response), one it never answers in time
   (a hang, told while it lasts), and what must not count: short gaps, a screen simply standing still, a press answered
   quickly. Then the record: its verdict, its JSON and its log sentence from made-up samples. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../devices/pvg_lite.h" /* HV_PVG_STAGE_* */
#include "../host/stall_detect.h"

static int failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fprintf(stderr, "\n");                                                                                     \
        }                                                                                                              \
    } while (0)

#define MS 1000ull
#define MAX_EVENTS 16

/* A timeline: when frames came, when the person gave input and pressed. */
typedef struct {
    uint64_t frames[4096];
    int nframes;
    uint64_t inputs[64], presses[64];
    int ninputs, npresses;
} Timeline;

static void frames_every(Timeline *tl, uint64_t from, uint64_t to, uint64_t every) {
    for (uint64_t t = from; t <= to && tl->nframes < 4096; t += every) tl->frames[tl->nframes++] = t;
}
static void frame_at(Timeline *tl, uint64_t t) { tl->frames[tl->nframes++] = t; }
static void press_at(Timeline *tl, uint64_t t) { tl->presses[tl->npresses++] = t, tl->inputs[tl->ninputs++] = t; }
static void input_at(Timeline *tl, uint64_t t) { tl->inputs[tl->ninputs++] = t; }

static uint64_t latest(const uint64_t *v, int n, uint64_t now) {
    uint64_t best = 0;
    for (int i = 0; i < n; i++)
        if (v[i] <= now && v[i] > best) best = v[i];
    return best;
}

/* Runs the detector over the timeline from 1 ms to end; returns how many events, in ev. */
static int run(const Timeline *tl, uint64_t end, HvStallEvent *ev) {
    HvStallDetector d;
    hv_stall_reset(&d);
    int n = 0;
    for (uint64_t now = 1 * MS; now <= end; now += 10 * MS) {
        uint64_t frames = 0;
        for (int i = 0; i < tl->nframes; i++) frames += tl->frames[i] <= now;
        HvStallEvent e;
        if (hv_stall_step(&d, now, frames, latest(tl->inputs, tl->ninputs, now), latest(tl->presses, tl->npresses, now), &e) &&
            n < MAX_EVENTS)
            ev[n++] = e;
    }
    return n;
}

static int near(uint64_t got, uint64_t want) { return got + 20 * MS >= want && got <= want + 20 * MS; }

static void detection(void) {
    HvStallEvent ev[MAX_EVENTS];
    static Timeline tl;

    /* 60 fps, a 200 ms stop, 60 fps again: one hitch from the last frame to the next. */
    memset(&tl, 0, sizeof tl);
    frames_every(&tl, 100 * MS, 1000 * MS, 16 * MS);
    frames_every(&tl, 1200 * MS, 2000 * MS, 16 * MS);
    int n = run(&tl, 2500 * MS, ev);
    CHECK(n == 1 && ev[0].kind == HV_STALL_HITCH, "hitch: %d events, kind %d", n, n ? ev[0].kind : -1);
    CHECK(n == 1 && near(ev[0].end_us - ev[0].start_us, 200 * MS), "hitch length %llu",
          n ? (unsigned long long)(ev[0].end_us - ev[0].start_us) : 0ull);
    CHECK(n == 1 && ev[0].frames_before >= HV_STALL_FLOW_FRAMES, "frames before %d", n ? ev[0].frames_before : -1);

    /* An 80 ms stop is not one. */
    memset(&tl, 0, sizeof tl);
    frames_every(&tl, 100 * MS, 1000 * MS, 16 * MS);
    frames_every(&tl, 1080 * MS, 2000 * MS, 16 * MS);
    n = run(&tl, 2500 * MS, ev);
    CHECK(n == 0, "short gap: %d events", n);

    /* An animation that ends and another 3 s later, nobody touching anything: the screen was just still. */
    memset(&tl, 0, sizeof tl);
    frames_every(&tl, 100 * MS, 600 * MS, 16 * MS);
    frames_every(&tl, 3600 * MS, 4000 * MS, 16 * MS);
    n = run(&tl, 4500 * MS, ev);
    CHECK(n == 0, "quiet screen: %d events", n);

    /* The same with the pointer moving through the gap (a scroll that froze): a hitch. */
    input_at(&tl, 1500 * MS);
    n = run(&tl, 4500 * MS, ev);
    CHECK(n == 1 && ev[0].kind == HV_STALL_HITCH && near(ev[0].end_us - ev[0].start_us, 3000 * MS),
          "scroll freeze: %d events", n);

    /* A still screen, a press, and the screen changing 500 ms later: a slow response from the press. */
    memset(&tl, 0, sizeof tl);
    frame_at(&tl, 100 * MS);
    press_at(&tl, 3000 * MS);
    frame_at(&tl, 3500 * MS);
    n = run(&tl, 4000 * MS, ev);
    CHECK(n == 1 && ev[0].kind == HV_STALL_RESPONSE, "response: %d events", n);
    CHECK(n == 1 && near(ev[0].start_us, 3000 * MS) && near(ev[0].end_us, 3500 * MS), "response stretch");

    /* Answered in 100 ms: fine. */
    memset(&tl, 0, sizeof tl);
    frame_at(&tl, 100 * MS);
    press_at(&tl, 3000 * MS);
    frame_at(&tl, 3100 * MS);
    n = run(&tl, 4000 * MS, ev);
    CHECK(n == 0, "quick response: %d events", n);

    /* Pressed again and again while it was stuck: it counts from the first press. */
    memset(&tl, 0, sizeof tl);
    frame_at(&tl, 100 * MS);
    press_at(&tl, 1000 * MS);
    press_at(&tl, 1300 * MS);
    press_at(&tl, 1600 * MS);
    frame_at(&tl, 1800 * MS);
    n = run(&tl, 2500 * MS, ev);
    CHECK(n == 1 && ev[0].kind == HV_STALL_RESPONSE && near(ev[0].start_us, 1000 * MS), "repeated presses");

    /* No answer for 3 s: a hang told at 1.5 s, then the response when the screen changes, marked as that hang. */
    memset(&tl, 0, sizeof tl);
    frame_at(&tl, 100 * MS);
    press_at(&tl, 1000 * MS);
    frame_at(&tl, 4000 * MS);
    n = run(&tl, 5000 * MS, ev);
    CHECK(n == 2, "hang: %d events", n);
    CHECK(n >= 1 && ev[0].kind == HV_STALL_HANG && near(ev[0].end_us, 2500 * MS), "hang told while it lasts");
    CHECK(n == 2 && ev[1].kind == HV_STALL_RESPONSE && ev[1].ended_hang && near(ev[1].end_us - ev[1].start_us, 3000 * MS),
          "hang's end");

    /* A press nothing answered, and the screen changing a minute and more later: the hang only. */
    memset(&tl, 0, sizeof tl);
    frame_at(&tl, 100 * MS);
    press_at(&tl, 1000 * MS);
    frame_at(&tl, 70000 * MS);
    n = run(&tl, 71000 * MS, ev);
    CHECK(n == 1 && ev[0].kind == HV_STALL_HANG, "unanswered press: %d events", n);
}

static void label(uint64_t addr, char *buf, size_t len) {
    if (addr >= 0x100000000ull && addr < 0x110000000ull)
        snprintf(buf, len, "app+0x%llx", (unsigned long long)(addr - 0x100000000ull));
    else snprintf(buf, len, "libsystem_kernel.dylib:__psynch_mutexwait+0x8");
}

/* The JSON is one line, its brackets balanced outside strings. */
static int well_formed(const char *s) {
    int depth = 0, in_str = 0;
    for (const char *p = s; *p; p++) {
        if (in_str) {
            if (*p == '\\') p++;
            else if (*p == '"') in_str = 0;
            continue;
        }
        if (*p == '"') in_str = 1;
        else if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') depth--;
        else if (*p == '\n' && p[1]) return 0;
        if (depth < 0) return 0;
    }
    return depth == 0 && !in_str;
}

static uint32_t gfx_ms[HV_STALL_GFX]; /* the graphics stages' ages the next records get */

static char *write_record(const HvStallEvent *ev, const HvStallSample *s, int n, const char *const *names, int nt,
                          char *line, size_t line_len, const char **verdict) {
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    HvStallRecordInfo info = {.ev = ev, .when = "2026-11-08T12:00:00Z", .thermal = 1, .avail_mb = 2048,
                              .image = "UUID@0x100000000", .covered_us = ev->end_us - ev->start_us};
    info.counters[0] = 42;
    memcpy(info.gfx_ms, gfx_ms, sizeof gfx_ms);
    *verdict = hv_stall_write(f, &info, s, n, names, nt, label, line, line_len);
    fclose(f);
    return buf;
}

static void overlap(void) {
    printf("graphics stages over a stretch\n");
    uint64_t s = 1000 * MS, e = 1600 * MS;
    CHECK(hv_stall_gfx_overlap(1300 * MS, 200, s, e) == 200, "held up within it: all of it");
    CHECK(hv_stall_gfx_overlap(1300 * MS, 90000, s, e) == 300, "since long before: only from the start (%u)", hv_stall_gfx_overlap(1300 * MS, 90000, s, e));
    CHECK(hv_stall_gfx_overlap(1640 * MS, 700, s, e) == 600, "seen after its end, since before its start: the whole stretch");
    CHECK(hv_stall_gfx_overlap(1640 * MS, 30, s, e) == 0, "begun after its end: none");
    CHECK(hv_stall_gfx_overlap(1300 * MS, 0, s, e) == 0 && hv_stall_gfx_overlap(1300 * MS, 50, e, s) == 0, "not held up, or no stretch: none");
}

static void records(void) {
    const char *names[] = {"cpu0", "cpu1", "main"};
    static HvStallSample s[300];
    int n = 0;
    HvStallEvent ev = {.kind = HV_STALL_RESPONSE, .start_us = 1000 * MS, .end_us = 1600 * MS, .frames_before = 3};
    /* cpu0 in the guest's kernel; cpu1 waiting on a lock in the app; main idle in its run loop; and samples outside
       the stretch, which must not count. */
    for (int i = 0; i < 60; i++) {
        uint64_t t = 1000 * MS + (uint64_t)i * 10 * MS;
        s[n++] = (HvStallSample){.t_us = t, .thread = 0, .what = HV_SAMPLE_GUEST_KERNEL, .pc = 0xfffffe0007001000ull};
        s[n] = (HvStallSample){.t_us = t, .thread = 1, .what = HV_SAMPLE_WAITING, .pc = 0x1f0000000ull, .nbt = 2};
        s[n].bt[0] = 0x100012340ull, s[n].bt[1] = 0x100045670ull;
        n++;
        s[n++] = (HvStallSample){.t_us = t, .thread = 2, .what = HV_SAMPLE_WAITING, .pc = 0x1f0000000ull};
    }
    for (int i = 0; i < 20; i++)
        s[n++] = (HvStallSample){.t_us = 3000 * MS + (uint64_t)i * MS, .thread = 0, .what = HV_SAMPLE_ENGINE, .pc = 0x100099000ull};
    char line[512];
    const char *verdict;
    char *json = write_record(&ev, s, n, names, 3, line, sizeof line, &verdict);
    CHECK(well_formed(json), "not well formed: %s", json);
    CHECK(!strcmp(verdict, "guest") || !strcmp(verdict, "waiting"), "verdict %s", verdict);
    CHECK(strstr(json, "\"kind\":\"response\"") && strstr(json, "\"ms\":600"), "kind and length: %s", json);
    CHECK(strstr(json, "\"name\":\"cpu0\",\"samples\":60,\"guest_app\":0,\"guest_kernel\":60,\"engine\":0"),
          "cpu0 counts (the engine samples after the stretch left out): %s", json);
    CHECK(strstr(json, "\"at\":[\"libsystem_kernel.dylib:__psynch_mutexwait+0x8\",\"app+0x12340\",\"app+0x45670\"]"),
          "cpu1's stack: %s", json);
    CHECK(strstr(json, "\"translations\":42"), "counters: %s", json);
    CHECK(strstr(line, "freeze: response 600 ms") && strstr(line, "cpu1 waiting 100% at app+0x12340"), "line: %s", line);
    CHECK(!strstr(line, "main"), "the main thread waiting in its run loop is idle, not in the line: %s", line);
    free(json);

    /* More waiting than guest work across the CPUs: the verdict says so. */
    for (int i = 0; i < n; i++)
        if (s[i].thread == 0 && s[i].t_us < 2000 * MS) s[i].what = HV_SAMPLE_WAITING, s[i].nbt = 0;
    json = write_record(&ev, s, n, names, 3, line, sizeof line, &verdict);
    CHECK(!strcmp(verdict, "waiting"), "waiting verdict: %s", verdict);
    free(json);

    /* A guest CPU waiting in place on another's spinlock is the guest's doing, not the engine's or a lock of Henge's;
       one held in page faults is memory, not a lock: each has its own verdict. */
    for (int i = 0; i < n; i++)
        if (s[i].t_us < 2000 * MS && s[i].thread < 2) s[i].what = s[i].thread == 0 ? HV_SAMPLE_GUEST_SPIN : HV_SAMPLE_IDLE;
    json = write_record(&ev, s, n, names, 3, line, sizeof line, &verdict);
    CHECK(!strcmp(verdict, "guest_spin"), "guest spin verdict: %s", verdict);
    CHECK(strstr(json, "\"guest_spin\":60,\"fault\":0"), "cpu0's spin samples: %s", json);
    CHECK(strstr(line, "cpu0 guest_spin 100%"), "line: %s", line);
    free(json);
    for (int i = 0; i < n; i++)
        if (s[i].t_us < 2000 * MS && s[i].thread == 0) s[i].what = HV_SAMPLE_FAULT;
    json = write_record(&ev, s, n, names, 3, line, sizeof line, &verdict);
    CHECK(!strcmp(verdict, "fault"), "fault verdict: %s", verdict);
    CHECK(strstr(json, "\"guest_spin\":0,\"fault\":60"), "cpu0's fault samples: %s", json);
    free(json);

    /* Every CPU asleep: nothing in the Mac was busy. */
    for (int i = 0; i < n; i++)
        if (s[i].thread < 2) s[i].what = HV_SAMPLE_IDLE;
    json = write_record(&ev, s, n, names, 3, line, sizeof line, &verdict);
    CHECK(!strcmp(verdict, "idle"), "idle verdict: %s", verdict);
    CHECK(!strstr(line, "cpu0"), "idle CPUs stay out of the line: %s", line);
    CHECK(strstr(json, "\"gfx\":{\"queued_ms\":0,\"worker_ms\":0,\"gpu_ms\":0,\"barrier_ms\":0,\"drawable_ms\":0}") && !strstr(line, "gfx"),
          "no graphics stage held up: %s / %s", json, line);
    free(json);

    /* ... while the GPU had work and finished none for most of the stretch: the graphics side held it up. */
    gfx_ms[HV_PVG_STAGE_GPU] = 450, gfx_ms[HV_PVG_STAGE_QUEUED] = 120;
    json = write_record(&ev, s, n, names, 3, line, sizeof line, &verdict);
    CHECK(!strcmp(verdict, "graphics") && well_formed(json), "graphics verdict: %s", verdict);
    CHECK(strstr(json, "\"gfx\":{\"queued_ms\":120,\"worker_ms\":0,\"gpu_ms\":450,\"barrier_ms\":0,\"drawable_ms\":0}"), "gfx: %s", json);
    CHECK(strstr(line, ", graphics; gfx queued 120 ms; gfx gpu 450 ms"), "line: %s", line);
    free(json);
    /* The presenter without a drawable for most of it: the same, named. */
    gfx_ms[HV_PVG_STAGE_GPU] = gfx_ms[HV_PVG_STAGE_QUEUED] = 0, gfx_ms[HV_PVG_STAGE_DRAWABLE] = 590;
    json = write_record(&ev, s, n, names, 3, line, sizeof line, &verdict);
    CHECK(!strcmp(verdict, "graphics") && strstr(json, "\"drawable_ms\":590") && strstr(line, "; gfx drawable 590 ms"),
          "drawable wait: %s / %s", verdict, line);
    free(json);
    gfx_ms[HV_PVG_STAGE_DRAWABLE] = 0;
    /* From the snapshots, as the recorder takes them: a GPU wait that began 5 s before the stretch and ended 100 ms into
       it was seen at 1100 ms as 5100 ms old, and after it as none. It held up only the stretch's first 100 ms of 600. */
    HvStallGfxSnap snaps[] = {{900 * MS, {[HV_PVG_STAGE_GPU] = 4900}}, {1050 * MS, {[HV_PVG_STAGE_GPU] = 5050}},
                              {1100 * MS, {[HV_PVG_STAGE_GPU] = 5100}}, {1150 * MS, {0}}, {1550 * MS, {0}}};
    hv_stall_gfx_held(snaps, 5, ev.start_us, ev.end_us, gfx_ms);
    CHECK(gfx_ms[HV_PVG_STAGE_GPU] == 100, "only its part in the stretch: %u ms", gfx_ms[HV_PVG_STAGE_GPU]);
    json = write_record(&ev, s, n, names, 3, line, sizeof line, &verdict);
    CHECK(!strcmp(verdict, "idle") && strstr(json, "\"gpu_ms\":100"), "a long wait that ended early on stays idle: %s", verdict);
    free(json);
    /* Seen at 1640 ms, past the end, as 700 ms old: it covered the whole stretch. One at 1700 ms is too late to count. */
    HvStallGfxSnap after[] = {{1640 * MS, {[HV_PVG_STAGE_DRAWABLE] = 700}}, {1700 * MS, {[HV_PVG_STAGE_QUEUED] = 900}}};
    hv_stall_gfx_held(after, 2, ev.start_us, ev.end_us, gfx_ms);
    CHECK(gfx_ms[HV_PVG_STAGE_DRAWABLE] == 600 && !gfx_ms[HV_PVG_STAGE_QUEUED], "a hold seen just after it: %u %u",
          gfx_ms[HV_PVG_STAGE_DRAWABLE], gfx_ms[HV_PVG_STAGE_QUEUED]);
    json = write_record(&ev, s, n, names, 3, line, sizeof line, &verdict);
    CHECK(!strcmp(verdict, "graphics"), "and that is graphics: %s", verdict);
    free(json);
    memset(gfx_ms, 0, sizeof gfx_ms);
    /* Held up for less than half the stretch is not what stalled it. */
    gfx_ms[HV_PVG_STAGE_GPU] = 250;
    json = write_record(&ev, s, n, names, 3, line, sizeof line, &verdict);
    CHECK(!strcmp(verdict, "idle"), "a short graphics wait stays idle: %s", verdict);
    free(json);
    /* Busy CPUs keep their verdict, whatever the graphics stages say. */
    gfx_ms[HV_PVG_STAGE_GPU] = 600;
    for (int i = 0; i < n; i++)
        if (s[i].thread == 0) s[i].what = HV_SAMPLE_GUEST_APP;
    json = write_record(&ev, s, n, names, 3, line, sizeof line, &verdict);
    CHECK(!strcmp(verdict, "guest"), "guest verdict with the GPU held up: %s", verdict);
    free(json);
    for (int i = 0; i < n; i++)
        if (s[i].thread == 0) s[i].what = HV_SAMPLE_IDLE;
    memset(gfx_ms, 0, sizeof gfx_ms);

    /* The main thread's own stall is the app's, whatever the CPUs did. */
    ev.kind = HV_STALL_APP;
    json = write_record(&ev, s, n, names, 3, line, sizeof line, &verdict);
    CHECK(!strcmp(verdict, "app") && strstr(json, "\"kind\":\"app\""), "app verdict: %s", verdict);
    free(json);

    /* A name with a quote in it stays valid JSON; no samples at all is still a record. */
    const char *odd[] = {"cpu\"0"};
    json = write_record(&ev, s, 0, odd, 1, line, sizeof line, &verdict);
    CHECK(well_formed(json), "empty record: %s", json);
    free(json);
}

/* When the recorder would stop threads to sample them (hv_stall_watch, asked before each step as host/stallmon.c
   does): how many ticks after `after` (stepping from 1 ms) to end, and the first and last; ping_from..ping_to the main
   thread's task waits. */
typedef struct {
    int ticks;
    uint64_t first, last;
} Watched;

static Watched watched_after(const Timeline *tl, uint64_t after, uint64_t end, uint64_t ping_from, uint64_t ping_to) {
    HvStallDetector d;
    hv_stall_reset(&d);
    Watched w = {0};
    for (uint64_t now = 1 * MS; now <= end; now += 10 * MS) {
        uint64_t frames = 0;
        for (int i = 0; i < tl->nframes; i++) frames += tl->frames[i] <= now;
        uint64_t input = latest(tl->inputs, tl->ninputs, now), press = latest(tl->presses, tl->npresses, now);
        uint64_t ping_wait = ping_from && now > ping_from && now < ping_to ? now - ping_from : 0;
        if (hv_stall_watch(&d, now, frames, input, ping_wait) && now > after) {
            if (!w.ticks++) w.first = now;
            w.last = now;
        }
        HvStallEvent e;
        hv_stall_step(&d, now, frames, input, press, &e);
    }
    return w;
}
static Watched watched(const Timeline *tl, uint64_t end, uint64_t ping_from, uint64_t ping_to) {
    return watched_after(tl, 0, end, ping_from, ping_to);
}

static void watching(void) {
    Timeline tl = {0};
    /* Steady frames, nothing waiting: never. A frame late by less than 50 ms: never. */
    frames_every(&tl, 0, 3000 * MS, 16 * MS);
    Watched w = watched(&tl, 3000 * MS, 0, 0);
    CHECK(w.ticks == 0, "steady frames: watched %d ticks from %llu ms", w.ticks, (unsigned long long)(w.first / MS));
    memset(&tl, 0, sizeof tl);
    frames_every(&tl, 0, 1000 * MS, 16 * MS);
    frames_every(&tl, 1040 * MS, 2000 * MS, 16 * MS);
    w = watched(&tl, 2000 * MS, 0, 0);
    CHECK(w.ticks == 0, "a 40 ms gap: watched %d ticks", w.ticks);
    /* A steady 20 fps (a frame every HV_STALL_WATCH_US), the frames a little early or late: never. The tick a frame
       comes in sees it (the watch is asked with the step's frame count), not a 50 ms gap. */
    for (uint64_t late = 0; late <= 4; late += 2) {
        memset(&tl, 0, sizeof tl);
        for (uint64_t t = 0; t <= 3000 * MS; t += HV_STALL_WATCH_US) frame_at(&tl, t + (t / HV_STALL_WATCH_US % 2 ? late * MS : 0));
        w = watched(&tl, 3000 * MS, 0, 0);
        CHECK(w.ticks == 0, "20 fps, every other frame %llu ms late: watched %d ticks from %llu ms", (unsigned long long)late,
              w.ticks, (unsigned long long)(w.first / MS));
    }

    /* 20 fps, then none for 400 ms: watched from a tick past the slowest of the frames (60 ms, as the ticks see
       them) to the frame that ends it. */
    memset(&tl, 0, sizeof tl);
    for (uint64_t t = 0; t <= 1000 * MS; t += HV_STALL_WATCH_US) frame_at(&tl, t + (t / HV_STALL_WATCH_US % 2 ? 4 * MS : 0));
    frames_every(&tl, 1400 * MS, 2000 * MS, HV_STALL_WATCH_US);
    w = watched(&tl, 2000 * MS, 0, 0);
    CHECK(w.first >= 1070 * MS && w.first <= 1085 * MS && w.last >= 1390 * MS && w.last < 1400 * MS,
          "20 fps, then a 400 ms stop: watched %llu..%llu ms", (unsigned long long)(w.first / MS), (unsigned long long)(w.last / MS));

    /* The pace is the flowing frames', not a stall's. A still screen, a short burst of frames, then a 500 ms freeze:
       watched from 50 ms in (the burst's first frame pairs with the last before the still screen: no pace). */
    memset(&tl, 0, sizeof tl);
    frames_every(&tl, 0, 1000 * MS, 16 * MS);
    frames_every(&tl, 6000 * MS, 6100 * MS, 16 * MS);
    frames_every(&tl, 6600 * MS, 7000 * MS, 16 * MS);
    w = watched_after(&tl, 6000 * MS, 7000 * MS, 0, 0);
    CHECK(w.first >= 6150 * MS && w.first <= 6160 * MS && w.last >= 6590 * MS && w.last < 6600 * MS,
          "a burst after a still screen, then a freeze: watched %d ticks, %llu..%llu ms", w.ticks,
          (unsigned long long)(w.first / MS), (unsigned long long)(w.last / MS));
    /* 60 fps, a 200 ms hitch, 100 ms of frames, then a 1 s freeze: the hitch is no pace, watched from 50 ms in */
    memset(&tl, 0, sizeof tl);
    frames_every(&tl, 0, 1000 * MS, 16 * MS);
    frames_every(&tl, 1200 * MS, 1300 * MS, 16 * MS);
    frames_every(&tl, 2300 * MS, 2500 * MS, 16 * MS);
    w = watched_after(&tl, 1300 * MS, 2500 * MS, 0, 0);
    CHECK(w.first >= 1350 * MS && w.first <= 1360 * MS, "a hitch, frames, then a freeze: watched from %llu ms",
          (unsigned long long)(w.first / MS));
    /* A 130 ms hitch, both its frames in the window before the freeze: a stall, not the pace (from 50 ms in) */
    memset(&tl, 0, sizeof tl);
    frames_every(&tl, 0, 1000 * MS, 16 * MS);
    frames_every(&tl, 1130 * MS, 1230 * MS, 16 * MS);
    frames_every(&tl, 1700 * MS, 2000 * MS, 16 * MS);
    w = watched_after(&tl, 1230 * MS, 2000 * MS, 0, 0);
    CHECK(w.first >= 1280 * MS && w.first <= 1290 * MS, "a 130 ms hitch, frames, then a freeze: watched from %llu ms",
          (unsigned long long)(w.first / MS));
    /* A 110 ms wait whose first frame is before the window, then 30 fps (the pace, 40 ms as the ticks see it), then a
       freeze: the wait is not counted (from 50 ms in) */
    memset(&tl, 0, sizeof tl);
    frames_every(&tl, 0, 1000 * MS, 16 * MS);
    frames_every(&tl, 1110 * MS, 1340 * MS, 33 * MS); /* the last at 1308, seen at 1311 */
    frames_every(&tl, 1750 * MS, 2000 * MS, 16 * MS);
    w = watched_after(&tl, 1311 * MS, 2000 * MS, 0, 0);
    CHECK(w.first >= 1360 * MS && w.first <= 1365 * MS, "a wait half out of the window, then a freeze: watched from %llu ms",
          (unsigned long long)(w.first / MS));
    /* 60 fps, one 100 ms frame well inside the window, 110 ms of frames, then a freeze: watched from
       HV_STALL_WATCH_MAX_US in, before the gap is a hitch, though the pace makes it 110 ms (from 50 ms in if the wait
       were not counted, from 110 ms without the cap) */
    memset(&tl, 0, sizeof tl);
    frames_every(&tl, 0, 1040 * MS, 16 * MS);
    frames_every(&tl, 1140 * MS, 1252 * MS, 16 * MS); /* the last seen at 1261 */
    frames_every(&tl, 1650 * MS, 2000 * MS, 16 * MS);
    w = watched_after(&tl, 1261 * MS, 2000 * MS, 0, 0);
    CHECK(w.first >= 1261 * MS + HV_STALL_WATCH_MAX_US - 10 * MS && w.first <= 1261 * MS + HV_STALL_WATCH_MAX_US,
          "a slow frame, frames, then a freeze: watched from %llu ms", (unsigned long long)(w.first / MS));

    /* Frames flowing, then none for 400 ms: watched from 50 ms in until the frame that ends it (the tick that sees
       it is not: the watch is asked with the frame count the step is given). */
    memset(&tl, 0, sizeof tl);
    frames_every(&tl, 0, 1000 * MS, 16 * MS);
    frames_every(&tl, 1400 * MS, 2000 * MS, 16 * MS);
    w = watched(&tl, 2000 * MS, 0, 0);
    CHECK(w.ticks >= 30 && w.ticks <= 36, "a 400 ms stop: watched %d ticks", w.ticks);
    CHECK(w.first >= 1040 * MS && w.first <= 1070 * MS, "a 400 ms stop: watched from %llu ms", (unsigned long long)(w.first / MS));
    CHECK(w.last >= 1390 * MS && w.last < 1400 * MS, "a 400 ms stop: watched to %llu ms", (unsigned long long)(w.last / MS));

    /* A screen that just stops (no input) is watched up to HV_STALL_QUIET_US, then not; with input during it, on. */
    memset(&tl, 0, sizeof tl);
    frames_every(&tl, 0, 1000 * MS, 16 * MS);
    w = watched(&tl, 4000 * MS, 0, 0);
    CHECK(w.last <= 1000 * MS + HV_STALL_QUIET_US + 10 * MS, "a still screen: watched to %llu ms", (unsigned long long)(w.last / MS));
    input_at(&tl, 1500 * MS);
    w = watched(&tl, 4000 * MS, 0, 0);
    CHECK(w.last >= 3990 * MS, "a stop with input in it: watched to %llu ms", (unsigned long long)(w.last / MS));

    /* Frames that were not flowing (one now and then): a gap is the screen standing still. */
    memset(&tl, 0, sizeof tl);
    frames_every(&tl, 0, 3000 * MS, 500 * MS);
    w = watched(&tl, 3000 * MS, 0, 0);
    CHECK(w.ticks == 0, "occasional frames: watched %d ticks", w.ticks);

    /* A press on a still screen, answered after 300 ms: watched from 50 ms after it to the answer. */
    memset(&tl, 0, sizeof tl);
    frame_at(&tl, 0);
    press_at(&tl, 1000 * MS);
    frame_at(&tl, 1300 * MS);
    w = watched(&tl, 2000 * MS, 0, 0);
    CHECK(w.first >= 1050 * MS && w.first <= 1070 * MS && w.last >= 1290 * MS && w.last < 1300 * MS,
          "a late answer: watched %llu..%llu ms", (unsigned long long)(w.first / MS), (unsigned long long)(w.last / MS));

    /* The main thread's task waiting 200 ms: watched from 50 ms into the wait. */
    memset(&tl, 0, sizeof tl);
    frames_every(&tl, 0, 2000 * MS, 16 * MS);
    w = watched(&tl, 2000 * MS, 1000 * MS, 1200 * MS);
    CHECK(w.first >= 1050 * MS && w.first <= 1060 * MS && w.last >= 1190 * MS && w.last < 1200 * MS,
          "the main thread's wait: watched %llu..%llu ms", (unsigned long long)(w.first / MS), (unsigned long long)(w.last / MS));

    /* The budget: every tick for HV_STALL_BUDGET_TICKS, then every HV_STALL_BUDGET_RATE-th; ticks not watched earn
       it back; a tick not watched never samples. */
    HvStallBudget b;
    hv_stall_budget_fill(&b);
    int n = 0;
    for (int i = 0; i < HV_STALL_BUDGET_TICKS; i++) n += hv_stall_budget_take(&b, 1);
    CHECK(n == HV_STALL_BUDGET_TICKS, "full bank: %d of %d ticks sampled", n, HV_STALL_BUDGET_TICKS);
    n = 0;
    for (int i = 0; i < 400; i++) n += hv_stall_budget_take(&b, 1);
    CHECK(n >= 400 / HV_STALL_BUDGET_RATE - 2 && n <= 400 / HV_STALL_BUDGET_RATE + 2, "spent: %d of 400 ticks sampled", n);
    n = 0;
    for (int i = 0; i < 40; i++) n += hv_stall_budget_take(&b, 0);
    CHECK(n == 0, "not watching: %d ticks sampled", n);
    n = 0;
    for (int i = 0; i < 10; i++) n += hv_stall_budget_take(&b, 1);
    CHECK(n == 10, "earned back by 40 ticks unwatched: %d of 10 ticks sampled", n);
    for (int i = 0; i < 100000; i++) hv_stall_budget_take(&b, 0);
    n = 0;
    for (int i = 0; i < 2 * HV_STALL_BUDGET_TICKS; i++) n += hv_stall_budget_take(&b, 1);
    CHECK(n <= HV_STALL_BUDGET_TICKS + HV_STALL_BUDGET_TICKS / HV_STALL_BUDGET_RATE + 1,
          "a bank never holds more than its %d ticks: %d of %d sampled", HV_STALL_BUDGET_TICKS, n, 2 * HV_STALL_BUDGET_TICKS);
}

int main(void) {
    detection();
    watching();
    overlap();
    records();
    if (failures) {
        fprintf(stderr, "test_stallmon: %d failures\n", failures);
        return 1;
    }
    printf("test_stallmon: ok\n");
    return 0;
}
