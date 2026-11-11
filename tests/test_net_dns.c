/* The guest's DNS server (host/net_dns.c), played with raw Ethernet frames and three DNS servers on this
   Mac's loopback: one that never answers, one that refuses everything, and one (IPv6) that answers. A
   query goes past the first two to the third, which is asked first from then on; an answer too long for
   one frame comes in IPv4 fragments; the same query sent twice is answered once; with no server
   answering, an address query is answered from getaddrinfo, and anything else gets the refusal or
   SERVFAIL rather than nothing. Needs no network beyond this Mac's loopback. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "net_dns.h"

static int checks, failures;
static void check(const char *what, long long got, long long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got %lld, want %lld\n", what, got, want);
    }
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8, p[1] = v & 0xff; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t sum16(const uint8_t *p, size_t len, uint32_t sum) {
    for (size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)p[i] << 8 | p[i + 1];
    if (len & 1) sum += (uint32_t)p[len - 1] << 8;
    return sum;
}
static uint16_t fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}
static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void sleep_ms(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&ts, NULL);
}

static const uint8_t guest_mac[6] = {0x02, 0x48, 0x45, 0x4e, 0x47, 0x45}, dns_mac[6] = {0x52, 0x55, 10, 0, 2, 3};
static const uint8_t guest_ip[4] = {10, 0, 2, 15}, dns_ip[4] = {10, 0, 2, 3};

/* ---- the fake servers ---- */

enum { SILENT, REFUSER, ANSWERER };
typedef struct Server {
    int fd, kind;
    unsigned port;
    _Atomic int queries;
} Server;
static Server silent, refuser, answerer;

static int server_open(Server *s, int kind, int v6) {
    s->kind = kind;
    s->fd = socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    socklen_t len;
    if (v6) {
        struct sockaddr_in6 *a = (void *)&ss;
        a->sin6_family = AF_INET6, a->sin6_len = sizeof *a, a->sin6_addr = in6addr_loopback;
        len = sizeof *a;
    } else {
        struct sockaddr_in *a = (void *)&ss;
        a->sin_family = AF_INET, a->sin_len = sizeof *a, a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        len = sizeof *a;
    }
    if (s->fd < 0 || bind(s->fd, (struct sockaddr *)&ss, len) != 0) return -1;
    getsockname(s->fd, (struct sockaddr *)&ss, &len);
    s->port = ntohs(v6 ? ((struct sockaddr_in6 *)&ss)->sin6_port : ((struct sockaddr_in *)&ss)->sin_port);
    return 0;
}

static void *server_thread(void *arg) {
    Server *s = arg;
    static uint8_t in[4096], out[8192];
    for (;;) {
        struct sockaddr_storage from;
        socklen_t flen = sizeof from;
        ssize_t n = recvfrom(s->fd, in, sizeof in, 0, (struct sockaddr *)&from, &flen);
        if (n < 12) continue;
        s->queries++;
        size_t q = 12; /* the end of the one question */
        char name[256];
        size_t nl = 0;
        while (q < (size_t)n && in[q]) {
            if (nl) name[nl++] = '.';
            memcpy(name + nl, in + q + 1, in[q]);
            nl += in[q], q += 1 + in[q];
        }
        name[nl] = 0;
        q += 5;
        unsigned type = get16(in + q - 4);
        memcpy(out, in, q);
        put16(out + 2, 0x8180), put16(out + 4, 1), put16(out + 6, 0), put16(out + 8, 0), put16(out + 10, 0);
        size_t len = q;
        if (s->kind == REFUSER) {
            out[3] = 0x85;
        } else if (!strcmp(name, "example.test") && type == 1) {
            put16(out + 6, 1);
            static const uint8_t rr[] = {0xc0, 12, 0, 1, 0, 1, 0, 0, 1, 44, 0, 4, 192, 0, 2, 7};
            memcpy(out + len, rr, sizeof rr), len += sizeof rr;
        } else if (!strcmp(name, "big.test") && type == 16) {
            put16(out + 6, 12);
            for (int i = 0; i < 12; i++) {
                static const uint8_t head[] = {0xc0, 12, 0, 16, 0, 1, 0, 0, 1, 44, 0, 251, 250};
                memcpy(out + len, head, sizeof head), len += sizeof head;
                memset(out + len, 'a' + i, 250), len += 250;
            }
        } else {
            out[3] = 0x83; /* no such name */
        }
        sendto(s->fd, out, len, 0, (struct sockaddr *)&from, flen);
    }
    return NULL;
}

/* ---- the guest ---- */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *frames[256];
static uint32_t frame_len[256];
static int nframes;

static void deliver(const uint8_t *f, uint32_t len) {
    pthread_mutex_lock(&mu);
    if (nframes < 256) {
        frames[nframes] = malloc(len);
        memcpy(frames[nframes], f, len);
        frame_len[nframes++] = len;
    }
    pthread_mutex_unlock(&mu);
}

