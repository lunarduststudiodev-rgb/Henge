#include "rosetta.h"

#include <sys/sysctl.h>
#include "exception.h"
#include "machine.h"

int hv_rosetta_host_supported(void) {
    int supported = 0;
    size_t size = sizeof supported;
    return sysctlbyname("hw.optional.arm.FEAT_AFP", &supported, &size, NULL, 0) == 0 && supported;
}

/* APFLG adds P/A to flag-setting integer arithmetic. The native NZCV
   register keeps N/Z/C/V; P/A have their own per-vCPU storage, so host
   instructions used by the translator can never overwrite them. */
int hv_rosetta_integer_flags(uint32_t i) {
    return (i & 0x3f000000u) == 0x31000000u || /* ADDS/SUBS immediate */
           (i & 0x3f000000u) == 0x2b000000u || /* ADDS/SUBS register */
           (i & 0x3fe0fc00u) == 0x3a000000u || /* ADCS/SBCS */
           (i & 0x7f800000u) == 0x72000000u || /* ANDS immediate */
           (i & 0x7f000000u) == 0x6a000000u || /* ANDS/BICS register */
           (i & 0x3fe00410u) == 0x3a400000u;   /* CCMN/CCMP */
}

int hv_rosetta_fp_compare(uint32_t i) {
    return (i & 0x5f203c00u) == 0x1e202000u || /* FCMP/FCMPE */
           (i & 0x5f200c00u) == 0x1e200400u;   /* FCCMP/FCCMPE */
}

static uint64_t reg(HvCpu *c, unsigned r, int sp) {
    return r == 31 ? (sp ? c->sp : 0) : c->x[r];
}

static uint64_t mask(unsigned bits) {
    return bits == 64 ? UINT64_MAX : (1ull << bits) - 1;
}

static uint64_t ror(uint64_t x, unsigned shift, unsigned bits) {
    x &= mask(bits);
    shift &= bits - 1;
    return shift ? ((x >> shift) | (x << (bits - shift))) & mask(bits) : x;
}

static int condition(uint64_t f, unsigned cc) {
    unsigned n = f >> 31 & 1, z = f >> 30 & 1, c = f >> 29 & 1, v = f >> 28 & 1;
    int result;
    switch (cc >> 1) {
    case 0: result = z; break;
    case 1: result = c; break;
    case 2: result = n; break;
    case 3: result = v; break;
    case 4: result = c && !z; break;
    case 5: result = n == v; break;
    case 6: result = n == v && !z; break;
    default: return 1;
    }
    return result ^ (cc & 1);
}

int hv_rosetta_alu(HvCpu *c, uint32_t i, uint64_t pc) {
    unsigned bits = i >> 31 ? 64 : 32, rn = i >> 5 & 31, rd = i & 31;
    uint64_t a, b, result, flags, m = mask(bits);
    int sub = (i >> 30) & 1, compare = (i & 0x3fe00410u) == 0x3a400000u;
    int logical = (i & 0x7f800000u) == 0x72000000u || (i & 0x7f000000u) == 0x6a000000u;
    unsigned carry = 0;
    a = reg(c, rn, !logical && !compare && ((i & 0x1f000000u) == 0x11000000u || (i & 0x1fe00000u) == 0x0b200000u)) & m;
    if (compare) {
        if (!condition(c->nzcv, i >> 12 & 15)) {
            c->nzcv = (uint64_t)(i & 15) << 28;
            c->apflags = 0;
            return 0;
        }
        b = i & (1u << 11) ? (i >> 16 & 31) : reg(c, i >> 16 & 31, 0);
    } else if ((i & 0x1f000000u) == 0x11000000u) {
        b = (uint64_t)(i >> 10 & 4095) << ((i >> 22 & 1) ? 12 : 0);
    } else if ((i & 0x7f800000u) == 0x72000000u) {
        unsigned imms = i >> 10 & 63, immr = i >> 16 & 63, n = i >> 22 & 1;
        unsigned encoding = n << 6 | (~imms & 63);
        if (encoding < 2) { hv_raise_undef(c, pc); return 1; }
        unsigned len = 31u - (unsigned)__builtin_clz(encoding), width = 1u << len;
        unsigned s = imms & (width - 1);
        if (s == width - 1 || width > bits) { hv_raise_undef(c, pc); return 1; }
        b = ror(mask(s + 1), immr, width);
        for (unsigned step = width; step < bits; step *= 2) b |= b << step;
    } else {
        b = reg(c, i >> 16 & 31, 0) & m;
        if ((i & 0x3fe0fc00u) == 0x3a000000u) {
            carry = c->nzcv >> 29 & 1;
        } else if (!logical && (i & (1u << 21))) {
            unsigned opt = i >> 13 & 7, width = 8u << (opt & 3);
            b &= mask(width);
            if (opt & 4) b = (uint64_t)((int64_t)(b << (64 - width)) >> (64 - width));
            b <<= i >> 10 & 7;
        } else {
            unsigned shift = i >> 10 & 63;
            switch (i >> 22 & 3) {
            case 0: b <<= shift; break;
            case 1: b >>= shift; break;
            case 2: b = bits == 64 ? (uint64_t)((int64_t)b >> shift) : (uint32_t)((int32_t)b >> shift); break;
            default: b = ror(b, shift, bits); break;
            }
            if (logical && (i & (1u << 21))) b = ~b;
        }
    }
    b &= m;
    if (logical) {
        result = a & b;
        flags = 0;
    } else {
        if ((i & 0x3fe0fc00u) != 0x3a000000u) carry = sub;
        __uint128_t sum = (__uint128_t)a + (sub ? (b ^ m) : b) + carry;
        result = (uint64_t)sum & m;
        flags = (uint64_t)(sum >> bits) << 29;
        uint64_t overflow = (sub ? (a ^ b) : ~(a ^ b)) & (a ^ result);
        flags |= (overflow >> (bits - 1) & 1) << 28;
    }
    flags |= (result >> (bits - 1)) << 31 | (uint64_t)(result == 0) << 30;
    unsigned parity = !__builtin_parity((unsigned)(result & 255));
    unsigned adjust = logical ? 0 : (unsigned)((a ^ b ^ result) >> 4 & 1);
    c->nzcv = flags;
    c->apflags = (uint64_t)parity << 26 | (uint64_t)adjust << 27;
    if (!compare && rd != 31) c->x[rd] = result;
    return 0;
}

/* Apple AFPCR's independent DAZ/FTZ and ACTLR.PRSV map to the standard
   AFP controls on supported hosts. Keep guest FPCR separate from this
   effective value: kernel saves/restores must observe the original bits. */
void hv_rosetta_fp_apply(HvCpu *c) {
    if (!c->rosetta_mode) return;
    uint64_t value = c->fpcr & (HV_FPCR_MASK | 7); /* no trap enables on the host (cpu.h) */
    if (c->el == 0 && (c->actlr_el1 & (1u << 5))) {
        /* Rosetta also writes the architectural FPCR.FIZ/FZ controls when
           ID_AA64MMFR1 advertises AFP. Keep those controls: clearing them
           would silently ignore changes to the x86 MXCSR DAZ/FTZ bits. */
        value |= 2 | (c->afpcr_el0 & 1) | (c->afpcr_el0 >> 1 & 1) << 24;
        if (c->actlr_el1 & (1u << 6)) value |= 4;
    }
    if (value != c->fpcr_native) {
        c->fpcr_native = value;
        __asm__ volatile("msr fpcr, %0" :: "r"(value));
    }
}
