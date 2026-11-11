/* Suspend, the devices (devices/vmapple_snap.c): a board whose devices are in the middle of things
   (a disk request under way, input waiting for buffers, a character waiting in the UART, interrupts
   raised) is saved, loaded into a second board built the same way, and each device must carry on
   there as it would have on the first. Then the second board saved again must give the same bytes
   (nothing lost or changed on the way), and every state that can't be saved or loaded is refused. */
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "snapshot.h"
#include "pvg_lite.h"
#include "virtio_net.h"
#include "vmapple.h"

#define RAM_SIZE (64ull << 20)

static int checks, failures;
static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: %llx, expected %llx\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

typedef struct {
    HvMachine m;
    HvCpu cpu;
    HvVmapple b;
    HvDisk root, aux;
} Board;

static Board A, B, C, D, E;
static Board *cur; /* the board the guest-side helpers talk to */

static uint32_t rd32(uint64_t pa) {
    uint32_t v = 0;
    hv_phys_read(&cur->m, pa, &v, 4);
    return v;
}
static uint8_t rd8(uint64_t pa) {
    uint8_t v = 0;
    hv_phys_read(&cur->m, pa, &v, 1);
    return v;
}
static uint64_t rd64(uint64_t pa) {
    uint64_t v = 0;
    hv_phys_read(&cur->m, pa, &v, 8);
    return v;
}
static void wr32(uint64_t pa, uint32_t v) { hv_phys_write(&cur->m, pa, &v, 4); }
static void wr16(uint64_t pa, uint16_t v) { hv_phys_write(&cur->m, pa, &v, 2); }
static void wr8(uint64_t pa, uint8_t v) { hv_phys_write(&cur->m, pa, &v, 1); }
static void wr64(uint64_t pa, uint64_t v) { hv_phys_write(&cur->m, pa, &v, 8); }
static uint8_t *ram(uint64_t pa) { return hv_phys_ptr(&cur->m, pa, 1); }

static int make_disk(const char *path, uint64_t size, uint8_t salt) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    static uint8_t buf[4096];
    for (uint64_t off = 0; off < size; off += sizeof buf) {
        for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)((off + i) / 512 * 7 + i + salt);
        if (write(fd, buf, sizeof buf) != (ssize_t)sizeof buf) return close(fd), -1;
    }
    close(fd);
    return 0;
}

static int build(Board *x, const char *root, const char *aux, const char *share) {
    if (hv_machine_init(&x->m, 4 << 20) != 0) return -1;
    if (root && hv_disk_open(&x->root, root, 0, 0) != 0) return -1;
    if (aux && hv_disk_open(&x->aux, aux, 16384, 0) != 0) return -1;
    HvCfgParams cfg = {.nr_cpus = 1, .ecid = 0x1234, .mac_en0 = {2, 0, 0, 0x12, 0x34, 0x56}};
    static const uint8_t mac[6] = {2, 0, 0, 0x12, 0x34, 0x56};
    if (hv_vmapple_create(&x->b, &x->m, &x->cpu, RAM_SIZE, root ? &x->root : NULL, aux ? &x->aux : NULL, &cfg, mac) != 0) return -1;
    if (share) {
        const char *names[] = {"iPad"}, *paths[] = {share};
        const int ro[] = {0};
        if (hv_vmapple_share_folders(&x->b, names, paths, ro, 1) != 1) return -1;
    }
    hv_vmapple_reset_cpu(&x->b);
    return 0;
}

/* ---- a small virtio driver (as tests/test_devices.c) ---- */

static int find_slot(uint16_t vendor, uint16_t device, int nth) {
    for (int s = 0; s < 32; s++) {
        uint32_t id = rd32(VMAPPLE_PCIE_ECAM + ((uint64_t)s << 15));
        if (id == ((uint32_t)device << 16 | vendor) && nth-- == 0) return s;
    }
    return -1;
}

static void setup_virtio(int slot, uint64_t bar, uint64_t ring) {
    uint64_t cfg = VMAPPLE_PCIE_ECAM + ((uint64_t)slot << 15);
    wr32(cfg + 0x20, (uint32_t)bar);
    wr32(cfg + 0x24, 0);
    wr16(cfg + 0x04, 0x6);
    wr8(bar + 0x14, 0);
    wr8(bar + 0x14, 1 | 2);
    wr32(bar + 0x00, 1);
    wr32(bar + 0x08, 1);
    wr32(bar + 0x0c, 1);
    wr8(bar + 0x14, 1 | 2 | 8);
    for (int q = 0; q < 2; q++) {
        uint64_t qr = ring + (uint64_t)q * 0x4000;
        memset(ram(qr), 0, 0x4000);
        wr16(bar + 0x16, (uint16_t)q);
        wr16(bar + 0x18, 16);
        wr64(bar + 0x20, qr);
        wr64(bar + 0x28, qr + 0x1000);
        wr64(bar + 0x30, qr + 0x2000);
        wr16(bar + 0x1c, 1);
    }
    wr8(bar + 0x14, 1 | 2 | 8 | 4);
}

static void put_desc(uint64_t table, int i, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next) {
    uint8_t *d = ram(table + 16u * i);
    memcpy(d, &addr, 8);
    memcpy(d + 8, &len, 4);
    memcpy(d + 12, &flags, 2);
    memcpy(d + 14, &next, 2);
}
static void offer(uint64_t ring, uint16_t head) {
    uint16_t *avail = (uint16_t *)ram(ring + 0x1000);
    avail[2 + avail[1] % 16] = head;
    avail[1]++;
}
static uint16_t used_idx(uint64_t ring) { return ((uint16_t *)ram(ring + 0x2000))[1]; }

/* Where the test's guest put things. */
#define BLK_BAR (VMAPPLE_PCIE_MMIO + 0x100000)
#define BLK_RING (VMAPPLE_MEM + 0x100000)
#define BLK_HDR (VMAPPLE_MEM + 0x200000)
#define NET_BAR (VMAPPLE_PCIE_MMIO + 0x200000)
#define NET_RING (VMAPPLE_MEM + 0x300000)
#define NET_BUF (VMAPPLE_MEM + 0x400000)
#define TAB_BAR (VMAPPLE_PCIE_MMIO + 0x400000)
#define TAB_RING (VMAPPLE_MEM + 0x500000)
#define TAB_BUF (VMAPPLE_MEM + 0x600000)

static int blk_slot, net_slot, tab_slot;

#define SND_CONSUMED (3ull * HV_VSND_RING - 100)
#define SND_WRITTEN (SND_CONSUMED + 600)
static unsigned snd_started, snd_args[3];
static int fake_start(void *opaque, unsigned channels, unsigned format, unsigned rate_hz) {
    (void)opaque;
    snd_started++, snd_args[0] = channels, snd_args[1] = format, snd_args[2] = rate_hz;
    return 0;
}
static void fake_stop(void *opaque) { (void)opaque; }