static uint32_t make_query(uint8_t *f, uint16_t id, uint16_t sport, const char *name, uint16_t type) {
    uint8_t dns[300];
    put16(dns, id), put16(dns + 2, 0x0100), put16(dns + 4, 1), put16(dns + 6, 0), put16(dns + 8, 0), put16(dns + 10, 1);
    size_t len = 12;
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        dns[len++] = (uint8_t)l;
        memcpy(dns + len, p, l), len += l;
        p += l + (dot != NULL);
    }
    dns[len++] = 0;
    put16(dns + len, type), put16(dns + len + 2, 1), len += 4;
    static const uint8_t opt[] = {0, 0, 41, 16, 0, 0, 0, 0, 0, 0, 0}; /* EDNS, 4096 bytes */
    memcpy(dns + len, opt, sizeof opt), len += sizeof opt;
    memcpy(f, dns_mac, 6), memcpy(f + 6, guest_mac, 6), put16(f + 12, 0x0800);
    uint8_t *ip = f + 14, *udp = ip + 20;
    ip[0] = 0x45, ip[1] = 0, put16(ip + 2, (uint16_t)(28 + len)), put16(ip + 4, id), put16(ip + 6, 0x4000);
    ip[8] = 64, ip[9] = 17, put16(ip + 10, 0), memcpy(ip + 12, guest_ip, 4), memcpy(ip + 16, dns_ip, 4);
    put16(ip + 10, fold(sum16(ip, 20, 0)));
    put16(udp, sport), put16(udp + 2, 53), put16(udp + 4, (uint16_t)(8 + len)), put16(udp + 6, 0);
    memcpy(udp + 8, dns, len);
    return (uint32_t)(14 + 28 + len);
}

/* The next whole datagram for the guest, put together from its fragments: the DNS message in msg. Checks
   every frame's addresses and IP checksum and the datagram's UDP checksum. Its length, or -1. */
static int await_answer(uint16_t dport, uint8_t *msg, int timeout_ms, int *nfrags) {
    int64_t end = now_ms() + timeout_ms;
    for (;;) {
        pthread_mutex_lock(&mu);
        int done = -1, last = -1;
        size_t total = 0;
        for (int i = 0; i < nframes && done < 0; i++) {
            const uint8_t *ip = frames[i] + 14;
            if (!(get16(ip + 6) & 0x2000)) last = i, total = (get16(ip + 6) & 0x1fff) * 8u + get16(ip + 2) - 20u, done = i;
        }
        if (done >= 0) {
            static uint8_t udp[70000];
            int ok = 1;
            for (int i = 0; i <= last; i++) {
                const uint8_t *f = frames[i], *ip = f + 14;
                ok &= memcmp(f, guest_mac, 6) == 0 && memcmp(f + 6, dns_mac, 6) == 0 && get16(f + 12) == 0x0800;
                ok &= memcmp(ip + 12, dns_ip, 4) == 0 && memcmp(ip + 16, guest_ip, 4) == 0 && ip[9] == 17;
                ok &= fold(sum16(ip, 20, 0)) == 0 && get16(ip + 4) == get16(frames[0] + 18);
                ok &= frame_len[i] == 14u + get16(ip + 2) && frame_len[i] <= 1514;
                memcpy(udp + (get16(ip + 6) & 0x1fff) * 8, ip + 20, get16(ip + 2) - 20u);
            }
            check("frames are well formed", ok, 1);
            uint8_t pseudo[12];
            memcpy(pseudo, dns_ip, 4), memcpy(pseudo + 4, guest_ip, 4), pseudo[8] = 0, pseudo[9] = 17;
            put16(pseudo + 10, (uint16_t)total);
            check("UDP checksum", fold(sum16(udp, total, sum16(pseudo, 12, 0))), 0);
            check("from port 53", get16(udp), 53);
            check("to the guest's port", get16(udp + 2), dport);
            check("UDP length", get16(udp + 4), (long long)total);
            memcpy(msg, udp + 8, total - 8);
            if (nfrags) *nfrags = last + 1;
            for (int i = 0; i <= last; i++) free(frames[i]);
            memmove(frames, frames + last + 1, sizeof *frames * (size_t)(nframes - last - 1));
            memmove(frame_len, frame_len + last + 1, sizeof *frame_len * (size_t)(nframes - last - 1));
            nframes -= last + 1;
            pthread_mutex_unlock(&mu);
            return (int)total - 8;
        }
        pthread_mutex_unlock(&mu);
        if (now_ms() > end) return -1;
        sleep_ms(5);
    }
}

