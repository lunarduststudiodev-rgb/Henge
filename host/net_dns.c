/* The guest's DNS server, 10.0.2.3, answered here rather than by libslirp.

   libslirp forwards the guest's queries to the first IPv4 server res_ninit names, once a minute looked up.
   On an iPad that is often no server at all: the app may not read the resolver's configuration, the
   network may give IPv6 DNS servers only (libslirp then refuses the query), or a VPN names one that does
   not answer. macOS then shows a connection and nothing loads, while a DNS server set by hand works
   (issue #34).

   So every query the guest sends to 10.0.2.3 over UDP comes here and goes out as it is, in turn to:
     1. the host's own DNS servers (res_ninit), IPv4 and IPv6, starting with the one that answered last;
     2. public ones: 1.1.1.1, 8.8.8.8 and their IPv6 twins;
   the next one STAGGER_MS after the last, or at once when a server refuses (SERVFAIL, REFUSED). The first
   real answer goes to the guest. If none has answered GAI_AFTER_MS in, an address query (A, AAAA) is
   also put to the host's own resolver, getaddrinfo, the one Safari uses, which works whenever the host
   has internet (a VPN's split DNS, encrypted DNS, NAT64), and its addresses are answered back. After
   GIVE_UP_MS the guest gets the first refusal it saw, or SERVFAIL. While only getaddrinfo answers,
   it is asked at once, alongside the servers.

   An answer too long for UDP comes marked truncated (TC), as the server sent it, and the guest asks again
   over TCP. libslirp sends TCP to 10.0.2.3:53 here too (third_party/libslirp, sotranslate_out4), to a
   server on the host's loopback: each query on such a connection goes over TCP to the same servers, the
   one that answered last first, each given TCP_TRY_MS, then to getaddrinfo for an address, then the first
   refusal or SERVFAIL, like a UDP one. (libslirp itself would send it to the first IPv4 server res_ninit
   names, often none on an iPad, and the guest's lookup would fail.) A thread per connection, at most
   TCP_CONNS; they are rare.

   HV_DNS=off leaves it all to libslirp as before. HV_DNS=addr[:port],... asks only those servers (and
   getaddrinfo), for trying things out and for the test.

   Answers longer than the guest's MTU go out as IPv4 fragments. Nothing here is in a snapshot: a query
   in flight when the Mac is suspended is simply asked again by the guest. */
#include "net_dns.h"

#include <TargetConditionals.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <resolv.h>
#include <netinet/tcp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_PENDING 128
#define MAX_SERVERS 12
#define MAX_INBOX 128
#define MAX_GAI 24
#define MAX_ADDRS 16
#define STAGGER_MS 80
#define GAI_AFTER_MS 80
#define GIVE_UP_MS 3000
#define SERVERS_FRESH_MS 30000
#define MTU 1500
#define TCP_CONNS 16
#define TCP_TRY_MS 800
#define TCP_GIVE_UP_MS 3000
#define TCP_IDLE_MS 10000

typedef struct Query { /* as the guest sent it */
    uint8_t guest_mac[6], our_mac[6], guest_ip[4], our_ip[4];
    uint16_t guest_port;
    uint8_t *msg;
    uint32_t len;
} Query;

typedef struct Pending {
    int used;
    Query q;
    uint16_t guest_id, up_id;
    uint32_t qend;   /* the end of the question section, or 12 when it could not be read */
    int gai_type;    /* 1 (A) or 28 (AAAA) when getaddrinfo can answer it, else 0 */
    char name[256];  /* the name asked, for getaddrinfo */
    struct sockaddr_storage srv[MAX_SERVERS];
    int nsrv, next, outstanding;
    unsigned answered; /* a bit per srv[] index: its reply is counted (a repeated datagram is not again) */
    int fd[2];       /* IPv4, IPv6 */
    int64_t t0, next_send;
    int gai;         /* 0 not asked, 1 asking, 2 done */
    uint64_t token;
    uint8_t *fallback; /* the best refusal so far */
    uint32_t fallback_len;
} Pending;

typedef struct GaiJob {
    uint64_t token;
    int family, n, status; /* status 0: addresses (n of them), 1: none (the name has none of that kind), 2: failed */
    char name[256];
    uint8_t addrs[MAX_ADDRS][16];
    struct GaiJob *next;
} GaiJob;

static void (*deliver)(const uint8_t *frame, uint32_t len);
static int enabled;
static int wake_pipe[2] = {-1, -1};
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; /* the inbox and the finished getaddrinfo jobs */
static Query inbox[MAX_INBOX];
static int ninbox;
static GaiJob *gai_done;
static _Atomic int gai_running;
static _Atomic int servers_stale = 1;

