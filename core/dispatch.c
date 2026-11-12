#include "dispatch.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include "classify.h"
#include "exception.h"
#include "memop.h"
#include "mmu.h"
#include "operands.h"
#include "sysreg.h"
#include "translate.h"
#include "rosetta.h"

/* ---- The code cache: its zones, making room, flushing it all ----
   Everything here runs with every other CPU parked (stop_the_world) and the translation lock held, or before any
   CPU runs; only the helpers that report on it (hv_block_at and the totals) are for any thread. */

static size_t zone_word0(const HvMachine *m, int z) { return (size_t)z * m->zone_words; }

/* How far the zone being filled is, from the cursors the translators move, into its HvJitZone. */
static void zone_save(HvMachine *m) {
    int z = m->zone;
    HvJitZone *zn = &m->zones[z];
    size_t c0 = zone_word0(m, z), used = __atomic_load_n(&m->jit.used, __ATOMIC_RELAXED);
    used = used < c0 ? 0 : used - c0;
    if (used > m->zone_words) used = m->zone_words; /* a reservation that did not fit counted past the end */
    zn->used = used;
    uint32_t ns = __atomic_load_n(&m->nslots, __ATOMIC_RELAXED);
    if (ns > m->slot_limit) ns = m->slot_limit; /* the same for a slot */
    zn->nblocks = (uint32_t)m->nblocks - (uint32_t)z * m->zone_blocks;
    zn->nslots = ns - (uint32_t)z * m->zone_slots;
    zn->nundef = m->nundef - (uint32_t)z * m->zone_undef;
    /* How far the tables were written, for a trim (now or a later one) */
    if ((uint32_t)m->nblocks > m->hw_blocks) m->hw_blocks = (uint32_t)m->nblocks;
    if (ns > m->hw_slots) m->hw_slots = ns;
    if (m->nundef > m->hw_undef) m->hw_undef = m->nundef;
}

/* HV_JIT_POISON: zero code that goes, through hv_jit_write (on a Mac the region is writable only between that and
   hv_jit_finish, on this thread). */
static void jit_zero(HvMachine *m, uint32_t *code, size_t words) {
    static const uint32_t zero[4096];
    for (size_t i = 0; i < words; i += 4096) {
        size_t n = words - i < 4096 ? words - i : 4096;
        hv_jit_write(&m->jit, code + i, zero, n == 1 ? 4 : n * 4);
    }
    hv_jit_finish(&m->jit, code, words);
}

static void early_marks(HvMachine *m, int z);

/* Fill zone z next: the cursors to where it is up to, the limits to its end (and where it is 3/4 full: early_marks). */
static void zone_load(HvMachine *m, int z) {
    HvJitZone *zn = &m->zones[z];
    m->zone = z;
    m->nblocks = (int)((uint32_t)z * m->zone_blocks + zn->nblocks);
    m->block_limit = (uint32_t)(z + 1) * m->zone_blocks;
    m->nundef = (uint32_t)z * m->zone_undef + zn->nundef;
    m->undef_limit = (uint32_t)(z + 1) * m->zone_undef;
    m->slot_limit = (uint32_t)(z + 1) * m->zone_slots;
    __atomic_store_n(&m->nslots, (uint32_t)z * m->zone_slots + zn->nslots, __ATOMIC_RELAXED);
    m->jit.limit = zone_word0(m, z) + m->zone_words;
    __atomic_store_n(&m->jit.used, zone_word0(m, z) + zn->used, __ATOMIC_RELAXED);
    /* A zone filled from here on, with no translator part way, has all its slots so far written: as far as a
       reclaimer's hash phase under way looks (perhaps none, if it was emptied since the mark: none since is chained
       into the zone being emptied) */
    m->rc_slots_end[z] = zn->nslots;
    early_marks(m, z);
}

void hv_jit_zones_sync(HvMachine *m) { zone_save(m); }
void hv_jit_zone_fill(HvMachine *m, int z) { zone_load(m, z); }

/* The fewest zones a trim leaves in use (zones_retired): half of them, and never fewer than two. */
static int zones_floor(const HvMachine *m) { return m->nzones / 2 > 2 ? m->nzones / 2 : 2; }

static int zones_active(const HvMachine *m) {
    int n = 0;
    for (int z = 0; z < m->nzones; z++) n += !(m->zones_retired >> z & 1);
    return n;
}

void hv_jit_zones_reset(HvMachine *m) {
    size_t cap = m->jit.capacity;
    if (hv_knob[HV_K_JITCAP] > 0 && ((size_t)hv_knob[HV_K_JITCAP] << 20) / sizeof(uint32_t) < cap)
        cap = ((size_t)hv_knob[HV_K_JITCAP] << 20) / sizeof(uint32_t); /* MB of it to use (testing) */
    int n = m->zones_asked < 1 ? 1 : m->zones_asked > HV_JIT_ZONES ? HV_JIT_ZONES : m->zones_asked;
    size_t words = n == 1 ? cap : cap / (size_t)n & ~(size_t)4095; /* whole 16 KB pages */
    if (!words || m->max_blocks / (uint32_t)n < 64) n = 1, words = cap;
    m->nzones = n;
    m->zone_words = words;
    m->zone_blocks = m->max_blocks / (uint32_t)n;
    m->zone_slots = m->max_slots / (uint32_t)n;
    m->zone_undef = m->max_undef / (uint32_t)n;
    hv_jit_reclaim_cancel(m); /* every zone is emptied at once: the reclaimer's job with them */
    m->zones_dying = 0, m->zone_ready = -1, m->room_early = m->room_early_seen = 0;
    memset(m->zones, 0, sizeof m->zones);
    memset(m->unhook_at, 0, sizeof m->unhook_at); /* nothing half taken off: every zone is empty */
    for (int z = 0; z < HV_JIT_ZONES; z++) m->zones[z].born = m->jit_gen;
    if (n == 1) m->zones_retired = 0;
    int first = 0;
    while (first < n - 1 && m->zones_retired >> first & 1) first++;
    zone_load(m, first);
}

void hv_machine_set_zones(HvMachine *m, int n) {
    pthread_mutex_lock(&m->tr_mu); /* the reclaimer reads zones_retired under it (a retire job) */
    m->zones_asked = n;
    m->zones_retired = 0;
    pthread_mutex_unlock(&m->tr_mu);
    hv_flush_blocks(m);
}

#define zone_nblocks hv_zone_nblocks
#define zone_used hv_zone_used

uint64_t hv_jit_words_used(const HvMachine *m) {
    uint64_t n = 0;
    for (int z = 0; z < m->nzones; z++) n += zone_used(m, z);
    return n;
}

uint64_t hv_jit_words_live(const HvMachine *m) {
    uint64_t n = 0;
    for (int z = 0; z < m->nzones; z++) {
        uint64_t u = zone_used(m, z), d = m->zones[z].dead_words;
        n += u > d ? u - d : 0;
    }
    return n;
}

uint32_t hv_blocks_held(const HvMachine *m) {
    uint32_t n = 0;
    for (int z = 0; z < m->nzones; z++) n += zone_nblocks(m, z);
    return n;
}

/* Is p in code that lives now (HV_JIT_POISON checks)? */
static int code_live(const HvMachine *m, const void *p) {
    int z = hv_jit_zone_of(m, p);
    return z >= 0 && (size_t)((const uint32_t *)p - m->jit.base) - zone_word0(m, z) < zone_used(m, z);
}

static int block_live(const HvMachine *m, const HvBlock *b) {
    return b->zone < m->nzones && b->jit_gen == m->zones[b->zone].born && code_live(m, b->code);
}

/* An exit no longer goes straight to its target: its B back to its stub (and the conditional branch that
   chaining sent there too, back to that B). Its target slot reads 0, so the stub asks the dispatcher. Under tr_mu;
   the code changes at the caller's hv_patch_flush. */
static void unchain_slot(HvMachine *m, uint32_t s) {
    __atomic_store_n(&m->slots[s], 0, __ATOMIC_RELEASE);
    uint32_t *site = m->slot_site[s];
    if (!site) return;
    hv_patch_add(m, site, 0x14000000u | (uint32_t)((m->slot_stub[s] - site) & 0x3ffffff));
    uint32_t *cond = m->slot_cond[s];
    if (cond) {
        uint32_t insn = *cond;
        if (hv_branch_retarget(&insn, site - cond)) hv_patch_add(m, cond, insn);
    }
}

/* Taking zone v off everything that leads into it (evict_unhook), in two phases. */
#define HV_UNHOOK_HASH 0  /* its blocks out of the hash; other zones' exits into its code cut, and off their lists */
#define HV_UNHOOK_LISTS 1 /* its blocks off their pages' lists; its own exits off the lists they are on */
_Static_assert(sizeof(((HvMachine *)0)->unhook_at[0]) == 2 * sizeof(((HvMachine *)0)->unhook_at[0][0]), "a cursor per phase");

/* One phase of taking zone v off what leads into it, a budget of work at a time. The contract, for emptying a zone
   in a stop (evict_zone) and in the background while the other CPUs run:
   - HV_UNHOOK_HASH: each block of v not retired comes out of the hash (hv_hash_remove: a release store into the
     link to it, its own hash_next left alone, so a lookup walking the bucket lock-free goes on along a valid
     chain); then each exit of every other zone whose chained target is in v's code goes back to its stub
     (unchain_slot) and, if it is on a page's list of exits from other pages, comes off it (hv_xlist_remove). The
     blocks stay on their pages' lists: a write to their code still finds them there and gates them (retire_page).
   - HV_UNHOOK_LISTS: each block of v not retired comes off its page's list (hv_page_list_remove), then each of v's
     own exits that is on a list comes off it. Both O(1) each, whatever else the pages hold.
   - A phase resumes where its last call stopped: m->unhook_at[v][phase], the next block of v, then the next slot
     (an index into the whole table: the hash phase goes over every other zone's used slots in zone order, the lists
     phase over v's own). A call looks at about budget blocks and slots (0: no limit) and returns 1 once the phase
     is done (its cursor back to 0, so another call would start it over), 0 when the budget ran out first.
   - The caller holds tr_mu for each call and may drop it between calls while the other CPUs run: each call leaves
     the hash, the lists and nxchained consistent, and runs hv_patch_flush before it returns, so no rewrite of an
     exit waits in the machine's queue for code that may be filled again once tr_mu is dropped. A block retired
     between calls (retire_page: out of the hash if it was still in, off its list, gated) is dead, and skipped.
   - v is not the zone being filled: its counts (zones[v].nblocks, nslots, used) do not move. The other zones' slots
     are scanned as far as their counts (hv_zone_nslots), and the zone being filled's no further than fill_slots of
     it. A translator takes a slot (put_branch_exit: the bump of m->nslots) lock-free before it writes the slot's
     target, site and list fields, so a slot past those of the blocks published may still hold what an emptied
     zone's block left there, a target in v among it, and cutting it would patch code another CPU has just placed.
     In a stop no translation is under way and HV_UNHOOK_LIVE_SLOTS (the live count) is right; in the background
     the caller passes a count of the zone being filled's slots published under tr_mu, all written before it (the
     reclaimer: that zone's count at its mark, rc_slots_end, taken in the stop).
   - What it does not do: anything a CPU holds or is doing. That is the front caches, branch caches and return tables
     (step 4 of evict_zone); a lookup that found a block of v before the hash phase, which may still chain an exit of
     another zone to it, after the scan has passed that slot; a CPU running v's code, which may chain v's own exits
     (chain()). In a stop none of these can be under way. In the background the caller must, BEFORE the hash phase,
     make v dying (hv_block_find and the front caches miss its blocks), set while every CPU is stopped, and then see
     every CPU pass a point holding no block of v it found before: only then is nothing left to chain into v once
     its slot has been scanned. It must also keep v's code from running and v's exits from being chained before the
     lists phase, and see every CPU hold none of v's code or blocks before v is filled again.
   Counts: blocks out of the hash (evict_live) and retired ones passed (evict_dead), hash links walked past
   (evict_hash_hops), blocks off page lists (evict_page_unlinks), exits cut (evict_unchained) and off lists
   (evict_delists); host ticks in evict_step_ticks[0] (blocks, both phases), [1] (the other zones' exits, with the
   code patches) and [2] (its own exits). */
#define HV_UNHOOK_LIVE_SLOTS ~0u /* fill_slots in a stop: the zone being filled's slots as far as its live count */
static int evict_unhook(HvMachine *m, int v, int phase, uint32_t budget, uint32_t fill_slots) {
    const HvJitZone *zn = &m->zones[v];
    HvBlock *b0 = m->blocks + (size_t)v * m->zone_blocks;
    uint32_t *at_b = &m->unhook_at[v][phase].block, *at_s = &m->unhook_at[v][phase].slot;
    uint32_t work = 0, live = 0, dead = 0, hops = 0, unlinks = 0, delists = 0, cut = 0, entries[4] = {0};
    int done = 0;
    uint64_t t0 = mach_absolute_time();
    for (; *at_b < zn->nblocks && (!budget || work < budget); work++) {
        HvBlock *b = &b0[(*at_b)++];
        if (b->dead) { /* retired: out of the hash and off its list already */
            dead++;
            continue;
        }
        if (phase == HV_UNHOOK_HASH) {
            hops += hv_hash_remove(m, b);
            hv_ghost_note(m, b, HV_GHOST_EVICTED);
            entries[hv_entries_bucket(__atomic_load_n(&b->entries, __ATOMIC_RELAXED))]++; /* CPUs bump it outside a stop */
            live++;
        } else {
            hv_page_list_remove(m, b);
            unlinks++;
        }
    }
    uint64_t t1 = mach_absolute_time();
    if (*at_b >= zn->nblocks) {
        uint32_t zs = m->zone_slots;
        if (phase == HV_UNHOOK_LISTS) { /* its own exits: translating sets the rest again before a slot is used */
            uint32_t s0 = (uint32_t)v * zs, s1 = s0 + zn->nslots;
            if (*at_s < s0) *at_s = s0;
            for (; *at_s < s1 && (!budget || work < budget); work++, ++*at_s)
                if (m->slot_xpg[*at_s]) hv_xlist_remove(m, *at_s), delists++;
            done = *at_s >= s1;
        } else if (!zn->used || !zs) {
            done = 1; /* no code: nothing chained into it */
        } else { /* the other zones' exits into its code */
            uint64_t lo = (uint64_t)(uintptr_t)(m->jit.base + zone_word0(m, v)), span = (uint64_t)zn->used * 4;
            for (;;) {
                uint32_t z = *at_s / zs;
                if (z >= (uint32_t)m->nzones) {
                    done = 1;
                    break;
                }
                uint32_t n = hv_zone_nslots(m, (int)z);
                if ((int)z == __atomic_load_n(&m->zone, __ATOMIC_RELAXED) && fill_slots < n) n = fill_slots;
                if ((int)z == v || *at_s >= z * zs + n) {
                    *at_s = (z + 1) * zs;
                    continue;
                }
                if (budget && work >= budget) break;
                uint32_t sl = (*at_s)++;
                work++;
                uint64_t t = __atomic_load_n(&m->slots[sl], __ATOMIC_RELAXED);
                if (!t || t - lo >= span) continue;
                unchain_slot(m, sl);
                cut++;
                if (m->slot_xpg[sl]) hv_xlist_remove(m, sl), delists++;
            }
        }
    }
    hv_patch_flush(m); /* one barrier per 4096 of them (HV_PATCH_MAX), not one each */
    uint64_t t2 = mach_absolute_time();
    if (done) *at_b = 0, *at_s = 0;
    if (phase == HV_UNHOOK_HASH) {
        HV_STAT_ADD(m, evict_live, live);
        HV_STAT_ADD(m, evict_dead, dead);
        HV_STAT_ADD(m, evict_hash_hops, hops);
        HV_STAT_ADD(m, evict_unchained, cut);
        for (int k = 0; k < 4; k++) HV_STAT_ADD(m, entries_evicted[k], entries[k]);
    } else {
        HV_STAT_ADD(m, evict_page_unlinks, unlinks);
    }
    HV_STAT_ADD(m, evict_delists, delists);
    HV_STAT_ADD(m, evict_step_ticks[0], t1 - t0);
    HV_STAT_ADD(m, evict_step_ticks[phase == HV_UNHOOK_HASH ? 1 : 2], t2 - t1);
    return done;
}

/* Where the reclaimer (below) has got with the zone it is emptying, in order. */
enum { RC_HASH, RC_WAIT, RC_LISTS, RC_POISON, RC_DROP, RC_DONE };

static void stat_max(uint64_t *at, uint64_t v) {
    for (uint64_t was = __atomic_load_n(at, __ATOMIC_RELAXED);
         v > was && !__atomic_compare_exchange_n(at, &was, v, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);) {
    }
}

/* A CPU's front cache without its entries for blocks of the zones in mask: only the parts lookups wrote (jc_used:
   the rest reads zero, and reading it would bring it into memory); a part left with no entry loses its bit. The
   zone is told from the record's place in the table, not read from it. On the CPU's own thread, or with it parked. */
static void jc_purge(const HvMachine *m, HvPerCpu *p, uint32_t mask) {
    if (!mask) return;
    for (uint64_t u = __atomic_load_n(&p->jc_used, __ATOMIC_RELAXED); u; u &= u - 1) { /* as hv_parts_clear */
        int part = __builtin_ctzll(u);
        int left = 0;
        for (uint32_t j = (uint32_t)part << (HV_JC_BITS - 6); j < ((uint32_t)part + 1) << (HV_JC_BITS - 6); j++) {
            const HvBlock *b = p->jc[j].b;
            if (b && mask >> ((size_t)(b - m->blocks) / m->zone_blocks) & 1) p->jc[j].va = 0, p->jc[j].b = NULL;
            left |= p->jc[j].b != NULL || p->jc[j].va != 0;
        }
        if (!left) __atomic_and_fetch(&p->jc_used, ~(1ull << part), __ATOMIC_RELAXED);
    }
}

/* Empty zone v: nothing anywhere may lead into its code or its block records afterwards. Every other CPU is stopped
   and tr_mu held, or none runs. from: how far the reclaimer had got with it (RC_HASH: not started, or not its zone);
   the rest is done here, from its cursors. */
static void evict_zone_from(HvMachine *m, int v, int from) {
    HvJitZone *zn = &m->zones[v];
    if (from == RC_HASH && !zn->nblocks && !zn->used && !zn->nslots && !zn->nundef) { /* never filled since the last full flush */
        memset(zn, 0, sizeof *zn);
        memset(m->unhook_at[v], 0, sizeof m->unhook_at[v]);
        return;
    }
    uint64_t t0 = mach_absolute_time();
    /* 1-3. Its blocks out of the hash and off their pages' lists, the exits into its code cut, and every exit
       involved off its list: both phases whole, as no CPU runs (from where any background work left them). */
    if (from <= RC_HASH)
        while (!evict_unhook(m, v, HV_UNHOOK_HASH, 0, HV_UNHOOK_LIVE_SLOTS)) {
        }
    if (from <= RC_LISTS)
        while (!evict_unhook(m, v, HV_UNHOOK_LISTS, 0, HV_UNHOOK_LIVE_SLOTS)) {
        }
    uint64_t t3 = mach_absolute_time();
    /* 4. Every CPU's caches: the front cache's entries for its blocks, and the branch caches entirely (they hold code
       addresses, and fill again from the hash without translating). hv_ijc_reset gives each half a new salt, which
       the return tables are keyed by too: their entries into this zone's code are dead with it. Not once the
       reclaimer's grace period is over: every CPU has done the same since the mark (reclaim_ack), and nothing has
       put this zone's code in them since (its blocks are found by no lookup). */
    if (from <= RC_WAIT)
        for (int i = 0; i < m->ncpus; i++) {
            if (!m->cpus[i]) continue;
            jc_purge(m, &m->percpu[i], 1u << v);
            __atomic_and_fetch(&m->percpu[i].reclaim_purge, ~(1u << v), __ATOMIC_RELAXED); /* done for it */
            hv_ijc_reset(m->cpus[i], 0);
            hv_ijc_reset(m->cpus[i], 1);
        }
    uint64_t t4 = mach_absolute_time();
    HV_STAT_ADD(m, evicted_blocks, zn->nblocks);
    HV_STAT_ADD(m, evict_step_ticks[3], t4 - t3);
    stat_max(&m->stats.evict_max_ticks, t4 - t0);
    /* HV_JIT_POISON: its code goes, so a pointer left into it faults where it is used (from where the reclaimer got). */
    size_t done = from >= RC_POISON ? m->rc_poison_at : 0;
    if (m->gen_checks && zn->used > done) jit_zero(m, m->jit.base + zone_word0(m, v) + done, zn->used - done);
    memset(zn, 0, sizeof *zn);
    memset(m->unhook_at[v], 0, sizeof m->unhook_at[v]);
}

static void evict_zone(HvMachine *m, int v) { evict_zone_from(m, v, RC_HASH); }

/* The zone to empty next, of those in use other than cur: an empty one before any, or one whose every block is
   retired (nothing to lose); else, among those not filled most recently (their code was only just made, so it has
   had little time to be used), the one least used lately (retired blocks are never entered, so they add nothing to
   that), then the one filled longest ago. A zone with fewer live blocks is not thereby less used: few hot ones
   cost more to lose than many no process runs any more. held: only a zone that holds something, the newest
   quarter of those kept (the one a memory warning retires: an empty one would give nothing back now); any zone if
   none does. Never one being emptied, or emptied and waiting to be filled (zones_dying). -1: none. */
