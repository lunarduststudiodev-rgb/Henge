/* The device's queues (pvg_lite.c) with guest memory and a stand-in for the Metal side: no CPU, GPU or timing.
   The root ring, the channels (work the guest adds with no doorbell, a command longer than what was written,
   no memory for a copy), and the mapper (bad indexes, the result each entry gets, an unmap completing only
   once the GPU is done). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Built against the baseline too (HV_BASELINE, with what it lacks stubbed), to show the checks fail there */
static int fail_malloc; /* the next mallocs from the device fail */
static void *test_malloc(size_t n) { return fail_malloc ? NULL : malloc(n); }
#define malloc(n) test_malloc(n)
#ifndef HV_PVG_MAPPER_BATCH
#define HV_PVG_MAPPER_BATCH 3 /* a long ring walked a few entries a poll (test_mapper_batches) */
#endif
#include "../devices/pvg_lite.c"
#undef malloc

#define BASE 0x40000000ull
#define PG(n) (BASE + 0x4000ull * (n))
static uint8_t *ram;
static const uint64_t ram_size = 64 * 0x4000;
static HvMachine *machine;
static unsigned gfx_irqs, iosfc_irqs;

void *hv_phys_ptr(HvMachine *m, uint64_t pa, uint64_t len) {
    (void)m;
    return pa >= BASE && pa - BASE <= ram_size && len <= ram_size - (pa - BASE) ? ram + (pa - BASE) : NULL;
}
/* For a pointer written through (machine.h): no save runs here, so the same as hv_phys_ptr */
void *hv_phys_ptr_w(HvMachine *m, uint64_t pa, uint64_t len) { return hv_phys_ptr(m, pa, len); }
const void *hv_phys_ptr_r(HvMachine *m, uint64_t pa, uint64_t len) { return hv_phys_ptr(m, pa, len); }
/* A device's write done (machine.h): no RAM file or code here, nothing to mark */
void hv_dma_written(HvMachine *m, uint64_t pa, uint64_t len) { (void)m, (void)pa, (void)len; }
int hv_machine_add_mmio(HvMachine *m, uint64_t pa, uint64_t size, void *opaque, uint64_t (*read)(void *, uint64_t, unsigned),
                        void (*write)(void *, uint64_t, uint64_t, unsigned), const char *name) {
    (void)m, (void)pa, (void)size, (void)opaque, (void)read, (void)write, (void)name;
    return 0;
}
void hv_gicv3_pend(HvGic *g, int intid) { (void)g; if (intid == 1) gfx_irqs++; else iosfc_irqs++; }
void hv_machine_wake(HvMachine *m) { (void)m; }
/* The device lock: held around a fence's completion, and the completed index as each unlock leaves it */
static int locked, lock_errors;
static uint64_t *watch, at_lock, at_unlock;
void hv_dev_lock(HvMachine *m) { (void)m; if (locked++) lock_errors++; at_lock = watch ? __atomic_load_n(watch, __ATOMIC_ACQUIRE) : 0; }
void hv_dev_unlock(HvMachine *m) { (void)m; if (--locked) lock_errors++; at_unlock = watch ? __atomic_load_n(watch, __ATOMIC_ACQUIRE) : 0; }

/* The Metal side: commands it was handed, unmaps and fences in the order asked */
#include "pvg_metal_standin.h"
#include "zalloc.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t *g32(uint64_t pa) { return hv_phys_ptr(NULL, pa, 4); }

static HvPvgLite *fresh(int with_metal) {
    memset(ram, 0, ram_size);
    metal_on = with_metal, nhanded = nunmapped = nfences = nforgot = 0, gfx_irqs = iosfc_irqs = 0, while_handed = NULL, fail_malloc = 0;
    return hv_pvg_lite_create(machine, NULL, 0, 0, 1, 2);
}

/* ---- Root ring: page 1 holds the stamp, the ring is its second half ---- */
static void root_setup(HvPvgLite *p) {
    gfx_write(p, 0x1030, PG(1) >> 14, 4), gfx_write(p, 0x1010, 0x2000, 4), gfx_write(p, 0x1004, 0x4000, 4);
}
static void root_put(uint32_t at, uint32_t op, uint32_t len, uint32_t stamp) {
    uint32_t c[3] = {op, len, stamp};
    memcpy(ram + (PG(1) - BASE) + 0x2000 + at, c, 12);
}

static void test_root(void) {
    printf("root ring\n");
    HvPvgLite *p = fresh(0);
    root_setup(p);
    root_put(0, 0x99, 16, 7), root_put(16, 0x99, 12, 8);
    gfx_write(p, 0x1008, 28, 4);
    CHECK(p->root_rp == 28 && *g32(PG(1)) == 8, "two commands taken: rp %x stamp %u", p->root_rp, *g32(PG(1)));
    /* One that says it runs past the write pointer: it would take the ring's other commands as its own, and
       the read pointer would chase the write pointer round 2^32 */
    root_put(28, 0x99, 16, 9);
    gfx_write(p, 0x1008, 28 + 12, 4);
    CHECK(p->root_rp == 40 && *g32(PG(1)) == 8, "longer than written: dropped, rp %x stamp %u", p->root_rp, *g32(PG(1)));
    root_put(40, 0x99, 0, 10);
    gfx_write(p, 0x1008, 40 + 0x100, 4);
    CHECK(p->root_rp == 40 + 0x100, "length 0: dropped, rp %x", p->root_rp);
    gfx_write(p, 0x1008, p->root_rp + 0x10000, 4); /* more than the ring holds */
    CHECK(p->root_rp == reg(p, 0x1008), "write pointer a ring ahead: dropped, rp %x", p->root_rp);
    gfx_write(p, 0x1004, 0x1000, 4), gfx_write(p, 0x1008, p->root_rp + 12, 4); /* the ring's start past its size */
    CHECK(p->root_rp != reg(p, 0x1008), "a ring that ends before it starts: nothing taken");
}

static void chan_setup(HvPvgLite *p, uint32_t ch, uint32_t ring_page);
static void chan_put(uint32_t ch, uint32_t op, uint32_t len, uint32_t stamp);

/* Apple's host takes a delete on the root ring as on a channel: here it goes to the Metal side, once there is one, and
   after the channel work its barriers name (that app's last work must not come after its task is forgotten) */
