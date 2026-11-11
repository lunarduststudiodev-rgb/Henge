/* A protector must not skip a CPU that an earlier,
   unfinished protector of the same page is still waiting for.

   A marks the page and drops T's writable entry, then waits for T (T models a
   CPU preempted between its TLB check and its STR: it never acks until we let
   it). A write clears the mark (hv_invalidate_code). B marks the page again:
   T's hint is gone (A took it), so B drops nothing, waits for nobody, sets
   PROTECTED and would read the code while T's store is still in flight.

   Exit 1 = B finished before T acked (the hole). Exit 0 = B waited. */
#include "../core/dispatch.c"
#include <pthread.h>
#include <unistd.h>
#include "zalloc.h"

static HvMachine M;
static HvCpu *A, *B, *T;
static HvRam *ram;
static uint64_t pg, pa;
static volatile int a_done, b_done;
static uint32_t a_gen, b_gen;

static void *runA(void *u) { (void)u; hv_self = A; a_gen = protect_code_page(A, ram, pg); a_done = 1; return NULL; }
static void *runB(void *u) { (void)u; hv_self = B; b_gen = protect_code_page(B, ram, pg); b_done = 1; return NULL; }

static HvCpu *mk(int k) {
    HvCpu *c = zalloc(_Alignof(HvCpu), sizeof(HvCpu));
    c->pc_state = NULL;
    hv_cpu_reset_index(c, &M, k);
    hv_tlb_ready(c); /* CPUs that run (their TLBs all invalid): T's entry is put in by hand below */
    return c;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (hv_machine_init(&M, 64u << 20) != 0) return 2;
    uint8_t *buf = aligned_alloc(16384, 4 * 16384);
    memset(buf, 0, 4 * 16384);
    hv_machine_add_ram(&M, (uint64_t)(uintptr_t)buf, 4 * 16384, buf, 0);
    A = mk(0), B = mk(1), T = mk(2);
    pa = (uint64_t)(uintptr_t)buf + 16384;
    ram = hv_ram_lookup(&M, pa);
    pg = (pa - ram->pa) >> HV_RAM_PAGE_BITS;

    /* T maps the page writable (identity: va == pa == host) and its hint names the entry. */
    uint32_t slot = (uint32_t)(pa >> HV_TLB_PAGE_BITS) & (HV_TLB_SIZE - 1);
    HvTlbEntry *e = hv_tlb_ent(T, slot, 0);
    e->write_tag = pa - HV_TLB_ADJ;
    e->addend = 0;
    ram->whint[pg * HV_MAX_CPUS + T->index] = hv_tlb_code(slot, 0) + 1;
    /* T runs translated code (its store is in flight): live, never acking on its own. */
    __atomic_store_n(&M.percpu[T->index].tlb_live, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&M.percpu[A->index].tlb_live, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&M.percpu[B->index].tlb_live, 1, __ATOMIC_SEQ_CST);

    pthread_t ta, tb;
    pthread_create(&ta, NULL, runA, NULL);
    for (int i = 0; i < 1000 && __atomic_load_n(&ram->code_pages[pg], __ATOMIC_SEQ_CST) != HV_CODE_MARKED_BY(0); i++) usleep(1000);
    usleep(50000);
    printf("A marked the page (mark %u), dropped T's entry (write_tag %s), waits for T: %s\n",
           ram->code_pages[pg], e->write_tag == HV_TLB_INVALID ? "invalid" : "VALID", a_done ? "no (returned!)" : "yes");

    /* Another CPU's slow-path write to the page (it is marked, so writes are noticed). */
    hv_invalidate_code(&M, pa);
    printf("after hv_invalidate_code: mark %u, gen %u\n", ram->code_pages[pg], ram->page_gen[pg]);

    pthread_create(&tb, NULL, runB, NULL);
    for (int i = 0; i < 300 && !b_done; i++) usleep(1000);
    int hole = b_done && !a_done;
    if (hole) {
        printf("HOLE: B finished protecting (mark %u = PROTECTED, gen %u) while T has not acked: its in-flight store is unseen\n",
               ram->code_pages[pg], b_gen);
        /* T's store lands now, through the entry it checked before A dropped it: no slow path, no invalidation. */
        uint32_t gen_before = __atomic_load_n(&ram->page_gen[pg], __ATOMIC_SEQ_CST);
        ((volatile uint32_t *)(uintptr_t)pa)[0] = 0xd2800020; /* the new code */
        printf("T's store landed: mark %u, gen %u (was %u): a block B translated from the old bytes stays valid\n",
               ram->code_pages[pg], ram->page_gen[pg], gen_before);
    } else {
        printf("B is still waiting (done %d) while T has a store in flight: ok\n", b_done);
    }
    /* Let T ack (it left its block). */
    hv_cpu_process_requests(T);
    for (int i = 0; i < 2000 && !(a_done && b_done); i++) { usleep(1000); hv_cpu_process_requests(T); }
    printf("after T acked: A done %d gen %u, B done %d gen %u, mark %u, page gen %u\n", a_done, a_gen, b_done, b_gen,
           ram->code_pages[pg], ram->page_gen[pg]);
    if (!(a_done && b_done)) { printf("FAIL: protectors hung\n"); return 1; }
    printf(hole ? "FAIL (hole reproduced)\n" : "ok\n");
    return hole;
}
