#include "exception.h"

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "machine.h"
#include "dispatch.h"
#include "mmu.h"
#include "rosetta.h"

/* Only EL0 and EL1 exist for the guest; HengeVM plays EL2 (PSCI over HVC, as
   QEMU's vmapple machine sets up) and there is no EL3. */

#define PSTATE_EXTRA_MASK ((1ull << 22) | (1ull << 23) | (1ull << 12) | (1ull << 24) | (1ull << 25))

uint64_t hv_pstate_get(HvCpu *cpu) {
    return (cpu->nzcv & 0xf0000000) | (cpu->daif & HV_PSTATE_DAIF) | (cpu->pstate_extra & PSTATE_EXTRA_MASK) |
           (uint64_t)cpu->el << 2 | (cpu->el ? cpu->spsel : 0);
}

void hv_sp_switch(HvCpu *cpu, uint32_t el, uint32_t spsel) {
    if (cpu->el == 0 || cpu->spsel == 0) cpu->sp_el0 = cpu->sp;
    else cpu->sp_el1 = cpu->sp;
    cpu->el = el;
    cpu->spsel = el ? spsel : 0;
    cpu->sp = cpu->spsel ? cpu->sp_el1 : cpu->sp_el0;
}

static void set_extra(HvCpu *cpu, uint64_t extra) {
    /* PAN selects a different TLB table (hv_tlb_table), so nothing to flush. */
    cpu->pstate_extra = extra;
}

void hv_pstate_set(HvCpu *cpu, uint64_t spsr) {
    uint32_t m = spsr & 0x1f;
    uint32_t el = (m >> 2) & 3;
    if ((m & 0x10) || el > 1) el = 1; /* AArch32 and EL2/EL3 do not exist: an illegal return, kept at EL1 */
    cpu->nzcv = spsr & 0xf0000000;
    cpu->daif = spsr & HV_PSTATE_DAIF;
    set_extra(cpu, spsr & PSTATE_EXTRA_MASK);
    hv_sp_switch(cpu, el, m & 1);
}

void hv_take_exception(HvCpu *cpu, int type, uint32_t esr, uint64_t far, int set_far, uint64_t ret) {
    /* An x86 thread's accesses (fenced) stay before the kernel's (not fenced: on a Mac it runs in TSO mode too, here
       it doesn't need to, but nothing it does may pass the thread's). The same on the way back (hv_eret). */
    if (hv_tso_thread(cpu)) __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint64_t spsr = hv_pstate_get(cpu);
    cpu->aspsr_el1 = (cpu->apflags >> 25) & 6;
    cpu->apflags = 0;
    if (type == HV_EXC_SYNC) HV_CPU_COUNT(cpu, exceptions, 1);
    else HV_CPU_COUNT(cpu, interrupts, 1);
    if (cpu->pc_state) { /* this CPU's history of EL changes (hv_debug_el0_in_kernel) */
        HvPerCpu *p = cpu->pc_state;
        uint32_t k = p->trace.nelchg++ & 15;
        p->trace.elchg[k].from = ret, p->trace.elchg[k].spsr = spsr, p->trace.elchg[k].kind = (uint32_t)type, p->trace.elchg[k].el = cpu->el;
        p->trace.elchg[k].to = esr;
    }
    if (cpu->m->trace || cpu->m->log >= 2)
        fprintf(stderr, "exception type %d esr %08x far %llx from pc %llx el%u\n", type, esr, (unsigned long long)far,
                (unsigned long long)ret, cpu->el);
    uint64_t offset = cpu->el == 0 ? 0x400 : cpu->spsel ? 0x200 : 0x000;
    cpu->spsr_el1 = spsr;
    cpu->elr_el1 = ret;
    if (type == HV_EXC_SYNC || type == HV_EXC_SERROR) cpu->esr_el1 = esr;
    if (set_far) cpu->far_el1 = far;
    hv_sp_switch(cpu, 1, 1);
    cpu->daif = HV_PSTATE_DAIF;
    uint64_t extra = cpu->pstate_extra;
    if (!((cpu->sctlr_el1 >> 23) & 1)) extra |= HV_PSTATE_PAN; /* SCTLR_EL1.SPAN == 0 */
    extra &= ~(1ull << 12);
    if ((cpu->sctlr_el1 >> 44) & 1) extra |= 1ull << 12;      /* SSBS from DSSBS */
    set_extra(cpu, extra);
    cpu->excl_addr = ~0ull;
    cpu->pc = (cpu->vbar_el1 & ~0x7ffull) + offset + (uint64_t)type * 0x80;
    cpu->exit_reason = HV_EXIT_EXCEPTION;
    hv_rosetta_fp_apply(cpu);
}

