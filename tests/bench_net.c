/* How fast a download goes through the guest's network: Henge's own path (libslirp's NAT on its thread, the
   queues in host/net_slirp.c, the virtio-net card writing frames into receive buffers in guest memory), with
   the guest played here: a driver posting receive buffers and a TCP receiver acknowledging what arrives.

   A server on this Mac's loopback sends BENCH_MB megabytes; the guest fetches them from 10.0.2.2 as macOS
   would from the internet, on one connection or BENCH_CONNS at once. The guest answers each burst it takes
   after BENCH_LAG_MS (a list: one run each), which stands for the time a Mac on the iPad takes from the
   card's interrupt to its TCP acknowledgement going out. Each run prints the throughput, and the network's
   own report line (hv_net_report), as the perf log shows it.

   BENCH_DNS=1 also times a lookup through 10.0.2.3 (host/net_dns.c) against the host's own getaddrinfo.

   make build/bench_net && build/bench_net
   BENCH_LAG_MS=0,1,5,20 BENCH_MB=64 BENCH_CONNS=1 HV_NET_MTU=1500 build/bench_net */
#include <arpa/inet.h>
#include <netdb.h>
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
#include "virtio_net.h"

#define RAM_PA 0x40000000ull
#define RAM_SIZE (96u << 20)
#define RXQ 0
#define MAX_CONNS 16

static uint8_t *ram;
static HvMachine m;
static HvVirtioNet *n;
static int qsize = 256;
static uint32_t bufsize;
static uint16_t last_used;
static const uint8_t guest_mac[6] = {0x02, 0x48, 0x45, 0x4e, 0x47, 0x45};
static const uint8_t guest_ip[4] = {10, 0, 2, 15}, host_ip[4] = {10, 0, 2, 2}, dns_ip[4] = {10, 0, 2, 3};
static uint8_t router_mac[6];
static int have_router;

