#ifndef HENGEVM_MACHINE_H
#define HENGEVM_MACHINE_H

#include <pthread.h>
#include <sched.h> /* sched_yield: hv_stop_hook */
#include <stddef.h> /* offsetof */
#include <stdint.h>
#include <string.h> /* memcpy: hv_page_hash */

#include "cpu.h"
#include "jit_region.h"
#include "ramfile.h" /* HV_DIRTY_* */

/* Guest physical memory: RAM regions backed by host memory, and MMIO regions
   handled by device callbacks. The fast path only ever sees RAM. */
typedef struct HvRam {
    uint64_t pa, size;
    uint8_t *host;
    int readonly;
    uint8_t *code_pages; /* one byte per RAM page (HV_RAM_PAGE_BITS): a translated block came from here (2^pages_log2 of them) */
    uint32_t pages_log2;
    uint32_t *page_gen;  /* per page: bumped to invalidate the blocks translated from it */
    /* Per page and CPU: up to three TLB entries (slot << 2 | table, plus 1;
       16 bits each) that mapped the page writable, or HV_WHINT_MANY when there
       were more. When the page becomes code only those are dropped, not a whole
       TLB scanned. Page pg's hint for CPU i is whint[pg * whint_cpus + i]. */
    uint64_t *whint;
    uint32_t whint_cpus; /* hints a page: the machine's CPUs (hv_machine_set_cpus), HV_MAX_CPUS if not set */
    /* Exits from other pages chained into this page's blocks: slot index + 1 (0 ends the list), linked both ways
       through HvMachine.slot_xnext and slot_xprev, so that one comes off in O(1) (hv_xlist_remove). Under tr_mu. */
    uint32_t *chain_heads;
    /* Per page: its blocks not retired yet (index in HvMachine.blocks + 1, through HvBlock.page_next and page_prev,
       both ways, so that one comes off in O(1): hv_page_list_remove; 0 ends the list). When the page's code changes
       they are retired at once (hv_invalidate_code), not left to be found stale. Under tr_mu. */
    uint32_t *block_heads;
    /* Per page: one bit per 256-byte piece (HV_CODE_PIECE_BITS) that translated code was read from since the page's
       code last changed. A write that misses them all leaves the code as it is (hv_phys_written). */
    uint64_t *code_mask;
    /* Per page: exact writes since the page's code last changed that missed code_mask (a byte, so it may wrap: only a
       limit is read). Every such write took the slow path, as the page is still code; HV_CODE_MISS_LIMIT of them
       drop the page as any write to its code would, so data stores near code that no CPU runs again go fast. */
    uint8_t *code_misses;
    /* Per page: a hash of its bytes when it became code (protect_code_page), 0 when not known (a write that missed its
       code since: code_misses), for HvStats.rewrite_*: did the write that dropped its code change it? 4 bytes per
       page: 2 MB for 8 GB. */
    uint32_t *code_hash;
    /* The region is kept in files (core/ramfile.h): a save writes it ahead and records only what the files don't
       hold yet. NULL: plain memory, saved whole. */
    struct HvRamFile *file;
    /* ...and then one byte per page saying whether the files may not hold it (HV_DIRTY_*), the file's own map
       (hv_machine_attach_file); NULL otherwise. Whatever writes the memory marks the page: a TLB fill that makes an
       entry writable (mmu.c), hv_dma_written after C's writes, hv_phys_ptr as it hands out a pointer. The writer
       clears a mark, then takes back every writable entry for the page (hv_ram_revoke_writes), then copies it. */
    uint8_t *dirty;
    /* The save guard (core/saveguard.h): one byte per page, 0 unless an auto-save is keeping this page as it was. */
    uint8_t *save_guard;
} HvRam;


/* After the page changed, or after its entry became writable, with a full fence before (the order the writer's
   clear, fence and revoke pair with). Never takes LENT back (only hv_dma_written does). */
