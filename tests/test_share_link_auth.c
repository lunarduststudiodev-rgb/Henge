/* Which of the helper's system links may change the Mac's ports (host/share.c; gates r1 S8 and S3). Anyone in the Mac
   can open the system link; only root can read the key its request carries. The app keeps that key's SHA-256 per Mac
   and takes "ports" only from a link with it. A new key is taken where this Mac has none, or on the first system link
   after Henge started the Mac afresh, before any user's helper has linked: not after a Suspend, not after a login,
   not on a later link. A link without the key never has its ports taken (one log line a link), and never takes the
   place of one with it. A list stays only while the link it came from, or one with the key that will say its own,
   is in place. Once this Mac has a key, nothing else a link without it says is taken either (its session, touchscreen,
   notes). A pin that can't be saved leaves an unreadable file in its place, so the next launch takes no key until the
   Mac starts afresh (gate r3 S1, S2).

   Each run below is a child process, as each launch of Henge is (one Mac per launch): its own share server, the same
   pin file. Played over share.c's own socket. */
#include "scratch_dir.h"
#include "share.h"

#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

static int failures;
#define CHECK(ok, what)                                                                                                \
    do {                                                                                                               \
        if (!(ok)) {                                                                                                   \
            failures++;                                                                                                \
            printf("FAIL run %d line %d: %s\n", run_no, __LINE__, what);                                               \
        }                                                                                                              \
    } while (0)

static int run_no;
static char dir[PATH_MAX], pin_file[PATH_MAX], sock[200], log_file[PATH_MAX], kept[PATH_MAX + 8];

/* Two keys, as two system parts would have made them; K1 is this Mac's first. */
static const char K1[] = "1111111111111111111111111111111111111111111111111111111111111111";
static const char K2[] = "2222222222222222222222222222222222222222222222222222222222222222";
/* A third whose hash starts and ends as K1's does (found when the test starts): only the whole hash is compared. */
static char K3[65];

static void pause_ms(int ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&t, NULL);
}

/* A link as a helper would open it; key NULL for none. Its fd once upgraded (101), -403 if refused, -1 otherwise.
   requests_sent counts requests written whole, for a test that must know one is on its way. */
static _Atomic int requests_sent;
static int open_link(const char *which, int version, const char *key) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) return -1;
    char req[300], k[100] = "";
    if (key) snprintf(k, sizeof k, "Henge-Key: %s\r\n", key);
    int n = snprintf(req, sizeof req, "GET /.henge/%s?v=%d&trusted=1&boot=12 HTTP/1.1\r\nHost: 10.0.2.4\r\n%s\r\n", which, version, k);
    if (write(fd, req, (size_t)n) != n) return -1;
    requests_sent++;
    char head[256];
    size_t got = 0;
    head[0] = 0;
    while (got < sizeof head - 1 && !strstr(head, "\r\n\r\n")) {
        ssize_t r = read(fd, head + got, 1);
        if (r <= 0) break;
        head[++got] = 0;
    }
    pause_ms(60); /* share.c takes the link in */
    if (strstr(head, " 101 ")) return fd;
    close(fd);
    return strstr(head, " 403 ") ? -403 : -1;
}

static void say(int fd, const char *line) {
    size_t n = strlen(line);
    if (write(fd, line, n) != (ssize_t)n) printf("write failed\n");
    pause_ms(80);
}

static void done_with(int fd) {
    if (fd >= 0) close(fd);
    pause_ms(80);
}

/* What the network was told (share.c's hv_share_ports). A call can be held at the gate: armed (1), the next call
   waits there (2) until it is opened (3), and is counted then. */
static pthread_mutex_t ports_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cv = PTHREAD_COND_INITIALIZER;
static int ports_calls, ports_n = -2, ports_first, gate;
static void took_ports(const uint16_t *port, const uint8_t *udp, int n) {
    (void)udp;
    pthread_mutex_lock(&ports_mu);
    if (gate == 1) {
        gate = 2;
        pthread_cond_broadcast(&gate_cv);
        while (gate != 3) pthread_cond_wait(&gate_cv, &ports_mu);
        gate = 0;
    }
    ports_calls++;
    ports_n = n;
    ports_first = n > 0 ? port[0] : 0;
    pthread_mutex_unlock(&ports_mu);
}
static int calls(void) {
    pthread_mutex_lock(&ports_mu);
    int c = ports_calls;
    pthread_mutex_unlock(&ports_mu);
    return c;
}
/* The last call: n (and its first port), and that there were `c` calls in all. A busy Mac may be slow to make the
   c-th: it is given up to 2 s. */
