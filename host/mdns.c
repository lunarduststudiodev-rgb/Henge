/* The Mac's name and services on the network, and the network's devices in the Mac, as a MacBook's (design:
   the port forwarding hostfwd notes, 5, 5a and 5b).

   The Mac's mDNSResponder announces its name (an A record, "My-Mac.local" -> 10.0.2.15) and the services it
   offers (PTR, SRV, TXT) on its network card; behind Henge's NAT nobody hears them. net_slirp.c hands this file
   every mDNS frame the Mac sends (never libslirp). What the Mac says about itself is kept while it lives (the
   Mac's TTLs, refreshed by asking it again at 80-95% as any mDNS cache does, dropped on its goodbye), and
   registered on the device's own network through dns_sd: the name as an A record on each of the device's
   interfaces with that interface's address, and each service under the same instance name, type, port and TXT,
   on that name. With or without Henge Tools: the Mac itself is the authority.

   - A service is announced only while its port is open on the device (net_forward.c), and its port is opened
     whether or not the helper is linked (hv_net_ports_advertised): nothing dead is announced.
   - A name another device on the network already has: the Mac's name and services are withdrawn, and Henge picks
     no name of its own. The Mac takes another as a Mac does when it sees the clash, and that one is announced;
     Details says so meanwhile.
   - Held (Henge off the screen, the Mac paused: net_forward.c's hold): everything is withdrawn, with goodbyes,
     and registered again on release, after asking the Mac again.
   - Caps: 64 services, 8 registration changes a second, TXT records up to 1300 bytes.

   The other way (5b, at the end of this file): what the device's Bonjour finds on its network is told to the Mac,
   so its Finder, printing and `.local` names see the network as a MacBook's would.

   Everything here runs on the network thread: dns_sd's one shared connection is in its poll set. The parser reads
   what the guest wrote, so it is strict (hv_dns_parse). */
#include "mdns.h"
#include "net_forward.h"

#include <arpa/inet.h>
#include <dns_sd.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

#define T_A 1
#define T_PTR 12
#define T_TXT 16
#define T_SRV 33
#define MDNS_PORT 5353
#define MAX_SERVICES 64
#define MAX_TYPES 32
#define MAX_TXT 1300
#define MAX_IFS 8
#define CHANGES_PER_S 8
#define CLASH_WAIT_MS 10000 /* a clashed service the Mac hasn't renamed by then is announced as "<name> (2)" */

/* ---------------------------------------------------------------- the wire */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) << 16 | rd16(p + 2); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }

/* A name at *at (moved past it). Pointers only backwards, from where each is: no loop can be made. */
static int read_name(const uint8_t *m, size_t len, size_t *at, HvDnsName *out) {
    size_t p = *at, o = 0;
    int jumped = 0, hops = 0;
    for (;;) {
        if (p >= len) return -1;
        uint8_t l = m[p];
        if ((l & 0xc0) == 0xc0) {
            if (p + 1 >= len) return -1;
            size_t to = (size_t)(l & 0x3f) << 8 | m[p + 1];
            if (!jumped) *at = p + 2;
            if (to >= p || ++hops > 64) return -1;
            jumped = 1;
            p = to;
            continue;
        }
        if (l & 0xc0) return -1; /* the extended label types: not in mDNS */
        if (l == 0) {
            out->b[o++] = 0;
            out->len = (uint16_t)o;
            if (!jumped) *at = p + 1;
            return 0;
        }
        if (p + 1 + l > len || o + 1 + l + 1 > 255) return -1;
        out->b[o++] = l;
        memcpy(out->b + o, m + p + 1, l);
        o += l;
        p += 1 + (size_t)l;
    }
}

/* A TXT's data: length-prefixed strings that end exactly with it. */
static int txt_ok(const uint8_t *t, size_t len) {
    size_t p = 0;
    while (p < len) p += 1 + (size_t)t[p];
    return p == len;
}

int hv_dns_parse(const uint8_t *m, size_t len, HvDnsMsg *o) {
    if (len < 12 || len > HV_DNS_MAX_MSG) return -1;
    o->id = rd16(m), o->flags = rd16(m + 2);
    unsigned qd = rd16(m + 4), an = rd16(m + 6), ns = rd16(m + 8), ar = rd16(m + 10), nrr = an + ns + ar;
    o->nq = o->nrr = o->nan = 0;
    if (qd > HV_DNS_MAX_Q || nrr > HV_DNS_MAX_RR || 12 + (size_t)qd * 5 + (size_t)nrr * 11 > len) return -1;
    size_t at = 12;
    for (unsigned i = 0; i < qd; i++) {
        HvDnsRR *q = &o->q[i];
        if (read_name(m, len, &at, &q->name) || at + 4 > len) return -1;
        q->type = rd16(m + at);
        q->cls = rd16(m + at + 2) & 0x7fff, q->top = m[at + 2] >> 7;
        q->ttl = 0, q->rdlen = 0, q->rdata = NULL, q->target.len = 0, q->port = 0, q->a = 0;
        at += 4;
    }
    for (unsigned i = 0; i < nrr; i++) {
        HvDnsRR *r = &o->rr[i];
        if (read_name(m, len, &at, &r->name) || at + 10 > len) return -1;
        r->type = rd16(m + at);
        r->cls = rd16(m + at + 2) & 0x7fff, r->top = m[at + 2] >> 7;
        r->ttl = rd32(m + at + 4);
        r->rdlen = rd16(m + at + 8);
        at += 10;
        if (r->rdlen > len - at) return -1;
        size_t end = at + r->rdlen, p = at;
        r->rdata = m + at, r->target.len = 0, r->port = 0, r->a = 0;
        switch (r->type) {
        case T_A:
            if (r->rdlen != 4) return -1;
            memcpy(&r->a, m + at, 4);
            break;
        case T_PTR:
            if (read_name(m, end, &p, &r->target) || p != end) return -1;
            break;
        case T_SRV:
            if (r->rdlen < 7) return -1;
            r->port = rd16(m + at + 4);
            p = at + 6;
            if (read_name(m, end, &p, &r->target) || p != end) return -1;
            break;
        case T_TXT:
            if (!txt_ok(m + p, end - p)) return -1;
            break;
        }
        at = end;
    }
    o->nq = (int)qd, o->nrr = (int)nrr, o->nan = (int)an;
    return 0;
}

static uint8_t lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; }

/* Length bytes are at most 63, never a letter: the whole buffer compares as it is. */
int hv_dns_name_eq(const HvDnsName *a, const HvDnsName *b) {
    if (a->len != b->len) return 0;
    for (int i = 0; i < a->len; i++)
        if (lower(a->b[i]) != lower(b->b[i])) return 0;
    return 1;
}

int hv_dns_name_text(const HvDnsName *n, char *out, size_t cap) {
    size_t o = 0, p = 0;
    if (!n->len || cap < 2) return -1;
    if (!n->b[0]) return out[0] = '.', out[1] = 0, 0;
    while (p < n->len && n->b[p]) {
        uint8_t l = n->b[p++];
        for (int i = 0; i < l; i++) {
            uint8_t c = n->b[p++];
            char t[8];
            int k;
            if (c == '.' || c == '\\') t[0] = '\\', t[1] = (char)c, k = 2;
            else if (c < 0x20 || c == 0x7f) k = snprintf(t, sizeof t, "\\%03u", c);
            else t[0] = (char)c, k = 1;
            if (o + (size_t)k + 2 > cap) return -1;
            memcpy(out + o, t, (size_t)k);
            o += (size_t)k;
        }
        out[o++] = '.';
    }
    out[o] = 0;
    return 0;
}

int hv_dns_name_from(HvDnsName *n, const char *text) {
    size_t o = 0;
    const char *s = text;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        if (l == 0 || l > 63 || o + 1 + l + 1 > 255) return -1;
        n->b[o++] = (uint8_t)l;
        memcpy(n->b + o, s, l);
        o += l;
        s += l + (dot ? 1 : 0);
    }
    n->b[o++] = 0;
    n->len = (uint16_t)o;
    return 0;
}