static inline void hv_ram_mark(HvRam *r, uint64_t pg) {
    uint8_t *d = r->dirty;
    if (!d) return;
    uint8_t v = HV_DIRTY_CLEAN;
    if (__atomic_load_n(&d[pg], __ATOMIC_RELAXED) == HV_DIRTY_CLEAN)
        __atomic_compare_exchange_n(&d[pg], &v, HV_DIRTY, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
}

typedef struct HvMmio {
    uint64_t pa, size;
    void *opaque;
    uint64_t (*read)(void *opaque, uint64_t off, unsigned size);
    void (*write)(void *opaque, uint64_t off, uint64_t val, unsigned size);
    const char *name;
    int self_locked;     /* the device takes its own lock (the GIC): no device lock around it */
} HvMmio;

typedef struct HvBlock {
    uint32_t jit_gen;    /* HvMachine.jit_gen when it was made: its zone's HvJitZone.born while it lives */
    uint32_t undef_at;   /* its entries in m->undef_map (see hv_jit_sigill) */
    uint64_t va, pa;
    uint32_t flags;      /* HV_BLOCK_* state the translation depends on */
    uint8_t ninsns;      /* at most HV_MAX_BLOCK_INSNS (translate.c) */
    /* Times the dispatcher entered it (lookup: its front cache or the hash, and once as it was made), up to 255; not
       counting runs through chained exits, branch caches or return tables, which leave no trace in C. Read into
       HvStats.entries_written or entries_evicted when it is thrown out. Plain relaxed bytes: a count lost to a race
       between CPUs is lost. */
    uint8_t entries;
    uint16_t words;      /* its code's size, both parts and the literals (HvJitZone.dead_words) */
    uint32_t gen;        /* page_gen of its page when translated */
    uint32_t nundef;     /* how many; HV_UNDEF_UNKNOWN: not recorded */
    struct HvRam *ram;   /* where its page lives, for a quick staleness check */
    uint32_t pg;         /* that page, in ram */
    uint32_t page_next;  /* the next block on the page's list (HvRam.block_heads): index + 1, 0 ends it */
    /* Its entry. Word 0 is a NOP, which retiring the block turns into a B to its gate: an exit with pc = va, so
       whatever still leads here (a branch cache, a return table, a chained exit) finds the code made again. */
    uint32_t *code;
    /* The next block in its hash bucket (HvMachine.hash): index + 1, 0 ends it. Read lock-free (hv_block_find), so a
       block taken out of the hash keeps it as it was: a lookup standing on it goes on along a valid rest of the chain. */
    uint32_t hash_next;
    /* The previous block on the page's list (HvRam.block_heads): index + 1, 0 when it is the head. A block on no list
       (retired, run once, dead at birth, an emptied zone's) has page_prev and page_next both 0. Under tr_mu. */
    uint32_t page_prev;
    uint32_t gate;       /* where the gate is: words after code */
    uint8_t maker;       /* the CPU that translated it (index + 1; 0: not a CPU, as in tests) */
    uint8_t zone;        /* the code cache zone its code and this record are in (HvJitZone) */
    uint8_t nosave;      /* its code holds this run's host addresses: not kept on storage (core/codestore.c) */
    uint8_t dead;        /* retired (retire_page, machine.c): out of the hash and off the page's list, its entry gated. The
                            record stays until its zone is emptied, with every CPU stopped. */
} HvBlock;
_Static_assert(sizeof(HvBlock) == 80, "a block record stays 80 bytes (HV_MAX_BLOCKS of them)");
_Static_assert(offsetof(HvBlock, hash_next) == 64 && offsetof(HvBlock, page_prev) == 68,
               "the hash link and the page list's back link take the 8 bytes the hash pointer had");

#define HV_BLOCK_NOP 0xd503201fu   /* word 0 of every block while it lives */
#define HV_CODE_MISS_LIMIT 64      /* HvRam.code_misses: writes that miss the code before the page is dropped anyway */
#define HV_CODE_PIECE_BITS 8       /* HvRam.code_mask: 256-byte pieces, 64 to a 16 KB page */
_Static_assert(HV_RAM_PAGE_BITS - HV_CODE_PIECE_BITS == 6, "a page's pieces fill a uint64_t");
/* The code_mask bits of the pieces that bytes [off, off + len) of a page touch (len > 0; the end clamped to the page). */
static inline uint64_t hv_code_pieces(uint64_t off, uint64_t len) {
    uint64_t end = off + len < (1ull << HV_RAM_PAGE_BITS) ? off + len : 1ull << HV_RAM_PAGE_BITS;
    uint64_t a = off >> HV_CODE_PIECE_BITS, z = (end - 1) >> HV_CODE_PIECE_BITS;
    return (z - a == 63 ? ~0ull : (2ull << (z - a)) - 1) << a;
}

#define HV_UNDEF_UNKNOWN 0xffffffffu

#define HV_BLOCK_EL1   1u        /* translated for EL1 (selects tlb[1]) */
#define HV_BLOCK_FPOFF 2u        /* CPACR_EL1.FPEN traps FP/SIMD at this EL */
#define HV_BLOCK_SPX   4u        /* EL1 with SPSel = 1 (SP_EL1 in use, SP_EL0 banked) */
#define HV_BLOCK_PAN   8u        /* EL1 with PSTATE.PAN set (selects TLB table 2) */
#define HV_BLOCK_SMALL 16u       /* from a guest page smaller than the software page (hv_code_page_bits): the block
                                    stays in it, and only branches in it chain unchecked */
#define HV_BLOCK_TSO   32u       /* EL0 with ACTLR_EL1.EnTSO, when software TSO follows the thread (hv_tso_thread):
                                    an Intel app's thread under Rosetta. Its loads and stores are fenced to x86 order. */

/* The branch caches' table for a block's flags (four bits, HV_IJC_TABLES). A TSO block's is its EL0 table with
   bit 2 set: HV_BLOCK_SPX's, which only EL1 blocks have. So an x86 thread's blocks and a native thread's never
   stand in for each other there. hv_xjump_thunk (host/enter.S) does the same. */
static inline uint32_t hv_ijc_table(uint32_t flags) {
    return (flags & 15u) | (flags & HV_BLOCK_TSO ? HV_BLOCK_SPX : 0);
}

#define HV_MAX_RAM 8
/* A RAM page as one u32 (HvMachine.slot_xpg): (region index << HV_PAGE_KEY_BITS | page) + 1, 0 meaning none. A region
   has at most 2^28 16 KB pages (4 TB; hv_machine_add_ram refuses more), so the largest key, of the last region's last
   page, is (HV_MAX_RAM - 1) << 28 | (2^28 - 1), and its + 1 must not wrap to 0: HV_MAX_RAM <= 15. */
#define HV_PAGE_KEY_BITS 28
_Static_assert((((uint64_t)(HV_MAX_RAM - 1) << HV_PAGE_KEY_BITS | ((1ull << HV_PAGE_KEY_BITS) - 1)) + 1) <= 0xffffffffull,
               "page key: the last region's last page, + 1, must fit a u32 and not be 0 (HV_MAX_RAM <= 15)");
#define HV_MAX_MMIO 48
#define HV_BLOCK_HASH (1 << 21) /* one bucket per block: chains are walked on every front-cache miss */
#define HV_JC_BITS 16
/* The used-part bit of element i of an array of 2^bits elements cut in 64 parts (HvPerCpu's *_used masks) */
#define HV_PART(i, bits) (1ull << ((i) >> ((bits) - 6)))
#define HV_MAX_BLOCKS (3 << 20) /* a macOS boot runs well over a million distinct blocks; a full flush retranslates them all */
#define HV_PROF_RING (1 << 16)
#define HV_MAX_SLOTS (6 << 20)  /* chainable exits: about two per block */

/* The code cache is cut into zones (at most this many, each at least HV_JIT_ZONE_MIN bytes of code):
   when it is full, one zone is emptied (the least used lately) and filled next, so the code still
   running elsewhere stays translated. Each zone has its own share of the code region, of the block
   records, of the chainable exits and of the FP/SIMD word records, at fixed places. */
#define HV_JIT_ZONES 8
#define HV_JIT_ZONE_MIN (64u << 20)
typedef struct HvJitZone {
    size_t used;         /* code words used from its start (the zone being filled: in HvMachine.jit.used) */
    uint32_t nblocks, nslots, nundef; /* records used (the zone being filled: HvMachine.nblocks and so on) */
    uint32_t born;       /* jit_gen when it was emptied last: every block living in it was made then */
    uint64_t score;      /* how much the CPUs used it lately (decayed at each switch) */
    uint64_t filled;     /* switches so far when it stopped being filled (0: never filled) */
    uint32_t ndead;      /* of its blocks, those retired (their page's code changed): emptying it loses only the rest */
    uint64_t dead_words; /* their code (HvBlock.words): used minus this is the code still held for something */
} HvJitZone;

/* Counters: where the time goes. Every CPU has its own (HvPerCpu.stats), written only by its own thread with a relaxed
   load and store (hv_count_own: the same ldr, add, str as a plain add), so counting costs a core-local add, no increment
   is lost, and other threads may read it at any time; the few counted on other threads (a device's DMA, the kicker) go
   to HvMachine.stats with relaxed atomic adds (HV_STAT_ADD). Readers add them all up (hv_stats_sum).
   Only uint64_t counters belong here: the sum adds the struct word by word. Two kinds:
   - always counted, cheap (one add, or a host-clock read on a path that already waits): what the dispatcher, the
     freeze recorder (host/stallmon.c) and the app's perf line read;
   - counted only with --stats / HV_STATS (HvMachine.stats_interval), where finding the number costs extra work
     (a second TLB probe or page walk, a table read, a host-clock read on a fast path). */
typedef struct HvStats {
    uint64_t blocks, translations, block_flushes;
    uint64_t flush_smc, flush_ic, flush_full;  /* why translations were dropped (flush_full: the cache was full, a zone emptied) */
    uint64_t zone_evictions, evicted_blocks, evict_unchained, evict_ticks; /* zones emptied, their blocks, links cut, host ticks */
    uint64_t stops, stop_ticks; /* every CPU stopped for the code cache (all of it dropped, or a zone emptied), and host
                                   ticks from asking them to stop until they were let go */
    uint64_t stop_max_ticks;    /* the longest such stop since the benchmark last took it (atomic: it zeroes it) */
    uint64_t stop_wait_ticks;   /* host ticks the other CPUs spent held at those stops, together (atomic) */
    uint64_t chained, ijc_fills, ijc_resets;
    uint64_t xchained;
    uint64_t kick_src[5];     /* exit_req set by: a request (TLBI), hv_cpu_wake, kick_all, hv_machine_wake, the CPU itself */        /* of them, exits to another page (TLB-checked) */
    uint64_t syncs, sync_ticks;        /* TLBI acks waited for, and host ticks spent waiting */
    uint64_t code_protect_waits;       /* pages becoming code that waited for a CPU mapping them writable */
    uint64_t dev_waits, dev_wait_ticks; /* device lock taken while another CPU held it */
    uint64_t tr_waits, tr_wait_ticks;   /* translation lock likewise */
    uint64_t words_inline, words_ool, words_pool; /* translated code size by part */
    uint64_t refills;         /* TLB misses filled by the light path (hv_tlb_refill) */
    uint64_t page_end_light;  /* plain accesses in a page's last bytes done by the light refill path, not the helper */
    uint64_t host_undefs;     /* copied instructions the host did not have: the guest got an undefined-instruction exception */
    uint64_t ijc_light;       /* branch-cache misses resolved without the dispatcher (hv_ijc_resolve) */
    uint64_t stash_saves, stash_hits, stash_entries; /* user TLB entries kept across process switches */
    uint64_t code_protects;   /* pages that became code (every CPU's TLB scanned for writable entries) */
    uint64_t tlb_miss_kind[4]; /* TLB misses: EL0 empty, EL0 other page there, EL1 empty, EL1 other page there */
    uint64_t stash_stale;     /* process switches that found the stash dropped by a TLBI meanwhile */
    uint64_t slow_why[6];     /* slow accesses by cause: MMIO, code-page write, page end, TLB miss, other, TLB entry there but mismatched */
    uint64_t small_pages;     /* TLB misses on guest mappings smaller than the software page size */
    uint64_t small_fills;     /* entries filled in the 4 KB table (a 4 KB-page process's own pages, mmu.h) */
    uint64_t small_join[4];   /* walks that fill the TLB from a smaller page (not lookups for stats or checks): joined into a software page, or not (a neighbour unmapped, other attributes, not one aligned run) */
    uint64_t slow_kind[12];   /* hv_mem_emulate calls by HV_MEM_* kind */
    uint64_t light_kind[4];
    uint64_t light_fail[4];
    uint64_t light_walks;
    uint64_t fast_resumes;
    uint64_t fr_fail[5];    /* SVCs and ERETs that went to their block without the dispatcher */
    uint64_t protect_kind[3]; /* a page becoming code, per CPU: not mapped writable, one entry, scan */
    uint64_t light_region[2]; /* user light fills: in the shared region (0x18..0x28 << 28), elsewhere */
    uint64_t chained_early;   /* exits chained as their block was placed */
    uint64_t wfe_waits;       /* WFEs that waited on the monitored word in place */
    uint64_t wfe_yields;      /* of them, the ones that gave the host core up after ~200 us on one word (dispatch.c wfe) */
    uint64_t kind_insns[16], kind_words[16]; /* --stats: inline words by instruction kind (HvInsnKind), as translated */
    uint64_t posted_kind[4];
    uint64_t race_kind[3];    /* remote drops that raced: walk under way, cache fill under way, either during the drop */  /* TLBIs other CPUs had to do themselves: a page or small range that raced, a big range, the rest (ASID, all) */
    uint64_t range_spared;    /* big user-range TLBIs that needed no cached targets dropped */
    uint64_t kernel_ranges;   /* big kernel-range TLBIs, dropped by range rather than with everything */
    uint64_t tlb_range_flushes; /* and that dropped only the TLB entries in the range */     /* of hv_ijc_resolve's lookups, ones that walked for the target page */   /* hv_ijc_resolve could not: chaining off/odd target, filters full, no TLB entry, block not made yet */   /* the same for hv_ijc_resolve's fills */
    uint64_t fill_kind[4];    /* ijc fills into an empty slot, over another live salt, over the same salt, over a dead salt */
    uint64_t flush_why[8];    /* whole-TLB flushes: TLBI all (local), TLBI all (shared), SCTLR, TTBR1, TCR, MAIR, range of everything, requests overflowed */
    uint64_t flush_after_switch[4];
    uint64_t flush_checked, flush_check_entries, flush_check_bad, flush_check_af; /* HV_SWITCH_FLUSH=2: flushes checked, entries walked, out of date (the flush was done), only the access flag */
    uint64_t flush_skipped;   /* TLBI all here right after a process switch, left out (HV_SWITCH_FLUSH=1 or 2) */ /* TLBI all here, by time since this CPU's last TTBR0 change: <2 us, <20 us, <200 us, later */
    uint64_t reset_why[8];    /* cache drops: full flush, TLBI by ASID, TLBI user VA, TLBI kernel VA, code change, user VA in every ASID (whole user half), keyed filter full, big user range */
    uint64_t why[6];          /* dispatcher entries by cause: chain fill, kick, exception/sysreg, interrupt, cached target, cache miss */
    uint64_t miss_kind[7];    /* cache misses by the branch that missed: RET, BR/BLR, B/BL, conditional, MSR, other, unknown */
    uint64_t translate_ticks, translated_insns, translated_words; /* time spent translating, guest and host code size */
    uint64_t retrans_evicted; /* translations of code an emptied zone held, its page's code unchanged since (HV_GHOST:
                                 it remembers about the last three zones of 650K blocks emptied, so code thrown out
                                 longer ago than that counts as neither this nor retrans_written) */
    uint64_t retrans_written; /* translations of code thrown out before whose page was written since: retired, or emptied
                                 and then written */
    uint64_t lost_races;      /* translations thrown away at publish: another CPU had placed the same block meanwhile */
    uint64_t helper[12];      /* by HV_OP_* */
    uint64_t tlb_flushes, fetch_walks, exceptions, interrupts;
    uint64_t wfi, wfi_ticks;  /* WFI exits, host ticks spent asleep */
    uint64_t smc_link_checks, smc_lock_ticks, smc_hold_ticks; /* unlinking chains into changed code */
    uint64_t retired;         /* blocks retired as their page's code changed (gated, out of the hash) */
    uint64_t retired_words;   /* their translated code, in words (per page dropped: retired / flush_smc) */
    uint64_t entries_written[4], entries_evicted[4]; /* blocks retired by a code write, and live blocks of emptied
                                                        zones, by the dispatcher's entries into them (HvBlock.entries):
                                                        1 (or 0), 2-8, 9-64, more (hv_entries_bucket) */
    uint64_t rewrite_same, rewrite_changed; /* pages whose code a write dropped: the page's bytes as they were when it
                                               became code, or not (HvRam.code_hash; neither when it was not known) */
    uint64_t smc_missed;      /* writes to a code page that missed every piece its code came from: nothing dropped */
    uint64_t smc_miss_drops;  /* pages dropped after HV_CODE_MISS_LIMIT such writes (HvRam.code_misses) */
    uint64_t tr_once;         /* translations run once and dropped: the guest's bytes changed while they were read */
    uint64_t evict_live, evict_dead; /* of the blocks in emptied zones: still in use, retired before */
    uint64_t zones_retired;          /* zones a trim took out of use (HvMachine.zones_retired) */
    uint64_t zones_returned;         /* ...and put back in use once warnings stopped (HvMachine.trim_quiet_ns) */
    uint64_t trim_stop_ticks;        /* a trim's share of the CPUs' stops: emptying the zone it retires */
    uint64_t giveback_ticks;         /* giving a trim's tables back, after the stop (hv_machine_give_back) */
    uint64_t off_givebacks, off_released; /* CPUs that stayed off gave back what they held: times, bytes (wait_while_off) */
    uint64_t evict_step_ticks[4]; /* emptying a zone, by step (evict_zone, evict_unhook): its blocks out of the hash and
                                     off their pages' lists, other zones' exits into it (cut, off their lists, the
                                     code patched), its own exits off their lists, the CPUs' caches */
    uint64_t evict_page_unlinks;  /* blocks emptying a zone took off a page's list (O(1) each: list nodes touched) */
    uint64_t evict_delists;       /* exits it took off a page's list of exits from other pages (O(1) each) */
    uint64_t evict_hash_hops;     /* hash links it walked past to find its blocks in their buckets */
    uint64_t evict_max_ticks;     /* the longest emptying since the stats line last took it (machine's; atomic max) */
    /* Emptying zones in the background (dispatch.c, the reclaimer), all the machine's: */
    uint64_t evict_sync;          /* zones holding code emptied in a stop all the same: no zone was ready (the
                                     cache filled faster than the reclaimer emptied one, or HV_EVICT_SYNC) */
    uint64_t reclaims;            /* zones the reclaimer emptied (ready to fill, or retired by a trim) */
    uint64_t reclaim_marks;       /* stops that only marked a zone for it (O(1)), the active zone 3/4 full */
    uint64_t reclaim_aborts;      /* reclaims given up: a full flush, a reset, a code-store load, or a stop that needed
                                     the zone at once and finished it itself */
    uint64_t reclaim_ticks[3];    /* host ticks it took, wall time: taking the zone off the hash and the exits into it;
                                     waiting for every CPU to pass the dispatcher (the grace period); the lists and
                                     the rest after */
    uint64_t reclaim_kicks;       /* its kicks of every CPU to its dispatcher in a grace period: one as it starts, and
                                     one each 1 ms after while a CPU has not answered (one stuck in a long helper) */
    uint64_t reclaim_gp_max_ticks;   /* the longest grace period since the stats line last took it (atomic max) */
    uint64_t reclaim_hold_max_ticks; /* the longest it held tr_mu at once, likewise: what a translator may wait */
    uint64_t quick_stop_max_ticks;   /* the longest a stop's own work took when it emptied nothing: filling a zone the
                                        reclaimer had readied, or marking one for it (atomic max, likewise) */
    uint64_t quick_stops, quick_stop_ticks; /* how many such stops, and their own work in all */
    uint64_t slow_mem;        /* hv_mem_emulate calls */
} HvStats;
_Static_assert(sizeof(HvStats) % sizeof(uint64_t) == 0, "HvStats: uint64_t counters only (hv_stats_sum)");

/* --stats tables keyed by an address: the first 8 keys seen and their counts (per CPU; hv_stats_keys merges them). */
typedef struct HvStatKey { uint64_t key, n; } HvStatKey;

/* Debugging rings, per CPU (HvPerCpu.trace): what this CPU did last, for crash dumps and hv_debug_el0_in_kernel. */
typedef struct HvTrace {
    uint64_t recent[16];      /* the last blocks the dispatcher entered (pc | el) */
    uint64_t nrecent;
    struct { uint64_t from, to, spsr; uint32_t kind, el; } elchg[16]; /* last exception entries (kind = type) and ERETs (kind 9) */
    uint32_t nelchg;
} HvTrace;

#define HV_MAX_CPUS 8
#define HV_WHINT_MANY (1ull << 63) /* whint: more than three; else up to three TLB codes + 1, 16 bits each. The one
                                       code with no 16-bit name (the 4 KB table's last slot) is always MANY. */

/* What each CPU has to itself and keeps across hv_cpu_reset (tests copy HvCpu
   structs around, so it lives here, by CPU index): its indirect-branch cache and
   the filters for dropping entries from it, the front cache of blocks, its
   interrupt lines and its wakeup. */
/* A process's user TLB entries, put aside when it is switched out (TTBR0 now
   names another address space) and put back when it comes back, as a TLB
   tagged by ASID keeps them. Any TLBI that could touch the address space
   (here or from another CPU) bumps gen; a stash whose gen moved is dropped. */
#define HV_STASHES 8
#define HV_STASH_CAP 1024
typedef struct HvTlbStash {
    uint64_t ttbr0;      /* the address space (0: empty); read by other CPUs */
    uint64_t tcr0;       /* TCR_EL1's TTBR0 controls (low 16 bits) the entries were walked under; set before ttbr0 */
    uint32_t gen;        /* bumped by TLBIs (atomic) */
    uint32_t saved_gen;  /* gen when the entries were saved */
    uint32_t n;
    uint64_t used;       /* for replacing the least recently saved */
    uint16_t *slot;      /* slot << 2 | table, as user_fills */
    HvTlbEntry *ent;
} HvTlbStash;

typedef struct HvPerCpu {
    /* Interrupt lines into this CPU, driven by the GIC. First and together:
       translated code reads both with one load (HvCpu.lines_ptr). */
    int irq_line, fiq_line;
    uint64_t *ijc;           /* indirect-branch cache tables (cpu->ijc points here) */
    /* The filters and jc start on a page (16 KB on arm64): a 64th of one never shares a page with another part or
       another field, so clearing the parts fills used (HV_PART) writes no page they didn't. */
    uint64_t ijc_pages[2][1024] __attribute__((aligned(16384))); /* hashed bitmap of the user pages any process cached targets in (for all-ASID TLBIs) */
    /* Hashed bitmap of (page, salt) pairs with cached targets: a TLBI for one
       address space drops its entries only if that address space cached
       something in the page. A new salt makes old bits meaningless, so this is
       only cleared (with the whole cache) when it gets an eighth full. */
    uint64_t ijc_keyed[1 << 14];
    uint32_t ijc_keyed_count;
    uint32_t ijc_tables_used; /* bit per flags value whose table may hold entries: filled since it was last cleared */
    /* Bit per table that may hold memory: set with ijc_tables_used, but kept when a run-out or a reset clears the
       table in place (the pages stay, and the next fills need no fault-ins); cleared only when a trim gives the table
       back (hv_cpu_ijc_give_back), which clears its ijc_tables_used bit too once the swap worked, so no clear writes
       it in again (a table the system wouldn't swap keeps both bits). */
    uint32_t ijc_tables_held;
    int ijc_giveback;         /* a trim asked for its tables back: the CPU does it itself, out of the stop (unpark) */
    int ijc_dropping;         /* the stopper is giving them back for it, parked (hv_machine_give_back): unpark waits */
    /* Exactly which (user id, user page) pairs have cached targets: a hash set,
       so unmapping a big range of one address space drops only its code pages'
       entries instead of the whole address space's. Emptied with the user half, and
       given back with the user tables by a trim (hv_cpu_ijc_give_back). */
    uint64_t *ucode;
    uint32_t ucode_count;
    /* Per user id: which 32 MB chunks of its VA (hashed into 256) have cached
       targets, so a TLBI for a big range elsewhere (data, nearly always) drops nothing. */
    uint64_t (*cid_chunks)[4];
    uint64_t last_fill[2];
    /* Per half, hashed by (page ^ salt): which 256-byte pieces of the page have
       cached targets, so dropping a page clears 64 entries per piece instead of
       all 4096. A slot two keys wanted is spoiled (key ~0: scan it all); only
       this CPU empties one again, after clearing its page. */
    struct { uint64_t key, bits; } ijc_hint[2][4096] __attribute__((aligned(16384)));   /* page ^ salt of the last fill in each half: its filter bits are set already */
    /* Which 64ths of the filters above may hold a bit (HV_PART: of ijc_keyed's words, of each half's hint slots,
       of ijc_pages[0]'s words; ijc_pages[1] is never written): set by a fill as it writes one, taken by the clear that
       empties it (hv_parts_clear), so a clear writes only what was filled since the last one, and never brings in a
       page no fill wrote. A CPU that never ran, or one with no fill since, writes none. Only this CPU writes them, or
       another with it parked (a flush's stop, a reset, a give-back). */
    uint64_t ijc_keyed_used, ijc_hint_used[2], ijc_pages_used;
    /* Direct-mapped cache in front of the hash, with the VA beside the block so
       a slot holding another block is passed over without reading it. */
    struct { uint64_t va; struct HvBlock *b; } jc[1 << HV_JC_BITS] __attribute__((aligned(16384)));
    /* Bit per 64th of jc (1024 entries, 16 KB) a lookup may have written since the part was last cleared: a flush, an
       eviction and a reset read and write only those parts (hv_jc_clear). Written as ijc_keyed_used is: a lookup's
       |= is a plain read-modify-write, on this CPU's own thread while it runs; every clear (a flush's or eviction's
       stop, a reset, a give-back) runs while it doesn't, so none meets a lookup's. Clears use atomics only because
       two of them can meet (a stop's flush and a parked CPU's own give-back: hv_parts_clear). */
    uint64_t jc_used;
    /* Lookups that found a block, by its zone, since the last zone switch (only this CPU writes them
       while it runs; the switch, with every CPU stopped, reads and clears them). */
    uint64_t zone_hits[HV_JIT_ZONES];
    /* WFI sleeps on this; device threads, the GIC and other CPUs signal it. */
    pthread_mutex_t wake_mu;
    pthread_cond_t wake_cv;
    int wake_pending;
    /* Work other CPUs ask of this one (only the owner may touch its TLB and
       branch cache while it runs). Posting bumps req_seq; the owner does the
       work at its next dispatcher entry and publishes ack_seq. */
    pthread_mutex_t req_mu;
    uint32_t req_flags;          /* HV_REQ_* */
    uint32_t nreq;
    struct { uint32_t op1, crn, crm, op2; uint64_t xt; } req[64]; /* TLBIs to apply */
    uint64_t req_seq, ack_seq;
    /* active: inside the dispatcher loop (0 while parked: WFI, powered off, at a
       flush barrier). tlb_live: active and not itself waiting for acks, i.e. it
       may be using TLB entries right now. */
    int active, tlb_live;
    /* The last HvMachine.reclaim_seq this CPU has answered (dispatch.c reclaim_ack): written by its own thread with a
       release store at the top of its dispatcher loop and as it unparks, read by the reclaimer. */
    uint64_t reclaim_ack;
    /* Zones marked for the reclaimer since this CPU last answered (bit per zone): its front cache's entries for them go
       at that answer. Not zones_dying: a CPU parked through a whole job answers only once the zone is filled again,
       no longer dying, and its entries for the old blocks would otherwise stay (their records keep va, pa and gen).
       Set in the stop that marks (atomic or), taken by the CPU's own thread (atomic exchange). */
    uint32_t reclaim_purge;
    uint32_t ack_waiters;        /* CPUs asleep in hv_cpu_sync waiting for ack_seq to move (HV_SYNC_WAIT) */
    uint32_t walk_seq;           /* odd while it walks page tables and fills its TLB (see hv_tlbi_drop_remote_va) */
    uint32_t ijc_seq;            /* odd while it checks a block's mapping in its TLB and caches it (hv_ijc_fill_checked),
                                    or while its branch caches are given back (hv_cpu_ijc_give_back) */
    uint32_t ijc_remote;         /* other CPUs' TLBIs inside their drop of this one's entries (hv_tlbi_drop_remote_va) */
    HvTlbStash stash[HV_STASHES];
    /* Every stash's entries, then every stash's slots, in one mapping (272 KB), so a trim, a memory warning or a CPU
       that stays off gives it back with the branch caches (hv_cpu_ijc_give_back) */
    void *stash_mem;
    uint64_t stash_clock;
    /* The in-place WFE wait (dispatch.c wfe): the word the last waits timed out on, the value they waited to see
       change, and how many in a row. */
    uint64_t wfe_spin_va, wfe_spin_val;
    uint32_t wfe_spin_rounds;
    uint32_t wfe_waiting; /* 1 while in it: the freeze recorder calls such a sample a guest spin (host/stallmon.c) */
    /* Pause (hv_machine_pause): at_gate while it waits at the dispatcher's pause gate, outside guest
       code; exited once hv_run has returned. Both under HvMachine.pause_mu. */
    int at_gate, exited;
    /* hv_cpu_pass_stop: the stop pc this CPU runs past if its next run starts there (~0: none). Used once, and dropped
       by a run that starts or goes elsewhere first. Its own thread only. */
    uint64_t stop_pass;
    /* This CPU's counters and rings: written by its own thread only, read (summed) by anyone. On cache lines of
       their own, so counting never moves a line another core reads or writes. Kept across hv_cpu_reset, like the
       rest of HvPerCpu: the totals cover the whole run. */
    HvStats stats __attribute__((aligned(128)));
    HvStatKey small_at[8];    /* --stats: where (VA >> 20, bit 0 = EL1) slow accesses to pages under the TLB page size were */
    HvStatKey flush_pc[8];    /* --stats: where TLBI all here comes from (guest PC) */
    HvTrace trace;
} HvPerCpu;

#define HV_REQ_TLB_ALL 1u   /* drop the whole TLB and branch cache */

typedef struct HvMachine {
    HvRam ram[HV_MAX_RAM];
    int nram;
    HvRam *main_ram;     /* the largest region: guest RAM */
    HvMmio mmio[HV_MAX_MMIO];
    int nmmio;

    HvJitRegion jit;
    /* Blocks by (va, pa, flags): each bucket's first block (index in blocks + 1), then HvBlock.hash_next; 0 ends it.
       64 parts of 128 KB, each starting on a page. */
    uint32_t hash[HV_BLOCK_HASH] __attribute__((aligned(16384)));
    /* which 64ths of hash an insert wrote since the last flush (HV_PART; under tr_mu or in a stop; read by the
       stats sample, hv_block_hash_sample) */
    uint64_t hash_used;
    HvBlock *blocks;
    /* nblocks, nundef, nslots and jit.used are where the zone being filled is up to in its tables (indexes
       into the whole table, not counts); block_limit, undef_limit, slot_limit and jit.limit are its ends. */
    int nblocks;
    uint32_t *undef_map, nundef, max_undef; /* HvBlock.undef_at/nundef: HV_UNDEF_ENTRY words (translate.h) */
    uint32_t max_blocks, max_slots; /* sized from the JIT region (machine.c) */
    uint32_t block_limit, undef_limit, slot_limit;
    /* Set by helpers; the dispatcher stops every CPU once the block has left and makes room: empties a
       zone, or with flush_all set (hv_request_full_flush) drops every translation. */
    int flush_pending;
    int flush_all;
    uint32_t room_gen;   /* jit_gen + 1 when a translation found the zone full under that generation (tr_mu) */
    int nzones, zone;    /* zones, and the one being filled */
    int zones_asked;     /* zones wanted: fewer while too little of the region is in use for them (HV_K_JITCAP) */
    uint32_t zone_blocks, zone_slots, zone_undef; /* each zone's share of the tables */
    size_t zone_words;   /* and of the code region */
    uint64_t zone_switches;
    HvJitZone zones[HV_JIT_ZONES];
    /* How far taking each zone's blocks and exits off the hash and the lists has got (evict_unhook, dispatch.c): by
       phase (HV_UNHOOK_*), the next block, then the next slot (index in the whole table). Zeroed as it finishes. */
    struct { uint32_t block, slot; } unhook_at[HV_JIT_ZONES][2];
    /* Each zone's slots in use at the reclaimer's mark (a stop: no translator was part way, so all were written). Its
       hash phase looks at the zone being filled's slots no further than its count here (evict_unhook's fill_slots): a
       slot taken since may hold what it held before its zone was last emptied until its translator writes it, and
       none is chained into a dying zone. A zone begun to be filled since (zone_load, in a stop) has its count there
       instead: emptied since the mark, perhaps, its old exits cut. The one it took over from had all its slots
       written by that stop. */
    uint32_t rc_slots_end[HV_JIT_ZONES];
    /* Emptying a zone in the background (dispatch.c: the protocol is written out at the reclaimer). Under tr_mu
       unless said otherwise.
       zones_dying: zones whose blocks no lookup may return or chain from (bit per zone; read relaxed by every CPU,
       changed only in a stop or with no CPU running): set as a zone is marked, cleared only as it is filled again
       (with jit_gen bumped), or by a full flush, a reset or a code-store load.
       rc_zone: the zone the reclaimer is emptying (-1: none), at rc_phase (RC_*), its job rc_job (bumped to give it
       up: atomic); rc_retire: a trim retires it once empty, its tables given back by the reclaimer (rc_owed: that
       part of the trim is counted in giveback_left). zone_ready: emptied, to fill next (-1: none). room_early:
       jit_gen + 1 when a translation found the zone being filled 3/4 full and asked a stop to mark the next one. */
    uint32_t zones_dying;
    int zone_ready, rc_zone, rc_phase, rc_retire, rc_owed;
    uint32_t rc_job, room_early;
    uint32_t room_early_seen; /* the room_early a stop has acted on: one mark per generation at most */
    size_t rc_poison_at;   /* HV_JIT_POISON: words of its code zeroed so far */
    uint64_t rc_wait_seq;  /* the reclaim_seq its grace period waits for the CPUs to answer (0: not asked yet) */
    uint64_t rc_t0;        /* host ticks when its phase began (reclaim_ticks) */
    uint32_t rc_drop_k;    /* RC_DROP: the table of the retired zone's share it is giving back, ... */
    size_t rc_drop_at;     /*          ... bytes into its pages, ... */
    uint64_t rc_drop_freed; /*         ... and the bytes given back so far that the process held */
    /* Where the zone being filled is 3/4 full (reclaim_early_pct; cursor values, as nblocks, nslots, jit.used): a
       translation published past one asks for the next zone to be marked (room_early). */
    uint32_t early_blocks, early_slots;
    size_t early_words;
    /* Bumped (seq_cst) by the reclaimer to ask every running CPU to pass its dispatcher (HvPerCpu.reclaim_ack). */
    uint64_t reclaim_seq;
    int reclaim_off;       /* HV_EVICT_SYNC=1 (tests: NOSPARE=1): every zone emptied in a stop, as before */
    int reclaim_manual;    /* tests: the reclaimer thread never runs; hv_jit_reclaim_step does its work */
    int reclaim_early_pct; /* how full the zone being filled is when the next is marked (75) */
    /* The reclaimer thread: started at the first job, joined by hv_machine_free. rc_mu and rc_cv carry the jobs
       to it and rc_quit; taken after tr_mu, never the other way. */
    pthread_t rc_thread;
    int rc_started, rc_quit;
    pthread_mutex_t rc_mu;
    pthread_cond_t rc_cv;

    void *gic;
    /* Devices: called between blocks every so often and while the CPU waits.
       Returns the host time (mach_absolute_time units) of the next timer event, or 0. */
    uint64_t (*tick)(struct HvMachine *m, HvCpu *cpu);
    void *tick_opaque;
    /* GIC CPU interface system registers (ICC_*_EL1), reg = op0:op1:CRn:CRm:op2. */
    uint64_t (*icc_read)(struct HvMachine *m, HvCpu *cpu, uint32_t reg);
    void (*icc_write)(struct HvMachine *m, HvCpu *cpu, uint32_t reg, uint64_t v);

    /* Where the dispatcher stops, and what it calls there. Set before the CPUs start with plain stores; changed while they
       run only through hv_machine_set_stop, and read on a CPU through hv_at_stop and hv_stop_hook (below). */
    uint64_t stop_pc;    /* tests: leave the dispatcher when pc reaches this */
    void (*stop_hook)(HvCpu *cpu); /* debugging: call this there instead of leaving */
    uint64_t stop_pc2;   /* debugging: a second such place */
    uint32_t stop_seq;   /* even, and odd while hv_stop_publish moves the stop: a CPU acts on a stop it read whole */
    uint32_t stop_cpus;  /* the CPUs the stop is for, bit i for CPU i (--break: CPU 0); 0: every CPU */
    uint32_t jit_gen;    /* full flushes and zone switches so far: what blocks are made under (HV_JIT_POISON checks,
                            each thread's code chunk, an exit waiting to be chained) */
    int gen_checks;
    int rewrite_hash;    /* HvStats.rewrite_*: hash each page as it becomes code and again when a write drops it (about
                            a microsecond each, thousands a second). Off unless HV_REWRITE_HASH is set or a benchmark
                            runs (host/bench.c); read and written with relaxed atomics */
    int trace;           /* print every block entered */
    int log;             /* 1: unknown sysregs, undefined instructions, unmapped accesses; 2: also every exception */
    int slow_mem;        /* tests: send every load and store through hv_mem_emulate */
    /* Fixed before translating: 0 off, 1 software TSO, 2 full-barrier reference.
       Guest ordering is implemented without changing privileged host modes. */
    int software_tso;
    /* 1: every block is fenced (HV_SOFTWARE_TSO=1 or 2). 0: only the blocks of threads that asked for x86 ordering
       (HV_BLOCK_TSO), Rosetta mode's default: native apps and the kernel run unfenced, as on a Mac. */
    int tso_every;
    int rosetta;         /* 0 off, 1 inline AP flags, 2 C reference; requires host AFP */
    int tso_asked;       /* HV_SOFTWARE_TSO's value, -1 when not set (Rosetta mode then turns TSO on per thread) */
    uint64_t access_pc;  /* guest PC of the load/store being emulated, for logs */
    uint64_t watch_va;   /* debugging: log every write that touches this guest VA (0: off) */
    uint64_t watch_pa;   /* debugging: the same by physical address */
    /* What threads other than CPU threads counted (HV_STAT_ADD); the CPUs' own are in percpu. Cache lines of its own:
       chain and slots below are read by every CPU on every chain. */
    HvStats stats __attribute__((aligned(128)));
    int chain __attribute__((aligned(128))); /* block chaining and the indirect-branch cache are enabled */
    uint64_t *slots;         /* chainable exits' targets (0: not chained) */
    uint32_t **slot_site;    /* the B in each exit's stub that chaining rewrites to jump to the target */
    uint32_t **slot_stub;    /* what that B goes to while it isn't chained */
    uint32_t **slot_cond;    /* a conditional branch in a block's hot part that chaining may send straight to the target too */
    /* Exits to another page (see put_branch_exit): slot_xa is the host addend the target page's TLB entry must hold
       for the exit to go straight to the chained block (0: a same-page exit). Once chained, the exit is on its
       target page's list (HvRam.chain_heads), so that invalidating that page's code visits only its own incoming
       chains: slot_xpg is the page's key (HV_PAGE_KEY_BITS; 0: on no list), slot_xprev and slot_xnext the slots
       before and after it (index + 1; 0: it is the head, the end). A slot is on at most one list: chain() links it
       only while slot_xpg is 0, and only invalidate_page (machine.c), emptying a zone (hv_xlist_remove, O(1)) or a
       full flush (every head cleared) unlinks it. A slot on no list has all three 0: translating zeroes them as it
       takes the slot (lock-free; it is on no list, and no list leads to it), so only slots in no zone's used range
       may hold old values, which nothing reads. Under tr_mu otherwise. */
    uint64_t *slot_xa;
    uint32_t *slot_xpg, *slot_xprev;
    uint32_t *slot_xnext, nxchained;
    /* What blocks emptied zones and code writes threw out, to tell what a translation makes again (HV_GHOST_*:
       HV_GHOST_ENTRIES of 16 bits, 4 MB; the machine does not start without them). Under tr_mu. */
    uint16_t *ghost;
    /* Words of translated code to rewrite, gathered under tr_mu and done together (hv_patch_flush). */
    uint32_t **patch_at, *patch_word;
    size_t npatch, patch_cap;
    uint32_t nslots;
    HvCpu *cpu;              /* CPU 0: the one that polls devices */
    HvCpu *cpus[HV_MAX_CPUS]; /* every CPU, so device code (and a full flush) can ask them to leave */
    int ncpus;
    int cpu_count;           /* the most CPUs it will have (hv_machine_set_cpus), 0 if not set: HV_MAX_CPUS */
    HvPerCpu percpu[HV_MAX_CPUS];
    /* With more than one CPU, only the holder of this runs guest code (a fair
       ticket lock, handed over at every kick and while a CPU sleeps). */
    int big_lock_on;
    /* Devices, the GIC and the ICC registers: one CPU at a time (recursive). */
    pthread_mutex_t dev_mu;
    /* Translating and inserting blocks, the JIT region and the slots. */
    pthread_mutex_t tr_mu;
    /* Stopping every CPU to flush all translations. */
    pthread_mutex_t stw_mu;
    pthread_cond_t stw_cv;
    int stw_active;
    /* Pausing the whole machine: while pause_req is set, every CPU stops at the top of its dispatcher
       loop (between guest instructions, its state all in HvCpu) and waits on pause_cv. */
    pthread_mutex_t pause_mu;
    pthread_cond_t pause_cv;
    int pause_req;
    pthread_mutex_t big_mu;
    pthread_cond_t big_cv;
    uint64_t big_next, big_serving;
    int stats_interval;  /* seconds between --stats lines, 0 = off */
    /* --profile: host PCs of the CPU thread sampled every millisecond, drained by the stats line. */
    uint64_t *prof;
    uint32_t prof_head, prof_tail;
    struct HvSaveGuard *guard; /* armed save guard (core/saveguard.h), or NULL */
    /* Giving memory back (hv_machine_request_trim): the tables a full flush leaves as written but unused
       (blocks, undef_map, the slot arrays and every CPU's indirect-branch cache) are swapped for fresh zero
       pages at the next full flush, or the emptied zone's share of them when the cache next makes room.
       The most of each used since the last trim says how far they were written. */
    int trim_pending;        /* taken (exchanged to 0) by the stop that acts on it */
    uint32_t trims;          /* trims done (atomic: read from other threads) */
    uint64_t trim_released;  /* bytes the last one gave back (of what the process held; atomic) */
    uint32_t hw_blocks, hw_slots, hw_undef;
    /* A trim in a stop only notes what goes back; the stopping CPU gives it back once the others run again
       (hv_machine_give_back), each CPU its own branch caches (HvPerCpu.ijc_giveback). Zones whose tables are to go. */
    int giveback_pending;
    uint32_t giveback_zones;
    /* Parts of the last trim not given back yet: the stopper's, and each CPU's caches. trim_released adds up as each
       is done, and trims counts the trim only when the last is (hv_machine_give_back_done; atomic). */
    int giveback_left;
    /* Zones a trim took out of use (bit per zone): not filled, so their tables stay given back. Down to
       zones_floor active ones. Once trim_quiet_ns pass with no warning (trim_at: CLOCK_UPTIME_RAW ns of the last),
       the cache making room takes one back instead of emptying a zone in use; hv_machine_set_zones starts over. */
    uint32_t zones_retired;
    uint64_t trim_at, trim_quiet_ns;
    uint32_t off_giveback_ms; /* a CPU off this long gives back what it holds (0: HV_OFF_GIVEBACK_MS; tests make it short) */
} HvMachine;
_Static_assert(HV_MAX_CPUS <= 32, "stop_cpus has a bit for each CPU");

/* The stop, read on a CPU while another thread may move it (hv_machine_set_stop). The check every block makes is two
   relaxed loads, the plain loads it always was. Only on a match does hv_stop_hook read the stop whole, as a seqlock
   reader: the pc and hook it acts on are from one hv_stop_publish, never the new pc with the old hook (or none) nor an
   old pc with the new hook, even when the stop moves away and back (A, B, A) while it reads. It reads again while a
   move is under way (a few stores), so it answers with the stop as it was before or after each move: a stop that is at
   pc on both sides of a move is always met. NULL still means "halt here". A stop for some CPUs only (stop_cpus) is
   one the others pass: they end their blocks there too, as hv_at_stop is all the translator asks, and run on. */
static inline uint64_t hv_stop_pc(const HvMachine *m) { return __atomic_load_n(&m->stop_pc, __ATOMIC_RELAXED); }
static inline uint64_t hv_stop_pc2(const HvMachine *m) { return __atomic_load_n(&m->stop_pc2, __ATOMIC_RELAXED); }
static inline int hv_at_stop(const HvMachine *m, uint64_t pc) { return pc == hv_stop_pc(m) || pc == hv_stop_pc2(m); }
/* What hv_stop_hook found: here 0 if the stop has moved away from pc or is not for this CPU; else here 1 and the hook
   to call (NULL: halt). */
typedef struct { int here; void (*hook)(HvCpu *cpu); } HvStopAt;
/* After hv_at_stop(m, pc). Out of line and cold, and its answer comes back in registers: inlined, its retry loop took
   registers the dispatch loop keeps for every block, which then reloaded them from the stack before its check.
   A pc that is not 4-aligned is never at a stop: no instruction is there, and fetching there takes the PC alignment
   fault first. That keeps "no stop", ~0 in stop_pc and stop_pc2, from being one: hv_at_stop matches a guest that
   jumps to -1, and the CPU must take that fault, as a real one does, not halt (on CPU 0, the whole Mac). */
static inline __attribute__((noinline, cold)) HvStopAt hv_stop_hook(const HvMachine *m, uint64_t pc, uint32_t cpu) {
    if (pc & 3) return (HvStopAt){0, NULL};
    for (unsigned spins = 0;; spins++) {
        uint32_t seq = __atomic_load_n(&m->stop_seq, __ATOMIC_ACQUIRE);
        if (seq & 1) { /* a move under way: a few stores, unless its thread lost the CPU in them */
            if (spins >= 256) sched_yield();
            continue;
        }
        uint32_t cpus = __atomic_load_n(&m->stop_cpus, __ATOMIC_RELAXED);
        HvStopAt at = {hv_at_stop(m, pc) && (!cpus || (cpus >> cpu & 1)),
                       __atomic_load_n(&m->stop_hook, __ATOMIC_RELAXED)};
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&m->stop_seq, __ATOMIC_RELAXED) == seq) return at;
    }
}
/* For a CPU stopped at its stop, on its thread: when it runs again, it runs past the stop there once, the stop staying
   where it is for every CPU (--break stepping past a hit, with nothing to move and so nothing to flush). Only if that
   is the first place the run comes to: an interrupt taken first goes elsewhere, and a thread the guest switches to
   there meets the stop, as does this one when it comes back. It runs past whatever stop is there, a hook's too: it
   is for a halt that stays put, as --break's does, and never for a hook's stop. */