/* The resolver thread's own. */
static Pending pend[MAX_PENDING];
static struct sockaddr_storage servers[MAX_SERVERS];
static int nservers;
static int64_t servers_time;
static struct sockaddr_storage best;
static int have_best, told_gai;
static pthread_mutex_t best_mu = PTHREAD_MUTEX_INITIALIZER; /* best and have_best, for the TCP threads */
static int tcp_fd = -1;
static uint16_t tcp_port;
static _Atomic int tcp_conns;
static uint64_t next_token = 1;
static uint16_t ip_id;

static const uint8_t dns_ip[4] = {10, 0, 2, 3};

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

static void wake(void) {
    char c = 0;
    (void)!write(wake_pipe[1], &c, 1);
}

/* ---- the CPU thread's side ---- */

int hv_dns_take(const uint8_t *f, uint32_t len) {
    if (!enabled || len < 14 + 20 + 8 + 12 || f[12] != 0x08 || f[13] != 0x00) return 0;
    const uint8_t *ip = f + 14;
    unsigned ihl = (ip[0] & 15) * 4u;
    if (ip[0] >> 4 != 4 || ihl < 20 || ip[9] != 17 || memcmp(ip + 16, dns_ip, 4) != 0) return 0;
    if (get16(ip + 6) & 0x3fff) return 0; /* a fragment: the NAT puts those together */
    unsigned total = get16(ip + 2);
    if (total < ihl + 8 || 14 + total > len) return 0;
    const uint8_t *udp = ip + ihl;
    unsigned ulen = get16(udp + 4);
    if (get16(udp + 2) != 53 || ulen < 8 + 12 || ulen > total - ihl) return 0;
    Query q;
    memcpy(q.guest_mac, f + 6, 6), memcpy(q.our_mac, f, 6);
    memcpy(q.guest_ip, ip + 12, 4), memcpy(q.our_ip, ip + 16, 4);
    q.guest_port = get16(udp);
    q.len = ulen - 8;
    q.msg = malloc(q.len);
    if (!q.msg) return 1;
    memcpy(q.msg, udp + 8, q.len);
    pthread_mutex_lock(&mu);
    int ok = ninbox < MAX_INBOX;
    if (ok) inbox[ninbox++] = q;
    pthread_mutex_unlock(&mu);
    if (ok) wake();
    else free(q.msg); /* so many at once: the guest asks again */
    return 1;
}

void hv_dns_network_changed(void) {
    atomic_store(&servers_stale, 1);
    if (wake_pipe[1] >= 0) wake();
}

/* ---- frames for the guest ---- */

/* msg as UDP from 10.0.2.3:53 to the guest, in IPv4 fragments when longer than the MTU. */
static void send_to_guest(const Query *q, const uint8_t *msg, size_t len) {
    size_t ulen = 8 + len;
    if (ulen > 65535 - 20) return;
    uint8_t *udp = malloc(ulen);
    if (!udp) return;
    put16(udp, 53), put16(udp + 2, q->guest_port), put16(udp + 4, (uint16_t)ulen), put16(udp + 6, 0);
    memcpy(udp + 8, msg, len);
    uint8_t pseudo[12];
    memcpy(pseudo, q->our_ip, 4), memcpy(pseudo + 4, q->guest_ip, 4);
    pseudo[8] = 0, pseudo[9] = 17, put16(pseudo + 10, (uint16_t)ulen);
    uint16_t c = fold(sum16(udp, ulen, sum16(pseudo, 12, 0)));
    put16(udp + 6, c ? c : 0xffff);
    uint16_t id = ip_id++;
    static uint8_t f[14 + MTU];
    for (size_t off = 0; off < ulen;) {
        size_t chunk = ulen - off;
        int more = chunk > MTU - 20;
        if (more) chunk = (MTU - 20) & ~7u;
        memcpy(f, q->guest_mac, 6), memcpy(f + 6, q->our_mac, 6), put16(f + 12, 0x0800);
        uint8_t *ip = f + 14;
        ip[0] = 0x45, ip[1] = 0;
        put16(ip + 2, (uint16_t)(20 + chunk)), put16(ip + 4, id);
        put16(ip + 6, (uint16_t)((more ? 0x2000 : 0) | off / 8));
        ip[8] = 64, ip[9] = 17, put16(ip + 10, 0);
        memcpy(ip + 12, q->our_ip, 4), memcpy(ip + 16, q->guest_ip, 4);
        put16(ip + 10, fold(sum16(ip, 20, 0)));
        memcpy(ip + 20, udp + off, chunk);
        deliver(f, (uint32_t)(14 + 20 + chunk));
        off += chunk;
    }
    free(udp);
}

#define ANSWER_MAX (12 + 512 + MAX_ADDRS * 28)

/* An answer made here, to the question in msg (its section ending at qend) with this id: rcode, and
   addresses (alen bytes each) of type. Its length. */
