#include "usb_storage.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "machine.h"
#include "saveguard.h"

/* ---- descriptors ---- */

#define EP0_MAX 64
#define BULK_MAX 512

static const uint8_t device_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, EP0_MAX, /* USB 2.0, class per interface */
    0x09, 0x12, 0x67, 0x48,               /* vendor 0x1209 (pid.codes), product 0x4867 */
    0x00, 0x01, 1, 2, 3, 1,               /* bcdDevice 1.00, strings 1-3, one configuration */
};

static const uint8_t config_desc[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 250,              /* one interface, bus powered, 500 mA */
    9, 4, 0, 0, 2, 8, 6, 0x50, 0,                 /* interface 0: mass storage, SCSI, bulk-only */
    7, 5, 0x81, 2, BULK_MAX & 0xff, BULK_MAX >> 8, 0, /* EP1 IN, bulk */
    7, 5, 0x02, 2, BULK_MAX & 0xff, BULK_MAX >> 8, 0, /* EP2 OUT, bulk */
};

static const uint8_t qualifier_desc[10] = {10, 6, 0x00, 0x02, 0, 0, 0, EP0_MAX, 1, 0};

/* ---- SCSI ---- */

enum { ST_CBW, ST_IN, ST_OUT, ST_CSW };
enum { IO_NONE, IO_READ, IO_WRITE };
enum { JOB_NONE, JOB_READ, JOB_WRITE, JOB_FLUSH, JOB_QUIT };

/* Sense keys and what they mean here. */
#define SK_NOT_READY 0x02
#define SK_MEDIUM_ERROR 0x03
#define SK_ILLEGAL 0x05
#define SK_UNIT_ATTENTION 0x06
#define SK_PROTECT 0x07

#define MAX_SEGS 1024

struct HvUsbStorage {
    HvMachine *m;
    HvDisk disk;
    char name[17], serial[33];
    int configured;
    int ejected;          /* atomic: macOS asked for the medium out */

    /* the transfer under way (CPU thread) */
    int state;
    uint32_t tag, host_len, host_done; /* what the CBW said the host moves, and what it has so far */
    int host_in;
    int io;                            /* the data phase reads or writes the disk, or uses resp */
    uint64_t disk_off;
    uint64_t dev_left;                 /* bytes the drive still has (IN) or takes (OUT) */
    uint8_t resp[256];
    uint32_t resp_pos;
    uint8_t status;                    /* the CSW's: 0 passed, 1 failed, 2 phase error */
    uint8_t sense_key, asc, ascq;

    /* the drive's thread */
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int job;                           /* under mu: what the thread is to do, JOB_NONE once done */
    int job_kind;                      /* what it was asked (CPU thread) */
    int busy;                          /* atomic: a job is out (set before handing it over, cleared when taken back) */
    int job_err;
    uint64_t job_off;
    uint32_t job_len;                  /* bytes the job reads or writes */
    uint32_t job_td;                   /* bytes the TD takes in all (an OUT TD past what the drive wants) */
    int job_in;
    int nseg;
    HvUsbSeg seg[MAX_SEGS];
};

static int slog(void) {
    static int on = -1;
    if (on < 0) on = getenv("HV_USB_LOG") != NULL;
    return on;
}

static void sense(HvUsbStorage *s, uint8_t key, uint8_t asc, uint8_t ascq) {
    s->sense_key = key, s->asc = asc, s->ascq = ascq;
    if (key) s->status = 1;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)), put32(p + 4, (uint32_t)v); }

static void padded(uint8_t *dst, const char *src, int n) {
    int i = 0;
    for (; i < n && src[i]; i++) dst[i] = (uint8_t)(src[i] >= 0x20 && src[i] < 0x7f ? src[i] : ' ');
    for (; i < n; i++) dst[i] = ' ';
}

/* The data an IN command answers with: the drive has n bytes of it, the host takes at most alloc. */
static void answer(HvUsbStorage *s, uint32_t n, uint32_t alloc) {
    s->io = IO_NONE;
    s->resp_pos = 0;
    s->dev_left = n < alloc ? n : alloc;
}

