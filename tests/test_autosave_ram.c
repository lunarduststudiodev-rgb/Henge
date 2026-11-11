/* The no-freeze auto-save of memory, end to end: a Mac whose memory is a RAM file (core/ramfile.h) plus a
   firmware region, guest CPUs storing all the while (plain, pair, atomic and DC ZVA stores) and a device thread
   writing as devices do. Each round:

     before the stop   catch up, prepare, allocate the guard's buffer
     the stop          pause the CPUs, record the memory (the truth at T), arm the guard, mark the checks, copy the
                       firmware region, resume with the guest's clock running on
     the Mac running   claim the GPU's pages, take the files' copy, end the guard, list the claimed pages, write
                       the snapshot (the claimed pages and the checks are part of it)

   then the snapshot is loaded into a fresh Mac, whose memory must be the truth at T byte for byte, while the first
   Mac has gone on changing its own. The background writer runs all the while, as fast as it can: a store the
   dirty map missed (core/ramfile.h) leaves the files, and so the resumed memory, wrong. The stop's length is measured and reported.

   usage: test_autosave_ram [folder] [rounds=4] [cpus=3]   (exit 1 = a failure) */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach_time.h>

#include "cpu.h"
#include "dispatch.h"
#include "machine.h"
#include "ramfile.h"
#include "saveguard.h"
#include "snapshot.h"
#include "scratch_dir.h"
#include "zalloc.h"

static int failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fprintf(stderr, "\n");                                                                                     \
        }                                                                                                              \
    } while (0)

#define PG 16384ull
#define RAM_PA 0x70000000ull
#define RAM_SIZE (256ull << 20)
#define FW_PA 0x100000ull
#define FW_SIZE (1ull << 20)
#define DATA_OFF (64ull << 20)  /* the guest's area in RAM: three spans of SPAN, one per kind of store */
#define SPAN (16ull << 20)
#define DEV_OFF (128ull << 20)  /* the device's: 8 MB */
#define DEV_SIZE (8ull << 20)

/* x19 area, x20 pages per span, x21 this CPU's offset in a page, x22/x23 the second and third span, x24 the
   step (a page), x6 = 1. Code and vectors live in the firmware region. */
__asm__(".text\n.p2align 2\n.globl _ar_guest\n_ar_guest:\n"
        "1: mov x1, x19\n"
        "   mov x2, x20\n"
        "2: ldr x3, [x1, x21]\n"
        "   add x3, x3, #1\n"
        "   str x3, [x1, x21]\n"
        "   add x4, x1, x21\n"
        "   stp x3, x3, [x4, #8]\n"
        "   add x5, x4, x22\n"
        "   ldadd x6, x7, [x5]\n"
        "   add x8, x4, x23\n"
        "   dc zva, x8\n"
        "   add x1, x1, x24\n"
        "   subs x2, x2, #1\n"
        "   b.ne 2b\n"
        "   b 1b\n"
        ".globl _ar_guest_end\n_ar_guest_end:\n");
extern const uint32_t ar_guest[], ar_guest_end[];

/* The probe: each time the word at x25 changes, stores it at x19 and x19 + x27 (two pages of RAM), then at x26
   (the acknowledgement, in the firmware region). */
__asm__(".text\n.p2align 2\n.globl _ar_probe\n_ar_probe:\n"
        "1: ldr x3, [x25]\n"
        "   cmp x3, x4\n"
        "   b.eq 1b\n"
        "   str x3, [x19]\n"
        "   str x3, [x19, x27]\n"
        "   mov x4, x3\n"
        "   str x3, [x26]\n"
        "   b 1b\n"
        ".globl _ar_probe_end\n_ar_probe_end:\n");
extern const uint32_t ar_probe[], ar_probe_end[];
#define PROBE_CODE 0x400ull
#define PROBE_GO 0xf0000ull
#define PROBE_ACK 0xf0008ull
#define PROBE_OFF (160ull << 20) /* the two pages it stores to */

typedef struct Mac {
    HvMachine m;
    HvRamFile *f;
    uint8_t *ram, *fw;
    int ram_region;
} Mac;

