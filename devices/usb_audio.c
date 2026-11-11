#include "usb_audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- descriptors ---- */

#define EP0_MAX 64
#define MAX_FRAMES 49 /* frames in one 1 ms packet: 48, and one for the host's clock running fast */
#define ISO_MAX_16 (MAX_FRAMES * 4)
#define ISO_MAX_24 (MAX_FRAMES * 8)

/* Entities in the audio function. */
#define ID_CLOCK 0x10
#define ID_INPUT 0x01   /* the USB stream */
#define ID_FEATURE 0x02 /* volume and mute */
#define ID_OUTPUT 0x03  /* the speaker */

#define IF_CONTROL 0
#define IF_STREAM 1
#define IF_HID 2

/* UAC 2.0 requests and control selectors. */
#define REQ_CUR 0x01
#define REQ_RANGE 0x02
#define CS_SAM_FREQ 0x01
#define CS_CLOCK_VALID 0x02
#define FU_MUTE 0x01
#define FU_VOLUME 0x02

static const uint8_t device_desc[18] = {
    18, 1, 0x00, 0x02, 0xef, 0x02, 0x01, EP0_MAX, /* USB 2.0; miscellaneous, interface association */
    0x09, 0x12, 0x68, 0x48,                       /* vendor 0x1209 (pid.codes), product 0x4868 */
    0x00, 0x01, 1, 2, 3, 1,                       /* bcdDevice 1.00, strings 1-3, one configuration */
};

static const uint8_t qualifier_desc[10] = {10, 6, 0x00, 0x02, 0xef, 0x02, 0x01, EP0_MAX, 1, 0};

/* Volume Up, Volume Down, Mute: a bit each, in a one-byte report (what a headset's buttons send). */
static const uint8_t hid_report_desc[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, /* Consumer page, Consumer Control, application collection */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03,
    0x09, 0xe9, 0x09, 0xea, 0x09, 0xe2, 0x81, 0x02, /* Volume Increment, Decrement, Mute: variables */
    0x95, 0x05, 0x81, 0x03,                         /* padding */
    0xc0,
};

#define LE16(v) (uint8_t)((v) & 0xff), (uint8_t)(((v) >> 8) & 0xff)
#define LE32(v) LE16((v) & 0xffff), LE16(((uint32_t)(v) >> 16) & 0xffff)

/* The class-specific audio control descriptors: header, clock, input terminal, feature unit, output terminal. */
#define AC_LEN (9 + 8 + 17 + 18 + 12)

static int build_config(uint8_t *c) {
    int n = 0;
#define B(...)                                             \
    do {                                                   \
        const uint8_t b_[] = {__VA_ARGS__};                \
        memcpy(c + n, b_, sizeof b_);                      \
        n += (int)sizeof b_;                               \
    } while (0)
    B(9, 2, 0, 0, 3, 1, 0, 0x80, 50);                     /* three interfaces, bus powered, 100 mA; length below */
    B(8, 0x0b, IF_CONTROL, 2, 0x01, 0x00, 0x20, 2);       /* interface association: audio 2.0, the product name */
    /* interface 0: audio control */
    B(9, 4, IF_CONTROL, 0, 1, 0x01, 0x01, 0x20, 0);
    B(9, 0x24, 0x01, LE16(0x0200), 0x01, LE16(AC_LEN), 0x00);         /* header: ADC 2.0, desktop speaker */
    B(8, 0x24, 0x0a, ID_CLOCK, 0x01, 0x05, 0, 0);                      /* clock source: internal, fixed; frequency and validity read-only */
    B(17, 0x24, 0x02, ID_INPUT, LE16(0x0101), 0, ID_CLOCK, 2, LE32(3), 0, LE16(0), 0); /* input terminal: USB streaming, stereo */
    B(18, 0x24, 0x06, ID_FEATURE, ID_INPUT, LE32(0x0f), LE32(0), LE32(0), 0);        /* feature unit: master mute and volume, read/write */
    B(12, 0x24, 0x03, ID_OUTPUT, LE16(0x0301), 0, ID_FEATURE, ID_CLOCK, LE16(0), 0);  /* output terminal: speaker */
    B(7, 5, 0x82, 0x03, LE16(6), 4);                      /* EP2 IN, interrupt, 6 bytes, every 1 ms */
    /* interface 1: the stream. Setting 0 none, 1 16-bit, 2 24-bit in 32 */
    B(9, 4, IF_STREAM, 0, 0, 0x01, 0x02, 0x20, 0);
    for (int alt = 1; alt <= 2; alt++) {
        int sub = alt == 1 ? 2 : 4, bits = alt == 1 ? 16 : 24, max = alt == 1 ? ISO_MAX_16 : ISO_MAX_24;
        B(9, 4, IF_STREAM, (uint8_t)alt, 1, 0x01, 0x02, 0x20, 0);
        B(16, 0x24, 0x01, ID_INPUT, 0x00, 0x01, LE32(1), 2, LE32(3), 0); /* general: PCM, stereo */
        B(6, 0x24, 0x02, 0x01, (uint8_t)sub, (uint8_t)bits);               /* format type I */
        B(7, 5, 0x01, 0x09, LE16(max), 4);                                 /* EP1 OUT, isochronous, adaptive, every 1 ms */
        B(8, 0x25, 0x01, 0x00, 0x00, 0, LE16(0));                          /* class-specific endpoint */
    }
    /* interface 2: the volume buttons */
    B(9, 4, IF_HID, 0, 1, 0x03, 0x00, 0x00, 0);
    B(9, 0x21, LE16(0x0111), 0, 1, 0x22, LE16(sizeof hid_report_desc));
    B(7, 5, 0x83, 0x03, LE16(1), 6);                      /* EP3 IN, interrupt, 1 byte, every 4 ms */
#undef B
    c[2] = (uint8_t)n, c[3] = (uint8_t)(n >> 8);
    return n;
}

