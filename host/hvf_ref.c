/* hengevm-ref: a Mac-only test tool. Runs the same machine (HengeVM's RAM,
   devices, cfg region and disks) with the guest CPU on Hypervisor.framework
   instead of the JIT, so a JIT run can be compared against real hardware at
   a breakpoint. HengeVM itself never uses a hypervisor.

   hengevm-ref --rom FILE --disk FILE --aux FILE --ecid N [--seed N] [--ram MB] [--break PC [--break-hits N]] [--log N] */
#include <Hypervisor/Hypervisor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exception.h"
#include "vmapple.h"

static HvMachine m;
static HvCpu cpu; /* only used as the device model's view of the CPU (timer, GIC, mpidr) */
static HvVmapple board;
static hv_vcpu_t vcpu;
static hv_vcpu_exit_t *vexit;

#define CHECK(x)                                                                         \
    do {                                                                                 \
        hv_return_t r_ = (x);                                                            \
        if (r_ != HV_SUCCESS) {                                                          \
            fprintf(stderr, "hengevm-ref: %s failed: %x\n", #x, r_);                     \
            exit(1);                                                                     \
        }                                                                                \
    } while (0)

static uint64_t reg(hv_reg_t r) {
    uint64_t v;
    CHECK(hv_vcpu_get_reg(vcpu, r, &v));
    return v;
}

static uint64_t sysreg(hv_sys_reg_t r) {
    uint64_t v;
    CHECK(hv_vcpu_get_sys_reg(vcpu, r, &v));
    return v;
}

static uint64_t gpr(int n) {
    return n == 31 ? 0 : reg(HV_REG_X0 + n);
}

static void set_gpr(int n, uint64_t v) {
    if (n != 31) CHECK(hv_vcpu_set_reg(vcpu, HV_REG_X0 + n, v));
}

static void advance(void) {
    CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC, reg(HV_REG_PC) + 4));
}

/* Map guest RAM regions of the machine into the VM's IPA space. */
static void map_ram(void) {
    for (int i = 0; i < m.nram; i++) {
        HvRam *r = &m.ram[i];
        CHECK(hv_vm_map(r->host, r->pa, r->size, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));
    }
}

static void dump(const char *why) {
    fprintf(stderr, "\nhengevm-ref: %s\n", why);
    uint64_t cpsr = reg(HV_REG_CPSR);
    fprintf(stderr, "  pc %016llx el%llu sp %016llx\n", (unsigned long long)reg(HV_REG_PC), (unsigned long long)((cpsr >> 2) & 3),
            (unsigned long long)((cpsr & 0xc) && (cpsr & 1) ? sysreg(HV_SYS_REG_SP_EL1) : sysreg(HV_SYS_REG_SP_EL0)));
    fprintf(stderr, "  esr %08llx far %016llx elr %016llx vbar %016llx sctlr %08llx\n",
            (unsigned long long)sysreg(HV_SYS_REG_ESR_EL1), (unsigned long long)sysreg(HV_SYS_REG_FAR_EL1),
            (unsigned long long)sysreg(HV_SYS_REG_ELR_EL1), (unsigned long long)sysreg(HV_SYS_REG_VBAR_EL1),
            (unsigned long long)sysreg(HV_SYS_REG_SCTLR_EL1));
    for (int i = 0; i < 31; i += 4)
        fprintf(stderr, "  x%-2d %016llx %016llx %016llx %016llx\n", i, (unsigned long long)gpr(i),
                (unsigned long long)gpr(i + 1), (unsigned long long)gpr(i + 2),
                (unsigned long long)(i + 3 < 31 ? gpr(i + 3) : 0));
    fprintf(stderr, "  nzcv %08llx\n", (unsigned long long)(cpsr & 0xf0000000));
}

/* A data abort on an address with no RAM behind it: a device access. */
static void mmio(uint64_t esr, uint64_t pa) {
    int write = (esr >> 6) & 1;
    if (!((esr >> 24) & 1)) {
        dump("device access without a syndrome (ISV=0): not handled");
        exit(1);
    }
    unsigned size = 1u << ((esr >> 22) & 3);
    int srt = (esr >> 16) & 31, sse = (esr >> 21) & 1, sf = (esr >> 15) & 1;
    if (write) {
        uint64_t v = gpr(srt);
        if (hv_phys_write(&m, pa, &v, size) != 0 && m.log) fprintf(stderr, "ref: unmapped write pa %llx\n", (unsigned long long)pa);
    } else {
        uint64_t v = 0;
        if (hv_phys_read(&m, pa, &v, size) != 0 && m.log) fprintf(stderr, "ref: unmapped read pa %llx\n", (unsigned long long)pa);
        if (sse && size < 8) v = (uint64_t)((int64_t)(v << (64 - size * 8)) >> (64 - size * 8));
        if (!sf) v &= 0xffffffff;
        set_gpr(srt, v);
    }
    advance();
}

/* HVC: the same PSCI the JIT path answers. */
static void hvc(void) {
    cpu.x[0] = gpr(0);
    cpu.x[1] = gpr(1);
    hv_exception_insn(&cpu, 0xd4000002, 0); /* HVC #0, handled by HengeVM's PSCI */
    set_gpr(0, cpu.x[0]);
    /* HVC's preferred return already points past the instruction. */
}

