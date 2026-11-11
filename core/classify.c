#include "classify.h"

/* Decoding follows the A64 top-level encoding table in the Arm ARM:
   bits 28..25 pick the group, then each group is narrowed by mask and value. */

static HvInsnKind classify_hint(uint32_t insn) {
    /* HINT #imm: CRm in 11..8, op2 in 7..5. */
    uint32_t crm = (insn >> 8) & 0xf;
    uint32_t op2 = (insn >> 5) & 0x7;
    uint32_t imm = (crm << 3) | op2;
    switch (imm) {
    case 2:  /* WFE */
    case 3:  /* WFI */
        return HV_INSN_WAIT;
    case 7:  /* XPACLRI */
    case 8: case 10: case 12: case 14:   /* PACIA1716, PACIB1716, AUTIA1716, AUTIB1716 */
    case 24: case 25: case 26: case 27:  /* PACIAZ, PACIASP, PACIBZ, PACIBSP */
    case 28: case 29: case 30: case 31:  /* AUTIAZ, AUTIASP, AUTIBZ, AUTIBSP */
        return HV_INSN_PAC;
    default:
        /* NOP, YIELD, SEV, SEVL, ESB, PSB, CSDB, BTI and unallocated hints run as NOPs. */
        return HV_INSN_COPY;
    }
}

static HvInsnKind classify_system(uint32_t insn) {
    uint32_t l = (insn >> 21) & 1;
    uint32_t op0 = (insn >> 19) & 3;
    uint32_t op1 = (insn >> 16) & 7;
    uint32_t crn = (insn >> 12) & 0xf;

    if (op0 == 0) {
        if (l == 0 && crn == 2 && op1 == 3 && (insn & 0x1f) == 0x1f) return classify_hint(insn);
        if (l == 0 && crn == 3 && op1 == 3 && (insn & 0x1f) == 0x1f) {
            uint32_t op2 = (insn >> 5) & 7;
            /* CLREX, DSB, DMB, ISB, SB and the DSB nXS forms. */
            (void)op2;
            return HV_INSN_BARRIER;
        }
        if (insn == 0xd500401f) return HV_INSN_COPY;    /* CFINV (FlagM, offered): flips C, which is the host's */
        if (l == 0 && crn == 4) return HV_INSN_SYSREG; /* MSR (immediate) to PSTATE */
        if (l == 0 && crn == 1 && op1 == 3 && (insn & 0x1f) == 0x1f) {
            /* WFET, WFIT (FEAT_WFxT) live in CRn=1. */
            return HV_INSN_WAIT;
        }
        return HV_INSN_UNDEFINED;
    }
    if (op0 == 1) return HV_INSN_SYS;     /* SYS, SYSL */
    return HV_INSN_SYSREG;                /* MRS, MSR (register) */
}

static HvInsnKind classify_branch_group(uint32_t insn) {
    if ((insn & 0x7c000000) == 0x14000000) return HV_INSN_BRANCH;   /* B, BL */
    if ((insn & 0x7e000000) == 0x34000000) return HV_INSN_BRANCH;   /* CBZ, CBNZ */
    if ((insn & 0x7e000000) == 0x36000000) return HV_INSN_BRANCH;   /* TBZ, TBNZ */
    if ((insn & 0xff000000) == 0x54000000) return HV_INSN_BRANCH;   /* B.cond, BC.cond */
    if ((insn & 0xff000000) == 0xd4000000) return HV_INSN_EXCEPTION;
    if ((insn & 0xffc00000) == 0xd5000000) return classify_system(insn);
    if ((insn & 0xfe000000) == 0xd6000000) {
        uint32_t opc = (insn >> 21) & 0xf;
        if (opc == 4) return HV_INSN_ERET;                          /* ERET, ERETAA, ERETAB */
        if (opc == 5) return HV_INSN_UNDEFINED;                     /* DRPS: not at EL0/EL1 */
        return HV_INSN_BRANCH_REG;                                  /* BR, BLR, RET and PAC forms */
    }
    return HV_INSN_UNDEFINED;
}

