/* The benchmark's measures: frame gaps (devices/frame_pace.c) and a step's line (host/bench_json.c). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../host/bench.h"

static int fails;
#define CHECK(c)                                                                                                        \
    do {                                                                                                                \
        if (!(c)) {                                                                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                \
            fails++;                                                                                                    \
        }                                                                                                               \
    } while (0)
#define NEAR(a, b) CHECK(fabs((double)(a) - (double)(b)) < 1e-6)

static HvPace pace;

static void test_pace(void) {
    HvPaceCopy a, b;
    hv_pace_copy(&pace, &a);
    CHECK(a.frames == 0);
    /* 100 frames 16.6 ms apart, then one 50.2 ms late (3 T: 2 missing), one 120 ms (7 T: 6 missing, stalled), and 2 s */
    uint64_t t = 1000000000ull;
    for (int i = 0; i < 100; i++, t += 16600000) hv_pace_note(&pace, t);
    t += 50200000 - 16600000;
    hv_pace_note(&pace, t);
    t += 120000000;
    hv_pace_note(&pace, t);
    hv_pace_copy(&pace, &b);
    HvPaceSummary s;
    hv_pace_summary(&a, &b, 2.0, 16.7, &s);
    CHECK(s.frames == 102);
    CHECK(s.gaps == 101); /* the first frame has no gap before it */
    NEAR(s.fps, 51);
    NEAR(s.p50_ms, 16);
    NEAR(s.max_ms, 120);
    NEAR(s.late, 2.0 / 101);
    NEAR(s.stalled, 1.0 / 101);
    NEAR(s.dropped_per_s, (2 + 6) / 2.0);
    /* a gap of a second or more: the full length when it is the longest */
    HvPaceCopy c;
    t += 2500000000ull;
    hv_pace_note(&pace, t);
    hv_pace_copy(&pace, &c);
    hv_pace_summary(&b, &c, 2.5, 16.7, &s);
    CHECK(s.gaps == 1);
    NEAR(s.max_ms, 2500);
    NEAR(s.dropped_per_s, 148 / 2.5); /* 2500 / 16.7 = 149.7 refreshes: 148 frames missing */
    /* no target: nothing late or dropped */
    hv_pace_summary(&a, &b, 2.0, 0, &s);
    CHECK(s.late == 0 && s.stalled == 0 && s.dropped_per_s == 0 && s.gaps == 101);
    /* a gap just under a refresh and a quarter isn't late; 1.75 T counts one missing */
    HvPace q = {0};
    HvPaceCopy q0, q1;
    hv_pace_copy(&q, &q0);
    hv_pace_note(&q, 1000000);
    hv_pace_note(&q, 1000000 + 20000000);  /* 20 ms: under 1.25 T = 20.9 */
    hv_pace_note(&q, 21000000 + 29500000); /* 29.5 ms: 1.77 T */
    hv_pace_copy(&q, &q1);
    hv_pace_summary(&q0, &q1, 1, 16.7, &s);
    NEAR(s.late, 0.5);
    NEAR(s.dropped_per_s, 1);
}

