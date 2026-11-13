#include "machine.h"

#include <mach/mach_time.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "mmu.h"
#include "ramfile.h"
#include "saveguard.h"
#include "sysreg.h"
#include "rosetta.h"

extern void hv_exit(void);
extern void hv_helper_entry(void);
extern void hv_helper_thunk(void);
extern void hv_chain_thunk(void);
extern void hv_ijc_thunk(void);
extern void hv_xjump_thunk(void);

static int init_rest(HvMachine *m);

/* The translation tables are mapped whole, page-aligned, so that their pages can be swapped for fresh zero ones
   (hv_machine_trim_tables) as calloc'd memory starts out: untouched pages cost nothing either way. */
static size_t table_bytes(size_t n, size_t size) {
    size_t page = (size_t)getpagesize();
    return n && size <= SIZE_MAX / n ? (n * size + page - 1) / page * page : 0;
}

static void *table_alloc(size_t n, size_t size) {
    size_t bytes = table_bytes(n, size);
    if (!bytes) return NULL;
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static void table_free(void *p, size_t n, size_t size) {
    if (p) munmap(p, table_bytes(n, size));
}

/* How much of [p, p + bytes) (whole pages) the process holds: in memory, or compressed or swapped out, all of
   which its footprint counts. */
static uint64_t resident(void *p, size_t bytes) {
    size_t page = (size_t)getpagesize();
    char vec[1024];
    uint64_t n = 0;
    for (size_t off = 0; off < bytes; off += sizeof vec * page) {
        size_t len = bytes - off < sizeof vec * page ? bytes - off : sizeof vec * page;
        if (mincore((char *)p + off, len, vec) != 0) return bytes; /* can't tell: all of it, as before */
        for (size_t i = 0; i < (len + page - 1) / page; i++) n += vec[i] & (MINCORE_INCORE | MINCORE_PAGED_OUT) ? page : 0;
    }
    return n;
}

/* Whole pages [p, p + bytes) swapped for fresh zero ones: MAP_FIXED over our own anonymous memory puts them in
   place and frees the old ones in one step under the kernel's map lock (on failure the old mapping stays as it
   was). What the process held of them, or 0 if it couldn't be done (*ok then 0). */
static uint64_t zero_pages(void *p, size_t bytes, int *ok) {
    uint64_t held = resident(p, bytes);
    void *q = mmap(p, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (ok) *ok = q == p;
    return q == p ? held : 0;
}

/* Zeros at [p, p + len) of anonymous memory, paying only for what the process holds there: whole pages in memory
   are cleared in place, kept for the code that is about to use them again (a CPU's reset, a run-out: filled again at
   once, fresh pages would only fault back in); compressed or swapped-out ones get fresh mappings, which cost
   nothing until touched (a memset would bring them back first); never-touched ones read zero already and are left
   alone, without a system call (a flush clears every CPU's return tables in its stop, mostly untouched pages).
   mincore only picks the cheaper way for each page: every byte reads zero after (where it can't tell, or the system
   refuses a mapping, a memset). Partial pages at the edges are memset. The rule, for a CPU's state and its caches
   alike: a reset or a clear zeroes what is held in place and never faults in what isn't; giving memory back is a
   trim's job. */
void hv_zero_kept(void *p, size_t len) {
    uintptr_t page = (uintptr_t)getpagesize(), s = (uintptr_t)p, e = s + len;
    uintptr_t in = (s + page - 1) & ~(page - 1), out = e & ~(page - 1);
    if (in >= out) { memset(p, 0, len); return; }
    memset(p, 0, in - s);
    memset((void *)out, 0, e - out);
    char vec[1024];
    for (uintptr_t c = in; c < out; c += sizeof vec * page) {
        uintptr_t ce = out - c < sizeof vec * page ? out : c + sizeof vec * page;
        size_t n = (ce - c) / page;
        if (mincore((void *)c, ce - c, vec) != 0) { memset((void *)c, 0, ce - c); continue; }
        for (size_t i = 0; i < n;) {
            /* 2 in memory, 1 compressed or swapped out, 0 never touched */
#define KIND(v) ((v) & MINCORE_INCORE ? 2 : (v) & MINCORE_PAGED_OUT ? 1 : 0)
            int kind = KIND(vec[i]);
            size_t j = i + 1;
            while (j < n && KIND(vec[j]) == kind) j++;
#undef KIND
            void *a = (void *)(c + i * page);
            size_t bytes = (j - i) * page;
            if (kind == 2 || (kind == 1 && mmap(a, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != a))
                memset(a, 0, bytes);
#ifdef HV_ZERO_KEPT_CHECK
            /* A debug build's check (TEST_CONFIG=-DHV_ZERO_KEPT_CHECK) that "never touched" is true, as it is of the
               anonymous memory (bss, mmap) this is used on: such pages read zero. Reading them brings them in, so a
               test of what stays out of memory fails in such a build. */
            if (kind == 0)
                for (const uint64_t *w = a, *we = (const uint64_t *)((char *)a + bytes); w < we; w++)
                    if (*w) {
                        fprintf(stderr, "hv_zero_kept: a page mincore calls untouched holds data at %p\n", (void *)w);
                        abort();
                    }
#endif
            i = j;
        }
    }
}

/* The first `used` entries' pages back to the system, the table reading zero there again: what that gave back. */
static uint64_t table_drop(void *p, size_t used, size_t size) {
    size_t bytes = table_bytes(used, size);
    if (!p || !bytes) return 0;
    return zero_pages(p, bytes, NULL);
}

/* The tables of `which` that may hold entries, zeroed in place (a run-out, a reset): their pages stay held
   (ijc_tables_held), for the fills that follow and for a trim to give back. The used bits are taken before the
   writing, and the held ones set after it. A give-back for a parked CPU that runs meanwhile takes a table's used bit
   only once the table reads zero: so this either finds the bit and zeroes the table itself (zeros on old or new
   pages alike, the new ones marked held after), or finds it gone because the table already reads zero. */
void hv_ijc_tables_clear(HvPerCpu *p, uint32_t which) {
    uint32_t t = __atomic_fetch_and(&p->ijc_tables_used, ~which, __ATOMIC_SEQ_CST) & which;
    for (int i = 0; i < HV_IJC_TABLES; i++)
        if (t >> i & 1) memset(p->ijc + (size_t)i * (HV_IJC_TABLE_BYTES / 8), 0, HV_IJC_TABLE_BYTES);
    __atomic_or_fetch(&p->ijc_tables_held, t, __ATOMIC_SEQ_CST);
}

void hv_request_full_flush(HvMachine *m) {
    __atomic_store_n(&m->flush_all, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&m->flush_pending, 1, __ATOMIC_SEQ_CST);
    hv_machine_kick_all(m);
}

void hv_machine_set_stop(HvMachine *m, uint64_t pc, uint64_t pc2, void (*hook)(HvCpu *cpu), uint32_t cpus) {
    /* Two setters at once could leave one's hook at the other's pc for good: one at a time. */
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    hv_stop_publish(m, pc, pc2, hook, cpus);
    pthread_mutex_unlock(&mu);
}

void hv_machine_set_break(HvMachine *m, uint64_t pc) {
    if (pc == ~0ull) return; /* no --break: the stop is not this one's (a hook may be there already: hooksha) */
    hv_machine_set_stop(m, pc, ~0ull, NULL, 1u << 0);
    hv_request_full_flush(m);
}

void hv_machine_request_trim(HvMachine *m, int now) {
    __atomic_store_n(&m->trim_at, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), __ATOMIC_RELAXED);
    __atomic_store_n(&m->trim_pending, 1, __ATOMIC_SEQ_CST);
    if (now) hv_request_full_flush(m);
}

uint64_t hv_machine_trim_tables(HvMachine *m) {
    uint64_t freed = 0;
    freed += table_drop(m->blocks, m->hw_blocks, sizeof(HvBlock));
    freed += table_drop(m->undef_map, m->hw_undef, sizeof *m->undef_map);
    size_t slots = m->hw_slots;
    freed += table_drop(m->slots, slots, sizeof *m->slots);
    freed += table_drop(m->slot_site, slots, sizeof *m->slot_site);
    freed += table_drop(m->slot_stub, slots, sizeof *m->slot_stub);
    freed += table_drop(m->slot_cond, slots, sizeof *m->slot_cond);
    freed += table_drop(m->slot_xa, slots, sizeof *m->slot_xa);
    freed += table_drop(m->slot_xpg, slots, sizeof *m->slot_xpg);
    freed += table_drop(m->slot_xprev, slots, sizeof *m->slot_xprev);
    freed += table_drop(m->slot_xnext, slots, sizeof *m->slot_xnext);
    m->hw_blocks = m->hw_slots = m->hw_undef = 0;
    /* A trim of its own, given back whole here. A part an earlier one still owes (a CPU's caches: note_give_back) is
       done by this too, so it is counted done, with nothing of its own. */
    if (!__atomic_load_n(&m->giveback_left, __ATOMIC_SEQ_CST)) __atomic_store_n(&m->trim_released, 0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&m->giveback_left, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < HV_MAX_CPUS; i++) {
        freed += hv_cpu_ijc_give_back(&m->percpu[i]);
        if (__atomic_exchange_n(&m->percpu[i].ijc_giveback, 0, __ATOMIC_SEQ_CST)) hv_machine_give_back_done(m, 0);
    }
    hv_machine_give_back_done(m, freed);
    return freed;
}

void hv_machine_give_back_done(HvMachine *m, uint64_t freed) {
    __atomic_add_fetch(&m->trim_released, freed, __ATOMIC_SEQ_CST);
    if (__atomic_sub_fetch(&m->giveback_left, 1, __ATOMIC_SEQ_CST) == 0) __atomic_add_fetch(&m->trims, 1, __ATOMIC_SEQ_CST);
}

/* One of a CPU's arrays in the machine, whole pages (16 KB aligned, a multiple of 16 KB long), back to the system if
   the process holds any of it: what it held. *used (the parts written since the last clear) goes to 0 once the array
   reads zero; refused, it stays, and the next clear empties those parts. */
_Static_assert(offsetof(HvPerCpu, ijc_pages) % 16384 == 0 && sizeof ((HvPerCpu *)0)->ijc_pages % 16384 == 0 &&
                   offsetof(HvPerCpu, ijc_keyed) % 16384 == 0 && sizeof ((HvPerCpu *)0)->ijc_keyed % 16384 == 0 &&
                   offsetof(HvPerCpu, ijc_hint) % 16384 == 0 && sizeof ((HvPerCpu *)0)->ijc_hint[0] % 16384 == 0 &&
                   offsetof(HvPerCpu, jc) % 16384 == 0 && sizeof ((HvPerCpu *)0)->jc % 16384 == 0 &&
                   sizeof(HvPerCpu) % 16384 == 0,
               "give_back_part swaps whole pages");
static uint64_t give_back_part(void *a, size_t bytes, uint64_t *used) {
    if (!resident(a, bytes)) return 0;
    int ok;
    uint64_t f = zero_pages(a, bytes, &ok);
    if (ok) __atomic_store_n(used, 0, __ATOMIC_RELAXED); /* a stop's clear may store it too (hv_parts_clear) */
    return f;
}

/* Indirect-branch caches: all zero is how they start (hv_cpu_init), every entry empty. Their salts and ids stay, so
   nothing that tells a live entry from a dead one changes; there are only no entries. The tables that may hold
   memory are the ones a CPU has filled since they were last given back (ijc_tables_held: a run-out or a reset
   zeroes them in place). Only its own thread fills them: done by another while it ran, a fill could land half on
   the old page and half on the new (a tag with no target). A parked CPU may clear tables meanwhile (a TLBI's
   run-out, a CPU_ON's reset): hv_ijc_tables_clear says how the two meet. A table's used bit goes once it reads
   zero, so no clear writes a given-back table in again (2ea4d2c found a reset after a trim doing that); not before,
   since a clear that ran meanwhile would pass it by and then start the ids over with its entries still there. So a
   table the system wouldn't replace (a failed mmap: most likely under the memory pressure that brings trims) keeps
   its used bit, and gets its held bit back: the next run-out's or reset's clear zeroes it, and the next trim tries
   again. The (user id, page) set goes back with the user tables (2 MB a CPU): it says which pages' user targets a
   TLBI must drop, so it may lose its entries only once every user table reads zero, none kept. Another CPU's TLBI
   may read it meanwhile (hv_tlbi_drop_remote_va): old entries or zeros, and with the tables zero already either
   answer is right. Fills, the only adds, are this CPU's own, and it isn't running. last_fill[0] goes with it, or
   the next fill in that page would skip adding it. The rest of what it holds for running indirect branches and
   address-space switches goes too, each part once what it describes is gone (below): the chunk set, jc and the
   filters, and the stashes. A stop may run meanwhile when this CPU is parked (hv_machine_give_back, off_give_back):
   its flush or eviction writes only zeros to these and takes used bits away, so zeros on old or new pages alike.
   That stop's trim gives back for every CPU (hv_machine_trim_tables), so two may run at once for a parked one. Only
   one takes the held bits, so if a table it holds is kept, the other, finding none, gives the (user id, page) set,
   the chunk set and the filters back all the same: the kept table's entries are dead by then (the stop's
   hv_ijc_reset gave new ids and a new kernel generation, and the CPU_ON reset zeroes the table), and the next run-out
   or reset empties it. Nor does the window's odd ijc_seq matter then, even when the two adds make it even: every
   other CPU is stopped. */
uint64_t hv_cpu_ijc_give_back(HvPerCpu *p) {
    uint64_t freed = 0;
    if (!p->ijc) return 0;
    /* Another CPU's TLBI drops this one's targets itself (hv_tlbi_drop_remote_va), reading entries in every used
       table: one reading a table just swapped brings its fresh pages in, with no held bit to say so, and no trim
       gives them back until the table fills again. So ijc_seq is odd while this runs, which such a drop finds and
       leaves to later (as for a fill), and one already inside (ijc_remote, each stored before the other is read)
       ends before anything is swapped. A drop never waits, so this waits only for drops that are under way. */
    __atomic_add_fetch(&p->ijc_seq, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&p->ijc_remote, __ATOMIC_SEQ_CST)) sched_yield();
    uint32_t held = __atomic_exchange_n(&p->ijc_tables_held, 0, __ATOMIC_SEQ_CST), kept = 0;
    for (int t = 0; t < HV_IJC_TABLES; t++)
        if (held >> t & 1) {
            int ok;
            freed += zero_pages(p->ijc + (size_t)t * (HV_IJC_TABLE_BYTES / 8), HV_IJC_TABLE_BYTES, &ok);
            if (ok) __atomic_and_fetch(&p->ijc_tables_used, ~(1u << t), __ATOMIC_SEQ_CST);
            else kept |= 1u << t;
        }
    if (kept) __atomic_or_fetch(&p->ijc_tables_held, kept, __ATOMIC_SEQ_CST);
    size_t ub = table_bytes(1u << HV_UCODE_BITS, sizeof *p->ucode);
    if (!(kept & 0x5555u) && p->ucode && resident(p->ucode, ub)) { /* user tables: the even ones */
        int ok;
        uint64_t f = zero_pages(p->ucode, ub, &ok);
        if (ok) {
            freed += f;
            p->last_fill[0] = 0;
            __atomic_store_n(&p->ucode_count, 0, __ATOMIC_RELAXED);
        }
    }
    /* Which 32 MB chunks each user id has targets in (2 MB): a superset of the user tables as the set is, so empty
       is exact once they all read zero. Read by a range TLBI (hv_ijc_drop_range), this CPU's own; fresh_cid zeroes an
       id's chunk as it hands the id out, so zeros here change nothing it relies on. */
    size_t cb = table_bytes(65536, sizeof *p->cid_chunks);
    if (!(kept & 0x5555u) && p->cid_chunks && resident(p->cid_chunks, cb)) {
        int ok;
        uint64_t f = zero_pages(p->cid_chunks, cb, &ok);
        if (ok) freed += f, p->last_fill[0] = 0;
    }
    /* jc and the filters (1.27 MB, in the machine: hv_ijc_filters_clear's), each only once what it describes is gone:
       jc always (a cache of blocks the hash still has: this CPU's lookups fill it again); the user filters once no
       user table was kept, the kernel hints once no kernel one was, ijc_keyed (both halves) once none was. A part
       the system wouldn't replace keeps its used bits, so the next clear still empties it. */
    freed += give_back_part(p->jc, sizeof p->jc, &p->jc_used);
    if (!(kept & 0x5555u)) {
        freed += give_back_part(p->ijc_pages, sizeof p->ijc_pages, &p->ijc_pages_used); /* [1] is never written */
        freed += give_back_part(p->ijc_hint[0], sizeof p->ijc_hint[0], &p->ijc_hint_used[0]);
        p->last_fill[0] = 0;
    }
    if (!(kept & 0xaaaau)) {
        freed += give_back_part(p->ijc_hint[1], sizeof p->ijc_hint[1], &p->ijc_hint_used[1]);
        p->last_fill[1] = 0;
    }
    if (!kept) {
        freed += give_back_part(p->ijc_keyed, sizeof p->ijc_keyed, &p->ijc_keyed_used);
        if (!p->ijc_keyed_used) p->ijc_keyed_count = 0;
    }
    /* The stashes: every one emptied, as a TLBI of every address space leaves them (the next switch walks the tables
       again), then their storage swapped. Inside the window too: another CPU's TLBI reads a stash's entries only
       there (hv_tlbi_drop_remote_va), and finds ttbr0 0 after it; its stash_drop reads only ttbr0. Saves and restores
       are this CPU's own, and it isn't running. */
    size_t sb = table_bytes(HV_STASHES * HV_STASH_CAP, sizeof(HvTlbEntry) + sizeof(uint16_t));
    if (p->stash_mem && resident(p->stash_mem, sb)) {
        for (int k = 0; k < HV_STASHES; k++) {
            __atomic_store_n(&p->stash[k].ttbr0, 0, __ATOMIC_SEQ_CST);
            __atomic_store_n(&p->stash[k].n, 0, __ATOMIC_RELAXED);
        }
        freed += zero_pages(p->stash_mem, sb, NULL); /* refused: the entries stay, in stashes no switch restores */
    }
    __atomic_add_fetch(&p->ijc_seq, 1, __ATOMIC_RELEASE); /* the set too inside the window: a drop reading it retries */
    return freed;
}

/* [p, p + len) reads zero after, its whole pages the system's again: what the process held of them. The partial
   pages at the edges, which hold other fields too, are memset; so is a range the system wouldn't map afresh. */
static uint64_t give_back_range(void *p, size_t len) {
    uintptr_t page = (uintptr_t)getpagesize(), s = (uintptr_t)p, e = s + len;
    uintptr_t in = (s + page - 1) & ~(page - 1), out = e & ~(page - 1);
    if (in >= out) {
        memset(p, 0, len);
        return 0;
    }
    memset(p, 0, in - s);
    memset((void *)out, 0, e - out);
    int ok;
    uint64_t freed = zero_pages((void *)in, out - in, &ok);
    if (!ok) memset((void *)in, 0, out - in);
    return freed;
}

/* A CPU that stayed off (wait_while_off): it runs again only through CPU_ON, whose reset zeroes all of this (and
   its first run makes the TLB all invalid), so what it held for running goes back to the system, as zero pages the
   reset then leaves untouched. Its architectural state, which a snapshot saves, stays.
   Other CPUs reach it meanwhile only through their TLBIs and write revokes, and through a stop, which can't be on
   (this runs active, as unpark). tlb_ready goes first, so from then on they leave the TLB alone; one that read it
   ready just before may still store a 0x80 entry, on the old page or a fresh one, which the reset zeroes anyway.
   The branch caches (with their filters, jc, the chunk set and the stashes: hv_cpu_ijc_give_back) go before the
   ASID tables: with every table back (its used bit gone) a TLBI's drop finds nothing to do whatever user id it
   reads. A table the system wouldn't replace is kept (hv_cpu_ijc_give_back): the reset zeroes it in place, as for
   a trim. */
uint64_t hv_cpu_off_give_back(HvCpu *cpu) {
    __atomic_store_n(&cpu->tlb_ready, NULL, __ATOMIC_SEQ_CST);
    uint64_t freed = hv_cpu_ijc_give_back(cpu->pc_state);
    freed += give_back_range(cpu->tlb, sizeof cpu->tlb);
    freed += give_back_range(cpu->ret_tab, sizeof cpu->ret_tab);
    /* the fill logs, the ASID log and tables, with their counts: zero is how a reset leaves them */
    _Static_assert(offsetof(HvCpu, user_fills) == offsetof(HvCpu, timer_due) + sizeof cpu->timer_due &&
                   offsetof(HvCpu, next_cid) == offsetof(HvCpu, asid_base) + sizeof cpu->asid_base, "HvCpu layout");
    size_t from = offsetof(HvCpu, user_fills), to = offsetof(HvCpu, next_cid);
    freed += give_back_range((char *)cpu + from, to - from);
    return freed;
}

/* Table k of a zone's share (the blocks, the undefined-instruction map, the slots' tables): the whole pages inside
   its entries, [*lo, *hi), which go back to the system (zero_pages), reading zero again. 0: no table k. */
static int zone_table(const HvMachine *m, int zone, uint32_t k, uintptr_t *lo, uintptr_t *hi) {
    size_t b0 = (size_t)zone * m->zone_blocks, s0 = (size_t)zone * m->zone_slots, u0 = (size_t)zone * m->zone_undef;
    size_t s1 = s0 + m->zone_slots;
    struct { void *p; size_t from, to, size; } t[] = {
        {m->blocks, b0, b0 + m->zone_blocks, sizeof(HvBlock)}, {m->undef_map, u0, u0 + m->zone_undef, sizeof *m->undef_map},
        {m->slots, s0, s1, sizeof *m->slots}, {m->slot_site, s0, s1, sizeof *m->slot_site},
        {m->slot_stub, s0, s1, sizeof *m->slot_stub}, {m->slot_cond, s0, s1, sizeof *m->slot_cond},
        {m->slot_xa, s0, s1, sizeof *m->slot_xa}, {m->slot_xpg, s0, s1, sizeof *m->slot_xpg},
        {m->slot_xprev, s0, s1, sizeof *m->slot_xprev}, {m->slot_xnext, s0, s1, sizeof *m->slot_xnext},
    };
    if (k >= sizeof t / sizeof t[0]) return 0;
    uintptr_t page = (uintptr_t)getpagesize();
    *lo = ((uintptr_t)t[k].p + t[k].from * t[k].size + page - 1) / page * page;
    *hi = ((uintptr_t)t[k].p + t[k].to * t[k].size) / page * page;
    if (!t[k].p || *hi < *lo) *hi = *lo;
    return 1;
}

uint64_t hv_machine_drop_zone_tables(HvMachine *m, int zone, HvCpu *self) {
    uint64_t freed = 0;
    uintptr_t lo, hi;
    for (uint32_t k = 0; zone_table(m, zone, k, &lo, &hi); k++) {
        if (hi > lo) freed += zero_pages((void *)lo, hi - lo, NULL);
        if (self) hv_cpu_process_requests(self);
    }
    return freed;
}

int hv_machine_drop_zone_tables_part(HvMachine *m, int zone, uint32_t *k, size_t *at, uint64_t *freed, uint64_t until) {
    uintptr_t lo, hi;
    while (zone_table(m, zone, *k, &lo, &hi)) {
        if (lo + *at >= hi) {
            ++*k, *at = 0;
            continue;
        }
        size_t n = hi - lo - *at < HV_DROP_PIECE ? hi - lo - *at : HV_DROP_PIECE;
        *freed += zero_pages((void *)(lo + *at), n, NULL);
        *at += n;
        if (mach_absolute_time() >= until) return !zone_table(m, zone, *k + (lo + *at >= hi), &lo, &hi);
    }
    return 1;
}

/* The stopping CPU, with the others running again: nothing it gives back is in use. The zones noted are retired, or
   (a full flush) empty and not the one being filled, and stay so until a stop, which waits for this CPU.
   A CPU that ran again gives its own branch caches back in unpark; one still parked (asleep in WFI, off, waiting for
   the big lock) may stay so for long, so this gives its back for it: ijc_dropping and its active are a pair, each
   stored before the other is read, so either this sees it active and leaves them to it, or its unpark sees
   ijc_dropping and waits before using them. Parked, it fills none (it only empties: zeros either way). */
uint64_t hv_machine_give_back(HvMachine *m, HvCpu *self) {
    if (!__atomic_exchange_n(&m->giveback_pending, 0, __ATOMIC_SEQ_CST)) return 0;
    uint64_t t0 = mach_absolute_time(), zfreed = 0, freed = 0;
    uint32_t zones = __atomic_exchange_n(&m->giveback_zones, 0, __ATOMIC_SEQ_CST);
    for (int z = 0; z < m->nzones; z++)
        if (zones >> z & 1) zfreed += hv_machine_drop_zone_tables(m, z, self);
    for (int i = 0; i < HV_MAX_CPUS; i++) {
        HvPerCpu *p = &m->percpu[i];
        if (!__atomic_load_n(&p->ijc_giveback, __ATOMIC_SEQ_CST)) continue;
        if (!self || p == self->pc_state) { /* its own, or none running */
            if (__atomic_exchange_n(&p->ijc_giveback, 0, __ATOMIC_SEQ_CST)) {
                uint64_t f = hv_cpu_ijc_give_back(p);
                freed += f;
                hv_machine_give_back_done(m, f);
            }
            continue;
        }
        __atomic_store_n(&p->ijc_dropping, 1, __ATOMIC_SEQ_CST);
        if (!__atomic_load_n(&p->active, __ATOMIC_SEQ_CST) && __atomic_exchange_n(&p->ijc_giveback, 0, __ATOMIC_SEQ_CST)) {
            uint64_t f = hv_cpu_ijc_give_back(p);
            freed += f;
            hv_machine_give_back_done(m, f);
        }
        __atomic_store_n(&p->ijc_dropping, 0, __ATOMIC_SEQ_CST);
        if (self) hv_cpu_process_requests(self); /* another may be waiting on a TLBI of ours: as between zones' tables */
    }
    HV_STAT_ADD(m, giveback_ticks, mach_absolute_time() - t0);
    hv_machine_give_back_done(m, zfreed); /* its own part, the zones' tables: counted last of its parts */
    return zfreed + freed;
}

int hv_knob[HV_K_COUNT];
static const char *const hv_knob_names[HV_K_COUNT] = {"HV_NO_RAS", "HV_SUPERBLOCKS", "HV_GROUPS", "HV_WFI_SPIN", "HV_FLUSH", "HV_K1", "HV_K2", "HV_K3", "HV_TLBT", "HV_JIT_CAP", "HV_RECHECK", "HV_SYNC_WAIT", "HV_SWITCH_FLUSH", "HV_RANGE_DROP"};

static void knobs_init(void) {
    for (int i = 0; i < HV_K_COUNT; i++)
        hv_knob[i] = getenv(hv_knob_names[i]) ? atoi(getenv(hv_knob_names[i])) : 0;
    if (getenv("HV_NO_RAS") && !*getenv("HV_NO_RAS")) hv_knob[HV_K_NO_RAS] = 1;
    if (!getenv("HV_SYNC_WAIT")) hv_knob[HV_K_SYNCWAIT] = 1;
    if (!getenv("HV_RANGE_DROP")) hv_knob[HV_K_RANGEDROP] = 1024;
    /* HV_SWITCH_FLUSH stays off unless asked (it was on by default from e540907): on the iPad (2026-11-09) its own
       checks found entries macOS had removed, a kernel page among them, still held when macOS asked for the flush
       it leaves out (tests/test_switch_flush.c). */
}

int hv_knob_set(HvMachine *m, const char *name, int value) {
    for (int i = 0; i < HV_K_COUNT; i++)
        if (!strcmp(name, hv_knob_names[i])) {
            hv_knob[i] = value;
            hv_request_full_flush(m);
            return 1;
        }
    return 0;
}

/* The machine (26.8 MB: the block hash, 16 MB, and 1.3 MB of caches and filters for each of HV_MAX_CPUS CPUs) starts
   zero. A memset made all of it resident at once: the caches of CPUs the guest never starts, and the hash before a
   block is made. In place only what is held (a machine set up again in the same process); the app's machine is
   zero-fill memory, untouched until used. */
static void machine_zero(HvMachine *m) {
    hv_zero_kept(m, sizeof *m);
}

int hv_machine_init(HvMachine *m, size_t jit_bytes) {
    knobs_init(); /* as hv_machine_init_jit: the Mac tool and the tests read them from the environment too */
    machine_zero(m);
    if (hv_jit_region_open(&m->jit, jit_bytes) != 0) return -1;
    return init_rest(m);
}

int hv_machine_init_jit(HvMachine *m, void *rx, void *rw, size_t jit_bytes) {
    knobs_init();
    machine_zero(m);
    if (hv_jit_region_attach(&m->jit, rx, rw, jit_bytes) != 0) return -1;
    return init_rest(m);
}

static int init_rest(HvMachine *m) {
    hv_kva_init(); /* HV_KVA_HIST: here, so the TLB fill calls nothing (core/mmu.c) */
#ifdef HV_TEST_AS_SHIPPED /* make test-shipped: every test's machine logs as the app's does and counts every second */
    m->log = 1, m->stats_interval = 1;
#endif
    /* The TLB layout (cpu.h): HV_TLBT from the environment here too, so the Mac tool and the tests can pick it. */
    hv_knob[HV_K_TLBT] = getenv("HV_TLBT") ? atoi(getenv("HV_TLBT")) : 1; /* table-major unless asked: 9-20% faster in 5 of 6 paired rounds of the mix */
    /* No TLB of the old layout lives on with one machine live at a time (the app has the one, host/vm.c m): a
       CPU joins m by a reset, which keeps nothing of a CPU not m's yet (hv_cpu_reset_index). Two live at once, with the
       layout changed between their set-ups (HV_TLBT, or a knob switch on the first: this reads the environment again),
       would leave the first one's ready TLBs in the other layout; so would the other writer, flush_blocks on the
       HV_TLBT knob, which clears only its own machine's CPUs' TLBs. */
    hv_tlb_tmajor = hv_knob[HV_K_TLBT] != 0;
    m->rosetta = getenv("HV_ROSETTA") ? atoi(getenv("HV_ROSETTA")) : 0;
    if (m->rosetta && m->rosetta != 2) m->rosetta = 1;
    if (m->rosetta && !hv_rosetta_host_supported()) {
        fprintf(stderr, "hengevm: Rosetta mode requires a host with FEAT_AFP\n");
        hv_jit_region_close(&m->jit);
        return -1;
    }
#if HENGE_ROSETTA_4K_CACHE
    if (m->rosetta) fprintf(stderr, "hengevm: Rosetta 4 KB software translation cache enabled\n");
#endif
    const char *tso = getenv("HV_SOFTWARE_TSO");
    /* Not set: TSO for the threads that ask for it, in Rosetta mode. 1 or 2: for all code. 0: off, also in Rosetta
       mode, to measure what it costs: macOS still sees AIDR.TSO and turns EnTSO on, but nothing is fenced, so x86
       apps may go wrong. */
    m->tso_asked = !tso || !*tso ? -1 : atoi(tso) == 0 ? 0 : atoi(tso) == 2 ? 2 : 1;
    m->software_tso = m->tso_asked >= 0 ? m->tso_asked : m->rosetta ? 1 : 0;
    m->tso_every = m->tso_asked > 0;
    /* As many blocks as the region holds (a block is at least ~160 bytes of
       code), so the region filling, not this table, is what flushes; two
       chainable exits each at most. Untouched entries cost nothing. */
    size_t bytes = m->jit.capacity * sizeof(uint32_t);
    m->max_blocks = (uint32_t)(bytes / 160 < 4096 ? 4096 : bytes / 160);
    m->max_slots = m->max_blocks * 2;
    m->blocks = table_alloc(m->max_blocks, sizeof(HvBlock));
    if (!m->blocks) return -1;
    /* 4 bytes per copied FP/SIMD word; untouched entries cost nothing. Past that,
       hv_jit_sigill falls back to matching the guest's bytes. */
    m->max_undef = m->max_blocks * 8;
    m->undef_map = table_alloc(m->max_undef, sizeof *m->undef_map);
    if (!m->undef_map) return -1;
    m->stop_pc = ~0ull;
    m->stop_pc2 = ~0ull;
    m->trim_quiet_ns = 120ull * 1000000000; /* two minutes with no memory warning: retired zones may come back */
    m->chain = 1;
    m->slots = table_alloc(m->max_slots, sizeof *m->slots);
    m->slot_site = table_alloc(m->max_slots, sizeof *m->slot_site);
    m->slot_stub = table_alloc(m->max_slots, sizeof *m->slot_stub);
    m->slot_cond = table_alloc(m->max_slots, sizeof *m->slot_cond);
    m->slot_xa = table_alloc(m->max_slots, sizeof *m->slot_xa);
    m->slot_xpg = table_alloc(m->max_slots, sizeof *m->slot_xpg);
    m->slot_xprev = table_alloc(m->max_slots, sizeof *m->slot_xprev);
    m->slot_xnext = table_alloc(m->max_slots, sizeof *m->slot_xnext);
    m->ghost = table_alloc(HV_GHOST_ENTRIES, sizeof *m->ghost); /* untouched pages cost nothing */
    if (!m->slots || !m->slot_site || !m->slot_stub || !m->slot_cond || !m->slot_xa || !m->slot_xpg || !m->slot_xprev ||
        !m->slot_xnext || !m->ghost)
        return -1;
    /* Zones of the code cache: one per HV_JIT_ZONE_MIN of it, at most HV_JIT_ZONES (the published results on code
       cache eviction, Hazelwood and Smith 2004, have about eight units doing nearly as well as anything finer).
       HV_JIT_ZONES=n: that many (1: flush it all when full, as before). */
    const char *zones = getenv("HV_JIT_ZONES");
    size_t nz = zones && atoi(zones) > 0 ? (size_t)atoi(zones) : bytes / HV_JIT_ZONE_MIN;
    m->zones_asked = (int)(nz < 1 ? 1 : nz > HV_JIT_ZONES ? HV_JIT_ZONES : nz);
    /* The reclaimer (dispatch.c): no job, no zone ready. HV_EVICT_SYNC=1: every zone emptied in a stop, as before;
       HV_EVICT_EARLY=n: the next zone marked for it once the one being filled is n% full (75). */
    m->rc_zone = m->zone_ready = -1;
    m->reclaim_off = getenv("HV_EVICT_SYNC") && atoi(getenv("HV_EVICT_SYNC")) > 0;
    m->reclaim_early_pct = getenv("HV_EVICT_EARLY") ? atoi(getenv("HV_EVICT_EARLY")) : 75;
    pthread_mutex_init(&m->rc_mu, NULL);
    pthread_cond_init(&m->rc_cv, NULL);
    hv_jit_zones_reset(m);
    pthread_mutex_init(&m->big_mu, NULL);
    pthread_cond_init(&m->big_cv, NULL);
    pthread_mutexattr_t ra;
    pthread_mutexattr_init(&ra);
    pthread_mutexattr_settype(&ra, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->dev_mu, &ra);
    pthread_mutex_init(&m->tr_mu, NULL);
    pthread_mutex_init(&m->stw_mu, NULL);
    pthread_cond_init(&m->stw_cv, NULL);
    pthread_mutex_init(&m->pause_mu, NULL);
    pthread_cond_init(&m->pause_cv, NULL);
    for (int i = 0; i < HV_MAX_CPUS; i++) { /* untouched pages cost nothing, so every CPU gets its tables now */
        HvPerCpu *p = &m->percpu[i];
        p->ijc = table_alloc(HV_IJC_TABLES * HV_IJC_TABLE_BYTES / 8, 8);
        p->ucode = table_alloc(1u << HV_UCODE_BITS, 8);
        p->cid_chunks = table_alloc(65536, sizeof *p->cid_chunks);
        if (!p->ijc || !p->ucode || !p->cid_chunks) return -1;
        p->stash_mem = table_alloc(HV_STASHES * HV_STASH_CAP, sizeof(HvTlbEntry) + sizeof(uint16_t));
        if (!p->stash_mem) return -1;
        for (int k = 0; k < HV_STASHES; k++) {
            p->stash[k].ent = (HvTlbEntry *)p->stash_mem + (size_t)k * HV_STASH_CAP;
            p->stash[k].slot = (uint16_t *)((HvTlbEntry *)p->stash_mem + HV_STASHES * HV_STASH_CAP) + (size_t)k * HV_STASH_CAP;
        }
        pthread_mutex_init(&p->wake_mu, NULL);
        pthread_cond_init(&p->wake_cv, NULL);
        pthread_mutex_init(&p->req_mu, NULL);
        p->exited = 1; /* not in hv_run yet: nothing a pause has to wait for */
        p->stop_pass = ~0ull;
    }
    return 0;
}

__thread HvCpu *hv_self;
static void ack_wake(HvPerCpu *p);

void hv_dev_lock(HvMachine *m) {
    if (pthread_mutex_trylock(&m->dev_mu) == 0) return;
    HvCpu *cpu = hv_self;
    HvPerCpu *p = cpu ? cpu->pc_state : NULL;
    uint64_t t0 = mach_absolute_time(); /* always: the freeze records show who waits for the device lock */
    if (p) {
        __atomic_store_n(&p->tlb_live, 0, __ATOMIC_SEQ_CST); /* others need not wait for this one meanwhile */
        ack_wake(p);
    }
    pthread_mutex_lock(&m->dev_mu);
    if (p) {
        __atomic_store_n(&p->tlb_live, 1, __ATOMIC_SEQ_CST);
        hv_cpu_process_requests(cpu); /* anything that came in while we were not live */
    }
    HV_STAT_ADD(m, dev_waits, 1);
    HV_STAT_ADD(m, dev_wait_ticks, mach_absolute_time() - t0);
}

void hv_dev_unlock(HvMachine *m) {
    pthread_mutex_unlock(&m->dev_mu);
}

void hv_stats_sum(const HvMachine *m, HvStats *out) {
    enum { N = sizeof(HvStats) / sizeof(uint64_t) };
    const uint64_t *src = (const uint64_t *)&m->stats;
    uint64_t *dst = (uint64_t *)out;
    for (size_t k = 0; k < N; k++) dst[k] = __atomic_load_n(&src[k], __ATOMIC_RELAXED);
    for (int i = 0; i < HV_MAX_CPUS; i++) {
        src = (const uint64_t *)&m->percpu[i].stats;
        for (size_t k = 0; k < N; k++) dst[k] += __atomic_load_n(&src[k], __ATOMIC_RELAXED);
    }
}

void hv_stats_keys(const HvMachine *m, size_t table, HvStatKey out[8]) {
    memset(out, 0, 8 * sizeof *out);
    for (int i = 0; i < HV_MAX_CPUS; i++) {
        const HvStatKey *t = (const HvStatKey *)((const char *)&m->percpu[i] + table);
        for (int j = 0; j < 8; j++) {
            uint64_t key = __atomic_load_n(&t[j].key, __ATOMIC_RELAXED), n = __atomic_load_n(&t[j].n, __ATOMIC_RELAXED);
            if (!n) continue;
            for (int k = 0; k < 8; k++) {
                if (out[k].n && out[k].key == key) { out[k].n += n; break; }
                if (!out[k].n) { out[k].key = key, out[k].n = n; break; }
            }
        }
    }
}

static uint64_t post_locked_done(HvCpu *target, HvPerCpu *p) {
    uint64_t seq = __atomic_add_fetch(&p->req_seq, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&p->req_mu);
    __atomic_store_n(&target->exit_req, 1, __ATOMIC_SEQ_CST);
    __asm__ volatile("sev" ::: "memory"); /* a WFE waiting in place (wfe() in dispatch.c) sees it now */
    if (target->m->stats_interval) HV_STAT_ADD(target->m, kick_src[0], 1);
    return seq;
}

uint64_t hv_cpu_post_tlbi(HvCpu *target, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint64_t xt) {
    HvPerCpu *p = target->pc_state;
    pthread_mutex_lock(&p->req_mu);
    if (p->nreq < sizeof p->req / sizeof p->req[0]) {
        p->req[p->nreq].op1 = op1, p->req[p->nreq].crn = crn, p->req[p->nreq].crm = crm, p->req[p->nreq].op2 = op2;
        p->req[p->nreq].xt = xt;
        p->nreq++;
    } else {
        p->req_flags |= HV_REQ_TLB_ALL; /* too many queued: drop everything instead */
    }
    return post_locked_done(target, p);
}

uint64_t hv_cpu_post(HvCpu *target, uint32_t flags) {
    HvPerCpu *p = target->pc_state;
    pthread_mutex_lock(&p->req_mu);
    p->req_flags |= flags;
    return post_locked_done(target, p);
}

/* HV_SYNC_WAIT: a CPU waiting for another's acknowledgement sleeps (after a short spin) instead of
   spinning and yielding, so a host core is free for the CPU it waits on: the phone has two fast cores
   for three guest CPUs, the display and the app. The kernel's compare-and-wait on a 32-bit word. */
extern int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout_us);
extern int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);
#define HV_UL_COMPARE_AND_WAIT 1u
#define HV_ULF_WAKE_ALL 0x100u
#define HV_ULF_NO_ERRNO 0x1000000u

