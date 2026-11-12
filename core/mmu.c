#include "mmu.h"

#include <stdlib.h>
#include <string.h>

#include "exception.h"
#include "machine.h"
#include "translate.h"

/* VMSAv8-64 stage 1 translation for the EL1&0 regime, 4 KB, 16 KB and 64 KB
   granules, 48-bit VA and PA. There is no stage 2: HengeVM is the hypervisor. */

#define PAGE_MASK ((1ull << HV_TLB_PAGE_BITS) - 1)
_Static_assert(HV_BLOCK_EL1 == 1u && HV_BLOCK_PAN == 8u && HV_BLOCK_SMALL == 16u, "hv_tlb_block_table (mmu.h) reads these");
#define OA_MASK 0x0000ffffffffffffull

static int granule_bits(uint32_t tg, int ttbr1) {
    if (ttbr1) return tg == 1 ? 14 : tg == 3 ? 16 : 12; /* TG1: 01 16K, 10 4K, 11 64K */
    return tg == 2 ? 14 : tg == 1 ? 16 : 12;            /* TG0: 00 4K, 01 64K, 10 16K */
}

/* This file is built with -mgeneral-regs-only, and the walk and the TLB fill
   call nothing in libc: hv_tlb_refill runs them from translated code without
   saving the guest's SIMD registers, which libc's memory routines would use. */
static inline HvRam *ram_of(HvMachine *m, uint64_t pa) {
    if (__builtin_expect(m->main_ram && pa - m->main_ram->pa < m->main_ram->size, 1))
        return m->main_ram;
    for (int i = 0; i < m->nram; i++)
        if (pa - m->ram[i].pa < m->ram[i].size) return &m->ram[i];
    return NULL;
}

/* A descriptor: RAM, 8 bytes, aligned. Device memory as a page table is not supported. */
static inline int read_desc(HvMachine *m, uint64_t pa, uint64_t *out) {
    HvRam *r = ram_of(m, pa);
    if (__builtin_expect(!r || pa + 8 - r->pa > r->size, 0)) return -1;
    *out = __atomic_load_n((const uint64_t *)(const void *)(r->host + (pa - r->pa)), __ATOMIC_RELAXED);
    return 0;
}

/* A page smaller than the software page (4 KB pages: an Intel app under Rosetta, whose process macOS maps
   with 4 KB pages while its own are 16 KB). The software TLB holds only whole software pages, so on its own
   such a page sends every access to the slow path, with a page walk each time (Steam took minutes to start,
   at millions a second). macOS has usually mapped the software page's other small pages too, as one aligned
   run of physical memory with the same attributes (the 16 KB page they all come from): then the mapping is
   the software page's, and is reported so. The TLB is dropped by software page (a TLBI of any one of the
   small pages drops it), so a change to one of them is seen as a change to the whole.
   Bits compared: the lower attributes (valid, AttrIndx, NS, AP, SH, AF, nG) and the upper ones that change
   an access (GP, DBM, Contiguous, PXN, UXN); not the software or PBHA bits. */
#define JOIN_ATTRS (0xfffull | 0x1full << 50)

static void join_small_pages(HvCpu *cpu, uint64_t table, uint64_t idx, uint64_t desc, int g, HvWalk *out) {
    unsigned n = 1u << (HV_TLB_PAGE_BITS - g); /* small pages in a software page */
    uint64_t first = idx & ~(uint64_t)(n - 1), pa0 = 0;
    int why = 0; /* 1: one of them unmapped, 2: other attributes, 3: not one aligned run */
    for (unsigned i = 0; i < n && !why; i++) {
        uint64_t d = desc;
        if (first + i != idx && (read_desc(cpu->m, table + (first + i) * 8, &d) != 0 || (d & 3) != 3)) why = 1;
        else if ((d ^ desc) & JOIN_ATTRS) why = 2;
        else if (i == 0) pa0 = d & OA_MASK & ~((1ull << g) - 1), why = pa0 & PAGE_MASK ? 3 : 0;
        else if ((d & OA_MASK & ~((1ull << g) - 1)) != pa0 + ((uint64_t)i << g)) why = 3;
    }
    out->join = (uint8_t)(why + 1); /* counted when the walk fills the TLB (tlb_fill), not for walks that only look */
    if (!why) out->block_size = 1ull << HV_TLB_PAGE_BITS;
}

void hv_mmu_walk(HvCpu *cpu, uint64_t va, HvAccess acc, int el, HvWalk *out) {
    *out = (HvWalk){0};
    uint64_t sctlr = cpu->sctlr_el1;
    if (!(sctlr & 1)) {
        /* MMU off: flat, everything allowed. Data is Device-nGnRnE, fetches Normal. */
        out->pa = va;
        out->r = out->w = out->x = 1;
        out->block_size = 1ull << 30;
        out->attr = acc == HV_ACC_EXEC ? 0xff : 0x00;
        return;
    }

    uint64_t tcr = cpu->tcr_el1;
    int hi = (va >> 55) & 1;
    int tbi = hi ? (tcr >> 38) & 1 : (tcr >> 37) & 1;
    int tbid = hi ? (tcr >> 52) & 1 : (tcr >> 51) & 1;
    if (acc == HV_ACC_EXEC && tbid) tbi = 0;
    uint32_t tsz = hi ? (tcr >> 16) & 63 : tcr & 63;
    int epd = hi ? (tcr >> 23) & 1 : (tcr >> 7) & 1;
    int hpd = hi ? (tcr >> 42) & 1 : (tcr >> 41) & 1;
    int g = granule_bits(hi ? (tcr >> 30) & 3 : (tcr >> 14) & 3, hi);
    if (tsz < 16) tsz = 16;
    if (tsz > 39) tsz = 39;
    int inputsize = 64 - (int)tsz;

    /* Bits above the input size must all match bit 55 (or bit 63 without TBI). */
    int top = tbi ? 55 : 63;
    uint64_t mask = (top - inputsize + 1 >= 64 ? ~0ull : ((1ull << (top - inputsize + 1)) - 1)) << inputsize;
    if ((va & mask) != (hi ? mask : 0) || epd) {
        out->fsc = HV_FSC_TRANSLATION(0);
        return;
    }

    int stride = g - 3;
    int levels = (inputsize - g + stride - 1) / stride;
    int level = 4 - levels;
    uint64_t base = (hi ? cpu->ttbr1_el1 : cpu->ttbr0_el1) & OA_MASK & ~1ull;
    int ap_table = 0, pxn_table = 0, uxn_table = 0;

    for (;; level++) {
        int shift = g + stride * (3 - level);
        int idx_bits = level == 4 - levels ? inputsize - shift : stride;
        uint64_t idx = (va >> shift) & ((1ull << idx_bits) - 1);
        uint64_t desc;
        if (read_desc(cpu->m, base + idx * 8, &desc) != 0) {
            out->fsc = 0x14 + level; /* synchronous external abort on a table walk */
            out->s1ptw = 1;
            return;
        }
        if (!(desc & 1)) {
            out->fsc = HV_FSC_TRANSLATION(level);
            return;
        }
        if (level < 3 && (desc & 2)) { /* table */
            base = desc & OA_MASK & ~((1ull << g) - 1);
            if (!hpd) {
                pxn_table |= (desc >> 59) & 1;
                uxn_table |= (desc >> 60) & 1;
                ap_table |= (desc >> 61) & 3;
            }
            continue;
        }
        if (level == 3 && !(desc & 2)) {
            out->fsc = HV_FSC_TRANSLATION(level);
            return;
        }
        if (level < 3) { /* block: 4K at levels 1-2, 16K and 64K at level 2 */
            if (level == 0 || (g != 12 && level == 1)) {
                out->fsc = HV_FSC_TRANSLATION(level);
                return;
            }
        }
        uint64_t size = 1ull << shift;
        out->block_size = size;
        out->pa = (desc & OA_MASK & ~(size - 1)) | (va & (size - 1));
        out->attr = (cpu->mair_el1 >> (((desc >> 2) & 7) * 8)) & 0xff;
        if (!(desc & (1u << 10))) {
            out->fsc = HV_FSC_ACCESS(level);
            return;
        }
        int ap = (desc >> 6) & 3;
        int uxn = ((desc >> 54) & 1) | uxn_table;
        int pxn = ((desc >> 53) & 1) | pxn_table;
        int el0_access = (ap & 1) && !(ap_table & 1);
        int read_only = (ap & 2) || (ap_table & 2);
        if (el == 0) {
            out->r = el0_access;
            out->w = el0_access && !read_only;
            out->x = !uxn;
        } else {
            int pan = (cpu->pstate_extra & HV_PSTATE_PAN) && el0_access;
            out->r = !pan;
            out->w = !pan && !read_only;
            /* EL1 never executes what EL0 can write. */
            out->x = !pxn && !(el0_access && !read_only);
        }
        if ((sctlr >> 19) & 1 && !read_only && (el == 0 ? el0_access : 1)) out->x = 0; /* WXN */
        int ok = acc == HV_ACC_READ ? out->r : acc == HV_ACC_WRITE ? out->w : out->x;
        if (!ok) out->fsc = HV_FSC_PERMISSION(level);
        else if (g < HV_TLB_PAGE_BITS && level == 3) join_small_pages(cpu, base, idx, desc, g, out);
        return;
    }
}

int hv_tlb_tmajor;

/* Only the entries filled since the last flush can be valid (both logs, unless one ran over): those made invalid.
   A TLB not ready has none to make so (and its pages are left untouched). */
static void tlb_clear_filled(HvCpu *cpu) {
    uint32_t nu = cpu->nuser_fills, nk = cpu->nkernel_fills;
    if (!hv_tlb_is_ready(cpu)) {
    } else if (nu <= HV_USER_FILLS && nk <= HV_KERNEL_FILLS) {
        for (uint32_t k = 0; k < nu; k++) memset(&cpu->tlb[cpu->user_fills[k]], 0x80, sizeof cpu->tlb[0]);
        for (uint32_t k = 0; k < nk; k++) memset(&cpu->tlb[cpu->kernel_fills[k]], 0x80, sizeof cpu->tlb[0]);
    } else {
        memset(cpu->tlb, 0x80, sizeof cpu->tlb);
    }
    cpu->nuser_fills = 0;
    cpu->nkernel_fills = 0;
}

void hv_tlb_ready(HvCpu *cpu) {
    memset(cpu->tlb, 0x80, sizeof cpu->tlb);
    cpu->nuser_fills = 0;
    cpu->nkernel_fills = 0;
    __atomic_store_n(&cpu->tlb_ready, (void *)cpu, __ATOMIC_RELEASE);
}

void hv_tlb_reset(HvCpu *cpu) { tlb_clear_filled(cpu); }

void hv_tlb_flush(HvCpu *cpu) {
    if (cpu->m) HV_CPU_COUNT(cpu, tlb_flushes, 1);
    if (cpu->m) HV_CPU_COUNT(cpu, reset_why[0], 1);
    hv_ijc_reset(cpu, 0); /* mappings changed: branch targets cached by VA are no longer trusted */
    hv_ijc_reset(cpu, 1);
    tlb_clear_filled(cpu);
    hv_tlb_stash_drop(cpu, -1);
}

/* Clear the tables of one half (user tables are the even flags values, kernel ones odd). */
static uint32_t hint_slot(uint64_t key) {
    return (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 52);
}

_Static_assert(sizeof ((HvPerCpu *)0)->ijc_hint[0] / sizeof ((HvPerCpu *)0)->ijc_hint[0][0] == 1u << 12 &&
               sizeof ((HvPerCpu *)0)->ijc_keyed / 8 == 1u << 14 && sizeof ((HvPerCpu *)0)->ijc_pages[0] / 8 == 1u << 10,
               "the HV_PART sizes in hv_ijc_fill");

/* The used mask atomic: a parked CPU's own give-back may store it meanwhile (hv_cpu_ijc_give_back), with zeros on
   both sides, so either order leaves the array reading zero. */
void hv_parts_clear(void *a, size_t bytes, uint64_t *used) {
    for (uint64_t u = __atomic_load_n(used, __ATOMIC_RELAXED); u; u &= u - 1)
        memset((char *)a + (size_t)__builtin_ctzll(u) * (bytes / 64), 0, bytes / 64);
    __atomic_store_n(used, 0, __ATOMIC_RELAXED);
}

/* A half's hints emptied, as far as fills wrote them since the last time (only fills set a key; other CPUs' drops
   only read them, and this CPU's own only empty a slot again): a CPU with no fill since writes nothing. */
static void hint_clear(HvPerCpu *p, int kernel) {
    hv_parts_clear(p->ijc_hint[kernel], sizeof p->ijc_hint[kernel], &p->ijc_hint_used[kernel]);
}

void hv_jc_clear(HvPerCpu *p) {
    hv_parts_clear(p->jc, sizeof p->jc, &p->jc_used);
}

static void keyed_clear(HvPerCpu *p) {
    hv_parts_clear(p->ijc_keyed, sizeof p->ijc_keyed, &p->ijc_keyed_used);
    p->ijc_keyed_count = 0;
}

/* Every bit the filters hold was set by a fill (hv_ijc_fill), which marks the part it writes: what no fill marked
   reads zero already, and is left untouched. */
void hv_ijc_filters_clear(HvPerCpu *p) {
    keyed_clear(p);
    hv_parts_clear(p->ijc_pages[0], sizeof p->ijc_pages[0], &p->ijc_pages_used);
    hint_clear(p, 0);
    hint_clear(p, 1);
    hv_jc_clear(p);
}

/* Only tables a fill marked can hold anything (other CPUs' drops write only zeros, and only to marked tables:
   ijc_clear_page), so only those are written: untouched tables stay untouched (mem-engine #1). They are zeroed in
   place, keeping their pages: giving memory back is a trim's job (hv_cpu_ijc_give_back), and a run-out's table is
   about to be filled again, which on fresh pages would fault every one of them back in. */
