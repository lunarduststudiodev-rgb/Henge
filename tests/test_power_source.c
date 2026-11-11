/* The virtual battery (devices/power_source.c), driven the way macOS's
   AppleVirtualPlatformPowerSource drives it: find 0x106b:0x1a07 on the PCI bus,
   negotiate VERSION_1 only, set up queue 0, read the source's kind from config
   byte 0, then keep one buffer (8 bytes out, 12 bytes in) posted and read each
   answer the way the driver does (type 1, f64 charge 0..100, three flags).
   Also: nothing is answered twice, a restarted driver hears the state at once,
   and Suspend carries the device over to a second board that then tells the
   guest the host's battery as it is after the load. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "snapshot.h"
#include "vmapple.h"

#define RAM_SIZE (64ull << 20)

static int checks, failures;
static void check(const char *what, double got, double want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: got %g, want %g\n", what, got, want);
        failures++;
    }
}

typedef struct {
    HvMachine m;
    HvCpu cpu;
    HvVmapple b;
} Board;

static Board A, B, W, O, O2, O3;
static Board *cur;

static uint32_t rd32(uint64_t pa) { uint32_t v = 0; hv_phys_read(&cur->m, pa, &v, 4); return v; }
static uint16_t rd16(uint64_t pa) { uint16_t v = 0; hv_phys_read(&cur->m, pa, &v, 2); return v; }
static uint8_t rd8(uint64_t pa) { uint8_t v = 0; hv_phys_read(&cur->m, pa, &v, 1); return v; }
static void wr32(uint64_t pa, uint32_t v) { hv_phys_write(&cur->m, pa, &v, 4); }
static void wr16(uint64_t pa, uint16_t v) { hv_phys_write(&cur->m, pa, &v, 2); }
static void wr8(uint64_t pa, uint8_t v) { hv_phys_write(&cur->m, pa, &v, 1); }
static void wr64(uint64_t pa, uint64_t v) { hv_phys_write(&cur->m, pa, &v, 8); }
static uint8_t *ram(uint64_t pa) { return hv_phys_ptr(&cur->m, pa, 1); }

static int build(Board *x, int kind) {
    if (hv_machine_init(&x->m, 4 << 20) != 0) return -1;
    HvCfgParams cfg = {.nr_cpus = 1, .ecid = 0x1234};
    if (hv_vmapple_create(&x->b, &x->m, &x->cpu, RAM_SIZE, NULL, NULL, &cfg, NULL) != 0) return -1;
    if (kind >= 0 && hv_vmapple_add_power_source(&x->b, kind) != 0) return -1;
    hv_vmapple_reset_cpu(&x->b);
    return 0;
}

#define BAR (VMAPPLE_PCIE_MMIO + 0x100000)
#define RING (VMAPPLE_MEM + 0x100000)
#define REQ (VMAPPLE_MEM + 0x200000)
#define RESP (VMAPPLE_MEM + 0x200ff9) /* unaligned, as the double in it is */
#define QSIZE 16

static int find_slot(void) {
    for (int s = 0; s < 32; s++)
        if (rd32(VMAPPLE_PCIE_ECAM + ((uint64_t)s << 15)) == 0x1a07106b) return s;
    return -1;
}

/* What initializeDevice() does, in its order. Returns the config byte (the kind). */
static int driver_start(int slot) {
    uint64_t cfg = VMAPPLE_PCIE_ECAM + ((uint64_t)slot << 15);
    wr32(cfg + 0x20, (uint32_t)BAR);
    wr32(cfg + 0x24, 0);
    wr16(cfg + 0x04, 0x6);
    wr8(BAR + 0x14, 0);
    check("status starts at 0 (the driver wants below 2)", rd8(BAR + 0x14), 0);
    wr8(BAR + 0x14, 1);
    wr8(BAR + 0x14, 1 | 2);
    wr32(BAR + 0x00, 1);
    check("device offers VIRTIO_F_VERSION_1 (the driver refuses a device without it)", rd32(BAR + 0x04) & 1, 1);
    wr32(BAR + 0x00, 0);
    check("and nothing else", rd32(BAR + 0x04), 0);
    wr32(BAR + 0x08, 1);
    wr32(BAR + 0x0c, 1);
    wr8(BAR + 0x14, 1 | 2 | 8);
    check("FEATURES_OK kept", rd8(BAR + 0x14) & 8, 8);
    check("one queue", rd16(BAR + 0x12), 1);
    memset(ram(RING), 0, 0x3000);
    wr16(BAR + 0x16, 0);
    wr16(BAR + 0x18, QSIZE);
    wr64(BAR + 0x20, RING);
    wr64(BAR + 0x28, RING + 0x1000);
    wr64(BAR + 0x30, RING + 0x2000);
    wr16(BAR + 0x1c, 1);
    int kind = rd8(BAR + 0x2000);
    wr8(BAR + 0x14, 1 | 2 | 8 | 4);
    return kind;
}

