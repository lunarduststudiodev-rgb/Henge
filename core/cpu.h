#ifndef HENGEVM_CPU_H
#define HENGEVM_CPU_H

#include <stddef.h>
#include <stdint.h>

struct HvMachine;

enum HvHaltReason {
    HV_HALT_NONE, HV_HALT_SHUTDOWN, HV_HALT_RESET, HV_HALT_HOST, HV_HALT_PANIC
};

/* One software TLB entry. host = guest VA + addend. The exec tag is the guest
   VA of the 16 KB page. The read and write tags are that minus HV_TLB_ADJ, so
   translated code checks "same page, and the access ends in it" for any access
   of up to 16 bytes with one subtract and shift: VA - tag = offset + 15 must
   be below 16 KB. (An access in a page's last 15 bytes takes the slow path.)
   Invalid tags are HV_TLB_INVALID, which no address minus it brings near 0. */
typedef struct HvTlbEntry {
    uint64_t read_tag;
    uint64_t write_tag;
    uint64_t exec_tag;
    int64_t addend;
} HvTlbEntry;

/* exec_tag is the page's VA and its addend in one word: a block chained across pages checks
   the mapping is what it was with one comparison. Given the addend it gives the VA back. */
static inline uint64_t hv_exec_key(uint64_t page_va, int64_t addend) {
    return page_va ^ ((uint64_t)addend >> 20 | (uint64_t)addend << 44);
}

#define HV_TLB_PAGE_BITS 14
#define HV_TLB_ADJ 15
#define HV_TLB_INVALID 0x8080808080808080ull /* what memset(0x80) leaves */
#define HV_TLB_BITS 14 /* 16384 entries per EL: 256 MB of 16 KB pages (xnu's working set overflowed 64 MB) */
#define HV_TLB_SIZE (1u << HV_TLB_BITS)
#define HV_IJC_TABLES 16
#define HV_RET_BITS 13 /* entries in a return table */
#define HV_RET_TAB_BYTES ((1u << HV_RET_BITS) * 16)
#define HV_CPU_RET_TAB 0x201000 /* just past the TLB */

/* Guest CPU state. While a translated block runs, host x28 points here and the
   guest's x0..x17, x19..x27, x29 and x30 live in the same host registers.
   Guest x18, x28 and SP stay in this struct: Darwin reserves host x18, and the
   translator owns host x28 and the host SP. */