/* Runs a command from its CDB: sets the data phase up (io, dev_left, resp) and the status. */
static void command(HvUsbStorage *s, const uint8_t *cdb, int cdb_len) {
    uint8_t op = cdb[0];
    uint64_t blocks = s->disk.size / 512;
    uint8_t *r = s->resp;
    int ejected = __atomic_load_n(&s->ejected, __ATOMIC_ACQUIRE);
    s->io = IO_NONE;
    s->dev_left = 0;
    s->status = 0;
    memset(r, 0, sizeof s->resp);
    if (slog()) fprintf(stderr, "usb-storage %s: SCSI %02x, %u bytes %s\n", s->serial, op, s->host_len, s->host_in ? "in" : "out");
    if (op != 0x03) s->sense_key = s->asc = s->ascq = 0; /* REQUEST SENSE reads the last; anything else starts over */
    switch (op) {
    case 0x00: /* TEST UNIT READY */
        if (ejected) sense(s, SK_NOT_READY, 0x3a, 0); /* medium not present */
        break;
    case 0x03: { /* REQUEST SENSE */
        r[0] = 0x70, r[2] = s->sense_key, r[7] = 10, r[12] = s->asc, r[13] = s->ascq;
        s->sense_key = s->asc = s->ascq = 0;
        answer(s, 18, cdb[4]);
        break;
    }
    case 0x12: { /* INQUIRY */
        uint32_t alloc = (uint32_t)cdb[3] << 8 | cdb[4];
        if (cdb[1] & 1) { /* vital product data */
            if (cdb[2] == 0x00) {
                r[1] = 0x00, r[3] = 2, r[4] = 0x00, r[5] = 0x80;
                answer(s, 6, alloc);
            } else if (cdb[2] == 0x80) {
                int n = (int)strlen(s->serial);
                r[1] = 0x80, r[3] = (uint8_t)n;
                memcpy(r + 4, s->serial, (size_t)n);
                answer(s, 4 + (uint32_t)n, alloc);
            } else
                sense(s, SK_ILLEGAL, 0x24, 0); /* invalid field in CDB */
            break;
        }
        r[0] = 0x00;                        /* a disk */
        r[1] = 0x80;                        /* removable medium: macOS ejects it, and says so */
        r[2] = 0x04, r[3] = 0x02, r[4] = 31; /* SPC-2, response format 2 */
        padded(r + 8, "Henge", 8);
        padded(r + 16, s->name, 16);
        padded(r + 32, "1.0", 4);
        answer(s, 36, alloc);
        break;
    }
    case 0x1a:   /* MODE SENSE (6) */
    case 0x5a: { /* MODE SENSE (10) */
        int six = op == 0x1a;
        uint32_t alloc = six ? cdb[4] : (uint32_t)cdb[7] << 8 | cdb[8];
        int page = cdb[2] & 0x3f;
        uint32_t h = six ? 4 : 8, n = h;
        if (page == 0x08 || page == 0x3f) { /* caching: a write cache, so macOS sends SYNCHRONIZE CACHE */
            r[n] = 0x08, r[n + 1] = 0x12, r[n + 2] = 0x04;
            n += 20;
        }
        uint8_t wp = s->disk.readonly ? 0x80 : 0;
        if (six) r[0] = (uint8_t)(n - 1), r[2] = wp;
        else r[0] = (uint8_t)((n - 2) >> 8), r[1] = (uint8_t)(n - 2), r[3] = wp;
        answer(s, n, alloc);
        break;
    }
    case 0x1b: /* START STOP UNIT */
        if (cdb[4] & 2) { /* load or eject */
            int out = !(cdb[4] & 1);
            __atomic_store_n(&s->ejected, out, __ATOMIC_RELEASE);
            if (out) fprintf(stderr, "usb-storage %s: the Mac ejected it\n", s->serial);
        }
        break;
    case 0x1e: /* PREVENT ALLOW MEDIUM REMOVAL: the iPad can take it any time, the app warns */
    case 0x2f: /* VERIFY (10) */
        if (ejected) sense(s, SK_NOT_READY, 0x3a, 0);
        break;
    case 0x23: /* READ FORMAT CAPACITIES */
        if (ejected) {
            sense(s, SK_NOT_READY, 0x3a, 0);
            break;
        }
        r[3] = 8;
        put32(r + 4, blocks > 0xffffffffull ? 0xffffffffu : (uint32_t)blocks);
        r[8] = 0x02; /* formatted media */
        r[10] = 512 >> 8;
        answer(s, 12, (uint32_t)cdb[7] << 8 | cdb[8]);
        break;
    case 0x25: /* READ CAPACITY (10) */
        if (ejected) {
            sense(s, SK_NOT_READY, 0x3a, 0);
            break;
        }
        put32(r, blocks - 1 > 0xffffffffull ? 0xffffffffu : (uint32_t)(blocks - 1));
        put32(r + 4, 512);
        answer(s, 8, 8);
        break;
    case 0x9e: /* SERVICE ACTION IN: READ CAPACITY (16) */
        if ((cdb[1] & 0x1f) != 0x10) {
            sense(s, SK_ILLEGAL, 0x20, 0);
            break;
        }
        if (ejected) {
            sense(s, SK_NOT_READY, 0x3a, 0);
            break;
        }
        put64(r, blocks - 1);
        put32(r + 8, 512);
        answer(s, 32, be32(cdb + 10));
        break;
    case 0x08: case 0x28: case 0xa8: case 0x88:   /* READ (6, 10, 12, 16) */
    case 0x0a: case 0x2a: case 0xaa: case 0x8a: { /* WRITE (6, 10, 12, 16) */
        int write = op == 0x0a || op == 0x2a || op == 0xaa || op == 0x8a;
        uint64_t lba;
        uint32_t count;
        if (op == 0x08 || op == 0x0a) {
            lba = (uint64_t)(cdb[1] & 0x1f) << 16 | (uint64_t)cdb[2] << 8 | cdb[3];
            count = cdb[4] ? cdb[4] : 256;
        } else if (op == 0x28 || op == 0x2a) {
            lba = be32(cdb + 2);
            count = (uint32_t)cdb[7] << 8 | cdb[8];
        } else if (op == 0xa8 || op == 0xaa) {
            lba = be32(cdb + 2);
            count = be32(cdb + 6);
        } else {
            lba = cdb_len >= 16 ? be64(cdb + 2) : 0;
            count = be32(cdb + 10);
        }
        if (ejected) sense(s, SK_NOT_READY, 0x3a, 0);
        else if (write && s->disk.readonly) sense(s, SK_PROTECT, 0x27, 0); /* write protected */
        else if (lba > blocks || count > blocks - lba) sense(s, SK_ILLEGAL, 0x21, 0); /* LBA out of range */
        else if (count && (s->host_in == write || !s->host_len)) {
            sense(s, SK_ILLEGAL, 0x24, 0); /* the CBW goes the other way: nothing moves */
            s->status = 2;
        } else {
            s->io = write ? IO_WRITE : IO_READ;
            s->disk_off = lba * 512;
            s->dev_left = (uint64_t)count * 512;
        }
        break;
    }
    case 0x35: /* SYNCHRONIZE CACHE (10) */
    case 0x91: /* SYNCHRONIZE CACHE (16): the drive's thread, before the CSW */
        break;
    default:
        if (slog() || s->m->log) fprintf(stderr, "usb-storage %s: SCSI command %02x not supported\n", s->serial, op);
        sense(s, SK_ILLEGAL, 0x20, 0); /* invalid command operation code */
        break;
    }
}