static void put_desc(int i, uint64_t addr, uint32_t len, uint16_t flags, uint16_t next) {
    uint8_t *d = ram(RING + 16u * i);
    memcpy(d, &addr, 8);
    memcpy(d + 8, &len, 4);
    memcpy(d + 12, &flags, 2);
    memcpy(d + 14, &next, 2);
}

/* The transaction: an 8-byte request the driver leaves as it is, then the 12-byte answer. */
static void post(void) {
    memset(ram(RESP), 0xee, 8), memset(ram(RESP + 8), 0xee, 4);
    put_desc(0, REQ, 8, 1, 1);
    put_desc(1, RESP, HV_POWER_MSG_LEN, 2, 0);
    uint16_t *avail = (uint16_t *)ram(RING + 0x1000);
    avail[2 + avail[1] % QSIZE] = 0;
    avail[1]++;
    wr16(BAR + 0x3000, 0); /* notify queue 0 */
}

static uint16_t used_idx(void) { return ((uint16_t *)ram(RING + 0x2000))[1]; }
static uint32_t used_len(uint16_t i) { uint32_t v; memcpy(&v, ram(RING + 0x2000 + 8 + 8u * ((i - 1) % QSIZE)), 4); return v; }

/* handleQueueEvents(): the answer as the driver takes it. */
static void expect(const char *what, double percent, int external, int charging, int full) {
    char name[160];
    uint8_t msg[HV_POWER_MSG_LEN];
    hv_phys_read(&cur->m, RESP, msg, 8);
    hv_phys_read(&cur->m, RESP + 8, msg + 8, 4);
    double got;
    memcpy(&got, msg + 1, 8);
    snprintf(name, sizeof name, "%s: type", what), check(name, msg[0], HV_POWER_MSG_STATUS);
    snprintf(name, sizeof name, "%s: charge", what), check(name, got, percent);
    snprintf(name, sizeof name, "%s: ExternalConnected", what), check(name, msg[9], external);
    snprintf(name, sizeof name, "%s: IsCharging", what), check(name, msg[10], charging);
    snprintf(name, sizeof name, "%s: FullyCharged", what), check(name, msg[11], full);
    snprintf(name, sizeof name, "%s: length", what), check(name, used_len(used_idx()), HV_POWER_MSG_LEN);
}

static int snap_round_trip(Board *from, Board *to, const char *path) {
    HvSnapWriter *w = hv_snap_begin(path, HV_SNAP_NO_SYNC);
    if (!w || hv_vmapple_snap_save(&from->b, w) != 0 || hv_snap_put_ram(w, &from->m, 4) != 0 || hv_snap_commit(w) != 0) return -1;
    char why[160];
    HvSnapReader *r = hv_snap_open(path, why, sizeof why);
    if (!r) return -1;
    int rc = hv_snap_get_ram(r, &to->m, 4, HV_SNAP_DIRTY_RAM) != 0 || hv_vmapple_snap_load(&to->b, r) != 0 ? -1 : 0;
    hv_snap_close(r);
    return rc;
}

/* As hv_vm_snapshot_has_power (host/vm.c), which the test doesn't link. */
static int has_power(const char *path) {
    HvSnapReader *r = hv_snap_open(path, NULL, 0);
    if (!r) return -1;
    int has = hv_vmapple_snap_has_power(r);
    hv_snap_close(r);
    return has;
}

