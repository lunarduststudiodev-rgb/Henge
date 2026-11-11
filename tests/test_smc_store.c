/* A guest CPU rewrites code with ordinary stores, which go through the JIT's
   fast write path, while other guest CPUs keep running that code. Every round
   the writer stores "mov x0,#k" into F and then publishes k; each reader calls
   F after seeing k and acks the value F returned. A reader that ran an old
   translation acks a smaller number and the writer waits forever.

   This guards the code-page protection in dispatch.c (protect_code_page): a
   page must not be translated until every CPU has lost its writable TLB entry
   for it and finished any store already in flight, and a second translator
   must not read the bytes while the first is still protecting the page.

   "big" runs it under HV_BIG_LOCK with a kick every millisecond: a CPU waiting
   for the lock must not be one the holder waits for (it hung at once before).

   usage: test_smc_store [readers=3] [rounds=3000] [big]   (exit 1 = stale code ran or hung)
   With no arguments: 3 readers, 3000 rounds, then the same with the big lock for 1000. */
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include "cpu.h"
#include "dispatch.h"
#include "machine.h"
#include "zalloc.h"
#define PG 16384
extern char **environ;
static HvMachine m;
static uint8_t *buf;
static volatile uint32_t *flag, *ack;
static uint32_t *code0, *wcode, *fpage;
static void *run(void *a) { HvCpu *c = a; int r = hv_run(c); if (r != HV_EXIT_HALT) fprintf(stderr, "cpu %u exit %d\n", c->index, r); return NULL; }
static int run_child(char *self, char *rounds, char *mode) {
    char *args[] = {self, "3", rounds, mode, NULL};
    pid_t pid;
    if (posix_spawn(&pid, self, NULL, NULL, args, environ) != 0) return 1;
    int st;
    while (waitpid(pid, &st, 0) < 0) {}
    return !WIFEXITED(st) || WEXITSTATUS(st) != 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 1) return run_child(argv[0], "3000", "plain") | run_child(argv[0], "1000", "big");
    int nread = argc > 1 ? atoi(argv[1]) : 3, rounds = argc > 2 ? atoi(argv[2]) : 3000;
    int big = argc > 3 && !strcmp(argv[3], "big");
    if (nread < 1 || nread >= HV_MAX_CPUS) return 1;
    if (hv_machine_init(&m, 256u << 20) != 0) return 1;
    m.chain = 1;
    m.big_lock_on = big;
    buf = aligned_alloc(PG, 6 * PG); memset(buf, 0, 6 * PG);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)buf, 6 * PG, buf, 0);
    code0 = (uint32_t *)buf; fpage = (uint32_t *)(buf + PG); flag = (volatile uint32_t *)(buf + 2 * PG); ack = flag + 16;
    wcode = (uint32_t *)(buf + 3 * PG);
    fpage[0] = 0xd2800000; fpage[1] = 0xd65f03c0;
    int i = 0; uint32_t loop = 0;
    code0[i++] = 0x88dffe61; code0[i++] = 0xd5033fdf; code0[i++] = 0xd63f02e0; code0[i++] = 0x6b01001f;
    code0[i++] = 0x54000042; code0[i++] = 0x91000718; code0[i++] = 0x889ffec0;
    code0[i++] = 0x7100001f | ((uint32_t)4000 << 10) | (1u << 5);
    code0[i] = 0x54000003 | ((uint32_t)((int32_t)(loop - i) & 0x7ffff) << 5); i++;
    uint64_t stop = (uint64_t)(uintptr_t)&code0[i]; code0[i++] = 0xd503201f;
    /* reader: ldar w1,[x19]; isb; blr x23; cmp w0,w1; b.hs 1f; add x24,x24,#1 (stale);
       1: stlr w0,[x22]; loop until w0 >= 4000.
       writer: x2 = k, x23 = F, x19 = flag, x20 = acks, x21 = readers, x22 = rounds, w4 = 0xd2800000 */
    uint32_t *w = wcode; int n = 0;
    uint32_t wl = n;
    w[n++] = 0x11000442;            /* add w2, w2, #1 */
    w[n++] = 0x531b6843;            /* lsl w3, w2, #5 */
    w[n++] = 0x2a040063;            /* orr w3, w3, w4  (w4 = 0xd2800000) */
    w[n++] = 0xb90082e3;            /* str w3, [x23, #128]: a dummy store, refills a writable entry */
    w[n++] = 0xb90082e3;            /* again: now on the fast path */
    w[n++] = 0xb90002e3;            /* str w3, [x23]: the real one, fast path */
    w[n++] = 0x889ffe62;            /* stlr w2, [x19] */
    w[n++] = 0xaa1403e5;            /* mov x5, x20 (ack ptr) */
    w[n++] = 0xaa1503e7;            /* mov x7, x21 (count) */
    uint32_t wt = n;
    w[n++] = 0x88dffca6;            /* ldar w6, [x5] */
    w[n++] = 0x6b0200df;            /* cmp w6, w2 */
    w[n] = 0x54000003 | ((uint32_t)((int32_t)(wt - n) & 0x7ffff) << 5); n++; /* b.lo wt */
    w[n++] = 0x910010a5;            /* add x5, x5, #4 */
    w[n++] = 0xf10004e7;            /* subs x7, x7, #1 */
    w[n] = 0x54000001 | ((uint32_t)((int32_t)(wt - n) & 0x7ffff) << 5); n++; /* b.ne wt */
    w[n++] = 0x6b16005f;            /* cmp w2, w22 */
    w[n] = 0x54000003 | ((uint32_t)((int32_t)(wl - n) & 0x7ffff) << 5); n++; /* b.lo wl */
    w[n++] = 0xd503201f;
    uint64_t wstop = (uint64_t)(uintptr_t)&w[n - 1];
    m.stop_pc = stop; m.stop_pc2 = wstop;
    uint8_t *stack = aligned_alloc(PG, 16 * PG);
    hv_machine_add_ram(&m, (uint64_t)(uintptr_t)stack, 16 * PG, stack, 0);
    int ncpu = nread + 1;
    HvCpu **c = calloc(ncpu, sizeof *c); pthread_t *t = calloc(ncpu, sizeof *t);
    for (int k = 0; k < ncpu; k++) {
        c[k] = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
        hv_cpu_reset_index(c[k], &m, k);
        c[k]->cpacr_el1 = 3u << 20; c[k]->el = 0; c[k]->spsel = 0;
        c[k]->sp = (uint64_t)(uintptr_t)stack + (k + 1) * PG - 64;
        c[k]->x[23] = (uint64_t)(uintptr_t)fpage; c[k]->x[19] = (uint64_t)(uintptr_t)flag;
        if (k < nread) { c[k]->x[22] = (uint64_t)(uintptr_t)(ack + k); c[k]->pc = (uint64_t)(uintptr_t)code0; }
        else { c[k]->x[4] = 0xd2800000; c[k]->x[20] = (uint64_t)(uintptr_t)ack; c[k]->x[21] = nread; c[k]->x[22] = rounds; c[k]->pc = (uint64_t)(uintptr_t)wcode; }
    }
    for (int k = 0; k < ncpu; k++) pthread_create(&t[k], NULL, run, c[k]);
    uint32_t last = 0; uint64_t lastt = mach_absolute_time();
    for (;;) {
        usleep(1000);
        if (big) for (int k = 0; k < ncpu; k++) hv_cpu_kick(c[k]); /* so each yields the lock */
        uint32_t f = *flag;
        if (f != last) { last = f; lastt = mach_absolute_time(); }
        if (f >= (uint32_t)rounds) { int ok = 1; for (int k = 0; k < nread; k++) if (ack[k] < (uint32_t)rounds) ok = 0; if (ok) break; }
        if (mach_absolute_time() - lastt > 24000ull * 2000) {
            printf("FAIL: stale code ran, stuck at round %u: acks", f); for (int k = 0; k < nread; k++) printf(" %u", ack[k]); printf("  F=%08x\n", fpage[0]);
            _exit(1);
        }
    }
    /* end readers */
    fpage[0] = 0xd2800000 | (4000u << 5); hv_dma_written(&m, (uint64_t)(uintptr_t)fpage, 4); *flag = 4000;
    for (int k = 0; k < ncpu; k++) pthread_join(t[k], NULL);
    uint64_t stale = 0; for (int k = 0; k < nread; k++) stale += c[k]->x[24];
    if (stale) { printf("FAIL: %llu stale calls\n", (unsigned long long)stale); return 1; }
    printf("ok%s: %d rounds, stale %llu, protects %llu, waits %llu, translations %llu\n", big ? " (big lock)" : "", rounds, (unsigned long long)stale,
           (unsigned long long)HV_STAT_TOTAL(&m, code_protects), (unsigned long long)HV_STAT_TOTAL(&m, code_protect_waits), (unsigned long long)HV_STAT_TOTAL(&m, translations));
    return 0;
}
