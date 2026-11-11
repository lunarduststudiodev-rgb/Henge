// Memory the code cache gives back, and keeps given back (mem-engine #1, mem-codecache #2):
//  1. A branch-cache id running out (kernel generation or user ids) writes only the tables used, untouched ones
//     still costing nothing; it zeroes them in place, as resetting a CPU does, so their pages stay for the fills that
//     follow (no fault-ins on a resume). A trim gives them back, and no later run-out or reset writes them in again.
//  2. A memory warning's trim lasts: the zone it empties is retired, not the one filled next, so the cache's tables
//     stay smaller once the Mac goes on filling the cache. A critical one (a full flush) retires one too. Never
//     fewer than half the zones; asking for a zone count starts over.
//  3. What a trim says it gave back is what the process held, not the address range.
//  4. A warning while most zones are still empty retires the zone holding the memory, and only that one.
//  5. Once warnings have stopped (trim_quiet_ns), the cache making room fills a retired zone again rather than
//     emptying one in use, one at a time, until every zone is back; a warning before that keeps them out.
//     A table the system won't give back (a failed mmap) stays counted as holding entries, so the next run-out
//     still zeroes it, as does a clear that runs during the give-back. The (user id, page) set goes back with the
//     user tables, and stays while one of them is kept. So do the (user id, chunk) set and the filters; the
//     stashes and jc go back whatever is kept.
// Each part fails on the code before (built with -DOLD it skips what the old code lacks).
#include <dlfcn.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dispatch.h"
#include "machine.h"
#include "mmu.h"

static HvCpu cpu;
static int failures;

/* The engine's mmap is this one (linked into the same image): with mmap_refuse set, a MAP_FIXED one fails as the
   system's does when it can't make the new mapping (the old one left as it was). */
static int mmap_refuse;
static void *mmap_refuse_at; /* with mmap_refuse: only a mapping at this address, if set */
static void (*mmap_meanwhile)(void); /* run before refusing: what another thread might do while the give-back runs */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    static void *(*sys)(void *, size_t, int, int, int, off_t);
    if (mmap_refuse && (flags & MAP_FIXED) && (!mmap_refuse_at || addr == mmap_refuse_at)) {
        if (mmap_meanwhile) mmap_meanwhile();
        return MAP_FAILED;
    }
    if (!sys) sys = (void *(*)(void *, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap");
    return sys(addr, len, prot, flags, fd, off);
}

static HvMachine m;
static void clear_cpu0(void) { hv_ijc_tables_clear(&m.percpu[0], ~0u); } /* a CPU_ON's reset, a run-out */

static void check(int ok, const char *what) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static uint64_t footprint(void) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS) return 0;
    return info.phys_footprint;
}
#define MB(x) ((double)(x) / 1048576.0)

/* How much of [p, p + bytes) the process holds (whole pages, in memory or paged out, as the engine counts it). */
static uint64_t in_memory(const void *p, size_t bytes) {
    size_t page = (size_t)getpagesize(), n = (bytes + page - 1) / page;
    char *vec = malloc(n);
    uint64_t held = 0;
    if (vec && mincore(p, bytes, vec) == 0)
        for (size_t i = 0; i < n; i++) held += vec[i] & (MINCORE_INCORE | MINCORE_PAGED_OUT) ? page : 0;
    free(vec);
    return held;
}

static int all_zero(const void *p, size_t bytes) {
    const uint64_t *w = p;
    for (size_t i = 0; i < bytes / 8; i++)
        if (w[i]) return 0;
    return 1;
}

