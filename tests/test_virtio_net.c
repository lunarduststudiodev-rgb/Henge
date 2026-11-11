/* The network card's cable following the host's own network (virtio_net.h's carrier): out once the host has
   been without a network for the debounce, back in once it has one again, a short drop (a hand-over from Wi-Fi
   to cellular) never showing; each change a config change the driver sees, as on a real card; nothing in or out
   while it is out; the backend told as it changes; Suspend's relink unchanged and still keeping the cable out
   while the host has no network; and reports from another thread while the board's tick applies them. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
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

#define RAM_PA 0x40000000ull
#define RAM_SIZE (4u << 20)
#define QSIZE 8
#define RXQ 0
#define TXQ 1
#define SEC 1000000000ull
static uint8_t *ram;
static HvMachine m;
static HvVirtioNet *n;

static void *at(uint64_t pa) { return ram + (pa - RAM_PA); }
static uint16_t r16(uint64_t pa) {
    uint16_t v;
    memcpy(&v, at(pa), 2);
    return v;
}
static void w16(uint64_t pa, uint16_t v) { memcpy(at(pa), &v, 2); }

/* Each queue: descriptors, available and used rings, and one buffer per descriptor. */
static uint64_t qbase(int q) { return RAM_PA + 0x10000 + (uint64_t)q * 0x10000; }
static uint64_t desc(int q) { return qbase(q); }
static uint64_t avail(int q) { return qbase(q) + 0x1000; }
static uint64_t used(int q) { return qbase(q) + 0x2000; }
static uint64_t buf(int q, int i) { return qbase(q) + 0x4000 + (uint64_t)i * 0x800; }

static void setup_queue(int q) {
    HvVirtq *vq = &n->vio.q[q];
    vq->size = QSIZE;
    vq->desc = desc(q);
    vq->avail = avail(q);
    vq->used = used(q);
    vq->enabled = 1;
}

/* The driver posts buffer i (written by the device on the receive queue, holding a frame on the send queue). */
static void post(int q, int i, uint32_t len) {
    uint8_t *d = at(desc(q) + 16u * i);
    uint64_t addr = buf(q, i);
    uint16_t flags = q == RXQ ? 2 : 0, next = 0;
    memcpy(d, &addr, 8);
    memcpy(d + 8, &len, 4);
    memcpy(d + 12, &flags, 2);
    memcpy(d + 14, &next, 2);
    uint16_t idx = r16(avail(q) + 2);
    w16(avail(q) + 4 + 2u * (idx % QSIZE), (uint16_t)i);
    w16(avail(q) + 2, idx + 1);
}

static int sent, changed_to = -1, changes;
static void transmit(void *opaque, const uint8_t *frame, uint32_t len) {
    (void)opaque, (void)frame, (void)len;
    sent++;
}
static void carrier_changed(void *opaque, int up) {
    (void)opaque;
    changed_to = up;
    changes++;
}

static uint16_t status(void) {
    uint16_t s;
    memcpy(&s, n->vio.config + 6, 2);
    return s;
}

/* A config-change interrupt the driver hasn't taken yet: taken (as reading the ISR does), 1; else 0. */
static int config_irq(void) {
    int got = (n->vio.isr & 2) != 0;
    n->vio.isr = 0;
    return got;
}

