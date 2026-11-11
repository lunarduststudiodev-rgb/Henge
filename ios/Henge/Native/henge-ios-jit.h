/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Henge -- iOS dual-mapped JIT memory for QEMU's TCG.
 *
 * Derived from AetherPS4-iOS's src/core/ios/ios_jit_allocator.cpp
 * (shadPS4 Emulator Project, GPL-2.0-or-later), ported from C++ to C and
 * reshaped to hand QEMU's tcg/region.c a split-W^X code buffer.
 *
 * ---------------------------------------------------------------------------
 * Why this file exists
 * ---------------------------------------------------------------------------
 * On iOS 27 every supported device enforces TXM, so an app cannot map RWX or
 * mprotect anything to PROT_EXEC. Executable memory can only be granted by an
 * attached debugger. StikDebug provides that service over a breakpoint-based
 * RPC (BreakpointJIT.framework):
 *
 *     BreakGetJITMapping(NULL, size)   ->  brk #0xf00d, x16 = 1
 *
 * StikDebug catches the trap, issues debugserver `_M<size>,rx` to allocate an
 * executable region inside us, then writes one byte through the debugger into
 * every 16 KiB page of it (`M<addr>,1:69`) -- which is what actually makes the
 * pages usable -- and finally stuffs the address back into x0 before resuming.
 *
 * We then make our own writable alias of the same physical pages with vm_remap.
 * QEMU emits code through the RW alias and executes through the RX alias;
 * tcg/region.c already understands exactly this shape via tcg_splitwx_diff.
 *
 * ---------------------------------------------------------------------------
 * The one-shot rule
 * ---------------------------------------------------------------------------
 * After setup the app calls BreakJITDetach() and StikDebug goes away. From then
 * on a BreakGetJITMapping trap is NOT serviced -- and an unserviced `brk` is a
 * fatal SIGTRAP, not a failed call. StikDebug can also be killed mid-session by
 * iOS's background wake-rate limiter.
 *
 * So: allocate ONCE, up front, at a size that is provably sufficient for the
 * whole session, and never plan on getting a second region. For QEMU this is a
 * natural fit -- TCG sizes its translation buffer once at init from
 * `-accel tcg,tb-size=N` and flushes rather than grows when it fills.
 *
 * henge_ios_jit_install_trap_handler() must be called early in app startup so
 * that a missed trap degrades into a NULL return instead of killing the process.
 */

#ifndef HENGE_IOS_JIT_H
#define HENGE_IOS_JIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* See henge-display.h for why this is necessary. */
#define HENGE_EXPORT __attribute__((visibility("default")))

typedef struct HengeDualMapping {
    uint8_t *rw_addr;   /* writable alias  -- emit code here            */
    uint8_t *rx_addr;   /* executable alias -- branch here              */
    size_t   size;
} HengeDualMapping;

/*
 * Install SIGTRAP/SIGBUS handlers that skip an unserviced BreakpointJIT trap
 * (pc += 4, x0 = 0) instead of letting it kill the process. Call once, early,
 * before any JIT allocation. Safe to call when StikDebug is absent.
 */
HENGE_EXPORT void henge_ios_jit_install_trap_handler(void);

/*
 * Allocate one dual-mapped region of `bytes`. Returns a mapping whose rw_addr is
 * NULL on failure. `bytes` is rounded up to a 16 KiB page multiple by the caller's
 * contract -- StikDebug prepares whole pages.
 */
HengeDualMapping henge_ios_jit_allocate(size_t bytes);

/*
 * Claim the JIT region now, at app launch, and hold it until QEMU asks.
 * StikDebug does not stay attached forever, and a first run spends a minute
 * downloading the guest before QEMU starts -- by which time the debugger has
 * let go and no executable memory can be had at all. Pass the same size QEMU
 * will ask for (tb-size).
 */
HENGE_EXPORT bool henge_ios_jit_prewarm(size_t bytes);
HENGE_EXPORT bool henge_ios_jit_prewarm_self(size_t bytes);

/*
 * A debugger Henge asked for has just attached: let the next prewarm try again if the last one failed only because no
 * debugger answered its probe. Any other failure stays final for this launch.
 */
HENGE_EXPORT void henge_ios_jit_prewarm_rearm(void);

/* The held region, or NULL when there is none. */
HENGE_EXPORT HengeDualMapping *henge_ios_jit_get_mapping(void);

/*
 * The native runtime shares the prewarmed region with QEMU: it takes bytes from the bottom, QEMU the top. Call before
 * writing anything below `end` (an offset from the region's start); false means QEMU holds that part.
 */
HENGE_EXPORT bool henge_ios_jit_reserve_bottom(size_t end);

/* Release a mapping obtained from henge_ios_jit_allocate(). */
void henge_ios_jit_release(HengeDualMapping *m);

/*
 * Tell StikDebug to detach. RX mappings already obtained stay valid and
 * executable; no further allocation is possible afterwards.
 */
HENGE_EXPORT void henge_ios_jit_detach(void);

/* True once a region has passed its execute self-test -- i.e. JIT is genuinely live. */
HENGE_EXPORT bool henge_ios_jit_is_available(void);

/*
 * Whether this process can execute memory it wrote itself, through a plain
 * MAP_JIT mapping and TCG's own W^X toggle -- the route every other iOS
 * emulator uses, and the one QEMU falls back to when the dual mapping is
 * unavailable.
 *
 * Measured, not inferred: it maps a page, writes two instructions into it,
 * calls them and checks the answer, with a guard around the call so a page that
 * turns out not to be executable fails this test instead of killing the app.
 * The result is cached after the first call.
 *
 * This exists because the question "does this device need a trap servicer?" was
 * previously answered from the device model and the iOS version, and that guess
 * is wrong on at least one real combination -- iOS 26, where StikDebug attaches
 * but services no traps because MAP_JIT works and it does not need to.
 */
HENGE_EXPORT bool henge_ios_jit_mapjit_works(void);

/*
 * When the region was made without a trap servicer (before iOS 26: TrollStore, a jailbreak, or a debugger that has let go),
 * which way: "MAP_JIT" or "plain". NULL otherwise.
 */
HENGE_EXPORT const char *henge_ios_jit_self_route(void);

/*
 * Log the process's phys_footprint -- the number jetsam actually kills on.
 * `tag` labels the call site in the log.
 */
HENGE_EXPORT void henge_ios_jit_log_footprint(const char *tag);

/*
 * Bytes this process may still allocate before jetsam kills it. Used to size the
 * guest's RAM to the device rather than to a guess.
 */
HENGE_EXPORT size_t henge_ios_available_memory(void);

#ifdef __cplusplus
}
#endif

#endif /* HENGE_IOS_JIT_H */