int hv_dns_name_parse(HvDnsName *n, const char *text) {
    size_t o = 0;
    const char *s = text;
    if (!strcmp(s, ".")) s++;
    while (*s) {
        size_t at = o++;
        int l = 0;
        while (*s && *s != '.') {
            uint8_t c = (uint8_t)*s++;
            if (c == '\\') {
                if (s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9' && s[2] >= '0' && s[2] <= '9') {
                    int v = (s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0');
                    if (v > 255) return -1;
                    c = (uint8_t)v, s += 3;
                } else if (*s && (*s < '0' || *s > '9')) { /* a digit starts \DDD, three of them */
                    c = (uint8_t)*s++;
                } else {
                    return -1;
                }
            }
            if (l == 63 || o + 2 > 255) return -1;
            n->b[o++] = c, l++;
        }
        if (!l) return -1;
        n->b[at] = (uint8_t)l;
        if (*s == '.') s++;
    }
    n->b[o++] = 0;
    n->len = (uint16_t)o;
    return 0;
}

static int nlabels(const HvDnsName *n) {
    int k = 0;
    for (int p = 0; p < n->len && n->b[p]; p += 1 + n->b[p]) k++;
    return k;
}

/* Label i (from 0), and its length. */
static const uint8_t *label(const HvDnsName *n, int i, int *len) {
    int p = 0;
    for (; i > 0 && p < n->len && n->b[p]; i--) p += 1 + n->b[p];
    *len = n->b[p];
    return n->b + p + 1;
}

static int label_is(const HvDnsName *n, int i, const char *s) {
    int l;
    const uint8_t *b = label(n, i, &l);
    if ((size_t)l != strlen(s)) return 0;
    for (int k = 0; k < l; k++)
        if (lower(b[k]) != (uint8_t)s[k]) return 0;
    return 1;
}

/* The name without its first label. */
static void parent(const HvDnsName *n, HvDnsName *out) {
    int skip = n->b[0] ? 1 + n->b[0] : 0;
    out->len = (uint16_t)(n->len - skip);
    memcpy(out->b, n->b + skip, out->len);
}

/* "_x._tcp.local" or "_x._udp.local". */
static int is_type(const HvDnsName *n) {
    int l;
    return nlabels(n) == 3 && label(n, 0, &l)[0] == '_' && l > 1 && (label_is(n, 1, "_tcp") || label_is(n, 1, "_udp")) &&
           label_is(n, 2, "local");
}

/* "<instance>._x._tcp.local" */
static int is_instance(const HvDnsName *n) {
    if (nlabels(n) != 4) return 0;
    HvDnsName t;
    parent(n, &t);
    return is_type(&t);
}

/* "<host>.local" */
static int is_host(const HvDnsName *n) { return nlabels(n) == 2 && label_is(n, 1, "local"); }

/* ---------------------------------------------------------------- state (the network thread's) */

typedef struct Rec {
    int64_t got;
    uint32_t ttl;
    uint8_t has, asked; /* asked: refresh queries sent since (at 80, 85, 90 and 95% of the TTL) */
} Rec;

typedef struct Service {
    uint8_t used, txt_only; /* txt_only: _device-info, a TXT record and nothing else */
    HvDnsName inst, type;
    Rec ptr, srv, txt;
    uint16_t port;
    HvDnsName target;
    uint8_t txt_b[MAX_TXT];
    uint16_t txt_len;
    /* on the device's network */
    DNSServiceRef ref;
    DNSRecordRef rec;
    uint8_t reg, denied, failed, alt, reg_alt; /* alt: announced as "<name> (alt + 1)" (service_clash) */
    HvDnsName reg_inst;                        /* the name it is announced under */
    int64_t clashed_at;
    uint16_t reg_port;
    uint32_t reg_txt, reg_gen;
} Service;

typedef struct Iface {
    uint32_t index;
    struct in_addr addr;
    DNSRecordRef rec;
} Iface;

static HvMdnsDeliver deliver;
static int local_only;
static HvDnsMsg msg; /* the one being read */
static Service svc[MAX_SERVICES];
static HvDnsName types[MAX_TYPES];
static Rec type_rec[MAX_TYPES];
static int ntypes;
static int full_logged;

static HvDnsName mac_name; /* as the Mac says it */
static Rec mac_a;
static HvDnsName our_name; /* registered (or found taken) */
static uint32_t name_gen;  /* bumped whenever our_name's registration starts afresh */
static int a_registered;
/* Another device on the network has our_name. Henge never picks a name of its own: it withdraws the Mac's, and the
   Mac, seeing the clash itself (2b), takes another as a Mac does ("<name>-2"), and announces it; Henge follows. */
static int taken;

static DNSServiceRef conn;
static int64_t conn_retry;
static Iface ifs[MAX_IFS];
static int nifs;
static int64_t ifs_checked;

static int held = -1; /* as net_forward.c last said (-1: not read yet) */
static int64_t held_since;
static unsigned fwd_gen = ~0u;
static HvFwdStatus fwd[HV_FWD_MAX + 128];
static int nfwd;
static HvFwdPort adv_sent[MAX_SERVICES];
static int nadv_sent = -1;

static int dirty = 1;
static int64_t ask_at[4]; /* when to ask the Mac what it is and offers (0: not) */
static int64_t window, next_scan;
static int changes;

static pthread_mutex_t status_mu = PTHREAD_MUTEX_INITIALIZER;
static char status_name[260];
static int status_taken, status_services, status_denied, status_unlisted, status_seen;
static char status_seen_list[2048];
static _Atomic uint64_t renames;

static int64_t now_cached;

/* 5b, at the end of this file. */
static void mark_ours(const HvDnsName *n, int clashed);
static void ours_pass(int64_t ms);
static int lan_defend(const HvDnsName *n);
static void lan_query(const uint8_t *mac, uint16_t port);
static void lan_stop(void);
static int lan_seen(char *out, size_t cap);
static uint32_t dev_addr[32]; /* every IPv4 address the device has (network order) */
static int ndev_addr;

void hv_mdns_local_only(int on) { local_only = on; }

uint64_t hv_mdns_renames(void) { return atomic_load(&renames); }

void hv_mdns_status(char *name, size_t n, int *name_taken, int *services, int *denied, int *unlisted, int *seen) {
    pthread_mutex_lock(&status_mu);
    if (name && n) snprintf(name, n, "%s", status_name);
    if (name_taken) *name_taken = status_taken;
    if (services) *services = status_services;
    if (denied) *denied = status_denied;
    if (unlisted) *unlisted = status_unlisted;
    if (seen) *seen = status_seen;
    pthread_mutex_unlock(&status_mu);
}

int hv_mdns_seen(char *list, size_t n) {
    pthread_mutex_lock(&status_mu);
    if (list && n) snprintf(list, n, "%s", status_seen_list);
    int k = status_seen;
    pthread_mutex_unlock(&status_mu);
    return k;
}

static uint32_t fnv(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h ^ (uint32_t)n;
}

static void text_of(const HvDnsName *n, char *out, size_t cap) {
    if (hv_dns_name_text(n, out, cap)) snprintf(out, cap, "(a long name)");
}

/* ---------------------------------------------------------------- frames to the guest */

static uint16_t csum(const uint8_t *p, size_t len, uint32_t sum) {
    for (size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)p[i] << 8 | p[i + 1];
    if (len & 1) sum += (uint32_t)p[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static int mtu = 1500;

/* The largest DNS message one frame to the Mac carries: the network's MTU less the IP and UDP headers. */
static size_t max_msg(void) { return (size_t)mtu - 28 < HV_DNS_MAX_MSG ? (size_t)mtu - 28 : HV_DNS_MAX_MSG; }

/* A DNS message to the Mac, from the gateway, 10.0.2.2:5353, IP TTL 255 (RFC 6762 11): to 224.0.0.251:5353
   (mac NULL), or to the Mac's own address at that MAC and port (a unicast answer). */
static void send_dns(const uint8_t *dns, size_t len, const uint8_t *mac, uint16_t port) {
    static uint8_t f[14 + 20 + 8 + HV_DNS_MAX_MSG];
    static uint16_t ip_id;
    if (!deliver || len > max_msg()) return;
    static const uint8_t group_mac[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb}, gw_mac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
    static const uint8_t gw[4] = {10, 0, 2, 2}, group[4] = {224, 0, 0, 251}, guest[4] = {10, 0, 2, 15};
    const uint8_t *to = mac ? guest : group;
    memcpy(f, mac ? mac : group_mac, 6), memcpy(f + 6, gw_mac, 6), wr16(f + 12, 0x0800);
    uint8_t *ip = f + 14, *udp = ip + 20;
    memset(ip, 0, 20);
    ip[0] = 0x45, wr16(ip + 2, (uint16_t)(28 + len)), wr16(ip + 4, ip_id++), ip[8] = 255, ip[9] = 17;
    memcpy(ip + 12, gw, 4), memcpy(ip + 16, to, 4);
    wr16(ip + 10, csum(ip, 20, 0));
    wr16(udp, MDNS_PORT), wr16(udp + 2, mac ? port : MDNS_PORT), wr16(udp + 4, (uint16_t)(8 + len)), wr16(udp + 6, 0);
    memcpy(udp + 8, dns, len);
    uint8_t pseudo[12] = {10, 0, 2, 2, 0, 0, 0, 0, 0, 17};
    memcpy(pseudo + 4, to, 4);
    wr16(pseudo + 10, (uint16_t)(8 + len));
    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2) sum += (uint32_t)pseudo[i] << 8 | pseudo[i + 1];
    uint16_t c = csum(udp, 8 + len, sum);
    wr16(udp + 6, c ? c : 0xffff);
    deliver(f, (uint32_t)(42 + len));
}

static void send_to_guest(const uint8_t *dns, size_t len) { send_dns(dns, len, NULL, MDNS_PORT); }

/* Questions to the Mac, batched: sent when the batch fills a message within the MTU, or flush() is called. */
static uint8_t q_buf[HV_DNS_MAX_MSG];
static size_t q_len;
static int q_n;

static void flush_asks(void) {
    if (!q_n) return;
    wr16(q_buf + 4, (uint16_t)q_n);
    send_to_guest(q_buf, q_len);
    q_n = 0;
}

static void ask(const HvDnsName *name, uint16_t type) {
    if (q_n && q_len + name->len + 4 > max_msg()) flush_asks();
    if (!q_n) memset(q_buf, 0, 12), q_len = 12;
    memcpy(q_buf + q_len, name->b, name->len);
    q_len += name->len;
    wr16(q_buf + q_len, type), wr16(q_buf + q_len + 2, 1); /* IN, a multicast answer (QM) */
    q_len += 4;
    q_n++;
}

/* What the Mac is called and offers: its services' types, the name its address has, and each type known. */
static void ask_everything(void) {
    HvDnsName n;
    hv_dns_name_from(&n, "_services._dns-sd._udp.local");
    ask(&n, T_PTR);
    hv_dns_name_from(&n, "15.2.0.10.in-addr.arpa");
    ask(&n, T_PTR);
    for (int i = 0; i < ntypes; i++) ask(&types[i], T_PTR);
    if (mac_a.has) ask(&mac_name, T_A);
    flush_asks();
}

/* ---------------------------------------------------------------- what the Mac says */

static void rec_set(Rec *r, uint32_t ttl) {
    if (!ttl) {
        r->has = 0; /* its goodbye */
        return;
    }
    r->has = 1, r->got = now_cached, r->ttl = ttl, r->asked = 0;
}

static Service *service(const HvDnsName *inst, int make) {
    Service *free_one = NULL;
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (svc[i].used && hv_dns_name_eq(&svc[i].inst, inst)) return &svc[i];
        if (!svc[i].used && !svc[i].reg && !free_one) free_one = &svc[i];
    }
    if (!make) return NULL;
    if (!free_one) {
        if (!full_logged++) fprintf(stderr, "hengevm: mdns: the Mac offers more than %d services: the rest aren't announced\n", MAX_SERVICES);
        return NULL;
    }
    memset(free_one, 0, sizeof *free_one);
    free_one->used = 1;
    free_one->inst = *inst;
    parent(inst, &free_one->type);
    return free_one;
}

static int type_known(const HvDnsName *t) {
    for (int i = 0; i < ntypes; i++)
        if (hv_dns_name_eq(&types[i], t)) return i;
    return -1;
}

static void learn(const HvDnsRR *r) {
    if (r->cls != 1) return;
    static HvDnsName services_name, reverse_name;
    if (!services_name.len) {
        hv_dns_name_from(&services_name, "_services._dns-sd._udp.local");
        hv_dns_name_from(&reverse_name, "15.2.0.10.in-addr.arpa");
    }
    switch (r->type) {
    case T_A: {
        static const uint8_t guest[4] = {10, 0, 2, 15};
        if (!is_host(&r->name) || memcmp(&r->a, guest, 4)) return;
        if (!r->ttl) {
            if (mac_a.has && hv_dns_name_eq(&r->name, &mac_name)) mac_a.has = 0;
            return;
        }
        if (!mac_a.has || !hv_dns_name_eq(&r->name, &mac_name)) {
            char t[300];
            text_of(&r->name, t, sizeof t);
            fprintf(stderr, "hengevm: mdns: the Mac is called %s\n", t);
            mac_name = r->name;
        }
        rec_set(&mac_a, r->ttl);
        return;
    }
    case T_PTR:
        if (hv_dns_name_eq(&r->name, &services_name)) {
            if (!is_type(&r->target)) return;
            int i = type_known(&r->target);
            if (i < 0 && r->ttl && ntypes < MAX_TYPES) {
                i = ntypes++;
                types[i] = r->target;
                ask(&types[i], T_PTR); /* its instances */
            }
            if (i >= 0) rec_set(&type_rec[i], r->ttl);
            return;
        }
        if (hv_dns_name_eq(&r->name, &reverse_name)) { /* the Mac's name, from its address */
            if (is_host(&r->target) && r->ttl && (!mac_a.has || !hv_dns_name_eq(&r->target, &mac_name))) ask(&r->target, T_A);
            return;
        }
        if (is_type(&r->name) && is_instance(&r->target)) {
            HvDnsName t;
            parent(&r->target, &t);
            if (!hv_dns_name_eq(&t, &r->name)) return;
            Service *s = service(&r->target, r->ttl != 0);
            if (!s) return;
            rec_set(&s->ptr, r->ttl);
            if (r->ttl && !s->srv.has) ask(&s->inst, T_SRV), ask(&s->inst, T_TXT);
        }
        return;
    case T_SRV: {
        if (!is_instance(&r->name)) return;
        Service *s = service(&r->name, r->ttl != 0);
        if (!s) return;
        rec_set(&s->srv, r->ttl);
        if (r->ttl) {
            s->port = r->port;
            s->target = r->target;
            if (is_host(&r->target) && (!mac_a.has || !hv_dns_name_eq(&r->target, &mac_name))) ask(&r->target, T_A);
        }
        return;
    }
    case T_TXT: {
        if (!is_instance(&r->name)) return;
        HvDnsName t;
        parent(&r->name, &t);
        int device_info = label_is(&t, 0, "_device-info");
        Service *s = service(&r->name, r->ttl != 0);
        if (!s) return;
        s->txt_only = (uint8_t)device_info;
        rec_set(&s->txt, r->ttl);
        if (!r->ttl) return;
        /* Up to MAX_TXT bytes (what fits one packet on the network); a longer one is left out, not the service. */
        size_t keep = r->rdlen <= MAX_TXT ? r->rdlen : 0;
        if (!keep && r->rdlen) {
            char n[300];
            text_of(&r->name, n, sizeof n);
            fprintf(stderr, "hengevm: mdns: %s's TXT is %u bytes, over %d: announced without it\n", n, r->rdlen, MAX_TXT);
        }
        memcpy(s->txt_b, r->rdata, keep);
        s->txt_len = (uint16_t)keep;
        return;
    }
    }
}

int hv_mdns_take(const uint8_t *f, uint32_t len) {
    if (len < 42 || rd16(f + 12) != 0x0800) return 0;
    const uint8_t *ip = f + 14;
    size_t ihl = (size_t)(ip[0] & 15) * 4;
    if ((ip[0] >> 4) != 4 || ihl < 20 || ip[9] != 17 || 14 + ihl + 8 > len) return 0;
    const uint8_t *udp = ip + ihl;
    static const uint8_t group[4] = {224, 0, 0, 251}, gw[4] = {10, 0, 2, 2}, guest[4] = {10, 0, 2, 15};
    if (rd16(udp + 2) != MDNS_PORT || (memcmp(ip + 16, group, 4) && memcmp(ip + 16, gw, 4))) return 0;
    /* mDNS: Henge's from here on, whatever it holds. */
    size_t ip_len = rd16(ip + 2), udp_len = rd16(udp + 4);
    if ((rd16(ip + 6) & 0x3fff) || ip_len > len - 14 || ip_len < ihl + 8 || udp_len < 8 || udp_len > ip_len - ihl) return 1;
    if (memcmp(ip + 12, guest, 4)) return 1;
    if (hv_dns_parse(udp + 8, udp_len - 8, &msg) || (msg.flags & 0x7800)) return 1; /* standard queries and responses */
    if (!(msg.flags & 0x8000)) { /* the Mac asks: of the network's devices (5b) */
        lan_query(f + 6, rd16(udp));
        return 1;
    }
    if (rd16(udp) != MDNS_PORT) return 1; /* only the Mac's own responder speaks for it */
    for (int i = 0; i < msg.nrr; i++) learn(&msg.rr[i]);
    flush_asks();
    dirty = 1;
    return 1;
}

/* ---------------------------------------------------------------- the device's network (dns_sd) */

static void set_status(void) {
    char n[300] = "";
    int count = 0, unlisted = 0;
    if (a_registered || taken) text_of(&our_name, n, sizeof n);
    size_t l = strlen(n);
    if (l && n[l - 1] == '.') n[l - 1] = 0;
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (svc[i].reg && !svc[i].txt_only) count++;
        if (svc[i].used && svc[i].denied) unlisted++;
    }
    char seen_list[sizeof status_seen_list];
    int seen = lan_seen(seen_list, sizeof seen_list);
    pthread_mutex_lock(&status_mu);
    snprintf(status_name, sizeof status_name, "%s", n);
    memcpy(status_seen_list, seen_list, sizeof seen_list);
    status_seen = seen;
    status_taken = taken;
    status_services = count;
    status_unlisted = unlisted;
    pthread_mutex_unlock(&status_mu);
}

