#include "vmapple.h"
#include "pvg_lite.h"
#include "sysreg.h"

#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <mach/vm_map.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

#include "timer.h"

#define ROM_SIZE 0x00100000ull

/* pvpanic (MMIO): reads give the events supported, as QEMU's default
   (panicked, crash loaded); a write reports one. */
static uint64_t pvpanic_read(void *opaque, uint64_t off, unsigned size) {
    (void)opaque, (void)off, (void)size;
    return 3;
}

static void pvpanic_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    (void)off, (void)size;
    HvVmapple *b = opaque;
    b->panic_events = v;
    fprintf(stderr, "hengevm: guest reported a panic (pvpanic event %llx)\n", (unsigned long long)v);
    /* The panic text, wherever the kernel formatted it (pram is not always
       where the log ends up, e.g. in the restore OS). */
    const char key[] = "panic(cpu ";
    for (uint8_t *p = b->ram, *end = b->ram + b->ram_size; p && p < end;) {
        p = memmem(p, (size_t)(end - p), key, sizeof key - 1);
        if (!p) break;
        size_t n = 0;
        while (p + n < end && n < 3000 && (p[n] == '\n' || p[n] == '\t' || (p[n] >= 32 && p[n] < 127))) n++;
        if (n > 80) {
            /* The log goes on past NULs (backtrace, kexts): print what reads as text. */
            char out[6001];
            size_t k = 0;
            for (size_t i = 0; i < 6000 && p + i < end && k < 6000; i++) {
                uint8_t c = p[i];
                if (c == '\n' || c == '\t' || (c >= 32 && c < 127)) out[k++] = (char)c;
                else if (c == 0 && k && out[k - 1] != '\n') out[k++] = '\n';
            }
            out[k] = 0;
            fprintf(stderr, "hengevm: panic text at PA %llx:\n%s\n", (unsigned long long)(VMAPPLE_MEM + (uint64_t)(p - b->ram)), out);
            break;
        }
        p += sizeof key - 1;
    }
    if (v & 1) hv_machine_halt(b->m, HV_HALT_PANIC);
}

/* Not in QEMU's vmapple, which targets macOS 12 guests. iBootStage2 (Sequoia)
   reads 0x30240040 and uses its PL031 driver ("pl031-rtc") if the value is
   0xf001, or an Apple virtual RTC ("avp-rtc") otherwise. HengeVM has a PL031,
   so it answers 0xf001. The kernel's AppleVirtualPlatformRTC uses the same
   window: +0 is a 64-bit microsecond counter, and wall time is the counter
   plus an offset the driver keeps in NVRAM ("AvpRtc", absent until the guest
   sets the clock), so the counter is Unix time in microseconds. +0x18 bit 0
   enables it, +0x20 is the interrupt control and +0x38 the alarm; they are
   plain storage here (the driver arms no alarm). Anything else is logged. */
static uint64_t rtc_select_read(void *opaque, uint64_t off, unsigned size) {
    HvVmapple *b = opaque;
    if (off == 0x40) return 0xf001;
    if (off < 0x40 && !(off & 7) && off) return b->avp_rtc[off / 8];
    if (off == 0 || off == 4) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000;
        return off ? us >> 32 : size == 4 ? (uint32_t)us : us;
    }
    if (b->m->log) fprintf(stderr, "hengevm: read of unknown register 0x%llx+%llx (size %u)\n",
                           (unsigned long long)VMAPPLE_RTC_SELECT, (unsigned long long)off, size);
    return 0;
}

static void rtc_select_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvVmapple *b = opaque;
    if (off && off < 0x40 && !(off & 7)) {
        b->avp_rtc[off / 8] = v;
        return;
    }
    if (b->m->log) fprintf(stderr, "hengevm: write of unknown register 0x%llx+%llx = %llx (size %u)\n",
                           (unsigned long long)VMAPPLE_RTC_SELECT, (unsigned long long)off, (unsigned long long)v, size);
}

/* macOS 27's virtual Mac (device tree: avp-ctrr at arm-io +0x20260000, avp-sealed-registers at +0x20270000).
   iBEC (mBoot-20457) reads an information block from avp-ctrr before it locks the kernel's text, and panics
   (4962211b03d706d:203) on a machine without one:
     +0x00 magic 'ctrr' (0x63747272)   +0x04 version 1   +0x0c must be > 7   +0x10 must be < 15
     +0x20, +0x28 the physical range the locked regions must lie in   +0x50, +0x70 must be 7
   then programs region registers (+0x40.., +0xc0.., +0xd0). Nothing is enforced here: both devices are
   plain registers that keep what is written, which is all the firmware can see of them. HV_AVP_LOG logs them. */
