/* The NAT behind the network card (host/net_slirp.c) across the cable going out and back in for the host's
   network: a guest TCP connection through it to a server on this Mac works; when the cable goes back in, the
   NAT starts afresh, so the host side of the old connection is closed (the server sees it end) and the
   guest's next packet on it is answered with a reset at once instead of hanging; and a new connection works.
   Window scaling (third_party/libslirp/HENGE.md): offered back when the guest's SYN offers it, as macOS's does,
   and not when it doesn't. A far side that resets a download: the guest still gets all the NAT had of it,
   then the reset, and the log says which connection and why. DNS: an answer too long for UDP comes to the
   guest marked truncated, and its query again over TCP to 10.0.2.3 gets the whole answer, through
   net_dns.c (a DNS server played here). The guest is played here with raw Ethernet frames. Needs no network
   beyond this Mac's loopback. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "loopback_only.h"
#include "machine.h"
#include "mdns.h"
#include "net_forward.h"
#include "net_slirp.h"
#include "pcie.h"
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
static const uint8_t guest_ip[4] = {10, 0, 2, 15}, host_ip[4] = {10, 0, 2, 2}, dns_ip[4] = {10, 0, 2, 3};
static const uint8_t *tcp_dst = host_ip; /* where the guest's TCP segments go */
static uint8_t router_mac[6];

/* What the NAT sent the guest, as the network thread saw it. */
static _Atomic int arp_replies, synacks, resets;
static _Atomic uint32_t synack_seq;
static _Atomic int synack_wscale = -2; /* the last SYN-ACK's window scale option, or -1 for none */

/* The guest's receiving side of one connection (its port rx_port): what arrived in order, acknowledged at
   once while rx_acking. */
static _Atomic int rx_port, rx_acking, rx_reset;
static _Atomic uint32_t rx_next, rx_bytes, rx_seq = 1003;
static uint8_t rx_data[4096]; /* its first bytes */
static _Atomic int dns_answers, dns_flags = -1; /* UDP answers from 10.0.2.3, and the last one's flags */

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

/* wscale >= 0: a window scale option (after a NOP), for a SYN. */
static void send_seg(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags, const void *data, size_t dlen, int wscale) {
    size_t hlen = wscale >= 0 ? 24 : 20;
    uint8_t f[14 + 20 + 24 + 256] = {0};
    memcpy(f, router_mac, 6), memcpy(f + 6, guest_mac, 6), put16(f + 12, 0x0800);
    uint8_t *ip = f + 14, *tcp = ip + 20;
    ip[0] = 0x45, put16(ip + 2, (uint16_t)(20 + hlen + dlen)), put16(ip + 4, 1), ip[8] = 64, ip[9] = 6;
    memcpy(ip + 12, guest_ip, 4), memcpy(ip + 16, tcp_dst, 4);
    put16(ip + 10, csum(ip, 20, 0));
    put16(tcp, sport), put16(tcp + 2, dport), put32(tcp + 4, seq), put32(tcp + 8, ack);
    tcp[12] = (uint8_t)(hlen / 4) << 4, tcp[13] = flags, put16(tcp + 14, 65535);
    if (wscale >= 0) tcp[20] = 1, tcp[21] = 3, tcp[22] = 3, tcp[23] = (uint8_t)wscale;
    if (dlen) memcpy(tcp + hlen, data, dlen);
    uint8_t pseudo[12];
    memcpy(pseudo, guest_ip, 4), memcpy(pseudo + 4, tcp_dst, 4), pseudo[8] = 0, pseudo[9] = 6;
    put16(pseudo + 10, (uint16_t)(hlen + dlen));
    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2) sum += (uint32_t)pseudo[i] << 8 | pseudo[i + 1];
    put16(tcp + 16, csum(tcp, hlen + dlen, sum));
    n->transmit(n->opaque, f, (uint32_t)(14 + 20 + hlen + dlen));
}
static void send_tcp_opt(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags, const char *data, int wscale) {
    send_seg(sport, dport, seq, ack, flags, data, data ? strlen(data) : 0, wscale);
}
static void send_tcp(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags, const char *data) {
    send_tcp_opt(sport, dport, seq, ack, flags, data, -1);
}