static void note_denied(void) {
    pthread_mutex_lock(&status_mu);
    int was = status_denied;
    status_denied = 1;
    pthread_mutex_unlock(&status_mu);
    if (!was) fprintf(stderr, "hengevm: mdns: iOS refused: Henge's Local Network access is off, so the Mac isn't announced on the network\n");
}

/* iOS refused one service while the Mac's name went: its type isn't among those Henge declares (Info.plist's
   NSBonjourServices); an app can't announce others. */
static void service_denied(Service *s) {
    s->denied = 1;
    char n[300];
    text_of(&s->inst, n, sizeof n);
    fprintf(stderr, "hengevm: mdns: iOS doesn't let Henge announce %s: its type isn't one Henge declares\n", n);
}

/* Another device has our_name: it is withdrawn, with the services on it, until the Mac takes another (reconcile
   doesn't register a taken name again). Each clash found is counted. */
static void name_taken(void) {
    char was[300];
    text_of(&our_name, was, sizeof was);
    fprintf(stderr, "hengevm: mdns: another device on the network is called %s: the Mac's name is withdrawn until the Mac takes another\n", was);
    taken = 1;
    mark_ours(&our_name, 1);
    atomic_fetch_add(&renames, 1);
    dirty = 1;
}

static void DNSSD_API a_done(DNSServiceRef c, DNSRecordRef rec, DNSServiceFlags flags, DNSServiceErrorType err, void *ctx) {
    (void)c, (void)flags, (void)ctx;
    int mine = 0;
    for (int i = 0; i < nifs; i++) mine |= ifs[i].rec == rec;
    if (!mine || !err) return;
    if (err == kDNSServiceErr_NameConflict) {
        name_taken();
    } else if (err == kDNSServiceErr_PolicyDenied) {
        note_denied();
    } else {
        fprintf(stderr, "hengevm: mdns: the Mac's name can't be registered (dns_sd %d)\n", (int)err);
    }
    dirty = 1;
}

static void DNSSD_API txt_done(DNSServiceRef c, DNSRecordRef rec, DNSServiceFlags flags, DNSServiceErrorType err, void *ctx) {
    (void)c, (void)flags;
    Service *s = &svc[(intptr_t)ctx];
    if (s->rec != rec || !err) return;
    if (err == kDNSServiceErr_PolicyDenied) service_denied(s);
    else s->failed = 1, fprintf(stderr, "hengevm: mdns: the Mac's device information can't be announced (dns_sd %d)\n", (int)err);
    dirty = 1;
}

/* Another device's service has this name: found at once (on this device) or later (on the network). Not renamed
   here (kDNSServiceFlagsNoAutoRename: the Mac names its own): withdrawn, and that device's service is told to the Mac
   (inst_told), which renames its own as a MacBook does; the new name is announced as it comes. */
static void service_clash(Service *s) {
    char n[300];
    text_of(&s->inst, n, sizeof n);
    if (s->alt) { /* a name the Mac doesn't know (below) taken too: the next, as Bonjour goes on */
        if (s->alt < 9) s->alt++;
        else s->failed = 1, fprintf(stderr, "hengevm: mdns: %s: every name up to (10) is taken on the network: not announced\n", n);
        return;
    }
    fprintf(stderr, "hengevm: mdns: another device on the network has a service called %s: the Mac's is withdrawn until the Mac renames it\n", n);
    mark_ours(&s->inst, 1);
    /* The Mac is given CLASH_WAIT_MS to rename its own when the other device's service can be told to it; when it
       can't, the Mac won't, so "(2)" at once. */
    s->failed = 1, s->clashed_at = lan_defend(&s->inst) ? now_cached : now_cached - CLASH_WAIT_MS;
}

static void DNSSD_API reg_done(DNSServiceRef ref, DNSServiceFlags flags, DNSServiceErrorType err, const char *name,
                               const char *type, const char *domain, void *ctx) {
    (void)flags, (void)domain;
    Service *s = &svc[(intptr_t)ctx];
    if (s->ref != ref) return;
    if (!err) {
        fprintf(stderr, "hengevm: mdns: the Mac's \"%s\" (%s) is announced on the network, port %u\n", name, type, s->reg_port);
        return;
    }
    if (err == kDNSServiceErr_NameConflict) service_clash(s);
    else if (err == kDNSServiceErr_PolicyDenied) {
        service_denied(s);
    } else {
        s->failed = 1, fprintf(stderr, "hengevm: mdns: the Mac's \"%s\" (%s) can't be announced (dns_sd %d)\n", name, type, (int)err);
    }
    dirty = 1;
}

static void drop_conn(void) {
    lan_stop(); /* its lookups are on the connection */
    if (conn) DNSServiceRefDeallocate(conn); /* every registration on it goes with it */
    conn = NULL;
    for (int i = 0; i < MAX_SERVICES; i++) {
        if (svc[i].reg) mark_ours(&svc[i].reg_inst, 0); /* as unregister_service: its name is still in caches */
        svc[i].reg = 0, svc[i].ref = NULL, svc[i].rec = NULL;
    }
    if (a_registered) mark_ours(&our_name, 0);
    for (int i = 0; i < nifs; i++) ifs[i].rec = NULL;
    a_registered = 0;
}