static void test_root_deletes(void) {
    printf("root ring deletes\n");
    HvPvgLite *p = fresh(1); /* a Metal side once a channel command comes (ensure_metal) */
    root_setup(p);
    uint32_t task = 6;
    root_put(0, 0x20, 16, 1), memcpy(ram + (PG(1) - BASE) + 0x2000 + 12, &task, 4);
    gfx_write(p, 0x1008, 16, 4);
    CHECK(nforgot == 0 && p->root_rp == 16 && *g32(PG(1)) == 1 && !p->metal, "no Metal side yet: nothing to forget, the command taken");
    chan_setup(p, 2, (uint32_t)(PG(12) >> 14));
    chan_put(2, 0x99, 12, 2), chan_put(2, 0x99, 12, 3); /* the app's last work: written, its doorbell not rung */
    /* a task's delete with one barrier {channel 2, stamp 3} before its task, then an object's {task, tag, size, ref} */
    uint32_t del[6] = {0x10020, 24, 2, 2, 3, task}, obj[7] = {0x28, 28, 3, task, 1005, 12, 9};
    memcpy(ram + (PG(1) - BASE) + 0x2000 + 16, del, 24), memcpy(ram + (PG(1) - BASE) + 0x2000 + 40, obj, 28);
    gfx_write(p, 0x1008, 16 + 24 + 28, 4);
    CHECK(nforgot == 2 && forgot[0][0] == 0x20 && forgot[0][1] == task && forgot[1][0] == 0x28 && forgot[1][1] == task,
          "both deletes handed over, past the barrier: %u, %x %u, %x %u", nforgot, forgot[0][0], forgot[0][1], forgot[1][0], forgot[1][1]);
    CHECK(nhanded == 2 && forgot_after[0] == 2 && p->metal, "channel 2's work up to the barrier handed over first (the drain making the "
          "Metal side): %u handed, the delete after %u", nhanded, forgot_after[0]);
    CHECK(p->root_rp == 68 && *g32(PG(1)) == 3, "and taken: rp %x stamp %u", p->root_rp, *g32(PG(1)));
    root_put(68, 0x99, 12, 4);
    gfx_write(p, 0x1008, 80, 4);
    CHECK(nforgot == 2 && *g32(PG(1)) == 4, "another op: not handed over");
    /* a delete whose barrier names work the guest has not written yet: it waits, and nothing after it goes first */
    uint32_t later[6] = {0x10020, 24, 5, 2, 5, task};
    memcpy(ram + (PG(1) - BASE) + 0x2000 + 80, later, 24), root_put(104, 0x99, 12, 6);
    gfx_write(p, 0x1008, 116, 4);
    for (int i = 0; i < 30000; i++) gfx_read(p, 0x100c, 4); /* the guest spinning on the root ring: no limit to the wait */
    CHECK(nforgot == 2 && p->root_rp == 80 && *g32(PG(1)) == 4 && p->root_waiting, "waits: rp %x stamp %u", p->root_rp, *g32(PG(1)));
    CHECK(p->wait_since[0] != 0, "the root ring's wait is a stage held up");
    chan_put(2, 0x99, 12, 4), chan_put(2, 0x99, 12, 5); /* written now, no doorbell */
    hv_pvg_lite_poll(p);
    CHECK(nforgot == 3 && forgot_after[2] == 4 && p->root_rp == 116 && *g32(PG(1)) == 6 && !p->root_waiting,
          "then the channel's work, the delete, and what came after: %u forgotten after %u, rp %x", nforgot, forgot_after[2], p->root_rp);
    CHECK(!p->wait_since[0], "and it ended");
    /* Waiting again, then the guest's write index thrown a ring ahead: what waited is skipped, and so is its wait */
    memcpy(ram + (PG(1) - BASE) + 0x2000 + 116, (uint32_t[6]){0x10020, 24, 7, 2, 9, task}, 24);
    gfx_write(p, 0x1008, 140, 4);
    CHECK(p->root_waiting && p->wait_since[0], "waiting for channel 2's stamp 9");
    gfx_write(p, 0x1008, 140 + 0x10000, 4);
    CHECK(!p->root_waiting && !p->wait_since[0], "skipped: not waiting");
    atomic_store_explicit(&p->metal, NULL, memory_order_release);
}

/* ---- A channel: its record on page 2 (root, + 0x400), its ring page 4 via the table on page 3, stamps page 1 ---- */
static uint32_t *chan_rec(uint32_t ch) { return g32(PG(2) + 0x400 + 0x14 * (ch - 1)); }
static void chan_setup(HvPvgLite *p, uint32_t ch, uint32_t ring_page) {
    gfx_write(p, 0x101c, PG(2) >> 14, 4), gfx_write(p, 0x1030, PG(1) >> 14, 4);
    uint32_t *rec = chan_rec(ch);
    rec[4] = (uint32_t)(PG(3 + ch * 2) >> 14);
    *g32(PG(3 + ch * 2)) = ring_page;
}
static void chan_put(uint32_t ch, uint32_t op, uint32_t len, uint32_t stamp) {
    uint32_t *rec = chan_rec(ch), c[3] = {op, len, stamp};
    uint64_t ring = (uint64_t)*g32(PG(3 + ch * 2)) << 14;
    memcpy(ram + (ring - BASE) + rec[0] % 0x4000, c, 12);
    rec[0] += len;
}
static uint32_t chan_stamp(uint32_t ch) { return *g32(PG(1) + 4 * ch); }

/* A define starts its task empty (Apple's host refuses a number in use): on the root ring or a channel, the task's
   object lists are forgotten here, and the Metal side is told on the root ring (a channel's define reaches it in order) */
static void chan_args(uint32_t ch, const uint32_t *a, uint32_t n) { /* a command's words after its header, then the header */
    uint32_t *rec = chan_rec(ch);
    uint64_t ring = (uint64_t)*g32(PG(3 + ch * 2)) << 14;
    memcpy(ram + (ring - BASE) + (rec[0] + 12) % 0x4000, a, n);
}
static void test_defines(void) {
    printf("a define starts its task empty\n");
    HvPvgLite *p = fresh(1);
    root_setup(p);
    atomic_store_explicit(&p->metal, (HvPvgMetal *)&metal_on, memory_order_release);
    p->tasks.objects[6] = 0x1000, p->tasks.placement[6] = 0x2000, p->tasks.objects[3] = 0x3000;
    /* {task << 1, size, size high, root page}, after one barrier on work already done */
    uint32_t def[9] = {0x10038, 36, 1, 1, 0, 6 << 1, 0x4000, 0, 0x77};
    memcpy(ram + (PG(1) - BASE) + 0x2000, def, 36);
    gfx_write(p, 0x1008, 36, 4);
    CHECK(p->root_rp == 36 && p->tasks.root[6] == 0x77, "the root page read after the barrier: %x", p->tasks.root[6]);
    CHECK(!p->tasks.objects[6] && !p->tasks.placement[6] && p->tasks.objects[3] == 0x3000, "task 6's object lists forgotten, task 3's kept");
    CHECK(nforgot == 1 && forgot[0][0] == 0x38 && forgot[0][1] == 6 << 1, "the Metal side told: %u, %x %u", nforgot, forgot[0][0], forgot[0][1]);
    chan_setup(p, 1, (uint32_t)(PG(10) >> 14));
    p->tasks.objects[3] = 0x3000;
    uint32_t again[4] = {3 << 1, 0x4000, 0, 0x88};
    chan_args(1, again, 16), chan_put(1, 0x38, 28, 1);
    gfx_write(p, 0x1020, 1, 4);
    CHECK(p->tasks.root[3] == 0x88 && !p->tasks.objects[3] && nhanded == 1 && nforgot == 1,
          "on a channel: task 3's root %x, lists forgotten, handed over in order (%u)", p->tasks.root[3], nhanded);
    /* On the root ring with no memory for the worker's copy: left on the ring, the device's tables untouched too (the
       worker would go on reading the number's old task), and both take it at the next poll */
    uint32_t def5[9] = {0x10038, 36, 2, 1, 0, 5 << 1, 0x4000, 0, 0x99};
    memcpy(ram + (PG(1) - BASE) + 0x2000 + 36, def5, 36);
    fail_malloc = 1;
    gfx_write(p, 0x1008, 72, 4);
    CHECK(p->root_rp == 36 && p->root_waiting && !p->tasks.root[5] && nforgot == 1 && *g32(PG(1)) == 1,
          "no memory: nothing of it taken (rp %x, root %x, %u told)", p->root_rp, p->tasks.root[5], nforgot);
    fail_malloc = 0;
    hv_pvg_lite_poll(p);
    CHECK(p->root_rp == 72 && !p->root_waiting && p->tasks.root[5] == 0x99 && nforgot == 2 && forgot[1][1] == 5 << 1 && *g32(PG(1)) == 2,
          "the next poll: both copies take it (rp %x, root %x, %u told)", p->root_rp, p->tasks.root[5], nforgot);
    atomic_store_explicit(&p->metal, NULL, memory_order_release);
}

