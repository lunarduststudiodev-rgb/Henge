/* The crash handler's writer (host/sig_say.h): what it prints matches printf, and a crash while another thread
   holds stderr's lock still ends the process, where a handler printing through stdio hangs. Forked children,
   each watched for a second. */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <fcntl.h>
#include "sig_say.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* What sig_say wrote, read back through a pipe on fd 2 */
static int said_pipe[2], saved_err;
static void capture(void) { pipe(said_pipe), saved_err = dup(2), dup2(said_pipe[1], 2); }
static void said(char *out, size_t n) {
    dup2(saved_err, 2), close(saved_err), close(said_pipe[1]);
    ssize_t r = read(said_pipe[0], out, n - 1);
    out[r > 0 ? r : 0] = 0;
    close(said_pipe[0]);
}

#define SAME(...) do { \
        char want[4096], got[4096]; \
        snprintf(want, sizeof want, __VA_ARGS__); \
        capture(), sig_say(__VA_ARGS__), said(got, sizeof got); \
        CHECK(strcmp(want, got) == 0, "\"%s\" printed \"%s\"", want, got); \
    } while (0)

static void test_format(void) {
    printf("the same text as printf\n");
    SAME("hengevm: signal %d in translated code: pc %llx (pc is rx%+lld, zone %d of %d, %s)\n", 11, 0xfffffe0007004000ull, -12ll, -1, 4, "no live code there");
    SAME("x%d=%llx%c", 28, 0ull, '\n');
    SAME(" %08x %08x", 0xd503201fu, 0x14000000u);
    SAME("addr %p, rx%+lld, %llu MB, %u", (void *)0x1234, 0x40ll, 18446744073709551615ull, 0u);
    SAME("%lld %d %%", -9223372036854775807ll - 1, -2147483647 - 1);
    char big[2000];
    memset(big, 'a', sizeof big - 1), big[sizeof big - 1] = 0;
    SAME("%s|", big); /* longer than its buffer */
}

/* A child: another thread holds stderr's lock, this one crashes. Its handler prints through sig_say, or
   through stdio, then hands the signal on. How did it end, within a second? */
static int with_stdio;
static void on_segv(int sig) {
    if (with_stdio) fprintf(stderr, "crash: signal %d\n", sig);
    else sig_say("crash: signal %d\n", sig);
    signal(sig, SIG_DFL);
    raise(sig);
}
static void *hold_stderr(void *arg) {
    flockfile(stderr);
    *(volatile int *)arg = 1;
    for (;;) pause();
}
static int crash_ends(int stdio) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, 2);
        with_stdio = stdio;
        signal(SIGSEGV, on_segv);
        volatile int held = 0;
        pthread_t t;
        pthread_create(&t, NULL, hold_stderr, (void *)&held);
        while (!held) {}
        raise(SIGSEGV);
        _exit(0);
    }
    for (int i = 0; i < 100; i++) {
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid) return WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV;
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL), waitpid(pid, NULL, 0);
    return 0; /* still there: hung */
}

static void test_crash_while_locked(void) {
    printf("a crash while stderr is locked\n");
    CHECK(crash_ends(0), "through sig_say: ends with the signal");
    CHECK(!crash_ends(1), "through stdio it hangs (what this test is here to catch)");
}

int main(void) {
    test_format();
    test_crash_while_locked();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
