#ifndef HENGEVM_SYSREG_H
#define HENGEVM_SYSREG_H

#include <stdint.h>

#include "cpu.h"

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