/* A root command too long to hand over is logged once an op, not once a command: a guest could send it on every one */
static void test_long_root_ops(void) {
    printf("root ring commands too long to take\n");
    HvPvgLite *p = fresh(1);
    root_setup(p);
    atomic_store_explicit(&p->metal, (HvPvgMetal *)&metal_on, memory_order_release);
    fflush(stderr);
    int saved = dup(2);
    FILE *log = tmpfile();
    dup2(fileno(log), 2);
    for (uint32_t i = 0; i < 5; i++) { /* one at a time: the ring holds 0x2000 bytes */
        root_put(0x1004 * i % 0x2000, i < 3 ? 0x20 : 0x38, 0x1004, 1 + i);
        uint32_t args[4] = {5 << 1, 0x4000, 0, 0x99}; /* a define's {task << 1, size, size high, root page} */
        memcpy(ram + (PG(1) - BASE) + 0x2000 + (0x1004 * i + 12) % 0x2000, args, 16);
        gfx_write(p, 0x1008, 0x1004 * (i + 1), 4);
    }
    fflush(stderr), dup2(saved, 2), close(saved);
    rewind(log);
    char line[256];
    int lines = 0;
    while (fgets(line, sizeof line, log)) lines += strstr(line, "pvg-lite: root ring op") != NULL;
    fclose(log);
    CHECK(p->root_rp == 0x1004 * 5 && *g32(PG(1)) == 5 && nforgot == 0, "all five gone past, none handed over: rp %x, %u told", p->root_rp, nforgot);
    CHECK(lines == 2, "one line for the deletes and one for the defines: %d", lines);
    CHECK(!p->tasks.root[5], "the device's tables didn't take the define either: root %x", p->tasks.root[5]);
    atomic_store_explicit(&p->metal, NULL, memory_order_release);
}

/* The worker's task reads go by what it has reached: a define read by the device while the number's old task still has
   work queued leaves that work reading the old task's memory, and the worker's own reads change at the define */
static void test_define_order(void) {
    printf("a define is the worker's when it reaches it\n");
    HvPvgLite *p = fresh(1);
    /* Task 4's memory, one level: root page 30 {table page 31, depth 1}, its page 0 at page 32 (old) or 34 (new) */
    *g32(PG(30)) = (uint32_t)(PG(31) >> 14), *(g32(PG(30)) + 1) = 1, *g32(PG(31)) = (uint32_t)(PG(32) >> 14), *g32(PG(32) + 8) = 0x01d;
    *g32(PG(33)) = (uint32_t)(PG(35) >> 14), *(g32(PG(33)) + 1) = 1, *g32(PG(35)) = (uint32_t)(PG(34) >> 14), *g32(PG(34) + 8) = 0x2e1;
    uint32_t old[4] = {4 << 1, 0x4000, 0, (uint32_t)(PG(30) >> 14)}, now[4] = {4 << 1, 0x4000, 0, (uint32_t)(PG(33) >> 14)};
    uint32_t list[7] = {4, 0x100, 0, 0x200, 0, 12, 24};
    chan_setup(p, 1, (uint32_t)(PG(10) >> 14));
    chan_args(1, old, 16), chan_put(1, 0x38, 28, 1);
    gfx_write(p, 0x1020, 1, 4); /* the Metal side made here, before the define: it is the worker's to reach */
    uint32_t w = 0;
    CHECK(hv_pvg_lite_task_read(p, 4, 8, &w, 4) == -1, "not reached: task 4 has no memory for the worker yet");
    hv_pvg_lite_task_command(p, 0x38, old, 16), hv_pvg_lite_task_command(p, 0x44, list, 28);
    CHECK(!hv_pvg_lite_task_read(p, 4, 8, &w, 4) && w == 0x01d && hv_pvg_lite_task_objects(p, 4) == 0x100, "reached: the old task's (%x)", w);
    /* the old app's last work queued, its delete, and a new app given the number: the device reads the define first */
    chan_args(1, now, 16), chan_put(1, 0x38, 28, 2);
    gfx_write(p, 0x1020, 1, 4);
    CHECK(p->tasks.root[4] == (uint32_t)(PG(33) >> 14), "the device has read the new define");
    CHECK(!hv_pvg_lite_task_read(p, 4, 8, &w, 4) && w == 0x01d && hv_pvg_lite_task_objects(p, 4) == 0x100,
          "work queued ahead of it still reads the old task: %x, objects %llx", w, (unsigned long long)hv_pvg_lite_task_objects(p, 4));
    hv_pvg_lite_task_command(p, 0x38, now, 16);
    CHECK(!hv_pvg_lite_task_read(p, 4, 8, &w, 4) && w == 0x2e1 && !hv_pvg_lite_task_objects(p, 4), "the worker at the define: the new task's (%x)", w);
    uint32_t page = 0;
    hv_pvg_lite_task_pages(p, 4, 0, 1, &page);
    CHECK(page == (uint32_t)(PG(34) >> 14) && hv_pvg_lite_task_page(p, 4, 0) == page, "and its pages: %x", page);
}

static void test_channel(void) {
    printf("channel commands\n");
    HvPvgLite *p = fresh(0);
    chan_setup(p, 1, (uint32_t)(PG(10) >> 14));
    chan_put(1, 0x99, 16, 5), chan_put(1, 0x99, 12, 6);
    gfx_write(p, 0x1020, 1, 4);
    CHECK(chan_rec(1)[1] == 28 && chan_stamp(1) == 6 && gfx_irqs == 1, "taken, stamped, told: rd %x stamp %u irqs %u", chan_rec(1)[1], chan_stamp(1), gfx_irqs);
    CHECK(gfx_read(p, 0x1018, 4) == 2 && gfx_read(p, 0x1018, 4) == 0, "status bit for channel 1, cleared once read");
    chan_put(1, 0x99, 0x40, 7);
    chan_rec(1)[0] -= 0x30; /* says 0x40, 0x10 written */
    gfx_write(p, 0x1020, 1, 4);
    CHECK(chan_rec(1)[1] == chan_rec(1)[0] && chan_stamp(1) == 6, "longer than written: dropped, rd %x wr %x", chan_rec(1)[1], chan_rec(1)[0]);
    chan_put(1, 0x99, 8, 8);
    gfx_write(p, 0x1020, 1, 4);
    CHECK(chan_rec(1)[1] == chan_rec(1)[0] && chan_stamp(1) == 6, "shorter than its header: dropped");
    gfx_write(p, 0x1020, 99, 4); /* no such channel */
    gfx_write(p, 0x1020, 0, 4);
    CHECK(1, "channels 0 and 99 rung: nothing to do");
}

/* The guest adds work while its earlier doorbell is still being answered, and doesn't ring again (its
   driver rings only for a ring it found empty): the device's own poll takes it, in order */
static HvPvgLite *busy;
static void add_while_handed(void) { chan_put(1, 0x99, 12, 21); }
static void test_lost_doorbell(void) {
    printf("work added with no doorbell\n");
    HvPvgLite *p = busy = fresh(1);
    chan_setup(p, 1, (uint32_t)(PG(10) >> 14));
    chan_put(1, 0x99, 12, 20);
    while_handed = add_while_handed;
    gfx_write(p, 0x1020, 1, 4);
    CHECK(nhanded == 1 && handed[0][1] == 20, "the rung command handed over (%u)", nhanded);
    CHECK(chan_rec(1)[0] != chan_rec(1)[1], "the one added meanwhile still queued");
    hv_pvg_lite_poll(p);
    CHECK(nhanded == 2 && handed[1][0] == 1 && handed[1][1] == 21, "the poll hands it over: %u handed, stamp %u", nhanded, handed[1][1]);
    CHECK(chan_rec(1)[0] == chan_rec(1)[1], "nothing left queued");
    chan_put(1, 0x99, 12, 22); /* and with no doorbell at all */
    hv_pvg_lite_poll(p);
    CHECK(nhanded == 3 && handed[2][1] == 22, "never rung: handed over at the next poll");
    hv_pvg_lite_poll(p);
    CHECK(nhanded == 3, "an empty ring: nothing handed twice");
}

/* The copy for the Metal worker: the ring is the guest's size, so malloc can fail. The command stays in the
   ring for the next poll, and nothing after it goes first */
