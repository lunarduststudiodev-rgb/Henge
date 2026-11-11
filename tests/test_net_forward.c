/* The Mac's ports on the device (host/net_forward.c, through host/net_slirp.c's libslirp): a port the Mac listens on
   is the same port on every interface of the device. Connecting to it reaches the guest's port, data both ways; a
   low port, which on this test's 127.0.0.1 only root may have, says so for itself alone; a port no longer listened on refuses, while a connection it
   already took goes on; held (Henge off the screen, the Mac paused) nothing listens, and after it everything does
   again, across a fresh NAT too; a port this Mac itself has shows as taken, and opens once it is free and asked
   again; UDP both ways, also for a port the guest had already sent from; the guest refusing a connection refuses it
   to the client (a reset, not an accepted connection that ends); the helper's list (share.c's hook) replaces the
   standard set and comes back to it once the helper goes; and a round trip through it stays quick (item 3's
   guard). The guest is played here with raw Ethernet frames; clients connect over this Mac's loopback. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "loopback_only.h"
#include "machine.h"
#include "mdns.h"
#include "net_forward.h"
#include "net_slirp.h"
#include "pcie.h"
#include "share.h"
#include "virtio_net.h"

static int checks, failures;
static void check(const char *what, long long got, long long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got %lld (%#llx), want %lld (%#llx)\n", what, got, (unsigned long long)got, want, (unsigned long long)want);
    }
}

static HvMachine m;
static HvVirtioNet *n;
static const uint8_t guest_mac[6] = {0x02, 0x48, 0x45, 0x4e, 0x47, 0x45};
static const uint8_t guest_ip[4] = {10, 0, 2, 15}, host_ip[4] = {10, 0, 2, 2};
static uint8_t router_mac[6];
static _Atomic int arp_replies;

static uint16_t csum(const uint8_t *p, size_t len, uint32_t sum) {
    for (size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)p[i] << 8 | p[i + 1];
    if (len & 1) sum += (uint32_t)p[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8, p[1] = v & 0xff; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16), put16(p + 2, v & 0xffff); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }

static void send_arp(int op, const uint8_t *dst_mac, const uint8_t *target_ip) {
    uint8_t f[42] = {0};
    memcpy(f, op == 1 ? (const uint8_t[]){0xff, 0xff, 0xff, 0xff, 0xff, 0xff} : dst_mac, 6);
    memcpy(f + 6, guest_mac, 6);
    put16(f + 12, 0x0806);
    put16(f + 14, 1), put16(f + 16, 0x0800), f[18] = 6, f[19] = 4, put16(f + 20, (uint16_t)op);
    memcpy(f + 22, guest_mac, 6), memcpy(f + 28, guest_ip, 4);
    if (op == 2) memcpy(f + 32, dst_mac, 6);
    memcpy(f + 38, target_ip, 4);
    n->transmit(n->opaque, f, sizeof f);
}

static void ip_head(uint8_t *f, const uint8_t *dst, int proto, size_t len) {
    memcpy(f, router_mac, 6), memcpy(f + 6, guest_mac, 6), put16(f + 12, 0x0800);
    uint8_t *ip = f + 14;
    ip[0] = 0x45, put16(ip + 2, (uint16_t)(20 + len)), put16(ip + 4, 1), ip[8] = 64, ip[9] = (uint8_t)proto;
    memcpy(ip + 12, guest_ip, 4), memcpy(ip + 16, dst, 4);
    put16(ip + 10, csum(ip, 20, 0));
}

static void send_tcp(const uint8_t *dst, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags,
                     const void *data, size_t dlen) {
    uint8_t f[14 + 20 + 20 + 1500] = {0};
    ip_head(f, dst, 6, 20 + dlen);
    uint8_t *tcp = f + 34;
    put16(tcp, sport), put16(tcp + 2, dport), put32(tcp + 4, seq), put32(tcp + 8, ack);
    tcp[12] = 5 << 4, tcp[13] = flags, put16(tcp + 14, 65535);
    if (dlen) memcpy(tcp + 20, data, dlen);
    uint8_t pseudo[12];
    memcpy(pseudo, guest_ip, 4), memcpy(pseudo + 4, dst, 4), pseudo[8] = 0, pseudo[9] = 6;
    put16(pseudo + 10, (uint16_t)(20 + dlen));
    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2) sum += (uint32_t)pseudo[i] << 8 | pseudo[i + 1];
    put16(tcp + 16, csum(tcp, 20 + dlen, sum));
    n->transmit(n->opaque, f, (uint32_t)(54 + dlen));
}

static void send_udp(const uint8_t *dst, uint16_t sport, uint16_t dport, const void *data, size_t dlen) {
    uint8_t f[14 + 20 + 8 + 1500] = {0};
    ip_head(f, dst, 17, 8 + dlen);
    uint8_t *udp = f + 34;
    put16(udp, sport), put16(udp + 2, dport), put16(udp + 4, (uint16_t)(8 + dlen)); /* no checksum: allowed in IPv4 */
    memcpy(udp + 8, data, dlen);
    n->transmit(n->opaque, f, (uint32_t)(42 + dlen));
}

