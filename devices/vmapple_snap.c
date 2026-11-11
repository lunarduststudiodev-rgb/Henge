/* Suspend: the virtual Mac's devices in a snapshot (core/snapshot.h). Apart from the devices
   themselves so the engine library links without the snapshot code until something uses it.

   Every device is its own section, field by field. What is saved is what the guest can see or
   depends on: registers, queues, interrupt lines, what a device holds for the guest (UART input,
   input events, audio not yet played). Not saved: host things a new process makes again (threads,
   locks, files, the audio and network back ends), and statistics only logs read.

   Work under way is finished before saving rather than saved: disk and shared-folder requests the
   workers have are completed and handed to the guest (quiesce), so a snapshot never holds a request
   half done. States that cannot be saved yet are refused up front (hv_vmapple_snap_can_save). */
#include "vmapple.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#include "pvg_lite.h"
#include "usb_storage.h"
#include "virtio_fs_internal.h"
#include "ramfile.h"
#include "snapshot.h"

#define TAG HV_SNAP_TAG
#define S_BOARD TAG('B', 'O', 'R', 'D')
#define S_UART TAG('U', 'A', 'R', 'T')
#define S_RTC TAG('P', 'L', '3', '1')
#define S_GPIO TAG('G', 'P', 'I', 'O')
#define S_AES TAG('A', 'E', 'S', ' ')
#define S_BDIF TAG('B', 'D', 'I', 'F')
#define S_PCIE TAG('P', 'C', 'I', 'E')
#define S_BLK_ROOT TAG('V', 'B', 'K', 'R')
#define S_BLK_AUX TAG('V', 'B', 'K', 'A')
#define S_NET TAG('V', 'N', 'E', 'T')
#define S_KBD TAG('V', 'K', 'B', 'D')
#define S_MOUSE TAG('V', 'M', 'O', 'U')
#define S_TABLET TAG('V', 'T', 'A', 'B')
#define S_SND TAG('V', 'S', 'N', 'D')
#define S_XHCI TAG('X', 'H', 'C', 'I')
#define S_UTCH TAG('U', 'T', 'C', 'H') /* the USB controller's touchscreen port: newer than XHCI, so optional */
#define S_UTCF TAG('U', 'T', 'C', 'F') /* the fingers macOS has down on it: newer than UTCH, so optional */
#define S_USPK TAG('U', 'S', 'P', 'K') /* the USB controller's speaker port: newer than UTCH, so optional */
#define S_UVID TAG('U', 'V', 'I', 'D') /* the USB controller's camera port: newer than UTCH, so optional */
#define S_SNDI TAG('V', 'S', 'N', 'I') /* the sound device's microphone stream: newer than VSND, so optional */
#define S_DRVS TAG('D', 'R', 'V', 'S') /* the USB drives again, alone: checked before anything is loaded (optional) */
#define S_FS TAG('V', 'F', 'S', ' ')
#define S_PVG TAG('P', 'V', 'G', ' ')
#define S_PVGM TAG('P', 'V', 'G', 'M') /* what only the GPU holds: streamed, any size; with a version 2 PVG */
/* The mapper's state (the ids the guest unmapped): newer than PVG, so optional, and never a reason to refuse a resume
   (load_pvg); not in sections_fit. An older Henge passes over it. */
#define S_PVGS TAG('P', 'V', 'G', 'S')
#define S_POWER TAG('P', 'W', 'R', 'S') /* the virtual battery: only when the board has one */
#define S_BALLOON TAG('B', 'A', 'L', 'N') /* the memory balloon: only when the board has one (HV_BALLOON) */

#define OUT(f) HV_SNAP_OUT(o, f)
#define IN(f) HV_SNAP_IN(in, f)

static void say(const char *fmt, const char *name) {
    fprintf(stderr, "hengevm: snapshot: ");
    fprintf(stderr, fmt, name);
    fputc('\n', stderr);
}

/* A section that must be there (the device is) or must not (it isn't). */
static int get_sec(HvSnapReader *r, uint32_t tag, int present, const char *name, HvSnapIn *in) {
    int got = hv_snap_get(r, tag, 1, in) == 0;
    if (got == present) return got ? 1 : 0;
    say(present ? "the saved state has no %s (it is from another version of Henge, or a Mac with other devices)"
                : "the saved Mac had a %s this one does not",
        name);
    return -1;
}

static int done(HvSnapIn *in, const char *name) {
    if (hv_snap_in_done(in) == 0) return 0;
    if (in->bad != HV_SNAP_REFUSED) say("the %s section is damaged", name); /* else the reason is already out */
    return -1;
}

/* ---- the board's own registers ---- */

/* A register window as (offset, value) pairs of its nonzero 64-bit words. */
static void put_sparse(HvSnapOut *o, const uint8_t *regs, uint32_t len) {
    uint32_t n = 0;
    for (uint32_t off = 0; off < len; off += 8) {
        uint64_t v;
        memcpy(&v, regs + off, 8);
        n += v != 0;
    }
    OUT(n);
    for (uint32_t off = 0; off < len; off += 8) {
        uint64_t v;
        memcpy(&v, regs + off, 8);
        if (!v) continue;
        OUT(off);
        OUT(v);
    }
}

static void get_sparse(HvSnapIn *in, uint8_t *regs, uint32_t len) {
    uint32_t n = 0;
    IN(n);
    if (n > len / 8) in->bad = 1;
    if (in->bad) return;
    memset(regs, 0, len);
    for (uint32_t i = 0; i < n && !in->bad; i++) {
        uint32_t off = 0;
        uint64_t v = 0;
        IN(off);
        IN(v);
        if (off >= len || off % 8) in->bad = 1;
        else memcpy(regs + off, &v, 8);
    }
}

static void save_board(HvVmapple *b, HvSnapOut *o) {
    pthread_mutex_lock(&b->avp_rtc_lock);
    for (int i = 0; i < 8; i++) OUT(b->avp_rtc[i]);
    pthread_mutex_unlock(&b->avp_rtc_lock);
    OUT(b->panic_events);
    put_sparse(o, b->avp_ctrr, VMAPPLE_AVP_REGS);
    put_sparse(o, b->avp_sealed, VMAPPLE_AVP_REGS);
}

static void load_board(HvVmapple *b, HvSnapIn *in) {
    pthread_mutex_lock(&b->avp_rtc_lock);
    for (int i = 0; i < 8; i++) IN(b->avp_rtc[i]);
    pthread_mutex_unlock(&b->avp_rtc_lock);
    IN(b->panic_events);
    get_sparse(in, b->avp_ctrr, VMAPPLE_AVP_REGS);
    get_sparse(in, b->avp_sealed, VMAPPLE_AVP_REGS);
}

/* ---- PL011, PL031, PL061, AES, bdif ---- */

#define UART_FIELDS(X) X(cr) X(lcr) X(ibrd) X(fbrd) X(ifls) X(imsc) X(ris) X(dmacr) X(rx) X(rx_head) X(rx_count)
#define RTC_FIELDS(X) X(offset) X(mr) X(imsc) X(ris)
#define GPIO_FIELDS(X) X(data) X(dir) X(isense) X(ibe) X(iev) X(im) X(istate) X(afsel) X(input) X(old_in)
#define AES_FIELDS(X) \
    X(status) X(q_status) X(irq_status) X(irq_enable) X(watermark) X(flag_info) X(fifo) X(fifo_idx) X(iv) X(encrypt) X(cbc)

static void save_uart(HvPl011 *u, HvSnapOut *o) {
#define F(f) OUT(u->f);
    UART_FIELDS(F)
#undef F
}
static void load_uart(HvPl011 *u, HvSnapIn *in) {
#define F(f) IN(u->f);
    UART_FIELDS(F)
#undef F
    if (u->rx_head < 0 || u->rx_head >= (int)sizeof u->rx || u->rx_count < 0 || u->rx_count > (int)sizeof u->rx) in->bad = 1;
}

static void save_rtc(HvPl031 *r, HvSnapOut *o) {
#define F(f) OUT(r->f);
    RTC_FIELDS(F)
#undef F
}
static void load_rtc(HvPl031 *r, HvSnapIn *in) {
#define F(f) IN(r->f);
    RTC_FIELDS(F)
#undef F
}

static void save_gpio(HvPl061 *g, HvSnapOut *o) {
#define F(f) OUT(g->f);
    GPIO_FIELDS(F)
#undef F
}
static void load_gpio(HvPl061 *g, HvSnapIn *in) {
#define F(f) IN(g->f);
    GPIO_FIELDS(F)
#undef F
}

static void save_aes(HvAes *a, HvSnapOut *o) {
#define F(f) OUT(a->f);
    AES_FIELDS(F)
#undef F
    for (int i = 0; i < 2; i++) {
        OUT(a->key[i].len);
        OUT(a->key[i].key);
    }
}
static void load_aes(HvAes *a, HvSnapIn *in) {
#define F(f) IN(a->f);
    AES_FIELDS(F)
#undef F
    for (int i = 0; i < 2; i++) {
        IN(a->key[i].len);
        IN(a->key[i].key);
    }
    if (a->fifo_idx > 9) in->bad = 1;
}

static void save_bdif(HvBdif *d, HvSnapOut *o) {
    OUT(d->usb_qsel);
    OUT(d->usb_irq);
    for (int i = 0; i < 2; i++) {
        OUT(d->usb_q[i].pending);
        OUT(d->usb_q[i].done);
    }
}
static void load_bdif(HvBdif *d, HvSnapIn *in) {
    IN(d->usb_qsel);
    IN(d->usb_irq);
    for (int i = 0; i < 2; i++) {
        IN(d->usb_q[i].pending);
        IN(d->usb_q[i].done);
    }
}

/* ---- PCI: every slot's configuration space and INTx level ---- */

/* What says which device is in a slot: vendor, device, class and revision, subsystem. */
static int same_device(const uint8_t *a, const uint8_t *b) {
    return !memcmp(a, b, 4) && !memcmp(a + 8, b + 8, 4) && !memcmp(a + 0x2c, b + 0x2c, 4);
}

static void save_pcie(HvPcie *p, HvSnapOut *o) {
    uint32_t slots = HV_PCI_SLOTS;
    OUT(slots);
    for (int s = 0; s < HV_PCI_SLOTS; s++) {
        HvPciDev *d = p->dev[s];
        uint8_t present = d != NULL;
        OUT(present);
        if (!d) continue;
        OUT(d->config);
        int32_t intx = d->intx;
        OUT(intx);
    }
}