/* What became of the Mac's frames on the way to the glass (hv_frame_replaced, hv_frame_presented). */
static void test_flow(void) {
    HvFrameFlowCopy a, b;
    HvFrameFlowSummary s;
    HvPaceCopy p0, p1;
    hv_frame_flow_copy(&hv_frame_flow, &a);
    hv_pace_copy(&hv_pace_shown, &p0);
    hv_frame_replaced();
    hv_frame_replaced();
    hv_frame_presented(0, 5000000); /* never shown: counted, no delay, no shown frame */
    uint64_t swap = 1000000000ull;
    for (int i = 0; i < 98; i++, swap += 16666666) hv_frame_presented(swap + 20000000, swap); /* 20 ms each */
    hv_frame_presented(swap + 250000000, swap);                                              /* one 250 ms */
    hv_frame_presented(swap + 2000000000ull, swap + 1);                                       /* one 2 s: past the buckets */
    hv_frame_presented(swap + 3000000, 0);  /* shown, swap not known: a shown frame, no delay */
    hv_frame_presented(swap, swap + 5);     /* shown before its swap (another clock?): no delay */
    hv_frame_flow_copy(&hv_frame_flow, &b);
    hv_pace_copy(&hv_pace_shown, &p1);
    hv_frame_flow_summary(&a, &b, &s);
    CHECK(s.replaced == 2 && s.not_shown == 1);
    CHECK(s.delays == 100);
    NEAR(s.delay_p50_ms, 20);
    CHECK(fabs(s.delay_max_ms - 2000) < 1e-3); /* the true longest when past the last bucket */
    CHECK(s.delay_p99_ms >= 250 && s.delay_p99_ms < 1000);
    CHECK(p1.frames - p0.frames == 102); /* every shown frame times the screen's pace */
    /* nothing between two readings */
    CHECK(s.held.n == 0 && s.held.max_ms == 0 && s.guest_waited.n == 0); /* nothing timed */
    hv_frame_flow_summary(&b, &b, &s);
    CHECK(s.replaced == 0 && s.not_shown == 0 && s.delays == 0 && s.delay_max_ms == 0);

    /* each present's hold on the Mac's GPU work (hv_frame_held): 98 of 0.2 ms, one of 3.05 ms, one of 40 ms (past the
       buckets: its own length) */
    for (int i = 0; i < 98; i++) hv_frame_held(200000);
    hv_frame_held(3050000);
    hv_frame_held(40000000);
    HvFrameFlowCopy c;
    hv_frame_flow_copy(&hv_frame_flow, &c);
    hv_frame_flow_summary(&b, &c, &s);
    CHECK(s.held.n == 100 && s.delays == 0 && s.replaced == 0 && s.guest_waited.n == 0);
    CHECK(fabs(s.held.mean_ms - (98 * 0.2 + 3.05 + 40) / 100) < 1e-9);
    CHECK(fabs(s.held.p99_ms - 3.0) < 1e-9); /* to the 0.1 ms bucket */
    CHECK(fabs(s.held.max_ms - 40) < 1e-9);
    hv_frame_held(1500000);
    hv_frame_guest_waited(700000); /* the Mac's batches' waits: kept apart from the holds */
    hv_frame_guest_waited(25000000);
    hv_frame_flow_copy(&hv_frame_flow, &b);
    hv_frame_flow_summary(&c, &b, &s);
    CHECK(s.held.n == 1 && fabs(s.held.max_ms - 1.5) < 1e-9); /* the step's own longest, not the run's 40 ms */
    CHECK(s.guest_waited.n == 2 && fabs(s.guest_waited.mean_ms - 12.85) < 1e-9 && fabs(s.guest_waited.max_ms - 25) < 1e-9);
}

static const char *find(const char *json, const char *key) {
    static char pat[80];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(json, pat);
    return p ? p + strlen(pat) : NULL;
}
static double num(const char *json, const char *key) {
    const char *p = find(json, key);
    return p ? atof(p) : NAN;
}