static size_t make_answer(uint8_t out[ANSWER_MAX], uint16_t id, const uint8_t *msg, uint32_t qend, int type, int rcode,
                          uint8_t addrs[][16], int n, int alen) {
    size_t qbytes = qend - 12;
    if (qbytes > 512) qbytes = 0;
    put16(out, id);
    out[2] = 0x80 | (msg[2] & 0x79); /* QR, the opcode and RD asked */
    out[3] = 0x80 | (rcode & 15);    /* RA */
    put16(out + 4, qbytes ? 1 : 0), put16(out + 6, qbytes ? (uint16_t)n : 0), put16(out + 8, 0), put16(out + 10, 0);
    memcpy(out + 12, msg + 12, qbytes);
    size_t len = 12 + qbytes;
    for (int i = 0; qbytes && i < n; i++) {
        out[len] = 0xc0, out[len + 1] = 12; /* the name asked */
        put16(out + len + 2, (uint16_t)type), put16(out + len + 4, 1);
        put16(out + len + 6, 0), put16(out + len + 8, 60); /* a minute */
        put16(out + len + 10, (uint16_t)alen);
        memcpy(out + len + 12, addrs[i], (size_t)alen);
        len += 12 + (size_t)alen;
    }
    return len;
}

static void answer_here(Pending *p, int rcode, uint8_t addrs[][16], int n, int alen) {
    uint8_t out[ANSWER_MAX];
    send_to_guest(&p->q, out, make_answer(out, p->guest_id, p->q.msg, p->qend, p->gai_type, rcode, addrs, n, alen));
}

/* ---- servers ---- */

static int same_addr(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
    if (a->ss_family != b->ss_family) return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *x = (const void *)a, *y = (const void *)b;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    const struct sockaddr_in6 *x = (const void *)a, *y = (const void *)b;
    return x->sin6_port == y->sin6_port && memcmp(&x->sin6_addr, &y->sin6_addr, 16) == 0;
}

static void describe(const struct sockaddr_storage *s, char *buf, size_t len) {
    char a[INET6_ADDRSTRLEN] = "?";
    if (s->ss_family == AF_INET) inet_ntop(AF_INET, &((const struct sockaddr_in *)s)->sin_addr, a, sizeof a);
    else inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)s)->sin6_addr, a, sizeof a);
    unsigned port = ntohs(s->ss_family == AF_INET ? ((const struct sockaddr_in *)s)->sin_port : ((const struct sockaddr_in6 *)s)->sin6_port);
    if (port == 53) snprintf(buf, len, "%s", a);
    else snprintf(buf, len, s->ss_family == AF_INET ? "%s:%u" : "[%s]:%u", a, port);
}

static void add_server(struct sockaddr_storage *list, int *count, const struct sockaddr_storage *s) {
    if (*count == MAX_SERVERS) return;
    if (s->ss_family == AF_INET) {
        in_addr_t a = ntohl(((const struct sockaddr_in *)s)->sin_addr.s_addr);
        if (a == 0) return; /* "no server": res_ninit's stand-in for the local machine */
#if TARGET_OS_IPHONE
        if (a >> 24 == 127) return; /* nothing on an iPad answers DNS on its loopback */
#endif
    } else if (s->ss_family == AF_INET6) {
        const struct in6_addr *a = &((const struct sockaddr_in6 *)s)->sin6_addr;
        if (IN6_IS_ADDR_UNSPECIFIED(a)) return;
#if TARGET_OS_IPHONE
        if (IN6_IS_ADDR_LOOPBACK(a)) return;
#endif
    } else {
        return;
    }
    for (int i = 0; i < *count; i++)
        if (same_addr(&list[i], s)) return;
    list[(*count)++] = *s;
}