static void *at(uint64_t pa) { return ram + (pa - RAM_PA); }
static uint64_t desc_pa(void) { return RAM_PA + 0x10000; }
static uint64_t avail_pa(void) { return RAM_PA + 0x20000; }
static uint64_t used_pa(void) { return RAM_PA + 0x30000; }
static uint64_t buf_pa(int i) { return RAM_PA + 0x100000 + (uint64_t)i * bufsize; }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8, p[1] = v & 0xff; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16), put16(p + 2, v & 0xffff); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }
static uint16_t csum(const uint8_t *p, size_t len, uint32_t sum) {
    for (size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)p[i] << 8 | p[i + 1];
    if (len & 1) sum += (uint32_t)p[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* The driver: buffer i back on the receive queue. */
static void post(int i) {
    uint8_t *d = at(desc_pa() + 16u * i);
    uint64_t addr = buf_pa(i);
    uint16_t flags = 2, next = 0;
    memcpy(d, &addr, 8);
    memcpy(d + 8, &bufsize, 4);
    memcpy(d + 12, &flags, 2);
    memcpy(d + 14, &next, 2);
    uint16_t idx;
    memcpy(&idx, at(avail_pa() + 2), 2);
    uint16_t slot = (uint16_t)i;
    memcpy(at(avail_pa() + 4 + 2u * (idx % qsize)), &slot, 2);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    idx++;
    memcpy(at(avail_pa() + 2), &idx, 2);
}

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

/* A segment from the guest; on a SYN, MSS and window-scale options. */
static void send_tcp(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags, uint16_t win,
                     const char *data, uint16_t mss, int wscale) {
    size_t dlen = data ? strlen(data) : 0, olen = (flags & 0x02) ? (wscale >= 0 ? 8 : 4) : 0;
    uint8_t f[14 + 20 + 20 + 8 + 64] = {0};
    memcpy(f, router_mac, 6), memcpy(f + 6, guest_mac, 6), put16(f + 12, 0x0800);
    uint8_t *ip = f + 14, *tcp = ip + 20;
    size_t tlen = 20 + olen + dlen;
    ip[0] = 0x45, put16(ip + 2, (uint16_t)(20 + tlen)), put16(ip + 4, 1), ip[8] = 64, ip[9] = 6;
    memcpy(ip + 12, guest_ip, 4), memcpy(ip + 16, host_ip, 4);
    put16(ip + 10, csum(ip, 20, 0));
    put16(tcp, sport), put16(tcp + 2, dport), put32(tcp + 4, seq), put32(tcp + 8, ack);
    tcp[12] = (uint8_t)((20 + olen) / 4) << 4, tcp[13] = flags, put16(tcp + 14, win);
    if (olen) {
        tcp[20] = 2, tcp[21] = 4, put16(tcp + 22, mss);
        if (olen == 8) tcp[24] = 1, tcp[25] = 3, tcp[26] = 3, tcp[27] = (uint8_t)wscale;
    }
    if (dlen) memcpy(tcp + 20 + olen, data, dlen);
    uint8_t pseudo[12];
    memcpy(pseudo, guest_ip, 4), memcpy(pseudo + 4, host_ip, 4), pseudo[8] = 0, pseudo[9] = 6;
    put16(pseudo + 10, (uint16_t)tlen);
    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2) sum += (uint32_t)pseudo[i] << 8 | pseudo[i + 1];
    put16(tcp + 16, csum(tcp, tlen, sum));
    n->transmit(n->opaque, f, (uint32_t)(14 + 20 + tlen));
}

/* ---- the guest's TCP: one receiver per connection ---- */
typedef struct Conn {
    uint16_t port;
    int state; /* 0 SYN sent, 1 established, 2 done */
    uint32_t iss, rcv_nxt;
    int scale;        /* the shift the NAT agreed to (-1: none, so the window is 64 KB at most) */
    uint64_t bytes, beyond_window, out_of_order;
    int unacked;      /* segments since the last acknowledgement */
} Conn;

static Conn conns[MAX_CONNS];
static int nconns;
static double lag;

/* Acknowledgements waiting out the guest's lag. */
#define ACKQ 65536
typedef struct Ack { double due; int c; uint32_t ack; } Ack;
static Ack ackq[ACKQ];
static unsigned ack_head, ack_tail;

static uint16_t window_field(Conn *c) {
    if (c->scale < 0) return 65535;
    uint32_t w = (8u << 20) >> c->scale; /* 8 MB, as macOS would offer with autotuning */
    return w > 65535 ? 65535 : (uint16_t)w;
}

static void ack_later(int ci) {
    Conn *c = &conns[ci];
    c->unacked = 0;
    if (ack_tail - ack_head < ACKQ) ackq[ack_tail++ % ACKQ] = (Ack){now_s() + lag, ci, c->rcv_nxt};
}

static uint16_t server_port;
static uint32_t want_window_scale = 7;

static void send_ack(Conn *c, uint32_t ack) {
    send_tcp(c->port, server_port, c->iss + 2, ack, 0x10, window_field(c), NULL, 0, -1);
}

static void flush_acks(void) {
    double t = now_s();
    while (ack_head != ack_tail && ackq[ack_head % ACKQ].due <= t) {
        Conn *c = &conns[ackq[ack_head % ACKQ].c];
        send_ack(c, ackq[ack_head % ACKQ].ack);
        ack_head++;
    }
}

static _Atomic int dns_answers;
static double dns_answered_at;

/* A frame the card wrote into a receive buffer. */
static void take(const uint8_t *f, uint32_t len, int *touched) {
    if (len < 14) return;
    uint16_t type = get16(f + 12);
    if (type == 0x0806 && len >= 42) {
        int op = get16(f + 20);
        if (op == 2) memcpy(router_mac, f + 22, 6), have_router = 1;
        else if (op == 1 && !memcmp(f + 38, guest_ip, 4)) send_arp(2, f + 22, f + 28);
        return;
    }
    if (type != 0x0800 || len < 34) return;
    uint32_t ihl = (f[14] & 15) * 4u, iplen = get16(f + 16);
    if (f[23] == 17 && !memcmp(f + 26, dns_ip, 4)) { /* a DNS answer */
        dns_answered_at = now_s();
        dns_answers++;
        return;
    }
    if (f[23] != 6) return;
    const uint8_t *tcp = f + 14 + ihl;
    uint32_t thl = (tcp[12] >> 4) * 4u, payload = iplen - ihl - thl;
    uint16_t dport = get16(tcp + 2);
    uint8_t flags = tcp[13];
    for (int i = 0; i < nconns; i++) {
        Conn *c = &conns[i];
        if (c->port != dport) continue;
        uint32_t seq = get32(tcp + 4);
        if (c->state == 0 && (flags & 0x12) == 0x12) {
            c->rcv_nxt = seq + 1;
            c->scale = -1;
            for (uint32_t o = 20; o < thl;) { /* the NAT's options: does it scale windows? */
                uint8_t kind = tcp[o];
                if (kind == 0) break;
                if (kind == 1) { o++; continue; }
                if (o + 1 >= thl || tcp[o + 1] < 2) break;
                if (kind == 3) c->scale = (int)want_window_scale;
                o += tcp[o + 1];
            }
            c->state = 1;
            send_ack(c, c->rcv_nxt);
            send_tcp(c->port, server_port, c->iss + 1, c->rcv_nxt, 0x18, window_field(c), "x", 0, -1);
            return;
        }
        if (c->state != 1) return;
        if (seq != c->rcv_nxt) { /* lost something: say what we have (the NAT resends) */
            c->out_of_order++;
            send_ack(c, c->rcv_nxt);
            return;
        }
        uint32_t window = c->scale < 0 ? 65535 : 8u << 20;
        if (payload > window) c->beyond_window++;
        c->rcv_nxt += payload;
        c->bytes += payload;
        if (payload) c->unacked++;
        if (flags & 0x01) {
            c->rcv_nxt++;
            c->state = 2;
            send_ack(c, c->rcv_nxt);
            return;
        }
        if (c->unacked >= 2) ack_later(i); /* every second segment, as macOS does */
        else if (payload) touched[i] = 1;
        return;
    }
}

/* One pass of the guest: the device's tick, then what it delivered, as an interrupt would be taken. */
static void guest_pass(void) {
    hv_virtio_net_poll(n);
    hv_net_poll();
    int touched[MAX_CONNS] = {0};
    for (;;) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint16_t idx;
        memcpy(&idx, at(used_pa() + 2), 2);
        if (idx == last_used) break;
        uint32_t id, len;
        memcpy(&id, at(used_pa() + 4 + 8u * (last_used % qsize)), 4);
        memcpy(&len, at(used_pa() + 8 + 8u * (last_used % qsize)), 4);
        last_used++;
        if (len > 12) take(at(buf_pa((int)id) + 12), len - 12, touched);
        post((int)id);
    }
    for (int i = 0; i < nconns; i++) /* the end of the burst: acknowledge what is left */
        if (touched[i] && conns[i].unacked && conns[i].state == 1) ack_later(i);
    flush_acks();
}