/* A disk read of sectors `sector`.. into BLK_HDR+0x1000, descriptors from 3*k. */
static void blk_read(int k, uint32_t sector) {
    uint64_t hdr = BLK_HDR + 0x4000u * k, data = hdr + 0x1000, status = hdr + 0x3000;
    uint32_t h[4] = {0, 0, sector, 0};
    memcpy(ram(hdr), h, 16);
    put_desc(BLK_RING, 3 * k, hdr, 16, 1, (uint16_t)(3 * k + 1));
    put_desc(BLK_RING, 3 * k + 1, data, 1024, 1 | 2, (uint16_t)(3 * k + 2));
    put_desc(BLK_RING, 3 * k + 2, status, 1, 2, 0);
    ram(status)[0] = 0xff;
    offer(BLK_RING, (uint16_t)(3 * k));
    wr16(BLK_BAR + 0x3000, 0);
}
static int blk_read_ok(int k, uint32_t sector, HvDisk *disk) {
    uint64_t hdr = BLK_HDR + 0x4000u * k;
    static uint8_t want[1024];
    hv_disk_read(disk, (uint64_t)sector * 512, want, 1024);
    return ram(hdr + 0x3000)[0] == 0 && !memcmp(ram(hdr + 0x1000), want, 1024);
}

static uint8_t sent[2048];
static uint32_t sent_len;
static void capture_tx(void *opaque, const uint8_t *frame, uint32_t len) {
    (void)opaque;
    memcpy(sent, frame, len < sizeof sent ? len : sizeof sent);
    sent_len = len;
}
static void net_send(int k, uint8_t first) {
    uint64_t txq = NET_RING + 0x4000, buf = NET_BUF + 0x8000 + 0x100u * k;
    memset(ram(buf), 0, 12);
    for (int i = 0; i < 60; i++) ram(buf + 12)[i] = (uint8_t)(first + i);
    put_desc(txq, k, buf, 72, 0, 0);
    offer(txq, (uint16_t)k);
    wr16(NET_BAR + 0x3000 + 4, 1);
}


static int load(Board *x, const char *path);

/* ---- shared folders: FUSE requests as macOS's driver sends them (the virtio transport is tested above) ---- */

enum { OP_LOOKUP = 1, OP_GETATTR = 3, OP_WRITE = 16, OP_OPEN = 14, OP_READ = 15, OP_INIT = 26, OP_OPENDIR = 27, OP_READDIR = 28 };
static char share_e[1100];
static uint8_t fuse_out[1 << 20];

/* One request: the error (0 or a negative Linux errno), the answer's body in fuse_out + 16. */
static int fuse(Board *x, uint32_t op, uint64_t node, const void *arg, uint32_t alen, uint32_t *body_len) {
    static uint8_t in[4096];
    static uint64_t unique;
    uint32_t len = 40 + alen;
    memset(in, 0, 40);
    memcpy(in, &len, 4);
    memcpy(in + 4, &op, 4);
    unique++;
    memcpy(in + 8, &unique, 8);
    memcpy(in + 16, &node, 8);
    memcpy(in + 40, arg, alen);
    uint32_t n = hv_fuse_handle(&x->b.fs->fuse, in, len, fuse_out, sizeof fuse_out);
    int32_t err = 0;
    if (n >= 16) memcpy(&err, fuse_out + 4, 4);
    if (body_len) *body_len = n >= 16 ? n - 16 : 0;
    return n < 16 ? -999 : err;
}
static uint64_t lookup(Board *x, uint64_t parent, const char *name) {
    if (fuse(x, OP_LOOKUP, parent, name, (uint32_t)strlen(name) + 1, NULL) != 0) return 0;
    uint64_t id;
    memcpy(&id, fuse_out + 16, 8);
    return id;
}
static uint64_t open_node(Board *x, uint32_t op, uint64_t node, uint32_t flags) {
    uint32_t a[2] = {flags, 0};
    if (fuse(x, op, node, a, sizeof a, NULL) != 0) return 0;
    uint64_t fh;
    memcpy(&fh, fuse_out + 16, 8);
    return fh;
}
static int fuse_io(Board *x, uint32_t op, uint64_t node, uint64_t fh, uint64_t offset, uint32_t size, const void *data, uint32_t *body_len) {
    uint8_t a[40 + 64] = {0};
    memcpy(a, &fh, 8);
    memcpy(a + 8, &offset, 8);
    memcpy(a + 16, &size, 4);
    if (data) memcpy(a + 40, data, size);
    return fuse(x, op, node, a, 40 + (data ? size : 0), body_len);
}
static int has_name(const uint8_t *body, uint32_t len, const char *name) {
    for (uint32_t at = 0; at + 24 <= len;) {
        uint32_t nl;
        memcpy(&nl, body + at + 16, 4);
        if (nl == strlen(name) && !memcmp(body + at + 24, name, nl)) return 1;
        at += (24 + nl + 7) & ~7u;
    }
    return 0;
}

static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}
static int make_share(const char *path) {
    char p[1200];
    if (mkdir(path, 0755) != 0) return -1;
    snprintf(p, sizeof p, "%s/a.txt", path);
    if (write_file(p, "hello world")) return -1;
    snprintf(p, sizeof p, "%s/gone.txt", path);
    if (write_file(p, "soon gone")) return -1;
    snprintf(p, sizeof p, "%s/sub", path);
    if (mkdir(p, 0755) != 0) return -1;
    snprintf(p, sizeof p, "%s/sub/b.txt", path);
    return write_file(p, "in a folder");
}

static uint64_t n_export, n_a, n_gone, n_sub, n_b, fh_a, fh_gone, fh_dir, saved_next_id;

static void fuse_guest(void) {
    uint32_t a[16] = {7, 31, 0x20000, 0};
    check("A: FUSE INIT", fuse(&A, OP_INIT, 0, a, sizeof a, NULL), 0);
    n_export = lookup(&A, 1, "iPad");
    n_a = lookup(&A, n_export, "a.txt");
    n_gone = lookup(&A, n_export, "gone.txt");
    n_sub = lookup(&A, n_export, "sub");
    n_b = lookup(&A, n_sub, "b.txt");
    check("A: looked up five nodes", n_export && n_a && n_gone && n_sub && n_b, 1);
    fh_a = open_node(&A, OP_OPEN, n_a, 2 /* O_RDWR */);
    fh_gone = open_node(&A, OP_OPEN, n_gone, 0);
    fh_dir = open_node(&A, OP_OPENDIR, n_export, 0);
    uint32_t len = 0;
    check("A: three handles open", fh_a && fh_gone && fh_dir, 1);
    check("A: read", fuse_io(&A, OP_READ, n_a, fh_a, 0, 5, NULL, &len) == 0 && len == 5 && !memcmp(fuse_out + 16, "hello", 5), 1);
    check("A: folder listing started", fuse_io(&A, OP_READDIR, n_export, fh_dir, 0, 64, NULL, &len) == 0 && len > 0, 1);
    saved_next_id = A.b.fs->fuse.next_id;
}

