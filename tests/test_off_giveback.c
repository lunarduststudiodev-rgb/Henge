/* A CPU that stays off gives back what it holds for running, and only then.

   macOS parks the CPUs it doesn't need with PSCI CPU_OFF, some for minutes. Such a CPU kept its TLB (2 MB), its
   return and ASID tables and its branch caches: up to ~9 MB each, for nothing until a CPU_ON, whose reset zeroes
   them anyway. A reset doesn't give memory back (it would fault it straight back in on the run that follows); a
   CPU that has stayed off HV_OFF_GIVEBACK_MS does (wait_while_off), on its own thread.

   On the real path: CPU 1 runs a guest HVC CPU_OFF in hv_run; CPU_ON comes through CPU 0's HVC.
   - while the machine is paused (a save may be reading it), nothing goes back, however long;
   - once resumed, past the threshold: its TLB, return and ASID tables, fill logs and branch caches, with their
     stashes, (user id, chunk) set, jc and filters, hold no page, the footprint drops, and its TLB reads not ready; once only, and code flushes meanwhile bring none back;
   - meanwhile a TLB refill for it (translated code's light path) leaves its TLB to the helper, untouched;
   - CPU_ON then starts it normally: its TLB made ready as it comes out of its wait (unpark), before the dispatcher
     does anything for its run (here a stop hook at its entry, which runs before the first fetch), and its code runs. */
#include <mach/mach.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dispatch.h"
#include "exception.h"
#include "machine.h"
#include "mmu.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static HvMachine m;
static HvCpu cpus[2];

static uint64_t footprint(void) {
    task_vm_info_data_t vi;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n);
    return vi.phys_footprint;
}

/* Whole pages in [p, p + len) the process holds: in memory, or compressed or swapped out (a busy Mac compresses an
   off CPU's tables while it waits) */
static size_t resident(const void *p, size_t len) {
    uintptr_t page = (uintptr_t)getpagesize(), lo = ((uintptr_t)p + page - 1) & ~(page - 1), hi = ((uintptr_t)p + len) & ~(page - 1);
    if (hi <= lo) return 0;
    char vec[4096];
    size_t n = (hi - lo) / page, got = 0;
    if (n > sizeof vec || mincore((void *)lo, hi - lo, vec) != 0) return 0;
    for (size_t i = 0; i < n; i++) got += vec[i] & (MINCORE_INCORE | MINCORE_PAGED_OUT) ? 1 : 0;
    return got;
}

static size_t asid_pages(HvCpu *c) {
    return resident(c->asid_cid, sizeof c->asid_cid) + resident(c->cid_asid, sizeof c->cid_asid) +
           resident(c->asid_base, sizeof c->asid_base);
}

/* What it holds for its caches besides the tables: the first stash's entries (user TLB entries kept across process
   switches), the (user id, 32 MB chunk) set, jc and the filters */
static size_t cache_pages(HvPerCpu *p) {
    return resident(p->stash[0].ent, HV_STASH_CAP * sizeof *p->stash[0].ent) + resident(p->cid_chunks, 65536 * sizeof *p->cid_chunks) +
           resident(p->jc, sizeof p->jc) + resident(p->ijc_keyed, sizeof p->ijc_keyed) + resident(p->ijc_hint, sizeof p->ijc_hint) +
           resident(p->ijc_pages, sizeof p->ijc_pages);
}

static uint64_t now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000; }

static void *run_cpu(void *arg) { return (void *)(intptr_t)hv_run(arg); }

/* The dispatcher at CPU 1's CPU_ON entry, before it fetches anything: was its TLB ready (-1: never got there) */
static int entry_ready = -1;
static void at_entry(HvCpu *cpu) {
    if (entry_ready < 0) __atomic_store_n(&entry_ready, hv_tlb_is_ready(cpu), __ATOMIC_RELEASE);
}