static void ijc_clear(HvCpu *cpu, int kernel) {
    HvPerCpu *p = cpu->pc_state;
    if (!p) return;
    hint_clear(p, kernel);
    hv_ijc_tables_clear(p, kernel ? 0xaaaau : 0x5555u);
}

/* ASIDs live in TTBR0 unless TCR.A1 puts them in TTBR1; then every process looks alike to us. */
static int current_asid(HvCpu *cpu) {
    return (cpu->tcr_el1 >> 22) & 1 ? -1 : (int)(cpu->ttbr0_el1 >> 48);
}

/* ---- the (user id, page) set ---- */

static uint64_t ucode_key(uint32_t cid, uint64_t va) {
    return ((uint64_t)cid << 40 | ((va >> HV_TLB_PAGE_BITS) & 0xffffffffffull)) + 1; /* never 0, the empty slot */
}

static uint32_t ucode_slot(uint64_t key) {
    return (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> (64 - HV_UCODE_BITS));
}

static void ucode_clear(HvPerCpu *m) {
    if (m) m->last_fill[0] = 0;
    if (m && m->ucode && m->ucode_count) memset(m->ucode, 0, (sizeof *m->ucode) << HV_UCODE_BITS); /* in place, as the tables */
    if (m) m->ucode_count = 0;
}

static int ucode_has(HvPerCpu *m, uint32_t cid, uint64_t va) {
    uint64_t key = ucode_key(cid, va);
    for (uint32_t i = ucode_slot(key);; i = (i + 1) & ((1u << HV_UCODE_BITS) - 1)) {
        if (m->ucode[i] == key) return 1;
        if (!m->ucode[i]) return 0;
    }
}

void hv_ucode_add(HvCpu *cpu, uint64_t va) {
    HvPerCpu *m = cpu->pc_state;
    uint32_t cid = (uint32_t)(cpu->ijc_salt[0] >> 48);
    if (!m || !cid) return;
    uint64_t key = ucode_key(cid, va);
    uint32_t i = ucode_slot(key);
    for (; m->ucode[i]; i = (i + 1) & ((1u << HV_UCODE_BITS) - 1))
        if (m->ucode[i] == key) return;
    if (m->ucode_count >= (1u << HV_UCODE_BITS) / 2) { /* half full: start the user half over */
        ucode_clear(m);
        hv_ijc_reset(cpu, 0);
        hv_ijc_switch(cpu);
        cid = (uint32_t)(cpu->ijc_salt[0] >> 48);
        key = ucode_key(cid, va);
        i = ucode_slot(key);
    }
    m->ucode[i] = key;
    m->ucode_count++;
}

/* A user id no tag in the tables uses yet; when they run out, empty the user tables and start over. */
static uint16_t fresh_cid(HvCpu *cpu) {
    if (cpu->next_cid == 0 || cpu->next_cid > 0xffff) {
        /* Run out: zeroed in place, as the tables, but only what is in memory (hv_zero_kept). Every id's chunk was
           written as it was handed out, yet a trim may have given the set back since (hv_cpu_ijc_give_back), and a
           memset would bring all 2 MB in again. A CPU never reset (next_cid 0) has handed none out, and each is zeroed
           below as it is, so nothing is written for it. */
        if (cpu->next_cid > 0xffff && cpu->pc_state && cpu->pc_state->cid_chunks)
            hv_zero_kept(cpu->pc_state->cid_chunks, 65536 * sizeof *cpu->pc_state->cid_chunks);
        if (cpu->pc_state) cpu->pc_state->last_fill[0] = cpu->pc_state->last_fill[1] = 0;
        ijc_clear(cpu, 0);
        ucode_clear(cpu->pc_state);
        memset(cpu->asid_cid, 0, sizeof cpu->asid_cid);
        cpu->nasid_log = 0;
        cpu->next_cid = 1;
    }
    if (cpu->pc_state && cpu->pc_state->cid_chunks) memset(cpu->pc_state->cid_chunks[cpu->next_cid], 0, sizeof cpu->pc_state->cid_chunks[0]);
    return (uint16_t)cpu->next_cid++;
}

/* Empty a half of the return tables by giving it a new salt. From the CPU's own thread;
   the salts run out here, and it is the only one that may clear the tables. */
static void ret_new(HvCpu *cpu, int kernel) {
    if (cpu->ret_salt[kernel] >> 48 >= 0xf000) {
        hv_ret_clear(cpu); /* both halves: nothing is left under any salt */
        return;
    }
    __atomic_add_fetch(&cpu->ret_salt[kernel], 1ull << 48, __ATOMIC_RELAXED);
}

/* Both halves (the reclaimer's grace period: translated code of a zone being emptied may have filled them since it
   was marked). From the CPU's own thread. */
void hv_ret_forget(HvCpu *cpu) {
    ret_new(cpu, 0);
    ret_new(cpu, 1);
}

/* The same from another CPU's thread. 0 if it is too near running out to do it here. */
static int ret_new_remote(HvCpu *cpu, int kernel) {
    if (__atomic_load_n(&cpu->ret_salt[kernel], __ATOMIC_RELAXED) >> 48 >= 0xff00) return 0;
    __atomic_add_fetch(&cpu->ret_salt[kernel], 1ull << 48, __ATOMIC_RELAXED);
    return 1;
}

void hv_ijc_switch(HvCpu *cpu) {
    int asid = current_asid(cpu);
    ret_new(cpu, 0); /* a switch forgets the old process's returns: its frames miss once at worst */
    if (asid < 0) { /* no ASIDs to tell processes apart: a switch forgets the old one */
        cpu->ijc_salt[0] = (uint64_t)fresh_cid(cpu) << 48;
        return;
    }
    uint64_t base = cpu->ttbr0_el1 & OA_MASK & ~1ull;
    if (!cpu->asid_cid[asid] || cpu->asid_base[asid] != base) {
        cpu->asid_cid[asid] = fresh_cid(cpu);
        cpu->asid_base[asid] = base;
        cpu->cid_asid[cpu->asid_cid[asid]] = (uint16_t)asid;
        if (cpu->nasid_log < HV_ASID_LOG) cpu->asid_log[cpu->nasid_log] = (uint16_t)asid;
        if (cpu->nasid_log <= HV_ASID_LOG) cpu->nasid_log++;
    }
    cpu->ijc_salt[0] = (uint64_t)cpu->asid_cid[asid] << 48;
}

void hv_ijc_drop_asid(HvCpu *cpu, uint16_t asid) {
    if (cpu->m) HV_CPU_COUNT(cpu, ijc_resets, 1);
    cpu->asid_cid[asid] = 0;
    int cur = current_asid(cpu);
    if (cur == asid || cur < 0) hv_ijc_switch(cpu); /* without ASIDs, any drop may be about the current process */
}

void hv_ijc_reset(HvCpu *cpu, int kernel) {
    ret_new(cpu, kernel);
    if (cpu->pc_state) cpu->pc_state->last_fill[0] = cpu->pc_state->last_fill[1] = 0;
    /* Every entry of the half is dead now (new ids or kernel generation): so are their hints, and (the user half)
       the pages any process cached targets in. Only what fills wrote since the last time is written. */
    if (cpu->pc_state) {
        HvPerCpu *p = cpu->pc_state;
        hint_clear(p, kernel);
        if (!kernel) hv_parts_clear(p->ijc_pages[0], sizeof p->ijc_pages[0], &p->ijc_pages_used);
    }
    if (cpu->m) HV_CPU_COUNT(cpu, ijc_resets, 1);
    if (kernel) {
        /* Kernel VAs have 0xffff in bits 63..48, so the salt must not make that 0 (the empty tag). */
        if (++cpu->kernel_gen >= 0xffff) {
            ijc_clear(cpu, 1);
            cpu->kernel_gen = 1;
        }
        cpu->ijc_salt[1] = (uint64_t)cpu->kernel_gen << 48;
        return;
    }
    /* New user ids are how the user half's entries die. A CPU whose TLB isn't ready (off since it gave its tables
       back, or not run since its reset) with no user table filled has none to kill, and it doesn't run until a
       reset or a snapshot load matches its id to its ASID again (hv_ijc_switch): its ids stay. Handing out new ones
       would write the ASID tables again, an off CPU's given-back ones at every flush. A running CPU goes on: this
       reset may follow a change of where its ASID comes from (TCR.A1), and its id must follow. */
    if (cpu->pc_state && !hv_tlb_is_ready(cpu) &&
        !(__atomic_load_n(&cpu->pc_state->ijc_tables_used, __ATOMIC_RELAXED) & 0x5555u))
        return;
    /* Every process gets a new user id: clear the ids given since the last time (all, if the log ran over).
       The (id, page) set keeps the dead ids' pages (no live id matches them) until it is half full. */
    if (cpu->nasid_log <= HV_ASID_LOG)
        for (uint32_t k = 0; k < cpu->nasid_log; k++) cpu->asid_cid[cpu->asid_log[k]] = 0;
    else
        memset(cpu->asid_cid, 0, sizeof cpu->asid_cid);
    cpu->nasid_log = 0;
    if (cpu->pc_state) cpu->pc_state->last_fill[0] = 0;
    hv_ijc_switch(cpu);
}

/* TTBR0 changed or an ASID was dropped: only user-half (TTBR0) translations go;
   the kernel's TTBR1 entries stay. */
void hv_tlb_flush_user(HvCpu *cpu) {
    if (cpu->m) HV_CPU_COUNT(cpu, tlb_flushes, 1);
    uint32_t n = cpu->nuser_fills;
    cpu->nuser_fills = 0;
    if (n <= HV_USER_FILLS) {
        for (uint32_t k = 0; k < n; k++) {
            HvTlbEntry *e = &cpu->tlb[0] + cpu->user_fills[k];
            uint64_t tag = e->read_tag != HV_TLB_INVALID ? e->read_tag + HV_TLB_ADJ : e->exec_tag != HV_TLB_INVALID ? hv_exec_key(e->exec_tag, e->addend) : e->write_tag + HV_TLB_ADJ;
            if (e->read_tag != HV_TLB_INVALID || e->exec_tag != HV_TLB_INVALID || e->write_tag != HV_TLB_INVALID) if (!((tag >> 55) & 1)) memset(e, 0x80, sizeof *e); /* the slot may hold a kernel page by now */
        }
        return;
    }
    for (int t = 0; t < HV_TLB_TABLES; t++)
        for (uint32_t i = 0; i < HV_TLB_SIZE; i++) {
            HvTlbEntry *e = hv_tlb_ent(cpu, i, t);
            uint64_t tag = e->read_tag != HV_TLB_INVALID ? e->read_tag + HV_TLB_ADJ : e->exec_tag != HV_TLB_INVALID ? hv_exec_key(e->exec_tag, e->addend) : e->write_tag + HV_TLB_ADJ;
            if (e->read_tag != HV_TLB_INVALID || e->exec_tag != HV_TLB_INVALID || e->write_tag != HV_TLB_INVALID) if (!((tag >> 55) & 1)) memset(e, 0x80, sizeof *e);
        }
}

/* A TLBI for a big user range: only the user entries filled since the last
   flush that fall in it (all of them if there were too many to list). */
void hv_tlb_flush_user_range(HvCpu *cpu, uint64_t va, uint64_t len) {
    uint32_t n = cpu->nuser_fills;
    if (n > HV_USER_FILLS) {
        hv_tlb_flush_user(cpu);
        return;
    }
    if (cpu->m) if (cpu->m->stats_interval) HV_CPU_COUNT(cpu, tlb_range_flushes, 1);
    uint64_t lo = va & 0x00ffffffffffffffull;
    for (uint32_t k = 0; k < n; k++) {
        HvTlbEntry *e = &cpu->tlb[0] + cpu->user_fills[k];
        if (e->read_tag == HV_TLB_INVALID && e->exec_tag == HV_TLB_INVALID && e->write_tag == HV_TLB_INVALID) continue;
        uint64_t tag = e->read_tag != HV_TLB_INVALID ? e->read_tag + HV_TLB_ADJ : e->exec_tag != HV_TLB_INVALID ? hv_exec_key(e->exec_tag, e->addend) : e->write_tag + HV_TLB_ADJ;
        if ((tag >> 55) & 1) continue; /* the slot holds a kernel page by now */
        /* Any overlap (a 4 KB-granule range may start inside the page); tagged addresses: by the low 56 bits. */
        uint64_t pg = tag & 0x00ffffffffffffffull;
        if (pg < lo + len && pg + PAGE_MASK + 1 > lo) memset(e, 0x80, sizeof *e);
    }
}

/* A big range of kernel VAs. A full flush here also dropped every process's entries and both halves of the branch
   caches, and macOS 27 sends ranges like this hundreds of times a second (a phone running it did 120-260 full flushes a
   second, Sequoia none). Up to 64 MB page by page, as small ranges are; past that, a scan for kernel entries in the range
   and the kernel half of the branch caches. */