static int pick_victim(const HvMachine *m, int cur, int held) {
    uint32_t out = m->zones_retired | m->zones_dying | 1u << cur;
    if (held)
        for (int z = 0; z < m->nzones; z++)
            if (!m->zones[z].nblocks && !m->zones[z].used) out |= 1u << z;
    int active = 0;
    for (int z = 0; z < m->nzones; z++) active += !(out >> z & 1);
    int protect = active >= 3 ? (active + 1) / 4 : 0, v = -1;
    for (int z = 0; z < m->nzones; z++)
        if (!(out >> z & 1) && (!m->zones[z].filled || m->zones[z].ndead == m->zones[z].nblocks)) return z;
    for (int z = 0; z < m->nzones; z++) {
        if (out >> z & 1) continue;
        int younger = 0;
        for (int y = 0; y < m->nzones; y++) younger += !(out >> y & 1) && m->zones[y].filled > m->zones[z].filled;
        if (younger < protect) continue;
        if (v < 0) {
            v = z;
            continue;
        }
        const HvJitZone *a = &m->zones[z], *c = &m->zones[v];
        if (a->score < c->score || (a->score == c->score && a->filled < c->filled)) v = z;
    }
    return v < 0 && held ? pick_victim(m, cur, 0) : v;
}

/* A zone in use, not the one being filled nor dying, that holds nothing (never filled since the last full flush):
   filling it costs nothing, so none need be emptied ahead. -1: none. */
static int free_zone(const HvMachine *m) {
    for (int z = 0; z < m->nzones; z++) {
        const HvJitZone *zn = &m->zones[z];
        if (z != m->zone && !((m->zones_retired | m->zones_dying) >> z & 1) && !zn->nblocks && !zn->used && !zn->nslots &&
            !zn->nundef)
            return z;
    }
    return -1;
}

static void flush_blocks(HvMachine *m, int trim);

/* A trim in a stop: what goes back is only noted (zones' tables, every CPU's branch caches), and given back once the
   CPUs run again (hv_machine_give_back; each CPU its own caches, in unpark), so the stop isn't made longer by it.
   Each part is counted in giveback_left, so the trim counts as done (trims, which the app waits for before it reports
   trim_released) only once every part has been given back. Every CPU is parked here, so nothing counts down
   meanwhile. A part of the last trim may still be owed: a CPU waking as that stopper looked was left its caches, then
   saw this stop and parked again. Its flag is still set, so it is not counted twice, and trim_released is not
   started over: the two trims count as one, done when the last part of either is. The reclaimer may owe one too
   (rc_owed: the tables of a zone it is emptying to retire), counted the same way (retire_one). */
static void note_give_back(HvMachine *m, uint32_t zones) {
    int left = 0;
    if (!__atomic_load_n(&m->giveback_left, __ATOMIC_SEQ_CST)) __atomic_store_n(&m->trim_released, 0, __ATOMIC_RELAXED);
    m->giveback_zones |= zones;
    for (int i = 0; i < HV_MAX_CPUS; i++)
        if (m->percpu[i].ijc && !__atomic_exchange_n(&m->percpu[i].ijc_giveback, 1, __ATOMIC_SEQ_CST)) left++;
    if (!__atomic_exchange_n(&m->giveback_pending, 1, __ATOMIC_SEQ_CST)) left++;
    __atomic_add_fetch(&m->giveback_left, left, __ATOMIC_SEQ_CST);
}

/* Warnings have stopped for trim_quiet_ns: a zone retired may be filled again. */
static int trim_quiet(const HvMachine *m) {
    uint64_t at = __atomic_load_n(&m->trim_at, __ATOMIC_RELAXED);
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - at >= m->trim_quiet_ns;
}

/* ---- Emptying a zone in the background: the reclaimer ----
   A stop used to empty the zone it filled next (evict_zone) with every CPU held for the whole of it: 145-360 ms on
   the iPad. Now, once the zone being filled is 3/4 full, a stop only marks the next one (reclaim_mark, O(1)); a
   thread of its own, the reclaimer, empties it with the CPUs running, in chunks under tr_mu; and the stop that finds
   the cache full fills it at once (zone_ready).

   The protocol's invariants, and the code that keeps each:
   1. A marked zone is in zones_dying. The bit is set in a stop, or with no CPU running, so every CPU sees it from
      when it runs again; it is cleared only as the zone is filled again (make_room_from: in a stop, with jit_gen
      bumped) or as every zone is emptied at once (hv_jit_zones_reset, hv_code_store_load: hv_jit_reclaim_cancel
      first). It is never the zone being filled.
   2. From the mark on, nothing new leads into a dying zone. No lookup returns its blocks (hv_block_find; the front
      cache in lookup and mmu.c's ijc_resolve, which also compare the block's own va). The stop that marks it resets
      every CPU's branch caches (hv_ijc_reset: a new salt, so the return tables' entries die too), and those fill
      only from lookups. chain() links no exit of a dying zone and none to one (rechecked under tr_mu for an exit to
      another page). A CPU may still run the zone's code (it was in it at the stop, or returns into it), chain its
      exits through slots already set, and write returns into its return tables: all of that stays in the zone.
   3. RC_HASH, in chunks under tr_mu (evict_unhook's hash phase): its blocks out of the hash, every other zone's
      exits into its code cut (back to their stubs) and off their lists. Its blocks stay on their pages' lists, so
      a write to their code still gates them (retire_page) while a CPU may be running them. Only the slots in use at
      the mark (rc_slots_end): one taken since is a translator's until it is written (it may hold a stale address in
      the zone, with a stale site), and none of them is chained into the zone (2).
   4. RC_WAIT, the grace period, with no lock held: reclaim_seq bumped (seq_cst); every CPU answers
      (HvPerCpu.reclaim_ack) at the top of its dispatcher loop and as it unparks (reclaim_ack): the front cache's
      entries for every zone marked since its last answer gone (HvPerCpu.reclaim_purge, set by the mark: a CPU parked
      through the whole job answers after the zone is filled again, no longer dying), the return tables' salts new,
      the exit it was about to chain and its last block forgotten. It is over once every CPU has answered or is
      parked (active 0). A parked CPU uses nothing of the zone before it unparks, and unparking answers first: it
      stores active (seq_cst) before it reads reclaim_seq, as the reclaimer bumps reclaim_seq before it reads active,
      so either the reclaimer waits for it or it sees the new count (a Dekker pair). After this no CPU is in the
      zone's code or holds its records, and none can get back there (2, 3).
   5. RC_LISTS, in chunks under tr_mu: its blocks off their pages' lists, its own exits off theirs (evict_unhook's
      lists phase). Only after the grace period: until then a CPU could be in the zone's blocks, which a write must
      still gate.
   6. RC_POISON, then the end: its code zeroed (HV_JIT_POISON), its HvJitZone emptied (born the current jit_gen,
      which the reclaimer never bumps: chunk_take, the gen checks and run_loop's chain_gen key on it), and the zone
      ready to fill (zone_ready). Or, for a trim (rc_retire), retired with its tables given back (RC_DROP, in
      pieces under tr_mu, like the other phases: a cancel finds it between two and never waits; what is left goes
      to the canceller's give-back with the rest of what was owed) and the trim's part counted done (rc_owed).
   7. Locks: the reclaimer holds tr_mu a chunk at a time (about RC_CHUNK_TICKS) and checks its job (rc_job) first
      every time it takes it. Every chunk ends with hv_patch_flush (evict_unhook), so no exit's rewrite waits in the
      machine's queue while another holder takes tr_mu. It holds no lock while it waits for the CPUs (rc_holds_tr:
      an abort if it ever does), and kicks them only then: a CPU it waits for may be waiting for tr_mu (a publish, a
      chain to another page, a write to code). It never takes stw_mu and is no CPU: no TLBI or stop waits for it, so
      no cycle of waits can include it. rc_mu, which carries jobs to it, is taken after tr_mu, never before.
   8. Giving up: a stop that needs a zone at once with none ready finishes the reclaimer's zone itself (it has every
      CPU stopped: a grace period of its own) from where the reclaimer got (evict_zone_from), and bumps rc_job; so
      does every cancel (a full flush, a reset, a code-store load, the machine freed), handing over what was owed of
      a trim. The reclaimer sees rc_job changed at its next look and drops the job.
   One job at a time. The zone marked is never the one being filled, one dying, nor the one ready (pick_victim
   leaves zones_dying out), and a trim never retires the ready zone: it retires the reclaimer's zone once empty, or
   marks another for it (retire_one). */
#define RC_CHUNK_TICKS 12000 /* 0.5 ms of 24 MHz ticks: about the longest it holds tr_mu at once */
#define RC_BUDGET 256        /* blocks or slots per evict_unhook call within a chunk */
#define RC_POISON_WORDS (64u << 10)

static __thread int rc_holds_tr; /* this thread (the reclaimer, or a test stepping it) holds tr_mu */

static uint64_t rc_lock(HvMachine *m) {
    pthread_mutex_lock(&m->tr_mu);
    rc_holds_tr = 1;
    return mach_absolute_time();
}

static void rc_unlock(HvMachine *m, uint64_t since) {
    stat_max(&m->stats.reclaim_hold_max_ticks, mach_absolute_time() - since);
    rc_holds_tr = 0;
    pthread_mutex_unlock(&m->tr_mu);
}

static void *reclaimer(void *arg);

/* A job for the reclaimer thread, which is started at the first. Under tr_mu (rc_mu after it). */
static void reclaim_post(HvMachine *m) {
    if (m->reclaim_manual) return;
    pthread_mutex_lock(&m->rc_mu);
    if (!m->rc_started && pthread_create(&m->rc_thread, NULL, reclaimer, m) == 0) m->rc_started = 1;
    pthread_cond_signal(&m->rc_cv); /* none started: the stop that needs the zone empties it itself (8) */
    pthread_mutex_unlock(&m->rc_mu);
}

/* Mark zone v for the reclaimer (invariants 1, 2): in a stop with tr_mu held, or with no CPU running. */
static void reclaim_mark(HvMachine *m, int v, int retire) {
    __atomic_or_fetch(&m->zones_dying, 1u << v, __ATOMIC_SEQ_CST);
    for (int i = 0; i < m->ncpus; i++)
        if (m->cpus[i]) {
            hv_ijc_reset(m->cpus[i], 0);
            hv_ijc_reset(m->cpus[i], 1);
        }
    for (int i = 0; i < HV_MAX_CPUS; i++) __atomic_or_fetch(&m->percpu[i].reclaim_purge, 1u << v, __ATOMIC_RELAXED);
    for (int z = 0; z < HV_JIT_ZONES; z++) m->rc_slots_end[z] = z < m->nzones ? hv_zone_nslots(m, z) : 0; /* 3 */
    m->rc_zone = v, m->rc_phase = RC_HASH, m->rc_retire = retire, m->rc_poison_at = 0, m->rc_wait_seq = 0;
    m->rc_t0 = mach_absolute_time();
    memset(m->unhook_at[v], 0, sizeof m->unhook_at[v]);
    __atomic_add_fetch(&m->rc_job, 1, __ATOMIC_SEQ_CST);
    reclaim_post(m);
}

/* The reclaimer's job given up (8): tr_mu held, every CPU stopped or none running. Its zone is left as far as it
   got (unhook_at, rc_poison_at, still dying) for the caller: finished, or emptied with every other. */
void hv_jit_reclaim_cancel(HvMachine *m) {
    if (m->rc_zone < 0) return;
    int w = m->rc_zone;
    __atomic_add_fetch(&m->rc_job, 1, __ATOMIC_SEQ_CST);
    if (m->rc_owed) { /* the trim's part it owed: the stopper's give-back takes it over (hv_machine_give_back) */
        m->rc_owed = 0;
        if (m->zones_retired >> w & 1) __atomic_or_fetch(&m->giveback_zones, 1u << w, __ATOMIC_SEQ_CST);
        if (__atomic_exchange_n(&m->giveback_pending, 1, __ATOMIC_SEQ_CST)) hv_machine_give_back_done(m, 0); /* counted there already */
    }
    m->rc_zone = -1, m->rc_phase = RC_DONE, m->rc_wait_seq = 0;
    HV_STAT_ADD(m, reclaim_aborts, 1);
}

/* Every CPU has answered seq or is parked (invariant 4). */
static int grace_done(HvMachine *m, uint64_t seq) {
    for (int i = 0; i < HV_MAX_CPUS; i++) {
        HvPerCpu *p = &m->percpu[i];
        if (__atomic_load_n(&p->active, __ATOMIC_SEQ_CST) && __atomic_load_n(&p->reclaim_ack, __ATOMIC_ACQUIRE) < seq) return 0;
    }
    return 1;
}

/* Wait for the grace period, holding no lock (invariant 7). 1: over; 0: the job given up, or the reclaimer told to
   quit; -1 (once: a test stepping it) not yet. */
static int reclaim_grace(HvMachine *m, uint32_t job, uint64_t seq, int once) {
    if (rc_holds_tr) {
        fprintf(stderr, "hengevm: the reclaimer would wait for the CPUs holding tr_mu\n");
        abort();
    }
    uint64_t kicked = 0;
    for (uint32_t spins = 0;; spins++) {
        if (grace_done(m, seq)) return 1;
        if (__atomic_load_n(&m->rc_job, __ATOMIC_SEQ_CST) != job || __atomic_load_n(&m->rc_quit, __ATOMIC_SEQ_CST)) return 0;
        uint64_t now = mach_absolute_time();
        if (!kicked || now - kicked > 24000) { /* each to its dispatcher now, and again every 1 ms */
            hv_machine_kick_all(m);
            HV_STAT_ADD(m, reclaim_kicks, 1);
            kicked = now;
        }
        if (once) return -1;
        if (spins < 16) sched_yield();
        else usleep(50);
    }
}

/* One step of the reclaimer's job: a chunk under tr_mu, or the grace period (once: one look at it). No lock held on
   entry or return. 1 once the job is over (done, given up, or not this one any more). */
static int reclaim_step(HvMachine *m, uint32_t job, int once) {
    uint64_t t0 = rc_lock(m);
    if (__atomic_load_n(&m->rc_job, __ATOMIC_SEQ_CST) != job || m->rc_zone < 0) {
        rc_unlock(m, t0);
        return 1;
    }
    int v = m->rc_zone, over = 0;
    HvJitZone *zn = &m->zones[v];
    switch (m->rc_phase) {
    case RC_HASH:
    case RC_LISTS: {
        int hash = m->rc_phase == RC_HASH, done;
        /* The zone being filled moves only in a stop, under tr_mu: steady while it is held (3) */
        uint32_t fill_slots = m->rc_slots_end[__atomic_load_n(&m->zone, __ATOMIC_RELAXED)];
        while (!(done = evict_unhook(m, v, hash ? HV_UNHOOK_HASH : HV_UNHOOK_LISTS, RC_BUDGET, fill_slots)) &&
               mach_absolute_time() - t0 < RC_CHUNK_TICKS) {
        }
        if (done) {
            uint64_t now = mach_absolute_time();
            HV_STAT_ADD(m, reclaim_ticks[hash ? 0 : 2], now - m->rc_t0);
            m->rc_t0 = now;
            m->rc_phase = hash ? RC_WAIT : RC_POISON;
        }
        break;
    }
    case RC_WAIT: {
        if (!m->rc_wait_seq) m->rc_wait_seq = __atomic_add_fetch(&m->reclaim_seq, 1, __ATOMIC_SEQ_CST);
        uint64_t seq = m->rc_wait_seq;
        rc_unlock(m, t0);
        int r = reclaim_grace(m, job, seq, once);
        if (r <= 0) return r == 0;
        t0 = rc_lock(m);
        if (__atomic_load_n(&m->rc_job, __ATOMIC_SEQ_CST) != job) {
            rc_unlock(m, t0);
            return 1;
        }
        HV_STAT_ADD(m, reclaim_ticks[1], t0 - m->rc_t0);
        stat_max(&m->stats.reclaim_gp_max_ticks, t0 - m->rc_t0);
        m->rc_t0 = t0;
        m->rc_wait_seq = 0;
        m->rc_phase = RC_LISTS;
        break;
    }
    case RC_POISON: {
        if (m->gen_checks)
            while (m->rc_poison_at < zn->used && mach_absolute_time() - t0 < RC_CHUNK_TICKS) {
                size_t n = zn->used - m->rc_poison_at < RC_POISON_WORDS ? zn->used - m->rc_poison_at : RC_POISON_WORDS;
                jit_zero(m, m->jit.base + zone_word0(m, v) + m->rc_poison_at, n);
                m->rc_poison_at += n;
            }
        if (m->gen_checks && m->rc_poison_at < zn->used) break;
        HV_STAT_ADD(m, evicted_blocks, zn->nblocks);
        HV_STAT_ADD(m, zone_evictions, 1);
        memset(zn, 0, sizeof *zn);
        zn->born = __atomic_load_n(&m->jit_gen, __ATOMIC_SEQ_CST); /* not bumped: the stop that fills it does */
        memset(m->unhook_at[v], 0, sizeof m->unhook_at[v]);
        if (m->rc_retire) { /* retired and dying: nothing fills or reads it */
            m->rc_phase = RC_DROP;
            m->rc_drop_k = 0, m->rc_drop_at = 0, m->rc_drop_freed = 0;
            break;
        }
        m->zone_ready = v;
        HV_STAT_ADD(m, reclaim_ticks[2], mach_absolute_time() - m->rc_t0);
        HV_STAT_ADD(m, reclaims, 1);
        m->rc_zone = -1, m->rc_phase = RC_DONE;
        over = 1;
        break;
    }
    case RC_DROP: {
        if (!hv_machine_drop_zone_tables_part(m, v, &m->rc_drop_k, &m->rc_drop_at, &m->rc_drop_freed, t0 + RC_CHUNK_TICKS))
            break;
        if (m->rc_owed) m->rc_owed = 0, hv_machine_give_back_done(m, m->rc_drop_freed);
        HV_STAT_ADD(m, reclaim_ticks[2], mach_absolute_time() - m->rc_t0);
        HV_STAT_ADD(m, reclaims, 1);
        m->rc_zone = -1, m->rc_phase = RC_DONE;
        over = 1;
        break;
    }
    default:
        over = 1;
        break;
    }
    rc_unlock(m, t0);
    return over;
}

static void *reclaimer(void *arg) {
    HvMachine *m = arg;
    pthread_setname_np("hengevm-reclaimer");
    /* As the CPUs it keeps from stopping: a translator may wait for its chunks, and a stop for the zone it readies */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    uint32_t seen = 0;
    pthread_mutex_lock(&m->rc_mu);
    for (;;) {
        uint32_t job = 0;
        while (!m->rc_quit && (job = __atomic_load_n(&m->rc_job, __ATOMIC_SEQ_CST)) == seen) pthread_cond_wait(&m->rc_cv, &m->rc_mu);
        if (m->rc_quit) break;
        seen = job;
        pthread_mutex_unlock(&m->rc_mu);
        while (!reclaim_step(m, job, 0) && !__atomic_load_n(&m->rc_quit, __ATOMIC_SEQ_CST)) {
        }
        pthread_mutex_lock(&m->rc_mu);
    }
    pthread_mutex_unlock(&m->rc_mu);
    return NULL;
}

void hv_jit_reclaim_stop(HvMachine *m) {
    pthread_mutex_lock(&m->tr_mu);
    hv_jit_reclaim_cancel(m);
    pthread_mutex_unlock(&m->tr_mu);
    if (!m->rc_started) return;
    pthread_mutex_lock(&m->rc_mu);
    m->rc_quit = 1;
    pthread_cond_signal(&m->rc_cv);
    pthread_mutex_unlock(&m->rc_mu);
    pthread_join(m->rc_thread, NULL);
    m->rc_started = 0, m->rc_quit = 0;
}

void hv_jit_reclaim_start(HvMachine *m, int v) {
    pthread_mutex_lock(&m->tr_mu);
    if (m->rc_zone < 0 && m->zone_ready < 0 && v >= 0 && v < m->nzones && v != m->zone && !((m->zones_dying | m->zones_retired) >> v & 1)) {
        zone_save(m);
        reclaim_mark(m, v, 0);
    }
    pthread_mutex_unlock(&m->tr_mu);
}

int hv_jit_reclaim_step(HvMachine *m, uint32_t job) { return reclaim_step(m, job, 1); }

/* A CPU answers the reclaimer (invariant 4), on its own thread at its dispatcher. */
static __attribute__((noinline, cold)) void reclaim_ack(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    uint64_t seq = __atomic_load_n(&m->reclaim_seq, __ATOMIC_SEQ_CST); /* and with it what the hash phase did */
    jc_purge(m, p, __atomic_exchange_n(&p->reclaim_purge, 0, __ATOMIC_RELAXED)); /* every zone marked since its last answer */
    hv_ret_forget(cpu);
#if defined(__aarch64__)
    __asm__ volatile("isb" ::: "memory"); /* the exits the hash phase patched back to their stubs, fetched afresh */
#endif
    __atomic_store_n(&p->reclaim_ack, seq, __ATOMIC_RELEASE);
}

/* Mark the next zone now? Under tr_mu, a translation just published past the zone being filled's early mark
   (early_*): not if a zone is ready, being emptied or empty already, or it was asked under this generation.
   1: asked (flush_pending set): the caller kicks the CPUs once it has let tr_mu go. */
static __attribute__((noinline, cold)) int reclaim_early_ask(HvMachine *m) {
    if (m->reclaim_off || m->nzones <= 1 || m->rc_zone >= 0 || m->zone_ready >= 0) return 0;
    uint32_t want = __atomic_load_n(&m->jit_gen, __ATOMIC_SEQ_CST) + 1;
    if (m->room_early == want || free_zone(m) >= 0) return 0;
    m->room_early = want;
    __atomic_store_n(&m->flush_pending, 1, __ATOMIC_SEQ_CST);
    return 1;
}

static inline int reclaim_early_due(const HvMachine *m) {
    return (uint32_t)m->nblocks >= m->early_blocks || __atomic_load_n(&m->nslots, __ATOMIC_RELAXED) >= m->early_slots ||
           __atomic_load_n(&m->jit.used, __ATOMIC_RELAXED) >= m->early_words;
}

/* Where the zone being filled is reclaim_early_pct full: past it, the next zone is marked for the reclaimer. */
static void early_marks(HvMachine *m, int z) {
    int pct = m->reclaim_early_pct > 0 && m->reclaim_early_pct <= 100 ? m->reclaim_early_pct : 75;
    if (m->reclaim_off || m->nzones <= 1) {
        m->early_blocks = m->early_slots = UINT32_MAX, m->early_words = SIZE_MAX;
        return;
    }
    m->early_blocks = (uint32_t)z * m->zone_blocks + (uint32_t)((uint64_t)m->zone_blocks * (unsigned)pct / 100);
    m->early_slots = (uint32_t)z * m->zone_slots + (uint32_t)((uint64_t)m->zone_slots * (unsigned)pct / 100);
    m->early_words = zone_word0(m, z) + m->zone_words / 100 * (size_t)pct;
}