static int load_pcie(HvPcie *p, HvSnapIn *in) {
    uint32_t slots = 0;
    IN(slots);
    if (in->bad || slots != HV_PCI_SLOTS) return in->bad = 1, 0;
    static uint8_t config[4096];
    for (int s = 0; s < HV_PCI_SLOTS && !in->bad; s++) {
        HvPciDev *d = p->dev[s];
        uint8_t present = 0;
        IN(present);
        if (in->bad) break;
        if (present != (d != NULL)) {
            fprintf(stderr, "hengevm: snapshot: PCI slot %d %s\n", s, present ? "was a device and is empty now" : "is a device the saved Mac did not have");
            return -1;
        }
        if (!present) continue;
        int32_t intx = 0;
        IN(config);
        IN(intx);
        if (in->bad) break;
        if (!same_device(config, d->config)) {
            fprintf(stderr, "hengevm: snapshot: PCI slot %d holds another device (%02x%02x:%02x%02x, saved %02x%02x:%02x%02x)\n", s,
                    d->config[1], d->config[0], d->config[3], d->config[2], config[1], config[0], config[3], config[2]);
            return -1;
        }
        memcpy(d->config, config, sizeof config);
        d->intx = intx != 0;
    }
    /* The lines are each the OR of their devices' (their levels are in the GIC's section). */
    memset(p->line_count, 0, sizeof p->line_count);
    for (int s = 0; s < HV_PCI_SLOTS; s++)
        if (p->dev[s] && p->dev[s]->intx) p->line_count[p->dev[s]->slot]++;
    return 0;
}

/* ---- virtio: the transport, then each device ---- */

static void save_virtio(HvVirtio *v, HvSnapOut *o) {
    OUT(v->features);
    OUT(v->driver_features);
    OUT(v->dfsel);
    OUT(v->gfsel);
    OUT(v->status);
    OUT(v->isr);
    OUT(v->config_gen);
    OUT(v->qsel);
    OUT(v->nq);
    for (int i = 0; i < v->nq; i++) {
        HvVirtq *q = &v->q[i];
        OUT(q->size);
        OUT(q->enabled);
        OUT(q->last_avail);
        OUT(q->desc);
        OUT(q->avail);
        OUT(q->used);
        uint8_t out[HV_VIRTQ_MAX_SIZE / 8] = {0}; /* heads the device holds, one bit each */
        for (int h = 0; h < HV_VIRTQ_MAX_SIZE; h++)
            if (q->out[h]) out[h / 8] |= (uint8_t)(1u << (h % 8));
        OUT(out);
    }
    OUT(v->config_len);
    OUT(v->config);
}

static void load_virtio(HvVirtio *v, HvSnapIn *in, const char *name) {
    uint64_t features = 0;
    uint16_t nq = 0;
    IN(features);
    IN(v->driver_features);
    IN(v->dfsel);
    IN(v->gfsel);
    IN(v->status);
    IN(v->isr);
    IN(v->config_gen);
    IN(v->qsel);
    IN(nq);
    if (in->bad) return;
    if (nq != v->nq || (v->driver_features & ~v->features)) {
        /* The driver agreed on features or queues this device does not have: it cannot carry on. */
        fprintf(stderr, "hengevm: snapshot: the %s was saved with %u queues and features %llx, this one has %u and %llx\n", name, nq,
                (unsigned long long)v->driver_features, v->nq, (unsigned long long)v->features);
        in->bad = HV_SNAP_REFUSED;
        return;
    }
    for (int i = 0; i < nq; i++) {
        HvVirtq *q = &v->q[i];
        IN(q->size);
        IN(q->enabled);
        IN(q->last_avail);
        IN(q->desc);
        IN(q->avail);
        IN(q->used);
        uint8_t out[HV_VIRTQ_MAX_SIZE / 8];
        IN(out);
        for (int h = 0; h < HV_VIRTQ_MAX_SIZE; h++) q->out[h] = (out[h / 8] >> (h % 8)) & 1;
        if (!q->size || q->size > HV_VIRTQ_MAX_SIZE || (q->size & (q->size - 1))) in->bad = 1;
    }
    IN(v->config_len);
    IN(v->config);
    if (v->config_len > sizeof v->config) in->bad = 1;
}

/* An element a device holds (sound): only the segments it has. */
static void save_elem(HvSnapOut *o, const HvVirtqElem *e) {
    OUT(e->head);
    int32_t n = e->nseg;
    OUT(n);
    for (int i = 0; i < e->nseg; i++) {
        OUT(e->seg[i].addr);
        OUT(e->seg[i].len);
        int32_t w = e->seg[i].write;
        OUT(w);
    }
}

static void load_elem(HvSnapIn *in, HvVirtqElem *e) {
    int32_t n = 0;
    IN(e->head);
    IN(n);
    if (in->bad || n < 0 || n > HV_VIRTQ_MAX_SEGS) {
        in->bad = 1;
        return;
    }
    e->nseg = n;
    for (int i = 0; i < n; i++) {
        int32_t w = 0;
        IN(e->seg[i].addr);
        IN(e->seg[i].len);
        IN(w);
        e->seg[i].write = w != 0;
    }
}

static void save_blk(HvVirtioBlk *d, HvSnapOut *o) {
    save_virtio(&d->vio, o);
    OUT(d->disk->size);
    OUT(d->requests);
    OUT(d->errors);
}

static int load_blk(HvVirtioBlk *d, HvSnapIn *in, const char *name) {
    if (d->submitted != d->published) {
        say("the %s is busy (loading needs it idle)", name);
        return -1;
    }
    uint64_t size = 0;
    load_virtio(&d->vio, in, name);
    IN(size);
    IN(d->requests);
    IN(d->errors);
    if (!in->bad && size != d->disk->size) {
        fprintf(stderr, "hengevm: snapshot: the %s was %llu bytes and is %llu now: not the disk it was saved with\n", name,
                (unsigned long long)size, (unsigned long long)d->disk->size);
        return -1;
    }
    return 0;
}

static void save_net(HvVirtioNet *d, HvSnapOut *o) {
    save_virtio(&d->vio, o);
    OUT(d->tx_frames);
    OUT(d->rx_frames);
    OUT(d->rx_dropped);
}

static void load_net(HvVirtioNet *d, HvSnapIn *in) {
    load_virtio(&d->vio, in, "network");
    IN(d->tx_frames);
    IN(d->rx_frames);
    IN(d->rx_dropped);
}

static void save_input(HvVirtioInput *d, HvSnapOut *o) {
    save_virtio(&d->vio, o);
    int32_t kind = d->kind, asked = d->asked_range;
    OUT(kind);
    OUT(asked);
    OUT(d->down);
    OUT(d->delivered);
    OUT(d->dropped);
    pthread_mutex_lock(&d->mu);
    uint32_t n = d->tail - d->head; /* events not yet delivered */
    OUT(n);
    for (uint32_t i = d->head; i != d->tail; i++) {
        OUT(d->ring[i % HV_VINPUT_RING].type);
        OUT(d->ring[i % HV_VINPUT_RING].code);
        OUT(d->ring[i % HV_VINPUT_RING].value);
    }
    pthread_mutex_unlock(&d->mu);
}

static void load_input(HvVirtioInput *d, HvSnapIn *in, const char *name) {
    load_virtio(&d->vio, in, name);
    int32_t kind = 0, asked = 0;
    uint32_t n = 0;
    IN(kind);
    IN(asked);
    IN(d->down);
    IN(d->delivered);
    IN(d->dropped);
    IN(n);
    if (in->bad || kind != d->kind || n > HV_VINPUT_RING) {
        in->bad = 1;
        return;
    }
    d->asked_range = asked;
    pthread_mutex_lock(&d->mu);
    d->head = d->tail = 0;
    for (uint32_t i = 0; i < n; i++, d->tail++) {
        IN(d->ring[i].type);
        IN(d->ring[i].code);
        IN(d->ring[i].value);
    }
    pthread_mutex_unlock(&d->mu);
}

/* The power source holds no buffer of the guest's (it answers one only when it has news), so its queue
   and what it last told the guest are all there is. After a load the guest is told the host's state
   again: the battery has moved on while the Mac was saved. */
/* The balloon holds no buffer of the guest's either (each list of pages is answered as it comes): its queues,
   what the guest was last asked for, and the two words the driver reads and writes (asked, given). The pages
   it holds are holes in the Mac's memory, which is saved as it is. */
static void save_balloon(HvVirtioBalloon *d, HvSnapOut *o) {
    save_virtio(&d->vio, o);
    uint64_t words = 0;
    memcpy(&words, d->vio.config, 8);
    OUT(d->applied);
    OUT(words);
}

static void load_balloon(HvVirtioBalloon *d, HvSnapIn *in) {
    load_virtio(&d->vio, in, "memory balloon");
    uint64_t words = 0;
    IN(d->applied);
    IN(words);
    if (in->bad) return;
    memcpy(d->vio.config, &words, 8);
    __atomic_store_n(&d->target, d->applied, __ATOMIC_RELEASE); /* as it was: the host asks again when it needs to */
}

static void save_power(HvPowerSource *d, HvSnapOut *o) {
    save_virtio(&d->vio, o);
    int32_t kind = d->kind;
    OUT(kind);
    OUT(d->sent.percent);
    OUT(d->sent.external);
    OUT(d->sent.charging);
    OUT(d->sent.full);
    OUT(d->messages);
}

static void load_power(HvPowerSource *d, HvSnapIn *in) {
    load_virtio(&d->vio, in, "battery");
    int32_t kind = -1;
    IN(kind);
    IN(d->sent.percent);
    IN(d->sent.external);
    IN(d->sent.charging);
    IN(d->sent.full);
    IN(d->messages);
    if (!in->bad && kind != d->kind) {
        fprintf(stderr, "hengevm: snapshot: the saved Mac had %s, this one has %s\n", kind ? "wall power" : "a battery",
                d->kind ? "wall power" : "a battery");
        in->bad = HV_SNAP_REFUSED;
    }
    d->resend = 1;
}

#define SND_FIELDS(X) \
    X(channels) X(format) X(rate) X(frame_bytes) X(rate_hz) X(buffer_bytes) X(period_bytes) X(prepared) X(running)

