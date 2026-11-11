/* The Mac's name and services on the device's network (host/mdns.c, through host/net_slirp.c): what the Mac's own
   Bonjour says of itself is registered on the device under the same name, at the device's address. Played here: the
   guest's responder, with raw frames (it answers what Henge asks, and announces); the device's network is this Mac's
   own Bonjour, local only (kDNSServiceInterfaceIndexLocalOnly: nothing leaves this Mac), browsed and resolved as any
   Mac on the network would. Checked: Henge asks the Mac what it is and offers as it starts, from the gateway, as an
   mDNS querier does; a service it learns that way is announced with the Mac's name, its port, its TXT, and the name
   resolves to the device's address; its port is forwarded though no helper or standard set has it; a TXT change
   reaches the network; a service whose port the device itself has isn't announced; the Mac's goodbye withdraws it and
   its announcement brings it back; held (Henge off the screen), nothing is announced, and after it the Mac is asked
   again and everything is back, and while held nothing is due (the network thread rests); the Mac's name's goodbye
   withdraws the name and its services; a TXT too long for one packet is left out, not the service; a short-lived
   record is asked for before it ends, and when the Mac stops answering it is withdrawn; the Mac's name, taken by
   another device, is withdrawn, and Henge picks no name of its own; the Mac is told the other device's address for
   it, as it would hear it on a real network; once the Mac takes another, as a Mac does, that one is announced, the
   services on it, and it stays.
   5b, the network's devices told to the Mac (frames to it parsed and logged): a file server appearing is announced
   to the Mac unasked, with its SRV, TXT and address; the Mac's question is answered with what goes with the
   answer, and so is its list of kinds; a known answer isn't said again unless under half its TTL is left; a QU
   question is answered to the Mac alone; a plain DNS client gets its ID and question back, TTLs of at most 10 s and
   no cache-flush; Details lists the server; held, nothing is said to the Mac, and after, the server is told again
   unasked; its goodbye reaches the Mac; 30 web servers are answered in as many messages as they take, each within
   the MTU; a name nothing offered is looked up when asked, then said, once, 16 at a time; the Mac's own service,
   found on the device's network, and a device on the device's own address are never said to it; renamed while its
   old name is still about at another address, the old name isn't said to it (so it doesn't rename itself), and back
   to it, no clash. */
#include <arpa/inet.h>
#include <dlfcn.h>
#include <dns_sd.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
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

static int checks, failures;
static void check(const char *what, long long got, long long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got %lld, want %lld\n", what, got, want);
    }
}
static void check_str(const char *what, const char *got, const char *want) {
    checks++;
    if (strcmp(got, want)) {
        failures++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    }
}

static HvMachine m;
static HvVirtioNet *n;
static const uint8_t guest_mac[6] = {0x02, 0x48, 0x45, 0x4e, 0x47, 0x45};

