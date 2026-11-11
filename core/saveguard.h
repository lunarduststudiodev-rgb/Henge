/* The save guard: guest RAM as it was at one instant (T), kept while the Mac runs on.

   An auto-save stops the Mac only long enough to fix T. hv_guard_arm (all CPUs paused) marks every page of
   writable RAM guarded; from then on the first write to a page, by anyone, first copies the page as it was into
   the guard's buffer ("claims" it). The save is then made while the Mac runs: a page it reads from RAM or from a
   clone of the RAM files is T's as long as it was never claimed, and a claimed page's T bytes are in the buffer.

   Who writes RAM, and how each claims first:
   - translated code: a guarded page gets no TLB write tag (tlb_fill, the stash restore in mmu.c), so its stores
     take the slow path, which writes through hv_phys_write. Arming posts HV_REQ_TLB_ALL to every CPU, which each
     processes before it runs again.
   - hv_phys_write and the atomics' host pointer (memop.c rmw_target): hv_guard_write before the bytes change.
   - devices: hv_phys_ptr_w (machine.h) for a pointer they will write through.
   - the GPU, which nothing can intercept: the save claims its pages itself (hv_guard_claim_range) while the GPU
     runs nothing new.

   Page states (HvRam.save_guard, one byte per 16 KB page): 0 free (never armed, or claimed already),
   1 guarded, 2 being copied. A claim is 1 -> 2, copy, record the slot, -> 0 (release). A writer that finds 2 waits
   the few microseconds of the copy. The guard never makes a writer wait for room: when the buffer is full the
   claim records nothing, the save is void (full), and the write goes on. */
#ifndef HENGEVM_SAVEGUARD_H
#define HENGEVM_SAVEGUARD_H

#include <stdint.h>

struct HvMachine;
struct HvRam;

#define HV_GUARD_PAGE (1ull << 14) /* HV_RAM_PAGE_BITS (saveguard.c checks) */
#define HV_GUARD_FREE 0
#define HV_GUARD_ON 1
#define HV_GUARD_COPYING 2

typedef struct HvSaveGuard {
    uint8_t *buf;          /* slot k at buf + k * HV_GUARD_PAGE (the caller's; touched before arming) */
    uint64_t cap;          /* slots in buf */
    uint64_t *slot_page;   /* per slot: region index << 48 | page index (the caller's, cap entries) */
    uint64_t used;         /* slots taken (atomic; can pass cap: then full) */
    int full;              /* a claim found no room: what was claimed is incomplete, the save must be dropped */
    uint64_t claims_cpu;   /* claims through hv_phys_write (mostly guest stores; statistics) */
    uint64_t claims_dev;   /* through hv_phys_ptr_w, the atomics, or the save itself */
    uint64_t waits;        /* writers that found a copy in progress */
} HvSaveGuard;

/* The buffer for up to `want` pages: a file in dir (unlinked at once: nothing is left of it after a crash), sparse,
   mapped shared, so pages copied into it are the file's, which iOS writes out rather than counting them as this
   app's memory. Fewer slots where storage (less `reserve` bytes) has no room for want; -1 if not even `least`. */
int hv_guard_alloc(HvSaveGuard *g, const char *dir, uint64_t want, uint64_t least, uint64_t reserve);
void hv_guard_release(HvSaveGuard *g);

/* All CPUs paused (hv_machine_pause_wait), and under the device lock: guards every page of every writable RAM
   region and asks every CPU to drop its TLB before it runs again. 0, or -1 if already armed. */
int hv_guard_arm(struct HvMachine *m, HvSaveGuard *g);

/* Ends the guard: no claim after it, and every claim under way has finished (its slot recorded). Any thread,
   CPUs running. Returns the slots holding pages (<= cap). Safe when not armed (0). */
uint64_t hv_guard_disarm(struct HvMachine *m);

/* Claim page pg of r if it is still guarded (copy it into the buffer). Returns once the page is free to write. */
void hv_guard_claim(struct HvMachine *m, struct HvRam *r, uint64_t pg, int by_cpu);

/* Claim every page of [pa, pa + len) that is RAM and still guarded. */
void hv_guard_claim_range(struct HvMachine *m, uint64_t pa, uint64_t len);

/* Before writing [off, off + len) of r: claims what the write touches. One load when nothing is guarded. */
static inline void hv_guard_write(struct HvMachine *m, struct HvRam *r, uint8_t *pages, uint64_t off, uint64_t len,
                                  int by_cpu) {
    if (!len) return;
    uint64_t first = off >> 14, last = (off + len - 1) >> 14;
    for (uint64_t pg = first; pg <= last; pg++)
        if (__atomic_load_n(&pages[pg], __ATOMIC_ACQUIRE) != HV_GUARD_FREE) hv_guard_claim(m, r, pg, by_cpu);
}

#endif