static void save_snd(HvVirtioSnd *d, HvSnapOut *o) {
    save_virtio(&d->vio, o);
#define F(f) OUT(d->f);
    SND_FIELDS(F)
#undef F
    /* The audio not yet played, at the same place in the ring, and the buffers waiting on it. */
    uint64_t written = atomic_load(&d->written), consumed = atomic_load(&d->consumed);
    OUT(written);
    OUT(consumed);
    for (uint64_t at = consumed; at < written;) {
        uint32_t i = (uint32_t)(at % HV_VSND_RING), n = (uint32_t)(written - at);
        if (n > HV_VSND_RING - i) n = HV_VSND_RING - i;
        hv_snap_out(o, d->ring + i, n);
        at += n;
    }
    OUT(d->phead);
    OUT(d->pcopy);
    OUT(d->ptail);
    for (uint32_t i = d->phead; i != d->ptail; i++) {
        save_elem(o, &d->pending[i % HV_VSND_PENDING]);
        OUT(d->pending_end[i % HV_VSND_PENDING]);
    }
    OUT(d->underruns);
    OUT(d->xfers);
}

/* The stream's parameters as PCM_SET_PARAMS would have set them, or none set yet. */
static int snd_params_ok(const HvVirtioSnd *d) {
    if (!d->channels && !d->frame_bytes && !d->prepared && !d->running) return 1;
    unsigned fb = d->format == HV_VSND_FMT_S16 ? 2 : 4;
    return d->channels >= 1 && d->channels <= 2 &&
           (d->format == HV_VSND_FMT_S16 || d->format == HV_VSND_FMT_S32 || d->format == HV_VSND_FMT_FLOAT) &&
           ((d->rate == HV_VSND_RATE_44100 && d->rate_hz == 44100) || (d->rate == HV_VSND_RATE_48000 && d->rate_hz == 48000)) &&
           d->frame_bytes == d->channels * fb && (d->prepared || !d->running);
}

/* Sound playing when it was saved starts again only once the whole load has worked (hv_vmapple_restored). */
static void load_snd(HvVmapple *b, HvVirtioSnd *d, HvSnapIn *in) {
    if (d->running && d->backend.stop) d->backend.stop(d->backend.opaque);
    d->running = 0;
    b->snd_restart = 0;
    load_virtio(&d->vio, in, "sound device");
#define F(f) IN(d->f);
    SND_FIELDS(F)
#undef F
    d->prepared = d->prepared != 0;
    d->running = d->running != 0;
    if (!in->bad && !snd_params_ok(d)) in->bad = 1;
    uint64_t written = 0, consumed = 0;
    IN(written);
    IN(consumed);
    if (in->bad || written < consumed || written - consumed > HV_VSND_RING) {
        in->bad = 1;
        return;
    }
    for (uint64_t at = consumed; at < written;) {
        uint32_t i = (uint32_t)(at % HV_VSND_RING), n = (uint32_t)(written - at);
        if (n > HV_VSND_RING - i) n = HV_VSND_RING - i;
        hv_snap_in(in, d->ring + i, n);
        at += n;
    }
    atomic_store(&d->written, written);
    atomic_store(&d->consumed, consumed);
    IN(d->phead);
    IN(d->pcopy);
    IN(d->ptail);
    if (in->bad || d->ptail - d->phead > HV_VSND_PENDING || d->pcopy - d->phead > d->ptail - d->phead) {
        in->bad = 1;
        return;
    }
    uint64_t last = 0;
    for (uint32_t i = d->phead; i != d->ptail; i++) {
        load_elem(in, &d->pending[i % HV_VSND_PENDING]);
        uint64_t end = 0;
        IN(end);
        d->pending_end[i % HV_VSND_PENDING] = end;
        /* each buffer whose audio is in the ring ends after the one before, and within what was written */
        if (i - d->phead < d->pcopy - d->phead && (end < last || end > written)) in->bad = 1;
        last = end;
    }
    IN(d->underruns);
    IN(d->xfers);
    d->clock_start_ns = 0;
    d->clock_start_consumed = consumed;
    /* running stays as saved (the device's own state); the host's audio starts in hv_vmapple_restored */
    if (!in->bad) b->snd_restart = d->running;
}

/* The microphone's stream: its parameters and the capture buffers the guest left with it. The audio
   waiting in its ring is not kept (a resumed Mac hears the microphone from then on). */
#define SND_IN_FIELDS(X) \
    X(channels) X(format) X(rate) X(frame_bytes) X(rate_hz) X(buffer_bytes) X(period_bytes) X(prepared) X(running) \
    X(phead) X(ptail) X(overruns) X(underruns) X(xfers)

static void save_snd_in(HvVirtioSnd *d, HvSnapOut *o) {
    HvVirtioSndIn *c = &d->in;
#define F(f) OUT(c->f);
    SND_IN_FIELDS(F)
#undef F
    for (uint32_t i = c->phead; i != c->ptail; i++) save_elem(o, &c->pending[i % HV_VSND_PENDING]);
}

static int load_snd_in(HvVirtioSnd *d, HvSnapReader *r) {
    HvVirtioSndIn *c = &d->in;
    if (c->capturing && d->backend.cap_stop) d->backend.cap_stop(d->backend.opaque);
    /* idle, as a new device's, keeping its ring */
    uint8_t *ring = c->ring;
    uint64_t w = atomic_load(&c->written);
    memset(c, 0, sizeof *c);
    c->ring = ring;
    atomic_store(&c->written, w);
    atomic_store(&c->read, w);
    HvSnapIn in_;
    HvSnapIn *in = &in_;
    if (hv_snap_get(r, S_SNDI, 1, in) != 0) return 0; /* from before the microphone: it is idle */
#define F(f) IN(c->f);
    SND_IN_FIELDS(F)
#undef F
    c->prepared = c->prepared != 0;
    c->running = c->running != 0;
    unsigned fb = c->format == HV_VSND_FMT_S16 ? 2 : 4;
    int idle = !c->channels && !c->frame_bytes && !c->prepared && !c->running;
    int ok = idle || (c->channels >= 1 && c->channels <= 2 &&
                      (c->format == HV_VSND_FMT_S16 || c->format == HV_VSND_FMT_S32 || c->format == HV_VSND_FMT_FLOAT) &&
                      ((c->rate == HV_VSND_RATE_44100 && c->rate_hz == 44100) || (c->rate == HV_VSND_RATE_48000 && c->rate_hz == 48000)) &&
                      c->frame_bytes == c->channels * fb && (c->prepared || !c->running));
    /* buffers only while prepared, and only on a device that told the guest of the microphone */
    if (c->ptail - c->phead > HV_VSND_PENDING || (c->ptail != c->phead && !c->prepared) ||
        (!idle && hv_virtio_snd_streams(d) < 2))
        ok = 0;
    if (!in->bad && !ok) say("the %s section makes no sense", "microphone");
    if (ok && !in->bad)
        for (uint32_t i = c->phead; i != c->ptail; i++) load_elem(in, &c->pending[i % HV_VSND_PENDING]);
    if (!ok || done(in, "microphone") != 0 || (c->prepared && !c->ring && !(c->ring = calloc(1, HV_VSND_RING)))) {
        c->running = c->prepared = 0; /* idle again */
        c->phead = c->ptail = 0;
        return -1;
    }
    c->restart = c->running; /* the host's capture starts again once the whole load has worked */
    return 0;
}

static void snd_restart(HvVmapple *b) {
    HvVirtioSnd *d = b->snd;
    hv_virtio_snd_capture_restart(d);
    if (!d || !b->snd_restart) return;
    b->snd_restart = 0;
    /* It was playing: play on from where it was. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    d->clock_start_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    d->clock_start_consumed = atomic_load(&d->consumed);
    if (d->backend.start && d->backend.start(d->backend.opaque, d->channels, d->format, d->rate_hz) != 0)
        d->backend.start = NULL; /* as PCM_START does: drain by the clock instead */
}

/* ---- xHCI: the USB keyboard and the drives plugged in ---- */

static void put_str(HvSnapOut *o, const char *str) {
    uint16_t n = (uint16_t)strlen(str);
    OUT(n);
    hv_snap_out(o, str, n);
}

static int get_str(HvSnapIn *in, char *buf, size_t cap) {
    uint16_t n = 0;
    IN(n);
    if (in->bad || n >= cap) return in->bad = 1, -1;
    hv_snap_in(in, buf, n);
    buf[n] = 0;
    return in->bad ? -1 : 0;
}

static void put_ring(HvSnapOut *o, const HvXhciRing *r) {
    OUT(r->dequeue);
    int32_t ccs = r->ccs;
    OUT(ccs);
}
static void get_ring(HvSnapIn *in, HvXhciRing *r) {
    int32_t ccs = 0;
    IN(r->dequeue);
    IN(ccs);
    r->ccs = ccs != 0;
}

