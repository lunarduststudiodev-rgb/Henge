/* The statistics block (dispatch.c print_stats) as the app prints it, with the log on: a guest loop with chaining
   off, so every block comes back through the dispatcher, runs on CPU 0 for about 1.3 s with HV_STATS=1. The block
   must come out, and the guest's floating-point controls and flags must be as they were (print_stats computes
   in floating point between hv_fp_leave and hv_fp_enter). The plain tests chain their code and finish within a
   second, so nothing else reaches it. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cpu.h"
#include "dispatch.h"
#include "machine.h"
#include "zalloc.h"

#define PG 16384
static HvMachine m;
static volatile uint32_t *flag;

static void *stopper(void *u) {
    (void)u;
    usleep(1300 * 1000);
    __atomic_store_n(flag, 1, __ATOMIC_RELEASE);
    return NULL;
}

int main(void) {
    if (hv_machine_init(&m, 64u << 20) != 0) return 1;
    m.chain = 0, m.log = 1, m.stats_interval = 1;
    uint8_t *buf = aligned_alloc(PG, 2 * PG);
    memset(buf, 0, 2 * PG);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)buf, 2 * PG, buf, 0);
    uint32_t *code = (uint32_t *)buf;
    flag = (volatile uint32_t *)(buf + PG);
    code[0] = 0xb9400261; /* loop: ldr w1, [x19] */
    code[1] = 0x14000001; /* b next: a block of its own, so the loop is two blocks, not one that loops in itself */
    code[2] = 0x34ffffc1; /* next: cbz w1, loop */
    code[3] = 0xd503201f; /* nop: stop here */
    m.stop_pc = (uint64_t)(uintptr_t)&code[3];
    HvCpu *cpu = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
    hv_cpu_reset_index(cpu, &m, 0);
    m.cpus[0] = cpu, m.ncpus = 1;
    cpu->cpacr_el1 = 3u << 20;
    cpu->el = 0, cpu->spsel = 0;
    cpu->x[19] = (uint64_t)(uintptr_t)flag;
    cpu->pc = (uint64_t)(uintptr_t)code;
    const uint64_t guest_fpcr = 3ull << 22 | 1ull << 24; /* round to zero, flush to zero */
    cpu->fpcr = guest_fpcr, cpu->fpsr = 0;

    char path[] = "/tmp/test_stats_print.XXXXXX";
    int out = mkstemp(path), err = dup(2);
    unlink(path);
    fflush(stderr);
    dup2(out, 2);
    pthread_t t;
    pthread_create(&t, NULL, stopper, NULL);
    int r = hv_run(cpu);
    pthread_join(t, NULL);
    fflush(stderr);
    dup2(err, 2);

    char text[1 << 16];
    ssize_t n = pread(out, text, sizeof text - 1, 0);
    text[n > 0 ? n : 0] = 0;
    int failures = 0;
    if (r != HV_EXIT_HALT) printf("FAIL: run ended %d at %llx\n", r, (unsigned long long)cpu->pc), failures++;
    if (!strstr(text, "stats ") || !strstr(text, "blocks ")) printf("FAIL: no statistics block printed\n"), failures++;
    if (cpu->fpcr != guest_fpcr || cpu->fpsr != 0)
        printf("FAIL: the guest's FPCR %llx FPSR %llx after the block\n", (unsigned long long)cpu->fpcr, (unsigned long long)cpu->fpsr), failures++;
    printf("%llu blocks through the dispatcher, %zd bytes of log: %s\n", (unsigned long long)HV_STAT_TOTAL(&m, blocks), n,
           failures ? "FAILED" : "ok");
    return failures != 0;
}