void hv_tlb_flush_kernel_range(HvCpu *cpu, uint64_t va, uint64_t len) {
    if (cpu->m && cpu->m->stats_interval) HV_CPU_COUNT(cpu, kernel_ranges, 1);
    uint64_t first = va & ~(uint64_t)PAGE_MASK;
    if (len <= (4096ull << HV_TLB_PAGE_BITS)) {
        for (uint64_t p = first; p < va + len; p += 1ull << HV_TLB_PAGE_BITS) hv_tlb_flush_va_asid(cpu, p, -1);
        return;
    }
    uint64_t lo = first & 0x00ffffffffffffffull, hi = (va + len) & 0x00ffffffffffffffull; /* tagged addresses: by the low 56 bits */
    for (int t = 0; t < 3; t++) /* the 4 KB table holds user pages only */
        for (uint32_t i = 0; i < HV_TLB_SIZE; i++) {
            HvTlbEntry *e = hv_tlb_ent(cpu, i, t);
            if (e->read_tag == HV_TLB_INVALID && e->exec_tag == HV_TLB_INVALID && e->write_tag == HV_TLB_INVALID) continue;
            uint64_t tag = e->read_tag != HV_TLB_INVALID ? e->read_tag + HV_TLB_ADJ : e->exec_tag != HV_TLB_INVALID ? hv_exec_key(e->exec_tag, e->addend) : e->write_tag + HV_TLB_ADJ;
            if (!((tag >> 55) & 1)) continue; /* a process's page */
            uint64_t pg = tag & 0x00ffffffffffffffull;
            if (pg < hi && pg + PAGE_MASK + 1 > lo) memset(e, 0x80, sizeof *e);
        }
    hv_ijc_reset(cpu, 1);
}

/* A page just got translated code: writes to it must take the slow path from now
   on, so drop just the write permission of the entries that map it. */
/* host_page is a RAM page (HV_RAM_PAGE_BITS); a smaller entry maps one of its parts. */
static int maps_writable(const HvTlbEntry *e, const uint8_t *host_page) {
    return e->write_tag != HV_TLB_INVALID &&
           (uint64_t)(e->write_tag + HV_TLB_ADJ + e->addend) - (uint64_t)(uintptr_t)host_page < (1ull << HV_RAM_PAGE_BITS);
}

/* RAM page pg just became code: drop write permission from cpu's entries for it.
   Returns how many it dropped (with none, cpu has no store to the page in flight). */
int hv_tlb_protect_code_page(HvCpu *cpu, HvRam *ram, uint64_t pg) {
    uint64_t *hint = &ram->whint[pg * ram->whint_cpus + __atomic_load_n(&cpu->index, __ATOMIC_RELAXED)];
    uint64_t h = __atomic_exchange_n(hint, 0, __ATOMIC_SEQ_CST);
    const uint8_t *host_page = ram->host + (pg << HV_RAM_PAGE_BITS);
    if (cpu->m && cpu->m->stats_interval) HV_STAT_ADD(cpu->m, protect_kind[h & HV_WHINT_MANY ? 2 : h ? 1 : 0], 1); /* cpu may be another CPU */
    if (!hv_tlb_is_ready(cpu)) return 0; /* no entry (its hints are older than its TLB), and not in a block */
    /* Ready, a hint may still be older than the entry it names: a fill whose place the hint names already (an entry
       there for this page that a TLBI or a reset dropped since, or one from before the TLB was last given back)
       stores no hint (write_hint). Such an entry is seen here through a store-then-load pair instead: the mark's CAS
       and the exchange above, then this read; the entry's write, then write_guard's fence and its read of the mark.
       Either this sees the entry, or that CPU sees the page as code and drops it itself. The is_ready read above is
       on this side of the same pair: a TLB made ready after it fills only entries that CPU's own mark check covers. */
    /* any hint means cpu mapped the page writable since the last protection. Its entry may be gone
       already without cpu having left the store it checked it for (a remote TLBI drops entries in place,
       hv_tlbi_drop_remote_va), so it is waited for even when nothing is left to drop. */
    if (h & HV_WHINT_MANY) { int d = hv_tlb_protect_code(cpu, host_page); return d ? d : 1; }
    int dropped = 0;
    for (int k = 0; k < 3; k++) {
        uint64_t c = h >> (16 * k) & 0xffff;
        if (!c) continue;
        HvTlbEntry *e = &cpu->tlb[0] + (c - 1);
        if (maps_writable(e, host_page)) e->write_tag = HV_TLB_INVALID, dropped++;
    }
    return dropped ? dropped : h != 0;
}

int hv_tlb_protect_code(HvCpu *cpu, const uint8_t *host_page) {
    int dropped = 0;
    for (int t = 0; t < HV_TLB_TABLES; t++)
        for (uint32_t i = 0; i < HV_TLB_SIZE; i++) {
            HvTlbEntry *e = hv_tlb_ent(cpu, i, t);
            if (maps_writable(e, host_page)) e->write_tag = HV_TLB_INVALID, dropped++;
        }
    return dropped;
}

/* Forget one salt's cached targets in one software page. Each 256-byte piece
   names 64 instruction slots; the hint is relative to this page's base. */
/* local: cpu itself (it may empty the page's hint slot afterwards). */
static void ijc_clear_page(HvCpu *cpu, uint64_t page, int kernel, uint64_t salt, int local) {
    HvPerCpu *m = cpu->pc_state;
    uint64_t key = (page ^ salt) | 1;
    __typeof__(m->ijc_hint[0][0]) *h = &m->ijc_hint[kernel][hint_slot(key)];
    uint64_t hk = __atomic_load_n(&h->key, __ATOMIC_RELAXED);
    /* Another key's slot means this one has filled nothing since its own drop or the last reset. */
    uint64_t bits = hk == key ? __atomic_load_n(&h->bits, __ATOMIC_RELAXED) : hk == ~0ull ? ~0ull : 0;
    if (!bits) return;
    for (int t = kernel; t < HV_IJC_TABLES; t += 2) {
        if (!(m->ijc_tables_used >> t & 1)) continue;
        uint64_t *tab = m->ijc + (size_t)t * (HV_IJC_TABLE_BYTES / 8);
        for (uint32_t piece = 0; piece < (1u << (HV_TLB_PAGE_BITS - 8)); piece++) {
            if (!(bits >> piece & 1)) continue;
            for (uint32_t o = piece * 64; o < piece * 64 + 64; o++) {
                uint64_t tag = (page | (uint64_t)o << 2) ^ salt;
                uint64_t *ent = tab + (size_t)hv_ijc_index(tag) * 2;
                if (ent[0] == tag) ent[0] = 0;
                if (ent[2] == tag) ent[2] = 0;
            }
        }
    }
    if (local && hk == key) h->key = 0, h->bits = 0;
}

/* The page may hold cached branch targets even if its TLB entry is gone: forget
   them, if the filters say it has any. */
/* remote: another CPU doing it (hv_tlbi_drop_remote_va): returns 0 instead of
   starting the user half over, which only cpu itself may do. */
static int ijc_drop_va(HvCpu *cpu, uint64_t va, int asid, int remote) {
    int kernel = (va >> 55) & 1;
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    {   /* A return may lead into the page. For the kernel only if it is code the CPU has run;
           xnu remaps data all the time and its code hardly ever. For a process only if it is the
           one running: a switch forgets the returns of the others anyway. */
        uint64_t page = va & ~((1ull << HV_TLB_PAGE_BITS) - 1);
        uint32_t sl = (uint32_t)(va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1);
        const HvTlbEntry *t1 = hv_tlb_ent(cpu, sl, 1), *t2 = hv_tlb_ent(cpu, sl, 2);
        /* A TLB not ready: the CPU has run nothing since its reset emptied its return tables (nor is it read) */
        int some = kernel ? hv_tlb_is_ready(cpu) && (t1->exec_tag == hv_exec_key(page, t1->addend) || t2->exec_tag == hv_exec_key(page, t2->addend)) : asid < 0 || asid == current_asid(cpu) || current_asid(cpu) < 0;
        if (some) {
            if (!remote) ret_new(cpu, kernel);
            else if (!ret_new_remote(cpu, kernel)) return 0;
        }
    }
    /* The filters say which targets a half's tables may hold; with none of its tables filled since they were last
       emptied there is none, and nothing to read (a CPU that never ran, or whose caches were given back: reading its
       filters or its ASID table would bring their pages back into memory) */
    if (m && p && (__atomic_load_n(&p->ijc_tables_used, __ATOMIC_RELAXED) & (kernel ? 0xaaaau : 0x5555u))) {
        int drop = 0;
        if (kernel) {
            uint32_t k = hv_ijc_key(va, cpu->ijc_salt[1]);
            drop = p->ijc_keyed[k >> 6] >> (k & 63) & 1;
        } else if (asid < 0) { /* every address space: any process's targets in the page count */
            uint32_t h = (uint32_t)(va >> HV_TLB_PAGE_BITS) & 0xffff;
            drop = p->ijc_pages[0][h >> 6] >> (h & 63) & 1;
        } else {
            uint64_t salt = current_asid(cpu) < 0 ? cpu->ijc_salt[0] : (uint64_t)cpu->asid_cid[asid] << 48;
            drop = (salt >> 48) && ucode_has(p, (uint32_t)(salt >> 48), va);
        }
        if (drop) {
            if (remote && !kernel && asid < 0) return 0;
            HV_STAT_ADD(m, reset_why[kernel ? 3 : 2], 1);
            uint64_t page = va & ~((1ull << HV_TLB_PAGE_BITS) - 1);
            if (kernel) ijc_clear_page(cpu, page, 1, cpu->ijc_salt[1], !remote);
            else if (asid < 0) { /* every address space: their slots differ, so start the user half over */
                HV_STAT_ADD(m, reset_why[5], 1);
                hv_ijc_reset(cpu, 0);
            }
            else ijc_clear_page(cpu, page, 0, current_asid(cpu) < 0 ? cpu->ijc_salt[0] : (uint64_t)cpu->asid_cid[asid] << 48, !remote);
        }
    }
    return 1;
}

void hv_ijc_drop_va(HvCpu *cpu, uint64_t va, int asid) {
    ijc_drop_va(cpu, va, asid, 0);
}

static void stash_drop_va(HvPerCpu *p, uint64_t va, int asid);

/* The 4 KB table's entries for the software page va is in (all of its 4 KB pages: TLBIs are by software page here, as
   they are for the other tables), whatever their tags. Another CPU may do it too (hv_tlbi_drop_remote_*), so only
   tags are written, one at a time. Returns whether one of them was for fetching from that page. */