static void test_no_memory(void) {
    printf("no memory for a command's copy\n");
    HvPvgLite *p = fresh(1);
    chan_setup(p, 1, (uint32_t)(PG(10) >> 14));
    chan_put(1, 0x99, 12, 30), chan_put(1, 0x99, 12, 31);
    fail_malloc = 1;
    gfx_write(p, 0x1020, 1, 4);
    CHECK(nhanded == 0 && chan_rec(1)[1] == 0 && p->submitted[1] == 0, "nothing taken: rd %x", chan_rec(1)[1]);
    CHECK(p->chan_pending & 2, "the channel waits for the next poll");
    fail_malloc = 0;
    hv_pvg_lite_poll(p);
    CHECK(nhanded == 2 && handed[0][1] == 30 && handed[1][1] == 31, "then both, in order: %u", nhanded);
}

/* A command after a barrier {bch, bstamp} */
static void chan_put_after(uint32_t ch, uint32_t stamp, uint32_t bch, uint32_t bstamp) {
    uint32_t *rec = chan_rec(ch), c[5] = {0x10099, 20, stamp, bch, bstamp};
    uint64_t ring = (uint64_t)*g32(PG(3 + ch * 2)) << 14;
    memcpy(ram + (ring - BASE) + rec[0] % 0x4000, c, 20);
    rec[0] += 20;
}

/* A barrier on work the guest has not written: the channel waits as long as it takes (it gave up after 20000 polls
   and ran out of order), and two channels waiting on each other are said to */
static void test_barriers(void) {
    printf("barriers between channels\n");
    HvPvgLite *p = fresh(1);
    chan_setup(p, 1, (uint32_t)(PG(10) >> 14)), chan_setup(p, 2, (uint32_t)(PG(12) >> 14));
    chan_put_after(1, 40, 2, 9);
    gfx_write(p, 0x1020, 1, 4);
    for (int i = 0; i < 25000; i++) hv_pvg_lite_poll(p);
    CHECK(nhanded == 0 && chan_rec(1)[1] == 0, "25000 polls: still waiting for channel 2 (%u handed)", nhanded);
    uint64_t since = p->wait_since[1];
    CHECK(since && !p->wait_since[2] && !p->wait_since[0], "the freeze recorder's barrier stage: channel 1 waits, since its first try");
    hv_pvg_lite_poll(p);
    CHECK(p->wait_since[1] == since, "a later try keeps when it began");
    chan_put(2, 0x99, 12, 9);
    hv_pvg_lite_poll(p);
    CHECK(nhanded == 2 && handed[0][0] == 2 && handed[1][0] == 1 && handed[1][1] == 40, "written: channel 2's, then channel 1's (%u)", nhanded);
    CHECK(p->barrier_cycles == 0 && !p->waits_on[1] && !p->wait_since[1], "no loop; channel 1 waits for nothing now");
    /* Waiting again, then its ring taken down (its table emptied): the poll that finds no ring ends the wait */
    chan_put_after(1, 41, 2, 10);
    hv_pvg_lite_poll(p);
    CHECK(p->wait_since[1] && p->waits_on[1] == 2, "waiting for channel 2's stamp 10");
    *g32(PG(3 + 1 * 2)) = 0;
    hv_pvg_lite_poll(p);
    CHECK(!p->wait_since[1] && !p->waits_on[1], "no ring: not waiting (%llu)", (unsigned long long)p->wait_since[1]);
    p = fresh(1);
    chan_setup(p, 1, (uint32_t)(PG(10) >> 14)), chan_setup(p, 2, (uint32_t)(PG(12) >> 14));
    chan_put_after(1, 10, 2, 20), chan_put_after(2, 20, 1, 10);
    gfx_write(p, 0x1020, 1, 4);
    CHECK(nhanded == 0 && p->barrier_cycles == 2 && p->cycles_told == 6, "each waiting on the other: said for both at the first doorbell (%llu, %x), "
          "nothing run out of order", (unsigned long long)p->barrier_cycles, p->cycles_told);
    CHECK(!p->wait_since[1] && !p->wait_since[2], "nor a stage held up from the first doorbell: %llu %llu",
          (unsigned long long)p->wait_since[1], (unsigned long long)p->wait_since[2]);
    hv_pvg_lite_poll(p);
    CHECK(nhanded == 0, "and still waiting after a poll");
    CHECK(!p->wait_since[1] && !p->wait_since[2], "a wait that can't end is not a stage held up: %llu %llu",
          (unsigned long long)p->wait_since[1], (unsigned long long)p->wait_since[2]);
    /* ... and the root ring behind them: a delete whose barrier names channel 1 can't go on either */
    root_setup(p);
    memcpy(ram + (PG(1) - BASE) + 0x2000, (uint32_t[6]){0x10020, 24, 1, 1, 10, 6}, 24);
    gfx_write(p, 0x1008, 24, 4);
    CHECK(p->root_waiting && !p->wait_since[0], "the root ring waits behind the loop, not a stage held up (%llu)",
          (unsigned long long)p->wait_since[0]);
    /* Not a loop: channel 1 waits for channel 2's stamp 20, rung before channel 2 wrote anything; then channel 2 writes
       20, and 21 after a barrier on channel 1's 11. The walk of channel 1 drains channel 2, whose 20 meets channel 1's
       wait, so channel 2's own wait on channel 1 (still in its walk, above) is no loop, and all three go on. */
    p = fresh(1);
    chan_setup(p, 1, (uint32_t)(PG(10) >> 14)), chan_setup(p, 2, (uint32_t)(PG(12) >> 14));
    chan_put_after(1, 11, 2, 20);
    gfx_write(p, 0x1020, 1, 4);
    CHECK(nhanded == 0 && p->waits_on[1] == 2 && p->wait_since[1], "channel 1 waits for channel 2's 20");
    chan_put(2, 0x99, 12, 20), chan_put_after(2, 21, 1, 11);
    hv_pvg_lite_poll(p);
    CHECK(nhanded == 3 && handed[0][1] == 20 && handed[1][1] == 11 && handed[2][1] == 21, "all three, in order (%u)", nhanded);
    CHECK(p->barrier_cycles == 0 && !p->cycles_told, "no loop said: %llu %x", (unsigned long long)p->barrier_cycles, p->cycles_told);
    CHECK(!p->wait_since[1] && !p->wait_since[2] && !p->waits_on[1] && !p->waits_on[2], "nothing waits");
    /* A command that says it has more barriers than its length holds is refused, as Apple's host refuses it: its
       "barriers" past its end would be the next command's header, here {channel 2, stamp 0x40}, a stamp channel 2
       never reaches. Its length is good, so it is stepped over and its stamp reached (pushed and faulted there), in
       order on the worker; the command after it is the guest's next and is taken. */
    p = fresh(1);
    chan_setup(p, 1, (uint32_t)(PG(10) >> 14)), chan_setup(p, 2, (uint32_t)(PG(12) >> 14));
    chan_put(1, 0x30099, 12, 5), chan_put(1, 2, 0x40, 6);
    gfx_write(p, 0x1020, 1, 4);
    CHECK(nhanded == 2 && handed[0][0] == 1 && handed[0][1] == 5 && handed[1][1] == 6 && chan_rec(1)[1] == chan_rec(1)[0],
          "channel: refused, its stamp handed over in order, the next taken (%u handed: %u %u; read %x of %x)", nhanded,
          handed[0][1], handed[1][1], chan_rec(1)[1], chan_rec(1)[0]);
    CHECK(handed_head[0][0] == 0 && handed_head[0][1] == 12 && handed_head[1][0] == 2 && handed_head[1][1] == 0x40,
          "channel: the refused one handed as its stamp alone, the next as written (%x/%u, %x/%u)", handed_head[0][0],
          handed_head[0][1], handed_head[1][0], handed_head[1][1]);
    CHECK(p->submitted[1] == 6 && !p->waits_on[1] && !p->wait_since[1] && !p->waits_on[2], "channel: waits for nothing (on %u)", p->waits_on[1]);
    /* What waits on a refused command's stamp goes on: channel 2 waits for channel 1's 7, a refused command's, and
       channel 1 gets no work after it (before, nothing passed 7 and channel 2 waited for good) */
    chan_put_after(2, 30, 1, 7), chan_put(1, 0x20099, 20, 7);
    gfx_write(p, 0x1020, 2, 4);
    CHECK(nhanded == 4 && handed[2][0] == 1 && handed[2][1] == 7 && handed[3][0] == 2 && handed[3][1] == 30,
          "a wait on a refused stamp: met, in order (%u handed)", nhanded);
    CHECK(!p->waits_on[2] && !p->wait_since[2] && p->barrier_cycles == 0, "nothing waits (channel 2 on %u)", p->waits_on[2]);
    /* More than 64 barriers is refused the same way, however long the command (Apple's host: count 0x41 and up) */
    nhanded = 0;
    chan_put(1, 0x410099, 12 + 65 * 8, 8);
    gfx_write(p, 0x1020, 1, 4);
    CHECK(nhanded == 1 && handed[0][1] == 8 && p->submitted[1] == 8 && chan_rec(1)[1] == chan_rec(1)[0] && !p->waits_on[1],
          "65 barriers: refused, its stamp reached (%u handed)", nhanded);
    CHECK(handed_head[0][0] == 0 && handed_head[0][1] == 12, "65 barriers: not run, only its stamp handed over (op %x length %u)",
          handed_head[0][0], handed_head[0][1]);
    /* No Metal side: the stamp is written by the device, as for every command it takes */
    p = fresh(0);
    chan_setup(p, 1, (uint32_t)(PG(10) >> 14));
    chan_put(1, 0x30099, 12, 5);
    gfx_write(p, 0x1020, 1, 4);
    CHECK(chan_stamp(1) == 5 && p->submitted[1] == 5 && chan_rec(1)[1] == chan_rec(1)[0], "no Metal: the refused stamp written (%u)", chan_stamp(1));
    p = fresh(0);
    root_setup(p);
    root_put(0, 0x10099, 12, 7), root_put(12, 0x99, 12, 8);
    gfx_write(p, 0x1008, 12, 4);
    CHECK(p->root_rp == 12 && *g32(PG(1)) == 7 && !p->root_waiting, "root ring: refused, its stamp reached (%u)", *g32(PG(1)));
    gfx_write(p, 0x1008, 24, 4);
    CHECK(p->root_rp == 24 && *g32(PG(1)) == 8, "root ring: the next command taken (stamp %u)", *g32(PG(1)));
    /* A bad length is not refused this way: Apple's host pushes no stamp for it, and where the next command starts
       isn't known. The rest is skipped, and its stamp is not reached. */
    p = fresh(1);
    chan_setup(p, 1, (uint32_t)(PG(10) >> 14));
    chan_put(1, 0x99, 8, 9), chan_put(1, 0x99, 12, 10);
    gfx_write(p, 0x1020, 1, 4);
    CHECK(nhanded == 0 && p->submitted[1] == 0 && chan_rec(1)[1] == chan_rec(1)[0], "a bad length: skipped, no stamp (%u handed)", nhanded);
}