static HvInsnKind classify_memory(uint32_t insn) {
    /* Load register (literal): bits 29..27 = 011, bit 24 = 0. */
    if ((insn & 0x3b000000) == 0x18000000) {
        uint32_t opc = insn >> 30;
        uint32_t v = (insn >> 26) & 1;
        if (opc == 3 && v == 0) return HV_INSN_PREFETCH;            /* PRFM (literal) */
        return HV_INSN_LOAD_LITERAL;
    }
    /* PRFM (immediate) and PRFM (register), PRFUM: size=11, V=0, opc=10. */
    if ((insn & 0xffc00000) == 0xf9800000) return HV_INSN_PREFETCH;            /* PRFM (unsigned offset) */
    if ((insn & 0xffe00c00) == 0xf8a00800) return HV_INSN_PREFETCH;            /* PRFM (register) */
    if ((insn & 0xffe00c00) == 0xf8800000) return HV_INSN_PREFETCH;            /* PRFUM */
    return HV_INSN_MEMORY;
}

/* Copied instructions run as they are on the host, so an encoding the architecture leaves
   unallocated, or one from a feature the guest is not told about (ID registers in
   sysreg.c), must not be copied: the host would either stop the whole app with SIGILL or,
   on a newer chip that has the feature, run it (operands.c knows the register fields of
   allocated forms only, so a reserved register could be used as it stands). The integer
   groups and the SIMD/FP forms that name general registers are decoded exactly here, as in
   the Arm ARM; any other SIMD/FP encoding has no general register, and if the host finds it
   unallocated, hv_jit_sigill (dispatch.c) gives the guest its exception. */

/* DecodeBitMasks(N, imms, immr, immediate = TRUE) succeeds: a logical immediate exists. */
static int bitmask_ok(uint32_t n, uint32_t imms, int sf) {
    if (!sf && n) return 0;
    uint32_t v = (n << 6) | (~imms & 0x3f); /* N:NOT(imms) */
    if (!v) return 0;
    int len = 31 - __builtin_clz(v);
    if (len < 1) return 0;
    uint32_t levels = (1u << len) - 1;
    return (imms & levels) != levels;
}

static int dp_immediate_ok(uint32_t insn) {
    uint32_t sf = insn >> 31, op = (insn >> 23) & 7, opc = (insn >> 29) & 3, n = (insn >> 22) & 1;
    uint32_t immr = (insn >> 16) & 0x3f, imms = (insn >> 10) & 0x3f;
    switch (op) {
    case 0: case 1: case 2: return 1;          /* ADR/ADRP, add/sub (immediate) */
    case 3: return 0;                          /* with tags (MTE), min/max (CSSC): not offered */
    case 4: return bitmask_ok(n, imms, sf);    /* logical (immediate) */
    case 5: return opc != 1 && (sf || !((insn >> 22) & 1)); /* move wide: hw 2-3 only for 64 bits */
    case 6: return opc != 3 && n == sf && (sf || (immr < 32 && imms < 32)); /* bitfield */
    default: return opc == 0 && !((insn >> 21) & 1) && n == sf && (sf || imms < 32); /* extract */
    }
}

