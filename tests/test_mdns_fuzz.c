/* mdns.c's reading of what the guest writes (hv_dns_parse, hv_mdns_take): the guest is not trusted, so a message
   that breaks any rule is dropped whole, and no message, however made, reads outside itself (built with ASan and
   UBSan; each message is in a heap buffer of exactly its length). Checked: a real announcement parses, with its
   compressed names; a pointer forwards or to itself, a name past 255 bytes, a label type mDNS doesn't have, a
   count that can't fit, a record's data that doesn't parse, are refused; names print with RFC 6763's escapes;
   then random and mutated messages and frames, CPU only. 5b: escaped names read back exactly what printed them,
   and malformed ones are refused; with 100 of the network's devices known (a TXT of 1300 bytes among them), the
   Mac's questions, built and mutated, from its responder and from plain DNS clients, get answers that each fit
   one 1500-byte frame and parse. */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mdns.h"
#include "net_forward.h"

static int checks, failures;
static void check(const char *what, long long got, long long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got %lld, want %lld\n", what, got, want);
    }
}

static uint8_t *put_name(uint8_t *p, const char *dotted) {
    HvDnsName n;
    hv_dns_name_from(&n, dotted);
    memcpy(p, n.b, n.len);
    return p + n.len;
}
static uint8_t *put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; return p + 2; }
static uint8_t *put32(uint8_t *p, uint32_t v) { return put16(put16(p, (uint16_t)(v >> 16)), (uint16_t)v); }

/* What a Mac announces: its A, a service's PTR, SRV and TXT (names compressed against the first), and its type. */
static size_t announcement(uint8_t *m) {
    memset(m, 0, 12);
    put16(m + 2, 0x8400), put16(m + 6, 5);
    uint8_t *p = m + 12;
    size_t host = (size_t)(p - m);
    p = put_name(p, "testmac.local");
    p = put16(p, 1), p = put16(p, 0x8001), p = put32(p, 120), p = put16(p, 4);
    *p++ = 10, *p++ = 0, *p++ = 2, *p++ = 15;
    size_t type = (size_t)(p - m);
    p = put_name(p, "_ssh._tcp.local");
    p = put16(p, 12), p = put16(p, 1), p = put32(p, 4500);
    uint8_t *rdlen = p;
    p += 2;
    size_t inst = (size_t)(p - m);
    *p++ = 8, memcpy(p, "Test Mac", 8), p += 8;
    *p++ = (uint8_t)(0xc0 | type >> 8), *p++ = (uint8_t)type; /* -> _ssh._tcp.local */
    put16(rdlen, (uint16_t)(p - rdlen - 2));
    *p++ = (uint8_t)(0xc0 | inst >> 8), *p++ = (uint8_t)inst; /* SRV: the instance */
    p = put16(p, 33), p = put16(p, 0x8001), p = put32(p, 120), p = put16(p, 8);
    p = put16(p, 0), p = put16(p, 0), p = put16(p, 22);
    *p++ = (uint8_t)(0xc0 | host >> 8), *p++ = (uint8_t)host;
    *p++ = (uint8_t)(0xc0 | inst >> 8), *p++ = (uint8_t)inst; /* TXT */
    p = put16(p, 16), p = put16(p, 0x8001), p = put32(p, 4500), p = put16(p, 4);
    *p++ = 3, memcpy(p, "a=1", 3), p += 3;
    p = put_name(p, "_services._dns-sd._udp.local");
    p = put16(p, 12), p = put16(p, 1), p = put32(p, 4500), p = put16(p, 2);
    *p++ = (uint8_t)(0xc0 | type >> 8), *p++ = (uint8_t)type;
    return (size_t)(p - m);
}

static HvDnsMsg msg;

static int parse_exact(const uint8_t *src, size_t len) {
    uint8_t *b = malloc(len ? len : 1);
    memcpy(b, src, len);
    int r = hv_dns_parse(b, len, &msg);
    free(b);
    return r;
}

static uint32_t rng = 12345;
static uint32_t rnd(void) {
    rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
    return rng;
}

/* Every frame Henge sends the guest: within a 1500-byte MTU, and a message that parses. */
static int delivered, answers, frames_over, frames_unparsed, txt_max, mtu = 1500, questions;
static HvDnsMsg out;
static void deliver(const uint8_t *f, uint32_t len) {
    delivered++;
    size_t ulen = len >= 42 ? (size_t)(f[38] << 8 | f[39]) : 0;
    if (len > 14u + (uint32_t)mtu) frames_over++;
    if (len < 42 || ulen < 8 || 34 + ulen != len || hv_dns_parse(f + 42, ulen - 8, &out)) {
        frames_unparsed++;
        return;
    }
    if (!(out.flags & 0x8000)) {
        questions += out.nq;
        return;
    }
    answers++;
    for (int i = 0; i < out.nrr; i++)
        if (out.rr[i].type == 16 && out.rr[i].rdlen > txt_max) txt_max = out.rr[i].rdlen;
}