static void fuse_carries_on(void) {
    uint32_t len = 0;
    check("B: read on the handle A opened", fuse_io(&B, OP_READ, n_a, fh_a, 6, 5, NULL, &len) == 0 && len == 5 &&
                                                !memcmp(fuse_out + 16, "world", 5), 1);
    check("B: write on it", fuse_io(&B, OP_WRITE, n_a, fh_a, 0, 5, "HELLO", NULL), 0);
    char p[1200], got[16] = {0};
    snprintf(p, sizeof p, "%s/a.txt", B.b.fs->fuse.exports[0].path);
    int fd = open(p, O_RDONLY);
    check("B: the write reached B's folder", fd >= 0 && read(fd, got, 11) == 11 && !strcmp(got, "HELLO world"), 1);
    if (fd >= 0) close(fd);
    check("B: the other open file reads", fuse_io(&B, OP_READ, n_gone, fh_gone, 0, 4, NULL, &len) == 0 && len == 4, 1);
    check("B: a node in a subfolder", fuse(&B, OP_GETATTR, n_b, (uint64_t[2]){0, 0}, 16, NULL), 0);
    check("B: the folder listing carries on", fuse_io(&B, OP_READDIR, n_export, fh_dir, 2, 4096, NULL, &len) == 0 &&
                                                   has_name(fuse_out + 16, len, "sub") && has_name(fuse_out + 16, len, "a.txt"), 1);
    snprintf(p, sizeof p, "%s/new.txt", B.b.fs->fuse.exports[0].path);
    write_file(p, "new");
    uint64_t n_new = lookup(&B, n_export, "new.txt");
    check("B: a new node never reuses a saved ID", n_new >= saved_next_id, 1);
    check("B: the same name is the same node", lookup(&B, n_export, "a.txt"), n_a);
}

/* A file deleted while the Mac was saved: its handle answers "bad file", the others carry on. */
static void shared_folders(const char *snap, const char *root, const char *aux) {
    char p[1200];
    snprintf(p, sizeof p, "%s/gone.txt", share_e);
    unlink(p);
    if (build(&E, root, aux, share_e) != 0) {
        printf("cannot build board E\n");
        failures++;
        return;
    }
    check("E: loads with a file gone", load(&E, snap), 0);
    uint32_t len = 0;
    check("E: the gone file's handle answers EBADF", fuse_io(&E, OP_READ, n_gone, fh_gone, 0, 4, NULL, &len), (uint64_t)-9);
    check("E: the other file still reads", fuse_io(&E, OP_READ, n_a, fh_a, 0, 5, NULL, &len) == 0 && !memcmp(fuse_out + 16, "hello", 5), 1);
    check("E: looking the gone file up says so", lookup(&E, n_export, "gone.txt"), 0);
}

/* ---- saving and loading ---- */

static char dir[1024];

static int save(Board *x, const char *path) {
    HvSnapWriter *w = hv_snap_begin(path, HV_SNAP_NO_SYNC);
    if (!w) return -1;
    if (hv_vmapple_snap_save(&x->b, w) != 0 || hv_snap_put_ram(w, &x->m, 4) != 0) {
        hv_snap_abort(w);
        return -1;
    }
    return hv_snap_commit(w);
}

static int load(Board *x, const char *path) {
    char why[200];
    HvSnapReader *r = hv_snap_open(path, why, sizeof why);
    if (!r) {
        printf("  cannot open %s: %s\n", path, why);
        return -1;
    }
    int rc = hv_snap_get_ram(r, &x->m, 4, HV_SNAP_DIRTY_RAM) != 0 || hv_vmapple_snap_load(&x->b, r) != 0 ? -1 : 0;
    hv_snap_close(r);
    return rc;
}

/* What the restore asks before anything is loaded (hv_vmapple_snap_check): 0 it may go on, -1 refused. */
static int precheck(Board *x, const char *path) {
    char why[200];
    HvSnapReader *r = hv_snap_open(path, why, sizeof why);
    if (!r) return -2;
    int rc = hv_vmapple_snap_check(&x->b, r, why, sizeof why);
    hv_snap_close(r);
    return rc;
}

static const uint32_t device_tags[] = {
    HV_SNAP_TAG('B', 'O', 'R', 'D'), HV_SNAP_TAG('U', 'A', 'R', 'T'), HV_SNAP_TAG('P', 'L', '3', '1'),
    HV_SNAP_TAG('G', 'P', 'I', 'O'), HV_SNAP_TAG('A', 'E', 'S', ' '), HV_SNAP_TAG('B', 'D', 'I', 'F'),
    HV_SNAP_TAG('P', 'C', 'I', 'E'), HV_SNAP_TAG('V', 'B', 'K', 'R'), HV_SNAP_TAG('V', 'B', 'K', 'A'),
    HV_SNAP_TAG('V', 'N', 'E', 'T'), HV_SNAP_TAG('V', 'K', 'B', 'D'), HV_SNAP_TAG('V', 'M', 'O', 'U'),
    HV_SNAP_TAG('V', 'T', 'A', 'B'), HV_SNAP_TAG('V', 'S', 'N', 'D'), HV_SNAP_TAG('V', 'S', 'N', 'I'), HV_SNAP_TAG('X', 'H', 'C', 'I'), HV_SNAP_TAG('V', 'F', 'S', ' '),
    HV_SNAP_GIC,
};

/* Every device section of a and b, byte for byte. */
static void same_sections(const char *a, const char *b) {
    HvSnapReader *ra = hv_snap_open(a, NULL, 0), *rb = hv_snap_open(b, NULL, 0);
    check("both snapshots open", ra && rb, 1);
    if (!ra || !rb) return;
    for (size_t i = 0; i < sizeof device_tags / sizeof *device_tags; i++) {
        HvSnapIn x, y;
        char what[64];
        uint32_t t = device_tags[i];
        snprintf(what, sizeof what, "section %c%c%c%c saved again is the same", t & 0xff, t >> 8 & 0xff, t >> 16 & 0xff, t >> 24);
        int gx = hv_snap_get(ra, t, 1, &x), gy = hv_snap_get(rb, t, 1, &y);
        check(what, gx == 0 && gy == 0 && x.len == y.len && !memcmp(x.p, y.p, x.len), 1);
    }
    hv_snap_close(ra);
    hv_snap_close(rb);
}

/* ---- damaged files that pass their CRC ----

   A CRC only catches accidents. Each device section is re-written with a few bytes changed (a valid CRC on
   the changed bytes) and loaded into a board: the load may refuse it or take it, but must never crash or
   hang, and a board that took it must run its devices (a tick) without either. Everything an index read
   from the file could reach is meant to be checked where it is loaded. */