/* Wake whoever sleeps on p's ack_seq (it moved, or p stopped using its TLB). */
static void ack_wake(HvPerCpu *p) {
    if (__atomic_load_n(&p->ack_waiters, __ATOMIC_SEQ_CST))
        __ulock_wake(HV_UL_COMPARE_AND_WAIT | HV_ULF_WAKE_ALL | HV_ULF_NO_ERRNO, &p->ack_seq, 0);
}

void hv_cpu_ack_wake(HvCpu *cpu) {
    if (cpu->pc_state) ack_wake(cpu->pc_state);
}

void hv_cpu_process_requests(HvCpu *cpu) {
    HvPerCpu *p = cpu->pc_state;
    uint64_t seq = __atomic_load_n(&p->req_seq, __ATOMIC_ACQUIRE);
    if (seq == __atomic_load_n(&p->ack_seq, __ATOMIC_RELAXED)) return;
    static __thread struct { uint32_t op1, crn, crm, op2; uint64_t xt; } q[64];
    pthread_mutex_lock(&p->req_mu);
    seq = p->req_seq;
    uint32_t flags = p->req_flags, n = p->nreq;
    memcpy(q, p->req, n * sizeof q[0]);
    p->req_flags = 0;
    p->nreq = 0;
    pthread_mutex_unlock(&p->req_mu);
    /* A TLB not ready: the CPU hasn't run since its reset emptied its TLB, branch caches and stashes, and it is
       reset again before it runs if its caches were given back while off. Nothing there for a TLBI to drop, and the
       TLB is made all invalid before the CPU runs (hv_tlb_ready): acknowledged, its TLB left untouched. */
    if (!hv_tlb_is_ready(cpu)) flags = 0, n = 0;
    if (flags & HV_REQ_TLB_ALL) {
        HV_CPU_COUNT(cpu, flush_why[7], 1);
        hv_tlb_flush(cpu);
    } else {
        for (uint32_t i = 0; i < n; i++) hv_tlbi_apply(cpu, q[i].op1, q[i].crn, q[i].crm, q[i].op2, q[i].xt);
    }
    __atomic_store_n(&p->ack_seq, seq, __ATOMIC_RELEASE);
    ack_wake(p);
}

