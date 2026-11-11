/* The Mac's ports on the network, as a MacBook's would be (design: the port
   forwarding hostfwd notes).

   iOS gives an app no second address and no bridge onto Wi-Fi, so the Mac shares the device's: each port the
   Mac listens on is opened at the same number on the device, on every interface (0.0.0.0), and libslirp hands
   what arrives to 10.0.2.15 at that port. There is no switch: the Mac's own Sharing settings and firewall decide,
   as they would on a MacBook. Which ports: what the helper in the Mac says it listens on (share.c's "ports"), or,
   while no helper is linked, the standard sharing ports; and every port the Mac advertises a Bonjour service on.

   The lists change on any thread; the listeners are opened and closed only on the network thread (libslirp is
   not thread-safe), at the top of its loop (hv_fwd_apply), which a change wakes. */
#include "net_forward.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Remote Login (SSH and SFTP), Kerberos (Screen Sharing's and File Sharing's logins against the Mac's own KDC),
   File Sharing (SMB), AFP, Remote Management, Screen Sharing. */
static const HvFwdPort sharing[] = {{22, 0}, {88, 0}, {445, 0}, {548, 0}, {3283, 0}, {5900, 0}, {88, 1}, {3283, 1}};
#define NSTANDARD (int)(sizeof sharing / sizeof sharing[0])
#define MAX_ADV 64

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static HvFwdPort helper[HV_FWD_MAX], adv[MAX_ADV];
static int nhelper = -1, nadv; /* nhelper -1: no helper linked */
static int hold, closed, dirty, retry;
static HvFwdPort standard[NSTANDARD] = {{22, 0}, {88, 0}, {445, 0}, {548, 0}, {3283, 0}, {5900, 0}, {88, 1}, {3283, 1}};
static int nstandard = NSTANDARD;
static HvFwdStatus status[HV_FWD_MAX + MAX_ADV + NSTANDARD];
static int nstatus;
static const HvFwdOps *ops;
static _Atomic unsigned generation;

/* The network thread's own: what is open, or failed and why. */
typedef struct Entry {
    HvFwdPort p;
    int err; /* 0: open */
} Entry;
static Entry table[HV_FWD_MAX + MAX_ADV + NSTANDARD];
static int ntable;
/* Network thread: the ports the log has said go to the Mac, so that opening them again (Henge back on the screen, a
   fresh NAT) is one line, not one per port. A port the Mac stops listening on leaves it. */
static HvFwdPort said[HV_FWD_MAX + MAX_ADV + NSTANDARD];
static int nsaid;

static int cmp(const void *a, const void *b) {
    const HvFwdPort *x = a, *y = b;
    return x->udp != y->udp ? x->udp - y->udp : (int)x->port - (int)y->port;
}

static int same(HvFwdPort a, HvFwdPort b) { return a.port == b.port && a.udp == b.udp; }

static void changed(int again) {
    dirty = 1;
    if (again) retry = 1;
}

static void wake(void) {
    if (ops && ops->wake) ops->wake();
}

static int take(HvFwdPort *dst, int max, const HvFwdPort *src, int n) {
    int k = 0;
    for (int i = 0; i < n && k < max; i++)
        if (src[i].port && src[i].udp <= 1) dst[k++] = src[i];
    return k;
}

void hv_net_ports_set(const HvFwdPort *ports, int n) {
    pthread_mutex_lock(&mu);
    nhelper = n < 0 ? -1 : take(helper, HV_FWD_MAX, ports, n);
    changed(1);
    pthread_mutex_unlock(&mu);
    wake();
}

void hv_net_ports_standard(const HvFwdPort *ports, int n) {
    pthread_mutex_lock(&mu);
    if (n < 0) memcpy(standard, sharing, sizeof sharing), nstandard = NSTANDARD;
    else nstandard = take(standard, NSTANDARD, ports, n);
    changed(1);
    pthread_mutex_unlock(&mu);
    wake();
}

void hv_net_ports_advertised(const HvFwdPort *ports, int n) {
    pthread_mutex_lock(&mu);
    nadv = take(adv, MAX_ADV, ports, n < 0 ? 0 : n);
    changed(1);
    pthread_mutex_unlock(&mu);
    wake();
}

void hv_net_forwards_hold(int h) {
    pthread_mutex_lock(&mu);
    int was = hold;
    hold = h != 0;
    if (was != hold) changed(!hold);
    pthread_mutex_unlock(&mu);
    wake();
}

void hv_net_forwards_close(void) {
    pthread_mutex_lock(&mu);
    closed = 1;
    changed(0);
    pthread_mutex_unlock(&mu);
    wake();
}

int hv_net_forward_status(HvFwdStatus *out, int max, int *from_helper, int *held) {
    pthread_mutex_lock(&mu);
    int n = nstatus < max ? nstatus : max;
    if (out && n > 0) memcpy(out, status, (size_t)n * sizeof *out);
    if (from_helper) *from_helper = nhelper >= 0;
    if (held) *held = hold || closed;
    pthread_mutex_unlock(&mu);
    return n;
}

static const char *proto(int udp) { return udp ? "UDP" : "TCP"; }

/* Why a port can't be the Mac's, in words. */
static const char *why(int err) {
    if (err == EADDRINUSE) return "the device itself uses it";
    if (err == EACCES || err == EPERM) return "iOS keeps it for its own";
    return strerror(err);
}

