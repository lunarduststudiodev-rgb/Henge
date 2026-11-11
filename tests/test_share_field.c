/* The helper's focus and tap lines as the device hears them (host/share.c, helper version 28): one numbered
   "field" line for each change, "ctx" tagged with the field it is for, "tap" answers, and the listener that pushes
   each to the iPad, and "look" (helper 34) answered by "looked". Plays the helper over share.c's own socket. */
#include "scratch_dir.h"
#include "share.h"

#include <pthread.h>
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

static _Atomic unsigned told;
static void listener(void) { told++; }

static void pause_ms(int ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&t, NULL);
}

/* Waits up to a second for the listener to have been called more than `since` times. */
static int heard_after(unsigned since) {
    for (int i = 0; i < 200; i++) {
        if (told > since) return 1;
        pause_ms(5);
    }
    return 0;
}

static int link_open(const char *sock) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) return -1;
    char req[160];
    int n = snprintf(req, sizeof req, "GET /.henge/link?v=%d&trusted=1 HTTP/1.1\r\nHost: 10.0.2.4\r\n\r\n", HV_AGENT_VERSION);
    if (write(fd, req, (size_t)n) != n) return -1;
    char head[256];
    size_t got = 0;
    while (got < sizeof head - 1 && !strstr(head, "\r\n\r\n")) { /* the 101 */
        ssize_t r = read(fd, head + got, 1);
        if (r <= 0) return -1;
        head[++got] = 0;
    }
    return strstr(head, " 101 ") ? fd : -1;
}

static void say(int fd, const char *line) {
    size_t n = strlen(line);
    if (write(fd, line, n) != (ssize_t)n) fprintf(stderr, "write failed\n");
}