/* Zone z as translating leaves it: n blocks, one and a half exits and one FP word each, its code full. */
static void fill_zone(int z, uint32_t n) {
    hv_jit_zone_fill(&m, z);
    uint32_t b0 = (uint32_t)z * m.zone_blocks, s0 = (uint32_t)z * m.zone_slots, u0 = (uint32_t)z * m.zone_undef;
    uint32_t ns = n * 3 / 2;
    for (uint32_t i = 0; i < n; i++) {
        m.blocks[b0 + i].va = 0x1000 + i, m.blocks[b0 + i].zone = (uint8_t)z;
#ifndef OLD
        m.blocks[b0 + i].dead = 1; /* in no hash chain or page list: emptying the zone passes them by */
#endif
    }
    for (uint32_t i = 0; i < ns; i++) {
        m.slots[s0 + i] = 0;
        m.slot_site[s0 + i] = NULL, m.slot_stub[s0 + i] = (uint32_t *)8, m.slot_cond[s0 + i] = NULL;
        m.slot_xa[s0 + i] = 1, m.slot_xpg[s0 + i] = m.slot_xprev[s0 + i] = m.slot_xnext[s0 + i] = 0;
    }
    for (uint32_t i = 0; i < n; i++) m.undef_map[u0 + i] = 0xabcd;
    m.nblocks = (int)(b0 + n);
    __atomic_store_n(&m.nslots, s0 + ns, __ATOMIC_RELAXED);
    m.nundef = u0 + n;
    __atomic_store_n(&m.jit.used, (size_t)z * m.zone_words + m.zone_words - 16, __ATOMIC_RELAXED);
    hv_jit_zones_sync(&m);
}

static int active(void) {
#ifndef OLD
    int n = 0;
    for (int z = 0; z < m.nzones; z++) n += !(m.zones_retired >> z & 1);
    return n;
#else
    return m.nzones;
#endif
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&m, 256u << 20) != 0) return 1;
    /* Every zone emptied in the stop that makes room (HV_EVICT_SYNC), so a trim's zone goes back at once, as this
       counts: with the reclaimer it goes back once it is empty (test_evict_reclaim, test_evict_concurrent_mt). */
    m.reclaim_off = 1;
    hv_cpu_reset_index(&cpu, &m, 0);
    HvPerCpu *p = &m.percpu[0];

    /* 1. Run-outs. With no table used, nothing may become held: before, both halves' 8 tables were zero-filled
       (64 MB) at the first run-out of each. */
    uint64_t f0 = footprint();
    for (int i = 0; i < 70000; i++) { /* past both: the kernel generation at 0xffff, user ids at 0xffff */
        hv_ijc_reset(&cpu, 1);
        hv_ijc_reset(&cpu, 0);
    }
    uint64_t f1 = footprint();
    printf("70000 resets of both halves, no table used: footprint %.1f -> %.1f MB\n", MB(f0), MB(f1));
    check(f1 < f0 + (8u << 20), "a run-out with no table used holds no more memory");