static void save_xhci(HvXhci *x, HvSnapOut *o) {
    uint32_t ports = HV_XHCI_PORTS, intrs = HV_XHCI_INTRS, slots = HV_XHCI_SLOTS, eps = HV_XHCI_EPS;
    OUT(ports);
    OUT(intrs);
    OUT(slots);
    OUT(eps);
    OUT(x->usbcmd);
    OUT(x->usbsts);
    OUT(x->dnctrl);
    OUT(x->config);
    OUT(x->dcbaap);
    put_ring(o, &x->cmd);
    int32_t v = x->cmd_running;
    OUT(v);
    OUT(x->portsc);
    for (int i = 0; i < HV_XHCI_INTRS; i++) {
        struct HvXhciIntr *it = &x->intr[i];
        OUT(it->iman);
        OUT(it->imod);
        OUT(it->erstsz);
        OUT(it->erstba);
        OUT(it->erdp);
        OUT(it->seg_base);
        OUT(it->seg_size);
        OUT(it->seg_index);
        OUT(it->index);
        v = it->pcs;
        OUT(v);
    }
    for (int s = 0; s <= HV_XHCI_SLOTS; s++) {
        HvXhciSlot *sl = &x->slots[s];
        v = x->slot_used[s];
        OUT(v);
        v = sl->port;
        OUT(v);
        for (int e = 0; e < HV_XHCI_EPS; e++) {
            put_ring(o, &sl->ep[e]);
            v = sl->ep_running[e];
            OUT(v);
        }
    }
    v = x->kbd_slot;
    OUT(v);
    v = x->configured;
    OUT(v);
    pthread_mutex_lock(&x->mu);
    OUT(x->state);
    OUT(x->delivered);
    uint32_t n = x->rtail - x->rhead;
    OUT(n);
    for (uint32_t i = x->rhead; i != x->rtail; i++) hv_snap_out(o, x->reports[i % HV_XHCI_REPORTS], 8);
    /* The drives (settled: none busy). Which file each is, is the host's: it plugs them in again before a load. */
    for (int p = 1; p < HV_XHCI_PORTS; p++) {
        struct HvUsbStorage *d = x->port[p].drive;
        v = d != NULL;
        OUT(v);
        if (!d) continue;
        HvUsbStorageState st;
        int64_t mtime = 0;
        if (hv_usb_storage_get_state(d, &st) != 0 || hv_usb_storage_mtime(d, &mtime) != 0) o->bad = 1;
        v = x->port[p].slot;
        OUT(v);
        put_str(o, hv_usb_storage_serial(d));
        put_str(o, hv_usb_storage_name(d));
        uint64_t size = hv_usb_storage_size(d);
        OUT(size);
        v = hv_usb_storage_readonly(d);
        OUT(v);
        OUT(mtime);
        OUT(st.configured);
        OUT(st.ejected);
        OUT(st.state);
        OUT(st.host_in);
        OUT(st.io);
        OUT(st.tag);
        OUT(st.host_len);
        OUT(st.host_done);
        OUT(st.resp_pos);
        OUT(st.disk_off);
        OUT(st.dev_left);
        OUT(st.status);
        OUT(st.sense_key);
        OUT(st.asc);
        OUT(st.ascq);
        OUT(st.resp);
    }
    pthread_mutex_unlock(&x->mu);
}

static void load_xhci(HvXhci *x, HvSnapIn *in) {
    uint32_t ports = 0, intrs = 0, slots = 0, eps = 0;
    IN(ports);
    IN(intrs);
    IN(slots);
    IN(eps);
    if (in->bad || ports != HV_XHCI_PORTS || intrs != HV_XHCI_INTRS || slots != HV_XHCI_SLOTS || eps != HV_XHCI_EPS) {
        in->bad = 1;
        return;
    }
    int32_t v = 0;
    IN(x->usbcmd);
    IN(x->usbsts);
    IN(x->dnctrl);
    IN(x->config);
    IN(x->dcbaap);
    get_ring(in, &x->cmd);
    IN(v);
    x->cmd_running = v != 0;
    IN(x->portsc);
    for (int i = 0; i < HV_XHCI_INTRS; i++) {
        struct HvXhciIntr *it = &x->intr[i];
        IN(it->iman);
        IN(it->imod);
        IN(it->erstsz);
        IN(it->erstba);
        IN(it->erdp);
        IN(it->seg_base);
        IN(it->seg_size);
        IN(it->seg_index);
        IN(it->index);
        IN(v);
        it->pcs = v != 0;
        /* The next event goes at index: past its segment it would never wrap, and run on through guest RAM */
        if (it->seg_size ? it->index >= it->seg_size : it->index != 0) in->bad = 1;
    }
    for (int s = 0; s <= HV_XHCI_SLOTS; s++) {
        HvXhciSlot *sl = &x->slots[s];
        memset(sl, 0, sizeof *sl);
        IN(v);
        x->slot_used[s] = v;
        IN(v);
        sl->port = v;
        if (v < 0 || v > HV_XHCI_ROOT_PORTS) in->bad = 1; /* it indexes the ports (6 and 7: load_touch and load_speaker check them) */
        for (int e = 0; e < HV_XHCI_EPS; e++) {
            get_ring(in, &sl->ep[e]);
            IN(v);
            sl->ep_running[e] = v != 0;
        }
    }
    IN(v);
    x->kbd_slot = v;
    IN(v);
    x->configured = v;
    uint32_t n = 0;
    pthread_mutex_lock(&x->mu);
    IN(x->state);
    IN(x->delivered);
    IN(n);
    if (in->bad || n > HV_XHCI_REPORTS || x->kbd_slot < 0 || x->kbd_slot > HV_XHCI_SLOTS) {
        in->bad = 1;
        n = 0;
    }
    x->rhead = x->rtail = 0;
    for (uint32_t i = 0; i < n; i++, x->rtail++) hv_snap_in(in, x->reports[i], 8);
    /* Each drive the saved Mac had: the one the host plugged in with its serial number, the same file as it was. */
    for (int p = 1; p < HV_XHCI_PORTS && !in->bad; p++) {
        IN(v);
        if (!v) continue;
        char serial[64], name[64];
        uint64_t size = 0;
        int64_t mtime = 0;
        int32_t slot = 0, ro = 0;
        HvUsbStorageState st;
        memset(&st, 0, sizeof st);
        IN(slot);
        get_str(in, serial, sizeof serial);
        get_str(in, name, sizeof name);
        IN(size);
        IN(ro);
        IN(mtime);
        IN(st.configured);
        IN(st.ejected);
        IN(st.state);
        IN(st.host_in);
        IN(st.io);
        IN(st.tag);
        IN(st.host_len);
        IN(st.host_done);
        IN(st.resp_pos);
        IN(st.disk_off);
        IN(st.dev_left);
        IN(st.status);
        IN(st.sense_key);
        IN(st.asc);
        IN(st.ascq);
        IN(st.resp);
        if (in->bad) break;
        if (slot < 0 || slot > HV_XHCI_SLOTS || (slot && x->slots[slot].port != p + 1) || x->port[p].drive) {
            in->bad = 1;
            break;
        }
        int q = 1;
        while (q < HV_XHCI_PORTS && !(x->port[q].plug && !strcmp(hv_usb_storage_serial(x->port[q].plug), serial))) q++;
        struct HvUsbStorage *d = q < HV_XHCI_PORTS ? x->port[q].plug : NULL;
        int64_t now = -1;
        const char *no = NULL;
        if (!d) no = "isn't plugged in (plug it in again, then resume)";
        else if (hv_usb_storage_size(d) != size) no = "is another size now: not the drive it was saved with";
        else if (hv_usb_storage_readonly(d) != (ro != 0)) no = ro ? "was read-only and is not now" : "was writable and is read-only now";
        else if (hv_usb_storage_mtime(d, &now) != 0 || now != mtime) no = "has changed since it was saved";
        else if (hv_usb_storage_set_state(d, &st) != 0) no = "was saved in a state that makes no sense";
        if (no) {
            fprintf(stderr, "hengevm: snapshot: the USB drive \"%s\" (%s) %s\n", name, serial, no);
            in->bad = HV_SNAP_REFUSED;
            break;
        }
        x->port[q].plug = NULL;
        x->port[p].drive = d;
        x->port[p].slot = slot;
    }
    /* Drives plugged in that the saved Mac didn't have arrive as new ones, each on a port that is free. */
    for (int q = 1; q < HV_XHCI_PORTS && !in->bad; q++) {
        if (!x->port[q].plug || !x->port[q].drive) continue;
        int p = 1;
        while (p < HV_XHCI_PORTS && (x->port[p].drive || x->port[p].plug)) p++;
        if (p < HV_XHCI_PORTS) {
            x->port[p].plug = x->port[q].plug;
        } else {
            fprintf(stderr, "hengevm: snapshot: no free USB port for the drive \"%s\"\n", hv_usb_storage_name(x->port[q].plug));
            hv_usb_storage_free(x->port[q].plug);
        }
        x->port[q].plug = NULL;
    }
    for (int q = 1; q < HV_XHCI_PORTS; q++)
        if (x->port[q].plug) __atomic_store_n(&x->changes, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&x->mu);
}

/* The touchscreen's part, beside XHCI (whose layout older snapshots fix): always written, so a snapshot without
   it is one from before the touchscreen, whose Mac's driver knows five ports. */
static void save_touch(HvXhci *x, HvSnapOut *o) {
    HvXhciTouchSnap t;
    hv_xhci_touch_get_snap(x, &t);
    OUT(t.nports);
    OUT(t.present);
    OUT(t.w_mm);
    OUT(t.h_mm);
    OUT(t.slot);
    OUT(t.configured);
    OUT(t.portsc);
}

/* The fingers macOS had down on the touchscreen, and where: a resumed Mac lifts them where they are. */
static void save_touch_fingers(HvXhci *x, HvSnapOut *o) {
    HvXhciTouchSnap t;
    hv_xhci_touch_get_snap(x, &t);
    uint32_t down = t.mac_down | (t.fingers_known ? 0 : 1u << 31); /* the top bit: where they are isn't known */
    OUT(down);
    for (int i = 0; i < HV_XHCI_CONTACTS; i++) OUT(t.mac_x[i]);
    for (int i = 0; i < HV_XHCI_CONTACTS; i++) OUT(t.mac_y[i]);
}

static int load_touch(HvXhci *x, HvSnapReader *r) {
    HvSnapIn in_;
    HvSnapIn *in = &in_;
    if (hv_snap_get(r, S_UTCH, 1, in) != 0) {
        if (hv_xhci_touch_set_snap(x, NULL) == 0) return 0;
        say("the %s has a device on a port it didn't have", "USB controller");
        return -1;
    }
    HvXhciTouchSnap t;
    memset(&t, 0, sizeof t);
    IN(t.nports);
    IN(t.present);
    IN(t.w_mm);
    IN(t.h_mm);
    IN(t.slot);
    IN(t.configured);
    IN(t.portsc);
    if (done(in, "USB touchscreen") != 0) return -1;
    /* Without the fingers' section (an older Henge's), every finger is lifted, where none can be seen. */
    if (hv_snap_get(r, S_UTCF, 1, in) == 0) {
        IN(t.mac_down);
        for (int i = 0; i < HV_XHCI_CONTACTS; i++) IN(t.mac_x[i]);
        for (int i = 0; i < HV_XHCI_CONTACTS; i++) IN(t.mac_y[i]);
        if (done(in, "USB touchscreen's fingers") != 0) return -1;
        t.fingers_known = !(t.mac_down >> 31);
        t.mac_down &= ~(1u << 31);
    }
    if (hv_xhci_touch_set_snap(x, &t) == 0) return 0;
    say("the %s section makes no sense", "USB touchscreen");
    return -1;
}

/* The speaker on port 7, beside XHCI: always written, so a snapshot without it is from before the speaker, and
   that Mac's driver doesn't know port 7. */
