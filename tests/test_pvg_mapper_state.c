/* The mapper's state (pvg_lite.c): the ids the guest unmapped and has not mapped again, kept where every mapper op is
   seen, cleared (unknown) whenever the walk moves past entries it did not read, handed to the Metal side whole when it
   is made (and so on a restore, before the saved surfaces), and saved in its own snapshot section (PVGS), which loads
   as unknown when it is missing, of another version or damaged. Guest memory and the Metal side are stand-ins: no
   CPU, GPU or timing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail_malloc; /* the next mallocs from the device fail */
static void *test_malloc(size_t n) { return fail_malloc ? NULL : malloc(n); }
#define malloc(n) test_malloc(n)
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

/* The Metal side: commands it was handed, and what it was told of the mapper, in order */
#include "pvg_metal_standin.h"
#include "zalloc.h"


static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t *g32(uint64_t pa) { return hv_phys_ptr(NULL, pa, 4); }

static HvPvgLite *fresh(int with_metal) {
    memset(ram, 0, ram_size);
    metal_on = with_metal, nhanded = nunmapped = nfences = nforgot = ntold = 0, told[0] = 0, gfx_irqs = iosfc_irqs = 0;
    while_handed = NULL, fail_malloc = 0;
    memset(handed_bits, 0, sizeof handed_bits);
    return hv_pvg_lite_create(machine, NULL, 0, 0, 1, 2);
}

/* The mapper's ring on page 20, 4 entries of 16 bytes {op, id, result, 0}; submitted counts up from 0 */
static uint64_t submitted;
static void ring_setup(HvPvgLite *p) {
    iosfc_write(p, 0x1000, PG(20), 8), iosfc_write(p, 0x1008, 4, 8);
    submitted = 0;
}
static void op(HvPvgLite *p, uint32_t o, uint32_t id) {
    uint32_t *e = g32(PG(20) + (submitted % 4) * 16);
    e[0] = o, e[1] = id, e[2] = 0, e[3] = 0;
    iosfc_write(p, 0x1018, ++submitted, 8);
    if (p->mapper_fenced != iosfc_read(p, 0x1020, 8) && nfences) hv_pvg_lite_mapper_done(p, fences[nfences - 1], fence_gens[nfences - 1]);
}
static int held(const uint8_t *bits, uint32_t id) { return bits[id / 8] >> (id % 8) & 1; }
static int none(const uint8_t *bits) {
    for (uint32_t i = 0; i < HV_PVG_MAPPING_IDS / 8; i++)
        if (bits[i]) return 0;
    return 1;
}

static void test_ops(void) {
    printf("map and unmap ops: the ids held unmapped\n");
    HvPvgLite *p = fresh(0);
    ring_setup(p);
    op(p, 2, 5), op(p, 2, 6), op(p, 1, 6), op(p, 2, 0x1fff);
    CHECK(held(p->unmapped, 5) && !held(p->unmapped, 6) && held(p->unmapped, 0x1fff), "5 and 0x1fff unmapped, 6 mapped again");
    op(p, 2, 0x2000), op(p, 2, 0), op(p, 1, 5);
    CHECK(!held(p->unmapped, 0) && !held(p->unmapped, 5), "ids the mapper refuses change nothing; 5 mapped again");
}

/* No Metal side yet: the ops are kept, and it gets them whole when it is made, before anything after */
static void test_before_metal(void) {
    printf("ops before the Metal side exists reach it when it is made\n");
    HvPvgLite *p = fresh(1);
    ring_setup(p);
    op(p, 2, 9), op(p, 2, 10), op(p, 1, 10);
    CHECK(ntold == 0, "no Metal side yet: nothing told (%s)", told);
    ensure_metal(p);
    CHECK(hv_pvg_lite_metal(p) != NULL && !strcmp(told, "s"), "made: the state handed over first (%s)", told);
    CHECK(held(handed_bits, 9) && !held(handed_bits, 10), "with 9 unmapped and 10 mapped again");
    op(p, 2, 11);
    CHECK(!strcmp(told, "su") && held(p->unmapped, 11), "then each op in order (%s)", told);
}

/* Either switch keeps the Metal side unmade: HV_NO_GPU alone does, as runtests.sh and the host's runs set it */
static void test_no_gpu(void) {
    printf("HV_NO_GPU or HV_PVG_NO_METAL: no Metal side\n");
    const char *names[2] = {"HV_NO_GPU", "HV_PVG_NO_METAL"};
    for (int i = 0; i < 2; i++) {
        HvPvgLite *p = fresh(1);
        setenv(names[i], "1", 1);
        ensure_metal(p);
        unsetenv(names[i]);
        CHECK(hv_pvg_lite_metal(p) == NULL, "%s: none made", names[i]);
    }
}