static int may_change(int urgent) {
    if (now_cached - window >= 1000) window = now_cached, changes = 0;
    if (!urgent && changes >= CHANGES_PER_S) return 0;
    changes++;
    return 1;
}

static void unregister_service(Service *s) {
    if (!s->reg) return;
    if (conn) {
        if (s->txt_only) DNSServiceRemoveRecord(conn, s->rec, 0);
        else DNSServiceRefDeallocate(s->ref); /* its goodbyes */
        char n[300];
        text_of(&s->reg_inst, n, sizeof n);
        fprintf(stderr, "hengevm: mdns: %s is withdrawn from the network\n", n);
    }
    mark_ours(&s->reg_inst, 0);
    s->reg = 0, s->ref = NULL, s->rec = NULL;
}

static void unregister_name(void) {
    if (a_registered) mark_ours(&our_name, 0);
    for (int i = 0; i < nifs; i++) {
        if (conn && ifs[i].rec) DNSServiceRemoveRecord(conn, ifs[i].rec, 0);
        ifs[i].rec = NULL;
    }
    a_registered = 0;
}

static void register_name(void) {
    char text[300];
    if (hv_dns_name_text(&our_name, text, sizeof text)) return;
    for (int i = 0; i < nifs; i++) {
        DNSServiceErrorType e = DNSServiceRegisterRecord(conn, &ifs[i].rec, kDNSServiceFlagsUnique, ifs[i].index, text,
                                                         kDNSServiceType_A, kDNSServiceClass_IN, 4, &ifs[i].addr, 120, a_done, NULL);
        if (e) {
            ifs[i].rec = NULL;
            if (e == kDNSServiceErr_NameConflict) { /* known at once (a local-only name): withdrawn whole */
                name_taken();
                unregister_name();
                return;
            }
            if (e == kDNSServiceErr_PolicyDenied) note_denied();
            else fprintf(stderr, "hengevm: mdns: the Mac's name can't be registered (dns_sd %d)\n", (int)e);
        }
    }
    a_registered = 1; /* refused or not: tried again when the name changes, or on return from a hold */
    mark_ours(&our_name, -1);
    char ip[INET_ADDRSTRLEN] = "";
    if (nifs) inet_ntop(AF_INET, &ifs[0].addr, ip, sizeof ip);
    fprintf(stderr, "hengevm: mdns: the Mac is %s on the network (%d interface%s, %s%s)\n", text, nifs, nifs == 1 ? "" : "s",
            ip, nifs > 1 ? " and others" : "");
}

static int register_service(Service *s) {
    char type[300], host[300], inst[64];
    int l;
    const uint8_t *b = label(&s->inst, 0, &l);
    if (memchr(b, 0, (size_t)l)) return -1; /* a name dns_sd can't take as it is */
    const uint8_t *tail = b + l; /* the type and domain after the first label */
    size_t tail_len = s->inst.len - (size_t)(tail - s->inst.b);
    memcpy(inst, b, (size_t)l);
    inst[l] = 0;
    if (s->alt) { /* "<name> (n)", the name cut between UTF-8 characters to fit a label's 63 bytes */
        char suffix[8];
        int k = snprintf(suffix, sizeof suffix, " (%d)", s->alt + 1);
        if (l > 63 - k) l = 63 - k;
        while (l > 0 && ((uint8_t)inst[l] & 0xc0) == 0x80) l--;
        memcpy(inst + l, suffix, (size_t)k + 1);
        l += k;
    }
    /* The name in full, as announced. */
    HvDnsName full;
    if (1 + (size_t)l + tail_len > sizeof full.b) {
        s->failed = 1;
        fprintf(stderr, "hengevm: mdns: \"%s\" is too long a name with its type: not announced\n", inst);
        return -1;
    }
    full.b[0] = (uint8_t)l, memcpy(full.b + 1, inst, (size_t)l), memcpy(full.b + 1 + l, tail, tail_len);
    full.len = (uint16_t)(1 + l + tail_len);
    if (hv_dns_name_text(&our_name, host, sizeof host)) return -1;
    /* A name the Mac announces is the Mac's: a "(n)" Henge chose for another of its services moves to the next. */
    if (!s->alt)
        for (int i = 0; i < MAX_SERVICES; i++) {
            Service *o = &svc[i];
            if (o == s || !o->reg || !o->alt || !hv_dns_name_eq(&o->reg_inst, &full)) continue;
            unregister_service(o);
            service_clash(o); /* the next "(n)", as on a clash */
        }
    uint32_t index = local_only ? kDNSServiceInterfaceIndexLocalOnly : 0;
    DNSServiceErrorType e;
    if (s->txt_only) {
        char full[300];
        if (hv_dns_name_text(&s->inst, full, sizeof full)) return -1;
        e = DNSServiceRegisterRecord(conn, &s->rec, kDNSServiceFlagsUnique, index, full, kDNSServiceType_TXT,
                                     kDNSServiceClass_IN, s->txt_len, s->txt_b, 4500, txt_done, (void *)(intptr_t)(s - svc));
    } else {
        /* "_x._tcp": the type's first two labels. */
        HvDnsName t = s->type;
        int p = 1 + t.b[0];
        p += 1 + t.b[p];
        t.b[p] = 0, t.len = (uint16_t)(p + 1);
        if (hv_dns_name_text(&t, type, sizeof type)) return -1;
        type[strlen(type) - 1] = 0;
        uint8_t empty = 0;
        s->ref = conn;
        e = DNSServiceRegister(&s->ref, kDNSServiceFlagsShareConnection | kDNSServiceFlagsNoAutoRename, index, inst, type, "local.", host, htons(s->port),
                               s->txt_len ? s->txt_len : 1, s->txt_len ? s->txt_b : &empty, reg_done, (void *)(intptr_t)(s - svc));
    }
    if (e) {
        s->ref = NULL, s->rec = NULL;
        if (e == kDNSServiceErr_PolicyDenied) service_denied(s);
        else if (e == kDNSServiceErr_NameConflict && !s->txt_only) service_clash(s);
        else s->failed = 1, fprintf(stderr, "hengevm: mdns: the Mac's \"%s\" can't be announced (dns_sd %d)\n", inst, (int)e);
        return -1;
    }
    s->reg = 1, s->reg_port = s->port, s->reg_txt = fnv(s->txt_b, s->txt_len), s->reg_gen = name_gen, s->reg_alt = s->alt;
    s->reg_inst = full;
    mark_ours(&full, -1); /* under "(n)", that name: the Mac's own stays the other device's (clashed) */
    return 0;
}

/* The device's IPv4 interfaces where mDNS runs (Wi-Fi, Ethernet, Personal Hotspot's bridge). 1 if they changed. */
static int read_ifs(void) {
    Iface now[MAX_IFS];
    int n = 0;
    ndev_addr = 0;
    if (local_only) {
        now[0].index = kDNSServiceInterfaceIndexLocalOnly;
        now[0].addr.s_addr = htonl(INADDR_LOOPBACK);
        n = 1;
        dev_addr[ndev_addr++] = now[0].addr.s_addr;
    } else {
        struct ifaddrs *list;
        if (getifaddrs(&list) == 0) {
            for (struct ifaddrs *i = list; i; i = i->ifa_next) {
                if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && ndev_addr < (int)(sizeof dev_addr / sizeof *dev_addr))
                    dev_addr[ndev_addr++] = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr; /* every one: never "another device" */
                if (n == MAX_IFS) continue;
                unsigned fl = i->ifa_flags;
                if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(fl & IFF_UP) || !(fl & IFF_RUNNING) ||
                    !(fl & IFF_MULTICAST) || (fl & (IFF_LOOPBACK | IFF_POINTOPOINT)))
                    continue;
                now[n].index = if_nametoindex(i->ifa_name);
                now[n].addr = ((struct sockaddr_in *)i->ifa_addr)->sin_addr;
                if (now[n].index) n++;
            }
            freeifaddrs(list);
        }
    }
    int same = n == nifs;
    for (int i = 0; same && i < n; i++) same = now[i].index == ifs[i].index && now[i].addr.s_addr == ifs[i].addr.s_addr;
    if (same) return 0;
    unregister_name();
    for (int i = 0; i < n; i++) now[i].rec = NULL;
    memcpy(ifs, now, sizeof now);
    nifs = n;
    return 1;
}

static int port_open(const Service *s) {
    if (s->txt_only) return 1;
    if (!s->port) return 0;
    int udp = label_is(&s->type, 1, "_udp");
    for (int i = 0; i < nfwd; i++)
        if (fwd[i].port == s->port && fwd[i].udp == udp) return fwd[i].err == 0;
    return 0;
}

/* The Mac's live services: their ports go to net_forward.c, whether or not the helper is linked. */
static void advertise(void) {
    HvFwdPort p[MAX_SERVICES];
    int n = 0;
    for (int i = 0; i < MAX_SERVICES; i++) {
        Service *s = &svc[i];
        if (s->used && !s->txt_only && s->ptr.has && s->srv.has && s->port && mac_a.has && hv_dns_name_eq(&s->target, &mac_name))
            p[n++] = (HvFwdPort){s->port, (uint8_t)label_is(&s->type, 1, "_udp")};
    }
    if (n == nadv_sent && !memcmp(p, adv_sent, (size_t)n * sizeof *p)) return;
    memcpy(adv_sent, p, (size_t)n * sizeof *p);
    nadv_sent = n;
    hv_net_ports_advertised(p, n);
}

