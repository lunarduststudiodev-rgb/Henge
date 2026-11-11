/* The save guard (core/saveguard.h): RAM as it was at one instant, kept while guest CPUs and a device go on
   writing it.

   Guest CPUs loop over every page of a data area with each kind of store the translator has: a plain STR (the
   fast path, through a TLB write tag), STP, an LSE atomic (memop.c's host pointer), and DC ZVA (the slow path,
   hv_vmem_xfer). A host thread plays a device, writing through hv_phys_ptr_w and hv_phys_write. Each round:
   pause the CPUs, record the area (the truth at T), arm the guard, resume; while they write, copy the area as a
   save would (a "clone" that races the writers); disarm; lay the claimed pages over the clone. It must equal
   the truth byte for byte, every round. Also: a full buffer voids the save without stopping anyone, and pages
   left after a claim go back to the fast path.

   usage: test_save_guard [rounds=300] [cpus=3]   (exit 1 = a saved page was not the instant's) */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include "cpu.h"
#include "dispatch.h"
#include "machine.h"
#include "saveguard.h"
#include "zalloc.h"
#define PG 16384
#define DATA_PAGES 128 /* 3 x 32 for the guest, the last 32 for the device */

/* The guest: x19 data, x20 pages per kind (a third of the area), x21 this CPU's offset in a page (64-byte
   aligned), x6 = 1. Each kind of store has pages of its own, so each is the first to write its pages after T and
   the test fails if that kind is not guarded: STR/STP in the first third, the LSE atomic in the second, DC ZVA
   (slow path) in the last. */
__asm__(".text\n.p2align 2\n.globl _guest_code\n_guest_code:\n"
        "1: mov x1, x19\n"
        "   mov x2, x20\n"
        "2: ldr x3, [x1, x21]\n"
        "   add x3, x3, #1\n"
        "   str x3, [x1, x21]\n"
        "   add x4, x1, x21\n"
        "   stp x3, x3, [x4, #8]\n"
        "   add x5, x4, #0x80, lsl #12\n"   /* + 32 pages */
        "   ldadd x6, x7, [x5]\n"
        "   add x8, x4, #0x100, lsl #12\n"  /* + 64 pages */
        "   dc zva, x8\n"
        "3: add x1, x1, #4, lsl #12\n"
        "   subs x2, x2, #1\n"
        "   b.ne 2b\n"
        "   b 1b\n"
        ".globl _guest_code_end\n_guest_code_end:\n");
extern const uint32_t guest_code[], guest_code_end[];

static HvMachine m;
static uint8_t *data;
static pthread_mutex_t devmu = PTHREAD_MUTEX_INITIALIZER;
static volatile int dev_stop;
static uint64_t dev_writes;

static void *run(void *a) {
    HvCpu *c = a;
    int r = hv_run(c);
    if (r != HV_EXIT_HALT)
        fprintf(stderr, "cpu %u exit %d: esr %llx far %llx elr %llx (code at %llx)\n", c->index, r, (unsigned long long)c->esr_el1,
                (unsigned long long)c->far_el1, (unsigned long long)c->elr_el1, (unsigned long long)c->x[30]);
    return NULL;
}

/* A device: writes under the device lock (devmu), as devices write under hv_dev_lock. */
static void *device(void *unused) {
    (void)unused;
    uint32_t seed = 12345;
    while (!dev_stop) {
        seed = seed * 1664525u + 1013904223u;
        uint64_t pg = 96 + seed % 32, at = pg * PG + 8192 + (seed >> 8) % 4096;
        uint8_t v[48];
        memset(v, (int)(seed >> 24), sizeof v);
        pthread_mutex_lock(&devmu);
        if (seed % 3 == 0) {
            uint8_t *p = hv_phys_ptr_w(&m, (uint64_t)(uintptr_t)data + at, sizeof v);
            if (p) memcpy(p, v, sizeof v);
        } else if (seed % 3 == 1) { /* the default pointer: guarded as well, without saying so */
            uint8_t *p = hv_phys_ptr(&m, (uint64_t)(uintptr_t)data + at, sizeof v);
            if (p) memcpy(p, v, sizeof v);
        } else {
            hv_phys_write(&m, (uint64_t)(uintptr_t)data + at, v, sizeof v);
        }
        pthread_mutex_unlock(&devmu);
        dev_writes++;
        if ((seed & 15) == 0) usleep(50);
    }
    return NULL;
}

