#ifndef HENGEVM_DISPATCH_H
#define HENGEVM_DISPATCH_H

#include "cpu.h"
#include "machine.h"

/* Runs translated code until the CPU halts, the test stop PC is reached, or
   something the dispatcher cannot handle happens. Returns the HV_EXIT_* reason. */
int hv_run(HvCpu *cpu);
/* Runs a single block (after interrupt checks). */
int hv_run_one(HvCpu *cpu);

/* Drops every translated block (the JIT region is reused from the start). With no CPU running; takes tr_mu itself
   (the code cache's reclaimer may be running), so never with it held. */
void hv_flush_blocks(HvMachine *m);

void hv_enter(HvCpu *cpu, void *block);
/* While hv_run runs, the host FPCR/FPSR hold the guest's; these move them in and out. */
void hv_fp_enter(HvCpu *cpu);
void hv_fp_leave(HvCpu *cpu);

void hv_debug_el0_in_kernel(HvCpu *cpu, const char *what);

/* print_stats' sample of the block hash: buckets part (0-63), part + 64, ... */
typedef struct HvHashSample {
    uint64_t used, stale, maxlen, sumsq; /* buckets with a block, stale blocks, longest chain, sum of squared lengths */
    HvBlock *worst;                      /* the longest chain's first block: NULL when no bucket held one */
} HvHashSample;
void hv_block_hash_sample(HvMachine *m, uint32_t part, HvHashSample *s);

/* For a SIGILL handler (ucontext is its third argument): if the host stopped at a guest
   instruction that translated code runs as it is, and the host CPU does not have it,
   sets the thread up to give the guest an undefined-instruction exception instead, as a
   real Mac would, and returns 1. Returns 0 for anything else (a bug of ours: let it crash). */
int hv_jit_sigill(HvMachine *m, void *ucontext);

#endif