static int last_was(int c, int n, int first) {
    for (int waited = 0; calls() < c && waited < 2000; waited += 10) pause_ms(10);
    pthread_mutex_lock(&ports_mu);
    int ok = ports_calls == c && ports_n == n && (n <= 0 || ports_first == first);
    pthread_mutex_unlock(&ports_mu);
    return ok;
}

/* How many lines of share.c's log (this run's) have `needle`. */
static int log_count(const char *needle) {
    fflush(stderr);
    FILE *f = fopen(log_file, "r");
    if (!f) return -1;
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof line, f))
        if (strstr(line, needle)) n++;
    fclose(f);
    return n;
}

static void hash_of(const char *hex, uint8_t out[CC_SHA256_DIGEST_LENGTH]) {
    uint8_t key[32];
    for (int i = 0; i < 32; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        key[i] = (uint8_t)v;
    }
    CC_SHA256(key, sizeof key, out);
}
static void tag_of(const char *hex, char out[9]) {
    uint8_t h[CC_SHA256_DIGEST_LENGTH];
    hash_of(hex, h);
    snprintf(out, 9, "%02x%02x%02x%02x", h[0], h[1], h[2], h[3]);
}
/* The pin file holds exactly this key's hash, is 0600, and holds no key. */
static int pinned(const char *hex) {
    struct stat st;
    if (lstat(pin_file, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 0777) != 0600 || st.st_size != CC_SHA256_DIGEST_LENGTH)
        return 0;
    uint8_t want[CC_SHA256_DIGEST_LENGTH], got[CC_SHA256_DIGEST_LENGTH];
    hash_of(hex, want);
    int fd = open(pin_file, O_RDONLY);
    int ok = read(fd, got, sizeof got) == (ssize_t)sizeof got && !memcmp(got, want, sizeof got);
    close(fd);
    return ok;
}

/* One launch of Henge: cold 1 if it starts the Mac afresh. */
static int run(int cold, void (*body)(void)) {
    run_no++;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        snprintf(log_file, sizeof log_file, "%s/log%d", dir, run_no);
        setvbuf(stdout, NULL, _IONBF, 0);
        signal(SIGPIPE, SIG_IGN); /* a link share.c closed: the write fails, as the helper's would */
        freopen(log_file, "w", stderr);
        setvbuf(stderr, NULL, _IONBF, 0);
        char folder[PATH_MAX];
        snprintf(folder, sizeof folder, "%s/f%d", dir, run_no);
        mkdir(folder, 0755);
        snprintf(sock, sizeof sock, "%s/s%d", dir, run_no);
        hv_share_system_pin(pin_file);
        hv_share_ports = took_ports;
        if (cold) hv_share_system_cold_boot();
        if (hv_share_start(folder, sock, NULL) != 0) {
            printf("share didn't start\n");
            _exit(99);
        }
        body();
        if (failures) { /* what share.c said, to see why */
            FILE *f = fopen(log_file, "r");
            char line[1024];
            while (f && fgets(line, sizeof line, f)) printf("  log: %s", line);
            if (f) fclose(f);
        }
        fflush(stdout);
        _exit(failures > 98 ? 98 : failures);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) printf("FAIL run %d: ended by signal %d\n", run_no, WIFSIGNALED(status) ? WTERMSIG(status) : -1);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 99;
}