/* Every move of the walk past entries it did not read: every id unknown, here and on the Metal side */
static void test_unknown(void) {
    printf("the walk passed over entries: every id unknown\n");
    HvPvgLite *p = fresh(1);
    ensure_metal(p);
    ring_setup(p);
    op(p, 2, 5);
    iosfc_write(p, 0x1020, submitted, 8); /* the guest starting its ring again */
    CHECK(none(p->unmapped) && told[ntold - 1] == 'r', "a ring restart (0x1020): none held (%s)", told);
    ring_setup(p);
    iosfc_write(p, 0x1020, 0, 8);
    op(p, 2, 5);
    ntold = 0, told[0] = 0;
    iosfc_write(p, 0x1018, submitted + (1ull << 40), 8); /* not a ring position: passed over */
    CHECK(none(p->unmapped) && !strcmp(told, "r"), "a submitted index far past the ring: none held (%s)", told);
    ring_setup(p);
    iosfc_write(p, 0x1020, 0, 8);
    op(p, 2, 5);
    ntold = 0, told[0] = 0;
    uint32_t *e = g32(PG(20) + (submitted % 4) * 16);
    e[0] = 1, e[1] = 5; /* a map of 5 in what is passed over */
    iosfc_write(p, 0x1000, BASE + ram_size - 16, 8); /* the ring is no longer in guest memory */
    iosfc_write(p, 0x1018, submitted + 1, 8);
    CHECK(p->mapper_rp == submitted + 1, "a ring outside guest memory: passed over");
    CHECK(none(p->unmapped) && !strcmp(told, "r"), "and none held: the map it held may have been any (%s)", told);
}

/* Saved after the walk left for the polls, read back whole, or unknown whatever is wrong with it */
static HvPvgLite *saved_with(HvPvgBlob *regs, HvPvgBlob *ids, uint64_t *mlen) {
    HvPvgLite *p = fresh(1);
    ensure_metal(p);
    ring_setup(p);
    op(p, 2, 5), op(p, 2, 0x1ffe);
    HvPvgSink sink = {NULL, NULL, NULL};
    char why[160];
    CHECK(hv_pvg_lite_save(p, regs, &sink, mlen, ids, 0, why, sizeof why) == 0, "saved");
    return p;
}

static void test_saved(void) {
    printf("saved: PVGS\n");
    HvPvgBlob regs = {0}, ids = {0};
    uint64_t mlen = 0;
    saved_with(&regs, &ids, &mlen);
    CHECK(ids.len == HV_PVG_MAPPING_IDS / 8 && held(ids.p, 5) && held(ids.p, 0x1ffe) && !held(ids.p, 6), "the ids unmapped, a bit each");
    static uint8_t odd[HV_PVG_MAPPING_IDS / 8 + 4];
    memcpy(odd, ids.p, ids.len);
    struct { const char *what; const uint8_t *data; size_t len; int version; int whole; } cases[] = {
        {"whole", ids.p, ids.len, 1, 1},
        {"none (an older Henge's)", NULL, 0, -1, 0},
        {"another version", ids.p, ids.len, 2, 0},
        {"cut short", ids.p, ids.len - 1, 1, 0},
        {"longer", odd, sizeof odd, 1, 0},
        {"not read", NULL, 0, 1, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        HvPvgLite *q = fresh(1);
        q->unmapped[3] = 0xff; /* whatever was there before */
        hv_pvg_lite_load_mapper(q, cases[i].data, cases[i].len, cases[i].version);
        if (cases[i].whole) CHECK(!memcmp(q->unmapped, ids.p, ids.len), "%s: restored", cases[i].what);
        else CHECK(none(q->unmapped), "%s: every id unknown", cases[i].what);
    }
    ids.p[0] |= 1; /* id 0, which is none */
    HvPvgLite *q = fresh(1);
    hv_pvg_lite_load_mapper(q, ids.p, ids.len, 1);
    CHECK(none(q->unmapped), "id 0 held: damaged, every id unknown");
    ids.p[0] &= ~1;
    /* Restored: the Metal side gets the state before it makes the saved surfaces again */
    q = fresh(1);
    static const uint8_t no_bytes[1];
    HvPvgBytes gpu = {no_bytes, 0, NULL, NULL};
    CHECK(hv_pvg_lite_load(q, regs.p, regs.len, &gpu) == 0, "the rest loads");
    hv_pvg_lite_load_mapper(q, ids.p, ids.len, 1);
    hv_pvg_lite_restored(q);
    CHECK(!strcmp(told, "sR"), "restored: the state handed over, then the saved surfaces (%s)", told);
    CHECK(held(handed_bits, 5) && held(handed_bits, 0x1ffe), "the saved state");
    free(regs.p), free(ids.p);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    ram = calloc(1, ram_size);
    machine = zalloc(_Alignof(HvMachine), sizeof *machine); /* as aligned as it asks, 16 KB: calloc gives 16 */
    machine->ncpus = 2;
    test_ops();
    test_before_metal();
    test_no_gpu();
    test_unknown();
    test_saved();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