/* The stop room_early asked for: mark the next zone, if it is still wanted. */
static void reclaim_mark_next(HvMachine *m) {
    m->room_early_seen = m->room_early;
    if (m->reclaim_off || m->nzones <= 1 || m->rc_zone >= 0 || m->zone_ready >= 0 || free_zone(m) >= 0) return;
    uint64_t t0 = mach_absolute_time();
    zone_save(m); /* pick_victim reads the zones' counts */
    int v = pick_victim(m, m->zone, 0);
    if (v < 0) return;
    reclaim_mark(m, v, 0);
    HV_STAT_ADD(m, reclaim_marks, 1);
    uint64_t took = mach_absolute_time() - t0;
    stat_max(&m->stats.quick_stop_max_ticks, took);
    HV_STAT_ADD(m, quick_stops, 1);
    HV_STAT_ADD(m, quick_stop_ticks, took);
}

void hv_jit_reclaim_kick(HvMachine *m) {
    pthread_mutex_lock(&m->tr_mu);
    early_marks(m, m->zone);
    if (!m->reclaim_off && m->nzones > 1 && m->rc_zone < 0 && m->zone_ready < 0 && free_zone(m) < 0 && reclaim_early_due(m)) {
        zone_save(m);
        int v = pick_victim(m, m->zone, 0);
        if (v >= 0) reclaim_mark(m, v, 0);
    }
    pthread_mutex_unlock(&m->tr_mu);
}

/* A trim that retires a zone (make_room_from): the reclaimer's own if it is emptying one (retired once it is empty),
   else the one a trim would retire (pick_victim, held), marked for it; never the ready zone, filled next. Its tables
   go back once it is empty: a part of the trim (rc_owed). HV_EVICT_SYNC: emptied here, as before. */
static void retire_one(HvMachine *m, int cur) {
    uint64_t t = mach_absolute_time();
    int w = m->rc_zone >= 0 ? m->rc_zone : pick_victim(m, cur, 1);
    if (w < 0) {
        note_give_back(m, 0);
    } else if (m->reclaim_off) {
        evict_zone(m, w); /* empty or not: its tables go back, and the zone emptied next to fill is another */
        m->zones_retired |= 1u << w;
        HV_STAT_ADD(m, zone_evictions, 1);
        note_give_back(m, 1u << w);
    } else {
        note_give_back(m, 0); /* the CPUs' branch caches, as before */
        if (m->rc_zone != w) reclaim_mark(m, w, 1);
        else m->rc_retire = 1;
        m->zones_retired |= 1u << w;
        if (!m->rc_owed) m->rc_owed = 1, __atomic_add_fetch(&m->giveback_left, 1, __ATOMIC_SEQ_CST);
    }
    if (w >= 0) HV_STAT_ADD(m, zones_retired, 1);
    HV_STAT_ADD(m, trim_stop_ticks, mach_absolute_time() - t);
}

/* The cache is full: fill another zone. The one the reclaimer emptied meanwhile (zone_ready), at once. With none
   ready, one emptied here with every CPU stopped, as before (evict_sync): the reclaimer's if it is part way through
   one (finished from where it got, the job given up), else pick_victim's.
   Under a trim (a memory warning, taken here) with more zones in use than zones_floor, one is retired too, its
   tables given back once it is empty (retire_one): otherwise the cache would fill back to where it was, and the
   memory be held again within seconds (mem-codecache #2). At the floor a trim gives back only the branch caches.
   With no warning for trim_quiet_ns, a retired zone (empty since) is filled instead: the cache grows back to its
   size once the pressure has gone. victim >= 0 (tests): that zone is filled, emptied here unless it is ready. */
static void make_room_from(HvMachine *m, int victim) {
    int trim = __atomic_exchange_n(&m->trim_pending, 0, __ATOMIC_SEQ_CST);
    if (m->nzones <= 1) {
        flush_blocks(m, trim);
        return;
    }
    uint64_t t0 = mach_absolute_time();
    zone_save(m);
    int cur = m->zone;
    m->zones[cur].filled = ++m->zone_switches;
    for (int z = 0; z < m->nzones; z++) {
        uint64_t hits = 0;
        for (int i = 0; i < m->ncpus; i++) hits += m->percpu[i].zone_hits[z], m->percpu[i].zone_hits[z] = 0;
        m->zones[z].score = m->zones[z].score / 2 + hits;
    }
    if (trim && zones_active(m) > zones_floor(m)) retire_one(m, cur);
    else if (trim) note_give_back(m, 0);
    int v;
    uint32_t back = m->zones_retired & ~(m->rc_zone >= 0 ? 1u << m->rc_zone : 0u);
    if (victim >= 0 && victim < m->nzones && victim != cur && !(m->zones_retired >> victim & 1)) {
        v = victim;
    } else if (!trim && victim < 0 && back && trim_quiet(m)) {
        v = __builtin_ctz(back);
        m->zones_retired &= ~(1u << v);
        m->giveback_zones &= ~(1u << v);
        HV_STAT_ADD(m, zones_returned, 1); /* empty since it was retired */
    } else if (m->zone_ready >= 0) {
        v = m->zone_ready;
    } else if (m->rc_zone >= 0 && !m->rc_retire) {
        v = m->rc_zone;
    } else {
        v = pick_victim(m, cur, 0);
    }
    if (v < 0) { /* no zone but the one being filled (not expected: the floor keeps two): start over */
        fprintf(stderr, "hengevm: no zone to empty (zone %d of %d, retired %#x): flushing\n", cur, m->nzones, m->zones_retired);
        flush_blocks(m, 0);
        return;
    }
    int ready = v == m->zone_ready;
    if (ready) {
        m->zone_ready = -1; /* the reclaimer emptied it: nothing to do here */
    } else {
        int from = RC_HASH;
        if (v == m->rc_zone) { /* part way: finished here from where it got, the job given up */
            from = m->rc_phase;
            hv_jit_reclaim_cancel(m);
        }
        const HvJitZone *zn = &m->zones[v];
        if (zn->nblocks || zn->used || zn->nslots || zn->nundef || from != RC_HASH) HV_STAT_ADD(m, evict_sync, 1);
        evict_zone_from(m, v, from);
        HV_STAT_ADD(m, zone_evictions, 1);
    }
    __atomic_and_fetch(&m->zones_dying, ~(1u << v), __ATOMIC_SEQ_CST);
    m->zones[v].born = __atomic_add_fetch(&m->jit_gen, 1, __ATOMIC_SEQ_CST); /* each thread's code chunk is in the old zone */
    zone_load(m, v);
    uint64_t took = mach_absolute_time() - t0;
    HV_STAT_ADD(m, evict_ticks, took);
    if (ready) {
        stat_max(&m->stats.quick_stop_max_ticks, took);
        HV_STAT_ADD(m, quick_stops, 1);
        HV_STAT_ADD(m, quick_stop_ticks, took);
    }
}

static void make_room(HvMachine *m) { make_room_from(m, -1); }

/* No CPU runs, but the reclaimer may: tr_mu, as a stop holds it. */
void hv_jit_make_room(HvMachine *m, int victim) {
    pthread_mutex_lock(&m->tr_mu);
    make_room_from(m, victim);
    pthread_mutex_unlock(&m->tr_mu);
    hv_machine_give_back(m, NULL);
}

void hv_flush_blocks(HvMachine *m) {
    pthread_mutex_lock(&m->tr_mu);
    flush_blocks(m, __atomic_exchange_n(&m->trim_pending, 0, __ATOMIC_SEQ_CST));
    pthread_mutex_unlock(&m->tr_mu);
    hv_machine_give_back(m, NULL);
}

/* Every block goes. trim: a memory warning taken by the caller. */
static void flush_blocks(HvMachine *m, int trim) {
    hv_jit_reclaim_cancel(m); /* before anything its zone is on is cleared (hv_jit_zones_reset's is a no-op then) */
    /* Every other CPU is parked (stop_the_world): the one moment the TLB layout can change. */
    if ((hv_knob[HV_K_TLBT] != 0) != hv_tlb_tmajor) {
        hv_tlb_tmajor = hv_knob[HV_K_TLBT] != 0;
        for (int i = 0; i < m->ncpus; i++)
            if (m->cpus[i]) {
                m->cpus[i]->nkernel_fills = HV_KERNEL_FILLS + 1; /* the logs hold slots of the old layout: clear it all */
                hv_tlb_flush(m->cpus[i]);
            }
    }
    HV_STAT_ADD(m, block_flushes, 1);
    __atomic_add_fetch(&m->jit_gen, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < m->ncpus; i++)
        if (m->cpus[i]) {
            hv_ijc_reset(m->cpus[i], 0);
            hv_ijc_reset(m->cpus[i], 1);
            hv_ret_clear(m->cpus[i]); /* they point at code that is going */
            hv_jc_clear(&m->percpu[i]); /* the parts lookups wrote: a CPU that never ran has none */
            __atomic_store_n(&m->percpu[i].reclaim_purge, 0, __ATOMIC_RELAXED); /* nothing left in it to take out */
        }
    for (int i = 0; i < HV_MAX_CPUS; i++) memset(m->percpu[i].zone_hits, 0, sizeof m->percpu[i].zone_hits);
    _Static_assert(HV_BLOCK_HASH == 1 << 21, "the HV_PART size at the hash's inserts");
    hv_parts_clear(m->hash, sizeof m->hash, &m->hash_used); /* 8 MB: only the parts inserts wrote */
    zone_save(m); /* the high-water marks: the other zones' were taken as they stopped being filled */
    m->nxchained = 0;
    /* Every slot is gone, so every page's list of them, and every block: the heads cleared. The slots' and blocks'
       own links are left as they were: nothing leads to them, and each is set again as its slot or record is used. */
    for (int i = 0; i < m->nram; i++) {
        HvRam *r = &m->ram[i];
        size_t pages = (r->size + (1ull << HV_RAM_PAGE_BITS) - 1) >> HV_RAM_PAGE_BITS;
        memset(r->chain_heads, 0, pages * sizeof *r->chain_heads);
        memset(r->block_heads, 0, pages * sizeof *r->block_heads);
    }
    hv_jit_reset(&m->jit);
    hv_jit_zones_reset(m);
    if (trim) {
        uint64_t t = mach_absolute_time();
        if (m->nzones <= 1) {
            hv_machine_trim_tables(m); /* one zone: filled again from now, so given back here */
        } else {
            /* As make_room_from: one zone fewer from now on, or the cache fills back to where it was. Every zone is
               empty: all but the one being filled give their tables back once the CPUs run again. */
            if (zones_active(m) > zones_floor(m)) {
                int z = m->nzones - 1;
                while (z == m->zone || m->zones_retired >> z & 1) z--;
                m->zones_retired |= 1u << z;
                HV_STAT_ADD(m, zones_retired, 1);
            }
            note_give_back(m, ((1u << m->nzones) - 1) & ~(1u << m->zone));
        }
        HV_STAT_ADD(m, trim_stop_ticks, mach_absolute_time() - t);
    }
    /* HV_JIT_POISON: nothing old is left to run, so a stale pointer faults where it is used. */
    static int poison = -1;
    if (poison < 0) poison = getenv("HV_JIT_POISON") != NULL;
    m->gen_checks = poison;
    if (poison) {
        jit_zero(m, m->jit.base, m->jit.capacity);
    }
}

/* Parked: not running guest code, nor using its TLB, so other CPUs need not
   wait for it (TLBIs, a flush). It catches up with requests on the way back. */
/* gen_checks: the block this CPU's lookup returned, until run_loop has chained to it (or not): park checks the rule
   that it never parks in between (run_loop, at the chain). */
static __thread const HvBlock *found_unchained;

static void park(HvCpu *cpu) {
    HvPerCpu *p = cpu->pc_state;
    if (__builtin_expect(found_unchained != NULL, 0)) {
        fprintf(stderr, "hengevm: cpu %u parks between its lookup of %llx and the chain to it\n", cpu->index,
                (unsigned long long)found_unchained->va);
        abort();
    }
    __atomic_store_n(&p->tlb_live, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&p->active, 0, __ATOMIC_SEQ_CST);
    hv_cpu_ack_wake(cpu); /* a CPU asleep waiting for this one's acknowledgement need not wait any more */
}

static void unpark(HvCpu *cpu) {
    HvPerCpu *p = cpu->pc_state;
    HvMachine *m = cpu->m;
    /* A flush rewrites every CPU's branch caches and their salts, taking "parked"
       to mean nobody is using them. So not back while one runs: hv_cpu_process_requests
       below may reach them, and a salt read before the flush and stored after it
       brings the entries of dropped translations back to life. */
    for (;;) {
        __atomic_store_n(&p->active, 1, __ATOMIC_SEQ_CST);
        if (!__atomic_load_n(&m->stw_active, __ATOMIC_SEQ_CST)) break;
        __atomic_store_n(&p->active, 0, __ATOMIC_SEQ_CST);
        pthread_mutex_lock(&m->stw_mu);
        while (m->stw_active) pthread_cond_wait(&m->stw_cv, &m->stw_mu);
        pthread_mutex_unlock(&m->stw_mu);
    }
    /* The stopper may be giving its branch caches back for it (hv_machine_give_back): not used before that is done. */
    while (__atomic_load_n(&p->ijc_dropping, __ATOMIC_SEQ_CST)) sched_yield();
    /* The reclaimer's grace period passed it while parked: answered before anything is used (active, stored above,
       then reclaim_seq read: the reclaimer's protocol, invariant 4). */
    if (__atomic_load_n(&m->reclaim_seq, __ATOMIC_SEQ_CST) != __atomic_load_n(&p->reclaim_ack, __ATOMIC_RELAXED)) reclaim_ack(cpu);
    /* About to run for the first time since its reset (or since an off CPU gave its TLB back): its TLB all invalid.
       Every way to guest code comes through here once off is clear; an off CPU's TLB stays untouched. */
    if (!__atomic_load_n(&cpu->off, __ATOMIC_ACQUIRE) && !hv_tlb_is_ready(cpu)) hv_tlb_ready(cpu);
    __atomic_store_n(&p->tlb_live, 1, __ATOMIC_SEQ_CST);
    hv_cpu_process_requests(cpu); /* what came in while it was parked */
    /* A trim while it was parked: its branch caches back to the system, on its own thread (they were emptied). */
    if (__atomic_load_n(&p->ijc_giveback, __ATOMIC_SEQ_CST) && __atomic_exchange_n(&p->ijc_giveback, 0, __ATOMIC_SEQ_CST)) {
        uint64_t t0 = mach_absolute_time(), freed = hv_cpu_ijc_give_back(p);
        HV_STAT_ADD(m, giveback_ticks, mach_absolute_time() - t0);
        hv_machine_give_back_done(m, freed);
    }
}

/* HV_BIG_LOCK: a CPU waiting for the lock runs no guest code, and the one holding
   it may be waiting for it (hv_cpu_sync after a TLBI or protecting a code page, or
   a stop-the-world flush): so it waits parked. It cannot then miss a flush on the
   way back: only a CPU holding the lock starts one, and it does not let go meanwhile. */
static void big_lock(HvCpu *cpu) {
    if (!cpu->m->big_lock_on) return;
    park(cpu);
    hv_big_lock(cpu->m);
    unpark(cpu);
}

static uint32_t block_flags(HvCpu *cpu) {
    uint32_t flags = cpu->el ? HV_BLOCK_EL1 : 0;
    if (cpu->el && cpu->spsel) flags |= HV_BLOCK_SPX;
    if (cpu->el && (cpu->pstate_extra & HV_PSTATE_PAN)) flags |= HV_BLOCK_PAN;
    uint32_t fpen = (cpu->cpacr_el1 >> 20) & 3;
    if (fpen == 0 || fpen == 2 || (fpen == 1 && cpu->el == 0)) flags |= HV_BLOCK_FPOFF;
    if (hv_code_page_bits(cpu, cpu->pc) < HV_TLB_PAGE_BITS) flags |= HV_BLOCK_SMALL;
    if (hv_tso_thread(cpu)) flags |= HV_BLOCK_TSO;
    return flags;
}


/* Finding a block is lock-free (blocks are only ever added, with a release
   store, until a flush, which stops every CPU); making one takes the
   translation lock. Returns NULL if the translations must all be dropped first
   (m->flush_pending is set: the dispatcher does that, then tries again). */
#define hash_of hv_block_hash
#define find hv_block_find
/* HV_BLOCK_DUMP=file: every 64th (HV_BLOCK_DUMP_EVERY) translated block (guest and host words) for
   bench/hvdis.py, to see what the translator makes of real code. */
static void block_dump(const HvBlock *b, const uint32_t *code) {
    static FILE *f;
    static int on = -1;
    static uint32_t count, every = 64; /* HV_BLOCK_DUMP_EVERY: the sampling */
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    if (on < 0) {
        const char *path = getenv("HV_BLOCK_DUMP");
        on = path && (f = fopen(path, "wb")) != NULL;
        if (getenv("HV_BLOCK_DUMP_EVERY")) every = (uint32_t)atoi(getenv("HV_BLOCK_DUMP_EVERY"));
        if (!every) every = 1;
    }
    if (!on || __atomic_add_fetch(&count, 1, __ATOMIC_RELAXED) % every) return;
    uint64_t head[3] = {b->va, b->ninsns, 0};
    const uint32_t *w = b->code;
    uint32_t n = hv_translate_last_words();
    head[2] = n;
    pthread_mutex_lock(&mu);
    fwrite(head, 8, 3, f);
    fwrite(code, 4, b->ninsns, f);
    fwrite(w, 4, n, f);
    fflush(f);
    pthread_mutex_unlock(&mu);
}

/* Writes to a page we translate from must take the slow path, so we notice
   them: protected before its code is read, so that any write after that (or
   the IC IVAU after it) invalidates what we make. code_pages[pg]:
     0                     no translated code;
     HV_CODE_PROTECTED     no CPU can write it without noticing;
     HV_CODE_MARKED_BY(i)  CPU i is protecting it: writes take the slow path from
                           now on, but another CPU may still map the page writable
                           or have a store to it in flight, so its code must not be
                           read yet;
     HV_CODE_RETIRING      a write is retiring its blocks (hv_invalidate_code): wait.
   The mark names its CPU because a write may clear it (hv_invalidate_code) while
   CPU i waits, and another CPU mark it again: i must not then finish what that
   one started. Everything else treats any nonzero mark as code. Returns the
   generation to stamp the block with. */
