/* USB drives on the xHCI controller, driven the way macOS's xHCI and mass storage drivers drive them:
   a drive plugged in while the controller runs, enumerated, configured, SCSI commands over bulk-only
   transport, reads and writes reaching the image file, eject, unplug, and the keyboard on port 1 still
   answering as before. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "machine.h"
#include "pcie.h"
#include "usb_storage.h"
#include "xhci.h"

static int checks, failures;
static void check(const char *what, long long got, long long want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got %lld (%#llx), want %lld (%#llx)\n", what, got, (unsigned long long)got, want, (unsigned long long)want);
    }
}

#define RAM_PA 0x40000000ull
#define RAM_SIZE (32u << 20)
static uint8_t *ram;
static HvMachine m;
static HvXhci *x;

static void *at(uint64_t pa) { return ram + (pa - RAM_PA); }
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

/* A producer ring of 64 TRBs with a link back at the end. */
typedef struct {
    uint64_t base;
    int idx, cycle;
} Ring;

static void ring_init(Ring *r, uint64_t base) {
    memset(at(base), 0, 64 * 16);
    r->base = base, r->idx = 0, r->cycle = 1;
}

static uint64_t ring_put(Ring *r, uint64_t param, uint32_t status, uint32_t ctl) {
    if (r->idx == 63) { /* link, toggling the cycle */
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

/* The event ring. */
#define ER 0x40003000ull
static int er_idx, er_cycle = 1;
typedef struct {
    uint64_t param;
    uint32_t status, ctl;
} Ev;

static int next_event(Ev *e) {
    for (int tries = 0; tries < 2000; tries++) {
        uint64_t t = ER + 16ull * (uint64_t)er_idx;
        uint32_t ctl = r32(t + 12);
        if ((ctl & 1) == (uint32_t)er_cycle) {
            memcpy(&e->param, at(t), 8);
            e->status = r32(t + 8);
            e->ctl = ctl;
            if (++er_idx == 256) er_idx = 0, er_cycle ^= 1;
            reg_w(RT + 0x20 + 0x18, (uint32_t)(ER + 16ull * (uint64_t)er_idx) | 8); /* ERDP, EHB */
            reg_w(RT + 0x20 + 0x1c, (uint32_t)((ER + 16ull * (uint64_t)er_idx) >> 32));
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

/* ---- commands ---- */

static Ring cmd;
static Ev command(uint64_t param, uint32_t ctl) {
    uint64_t t = ring_put(&cmd, param, 0, ctl);
    reg_w(DB, 0);
    Ev e = {0};
    if (!next_event(&e)) printf("FAIL no completion for command at %#llx\n", (unsigned long long)t), failures++;
    check("command completion event", ev_type(&e), 33);
    check("completion is for that command", (long long)e.param, (long long)t);
    return e;
}

#define DCBAA 0x40001000ull
#define INPUT 0x40020000ull
static uint64_t out_ctx(int slot) { return 0x40010000ull + 0x1000ull * (uint64_t)slot; }
static uint64_t ring_base(int slot, int dci) { return 0x40100000ull + 0x10000ull * (uint64_t)slot + 0x1000ull * (uint64_t)dci; }

typedef struct {
    int slot;
    Ring ep[32];
} Dev;

static int enumerate(Dev *d, int port, int speed) {
    Ev e = command(0, 9u << 10); /* Enable Slot */
    check("enable slot", ev_cc(&e), 1);
    d->slot = (int)(e.ctl >> 24);
    w64(DCBAA + 8ull * (uint64_t)d->slot, out_ctx(d->slot));
    memset(at(out_ctx(d->slot)), 0, 1024);
    memset(at(INPUT), 0, 33 * 32);
    w32(INPUT + 4, 3);                                        /* add slot, EP0 */
    w32(INPUT + 32, 1u << 27 | (uint32_t)speed << 20);        /* one context entry */
    w32(INPUT + 32 + 4, (uint32_t)port << 16);                /* root hub port */
    ring_init(&d->ep[1], ring_base(d->slot, 1));
    w32(INPUT + 64 + 4, 4u << 3 | 3u << 1 | 64u << 16);      /* control, CErr 3, 64 bytes */
    w64(INPUT + 64 + 8, d->ep[1].base | 1);
    e = command(INPUT, 11u << 10 | (uint32_t)d->slot << 24);  /* Address Device */
    return ev_cc(&e);
}

/* A control transfer on EP0: returns the bytes the IN data stage got (or -1 on a stall). */
static int control(Dev *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, void *buf) {
    uint64_t data = 0x41000000ull;
    uint8_t setup[8] = {type, req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8), (uint8_t)len, (uint8_t)(len >> 8)};
    uint64_t sp;
    memcpy(&sp, setup, 8);
    int in = type & 0x80;
    ring_put(&d->ep[1], sp, 8, 2u << 10 | 1u << 6 | (uint32_t)(len ? (in ? 3 : 2) : 0) << 16);
    uint64_t dt = 0;
    if (len) {
        memset(at(data), 0xee, len);
        if (!in && buf) memcpy(at(data), buf, len);
        dt = ring_put(&d->ep[1], data, len, 3u << 10 | 1u << 2 | (uint32_t)(in ? 1 : 0) << 16);
    }
    uint64_t st = ring_put(&d->ep[1], 0, 0, 4u << 10 | 1u << 5 | (uint32_t)(in && len ? 0 : 1) << 16);
    reg_w(DB + 4u * (uint32_t)d->slot, 1);
    Ev e;
    int got = len;
    for (;;) {
        if (!next_event(&e)) return -2;
        if (ev_type(&e) != 32) continue;
        if (ev_cc(&e) == 6) return -1; /* stall */
        if (e.param == dt && ev_cc(&e) == 13) got = len - (int)(e.status & 0xffffff);
        if (e.param == st) break;
    }
    if (in && buf && got > 0) memcpy(buf, at(data), (size_t)got);
    return got;
}

static void configure_bulk(Dev *d) {
    memset(at(INPUT), 0, 33 * 32);
    w32(INPUT + 4, 1u | 1u << 3 | 1u << 4);
    w32(INPUT + 32, 4u << 27 | 3u << 20);
    ring_init(&d->ep[3], ring_base(d->slot, 3));
    ring_init(&d->ep[4], ring_base(d->slot, 4));
    w32(INPUT + 32 * 4 + 4, 6u << 3 | 3u << 1 | 512u << 16); /* dci 3: bulk IN */
    w64(INPUT + 32 * 4 + 8, d->ep[3].base | 1);
    w32(INPUT + 32 * 5 + 4, 2u << 3 | 3u << 1 | 512u << 16); /* dci 4: bulk OUT */
    w64(INPUT + 32 * 5 + 8, d->ep[4].base | 1);
    Ev e = command(INPUT, 12u << 10 | (uint32_t)d->slot << 24);
    check("configure endpoint", ev_cc(&e), 1);
}

/* A bulk TD of `len` bytes at buffer pa, split into TRBs of at most `chunk`; waits for its event.
   Returns the bytes moved (len less the residue), or -1 with no event. */
static int bulk(Dev *d, int dci, uint64_t pa, uint32_t len, uint32_t chunk) {
    uint64_t trb[512];
    uint32_t tlen[512];
    int nt = 0;
    uint32_t off = 0;
    do {
        uint32_t n = len - off < chunk ? len - off : chunk;
        int more = off + n < len;
        tlen[nt] = n;
        trb[nt++] = ring_put(&d->ep[dci], pa + off, n, 1u << 10 | 1u << 2 | (more ? 1u << 4 : 1u << 5));
        off += n;
    } while (off < len);
    reg_w(DB + 4u * (uint32_t)d->slot, (uint32_t)dci);
    Ev e;
    for (;;) {
        if (!next_event(&e)) return -1;
        if (ev_type(&e) == 32) break;
    }
    check("bulk event's endpoint", (e.ctl >> 16) & 0x1f, dci);
    int cc = ev_cc(&e);
    if (cc != 1 && cc != 13) {
        printf("FAIL bulk completion code %d\n", cc), failures++;
        return -1;
    }
    if (cc == 1) {
        check("a full TD's event is on its last TRB", (long long)e.param, (long long)trb[nt - 1]);
        return (int)len;
    }
    /* Short: the event is on the TRB it ended in, with that TRB's residue; the ones before moved in full. */
    uint32_t moved = 0;
    for (int i = 0; i < nt; i++) {
        if (trb[i] == e.param) return (int)(moved + tlen[i] - (e.status & 0xffffff));
        moved += tlen[i];
    }
    printf("FAIL short event on a TRB not in the TD\n"), failures++;
    return -1;
}

/* ---- bulk-only transport ---- */

#define CBW_PA 0x41100000ull
#define DATA_PA 0x41200000ull
#define CSW_PA 0x41110000ull
static uint32_t tag = 100;

typedef struct {
    int moved;  /* data stage bytes */
    int status; /* CSW status, -1 if no good CSW */
    uint32_t residue;
} Bot;

static Bot scsi(Dev *d, const uint8_t *cdb, int cdb_len, uint32_t len, int in, uint32_t chunk) {
    Bot b = {0, -1, 0};
    uint8_t cbw[31] = {0};
    uint32_t sig = 0x43425355u, t = ++tag;
    memcpy(cbw, &sig, 4);
    memcpy(cbw + 4, &t, 4);
    memcpy(cbw + 8, &len, 4);
    cbw[12] = in ? 0x80 : 0;
    cbw[14] = (uint8_t)cdb_len;
    memcpy(cbw + 15, cdb, (size_t)cdb_len);
    memcpy(at(CBW_PA), cbw, 31);
    if (bulk(d, 4, CBW_PA, 31, 512) != 31) printf("FAIL CBW not taken\n"), failures++;
    if (len) b.moved = bulk(d, in ? 3 : 4, DATA_PA, len, chunk);
    memset(at(CSW_PA), 0, 13);
    int n = bulk(d, 3, CSW_PA, 13, 512);
    uint32_t csig, ctag;
    memcpy(&csig, at(CSW_PA), 4);
    memcpy(&ctag, at(CSW_PA + 4), 4);
    if (n == 13 && csig == 0x53425355u && ctag == t) {
        memcpy(&b.residue, at(CSW_PA + 8), 4);
        b.status = ((uint8_t *)at(CSW_PA))[12];
    }
    return b;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void rw10(uint8_t *cdb, uint8_t op, uint32_t lba, uint16_t count) {
    memset(cdb, 0, 10);
    cdb[0] = op;
    cdb[2] = (uint8_t)(lba >> 24), cdb[3] = (uint8_t)(lba >> 16), cdb[4] = (uint8_t)(lba >> 8), cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(count >> 8), cdb[8] = (uint8_t)count;
}

static int make_image(const char *path, uint64_t size) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || ftruncate(fd, (off_t)size) != 0) return -1;
    close(fd);
    return 0;
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
    const char *dir = getenv("HV_TEST_WORK") ? getenv("HV_TEST_WORK") : "/tmp";
    char img[512], img2[512];
    snprintf(img, sizeof img, "%s/usb-test-%d.img", dir, getpid());
    snprintf(img2, sizeof img2, "%s/usb-test-ro-%d.img", dir, getpid());
    if (make_image(img, 8 << 20) || make_image(img2, 1 << 20)) {
        printf("cannot make images in %s\n", dir);
        return 1;
    }

    if (hv_machine_init(&m, 4 << 20) != 0) return 1;
    ram = calloc(1, RAM_SIZE);
    hv_machine_add_ram(&m, RAM_PA, RAM_SIZE, ram, 0);
    HvGic *gic = hv_gicv3_create(&m, 0x08000000, 0x080a0000);
    HvPcie *bus = hv_pcie_create(&m, gic, 0x10000000, 0x10000000, 0x50000000, 0x10000000, 32 + 4);
    x = hv_xhci_create(&m, bus);

    /* What macOS's driver does to start the controller. */
    check("every root port (the touchscreen's and camera's too)", reg_r(4) >> 24, HV_XHCI_ROOT_PORTS);
    reg_w(OP + 0, 2); /* HCRST */
    w64(DCBAA, 0);
    reg_w(OP + 0x30, (uint32_t)DCBAA);
    reg_w(OP + 0x34, 0);
    ring_init(&cmd, 0x40002000ull);
    reg_w(OP + 0x18, (uint32_t)cmd.base | 1);
    reg_w(OP + 0x1c, 0);
    w64(0x40004000ull, ER);
    w32(0x40004008ull, 256);
    memset(at(ER), 0, 256 * 16);
    reg_w(RT + 0x20 + 0x08, 1);  /* ERSTSZ */
    reg_w(RT + 0x20 + 0x18, (uint32_t)ER);
    reg_w(RT + 0x20 + 0x1c, 0);
    reg_w(RT + 0x20 + 0x10, 0x40004000u);
    reg_w(RT + 0x20 + 0x14, 0);
    reg_w(RT + 0x20 + 0x00, 2);  /* IE */
    reg_w(OP + 0, 1 | 4);        /* run, interrupts */

    Ev e;
    check("keyboard's port event at start", next_event(&e) && ev_type(&e) == 34 && (e.param >> 24) == 1, 1);
    check("speaker's port event at start", next_event(&e) && ev_type(&e) == 34 && (e.param >> 24) == 7, 1);
    check("drive ports empty", reg_r(PORTSC(2)) & 1, 0);

    /* The keyboard, on port 1, as before. */
    reg_w(PORTSC(1), (1u << 9) | (1u << 17) | (1u << 4)); /* clear CSC, reset */
    check("keyboard port reset event", next_event(&e) && ev_type(&e) == 34, 1);
    Dev kbd = {0};
    check("address the keyboard", enumerate(&kbd, 1, 1), 1);
    uint8_t desc[64];
    check("keyboard device descriptor", control(&kbd, 0x80, 6, 0x100, 0, 18, desc), 18);
    check("keyboard is Apple's", desc[8] | desc[9] << 8, 0x05ac);

    /* A drive plugged in while it runs. */
    int port = hv_xhci_plug(x, img, 0, "Test Disk", "HENGE-T1");
    check("plug: port 2", port, 2);
    hv_xhci_poll(x);
    check("drive arrives: port status change", next_event(&e) && ev_type(&e) == 34 && (e.param >> 24) == 2, 1);
    uint32_t ps = reg_r(PORTSC(2));
    check("connected", ps & 1, 1);
    check("high speed", (ps >> 10) & 0xf, 3);
    check("state: plugged", hv_xhci_drive_state(x, 2), HV_XHCI_DRIVE_PLUGGED);
    reg_w(PORTSC(2), (1u << 9) | (1u << 17) | (1u << 4));
    check("drive port reset event", next_event(&e) && ev_type(&e) == 34, 1);
    check("port enabled", (reg_r(PORTSC(2)) >> 1) & 1, 1);

    Dev drv = {0};
    check("address the drive", enumerate(&drv, 2, 3), 1);
    check("drive device descriptor", control(&drv, 0x80, 6, 0x100, 0, 18, desc), 18);
    check("USB 2.0", desc[2] | desc[3] << 8, 0x200);
    check("high-speed qualifier", control(&drv, 0x80, 6, 0x600, 0, 10, desc), 10);
    uint8_t cfg[64];
    check("config descriptor", control(&drv, 0x80, 6, 0x200, 0, 64, cfg), 32);
    check("mass storage, SCSI, bulk-only", cfg[9 + 5] << 16 | cfg[9 + 6] << 8 | cfg[9 + 7], 0x080650);
    uint8_t str[64];
    int sl = control(&drv, 0x80, 6, 0x303, 0x409, 64, str);
    check("serial string", sl, 2 + 2 * 8);
    check("serial's first letter", str[2], 'H');
    check("set configuration", control(&drv, 0x00, 9, 1, 0, 0, NULL), 0);
    uint8_t lun = 9;
    check("get max LUN", control(&drv, 0xa1, 0xfe, 0, 0, 1, &lun), 1);
    check("one LUN", lun, 0);
    configure_bulk(&drv);
    check("state: in use", hv_xhci_drive_state(x, 2), HV_XHCI_DRIVE_IN_USE);

    /* SCSI. */
    uint8_t cdb[16] = {0x12, 0, 0, 0, 36, 0};
    Bot b = scsi(&drv, cdb, 6, 36, 1, 512);
    check("INQUIRY data", b.moved, 36);
    check("INQUIRY passed", b.status, 0);
    check("removable", ((uint8_t *)at(DATA_PA))[1], 0x80);
    check("product name", memcmp((uint8_t *)at(DATA_PA) + 16, "Test Disk       ", 16), 0);

    memset(cdb, 0, 16);
    cdb[0] = 0x25;
    b = scsi(&drv, cdb, 10, 8, 1, 512);
    check("READ CAPACITY passed", b.status, 0);
    check("last LBA", be32(at(DATA_PA)), (8 << 20) / 512 - 1);
    check("block size", be32((uint8_t *)at(DATA_PA) + 4), 512);

    memset(cdb, 0, 16);
    cdb[0] = 0x00;
    b = scsi(&drv, cdb, 6, 0, 0, 512);
    check("TEST UNIT READY", b.status, 0);

    /* A host buffer longer than the answer: a short packet, and the CSW's residue. */
    memset(cdb, 0, 16);
    cdb[0] = 0x1a, cdb[2] = 0x3f, cdb[4] = 192;
    b = scsi(&drv, cdb, 6, 192, 1, 512);
    check("MODE SENSE short", b.moved, 24);
    check("MODE SENSE residue", b.residue, 192 - 24);
    check("MODE SENSE passed", b.status, 0);
    check("not write protected", ((uint8_t *)at(DATA_PA))[2] & 0x80, 0);

    /* Write 64 KB in 4 KB TRBs, read it back, find it in the file. */
    uint8_t *pat = malloc(65536);
    for (int i = 0; i < 65536; i++) pat[i] = (uint8_t)(i * 7 + 3);
    memcpy(at(DATA_PA), pat, 65536);
    rw10(cdb, 0x2a, 100, 128);
    b = scsi(&drv, cdb, 10, 65536, 0, 4096);
    check("WRITE(10) moved", b.moved, 65536);
    check("WRITE(10) passed", b.status, 0);
    check("WRITE(10) residue", b.residue, 0);
    memset(at(DATA_PA), 0, 65536);
    rw10(cdb, 0x28, 100, 128);
    b = scsi(&drv, cdb, 10, 65536, 1, 16384);
    check("READ(10) moved", b.moved, 65536);
    check("READ(10) passed", b.status, 0);
    check("read back what was written", memcmp(at(DATA_PA), pat, 65536), 0);
    memset(cdb, 0, 16);
    cdb[0] = 0x35;
    b = scsi(&drv, cdb, 10, 0, 0, 512);
    check("SYNCHRONIZE CACHE", b.status, 0);
    {
        uint8_t *f = malloc(65536);
        int fd = open(img, O_RDONLY);
        check("in the image file", pread(fd, f, 65536, 100 * 512) == 65536 && memcmp(f, pat, 65536) == 0, 1);
        close(fd);
        free(f);
    }

    /* Past the end: a failed command, and its sense. */
    rw10(cdb, 0x28, (8 << 20) / 512, 1);
    b = scsi(&drv, cdb, 10, 512, 1, 512);
    check("read past the end fails", b.status, 1);
    check("nothing moved", b.moved, 0);
    memset(cdb, 0, 16);
    cdb[0] = 0x03, cdb[4] = 18;
    b = scsi(&drv, cdb, 6, 18, 1, 512);
    check("REQUEST SENSE", b.moved, 18);
    check("illegal request", ((uint8_t *)at(DATA_PA))[2], 0x05);
    check("LBA out of range", ((uint8_t *)at(DATA_PA))[12], 0x21);

    /* An unknown command. */
    memset(cdb, 0, 16);
    cdb[0] = 0xd7;
    b = scsi(&drv, cdb, 6, 0, 0, 512);
    check("unknown command fails", b.status, 1);
    /* The transport is still in step afterwards. */
    memset(cdb, 0, 16);
    b = scsi(&drv, cdb, 6, 0, 0, 512);
    check("in step after a failure", b.status, 0);

    /* A second drive, read-only. */
    int port2 = hv_xhci_plug(x, img2, 1, "Henge Tools", "HENGE-TOOLS");
    check("second drive: port 3", port2, 3);
    hv_xhci_poll(x);
    check("it arrives too", next_event(&e) && ev_type(&e) == 34 && (e.param >> 24) == 3, 1);
    reg_w(PORTSC(3), (1u << 9) | (1u << 17) | (1u << 4));
    next_event(&e);
    Dev ro = {0};
    check("address it", enumerate(&ro, 3, 3), 1);
    control(&ro, 0x00, 9, 1, 0, 0, NULL);
    configure_bulk(&ro);
    memset(cdb, 0, 16);
    cdb[0] = 0x1a, cdb[2] = 0x3f, cdb[4] = 192;
    b = scsi(&ro, cdb, 6, 192, 1, 512);
    check("write protected", ((uint8_t *)at(DATA_PA))[2] & 0x80, 0x80);
    memset(at(DATA_PA), 0x5a, 512);
    rw10(cdb, 0x2a, 0, 1);
    b = scsi(&ro, cdb, 10, 512, 0, 512);
    check("write to it fails", b.status, 1);
    check("the data is still taken", b.moved, 512);
    memset(cdb, 0, 16);
    cdb[0] = 0x03, cdb[4] = 18;
    scsi(&ro, cdb, 6, 18, 1, 512);
    check("data protect", ((uint8_t *)at(DATA_PA))[2], 0x07);

    /* Both drives work side by side: the first still reads. */
    rw10(cdb, 0x28, 100, 8);
    b = scsi(&drv, cdb, 10, 4096, 1, 4096);
    check("first drive still reads", b.status == 0 && memcmp(at(DATA_PA), pat, 4096) == 0, 1);

    /* Ports run out at four drives. */
    check("third", hv_xhci_plug(x, img2, 1, "c", "c"), 4);
    check("fourth", hv_xhci_plug(x, img2, 1, "d", "d"), 5);
    check("no fifth", hv_xhci_plug(x, img2, 1, "e", "e"), -2);
    check("a missing file", hv_xhci_plug(x, "/nonexistent/disk.img", 1, "f", "f"), -1);
    hv_xhci_unplug(x, 4); /* before they arrive */
    hv_xhci_unplug(x, 5);
    hv_xhci_poll(x);
    check("unplugged before arriving", hv_xhci_drive_state(x, 4), HV_XHCI_DRIVE_EMPTY);
    for (int i = 0; i < 4 && !no_event(); i++) next_event(&e);

    /* macOS ejects the first: START STOP UNIT, eject. */
    memset(cdb, 0, 16);
    cdb[0] = 0x1b, cdb[4] = 0x02;
    b = scsi(&drv, cdb, 6, 0, 0, 512);
    check("eject passed", b.status, 0);
    check("state: ejected", hv_xhci_drive_state(x, 2), HV_XHCI_DRIVE_EJECTED);
    memset(cdb, 0, 16);
    b = scsi(&drv, cdb, 6, 0, 0, 512);
    check("no medium after eject", b.status, 1);

    /* Unplug it: the port goes empty, macOS hears of it, its slot takes nothing more. */
    hv_xhci_unplug(x, 2);
    hv_xhci_poll(x);
    check("drive leaves: port status change", next_event(&e) && ev_type(&e) == 34 && (e.param >> 24) == 2, 1);
    check("disconnected", reg_r(PORTSC(2)) & 1, 0);
    wait_state(2, HV_XHCI_DRIVE_EMPTY, "state: empty");
    ring_put(&drv.ep[4], CBW_PA, 31, 1u << 10 | 1u << 5);
    reg_w(DB + 4u * (uint32_t)drv.slot, 4);
    hv_xhci_poll(x);
    check("a gone drive's doorbell does nothing", no_event(), 1);
    e = command(0, 10u << 10 | (uint32_t)drv.slot << 24); /* Disable Slot */
    check("disable its slot", ev_cc(&e), 1);

    /* Plugged in again, it comes back on the free port. */
    check("plug again", hv_xhci_plug(x, img, 0, "Test Disk", "HENGE-T1"), 2);
    hv_xhci_poll(x);
    check("arrives again", next_event(&e) && ev_type(&e) == 34 && (e.param >> 24) == 2, 1);

    /* The keyboard still types. */
    hv_xhci_key(x, 4, 1);
    memset(at(INPUT), 0, 33 * 32);
    w32(INPUT + 4, 1u | 1u << 3);
    w32(INPUT + 32, 3u << 27 | 1u << 20);
    ring_init(&kbd.ep[3], ring_base(kbd.slot, 3));
    w32(INPUT + 32 * 4 + 4, 7u << 3 | 3u << 1 | 8u << 16); /* interrupt IN */
    w64(INPUT + 32 * 4 + 8, kbd.ep[3].base | 1);
    command(INPUT, 12u << 10 | (uint32_t)kbd.slot << 24);
    ring_put(&kbd.ep[3], DATA_PA, 8, 1u << 10 | 1u << 5);
    reg_w(DB + 4u * (uint32_t)kbd.slot, 3);
    check("key report arrives", next_event(&e) && ev_type(&e) == 32, 1);
    check("the A key", ((uint8_t *)at(DATA_PA))[2], 4);

    /* ⇧⌃⌥⌘ up, whoever holds them (before a key Henge types): the A stays down; nothing is sent when none is. */
    hv_xhci_key(x, 0xe3, 1);
    hv_xhci_release_modifiers(x);
    hv_xhci_release_modifiers(x);
    ring_put(&kbd.ep[3], DATA_PA, 8, 1u << 10 | 1u << 5);
    reg_w(DB + 4u * (uint32_t)kbd.slot, 3);
    check("⌘ goes down", next_event(&e) && ev_type(&e) == 32 && ((uint8_t *)at(DATA_PA))[0] == 0x08, 1);
    ring_put(&kbd.ep[3], DATA_PA, 8, 1u << 10 | 1u << 5);
    reg_w(DB + 4u * (uint32_t)kbd.slot, 3);
    check("and up, the A still down", next_event(&e) && ev_type(&e) == 32 && ((uint8_t *)at(DATA_PA))[0] == 0 &&
                                           ((uint8_t *)at(DATA_PA))[2] == 4, 1);
    ring_put(&kbd.ep[3], DATA_PA, 8, 1u << 10 | 1u << 5);
    reg_w(DB + 4u * (uint32_t)kbd.slot, 3);
    hv_xhci_poll(x);
    check("none down: nothing more sent", no_event(), 1);

    unlink(img);
    unlink(img2);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