/* The guest's side: each port listens (echoing what comes), but refuse_port, whose SYNs it resets. */
typedef struct GuestConn {
    uint16_t port, peer;
    uint8_t from[4];
    uint32_t snd, rcv;
} GuestConn;
static GuestConn conns[64];
static int nconns;
static _Atomic int refuse_port, syns, refused, udp_in;
static _Atomic int udp_in_port; /* the guest port the last UDP datagram came to */

/* Network thread: every frame for the guest. */
static void watch(const uint8_t *f, uint32_t len, int to_guest) {
    if (!to_guest || len < 14) return;
    uint16_t type = get16(f + 12);
    if (type == 0x0806 && len >= 42) {
        int op = get16(f + 20);
        if (op == 2) memcpy(router_mac, f + 22, 6), arp_replies++;
        else if (op == 1 && !memcmp(f + 38, guest_ip, 4)) send_arp(2, f + 22, f + 28);
        return;
    }
    if (type != 0x0800 || len < 34 || memcmp(f + 30, guest_ip, 4)) return;
    const uint8_t *ip = f + 14, *src = ip + 12;
    uint32_t ihl = (uint32_t)(ip[0] & 15) * 4, ip_len = get16(ip + 2);
    if (ip_len > len - 14) return;
    if (ip[9] == 17 && ip_len >= ihl + 8) {
        const uint8_t *udp = ip + ihl;
        udp_in_port = get16(udp + 2);
        udp_in++;
        send_udp(src, get16(udp + 2), get16(udp), udp + 8, ip_len - ihl - 8); /* echoed */
        return;
    }
    if (ip[9] != 6 || ip_len < ihl + 20) return;
    const uint8_t *tcp = ip + ihl;
    uint16_t sport = get16(tcp), dport = get16(tcp + 2);
    uint8_t flags = tcp[13];
    uint32_t seq = get32(tcp + 4), hl = (uint32_t)(tcp[12] >> 4) * 4, dlen = ip_len - ihl - hl;
    if ((flags & 0x12) == 0x02) { /* a SYN */
        syns++;
        if (dport == refuse_port) {
            refused++;
            send_tcp(src, dport, sport, 0, seq + 1, 0x14, NULL, 0); /* RST+ACK: nothing listens */
            return;
        }
        if (nconns == 64) return;
        GuestConn *c = &conns[nconns++];
        *c = (GuestConn){dport, sport, {src[0], src[1], src[2], src[3]}, 7000, seq + 1};
        send_tcp(src, dport, sport, c->snd++, c->rcv, 0x12, NULL, 0);
        return;
    }
    GuestConn *c = NULL;
    for (int i = nconns - 1; i >= 0 && !c; i--)
        if (conns[i].port == dport && conns[i].peer == sport) c = &conns[i];
    if (!c || (flags & 0x04)) return;
    if (dlen && seq == c->rcv) {
        c->rcv += dlen;
        send_tcp(src, dport, sport, c->snd, c->rcv, 0x18, tcp + hl, dlen); /* echoed */
        c->snd += dlen;
    }
    if (flags & 0x01) { /* FIN: closed back */
        c->rcv++;
        send_tcp(src, dport, sport, c->snd++, c->rcv, 0x11, NULL, 0);
    }
}