/* A DNS query from the guest's port 50000 to 10.0.2.3:53. */
static void send_dns_udp(const uint8_t *msg, size_t len) {
    uint8_t f[14 + 20 + 8 + 256] = {0};
    memcpy(f, router_mac, 6), memcpy(f + 6, guest_mac, 6), put16(f + 12, 0x0800);
    uint8_t *ip = f + 14, *udp = ip + 20;
    ip[0] = 0x45, put16(ip + 2, (uint16_t)(28 + len)), put16(ip + 4, 2), ip[8] = 64, ip[9] = 17;
    memcpy(ip + 12, guest_ip, 4), memcpy(ip + 16, dns_ip, 4);
    put16(ip + 10, csum(ip, 20, 0));
    put16(udp, 50000), put16(udp + 2, 53), put16(udp + 4, (uint16_t)(8 + len)); /* no checksum: allowed in IPv4 */
    memcpy(udp + 8, msg, len);
    n->transmit(n->opaque, f, (uint32_t)(14 + 28 + len));
}

/* The DNS server the guest's 10.0.2.3 asks (HV_DNS): over UDP it says the answer is too long (TC), over TCP
   it gives it, BIG_ANSWERS addresses. */
enum { BIG_ANSWERS = 40 };
static int dns_udp = -1, dns_tcp = -1;
static _Atomic int dns_tcp_queries;
static void *dns_server(void *unused) {
    (void)unused;
    for (;;) {
        struct pollfd p[2] = {{dns_udp, POLLIN, 0}, {dns_tcp, POLLIN, 0}};
        if (poll(p, 2, -1) <= 0) continue;
        uint8_t q[512], a[2 + 512 + BIG_ANSWERS * 16];
        if (p[0].revents & POLLIN) {
            struct sockaddr_storage from;
            socklen_t fl = sizeof from;
            ssize_t r = recvfrom(dns_udp, q, sizeof q, 0, (struct sockaddr *)&from, &fl);
            if (r >= 12) {
                q[2] |= 0x82, q[3] = 0x80; /* an answer, truncated: no records */
                sendto(dns_udp, q, (size_t)r, 0, (struct sockaddr *)&from, fl);
            }
        }
        if (p[1].revents & POLLIN) {
            int c = accept(dns_tcp, NULL, NULL);
            uint8_t hdr[2];
            if (c >= 0 && recv(c, hdr, 2, MSG_WAITALL) == 2 && get16(hdr) <= sizeof q &&
                recv(c, q, get16(hdr), MSG_WAITALL) == get16(hdr)) {
                size_t ql = get16(hdr), len = ql;
                dns_tcp_queries++;
                memcpy(a + 2, q, ql);
                a[2 + 2] |= 0x80, a[2 + 3] = 0x80, put16(a + 2 + 6, BIG_ANSWERS);
                for (int i = 0; i < BIG_ANSWERS; i++, len += 16) {
                    uint8_t *rr = a + 2 + len;
                    rr[0] = 0xc0, rr[1] = 12, put16(rr + 2, 1), put16(rr + 4, 1), put32(rr + 6, 60), put16(rr + 10, 4);
                    rr[12] = 192, rr[13] = 0, rr[14] = 2, rr[15] = (uint8_t)i;
                }
                put16(a, (uint16_t)len);
                send(c, a, 2 + len, 0);
            }
            if (c >= 0) close(c);
        }
    }
    return NULL;
}