static int make_mac(Mac *mac, const char *run) {
    memset(mac, 0, sizeof *mac);
    if (hv_machine_init(&mac->m, 64u << 20) != 0) return -1;
    mac->fw = mmap(NULL, FW_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    mac->f = hv_ramfile_open(run, RAM_SIZE);
    if (!mac->f || mac->fw == MAP_FAILED) return -1;
    mac->ram = hv_ramfile_base(mac->f);
    hv_machine_add_ram(&mac->m, FW_PA, FW_SIZE, mac->fw, 0);
    if (hv_machine_add_ram(&mac->m, RAM_PA, RAM_SIZE, mac->ram, 0) != 0) return -1;
    mac->ram_region = mac->m.nram - 1;
    return hv_machine_attach_file(&mac->m, &mac->m.ram[mac->ram_region], mac->f);
}

static Mac A; /* the Mac that runs */
static pthread_mutex_t devmu = PTHREAD_MUTEX_INITIALIZER; /* plays hv_dev_lock */
static volatile int dev_stop;
static uint64_t dev_writes;

static void *run_cpu(void *a) {
    HvCpu *c = a;
    int r = hv_run(c);
    if (r != HV_EXIT_HALT)
        fprintf(stderr, "cpu %u exit %d: esr %llx far %llx elr %llx\n", c->index, r, (unsigned long long)c->esr_el1,
                (unsigned long long)c->far_el1, (unsigned long long)c->elr_el1);
    return NULL;
}

/* A device: DMA into its buffers under the device lock, through a pointer or a copy, and now and then a firmware
   variable (the firmware region is guarded too). */
static void *device(void *unused) {
    (void)unused;
    uint32_t seed = 777;
    while (!dev_stop) {
        seed = seed * 1664525u + 1013904223u;
        uint64_t at = RAM_PA + DEV_OFF + (seed >> 4) % (DEV_SIZE - 512);
        uint8_t v[300];
        memset(v, (int)(seed >> 24), sizeof v);
        pthread_mutex_lock(&devmu);
        if (seed & 1) {
            uint8_t *p = hv_phys_ptr_w(&A.m, at, sizeof v);
            if (p) memcpy(p, v, sizeof v);
        } else {
            hv_phys_write(&A.m, at, v, sizeof v);
        }
        if ((seed & 63) == 0) hv_phys_write(&A.m, FW_PA + 0x80000 + (seed >> 8) % 0x7000, v, 16);
        pthread_mutex_unlock(&devmu);
        dev_writes++;
        if ((seed & 15) == 0) usleep(30);
    }
    return NULL;
}

static void claim_gpu(void *ctx, uint64_t off, uint64_t len) {
    Mac *mac = ctx;
    hv_guard_claim_range(&mac->m, mac->m.ram[mac->ram_region].pa + off, len);
}

static uint64_t now_us(void) { return mach_absolute_time() / 24; }

/* The probe stores v (and has, when this returns). */
static int probe_store(uint64_t v) {
    __atomic_store_n((uint64_t *)(void *)(A.fw + PROBE_GO), v, __ATOMIC_RELEASE);
    for (uint64_t w0 = now_us(); __atomic_load_n((uint64_t *)(void *)(A.fw + PROBE_ACK), __ATOMIC_ACQUIRE) != v;) {
        if (now_us() - w0 > 2000000) return -1;
        usleep(50);
    }
    return 0;
}

/* What run file 0001 holds at off (the files, not the memory). */
static uint64_t file_word(const char *run, uint64_t off) {
    char p[1100];
    snprintf(p, sizeof p, "%s/%04u", run, (unsigned)(off / (128ull << 20)));
    int fd = open(p, O_RDONLY);
    uint64_t v = UINT64_MAX;
    if (fd >= 0 && pread(fd, &v, 8, (off_t)(off % (128ull << 20))) != 8) v = UINT64_MAX;
    if (fd >= 0) close(fd);
    return v;
}

/* A CPU's store to a page after the writer took it marks it again, though the CPU could write the page before (its
   TLB entry to it is taken back as the page is taken): one the dirty map missed would never reach the files. */
static void probe(const char *run) {
    memcpy(A.fw + PROBE_CODE, ar_probe, (size_t)((const uint8_t *)ar_probe_end - (const uint8_t *)ar_probe));
    memset(A.fw + PROBE_GO, 0, 16);
    HvCpu *c = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
    hv_cpu_reset_index(c, &A.m, 0);
    c->cpacr_el1 = 3u << 20, c->el = 0, c->spsel = 0;
    c->x[19] = RAM_PA + PROBE_OFF, c->x[25] = FW_PA + PROBE_GO, c->x[26] = FW_PA + PROBE_ACK, c->x[27] = PG;
    c->pc = FW_PA + PROBE_CODE;
    c->vbar_el1 = FW_PA + 0x800;
    pthread_t t;
    pthread_create(&t, NULL, run_cpu, c);
    uint8_t *d = hv_ramfile_dirty_map(A.f);
    uint64_t p0 = PROBE_OFF / PG, p1 = p0 + 1;
    CHECK(hv_ramfile_sync(A.f, 2, NULL, 0) == 0 && !d[p0] && !d[p1], "probe: clean to begin with");
    for (uint64_t v = 1; v <= 4; v++) {
        CHECK(probe_store(v) == 0, "probe: stored %llu", (unsigned long long)v);
        CHECK(d[p0] && d[p1], "probe: store %llu marked both pages (%u %u)", (unsigned long long)v, d[p0], d[p1]);
        CHECK(hv_ramfile_sync(A.f, 2, NULL, 0) == 0 && !d[p0] && !d[p1], "probe: written");
        CHECK(file_word(run, PROBE_OFF) == v && file_word(run, PROBE_OFF + PG) == v, "probe: the files hold %llu: %llu %llu",
              (unsigned long long)v, (unsigned long long)file_word(run, PROBE_OFF), (unsigned long long)file_word(run, PROBE_OFF + PG));
    }
    __atomic_store_n(&c->halted, 1, __ATOMIC_SEQ_CST);
    hv_machine_kick_all(&A.m);
    pthread_join(t, NULL);
}

/* Some of the memory, as a Mac's would be, before the CPUs start: the files then hold most of it. */
static void fill(uint8_t *p, uint64_t n, uint64_t s) {
    for (uint64_t k = 0; k < n / 8; k += 3) ((uint64_t *)(void *)p)[k] = s = s * 6364136223846793005ull + 1442695040888963407ull;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *dir = scratch_dir(argc, argv, "test_autosave_ram");
    int rounds = argc > 2 ? atoi(argv[2]) : 4, ncpu = argc > 3 ? atoi(argv[3]) : 3;
    if (rounds < 1 || ncpu < 1 || ncpu > 8) return 1;
    char run1[1024], run2[1024], snap[1024], why[256];
    snprintf(run1, sizeof run1, "%s/run1", dir);
    snprintf(run2, sizeof run2, "%s/run2", dir);
    if (make_mac(&A, run1) != 0) {
        fprintf(stderr, "can't make the Mac\n");
        return 1;
    }
    const char *sample = getenv("AR_SAMPLE");
    if (!sample || !*sample) hv_ramfile_check_all(A.f, 1); /* every piece checked at the resume, not a sample */
    A.m.chain = 1;
    memcpy(A.fw, ar_guest, (size_t)((const uint8_t *)ar_guest_end - (const uint8_t *)ar_guest));
    for (int i = 0x800 / 4; i < 0x1000 / 4; i++) ((uint32_t *)(void *)A.fw)[i] = 0x14000000; /* vectors: b . */
    fill(A.ram, RAM_SIZE, 1);
    hv_ramfile_mark_dirty(A.f, 0, RAM_SIZE);
    fill(A.fw + 0x10000, FW_SIZE - 0x10000, 2);
    probe(run1);
    if (failures) return 1;
    /* The background writer too, writing any page as soon as it can: its passes race the CPUs' stores (a store after
       a page is taken must mark it again) */
    hv_ramfile_set_policy(A.f, 1, 1e12, 1ull << 40, 1);
    if (!getenv("AR_NO_WRITER")) hv_ramfile_writer_start(A.f);

    HvCpu **c = calloc(ncpu, sizeof *c);
    pthread_t *t = calloc(ncpu, sizeof *t), dt;
    for (int k = 0; k < ncpu; k++) {
        c[k] = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
        hv_cpu_reset_index(c[k], &A.m, k);
        c[k]->cpacr_el1 = 3u << 20, c[k]->el = 0, c[k]->spsel = 0;
        c[k]->x[19] = RAM_PA + DATA_OFF, c[k]->x[20] = SPAN / PG, c[k]->x[21] = 64u * k;
        c[k]->x[22] = SPAN, c[k]->x[23] = 2 * SPAN, c[k]->x[24] = PG, c[k]->x[6] = 1;
        c[k]->pc = FW_PA;
        c[k]->vbar_el1 = FW_PA + 0x800;
        c[k]->sctlr_el1 |= 1u << 14; /* DZE */
    }
    for (int k = 0; k < ncpu; k++) pthread_create(&t[k], NULL, run_cpu, c[k]);
    pthread_create(&dt, NULL, device, NULL);

    uint8_t *truth = malloc(RAM_SIZE), *fw_truth = malloc(FW_SIZE), *fw_copy = malloc(FW_SIZE);
    uint64_t worst_stop = 0, total_claimed = 0;
    for (int r = 0; r < rounds && !failures; r++) {
        usleep(20000 + (r * 7919) % 30000);
        snprintf(snap, sizeof snap, "%s/save%d.hengesnap", dir, r);
        /* Before the stop */
        uint64_t left = hv_ramfile_catch_up(A.f, 0, 4, 2000000000ull, 4, NULL, 0);
        HvRamFileSave s;
        CHECK(hv_ramfile_prepare(A.f, &s) == 0, "round %d: prepared", r);
        HvSaveGuard g;
        CHECK(hv_guard_alloc(&g, dir, RAM_SIZE / PG + FW_SIZE / PG + 64, 4096, 0) == 0, "round %d: the guard's buffer", r);
        if (failures) break;

        /* The stop */
        uint64_t t0 = now_us();
        hv_machine_pause(&A.m);
        CHECK(hv_machine_pause_wait(&A.m, 2000) == 0, "round %d: paused", r);
        if (failures) break;
        pthread_mutex_lock(&devmu);
        uint64_t t_paused = now_us();
        CHECK(hv_guard_arm(&A.m, &g) == 0, "round %d: armed", r);
        uint64_t t_armed = now_us();
        CHECK(hv_ramfile_mark(A.f, 4, &s) == 0, "round %d: marked", r);
        uint64_t t_marked = now_us();
        memcpy(fw_copy, A.fw, FW_SIZE);
        uint64_t t_done = now_us();
        /* (the test's own record of T, outside the measured stop) */
        memcpy(truth, A.ram, RAM_SIZE);
        memcpy(fw_truth, A.fw, FW_SIZE);
        uint64_t t_truth = now_us();
        pthread_mutex_unlock(&devmu);
        hv_machine_resume_live(&A.m);
        uint64_t t_resumed = now_us();
        uint64_t stop = (t_paused - t0) + (t_done - t_paused) + (t_resumed - t_truth);
        if (stop > worst_stop) worst_stop = stop;

        /* The Mac running: the save */
        for (uint64_t w0 = now_us(); !memcmp(A.ram + DATA_OFF, truth + DATA_OFF, PG);) {
            if (now_us() - w0 > 2000000) {
                CHECK(0, "round %d: the guest did not store for 2 s", r);
                break;
            }
            usleep(100);
        }
        uint64_t gp = hv_ramfile_claim_gpu(A.f, claim_gpu, &A);
        usleep(5000);
        CHECK(hv_ramfile_sync(A.f, 2, NULL, 0) == 0, "round %d: the files written as the Mac runs", r); /* post-T bytes in the files */
        int rc = hv_ramfile_take(A.f, snap, 4, NULL, 0, &s);
        CHECK(rc == 0, "round %d: taken: %d", r, rc);
        usleep(5000);
        hv_ramfile_claim_stop(A.f);
        uint64_t used = hv_guard_disarm(&A.m);
        CHECK(!g.full, "round %d: the guard's buffer held every page (%llu)", r, (unsigned long long)g.used);
        rc = rc == 0 ? hv_ramfile_claimed(A.f, snap, &s, g.slot_page, used, (unsigned)A.ram_region, g.buf) : -1;
        CHECK(rc == 0, "round %d: claimed: %d", r, rc);
        if (failures) {
            hv_ramfile_save_free(&s);
            hv_guard_release(&g);
            break;
        }
        total_claimed += used;
        HvSnapWriter *w = hv_snap_begin(snap, HV_SNAP_NO_SYNC);
        CHECK(w != NULL, "round %d: begin", r);
        HvSnapTaken tk;
        memset(&tk, 0, sizeof tk);
        tk.file[A.ram_region] = &s;
        tk.host[0] = fw_copy;
        rc = w ? hv_snap_put_ram_taken(w, &A.m, 4, &tk) : -1;
        CHECK(rc == 0, "round %d: put", r);
        if (w) {
            if (rc == 0) CHECK(hv_snap_commit(w) == 0, "round %d: committed", r);
            else hv_snap_abort(w);
        }
        hv_ramfile_save_free(&s);
        hv_guard_release(&g);
        CHECK(memcmp(A.ram, truth, RAM_SIZE) != 0, "round %d: (the Mac ran on)", r);

        /* The resume, in another Mac */
        Mac B;
        CHECK(make_mac(&B, run2) == 0, "round %d: a second Mac", r);
        HvSnapReader *rd = hv_snap_open(snap, why, sizeof why);
        CHECK(rd != NULL, "round %d: open: %s", r, why);
        if (rd) {
            CHECK(hv_snap_check_ram(rd, &B.m, why, sizeof why) == 0, "round %d: check: %s", r, why);
            CHECK(hv_snap_get_ram(rd, &B.m, 4, HV_SNAP_DIRTY_RAM) == 0, "round %d: loaded", r);
            hv_snap_close(rd);
            uint64_t bad = 0, first = 0;
            for (uint64_t p = 0; p < RAM_SIZE / PG; p++)
                if (memcmp(B.ram + p * PG, truth + p * PG, PG) && !bad++) first = p;
            CHECK(!bad, "round %d: %llu pages of the resumed memory aren't T's (first %llu)", r, (unsigned long long)bad,
                  (unsigned long long)first);
            CHECK(memcmp(B.fw, fw_truth, FW_SIZE) == 0, "round %d: the firmware region is T's", r);
        }
        hv_ramfile_close(B.f);
        munmap(B.fw, FW_SIZE);
        hv_machine_free(&B.m);
        printf("round %d: stop %llu us (pause %llu, arm %llu, checks %llu, firmware %llu, resume %llu), %llu left before it, %llu GPU pages, %llu pages claimed\n", r, (unsigned long long)stop,
               (unsigned long long)(t_paused - t0), (unsigned long long)(t_armed - t_paused), (unsigned long long)(t_marked - t_armed),
               (unsigned long long)(t_done - t_marked), (unsigned long long)(t_resumed - t_truth), (unsigned long long)left, (unsigned long long)gp, (unsigned long long)used);
        hv_ramfile_remove_saved(snap, NULL);
        unlink(snap);
    }

    dev_stop = 1;
    pthread_join(dt, NULL);
    for (int k = 0; k < ncpu; k++) __atomic_store_n(&c[k]->halted, 1, __ATOMIC_SEQ_CST);
    hv_machine_kick_all(&A.m);
    for (int k = 0; k < ncpu; k++) pthread_join(t[k], NULL);
    if (!failures) scratch_done();
    if (failures) {
        printf("test_autosave_ram: %d failures\n", failures);
        return 1;
    }
    printf("test_autosave_ram: all passed (%d rounds, %d CPUs, %llu device writes, %llu pages claimed, stop <= %llu us)\n",
           rounds, ncpu, (unsigned long long)dev_writes, (unsigned long long)total_claimed, (unsigned long long)worst_stop);
    return 0;
}