/* 1. Henge starts this Mac afresh for the first time: no key is pinned yet. */
static void first_run(void) {
    /* A link without a key (a helper not proven root): its ports are never taken, and that's said once for the link. */
    int fd = open_link("system", HV_AGENT_SINCE_PORTS, NULL);
    CHECK(fd >= 0, "a link without a key is taken (session, touchscreen) while no proven one is linked");
    say(fd, "ports tcp 22\n");
    say(fd, "ports tcp 22 8080\n");
    CHECK(calls() == 0, "its ports aren't taken");
    CHECK(log_count("ports aren't taken: its link doesn't have this Mac's key") == 1, "said once for the link, not once a line");
    CHECK(hv_share_session() == HV_SESSION_UNKNOWN, "(no session said yet)");
    say(fd, "session login\n");
    CHECK(hv_share_session() == HV_SESSION_LOGIN, "its session line is taken");
    done_with(fd);
    CHECK(calls() == 0, "nor does its end change anything");

    /* The first link with a key: it is pinned, as its hash only, and its ports are taken. */
    fd = open_link("system", HV_AGENT_VERSION, K1);
    CHECK(fd >= 0, "the system part with its key links");
    CHECK(pinned(K1), "the pin file is the key's SHA-256, 0600, and holds no key");
    char tag[9];
    tag_of(K1, tag);
    char want[64];
    snprintf(want, sizeof want, "this Mac's key (%s...) is kept", tag);
    CHECK(log_count(want) == 1, "the log says it, with 8 hex digits of the hash");
    say(fd, "ports tcp 22 8080\n");
    CHECK(last_was(1, 2, 22), "its ports are taken");

    /* While it's linked: a link without the key is refused at once (said once), and doesn't take its place. */
    int other = open_link("system", HV_AGENT_VERSION, NULL);
    CHECK(other == -403, "a link without the key is refused while the proven one is linked");
    other = open_link("system", HV_AGENT_VERSION, NULL);
    CHECK(other == -403 && log_count("without this Mac's key, while the one with it is linked") == 1, "again: refused, said once");
    other = open_link("system", HV_AGENT_VERSION, K2);
    CHECK(other == -403, "and a link with another key");
    other = open_link("system", HV_AGENT_VERSION, K2);
    CHECK(other == -403 && log_count("a key this Mac's isn't") == 1, "again: refused, said once");
    other = open_link("system", HV_AGENT_VERSION, K3);
    CHECK(other == -403, "and one whose hash starts and ends as this Mac's does");
    say(fd, "ports tcp 22\n");
    CHECK(last_was(2, 1, 22), "the proven link is still the one: its next list is taken");

    /* S3: the system part links again (the same key) before its old link's end is seen; the old link's lines after
       that aren't taken. The new link goes before it says its list: the standard set again. */
    int again = open_link("system", HV_AGENT_VERSION, K1);
    CHECK(again >= 0 && calls() == 2, "linked again with the key: its list stays for now (it says its own at once)");
    /* The replaced link's lines after that aren't taken. Here macOS drops them with the link (share.c shuts it, which
       discards what it hadn't read); share.c also takes a line only while its link is the one, in the same step as
       the list is applied, for a line read whole just before. That moment is too short to bring about from here. */
    if (write(fd, "ports tcp 9999\n", 15) != 15) { /* refused, as it is shut: nothing to say */ }
    pause_ms(80);
    CHECK(calls() == 2, "a replaced link's line isn't taken");
    done_with(fd);
    CHECK(calls() == 2, "nor does the replaced link's end change anything");
    done_with(again);
    CHECK(last_was(3, -1, 0), "the new link went before it said its list: the standard set again");

    /* S3: a link with the key but of a version that never says ports takes the place of one that did: the
       standard set again at once. */
    fd = open_link("system", HV_AGENT_VERSION, K1);
    say(fd, "ports tcp 3000\n");
    CHECK(last_was(4, 1, 3000), "(a list)");
    int older = open_link("system", HV_AGENT_SINCE_PORTS - 1, K1);
    CHECK(older >= 0 && last_was(5, -1, 0), "an older link in its place: the standard set at once");
    done_with(fd);
    done_with(older);
    CHECK(calls() == 5, "and nothing more as they go");

    CHECK(log_count(K1) == 0 && log_count(K2) == 0, "no key is ever in the log");
    /* Of each key's hash, no 9 hex digits in a row anywhere: the 8 said are its first, and nothing else of it. */
    const char *keys[] = {K1, K2, K3};
    int more = 0;
    for (int k = 0; k < 3; k++) {
        uint8_t h[CC_SHA256_DIGEST_LENGTH];
        char full[65], piece[10];
        hash_of(keys[k], h);
        for (int i = 0; i < 32; i++) snprintf(full + 2 * i, 3, "%02x", h[i]);
        for (int at = 0; at + 9 <= 64; at++) {
            memcpy(piece, full + at, 9), piece[9] = 0;
            more += log_count(piece);
        }
    }
    CHECK(more == 0, "nor more than 8 hex digits of a key's hash");
}