void hv_raise_sync(HvCpu *cpu, uint32_t ec, uint32_t iss, uint64_t far, uint64_t pc) {
    int set_far = ec == HV_EC_PC_ALIGN || ec == HV_EC_IABT_LOWER || ec == HV_EC_IABT_SAME ||
                  ec == HV_EC_DABT_LOWER || ec == HV_EC_DABT_SAME;
    hv_take_exception(cpu, HV_EXC_SYNC, ec << 26 | 1u << 25 | (iss & 0x1ffffff), far, set_far, pc);
}

void hv_raise_undef(HvCpu *cpu, uint64_t pc) {
    static int reported;
    if (cpu->el == 0 && ((pc >> 55) & 1) && reported++ < 3) hv_debug_el0_in_kernel(cpu, "undef");
    if (cpu->m->log) {
        uint32_t insn = 0;
        uint64_t pa;
        if (!hv_mmu_access(cpu, pc, HV_ACC_EXEC, cpu->el, &pa)) hv_phys_read(cpu->m, pa, &insn, 4);
        fprintf(stderr, "undefined: pc %llx insn %08x el%u\n", (unsigned long long)pc, insn, cpu->el);
        if (cpu->el == 1) { /* a kernel assert: its backtrace, from the frame pointers (lr first) */
            fprintf(stderr, "  backtrace: lr %llx", (unsigned long long)(cpu->x[30] | 0xffffff8000000000ull));
            uint64_t fp = cpu->x[29];
            for (int i = 0; i < 24 && fp; i++) {
                uint64_t fr[2], fpa;
                if (hv_mmu_access(cpu, fp, HV_ACC_READ, 1, &fpa) || hv_phys_read(cpu->m, fpa, fr, 16)) break;
                uint64_t ret = fr[1] | 0xffffff8000000000ull; /* strip the PAC bits */
                fprintf(stderr, " %llx", (unsigned long long)ret);
                if (fr[0] <= fp) break;
                fp = fr[0];
            }
            fprintf(stderr, "\n  vbar %llx\n", (unsigned long long)cpu->vbar_el1);
        }
    }
    hv_raise_sync(cpu, HV_EC_UNKNOWN, 0, 0, pc);
}

void hv_raise_abort(HvCpu *cpu, int insn, uint64_t va, uint32_t fsc, int wnr, int s1ptw, uint64_t pc) {
    uint32_t ec = insn ? (cpu->el ? HV_EC_IABT_SAME : HV_EC_IABT_LOWER)
                       : (cpu->el ? HV_EC_DABT_SAME : HV_EC_DABT_LOWER);
    uint32_t iss = fsc | (uint32_t)(wnr && !insn) << 6 | (uint32_t)s1ptw << 7;
    hv_raise_sync(cpu, ec, iss, va, pc);
}

void hv_eret(HvCpu *cpu) {
    if (cpu->pc_state) {
        HvTrace *t = &cpu->pc_state->trace;
        uint32_t k = t->nelchg++ & 15;
        t->elchg[k].from = cpu->pc, t->elchg[k].to = cpu->elr_el1, t->elchg[k].spsr = cpu->spsr_el1;
        t->elchg[k].kind = 9, t->elchg[k].el = cpu->el;
    }
    cpu->pc = cpu->elr_el1;
    hv_pstate_set(cpu, cpu->spsr_el1);
    cpu->apflags = (cpu->aspsr_el1 & 6) << 25;
    hv_rosetta_fp_apply(cpu);
    cpu->excl_addr = ~0ull;
    if (cpu->m && hv_tso_thread(cpu)) __atomic_thread_fence(__ATOMIC_SEQ_CST); /* see hv_take_exception */
}

static HvCpu *cpu_by_mpidr(HvMachine *m, uint64_t mpidr) {
    for (int i = 0; i < m->ncpus; i++)
        if (m->cpus[i] && (m->cpus[i]->mpidr & 0xff00ffffffull) == (mpidr & 0xff00ffffffull)) return m->cpus[i];
    return NULL;
}