static inline void hv_cpu_pass_stop(HvCpu *cpu) { cpu->pc_state->stop_pass = cpu->pc; }
/* hv_machine_set_stop's stores, for one writer at a time: the count odd, the stop, the count even again. */
static inline void hv_stop_publish(HvMachine *m, uint64_t pc, uint64_t pc2, void (*hook)(HvCpu *cpu), uint32_t cpus) {
    uint32_t seq = __atomic_load_n(&m->stop_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&m->stop_seq, seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&m->stop_hook, hook, __ATOMIC_RELAXED);
    __atomic_store_n(&m->stop_cpus, cpus, __ATOMIC_RELAXED);
    __atomic_store_n(&m->stop_pc2, pc2, __ATOMIC_RELAXED);
    __atomic_store_n(&m->stop_pc, pc, __ATOMIC_RELAXED);
    __atomic_store_n(&m->stop_seq, seq + 2, __ATOMIC_RELEASE);
}

/* Does this CPU run a thread that asked for x86 ordering (ACTLR_EL1.EnTSO, which macOS sets for an Intel app's
   threads), with software TSO following the thread? Its EL0 blocks are HV_BLOCK_TSO. */
static inline int hv_tso_thread(const HvCpu *cpu) {
    return cpu->el == 0 && (cpu->actlr_el1 & 2) && cpu->m->software_tso && !cpu->m->tso_every;
}