static uint16_t csum(const uint8_t *p, size_t len, uint32_t sum) {
    for (size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t)p[i] << 8 | p[i + 1];
    if (len & 1) sum += (uint32_t)p[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}
static uint8_t *put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; return p + 2; }
static uint8_t *put32(uint8_t *p, uint32_t v) { return put16(put16(p, (uint16_t)(v >> 16)), (uint16_t)v); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* ---------------------------------------------------------------- the guest's responder */

typedef struct GRec {
    int used, silent;
    char name[200];
    uint16_t type;
    uint32_t ttl;
    char target[200]; /* PTR, SRV */
    uint16_t port;    /* SRV */
    uint8_t txt[1400];
    int txt_len;
    uint32_t a; /* A: the address, host order; 0 for the guest's own (10.0.2.15) */
} GRec;
static GRec zone[64];
static pthread_mutex_t zone_mu = PTHREAD_MUTEX_INITIALIZER;

static GRec *zone_set(const char *name, uint16_t type, uint32_t ttl, const char *target, uint16_t port, const char *txt) {
    pthread_mutex_lock(&zone_mu);
    GRec *r = NULL;
    for (int i = 0; i < 64 && !r; i++)
        if (zone[i].used && zone[i].type == type && !strcasecmp(zone[i].name, name) &&
            (type != 12 || !strcasecmp(zone[i].target, target ? target : "")))
            r = &zone[i];
    for (int i = 0; i < 64 && !r; i++)
        if (!zone[i].used) r = &zone[i];
    *r = (GRec){.used = 1, .type = type, .ttl = ttl, .port = port};
    snprintf(r->name, sizeof r->name, "%s", name);
    if (target) snprintf(r->target, sizeof r->target, "%s", target);
    for (const char *s = txt; s && *s;) { /* "a=1;b=2" -> TXT strings */
        const char *e = strchr(s, ';');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        r->txt[r->txt_len++] = (uint8_t)l;
        memcpy(r->txt + r->txt_len, s, l);
        r->txt_len += (int)l;
        s += l + (e != NULL);
    }
    pthread_mutex_unlock(&zone_mu);
    return r;
}

static void zone_silence(const char *name, int silent) {
    pthread_mutex_lock(&zone_mu);
    for (int i = 0; i < 64; i++)
        if (zone[i].used && (!strcasecmp(zone[i].name, name) || !strcasecmp(zone[i].target, name))) zone[i].silent = silent;
    pthread_mutex_unlock(&zone_mu);
}

static void zone_drop(GRec *r) {
    pthread_mutex_lock(&zone_mu);
    r->used = 0;
    pthread_mutex_unlock(&zone_mu);
}

static uint8_t *put_name(uint8_t *p, const char *dotted) {
    HvDnsName nm;
    hv_dns_name_from(&nm, dotted);
    memcpy(p, nm.b, nm.len);
    return p + nm.len;
}

static uint8_t *put_rr(uint8_t *p, const GRec *r, uint32_t ttl) {
    p = put_name(p, r->name);
    p = put16(p, r->type), p = put16(p, (uint16_t)(1 | (r->type != 12 ? 0x8000 : 0))), p = put32(p, ttl);
    uint8_t *rdlen = p;
    p += 2;
    if (r->type == 1) {
        uint32_t a = r->a ? r->a : 0x0a00020f;
        *p++ = (uint8_t)(a >> 24), *p++ = (uint8_t)(a >> 16), *p++ = (uint8_t)(a >> 8), *p++ = (uint8_t)a;
    }
    else if (r->type == 12) p = put_name(p, r->target);
    else if (r->type == 33) p = put16(put16(put16(p, 0), 0), r->port), p = put_name(p, r->target);
    else if (r->type == 16) {
        if (!r->txt_len) *p++ = 0;
        memcpy(p, r->txt, (size_t)r->txt_len), p += r->txt_len;
    }
    put16(rdlen, (uint16_t)(p - rdlen - 2));
    return p;
}

/* A message from the guest: 10.0.2.15:sport to 224.0.0.251:5353 (its responder's are from 5353). */
static void guest_send_from(const uint8_t *dns, size_t len, uint16_t sport) {
    uint8_t f[14 + 20 + 8 + 1500];
    static const uint8_t to[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb};
    memcpy(f, to, 6), memcpy(f + 6, guest_mac, 6), put16(f + 12, 0x0800);
    uint8_t *ip = f + 14, *udp = ip + 20;
    memset(ip, 0, 20);
    ip[0] = 0x45, put16(ip + 2, (uint16_t)(28 + len)), ip[8] = 255, ip[9] = 17;
    ip[12] = 10, ip[14] = 2, ip[15] = 15, ip[16] = 224, ip[19] = 251;
    put16(ip + 10, csum(ip, 20, 0));
    put16(udp, sport), put16(udp + 2, 5353), put16(udp + 4, (uint16_t)(8 + len)), put16(udp + 6, 0);
    memcpy(udp + 8, dns, len);
    n->transmit(n->opaque, f, (uint32_t)(42 + len));
}
static void guest_send(const uint8_t *dns, size_t len) { guest_send_from(dns, len, 5353); }

/* Announce records (ttl 0: their goodbye), as the Mac's responder does when something changes. */
static void announce(GRec **rs, int k, int goodbye) {
    uint8_t b[1500] = {0}, *p = b + 12;
    put16(b + 2, 0x8400), put16(b + 6, (uint16_t)k);
    for (int i = 0; i < k; i++) p = put_rr(p, rs[i], goodbye ? 0 : rs[i]->ttl);
    guest_send(b, (size_t)(p - b));
}

/* What Henge asked, by "name/type" (lower case), and how the frames came. */
typedef struct Asked {
    char key[300];
    int count;
} Asked;
static Asked asked_log[256];
static int nasked;
static _Atomic int frames_seen, frames_bad, responses_from_henge;

/* Another test_mdns on this Mac shares the system's LocalOnly domain: its services are, rightly, told to this one's
   guest. So what is counted as this run's is what carries this process's pid (a number on its own, not part of a
   longer one). */
static int has_pid(const char *s) {
    static char pid[16];
    if (!pid[0]) snprintf(pid, sizeof pid, "%d", (int)getpid());
    size_t l = strlen(pid);
    for (const char *p = s; (p = strstr(p, pid)); p++)
        if ((p == s || p[-1] < '0' || p[-1] > '9') && (p[l] < '0' || p[l] > '9')) return 1;
    return 0;
}

static int asked(const char *name, int type) {
    char key[300];
    snprintf(key, sizeof key, "%s/%d", name, type);
    for (char *c = key; *c; c++) *c = (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
    int k = 0;
    pthread_mutex_lock(&zone_mu);
    for (int i = 0; i < nasked; i++)
        if (!strcmp(asked_log[i].key, key)) k = asked_log[i].count;
    pthread_mutex_unlock(&zone_mu);
    return k;
}

/* What Henge said to the guest (its answers and announcements of the network's devices, 2b): every record, and
   each message's shape. */
typedef struct Said {
    char name[300], target[300]; /* target: PTR, SRV */
    uint16_t type, port;
    uint8_t flush;
    uint32_t ttl, a; /* a: network order */
    int frame;
} Said;
typedef struct SaidFrame {
    uint8_t unicast;
    uint16_t dport, id;
    int nq;
    char q0[300];
    uint32_t len;
} SaidFrame;
static Said said[16384];
static int nsaid;
static SaidFrame sframe[4096];
static int nsframe;
static _Atomic int frames_big;

static void name_of(const HvDnsName *nm, char *out) {
    if (hv_dns_name_text(nm, out, 300)) {
        out[0] = 0;
        return;
    }
    size_t l = strlen(out);
    if (l && out[l - 1] == '.') out[l - 1] = 0;
}

/* Network thread: every frame for the guest. Henge's mDNS questions are answered from the zone; what it says is
   logged. */
static HvDnsMsg qmsg;
static void watch(const uint8_t *f, uint32_t len, int to_guest) {
    if (!to_guest || len < 42 || get16(f + 12) != 0x0800) return;
    const uint8_t *ip = f + 14, *udp = ip + 20;
    static const uint8_t group[4] = {224, 0, 0, 251}, gw[4] = {10, 0, 2, 2}, guest_ip[4] = {10, 0, 2, 15};
    int unicast = !memcmp(ip + 16, guest_ip, 4);
    if (ip[9] != 17 || (unicast ? get16(udp) != 5353 : memcmp(ip + 16, group, 4) || get16(udp + 2) != 5353)) return;
    frames_seen++;
    if (len > 14 + 1500) frames_big++;
    uint16_t ulen = get16(udp + 4);
    uint8_t pseudo[12] = {0};
    memcpy(pseudo, ip + 12, 8), pseudo[9] = 17, put16(pseudo + 10, ulen);
    uint32_t sum = 0;
    for (int i = 0; i < 12; i += 2) sum += (uint32_t)pseudo[i] << 8 | pseudo[i + 1];
    static const uint8_t to[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb};
    if (memcmp(f, unicast ? guest_mac : to, 6) || memcmp(ip + 12, gw, 4) || ip[8] != 255 || get16(udp) != 5353 || csum(ip, 20, 0) ||
        ulen + 34u > len || csum(udp, ulen, sum))
        frames_bad++;
    if (hv_dns_parse(udp + 8, ulen - 8u, &qmsg)) {
        frames_bad++;
        return;
    }
    if (qmsg.flags & 0x8000) {
        int mine = 0;
        pthread_mutex_lock(&zone_mu);
        if (nsframe < 4096) {
            SaidFrame *s = &sframe[nsframe];
            *s = (SaidFrame){.unicast = (uint8_t)unicast, .dport = get16(udp + 2), .id = qmsg.id, .nq = qmsg.nq, .len = len};
            if (qmsg.nq) name_of(&qmsg.q[0].name, s->q0);
            for (int i = 0; i < qmsg.nrr && nsaid < 16384; i++) {
                const HvDnsRR *x = &qmsg.rr[i];
                Said *d = &said[nsaid++];
                *d = (Said){.type = x->type, .port = x->port, .flush = x->top, .ttl = x->ttl, .a = x->a, .frame = nsframe};
                name_of(&x->name, d->name);
                if (x->type == 12 || x->type == 33) name_of(&x->target, d->target);
                mine |= has_pid(d->name) || has_pid(d->target);
            }
            nsframe++;
        }
        pthread_mutex_unlock(&zone_mu);
        if (mine) responses_from_henge++; /* about this run's names: not another test_mdns's services */
        return;
    }
    if (unicast) return;
    uint8_t b[1400] = {0}, *p = b + 12;
    int k = 0;
    pthread_mutex_lock(&zone_mu);
    for (int q = 0; q < qmsg.nq; q++) {
        char name[300], key[310];
        hv_dns_name_text(&qmsg.q[q].name, name, sizeof name);
        size_t l = strlen(name);
        if (l && name[l - 1] == '.') name[l - 1] = 0;
        snprintf(key, sizeof key, "%s/%d", name, qmsg.q[q].type);
        for (char *c = key; *c; c++) *c = (char)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
        int i = 0;
        while (i < nasked && strcmp(asked_log[i].key, key)) i++;
        if (i == nasked && nasked < 256) snprintf(asked_log[nasked++].key, sizeof asked_log[0].key, "%s", key);
        if (i < 256) asked_log[i].count++;
        for (int z = 0; z < 64; z++)
            if (zone[z].used && !zone[z].silent && zone[z].type == qmsg.q[q].type && !strcasecmp(zone[z].name, name) &&
                p - b < 1100)
                p = put_rr(p, &zone[z], zone[z].ttl), k++;
    }
    pthread_mutex_unlock(&zone_mu);
    if (!k) return;
    put16(b + 2, 0x8400), put16(b + 6, (uint16_t)k);
    guest_send(b, (size_t)(p - b));
}

/* ---------------------------------------------------------------- the device's network, as another Mac sees it */

static DNSServiceRef refs[16];
static int nrefs;

static void tick(void) {
    hv_virtio_net_poll(n);
    hv_net_poll();
}

static void pump(int ms) {
    for (int t = 0; t < ms; t++) {
        tick();
        struct pollfd p[16];
        for (int i = 0; i < nrefs; i++) p[i] = (struct pollfd){DNSServiceRefSockFD(refs[i]), POLLIN, 0};
        if (!nrefs) {
            usleep(1000);
            continue;
        }
        if (poll(p, (nfds_t)nrefs, 1) > 0)
            for (int i = 0; i < nrefs; i++)
                if (p[i].revents) DNSServiceProcessResult(refs[i]);
    }
}

static void add_ref(DNSServiceRef r) { refs[nrefs++] = r; }
static void drop_ref(DNSServiceRef r) {
    for (int i = 0; i < nrefs; i++)
        if (refs[i] == r) refs[i] = refs[--nrefs];
    DNSServiceRefDeallocate(r);
}

/* Browse: the instances there now. */
static char present[16][64];
static int npresent;
static void DNSSD_API browsed(DNSServiceRef r, DNSServiceFlags fl, uint32_t ifi, DNSServiceErrorType e, const char *name,
                              const char *type, const char *dom, void *ctx) {
    (void)r, (void)ifi, (void)type, (void)dom, (void)ctx;
    if (e || !has_pid(name)) return; /* another test_mdns's services fill no slot here */
    int i = 0;
    while (i < npresent && strcmp(present[i], name)) i++;
    if ((fl & kDNSServiceFlagsAdd) && i == npresent && npresent < 16) snprintf(present[npresent++], 64, "%s", name);
    if (!(fl & kDNSServiceFlagsAdd) && i < npresent) memcpy(present[i], present[--npresent], 64);
}
static int is_present(const char *name) {
    for (int i = 0; i < npresent; i++)
        if (!strcmp(present[i], name)) return 1;
    return 0;
}
static int wait_present(const char *name, int want, int ms) {
    /* Every wait is for something that should come: at least 15 s for it, so a MacBook busy with a dozen other
       builds and test copies (load 50) doesn't fail a check that only came late. It returns as soon as it comes. */
    if (ms < 15000) ms = 15000;
    for (int t = 0; t < ms; t += 10) {
        if (is_present(name) == want) return 1;
        pump(10);
    }
    return is_present(name) == want;
}

/* Resolve: where an instance is. */
typedef struct Resolved {
    int done;
    char host[300];
    uint16_t port;
    char txt[256];
} Resolved;
static void DNSSD_API resolved(DNSServiceRef r, DNSServiceFlags fl, uint32_t ifi, DNSServiceErrorType e, const char *full,
                               const char *host, uint16_t port, uint16_t tl, const unsigned char *txt, void *ctx) {
    (void)r, (void)fl, (void)ifi, (void)full;
    Resolved *o = ctx;
    if (e) return;
    snprintf(o->host, sizeof o->host, "%s", host);
    o->port = ntohs(port);
    /* The TXT's strings, ";"-joined. */
    size_t at = 0, w = 0;
    while (at < tl && w + 1 < sizeof o->txt) {
        size_t l = txt[at++];
        if (w) o->txt[w++] = ';';
        for (size_t i = 0; i < l && at < tl && w + 1 < sizeof o->txt; i++) o->txt[w++] = (char)txt[at++];
    }
    o->txt[w] = 0;
    o->done = 1;
}
static Resolved resolve(const char *inst, const char *type) {
    Resolved o = {0};
    DNSServiceRef r;
    if (DNSServiceResolve(&r, 0, kDNSServiceInterfaceIndexLocalOnly, inst, type, "local.", resolved, &o)) return o;
    add_ref(r);
    for (int t = 0; t < 3000 && !o.done; t += 10) pump(10);
    drop_ref(r);
    return o;
}

/* The address a name has. */
typedef struct Addr {
    int done;
    char ip[INET_ADDRSTRLEN];
} Addr;
static void DNSSD_API got_addr(DNSServiceRef r, DNSServiceFlags fl, uint32_t ifi, DNSServiceErrorType e, const char *host,
                               const struct sockaddr *a, uint32_t ttl, void *ctx) {
    (void)r, (void)ifi, (void)host, (void)ttl;
    Addr *o = ctx;
    if (e || !(fl & kDNSServiceFlagsAdd) || a->sa_family != AF_INET) return;
    inet_ntop(AF_INET, &((const struct sockaddr_in *)a)->sin_addr, o->ip, sizeof o->ip);
    o->done = 1;
}
static Addr address_of(const char *host) {
    Addr o = {0};
    DNSServiceRef r;
    if (DNSServiceGetAddrInfo(&r, 0, kDNSServiceInterfaceIndexLocalOnly, kDNSServiceProtocol_IPv4, host, got_addr, &o)) return o;
    add_ref(r);
    for (int t = 0; t < 3000 && !o.done; t += 10) pump(10);
    drop_ref(r);
    return o;
}

static void DNSSD_API reg_cb(DNSServiceRef c, DNSRecordRef rec, DNSServiceFlags fl, DNSServiceErrorType e, void *ctx) {
    (void)c, (void)rec, (void)fl, (void)e, (void)ctx;
}

/* ---------------------------------------------------------------- the network's other devices (2b) */

static DNSServiceRef lanc; /* their registrations, on this Mac's own Bonjour, local only */

static int host_rec(const char *host, uint32_t ip, DNSRecordRef *rec) {
    char full[120];
    snprintf(full, sizeof full, "%s.", host);
    struct in_addr ia = {htonl(ip)};
    return DNSServiceRegisterRecord(lanc, rec, kDNSServiceFlagsUnique, kDNSServiceInterfaceIndexLocalOnly, full, kDNSServiceType_A,
                                    kDNSServiceClass_IN, 4, &ia, 120, reg_cb, NULL);
}
static void DNSSD_API svc_cb(DNSServiceRef r, DNSServiceFlags fl, DNSServiceErrorType e, const char *nm, const char *t,
                             const char *d, void *ctx) {
    (void)r, (void)fl, (void)e, (void)nm, (void)t, (void)d, (void)ctx;
}
static DNSServiceRef lan_service(const char *inst, const char *type, const char *host, uint16_t port, const void *txt,
                                 uint16_t txt_len) {
    DNSServiceRef r = lanc;
    char h[120];
    snprintf(h, sizeof h, "%s.", host);
    if (DNSServiceRegister(&r, kDNSServiceFlagsShareConnection, kDNSServiceInterfaceIndexLocalOnly, inst, type, "local.", h,
                           htons(port), txt_len, txt, svc_cb, NULL))
        return NULL;
    return r;
}

/* Bonjour's register, between Henge and the device's Bonjour (a definition here is what mdns.o links to; the real one is
   the next). The test's own registrations (svc_cb) pass straight through. Henge's are counted, with any that would
   let Bonjour rename the Mac's service on its own (the Mac names its own: kDNSServiceFlagsNoAutoRename). And a
   name set in late_name is held, so that a conflict can be told to Henge later, on its own thread and inside its
   own DNSServiceProcessResult, as Bonjour tells one found on the network after the name was announced (here every
   name is local-only, and such a conflict is refused at once instead). */
static _Atomic int henge_regs, henge_regs_renamable, late_told;
static pthread_mutex_t late_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char name[80], type[40];
    int due;
    DNSServiceRef ref;
    DNSServiceRegisterReply cb;
    void *ctx;
} late_reg;

DNSServiceErrorType DNSSD_API DNSServiceRegister(DNSServiceRef *ref, DNSServiceFlags fl, uint32_t ifi, const char *name,
                                                 const char *type, const char *dom, const char *host, uint16_t port,
                                                 uint16_t txt_len, const void *txt, DNSServiceRegisterReply cb, void *ctx) {
    static DNSServiceErrorType (*real)(DNSServiceRef *, DNSServiceFlags, uint32_t, const char *, const char *, const char *,
                                       const char *, uint16_t, uint16_t, const void *, DNSServiceRegisterReply, void *);
    if (!real) *(void **)&real = dlsym(RTLD_NEXT, "DNSServiceRegister");
    DNSServiceErrorType e = real(ref, fl, ifi, name, type, dom, host, port, txt_len, txt, cb, ctx);
    if (cb == svc_cb) return e;
    henge_regs++;
    if (!(fl & kDNSServiceFlagsNoAutoRename)) henge_regs_renamable++;
    pthread_mutex_lock(&late_mu);
    if (!e && name && late_reg.name[0] && !strcmp(name, late_reg.name))
        late_reg.ref = *ref, late_reg.cb = cb, late_reg.ctx = ctx, snprintf(late_reg.type, sizeof late_reg.type, "%s", type);
    pthread_mutex_unlock(&late_mu);
    return e;
}

DNSServiceErrorType DNSSD_API DNSServiceProcessResult(DNSServiceRef r) {
    static DNSServiceErrorType (*real)(DNSServiceRef);
    if (!real) *(void **)&real = dlsym(RTLD_NEXT, "DNSServiceProcessResult");
    DNSServiceErrorType e = real(r);
    if (pthread_main_np()) return e; /* the test's own; Henge's run on the network thread */
    DNSServiceRegisterReply cb = NULL;
    DNSServiceRef ref = NULL;
    void *ctx = NULL;
    char name[80], type[40];
    pthread_mutex_lock(&late_mu);
    if (late_reg.due && late_reg.cb) {
        cb = late_reg.cb, ref = late_reg.ref, ctx = late_reg.ctx, late_reg.due = 0, late_reg.cb = NULL;
        snprintf(name, sizeof name, "%s", late_reg.name), snprintf(type, sizeof type, "%s", late_reg.type);
    }
    pthread_mutex_unlock(&late_mu);
    if (cb) late_told++, cb(ref, 0, kDNSServiceErr_NameConflict, name, type, "local.", ctx);
    return e;
}

/* The guest's question, from sport (5353: its responder; else a plain DNS client), QU or not, with one known answer. */
static void guest_ask(const char *name, uint16_t type, int qu, uint16_t sport, uint16_t id, const GRec *known) {
    uint8_t b[600] = {0}, *p = b + 12;
    put16(b, id), put16(b + 4, 1);
    p = put_name(p, name), p = put16(p, type), p = put16(p, (uint16_t)(1 | (qu ? 0x8000 : 0)));
    if (known) put16(b + 6, 1), p = put_rr(p, known, known->ttl);
    guest_send_from(b, (size_t)(p - b), sport);
}

/* Records Henge said since index from: by name, type, target (NULL: any); live (TTL > 0) or goodbyes. The last
   one in *out. */
static int said_find(int from, const char *name, int type, const char *target, int live, Said *out) {
    int k = 0;
    pthread_mutex_lock(&zone_mu);
    for (int i = from; i < nsaid; i++)
        if (!strcasecmp(said[i].name, name) && said[i].type == type && (!target || !strcasecmp(said[i].target, target)) &&
            (said[i].ttl > 0) == live) {
            k++;
            if (out) *out = said[i];
        }
    pthread_mutex_unlock(&zone_mu);
    return k;
}
static int wait_said(int from, const char *name, int type, const char *target, int live, int ms) {
    for (int t = 0; t < ms && !said_find(from, name, type, target, live, NULL); t += 10) pump(10);
    return said_find(from, name, type, target, live, NULL) > 0;
}
/* Waits (pumping) until the network thread has begun n more ticks: what was asked of it before is done, and the
   guest's frames handed over before (pump hands them over) are taken. 1 if it did, in 10 s. */
static int net_ticks(int n) {
    pump(1);
    uint64_t t0 = hv_mdns_test_ticks();
    for (int t = 0; t < 10000 && hv_mdns_test_ticks() < t0 + (uint64_t)n; t++) pump(1);
    return hv_mdns_test_ticks() >= t0 + (uint64_t)n;
}
static SaidFrame frame_of(int i) {
    pthread_mutex_lock(&zone_mu);
    SaidFrame f = sframe[i];
    pthread_mutex_unlock(&zone_mu);
    return f;
}
/* How many different PTR targets starting with prefix were said for name since from. */
static int distinct_ptrs(int from, const char *name, const char *prefix) {
    static char seen[128][300];
    int k = 0;
    pthread_mutex_lock(&zone_mu);
    for (int i = from; i < nsaid; i++) {
        if (said[i].type != 12 || !said[i].ttl || strcasecmp(said[i].name, name) || strncasecmp(said[i].target, prefix, strlen(prefix)))
            continue;
        int j = 0;
        while (j < k && strcasecmp(seen[j], said[i].target)) j++;
        if (j == k && k < 128) snprintf(seen[k++], 300, "%s", said[i].target);
    }
    pthread_mutex_unlock(&zone_mu);
    return k;
}
/* Anything said of a name (as a record's name or target, or a part of it), or an A record of an address. */
static int said_of(const char *part, uint32_t addr) {
    int k = 0;
    pthread_mutex_lock(&zone_mu);
    for (int i = 0; i < nsaid; i++)
        if ((part && (strcasestr(said[i].name, part) || strcasestr(said[i].target, part))) ||
            (addr && said[i].type == 1 && said[i].a == htonl(addr)))
            k++;
    pthread_mutex_unlock(&zone_mu);
    return k;
}

/* The guest's name, changed as in its Sharing settings: the old one's goodbye, the new one and what is on it
   announced. */
static GRec *g_a, *g_rev;
static void guest_rename(const char *to) {
    char old[200];
    snprintf(old, sizeof old, "%s", g_a->name);
    GRec *bye[2] = {g_a, g_rev};
    announce(bye, 2, 1);
    zone_drop(g_a), zone_drop(g_rev);
    g_a = zone_set(to, 1, 120, NULL, 0, NULL);
    g_rev = zone_set("15.2.0.10.in-addr.arpa", 12, 120, to, 0, NULL);
    GRec *now[34] = {g_a};
    int k = 1;
    pthread_mutex_lock(&zone_mu);
    for (int i = 0; i < 64; i++)
        if (zone[i].used && zone[i].type == 33 && !strcasecmp(zone[i].target, old)) snprintf(zone[i].target, sizeof zone[i].target, "%s", to), now[k++] = &zone[i];
    pthread_mutex_unlock(&zone_mu);
    announce(now, k, 0);
}

/* ---------------------------------------------------------------- the forwarding's view */

static int status_of(uint16_t port, int *held) {
    HvFwdStatus s[600];
    int k = hv_net_forward_status(s, 600, NULL, held);
    for (int i = 0; i < k; i++)
        if (s[i].port == port && !s[i].udp) return s[i].err;
    return -1;
}

static uint16_t free_port(void) {
    int t = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET};
    socklen_t al = sizeof a;
    bind(t, (struct sockaddr *)&a, sizeof a);
    getsockname(t, (struct sockaddr *)&a, &al);
    close(t);
    return ntohs(a.sin_port);
}

