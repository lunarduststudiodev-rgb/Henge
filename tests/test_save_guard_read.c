/* An armed save guard copies a page only when something writes it. A guest that only reads (the NEON post-indexed
   loads take hv_mem_emulate on a TLB miss, the plain one the fast path) claims nothing, so an auto-save of a Mac
   that is mostly reading costs no copies. The same guest told to write too must claim pages, or the first half
   would pass with a guard that sees nothing. A review found the emulated load claiming (memop.c's RAM test). */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cpu.h"
#include "dispatch.h"
#include "machine.h"
#include "saveguard.h"
#include "zalloc.h"

#define PG 16384
#define DATA_PAGES 64
#define CAP 16

/* x19 data, x20 pages, x9 page size, x21 the flag (non-zero: write each page too), x10 counts laps */
__asm__(".text\n.p2align 2\n.globl _rguest\n_rguest:\n"
        "1: mov x1, x19\n"
        "   mov x2, x20\n"
        "2: ld1 {v0.16b}, [x1], x9\n"
        "   ldur x3, [x1, #-16]\n"
        "   ldr x11, [x21]\n"
        "   cbz x11, 3f\n"
        "   sub x4, x1, x9\n"
        "   st1 {v0.16b}, [x4], x9\n"
        "3: subs x2, x2, #1\n"
        "   b.ne 2b\n"
        "   add x10, x10, #1\n"
        "   b 1b\n"
        ".globl _rguest_end\n_rguest_end:\n");
extern const uint32_t rguest[], rguest_end[];

static HvMachine m;
static HvCpu *c;
static uint64_t *flag;
static int fails;

static void *run(void *a) { hv_run(a); return NULL; }

/* Arms a fresh guard with the guest reading (write 0) or reading and writing (1) for 200 ms. */
static void phase(int write, const char *name) {
    HvSaveGuard g = {.buf = malloc((size_t)CAP * PG), .cap = CAP, .slot_page = calloc(CAP, sizeof(uint64_t))};
    hv_machine_pause(&m);
    if (hv_machine_pause_wait(&m, 2000)) { printf("FAIL %s: the guest didn't pause\n", name); exit(1); }
    __atomic_store_n(flag, (uint64_t)write, __ATOMIC_SEQ_CST);
    uint64_t laps0 = __atomic_load_n(&c->x[10], __ATOMIC_RELAXED);
    hv_guard_arm(&m, &g);
    hv_machine_resume(&m);
    usleep(200000);
    hv_guard_disarm(&m);
    hv_machine_pause(&m); /* the registers are written back at a stop, not while it runs */
    if (hv_machine_pause_wait(&m, 2000)) { printf("FAIL %s: the guest didn't pause\n", name); exit(1); }
    uint64_t laps = __atomic_load_n(&c->x[10], __ATOMIC_RELAXED) - laps0;
    hv_machine_resume(&m);
    printf("%s: %llu laps, %llu pages claimed (device %llu, cpu %llu), full %d\n", name, (unsigned long long)laps,
           (unsigned long long)g.used, (unsigned long long)g.claims_dev, (unsigned long long)g.claims_cpu, g.full);
    if (!laps) printf("FAIL %s: the guest didn't run while armed\n", name), fails++;
    if (!write && (g.used || g.full)) printf("FAIL %s: a guest that only reads claimed pages\n", name), fails++;
    if (write && !g.used) printf("FAIL %s: a writing guest claimed nothing\n", name), fails++;
    free(g.buf), free(g.slot_page);
}

int main(void) {
    if (hv_machine_init(&m, 256u << 20) != 0) return 2;
    uint8_t *code = aligned_alloc(PG, PG), *data = aligned_alloc(PG, DATA_PAGES * PG), *stack = aligned_alloc(PG, PG);
    memset(code, 0, PG), memset(stack, 0, PG);
    memcpy(code, rguest, (size_t)((const uint8_t *)rguest_end - (const uint8_t *)rguest));
    for (int i = 0x800 / 4; i < 0x1000 / 4; i++) ((uint32_t *)code)[i] = 0x14000000; /* a fault spins: no laps */
    memset(data, 0x33, DATA_PAGES * PG);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)code, PG, code, 0);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)data, DATA_PAGES * PG, data, 0);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)stack, PG, stack, 0);
    flag = (uint64_t *)stack;
    c = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
    hv_cpu_reset_index(c, &m, 0);
    c->cpacr_el1 = 3u << 20, c->el = 0, c->spsel = 0;
    c->sp = (uint64_t)(uintptr_t)stack + PG - 64;
    c->x[19] = (uint64_t)(uintptr_t)data, c->x[20] = DATA_PAGES, c->x[9] = PG, c->x[21] = (uint64_t)(uintptr_t)flag;
    c->pc = (uint64_t)(uintptr_t)code, c->vbar_el1 = (uint64_t)(uintptr_t)code + 0x800;
    pthread_t t;
    pthread_create(&t, NULL, run, c);
    usleep(50000);
    phase(0, "reading");
    phase(1, "reading and writing");
    phase(0, "reading again");
    __atomic_store_n(&c->halted, 1, __ATOMIC_SEQ_CST);
    hv_machine_kick_all(&m);
    pthread_join(t, NULL);
    if (fails) return 1;
    printf("PASS: reads claim nothing, writes are claimed\n");
    return 0;
}
