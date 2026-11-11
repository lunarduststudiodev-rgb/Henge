/* What Henge uses of a helper older than its own (host/share.c, HV_AGENT_SINCE_*): an out-of-date helper is told to
   update, and until it has, what it already does is used, not nothing. Also the system part's touch trace, asked for
   only with HENGE_TOUCH_TRACE, and which link the benchmark's lines come from. Plays the helper over share.c's own
   socket. */
#include "scratch_dir.h"
#include "share.h"

#include <poll.h>
#include <pthread.h>
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

/* A link as a helper of this version would open: "link" (the touch link) or "system". */
/* The system part's key, once it proves it is root (share.c pins the first; test_share_link_auth tests the rest). */
static const char *system_key;

static int open_as(const char *which, int version, int trusted) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) return -1;
    char req[300];
    int keyed = system_key && !strcmp(which, "system");
    int n = snprintf(req, sizeof req, "GET /.henge/%s?v=%d&trusted=%d HTTP/1.1\r\nHost: 10.0.2.4\r\n%s%s%s\r\n", which, version,
                     trusted, keyed ? "Henge-Key: " : "", keyed ? system_key : "", keyed ? "\r\n" : "");
    if (write(fd, req, (size_t)n) != n) return -1;
    char head[256];
    size_t got = 0;
    head[0] = 0;
    while (got < sizeof head - 1 && !strstr(head, "\r\n\r\n")) { /* the 101 */
        ssize_t r = read(fd, head + got, 1);
        if (r <= 0) return -1;
        head[++got] = 0;
    }
    pause_ms(50); /* share.c takes the link in */
    return strstr(head, " 101 ") ? fd : -1;
}

static void say(int fd, const char *line) {
    size_t n = strlen(line);
    if (write(fd, line, n) != (ssize_t)n) fprintf(stderr, "write failed\n");
}

/* One line the device sent within ms, or -1 if none came. */
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

/* What the benchmark's listener heard: the last line, the link it came on, and how many. */
static pthread_mutex_t bench_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned bench_link, bench_count;
static char bench_line[128];
static void bench_heard(unsigned link, const char *l) {
    pthread_mutex_lock(&bench_mu);
    bench_link = link, bench_count++;
    snprintf(bench_line, sizeof bench_line, "%s", l);
    pthread_mutex_unlock(&bench_mu);
}
/* Whether the listener heard exactly `line` on `link` last. */
static int bench_was(unsigned link, const char *line) {
    pthread_mutex_lock(&bench_mu);
    int ok = bench_count && bench_link == link && !strcmp(bench_line, line);
    pthread_mutex_unlock(&bench_mu);
    return ok;
}

static void *sync_now(void *out) {
    *(int *)out = hv_share_guest_sync(1500);
    return NULL;
}

/* A pause's sync, with fd answering as the helper does; -2 if it was never asked. */
static int sync_with(int fd) {
    int result = -3;
    pthread_t t;
    pthread_create(&t, NULL, sync_now, &result);
    char line[64];
    unsigned id = 0;
    if (hear(fd, line, sizeof line, 300) == 0 && sscanf(line, "sync %u", &id) == 1) {
        char answer[64];
        snprintf(answer, sizeof answer, "synced %u 2 0\n", id);
        say(fd, answer);
    } else {
        pthread_join(t, NULL);
        return result == -1 ? -2 : result;
    }
    pthread_join(t, NULL);
    return result;
}

/* What the system part said it listens on (share.c's hv_share_ports), as the network would take it. */
static pthread_mutex_t ports_mu = PTHREAD_MUTEX_INITIALIZER;
static int ports_calls, ports_n = -2;
static uint16_t ports_port[HV_SHARE_MAX_PORTS];
static uint8_t ports_udp[HV_SHARE_MAX_PORTS];
static void took_ports(const uint16_t *port, const uint8_t *udp, int n) {
    pthread_mutex_lock(&ports_mu);
    ports_calls++;
    ports_n = n;
    for (int i = 0; i < n; i++) ports_port[i] = port[i], ports_udp[i] = udp[i];
    pthread_mutex_unlock(&ports_mu);
}

