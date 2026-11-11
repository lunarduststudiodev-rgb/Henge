/* Suspend with a USB drive plugged in (devices/vmapple_snap.c, the xHCI section): a drive in the middle of
   a SCSI command is saved with its board, the host plugs the same file in again (by serial number) before
   loading into a second board, and the command carries on there as it would have. TDs the guest had
   queued when the save came are finished first. A drive that isn't plugged in again, or whose file
   changed or is another size, is refused; another drive plugged in meanwhile arrives as a new one. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "snapshot.h"
#include "usb_storage.h"
#include "vmapple.h"
#include "xhci.h"

#define RAM_SIZE (64ull << 20)

static int checks, failures;
static void check(const char *what, long long got, long long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL %s: got %lld (%#llx), want %lld (%#llx)\n", what, got, (unsigned long long)got, want, (unsigned long long)want);
    }
}

typedef struct {
    HvMachine m;
    HvCpu cpu;
    HvVmapple b;
    HvDisk root;
} Board;

static Board A, B, C, D;
static Board *cur;
static HvXhci *x;

static void use(Board *bd) {
    cur = bd;
    x = bd->b.usb;
}

/* Guest addresses, as tests/test_usb_storage.c lays them out, in the board's RAM. */
#define G(off) (VMAPPLE_MEM + (off))
static void *at(uint64_t pa) { return hv_phys_ptr(&cur->m, pa, 1); }
static void w32(uint64_t pa, uint32_t v) { memcpy(at(pa), &v, 4); }
static uint32_t r32(uint64_t pa) {
    uint32_t v;
    memcpy(&v, at(pa), 4);
    return v;
}
static void w64(uint64_t pa, uint64_t v) { memcpy(at(pa), &v, 8); }
static void reg_w(uint64_t off, uint32_t v) { x->pci.bar_write(&x->pci, 0, off, v, 4); }
static uint32_t reg_r(uint64_t off) { return (uint32_t)x->pci.bar_read(&x->pci, 0, off, 4); }

#define OP 0x40
#define PORTSC(p) (0x440 + 16 * ((p) - 1))
#define RT 0x1000
#define DB 0x2000

typedef struct {
    uint64_t base;
    int idx, cycle;
} Ring;

static void ring_init(Ring *r, uint64_t base) {
    memset(at(base), 0, 64 * 16);
    r->base = base, r->idx = 0, r->cycle = 1;
}

static uint64_t ring_put(Ring *r, uint64_t param, uint32_t status, uint32_t ctl) {
    if (r->idx == 63) {
        uint64_t l = r->base + 63 * 16;
        w64(l, r->base);
        w32(l + 8, 0);
        w32(l + 12, 6u << 10 | 2u | (uint32_t)r->cycle);
        r->idx = 0, r->cycle ^= 1;
    }
    uint64_t t = r->base + 16ull * (uint64_t)r->idx++;
    w64(t, param);
    w32(t + 8, status);
    w32(t + 12, ctl | (uint32_t)r->cycle);
    return t;
}

#define ER G(0x3000)
static int er_idx, er_cycle = 1;
typedef struct {
    uint64_t param;
    uint32_t status, ctl;
} Ev;

static int cam_events; /* port change events from the camera's port */
static int next_event(Ev *e) {
    for (int tries = 0; tries < 2000; tries++) {
        uint64_t t = ER + 16ull * (uint64_t)er_idx;
        uint32_t ctl = r32(t + 12);
        if ((ctl & 1) == (uint32_t)er_cycle) {
            memcpy(&e->param, at(t), 8);
            e->status = r32(t + 8);
            e->ctl = ctl;
            if (++er_idx == 256) er_idx = 0, er_cycle ^= 1;
            reg_w(RT + 0x20 + 0x18, (uint32_t)(ER + 16ull * (uint64_t)er_idx) | 8);
            reg_w(RT + 0x20 + 0x1c, (uint32_t)((ER + 16ull * (uint64_t)er_idx) >> 32));
            /* The board's camera (port 8) comes and goes on its own: not what these drives' checks are about. */
            if (((ctl >> 10) & 0x3f) == 34 && (e->param >> 24) == HV_XHCI_CAMERA_PORT) { cam_events++; continue; }
            return 1;
        }
        hv_xhci_poll(x);
        usleep(500);
    }
    return 0;
}