/* PSCI 1.0. */
static uint64_t psci(HvCpu *cpu, uint64_t fn) {
    static int log = -1;
    if (log < 0) log = getenv("HV_PSCI_LOG") != NULL;
    if (log && ((uint32_t)fn >> 24 == 0x84 || (uint32_t)fn >> 24 == 0xc4))
        fprintf(stderr, "psci %#llx x1 %#llx x2 %#llx x3 %#llx pc %llx\n", (unsigned long long)fn, (unsigned long long)cpu->x[1],
                (unsigned long long)cpu->x[2], (unsigned long long)cpu->x[3], (unsigned long long)cpu->pc);
    switch ((uint32_t)fn) {
    case 0x84000000: return 0x10000;                         /* PSCI_VERSION: 1.0 */
    case 0x84000006: return 2;                               /* MIGRATE_INFO_TYPE: no trusted OS */
    case 0x84000008:                                         /* SYSTEM_OFF */
    case 0x84000009:                                         /* SYSTEM_RESET */
        if (cpu->m->log)
            fprintf(stderr, "hengevm: macOS asked to %s (cpu %u)\n", (uint32_t)fn == 0x84000008 ? "shut down" : "restart",
                    cpu->index);
        /* The whole machine stops, CPUs parked in CPU_OFF included (CPU 0 may be one of them). */
        hv_machine_halt(cpu->m, (uint32_t)fn == 0x84000008 ? HV_HALT_SHUTDOWN : HV_HALT_RESET);
        return 0;
    case 0x8400000a: {                                       /* PSCI_FEATURES */
        uint32_t q = (uint32_t)cpu->x[1];
        if (q == 0x84000000 || q == 0x84000002 || q == 0x84000006 || q == 0x84000008 || q == 0x84000009 ||
            q == 0x8400000a || q == 0xc4000003 || q == 0x84000003 || q == 0xc4000004 || q == 0x84000004)
            return 0;
        return (uint64_t)-1;
    }
    case 0x84000002:                                         /* CPU_OFF: until another CPU_ON */
        cpu->off = 1;
        return 0;
    case 0x84000003: case 0xc4000003: {                      /* CPU_ON: target, entry point, context id */
        HvCpu *t = cpu_by_mpidr(cpu->m, cpu->x[1]);
        if (!t) return (uint64_t)-2;                         /* INVALID_PARAMETERS */
        if (!t->off) return (uint64_t)-4;                    /* ALREADY_ON */
        /* Parked once parked_off reads 1 under its wake_mu: the target changes it only under that lock, and clears
           it while it gives back what it held for running (wait_while_off), which a reset mustn't meet. */
        HvPerCpu *pcs = t->pc_state;
        for (;;) {
            while (!__atomic_load_n(&t->parked_off, __ATOMIC_SEQ_CST)) { /* on its way out of CPU_OFF, or giving back */
                /* The machine is stopping: the target may never park (it leaves instead). DENIED. */
                if (__atomic_load_n(&cpu->halted, __ATOMIC_ACQUIRE) || __atomic_load_n(&t->halted, __ATOMIC_ACQUIRE))
                    return (uint64_t)-3;
                sched_yield();
            }
            pthread_mutex_lock(&pcs->wake_mu);
            if (__atomic_load_n(&t->parked_off, __ATOMIC_SEQ_CST)) break;
            pthread_mutex_unlock(&pcs->wake_mu);
        }
        uint64_t entry = cpu->x[2], context = cpu->x[3];
        if ((uint32_t)fn == 0x84000003) entry = (uint32_t)entry, context = (uint32_t)context;
        /* As at reset: EL1h, MMU and caches off, everything masked. Its thread
           waits on wake_mu, so it cannot see the CPU half reset. */
        if (__atomic_load_n(&t->halted, __ATOMIC_ACQUIRE)) { /* the reset would clear the halt: the machine is stopping */
            pthread_mutex_unlock(&pcs->wake_mu);
            return (uint64_t)-3;
        }
        hv_cpu_reset_index(t, cpu->m, t->index); /* keeps its mpidr: another CPU's PSCI call may look for it */
        t->pc = entry;
        t->x[0] = context;
        t->parked_off = 1;
        __atomic_store_n(&t->off, 0, __ATOMIC_RELEASE);
        pcs->wake_pending = 1;
        pthread_cond_signal(&pcs->wake_cv);
        pthread_mutex_unlock(&pcs->wake_mu);
        return 0;
    }
    case 0x84000004: case 0xc4000004: {                      /* AFFINITY_INFO: 0 on, 1 off */
        HvCpu *t = cpu_by_mpidr(cpu->m, cpu->x[1]);
        if (!t) return (uint64_t)-2;
        return t->off ? 1 : 0;
    }
    /* vmapple's paravirtualized PAC (XNU osfmk/arm64/hv_hvc.h): the guest
       cannot program PAC keys itself, so the hypervisor does. HengeVM never
       signs pointers, so there is nothing to program; each call only has to
       succeed (XNU spins on a nonzero x0). */
    case 0xc1000000: /* VMAPPLE_PAC_SET_INITIAL_STATE */
    case 0xc1000002: /* VMAPPLE_PAC_SET_A_KEYS */
    case 0xc1000003: /* VMAPPLE_PAC_SET_B_KEYS */
    case 0xc1000004: /* VMAPPLE_PAC_SET_EL0_DIVERSIFIER */
    case 0xc1000005: /* VMAPPLE_PAC_SET_EL0_DIVERSIFIER_AT_EL1 */
    case 0xc1000006: /* VMAPPLE_PAC_SET_G_KEY */
    case 0xc10000f0: /* VMAPPLE_PAC_NOP */
        return 0;
    case 0xc1000001: /* VMAPPLE_PAC_GET_DEFAULT_KEYS: x2 = default ROP id, x3 = default JOP id */
        cpu->x[2] = 0x524f50; /* the values only name keys, and no key is ever used */
        cpu->x[3] = 0x4a4f50;
        return 0;
    default:
        if (cpu->m->log) fprintf(stderr, "hengevm: unknown hypervisor call %#llx (x1 %#llx) at el%u\n",
                                 (unsigned long long)fn, (unsigned long long)cpu->x[1], cpu->el);
        return (uint64_t)-1;                                 /* NOT_SUPPORTED */
    }
}