#define HV_CODE_PROTECTED 1
#define HV_CODE_MARKED_BY(i) (2 + (i))
_Static_assert(HV_CODE_MARKED_BY(HV_MAX_CPUS - 1) < HV_CODE_RETIRING, "code_pages holds a byte, RETIRING its top value");
static uint32_t protect_code_page(HvCpu *cpu, HvRam *ram, uint64_t pg) {
    HvMachine *m = cpu->m;
    uint8_t *mark = &ram->code_pages[pg];
    for (uint32_t spins = 0;; spins++) {
        uint8_t v = __atomic_load_n(mark, __ATOMIC_ACQUIRE);
        if (v == HV_CODE_PROTECTED) {
            uint32_t gen = __atomic_load_n(&ram->page_gen[pg], __ATOMIC_ACQUIRE);
            /* hv_invalidate_code clears the mark before it moves the generation:
               still protected after reading gen, the page was protected under it. */
            if (__atomic_load_n(mark, __ATOMIC_ACQUIRE) == HV_CODE_PROTECTED) return gen;
            continue;
        }
        if (v == 0) {
            uint8_t zero = 0, mine = (uint8_t)HV_CODE_MARKED_BY(cpu->index);
            /* Before the TLBs: see the end of tlb_fill (write_guard in mmu.c). */
            if (!__atomic_compare_exchange_n(mark, &zero, mine, 0, __ATOMIC_SEQ_CST, __ATOMIC_ACQUIRE)) continue;
            HV_CPU_COUNT(cpu, code_protects, 1);
            /* In every CPU's TLB, or a write from another CPU goes unnoticed. A CPU
               whose entry we dropped may be between its TLB check and the store
               itself (a few instructions, or preempted there): that store would land
               after we read the code yet never take the slow path, leaving the old
               code translated for good (IC IVAU does nothing, see sysreg.c). So wait
               until each such CPU has left its block, which it does at once on the
               kick; only CPUs that really mapped the page writable are waited for. */
            uint64_t seq[HV_MAX_CPUS];
            uint32_t wait = 0;
            for (int i = 0; i < m->ncpus; i++)
                if (m->cpus[i] && hv_tlb_protect_code_page(m->cpus[i], ram, pg) && m->cpus[i] != cpu) {
                    seq[i] = hv_cpu_post(m->cpus[i], 0);
                    wait |= 1u << i;
                }
            for (int i = 0; i < m->ncpus; i++)
                if (wait >> i & 1) hv_cpu_sync(cpu, m->cpus[i], seq[i]);
            if (wait) HV_CPU_COUNT(cpu, code_protect_waits, 1);
            /* No write can land unseen now: its bytes, for HvStats.rewrite_* when a write drops its code (machine.c
               invalidate_page; one meanwhile takes the hash away again). */
            if (__atomic_load_n(&m->rewrite_hash, __ATOMIC_RELAXED))
                __atomic_store_n(&ram->code_hash[pg], hv_page_hash(ram, pg), __ATOMIC_RELAXED);
            /* A write meanwhile leaves our mark in place (hv_invalidate_code clears only
               a finished one) and moves the generation, which the loop reads next. If the
               mark is somehow not ours any more, over again. */
            __atomic_compare_exchange_n(mark, &mine, HV_CODE_PROTECTED, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
            continue;
        }
        /* Another CPU is protecting it. It may be waiting for us to leave a block
           (we have: do what it asks), so do that while waiting. */
        hv_cpu_process_requests(cpu);
        if (spins > 100) sched_yield();
    }
}

/* *once: the block was made from bytes a write changed meanwhile. It is not kept (nothing finds it, its entry is
   gated): this one run starts past its entry, at word 1. So a CPU still gets on while another keeps writing its
   code, as with a real pipeline that fetched the old instructions. */
static HvBlock *lookup(HvCpu *cpu, uint64_t va, uint64_t pa, uint32_t flags, const uint32_t *code, uint32_t avail, int *once) {
    HvMachine *m = cpu->m;
    *once = 0;
    uint32_t j = (uint32_t)((va >> 2) ^ (va >> 18) ^ ((uint64_t)flags << 13)) & ((1u << HV_JC_BITS) - 1);
    __typeof__(cpu->pc_state->jc[0]) *jc = cpu->pc_state->jc;
    HvBlock *c = jc[j].va == va ? jc[j].b : NULL;
    if (c && c->va == va && !hv_block_dying(m, c) && c->pa == pa && c->flags == flags && c->ram->page_gen[c->pg] == c->gen) {
        cpu->pc_state->zone_hits[c->zone]++; /* which zones are in use (make_room) */
        hv_block_entered(c);
        return c;
    }
    uint32_t h = hash_of(va, pa, flags);
    HvRam *ram = hv_ram_lookup(m, pa);
    uint64_t pg = (pa - ram->pa) >> HV_RAM_PAGE_BITS;
    uint32_t gen = __atomic_load_n(&ram->page_gen[pg], __ATOMIC_ACQUIRE);
    HvBlock *b = find(m, h, va, pa, flags, gen);
    if (b) {
        jc[j].va = va, jc[j].b = b, cpu->pc_state->jc_used |= HV_PART(j, HV_JC_BITS); /* plain: HvPerCpu.jc_used */
        cpu->pc_state->zone_hits[b->zone]++;
        hv_block_entered(b);
        return b;
    }

    /* Only a shortcut: a stop asked meanwhile is seen with full ordering where a failed lookup returns (run_loop). */
    if (__atomic_load_n(&m->flush_pending, __ATOMIC_RELAXED)) goto flush_unlocked;
    static __thread uint32_t copy[HV_MAX_BLOCK_INSNS];
    uint32_t ncopy = avail < HV_MAX_BLOCK_INSNS ? avail : HV_MAX_BLOCK_INSNS;
    gen = protect_code_page(cpu, ram, pg);
    /* The mark, then the bytes, with a full fence between: the other half of the rule
       in written() (machine.c: hv_phys_written, hv_dma_written), so a write made through C either sees the mark or
       is in what we read. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    /* Translate without the lock, from a copy of the guest's words, into this thread's buffer... */
    memcpy(copy, code, ncopy * 4);
    HvBlock nb = {.va = va, .pa = pa, .flags = flags, .gen = gen, .ram = ram, .pg = (uint32_t)pg, .maker = (uint8_t)(cpu->index + 1), .jit_gen = __atomic_load_n(&m->jit_gen, __ATOMIC_SEQ_CST)};
    uint64_t t0 = m->stats_interval ? mach_absolute_time() : 0;
    if (hv_translate_emit(m, &nb, copy, ncopy) != 0) return NULL; /* too big for the buffer, however often it is tried:
                                                                    the dispatcher says it cannot translate it */
    nb.nosave = (uint8_t)hv_translate_last_nosave();
    /* ...note the pieces of the page it came from, then check they still hold what it was made from: a write that
       came between either sees a piece noted and drops the page's code (hv_phys_written), or is seen here. */
    int changed = 0;
    {
        uint32_t nread = hv_translate_last_read();
        if (nread) {
            __atomic_fetch_or(&ram->code_mask[pg], hv_code_pieces((pa - ram->pa) & ((1ull << HV_RAM_PAGE_BITS) - 1), 4ull * nread), __ATOMIC_SEQ_CST);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            changed = memcmp(copy, code, 4ull * nread) != 0;
        }
    }
    if (m->stats_interval) {
        HV_CPU_COUNT(cpu, translate_ticks, mach_absolute_time() - t0);
        HV_CPU_COUNT(cpu, translated_insns, nb.ninsns);
    }
    /* ...and place it in the region (space is reserved atomically, so no lock either)... */
    if (hv_translate_place(m, &nb) != 0) { /* the zone being filled is full: make room */
        pthread_mutex_lock(&m->tr_mu);
        goto full;
    }
    if (m->gen_checks && nb.jit_gen != __atomic_load_n(&m->jit_gen, __ATOMIC_SEQ_CST))
        fprintf(stderr, "hengevm: cpu %u translated across a flush: gen %u to %u, placed at rx+%llx, used %llx, active %d\n", cpu->index, nb.jit_gen, m->jit_gen, (unsigned long long)(nb.code - m->jit.base) * 4, (unsigned long long)__atomic_load_n(&m->jit.used, __ATOMIC_RELAXED) * 4, cpu->pc_state->active);
    block_dump(&nb, copy);
    /* ...then take the lock only to publish it. */
    if (pthread_mutex_trylock(&m->tr_mu) != 0) {
        uint64_t w0 = mach_absolute_time(); /* always: the freeze records show who waits for the translation lock */
        pthread_mutex_lock(&m->tr_mu);
        HV_CPU_COUNT(cpu, tr_waits, 1);
        HV_CPU_COUNT(cpu, tr_wait_ticks, mach_absolute_time() - w0);
    }
    b = find(m, h, va, pa, flags, gen); /* another CPU may have just made it: ours is dropped (its space stays used until a flush) */
    if (b) {
        pthread_mutex_unlock(&m->tr_mu);
        HV_CPU_COUNT(cpu, lost_races, 1);
        hv_block_entered(b);
        jc[j].va = va, jc[j].b = b, cpu->pc_state->jc_used |= HV_PART(j, HV_JC_BITS); /* plain: HvPerCpu.jc_used */
        return b;
    }
    if (__atomic_load_n(&m->flush_pending, __ATOMIC_RELAXED)) goto flush; /* a shortcut, as above */
    /* The bytes changed under it, or its page's code did since it was protected (that write may have retired the
       page's blocks already, before this one was on the list): run once. */
    if (__atomic_load_n(&ram->page_gen[pg], __ATOMIC_SEQ_CST) != nb.gen) changed = 1;
    if ((uint32_t)m->nblocks >= m->block_limit) goto full;
    /* Where its copied FP/SIMD words are, for hv_jit_sigill. */
    uint32_t nu;
    const uint32_t *u = hv_translate_undef_map(&nu);
    nb.nundef = nu;
    if (nu == HV_UNDEF_UNKNOWN || nu > m->undef_limit - m->nundef) nb.nundef = HV_UNDEF_UNKNOWN;
    else if (nu) {
        memcpy(m->undef_map + m->nundef, u, nu * sizeof *u);
        nb.undef_at = m->nundef;
        m->nundef += nu;
    }
    nb.zone = (uint8_t)m->zone;
    if (changed) {
        b = &m->blocks[m->nblocks++]; /* the record stays, for hv_jit_sigill */
        *b = nb;
        b->dead = 1;
        m->zones[b->zone].ndead++;
        m->zones[b->zone].dead_words += b->words;
        HV_CPU_COUNT(cpu, tr_once, 1);
        hv_block_gate(m, b); /* its exits chained back to its own start (hv_translate_place) leave there */
        hv_patch_flush(m);
        pthread_mutex_unlock(&m->tr_mu);
        *once = 1;
        return b;
    }
    b = &m->blocks[m->nblocks];
    *b = nb;
    b->entries = 1; /* this one, as it is made */
    hv_page_list_insert(m, ram, (uint32_t)pg, b);
    HV_CPU_COUNT(cpu, translations, 1);
    if (m->stats_interval) HV_CPU_COUNT(cpu, translated_words, b->words); /* this block's own (other CPUs reserve from the region meanwhile) */
    /* Was this code thrown out before: by emptying its zone with its page unchanged since (the cache is too small
       for what runs), or by a write to its page (HV_GHOST_*)? Always: one 16-byte set read under the lock already held. */
    int again = hv_ghost_take(m, va, pa, flags, gen);
    if (again == HV_GHOST_EVICTED) HV_CPU_COUNT(cpu, retrans_evicted, 1);
    else if (again) HV_CPU_COUNT(cpu, retrans_written, 1); /* retired, or emptied and its page written since */
    m->nblocks++;
    hv_hash_insert(m, b, h);
    __atomic_or_fetch(&m->hash_used, HV_PART(h, 21), __ATOMIC_RELAXED); /* atomic: the stats sample reads it */
    /* Past the zone's early mark: the next zone marked for the reclaimer now, so it is empty when this one is full */
    int early = reclaim_early_due(m) && reclaim_early_ask(m);
    pthread_mutex_unlock(&m->tr_mu);
    if (early) hv_machine_kick_all(m);
    return b;
full:
    /* Room in the cache for this generation of it: once, however many CPUs find it full (a CPU that finds it
       once more after the room is made sees a newer generation, and tries again). */
    if (nb.jit_gen == __atomic_load_n(&m->jit_gen, __ATOMIC_SEQ_CST)) {
        if (m->room_gen != nb.jit_gen + 1) HV_CPU_COUNT(cpu, flush_full, 1);
        m->room_gen = nb.jit_gen + 1;
    }
    goto flush;
flush_unlocked:
    pthread_mutex_lock(&m->tr_mu);
flush:
    __atomic_store_n(&m->flush_pending, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&m->tr_mu);
    hv_machine_kick_all(m);
    return NULL;
}

/* WFE. xnu's spin loops arm the exclusive monitor on the lock word (LDAXR)
   and wait for it to change; leaving for the dispatcher every time made a
   waiting CPU spin through it (a hundred thousand times a second). Wait here
   instead: the host core sleeps in its own WFE on the host copy of the word,
   which wakes when another core writes it (or at the event stream, about every
   microsecond). A request for this CPU (an interrupt, a TLBI) ends the wait, and
   so does a short timeout, so its timer and devices are seen to. Without a
   monitor to watch it leaves at once, as before. HV_WFE_SPIN: always that.
   The word's owner is another vCPU, maybe one the host has taken off its core: waits that keep timing out on
   the same word (WFE_YIELD_ROUNDS of them, about 200 us) give this core up for a moment, so the host can run the
   owner (lock-holder pre-emption). The priority is lowered only while off the core, for at most 1 ms. */
enum { WFE_YIELD_ROUNDS = 4 };

static inline uint64_t wfi_timer_now(void) {
#if defined(__aarch64__)
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    return mach_absolute_time();
#endif
}

static uint32_t wfe(HvCpu *cpu) {
    static int spin = -1;
    if (spin < 0) spin = getenv("HV_WFE_SPIN") != NULL;
    uint64_t va = cpu->excl_addr;
    uint32_t size = cpu->excl_size;
    unsigned bits;
    int table = hv_tlb_where(cpu, va, cpu->el, &bits);
    const HvTlbEntry *e = hv_tlb_at(cpu, va, table, bits);
    uint64_t page = va & ~((1ull << bits) - 1);
    if (spin || va == ~0ull || (size != 1 && size != 2 && size != 4 && size != 8) || (va & (size - 1)) ||
        e->read_tag == HV_TLB_INVALID || e->read_tag + HV_TLB_ADJ != page) {
        __atomic_store_n(&cpu->exit_req, 1, __ATOMIC_RELAXED); /* let another CPU run: the dispatcher's way */
        return 0;
    }
    const void *host = (const void *)(uintptr_t)(va + (uint64_t)e->addend);
    uint64_t want = cpu->excl_val[0] & (size == 8 ? ~0ull : (1ull << (8 * size)) - 1);
    /* The host's WFE wakes at its event stream (about every microsecond on the
       Mac); if it does not come back that soon (no event stream), spin instead. */
    static int sleep_ok = -1;
    if (sleep_ok < 0) {
        __asm__ volatile("sevl; wfe" ::: "memory"); /* clear the event register */
        uint64_t t0 = wfi_timer_now();
        __asm__ volatile("wfe" ::: "memory");
        sleep_ok = wfi_timer_now() - t0 < 24 * 20;
    }
    uint64_t deadline = wfi_timer_now() + 24 * 50; /* 50 us at 24 MHz */
    if (cpu->m->stats_interval) HV_CPU_COUNT(cpu, wfe_waits, 1);
    HvPerCpu *pcs = cpu->pc_state;
    int timed_out = 0;
    __atomic_store_n(&pcs->wfe_waiting, 1, __ATOMIC_RELAXED);
    for (;;) {
        uint64_t v;
        switch (size) { /* LDAXR arms the host monitor too, so the WFE below wakes on a write */
        case 1: { uint32_t w; __asm__ volatile("ldaxrb %w0, [%1]" : "=r"(w) : "r"(host) : "memory"); v = w; break; }
        case 2: { uint32_t w; __asm__ volatile("ldaxrh %w0, [%1]" : "=r"(w) : "r"(host) : "memory"); v = w; break; }
        case 4: { uint32_t w; __asm__ volatile("ldaxr %w0, [%1]" : "=r"(w) : "r"(host) : "memory"); v = w; break; }
        default: __asm__ volatile("ldaxr %0, [%1]" : "=r"(v) : "r"(host) : "memory"); break;
        }
        if (v != want) {
            pcs->wfe_spin_rounds = 0; /* the word moved: whoever held it got to run */
            break;
        }
        if (__atomic_load_n(&cpu->exit_req, __ATOMIC_RELAXED)) break; /* a request: neither progress nor a wasted round */
        if (wfi_timer_now() >= deadline) {
            __atomic_store_n(&cpu->exit_req, 1, __ATOMIC_RELAXED);
            timed_out = 1;
            break;
        }
        if (sleep_ok) __asm__ volatile("wfe" ::: "memory");
        else __asm__ volatile("yield" ::: "memory");
    }
    __asm__ volatile("clrex" ::: "memory");
    if (timed_out) {
        /* A wait on another word, or on a new value of this one (it moved while the guest was between waits: its
           owner got to run), starts the count again. */
        if (pcs->wfe_spin_va != va || pcs->wfe_spin_val != want) pcs->wfe_spin_va = va, pcs->wfe_spin_val = want, pcs->wfe_spin_rounds = 0;
        if (++pcs->wfe_spin_rounds >= WFE_YIELD_ROUNDS) {
            pcs->wfe_spin_rounds = 0;
            if (cpu->m->stats_interval) HV_CPU_COUNT(cpu, wfe_yields, 1);
            /* Every other runnable thread here first, this one's priority lowered until it runs again (or 1 ms).
               Nothing is held: the dispatcher sees exit_req next and answers requests as usual. */
            thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 1);
        }
    }
    __atomic_store_n(&pcs->wfe_waiting, 0, __ATOMIC_RELAXED); /* the core given up above is part of the spin */
    return 0;
}

/* Which zones of the code cache are in use (make_room). Code chained to code never passes the lookups that count
   the rest; a kick (the 1 ms kicker, an interrupt, another CPU's request) stops the CPU wherever it is, so the zone
   of the code it was going to next is a sample of where the time goes, worth many lookups. Found in this CPU's
   caches only (the indirect-branch cache, which every block entered from the dispatcher goes into, then the
   front cache): nothing translated, no lock. Not counted at every exit: code just made, not chained yet, leaves at
   each of its exits, and would make each new zone look the busiest. */
static void sample_zone(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    uint64_t va = cpu->pc;
    uint32_t flags = block_flags(cpu);
    uint32_t j = (uint32_t)((va >> 2) ^ (va >> 18) ^ ((uint64_t)flags << 13)) & ((1u << HV_JC_BITS) - 1);
    const void *code = NULL;
    int kernel = flags & HV_BLOCK_EL1;
    if ((int)((va >> 55) & 1) == kernel && !(va & 3)) { /* the branch cache first: it is this address space's */
        uint64_t tag = va ^ cpu->ijc_salt[kernel];
        uint64_t *ent = p->ijc + (size_t)hv_ijc_table(flags) * (HV_IJC_TABLE_BYTES / 8) + (size_t)hv_ijc_index(tag) * 2;
        code = ent[0] == tag ? (void *)(uintptr_t)ent[1] : ent[2] == tag ? (void *)(uintptr_t)ent[3] : NULL;
    }
    if (!code && p->jc[j].va == va && p->jc[j].b && p->jc[j].b->flags == flags) code = p->jc[j].b->code;
    int z = code ? hv_jit_zone_of(m, code) : -1;
    if (z >= 0) p->zone_hits[z] += 64;
}

/* After an SVC or ERET, the block the CPU goes to next, if the dispatcher has
   nothing to do first: no request, interrupt or timer due, no flush, and the
   block in this CPU's indirect-branch cache for the new state. hv_helper then
   resumes there instead of leaving (a hundred thousand round trips a second). */
static void *fast_resume(HvCpu *cpu) {
    static int off = -1; /* HV_NO_FAST_RESUME: every exception and return goes through the dispatcher */
    if (off < 0) off = getenv("HV_NO_FAST_RESUME") != NULL;
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    if (off || !m->chain || m->trace || m->log >= 2 || !p || hv_at_stop(m, cpu->pc) || cpu->halted || cpu->off) { if (m->stats_interval) HV_CPU_COUNT(cpu, fr_fail[0], 1); return NULL; }
    if (__atomic_load_n(&cpu->exit_req, __ATOMIC_RELAXED) || __atomic_load_n(&m->flush_pending, __ATOMIC_RELAXED) ||
        __atomic_load_n(&m->stw_active, __ATOMIC_RELAXED) || __atomic_load_n(&m->pause_req, __ATOMIC_RELAXED)) {
        if (m->stats_interval) HV_CPU_COUNT(cpu, fr_fail[1], 1);
        return NULL;
    }
    if ((p->irq_line && !(cpu->daif & HV_PSTATE_I)) || (p->fiq_line && !(cpu->daif & HV_PSTATE_F))) { if (m->stats_interval) HV_CPU_COUNT(cpu, fr_fail[2], 1); return NULL; }
    if (cpu->timer_due && mach_absolute_time() >= cpu->timer_due) { if (m->stats_interval) HV_CPU_COUNT(cpu, fr_fail[3], 1); return NULL; }
    uint32_t flags = block_flags(cpu);
    int kernel = flags & HV_BLOCK_EL1;
    uint64_t va = cpu->pc;
    if ((int)((va >> 55) & 1) != kernel || (va & 3)) return NULL;
    uint64_t tag = va ^ cpu->ijc_salt[kernel];
    uint64_t *ent = p->ijc + (size_t)hv_ijc_table(flags) * (HV_IJC_TABLE_BYTES / 8) + (size_t)hv_ijc_index(tag) * 2;
    void *code = ent[0] == tag ? (void *)(uintptr_t)ent[1] : ent[2] == tag ? (void *)(uintptr_t)ent[3] : NULL;
    if (code) {
        int z = hv_jit_zone_of(m, code);
        if (z >= 0) p->zone_hits[z]++; /* which zones are in use (make_room) */
    } else code = hv_ijc_resolve(cpu, va, flags); /* made already but not cached: cache it */
    if (code) {
        cpu->exit_reason = HV_EXIT_BRANCH;
        if (m->stats_interval) HV_CPU_COUNT(cpu, fast_resumes, 1);
    } else if (m->stats_interval) HV_CPU_COUNT(cpu, fr_fail[4], 1);
    return code;
}

/* Blocks call this through hv_helper_entry. Returns 1 to leave the block. */
static uint64_t reg_hist[65536]; /* --stats: MRS/MSR/SYS by op0:op1:CRn:CRm:op2; reads low, writes high 32 bits */

/* Could this TLBI (just done) change the mapping of pc's page? A page or range
   containing it, anything wider than one address space, or one address space's
   all when pc is a user address. */
static int tlbi_may_hit(HvCpu *cpu, uint32_t insn, uint64_t pc) {
    uint32_t op1 = (insn >> 16) & 7, crm = (insn >> 8) & 0xf, op2 = (insn >> 5) & 7;
    int rt = insn & 0x1f;
    uint64_t xt = rt == 31 ? 0 : cpu->x[rt];
    uint64_t page = pc & 0x00ffffffffffffffull & ~((1ull << HV_TLB_PAGE_BITS) - 1);
    if (op1 != 0) return 1;
    if ((crm == 1 || crm == 3 || crm == 7) && (op2 & 1)) { /* one page */
        uint64_t va = (uint64_t)((int64_t)((xt & 0xfffffffffffull) << 20) >> 8);
        uint64_t mask = 0x00ffffffffffffffull & ~((1ull << hv_tlbi_page_bits(cpu, va)) - 1);
        return (va & mask) == (pc & mask);
    }
    if ((crm == 2 || crm == 5 || crm == 6) && (op2 & 1)) { /* a range */
        uint32_t tg = (xt >> 46) & 3;
        int shift = tg == 1 ? 12 : tg == 2 ? 14 : tg == 3 ? 16 : 0;
        if (!shift) return 1;
        uint64_t pages = (((xt >> 39) & 31) + 1) << (5 * ((xt >> 44) & 3) + 1);
        uint64_t va = (uint64_t)((int64_t)((xt & 0x1fffffffffull) << (64 - 37)) >> (64 - 37 - shift)) & 0x00ffffffffffffffull;
        uint64_t len = pages << shift;
        return page + (1ull << HV_TLB_PAGE_BITS) > va && page < va + len;
    }
    if ((crm == 1 || crm == 3 || crm == 7) && op2 == 2) return !((pc >> 55) & 1); /* ASIDE1: user pages */
    return 1; /* VMALLE1 and the rest */
}

static uint32_t helper(HvCpu *cpu, const HvHelperInfo *info_in);

uint32_t hv_helper(HvCpu *cpu, const HvHelperInfo *info_in) {
    uint32_t r = helper(cpu, info_in);
    /* An exception just taken (an abort, an undefined instruction, a trap): its
       vector's block, as after an SVC. */
    if (r && !cpu->off && cpu->exit_reason == HV_EXIT_EXCEPTION) {
        void *code = fast_resume(cpu);
        if (code) {
            cpu->helper_ret = (uint64_t)(uintptr_t)code;
            return 0;
        }
    }
    return r;
}