static int no_event(void) {
    uint32_t ctl = r32(ER + 16ull * (uint64_t)er_idx + 12);
    return (ctl & 1) != (uint32_t)er_cycle;
}

static int ev_type(const Ev *e) { return (e->ctl >> 10) & 0x3f; }
static int ev_cc(const Ev *e) { return e->status >> 24; }

static Ring cmd;
static Ev command(uint64_t param, uint32_t ctl) {
    uint64_t t = ring_put(&cmd, param, 0, ctl);
    reg_w(DB, 0);
    Ev e = {0};
    if (!next_event(&e)) printf("  FAIL no completion for a command\n"), failures++;
    check("command completion", ev_type(&e) == 33 && (long long)e.param == (long long)t, 1);
    return e;
}

#define DCBAA G(0x1000)
#define INPUT G(0x20000)
static uint64_t out_ctx(int slot) { return G(0x10000) + 0x1000ull * (uint64_t)slot; }
static uint64_t ring_base(int slot, int dci) { return G(0x100000) + 0x10000ull * (uint64_t)slot + 0x1000ull * (uint64_t)dci; }

typedef struct {
    int slot;
    Ring ep[32];
} Dev;

static int enumerate(Dev *d, int port, int speed) {
    Ev e = command(0, 9u << 10);
    d->slot = (int)(e.ctl >> 24);
    w64(DCBAA + 8ull * (uint64_t)d->slot, out_ctx(d->slot));
    memset(at(out_ctx(d->slot)), 0, 1024);
    memset(at(INPUT), 0, 33 * 32);
    w32(INPUT + 4, 3);
    w32(INPUT + 32, 1u << 27 | (uint32_t)speed << 20);
    w32(INPUT + 32 + 4, (uint32_t)port << 16);
    ring_init(&d->ep[1], ring_base(d->slot, 1));
    w32(INPUT + 64 + 4, 4u << 3 | 3u << 1 | 64u << 16);
    w64(INPUT + 64 + 8, d->ep[1].base | 1);
    e = command(INPUT, 11u << 10 | (uint32_t)d->slot << 24);
    return ev_cc(&e);
}

static int control(Dev *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, void *buf) {
    uint64_t data = G(0x1000000);
    uint8_t setup[8] = {type, req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)len, (uint8_t)(len >> 8)};
    uint64_t sp;
    memcpy(&sp, setup, 8);
    int in = type & 0x80;
    ring_put(&d->ep[1], sp, 8, 2u << 10 | 1u << 6 | (uint32_t)(len ? (in ? 3 : 2) : 0) << 16);
    if (len) {
        memset(at(data), 0xee, len);
        ring_put(&d->ep[1], data, len, 3u << 10 | 1u << 2 | (uint32_t)(in ? 1 : 0) << 16);
    }
    uint64_t st = ring_put(&d->ep[1], 0, 0, 4u << 10 | 1u << 5 | (uint32_t)(in && len ? 0 : 1) << 16);
    reg_w(DB + 4u * (uint32_t)d->slot, 1);
    Ev e;
    for (;;) {
        if (!next_event(&e)) return -2;
        if (ev_type(&e) != 32) continue;
        if (ev_cc(&e) == 6) return -1;
        if (e.param == st) break;
    }
    if (in && buf && len) memcpy(buf, at(data), len);
    return len;
}

static void configure_bulk(Dev *d) {
    memset(at(INPUT), 0, 33 * 32);
    w32(INPUT + 4, 1u | 1u << 3 | 1u << 4);
    w32(INPUT + 32, 4u << 27 | 3u << 20);
    ring_init(&d->ep[3], ring_base(d->slot, 3));
    ring_init(&d->ep[4], ring_base(d->slot, 4));
    w32(INPUT + 32 * 4 + 4, 6u << 3 | 3u << 1 | 512u << 16);
    w64(INPUT + 32 * 4 + 8, d->ep[3].base | 1);
    w32(INPUT + 32 * 5 + 4, 2u << 3 | 3u << 1 | 512u << 16);
    w64(INPUT + 32 * 5 + 8, d->ep[4].base | 1);
    Ev e = command(INPUT, 12u << 10 | (uint32_t)d->slot << 24);
    check("configure endpoint", ev_cc(&e), 1);
}