/* ---- Mapper: ring on page 20, 4 entries of 16 bytes {op, id, result, 0} ---- */
static uint32_t *map_entry(uint64_t i) { return g32(PG(20) + (i % 4) * 16); }
static void map_put(HvPvgLite *p, uint64_t i, uint32_t op, uint32_t id) {
    (void)p;
    uint32_t *e = map_entry(i);
    e[0] = op, e[1] = id, e[2] = 0xdeadbeef, e[3] = 0;
}
static uint64_t completed(HvPvgLite *p) { return iosfc_read(p, 0x1020, 8); }

static void test_mapper(void) {
    printf("mapper\n");
    HvPvgLite *p = fresh(0);
    iosfc_write(p, 0x1000, PG(20), 8), iosfc_write(p, 0x1008, 4, 8);
    map_put(p, 0, 1, 5), map_put(p, 1, 3, 5), map_put(p, 2, 0, 5), map_put(p, 3, 9, 5);
    iosfc_write(p, 0x1018, 4, 8), hv_pvg_lite_poll(p); /* three a call here (HV_PVG_MAPPER_BATCH) */
    CHECK(completed(p) == 4 && iosfc_irqs == 1, "no GPU: done at once, completed %llu", (unsigned long long)completed(p));
    /* as Apple's wakeDevice: a map done, a test (testSurface:) and any other op kIOReturnUnsupported */
    CHECK(map_entry(0)[2] == 0 && map_entry(1)[2] == 0xe00002c7 && map_entry(2)[2] == 0xe00002c7 && map_entry(3)[2] == 0xe00002c7,
          "each entry's result written: %x %x %x %x", map_entry(0)[2], map_entry(1)[2], map_entry(2)[2], map_entry(3)[2]);
    iosfc_write(p, 0x1018, 4 + (1ull << 40), 8); /* far ahead of what the ring holds */
    CHECK(completed(p) == 4 + (1ull << 40) && p->mapper_rp == completed(p), "a submitted index 2^40 ahead: skipped, not walked");
    iosfc_write(p, 0x1020, 0, 8), iosfc_write(p, 0x1000, BASE + ram_size - 16, 8), iosfc_write(p, 0x1008, 4, 8);
    iosfc_write(p, 0x1018, 2, 8); /* a ring that runs off guest memory */
    CHECK(completed(p) == 2, "a ring outside guest memory: nothing read, completed %llu", (unsigned long long)completed(p));
    iosfc_write(p, 0x1000, PG(20), 8), iosfc_write(p, 0x1008, UINT64_MAX / 8, 8);
    iosfc_write(p, 0x1018, 3, 8);
    CHECK(completed(p) == 3, "a size whose bytes overflow: refused");
}

/* A ring with more submitted than one doorbell walks (HV_PVG_MAPPER_BATCH, here 3): the rest walked by the polls, a
   batch each, the completed index moved only once all of it is, and an unmap in an earlier batch still fenced */
static void test_mapper_batches(void) {
    printf("a long mapper ring, a batch at a time\n");
    HvPvgLite *p = fresh(1);
    atomic_store(&p->metal, (HvPvgMetal *)&metal_on);
    iosfc_write(p, 0x1000, PG(20), 8), iosfc_write(p, 0x1008, 4, 8);
    map_put(p, 0, 2, 4), map_put(p, 1, 1, 11), map_put(p, 2, 1, 12), map_put(p, 3, 1, 13);
    nmapped = 0;
    unsigned irqs = iosfc_irqs;
    iosfc_write(p, 0x1018, 4, 8);
    CHECK(nunmapped == 1 && nmapped == 2 && p->mapper_rp == 3 && map_entry(3)[2] == 0xdeadbeef && p->mapper_more,
          "the doorbell walks one batch (%u maps, at %llu)", nmapped, (unsigned long long)p->mapper_rp);
    CHECK(completed(p) == 0 && nfences == 0 && iosfc_irqs == irqs, "not done before all of it is walked");
    hv_pvg_lite_poll(p);
    CHECK(nmapped == 3 && p->mapper_rp == 4 && map_entry(3)[2] == 0 && !p->mapper_more, "the next poll walks the rest (%u)", nmapped);
    CHECK(nfences == 1 && fences[0] == 4 && completed(p) == 0, "the unmap from the first batch: fenced at the end");
    hv_pvg_lite_mapper_done(p, 4, fence_gens[0]);
    CHECK(completed(p) == 4, "and done once the GPU is");
    hv_pvg_lite_poll(p);
    CHECK(nmapped == 3 && completed(p) == 4, "nothing more to walk");
}

/* Mapping ids are 1 to 0x1fff, as Apple's host takes them: a map or unmap of any other is refused (0xe00002c2) and
   never reaches the Metal side, which keeps what it has by id; nor does such an id have a descriptor */
