#ifndef HENGEVM_MMU_H
#define HENGEVM_MMU_H

#include <stdint.h>
#include <stdio.h>

#include "cpu.h"

struct HvRam;

typedef enum { HV_ACC_READ = 0, HV_ACC_WRITE = 1, HV_ACC_EXEC = 2 } HvAccess;

/* Result of a stage 1 translation for the EL1&0 regime. */
typedef struct HvWalk {
    uint64_t pa;
    uint32_t fsc;        /* 0 on success, otherwise the abort's fault status code */
    int s1ptw;           /* the fault happened reading a table */
    uint64_t block_size; /* size of the mapping that was found */
    uint8_t r, w, x;     /* permissions at the EL that was asked about */
    uint8_t attr;        /* MAIR attribute byte */
    uint8_t join;        /* a page smaller than the software page: 1 + join_small_pages' reason (0: joined); 0 otherwise */
} HvWalk;

/* Fault status codes (ESR ISS DFSC/IFSC). */
#define HV_FSC_ADDR_SIZE(l)   (0x00 + (l))
#define HV_FSC_TRANSLATION(l) (0x04 + (l))
#define HV_FSC_ACCESS(l)      (0x08 + (l))
#define HV_FSC_PERMISSION(l)  (0x0c + (l))
#define HV_FSC_EXTERNAL       0x10
#define HV_FSC_ALIGNMENT      0x21

/* el is the EL whose permissions apply (0 for LDTR/STTR at EL1). */
void hv_mmu_walk(HvCpu *cpu, uint64_t va, HvAccess acc, int el, HvWalk *out);

void hv_tlb_flush(HvCpu *cpu);
/* HvCpu.tlb_ready: whether the TLB holds entries (all invalid but the logged ones). Read by other CPUs. */
static inline int hv_tlb_is_ready(HvCpu *cpu) { return __atomic_load_n(&cpu->tlb_ready, __ATOMIC_ACQUIRE) == (void *)cpu; }
void hv_tlb_ready(HvCpu *cpu);      /* the TLB all invalid, before the CPU first runs (the run loop does it) */
void hv_tlb_reset(HvCpu *cpu);      /* a reset: the logged entries invalid, the logs empty; a TLB not ready untouched */
void hv_tlb_flush_va(HvCpu *cpu, uint64_t va);
void hv_tlb_flush_user(HvCpu *cpu);
void hv_kva_hist_dump(FILE *f); /* HV_KVA_HIST, see mmu.c */
void hv_kva_init(void);         /* reads HV_KVA_*: at machine setup, never on a fill */
/* A big range of kernel VAs: its kernel entries and cached branch targets, not every process's entries. */
void hv_tlb_flush_kernel_range(HvCpu *cpu, uint64_t va, uint64_t len);
void hv_tlb_flush_user_range(HvCpu *cpu, uint64_t va, uint64_t len);
void hv_tlb_switch_user(HvCpu *cpu, uint64_t old_ttbr0, uint64_t new_ttbr0); /* a process switch */
/* TCR_EL1 changed only TTBR0's controls (cpu->tcr_el1 is the new value): the same, for the TCR they were walked under. */
void hv_tlb_switch_tcr(HvCpu *cpu, uint64_t old_tcr);
void hv_tlb_stash_drop(HvCpu *cpu, int asid); /* asid -1: all */
/* Empty the indirect-branch cache for all user (0) or for kernel (1) VAs. */
void hv_ijc_reset(HvCpu *cpu, int kernel);
/* Both halves of the return tables emptied (a new salt each), on the CPU's own thread. */
void hv_ret_forget(HvCpu *cpu);
/* The indirect-branch cache slot of a tag (VA ^ salt); translated code computes the same. */
/* The cache is two-way: a tag's set is two entries, this one (way 0, the one
   filled last) and the next. */
static inline uint32_t hv_ijc_index(uint64_t tag) {
    return (uint32_t)(((tag ^ tag >> 16 ^ tag >> 46) >> 2) & ((1u << (HV_IJC_BITS - 1)) - 1)) << 1;
}
static inline uint32_t hv_ijc_key(uint64_t va, uint64_t salt) {
    return (uint32_t)(((va >> HV_TLB_PAGE_BITS) * 0x9e3779b97f4a7c15ull ^ (salt >> 48) * 0xc2b2ae3d27d4eb4full) >> 44);
}
/* A single-address TLBI covers the guest granule, including every software
   fragment of a 16/64 KB mapping. TCR changes flush current and stashed TLBs. */
static inline unsigned hv_tlbi_page_bits(const HvCpu *cpu, uint64_t va) {
#if HENGE_ROSETTA_4K_CACHE
    int high = (va >> 55) & 1;
    unsigned tg = (cpu->tcr_el1 >> (high ? 30 : 14)) & 3;
    return high ? (tg == 1 ? 14 : tg == 3 ? 16 : 12)
                : (tg == 2 ? 14 : tg == 1 ? 16 : 12);
#else
    (void)cpu; (void)va;
    return HV_TLB_PAGE_BITS;
#endif
}
/* The guest page that code at va comes from, in bits: smaller than the software page only for a process macOS maps
   with 4 KB pages (TCR_EL1.TG0, an Intel app under Rosetta). Such a page may map anywhere, apart from its neighbours,
   and be remapped alone (the software TLB holds the software page only while all of its small pages are one run, see
   join_small_pages), so a translated block stays inside it, and only branches inside it chain without a check. */