static void test_step(void) {
    static HvBenchSnap a, b;
    a.t_ns = 5000000000ull;
    b.t_ns = a.t_ns + 10000000000ull; /* 10 s */
    a.blocks = 1000, b.blocks = 1000 + 30000000;
    a.translations = 100, b.translations = 100 + 2000;
    a.retrans_evicted = 10, b.retrans_evicted = 10 + 500;
    a.retrans_written = 3, b.retrans_written = 3 + 100;
    a.lost_races = 7, b.lost_races = 7 + 40;
    a.page_drops = 2, b.page_drops = 2 + 50;
    a.retired = 1, b.retired = 1 + 150;
    a.retired_words = 0, b.retired_words = 25600; /* 100 KB over 50 drops */
    a.rewrite_same = 1, b.rewrite_same = 1 + 10;
    a.rewrite_changed = 0, b.rewrite_changed = 30;
    b.entries_written[0] = 3, b.entries_written[3] = 1; /* 75% once, 25% more than 64 times */
    a.entries_evicted[1] = 5, b.entries_evicted[1] = 5 + 2, b.entries_evicted[2] = 2;
    a.tlbi_all_here = 0, b.tlbi_all_here = 1020;
    a.tlb_full_drops = 5, b.tlb_full_drops = 5 + 1020;
    a.cache_drops_all = 1, b.cache_drops_all = 2;
    a.cache_empties = 4, b.cache_empties = 4 + 6;
    a.empties_in_stop = 1, b.empties_in_stop = 1 + 2;
    a.blocks_dropped = 1000, b.blocks_dropped = 1000 + 90000;
    a.stops = 5, b.stops = 5 + 7;
    a.stop_s = 1.0, b.stop_s = 1.125;
    a.stop_max_s = 9, b.stop_max_s = 0.04; /* the longest in the step is the end's reading alone */
    a.flow.replaced = 3, b.flow.replaced = 3 + 12;
    a.flow.not_shown = 0, b.flow.not_shown = 2;
    b.flow.delay[25] = 299, b.flow.delay[40] = 1, b.flow.delays = 300;
    a.flow.held.span[2] = 7, a.flow.held.ns = 1400000, a.flow.held.n = 7; /* before the step: not counted */
    b.flow.held.span[2] = 7 + 98, b.flow.held.span[12] = 2, b.flow.held.ns = 1400000 + 98 * 200000 + 2 * 1200000, b.flow.held.n = 7 + 100;
    b.flow.guest_waited.span[5] = 3, b.flow.guest_waited.span[40] = 1, b.flow.guest_waited.ns = 3 * 500000 + 4000000;
    b.flow.guest_waited.n = 4;
    a.sw_saved = 0, b.sw_saved = 300;
    a.sw_dropped = 0, b.sw_dropped = 100;
    a.code_protects = 0, b.code_protects = 70;
    a.stats_counted = b.stats_counted = 1;
    a.sync_s = 1, b.sync_s = 3;      /* 2 s waiting in 10: 20% of one CPU */
    a.wfi_s = 0, b.wfi_s = 20;       /* of 4 CPUs x 10 s: 50% idle */
    a.stop_wait_s = 1, b.stop_wait_s = 5; /* 4 s held at code-cache stops: not wanted */
    a.cpu_s = 0, b.cpu_s = 12;       /* of the 40 - 20 - 4 = 16 s wanted: 75% */
    a.cpu_threads = b.cpu_threads = 4;
    a.stalls[1] = 2, b.stalls[1] = 5; /* 3 hitches */
    b.stalls[3] = 1;
    b.stalls[4] = 1, b.stalls[5] = 1;
    a.stall_ms = 100, b.stall_ms = 900;
    a.thermal = 0, b.thermal = 1;
    static HvPace video;
    hv_pace_copy(&video, &a.mac);
    a.shown = a.mac;
    uint64_t t = 1;
    for (int i = 0; i < 300; i++, t += 33300000) hv_pace_note(&video, t);
    hv_pace_copy(&video, &b.mac);
    b.shown = b.mac;
    char out[8192];
    int n = hv_bench_step_json(out, sizeof out, "r1", "video", "ok", "", 33.3, "launch_ms_median=812.5 Bad=1 x=abc work_s=4 =3 y=",
                               &a, &b, 4);
    CHECK(n > 0 && (size_t)n == strlen(out));
    printf("%s\n", out);
    const char *head = "{\"type\":\"step\",\"run\":\"r1\",\"step\":\"video\",\"status\":\"ok\",\"why\":\"\",\"target_ms\":33.3,\"thermal\":[0,1],\"m\":{";
    CHECK(!strncmp(out, head, strlen(head)));
    CHECK(!strcmp(out + n - 2, "}}"));
    NEAR(num(out, "wall_s"), 10);
    NEAR(num(out, "mac_fps"), 30);
    NEAR(num(out, "mac_p50_ms"), 33);
    NEAR(num(out, "mac_late_pct"), 0);
    NEAR(num(out, "mac_dropped_per_s"), 0);
    NEAR(num(out, "shown_fps"), 30);
    CHECK(!find(out, "shown_p50_ms")); /* the shown frames: the short list */
    NEAR(num(out, "hitches"), 3);
    NEAR(num(out, "hangs"), 1);
    NEAR(num(out, "app_stalls"), 1); /* kind 5 is the same stall as kind 4 */
    NEAR(num(out, "stall_ms"), 800);
    NEAR(num(out, "blocks_m_per_s"), 3);
    NEAR(num(out, "translated_per_s"), 200);
    NEAR(num(out, "retrans_evicted_pct"), 25);
    NEAR(num(out, "retrans_written_pct"), 5);
    NEAR(num(out, "lost_races_per_s"), 4);
    NEAR(num(out, "page_drops_per_s"), 5);
    NEAR(num(out, "blocks_per_page_drop"), 3);
    NEAR(num(out, "kb_per_page_drop"), 2);
    NEAR(num(out, "rewrites_same_pct"), 25);
    CHECK(!find(out, "again_pct"));
    NEAR(num(out, "entries_before_write_1_pct"), 75);
    NEAR(num(out, "entries_before_write_2_8_pct"), 0);
    NEAR(num(out, "entries_before_write_65up_pct"), 25);
    NEAR(num(out, "entries_before_evict_2_8_pct"), 50);
    NEAR(num(out, "entries_before_evict_9_64_pct"), 50);
    NEAR(num(out, "entries_before_evict_1_pct"), 0);
    NEAR(num(out, "tlbi_all_per_s"), 102);
    NEAR(num(out, "tlb_full_drops_per_s"), 102);
    CHECK(!find(out, "drops_full_per_s"));
    NEAR(num(out, "cache_drops_all"), 1);
    NEAR(num(out, "cache_empties"), 6);
    NEAR(num(out, "evict_sync"), 2);
    NEAR(num(out, "blocks_dropped"), 90000);
    NEAR(num(out, "stops"), 7);
    NEAR(num(out, "stop_ms"), 125);
    NEAR(num(out, "stop_max_ms"), 40);
    NEAR(num(out, "frames_replaced"), 12);
    NEAR(num(out, "presents_not_shown"), 2);
    NEAR(num(out, "swap_to_glass_p50_ms"), 25);
    NEAR(num(out, "swap_to_glass_max_ms"), 40);
    NEAR(num(out, "present_hold_mean_ms"), 0.22);
    NEAR(num(out, "present_hold_p99_ms"), 1.2);
    NEAR(num(out, "present_hold_max_ms"), 1.2);
    NEAR(num(out, "mac_gpu_wait_mean_ms"), 1.375);
    NEAR(num(out, "mac_gpu_wait_p99_ms"), 4);
    NEAR(num(out, "mac_gpu_wait_max_ms"), 4);
    NEAR(num(out, "switches_saved_per_s"), 30);
    NEAR(num(out, "switches_dropped_per_s"), 10);
    NEAR(num(out, "new_code_pages_per_s"), 7);
    NEAR(num(out, "tlbi_wait_pct"), 20);
    NEAR(num(out, "idle_pct"), 50);
    NEAR(num(out, "cpu_got_pct"), 75);
    NEAR(num(out, "agent_launch_ms_median"), 812.5);
    NEAR(num(out, "agent_work_s"), 4);
    CHECK(!find(out, "agent_Bad") && !find(out, "agent_x") && !find(out, "agent_y") && !find(out, "agent_"));

    CHECK(!find(out, "not_counted"));
    /* HV_STATS off at either reading: those two counters read 0, so they are named as not counted, not given */
    b.stats_counted = 0;
    n = hv_bench_step_json(out, sizeof out, "r1", "video", "ok", "", 33.3, NULL, &a, &b, 4);
    CHECK(n > 0 && strstr(out, ",\"not_counted\":[\"switches_saved_per_s\",\"switches_dropped_per_s\"],\"m\":{"));
    CHECK(!strstr(strstr(out, "\"m\":"), "switches_"));
    NEAR(num(out, "translated_per_s"), 200); /* the others still given */
    NEAR(num(out, "retrans_evicted_pct"), 25); /* always counted */
    b.stats_counted = 1;

    /* the CPU threads changed: no cpu_got_pct; nothing translated: no retrans_*_pct */
    b.cpu_threads = 3;
    b.translations = a.translations;
    n = hv_bench_step_json(out, sizeof out, "r1", "idle", "ok", "", 0, NULL, &a, &b, 4);
    CHECK(n > 0 && !find(out, "cpu_got_pct") && !find(out, "retrans_evicted_pct") && !find(out, "retrans_written_pct"));
    CHECK(!find(out, "mac_late_pct")); /* no target */
    b.flow.delays = 0, memset(b.flow.delay, 0, sizeof b.flow.delay);
    n = hv_bench_step_json(out, sizeof out, "r1", "idle", "ok", "", 0, NULL, &a, &b, 4);
    CHECK(n > 0 && !find(out, "swap_to_glass_p50_ms") && find(out, "frames_replaced")); /* no delays: none said */
    b.flow.held = a.flow.held;
    n = hv_bench_step_json(out, sizeof out, "r1", "idle", "ok", "", 0, NULL, &a, &b, 4);
    CHECK(n > 0 && !find(out, "present_hold_mean_ms") && !find(out, "present_hold_max_ms")); /* no presents: none said */
    CHECK(find(out, "mac_gpu_wait_mean_ms"));
    b.flow.guest_waited = a.flow.guest_waited;
    n = hv_bench_step_json(out, sizeof out, "r1", "idle", "ok", "", 0, NULL, &a, &b, 4);
    CHECK(n > 0 && !find(out, "mac_gpu_wait_mean_ms")); /* none of the Mac's batches timed: none said */

    /* skipped: no measures; a why with quotes kept as JSON */
    n = hv_bench_step_json(out, sizeof out, "r\"1", "rosetta", "skip", "no\\rosetta\n", 0, "a=1", &a, &b, 4);
    CHECK(n > 0 && strstr(out, "\"run\":\"r\\\"1\"") && strstr(out, "\"why\":\"no\\\\rosetta\\u000a\"") && !strcmp(out + n - 7, "\"m\":{}}"));

    /* too small a buffer: -1, never a cut line */
    for (size_t cap = 0; cap < 200; cap += 7) CHECK(hv_bench_step_json(out, cap, "r1", "video", "ok", "", 33.3, "", &a, &b, 4) == -1);

    /* not a number: left out */
    b.cpu_threads = 4;
    b.t_ns = a.t_ns; /* no time passed */
    n = hv_bench_step_json(out, sizeof out, "r1", "x", "fail", "", 16.7, "", &a, &b, 4);
    CHECK(n > 0 && !strstr(out, "nan") && !strstr(out, "inf"));
}

int main(void) {
    test_pace();
    test_flow();
    test_step();
    if (fails) {
        fprintf(stderr, "%d failed\n", fails);
        return 1;
    }
    printf("test_bench_json: ok\n");
    return 0;
}