#ifndef OLD
    /* A used kernel table, written all through as fills mark it, then a kernel run-out: it reads zero, its pages kept
       (filled again at once, they would only fault back in), and it is no longer counted as holding entries. */
    uint64_t *t1 = p->ijc + (size_t)1 * (HV_IJC_TABLE_BYTES / 8);
    memset(t1, 0x5a, HV_IJC_TABLE_BYTES);
    p->ijc_tables_used |= 1u << 1, p->ijc_tables_held |= 1u << 1;
    uint64_t f2 = footprint();
    for (int i = 0; i < 0x10000; i++) hv_ijc_reset(&cpu, 1);
    uint64_t f3 = footprint();
    printf("a used kernel table, then a kernel run-out: footprint %.1f -> %.1f MB\n", MB(f2), MB(f3));
    check(all_zero(t1, HV_IJC_TABLE_BYTES), "the used table reads empty after the run-out");
    check(f3 + (HV_IJC_TABLE_BYTES / 4) > f2 && in_memory(t1, HV_IJC_TABLE_BYTES) == HV_IJC_TABLE_BYTES,
          "zeroed in place: its pages stay, with no fault-ins to fill it again");
    check(!(p->ijc_tables_used >> 1 & 1) && (p->ijc_tables_held >> 1 & 1), "no entries in it, its memory still held");
    /* A trim gives it back. */
    uint64_t freed = hv_cpu_ijc_give_back(p);
    uint64_t f4 = footprint();
    check(freed >= HV_IJC_TABLE_BYTES * 3 / 4 && f4 + (HV_IJC_TABLE_BYTES * 3 / 4) < f3 && !p->ijc_tables_held,
          "a trim gives the held table back");
    /* Resetting the CPU (a resume, PSCI CPU_ON): the same, in place. */
    memset(t1, 0x5a, HV_IJC_TABLE_BYTES);
    p->ijc_tables_used |= 1u << 1, p->ijc_tables_held |= 1u << 1;
    hv_cpu_reset_index(&cpu, &m, 0); /* its own state too: in place what it holds (hv_zero_kept) */
    /* in memory before reading it: reading fresh pages brings them in, so a reset that swapped them would pass */
    uint64_t kept = in_memory(t1, HV_IJC_TABLE_BYTES);
    check(kept == HV_IJC_TABLE_BYTES && all_zero(t1, HV_IJC_TABLE_BYTES) && (p->ijc_tables_held >> 1 & 1),
          "a CPU reset zeroes its used tables in place");
    printf("  a reset kept %.1f of the used table's %.1f MB\n", MB(kept), MB(HV_IJC_TABLE_BYTES));
    /* After a trim, neither a run-out nor a reset writes the given-back table in again (2ea4d2c found a reset doing
       so: the give-back kept the used bit). The table is used, so the clears would write it if they still saw it. */
    memset(t1, 0x5a, HV_IJC_TABLE_BYTES);
    p->ijc_tables_used |= 1u << 1, p->ijc_tables_held |= 1u << 1;
    hv_cpu_ijc_give_back(p);
    f2 = footprint();
    for (int i = 0; i < 0x10000; i++) hv_ijc_reset(&cpu, 1);
    hv_cpu_reset_index(&cpu, &m, 0);
    f3 = footprint();
    uint64_t mem = in_memory(t1, HV_IJC_TABLE_BYTES); /* before reading it: a read may bring pages in */
    printf("after a trim, a run-out and a reset: footprint %.1f -> %.1f MB, the table %.1f MB in memory\n", MB(f2), MB(f3),
           MB(mem));
    check(mem == 0 && f3 < f2 + (HV_IJC_TABLE_BYTES / 4) && !p->ijc_tables_used && all_zero(t1, HV_IJC_TABLE_BYTES),
          "after a trim, run-outs and resets bring none of it back");
    /* A give-back the system refuses (mmap failing under memory pressure) leaves the table's entries where they were,
       so it stays counted as holding them: the next run-out must still zero it, before an id comes round to an old
       tag. Before, the give-back dropped its bits either way, and the run-out passed it by. */
    memset(t1, 0x5a, HV_IJC_TABLE_BYTES);
    p->ijc_tables_used |= 1u << 1, p->ijc_tables_held |= 1u << 1;
    mmap_refuse = 1;
    freed = hv_cpu_ijc_give_back(p);
    mmap_refuse = 0;
    check(freed == 0 && t1[0] == 0x5a5a5a5a5a5a5a5aull && (p->ijc_tables_used >> 1 & 1) && (p->ijc_tables_held >> 1 & 1),
          "a refused give-back keeps the table counted as used and held");
    for (int i = 0; i < 0x10000; i++) hv_ijc_reset(&cpu, 1);
    check(all_zero(t1, HV_IJC_TABLE_BYTES), "and the next run-out still zeroes it");
    freed = hv_cpu_ijc_give_back(p);
    check(freed >= HV_IJC_TABLE_BYTES * 3 / 4 && !p->ijc_tables_held && !p->ijc_tables_used, "the next trim gives it back");
    /* The same, with the CPU's tables cleared while the give-back runs (a parked CPU's CPU_ON reset, a TLBI's
       run-out: right after, its ids start over). That clear must still see the table as used and zero it. Before,
       the give-back took the used bits before the drop and put them back after it failed: the clear passed the
       table by, and it kept its entries under ids about to be handed out again. */
    memset(t1, 0x5a, HV_IJC_TABLE_BYTES);
    p->ijc_tables_used |= 1u << 1, p->ijc_tables_held |= 1u << 1;
    mmap_refuse = 1, mmap_meanwhile = clear_cpu0;
    freed = hv_cpu_ijc_give_back(p);
    mmap_refuse = 0, mmap_meanwhile = NULL;
    check(freed == 0 && all_zero(t1, HV_IJC_TABLE_BYTES) && (p->ijc_tables_held >> 1 & 1),
          "a clear during a refused give-back still zeroes the table, its memory still held");
    hv_cpu_ijc_give_back(p);

    /* The (user id, page) set (2 MB a CPU) goes back with the user tables: as a process that cached targets in 30000
       pages leaves it, beside the user table holding them. Before, a trim never gave it back. */
    size_t ub = ((sizeof *p->ucode << HV_UCODE_BITS) + getpagesize() - 1) / getpagesize() * getpagesize();
    uint64_t *t0 = p->ijc; /* table 0: a user one */
    for (uint64_t i = 0; i < 30000; i++) hv_ucode_add(&cpu, 0x100000000ull + (i << HV_TLB_PAGE_BITS));
    uint32_t count = p->ucode_count;
    memset(t0, 0x5a, HV_IJC_TABLE_BYTES);
    p->ijc_tables_used |= 1u, p->ijc_tables_held |= 1u;
    p->last_fill[0] = 0x1234;
    uint64_t uheld = in_memory(p->ucode, ub);
    /* A user table the system won't give back still holds entries: the set must keep saying which pages they are in,
       or a TLBI would pass them by. */
    mmap_refuse = 1, mmap_refuse_at = t0;
    freed = hv_cpu_ijc_give_back(p);
    mmap_refuse = 0, mmap_refuse_at = NULL;
    check(count == 30000 && uheld >= ub * 3 / 4 && p->ucode_count == count && in_memory(p->ucode, ub) == uheld &&
              !all_zero(p->ucode, ub) && p->last_fill[0] == 0x1234 && t0[0] == 0x5a5a5a5a5a5a5a5aull,
          "a user table kept by a refused give-back keeps the (user id, page) set as it was");
    f2 = footprint();
    freed = hv_cpu_ijc_give_back(p);
    f3 = footprint();
    mem = in_memory(p->ucode, ub);
    printf("a trim with the set %.1f MB in memory and a user table: gave back %.1f MB, footprint %.1f -> %.1f MB, set %.1f MB left\n",
           MB(uheld), MB(freed), MB(f2), MB(f3), MB(mem));
    check(mem == 0 && freed >= HV_IJC_TABLE_BYTES * 3 / 4 + uheld * 3 / 4 && f3 + uheld / 2 + HV_IJC_TABLE_BYTES / 2 < f2,
          "the next trim gives the set back with the user table");
    check(p->ucode_count == 0 && p->last_fill[0] == 0, "  its count 0, and the next fill in the last page adds it again");
    hv_ucode_add(&cpu, 0x100000000ull);
    mem = in_memory(p->ucode, ub); /* before reading it, as above */
    uint32_t live = 0;
    for (size_t i = 0; i < ub / 8; i++) live += p->ucode[i] != 0;
    check(p->ucode_count == 1 && live == 1 && mem <= (uint64_t)getpagesize() && all_zero(t0, HV_IJC_TABLE_BYTES),
          "  it reads empty but for the one added after, filling again a page at a time");
    hv_cpu_ijc_give_back(p);