typedef struct {
    const char *name;
    int log;
    uint8_t regs[0x10000];
} AvpRegs;

static uint64_t avp_regs_read(void *opaque, uint64_t off, unsigned size) {
    AvpRegs *r = opaque;
    uint64_t v = 0;
    if (off + size <= sizeof r->regs) memcpy(&v, r->regs + off, size);
    if (r->log) fprintf(stderr, "hengevm: %s read +%llx (size %u) = %llx\n", r->name, (unsigned long long)off, size,
                        (unsigned long long)v);
    return v;
}

static void avp_regs_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    AvpRegs *r = opaque;
    if (off + size <= sizeof r->regs) memcpy(r->regs + off, &v, size);
    if (r->log) fprintf(stderr, "hengevm: %s write +%llx (size %u) = %llx\n", r->name, (unsigned long long)off, size,
                        (unsigned long long)v);
}

static void avp_put(AvpRegs *r, uint64_t off, uint64_t v, unsigned size) { memcpy(r->regs + off, &v, size); }

static void avp_regs_create(HvVmapple *b, uint64_t ram_size) {
    HvMachine *m = b->m;
    AvpRegs *ctrr = calloc(1, sizeof *ctrr), *sealed = calloc(1, sizeof *sealed);
    b->ctrr = ctrr, b->sealed = sealed;
    ctrr->name = "avp-ctrr", sealed->name = "avp-sealed-registers";
    ctrr->log = sealed->log = getenv("HV_AVP_LOG") != NULL;
    avp_put(ctrr, 0x00, 0x63747272, 4);
    avp_put(ctrr, 0x04, 1, 4);
    avp_put(ctrr, 0x0c, 14, 4);
    avp_put(ctrr, 0x10, 14, 4);
    avp_put(ctrr, 0x20, VMAPPLE_MEM, 8);
    avp_put(ctrr, 0x28, VMAPPLE_MEM + ram_size, 8);
    avp_put(ctrr, 0x50, 7, 8);
    avp_put(ctrr, 0x70, 7, 8);
    hv_machine_add_mmio(m, VMAPPLE_CTRR, sizeof ctrr->regs, ctrr, avp_regs_read, avp_regs_write, "avp-ctrr");
    hv_machine_add_mmio(m, VMAPPLE_SEALED, sizeof sealed->regs, sealed, avp_regs_read, avp_regs_write, "avp-sealed-registers");
}

/* The guest's device tree gives the GIC a third region (gic,vmapple1), and
   the kernel's AppleVirtualPlatformPCIEMSIController asserts that the type
   register at +0x8 has bit 31 ("valid") set. It splits the rest into two
   13-bit fields and uses +0x40 as the doorbell: an MSI frame in the style of
   GICv2m, so it follows that layout: base INTID in bits 28..16, count in bits
   12..0, and a write of an INTID to +0x40 makes it pending. */
static uint64_t gic_msi_read(void *opaque, uint64_t off, unsigned size) {
    HvVmapple *b = opaque;
    if (off == 0x8) return 1u << 31 | (uint32_t)VMAPPLE_MSI_BASE << 16 | VMAPPLE_MSI_COUNT;
    if (b->m->log) fprintf(stderr, "hengevm: gic msi frame read +%llx (size %u)\n", (unsigned long long)off, size);
    return 0;
}

static void gic_msi_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvVmapple *b = opaque;
    if (off == 0x40) {
        if (v >= VMAPPLE_MSI_BASE && v < VMAPPLE_MSI_BASE + VMAPPLE_MSI_COUNT) hv_gicv3_pend(b->gic, (int)v);
        else if (b->m->log) fprintf(stderr, "hengevm: msi doorbell for INTID %llu outside the frame\n", (unsigned long long)v);
        return;
    }
    if (b->m->log) fprintf(stderr, "hengevm: gic msi frame write +%llx = %llx (size %u)\n", (unsigned long long)off,
                           (unsigned long long)v, size);
}

/* Guest RAM the memory limit does not count: asking the kernel to leave these
   pages out of this process's footprint. It allows that once a debugger has
   been attached, which is how an iOS app gets to use JIT at all. Returns NULL
   if it is refused. In pieces, as one memory entry has a size limit. */
