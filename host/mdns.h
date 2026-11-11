#ifndef HENGEVM_MDNS_H
#define HENGEVM_MDNS_H

/* The Mac's name and services on the network, and the network's devices in the Mac, as a MacBook's (mdns.c;
   design: the port forwarding hostfwd notes, 5a and 5b). The Mac's own Bonjour traffic is
   read off its network card, and what it says about itself is registered on the device's network under the same
   name, pointing at the device's address; what the device's Bonjour finds there is told to the Mac. */

#include <stddef.h>
#include <stdint.h>

/* ---- the network thread (net_slirp.c) ---- */

/* A frame for the guest (from the gateway, 10.0.2.2). Frames stay within the network's MTU. */
typedef void (*HvMdnsDeliver)(const uint8_t *frame, uint32_t len);
void hv_mdns_start(HvMdnsDeliver deliver, int mtu);
/* A frame the guest sent: 1 if it is mDNS (IPv4 UDP to port 5353, to 224.0.0.251 or 10.0.2.2), which is mdns.c's
   and never goes to libslirp; 0 otherwise. */
int hv_mdns_take(const uint8_t *frame, uint32_t len);
/* dns_sd's socket, for the poll set (-1: none now), and its turn when it is readable. */
int hv_mdns_fd(void);
void hv_mdns_ready(void);
/* Each pass of the loop: what is due (refresh queries to the Mac, expiry, registration changes). Returns the ms
   until it next has something to do. */
int64_t hv_mdns_tick(int64_t now_ms);

/* ---- any thread ---- */

/* The name the Mac has on the network ("My-Mac.local"; "" while none); whether another device on the network
   has it, so it is withdrawn until the Mac takes another; how many services are announced; whether iOS refused the
   name (Henge's Local Network access is off); and how many services iOS refused alone (their type isn't one Henge
   declares in Info.plist; an app can announce only those); how many of the network's devices the Mac is told of. */
void hv_mdns_status(char *name, size_t n, int *taken, int *services, int *denied, int *unlisted, int *seen);
/* Those devices' names, one a line ("Sam's MacBook Pro\nOffice Printer\n"); returns how many. */
int hv_mdns_seen(char *list, size_t n);
/* How many times the Mac's name was found taken on the network, since Henge started. */
uint64_t hv_mdns_renames(void);

/* For the host's tests: register and look on this Mac only (kDNSServiceInterfaceIndexLocalOnly, the name at
   127.0.0.1), so nothing leaves it. Before hv_mdns_start. */
void hv_mdns_local_only(int on);
/* How many `.local` names the Mac asked for were looked up on the network (at most 16 at a time), since start. */
uint64_t hv_mdns_lookups(void);
/* For the host's tests: as if Bonjour on the device went away (as hv_mdns_ready finds it), with the guard's time (a
   name Henge put on the network is kept 2 x 4500 s) moved on by pass_ms first, so names marked before are as old as
   that. It stays away until hv_mdns_test_back. Each is done at the network thread's next tick (hv_mdns_tick), and
   hv_mdns_test_ticks counts the ticks begun: once it is 2 past a reading, what was asked before it is done, and the
   guest's frames handed over before it are taken. */
void hv_mdns_test_away(int64_t pass_ms);
void hv_mdns_test_back(void);
uint64_t hv_mdns_test_ticks(void);
/* For the fuzz test (no dns_sd): the network's devices, as if found: one service on a host at an address
   (network order). "Office._ipp._tcp.local", "office.local". */
void hv_mdns_test_found(const char *instance, const char *host, uint32_t addr, uint16_t port, const uint8_t *txt, uint16_t txt_len);

/* ---- the wire (RFC 1035, RFC 6762), strict: what the guest wrote ---- */

#define HV_DNS_MAX_Q 64
#define HV_DNS_MAX_RR 256
#define HV_DNS_MAX_MSG 9000

/* A name in wire form, uncompressed: labels, each its length first, and the final 0. At most 255 bytes. */
typedef struct HvDnsName {
    uint8_t b[256];
    uint16_t len;
} HvDnsName;

typedef struct HvDnsRR {
    HvDnsName name;
    uint16_t type, cls; /* cls without its top bit */
    uint8_t top;        /* the class's top bit: cache-flush (a record) or unicast-response (a question) */
    uint32_t ttl;
    uint16_t rdlen;
    const uint8_t *rdata; /* in the message */
    HvDnsName target;     /* PTR, SRV */
    uint16_t port;        /* SRV */
    uint32_t a;           /* A, network order */
} HvDnsRR;

typedef struct HvDnsMsg {
    uint16_t id, flags;
    int nq, nrr, nan; /* nrr: answers, authority and additional; the first nan of them are the answers */
    HvDnsRR q[HV_DNS_MAX_Q];
    HvDnsRR rr[HV_DNS_MAX_RR];
} HvDnsMsg;

/* 0, or -1: the message is dropped whole (a read past its end, a name too long or a pointer not backwards, more
   records than it can hold, a known record's data that doesn't parse). */
int hv_dns_parse(const uint8_t *msg, size_t len, HvDnsMsg *out);
/* Equal, ASCII letters in either case. */
int hv_dns_name_eq(const HvDnsName *a, const HvDnsName *b);
/* "Sam\'s Mac._ssh._tcp.local." with RFC 6763 4.3's escapes (a dot or backslash in a label, a byte below 0x20).
   0, or -1 if it doesn't fit. */
int hv_dns_name_text(const HvDnsName *n, char *out, size_t cap);
/* A name from plain dotted text without escapes ("_services._dns-sd._udp.local"). 0, or -1. */
int hv_dns_name_from(HvDnsName *n, const char *text);
/* A name from dns_sd's text, with RFC 6763 4.3's escapes ("My\.Mac._smb._tcp.local.", "a\032b").
   0, or -1 (a label over 63 bytes or empty, a name over 255, a bad escape). */
int hv_dns_name_parse(HvDnsName *n, const char *text);

#endif