static inline unsigned hv_code_page_bits(const HvCpu *cpu, uint64_t va) {
#if HV_TLB_PAGE_BITS > 12
    if ((cpu->sctlr_el1 & 1) && !((va >> 55) & 1) && ((cpu->tcr_el1 >> 14) & 3) == 0) return 12;
#endif
    (void)cpu; (void)va;
    return HV_TLB_PAGE_BITS;
}
/* The 4 KB table (table 3, the slot's spare place): a process macOS maps with 4 KB pages (an Intel app under Rosetta)
   has its own pages cached here, one 4 KB page per entry, indexed by VA >> 12, with the same tag scheme. Only its own
   blocks (HV_BLOCK_SMALL) and the C paths for its EL0 accesses read it; nothing that checks a whole software page does,
   so an entry never lets an access reach past its 4 KB. Every TLBI of a software page drops the 4 KB entries in it.
   The pages macOS has mapped with all their neighbours, as one aligned run, are cached in table 0 as well (join_small_pages). */
#if HV_TLB_PAGE_BITS > 12
#define HV_TLB_SMALL 1
#else
#define HV_TLB_SMALL 0
#endif
#define HV_TLB_SMALL_TABLE 3
#define HV_TLB_SMALL_BITS 12
#define HV_TLB_TABLES (3 + HV_TLB_SMALL) /* tables 0..2, and the 4 KB one where there is one (in use) */
/* Which table holds va for an access with el's permissions, and its page bits. */
static inline int hv_tlb_where(const HvCpu *cpu, uint64_t va, int el, unsigned *bits) {
    if (HV_TLB_SMALL && el == 0 && hv_code_page_bits(cpu, va) < HV_TLB_PAGE_BITS) {
        *bits = HV_TLB_SMALL_BITS;
        return HV_TLB_SMALL_TABLE;
    }
    *bits = HV_TLB_PAGE_BITS;
    return hv_tlb_table(cpu, el);
}
/* The same for a block's flags (the table its code and its EL0 accesses use). */
static inline int hv_tlb_block_table(uint32_t flags, unsigned *bits) {
    *bits = HV_TLB_PAGE_BITS;
    if (!(flags & 1u)) { /* HV_BLOCK_EL1 */
        if (!HV_TLB_SMALL || !(flags & 16u)) return 0; /* HV_BLOCK_SMALL (a 4 KB build's own pages are 4 KB already) */
        *bits = HV_TLB_SMALL_BITS;
        return HV_TLB_SMALL_TABLE;
    }
    return (flags & 8u) ? 2 : 1; /* HV_BLOCK_PAN */
}
static inline HvTlbEntry *hv_tlb_at(HvCpu *cpu, uint64_t va, int table, unsigned bits) {
    return hv_tlb_ent(cpu, (uint32_t)(va >> bits) & (HV_TLB_SIZE - 1), table);
}

/* Forget one address space's cached user targets. */
void hv_ijc_drop_asid(HvCpu *cpu, uint16_t asid);
/* TTBR0 changed: use the new address space's user entries. */
void hv_ijc_switch(HvCpu *cpu);
void hv_tlb_flush_va_asid(HvCpu *cpu, uint64_t va, int asid); /* asid -1: every address space */
void hv_tlb_flush_va_page(HvCpu *cpu, uint64_t va, int asid);
int hv_tlb_check_all(HvCpu *cpu); /* one page: stashes lose only its entries */
void hv_ijc_drop_va(HvCpu *cpu, uint64_t va, int asid);
/* Translated code's TLB miss: walk and fill without the full helper (see mmu.c). flags: HV_REFILL_* */
#define HV_REFILL_WRITE 1u
#define HV_REFILL_ON (1u << 9)     /* always set, so the flags are never 0 */
#define HV_REFILL_ALIGN (1u << 10) /* the access must be aligned */
int hv_tlb_refill(HvCpu *cpu, uint64_t va, uint32_t flags);
struct HvBlock;
void hv_ijc_fill(HvCpu *cpu, struct HvBlock *b);
int hv_ijc_fill_checked(HvCpu *cpu, struct HvBlock *b); /* only if the TLB still maps it (0: not cached) */
void *hv_ijc_resolve(HvCpu *cpu, uint64_t va, uint32_t flags);
/* Another CPU's TLBI of one page: drop target's entries for it from here. 1 if
   that is enough (no page walk of target's raced it), 0 if target must do it
   itself and be waited for. Its branch cache is still target's own business. */
/* asid -1: all. 1: done; 0: raced the target's own page walk or cache fill
   (try again); -1: the target has to do it itself. */
int hv_tlbi_drop_remote_va(HvCpu *target, uint64_t va, int asid);       /* just the cached branch targets in the page */
/* The same for npages pages from va in one go (big range TLBIs; HV_RANGE_DROP sets how big). */
int hv_tlbi_drop_remote_range(HvCpu *target, uint64_t va, uint64_t npages, int asid);
void hv_ijc_drop_range(HvCpu *cpu, uint64_t va, uint64_t len, int asid);
void hv_ucode_add(HvCpu *cpu, uint64_t va); /* a user target was cached in this page (current address space) */
/* Both return how many writable entries they dropped. */
int hv_tlb_protect_code(HvCpu *cpu, const uint8_t *host_page);
int hv_tlb_protect_code_page(HvCpu *cpu, struct HvRam *ram, uint64_t pg); /* the same, through the page's write hints */

/* Translates for a C access that stays inside one software page,
   filling the TLB when the page is ordinary RAM. Returns 0 and sets *pa, or the
   fault status code. */
uint32_t hv_mmu_access(HvCpu *cpu, uint64_t va, HvAccess acc, int el, uint64_t *pa);

/* For the dispatcher: host pointer to the guest instructions at va and how many
   can be read before the page ends. On a fault, the instruction abort is taken
   and NULL is returned. */
const uint32_t *hv_mmu_fetch(HvCpu *cpu, uint64_t va, uint64_t *pa, uint32_t *avail);

#endif
