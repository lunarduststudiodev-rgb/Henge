/* The input fence (host/input_fence.c) with share.c: a press after scroll lines waits, with everything after it, for the
   helper to answer a stop sent after them; nothing else waits, and nothing is reordered or lost. Plays the helper over
   share.c's own socket, as test_share_versions does. */
#include "scratch_dir.h"
#include "input_fence.h"
#include "share.h"

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

static int failures;
#define CHECK(ok, what)                                                                                                \
    do {                                                                                                               \
        if (!(ok)) {                                                                                                   \
            failures++;                                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, what);                                                     \
        }                                                                                                              \
    } while (0)

static char sock[200];

static void pause_ms(int ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&t, NULL);
}

static int open_link(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) return -1;
    char req[200];
    int n = snprintf(req, sizeof req, "GET /.henge/link?v=%d&trusted=1 HTTP/1.1\r\nHost: 10.0.2.4\r\n\r\n", HV_AGENT_VERSION);
    if (write(fd, req, (size_t)n) != n) return -1;
    char head[256];
    size_t got = 0;
    head[0] = 0;
    while (got < sizeof head - 1 && !strstr(head, "\r\n\r\n")) {
        ssize_t r = read(fd, head + got, 1);
        if (r <= 0) return -1;
        head[++got] = 0;
    }
    pause_ms(50);
    return strstr(head, " 101 ") ? fd : -1;
}

static void say(int fd, const char *fmt, unsigned id) {
    char line[64];
    int n = snprintf(line, sizeof line, fmt, id);
    if (write(fd, line, (size_t)n) != n) fprintf(stderr, "write failed\n");
}

static int hear(int fd, char *line, size_t cap, int ms) {
    size_t n = 0;
    while (n < cap - 1) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, ms) != 1) return -1;
        ssize_t r = read(fd, line + n, 1);
        if (r <= 0) return -1;
        if (line[n] == '\n') break;
        n++;
    }
    line[n] = 0;
    return 0;
}

/* The stop the helper hears next, or 0 if the next line isn't one (or none comes). */
static unsigned hear_stop(int fd) {
    char line[128];
    unsigned id = 0;
    if (hear(fd, line, sizeof line, 300) != 0 || sscanf(line, "stop %u", &id) != 1) return 0;
    return id;
}

/* No stop comes within ms (shorter than the fence's limit, so a hold being checked doesn't time out meanwhile). */
static int no_stop(int fd, int ms) {
    char line[128];
    unsigned id;
    while (hear(fd, line, sizeof line, ms) == 0)
        if (sscanf(line, "stop %u", &id) == 1) return 0;
    return 1;
}

/* What reached "the Mac", in order: one letter an event. */
static pthread_mutex_t got_mu = PTHREAD_MUTEX_INITIALIZER;
static char got[256];
static int delivered; /* all of them, past what got holds */
static void deliver(const HvFenceEvent *e) {
    pthread_mutex_lock(&got_mu);
    size_t n = strlen(got);
    if (n < sizeof got - 1) got[n] = (char)e->a, got[n + 1] = 0;
    delivered++;
    pthread_mutex_unlock(&got_mu);
}
static int delivered_now(void) {
    pthread_mutex_lock(&got_mu);
    int n = delivered;
    pthread_mutex_unlock(&got_mu);
    return n;
}
static void deliver_scroll(const HvFenceEvent *e) {
    hv_share_touch_scroll(e->b, 0.5, 0.5, 0, e->dy);
    deliver(e);
}
static int got_is(const char *want) {
    pthread_mutex_lock(&got_mu);
    int same = !strcmp(got, want);
    if (!same) fprintf(stderr, "  delivered \"%s\", wanted \"%s\"\n", got, want);
    pthread_mutex_unlock(&got_mu);
    return same;
}
static void got_clear(void) {
    pthread_mutex_lock(&got_mu);
    got[0] = 0;
    pthread_mutex_unlock(&got_mu);
}