int main(void) {
    alarm(30);
    enum { THRESHOLD_MS = 200 };
    uint32_t *code = aligned_alloc(16384, 16384);
    if (!code || hv_machine_init(&m, 4 << 20)) return 1;
    memset(code, 0, 16384);
    code[0] = 0xd4000002;    /* 0x10000: hvc #0 (x0 = CPU_OFF) */
    code[0x40] = 0x91000529; /* 0x10100: add x9, x9, #1 (CPU_ON's entry) */
    code[0x41] = 0x17ffffff; /*          b . - 4 */
    hv_machine_add_ram(&m, 0x10000, 16384, code, 1);
    m.off_giveback_ms = THRESHOLD_MS;
    for (int i = 0; i < 2; i++) {
        hv_cpu_reset_index(&cpus[i], &m, i);
        cpus[i].mpidr = i;
    }
    /* CPU 0 is this thread: off and parked as far as a pause is concerned, so a pause can settle */
    cpus[0].off = 1;
    cpus[0].parked_off = 1;
    HvCpu *c = &cpus[1];
    c->pc = 0x10000;
    c->x[0] = 0x84000002;
    /* What a CPU that ran holds besides its TLB: some return-table and ASID-table pages, its fill logs */
    memset(c->ret_tab, 0, sizeof c->ret_tab / 4);
    memset(c->asid_base, 0, sizeof c->asid_base / 4);
    memset(c->asid_cid, 0, sizeof c->asid_cid / 2);
    memset(c->user_fills, 0, sizeof c->user_fills);
    /* and of its caches' memory: a process's stashed entries (ttbr0 0: no switch restores them), some chunk bits,
       and zeros through part of jc and the filters (written, so held) */
    HvPerCpu *pc = c->pc_state;
    memset(pc->stash[0].ent, 0x11, HV_STASH_CAP * sizeof *pc->stash[0].ent);
    memset(pc->cid_chunks, 0x33, 65536 * sizeof *pc->cid_chunks / 4);
    memset(pc->jc, 0, sizeof pc->jc / 4);
    memset(pc->ijc_keyed, 0, sizeof pc->ijc_keyed / 4);
    memset(pc->ijc_hint, 0, sizeof pc->ijc_hint / 4);
    memset(pc->ijc_pages, 0, sizeof pc->ijc_pages);
    pthread_t t;
    pthread_create(&t, NULL, run_cpu, c);
    for (int n = 0; n < 5000 && !__atomic_load_n(&c->parked_off, __ATOMIC_SEQ_CST); n++) usleep(1000);
    CHECK(c->parked_off && c->off, "CPU 1 reached CPU_OFF");

    /* Paused: nothing goes back, for four times the threshold */
    hv_machine_pause(&m);
    CHECK(hv_machine_pause_wait(&m, 2000) == 0, "the machine pauses with CPU 1 off");
    uint64_t foot_off = footprint();
    usleep(4 * THRESHOLD_MS * 1000);
    size_t tlb = resident(c->tlb, sizeof c->tlb), ret = resident(c->ret_tab, sizeof c->ret_tab), asid = asid_pages(c);
    size_t caches = cache_pages(pc);
    uint64_t held = (tlb + ret + asid + resident(c->user_fills, sizeof c->user_fills) + caches) * (uint64_t)getpagesize();
    printf("off and paused %d ms: TLB %zu pages, return tables %zu, ASID tables %zu, branch caches %s, stash, chunks, jc "
           "and filters %zu; %llu give-backs\n",
           4 * THRESHOLD_MS, tlb, ret, asid, c->pc_state->ijc_tables_held ? "held" : "none", caches,
           (unsigned long long)HV_STAT_TOTAL(&m, off_givebacks));
    CHECK(HV_STAT_TOTAL(&m, off_givebacks) == 0, "nothing given back while paused");
    CHECK(hv_tlb_is_ready(c) && tlb >= 100 && ret > 0 && asid > 0 && caches >= 40, "an off CPU holds its tables while paused");
    CHECK(hv_machine_pause_wait(&m, 100) == 0, "the machine stays settled while CPU 1 waits off");

    /* Resumed: given back within about a threshold */
    uint64_t t0 = now_ms();
    hv_machine_resume(&m);
    while (!HV_STAT_TOTAL(&m, off_givebacks) && now_ms() - t0 < 5000) usleep(1000);
    uint64_t took = now_ms() - t0;
    tlb = resident(c->tlb, sizeof c->tlb), ret = resident(c->ret_tab, sizeof c->ret_tab), asid = asid_pages(c);
    size_t logs = resident(c->user_fills, sizeof c->user_fills);
    caches = cache_pages(pc);
    uint64_t foot_back = footprint();
    printf("resumed: gave back %.1f MB after %llu ms; TLB %zu pages, return tables %zu, ASID tables %zu, fill logs %zu, "
           "branch caches %s, stash, chunks, jc and filters %zu; footprint %.1f -> %.1f MB\n",
           HV_STAT_TOTAL(&m, off_released) / 1048576.0, (unsigned long long)took, tlb, ret, asid, logs,
           c->pc_state->ijc_tables_held ? "held" : "none", caches, foot_off / 1048576.0, foot_back / 1048576.0);
    CHECK(HV_STAT_TOTAL(&m, off_givebacks) == 1, "a CPU off past the threshold gives back (%llu times)",
          (unsigned long long)HV_STAT_TOTAL(&m, off_givebacks));
    CHECK(!hv_tlb_is_ready(c), "its TLB reads not ready");
    CHECK(tlb <= 1 && ret == 0 && asid == 0 && logs == 0, "its TLB, return and ASID tables and fill logs hold nothing");
    CHECK(!c->pc_state->ijc_tables_held, "its branch caches hold nothing");
    CHECK(caches == 0, "its stash, chunk set, jc and filters hold nothing (%zu pages)", caches);
    CHECK((int64_t)(foot_off - foot_back) >= (int64_t)(held * 9 / 10) && HV_STAT_TOTAL(&m, off_released) >= held * 9 / 10,
          "the footprint drops by what it held (%.1f MB)", held / 1048576.0);
    usleep(3 * THRESHOLD_MS * 1000);
    CHECK(HV_STAT_TOTAL(&m, off_givebacks) == 1, "and doesn't do it again while still off");
    /* A code flush starts every CPU's branch caches over (new ids), this one's too: nothing of its comes back */
    hv_flush_blocks(&m);
    hv_flush_blocks(&m);
    /* the fill logs and ASID tables as one range (its whole pages): a new id writes near their starts */
    size_t span = resident(c->user_fills, offsetof(HvCpu, next_cid) - offsetof(HvCpu, user_fills));
    tlb = resident(c->tlb, sizeof c->tlb), ret = resident(c->ret_tab, sizeof c->ret_tab);
    caches = cache_pages(pc);
    printf("after two code flushes: TLB %zu pages, return tables %zu, fill logs and ASID tables %zu, stash, chunks, jc "
           "and filters %zu\n", tlb, ret, span, caches);
    CHECK(tlb <= 1 && ret == 0 && span == 0 && caches == 0, "code flushes bring none of them back");

    /* Translated code's light refill path, given a TLB not ready (zeros, or untouched pages), hands the access to
       the helper (which walks with hv_mmu_access, readying the TLB) and writes nothing in it */
    int refill = hv_tlb_refill(c, 0x10000, 1u << 1 | 7u << 3); /* an 8-byte EL1 read */
    tlb = resident(c->tlb, sizeof c->tlb);
    CHECK(refill == 1 && !hv_tlb_is_ready(c) && tlb <= 1, "a refill on a TLB not ready went to the helper (%d), "
          "leaving it not ready and untouched (%zu pages)", refill, tlb);

    /* CPU_ON: it runs again, its TLB made ready as it leaves its wait, before its run's first fetch */
    m.stop_pc = 0x10100;
    m.stop_hook = at_entry;
    cpus[0].x[0] = 0xc4000003;
    cpus[0].x[1] = 1;
    cpus[0].x[2] = 0x10100;
    cpus[0].x[3] = 0;
    hv_exception_insn(&cpus[0], 0xd4000002, 0x10000);
    CHECK(cpus[0].x[0] == 0, "CPU_ON succeeds (%lld)", (long long)cpus[0].x[0]);
    for (int n = 0; n < 5000 && !hv_tlb_is_ready(c); n++) usleep(1000);
    CHECK(hv_tlb_is_ready(c) && !c->off, "CPU 1 runs again, its TLB ready");
    for (int n = 0; n < 5000 && __atomic_load_n(&entry_ready, __ATOMIC_ACQUIRE) < 0; n++) usleep(1000);
    int er = __atomic_load_n(&entry_ready, __ATOMIC_ACQUIRE);
    CHECK(er == 1, "its TLB was ready when the dispatcher reached its entry, before fetching (%d)", er);
    usleep(20000);

    hv_machine_halt(&m, HV_HALT_HOST);
    hv_cpu_wake(c);
    void *r;
    pthread_join(t, &r);
    CHECK((intptr_t)r == HV_EXIT_HALT, "CPU 1 ends on the halt");
    CHECK(c->x[9] > 0 && c->pc >= 0x10100 && c->pc < 0x10108, "having run its CPU_ON entry (x9 %llu, pc %#llx)",
          (unsigned long long)c->x[9], (unsigned long long)c->pc);
    hv_machine_free(&m);
    free(code);
    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails != 0;
}