/* Bring-up aid: x0-x3, and any string they point to (read with the current EL's mapping). */
void hv_debug_args(HvCpu *cpu) {
    for (int i = 0; i < 4; i++) fprintf(stderr, " x%d=%llx", i, (unsigned long long)cpu->x[i]);
    fprintf(stderr, "\n");
    for (int i = 0; i < 4; i++) {
        char str[161];
        int n = 0;
        for (; n < 160; n++) {
            uint64_t pa;
            if (hv_mmu_access(cpu, cpu->x[i] + n, HV_ACC_READ, cpu->el, &pa) || hv_phys_read(cpu->m, pa, &str[n], 1) ||
                str[n] < 9 || (unsigned char)str[n] > 126)
                break;
        }
        str[n] = 0;
        if (n >= 4) fprintf(stderr, "    x%d -> \"%s\"\n", i, str);
    }
}

int hv_exception_insn(HvCpu *cpu, uint32_t insn, uint64_t pc) {
    uint32_t imm16 = (insn >> 5) & 0xffff;
    uint32_t opc = (insn >> 21) & 7, ll = insn & 3;
    if (opc == 0 && ll == 1 && cpu->m->log >= 3) {
        fprintf(stderr, "svc #%x at %llx el%u:", imm16, (unsigned long long)pc, cpu->el);
        hv_debug_args(cpu);
    }
    if (opc == 0 && ll == 1) { /* SVC: ELR is the next instruction */
        hv_take_exception(cpu, HV_EXC_SYNC, HV_EC_SVC64 << 26 | 1u << 25 | imm16, 0, 0, pc + 4);
        return 1;
    }
    if (opc == 0 && ll == 2 && cpu->el == 1) { /* HVC: the hypervisor's PSCI and vmapple's PAC calls */
        cpu->x[0] = psci(cpu, cpu->x[0]);
        if (!cpu->halted && !cpu->off) return 0; /* like a call: the block carries on (xnu makes one per context switch) */
        cpu->pc = pc + 4;
        cpu->exit_reason = cpu->halted ? HV_EXIT_HALT : HV_EXIT_BRANCH; /* CPU_OFF: the dispatcher parks it */
        return 1;
    }
    if (opc == 1 && ll == 0) { /* BRK */
        hv_take_exception(cpu, HV_EXC_SYNC, HV_EC_BRK << 26 | 1u << 25 | imm16, 0, 0, pc);
        return 1;
    }
    /* SMC with no EL3, HVC from EL0, HLT without halting debug, DCPS: undefined. */
    hv_raise_undef(cpu, pc);
    return 1;
}

int hv_check_interrupts(HvCpu *cpu) {
    HvMachine *m = cpu->m;
    HvPerCpu *p = cpu->pc_state;
    (void)m;
    /* Taken, an interrupt is where the CPU goes first: hv_cpu_pass_stop's pass, for the place it was, goes. Stored
       before the call, so nothing lives across it: the path that takes none keeps its no-frame shape. */
    if (p->fiq_line && !(cpu->daif & HV_PSTATE_F)) {
        p->stop_pass = ~0ull;
        hv_take_exception(cpu, HV_EXC_FIQ, 0, 0, 0, cpu->pc);
        return 1;
    }
    if (p->irq_line && !(cpu->daif & HV_PSTATE_I)) {
        p->stop_pass = ~0ull;
        hv_take_exception(cpu, HV_EXC_IRQ, 0, 0, 0, cpu->pc);
        return 1;
    }
    return 0;
}