extern kern_return_t mach_memory_entry_ownership(mach_port_t entry, task_t owner, int ledger_tag, int ledger_flags);

static uint8_t *alloc_no_footprint(uint64_t size) {
    enum { CHUNK = 128u << 20, MAP_MEM_LEDGER_TAGGED_ = 0x002000, MAP_MEM_NAMED_CREATE_ = 0x020000, LEDGER_TAG_DEFAULT = 1, LEDGER_NO_FOOTPRINT = 1 };
    vm_address_t base = 0;
    if (vm_allocate(mach_task_self(), &base, size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return NULL;
    for (uint64_t off = 0; off < size; off += CHUNK) {
        uint64_t len = size - off < CHUNK ? size - off : CHUNK;
        memory_object_size_t got = len;
        mach_port_t entry = MACH_PORT_NULL;
        kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &got, 0, MAP_MEM_NAMED_CREATE_ | MAP_MEM_LEDGER_TAGGED_ | VM_PROT_READ | VM_PROT_WRITE, &entry, MACH_PORT_NULL);
        if (kr == KERN_SUCCESS) kr = mach_memory_entry_ownership(entry, TASK_NULL, LEDGER_TAG_DEFAULT, LEDGER_NO_FOOTPRINT);
        if (kr == KERN_SUCCESS) {
            vm_address_t to = base + off;
            kr = vm_map(mach_task_self(), &to, len, 0, VM_FLAGS_OVERWRITE, entry, 0, FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
        }
        if (entry != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), entry);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "hengevm: guest RAM outside the footprint refused: %s\n", mach_error_string(kr));
            vm_deallocate(mach_task_self(), base, size);
            return NULL;
        }
    }
    fprintf(stderr, "hengevm: guest RAM (%llu MB) is outside the footprint\n", (unsigned long long)(size >> 20));
    return (uint8_t *)base;
}

/* A mapped file's page is slow to touch the first time and as fast as any other
   afterwards, so a background thread gets there before the guest does. Adding
   zero dirties the page without changing it, whatever the guest has written. */
/* An Apple register block's bytes (64 KB), for saving the machine. */
uint8_t *hv_vmapple_avp_regs(void *avp) { return avp ? ((AvpRegs *)avp)->regs : NULL; }

struct PrefaultArgs {
    uint8_t *ram;
    uint64_t size;
};

static void *prefault_thread(void *arg) {
    struct PrefaultArgs a = *(struct PrefaultArgs *)arg;
    free(arg);
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint64_t off = 0; off < a.size; off += 0x4000) __atomic_fetch_add((volatile uint64_t *)(a.ram + off), 0, __ATOMIC_RELAXED);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    fprintf(stderr, "hengevm: guest RAM file ready in %.1f s\n", (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9);
    return NULL;
}

static uint64_t tick_locked(HvMachine *m, HvCpu *cpu) {
    HvVmapple *b = m->tick_opaque;
    uint64_t deadline = hv_timer_update(b->gic, cpu);
    if (cpu->index != 0) return deadline; /* the others only have their own timers to look after */
    hv_pl031_tick(b->rtc);
    if (b->poll) b->poll();
    hv_pvg_lite_poll(b->pvg);
    hv_virtio_blk_poll(b->root_blk);
    hv_virtio_blk_poll(b->aux_blk);
    hv_xhci_poll(b->usb);
    hv_virtio_input_poll(b->keyboard);
    hv_virtio_input_poll(b->mouse);
    hv_virtio_snd_poll(b->snd);
    hv_virtio_usb_poll(b->vusb);
    hv_virtio_balloon_poll(b->balloon);
    hv_bdif_poll(b->bdif);
    return deadline; /* every GIC state change already updated the CPU's lines */
}

/* Devices and the GIC are shared by the CPUs: one at a time. */
static uint64_t tick(HvMachine *m, HvCpu *cpu) {
    if (cpu->index != 0) return hv_timer_update(((HvVmapple *)m->tick_opaque)->gic, cpu); /* its own timer: the GIC's lock only */
    hv_dev_lock(m);
    uint64_t deadline = tick_locked(m, cpu);
    hv_dev_unlock(m);
    return deadline;
}

