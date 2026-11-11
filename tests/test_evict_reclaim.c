// Emptying a code-cache zone in the background (core/dispatch.c, the reclaimer; its protocol is written out there),
// one step at a time on this thread (reclaim_manual), with the CPU run between steps, so each moment of it is looked
// at: 1. a lookup never returns a block of a zone being emptied, nor does an indirect branch's own (ijc_resolve); 2. no exit is chained from or into one; 3. a write to
// its code still gates its blocks during the grace period, and the hash phase leaves alone a slot a translator has
// taken but not written yet; 4. a CPU parked through the grace period answers as it
// unparks, its front cache without the zone's blocks and its return tables under new salts; 5. the zone emptied
// ahead is filled with nothing emptied in the stop, and a CPU parked through the whole job, which answers only after
// that, finds none of the old blocks; 6. a stop that needs a zone with none ready finishes the
// reclaimer's from where it got; 7. a full flush gives the job up, and its steps do nothing to the next job; 8. a
// code-store save keeps nothing of a zone being emptied; 9. a trim's zone goes back only once the reclaimer has
// emptied it, and the zone ready to fill is never the one retired; 11. a trim during a job's hash phase empties
// another zone and fills it: the slots it holds since are looked at no further than those it held as it began to be
// filled (none), while every other zone's exits into the zone being emptied are still cut; a full flush as the
// zone's tables go back gives the job up at once, the rest of the trim done by the stopper's give-back.
// Emptied code is zeroed (HV_JIT_POISON).
// NOSPARE=1 (HV_EVICT_SYNC): every zone emptied in the stop that needs it, as before: 10.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "codestore.h"
#include "dispatch.h"
#include "machine.h"
#include "evict_lists.h"

#define PAGE (1u << HV_RAM_PAGE_BITS)
#define BASE 0x80000000ull
#define A BASE                 /* page 0: add #1; b A2 (an exit on its own page) */
#define A2 (BASE + 0x100)      /*         add #16; b STOP */
#define B (BASE + PAGE)        /* page 1: add #2; b STOP */
#define C (BASE + 2 * PAGE)    /* page 2: add #4; b C2 */
#define C2 (BASE + 2 * PAGE + 0x100)
#define D (BASE + 3 * PAGE)    /* page 3: add #8; b STOP */
#define STOP (BASE + 4 * PAGE) /* never translated: the dispatcher stops there */
#define E (BASE + 5 * PAGE)    /* page 5: adr x23, T; br x23 (an indirect branch) */
#define T (E + 0x100)          /*         add #4; b STOP */
#define F (E + 0x200)          /*         add #32; b A (an exit into another zone's code) */
#define G (E + 0x300)          /*         add #64; b A */
#define NPAGES 6
#define JIT_BYTES (8u << 20)

enum { RC_HASH, RC_WAIT, RC_LISTS, RC_POISON, RC_DROP, RC_DONE }; /* as core/dispatch.c */

static HvMachine m, m2;
static HvCpu cpu, cpu2;
static uint32_t *ram, *ram2;
static int failures;

static void check(int ok, const char *what) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static uint32_t branch(uint64_t from, uint64_t to) { return 0x14000000u | ((uint32_t)((int64_t)(to - from) / 4) & 0x03ffffff); }

static uint32_t *guest_ram(void) {
    uint32_t *r = aligned_alloc(PAGE, (size_t)NPAGES * PAGE);
    if (!r) exit(1);
    for (size_t i = 0; i < (size_t)NPAGES * PAGE / 4; i++) r[i] = 0xd503201f; /* nop */
#define AT(va) (r + ((va) - BASE) / 4)
    AT(A)[0] = 0x91000400, AT(A)[1] = branch(A + 4, A2);
    AT(A2)[0] = 0x91004000, AT(A2)[1] = branch(A2 + 4, STOP);
    AT(B)[0] = 0x91000800, AT(B)[1] = branch(B + 4, STOP);
    AT(C)[0] = 0x91001000, AT(C)[1] = branch(C + 4, C2);
    AT(C2)[0] = 0x91004000, AT(C2)[1] = branch(C2 + 4, STOP);
    AT(D)[0] = 0x91002000, AT(D)[1] = branch(D + 4, STOP);
    AT(E)[0] = 0x10000817, AT(E)[1] = 0xd61f02e0; /* adr x23, .+0x100; br x23 */
    AT(T)[0] = 0x91001000, AT(T)[1] = branch(T + 4, STOP);
    AT(F)[0] = 0x91008000, AT(F)[1] = branch(F + 4, A);
    AT(G)[0] = 0x91010000, AT(G)[1] = branch(G + 4, A);
#undef AT
    return r;
}