static void *reporter(void *arg) {
    (void)arg;
    uint64_t t = hv_virtio_net_now();
    for (int i = 0; i < 200000; i++) hv_virtio_net_set_carrier(n, i & 1, t);
    hv_virtio_net_set_carrier(n, 0, t - 10 * SEC); /* it ends without a network, long since */
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&m, 4 << 20) != 0) return 1;
    ram = calloc(1, RAM_SIZE);
    hv_machine_add_ram(&m, RAM_PA, RAM_SIZE, ram, 0);
    HvGic *gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    HvPcie *bus = hv_pcie_create(&m, gic, 0x10000000, 0x10000000, 0x50000000, 0x10000000, 32 + 4);
    static const uint8_t mac[6] = {0x02, 0, 0, 0, 0, 1};
    n = hv_virtio_net_create(&m, bus, mac);
    n->transmit = transmit;
    n->carrier_changed = carrier_changed;
    check("offers the link status (F_STATUS)", (n->vio.features >> 16) & 1, 1);
    check("cable in from the start", hv_virtio_net_link_up(n), 1);
    check("debounce out: 3 s", (long long)n->carrier_down_ns, 3 * SEC);
    check("debounce back in: 1 s", (long long)n->carrier_up_ns, 1 * SEC);

    /* The driver running, a receive buffer posted */
    n->vio.status = 1 | 2 | 4 | 8;
    setup_queue(RXQ);
    setup_queue(TXQ);
    post(RXQ, 0, 0x800);
    hv_virtio_net_poll(n);
    check("nothing changes while the host has its network", status() == 1 && changes == 0 && !config_irq(), 1);

    /* The host loses its network: nothing at once, the cable out after the debounce */
    uint64_t gen = n->vio.config_gen;
    hv_virtio_net_set_carrier(n, 0, 0);
    hv_virtio_net_poll(n);
    check("a drop just now: still in", status(), 1);
    check("a drop just now: the driver told nothing", config_irq(), 0);
    /* A hand-over: the network back before the debounce ran out */
    hv_virtio_net_set_carrier(n, 1, 0);
    hv_virtio_net_poll(n);
    check("hand-over: never out", status() == 1 && changes == 0 && n->vio.config_gen == gen, 1);

    /* Gone for good (reported 4 s ago) */
    hv_virtio_net_set_carrier(n, 0, hv_virtio_net_now() - 4 * SEC);
    uint64_t req = atomic_load(&n->carrier_req);
    hv_virtio_net_set_carrier(n, 0, 0); /* told again: when it went stands */
    check("the same report again keeps its time", (long long)atomic_load(&n->carrier_req), (long long)req);
    hv_virtio_net_poll(n);
    check("gone: cable out", status(), 0);
    check("gone: link down", hv_virtio_net_link_up(n), 0);
    check("gone: a config change", n->vio.config_gen != gen && config_irq(), 1);
    check("gone: the card's INTx raised", n->vio.pci.intx, 1);
    check("gone: the backend told", changed_to == 0 && changes == 1, 1);
    hv_virtio_net_poll(n);
    check("gone: told once", changes == 1 && !config_irq(), 1);

    /* Unplugged, nothing arrives and nothing leaves */
    static const uint8_t frame[64] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint64_t dropped = n->rx_dropped;
    check("out: a frame for the guest is taken", hv_virtio_net_receive(n, frame, sizeof frame), 1);
    check("out: and dropped", (long long)(n->rx_dropped - dropped), 1);
    check("out: the receive buffer still waiting", r16(used(RXQ) + 2), 0);
    post(TXQ, 0, 12 + 60);
    n->vio.notify(&n->vio, TXQ);
    check("out: the guest's frame goes nowhere", sent, 0);
    check("out: counted as dropped", (long long)n->tx_dropped, 1);
    check("out: its buffer handed back", r16(used(TXQ) + 2), 1);

    /* Suspend's relink while the host has no network: the cable stays out after its time is up */
    hv_virtio_net_relink(n, 0);
    n->link_up_at = 1; /* its time has come */
    hv_virtio_net_poll(n);
    check("relink without a network: still out", status() == 0 && n->link_up_at == 0, 1);

    /* The network back: in after the debounce, the driver told, the backend told, frames flow again */
    gen = n->vio.config_gen;
    hv_virtio_net_set_carrier(n, 1, 0);
    hv_virtio_net_poll(n);
    check("back just now: still out", status(), 0);
    hv_virtio_net_set_carrier(n, 0, 0); /* a flap */
    hv_virtio_net_set_carrier(n, 1, hv_virtio_net_now() - 2 * SEC);
    hv_virtio_net_poll(n);
    check("back: cable in", status(), 1);
    check("back: a config change", n->vio.config_gen != gen && config_irq(), 1);
    check("back: the backend told", changed_to == 1 && changes == 2, 1);
    check("back: a frame arrives", hv_virtio_net_receive(n, frame, sizeof frame), 1);
    check("back: in the guest's buffer", r16(used(RXQ) + 2), 1);
    post(TXQ, 1, 12 + 60);
    n->vio.notify(&n->vio, TXQ);
    check("back: the guest's frame sent", sent, 1);

    /* Suspend's relink with the network there: as before, out then in, the backend not told */
    hv_virtio_net_relink(n, 1500);
    check("relink: out at once", status() == 0 && config_irq(), 1);
    hv_virtio_net_poll(n);
    check("relink: still out before its time", status(), 0);
    n->link_up_at = 1;
    hv_virtio_net_poll(n);
    check("relink: then in", status() == 1 && n->link_up_at == 0 && config_irq(), 1);
    check("relink: not a change of the host's network", changes, 2);
    /* Relinked frames still flow while the relink has the cable out (the host's network is there) */
    hv_virtio_net_relink(n, 1500);
    post(RXQ, 1, 0x800);
    check("relink: a frame still arrives", hv_virtio_net_receive(n, frame, sizeof frame) == 1 && r16(used(RXQ) + 2) == 2, 1);
    n->link_up_at = 1;
    hv_virtio_net_poll(n);
    config_irq();

    /* No driver yet (before macOS starts it): the status is set for it to read, no interrupt */
    n->vio.status = 0;
    hv_virtio_net_set_carrier(n, 0, hv_virtio_net_now() - 4 * SEC);
    hv_virtio_net_poll(n);
    check("no driver: status out", status(), 0);
    check("no driver: no interrupt", config_irq(), 0);
    hv_virtio_net_set_carrier(n, 1, hv_virtio_net_now() - 2 * SEC);
    hv_virtio_net_poll(n);
    check("no driver: status back in", status(), 1);
    n->vio.status = 1 | 2 | 4 | 8;

    /* Reports from another thread while the tick applies them */
    pthread_t t;
    pthread_create(&t, NULL, reporter, NULL);
    for (int i = 0; i < 20000; i++) hv_virtio_net_poll(n);
    pthread_join(t, NULL);
    hv_virtio_net_poll(n);
    check("from another thread: the last report wins", status(), 0);
    check("from another thread: the backend's last word is out", changed_to, 0);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