static void reconcile(void) {
    if (!conn && now_cached >= conn_retry) {
        if (DNSServiceCreateConnection(&conn) != kDNSServiceErr_NoError) {
            conn = NULL;
            conn_retry = now_cached + 5000;
            static int said;
            if (!said++) fprintf(stderr, "hengevm: mdns: no Bonjour on this device now: the Mac's name isn't announced\n");
        }
    }
    if (now_cached - ifs_checked >= 2000 || !ifs_checked) {
        ifs_checked = now_cached;
        if (read_ifs()) dirty = 1;
    }
    if (taken && mac_a.has && !hv_dns_name_eq(&mac_name, &our_name)) taken = 0; /* the Mac took another name */
    int want_name = conn && !held && mac_a.has && nifs > 0 && !taken;
    HvDnsName want = mac_name;
    if (a_registered && (!want_name || !hv_dns_name_eq(&want, &our_name))) {
        for (int i = 0; i < MAX_SERVICES; i++) unregister_service(&svc[i]);
        unregister_name();
    }
    if (want_name && !a_registered && may_change(0)) {
        if (!hv_dns_name_eq(&want, &our_name)) name_gen++; /* the services go on the new name */
        our_name = want;
        register_name();
    }
    int pending = want_name && !a_registered;
    for (int i = 0; i < MAX_SERVICES; i++) {
        Service *s = &svc[i];
        if (s->failed && s->clashed_at && now_cached - s->clashed_at >= CLASH_WAIT_MS) {
            /* The Mac didn't take another name: the other device's service couldn't be told to it (a kind Henge
               doesn't look for, or a service on this device itself, whose addresses aren't told). On the network
               still under the name Bonjour would give it ("<name> (2)"), though the Mac doesn't know that name. */
            fprintf(stderr, "hengevm: mdns: the Mac didn't rename its service in %d s: it is announced as \"<name> (2)\", as Bonjour renames\n", CLASH_WAIT_MS / 1000);
            s->failed = 0, s->clashed_at = 0, s->alt = 1;
        }
        int live = s->used && a_registered && !taken && !s->denied && !s->failed &&
                   (s->txt_only ? s->txt.has : s->ptr.has && s->srv.has && hv_dns_name_eq(&s->target, &mac_name)) && port_open(s);
        if (s->reg && (!live || s->reg_port != s->port || s->reg_gen != name_gen || s->reg_alt != s->alt)) {
            if (!may_change(!live && held)) {
                pending = 1;
                continue;
            }
            unregister_service(s);
        }
        if (live && s->reg && s->reg_txt != fnv(s->txt_b, s->txt_len)) {
            uint8_t empty = 0;
            DNSServiceUpdateRecord(s->txt_only ? conn : s->ref, s->txt_only ? s->rec : NULL, 0, s->txt_len ? s->txt_len : 1,
                                   s->txt_len ? s->txt_b : &empty, s->txt_only ? 4500 : 0);
            s->reg_txt = fnv(s->txt_b, s->txt_len);
        }
        if (live && !s->reg) {
            if (!may_change(0)) {
                pending = 1;
                continue;
            }
            register_service(s);
        }
        if (!s->reg && !s->ptr.has && !s->srv.has && !s->txt.has) s->used = 0;
    }
    advertise();
    set_status();
    if (pending) next_scan = window + 1000;
}

int hv_mdns_fd(void) { return conn ? DNSServiceRefSockFD(conn) : -1; }

void hv_mdns_ready(void) {
    if (!conn) return;
    DNSServiceErrorType e = DNSServiceProcessResult(conn);
    if (e != kDNSServiceErr_NoError) {
        fprintf(stderr, "hengevm: mdns: the device's Bonjour went away (dns_sd %d): starting again\n", (int)e);
        drop_conn();
        conn_retry = 0;
    }
    dirty = 1;
}

static int64_t lan_tick(int run);

/* Refresh what is due, drop what expired. While held the Mac doesn't run: its records don't age. */
static void age(Rec *r, const HvDnsName *name, uint16_t type, int *gone) {
    if (!r->has) return;
    int64_t life = (int64_t)r->ttl * 1000, at = now_cached - r->got;
    if (at >= life) {
        r->has = 0;
        *gone = 1;
        return;
    }
    if (r->asked < 4 && at >= life * (80 + 5 * r->asked) / 100) {
        r->asked++;
        ask(name, type);
    }
}

static void age_all(void) {
    int gone = 0;
    age(&mac_a, &mac_name, T_A, &gone);
    for (int i = 0; i < ntypes; i++) {
        HvDnsName n;
        hv_dns_name_from(&n, "_services._dns-sd._udp.local");
        age(&type_rec[i], &n, T_PTR, &gone);
    }
    for (int i = 0; i < MAX_SERVICES; i++) {
        Service *s = &svc[i];
        if (!s->used) continue;
        int was = s->ptr.has || s->txt_only;
        age(&s->ptr, &s->type, T_PTR, &gone);
        age(&s->srv, &s->inst, T_SRV, &gone);
        age(&s->txt, &s->inst, T_TXT, &gone);
        if (was && !s->ptr.has && !s->txt_only) {
            char n[300];
            text_of(&s->inst, n, sizeof n);
            fprintf(stderr, "hengevm: mdns: the Mac no longer says it offers %s\n", n);
        }
    }
    flush_asks();
    if (gone) dirty = 1;
}

static void shift(Rec *r, int64_t by) { r->got += by; }

/* hv_mdns_test_away / _back / _ticks. A request's parts are taken together, under test_mu. */
static pthread_mutex_t test_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int test_wanted; /* 1: away, 2: back */
static int64_t test_pass_ms;
static _Atomic uint64_t ticks;

static void test_take(int64_t now) {
    pthread_mutex_lock(&test_mu);
    int w = atomic_exchange(&test_wanted, 0);
    int64_t pass = test_pass_ms;
    test_pass_ms = 0;
    pthread_mutex_unlock(&test_mu);
    if (w & 1) {
        ours_pass(pass);
        fprintf(stderr, "hengevm: mdns: (a test) the device's Bonjour went away, %lld s on\n", (long long)(pass / 1000));
        drop_conn();
        conn_retry = INT64_MAX; /* until hv_mdns_test_back: nothing else sets it while there's no connection */
        dirty = 1;
    }
    if (w & 2) conn_retry = now, dirty = 1;
}

int64_t hv_mdns_tick(int64_t now) {
    now_cached = now;
    atomic_fetch_add_explicit(&ticks, 1, memory_order_relaxed);
    if (atomic_load_explicit(&test_wanted, memory_order_relaxed)) test_take(now);
    unsigned g = hv_net_forward_generation();
    if (g != fwd_gen) {
        fwd_gen = g;
        int h = 0;
        nfwd = hv_net_forward_status(fwd, (int)(sizeof fwd / sizeof fwd[0]), NULL, &h);
        if (nfwd > (int)(sizeof fwd / sizeof fwd[0])) nfwd = (int)(sizeof fwd / sizeof fwd[0]);
        if (h != held) {
            if (h) {
                held_since = now;
                memset(ask_at, 0, sizeof ask_at); /* asked afresh on return */
            } else {
                if (held == 1) { /* the Mac didn't run meanwhile: nothing it said aged */
                    int64_t by = now - held_since;
                    shift(&mac_a, by);
                    for (int i = 0; i < ntypes; i++) shift(&type_rec[i], by);
                    for (int i = 0; i < MAX_SERVICES; i++) shift(&svc[i].ptr, by), shift(&svc[i].srv, by), shift(&svc[i].txt, by);
                }
                for (int i = 0; i < MAX_SERVICES; i++) svc[i].denied = svc[i].failed = svc[i].alt = 0, svc[i].clashed_at = 0; /* tried again on return, as the Mac names them */
                taken = 0;
                pthread_mutex_lock(&status_mu);
                status_denied = 0;
                pthread_mutex_unlock(&status_mu);
                /* The Mac is asked what it is and offers at once, and again 3 s and 15 s later; until then, and
                   after, its own announcements say. */
                ask_at[0] = now, ask_at[1] = now + 3000, ask_at[2] = now + 15000;
            }
            held = h;
        }
        dirty = 1;
    }
    if (!held) {
        for (int i = 0; i < 4; i++)
            if (ask_at[i] && now >= ask_at[i]) {
                ask_at[i] = 0;
                ask_everything();
            }
        age_all();
    }
    if (dirty || now >= next_scan) {
        dirty = 0;
        next_scan = now + 1000;
        reconcile();
    }
    int64_t next = next_scan;
    for (int i = 0; i < 4 && !held; i++)
        if (ask_at[i] && ask_at[i] < next) next = ask_at[i];
    int64_t lan_in = lan_tick(conn && held == 0 && nifs > 0);
    if (lan_in < next - now) next = now + lan_in;
    return next > now ? next - now : 0;
}

void hv_mdns_start(HvMdnsDeliver d, int net_mtu) {
    deliver = d;
    if (net_mtu >= 576) mtu = net_mtu;
}

/* ================================================================ the network's devices, in the Mac (5b) */

/* What the device's Bonjour finds on its network, of the kinds Henge declares (Info.plist's NSBonjourServices: iOS
   lets an app look for those only), told to the Mac as its own network's devices: answered when the Mac asks (its
   Finder, its printing, `ping othermac.local`), announced as they come and said goodbye to as they go. A `.local`
   name the Mac asks for that nothing offered is looked up once (at most 16 at a time; kept 120 s, or 10 s when not
   found). While held nothing is looked for; what the Mac had ages on its own TTLs.

   Never the Mac's own (amendment 4, by provenance, not by its current name): the device's Bonjour finds what Henge
   registered for the Mac, and other devices' caches keep it a while, so the Mac would see "another device" with its
   name and rename itself. Never told to the Mac: a name Henge registered for it, or withdrew within 2 x 4500 s; an
   address that is the device's own; a service on such a name. The one exception is a clash: when another device
   has the Mac's name, that device's address for it is told (it isn't the device's own), so the Mac renames itself
   as a MacBook would (Henge picks no name). */

#define LAN_INSTANCES 128
#define LAN_HOSTS 64
#define LAN_LOOKUPS 16
#define LAN_ADDRS 4
#define LAN_ANSWERS 512
#define OURS 256 /* names Henge put on the network for the Mac within OURS_KEPT_MS: its names and its services' */
#define OURS_KEPT_MS (2 * 4500 * 1000LL)

static const char *const lan_type_text[] = {
    "_smb._tcp",  "_afpovertcp._tcp", "_device-info._tcp", "_adisk._tcp",          "_rfb._tcp",   "_ssh._tcp",
    "_sftp-ssh._tcp", "_ipp._tcp",    "_ipps._tcp",        "_printer._tcp",        "_pdl-datastream._tcp",
    "_uscan._tcp", "_uscans._tcp",    "_scanner._tcp",     "_http._tcp",           "_https._tcp"};
#define LAN_TYPES (int)(sizeof lan_type_text / sizeof *lan_type_text)

typedef struct LanHost {
    uint8_t used, lookup, missed; /* lookup: started by the Mac's question, not by a service found */
    HvDnsName name;
    DNSServiceRef ref;
    uint32_t addr[LAN_ADDRS]; /* network order; never the device's own */
    int naddr, users;
    int64_t started, until; /* a lookup: when it started; until when it is kept (0: while it runs) */
} LanHost;

typedef struct LanInst {
    uint8_t used, resolved, told; /* told: announced to the Mac */
    int type, host, seen;          /* host: LanHost, -1; seen: the browse's adds less its removes (per interface) */
    HvDnsName name;                /* "<instance>._x._tcp.local" */
    DNSServiceRef ref;             /* its resolve, while it lasts */
    uint16_t port, txt_len;
    uint8_t txt[MAX_TXT];
} LanInst;

typedef struct Ours {
    HvDnsName n;
    int64_t until;
    uint8_t clashed;
} Ours;