/* ---- the drive's thread ---- */

static void *worker(void *opaque) {
    HvUsbStorage *s = opaque;
    pthread_mutex_lock(&s->mu);
    for (;;) {
        while (s->job == JOB_NONE) pthread_cond_wait(&s->cv, &s->mu);
        if (s->job == JOB_QUIT) break;
        int job = s->job;
        pthread_mutex_unlock(&s->mu);
        int err = 0;
        if (job == JOB_FLUSH) {
            err = hv_disk_flush(&s->disk);
        } else {
            uint64_t off = s->job_off;
            uint32_t left = s->job_len;
            for (int i = 0; i < s->nseg && left && !err; i++) {
                uint32_t n = s->seg[i].len < left ? s->seg[i].len : left;
                if (job == JOB_READ) {
                    /* (the stop waits for this job to finish: a claim here is never one from before it) */
                    if (s->seg[i].pa) hv_guard_claim_range(s->m, s->seg[i].pa, n);
                    err = hv_disk_read(&s->disk, off, s->seg[i].p, n);
                    if (!err) hv_dma_written(s->m, s->seg[i].pa, n);
                } else
                    err = hv_disk_write(&s->disk, off, s->seg[i].p, n);
                off += n, left -= n;
            }
        }
        pthread_mutex_lock(&s->mu);
        s->job_err = err;
        s->job = JOB_NONE;
        hv_machine_wake(s->m);
    }
    pthread_mutex_unlock(&s->mu);
    return NULL;
}

