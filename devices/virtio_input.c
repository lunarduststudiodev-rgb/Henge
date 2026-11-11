#include "virtio_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EVENTQ 0
#define STATUSQ 1

/* Config space (virtio 1.x, 5.8.4): the driver writes select and subsel,
   then reads size and up to 128 bytes of data from offset 8. */
#define CFG_ID_NAME 0x01
#define CFG_ID_SERIAL 0x02
#define CFG_ID_DEVIDS 0x03
#define CFG_PROP_BITS 0x10
#define CFG_EV_BITS 0x11
#define CFG_ABS_INFO 0x12

static const char *kind_name(int kind) {
    return kind == HV_VINPUT_KEYBOARD ? "keyboard" : kind == HV_VINPUT_MOUSE ? "mouse" : "tablet";
}

static void set_bit(uint8_t *bits, unsigned n) {
    bits[n / 8] |= (uint8_t)(1u << (n % 8));
}

/* Answer a query: fill size and data for the current select/subsel. */
static void answer(HvVirtioInput *d) {
    uint8_t *c = d->vio.config;
    uint8_t select = c[0], subsel = c[1];
    uint8_t *data = c + 8;
    memset(c + 2, 0, 6 + 128);
    uint32_t size = 0;
    int kbd = d->kind == HV_VINPUT_KEYBOARD, tablet = d->kind == HV_VINPUT_TABLET;
    switch (select) {
    case CFG_ID_NAME: {
        const char *name = kbd ? "HengeVM Keyboard" : tablet ? "HengeVM Touch" : "HengeVM Mouse";
        size = (uint32_t)strlen(name);
        memcpy(data, name, size);
        break;
    }
    case CFG_ID_SERIAL:
        data[0] = '0';
        size = 1;
        break;
    case CFG_ID_DEVIDS: { /* bustype BUS_VIRTUAL, vendor, product, version */
        /* AppleVirtIOInputKeyboard always says vendor Apple (0x5ac) and takes the
           product from here; HV_KBD_PRODUCT tries others (experiment). */
        uint16_t prod = (uint16_t)(kbd ? (getenv("HV_KBD_PRODUCT") ? atoi(getenv("HV_KBD_PRODUCT")) : 1) : tablet ? 3 : 2);
        uint16_t ids[4] = {0x06, 0x0627, prod, 1};
        memcpy(data, ids, sizeof ids);
        size = sizeof ids;
        break;
    }
    case CFG_PROP_BITS:
        if (!kbd) {
            data[0] = 1; /* INPUT_PROP_POINTER */
            size = 1;
        }
        break;
    case CFG_EV_BITS:
        if (kbd && subsel == HV_EV_KEY) {
            for (unsigned k = 1; k < 0x100; k++) set_bit(data, k); /* every key, no buttons */
            size = 0x100 / 8;
        } else if (!kbd && subsel == HV_EV_KEY) {
            set_bit(data, HV_BTN_LEFT);
            set_bit(data, HV_BTN_RIGHT);
            set_bit(data, HV_BTN_MIDDLE);
            size = HV_BTN_MIDDLE / 8 + 1;
        } else if (tablet && subsel == HV_EV_ABS) {
            set_bit(data, HV_ABS_X);
            set_bit(data, HV_ABS_Y);
            size = 1;
        } else if (!kbd && !tablet && subsel == HV_EV_REL) {
            /* Relative X and Y are what make macOS call it a mouse: the tablet has none, and
               no wheel either (the mouse scrolls, in both directions). */
            set_bit(data, HV_REL_X);
            set_bit(data, HV_REL_Y);
            set_bit(data, HV_REL_HWHEEL);
            set_bit(data, HV_REL_WHEEL);
            size = 2;
        }
        break;
    case CFG_ABS_INFO: /* struct virtio_input_absinfo: min, max, fuzz, flat, res */
        if (tablet && (subsel == HV_ABS_X || subsel == HV_ABS_Y)) {
            uint32_t info[5] = {0, HV_TABLET_MAX, 0, 0, 0};
            memcpy(data, info, sizeof info);
            size = sizeof info;
            if (!d->asked_range) fprintf(stderr, "virtio-input tablet: the guest's trackpad driver took it\n");
            d->asked_range = 1;
        }
        break;
    default:
        break;
    }
    c[2] = (uint8_t)size;
    if (getenv("HV_INPUT_LOG")) /* debugging: what the guest driver asks */
        fprintf(stderr, "virtio-input %s: select %#x subsel %u -> size %u\n", kind_name(d->kind), select, subsel, size);
}

static void config_write(HvVirtio *v, uint32_t off, uint64_t val, unsigned size) {
    HvVirtioInput *d = v->dev;
    for (unsigned i = 0; i < size && off + i < 2; i++) v->config[off + i] = (uint8_t)(val >> (8 * i));
    answer(d);
}