static HvDnsName lan_type[LAN_TYPES], services_ptr_name;
static DNSServiceRef lan_browse[LAN_TYPES];
static int lan_on, lan_test;
static LanInst lan_inst[LAN_INSTANCES];
static LanHost lan_host[LAN_HOSTS];
static int lan_clash = -1; /* the host entry looking up the Mac's name while another device has it */
static int lan_inst_said, lan_host_said, lan_big_said;
static _Atomic uint64_t lan_lookups_started;
static Ours ours[OURS];

/* ---- the guard */

/* A name Henge put on the network for the Mac: kept 2 x 4500 s (the longest TTL, twice) after it was last there.
   clashed: 1 another device was found to have it; 0 as it was; -1 Henge has it again (registered: not clashed). A
   name is forgotten only once its time is up; with every slot in time, the one nearest its end goes (said once). */
static void mark_ours(const HvDnsName *n, int clashed) {
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < OURS; i++) {
        if (ours[i].n.len && hv_dns_name_eq(&ours[i].n, n)) {
            ours[i].until = now_cached + OURS_KEPT_MS;
            if (clashed) ours[i].clashed = clashed > 0;
            return;
        }
        if (free_slot < 0 && (!ours[i].n.len || ours[i].until <= now_cached)) free_slot = i;
        if (ours[i].until < ours[oldest].until) oldest = i;
    }
    if (free_slot < 0) {
        static int said;
        if (!said++) fprintf(stderr, "hengevm: mdns: the Mac put more than %d names on the network within %lld s: the oldest is forgotten early\n", OURS, OURS_KEPT_MS / 1000);
        free_slot = oldest;
    }
    Ours *o = &ours[free_slot];
    o->n = *n, o->until = now_cached + OURS_KEPT_MS, o->clashed = clashed > 0;
}

/* For a test (hv_mdns_test_away): every name's time moved ms nearer its end. */
static void ours_pass(int64_t ms) {
    for (int i = 0; i < OURS; i++) ours[i].until -= ms;
}

/* 1 if the name is (or was lately) one Henge put on the network for the Mac; *clashed: another device has it. */
static int ours_name(const HvDnsName *n, int *clashed) {
    int is = 0, c = 0;
    if ((a_registered || taken) && hv_dns_name_eq(n, &our_name)) is = 1, c |= taken;
    for (int i = 0; i < MAX_SERVICES && !is; i++)
        if (svc[i].reg && hv_dns_name_eq(&svc[i].inst, n)) is = 1;
    for (int i = 0; i < OURS; i++)
        if (ours[i].n.len && ours[i].until > now_cached && hv_dns_name_eq(&ours[i].n, n)) is = 1, c |= ours[i].clashed;
    if (clashed) *clashed = c;
    return is;
}

static int device_addr(uint32_t a) {
    for (int i = 0; i < ndev_addr; i++)
        if (dev_addr[i] == a) return 1;
    return 0;
}

/* A host's records may go to the Mac: not a name Henge registered for it, unless another device was found to have
   it (then only that device's addresses: the device's own never are, see lan_addr). */
static int host_ok(const HvDnsName *n) {
    int c = 0;
    return !ours_name(n, &c) || c;
}

/* ---- what is known */

static int host_told(const LanHost *h) { return h->used && h->naddr > 0 && host_ok(&h->name); }

static int inst_told(const LanInst *i) {
    int c = 0;
    if (!i->used || !i->resolved || i->seen <= 0 || (ours_name(&i->name, &c) && !c)) return 0; /* c: another device's (reg_done) */
    if (i->host < 0) return i->port == 0; /* _device-info: a TXT, on no host */
    return host_ok(&lan_host[i->host].name) && (i->port == 0 || lan_host[i->host].naddr > 0);
}

static int find_inst(const HvDnsName *n) {
    for (int i = 0; i < LAN_INSTANCES; i++)
        if (lan_inst[i].used && hv_dns_name_eq(&lan_inst[i].name, n)) return i;
    return -1;
}

static int find_host(const HvDnsName *n) {
    for (int i = 0; i < LAN_HOSTS; i++)
        if (lan_host[i].used && hv_dns_name_eq(&lan_host[i].name, n)) return i;
    return -1;
}

static int find_type(const HvDnsName *n) {
    for (int i = 0; i < LAN_TYPES; i++)
        if (hv_dns_name_eq(&lan_type[i], n)) return i;
    return -1;
}

/* ---- answers to the Mac */

enum { K_TYPES, K_PTR, K_SRV, K_TXT, K_A };
typedef struct LanRef {
    uint8_t kind, sub;
    uint16_t i;
} LanRef;

typedef struct LanTo {
    const uint8_t *mac; /* NULL: multicast */
    uint16_t port, id;
    int legacy; /* a plain DNS client's question (not from port 5353): its ID and questions back, TTLs at most 10 s */
} LanTo;

static uint8_t *put_name(uint8_t *p, const HvDnsName *n) {
    memcpy(p, n->b, n->len);
    return p + n->len;
}

/* The record r at b + at, within cap: the new end, or 0 if it doesn't fit. */
static size_t rr_put(uint8_t *b, size_t cap, size_t at, const LanRef *r, int legacy, int bye) {
    const HvDnsName *name, *target = NULL;
    uint16_t type, port = 0, rdlen;
    uint32_t ttl;
    int flush = 1;
    const uint8_t *raw = NULL;
    uint8_t one = 0;
    switch (r->kind) {
    case K_TYPES:
        name = &services_ptr_name, type = T_PTR, ttl = 4500, flush = 0, target = &lan_type[r->i];
        break;
    case K_PTR:
        name = &lan_type[lan_inst[r->i].type], type = T_PTR, ttl = 4500, flush = 0, target = &lan_inst[r->i].name;
        break;
    case K_SRV:
        if (lan_inst[r->i].host < 0) return at; /* nothing to say */
        name = &lan_inst[r->i].name, type = T_SRV, ttl = 120, port = lan_inst[r->i].port;
        target = &lan_host[lan_inst[r->i].host].name;
        break;
    case K_TXT:
        name = &lan_inst[r->i].name, type = T_TXT, ttl = 4500;
        raw = lan_inst[r->i].txt_len ? lan_inst[r->i].txt : &one;
        break;
    default:
        name = &lan_host[r->i].name, type = T_A, ttl = 120, raw = (const uint8_t *)&lan_host[r->i].addr[r->sub];
        break;
    }
    rdlen = type == T_PTR ? target->len : type == T_SRV ? (uint16_t)(6 + target->len) : type == T_A ? 4
            : (uint16_t)(lan_inst[r->i].txt_len ? lan_inst[r->i].txt_len : 1);
    if (at + name->len + 10 + rdlen > cap) return 0;
    uint8_t *p = put_name(b + at, name);
    if (legacy) flush = 0, ttl = ttl < 10 ? ttl : 10;
    if (bye) ttl = 0;
    wr16(p, type), wr16(p + 2, (uint16_t)(flush ? 0x8001 : 1));
    wr16(p + 4, (uint16_t)(ttl >> 16)), wr16(p + 6, (uint16_t)ttl), wr16(p + 8, rdlen);
    p += 10;
    if (type == T_SRV) wr16(p, 0), wr16(p + 2, 0), wr16(p + 4, port), p += 6;
    if (target) p = put_name(p, target);
    else memcpy(p, raw, rdlen), p += rdlen;
    return (size_t)(p - b);
}

/* What goes with an answer, as additional records (RFC 6763 12). */
static int extras(const LanRef *r, LanRef *out) {
    int n = 0;
    const LanInst *i = &lan_inst[r->i];
    if (r->kind == K_PTR) out[n++] = (LanRef){K_SRV, 0, r->i}, out[n++] = (LanRef){K_TXT, 0, r->i};
    if ((r->kind == K_PTR || r->kind == K_SRV) && i->host >= 0 && host_told(&lan_host[i->host]))
        for (int k = 0; k < lan_host[i->host].naddr && n < 2 + LAN_ADDRS; k++) out[n++] = (LanRef){K_A, (uint8_t)k, (uint16_t)i->host};
    return n;
}

static int same_ref(const LanRef *a, const LanRef *b) { return a->kind == b->kind && a->sub == b->sub && a->i == b->i; }

/* Answers (and what goes with them) to the Mac, in as many messages as they take, each within the network's MTU. */
static void lan_send(const LanRef *ans, int n, const LanTo *to, int bye) {
    static uint8_t b[HV_DNS_MAX_MSG];
    size_t cap = max_msg();
    int a = 0;
    while (a < n) {
        memset(b, 0, 12);
        wr16(b + 2, 0x8400);
        size_t at = 12;
        if (to->legacy) { /* the questions back, as a unicast DNS answer has them */
            wr16(b, to->id);
            int nq = 0;
            for (int q = 0; q < msg.nq && at + msg.q[q].name.len + 4 <= cap / 2; q++, nq++) {
                put_name(b + at, &msg.q[q].name);
                at += msg.q[q].name.len;
                wr16(b + at, msg.q[q].type), wr16(b + at + 2, msg.q[q].cls), at += 4;
            }
            wr16(b + 4, (uint16_t)nq);
        }
        int first = a, nan = 0, nar = 0;
        LanRef in[64];
        int nin = 0;
        for (; a < n && nin < 64; a++) {
            size_t e = rr_put(b, cap, at, &ans[a], to->legacy, bye);
            if (!e) break;
            if (e != at) nan++, in[nin++] = ans[a];
            at = e;
        }
        if (a == first) { /* one record bigger than a message: left out */
            if (!lan_big_said++) fprintf(stderr, "hengevm: mdns: a record too big for one packet isn't told to the Mac\n");
            a++;
            continue;
        }
        for (int k = 0; k < nin && !bye; k++) {
            LanRef x[2 + LAN_ADDRS];
            int nx = extras(&in[k], x);
            for (int j = 0; j < nx && nin < 64; j++) {
                int dup = 0;
                for (int m = 0; m < nin && !dup; m++) dup = same_ref(&in[m], &x[j]);
                if (dup) continue;
                size_t e = rr_put(b, cap, at, &x[j], to->legacy, 0);
                if (!e) break;
                if (e != at) nar++, in[nin++] = x[j];
                at = e;
            }
        }
        if (!nan) continue;
        wr16(b + 6, (uint16_t)nan), wr16(b + 10, (uint16_t)nar);
        send_dns(b, at, to->mac, to->port);
    }
}

static void lan_announce(const LanRef *r, int n, int bye) {
    static const LanTo all = {NULL, MDNS_PORT, 0, 0};
    lan_send(r, n, &all, bye);
}