static void use_servers(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void use_servers(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    setenv("HV_DNS", buf, 1);
    hv_dns_network_changed();
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    uint8_t f[600], msg[70000];
    uint32_t len;

    if (server_open(&silent, SILENT, 0) || server_open(&refuser, REFUSER, 0) || server_open(&answerer, ANSWERER, 1)) {
        printf("cannot open the test's DNS servers on the loopback\n");
        return 1;
    }
    pthread_t t;
    pthread_create(&t, NULL, server_thread, &refuser);
    pthread_create(&t, NULL, server_thread, &answerer);

    len = make_query(f, 1, 40000, "example.test", 1);
    check("nothing is taken before the resolver starts", hv_dns_take(f, len), 0);
    use_servers("127.0.0.1:%u, 127.0.0.1:%u, [::1]:%u", silent.port, refuser.port, answerer.port);
    check("start", hv_dns_start(deliver), 0);

    /* What is not a UDP query to 10.0.2.3:53 is left to the NAT. */
    uint8_t g[600];
    memcpy(g, f, len);
    g[14 + 19] = 2; /* to 10.0.2.2 */
    check("to another address", hv_dns_take(g, len), 0);
    memcpy(g, f, len);
    put16(g + 14 + 22, 123);
    check("to another port", hv_dns_take(g, len), 0);
    memcpy(g, f, len);
    g[14 + 9] = 6;
    check("TCP", hv_dns_take(g, len), 0);
    memcpy(g, f, len);
    put16(g + 12, 0x0806);
    check("ARP", hv_dns_take(g, len), 0);
    memcpy(g, f, len);
    put16(g + 14 + 6, 0x2000);
    check("a fragment", hv_dns_take(g, len), 0);

    /* Past the silent server and the refusing one to the one that answers. */
    int64_t t0 = now_ms();
    len = make_query(f, 0x1234, 40000, "example.test", 1);
    check("a query is taken", hv_dns_take(f, len), 1);
    int n = await_answer(40000, msg, 4000, NULL);
    int64_t took = now_ms() - t0;
    check("answered", n > 0, 1);
    if (n > 0) {
        check("the guest's id", get16(msg), 0x1234);
        check("no error", msg[3] & 15, 0);
        check("one answer", get16(msg + 6), 1);
        static const uint8_t addr[4] = {192, 0, 2, 7};
        check("the address", memcmp(msg + n - 4, addr, 4), 0);
    }
    check("the silent server was waited for, briefly", took >= 600 && took < 2000, 1);
    check("the refusing server was asked", refuser.queries, 1);
    check("the answering server was asked", answerer.queries, 1);

    /* The one that answered is asked first now. */
    t0 = now_ms();
    len = make_query(f, 0x2222, 40001, "example.test", 1);
    hv_dns_take(f, len);
    n = await_answer(40001, msg, 4000, NULL);
    took = now_ms() - t0;
    check("answered again", n > 0 && get16(msg) == 0x2222 && get16(msg + 6) == 1, 1);
    check("at once", took < 400, 1);
    check("without the refusing server", refuser.queries, 1);

    /* An answer longer than a frame: IPv4 fragments. */
    len = make_query(f, 0x3333, 40002, "big.test", 16);
    hv_dns_take(f, len);
    int frags = 0;
    n = await_answer(40002, msg, 4000, &frags);
    check("the long answer", n > 3000 && get16(msg) == 0x3333 && get16(msg + 6) == 12, 1);
    check("in fragments", frags, 3);
    if (n > 0) check("whole", msg[n - 1], 'a' + 11);

    /* The same query twice: one answer. */
    len = make_query(f, 0x4444, 40003, "nothing.test", 1);
    hv_dns_take(f, len);
    hv_dns_take(f, len);
    n = await_answer(40003, msg, 4000, NULL);
    check("no such name", n > 0 && get16(msg) == 0x4444 && (msg[3] & 15) == 3, 1);
    check("answered once", await_answer(40003, msg, 300, NULL), -1);

    /* Nothing answers: getaddrinfo does, for an address. */
    use_servers("127.0.0.1:%u", silent.port);
    len = make_query(f, 0x5555, 40004, "localhost", 1);
    hv_dns_take(f, len);
    n = await_answer(40004, msg, 4000, NULL);
    check("answered by getaddrinfo", n > 0 && get16(msg) == 0x5555 && (msg[3] & 15) == 0 && get16(msg + 6) >= 1, 1);
    if (n > 0) {
        static const uint8_t lo[] = {0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 127, 0, 0, 1};
        check("127.0.0.1", memcmp(msg + n - sizeof lo, lo, sizeof lo), 0);
    }

    /* Only refusals, and not an address question: the refusal at once. */
    use_servers("127.0.0.1:%u", refuser.port);
    t0 = now_ms();
    len = make_query(f, 0x6666, 40005, "example.test", 15);
    hv_dns_take(f, len);
    n = await_answer(40005, msg, 4000, NULL);
    check("refused", n > 0 && get16(msg) == 0x6666 && (msg[3] & 15) == 5, 1);
    check("refused at once", now_ms() - t0 < 400, 1);

    /* No answer at all: SERVFAIL in the end, not silence. */
    use_servers("127.0.0.1:%u", silent.port);
    len = make_query(f, 0x7777, 40006, "example.test", 15);
    hv_dns_take(f, len);
    n = await_answer(40006, msg, 8000, NULL);
    check("SERVFAIL", n > 0 && get16(msg) == 0x7777 && (msg[3] & 15) == 2 && get16(msg + 4) == 1, 1);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
