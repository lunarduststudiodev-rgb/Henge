#include "virtio_net.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define F_MAC (1ull << 5)
#define F_STATUS (1ull << 16)
#define RXQ 0
#define TXQ 1
#define HDR_LEN 12 /* virtio_net_hdr with num_buffers (VERSION_1) */
#define MAX_FRAME 65550
#define CARRIER_UP (1ull << 63)
#define CARRIER_DOWN_NS 3000000000ull /* the host's network gone this long: the cable is out */
#define CARRIER_UP_NS 1000000000ull   /* and back this long: in again */

static void notify(HvVirtio *v, int q) {
    HvVirtioNet *n = v->dev;
    if (q != TXQ) return; /* new receive buffers: nothing waiting to deliver */
    static HvVirtqElem e;
    static uint8_t frame[MAX_FRAME];
    int any = 0;
    while (hv_virtq_pop(v, q, &e) > 0) {
        uint32_t len = hv_virtq_read(v, &e, HDR_LEN, frame, sizeof frame);
        if (!n->carrier) n->tx_dropped++; /* the cable is out: it goes nowhere */
        else {
            if (n->transmit) n->transmit(n->opaque, frame, len);
            n->tx_frames++;
        }
        hv_virtq_push(v, q, &e, 0);
        any = 1;
    }
    if (any) hv_virtio_interrupt(v);
}

int hv_virtio_net_receive(HvVirtioNet *n, const uint8_t *frame, uint32_t len) {
    HvVirtio *v = &n->vio;
    static HvVirtqElem e;
    if (!n->carrier) { /* the cable is out: nothing arrives (taken, so nothing stale waits for it to go back in) */
        n->rx_dropped++;
        return 1;
    }
    if (!(v->status & 4) || !v->q[RXQ].enabled || hv_virtq_pop(v, RXQ, &e) <= 0) return 0;
    if (hv_virtq_writable(&e) < HDR_LEN + len) { /* too small for it: drop the frame, return the buffer */
        hv_virtq_push(v, RXQ, &e, 0);
        hv_virtio_interrupt(v);
        n->rx_dropped++;
        return 1;
    }
    uint8_t hdr[HDR_LEN] = {0};
    hdr[10] = 1; /* num_buffers */
    hv_virtq_write(v, &e, 0, hdr, HDR_LEN);
    hv_virtq_write(v, &e, HDR_LEN, frame, len);
    hv_virtq_push(v, RXQ, &e, HDR_LEN + len);
    hv_virtio_interrupt(v);
    n->rx_frames++;
    return 1;
}

uint64_t hv_virtio_net_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void set_link(HvVirtioNet *n, int up) {
    HvVirtio *v = &n->vio;
    uint16_t status = up ? 1 : 0;
    memcpy(v->config + 6, &status, 2);
    v->config_gen++;
    if (!(v->status & 4)) return; /* no driver running: it reads the status when it starts */
    v->isr |= 2;                  /* configuration changed */
    hv_pci_set_intx(&v->pci, 1);
}

void hv_virtio_net_relink(HvVirtioNet *n, unsigned ms) {
    if (!n) return;
    set_link(n, 0);
    n->link_up_at = hv_virtio_net_now() + (uint64_t)ms * 1000000ull;
    if (!n->link_up_at) n->link_up_at = 1;
}

int hv_virtio_net_link_up(HvVirtioNet *n) {
    uint16_t status;
    memcpy(&status, n->vio.config + 6, 2);
    return status & 1;
}

void hv_virtio_net_set_carrier(HvVirtioNet *n, int up, uint64_t at_ns) {
    if (!n) return;
    if (!at_ns) at_ns = hv_virtio_net_now();
    uint64_t want = (up ? CARRIER_UP : 0) | (at_ns & ~CARRIER_UP);
    uint64_t old = atomic_load_explicit(&n->carrier_req, memory_order_relaxed);
    do {
        if ((old & CARRIER_UP) == (want & CARRIER_UP)) return; /* no change: the time it changed stands */
    } while (!atomic_compare_exchange_weak_explicit(&n->carrier_req, &old, want, memory_order_release, memory_order_relaxed));
}

void hv_virtio_net_poll(HvVirtioNet *n) {
    if (!n) return;
    uint64_t now = hv_virtio_net_now();
    uint64_t req = atomic_load_explicit(&n->carrier_req, memory_order_acquire);
    int want = (req & CARRIER_UP) != 0;
    uint64_t since = req & ~CARRIER_UP;
    if (want != n->carrier && now >= since && now - since >= (want ? n->carrier_up_ns : n->carrier_down_ns)) {
        n->carrier = want;
        n->carrier_changes++;
        if (n->carrier_changed) n->carrier_changed(n->opaque, want);
    }
    if (n->link_up_at && now >= n->link_up_at) n->link_up_at = 0;
    int link = n->carrier && !n->link_up_at;
    if (link != hv_virtio_net_link_up(n)) set_link(n, link);
}

HvVirtioNet *hv_virtio_net_create(HvMachine *m, HvPcie *bus, const uint8_t mac[6]) {
    HvVirtioNet *n = calloc(1, sizeof *n);
    HvVirtio *v = &n->vio;
    hv_virtio_init(v, m, 1, 0x020000, 2, F_MAC | F_STATUS);
    v->dev = n;
    v->notify = notify;
    memcpy(v->config, mac, 6);
    uint16_t status = 1, pairs = 1; /* link up */
    memcpy(v->config + 6, &status, 2);
    memcpy(v->config + 8, &pairs, 2);
    v->config_len = 12;
    n->carrier = 1;
    atomic_init(&n->carrier_req, CARRIER_UP);
    n->carrier_down_ns = CARRIER_DOWN_NS;
    n->carrier_up_ns = CARRIER_UP_NS;
    hv_pcie_attach(bus, &v->pci);
    return n;
}