static uint32_t helper(HvCpu *cpu, const HvHelperInfo *info_in) {
    HvHelperInfo info_copy = *info_in;
    info_copy.op &= 0xff; /* the rest: hv_helper_thunk's refill flags */
    const HvHelperInfo *info = &info_copy;
    if (info->op < 12 && cpu->m->stats_interval) HV_CPU_COUNT(cpu, helper[info->op], 1);
    if (cpu->m->stats_interval && (info->op == HV_OP_MRS || info->op == HV_OP_MSR || info->op == HV_OP_SYS))
        reg_hist[(info->insn >> 5) & 0xffff] += info->op == HV_OP_MRS ? 1 : 1ull << 32;
    switch (info->op) {
    case HV_OP_APFLAGS:
        return hv_rosetta_alu(cpu, info->insn, info->pc);
    case HV_OP_APFCMP:
        cpu->apflags = (cpu->nzcv >> 28 & 1) << 26;
        return 0;
    case HV_OP_MEM:
        HV_CPU_COUNT(cpu, slow_mem, 1);
        if (cpu->m->stats_interval) { /* --stats: which kinds of access take the slow path */
            HvMemOp op;
            if (hv_mem_decode(info->insn, &op) == 0 && op.kind < 12) HV_CPU_COUNT(cpu, slow_kind[op.kind], 1);
        }
        return hv_mem_emulate(cpu, info->insn, info->pc);
    case HV_OP_MRS:
        return hv_sysreg_mrs(cpu, info->insn, info->pc);
    case HV_OP_MSR:
        return hv_sysreg_msr(cpu, info->insn, info->pc);
    case HV_OP_SYS: {
        uint32_t r = hv_sysreg_sys(cpu, info->insn, info->pc);
        if (!r && ((info->insn >> 12) & 0xf) == 8 && tlbi_may_hit(cpu, info->insn, info->pc)) {
            cpu->pc = info->pc + 4; /* the block carries on otherwise: leave, to fetch the rest anew */
            return 1;
        }
        return r;
    }
    case HV_OP_EXCEPTION: {
        return hv_exception_insn(cpu, info->insn, info->pc); /* SVC: see hv_helper */
    }
    case HV_OP_ERET: {
        hv_eret(cpu);
        void *code = fast_resume(cpu);
        if (code) {
            cpu->helper_ret = (uint64_t)(uintptr_t)code;
            return 0;
        }
        return 1;
    }
    case HV_OP_FPTRAP:
        hv_raise_sync(cpu, HV_EC_FP_ACCESS, 1u << 24 | 0xeu << 20, 0, info->pc);
        return 1;
    case HV_OP_CLREX:
        cpu->excl_addr = ~0ull;
        return 0;
    case HV_OP_WFI:
        cpu->pc = info->pc + 4;
        cpu->exit_reason = HV_EXIT_WFI;
        return 1;
    case HV_OP_WFE:
        return wfe(cpu);
    case HV_OP_UNDEF:
    default:
        hv_raise_undef(cpu, info->pc);
        return 1;
    }
}

/* Debugging: EL0 is running a block translated from kernel code. Which block, and how did we get here? */
void hv_debug_el0_in_kernel(HvCpu *cpu, const char *what) {
    {
        {
            HvMachine *m = cpu->m;
            uint32_t *host = (uint32_t *)(uintptr_t)cpu->helper_ret; /* the block's blr into hv_helper_entry */
            HvBlock *b = hv_block_at(m, host);
            HvTrace none = {0};
            const HvTrace *tr = cpu->pc_state ? &cpu->pc_state->trace : &none;
            fprintf(stderr, "hengevm: el0 runs kernel code: block %llx flags %u (index %d of %u), flushes %llu, last stub %llx, "
                            "miss site %llx\n  recent:", b ? (unsigned long long)b->va : 0ull, b ? b->flags : 0, b ? (int)(b - m->blocks) : -1, hv_blocks_held(m),
                    (unsigned long long)HV_STAT_TOTAL(m, block_flushes), (unsigned long long)cpu->last_stub,
                    (unsigned long long)cpu->miss_site);
            for (int k = 15; k >= 0; k--) fprintf(stderr, " %llx", (unsigned long long)tr->recent[(tr->nrecent - 1 - k) & 15]);
            fprintf(stderr, "\n  (%s) el changes, oldest first:\n", what);
            for (int k = 15; k >= 0; k--) {
                uint32_t i = (tr->nelchg - 1 - k) & 15;
                if (tr->elchg[i].kind == 9)
                    fprintf(stderr, "    eret at %llx from el%u to %llx spsr %llx\n", (unsigned long long)tr->elchg[i].from,
                            tr->elchg[i].el, (unsigned long long)tr->elchg[i].to, (unsigned long long)tr->elchg[i].spsr);
                else
                    fprintf(stderr, "    exception type %u esr %llx from el%u pc %llx (spsr %llx)\n", tr->elchg[i].kind,
                            (unsigned long long)tr->elchg[i].to, tr->elchg[i].el,
                            (unsigned long long)tr->elchg[i].from, (unsigned long long)tr->elchg[i].spsr);
            }
        }
    }
}

/* WFI: sleep until an interrupt is pending (masked or not) or the next timer
   event, at most 10 ms. WFI may complete early, so the guest just runs again
   if nothing happened. */
static int wait_for_interrupt(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    uint64_t deadline = m->tick ? m->tick(m, cpu) : 0;
    HvPerCpu *p = cpu->pc_state;
    if (p->irq_line || p->fiq_line) return 0;
    if (!m->tick) return -1; /* nothing could ever wake us */
    uint64_t now = mach_absolute_time();
    uint64_t wait_ticks = deadline && deadline > now ? deadline - now : 240000; /* the counter runs at 24 MHz */
    if (wait_ticks > 240000) wait_ticks = 240000;
    uint64_t t0 = mach_absolute_time();
    /* Sleep until the next timer event or until a device thread has work for us. */
    hv_big_unlock(m); /* let the other CPUs run while this one sleeps */
    park(cpu);
    /* A thread on another CPU often has something for this one microseconds later
       (a pipe, a Mach message: macOS is made of these), and waking a sleeping host
       thread takes far longer than that: watch for it a little first. HV_WFI_SPIN: microseconds. */
    int spin_us = hv_knob[HV_K_WFI_SPIN];
    if (spin_us) {
        static int sleep_ok = -1; /* does the host's WFE come back soon by itself? (see wfe()) */
        if (sleep_ok < 0) {
            __asm__ volatile("sevl; wfe" ::: "memory");
            uint64_t t = wfi_timer_now();
            __asm__ volatile("wfe" ::: "memory");
            sleep_ok = wfi_timer_now() - t < 24 * 20;
        }
        uint64_t until = wfi_timer_now() + (uint64_t)spin_us * 24;
        for (;;) {
            uint32_t v;
            __asm__ volatile("ldaxr %w0, [%1]" : "=r"(v) : "r"(&p->wake_pending) : "memory"); /* a store to it wakes the WFE */
            if (v || wfi_timer_now() >= until) break;
            if (sleep_ok) __asm__ volatile("wfe" ::: "memory");
            else __asm__ volatile("yield" ::: "memory");
        }
        __asm__ volatile("clrex" ::: "memory");
    }
    if (__atomic_exchange_n(&p->wake_pending, 0, __ATOMIC_ACQ_REL)) {
        hv_big_lock(m); /* still parked: see big_lock */
        unpark(cpu);
        HV_CPU_COUNT(cpu, wfi, 1);
        HV_CPU_COUNT(cpu, wfi_ticks, mach_absolute_time() - t0);
        m->tick(m, cpu);
        return 0;
    }
    pthread_mutex_lock(&p->wake_mu);
    if (!p->wake_pending) {
        struct timespec ts = {(time_t)(wait_ticks / 24000000), (long)(wait_ticks % 24000000) * 1000 / 24};
        pthread_cond_timedwait_relative_np(&p->wake_cv, &p->wake_mu, &ts);
    }
    p->wake_pending = 0;
    pthread_mutex_unlock(&p->wake_mu);
    hv_big_lock(m); /* still parked: see big_lock */
    unpark(cpu);
    HV_CPU_COUNT(cpu, wfi, 1);
    HV_CPU_COUNT(cpu, wfi_ticks, mach_absolute_time() - t0);
    m->tick(m, cpu);
    return 0;
}

/* --profile: which guest blocks the sampled host PCs were in. Blocks are laid
   out in the JIT region in the order they were made, so a binary search over
   m->blocks finds the one holding a PC. */
static void print_profile(HvMachine *m) {
    enum { SLOTS = 4096 };
    static struct { uint64_t va; uint32_t flags, n, ninsns; } top[SLOTS];
    memset(top, 0, sizeof top);
    uint32_t total = 0, outside = 0;
    static struct { const char *name; uint32_t n; } fn[64];
    static struct { uint32_t off, n; } run_off[256];
    int nfn = 0, nrun = 0;
    uint32_t head = __atomic_load_n(&m->prof_head, __ATOMIC_ACQUIRE);
    if (head - m->prof_tail > HV_PROF_RING) m->prof_tail = head - HV_PROF_RING;
    for (; m->prof_tail != head; m->prof_tail++) {
        uint32_t *pc = (uint32_t *)(uintptr_t)m->prof[m->prof_tail & (HV_PROF_RING - 1)];
        total++;
        HvBlock *b = hv_block_at(m, pc);
        if (!b) {
            outside++;
            Dl_info di;
            const char *name = dladdr(pc, &di) && di.dli_sname ? di.dli_sname : "?";
            static const char *detail;
            if (!detail) detail = getenv("HV_PROF_FN") ? getenv("HV_PROF_FN") : "run";
            if (!strcmp(name, detail)) { /* per-offset detail for one function (HV_PROF_FN, default the dispatcher loop) */
                uint32_t off = (uint32_t)((uint8_t *)pc - (uint8_t *)di.dli_saddr);
                int r = 0;
                while (r < nrun && run_off[r].off != off) r++;
                if (r == nrun && nrun < 256) { run_off[nrun].off = off; run_off[nrun++].n = 0; }
                if (r < nrun) run_off[r].n++;
            }
            int k = 0;
            while (k < nfn && strcmp(fn[k].name, name)) k++;
            if (k == nfn && nfn < 64) { fn[nfn].name = name; fn[nfn++].n = 0; }
            if (k < nfn) fn[k].n++;
            continue;
        }
        uint32_t h = (uint32_t)((b->va >> 2) ^ b->flags) & (SLOTS - 1);
        int probes = 0;
        while (top[h].n && (top[h].va != b->va || top[h].flags != b->flags) && ++probes < 64) h = (h + 1) & (SLOTS - 1);
        if (probes == 64) continue; /* table full: the block is too cold to make the list anyway */
        top[h].va = b->va;
        top[h].flags = b->flags;
        top[h].ninsns = b->ninsns;
        top[h].n++;
    }
    if (!total) return;
    fprintf(stderr, "  profile: %u samples, %.0f%% outside translated code:", total, 100.0 * outside / total);
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < nfn; i++)
            if (fn[i].n && (best < 0 || fn[i].n > fn[best].n)) best = i;
        if (best < 0) break;
        fprintf(stderr, " %s %.1f%%", fn[best].name, 100.0 * fn[best].n / total);
        fn[best].n = 0;
    }
    fprintf(stderr, "\n  %s+:", getenv("HV_PROF_FN") ? getenv("HV_PROF_FN") : "run");
    for (int k = 0; k < 12; k++) {
        int best = -1;
        for (int i = 0; i < nrun; i++)
            if (run_off[i].n && (best < 0 || run_off[i].n > run_off[best].n)) best = i;
        if (best < 0) break;
        fprintf(stderr, " %#x:%.1f%%", run_off[best].off, 100.0 * run_off[best].n / total);
        run_off[best].n = 0;
    }
    fprintf(stderr, "\n");
    for (int k = 0; k < 12; k++) {
        int best = -1;
        for (int i = 0; i < SLOTS; i++)
            if (top[i].n && (best < 0 || top[i].n > top[best].n)) best = i;
        if (best < 0 || top[best].n * 200 < total) break;
        fprintf(stderr, "    %5.1f%%  %llx el%u (%u insns)\n", 100.0 * top[best].n / total,
                (unsigned long long)top[best].va, top[best].flags & HV_BLOCK_EL1, top[best].ninsns);
        top[best].n = 0;
    }
}

/* Buckets part, part + 64, ... of the block hash (print_stats' sample): only in the 64ths inserts wrote (hash_used).
   The rest reads zero, and reading it, 1 bucket in 256 bytes, would bring all 8 MB of it into memory. Plain reads
   with the CPUs running: a record may be filled again under it, so a walk stops at HV_HASH_SAMPLE_MAX links (the
   table is never freed while the machine lives, so a wrong index only miscounts). */
#define HV_HASH_SAMPLE_MAX 4096
void hv_block_hash_sample(HvMachine *m, uint32_t part, HvHashSample *s) {
    *s = (HvHashSample){0};
    const uint32_t per = HV_BLOCK_HASH / 64; /* buckets in a 64th: a multiple of 64, so each holds its share */
    for (uint64_t u = __atomic_load_n(&m->hash_used, __ATOMIC_RELAXED); u; u &= u - 1) {
        uint32_t q = (uint32_t)__builtin_ctzll(u);
        for (uint32_t h = q * per + (part & 63); h < (q + 1) * per; h += 64) {
            uint64_t len = 0;
            uint32_t first = __atomic_load_n(&m->hash[h], __ATOMIC_ACQUIRE);
            for (uint32_t i = first; i && i <= m->max_blocks && len < HV_HASH_SAMPLE_MAX;) {
                const HvBlock *b = &m->blocks[i - 1];
                len++;
                /* ram read once: a retired zone's records go to zero as the reclaimer gives its tables back */
                const HvRam *r = __atomic_load_n((HvRam *const *)&b->ram, __ATOMIC_RELAXED);
                if (r && r->page_gen[b->pg] != b->gen) s->stale++;
                i = __atomic_load_n(&b->hash_next, __ATOMIC_ACQUIRE);
            }
            if (len) s->used++;
            s->sumsq += len * len;
            if (len > s->maxlen) s->maxlen = len, s->worst = &m->blocks[first - 1];
        }
    }
}