/* 2. A Suspended Mac resumes: not a fresh start. Another key, even as the first link with no one logged in, is
   refused, and said once for that key. */
static void resumed(void) {
    int fd = open_link("system", HV_AGENT_VERSION, K2);
    CHECK(fd >= 0, "another key's link is taken as one without the key (no proven one is linked)");
    say(fd, "ports tcp 22\n");
    say(fd, "ports tcp 23\n");
    CHECK(calls() == 0, "its ports aren't taken");
    CHECK(log_count("a key this Mac's isn't") == 1, "refused, and said");
    CHECK(log_count("ports aren't taken: its link doesn't have this Mac's key") == 1, "its ports: said once for the link");
    int again = open_link("system", HV_AGENT_VERSION, K2);
    CHECK(again >= 0 && log_count("a key this Mac's isn't") == 1, "the same key again: not said again");
    done_with(fd);
    CHECK(pinned(K1), "the pin is still this Mac's key");
    fd = open_link("system", HV_AGENT_VERSION, K1);
    CHECK(fd >= 0, "the key's link takes the place of one without it");
    say(fd, "ports tcp 445\n");
    CHECK(last_was(1, 1, 445), "and its ports are taken");
    done_with(again);
    done_with(fd);
}

/* 3. The Mac started afresh, but someone's helper linked before the system part: a new key isn't taken. */
static void cold_after_login(void) {
    int touch = open_link("link", HV_AGENT_VERSION, NULL);
    CHECK(touch >= 0, "a user's helper links first");
    int fd = open_link("system", HV_AGENT_VERSION, K2);
    say(fd, "ports tcp 22\n");
    CHECK(fd >= 0 && calls() == 0 && log_count("a key this Mac's isn't") == 1, "another key after it: refused");
    CHECK(pinned(K1), "the pin stays");
    done_with(fd);
    done_with(touch);
}

/* 4. macOS reinstalled on this Mac: its system part made a new key. Henge starts it afresh and the system part links
   first (before the login window, as a LaunchDaemon does): the new key is pinned, and forwarding works with nothing
   to do. A link after it with the old key is then refused. */
static void reinstalled(void) {
    int fd = open_link("system", HV_AGENT_VERSION, K2);
    CHECK(fd >= 0, "the new key's link");
    CHECK(pinned(K2), "is pinned in place of the old one");
    CHECK(log_count("has a new key") == 1, "and the log says so");
    say(fd, "ports tcp 22 5000\n");
    CHECK(last_was(1, 2, 22), "its ports are taken");
    done_with(fd);
    fd = open_link("system", HV_AGENT_VERSION, K1);
    CHECK(fd >= 0 && log_count("a key this Mac's isn't") == 1, "not the first link: the old key is refused");
    say(fd, "ports tcp 1\n");
    CHECK(calls() == 2 && pinned(K2), "its ports aren't taken, the pin stays");
    done_with(fd);
}

/* 5. Henge launched again while the Mac it had runs on (no fresh start, as the MacBook's --restore): the first link
   with the old key is refused. */
static void reconnected(void) {
    int fd = open_link("system", HV_AGENT_VERSION, K1);
    say(fd, "ports tcp 22\n");
    CHECK(fd >= 0 && calls() == 0 && log_count("a key this Mac's isn't") == 1, "the old key is refused, said once");
    CHECK(pinned(K2), "the pin stays");
    done_with(fd);
}

/* 6. S3: the link is replaced, by a keyed one that never says ports, while its list is being applied. The list
   mustn't outlast it: the standard set is the network's last word. */