int main(int argc, char **argv) {
    const char *dir = scratch_dir(argc, argv, "test_share_versions");
    snprintf(sock, sizeof sock, "%s/s", dir);
    unsetenv("HENGE_TOUCH_TRACE");
    if (hv_share_start(dir, sock, NULL) != 0) {
        fprintf(stderr, "share didn't start\n");
        return 2;
    }
    CHECK(hv_share_agent_version() == 0 && !hv_share_agent_current(), "none linked");
    char line[128];

    /* This build's helper: current, and everything. */
    int fd = open_as("link", HV_AGENT_VERSION, 1);
    CHECK(fd >= 0, "the link opens");
    CHECK(hv_share_agent_current() && hv_share_agent_version() == HV_AGENT_VERSION, "current");
    CHECK(hv_share_touch_ready(), "touch, allowed");
    close(fd);
    pause_ms(50);
    CHECK(hv_share_agent_version() == 0, "closed: none linked");

    /* Shut Down as the Apple menu does it: only a helper that can is asked (the power button is pressed otherwise). */
    CHECK(hv_share_power(0) == 0, "no helper: not asked");
    fd = open_as("link", HV_AGENT_SINCE_POWER - 1, 1);
    CHECK(hv_share_power(0) == 0 && hear(fd, line, sizeof line, 200) < 0, "a helper older than power: not asked");
    close(fd);
    pause_ms(50);
    fd = open_as("link", HV_AGENT_SINCE_POWER, 1);
    CHECK(hv_share_power(1) != 0 && hear(fd, line, sizeof line, 300) == 0 && !strncmp(line, "power restart ", 14), "asked");
    close(fd);
    pause_ms(50);

    /* One version behind: out of date, yet touch, menus, the pause's sync, the screen and the focus all still work. */
    fd = open_as("link", HV_AGENT_VERSION - 1, 1);
    CHECK(!hv_share_agent_current() && hv_share_agent_version() == HV_AGENT_VERSION - 1, "out of date, its version known");
    CHECK(hv_share_touch_ready(), "touch still works");
    CHECK(sync_with(fd) == 0, "the pause's sync still works");
    unsigned menu = hv_share_menu_ask();
    CHECK(menu != 0 && hear(fd, line, sizeof line, 300) == 0 && !strncmp(line, "menu ", 5), "menus are asked");
    say(fd, "screen away 1\n");
    say(fd, "field 1 3 text prose\n");
    pause_ms(100);
    CHECK(hv_share_screen_away() == 1, "the screen's state is heard");
    HvShareField f;
    CHECK(hv_share_field(&f) && f.state == HV_FOCUS_TEXT, "the focus is heard");
    /* Asked what has the focus before the password's Return only if it answers "look". */
    CHECK((hv_share_look() != 0) == (HV_AGENT_VERSION - 1 >= HV_AGENT_SINCE_LOOK), "a look only of helpers that answer it");
    close(fd);
    pause_ms(50);

    /* Older than the focus lines: touch and the sync, no focus, and a hit's tap answer isn't waited for. */
    fd = open_as("link", HV_AGENT_SINCE_FIELD - 1, 1);
    CHECK(hv_share_touch_ready(), "touch works");
    CHECK(hv_share_tap(0.5, 0.5) == 0, "no tap asked of a helper that wouldn't answer");
    unsigned hit = hv_share_touch_hit(0.5, 0.5);
    CHECK(hit && hear(fd, line, sizeof line, 300) == 0 && !strncmp(line, "hit ", 4), "a hit is asked");
    CHECK(hv_share_tap_answer(hit, NULL, NULL, 0) == HV_TAP_UNKNOWN, "what it lands in: unknown, not waited for");
    say(fd, "field 1 3 text prose\n");
    pause_ms(100);
    CHECK(!hv_share_field(&f) && f.state == HV_FOCUS_UNKNOWN, "its focus isn't used");
    CHECK(hv_share_focus() == HV_FOCUS_UNKNOWN, "nor the old way");
    close(fd);
    pause_ms(50);

    /* Before screen away: what it says of the screen isn't taken. */
    fd = open_as("link", HV_AGENT_SINCE_SCREEN_AWAY - 1, 1);
    say(fd, "screen away 1\n");
    pause_ms(100);
    CHECK(hv_share_screen_away() == -1, "screen away: not known");
    close(fd);
    pause_ms(50);

    /* Too old for touch: not ready even when allowed, and its "trust 1" doesn't make it so; no menus. The sync works. */
    fd = open_as("link", HV_AGENT_SINCE_MENUS - 1, 1);
    CHECK(!hv_share_touch_ready(), "too old for touch");
    say(fd, "trust 1\n");
    pause_ms(100);
    CHECK(!hv_share_touch_ready(), "still too old after trust 1");
    CHECK(hv_share_menu_ask() == 0, "no menus asked");
    CHECK(sync_with(fd) == 0, "the sync works");
    CHECK(hv_share_agent_version() >= HV_AGENT_SINCE_DRIVES, "it can eject");
    close(fd);
    pause_ms(50);

    /* Older than the sync: not asked, and the pause goes on without it. */
    fd = open_as("link", HV_AGENT_SINCE_SYNC - 1, 1);
    CHECK(sync_with(fd) == -2, "no sync asked");
    CHECK(hv_share_agent_version() < HV_AGENT_SINCE_DRIVES, "it can't eject");
    close(fd);
    pause_ms(50);

    /* The system part: "touchscreen ready" counts only from one that tags the touchscreen. */
    fd = open_as("system", HV_AGENT_SINCE_TOUCHSCREEN - 1, 0);
    say(fd, "touchscreen ready\n");
    pause_ms(100);
    CHECK(hv_share_touchscreen_ready() == -1, "untagged: not ready");
    close(fd);
    pause_ms(50);
    fd = open_as("system", HV_AGENT_SINCE_TOUCHSCREEN, 0);
    say(fd, "touchscreen ready\n");
    pause_ms(100);
    CHECK(hv_share_touchscreen_ready() == 1, "tagged: ready");
    CHECK(hear(fd, line, sizeof line, 200) != 0, "no trace asked without HENGE_TOUCH_TRACE");
    close(fd);
    pause_ms(50);

    /* The touch trace: asked only with HENGE_TOUCH_TRACE, and only of a system part that waits to be asked. */
    setenv("HENGE_TOUCH_TRACE", "1", 1);
    fd = open_as("system", HV_AGENT_VERSION, 0);
    CHECK(hear(fd, line, sizeof line, 300) == 0 && !strcmp(line, "trace"), "trace asked");
    close(fd);
    pause_ms(50);
    fd = open_as("system", HV_AGENT_SINCE_TRACE - 1, 0);
    CHECK(hear(fd, line, sizeof line, 200) != 0, "an older system part isn't asked (it records anyway)");
    close(fd);
    pause_ms(50);
    setenv("HENGE_TOUCH_TRACE", "0", 1);
    fd = open_as("system", HV_AGENT_VERSION, 0);
    CHECK(hear(fd, line, sizeof line, 200) != 0, "HENGE_TOUCH_TRACE=0: not asked");
    close(fd);
    pause_ms(50);

    /* The Mac's ports: taken from a system part that says them, in order, a bad number skipped; a long line whole;
       the standard set again (-1) once its link closes. An older one's line, if it sent one, isn't taken. */
    hv_share_ports = took_ports;
    system_key = "1111111111111111111111111111111111111111111111111111111111111111";
    fd = open_as("system", HV_AGENT_SINCE_PORTS - 1, 0);
    say(fd, "ports tcp 22\n");
    pause_ms(100);
    CHECK(ports_calls == 0, "an older system part's ports aren't taken");
    close(fd);
    pause_ms(50);
    CHECK(ports_calls == 0, "nor is its link's end");
    fd = open_as("system", HV_AGENT_SINCE_PORTS, 0);
    say(fd, "ports tcp 22 445 0 70000 655361 3000 udp 88\n");
    pause_ms(100);
    pthread_mutex_lock(&ports_mu);
    CHECK(ports_calls == 1 && ports_n == 4, "the ports are taken, 0, 70000 and 655361 skipped (not read as 65536 and 1)");
    CHECK(ports_port[0] == 22 && !ports_udp[0] && ports_port[1] == 445 && ports_port[2] == 3000 && !ports_udp[2] &&
              ports_port[3] == 88 && ports_udp[3] == 1,
          "each with its protocol");
    pthread_mutex_unlock(&ports_mu);
    char big[4096];
    size_t bl = (size_t)snprintf(big, sizeof big, "ports tcp");
    for (int p = 10000; p < 10500; p++) bl += (size_t)snprintf(big + bl, sizeof big - bl, " %d", p);
    snprintf(big + bl, sizeof big - bl, "\n");
    say(fd, big);
    pause_ms(100);
    pthread_mutex_lock(&ports_mu);
    CHECK(ports_calls == 2 && ports_n == 500 && ports_port[499] == 10499, "a 500-port line, whole");
    pthread_mutex_unlock(&ports_mu);
    say(fd, "ports\n");
    pause_ms(100);
    pthread_mutex_lock(&ports_mu);
    CHECK(ports_calls == 3 && ports_n == 0, "then none");
    pthread_mutex_unlock(&ports_mu);
    close(fd);
    pause_ms(50);
    pthread_mutex_lock(&ports_mu);
    CHECK(ports_calls == 4 && ports_n == -1, "its link closed: the standard set again");
    pthread_mutex_unlock(&ports_mu);
    hv_share_ports = NULL;

    /* A helper older than the numbered stop: sent the plain "stop", and never waited on (it wouldn't answer). */
    fd = open_as("link", HV_AGENT_SINCE_STOPPED - 1, 1);
    hv_share_touch_scroll(HV_SCROLL_ENDED, 0.5, 0.5, 0, 0.5);
    CHECK(hear(fd, line, sizeof line, 300) == 0, "(the fling)");
    hv_share_touch_stop();
    CHECK(hear(fd, line, sizeof line, 300) == 0 && !strcmp(line, "stop"), "the plain stop");
    CHECK(hv_share_glide_fence() == 0, "not waited on");
    close(fd);
    pause_ms(50);

    /* The benchmark: asked only of a helper that can, each link with its own number, and every way a link goes tells
       it, with that link's number, so a run hears only the link it was asked on. */
    hv_share_set_bench_listener(bench_heard);
    fd = open_as("link", HV_AGENT_SINCE_BENCH - 1, 1);
    CHECK(hv_share_bench("idle") == 0 && hear(fd, line, sizeof line, 200) < 0, "a helper older than bench: not asked");
    close(fd);
    pause_ms(50);
    CHECK(bench_count == 1 && bench_link != 0 && !strcmp(bench_line, "failed gone"), "a link closing tells it, by its number");
    fd = open_as("link", HV_AGENT_VERSION, 1);
    unsigned first = hv_share_bench("idle,cpu");
    CHECK(first != 0 && hear(fd, line, sizeof line, 300) == 0 && !strcmp(line, "bench idle,cpu"), "asked");
    CHECK(hv_share_bench("a b") == 0, "steps with a space: not sent");
    say(fd, "bench begin idle 0\n");
    pause_ms(100);
    CHECK(bench_was(first, "begin idle 0"), "its lines, with its link");
    int newer = open_as("link", HV_AGENT_VERSION, 1); /* a newer helper replaces it */
    CHECK(bench_was(first, "failed gone"), "replaced: gone, the old link's number");
    unsigned second = hv_share_bench("idle");
    CHECK(second != 0 && second != first && hear(newer, line, sizeof line, 300) == 0, "the new link: another number");
    say(newer, "bench note hi\n");
    pause_ms(100);
    CHECK(bench_was(second, "note hi"), "the new link's lines, with its number");
    close(fd);
    close(newer);
    pause_ms(50);
    CHECK(bench_was(second, "failed gone"), "closed: gone, with its number");
    /* Closed as something is sent: the send fails, or the link's reader sees it first; either way it hears once. */
    fd = open_as("link", HV_AGENT_VERSION, 1);
    unsigned third = hv_share_bench("idle");
    CHECK(third != 0 && hear(fd, line, sizeof line, 300) == 0, "asked again");
    unsigned before = bench_count;
    close(fd);
    hv_share_touch_stop();
    hv_share_touch_stop();
    pause_ms(100);
    CHECK(!hv_share_agent_version(), "(the link closed)");
    CHECK(bench_was(third, "failed gone") && bench_count == before + 1, "closed while sending: gone once, with its number");

    /* A helper slow for a while, as the guest is in a code-cache flush or under load: 240 scroll lines (two seconds
       of a finger at 120 a second, more than the 8 KB a socket holds by default) sent while it doesn't read, and
       then two seconds more. The link stays up and every line arrives, in order (share.c's LINK_SEND_BUFFER). */
    fd = open_as("link", HV_AGENT_VERSION, 1);
    unsigned slow = hv_share_bench("idle");
    CHECK(slow != 0 && hear(fd, line, sizeof line, 300) == 0, "a slow helper's link");
    for (int i = 0; i < 240; i++) {
        hv_share_touch_scroll(i == 0 ? HV_SCROLL_BEGAN : i == 239 ? HV_SCROLL_ENDED : HV_SCROLL_CHANGED, 0.5, 0.5, 0, i);
        pause_ms(8);
    }
    pause_ms(2000);
    CHECK(hv_share_agent_version() == HV_AGENT_VERSION, "slow: the link is still up");
    int in_order = 0;
    for (int i = 0; i < 240; i++) {
        char want[128];
        snprintf(want, sizeof want, "s %c 0.50000 0.50000 0.000000 %d.000000", i == 0 ? 'b' : i == 239 ? 'e' : 'c', i);
        if (hear(fd, line, sizeof line, 1000) != 0 || strcmp(line, want)) {
            fprintf(stderr, "line %d: heard \"%s\", wanted \"%s\"\n", i, line, want);
            break;
        }
        in_order++;
    }
    CHECK(in_order == 240, "slow: every scroll line, in order");
    CHECK(!(bench_count && bench_link == slow && !strcmp(bench_line, "failed gone")), "slow: not gone");
    close(fd);
    pause_ms(50);
    CHECK(bench_was(slow, "failed gone"), "(closed)");

    /* A helper that stops reading: its socket fills, and a send must fail at once and end the link, not wait under
       link_mu for the helper to read (on a Unix socket MSG_DONTWAIT alone waited: share.c's link_nonblocking). The
       alarm turns a send that waits into a failure here rather than a hang. */
    fd = open_as("link", HV_AGENT_VERSION, 1);
    unsigned full = hv_share_bench("idle");
    CHECK(full != 0 && hear(fd, line, sizeof line, 300) == 0, "asked once more");
    alarm(30);
    int sent = 0;
    while (hv_share_agent_version() && sent < 1000000) hv_share_touch_stop(), sent++; /* never read */
    alarm(0);
    CHECK(!hv_share_agent_version() && sent > 16000, "full: the send failed, the link ended (once 256 KB of stops waited)");
    CHECK(bench_was(full, "failed gone"), "full: gone, with its number");
    close(fd);
    pause_ms(50);
    /* and Henge goes on: link_mu was let go, a new link is taken and heard */
    fd = open_as("link", HV_AGENT_VERSION, 1);
    unsigned after = hv_share_bench("idle");
    CHECK(after != 0 && after != full && hear(fd, line, sizeof line, 300) == 0 && !strcmp(line, "bench idle"), "a new link afterwards");
    say(fd, "bench note again\n");
    pause_ms(100);
    CHECK(bench_was(after, "note again"), "its reader hears it");
    close(fd);
    pause_ms(50);

    scratch_done();
    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    printf("share versions: all passed\n");
    return 0;
}