void hv_net_forward_line(char *buf, size_t n) {
    HvFwdStatus s[HV_FWD_MAX + MAX_ADV + NSTANDARD];
    int helper_on, held;
    int k = hv_net_forward_status(s, (int)(sizeof s / sizeof s[0]), &helper_on, &held);
    size_t l = (size_t)snprintf(buf, n, "%s%s:", held ? "held, " : "", helper_on ? "the helper's list" : "the standard sharing ports");
    for (int i = 0; i < k && l < n; i++)
        l += (size_t)snprintf(buf + l, n - l, " %u %s%s%s", s[i].port, proto(s[i].udp), s[i].err ? ": " : "",
                              s[i].err ? why(s[i].err) : "");
}

void hv_fwd_attach(const HvFwdOps *o) {
    pthread_mutex_lock(&mu);
    ops = o;
    nhelper = -1, nadv = 0, closed = 0; /* a machine of its own */
    pthread_mutex_unlock(&mu);
}

void hv_fwd_lost(void) {
    ntable = 0;
    pthread_mutex_lock(&mu);
    changed(1);
    pthread_mutex_unlock(&mu);
}

void hv_fwd_apply(void) {
    HvFwdPort want[HV_FWD_MAX + MAX_ADV + NSTANDARD];
    int nwant = 0, again, held;
    pthread_mutex_lock(&mu);
    if (!dirty || !ops) {
        pthread_mutex_unlock(&mu);
        return;
    }
    dirty = 0;
    again = retry;
    retry = 0;
    held = hold || closed;
    int stopped = closed;
    if (!held) {
        const HvFwdPort *base = nhelper >= 0 ? helper : standard;
        int nbase = nhelper >= 0 ? nhelper : nstandard;
        memcpy(want, base, (size_t)nbase * sizeof *want);
        memcpy(want + nbase, adv, (size_t)nadv * sizeof *want);
        nwant = nbase + nadv;
    }
    pthread_mutex_unlock(&mu);

    qsort(want, (size_t)nwant, sizeof *want, cmp);
    int u = 0;
    for (int i = 0; i < nwant; i++)
        if (!u || !same(want[u - 1], want[i])) want[u++] = want[i];
    nwant = u;

    /* What is no longer wanted goes. */
    int closed_now = 0, k = 0;
    for (int i = 0; i < ntable; i++) {
        HvFwdPort p = table[i].p;
        if (bsearch(&p, want, (size_t)nwant, sizeof *want, cmp)) {
            table[k++] = table[i];
            continue;
        }
        if (!table[i].err) {
            ops->close(p.udp, p.port);
            closed_now++;
            if (!held) fprintf(stderr, "hengevm: net: port %u %s no longer goes to the Mac (it stopped listening)\n", p.port, proto(p.udp));
        }
        if (!held)
            for (int j = 0; j < nsaid; j++)
                if (same(said[j], p)) said[j] = said[--nsaid];
    }
    ntable = k;
    if (stopped && closed_now) fprintf(stderr, "hengevm: net: the Mac has stopped: its %d ports on the device are closed\n", closed_now);
    else if (held && closed_now) fprintf(stderr, "hengevm: net: the Mac's %d ports are closed while Henge is off the screen or the Mac is paused\n", closed_now);

    /* What is wanted and not open, opens; one that failed is tried again only when asked to (a list changed,
       Henge came back). */
    int opened_now = 0, again_now = 0;
    for (int i = 0; i < nwant; i++) {
        Entry *e = NULL;
        for (int j = 0; j < ntable; j++)
            if (same(table[j].p, want[i])) e = &table[j];
        if (e && (!e->err || !again)) continue;
        int err = ops->open(want[i].udp, want[i].port);
        if (!e) {
            e = &table[ntable++];
            e->p = want[i];
            e->err = -1; /* not tried before: log either way */
        }
        if (!err) {
            int k = 0;
            while (k < nsaid && !same(said[k], want[i])) k++;
            if (k < nsaid && e->err <= 0) {
                again_now++; /* as it was before Henge left the screen, or before the fresh NAT */
            } else {
                opened_now++;
                if (e->err > 0 || nwant <= 16) fprintf(stderr, "hengevm: net: port %u %s goes to the Mac\n", want[i].port, proto(want[i].udp));
                if (k == nsaid && nsaid < (int)(sizeof said / sizeof *said)) said[nsaid++] = want[i];
            }
        } else if (err != e->err) {
            fprintf(stderr, "hengevm: net: port %u %s can't be the Mac's: %s (%s, errno %d)\n", want[i].port,
                    proto(want[i].udp), why(err), strerror(err), err);
        }
        e->err = err;
    }
    if (opened_now && nwant > 16) fprintf(stderr, "hengevm: net: %d of the Mac's ports go to it\n", opened_now);
    if (again_now) fprintf(stderr, "hengevm: net: the Mac's %d port%s go%s to it again\n", again_now, again_now == 1 ? "" : "s", again_now == 1 ? "es" : "");

    qsort(table, (size_t)ntable, sizeof *table, cmp); /* Entry starts with its HvFwdPort */
    pthread_mutex_lock(&mu);
    nstatus = ntable;
    for (int i = 0; i < ntable; i++) status[i] = (HvFwdStatus){table[i].p.port, table[i].p.udp, table[i].err};
    pthread_mutex_unlock(&mu);
    atomic_fetch_add(&generation, 1);
}

unsigned hv_net_forward_generation(void) { return atomic_load(&generation); }