static void tick(void) {
    hv_virtio_net_poll(n);
    hv_net_poll();
}

static void pause_ticking(int ms) {
    for (int i = 0; i < ms; i++) {
        tick();
        usleep(1000);
    }
}

/* Henge's log (stderr), kept in a file so that a check can read it; shown on stderr at the end as usual. */
static FILE *log_file;
static int real_stderr = -1;
static void log_capture(void) {
    fflush(stderr);
    log_file = tmpfile();
    if (!log_file) return;
    real_stderr = dup(2);
    dup2(fileno(log_file), 2);
}
static long log_at(void) {
    fflush(stderr);
    return log_file ? ftell(log_file) : 0;
}
/* How many lines since `from` hold `needle`. */
static int log_count(long from, const char *needle) {
    if (!log_file) return -1;
    fflush(stderr);
    long end = ftell(log_file);
    int k = 0;
    char line[512];
    fseek(log_file, from, SEEK_SET);
    while (ftell(log_file) < end && fgets(line, sizeof line, log_file)) k += strstr(line, needle) != NULL;
    fseek(log_file, end, SEEK_SET);
    return k;
}
static void log_release(void) {
    if (!log_file || real_stderr < 0) return;
    fflush(stderr);
    dup2(real_stderr, 2);
    rewind(log_file);
    char b[4096];
    size_t got;
    while ((got = fread(b, 1, sizeof b, log_file)) > 0) fwrite(b, 1, got, stderr);
}

/* What the network thread says of a port: its errno, 0 open, -1 not wanted. */
static int status_of(uint16_t port, int udp, int *held) {
    HvFwdStatus s[600];
    int k = hv_net_forward_status(s, 600, NULL, held);
    for (int i = 0; i < k; i++)
        if (s[i].port == port && s[i].udp == udp) return s[i].err;
    return -1;
}

static int wait_status(uint16_t port, int udp, int want) {
    for (int i = 0; i < 300; i++) {
        if (status_of(port, udp, NULL) == want) return 1;
        pause_ticking(10);
    }
    return 0;
}

/* A free port on this Mac (TCP and UDP both), for the test's own use. */
static uint16_t free_port(void) {
    for (;;) {
        int t = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = {.sin_family = AF_INET};
        socklen_t al = sizeof a;
        bind(t, (struct sockaddr *)&a, sizeof a);
        getsockname(t, (struct sockaddr *)&a, &al);
        int u = socket(AF_INET, SOCK_DGRAM, 0);
        int ok = bind(u, (struct sockaddr *)&a, sizeof a) == 0;
        close(t), close(u);
        if (ok) return ntohs(a.sin_port);
    }
}

/* A client's connection to 127.0.0.1:port: its socket, or -errno. */
static int dial(uint16_t port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        int e = errno;
        close(s);
        return -e;
    }
    return s;
}

/* Says text on s and reads back the guest's echo: 1 if it came, whole, within 3 s. */
static int echoes(int s, const char *text) {
    size_t want = strlen(text), got = 0;
    char buf[256];
    if (write(s, text, want) != (ssize_t)want) return 0;
    struct pollfd p = {s, POLLIN, 0};
    for (int i = 0; i < 3000 && got < want; i++) {
        tick();
        if (poll(&p, 1, 1) == 1) {
            ssize_t r = read(s, buf + got, sizeof buf - got);
            if (r <= 0) return 0;
            got += (size_t)r;
        }
    }
    return got == want && !memcmp(buf, text, want);
}