static void start_job(HvUsbStorage *s, int job) {
    s->job_kind = job;
    __atomic_store_n(&s->busy, 1, __ATOMIC_RELEASE);
    pthread_mutex_lock(&s->mu);
    s->job = job;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

/* 1 once the job is back (busy is still set: the caller takes the result, then clears it). */
static int job_back(HvUsbStorage *s) {
    pthread_mutex_lock(&s->mu);
    int back = s->job == JOB_NONE;
    pthread_mutex_unlock(&s->mu);
    return back;
}

/* ---- bulk-only transport ---- */

static uint32_t gather(const HvUsbSeg *seg, int nseg, void *dst, uint32_t n) {
    uint32_t got = 0;
    for (int i = 0; i < nseg && got < n; i++) {
        uint32_t k = seg[i].len < n - got ? seg[i].len : n - got;
        memcpy((uint8_t *)dst + got, seg[i].p, k);
        got += k;
    }
    return got;
}

static uint32_t scatter(HvMachine *m, const HvUsbSeg *seg, int nseg, const void *src, uint32_t n) {
    uint32_t put = 0;
    for (int i = 0; i < nseg && put < n; i++) {
        uint32_t k = seg[i].len < n - put ? seg[i].len : n - put;
        if (seg[i].pa) hv_guard_claim_range(m, seg[i].pa, k); /* an auto-save's guard: before the bytes change */
        memcpy(seg[i].p, (const uint8_t *)src + put, k);
        hv_dma_written(m, seg[i].pa, k);
        put += k;
    }
    return put;
}

static void after_data(HvUsbStorage *s) {
    if (s->host_done == s->host_len) s->state = ST_CSW;
}

static int take_job(HvUsbStorage *s, int in, const HvUsbSeg *seg, int nseg, uint32_t n, uint32_t td) {
    if (nseg > MAX_SEGS) return -1;
    memcpy(s->seg, seg, (size_t)nseg * sizeof *seg);
    s->nseg = nseg;
    s->job_in = in;
    s->job_off = s->disk_off;
    s->job_len = n;
    s->job_td = td;
    start_job(s, in ? JOB_READ : JOB_WRITE);
    return HV_USB_PENDING;
}

int hv_usb_storage_td(HvUsbStorage *s, int in, const HvUsbSeg *seg, int nseg, uint32_t len) {
    if (__atomic_load_n(&s->busy, __ATOMIC_ACQUIRE)) return HV_USB_NAK;
    switch (s->state) {
    case ST_CBW: {
        if (in) return HV_USB_NAK;
        uint8_t cbw[31] = {0};
        uint32_t got = gather(seg, nseg, cbw, sizeof cbw);
        uint32_t sig;
        memcpy(&sig, cbw, 4);
        if (got != 31 || sig != 0x43425355u) { /* not a CBW: let it go */
            if (s->m->log || slog()) fprintf(stderr, "usb-storage %s: a %u-byte OUT transfer that is not a command\n", s->serial, len);
            return (int)len;
        }
        memcpy(&s->tag, cbw + 4, 4);
        memcpy(&s->host_len, cbw + 8, 4);
        s->host_in = cbw[12] >> 7;
        s->host_done = 0;
        int cdb_len = cbw[14] & 0x1f;
        if (cdb_len < 1 || cdb_len > 16) cdb_len = 16;
        command(s, cbw + 15, cdb_len);
        s->state = s->host_len ? (s->host_in ? ST_IN : ST_OUT) : ST_CSW;
        if (cbw[15] == 0x35 || cbw[15] == 0x91) start_job(s, JOB_FLUSH); /* the CSW waits for it */
        return (int)len;
    }
    case ST_IN: {
        if (!in) return HV_USB_NAK;
        uint32_t room = s->host_len - s->host_done;
        if (room > len) room = len;
        uint32_t n = s->dev_left < room ? (uint32_t)s->dev_left : room;
        if (s->io == IO_READ && n) return take_job(s, 1, seg, nseg, n, len);
        if (s->io == IO_NONE && n) n = scatter(s->m, seg, nseg, s->resp + s->resp_pos, n), s->resp_pos += n;
        s->dev_left -= n;
        s->host_done += n;
        if (n < len) s->state = ST_CSW; /* a short packet ends the data stage */
        else after_data(s);
        return (int)n;
    }
    case ST_OUT: {
        if (in) return HV_USB_NAK;
        uint32_t n = s->host_len - s->host_done;
        if (n > len) n = len;
        uint32_t take = s->dev_left < n ? (uint32_t)s->dev_left : n;
        if (s->io == IO_WRITE && take) return take_job(s, 0, seg, nseg, take, n);
        s->dev_left -= take;
        s->host_done += n;
        after_data(s);
        return (int)len;
    }
    default: { /* ST_CSW */
        if (!in) return HV_USB_NAK;
        uint8_t csw[13];
        uint32_t sig = 0x53425355u, residue = s->host_len - s->host_done;
        uint8_t status = s->status;
        if (!status && s->io != IO_NONE && s->dev_left) status = 2; /* the host moved less than the command did */
        memcpy(csw, &sig, 4);
        memcpy(csw + 4, &s->tag, 4);
        memcpy(csw + 8, &residue, 4);
        csw[12] = status;
        if (slog() && status) fprintf(stderr, "usb-storage %s: status %u, sense %02x/%02x\n", s->serial, status, s->sense_key, s->asc);
        s->state = ST_CBW;
        return (int)scatter(s->m, seg, nseg, csw, len < 13 ? len : 13);
    }
    }
}

int hv_usb_storage_done(HvUsbStorage *s, uint32_t *moved) {
    if (!__atomic_load_n(&s->busy, __ATOMIC_ACQUIRE) || !job_back(s)) return 0;
    int err = s->job_err;
    uint32_t n = s->job_len;
    if (s->job_kind == JOB_READ) {
        if (err) {
            sense(s, SK_MEDIUM_ERROR, 0x11, 0); /* unrecovered read error */
            n = 0;
            s->dev_left = 0;
        } else
            s->dev_left -= n, s->disk_off += n;
        s->host_done += n;
        if (n < s->job_td) s->state = ST_CSW;
        else after_data(s);
        *moved = n;
    } else if (s->job_kind == JOB_WRITE) {
        if (err) {
            sense(s, SK_MEDIUM_ERROR, 0x0c, 0); /* write error: the rest is taken and dropped */
            s->dev_left = 0;
        } else
            s->dev_left -= n, s->disk_off += n;
        s->host_done += s->job_td;
        after_data(s);
        *moved = s->job_td; /* an OUT TD is always taken whole */
    } else { /* a cache flush, before the CSW */
        if (err) sense(s, SK_MEDIUM_ERROR, 0x0c, 0);
        *moved = 0;
    }
    __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
    return 1;
}

int hv_usb_storage_busy(HvUsbStorage *s) {
    return __atomic_load_n(&s->busy, __ATOMIC_ACQUIRE);
}

static void bot_reset(HvUsbStorage *s);

/* ---- endpoint 0 ---- */

static int string_desc(const char *str, uint8_t *out) {
    int n = (int)strlen(str);
    if (n > 126) n = 126;
    out[0] = (uint8_t)(2 + 2 * n), out[1] = 3;
    for (int i = 0; i < n; i++) out[2 + 2 * i] = (uint8_t)str[i], out[3 + 2 * i] = 0;
    return 2 + 2 * n;
}

int hv_usb_storage_control(HvUsbStorage *s, const uint8_t setup[8], uint8_t *out, int max) {
    uint8_t type = setup[0], req = setup[1];
    uint16_t value = (uint16_t)(setup[2] | setup[3] << 8);
    uint8_t tmp[256];
    const uint8_t *src = tmp;
    int len;
    if (slog()) fprintf(stderr, "usb-storage %s: request %02x %02x value %04x\n", s->serial, type, req, value);
    if (type == 0x80 && req == 6) { /* GET_DESCRIPTOR */
        uint8_t dt = value >> 8, idx = value & 0xff;
        if (dt == 1) src = device_desc, len = sizeof device_desc;
        else if (dt == 2) src = config_desc, len = sizeof config_desc;
        else if (dt == 6) src = qualifier_desc, len = sizeof qualifier_desc;
        else if (dt == 3 && idx == 0) tmp[0] = 4, tmp[1] = 3, tmp[2] = 0x09, tmp[3] = 0x04, len = 4;
        else if (dt == 3 && idx == 1) len = string_desc("Henge", tmp);
        else if (dt == 3 && idx == 2) len = string_desc(s->name, tmp);
        else if (dt == 3 && idx == 3) len = string_desc(s->serial, tmp);
        else return -1;
    } else if ((type & 0xfc) == 0x80 && req == 0) { /* GET_STATUS: device, interface, endpoint */
        tmp[0] = tmp[1] = 0, len = 2;
    } else if (type == 0x80 && req == 8) { /* GET_CONFIGURATION */
        tmp[0] = (uint8_t)s->configured, len = 1;
    } else if (type == 0x00 && req == 9) { /* SET_CONFIGURATION */
        s->configured = value & 0xff;
        return 0;
    } else if (type == 0x81 && req == 10) { /* GET_INTERFACE */
        tmp[0] = 0, len = 1;
    } else if (type == 0xa1 && req == 0xfe) { /* GET MAX LUN */
        tmp[0] = 0, len = 1;
    } else if (type == 0x21 && req == 0xff) { /* bulk-only mass storage reset: the next transfer is a command */
        bot_reset(s);
        return 0;
    } else if ((type == 0x00 || type == 0x01 || type == 0x02) && (req == 1 || req == 3 || req == 11)) {
        return 0; /* CLEAR_FEATURE (an endpoint halt), SET_FEATURE, SET_INTERFACE */
    } else {
        if (s->m->log || slog()) fprintf(stderr, "usb-storage %s: unhandled request %02x %02x value %04x\n", s->serial, type, req, value);
        return -1;
    }
    if (len > max) len = max;
    memcpy(out, src, (size_t)len);
    return len;
}

int hv_usb_storage_configured(HvUsbStorage *s) {
    return s->configured;
}

static void bot_reset(HvUsbStorage *s) {
    /* A job under way finishes first: its buffers are the guest's. */
    while (hv_usb_storage_busy(s)) {
        uint32_t moved;
        if (!hv_usb_storage_done(s, &moved)) usleep(50);
    }
    s->state = ST_CBW;
    s->io = IO_NONE;
    s->dev_left = s->host_len = s->host_done = 0;
}

void hv_usb_storage_reset(HvUsbStorage *s) {
    bot_reset(s);
    s->configured = 0;
}

/* ---- the drive ---- */

HvUsbStorage *hv_usb_storage_open(HvMachine *m, const char *path, int readonly, const char *name, const char *serial) {
    HvUsbStorage *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->m = m;
    s->disk.fd = -1;
    if (hv_disk_open(&s->disk, path, 0, readonly) != 0 && (readonly || hv_disk_open(&s->disk, path, 0, 1) != 0)) {
        free(s);
        return NULL;
    }
    snprintf(s->name, sizeof s->name, "%s", name && *name ? name : "Disk");
    snprintf(s->serial, sizeof s->serial, "%s", serial && *serial ? serial : "HENGE");
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    if (pthread_create(&s->thread, NULL, worker, s) != 0) {
        hv_disk_close(&s->disk);
        free(s);
        return NULL;
    }
    return s;
}

void hv_usb_storage_free(HvUsbStorage *s) {
    if (!s) return;
    pthread_mutex_lock(&s->mu);
    while (s->job != JOB_NONE) { /* let it finish what it started */
        pthread_mutex_unlock(&s->mu);
        sched_yield();
        pthread_mutex_lock(&s->mu);
    }
    s->job = JOB_QUIT;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->mu);
    pthread_join(s->thread, NULL);
    hv_disk_sync(&s->disk);
    hv_disk_close(&s->disk);
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cv);
    free(s);
}