void hv_cpu_sync(HvCpu *self, HvCpu *target, uint64_t seq) {
    HvPerCpu *t = target->pc_state, *s = self ? self->pc_state : NULL;
    HvMachine *mm = target->m;
    uint64_t t0 = mach_absolute_time(); /* always: the freeze records count TLB syncs and their wait */
    if (s) {
        __atomic_store_n(&s->tlb_live, 0, __ATOMIC_SEQ_CST); /* others need not wait for this one meanwhile */
        ack_wake(s);
    }
    int sleep = hv_knob[HV_K_SYNCWAIT];
    for (uint32_t spins = 0;; spins++) {
        uint64_t ack = __atomic_load_n(&t->ack_seq, __ATOMIC_ACQUIRE);
        if (ack >= seq) break;
        if (!__atomic_load_n(&t->tlb_live, __ATOMIC_SEQ_CST)) break; /* parked: it does the work before it runs again */
        if (self) hv_cpu_process_requests(self);
        if (sleep && spins >= 32) {
            __atomic_add_fetch(&t->ack_waiters, 1, __ATOMIC_SEQ_CST);
            /* Asleep until ack_seq's low word moves or 100 us pass (a missed wake costs no more than that). */
            if (__atomic_load_n(&t->ack_seq, __ATOMIC_SEQ_CST) == ack && __atomic_load_n(&t->tlb_live, __ATOMIC_SEQ_CST))
                __ulock_wait(HV_UL_COMPARE_AND_WAIT | HV_ULF_NO_ERRNO, &t->ack_seq, (uint32_t)ack, 100);
            __atomic_sub_fetch(&t->ack_waiters, 1, __ATOMIC_SEQ_CST);
        } else if (spins > 200) {
            sched_yield();
        } else {
            __asm__ volatile("yield" ::: "memory");
        }
    }
    if (s) {
        __atomic_store_n(&s->tlb_live, 1, __ATOMIC_SEQ_CST);
        hv_cpu_process_requests(self); /* anything that came in while we were not live */
    }
    HV_STAT_ADD(mm, syncs, 1);
    HV_STAT_ADD(mm, sync_ticks, mach_absolute_time() - t0);
}