/* An instance, now complete, to the Mac: its PTR, SRV, TXT and its host's addresses, unasked. */
static void tell(int k) {
    LanInst *i = &lan_inst[k];
    if (i->told || !inst_told(i)) return;
    LanRef r[3 + LAN_ADDRS];
    int n = 0;
    r[n++] = (LanRef){K_PTR, 0, (uint16_t)k}, r[n++] = (LanRef){K_SRV, 0, (uint16_t)k}, r[n++] = (LanRef){K_TXT, 0, (uint16_t)k};
    if (i->host >= 0 && host_told(&lan_host[i->host]))
        for (int a = 0; a < lan_host[i->host].naddr; a++) r[n++] = (LanRef){K_A, (uint8_t)a, (uint16_t)i->host};
    lan_announce(r, n, 0);
    i->told = 1;
}

/* The Mac's service has the name of this device's: its records said to the Mac again (told before or not), as that
   device defends its name on a network when a MacBook announces the same (RFC 6762 9), so the Mac renames its own.
   Not yet resolved: told when it is (tell). */
static int lan_defend(const HvDnsName *n) {
    int k = find_inst(n);
    if (k >= 0) lan_inst[k].told = 0, tell(k);
    return k >= 0; /* 0: a service Henge doesn't know (of a kind not looked for), so it can't be told to the Mac */
}

static void untell(int k) {
    LanInst *i = &lan_inst[k];
    if (!i->told) return;
    LanRef r[3] = {{K_PTR, 0, (uint16_t)k}, {K_SRV, 0, (uint16_t)k}, {K_TXT, 0, (uint16_t)k}};
    lan_announce(r, 3, 1);
    i->told = 0;
}

/* Known answers: the Mac's question lists what it has (with at least half its TTL left); those aren't sent. */
static int known(const LanRef *r) {
    for (int k = 0; k < msg.nan; k++) {
        const HvDnsRR *a = &msg.rr[k];
        const LanInst *i = &lan_inst[r->i];
        switch (r->kind) {
        case K_TYPES:
            if (a->type == T_PTR && a->ttl >= 2250 && hv_dns_name_eq(&a->name, &services_ptr_name) && hv_dns_name_eq(&a->target, &lan_type[r->i])) return 1;
            break;
        case K_PTR:
            if (a->type == T_PTR && a->ttl >= 2250 && hv_dns_name_eq(&a->name, &lan_type[i->type]) && hv_dns_name_eq(&a->target, &i->name)) return 1;
            break;
        case K_SRV:
            if (a->type == T_SRV && a->ttl >= 60 && i->host >= 0 && hv_dns_name_eq(&a->name, &i->name) && a->port == i->port &&
                hv_dns_name_eq(&a->target, &lan_host[i->host].name))
                return 1;
            break;
        case K_TXT:
            if (a->type == T_TXT && a->ttl >= 2250 && hv_dns_name_eq(&a->name, &i->name) &&
                a->rdlen == (i->txt_len ? i->txt_len : 1) && !memcmp(a->rdata, i->txt_len ? i->txt : (const uint8_t *)"", a->rdlen))
                return 1;
            break;
        default:
            if (a->type == T_A && a->ttl >= 60 && hv_dns_name_eq(&a->name, &lan_host[r->i].name) && a->a == lan_host[r->i].addr[r->sub]) return 1;
        }
    }
    return 0;
}

static void lookup(const HvDnsName *n);

/* The Mac's question (msg): answered from what is known, multicast, or unicast to the asker when it asks for that
   (the QU bit, or a plain DNS client's port). */
static void lan_query(const uint8_t *mac, uint16_t port) {
    if (!lan_on) return;
    LanTo to = {NULL, MDNS_PORT, msg.id, port != MDNS_PORT};
    static LanRef ans[LAN_ANSWERS];
    int n = 0;
#define ADD(...)                                              \
    do {                                                      \
        LanRef r_ = {__VA_ARGS__};                            \
        if (n < LAN_ANSWERS && !known(&r_)) ans[n++] = r_;    \
    } while (0)
    for (int q = 0; q < msg.nq; q++) {
        const HvDnsRR *Q = &msg.q[q];
        if (Q->cls != 1 && Q->cls != 255) continue;
        int any = Q->type == 255;
        if (Q->top) to.mac = mac, to.port = port;
        if ((Q->type == T_PTR || any) && hv_dns_name_eq(&Q->name, &services_ptr_name)) {
            for (int t = 0; t < LAN_TYPES; t++) {
                int has = 0;
                for (int i = 0; i < LAN_INSTANCES && !has; i++) has = lan_inst[i].type == t && inst_told(&lan_inst[i]);
                if (has) ADD(K_TYPES, 0, (uint16_t)t);
            }
            continue;
        }
        int t = find_type(&Q->name);
        if (t >= 0 && (Q->type == T_PTR || any)) {
            for (int i = 0; i < LAN_INSTANCES; i++)
                if (lan_inst[i].type == t && inst_told(&lan_inst[i])) ADD(K_PTR, 0, (uint16_t)i);
            continue;
        }
        int i = find_inst(&Q->name);
        if (i >= 0 && inst_told(&lan_inst[i])) {
            if (Q->type == T_SRV || any) ADD(K_SRV, 0, (uint16_t)i);
            if (Q->type == T_TXT || any) ADD(K_TXT, 0, (uint16_t)i);
            continue;
        }
        if ((Q->type == T_A || any) && is_host(&Q->name)) {
            int h = find_host(&Q->name);
            if (h >= 0 && host_told(&lan_host[h])) {
                for (int a = 0; a < lan_host[h].naddr; a++) ADD(K_A, (uint8_t)a, (uint16_t)h);
            } else if (h < 0 && host_ok(&Q->name)) {
                lookup(&Q->name); /* the Mac asks again in a second or so */
            }
        }
    }
#undef ADD
    if (to.legacy) to.mac = mac, to.port = port;
    if (n) lan_send(ans, n, &to, 0);
}

/* ---- the device's Bonjour */

static uint32_t lan_ifi(void) { return local_only ? kDNSServiceInterfaceIndexLocalOnly : kDNSServiceInterfaceIndexAny; }

static void DNSSD_API lan_addr(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifi, DNSServiceErrorType err,
                               const char *hostname, const struct sockaddr *sa, uint32_t ttl, void *ctx);

static void host_free(int h) {
    LanHost *x = &lan_host[h];
    if (x->ref) DNSServiceRefDeallocate(x->ref);
    memset(x, 0, sizeof *x);
}

/* A host entry's addresses looked up (IPv4), from now on, as long as it lasts. -1 if dns_sd refused. */
static int host_ask(int h) {
    LanHost *x = &lan_host[h];
    char text[300];
    if (lan_test) return 0;
    if (hv_dns_name_text(&x->name, text, sizeof text)) return -1;
    x->ref = conn;
    if (DNSServiceGetAddrInfo(&x->ref, kDNSServiceFlagsShareConnection, lan_ifi(), kDNSServiceProtocol_IPv4, text, lan_addr,
                              (void *)(intptr_t)h)) {
        x->ref = NULL;
        return -1;
    }
    return 0;
}

/* The host entry for a name, its addresses looked up. -1 when the table is full. For a service (as_lookup 0), an
   entry left by the Mac's lookup becomes the service's: looked up again if that lookup ended unanswered (the device
   may have come since), and no longer ended after 3 s or let go after 10 s or 120 s (host_release lets it go). */
static int host_get(const HvDnsName *n, int as_lookup) {
    int h = find_host(n);
    if (h >= 0) {
        LanHost *x = &lan_host[h];
        if (!as_lookup) {
            x->lookup = 0, x->missed = 0, x->until = 0;
            if (!x->ref) host_ask(h); /* refused: tried again when the next service on it comes */
        }
        return h;
    }
    for (h = 0; h < LAN_HOSTS && lan_host[h].used; h++) {}
    if (h == LAN_HOSTS) {
        if (!lan_host_said++) fprintf(stderr, "hengevm: mdns: more than %d of the network's devices: the newest aren't told to the Mac\n", LAN_HOSTS);
        return -1;
    }
    LanHost *x = &lan_host[h];
    memset(x, 0, sizeof *x);
    x->used = 1, x->lookup = (uint8_t)as_lookup, x->name = *n, x->started = now_cached;
    if (host_ask(h)) {
        x->used = 0;
        return -1;
    }
    return h;
}

static void host_release(int h) {
    if (h < 0 || !lan_host[h].used) return;
    if (--lan_host[h].users <= 0 && !lan_host[h].lookup && h != lan_clash) host_free(h);
}

/* A .local name the Mac asked for that nothing offered: looked up once (16 at a time). */
static void lookup(const HvDnsName *n) {
    int running = 0;
    for (int h = 0; h < LAN_HOSTS; h++) running += lan_host[h].used && lan_host[h].lookup && lan_host[h].ref && !lan_host[h].naddr;
    if (running >= LAN_LOOKUPS || lan_test) return;
    if (host_get(n, 1) >= 0) atomic_fetch_add(&lan_lookups_started, 1);
}

static void DNSSD_API lan_addr(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifi, DNSServiceErrorType err,
                               const char *hostname, const struct sockaddr *sa, uint32_t ttl, void *ctx) {
    (void)ifi, (void)hostname, (void)ttl;
    int h = (int)(intptr_t)ctx;
    LanHost *x = &lan_host[h];
    if (x->ref != ref || err || !sa || sa->sa_family != AF_INET) return;
    uint32_t a = ((const struct sockaddr_in *)sa)->sin_addr.s_addr;
    if (device_addr(a)) return; /* the device's own: the Mac's name, never "another device" */
    int k = 0;
    while (k < x->naddr && x->addr[k] != a) k++;
    if (flags & kDNSServiceFlagsAdd) {
        if (k < x->naddr || x->naddr == LAN_ADDRS) return;
        x->addr[x->naddr++] = a;
        if (x->lookup) x->until = now_cached + 120000;
        if (!host_told(x)) return;
        LanRef r = {K_A, (uint8_t)k, (uint16_t)h};
        lan_announce(&r, 1, 0);
        for (int i = 0; i < LAN_INSTANCES; i++)
            if (lan_inst[i].used && lan_inst[i].host == h) tell(i);
        if (h == lan_clash) {
            char t[300], ip[INET_ADDRSTRLEN];
            text_of(&x->name, t, sizeof t);
            inet_ntop(AF_INET, &a, ip, sizeof ip);
            fprintf(stderr, "hengevm: mdns: told the Mac that %s is %s's name, so it takes another\n", ip, t);
        }
    } else if (k < x->naddr) {
        if (host_told(x)) {
            LanRef r = {K_A, (uint8_t)k, (uint16_t)h};
            lan_announce(&r, 1, 1);
        }
        x->addr[k] = x->addr[--x->naddr];
    }
}