/* ---- the server on this Mac ---- */
static int listener;
static uint64_t send_bytes;

static void *serve(void *arg) {
    int s = (int)(intptr_t)arg;
    static char chunk[1 << 18];
    char x;
    if (read(s, &x, 1) != 1) { close(s); return NULL; }
    for (uint64_t left = send_bytes; left;) {
        ssize_t w = write(s, chunk, left < sizeof chunk ? left : sizeof chunk);
        if (w <= 0) break;
        left -= (uint64_t)w;
    }
    close(s);
    return NULL;
}

static void *acceptor(void *arg) {
    (void)arg;
    for (;;) {
        int s = accept(listener, NULL, NULL);
        if (s < 0) return NULL;
        pthread_t t;
        pthread_create(&t, NULL, serve, (void *)(intptr_t)s);
        pthread_detach(t);
    }
}

static uint16_t next_port = 41000;

/* One download over `count` connections at once with the guest's lag; MB/s. */
static double run(int count, double lag_ms, double mb) {
    lag = lag_ms / 1000.0;
    send_bytes = (uint64_t)(mb * 1e6 / count);
    nconns = count;
    for (int i = 0; i < count; i++) {
        conns[i] = (Conn){.port = next_port++, .iss = 1000u * (uint32_t)(i + 1), .scale = -1};
        send_tcp(conns[i].port, server_port, conns[i].iss, 0, 0x02, 65535, NULL, (uint16_t)(hv_net_mtu() - 40), (int)want_window_scale);
    }
    char line[512];
    hv_net_report(line, sizeof line); /* from here on */
    double t0 = now_s(), last = t0;
    for (;;) {
        guest_pass();
        int done = 1;
        for (int i = 0; i < count; i++) done &= conns[i].state == 2;
        if (done) break;
        double t = now_s();
        if (t - last > 60) {
            printf("  no progress\n");
            break;
        }
        uint64_t sum = 0;
        for (int i = 0; i < count; i++) sum += conns[i].bytes;
        static uint64_t seen;
        if (sum != seen) seen = sum, last = t;
    }
    double secs = now_s() - t0;
    uint64_t total = 0, ooo = 0, beyond = 0;
    for (int i = 0; i < count; i++) total += conns[i].bytes, ooo += conns[i].out_of_order, beyond += conns[i].beyond_window;
    hv_net_report(line, sizeof line);
    printf("lag %5.1f ms, %d connection%s: %7.2f MB/s (%.1f MB in %.2f s)%s%s\n    %s\n", lag_ms, count, count == 1 ? " " : "s",
           (double)total / secs / 1e6, (double)total / 1e6, secs, ooo ? ", out of order/lost seen" : "",
           conns[0].scale < 0 ? ", no window scaling" : ", window scaling", line);
    /* Settle: nothing of this run left for the next. */
    for (double end = now_s() + 0.2; now_s() < end;) guest_pass();
    return (double)total / secs / 1e6;
}