/* A frame from the guest (10.0.2.15:sport to 224.0.0.251:5353) around a message, exactly its length. */
static void take_from(const uint8_t *dns, size_t len, uint16_t sport) {
    size_t total = 42 + len;
    uint8_t *f = malloc(total);
    memset(f, 0, 42);
    f[12] = 8, f[14] = 0x45;
    put16(f + 16, (uint16_t)(28 + len)), f[22] = 255, f[23] = 17;
    f[26] = 10, f[28] = 2, f[29] = 15, f[30] = 224, f[33] = 251;
    put16(f + 34, sport), put16(f + 36, 5353), put16(f + 38, (uint16_t)(8 + len));
    memcpy(f + 42, dns, len);
    hv_mdns_take(f, (uint32_t)total);
    free(f);
}
static void take_exact(const uint8_t *dns, size_t len) { take_from(dns, len, 5353); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    static uint8_t m[HV_DNS_MAX_MSG];
    size_t len = announcement(m);
    check("an announcement parses", parse_exact(m, len), 0);
    check("its records", msg.nrr, 5);
    char t[300];
    hv_dns_name_text(&msg.rr[1].target, t, sizeof t);
    check("a compressed PTR target", strcmp(t, "Test Mac._ssh._tcp.local."), 0);
    hv_dns_name_text(&msg.rr[2].target, t, sizeof t);
    check("an SRV target", strcmp(t, "testmac.local."), 0);
    check("its port", msg.rr[2].port, 22);
    check("the cache-flush bit", msg.rr[0].top, 1);
    HvDnsName a, b;
    hv_dns_name_from(&a, "TestMac.LOCAL"), hv_dns_name_from(&b, "testmac.local");
    check("names compare in either case", hv_dns_name_eq(&a, &b), 1);

    /* A pointer forwards, or at itself. */
    uint8_t bad[64] = {0, 0, 0x84, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xc0, 14, 0, 1, 0, 1};
    check("a pointer forwards", parse_exact(bad, 18), -1);
    bad[13] = 12;
    check("a pointer at itself", parse_exact(bad, 18), -1);
    /* A pointer to a pointer to... backwards each time: fine. */
    uint8_t chain[40] = {0, 0, 0x84, 0, 0, 2, 0, 0, 0, 0, 0, 0, 1, 'a', 0, 0, 1, 0, 1, 0xc0, 12, 0, 1, 0, 1};
    check("a pointer backwards", parse_exact(chain, 25), 0);
    /* A label of the extended types (0x40, 0x80), whole within the message: refused for its type alone. */
    uint8_t ext[100] = {0, 0, 0x84, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0x41};
    memset(ext + 13, 'x', 65), ext[78] = 0, put16(ext + 79, 1), put16(ext + 81, 1);
    check("an extended label type", parse_exact(ext, 83), -1);
    /* A name of 256 bytes and more: four labels of 63. */
    uint8_t lng[300] = {0, 0, 0x84, 0, 0, 1};
    size_t at = 12;
    for (int i = 0; i < 4; i++) lng[at++] = 63, memset(lng + at, 'x', 63), at += 63;
    lng[at++] = 0, put16(lng + at, 1), put16(lng + at + 2, 1), at += 4;
    check("a name past 255 bytes", parse_exact(lng, at), -1);
    /* More records than the message could hold. */
    uint8_t many[40] = {0, 0, 0x84, 0, 0, 0, 0, 200};
    check("a count that can't fit", parse_exact(many, sizeof many), -1);
    /* An A record of 5 bytes; an SRV whose name runs past its data; a TXT string past its data. */
    memcpy(bad, (uint8_t[]){0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 5, 1, 2, 3, 4, 5}, 28);
    check("an A of 5 bytes", parse_exact(bad, 28), -1);
    memcpy(bad, (uint8_t[]){0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 33, 0, 1, 0, 0, 0, 1, 0, 8, 0, 0, 0, 0, 0, 22, 3, 'a', 'b', 'c', 0}, 34);
    check("an SRV name past its data", parse_exact(bad, 34), -1);
    memcpy(bad, (uint8_t[]){0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 16, 0, 1, 0, 0, 0, 1, 0, 2, 5, 'a'}, 25);
    check("a TXT string past its data", parse_exact(bad, 25), -1);
    /* A PTR whose name ends past its data, though within the message. */
    memcpy(bad, (uint8_t[]){0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 12, 0, 1, 0, 0, 0, 1, 0, 2, 1, 'a', 0}, 26);
    check("a PTR name past its data", parse_exact(bad, 26), -1);
    /* Escapes: a dot, a backslash and a control byte in a label. */
    HvDnsName e = {{7, 'a', '.', 'b', '\\', 'c', 1, 'd', 5, 'l', 'o', 'c', 'a', 'l', 0}, 15};
    hv_dns_name_text(&e, t, sizeof t);
    check("escapes (RFC 6763 4.3)", strcmp(t, "a\\.b\\\\c\\001d.local."), 0);
    check("a name too long for its buffer", hv_dns_name_text(&e, t, 8), -1);

    /* Cut anywhere: parsed or refused, never read past. */
    int cut_ok = 0;
    for (size_t c = 0; c < len; c++) cut_ok += parse_exact(m, c) == 0;
    check("cut short: the whole announcement never parses from less", cut_ok, 0);

    /* On a network of the smallest MTU (576): 10 of the Mac's services in one message that fits it (names
       compressed), and the 20 questions Henge asks about them (SRV, TXT), each message within one frame. */
    hv_net_ports_standard(NULL, 0);
    hv_mdns_start(deliver, 576);
    mtu = 576;
    for (int bye = 0; bye < 2; bye++) {
        uint8_t a[576] = {0}, *p = a + 12;
        put16(a + 2, 0x8400), put16(a + 6, 10);
        for (int i = 0; i < 10; i++) {
            if (i) *p++ = 0xc0, *p++ = 12; /* -> _ssh._tcp.local */
            else p = put_name(p, "_ssh._tcp.local");
            p = put16(p, 12), p = put16(p, 1), p = put32(p, bye ? 0 : 4500);
            char label[64];
            int l = snprintf(label, sizeof label, "A service with a long name, %02d", i);
            p = put16(p, (uint16_t)(1 + l + 2)), *p++ = (uint8_t)l, memcpy(p, label, (size_t)l), p += l;
            *p++ = 0xc0, *p++ = 12;
        }
        if (!bye) check("the Mac's message fits the MTU", (long long)(p - a) <= 576 - 28, 1);
        take_exact(a, (size_t)(p - a));
    }
    check("on a 576-byte network, Henge asks about all 10", questions, 20);
    check("each message within its MTU", frames_over, 0);
    mtu = 1500;
    hv_mdns_start(deliver, 1500);

    /* Random messages, and the announcement mutated: 1 to 8 bytes changed, or cut, or grown. */
    int parsed = 0;
    for (int i = 0; i < 300000; i++) {
        static uint8_t r[HV_DNS_MAX_MSG];
        size_t n;
        if (i % 4 == 0) {
            n = 12 + rnd() % 600;
            for (size_t k = 0; k < n; k++) r[k] = (uint8_t)rnd();
            r[4] = 0, r[5] = (uint8_t)(rnd() % 4), r[6] = 0, r[7] = (uint8_t)(rnd() % 8);
        } else {
            memcpy(r, m, len);
            n = len;
            int changes = 1 + (int)(rnd() % 8);
            for (int k = 0; k < changes; k++) r[rnd() % n] = (uint8_t)rnd();
            if (rnd() % 8 == 0) n = rnd() % n;
            else if (rnd() % 8 == 0) {
                size_t more = rnd() % 64;
                for (size_t k = 0; k < more && n < sizeof r; k++) r[n++] = (uint8_t)rnd();
            }
        }
        parsed += parse_exact(r, n) == 0;
        if (i % 3 == 0) take_exact(r, n); /* and as a frame, through what learns from it */
    }
    printf("  %d of 300000 random or mutated messages parsed; %d questions asked of the guest\n", parsed, delivered);
    check("some mutated messages still parse (the fuzz reaches the records)", parsed > 1000, 1);

    /* 5b. Escaped names: what prints reads back to the same bytes; malformed text is refused. */
    int round = 0;
    for (int i = 0; i < 100000; i++) {
        HvDnsName nm, back;
        size_t o = 0;
        int labels = 1 + (int)(rnd() % 5);
        for (int k = 0; k < labels; k++) {
            int l = 1 + (int)(rnd() % 63);
            if (o + 1 + (size_t)l + 1 > 255) break;
            nm.b[o++] = (uint8_t)l;
            for (int j = 0; j < l; j++) nm.b[o++] = (uint8_t)(rnd() % 4 ? 0x20 + rnd() % 95 : rnd());
        }
        nm.b[o++] = 0, nm.len = (uint16_t)o;
        char text[1100];
        if (hv_dns_name_text(&nm, text, sizeof text)) continue;
        round += !hv_dns_name_parse(&back, text) && back.len == nm.len && !memcmp(back.b, nm.b, nm.len);
    }
    check("every name printed reads back the same", round, 100000);
    HvDnsName pn;
    check("an empty label", hv_dns_name_parse(&pn, "a..local"), -1);
    char l64[80];
    memset(l64, 'x', 64), strcpy(l64 + 64, ".local");
    check("a label of 64", hv_dns_name_parse(&pn, l64), -1);
    check("an escape past 255", hv_dns_name_parse(&pn, "a\\256b.local"), -1);
    check("an escape cut short", hv_dns_name_parse(&pn, "a\\2"), -1);
    check("a backslash at the end", hv_dns_name_parse(&pn, "a\\"), -1);
    check("escapes read", hv_dns_name_parse(&pn, "My\\.Mac\\032x._smb._tcp.local."), 0);
    char pt[300];
    hv_dns_name_text(&pn, pt, sizeof pt);
    check("and print back", strcmp(pt, "My\\.Mac x._smb._tcp.local."), 0);

    /* 5b. The network's devices known; the Mac's questions, built and mutated. */
    static const char *const types[] = {"_smb._tcp", "_ipp._tcp", "_http._tcp", "_device-info._tcp", "_ssh._tcp", "_printer._tcp"};
    static char pool[200][120];
    int npool = 0;
    snprintf(pool[npool++], 120, "_services._dns-sd._udp.local");
    for (int t = 0; t < 6; t++) snprintf(pool[npool++], 120, "%s.local", types[t]);
    static uint8_t txt[1300];
    for (int i = 0; i < 100; i++) {
        char inst[120], host[80];
        snprintf(inst, sizeof inst, "Dev\\.ice %d.%s.local", i, types[i % 6]);
        snprintf(host, sizeof host, "host%d.local", i % 40);
        uint16_t tl = i % 9 == 0 ? 1300 : (uint16_t)(rnd() % 300);
        for (int k = 0; k < tl; k++) txt[k] = (uint8_t)rnd();
        if (i % 5) /* well formed (strings of up to 255 that end with it); the rest as they came, mostly not */
            for (int k = 0; k < tl;) {
                int l = tl - k - 1 < 255 ? tl - k - 1 : (int)(rnd() % 256);
                txt[k] = (uint8_t)l, k += 1 + l;
            }
        hv_mdns_test_found(inst, host, htonl((uint32_t)(0x0a000100 + i % 40)), (uint16_t)(1000 + i), txt, tl);
        snprintf(pool[npool++], 120, "%s", inst);
        if (i < 40) snprintf(pool[npool++], 120, "%s", host);
    }
    static const uint16_t qtypes[] = {1, 12, 16, 33, 255, 28, 47};
    for (int i = 0; i < 100000; i++) {
        uint8_t q[1500] = {0}, *p = q + 12;
        HvDnsName qn;
        int nq = 1 + (int)(rnd() % 4), na = rnd() % 3 ? 0 : 1 + (int)(rnd() % 3);
        put16(q, (uint16_t)rnd()), put16(q + 4, (uint16_t)nq), put16(q + 6, (uint16_t)na);
        for (int k = 0; k < nq + na; k++) {
            if (hv_dns_name_parse(&qn, pool[rnd() % (uint32_t)npool])) continue;
            memcpy(p, qn.b, qn.len), p += qn.len;
            p = put16(p, k < nq ? qtypes[rnd() % 7] : 12), p = put16(p, (uint16_t)(1 | (rnd() % 4 ? 0 : 0x8000)));
            if (k >= nq) { /* a known answer: a PTR at one of the pool's names */
                p = put32(p, rnd() % 5000);
                if (hv_dns_name_parse(&qn, pool[rnd() % (uint32_t)npool])) qn.len = 1, qn.b[0] = 0;
                p = put16(p, qn.len), memcpy(p, qn.b, qn.len), p += qn.len;
            }
        }
        size_t n = (size_t)(p - q);
        if (rnd() % 2) {
            int changes = 1 + (int)(rnd() % 4);
            for (int k = 0; k < changes; k++) q[12 + rnd() % (n - 12)] = (uint8_t)rnd();
        }
        take_from(q, n, rnd() % 3 ? 5353 : (uint16_t)(1024 + rnd() % 60000));
    }
    printf("  %d answers to the Mac's questions; largest TXT %d bytes\n", answers, txt_max);
    check("the Mac's questions are answered", answers > 10000, 1);
    check("a TXT of 1300 bytes among them", txt_max, 1300);
    check("every frame to the guest within a 1500-byte MTU", frames_over, 0);
    check("and a message that parses", frames_unparsed, 0);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
