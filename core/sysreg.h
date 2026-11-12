#ifndef HENGEVM_SYSREG_H
#define HENGEVM_SYSREG_H

#include <stdint.h>

#include "cpu.h"

/* Architectural register constants */
#define ID_AA64PFR0  0x1100000001110011ull /* EL0/EL1 AArch64, FP+AdvSIMD with FP16, GICv3 sysregs, CSV2, CSV3 */
#define ID_AA64ISAR0 0x0011100110212120ull /* AES+PMULL, SHA1, SHA2/512, CRC32, LSE, RDM, SHA3, DotProd, FHM, FlagM */
#define ID_AA64ISAR0_ROSETTA 0x0021100110212120ull /* the same with FlagM2 (XAFLAG, AXFLAG), in Rosetta mode only */
#define ID_AA64ISAR1 0x0000001110211011ull /* DPB, PAuth (no-op), JSCVT, FCMA, LRCPC2, PACGA, FRINTTS, SB */
#define ID_AA64MMFR0 0x0000000000100025ull /* 48-bit PA, 16-bit ASID, 4K/16K/64K granules */
#define ID_AA64MMFR1 0x0000000000111000ull /* HPDS, LOR, PAN */
#define ID_AA64DFR0  0x0000000000000006ull /* Armv8 debug, no PMU */
#define MIDR         0x611f0220ull         /* Apple; a configuration constant for now */
#define CTR_EL0      0x9444c004ull         /* 64-byte lines, PIPT I-cache, IDC */
#define CLIDR        0x0a200023ull         /* L1 split, L2 unified */
#define CNTFRQ       24000000ull

/* MRS, MSR (register and immediate), and SYS (DC, IC, TLBI, AT).
   Each returns 1 if the block must leave. */
int hv_sysreg_mrs(HvCpu *cpu, uint32_t insn, uint64_t pc);
int hv_sysreg_msr(HvCpu *cpu, uint32_t insn, uint64_t pc);
int hv_sysreg_sys(HvCpu *cpu, uint32_t insn, uint64_t pc);

/* The guest's counter (CNTPCT): host mach_absolute_time, which runs at 24 MHz. */
uint64_t hv_counter(void);
/* Host ticks spent paused: guest counter = mach_absolute_time() - hv_time_bias. A guest deadline is
   due at host time deadline + hv_time_bias. Changed only while every CPU is paused. */
extern uint64_t hv_time_bias;

/* One TLBI on one CPU's own TLB and branch cache. */
void hv_tlbi_apply(HvCpu *cpu, uint32_t op1, uint32_t crn, uint32_t crm, uint32_t op2, uint64_t xt);

#endif
