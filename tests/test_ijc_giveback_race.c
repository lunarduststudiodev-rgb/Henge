/* A trim's give-back of a CPU's branch-cache tables against another CPU's TLBI dropping targets in them.

   The give-back swaps each held table for fresh pages, then clears its used bit. Another CPU's TLBI
   (hv_tlbi_drop_remote_va) looks a page's targets up in every table whose used bit it sees, reading entries, and a
   read of a fresh page brings it in. One that saw the bit just before it went read the new pages: up to a table's
   worth came back with no held bit, so no trim gave them back until the table filled again.

   Here a thread drops targets in a kernel page of CPU 0's, whose filters say it has some in every piece, as fast as
   it can, while this one marks a table used and held and gives it back, round after round. After each give-back,
   once the dropper has gone round twice more, the table must hold no page (its held bit is gone).

   Then the same for the TLB stashes: a thread drops a user page from CPU 0's, reading every entry of each stash
   whose ttbr0 isn't 0, while this one fills them all and gives them back. A drop reads stash entries only inside
   the give-back's window (ijc_remote, ijc_seq odd), so once it has gone round twice more the stashes must hold no
   page: one that read ttbr0 before the give-back emptied it and the entries after it swapped them brought fresh
   pages in, which nothing marks, so no give-back took them until the stashes were used again. */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dispatch.h"
#include "machine.h"
#include "mmu.h"

static HvMachine m;
static HvCpu cpu;
static _Atomic int stop;
static _Atomic uint64_t rounds;
static const uint64_t KVA = 0xfffffe0007000000ull;

static uint64_t held(const void *p, size_t bytes) {
    size_t page = (size_t)getpagesize(), n = (bytes + page - 1) / page;
    char *vec = malloc(n);
    uint64_t h = 0;
    if (vec && mincore(p, bytes, vec) == 0)
        for (size_t i = 0; i < n; i++) h += vec[i] & (MINCORE_INCORE | MINCORE_PAGED_OUT) ? page : 0;
    free(vec);
    return h;
}

static const uint64_t UVA = 0x200000000ull;

static void *dropper(void *arg) {
    uint64_t va = arg ? UVA : KVA;
    while (!atomic_load(&stop)) {
        hv_tlbi_drop_remote_va(&cpu, va, -1);
        atomic_fetch_add(&rounds, 1);
    }
    return NULL;
}

static void wait_rounds(uint64_t n) {
    uint64_t r0 = atomic_load(&rounds);
    while (atomic_load(&rounds) < r0 + n) sched_yield();
}

int main(void) {
    alarm(120);
    if (hv_machine_init(&m, 4 << 20)) return 1;
    hv_cpu_reset_index(&cpu, &m, 0);
    HvPerCpu *p = cpu.pc_state;
    /* The filters: the kernel page has targets in this salt, in every piece of it */
    uint32_t k = hv_ijc_key(KVA, cpu.ijc_salt[1]);
    p->ijc_keyed[k >> 6] |= 1ull << (k & 63);
    for (int i = 0; i < 4096; i++) p->ijc_hint[1][i].key = ~0ull;
    const int T = 1; /* a kernel table */
    uint64_t *tab = p->ijc + (size_t)T * (HV_IJC_TABLE_BYTES / 8);
    pthread_t th;
    pthread_create(&th, NULL, dropper, NULL);
    int n = 3000, leaked = 0;
    uint64_t worst = 0;
    for (int r = 0; r < n; r++) {
        __atomic_or_fetch(&p->ijc_tables_used, 1u << T, __ATOMIC_SEQ_CST);
        __atomic_or_fetch(&p->ijc_tables_held, 1u << T, __ATOMIC_SEQ_CST);
        wait_rounds(1); /* the dropper is reading the table: its pages are in */
        hv_cpu_ijc_give_back(p);
        wait_rounds(2); /* every drop that began before the give-back has ended */
        uint64_t h = held(tab, HV_IJC_TABLE_BYTES);
        if (h && !(__atomic_load_n(&p->ijc_tables_held, __ATOMIC_SEQ_CST) >> T & 1)) {
            leaked++;
            if (h > worst) worst = h;
            hv_cpu_ijc_give_back(p); /* not held: this gives back nothing; next round starts clean anyway */
            mmap(tab, HV_IJC_TABLE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        }
    }
    atomic_store(&stop, 1);
    pthread_join(th, NULL);
    printf("%d give-backs against %llu remote drops: %d left pages in memory with no held bit (worst %llu KB)\n", n,
           (unsigned long long)atomic_load(&rounds), leaked, (unsigned long long)worst >> 10);
    printf("%s\n", leaked ? "FAIL: a give-back raced a TLBI's drop and lost track of pages" : "ok");

    /* The stashes: every one full of entries for other pages, so a drop reads them all and changes none. */
    size_t page = (size_t)getpagesize();
    size_t sb = (HV_STASHES * HV_STASH_CAP * (sizeof(HvTlbEntry) + 2) + page - 1) & ~(page - 1);
    atomic_store(&stop, 0);
    atomic_store(&rounds, 0);
    pthread_create(&th, NULL, dropper, (void *)1);
    int sn = 3000, sleaked = 0;
    uint64_t sworst = 0;
    for (int r = 0; r < sn; r++) {
        for (int i = 0; i < HV_STASHES; i++) {
            HvTlbStash *st = &p->stash[i];
            for (int k = 0; k < HV_STASH_CAP; k++)
                st->ent[k] = (HvTlbEntry){.read_tag = UVA + ((uint64_t)(k + 1) << HV_TLB_PAGE_BITS) - HV_TLB_ADJ,
                                          .write_tag = HV_TLB_INVALID, .exec_tag = HV_TLB_INVALID};
            st->tcr0 = 0;
            __atomic_store_n(&st->n, HV_STASH_CAP, __ATOMIC_RELAXED);
            __atomic_store_n(&st->ttbr0, (uint64_t)(i + 1) << 48 | 0x4000, __ATOMIC_SEQ_CST);
        }
        wait_rounds(1); /* the dropper is reading the stashes */
        hv_cpu_ijc_give_back(p);
        wait_rounds(2); /* every drop that began before the give-back has ended */
        uint64_t h = held(p->stash_mem, sb);
        if (h) {
            sleaked++;
            if (h > sworst) sworst = h;
            mmap(p->stash_mem, sb, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        }
    }
    atomic_store(&stop, 1);
    pthread_join(th, NULL);
    printf("%d stash give-backs against %llu remote drops: %d left stash pages in memory (worst %llu KB)\n", sn,
           (unsigned long long)atomic_load(&rounds), sleaked, (unsigned long long)sworst >> 10);
    printf("%s\n", sleaked ? "FAIL: a stash give-back raced a TLBI's drop and lost track of pages" : "ok");
    hv_machine_free(&m);
    return leaked != 0 || sleaked != 0;
}
