#ifndef HENGEVM_ROSETTA_H
#define HENGEVM_ROSETTA_H

#include "cpu.h"

#define HV_APFLAGS_MASK (3ull << 26)

int hv_rosetta_host_supported(void);
int hv_rosetta_integer_flags(uint32_t insn);
int hv_rosetta_fp_compare(uint32_t insn);
int hv_rosetta_alu(HvCpu *cpu, uint32_t insn, uint64_t pc);
void hv_rosetta_fp_apply(HvCpu *cpu);

#endif