/* The status queue carries LED changes: hand the buffers straight back. */
static void notify(HvVirtio *v, int q) {
    if (getenv("HV_INPUT_LOG")) fprintf(stderr, "virtio-input %s: notify queue %d\n", kind_name(((HvVirtioInput *)v->dev)->kind), q);
    if (q != STATUSQ) return; /* new event buffers: delivered at the next poll */
    static HvVirtqElem e;
    int any = 0;
    while (hv_virtq_pop(v, q, &e) > 0) {
        hv_virtq_push(v, q, &e, 0);
        any = 1;
    }
    if (any) hv_virtio_interrupt(v);
}

void hv_virtio_input_send(HvVirtioInput *d, uint16_t type, uint16_t code, int32_t value) {
    if (!d) return;
    pthread_mutex_lock(&d->mu);
    if (d->tail - d->head == HV_VINPUT_RING) {
        d->head++; /* full: lose the oldest */
        d->dropped++;
    }
    uint32_t i = d->tail++ % HV_VINPUT_RING;
    d->ring[i].type = type;
    d->ring[i].code = code;
    d->ring[i].value = (uint32_t)value;
    pthread_mutex_unlock(&d->mu);
    hv_machine_wake(d->vio.m);
}

void hv_virtio_input_poll(HvVirtioInput *d) {
    if (!d) return;
    HvVirtio *v = &d->vio;
    if (!(v->status & 4) || !v->q[EVENTQ].enabled) return; /* DRIVER_OK */
    static HvVirtqElem e;
    int any = 0;
    pthread_mutex_lock(&d->mu);
    while (d->head != d->tail) {
        if (hv_virtq_pop(v, EVENTQ, &e) <= 0) break; /* no buffer posted: keep it for later */
        uint32_t i = d->head++ % HV_VINPUT_RING;
        uint8_t ev[8];
        memcpy(ev, &d->ring[i].type, 2);
        memcpy(ev + 2, &d->ring[i].code, 2);
        memcpy(ev + 4, &d->ring[i].value, 4);
        hv_virtq_write(v, &e, 0, ev, 8);
        hv_virtq_push(v, EVENTQ, &e, 8);
        d->delivered++;
        uint16_t code = d->ring[i].code;
        if (d->ring[i].type == HV_EV_KEY && code < HV_VINPUT_KEYS && d->ring[i].value != 2) { /* 2: a repeat */
            uint8_t bit = (uint8_t)(1u << (code % 8));
            d->down[code / 8] = d->ring[i].value ? d->down[code / 8] | bit : d->down[code / 8] & (uint8_t)~bit;
        }
        any = 1;
    }
    pthread_mutex_unlock(&d->mu);
    if (any) hv_virtio_interrupt(v);
}

void hv_virtio_input_release_all(HvVirtioInput *d) {
    if (!d) return;
    /* What the guest will have been told once the events still waiting reach it: the releases go after them */
    uint8_t down[HV_VINPUT_KEYS / 8];
    pthread_mutex_lock(&d->mu);
    memcpy(down, d->down, sizeof down);
    for (uint32_t h = d->head; h != d->tail; h++) {
        uint32_t i = h % HV_VINPUT_RING;
        uint16_t code = d->ring[i].code;
        if (d->ring[i].type != HV_EV_KEY || code >= HV_VINPUT_KEYS || d->ring[i].value == 2) continue;
        uint8_t bit = (uint8_t)(1u << (code % 8));
        down[code / 8] = d->ring[i].value ? down[code / 8] | bit : down[code / 8] & (uint8_t)~bit;
    }
    pthread_mutex_unlock(&d->mu);
    int any = 0;
    for (unsigned code = 0; code < HV_VINPUT_KEYS; code++)
        if (down[code / 8] & (1u << (code % 8))) hv_virtio_input_send(d, HV_EV_KEY, (uint16_t)code, 0), any = 1;
    if (any) hv_virtio_input_send(d, HV_EV_SYN, 0, 0);
}

int hv_virtio_input_ready(HvVirtioInput *d) {
    return d && (d->vio.status & 4) && d->vio.q[EVENTQ].enabled;
}

HvVirtioInput *hv_virtio_input_create(HvMachine *m, HvPcie *bus, int kind) {
    HvVirtioInput *d = calloc(1, sizeof *d);
    HvVirtio *v = &d->vio;
    hv_virtio_init(v, m, 18, 0x098000, 2, 0); /* input device, "other" subclass */
    v->dev = d;
    v->notify = notify;
    v->config_write = config_write;
    v->config_len = 8 + 128;
    d->kind = kind;
    pthread_mutex_init(&d->mu, NULL);
    answer(d);
    hv_pcie_attach(bus, &v->pci);
    return d;
}