/* What a damaged file holds besides the device sections above: the newer ones, damaged too (not compared above:
   saved again they may differ), and the screen's, copied as they are, never damaged (what reaches the GPU is not
   fuzzed on this Mac). A file without any of them is refused before its damaged section is read ("the saved state
   has no screen"), so each round is checked to reach the loaders: an undamaged rewrite must load. */
static const uint32_t more_tags[] = {
    HV_SNAP_TAG('P', 'W', 'R', 'S'), HV_SNAP_TAG('U', 'T', 'C', 'H'), HV_SNAP_TAG('U', 'T', 'C', 'F'),
    HV_SNAP_TAG('U', 'S', 'P', 'K'), HV_SNAP_TAG('U', 'V', 'I', 'D'), HV_SNAP_TAG('D', 'R', 'V', 'S'),
};
/* (Never onto a Metal side: main turns the GPU off for the whole test.) */
static const uint32_t kept_tags[] = {HV_SNAP_TAG('P', 'V', 'G', ' '), HV_SNAP_TAG('P', 'V', 'G', 'M'), HV_SNAP_TAG('P', 'V', 'G', 'S')};
#define NTAGS(a) (sizeof a / sizeof *a)
/* The fuzz's sections, each with the version the file has (-1: not in it) */
static uint32_t fuzz_tag(size_t i, int *keep) {
    *keep = i >= NTAGS(device_tags) + NTAGS(more_tags);
    return i < NTAGS(device_tags) ? device_tags[i] : !*keep ? more_tags[i - NTAGS(device_tags)] : kept_tags[i - NTAGS(device_tags) - NTAGS(more_tags)];
}
#define FUZZ_TAGS (NTAGS(device_tags) + NTAGS(more_tags) + NTAGS(kept_tags))