static void save_speaker(HvXhci *x, HvSnapOut *o) {
    HvXhciSpeakerSnap s;
    hv_xhci_speaker_get_snap(x, &s);
    OUT(s.slot);
    OUT(s.portsc);
    OUT(s.configured);
    OUT(s.alt);
    OUT(s.vol);
    OUT(s.mute);
}

static int load_speaker(HvXhci *x, HvSnapReader *r) {
    HvSnapIn in_;
    HvSnapIn *in = &in_;
    if (hv_snap_get(r, S_USPK, 1, in) != 0) {
        if (hv_xhci_speaker_set_snap(x, NULL) == 0) return 0;
        say("the %s has a device on a port it didn't have", "USB controller");
        return -1;
    }
    HvXhciSpeakerSnap s;
    memset(&s, 0, sizeof s);
    IN(s.slot);
    IN(s.portsc);
    IN(s.configured);
    IN(s.alt);
    IN(s.vol);
    IN(s.mute);
    if (done(in, "USB speaker") != 0) return -1;
    if (hv_xhci_speaker_set_snap(x, &s) == 0) return 0;
    say("the %s section makes no sense", "USB speaker");
    return -1;
}

/* The camera's part, beside XHCI: written whenever the machine has a camera, so a snapshot without it is one from
   before the camera (or without one), whose Mac never saw it. */
static void save_camera(HvXhci *x, HvSnapOut *o) {
    HvXhciCamSnap c;
    hv_xhci_camera_get_snap(x, &c);
    OUT(c.present);
    OUT(c.slot);
    OUT(c.portsc);
    OUT(c.dev.configured);
    OUT(c.dev.committed);
    OUT(c.dev.probe);
    OUT(c.dev.commit);
    OUT(c.dev.fid);
}

static int load_camera(HvXhci *x, HvSnapReader *r) {
    HvSnapIn in_;
    HvSnapIn *in = &in_;
    HvXhciCamSnap c;
    memset(&c, 0, sizeof c);
    if (hv_snap_get(r, S_UVID, 1, in) != 0) {
        if (hv_xhci_camera_set_snap(x, NULL) == 0) return 0;
        say("the %s has a device on a port it didn't have", "USB controller");
        return -1;
    }
    IN(c.present);
    IN(c.slot);
    IN(c.portsc);
    IN(c.dev.configured);
    IN(c.dev.committed);
    IN(c.dev.probe);
    IN(c.dev.commit);
    IN(c.dev.fid);
    if (done(in, "USB camera") != 0) return -1;
    /* A Mac saved with a camera, resumed where there is none (HV_NO_CAMERA): it was unplugged. */
    if (!x->cam.dev && c.present && !c.slot) c.present = 0;
    if (hv_xhci_camera_set_snap(x, &c) == 0) return 0;
    say("the %s section makes no sense", "USB camera");
    return -1;
}

/* ---- virtio-fs: the shared folders, as the Mac knows them ----

   The Mac holds node IDs (every file and folder it looked up) and handles (every one it has open).
   Both are saved; on load each open file is opened again by its path. The folders are matched by
   name, not path: on the iPad an app's folders move between launches. A file deleted meanwhile
   leaves a handle with no file, which answers "bad file" as a file pulled from under a Mac does. */

#define FS_MAX_HANDLES (1u << 20)

static int by_id(const void *a, const void *b) {
    uint64_t x = (*(HvFuseNode *const *)a)->id, y = (*(HvFuseNode *const *)b)->id;
    return x < y ? -1 : x > y;
}

static void save_fs(HvVirtioFs *d, HvSnapOut *o) {
    HvFuse *f = &d->fuse;
    save_virtio(&d->vio, o);
    int32_t nexp = f->nexports;
    OUT(nexp);
    for (int i = 0; i < f->nexports; i++) {
        put_str(o, f->exports[i].name);
        int32_t ro = f->exports[i].readonly;
        OUT(ro);
    }
    OUT(f->next_id);
    OUT(f->max_write);
    OUT(f->born);
    OUT(f->requests);
    OUT(f->errors);
    /* In ID order, so the same nodes always give the same bytes */
    HvFuseNode **all = malloc((f->nnodes ? f->nnodes : 1) * sizeof *all);
    uint32_t count = 0;
    if (!all) {
        o->bad = 1;
        return;
    }
    for (uint32_t b = 0; b < f->nbuckets; b++)
        for (HvFuseNode *n = f->by_id[b]; n && count < f->nnodes; n = n->next_id) all[count++] = n;
    qsort(all, count, sizeof *all, by_id);
    OUT(count);
    for (uint32_t i = 0; i < count; i++) {
        HvFuseNode *n = all[i];
        {
            OUT(n->id);
            OUT(n->parent);
            OUT(n->nlookup);
            int32_t exp = n->export;
            uint8_t linked = n->linked != 0, pinned = n->pinned != 0;
            OUT(exp);
            OUT(linked);
            OUT(pinned);
            put_str(o, n->name);
        }
    }
    free(all);
    uint32_t used = 0;
    for (uint32_t i = 0; i < f->nhandles; i++) used += f->handles[i].used != 0;
    OUT(used);
    for (uint32_t i = 0; i < f->nhandles; i++) {
        HvFuseHandle *hd = &f->handles[i];
        if (!hd->used) continue;
        uint32_t fh = i + 1;
        int32_t access = -1; /* no file */
        if (hd->fd >= 0) {
            int fl = fcntl(hd->fd, F_GETFL);
            access = fl < 0 ? -1 : (fl & O_ACCMODE) == O_RDWR ? 2 : (fl & O_ACCMODE) == O_WRONLY ? 1 : 0;
        }
        uint8_t dir = hd->dir != 0, ro = hd->readonly != 0, listed = hd->names != NULL;
        OUT(fh);
        OUT(dir);
        OUT(ro);
        OUT(hd->node);
        OUT(access);
        OUT(listed);
        if (!listed) continue;
        OUT(hd->count);
        for (uint32_t k = 0; k < hd->count; k++) {
            put_str(o, hd->names[k]);
            OUT(hd->types[k]);
            OUT(hd->inos[k]);
        }
    }
}

/* Every node must reach the top folder through nodes that exist, with no loops, and each is in the folder
   it is under, whatever the file says: paths are built from parents, so the folder (read-only or not)
   must come from them too. One top node per shared folder, and only those (and the top) pinned. */
static int fs_check_tree(HvFuse *f) {
    uint8_t seen[HV_FUSE_MAX_EXPORTS] = {0};
    for (uint32_t b = 0; b < f->nbuckets; b++)
        for (HvFuseNode *n = f->by_id[b]; n; n = n->next_id) {
            if (n->id == HV_FUSE_ROOT_ID) continue;
            HvFuseNode *top = n;
            for (int depth = 0; top && top->parent != HV_FUSE_ROOT_ID; depth++)
                top = depth < 4096 && top->parent != top->id ? hv_fuse_node(f, top->parent) : NULL;
            if (!top || top->id == HV_FUSE_ROOT_ID || !top->pinned || top->export < 0 || top->export >= f->nexports)
                return -1;
            if (n == top) {
                if (seen[top->export]++ || strcmp(n->name, f->exports[top->export].name)) return -1;
            } else if (n->pinned)
                return -1;
            n->export = top->export;
        }
    return 0;
}

static int load_fs(HvVirtioFs *d, HvSnapIn *in) {
    HvFuse *f = &d->fuse;
    if (d->submitted != d->published) {
        say("the %s are busy (loading needs them idle)", "shared folders");
        return -1;
    }
    load_virtio(&d->vio, in, "shared folders");
    /* The saved folders, each to the one of the same name now. */
    int32_t nexp = 0;
    int map[HV_FUSE_MAX_EXPORTS];
    IN(nexp);
    if (in->bad || nexp < 0 || nexp > HV_FUSE_MAX_EXPORTS) return in->bad = 1, 0;
    for (int i = 0; i < nexp; i++) {
        char name[64];
        int32_t ro = 0;
        if (get_str(in, name, sizeof name) != 0) return 0;
        IN(ro);
        map[i] = -1;
        for (int j = 0; j < f->nexports; j++)
            if (!strcmp(f->exports[j].name, name)) map[i] = j;
        if (map[i] < 0) {
            fprintf(stderr, "hengevm: snapshot: the saved Mac had the shared folder \"%s\", which isn't shared now\n", name);
            return -1;
        }
    }
    uint64_t next_id = 0;
    uint32_t nnodes = 0;
    IN(next_id);
    IN(f->max_write);
    IN(f->born);
    IN(f->requests);
    IN(f->errors);
    IN(nnodes);
    if (in->bad || next_id <= HV_FUSE_ROOT_ID || f->max_write == 0 || f->max_write > HV_VFS_BUF / 2) return in->bad = 1, 0;
    hv_fuse_clear(f);
    f->next_id = next_id;
    static char name[1024];
    for (uint32_t i = 0; i < nnodes && !in->bad; i++) {
        uint64_t id = 0, parent = 0, nlookup = 0;
        int32_t exp = 0;
        uint8_t linked = 0, pinned = 0;
        IN(id);
        IN(parent);
        IN(nlookup);
        IN(exp);
        IN(linked);
        IN(pinned);
        if (get_str(in, name, 256) != 0) break;
        int root = id == HV_FUSE_ROOT_ID;
        /* Names are joined into host paths: never accept one that could climb out of a folder. */
        if (id == 0 || id >= next_id || (root ? exp != -1 || name[0] : exp < 0 || exp >= nexp || !hv_fuse_name_ok(name))) {
            in->bad = 1;
            break;
        }
        if (hv_fuse_add_node(f, id, parent, name, root ? -1 : map[exp], nlookup, linked, pinned) != 0) in->bad = 1;
    }
    if (in->bad) return 0;
    HvFuseNode *root = hv_fuse_node(f, HV_FUSE_ROOT_ID);
    if (!root || !root->pinned || fs_check_tree(f) != 0) return in->bad = 1, 0;
    /* A folder shared now that wasn't then: its node, new to the Mac */
    for (int j = 0; j < f->nexports; j++) {
        int had = 0;
        for (int i = 0; i < nexp; i++) had |= map[i] == j;
        if (!had && hv_fuse_add_node(f, f->next_id++, HV_FUSE_ROOT_ID, f->exports[j].name, j, 0, 1, 1) != 0) return in->bad = 1, 0;
    }
    hv_fuse_count_children(f);

    uint32_t used = 0, gone = 0;
    IN(used);
    if (in->bad || used > FS_MAX_HANDLES) return in->bad = 1, 0;
    for (uint32_t i = 0; i < used && !in->bad; i++) {
        uint32_t fh = 0, count = 0;
        uint8_t dir = 0, ro = 0, listed = 0;
        uint64_t node = 0;
        int32_t access = -1;
        IN(fh);
        IN(dir);
        IN(ro);
        IN(node);
        IN(access);
        IN(listed);
        HvFuseHandle *hd = in->bad ? NULL : hv_fuse_add_handle(f, fh);
        if (!hd || access < -1 || access > 2 || (dir && access != -1)) {
            in->bad = 1;
            break;
        }
        hd->dir = dir, hd->readonly = ro, hd->node = node;
        if (listed) {
            uint32_t cap = 0;
            IN(count);
            if (in->bad || count > in->len) {
                in->bad = 1;
                break;
            }
            for (uint32_t k = 0; k < count && !in->bad; k++) {
                uint8_t type = 0;
                uint64_t ino = 0;
                if (get_str(in, name, 256) != 0) break;
                IN(type);
                IN(ino);
                if (hv_fuse_listing_add(hd, &cap, name, type, ino) != 0) in->bad = 1;
            }
            if (!count && !hd->names) hd->names = calloc(1, sizeof *hd->names); /* listed, and empty */
        }
        /* A folder shared read-only now stays so, whatever the Mac had it open for */
        HvFuseNode *nd = hv_fuse_node(f, node);
        if (!nd || nd->export < 0 || f->exports[nd->export].readonly) hd->readonly = 1;
        if (access >= 0 && hv_fuse_reopen(f, hd, hd->readonly ? 0 : access) != 0) gone++;
    }
    if (gone) fprintf(stderr, "hengevm: snapshot: %u file%s the Mac had open in a shared folder %s gone\n", gone, gone == 1 ? "" : "s", gone == 1 ? "is" : "are");
    return 0;
}