static void machine(HvMachine *mm, HvCpu *c, uint32_t **r) {
    memset(mm, 0, sizeof *mm);
    if (hv_machine_init(mm, JIT_BYTES) != 0) exit(1);
    *r = guest_ram();
    if (hv_machine_add_ram(mm, BASE, (uint64_t)NPAGES * PAGE, *r, 0) != 0) exit(1);
    if (getenv("NOSPARE") && atoi(getenv("NOSPARE")) > 0) mm->reclaim_off = 1;
    mm->reclaim_manual = 1; /* no reclaimer thread: hv_jit_reclaim_step does its work */
    mm->chain = 1;
    hv_machine_set_zones(mm, 4);
    mm->gen_checks = 1; /* emptied code zeroed, and a link to code that isn't live reported */
    mm->stats_interval = 0;
    mm->stop_pc = STOP;
    hv_cpu_reset(c, mm);
    c->el = 0, c->spsel = 0, c->cpacr_el1 = 3u << 20;
}

/* Run from pc to STOP on c, which keeps what it remembers (front cache, branch caches, return tables). */
static uint64_t run_on(HvCpu *c, uint64_t pc) {
    c->pc = pc, c->x[0] = 0;
    int reason = hv_run(c);
    if (reason != HV_EXIT_HALT || c->pc != STOP) {
        printf("run from %llx stopped with %d at %llx\n", (unsigned long long)pc, reason, (unsigned long long)c->pc);
        failures++;
    }
    return c->x[0];
}
static uint64_t run(uint64_t pc) { return run_on(&cpu, pc); }

static HvBlock *block_in(HvMachine *mm, uint64_t va) {
    HvRam *r = &mm->ram[0];
    return hv_block_find(mm, hv_block_hash(va, va, 0), va, va, 0, r->page_gen[(va - r->pa) >> HV_RAM_PAGE_BITS]);
}
static HvBlock *block(uint64_t va) { return block_in(&m, va); }

static int zone_of(const HvBlock *b) { return (int)((size_t)(b - m.blocks) / m.zone_blocks); }

/* CPU 0's front-cache entries for blocks of zone z. */
static int jc_in_zone(int z) {
    int n = 0;
    for (uint32_t j = 0; j < 1u << HV_JC_BITS; j++) n += cpu.pc_state->jc[j].b && zone_of(cpu.pc_state->jc[j].b) == z;
    return n;
}

/* The exit of zone z's slots chained to code. */
static uint64_t *slot_to(int z, const uint32_t *code) {
    for (uint32_t s = (uint32_t)z * m.zone_slots; s < (uint32_t)(z + 1) * m.zone_slots; s++)
        if (m.slots[s] == (uint64_t)(uintptr_t)code) return &m.slots[s];
    return NULL;
}

/* The job's steps until it is at phase or over. */
static void step_to(uint32_t job, int phase) {
    for (int i = 0; i < 100000 && m.rc_zone >= 0 && m.rc_phase != phase; i++) hv_jit_reclaim_step(&m, job);
}
static int finish(uint32_t job) {
    for (int i = 0; i < 100000; i++)
        if (hv_jit_reclaim_step(&m, job)) return 1;
    return 0;
}

#define ST(f) HV_STAT_TOTAL(&m, f)