static int unlisted = -1;
static int status_name(char *name, size_t cap, int *taken, int *services) {
    int denied = 0;
    hv_mdns_status(name, cap, taken, services, &denied, &unlisted, NULL);
    return denied;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&m, 4 << 20) != 0) return 1;
    HvGic *gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    HvPcie *bus = hv_pcie_create(&m, gic, 0x10000000, 0x10000000, 0x50000000, 0x10000000, 32 + 4);
    n = hv_virtio_net_create(&m, bus, guest_mac);
    hv_net_watch = watch;
    setenv("HV_DNS", "off", 1);
    hv_mdns_local_only(1), hv_net_ports_loopback(1); /* nothing here is reachable from the MacBook's network */
    hv_net_ports_standard(NULL, 0); /* nothing forwarded but what the Mac announces */

    int pid = (int)getpid();
    char mac[64], mac_local[80], inst[64], inst_full[160], type_full[80] = "_hengetest._tcp.local";
    snprintf(mac, sizeof mac, "testmac-%d", pid);
    snprintf(mac_local, sizeof mac_local, "%s.local", mac);
    snprintf(inst, sizeof inst, "Henge Test %d", pid);
    snprintf(inst_full, sizeof inst_full, "%s._hengetest._tcp.local", inst);
    uint16_t port = free_port();

    /* The guest's zone, answered when asked (nothing announced yet). */
    GRec *a = zone_set(mac_local, 1, 120, NULL, 0, NULL);
    GRec *rev = zone_set("15.2.0.10.in-addr.arpa", 12, 120, mac_local, 0, NULL);
    zone_set("_services._dns-sd._udp.local", 12, 4500, type_full, 0, NULL);
    GRec *ptr = zone_set(type_full, 12, 4500, inst_full, 0, NULL);
    GRec *srv = zone_set(inst_full, 33, 120, mac_local, port, NULL);
    GRec *txt = zone_set(inst_full, 16, 4500, NULL, 0, "path=/x;a=1");

    DNSServiceRef browse;
    check("browsing starts", DNSServiceBrowse(&browse, 0, kDNSServiceInterfaceIndexLocalOnly, "_hengetest._tcp", "local.", browsed, NULL), 0);
    add_ref(browse);

    /* 1. Started, Henge asks the Mac what it is and offers; the Mac's answers are enough. */
    check("the NAT starts", hv_net_attach(&m, n), 0);
    check("the service is announced, from the Mac's answers alone", wait_present(inst, 1, 10000), 1);
    check("Henge asked for the Mac's services", asked("_services._dns-sd._udp.local", 12) >= 1, 1);
    check("and for the name of the Mac's address", asked("15.2.0.10.in-addr.arpa", 12) >= 1, 1);
    check("and for the type's instances", asked(type_full, 12) >= 1, 1);
    check("and for the service's SRV", asked(inst_full, 33) >= 1, 1);
    check("every frame from the gateway, as an mDNS querier sends it", frames_bad, 0);
    check("questions only (nothing said for the Mac on its own network)", responses_from_henge, 0);
    Resolved r = resolve(inst, "_hengetest._tcp");
    check("it resolves", r.done, 1);
    char want_host[100];
    snprintf(want_host, sizeof want_host, "%s.", mac_local);
    check_str("to the Mac's own name", r.host, want_host);
    check("and its port", r.port, port);
    check_str("with its TXT", r.txt, "path=/x;a=1");
    Addr ad = address_of(mac_local);
    check_str("the Mac's name is the device's address", ad.ip, "127.0.0.1");
    check("the service's port is forwarded, though nothing else has it", status_of(port, NULL), 0);
    char wide[96];
    check("every port this test listens on is on 127.0.0.1 only", open_beyond_loopback(wide, sizeof wide), 0);
    if (wide[0]) printf("  %s\n", wide);
    char nm[300];
    int taken = -1, services = -1;
    check("iOS didn't refuse", status_name(nm, sizeof nm, &taken, &services), 0);
    check_str("the status says the Mac's name", nm, mac_local);
    check("not taken", taken, 0);
    check("one service", services, 1);
    check("none refused", unlisted, 0);

    /* 1b. The guest says it knows another name at another address (a proxy, a VM inside it, a stale record): only
       the Mac's own address names the Mac, so its name stays, and the other isn't announced as the Mac. */
    char other_local[80];
    snprintf(other_local, sizeof other_local, "other-%d.local", pid);
    static GRec other_a;
    other_a = (GRec){.used = 1, .type = 1, .ttl = 120, .a = 0x0a000263}; /* 10.0.2.99 */
    snprintf(other_a.name, sizeof other_a.name, "%s", other_local);
    GRec *oa = &other_a;
    announce(&oa, 1, 0);
    pump(2000);
    check("another address's name doesn't name the Mac", status_name(nm, sizeof nm, NULL, NULL), 0);
    check_str("the Mac's name stays", nm, mac_local);
    check("and that name isn't announced", address_of(other_local).done, 0);
    announce(&oa, 1, 1);

    /* 2. The TXT changes: the network sees it. */
    txt = zone_set(inst_full, 16, 4500, NULL, 0, "path=/y");
    announce(&txt, 1, 0);
    int changed = 0;
    for (int t = 0; t < 30 && !changed; t++) {
        pump(100);
        r = resolve(inst, "_hengetest._tcp");
        changed = !strcmp(r.txt, "path=/y");
    }
    check("a TXT change reaches the network", changed, 1);

    /* 3. A service on a port this device itself has isn't announced (no one could reach it). */
    int mine = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t sl = sizeof sa;
    bind(mine, (struct sockaddr *)&sa, sizeof sa), listen(mine, 1), getsockname(mine, (struct sockaddr *)&sa, &sl);
    uint16_t busy = ntohs(sa.sin_port);
    char binst[64], binst_full[160];
    snprintf(binst, sizeof binst, "Busy %d", pid);
    snprintf(binst_full, sizeof binst_full, "%s._hengetest._tcp.local", binst);
    GRec *bus3[3] = {zone_set(type_full, 12, 4500, binst_full, 0, NULL), zone_set(binst_full, 33, 120, mac_local, busy, NULL),
                     zone_set(binst_full, 16, 4500, NULL, 0, "")};
    announce(bus3, 3, 0);
    pump(3000);
    check("its port shows as taken", status_of(busy, NULL), EADDRINUSE);
    check("and it isn't announced", is_present(binst), 0);
    announce(bus3, 3, 1);
    for (int i = 0; i < 3; i++) zone_drop(bus3[i]);
    close(mine);

    /* 4. The Mac's goodbye withdraws the service; its announcement brings it back. */
    announce(&ptr, 1, 1);
    check("a goodbye withdraws it", wait_present(inst, 0, 5000), 1);
    announce(&ptr, 1, 0);
    check("an announcement brings it back", wait_present(inst, 1, 5000), 1);
    /* The name's goodbye: the name and what is on it go. */
    zone_silence(mac_local, 1);
    announce(&a, 1, 1);
    check("the Mac's name's goodbye withdraws the service", wait_present(inst, 0, 5000), 1);
    pump(300);
    status_name(nm, sizeof nm, NULL, NULL);
    check_str("and the name", nm, "");
    check("which no longer resolves", address_of(mac_local).done, 0);
    zone_silence(mac_local, 0);
    announce(&a, 1, 0);
    check("its announcement brings both back", wait_present(inst, 1, 5000), 1);
    /* A TXT too long for one packet (1326 bytes): left out, the service stays. */
    {
        char longtxt[1400] = "", piece[230];
        for (int i = 0; i < 6; i++) {
            snprintf(piece, sizeof piece, "%sk%d=", i ? ";" : "", i);
            strcat(longtxt, piece);
            memset(piece, 'x', 216), piece[216] = 0;
            strcat(longtxt, piece);
        }
        txt = zone_set(inst_full, 16, 4500, NULL, 0, longtxt);
        check("the long TXT is over 1300 bytes", txt->txt_len > 1300, 1);
        announce(&txt, 1, 0);
        int gone = 0;
        for (int t = 0; t < 30 && !gone; t++) {
            pump(100);
            r = resolve(inst, "_hengetest._tcp");
            gone = r.done && r.txt[0] == 0;
        }
        check("a TXT over 1300 bytes is left out", gone, 1);
        check("the service stays", is_present(inst), 1);
        txt = zone_set(inst_full, 16, 4500, NULL, 0, "path=/y");
        announce(&txt, 1, 0);
    }

    /* 5. Held: nothing on the network; after: asked again, everything back. */
    int before = asked("_services._dns-sd._udp.local", 12);
    hv_net_forwards_hold(1);
    check("held, the service is withdrawn", wait_present(inst, 0, 5000), 1);
    pump(200);
    status_name(nm, sizeof nm, NULL, &services);
    check_str("and the Mac has no name on the network", nm, "");
    hv_net_forwards_hold(0);
    check("after, it is back", wait_present(inst, 1, 8000), 1);
    check("and the Mac was asked again", asked("_services._dns-sd._udp.local", 12) > before, 1);
    r = resolve(inst, "_hengetest._tcp");
    check("at the same port", r.port, port);

    /* 5b. Held again at once, while the asks after a return are still due: nothing is due while held, so the
       network thread rests (measured from 15.5 s in, when every ask has come due). */
    hv_net_forwards_hold(1);
    pump(15500);
    struct rusage u0, u1;
    struct timespec w0, w1;
    getrusage(RUSAGE_SELF, &u0), clock_gettime(CLOCK_MONOTONIC, &w0);
    pump(3000);
    getrusage(RUSAGE_SELF, &u1), clock_gettime(CLOCK_MONOTONIC, &w1);
    double cpu = (double)(u1.ru_utime.tv_sec - u0.ru_utime.tv_sec + u1.ru_stime.tv_sec - u0.ru_stime.tv_sec) +
                 (double)(u1.ru_utime.tv_usec - u0.ru_utime.tv_usec + u1.ru_stime.tv_usec - u0.ru_stime.tv_usec) / 1e6;
    double wall = (double)(w1.tv_sec - w0.tv_sec) + (double)(w1.tv_nsec - w0.tv_nsec) / 1e9;
    printf("  held: this process used %.0f%% of a core\n", 100 * cpu / wall);
    check("held, the network thread rests (under a tenth of a core for the whole process; spinning, it was a fifth)", cpu < wall / 10, 1);
    hv_net_forwards_hold(0);
    check("and after, the service is back again", wait_present(inst, 1, 8000), 1);

    /* 6. A record that lives 10 s: asked for before it ends; when the Mac stops answering, withdrawn. (10 s, not 5:
       the asks fall at 80-95% of its life, and with a dozen test copies sharing the cores, 5 s left them under a
       second to be answered in.) */
    char sinst[64], sinst_full[160];
    snprintf(sinst, sizeof sinst, "Short %d", pid);
    snprintf(sinst_full, sizeof sinst_full, "%s._hengetest._tcp.local", sinst);
    uint16_t sport = free_port();
    GRec *sh[3] = {zone_set(type_full, 12, 10, sinst_full, 0, NULL), zone_set(sinst_full, 33, 10, mac_local, sport, NULL),
                   zone_set(sinst_full, 16, 10, NULL, 0, "s=1")};
    announce(sh, 3, 0);
    check("a short-lived service is announced", wait_present(sinst, 1, 5000), 1);
    int srv_asks = asked(sinst_full, 33);
    pump(13000); /* past its first life: had it not been asked again and answered, it would have ended */
    check("its SRV is asked for before it ends", asked(sinst_full, 33) > srv_asks, 1);
    check("answered, it stays", is_present(sinst), 1);
    zone_silence(sinst_full, 1);
    check("unanswered, it is withdrawn when it ends", wait_present(sinst, 0, 16000), 1);
    check("and the other stays", is_present(inst), 1);

    /* 7. The Mac's name, taken by another device: withdrawn, and Henge picks no name of its own; the Mac takes
       another as a Mac does ("<name>-2"), and that one is announced, the services on it. */
    char mac2_local[80], taken_full[90], mac3_local[80];
    snprintf(mac2_local, sizeof mac2_local, "renamemac-%d.local", pid);
    snprintf(taken_full, sizeof taken_full, "%s.", mac2_local);
    snprintf(mac3_local, sizeof mac3_local, "renamemac-%d-2.local", pid);
    DNSServiceRef other;
    DNSRecordRef other_rec;
    check("another device's connection", DNSServiceCreateConnection(&other), 0);
    add_ref(other);
    struct in_addr two = {htonl(0x7f000002)};
    check("another device takes the name",
          DNSServiceRegisterRecord(other, &other_rec, kDNSServiceFlagsUnique, kDNSServiceInterfaceIndexLocalOnly, taken_full,
                                   kDNSServiceType_A, kDNSServiceClass_IN, 4, &two, 120, reg_cb, NULL),
          0);
    pump(500);
    /* The Mac is renamed to that name (in its Sharing settings). */
    GRec *old[2] = {a, rev};
    announce(old, 2, 1);
    zone_drop(a), zone_drop(rev);
    a = zone_set(mac2_local, 1, 120, NULL, 0, NULL);
    srv = zone_set(inst_full, 33, 120, mac2_local, port, NULL);
    rev = zone_set("15.2.0.10.in-addr.arpa", 12, 120, mac2_local, 0, NULL);
    GRec *now[2] = {a, srv};
    int s7 = nsaid;
    announce(now, 2, 0);
    taken = 0;
    for (int t = 0; t < 100 && !taken; t++) {
        pump(100);
        status_name(nm, sizeof nm, &taken, NULL);
    }
    check("the status says another device has the name", taken, 1);
    Said x;
    check("the Mac is told the other device's address for its name (as any Mac on the network would hear it)",
          wait_said(s7, mac2_local, 1, NULL, 1, 5000), 1);
    said_find(s7, mac2_local, 1, NULL, 1, &x);
    check("127.0.0.2", x.a, htonl(0x7f000002));
    check("cache-flush, so the Mac knows it isn't its own", x.flush, 1);
    check_str("which it names", nm, mac2_local);
    check("one clash counted", (long long)hv_mdns_renames(), 1);
    check("the services are withdrawn", wait_present(inst, 0, 5000), 1);
    check_str("the name stays the other device's", address_of(mac2_local).ip, "127.0.0.2");
    pump(2000);
    check("Henge picks no name of its own", (long long)hv_mdns_renames(), 1);
    check("nor announces the service", is_present(inst), 0);
    check("on no other name", address_of(mac3_local).done, 0);
    /* The Mac takes "<name>-2", and announces it. */
    old[0] = a, old[1] = rev;
    announce(old, 1, 1);
    zone_drop(a), zone_drop(rev);
    a = zone_set(mac3_local, 1, 120, NULL, 0, NULL);
    srv = zone_set(inst_full, 33, 120, mac3_local, port, NULL);
    rev = zone_set("15.2.0.10.in-addr.arpa", 12, 120, mac3_local, 0, NULL);
    now[0] = a, now[1] = srv;
    announce(now, 2, 0);
    int got = 0;
    for (int t = 0; t < 100 && !got; t++) {
        pump(100);
        status_name(nm, sizeof nm, &taken, NULL);
        got = !strcmp(nm, mac3_local) && !taken;
    }
    check_str("the Mac's new name is announced", nm, mac3_local);
    check("and isn't taken", taken, 0);
    check("the service is back", wait_present(inst, 1, 5000), 1);
    r = resolve(inst, "_hengetest._tcp");
    snprintf(want_host, sizeof want_host, "%s.", mac3_local);
    check_str("on the new name", r.host, want_host);
    check_str("which is the device's address", address_of(mac3_local).ip, "127.0.0.1");
    pump(2000);
    status_name(nm, sizeof nm, &taken, NULL);
    check("and it stays: one clash in all", (long long)hv_mdns_renames(), 1);
    check_str("the name kept", nm, mac3_local);
    drop_ref(other);
    g_a = a, g_rev = rev;

    /* 8. The network's devices, told to the Mac (2b). A file server appears: the Mac hears of it unasked, with its
       SRV, TXT and address; asked, it is answered, with what goes with the answer, and so is the list of kinds;
       what the Mac already has isn't said again; asked QU, the answer comes to the Mac alone; a plain DNS client
       (not port 5353) gets its ID and question back, TTLs of at most 10 s, no cache-flush; Details lists it. */
    check("the network's side", DNSServiceCreateConnection(&lanc), 0);
    add_ref(lanc);
    char smbhost[80], smbinst[64], smbinst_full[120];
    snprintf(smbhost, sizeof smbhost, "smbhost-%d.local", pid);
    snprintf(smbinst, sizeof smbinst, "SMBBox-%d", pid);
    snprintf(smbinst_full, sizeof smbinst_full, "%s._smb._tcp.local", smbinst);
    DNSRecordRef smb_a;
    check("a file server's address", host_rec(smbhost, 0x7f000009, &smb_a), 0);
    int s8 = nsaid;
    DNSServiceRef smb = lan_service(smbinst, "_smb._tcp", smbhost, 4450, "\007model=X", 8);
    check("and its service", smb != NULL, 1);
    check("the Mac hears of it, unasked", wait_said(s8, "_smb._tcp.local", 12, smbinst_full, 1, 5000), 1);
    pump(300);
    check("its SRV, with its port", said_find(s8, smbinst_full, 33, smbhost, 1, &x) >= 1 && x.port == 4450, 1);
    check("its TXT", said_find(s8, smbinst_full, 16, NULL, 1, NULL) >= 1, 1);
    check("its host's address", said_find(s8, smbhost, 1, NULL, 1, &x) >= 1 && x.a == htonl(0x7f000009), 1);
    check("cache-flush on the address, as its own responder says it", x.flush, 1);
    said_find(s8, "_smb._tcp.local", 12, smbinst_full, 1, &x);
    check("not on the PTR (shared)", x.flush, 0);
    check("to every listener (multicast)", frame_of(x.frame).unicast, 0);
    int s = nsaid;
    guest_ask("_smb._tcp.local", 12, 0, 5353, 0, NULL);
    pump(300);
    check("the Mac asks: answered", said_find(s, "_smb._tcp.local", 12, smbinst_full, 1, NULL), 1);
    check("with its SRV", said_find(s, smbinst_full, 33, smbhost, 1, NULL), 1);
    check("its TXT", said_find(s, smbinst_full, 16, NULL, 1, NULL), 1);
    check("and the address", said_find(s, smbhost, 1, NULL, 1, NULL), 1);
    s = nsaid;
    guest_ask("_services._dns-sd._udp.local", 12, 0, 5353, 0, NULL);
    pump(300);
    check("the kinds of service there are", said_find(s, "_services._dns-sd._udp.local", 12, "_smb._tcp.local", 1, NULL), 1);
    GRec kn = {.used = 1, .type = 12, .ttl = 4500};
    snprintf(kn.name, sizeof kn.name, "_smb._tcp.local");
    snprintf(kn.target, sizeof kn.target, "%s", smbinst_full);
    s = nsaid;
    guest_ask("_smb._tcp.local", 12, 0, 5353, 0, &kn);
    pump(300);
    check("what the Mac has (known answer) isn't said again", said_find(s, "_smb._tcp.local", 12, smbinst_full, 1, NULL), 0);
    kn.ttl = 1000;
    s = nsaid;
    guest_ask("_smb._tcp.local", 12, 0, 5353, 0, &kn);
    pump(300);
    check("unless under half its TTL is left", said_find(s, "_smb._tcp.local", 12, smbinst_full, 1, NULL), 1);
    s = nsaid;
    guest_ask("_smb._tcp.local", 12, 1, 5353, 0, NULL);
    pump(300);
    check("asked QU: answered", said_find(s, "_smb._tcp.local", 12, smbinst_full, 1, &x), 1);
    SaidFrame fr = frame_of(x.frame);
    check("to the Mac alone, at 5353", fr.unicast && fr.dport == 5353, 1);
    s = nsaid;
    guest_ask("_smb._tcp.local", 12, 0, 5300, 0x4242, NULL);
    pump(300);
    check("a plain DNS client: answered", said_find(s, "_smb._tcp.local", 12, smbinst_full, 1, &x), 1);
    fr = frame_of(x.frame);
    check("at its own port", fr.unicast && fr.dport == 5300, 1);
    check("with its ID", fr.id, 0x4242);
    check("and its question", fr.nq == 1 && !strcasecmp(fr.q0, "_smb._tcp.local"), 1);
    int bad = 0;
    pthread_mutex_lock(&zone_mu);
    for (int i = s; i < nsaid; i++)
        if (said[i].frame == x.frame && (said[i].ttl > 10 || said[i].flush)) bad++;
    pthread_mutex_unlock(&zone_mu);
    check("TTLs of at most 10 s, no cache-flush", bad, 0);
    pump(1200); /* Details catches up each second */
    int seen = -1, dn = 0, un = 0;
    hv_mdns_status(nm, sizeof nm, NULL, NULL, &dn, &un, &seen);
    check("Details counts it", seen >= 1, 1);
    char list[2048];
    hv_mdns_seen(list, sizeof list);
    check("by its name", strstr(list, smbinst) != NULL, 1);

    /* 9. Held: nothing to the Mac, asked or not; after, the file server is told again, unasked. Its goodbye
       reaches the Mac. */
    hv_net_forwards_hold(1);
    pump(500);
    int f9 = nsframe;
    guest_ask("_smb._tcp.local", 12, 0, 5353, 0, NULL);
    pump(1500);
    check("held, nothing is said to the Mac", nsframe - f9, 0);
    s = nsaid;
    hv_net_forwards_hold(0);
    check("after, the file server is told again, unasked", wait_said(s, "_smb._tcp.local", 12, smbinst_full, 1, 8000), 1);
    s = nsaid;
    DNSServiceRefDeallocate(smb);
    check("its goodbye reaches the Mac", wait_said(s, "_smb._tcp.local", 12, smbinst_full, 0, 5000), 1);
    DNSServiceRemoveRecord(lanc, smb_a, 0);

    /* 10. Many: 30 web servers with 200-byte TXTs. Asked, all are answered, in as many messages as that takes,
       each within the network's MTU (1500). */
    char webhost[80], prefix[64], wi[64];
    snprintf(webhost, sizeof webhost, "webhost-%d.local", pid);
    snprintf(prefix, sizeof prefix, "Web-%d-", pid);
    DNSRecordRef web_a;
    check("the web servers' address", host_rec(webhost, 0x7f00000b, &web_a), 0);
    uint8_t wtxt[201];
    wtxt[0] = 200, memset(wtxt + 1, 'w', 200);
    DNSServiceRef web[30];
    s = nsaid;
    for (int i = 0; i < 30; i++) {
        snprintf(wi, sizeof wi, "%s%02d", prefix, i);
        web[i] = lan_service(wi, "_http._tcp", webhost, (uint16_t)(8000 + i), wtxt, sizeof wtxt);
    }
    int told = 0;
    for (int t = 0; t < 100 && told < 30; t++) pump(100), told = distinct_ptrs(s, "_http._tcp.local", prefix);
    check("30 web servers are told to the Mac", told, 30);
    s = nsaid;
    int f10 = nsframe;
    guest_ask("_http._tcp.local", 12, 0, 5353, 0, NULL);
    pump(500);
    check("asked, all 30 are answered", distinct_ptrs(s, "_http._tcp.local", prefix), 30);
    check("in more than one message", nsframe - f10 >= 2, 1);
    check("each within the network's MTU", frames_big, 0);
    for (int i = 0; i < 30; i++) DNSServiceRefDeallocate(web[i]);
    DNSServiceRemoveRecord(lanc, web_a, 0);
    pump(1000);

    /* 11. A name the Mac asks for that nothing offered: looked up, then said; once; 16 at a time. */
    char lookme[80], nb[80];
    snprintf(lookme, sizeof lookme, "lookme-%d.local", pid);
    DNSRecordRef look_a;
    check("a device with only a name", host_rec(lookme, 0x7f00000a, &look_a), 0);
    pump(200);
    uint64_t l0 = hv_mdns_lookups();
    s = nsaid;
    guest_ask(lookme, 1, 0, 5353, 0, NULL);
    pump(100);
    check("asked for, it is looked up", (long long)(hv_mdns_lookups() - l0), 1);
    pump(900);
    guest_ask(lookme, 1, 0, 5353, 0, NULL); /* the Mac asks again a second later */
    pump(300);
    check("and said to the Mac", said_find(s, lookme, 1, NULL, 1, &x) >= 1 && x.a == htonl(0x7f00000a), 1);
    check("looked up once", (long long)(hv_mdns_lookups() - l0), 1);
    for (int i = 0; i < 20; i++) {
        snprintf(nb, sizeof nb, "nobody%d-%d.local", i, pid);
        guest_ask(nb, 1, 0, 5353, 0, NULL);
    }
    pump(300);
    check("names no one has: 16 looked up at a time", (long long)(hv_mdns_lookups() - l0), 17);
    pump(3500);
    for (int i = 0; i < 20; i++) {
        snprintf(nb, sizeof nb, "nobody%d-%d.local", i, pid);
        guest_ask(nb, 1, 0, 5353, 0, NULL);
    }
    pump(300);
    check("asked again, the missed aren't looked up again; the other 4 are", (long long)(hv_mdns_lookups() - l0), 21);
    DNSServiceRemoveRecord(lanc, look_a, 0);

    /* 11b. A device the Mac asked for before it came: the lookup misses; the device then comes with a service
       (within the 10 s the miss is kept). Its address is looked up again and told to the Mac with the service. */
    char late[80], late_inst[64], late_full[120];
    snprintf(late, sizeof late, "late-%d.local", pid);
    snprintf(late_inst, sizeof late_inst, "Late-%d", pid);
    snprintf(late_full, sizeof late_full, "%s._smb._tcp.local", late_inst);
    pump(3500);
    uint64_t l11 = hv_mdns_lookups();
    guest_ask(late, 1, 0, 5353, 0, NULL);
    pump(3500);
    check("a name no one has yet: looked up, and missed", (long long)(hv_mdns_lookups() - l11), 1);
    DNSRecordRef late_a;
    check("then the device comes", host_rec(late, 0x7f00000c, &late_a), 0);
    s = nsaid;
    DNSServiceRef late_svc = lan_service(late_inst, "_smb._tcp", late, 4451, "", 0);
    check("with a service", late_svc != NULL, 1);
    check("the service is told to the Mac", wait_said(s, "_smb._tcp.local", 12, late_full, 1, 6000), 1);
    check("with the device's address", wait_said(s, late, 1, NULL, 1, 2000) && said_find(s, late, 1, NULL, 1, &x) &&
                                           x.a == htonl(0x7f00000c), 1);
    pump(8000); /* past the miss's 10 s: the service keeps its device */
    s = nsaid;
    guest_ask(late, 1, 0, 5353, 0, NULL);
    pump(300);
    check("and the address is still the Mac's to ask for", said_find(s, late, 1, NULL, 1, NULL) >= 1, 1);
    DNSServiceRefDeallocate(late_svc);
    DNSServiceRemoveRecord(lanc, late_a, 0);

    /* 12. Never the Mac's own (amendment 4): its own service, of a kind Henge looks for (_ssh), is found on the
       device's network, and a device on the device's own address; neither is said to the Mac. */
    char gssh[64], gssh_full[120], self_host[80], self_inst[64];
    snprintf(gssh, sizeof gssh, "GuestSSH-%d", pid);
    snprintf(gssh_full, sizeof gssh_full, "%s._ssh._tcp.local", gssh);
    snprintf(self_host, sizeof self_host, "ipadself-%d.local", pid);
    snprintf(self_inst, sizeof self_inst, "Self-%d", pid);
    uint16_t sshport = free_port();
    GRec *ssh3[4] = {zone_set("_services._dns-sd._udp.local", 12, 4500, "_ssh._tcp.local", 0, NULL),
                     zone_set("_ssh._tcp.local", 12, 4500, gssh_full, 0, NULL), zone_set(gssh_full, 33, 120, mac3_local, sshport, NULL),
                     zone_set(gssh_full, 16, 4500, NULL, 0, "")};
    announce(ssh3 + 1, 3, 0);
    int reg = 0;
    for (int t = 0; t < 30 && !reg; t++) pump(100), reg = resolve(gssh, "_ssh._tcp").done;
    check("the Mac's ssh is on the device's network", reg, 1);
    DNSRecordRef self_a;
    check("a device on the device's own address", host_rec(self_host, 0x7f000001, &self_a), 0);
    DNSServiceRef self = lan_service(self_inst, "_http._tcp", self_host, 81, "", 0);
    pump(3000);
    guest_ask("_ssh._tcp.local", 12, 0, 5353, 0, NULL);
    guest_ask("_http._tcp.local", 12, 0, 5353, 0, NULL);
    guest_ask(self_host, 1, 0, 5353, 0, NULL);
    guest_ask(mac3_local, 1, 0, 5353, 0, NULL);
    pump(1500);
    check("the Mac isn't told of its own service", said_of(gssh, 0), 0);
    check("nor of the device on the device's address", said_of(self_inst, 0) + said_of(self_host, 0), 0);
    DNSServiceRefDeallocate(self);
    DNSServiceRemoveRecord(lanc, self_a, 0);
    announce(ssh3 + 1, 3, 1);
    for (int i = 0; i < 4; i++) zone_drop(ssh3[i]);

    /* 12b. The Mac's service has the name of another device's: Henge doesn't rename it on the network (the Mac
       names its own); it is withdrawn, and the other device's service is told to the Mac, as a MacBook would hear
       it, so the Mac renames its own ("<name> (2)"); that one is announced. */
    char cl_host[80], cl_inst[64], cl_full[120], cl2_inst[80], cl2_full[140];
    snprintf(cl_host, sizeof cl_host, "clashhost-%d.local", pid);
    snprintf(cl_inst, sizeof cl_inst, "Clash-%d", pid);
    snprintf(cl_full, sizeof cl_full, "%s._smb._tcp.local", cl_inst);
    snprintf(cl2_inst, sizeof cl2_inst, "%s (2)", cl_inst);
    snprintf(cl2_full, sizeof cl2_full, "%s._smb._tcp.local", cl2_inst);
    DNSServiceRef smb_browse;
    check("browsing file servers", DNSServiceBrowse(&smb_browse, 0, kDNSServiceInterfaceIndexLocalOnly, "_smb._tcp", "local.", browsed, NULL), 0);
    add_ref(smb_browse);
    DNSRecordRef cl_a;
    check("another device's address", host_rec(cl_host, 0x7f00000d, &cl_a), 0);
    DNSServiceRef cl_svc = lan_service(cl_inst, "_smb._tcp", cl_host, 4452, "", 0);
    check("and its file server", cl_svc != NULL, 1);
    check("on the network", wait_present(cl_inst, 1, 5000), 1);
    pump(1000);
    uint16_t smbport = free_port();
    GRec *cl3[4] = {zone_set("_services._dns-sd._udp.local", 12, 4500, "_smb._tcp.local", 0, NULL),
                    zone_set("_smb._tcp.local", 12, 4500, cl_full, 0, NULL), zone_set(cl_full, 33, 120, mac3_local, smbport, NULL),
                    zone_set(cl_full, 16, 4500, NULL, 0, "")};
    s = nsaid;
    announce(cl3 + 1, 3, 0);
    check("the Mac is told the other device's service of that name",
          wait_said(s, cl_full, 33, cl_host, 1, 8000), 1);
    pump(3000);
    check("Henge gives the Mac's no other name", is_present(cl2_inst), 0);
    check("the name stays the other device's", resolve(cl_inst, "_smb._tcp").port, 4452);
    /* The Mac hasn't renamed its own in 10 s (the other's not heard, say): announced as "(2)", as Bonjour renames.
       The other device's is still told to the Mac under the name; Henge's "(2)" isn't told to it as another's. */
    check("not renamed by the Mac: announced as Bonjour renames it", wait_present(cl2_inst, 1, 10000), 1);
    check("at the Mac's port", resolve(cl2_inst, "_smb._tcp").port, smbport);
    s = nsaid;
    guest_ask("_smb._tcp.local", 12, 0, 5353, 0, NULL);
    pump(1500);
    check("the other device's is still told to the Mac", said_find(s, cl_full, 33, cl_host, 1, NULL) > 0, 1);
    check("Henge's \"(2)\" isn't told to it as another device's", said_find(s, cl2_full, 33, NULL, 1, NULL), 0);
    /* The Mac renames its own late, as "(2)" (here at another port, to tell it from Henge's): the new name, then the
       old one's goodbye. The name is the Mac's: Henge's "(2)" moves to "(3)" and the Mac's isn't renamed. */
    char cl3_inst[80], cl22_inst[90];
    snprintf(cl3_inst, sizeof cl3_inst, "%s (3)", cl_inst);
    snprintf(cl22_inst, sizeof cl22_inst, "%s (2)", cl2_inst);
    uint16_t smbport2 = free_port();
    GRec *cl3b[3] = {zone_set("_smb._tcp.local", 12, 4500, cl2_full, 0, NULL), zone_set(cl2_full, 33, 120, mac3_local, smbport2, NULL),
                     zone_set(cl2_full, 16, 4500, NULL, 0, "")};
    announce(cl3b, 3, 0);
    int cl_ok = 0;
    for (int t = 0; t < 80 && !cl_ok; t++) pump(100), cl_ok = resolve(cl2_inst, "_smb._tcp").port == smbport2;
    check("the Mac's new name is announced as it is", cl_ok, 1);
    check("Henge's moves on, at the old one's port", wait_present(cl3_inst, 1, 5000) && resolve(cl3_inst, "_smb._tcp").port == smbport, 1);
    check("the Mac's own isn't renamed", is_present(cl22_inst), 0);
    announce(cl3 + 1, 3, 1);
    zone_drop(cl3[1]), zone_drop(cl3[2]), zone_drop(cl3[3]);
    check("the old one's goodbye withdraws Henge's", wait_present(cl3_inst, 0, 5000), 1);
    check("the Mac's stays", resolve(cl2_inst, "_smb._tcp").port, smbport2);
    check("the other device's is untouched", resolve(cl_inst, "_smb._tcp").port, 4452);
    announce(cl3b, 3, 1);
    for (int i = 0; i < 3; i++) zone_drop(cl3b[i]);
    zone_drop(cl3[0]);

    /* 12c. The same, of a kind Henge doesn't look for on the network (so the Mac can't be told of the other, and
       won't rename its own): it is announced at once as Bonjour would rename it: "(2)", here taken too, so "(3)".
       Then the Mac announces another service as "(3)": the name is the Mac's, and Henge's "(3)" moves to "(4)". */
    char un_inst[64], un_full[120], un2_inst[80], un3_inst[80], un3_full[140], un4_inst[80], un33_inst[90];
    snprintf(un_inst, sizeof un_inst, "Unseen-%d", pid);
    snprintf(un_full, sizeof un_full, "%s._hengetest._tcp.local", un_inst);
    snprintf(un2_inst, sizeof un2_inst, "%s (2)", un_inst);
    snprintf(un3_inst, sizeof un3_inst, "%s (3)", un_inst);
    snprintf(un3_full, sizeof un3_full, "%s._hengetest._tcp.local", un3_inst);
    snprintf(un4_inst, sizeof un4_inst, "%s (4)", un_inst);
    snprintf(un33_inst, sizeof un33_inst, "%s (2)", un3_inst);
    DNSServiceRef un_svc = lan_service(un_inst, "_hengetest._tcp", cl_host, 4453, "", 0);
    DNSServiceRef un_svc2 = lan_service(un2_inst, "_hengetest._tcp", cl_host, 4454, "", 0);
    check("another device's services of a kind Henge doesn't look for", wait_present(un_inst, 1, 5000) && wait_present(un2_inst, 1, 5000), 1);
    uint16_t unport = free_port();
    static GRec un3[3];
    un3[0] = (GRec){.used = 1, .type = 12, .ttl = 4500}, un3[1] = (GRec){.used = 1, .type = 33, .ttl = 120, .port = unport};
    un3[2] = (GRec){.used = 1, .type = 16, .ttl = 4500};
    snprintf(un3[0].name, sizeof un3[0].name, "%s", type_full), snprintf(un3[0].target, sizeof un3[0].target, "%s", un_full);
    snprintf(un3[1].name, sizeof un3[1].name, "%s", un_full), snprintf(un3[1].target, sizeof un3[1].target, "%s", mac3_local);
    snprintf(un3[2].name, sizeof un3[2].name, "%s", un_full);
    GRec *un3p[3] = {&un3[0], &un3[1], &un3[2]};
    announce(un3p, 3, 0);
    check("the Mac's of that name is announced at once as Bonjour renames it", wait_present(un3_inst, 1, 5000), 1);
    check("at the Mac's port", resolve(un3_inst, "_hengetest._tcp").port, unport);
    check("the other device's are untouched", resolve(un_inst, "_hengetest._tcp").port == 4453 && resolve(un2_inst, "_hengetest._tcp").port == 4454, 1);
    uint16_t unport2 = free_port();
    GRec *un3x[3] = {zone_set(type_full, 12, 4500, un3_full, 0, NULL), zone_set(un3_full, 33, 120, mac3_local, unport2, NULL),
                     zone_set(un3_full, 16, 4500, NULL, 0, "")};
    announce(un3x, 3, 0);
    int un_ok = 0;
    for (int t = 0; t < 60 && !un_ok; t++) pump(100), un_ok = resolve(un3_inst, "_hengetest._tcp").port == unport2;
    check("the Mac's own \"(3)\" is announced as it is", un_ok, 1);
    check("Henge's moves on", wait_present(un4_inst, 1, 5000), 1);
    check("to the next, at its port", resolve(un4_inst, "_hengetest._tcp").port, unport);
    check("the Mac's own isn't renamed", is_present(un33_inst), 0);
    announce(un3x, 3, 1);
    for (int i = 0; i < 3; i++) zone_drop(un3x[i]);
    check("the Mac's goodbye withdraws its own", wait_present(un3_inst, 0, 5000), 1);
    announce(un3p, 3, 1);
    check("and the other", wait_present(un4_inst, 0, 5000), 1);
    DNSServiceRefDeallocate(un_svc), DNSServiceRefDeallocate(un_svc2);

    /* 12e. A conflict Bonjour finds later, on the network, for a name Henge chose ("(2)", the Mac's own taken at
       once): Henge moves on to the next ("(3)"), and the "(2)" is withdrawn. */
    char mv_inst[64], mv_full[120], mv2_inst[80], mv3_inst[80], poke_inst[64];
    snprintf(mv_inst, sizeof mv_inst, "Moved-%d", pid);
    snprintf(mv_full, sizeof mv_full, "%s._hengetest._tcp.local", mv_inst);
    snprintf(mv2_inst, sizeof mv2_inst, "%s (2)", mv_inst);
    snprintf(mv3_inst, sizeof mv3_inst, "%s (3)", mv_inst);
    snprintf(poke_inst, sizeof poke_inst, "Poke-%d", pid);
    pthread_mutex_lock(&late_mu);
    snprintf(late_reg.name, sizeof late_reg.name, "%s", mv2_inst);
    pthread_mutex_unlock(&late_mu);
    DNSServiceRef mv_svc = lan_service(mv_inst, "_hengetest._tcp", cl_host, 4455, "", 0);
    check("another device's service", wait_present(mv_inst, 1, 5000), 1);
    uint16_t mvport = free_port();
    GRec *mv[3] = {zone_set(type_full, 12, 4500, mv_full, 0, NULL), zone_set(mv_full, 33, 120, mac3_local, mvport, NULL),
                   zone_set(mv_full, 16, 4500, NULL, 0, "")};
    announce(mv, 3, 0);
    check("the Mac's of that name is announced as \"(2)\"", wait_present(mv2_inst, 1, 5000), 1);
    pthread_mutex_lock(&late_mu);
    int held = late_reg.cb != NULL;
    late_reg.due = 1;
    pthread_mutex_unlock(&late_mu);
    check("(Henge's registration of it is seen)", held, 1);
    /* Bonjour tells Henge's thread something (a file server comes and goes), and with it, the conflict. */
    for (int t = 0; t < 20 && !late_told; t++) {
        DNSServiceRef poke = lan_service(poke_inst, "_smb._tcp", cl_host, 4456, "", 0);
        pump(250);
        if (poke) DNSServiceRefDeallocate(poke);
        pump(250);
    }
    check("(the conflict is told to Henge)", late_told, 1);
    check("Henge's moves on to the next name", wait_present(mv3_inst, 1, 5000), 1);
    check("at the Mac's port", resolve(mv3_inst, "_hengetest._tcp").port, mvport);
    check("and the taken \"(2)\" is withdrawn", wait_present(mv2_inst, 0, 5000), 1);
    check("the other device's is untouched", resolve(mv_inst, "_hengetest._tcp").port, 4455);
    announce(mv, 3, 1);
    for (int i = 0; i < 3; i++) zone_drop(mv[i]);
    check("the Mac's goodbye withdraws it", wait_present(mv3_inst, 0, 5000), 1);
    DNSServiceRefDeallocate(mv_svc);
    pthread_mutex_lock(&late_mu);
    late_reg.name[0] = 0;
    pthread_mutex_unlock(&late_mu);
    check("every service Henge announced, it announced under that name only (Bonjour never renames the Mac's)",
          henge_regs > 0 && henge_regs_renamable == 0, 1);

    /* 12d. The other device leaves; the Mac takes the name back (announced: the Mac's again, no longer the other's),
       then drops it. The other's service, about again under that name, isn't told to the Mac as another device's. */
    DNSServiceRefDeallocate(cl_svc);
    check("the other device leaves", wait_present(cl_inst, 0, 5000), 1);
    GRec *cl3c[3] = {zone_set("_smb._tcp.local", 12, 4500, cl_full, 0, NULL), zone_set(cl_full, 33, 120, mac3_local, smbport, NULL),
                     zone_set(cl_full, 16, 4500, NULL, 0, "")};
    announce(cl3c, 3, 0);
    check("the Mac takes the name back", wait_present(cl_inst, 1, 8000), 1);
    check("its own", resolve(cl_inst, "_smb._tcp").port, smbport);
    announce(cl3c, 3, 1);
    for (int i = 0; i < 3; i++) zone_drop(cl3c[i]);
    check("then drops it", wait_present(cl_inst, 0, 5000), 1);
    s = nsaid;
    cl_svc = lan_service(cl_inst, "_smb._tcp", cl_host, 4452, "", 0);
    check("the name about again", wait_present(cl_inst, 1, 5000), 1);
    pump(1500);
    guest_ask("_smb._tcp.local", 12, 0, 5353, 0, NULL);
    pump(1000);
    check("not told to the Mac as another device's", said_find(s, cl_full, 33, NULL, 1, NULL), 0);
    DNSServiceRefDeallocate(cl_svc);
    DNSServiceRemoveRecord(lanc, cl_a, 0);
    drop_ref(smb_browse);

    /* 13. The Mac renamed while the old name is still about (another device's cache, or the device's old
       address): not said to the Mac as another device's, so it doesn't rename itself; back to it, no clash. */
    char stale_local[80];
    snprintf(stale_local, sizeof stale_local, "stalemac-%d.local", pid);
    guest_rename(stale_local);
    got = 0;
    for (int t = 0; t < 100 && !got; t++) pump(100), status_name(nm, sizeof nm, &taken, NULL), got = !strcmp(nm, stale_local);
    check_str("renamed", nm, stale_local);
    DNSRecordRef old_a;
    check("its old name, still about at another address", host_rec(mac3_local, 0x7f000007, &old_a), 0);
    pump(300);
    uint64_t l13 = hv_mdns_lookups();
    s = nsaid;
    guest_ask(mac3_local, 1, 0, 5353, 0, NULL);
    pump(1000);
    guest_ask(mac3_local, 1, 0, 5353, 0, NULL);
    pump(1000);
    check("the Mac's old name isn't said to it", said_find(s, mac3_local, 1, NULL, 1, NULL), 0);
    check("nor looked up", (long long)(hv_mdns_lookups() - l13), 0);
    DNSServiceRemoveRecord(lanc, old_a, 0);
    pump(300);
    guest_rename(mac3_local);
    got = 0;
    for (int t = 0; t < 100 && !got; t++) pump(100), status_name(nm, sizeof nm, &taken, NULL), got = !strcmp(nm, mac3_local);
    check_str("back to its old name", nm, mac3_local);
    check("not taken", taken, 0);
    check("no clash", (long long)hv_mdns_renames(), 1);

    /* 13b. Many service names since (80, more than the old guard's 64): the Mac's old name is still known as its
       own, not said to it as another device's. */
    static GRec churn[20][3];
    int base = 0, many = 0;
    status_name(nm, sizeof nm, NULL, &base);
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < 20; i++) {
            GRec *c = churn[i];
            c[0] = (GRec){.used = 1, .type = 12, .ttl = 4500};
            snprintf(c[0].name, sizeof c[0].name, "%s", type_full);
            snprintf(c[0].target, sizeof c[0].target, "Churn %d-%d %d._hengetest._tcp.local", round, i, pid);
            c[1] = (GRec){.used = 1, .type = 33, .ttl = 120, .port = port};
            snprintf(c[1].name, sizeof c[1].name, "%s", c[0].target);
            snprintf(c[1].target, sizeof c[1].target, "%s", mac3_local);
            c[2] = (GRec){.used = 1, .type = 16, .ttl = 4500};
            snprintf(c[2].name, sizeof c[2].name, "%s", c[0].target);
            GRec *one[3] = {&c[0], &c[1], &c[2]};
            announce(one, 3, 0);
        }
        int up = 0;
        for (int t = 0; t < 100 && up < base + 20; t++) pump(100), status_name(nm, sizeof nm, NULL, &up);
        many += up - base;
        for (int i = 0; i < 20; i++) {
            GRec *one[3] = {&churn[i][0], &churn[i][1], &churn[i][2]};
            announce(one, 3, 1);
        }
        for (int t = 0; t < 100 && up > base; t++) pump(100), status_name(nm, sizeof nm, NULL, &up);
    }
    check("80 of the Mac's services came and went", many, 80);
    check("its old name, about again at another address", host_rec(stale_local, 0x7f000008, &old_a), 0);
    pump(300);
    l13 = hv_mdns_lookups();
    s = nsaid;
    guest_ask(stale_local, 1, 0, 5353, 0, NULL);
    pump(1000);
    guest_ask(stale_local, 1, 0, 5353, 0, NULL);
    pump(1000);
    check("still not said to the Mac", said_find(s, stale_local, 1, NULL, 1, NULL), 0);
    check("nor looked up", (long long)(hv_mdns_lookups() - l13), 0);
    DNSServiceRemoveRecord(lanc, old_a, 0);

    /* 13c. The name another device had (7), now free: the Mac takes it back (announced: the Mac's again, no longer
       the other's), then another. The name, about again, isn't said to the Mac as another device's. */
    guest_rename(mac2_local);
    got = 0;
    for (int t = 0; t < 100 && !got; t++) pump(100), status_name(nm, sizeof nm, &taken, NULL), got = !strcmp(nm, mac2_local) && !taken;
    check_str("the Mac takes back the name another device had", nm, mac2_local);
    check("not taken now", taken, 0);
    guest_rename(mac3_local);
    got = 0;
    for (int t = 0; t < 100 && !got; t++) pump(100), status_name(nm, sizeof nm, &taken, NULL), got = !strcmp(nm, mac3_local);
    check_str("then another", nm, mac3_local);
    check("the name, about again", host_rec(mac2_local, 0x7f000007, &old_a), 0);
    pump(300);
    l13 = hv_mdns_lookups();
    s = nsaid;
    guest_ask(mac2_local, 1, 0, 5353, 0, NULL);
    pump(1000);
    guest_ask(mac2_local, 1, 0, 5353, 0, NULL);
    pump(1000);
    check("isn't said to the Mac as another device's", said_find(s, mac2_local, 1, NULL, 1, NULL), 0);
    check("nor looked up", (long long)(hv_mdns_lookups() - l13), 0);
    check("no clash in all this", (long long)hv_mdns_renames(), 1);
    DNSServiceRemoveRecord(lanc, old_a, 0);

    /* 14. The device's Bonjour goes away (as when iOS restarts it) after the Mac's name and an ssh service of its were
       up for longer than the guard keeps a name (2 x 4500 s, passed at once here). While it's away the Mac renames
       and withdraws the service, so neither is announced again when it's back; both stay in other devices' caches
       for up to 4500 s. Neither is said to the Mac as another device's, so it doesn't rename its own. */
    char g14[64], g14_full[120], new14[80], other14[80];
    snprintf(g14, sizeof g14, "GuestSSH14-%d", pid);
    snprintf(g14_full, sizeof g14_full, "%s._ssh._tcp.local", g14);
    snprintf(new14, sizeof new14, "newmac14-%d.local", pid);
    snprintf(other14, sizeof other14, "other14-%d.local", pid);
    GRec *ssh14[4] = {zone_set("_services._dns-sd._udp.local", 12, 4500, "_ssh._tcp.local", 0, NULL),
                      zone_set("_ssh._tcp.local", 12, 4500, g14_full, 0, NULL), zone_set(g14_full, 33, 120, mac3_local, sshport, NULL),
                      zone_set(g14_full, 16, 4500, NULL, 0, "")};
    announce(ssh14 + 1, 3, 0);
    reg = 0;
    for (int t = 0; t < 30 && !reg; t++) pump(100), reg = resolve(g14, "_ssh._tcp").done;
    check("the Mac's ssh is on the device's network", reg, 1);
    hv_mdns_test_away(2 * 4500 * 1000LL + 60000);
    check("Bonjour away", net_ticks(2), 1);
    announce(ssh14 + 1, 3, 1); /* its goodbye, and the rename, while Bonjour is away */
    for (int i = 1; i < 4; i++) zone_drop(ssh14[i]);
    guest_rename(new14);
    pump(100);
    check("both taken while it's away", net_ticks(2), 1);
    hv_mdns_test_back();
    got = 0;
    for (int t = 0; t < 150 && !got; t++) pump(100), status_name(nm, sizeof nm, &taken, NULL), got = !strcmp(nm, new14);
    check_str("Bonjour back: the Mac is announced by its new name", nm, new14);
    pump(1000);
    check("its ssh isn't announced again", resolve(g14, "_ssh._tcp").done, 0);
    DNSRecordRef old14, oth14;
    check("its old name, still about at another address", host_rec(mac3_local, 0x7f00000a, &old14), 0);
    check("its service's name, on another device", host_rec(other14, 0x7f00000b, &oth14), 0);
    DNSServiceRef s14 = lan_service(g14, "_ssh._tcp", other14, 22, "", 0);
    check("(registered)", s14 != NULL, 1);
    /* And one of that device's that the Mac never had: it is told to the Mac, so the network was heard. */
    char ctl14[64], ctl14_full[120];
    snprintf(ctl14, sizeof ctl14, "Other14-%d", pid);
    snprintf(ctl14_full, sizeof ctl14_full, "%s._ssh._tcp.local", ctl14);
    DNSServiceRef c14 = lan_service(ctl14, "_ssh._tcp", other14, 22, "", 0);
    check("(registered)", c14 != NULL, 1);
    s = nsaid;
    got = 0;
    for (int k = 0; k < 10 && !got; k++) {
        guest_ask("_ssh._tcp.local", 12, 0, 5353, 0, NULL);
        for (int t = 0; t < 15 && !got; t++) pump(100), got = said_find(s, ctl14_full, 33, other14, 1, NULL) > 0;
    }
    check("the Mac is told the other device's other ssh", got, 1);
    guest_ask("_ssh._tcp.local", 12, 0, 5353, 0, NULL);
    pump(1500);
    check("not its old service, as another device's", said_of(g14, 0), 0);
    l13 = hv_mdns_lookups();
    s = nsaid;
    for (int k = 0; k < 2; k++) {
        guest_ask(mac3_local, 1, 0, 5353, 0, NULL);
        pump(1500);
    }
    check("(its questions taken)", net_ticks(2), 1);
    check("the Mac's old name isn't said to it", said_find(s, mac3_local, 1, NULL, 1, NULL), 0);
    check("nor looked up", (long long)(hv_mdns_lookups() - l13), 0);
    check("no clash", (long long)hv_mdns_renames(), 1);
    if (s14) DNSServiceRefDeallocate(s14);
    if (c14) DNSServiceRefDeallocate(c14);
    DNSServiceRemoveRecord(lanc, old14, 0);
    DNSServiceRemoveRecord(lanc, oth14, 0);

    /* Over the whole run: nothing of the Mac's own said to it (its names, its service, the device's address). */
    check("the device's address is never said to the Mac", said_of(NULL, 0x7f000001), 0);
    check("nor its names", said_of(mac_local, 0) + said_of(mac3_local, 0) + said_of(stale_local, 0) + said_of(new14, 0), 0);
    check("nor its services", said_of(inst, 0), 0);
    check("every frame within the MTU", frames_big, 0);
    check("every frame well formed", frames_bad, 0);

    drop_ref(lanc);
    drop_ref(browse);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