static void test_mapper_ids(void) {
    printf("mapper ids outside what Apple's host takes\n");
    HvPvgLite *p = fresh(1);
    atomic_store(&p->metal, (HvPvgMetal *)&metal_on);
    iosfc_write(p, 0x1000, PG(20), 8), iosfc_write(p, 0x1008, 4, 8);
    fflush(stderr);
    int saved = dup(2);
    FILE *log = tmpfile();
    dup2(fileno(log), 2);
    map_put(p, 0, 2, 0), map_put(p, 1, 2, 0x2000), map_put(p, 2, 1, 0xffffffff), map_put(p, 3, 2, 0x1fff);
    nmapped = 0;
    iosfc_write(p, 0x1018, 4, 8), hv_pvg_lite_poll(p); /* three a call here (HV_PVG_MAPPER_BATCH) */
    CHECK(nunmapped == 1 && unmapped[0] == 0x1fff && nmapped == 0, "only id 0x1fff reaches the Metal side (%u unmaps, %u maps)",
          nunmapped, nmapped);
    CHECK(map_entry(0)[2] == 0xe00002c2 && map_entry(1)[2] == 0xe00002c2 && map_entry(2)[2] == 0xe00002c2 && map_entry(3)[2] == 0,
          "refused ones answered 0xe00002c2: %x %x %x %x", map_entry(0)[2], map_entry(1)[2], map_entry(2)[2], map_entry(3)[2]);
    CHECK(nfences == 1 && fences[0] == 4, "fenced for the one unmap taken");
    hv_pvg_lite_mapper_done(p, 4, fence_gens[0]);
    for (uint32_t i = 4; i < 4 + 1000; i++) map_put(p, i, 2, 0x2000 + i), iosfc_write(p, 0x1018, i + 1, 8); /* made-up ids */
    CHECK(nunmapped == 1 && completed(p) == 4 + 1000, "a thousand more: none reach it, all completed (%llu)", (unsigned long long)completed(p));
    map_put(p, 1004, 1, 1);
    iosfc_write(p, 0x1018, 1005, 8);
    CHECK(nmapped == 1 && map_entry(1004)[2] == 0, "id 1 taken");
    fflush(stderr), dup2(saved, 2), close(saved);
    rewind(log);
    char line[256];
    int lines = 0;
    while (fgets(line, sizeof line, log)) lines += strstr(line, "outside 1-8191: refused") != NULL;
    fclose(log);
    CHECK(lines == 1, "told once: %d", lines);
    /* the descriptor table on page 21, its first u32 (ids 0-15) present at page 22; the u32 for 0x2000-0x200f too */
    iosfc_write(p, 0x1010, PG(21), 8);
    *g32(PG(21)) = (uint32_t)(PG(22) >> 12) | 1, *g32(PG(21) + 0x2000 / 16 * 4) = (uint32_t)(PG(22) >> 12) | 1;
    CHECK(hv_pvg_lite_surface(p, 1) == ram + (PG(22) - BASE) + 0x400, "id 1: its descriptor");
    CHECK(!hv_pvg_lite_surface(p, 0) && !hv_pvg_lite_surface(p, 0x2000) && !hv_pvg_lite_surface(p, 0x2001),
          "ids 0, 0x2000, 0x2001: none, though the table has entries there");
    CHECK(!hv_pvg_lite_surface(p, (1ull << 32) + 1), "id 2^32 + 1: none, not id 1's descriptor");
}

/* An unmap completes after the GPU work handed over before it (which may still use the pages), and the
   vCPU doesn't wait for it: the completed index moves when the Metal side says the GPU is done */
static void test_unmap_after_gpu(void) {
    printf("unmap waits for the GPU, not the vCPU\n");
    HvPvgLite *p = fresh(1);
    atomic_store(&p->metal, (HvPvgMetal *)&metal_on);
    iosfc_write(p, 0x1000, PG(20), 8), iosfc_write(p, 0x1008, 4, 8);
    map_put(p, 0, 1, 7);
    iosfc_write(p, 0x1018, 1, 8);
    CHECK(completed(p) == 1 && nfences == 0, "a map with nothing pending: done at once");
    map_put(p, 1, 2, 7);
    iosfc_write(p, 0x1018, 2, 8);
    CHECK(nunmapped == 1 && unmapped[0] == 7, "the unmap handed to the Metal side");
    CHECK(completed(p) == 1 && nfences == 1 && fences[0] == 2, "not complete yet: fenced at 2, completed %llu", (unsigned long long)completed(p));
    CHECK(map_entry(1)[2] == 0, "its result written");
    map_put(p, 2, 1, 8);
    iosfc_write(p, 0x1018, 3, 8);
    CHECK(completed(p) == 1 && nfences == 2 && fences[1] == 3, "a map behind it waits its turn (the index is one count)");
    unsigned before = iosfc_irqs;
    watch = &p->io_regs[0x20 / 8];
    hv_pvg_lite_mapper_done(p, 2, fence_gens[0]);
    CHECK(completed(p) == 2 && iosfc_irqs == before + 1, "the GPU done: completed 2, interrupt");
    CHECK(!lock_errors && !locked && at_lock == 1 && at_unlock == 2, "published under the device lock (%llu then %llu)",
          (unsigned long long)at_lock, (unsigned long long)at_unlock);
    watch = NULL;
    hv_pvg_lite_mapper_done(p, 3, fence_gens[1]);
    CHECK(completed(p) == 3, "then 3");
    map_put(p, 3, 1, 9);
    iosfc_write(p, 0x1018, 4, 8);
    CHECK(completed(p) == 4 && nfences == 2, "nothing pending again: done at once");
    iosfc_write(p, 0x1020, 0, 8);
    CHECK(p->mapper_rp == 0 && p->mapper_fenced == 0, "the guest starting its ring again");
}

/* A fence asked for before the guest starts its ring again finishes after it: it must not move the new ring's
   completed index (to an index of the old ring's) */
static void test_stale_fence(void) {
    printf("a fence from before a ring restart\n");
    HvPvgLite *p = fresh(1);
    atomic_store(&p->metal, (HvPvgMetal *)&metal_on);
    iosfc_write(p, 0x1000, PG(20), 8), iosfc_write(p, 0x1008, 4, 8);
    map_put(p, 0, 2, 7);
    iosfc_write(p, 0x1018, 1, 8);
    CHECK(nfences == 1 && completed(p) == 0, "an unmap fenced at 1");
    iosfc_write(p, 0x1020, 0, 8), iosfc_write(p, 0x1018, 0, 8); /* the guest starts again from 0 */
    map_put(p, 0, 2, 8), map_put(p, 1, 2, 9), map_put(p, 2, 2, 10);
    iosfc_write(p, 0x1018, 3, 8);
    CHECK(nfences == 2 && fences[1] == 3, "the new ring's unmaps fenced at 3");
    hv_pvg_lite_mapper_done(p, 1, fence_gens[0]); /* the old fence, late */
    CHECK(completed(p) == 0, "the old fence completes nothing in the new ring (completed %llu)", (unsigned long long)completed(p));
    hv_pvg_lite_mapper_done(p, 3, fence_gens[1]);
    CHECK(completed(p) == 3, "the new one does");
    hv_pvg_lite_mapper_done(p, 2, fence_gens[1]);
    CHECK(completed(p) == 3, "a completion behind it doesn't move it back");
}

/* The same when the guest only moves its submitted index back (0x1018 lowered, no restart): the old fence completes
   nothing of the ring from there */
static void test_lowered_fence(void) {
    printf("a fence from before the submitted index went back\n");
    HvPvgLite *p = fresh(1);
    atomic_store(&p->metal, (HvPvgMetal *)&metal_on);
    iosfc_write(p, 0x1000, PG(20), 8), iosfc_write(p, 0x1008, 4, 8);
    map_put(p, 0, 2, 7);
    iosfc_write(p, 0x1018, 1, 8);
    CHECK(nfences == 1 && completed(p) == 0, "an unmap fenced at 1");
    iosfc_write(p, 0x1018, 0, 8); /* back to 0, no restart */
    CHECK(p->mapper_rp == 0 && p->mapper_fenced == 0, "the walk at 0 again");
    map_put(p, 0, 2, 8), map_put(p, 1, 2, 9), map_put(p, 2, 2, 10);
    iosfc_write(p, 0x1018, 3, 8);
    uint32_t last = nfences - 1;
    CHECK(fences[last] == 3, "the unmaps from there fenced at 3");
    hv_pvg_lite_mapper_done(p, 1, fence_gens[0]); /* the old fence, late */
    CHECK(completed(p) == 0, "the old fence completes nothing after (completed %llu)", (unsigned long long)completed(p));
    hv_pvg_lite_mapper_done(p, 3, fence_gens[last]);
    CHECK(completed(p) == 3, "the new one does");
}