static void DNSSD_API lan_resolved(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifi, DNSServiceErrorType err,
                                   const char *full, const char *target, uint16_t port_be, uint16_t txt_len,
                                   const unsigned char *txt, void *ctx) {
    (void)flags, (void)ifi, (void)full;
    int k = (int)(intptr_t)ctx;
    LanInst *i = &lan_inst[k];
    if (i->ref != ref || err) return;
    HvDnsName h;
    if (hv_dns_name_parse(&h, target) || !is_host(&h)) return;
    if (txt_len > MAX_TXT || !txt_ok(txt, txt_len)) {
        char n[300];
        text_of(&i->name, n, sizeof n);
        fprintf(stderr, "hengevm: mdns: %s's TXT (%u bytes) is over %d or malformed: told to the Mac without it\n", n, txt_len, MAX_TXT);
        txt_len = 0;
    }
    uint16_t port = ntohs(port_be);
    int moved = i->host < 0 || !hv_dns_name_eq(&lan_host[i->host].name, &h);
    int srv_changed = i->resolved && (moved || port != i->port);
    int txt_changed = i->resolved && (txt_len != i->txt_len || memcmp(txt, i->txt, txt_len));
    if (moved) {
        host_release(i->host);
        i->host = host_get(&h, 0);
        if (i->host >= 0) lan_host[i->host].users++;
    }
    i->port = port, i->txt_len = txt_len;
    memcpy(i->txt, txt, txt_len);
    i->resolved = 1;
    if (!i->told) {
        tell(k);
    } else if (srv_changed || txt_changed) {
        LanRef r[2];
        int n = 0;
        if (srv_changed) r[n++] = (LanRef){K_SRV, 0, (uint16_t)k};
        if (txt_changed) r[n++] = (LanRef){K_TXT, 0, (uint16_t)k};
        lan_announce(r, n, 0);
    }
}

static void inst_free(int k, int goodbye) {
    LanInst *i = &lan_inst[k];
    if (goodbye) untell(k);
    if (i->ref) DNSServiceRefDeallocate(i->ref);
    host_release(i->host);
    memset(i, 0, sizeof *i);
    i->host = -1;
}

static void DNSSD_API lan_found(DNSServiceRef ref, DNSServiceFlags flags, uint32_t ifi, DNSServiceErrorType err,
                                const char *name, const char *type, const char *domain, void *ctx) {
    (void)ifi;
    int t = (int)(intptr_t)ctx;
    if (t < 0 || t >= LAN_TYPES || lan_browse[t] != ref) return;
    if (err) {
        if (err == kDNSServiceErr_PolicyDenied) note_denied();
        return;
    }
    size_t l = strlen(name);
    if (strcasecmp(domain, "local.") || !l || l > 63) return;
    HvDnsName n;
    n.b[0] = (uint8_t)l;
    memcpy(n.b + 1, name, l);
    if (1 + l + lan_type[t].len > 255) return;
    memcpy(n.b + 1 + l, lan_type[t].b, lan_type[t].len);
    n.len = (uint16_t)(1 + l + lan_type[t].len);
    int k = find_inst(&n);
    if (!(flags & kDNSServiceFlagsAdd)) {
        if (k >= 0 && --lan_inst[k].seen <= 0) inst_free(k, 1);
        return;
    }
    if (k < 0) {
        for (k = 0; k < LAN_INSTANCES && lan_inst[k].used; k++) {}
        if (k == LAN_INSTANCES) {
            if (!lan_inst_said++) fprintf(stderr, "hengevm: mdns: more than %d services on the network: the newest aren't told to the Mac\n", LAN_INSTANCES);
            return;
        }
        LanInst *i = &lan_inst[k];
        memset(i, 0, sizeof *i);
        i->used = 1, i->type = t, i->host = -1, i->name = n;
        i->ref = conn;
        if (DNSServiceResolve(&i->ref, kDNSServiceFlagsShareConnection, lan_ifi(), name, type, domain, lan_resolved, (void *)(intptr_t)k)) {
            i->ref = NULL;
            i->used = 0;
            return;
        }
    }
    lan_inst[k].seen++;
}

static void lan_stop(void) {
    for (int t = 0; t < LAN_TYPES; t++) {
        if (lan_browse[t]) DNSServiceRefDeallocate(lan_browse[t]);
        lan_browse[t] = NULL;
    }
    for (int k = 0; k < LAN_INSTANCES; k++)
        if (lan_inst[k].used) inst_free(k, 0); /* no goodbyes: the Mac's cache ages them on their own TTLs */
    for (int h = 0; h < LAN_HOSTS; h++)
        if (lan_host[h].used) host_free(h);
    lan_clash = -1;
    if (lan_on) fprintf(stderr, "hengevm: mdns: no longer looking at the network for the Mac\n");
    lan_on = 0;
}

static void lan_start(void) {
    for (int t = 0; t < LAN_TYPES; t++) {
        char type[64];
        snprintf(type, sizeof type, "%s.", lan_type_text[t]);
        lan_browse[t] = conn;
        if (DNSServiceBrowse(&lan_browse[t], kDNSServiceFlagsShareConnection, lan_ifi(), type, "local.", lan_found, (void *)(intptr_t)t))
            lan_browse[t] = NULL;
    }
    lan_on = 1;
    fprintf(stderr, "hengevm: mdns: looking at the network for the Mac (%d kinds of service)\n", LAN_TYPES);
}

static void lan_init(void) {
    if (services_ptr_name.len) return;
    hv_dns_name_from(&services_ptr_name, "_services._dns-sd._udp.local");
    for (int t = 0; t < LAN_TYPES; t++) {
        char n[64];
        snprintf(n, sizeof n, "%s.local", lan_type_text[t]);
        hv_dns_name_from(&lan_type[t], n);
    }
    for (int k = 0; k < LAN_INSTANCES; k++) lan_inst[k].host = -1;
}

/* Each tick: browse while the Mac runs on screen; lookups end; a clash looked up. ms until it has more to do. */
static int64_t lan_tick(int run) {
    lan_init();
    if (lan_test) return INT64_MAX;
    if (!run) {
        if (lan_on) lan_stop();
        return INT64_MAX;
    }
    if (!lan_on) lan_start();
    /* Another device has the Mac's name: its address is looked up and told to the Mac (lan_addr). */
    if (taken && lan_clash < 0) {
        lan_clash = host_get(&our_name, 0);
        if (lan_clash >= 0) lan_host[lan_clash].users++;
    } else if (!taken && lan_clash >= 0) {
        int h = lan_clash;
        lan_clash = -1;
        host_release(h);
    }
    int64_t next = INT64_MAX;
    for (int h = 0; h < LAN_HOSTS; h++) {
        LanHost *x = &lan_host[h];
        if (!x->used || !x->lookup) continue;
        if (!x->naddr && !x->missed && now_cached - x->started >= 3000) { /* not found: kept 10 s, not asked again */
            if (x->ref) DNSServiceRefDeallocate(x->ref);
            x->ref = NULL, x->missed = 1, x->until = now_cached + 10000;
        }
        if (x->until && now_cached >= x->until) {
            x->lookup = 0;
            if (x->users <= 0 && h != lan_clash) {
                host_free(h);
                continue;
            }
        }
        int64_t due = x->until ? x->until : x->started + 3000;
        if (due < next) next = due;
    }
    return next == INT64_MAX ? INT64_MAX : next > now_cached ? next - now_cached : 0;
}

/* The devices the Mac is told of, for Details: their names, one a line; how many. */
static int lan_seen(char *out, size_t cap) {
    int n = 0;
    size_t o = 0;
    if (cap) out[0] = 0;
    for (int h = 0; h < LAN_HOSTS; h++) {
        if (!host_told(&lan_host[h]) || h == lan_clash) continue;
        const LanInst *pick = NULL;
        for (int k = 0; k < LAN_INSTANCES; k++) {
            const LanInst *i = &lan_inst[k];
            if (i->host == h && inst_told(i) && (!pick || !label_is(&lan_type[pick->type], 0, "_device-info"))) pick = i;
        }
        if (!pick) continue;
        int l;
        const uint8_t *b = label(&pick->name, 0, &l);
        if (o + (size_t)l + 2 <= cap) memcpy(out + o, b, (size_t)l), o += (size_t)l, out[o++] = '\n', out[o] = 0;
        n++;
    }
    return n;
}

uint64_t hv_mdns_lookups(void) { return atomic_load(&lan_lookups_started); }

void hv_mdns_test_away(int64_t pass_ms) {
    pthread_mutex_lock(&test_mu);
    test_pass_ms += pass_ms;
    atomic_fetch_or(&test_wanted, 1);
    pthread_mutex_unlock(&test_mu);
}

void hv_mdns_test_back(void) {
    pthread_mutex_lock(&test_mu);
    atomic_fetch_or(&test_wanted, 2);
    pthread_mutex_unlock(&test_mu);
}

uint64_t hv_mdns_test_ticks(void) { return atomic_load(&ticks); }

void hv_mdns_test_found(const char *instance, const char *hostname, uint32_t addr, uint16_t port, const uint8_t *txt,
                        uint16_t txt_len) {
    lan_init();
    lan_test = lan_on = 1;
    HvDnsName in, t, h;
    if (hv_dns_name_parse(&in, instance) || hv_dns_name_parse(&h, hostname)) return;
    if (txt_len > MAX_TXT || !txt_ok(txt, txt_len)) txt_len = 0; /* as lan_resolved has it */
    parent(&in, &t);
    int type = find_type(&t), k = find_inst(&in);
    if (type < 0) return;
    for (k = k >= 0 ? k : 0; k < LAN_INSTANCES && lan_inst[k].used && !hv_dns_name_eq(&lan_inst[k].name, &in); k++) {}
    if (k == LAN_INSTANCES) return;
    LanInst *i = &lan_inst[k];
    if (!i->used) {
        memset(i, 0, sizeof *i);
        i->used = 1, i->type = type, i->name = in, i->seen = 1, i->host = host_get(&h, 0);
        if (i->host >= 0) lan_host[i->host].users++;
    }
    i->resolved = 1, i->port = port, i->txt_len = txt_len;
    memcpy(i->txt, txt, txt_len);
    if (i->host >= 0 && lan_host[i->host].naddr < LAN_ADDRS && !device_addr(addr)) lan_host[i->host].addr[lan_host[i->host].naddr++] = addr;
}