int hv_vmapple_create(HvVmapple *b, HvMachine *m, HvCpu *cpu, uint64_t ram_size, HvDisk *root, HvDisk *aux,
                      const HvCfgParams *cfg, const uint8_t mac[6]) {
    memset(b, 0, sizeof *b);
    b->ram_fd = -1;
    b->m = m;
    b->cpu = cpu;
    b->ncpus = cfg->nr_cpus ? (int)cfg->nr_cpus : 1;
    if (b->ncpus > HV_MAX_CPUS) b->ncpus = HV_MAX_CPUS;
    b->ram_size = ram_size;
    /* HV_RAM_FILE: guest RAM in a file. iOS's memory limit counts anonymous
       pages and not pages of a mapped file, which it can also write out and
       drop by itself, so the guest can have more RAM than the app may hold.
       The file is unlinked at once and goes away with the process. */
    const char *ram_file = getenv("HV_RAM_FILE");
    b->ram = MAP_FAILED;
    if (!getenv("HV_RAM_NO_LEDGER")) {
        uint8_t *free_of_footprint = alloc_no_footprint(ram_size);
        if (free_of_footprint) b->ram = free_of_footprint;
    }
    if (b->ram == MAP_FAILED && ram_file && *ram_file) {
        int fd = open(ram_file, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            unlink(ram_file);
            if (ftruncate(fd, (off_t)ram_size) == 0)
                b->ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (b->ram != MAP_FAILED) b->ram_fd = fd; /* the balloon punches holes in it */
            else close(fd);
        }
        if (b->ram == MAP_FAILED) fprintf(stderr, "hengevm: guest RAM file %s failed: %s; using anonymous memory\n", ram_file, strerror(errno));
        else {
            fprintf(stderr, "hengevm: guest RAM (%llu MB) is backed by a file\n", (unsigned long long)(ram_size >> 20));
            if (!getenv("HV_RAM_NO_PREFAULT")) {
                struct PrefaultArgs *a = malloc(sizeof *a);
                a->ram = b->ram, a->size = ram_size;
                if (pthread_create(&b->prefault_thread, NULL, prefault_thread, a) == 0) b->prefault_started = 1;
                else free(a);
            }
        }
    }
    if (b->ram == MAP_FAILED) b->ram = mmap(NULL, ram_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (b->ram == MAP_FAILED) return -1;
    b->rom = mmap(NULL, ROM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (b->rom == MAP_FAILED) return -1;
    /* QEMU maps the firmware as RAM, and so do we. */
    hv_machine_add_ram(m, VMAPPLE_FIRMWARE, ROM_SIZE, b->rom, 0);
    hv_machine_add_ram(m, VMAPPLE_MEM, ram_size, b->ram, 0);

    b->gic = hv_gicv3_create(m, VMAPPLE_GIC_DIST, VMAPPLE_GIC_REDIST);
    hv_machine_add_mmio(m, VMAPPLE_GIC_MSI, 0x10000, b, gic_msi_read, gic_msi_write, "gic-msi");
    b->uart = hv_pl011_create(m, b->gic, VMAPPLE_UART, 32 + VMAPPLE_IRQ_UART);
    b->rtc = hv_pl031_create(m, b->gic, VMAPPLE_RTC, 32 + VMAPPLE_IRQ_RTC);
    b->gpio = hv_pl061_create(m, b->gic, VMAPPLE_GPIO, 32 + VMAPPLE_IRQ_GPIO);
    b->bdif = hv_bdif_create(m, VMAPPLE_BDOOR, root, aux);
    b->aes = hv_aes_create(m, b->gic, VMAPPLE_AES_1, VMAPPLE_AES_2, 32 + VMAPPLE_IRQ_AES);
    hv_machine_add_mmio(m, VMAPPLE_PVPANIC, 2, b, pvpanic_read, pvpanic_write, "pvpanic");
    b->pvg = hv_pvg_lite_create(m, b->gic, VMAPPLE_GFX, VMAPPLE_IOSFC, 32 + 0x11, 32 + 0x10);
    hv_machine_add_mmio(m, VMAPPLE_RTC_SELECT, 0x4000, b, rtc_select_read, rtc_select_write, "rtc-select");
    avp_regs_create(b, ram_size);
    HvCfgParams p = *cfg;
    p.ram_size = ram_size;
    b->cfg = hv_cfg_create(m, VMAPPLE_CONFIG, &p);
    b->pcie = hv_pcie_create(m, b->gic, VMAPPLE_PCIE_ECAM, 0x10000000, VMAPPLE_PCIE_MMIO, 0x1fff0000,
                             32 + VMAPPLE_IRQ_PCIE);
    /* Same order as QEMU's documented command line: aux, root, then the NIC. */
    if (aux) b->aux_blk = hv_virtio_blk_create(m, b->pcie, aux, HV_VBLK_AUX);
    if (root) b->root_blk = hv_virtio_blk_create(m, b->pcie, root, HV_VBLK_ROOT);
    if (mac) b->net = hv_virtio_net_create(m, b->pcie, mac);
    b->usb = hv_xhci_create(m, b->pcie);
    /* HV_INPUT_ORDER (experiment): "keyboard" = keyboard only, "swap" = mouse first. */
    const char *order = getenv("HV_INPUT_ORDER");
    if (order && !strcmp(order, "swap")) b->mouse = hv_virtio_input_create(m, b->pcie, HV_VINPUT_MOUSE);
    b->keyboard = hv_virtio_input_create(m, b->pcie, HV_VINPUT_KEYBOARD);
    if (!order) b->mouse = hv_virtio_input_create(m, b->pcie, HV_VINPUT_MOUSE);
    if (!getenv("HV_NO_SOUND")) b->snd = hv_virtio_snd_create(m, b->pcie);
    if ((aux && !b->aux_blk) || (root && !b->root_blk)) return -1;
    if (getenv("HV_BALLOON")) {
        b->balloon = hv_virtio_balloon_create(m, b->pcie, b->ram, VMAPPLE_MEM, ram_size, b->ram_fd);
        hv_virtio_balloon_set_target(b->balloon, (uint64_t)atoll(getenv("HV_BALLOON")) << 20);
    }
    /* Installer boots (the ROM goes to DFU) get the USB device port, as VZ gives it. */
    if (cfg->run_installer1 || cfg->run_installer2) {
        b->vusb = hv_virtio_usb_create(m, b->pcie);
        b->bdif->usb = b->vusb;
    }

    m->tick = tick;
    m->tick_opaque = b;
    cpu->m = m;
    return 0;
}

int hv_vmapple_load_rom(HvVmapple *b, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(b->rom, 1, ROM_SIZE, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    return n == 0 || more ? -1 : 0;
}

void hv_vmapple_reset_cpu(HvVmapple *b) {
    b->m->cntvoff = hv_counter();
    for (int i = 0; i < b->ncpus; i++) {
        HvCpu *c = &b->cpu[i];
        hv_cpu_reset_index(c, b->m, (uint32_t)i);
        c->mpidr = 0x80000000u | (uint32_t)i; /* Aff0 = the config's cpu_ids[i], bit 31 RES1 */
        c->pc = VMAPPLE_FIRMWARE;
        c->off = i != 0; /* secondaries wait for PSCI CPU_ON */
    }
}

void hv_vmapple_destroy(HvVmapple *b) {
    if (b->prefault_started) pthread_join(b->prefault_thread, NULL);
    /* Device workers and GPU completions can still touch RAM and the GIC. */
    hv_virtio_blk_destroy(b->root_blk);
    hv_virtio_blk_destroy(b->aux_blk);
    hv_pvg_lite_destroy(b->pvg);
    if (b->vusb) {
        hv_vusb_set_online(b->vusb, 0);
        pthread_mutex_destroy(&b->vusb->mu);
        pthread_cond_destroy(&b->vusb->cv);
        free(b->vusb);
    }
    if (b->snd) { free(b->snd->ring); free(b->snd); }
    free(b->balloon);
    if (b->keyboard) { pthread_mutex_destroy(&b->keyboard->mu); free(b->keyboard); }
    if (b->mouse) { pthread_mutex_destroy(&b->mouse->mu); free(b->mouse); }
    if (b->usb) { pthread_mutex_destroy(&b->usb->mu); free(b->usb); }
    if (b->gic) { pthread_mutex_destroy(&b->gic->mu); free(b->gic); }
    free(b->net);
    free(b->pcie);
    free(b->uart);
    free(b->rtc);
    free(b->gpio);
    free(b->bdif);
    free(b->aes);
    free(b->ctrr);
    free(b->sealed);
    free(b->cfg);
    if (b->rom && b->rom != MAP_FAILED) munmap(b->rom, ROM_SIZE);
    if (b->ram && b->ram != MAP_FAILED) munmap(b->ram, b->ram_size);
    if (b->ram_fd >= 0) close(b->ram_fd);
    memset(b, 0, sizeof *b);
    b->ram_fd = -1;
}