static int drop_small(HvCpu *cpu, uint64_t va) {
    if (!HV_TLB_SMALL || ((va >> 55) & 1)) return 0; /* user pages only */
    uint32_t first = (uint32_t)((va & ~PAGE_MASK) >> HV_TLB_SMALL_BITS);
    int fetched = 0;
    for (uint32_t i = 0; i < 1u << (HV_TLB_PAGE_BITS - HV_TLB_SMALL_BITS); i++) {
        HvTlbEntry *e = hv_tlb_ent(cpu, (first + i) & (HV_TLB_SIZE - 1), HV_TLB_SMALL_TABLE);
        uint64_t x = __atomic_load_n(&e->exec_tag, __ATOMIC_RELAXED);
        if (x != HV_TLB_INVALID && ((hv_exec_key(x, e->addend) ^ va) & 0x00ffffffffffffffull & ~PAGE_MASK) == 0) fetched = 1;
        __atomic_store_n(&e->read_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
        __atomic_store_n(&e->write_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
        __atomic_store_n(&e->exec_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
    }
    return fetched;
}

void hv_tlb_flush_va_asid(HvCpu *cpu, uint64_t va, int asid) {
    /* Tagged addresses may sit in any slot's tag byte, so check by the low 56 bits. */
    uint32_t idx = (va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1);
    hv_ijc_drop_va(cpu, va, asid);
    if (!((va >> 55) & 1)) hv_tlb_stash_drop(cpu, asid);
    for (int el = 0; el < 3; el++) {
        HvTlbEntry *e = hv_tlb_ent(cpu, idx, el);
        memset(e, 0x80, sizeof *e);
    }
    drop_small(cpu, va);
}

/* One page (a TLBI by VA): as hv_tlb_flush_va_asid, but in the stashes only that page's entries go,
   not every entry of the address space (macOS 27 sends tens of thousands a second, and each made a
   process refill its whole TLB the next time it ran). */
void hv_tlb_flush_va_page(HvCpu *cpu, uint64_t va, int asid) {
    uint32_t idx = (va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1);
    hv_ijc_drop_va(cpu, va, asid);
    if (!((va >> 55) & 1) && cpu->pc_state) stash_drop_va(cpu->pc_state, va, asid);
    for (int el = 0; el < 3; el++) {
        HvTlbEntry *e = hv_tlb_ent(cpu, idx, el);
        memset(e, 0x80, sizeof *e);
    }
    drop_small(cpu, va);
}

/* A TLBI by range too big to do page by page in the TLB (which then drops its
   user half): forget cached branch targets only in the range's code pages. */
void hv_ijc_drop_range(HvCpu *cpu, uint64_t va, uint64_t len, int asid) {
    HvMachine *m = cpu->m;
    if (!m) return;
    if (asid < 0) { /* every address space */
        HV_CPU_COUNT(cpu, reset_why[5], 1);
        hv_ijc_reset(cpu, 0);
        return;
    }
    if (asid == current_asid(cpu) || current_asid(cpu) < 0) ret_new(cpu, 0);
    /* no user table filled since they were emptied: no target to drop, and nothing to read (as ijc_drop_va: the
       chunks of a CPU whose caches were given back would come back into memory) */
    if (!cpu->pc_state || !(__atomic_load_n(&cpu->pc_state->ijc_tables_used, __ATOMIC_RELAXED) & 0x5555u)) return;
    uint64_t salt = current_asid(cpu) < 0 ? cpu->ijc_salt[0] : (uint64_t)cpu->asid_cid[asid] << 48;
    uint32_t cid = (uint32_t)(salt >> 48);
    if (!cid) return; /* nothing cached for it */
    uint64_t first = va >> HV_TLB_PAGE_BITS, last = (va + len - 1) >> HV_TLB_PAGE_BITS;
    {   /* nothing cached in the chunks the range covers: nothing to drop */
        uint64_t c0 = va >> 25, c1 = (va + len - 1) >> 25;
        int any = c1 - c0 >= 255;
        for (uint64_t c = c0; !any && c <= c1; c++)
            any = cpu->pc_state->cid_chunks[cid][(c & 255) >> 6] >> (c & 63) & 1;
        if (!any) {
            if (m->stats_interval) HV_CPU_COUNT(cpu, range_spared, 1);
            return;
        }
    }
    if (last - first >= 4096) { /* scanning the whole set costs more than the cold start: drop the address space */
        hv_ijc_drop_asid(cpu, (uint16_t)asid);
        return;
    }
    {
        for (uint64_t p = first; p <= last; p++)
            if (ucode_has(cpu->pc_state, cid, p << HV_TLB_PAGE_BITS)) {
                HV_CPU_COUNT(cpu, reset_why[2], 1);
                ijc_clear_page(cpu, p << HV_TLB_PAGE_BITS, 0, salt, 1);
            }
        return;
    }
    for (uint32_t i = 0; i < 1u << HV_UCODE_BITS; i++) {
        uint64_t k = cpu->pc_state->ucode[i];
        if (!k || (k - 1) >> 40 != cid) continue;
        uint64_t p = (k - 1) & 0xffffffffffull;
        if (p >= (first & 0xffffffffffull) && p <= (last & 0xffffffffffull)) {
            HV_CPU_COUNT(cpu, reset_why[2], 1);
            ijc_clear_page(cpu, p << HV_TLB_PAGE_BITS, 0, salt, 1);
        }
    }
}

void hv_tlb_flush_va(HvCpu *cpu, uint64_t va) {
    hv_tlb_flush_va_asid(cpu, va, -1);
}

/* A writable entry just went into the TLB at code (slot << 2 | table) for RAM
   page pg. Another CPU may have just translated code from the page, setting
   code_pages and then dropping write tags in every TLB: if that came before
   this entry was there, it must not stay writable. */
static void stash_drop(HvPerCpu *p, int asid);

/* Say where, so that the page becoming code drops just this entry (or, if
   this CPU now maps it writable twice, makes the protector scan). The check
   that must follow, after a full fence, is write_guard's last two lines. */
static void write_hint(HvCpu *cpu, HvRam *ram, uint64_t pg, uint32_t code) {
    uint64_t *hint = &ram->whint[pg * ram->whint_cpus + cpu->index];
    uint64_t mine = code + 1, old = __atomic_load_n(hint, __ATOMIC_RELAXED), now = old;
    const uint8_t *host_page = ram->host + (pg << HV_RAM_PAGE_BITS);
    if (mine > 0xffff) { /* the very last code (the 4 KB table's last slot) has no 16-bit name: the protector scans */
        if (!(old & HV_WHINT_MANY)) __atomic_store_n(hint, HV_WHINT_MANY, __ATOMIC_RELEASE);
        return;
    }
    if (!(old & HV_WHINT_MANY)) {
        /* Up to three entries: this one, in a free place or one whose entry maps
           something else by now; beyond that the protector scans. */
        int have = 0, place = -1;
        for (int k = 0; k < 3; k++) {
            uint64_t c = old >> (16 * k) & 0xffff;
            if (c == mine) have = 1;
            else if (place < 0 && (!c || !maps_writable(&cpu->tlb[0] + (c - 1), host_page))) place = k;
        }
        if (!have) now = place >= 0 ? (old & ~(0xffffull << (16 * place))) | mine << (16 * place) : HV_WHINT_MANY;
        /* Release: a protector that reads this hint (its exchange is an acquire) must also see the entry it names,
           written just before. Relaxed, the hint could be seen first, the entry still old, nothing dropped and this
           CPU not waited for, while its own code_pages check below read the page as not yet code. */
        if (now != old) __atomic_store_n(hint, now, __ATOMIC_RELEASE);
        /* Unchanged (an earlier entry here for this page left it: a TLBI or a reset dropped that entry, or the TLB was
           given back and made ready again), nothing is stored, and a protector sees this entry through write_guard's
           fence and its mark check instead (hv_tlb_protect_code_page says how). A stash restore (switch_space) has
           the same pair through its own batched fence and mark checks. */
    }
}

static void write_guard(HvCpu *cpu, HvRam *ram, uint64_t pg, uint32_t code, HvTlbEntry *e) {
    write_hint(cpu, ram, pg, code);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&ram->code_pages[pg], __ATOMIC_RELAXED)) e->write_tag = HV_TLB_INVALID;
    /* Kept in files: stores through this entry are never seen one by one, so the page is dirty from now on. The
       same order as the code check: the writer clears the mark, fences and reads the hints (hv_ram_revoke_writes),
       so either it finds this entry and drops it, or the mark is seen clear here and set again. */
    else hv_ram_mark(ram, pg);
}

/* The writer took the pages' marks (core/ramfile.c): every writable entry to them dropped, each CPU that may have
   one (or a store through one under way) waited for. A CPU's hints name its entries; none (0) means it has made
   no writable entry to the page since the page last became code, unless the page is code now, when a protector
   may have just taken the hint and not yet dropped the entry: then, as for HV_WHINT_MANY, a scan. The hints are
   read, not taken: protect_code_page still needs them to know whom to wait for. Stashed entries need nothing:
   one put back marks its page (the restore below). */
static int in_pages(const uint32_t *pages, uint32_t n, uint64_t pg) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (pages[mid] < pg) lo = mid + 1;
        else hi = mid;
    }
    return lo < n && pages[lo] == pg;
}

void hv_ram_revoke_writes(HvMachine *m, HvRam *ram, const uint32_t *pages, uint32_t n) {
    if (!n) return;
    uint64_t seq[HV_MAX_CPUS];
    uint32_t wait = 0;
    for (int i = 0; i < m->ncpus && i < HV_MAX_CPUS; i++) {
        HvCpu *c = m->cpus[i];
        if (!c || !hv_tlb_is_ready(c)) continue; /* not ready: no entry, and nothing in a block (it runs only once ready) */
        int scan = 0, any = 0;
        uint32_t ci = __atomic_load_n(&c->index, __ATOMIC_RELAXED); /* c may be being reset: hv_cpu_reset_index */
        for (uint32_t k = 0; k < n; k++) {
            uint64_t pg = pages[k];
            uint64_t h = __atomic_load_n(&ram->whint[pg * ram->whint_cpus + ci], __ATOMIC_ACQUIRE);
            if (!h) {
                if (__atomic_load_n(&ram->code_pages[pg], __ATOMIC_ACQUIRE)) scan = any = 1;
                continue;
            }
            any = 1;
            if (h & HV_WHINT_MANY) {
                scan = 1;
                continue;
            }
            const uint8_t *host_page = ram->host + (pg << HV_RAM_PAGE_BITS);
            for (int s = 0; s < 3; s++) {
                uint64_t code = h >> (16 * s) & 0xffff;
                if (!code) continue;
                HvTlbEntry *e = &c->tlb[0] + (code - 1);
                if (maps_writable(e, host_page)) e->write_tag = HV_TLB_INVALID;
            }
        }
        if (scan) {
            uint64_t lo = pages[0], hi = (uint64_t)pages[n - 1] + 1;
            for (int t = 0; t < HV_TLB_TABLES; t++) /* the 4 KB table too */
                for (uint32_t j = 0; j < HV_TLB_SIZE; j++) {
                    HvTlbEntry *e = hv_tlb_ent(c, j, t);
                    uint64_t wt = __atomic_load_n(&e->write_tag, __ATOMIC_RELAXED);
                    if (wt == HV_TLB_INVALID) continue;
                    const uint8_t *host = (const uint8_t *)(uintptr_t)(wt + HV_TLB_ADJ + e->addend);
                    if (host < ram->host || host >= ram->host + ram->size) continue;
                    uint64_t pg = (uint64_t)(host - ram->host) >> HV_RAM_PAGE_BITS;
                    if (pg >= lo && pg < hi && in_pages(pages, n, pg)) e->write_tag = HV_TLB_INVALID;
                }
        }
        /* As protect_code_page: the CPU may be between its check of an entry and the store; once it has left its
           block (it does at the kick), the store is in the memory. Not this thread's own CPU: it is here, in C. */
        if (any && c != hv_self && c->pc_state) {
            seq[i] = hv_cpu_post(c, 0);
            wait |= 1u << i;
        }
    }
    for (int i = 0; i < m->ncpus && i < HV_MAX_CPUS; i++)
        if (wait >> i & 1) hv_cpu_sync(hv_self, m->cpus[i], seq[i]);
}

/* HV_KVA_HIST (measuring, for a kernel fastmem window): which kernel VAs get TLB fills, per GB from the bottom of xnu's
   kernel VA space, and how many distinct software pages. hv_kva_hist_dump writes it out. */
#define KVA_GBS 2048 /* buckets: 2048 of 1 GB from 0xfffffe0000000000 by default; HV_KVA_BASE (hex) and HV_KVA_SHIFT move and widen them */
static uint64_t KVA_BASE = 0xfffffe0000000000ull;
static int kva_shift = 30;
static uint64_t kva_fills[KVA_GBS], kva_pages_n[KVA_GBS], kva_below, kva_above;
static uint8_t *kva_seen; /* one bit per page */
static int kva_on;
/* At machine setup (hv_machine_init), not on the fill: tlb_fill runs from the refill and ijc thunks, which save no
   SIMD register, so it calls nothing outside this file (tools/check_mmu_calls.py). Before any CPU runs. */
void hv_kva_init(void) {
    if (kva_seen) return; /* the first machine's settings, for the whole process (the tests make several) */
    if (!getenv("HV_KVA_HIST")) return;
    if (getenv("HV_KVA_BASE")) KVA_BASE = strtoull(getenv("HV_KVA_BASE"), NULL, 16);
    if (getenv("HV_KVA_SHIFT")) kva_shift = atoi(getenv("HV_KVA_SHIFT"));
    if (kva_shift < HV_TLB_PAGE_BITS + 3 || kva_shift > 40) kva_shift = 30;
    kva_seen = calloc((size_t)KVA_GBS << (kva_shift - HV_TLB_PAGE_BITS - 3), 1);
    kva_on = kva_seen != NULL;
}
static void kva_note(uint64_t va) {
    if (!kva_on) return;
    if (va < KVA_BASE) { __atomic_add_fetch(&kva_below, 1, __ATOMIC_RELAXED); return; }
    uint64_t off = va - KVA_BASE, gb = off >> kva_shift;
    if (gb >= KVA_GBS) { __atomic_add_fetch(&kva_above, 1, __ATOMIC_RELAXED); return; }
    __atomic_add_fetch(&kva_fills[gb], 1, __ATOMIC_RELAXED);
    uint64_t pg = off >> HV_TLB_PAGE_BITS;
    uint8_t bit = (uint8_t)(1u << (pg & 7));
    if (!(__atomic_fetch_or(&kva_seen[pg >> 3], bit, __ATOMIC_RELAXED) & bit)) __atomic_add_fetch(&kva_pages_n[gb], 1, __ATOMIC_RELAXED);
}

void hv_kva_hist_dump(FILE *f) {
    if (kva_on <= 0) return;
    fprintf(f, "kva below %llu above %llu (outside %d buckets of 2^%d bytes from %llx)\n", (unsigned long long)kva_below, (unsigned long long)kva_above, KVA_GBS, kva_shift, (unsigned long long)KVA_BASE);
    for (int g = 0; g < KVA_GBS; g++)
        if (kva_fills[g]) fprintf(f, "kva gb %d fills %llu pages %llu\n", g, (unsigned long long)kva_fills[g], (unsigned long long)kva_pages_n[g]);
}

/* One entry, of table `table` whose pages are 1 << bits bytes. Code pages, auto-save guards and write hints are
   per RAM page (pg, HV_RAM_PAGE_BITS): a smaller entry's are those of the RAM page it is in. */
static void fill_entry(HvCpu *cpu, uint64_t va, const HvWalk *w, int table, unsigned bits) {
    uint64_t mask = (1ull << bits) - 1;
    HvRam *ram = ram_of(cpu->m, w->pa);
    if (!ram) return;
    uint64_t page_pa = w->pa & ~mask;
    if (page_pa < ram->pa || page_pa + mask + 1 - ram->pa > ram->size) return;
    uint64_t pg = (page_pa - ram->pa) >> HV_RAM_PAGE_BITS;
    uint64_t page_va = va & ~mask;
    uint32_t slot = (uint32_t)(va >> bits) & (HV_TLB_SIZE - 1);
    /* Only into a ready TLB, whose other entries read invalid: the C callers (hv_mmu_access, hv_mmu_fetch) ready it
       first, and the thunk paths refuse an unready one (they can't make it: a 2 MB memset, with SIMD) */
    HvTlbEntry *e = hv_tlb_ent(cpu, slot, table);
    if (table == HV_TLB_SMALL_TABLE && cpu->m->stats_interval) HV_CPU_COUNT(cpu, small_fills, 1);
    if (!((page_va >> 55) & 1)) {
        if (cpu->nuser_fills < HV_USER_FILLS) cpu->user_fills[cpu->nuser_fills] = (uint16_t)hv_tlb_code(slot, table);
        if (cpu->nuser_fills <= HV_USER_FILLS) cpu->nuser_fills++;
    } else {
        if (cpu->nkernel_fills < HV_KERNEL_FILLS) cpu->kernel_fills[cpu->nkernel_fills] = (uint16_t)hv_tlb_code(slot, table);
        if (cpu->nkernel_fills <= HV_KERNEL_FILLS) cpu->nkernel_fills++;
    }
    e->addend = (int64_t)((uint64_t)(uintptr_t)(ram->host + (page_pa - ram->pa)) - page_va);
    e->read_tag = w->r ? page_va - HV_TLB_ADJ : HV_TLB_INVALID;
    /* Writes to ROM or to pages we translated from take the slow path. */
    /* ...and so do pages an auto-save is keeping as they were (core/saveguard.h): the slow path claims them first. */
    e->write_tag = w->w && !ram->readonly && !ram->code_pages[pg] && !__atomic_load_n(&ram->save_guard[pg], __ATOMIC_ACQUIRE)
                       ? page_va - HV_TLB_ADJ : HV_TLB_INVALID;
    /* A watched page takes the slow path for every write. */
    if (cpu->m->watch_va && (cpu->m->watch_va & ~PAGE_MASK & 0x00ffffffffffffffull) == (page_va & ~PAGE_MASK & 0x00ffffffffffffffull))
        e->write_tag = HV_TLB_INVALID;
    if (cpu->m->watch_pa && (cpu->m->watch_pa & ~PAGE_MASK) == (page_pa & ~PAGE_MASK)) e->write_tag = HV_TLB_INVALID;
    e->exec_tag = w->x ? hv_exec_key(page_va, e->addend) : HV_TLB_INVALID;
    if (e->write_tag != HV_TLB_INVALID) write_guard(cpu, ram, pg, hv_tlb_code(slot, table), e);
}