/* Network thread: every frame for the guest. The guest answers ARP, as macOS would. */
static void watch(const uint8_t *f, uint32_t len, int to_guest) {
    if (!to_guest || len < 14) return;
    uint16_t type = get16(f + 12);
    if (type == 0x0806 && len >= 42) {
        int op = get16(f + 20);
        if (op == 2) {
            memcpy(router_mac, f + 22, 6);
            arp_replies++;
        } else if (op == 1 && !memcmp(f + 38, guest_ip, 4)) {
            send_arp(2, f + 22, f + 28);
        }
    } else if (type == 0x0800 && len >= 14 + 20 + 8 + 12 && f[23] == 17 && get16(f + 14 + 20) == 53) {
        dns_flags = get16(f + 14 + 28 + 2);
        dns_answers++;
    } else if (type == 0x0800 && len >= 54 && f[23] == 6) {
        const uint8_t *tcp = f + 14 + (f[14] & 15) * 4;
        uint8_t flags = tcp[13];
        if ((flags & 0x12) == 0x12) {
            int ws = -1;
            const uint8_t *o = tcp + 20, *end = tcp + (tcp[12] >> 4) * 4;
            if (end > f + len) end = f + len;
            while (o < end && *o != 0) {
                if (*o == 1) { o++; continue; }
                if (o + 1 >= end || o[1] < 2) break;
                if (o[0] == 3 && o[1] == 3 && o + 2 < end) ws = o[2];
                o += o[1];
            }
            synack_wscale = ws;
            synack_seq = get32(tcp + 4);
            synacks++;
        }
        if (flags & 0x04) resets++;
        if (get16(tcp + 2) == rx_port && rx_port) {
            uint32_t ip_len = get16(f + 16), hl = (uint32_t)(tcp[12] >> 4) * 4, ihl = (uint32_t)(f[14] & 15) * 4;
            uint32_t dlen = ip_len - ihl - hl, seq = get32(tcp + 4);
            if (flags & 0x04) rx_reset = 1;
            else if (seq == rx_next && dlen) {
                if (rx_bytes < sizeof rx_data)
                    memcpy(rx_data + rx_bytes, tcp + hl, dlen < sizeof rx_data - rx_bytes ? dlen : sizeof rx_data - rx_bytes);
                rx_next += dlen, rx_bytes += dlen;
            }
            if (rx_acking && dlen) send_tcp(rx_port, get16(tcp), rx_seq, rx_next, 0x10, NULL);
        }
    }
}

static void tick(void) {
    hv_virtio_net_poll(n);
    hv_net_poll();
}

static int wait_for(_Atomic int *what, int at_least) {
    for (int i = 0; i < 300 && *what < at_least; i++) {
        tick();
        usleep(10000);
    }
    return *what >= at_least;
}

/* Connect the guest's port to the server and say hello, its SYN offering window scale wscale (-1: none); the
   server's side of it, or -1. */
static int connect_guest(int listener, uint16_t sport, uint16_t dport, uint32_t *next_seq, int wscale) {
    int before = synacks;
    send_tcp_opt(sport, dport, 1000, 0, 0x02, NULL, wscale);
    struct pollfd p = {listener, POLLIN, 0};
    if (poll(&p, 1, 3000) != 1) return -1;
    int s = accept(listener, NULL, NULL);
    if (!wait_for(&synacks, before + 1)) return -1;
    send_tcp(sport, dport, 1001, synack_seq + 1, 0x10, NULL);
    send_tcp(sport, dport, 1001, synack_seq + 1, 0x18, "hi");
    *next_seq = 1003;
    return s;
}