int hv_usb_storage_ejected(HvUsbStorage *s) {
    return __atomic_load_n(&s->ejected, __ATOMIC_ACQUIRE);
}

int hv_usb_storage_readonly(HvUsbStorage *s) {
    return s->disk.readonly;
}

uint64_t hv_usb_storage_size(HvUsbStorage *s) {
    return s->disk.size;
}

int hv_usb_storage_flush(HvUsbStorage *s, int sync) {
    return sync ? hv_disk_sync(&s->disk) : hv_disk_flush(&s->disk);
}

const char *hv_usb_storage_serial(HvUsbStorage *s) {
    return s->serial;
}

const char *hv_usb_storage_name(HvUsbStorage *s) {
    return s->name;
}

int hv_usb_storage_mtime(HvUsbStorage *s, int64_t *ns) {
    struct stat st;
    if (fstat(s->disk.fd, &st) != 0) return -1;
    *ns = (int64_t)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec;
    return 0;
}

int hv_usb_storage_get_state(HvUsbStorage *s, HvUsbStorageState *st) {
    if (hv_usb_storage_busy(s)) return -1;
    memset(st, 0, sizeof *st);
    st->configured = s->configured;
    st->ejected = __atomic_load_n(&s->ejected, __ATOMIC_ACQUIRE);
    st->state = s->state;
    st->host_in = s->host_in;
    st->io = s->io;
    st->tag = s->tag;
    st->host_len = s->host_len;
    st->host_done = s->host_done;
    st->resp_pos = s->resp_pos;
    st->disk_off = s->disk_off;
    st->dev_left = s->dev_left;
    st->status = s->status;
    st->sense_key = s->sense_key;
    st->asc = s->asc;
    st->ascq = s->ascq;
    memcpy(st->resp, s->resp, sizeof st->resp);
    return 0;
}