static int older_fd;
static void *open_older(void *arg) {
    (void)arg;
    older_fd = open_link("system", HV_AGENT_SINCE_PORTS - 1, K2);
    return NULL;
}
static void replaced_while_applying(void) {
    int fd = open_link("system", HV_AGENT_VERSION, K2);
    CHECK(fd >= 0, "linked with this Mac's key");
    pthread_mutex_lock(&ports_mu);
    gate = 1;
    pthread_mutex_unlock(&ports_mu);
    if (write(fd, "ports tcp 22\n", 13) != 13) printf("write failed\n");
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 5;
    pthread_mutex_lock(&ports_mu);
    while (gate == 1 && pthread_cond_timedwait(&gate_cv, &ports_mu, &until) == 0) {}
    int held = gate == 2;
    pthread_mutex_unlock(&ports_mu);
    CHECK(held, "its list is on its way to the network");
    int sent = requests_sent;
    older_fd = -2;
    pthread_t t;
    pthread_create(&t, NULL, open_older, NULL);
    for (int waited = 0; requests_sent == sent && waited < 5000; waited += 10) pause_ms(10);
    pause_ms(300); /* share.c reads it and waits for the list's end */
    CHECK(requests_sent > sent && older_fd == -2, "the older link came in while the list was on its way, and waits for it");
    pthread_mutex_lock(&ports_mu);
    gate = 3;
    pthread_cond_broadcast(&gate_cv);
    pthread_mutex_unlock(&ports_mu);
    pthread_join(t, NULL);
    pause_ms(80);
    CHECK(older_fd >= 0, "the older link takes its place");
    CHECK(last_was(2, -1, 0), "the list, then the standard set: never the list after it");
    done_with(fd);
    done_with(older_fd);
    CHECK(calls() == 2, "and nothing more as they go");
}

/* 7. Started afresh, but a link without a key came first: the moment for a new key has gone. */
static void cold_after_unkeyed(void) {
    int none = open_link("system", HV_AGENT_VERSION, NULL);
    CHECK(none >= 0, "a link without a key comes first");
    done_with(none);
    int fd = open_link("system", HV_AGENT_VERSION, K1);
    say(fd, "ports tcp 22\n");
    CHECK(fd >= 0 && calls() == 0 && log_count("a key this Mac's isn't") == 1, "a new key after it: refused");
    CHECK(pinned(K2), "the pin stays");
    done_with(fd);
}

/* 8, 9. The pin file is there but can't be read (cut short; a link in its place): this Mac is pinned to a key no link
   has, not to the last Mac's. On a resume even the key it held is refused, and the file is left as it is. */
static const char cut[] = "sixteen bytes...";
static int pin_is_cut(void) {
    struct stat st;
    char got[sizeof cut];
    int fd = open(pin_file, O_RDONLY | O_NOFOLLOW);
    int ok = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == (off_t)sizeof cut - 1 &&
             read(fd, got, sizeof got) == (ssize_t)sizeof cut - 1 && !memcmp(got, cut, sizeof cut - 1);
    if (fd >= 0) close(fd);
    return ok;
}
static void pin_unreadable(int is_link) {
    /* The iPad app runs one Mac after another in a launch: the one before it had K2's pin, read whole. */
    hv_share_system_pin(kept);
    hv_share_system_pin(pin_file);
    CHECK(log_count("this Mac's pinned key") == 2 && log_count("can't be read") == 2, "the log says the pin can't be read, each time");
    int fd = open_link("system", HV_AGENT_VERSION, K2);
    say(fd, "ports tcp 22\n");
    CHECK(fd >= 0 && calls() == 0, "even the key it held: its ports aren't taken");
    CHECK(log_count("a key this Mac's isn't") == 1 && log_count("this Mac's is unreadable)") == 1, "refused, and said why");
    done_with(fd);
    struct stat st;
    CHECK(is_link ? lstat(pin_file, &st) == 0 && S_ISLNK(st.st_mode) : pin_is_cut(), "the pin file is left as it is");
}
static void pin_cut(void) { pin_unreadable(0); }
static void pin_linked(void) { pin_unreadable(1); }

/* 10. Then the Mac starts afresh: its system part's first link is pinned, and forwarding works again. */
static void unreadable_then_cold(void) {
    int fd = open_link("system", HV_AGENT_VERSION, K2);
    CHECK(fd >= 0 && pinned(K2), "the key is pinned in a plain file in place of the link");
    CHECK(log_count("has a new key") == 1 && log_count("was unreadable)") == 1, "the log says the pin it replaced couldn't be read");
    say(fd, "ports tcp 22\n");
    CHECK(last_was(1, 1, 22), "its ports are taken");
    done_with(fd);
}