typedef struct HvCpu {
    /* Fixed layout: translated code and host/enter.S address these by offset. */
    uint64_t x[31];          /* 0x000 */
    uint64_t sp;             /* 0x0f8: the guest SP for the current EL and SPSel */
    uint64_t pc;             /* 0x100 */
    uint64_t nzcv;           /* 0x108: N, Z, C, V in bits 31..28 */
    uint64_t exit_routine;   /* 0x110: host address blocks branch to when they leave */
    uint64_t spill[4];       /* 0x118: host scratch registers saved around a rewrite */
    uint32_t exit_reason;    /* 0x138 */
    uint32_t exit_insn;      /* 0x13c */
    __uint128_t v[32];       /* 0x140 */
    uint64_t fpcr;           /* 0x340 */
    uint64_t fpsr;           /* 0x348 */
    uint64_t helper_routine; /* 0x350: hv_helper_entry */
    uint64_t helper_ret;     /* 0x358: where hv_helper_entry returns into the block */
    /* System registers translated code reads and writes directly. */
    uint64_t tpidr_el0;      /* 0x360 */
    uint64_t tpidrro_el0;    /* 0x368 */
    uint64_t tpidr_el1;      /* 0x370 */
    uint64_t elr_el1;        /* 0x378 */
    uint64_t spsr_el1;       /* 0x380 */
    uint64_t sp_el0;         /* 0x388: banked SP_EL0 while SPSel is 1 */
    uint64_t daif;           /* 0x390: D, A, I, F in bits 9..6, as in SPSR */
    uint64_t sp_el1;         /* 0x398: banked SP_EL1 while SPSel is 0 */
    uint64_t pstate_extra;   /* 0x3a0: PAN (bit 22), UAO (23), SSBS (12), DIT (24), TCO (25) */
    uint64_t sctlr_el1;      /* 0x3a8 */
    uint64_t esr_el1;        /* 0x3b0 */
    uint64_t far_el1;        /* 0x3b8 */
    uint32_t exit_req;       /* 0x3c0: set (by another thread too) to make chained blocks leave */
    uint32_t el0_cnt;        /* 0x3c4: CNTKCTL_EL1 bits 1..0 (EL0VCTEN, EL0PCTEN), for translated counter reads */
    uint64_t last_stub;      /* 0x3c8: slot of the unchained exit just taken, or 0 */
    uint64_t ijc;            /* 0x3d0: indirect-branch cache, HV_IJC_TABLES tables of {tag, entry} */
    uint64_t ijc_salt[2];    /* 0x3d8: tags are VA ^ salt, [0] for user (EL0) tables, [1] kernel (see hv_ijc_* in mmu.c) */
    uint64_t miss_site;      /* 0x3e8: --stats: guest PC of the branch whose cache lookup just missed */
    uint64_t excl_addr;      /* 0x3f0: exclusive monitor: guest VA, or ~0 when clear */
    uint32_t excl_size;      /* 0x3f8: bytes the LDXR read */
    uint32_t spsel;          /* 0x3fc: SPSel (translated MSR SPSel writes it) */
    uint64_t excl_val[2];    /* 0x400: what it read (a pair's two halves) */
    uint64_t host_sp;        /* 0x410: the host's stack while blocks run: they keep the guest SP in the SP register */
    uint64_t lines_ptr;      /* 0x418: &pc_state->irq_line (irq, then fiq: one 8-byte load sees both) */
    uint64_t helper_thunk;   /* 0x420: hv_helper_thunk (blocks reach it through a register: it may be out of BL range) */
    uint64_t chain_thunk;    /* 0x428: hv_chain_thunk */
    uint64_t ijc_thunk;      /* 0x430: hv_ijc_thunk */
    uint64_t refill_ret;     /* 0x438: where it goes back to */
    uint64_t ijc_tab[16];    /* 0x440: each flags value's branch-cache table (ijc + flags * table size) */
    uint64_t gaddend[4];     /* 0x4c0: host addends of the pages groups of accesses on one base register were checked in (translate_mem) */
    uint64_t xjump_thunk;    /* 0x4e0: hv_xjump_thunk */
    uint64_t tcr_el1;        /* 0x4e8: here, in reach of one load, for translated MRS (xnu reads it ~17K times a second at login) */
    uint64_t par_el1;        /* 0x4f0: likewise, read after each AT */
    uint64_t cntvoff;        /* 0x4f8: the virtual counter is the host's less this (m->cntvoff), read inline */
    /* 0x500: what tags in the return tables (ret_tab) are xored with, [0] user, [1] kernel.
       Bits 63..48, as ijc_salt; a new value empties that half of the tables. */
    uint64_t ret_salt[2] __attribute__((aligned(128)));
    uint64_t helper_came_from; /* 0x510: where hv_helper_entry was called from, to tell if it goes back there */
    /* 0x580: [0] EL0 and unprivileged accesses, [1] EL1, [2] EL1 with PSTATE.PAN set,
       so toggling PAN (the kernel does it around every user copy) flushes nothing.
       [slot][EL0, EL1, EL1 with PAN, unused]: a slot's three in one 128-byte cache line. */
    HvTlbEntry tlb[HV_TLB_SIZE * 4] __attribute__((aligned(128))); /* four tables; where an entry is: hv_tlb_code */
    /* Each flags value's return table: {tag, host address} of the code after a call, by the
       return address's low bits (see put_call). At a multiple of 4 KB from the HvCpu, as the
       translated code adds it with one ADD. */
    char ret_pad[HV_CPU_RET_TAB - 0x580 - (HV_TLB_SIZE * 4 * 32)];
    uint8_t ret_tab[HV_IJC_TABLES][HV_RET_TAB_BYTES];

    /* The rest is only touched by C. */
    struct HvMachine *m;
    uint32_t el;

    /* The exclusive monitor is excl_addr, excl_size and excl_val above: STXR is a
       compare-and-swap against the value the LDXR saw, the way QEMU does it, so
       another CPU's store in between makes it fail. */

    uint64_t ttbr0_el1;
    uint64_t ttbr1_el1;
    uint64_t mair_el1;
    uint64_t amair_el1;
    uint64_t vbar_el1;
    uint64_t afsr0_el1, afsr1_el1;
    uint64_t contextidr_el1;
    uint64_t cpacr_el1;
    uint64_t mdscr_el1;
    uint64_t csselr_el1;
    uint64_t cntkctl_el1;
    uint64_t actlr_el1;
    uint64_t sctlr_seen;     /* MMU/FP state the current TLB contents were filled under */
    uint64_t timer_due;      /* host time this CPU's timer next fires (the dispatcher's last tick said), 0: none */
    /* TLB slots (table << HV_TLB_BITS | index) filled with user addresses since the
       last user flush, so a context switch clears those instead of scanning all
       three tables. More than HV_USER_FILLS means: scan. */
#define HV_USER_FILLS 2048
    uint16_t user_fills[HV_USER_FILLS];
    uint32_t nuser_fills;
    /* The same for kernel entries, so a whole-TLB flush (macOS 27 asks for hundreds a second) clears
       what was filled since the last one instead of all four tables (2 MB). More than HV_KERNEL_FILLS: all. */
#define HV_KERNEL_FILLS 4096
    uint16_t kernel_fills[HV_KERNEL_FILLS];
    uint32_t nkernel_fills;
    /* ASIDs given a user id since the user half was last started over: that drop clears just these. */
#define HV_ASID_LOG 512
    uint16_t asid_log[HV_ASID_LOG];
    uint32_t nasid_log;
    /* Indirect-branch cache salts live in bits 63..48, which no user VA sets and
       every kernel VA has all ones in, so each salt value names a distinct set
       of tags. User salts are per ASID (asid_cid[ASID], 0 = none yet), so a
       process switch keeps every process's entries; dropping one means giving
       its ASID a fresh id. Ids only grow until they run out, then the tables
       are cleared. */
    uint16_t asid_cid[65536];
    uint16_t cid_asid[65536]; /* --stats: the ASID a user id was given to (to tell live tags from dead ones) */
    /* The translation table base asid_cid[ASID] was given under: an ASID back with another table (xnu gives one hardware
       ASID to more than one address space, and may follow a switch between them with a whole-TLB flush rather than one
       by ASID) gets a fresh id, as the TLB stashes are keyed by the whole TTBR0. */
    uint64_t asid_base[65536];
    uint32_t next_cid, kernel_gen;
    uint64_t ttbr0_changed_at; /* --stats: host time of the last TTBR0 change */

    /* Generic timer: physical and virtual. The counter is host time scaled to 24 MHz. */
    uint64_t cntv_ctl, cntv_cval;
    uint64_t cntp_ctl, cntp_cval;

    uint64_t mpidr;
    uint32_t index;          /* which CPU: m->percpu[index] holds its caches and interrupt lines */
    struct HvPerCpu *pc_state;
    uint64_t icount;         /* blocks entered, for tests and stats */
    _Atomic int halted;      /* HvHaltReason; published across CPU and UI threads */
    int off;                 /* PSCI: powered off (a secondary not started yet, or CPU_OFF) until CPU_ON */
    int parked_off;          /* off, and its thread is waiting (so CPU_ON may reset it) */
} HvCpu;