/* "1.1.1.1", "1.1.1.1:5353", "2606:4700:4700::1111", "[::1]:5353". */
static int parse_server(const char *text, struct sockaddr_storage *s) {
    char host[INET6_ADDRSTRLEN + 2];
    unsigned port = 53;
    const char *colon = strrchr(text, ':');
    memset(s, 0, sizeof *s);
    if (text[0] == '[') {
        const char *end = strchr(text, ']');
        if (!end || (size_t)(end - text - 1) >= sizeof host) return -1;
        memcpy(host, text + 1, (size_t)(end - text - 1)), host[end - text - 1] = 0;
        if (end[1] == ':') port = (unsigned)atoi(end + 2);
    } else if (colon && strchr(text, ':') == colon) { /* one colon: IPv4 and a port */
        if ((size_t)(colon - text) >= sizeof host) return -1;
        memcpy(host, text, (size_t)(colon - text)), host[colon - text] = 0;
        port = (unsigned)atoi(colon + 1);
    } else {
        snprintf(host, sizeof host, "%s", text);
    }
    if (port == 0 || port > 65535) return -1;
    struct sockaddr_in *v4 = (struct sockaddr_in *)s;
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)s;
    if (inet_pton(AF_INET, host, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET, v4->sin_len = sizeof *v4, v4->sin_port = htons((uint16_t)port);
        return 0;
    }
    memset(s, 0, sizeof *s);
    if (inet_pton(AF_INET6, host, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6, v6->sin6_len = sizeof *v6, v6->sin6_port = htons((uint16_t)port);
        return 0;
    }
    return -1;
}

/* The servers to ask, in order: HV_DNS's, or the host's own and the public ones. Their number. */
static int collect_servers(struct sockaddr_storage *list) {
    int count = 0;
    const char *env = getenv("HV_DNS");
    struct sockaddr_storage s;
    if (env && *env) {
        char text[512];
        snprintf(text, sizeof text, "%s", env);
        for (char *save = NULL, *t = strtok_r(text, ", ", &save); t; t = strtok_r(NULL, ", ", &save))
            if (parse_server(t, &s) == 0) add_server(list, &count, &s);
            else fprintf(stderr, "hengevm: dns: HV_DNS: not an address: %s\n", t);
        return count;
    }
    struct __res_state st;
    memset(&st, 0, sizeof st);
    if (res_ninit(&st) == 0) {
        union res_sockaddr_union u[MAX_SERVERS];
        int n = res_getservers(&st, u, MAX_SERVERS);
        for (int i = 0; i < n; i++) {
            memset(&s, 0, sizeof s);
            if (u[i].sin.sin_family == AF_INET) memcpy(&s, &u[i].sin, sizeof u[i].sin);
            else if (u[i].sin6.sin6_family == AF_INET6) memcpy(&s, &u[i].sin6, sizeof u[i].sin6);
            else continue;
            in_port_t *port = s.ss_family == AF_INET ? &((struct sockaddr_in *)&s)->sin_port : &((struct sockaddr_in6 *)&s)->sin6_port;
            if (*port == 0) *port = htons(53);
            add_server(list, &count, &s);
        }
        res_ndestroy(&st);
    }
    static const char *const fallback[] = {"1.1.1.1", "8.8.8.8", "2606:4700:4700::1111", "2001:4860:4860::8888"};
    for (size_t i = 0; i < sizeof fallback / sizeof *fallback; i++)
        if (parse_server(fallback[i], &s) == 0) add_server(list, &count, &s);
    return count;
}

static void load_servers(void) {
    nservers = collect_servers(servers);
    servers_time = now_ms();
}

/* ---- queries ---- */

/* The end of the question section (names are not compressed in a question), or 0. name gets the first
   name asked as text, or "" when it is not one getaddrinfo could be given. */
static uint32_t question_end(const uint8_t *m, uint32_t len, char *name, size_t namelen) {
    unsigned qd = get16(m + 4);
    uint32_t off = 12;
    size_t nl = 0;
    int bad = qd == 0;
    for (unsigned i = 0; i < qd; i++) {
        for (;;) {
            if (off >= len) return 0;
            unsigned l = m[off++];
            if (l == 0) break;
            if (l > 63 || off + l > len) return 0;
            if (i == 0 && !bad) {
                if (nl + l + 2 > namelen) bad = 1;
                if (nl && !bad) name[nl++] = '.';
                for (unsigned k = 0; k < l && !bad; k++) {
                    uint8_t c = m[off + k];
                    if (c <= ' ' || c >= 127 || c == '.' || c == '\\') bad = 1;
                    else name[nl++] = (char)c;
                }
            }
            off += l;
        }
        if (off + 4 > len) return 0;
        off += 4;
    }
    name[bad ? 0 : nl] = 0;
    return off;
}

/* 1 (A) or 28 (AAAA) when getaddrinfo can answer msg (one question, of class IN, a name it takes), else 0. */
static int gai_type_of(const uint8_t *msg, uint32_t qend, const char *name) {
    if (!qend || get16(msg + 4) != 1 || (msg[2] & 0x78) != 0 || !name[0] || get16(msg + qend - 2) != 1) return 0;
    unsigned type = get16(msg + qend - 4);
    return type == 1 || type == 28 ? (int)type : 0;
}

static void finish(Pending *p) {
    for (int i = 0; i < 2; i++)
        if (p->fd[i] >= 0) close(p->fd[i]);
    free(p->q.msg);
    free(p->fallback);
    p->used = 0;
}

static void gai_lookup(GaiJob *j) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = j->family;
    hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo(j->name, NULL, &hints, &res);
    if (r == 0) {
        for (struct addrinfo *a = res; a && j->n < MAX_ADDRS; a = a->ai_next) {
            uint8_t addr[16];
            int alen = j->family == AF_INET ? 4 : 16;
            if (a->ai_family != j->family) continue;
            if (j->family == AF_INET) {
                memcpy(addr, &((struct sockaddr_in *)a->ai_addr)->sin_addr, 4);
            } else {
                const struct in6_addr *v6 = &((struct sockaddr_in6 *)a->ai_addr)->sin6_addr;
                if (IN6_IS_ADDR_V4MAPPED(v6)) continue; /* Darwin's stand-in for an IPv4 address, not an AAAA */
                memcpy(addr, v6, 16);
            }
            int dup = 0;
            for (int i = 0; i < j->n; i++) dup |= memcmp(j->addrs[i], addr, (size_t)alen) == 0;
            if (!dup) memcpy(j->addrs[j->n++], addr, (size_t)alen);
        }
        freeaddrinfo(res);
        j->status = j->n ? 0 : 1;
    } else {
        j->status = r == EAI_NONAME ? 1 : 2;
#ifdef EAI_NODATA
        if (r == EAI_NODATA) j->status = 1;
#endif
    }
}