/* Saved while an unmap waits for the GPU: on load the walk is derived from what was submitted, the old GPU's fence
   completes nothing, and once restored the guest is told the unmap is done (that GPU is gone) */
static void test_fence_snapshot(void) {
    printf("a fence waiting at a snapshot\n");
    HvPvgLite *p = fresh(1);
    atomic_store(&p->metal, (HvPvgMetal *)&metal_on);
    iosfc_write(p, 0x1000, PG(20), 8), iosfc_write(p, 0x1008, 4, 8);
    map_put(p, 0, 1, 7), map_put(p, 1, 2, 7);
    iosfc_write(p, 0x1018, 2, 8);
    CHECK(completed(p) == 0 && nfences == 1, "the unmap waits");
    HvPvgBlob blob = {0};
    uint64_t mlen = 0;
    char why[128] = "";
    HvPvgSink sink = {0};
    int saved = hv_pvg_lite_save(p, &blob, &sink, &mlen, NULL, 0, why, sizeof why);
    CHECK(saved == 0, "saved (%s)", why);
    uint64_t gen = fence_gens[0];
    metal_on = 0;
    HvPvgLite *q = hv_pvg_lite_create(machine, NULL, 0, 0, 1, 2);
    static const uint8_t none[1];
    HvPvgBytes ext = {none, (size_t)mlen, NULL, NULL};
    CHECK(saved == 0 && hv_pvg_lite_load(q, blob.p, blob.len, &ext) == 0, "loaded");
    CHECK(q->mapper_rp == 2 && q->mapper_fenced == 2, "the walk derived: at what was submitted (%llu)", (unsigned long long)q->mapper_rp);
    hv_pvg_lite_mapper_done(q, 2, gen);
    CHECK(completed(q) == 0, "the old GPU's fence completes nothing (completed %llu)", (unsigned long long)completed(q));
    unsigned before = iosfc_irqs;
    hv_pvg_lite_restored(q);
    CHECK(completed(q) == 2 && iosfc_irqs == before + 1, "restored: the unmap is done, and the guest told");
    free(blob.p);
}

/* A swap command, as Apple's CmdDisplaySwapMapping takes it: the ten words display_swap reads, and a zero second word */
static void test_short_swap(void) {
    printf("short swap\n");
    HvPvgLite *p = fresh(0);
    uint32_t page[5] = {1, 20, 0, 0, (uint32_t)(PG(30) >> 14)};
    hv_pvg_lite_command(p, page, 20);
    uint32_t *c = malloc(48); /* 36 bytes of arguments, at the end of an allocation */
    c[0] = 8, c[1] = 48, c[2] = 0, c[3] = 0, c[4] = 0, c[5] = 1, c[6] = 0;
    CHECK(!hv_pvg_lite_command(p, c, 48) && p->swaps == 0, "36 bytes of arguments: refused (%llu swaps)", (unsigned long long)p->swaps);
    free(c);
    uint32_t swap[13] = {8, 52, 0, 0, 0, 1};
    CHECK(hv_pvg_lite_command(p, swap, 52) == 1 && p->swaps == 1, "40: taken");
    swap[4] = 1;
    CHECK(!hv_pvg_lite_command(p, swap, 52) && p->swaps == 1, "word 1 set: refused");
    CHECK(!hv_pvg_lite_command(p, swap, 8), "a swap with no arguments at all: refused");
    /* The same on a channel with no Metal side (channel_command), each reaching its stamp */
    chan_setup(p, 1, (uint32_t)(PG(10) >> 14));
    memset(ram + (PG(10) - BASE), 0, 0x100);
    chan_put(1, 8, 12 + 36, 5), chan_put(1, 8, 12 + 40, 6);
    gfx_write(p, 0x1020, 1, 4);
    CHECK(p->swaps == 2 && chan_stamp(1) == 6, "on a channel: 36 bytes refused, 40 taken (%llu swaps, stamp %u)",
          (unsigned long long)p->swaps, chan_stamp(1));
}

/* The cursor commands, as Apple's CmdDisplayCursorGlyph and CmdDisplayCursorShow take them: the glyph needs 0x2c bytes
   of arguments and a zero second word, the show 8 bytes, and both a port Henge has a display for */
static void test_cursor_commands(void) {
    printf("cursor commands\n");
    HvPvgLite *p = fresh(0);
    uint32_t glyph[14] = {4, 56}, show[5] = {5, 20, 0, 0, 1};
    CHECK(hv_pvg_lite_command(p, glyph, 56) == 1 && hv_pvg_lite_command(p, show, 20) == 1, "port 0: taken");
    uint32_t short_glyph[13] = {4, 52}, short_show[4] = {5, 16};
    CHECK(!hv_pvg_lite_command(p, short_glyph, 52) && !hv_pvg_lite_command(p, short_show, 16), "shorter than Apple's host takes: not taken");
    glyph[4] = 1;
    CHECK(!hv_pvg_lite_command(p, glyph, 56), "a glyph with word 1 set: not taken");
    glyph[4] = 0, glyph[3] = 1, show[3] = 1;
    CHECK(!hv_pvg_lite_command(p, glyph, 56) && !hv_pvg_lite_command(p, show, 20), "port 1: not taken");
}

/* The guest's mask word at display page +0x104, read at each swap as Apple's host does before its display poke: a log
   says when it changes and as the swaps made with its bit 1 set reach each power of two */
static void test_swap_mask(void) {
    printf("the display mask word at swaps\n");
    HvPvgLite *p = fresh(0);
    uint32_t page[5] = {1, 20, 0, 0, (uint32_t)(PG(30) >> 14)}, swap[13] = {8, 52};
    hv_pvg_lite_command(p, page, 20);
    fflush(stderr);
    int saved = dup(2);
    FILE *log = tmpfile();
    dup2(fileno(log), 2);
    hv_pvg_lite_command(p, swap, 52); /* mask 0: nothing to say */
    *g32(PG(30) + 0x104) = 2;
    for (int i = 0; i < 4; i++) hv_pvg_lite_command(p, swap, 52);
    *g32(PG(30) + 0x104) = 0;
    hv_pvg_lite_command(p, swap, 52);
    fflush(stderr), dup2(saved, 2), close(saved);
    rewind(log);
    char line[256], said[4][256] = {{0}};
    int lines = 0;
    while (fgets(line, sizeof line, log))
        if (strstr(line, "display: guest mask word") && lines++ < 4) strcpy(said[lines - 1], line);
    fclose(log);
    CHECK(lines == 4, "the change, the 2nd and 4th swaps that asked, the change back: %d lines", lines);
    CHECK(strstr(said[0], "word 2 (was 0) at swap 2; 1 swaps asked") != NULL, "first: %s", said[0]);
    CHECK(strstr(said[2], "at swap 5; 4 swaps asked") != NULL, "third: %s", said[2]);
    CHECK(strstr(said[3], "word 0 (was 2) at swap 6; 4 swaps asked") != NULL, "last: %s", said[3]);
    CHECK(p->swaps == 6 && !(*g32(PG(30) + 0x100) & ~6u), "the swaps themselves as before");
}

/* The display poke, as Apple's host gives it (displayPokePort:): bit 0 of +1014, read and cleared, with the interrupt a
   stamp raises. The guest asks with its mask word at display page +104: bit 2 for the modes posted, bit 1 for a swap. */