static inline uint32_t hv_block_hash(uint64_t va, uint64_t pa, uint32_t flags) {
    uint64_t h = (va >> 2) ^ (pa >> 2) * 0x9e3779b97f4a7c15ull ^ flags;
    return (uint32_t)(h ^ (h >> 29)) & (HV_BLOCK_HASH - 1);
}

/* Lock-free. A block goes into its bucket with a release store of its index after its record (hash_next too) is
   written. It comes out (retire_page, emptying a zone) with a release store of its hash_next into the link to it,
   its own hash_next left as it was, and its record is used again only after its zone is emptied and every CPU has
   left what it was doing (a stop, or the reclaimer's grace period): so a lookup standing on it goes on along a valid
   rest of the chain. A block of a zone being emptied (zones_dying) is never returned, though it may still be in the
   bucket: from the moment the zone is marked nothing new may lead into it (dispatch.c, the reclaimer). */
static inline int hv_block_dying(const HvMachine *m, const HvBlock *b) {
    return __atomic_load_n(&m->zones_dying, __ATOMIC_RELAXED) >> b->zone & 1;
}
static inline HvBlock *hv_block_find(HvMachine *m, uint32_t h, uint64_t va, uint64_t pa, uint32_t flags, uint32_t gen) {
    for (uint32_t i = __atomic_load_n(&m->hash[h], __ATOMIC_ACQUIRE); i;) {
        HvBlock *b = &m->blocks[i - 1];
        if (b->va == va && b->pa == pa && b->flags == flags && b->gen == gen && !hv_block_dying(m, b)) return b;
        i = __atomic_load_n(&b->hash_next, __ATOMIC_ACQUIRE);
    }
    return NULL;
}
_Static_assert(sizeof(((HvMachine *)0)->hash) == HV_BLOCK_HASH * sizeof(uint32_t), "hash buckets are block indexes");