/* ---- the volume ---- */

static int clamp_vol(int v) {
    if (v < HV_USB_AUDIO_VOL_MIN) v = HV_USB_AUDIO_VOL_MIN;
    if (v > HV_USB_AUDIO_VOL_MAX) v = HV_USB_AUDIO_VOL_MAX;
    /* to a step of the range, from its minimum */
    int k = (v - HV_USB_AUDIO_VOL_MIN + HV_USB_AUDIO_VOL_RES / 2) / HV_USB_AUDIO_VOL_RES;
    return HV_USB_AUDIO_VOL_MIN + k * HV_USB_AUDIO_VOL_RES;
}

double hv_usb_audio_db_to_scalar(int db256) {
    if (db256 <= HV_USB_AUDIO_VOL_MIN) return 0.0;
    if (db256 >= HV_USB_AUDIO_VOL_MAX) return 1.0;
    return (double)(db256 - HV_USB_AUDIO_VOL_MIN) / (HV_USB_AUDIO_VOL_MAX - HV_USB_AUDIO_VOL_MIN);
}

int hv_usb_audio_scalar_to_db(double s) {
    if (!(s > 0)) return HV_USB_AUDIO_VOL_MIN;
    if (s >= 1) return HV_USB_AUDIO_VOL_MAX;
    return clamp_vol((int)lround(HV_USB_AUDIO_VOL_MIN + s * (HV_USB_AUDIO_VOL_MAX - HV_USB_AUDIO_VOL_MIN)));
}

/* Under mu: tell macOS a Feature Unit control changed (its interrupt data message). */
static void status_changed(HvUsbAudio *a, int cs) {
    if (!a->configured) return; /* it reads them all when it configures the device */
    if (a->stail - a->shead >= HV_USB_AUDIO_QUEUE) return;
    uint8_t *msg = a->status[a->stail++ % HV_USB_AUDIO_QUEUE];
    msg[0] = 0x00;            /* bInfo: an interface's, class-specific */
    msg[1] = REQ_CUR;         /* bAttribute: its CUR changed */
    msg[2] = 0, msg[3] = (uint8_t)cs;         /* wValue: channel 0 (master), control selector */
    msg[4] = IF_CONTROL, msg[5] = ID_FEATURE; /* wIndex: interface, entity */
    atomic_store(&a->pending, 1);
}

/* ---- the USB side ---- */

static int string_desc(const char *str, uint8_t *out) {
    int n = (int)strlen(str);
    if (n > 126) n = 126;
    out[0] = (uint8_t)(2 + 2 * n), out[1] = 3;
    for (int i = 0; i < n; i++) out[2 + 2 * i] = (uint8_t)str[i], out[3 + 2 * i] = 0;
    return 2 + 2 * n;
}