/* Reads one line the device sent. */
static int hear(int fd, char *line, size_t cap) {
    size_t n = 0;
    while (n < cap - 1) {
        ssize_t r = read(fd, line + n, 1);
        if (r <= 0) return -1;
        if (line[n] == '\n') break;
        n++;
    }
    line[n] = 0;
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = scratch_dir(argc, argv, "test_share_field");
    char sock[200];
    snprintf(sock, sizeof sock, "%s/s", dir);
    if (hv_share_start(dir, sock, NULL) != 0) {
        fprintf(stderr, "share didn't start\n");
        return 2;
    }
    hv_share_set_listener(listener);
    HvShareField f;
    CHECK(!hv_share_field(&f) && f.state == HV_FOCUS_UNKNOWN, "unknown with no helper");

    unsigned before = told;
    int fd = link_open(sock);
    CHECK(fd >= 0, "the link opens");
    CHECK(heard_after(before), "the listener hears the link open");
    hv_share_field(&f);
    unsigned opened = f.generation;
    CHECK(f.state == HV_FOCUS_UNKNOWN, "a new link knows nothing yet");

    /* A text field (code), with its frame. */
    before = told;
    say(fd, "field 1 3 text code 0.1000 0.2000 0.3000 0.0400\n");
    CHECK(heard_after(before), "the listener hears a field line");
    CHECK(hv_share_field(&f) && f.serial == 1 && f.id == 3 && f.state == HV_FOCUS_TEXT && f.kind == HV_FIELD_CODE,
          "field 1 3 text code");
    CHECK(f.frame[2] > 0.29f && f.frame[2] < 0.31f && f.generation == opened, "its frame, the same link");
    CHECK(hv_share_focus() == HV_FOCUS_TEXT, "the old way agrees");

    /* Its context; then a stale one for a field that had the focus before. */
    say(fd, "ctx 3 2 aGk= Zm9v\n");
    say(fd, "ctx 2 0 c3RhbGU= -\n");
    pause_ms(100);
    char b[64], a[64];
    int sel = 0, known = 0, kind = -1;
    hv_share_context(b, sizeof b, a, sizeof a, &sel, &known, &kind);
    CHECK(known && sel == 2 && !strcmp(b, "aGk=") && !strcmp(a, "Zm9v") && kind == HV_FIELD_CODE, "ctx for field 3, not field 2's");

    /* The helper can't tell: the field line says so; the old focus stays. */
    before = told;
    say(fd, "field 2 4 unknown prose\n");
    CHECK(heard_after(before), "heard");
    hv_share_field(&f);
    CHECK(f.state == HV_FOCUS_UNKNOWN && f.id == 4 && f.serial == 2, "unknown, field 4");
    CHECK(hv_share_focus() == HV_FOCUS_TEXT, "the old way keeps what was known");
    hv_share_context(b, sizeof b, a, sizeof a, &sel, &known, &kind);
    CHECK(!known, "another field: field 3's context is gone");

    /* Nothing to type into. */
    say(fd, "field 3 5 none prose\n");
    pause_ms(100);
    hv_share_field(&f);
    CHECK(f.state == HV_FOCUS_NONE && f.frame[2] == 0, "none, no frame");

    /* A tap on the touchscreen: asked, and answered. */
    unsigned id = hv_share_tap(0.25, 0.75);
    CHECK(id != 0, "a tap is asked");
    char line[128];
    CHECK(hear(fd, line, sizeof line) == 0, "the helper is asked");
    unsigned asked = 0;
    double x = 0, y = 0;
    CHECK(sscanf(line, "tap %u %lf %lf", &asked, &x, &y) == 3 && asked == id && x == 0.25 && y == 0.75, line);
    float frame[4];
    char role[48];
    CHECK(hv_share_tap_answer(id, frame, role, sizeof role) == HV_TAP_WAITING, "not answered yet");
    char answer[128];
    snprintf(answer, sizeof answer, "tap %u editable AXTextField 0.2000 0.7000 0.1000 0.0500\n", id);
    before = told;
    say(fd, answer);
    CHECK(heard_after(before), "the listener hears the tap's answer");
    CHECK(hv_share_tap_answer(id, frame, role, sizeof role) == HV_TAP_EDITABLE && !strcmp(role, "AXTextField") &&
              frame[3] > 0.04f,
          "editable, its role and frame");

    /* A hit is answered with a tap line too, by the same id. */
    unsigned hit = hv_share_touch_hit(0.5, 0.5);
    CHECK(hear(fd, line, sizeof line) == 0 && !strncmp(line, "hit ", 4), "the hit is asked");
    snprintf(answer, sizeof answer, "hit %u scroll\ntap %u no AXButton\n", hit, hit);
    say(fd, answer);
    pause_ms(100);
    CHECK(hv_share_touch_answer(hit) == HV_TOUCH_SCROLL, "the hit's answer");
    CHECK(hv_share_tap_answer(hit, frame, role, sizeof role) == HV_TAP_NO && !strcmp(role, "AXButton") && frame[2] == 0, "and what it landed in");

    /* Shut Down, asked of the helper: macOS asks the person first (only the first time), then has it in hand; an
       app stops it, and a late "sent" doesn't hide that. A newer request: the older one's answers no longer count. */
    unsigned power = hv_share_power(0);
    CHECK(power != 0, "a shutdown is asked");
    unsigned heard_id = 0;
    CHECK(hear(fd, line, sizeof line) == 0 && sscanf(line, "power shutdown %u", &heard_id) == 1 && heard_id == power, line);
    CHECK(hv_share_power_answer(power) == HV_POWER_WAITING, "not answered yet");
    snprintf(answer, sizeof answer, "power %u asking\n", power);
    say(fd, answer);
    pause_ms(100);
    CHECK(hv_share_power_answer(power) == HV_POWER_ASKING, "macOS asks the person");
    snprintf(answer, sizeof answer, "power %u sent\n", power);
    say(fd, answer);
    pause_ms(100);
    CHECK(hv_share_power_answer(power) == HV_POWER_SENT, "sent");
    snprintf(answer, sizeof answer, "power %u cancelled\npower %u sent\n", power, power);
    say(fd, answer);
    pause_ms(100);
    CHECK(hv_share_power_answer(power) == HV_POWER_CANCELLED, "an app stopped it, and stays stopped");
    unsigned again = hv_share_power(1);
    CHECK(again != 0 && again != power && hear(fd, line, sizeof line) == 0 && !strncmp(line, "power restart ", 14), "a restart is asked");
    snprintf(answer, sizeof answer, "power %u failed -1743\n", power);
    say(fd, answer);
    pause_ms(100);
    CHECK(hv_share_power_answer(again) == HV_POWER_WAITING && hv_share_power_answer(power) == HV_POWER_WAITING,
          "the old request's answer is for no one");
    snprintf(answer, sizeof answer, "power %u failed -1743\n", again);
    say(fd, answer);
    pause_ms(100);
    CHECK(hv_share_power_answer(again) == HV_POWER_FAILED, "failed: the power button is pressed instead");

    /* "look": asked as the password's Return is due, answered after anything said before it. */
    unsigned look = hv_share_look();
    hv_share_look_send(look);
    char lookLine[32];
    snprintf(lookLine, sizeof lookLine, "look %u", look);
    CHECK(look != 0 && hear(fd, line, sizeof line) == 0 && !strcmp(line, lookLine), "a look is asked");
    int sure = 7;
    CHECK(hv_share_look_answer(look, &sure) == HV_TAP_WAITING && sure == 0, "not yet");
    before = told;
    snprintf(answer, sizeof answer, "looked %u secure\n", look);
    say(fd, answer);
    CHECK(heard_after(before), "the listener hears the answer");
    CHECK(hv_share_look_answer(look, &sure) == HV_FOCUS_SECURE && sure == 1, "a password field it is sure of");
    snprintf(answer, sizeof answer, "looked %u text\n", look);
    say(fd, answer);
    pause_ms(100);
    CHECK(hv_share_look_answer(look, &sure) == HV_FOCUS_SECURE && sure == 1, "answered once");
    /* One taken only from its app holding secure input (Terminal's sudo, or its Secure Keyboard Entry). */
    unsigned held = hv_share_look();
    hv_share_look_send(held);
    CHECK(held != 0 && held != look && hear(fd, line, sizeof line) == 0, "another look");
    snprintf(answer, sizeof answer, "looked %u held\n", held);
    say(fd, answer);
    pause_ms(100);
    CHECK(hv_share_look_answer(held, &sure) == HV_FOCUS_SECURE && sure == 0, "held: a password field it isn't sure of");
    unsigned text = hv_share_look();
    hv_share_look_send(text);
    CHECK(text != 0 && hear(fd, line, sizeof line) == 0, "a third look");
    snprintf(answer, sizeof answer, "looked %u text\nlooked %u secure\n", text, text + 1);
    say(fd, answer);
    pause_ms(100);
    CHECK(hv_share_look_answer(text, &sure) == HV_FOCUS_TEXT && sure == 0, "a text field");
    CHECK(hv_share_look_answer(text + 1, &sure) == HV_FOCUS_UNKNOWN && sure == 0, "an answer to nothing asked is for no one");
    CHECK(hv_share_look_answer(0, NULL) == HV_FOCUS_UNKNOWN, "no look");

    /* A password field; then the link closes with a look unanswered: unknown, and a new generation. */
    say(fd, "field 4 6 secure plain 0.1 0.1 0.2 0.03\n");
    pause_ms(100);
    CHECK(hv_share_field(&f) && f.state == HV_FOCUS_SECURE && f.kind == HV_FIELD_PLAIN, "a password field");
    unsigned unanswered = hv_share_look();
    hv_share_look_send(unanswered);
    CHECK(unanswered != 0 && hear(fd, line, sizeof line) == 0, "a look the link closes on");
    before = told;
    close(fd);
    CHECK(heard_after(before), "the listener hears the link close");
    CHECK(!hv_share_field(&f) && f.state == HV_FOCUS_UNKNOWN && f.generation != opened, "closed: unknown, another generation");
    CHECK(hv_share_look_answer(unanswered, &sure) == HV_FOCUS_UNKNOWN && sure == 0, "closed: the look has no answer coming");
    CHECK(hv_share_look() == 0, "no look asked with no helper");
    hv_share_look_send(unanswered); /* sent after the link closed on it: nothing goes, nothing breaks */
    CHECK(hv_share_look_answer(unanswered, &sure) == HV_FOCUS_UNKNOWN, "a look sent after its link closed stays unknown");

    /* A new helper numbers afresh; its generation tells it apart. */
    fd = link_open(sock);
    say(fd, "field 1 1 text prose\n");
    pause_ms(150);
    unsigned closed = f.generation;
    CHECK(hv_share_field(&f) && f.state == HV_FOCUS_TEXT && f.id == 1 && f.generation != closed, "a new link, field 1");
    close(fd);
    pause_ms(50);

    scratch_done();
    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    printf("share field: all passed\n");
    return 0;
}