void hv_big_lock(HvMachine *m) {
    if (!m->big_lock_on) return;
    pthread_mutex_lock(&m->big_mu);
    uint64_t me = m->big_next++;
    while (m->big_serving != me) pthread_cond_wait(&m->big_cv, &m->big_mu);
    pthread_mutex_unlock(&m->big_mu);
}

void hv_big_unlock(HvMachine *m) {
    if (!m->big_lock_on) return;
    pthread_mutex_lock(&m->big_mu);
    m->big_serving++;
    pthread_cond_broadcast(&m->big_cv);
    pthread_mutex_unlock(&m->big_mu);
}

int hv_big_contended(HvMachine *m) { /* another thread waits for the big lock */
    if (!m->big_lock_on) return 0;
    pthread_mutex_lock(&m->big_mu);
    int waiting = m->big_next - m->big_serving > 1;
    pthread_mutex_unlock(&m->big_mu);
    return waiting;
}

/* Make cpu leave its block at its next checked exit (and end an in-place WFE wait). */
void hv_cpu_kick(HvCpu *cpu) {
    __atomic_store_n(&cpu->exit_req, 1, __ATOMIC_SEQ_CST);
    __asm__ volatile("sev" ::: "memory");
}

/* Return tables: nothing to return to. A tag is never 0: salts are not, and no VA has their bits. Zeroed as a reset
   zeroes the CPU (hv_zero_kept): the pages it holds in place, for the calls that fill them again at once; the others
   left out of memory. A flush or a zone's eviction does this for every CPU, one that never ran or is off too, and a
   run-out on the CPU's own thread: none of them makes the 2 MB resident where the CPU hadn't. */