/* The software-page entry for va already maps what walk w found, with no right w lacks and none it grants missing. */
static int filled_already(HvCpu *cpu, uint64_t va, int el, const HvWalk *w) {
    const HvTlbEntry *e = hv_tlb_at(cpu, va, hv_tlb_table(cpu, el), HV_TLB_PAGE_BITS);
    uint64_t page_va = va & ~(uint64_t)PAGE_MASK, page_pa = w->pa & ~(uint64_t)PAGE_MASK;
    HvRam *ram = ram_of(cpu->m, w->pa);
    if (!ram || !w->r || e->read_tag != page_va - HV_TLB_ADJ ||
        page_va + (uint64_t)e->addend != (uint64_t)(uintptr_t)(ram->host + (page_pa - ram->pa)))
        return 0;
    return (e->write_tag != HV_TLB_INVALID) == !!w->w && (e->exec_tag != HV_TLB_INVALID) == !!w->x;
}

static void tlb_fill(HvCpu *cpu, uint64_t va, int el, const HvWalk *w) {
    if ((va >> 55) & 1) kva_note(va & ~(uint64_t)PAGE_MASK);
    if (w->join && cpu->m && cpu->m->stats_interval) HV_CPU_COUNT(cpu, small_join[w->join - 1], 1);
    unsigned bits;
    int table = hv_tlb_where(cpu, va, el, &bits);
    if (table == HV_TLB_SMALL_TABLE) fill_entry(cpu, va, w, table, bits); /* a 4 KB-page process: its own table */
    if (w->block_size < (1ull << HV_TLB_PAGE_BITS)) return; /* smaller guest pages: not in the software-page tables */
    /* A joined run's software-page entry (for the kernel's LDTR/STTR) is made once, not again at each of its 4 KB
       pages' misses in the 4 KB table: each time would take another of the few places in user_fills (past them,
       no stash is kept at a switch and a flush scans every table). A page written but now code isn't writable in
       the entry; that is filled again, as before, and finds it so. */
    if (table == HV_TLB_SMALL_TABLE && filled_already(cpu, va, el, w)) return;
    fill_entry(cpu, va, w, hv_tlb_table(cpu, el), HV_TLB_PAGE_BITS);
}

/* Walks that may fill the TLB are bracketed by the CPU's walk_seq (odd while
   one runs), so another CPU's TLBI can drop entries here itself and know no
   fill from an old page table raced it (hv_tlbi_drop_remote_va). */
static void walk_begin(HvCpu *cpu) {
    if (cpu->pc_state) __atomic_add_fetch(&cpu->pc_state->walk_seq, 1, __ATOMIC_SEQ_CST);
}

static void walk_end(HvCpu *cpu) {
    if (cpu->pc_state) __atomic_add_fetch(&cpu->pc_state->walk_seq, 1, __ATOMIC_RELEASE);
}

static HvRam *ram_of_host(HvMachine *m, const uint8_t *host);

uint32_t hv_mmu_access(HvCpu *cpu, uint64_t va, HvAccess acc, int el, uint64_t *pa) {
    /* The TLB entry first, as translated code trusts it: slow paths that are slow for another reason (a page's
       last bytes, the access kind) need no walk (macOS 27: 100-250K a second). */
    if (cpu->m) {
        unsigned bits;
        int table = hv_tlb_where(cpu, va, el, &bits);
        const HvTlbEntry *e = hv_tlb_at(cpu, va, table, bits);
        if (acc == HV_ACC_EXEC) {
            uint64_t page_va = va & ~((1ull << bits) - 1);
            if (e->exec_tag != HV_TLB_INVALID && e->exec_tag == hv_exec_key(page_va, e->addend)) {
                const uint8_t *host = (const uint8_t *)(uintptr_t)(va + (uint64_t)e->addend);
                HvRam *ram = ram_of_host(cpu->m, host);
                if (ram) {
                    *pa = ram->pa + (uint64_t)(host - ram->host);
                    return 0;
                }
            }
        } else {
            uint64_t tag = acc == HV_ACC_WRITE ? e->write_tag : e->read_tag;
            if (tag != HV_TLB_INVALID && tag + HV_TLB_ADJ == (va & ~((1ull << bits) - 1))) {
                const uint8_t *host = (const uint8_t *)(uintptr_t)(va + (uint64_t)e->addend);
                HvRam *ram = ram_of_host(cpu->m, host);
                if (ram) {
                    *pa = ram->pa + (uint64_t)(host - ram->host);
                    return 0;
                }
            }
        }
    }
    HvWalk w;
    walk_begin(cpu);
    hv_mmu_walk(cpu, va, acc, el, &w);
    if (!w.fsc && !hv_tlb_is_ready(cpu)) hv_tlb_ready(cpu); /* a C lookup before the CPU's first run */
    if (!w.fsc) tlb_fill(cpu, va, el, &w);
    walk_end(cpu);
    if (w.fsc) return w.fsc;
    *pa = w.pa;
    return 0;
}

static int drop_remote_va(HvCpu *target, uint64_t va, int asid);

/* Inside target's ijc_remote, so a give-back of its branch caches waits for this or this sees it (ijc_seq odd). */
int hv_tlbi_drop_remote_va(HvCpu *target, uint64_t va, int asid) {
    HvPerCpu *p = target->pc_state;
    __atomic_add_fetch(&p->ijc_remote, 1, __ATOMIC_SEQ_CST);
    int r = drop_remote_va(target, va, asid);
    __atomic_sub_fetch(&p->ijc_remote, 1, __ATOMIC_RELEASE);
    return r;
}