int main(void) {
    printf("== power source\n");
    if (build(&A, HV_POWER_BATTERY) != 0) return printf("cannot build the board\n"), 1;
    cur = &A;
    HvPowerSource *p = A.b.power;
    check("a second power source is refused", hv_vmapple_add_power_source(&A.b, HV_POWER_BATTERY), -1);
    check("an unknown kind is refused", hv_power_source_create(&A.m, A.b.pcie, 2) == NULL, 1);
    int slot = find_slot();
    check("0x106b:0x1a07 is on the bus", slot >= 0, 1);
    check("after the devices that were there before it", slot > A.b.tablet->vio.pci.slot, 1);
    if (slot < 0) return 1;
    check("config byte 0: a battery", driver_start(slot), HV_POWER_BATTERY);

    /* Before the host says anything: on the charger, full. The first buffer is answered at once. */
    post();
    check("first buffer answered at once", used_idx(), 1);
    expect("default", 100, 1, 0, 1);
    check("interrupt raised", rd8(BAR + 0x1000) & 1, 1);
    check("ISR clears on read", rd8(BAR + 0x1000), 0);

    /* Posted again: nothing new, so it waits. */
    post();
    hv_power_source_poll(p);
    check("no news: buffer kept", used_idx(), 1);

    HvPowerState s = {.percent = 42.5};
    hv_power_source_set(p, &s);
    check("set alone tells the guest nothing until the poll", used_idx(), 1);
    hv_power_source_poll(p);
    check("change answered at the poll", used_idx(), 2);
    expect("on battery", 42.5, 0, 0, 0);

    post();
    hv_power_source_set(p, &s); /* the same again */
    hv_power_source_poll(p);
    check("same state: not sent again", used_idx(), 2);

    /* A change while no buffer is posted waits for the next one. */
    s = (HvPowerState){.percent = 43, .external = 7, .charging = 1}; /* any nonzero flag is 1 */
    hv_power_source_set(p, &s);
    hv_power_source_poll(p);
    check("charging answered", used_idx(), 3);
    expect("charging", 43, 1, 1, 0);
    s.percent = 44;
    hv_power_source_set(p, &s);
    hv_power_source_poll(p);
    check("no buffer: nothing sent", used_idx(), 3);
    post();
    check("the new buffer gets it at its notify", used_idx(), 4);
    expect("charging, later", 44, 1, 1, 0);

    /* What the driver would refuse is never sent. */
    post();
    s = (HvPowerState){.percent = 150, .external = 1, .full = 1};
    hv_power_source_set(p, &s);
    hv_power_source_poll(p);
    expect("over 100 is 100", 100, 1, 0, 1);
    post();
    s = (HvPowerState){.percent = NAN};
    hv_power_source_set(p, &s);
    hv_power_source_poll(p);
    expect("not a number is 0", 0, 0, 0, 0);
    post();
    s = (HvPowerState){.percent = -3};
    hv_power_source_set(p, &s);
    hv_power_source_poll(p);
    check("below 0 is 0, as NaN was: no message", used_idx(), 6);

    /* The driver restarts (macOS rebooted): its first buffer hears the state at once. */
    s = (HvPowerState){.percent = 61, .charging = 1, .external = 1};
    hv_power_source_set(p, &s);
    hv_power_source_poll(p);
    check("before the reset", used_idx(), 7);
    driver_start(slot);
    post();
    check("restarted driver: answered at once", used_idx(), 1);
    expect("after a restart", 61, 1, 1, 0);
    post(); /* left waiting for the snapshot */
    hv_power_source_poll(p);
    check("and then it waits", used_idx(), 1);

    /* Suspend: the waiting buffer goes over with the RAM and the queue; the new board tells the
       guest its own host's battery at its first poll. */
    if (build(&B, HV_POWER_BATTERY) != 0) return printf("cannot build board B\n"), 1;
    char path[] = "/tmp/hengevm-power-XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) close(fd);
    check("saved and loaded", snap_round_trip(&A, &B, path), 0);
    cur = &B;
    hv_power_source_poll(B.b.power);
    check("loaded board: the guest is told again at once", used_idx(), 2);
    expect("after a load, before the host spoke", 100, 1, 0, 1);
    post();
    s = (HvPowerState){.percent = 12, .external = 0};
    hv_power_source_set(B.b.power, &s);
    hv_power_source_poll(B.b.power);
    check("loaded board carries on", used_idx(), 3);
    expect("after a load", 12, 0, 0, 0);

    /* Wall power: config byte 1. */
    if (build(&W, HV_POWER_WALL) != 0) return printf("cannot build board W\n"), 1;
    cur = &W;
    check("config byte 0: wall power", driver_start(find_slot()), HV_POWER_WALL);
    post();
    expect("wall", 100, 1, 0, 1);

    /* A battery Mac's snapshot does not go into a wall-power board. */
    check("battery snapshot refused by a wall-power board", snap_round_trip(&A, &W, path), -1);
    check("a battery Mac's snapshot says it has power", has_power(path), 1);

    /* A Mac saved before the battery (a board without one): the app sees that, builds the board without one
       too (HV_VM_POWER_NONE), and it resumes; a board with the battery would have a PCI slot it didn't. */
    if (build(&O, -1) != 0 || build(&O2, HV_POWER_BATTERY) != 0 || build(&O3, -1) != 0) return printf("cannot build boards O\n"), 1;
    char opath[] = "/tmp/hengevm-power-old-XXXXXX";
    fd = mkstemp(opath);
    if (fd >= 0) close(fd);
    HvSnapWriter *w = hv_snap_begin(opath, HV_SNAP_NO_SYNC);
    check("saved without a battery", !w || hv_vmapple_snap_save(&O.b, w) != 0 || hv_snap_put_ram(w, &O.m, 4) != 0 || hv_snap_commit(w) != 0, 0);
    check("a snapshot from before the battery says so", has_power(opath), 0);
    check("an unreadable snapshot: -1", has_power("/nonexistent/henge-snapshot"), -1);
    check("it doesn't go into a board with the battery", snap_round_trip(&O, &O2, opath), -1);
    check("it goes into a board built without one", snap_round_trip(&O, &O3, opath), 0);
    unlink(opath);
    unlink(path);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