#define HV_CPU_X(n)        ((n) * 8)
#define HV_CPU_SP          0x0f8
#define HV_CPU_PC          0x100
#define HV_CPU_NZCV        0x108
#define HV_CPU_EXIT        0x110
#define HV_CPU_SPILL(n)    (0x118 + (n) * 8)
#define HV_CPU_REASON      0x138
#define HV_CPU_V           0x140
#define HV_CPU_FPCR        0x340
#define HV_CPU_FPSR        0x348
#define HV_CPU_HELPER      0x350
#define HV_CPU_HELPER_RET  0x358
#define HV_CPU_TPIDR_EL0   0x360
#define HV_CPU_TPIDRRO_EL0 0x368
#define HV_CPU_TPIDR_EL1   0x370
#define HV_CPU_ELR_EL1     0x378
#define HV_CPU_SPSR_EL1    0x380
#define HV_CPU_SP_EL0      0x388
#define HV_CPU_DAIF        0x390
#define HV_CPU_PSTATE_EXTRA 0x3a0
#define HV_CPU_SCTLR_EL1   0x3a8
#define HV_CPU_ESR_EL1     0x3b0
#define HV_CPU_FAR_EL1     0x3b8
#define HV_CPU_EXIT_REQ    0x3c0
#define HV_CPU_EL0_CNT     0x3c4
#define HV_CPU_LAST_STUB   0x3c8
#define HV_CPU_IJC         0x3d0
#define HV_CPU_IJC_SALT    0x3d8
#define HV_CPU_MISS_SITE   0x3e8
#define HV_CPU_EXCL_ADDR   0x3f0
#define HV_CPU_EXCL_SIZE   0x3f8
#define HV_CPU_EXCL_VAL    0x400
#define HV_CPU_HOST_SP     0x410
#define HV_CPU_LINES_PTR   0x418
#define HV_CPU_HELPER_THUNK 0x420
#define HV_CPU_CHAIN_THUNK 0x428
#define HV_CPU_IJC_THUNK   0x430
#define HV_CPU_REFILL_RET  0x438
#define HV_CPU_SPSEL       0x3fc
#define HV_CPU_SP_EL1      0x398
#define HV_CPU_IJC_TAB     0x440
#define HV_CPU_TLB         0x580
#define HV_CPU_RET_SALT    0x500
#define HV_CPU_GADDEND     0x4c0
#define HV_CPU_XJUMP_THUNK 0x4e0
#define HV_CPU_TCR_EL1     0x4e8
#define HV_CPU_PAR_EL1     0x4f0
#define HV_CPU_CNTVOFF     0x4f8