int hv_usb_storage_set_state(HvUsbStorage *s, const HvUsbStorageState *st) {
    if (hv_usb_storage_busy(s)) return -1;
    if (st->state < ST_CBW || st->state > ST_CSW || st->io < IO_NONE || st->io > IO_WRITE) return -1;
    if (st->host_done > st->host_len || st->resp_pos > sizeof s->resp || st->status > 2) return -1;
    if (st->io == IO_NONE && st->dev_left > sizeof s->resp - st->resp_pos) return -1;
    if (st->io != IO_NONE && (st->disk_off > s->disk.size || st->dev_left > s->disk.size - st->disk_off)) return -1;
    if (st->io == IO_WRITE && s->disk.readonly) return -1;
    s->configured = st->configured != 0;
    __atomic_store_n(&s->ejected, st->ejected != 0, __ATOMIC_RELEASE);
    s->state = st->state;
    s->host_in = st->host_in != 0;
    s->io = st->io;
    s->tag = st->tag;
    s->host_len = st->host_len;
    s->host_done = st->host_done;
    s->resp_pos = st->resp_pos;
    s->disk_off = st->disk_off;
    s->dev_left = st->dev_left;
    s->status = st->status;
    s->sense_key = st->sense_key;
    s->asc = st->asc;
    s->ascq = st->ascq;
    memcpy(s->resp, st->resp, sizeof s->resp);
    return 0;
}
