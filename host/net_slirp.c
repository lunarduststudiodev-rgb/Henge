/* The guest's network: user-mode NAT through libslirp (as QEMU's -netdev user),
   so it needs no root and no entitlement. 10.0.2.0/24: the host is 10.0.2.2,
   DNS 10.0.2.3, the guest gets 10.0.2.15 by DHCP.

   libslirp is not thread-safe, so it lives on one thread with its sockets and
   timers. Frames the guest sends are queued to it (and the thread woken through
   a pipe); frames for the guest are queued the other way and handed to the
   virtio-net device on the CPU thread (hv_net_poll), which may only take them
   when the guest has posted receive buffers.

   When the guest's cable goes back in after the host had no network at all (virtio_net.h's carrier), the
   NAT starts afresh: its connections out went through a network that is gone, and would hang until the
   host's TCP gave up on them, minutes later. A new one answers the guest's next packet on an old
   connection with a reset, so its apps see the network changed and connect again at once.

   The guest's DNS queries to 10.0.2.3 over UDP never reach libslirp: net_dns.c answers them, through the
   host's DNS servers, public ones, or the host's own resolver, whichever works (HV_DNS=off: libslirp).
   Over TCP (for an answer too long for UDP) libslirp hands the connection to net_dns.c's loopback server.

   Every port the guest listens on is the same port on the device, on every interface (net_forward.c): libslirp's
   host forwards, opened and closed here, at the top of the loop. The guest's mDNS frames never reach libslirp:
   mdns.c reads them, and announces the guest's name and services on the device's network, through dns_sd's
   socket, which is in this thread's poll set too. */
#include "net_slirp.h"
#include "net_dns.h"
#include "net_forward.h"
#include "mdns.h"
#include "share.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <libslirp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Frames waiting to cross, either way. Toward the guest they pile up while it doesn't run (an engine freeze
   stops CPU 0's tick, which alone hands them to the card): what each connection has in flight, up to
   libslirp's 1 MB send buffer (third_party/libslirp/src/tcp.h), since the guest acknowledges nothing
   meanwhile. 16 MB holds that for a dozen busy connections; the frame count only bounds a flood of small
   frames. A frame that doesn't fit is lost, and TCP resends it only after a timeout that backs off. */
#define QUEUE 16384
#define QUEUE_BYTES (16u << 20)
#define MAX_FRAME 65550
#define MAX_TIMERS 64
#define MAX_POLL 1024

typedef struct Frame {
    uint32_t len;
    uint8_t *data;
    uint64_t queued_ns; /* when it was queued, for how long frames wait for the guest */
} Frame;

typedef struct Queue {
    Frame f[QUEUE];
    uint32_t head, tail;
    uint64_t bytes;
    pthread_mutex_t mu;
} Queue;

typedef struct Timer {
    int used;
    SlirpTimerId id;
    void *cb_opaque;
    int64_t expire_ms; /* -1: not armed */
} Timer;

static Slirp *slirp;
static HvVirtioNet *net;
static HvMachine *machine;
static Queue to_guest, to_host;
static int wake_pipe[2];
static Timer timers[MAX_TIMERS];
static struct pollfd pfds[MAX_POLL];
static int npfd;
static _Atomic int restart_wanted;
static uint64_t restarts;
static void restart(void);
uint64_t hv_net_tx_frames, hv_net_rx_frames;
void (*hv_net_watch)(const uint8_t *frame, uint32_t len, int to_guest);

static char share_socket[256];

void hv_net_set_share_socket(const char *path) {
    snprintf(share_socket, sizeof share_socket, "%s", path ? path : "");
}

/* ---- what the network did, for the perf log (hv_net_report) ----
   Counted where it happens: frames queued on the network and DNS threads, delivered and sent on the CPU
   thread that runs the devices (under the device lock, so the TCP samples below have one thread). */
static struct {
    _Atomic uint64_t in_bytes, out_bytes, in_frames, out_frames;
    _Atomic uint64_t in_udp_bytes;            /* of in_bytes, IPv4 UDP: QUIC (HTTP/3) rides on it, not on TCP */
    _Atomic uint64_t in_dropped, out_dropped; /* a queue was full: the frame is lost, and TCP has to resend it */
    _Atomic uint64_t too_long;                /* frames for the guest longer than the card takes: lost too */
    _Atomic uint64_t resets;                  /* TCP resets the NAT sent the guest: a connection it ended or refused */
    _Atomic uint64_t mac_resets;              /* TCP resets the guest sent: a connection it gave up on */
    _Atomic uint64_t no_buffer;               /* frames were waiting and the guest had no receive buffer posted */
    _Atomic uint64_t wait_ns, wait_max_ns, waits; /* queued until the card took it */
    _Atomic uint64_t ack_ns, ack_max_ns, acks;    /* data handed to the guest until its TCP acknowledged it */
    _Atomic uint32_t peak;                        /* the most frames waiting for the guest at once */
} st;