/* Queues a one-TRB bulk TD and rings its doorbell, without waiting. Returns the TRB's address. */
static uint64_t queue(Dev *d, int dci, uint64_t pa, uint32_t len) {
    uint64_t t = ring_put(&d->ep[dci], pa, len, 1u << 10 | 1u << 5);
    reg_w(DB + 4u * (uint32_t)d->slot, (uint32_t)dci);
    return t;
}

/* Port change events seen while waiting for transfers, by port. */
static int port_changed[8];

/* The transfer event for the TRB at t: its completion code and the bytes it moved. */
static int transfer_done(uint64_t t, uint32_t len, int *moved) {
    Ev e;
    for (;;) {
        if (!next_event(&e)) return -1;
        if (ev_type(&e) == 32) break;
        if (ev_type(&e) == 34 && (e.param >> 24) < 8) port_changed[e.param >> 24] = 1;
    }
    if ((long long)e.param != (long long)t) return -2;
    *moved = (int)(len - (e.status & 0xffffff));
    return ev_cc(&e);
}

#define CBW_PA G(0x1100000)
#define DATA_PA G(0x1200000)
#define CSW_PA G(0x1110000)

static uint32_t put_cbw(uint32_t tag, const uint8_t *cdb, int cdb_len, uint32_t len, int in) {
    uint8_t cbw[31] = {0};
    uint32_t sig = 0x43425355u;
    memcpy(cbw, &sig, 4);
    memcpy(cbw + 4, &tag, 4);
    memcpy(cbw + 8, &len, 4);
    cbw[12] = in ? 0x80 : 0;
    cbw[14] = (uint8_t)cdb_len;
    memcpy(cbw + 15, cdb, (size_t)cdb_len);
    memcpy(at(CBW_PA), cbw, 31);
    return tag;
}

/* The CSW at CSW_PA: its status if it is the one for tag, else -1. */
static int csw_status(uint32_t tag, uint32_t *residue) {
    uint32_t sig, t;
    memcpy(&sig, at(CSW_PA), 4);
    memcpy(&t, at(CSW_PA + 4), 4);
    memcpy(residue, at(CSW_PA + 8), 4);
    return sig == 0x53425355u && t == tag ? ((uint8_t *)at(CSW_PA))[12] : -1;
}

static void rw10(uint8_t *cdb, uint8_t op, uint32_t lba, uint16_t count) {
    memset(cdb, 0, 10);
    cdb[0] = op;
    cdb[2] = (uint8_t)(lba >> 24), cdb[3] = (uint8_t)(lba >> 16), cdb[4] = (uint8_t)(lba >> 8), cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(count >> 8), cdb[8] = (uint8_t)count;
}

/* An image whose every 512-byte block holds its number and salt, so a read shows which block it got. */
static int make_image(const char *path, uint64_t size, uint8_t salt) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    uint8_t blk[512];
    for (uint64_t lba = 0; lba < size / 512; lba++) {
        for (int i = 0; i < 512; i++) blk[i] = (uint8_t)(lba * 13 + (uint64_t)i + salt);
        if (write(fd, blk, 512) != 512) return close(fd), -1;
    }
    close(fd);
    return 0;
}

static int block_is(uint64_t pa, uint64_t lba, uint8_t salt) {
    uint8_t *p = at(pa);
    for (int i = 0; i < 512; i++)
        if (p[i] != (uint8_t)(lba * 13 + (uint64_t)i + salt)) return 0;
    return 1;
}

static char dir[1024], root[1100];

static int build(Board *bd) {
    memset(bd, 0, sizeof *bd);
    if (hv_machine_init(&bd->m, 4 << 20) != 0) return -1;
    if (hv_disk_open(&bd->root, root, 0, 0) != 0) return -1;
    HvCfgParams cfg = {.nr_cpus = 1, .ecid = 0x1234, .mac_en0 = {2, 0, 0, 0x12, 0x34, 0x56}};
    static const uint8_t mac[6] = {2, 0, 0, 0x12, 0x34, 0x56};
    if (hv_vmapple_create(&bd->b, &bd->m, &bd->cpu, RAM_SIZE, &bd->root, NULL, &cfg, mac) != 0) return -1;
    hv_vmapple_reset_cpu(&bd->b);
    return 0;
}