/* ---- The hash, the pages' block lists and their lists of exits from other pages: changed under tr_mu ---- */

static inline uint32_t hv_block_index(const HvMachine *m, const HvBlock *b) { return (uint32_t)(b - m->blocks) + 1; }

/* Into bucket h, first: its link, then the bucket with a release store (lookups read it lock-free). */
static inline void hv_hash_insert(HvMachine *m, HvBlock *b, uint32_t h) {
    b->hash_next = m->hash[h];
    __atomic_store_n(&m->hash[h], hv_block_index(m, b), __ATOMIC_RELEASE);
}

/* Out of its bucket: the link to it gets its hash_next with a release store (CPUs may be walking the bucket); its
   own hash_next stays, for a lookup standing on it (hv_block_find). Walks the bucket to find that link: a hash link
   has no way back (no room in the record, and lookups need the forward link left alone). Returns the links walked
   past (none if it is first). */
static inline uint32_t hv_hash_remove(HvMachine *m, HvBlock *b) {
    uint32_t me = hv_block_index(m, b), *pp = &m->hash[hv_block_hash(b->va, b->pa, b->flags)], n = 0;
    while (*pp && *pp != me) pp = &m->blocks[*pp - 1].hash_next, n++;
    if (*pp) __atomic_store_n(pp, b->hash_next, __ATOMIC_RELEASE);
    return n;
}