void hv_ret_clear(HvCpu *cpu) {
    hv_zero_kept(cpu->ret_tab, sizeof cpu->ret_tab);
    cpu->ret_salt[0] = cpu->ret_salt[1] = 1ull << 48;
}

void hv_cpu_wake(HvCpu *cpu) {
    __atomic_store_n(&cpu->exit_req, 1, __ATOMIC_SEQ_CST);
    __asm__ volatile("sev" ::: "memory");
    if (cpu->m->stats_interval) HV_STAT_ADD(cpu->m, kick_src[1], 1); /* cpu: the one woken, not this thread's */
    HvPerCpu *p = cpu->pc_state;
    pthread_mutex_lock(&p->wake_mu);
    p->wake_pending = 1;
    pthread_cond_signal(&p->wake_cv);
    pthread_mutex_unlock(&p->wake_mu);
}

void hv_machine_kick_all(HvMachine *m) {
    if (m->stats_interval) HV_STAT_ADD(m, kick_src[2], 1);
    for (int i = 0; i < m->ncpus; i++)
        if (m->cpus[i]) __atomic_store_n(&m->cpus[i]->exit_req, 1, __ATOMIC_SEQ_CST);
    __asm__ volatile("sev" ::: "memory");
}

void hv_machine_halt(HvMachine *m, int reason) {
    for (int i = 0; i < m->ncpus; i++) {
        HvCpu *c = m->cpus[i];
        if (!c) continue;
        /* Under wake_mu: PSCI CPU_ON resets a parked CPU (halted included) under it, so a
           CPU being started cannot lose the halt. */
        HvPerCpu *p = c->pc_state;
        pthread_mutex_lock(&p->wake_mu);
        int none = HV_HALT_NONE;
        __atomic_compare_exchange_n(&c->halted, &none, reason, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&p->wake_mu);
    }
    hv_machine_kick_all(m);
    for (int i = 0; i < m->ncpus; i++)
        if (m->cpus[i]) hv_cpu_wake(m->cpus[i]); /* WFI and CPU_OFF waits */
    hv_machine_pause_poke(m);                    /* the pause gate */
}

/* Device work is for CPU 0, which polls the devices. */
void hv_machine_wake(HvMachine *m) {
    if (m->stats_interval) HV_STAT_ADD(m, kick_src[3], 1);
    if (m->cpu) hv_cpu_wake(m->cpu);
}