/* ---- the board ---- */

static int check(HvVmapple *b, int load, char *why, size_t why_len) {
    const char *no = NULL;
    if (b->vusb) no = "the Mac is being installed (an installer boot)";
    else if (b->usb)
        for (int p = 1; p < HV_XHCI_PORTS && !no; p++) {
            pthread_mutex_lock(&b->usb->mu);
            struct HvXhciPort *pt = &b->usb->port[p];
            if (load ? pt->drive != NULL : pt->plug != NULL || pt->unplug)
                no = load ? "this Mac is already running" : "a USB drive is being plugged in or unplugged (wait a moment)";
            pthread_mutex_unlock(&b->usb->mu);
        }
    if (!no) return 0;
    if (why && why_len) snprintf(why, why_len, "%s", no);
    return -1;
}

int hv_vmapple_snap_can_save(HvVmapple *b, char *why, size_t why_len) {
    return check(b, 0, why, why_len);
}

/* ---- the USB drives the host must plug in again, alone ----
   The same facts as XHCI's drive records, in a section of their own so a restore can check them before it
   changes anything: a drive missing or changed then refuses the restore with the machine as it was built
   (it can start afresh), instead of failing halfway with memory already loaded. */
static void save_drives(HvXhci *x, HvSnapOut *o) {
    pthread_mutex_lock(&x->mu);
    uint32_t n = 0;
    for (int p = 1; p < HV_XHCI_PORTS; p++) n += x->port[p].drive != NULL;
    OUT(n);
    for (int p = 1; p < HV_XHCI_PORTS; p++) {
        struct HvUsbStorage *d = x->port[p].drive;
        if (!d) continue;
        int64_t mtime = 0;
        if (hv_usb_storage_mtime(d, &mtime) != 0) o->bad = 1;
        put_str(o, hv_usb_storage_serial(d));
        put_str(o, hv_usb_storage_name(d));
        uint64_t size = hv_usb_storage_size(d);
        OUT(size);
        int32_t ro = hv_usb_storage_readonly(d);
        OUT(ro);
        OUT(mtime);
    }
    pthread_mutex_unlock(&x->mu);
}

/* Every section the load takes is there (or not, as this Mac's devices are) at the version it reads, and the
   screen's state is one it takes: what an update of Henge would refuse, found before anything is loaded. */
static int sections_fit(HvVmapple *b, HvSnapReader *r, const char **which) {
    const struct {
        uint32_t tag;
        int present;
        const char *name;
    } want[] = {
        {S_BOARD, 1, "board"}, {S_UART, 1, "serial port"}, {S_RTC, 1, "PL031 clock"}, {S_GPIO, 1, "GPIO"},
        {S_AES, 1, "AES engine"}, {S_BDIF, 1, "boot disk interface"}, {S_PCIE, 1, "PCI bus"},
        {S_BLK_ROOT, b->root_blk != NULL, "Mac's disk"}, {S_BLK_AUX, b->aux_blk != NULL, "auxiliary disk"},
        {S_NET, b->net != NULL, "network device"}, {S_KBD, b->keyboard != NULL, "keyboard"},
        {S_MOUSE, b->mouse != NULL, "mouse"}, {S_TABLET, b->tablet != NULL, "touch tablet"},
        {S_SND, b->snd != NULL, "sound device"}, {S_XHCI, b->usb != NULL, "USB controller"},
        {S_FS, b->fs != NULL, "shared folders"}, {S_POWER, b->power != NULL, "battery"},
        {S_BALLOON, b->balloon != NULL, "memory balloon"},
        {S_PVG, b->pvg != NULL, "screen"}, {HV_SNAP_GIC, 1, "interrupt controller"},
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        HvSnapIn in;
        if (get_sec(r, want[i].tag, want[i].present, want[i].name, &in) < 0) return *which = want[i].name, -1;
    }
    HvSnapIn in, gpu;
    if (b->pvg && hv_snap_get(r, S_PVG, 1, &in) == 0) {
        int has_gpu = hv_snap_has(r, S_PVGM) && hv_snap_get(r, S_PVGM, 1, &gpu) == 0;
        if (hv_pvg_lite_check(in.p, in.len, has_gpu ? gpu.p : NULL, has_gpu ? gpu.len : 0) != 0) return *which = "screen", -1;
    }
    return 0;
}

int hv_vmapple_snap_has_power(HvSnapReader *r) {
    HvSnapIn in;
    return hv_snap_get(r, S_POWER, 1, &in) == 0;
}

int hv_vmapple_snap_check(HvVmapple *b, HvSnapReader *r, char *why, size_t why_len) {
    if (why && why_len) why[0] = 0;
    char no_why[160];
    if (check(b, 1, no_why, sizeof no_why) != 0) {
        if (why && why_len) snprintf(why, why_len, "This Mac can't be restored now: %s.", no_why);
        return -1;
    }
    const char *which = "";
    if (sections_fit(b, r, &which) != 0) {
        if (why && why_len)
            snprintf(why, why_len, "The saved state doesn't fit this Mac (its %s): it is from another version of Henge, "
                     "or the Mac had other devices then.", which);
        return -1;
    }
    if (!b->usb) return 0;
    HvSnapIn in_;
    HvSnapIn *in = &in_;
    if (hv_snap_get(r, S_DRVS, 1, in) != 0) return 0; /* an older snapshot: the load checks its drives */
    uint32_t n = 0;
    IN(n);
    if (in->bad || n >= HV_XHCI_PORTS) return 0; /* damaged: the load says so */
    HvXhci *x = b->usb;
    for (uint32_t i = 0; i < n; i++) {
        char serial[64], name[64];
        uint64_t size = 0;
        int32_t ro = 0;
        int64_t mtime = 0, now = -1;
        get_str(in, serial, sizeof serial);
        get_str(in, name, sizeof name);
        IN(size);
        IN(ro);
        IN(mtime);
        if (in->bad) return 0;
        const char *no = NULL;
        pthread_mutex_lock(&x->mu);
        struct HvUsbStorage *d = NULL;
        for (int q = 1; q < HV_XHCI_PORTS && !d; q++)
            if (x->port[q].plug && !strcmp(hv_usb_storage_serial(x->port[q].plug), serial)) d = x->port[q].plug;
        if (!d) no = "isn't plugged in";
        else if (hv_usb_storage_size(d) != size) no = "is another size now";
        else if (hv_usb_storage_readonly(d) != (ro != 0)) no = ro ? "was read-only and is not now" : "was writable and is read-only now";
        else if (hv_usb_storage_mtime(d, &now) != 0 || now != mtime) no = "has changed since the Mac was suspended";
        pthread_mutex_unlock(&x->mu);
        if (no) {
            fprintf(stderr, "hengevm: snapshot: the USB drive \"%s\" (%s) %s\n", name, serial, no);
            if (why && why_len) snprintf(why, why_len, "The USB drive \"%s\" %s.", name, no);
            return -1;
        }
    }
    return 0;
}

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* How long a save waits for requests under way. A file that isn't on the iPad (iCloud) can keep one for
   minutes, and iOS gives an app in the background seconds: the save gives up instead, and the Mac runs on. */
#define QUIESCE_NS (3ull * 1000000000ull)

/* Lets every disk and shared-folder request under way finish and reach the guest. Device lock held; the CPUs
   paused, so no new requests come, only ones the finished ones made room for. 0, or -1 at the deadline. */
static int quiesce(HvVmapple *b, uint64_t deadline) {
    for (;;) {
        int busy = 0;
        HvVirtioBlk *blk[2] = {b->root_blk, b->aux_blk};
        for (int i = 0; i < 2; i++) {
            if (!blk[i]) continue;
            if (__atomic_load_n(&blk[i]->completed, __ATOMIC_ACQUIRE) != blk[i]->submitted) busy = 1;
            else if (blk[i]->published != blk[i]->submitted) hv_virtio_blk_poll(blk[i]), busy = 1;
        }
        if (b->fs) {
            if (__atomic_load_n(&b->fs->completed, __ATOMIC_ACQUIRE) != b->fs->submitted) busy = 1;
            else if (b->fs->published != b->fs->submitted) hv_virtio_fs_poll(b->fs), busy = 1;
        }
        if (!busy) return 0;
        if (mono_ns() > deadline) return -1;
        usleep(100);
    }
}

typedef struct {
    uint32_t tag;
    const char *name;
} Sec;