static void print_stats(HvCpu *cpu, int force) {
    static uint64_t last_t, start_t;
    /* This uses floating point, and the host FP registers hold the guest's state. */
    hv_fp_leave(cpu);
    uint64_t zero = 0;
    __asm__ volatile("msr fpcr, %0" ::"r"(zero));
    static HvStats last;
    HvMachine *m = cpu->m;
    uint64_t now = mach_absolute_time();
    if (!start_t) start_t = last_t = now;
    if (!force && now - last_t < (uint64_t)m->stats_interval * 24000000ull) {
        hv_fp_enter(cpu);
        return;
    }
    double dt = (double)(now - last_t) / 24e6;
    static HvStats sum; /* every CPU's and the machine's counters added up */
    hv_stats_sum(m, &sum);
    const HvStats *s = &sum;
#define RATE(f) ((double)(s->f - last.f) / dt)
    fprintf(stderr,
            "stats %5.0fs: blocks %.2fM/s, translated %.0f/s (made again: emptied %.0f%% written %.0f%%, lost races %.0f/s; %.0f%% of time, %.1f insns, %.1fx), slow mem %.0fK/s, mrs %.0fK/s msr %.0fK/s sys %.0fK/s, "
            "exc %.0f/s irq %.0f/s, tlb flush %.0f/s, fetch walks %.0fK/s, wfi %.0f/s (%.0f%% idle), flushes %.0f/s (smc %.0f ic %.0f full %.0f), blocks held %u (%.0f MB, %.0f MB live, %llu full flushes, %llu zones emptied), chained %.0f/s ijc fills %.0fK/s resets %.0f/s, el%u pc %llx\n",
            (double)(now - start_t) / 24e6, RATE(blocks) / 1e6, RATE(translations), 100.0 * RATE(retrans_evicted) / (RATE(translations) + 1e-9), 100.0 * RATE(retrans_written) / (RATE(translations) + 1e-9), RATE(lost_races), 100.0 * (double)(s->translate_ticks - last.translate_ticks) / (double)(now - last_t), RATE(translated_insns) / (RATE(translations) + 1e-9), RATE(translated_words) / (RATE(translated_insns) + 1e-9), RATE(helper[HV_OP_MEM]) / 1e3,
            RATE(helper[HV_OP_MRS]) / 1e3, RATE(helper[HV_OP_MSR]) / 1e3, RATE(helper[HV_OP_SYS]) / 1e3,
            RATE(exceptions), RATE(interrupts), RATE(tlb_flushes), RATE(fetch_walks) / 1e3, RATE(wfi),
            100.0 * (double)(s->wfi_ticks - last.wfi_ticks) / (double)(now - last_t), RATE(block_flushes), RATE(flush_smc),
            RATE(flush_ic), RATE(flush_full), hv_blocks_held(m), (double)hv_jit_words_used(m) * 4 / (1 << 20), (double)hv_jit_words_live(m) * 4 / (1 << 20), (unsigned long long)s->block_flushes, (unsigned long long)s->zone_evictions, RATE(chained), RATE(ijc_fills) / 1e3, RATE(ijc_resets), cpu->el,
            (unsigned long long)cpu->pc);
    {
        double tot = 0;
        for (int k = 0; k < 6; k++) tot += RATE(why[k]);
        /* How much of the interval the CPU thread actually ran: below 100% means the host took it away. */
        static double last_busy;
        thread_basic_info_data_t ti;
        mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
        mach_port_t self = mach_thread_self();
        if (thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&ti, &cnt) == KERN_SUCCESS) {
            double busy = ti.user_time.seconds + ti.user_time.microseconds / 1e6 + ti.system_time.seconds +
                          ti.system_time.microseconds / 1e6;
            fprintf(stderr, "  cpu thread ran %.0f%% of the interval\n", 100 * (busy - last_busy) / dt);
            if (m->ncpus > 1) {
                fprintf(stderr, "  cpus:");
                for (int i = 0; i < m->ncpus; i++)
                    if (m->cpus[i])
                        fprintf(stderr, " [%d] %s el%u pc %llx", i, m->cpus[i]->off ? "off" : "on", m->cpus[i]->el,
                                (unsigned long long)m->cpus[i]->pc);
                fprintf(stderr, "\n");
            }
            last_busy = busy;
        }
        mach_port_deallocate(mach_task_self(), self);
        fprintf(stderr, "  whole-TLB flushes/s: TLBI all here %.0f, TLBI all shared %.0f, SCTLR %.0f, TTBR1 %.0f, TCR %.0f, MAIR %.0f, range of all %.0f, requests overflowed %.0f\n",
                RATE(flush_why[0]), RATE(flush_why[1]), RATE(flush_why[2]), RATE(flush_why[3]), RATE(flush_why[4]), RATE(flush_why[5]),
                RATE(flush_why[6]), RATE(flush_why[7]));
        fprintf(stderr, "  TLBI all here/s by time since a TTBR0 change: <2us %.0f, <20us %.0f, <200us %.0f, later %.0f; left out %.0f; at",
                RATE(flush_after_switch[0]), RATE(flush_after_switch[1]), RATE(flush_after_switch[2]), RATE(flush_after_switch[3]), RATE(flush_skipped));
        HvStatKey keys[8];
        hv_stats_keys(m, offsetof(HvPerCpu, flush_pc), keys);
        for (int i = 0; i < 8 && keys[i].n; i++) fprintf(stderr, " %llx x%llu", (unsigned long long)keys[i].key, (unsigned long long)keys[i].n);
        fprintf(stderr, "\n");
        if (s->flush_checked)
            fprintf(stderr, "  switch-flush checks/s: %.0f, entries walked %.0f, found out of date %.0f (flushed), access flag only %.0f\n",
                    RATE(flush_checked), RATE(flush_check_entries), RATE(flush_check_bad), RATE(flush_check_af));
        fprintf(stderr, "  cache drops/s: full %.0f, ASID %.0f, user VA %.0f, kernel VA %.0f, code %.0f (whole user half %.0f, filter full %.0f, big user range %.0f)\n", RATE(reset_why[0]),
                RATE(reset_why[1]), RATE(reset_why[2]), RATE(reset_why[3]), RATE(reset_why[4]), RATE(reset_why[5]), RATE(reset_why[6]), RATE(reset_why[7]));
        if (m->nzones > 1 || s->zone_evictions)
            fprintf(stderr, "  code cache zones: %d of %.0f MB (%d retired), filling %d; emptied %.2f/s (%.0f blocks/s, %.0f links cut/s, %.1f ms each)\n",
                    m->nzones, (double)m->zone_words * 4 / (1 << 20), m->nzones - zones_active(m), m->zone, RATE(zone_evictions), RATE(evicted_blocks),
                    RATE(evict_unchained), RATE(zone_evictions) > 0 ? (double)(s->evict_ticks - last.evict_ticks) / 24e3 / (double)(s->zone_evictions - last.zone_evictions) : 0.0);
        { /* Writes to code pages (retire_page, lookup's run-once blocks, written's mask) and what emptying a zone cost */
            double ne = (double)(s->zone_evictions - last.zone_evictions);
#define STEP_MS(k) (ne > 0 ? (double)(s->evict_step_ticks[k] - last.evict_step_ticks[k]) / 24e3 / ne : 0.0)
            double drops = (double)(s->flush_smc - last.flush_smc), known = (double)(s->rewrite_same - last.rewrite_same + s->rewrite_changed - last.rewrite_changed);
            double ew[4], ee[4], nw = 0, nev = 0;
            for (int k = 0; k < 4; k++) ew[k] = (double)(s->entries_written[k] - last.entries_written[k]), nw += ew[k];
            for (int k = 0; k < 4; k++) ee[k] = (double)(s->entries_evicted[k] - last.entries_evicted[k]), nev += ee[k];
            for (int k = 0; k < 4; k++) ew[k] = nw > 0 ? 100.0 * ew[k] / nw : 0.0, ee[k] = nev > 0 ? 100.0 * ee[k] / nev : 0.0;
            fprintf(stderr, "  dispatcher entries before death, %% of blocks with 1/2-8/9-64/more: retired by a write %.0f/%.0f/%.0f/%.0f of %.0f, "
                            "emptied with their zone %.0f/%.0f/%.0f/%.0f of %.0f\n",
                    ew[0], ew[1], ew[2], ew[3], nw, ee[0], ee[1], ee[2], ee[3], nev);
            fprintf(stderr, "  code writes: blocks retired %.0f/s, per page dropped %.1f blocks %.1f KB, same bytes as before %.0f%% of %.0f known, run once %.0f/s, writes that kept the code %.0f/s (pages dropped after %d %.1f/s); code held %.0f MB live, "
                            "emptied zones' blocks live %.0f/s retired %.0f/s, ms each in blocks/exits in/own exits/caches %.2f/%.2f/%.2f/%.2f; zones retired %llu "
                            "returned %llu, trims' ms in stops %.1f, giving back after %.1f\n",
                    RATE(retired), drops > 0 ? (double)(s->retired - last.retired) / drops : 0.0,
                    drops > 0 ? (double)(s->retired_words - last.retired_words) * 4 / 1024 / drops : 0.0,
                    known > 0 ? 100.0 * (double)(s->rewrite_same - last.rewrite_same) / known : 0.0, known, RATE(tr_once), RATE(smc_missed), HV_CODE_MISS_LIMIT, RATE(smc_miss_drops), (double)hv_jit_words_live(m) * 4 / (1 << 20), RATE(evict_live),
                    RATE(evict_dead), STEP_MS(0), STEP_MS(1), STEP_MS(2), STEP_MS(3), (unsigned long long)s->zones_retired,
                    (unsigned long long)s->zones_returned, (double)(s->trim_stop_ticks - last.trim_stop_ticks) / 24e3,
                    (double)(s->giveback_ticks - last.giveback_ticks) / 24e3);
#undef STEP_MS
            /* What emptying a zone did per zone (list nodes: one per block or exit taken off, O(1) each), the longest
               emptying since the last line, and the longest stop (the benchmark takes and resets that one) */
            fprintf(stderr, "  emptying a zone: %.0f blocks off page lists, %.0f exits off lists (%.0f cut), %.2f hash links "
                            "passed per block; longest %.1f ms, longest stop %.1f ms\n",
                    ne > 0 ? (double)(s->evict_page_unlinks - last.evict_page_unlinks) / ne : 0.0,
                    ne > 0 ? (double)(s->evict_delists - last.evict_delists) / ne : 0.0,
                    ne > 0 ? (double)(s->evict_unchained - last.evict_unchained) / ne : 0.0,
                    s->evict_live > last.evict_live ? (double)(s->evict_hash_hops - last.evict_hash_hops) / (double)(s->evict_live - last.evict_live) : 0.0,
                    (double)__atomic_exchange_n(&m->stats.evict_max_ticks, 0, __ATOMIC_RELAXED) / 24e3,
                    (double)__atomic_load_n(&m->stats.stop_max_ticks, __ATOMIC_RELAXED) / 24e3);
            /* The reclaimer (emptying zones with the CPUs running): zones it emptied, the stops that only marked one
               for it, zones still emptied in a stop (none ready), jobs given up, its kicks of every CPU in grace
               periods (one per zone when each CPU reaches its dispatcher within 1 ms); its wall time per zone by part
               (unhooking, the grace period, the rest); the longest grace period, tr_mu hold, and stop that only filled
               a ready zone or marked one (its own work, not the wait for the others to stop) since the last line */
            double nr = (double)(s->reclaims - last.reclaims);
#define RC_MS(k) (nr > 0 ? (double)(s->reclaim_ticks[k] - last.reclaim_ticks[k]) / 24e3 / nr : 0.0)
            fprintf(stderr, "  reclaimer: emptied %.0f, marked %.0f, emptied in a stop %.0f (%llu in all), given up %.0f, kicks %.0f; ms each "
                            "unhook/grace/after %.2f/%.2f/%.2f; longest grace %.2f ms, longest tr_mu hold %.2f ms, longest "
                            "quick stop %.3f ms\n",
                    nr, (double)(s->reclaim_marks - last.reclaim_marks), (double)(s->evict_sync - last.evict_sync),
                    (unsigned long long)s->evict_sync, (double)(s->reclaim_aborts - last.reclaim_aborts),
                    (double)(s->reclaim_kicks - last.reclaim_kicks), RC_MS(0), RC_MS(1),
                    RC_MS(2), (double)__atomic_exchange_n(&m->stats.reclaim_gp_max_ticks, 0, __ATOMIC_RELAXED) / 24e3,
                    (double)__atomic_exchange_n(&m->stats.reclaim_hold_max_ticks, 0, __ATOMIC_RELAXED) / 24e3,
                    (double)__atomic_exchange_n(&m->stats.quick_stop_max_ticks, 0, __ATOMIC_RELAXED) / 24e3);
#undef RC_MS
        }
        { /* How long the pages' lists are: blocks per code page, exits from other pages per page. A sample: 1 page in
             64 of each RAM region (a different 64th each time), at most HV_LIST_SAMPLE_MAX nodes, under tr_mu */
            static uint32_t lpart;
            lpart = (lpart + 1) & 63;
            uint64_t bp = 0, bn = 0, bmax = 0, xp = 0, xn = 0, xmax = 0, nodes = 0;
            enum { HV_LIST_SAMPLE_MAX = 1 << 16 };
            pthread_mutex_lock(&m->tr_mu);
            for (int ri = 0; ri < m->nram && nodes < HV_LIST_SAMPLE_MAX; ri++) {
                const HvRam *r = &m->ram[ri];
                if (!r->block_heads || !r->chain_heads) continue;
                uint64_t pages = (r->size + (1ull << HV_RAM_PAGE_BITS) - 1) >> HV_RAM_PAGE_BITS;
                for (uint64_t pg = lpart; pg < pages && nodes < HV_LIST_SAMPLE_MAX; pg += 64) {
                    uint64_t n = 0;
                    for (uint32_t i = r->block_heads[pg]; i && nodes < HV_LIST_SAMPLE_MAX; i = m->blocks[i - 1].page_next) n++, nodes++;
                    if (n) bp++, bn += n, bmax = n > bmax ? n : bmax;
                    n = 0;
                    for (uint32_t i = r->chain_heads[pg]; i && nodes < HV_LIST_SAMPLE_MAX; i = m->slot_xnext[i - 1]) n++, nodes++;
                    if (n) xp++, xn += n, xmax = n > xmax ? n : xmax;
                }
            }
            pthread_mutex_unlock(&m->tr_mu);
            fprintf(stderr, "  page lists (1/64 sampled): %llu code pages, blocks each %.1f (most %llu); %llu pages with exits "
                            "from others, exits each %.1f (most %llu)%s\n",
                    (unsigned long long)bp * 64, bp ? (double)bn / (double)bp : 0.0, (unsigned long long)bmax,
                    (unsigned long long)xp * 64, xp ? (double)xn / (double)xp : 0.0, (unsigned long long)xmax,
                    nodes >= HV_LIST_SAMPLE_MAX ? " (cut short)" : "");
        }
        { /* block hash health: chain lengths and how much of it is stale. A sample of the buckets (1 in 64, a
             different 64th each time): walking every one, millions of blocks, stalled this CPU for a good part of a second. */
            static uint32_t part;
            part = (part + 1) & 63;
            HvHashSample hs;
            hv_block_hash_sample(m, part, &hs);
            uint64_t used = hs.used, stale = hs.stale, maxlen = hs.maxlen, sumsq = hs.sumsq;
            uint32_t held = hv_blocks_held(m);
            fprintf(stderr, "  block hash (1/64 sampled, scaled): %llu buckets used, %llu stale blocks, max chain %llu, mean walk %.1f\n",
                    (unsigned long long)used * 64, (unsigned long long)stale * 64, (unsigned long long)maxlen,
                    held ? (double)sumsq * 64 / held : 0.0);
            int k = 0;
            for (const HvBlock *b = hs.worst; b && k < 6; k++) { /* not m->hash[0] when none: a page brought in */
                const HvRam *r = __atomic_load_n((HvRam *const *)&b->ram, __ATOMIC_RELAXED); /* once: as in the sample */
                if (!r) break;
                fprintf(stderr, "    worst: va %llx pa %llx flags %x gen %u/%u\n", (unsigned long long)b->va,
                        (unsigned long long)b->pa, b->flags, b->gen, r->page_gen[b->pg]);
                uint32_t i = __atomic_load_n(&b->hash_next, __ATOMIC_ACQUIRE);
                b = i && i <= m->max_blocks ? &m->blocks[i - 1] : NULL;
            }
        }
        {
            double tw = RATE(words_inline) + RATE(words_ool) + RATE(words_pool) + 1e-9;
            fprintf(stderr, "  code words: inline %.0f%% out of line %.0f%% literals %.0f%%\n", 100 * RATE(words_inline) / tw,
                    100 * RATE(words_ool) / tw, 100 * RATE(words_pool) / tw);
        }
        fprintf(stderr, "  new code pages %.0f/s (%.0f/s waited for a writer); waits: tlbi acks %.0f/s %.1f%%, device lock %.0f/s %.1f%%, translation lock %.0f/s %.1f%% (of one CPU)\n",
                RATE(code_protects), RATE(code_protect_waits), RATE(syncs), RATE(sync_ticks) / 240000.0, RATE(dev_waits), RATE(dev_wait_ticks) / 240000.0,
                RATE(tr_waits), RATE(tr_wait_ticks) / 240000.0);
        fprintf(stderr, "  light lookups failed/s: odd %.0f, filters full %.0f, no mapping %.0f, no block yet %.0f; walked %.0fK/s\n",
                RATE(light_fail[0]), RATE(light_fail[1]), RATE(light_fail[2]), RATE(light_fail[3]), RATE(light_walks) / 1e3);
        fprintf(stderr, "  big user-range TLBIs/s: %.0f TLB only in the range, %.0f with no cached targets to drop; big kernel ranges %.0f\n",
                RATE(tlb_range_flushes), RATE(range_spared), RATE(kernel_ranges));
        fprintf(stderr, "  TLBIs left to other CPUs/s: raced %.0f, every address space's cached targets %.0f, big range %.0f, ASID or all %.0f\n",
                RATE(posted_kind[0]), RATE(posted_kind[3]), RATE(posted_kind[1]), RATE(posted_kind[2]));
        fprintf(stderr, "  remote drop races/s: walking %.0f, filling %.0f, during %.0f\n", RATE(race_kind[0]), RATE(race_kind[1]), RATE(race_kind[2]));
        {
            static const char *kn[16] = {"copy", "pcrel", "branch", "branch-reg", "eret", "literal", "memory", "prefetch",
                                         "exception", "sysreg", "sys", "barrier", "wait", "pac", "undef", "?"};
            uint64_t ti = 0, tw = 0;
            for (int k = 0; k < 16; k++) ti += s->kind_insns[k] - last.kind_insns[k], tw += s->kind_words[k] - last.kind_words[k];
            if (ti) {
                fprintf(stderr, "  inline words per insn %.2f:", (double)tw / ti);
                for (int k = 0; k < 16; k++) {
                    uint64_t i = s->kind_insns[k] - last.kind_insns[k], w = s->kind_words[k] - last.kind_words[k];
                    if (i * 200 >= ti) fprintf(stderr, " %s %.0f%%x%.1f", kn[k], 100.0 * i / ti, (double)w / i);
                }
                fprintf(stderr, "\n");
            }
        }
        fprintf(stderr, "  user light fills: shared region %.0fK/s, elsewhere %.0fK/s\n", RATE(light_region[0]) / 1e3, RATE(light_region[1]) / 1e3);
        fprintf(stderr, "  code-page protects/s: none %.0f, one entry %.0f, whole-TLB scans %.0f\n", RATE(protect_kind[0]),
                RATE(protect_kind[1]), RATE(protect_kind[2]));
        fprintf(stderr, "  fast resume refused/s: state %.0f, exit request %.0f, interrupt %.0f, timer %.0f, not cached %.0f\n", RATE(fr_fail[0]),
                RATE(fr_fail[1]), RATE(fr_fail[2]), RATE(fr_fail[3]), RATE(fr_fail[4]));
        fprintf(stderr, "  WFE waits in place %.0f/s (gave the core up %.0f/s); exits chained as placed %.0f/s; SVC/ERET resumed in place %.0f/s\n",
                RATE(wfe_waits), RATE(wfe_yields), RATE(chained_early), RATE(fast_resumes));
        fprintf(stderr, "  exit requests/s: TLBI posts %.0f, wakes %.0f (of them device work %.0f), kick all %.0f, own %.0f\n",
                RATE(kick_src[0]), RATE(kick_src[1]), RATE(kick_src[3]), RATE(kick_src[2]), RATE(kick_src[4]));
        fprintf(stderr, "  light refills %.0fK/s, light branch-cache fills %.0fK/s, chained to other pages %.0f/s\n", RATE(refills) / 1e3,
                RATE(ijc_light) / 1e3, RATE(xchained));
        fprintf(stderr, "  process switches: %.0f/s saved, %.0f/s restored, %.0f/s dropped by a TLBI (%.0f entries each)\n", RATE(stash_saves), RATE(stash_hits), RATE(stash_stale),
                s->stash_hits - last.stash_hits ? (double)(s->stash_entries - last.stash_entries) / (s->stash_hits - last.stash_hits) : 0.0);
        fprintf(stderr, "  tlb misses K/s: el0 empty %.0f el0 conflict %.0f el1 empty %.0f el1 conflict %.0f\n",
                RATE(tlb_miss_kind[0]) / 1e3, RATE(tlb_miss_kind[1]) / 1e3, RATE(tlb_miss_kind[2]) / 1e3, RATE(tlb_miss_kind[3]) / 1e3);
        fprintf(stderr, "  slow mem causes K/s: mmio %.0f code-page write %.0f page end %.0f tlb miss %.0f other %.0f entry-but-tag %.0f"
                        " (page end done light %.0f)\n",
                RATE(slow_why[0]) / 1e3, RATE(slow_why[1]) / 1e3, RATE(slow_why[2]) / 1e3, RATE(slow_why[3]) / 1e3,
                RATE(slow_why[4]) / 1e3, RATE(slow_why[5]) / 1e3, RATE(page_end_light) / 1e3);
        fprintf(stderr, "  walks to pages under %u KB: joined %.0fK/s; not: one unmapped %.0fK/s, other attributes %.0fK/s, not one run %.0fK/s;"
                        " 4 KB table fills %.0fK/s\n",
                (1u << HV_TLB_PAGE_BITS) / 1024, RATE(small_join[0]) / 1e3, RATE(small_join[1]) / 1e3, RATE(small_join[2]) / 1e3,
                RATE(small_join[3]) / 1e3, RATE(small_fills) / 1e3);
        fprintf(stderr, "  slow mem on pages mapped under %u KB: %.0fK/s; at (MB, el)", (1u << HV_TLB_PAGE_BITS) / 1024, RATE(small_pages) / 1e3);
        HvStatKey at[8];
        hv_stats_keys(m, offsetof(HvPerCpu, small_at), at);
        for (int i = 0; i < 8 && at[i].n; i++)
            fprintf(stderr, " %llx/%d x%llu", (unsigned long long)(at[i].key >> 1), (int)(at[i].key & 1), (unsigned long long)at[i].n);
        fprintf(stderr, "\n");
        fprintf(stderr, "  slow mem K/s: single %.0f pair %.0f literal %.0f ldra %.0f ordered %.0f excl %.0f cas %.0f casp %.0f atomic %.0f vec %.0f/%.0f\n",
                RATE(slow_kind[0]) / 1e3, RATE(slow_kind[1]) / 1e3, RATE(slow_kind[2]) / 1e3, RATE(slow_kind[3]) / 1e3,
                RATE(slow_kind[4]) / 1e3, RATE(slow_kind[5]) / 1e3, RATE(slow_kind[6]) / 1e3, RATE(slow_kind[7]) / 1e3,
                RATE(slow_kind[8]) / 1e3, RATE(slow_kind[9]) / 1e3, RATE(slow_kind[10]) / 1e3);
        fprintf(stderr, "  ijc fills into: empty %.0fK/s, live other salt %.0fK/s, dead salt %.0fK/s, same salt %.0fK/s\n",
                RATE(fill_kind[0]) / 1e3, RATE(fill_kind[1]) / 1e3, RATE(fill_kind[3]) / 1e3, RATE(fill_kind[2]) / 1e3);
        fprintf(stderr, "  light fills into: empty %.0fK/s, live other salt %.0fK/s, dead salt %.0fK/s, same salt %.0fK/s\n",
                RATE(light_kind[0]) / 1e3, RATE(light_kind[1]) / 1e3, RATE(light_kind[3]) / 1e3, RATE(light_kind[2]) / 1e3);
        fprintf(stderr, "  misses at: RET %.0fK/s, BR/BLR %.0fK/s, B/BL %.0fK/s, conditional %.0fK/s, MSR %.0fK/s, other %.0fK/s, unknown %.0fK/s\n",
                RATE(miss_kind[0]) / 1e3, RATE(miss_kind[1]) / 1e3, RATE(miss_kind[2]) / 1e3, RATE(miss_kind[3]) / 1e3,
                RATE(miss_kind[4]) / 1e3, RATE(miss_kind[5]) / 1e3, RATE(miss_kind[6]) / 1e3);
        fprintf(stderr, "  entries %.2fM/s: chain fill %.0f%%, kick %.0f%%, exception/sysreg %.0f%%, interrupt %.0f%%, cached target %.0f%%, cache miss %.0f%%\n",
                tot / 1e6, 100 * RATE(why[0]) / tot, 100 * RATE(why[1]) / tot, 100 * RATE(why[2]) / tot,
                100 * RATE(why[3]) / tot, 100 * RATE(why[4]) / tot, 100 * RATE(why[5]) / tot);
    }
#undef RATE
    if (m->prof) print_profile(m);
    /* The five busiest system registers or SYS operations (reads + writes) this interval. */
    for (int k = 0; k < 14; k++) {
        uint64_t best = 0;
        uint32_t bi = 0;
        for (uint32_t r = 0; r < 65536; r++) {
            uint64_t n = (reg_hist[r] & 0xffffffff) + (reg_hist[r] >> 32);
            if (n > best) best = n, bi = r;
        }
        if (!best) break;
        fprintf(stderr, "   S%u_%u_C%u_C%u_%u: %.0f reads %.0f writes/sys per s\n", bi >> 14, (bi >> 11) & 7, (bi >> 7) & 15,
                (bi >> 3) & 15, bi & 7, (double)(reg_hist[bi] & 0xffffffff) / dt, (double)(reg_hist[bi] >> 32) / dt);
        reg_hist[bi] = 0;
    }
    memset(reg_hist, 0, sizeof reg_hist);
    last = *s;
    last_t = now;
    hv_fp_enter(cpu);
}

/* Fill the slot of the chainable exit a block just left through, so it jumps
   straight to b next time. Chains never need undoing: both blocks are in the
   same software (TLB) page, so in one RAM page, and when that page's code is invalidated its blocks are retired
   together: a chained jump into one lands on its gated word 0 and leaves for the dispatcher (hv_block_gate). */
static void chain(HvMachine *m, uint64_t *slot, HvBlock *b) {
    /* The exit's fields read below (slot_xa, slot_site, slot_cond) were written by the CPU that translated its block,
       and this CPU may have reached that block only through a link another CPU made (its B, or its slot read in
       hv_chain_thunk), never looking the block up itself. The translator stored them before publishing the block in
       the hash (release); the linking CPU found it there (acquire) before storing the slot and writing the B, then
       cleaned the cache (hv_jit_finish: DSB ISH, IC IVAU, DSB ISH, ISB). On this side only a control dependency is
       left (the branch went where that slot or B said), and Arm orders no later read by one. This isb does
       ([R]; ctrl; [ISB]; po; [R] in the Arm memory model) for the reads of those fields here. It does not cover what
       ran before this call through such a link (the thunk's loads, the block's literals): that is a separate
       question. TSan can't see a path through code that runs, so it is told of this one edge: the translator
       releases each exit's slot once its fields are set (hv_translate_place), acquired here. */
#if defined(__aarch64__)
    __asm__ volatile("isb" ::: "memory");
#endif
    uint32_t idx = (uint32_t)(slot - m->slots);
    /* Not from a zone being emptied, nor into one (the reclaimer: invariant 2) */
    uint32_t dying = __atomic_load_n(&m->zones_dying, __ATOMIC_RELAXED);
    if (dying && ((dying >> (idx / m->zone_slots) & 1) || hv_block_dying(m, b))) return;
    hv_tsan_acquire(&m->slot_xa[idx]);
    if (m->gen_checks && (!block_live(m, b) || (m->slot_site[idx] && !code_live(m, m->slot_site[idx]))))
        fprintf(stderr, "hengevm: chain slot %u site %p to rx+%llx va %llx: block gen %u, zone's %u\n", idx, (void *)m->slot_site[idx], (unsigned long long)(b->code - m->jit.base) * 4, (unsigned long long)b->va, b->jit_gen, m->zones[b->zone % HV_JIT_ZONES].born);
    uint64_t xa = m->slot_xa[idx];
    if (xa) {
        /* From another page: only to the block in the physical page the exit's
           check names, and not a stale one (see hv_invalidate_code). */
        /* The exit checked the target's entry in its block's table: a 4 KB one for a 4 KB-page process (cross_addend). */
        uint64_t cmask = (b->flags & HV_BLOCK_SMALL) && HV_TLB_SMALL ? (1ull << HV_TLB_SMALL_BITS) - 1 : (1ull << HV_TLB_PAGE_BITS) - 1;
        uint64_t host = (uint64_t)(uintptr_t)(b->ram->host + ((b->pa & ~cmask) - b->ram->pa));
        if (host - (b->va & ~cmask) != xa) return;
        pthread_mutex_lock(&m->tr_mu);
        dying = __atomic_load_n(&m->zones_dying, __ATOMIC_RELAXED); /* a mark is in a stop, which waits for this CPU: as above */
        if (__atomic_load_n(&b->ram->page_gen[b->pg], __ATOMIC_ACQUIRE) != b->gen || m->slot_xpg[idx] ||
            (dying >> (idx / m->zone_slots) & 1) || (dying >> b->zone & 1)) {
            pthread_mutex_unlock(&m->tr_mu);
            return;
        }
        hv_xlist_insert(m, b->ram, b->pg, idx); /* on the target's page's list, so a write to that page unchains it */
        __atomic_store_n(slot, (uint64_t)(uintptr_t)b->code, __ATOMIC_RELEASE);
        uint32_t *site = m->slot_site[idx];
        int64_t off = site ? b->code - site : 0;
        if (site && off >= -(1 << 25) && off < (1 << 25)) {
            uint32_t insn = 0x14000000u | (uint32_t)(off & 0x3ffffff);
            hv_jit_write(&m->jit, site, &insn, 4);
            hv_jit_finish_branch(&m->jit, site); /* B to B; the stub follows the slot meanwhile */
        }
        HV_STAT_ADD(m, chained, 1);
        if (m->stats_interval) HV_STAT_ADD(m, xchained, 1);
        pthread_mutex_unlock(&m->tr_mu);
        return;
    }
    __atomic_store_n(slot, (uint64_t)(uintptr_t)b->code, __ATOMIC_RELEASE);
    /* And the stub's B, when the target is in its range: a direct branch, no load. */
    uint32_t *site = m->slot_site[slot - m->slots];
    int64_t off = site ? b->code - site : 0;
    if (site && off >= -(1 << 25) && off < (1 << 25)) {
        uint32_t insn = 0x14000000u | (uint32_t)(off & 0x3ffffff);
        hv_jit_write(&m->jit, site, &insn, 4);
        hv_jit_finish_branch(&m->jit, site); /* B to B; the stub follows the slot meanwhile */
    }
    uint32_t *cond = m->slot_cond[idx];
    if (cond) { /* the conditional branch to that stub, too */
        uint32_t insn = *cond;
        if (hv_branch_retarget(&insn, b->code - cond)) {
            hv_jit_write(&m->jit, cond, &insn, 4);
            hv_jit_finish(&m->jit, cond, 1);
        }
    }
    HV_STAT_ADD(m, chained, 1);
}