/* 11. A Mac with no pin yet (Henge from before keys): the first key's link is pinned, even on a resume. */
static void no_pin_resumed(void) {
    int fd = open_link("system", HV_AGENT_VERSION, K1);
    CHECK(fd >= 0 && pinned(K1), "the key is pinned");
    say(fd, "ports tcp 22\n");
    CHECK(last_was(1, 1, 22), "its ports are taken");
    done_with(fd);
}

/* Waits (up to 5 s) until share.c's log has n lines with `needle`. */
static int wait_log(const char *needle, int n) {
    for (int waited = 0; log_count(needle) < n && waited < 5000; waited += 10) pause_ms(10);
    return log_count(needle) == n;
}

/* 12. Gate r3 S2: once this Mac has a key, a link without it (here about 15 s after each resume, before the system
   part links again; or always, on a Mac whose key file is refused) has none of its lines taken: not the session
   (which drives the iPad's Log In button), the touchscreen or its notes, as not its ports. Said once for the link. */
static void untrusted_lines(void) {
    char lots[64 * 40 + 200];
    size_t n = 0;
    n += (size_t)snprintf(lots + n, sizeof lots - n, "session user\ntouchscreen replug\n");
    for (int i = 0; i < 64; i++) n += (size_t)snprintf(lots + n, sizeof lots - n, "note spoofed %d\n", i);
    n += (size_t)snprintf(lots + n, sizeof lots - n, "ports tcp 22\n");
    const char *keys[] = {K2, NULL};
    for (int k = 0; k < 2; k++) {
        int fd = open_link("system", HV_AGENT_VERSION, keys[k]);
        CHECK(fd >= 0, k ? "a link without a key (none with it is linked)" : "a link with another key (none with it is linked)");
        /* While it is the link in place (as the iPad's Log In button reads it). */
        static const char first[] = "session login\ntouchscreen ready\n";
        if (write(fd, first, sizeof first - 1) != (ssize_t)sizeof first - 1) printf("write failed\n");
        CHECK(wait_log("lines aren't taken: its link doesn't have this Mac's key", k + 1), "said once for the link");
        pause_ms(200);
        CHECK(hv_share_session() == HV_SESSION_UNKNOWN, "its session isn't taken");
        CHECK(hv_share_touchscreen_ready() == -1, "nor its touchscreen");
        if (write(fd, lots, n) != (ssize_t)n) printf("write failed\n");
        close(fd); /* share.c reads every line before it sees the end */
        CHECK(wait_log("system part's link closed", k + 1), "(all its lines read)");
        CHECK(hv_share_touchscreen_take_replug() == 0, "nor its replug");
        CHECK(log_count("spoofed") == 0 && log_count("the Mac is at") == 0 && log_count("macOS takes a touchscreen") == 0 &&
                  log_count("plugged in again") == 0, "nor its notes");
        CHECK(log_count("lines aren't taken: its link doesn't have this Mac's key") == k + 1, "and only once");
        CHECK(calls() == 0, "nor its ports");
    }
    /* With the key: all of it is taken. */
    int fd = open_link("system", HV_AGENT_VERSION, K1);
    CHECK(fd >= 0, "this Mac's key");
    say(fd, "session login\n");
    say(fd, "touchscreen ready\n");
    say(fd, "note proven\n");
    say(fd, "ports tcp 22\n");
    CHECK(hv_share_session() == HV_SESSION_LOGIN, "its session is taken");
    CHECK(hv_share_touchscreen_ready() == 1, "and its touchscreen");
    CHECK(log_count("system part: proven") == 1, "and its note");
    CHECK(last_was(1, 1, 22), "and its ports");
    done_with(fd);
}

/* 13. Gate r3 S1: the first key's pin can't be saved (its file can't be made: a full disk, say). The key is trusted
   for this launch, and an empty file is left where the pin goes. */