#endif

    /* What else a CPU holds for its caches goes back with them: its stashes (a process's user TLB entries kept across
       switches, 272 KB), the (user id, 32 MB chunk) set (2 MB), jc (1 MB) and the filters (ijc_keyed, both hint
       halves, ijc_pages). Before, a trim gave none of it back. */
    {
        HvTlbStash *st = &p->stash[3];
        size_t cb = 65536 * sizeof *p->cid_chunks;
#define CACHES_HELD() (in_memory(st->ent, HV_STASH_CAP * sizeof *st->ent) + in_memory(p->cid_chunks, cb) + \
                       in_memory(p->jc, sizeof p->jc) + in_memory(p->ijc_keyed, sizeof p->ijc_keyed) + \
                       in_memory(p->ijc_hint, sizeof p->ijc_hint) + in_memory(p->ijc_pages, sizeof p->ijc_pages))
        /* as a run leaves them: every byte written (jc and the filters with zeros: what they hold doesn't matter here) */
#define FILL_CACHES() do { \
            memset(st->ent, 0x11, HV_STASH_CAP * sizeof *st->ent); \
            memset(st->slot, 0x22, HV_STASH_CAP * sizeof *st->slot); \
            st->n = HV_STASH_CAP, st->ttbr0 = 0x5000000004000ull; \
            memset(p->cid_chunks, 0x33, cb); \
            memset(p->jc, 0, sizeof p->jc); \
            memset(p->ijc_keyed, 0, sizeof p->ijc_keyed), p->ijc_keyed_count = 1000; \
            memset(p->ijc_hint, 0, sizeof p->ijc_hint); \
            memset(p->ijc_pages, 0, sizeof p->ijc_pages); \
            p->last_fill[0] = 0x1234, p->last_fill[1] = 0x5678; \
        } while (0)
        FILL_CACHES();