static void *gai_thread(void *arg) {
    GaiJob *j = arg;
    gai_lookup(j);
    pthread_mutex_lock(&mu);
    j->next = gai_done;
    gai_done = j;
    pthread_mutex_unlock(&mu);
    atomic_fetch_sub(&gai_running, 1);
    wake();
    return NULL;
}

static void start_gai(Pending *p) {
    p->gai = 2;
    if (!p->gai_type || atomic_load(&gai_running) >= MAX_GAI) return;
    GaiJob *j = calloc(1, sizeof *j);
    if (!j) return;
    j->token = p->token;
    j->family = p->gai_type == 1 ? AF_INET : AF_INET6;
    snprintf(j->name, sizeof j->name, "%s", p->name);
    atomic_fetch_add(&gai_running, 1);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, gai_thread, j) != 0) {
        atomic_fetch_sub(&gai_running, 1);
        free(j);
    } else {
        p->gai = 1;
    }
    pthread_attr_destroy(&a);
}

/* What the guest gets if nothing better comes: the first refusal. */
static void keep_refusal(Pending *p, const uint8_t *msg, uint32_t len) {
    if (p->fallback) return;
    uint8_t *copy = malloc(len);
    if (!copy) return;
    memcpy(copy, msg, len);
    put16(copy, p->guest_id);
    free(p->fallback);
    p->fallback = copy, p->fallback_len = len;
}

static void give_up(Pending *p) {
    if (p->fallback) send_to_guest(&p->q, p->fallback, p->fallback_len);
    else answer_here(p, 2, NULL, 0, 0); /* SERVFAIL */
    finish(p);
}

/* All servers asked and turned it down, and getaddrinfo is not asking: no use waiting. */
static void maybe_done(Pending *p) {
    if (p->used && p->next == p->nsrv && p->outstanding == 0 && p->gai != 1) {
        if (p->gai == 0) start_gai(p);
        if (p->gai != 1) give_up(p);
    }
}