static void time_dns(void) {
    const char *name = getenv("BENCH_DNS_NAME") ? getenv("BENCH_DNS_NAME") : "www.apple.com";
    uint8_t q[512] = {0};
    size_t ql = 12;
    put16(q, 0x4242), put16(q + 2, 0x0100), put16(q + 4, 1);
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        q[ql++] = (uint8_t)l;
        memcpy(q + ql, p, l), ql += l;
        p += l + (dot ? 1 : 0);
    }
    q[ql++] = 0, put16(q + ql, 1), put16(q + ql + 2, 1), ql += 4;
    uint8_t f[600] = {0};
    memcpy(f, router_mac, 6), memcpy(f + 6, guest_mac, 6), put16(f + 12, 0x0800);
    uint8_t *ip = f + 14, *udp = ip + 20;
    ip[0] = 0x45, put16(ip + 2, (uint16_t)(28 + ql)), ip[8] = 64, ip[9] = 17;
    memcpy(ip + 12, guest_ip, 4), memcpy(ip + 16, dns_ip, 4);
    put16(ip + 10, csum(ip, 20, 0));
    put16(udp, 5353 + 1000), put16(udp + 2, 53), put16(udp + 4, (uint16_t)(8 + ql));
    memcpy(udp + 8, q, ql);
    int before = dns_answers;
    double t0 = now_s();
    n->transmit(n->opaque, f, (uint32_t)(14 + 28 + ql));
    while (dns_answers == before && now_s() - t0 < 6) guest_pass();
    double through = dns_answers > before ? (dns_answered_at - t0) * 1000 : -1;
    t0 = now_s();
    struct addrinfo *ai = NULL, hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    int rc = getaddrinfo(name, "443", &hints, &ai);
    double host = (now_s() - t0) * 1000;
    if (ai) freeaddrinfo(ai);
    printf("DNS %s: through 10.0.2.3 %.1f ms, the host's getaddrinfo just after %.1f ms%s\n", name, through, host, rc ? " (failed)" : "");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getenv("BENCH_QSIZE")) qsize = atoi(getenv("BENCH_QSIZE"));
    bufsize = (uint32_t)((12 + 14 + hv_net_mtu() + 2047) & ~2047);
    if ((uint64_t)qsize * bufsize + 0x100000 > RAM_SIZE) {
        printf("queue too big for the bench's memory\n");
        return 1;
    }
    if (getenv("BENCH_WSCALE")) want_window_scale = (uint32_t)atoi(getenv("BENCH_WSCALE"));
    if (hv_machine_init(&m, 4 << 20) != 0) return 1;
    ram = calloc(1, RAM_SIZE);
    hv_machine_add_ram(&m, RAM_PA, RAM_SIZE, ram, 0);
    HvGic *gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    HvPcie *bus = hv_pcie_create(&m, gic, 0x10000000, 0x10000000, 0x50000000, 0x10000000, 32 + 4);
    n = hv_virtio_net_create(&m, bus, guest_mac);
    hv_net_ports_standard(NULL, 0); /* none of this Mac's own ports: not what this tests */
    hv_net_ports_loopback(1), hv_mdns_local_only(1); /* nothing here is reachable from the MacBook's network */
    if (hv_net_attach(&m, n) != 0) return 1;
    HvVirtq *vq = &n->vio.q[RXQ];
    vq->size = (uint16_t)qsize, vq->desc = desc_pa(), vq->avail = avail_pa(), vq->used = used_pa(), vq->enabled = 1;
    n->vio.status |= 4; /* DRIVER_OK */
    for (int i = 0; i < qsize; i++) post(i);

    listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t sl = sizeof sa;
    bind(listener, (struct sockaddr *)&sa, sizeof sa);
    listen(listener, 64);
    getsockname(listener, (struct sockaddr *)&sa, &sl);
    server_port = ntohs(sa.sin_port);
    pthread_t t;
    pthread_create(&t, NULL, acceptor, NULL);

    send_arp(1, NULL, host_ip);
    for (double end = now_s() + 3; !have_router && now_s() < end;) guest_pass();
    if (!have_router) {
        printf("the NAT never answered ARP\n");
        return 1;
    }
    char wide[96];
    if (open_beyond_loopback(wide, sizeof wide)) { /* gate r2 M2: a benchmark is no reason to open the MacBook up */
        printf("this benchmark listens beyond 127.0.0.1: %s\n", wide);
        return 1;
    }
    printf("MTU %d, receive queue %d buffers of %u bytes\n", hv_net_mtu(), qsize, bufsize);
    if (getenv("BENCH_DNS")) time_dns();
    double mb = getenv("BENCH_MB") ? atof(getenv("BENCH_MB")) : 32;
    int count = getenv("BENCH_CONNS") ? atoi(getenv("BENCH_CONNS")) : 1;
    if (count < 1) count = 1;
    if (count > MAX_CONNS) count = MAX_CONNS;
    char lags[256];
    snprintf(lags, sizeof lags, "%s", getenv("BENCH_LAG_MS") ? getenv("BENCH_LAG_MS") : "0,1,2,5,10,20");
    for (char *p = strtok(lags, ","); p; p = strtok(NULL, ",")) run(count, atof(p), mb);
    return 0;
}