static void set_alt(HvUsbAudio *a, int alt) {
    if (alt == a->alt) return;
    if (a->alt && a->started) {
        atomic_store(&a->frame_bytes, 0);
        if (a->backend.stop) a->backend.stop(a->backend.opaque);
        a->started = 0;
    }
    a->alt = alt;
    if (alt) {
        /* A new epoch: the host's thread starts reading where this stream's frames begin. */
        atomic_store(&a->start_at, atomic_load(&a->written));
        atomic_store(&a->frame_bytes, alt == 1 ? 4u : 8u);
        atomic_fetch_add(&a->epoch, 1);
        if (a->backend.start) a->backend.start(a->backend.opaque, alt == 1 ? 16 : 24);
        a->started = 1;
    }
}

static int put(uint8_t *out, int max, const uint8_t *src, int len) {
    if (len > max) len = max;
    memcpy(out, src, (size_t)len);
    return len;
}

/* An audio class request to the control interface (entity: wIndex's high byte). */
static int audio_request(HvUsbAudio *a, int in, int req, int cs, int cn, int entity, const uint8_t *data, int data_len,
                         uint8_t *out, int max) {
    uint8_t r[16];
    if (entity == ID_CLOCK) {
        if (cs == CS_SAM_FREQ && in && req == REQ_CUR) {
            const uint8_t v[4] = {LE32(HV_USB_AUDIO_RATE)};
            return put(out, max, v, 4);
        }
        if (cs == CS_SAM_FREQ && in && req == REQ_RANGE) {
            const uint8_t v[14] = {LE16(1), LE32(HV_USB_AUDIO_RATE), LE32(HV_USB_AUDIO_RATE), LE32(0)};
            return put(out, max, v, 14);
        }
        if (cs == CS_SAM_FREQ && !in && req == REQ_CUR) { /* the one rate there is */
            if (data_len < 4) return -1;
            uint32_t f = (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
            return f == HV_USB_AUDIO_RATE ? 0 : -1;
        }
        if (cs == CS_CLOCK_VALID && in && req == REQ_CUR) {
            r[0] = 1;
            return put(out, max, r, 1);
        }
        return -1;
    }
    if (entity != ID_FEATURE || cn != 0) return -1; /* the feature unit has master controls only */
    if (cs == FU_MUTE && req == REQ_CUR) {
        if (in) {
            pthread_mutex_lock(&a->mu);
            r[0] = (uint8_t)a->mute;
            pthread_mutex_unlock(&a->mu);
            return put(out, max, r, 1);
        }
        if (data_len < 1) return -1;
        pthread_mutex_lock(&a->mu);
        a->mute = data[0] != 0;
        int vol = a->vol, mute = a->mute;
        pthread_mutex_unlock(&a->mu);
        if (a->backend.volume) a->backend.volume(a->backend.opaque, vol, mute);
        return 0;
    }
    if (cs == FU_VOLUME && req == REQ_CUR) {
        if (in) {
            pthread_mutex_lock(&a->mu);
            int v = a->vol;
            pthread_mutex_unlock(&a->mu);
            r[0] = (uint8_t)(v & 0xff), r[1] = (uint8_t)((v >> 8) & 0xff);
            return put(out, max, r, 2);
        }
        if (data_len < 2) return -1;
        int v = (int16_t)(data[0] | data[1] << 8);
        if (v == (int16_t)0x8000) v = HV_USB_AUDIO_VOL_MIN; /* "-infinity" */
        pthread_mutex_lock(&a->mu);
        a->vol = clamp_vol(v);
        int vol = a->vol, mute = a->mute;
        pthread_mutex_unlock(&a->mu);
        if (a->backend.volume) a->backend.volume(a->backend.opaque, vol, mute);
        return 0;
    }
    if (cs == FU_VOLUME && req == REQ_RANGE && in) {
        const uint8_t v[8] = {LE16(1), LE16(HV_USB_AUDIO_VOL_MIN), LE16(HV_USB_AUDIO_VOL_MAX), LE16(HV_USB_AUDIO_VOL_RES)};
        return put(out, max, v, 8);
    }
    return -1;
}

int hv_usb_audio_control(HvUsbAudio *a, const uint8_t setup[8], const uint8_t *data, int data_len, uint8_t *out, int max) {
    uint8_t type = setup[0], req = setup[1];
    uint16_t value = (uint16_t)(setup[2] | setup[3] << 8), index = (uint16_t)(setup[4] | setup[5] << 8);
    uint8_t tmp[256];
    int len;
    if (type == 0x80 && req == 6) { /* GET_DESCRIPTOR */
        uint8_t dt = value >> 8, idx = value & 0xff;
        if (dt == 1) return put(out, max, device_desc, sizeof device_desc);
        if (dt == 2) return put(out, max, a->cfg, a->cfg_len);
        if (dt == 6) return put(out, max, qualifier_desc, sizeof qualifier_desc);
        if (dt == 3 && idx == 0) tmp[0] = 4, tmp[1] = 3, tmp[2] = 0x09, tmp[3] = 0x04, len = 4;
        else if (dt == 3 && idx == 1) len = string_desc("Henge", tmp);
        else if (dt == 3 && idx == 2) len = string_desc(a->name, tmp);
        else if (dt == 3 && idx == 3) len = string_desc("1", tmp);
        else return -1;
        return put(out, max, tmp, len);
    }
    if (type == 0x81 && req == 6 && (index & 0xff) == IF_HID) { /* the HID interface's descriptors */
        if (value >> 8 == 0x22) return put(out, max, hid_report_desc, sizeof hid_report_desc);
        if (value >> 8 == 0x21) {
            const uint8_t *hid = a->cfg + a->cfg_len - 16; /* its HID descriptor, before its endpoint's */
            return put(out, max, hid, 9);
        }
        return -1;
    }
    if ((type & 0xfc) == 0x80 && req == 0) { /* GET_STATUS */
        tmp[0] = tmp[1] = 0;
        return put(out, max, tmp, 2);
    }
    if (type == 0x80 && req == 8) { /* GET_CONFIGURATION */
        tmp[0] = (uint8_t)a->configured;
        return put(out, max, tmp, 1);
    }
    if (type == 0x00 && req == 9) { /* SET_CONFIGURATION */
        int c = value & 0xff;
        if (c > 1) return -1;
        set_alt(a, 0);
        pthread_mutex_lock(&a->mu);
        a->configured = c;
        a->shead = a->stail, a->hhead = a->htail; /* nothing old waits */
        a->hid_cur = 0;
        pthread_mutex_unlock(&a->mu);
        return 0;
    }
    if (type == 0x01 && req == 11) { /* SET_INTERFACE */
        int intf = index & 0xff, alt = value & 0xff;
        if (intf == IF_STREAM && alt <= 2) {
            set_alt(a, alt);
            return 0;
        }
        return (intf == IF_CONTROL || intf == IF_HID) && alt == 0 ? 0 : -1;
    }
    if (type == 0x81 && req == 10) { /* GET_INTERFACE */
        int intf = index & 0xff;
        if (intf > IF_HID) return -1;
        tmp[0] = (uint8_t)(intf == IF_STREAM ? a->alt : 0);
        return put(out, max, tmp, 1);
    }
    if ((type == 0x00 || type == 0x01 || type == 0x02) && (req == 1 || req == 3)) return 0; /* CLEAR/SET_FEATURE */
    if ((type == 0x21 || type == 0xa1) && (index & 0xff) == IF_HID) { /* HID class */
        if (type == 0xa1 && req == 0x01) { /* GET_REPORT */
            pthread_mutex_lock(&a->mu);
            tmp[0] = a->hid_cur;
            pthread_mutex_unlock(&a->mu);
            return put(out, max, tmp, 1);
        }
        if (type == 0xa1 && req == 0x02) { tmp[0] = 0; return put(out, max, tmp, 1); } /* GET_IDLE */
        if (type == 0xa1 && req == 0x03) { tmp[0] = 1; return put(out, max, tmp, 1); } /* GET_PROTOCOL: report */
        if (type == 0x21 && (req == 0x09 || req == 0x0a || req == 0x0b)) return 0;   /* SET_REPORT, SET_IDLE, SET_PROTOCOL */
        return -1;
    }
    if ((type == 0x21 || type == 0xa1) && (index & 0xff) == IF_CONTROL) /* audio class, to an entity */
        return audio_request(a, type == 0xa1, req, value >> 8, value & 0xff, index >> 8, data, data_len, out, max);
    if (a->m && a->m->log) fprintf(stderr, "usb-audio: unhandled request %02x %02x value %04x index %04x\n", type, req, value, index);
    return -1;
}

void hv_usb_audio_reset(HvUsbAudio *a) {
    set_alt(a, 0);
    pthread_mutex_lock(&a->mu);
    a->configured = 0;
    a->shead = a->stail, a->hhead = a->htail;
    a->hid_cur = 0;
    atomic_store(&a->pending, 0);
    pthread_mutex_unlock(&a->mu);
}

void hv_usb_audio_iso_out(HvUsbAudio *a, const void *data, uint32_t len) {
    unsigned fb = atomic_load_explicit(&a->frame_bytes, memory_order_relaxed);
    if (!fb) return;
    len -= len % fb; /* whole frames */
    if (!len) return;
    a->packets++;
    uint64_t w = atomic_load_explicit(&a->written, memory_order_relaxed);
    uint64_t c = atomic_load_explicit(&a->consumed, memory_order_acquire);
    uint64_t start = atomic_load_explicit(&a->start_at, memory_order_relaxed);
    if (c < start) c = start; /* the host's thread hasn't caught up with this stream's start: that part is free */
    if (w - c + len > HV_USB_AUDIO_RING) { /* the host doesn't take it (fast enough): this packet is lost */
        a->dropped++;
        return;
    }
    uint32_t at = (uint32_t)(w % HV_USB_AUDIO_RING), first = HV_USB_AUDIO_RING - at;
    if (first > len) first = len;
    memcpy(a->ring + at, data, first);
    if (len > first) memcpy(a->ring, (const uint8_t *)data + first, len - first);
    atomic_store_explicit(&a->written, w + len, memory_order_release);
}

int hv_usb_audio_next_status(HvUsbAudio *a, uint8_t out[6]) {
    int n = 0;
    pthread_mutex_lock(&a->mu);
    if (a->shead != a->stail) memcpy(out, a->status[a->shead++ % HV_USB_AUDIO_QUEUE], 6), n = 6;
    atomic_store(&a->pending, a->shead != a->stail || a->hhead != a->htail);
    pthread_mutex_unlock(&a->mu);
    return n;
}

int hv_usb_audio_next_hid(HvUsbAudio *a, uint8_t out[1]) {
    int n = 0;
    pthread_mutex_lock(&a->mu);
    if (a->hhead != a->htail) out[0] = a->hid_cur = a->hid[a->hhead++ % HV_USB_AUDIO_QUEUE], n = 1;
    atomic_store(&a->pending, a->shead != a->stail || a->hhead != a->htail);
    pthread_mutex_unlock(&a->mu);
    return n;
}

int hv_usb_audio_pending(HvUsbAudio *a) {
    return atomic_load_explicit(&a->pending, memory_order_relaxed);
}

/* ---- the host's side ---- */

void hv_usb_audio_host_volume(HvUsbAudio *a, int db256) {
    pthread_mutex_lock(&a->mu);
    int v = clamp_vol(db256);
    int changed = v != a->vol;
    a->vol = v;
    if (changed) status_changed(a, FU_VOLUME);
    pthread_mutex_unlock(&a->mu);
    if (changed && a->m) hv_machine_wake(a->m);
}

void hv_usb_audio_host_mute(HvUsbAudio *a, int mute) {
    pthread_mutex_lock(&a->mu);
    int changed = (mute != 0) != a->mute;
    a->mute = mute != 0;
    if (changed) status_changed(a, FU_MUTE);
    pthread_mutex_unlock(&a->mu);
    if (changed && a->m) hv_machine_wake(a->m);
}

void hv_usb_audio_host_key(HvUsbAudio *a, int usage) {
    uint8_t bit = usage == HV_USB_AUDIO_KEY_VOLUME_UP ? 1 : usage == HV_USB_AUDIO_KEY_VOLUME_DOWN ? 2 : usage == HV_USB_AUDIO_KEY_MUTE ? 4 : 0;
    if (!bit) return;
    int queued = 0;
    pthread_mutex_lock(&a->mu);
    if (a->configured && a->htail - a->hhead <= HV_USB_AUDIO_QUEUE - 2) { /* pressed, then let go */
        a->hid[a->htail++ % HV_USB_AUDIO_QUEUE] = bit;
        a->hid[a->htail++ % HV_USB_AUDIO_QUEUE] = 0;
        atomic_store(&a->pending, 1);
        queued = 1;
    }
    pthread_mutex_unlock(&a->mu);
    if (queued && a->m) hv_machine_wake(a->m);
}

void hv_usb_audio_get_volume(HvUsbAudio *a, int *db256, int *mute) {
    pthread_mutex_lock(&a->mu);
    if (db256) *db256 = a->vol;
    if (mute) *mute = a->mute;
    pthread_mutex_unlock(&a->mu);
}

static float sample(const uint8_t *p, unsigned bytes) {
    if (bytes == 2) return (float)(int16_t)(p[0] | p[1] << 8) * (1.0f / 32768.0f);
    int32_t v = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
    return (float)v * (1.0f / 2147483648.0f);
}

uint32_t hv_usb_audio_pull(HvUsbAudio *a, float *out, uint32_t frames, float gain) {
    unsigned epoch = atomic_load_explicit(&a->epoch, memory_order_acquire);
    unsigned fb = atomic_load_explicit(&a->frame_bytes, memory_order_acquire);
    if (epoch != a->seen_epoch) { /* a new stream: what was left of the last one goes */
        a->seen_epoch = epoch;
        a->cur_frame_bytes = fb;
        atomic_store_explicit(&a->consumed, atomic_load_explicit(&a->start_at, memory_order_acquire), memory_order_release);
        a->primed = 0;
    }
    memset(out, 0, sizeof *out * 2 * frames);
    if (!fb || fb != a->cur_frame_bytes) return 0;
    uint64_t c = atomic_load_explicit(&a->consumed, memory_order_relaxed);
    uint64_t w = atomic_load_explicit(&a->written, memory_order_acquire);
    uint64_t have = (w - c) / fb;
    uint64_t target = (uint64_t)HV_USB_AUDIO_RATE * 30 / 1000 + frames; /* 30 ms besides this pull */
    if (!a->primed) {
        if (have < target) return 0;
        a->primed = 1;
    }
    if (have > target + (uint64_t)HV_USB_AUDIO_RATE * 70 / 1000) { /* too much waits: play the newest */
        c += (have - target) * fb;
        have = target;
        a->skipped++;
    }
    uint32_t n = have < frames ? (uint32_t)have : frames;
    unsigned bytes = fb / 2;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t f[8];
        for (unsigned k = 0; k < fb; k++) f[k] = a->ring[(c + k) % HV_USB_AUDIO_RING];
        out[2 * i] = sample(f, bytes) * gain;
        out[2 * i + 1] = sample(f + bytes, bytes) * gain;
        c += fb;
    }
    if (n < frames) { /* ran dry: wait for 30 ms again */
        a->primed = 0;
        a->underruns++;
    }
    atomic_store_explicit(&a->consumed, c, memory_order_release);
    return n;
}