/* A page's list of blocks: for a listed block b, page_prev is 0 exactly when the page's head is b, else the block it
   names has page_next naming b; and the block page_next names has page_prev naming b. Unlisted: both 0. */
static inline void hv_page_list_insert(HvMachine *m, HvRam *r, uint32_t pg, HvBlock *b) {
    uint32_t me = hv_block_index(m, b), n = r->block_heads[pg];
    b->page_prev = 0, b->page_next = n;
    if (n) m->blocks[n - 1].page_prev = me;
    r->block_heads[pg] = me;
}
static inline void hv_page_list_remove(HvMachine *m, HvBlock *b) { /* only a listed block */
    uint32_t p = b->page_prev, n = b->page_next;
    if (p) m->blocks[p - 1].page_next = n;
    else b->ram->block_heads[b->pg] = n;
    if (n) m->blocks[n - 1].page_prev = p;
    b->page_prev = b->page_next = 0;
}

/* A RAM page as a key (HV_PAGE_KEY_BITS) and back. */
static inline uint32_t hv_page_key(const HvMachine *m, const HvRam *r, uint32_t pg) {
    return ((uint32_t)(r - m->ram) << HV_PAGE_KEY_BITS | pg) + 1;
}
static inline HvRam *hv_page_key_ram(HvMachine *m, uint32_t key) { return &m->ram[(key - 1) >> HV_PAGE_KEY_BITS]; }
static inline uint32_t hv_page_key_pg(uint32_t key) { return (key - 1) & ((1u << HV_PAGE_KEY_BITS) - 1); }

/* Exit idx onto page pg's list of exits from other pages, first (it is on none: slot_xpg 0); and off its list. */
static inline void hv_xlist_insert(HvMachine *m, HvRam *r, uint32_t pg, uint32_t idx) {
    uint32_t n = r->chain_heads[pg];
    m->slot_xpg[idx] = hv_page_key(m, r, pg);
    m->slot_xprev[idx] = 0, m->slot_xnext[idx] = n;
    if (n) m->slot_xprev[n - 1] = idx + 1;
    r->chain_heads[pg] = idx + 1;
    m->nxchained++;
}
static inline void hv_xlist_remove(HvMachine *m, uint32_t idx) { /* only when slot_xpg[idx] != 0 */
    uint32_t k = m->slot_xpg[idx], p = m->slot_xprev[idx], n = m->slot_xnext[idx];
    if (p) m->slot_xnext[p - 1] = n;
    else hv_page_key_ram(m, k)->chain_heads[hv_page_key_pg(k)] = n;
    if (n) m->slot_xprev[n - 1] = p;
    m->slot_xpg[idx] = m->slot_xprev[idx] = m->slot_xnext[idx] = 0;
    m->nxchained--;
}

/* Blocks thrown out, by key, to tell what a translation makes again (HvStats.retrans_*), always counted: 4 MB, one
   16-byte set written per block thrown out and read per block published, both under tr_mu off the fast path.
   2^HV_GHOST_SET_BITS sets of HV_GHOST_WAYS 16-bit entries, newest first: a note goes in at the front of its key's
   set and pushes the others back as far as the first empty entry, the oldest falling off the end, so a set holds
   the last 8 keys thrown out that fell in it. That is 2M keys, about three zones of 650K blocks (test_ghost):
   99.9% of a zone's keys are there just after it is emptied, 95% after one more such zone, 71% after two, 37% after
   three, so code thrown out longer ago than that is mostly made again uncounted (direct-mapped, the same memory
   kept 86% / 63% / 46%). An entry: 12 more bits of the key's hash (never 0: an entry of 0 is empty), the low 3 bits
   of the page generation the block was made under, and whether an emptied zone threw it out (else a write to its
   page). A key matches an entry of its set with its 12 bits, so in a full set another key's entry passes for it in
   about 8 in 4096 lookups (0.19% measured: a translation of new code counted as made again, and that entry lost); a
   page written a multiple of 8 times between a block thrown out and made again passes for unchanged. A match is
   cleared: each is counted once. */
#define HV_GHOST_SET_BITS 18
#define HV_GHOST_WAYS 8
#define HV_GHOST_ENTRIES ((size_t)HV_GHOST_WAYS << HV_GHOST_SET_BITS)
#define HV_GHOST_EVICTED 1u /* an emptied zone's, its page's code unchanged since (else retired: its page written) */
static inline uint64_t hv_ghost_hash(uint64_t va, uint64_t pa, uint32_t flags) {
    uint64_t h = va * 0x9e3779b97f4a7c15ull ^ (pa + 0x632be59bd9b4e019ull) * 0xc2b2ae3d27d4eb4full ^ flags;
    h ^= h >> 31, h *= 0xbf58476d1ce4e5b9ull, h ^= h >> 29, h *= 0x94d049bb133111ebull;
    return h ^ (h >> 32);
}
static inline uint16_t *hv_ghost_set(HvMachine *m, uint64_t h) {
    return &m->ghost[(size_t)(h & ((1u << HV_GHOST_SET_BITS) - 1)) * HV_GHOST_WAYS];
}
static inline uint16_t hv_ghost_tag(uint64_t h) {
    uint16_t t = (uint16_t)(h >> HV_GHOST_SET_BITS) & 0xfffu;
    return (uint16_t)((t ? t : 1) << 4);
}
static inline uint16_t hv_ghost_entry(uint64_t h, uint32_t gen, uint32_t why) {
    return (uint16_t)(hv_ghost_tag(h) | (gen & 7u) << 1 | why);
}
/* m->ghost is NULL only for a machine init_rest has not set up (it fails the machine without the table). */
static inline void hv_ghost_note(HvMachine *m, const HvBlock *b, uint32_t why) {
    if (!m->ghost) return;
    uint64_t h = hv_ghost_hash(b->va, b->pa, b->flags);
    uint16_t *s = hv_ghost_set(m, h);
    int w = 0;
    while (w < HV_GHOST_WAYS - 1 && s[w]) w++;
    memmove(s + 1, s, (size_t)w * sizeof *s);
    s[0] = hv_ghost_entry(h, b->gen, why);
}
/* What threw out a block with this key, made under page generation gen now: HV_GHOST_EVICTED (an emptied zone, its
   page unchanged since), HV_GHOST_WRITTEN (a write to its page, or emptied and its page written since), or 0 (not
   found). Its entry is cleared. */
#define HV_GHOST_WRITTEN 2
static inline int hv_ghost_take(HvMachine *m, uint64_t va, uint64_t pa, uint32_t flags, uint32_t gen) {
    if (!m->ghost) return 0;
    uint64_t h = hv_ghost_hash(va, pa, flags);
    uint16_t *s = hv_ghost_set(m, h), t = hv_ghost_tag(h);
    for (int w = 0; w < HV_GHOST_WAYS; w++)
        if ((s[w] & 0xfff0u) == t) {
            uint16_t e = s[w];
            s[w] = 0;
            return e == hv_ghost_entry(h, gen, HV_GHOST_EVICTED) ? (int)HV_GHOST_EVICTED : HV_GHOST_WRITTEN;
        }
    return 0;
}

/* HvStats.entries_*: which bucket a block's HvBlock.entries counts in. */
static inline int hv_entries_bucket(uint8_t e) { return e <= 1 ? 0 : e <= 8 ? 1 : e <= 64 ? 2 : 3; }
static inline void hv_block_entered(HvBlock *b) {
    uint8_t e = __atomic_load_n(&b->entries, __ATOMIC_RELAXED);
    if (e != 255) __atomic_store_n(&b->entries, (uint8_t)(e + 1), __ATOMIC_RELAXED);
}

/* HvRam.code_hash of page pg's bytes as they are now: never 0. Four lanes, so it runs at about a word a cycle (about
   a microsecond for 16 KB): on a page becoming code, and on a write dropping its code, both slow already. */
static inline uint32_t hv_page_hash(const HvRam *r, uint64_t pg) {
    uint64_t off = pg << HV_RAM_PAGE_BITS, n = r->size - off < (1ull << HV_RAM_PAGE_BITS) ? r->size - off : 1ull << HV_RAM_PAGE_BITS;
    const uint8_t *p = r->host + off;
    uint64_t h[4] = {1, 2, 3, 4}, i = 0, w[4];
    for (; i + 32 <= n; i += 32) {
        memcpy(w, p + i, 32);
        for (int k = 0; k < 4; k++) h[k] = (h[k] ^ w[k]) * 0x9e3779b97f4a7c15ull, h[k] ^= h[k] >> 29;
    }
    for (; i < n; i++) h[0] = (h[0] ^ p[i]) * 0x9e3779b97f4a7c15ull;
    uint64_t x = h[0] ^ h[1] * 0xbf58476d1ce4e5b9ull ^ h[2] * 0x94d049bb133111ebull ^ h[3] * 0xc2b2ae3d27d4eb4full;
    x ^= x >> 32;
    return (uint32_t)x | 1;
}

/* ThreadSanitizer can't see what is published through code that runs (chain(), dispatch.c), so it is told of
   that edge; nothing outside sanitizer builds. */
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define HV_TSAN 1
#endif
#endif
#ifdef HV_TSAN
void __tsan_acquire(void *addr);
void __tsan_release(void *addr);
#define hv_tsan_acquire(p) __tsan_acquire(p)
#define hv_tsan_release(p) __tsan_release(p)
#else
#define hv_tsan_acquire(p) ((void)0)
#define hv_tsan_release(p) ((void)0)
#endif

int hv_machine_init(HvMachine *m, size_t jit_bytes);
/* The same with JIT memory the host provides (iOS: the region a debugger granted and its writable alias). */
int hv_machine_init_jit(HvMachine *m, void *rx, void *rw, size_t jit_bytes);
/* The experimental Rosetta mode (0 off, 1 on, 2 the C reference), which hv_machine_init takes from HV_ROSETTA:
   for a machine whose mode is chosen elsewhere (a Mac's own setting, or the mode it was suspended in). Software TSO
   follows it (on with Rosetta) unless HV_SOFTWARE_TSO says otherwise. 0; -1 (nothing changed) if the host can't run
   Rosetta mode (no FEAT_AFP); -2 (nothing changed) for another mode once the machine holds translated code, which was
   made for this one. The same mode again is fine. (A resume sets the saved mode before it loads the code store, which
   loads only if it was made in that mode: hv_translate_config.) */