static int save(Board *bd, const char *path) {
    HvSnapWriter *w = hv_snap_begin(path, HV_SNAP_NO_SYNC);
    if (!w) return -1;
    /* devices first: settling the drives writes their last TDs' events into RAM */
    if (hv_vmapple_snap_save(&bd->b, w) != 0 || hv_snap_put_ram(w, &bd->m, 4) != 0) {
        hv_snap_abort(w);
        return -1;
    }
    return hv_snap_commit(w);
}

static int load(Board *bd, const char *path) {
    char why[200];
    HvSnapReader *r = hv_snap_open(path, why, sizeof why);
    if (!r) return -1;
    int rc = hv_snap_get_ram(r, &bd->m, 4, HV_SNAP_DIRTY_RAM) != 0 || hv_vmapple_snap_load(&bd->b, r) != 0 ? -1 : 0;
    hv_snap_close(r);
    return rc;
}

/* The check a restore makes before it changes anything: 0 if the drives are right, else -1 with why. */
static int precheck(Board *bd, const char *path, char *why, size_t why_len) {
    HvSnapReader *r = hv_snap_open(path, why, why_len);
    if (!r) return -2;
    int rc = hv_vmapple_snap_check(&bd->b, r, why, why_len);
    hv_snap_close(r);
    return rc;
}

/* Refused before loading, with the drive named in the reason and the board untouched. */
static int refused_early(Board *bd, const char *path, const char *words) {
    char why[200] = "";
    return precheck(bd, path, why, sizeof why) == -1 && strstr(why, "\"Work\"") && strstr(why, words);
}