/* The screen: its registers, and what only the GPU holds, read back. After the other devices, for
   finishing the GPU's work writes to the Mac's memory and raises interrupts; before its memory is saved. */
typedef struct {
    HvSnapWriter *w;
    HvSnapStream *st;
    uint64_t ram; /* the Mac's memory, written after */
} PvgOut;

static int pvg_stream_write(void *ctx, const void *p, size_t n) {
    return hv_snap_stream_write(((PvgOut *)ctx)->st, p, n);
}

/* The GPU's part, before any of it is written; the RAM comes after it. Both must fit, with room to spare:
   a storage filled to the brim mid-save fails the save anyway, and troubles iOS and every other app meanwhile.
   The RAM is counted whole (it mostly compresses, but can't be counted on to), as the app counts it before. */
#define PVG_STORAGE_SPARE (512ull << 20)

/* Storage the RAM's part of a save may still take. Kept in files (core/ramfile.h): what the files need now. */
static uint64_t ram_room(const HvVmapple *b) {
    for (int i = 0; i < b->m->nram; i++) {
        const HvRam *r = &b->m->ram[i];
        if (r->host == b->ram && r->file) return hv_ramfile_save_need(r->file);
    }
    return b->ram_size;
}

uint64_t hv_vmapple_save_ram_room(const HvVmapple *b) { return ram_room(b); }

static int pvg_stream_room(void *ctx, uint64_t bytes, char *why, size_t why_len) {
    PvgOut *o = ctx;
    uint64_t free = hv_snap_free_bytes(o->w);
    if (free == UINT64_MAX) return 0; /* can't be told: the writes say if it doesn't fit */
    uint64_t need = PVG_STORAGE_SPARE + o->ram;
    if (free >= need && free - need >= bytes) return 0;
    need = bytes > UINT64_MAX - need ? UINT64_MAX : need + bytes;
    snprintf(why, why_len, "there isn't enough free storage on the iPad (%.1f GB needed, %.1f GB free)",
             (double)need / (1ull << 30), (double)free / (1ull << 30));
    return -1;
}

/* What only the GPU holds goes straight to the file, as it is read back (it may be gigabytes); the
   registers into o. why says what went wrong, for the person. */
/* ---- The screen's state read ahead (hv_vmapple_snap_capture) ---- */

struct HvVmappleCapture {
    HvPvgBlob regs;   /* the device's own state: what save_pvg puts in S_PVG */
    HvPvgBlob gpu;    /* what only the GPU holds: what save_pvg streams into S_PVGM */
    uint64_t gpu_len; /* UINT64_MAX: no GPU side yet (no S_PVGM) */
    HvPvgBlob mapper; /* the mapper's state: S_PVGS */
};

typedef struct {
    HvPvgBlob *to;
    uint64_t limit;
} CapOut;
/* (No refusal for time: one that runs late only keeps the save waiting, while not reading it here leaves the
   save to read the GPU later, when iOS may have taken it: then the save fails for certain.) */

static int cap_write(void *ctx, const void *p, size_t n) {
    CapOut *c = ctx;
    hv_pvg_blob_put(c->to, p, n);
    return c->to->bad ? -1 : 0;
}

/* Asked once, before any of it comes: within the limit, and the memory for it taken now, whole (growing as
   it comes would hold up to twice as much while it is moved) */
static int cap_room(void *ctx, uint64_t bytes, char *why, size_t why_len) {
    CapOut *c = ctx;
    if (bytes > c->limit || bytes > SIZE_MAX / 2) {
        snprintf(why, why_len, "the screen's state (%.1f MB) is more than Henge can hold in memory now (%.1f MB)",
                 (double)bytes / 1048576.0, (double)c->limit / 1048576.0);
        return -1;
    }
    /* With room for what comes beside the textures (records, answers): a sixteenth more and 1 MB */
    size_t want = c->to->len + (size_t)bytes + (size_t)bytes / 16 + (1u << 20);
    if (want > c->to->cap) {
        uint8_t *q = realloc(c->to->p, want);
        if (!q) {
            snprintf(why, why_len, "not enough memory to hold the screen's state (%.0f MB)", (double)bytes / 1048576.0);
            return -1;
        }
        c->to->p = q, c->to->cap = want;
    }
    return 0;
}

static void capture_free(struct HvVmappleCapture *c) {
    if (!c) return;
    free(c->regs.p);
    free(c->gpu.p);
    free(c->mapper.p);
    free(c);
}

void hv_vmapple_snap_drop_capture(HvVmapple *b) {
    capture_free(b->capture);
    b->capture = NULL;
}

int hv_vmapple_snap_capture(HvVmapple *b, uint64_t limit, uint64_t deadline_ns, char *why, size_t why_len) {
    if (why && why_len) why[0] = 0;
    /* One taken since the pause is still exact (it is dropped as the Mac goes on): kept, not risked on a retry */
    if (b->capture) return 0;
    if (!b->pvg) {
        snprintf(why, why_len, "no screen");
        return -1;
    }
    struct HvVmappleCapture *c = calloc(1, sizeof *c);
    if (!c) {
        snprintf(why, why_len, "out of memory");
        return -1;
    }
    uint64_t t0 = mono_ns();
    CapOut out = {&c->gpu, limit};
    HvPvgSink sink = {cap_write, &out, cap_room};
    c->gpu_len = UINT64_MAX;
    hv_dev_lock(b->m);
    int rc = hv_pvg_lite_save(b->pvg, &c->regs, &sink, &c->gpu_len, &c->mapper, deadline_ns, why, why_len);
    hv_dev_unlock(b->m);
    if (rc == 0 && (c->regs.bad || c->gpu.bad || c->mapper.bad || (c->gpu_len != UINT64_MAX && c->gpu_len != c->gpu.len))) {
        snprintf(why, why_len, "not enough memory to hold the screen's state");
        rc = -1;
    }
    if (rc != 0) {
        fprintf(stderr, "hengevm: snapshot: the screen's state wasn't read ahead: %s\n", why);
        capture_free(c);
        return -1;
    }
    b->capture = c;
    fprintf(stderr, "hengevm: snapshot: the screen's state read ahead in %.0f ms (%.1f MB from the GPU)\n",
            (double)(mono_ns() - t0) / 1e6, c->gpu_len == UINT64_MAX ? 0.0 : (double)c->gpu_len / 1048576.0);
    return 0;
}

/* A copy of what was read ahead into o, for its section */
static int blob_copy(HvSnapOut *o, const HvPvgBlob *from) {
    uint8_t *p = malloc(from->len ? from->len : 1);
    if (!p) return -1;
    memcpy(p, from->p, from->len);
    o->p = p, o->len = from->len, o->cap = from->len, o->bad = 0;
    return 0;
}

/* The save from what was read ahead: the GPU's part into the file as save_pvg's stream would have it, the
   device's own state into o and the mapper's into mapper (copies: the capture is kept until the guest goes on, for a
   save tried again). */
static int save_pvg_captured(HvVmapple *b, HvSnapWriter *w, HvSnapOut *o, HvSnapOut *mapper, char *why, size_t why_len) {
    struct HvVmappleCapture *c = b->capture;
    if (c->gpu_len != UINT64_MAX) {
        HvSnapStream *st = hv_snap_stream_begin(w, S_PVGM, 1);
        if (!st) {
            snprintf(why, why_len, "the screen's state couldn't be written to storage (is the iPad's storage full?)");
            return -1;
        }
        PvgOut out = {w, st, ram_room(b)};
        if (pvg_stream_room(&out, c->gpu_len, why, why_len) != 0) {
            hv_snap_stream_end(st, 0);
            return -1;
        }
        /* In pieces: one write of gigabytes is more than write(2) takes at once */
        int bad = 0;
        for (size_t at = 0; !bad && at < c->gpu.len; at += 64u << 20) {
            size_t n = c->gpu.len - at < (64u << 20) ? c->gpu.len - at : (64u << 20);
            bad = hv_snap_stream_write(st, c->gpu.p + at, n) != 0;
        }
        if (hv_snap_stream_end(st, !bad) != 0 || bad) {
            snprintf(why, why_len, "the screen's state couldn't be written to storage (is the iPad's storage full?)");
            return -1;
        }
    }
    if (blob_copy(o, &c->regs) != 0 || blob_copy(mapper, &c->mapper) != 0) {
        snprintf(why, why_len, "not enough memory to save the screen");
        return -1;
    }
    fprintf(stderr, "hengevm: snapshot: the screen's state as read ahead (%.1f MB from the GPU)\n",
            c->gpu_len == UINT64_MAX ? 0.0 : (double)c->gpu_len / 1048576.0);
    return 0;
}

static void save_failed(HvVmapple *b, const char *why);
/* The device's own state into o, and the mapper's into mapper (S_PVGS), which is put after it */
static int save_pvg(HvVmapple *b, HvSnapWriter *w, HvSnapOut *o, HvSnapOut *mapper, uint64_t deadline) {
    HvPvgLite *p = b->pvg;
    char why[160] = "";
    size_t why_len = sizeof why;
    if (b->capture) {
        if (save_pvg_captured(b, w, o, mapper, why, why_len) == 0) return 0;
        fprintf(stderr, "hengevm: snapshot: can't save now: %s\n", why);
        save_failed(b, why);
        return -1;
    }
    HvPvgBlob blob = {0}, ids = {0};
    HvSnapStream *st = hv_snap_stream_begin(w, S_PVGM, 1);
    if (!st) {
        snprintf(why, why_len, "the screen's state couldn't be written to storage (is the iPad's storage full?)");
        fprintf(stderr, "hengevm: snapshot: can't save now: %s\n", why);
        save_failed(b, why);
        return -1;
    }
    PvgOut out = {w, st, ram_room(b)};
    HvPvgSink sink = {pvg_stream_write, &out, pvg_stream_room};
    uint64_t gpu_len = UINT64_MAX;
    int rc = hv_pvg_lite_save(p, &blob, &sink, &gpu_len, &ids, deadline, why, why_len);
    int keep = rc == 0 && gpu_len != UINT64_MAX;
    if (hv_snap_stream_end(st, keep) != 0 && keep) {
        snprintf(why, why_len, "the screen's state couldn't be written to storage (is the iPad's storage full?)");
        rc = -1;
    }
    if (rc != 0) {
        fprintf(stderr, "hengevm: snapshot: can't save now: %s\n", why);
        save_failed(b, why);
        free(blob.p);
        free(ids.p);
        return -1;
    }
    o->p = blob.p, o->len = blob.len, o->cap = blob.cap, o->bad = blob.bad;
    mapper->p = ids.p, mapper->len = ids.len, mapper->cap = ids.cap, mapper->bad = ids.bad;
    return 0;
}