static int read_some(int s, char *buf, size_t len) {
    struct pollfd p = {s, POLLIN, 0};
    for (int i = 0; i < 300; i++) {
        tick();
        if (poll(&p, 1, 10) == 1) return (int)read(s, buf, len);
    }
    return -2; /* nothing in 3 s */
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&m, 4 << 20) != 0) return 1;
    HvGic *gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    HvPcie *bus = hv_pcie_create(&m, gic, 0x10000000, 0x10000000, 0x50000000, 0x10000000, 32 + 4);
    n = hv_virtio_net_create(&m, bus, guest_mac);
    hv_net_watch = watch;

    /* The DNS server, UDP and TCP on one port of this Mac's loopback */
    for (int tries = 0; tries < 20 && dns_udp < 0; tries++) {
        struct sockaddr_in da = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        socklen_t dl = sizeof da;
        if (dns_tcp >= 0) close(dns_tcp);
        dns_tcp = socket(AF_INET, SOCK_STREAM, 0);
        bind(dns_tcp, (struct sockaddr *)&da, sizeof da);
        listen(dns_tcp, 4);
        getsockname(dns_tcp, (struct sockaddr *)&da, &dl);
        dns_udp = socket(AF_INET, SOCK_DGRAM, 0);
        if (bind(dns_udp, (struct sockaddr *)&da, sizeof da) != 0) close(dns_udp), dns_udp = -1;
        else {
            char env[64];
            snprintf(env, sizeof env, "127.0.0.1:%u", ntohs(da.sin_port));
            setenv("HV_DNS", env, 1);
        }
    }
    check("a DNS server to ask", dns_udp >= 0, 1);
    pthread_t dt;
    pthread_create(&dt, NULL, dns_server, NULL);

    hv_net_ports_standard(NULL, 0); /* none of this Mac's own ports: not what this tests */
    hv_net_ports_loopback(1), hv_mdns_local_only(1); /* nothing here is reachable from the MacBook's network */
    check("the NAT starts", hv_net_attach(&m, n), 0);
    check("the card hooked to it", n->carrier_changed != NULL, 1);

    /* A server on this Mac's loopback: the guest reaches it at 10.0.2.2 */
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t sl = sizeof sa;
    bind(listener, (struct sockaddr *)&sa, sizeof sa);
    listen(listener, 4);
    getsockname(listener, (struct sockaddr *)&sa, &sl);
    uint16_t port = ntohs(sa.sin_port);

    send_arp(1, NULL, host_ip);
    check("the NAT answers ARP", wait_for(&arp_replies, 1), 1);
    uint32_t seq;
    int s = connect_guest(listener, 40000, port, &seq, 6);
    check("a guest connection reaches the server", s >= 0, 1);
    char wide[96];
    check("every port this test listens on is on 127.0.0.1 only", open_beyond_loopback(wide, sizeof wide), 0);
    if (wide[0]) printf("  %s\n", wide);
    check("its SYN offered window scaling: the SYN-ACK offers it back", synack_wscale >= 1 && synack_wscale <= 14, 1);
    char buf[16] = {0};
    check("its data arrives", read_some(s, buf, sizeof buf) == 2 && !memcmp(buf, "hi", 2), 1);

    /* The host loses its network (cable out), then has it back (cable in): the NAT starts afresh */
    hv_virtio_net_set_carrier(n, 0, hv_virtio_net_now() - 4000000000ull);
    tick();
    check("cable out", hv_virtio_net_link_up(n), 0);
    hv_virtio_net_set_carrier(n, 1, hv_virtio_net_now() - 2000000000ull);
    tick();
    check("cable back in", hv_virtio_net_link_up(n), 1);
    check("the old connection's host side closed (the server sees it end)", read_some(s, buf, sizeof buf), 0);
    close(s);

    /* The guest's next packet on the old connection: a reset at once, not silence */
    int before = resets;
    send_tcp(40000, port, seq, synack_seq + 1, 0x18, "again");
    check("the old connection is reset", wait_for(&resets, before + 1), 1);
    uint64_t lost_to, lost_from, too_long, nat_resets;
    hv_net_losses(&lost_to, &lost_from, &too_long, &nat_resets);
    check("and the reset counted", nat_resets >= 1 && nat_resets == (uint64_t)resets, 1);
    char why[256];
    hv_net_last_reset(why, sizeof why);
    printf("  log: %s\n", why);
    check("the log names the connection and why", strstr(why, "reset to the Mac, port 40000 to 10.0.2.2:") && strstr(why, "opened ") &&
          strstr(why, "a packet for a connection the NAT does not have"), 1);
    check("nothing dropped (queues or too long)", (long long)(lost_to + lost_from + too_long), 0);

    /* And a new connection works */
    s = connect_guest(listener, 40001, port, &seq, -1);
    check("a new connection after it reaches the server", s >= 0, 1);
    check("its SYN offered no window scaling: nor does the SYN-ACK", synack_wscale, -1);
    memset(buf, 0, sizeof buf);
    check("its data arrives", s >= 0 && read_some(s, buf, sizeof buf) == 2 && !memcmp(buf, "hi", 2), 1);
    if (s >= 0) close(s);

    /* The far side sends a download and then resets the connection (a server closing with SO_LINGER 0)
       while most of it still waits in the NAT for the guest, which is slow to acknowledge. All of it reaches
       the guest, and then the reset, as it would without the NAT: the NAT acknowledged that data to the
       server, so nobody else has it to send again. */
    enum { DOWNLOAD = 300 * 1024 };
    rx_port = 40002, rx_next = 0, rx_bytes = 0, rx_reset = 0, rx_acking = 0;
    s = connect_guest(listener, 40002, port, &seq, 6);
    check("a connection for a download", s >= 0, 1);
    rx_next = synack_seq + 1;
    if (s >= 0) {
        read_some(s, buf, sizeof buf);
        static char data[DOWNLOAD];
        for (int i = 0; i < DOWNLOAD; i++) data[i] = (char)i;
        size_t off = 0;
        while (off < DOWNLOAD) {
            ssize_t w = write(s, data + off, DOWNLOAD - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
        int unsent = 1;
        for (int i = 0; i < 300 && unsent; i++) { /* until the NAT's side has acknowledged all of it */
            socklen_t ul = sizeof unsent;
            getsockopt(s, SOL_SOCKET, SO_NWRITE, &unsent, &ul);
            if (unsent) usleep(10000);
        }
        check("the server's data all taken by the NAT", unsent, 0);
        struct linger lg = {1, 0};
        setsockopt(s, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
        close(s);
        usleep(200000); /* the NAT reads the reset */
        tick();
        check("the guest has only had a little of it yet", rx_bytes < DOWNLOAD, 1);
        rx_acking = 1;
        send_tcp(40002, port, 1003, rx_next, 0x10, NULL);
        for (int i = 0; i < 500 && !rx_reset; i++) {
            tick();
            usleep(10000);
        }
        check("all of the download reaches the guest", rx_bytes, DOWNLOAD);
        check("and then the reset", rx_reset, 1);
        hv_net_last_reset(why, sizeof why);
        printf("  log: %s\n", why);
        check("which the log puts down to the far side", strstr(why, "port 40002") && strstr(why, ": the far side reset it"), 1);
    }

    /* DNS: the answer is too long for UDP. The guest is told so (TC), as the server said... */
    static const uint8_t query[] = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
                                    3, 'b', 'i', 'g', 4, 't', 'e', 's', 't', 0, 0, 1, 0, 1};
    send_dns_udp(query, sizeof query);
    check("a UDP answer from 10.0.2.3", wait_for(&dns_answers, 1), 1);
    check("marked truncated", (dns_flags & 0x8200) == 0x8200, 1);

    /* ...and asks again over TCP to 10.0.2.3:53, which gets the whole of it */
    tcp_dst = dns_ip;
    rx_port = 40003, rx_bytes = 0, rx_reset = 0, rx_acking = 1, rx_seq = 1001;
    int before_syn = synacks;
    send_tcp_opt(40003, 53, 1000, 0, 0x02, NULL, 6);
    check("TCP to 10.0.2.3:53 is answered", wait_for(&synacks, before_syn + 1), 1);
    rx_next = synack_seq + 1;
    uint8_t framed[2 + sizeof query];
    put16(framed, sizeof query), memcpy(framed + 2, query, sizeof query);
    rx_seq = 1001 + sizeof framed;
    send_tcp(40003, 53, 1001, rx_next, 0x10, NULL);
    send_seg(40003, 53, 1001, rx_next, 0x18, framed, sizeof framed, -1);
    size_t want = 2 + sizeof query + BIG_ANSWERS * 16;
    for (int i = 0; i < 500 && rx_bytes < want && !rx_reset; i++) {
        tick();
        usleep(10000);
    }
    check("the query reached the DNS server over TCP", dns_tcp_queries, 1);
    check("the whole answer came back over TCP", rx_bytes, (long long)want);
    check("framed, for this query, with every address, not truncated",
          get16(rx_data) == want - 2 && get16(rx_data + 2) == 0x1234 && get16(rx_data + 2 + 6) == BIG_ANSWERS &&
              !(rx_data[2 + 2] & 0x02),
          1);
    send_tcp(40003, 53, rx_seq, rx_next, 0x11, NULL); /* the guest closes */

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