#ifndef OLD
        p->jc_used = p->ijc_keyed_used = p->ijc_hint_used[0] = p->ijc_hint_used[1] = p->ijc_pages_used = ~0ull;
#endif
        uint64_t held = CACHES_HELD(), before = footprint(), gave = hv_cpu_ijc_give_back(p), after = footprint();
        uint64_t left = CACHES_HELD();
        printf("a trim with a full stash, the chunk set, jc and the filters in memory (%.2f MB): gave back %.2f MB, "
               "footprint %.1f -> %.1f MB, %.2f MB left\n", MB(held), MB(gave), MB(before), MB(after), MB(left));
        check(held >= (3u << 20) && left == 0 && gave >= held * 9 / 10 && after + held * 3 / 4 < before,
              "a trim gives back the stashes, the chunk set, jc and the filters");
        check(st->ttbr0 == 0 && st->n == 0, "  the stash is empty: no switch restores it");
        check(p->ijc_keyed_count == 0 && p->last_fill[0] == 0 && p->last_fill[1] == 0,
              "  the filters' count 0, and the next fill in the last pages sets their bits again");
        check(all_zero(p->cid_chunks, cb) && all_zero(p->ijc_keyed, sizeof p->ijc_keyed), "  they read empty");
#ifndef OLD
        check(!p->jc_used && !p->ijc_keyed_used && !p->ijc_hint_used[0] && !p->ijc_hint_used[1] && !p->ijc_pages_used,
              "  no part of them counted as written");
        /* A user table the system won't give back still holds entries under the user ids: the chunk set, the user
           filters and ijc_keyed (both halves) keep saying where, used bits and all. The stash, jc and the kernel
           hints have nothing to do with it, and go. */
        FILL_CACHES();
        p->jc_used = p->ijc_keyed_used = p->ijc_hint_used[0] = p->ijc_hint_used[1] = p->ijc_pages_used = ~0ull;
        memset(t0, 0x5a, HV_IJC_TABLE_BYTES);
        p->ijc_tables_used |= 1u, p->ijc_tables_held |= 1u;
        mmap_refuse = 1, mmap_refuse_at = t0;
        hv_cpu_ijc_give_back(p);
        mmap_refuse = 0, mmap_refuse_at = NULL;
        check(in_memory(p->cid_chunks, cb) == cb && in_memory(p->ijc_pages, sizeof p->ijc_pages) == sizeof p->ijc_pages &&
                  in_memory(p->ijc_hint[0], sizeof p->ijc_hint[0]) == sizeof p->ijc_hint[0] &&
                  in_memory(p->ijc_keyed, sizeof p->ijc_keyed) == sizeof p->ijc_keyed && p->ijc_pages_used == ~0ull &&
                  p->ijc_hint_used[0] == ~0ull && p->ijc_keyed_used == ~0ull && p->ijc_keyed_count == 1000 &&
                  p->last_fill[0] == 0x1234,
              "a kept user table keeps the chunk set, the user filters and ijc_keyed, used bits and all");
        check(in_memory(st->ent, HV_STASH_CAP * sizeof *st->ent) == 0 && in_memory(p->jc, sizeof p->jc) == 0 &&
                  in_memory(p->ijc_hint[1], sizeof p->ijc_hint[1]) == 0 && !p->jc_used && !p->ijc_hint_used[1] &&
                  p->last_fill[1] == 0,
              "  and the stash, jc and the kernel hints go");
        hv_cpu_ijc_give_back(p);
        check(CACHES_HELD() == 0 && !p->ijc_tables_used, "  the next trim gives back the rest");
        /* User ids running out after that: the chunk set is zeroed in place, which must not bring back what the trim
           gave (a memset of all 2 MB did): only the chunk of the id handed out comes in. */
        cpu.next_cid = 0x10000, cpu.tcr_el1 = 0, cpu.ttbr0_el1 = 9ull << 48 | 0x4000, cpu.asid_cid[9] = 0;
        hv_ijc_switch(&cpu);
        uint64_t chunks = in_memory(p->cid_chunks, cb);
        printf("user ids run out after the trim: the chunk set %.2f MB in memory\n", MB(chunks));
        check(cpu.asid_cid[9] == 1 && chunks <= (uint64_t)getpagesize() && all_zero(p->cid_chunks, cb),
              "  a run-out of user ids after it brings back only the new id's chunk");
        hv_cpu_ijc_give_back(p);
