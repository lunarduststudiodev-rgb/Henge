/* The save guard: see saveguard.h. */
#include "saveguard.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/mount.h>

#include "machine.h"

_Static_assert(HV_GUARD_PAGE == 1ull << HV_RAM_PAGE_BITS, "the guard works in RAM pages (HvRam.save_guard)");

static uint64_t pages_of(const HvRam *r) {
    return (r->size + HV_GUARD_PAGE - 1) / HV_GUARD_PAGE;
}

int hv_guard_arm(HvMachine *m, HvSaveGuard *g) {
    if (!g || __atomic_load_n(&m->guard, __ATOMIC_ACQUIRE)) return -1;
    g->used = 0;
    g->full = 0;
    g->claims_cpu = g->claims_dev = g->waits = 0;
    /* The guard before any page is guarded: a claimer that finds a page guarded finds the guard too. */
    __atomic_store_n(&m->guard, g, __ATOMIC_RELEASE);
    for (int i = 0; i < m->nram; i++) {
        HvRam *r = &m->ram[i];
        if (!r->readonly && r->save_guard) memset(r->save_guard, HV_GUARD_ON, pages_of(r));
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    /* No CPU keeps a write tag from before: each drops its TLB (and the stash with it) before it runs again
       (run_loop processes requests first, also on the way out of the pause gate and CPU_OFF). */
    for (int i = 0; i < m->ncpus; i++)
        if (m->cpus[i] && m->cpus[i]->pc_state) hv_cpu_post(m->cpus[i], HV_REQ_TLB_ALL);
    return 0;
}

void hv_guard_claim(HvMachine *m, HvRam *r, uint64_t pg, int by_cpu) {
    uint8_t *s = &r->save_guard[pg];
    for (uint32_t spins = 0;; spins++) {
        uint8_t v = __atomic_load_n(s, __ATOMIC_ACQUIRE);
        if (v == HV_GUARD_FREE) return;
        if (v == HV_GUARD_ON) {
            uint8_t on = HV_GUARD_ON;
            if (!__atomic_compare_exchange_n(s, &on, HV_GUARD_COPYING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
            HvSaveGuard *g = __atomic_load_n(&m->guard, __ATOMIC_ACQUIRE); /* set before the page was guarded */
            if (g) {
                uint64_t k = __atomic_fetch_add(&g->used, 1, __ATOMIC_RELAXED);
                if (k < g->cap) {
                    uint64_t at = pg * HV_GUARD_PAGE, n = r->size - at < HV_GUARD_PAGE ? r->size - at : HV_GUARD_PAGE;
                    memcpy(g->buf + k * HV_GUARD_PAGE, r->host + at, n);
                    g->slot_page[k] = (uint64_t)(r - m->ram) << 48 | pg;
                } else {
                    __atomic_store_n(&g->full, 1, __ATOMIC_RELAXED); /* the write goes on; the save is void */
                }
                __atomic_add_fetch(by_cpu ? &g->claims_cpu : &g->claims_dev, 1, __ATOMIC_RELAXED);
            }
            /* The copy and its slot, then the page is free: whoever waits on it, and the disarm, see both. */
            __atomic_store_n(s, HV_GUARD_FREE, __ATOMIC_RELEASE);
            return;
        }
        /* Another thread is copying it: a 16 KB memcpy. */
        if (!spins) {
            HvSaveGuard *g = __atomic_load_n(&m->guard, __ATOMIC_ACQUIRE);
            if (g) __atomic_add_fetch(&g->waits, 1, __ATOMIC_RELAXED);
        }
        if (spins > 64) sched_yield();
    }
}

void hv_guard_claim_range(HvMachine *m, uint64_t pa, uint64_t len) {
    /* Not armed: nothing to claim. A writer is never concurrent with the arm (it holds the device lock, or the
       stop waited for it), so an arm can't come between this load and its write. */
    if (!len || !__atomic_load_n(&m->guard, __ATOMIC_ACQUIRE)) return;
    for (int i = 0; i < m->nram; i++) {
        HvRam *r = &m->ram[i];
        if (r->readonly || !r->save_guard) continue;
        uint64_t lo = pa > r->pa ? pa : r->pa, hi = pa + len < r->pa + r->size ? pa + len : r->pa + r->size;
        if (lo >= hi) continue;
        hv_guard_write(m, r, r->save_guard, lo - r->pa, hi - lo, 0);
    }
}

/* Waits for page s to leave COPYING, or takes its guard off. */
static void unguard(uint8_t *s) {
    for (uint32_t spins = 0;; spins++) {
        uint8_t v = __atomic_load_n(s, __ATOMIC_ACQUIRE);
        if (v == HV_GUARD_FREE) return;
        if (v == HV_GUARD_ON) {
            uint8_t on = HV_GUARD_ON;
            if (__atomic_compare_exchange_n(s, &on, HV_GUARD_FREE, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
            continue;
        }
        if (spins > 64) sched_yield();
    }
}

uint64_t hv_guard_disarm(HvMachine *m) {
    HvSaveGuard *g = __atomic_load_n(&m->guard, __ATOMIC_ACQUIRE);
    if (!g) return 0;
    for (int i = 0; i < m->nram; i++) {
        HvRam *r = &m->ram[i];
        if (r->readonly || !r->save_guard) continue;
        uint64_t n = pages_of(r);
        /* Eight pages at a time where it can: a word of eight guarded pages unguarded at once. The array is
           calloc'd (16-byte aligned) with 8 bytes to spare; a word CAS and the claims' byte CAS on the same bytes
           are coherent on arm64 (both are single-copy atomic, aligned). */
        for (uint64_t pg = 0; pg < n;) {
            if (!(pg & 7) && pg + 8 <= n) {
                uint64_t *w = (uint64_t *)(void *)(r->save_guard + pg), v = __atomic_load_n(w, __ATOMIC_ACQUIRE);
                if (!v) { pg += 8; continue; }
                if (v == 0x0101010101010101ull && __atomic_compare_exchange_n(w, &v, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    pg += 8;
                    continue;
                }
            }
            unguard(&r->save_guard[pg]);
            pg++;
        }
    }
    /* Every claim that took a page has stored FREE after its slot (we saw each page FREE): none is under way. */
    __atomic_store_n(&m->guard, NULL, __ATOMIC_RELEASE);
    uint64_t used = __atomic_load_n(&g->used, __ATOMIC_ACQUIRE);
    return used < g->cap ? used : g->cap;
}

int hv_guard_alloc(HvSaveGuard *g, const char *dir, uint64_t want, uint64_t least, uint64_t reserve) {
    memset(g, 0, sizeof *g);
    char path[1100];
    snprintf(path, sizeof path, "%s/.save-guard-XXXXXX", dir);
    int fd = mkstemp(path);
    if (fd < 0) {
        fprintf(stderr, "hengevm: save guard: can't make a buffer in %s: %s\n", dir, strerror(errno));
        return -1;
    }
    unlink(path);
    struct statfs st;
    uint64_t cap = want;
    if (fstatfs(fd, &st) == 0) {
        uint64_t room = (uint64_t)st.f_bavail * st.f_bsize;
        room = room > reserve ? (room - reserve) / HV_GUARD_PAGE : 0;
        if (room < cap) cap = room;
    }
    if (cap < least || !cap) {
        fprintf(stderr, "hengevm: save guard: room for %llu pages, %llu needed\n", (unsigned long long)cap, (unsigned long long)least);
        close(fd);
        return -1;
    }
    void *p = MAP_FAILED;
    if (ftruncate(fd, (off_t)(cap * HV_GUARD_PAGE)) == 0)
        p = mmap(NULL, cap * HV_GUARD_PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int e = errno;
    close(fd);
    g->slot_page = p != MAP_FAILED ? calloc(cap, sizeof *g->slot_page) : NULL;
    if (!g->slot_page) {
        fprintf(stderr, "hengevm: save guard: no buffer of %llu MB: %s\n", (unsigned long long)(cap * HV_GUARD_PAGE >> 20), strerror(e));
        if (p != MAP_FAILED) munmap(p, cap * HV_GUARD_PAGE);
        return -1;
    }
    g->buf = p;
    g->cap = cap;
    return 0;
}

void hv_guard_release(HvSaveGuard *g) {
    if (g->buf) munmap(g->buf, g->cap * HV_GUARD_PAGE);
    free(g->slot_page);
    memset(g, 0, sizeof *g);
}
