/* henge.log's queue and writer thread (ios/Henge/Native/henge-logq.c): lines in order from many threads, a full
   queue that drops instead of waiting and marks the gap, the crash path writing what is still queued (and what is
   logged after it), exit() waiting for the queue, flush's timeout, a line too long for the queue, and failed writes
   counted and marked. Each case runs in a child of its own: the queue is started once per process. */
#include "scratch_dir.h"
#include "henge-logq.h"

#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

static const char *dir;
static int failures;

#define CHECK(c, ...)                                                    \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);         \
            fprintf(stderr, __VA_ARGS__);                                \
            fprintf(stderr, "\n");                                       \
            failures++;                                                  \
        }                                                                \
    } while (0)

static double now_s(void) {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (double)t.tv_sec + t.tv_usec / 1e6;
}

static void path_for(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", dir, name); }

static int open_log(const char *name) {
    char p[1100];
    path_for(p, sizeof p, name);
    return open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
}

static char *slurp(const char *name, size_t *len) {
    char p[1100];
    path_for(p, sizeof p, name);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = 0;
    if (len) *len = got;
    return b;
}

static void push_str(const char *s) { henge_logq_push(s, strlen(s)); }

/* ---- many threads, each line in its thread's order, none lost ---- */

enum { THREADS = 4, PER = 20000 };
static double worst_push;
static pthread_mutex_t worst_mu = PTHREAD_MUTEX_INITIALIZER;

static void *producer(void *arg) {
    int id = (int)(intptr_t)arg;
    char line[64];
    double worst = 0;
    for (int i = 0; i < PER; i++) {
        int n = snprintf(line, sizeof line, "T%d %06d some text to make the line a realistic length\n", id, i);
        double t0 = now_s();
        henge_logq_push(line, (size_t)n);
        double d = now_s() - t0;
        if (d > worst) worst = d;
    }
    pthread_mutex_lock(&worst_mu);
    if (worst > worst_push) worst_push = worst;
    pthread_mutex_unlock(&worst_mu);
    return NULL;
}

static int case_order(void) {
    int fd = open_log("order.log");
    CHECK(henge_logq_start(fd) == 0, "start");
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) pthread_create(&t[i], NULL, producer, (void *)(intptr_t)i);
    for (int i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    CHECK(henge_logq_flush(10) == 1, "flush in time");
    CHECK(henge_logq_pending() == 0, "nothing pending after flush (%llu)", (unsigned long long)henge_logq_pending());
    char *b = slurp("order.log", NULL);
    int next[THREADS] = {0}, lines = 0, markers = 0, out_of_order = 0;
    for (char *s = b, *e; s && *s; s = e + 1) {
        e = strchr(s, '\n');
        if (!e) break;
        int id, seq;
        if (sscanf(s, "T%d %d", &id, &seq) == 2 && id >= 0 && id < THREADS) {
            if (seq < next[id]) out_of_order++;
            next[id] = seq + 1;
            lines++;
        } else if (!strncmp(s, "[log] ", 6)) {
            markers++;
        }
    }
    unsigned long long dropped = henge_logq_dropped();
    CHECK(out_of_order == 0, "%d lines out of their thread's order", out_of_order);
    CHECK((unsigned long long)lines + dropped == THREADS * PER, "%d lines + %llu dropped != %d", lines, dropped,
          THREADS * PER);
    CHECK(dropped == 0 || markers > 0, "drops are marked in the file");
    CHECK(worst_push < 0.05, "a push took %.3f s", worst_push);
    printf("  order: %d lines from %d threads, %llu dropped, slowest push %.1f us\n", lines, THREADS, dropped,
           worst_push * 1e6);
    free(b);
    return failures;
}

/* ---- the writer stuck: pushes still return at once, drop and count, and the gap is marked ---- */

static int case_full(void) {
    int fd = open_log("full.log");
    henge_logq_start(fd);
    henge_logq_test_hold(1);
    char line[128];
    int queued = 0;
    double worst = 0;
    for (int i = 0; henge_logq_dropped() < 3000; i++) {
        int n = snprintf(line, sizeof line, "line %07d ................................................\n", i);
        double t0 = now_s();
        if (henge_logq_push(line, (size_t)n) == 0) queued++;
        double d = now_s() - t0;
        if (d > worst) worst = d;
        if (i > 1000000) break;
    }
    CHECK(henge_logq_dropped() == 3000, "3000 dropped (%llu)", (unsigned long long)henge_logq_dropped());
    CHECK(worst < 0.05, "a push into a full queue took %.3f s", worst);
    CHECK(henge_logq_pending() > (1u << 20) - 256, "the queue is full (%llu)", (unsigned long long)henge_logq_pending());
    henge_logq_test_hold(0);
    CHECK(henge_logq_flush(10) == 1, "the full queue written");
    push_str("after the gap\n");
    CHECK(henge_logq_flush(10) == 1, "flush in time");
    char *b = slurp("full.log", NULL);
    char want[96];
    snprintf(want, sizeof want, "[log] 3000 lines dropped here: the log file fell behind\nafter the gap\n");
    size_t bl = strlen(b), wl = strlen(want);
    CHECK(bl >= wl && !strcmp(b + bl - wl, want), "the file ends with the marker, then the next line");
    int lines = 0;
    for (char *s = b; (s = strstr(s, "line ")); s++) lines++;
    CHECK(lines == queued, "every queued line written (%d of %d)", lines, queued);
    printf("  full: %d queued, 3000 dropped and marked, slowest push %.1f us\n", queued, worst * 1e6);
    free(b);
    return failures;
}

/* ---- a crash: what is still queued, the marker, native stderr's unread tail, a backtrace ---- */

static int case_crash(void) {
    int fd = open_log("crash.log");
    char np[1100];
    path_for(np, sizeof np, "native.log");
    int nw = open(np, O_CREAT | O_WRONLY | O_TRUNC, 0644), nr = open(np, O_RDONLY);
    henge_logq_start(fd);
    push_str("written before the hold\n");
    CHECK(henge_logq_flush(5) == 1, "flush in time");
    henge_logq_test_hold(1);
    char line[64];
    for (int i = 0; i < 100; i++) {
        int n = snprintf(line, sizeof line, "queued %03d\n", i);
        henge_logq_push(line, (size_t)n);
    }
    const char *native = "assertion failed: the reason it died\n";
    write(nw, native, strlen(native));
    henge_logq_install_crash_handlers(nr);
    henge_logq_crash_write(11);
    henge_logq_test_hold(0);
    push_str("after the crash: written straight to the file\n");
    usleep(100000);
    char *b = slurp("crash.log", NULL);
    char *first = strstr(b, "written before the hold\n"), *q0 = strstr(b, "queued 000\n"), *q99 = strstr(b, "queued 099\n");
    char *mark = strstr(b, "*** FATAL SIGNAL 11 -- process is dying ***"), *nat = strstr(b, native);
    char *bt = strstr(b, "--- backtrace ---");
    CHECK(first && q0 && q99 && mark && nat && bt, "all parts there (%d %d %d %d %d %d)", !!first, !!q0, !!q99,
          !!mark, !!nat, !!bt);
    CHECK(first < q0 && q0 < q99 && q99 < mark && mark < nat && nat < bt, "in order");
    int queued = 0;
    for (char *s = b; (s = strstr(s, "queued ")); s++) queued++;
    CHECK(queued == 100, "each queued line once (%d)", queued);
    char *after = strstr(b, "after the crash: written straight to the file\n");
    CHECK(after && after > bt, "a line logged after the crash path (the reader's last native lines) still lands");
    printf("  crash: 100 queued lines, marker, native tail and backtrace, in order\n");
    free(b);
    return failures;
}

/* ---- two threads dying at once: the queue once, the first thread's whole record, then the second's. (In the app
   the first one's raise() usually ends the process before the second writes; here both finish. The two calls may
   or may not overlap; either way nothing is written twice or interleaved.) ---- */

static void *crash_too(void *arg) {
    (void)arg;
    henge_logq_crash_write(7);
    return NULL;
}

static int case_crash2(void) {
    int fd = open_log("crash2.log");
    henge_logq_start(fd);
    henge_logq_test_hold(1);
    char line[64];
    for (int i = 0; i < 2000; i++) {
        int n = snprintf(line, sizeof line, "queued %04d\n", i);
        henge_logq_push(line, (size_t)n);
    }
    pthread_t t;
    pthread_create(&t, NULL, crash_too, NULL);
    henge_logq_crash_write(11);
    pthread_join(t, NULL);
    char *b = slurp("crash2.log", NULL);
    int queued = 0, marks = 0, bts = 0;
    for (char *s = b; (s = strstr(s, "queued ")); s++) queued++;
    for (char *s = b; (s = strstr(s, "*** FATAL SIGNAL ")); s++) marks++;
    for (char *s = b; (s = strstr(s, "--- backtrace ---")); s++) bts++;
    char *m1 = strstr(b, "*** FATAL SIGNAL "), *bt1 = strstr(b, "--- backtrace ---");
    char *m2 = m1 ? strstr(m1 + 1, "*** FATAL SIGNAL ") : NULL, *last = strstr(b, "queued 1999\n");
    CHECK(queued == 2000, "each queued line once (%d)", queued);
    CHECK(marks == 2 && bts == 2, "both threads' markers and backtraces (%d %d)", marks, bts);
    CHECK(last && m1 && bt1 && m2 && last < m1 && m1 < bt1 && bt1 < m2, "the queue, the first record, then the second");
    printf("  crash2: two threads dying at once, the queue once and each record whole\n");
    free(b);
    return failures;
}

/* ---- exit(): waits for the queue ---- */

static void *let_go(void *arg) {
    (void)arg;
    usleep(150000);
    henge_logq_test_hold(0);
    return NULL;
}

static int case_exit(void) {
    int fd = open_log("exit.log");
    henge_logq_start(fd);
    henge_logq_test_hold(1);
    char line[64];
    for (int i = 0; i < 1000; i++) {
        int n = snprintf(line, sizeof line, "before exit %04d\n", i);
        henge_logq_push(line, (size_t)n);
    }
    pthread_t t;
    pthread_create(&t, NULL, let_go, NULL);
    exit(0); /* the parent counts the lines */
}

/* ---- flush gives up at its timeout ---- */

static int case_timeout(void) {
    int fd = open_log("timeout.log");
    henge_logq_start(fd);
    henge_logq_test_hold(1);
    push_str("held\n");
    double t0 = now_s();
    int ok = henge_logq_flush(0.2);
    double took = now_s() - t0;
    CHECK(ok == 0, "a held writer cannot be flushed");
    CHECK(took >= 0.15 && took < 1.5, "flush waited its timeout (%.3f s)", took);
    henge_logq_test_hold(0);
    CHECK(henge_logq_flush(5) == 1, "flushed once let go");
    printf("  timeout: flush gave up after %.2f s, then flushed\n", took);
    return failures;
}

/* ---- a line too long for the queue is cut, not lost ---- */

static int case_long(void) {
    int fd = open_log("long.log");
    henge_logq_start(fd);
    size_t n = 400 * 1024;
    char *big = malloc(n);
    memset(big, 'x', n - 1);
    big[n - 1] = '\n';
    CHECK(henge_logq_push(big, n) == 0, "a 400 KiB line is queued");
    push_str("after the long line\n");
    CHECK(henge_logq_flush(5) == 1, "flush in time");
    size_t len = 0;
    char *b = slurp("long.log", &len);
    CHECK(len == 256 * 1024 + strlen("after the long line\n"), "cut to 256 KiB with its newline (%zu)", len);
    CHECK(b[256 * 1024 - 1] == '\n' && b[256 * 1024 - 2] == 'x', "the cut line ends in a newline");
    printf("  long: a 400 KiB line cut to 256 KiB, the next line after it\n");
    free(b);
    free(big);
    return failures;
}

/* ---- the file cannot be written: the bytes given up are counted, and said once it can ---- */

static int case_lost(void) {
    char p[1100];
    path_for(p, sizeof p, "lost.log");
    close(open(p, O_CREAT | O_TRUNC | O_WRONLY, 0644));
    int fd = open(p, O_RDONLY); /* every write fails, as on full storage */
    henge_logq_start(fd);
    push_str("never written: 23 bytes\n");
    henge_logq_flush(5);
    int w = open(p, O_WRONLY | O_APPEND);
    dup2(w, fd); /* storage back */
    push_str("written again\n");
    CHECK(henge_logq_flush(5) == 1, "flush in time");
    char *b = slurp("lost.log", NULL);
    CHECK(!strcmp(b, "[log] 24 bytes lost here: henge.log could not be written\nwritten again\n"),
          "the loss is said before the next line (got: %s)", b);
    printf("  lost: a failed write counted and marked\n");
    free(b);
    return failures;
}

static int run_child(const char *name, int (*fn)(void)) {
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        int r = fn();
        fflush(stdout);
        _exit(r ? 1 : 0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (!ok) fprintf(stderr, "FAIL case %s (status %d)\n", name, st);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    dir = scratch_dir(argc, argv, "test_logq");
    int bad = 0;
    bad += run_child("order", case_order);
    bad += run_child("full", case_full);
    bad += run_child("crash", case_crash);
    bad += run_child("crash2", case_crash2);
    bad += run_child("timeout", case_timeout);
    bad += run_child("long", case_long);
    bad += run_child("lost", case_lost);
    bad += run_child("exit", case_exit);
    char *b = slurp("exit.log", NULL);
    int lines = 0;
    for (char *s = b; s && (s = strstr(s, "before exit ")); s++) lines++;
    if (lines != 1000) {
        fprintf(stderr, "FAIL exit: %d of 1000 lines written before the process ended\n", lines);
        bad++;
    } else {
        printf("  exit: all 1000 queued lines written before the process ended\n");
    }
    free(b);
    scratch_done();
    printf(bad ? "test_logq: %d FAILED\n" : "test_logq: all passed\n", bad);
    return bad ? 1 : 0;
}