static uint32_t pokes(HvPvgLite *p) { return (uint32_t)gfx_read(p, 0x1014, 4); }
static void test_display_poke(void) {
    printf("the display poke\n");
    HvPvgLite *p = fresh(0);
    *g32(PG(30) + 0x100) = 0x10, *g32(PG(30) + 0x104) = 4; /* a bit already there, and the mask asking for the modes */
    uint32_t page[5] = {1, 20, 0, 0, (uint32_t)(PG(30) >> 14)}, swap[13] = {8, 52};
    hv_pvg_lite_command(p, page, 20);
    CHECK(*g32(PG(30) + 0x100) == 0x14, "the modes posted or'd in: %x", *g32(PG(30) + 0x100));
    CHECK(gfx_irqs == 1 && pokes(p) == 1 && pokes(p) == 0, "poked for the modes, +1014 read and cleared (%u interrupts)", gfx_irqs);
    *g32(PG(30) + 0x104) = 2;
    hv_pvg_lite_command(p, swap, 52);
    CHECK(gfx_irqs == 2 && pokes(p) == 1, "a swap the mask asks for: poked (%u interrupts)", gfx_irqs);
    *g32(PG(30) + 0x104) = 4;
    hv_pvg_lite_command(p, swap, 52);
    CHECK(gfx_irqs == 2 && pokes(p) == 0 && p->swaps == 2, "a swap it doesn't ask for: none (%u interrupts)", gfx_irqs);
    /* Another port: Henge has one display (+122c), so Apple's host would have no display there either */
    uint32_t other[5] = {1, 20, 0, 1, (uint32_t)(PG(31) >> 14)}, swap1[13] = {8, 52, 0, 1};
    uint32_t ack1[5] = {2, 20, 0, 1, 77};
    *g32(PG(30) + 0xfbc) = 5;
    CHECK(!hv_pvg_lite_command(p, other, 20) && !hv_pvg_lite_command(p, swap1, 52) && !hv_pvg_lite_command(p, ack1, 20),
          "port 1: a set-page, a swap and an ack answered not taken (nothing to present)");
    CHECK(p->display_pa == PG(30) && p->swaps == 2 && !*g32(PG(31)) && *g32(PG(30) + 0xfbc) == 5, "port 1: none of them done");
    CHECK(hv_pvg_lite_command(p, swap, 52) == 1 && p->swaps == 3 && pokes(p) == 0, "port 0: taken");
    /* Saved with a swap's flag the mask asks for still set: the poke, not saved, is owed again after loading */
    *g32(PG(30) + 0x100) = 2, *g32(PG(30) + 0x104) = 2;
    HvPvgBlob blob = {0};
    uint64_t mlen = 0;
    char why[128] = "";
    HvPvgSink sink = {0};
    int saved = hv_pvg_lite_save(p, &blob, &sink, &mlen, NULL, 0, why, sizeof why);
    HvPvgLite *q = hv_pvg_lite_create(machine, NULL, 0, 0, 1, 2);
    static const uint8_t none[1];
    HvPvgBytes ext = {none, (size_t)mlen, NULL, NULL};
    CHECK(saved == 0 && hv_pvg_lite_load(q, blob.p, blob.len, &ext) == 0, "saved and loaded (%s)", why);
    unsigned before = gfx_irqs;
    hv_pvg_lite_restored(q);
    CHECK(gfx_irqs == before + 1 && pokes(q) == 1, "restored: poked again");
    *g32(PG(30) + 0x104) = 0;
    hv_pvg_lite_restored(q);
    CHECK(gfx_irqs == before + 1 && pokes(q) == 0, "with nothing asked for: not");
    free(blob.p);
}

/* A swap's page writes, as Apple's host makes them: +f98 the page's own u64 count plus one, +fb0 the whole u64 mapping
   id, +fa4 zero */
static void test_swap_page(void) {
    printf("a swap's page writes\n");
    HvPvgLite *p = fresh(0);
    uint32_t page[5] = {1, 20, 0, 0, (uint32_t)(PG(30) >> 14)}, swap[13] = {8, 52, 0, 0, 0, 3, 5};
    hv_pvg_lite_command(p, page, 20);
    uint64_t *count = (uint64_t *)hv_phys_ptr(NULL, PG(30) + 0xf98, 8), *mapping = (uint64_t *)hv_phys_ptr(NULL, PG(30) + 0xfb0, 8);
    *count = 0x1ffffffffull, *mapping = ~0ull, *g32(PG(30) + 0xfa4) = 7;
    hv_pvg_lite_command(p, swap, 52);
    CHECK(*count == 0x200000000ull, "the count, 64 bits from the page's own: %llx", (unsigned long long)*count);
    CHECK(*mapping == 0x500000003ull, "the mapping, 64 bits: %llx", (unsigned long long)*mapping);
    CHECK(*g32(PG(30) + 0xfa4) == 0, "+fa4 zeroed");
}

/* A guest that writes one bad header and rings in a loop: each refusal it can repeat is logged at the 1st, 10th and
   100th time, not at every doorbell write */
static int lines_with(const char *got, const char *what) {
    int k = 0;
    for (const char *at = got; (at = strstr(at, what)); at++) k++;
    return k;
}
static void test_bad_logged(void) {
    printf("refusals a doorbell repeats: logged at 1, 10, 100\n");
    HvPvgLite *p = fresh(0);
    root_setup(p);
    chan_setup(p, 1, (uint32_t)(PG(10) >> 14));
    int fds[2];
    CHECK(pipe(fds) == 0, "a pipe for stderr");
    fflush(stderr);
    int out = dup(2);
    dup2(fds[1], 2);
    for (uint32_t i = 0; i < 150; i++) {
        root_put(p->root_rp % 0x2000, 0x99, 4, 1); /* shorter than its header */
        gfx_write(p, 0x1008, p->root_rp + 12, 4);
        gfx_write(p, 0x1008, p->root_rp + 0x3000, 4); /* more than a ring ahead */
        chan_put(1, 0x99, 4, 1);
        chan_rec(1)[0] += 8; /* 12 written, as the channel's write index says */
        gfx_write(p, 0x1020, 1, 4);
    }
    fflush(stderr), dup2(out, 2), close(out), close(fds[1]);
    static char got[16384];
    ssize_t n = read(fds[0], got, sizeof got - 1);
    close(fds[0]);
    got[n > 0 ? n : 0] = 0;
    CHECK(p->bad_seen[PVG_BAD_ROOT_CMD] == 150 && p->bad_seen[PVG_BAD_ROOT_WP] == 150 && p->bad_seen[PVG_BAD_CHAN_CMD] == 150,
          "every refusal counted (%u %u %u)", p->bad_seen[PVG_BAD_ROOT_CMD], p->bad_seen[PVG_BAD_ROOT_WP], p->bad_seen[PVG_BAD_CHAN_CMD]);
    CHECK(lines_with(got, "bad root command") == 3 && lines_with(got, "more than a ring past") == 3 &&
              lines_with(got, "channel 1: bad command") == 3,
          "3 lines each, not 150 (%d %d %d)", lines_with(got, "bad root command"), lines_with(got, "more than a ring past"),
          lines_with(got, "channel 1: bad command"));
    CHECK(strstr(got, "(100 so far)") != NULL, "the count said");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    ram = calloc(1, ram_size);
    machine = zalloc(_Alignof(HvMachine), sizeof *machine); /* as aligned as it asks, 16 KB: calloc gives 16 */
    machine->ncpus = 2;
    test_root();
    test_root_deletes();
    test_defines();
    test_long_root_ops();
    test_define_order();
    test_channel();
    test_lost_doorbell();
    test_no_memory();
    test_barriers();
    test_mapper();
    test_mapper_batches();
    test_mapper_ids();
    test_unmap_after_gpu();
    test_stale_fence();
    test_lowered_fence();
    test_fence_snapshot();
    test_short_swap();
    test_cursor_commands();
    test_swap_mask();
    test_display_poke();
    test_swap_page();
    test_bad_logged();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