void hv_jit_chain(HvMachine *m, uint64_t *slot, HvBlock *b) { chain(m, slot, b); }

static int run_loop(HvCpu *cpu, int one);

static void set_exited(HvCpu *cpu, int exited) {
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    if (!p) return;
    pthread_mutex_lock(&m->pause_mu);
    p->exited = exited;
    pthread_cond_broadcast(&m->pause_cv); /* a pause may be waiting for this CPU */
    pthread_mutex_unlock(&m->pause_mu);
}

static int run(HvCpu *cpu, int one) {
    /* hv_cpu_pass_stop's pc is for the stop the run comes to first: a run that starts elsewhere, or where the stop no
       longer is, drops it, as one that goes elsewhere first does (hv_check_interrupts taking one, wait_while_off).
       These drop it where they are, outside run_loop: its registers stay as they were for the check every block
       makes. */
    if (cpu->pc_state && (cpu->pc_state->stop_pass != cpu->pc || !hv_at_stop(cpu->m, cpu->pc)))
        cpu->pc_state->stop_pass = ~0ull;
    set_exited(cpu, 0);
    int r = run_loop(cpu, one);
    park(cpu);
    set_exited(cpu, 1);
    hv_self = NULL;
    return r;
}

/* Paused (hv_machine_pause): the CPU waits here, between guest instructions, parked and without the
   big lock, until the machine resumes or is halted. Everything the guest had is in HvCpu and RAM. */
static void pause_gate(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    hv_fp_leave(cpu); /* FPCR and FPSR into the HvCpu too, for a snapshot; read back on the way out */
    hv_big_unlock(m);
    park(cpu);
    pthread_mutex_lock(&m->pause_mu);
    p->at_gate = 1;
    pthread_cond_broadcast(&m->pause_cv);
    while (__atomic_load_n(&m->pause_req, __ATOMIC_ACQUIRE) && !__atomic_load_n(&cpu->halted, __ATOMIC_ACQUIRE))
        pthread_cond_wait(&m->pause_cv, &m->pause_mu);
    p->at_gate = 0;
    pthread_mutex_unlock(&m->pause_mu);
    hv_big_lock(m); /* still parked: see big_lock */
    unpark(cpu);
    hv_fp_enter(cpu);
}

/* Settled: no CPU can run guest code. Under pause_mu. A powered-off CPU counts; one turned on before
   its turner reached the gate comes to the gate itself before it runs anything (the loop checks the
   gate before running), so it is never in the guest while this says 1. */
static int pause_settled(HvMachine *m) {
    if (!m->pause_req) return 0;
    for (int i = 0; i < m->ncpus; i++) {
        HvCpu *c = m->cpus[i];
        if (!c || !c->pc_state) continue;
        HvPerCpu *p = c->pc_state;
        if (p->at_gate || p->exited) continue;
        if (__atomic_load_n(&c->parked_off, __ATOMIC_SEQ_CST) && __atomic_load_n(&c->off, __ATOMIC_SEQ_CST)) continue;
        return 0;
    }
    return 1;
}

static uint64_t paused_at; /* host time the machine settled, for hv_machine_resume (0: it never did) */

void hv_machine_pause(HvMachine *m) {
    pthread_mutex_lock(&m->pause_mu);
    if (!m->pause_req) paused_at = 0;
    __atomic_store_n(&m->pause_req, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&m->pause_mu);
    hv_machine_kick_all(m);
    for (int i = 0; i < m->ncpus; i++)
        if (m->cpus[i] && m->cpus[i]->pc_state) hv_cpu_wake(m->cpus[i]); /* out of WFI, to the gate */
}

int hv_machine_paused(HvMachine *m) {
    pthread_mutex_lock(&m->pause_mu);
    int r = pause_settled(m);
    if (r && !paused_at) paused_at = mach_absolute_time();
    pthread_mutex_unlock(&m->pause_mu);
    return r;
}

int hv_machine_pause_wait(HvMachine *m, unsigned timeout_ms) {
    uint64_t until = mach_absolute_time() + (uint64_t)timeout_ms * 24000;
    pthread_mutex_lock(&m->pause_mu);
    for (;;) {
        if (pause_settled(m)) {
            if (!paused_at) paused_at = mach_absolute_time();
            pthread_mutex_unlock(&m->pause_mu);
            return 0;
        }
        uint64_t now = mach_absolute_time();
        if (!m->pause_req || now >= until) break;
        pthread_mutex_unlock(&m->pause_mu);
        hv_machine_kick_all(m); /* a CPU past the gate check when the first kick came runs to its next exit */
        pthread_mutex_lock(&m->pause_mu);
        struct timespec ts = {0, 2000000}; /* 2 ms, or sooner when a CPU arrives */
        pthread_cond_timedwait_relative_np(&m->pause_cv, &m->pause_mu, &ts);
    }
    pthread_mutex_unlock(&m->pause_mu);
    return -1;
}

void hv_machine_resume(HvMachine *m) {
    pthread_mutex_lock(&m->pause_mu);
    if (m->pause_req && paused_at) {
        /* The guest saw nothing after paused_at: its clock picks up from there. Published before any
           CPU leaves the gate (they take pause_mu on the way out). */
        uint64_t now = mach_absolute_time();
        if (now > paused_at) __atomic_fetch_add(&hv_time_bias, now - paused_at, __ATOMIC_SEQ_CST);
    }
    paused_at = 0;
    __atomic_store_n(&m->pause_req, 0, __ATOMIC_SEQ_CST);
    pthread_cond_broadcast(&m->pause_cv);
    pthread_mutex_unlock(&m->pause_mu);
}

void hv_machine_resume_live(HvMachine *m) {
    pthread_mutex_lock(&m->pause_mu);
    paused_at = 0; /* the guest's clock ran on through the stop, as the real one did */
    __atomic_store_n(&m->pause_req, 0, __ATOMIC_SEQ_CST);
    pthread_cond_broadcast(&m->pause_cv);
    pthread_mutex_unlock(&m->pause_mu);
}

/* Why pause_settled said no, in one line (under pause_mu): the first CPU still able to run guest code, or no
   pause asked for. Only where a caller relied on the machine being paused and it wasn't, so not in the waits. */
static void say_unsettled(HvMachine *m) {
    if (!m->pause_req) {
        fprintf(stderr, "hengevm: pause: not settled: no pause requested\n");
        return;
    }
    for (int i = 0; i < m->ncpus; i++) {
        HvCpu *c = m->cpus[i];
        if (!c || !c->pc_state) continue;
        HvPerCpu *p = c->pc_state;
        int off = __atomic_load_n(&c->off, __ATOMIC_SEQ_CST), parked = __atomic_load_n(&c->parked_off, __ATOMIC_SEQ_CST);
        if (p->at_gate || p->exited || (parked && off)) continue;
        fprintf(stderr, "hengevm: pause: not settled: cpu %d at_gate %d exited %d off %d parked_off %d active %d\n", i,
                p->at_gate, p->exited, off, parked, p->active);
        return;
    }
}

int hv_machine_paused_counter(HvMachine *m, uint64_t *counter) {
    pthread_mutex_lock(&m->pause_mu);
    int r = pause_settled(m) ? 0 : -1;
    if (r == 0) {
        if (!paused_at) paused_at = mach_absolute_time();
        *counter = paused_at - __atomic_load_n(&hv_time_bias, __ATOMIC_SEQ_CST);
    } else {
        say_unsettled(m);
    }
    pthread_mutex_unlock(&m->pause_mu);
    return r;
}

void hv_machine_restore_paused(HvMachine *m, uint64_t counter) {
    pthread_mutex_lock(&m->pause_mu);
    __atomic_store_n(&m->pause_req, 1, __ATOMIC_SEQ_CST);
    paused_at = mach_absolute_time();
    /* The counter reads `counter` at paused_at; resuming adds the time from here on, as for any pause.
       Wraps when this host's clock is behind the saved one (after a reboot): the arithmetic is modular. */
    __atomic_store_n(&hv_time_bias, paused_at - counter, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&m->pause_mu);
}

void hv_machine_pause_poke(HvMachine *m) {
    pthread_mutex_lock(&m->pause_mu);
    pthread_cond_broadcast(&m->pause_cv);
    pthread_mutex_unlock(&m->pause_mu);
}

/* A CPU off this long gives back what it holds for running (hv_cpu_off_give_back): its TLB, return and ASID tables
   and branch caches, up to ~9 MB, with ~3.5 MB more since jc, the filters, the chunk set and the stashes go too.
   Measured on this MacBook (load ~40) for a CPU that used all 4.7 MB of its HvCpu tables: the give-back 0.13 ms
   (max 0.25), and its CPU_ON with everything used again 0.2 ms more than if kept (302 page faults; max 0.45):
   ~0.35 ms a cycle, ~0.7 ms with full branch caches, under 0.1% of a second off.
   Much shorter, and a CPU macOS parks and starts again often would pay it each time for memory it is about to use;
   much longer, and a parked CPU's memory waits for a trim. */
#define HV_OFF_GIVEBACK_MS 1000

/* The give-back of a CPU that stayed off, on its own thread. Active meanwhile, as unpark, so no stop runs during it
   (a flush writes every parked CPU's caches) and the stopper's trim leaves its branch caches to it; but it doesn't
   wait for a stop that is on: CPU_ON may be spinning on parked_off inside a CPU that stop waits for. 0: a stop
   was on, nothing done. */
static int off_give_back(HvCpu *cpu) {
    HvPerCpu *p = cpu->pc_state;
    HvMachine *m = cpu->m;
    __atomic_store_n(&p->active, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&m->stw_active, __ATOMIC_SEQ_CST)) {
        park(cpu);
        return 0;
    }
    while (__atomic_load_n(&p->ijc_dropping, __ATOMIC_SEQ_CST)) sched_yield(); /* the stopper is giving them back */
    uint64_t t0 = mach_absolute_time(), freed = hv_cpu_off_give_back(cpu);
    HV_STAT_ADD(m, giveback_ticks, mach_absolute_time() - t0);
    HV_STAT_ADD(m, off_givebacks, 1);
    HV_STAT_ADD(m, off_released, freed);
    park(cpu);
    /* A trim whose stopper found it active left its branch caches to it: given back now, parked, as that stopper does
       for a parked CPU (only the one that takes ijc_giveback does it). Checked after parking, so a trim after this
       finds it parked and does it itself. */
    if (__atomic_load_n(&p->ijc_giveback, __ATOMIC_SEQ_CST) && __atomic_exchange_n(&p->ijc_giveback, 0, __ATOMIC_SEQ_CST))
        hv_machine_give_back_done(m, hv_cpu_ijc_give_back(p));
    fprintf(stderr, "hengevm: cpu %u stayed off: gave back %.1f MB in %.2f ms\n", cpu->index, freed / 1048576.0,
            (mach_absolute_time() - t0) / 24000.0);
    return 1;
}

/* A powered-off CPU waits here, without the big lock, for PSCI CPU_ON. Once it has been off a while, what it holds
   for running goes back (off_give_back): not while parked_off, which CPU_ON waits for before resetting it and a pause
   for before calling the machine settled, and not while a pause is on (a save may be reading the machine).
   parked_off changes only under wake_mu (CPU_ON reads it again under it). Its clear for a give-back and its set
   after it are under pause_mu too. Its clear when the CPU leaves isn't, and needn't be: off is clear by then (CPU_ON)
   or the machine is halting, and pause_settled counts an off CPU settled only while parked_off and off both read 1. */
static void wait_while_off(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    p->stop_pass = ~0ull; /* it comes back at its entry point: hv_cpu_pass_stop's pc is not where it goes first */
    hv_fp_leave(cpu);
    hv_big_unlock(m);
    park(cpu);
    uint64_t wait_ns = (m->off_giveback_ms ? m->off_giveback_ms : HV_OFF_GIVEBACK_MS) * 1000000ull;
    uint64_t due = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + wait_ns;
    /* Holds anything: it ran since its reset (its TLB ready) or its branch caches hold pages (a CPU that never ran,
       a secondary waiting for its first CPU_ON, holds neither, and waits without waking) */
    int holds = hv_tlb_is_ready(cpu) || __atomic_load_n(&p->ijc_tables_held, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&p->wake_mu);
    __atomic_store_n(&cpu->parked_off, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&cpu->off, __ATOMIC_ACQUIRE) && !cpu->halted) {
        p->wake_pending = 0;
        if (!holds) {
            pthread_cond_wait(&p->wake_cv, &p->wake_mu);
            continue;
        }
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (now < due) {
            struct timespec ts = {(time_t)((due - now) / 1000000000ull), (long)((due - now) % 1000000000ull)};
            pthread_cond_timedwait_relative_np(&p->wake_cv, &p->wake_mu, &ts);
            continue;
        }
        /* Whether a pause is on, and parked_off cleared if not, in one step under pause_mu, which a pause sets
           pause_req under and pause_settled reads parked_off under: a pause either finds the CPU unsettled and waits
           for it, or came first and finds it settled for as long as it lasts (a save checks that once, later). */
        pthread_mutex_lock(&m->pause_mu);
        int paused = m->pause_req, done = 0;
        if (!paused) __atomic_store_n(&cpu->parked_off, 0, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&m->pause_mu);
        if (!paused) {
            pthread_mutex_unlock(&p->wake_mu); /* other CPUs' wakes and kicks take it */
            done = off_give_back(cpu);
            pthread_mutex_lock(&p->wake_mu);
            pthread_mutex_lock(&m->pause_mu);
            __atomic_store_n(&cpu->parked_off, 1, __ATOMIC_SEQ_CST);
            pthread_cond_broadcast(&m->pause_cv); /* a pause that came meanwhile waits for this */
            pthread_mutex_unlock(&m->pause_mu);
        }
        if (done) holds = 0;
        else due = now + (paused ? wait_ns : wait_ns / 64); /* a pause can last (a suspended Mac); a stop doesn't */
    }
    __atomic_store_n(&cpu->parked_off, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&p->wake_mu);
    hv_big_lock(m); /* still parked: see big_lock */
    unpark(cpu);
    hv_fp_enter(cpu); /* CPU_ON reset it: FPCR and FPSR start at 0 */
}

/* Dropping every translation: the CPU that finds a flush pending waits until
   all the others have parked (at this barrier, asleep or off), flushes, and
   lets them go. Nobody holds a lock while waiting here. */
static void stop_the_world(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    pthread_mutex_lock(&m->stw_mu);
    if (__atomic_load_n(&m->flush_pending, __ATOMIC_SEQ_CST) && !m->stw_active) {
        __atomic_store_n(&m->stw_active, 1, __ATOMIC_SEQ_CST);
        __atomic_store_n(&m->flush_pending, 0, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&m->stw_mu);
        uint64_t stop_began = mach_absolute_time();
        hv_machine_kick_all(m);
        /* No time limit: emptying code while a CPU may be in it is not an option. A line once it has waited a second,
           naming the CPUs not parked yet and where they are, to match a freeze with the stall log. */
        uint64_t waited = 0;
        for (uint32_t spins = 0;; spins++) {
            int busy = 0;
            for (int i = 0; i < m->ncpus; i++)
                if (m->cpus[i] && m->cpus[i] != cpu && __atomic_load_n(&m->cpus[i]->pc_state->active, __ATOMIC_SEQ_CST)) busy = 1;
            if (!busy) break;
            hv_cpu_process_requests(cpu); /* one of them may be waiting on a TLBI of ours */
            if (spins <= 100) continue;
            sched_yield();
            if (!waited) waited = mach_absolute_time();
            else if (waited != 1 && mach_absolute_time() - waited > 24000000) { /* 1 s of 24 MHz ticks */
                char who[256];
                int n = 0;
                for (int i = 0; i < m->ncpus && n < (int)sizeof who - 40; i++)
                    if (m->cpus[i] && m->cpus[i] != cpu && __atomic_load_n(&m->cpus[i]->pc_state->active, __ATOMIC_SEQ_CST))
                        n += snprintf(who + n, sizeof who - (size_t)n, " cpu %d at %llx", i,
                                      (unsigned long long)__atomic_load_n(&m->cpus[i]->pc, __ATOMIC_RELAXED));
                who[n] = 0;
                fprintf(stderr, "hengevm: cpu %u has waited 1 s to stop the others (%s); still running:%s\n", cpu->index,
                        m->flush_all ? "full flush" : "making room", who);
                waited = 1;
            }
        }
        if (m->gen_checks) for (int i = 0; i < m->ncpus; i++) if (m->cpus[i] && m->cpus[i] != cpu && m->cpus[i]->pc_state->active) fprintf(stderr, "hengevm: flush while cpu %d is active\n", i);
        pthread_mutex_lock(&m->tr_mu);
        if (__atomic_exchange_n(&m->flush_all, 0, __ATOMIC_SEQ_CST)) /* taken, so one asked meanwhile stays */
            flush_blocks(m, __atomic_exchange_n(&m->trim_pending, 0, __ATOMIC_SEQ_CST));
        else if (m->room_gen == m->jit_gen + 1) make_room(m); /* asked for this generation: not made yet */
        else if (m->room_early == m->jit_gen + 1 && m->room_early_seen != m->room_early) reclaim_mark_next(m);
        pthread_mutex_unlock(&m->tr_mu);
        uint64_t took = mach_absolute_time() - stop_began; /* still stopped: one stop at a time, only the max is shared */
        hv_count_own(&m->stats.stops, 1); /* one writer at a time (this stop), read from anywhere */
        hv_count_own(&m->stats.stop_ticks, took);
        for (uint64_t was = __atomic_load_n(&m->stats.stop_max_ticks, __ATOMIC_RELAXED);
             took > was && !__atomic_compare_exchange_n(&m->stats.stop_max_ticks, &was, took, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);) {
        }
        pthread_mutex_lock(&m->stw_mu);
        __atomic_store_n(&m->stw_active, 0, __ATOMIC_SEQ_CST);
        pthread_cond_broadcast(&m->stw_cv);
        pthread_mutex_unlock(&m->stw_mu);
        hv_machine_give_back(m, cpu); /* a trim's tables, with the others running: the next stop waits for this */
    } else if (m->stw_active) {
        uint64_t held_since = mach_absolute_time();
        park(cpu);
        while (m->stw_active) pthread_cond_wait(&m->stw_cv, &m->stw_mu);
        pthread_mutex_unlock(&m->stw_mu);
        unpark(cpu);
        __atomic_add_fetch(&m->stats.stop_wait_ticks, mach_absolute_time() - held_since, __ATOMIC_RELAXED);
    } else {
        pthread_mutex_unlock(&m->stw_mu);
    }
}

int hv_run(HvCpu *cpu) {
    big_lock(cpu);
    hv_rosetta_fp_apply(cpu);
    hv_fp_enter(cpu);
    int r = run(cpu, 0);
    hv_fp_leave(cpu);
    hv_big_unlock(cpu->m);
    return r;
}

int hv_run_one(HvCpu *cpu) {
    big_lock(cpu);
    hv_rosetta_fp_apply(cpu);
    hv_fp_enter(cpu);
    int r = run(cpu, 1);
    hv_fp_leave(cpu);
    hv_big_unlock(cpu->m);
    return r;
}

/* Bring-up aid (log >= 4): report the first block entered with a host address
   in a guest register, which means the translator leaked one. */
static int host_pointer_check(HvCpu *cpu, uint64_t prev_pc) {
    HvMachine *m = cpu->m;
    for (int r = 0; r < 31; r++) {
        uint64_t v = cpu->x[r];
        const char *what = NULL;
        for (int i = 0; i < m->nram && !what; i++)
            if (v - (uint64_t)(uintptr_t)m->ram[i].host < m->ram[i].size && m->ram[i].pa != (uint64_t)(uintptr_t)m->ram[i].host)
                what = "host copy of guest RAM";
        if (v - (uint64_t)(uintptr_t)m->jit.base < m->jit.capacity * 4) what = "JIT region";
        if (v - (uint64_t)(uintptr_t)cpu < sizeof *cpu) what = "HvCpu";
        if (what) {
            fprintf(stderr, "host pointer: x%d = %llx (%s) entering %llx, previous block %llx\n", r,
                    (unsigned long long)v, what, (unsigned long long)cpu->pc, (unsigned long long)prev_pc);
            return 1;
        }
    }
    return 0;
}