void hv_machine_free(HvMachine *m) {
    hv_jit_reclaim_stop(m); /* its job given up, the thread joined: it uses everything below */
    hv_jit_region_close(&m->jit);
    for (int i = 0; i < m->nram; i++) {
        free(m->ram[i].code_pages);
        free(m->ram[i].page_gen);
        free(m->ram[i].chain_heads);
        free(m->ram[i].block_heads);
        free(m->ram[i].code_mask);
        free(m->ram[i].code_misses);
        free(m->ram[i].code_hash);
        free(m->ram[i].save_guard);
    }
    free(m->patch_at);
    free(m->patch_word);
    table_free(m->blocks, m->max_blocks, sizeof(HvBlock));
    table_free(m->undef_map, m->max_undef, sizeof *m->undef_map);
    for (int i = 0; i < HV_MAX_CPUS; i++) {
        table_free(m->percpu[i].ijc, HV_IJC_TABLES * HV_IJC_TABLE_BYTES / 8, 8);
        table_free(m->percpu[i].ucode, 1u << HV_UCODE_BITS, 8);
        table_free(m->percpu[i].cid_chunks, 65536, sizeof *m->percpu[i].cid_chunks);
        table_free(m->percpu[i].stash_mem, HV_STASHES * HV_STASH_CAP, sizeof(HvTlbEntry) + sizeof(uint16_t));
    }
    table_free(m->slots, m->max_slots, sizeof *m->slots);
    table_free(m->slot_site, m->max_slots, sizeof *m->slot_site);
    table_free(m->slot_stub, m->max_slots, sizeof *m->slot_stub);
    table_free(m->slot_cond, m->max_slots, sizeof *m->slot_cond);
    table_free(m->slot_xa, m->max_slots, sizeof *m->slot_xa);
    table_free(m->slot_xpg, m->max_slots, sizeof *m->slot_xpg);
    table_free(m->slot_xprev, m->max_slots, sizeof *m->slot_xprev);
    table_free(m->slot_xnext, m->max_slots, sizeof *m->slot_xnext);
    table_free(m->ghost, HV_GHOST_ENTRIES, sizeof *m->ghost);
    m->ghost = NULL;
}

void hv_machine_set_cpus(HvMachine *m, int n) {
    m->cpu_count = n < 1 ? 1 : n > HV_MAX_CPUS ? HV_MAX_CPUS : n;
}

int hv_machine_add_ram(HvMachine *m, uint64_t pa, uint64_t size, void *host, int readonly) {
    if (m->nram == HV_MAX_RAM) return -1;
    /* A page of it must fit a page key (HV_PAGE_KEY_BITS: 4 TB of 16 KB pages) */
    if ((size + (1ull << HV_RAM_PAGE_BITS) - 1) >> HV_RAM_PAGE_BITS > 1ull << HV_PAGE_KEY_BITS) return -1;
    HvRam *r = &m->ram[m->nram++];
    r->pa = pa;
    r->size = size;
    r->host = host;
    r->readonly = readonly;
    size_t pages = (size + (1u << HV_RAM_PAGE_BITS) - 1) >> HV_RAM_PAGE_BITS;
    r->pages_log2 = 0;
    while ((1ull << r->pages_log2) < pages) r->pages_log2++;
    /* A power of two, so translated code bounds-checks a page index with one shift (IC IVAU). */
    r->code_pages = calloc((size_t)1 << r->pages_log2, 1);
    if (!m->main_ram || size > m->main_ram->size) m->main_ram = r;
    r->page_gen = calloc(pages, sizeof *r->page_gen);
    r->whint_cpus = m->cpu_count ? (uint32_t)m->cpu_count : HV_MAX_CPUS;
    r->whint = calloc(pages * r->whint_cpus, sizeof *r->whint);
    r->chain_heads = calloc(pages, sizeof *r->chain_heads);
    r->block_heads = calloc(pages, sizeof *r->block_heads);
    r->code_mask = calloc(pages, sizeof *r->code_mask);
    r->code_misses = calloc(pages, 1);
    r->code_hash = calloc(pages, sizeof *r->code_hash);
    r->save_guard = calloc(pages + 8, 1); /* + 8: hv_guard_disarm reads it 8 bytes at a time */
    return r->code_pages && r->page_gen && r->whint && r->chain_heads && r->block_heads && r->code_mask && r->code_misses &&
                   r->code_hash && r->save_guard
               ? 0
               : -1;
}

int hv_machine_add_mmio(HvMachine *m, uint64_t pa, uint64_t size, void *opaque,
                        uint64_t (*read)(void *, uint64_t, unsigned),
                        void (*write)(void *, uint64_t, uint64_t, unsigned), const char *name) {
    if (m->nmmio == HV_MAX_MMIO) return -1;
    m->mmio[m->nmmio++] = (HvMmio){pa, size, opaque, read, write, name, 0};
    return 0;
}

HvRam *hv_ram_lookup(HvMachine *m, uint64_t pa) {
    if (__builtin_expect(m->main_ram && pa - m->main_ram->pa < m->main_ram->size, 1))
        return m->main_ram;
    for (int i = 0; i < m->nram; i++) {
        HvRam *r = &m->ram[i];
        if (pa - r->pa < r->size) return r;
    }
    return NULL;
}

uint32_t *hv_smc_counts;
uint64_t *hv_smc_last;
uint64_t hv_smc_pages, hv_smc_cpu, hv_smc_dma;

/* Batches of at most HV_PATCH_MAX (48 KB held for good): each change is right on its own (a gate, or an exit back to
   its stub), so writing a full batch early only costs one more barrier. */
#define HV_PATCH_MAX 4096
void hv_patch_add(HvMachine *m, uint32_t *at, uint32_t word) {
    if (m->npatch == HV_PATCH_MAX) hv_patch_flush(m);
    if (m->npatch == m->patch_cap) {
        size_t cap = m->patch_cap ? m->patch_cap * 2 : 1024;
        uint32_t **a = realloc(m->patch_at, cap * sizeof *a);
        if (!a) abort(); /* a few KB, gone only when everything else is */
        m->patch_at = a;
        uint32_t *w = realloc(m->patch_word, cap * sizeof *w);
        if (!w) abort();
        m->patch_word = w;
        m->patch_cap = cap;
    }
    m->patch_at[m->npatch] = at;
    m->patch_word[m->npatch++] = word;
}

void hv_patch_flush(HvMachine *m) {
    hv_jit_patch(&m->jit, m->patch_at, m->patch_word, m->npatch);
    m->npatch = 0;
}

void hv_block_gate(HvMachine *m, HvBlock *b) {
    hv_patch_add(m, b->code, 0x14000000u | (b->gate & 0x3ffffff)); /* b gate */
}

/* Under tr_mu, the page's generation moved: each of its blocks made before goes out of the hash and off the page's
   list, its entry to its gate. Whatever still leads to one (a branch cache entry, a return table, an exit chained in
   the same page) leaves for the dispatcher there, which finds the code made again; nothing else has to forget it,
   and no CPU has to stop. A CPU in the middle of one finishes it, as a CPU may run old instructions until it next
   synchronises its context; when it does (an ISB, an exception, an exception return) it goes through the
   dispatcher or an isb in its block, which sees the gate. The record stays as it is until its zone is emptied with
   every CPU stopped, so a lookup walking the hash past it carries on. */
static void retire_page(HvMachine *m, HvRam *r, uint64_t pg) {
    uint32_t gen = __atomic_load_n(&r->page_gen[pg], __ATOMIC_ACQUIRE);
    for (uint32_t i = r->block_heads[pg]; i;) {
        HvBlock *b = &m->blocks[i - 1];
        i = b->page_next;
        if (b->gen == gen) continue; /* made since: current */
        hv_page_list_remove(m, b);
        hv_hash_remove(m, b); /* its hash_next as it was: a lookup standing on it goes on (hv_block_find) */
        hv_ghost_note(m, b, 0);
        hv_block_gate(m, b);
        b->dead = 1;
        m->zones[b->zone].ndead++;
        m->zones[b->zone].dead_words += b->words;
        HV_STAT_ADD(m, retired, 1);
        HV_STAT_ADD(m, retired_words, b->words);
        HV_STAT_ADD(m, entries_written[hv_entries_bucket(__atomic_load_n(&b->entries, __ATOMIC_RELAXED))], 1); /* CPUs bump it lock-free */
    }
}

/* The page's code changed: its blocks are out of date. code_pages goes from PROTECTED (1) to RETIRING while they are
   retired, so another write to the page that sees the mark waits until they are (it must not go on, and let a
   CPU that saw it jump into one, before it is gated); then to 0, writable again.
   The mark leaves PROTECTED before the generation moves: a CPU that marks the page again in between reads the new
   bytes but keeps the old generation, so its block is only retranslated once more. The other order could leave a
   valid block on a page nothing marks, whose writes would go unnoticed. A mark in progress (HV_CODE_MARKED_BY,
   dispatch.c) stays: its protector may still be waiting for a CPU with a store in flight, and a second protector
   that marked the page afresh would find that CPU's hint already taken and not wait for it. That protector then
   finishes and reads the generation after this bump (its bytes are new) or before it (its block is stale once the
   bump lands, and refused or retired). */
