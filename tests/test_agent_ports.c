/* The helper's reading of which ports a Mac listens on from outside itself (tools/agent-ports.c), against this Mac's
   own socket tables: what the test opens here, as a process that isn't root, which sees its own sockets. A TCP
   listener on every address (IPv4, or IPv6 taking IPv4 too) is found; one on 127.0.0.1 only, one bound and not
   listening, and an IPv6-only one aren't. A UDP socket on every address that isn't connected is found, on any port
   (one the system picked, 49152 and up, too: a MacBook's network reaches it there); one on 127.0.0.1 isn't, nor a
   connected one (a client's), on either side of 49152. The tables cut short, or with a record of a length this
   doesn't know (0 among them), are refused whole (-1) or read no further, never misread or read past (built with
   AddressSanitizer); the line the system part sends says them in order; and what the looks cost is noted once an
   hour, or at once when one is slow. The system part's key file, in a root folder of the test's own: made only
   where there is none, then read; a link, a folder, the wrong owner, mode or length, or one that can't be read,
   refused with a reason and left exactly as it was; a start cut short in the middle of making one doesn't stop the
   next. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "agent-ports.h"
#include "scratch_dir.h"

static int checks, failures;
static void check(const char *what, long long got, long long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got %lld, want %lld\n", what, got, want);
    }
}

/* A socket of type on addr (IPv4 "a.b.c.d" or IPv6 "::"), at port (0: any), listening or not; its port, or 0. */
static uint16_t open_at(int type, const char *addr, uint16_t port, int listening, int v6only, int *fd) {
    int six = strchr(addr, ':') != NULL;
    int s = socket(six ? AF_INET6 : AF_INET, type, 0);
    if (six) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof v6only);
    struct sockaddr_storage ss = {0};
    socklen_t sl;
    if (six) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
        a->sin6_family = AF_INET6, a->sin6_port = htons(port), sl = sizeof *a;
        inet_pton(AF_INET6, addr, &a->sin6_addr);
    } else {
        struct sockaddr_in *a = (struct sockaddr_in *)&ss;
        a->sin_family = AF_INET, a->sin_port = htons(port), sl = sizeof *a;
        inet_pton(AF_INET, addr, &a->sin_addr);
    }
    if (bind(s, (struct sockaddr *)&ss, sl) != 0 || (listening && listen(s, 4) != 0)) {
        close(s);
        return 0;
    }
    getsockname(s, (struct sockaddr *)&ss, &sl);
    *fd = s;
    return ntohs(six ? ((struct sockaddr_in6 *)&ss)->sin6_port : ((struct sockaddr_in *)&ss)->sin_port);
}

/* A UDP port below the ephemeral range, free on addr. */
static uint16_t udp_below(const char *addr, int *fd) {
    for (uint16_t p = (uint16_t)(20000 + getpid() % 20000);; p = p >= 48000 ? 20000 : p + 1) {
        uint16_t got = open_at(SOCK_DGRAM, addr, p, 0, 0, fd);
        if (got) return got;
    }
}

static int has(const AgentPort *p, int n, uint16_t port, int udp) {
    for (int i = 0; i < n; i++)
        if (p[i].port == port && p[i].udp == udp) return 1;
    return 0;
}

static void *table(const char *name, size_t *len) {
    *len = 0;
    if (sysctlbyname(name, NULL, len, NULL, 0) != 0) return NULL;
    *len += 65536;
    void *b = malloc(*len);
    if (sysctlbyname(name, b, len, NULL, 0) != 0) return free(b), NULL;
    return b;
}

/* The bytes in a file, or -1. */
static long file_bytes(const char *path, uint8_t *out, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    long n = (long)fread(out, 1, cap, f);
    fclose(f);
    return n;
}

static void put_file(const char *path, const void *bytes, size_t n, mode_t mode) {
    FILE *f = fopen(path, "wb");
    if (f) fwrite(bytes, 1, n, f), fclose(f);
    chmod(path, mode);
}