static int drop_remote_va(HvCpu *target, uint64_t va, int asid) {
    HvPerCpu *p = target->pc_state;
    uint32_t s1 = __atomic_load_n(&p->walk_seq, __ATOMIC_SEQ_CST);
    if (s1 & 1) { if (target->m->stats_interval) HV_STAT_ADD(target->m, race_kind[0], 1); return 0; } /* mid-walk: it may be about to fill from the old table */
    uint32_t c1 = __atomic_load_n(&p->ijc_seq, __ATOMIC_SEQ_CST);
    if (c1 & 1) { if (target->m->stats_interval) HV_STAT_ADD(target->m, race_kind[1], 1); return 0; } /* caching a block it may have looked up through the old mapping */
    if (!((va >> 55) & 1)) stash_drop_va(p, va, asid);
    uint32_t idx = (va >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1);
    int fetched = 0; /* the target may be running code from the page: it must leave and look again */
    /* A TLB not ready holds nothing (it is made all invalid before the CPU runs): left untouched */
    for (int t = 0; t < 3 && hv_tlb_is_ready(target); t++) { /* the slot whatever its tag byte, as hv_tlb_flush_va_asid does */
        HvTlbEntry *e = hv_tlb_ent(target, idx, t);
        uint64_t x = __atomic_load_n(&e->exec_tag, __ATOMIC_RELAXED);
        if (x != HV_TLB_INVALID && ((hv_exec_key(x, e->addend) ^ va) & 0x00ffffffffffffffull & ~PAGE_MASK) == 0) fetched = 1;
        __atomic_store_n(&e->read_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
        __atomic_store_n(&e->write_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
        __atomic_store_n(&e->exec_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
    }
    if (hv_tlb_is_ready(target) && drop_small(target, va)) fetched = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    /* Its cached branch targets in the page too: a cache fill checks the TLB
       entry first (hv_ijc_fill_checked), so one after this sees it gone, and one
       during it changed ijc_seq. */
    if (!ijc_drop_va(target, va, asid, 1)) return -1; /* only target can do it: no use trying again */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int ok = __atomic_load_n(&p->walk_seq, __ATOMIC_SEQ_CST) == s1 && /* no fill in between */
             __atomic_load_n(&p->ijc_seq, __ATOMIC_SEQ_CST) == c1;
    if (!ok) {
        if (target->m->stats_interval) HV_STAT_ADD(target->m, race_kind[2], 1);
    } else if (fetched) {
        hv_cpu_kick(target);
    }
    return ok;
}

/* hv_tlbi_drop_remote_va for npages pages from va at once (big range TLBIs, HV_RANGE_DROP): one pass over the
   stashes instead of one per page, then each page's TLB slots and cached branch targets, all inside one
   walk_seq/ijc_seq window. Same answers: 1 done, 0 raced (try again), -1 only the target can do it. */
static void stash_drop_range(HvPerCpu *p, uint64_t va, uint64_t npages, int asid);

static int drop_remote_range(HvCpu *target, uint64_t va, uint64_t npages, int asid);

int hv_tlbi_drop_remote_range(HvCpu *target, uint64_t va, uint64_t npages, int asid) {
    HvPerCpu *p = target->pc_state;
    __atomic_add_fetch(&p->ijc_remote, 1, __ATOMIC_SEQ_CST); /* as hv_tlbi_drop_remote_va */
    int r = drop_remote_range(target, va, npages, asid);
    __atomic_sub_fetch(&p->ijc_remote, 1, __ATOMIC_RELEASE);
    return r;
}

static int drop_remote_range(HvCpu *target, uint64_t va, uint64_t npages, int asid) {
    HvPerCpu *p = target->pc_state;
    uint32_t s1 = __atomic_load_n(&p->walk_seq, __ATOMIC_SEQ_CST);
    if (s1 & 1) { if (target->m->stats_interval) HV_STAT_ADD(target->m, race_kind[0], 1); return 0; }
    uint32_t c1 = __atomic_load_n(&p->ijc_seq, __ATOMIC_SEQ_CST);
    if (c1 & 1) { if (target->m->stats_interval) HV_STAT_ADD(target->m, race_kind[1], 1); return 0; }
    if (!((va >> 55) & 1)) stash_drop_range(p, va, npages, asid);
    int fetched = 0, ready = hv_tlb_is_ready(target); /* not ready: nothing in it, left untouched */
    for (uint64_t i = 0; ready && i < npages && i < HV_TLB_SIZE; i++) { /* past HV_TLB_SIZE pages the slots come round again */
        uint32_t idx = (uint32_t)((va >> HV_TLB_PAGE_BITS) + i) & (HV_TLB_SIZE - 1);
        for (int t = 0; t < 3; t++) {
            HvTlbEntry *e = hv_tlb_ent(target, idx, t);
            uint64_t x = __atomic_load_n(&e->exec_tag, __ATOMIC_RELAXED);
            /* the slot whatever its tag, as for one page; fetched if it holds code from the range */
            if (x != HV_TLB_INVALID) {
                uint64_t off = ((hv_exec_key(x, e->addend) & 0x00ffffffffffc000ull) - (va & 0x00ffffffffffc000ull)) & 0x00ffffffffffffffull;
                if (off < (npages << HV_TLB_PAGE_BITS)) fetched = 1;
            }
            __atomic_store_n(&e->read_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
            __atomic_store_n(&e->write_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
            __atomic_store_n(&e->exec_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
        }
    }
    /* The 4 KB table: its slots come round after HV_TLB_SIZE 4 KB pages. One a page covers is that page's; past that
       every slot is gone through, so one may be another page's: fetched from the range or not, by its own address. */
    for (uint64_t i = 0; ready && HV_TLB_SMALL && !((va >> 55) & 1) && i < npages && (i << (HV_TLB_PAGE_BITS - HV_TLB_SMALL_BITS)) < HV_TLB_SIZE; i++) {
        uint32_t first = (uint32_t)((((va & ~PAGE_MASK) >> HV_TLB_SMALL_BITS)) + (i << (HV_TLB_PAGE_BITS - HV_TLB_SMALL_BITS)));
        for (uint32_t j = 0; j < 1u << (HV_TLB_PAGE_BITS - HV_TLB_SMALL_BITS); j++) {
            HvTlbEntry *e = hv_tlb_ent(target, (first + j) & (HV_TLB_SIZE - 1), HV_TLB_SMALL_TABLE);
            uint64_t x = __atomic_load_n(&e->exec_tag, __ATOMIC_RELAXED);
            if (x != HV_TLB_INVALID) {
                uint64_t off = ((hv_exec_key(x, e->addend) & 0x00ffffffffffffffull & ~PAGE_MASK) - (va & 0x00ffffffffffffffull & ~PAGE_MASK)) & 0x00ffffffffffffffull;
                if (off < (npages << HV_TLB_PAGE_BITS)) fetched = 1;
            }
            __atomic_store_n(&e->read_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
            __atomic_store_n(&e->write_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
            __atomic_store_n(&e->exec_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
        }
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (uint64_t i = 0; i < npages; i++)
        if (!ijc_drop_va(target, va + (i << HV_TLB_PAGE_BITS), asid, 1)) return -1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int ok = __atomic_load_n(&p->walk_seq, __ATOMIC_SEQ_CST) == s1 && __atomic_load_n(&p->ijc_seq, __ATOMIC_SEQ_CST) == c1;
    if (!ok) {
        if (target->m->stats_interval) HV_STAT_ADD(target->m, race_kind[2], 1);
    } else if (fetched) {
        hv_cpu_kick(target);
    }
    return ok;
}

const uint32_t *hv_mmu_fetch(HvCpu *cpu, uint64_t va, uint64_t *pa, uint32_t *avail) {
    if (va & 3) {
        hv_raise_sync(cpu, HV_EC_PC_ALIGN, 0, va, va);
        return NULL;
    }
    unsigned bits;
    int table = hv_tlb_where(cpu, va, cpu->el, &bits);
    HvTlbEntry *t = hv_tlb_at(cpu, va, table, bits);
    if (t->exec_tag == hv_exec_key(va & ~((1ull << bits) - 1), t->addend)) {
        uint8_t *host = (uint8_t *)(uintptr_t)(va + t->addend);
        HvRam *main_r = cpu->m->main_ram;
        if (__builtin_expect(main_r && (uint64_t)(host - main_r->host) < main_r->size, 1)) {
            *pa = main_r->pa + (uint64_t)(host - main_r->host);
            uint64_t page = 1ull << hv_code_page_bits(cpu, va);
            *avail = (uint32_t)((page - (va & (page - 1))) / 4);
            return (const uint32_t *)host;
        }
        for (int i = 0; i < cpu->m->nram; i++) {
            HvRam *r = &cpu->m->ram[i];
            if ((uint64_t)(host - r->host) < r->size) {
                *pa = r->pa + (uint64_t)(host - r->host);
                uint64_t page = 1ull << hv_code_page_bits(cpu, va); /* a joined 4 KB page's neighbours may be remapped alone */
                *avail = (uint32_t)((page - (va & (page - 1))) / 4);
                return (const uint32_t *)host;
            }
        }
    }
    HvWalk w;
    HV_CPU_COUNT(cpu, fetch_walks, 1);
    walk_begin(cpu);
    hv_mmu_walk(cpu, va, HV_ACC_EXEC, cpu->el, &w);
    if (!w.fsc && !hv_tlb_is_ready(cpu)) hv_tlb_ready(cpu); /* as hv_mmu_access */
    if (!w.fsc) tlb_fill(cpu, va, cpu->el, &w);
    walk_end(cpu);
    if (w.fsc) {
        hv_raise_abort(cpu, 1, va, w.fsc, 0, w.s1ptw, va);
        return NULL;
    }
    uint64_t chunk = w.block_size < (1ull << HV_TLB_PAGE_BITS) ? w.block_size : 1ull << HV_TLB_PAGE_BITS;
    if (chunk > 1ull << hv_code_page_bits(cpu, va)) chunk = 1ull << hv_code_page_bits(cpu, va);
    uint64_t left = chunk - (va & (chunk - 1));
    const uint32_t *p = hv_phys_ptr_r(cpu->m, w.pa, 4);
    if (!p) {
        hv_raise_abort(cpu, 1, va, HV_FSC_EXTERNAL, 0, 0, va);
        return NULL;
    }
    HvRam *ram = hv_ram_lookup(cpu->m, w.pa);
    uint64_t ram_left = ram->size - (w.pa - ram->pa);
    if (left > ram_left) left = ram_left;
    *pa = w.pa;
    *avail = (uint32_t)(left / 4);
    return p;
}

/* A plain load or store of general registers that the fast check refused only because it is in a page's last
   HV_TLB_ADJ bytes: done here, through the TLB entry (tag and addend as the caller read them), the same access
   translated code would have made. The guest's registers are all in the HvCpu (hv_helper_thunk puts x19..x29 there
   too, and reloads every one this may write). Returns 0 done, 1 for the helper (any other form: writeback, vector,
   ordered, literal, pointer-authenticated). No SIMD here, and no memcpy (the library's uses SIMD). */
static int page_end_access(HvCpu *cpu, uint64_t va, uint32_t bytes, int write, int64_t addend, uint32_t insn) {
    uint32_t size = insn >> 30, opc = (insn >> 22) & 3, rt = insn & 31, rt2 = (insn >> 10) & 31;
    int pair = 0, sign = 0, load;
    if ((insn & 0x3b000000u) == 0x39000000u || /* LDR/STR (unsigned offset) */
        ((insn & 0x3b200000u) == 0x38000000u && !(insn & (1u << 10))) || /* LDUR/STUR, LDTR/STTR */
        (insn & 0x3b200c00u) == 0x38200800u) { /* LDR/STR (register offset) */
        if (insn & (1u << 26)) return 1;       /* SIMD&FP */
        if (opc == 0) load = 0;
        else if (opc == 1) load = 1;
        else if (opc == 2 && size < 3) load = 1, sign = 64;
        else if (opc == 3 && size < 2) load = 1, sign = 32;
        else return 1;                         /* PRFM, and unallocated */
    } else if ((insn & 0x3e000000u) == 0x28000000u && ((insn >> 23) & 3) != 1 && ((insn >> 23) & 3) != 3) {
        /* LDP/STP (signed offset), LDNP/STNP, LDPSW: general registers, no writeback */
        uint32_t o = insn >> 30;
        if (insn & (1u << 26)) return 1;
        load = (insn >> 22) & 1;
        if (o == 0) size = 2;
        else if (o == 2) size = 3;
        else if (o == 1 && load && ((insn >> 23) & 3) == 2) size = 2, sign = 64; /* LDPSW */
        else return 1;                         /* STGP, LDNP with opc 01, unallocated */
        pair = 1;
    } else {
        return 1;
    }
    uint32_t esize = 1u << size;
    if (bytes != (pair ? 2 * esize : esize) || write == load) return 1; /* not the access the site said */
    uint8_t *p = (uint8_t *)(uintptr_t)(va + (uint64_t)addend);
    if (pair && size == 2) { /* a 32-bit pair is one 8-byte access, as the CPU makes it (atomic when aligned) */
        if (!load) {
            uint64_t v = (rt == 31 ? 0 : (uint32_t)cpu->x[rt]) | (rt2 == 31 ? 0 : cpu->x[rt2]) << 32;
            __builtin_memcpy(p, &v, 8);
        } else {
            uint64_t v;
            __builtin_memcpy(&v, p, 8);
            uint64_t lo = sign ? (uint64_t)(int64_t)(int32_t)v : (uint32_t)v;
            uint64_t hi = sign ? (uint64_t)(int64_t)(int32_t)(v >> 32) : v >> 32;
            if (rt2 != 31) cpu->x[rt2] = hi;
            if (rt != 31) cpu->x[rt] = lo; /* rt last: when rt == rt2 (unpredictable) it gets the first word */
        }
        pair = -1;
    }
    for (int i = 0; i <= pair; i++, p += esize) {
        uint32_t r = i ? rt2 : rt;
        if (!load) {
            uint64_t v = r == 31 ? 0 : cpu->x[r];
            switch (size) {
            case 0: { uint8_t t = (uint8_t)v; __builtin_memcpy(p, &t, 1); break; }
            case 1: { uint16_t t = (uint16_t)v; __builtin_memcpy(p, &t, 2); break; }
            case 2: { uint32_t t = (uint32_t)v; __builtin_memcpy(p, &t, 4); break; }
            default: __builtin_memcpy(p, &v, 8); break;
            }
            continue;
        }
        uint64_t v;
        switch (size) {
        case 0: { uint8_t t; __builtin_memcpy(&t, p, 1); v = sign ? (uint64_t)(int64_t)(int8_t)t : t; break; }
        case 1: { uint16_t t; __builtin_memcpy(&t, p, 2); v = sign ? (uint64_t)(int64_t)(int16_t)t : t; break; }
        case 2: { uint32_t t; __builtin_memcpy(&t, p, 4); v = sign ? (uint64_t)(int64_t)(int32_t)t : t; break; }
        default: __builtin_memcpy(&v, p, 8); break;
        }
        if (sign == 32) v = (uint32_t)v;
        if (r != 31) cpu->x[r] = v;
    }
    if (cpu->m->stats_interval) HV_CPU_COUNT(cpu, page_end_light, 1);
    return 0;
}

/* A TLB miss in translated code, before the full helper: walk and fill, and
   say whether the fast path will now succeed (0), the helper must do the
   access (1: a fault, device memory, a write to a code page, an access that
   crosses into the next page or must be aligned and is not), or it was done
   here (2: a plain one in a page's last bytes, page_end_access). Called by
   hv_helper_thunk with the guest's SIMD registers live, so nothing here may
   use them (see read_desc). flags (HV_REFILL_*): bit 0 write, bits 2..1 the TLB
   table (0 EL0 or unprivileged, 1 EL1, 2 EL1 with PAN), bits 8..3 bytes - 1,
   bit 10 must be aligned (whose fast check is exact). */
int hv_tlb_refill(HvCpu *cpu, uint64_t va, uint32_t flags) {
    /* A TLB not ready (all zeros, never the case once the CPU runs: unpark readies it) is the helper's to ready */
    if (!hv_tlb_is_ready(cpu)) return 1;
    int write = flags & 1, table = (flags >> 1) & 3;
    uint32_t bytes = ((flags >> 3) & 63) + 1;
    int el = table == 1 || table == 2 ? 1 : 0;
    unsigned bits = table == HV_TLB_SMALL_TABLE ? HV_TLB_SMALL_BITS : HV_TLB_PAGE_BITS;
    uint64_t pmask = (1ull << bits) - 1;
    uint64_t off = va & pmask, page_va = va & ~pmask;
    if (off + bytes > pmask + 1) return 1; /* crosses into the next page: the helper */
    /* Misaligned where it must not be: the helper raises the alignment fault (a refill would send it round again). */
    if ((flags & HV_REFILL_ALIGN) && (va & (bytes - 1))) return 1;
    /* DC ZVA (the only aligned 64-byte write) at EL0 with SCTLR_EL1.DZE clear: the helper's UNDEF, not a refill. */
    if (el == 0 && write && bytes == 64 && (flags & HV_REFILL_ALIGN) && !((cpu->sctlr_el1 >> 14) & 1)) return 1;
    /* In the page, but where the fast check cannot pass (it allows HV_TLB_ADJ bytes of every access, so a small one in
       a page's last HV_TLB_ADJ bytes misses): a refill would come straight back here, so do the access itself. */
    int page_end = !(flags & HV_REFILL_ALIGN) && bytes <= HV_TLB_ADJ + 1 && off + HV_TLB_ADJ > pmask;
    /* Only a 4 KB-page process's own blocks use the 4 KB table; if its pages are not 4 KB now (a TCR write without a
       TLBI yet), the fill below cannot satisfy them: the helper. */
    if (table == HV_TLB_SMALL_TABLE && hv_code_page_bits(cpu, va) == HV_TLB_PAGE_BITS) return 1;
    HvTlbEntry *e = hv_tlb_at(cpu, va, table, bits);
    uint64_t tag = write ? e->write_tag : e->read_tag;
    if (page_end && tag != HV_TLB_INVALID && tag + HV_TLB_ADJ == page_va) /* the entry is there: no walk */
        return page_end_access(cpu, va, bytes, write, e->addend, ((const HvHelperInfo *)cpu->spill[3])->insn) ? 1 : 2;
    HvWalk w;
    walk_begin(cpu);
    hv_mmu_walk(cpu, va, write ? HV_ACC_WRITE : HV_ACC_READ, el, &w);
    if (!w.fsc) tlb_fill(cpu, va, el, &w);
    walk_end(cpu);
    if (w.fsc) return 1;
    tag = write ? e->write_tag : e->read_tag;
    HV_CPU_COUNT(cpu, refills, 1); /* always: the freeze records' refills */
    if (tag == HV_TLB_INVALID || tag + HV_TLB_ADJ != page_va) return 1;
    if (page_end)
        return page_end_access(cpu, va, bytes, write, e->addend, ((const HvHelperInfo *)cpu->spill[3])->insn) ? 1 : 2;
    return 0;
}

/* Put block b in this CPU's indirect-branch cache under its VA (b->va must be in
   the half its flags say: kernel blocks, kernel VAs). */
void hv_ijc_fill(HvCpu *cpu, HvBlock *b) {
    HvPerCpu *pcs = cpu->pc_state;
    int kernel = b->flags & HV_BLOCK_EL1;
    uint64_t *t = pcs->ijc + (size_t)hv_ijc_table(b->flags) * (HV_IJC_TABLE_BYTES / 8);
    uint64_t tag = b->va ^ cpu->ijc_salt[kernel];
    uint64_t *ent = t + (size_t)hv_ijc_index(tag) * 2;
    if (ent[0] != tag && ent[0]) ent[2] = ent[0], ent[3] = ent[1]; /* way 0's goes to way 1 (over this one's, if there) */
    ent[0] = tag;
    ent[1] = (uint64_t)(uintptr_t)b->code;
    uint32_t tb = 1u << hv_ijc_table(b->flags);
    if (!(pcs->ijc_tables_used & tb)) { /* its first fill since the table was cleared or given back */
        pcs->ijc_tables_used |= tb;
        __atomic_or_fetch(&pcs->ijc_tables_held, tb, __ATOMIC_RELAXED);
    }
    {   /* the page's hint: which piece has a target now */
        uint64_t key = ((b->va & ~(uint64_t)PAGE_MASK) ^ cpu->ijc_salt[kernel]) | 1;
        uint64_t bit = 1ull << ((b->va & PAGE_MASK) >> 8);
        __typeof__(pcs->ijc_hint[0][0]) *h = &pcs->ijc_hint[kernel][hint_slot(key)];
        if (h->key == key) h->bits |= bit;
        else if (!h->key) h->bits = bit, __atomic_store_n(&h->key, key, __ATOMIC_RELEASE), pcs->ijc_hint_used[kernel] |= HV_PART(hint_slot(key), 12);
        else if (h->key != ~0ull) h->bits = ~0ull, __atomic_store_n(&h->key, ~0ull, __ATOMIC_RELEASE);
    }
    /* The filters below are per page: the next fill is mostly in the same one. */
    uint64_t pkey = (b->va & ~(uint64_t)PAGE_MASK) ^ cpu->ijc_salt[kernel] ^ 1;
    if (pcs->last_fill[kernel] == pkey) return;
    if (!kernel) {
        hv_ucode_add(cpu, b->va);
        uint32_t h = (uint32_t)(b->va >> HV_TLB_PAGE_BITS) & 0xffff;
        pcs->ijc_pages[0][h >> 6] |= 1ull << (h & 63);
        pcs->ijc_pages_used |= HV_PART(h >> 6, 10);
        uint32_t ch = (uint32_t)(b->va >> 25) & 255, cid = (uint32_t)(cpu->ijc_salt[0] >> 48); /* the id after any reset in ucode_add */
        pcs->cid_chunks[cid][ch >> 6] |= 1ull << (ch & 63);
    }
    uint32_t k = hv_ijc_key(b->va, cpu->ijc_salt[kernel]);
    if (!(pcs->ijc_keyed[k >> 6] >> (k & 63) & 1)) {
        pcs->ijc_keyed[k >> 6] |= 1ull << (k & 63);
        pcs->ijc_keyed_used |= HV_PART(k >> 6, 14);
        if (++pcs->ijc_keyed_count > (1u << 17)) { /* an eighth full: start the cache and the filter over */
            HV_CPU_COUNT(cpu, reset_why[6], 1);
            hv_ijc_reset(cpu, 0);
            hv_ijc_reset(cpu, 1);
            keyed_clear(pcs);
        }
    }
    pcs->last_fill[kernel] = (b->va & ~(uint64_t)PAGE_MASK) ^ cpu->ijc_salt[kernel] ^ 1; /* the salt after any reset above */
}

/* An indirect-branch cache miss in translated code, before the dispatcher: if
   the target's block already exists (found through this CPU's TLB and the
   block hash), cache it and return its code for the thunk to jump to; else 0
   and the dispatcher does it. Called by hv_ijc_thunk with the guest's SIMD
   registers live (see read_desc), so nothing that could reach libc runs: a
   fill that would reset the cache's filters goes to the dispatcher instead.
   flags: the block flags the branch was translated under (the target's too). */
/* Does this CPU's TLB (the table for flags) map b's page where b came from? */
static int tlb_maps_block(HvCpu *cpu, const HvBlock *b) {
    unsigned bits;
    int table = hv_tlb_block_table(b->flags, &bits);
    const HvTlbEntry *e = hv_tlb_at(cpu, b->va, table, bits);
    uint64_t page_va = b->va & ~((1ull << bits) - 1), page_pa = b->pa & ~((1ull << bits) - 1);
    return __atomic_load_n(&e->exec_tag, __ATOMIC_RELAXED) == hv_exec_key(page_va, e->addend) &&
           (uint64_t)e->addend == (uint64_t)(uintptr_t)(b->ram->host + (page_pa - b->ram->pa)) - page_va;
}

/* The dispatcher caching a block it found for va: only while the TLB still
   says va is there, inside ijc_seq, so another CPU's TLBI can drop this CPU's
   cached targets itself (hv_tlbi_drop_remote_va). */
int hv_ijc_fill_checked(HvCpu *cpu, HvBlock *b) {
    HvPerCpu *p = cpu->pc_state;
    if (!p) {
        hv_ijc_fill(cpu, b);
        return 1;
    }
    __atomic_add_fetch(&p->ijc_seq, 1, __ATOMIC_SEQ_CST);
    int ok = tlb_maps_block(cpu, b);
    if (ok) hv_ijc_fill(cpu, b);
    __atomic_add_fetch(&p->ijc_seq, 1, __ATOMIC_RELEASE);
    return ok;
}

static void *ijc_resolve(HvCpu *cpu, uint64_t va, uint32_t flags);

void *hv_ijc_resolve(HvCpu *cpu, uint64_t va, uint32_t flags) {
    return ijc_resolve(cpu, va, flags);
}

static void *ijc_resolve(HvCpu *cpu, uint64_t va, uint32_t flags) {
    HvMachine *m = cpu->m;
    HvPerCpu *pcs = cpu->pc_state;
    int kernel = flags & HV_BLOCK_EL1;
#define FAIL(k) do { if (m->stats_interval) HV_CPU_COUNT(cpu, light_fail[k], 1); return 0; } while (0)
    if (!m->chain || hv_at_stop(m, va) || (int)((va >> 55) & 1) != kernel || (va & 3)) FAIL(0);
    if (pcs->ijc_keyed_count >= (1u << 17) || pcs->ucode_count + 1 >= (1u << HV_UCODE_BITS) / 2) FAIL(1);
    if (!hv_tlb_is_ready(cpu)) FAIL(2); /* as hv_tlb_refill: the dispatcher readies it */
    unsigned bits;
    int table = hv_tlb_block_table(flags, &bits);
    uint64_t page_va = va & ~((1ull << bits) - 1);
    if (table == HV_TLB_SMALL_TABLE && hv_code_page_bits(cpu, va) == HV_TLB_PAGE_BITS) FAIL(2); /* not a 4 KB-page process now */
    HvTlbEntry *e = hv_tlb_at(cpu, va, table, bits);
    if (e->exec_tag != hv_exec_key(page_va, e->addend)) {
        /* Not in the TLB (a process switch, or another page took the slot):
           walk for it here, as hv_tlb_refill does for data. A fault is for the
           dispatcher to take. */
        HvWalk w;
        walk_begin(cpu);
        hv_mmu_walk(cpu, va, HV_ACC_EXEC, kernel ? 1 : 0, &w);
        if (!w.fsc) tlb_fill(cpu, va, kernel ? 1 : 0, &w);
        walk_end(cpu);
        if (w.fsc || e->exec_tag != hv_exec_key(page_va, e->addend)) FAIL(2);
        if (m->stats_interval) HV_CPU_COUNT(cpu, light_walks, 1);
    }
    const uint8_t *host = (const uint8_t *)(uintptr_t)(va + (uint64_t)e->addend);
    HvRam *ram = ram_of_host(m, host);
    if (!ram) return 0;
    uint64_t pa = ram->pa + (uint64_t)(host - ram->host);
    uint32_t gen = __atomic_load_n(&ram->page_gen[(pa - ram->pa) >> HV_RAM_PAGE_BITS], __ATOMIC_ACQUIRE);
    /* This CPU's small cache in front of the block hash first (as the dispatcher's lookup). */
    uint32_t j = (uint32_t)((va >> 2) ^ (va >> 18) ^ ((uint64_t)flags << 13)) & ((1u << HV_JC_BITS) - 1);
    HvBlock *b = pcs->jc[j].va == va ? pcs->jc[j].b : NULL;
    /* Not a block of a zone being emptied (the reclaimer: this CPU's entries for it go at its next answer) */
    if (!(b && b->va == va && b->pa == pa && b->flags == flags && b->gen == gen && !hv_block_dying(m, b))) {
        b = hv_block_find(m, hv_block_hash(va, pa, flags), va, pa, flags, gen);
        if (!b) FAIL(3);
#undef FAIL
        pcs->jc[j].va = va, pcs->jc[j].b = b, pcs->jc_used |= HV_PART(j, HV_JC_BITS); /* plain: HvPerCpu.jc_used */
    }
    if (m->stats_interval) { /* what the fill replaces, as the dispatcher counts it */
        uint64_t *t = pcs->ijc + (size_t)hv_ijc_table(b->flags) * (HV_IJC_TABLE_BYTES / 8);
        uint64_t tag = b->va ^ cpu->ijc_salt[kernel];
        uint64_t old = t[(size_t)hv_ijc_index(tag) * 2];
        int k = !old ? 0 : (old >> 48) != (tag >> 48) ? 1 : 2;
        if (k == 1) {
            uint32_t c = (uint32_t)(old >> 48);
            if (!(kernel ? (c ^ 0xffff) == cpu->kernel_gen : cpu->asid_cid[cpu->cid_asid[c]] == c)) k = 3;
        }
        HV_CPU_COUNT(cpu, light_kind[k], 1);
        HV_CPU_COUNT(cpu, ijc_light, 1);
        if (!kernel) HV_CPU_COUNT(cpu, light_region[(va - 0x180000000ull) < 0x100000000ull ? 0 : 1], 1);
    }
    /* Cached only while the TLB still maps it, inside ijc_seq (see hv_ijc_fill_checked). */
    if (!hv_ijc_fill_checked(cpu, b)) return 0;
    pcs->zone_hits[b->zone]++; /* which zones of the code cache are in use (core/dispatch.c, make_room) */
    return b->code;
}

/* ---- user TLB entries kept across process switches (HvTlbStash) ---- */

/* Forget the stashed entries of address space asid (-1: all). Other CPUs call
   this on the target's stashes too, between reading and rechecking its
   walk_seq: saves and restores run inside walk_begin/walk_end, so a TLBI that
   races one of them falls back to waiting for the target. */
static void stash_drop(HvPerCpu *p, int asid) {
    for (int i = 0; i < HV_STASHES; i++) {
        uint64_t t = __atomic_load_n(&p->stash[i].ttbr0, __ATOMIC_SEQ_CST);
        if (t && (asid < 0 || (int)(t >> 48) == asid)) __atomic_add_fetch(&p->stash[i].gen, 1, __ATOMIC_SEQ_CST);
    }
}

/* The guest page of a stash's entries, as a mask. Stashes are kept per TCR, so in a 4 KB build one kept while a
   4 KB process runs may hold a 16 KB process's pages as four entries each: a TLBI of any address in such a page (whose
   size the TCR then current gives as 4 KB) drops them all. Read by other CPUs under the walk_seq check, like ttbr0. */
static uint64_t stash_page_mask(const HvTlbStash *s) {
#if HV_TLB_PAGE_BITS == 12
    unsigned tg = (unsigned)(__atomic_load_n(&s->tcr0, __ATOMIC_RELAXED) >> 14) & 3;
    return tg == 2 ? 0x3fff : tg == 1 ? 0xffff : PAGE_MASK;
#else
    (void)s;
    return PAGE_MASK;
#endif
}

/* Forget the stashed entries for va's page in address space asid (-1: all). Other CPUs do this to
   the target's stashes too, under the same walk_seq check as stash_drop; they only ever write tags
   (never an addend), so a restore racing it reads each entry whole or with a tag gone. */
static void stash_drop_va(HvPerCpu *p, uint64_t va, int asid) {
    for (int i = 0; i < HV_STASHES; i++) {
        HvTlbStash *s = &p->stash[i];
        uint64_t t = __atomic_load_n(&s->ttbr0, __ATOMIC_SEQ_CST);
        if (!t || (asid >= 0 && (int)(t >> 48) != asid)) continue;
        uint64_t mask = stash_page_mask(s);
        uint64_t pg = va & 0x00ffffffffffffffull & ~mask; /* tagged addresses: by the low 56 bits */
        uint32_t n = __atomic_load_n(&s->n, __ATOMIC_RELAXED);
        if (n > HV_STASH_CAP) n = HV_STASH_CAP;
        for (uint32_t k = 0; k < n; k++) {
            HvTlbEntry *e = &s->ent[k];
            uint64_t rt = __atomic_load_n(&e->read_tag, __ATOMIC_RELAXED), wt = __atomic_load_n(&e->write_tag, __ATOMIC_RELAXED),
                     xt = __atomic_load_n(&e->exec_tag, __ATOMIC_RELAXED);
            if (rt == HV_TLB_INVALID && wt == HV_TLB_INVALID && xt == HV_TLB_INVALID) continue;
            uint64_t tag = rt != HV_TLB_INVALID ? rt + HV_TLB_ADJ : xt != HV_TLB_INVALID ? hv_exec_key(xt, e->addend) : wt + HV_TLB_ADJ;
            if ((tag & 0x00ffffffffffffffull & ~mask) != pg) continue;
            __atomic_store_n(&e->read_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
            __atomic_store_n(&e->write_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
            __atomic_store_n(&e->exec_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
        }
    }
}

/* stash_drop_va for npages pages from va, in one pass. */
static void stash_drop_range(HvPerCpu *p, uint64_t va, uint64_t npages, int asid) {
    uint64_t start = va & 0x00ffffffffffffffull & ~(uint64_t)PAGE_MASK, end = start + (npages << HV_TLB_PAGE_BITS);
    for (int i = 0; i < HV_STASHES; i++) {
        HvTlbStash *s = &p->stash[i];
        uint64_t t = __atomic_load_n(&s->ttbr0, __ATOMIC_SEQ_CST);
        if (!t || (asid >= 0 && (int)(t >> 48) != asid)) continue;
        uint64_t mask = stash_page_mask(s); /* whole pages of the stash's own size */
        uint64_t lo = start & ~mask, span = ((end + mask) & ~mask) - lo;
        uint32_t n = __atomic_load_n(&s->n, __ATOMIC_RELAXED);
        if (n > HV_STASH_CAP) n = HV_STASH_CAP;
        for (uint32_t k = 0; k < n; k++) {
            HvTlbEntry *e = &s->ent[k];
            uint64_t rt = __atomic_load_n(&e->read_tag, __ATOMIC_RELAXED), wt = __atomic_load_n(&e->write_tag, __ATOMIC_RELAXED),
                     xt = __atomic_load_n(&e->exec_tag, __ATOMIC_RELAXED);
            if (rt == HV_TLB_INVALID && wt == HV_TLB_INVALID && xt == HV_TLB_INVALID) continue;
            uint64_t tag = rt != HV_TLB_INVALID ? rt + HV_TLB_ADJ : xt != HV_TLB_INVALID ? hv_exec_key(xt, e->addend) : wt + HV_TLB_ADJ;
            if ((((tag & 0x00ffffffffffffffull & ~mask) - lo) & 0x00ffffffffffffffull) >= span) continue;
            __atomic_store_n(&e->read_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
            __atomic_store_n(&e->write_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
            __atomic_store_n(&e->exec_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
        }
    }
}

void hv_tlb_stash_drop(HvCpu *cpu, int asid) {
    if (cpu->pc_state) stash_drop(cpu->pc_state, asid);
}

static int user_entry(const HvTlbEntry *e) {
    if (e->read_tag == HV_TLB_INVALID && e->exec_tag == HV_TLB_INVALID && e->write_tag == HV_TLB_INVALID) return 0;
    uint64_t tag = e->read_tag != HV_TLB_INVALID ? e->read_tag + HV_TLB_ADJ : e->exec_tag != HV_TLB_INVALID ? hv_exec_key(e->exec_tag, e->addend) : e->write_tag + HV_TLB_ADJ;
    return !((tag >> 55) & 1);
}

static inline HvRam *ram_of_host(HvMachine *m, const uint8_t *host) {
    if (__builtin_expect(m->main_ram && (uint64_t)(host - m->main_ram->host) < m->main_ram->size, 1))
        return m->main_ram;
    for (int i = 0; i < m->nram; i++)
        if ((uint64_t)(host - m->ram[i].host) < m->ram[i].size) return &m->ram[i];
    return NULL;
}

/* HV_SWITCH_FLUSH=2 (an unsafe experiment, off by default; see sysreg.c): every entry this CPU holds, in its TLB and in the stashes it would bring back, walked again
   in its own address space. Returns how many no longer match the page tables (a walk fault, or another page), so that
   a whole-TLB flush that is left out would have dropped something the guest expects gone. A cleared access flag alone
   is counted apart (stats) and not as out of date: xnu ages pages with its own TLBIs. */
int hv_tlb_check_all(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    uint64_t ttbr0 = cpu->ttbr0_el1, tcr = cpu->tcr_el1;
    int bad = 0;
    static int told;
    for (int pass = -1; pass < (p ? HV_STASHES : 0); pass++) {
        uint32_t n = HV_TLB_TABLES * HV_TLB_SIZE;
        const HvTlbStash *s = NULL;
        if (pass >= 0) {
            s = &p->stash[pass];
            uint64_t t = __atomic_load_n(&s->ttbr0, __ATOMIC_ACQUIRE);
            if (!t || __atomic_load_n(&s->gen, __ATOMIC_ACQUIRE) != s->saved_gen) continue; /* would not be brought back */
            n = s->n > HV_STASH_CAP ? HV_STASH_CAP : s->n;
            cpu->ttbr0_el1 = t;
            cpu->tcr_el1 = (tcr & ~0xffffull) | s->tcr0; /* walked under the TCR it was kept with */
        }
        for (uint32_t k = 0; k < n; k++) {
            const HvTlbEntry *e;
            int table;
            if (!s) table = (int)(k / HV_TLB_SIZE), e = hv_tlb_ent(cpu, k % HV_TLB_SIZE, table);
            else table = hv_tlb_tmajor ? s->slot[k] >> HV_TLB_BITS : s->slot[k] & 3, e = &s->ent[k];
            uint64_t rt = e->read_tag, wt = e->write_tag, xt = e->exec_tag;
            if (rt == HV_TLB_INVALID && wt == HV_TLB_INVALID && xt == HV_TLB_INVALID) continue;
            uint64_t va = rt != HV_TLB_INVALID ? rt + HV_TLB_ADJ : wt != HV_TLB_INVALID ? wt + HV_TLB_ADJ : hv_exec_key(xt, e->addend);
            if (s && (va >> 55) & 1) continue; /* stashes hold the user half only */
            HvWalk w;
            int small = table == HV_TLB_SMALL_TABLE; /* one 4 KB page, EL0's */
            uint64_t pmask = small ? (1ull << HV_TLB_SMALL_BITS) - 1 : PAGE_MASK;
            hv_mmu_walk(cpu, va, rt != HV_TLB_INVALID || wt != HV_TLB_INVALID ? HV_ACC_READ : HV_ACC_EXEC, table == 1 || table == 2 ? 1 : 0, &w);
            if (m->stats_interval) HV_CPU_COUNT(cpu, flush_check_entries, 1);
            const char *why = NULL;
            if (w.fsc >= HV_FSC_ACCESS(0) && w.fsc <= HV_FSC_ACCESS(3)) {
                if (m->stats_interval) HV_CPU_COUNT(cpu, flush_check_af, 1);
                continue;
            }
            if (w.fsc >= HV_FSC_PERMISSION(0) && w.fsc <= HV_FSC_PERMISSION(3)) continue; /* PAN or EL: not about where the page is */
            if (w.fsc) why = "no mapping";
            else if (!small && w.block_size < (1ull << HV_TLB_PAGE_BITS)) why = "small pages no longer one run";
            else {
                uint64_t page_pa = w.pa & ~pmask;
                const uint8_t *host = (const uint8_t *)(uintptr_t)((va & ~pmask) + (uint64_t)e->addend);
                HvRam *ram = ram_of_host(m, host);
                if (!ram || ram->pa + (uint64_t)(host - ram->host) != page_pa) why = "another page";
            }
            if (why) {
                bad++;
                if (told++ < 16)
                    fprintf(stderr, "hengevm: switch-flush check: %s entry (table %d, va %llx, ttbr0 %llx) is out of date: %s (fsc %x)\n",
                            s ? "stashed" : "live", table, (unsigned long long)va, (unsigned long long)cpu->ttbr0_el1, why, w.fsc);
            }
        }
        cpu->ttbr0_el1 = ttbr0, cpu->tcr_el1 = tcr;
    }
    cpu->ttbr0_el1 = ttbr0, cpu->tcr_el1 = tcr;
    if (m->stats_interval) {
        HV_CPU_COUNT(cpu, flush_checked, 1);
        if (bad) HV_CPU_COUNT(cpu, flush_check_bad, 1);
    }
    return bad;
}

/* The user half went from (TTBR0 old, TCR TTBR0 controls old_tcr0) to (now, now_tcr0): a process switch, or macOS
   switching TG0 between a 4 KB process (an Intel app under Rosetta) and a 16 KB one, in either order around the TTBR0
   write. Keep the old user entries aside, keyed by both (each was walked under that TTBR0 and TCR), drop them from the
   TLB, and bring back the new pair's if they were kept and no TLBI touched them since: what a TLB tagged by ASID does,
   whose entries a TCR write leaves alone. tcr: the TCR changed (if nothing is kept, no stash can be trusted either). */
static void switch_space(HvCpu *cpu, uint64_t old, uint64_t old_tcr0, uint64_t now, uint64_t now_tcr0, int tcr) {
    HvPerCpu *p = cpu->pc_state;
    HvMachine *m = cpu->m;
    if (old == now && old_tcr0 == now_tcr0) return; /* the same address space: a TLB tagged by ASID would keep everything too */
    static int no_stash = -1; /* HV_NO_STASH: a switch just drops the user entries */
    if (no_stash < 0) no_stash = getenv("HV_NO_STASH") != NULL;
    if (!p || current_asid(cpu) < 0 || m->watch_va || m->watch_pa || no_stash) {
        hv_tlb_flush_user(cpu);
        if (tcr) hv_tlb_stash_drop(cpu, -1);
        return;
    }
    walk_begin(cpu);
    HV_CPU_COUNT(cpu, tlb_flushes, 1);
    uint32_t n = cpu->nuser_fills;
    cpu->nuser_fills = 0;
    HvTlbStash *s = NULL;
    if (old && n && n <= HV_STASH_CAP) { /* nothing filled: keep the stashes there are (the other half of a two-write switch) */
        for (int i = 0; i < HV_STASHES && !s; i++)
            if (p->stash[i].ttbr0 == old && p->stash[i].tcr0 == old_tcr0) s = &p->stash[i];
        for (int i = 0; i < HV_STASHES && !s; i++)
            if (!p->stash[i].ttbr0) s = &p->stash[i];
        if (!s) {
            s = &p->stash[0];
            for (int i = 1; i < HV_STASHES; i++)
                if (p->stash[i].used < s->used) s = &p->stash[i];
        }
        /* Before the TTBR0 that publishes the stash: other CPUs' TLBIs read it (stash_page_mask). */
        __atomic_store_n(&s->tcr0, old_tcr0, __ATOMIC_RELAXED);
        __atomic_store_n(&s->ttbr0, old, __ATOMIC_SEQ_CST);
        s->saved_gen = __atomic_load_n(&s->gen, __ATOMIC_SEQ_CST);
        s->used = ++p->stash_clock;
        if (m->stats_interval) HV_CPU_COUNT(cpu, stash_saves, 1);
    }
    uint32_t k = 0;
    if (n <= HV_USER_FILLS) {
        for (uint32_t i = 0; i < n; i++) {
            uint16_t code = cpu->user_fills[i];
            HvTlbEntry *e = &cpu->tlb[0] + code;
            if (!user_entry(e)) continue; /* the slot may hold a kernel page by now */
            if (s) {
                s->slot[k] = code;
                s->ent[k++] = *e;
            }
            memset(e, 0x80, sizeof *e);
        }
    } else {
        for (uint32_t i = 0; i < HV_TLB_SIZE; i++)
            for (int t = 0; t < HV_TLB_TABLES; t++)
                if (user_entry(hv_tlb_ent(cpu, i, t))) memset(hv_tlb_ent(cpu, i, t), 0x80, sizeof(HvTlbEntry));
    }
    if (s) s->n = k;

    /* None for TTBR0 0: no stash is saved for it (old, above), and 0 is what a free or used-up stash reads, entries
       and all, which no TLBI reaches any more (stash_drop passes ttbr0 0 by). */
    for (int i = 0; now && i < HV_STASHES; i++) {
        HvTlbStash *r = &p->stash[i];
        if (r->ttbr0 != now || r->tcr0 != now_tcr0) continue;
        int fresh = __atomic_load_n(&r->gen, __ATOMIC_SEQ_CST) == r->saved_gen;
        __atomic_store_n(&r->ttbr0, 0, __ATOMIC_SEQ_CST); /* used up either way: the live TLB has them now */
        if (!fresh) {
            if (m->stats_interval) HV_CPU_COUNT(cpu, stash_stale, 1);
            break;
        }
        /* Entries only in a TLB whose others read invalid, as fill_entry: one not ready is zeros, which nobody's TLBI
           reaches (a CPU switches address space only as it runs, so its TLB is ready already but for a caller that
           didn't run it first) */
        if (!hv_tlb_is_ready(cpu)) hv_tlb_ready(cpu);
        /* Writable entries: all their hints, one fence, then each page's code check (write_guard, batched: one fence per
           switch instead of one per entry; every entry is in place before the fence and checked after it). */
        uint16_t pend[HV_STASH_CAP];
        uint32_t npend = 0;
        for (uint32_t j = 0; j < r->n; j++) {
            const HvTlbEntry *from = &r->ent[j];
            if (from->read_tag == HV_TLB_INVALID && from->write_tag == HV_TLB_INVALID && from->exec_tag == HV_TLB_INVALID)
                continue; /* its page had a TLBI while it was put aside (stash_drop_va) */
            HvTlbEntry *e = &cpu->tlb[0] + r->slot[j];
            *e = *from;
            cpu->user_fills[cpu->nuser_fills++] = r->slot[j];
            if (e->write_tag != HV_TLB_INVALID) {
                const uint8_t *host = (const uint8_t *)(uintptr_t)(from->write_tag + HV_TLB_ADJ + from->addend);
                HvRam *ram = ram_of_host(m, host);
                if (!ram) e->write_tag = HV_TLB_INVALID;
                else write_hint(cpu, ram, (uint64_t)(host - ram->host) >> HV_RAM_PAGE_BITS, r->slot[j]), pend[npend++] = (uint16_t)j;
            }
        }
        if (npend) __atomic_thread_fence(__ATOMIC_SEQ_CST);
        for (uint32_t q = 0; q < npend; q++) {
            /* from the live entry, not the stash copy, which a remote stash_drop_va may have wiped since. */
            HvTlbEntry *e = &cpu->tlb[0] + r->slot[pend[q]];
            uint64_t wt = __atomic_load_n(&e->write_tag, __ATOMIC_RELAXED);
            if (wt == HV_TLB_INVALID) continue;
            const uint8_t *host = (const uint8_t *)(uintptr_t)(wt + HV_TLB_ADJ + e->addend);
            HvRam *ram = ram_of_host(m, host);
            uint64_t hpg = ram ? (uint64_t)(host - ram->host) >> HV_RAM_PAGE_BITS : 0;
            if (!ram || __atomic_load_n(&ram->code_pages[hpg], __ATOMIC_RELAXED) || __atomic_load_n(&ram->save_guard[hpg], __ATOMIC_ACQUIRE))
                e->write_tag = HV_TLB_INVALID; /* code now, or kept by an auto-save (saveguard.h) */
            else hv_ram_mark(ram, hpg); /* writable again: as write_guard */
        }
        if (m->stats_interval) HV_CPU_COUNT(cpu, stash_hits, 1);
        if (m->stats_interval) HV_CPU_COUNT(cpu, stash_entries, r->n);
        break;
    }
    walk_end(cpu);
}

void hv_tlb_switch_user(HvCpu *cpu, uint64_t old, uint64_t now) {
    uint64_t tcr0 = cpu->tcr_el1 & 0xffff;
    switch_space(cpu, old, tcr0, now, tcr0, 0);
}

void hv_tlb_switch_tcr(HvCpu *cpu, uint64_t old_tcr) {
    switch_space(cpu, cpu->ttbr0_el1, old_tcr & 0xffff, cpu->ttbr0_el1, cpu->tcr_el1 & 0xffff, 1);
}