static void event(char tag) { hv_fence_send(&(HvFenceEvent){.deliver = deliver, .a = tag}); }
/* A stop sent early (a finger landing), through the fence as henge-vm.c sends it. */
static void deliver_stop(const HvFenceEvent *e) { (void)e, hv_share_touch_stop(); }
static void early_stop(void) { hv_fence_send(&(HvFenceEvent){.deliver = deliver_stop}); }
static void press(char tag) { hv_fence_send(&(HvFenceEvent){.deliver = deliver, .press = 1, .a = tag}); }
/* A scroll line through the fence; fling: the finger's speed as it ends. */
static void scroll(char tag, int phase, double fling) {
    hv_fence_send(&(HvFenceEvent){.deliver = deliver_scroll, .a = tag, .b = phase, .dy = fling});
}
/* The Mac's stop count (hv_fence_set_stops): odd while it stands still. */
static _Atomic unsigned mac_stops;
static unsigned stops(void) { return mac_stops; }
static void drain(int fd) {
    char line[128];
    while (hear(fd, line, sizeof line, 50) == 0) {}
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN); /* the old link, answering after share.c closed it */
    const char *dir = scratch_dir(argc, argv, "test_input_fence");
    snprintf(sock, sizeof sock, "%s/s", dir);
    if (hv_share_start(dir, sock, NULL) != 0) {
        fprintf(stderr, "share didn't start\n");
        return 2;
    }
    /* Long, so a slow machine doesn't end a hold before it is checked; the tests of the limit itself set it short. */
    hv_fence_set_limit_ms(3000);
    hv_fence_set_stops(stops);

    /* No helper: nothing waits. */
    scroll('s', HV_SCROLL_ENDED, 0.5);
    press('P'), event('u');
    CHECK(got_is("sPu") && !hv_fence_held(), "no helper: in order, at once");
    got_clear();

    int fd = open_link();
    CHECK(fd >= 0, "the link opens");

    /* No scroll since the helper linked: a press goes at once, and no stop is sent. */
    press('P'), event('u');
    CHECK(got_is("Pu"), "nothing to wait for");
    CHECK(hear_stop(fd) == 0, "no stop sent");
    got_clear();

    /* After a fling: the press, and what follows it, wait for the answer to the stop sent for it. */
    scroll('b', HV_SCROLL_BEGAN, 0);
    scroll('e', HV_SCROLL_ENDED, 0.5);
    drain(fd);
    press('P'), event('m'), event('k'), event('u');
    unsigned id = hear_stop(fd);
    CHECK(id != 0, "a stop is sent for the press");
    CHECK(got_is("be") && hv_fence_held() == 4, "the press and what follows are held");
    say(fd, "stopped %u\n", id - 1); /* an earlier stop's answer: from before the fling */
    pause_ms(100);
    CHECK(got_is("be") && hv_fence_held() == 4, "an earlier answer doesn't let them go");
    say(fd, "stopped %u\n", id);
    pause_ms(100);
    CHECK(got_is("bePmku") && !hv_fence_held(), "its answer lets them go, in order");
    press('Q');
    CHECK(got_is("bePmkuQ") && no_stop(fd, 50), "answered since: the next press goes at once");
    got_clear();

    /* A scroll that ended still (no fling) waits too: its own lines may not be posted yet. */
    scroll('e', HV_SCROLL_ENDED, 0);
    drain(fd);
    press('P');
    id = hear_stop(fd);
    CHECK(id && got_is("e"), "after any scroll line, a press waits");
    say(fd, "stopped %u\n", id);
    pause_ms(100);
    CHECK(got_is("eP"), "and goes once answered");
    got_clear();

    /* A stop sent early (a finger landing, a button going down) is the one waited for: no second stop. */
    scroll('e', HV_SCROLL_ENDED, 0.5);
    drain(fd);
    hv_share_touch_stop();
    unsigned early = hear_stop(fd);
    press('P');
    CHECK(early && no_stop(fd, 50), "no second stop");
    CHECK(got_is("e") && hv_fence_held() == 1, "held for the early stop");
    say(fd, "stopped %u\n", early);
    pause_ms(100);
    CHECK(got_is("eP"), "its answer lets it go");
    got_clear();

    /* A fling held behind a press: once it goes, the press after it waits again, for a stop sent after it. */
    scroll('e', HV_SCROLL_ENDED, 0.5);
    drain(fd);
    press('P'), event('u'), scroll('f', HV_SCROLL_ENDED, 0.5), press('Q'), event('v');
    id = hear_stop(fd);
    say(fd, "stopped %u\n", id);
    pause_ms(100);
    CHECK(got_is("ePuf") && hv_fence_held() == 2, "the second press waits for the fling held before it");
    char line[128];
    unsigned second = 0;
    while (!second && hear(fd, line, sizeof line, 300) == 0) sscanf(line, "stop %u", &second); /* after its "s e" */
    CHECK(second && (int)(second - id) > 0, "a new stop, after the fling");
    say(fd, "stopped %u\n", second);
    pause_ms(100);
    CHECK(got_is("ePufQv") && !hv_fence_held(), "then it goes");
    got_clear();

    /* The Mac pausing: what is held goes now. */
    scroll('e', HV_SCROLL_ENDED, 0.5);
    drain(fd);
    press('P'), event('u');
    CHECK(hv_fence_held() == 2, "held");
    hv_fence_flush("test: the Mac pauses");
    CHECK(got_is("ePu") && !hv_fence_held(), "a flush sends them at once");
    id = hear_stop(fd);
    say(fd, "stopped %u\n", id); /* answered late: nothing is waiting, nothing happens */
    pause_ms(100);
    CHECK(got_is("ePu"), "a late answer changes nothing");
    got_clear();

    /* No answer in time: it goes after the limit, and the helper isn't waited on again until it answers. */
    hv_fence_set_limit_ms(200);
    scroll('e', HV_SCROLL_ENDED, 0.5);
    drain(fd);
    press('P');
    id = hear_stop(fd);
    pause_ms(50);
    CHECK(got_is("e"), "still held before the limit");
    pause_ms(150 + 250);
    CHECK(got_is("eP"), "gone after the limit");
    scroll('f', HV_SCROLL_ENDED, 0.5);
    drain(fd);
    press('Q');
    CHECK(got_is("ePfQ"), "a helper that didn't answer isn't waited on");
    say(fd, "stopped %u\n", id); /* it answers again */
    pause_ms(100);
    scroll('g', HV_SCROLL_ENDED, 0.5);
    drain(fd);
    press('R');
    unsigned again = hear_stop(fd);
    CHECK(again && got_is("ePfQg"), "once it answers, it is waited on again");
    say(fd, "stopped %u\n", again);
    pause_ms(100);
    CHECK(got_is("ePfQgR"), "and answered, it goes");
    got_clear();

    /* No answer in time while the Mac stood still (an auto-save's stop): the helper couldn't answer, so the press waits
       on, and the helper is still waited on. Stopped as the press came; or stopped and started during the wait. */
    hv_fence_set_limit_ms(400);
    for (int during = 0; during < 2; during++) {
        mac_stops = during ? 2 : 3;
        scroll('e', HV_SCROLL_ENDED, 0.5);
        drain(fd);
        press('P');
        id = hear_stop(fd);
        pause_ms(50);
        if (during) mac_stops = 4;
        pause_ms(during ? 450 : 850); /* past the first limit (and the second, stopped throughout), short of the next */
        CHECK(got_is("e") && hv_fence_held() == 1, "the Mac stood still: still waiting for its answer");
        mac_stops = 4;
        say(fd, "stopped %u\n", id);
        pause_ms(100);
        CHECK(got_is("eP"), "its answer lets it go");
        press('Q'); /* no scroll since: nothing to wait for */
        CHECK(got_is("ePQ") && no_stop(fd, 50), "then a press goes at once");
        scroll('f', HV_SCROLL_ENDED, 0.5);
        drain(fd);
        press('R');
        unsigned next = hear_stop(fd);
        CHECK(next && got_is("ePQf") && hv_fence_held() == 1, "the helper is still waited on");
        say(fd, "stopped %u\n", next);
        pause_ms(100);
        CHECK(got_is("ePQfR"), "and answered, it goes");
        got_clear();
    }
    hv_fence_set_limit_ms(3000);

    /* The link going while a press waits: it goes. */
    scroll('e', HV_SCROLL_ENDED, 0.5);
    drain(fd);
    press('P'), event('u');
    CHECK(hear_stop(fd) && hv_fence_held() == 2, "held");
    close(fd);
    pause_ms(150);
    CHECK(got_is("ePu") && !hv_fence_held(), "the link closing lets them go");
    got_clear();

    /* A new link replacing one a press waits on: it goes (share.c closes the old link, so nothing more comes on it). */
    fd = open_link();
    scroll('e', HV_SCROLL_ENDED, 0.5);
    drain(fd);
    press('P');
    id = hear_stop(fd);
    CHECK(id && hv_fence_held() == 1, "held");
    int fd2 = open_link();
    pause_ms(100);
    CHECK(got_is("eP"), "a new link lets it go");
    hv_share_touch_late(id); /* as the old hold's time limit would, firing as the new helper linked */
    scroll('f', HV_SCROLL_ENDED, 0.5);
    drain(fd2);
    press('Q');
    unsigned on_new = hear_stop(fd2);
    CHECK(on_new && hv_fence_held() == 1, "the new link is waited on, the old link's stop going unanswered or not");
    say(fd2, "stopped %u\n", on_new);
    pause_ms(100);
    CHECK(got_is("ePfQ"), "and its answer lets it go");
    close(fd);
    fd = fd2;
    got_clear();

    /* A finger landing to catch a glide flung while a click waits: its early stop goes after that fling, not before
       it (it would stop nothing), and the press after it waits for that stop. */
    scroll('e', HV_SCROLL_ENDED, 0.5);
    drain(fd);
    press('P'), scroll('f', HV_SCROLL_ENDED, 0.5), early_stop(), press('Q');
    id = hear_stop(fd);
    CHECK(id && no_stop(fd, 50) && hv_fence_held() == 4, "the fling, the early stop and the press wait behind the click");
    say(fd, "stopped %u\n", id);
    char l1[128] = "", l2[128] = "";
    unsigned early2 = 0;
    CHECK(hear(fd, l1, sizeof l1, 300) == 0 && l1[0] == 's' && hear(fd, l2, sizeof l2, 300) == 0
              && sscanf(l2, "stop %u", &early2) == 1, "the fling, then its stop, in that order");
    CHECK(no_stop(fd, 50), "the press reuses the early stop");
    CHECK(got_is("ePf") && hv_fence_held() == 1, "the press waits for the early stop");
    say(fd, "stopped %u\n", early2);
    pause_ms(100);
    CHECK(got_is("ePfQ"), "which lets it go");
    got_clear();

    /* A flush, then a press at once waiting on the same unanswered stop: the first hold's time limit doesn't end the
       second early. */
    hv_fence_set_limit_ms(1000);
    scroll('e', HV_SCROLL_ENDED, 0.5);
    drain(fd);
    press('P');
    id = hear_stop(fd);
    pause_ms(500);
    hv_fence_flush("test: a pause");
    press('Q'); /* the same stop is waited for: no answer yet */
    CHECK(no_stop(fd, 20) && hv_fence_held() == 1, "the second press waits on the same stop");
    pause_ms(700); /* the first hold's limit has passed, not the second's (about 300 ms on) */
    CHECK(got_is("eP") && hv_fence_held() == 1, "the first hold's limit doesn't end the second");
    say(fd, "stopped %u\n", id);
    pause_ms(100);
    CHECK(got_is("ePQ"), "its answer does");
    got_clear();
    hv_fence_set_limit_ms(3000);

    /* The time limit crossing the answer: answered, the helper isn't marked as not answering. */
    scroll('e', HV_SCROLL_ENDED, 0.5);
    drain(fd);
    press('P');
    id = hear_stop(fd);
    say(fd, "stopped %u\n", id);
    pause_ms(100);
    hv_share_touch_late(id); /* as the timer would, having fired just before the answer was taken */
    scroll('f', HV_SCROLL_ENDED, 0.5);
    drain(fd);
    press('Q');
    again = hear_stop(fd);
    CHECK(again && got_is("ePf") && hv_fence_held() == 1, "still waited on");
    say(fd, "stopped %u\n", again);
    pause_ms(100);
    CHECK(got_is("ePfQ"), "and answered, it goes");
    got_clear();

    /* More held than the fence keeps: rather out of order than lost; nothing is sent twice. */
    scroll('e', HV_SCROLL_ENDED, 0.5);
    drain(fd);
    int before = delivered_now();
    press('P');
    id = hear_stop(fd);
    for (int i = 0; i < 600; i++) event('x');
    CHECK(delivered_now() - before == 601 && !hv_fence_held(), "all of them went, once each");
    say(fd, "stopped %u\n", id);
    pause_ms(100);
    CHECK(delivered_now() - before == 601, "a late answer sends nothing again");
    got_clear();
    close(fd);
    pause_ms(50);

    scratch_done();
    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    printf("input fence: all passed\n");
    return 0;
}