static void stat_max(_Atomic uint64_t *m, uint64_t v) {
    uint64_t old = atomic_load_explicit(m, memory_order_relaxed);
    while (v > old && !atomic_compare_exchange_weak_explicit(m, &old, v, memory_order_relaxed, memory_order_relaxed)) {}
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int push(Queue *q, const uint8_t *data, uint32_t len) {
    pthread_mutex_lock(&q->mu);
    uint32_t depth = q->tail - q->head;
    int ok = depth < QUEUE && q->bytes + len <= QUEUE_BYTES;
    if (ok) {
        Frame *f = &q->f[q->tail % QUEUE];
        f->data = malloc(len);
        memcpy(f->data, data, len);
        f->len = len;
        f->queued_ns = now_ns();
        q->tail++;
        q->bytes += len;
        if (q == &to_guest && depth + 1 > atomic_load_explicit(&st.peak, memory_order_relaxed))
            atomic_store_explicit(&st.peak, depth + 1, memory_order_relaxed);
    }
    pthread_mutex_unlock(&q->mu);
    if (!ok) atomic_fetch_add_explicit(q == &to_guest ? &st.in_dropped : &st.out_dropped, 1, memory_order_relaxed);
    return ok;
}

/* How quickly the guest's TCP answers: for one segment at a time per connection, the time from the card
   handing it over to the guest's acknowledgement of it going out. It covers the guest's interrupt, its
   driver and its TCP (a delayed acknowledgement included), which is what limits a download through
   libslirp: one connection gets at most what libslirp keeps in flight to the guest per answer time
   (64 KB without window scaling, up to its 1 MB send buffer with it; third_party/libslirp/HENGE.md). */
typedef struct Tcp {
    uint32_t far_ip, seq, ack;
    uint16_t guest_port, far_port, payload;
    uint8_t flags;
} Tcp;

/* An IPv4 TCP segment, from the far side's point of view (to_guest) or the guest's: 1, else 0. */
static int tcp_of(const uint8_t *p, uint32_t len, int to_guest_dir, Tcp *t) {
    if (len < 54 || p[12] != 0x08 || p[13] != 0x00 || (p[14] >> 4) != 4 || p[23] != 6) return 0;
    uint32_t ihl = (p[14] & 15) * 4u, iplen = (uint32_t)p[16] << 8 | p[17];
    if (ihl < 20 || 14 + iplen > len || iplen < ihl + 20) return 0;
    const uint8_t *tcp = p + 14 + ihl;
    uint32_t thl = (tcp[12] >> 4) * 4u;
    if (thl < 20 || ihl + thl > iplen) return 0;
    const uint8_t *far = to_guest_dir ? p + 26 : p + 30;
    t->far_ip = (uint32_t)far[0] << 24 | far[1] << 16 | far[2] << 8 | far[3];
    uint16_t sp = (uint16_t)(tcp[0] << 8 | tcp[1]), dp = (uint16_t)(tcp[2] << 8 | tcp[3]);
    t->guest_port = to_guest_dir ? dp : sp;
    t->far_port = to_guest_dir ? sp : dp;
    t->seq = (uint32_t)tcp[4] << 24 | tcp[5] << 16 | tcp[6] << 8 | tcp[7];
    t->ack = (uint32_t)tcp[8] << 24 | tcp[9] << 16 | tcp[10] << 8 | tcp[11];
    t->flags = tcp[13];
    t->payload = (uint16_t)(iplen - ihl - thl);
    return 1;
}

#define SAMPLES 64
static struct Sample {
    uint32_t far_ip, seq_end;
    uint16_t guest_port, far_port;
    uint64_t t; /* 0: none waiting */
} samples[SAMPLES];

static struct Sample *sample_for(const Tcp *t) {
    uint32_t h = (t->far_ip * 2654435761u) ^ (uint32_t)t->guest_port * 40503u ^ t->far_port;
    return &samples[h % SAMPLES];
}

/* CPU thread: the card took this frame for the guest. */
static void delivered(const uint8_t *frame, uint32_t len, uint64_t queued_ns) {
    uint64_t now = now_ns(), waited = now - queued_ns;
    atomic_fetch_add_explicit(&st.in_bytes, len, memory_order_relaxed);
    atomic_fetch_add_explicit(&st.in_frames, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&st.wait_ns, waited, memory_order_relaxed);
    atomic_fetch_add_explicit(&st.waits, 1, memory_order_relaxed);
    stat_max(&st.wait_max_ns, waited);
    if (len >= 14 + 20 && frame[12] == 0x08 && frame[13] == 0x00 && frame[14 + 9] == 17)
        atomic_fetch_add_explicit(&st.in_udp_bytes, len, memory_order_relaxed);
    Tcp t;
    if (!tcp_of(frame, len, 1, &t) || !t.payload) return;
    struct Sample *s = sample_for(&t);
    if (s->t && s->far_ip == t.far_ip && s->guest_port == t.guest_port && s->far_port == t.far_port) return; /* one at a time */
    *s = (struct Sample){t.far_ip, t.seq + t.payload, t.guest_port, t.far_port, now};
}

/* CPU thread: the guest sent this frame. */
static void sent_by_guest(const uint8_t *frame, uint32_t len) {
    atomic_fetch_add_explicit(&st.out_bytes, len, memory_order_relaxed);
    atomic_fetch_add_explicit(&st.out_frames, 1, memory_order_relaxed);
    Tcp t;
    if (!tcp_of(frame, len, 0, &t) || !(t.flags & 0x10)) return;
    struct Sample *s = sample_for(&t);
    if (!s->t || s->far_ip != t.far_ip || s->guest_port != t.guest_port || s->far_port != t.far_port) return;
    if ((int32_t)(t.ack - s->seq_end) < 0) return; /* not that far yet */
    uint64_t took = now_ns() - s->t;
    s->t = 0;
    atomic_fetch_add_explicit(&st.ack_ns, took, memory_order_relaxed);
    atomic_fetch_add_explicit(&st.acks, 1, memory_order_relaxed);
    stat_max(&st.ack_max_ns, took);
}

/* HV_NET_LOG: one line per frame (ethertype, IP protocol, ports). */
static void log_frame(const char *dir, const uint8_t *p, uint32_t len) {
    static int on = -1;
    if (on < 0) on = getenv("HV_NET_LOG") != NULL;
    if (!on || len < 14) return;
    unsigned et = p[12] << 8 | p[13];
    if (et == 0x0800 && len >= 34) {
        unsigned ihl = (p[14] & 15) * 4, proto = p[23];
        unsigned sp = 0, dp = 0;
        if ((proto == 6 || proto == 17) && len >= 14 + ihl + 4) {
            sp = p[14 + ihl] << 8 | p[15 + ihl];
            dp = p[16 + ihl] << 8 | p[17 + ihl];
        }
        fprintf(stderr, "net %s ipv4 %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u proto %u len %u\n", dir, p[26], p[27], p[28],
                p[29], sp, p[30], p[31], p[32], p[33], dp, proto, len);
    } else {
        fprintf(stderr, "net %s ethertype %#06x len %u\n", dir, et, len);
    }
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- slirp callbacks (all on the network thread) ---- */

/* A TCP reset (IPv4, or IPv6 without extension headers). */
static int is_reset(const uint8_t *p, size_t len) {
    if (len < 14) return 0;
    unsigned et = p[12] << 8 | p[13];
    if (et == 0x0800 && len >= 34 && p[23] == 6) {
        size_t tcp = 14 + (size_t)(p[14] & 15) * 4;
        return len >= tcp + 14 && (p[tcp + 13] & 0x04);
    }
    return et == 0x86dd && len >= 54 + 14 && p[20] == 6 && (p[54 + 13] & 0x04);
}

/* ---- which connections are reset, and why (network thread) ----
   Each reset either way is logged with its connection, how long ago the guest opened it, who had closed
   which side before, and, for the NAT's, why libslirp sent it (slirp_henge_reset_why, third_party/libslirp):
   the far side reset it, the Mac stopped acknowledging, or a packet came for a connection the NAT no longer
   has. At most RESET_LINES a RESET_SECS, so a burst doesn't flood the log; the rest are counted. */
#define CONNS 256
#define CONN_HASH_SIZE 512
#define CONN_HASH_MASK (CONN_HASH_SIZE - 1)
#define RESET_LINES 12
#define RESET_SECS 10
static struct Conn {
    uint32_t far_ip;
    uint16_t guest_port, far_port;
    int64_t opened, mac_fin, nat_fin, mac_rst, nat_rst; /* ms, 0: not seen */
} conns[CONNS];
static int16_t conn_hash[CONN_HASH_SIZE]; /* -1: empty, else index into conns[] */
static int conn_hash_inited = 0;
static uint32_t conn_next;
static char last_reset[256];
static pthread_mutex_t last_reset_mu = PTHREAD_MUTEX_INITIALIZER;

static inline uint32_t conn_hash_fn(uint32_t far_ip, uint16_t guest_port, uint16_t far_port) {
    return ((far_ip * 2654435761u) ^ ((uint32_t)guest_port * 40503u) ^ (uint32_t)far_port) & CONN_HASH_MASK;
}

static struct Conn *conn_find(const Tcp *t) {
    if (!conn_hash_inited) return NULL;
    uint32_t h = conn_hash_fn(t->far_ip, t->guest_port, t->far_port);
    for (int step = 0; step < 16; step++) {
        int idx = conn_hash[(h + step) & CONN_HASH_MASK];
        if (idx < 0) return NULL;
        struct Conn *c = &conns[idx];
        if (c->opened && c->far_ip == t->far_ip && c->guest_port == t->guest_port && c->far_port == t->far_port) return c;
    }
    return NULL;
}

static void ago(char *buf, size_t n, const char *what, int64_t then, int64_t now) {
    size_t l = strlen(buf);
    if (then && l + 1 < n) snprintf(buf + l, n - l, "%s%s %.1f s ago", l ? "; " : "", what, (double)(now - then) / 1000);
}

static void reset_line(const Tcp *t, int by_nat, const char *why, int64_t now) {
    static int64_t window;
    static int lines, unshown;
    struct Conn *c = conn_find(t);
    char hist[160] = "";
    if (c) {
        ago(hist, sizeof hist, "opened", c->opened, now);
        ago(hist, sizeof hist, "the Mac closed its side", c->mac_fin, now);
        ago(hist, sizeof hist, "the far side closed its side", c->nat_fin, now);
        ago(hist, sizeof hist, by_nat ? "the Mac reset it" : "the NAT reset it", by_nat ? c->mac_rst : c->nat_rst, now);
    } else {
        snprintf(hist, sizeof hist, "not seen opening");
    }
    char line[256];
    snprintf(line, sizeof line, "%s, port %u to %u.%u.%u.%u:%u (%s)%s%s", by_nat ? "reset to the Mac" : "reset from the Mac",
             t->guest_port, t->far_ip >> 24, t->far_ip >> 16 & 255, t->far_ip >> 8 & 255, t->far_ip & 255, t->far_port, hist,
             by_nat ? ": " : "", by_nat ? (why ? why : "no reason given") : "");
    pthread_mutex_lock(&last_reset_mu);
    memcpy(last_reset, line, sizeof line);
    pthread_mutex_unlock(&last_reset_mu);
    if (now - window >= RESET_SECS * 1000) {
        if (unshown) fprintf(stderr, "hengevm: net: %d more resets not shown\n", unshown);
        window = now, lines = 0, unshown = 0;
    }
    if (lines++ < RESET_LINES) fprintf(stderr, "hengevm: net: %s\n", line);
    else unshown++;
}

/* A TCP segment crossing, either way: what it says about its connection. Resets are logged. */
static void track(const uint8_t *frame, uint32_t len, int to_guest_dir) {
    Tcp t;
    if (!tcp_of(frame, len, to_guest_dir, &t)) return; /* IPv4 only, as the NAT */
    int64_t now = now_ms();
    struct Conn *c = conn_find(&t);
    if (!to_guest_dir && (t.flags & 0x12) == 0x02) { /* the guest opens one */
        if (!c) {
            if (!conn_hash_inited) {
                memset(conn_hash, -1, sizeof conn_hash);
                conn_hash_inited = 1;
            }
            int slot = (int)(conn_next++ % CONNS);
            c = &conns[slot];
            if (c->opened) {
                uint32_t old_h = conn_hash_fn(c->far_ip, c->guest_port, c->far_port);
                for (int step = 0; step < 16; step++) {
                    uint32_t pos = (old_h + step) & CONN_HASH_MASK;
                    if (conn_hash[pos] == slot) { conn_hash[pos] = -1; break; }
                }
            }
            *c = (struct Conn){.far_ip = t.far_ip, .guest_port = t.guest_port, .far_port = t.far_port, .opened = now};
            uint32_t new_h = conn_hash_fn(t.far_ip, t.guest_port, t.far_port);
            for (int step = 0; step < 16; step++) {
                uint32_t pos = (new_h + step) & CONN_HASH_MASK;
                if (conn_hash[pos] < 0 || conn_hash[pos] == slot) {
                    conn_hash[pos] = (int16_t)slot;
                    break;
                }
            }
            return;
        }
        *c = (struct Conn){.far_ip = t.far_ip, .guest_port = t.guest_port, .far_port = t.far_port, .opened = now};
        return;
    }
    if (t.flags & 0x04) {
        reset_line(&t, to_guest_dir, to_guest_dir ? slirp_henge_reset_why : NULL, now);
        if (c && !*(to_guest_dir ? &c->nat_rst : &c->mac_rst)) *(to_guest_dir ? &c->nat_rst : &c->mac_rst) = now;
        if (!to_guest_dir) atomic_fetch_add_explicit(&st.mac_resets, 1, memory_order_relaxed);
    } else if (c && (t.flags & 0x01)) {
        int64_t *fin = to_guest_dir ? &c->nat_fin : &c->mac_fin;
        if (!*fin) *fin = now;
    }
}

void hv_net_last_reset(char *buf, size_t n) {
    pthread_mutex_lock(&last_reset_mu);
    snprintf(buf, n, "%s", last_reset);
    pthread_mutex_unlock(&last_reset_mu);
}

void hv_net_losses(uint64_t *dropped_to_guest, uint64_t *dropped_from_guest, uint64_t *too_long, uint64_t *resets) {
    *dropped_to_guest = atomic_load_explicit(&st.in_dropped, memory_order_relaxed);
    *dropped_from_guest = atomic_load_explicit(&st.out_dropped, memory_order_relaxed);
    *too_long = atomic_load_explicit(&st.too_long, memory_order_relaxed);
    *resets = atomic_load_explicit(&st.resets, memory_order_relaxed);
}

static slirp_ssize_t send_packet(const void *buf, size_t len, void *opaque) {
    (void)opaque;
    if (len > MAX_FRAME) {
        atomic_fetch_add_explicit(&st.too_long, 1, memory_order_relaxed);
        return (slirp_ssize_t)len;
    }
    if (is_reset(buf, len)) atomic_fetch_add_explicit(&st.resets, 1, memory_order_relaxed);
    track(buf, (uint32_t)len, 1);
    slirp_henge_reset_why = NULL;
    log_frame("rx", buf, (uint32_t)len);
    if (hv_net_watch) hv_net_watch(buf, (uint32_t)len, 1);
    if (push(&to_guest, buf, (uint32_t)len)) hv_machine_wake(machine);
    return (slirp_ssize_t)len;
}

/* The network thread (mdns.c): a frame for the guest. */
static void mdns_deliver(const uint8_t *frame, uint32_t len) {
    log_frame("rx", frame, len);
    if (hv_net_watch) hv_net_watch(frame, len, 1);
    if (push(&to_guest, frame, len)) hv_machine_wake(machine);
}

/* net_dns.c's thread: an answer for the guest. */
static void dns_deliver(const uint8_t *frame, uint32_t len) {
    log_frame("rx", frame, len);
    if (hv_net_watch) hv_net_watch(frame, len, 1);
    if (push(&to_guest, frame, len)) hv_machine_wake(machine);
}

static void guest_error(const char *msg, void *opaque) {
    (void)opaque;
    fprintf(stderr, "hengevm: net: %s\n", msg);
}

static int64_t clock_get_ns(void *opaque) {
    (void)opaque;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void *timer_new_opaque(SlirpTimerId id, void *cb_opaque, void *opaque) {
    (void)opaque;
    for (int i = 0; i < MAX_TIMERS; i++)
        if (!timers[i].used) {
            timers[i] = (Timer){1, id, cb_opaque, -1};
            return &timers[i];
        }
    return NULL;
}

static void timer_free(void *timer, void *opaque) {
    (void)opaque;
    if (timer) ((Timer *)timer)->used = 0;
}

static void timer_mod(void *timer, int64_t expire_ms, void *opaque) {
    (void)opaque;
    if (timer) ((Timer *)timer)->expire_ms = expire_ms;
}

static void register_poll_socket(slirp_os_socket fd, void *opaque) {
    (void)fd, (void)opaque;
}

static void unregister_poll_socket(slirp_os_socket fd, void *opaque) {
    (void)fd, (void)opaque;
}

static void notify(void *opaque) {
    (void)opaque;
    char c = 0;
    (void)!write(wake_pipe[1], &c, 1);
}

static int add_poll(slirp_os_socket fd, int events, void *opaque) {
    (void)opaque;
    if (npfd == MAX_POLL) return -1;
    short ev = 0;
    if (events & SLIRP_POLL_IN) ev |= POLLIN;
    if (events & SLIRP_POLL_OUT) ev |= POLLOUT;
    if (events & SLIRP_POLL_PRI) ev |= POLLPRI;
    pfds[npfd] = (struct pollfd){fd, ev, 0};
    return npfd++;
}

static int get_revents(int idx, void *opaque) {
    (void)opaque;
    short r = pfds[idx].revents;
    int ev = 0;
    if (r & POLLIN) ev |= SLIRP_POLL_IN;
    if (r & POLLOUT) ev |= SLIRP_POLL_OUT;
    if (r & POLLPRI) ev |= SLIRP_POLL_PRI;
    if (r & POLLERR) ev |= SLIRP_POLL_ERR;
    if (r & POLLHUP) ev |= SLIRP_POLL_HUP;
    return ev;
}

static const SlirpCb callbacks = {
    .send_packet = send_packet,
    .guest_error = guest_error,
    .clock_get_ns = clock_get_ns,
    .timer_free = timer_free,
    .timer_mod = timer_mod,
    .notify = notify,
    .timer_new_opaque = timer_new_opaque,
    .register_poll_socket = register_poll_socket,
    .unregister_poll_socket = unregister_poll_socket,
};

/* ---- the network thread ---- */

/* ---- the Mac's ports on the device (net_forward.c), on the network thread ---- */

/* Where the Mac's ports listen: every interface of the device, as a MacBook's do. The host's tests take 127.0.0.1
   (hv_net_ports_loopback) so that nothing on the network can reach a test. */
static struct in_addr fwd_addr = {INADDR_ANY};
void hv_net_ports_loopback(int on) { fwd_addr.s_addr = on ? htonl(INADDR_LOOPBACK) : INADDR_ANY; }

static int fwd_open(int udp, uint16_t port) {
    if (!slirp) return ENETDOWN;
    struct in_addr guest;
    inet_pton(AF_INET, "10.0.2.15", &guest);
    errno = 0;
    if (slirp_add_hostfwd(slirp, udp, fwd_addr, port, guest, port) != 0) return errno ? errno : EIO; /* the Mac's own flows go on */
    if (udp) slirp_henge_udp_forget(slirp, guest, port); /* its answers must leave through the forward, now there */
    return 0;
}

static void fwd_close(int udp, uint16_t port) {
    if (slirp) slirp_remove_hostfwd(slirp, udp, fwd_addr, port);
}

static void fwd_wake(void) {
    char c = 0;
    (void)!write(wake_pipe[1], &c, 1);
}

static const HvFwdOps fwd_ops = {fwd_open, fwd_close, fwd_wake};

/* share.c's thread: the helper's "ports" line (n -1: its link closed). */
static void ports_from_share(const uint16_t *port, const uint8_t *udp, int n) {
    static HvFwdPort p[HV_SHARE_MAX_PORTS];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    if (n > HV_SHARE_MAX_PORTS) n = HV_SHARE_MAX_PORTS;
    for (int i = 0; i < n; i++) p[i] = (HvFwdPort){port[i], udp[i]};
    hv_net_ports_set(p, n);
    pthread_mutex_unlock(&mu);
}

static void *net_thread(void *unused) {
    (void)unused;
    for (;;) {
        if (atomic_exchange(&restart_wanted, 0)) restart();
        hv_fwd_apply(); /* before the poll set is made: a listener it closes must not be in it. With no NAT (it
                           couldn't start again), each port says so (ENETDOWN) rather than what it was */
        int64_t mdns_in = hv_mdns_tick(now_ms());
        if (!slirp) { /* it couldn't start again: what the guest sends goes nowhere, but its mDNS (mdns.c) */
            char buf[64];
            struct pollfd p[2] = {{wake_pipe[0], POLLIN, 0}, {hv_mdns_fd(), POLLIN, 0}};
            poll(p, p[1].fd >= 0 ? 2 : 1, (int)(mdns_in < 1000 ? mdns_in : 1000));
            while (read(wake_pipe[0], buf, sizeof buf) > 0) {}
            if (p[1].fd >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) hv_mdns_ready();
            for (;;) {
                pthread_mutex_lock(&to_host.mu);
                Frame f = {0, NULL, 0};
                if (to_host.head != to_host.tail) {
                    f = to_host.f[to_host.head++ % QUEUE];
                    to_host.bytes -= f.len;
                }
                pthread_mutex_unlock(&to_host.mu);
                if (!f.data) break;
                hv_mdns_take(f.data, f.len);
                free(f.data);
            }
            continue;
        }
        uint32_t timeout = 1000;
        if (mdns_in < timeout) timeout = (uint32_t)mdns_in;
        int64_t now = now_ms();
        for (int i = 0; i < MAX_TIMERS; i++)
            if (timers[i].used && timers[i].expire_ms >= 0) {
                int64_t left = timers[i].expire_ms - now;
                if (left < 0) left = 0;
                if ((uint64_t)left < timeout) timeout = (uint32_t)left;
            }
        npfd = 0;
        pfds[npfd++] = (struct pollfd){wake_pipe[0], POLLIN, 0};
        pfds[npfd++] = (struct pollfd){hv_mdns_fd(), POLLIN, 0}; /* -1: poll skips it */
        slirp_pollfds_fill_socket(slirp, &timeout, add_poll, NULL);
        int r = poll(pfds, (nfds_t)npfd, (int)timeout);
        if (pfds[0].revents & POLLIN) {
            char buf[64];
            while (read(wake_pipe[0], buf, sizeof buf) > 0) {}
        }
        if (pfds[1].fd >= 0 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) hv_mdns_ready();
        slirp_henge_reset_why = NULL;
        slirp_pollfds_poll(slirp, r < 0, get_revents, NULL);
        now = now_ms();
        for (int i = 0; i < MAX_TIMERS; i++)
            if (timers[i].used && timers[i].expire_ms >= 0 && timers[i].expire_ms <= now) {
                timers[i].expire_ms = -1;
                slirp_henge_reset_why = NULL;
                slirp_handle_timer(slirp, timers[i].id, timers[i].cb_opaque);
            }
        /* What the guest sent since: batch dequeue up to 32 frames under one lock */
        for (;;) {
            Frame batch[32];
            uint32_t count = 0;
            pthread_mutex_lock(&to_host.mu);
            uint32_t depth = to_host.tail - to_host.head;
            if (depth > 32) depth = 32;
            for (uint32_t i = 0; i < depth; i++) {
                batch[i] = to_host.f[(to_host.head + i) % QUEUE];
            }
            to_host.head += depth;
            for (uint32_t i = 0; i < depth; i++) {
                to_host.bytes -= batch[i].len;
            }
            count = depth;
            pthread_mutex_unlock(&to_host.mu);
            if (!count) break;

            for (uint32_t i = 0; i < count; i++) {
                if (!hv_mdns_take(batch[i].data, batch[i].len)) {
                    track(batch[i].data, batch[i].len, 0);
                    slirp_input(slirp, batch[i].data, (int)batch[i].len);
                }
                free(batch[i].data);
            }
        }
    }
    return NULL;
}

/* CPU thread: the guest sent a frame. */
static void transmit(void *opaque, const uint8_t *frame, uint32_t len) {
    (void)opaque;
    hv_net_tx_frames++;
    sent_by_guest(frame, len);
    log_frame("tx", frame, len);
    if (hv_net_watch) hv_net_watch(frame, len, 0);
    if (hv_dns_take(frame, len)) return;
    if (push(&to_host, frame, len)) {
        char c = 0;
        (void)!write(wake_pipe[1], &c, 1);
    }
}

void hv_net_poll(void) {
    if (!net || to_guest.head == to_guest.tail) return;
    for (;;) {
        Frame batch[32];
        uint32_t count = 0;
        pthread_mutex_lock(&to_guest.mu);
        uint32_t depth = to_guest.tail - to_guest.head;
        if (!depth) {
            pthread_mutex_unlock(&to_guest.mu);
            return;
        }
        if (depth > 32) depth = 32;
        for (uint32_t i = 0; i < depth; i++) {
            batch[i] = to_guest.f[(to_guest.head + i) % QUEUE];
        }
        count = depth;
        pthread_mutex_unlock(&to_guest.mu);

        uint32_t delivered_count = 0;
        uint64_t freed_bytes = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!hv_virtio_net_receive(net, batch[i].data, batch[i].len)) {
                atomic_fetch_add_explicit(&st.no_buffer, 1, memory_order_relaxed);
                break;
            }
            hv_net_rx_frames++;
            delivered(batch[i].data, batch[i].len, batch[i].queued_ns);
            freed_bytes += batch[i].len;
            free(batch[i].data);
            delivered_count++;
        }

        if (delivered_count > 0) {
            pthread_mutex_lock(&to_guest.mu);
            to_guest.head += delivered_count;
            to_guest.bytes -= freed_bytes;
            pthread_mutex_unlock(&to_guest.mu);
        }

        if (delivered_count < count) return;
    }
}

int hv_net_mtu(void) {
    static int mtu;
    if (!mtu) {
        const char *e = getenv("HV_NET_MTU");
        int v = e ? atoi(e) : 0;
        mtu = v >= 576 && v <= 65520 ? v : 1500;
    }
    return mtu;
}

/* For the perf log: what the network did since the last call, one line. Any thread (one caller). */
int hv_net_report(char *buf, size_t n) {
    static uint64_t last_ns, last[9];
    uint64_t now = now_ns();
    uint64_t cur[9] = {st.in_bytes, st.in_frames, st.out_bytes, st.out_frames, st.in_dropped + st.out_dropped + st.too_long,
                       st.no_buffer, st.in_udp_bytes, st.resets, st.mac_resets};
    double secs = last_ns ? (double)(now - last_ns) / 1e9 : 0;
    uint64_t d[9];
    for (int i = 0; i < 9; i++) d[i] = cur[i] - last[i], last[i] = cur[i];
    last_ns = now;
    uint64_t waits = atomic_exchange(&st.waits, 0), wait_ns = atomic_exchange(&st.wait_ns, 0), wait_max = atomic_exchange(&st.wait_max_ns, 0);
    uint64_t acks = atomic_exchange(&st.acks, 0), ack_ns = atomic_exchange(&st.ack_ns, 0), ack_max = atomic_exchange(&st.ack_max_ns, 0);
    uint32_t peak = atomic_exchange(&st.peak, 0);
    static uint64_t last_renames;
    uint64_t renames = hv_mdns_renames(), renamed = renames - last_renames;
    last_renames = renames;
    if (secs <= 0) secs = 1;
    snprintf(buf, n,
             "in %.2f MB/s (%.0f frames/s, %.0f%% UDP), out %.2f MB/s (%.0f frames/s); the Mac acknowledges in %.1f ms (max %.0f, %llu samples); "
             "frames wait %.2f ms for it (max %.1f), queue peak %u, dropped %llu, no buffer %llu; resets %llu, from the Mac %llu; MTU %d; "
             "the Mac's name found taken on the network %llu",
             (double)d[0] / secs / 1e6, (double)d[1] / secs, d[0] ? 100.0 * (double)d[6] / (double)d[0] : 0.0,
             (double)d[2] / secs / 1e6, (double)d[3] / secs,
             acks ? (double)ack_ns / acks / 1e6 : 0.0, (double)ack_max / 1e6, (unsigned long long)acks,
             waits ? (double)wait_ns / waits / 1e6 : 0.0, (double)wait_max / 1e6, peak, (unsigned long long)d[4],
             (unsigned long long)d[5], (unsigned long long)d[7], (unsigned long long)d[8], hv_net_mtu(),
             (unsigned long long)renamed);
    return d[1] + d[3] + renamed != 0;
}

/* The host's tests: while set, the NAT can't be made (as when libslirp runs out of memory). */
_Atomic int hv_net_test_fail_start;

/* The NAT, with the shared folder's address. 0, or -1. */
static int make_slirp(void) {
    if (hv_net_test_fail_start) return -1;
    SlirpConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.version = 6; /* below 6 it calls the old register_poll_fd, which we leave unset */
    cfg.in_enabled = true;
    inet_pton(AF_INET, "10.0.2.0", &cfg.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &cfg.vnetmask);
    inet_pton(AF_INET, "10.0.2.2", &cfg.vhost);
    inet_pton(AF_INET, "10.0.2.15", &cfg.vdhcp_start);
    inet_pton(AF_INET, HV_DNS_ADDR, &cfg.vnameserver);
    cfg.vhostname = "hengevm";
    cfg.if_mtu = hv_net_mtu();
    cfg.if_mru = hv_net_mtu();
    slirp = slirp_new(&cfg, &callbacks, NULL);
    if (!slirp) return -1;
    if (share_socket[0]) { /* 10.0.2.4:80 is the shared folder and clipboard: see share.c */
        struct in_addr addr;
        inet_pton(AF_INET, HV_SHARE_ADDR, &addr);
        if (slirp_add_unix(slirp, share_socket, &addr, 80) != 0) fprintf(stderr, "hengevm: net: no route to the shared folder\n");
    }
    return 0;
}

/* The network thread, when the guest's cable went back in: the NAT afresh (see the top). */
static void restart(void) {
    if (slirp) slirp_cleanup(slirp); /* closes every socket it had, the shared folder's connection with them */
    for (int i = 0; i < MAX_TIMERS; i++) timers[i].used = 0;
    conn_hash_inited = 0;
    memset(conns, 0, sizeof conns);
    slirp = NULL;
    restarts++;
    hv_dns_network_changed();
    hv_fwd_lost(); /* slirp_cleanup closed the Mac's ports too: opened again on the new one, unless held */
    if (make_slirp() != 0) fprintf(stderr, "hengevm: net: cannot start the network again\n");
    else fprintf(stderr, "hengevm: net: the cable is back in: a fresh network behind it (%llu)\n", (unsigned long long)restarts);
}

/* CPU thread: virtio_net.c's carrier. */
static void carrier_changed(void *opaque, int up) {
    (void)opaque;
    fprintf(stderr, "hengevm: net: the host's network is %s: the Mac's Ethernet cable is %s\n", up ? "back" : "gone", up ? "in" : "out");
    if (!up) return;
    atomic_store(&restart_wanted, 1);
    char c = 0;
    (void)!write(wake_pipe[1], &c, 1);
}

static void on_rx_notify(void *opaque) {
    (void)opaque;
    hv_net_poll();
}

int hv_net_attach(HvMachine *m, HvVirtioNet *n) {
    machine = m;
    pthread_mutex_init(&to_guest.mu, NULL);
    pthread_mutex_init(&to_host.mu, NULL);
    if (pipe(wake_pipe) != 0) return -1;
    fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);
    if (make_slirp() != 0) return -1;
    hv_fwd_attach(&fwd_ops);
    hv_fwd_lost(); /* the first time: open the standard set until the helper says */
    hv_share_ports = ports_from_share; /* before the guest can reach share.c: through this network only */
    hv_mdns_start(mdns_deliver, hv_net_mtu());
    if (hv_dns_start(dns_deliver) != 0) fprintf(stderr, "hengevm: net: DNS is left to libslirp\n");
    slirp_henge_dns_tcp_port = hv_dns_tcp_port(); /* DNS over TCP, too long for UDP: net_dns.c */
    net = n;
    n->transmit = transmit;
    n->carrier_changed = carrier_changed;
    n->rx_notify = on_rx_notify;
    pthread_t t;
    pthread_create(&t, NULL, net_thread, NULL);
    return 0;
}