static void wait_state(int port, int want, const char *what) {
    int st = -1;
    for (int i = 0; i < 400 && (st = hv_xhci_drive_state(x, port)) != want; i++) {
        hv_xhci_poll(x);
        usleep(1000);
    }
    check(what, st, want);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    snprintf(dir, sizeof dir, "%s/henge-usbsnap.XXXXXX", tmp);
    if (!mkdtemp(dir)) return 1;
    char img[1100], other[1100], small[1100], snap1[1100], snap2[1100];
    snprintf(root, sizeof root, "%s/root.img", dir);
    snprintf(img, sizeof img, "%s/drive.img", dir);
    snprintf(other, sizeof other, "%s/other.img", dir);
    snprintf(small, sizeof small, "%s/small.img", dir);
    snprintf(snap1, sizeof snap1, "%s/1.hvsnap", dir);
    snprintf(snap2, sizeof snap2, "%s/2.hvsnap", dir);
    if (make_image(root, 1 << 20, 0) || make_image(img, 4 << 20, 5) || make_image(other, 1 << 20, 9) || make_image(small, 2 << 20, 5)) {
        printf("cannot make images in %s\n", dir);
        return 1;
    }
    if (build(&A) != 0) return printf("cannot build board A\n"), 1;
    use(&A);

    /* The controller started, as macOS's driver starts it. */
    reg_w(OP + 0, 2);
    w64(DCBAA, 0);
    reg_w(OP + 0x30, (uint32_t)DCBAA);
    reg_w(OP + 0x34, (uint32_t)(DCBAA >> 32));
    ring_init(&cmd, G(0x2000));
    reg_w(OP + 0x18, (uint32_t)cmd.base | 1);
    reg_w(OP + 0x1c, (uint32_t)(cmd.base >> 32));
    w64(G(0x4000), ER);
    w32(G(0x4008), 256);
    memset(at(ER), 0, 256 * 16);
    reg_w(RT + 0x20 + 0x08, 1);
    reg_w(RT + 0x20 + 0x18, (uint32_t)ER);
    reg_w(RT + 0x20 + 0x1c, (uint32_t)(ER >> 32));
    reg_w(RT + 0x20 + 0x10, (uint32_t)G(0x4000));
    reg_w(RT + 0x20 + 0x14, (uint32_t)(G(0x4000) >> 32));
    reg_w(RT + 0x20 + 0x00, 2);
    reg_w(OP + 0, 1 | 4);
    Ev e;
    check("keyboard's port event", next_event(&e) && ev_type(&e) == 34, 1);
    check("speaker's port event", next_event(&e) && ev_type(&e) == 34 && (e.param >> 24) == 7, 1);

    /* A drive, enumerated and configured. */
    check("plugged in on port 2", hv_xhci_plug(x, img, 0, "Work", "HENGE-W1"), 2);
    hv_xhci_poll(x);
    check("drive arrives", next_event(&e) && ev_type(&e) == 34 && (e.param >> 24) == 2, 1);
    reg_w(PORTSC(2), (1u << 9) | (1u << 17) | (1u << 4));
    check("port reset", next_event(&e) && ev_type(&e) == 34, 1);
    Dev drv = {0};
    check("addressed", enumerate(&drv, 2, 3), 1);
    check("configured", control(&drv, 0x00, 9, 1, 0, 0, NULL), 0);
    configure_bulk(&drv);
    check("A: in use", hv_xhci_drive_state(x, 2), HV_XHCI_DRIVE_IN_USE);
    check("A: a plugged drive can be saved", hv_vmapple_snap_can_save(&A.b, NULL, 0), 0);

    /* The guest queues a whole READ (CBW, data, CSW) and the save comes before any of it is reported:
       the save lets the drive finish it, so its events are in the saved RAM. */
    uint8_t cdb[16];
    rw10(cdb, 0x28, 7, 2);
    uint32_t tag1 = put_cbw(501, cdb, 10, 1024, 1);
    memset(at(DATA_PA), 0, 1024);
    memset(at(CSW_PA), 0, 13);
    uint64_t t_cbw = queue(&drv, 4, CBW_PA, 31);
    uint64_t t_data = queue(&drv, 3, DATA_PA, 1024);
    uint64_t t_csw = queue(&drv, 3, CSW_PA, 13);
    check("A saved with a READ queued", save(&A, snap1), 0);
    check("A: nothing busy after the save", hv_xhci_drives_busy(A.b.usb), 0);

    /* B: the host plugs the same file in again (as it would on launch), then loads. */
    if (build(&B) != 0) return printf("cannot build board B\n"), 1;
    check("B: the drive plugged in again", hv_xhci_plug(B.b.usb, img, 0, "Work", "HENGE-W1"), 2);
    char bwhy[200] = "";
    check("B: the drives check out before loading", precheck(&B, snap1, bwhy, sizeof bwhy), 0);
    check("B loaded", load(&B, snap1), 0);
    check("B: a board that runs can't take a snapshot", precheck(&B, snap1, bwhy, sizeof bwhy), -1);
    use(&B);
    check("B: the drive is in use, not newly arrived", hv_xhci_drive_state(x, 2), HV_XHCI_DRIVE_IN_USE);
    check("B: the camera came back plugged in", hv_xhci_camera_state(x), HV_XHCI_TOUCH_PLUGGED);
    check("B: the camera's port still connected", (int)(reg_r(PORTSC(HV_XHCI_CAMERA_PORT)) & 1), 1);
    check("B: connected", reg_r(PORTSC(2)) & 1, 1);
    int moved = 0;
    check("B: CBW's event", transfer_done(t_cbw, 31, &moved), 1);
    check("B: data's event", transfer_done(t_data, 1024, &moved), 1);
    check("B: 1024 bytes", moved, 1024);
    check("B: block 7", block_is(DATA_PA, 7, 5), 1);
    check("B: block 8", block_is(DATA_PA + 512, 8, 5), 1);
    check("B: CSW's event", transfer_done(t_csw, 13, &moved), 1);
    uint32_t residue = 9;
    check("B: CSW passed for that command", csw_status(tag1, &residue), 0);
    check("B: no residue", residue, 0);
    hv_xhci_poll(x);
    check("B: no port change (it didn't arrive again)", no_event() && !port_changed[2], 1);

    /* B: a WRITE that reaches the file, then a READ saved halfway through its data. */
    rw10(cdb, 0x2a, 3, 1);
    uint32_t tag2 = put_cbw(502, cdb, 10, 512, 0);
    memset(at(DATA_PA), 0x5a, 512);
    t_cbw = queue(&drv, 4, CBW_PA, 31);
    check("B: write's CBW", transfer_done(t_cbw, 31, &moved), 1);
    t_data = queue(&drv, 4, DATA_PA, 512);
    check("B: write's data", transfer_done(t_data, 512, &moved), 1);
    t_csw = queue(&drv, 3, CSW_PA, 13);
    check("B: write's CSW", transfer_done(t_csw, 13, &moved), 1);
    check("B: write passed", csw_status(tag2, &residue), 0);

    rw10(cdb, 0x28, 3, 2);
    uint32_t tag3 = put_cbw(503, cdb, 10, 1024, 1);
    t_cbw = queue(&drv, 4, CBW_PA, 31);
    check("B: read's CBW", transfer_done(t_cbw, 31, &moved), 1);
    memset(at(DATA_PA), 0, 1024);
    t_data = queue(&drv, 3, DATA_PA, 512);
    check("B: first half", transfer_done(t_data, 512, &moved), 1);
    check("B: first half is what was written", ((uint8_t *)at(DATA_PA))[0] == 0x5a && ((uint8_t *)at(DATA_PA))[511] == 0x5a, 1);
    check("B saved mid-command", save(&B, snap2), 0);
    struct stat saved_st;
    stat(img, &saved_st); /* the fuzz pass below puts the file's date back to this */

    /* C: the drive and another one plugged in, both before the load: the saved one goes back on port 2,
       the other arrives as a new drive on a free port. */
    if (build(&C) != 0) return printf("cannot build board C\n"), 1;
    check("C: another drive first", hv_xhci_plug(C.b.usb, other, 1, "Other", "HENGE-O1"), 2);
    check("C: then the saved one", hv_xhci_plug(C.b.usb, img, 0, "Work", "HENGE-W1"), 3);
    check("C loaded", load(&C, snap2), 0);
    use(&C);
    check("C: the saved drive on port 2", hv_xhci_drive_state(x, 2), HV_XHCI_DRIVE_IN_USE);
    t_data = queue(&drv, 3, DATA_PA + 512, 512);
    check("C: second half", transfer_done(t_data, 512, &moved), 1);
    check("C: second half is block 4", block_is(DATA_PA + 512, 4, 5), 1);
    t_csw = queue(&drv, 3, CSW_PA, 13);
    check("C: CSW", transfer_done(t_csw, 13, &moved), 1);
    check("C: that command passed", csw_status(tag3, &residue), 0);
    wait_state(3, HV_XHCI_DRIVE_PLUGGED, "C: the other drive arrives on port 3");
    if (!port_changed[3]) port_changed[3] = next_event(&e) && ev_type(&e) == 34 && (e.param >> 24) == 3;
    check("C: its port change", port_changed[3], 1);
    int fd = open(img, O_RDONLY);
    uint8_t got[512] = {0};
    check("C: the write reached the file", fd >= 0 && pread(fd, got, 512, 3 * 512) == 512 && got[0] == 0x5a && got[511] == 0x5a, 1);
    if (fd >= 0) close(fd);

    /* Refusals: the drive not there, a smaller file with its serial, the same file changed since. */
    if (build(&D) != 0) return printf("cannot build board D\n"), 1;
    check("refused early: the saved drive isn't plugged in", refused_early(&D, snap2, "isn't plugged in"), 1);
    check("refused: the saved drive isn't plugged in", load(&D, snap2) != 0, 1);
    if (build(&D) != 0) return 1;
    hv_xhci_plug(D.b.usb, small, 0, "Work", "HENGE-W1");
    check("refused early: another size", refused_early(&D, snap2, "another size"), 1);
    check("refused: another size", load(&D, snap2) != 0, 1);
    if (build(&D) != 0) return 1;
    hv_xhci_plug(D.b.usb, img, 1, "Work", "HENGE-W1");
    check("refused early: read-only now", refused_early(&D, snap2, "read-only now"), 1);
    check("refused: read-only now", load(&D, snap2) != 0, 1);
    struct timeval tv[2] = {{1000000000, 0}, {1000000000, 0}};
    check("the drive's file touched", utimes(img, tv), 0);
    if (build(&D) != 0) return 1;
    hv_xhci_plug(D.b.usb, img, 0, "Work", "HENGE-W1");
    check("refused early: the file changed since", refused_early(&D, snap2, "changed since"), 1);
    check("refused: the file changed since", load(&D, snap2) != 0, 1);

    /* Saving waits for a plug the host asked for: refused while one is pending. */
    use(&C);
    hv_xhci_plug(x, small, 1, "Third", "HENGE-T3");
    char why[200] = "";
    check("refused: a plug under way", hv_vmapple_snap_can_save(&C.b, why, sizeof why) != 0 && strstr(why, "USB") != NULL, 1);

    /* Damaged USB sections that pass their CRC: refused or taken, never a crash (run it under ASan too) */
    HvSnapReader *r = hv_snap_open(snap2, NULL, 0);
    uint32_t xt = HV_SNAP_TAG('X', 'H', 'C', 'I');
    HvSnapIn xin;
    int took = 0, refused = 0;
    if (r && hv_snap_get(r, xt, 1, &xin) == 0) {
        struct timespec ts[2] = {saved_st.st_atimespec, saved_st.st_mtimespec};
        check("fuzz: the file's saved date put back", utimensat(AT_FDCWD, img, ts, 0), 0);
        static uint8_t body[1 << 16];
        uint64_t rng = 0x2545f4914f6cdd1dull;
        int quiet = dup(2), null = open("/dev/null", O_WRONLY);
        for (int round = 0; round < 121 && xin.len <= sizeof body; round++) {
            memcpy(body, xin.p, xin.len);
            /* round 0 is the undamaged section, which must load; the drive records are at the end: aim most changes there */
            for (int k = 0; k < 2 && round > 0; k++) {
                rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
                size_t span = xin.len > 400 && (rng & 3) ? 400 : xin.len;
                size_t pos = xin.len - span + (size_t)((rng >> 8) % span);
                body[pos] = (rng >> 40) & 1 ? (uint8_t)(rng >> 48) : body[pos] ^ (uint8_t)(1u << ((rng >> 52) % 8));
            }
            char fz[1200];
            snprintf(fz, sizeof fz, "%s/fuzz.hvsnap", dir);
            HvSnapWriter *w = hv_snap_begin(fz, HV_SNAP_NO_SYNC);
            uint32_t tags[] = {HV_SNAP_TAG('B', 'O', 'R', 'D'), HV_SNAP_TAG('U', 'A', 'R', 'T'), HV_SNAP_TAG('P', 'L', '3', '1'),
                               HV_SNAP_TAG('G', 'P', 'I', 'O'), HV_SNAP_TAG('A', 'E', 'S', ' '), HV_SNAP_TAG('B', 'D', 'I', 'F'),
                               HV_SNAP_TAG('P', 'C', 'I', 'E'), HV_SNAP_TAG('V', 'B', 'K', 'R'), HV_SNAP_TAG('V', 'N', 'E', 'T'),
                               HV_SNAP_TAG('V', 'K', 'B', 'D'), HV_SNAP_TAG('V', 'M', 'O', 'U'), HV_SNAP_TAG('V', 'T', 'A', 'B'),
                               HV_SNAP_TAG('V', 'S', 'N', 'D'), HV_SNAP_TAG('P', 'V', 'G', ' '), xt, HV_SNAP_GIC};
            for (size_t i = 0; w && i < sizeof tags / sizeof *tags; i++) {
                HvSnapIn in;
                if (hv_snap_get(r, tags[i], 1, &in) != 0) continue;
                HvSnapOut o = {0};
                hv_snap_out(&o, tags[i] == xt ? body : in.p, in.len);
                hv_snap_put(w, tags[i], 1, &o);
            }
            if (!w || hv_snap_commit(w) != 0) {
                failures++;
                break;
            }
            static Board Z;
            if (build(&Z) != 0) break;
            hv_xhci_plug(Z.b.usb, img, 0, "Work", "HENGE-W1");
            dup2(null, 2);
            HvSnapReader *zr = hv_snap_open(fz, NULL, 0);
            int ok = zr && hv_vmapple_snap_load(&Z.b, zr) == 0;
            if (zr) hv_snap_close(zr);
            if (round == 0) {
                dup2(quiet, 2);
                check("fuzz: the undamaged section loads", ok, 1);
                continue;
            }
            if (ok) {
                took++;
                hv_vmapple_restored(&Z.b);
                for (int t = 0; t < 3; t++) Z.m.tick(&Z.m, &Z.cpu);
            } else
                refused++;
            dup2(quiet, 2);
        }
        close(null);
        close(quiet);
    }
    if (r) hv_snap_close(r);
    printf("  fuzz: %d damaged USB sections taken, %d refused\n", took, refused);
    check("fuzz: ran", took + refused, 120);

    char rm[1200];
    snprintf(rm, sizeof rm, "rm -rf '%s'", dir);
    if (system(rm) != 0) printf("  (could not remove %s)\n", dir);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