/* What a read on s gives within 3 s: bytes, 0 at its end, -errno, or -1000 if nothing came. */
static int read_end(int s) {
    char buf[64];
    struct pollfd p = {s, POLLIN, 0};
    for (int i = 0; i < 3000; i++) {
        tick();
        if (poll(&p, 1, 1) == 1) {
            ssize_t r = read(s, buf, sizeof buf);
            return r < 0 ? -errno : (int)r;
        }
    }
    return -1000;
}

static int udp_echoes(int u, const char *text) {
    size_t want = strlen(text);
    if (send(u, text, want, 0) != (ssize_t)want) return 0;
    char buf[256];
    struct pollfd p = {u, POLLIN, 0};
    for (int i = 0; i < 3000; i++) {
        tick();
        if (poll(&p, 1, 1) == 1) {
            ssize_t r = recv(u, buf, sizeof buf, 0);
            return r == (ssize_t)want && !memcmp(buf, text, want);
        }
    }
    return 0;
}

static int udp_client(uint16_t port) {
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    connect(u, (struct sockaddr *)&a, sizeof a); /* only answers from that port are taken */
    return u;
}

static void set_standard(const HvFwdPort *p, int k) { hv_net_ports_standard(p, k); }

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&m, 4 << 20) != 0) return 1;
    HvGic *gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    HvPcie *bus = hv_pcie_create(&m, gic, 0x10000000, 0x10000000, 0x50000000, 0x10000000, 32 + 4);
    n = hv_virtio_net_create(&m, bus, guest_mac);
    hv_net_watch = watch;
    setenv("HV_DNS", "off", 1);
    log_capture();

    /* Instead of this Mac's own sharing ports: free ones, and a low one. */
    uint16_t a = free_port(), b = free_port(), u = free_port(), low = 1022;
    while (b == a) b = free_port();
    while (u == a || u == b) u = free_port();
    HvFwdPort std[] = {{a, 0}, {b, 0}, {low, 0}, {u, 1}};
    set_standard(std, 4);
    hv_net_ports_loopback(1), hv_mdns_local_only(1); /* nothing here is reachable from the MacBook's network */
    check("the NAT starts", hv_net_attach(&m, n), 0);
    check("share.c's ports go to the forwards", hv_share_ports != NULL, 1);
    send_arp(1, NULL, host_ip);
    for (int i = 0; i < 300 && !arp_replies; i++) pause_ticking(10);
    check("the NAT answers ARP", arp_replies >= 1, 1);

    /* 1. A port the Mac listens on: a client reaches the guest's same port, both ways. */
    check("port a opens", wait_status(a, 0, 0), 1);
    int s = dial(a);
    check("a client connects to port a", s >= 0, 1);
    char wide[96];
    check("every port this test listens on is on 127.0.0.1 only", open_beyond_loopback(wide, sizeof wide), 0);
    if (wide[0]) printf("  %s\n", wide);
    {
        /* The check itself sees a listener on more than 127.0.0.1: one on the wildcard address, which macOS still
           only lets in through the loopback interface (IP_BOUND_IF), for no longer than the look. */
        int x = socket(AF_INET, SOCK_STREAM, 0);
        unsigned lo = if_nametoindex("lo0");
        struct sockaddr_in xa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
        int bound = x >= 0 && lo && setsockopt(x, IPPROTO_IP, IP_BOUND_IF, &lo, sizeof lo) == 0 &&
                    bind(x, (struct sockaddr *)&xa, sizeof xa) == 0 && listen(x, 1) == 0;
        check("and that check sees one that isn't", bound && open_beyond_loopback(NULL, 0) == 1, 1);
        if (x >= 0) close(x);
    }
    check("and the guest's port a echoes it", s >= 0 && echoes(s, "hello, Mac"), 1);
    check("the guest saw it on its own port a", nconns >= 1 && conns[nconns - 1].port == a, 1);
    check("from the device's side of the NAT (a client on the device itself)", nconns >= 1 && !memcmp(conns[nconns - 1].from, host_ip, 4), 1);

    /* 1b. Many clients at once (Finder opens several connections together): every one is taken at once, none
       waits for a SYN sent again a second later. */
    {
        enum { BURST = 24 };
        int bs[BURST], done = 0;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < BURST; i++) {
            bs[i] = socket(AF_INET, SOCK_STREAM, 0);
            fcntl(bs[i], F_SETFL, O_NONBLOCK);
            struct sockaddr_in ba = {.sin_family = AF_INET, .sin_port = htons(a), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
            connect(bs[i], (struct sockaddr *)&ba, sizeof ba);
        }
        for (int i = 0; i < BURST; i++) {
            struct pollfd p = {bs[i], POLLOUT, 0};
            int e = 1;
            socklen_t el = sizeof e;
            if (poll(&p, 1, 3000) == 1 && getsockopt(bs[i], SOL_SOCKET, SO_ERROR, &e, &el) == 0 && !e) done++;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        printf("  %d connections at once: all taken in %.1f ms\n", BURST, ms);
        check("24 clients at once all connect", done, BURST);
        check("within half a second (none waits to send its SYN again)", ms < 500, 1);
        pause_ticking(200);
        for (int i = 0; i < BURST; i++) close(bs[i]);
        pause_ticking(100);
    }

    /* 2. A low port. On the device Henge listens on the wildcard address, where macOS lets anyone have one; this
       test listens on 127.0.0.1 only (gate r2 M2), where macOS keeps the reserved ports for root. So here it can't
       open, and says why for that port alone; the others open all the same. */
    int low_want = geteuid() == 0 ? 0 : EACCES;
    check("low port 1022 says only root may have it on 127.0.0.1", wait_status(low, 0, low_want), 1);
    check("and the others open all the same", wait_status(b, 0, 0) && wait_status(u, 1, 0), 1);

    /* 3. The Mac stops listening on a: refused. The connection already open goes on. */
    HvFwdPort less[] = {{b, 0}, {low, 0}, {u, 1}};
    set_standard(less, 3);
    check("a is no longer wanted", wait_status(a, 0, -1), 1);
    int r = dial(a);
    check("a new client of a is refused", r, -ECONNREFUSED);
    if (r >= 0) close(r);
    check("the open connection on a still echoes", s >= 0 && echoes(s, "still here"), 1);
    if (s >= 0) close(s);

    /* 4. Held: nothing listens; then everything does again (the log says so in one line, not a line a port). */
    long log_from = log_at();
    hv_net_forwards_hold(1);
    int held = 0;
    for (int i = 0; i < 300 && status_of(b, 0, &held) != -1; i++) pause_ticking(10);
    status_of(b, 0, &held);
    check("held: b closed", status_of(b, 0, NULL), -1);
    check("and said to be held", held, 1);
    r = dial(b);
    check("a client of b is refused while held", r, -ECONNREFUSED);
    if (r >= 0) close(r);
    hv_net_forwards_hold(0);
    check("released: b opens", wait_status(b, 0, 0), 1);
    check("the log: no line a port on the way back", log_count(log_from, "goes to the Mac"), 0);
    check("one line for them all", log_count(log_from, "to it again"), 1);
    r = dial(b);
    check("and a client of b reaches the guest", r >= 0 && echoes(r, "back"), 1);
    if (r >= 0) close(r);

    /* 5. A fresh NAT (the cable out and in) while held, and while not: open again either way. */
    hv_net_forwards_hold(1);
    check("held again", wait_status(b, 0, -1), 1);
    hv_virtio_net_set_carrier(n, 0, hv_virtio_net_now() - 4000000000ull);
    tick();
    hv_virtio_net_set_carrier(n, 1, hv_virtio_net_now() - 2000000000ull);
    pause_ticking(200);
    check("a fresh NAT while held: still nothing", status_of(b, 0, NULL), -1);
    r = dial(b);
    check("and b refused", r, -ECONNREFUSED);
    if (r >= 0) close(r);
    hv_net_forwards_hold(0);
    check("released after it: b opens", wait_status(b, 0, 0), 1);
    send_arp(1, NULL, host_ip);
    pause_ticking(50);
    r = dial(b);
    check("a client of b reaches the guest through the fresh NAT", r >= 0 && echoes(r, "fresh"), 1);
    if (r >= 0) close(r);
    hv_virtio_net_set_carrier(n, 0, hv_virtio_net_now() - 4000000000ull);
    tick();
    hv_virtio_net_set_carrier(n, 1, hv_virtio_net_now() - 2000000000ull);
    pause_ticking(200);
    check("a fresh NAT, not held: b open on it", wait_status(b, 0, 0), 1);
    send_arp(1, NULL, host_ip);
    pause_ticking(50);
    r = dial(b);
    check("and reached", r >= 0 && echoes(r, "again"), 1);
    if (r >= 0) close(r);

    /* 6. A port this Mac itself has: taken, said why; once free and the list is set again, open. */
    uint16_t c = free_port();
    int mine = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ma = {.sin_family = AF_INET, .sin_port = htons(c), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    check("the test takes port c", bind(mine, (struct sockaddr *)&ma, sizeof ma) == 0 && listen(mine, 1) == 0, 1);
    HvFwdPort with_c[] = {{b, 0}, {c, 0}, {low, 0}, {u, 1}};
    set_standard(with_c, 4);
    check("c can't be the Mac's: the device uses it", wait_status(c, 0, EADDRINUSE), 1);
    char line[2048];
    hv_net_forward_line(line, sizeof line);
    printf("  status: %s\n", line);
    check("the status line says so", strstr(line, "the device itself uses it") != NULL, 1);
    close(mine);
    set_standard(with_c, 4); /* asked again (as a list that changes, or Henge coming back, does) */
    check("free now: c opens", wait_status(c, 0, 0), 1);
    r = dial(c);
    check("and reaches the guest", r >= 0 && echoes(r, "c"), 1);
    if (r >= 0) close(r);

    /* 7. UDP both ways; and for a port the guest had already sent from (a NAT socket of its own for it). */
    check("UDP u opens", wait_status(u, 1, 0), 1);
    int uc = udp_client(u);
    check("a datagram to u reaches the guest's u and its answer comes back from u", udp_echoes(uc, "udp ping"), 1);
    check("on the guest's u", udp_in_port, u);
    close(uc);
    uint16_t v = free_port();
    int far = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in fa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t fl = sizeof fa;
    bind(far, (struct sockaddr *)&fa, sizeof fa);
    getsockname(far, (struct sockaddr *)&fa, &fl);
    /* The guest sends from v before v is forwarded, twice: the NAT makes v a socket of its own, and the second
       finds it again (libslirp then keeps it as the one to try first for v). */
    struct pollfd fp = {far, POLLIN, 0};
    int arrived = 0;
    char fb[64];
    for (int k = 0; k < 2; k++) {
        send_udp(host_ip, v, ntohs(fa.sin_port), "first", 5);
        int got = 0;
        for (int i = 0; i < 3000 && !got; i++) tick(), got = poll(&fp, 1, 1) == 1;
        if (got) recv(far, fb, sizeof fb, 0), arrived++;
    }
    check("the guest's datagrams from v went out", arrived, 2);
    HvFwdPort with_v[] = {{b, 0}, {low, 0}, {u, 1}, {v, 1}};
    set_standard(with_v, 4);
    check("UDP v opens", wait_status(v, 1, 0), 1);
    uc = udp_client(v);
    check("a datagram to v: answered from v (the guest's earlier socket gave way)", udp_echoes(uc, "v ping"), 1);
    close(uc);
    send_udp(host_ip, v, ntohs(fa.sin_port), "later", 5);
    struct sockaddr_in src;
    socklen_t srcl = sizeof src;
    ssize_t fr = -1;
    for (int i = 0; i < 3000 && fr < 0; i++) {
        tick();
        if (poll(&fp, 1, 1) == 1) fr = recvfrom(far, fb, sizeof fb, 0, (struct sockaddr *)&src, &srcl);
    }
    check("and what the guest sends from v now leaves the device from v", fr == 5 && ntohs(src.sin_port) == v, 1);

    /* 7b. A UDP port that can't be forwarded (the device has it): the guest's own flow from it goes on, its answers
       still reach the guest through the NAT's socket (that socket is given up only for a forward that opened). */
    uint16_t w = free_port();
    int wmine = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in wa = {.sin_family = AF_INET, .sin_port = htons(w), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    check("the test takes UDP w", bind(wmine, (struct sockaddr *)&wa, sizeof wa), 0);
    send_udp(host_ip, w, ntohs(fa.sin_port), "from w", 6);
    struct sockaddr_in nat;
    socklen_t natl = sizeof nat;
    fr = -1;
    for (int i = 0; i < 3000 && fr < 0; i++) {
        tick();
        if (poll(&fp, 1, 1) == 1) fr = recvfrom(far, fb, sizeof fb, 0, (struct sockaddr *)&nat, &natl);
    }
    check("the guest's datagram from w went out", fr, 6);
    HvFwdPort with_w[] = {{b, 0}, {low, 0}, {u, 1}, {v, 1}, {w, 1}};
    set_standard(with_w, 5);
    check("w can't be the Mac's: the device uses it", wait_status(w, 1, EADDRINUSE), 1);
    int before = udp_in;
    sendto(far, "answer", 6, 0, (struct sockaddr *)&nat, natl);
    int answered = 0;
    for (int i = 0; i < 3000 && !answered; i++) tick(), pause_ticking(1), answered = udp_in > before && udp_in_port == w;
    check("an answer to the guest's flow from w still reaches the guest's w", answered, 1);
    close(wmine);
    close(far);

    /* 8. The guest refuses a connection (nothing listens there): the client is refused too, by a reset. */
    refuse_port = b;
    r = dial(b); /* the device accepts at once, before the guest has answered */
    int end = r >= 0 ? read_end(r) : r;
    check("the guest refused it", refused >= 1, 1);
    check("the client gets a reset, not an end", end, -ECONNRESET);
    if (r >= 0) close(r);
    refuse_port = 0;

    /* 9. The helper's list replaces the standard set, and once it goes, the standard set is back. */
    uint16_t h = free_port();
    uint16_t hp[] = {h};
    uint8_t hu[] = {0};
    hv_share_ports(hp, hu, 1);
    check("the helper's port opens", wait_status(h, 0, 0), 1);
    check("the standard set's b closes", wait_status(b, 0, -1), 1);
    int from_helper = 0;
    hv_net_forward_status(NULL, 0, &from_helper, NULL);
    check("the list is the helper's", from_helper, 1);
    r = dial(h);
    check("a client of the helper's port reaches the guest", r >= 0 && echoes(r, "helper"), 1);
    if (r >= 0) close(r);
    hv_share_ports(NULL, NULL, -1);
    check("the helper gone: b again", wait_status(b, 0, 0), 1);
    check("and its port closed", wait_status(h, 0, -1), 1);

    /* 10. Item 3's guard: a round trip through the forward stays quick (the NAT's loop adds no wait of its own). */
    s = dial(b);
    double t[60];
    int ok = s >= 0;
    for (int i = 0; i < 60 && ok; i++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        ok = echoes(s, "x");
        clock_gettime(CLOCK_MONOTONIC, &t1);
        t[i] = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    }
    check("60 round trips", ok, 1);
    if (ok) {
        qsort(t, 60, sizeof *t, cmp_double);
        printf("  round trip through the forward: median %.2f ms, slowest %.2f ms\n", t[30], t[59]);
        check("its median is under 5 ms", t[30] < 5.0, 1);
    }
    if (s >= 0) close(s);

    /* 11. The cable goes back in and the NAT can't be made: every port says the network is down (not what it was),
       and the Mac's mDNS still goes on (the Mac names itself; that is heard). Once it can, a fresh NAT, every port
       open on it. */
    check("before: UDP u is open", wait_status(u, 1, 0), 1);
    check("every port this test listens on is still on 127.0.0.1 only", open_beyond_loopback(wide, sizeof wide), 0);
    if (wide[0]) printf("  %s\n", wide);
    hv_net_test_fail_start = 1;
    hv_virtio_net_set_carrier(n, 0, hv_virtio_net_now() - 4000000000ull);
    pause_ticking(50);
    hv_virtio_net_set_carrier(n, 1, hv_virtio_net_now() - 2000000000ull);
    check("no NAT: b says the network is down", wait_status(b, 0, ENETDOWN), 1);
    check("and the low port", wait_status(low, 0, ENETDOWN), 1);
    check("and UDP u", wait_status(u, 1, ENETDOWN), 1);
    check("and UDP v", wait_status(v, 1, ENETDOWN), 1);
    r = dial(b);
    check("a client of b is refused", r, -ECONNREFUSED);
    if (r >= 0) close(r);
    {
        char name[64], want[80], got[300];
        snprintf(name, sizeof name, "fwdtest-%d", (int)getpid());
        snprintf(want, sizeof want, "%s.local", name);
        uint8_t q[128] = {0}, *p = q + 12;
        put16(q + 2, 0x8400), put16(q + 6, 1); /* a response, one answer */
        *p++ = (uint8_t)strlen(name), memcpy(p, name, strlen(name)), p += strlen(name);
        *p++ = 5, memcpy(p, "local", 5), p += 5, *p++ = 0;
        put16(p, 1), put16(p + 2, 0x8001), put16(p + 6, 120), put16(p + 8, 4), memcpy(p + 10, guest_ip, 4), p += 14;
        int heard = 0;
        for (int t = 0; t < 100 && !heard; t++) {
            if (t % 10 == 0) send_udp(host_ip, 5353, 5353, q, (size_t)(p - q));
            pause_ticking(100);
            hv_mdns_status(got, sizeof got, NULL, NULL, NULL, NULL, NULL);
            heard = !strcmp(got, want);
        }
        check("the Mac's mDNS is still heard with no NAT", heard, 1);
    }
    hv_net_test_fail_start = 0;
    hv_virtio_net_set_carrier(n, 0, hv_virtio_net_now() - 4000000000ull);
    pause_ticking(50);
    hv_virtio_net_set_carrier(n, 1, hv_virtio_net_now() - 2000000000ull);
    check("the NAT again: b opens", wait_status(b, 0, 0), 1);
    check("and the low port as before", wait_status(low, 0, low_want), 1);
    check("and UDP u", wait_status(u, 1, 0), 1);
    check("and UDP v", wait_status(v, 1, 0), 1);
    r = dial(b);
    check("and a client of b reaches the guest", r >= 0 && echoes(r, "up"), 1);
    if (r >= 0) close(r);

    /* The Mac stops: every listener goes, for good. */
    hv_net_forwards_close();
    check("closed: b goes", wait_status(b, 0, -1), 1);
    r = dial(b);
    check("and is refused", r, -ECONNREFUSED);
    if (r >= 0) close(r);

    log_release();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