/* Indirect-branch cache: one table per block-flags value, 64K entries of 16 bytes,
   indexed by ((va ^ va >> 16) >> 2) so code at the same offset in different pages spreads out. */
#define HV_IJC_BITS 18
#define HV_UCODE_BITS 18
#define HV_IJC_TABLE_BYTES ((1u << HV_IJC_BITS) * 16)
#define HV_CPU_TLB_EL      32 /* between a slot's tables */

_Static_assert(offsetof(HvCpu, sp) == HV_CPU_SP, "HvCpu layout");
_Static_assert(offsetof(HvCpu, pc) == HV_CPU_PC, "HvCpu layout");
_Static_assert(offsetof(HvCpu, nzcv) == HV_CPU_NZCV, "HvCpu layout");
_Static_assert(offsetof(HvCpu, exit_routine) == HV_CPU_EXIT, "HvCpu layout");
_Static_assert(offsetof(HvCpu, spill) == HV_CPU_SPILL(0), "HvCpu layout");
_Static_assert(offsetof(HvCpu, exit_reason) == HV_CPU_REASON, "HvCpu layout");
_Static_assert(offsetof(HvCpu, v) == HV_CPU_V, "HvCpu layout");
_Static_assert(offsetof(HvCpu, fpcr) == HV_CPU_FPCR, "HvCpu layout");
_Static_assert(offsetof(HvCpu, fpsr) == HV_CPU_FPSR, "HvCpu layout");
_Static_assert(offsetof(HvCpu, helper_routine) == HV_CPU_HELPER, "HvCpu layout");
_Static_assert(offsetof(HvCpu, helper_ret) == HV_CPU_HELPER_RET, "HvCpu layout");
_Static_assert(offsetof(HvCpu, tlb) == HV_CPU_TLB, "HvCpu layout");
_Static_assert(offsetof(HvCpu, ret_salt) == HV_CPU_RET_SALT, "HvCpu layout");
_Static_assert(offsetof(HvCpu, tcr_el1) == HV_CPU_TCR_EL1, "HvCpu layout");
_Static_assert(offsetof(HvCpu, cntvoff) == HV_CPU_CNTVOFF, "HvCpu layout");
_Static_assert(offsetof(HvCpu, par_el1) == HV_CPU_PAR_EL1, "HvCpu layout");
_Static_assert(offsetof(HvCpu, ret_tab) == HV_CPU_RET_TAB, "HvCpu layout");
_Static_assert(offsetof(HvCpu, gaddend) == HV_CPU_GADDEND, "HvCpu layout");
_Static_assert(offsetof(HvCpu, xjump_thunk) == HV_CPU_XJUMP_THUNK, "HvCpu layout");
_Static_assert(offsetof(HvCpu, tpidr_el0) == HV_CPU_TPIDR_EL0, "HvCpu layout");
_Static_assert(offsetof(HvCpu, daif) == HV_CPU_DAIF, "HvCpu layout");
_Static_assert(offsetof(HvCpu, far_el1) == HV_CPU_FAR_EL1, "HvCpu layout");
_Static_assert(offsetof(HvCpu, exit_req) == HV_CPU_EXIT_REQ, "HvCpu layout");
_Static_assert(offsetof(HvCpu, last_stub) == HV_CPU_LAST_STUB, "HvCpu layout");
_Static_assert(offsetof(HvCpu, ijc_salt) == HV_CPU_IJC_SALT, "HvCpu layout");
_Static_assert(offsetof(HvCpu, excl_addr) == HV_CPU_EXCL_ADDR, "HvCpu layout");
_Static_assert(offsetof(HvCpu, el0_cnt) == HV_CPU_EL0_CNT, "HvCpu layout");
_Static_assert(offsetof(HvCpu, excl_size) == HV_CPU_EXCL_SIZE, "HvCpu layout");
_Static_assert(offsetof(HvCpu, excl_val) == HV_CPU_EXCL_VAL, "HvCpu layout");
_Static_assert(offsetof(HvCpu, host_sp) == HV_CPU_HOST_SP, "HvCpu layout");
_Static_assert(offsetof(HvCpu, lines_ptr) == HV_CPU_LINES_PTR, "HvCpu layout");
_Static_assert(offsetof(HvCpu, helper_thunk) == HV_CPU_HELPER_THUNK, "HvCpu layout");
_Static_assert(offsetof(HvCpu, chain_thunk) == HV_CPU_CHAIN_THUNK, "HvCpu layout");
_Static_assert(offsetof(HvCpu, ijc_thunk) == HV_CPU_IJC_THUNK, "HvCpu layout");
_Static_assert(offsetof(HvCpu, spsel) == HV_CPU_SPSEL, "HvCpu layout");
_Static_assert(offsetof(HvCpu, sp_el1) == HV_CPU_SP_EL1, "HvCpu layout");
_Static_assert(offsetof(HvCpu, ijc_tab) == HV_CPU_IJC_TAB, "HvCpu layout");
_Static_assert(HV_IJC_TABLES == 16, "ijc_tab has one pointer per table");
_Static_assert(sizeof(HvTlbEntry) == 32, "TLB entries are indexed with lsl #5");