static Board F;
static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) {
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

static void mutate(uint8_t *p, size_t len) {
    if (!len) return;
    static const uint32_t words[] = {0, 1, 2, 5, 6, 0x7f, 0xff, 0x100, 0xffff, 0x10000, 0x7fffffff, 0x80000000u, 0xffffffffu};
    int n = 1 + (int)(rnd() % 3);
    for (int k = 0; k < n; k++) {
        size_t at = rnd() % len;
        switch (rnd() % 4) {
        case 0: p[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: p[at] = (uint8_t)rnd(); break;
        default: { /* an interesting 32-bit value on a 4-byte boundary */
            at &= ~(size_t)3;
            uint32_t v = words[rnd() % (sizeof words / sizeof *words)];
            size_t m = len - at < 4 ? len - at : 4;
            memcpy(p + at, &v, m);
        }
        }
    }
}

/* Writes every section of the reader the devices load, each at its own version, with section `which` replaced by
   `body` (which 0: none). */
static int write_with(HvSnapReader *r, const char *path, uint32_t which, const uint8_t *body, size_t len) {
    HvSnapWriter *w = hv_snap_begin(path, HV_SNAP_NO_SYNC);
    if (!w) return -1;
    for (size_t i = 0; i < FUZZ_TAGS; i++) {
        int keep, v;
        uint32_t t = fuzz_tag(i, &keep);
        HvSnapIn in;
        if ((v = hv_snap_version(r, t)) < 0 || hv_snap_get(r, t, (uint32_t)v, &in) != 0) continue;
        HvSnapOut o = {0};
        if (t == which) hv_snap_out(&o, body, len);
        else hv_snap_out(&o, in.p, in.len);
        if (hv_snap_put(w, t, (uint32_t)v, &o) != 0) return hv_snap_abort(w), -1;
    }
    return hv_snap_commit(w);
}

static int load_devices(Board *x, const char *path) {
    HvSnapReader *r = hv_snap_open(path, NULL, 0);
    if (!r) return -1;
    int rc = hv_vmapple_snap_load(&x->b, r);
    hv_snap_close(r);
    return rc;
}

static void fuzz(const char *snap, const char *root, const char *aux, const char *share) {
    char path[1200];
    snprintf(path, sizeof path, "%s/fuzz.hvsnap", dir);
    HvSnapReader *r = hv_snap_open(snap, NULL, 0);
    check("fuzz: the snapshot opens", r != NULL, 1);
    if (!r || build(&F, root, aux, share) != 0) {
        failures++;
        return;
    }
    F.b.snd->backend = (HvVirtioSndBackend){fake_start, fake_stop, NULL, NULL, NULL};
    int quiet = dup(2), null = open("/dev/null", O_WRONLY);
    int took = 0, refused = 0, unclean = 0, sections = 0;
    /* Undamaged, the rewrite loads: every section a load needs is in it, so the damage below is what is refused or taken */
    check("fuzz: the file rewritten undamaged loads", write_with(r, path, 0, NULL, 0) == 0 && load_devices(&F, path) == 0, 1);
    static uint8_t body[1 << 20];
    for (size_t i = 0; i < FUZZ_TAGS; i++) {
        int keep, v;
        uint32_t tag = fuzz_tag(i, &keep);
        HvSnapIn in;
        if (keep || (v = hv_snap_version(r, tag)) < 0 || hv_snap_get(r, tag, (uint32_t)v, &in) != 0 || in.len > sizeof body) continue;
        sections++;
        for (int round = 0; round < 150; round++) {
            memcpy(body, in.p, in.len);
            mutate(body, in.len);
            if (write_with(r, path, tag, body, in.len) != 0) {
                failures++;
                break;
            }
            dup2(null, 2); /* the refusals say why on stderr: thousands of lines */
            alarm(20);
            if (load_devices(&F, path) == 0) {
                took++;
                hv_vmapple_restored(&F.b);
                F.m.tick(&F.m, &F.cpu); /* the devices run on what they were given */
            } else
                refused++;
            /* The board must still take the good file: a refused load leaves nothing it can't recover from */
            if (load_devices(&F, snap) != 0) {
                dup2(quiet, 2);
                uint32_t t = tag;
                printf("  fuzz: after a damaged %c%c%c%c, the good file is refused\n", t & 0xff, t >> 8 & 0xff, t >> 16 & 0xff, t >> 24);
                unclean++;
                if (build(&F, root, aux, share) != 0) break; /* a new board; the old one is left as it is */
                F.b.snd->backend = (HvVirtioSndBackend){fake_start, fake_stop, NULL, NULL, NULL};
            }
            alarm(0);
            dup2(quiet, 2);
        }
    }
    dup2(quiet, 2);
    close(null);
    close(quiet);
    hv_snap_close(r);
    printf("  fuzz: %d sections damaged, %d damaged files taken, %d refused\n", sections, took, refused);
    check("fuzz: ran", took + refused > 1000, 1);
    check("fuzz: damage the loaders took (lengths and checksums right, the contents wrong) reached the devices", took > 0, 1);
    check("fuzz: no load left a board that can't load the good file again", unclean, 0);
    /* What it took was restored (hv_vmapple_restored): never onto a Metal side, for the GPU is off for this test */
    check("fuzz: no Metal side was made", F.b.pvg != NULL && hv_pvg_lite_metal(F.b.pvg) == NULL, 1);
}

/* ---- the test ---- */

/* Board A, as a guest in the middle of things. */
static void busy_guest(void) {
    cur = &A;
    HvGic *g = A.b.gic;
    /* GIC on, so the levels mean something to a CPU */
    wr32(VMAPPLE_GIC_DIST, 3);

    /* UART: receive interrupt on, two characters waiting */
    wr32(VMAPPLE_UART + 0x38, 1u << 4);
    hv_pl011_receive(A.b.uart, 'h');
    hv_pl011_receive(A.b.uart, 'i');
    check("A: UART receive line up", g->level[32 + VMAPPLE_IRQ_UART], 1);

    /* PL031: the guest set its clock to 1,000,000 */
    wr32(VMAPPLE_RTC + 8, 1000000);

    /* PL061: power button edge on pin 3, not yet taken */
    wr32(VMAPPLE_GPIO + 0x40c, 1u << 3);
    wr32(VMAPPLE_GPIO + 0x410, 1u << 3);
    hv_pl061_set_input(A.b.gpio, 3, 1);
    check("A: GPIO line up", g->level[32 + VMAPPLE_IRQ_GPIO], 1);

    /* Apple's RTC events unmasked, nothing pending */
    wr64(VMAPPLE_RTC_SELECT + 0x38, 0x00ffffffffffffffull);
    wr64(VMAPPLE_RTC_SELECT + 0x20, 6);

    /* The auxiliary disk: one read done and taken, one under way */
    blk_slot = find_slot(0x106b, 0x1a00, 0);
    check("A: disk found", blk_slot >= 0, 1);
    setup_virtio(blk_slot, BLK_BAR, BLK_RING);
    blk_read(0, 5);
    hv_virtio_blk_drain(A.b.aux_blk);
    check("A: first read done", used_idx(BLK_RING) == 1 && blk_read_ok(0, 5, &A.aux), 1);
    rd8(BLK_BAR + 0x1000); /* ISR: line down */
    blk_read(1, 20);
    check("A: second read not yet handed back", used_idx(BLK_RING), 1);

    /* Network: one frame sent, one receive buffer waiting */
    net_slot = find_slot(0x1af4, 0x1041, 0);
    check("A: network found", net_slot >= 0, 1);
    setup_virtio(net_slot, NET_BAR, NET_RING);
    A.b.net->transmit = capture_tx;
    net_send(0, 0x10);
    check("A: frame sent", sent_len == 60 && sent[0] == 0x10, 1);
    put_desc(NET_RING, 0, NET_BUF, 2048, 2, 0);
    offer(NET_RING, 0);
    wr16(NET_BAR + 0x3000, 0);

    /* Touch: its range read, two buffers, four events: two delivered, two waiting */
    tab_slot = find_slot(0x1af4, 0x1052, 2);
    check("A: tablet found", tab_slot >= 0, 1);
    setup_virtio(tab_slot, TAB_BAR, TAB_RING);
    for (uint8_t axis = 0; axis < 2; axis++) {
        wr8(TAB_BAR + 0x2000, 0x12);
        wr8(TAB_BAR + 0x2001, axis);
        rd8(TAB_BAR + 0x2002);
    }
    for (int i = 0; i < 2; i++) put_desc(TAB_RING, i, TAB_BUF + 8u * i, 8, 2, 0), offer(TAB_RING, (uint16_t)i);
    wr16(TAB_BAR + 0x3000, 0);
    hv_virtio_input_send(A.b.tablet, HV_EV_ABS, HV_ABS_X, 111);
    hv_virtio_input_send(A.b.tablet, HV_EV_ABS, HV_ABS_Y, 222);
    hv_virtio_input_send(A.b.tablet, HV_EV_KEY, HV_BTN_SECONDARY, 1);
    hv_virtio_input_send(A.b.tablet, HV_EV_SYN, 0, 0);
    hv_virtio_input_poll(A.b.tablet);
    check("A: two touch events delivered", used_idx(TAB_RING), 2);

    /* Sound playing: audio in the ring across its end, two buffers waiting on it (the indices wrap) */
    HvVirtioSnd *s = A.b.snd;
    s->channels = 2, s->format = 5, s->rate = 7, s->frame_bytes = 4, s->rate_hz = 48000;
    s->buffer_bytes = 0x8000, s->period_bytes = 0x1000, s->prepared = 1, s->running = 1;
    for (uint64_t at = SND_CONSUMED; at < SND_WRITTEN; at++) s->ring[at % HV_VSND_RING] = (uint8_t)(at * 13);
    atomic_store(&s->written, SND_WRITTEN);
    atomic_store(&s->consumed, SND_CONSUMED);
    s->phead = HV_VSND_PENDING + 6, s->pcopy = s->phead + 1, s->ptail = s->phead + 2;
    for (int i = 0; i < 2; i++) {
        HvVirtqElem *e = &s->pending[(s->phead + i) % HV_VSND_PENDING];
        e->head = (uint16_t)(3 + i), e->nseg = 2;
        e->seg[0] = (HvVirtqSeg){0x1000 + i, 8, 0};
        e->seg[1] = (HvVirtqSeg){0x2000 + i, 300, 1};
        s->pending_end[(s->phead + i) % HV_VSND_PENDING] = SND_CONSUMED + 300u * (i + 1);
    }
    s->underruns = 4, s->xfers = 9;
}

static void carries_on(void) {
    cur = &B;
    HvGic *g = B.b.gic;
    check("B: the guest's memory is A's", memcmp(A.b.ram, B.b.ram, RAM_SIZE), 0);

    /* UART */
    check("B: UART receive line still up", g->level[32 + VMAPPLE_IRQ_UART], 1);
    check("B: UART first character", rd32(VMAPPLE_UART) & 0xff, 'h');
    check("B: UART second character", rd32(VMAPPLE_UART) & 0xff, 'i');
    check("B: UART line drops once read", g->level[32 + VMAPPLE_IRQ_UART], 0);

    /* PL031 keeps the guest's setting against real time */
    check("B: PL031 kept the guest's clock", rd32(VMAPPLE_RTC) - 1000000 <= 2, 1);

    /* PL061 */
    check("B: GPIO line still up", g->level[32 + VMAPPLE_IRQ_GPIO], 1);
    check("B: GPIO pin still high", rd32(VMAPPLE_GPIO + (1u << 3 << 2)), 1u << 3);
    wr32(VMAPPLE_GPIO + 0x41c, 1u << 3);
    check("B: GPIO clears", g->level[32 + VMAPPLE_IRQ_GPIO], 0);

    /* The disk: the request under way was finished before saving, reached the guest, and its
       interrupt survived; the next one works (INTx counting rebuilt right) */
    int line = 32 + VMAPPLE_IRQ_PCIE + blk_slot;
    check("B: request under way finished at save", used_idx(BLK_RING) == 2 && blk_read_ok(1, 20, &B.aux), 1);
    check("B: its interrupt is up", g->level[line], 1);
    check("B: ISR", rd8(BLK_BAR + 0x1000), 1);
    check("B: ISR read drops it", g->level[line], 0);
    blk_read(2, 33);
    hv_virtio_blk_drain(B.b.aux_blk);
    check("B: next read done", used_idx(BLK_RING) == 3 && blk_read_ok(2, 33, &B.aux), 1);
    check("B: next read's interrupt", g->level[line], 1);
    rd8(BLK_BAR + 0x1000);

    /* Network: the waiting buffer takes a frame; sending carries on in the ring */
    uint8_t frame[64];
    for (int i = 0; i < 64; i++) frame[i] = (uint8_t)(0xa0 + i);
    check("B: frame received into A's buffer", hv_virtio_net_receive(B.b.net, frame, 64), 1);
    check("B: frame bytes", memcmp(ram(NET_BUF + 12), frame, 64), 0);
    B.b.net->transmit = capture_tx;
    sent_len = 0;
    net_send(1, 0x40);
    check("B: next frame sent", sent_len == 60 && sent[0] == 0x40, 1);
    check("B: send ring used", used_idx(NET_RING + 0x4000), 2);

    /* Touch: the two waiting events reach the next buffers, in order */
    check("B: tablet ready", hv_virtio_input_ready(B.b.tablet), 1);
    for (int i = 2; i < 4; i++) put_desc(TAB_RING, i, TAB_BUF + 8u * i, 8, 2, 0), offer(TAB_RING, (uint16_t)i);
    wr16(TAB_BAR + 0x3000, 0);
    hv_virtio_input_poll(B.b.tablet);
    uint16_t ev[6][4];
    check("B: waiting touch events delivered", used_idx(TAB_RING), 4);
    memcpy(ev, ram(TAB_BUF), 4 * 8);
    check("B: first two as A gave them", ev[0][1] == HV_ABS_X && ev[0][2] == 111 && ev[1][1] == HV_ABS_Y && ev[1][2] == 222, 1);
    check("B: then the button", ev[2][0] == HV_EV_KEY && ev[2][1] == HV_BTN_SECONDARY && ev[2][2] == 1, 1);
    check("B: then SYN", ev[3][0] == HV_EV_SYN, 1);

    /* Sound: plays on from where it was */
    HvVirtioSnd *s = B.b.snd, *a = A.b.snd;
    check("B: sound not started by the load itself (only once it all worked)", snd_started, 0);
    check("B: sound counters", atomic_load(&s->written) == SND_WRITTEN && atomic_load(&s->consumed) == SND_CONSUMED, 1);
    int same = 1;
    for (uint64_t at = SND_CONSUMED; at < SND_WRITTEN; at++) same &= s->ring[at % HV_VSND_RING] == (uint8_t)(at * 13);
    check("B: audio not yet played is there, at its place in the ring", same, 1);
    check("B: waiting buffers", s->phead == a->phead && s->pcopy == a->pcopy && s->ptail == a->ptail, 1);
    for (uint32_t i = s->phead; i != s->ptail; i++) {
        HvVirtqElem *x = &s->pending[i % HV_VSND_PENDING], *y = &a->pending[i % HV_VSND_PENDING];
        check("B: a waiting buffer", x->head == y->head && x->nseg == 2 && !memcmp(x->seg, y->seg, 2 * sizeof *x->seg) &&
                                         s->pending_end[i % HV_VSND_PENDING] == a->pending_end[i % HV_VSND_PENDING], 1);
    }
    check("B: its clock not started before the restore is done", s->clock_start_ns, 0);
}

static void restored(void) {
    cur = &B;
    HvGic *g = B.b.gic;
    hv_vmapple_restored(&B.b);
    check("restored: sound back end started once", snd_started, 1);
    check("restored: with the stream's format", snd_args[0] == 2 && snd_args[1] == 5 && snd_args[2] == 48000, 1);
    check("restored: its clock starts at the saved position", B.b.snd->clock_start_consumed == SND_CONSUMED && B.b.snd->clock_start_ns != 0, 1);
    /* Nobody holds the button now: it is let go */
    for (int i = 4; i < 6; i++) put_desc(TAB_RING, i, TAB_BUF + 8u * i, 8, 2, 0), offer(TAB_RING, (uint16_t)i);
    wr16(TAB_BAR + 0x3000, 0);
    hv_virtio_input_poll(B.b.tablet);
    uint16_t ev[2][4];
    memcpy(ev, ram(TAB_BUF + 32), sizeof ev);
    check("restored: button let go", used_idx(TAB_RING) == 6 && ev[0][0] == HV_EV_KEY && ev[0][1] == HV_BTN_SECONDARY && ev[0][2] == 0, 1);
    check("restored: then SYN", ev[1][0], HV_EV_SYN);
    /* macOS is told its clock is behind */
    check("restored: Apple RTC says woke and restored", rd64(VMAPPLE_RTC_SELECT + 0x28), HV_AVP_RTC_WOKE | HV_AVP_RTC_RESTORED);
    check("restored: its line is up", g->level[32 + VMAPPLE_IRQ_AVP_RTC], 1);
    /* The network behind the device is a new one: the cable goes out, then back in */
    HvVirtioNet *n = B.b.net;
    check("restored: network link down", n && !hv_virtio_net_link_up(n), 1);
    check("restored: the driver is told its config changed", n && (n->vio.status & 4 ? (n->vio.isr & 2) != 0 : 1), 1);
    hv_virtio_net_poll(n);
    check("restored: link still down at once", n && !hv_virtio_net_link_up(n), 1);
    if (n) n->link_up_at = 1; /* as if its time had come */
    uint8_t gen = n ? n->vio.config_gen : 0;
    hv_virtio_net_poll(n);
    check("restored: then the link is back", n && hv_virtio_net_link_up(n) && n->link_up_at == 0 && n->vio.config_gen != gen, 1);

    /* A press still waiting for a buffer when the save came reaches the guest after the restore: the
       release must come after it, or the button stays down. */
    HvVirtioInput *t = B.b.tablet;
    hv_virtio_input_send(t, HV_EV_KEY, HV_BTN_LEFT, 1);
    hv_virtio_input_send(t, HV_EV_SYN, 0, 0);
    uint32_t before = t->tail;
    hv_virtio_input_release_all(t);
    int released = t->tail - before == 2 && t->ring[before % HV_VINPUT_RING].type == HV_EV_KEY &&
                   t->ring[before % HV_VINPUT_RING].code == HV_BTN_LEFT && t->ring[before % HV_VINPUT_RING].value == 0;
    check("restored: a press still queued is let go after it", released, 1);
}

static void refusals(const char *snap, const char *root, const char *small_aux, const char *share_c) {
    char path[1200], why[200];
    /* A file with no device sections */
    snprintf(path, sizeof path, "%s/ram-only.hvsnap", dir);
    HvSnapWriter *w = hv_snap_begin(path, HV_SNAP_NO_SYNC);
    hv_snap_put_ram(w, &A.m, 4);
    check("RAM-only snapshot written", hv_snap_commit(w), 0);
    if (build(&C, root, small_aux, share_c) != 0) {
        printf("cannot build board C\n");
        failures++;
        return;
    }
    check("refused early: no device sections", precheck(&C, path), (uint64_t)-1);
    check("refused: no device sections", load(&C, path) != 0, 1);
    check("refused: an auxiliary disk of another size", load(&C, snap) != 0, 1);

    /* A board without the auxiliary disk: PCI slots differ */
    if (build(&D, root, NULL, NULL) != 0) {
        printf("cannot build board D\n");
        failures++;
        return;
    }
    check("refused early: a board without the auxiliary disk", precheck(&D, snap), (uint64_t)-1);
    check("refused: a board without the auxiliary disk", load(&D, snap) != 0, 1);

    /* What can't be saved yet */
    check("board D can be saved", hv_vmapple_snap_can_save(&D.b, why, sizeof why), 0);
    check("USB drive plugged", hv_xhci_plug(C.b.usb, small_aux, 1, "Test", "0001") >= 2, 1);
    check("refused: a USB drive", hv_vmapple_snap_can_save(&C.b, why, sizeof why) != 0 && strstr(why, "USB") != NULL, 1);
    snprintf(path, sizeof path, "%s/refused.hvsnap", dir);
    check("refused: saving with a USB drive", save(&C, path) != 0, 1);
    check("refused: and no file left", access(path, F_OK) != 0, 1);
    const char *names[] = {"iPad"};
    const char *paths[] = {dir};
    const int ro[] = {0};
    check("shared folder set up", hv_vmapple_share_folders(&D.b, names, paths, ro, 1), 1);
    check("a board with a shared folder can be saved", hv_vmapple_snap_can_save(&D.b, why, sizeof why), 0);

    /* A request that never finishes (a file iCloud is still fetching): the save gives up, it doesn't hang.
       Faked, with the worker held off (its lock) so that it can't take the fake request and finish it. */
    pthread_mutex_lock(&D.b.fs->mu);
    D.b.fs->submitted++;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    snprintf(path, sizeof path, "%s/stuck.hvsnap", dir);
    check("refused: a request that doesn't finish", save(&D, path) != 0, 1);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double took = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    check("gave up after about 3 s", took >= 2.9 && took < 10, 1);
    check("and no file left", access(path, F_OK) != 0, 1);
    D.b.fs->submitted--;
    pthread_mutex_unlock(&D.b.fs->mu);
    check("saves once it is done", save(&D, path), 0);
}

/* ---- the screen's mapper state (PVGS): saved, read ahead, and never a reason to refuse a resume ---- */

#define T_PVG HV_SNAP_TAG('P', 'V', 'G', ' ')
#define T_PVGM HV_SNAP_TAG('P', 'V', 'G', 'M')
#define T_PVGS HV_SNAP_TAG('P', 'V', 'G', 'S')
#define MAP_RING (VMAPPLE_MEM + 0x700000)
static Board G, H;

/* The device sections and the screen's of r, with PVGS as given (version 0: none, as an older Henge saves) */
static int write_pvgs(HvSnapReader *r, const char *path, int version, const uint8_t *body, size_t len) {
    HvSnapWriter *w = hv_snap_begin(path, HV_SNAP_NO_SYNC);
    if (!w) return -1;
    uint32_t tags[sizeof device_tags / sizeof *device_tags + 2];
    size_t n = 0;
    for (size_t i = 0; i < sizeof device_tags / sizeof *device_tags; i++) tags[n++] = device_tags[i];
    tags[n++] = T_PVG, tags[n++] = T_PVGM;
    for (size_t i = 0; i < n; i++) {
        HvSnapIn in;
        if (hv_snap_get(r, tags[i], 1, &in) != 0) continue;
        HvSnapOut o = {0};
        hv_snap_out(&o, in.p, in.len);
        if (hv_snap_put(w, tags[i], 1, &o) != 0) return hv_snap_abort(w), -1;
    }
    if (version) {
        HvSnapOut o = {0};
        hv_snap_out(&o, body, len);
        if (hv_snap_put(w, T_PVGS, (uint32_t)version, &o) != 0) return hv_snap_abort(w), -1;
    }
    return hv_snap_commit(w);
}

/* A file's PVGS: its bytes into bits (1024), and its version (-1 none) */
static int read_pvgs(const char *path, uint8_t *bits) {
    HvSnapReader *r = hv_snap_open(path, NULL, 0);
    if (!r) return -2;
    int v = hv_snap_version(r, T_PVGS);
    HvSnapIn in;
    memset(bits, 0xee, 1024);
    if (v >= 0 && hv_snap_get(r, T_PVGS, (uint32_t)v, &in) == 0 && in.len == 1024) memcpy(bits, in.p, 1024);
    hv_snap_close(r);
    return v;
}
static int bit(const uint8_t *bits, uint32_t id) { return bits[id / 8] >> (id % 8) & 1; }
static int no_bits(const uint8_t *bits) {
    for (int i = 0; i < 1024; i++)
        if (bits[i]) return 0;
    return 1;
}

/* The guest's mapper: a ring of 4 entries {op, id, result, 0}, submitted up to n */
static void mapper_ops(Board *x, const uint32_t (*ops)[2], uint64_t from, uint64_t n) {
    cur = x;
    HvMmio *io = hv_mmio_lookup(&x->m, VMAPPLE_IOSFC);
    io->write(io->opaque, 0x1000, MAP_RING, 8), io->write(io->opaque, 0x1008, 4, 8);
    for (uint64_t i = from; i < n; i++) wr32(MAP_RING + (i % 4) * 16, ops[i - from][0]), wr32(MAP_RING + (i % 4) * 16 + 4, ops[i - from][1]);
    io->write(io->opaque, 0x1018, n, 8);
}

static void mapper_state(const char *root, const char *aux, const char *share) {
    printf("the screen's mapper state (PVGS)\n");
    char p1[1200], p2[1200], p3[1200], p4[1200];
    snprintf(p1, sizeof p1, "%s/pvgs-1.hvsnap", dir);
    snprintf(p2, sizeof p2, "%s/pvgs-2.hvsnap", dir);
    snprintf(p3, sizeof p3, "%s/pvgs-3.hvsnap", dir);
    snprintf(p4, sizeof p4, "%s/pvgs-4.hvsnap", dir);
    if (build(&G, root, aux, share) != 0 || build(&H, root, aux, share) != 0) {
        printf("  cannot build the boards\n");
        failures++;
        return;
    }
    G.b.snd->backend = H.b.snd->backend = (HvVirtioSndBackend){fake_start, fake_stop, NULL, NULL, NULL};
    static const uint32_t ops[4][2] = {{2, 5}, {2, 0x1ffe}, {2, 6}, {1, 6}};
    mapper_ops(&G, ops, 0, 4);
    static uint8_t bits[1024], want[1024];
    check("PVGS: saved", save(&G, p1), 0);
    check("PVGS: in the file, version 1", read_pvgs(p1, want), 1);
    check("PVGS: the ids unmapped (5, 0x1ffe), not one mapped again (6)", bit(want, 5) && bit(want, 0x1ffe) && !bit(want, 6), 1);
    /* Read ahead (a Suspend's capture): what was read then is what is saved */
    char why[200];
    uint64_t deadline = clock_gettime_nsec_np(CLOCK_MONOTONIC) + 5000000000ull;
    check("PVGS: read ahead", hv_vmapple_snap_capture(&G.b, UINT64_MAX, deadline, why, sizeof why), 0);
    static const uint32_t later[1][2] = {{1, 5}};
    mapper_ops(&G, later, 4, 5); /* after the capture: not in what it read */
    check("PVGS: saved from what was read ahead", save(&G, p2), 0);
    hv_vmapple_snap_drop_capture(&G.b);
    check("PVGS: the read-ahead save carries it", read_pvgs(p2, bits), 1);
    check("PVGS: as it was read ahead", !memcmp(bits, want, 1024), 1);
    check("PVGS: saved again, now", save(&G, p3), 0);
    check("PVGS: 5 mapped again since", read_pvgs(p3, bits) == 1 && !bit(bits, 5) && bit(bits, 0x1ffe), 1);
    /* A reader that knows only the screen's own section (an older Henge's) takes the file */
    HvSnapReader *r = hv_snap_open(p1, why, sizeof why);
    HvSnapIn in;
    check("PVGS: the file opens", r != NULL, 1);
    if (!r) return;
    check("PVGS: the screen's section, as an older Henge reads it, is whole",
          hv_snap_get(r, T_PVG, 1, &in) == 0 && hv_pvg_lite_check(in.p, in.len, NULL, 0) == 0 && !hv_snap_has(r, T_PVGM), 1);
    /* Loaded: whole, or unknown (none held) whatever is wrong with it, and never refused */
    struct { const char *what; int version; size_t len; int whole; } cases[] = {
        {"whole", 1, 1024, 1}, {"none (an older Henge's)", 0, 0, 0}, {"version 2", 2, 1024, 0}, {"cut short", 1, 1000, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char what[160];
        int wrote = write_pvgs(r, p4, cases[i].version, want, cases[i].len);
        snprintf(what, sizeof what, "PVGS %s: taken early", cases[i].what);
        check(what, wrote == 0 && precheck(&H, p4) == 0, 1);
        snprintf(what, sizeof what, "PVGS %s: loaded", cases[i].what);
        check(what, load_devices(&H, p4), 0);
        snprintf(what, sizeof what, "PVGS %s: %s", cases[i].what, cases[i].whole ? "the ids restored" : "every id unknown");
        check(what, save(&H, p3) == 0 && read_pvgs(p3, bits) == 1 && (cases[i].whole ? !memcmp(bits, want, 1024) : no_bits(bits)), 1);
    }
    hv_snap_close(r);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* The GPU is off for this test whoever runs it (make test too): the fuzz restores thousands of damaged files, and a
       restore with the screen's GPU state would make a Metal side and do GPU work on this Mac (pvg_lite.c,
       ensure_metal), which has frozen the MacBook */
    setenv("HV_NO_GPU", "1", 1);
    setenv("HV_PVG_NO_METAL", "1", 1);
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    snprintf(dir, sizeof dir, "%s/henge-devsnap.XXXXXX", tmp);
    if (!mkdtemp(dir)) return 1;
    char root[1100], aux_a[1100], aux_b[1100], small_aux[1100], snap1[1100], snap2[1100];
    snprintf(root, sizeof root, "%s/root.img", dir);
    snprintf(aux_a, sizeof aux_a, "%s/aux.img", dir);
    snprintf(aux_b, sizeof aux_b, "%s/aux-b.img", dir);
    snprintf(small_aux, sizeof small_aux, "%s/aux-small.img", dir);
    snprintf(snap1, sizeof snap1, "%s/a.hvsnap", dir);
    snprintf(snap2, sizeof snap2, "%s/b.hvsnap", dir);
    /* B gets its own copy of the same disk contents, so its reads prove nothing came from A's file */
    if (make_disk(root, 1 << 20, 1) || make_disk(aux_a, 256 << 10, 2) || make_disk(aux_b, 256 << 10, 2) ||
        make_disk(small_aux, 128 << 10, 3)) {
        printf("cannot create test disks in %s\n", dir);
        return 1;
    }
    char share_a[1100], share_b[1100], share_c[1100];
    snprintf(share_a, sizeof share_a, "%s/share-a", dir);
    snprintf(share_b, sizeof share_b, "%s/share-b", dir);
    snprintf(share_c, sizeof share_c, "%s/share-c", dir);
    snprintf(share_e, sizeof share_e, "%s/share-e", dir);
    /* The same files in each board's folder, at another path each (as an iPad app's folder moves) */
    if (make_share(share_a) || make_share(share_b) || make_share(share_c) || make_share(share_e)) {
        printf("cannot create the shared folders in %s\n", dir);
        return 1;
    }
    if (build(&A, root, aux_a, share_a) != 0 || build(&B, root, aux_b, share_b) != 0) {
        printf("cannot build the boards\n");
        return 1;
    }

    busy_guest();
    fuse_guest();
    char why[200];
    check("A can be saved", hv_vmapple_snap_can_save(&A.b, why, sizeof why), 0);
    check("A saved", save(&A, snap1), 0);
    cur = &A;
    check("saving finished the request under way", used_idx(BLK_RING), 2);

    B.b.snd->backend = (HvVirtioSndBackend){fake_start, fake_stop, NULL, NULL, NULL};
    check("taken early: B fits", precheck(&B, snap1), 0);
    check("loaded into B", load(&B, snap1), 0);
    check("B saved again", save(&B, snap2), 0);
    same_sections(snap1, snap2);
    carries_on();
    fuse_carries_on();
    restored();
    shared_folders(snap1, root, aux_a);
    refusals(snap1, root, small_aux, share_c);
    fuzz(snap1, root, aux_b, share_b);
    mapper_state(root, aux_b, share_b);

    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0) printf("  (could not remove %s)\n", dir);
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