static int dp_register_ok(uint32_t insn) {
    uint32_t sf = insn >> 31, op = (insn >> 30) & 1, s = (insn >> 29) & 1, op1 = (insn >> 28) & 1;
    uint32_t op2 = (insn >> 21) & 0xf, op3 = (insn >> 10) & 0x3f;
    uint32_t imm6 = op3, rn = (insn >> 5) & 0x1f;
    if (!op1) {
        if (!(op2 & 8)) return sf || imm6 < 32;                                  /* logical (shifted) */
        if (!(op2 & 1)) return ((insn >> 22) & 3) != 3 && (sf || imm6 < 32);    /* add/sub (shifted) */
        return ((insn >> 22) & 3) == 0 && ((insn >> 10) & 7) <= 4;               /* add/sub (extended) */
    }
    if (op2 & 8) { /* 3 source */
        uint32_t op54 = (insn >> 29) & 3, op31 = (insn >> 21) & 7, o0 = (insn >> 15) & 1;
        if (op54) return 0;
        switch (op31) {
        case 0: return 1;                    /* MADD, MSUB */
        case 1: case 5: return sf;           /* SMADDL, SMSUBL, UMADDL, UMSUBL */
        case 2: case 6: return sf && !o0;    /* SMULH, UMULH */
        default: return 0;
        }
    }
    switch (op2) {
    case 0x0:
        if (op3 == 0) return 1;                                                  /* ADC, ADCS, SBC, SBCS */
        if ((op3 & 0x1f) == 1) return sf && !op && s && !((insn >> 4) & 1);      /* RMIF */
        if ((op3 & 0xf) == 2) return !sf && !op && s && !((insn >> 15) & 0x3f) && (insn & 0x1f) == 0xd; /* SETF8, SETF16 */
        return 0;
    case 0x2: return s && !((insn >> 10) & 1) && !((insn >> 4) & 1);             /* conditional compare */
    case 0x4: return !s && ((insn >> 11) & 1) == 0;                              /* conditional select */
    case 0x6:
        if (op) { /* 1 source */
            uint32_t opcode2 = (insn >> 16) & 0x1f, opcode = op3;
            if (s) return 0;
            if (opcode2 == 0) return opcode <= 2 || (opcode == 3 && sf) || opcode == 4 || opcode == 5; /* RBIT..CLS */
            if (opcode2 == 1 && sf) { /* PAC, AUT, XPAC */
                if (opcode < 8) return 1;
                if (opcode < 16) return rn == 31;     /* the Z forms */
                return opcode <= 17 && rn == 31;      /* XPACI, XPACD */
            }
            return 0;
        }
        if (s) return 0; /* SUBPS (MTE) */
        switch (op3) {
        case 2: case 3: case 8: case 9: case 10: case 11: return 1;             /* UDIV, SDIV, shifts */
        case 12: return sf;                                                      /* PACGA */
        case 16: case 17: case 18: case 20: case 21: case 22: return !sf;        /* CRC32(C) B, H, W */
        case 19: case 23: return sf;                                             /* CRC32(C)X */
        default: return 0;                                                       /* SUBP, IRG, GMI (MTE), min/max (CSSC) */
        }
    default: return 0;
    }
}

/* The SIMD/FP forms that move values to or from general registers; 1 if allocated
   (and offered), 0 if not, -1 if insn is not one of them. */