static uint64_t now_us(void) { return mach_absolute_time() / 24; }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int rounds = argc > 1 ? atoi(argv[1]) : 300, ncpu = argc > 2 ? atoi(argv[2]) : 3;
    if (ncpu < 1 || ncpu > 8) return 1;
    if (hv_machine_init(&m, 256u << 20) != 0) return 1;
    m.chain = 1;
    uint8_t *code = aligned_alloc(PG, PG);
    memset(code, 0, PG);
    memcpy(code, guest_code, (size_t)((const uint8_t *)guest_code_end - (const uint8_t *)guest_code));
    for (int i = 0x800 / 4; i < 0x1000 / 4; i++) ((uint32_t *)code)[i] = 0x14000000; /* vectors: b . (an exception is a failure) */
    data = aligned_alloc(PG, DATA_PAGES * PG);
    memset(data, 0, DATA_PAGES * PG);
    uint8_t *stack = aligned_alloc(PG, 8 * PG);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)code, PG, code, 0);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)data, DATA_PAGES * PG, data, 0);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)stack, 8 * PG, stack, 0);
    HvRam *dr = hv_ram_lookup(&m, (uint64_t)(uintptr_t)data);
    int dri = (int)(dr - m.ram);

    HvCpu **c = calloc(ncpu, sizeof *c);
    pthread_t *t = calloc(ncpu, sizeof *t), dt;
    for (int k = 0; k < ncpu; k++) {
        c[k] = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
        hv_cpu_reset_index(c[k], &m, k);
        c[k]->cpacr_el1 = 3u << 20, c[k]->el = 0, c[k]->spsel = 0;
        c[k]->sp = (uint64_t)(uintptr_t)stack + (k + 1) * PG - 64;
        c[k]->x[19] = (uint64_t)(uintptr_t)data, c[k]->x[20] = 32, c[k]->x[21] = 64u * k, c[k]->x[6] = 1;
        c[k]->pc = (uint64_t)(uintptr_t)code;
        c[k]->vbar_el1 = (uint64_t)(uintptr_t)code + 0x800;
        c[k]->sctlr_el1 |= 1u << 14; /* DZE: DC ZVA allowed at EL0 */
    }
    for (int k = 0; k < ncpu; k++) pthread_create(&t[k], NULL, run, c[k]);
    pthread_create(&dt, NULL, device, NULL);

    uint64_t cap = DATA_PAGES + 64;
    HvSaveGuard g = {.buf = malloc(cap * PG), .cap = cap, .slot_page = calloc(cap, sizeof(uint64_t))};
    uint8_t *truth = malloc(DATA_PAGES * PG), *clone = malloc(DATA_PAGES * PG);
    int fails = 0;
    uint64_t claims = 0, cpu_claims = 0, waits = 0, worst_arm = 0;
    for (int r = 0; r < rounds && !fails; r++) {
        usleep(200 + (r * 37) % 1500);
        hv_machine_pause(&m);
        if (hv_machine_pause_wait(&m, 2000) != 0) { printf("FAIL: the CPUs did not pause\n"); return 1; }
        pthread_mutex_lock(&devmu);
        memcpy(truth, data, DATA_PAGES * PG);
        uint64_t t0 = now_us();
        if (hv_guard_arm(&m, &g) != 0) { printf("FAIL: arm\n"); return 1; }
        if (now_us() - t0 > worst_arm) worst_arm = now_us() - t0;
        pthread_mutex_unlock(&devmu);
        hv_machine_resume(&m);
        /* Until the guest has written while armed (else the round proves nothing); the host may take a while to
           wake its threads (up to 20 ms seen on a busy Mac, armed or not). */
        for (uint64_t w0 = now_us(); !memcmp(data, truth, 96 * PG);) {
            if (now_us() - w0 > 2000000) {
                printf("FAIL: round %d: the guest did not write for 2 s while armed (esr %llx elr %llx)\n", r,
                       (unsigned long long)c[0]->esr_el1, (unsigned long long)c[0]->elr_el1);
                return 1;
            }
            usleep(50);
        }
        usleep((r * 53) % 800);
        /* The save's clone: read while the writers go on. */
        for (int p = 0; p < DATA_PAGES; p++) {
            memcpy(clone + (size_t)p * PG, data + (size_t)p * PG, PG);
            if (p % 16 == 0) usleep(20);
        }
        uint64_t used = hv_guard_disarm(&m);
        if (g.full) { printf("FAIL: buffer full in round %d (%llu used)\n", r, (unsigned long long)g.used); return 1; }
        for (uint64_t k = 0; k < used; k++) {
            int ri = (int)(g.slot_page[k] >> 48);
            uint64_t pg = g.slot_page[k] & 0xffffffffffffull;
            if (ri == dri) memcpy(clone + pg * PG, g.buf + k * PG, PG);
        }
        if (memcmp(clone, truth, DATA_PAGES * PG) != 0) {
            for (int p = 0; p < DATA_PAGES; p++)
                if (memcmp(clone + (size_t)p * PG, truth + (size_t)p * PG, PG)) {
                    printf("FAIL: round %d page %d differs from the instant (%llu claimed)\n", r, p, (unsigned long long)used);
                    break;
                }
            fails++;
        }
        claims += used, cpu_claims += g.claims_cpu, waits += g.waits;
    }

    /* A buffer too small: the save is void, the writers never wait for room. */
    {
        HvSaveGuard small = {.buf = malloc(4 * PG), .cap = 4, .slot_page = calloc(4, sizeof(uint64_t))};
        uint64_t before = c[0]->x[3];
        hv_machine_pause(&m);
        if (hv_machine_pause_wait(&m, 2000) != 0) return 1;
        hv_guard_arm(&m, &small);
        hv_machine_resume(&m);
        usleep(20000);
        hv_guard_disarm(&m);
        if (!small.full) printf("FAIL: a 4-page buffer did not fill\n"), fails++;
        if (c[0]->x[3] == before)
            printf("FAIL: the CPU stopped while the buffer was full (esr %llx elr %llx far %llx)\n", (unsigned long long)c[0]->esr_el1,
                   (unsigned long long)c[0]->elr_el1, (unsigned long long)c[0]->far_el1), fails++;
    }
    /* Not armed: nothing is claimed, the stores go on as fast (the check is one load). */
    if (hv_guard_disarm(&m) != 0) printf("FAIL: disarm when not armed\n"), fails++;

    dev_stop = 1;
    pthread_join(dt, NULL);
    for (int k = 0; k < ncpu; k++) __atomic_store_n(&c[k]->halted, 1, __ATOMIC_SEQ_CST);
    hv_machine_kick_all(&m);
    for (int k = 0; k < ncpu; k++) pthread_join(t[k], NULL);

    /* Nothing else running: a read-only pointer claims nothing; the default pointer keeps the page as it was. */
    {
        uint64_t pa = (uint64_t)(uintptr_t)data + 110 * PG + 100;
        memset(data + 110 * PG, 0x5a, PG);
        HvSaveGuard one = {.buf = malloc(4 * PG), .cap = 4, .slot_page = calloc(4, sizeof(uint64_t))};
        hv_guard_arm(&m, &one);
        const uint8_t *r = hv_phys_ptr_r(&m, pa, 64);
        if (!r || r[0] != 0x5a || one.used != 0) printf("FAIL: hv_phys_ptr_r claimed (used %llu)\n", (unsigned long long)one.used), fails++;
        uint8_t *w = hv_phys_ptr(&m, pa, 64);
        if (w) memset(w, 0xa5, 64);
        uint64_t used = hv_guard_disarm(&m);
        if (used != 1 || one.slot_page[0] != ((uint64_t)dri << 48 | 110) || one.buf[100] != 0x5a || one.buf[0] != 0x5a)
            printf("FAIL: a write through hv_phys_ptr wasn't guarded (used %llu)\n", (unsigned long long)used), fails++;
        if (data[110 * PG + 100] != 0xa5) printf("FAIL: the write didn't land\n"), fails++;
        /* Not armed: the default pointer claims nothing */
        if (!hv_phys_ptr(&m, pa, 64) || one.used != 1) printf("FAIL: a claim with nothing armed\n"), fails++;
        free(one.buf), free(one.slot_page);
    }
    if (fails) return 1;
    printf("ok: %d rounds, %d CPUs, %llu pages claimed (%llu by stores), %llu waits, %llu device writes, arm <= %llu us\n",
           rounds, ncpu, (unsigned long long)claims, (unsigned long long)cpu_claims, (unsigned long long)waits,
           (unsigned long long)dev_writes, (unsigned long long)worst_arm);
    return 0;
}