static void send_next(Pending *p, int64_t now) {
    while (p->next < p->nsrv) {
        const struct sockaddr_storage *s = &p->srv[p->next++];
        int k = s->ss_family == AF_INET6;
        if (p->fd[k] < 0) {
            p->fd[k] = socket(k ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
            if (p->fd[k] >= 0) fcntl(p->fd[k], F_SETFL, O_NONBLOCK);
        }
        if (p->fd[k] >= 0 && sendto(p->fd[k], p->q.msg, p->q.len, 0, (const struct sockaddr *)s, s->ss_len) == (ssize_t)p->q.len) {
            p->outstanding++;
            p->next_send = now + STAGGER_MS;
            return;
        }
        /* no route that way (an IPv6 server on an IPv4-only network, say): the next one at once */
    }
    p->next_send = INT64_MAX;
}

static void new_query(Query q, int64_t now) {
    if (q.len < 12 || (q.msg[2] & 0x80)) { /* not a question */
        free(q.msg);
        return;
    }
    uint16_t id = get16(q.msg);
    Pending *p = NULL, *oldest = NULL;
    for (int i = 0; i < MAX_PENDING; i++) {
        Pending *e = &pend[i];
        if (!e->used) {
            if (!p) p = e;
            continue;
        }
        if (e->guest_id == id && e->q.guest_port == q.guest_port && e->q.len == q.len &&
            memcmp(e->q.guest_ip, q.guest_ip, 4) == 0 && memcmp(e->q.msg + 2, q.msg + 2, q.len - 2) == 0) {
            free(q.msg); /* the same query again: its answer is on the way */
            return;
        }
        if (!oldest || e->t0 < oldest->t0) oldest = e;
    }
    if (!p) {
        finish(oldest); /* the guest has long asked again */
        p = oldest;
    }
    int changed = atomic_exchange(&servers_stale, 0);
    if (changed) { /* another network: whoever answered on the last one may not */
        pthread_mutex_lock(&best_mu);
        have_best = 0;
        pthread_mutex_unlock(&best_mu);
        told_gai = 0;
    }
    if (changed || now - servers_time > SERVERS_FRESH_MS || nservers == 0) load_servers();
    memset(p, 0, sizeof *p);
    p->used = 1;
    p->q = q;
    p->fd[0] = p->fd[1] = -1;
    p->guest_id = id;
    p->t0 = now;
    p->token = next_token++;
    uint32_t qend = question_end(q.msg, q.len, p->name, sizeof p->name);
    p->qend = qend ? qend : 12;
    p->gai_type = gai_type_of(q.msg, qend, p->name);
    /* the one that answered last first, then the rest in order */
    if (have_best)
        for (int i = 0; i < nservers; i++)
            if (same_addr(&servers[i], &best)) p->srv[p->nsrv++] = servers[i];
    for (int i = 0; i < nservers; i++)
        if (!(have_best && same_addr(&servers[i], &best))) p->srv[p->nsrv++] = servers[i];
    p->up_id = (uint16_t)arc4random();
    put16(p->q.msg, p->up_id);
    send_next(p, now);
    if ((!have_best || told_gai) && p->gai_type) start_gai(p); /* ask the host's own fast resolver without delay */
    maybe_done(p);
}

static void got_reply(Pending *p, const uint8_t *m, uint32_t len, const struct sockaddr_storage *from) {
    int known = -1;
    for (int i = 0; i < p->next && known < 0; i++)
        if (same_addr(&p->srv[i], from)) known = i;
    if (known < 0 || len < 12 || get16(m) != p->up_id || !(m[2] & 0x80)) return;
    if (p->qend > 12 && (len < p->qend || memcmp(m + 4, p->q.msg + 4, 2) != 0 || memcmp(m + 12, p->q.msg + 12, p->qend - 12) != 0))
        return; /* not an answer to this question */
    if (p->answered & (1u << known)) return; /* the same server again (a repeated datagram): counted already */
    p->answered |= 1u << known;
    p->outstanding--;
    unsigned rcode = m[3] & 15;
    if (rcode == 2 || rcode == 5) { /* SERVFAIL, REFUSED: someone else may know */
        keep_refusal(p, m, len);
        p->next_send = 0;
        return;
    }
    if (!have_best || !same_addr(&best, from)) {
        char d[64];
        describe(from, d, sizeof d);
        fprintf(stderr, "hengevm: dns: answers come from %s\n", d);
        pthread_mutex_lock(&best_mu);
        best = *from, have_best = 1;
        pthread_mutex_unlock(&best_mu);
    }
    told_gai = 0;
    uint8_t *copy = malloc(len);
    if (!copy) return;
    memcpy(copy, m, len);
    put16(copy, p->guest_id);
    send_to_guest(&p->q, copy, len);
    free(copy);
    finish(p);
}

static void gai_finished(GaiJob *j) {
    for (int i = 0; i < MAX_PENDING; i++) {
        Pending *p = &pend[i];
        if (!p->used || p->token != j->token) continue;
        p->gai = 2;
        if (j->status == 2) {
            maybe_done(p);
            continue;
        }
        /* The host's own resolver has its say. With no address of that kind, an empty answer, not "no
           such name", so the other kind still works. */
        if (!told_gai) fprintf(stderr, "hengevm: dns: no DNS server answers; asking the host's own resolver\n");
        told_gai = 1;
        answer_here(p, 0, j->addrs, j->n, j->family == AF_INET ? 4 : 16);
        finish(p);
    }
    free(j);
}

static void *dns_thread(void *unused) {
    (void)unused;
    static struct pollfd pfds[1 + 2 * MAX_PENDING];
    static Pending *owner[1 + 2 * MAX_PENDING];
    static uint8_t buf[65536];
    for (;;) {
        int64_t now = now_ms(), wait = 1000;
        int n = 0;
        pfds[n++] = (struct pollfd){wake_pipe[0], POLLIN, 0};
        for (int i = 0; i < MAX_PENDING; i++) {
            Pending *p = &pend[i];
            if (!p->used) continue;
            for (int k = 0; k < 2; k++)
                if (p->fd[k] >= 0) owner[n] = p, pfds[n++] = (struct pollfd){p->fd[k], POLLIN, 0};
            int64_t due = p->t0 + GIVE_UP_MS;
            if (p->next_send < due) due = p->next_send;
            if (!p->gai && p->gai_type && p->t0 + GAI_AFTER_MS < due) due = p->t0 + GAI_AFTER_MS;
            if (due - now < wait) wait = due - now;
        }
        if (wait < 0) wait = 0;
        poll(pfds, (nfds_t)n, (int)wait);
        now = now_ms();
        if (pfds[0].revents & POLLIN) {
            char c[64];
            while (read(wake_pipe[0], c, sizeof c) > 0) {}
        }
        for (int i = 1; i < n; i++) {
            if (!(pfds[i].revents & (POLLIN | POLLERR))) continue;
            while (owner[i]->used) { /* an answer on its other socket may have settled it */
                struct sockaddr_storage from;
                socklen_t flen = sizeof from;
                ssize_t r = recvfrom(pfds[i].fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
                if (r < 0) break; /* nothing more, or an ICMP error from one server: others may still answer */
                got_reply(owner[i], buf, (uint32_t)r, &from);
            }
        }
        pthread_mutex_lock(&mu);
        Query in[MAX_INBOX];
        int nin = ninbox;
        memcpy(in, inbox, sizeof(Query) * (size_t)nin);
        ninbox = 0;
        GaiJob *done = gai_done;
        gai_done = NULL;
        pthread_mutex_unlock(&mu);
        while (done) {
            GaiJob *next = done->next;
            gai_finished(done);
            done = next;
        }
        for (int i = 0; i < nin; i++) new_query(in[i], now);
        for (int i = 0; i < MAX_PENDING; i++) {
            Pending *p = &pend[i];
            if (!p->used) continue;
            if (now - p->t0 >= GIVE_UP_MS) {
                give_up(p);
                continue;
            }
            if (now >= p->next_send) send_next(p, now);
            if (!p->gai && p->gai_type && now - p->t0 >= GAI_AFTER_MS) start_gai(p);
            maybe_done(p);
        }
    }
    return NULL;
}

/* ---- DNS over TCP (a thread per guest connection) ---- */

/* All of buf, read or written on the nonblocking fd before deadline (ms): 0, or -1. */
static int tcp_io(int fd, uint8_t *buf, size_t len, int writing, int64_t deadline) {
    while (len) {
        ssize_t r = writing ? send(fd, buf, len, 0) : recv(fd, buf, len, 0);
        if (r > 0) {
            buf += r, len -= (size_t)r;
            continue;
        }
        if (r == 0 || (errno != EAGAIN && errno != EINTR)) return -1;
        int64_t left = deadline - now_ms();
        struct pollfd p = {fd, writing ? POLLOUT : POLLIN, 0};
        if (left <= 0 || poll(&p, 1, (int)left) <= 0) return -1;
    }
    return 0;
}

/* The query (with its 2-byte length in front) over TCP to one server: its answer, malloc'd, or NULL. */
static uint8_t *ask_tcp(const struct sockaddr_storage *srv, const uint8_t *framed, uint32_t qlen, uint32_t *alen, int64_t deadline) {
    int fd = socket(srv->ss_family, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    uint8_t *ans = NULL;
    if (connect(fd, (const struct sockaddr *)srv, srv->ss_len) != 0) {
        struct pollfd p = {fd, POLLOUT, 0};
        int64_t left = deadline - now_ms();
        int err = 0;
        socklen_t el = sizeof err;
        if (errno != EINPROGRESS || left <= 0 || poll(&p, 1, (int)left) != 1 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0)
            goto out;
    }
    uint8_t hdr[2];
    if (tcp_io(fd, (uint8_t *)framed, 2 + qlen, 1, deadline) != 0 || tcp_io(fd, hdr, 2, 0, deadline) != 0) goto out;
    *alen = get16(hdr);
    if (*alen < 12 || !(ans = malloc(*alen))) goto out;
    if (tcp_io(fd, ans, *alen, 0, deadline) != 0 || get16(ans) != get16(framed + 2) || !(ans[2] & 0x80)) {
        free(ans);
        ans = NULL;
    }
out:
    close(fd);
    return ans;
}

/* The answer for a query the guest sent over TCP (framed: its length, then it), malloc'd: as for UDP, the
   servers in turn, then getaddrinfo, then the first refusal or SERVFAIL. */
static uint8_t *answer_tcp(const uint8_t *framed, uint32_t qlen, uint32_t *alen) {
    const uint8_t *msg = framed + 2;
    struct sockaddr_storage list[MAX_SERVERS + 1];
    int n = 0;
    pthread_mutex_lock(&best_mu);
    if (have_best) list[n++] = best;
    pthread_mutex_unlock(&best_mu);
    struct sockaddr_storage all[MAX_SERVERS];
    int nall = collect_servers(all);
    for (int i = 0; i < nall; i++)
        if (!n || !same_addr(&list[0], &all[i])) list[n++] = all[i];
    int64_t give_up = now_ms() + TCP_GIVE_UP_MS;
    uint8_t *refusal = NULL;
    uint32_t refusal_len = 0;
    for (int i = 0; i < n; i++) {
        int64_t now = now_ms(), deadline = now + TCP_TRY_MS;
        if (now >= give_up) break;
        if (deadline > give_up) deadline = give_up;
        uint32_t len;
        uint8_t *ans = ask_tcp(&list[i], framed, qlen, &len, deadline);
        if (!ans) continue;
        unsigned rcode = ans[3] & 15;
        if (rcode != 2 && rcode != 5) {
            free(refusal);
            *alen = len;
            return ans;
        }
        if (!refusal) refusal = ans, refusal_len = len;
        else free(ans);
    }
    char name[256];
    uint32_t qend = question_end(msg, qlen, name, sizeof name);
    int type = gai_type_of(msg, qend, name);
    uint8_t *out = malloc(ANSWER_MAX);
    if (!out) {
        *alen = refusal_len;
        return refusal;
    }
    if (type) {
        GaiJob j;
        memset(&j, 0, sizeof j);
        j.family = type == 1 ? AF_INET : AF_INET6;
        snprintf(j.name, sizeof j.name, "%s", name);
        gai_lookup(&j);
        if (j.status != 2) {
            free(refusal);
            *alen = (uint32_t)make_answer(out, get16(msg), msg, qend, type, 0, j.addrs, j.n, j.family == AF_INET ? 4 : 16);
            return out;
        }
    }
    if (refusal) {
        free(out);
        *alen = refusal_len;
        return refusal;
    }
    *alen = (uint32_t)make_answer(out, get16(msg), msg, qend ? qend : 12, type, 2, NULL, 0, 0); /* SERVFAIL */
    return out;
}

static void *tcp_conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    for (;;) { /* the guest may send several queries on one connection */
        uint8_t hdr[2];
        if (tcp_io(fd, hdr, 2, 0, now_ms() + TCP_IDLE_MS) != 0) break;
        uint32_t qlen = get16(hdr);
        uint8_t *framed = qlen >= 12 ? malloc(2 + qlen) : NULL;
        if (!framed) break;
        memcpy(framed, hdr, 2);
        if (tcp_io(fd, framed + 2, qlen, 0, now_ms() + TCP_IDLE_MS) != 0) {
            free(framed);
            break;
        }
        uint32_t alen = 0;
        uint8_t *ans = answer_tcp(framed, qlen, &alen);
        free(framed);
        if (!ans) break;
        uint8_t *out = malloc(2 + alen);
        int ok = out != NULL;
        if (ok) {
            put16(out, (uint16_t)alen);
            memcpy(out + 2, ans, alen);
            ok = tcp_io(fd, out, 2 + alen, 1, now_ms() + TCP_IDLE_MS) == 0;
        }
        free(out);
        free(ans);
        if (!ok) break;
    }
    close(fd);
    atomic_fetch_sub(&tcp_conns, 1);
    return NULL;
}

static void *tcp_accept_thread(void *unused) {
    (void)unused;
    for (;;) {
        int fd = accept(tcp_fd, NULL, NULL);
        if (fd < 0) {
            if (errno != EINTR && errno != ECONNABORTED) usleep(100000);
            continue;
        }
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        fcntl(fd, F_SETFL, O_NONBLOCK);
        pthread_t t;
        if (atomic_fetch_add(&tcp_conns, 1) >= TCP_CONNS || pthread_create(&t, NULL, tcp_conn_thread, (void *)(intptr_t)fd) != 0) {
            atomic_fetch_sub(&tcp_conns, 1);
            close(fd); /* so many at once: the guest asks again */
            continue;
        }
        pthread_detach(t);
    }
    return NULL;
}

/* The loopback server for DNS over TCP: its port, or 0. */
static uint16_t start_tcp(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in a = {.sin_len = sizeof a, .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t al = sizeof a;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 16) != 0 || getsockname(fd, (struct sockaddr *)&a, &al) != 0) {
        close(fd);
        return 0;
    }
    tcp_fd = fd;
    pthread_t t;
    if (pthread_create(&t, NULL, tcp_accept_thread, NULL) != 0) {
        close(fd);
        tcp_fd = -1;
        return 0;
    }
    pthread_detach(t);
    return ntohs(a.sin_port);
}

uint16_t hv_dns_tcp_port(void) { return tcp_port; }

int hv_dns_start(void (*d)(const uint8_t *frame, uint32_t len)) {
    const char *env = getenv("HV_DNS");
    if (env && strcmp(env, "off") == 0) return 0;
    if (wake_pipe[0] >= 0) return enabled ? 0 : -1;
    if (pipe(wake_pipe) != 0) return -1;
    fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);
    deliver = d;
    pthread_t t;
    if (pthread_create(&t, NULL, dns_thread, NULL) != 0) return -1;
    pthread_detach(t);
    enabled = 1;
    tcp_port = start_tcp();
    if (!tcp_port) fprintf(stderr, "hengevm: dns: no server for DNS over TCP: libslirp has it\n");
    return 0;
}