static void pvg_buf_release(void *ctx) {
    hv_snap_buf_free(ctx);
    free(ctx);
}

static int load_pvg(HvPvgLite *p, HvSnapReader *r, HvSnapIn *in) {
    HvPvgBytes gpu = {0};
    HvSnapBuf *buf = malloc(sizeof *buf);
    if (buf && hv_snap_has(r, S_PVGM) && hv_snap_take(r, S_PVGM, 1, buf) == 0) gpu = (HvPvgBytes){buf->p, buf->len, pvg_buf_release, buf};
    else free(buf); /* an older snapshot: the GPU's part is in the screen's section */
    if (hv_pvg_lite_load(p, in->p + in->at, in->len - in->at, &gpu) != 0) {
        in->bad = HV_SNAP_REFUSED; /* it said why */
        return -1;
    }
    in->at = in->len;
    /* The mapper's state: whatever it is (none, another version, damaged), the resume goes on (the ids unknown) */
    HvSnapIn ids;
    int version = hv_snap_version(r, S_PVGS);
    int got = version == HV_PVG_MAPPER_VERSION && hv_snap_get(r, S_PVGS, (uint32_t)version, &ids) == 0;
    hv_pvg_lite_load_mapper(p, got ? ids.p : NULL, got ? ids.len : 0, version);
    return 0;
}

const char *hv_vmapple_snap_save_why(HvVmapple *b) {
    return b->save_why[0] ? b->save_why : "A device couldn't be saved (the log says which).";
}

/* The sentence for the person, from a reason that starts in lower case */
static void save_failed(HvVmapple *b, const char *why) {
    snprintf(b->save_why, sizeof b->save_why, "%s", why && why[0] ? why : "");
    /* A sentence: capitalised, but not "iPad" or a word already in capitals */
    if (b->save_why[0] >= 'a' && b->save_why[0] <= 'z' && !(b->save_why[1] >= 'A' && b->save_why[1] <= 'Z')) b->save_why[0] -= 'a' - 'A';
    size_t n = strlen(b->save_why);
    if (n && n + 1 < sizeof b->save_why && b->save_why[n - 1] != '.') b->save_why[n] = '.', b->save_why[n + 1] = 0;
}

int hv_vmapple_snap_save(HvVmapple *b, HvSnapWriter *w) {
    char why[160];
    b->save_why[0] = 0;
    if (hv_vmapple_snap_can_save(b, why, sizeof why) != 0) {
        fprintf(stderr, "hengevm: snapshot: can't save now: %s\n", why);
        save_failed(b, why);
        return -1;
    }
    HvMachine *m = b->m;
    int rc = 0;
    hv_dev_lock(m);
    uint64_t deadline = mono_ns() + QUIESCE_NS;
    int settled = 0;
    if (quiesce(b, deadline) != 0) {
        fprintf(stderr, "hengevm: snapshot: can't save now: a disk or shared-folder request is taking too long\n");
        save_failed(b, "a disk or shared folder was still busy (a file in iCloud can keep one waiting), so nothing was saved");
        rc = -1;
    } else if (b->usb && (settled = hv_xhci_drives_settle(b->usb, deadline)) != 0) {
        const char *d = settled == -2 ? "a USB drive is taking too long" : "a USB drive could not write what it holds to its file";
        fprintf(stderr, "hengevm: snapshot: can't save now: %s\n", d);
        save_failed(b, d);
        rc = -1;
    }
#define PUT(tag, call)                                         \
    do {                                                       \
        HvSnapOut o_ = {0};                                    \
        HvSnapOut *o = &o_;                                    \
        call;                                                  \
        if (!rc && hv_snap_put(w, tag, 1, &o_) != 0) rc = -1;  \
        else if (rc) free(o_.p);                               \
    } while (0)
    PUT(S_BOARD, save_board(b, o));
    PUT(S_UART, save_uart(b->uart, o));
    PUT(S_RTC, save_rtc(b->rtc, o));
    PUT(S_GPIO, save_gpio(b->gpio, o));
    PUT(S_AES, save_aes(b->aes, o));
    PUT(S_BDIF, save_bdif(b->bdif, o));
    PUT(S_PCIE, save_pcie(b->pcie, o));
    if (b->root_blk) PUT(S_BLK_ROOT, save_blk(b->root_blk, o));
    if (b->aux_blk) PUT(S_BLK_AUX, save_blk(b->aux_blk, o));
    if (b->net) PUT(S_NET, save_net(b->net, o));
    if (b->keyboard) PUT(S_KBD, save_input(b->keyboard, o));
    if (b->mouse) PUT(S_MOUSE, save_input(b->mouse, o));
    if (b->tablet) PUT(S_TABLET, save_input(b->tablet, o));
    if (b->snd) PUT(S_SND, save_snd(b->snd, o));
    if (b->snd && hv_virtio_snd_streams(b->snd) > 1) PUT(S_SNDI, save_snd_in(b->snd, o));
    if (b->usb) PUT(S_XHCI, save_xhci(b->usb, o));
    if (b->usb) PUT(S_UTCH, save_touch(b->usb, o));
    if (b->usb) PUT(S_UTCF, save_touch_fingers(b->usb, o));
    if (b->usb) PUT(S_USPK, save_speaker(b->usb, o));
    if (b->usb && b->usb->cam.dev) PUT(S_UVID, save_camera(b->usb, o));
    if (b->usb) PUT(S_DRVS, save_drives(b->usb, o));
    if (b->fs) PUT(S_FS, save_fs(b->fs, o));
    if (b->power) PUT(S_POWER, save_power(b->power, o));
    if (b->balloon) PUT(S_BALLOON, save_balloon(b->balloon, o));
    if (b->pvg) {
        HvSnapOut ids = {0}; /* after S_PVG: its save finished the mapper's walk first */
        PUT(S_PVG, if (!rc && save_pvg(b, w, o, &ids, deadline) != 0) rc = -1);
        PUT(S_PVGS, if (!rc) { *o = ids; memset(&ids, 0, sizeof ids); });
        free(ids.p);
    }
#undef PUT
    hv_dev_unlock(m);
    /* Last: quiescing may have raised interrupts. */
    if (!rc && hv_gicv3_snap_save(b->gic, w) != 0) rc = -1;
    return rc;
}

int hv_vmapple_snap_load(HvVmapple *b, HvSnapReader *r) {
    char why[160];
    if (check(b, 1, why, sizeof why) != 0) {
        fprintf(stderr, "hengevm: snapshot: can't load into this Mac: %s\n", why);
        return -1;
    }
    HvMachine *m = b->m;
    int rc = 0;
    hv_dev_lock(m);
    /* Each: is it there as it should be, then its fields, then nothing left over. */
#define GET(tag, name, present, call)                              \
    do {                                                           \
        HvSnapIn in_;                                              \
        HvSnapIn *in = &in_;                                       \
        if (rc) break;                                             \
        int got_ = get_sec(r, tag, (present) != 0, name, in);      \
        if (got_ < 0) rc = -1;                                     \
        else if (got_ && ((call) != 0 || done(in, name) != 0)) rc = -1; \
    } while (0)
#define V(x) ((x), 0)
    GET(S_BOARD, "board", 1, V(load_board(b, in)));
    GET(S_UART, "serial port", 1, V(load_uart(b->uart, in)));
    GET(S_RTC, "PL031 clock", 1, V(load_rtc(b->rtc, in)));
    GET(S_GPIO, "GPIO", 1, V(load_gpio(b->gpio, in)));
    GET(S_AES, "AES engine", 1, V(load_aes(b->aes, in)));
    GET(S_BDIF, "boot disk interface", 1, V(load_bdif(b->bdif, in)));
    GET(S_PCIE, "PCI bus", 1, load_pcie(b->pcie, in));
    GET(S_BLK_ROOT, "Mac's disk", b->root_blk, load_blk(b->root_blk, in, "Mac's disk"));
    GET(S_BLK_AUX, "auxiliary disk", b->aux_blk, load_blk(b->aux_blk, in, "auxiliary disk"));
    GET(S_NET, "network device", b->net, V(load_net(b->net, in)));
    GET(S_KBD, "keyboard", b->keyboard, V(load_input(b->keyboard, in, "keyboard")));
    GET(S_MOUSE, "mouse", b->mouse, V(load_input(b->mouse, in, "mouse")));
    GET(S_TABLET, "touch tablet", b->tablet, V(load_input(b->tablet, in, "touch tablet")));
    GET(S_SND, "sound device", b->snd, V(load_snd(b, b->snd, in)));
    if (!rc && b->snd && load_snd_in(b->snd, r) != 0) rc = -1;
    GET(S_XHCI, "USB controller", b->usb, V(load_xhci(b->usb, in)));
    if (!rc && b->usb && load_touch(b->usb, r) != 0) rc = -1;
    if (!rc && b->usb && load_speaker(b->usb, r) != 0) rc = -1;
    if (!rc && b->usb && load_camera(b->usb, r) != 0) rc = -1;
    GET(S_FS, "shared folders", b->fs, load_fs(b->fs, in));
    GET(S_POWER, "battery", b->power, V(load_power(b->power, in)));
    GET(S_BALLOON, "memory balloon", b->balloon, V(load_balloon(b->balloon, in)));
    GET(S_PVG, "screen", b->pvg, load_pvg(b->pvg, r, in));
#undef GET
#undef V
    hv_dev_unlock(m);
    if (!rc && hv_gicv3_snap_load(b->gic, r) != 0) rc = -1;
    return rc;
}

void hv_vmapple_restored(HvVmapple *b) {
    snd_restart(b);
    hv_virtio_input_release_all(b->keyboard);
    hv_virtio_input_release_all(b->mouse);
    hv_virtio_input_release_all(b->tablet);
    hv_xhci_release_keys(b->usb);
    hv_virtio_net_relink(b->net, 1500); /* a new network behind it: the Mac asks for its address again */
    hv_pvg_lite_restored(b->pvg); /* the GPU's part back, and the last frame on the screen */
    hv_vmapple_rtc_event(b, HV_AVP_RTC_WOKE | HV_AVP_RTC_RESTORED);
}