/* Trapped MRS/MSR: GIC CPU interface registers go to HengeVM's GIC. */
static void sysreg_trap(uint64_t esr) {
    uint32_t iss = esr & 0x1ffffff;
    int read = iss & 1, rt = (iss >> 5) & 31;
    uint32_t op0 = (iss >> 20) & 3, op2 = (iss >> 17) & 7, op1 = (iss >> 14) & 7, crn = (iss >> 10) & 15,
             crm = (iss >> 1) & 15;
    uint32_t r = op0 << 14 | op1 << 11 | crn << 7 | crm << 3 | op2;
    if (read) set_gpr(rt, m.icc_read ? m.icc_read(&m, &cpu, r) : 0);
    else if (m.icc_write) m.icc_write(&m, &cpu, r, gpr(rt));
    if (m.log >= 3)
        fprintf(stderr, "ref: %s S%u_%u_C%u_C%u_%u\n", read ? "mrs" : "msr", op0, op1, crn, crm, op2);
    advance();
}

static void usage(void) {
    fprintf(stderr, "usage: hengevm-ref --rom FILE --disk FILE --aux FILE --ecid N [--seed N] [--ram MB] [--break PC [--break-hits N]] [--log N]\n");
    exit(2);
}

int main(int argc, char **argv) {
    const char *rom = NULL, *disk_path = NULL, *aux_path = NULL;
    uint64_t seed = 0, ecid = 0, ram_mb = 2048, brk = ~0ull, hits = 1;
    int log = 1;
    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc) usage();
        if (!strcmp(argv[i], "--rom")) rom = argv[++i];
        else if (!strcmp(argv[i], "--disk")) disk_path = argv[++i];
        else if (!strcmp(argv[i], "--aux")) aux_path = argv[++i];
        else if (!strcmp(argv[i], "--ecid")) ecid = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--ram")) ram_mb = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--seed")) seed = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--break")) brk = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--break-hits")) hits = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--log")) log = atoi(argv[++i]);
        else usage();
    }
    if (!rom || !disk_path || !aux_path) usage();
    setvbuf(stderr, NULL, _IONBF, 0);

    static HvDisk disk, aux;
    if (hv_disk_open(&disk, disk_path, 0, 0) != 0 || hv_disk_open(&aux, aux_path, 16384, 0) != 0) return 1;
    if (hv_machine_init(&m, 1 << 20) != 0) return 1;
    m.log = log;
    HvCfgParams cfg = {.nr_cpus = 1, .ecid = ecid, .seed = seed, .mac_en0 = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01}};
    static const uint8_t mac[6] = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01};
    if (hv_vmapple_create(&board, &m, &cpu, ram_mb << 20, &disk, &aux, &cfg, mac) != 0 ||
        hv_vmapple_load_rom(&board, rom) != 0)
        return 1;
    hv_vmapple_reset_cpu(&board);

    CHECK(hv_vm_create(NULL));
    map_ram();
    CHECK(hv_vcpu_create(&vcpu, &vexit, NULL));
    /* The virtual counter starts with the machine, as under the JIT: the timer model subtracts m.cntvoff. */
    CHECK(hv_vcpu_set_vtimer_offset(vcpu, m.cntvoff));
    CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC, VMAPPLE_FIRMWARE));
    CHECK(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5)); /* EL1h, DAIF masked */
    CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MPIDR_EL1, 0x80000000));
    if (brk != ~0ull) {
        CHECK(hv_vcpu_set_trap_debug_exceptions(vcpu, true));
        CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_DBGBVR0_EL1, brk));
        CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_DBGBCR0_EL1, 0x1e7)); /* enabled, EL1 and EL0, all bytes */
        CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, 1u << 15)); /* MDE */
    }

    for (;;) {
        CHECK(hv_vcpu_run(vcpu));
        if (vexit->reason != HV_EXIT_REASON_EXCEPTION) {
            if (vexit->reason == HV_EXIT_REASON_VTIMER_ACTIVATED) {
                cpu.cntv_ctl = sysreg(HV_SYS_REG_CNTV_CTL_EL0);
                cpu.cntv_cval = sysreg(HV_SYS_REG_CNTV_CVAL_EL0);
                board.m->tick(board.m, &cpu);
                CHECK(hv_vcpu_set_pending_interrupt(vcpu, HV_INTERRUPT_TYPE_IRQ, m.percpu[0].irq_line));
                CHECK(hv_vcpu_set_vtimer_mask(vcpu, false));
                continue;
            }
            fprintf(stderr, "hengevm-ref: exit reason %u\n", vexit->reason);
            break;
        }
        uint64_t esr = vexit->exception.syndrome;
        uint32_t ec = (uint32_t)(esr >> 26);
        if (ec == 0x24) {
            mmio(esr, vexit->exception.physical_address);
        } else if (ec == 0x16) {
            hvc();
            if (cpu.halted) {
                dump("guest powered off (PSCI)");
                break;
            }
        } else if (ec == 0x18) {
            sysreg_trap(esr);
        } else if (ec == 0x30 || ec == 0x31) {
            if (--hits == 0) {
                char why[64];
                snprintf(why, sizeof why, "breakpoint %llx", (unsigned long long)brk);
                dump(why);
                break;
            }
            /* Step over: disable, run one instruction with single step, re-enable. */
            CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_DBGBCR0_EL1, 0));
            CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, 1u << 15 | 1));
            uint64_t cpsr = reg(HV_REG_CPSR);
            CHECK(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, cpsr | (1u << 21))); /* PSTATE.SS */
        } else if (ec == 0x32 || ec == 0x33) {
            CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_MDSCR_EL1, 1u << 15));
            CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_DBGBCR0_EL1, 0x1e7));
        } else if (ec == 0x01) { /* WFI/WFE */
            advance();
        } else {
            char why[64];
            snprintf(why, sizeof why, "unhandled exit, EC %#x ESR %llx", ec, (unsigned long long)esr);
            dump(why);
            break;
        }
        if (board.panic_events & 1) {
            dump("guest reported a panic");
            break;
        }
    }
    return 0;
}
