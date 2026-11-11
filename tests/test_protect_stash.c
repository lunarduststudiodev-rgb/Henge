/* A stash restore must not lose the code check of
   an entry whose stash copy a remote TLBI wipes while the restore runs.

   The page is already code (a protector marked it and found no hint: the entry
   was in the stash then). The restore copies the stashed writable entry into
   the TLB and writes its hint; a remote TLBI for the same VA (stash_drop_va)
   then wipes the stash copy; the batched code check re-reads the stash copy,
   gets no RAM page from the wiped tag and skips the check. The live entry
   stays writable on a code page until the remote's fallback TLBI is processed.

   The remote's wipe is injected at the batch fence (the only full fence in
   the restore), which is a point it can really reach: the remote read
   walk_seq before walk_begin and runs stash_drop_va without further checks.

   Exit 1 = the live entry is still writable on the code page (the hole). */
#include <stdio.h>
static void review_hook(void);
static int review_armed;
static inline void review_fence(int o) {
    (void)o;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (review_armed) { review_armed = 0; review_hook(); }
}
#define __atomic_thread_fence(o) review_fence(o)
#include "../core/mmu.c"
#undef __atomic_thread_fence
#include <stdlib.h>
#include <string.h>
#include "zalloc.h"

static HvMachine M;
static HvCpu *C;
static uint64_t VA = 0x100000000ull;
static void review_hook(void) {
    /* Remote CPU: TLBI VAE1IS for VA in ASID 2 (hv_tlbi_drop_remote_va's first step). */
    /* stash_drop_va read this stash's ttbr0 (still `now`) before the restore cleared it, and was
       preempted; it resumes here and wipes the entry, as its loop body does. */
    HvTlbStash *s = &C->pc_state->stash[0];
    uint64_t pgva = VA & 0x00ffffffffffffffull & ~(uint64_t)PAGE_MASK;
    for (uint32_t k = 0; k < s->n; k++) {
        HvTlbEntry *e = &s->ent[k];
        uint64_t rt = __atomic_load_n(&e->read_tag, __ATOMIC_RELAXED);
        if (((rt + HV_TLB_ADJ) & 0x00ffffffffffffffull & ~(uint64_t)PAGE_MASK) != pgva) continue;
        __atomic_store_n(&e->read_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
        __atomic_store_n(&e->write_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
        __atomic_store_n(&e->exec_tag, HV_TLB_INVALID, __ATOMIC_RELAXED);
    }
    printf("remote TLBI (stash_drop_va, preempted after reading ttbr0) wiped the stash copy between the restore's fence and its code check\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&M, 64u << 20) != 0) return 2;
    uint8_t *buf = aligned_alloc(16384, 4 * 16384);
    memset(buf, 0, 4 * 16384);
    hv_machine_add_ram(&M, (uint64_t)(uintptr_t)buf, 4 * 16384, buf, 0);
    C = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
    hv_cpu_reset_index(C, &M, 0);
    HvPerCpu *p = C->pc_state;
    uint64_t old = 1ull << 48 | 0x4000, now = 2ull << 48 | 0x8000;
    C->tcr_el1 = 0;
    C->ttbr0_el1 = old;

    uint8_t *host = buf + 16384;
    HvRam *ram = ram_of_host(&M, host);
    uint64_t pg = (uint64_t)(host - ram->host) >> HV_RAM_PAGE_BITS;
    ram->code_pages[pg] = 1; /* HV_CODE_PROTECTED: translated while the entry was stashed (no hint to drop) */

    HvTlbStash *s = &p->stash[0];
    if (!s->slot) s->slot = calloc(HV_STASH_CAP, sizeof *s->slot);
    if (!s->ent) s->ent = calloc(HV_STASH_CAP, sizeof *s->ent);
    uint32_t slot = (uint32_t)(VA >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1);
    uint16_t code = (uint16_t)hv_tlb_code(slot, 0);
    s->ttbr0 = now; s->gen = s->saved_gen = 0; s->n = 1;
    s->slot[0] = code;
    s->ent[0].read_tag = VA - HV_TLB_ADJ;
    s->ent[0].write_tag = VA - HV_TLB_ADJ;
    s->ent[0].exec_tag = HV_TLB_INVALID;
    s->ent[0].addend = (int64_t)((uint64_t)(uintptr_t)host - VA);

    review_armed = 1;
    hv_tlb_switch_user(C, old, now);
    HvTlbEntry *e = &C->tlb[0] + code;
    printf("e: r %llx w %llx x %llx a %llx; stash w %llx; fills %u\n", (unsigned long long)e->read_tag, (unsigned long long)e->write_tag, (unsigned long long)e->exec_tag, (unsigned long long)e->addend, (unsigned long long)s->ent[0].write_tag, C->nuser_fills);
    int hole = e->write_tag != HV_TLB_INVALID && (uint8_t *)(uintptr_t)(e->write_tag + HV_TLB_ADJ + e->addend) == host;
    printf("after restore: live entry write_tag %s, page mark %u, hint %llx, hook ran %s\n",
           e->write_tag == HV_TLB_INVALID ? "invalid" : "VALID (maps the code page writable)", ram->code_pages[pg],
           (unsigned long long)ram->whint[pg * HV_MAX_CPUS], review_armed ? "no" : "yes");
    /* C never ran: its TLB was zeros, which other CPUs' TLBIs pass by. The restore makes it ready first (all invalid),
       as a fill does, or the entries it brings back would be beyond their reach. */
    int ready = hv_tlb_is_ready(C);
    printf("restored into a TLB %s\n", ready ? "made ready" : "NOT ready: other CPUs' TLBIs pass these entries by");
    printf(hole || !ready ? "FAIL%s\n" : "ok\n", hole ? " (hole reproduced): stores through this entry skip the slow path on a code page" : "");
    return hole || !ready;
}
