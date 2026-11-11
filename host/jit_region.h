#ifndef HENGEVM_JIT_REGION_H
#define HENGEVM_JIT_REGION_H

#include <stddef.h>
#include <stdint.h>

/* One executable region for translated blocks. Blocks are appended and never
   freed one by one; the whole region is reset when it fills.

   Two shapes. macOS: one MAP_JIT mapping, made writable per thread while code
   is copied in (rw_diff is 0). iOS: an executable mapping a debugger granted
   (StikDebug or the built-in helper) plus a writable alias of the same pages
   made with vm_remap; code is written through base + rw_diff and runs at base.
   Everything outside this file and the final copy in the translator sees only
   the executable addresses. HV_JIT_SPLIT=1 makes the macOS build use the iOS
   shape, to test it. */
typedef struct HvJitRegion {
    uint32_t *base;     /* executable addresses */
    intptr_t rw_diff;   /* writable alias = base + rw_diff (bytes), or 0 */
    size_t capacity;    /* in instructions */
    size_t used;        /* in instructions */
    size_t limit;       /* in instructions; 0: all of it (HV_JIT_CAP: a smaller cache in the same region, to compare) */
    int owned;          /* mapped here (closed here) rather than handed in */
} HvJitRegion;

int hv_jit_region_open(HvJitRegion *region, size_t bytes);
/* Use memory the host already has: rx executable, rw a writable alias of it (or rx itself on macOS MAP_JIT). */
int hv_jit_region_attach(HvJitRegion *region, void *rx, void *rw, size_t bytes);
void hv_jit_region_close(HvJitRegion *region);

/* Copy `words` instructions to start (an executable address) and make them runnable. */
void hv_jit_write(HvJitRegion *region, uint32_t *start, const void *src, size_t bytes);
void hv_jit_finish(HvJitRegion *region, uint32_t *start, size_t words);
/* Rewrite n single instructions anywhere in the region (*at[k] = words[k], each in one store as hv_jit_write does)
   and make them all runnable with one round of cache maintenance and barriers, not one each: what n pairs of
   hv_jit_write and hv_jit_finish do. Patching code other CPUs run must be a change CPUs may run either side of
   (a B or a NOP to another B). */
void hv_jit_patch(HvJitRegion *region, uint32_t *const *at, const uint32_t *words, size_t n);
/* hv_jit_finish for two ranges at once (a block's hot and cold parts): one wait for the instruction caches, not two. */
void hv_jit_finish2(HvJitRegion *region, uint32_t *a, size_t na, uint32_t *b, size_t nb);
/* After rewriting one B (an unconditional branch) as another B, which a CPU may run as it changes (Arm allows it for
   B): the instruction caches drop the line without waiting for them all to have done so. A CPU still running the old
   branch takes the way the old one went (chaining: the exit's stub, which follows the slot) until its cache lets go. */
void hv_jit_finish_branch(HvJitRegion *region, uint32_t *word);

/* Space for `words` instructions, or NULL when the region is full. */
uint32_t *hv_jit_reserve(HvJitRegion *region, size_t words);
void hv_jit_reset(HvJitRegion *region);

#endif