static int simd_gpr_ok(uint32_t insn) {
    uint32_t sf = insn >> 31, s = (insn >> 29) & 1, type = (insn >> 22) & 3;
    uint32_t rmode = (insn >> 19) & 3, opcode = (insn >> 16) & 7;
    if ((insn & 0x5f20fc00) == 0x1e200000) { /* conversion between floating-point and integer */
        if (s) return 0;
        if (type == 2) return sf && rmode == 1 && opcode >= 6;                    /* FMOV Xd, Vn.D[1] and back */
        switch (rmode) {
        case 0:
            if (opcode < 6) return 1;                                              /* FCVTN*, SCVTF, UCVTF, FCVTA* */
            return type == 3 || sf == (type == 1);                                 /* FMOV between same sizes */
        case 3:
            if (opcode < 2) return 1;                                              /* FCVTZS, FCVTZU */
            return opcode == 6 && !sf && type == 1;                               /* FJCVTZS */
        default: return opcode < 2;                                                /* FCVTP*, FCVTM* */
        }
    }
    if ((insn & 0x5f200000) == 0x1e000000) { /* conversion between floating-point and fixed-point */
        uint32_t scale = (insn >> 10) & 0x3f;
        if (s || type == 2 || (!sf && scale < 32)) return 0;
        return (rmode == 0 && (opcode == 2 || opcode == 3)) || (rmode == 3 && opcode < 2);
    }
    if ((insn & 0x9fe08400) == 0x0e000400) { /* Advanced SIMD copy */
        uint32_t q = (insn >> 30) & 1, imm5 = (insn >> 16) & 0x1f, imm4 = (insn >> 11) & 0xf;
        int size = imm5 & 1 ? 0 : imm5 & 2 ? 1 : imm5 & 4 ? 2 : imm5 & 8 ? 3 : -1;
        if (size < 0) return 0;
        if ((insn >> 29) & 1) return q;                   /* INS (element): imm4 is an index */
        switch (imm4) {
        case 0: return size < 3 || q;                     /* DUP (element) */
        case 1: return size < 3 || q;                     /* DUP (general) */
        case 3: return q;                                 /* INS (general) */
        case 5: return q ? size < 3 : size < 2;           /* SMOV */
        case 7: return q ? size == 3 : size < 3;          /* UMOV */
        default: return 0;
        }
    }
    return -1;
}

static HvInsnKind classify_dp_register(uint32_t insn) {
    if (!dp_register_ok(insn)) return HV_INSN_UNDEFINED;
    /* Data-processing (1 source) with opcode2 = 00001: PAC*, AUT*, XPAC*. */
    if ((insn & 0xffff0000) == 0xdac10000) return HV_INSN_PAC;
    /* PACGA: Data-processing (2 source), opcode 001100. */
    if ((insn & 0xffe0fc00) == 0x9ac03000) return HV_INSN_PAC;
    return HV_INSN_COPY;
}

HvInsnKind hv_classify(uint32_t insn) {
    uint32_t op0 = (insn >> 25) & 0xf;
    /* 00xx: UDF and SME (0000), SVE (0010), unallocated. None are exposed to the guest. */
    if ((op0 & 0xc) == 0x0) return HV_INSN_UNDEFINED;
    if ((op0 & 0xe) == 0x8) {                   /* 100x: data processing, immediate */
        if ((insn & 0x1f000000) == 0x10000000) return HV_INSN_PCREL;
        return dp_immediate_ok(insn) ? HV_INSN_COPY : HV_INSN_UNDEFINED;
    }
    if ((op0 & 0xe) == 0xa) return classify_branch_group(insn); /* 101x */
    if ((op0 & 0x5) == 0x4) return classify_memory(insn);       /* x1x0 */
    if ((op0 & 0x7) == 0x5) return classify_dp_register(insn);  /* x101 */
    if ((op0 & 0x7) == 0x7) return simd_gpr_ok(insn) == 0 ? HV_INSN_UNDEFINED : HV_INSN_COPY; /* x111: SIMD and FP */
    return HV_INSN_UNDEFINED;
}

const char *hv_insn_kind_name(HvInsnKind kind) {
    switch (kind) {
    case HV_INSN_COPY: return "copy";
    case HV_INSN_PCREL: return "pcrel";
    case HV_INSN_BRANCH: return "branch";
    case HV_INSN_BRANCH_REG: return "branch-reg";
    case HV_INSN_ERET: return "eret";
    case HV_INSN_LOAD_LITERAL: return "load-literal";
    case HV_INSN_MEMORY: return "memory";
    case HV_INSN_PREFETCH: return "prefetch";
    case HV_INSN_EXCEPTION: return "exception";
    case HV_INSN_SYSREG: return "sysreg";
    case HV_INSN_SYS: return "sys";
    case HV_INSN_BARRIER: return "barrier";
    case HV_INSN_WAIT: return "wait";
    case HV_INSN_PAC: return "pac";
    case HV_INSN_UNDEFINED: return "undefined";
    }
    return "?";
}