#endif
#undef FILL_CACHES
#undef CACHES_HELD
    }

    /* 2 and 3. Eight zones nearly full of records, as a long run leaves them. */
    hv_machine_set_zones(&m, 8);
    uint32_t per_zone = m.zone_blocks * 3 / 4;
    for (int z = 0; z < m.nzones; z++) fill_zone(z, per_zone);
    uint64_t full = footprint();
    printf("%d zones of %u blocks: footprint %.0f MB\n", m.nzones, per_zone, MB(full));
    hv_machine_request_trim(&m, 0); /* a memory warning */
    hv_jit_make_room(&m, -1);        /* the cache fills: the trim happens as it makes room */
    uint64_t trimmed = footprint();
    printf("after the warning's trim: %.0f MB (said %.0f MB given back), %d zones in use, filling zone %d\n", MB(trimmed),
           MB(m.trim_released), active(), m.zone);
    check(m.trims == 1 && m.trim_released > 0, "the warning trimmed a zone");
    double fell = (double)(full - trimmed), said = (double)m.trim_released;
    check(said <= fell * 1.15 + (1u << 20) && said >= fell * 0.85 - (1u << 20), "what it says it gave back is what the footprint lost");
    check(active() == m.nzones - 1, "the trimmed zone is out of use");
    fill_zone(m.zone, per_zone); /* the Mac goes on */
    uint64_t again = footprint();
    printf("once the zone being filled is full again: %.0f MB\n", MB(again));
    check(again + m.trim_released / 2 <= full, "a memory warning leaves the cache's tables smaller for good");
    /* The cache keeps going round the zones still in use; the retired one is never filled. */
    int retired_filled = 0;
    for (int k = 0; k < 3 * m.nzones; k++) {
        hv_jit_make_room(&m, -1);
#ifndef OLD
        retired_filled |= (int)(m.zones_retired >> m.zone & 1);
#endif
        fill_zone(m.zone, per_zone);
    }
    check(!retired_filled, "no retired zone is filled again");
    /* More warnings: down to half the zones, no further. */
    for (int k = 0; k < 6; k++) {
        hv_machine_request_trim(&m, 0);
        hv_jit_make_room(&m, -1);
        fill_zone(m.zone, per_zone);
    }
    printf("after 7 warnings: %d zones in use\n", active());
    check(active() == m.nzones / 2, "warnings retire zones down to half of them");
    check(m.trims == 7, "each still trims the zone it empties");
#ifndef OLD
    /* And never fewer than two, however few zones there are: with three, half would be one, and a warning that
       retired down to it would leave the cache no zone to empty but the one it fills (make_room's last resort, a
       full flush, for every block from then on). Warnings and full flushes both. */
    for (int nz = 2; nz <= 4; nz++) {
        hv_machine_set_zones(&m, nz);
        for (int k = 0; k < 2 * nz; k++) {
            hv_machine_request_trim(&m, 0);
            if (k & 1) hv_flush_blocks(&m); else hv_jit_make_room(&m, -1);
            fill_zone(m.zone, m.zone_blocks * 3 / 4);
        }
        int want = nz / 2 > 2 ? nz / 2 : 2;
        printf("%d zones, %d warnings: %d in use\n", nz, 2 * nz, active());
        check(active() == want && !(m.zones_retired >> m.zone & 1), "  never fewer than two zones in use, the one filled among them");
    }