/* Pages of zone z's share of the slots table the process holds (mincore), of those wholly inside it. */
static size_t slots_resident(int z) {
    uintptr_t page = (uintptr_t)getpagesize();
    uintptr_t lo = ((uintptr_t)(m.slots + (size_t)z * m.zone_slots) + page - 1) / page * page;
    uintptr_t hi = (uintptr_t)(m.slots + (size_t)(z + 1) * m.zone_slots) / page * page;
    if (hi <= lo) return 0;
    size_t n = (hi - lo) / page, held = 0;
    char *vec = malloc(n);
    if (!vec || mincore((void *)lo, hi - lo, vec) != 0) {
        free(vec);
        return (size_t)-1;
    }
    for (size_t i = 0; i < n; i++) held += vec[i] & 1;
    free(vec);
    return held;
}

/* 10. HV_EVICT_SYNC: nothing marked ahead, every zone emptied in the stop that needs one, a trim's at once. */
static void sync_only(void) {
    check(run(A) == 17, "A runs in zone 0");
    hv_jit_make_room(&m, -1);
    check(run(B) == 2, "B in zone 1");
    hv_jit_make_room(&m, -1);
    check(run(C) == 20, "C in zone 2");
    hv_jit_make_room(&m, -1);
    check(run(D) == 8 && m.zone == 3, "D in zone 3");
    uint64_t sync = ST(evict_sync);
    hv_jit_make_room(&m, -1); /* every zone holds code: one emptied here */
    check(ST(evict_sync) == sync + 1 && ST(reclaims) == 0 && ST(reclaim_marks) == 0 && m.rc_zone < 0 && m.zones_dying == 0,
          "NOSPARE: the zone was emptied in the stop, none by the reclaimer");
    check(run(A) == 17 && run(C) == 20, "the code runs on");
    uint32_t trims = m.trims;
    hv_machine_request_trim(&m, 0);
    hv_jit_make_room(&m, -1);
    check(m.trims == trims + 1 && m.zones_retired && !m.giveback_left, "NOSPARE: a trim's zone retired and given back at once");
    check(check_lists(&m, "sync") == 0, "the hash and the page lists hold together");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("HV_JIT_POISON", "1", 1);
    machine(&m, &cpu, &ram);
    if (m.reclaim_off) {
        sync_only();
        hv_machine_free(&m);
        printf("%s\n", failures ? "FAILED" : "passed");
        return failures != 0;
    }

    /* Zone 0: A, A2 and D; zone 1: B; zone 2 (being filled): C, C2. */
    check(run(A) == 17 && run(D) == 8, "A and D run");
    check(run(A) == 17 && run(D) == 8, "  and again (found in the hash: into the front cache)");
    check(run(E) == 4 && run(E) == 4 && run(T) == 4, "E branches to T through a register (and T into the front cache)");
    hv_jit_make_room(&m, -1);
    check(run(B) == 2, "B runs");
    hv_jit_make_room(&m, -1);
    check(run(C) == 20, "C runs");
    HvBlock *a = block(A), *a2 = block(A2), *c = block(C), *c2 = block(C2), *d = block(D), *tt = block(T);
    check(a && a2 && d && zone_of(a) == 0 && zone_of(d) == 0 && tt && zone_of(tt) == 0 && c && c2 && zone_of(c) == 2 && m.zone == 2,
          "A, D and T are in zone 0, C in zone 2, which is being filled");
    uint64_t *a_exit = slot_to(0, a2->code), *c_exit = slot_to(2, c2->code);
    check(a_exit && c_exit, "A's exit is chained to A2, C's to C2");
    check(jc_in_zone(0) > 0, "the CPU's front cache holds blocks of zone 0");

    /* Zone 0 marked, as a stop does */
    uint32_t gen0 = m.jit_gen;
    hv_jit_reclaim_start(&m, 0);
    uint32_t job = m.rc_job;
    check(m.rc_zone == 0 && m.rc_phase == RC_HASH && (m.zones_dying & 1) && m.jit_gen == gen0, "zone 0 marked: dying, jit_gen as it was");

    /* 2. No exit chained from or into it (the dispatcher's chain, as after a block left through the exit). */
    uint64_t was_a = *a_exit, was_c = *c_exit;
    hv_jit_chain(&m, c_exit, a);
    check(*c_exit == was_c, "an exit of a live zone is not chained into a block of the dying one");
    hv_jit_chain(&m, a_exit, c);
    check(*a_exit == was_a, "an exit of the dying zone is not chained to anything");

    /* 1. A lookup does not find A's block, though it is still in the hash: A is made again, in zone 2. */
    uint64_t t = ST(translations);
    check(run(A) == 17, "A runs, its zone marked");
    HvBlock *an = block(A);
    check(ST(translations) >= t + 2 && an && an != a && zone_of(an) == 2, "A and A2 were made again in zone 2, the old ones not used");
    /* Nor by an indirect branch's lookup (its branch cache emptied by the mark: ijc_resolve, which looks in the front
       cache first, where T's old block still is) */
    t = ST(translations);
    check(run(E) == 4, "E runs, its branch to T");
    HvBlock *tn = block(T);
    check(tn && tn != tt && zone_of(tn) == 2 && ST(translations) >= t + 2, "E and T were made again in zone 2, T's old block not used");

    /* 3. The hash phase: zone 0's blocks out of the hash, the exits into its code cut; still on their pages' lists.
       Meanwhile a translator in zone 2 has taken a slot (put_branch_exit's fetch_add) and not written it yet: it still
       holds what it held before zone 2 was last emptied, here an address in zone 0's code. Not looked at: cutting it
       would patch its stale site, where new code may now be. */
    uint32_t taken = __atomic_fetch_add(&m.nslots, 1, __ATOMIC_RELAXED);
    uint64_t stale = (uint64_t)(uintptr_t)d->code;
    check(taken / m.zone_slots == 2 && m.slots[taken] == 0, "a slot of zone 2 taken after the mark");
    m.slots[taken] = stale; /* as left from before */
    step_to(job, RC_WAIT);
    check(m.slots[taken] == stale, "the hash phase left the slot taken since the mark alone");
    m.slots[taken] = 0; /* its translator writes it */
    check(m.rc_phase == RC_WAIT && m.rc_zone == 0, "the hash phase is done; the grace period next");
    uint32_t dw = 0x91002000;
    hv_phys_write(&m, D, &dw, 4); /* the same instruction: D's page's blocks retired */
    check(d->dead, "a write to D's page during the grace period still gates D's old block");

    /* 4. CPU 0 is parked: the grace period passes it, and it answers as it unparks */
    uint64_t salt0 = cpu.ret_salt[0], salt1 = cpu.ret_salt[1];
    check(jc_in_zone(0) > 0, "before it answers, its front cache still holds zone 0's blocks");
    hv_jit_reclaim_step(&m, job);
    check(m.rc_phase == RC_LISTS && cpu.pc_state->reclaim_ack < m.reclaim_seq, "the grace period is over at once, the CPU parked, its answer not given yet");
    check(run(B) == 2, "B runs");
    check(cpu.pc_state->reclaim_ack == m.reclaim_seq, "the CPU answered as it ran again");
    check(jc_in_zone(0) == 0, "its front cache holds no block of zone 0");
    check(cpu.ret_salt[0] >> 48 == (salt0 >> 48) + 1 && cpu.ret_salt[1] >> 48 == (salt1 >> 48) + 1,
          "its return tables have new salts (both halves)");
    salt0 = cpu.ret_salt[0];
    run(B);
    check(cpu.ret_salt[0] == salt0, "  (a run with nothing to answer leaves them)");

    /* 5. The rest: lists, poison; zone 0 ready to fill, still dying, jit_gen as it was */
    check(finish(job), "the job finished");
    check(m.rc_zone < 0 && m.zone_ready == 0 && (m.zones_dying & 1) && ST(reclaims) == 1 && m.zones[0].nblocks == 0 &&
              m.zones[0].used == 0 && m.jit_gen == gen0,
          "zone 0 is empty and ready to fill, still dying, jit_gen not bumped");
    check(check_lists(&m, "zone 0 reclaimed") == 0, "the hash and the page lists hold together");
    uint64_t sync = ST(evict_sync), fills = ST(zone_evictions), quick = ST(quick_stops);
    hv_jit_make_room(&m, -1);
    check(m.zone == 0 && m.zone_ready < 0 && !(m.zones_dying & 1) && m.jit_gen == gen0 + 1 && ST(evict_sync) == sync &&
              ST(zone_evictions) == fills && ST(quick_stops) == quick + 1,
          "the full cache filled zone 0 at once: nothing emptied in the stop");
    check(run(A) == 17 && run(D) == 8 && run(C) == 20 && run(B) == 2, "the code runs on");
    check(check_lists(&m, "zone 0 filled again") == 0, "the hash and the page lists hold together");

    /* 5b. CPU 0 parked through a whole job (zone 2: C), and the zone filled again before it runs */
    check(run(C) == 20 && jc_in_zone(2) > 0, "C runs; the front cache holds blocks of zone 2");
    hv_jit_reclaim_start(&m, 2);
    check(finish(m.rc_job) && m.zone_ready == 2, "zone 2 emptied with the CPU parked throughout");
    hv_jit_make_room(&m, -1);
    check(m.zone == 2 && !(m.zones_dying & 4) && jc_in_zone(2) > 0, "zone 2 filled again: no longer dying, the CPU not answered yet");
    t = ST(translations);
    check(run(C) == 20 && ST(translations) > t && block(C) && zone_of(block(C)) == 2, "C runs: made again, the old block not used");

    /* 6. With none ready, the stop finishes the reclaimer's zone from where it got (zone 1, its lists half done) */
    hv_jit_reclaim_start(&m, 1);
    job = m.rc_job;
    step_to(job, RC_LISTS);
    uint64_t aborts = ST(reclaim_aborts);
    sync = ST(evict_sync);
    hv_jit_make_room(&m, -1);
    check(m.zone == 1 && m.rc_zone < 0 && !(m.zones_dying & 2) && ST(evict_sync) == sync + 1 && ST(reclaim_aborts) == aborts + 1,
          "no zone ready: the stop emptied the reclaimer's zone itself and gave its job up");
    check(hv_jit_reclaim_step(&m, job) == 1, "  the job's next step does nothing");
    check(check_lists(&m, "zone 1 finished in a stop") == 0, "the hash and the page lists hold together");
    check(run(B) == 2 && run(A) == 17, "the code runs on");

    /* 7. A full flush gives a job up during its grace period; its steps then leave the next job alone */
    hv_jit_reclaim_start(&m, 0);
    uint32_t job1 = m.rc_job;
    step_to(job1, RC_WAIT);
    __atomic_store_n(&cpu.pc_state->active, 1, __ATOMIC_SEQ_CST); /* as if running and not answering */
    check(hv_jit_reclaim_step(&m, job1) == 0 && m.rc_phase == RC_WAIT, "the grace period waits for a running CPU");
    hv_flush_blocks(&m);
    __atomic_store_n(&cpu.pc_state->active, 0, __ATOMIC_SEQ_CST);
    check(m.rc_zone < 0 && m.zones_dying == 0 && m.zone_ready < 0, "the flush gave the job up");
    run(A);
    hv_jit_make_room(&m, -1);
    run(B);
    hv_jit_make_room(&m, -1);
    hv_jit_reclaim_start(&m, 0);
    uint32_t job2 = m.rc_job;
    check(job2 != job1 && m.rc_zone == 0 && m.rc_phase == RC_HASH, "a new job on zone 0");
    check(hv_jit_reclaim_step(&m, job1) == 1 && m.rc_phase == RC_HASH && m.rc_zone == 0, "the old job's step did nothing to it");
    check(finish(job2) && m.zone_ready == 0, "the new job finished");

    /* 8. A code-store save with a zone being emptied (zone 1, in its grace period) and one ready (none: 0 filled) */
    hv_jit_make_room(&m, -1); /* zone 0, ready */
    check(m.zone == 0 && run(C) == 20 && run(D) == 8, "C and D run in zone 0");
    hv_jit_reclaim_start(&m, 1);
    job = m.rc_job;
    step_to(job, RC_WAIT);
    char path[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/hv_evict_reclaim_test_%d.hvsnap", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    hv_code_store_remove(path);
    HvCodeKey k;
    hv_code_key_new(&k);
    check(hv_code_store_save(&m, path, &k, NULL, 0) == 0, "saved, zone 1 being emptied");
    machine(&m2, &cpu2, &ram2);
    long n = hv_code_store_load(&m2, path, &k);
    hv_code_store_remove(path);
    check(n > 0 && block_in(&m2, C) && block_in(&m2, D) && !block_in(&m2, B) && m2.zones[1].nblocks == 0 && m2.zones_dying == 0,
          "loaded: zone 0's blocks, nothing of zone 1");
    uint64_t t2 = HV_STAT_TOTAL(&m2, translations);
    check(run_on(&cpu2, C) == 20 && run_on(&cpu2, D) == 8 && HV_STAT_TOTAL(&m2, translations) == t2, "the loaded code runs");
    check(run_on(&cpu2, B) == 2, "B, not loaded, is made again");
    check(check_lists(&m2, "loaded") == 0, "the loaded machine's hash and page lists hold together");
    hv_machine_free(&m2);
    check(finish(job) && m.zone_ready == 1, "the save left the job to finish");

    /* 9. Trims. a: the ready zone is never retired; the zone retired is marked for the reclaimer, and the trim is
       done only once it has emptied it and given its tables back */
    run(A);
    uint32_t trims = m.trims;
    hv_machine_request_trim(&m, 0);
    hv_jit_make_room(&m, -1); /* filling zone 0: retires zone 2 (A, B), fills zone 1 (ready) */
    int w = m.rc_zone;
    check(m.zone == 1 && w >= 0 && w != 1 && m.rc_retire && (m.zones_retired >> w & 1) && !(m.zones_retired & 2),
          "the trim marked a zone to retire, not the ready one, which was filled");
    check(m.trims == trims && m.giveback_left > 0, "  the trim is not done yet");
    check(finish(m.rc_job), "the reclaimer emptied it");
    check(m.trims == trims + 1 && !m.giveback_left && m.zone_ready < 0 && m.zones[w].nblocks == 0, "  then the trim was done, nothing left ready");
    check(run(B) == 2 && run(A) == 17, "the code runs on");
    /* b: a trim during a job: the job's zone retired once empty, never made ready */
    hv_jit_reclaim_start(&m, 0);
    job = m.rc_job;
    step_to(job, RC_LISTS);
    trims = m.trims;
    m.trim_quiet_ns = ~0ull; /* no retired zone filled again here */
    hv_machine_request_trim(&m, 0);
    hv_jit_make_room(&m, -1);
    check(m.rc_zone == 0 && m.rc_retire && (m.zones_retired & 1) && m.trims == trims, "a trim during a job retires its zone, once empty");
    check(finish(job) && m.zone_ready < 0 && m.trims == trims + 1 && !m.giveback_left, "  the reclaimer emptied it; the trim done");
    check(check_lists(&m, "after the trims") == 0, "the hash and the page lists hold together");
    check(run(A) == 17 && run(B) == 2 && run(C) == 20 && run(D) == 8, "the code runs on");
    hv_machine_free(&m);

    /* 11. A new machine. Zone 0 (w: A, A2) marked while zone 3 (D) is being filled; zones 1 and 2 hold B and C, and
       F and G, whose exits are chained into w, past their zones' first slots. A memory warning at the next full
       stop retires w (the job's, once empty: retire_one) and so empties another zone, Y, in the stop
       (pick_victim) and fills it, the hash phase still to run. A slot of Y taken since and not yet written still
       holds an address in w's code, and a site in Y's new code: neither is touched. The exit of the third zone, X,
       into w is still cut */
    free(ram);
    machine(&m, &cpu, &ram);
    check(run(A) == 17, "A runs in zone 0");
    hv_jit_make_room(&m, -1);
    check(run(B) == 2 && run(F) == 49 && run(F) == 49, "B and F in zone 1, F's exit chained to A");
    hv_jit_make_room(&m, -1);
    check(run(C) == 20 && run(G) == 81 && run(G) == 81, "C and G in zone 2, G's exit chained to A");
    hv_jit_make_room(&m, -1);
    check(run(D) == 8 && m.zone == 3, "D in zone 3, being filled");
    a = block(A);
    uint64_t *f_exit = slot_to(1, a ? a->code : NULL), *g_exit = slot_to(2, a ? a->code : NULL);
    check(a && zone_of(a) == 0 && f_exit && g_exit && (uint32_t)(f_exit - m.slots) % m.zone_slots > 0 &&
              (uint32_t)(g_exit - m.slots) % m.zone_slots > 0,
          "A in zone 0; the exits into it from zones 1 and 2 past their first slots");
    hv_jit_reclaim_start(&m, 0);
    job = m.rc_job;
    uint32_t at_mark[4];
    memcpy(at_mark, m.rc_slots_end, sizeof at_mark);
    sync = ST(evict_sync);
    trims = m.trims;
    m.trim_quiet_ns = ~0ull; /* no retired zone filled again here */
    hv_machine_request_trim(&m, 0);
    hv_jit_make_room(&m, -1);
    int y = m.zone, x = 3 - y;
    check((y == 1 || y == 2) && m.rc_zone == 0 && m.rc_retire && m.rc_phase == RC_HASH && (m.zones_retired & 1) &&
              ST(evict_sync) == sync + 1 && m.trims == trims,
          "the trim retired zone 0, its job's hash phase still to run; zone 1 or 2 emptied in the stop and being filled");
    uint64_t *x_exit = x == 1 ? f_exit : g_exit;
    check(*x_exit == (uint64_t)(uintptr_t)a->code, "the other one's exit into zone 0 still chained");
    check(run(T) == 4 && block(T) && zone_of(block(T)) == y, "T made in it");
    taken = __atomic_fetch_add(&m.nslots, 1, __ATOMIC_RELAXED);
    check(taken / m.zone_slots == (uint32_t)y && taken % m.zone_slots < at_mark[y],
          "a slot of it taken, within its count at the mark (the one before it was emptied)");
    uint32_t *site = block(T)->code, word = *site, *site_was = m.slot_site[taken];
    stale = (uint64_t)(uintptr_t)a->code;
    m.slots[taken] = stale, m.slot_site[taken] = site; /* as left from before: into zone 0, its site now T's code */
    step_to(job, RC_WAIT);
    check(m.slots[taken] == stale && *site == word, "the hash phase left the slot, and the code at its site, alone");
    m.slots[taken] = 0, m.slot_site[taken] = site_was; /* its translator writes them */
    check(*x_exit == 0, "it cut the exit of the third zone into zone 0");
    /* A full flush as the job gives the zone's tables back (in pieces under tr_mu), one piece given back already:
       the job given up at once, never waited for; the rest of the trim's part handed to the stopper's give-back,
       which gives the zone's tables back (its slots, which the piece did not reach, no longer held) */
    step_to(job, RC_DROP);
    check(m.rc_zone == 0 && m.rc_phase == RC_DROP && m.trims == trims, "zone 0 emptied; its tables next");
    check(hv_machine_drop_zone_tables_part(&m, 0, &m.rc_drop_k, &m.rc_drop_at, &m.rc_drop_freed, 0) == 0 &&
              (m.rc_drop_k > 0 || m.rc_drop_at > 0) && m.rc_drop_k < 2,
          "one piece of its tables given back, its slots not reached");
    size_t held = slots_resident(0);
    check(held > 0 && held != (size_t)-1, "  zone 0's slots still held");
    pthread_mutex_lock(&m.tr_mu); /* the flush's first step, in its stop (flush_blocks) */
    hv_jit_reclaim_cancel(&m);
    pthread_mutex_unlock(&m.tr_mu);
    check(m.rc_zone < 0 && hv_jit_reclaim_step(&m, job) == 1 && (m.zones_retired & 1) && (m.giveback_zones & 1) &&
              m.giveback_pending && m.trims == trims,
          "the flush gives the job up; zone 0 still retired, its tables handed to the stopper's give-back");
    hv_flush_blocks(&m); /* the rest, then the stopper's give-back */
    check(m.trims == trims + 1 && !m.giveback_left && !m.giveback_zones, "  which gave them back and finished the trim");
    check(slots_resident(0) == 0, "  zone 0's slots no longer held");
    check(check_lists(&m, "a trim during the hash phase") == 0, "the hash and the page lists hold together");
    check(run(A) == 17 && run(F) == 49 && run(G) == 81 && run(T) == 4, "the code runs on");

    hv_machine_free(&m);
    printf("%s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
