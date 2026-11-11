#include "power_source.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* See power_source.h for the protocol. The guest's one buffer stays in the
   ring (not taken) until there is something to say, so a snapshot holds
   nothing of it beyond the queue itself. */

void hv_power_source_encode(const HvPowerState *s, uint8_t out[HV_POWER_MSG_LEN]) {
    out[0] = HV_POWER_MSG_STATUS;
    memcpy(out + 1, &s->percent, 8); /* unaligned, little-endian: as the driver reads it */
    out[9] = s->external;
    out[10] = s->charging;
    out[11] = s->full;
}

/* Answer the waiting buffer if the guest hasn't heard the current state. */
static void try_send(HvPowerSource *p) {
    HvVirtio *v = &p->vio;
    HvVirtq *q = &v->q[0];
    if (!(v->status & 4) || !q->enabled) return; /* no driver running: it gets the state when it starts */
    pthread_mutex_lock(&p->mu);
    HvPowerState s = p->want;
    uint64_t gen = p->want_gen;
    pthread_mutex_unlock(&p->mu);
    /* Nothing taken since the driver set the queue up (it started, or restarted after a reset):
       it has been told nothing yet, so its first buffer gets the state at once. */
    int fresh = q->last_avail == 0;
    if (!fresh && !p->resend && gen == p->sent_gen) return;
    HvVirtqElem e;
    int r = hv_virtq_pop(v, 0, &e);
    if (r == 0) return; /* no buffer yet: the next notify or poll */
    p->sent_gen = gen;
    p->resend = 0;
    uint32_t written = 0;
    if (r > 0 && hv_virtq_writable(&e) >= HV_POWER_MSG_LEN) {
        uint8_t msg[HV_POWER_MSG_LEN];
        hv_power_source_encode(&s, msg);
        written = hv_virtq_write(v, &e, 0, msg, sizeof msg);
        p->sent = s;
        p->messages++;
    } else {
        static int said;
        if (said++ < 10) fprintf(stderr, "power source: guest buffer unusable (%u writable bytes)\n", r > 0 ? hv_virtq_writable(&e) : 0);
    }
    hv_virtq_push(v, 0, &e, written);
    hv_virtio_interrupt(v);
}

static void notify(HvVirtio *v, int q) {
    if (q == 0) try_send(v->dev);
}

static void config_write(HvVirtio *v, uint32_t off, uint64_t val, unsigned size) {
    (void)v, (void)off, (void)val, (void)size; /* read-only */
}

void hv_power_source_set(HvPowerSource *p, const HvPowerState *in) {
    if (!p || !in) return;
    HvPowerState s = {.percent = in->percent, .external = in->external != 0, .charging = in->charging != 0, .full = in->full != 0};
    if (isnan(s.percent) || s.percent < 0) s.percent = 0;
    if (s.percent > 100) s.percent = 100;
    pthread_mutex_lock(&p->mu);
    HvPowerState *w = &p->want;
    if (s.percent != w->percent || s.external != w->external || s.charging != w->charging || s.full != w->full) {
        *w = s;
        p->want_gen++;
    }
    pthread_mutex_unlock(&p->mu);
}

void hv_power_source_poll(HvPowerSource *p) {
    if (p) try_send(p);
}

HvPowerSource *hv_power_source_create(HvMachine *m, HvPcie *bus, int kind) {
    if (kind != HV_POWER_BATTERY && kind != HV_POWER_WALL) return NULL;
    HvPowerSource *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    HvVirtio *v = &p->vio;
    /* No virtio device type of its own: Apple's transport matches on the PCI ID alone.
       Class: system peripheral, other. */
    hv_virtio_init(v, m, 0, 0x088000, 1, 0);
    v->pci.config[0] = 0x6b, v->pci.config[1] = 0x10; /* vendor Apple */
    v->pci.config[2] = 0x07, v->pci.config[3] = 0x1a; /* device 0x1a07 */
    v->pci.config[0x2c] = 0x6b, v->pci.config[0x2d] = 0x10;
    v->pci.config[0x2e] = 0x07, v->pci.config[0x2f] = 0x1a;
    v->dev = p;
    v->notify = notify;
    v->config_write = config_write;
    v->config[0] = (uint8_t)kind;
    v->config_len = 1;
    p->kind = kind;
    pthread_mutex_init(&p->mu, NULL);
    /* Until the host says otherwise: on the charger and full (what a Mac on its power adapter shows). */
    p->want = (HvPowerState){.percent = 100, .external = 1, .full = 1};
    p->want_gen = 1;
    if (hv_pcie_attach(bus, &v->pci) < 0) {
        pthread_mutex_destroy(&p->mu);
        free(p);
        return NULL;
    }
    return p;
}