#endif
    /* A critical one, after asking for zones again (all back in use): a full flush, and one zone fewer. */
    hv_machine_set_zones(&m, 8);
    check(active() == 8, "asking for a zone count puts every zone back in use");
    hv_machine_request_trim(&m, 0);
    hv_flush_blocks(&m);
    check(active() == 7, "a full flush under a trim retires a zone too");
#ifndef OLD
    check(!(m.zones_retired >> m.zone & 1), "and fills one still in use");
#endif

    /* 4. A warning early on, or soon after a full flush: two zones filled, the rest empty. Before, emptying an empty
       zone skipped the trim, so each make_room retired another empty zone and nothing went back until half were
       gone, and then the trim fell on the zone about to be filled. Now the zone that holds the memory is retired. */
    hv_machine_set_zones(&m, 8);
    hv_flush_blocks(&m);
    int first = m.zone;
    fill_zone(first, per_zone);
    hv_jit_make_room(&m, -1); /* an empty zone next */
    fill_zone(m.zone, per_zone);
    unsigned trims0 = m.trims;
    uint64_t one = footprint();
    hv_machine_request_trim(&m, 0);
    hv_jit_make_room(&m, -1);
    uint64_t after = footprint();
    printf("two zones filled, then a warning: %.0f -> %.0f MB, %d zones in use, filling zone %d\n", MB(one), MB(after), active(),
           m.zone);
    check(m.trims == trims0 + 1 && !m.trim_pending, "it trims at once");
    check(active() == m.nzones - 1, "and retires one zone only");
#ifndef OLD
    check(m.zones_retired >> first & 1, "the one that held the memory");
#endif
    check(after + m.trim_released / 2 <= one && m.trim_released > 0, "which goes back to the system");
    hv_jit_make_room(&m, -1);
    check(active() == m.nzones - 1, "the next make_room retires nothing more");

#ifndef OLD
    /* 5. Zones come back. Three retired, every zone in use full. */
    hv_machine_set_zones(&m, 8);
    for (int z = 0; z < m.nzones; z++) fill_zone(z, per_zone);
    for (int k = 0; k < 3; k++) {
        hv_machine_request_trim(&m, 0);
        hv_jit_make_room(&m, -1);
        fill_zone(m.zone, per_zone);
    }
    check(active() == m.nzones - 3, "three warnings: three zones retired");
    m.trim_quiet_ns = 1ull << 62;
    hv_jit_make_room(&m, -1);
    fill_zone(m.zone, per_zone);
    check(active() == m.nzones - 3 && HV_STAT_TOTAL(&m, zones_returned) == 0, "not quiet for long enough: none comes back");
    m.trim_quiet_ns = 0;
    uint32_t retired = m.zones_retired;
    uint64_t emptied = HV_STAT_TOTAL(&m, evict_live) + HV_STAT_TOTAL(&m, evict_dead);
    hv_jit_make_room(&m, -1);
    check(active() == m.nzones - 2 && HV_STAT_TOTAL(&m, zones_returned) == 1 && (retired >> m.zone & 1),
          "quiet: making room fills a retired zone");
    check(HV_STAT_TOTAL(&m, evict_live) + HV_STAT_TOTAL(&m, evict_dead) == emptied, "  and empties no zone in use");
    fill_zone(m.zone, per_zone);
    hv_machine_request_trim(&m, 0);
    m.trim_quiet_ns = 1ull << 62; /* a new warning, just now */
    hv_jit_make_room(&m, -1);
    fill_zone(m.zone, per_zone);
    check(active() == m.nzones - 3, "a warning again retires one");
    m.trim_quiet_ns = 0;
    for (int k = 0; k < 3; k++) {
        hv_jit_make_room(&m, -1);
        fill_zone(m.zone, per_zone);
    }
    check(active() == m.nzones && HV_STAT_TOTAL(&m, zones_returned) == 4, "quiet again: one comes back each time, till all are");
    uint64_t f_all = footprint();
    printf("every zone back in use and full: %.0f MB\n", MB(f_all));
#endif

    hv_machine_free(&m);
    return failures != 0;
}
