/* The freeze recorder (host/stallmon.c) when it is itself kept off its core.
   - Its thread runs at the highest QoS class, at or above the guest CPUs' (USER_INITIATED on the iPad): it stops
     each CPU thread for a moment to read it, and pre-empted then it would leave that CPU stopped.
   - Stopped for 700 ms while Henge's other threads keep a core busy (starved, as by threads above it), it writes a
     "starved" record of about that length once it runs again; before, it dropped the stretch without a word.
   - Stopped for 700 ms while nothing in Henge runs (as when iOS suspends the app), it writes nothing.
   The frames keep coming throughout, so nothing else is a stall.
   Nothing here counts on how busy the Mac is: the first hold lasts until Henge's threads have had half a core over
   it, by their CPU time (the recorder asks a quarter), and the record's length is checked against the hold as it
   came out; the test waits for the record rather than a fixed time.

   Usage: test_stallmon_starved [folder] */
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "../core/cpu.h"
#include "../core/machine.h"
#include "../host/stallmon.h"
#include "../host/vm.h"
#include "scratch_dir.h"

__thread HvCpu *hv_self;
void (*hv_prof_register)(void);
static HvMachine machine;
static _Atomic uint64_t frames;
HvMachine *hv_vm_machine(void) { return &machine; }
int hv_vm_paused(void) { return 0; }
uint64_t hv_vm_gpu_frames(void) { return atomic_load(&frames); }
void hv_vm_gfx_stages(uint32_t ms[5]) { memset(ms, 0, 5 * sizeof *ms); }
static int running(void) { return 1; }
static double no_input(void) { return -1; }
static double no_press(void) { return 1e9; }

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

static _Atomic int done, burning;

static void *frame_thread(void *unused) {
    (void)unused;
    while (!atomic_load(&done)) atomic_fetch_add(&frames, 1), usleep(16000);
    return NULL;
}

enum { BURNERS = 4 };
static uint64_t wall_us(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000; }
/* The process's CPU time, as the recorder counts it */
static uint64_t process_us(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
}

static void *burner(void *unused) {
    (void)unused;
    volatile uint64_t x = 0;
    while (!atomic_load(&done))
        if (atomic_load(&burning)) x++;
        else usleep(1000);
    return NULL;
}

/* The recorder's thread, by its name. */
static thread_act_t monitor_thread(pthread_t *pt) {
    thread_act_array_t list;
    mach_msg_type_number_t n;
    thread_act_t found = MACH_PORT_NULL;
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) return found;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        pthread_t p = pthread_from_mach_thread_np(list[i]);
        char name[64] = "";
        if (p) pthread_getname_np(p, name, sizeof name);
        if (found == MACH_PORT_NULL && !strcmp(name, "henge.freezes")) found = list[i], *pt = p;
        else mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof *list);
    return found;
}

static int count(const char *path, const char *what) {
    FILE *f = fopen(path, "r");
    int n = 0;
    static char line[1 << 16];
    while (f && fgets(line, sizeof line, f)) n += strstr(line, what) != NULL;
    if (f) fclose(f);
    return n;
}

static const char *dir;

static void *scenario(void *unused) {
    (void)unused;
    pthread_t t;
    pthread_create(&t, NULL, frame_thread, NULL);
    for (int i = 0; i < BURNERS; i++) pthread_create(&t, NULL, burner, NULL);
    usleep(300000);

    pthread_t mp = NULL;
    thread_act_t mon = monitor_thread(&mp);
    CHECK(mon != MACH_PORT_NULL, "no henge.freezes thread");
    if (mon == MACH_PORT_NULL) exit(1);
    qos_class_t q = QOS_CLASS_UNSPECIFIED;
    int rel = 0;
    pthread_get_qos_class_np(mp, &q, &rel);
    CHECK(q == QOS_CLASS_USER_INTERACTIVE, "the recorder runs at QoS 0x%x, below the guest CPUs' USER_INITIATED (0x%x) or level",
          q, QOS_CLASS_USER_INITIATED);

    char path[1200];
    snprintf(path, sizeof path, "%s/henge-freezes.jsonl", dir);

    /* starved: kept off its core while Henge's other threads run. Held 700 ms at least, and until they have used
       half a core over the hold (on a busy Mac they may take longer to): 10 s without that and the test can't tell. */
    atomic_store(&burning, 1);
    thread_suspend(mon);
    uint64_t held_at = wall_us(), cpu_at = process_us(), held = 0;
    while ((held = wall_us() - held_at) < 700000 || process_us() - cpu_at < held / 2) {
        if (held > 10000000) break;
        usleep(5000);
    }
    thread_resume(mon);
    atomic_store(&burning, 0);
    CHECK(held <= 10000000, "the burners had under half a core for 10 s: the test can't tell");
    for (uint64_t until = wall_us() + 5000000; !hv_stall_records() && wall_us() < until;) usleep(5000);
    int starved = count(path, "\"kind\":\"starved\"");
    CHECK(starved == 1, "%d starved records after %llu ms without its core", starved, (unsigned long long)held / 1000);
    FILE *f = fopen(path, "r");
    static char line[1 << 16];
    while (f && fgets(line, sizeof line, f)) {
        if (!strstr(line, "\"kind\":\"starved\"")) continue;
        CHECK(strstr(line, "\"verdict\":\"starved\""), "verdict: %s", line);
        const char *m = strstr(line, "\"ms\":");
        double ms = m ? atof(m + 5) : 0, h = (double)held / 1000;
        /* From its last tick before the hold to its first after: on a busy Mac it may wait a while for its core
           once let go. */
        CHECK(ms >= h - 50 && ms <= h + 1300, "length %.0f ms for %.0f ms held: %s", ms, h, line);
    }
    if (f) fclose(f);

    /* the app suspended: nothing in Henge runs while the recorder is held */
    usleep(1100000); /* past the one-a-second limit, so a second record would show */
    thread_suspend(mon);
    usleep(700000);
    unsigned judged = hv_stall_ticks();
    thread_resume(mon);
    /* until two ticks are past their held-up check: the one it was stopped in, if it was, and the first after the
       hold, which decides (any record it writes is in the file by then) */
    for (uint64_t until = wall_us() + 5000000; hv_stall_ticks() - judged < 2 && wall_us() < until;) usleep(1000);
    CHECK(hv_stall_ticks() - judged >= 2, "the recorder didn't get past two ticks in 5 s after it was let go");
    CHECK(count(path, "\"kind\":\"starved\"") == starved, "a hold-up with Henge's threads idle was recorded as starved");
    CHECK(count(path, "\"kind\"") == hv_stall_records(), "records %d in the file, %d counted", count(path, "\"kind\""), hv_stall_records());
    CHECK(count(path, "\"kind\"") == starved, "%d records: only the starved one expected", count(path, "\"kind\""));

    atomic_store(&done, 1);
    usleep(50000);
    scratch_done();
    if (failures) {
        fprintf(stderr, "test_stallmon_starved: %d failures\n", failures);
        exit(1);
    }
    printf("test_stallmon_starved: ok\n");
    exit(0);
}

int main(int argc, char **argv) {
    dir = scratch_dir(argc, argv, "stallmon_starved");
    static uint32_t code[1024];
    machine.jit.base = code;
    machine.jit.capacity = 1024;
    HvStallHooks h = {.running = running, .input_idle_s = no_input, .press_idle_s = no_press};
    hv_stall_start(dir, &h);
    pthread_t t;
    pthread_create(&t, NULL, scenario, NULL);
    dispatch_main();
}