/* ---- the device ---- */

HvUsbAudio *hv_usb_audio_create(HvMachine *m, const char *name) {
    HvUsbAudio *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    a->ring = calloc(1, HV_USB_AUDIO_RING);
    if (!a->ring) {
        free(a);
        return NULL;
    }
    a->m = m;
    pthread_mutex_init(&a->mu, NULL);
    a->vol = HV_USB_AUDIO_VOL_MAX;
    a->cfg_len = build_config(a->cfg);
    hv_usb_audio_set_name(a, name);
    return a;
}

void hv_usb_audio_free(HvUsbAudio *a) {
    if (!a) return;
    pthread_mutex_destroy(&a->mu);
    free(a->ring);
    free(a);
}

void hv_usb_audio_set_name(HvUsbAudio *a, const char *name) {
    snprintf(a->name, sizeof a->name, "%s", name && *name ? name : "Henge Speaker");
}

void hv_usb_audio_get_state(HvUsbAudio *a, HvUsbAudioState *s) {
    memset(s, 0, sizeof *s);
    s->configured = a->configured;
    s->alt = a->alt;
    hv_usb_audio_get_volume(a, &s->vol, &s->mute);
}

int hv_usb_audio_set_state(HvUsbAudio *a, const HvUsbAudioState *s) {
    if (s->configured < 0 || s->configured > 1 || s->alt < 0 || s->alt > 2 || (s->alt && !s->configured) ||
        s->vol < HV_USB_AUDIO_VOL_MIN || s->vol > HV_USB_AUDIO_VOL_MAX || (s->mute != 0 && s->mute != 1))
        return -1;
    set_alt(a, 0);
    pthread_mutex_lock(&a->mu);
    a->configured = s->configured;
    a->vol = clamp_vol(s->vol);
    a->mute = s->mute;
    a->shead = a->stail, a->hhead = a->htail;
    a->hid_cur = 0;
    atomic_store(&a->pending, 0);
    pthread_mutex_unlock(&a->mu);
    set_alt(a, s->alt); /* the stream goes on where it was: the host plays it again */
    return 0;
}