static void save_fails(void) {
    int fd = open_link("system", HV_AGENT_VERSION, K1);
    CHECK(fd >= 0, "the first key's link");
    CHECK(log_count("key couldn't be kept") == 1 && log_count("no key is taken until the Mac starts afresh") == 1, "the log says so");
    say(fd, "ports tcp 22\n");
    CHECK(last_was(1, 1, 22), "it is trusted until Henge stops");
    done_with(fd);
    struct stat st;
    CHECK(lstat(pin_file, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0, "an empty file in the pin's place");
}

/* 14. The next launch, a resume: that file can't be read, so not even the first key sent (another) is taken. */
static void after_failed_save(void) {
    int fd = open_link("system", HV_AGENT_VERSION, K2);
    say(fd, "ports tcp 22\n");
    CHECK(fd >= 0 && calls() == 0, "another key on a resume: its ports aren't taken");
    CHECK(log_count("a key this Mac's isn't") == 1 && log_count("this Mac's is unreadable)") == 1, "refused, and said why");
    struct stat st;
    CHECK(lstat(pin_file, &st) == 0 && st.st_size == 0, "and it isn't pinned");
    done_with(fd);
}

/* 15. Then the Mac starts afresh: its system part's key is pinned. */
static void failed_save_then_cold(void) {
    int fd = open_link("system", HV_AGENT_VERSION, K1);
    CHECK(fd >= 0 && pinned(K1), "the key is pinned");
    say(fd, "ports tcp 22\n");
    CHECK(last_was(1, 1, 22), "its ports are taken");
    done_with(fd);
}

/* 16. A new key (macOS reinstalled) on a fresh start whose pin can't be saved: the old pin is emptied too (left, the
   next launch would take the key just replaced), so no key is taken until the next fresh start. */
static void repin_save_fails(void) {
    int fd = open_link("system", HV_AGENT_VERSION, K2);
    CHECK(fd >= 0 && log_count("has a new key") == 1, "the new key's link");
    CHECK(log_count("key couldn't be kept") == 1 && log_count("no key is taken until the Mac starts afresh") == 1, "the log says so");
    struct stat st;
    CHECK(lstat(pin_file, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0, "the old pin is emptied");
    done_with(fd);
}

int main(int argc, char **argv) {
    snprintf(dir, sizeof dir, "%s", scratch_dir(argc, argv, "test_share_link_auth"));
    snprintf(pin_file, sizeof pin_file, "%s/pin", dir);
    uint8_t h1[CC_SHA256_DIGEST_LENGTH], h3[CC_SHA256_DIGEST_LENGTH];
    hash_of(K1, h1);
    for (unsigned i = 0;; i++) {
        snprintf(K3, sizeof K3, "%08x33333333333333333333333333333333333333333333333333333333", i);
        hash_of(K3, h3);
        if (h3[0] == h1[0] && h3[31] == h1[31] && memcmp(h3, h1, sizeof h1)) break;
    }
    int bad = 0;
    bad += run(1, first_run);
    bad += run(0, resumed);
    bad += run(1, cold_after_login);
    bad += run(1, reinstalled);
    bad += run(0, reconnected);
    bad += run(0, replaced_while_applying);
    bad += run(1, cold_after_unkeyed);
    snprintf(kept, sizeof kept, "%s.kept", pin_file);
    rename(pin_file, kept); /* K2's hash, for the link below */
    int fd = open(pin_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, cut, sizeof cut - 1) != (ssize_t)sizeof cut - 1) printf("FAIL: couldn't cut the pin short\n"), bad++;
    if (fd >= 0) close(fd);
    bad += run(0, pin_cut);
    unlink(pin_file);
    if (symlink(kept, pin_file) != 0) printf("FAIL: couldn't link the pin\n"), bad++;
    bad += run(0, pin_linked);
    bad += run(1, unreadable_then_cold);
    unlink(pin_file);
    bad += run(0, no_pin_resumed);
    bad += run(0, untrusted_lines);
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof tmp, "%s.new", pin_file);
    unlink(pin_file);
    if (mkdir(tmp, 0755) != 0) printf("FAIL: couldn't put a folder where the pin is written first\n"), bad++;
    bad += run(0, save_fails);
    rmdir(tmp);
    bad += run(0, after_failed_save);
    bad += run(1, failed_save_then_cold);
    if (mkdir(tmp, 0755) != 0) printf("FAIL: couldn't put a folder where the pin is written first\n"), bad++;
    bad += run(1, repin_save_fails);
    rmdir(tmp);
    scratch_done();
    if (bad) {
        printf("share link auth: %d failed\n", bad);
        return 1;
    }
    printf("share link auth: all passed\n");
    return 0;
}
