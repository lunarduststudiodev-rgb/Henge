#include "jit_region.h"

#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* The split shape on a host that can make it itself: plain RW memory, an
   alias, and the original turned R-X. Works for macOS binaries without the
   hardened runtime; on iOS the executable half has to come from a debugger. */
static int open_split(HvJitRegion *region, size_t bytes) {
    void *rx = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rx == MAP_FAILED) return -1;
    vm_address_t rw = 0;
    vm_prot_t cur, max;
    if (vm_remap(mach_task_self(), &rw, bytes, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)rx, FALSE, &cur, &max,
                 VM_INHERIT_NONE) != KERN_SUCCESS ||
        mprotect(rx, bytes, PROT_READ | PROT_EXEC) != 0) {
        munmap(rx, bytes);
        return -1;
    }
    if (hv_jit_region_attach(region, rx, (void *)rw, bytes) != 0) return -1;
    region->owned = 1;
    return 0;
}

int hv_jit_region_open(HvJitRegion *region, size_t bytes) {
    memset(region, 0, sizeof *region);
    if (getenv("HV_JIT_SPLIT")) return open_split(region, bytes);
#if TARGET_OS_OSX
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) return -1;
    region->base = p;
    region->capacity = bytes / sizeof(uint32_t);
    region->owned = 1;
    return 0;
#else
    return open_split(region, bytes); /* works only where nothing enforces code signing (tests on a jailbroken device) */
#endif
}

int hv_jit_region_attach(HvJitRegion *region, void *rx, void *rw, size_t bytes) {
    memset(region, 0, sizeof *region);
    if (!rx || !rw || bytes < 4096) return -1;
    region->base = rx;
    region->rw_diff = (intptr_t)((uint8_t *)rw - (uint8_t *)rx);
    region->capacity = bytes / sizeof(uint32_t);
    return 0;
}

void hv_jit_region_close(HvJitRegion *region) {
    if (region->owned && region->base) {
        size_t bytes = region->capacity * sizeof(uint32_t);
        if (region->rw_diff) munmap((uint8_t *)region->base + region->rw_diff, bytes);
        munmap(region->base, bytes);
    }
    memset(region, 0, sizeof *region);
}

void hv_jit_write(HvJitRegion *region, uint32_t *start, const void *src, size_t bytes) {
    /* One instruction may be rewritten while other CPUs run it (chaining a branch).
       memcpy may copy a word in pieces, and a CPU fetching in between takes half of
       the old branch and half of the new one: a jump to nowhere. */
    if (bytes == 4) {
        uint32_t word;
        memcpy(&word, src, 4);
#if TARGET_OS_OSX
        if (!region->rw_diff) pthread_jit_write_protect_np(0);
#endif
        __atomic_store_n((uint32_t *)((uint8_t *)start + region->rw_diff), word, __ATOMIC_RELEASE);
        return;
    }
    if (region->rw_diff) {
        memcpy((uint8_t *)start + region->rw_diff, src, bytes);
        return;
    }
#if TARGET_OS_OSX
    pthread_jit_write_protect_np(0); /* MAP_JIT: writable on this thread only, until hv_jit_finish */
#endif
    memcpy(start, src, bytes);
}

void hv_jit_finish(HvJitRegion *region, uint32_t *start, size_t words) {
    (void)region;
#if TARGET_OS_OSX
    if (!region->rw_diff) pthread_jit_write_protect_np(1);
#endif
    if (start && words) sys_icache_invalidate(start, words * sizeof(uint32_t));
}

void hv_jit_patch(HvJitRegion *region, uint32_t *const *at, const uint32_t *words, size_t n) {
    if (!n) return;
#if TARGET_OS_OSX
    if (!region->rw_diff) pthread_jit_write_protect_np(0);
#endif
    for (size_t k = 0; k < n; k++) __atomic_store_n((uint32_t *)((uint8_t *)at[k] + region->rw_diff), words[k], __ATOMIC_RELEASE);
#if TARGET_OS_OSX
    if (!region->rw_diff) pthread_jit_write_protect_np(1);
#endif
#if defined(__aarch64__)
    /* As sys_icache_invalidate does for one range (its data side needs nothing on these hosts): the stores done, each
       line out of every core's instruction cache, then the barriers once. A DSB every 16 lines, as it also does on the
       cores that need one every 20. */
    __asm__ volatile("dsb ish" ::: "memory");
    for (size_t k = 0; k < n; k++) {
        __asm__ volatile("ic ivau, %0" ::"r"((uintptr_t)at[k] & ~(uintptr_t)63) : "memory");
        if ((k & 15) == 15) __asm__ volatile("dsb ish" ::: "memory");
    }
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
#else
    for (size_t k = 0; k < n; k++) sys_icache_invalidate(at[k], 4);
#endif
}

/* As sys_icache_invalidate does it (libsystem_platform: DSB, IC IVAU per 64-byte line with a DSB after every 20 for
   the CPUs that need one, DSB, ISB), over both ranges. */
static void ic_lines(uint32_t *start, size_t words, int *n) {
    if (!start || !words) return;
    uintptr_t a = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)(start + words);
    for (; a < end; a += 64) {
        __asm__ volatile("ic ivau, %0" ::"r"(a) : "memory");
        if (++*n % 20 == 0) __asm__ volatile("dsb ish" ::: "memory");
    }
}

void hv_jit_finish2(HvJitRegion *region, uint32_t *a, size_t na, uint32_t *b, size_t nb) {
    (void)region;
#if TARGET_OS_OSX
    if (!region->rw_diff) pthread_jit_write_protect_np(1);
#endif
    int n = 0;
    __asm__ volatile("dsb ish" ::: "memory");
    ic_lines(a, na, &n);
    ic_lines(b, nb, &n);
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}

void hv_jit_finish_branch(HvJitRegion *region, uint32_t *word) {
    (void)region;
#if TARGET_OS_OSX
    if (!region->rw_diff) pthread_jit_write_protect_np(1);
#endif
    /* The store first (so a refetch finds the new word), then the line, with no wait for every CPU's cache. */
    __asm__ volatile("dsb ish\n\tic ivau, %0" ::"r"((uintptr_t)word & ~(uintptr_t)63) : "memory");
}

/* Any thread: an even number of words (blocks stay 8-byte aligned), taken atomically. */
uint32_t *hv_jit_reserve(HvJitRegion *region, size_t words) {
    words = (words + 1) & ~(size_t)1;
    size_t at = __atomic_fetch_add(&region->used, words, __ATOMIC_RELAXED);
    size_t cap = region->limit && region->limit < region->capacity ? region->limit : region->capacity;
    if (at + words > cap) return NULL; /* full: the flush resets used */
    return region->base + at;
}

void hv_jit_reset(HvJitRegion *region) {
    region->used = 0;
}