static void key_file(const char *scratch) {
    char root[1100], dir[1200], kp[1300], other[1300], ln[1300], ln2[1300], nowhere[1300], stale[1300], why[400];
    uint8_t key[AGENT_KEY_SIZE], again[AGENT_KEY_SIZE], disk[64], before[64];
    uid_t me = geteuid();
    snprintf(root, sizeof root, "%s/root", scratch);
    snprintf(dir, sizeof dir, "%s/Library/Application Support/Henge", root);
    snprintf(kp, sizeof kp, "%s/system-key", dir);

    check("key: no folder for it: refused", agent_system_key(kp, me, key, why, sizeof why), -1);
    check("and why is said", strstr(why, "can't be made") != NULL, 1);
    char mk[1300];
    snprintf(mk, sizeof mk, "%s/Library", root), mkdir(root, 0755), mkdir(mk, 0755);
    snprintf(mk, sizeof mk, "%s/Library/Application Support", root), mkdir(mk, 0755);
    mkdir(dir, 0755);

    check("none there: made", agent_system_key(kp, me, key, why, sizeof why), 1);
    struct stat st;
    check("as a plain file, 0600, its maker's", stat(kp, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 07777) == 0600 && st.st_uid == me, 1);
    check("of 32 bytes: the key", file_bytes(kp, disk, sizeof disk) == AGENT_KEY_SIZE && !memcmp(disk, key, AGENT_KEY_SIZE), 1);
    uint8_t zero[AGENT_KEY_SIZE] = {0};
    check("not all zeros", memcmp(key, zero, sizeof zero) != 0, 1);
    char tmp[1400];
    snprintf(tmp, sizeof tmp, "%s.new", kp);
    check("nothing left beside it", access(tmp, F_OK), -1);
    check("there: read, not made again", agent_system_key(kp, me, again, why, sizeof why), 0);
    check("the same key", memcmp(again, key, sizeof key), 0);

    /* Each refusal: nothing in the key, a reason, and the file exactly as it was. */
    memcpy(before, disk, AGENT_KEY_SIZE);
    chmod(kp, 0644);
    memset(again, 0xee, sizeof again);
    check("readable by others (0644): refused", agent_system_key(kp, me, again, why, sizeof why), -1);
    check("and said", strstr(why, "mode 0644") != NULL, 1);
    check("nothing in the key", memcmp(again, zero, sizeof zero), 0);
    check("the file left as it was", stat(kp, &st) == 0 && (st.st_mode & 07777) == 0644 && file_bytes(kp, disk, sizeof disk) == AGENT_KEY_SIZE &&
                                          !memcmp(disk, before, AGENT_KEY_SIZE), 1);
    chmod(kp, 0600);
    check("someone else's (not root's): refused", agent_system_key(kp, me + 1, again, why, sizeof why), -1);
    check("and said", strstr(why, "owned by user") != NULL, 1);
    check("the file left as it was", file_bytes(kp, disk, sizeof disk) == AGENT_KEY_SIZE && !memcmp(disk, before, AGENT_KEY_SIZE), 1);
    if (me != 0) { /* root reads it whatever its mode */
        chmod(kp, 0000);
        check("one that can't be read: refused", agent_system_key(kp, me, again, why, sizeof why), -1);
        chmod(kp, 0600);
        check("the file left as it was", file_bytes(kp, disk, sizeof disk) == AGENT_KEY_SIZE && !memcmp(disk, before, AGENT_KEY_SIZE), 1);
    }

    snprintf(other, sizeof other, "%s/short-key", dir);
    put_file(other, before, 16, 0600);
    check("16 bytes: refused", agent_system_key(other, me, again, why, sizeof why), -1);
    check("and said", strstr(why, "32 bytes") != NULL, 1);
    check("left 16 bytes, not made again", file_bytes(other, disk, sizeof disk), 16);
    put_file(other, before, 0, 0600);
    check("an empty one: refused, not made again", agent_system_key(other, me, again, why, sizeof why) == -1 && file_bytes(other, disk, sizeof disk) == 0, 1);

    snprintf(ln, sizeof ln, "%s/linked-key", dir);
    symlink(kp, ln);
    check("a link to a good key: refused", agent_system_key(ln, me, again, why, sizeof why), -1);
    check("and said", strstr(why, "link") != NULL, 1);
    check("still a link", lstat(ln, &st) == 0 && S_ISLNK(st.st_mode), 1);
    snprintf(ln2, sizeof ln2, "%s/dangling-key", dir);
    snprintf(nowhere, sizeof nowhere, "%s/nowhere", dir);
    symlink(nowhere, ln2);
    check("a link to nothing: refused", agent_system_key(ln2, me, again, why, sizeof why), -1);
    check("and nothing made where it points", access(nowhere, F_OK), -1);
    snprintf(other, sizeof other, "%s/folder-key", dir);
    mkdir(other, 0700);
    check("a folder: refused", agent_system_key(other, me, again, why, sizeof why), -1);
    check("and said", strstr(why, "plain file") != NULL, 1);

    snprintf(other, sizeof other, "%s/cut-short-key", dir);
    snprintf(stale, sizeof stale, "%s.new", other);
    put_file(stale, "half", 4, 0644);
    check("a start cut short while making one: the next makes it", agent_system_key(other, me, again, why, sizeof why), 1);
    check("whole", file_bytes(other, disk, sizeof disk) == AGENT_KEY_SIZE && !memcmp(disk, again, AGENT_KEY_SIZE), 1);
    check("and new, not the first's", memcmp(again, key, sizeof key) != 0, 1);

    uint8_t k[AGENT_KEY_SIZE];
    for (int i = 0; i < AGENT_KEY_SIZE; i++) k[i] = (uint8_t)(i * 37 + 0x0f);
    char hex[2 * AGENT_KEY_SIZE + 1];
    agent_key_hex(k, hex);
    check("the key in hex", strcmp(hex, "0f34597ea3c8ed12375c81a6cbf0153a5f84a9cef3183d6287acd1f61b40658a") == 0 && strlen(hex) == 64, 1);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int fd[24], k = 0;
    uint16_t any4 = open_at(SOCK_STREAM, "0.0.0.0", 0, 1, 0, &fd[k++]);
    uint16_t loop4 = open_at(SOCK_STREAM, "127.0.0.1", 0, 1, 0, &fd[k++]);
    uint16_t idle4 = open_at(SOCK_STREAM, "0.0.0.0", 0, 0, 0, &fd[k++]);
    uint16_t dual6 = open_at(SOCK_STREAM, "::", 0, 1, 0, &fd[k++]);
    uint16_t only6 = open_at(SOCK_STREAM, "::", 0, 1, 1, &fd[k++]);
    uint16_t udp4 = udp_below("0.0.0.0", &fd[k++]);
    uint16_t udploop = udp_below("127.0.0.1", &fd[k++]);
    uint16_t udpeph = open_at(SOCK_DGRAM, "0.0.0.0", 0, 0, 0, &fd[k++]); /* the system's pick, not connected */
    /* Connected (to this Mac's own 127.0.0.1): clients', below 49152 and on the system's pick. */
    int cfd = -1, cefd = -1;
    uint16_t udpconn = udp_below("0.0.0.0", &cfd), udpconneph = open_at(SOCK_DGRAM, "0.0.0.0", 0, 0, 0, &cefd);
    fd[k++] = cfd, fd[k++] = cefd;
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(udploop), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    int conn_ok = connect(cfd, (struct sockaddr *)&to, sizeof to) == 0 && connect(cefd, (struct sockaddr *)&to, sizeof to) == 0;
    /* Connected to 127.0.0.1, macOS gives the socket 127.0.0.1 as its own address too (so it isn't reached from
       outside either way). Connected elsewhere, it keeps an address the network reaches: only being connected says
       it is a client's. 192.0.2.1 is for documentation only; connecting UDP sends nothing. */
    int ofd = -1;
    uint16_t udpconnout = udp_below("0.0.0.0", &ofd);
    fd[k++] = ofd;
    struct sockaddr_in doc = {.sin_family = AF_INET, .sin_port = htons(9)};
    inet_pton(AF_INET, "192.0.2.1", &doc.sin_addr);
    int out_ok = connect(ofd, (struct sockaddr *)&doc, sizeof doc) == 0;
    if (!out_ok) printf("  (no route out of this Mac: the connected-elsewhere case is skipped)\n");
    printf("  ports: any %u, loopback %u, idle %u, dual %u, v6only %u, udp %u, udp loopback %u, udp ephemeral %u\n", any4, loop4, idle4, dual6, only6, udp4, udploop, udpeph);
    check("the test's sockets open", any4 && loop4 && idle4 && dual6 && only6 && udp4 && udploop && udpeph >= 49152 && udpconn && udpconn < 49152 &&
          udpconneph >= 49152 && conn_ok, 1);

    static AgentPort p[4096];
    int sockets = 0;
    int got = agent_ports_read(p, 4096, &sockets);
    printf("  %d sockets seen (%s), %d ports listened on from outside\n", sockets, geteuid() ? "not root: its own" : "root", got);
    check("the tables are read", got >= 0, 1);
    check("at least the test's own sockets are walked", sockets >= 8, 1);
    check("a TCP listener on every address: found", has(p, got, any4, 0), 1);
    check("one on 127.0.0.1 only: not", has(p, got, loop4, 0), 0);
    check("one bound but not listening: not", has(p, got, idle4, 0), 0);
    check("an IPv6 one taking IPv4 too: found", has(p, got, dual6, 0), 1);
    check("an IPv6-only one: not (the device forwards IPv4)", has(p, got, only6, 0), 0);
    check("UDP on every address: found", has(p, got, udp4, 1), 1);
    check("UDP on 127.0.0.1: not", has(p, got, udploop, 1), 0);
    check("UDP on a port the system picked (49152 and up), not connected: found", has(p, got, udpeph, 1), 1);
    check("UDP on every address below 49152, connected: not (a client's)", has(p, got, udpconn, 1), 0);
    check("UDP on a port the system picked, connected: not", has(p, got, udpconneph, 1), 0);
    if (out_ok) check("UDP on every address, connected to another machine: not", has(p, got, udpconnout, 1), 0);
    int sorted = 1;
    for (int i = 1; i < got; i++)
        if (p[i - 1].udp > p[i].udp || (p[i - 1].udp == p[i].udp && p[i - 1].port >= p[i].port)) sorted = 0;
    check("sorted, TCP first, without repeats", sorted, 1);

    /* The tables cut anywhere, or a record's length changed: never misread (and never read past the end). */
    size_t len;
    unsigned char *t = table("net.inet.tcp.pcblist_n", &len);
    check("the TCP table", t != NULL, 1);
    if (t) {
        AgentPort q[4096];
        int whole = agent_ports_parse(t, len, 0, NULL, 0, q, 4096, NULL);
        check("whole: read", whole >= 1, 1);
        int bad = 0;
        for (size_t cut = 0; cut < len; cut += 1 + cut / 64) {
            unsigned char *c = malloc(cut ? cut : 1);
            memcpy(c, t, cut);
            int r = agent_ports_parse(c, cut, 0, NULL, 0, q, 4096, NULL);
            if (r > whole) bad++; /* a part can't hold more than the whole */
            free(c);
        }
        check("cut short anywhere: nothing more than the whole, nothing past the end", bad, 0);
        uint32_t head_len;
        memcpy(&head_len, t, 4);
        unsigned char *c = malloc(len);
        memcpy(c, t, len);
        uint32_t wrong = head_len + 8;
        memcpy(c, &wrong, 4);
        check("a head of another length: refused", agent_ports_parse(c, len, 0, NULL, 0, q, 4096, NULL), -1);
        /* The first socket record (kind 1) made 8 bytes longer, as a macOS that added a field would. */
        memcpy(c, t, len);
        size_t at = (head_len + 7u) & ~7u;
        int changed = 0;
        while (at + 8 <= len) {
            uint32_t rl, kind;
            memcpy(&rl, c + at, 4), memcpy(&kind, c + at + 4, 4);
            if (rl <= head_len) break;
            if (kind == 1) {
                rl += 8;
                memcpy(c + at, &rl, 4);
                changed = 1;
                break;
            }
            at += (rl + 7u) & ~7u;
        }
        check("(a socket record to change)", changed, 1);
        check("a socket record of another length: refused", agent_ports_parse(c, len, 0, NULL, 0, q, 4096, NULL), -1);
        /* Every record, its length made 0, 1, 7, 8, one less or more, all that is left, one past it, or the largest:
           refused or read no further, never more than the whole, never past the end. */
        int swept = 0, worse = 0;
        at = (head_len + 7u) & ~7u;
        while (at + 8 <= len) {
            uint32_t rl;
            memcpy(&rl, t + at, 4);
            if (rl <= head_len) break;
            uint32_t lens[] = {0, 1, 7, 8, rl - 1, rl + 1, (uint32_t)(len - at), (uint32_t)(len - at + 1), 0xffffffffu};
            for (size_t j = 0; j < sizeof lens / sizeof *lens; j++) {
                memcpy(c, t, len);
                memcpy(c + at, &lens[j], 4);
                if (agent_ports_parse(c, len, 0, NULL, 0, q, 4096, NULL) > whole) worse++; /* c is len long: past it, ASan stops the test */
                swept++;
            }
            at += (rl + 7u) & ~7u;
        }
        check("(records swept)", swept >= 9, 1);
        check("a record of any length: never more than the whole", worse, 0);
        free(c), free(t);
    }

    AgentPort l[] = {{22, 0}, {445, 0}, {88, 1}, {3283, 1}};
    char line[256];
    agent_ports_line(l, 4, line, sizeof line);
    check("the line", strcmp(line, "ports tcp 22 445 udp 88 3283\n"), 0);
    agent_ports_line(l, 0, line, sizeof line);
    check("none: an empty line", strcmp(line, "ports\n"), 0);

    /* What the looks cost: a look a second, 1 ms each: one note an hour, not before. */
    AgentCost cost = {0}, out;
    int notes = 0, first = 0;
    uint64_t s0 = 1000000000ull;
    for (int i = 0; i <= 7200; i++)
        if (agent_cost_add(&cost, s0 + (uint64_t)i * 1000000000ull, 1000000, &out) && !notes++) first = i;
    check("a look a second for two hours: two notes", notes, 2);
    check("the first after an hour", first, 3600);
    check("for that hour's looks", out.looks, 3600);
    /* A slow look: noted at once, once in its hour. */
    cost = (AgentCost){0}, notes = 0;
    for (int i = 0; i < 600; i++) notes += agent_cost_add(&cost, s0 + (uint64_t)i * 1000000000ull, i == 10 || i == 20 ? 50000000 : 1000000, &out);
    check("two slow looks in ten minutes: one note", notes, 1);
    check("of the slowest", out.most, 50000000);

    key_file(scratch_dir(argc, argv, "test_agent_ports"));
    scratch_done();

    while (k) close(fd[--k]);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