int hv_machine_set_rosetta(HvMachine *m, int mode);
void hv_machine_free(HvMachine *m);
/* Before its RAM is added: the machine will have at most n CPUs (1..HV_MAX_CPUS), so the RAM's per-CPU tables
   are made for n. A CPU reset with an index past it aborts. */
void hv_machine_set_cpus(HvMachine *m, int n);
int hv_machine_add_ram(HvMachine *m, uint64_t pa, uint64_t size, void *host, int readonly);
int hv_machine_add_mmio(HvMachine *m, uint64_t pa, uint64_t size, void *opaque,
                        uint64_t (*read)(void *, uint64_t, unsigned),
                        void (*write)(void *, uint64_t, uint64_t, unsigned), const char *name);

HvRam *hv_ram_lookup(HvMachine *m, uint64_t pa);
/* Drop the translations made from the RAM page holding pa (it was written, or the guest invalidated it). */
void hv_invalidate_code(HvMachine *m, uint64_t pa);
void hv_ret_clear(HvCpu *cpu);
int hv_ic_ivau_drops(void);
/* Settings that say how to translate or wait, to compare ways of doing it in one running guest
   ("knob NAME VALUE" in HV_INPUT_FILE; a new way starts from an empty code cache). Each starts
   from the environment variable named in hv_knob_names. */
enum { HV_K_NO_RAS, HV_K_SUPERBLOCKS, HV_K_GROUPS, HV_K_WFI_SPIN, HV_K_FLUSH, HV_K_TEST1, HV_K_TEST2, HV_K_TEST3, HV_K_TLBT, HV_K_JITCAP, HV_K_RECHECK, HV_K_SYNCWAIT, HV_K_SWITCHFLUSH, HV_K_RANGEDROP, HV_K_COUNT };
extern int hv_knob[HV_K_COUNT];
int hv_knob_set(HvMachine *m, const char *name, int value);
/* Memory: ask for the translation tables to be given back to the system (any thread). now: at a full flush at
   once (every block is translated again afterwards, which on a busy Mac stops it for seconds); otherwise the next
   time the code cache makes room as it fills, which gives back the emptied zone's share (all of them with one zone).
   trims counts them done. */
void hv_machine_request_trim(HvMachine *m, int now);
/* With every CPU stopped (hv_flush_blocks with one zone, after it has emptied the tables): swaps them for zero pages
   as far as they were used, and every CPU's branch caches. Returns the bytes given back. */
uint64_t hv_machine_trim_tables(HvMachine *m);
/* What a trim in a stop noted (giveback_pending): the zones' tables and, self NULL (no CPU running: tests), every
   CPU's branch caches, else self's own and those of CPUs still parked (the others give theirs back in unpark).
   Called by the CPU that stopped the others, after letting them go. */
uint64_t hv_machine_give_back(HvMachine *m, HvCpu *self);
/* One part of a trim given back, `freed` bytes of it: the last part done counts the trim (trims). */
void hv_machine_give_back_done(HvMachine *m, uint64_t freed);
/* One CPU's branch caches as zero pages: on its own thread, or with it not running. */
uint64_t hv_cpu_ijc_give_back(HvPerCpu *p);
/* A CPU that stayed off: its TLB, return and ASID tables, fill logs and branch caches as zero pages, its TLB not
   ready. On its own thread, active (wait_while_off). What the process held of them. */
uint64_t hv_cpu_off_give_back(HvCpu *cpu);
/* Branch-cache tables (a bit per table) emptied in place, as a run-out or a reset does: their pages stay held for
   the fills that follow, until a trim gives them back (hv_cpu_ijc_give_back). */
void hv_ijc_tables_clear(HvPerCpu *p, uint32_t which);
/* Zeros at [p, p + len) of anonymous memory: pages in memory cleared in place, compressed or swapped-out ones given
   fresh mappings, never-touched ones left alone (machine.c). */
void hv_zero_kept(void *p, size_t len);
/* The 64ths of [a, a + bytes) *used names zeroed, *used then 0 (mmu.c). */
void hv_parts_clear(void *a, size_t bytes, uint64_t *used);
/* jc's entries emptied: only the parts lookups wrote since the last time (jc_used). With the CPU not running. */
void hv_jc_clear(HvPerCpu *p);
/* The branch-cache filters (ijc_keyed, ijc_pages, both ijc_hint halves) and jc emptied, as far as they were filled
   (their *_used masks): a CPU's reset (mmu.c). */
void hv_ijc_filters_clear(HvPerCpu *p);
/* One emptied zone's share of the tables as zero pages: nothing may use them. self, if a CPU: it answers other CPUs'
   requests between tables, so their TLB syncs don't wait on the whole of it. */
uint64_t hv_machine_drop_zone_tables(HvMachine *m, int zone, HvCpu *self);
/* The same a piece at a time (the reclaimer, under tr_mu): from table *k, *at bytes into its pages, HV_DROP_PIECE
   bytes at a time until host tick until; the bytes it held added to *freed. 1: every table done. */
#define HV_DROP_PIECE ((size_t)1 << 20)
int hv_machine_drop_zone_tables_part(HvMachine *m, int zone, uint32_t *k, size_t *at, uint64_t *freed, uint64_t until);
/* Drop every translation at the CPUs' next stop (any thread), not just make room: a knob changed, a trim now. */
void hv_request_full_flush(HvMachine *m);
/* Moves the stop (pc, pc2; ~0 for none), its hook and the CPUs it is for (cpus; 0: all) while CPUs may run, from any
   thread, one caller at a time. Callers that set a stop flush translations after it (hv_request_full_flush): blocks
   made before end only at the stop they were made under, and chains made before may lead straight past the new one.
   So a running CPU meets a new stop once it has taken that flush, not as soon as this returns. Clearing the stop (~0)
   needs none. */
void hv_machine_set_stop(HvMachine *m, uint64_t pc, uint64_t pc2, void (*hook)(HvCpu *cpu), uint32_t cpus);
/* --break, set once as hengevm starts: halt CPU 0 (the one --break follows; the others run past) at pc, and flush
   what the CPUs made before it. ~0 (none given) leaves the stop as it is. CPU 0 goes on with hv_cpu_pass_stop. */
void hv_machine_set_break(HvMachine *m, uint64_t pc);
/* Zones: lays them out afresh, every one empty, zone 0 being filled (machine set-up and a full flush). */
void hv_jit_zones_reset(HvMachine *m);
/* The zone being filled: how far it is, from the cursors, into its HvJitZone (with every CPU stopped). And the other
   way: zone z to be filled next, the cursors from its HvJitZone. */
void hv_jit_zones_sync(HvMachine *m);
void hv_jit_zone_fill(HvMachine *m, int z);
/* Tests: n zones (1: the whole cache is flushed when full), from an empty cache. Before any CPU runs. */
void hv_machine_set_zones(HvMachine *m, int n);
/* Tests, with no CPU running: make room now, as a full cache does; victim >= 0 empties that zone (not the one being
   filled), -1 the one the dispatcher would choose. Takes tr_mu itself (the reclaimer may be running). */
void hv_jit_make_room(HvMachine *m, int victim);
/* Tests, with no CPU running (reclaim_manual set): mark zone v for the reclaimer, as a stop does (if no job is under
   way, none is ready, and v is in use, not dying and not the zone being filled), then do the work of job (rc_job as
   it was marked) one step at a time, as the reclaimer thread does: a chunk under tr_mu, or one look at the CPUs'
   answers. Step returns 1 once that job is over (done, or given up), 0 while there is more. */
void hv_jit_reclaim_start(HvMachine *m, int v);
int hv_jit_reclaim_step(HvMachine *m, uint32_t job);
/* Tests, with no other CPU running: link the exit whose slot is slot to block b, as the dispatcher does after a
   block left through it (chain: refused for a zone being emptied). */
void hv_jit_chain(HvMachine *m, uint64_t *slot, HvBlock *b);
/* With no CPU running or every one stopped: no zone empty or ready and no reclaim under way, start one (a code-store
   load, which may leave every zone full). */
void hv_jit_reclaim_kick(HvMachine *m);
/* hv_machine_free: the reclaimer told to quit and joined (its job given up first). */
void hv_jit_reclaim_stop(HvMachine *m);
/* Its job given up, with tr_mu held or no CPU running (a code-store load): what it owed of a trim handed over. */
void hv_jit_reclaim_cancel(HvMachine *m);
/* Code words in use and blocks held, over every zone (any thread: a snapshot that may be a little out of date). */
uint64_t hv_jit_words_used(const HvMachine *m);
uint32_t hv_blocks_held(const HvMachine *m);
uint64_t hv_jit_words_live(const HvMachine *m); /* hv_jit_words_used less the retired blocks' code */
/* Which zone a host address in the code region is in, or -1. */
static inline int hv_jit_zone_of(const HvMachine *m, const void *pc) {
    uint64_t off = (uint64_t)((const uint8_t *)pc - (const uint8_t *)m->jit.base) / 4;
    if ((const uint8_t *)pc < (const uint8_t *)m->jit.base || off >= m->jit.capacity || !m->zone_words) return -1;
    uint64_t z = off / m->zone_words;
    return z < (uint64_t)m->nzones ? (int)z : -1;
}
/* Zone z's block records and code words in use (the zone being filled has them in the cursors). */
static inline uint32_t hv_zone_nblocks(const HvMachine *m, int z) {
    if (z != __atomic_load_n(&m->zone, __ATOMIC_RELAXED)) return m->zones[z].nblocks;
    int64_t n = (int64_t)__atomic_load_n(&m->nblocks, __ATOMIC_RELAXED) - (int64_t)z * m->zone_blocks;
    return n < 0 ? 0 : n > (int64_t)m->zone_blocks ? m->zone_blocks : (uint32_t)n;
}
static inline uint32_t hv_zone_nslots(const HvMachine *m, int z) {
    if (z != __atomic_load_n(&m->zone, __ATOMIC_RELAXED)) return m->zones[z].nslots;
    int64_t n = (int64_t)__atomic_load_n(&m->nslots, __ATOMIC_RELAXED) - (int64_t)z * m->zone_slots;
    return n < 0 ? 0 : n > (int64_t)m->zone_slots ? m->zone_slots : (uint32_t)n;
}
static inline size_t hv_zone_used(const HvMachine *m, int z) {
    if (z != __atomic_load_n(&m->zone, __ATOMIC_RELAXED)) return m->zones[z].used;
    size_t c0 = (size_t)z * m->zone_words, used = __atomic_load_n(&m->jit.used, __ATOMIC_RELAXED);
    return used < c0 ? 0 : used - c0 > m->zone_words ? m->zone_words : used - c0;
}
/* The block whose code is at or just before host address pc in its zone, or NULL (profilers, stall reports;
   any thread, so it may be wrong while the cache changes, but never faults: the tables have a fixed size). */