static int run_loop(HvCpu *cpu, int one) {
    HvMachine *m = cpu->m;
    uint32_t since_tick = 0;
    uint64_t next_due = 0; /* host time of this CPU's next timer event, from its last tick (0: none armed) */
    uint64_t last_fault = ~0ull, prev_pc = 0;
    HvBlock *prev_b = NULL;
    uint64_t *pending_chain = NULL;
    uint64_t chain_pc = 0;    /* where that exit was going, and in which state */
    uint32_t chain_flags = 0;
    uint32_t chain_gen = 0;   /* and the code cache's generation then: after a flush or a zone emptied, the slot may be another's */
    uint32_t last_reason = HV_EXIT_BRANCH;
    uint64_t seen_ack = 0;    /* the reclaimer's count when pending_chain and prev_b were last forgotten for it */
    hv_self = cpu;
    unpark(cpu);
    for (;;) {
        if (__builtin_expect(__atomic_load_n(&m->flush_pending, __ATOMIC_ACQUIRE) || __atomic_load_n(&m->stw_active, __ATOMIC_ACQUIRE), 0)) {
            stop_the_world(cpu);
            pending_chain = NULL;
            prev_b = NULL;
            continue;
        }
        /* The reclaimer asks every CPU to pass here (its grace period): answered, or answered as it unparked */
        if (__builtin_expect(__atomic_load_n(&m->reclaim_seq, __ATOMIC_RELAXED) != seen_ack, 0)) {
            if (__atomic_load_n(&m->reclaim_seq, __ATOMIC_RELAXED) != cpu->pc_state->reclaim_ack) reclaim_ack(cpu);
            seen_ack = cpu->pc_state->reclaim_ack;
            pending_chain = NULL;
            prev_b = NULL;
        }
        if (__builtin_expect(cpu->pc_state && __atomic_load_n(&cpu->pc_state->req_seq, __ATOMIC_ACQUIRE) != __atomic_load_n(&cpu->pc_state->ack_seq, __ATOMIC_RELAXED), 0))
            hv_cpu_process_requests(cpu);
        if (cpu->halted) return HV_EXIT_HALT;
        if (__atomic_load_n(&m->pause_req, __ATOMIC_ACQUIRE)) {
            pause_gate(cpu); /* comes back resumed, or halted */
            pending_chain = NULL;
            prev_b = NULL;
            continue;
        }
        if (cpu->off) {
            wait_while_off(cpu); /* comes back after CPU_ON, reset to its entry point */
            pending_chain = NULL;
            last_reason = HV_EXIT_EXCEPTION;
            continue;
        }
        /* Someone asked the CPU to leave (the 1 ms kicker, a device, an invalidation): service devices now. */
        int kicked = __atomic_exchange_n(&cpu->exit_req, 0, __ATOMIC_ACQUIRE) != 0;
        if (kicked && m->nzones > 1) sample_zone(cpu);
        /* A request posted after the check above set exit_req once more, and clearing it
           just now would leave the request waiting for some later kick (a TLBI's sender
           spins on it meanwhile, and a branch-cache reset could be missed): look again
           now that the kick is consumed. It costs one load when nothing is queued. */
        if (kicked && cpu->pc_state && __atomic_load_n(&cpu->pc_state->req_seq, __ATOMIC_ACQUIRE) != __atomic_load_n(&cpu->pc_state->ack_seq, __ATOMIC_RELAXED))
            hv_cpu_process_requests(cpu);
        if (kicked && m->big_lock_on) {
            /* Give another CPU its turn. Whatever it does in between may touch
               what this CPU's exclusive monitor watches, so the monitor clears. */
            cpu->excl_addr = ~0ull;
            if (hv_big_contended(m)) {
                hv_big_unlock(m);
                big_lock(cpu);
            }
        }
        /* Ticks take the device lock, so not every few blocks: on a kick (the 1 ms
           kicker, a device with work for CPU 0, an interrupt raised for this
           CPU) or once this CPU's own timer is due. */
        if (m->tick && (kicked || (++since_tick >= 64 && (since_tick = 0, next_due && mach_absolute_time() >= next_due)))) {
            since_tick = 0;
            next_due = m->tick(m, cpu);
            cpu->timer_due = next_due;
        }
        int why = pending_chain ? 0 : kicked ? 1 : last_reason != HV_EXIT_BRANCH ? 2 : 5;
        if (hv_check_interrupts(cpu)) {
            pending_chain = NULL;
            why = 3;
        }
        if (hv_at_stop(m, cpu->pc)) {
            HvStopAt stop = hv_stop_hook(m, cpu->pc, cpu->index);
            /* Every CPU comes back through here at the stop, the CPUs that run past it too: a jump this one chained
               into it would take the CPUs it is for past it as well, for good. */
            pending_chain = NULL;
            /* the first stop it comes to uses up the pass, one not for this CPU too */
            if (cpu->pc_state->stop_pass == cpu->pc) stop.here = 0, cpu->pc_state->stop_pass = ~0ull;
            if (stop.here) {
                if (!stop.hook) return HV_EXIT_HALT;
                stop.hook(cpu);
            }
        }

        if ((m->log || m->gen_checks) && cpu->el && (cpu->sctlr_el1 & 1) && !((cpu->pc >> 55) & 1) && cpu->pc >= 0x100000000ull) { /* EL1 at a user address: how did we get here? */
            static int reported;
            if (reported++ < 4) {
                fprintf(stderr, "hengevm: el1 at user pc %llx: previous block %llx flags %x (%u insns), exit reason %u, why %d, "
                                "miss site %llx, elr %llx spsr %llx x30 %llx, fast resumes %llu\n  recent:",
                        (unsigned long long)cpu->pc, (unsigned long long)prev_pc, prev_b ? prev_b->flags : 0, prev_b ? prev_b->ninsns : 0,
                        last_reason, why, (unsigned long long)cpu->miss_site, (unsigned long long)cpu->elr_el1,
                        (unsigned long long)cpu->spsr_el1, (unsigned long long)cpu->x[30], (unsigned long long)HV_CPU_STAT(cpu, fast_resumes));
                const HvTrace *tr = &cpu->pc_state->trace;
                for (int k = 0; k < 16; k++)
                    fprintf(stderr, " %llx", (unsigned long long)tr->recent[(tr->nrecent + k) & 15]);
                fprintf(stderr, "\n");
            }
        }
        if (m->log && !cpu->el && ((cpu->pc >> 55) & 1)) { /* EL0 can never run kernel code: say how we got here */
            static int reported;
            if (reported++ < 4)
                fprintf(stderr, "hengevm: el0 at kernel pc %llx: previous block %llx, exit reason %u, why %d, miss site %llx, "
                                "elr %llx spsr %llx x30 %llx\n",
                        (unsigned long long)cpu->pc, (unsigned long long)prev_pc, last_reason, why,
                        (unsigned long long)cpu->miss_site, (unsigned long long)cpu->elr_el1,
                        (unsigned long long)cpu->spsr_el1, (unsigned long long)cpu->x[30]);
        }
        uint64_t pa;
        uint32_t avail;
        const uint32_t *code = hv_mmu_fetch(cpu, cpu->pc, &pa, &avail);
        if (!code) { /* the instruction abort was taken */
            /* The pc is the vector now, not the branch target: the exit that got
               here must not be chained to whatever runs next (that put EL0 code
               on a path straight into the kernel's vector code). */
            pending_chain = NULL;
            if (cpu->pc == last_fault) return HV_EXIT_FAULT_LOOP;
            last_fault = cpu->pc;
            continue;
        }
        last_fault = ~0ull;
        uint32_t gen0 = __atomic_load_n(&m->jit_gen, __ATOMIC_SEQ_CST);
        int once;
        uint32_t flags = (pending_chain && cpu->pc == chain_pc) ? chain_flags : block_flags(cpu);
        HvBlock *b = lookup(cpu, cpu->pc, pa, flags, code, avail, &once);
        if (__builtin_expect(m->gen_checks, 0)) found_unchained = b; /* park checks the rule at the chain below */
        if (!b) {
            found_unchained = NULL;
            /* Room is about to be made, or just was (another CPU may have
               done what this lookup asked for): try again after. All three
               read in this order, each seq_cst: a stopper sets stw_active
               before it takes the request, and can't make room or let go
               while this CPU is active, so a request seen taken means the stop
               is seen. (A plain read of flush_pending could be satisfied after
               the others: an old stw_active and a taken request, and this CPU
               gave up while the stop waited for it.) */
            if (__atomic_load_n(&m->flush_pending, __ATOMIC_SEQ_CST) || __atomic_load_n(&m->stw_active, __ATOMIC_SEQ_CST) ||
                __atomic_load_n(&m->jit_gen, __ATOMIC_SEQ_CST) != gen0)
                continue;
            fprintf(stderr, "hengevm: cannot translate block at %llx\n", (unsigned long long)cpu->pc);
            park(cpu);
            return HV_EXIT_UNSUPPORTED;
        }
        /* Remember it for indirect branches (not the tests' stop PC: that must reach us). */
        /* EL0 tables only hold user VAs and EL1 tables kernel VAs, so each half has its own salt. */
        int kernel = b->flags & HV_BLOCK_EL1;
        if (!once && m->chain && !hv_at_stop(m, b->va) && (int)((b->va >> 55) & 1) == kernel) {
            HvPerCpu *pcs = cpu->pc_state;
            uint64_t *t = pcs->ijc + (size_t)hv_ijc_table(b->flags) * (HV_IJC_TABLE_BYTES / 8);
            uint64_t tag = b->va ^ cpu->ijc_salt[kernel];
            uint64_t *ent = t + (size_t)hv_ijc_index(tag) * 2;
            if (why == 5 && (ent[0] == tag || ent[2] == tag)) {
                why = 4; /* it was cached: not a miss */
                static int dbg = -1, shown;
                if (dbg < 0) dbg = getenv("HV_IJC_DEBUG") != NULL;
                if (dbg) { /* what instruction came just before the target (put_exit leaves with pc + 4) */
                    static uint64_t hist[4], total;
                    uint32_t before = 0;
                    uint64_t off = b->pa - b->ram->pa;
                    if (off & ((1ull << HV_TLB_PAGE_BITS) - 1)) memcpy(&before, b->ram->host + off - 4, 4);
                    int k = cpu->miss_site ? 0 : before == 0xd5033fdf ? 1 : (before & 0xfff00000u) == 0xd5100000u ? 2 : 3;
                    hist[k]++;
                    if (++total % 200000 == 0)
                        fprintf(stderr, "ijc cached-miss kinds: ijc site %llu, after ISB %llu, after MSR %llu, other %llu\n",
                                (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2],
                                (unsigned long long)hist[3]);
                    static uint64_t msrs;
                    if (k == 2 && ++msrs % 20000 == 0)
                        fprintf(stderr, "ijc cached-miss msr: S%u_%u_C%u_C%u_%u\n", 2 + ((before >> 19) & 1), (before >> 16) & 7,
                                (before >> 12) & 15, (before >> 8) & 15, (before >> 5) & 7);
                    static uint64_t others;
                    if (k == 3 && ++others % 50000 == 0 && shown < 60) {
                        shown++;
                        fprintf(stderr, "ijc cached-miss other: target %llx el%u reason %u, insn before %08x\n", (unsigned long long)b->va, cpu->el, last_reason, before);
                    }
                }
                if (0) {
                    shown++;
                    uint32_t last = 0;
                    if (prev_b && prev_b->ninsns)
                        memcpy(&last, prev_b->ram->host + (prev_b->pa - prev_b->ram->pa) + 4 * (prev_b->ninsns - 1), 4);
                    fprintf(stderr, "ijc cached-miss: target %llx flags %x from site %llx (prev block %llx flags %x, %u insns, last %08x, reason %u)\n",
                            (unsigned long long)b->va, b->flags, (unsigned long long)cpu->miss_site,
                            (unsigned long long)prev_pc, prev_b ? prev_b->flags : 0, prev_b ? prev_b->ninsns : 0, last, last_reason);
                }
            }
            if (m->stats_interval) { /* what the fill replaces: nothing (cold), another salt's entry, or another target */
                uint64_t old = ent[0];
                int kind = !old ? 0 : (old >> 48) != (tag >> 48) ? 1 : 2;
                if (kind == 1) { /* another salt: is it still in use, or dead (dropped address space, reset)? */
                    uint32_t c = (uint32_t)(old >> 48);
                    int alive = kernel ? (c ^ 0xffff) == cpu->kernel_gen : cpu->asid_cid[cpu->cid_asid[c]] == c;
                    if (!alive) kind = 3;
                }
                HV_CPU_COUNT(cpu, fill_kind[kind], 1);
            }
            if (hv_ijc_fill_checked(cpu, b) && m->stats_interval) HV_CPU_COUNT(cpu, ijc_fills, 1);
        }
        /* The block before left through a chainable direct branch to this one: link them. */
        /* Only if nothing in between (an abort, an interrupt) sent the CPU elsewhere. */
        /* Not across a flush or a zone emptied (this CPU may have been parked since, as in big_lock): the slot
           the last exit came through may be gone, and new blocks may have it now: chaining it would rewrite theirs.
           The rule the reclaimer rests on (one grace period, after its hash phase; none before it): this CPU never
           parks between lookup returning b and this chain to it. A zone is marked dying only in a stop, which a
           running CPU joins parked, so a b of it returned here was chained (or not) before the mark, and from the
           mark on lookup misses the zone's blocks and chain() refuses one. A CPU that parked in between would let
           the stop mark b's zone and a grace period pass it, then chain an exit into the zone after the hash phase
           had scanned that slot. gen_checks: park aborts if it ever does (found_unchained). */
        if (pending_chain && !once && !m->trace && b->va == chain_pc && b->flags == chain_flags && chain_gen == __atomic_load_n(&m->jit_gen, __ATOMIC_SEQ_CST))
            chain(m, pending_chain, b);
        pending_chain = NULL;
        if (__builtin_expect(m->gen_checks, 0)) found_unchained = NULL;
        if (m->trace) fprintf(stderr, "block %llx el%u\n", (unsigned long long)cpu->pc, cpu->el);
        if (m->log >= 4 && host_pointer_check(cpu, prev_pc)) return HV_EXIT_UNSUPPORTED;
        prev_pc = cpu->pc;
        prev_b = b;
        if (m->stats_interval) HV_CPU_COUNT(cpu, why[why], 1);
        cpu->pc_state->trace.recent[cpu->pc_state->trace.nrecent++ & 15] = cpu->pc | cpu->el;
        if (why == 5 && m->stats_interval) {
            int kind = 6;
            /* A plain walk: fetching could raise an abort in the guest. */
            HvWalk w;
            uint32_t i = 0;
            if (cpu->miss_site) hv_mmu_walk(cpu, cpu->miss_site, HV_ACC_EXEC, cpu->el, &w);
            if (cpu->miss_site && !w.fsc && hv_phys_read(m, w.pa, &i, 4) == 0) {
                if ((i & 0xfffffc1fu) == 0xd65f0000u || (i & 0xfffffbffu) == 0xd65f0bffu) kind = 0;         /* RET, RETAA/AB */
                else if ((i & 0xfe9f0000u) == 0xd61f0000u) kind = 1; /* BR, BLR and their PAC forms (bit 24) */
                else if ((i & 0x7c000000u) == 0x14000000u) kind = 2;                                         /* B, BL */
                else if ((i & 0xff000010u) == 0x54000000u || (i & 0x7e000000u) == 0x34000000u ||
                         (i & 0x7e000000u) == 0x36000000u) kind = 3;                                        /* B.cond, CB(N)Z, TB(N)Z */
                else if ((i & 0xfff8f01fu) == 0xd500401fu) kind = 4;                                         /* MSR (immediate) */
                else kind = 5;
            }
            HV_CPU_COUNT(cpu, miss_kind[kind], 1);
        }
        cpu->miss_site = 0; /* only the next lookup miss sets it again */
        cpu->exit_reason = HV_EXIT_BRANCH;
        cpu->last_stub = 0;
        cpu->icount++;
        HV_CPU_COUNT(cpu, blocks, 1);
        if (m->stats_interval && cpu->index == 0 && (HV_CPU_STAT(cpu, blocks) & 0xffff) == 0) print_stats(cpu, 0);
        if (m->gen_checks && !block_live(m, b))
            fprintf(stderr, "hengevm: cpu %u entering %p (rx+%llx, zone %u) for va %llx: block gen %u, zone's %u, maker %u\n", cpu->index, (void *)b->code, (unsigned long long)(b->code - m->jit.base) * 4, b->zone, (unsigned long long)b->va, b->jit_gen, m->zones[b->zone % HV_JIT_ZONES].born, b->maker);
        hv_enter(cpu, b->code + once); /* past the gate of a block made from changed bytes, this once */

        if (cpu->exit_reason == HV_EXIT_BRANCH && cpu->last_stub && m->chain) {
            pending_chain = (uint64_t *)cpu->last_stub;
            chain_gen = __atomic_load_n(&m->jit_gen, __ATOMIC_SEQ_CST);
            chain_pc = cpu->pc;
            chain_flags = block_flags(cpu); /* the exiting block's state (a block ends where its state changes) */
        }
        last_reason = cpu->exit_reason;
        switch (cpu->exit_reason) {
        case HV_EXIT_BRANCH:
        case HV_EXIT_EXCEPTION:
        case HV_EXIT_SYSREG:
        case HV_EXIT_FLUSH:
            break;
        case HV_EXIT_WFI:
            if (wait_for_interrupt(cpu) != 0) return HV_EXIT_WFI;
            break;
        default:
            return (int)cpu->exit_reason;
        }
        if (one) return HV_EXIT_BRANCH;
    }
}

/* hv_classify copies SIMD and FP instructions that name no general register without
   checking they exist (there are too many forms to decode, and getting one wrong would
   break real code). The host is the judge: one it does not have raises SIGILL right at
   the copied word, with every guest register where translated code keeps it, as at the
   start of that instruction. So it can still be the guest's own undefined-instruction
   exception, made the way a translated UDF makes it (put_helper with HV_OP_UNDEF).
   Which guest instruction that word is: the faulting word is one the host cannot run,
   which we never emit ourselves, so it is a copied guest word; the translator recorded
   where each of those went (HvBlock.undef_at). Runs on the faulting CPU's own thread,
   inside a block. */
int hv_jit_sigill(HvMachine *m, void *ucontext) {
    ucontext_t *uc = ucontext;
    if (!m || !uc || !m->jit.base || !m->blocks) return 0;
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    uint64_t pc = ss->__pc, rx = (uint64_t)(uintptr_t)m->jit.base;
    if (pc < rx || pc >= rx + (uint64_t)m->jit.capacity * 4 || (pc & 3)) return 0;
    HvCpu *cpu = (HvCpu *)(uintptr_t)ss->__x[28];
    int known = 0;
    for (int i = 0; i < m->ncpus; i++) known |= m->cpus[i] == cpu;
    if (!known || !cpu) return 0;
    uint32_t word = *(const uint32_t *)(uintptr_t)pc;
    if (hv_classify(word) != HV_INSN_COPY || hv_gpr_fields(word)) return 0;
    /* The block whose hot code holds pc: the last one starting at or before it, in pc's zone. */
    int z = hv_jit_zone_of(m, (const void *)(uintptr_t)pc);
    if (z < 0) return 0;
    uint32_t born = m->zones[z].born, nblocks = zone_nblocks(m, z);
    HvBlock *b = NULL, *b0 = m->blocks + (size_t)z * m->zone_blocks;
    for (uint32_t i = 0; i < nblocks; i++) {
        HvBlock *c = &b0[i];
        uint64_t at = (uint64_t)(uintptr_t)c->code;
        if (c->jit_gen == born && at && at <= pc && (!b || at > (uint64_t)(uintptr_t)b->code)) b = c;
    }
    if (!b || !b->ram || !b->ninsns) return 0;
    uint32_t i = 0;
    if (b->nundef != HV_UNDEF_UNKNOWN) {
        /* Which guest instruction, from the translator's own record: right even when
           the guest has rewritten the page since (another CPU, or a store earlier in
           this very block), as the old instruction is what this CPU fetched. */
        uint64_t at = (pc - (uint64_t)(uintptr_t)b->code) / 4;
        uint32_t k = 0;
        while (k < b->nundef && m->undef_map[b->undef_at + k] >> 8 != at) k++;
        if (k == b->nundef) return 0; /* not a copied guest word: a translator bug, so the crash it is */
        i = m->undef_map[b->undef_at + k] & 0xff;
    } else {
        /* Not recorded (the record was full): the first such word in the guest's
           bytes, if they are still those the block was made from. */
        if (__atomic_load_n(&b->ram->page_gen[b->pg], __ATOMIC_ACQUIRE) != b->gen) return 0;
        const uint32_t *guest = hv_phys_ptr_r(m, b->pa, 4ull * b->ninsns);
        if (!guest) return 0;
        while (i < b->ninsns && guest[i] != word) i++;
    }
    if (i >= b->ninsns) return 0;
    /* What the call site would do: str x30, [x28, #x30]; adr x6, info; blr hv_helper_thunk. */
    cpu->sigill_info[0] = b->va + 4ull * i;
    cpu->sigill_info[1] = (uint64_t)word | (uint64_t)HV_OP_UNDEF << 32;
    cpu->x[30] = ss->__lr;
    ss->__x[6] = (uint64_t)(uintptr_t)cpu->sigill_info;
    ss->__lr = pc + 4;
    ss->__pc = cpu->helper_thunk;
    __atomic_add_fetch(&m->stats.host_undefs, 1, __ATOMIC_RELAXED);
    return 1;
}