static void invalidate_page(HvMachine *m, HvRam *r, uint64_t pg) {
    uint8_t *mark = &r->code_pages[pg];
    uint8_t v;
    for (;;) {
        v = __atomic_load_n(mark, __ATOMIC_ACQUIRE);
        if (v == 0) return;
        if (v == HV_CODE_RETIRING) { /* another write is retiring them: done once it is */
            /* No limit (going on before they are gated would let a CPU run the old code after seeing this write), but
               a line after a second, so a freeze can be matched with the stall log. Nothing that retires waits on
               another CPU (see above), so this has never been seen. */
            uint64_t t0 = 0;
            for (uint32_t spins = 0; __atomic_load_n(mark, __ATOMIC_ACQUIRE) == HV_CODE_RETIRING; spins++) {
                if (spins <= 100) continue;
                sched_yield();
                if (!t0) t0 = mach_absolute_time();
                else if (t0 != 1 && mach_absolute_time() - t0 > 24000000) { /* 1 s of 24 MHz ticks */
                    fprintf(stderr, "hengevm: a write to code page %llx has waited 1 s for another write to retire its blocks (cpu %d)\n",
                            (unsigned long long)(r->pa + (pg << HV_RAM_PAGE_BITS)), hv_self ? (int)hv_self->index : -1);
                    t0 = 1;
                }
            }
            return;
        }
        if (v != 1) break; /* being marked: leave the mark */
        if (__atomic_compare_exchange_n(mark, &v, HV_CODE_RETIRING, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) break;
    }
    /* Did this write change the page from what its code was made from (HvStats.rewrite_*)? Asked of the write that
       drops the page's code, the first to reach it: a rewrite with the same bytes counts as the same. Only of a
       protected page whose bytes were known then; a page being marked loses its protector's hash, which may be of
       bytes from before this write. */
    uint32_t was = __atomic_exchange_n(&r->code_hash[pg], 0, __ATOMIC_RELAXED);
    if (v == 1 && was) {
        if (hv_page_hash(r, pg) == was) HV_STAT_ADD(m, rewrite_same, 1);
        else HV_STAT_ADD(m, rewrite_changed, 1);
    }
    /* What code came from goes with it: blocks made from now set it again. */
    __atomic_store_n(&r->code_mask[pg], 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&r->code_misses[pg], 0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&r->page_gen[pg], 1, __ATOMIC_SEQ_CST);
    HV_STAT_ADD(m, flush_smc, 1);
    if (hv_smc_counts && pg < hv_smc_pages) {
        hv_smc_counts[pg]++;
        uint64_t caller = hv_self ? hv_self->pc : 0;
        hv_smc_last[pg] = caller;
        if (hv_self) hv_smc_cpu++;
        else hv_smc_dma++;
    }
    /* Exits from other pages chained into the stale blocks go back to their stub (whose next chain finds the new
       translation), and the blocks are retired. Under the translation lock, as chaining and publishing are: one that
       saw the old generation is done by now, and one that comes after sees the new one. */
    uint64_t wait_start = m->stats_interval ? mach_absolute_time() : 0;
    pthread_mutex_lock(&m->tr_mu);
    uint64_t hold_start = m->stats_interval ? mach_absolute_time() : 0;
    if (m->stats_interval) HV_STAT_ADD(m, smc_lock_ticks, hold_start - wait_start);
    uint32_t link = r->chain_heads[pg];
    r->chain_heads[pg] = 0; /* the whole list at once: each exit on it comes off below */
    while (link) {
        uint32_t idx = link - 1;
        link = m->slot_xnext[idx];
        m->slot_xpg[idx] = m->slot_xprev[idx] = m->slot_xnext[idx] = 0;
        HV_STAT_ADD(m, smc_link_checks, 1);
        __atomic_store_n(&m->slots[idx], 0, __ATOMIC_RELEASE);
        uint32_t *site = m->slot_site[idx];
        if (site) hv_patch_add(m, site, 0x14000000u | (uint32_t)((m->slot_stub[idx] - site) & 0x3ffffff)); /* to its stub */
        m->nxchained--;
    }
    retire_page(m, r, pg);
    /* Then the code, all at once (an isb on this thread too: a CPU writing its own page goes on in its block, and
       a chained exit there may lead into one just gated). */
    hv_patch_flush(m);
    if (m->stats_interval) HV_STAT_ADD(m, smc_hold_ticks, mach_absolute_time() - hold_start);
    pthread_mutex_unlock(&m->tr_mu);
    if (v == 1) __atomic_store_n(mark, 0, __ATOMIC_SEQ_CST);
}

void hv_invalidate_code(HvMachine *m, uint64_t pa) {
    HvRam *r = hv_ram_lookup(m, pa);
    if (!r) return;
    uint64_t pg = (pa - r->pa) >> HV_RAM_PAGE_BITS;
    if (!__atomic_load_n(&r->code_pages[pg], __ATOMIC_RELAXED)) return;
    invalidate_page(m, r, pg);
}

/* Every write to guest RAM that translated code's fast path does not make (C code on a
   CPU's behalf, atomics done here, devices) calls this AFTER its bytes are in place.
   The order is what keeps translations current. The translator marks a page as code,
   fences, then reads it (lookup in dispatch.c); a writer writes, fences, then checks the
   mark. So either the writer sees the mark and invalidates, or the translator reads the
   new bytes. Checking first (as this used to be called) left a window: a CPU could mark
   the page and read the old bytes after the check, and its block stayed valid for good.
   exact: [pa, pa + len) is all that was written, so a write that misses every piece code was read from (code_mask)
   changes no code. The translator sets those bits, fences, then compares the bytes it translated with the page's
   (lookup): either this sees the bit, or that sees the new bytes and translates again. Devices may write more than
   they say (a ring's last field), so theirs drop the page whatever they say.
   A missed write still came here the slow way (the page is code, so no CPU's TLB lets it store straight in), and so
   does every later one to the page while it stays code: HV_CODE_MISS_LIMIT misses drop it (code_misses), and the
   page goes fast again until code from it is next translated. */
static void written(HvMachine *m, uint64_t pa, uint64_t len, int exact) {
    HvRam *r = hv_ram_lookup(m, pa);
    if (!r || !len || pa + len - r->pa > r->size) return;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t first = (pa - r->pa) >> HV_RAM_PAGE_BITS, last = (pa + len - 1 - r->pa) >> HV_RAM_PAGE_BITS;
    for (uint64_t pg = first; pg <= last; pg++) {
        uint8_t v = __atomic_load_n(&r->code_pages[pg], __ATOMIC_RELAXED);
        if (v) {
            int hit = 1;
            if (exact && v != HV_CODE_RETIRING) {
                uint64_t p0 = pg << HV_RAM_PAGE_BITS, lo = pa - r->pa < p0 ? p0 : pa - r->pa;
                uint64_t want = hv_code_pieces(lo - p0, pa + len - r->pa - lo);
                hit = (__atomic_load_n(&r->code_mask[pg], __ATOMIC_RELAXED) & want) != 0;
                if (!hit && m->stats_interval) HV_STAT_ADD(m, smc_missed, 1);
                if (!hit) __atomic_store_n(&r->code_hash[pg], 0, __ATOMIC_RELAXED); /* the page's bytes moved: not known */
                if (!hit && __atomic_add_fetch(&r->code_misses[pg], 1, __ATOMIC_RELAXED) == HV_CODE_MISS_LIMIT) {
                    hit = 1;
                    HV_STAT_ADD(m, smc_miss_drops, 1);
                }
            }
            if (hit) invalidate_page(m, r, pg);
        }
        /* Kept in files: changed, after the bytes (the writer clears the mark before it copies). A pointer lent for
           this write is done with: LENT goes back to plain dirty. */
        if (r->dirty) {
            uint8_t d = __atomic_load_n(&r->dirty[pg], __ATOMIC_RELAXED);
            while (d != HV_DIRTY && !__atomic_compare_exchange_n(&r->dirty[pg], &d, HV_DIRTY, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
            }
        }
    }
}

void hv_dma_written(HvMachine *m, uint64_t pa, uint64_t len) { written(m, pa, len, 0); }

void hv_phys_written(HvMachine *m, uint64_t pa, uint64_t len) { written(m, pa, len, 1); }

/* The file's revoke hook (core/ramfile.h): ctx is the machine, the region the one kept in f. */
static void revoke_for_file(void *ctx, HvRamFile *f, const uint32_t *pages, uint32_t n) {
    HvMachine *m = ctx;
    for (int i = 0; i < m->nram; i++)
        if (m->ram[i].file == f) {
            hv_ram_revoke_writes(m, &m->ram[i], pages, n);
            return;
        }
}

int hv_machine_attach_file(HvMachine *m, HvRam *r, HvRamFile *f) {
    if (hv_ramfile_page(f) != 1ull << HV_RAM_PAGE_BITS || hv_ramfile_size(f) != r->size || hv_ramfile_base(f) != r->host) {
        fprintf(stderr, "hengevm: the memory's files don't match its region: not kept in them\n");
        return -1;
    }
    r->dirty = hv_ramfile_dirty_map(f);
    r->file = f;
    hv_ramfile_set_revoke(f, revoke_for_file, m);
    return 0;
}

HvMmio *hv_mmio_lookup(HvMachine *m, uint64_t pa) {
    for (int i = 0; i < m->nmmio; i++) {
        HvMmio *d = &m->mmio[i];
        if (pa - d->pa < d->size) return d;
    }
    return NULL;
}

const void *hv_phys_ptr_r(HvMachine *m, uint64_t pa, uint64_t len) {
    HvRam *r = hv_ram_lookup(m, pa);
    if (!r || len > r->size - (pa - r->pa)) return NULL; /* not pa + len: a huge len would wrap */
    return r->host + (pa - r->pa);
}

void *hv_phys_ptr(HvMachine *m, uint64_t pa, uint64_t len) {
    HvRam *r = hv_ram_lookup(m, pa);
    if (!r || len > r->size - (pa - r->pa)) return NULL; /* not pa + len: a huge len would wrap */
    /* Nothing armed (always but during an auto-save): no page to look at */
    if (!r->readonly && __atomic_load_n(&m->guard, __ATOMIC_ACQUIRE)) hv_guard_write(m, r, r->save_guard, pa - r->pa, len, 0);
    /* Kept in files: the pages are dirty for as long as the device may write them, which only hv_dma_written ends
       (the writer copies a LENT page but leaves its mark). Before the pointer is handed out. A pointer only read
       takes hv_phys_ptr_r: a page lent for good is written again at every pass. */
    if (r->dirty && len) {
        uint64_t first = (pa - r->pa) >> HV_RAM_PAGE_BITS, last = (pa + len - 1 - r->pa) >> HV_RAM_PAGE_BITS;
        for (uint64_t pg = first; pg <= last; pg++)
            if (__atomic_load_n(&r->dirty[pg], __ATOMIC_RELAXED) != HV_DIRTY_LENT)
                __atomic_store_n(&r->dirty[pg], HV_DIRTY_LENT, __ATOMIC_SEQ_CST);
    }
    return r->host + (pa - r->pa);
}

void *hv_phys_ptr_w(HvMachine *m, uint64_t pa, uint64_t len) {
    return hv_phys_ptr(m, pa, len);
}

/* MMIO is accessed in naturally sized pieces of at most 8 bytes. */
int hv_phys_read(HvMachine *m, uint64_t pa, void *buf, unsigned len) {
    const void *p = hv_phys_ptr_r(m, pa, len);
    if (p) {
        memcpy(buf, p, len);
        return 0;
    }
    HvMmio *d = hv_mmio_lookup(m, pa);
    if (!d || len > 8) return -1;
    if (!d->self_locked) hv_dev_lock(m);
    uint64_t v = d->read ? d->read(d->opaque, pa - d->pa, len) : 0;
    if (!d->self_locked) hv_dev_unlock(m);
    memcpy(buf, &v, len);
    if (m->log >= 3)
        fprintf(stderr, "mmio read  %s+%llx size %u = %llx pc %llx\n", d->name, (unsigned long long)(pa - d->pa), len,
                (unsigned long long)v, (unsigned long long)m->access_pc);
    return 0;
}

int hv_phys_write(HvMachine *m, uint64_t pa, const void *buf, unsigned len) {
    HvRam *r = hv_ram_lookup(m, pa);
    if (r && pa + len - r->pa <= r->size) {
        if (r->readonly) return 0; /* ROM: writes are ignored */
        uint64_t off = pa - r->pa;
        if (m->watch_pa && m->watch_pa - pa < len) {
            uint64_t v = 0;
            memcpy(&v, (const uint8_t *)buf + (m->watch_pa - pa), len - (m->watch_pa - pa) < 8 ? len - (m->watch_pa - pa) : 8);
            fprintf(stderr, "hengevm: watch: write to pa %llx = %llx (%u bytes at pa %llx) by pc %llx\n",
                    (unsigned long long)m->watch_pa, (unsigned long long)v, len, (unsigned long long)pa,
                    (unsigned long long)m->access_pc);
        }
        hv_guard_write(m, r, r->save_guard, off, len, 1); /* an auto-save keeps the page as it was first */
        memcpy(r->host + off, buf, len);
        hv_phys_written(m, pa, len); /* after the bytes: see there */
        return 0;
    }
    HvMmio *d = hv_mmio_lookup(m, pa);
    if (!d || len > 8) return -1;
    uint64_t v = 0;
    memcpy(&v, buf, len);
    if (m->log >= 3)
        fprintf(stderr, "mmio write %s+%llx size %u = %llx pc %llx\n", d->name, (unsigned long long)(pa - d->pa), len,
                (unsigned long long)v, (unsigned long long)m->access_pc);
    if (!d->self_locked) hv_dev_lock(m);
    if (d->write) d->write(d->opaque, pa - d->pa, v, len);
    if (!d->self_locked) hv_dev_unlock(m);
    return 0;
}

void hv_cpu_reset(HvCpu *cpu, HvMachine *m) {
    hv_cpu_reset_index(cpu, m, 0);
}

int hv_machine_set_rosetta(HvMachine *m, int mode) {
    if (mode && mode != 2) mode = 1;
    if (mode && !hv_rosetta_host_supported()) return -1;
    if (mode != m->rosetta && (hv_blocks_held(m) || hv_jit_words_used(m))) return -2;
    m->rosetta = mode;
    m->software_tso = m->tso_asked >= 0 ? m->tso_asked : mode ? 1 : 0;
    m->tso_every = m->tso_asked > 0;
    for (int i = 0; i < m->ncpus; i++)
        if (m->cpus[i]) m->cpus[i]->rosetta_mode = (uint64_t)mode;
    return 0;
}

void hv_cpu_reset_index(HvCpu *cpu, HvMachine *m, uint32_t index) {
    /* Everything but m, mpidr, index, pc_state and tlb_ready: other CPUs' TLBIs and code-page protection
       (hv_tlb_protect_code_page, hv_ram_revoke_writes) read those of a CPU being started (PSCI CPU_ON) at any time.
       An index zeroed meanwhile had them take CPU 0's write hint for this CPU's. mpidr is the board's (set once, as
       the CPU is made), and another CPU's PSCI call finds this one by it (cpu_by_mpidr): zeroed, it found none. */
    _Static_assert(offsetof(HvCpu, m) < offsetof(HvCpu, mpidr), "HvCpu layout");
    _Static_assert(offsetof(HvCpu, mpidr) < offsetof(HvCpu, index), "HvCpu layout");
    _Static_assert(offsetof(HvCpu, index) < offsetof(HvCpu, pc_state), "HvCpu layout");
    _Static_assert(offsetof(HvCpu, pc_state) < offsetof(HvCpu, tlb_ready), "HvCpu layout");
    if (index >= HV_MAX_CPUS) { /* m->cpus and m->percpu: the hints below check it too, but only with RAM added */
        fprintf(stderr, "hengevm: CPU %u, past the %d a machine holds\n", index, HV_MAX_CPUS);
        abort();
    }
    for (int i = 0; i < m->nram; i++)
        if (index >= m->ram[i].whint_cpus) {
            fprintf(stderr, "hengevm: CPU %u of a machine made for %u\n", index, m->ram[i].whint_cpus);
            abort();
        }
    /* In place what the CPU holds, untouched what it never used (most of its 4.8 MB, used a little at a time:
       hv_zero_kept). A resume or a CPU_ON runs it again at once, so its used pages are kept, not swapped for fresh
       ones that fault straight back in; a trim isn't the place to give back a live CPU's state. The TLB (2 MB) is
       kept as it is: one the CPU made ready has every entry invalid but those its fill logs name, so only those are
       written (before the logs go); one never made ready (a CPU that never ran) is zeros, made all invalid when the
       CPU first runs (hv_tlb_ready), so a CPU that never runs never has it in memory. */
    _Static_assert(offsetof(HvCpu, tlb) < offsetof(HvCpu, m), "HvCpu layout");
    /* What it keeps is only its own in m: a CPU last reset into another machine, or never (zero: m NULL), or not m's
       CPU index any more (m->cpus: m set up again since, which zeroes it) keeps nothing, its TLB included. That one's
       fill logs may name slots of another TLB layout (hv_tlb_tmajor, which a machine's set-up sets for the process),
       or it held the TLB of a CPU that left the memory as it was. An HvCpu must be zero, or a CPU reset before, when
       it is first reset: other bytes can pass for m. */
    int fresh = cpu->m != m || m->cpus[index] != cpu, ready = !fresh && hv_tlb_is_ready(cpu);
    if (ready) hv_tlb_reset(cpu);
    const struct { size_t at, n; } keep[] = { /* in HvCpu's order (the asserts above) */
        {offsetof(HvCpu, tlb), sizeof cpu->tlb},           {offsetof(HvCpu, m), sizeof cpu->m},
        {offsetof(HvCpu, mpidr), sizeof cpu->mpidr},       {offsetof(HvCpu, index), sizeof cpu->index},
        {offsetof(HvCpu, pc_state), sizeof cpu->pc_state}, {offsetof(HvCpu, tlb_ready), sizeof cpu->tlb_ready},
    };
    size_t at = 0, nkeep = fresh ? 0 : sizeof keep / sizeof *keep;
    for (size_t k = 0; k < nkeep; at = keep[k].at + keep[k].n, k++)
        hv_zero_kept((char *)cpu + at, keep[k].at - at);
    hv_zero_kept((char *)cpu + at, sizeof *cpu - at);
    if (!ready && !fresh) hv_zero_kept(cpu->tlb, sizeof cpu->tlb);
    /* Ready stays ready throughout, its TLB just reset (a TLBI meanwhile only writes invalid entries); anything else
       reads not ready from here on, as after a zeroing. */
    if (!ready) __atomic_store_n(&cpu->tlb_ready, NULL, __ATOMIC_RELEASE);
    cpu->m = m;
    cpu->rosetta_mode = m->rosetta;
    __atomic_store_n(&cpu->index, index, __ATOMIC_RELAXED); /* read by other CPUs, as above */
    cpu->pc_state = &m->percpu[index];
    m->cpus[index] = cpu;
    if (m->ncpus <= (int)index) m->ncpus = (int)index + 1;
    if (index == 0) m->cpu = cpu;
    HvPerCpu *p = cpu->pc_state;
    /* Ids start over, so entries tagged with the old ones must go. */
    hv_ijc_tables_clear(p, ~0u); /* in place: a resume or a CPU_ON fills them again at once */
    hv_ijc_filters_clear(p);     /* as far as they were filled: a CPU that never ran (or ran nothing since) writes none */
    cpu->ijc = (uint64_t)(uintptr_t)p->ijc;
    cpu->lines_ptr = (uint64_t)(uintptr_t)&p->irq_line;
    for (int t = 0; t < HV_IJC_TABLES; t++) cpu->ijc_tab[t] = (uint64_t)(uintptr_t)(p->ijc + (size_t)t * (HV_IJC_TABLE_BYTES / 8));
    cpu->ret_salt[0] = cpu->ret_salt[1] = 1ull << 48; /* as hv_ret_clear, the tables being zero already */
    cpu->next_cid = 1;
    cpu->kernel_gen = 1;
    cpu->ijc_salt[1] = 1ull << 48; /* tags of an all-zero table never match */
    hv_ijc_switch(cpu);
    cpu->exit_routine = (uint64_t)hv_exit;
    cpu->helper_routine = (uint64_t)hv_helper_entry;
    cpu->helper_thunk = (uint64_t)hv_helper_thunk;
    cpu->chain_thunk = (uint64_t)hv_chain_thunk;
    cpu->ijc_thunk = (uint64_t)hv_ijc_thunk;
    cpu->xjump_thunk = (uint64_t)hv_xjump_thunk;
    cpu->el = 1;
    cpu->spsel = 1;
    cpu->daif = HV_PSTATE_DAIF;
    cpu->excl_addr = ~0ull;
    cpu->mpidr_el1 = cpu->mpidr ? cpu->mpidr : (0x80000000u | (uint32_t)index);
    /* Architectural reset value has the MMU and caches off; RES1 bits set. */
    cpu->sctlr_el1 = 0x30d00980; /* RES1 bits, and ITD and SED, which read as 1 without AArch32 at EL0 */
    hv_tlb_flush(cpu); /* the TLB as above: this writes none of it */
}