static inline HvBlock *hv_block_at(HvMachine *m, const void *pc) {
    int z = hv_jit_zone_of(m, pc);
    if (z < 0 || !m->blocks) return NULL;
    uint32_t n = hv_zone_nblocks(m, z);
    if (!n) return NULL;
    /* In the order they were made, which is nearly the order of their code (each CPU fills its own chunk). */
    HvBlock *b0 = m->blocks + (size_t)z * m->zone_blocks;
    uint32_t lo = 0, hi = n - 1;
    while (lo < hi) {
        uint32_t mid = (lo + hi + 1) / 2;
        const uint32_t *c = b0[mid].code;
        if (c && (const void *)c <= pc) lo = mid;
        else hi = mid - 1;
    }
    return b0[lo].code && (const void *)b0[lo].code <= pc ? &b0[lo] : NULL;
}
/* Debugging (HV_WATCH): which pages keep losing their translations. */
extern uint32_t *hv_smc_counts;
extern uint64_t *hv_smc_last;
extern uint64_t hv_smc_pages, hv_smc_cpu, hv_smc_dma;
/* A device wrote guest memory directly: translations made from it are out of date. Any thread. */
void hv_dma_written(HvMachine *m, uint64_t pa, uint64_t len);
/* The same for a write of exactly [pa, pa + len) that C just made (hv_phys_write, an atomic done in C): only a write
   into a piece translated code came from (HvRam.code_mask) drops the page's translations. Any thread. */
void hv_phys_written(HvMachine *m, uint64_t pa, uint64_t len);
/* A block another CPU may run is out of date: its entry goes to its gate (HvBlock.code). Under tr_mu; the caller
   makes the patch visible (hv_patch_flush), and nothing makes it live again. */
void hv_block_gate(HvMachine *m, HvBlock *b);
/* Under tr_mu: rewrite the word at `at` in translated code, with the rest gathered, at hv_patch_flush. */
void hv_patch_add(HvMachine *m, uint32_t *at, uint32_t word);
void hv_patch_flush(HvMachine *m);
/* code_pages: a write is retiring the page's blocks (hv_invalidate_code); it becomes 0 once they are all gated. */
#define HV_CODE_RETIRING 255
/* Pages of ram (kept in files) whose marks the caller just cleared, with a full fence after: no CPU keeps a
   writable TLB entry to them, or a store through one still under way, once this returns (each CPU that may have
   one has dropped it and left its block). Any thread; a CPU's own entries are dropped in place. */
void hv_ram_revoke_writes(HvMachine *m, HvRam *ram, const uint32_t *pages, uint32_t n);
/* The region's memory is kept in f (core/ramfile.h): its marks and the writer's revoke hooked up. -1 (nothing
   changed) when the file's pages aren't the TLB's. */
int hv_machine_attach_file(HvMachine *m, HvRam *r, struct HvRamFile *f);
HvMmio *hv_mmio_lookup(HvMachine *m, uint64_t pa);
/* Host pointer for `len` bytes of guest RAM at pa, or NULL, that the caller may write through: while an auto-save's
   guard is armed, the pages it covers are kept as they were first (core/saveguard.h). The default, so that a writer
   that forgets to say so still can't spoil a save; a pointer that is only read takes hv_phys_ptr_r. */
void *hv_phys_ptr(HvMachine *m, uint64_t pa, uint64_t len);
/* hv_phys_ptr, said where the caller writes */
void *hv_phys_ptr_w(HvMachine *m, uint64_t pa, uint64_t len);
/* Read only: claims nothing (no copy while a save is armed), and the compiler refuses a store through it. For
   reads, above all hot or large ones (code fetch, page walks, a disk write's data). */
const void *hv_phys_ptr_r(HvMachine *m, uint64_t pa, uint64_t len);

/* Physical accesses from C (page-table walks, devices doing DMA, slow paths). */
int hv_phys_read(HvMachine *m, uint64_t pa, void *buf, unsigned len);
int hv_phys_write(HvMachine *m, uint64_t pa, const void *buf, unsigned len);

void hv_cpu_reset(HvCpu *cpu, HvMachine *m); /* CPU 0 */
void hv_cpu_reset_index(HvCpu *cpu, HvMachine *m, uint32_t index);
/* The CPU this thread runs (NULL outside the dispatcher: tests, devices). */
extern __thread HvCpu *hv_self;
/* Count n more in a counter only one thread writes (a CPU's own, HvPerCpu): other threads read it at any time
   (hv_stats_sum), so the write must be atomic too, or it is a data race. A relaxed load and store is, and on arm64 is
   the same ldr, add, str as a plain +=; an atomic add would be a slower ldadd, needed only with more writers. */
static inline void hv_count_own(uint64_t *c, uint64_t n) {
    __atomic_store_n(c, __atomic_load_n(c, __ATOMIC_RELAXED) + n, __ATOMIC_RELAXED);
}
/* Count where the caller has no CPU of its own at hand (code run by CPU threads and other threads alike): into the
   running CPU's own counters on a CPU thread, else into HvMachine.stats atomically. Not for the light refill and
   branch-cache paths (core/mmu.c): reading a thread-local may call into the system. */
#define HV_STAT_ADD(mach, field, n)                                                                                  \
    do {                                                                                                             \
        HvCpu *self_ = hv_self;                                                                                      \
        if (self_ && self_->m == (mach) && self_->pc_state) hv_count_own(&self_->pc_state->stats.field, (n));       \
        else __atomic_add_fetch(&(mach)->stats.field, (n), __ATOMIC_RELAXED);                                        \
    } while (0)
/* Every CPU's counters and the machine's added up (relaxed reads: a total, not a snapshot of one instant). */
void hv_stats_sum(const HvMachine *m, HvStats *out);
/* One counter added up the same way (HV_STAT_TOTAL(m, blocks)). */
static inline uint64_t hv_stat_total(const HvMachine *m, size_t offset) {
    uint64_t n = __atomic_load_n((const uint64_t *)((const char *)&m->stats + offset), __ATOMIC_RELAXED);
    for (int i = 0; i < HV_MAX_CPUS; i++)
        n += __atomic_load_n((const uint64_t *)((const char *)&m->percpu[i].stats + offset), __ATOMIC_RELAXED);
    return n;
}
#define HV_STAT_TOTAL(m, field) hv_stat_total((m), offsetof(HvStats, field))
/* Count n more in a CPU's own counters (HV_CPU_COUNT(cpu, blocks, 1)), from code that has the CPU (the same thread,
   or one that has it stopped). Tests that build a bare HvCpu without per-CPU state count into the machine's, which
   other threads write too: atomically there. No thread-local read: safe on the light refill path. An expression. */
#define HV_CPU_COUNT(cpu, field, n)                                                                                  \
    ((cpu)->pc_state ? hv_count_own(&(cpu)->pc_state->stats.field, (n))                                              \
                     : (void)__atomic_add_fetch(&(cpu)->m->stats.field, (n), __ATOMIC_RELAXED))
/* One of a CPU's own counters (HV_CPU_STAT(cpu, blocks)), read from code that has the CPU. A relaxed load: on its own
   counters a plain ldr, and on a bare HvCpu's (the machine's, which other threads add to) no data race. */
#define HV_CPU_STAT(cpu, field)                                                                                      \
    __atomic_load_n((cpu)->pc_state ? &(cpu)->pc_state->stats.field : &(cpu)->m->stats.field, __ATOMIC_RELAXED)
/* One keyed table (offset of small_at or flush_pc in HvPerCpu) of every CPU merged: the first 8 keys, counts added. */
void hv_stats_keys(const HvMachine *m, size_t table, HvStatKey out[8]);
/* Count one more for key in a CPU's own keyed table (the first 8 keys seen). */
static inline void hv_stat_key(HvStatKey t[8], uint64_t key) {
    for (int i = 0; i < 8; i++) { /* only its CPU writes it; hv_stats_keys reads it from anywhere (hv_count_own) */
        if (t[i].n && t[i].key == key) { hv_count_own(&t[i].n, 1); return; }
        if (!t[i].n) { __atomic_store_n(&t[i].key, key, __ATOMIC_RELAXED), hv_count_own(&t[i].n, 1); return; }
    }
}
void hv_dev_lock(HvMachine *m);
void hv_dev_unlock(HvMachine *m);
/* Ask another CPU to apply a TLBI (or HV_REQ_* work); returns the sequence to wait for. */
uint64_t hv_cpu_post_tlbi(HvCpu *target, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint64_t xt);
uint64_t hv_cpu_post(HvCpu *target, uint32_t flags);
/* Wait until target has done everything up to seq (or is parked, and will before it runs). */
void hv_cpu_sync(HvCpu *self, HvCpu *target, uint64_t seq);
/* The owner: do what other CPUs asked. */
void hv_cpu_process_requests(HvCpu *cpu);
void hv_cpu_ack_wake(HvCpu *cpu);
void hv_big_lock(HvMachine *m);
void hv_big_unlock(HvMachine *m);
int hv_big_contended(HvMachine *m); /* HV_BIG_LOCK: another thread is waiting for it */
/* Make every CPU leave its block at the next exit. */
void hv_machine_kick_all(HvMachine *m);
/* Stop the whole machine for reason (an HvHaltReason): every CPU that has no reason yet gets this
   one, then every CPU is woken, out of WFI, the pause gate and PSCI CPU_OFF too (a CPU parked
   there waits on its condition variable, which an exit request alone never ends: with CPU 0
   parked, a shutdown asked for by another CPU left the machine frozen). Any thread. */
void hv_machine_halt(HvMachine *m, int reason);
/* Wake one CPU out of WFI (and make it leave its block). */
void hv_cpu_wake(HvCpu *cpu);
/* Make it leave its block at the next checked exit, without waking it from WFI. */
void hv_cpu_kick(HvCpu *cpu);

/* Pausing every CPU between guest instructions, from any thread but a CPU's. hv_machine_pause asks;
   hv_machine_paused is 1 once every CPU is at the gate, powered off or gone (none can run guest code
   until hv_machine_resume); hv_machine_pause_wait waits for that, up to timeout_ms (0 or -1). Resuming
   lets them go. hv_machine_pause_poke wakes the gate so CPUs see a new halted. */
void hv_machine_pause(HvMachine *m);
int hv_machine_paused(HvMachine *m);
int hv_machine_pause_wait(HvMachine *m, unsigned timeout_ms);
void hv_machine_resume(HvMachine *m);
/* Resuming after a stop the guest isn't to notice (one bounded by deadlines to a few milliseconds: the copy-on-write
   auto-save's, not the save that ships, which is a pause): its clock is not held back
   by the time stopped, so it reads the real time as if it had run all along. */
void hv_machine_resume_live(HvMachine *m);
void hv_machine_pause_poke(HvMachine *m);
/* Paused (as hv_machine_paused): the guest's counter when it stopped, for a snapshot. -1 if not paused. */
int hv_machine_paused_counter(HvMachine *m, uint64_t *counter);
/* A machine loaded from a snapshot, before its CPUs run: paused, its counter at `counter` until
   hv_machine_resume. */
void hv_machine_restore_paused(HvMachine *m, uint64_t counter);

/* From any thread: device work is waiting for the CPU thread's tick. Makes a
   running block leave and ends a WFI sleep. */
void hv_machine_wake(HvMachine *m);

#endif