/* Why the last block returned to the dispatcher. */
enum {
    HV_EXIT_NONE = 0,
    HV_EXIT_BRANCH,      /* reached the end of a block; pc holds the next guest PC */
    HV_EXIT_SYSREG,      /* a system register write changed state blocks depend on */
    HV_EXIT_EXCEPTION,   /* an exception was taken; pc is the vector */
    HV_EXIT_WFI,
    HV_EXIT_UNDEF,
    HV_EXIT_UNSUPPORTED, /* the translator cannot handle this instruction yet */
    HV_EXIT_HALT,        /* PSCI off, or a test asked to stop */
    HV_EXIT_FLUSH,       /* translated code was invalidated (IC, self-modifying code) */
    HV_EXIT_FAULT_LOOP,  /* fetching the exception vector itself faulted: the guest is dead */
};

/* PSTATE bits as laid out in SPSR. */
#define HV_PSTATE_M_EL1H 0x5u
#define HV_PSTATE_DAIF   (0xfu << 6)
#define HV_PSTATE_I      (1u << 7)
#define HV_PSTATE_F      (1u << 6)
#define HV_PSTATE_PAN    (1u << 22)

/* Which TLB table accesses at this EL use (EL0 for LDTR/STTR). */
static inline int hv_tlb_table(const HvCpu *cpu, int el) {
    return el == 0 ? 0 : (cpu->pstate_extra & HV_PSTATE_PAN) ? 2 : 1;
}

/* Where slot `slot` of TLB table `table` is in cpu->tlb. The old layout keeps a slot's four tables side by side, so a
   lookup's 128-byte cache line holds one useful entry; table-major (HV_TLBT) keeps each table's entries together, so
   the line holds those of four neighbouring pages. Changed only during a stop-the-world flush, which empties every TLB
   (dispatch.c), and translated code is emitted for the layout in force (put_tlb_slot). */
extern int hv_tlb_tmajor;
static inline uint32_t hv_tlb_code(uint32_t slot, int table) {
    return hv_tlb_tmajor ? (uint32_t)table << HV_TLB_BITS | slot : slot << 2 | (uint32_t)table;
}
static inline HvTlbEntry *hv_tlb_ent(HvCpu *cpu, uint32_t slot, int table) { return &cpu->tlb[hv_tlb_code(slot, table)]; }

#endif
