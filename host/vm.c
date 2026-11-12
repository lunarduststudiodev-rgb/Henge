#include "vm.h"
#include "host_fp.h"
#include "sig_say.h"
#include "pvg_lite.h"

#include <TargetConditionals.h>
#include <CoreFoundation/CoreFoundation.h>
#include <os/lock.h>
#include <stdatomic.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <stdio.h>
#include <time.h>
#include <mach/mach_time.h>
#include "sysreg.h"

#include "usb_host.h"
#include "usb_storage.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio_mac.h"
#include "audio_usb.h"
#include "camera_apple.h"
#include "dispatch.h"
#include "net_slirp.h"
#include "net_forward.h"
#include "share.h"
#include "ramfile.h"
#include "snapshot.h"
#include "codestore.h"
#include "rosetta.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

static HvMachine m;
static HvCpu cpus[HV_MAX_CPUS]; /* the board takes them as an array */
static int ncpus = 1;
static HvVmapple board;
/* UI display changes may arrive before or during the background machine creation. */
static os_unfair_lock display_lock = OS_UNFAIR_LOCK_INIT;
static CFTypeRef display_layer;
static int display_layer_set;
static _Atomic(HvPvgLite *) display_pvg; /* published only when the board is ready */
static HvDisk disk, aux;
static int net_on;
/* The host's own network (hv_vm_host_network), kept for a machine built later: bit 63 up, the rest when it changed
   (virtio_net.h's clock). Unknown (0) until the host says, which leaves the cable in. */
static _Atomic uint64_t host_network;
static _Atomic(HvVirtioNet *) host_network_nic; /* the machine's, once it is built */
static void (*extra_poll)(void);
static int ts_forced = -1; /* HV_TOUCHSCREEN: 1 always plugged in, 0 never, -1 as the helper says */
static uint64_t ts_next_ns, ts_unconfirmed_since_ns, ts_replug_at_ns, ts_last_tick_ns; /* the touchscreen's, CPU 0's thread (and create) */
static int ts_replugged, ts_unplugged_to_replug;
static void (*exit_cb)(int);
static int vm_started, vm_paused; /* vm_paused: hv_vm_pause succeeded and no resume yet */
static unsigned vm_stops; /* hv_vm_stops */
static pthread_mutex_t pause_lock = PTHREAD_MUTEX_INITIALIZER; /* pause, resume and stop, one at a time */
/* Suspend: the snapshot hv_vm_suspend wrote, while the Mac has not run since (it goes the moment it does);
   restored: hv_vm_restore loaded the machine, which hv_vm_start lets go; broken: a restore failed halfway and
   the machine must never run. */
static char *suspended_path;
static int vm_restored, vm_broken;
static uint64_t vm_ecid, vm_seed;

HvMachine *hv_vm_machine(void) { return &m; }
HvVmapple *hv_vm_board(void) { return &board; }
HvCpu *hv_vm_cpu(int index) { return index >= 0 && index < ncpus ? &cpus[index] : NULL; }
int hv_vm_ncpus(void) { return ncpus; }

/* arm64 boot_args: {u16 Revision, u16 Version, u64 virtBase, physBase, memSize,
   topOfKernelData, Video {baseAddr, display, rowBytes, width, height, depth}, ...}.
   Found by scanning page starts for one whose physBase is the RAM base. */
int hv_vm_boot_display(HvBootDisplay *d, uint8_t *to, size_t cap) {
    /* What the guest's GPU has drawn, once it has drawn anything: copied under its lock, for the worker frees it
       when the size changes. */
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    if (!pvg) return -1;
    uint32_t fw, fh;
    int rc = hv_pvg_lite_frame(pvg, to, cap, &fw, &fh);
    if (rc >= 0) {
        d->width = fw, d->height = fh, d->stride = fw * 4;
        return rc;
    }
    static uint64_t found; /* RAM offset of boot_args */
    uint8_t *ram = board.ram;
    uint64_t size = board.ram_size;
    if (!ram) return -1;
    static double last_scan;
    for (int pass = 0; pass < 2; pass++) {
        if (!found || pass) {
            /* A full scan at most every 2 s; boot_args sits low in RAM
               (iBoot puts it near 0x78ed0000), so the first GB is enough. */
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            double now = ts.tv_sec + ts.tv_nsec * 1e-9;
            if (now - last_scan < 2) return -1;
            last_scan = now;
            if (pass && found) found = 0;
            uint64_t limit = size < (1ull << 30) ? size : 1ull << 30;
            for (uint64_t off = 0; off + 0x1000 <= limit && !found; off += 0x4000) {
                const uint64_t *q = (const uint64_t *)(ram + off);
                const uint16_t *h = (const uint16_t *)(ram + off);
                if (h[0] < 2 || h[0] > 3 || h[1] < 1 || h[1] > 3) continue; /* boot_args: revision, version */
                if (q[1] >> 40 != 0xfffffe || q[2] < VMAPPLE_MEM || q[2] >= VMAPPLE_MEM + size || q[3] == 0 || q[3] > size) continue;
                uint64_t base = q[5], row = q[7], w = q[8], hh = q[9], depth = q[10];
                if (w < 320 || w > 8192 || hh < 200 || hh > 8192 || row < w * 4 || (depth & 0xff) != 32) continue;
                if (base < VMAPPLE_MEM || base - VMAPPLE_MEM + row * hh > size) continue;
                found = off;
            }
        }
        if (!found) return -1;
        const uint64_t *q = (const uint64_t *)(ram + found);
        uint64_t base = q[5], row = q[7], w = q[8], hh = q[9];
        if (q[1] >> 40 == 0xfffffe && w >= 320 && w <= 8192 && hh >= 200 && hh <= 8192 && base >= VMAPPLE_MEM &&
            base - VMAPPLE_MEM + row * hh <= size) {
            d->width = (uint32_t)w, d->height = (uint32_t)hh, d->stride = (uint32_t)row;
            static uint64_t logged[4];
            if (logged[0] != w || logged[1] != hh || logged[2] != q[10] || logged[3] != q[6]) {
                logged[0] = w, logged[1] = hh, logged[2] = q[10], logged[3] = q[6];
                fprintf(stderr, "hengevm: boot display %llux%llu row %llu depth 0x%llx display 0x%llx (boot_args rev %u ver %u)\n",
                        (unsigned long long)w, (unsigned long long)hh, (unsigned long long)row, (unsigned long long)q[10],
                        (unsigned long long)q[6], ((const uint16_t *)q)[0], ((const uint16_t *)q)[1]);
            }
            if (!to || cap < row * hh) return 1;
            memcpy(to, ram + (base - VMAPPLE_MEM), row * hh); /* guest RAM, never freed: the guest may be drawing in it */
            return 0;
        }
    }
    return -1;
}
void hv_vm_set_poll(void (*poll)(void)) { extra_poll = poll; }

const char *hv_vm_reason_name(int r) {
    switch (r) {
    case HV_EXIT_HALT: return "halt";
    case HV_EXIT_WFI: return "wfi with nothing to wake it";
    case HV_EXIT_UNSUPPORTED: return "unsupported";
    case HV_EXIT_FAULT_LOOP: return "fault on the exception vector";
    default: return "other";
    }
}

static void touchscreen_tick(void);
static void park_tick(void);
static void power_tick(void);
static void volume_tick(void);

/* Host-side device work on CPU 0's thread each tick, under the host's FPCR (host/host_fp.h): the pointer's
   parking computes in floating point, and the network and the Mac's graphics poll may. */
static HV_HOST_FP_WORK void board_work(void) {
    if (extra_poll) extra_poll();
    if (net_on) hv_net_poll();
    touchscreen_tick();
    park_tick();
    power_tick();
    volume_tick();
}
static void board_poll(void) {
    HvHostFp fp = hv_host_fp_begin();
    board_work();
    hv_host_fp_end(fp);
}

static int fail(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
    return -1;
}

/* A branch landing where no translation is (an illegal instruction, or a fault
   fetching one): say where, and which direct branches in translated code lead
   there, for a crash report cannot. Through sig_say, not stdio: a thread holding stderr's lock would
   otherwise turn the crash into a hang (tools/check_signal_safe.py). */
static struct sigaction prior_ill, prior_bus, prior_segv;
#define zone_used_words(z) ((uint64_t)hv_zone_used(&m, (z)))
static void on_ill(int sig, siginfo_t *info, void *ctx) {
    if (sig == SIGILL && hv_jit_sigill(&m, ctx)) return; /* a guest instruction this Mac lacks: the guest's exception */
    hv_pvg_metal_crash_note(); /* the GPU driver stopped while building a pipeline: which shaders, in which slots */
    ucontext_t *uc = ctx;
    uint64_t pc = uc->uc_mcontext->__ss.__pc, rx = (uint64_t)(uintptr_t)m.jit.base, size = (uint64_t)m.jit.capacity * 4;
    uint64_t used = 0; /* bytes up to the end of the last code in use */
    for (int z = 0; z < m.nzones; z++)
        if (zone_used_words(z)) used = ((uint64_t)z * m.zone_words + zone_used_words(z)) * 4;
    int in_rx = pc >= rx && pc < rx + size;
    int zpc = in_rx ? hv_jit_zone_of(&m, (const void *)(uintptr_t)pc) : -1;
    int in_live = zpc >= 0 && (pc - rx) / 4 - (uint64_t)zpc * m.zone_words < zone_used_words(zpc);
    int fetch = (uint64_t)(uintptr_t)info->si_addr == pc;
    if (sig == SIGILL || in_rx || fetch) {
        sig_say("hengevm: signal %d in translated code: pc %llx (rx %llx size %llx used to %llx, pc is rx%+lld, zone %d of %d, %s), full flushes %llu, zones emptied %llu, lr %llx, addr %p\n",
                sig, (unsigned long long)pc, (unsigned long long)rx, (unsigned long long)size, (unsigned long long)used, (long long)(pc - rx),
                zpc, m.nzones, in_live ? "live code" : "no live code there", (unsigned long long)HV_STAT_TOTAL(&m, block_flushes),
                (unsigned long long)HV_STAT_TOTAL(&m, zone_evictions), (unsigned long long)uc->uc_mcontext->__ss.__lr, info->si_addr);
        for (int i = 0; i < 29; i++) sig_say("x%d=%llx%c", i, (unsigned long long)uc->uc_mcontext->__ss.__x[i], i % 6 == 5 ? '\n' : ' ');
        sig_say("\n");
        sig_say("sp=%llx ram %llx size %llx\n", (unsigned long long)uc->uc_mcontext->__ss.__sp, (unsigned long long)(uintptr_t)board.ram, (unsigned long long)board.ram_size);
        {
            uint64_t x7 = uc->uc_mcontext->__ss.__x[7], x28 = uc->uc_mcontext->__ss.__x[28];
            if (x7 >= x28 && x7 < x28 + sizeof(HvCpu)) {
                const uint64_t *ent = (const uint64_t *)(uintptr_t)(x7 + 0x500);
                sig_say("tlb entry at +%llx:", (unsigned long long)(x7 - x28));
                for (int i = 0; i < 16; i++) sig_say(" %llx", (unsigned long long)ent[i]);
                sig_say("\n");
            }
        }
        for (int i = 0; i < ncpus; i++)
            sig_say("  cpu %d: guest pc %llx el%u miss site %llx\n", i, (unsigned long long)cpus[i].pc, cpus[i].el, (unsigned long long)cpus[i].miss_site);
        if (in_rx) { /* the code it ran up to here, as it stands */
            uint64_t from = pc - 0x300 < rx ? rx : pc - 0x300;
            sig_say("  words from rx+%llx:", (unsigned long long)(from - rx));
            for (uint64_t at = from; at < pc + 0x600 && at < rx + size; at += 4) {
                if (((at - from) & 0x1f) == 0) sig_say("\n   ");
                sig_say(" %08x", *(const uint32_t *)(uintptr_t)at);
            }
            sig_say("\n");
        }
        { /* x15 after a chained exit's check: the literal it loaded. Which exits, and where do they go now? */
            uint64_t x15 = uc->uc_mcontext->__ss.__x[15];
            const uint32_t *w15 = (const uint32_t *)(uintptr_t)rx;
            uint64_t n15 = used / 4 < m.jit.capacity ? used / 4 : m.jit.capacity;
            int shown = 0;
            for (uint64_t k = 0; k + 8 < n15 && shown < 12; k++) {
                if ((w15[k] & 0xff00001fu) != 0x5800000fu) continue; /* ldr x15, literal */
                int64_t lo = (int64_t)(int32_t)(w15[k] << 8) >> 13;
                uint64_t at = k + (uint64_t)lo;
                if (at + 1 >= n15) continue;
                uint64_t lit = w15[at] | (uint64_t)w15[at + 1] << 32;
                if (lit != x15) continue;
                /* eor x6; orr; cbnz; B */
                uint32_t b = w15[k + 4];
                if ((b & 0xfc000000u) != 0x14000000u) continue;
                int64_t off = (int64_t)(b << 6) >> 6;
                sig_say("  exit at rx+%llx: B word %08x to rx+%llx\n", (unsigned long long)(k + 4) * 4, b, (unsigned long long)((int64_t)(k + 4) + off) * 4);
                shown++;
            }
        }
        if (!in_live) {
            /* Who branches there? */
            const uint32_t *w = (const uint32_t *)(uintptr_t)rx;
            int found = 0;
            uint64_t words = used / 4 < m.jit.capacity ? used / 4 : m.jit.capacity;
            for (uint64_t k = 0; k < words && found < 8; k++) {
                uint32_t i = w[k];
                int64_t off;
                if ((i & 0x7c000000u) == 0x14000000u) off = (int64_t)(i << 6) >> 6;
                else if ((i & 0xff000010u) == 0x54000000u || (i & 0x7e000000u) == 0x34000000u) off = (int64_t)(int32_t)(i << 8) >> 13;
                else if ((i & 0x7e000000u) == 0x36000000u) off = (int64_t)(int32_t)(i << 13) >> 18;
                else continue;
                if (rx + k * 4 + (uint64_t)(off * 4) != pc && rx + k * 4 + (uint64_t)(off * 4) != pc - 4) continue;
                found++;
                sig_say("  branch at rx+%llx (word %08x) leads there; around it:", (unsigned long long)k * 4, i);
                for (int64_t d = -6; d <= 3; d++)
                    if ((int64_t)k + d >= 0 && (uint64_t)((int64_t)k + d) < words) sig_say(" %08x", w[k + d]);
                sig_say("\n");
            }
            if (!found) sig_say("  no direct branch in %llu MB of code leads there\n", (unsigned long long)(used >> 20));
            /* Which slot or cache entry holds it? */
            for (int z = 0; z < m.nzones && m.slots; z++) /* the slots in use: an emptied zone's keep what they held */
                for (uint32_t k = (uint32_t)z * m.zone_slots, e = k + (z == m.zone ? (m.nslots < m.slot_limit ? m.nslots : m.slot_limit) - k : m.zones[z].nslots); k < e && k < m.max_slots; k++)
                    if (m.slots[k] == pc) sig_say("  chain slot %u (zone %d) holds it\n", k, z);
        }
    }
    struct sigaction *prior = sig == SIGILL ? &prior_ill : sig == SIGBUS ? &prior_bus : &prior_segv;
    sigaction(sig, prior, NULL);
}

/* A stop the GPU driver makes itself while building a pipeline, with a trap instruction or abort(): the same note,
   then the signal goes on to whoever had it before, exactly as if this handler were not here. On the iPad that is the
   JIT's trap guard (henge-ios-jit.c), which steps over its own probe's brk through this same context, and the log's
   abort writer (henge-logq.c); both are installed at app start, before this. The note says nothing on a thread that is
   not building a pipeline, so the JIT probe's trap prints nothing. */
static struct sigaction prior_trap, prior_abrt;
static void on_stop(int sig, siginfo_t *info, void *ctx) {
    hv_pvg_metal_crash_note();
    const struct sigaction *prior = sig == SIGTRAP ? &prior_trap : &prior_abrt;
    if (prior->sa_flags & SA_SIGINFO) {
        if (prior->sa_sigaction) prior->sa_sigaction(sig, info, ctx);
        return;
    }
    if (prior->sa_handler != SIG_DFL && prior->sa_handler != SIG_IGN) {
        prior->sa_handler(sig);
        return;
    }
    /* No handler before: the system's own action. A trap instruction (brk) runs again on return and stops there, as it
       would have; a signal that was sent (abort(), raise, kill) would end with this return, so it is sent again. Which
       it was is told by the word at the pc, not by si_code, which a signal sent to one thread may leave 0. */
    sigaction(sig, prior, NULL);
    if (sig == SIGTRAP && ctx) {
        uint32_t w = *(const uint32_t *)(uintptr_t)((ucontext_t *)ctx)->uc_mcontext->__ss.__pc;
        if ((w & 0xffe0001fu) == 0xd4200000u) return;
    }
    raise(sig);
}

/* Once per process. A second hv_vm_create (a retry after one that failed, say) would otherwise record these handlers
   as their own priors: on_ill would put itself back and the fault run again for good, a hang in place of a crash, and
   on_stop would call itself. */
static void install_fault_notes(void) {
    static atomic_flag done = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set(&done)) return;
    struct sigaction ill = {.sa_flags = SA_SIGINFO | SA_ONSTACK}; /* a CPU thread's SP may hold a guest address */
    ill.sa_sigaction = on_ill;
    sigemptyset(&ill.sa_mask);
    sigaction(SIGILL, &ill, &prior_ill);
    sigaction(SIGBUS, &ill, &prior_bus);
    sigaction(SIGSEGV, &ill, &prior_segv);
    struct sigaction stop = ill;
    stop.sa_sigaction = on_stop;
    sigaction(SIGTRAP, &stop, &prior_trap);
    sigaction(SIGABRT, &stop, &prior_abrt);
}

void (*hv_prof_register)(void);

void hv_vm_set_display_layer(void *layer) {
    CFTypeRef retained = layer ? CFRetain(layer) : NULL;
    os_unfair_lock_lock(&display_lock);
    CFTypeRef previous = display_layer;
    display_layer = retained;
    display_layer_set = 1;
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    if (pvg) hv_pvg_lite_set_layer(pvg, layer);
    os_unfair_lock_unlock(&display_lock);
    if (previous) CFRelease(previous);
}

uint64_t hv_vm_gpu_frames(void) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    return pvg ? hv_pvg_lite_gpu_frames(pvg) : 0;
}

void hv_vm_gfx_stages(uint32_t ms[5]) {
    _Static_assert(HV_PVG_STAGES == 5, "hv_vm_gfx_stages hands over five stages");
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    if (pvg) hv_pvg_lite_stages(pvg, ms);
    else memset(ms, 0, 5 * sizeof *ms);
}

int hv_vm_gpu_code_refused(void) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    return pvg ? hv_pvg_lite_gpu_code_refused(pvg) : 0;
}

int hv_vm_gpu_state(void) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    return pvg ? hv_pvg_lite_gpu_state(pvg) : HV_PVG_GPU_RUNS;
}

int hv_vm_gpu_try_again(void) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    return pvg ? hv_pvg_lite_gpu_try_again(pvg) : -1;
}

int hv_vm_pointer_shown(void) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    return pvg ? hv_pvg_lite_pointer_shown(pvg) : 0;
}

/* The folder the Mac gets in "My Shared Files", and the Henge Tools disk, set before hv_vm_create. */
static char *folder_shared, *folder_name, *tools_path;
static int vm_installer; /* an installer boot: no USB drives */

void hv_vm_set_folders(const char *shared_name, const char *shared, const char *tools_disk_path) {
    free(folder_shared), free(folder_name), free(tools_path);
    folder_name = strdup(shared_name && *shared_name ? shared_name : "Shared");
    folder_shared = shared && *shared ? strdup(shared) : NULL;
    tools_path = tools_disk_path && *tools_disk_path ? strdup(tools_disk_path) : NULL;
}

/* The host's battery, kept for a board built after it is set. */
static pthread_mutex_t power_mu = PTHREAD_MUTEX_INITIALIZER;
static HvPowerState power_state;
static int power_known;
static HvPowerSource *power_dev; /* under power_mu */

void hv_vm_set_power(double percent, int external, int charging, int full) {
    pthread_mutex_lock(&power_mu);
    power_state = (HvPowerState){.percent = percent, .external = external != 0, .charging = charging != 0, .full = full != 0};
    power_known = 1;
    if (power_dev) hv_power_source_set(power_dev, &power_state);
    pthread_mutex_unlock(&power_mu);
    hv_share_set_battery(percent, external, charging, full);
}

static void add_power_source(int which) {
    if (which != HV_VM_POWER_BATTERY && which != HV_VM_POWER_WALL) return;
    if (hv_vmapple_add_power_source(&board, which == HV_VM_POWER_BATTERY ? HV_POWER_BATTERY : HV_POWER_WALL) != 0) {
        fprintf(stderr, "hengevm: no power source for the Mac\n");
        return;
    }
    pthread_mutex_lock(&power_mu);
    power_dev = board.power;
    if (power_known) hv_power_source_set(power_dev, &power_state);
    pthread_mutex_unlock(&power_mu);
}

/* The USB speaker's name on the Mac (the host's: "iPad"), set before hv_vm_create. */
static char speaker_name[64];

void hv_vm_set_speaker_name(const char *name) { snprintf(speaker_name, sizeof speaker_name, "%s", name ? name : ""); }

int hv_vm_create(const HvVmConfig *c, char *err, size_t errlen) {
    if (!c->rom || !c->disk || !c->aux) return fail(err, errlen, "The ROM, disk and auxiliary storage are all needed.");
    install_fault_notes();
    signal(SIGPIPE, SIG_IGN); /* a guest connection closing at the far end must not end the machine */
    /* The Mac's counter (its mach_absolute_time, and so its uptime and how long macOS takes the user to have been
       away) starts near 0, as a Mac's does at power-on, not at the iPad's own uptime: days, with which macOS starts
       up believing nobody has touched it for that long. All the host's time so far counts as time the Mac did not
       see, as a pause does (sysreg.h), set before any device or CPU reads the counter. A restored Mac sets its own
       (hv_machine_restore_paused). HV_HOST_UPTIME=1: as before. */
    if (!getenv("HV_HOST_UPTIME")) __atomic_store_n(&hv_time_bias, mach_absolute_time(), __ATOMIC_SEQ_CST);
    if (hv_disk_open(&disk, c->disk, 0, 0) != 0) return fail(err, errlen, "Cannot open the disk image.");
    if (hv_disk_open(&aux, c->aux, c->aux_offset ? c->aux_offset : 16384, 0) != 0)
        return fail(err, errlen, "Cannot open the auxiliary storage.");
    size_t jit = c->jit_bytes ? c->jit_bytes : (size_t)768 << 20;
    int r = c->jit_rx ? hv_machine_init_jit(&m, c->jit_rx, c->jit_rw, jit) : hv_machine_init(&m, jit);
    if (r != 0) return fail(err, errlen, "Cannot set up the JIT region.");
    if (c->rosetta && hv_machine_set_rosetta(&m, c->rosetta) != 0)
        return fail(err, errlen, "This device can't run the experimental Rosetta mode. Turn it off in this Mac's settings.");
    if (m.rosetta) fprintf(stderr, "hengevm: the experimental Rosetta mode is on (%d)\n", m.rosetta);
    vm_ecid = c->ecid, vm_seed = c->seed;
    m.log = c->log;
    m.gen_checks = getenv("HV_JIT_POISON") != NULL;
    m.rewrite_hash = getenv("HV_REWRITE_HASH") != NULL; /* HvStats.rewrite_*: off by default, a cost on code writes */
    if (getenv("HV_SLOW_MEM")) m.slow_mem = 1; /* debugging: every load and store through the helper */
    ncpus = c->ncpus < 1 ? 1 : c->ncpus > HV_MAX_CPUS ? HV_MAX_CPUS : c->ncpus;
    m.big_lock_on = ncpus > 1 && getenv("HV_BIG_LOCK"); /* debugging: one CPU in guest code at a time */
    HvCfgParams cfg = {.nr_cpus = (uint32_t)ncpus, .ecid = c->ecid, .seed = c->seed, .mac_en0 = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01}};
    if (c->installer) cfg.run_installer1 = cfg.run_installer2 = 1;
    if (getenv("HV_RUN_INSTALLER")) { /* experiment: "a,b" for the two installer words */
        unsigned a = 0, b2 = 0;
        sscanf(getenv("HV_RUN_INSTALLER"), "%u,%u", &a, &b2);
        cfg.run_installer1 = a, cfg.run_installer2 = b2;
    }
    static const uint8_t mac[6] = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01};
    if (hv_vmapple_create(&board, &m, cpus, (c->ram_mb ? c->ram_mb : 2048) << 20, &disk, &aux, &cfg, mac) != 0)
        return fail(err, errlen, "Cannot build the machine (not enough memory for the guest's RAM?).");
    if (hv_vmapple_load_rom(&board, c->rom) != 0) return fail(err, errlen, "Cannot read the ROM (AVPBooter.vmapple2.bin).");
    hv_vmapple_reset_cpu(&board);
    if (!c->installer) add_power_source(c->power_source); /* before the shared folders: it takes the next PCI slot */
    if (folder_shared && !c->installer) {
        const char *const names[] = {folder_name};
        const char *const paths[] = {folder_shared};
        static const int readonly[] = {0};
        int n = hv_vmapple_share_folders(&board, names, paths, readonly, 1);
        fprintf(stderr, "hengevm: %d folder%s for My Shared Files\n", n, n == 1 ? "" : "s");
    }
    vm_installer = c->installer;
    {
        const char *e = getenv("HV_TOUCHSCREEN"); /* testing: 1 plugs it in from the start, 0 never */
        __atomic_store_n(&ts_forced, e ? (atoi(e) ? 1 : 0) : -1, __ATOMIC_RELAXED);
        ts_next_ns = ts_unconfirmed_since_ns = ts_replug_at_ns = ts_last_tick_ns = 0;
        ts_replugged = ts_unplugged_to_replug = 0;
    }
    if (tools_path && !c->installer && hv_vm_plug_disk(tools_path, 1, "Henge Tools", HV_VM_TOOLS_SERIAL) < 0)
        fprintf(stderr, "hengevm: cannot plug in the Henge Tools disk (%s)\n", tools_path);
    if (board.net) { /* the cable as the host's network has it: out from the start on an iPad in airplane mode */
        uint64_t h = atomic_load(&host_network);
        if (h) hv_virtio_net_set_carrier(board.net, (int)(h >> 63), h & ~(1ull << 63));
        atomic_store(&host_network_nic, board.net);
        h = atomic_load(&host_network); /* a change while it was being published */
        if (h) hv_virtio_net_set_carrier(board.net, (int)(h >> 63), h & ~(1ull << 63));
    }
    if (board.net && !c->no_net) {
        if (hv_net_attach(&m, board.net) != 0) fprintf(stderr, "hengevm: user-mode network unavailable\n");
        else net_on = 1;
    }
    if (board.snd && !c->no_sound) hv_audio_attach(board.snd);
    HvUsbAudio *spk = board.usb ? hv_xhci_speaker(board.usb) : NULL;
    if (spk) {
        hv_usb_audio_set_name(spk, speaker_name);
        if (!c->no_sound) hv_audio_usb_attach(spk);
    }
    if (board.camera) hv_camera_attach(board.camera);
    board.poll = board_poll;
    if (board.vusb && getenv("HV_VUSB_PROBE")) hv_usb_probe_start(board.vusb);
    // The setter and publication share this lock: an older preboot target can
    // never overwrite a monitor change made while the board was being built.
    os_unfair_lock_lock(&display_lock);
    if (display_layer_set && board.pvg) hv_pvg_lite_set_layer(board.pvg, (void *)display_layer);
    atomic_store_explicit(&display_pvg, board.pvg, memory_order_release);
    os_unfair_lock_unlock(&display_lock);
    return 0;
}

static pthread_t secondary_threads[HV_MAX_CPUS], kicker;
static int secondary_started[HV_MAX_CPUS], kicker_started, vm_finished;
static int kicker_stop;

static void *kicker_thread(void *unused) {
    (void)unused;
    while (!__atomic_load_n(&kicker_stop, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&vm_paused, __ATOMIC_ACQUIRE)) { /* nothing runs: no need to wake every millisecond */
            usleep(20000);
            continue;
        }
        usleep(1000);
        hv_machine_kick_all(&m);
    }
    return NULL;
}

/* A thread that runs guest code. While a block runs its SP register holds the
   guest's SP, a guest address, so the kernel must never push a signal frame
   there: other threads take the asynchronous signals, and a handler that does
   run here (a fault) gets a stack of its own. */
void hv_vm_prepare_cpu_thread(void) {
    static __thread int done;
    if (done) return;
    done = 1;
    if (hv_prof_register) hv_prof_register();
    stack_t ss = {.ss_sp = malloc(1u << 16), .ss_size = 1u << 16, .ss_flags = 0};
    if (ss.ss_sp) sigaltstack(&ss, NULL);
    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGSEGV), sigdelset(&set, SIGBUS), sigdelset(&set, SIGILL), sigdelset(&set, SIGTRAP), sigdelset(&set, SIGFPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

/* iOS parks default-QoS threads on efficiency cores readily; the guest's CPUs are what the user waits on. */
void hv_vm_cpu_qos(void) {
    hv_vm_prepare_cpu_thread();
    /* HV_QOS=ui|init|default (testing): the class the guest's CPUs run at. */
    const char *q = getenv("HV_QOS");
#if TARGET_OS_IPHONE
    qos_class_t c = QOS_CLASS_USER_INITIATED;
#else
    qos_class_t c = QOS_CLASS_DEFAULT;
#endif
    if (q && !strcmp(q, "ui")) c = QOS_CLASS_USER_INTERACTIVE;
    else if (q && !strcmp(q, "init")) c = QOS_CLASS_USER_INITIATED;
    else if (q && !strcmp(q, "default")) c = QOS_CLASS_DEFAULT;
    if (c != QOS_CLASS_DEFAULT) pthread_set_qos_class_self_np(c, 0);
}

/* CPUs 1 and up: run whenever PSCI CPU_ON starts them; the machine stops with CPU 0. */
static void *secondary_thread(void *arg) {
    HvCpu *c = arg;
    hv_vm_cpu_qos();
    int r = hv_run(c);
    if (!c->halted)
        fprintf(stderr, "hengevm: cpu %u stopped (%s) at pc %llx el%u\n", c->index, hv_vm_reason_name(r),
                (unsigned long long)c->pc, c->el);
    return NULL;
}

void hv_vm_flush_disks(void) {
    hv_disk_flush(&disk);
    hv_disk_flush(&aux);
    hv_xhci_flush_drives(board.usb, 0);
}

/* USB drives, plugged in and out while the Mac runs (devices/xhci.c, usb_storage.c). */
int hv_vm_plug_disk(const char *path, int readonly, const char *name, const char *serial) {
    if (!board.usb || vm_installer) return -3;
    return hv_xhci_plug(board.usb, path, readonly, name, serial);
}

void hv_vm_unplug_disk(int port) { hv_xhci_unplug(board.usb, port); }
int hv_vm_disk_state(int port) { return board.usb ? hv_xhci_drive_state(board.usb, port) : HV_XHCI_DRIVE_EMPTY; }
int hv_vm_disk_readonly(int port) { return board.usb ? hv_xhci_drive_readonly(board.usb, port) : 0; }

int hv_vm_disk_port(const char *serial) {
    if (!board.usb || !serial) return -1;
    HvXhci *x = board.usb;
    int port = -1;
    pthread_mutex_lock(&x->mu);
    for (int p = 1; p < HV_XHCI_PORTS && port < 0; p++) {
        struct HvUsbStorage *d = x->port[p].drive ? x->port[p].drive : x->port[p].plug;
        if (d && !x->port[p].unplug && !strcmp(hv_usb_storage_serial(d), serial)) port = p + 1;
    }
    pthread_mutex_unlock(&x->mu);
    return port;
}

int hv_vm_run_cpu0(void) {
    return hv_run(&cpus[0]);
}

/* CPU 0 has left (macOS shut down or restarted, hv_vm_stop, or an error): the rest of the machine stops
   with it before anything is said to have stopped. Every CPU gets a reason (one parked in CPU_OFF is woken
   for it) and its thread is joined, so none still runs guest code; the disks' workers finish what the guest
   handed them; then everything written is flushed. Once; CPU 0's thread. */
static int devices_idle(unsigned timeout_ms);

void hv_vm_finish(void) {
    if (vm_finished) return;
    vm_finished = 1;
    hv_machine_halt(&m, HV_HALT_HOST); /* no-op for a CPU that has its reason already (macOS's own) */
    for (int i = 1; i < ncpus; i++)
        if (secondary_started[i]) pthread_join(secondary_threads[i], NULL), secondary_started[i] = 0;
    __atomic_store_n(&kicker_stop, 1, __ATOMIC_RELEASE);
    if (kicker_started) pthread_join(kicker, NULL), kicker_started = 0;
    /* No CPU can hand the devices more work now. A write the guest was told was done is on its way to the
       file, and one still in a worker's hands lands before the flush rather than after it (or never). */
    if (devices_idle(10000) != 0) fprintf(stderr, "hengevm: a disk request was still in progress as the Mac stopped\n");
    hv_vm_flush_disks();
    if (net_on) hv_net_forwards_close(); /* the Mac's ports on the device go with it */
    static const char *const why[] = {"", "macOS shut down", "macOS restarted", "stopped by Henge or an error", "macOS panicked"};
    int by = hv_vm_halted_by();
    if (by > 0 && by < (int)(sizeof why / sizeof *why))
        fprintf(stderr, "hengevm: the Mac has stopped (%s): CPUs ended, disks flushed\n", why[by]);
}

static void *cpu0_thread(void *unused) {
    (void)unused;
    hv_vm_cpu_qos();
    int r = hv_vm_run_cpu0();
    hv_vm_finish();
    if (exit_cb) exit_cb(r);
    return NULL;
}

/* The Mac's memory when it is kept in files (core/ramfile.h, HV_RAM_SAVE_AHEAD), else NULL. */
static HvRamFile *ram_file(void) {
    for (int i = 0; i < m.nram; i++)
        if (m.ram[i].host == board.ram && m.ram[i].file) return m.ram[i].file;
    return NULL;
}

int hv_vm_memory_copyable(void) { return ram_file() && hv_ramfile_copyable(); }

int hv_vm_prepare_memory(void) {
    HvRamFile *f = ram_file();
    if (!f) return 0;
    /* Starting afresh where a resume put a saved copy (and was then refused): zeros, as any fresh start has */
    return !vm_restored && hv_ramfile_make_fresh(f) != 0 ? -1 : 0;
}

static void *resume_watch(void *unused);

void hv_vm_start(int cpu0, void (*on_exit)(int reason)) {
    if (vm_broken) { /* half restored: running it would run a Mac that never was */
        fprintf(stderr, "hengevm: not starting: the restore failed partway\n");
        return;
    }
    if (hv_vm_prepare_memory() != 0) {
        fprintf(stderr, "hengevm: not starting: the memory files couldn't be made ready\n");
        return;
    }
    exit_cb = on_exit;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    /* The handles CPU 0 joins as the machine stops are all here before it can start. */
    for (int i = 1; i < ncpus; i++)
        secondary_started[i] = pthread_create(&secondary_threads[i], &attr, secondary_thread, &cpus[i]) == 0;
    kicker_started = pthread_create(&kicker, NULL, kicker_thread, NULL) == 0;
    pthread_t t;
    if (cpu0 && pthread_create(&t, &attr, cpu0_thread, NULL) == 0) pthread_detach(t);
    pthread_attr_destroy(&attr);
    vm_started = 1;
    if (ram_file()) hv_ramfile_writer_start(ram_file());
    if (vm_restored) { /* loaded paused (hv_snap_get_cpus): it goes on from where it was saved */
        vm_restored = 0;
        hv_machine_resume(&m);
        fprintf(stderr, "hengevm: running on from the snapshot\n");
        pthread_t w;
        if (pthread_create(&w, NULL, resume_watch, NULL) == 0) pthread_detach(w);
    }
}

void hv_vm_stop(void) {
    pthread_mutex_lock(&pause_lock);
    /* The dispatchers stop at the next block; asleep, paused or in CPU_OFF, they are woken to. A reason
       macOS gave first (shut down, restart) is kept. */
    hv_machine_halt(&m, HV_HALT_HOST);
    pthread_mutex_unlock(&pause_lock);
}

/* Until the disks' (and the shared folder's) workers have done every request the guest handed them,
   or timeout_ms. CPUs paused: nothing new comes in. 0 when idle. */
static int devices_idle(unsigned timeout_ms) {
    HvVirtioBlk *blks[] = {board.root_blk, board.aux_blk};
    for (unsigned waited = 0;; waited++) {
        int busy = 0;
        for (size_t i = 0; i < sizeof blks / sizeof *blks; i++)
            if (blks[i] && __atomic_load_n(&blks[i]->completed, __ATOMIC_ACQUIRE) != __atomic_load_n(&blks[i]->submitted, __ATOMIC_ACQUIRE)) busy = 1;
        if (board.fs && __atomic_load_n(&board.fs->completed, __ATOMIC_ACQUIRE) != __atomic_load_n(&board.fs->submitted, __ATOMIC_ACQUIRE)) busy = 1;
        if (hv_xhci_drives_busy(board.usb)) busy = 1;
        if (!busy) return 0;
        if (waited >= timeout_ms) return -1;
        usleep(1000);
    }
}

/* hv_vm_pause with pause_lock held. */
static int pause_l(unsigned timeout_ms) {
    if (!vm_started || cpus[0].halted) return -2;
    if (vm_paused) return 0;
    uint64_t t0 = mach_absolute_time();
    __atomic_add_fetch(&vm_stops, 1, __ATOMIC_RELEASE); /* odd: stopping, or stopped */
    hv_machine_pause(&m);
    if (hv_machine_pause_wait(&m, timeout_ms) != 0) {
        hv_machine_resume(&m);
        __atomic_add_fetch(&vm_stops, 1, __ATOMIC_RELEASE);
        fprintf(stderr, "hengevm: pause: the CPUs did not stop within %u ms; the Mac runs on\n", timeout_ms);
        return -1;
    }
    __atomic_store_n(&vm_paused, 1, __ATOMIC_RELEASE);
    /* The memory stands still: the background writer stops (a save writes the rest itself, on the storage alone) */
    if (ram_file()) hv_ramfile_writer_hold(ram_file(), 1);
    uint64_t t1 = mach_absolute_time();
    if (board.snd) hv_audio_pause(1);
    hv_audio_usb_pause(1);
    hv_camera_pause(1);
    int safe = 1;
    if (devices_idle(5000) != 0) {
        safe = 0;
        fprintf(stderr, "hengevm: pause: a disk request is still in progress\n");
    }
    if (hv_disk_sync(&disk) != 0 || hv_disk_sync(&aux) != 0 || hv_xhci_flush_drives(board.usb, 1) != 0) {
        safe = 0;
        fprintf(stderr, "hengevm: pause: the disk could not be synced to storage\n");
    }
    uint64_t t2 = mach_absolute_time();
    fprintf(stderr, "hengevm: paused (CPUs stopped in %.1f ms, disks on storage in %.1f ms)%s\n", (t1 - t0) / 24000.0,
            (t2 - t1) / 24000.0, safe ? "" : ", with a problem above");
    return safe ? 0 : 1;
}

int hv_vm_pause(unsigned timeout_ms) {
    pthread_mutex_lock(&pause_lock);
    int rc = pause_l(timeout_ms);
    pthread_mutex_unlock(&pause_lock);
    return rc;
}

/* hv_vm_resume with pause_lock held. Every stop here is a pause to the guest, an auto-save's too: its time is taken
   off the guest's clock and macOS rereads the real time. (A stop the guest's clock may run on through is one bounded
   by deadlines, which this save's is not: core/dispatch.c hv_machine_resume_live.) */
static void resume_l(void) {
    if (vm_paused && suspended_path) {
        /* The Mac runs on in this process: what was saved is out of date from its first instruction. */
        if (hv_snap_discard(suspended_path) != 0)
            fprintf(stderr, "hengevm: the suspended state could not be deleted: it will be refused (its disks change)\n");
        free(suspended_path);
        suspended_path = NULL;
    }
    /* (A save keeps its own copy of the memory files: the Mac running on here changes nothing it holds.) */
    HvRamFile *rf = vm_paused ? ram_file() : NULL;
    if (rf) hv_ramfile_writer_hold(rf, 0);
    if (vm_paused) {
        hv_vmapple_snap_drop_capture(&board); /* out of date from the first instruction, as a suspend is */
        if (board.snd) hv_audio_pause(0);
        hv_audio_usb_pause(0);
        hv_camera_pause(0);
        __atomic_store_n(&vm_paused, 0, __ATOMIC_RELEASE);
        hv_machine_resume(&m);
        /* macOS's clock stood still while paused: it rereads the real time, as after the host slept. */
        hv_vmapple_rtc_event(&board, HV_AVP_RTC_WOKE);
        __atomic_add_fetch(&vm_stops, 1, __ATOMIC_RELEASE); /* even: running */
        fprintf(stderr, "hengevm: resumed\n");
    }
}

void hv_vm_resume(void) {
    pthread_mutex_lock(&pause_lock);
    resume_l();
    pthread_mutex_unlock(&pause_lock);
}

int hv_vm_paused(void) { return __atomic_load_n(&vm_paused, __ATOMIC_ACQUIRE); }
unsigned hv_vm_stops(void) { return __atomic_load_n(&vm_stops, __ATOMIC_ACQUIRE); }

int hv_vm_halted_by(void) { return vm_started ? (int)__atomic_load_n(&cpus[0].halted, __ATOMIC_ACQUIRE) : 0; }

/* ---- Suspend (core/snapshot.h, devices/vmapple_snap.c) ---- */

#define S_HOST HV_SNAP_TAG('H', 'O', 'S', 'T')
#define HOST_VERSION 1
/* Raised when the engine changes in a way the snapshot's own section versions don't show (how guest code
   runs, what a device does with the same saved state): a snapshot from before is then refused, not run
   wrongly. An update that keeps it lets a suspended Mac go on in the new Henge. */
#define HV_VM_SUSPEND_COMPAT 1
#define SUSPEND_THREADS 4

static double ms_since(uint64_t t0) { return (double)(mach_absolute_time() - t0) / 24000.0; }

static int say_why(char *why, size_t n, const char *msg) {
    if (why && n) snprintf(why, n, "%s", msg);
    fprintf(stderr, "hengevm: %s\n", msg);
    return -1;
}

/* Which file a disk image is, and as it was: the same inode (not a copy put in its place), the same size,
   and not written since (its modification time, to the nanosecond). A Mac restored over a disk that changed
   after it was saved would wreck that disk's file system, so anything else is refused. */
typedef struct {
    uint64_t ino, size;
    int64_t mtime_ns;
} DiskId;

static int disk_id(const HvDisk *d, DiskId *id) {
    struct stat st;
    if (fstat(d->fd, &st) != 0) return -1;
    id->ino = (uint64_t)st.st_ino;
    id->size = (uint64_t)st.st_size;
    id->mtime_ns = (int64_t)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec;
    return 0;
}

static void put_id(HvSnapOut *o, const DiskId *id) {
    HV_SNAP_OUT(o, id->ino);
    HV_SNAP_OUT(o, id->size);
    HV_SNAP_OUT(o, id->mtime_ns);
}

static void get_id(HvSnapIn *in, DiskId *id) {
    HV_SNAP_IN(in, id->ino);
    HV_SNAP_IN(in, id->size);
    HV_SNAP_IN(in, id->mtime_ns);
}

static int same_id(const DiskId *a, const DiskId *b) {
    return a->ino == b->ino && a->size == b->size && a->mtime_ns == b->mtime_ns;
}

/* What the snapshot was taken of: the engine, the machine's identity and shape, and its two disks. */
/* An auto-save names its copies of the disks (clone_d, clone_a), not the disks themselves. */
static int put_host(HvSnapWriter *w, const DiskId *clone_d, const DiskId *clone_a) {
    HvSnapOut o = {0};
    DiskId di, ai;
    if (disk_id(&disk, &di) != 0 || disk_id(&aux, &ai) != 0) return -1;
    if (clone_d) di = *clone_d;
    if (clone_a) ai = *clone_a;
    uint32_t compat = HV_VM_SUSPEND_COMPAT, n = (uint32_t)ncpus;
    uint64_t ram = board.ram_size;
    HV_SNAP_OUT(&o, compat);
    HV_SNAP_OUT(&o, n);
    HV_SNAP_OUT(&o, ram);
    HV_SNAP_OUT(&o, vm_ecid);
    HV_SNAP_OUT(&o, vm_seed);
    put_id(&o, &di);
    put_id(&o, &ai);
    return hv_snap_put(w, S_HOST, HOST_VERSION, &o);
}

/* 0 if this machine is the one the snapshot was taken of, else -1 and why. Nothing is changed. */
static int check_host(HvSnapReader *r, char *why, size_t n) {
    HvSnapIn in;
    if (hv_snap_get(r, S_HOST, HOST_VERSION, &in) != 0)
        return say_why(why, n, "The saved state is from another version of Henge and can't be loaded.");
    uint32_t compat = 0, cpus = 0;
    uint64_t ram = 0, ecid = 0, seed = 0;
    DiskId di = {0}, ai = {0}, dn, an;
    HV_SNAP_IN(&in, compat);
    HV_SNAP_IN(&in, cpus);
    HV_SNAP_IN(&in, ram);
    HV_SNAP_IN(&in, ecid);
    HV_SNAP_IN(&in, seed);
    get_id(&in, &di);
    get_id(&in, &ai);
    if (hv_snap_in_done(&in) != 0) return say_why(why, n, "The saved state is damaged.");
    if (compat != HV_VM_SUSPEND_COMPAT)
        return say_why(why, n, "The saved state is from another version of Henge and can't be loaded.");
    if (ecid != vm_ecid || seed != vm_seed) return say_why(why, n, "The saved state is of another Mac.");
    if ((int)cpus != ncpus || ram != board.ram_size)
        return say_why(why, n, "The saved state had another number of processors or amount of memory.");
    if (disk_id(&disk, &dn) != 0 || disk_id(&aux, &an) != 0) return say_why(why, n, "The Mac's disk can't be read.");
    if (!same_id(&di, &dn)) return say_why(why, n, "The Mac's disk has changed since it was suspended.");
    if (!same_id(&ai, &an)) return say_why(why, n, "The Mac's startup settings have changed since it was suspended.");
    return 0;
}

/* Counts hv_vm_suspend_cancel: a suspend whose ticket (the count when it was asked for) is out of date stops */
static uint64_t suspend_cancels;

uint64_t hv_vm_suspend_room(void) { return hv_vmapple_save_ram_room(&board); }

uint64_t hv_vm_suspend_ticket(void) { return __atomic_load_n(&suspend_cancels, __ATOMIC_ACQUIRE); }

void hv_vm_suspend_cancel(void) { __atomic_fetch_add(&suspend_cancels, 1, __ATOMIC_ACQ_REL); }

int hv_vm_suspend(const char *path, unsigned timeout_ms, char *why, size_t why_len) {
    return hv_vm_suspend_ticketed(path, timeout_ms, hv_vm_suspend_ticket(), why, why_len);
}

/* Counts hv_vm_autosave_hold: an auto-save whose hold ticket is out of date leaves the Mac paused when it ends */
static uint64_t autosave_holds;

uint64_t hv_vm_autosave_hold_ticket(void) { return __atomic_load_n(&autosave_holds, __ATOMIC_ACQUIRE); }

void hv_vm_autosave_hold(void) { __atomic_fetch_add(&autosave_holds, 1, __ATOMIC_ACQ_REL); }

/* Suspend, or (disk_to and aux_to set) an auto-save: the same save, with the disks cloned to those paths at the
   same instant and the snapshot naming the clones. A Suspend returns with the Mac paused (-2: it isn't). An
   auto-save is taken only of a Mac running as the person left it, never pauses one already paused, and never
   writes the memory whole (a minute's stop on the screen): it fails instead. It returns with the Mac running again
   (unless held, below), and *paused_here says whether it paused it at all. */
static int save(const char *path, unsigned timeout_ms, uint64_t ticket, const char *disk_to, const char *aux_to,
                uint64_t hold_ticket, int *paused_here, char *why, size_t why_len) {
    if (why && why_len) why[0] = 0;
    if (paused_here) *paused_here = 0;
#define CANCELLED() (__atomic_load_n(&suspend_cancels, __ATOMIC_ACQUIRE) != ticket)
    const int autosave = disk_to != NULL;
    pthread_mutex_lock(&pause_lock);
    if (autosave) {
        /* Checked and paused under one hold of the lock: a pause or Suspend can't land between the two and then be
           undone by this one's resume. Not a Mac the person paused (it would go on by itself after), not an
           installer, not one half restored or suspended. */
        const char *no = !vm_started || cpus[0].halted ? "The Mac isn't running."
                         : vm_paused || suspended_path ? "The Mac is paused."
                         : vm_installer ? "An installer isn't auto-saved."
                         : vm_broken ? "The Mac isn't running."
                         : CANCELLED() ? "Cancelled before it started."
                         : !hv_vm_memory_copyable() ? "The memory can't be copied this run (the log says why before)."
                         : NULL;
        if (no) {
            pthread_mutex_unlock(&pause_lock);
            say_why(why, why_len, no);
            return CANCELLED() ? -3 : -2;
        }
    }
    uint64_t t_stop = mach_absolute_time(); /* the CPUs stop from here: the stop the guest's clock would show */
    int p = pause_l(timeout_ms);
    if (p < 0) {
        pthread_mutex_unlock(&pause_lock);
        say_why(why, why_len, p == -1 ? "The Mac didn't stop in time." : "The Mac isn't running.");
        return -2;
    }
    if (paused_here) *paused_here = autosave;
    uint64_t t0 = mach_absolute_time(), t_dev = 0, t_sync = 0, t_ram = 0, t = 0;
    int rc = -1, cancelled = 0, cloned = 0;
    HvCodeKey code_key; /* a Suspend's: names the translated code kept beside it (core/codestore.h) */
    if (!autosave) hv_code_key_new(&code_key);
    DiskId cd, ca;
    HvSnapWriter *w = NULL;
    if (CANCELLED()) cancelled = 1;
    else if (p == 1) say_why(why, why_len, "The Mac's disk could not be saved to storage.");
    else if (hv_vmapple_snap_can_save(&board, why, why_len) != 0) fprintf(stderr, "hengevm: suspend: %s\n", why);
    else if (!(w = hv_snap_begin(path, autosave ? HV_SNAP_NO_WHOLE : 0)))
        say_why(why, why_len, "The file for the saved state can't be made.");
    else if (hv_snap_set_cancel(w, &suspend_cancels, ticket), t = mach_absolute_time(), hv_vmapple_snap_save(&board, w) != 0) {
        if (CANCELLED()) cancelled = 1;
        else say_why(why, why_len, hv_vmapple_snap_save_why(&board));
    }
    else if (t_dev = mach_absolute_time() - t, CANCELLED()) cancelled = 1;
    /* The devices' save let requests under way finish: those writes go to storage before the disks are
       described, so the description is of the disks as the saved Mac left them. */
    else if (t = mach_absolute_time(), hv_disk_sync(&disk) != 0 || hv_disk_sync(&aux) != 0)
        say_why(why, why_len, "The Mac's disk could not be saved to storage.");
    /* An auto-save's disks, as they are at this instant (the memory below is of the same one): APFS clones, so
       nothing is copied, and the live disks moving on later leave them as they are. */
    else if (autosave && (cloned = 1, unlink(disk_to), unlink(aux_to),
                          hv_disk_clone(&disk, disk_to, &cd.ino, &cd.size, &cd.mtime_ns) != 0 ||
                              hv_disk_clone(&aux, aux_to, &ca.ino, &ca.size, &ca.mtime_ns) != 0))
        say_why(why, why_len, "The Mac's disks couldn't be copied (is the iPad's storage full?).");
    else if (t_sync = mach_absolute_time() - t, t = mach_absolute_time(),
             put_host(w, autosave ? &cd : NULL, autosave ? &ca : NULL) != 0 || hv_snap_put_cpus(w, &m) != 0 ||
                 (!autosave && hv_code_key_put(w, &code_key) != 0) || hv_snap_put_ram(w, &m, SUSPEND_THREADS) != 0) {
        if (hv_snap_cancelled(w)) cancelled = 1;
        else say_why(why, why_len, autosave ? "The memory couldn't be copied (the log says why)."
                                            : "The saved state couldn't be written (is the iPad's storage full?).");
    } else if (t_ram = mach_absolute_time() - t, CANCELLED()) cancelled = 1;
    else {
        t = mach_absolute_time();
        int c = hv_snap_commit(w);
        w = NULL;
        if (c != 0 && CANCELLED()) cancelled = 1; /* it stops before its flushes and the header */
        else if (c != 0) say_why(why, why_len, "The saved state couldn't be written (is the iPad's storage full?).");
        else rc = 0;
        double t_commit = ms_since(t);
        /* A Suspend's translated code beside it, so the resumed Mac needn't translate it all again: never in the
           way (no room, or any failure: nothing kept, and the resume translates as before). Called off while it
           is written, the whole Suspend is. */
        t = mach_absolute_time();
        if (rc == 0 && !autosave && hv_code_store_save(&m, path, &code_key, &suspend_cancels, ticket) != 0 && CANCELLED())
            cancelled = 1;
        fprintf(stderr, "hengevm: %s took: devices %.0f ms, disks %.0f ms, memory %.0f ms, onto storage %.0f ms%s",
                autosave ? "auto-save" : "suspend", t_dev / 24000.0, t_sync / 24000.0, t_ram / 24000.0, t_commit,
                autosave ? "\n" : "");
        if (!autosave) fprintf(stderr, ", translated code %.0f ms\n", ms_since(t));
    }
    if (cancelled) {
        if (why && why_len) why[0] = 0;
        say_why(why, why_len, "Cancelled: the Mac goes on.");
        rc = -3;
    }
    if (w) hv_snap_abort(w);
    if (autosave) {
        /* The Mac goes on first (unless held for a pause or Suspend that came meanwhile: that one has it as it is,
           paused); what a failure leaves is deleted after, while it runs. */
        double stopped = ms_since(t_stop); /* from the CPUs stopping: the disks' sync in pause_l counts */
        int held = __atomic_load_n(&autosave_holds, __ATOMIC_ACQUIRE) != hold_ticket;
        if (!held) resume_l();
        pthread_mutex_unlock(&pause_lock);
        if (rc != 0) {
            hv_snap_discard(path);
            if (cloned) unlink(disk_to), unlink(aux_to);
        }
        struct stat st;
        double mb = rc == 0 && stat(path, &st) == 0 ? (double)st.st_blocks * 512 / 1048576.0 : 0;
        fprintf(stderr, "hengevm: auto-save %s; the Mac was stopped for %.0f ms (%.0f MB)%s\n",
                rc == 0 ? "kept" : rc == -3 ? "cancelled, nothing kept" : "failed, nothing kept", stopped, mb,
                held ? ", and stays paused (held)" : "");
        return rc;
    }
    if (rc == 0) {
        hv_vmapple_snap_drop_capture(&board); /* in the file now: not held in memory while Henge waits */
        free(suspended_path);
        suspended_path = strdup(path);
        if (!suspended_path) { /* without it, resuming here couldn't delete it: not worth the risk */
            hv_snap_discard(path);
            rc = -1;
            say_why(why, why_len, "Out of memory.");
        }
    } else {
        hv_snap_discard(path); /* any snapshot there from before is out of date: the Mac has run since */
    }
    if (rc == 0) {
        struct stat st;
        double mb = stat(path, &st) == 0 ? (double)st.st_blocks * 512 / 1048576.0 : 0;
        fprintf(stderr, "hengevm: suspended in %.0f ms (%.0f MB)\n", ms_since(t0), mb);
    } else if (rc == -3) {
        fprintf(stderr, "hengevm: suspend cancelled after %.0f ms, nothing kept; the Mac stays paused until it goes on\n", ms_since(t0));
    } else {
        fprintf(stderr, "hengevm: suspend failed after %.0f ms; the Mac stays paused\n", ms_since(t0));
    }
    pthread_mutex_unlock(&pause_lock);
    return rc;
#undef CANCELLED
}

int hv_vm_suspend_ticketed(const char *path, unsigned timeout_ms, uint64_t ticket, char *why, size_t why_len) {
    return save(path, timeout_ms, ticket, NULL, NULL, 0, NULL, why, why_len);
}

int hv_vm_autosave(const char *path, const char *disk_to, const char *aux_to, unsigned timeout_ms, uint64_t ticket,
                   uint64_t hold_ticket, char *why, size_t why_len) {
    if (why && why_len) why[0] = 0;
    if (!path || !disk_to || !aux_to) return say_why(why, why_len, "Nowhere to auto-save to."), -2;
    return save(path, timeout_ms, ticket, disk_to, aux_to, hold_ticket, NULL, why, why_len);
}

int hv_vm_capture_gpu(uint64_t limit, unsigned timeout_ms, char *why, size_t why_len) {
    if (why && why_len) why[0] = 0;
    pthread_mutex_lock(&pause_lock);
    if (!vm_paused) {
        pthread_mutex_unlock(&pause_lock);
        say_why(why, why_len, "The Mac isn't paused.");
        return -2;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t deadline = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec + (uint64_t)timeout_ms * 1000000ull;
    int rc = hv_vmapple_snap_capture(&board, limit, deadline, why, why_len);
    pthread_mutex_unlock(&pause_lock);
    return rc;
}

int hv_vm_snapshot_has_power(const char *path) {
    HvSnapReader *r = hv_snap_open(path, NULL, 0);
    if (!r) return -1;
    int has = hv_vmapple_snap_has_power(r);
    hv_snap_close(r);
    return has;
}

/* An auto-save's (disk_to set): the machine was built on its clones of the disks; after the load, before anything
   runs, those are put in the Mac's own places as files of their own (hv_disk_place: the disk that was there kept at
   disk_aside), the machine then on them. The auto-save itself is kept, untouched: should the Mac fail again, it is
   still there to go on from. A Suspend is consumed. */
static long code_loaded = -1; /* blocks the resume took from the code store (-1: none there or not for this Mac) */

static void load_code(HvSnapReader *r, const char *path) {
    HvCodeKey k;
    code_loaded = hv_code_key_get(r, &k) == 0 ? hv_code_store_load(&m, path, &k) : -1;
}

/* After a resume, how much was translated in its first minute: what the code store saves (compare a resume with it
   and one without, HV_CODE_STORE=0). Only reads counters. */
static void *resume_watch(void *unused) {
    (void)unused;
    static const unsigned at[] = {5, 15, 30, 60};
    uint64_t t0 = mach_absolute_time(), n0 = HV_STAT_TOTAL(&m, translations);
    unsigned slept = 0;
    for (size_t i = 0; i < sizeof at / sizeof *at; i++) {
        sleep(at[i] - slept);
        slept = at[i];
        uint64_t n = HV_STAT_TOTAL(&m, translations) - n0;
        char from[96] = "no code store";
        if (code_loaded >= 0) snprintf(from, sizeof from, "%ld blocks loaded from the code store", code_loaded);
        fprintf(stderr, "hengevm: %.0f s since the resume: %llu blocks translated (%s)\n", ms_since(t0) / 1000.0,
                (unsigned long long)n, from);
    }
    return NULL;
}

static int restore(const char *path, const char *disk_to, const char *disk_aside, const char *aux_to, char *why,
                   size_t why_len) {
    if (vm_started || vm_broken) return say_why(why, why_len, "This Mac has already run in this session.");
    if (vm_installer) return say_why(why, why_len, "An installer can't be restored.");
    struct stat st;
    if (stat(path, &st) != 0) return say_why(why, why_len, "There is no saved state.");
    uint64_t t0 = mach_absolute_time();
    char msg[256] = "";
    HvSnapReader *r = hv_snap_open(path, msg, sizeof msg);
    if (!r) {
        fprintf(stderr, "hengevm: restore: %s\n", msg);
        return say_why(why, why_len, "The saved state is damaged.");
    }
    if (check_host(r, why, why_len) != 0) {
        hv_snap_close(r);
        return -1;
    }
    if (hv_vmapple_snap_check(&board, r, why, why_len) != 0 || hv_snap_check_ram(r, &m, why, why_len) != 0) {
        fprintf(stderr, "hengevm: restore: refused: %s\n", why ? why : "");
        hv_snap_close(r);
        return -1;
    }
    if (hv_snap_cpus_rosetta(r) > 0 && !hv_rosetta_host_supported()) { /* hv_snap_get_cpus would fail halfway */
        hv_snap_close(r);
        return say_why(why, why_len, "This Mac was suspended in the experimental Rosetta mode, which this device can't run.");
    }
    /* From here the machine changes: a failure leaves it unusable. RAM first, into memory as the board made
       it: the firmware region holds the ROM, so it is cleared for the saved copy (main RAM is still all
       zeros, so its zero chunks need no writing and stay untouched, unbacked memory). */
    vm_broken = 1;
    for (int i = 0; i < m.nram; i++)
        if (m.ram[i].host != board.ram && !m.ram[i].readonly) memset(m.ram[i].host, 0, m.ram[i].size);
    int rc = -2;
    if (hv_snap_get_ram(r, &m, SUSPEND_THREADS, 0) != 0) say_why(why, why_len, "The saved memory couldn't be loaded.");
    else if (hv_snap_get_cpus(r, &m) != 0) say_why(why, why_len, "The saved processors couldn't be loaded.");
    else {
        /* A Suspend's translated code, if kept beside it (and for this Mac as it is now): a head start, never a
           failure. Here, with the RAM in place and the translator set as the Mac was (hv_snap_get_cpus sets its
           Rosetta and TSO modes), before the devices: whatever they write into RAM from now on finds the loaded
           code's pages marked as code, and drops what it changes. */
        if (!disk_to) load_code(r, path);
        if (hv_vmapple_snap_load(&board, r) != 0)
            say_why(why, why_len, "A device couldn't be restored. The log says which.");
        else if (disk_to && hv_disk_place(&disk, disk_to, disk_aside) != 0)
            say_why(why, why_len, "The saved disk couldn't be put in its place (is the iPad's storage full?).");
        else if (aux_to && hv_disk_place(&aux, aux_to, NULL) != 0)
            say_why(why, why_len, "The saved startup settings couldn't be put in their place.");
        /* A Suspend is consumed before anything runs: from the first instruction the Mac's disks move on from it. */
        else if (!disk_to && hv_snap_discard(path) != 0)
            say_why(why, why_len, "The saved state couldn't be cleared after loading.");
        else rc = 0;
    }
    hv_snap_close(r);
    if (rc != 0) {
        fprintf(stderr, "hengevm: restore failed after %.0f ms: this Mac can't run in this session\n", ms_since(t0));
        return rc;
    }
    hv_vmapple_restored(&board);
    vm_broken = 0;
    vm_restored = 1;
    fprintf(stderr, "hengevm: restored in %.0f ms\n", ms_since(t0));
    return 0;
}

int hv_vm_restore(const char *path, char *why, size_t why_len) {
    return restore(path, NULL, NULL, NULL, why, why_len);
}

int hv_vm_restore_autosave(const char *path, const char *disk_to, const char *disk_aside, const char *aux_to, char *why,
                           size_t why_len) {
    if (!disk_to || !aux_to) return say_why(why, why_len, "The auto-save's disks are not named.");
    return restore(path, disk_to, disk_aside, aux_to, why, why_len);
}

int hv_vm_use_disks(const char *disk_path, const char *aux_path) {
    if (vm_started || vm_broken || vm_restored) return -1;
    return hv_disk_reopen(&disk, disk_path) == 0 && hv_disk_reopen(&aux, aux_path) == 0 ? 0 : -1;
}

void hv_vm_host_network(int up) {
    uint64_t at = hv_virtio_net_now() & ~(1ull << 63);
    if (!at) at = 1;
    uint64_t want = (up ? 1ull << 63 : 0) | at, old = atomic_load(&host_network);
    do {
        if (old && (old >> 63) == (want >> 63)) return; /* no change: when it changed stands */
    } while (!atomic_compare_exchange_weak(&host_network, &old, want));
    HvVirtioNet *n = atomic_load(&host_network_nic);
    if (n) hv_virtio_net_set_carrier(n, up, at); /* the board's tick applies it, after the debounce */
}

void hv_vm_key(uint16_t code, int down) {
    hv_xhci_key_linux(board.usb, code, down);
    if (board.keyboard) {
        hv_virtio_input_send(board.keyboard, HV_EV_KEY, code, down);
        hv_virtio_input_send(board.keyboard, HV_EV_SYN, 0, 0);
    }
}

void hv_vm_key_hid(uint8_t usage, int down) {
    hv_xhci_key(board.usb, usage, down);
}

void hv_vm_modifiers_up(void) {
    hv_xhci_release_modifiers(board.usb);
}

/* ---- The speaker's volume (host/audio_usb.c) ---- */

static HvUsbAudio *speaker(void) { return board.usb ? hv_xhci_speaker(board.usb) : NULL; }

void hv_vm_set_speaker_listener(void (*cb)(void *opaque, double level, int mute), void *opaque) {
    hv_audio_usb_set_listener(cb, opaque);
}

void hv_vm_speaker_host_volume(double level) {
    HvUsbAudio *a = speaker();
    if (a) hv_usb_audio_host_volume(a, hv_usb_audio_scalar_to_db(level));
}

static _Atomic int pending_volume_key;
static int volume_key_held, volume_key_ticks;

static void volume_tick(void) {
    int key = atomic_exchange(&pending_volume_key, 0);
    if (key) {
        if (volume_key_held) {
            hv_xhci_key_linux(board.usb, (uint16_t)volume_key_held, 0);
            if (board.keyboard) {
                hv_virtio_input_send(board.keyboard, HV_EV_KEY, (uint16_t)volume_key_held, 0);
                hv_virtio_input_send(board.keyboard, HV_EV_SYN, 0, 0);
            }
        }
        volume_key_held = key > 0 ? 115 : key < 0 ? 114 : 113;
        volume_key_ticks = 2; /* hold for ~16-32 ms across ticks */
        hv_xhci_key_linux(board.usb, (uint16_t)volume_key_held, 1);
        if (board.keyboard) {
            hv_virtio_input_send(board.keyboard, HV_EV_KEY, (uint16_t)volume_key_held, 1);
            hv_virtio_input_send(board.keyboard, HV_EV_SYN, 0, 0);
        }
    } else if (volume_key_held) {
        if (--volume_key_ticks <= 0) {
            hv_xhci_key_linux(board.usb, (uint16_t)volume_key_held, 0);
            if (board.keyboard) {
                hv_virtio_input_send(board.keyboard, HV_EV_KEY, (uint16_t)volume_key_held, 0);
                hv_virtio_input_send(board.keyboard, HV_EV_SYN, 0, 0);
            }
            volume_key_held = 0;
        }
    }
}

void hv_vm_speaker_host_key(int key) {
    HvUsbAudio *a = speaker();
    if (a) hv_usb_audio_host_key(a, key > 0 ? HV_USB_AUDIO_KEY_VOLUME_UP : key < 0 ? HV_USB_AUDIO_KEY_VOLUME_DOWN : HV_USB_AUDIO_KEY_MUTE);
    atomic_store(&pending_volume_key, key);
    hv_share_volume_key(key);
}

int hv_vm_speaker_volume(double *level, int *mute) {
    HvUsbAudio *a = speaker();
    if (!a) return -1;
    int db, m;
    hv_usb_audio_get_volume(a, &db, &m);
    if (level) *level = hv_usb_audio_db_to_scalar(db);
    if (mute) *mute = m;
    return 0;
}

static _Atomic int pointer_hidden;
static _Atomic int park_wanted; /* below: where the pointer starts */

void hv_vm_hide_pointer(int hide) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    atomic_store(&pointer_hidden, hide != 0);
    hv_pvg_lite_hide_pointer(pvg, hide);
}

/* A mouse or trackpad that moves brings the pointer back. */
static void mouse_shows_pointer(void) {
    atomic_store_explicit(&park_wanted, 0, memory_order_relaxed); /* the mouse moves it from where it is */
    if (atomic_load_explicit(&pointer_hidden, memory_order_relaxed)) hv_vm_hide_pointer(0);
}

/* Every report says every axis, a 0 included. Scrolling with a mouse moved the pointer on and on in one
   direction: macOS's driver seems to keep an axis a report leaves out at what it last was. A 0 costs nothing
   either way. */
static void mouse_report(int dx, int dy, int wheel_y, int wheel_x) {
    hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_X, dx);
    hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_Y, dy);
    hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_WHEEL, wheel_y);
    hv_virtio_input_send(board.mouse, HV_EV_REL, HV_REL_HWHEEL, wheel_x);
    hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
}

void hv_vm_mouse_move(int dx, int dy) {
    mouse_shows_pointer();
    mouse_report(dx, dy, 0, 0);
}

/* 0 left, 1 right, 2 middle, in the codes macOS's driver turns into those buttons. */
static const uint16_t button_codes[3] = {HV_BTN_PRIMARY, HV_BTN_SECONDARY, HV_BTN_TERTIARY};

void hv_vm_mouse_button(int button, int down) {
    mouse_shows_pointer();
    if (button < 0 || button > 2) return;
    hv_virtio_input_send(board.mouse, HV_EV_KEY, button_codes[button], down);
    hv_virtio_input_send(board.mouse, HV_EV_SYN, 0, 0);
}

int hv_vm_touch_ready(void) {
    return board.tablet && board.tablet->asked_range && hv_virtio_input_ready(board.tablet);
}

/* Scale, then position: X and Y arrive in one report, so the pointer jumps straight there. */
static int touch_axis(double unit) {
    if (!(unit > 0)) return 0; /* also NaN */
    if (unit >= 1) return HV_TABLET_MAX;
    return (int)(unit * HV_TABLET_MAX + 0.5);
}

/* Where the pointer starts. A Mac puts it mid-screen; macOS here takes it from the touch device, which says
   0,0 (the top-left corner) until it is touched. So the host names a place, and it is sent from the moment the
   guest's driver can take it until macOS has shown its pointer, a little past that, or a real touch comes. */
static uint64_t now_ns(void);
static double park_x, park_y;
static uint64_t park_next_ns, park_shown_ns, park_until_ns;

void hv_vm_park_pointer(double x, double y) {
    park_x = x, park_y = y;
    park_next_ns = park_shown_ns = 0;
    park_until_ns = now_ns() + 180000000000ull; /* a start slower than 3 minutes keeps where it got to */
    atomic_store(&park_wanted, 1);
}

static void park_send(void) {
    hv_virtio_input_send(board.tablet, HV_EV_ABS, HV_ABS_X, touch_axis(park_x));
    hv_virtio_input_send(board.tablet, HV_EV_ABS, HV_ABS_Y, touch_axis(park_y));
    hv_virtio_input_send(board.tablet, HV_EV_SYN, 0, 0);
}

static void park_tick(void) {
    if (!atomic_load_explicit(&park_wanted, memory_order_relaxed)) return;
    uint64_t now = now_ns();
    if (now < park_next_ns) return;
    park_next_ns = now + 250000000ull;
    if (now > park_until_ns || (park_shown_ns && now > park_shown_ns + 3000000000ull)) {
        atomic_store(&park_wanted, 0);
        return;
    }
    if (!hv_vm_touch_ready()) return;
    if (!park_shown_ns && hv_vm_pointer_shown()) {
        park_shown_ns = now;
        HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
        const uint8_t *page = pvg ? hv_pvg_lite_display_page(pvg) : NULL;
        uint32_t where = page ? __atomic_load_n((const uint32_t *)(page + 0xe00), __ATOMIC_ACQUIRE) : 0xffffffff;
        if (where == 0xffffffff) fprintf(stderr, "hengevm: pointer: macOS has it, off the screen; kept at %.2f,%.2f\n", park_x, park_y);
        else fprintf(stderr, "hengevm: pointer: macOS has it at %u,%u; kept at %.2f,%.2f of the screen\n", where & 0xffff,
                     where >> 16, park_x, park_y);
        park_send();
        atomic_store(&park_wanted, 0);
        return;
    }
    park_send();
}

void hv_vm_touch(double x, double y) {
    atomic_store_explicit(&park_wanted, 0, memory_order_relaxed); /* a real touch: the pointer is the finger's now */
    hv_virtio_input_send(board.tablet, HV_EV_ABS, HV_ABS_X, touch_axis(x));
    hv_virtio_input_send(board.tablet, HV_EV_ABS, HV_ABS_Y, touch_axis(y));
    hv_virtio_input_send(board.tablet, HV_EV_SYN, 0, 0);
}

void hv_vm_touch_button(double x, double y, int button, int down) {
    if (button < 0 || button > 2) return;
    mouse_shows_pointer();
    hv_virtio_input_send(board.tablet, HV_EV_ABS, HV_ABS_X, touch_axis(x));
    hv_virtio_input_send(board.tablet, HV_EV_ABS, HV_ABS_Y, touch_axis(y));
    hv_virtio_input_send(board.tablet, HV_EV_KEY, button_codes[button], down);
    hv_virtio_input_send(board.tablet, HV_EV_SYN, 0, 0);
}

/* The guest takes a wheel event as 8 bits: -200 arrives as +56, the wrong way. So big moves go in pieces. */
static void wheel(uint16_t axis, int amount) { /* not the pointer back: a finger's swipe scrolls with this too */
    atomic_store_explicit(&park_wanted, 0, memory_order_relaxed);
    while (amount) {
        int piece = amount > 100 ? 100 : amount < -100 ? -100 : amount;
        mouse_report(0, 0, axis == HV_REL_WHEEL ? piece : 0, axis == HV_REL_HWHEEL ? piece : 0);
        amount -= piece;
    }
}

void hv_vm_mouse_wheel(int dy) {
    mouse_shows_pointer();
    wheel(HV_REL_WHEEL, dy);
}
void hv_vm_mouse_hwheel(int dx) {
    mouse_shows_pointer();
    wheel(HV_REL_HWHEEL, dx);
}

int hv_vm_pointer(int *x, int *y) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    const uint8_t *page = pvg ? hv_pvg_lite_display_page(pvg) : NULL;
    if (!page) return -1;
    uint32_t at = *(const volatile uint32_t *)(page + 0xe00);
    if (at == 0xffffffff) return -1;
    *x = (int)(at & 0xffff), *y = (int)(at >> 16);
    return 0;
}

int hv_vm_screen_size(int *w, int *h) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    uint32_t sw, sh;
    if (!pvg || hv_pvg_lite_screen_size(pvg, &sw, &sh) != 0) return -1;
    *w = (int)sw, *h = (int)sh;
    return 0;
}

int hv_vm_screen_points(int *w, int *h) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    uint32_t sw, sh;
    if (!pvg || hv_pvg_lite_screen_points(pvg, &sw, &sh) != 0) return -1;
    *w = (int)sw, *h = (int)sh;
    return 0;
}

int hv_vm_snapshot(uint8_t *bgra, int width, int height) {
    HvPvgLite *pvg = atomic_load_explicit(&display_pvg, memory_order_acquire);
    if (!pvg || width <= 0 || height <= 0) return -1;
    return hv_pvg_lite_snapshot(pvg, bgra, (uint32_t)width, (uint32_t)height);
}

/* ---- the touchscreen (macOS 27 and later) ----

   A USB HID touchscreen on the USB controller's port 6 (devices/xhci.c), plugged in once the helper's system part says
   macOS will take it as one: it turned WindowServer's external-touchscreen support on (a preference, kept from then
   on). Older macOS never sees it. Once in, it stays for this run of the Mac. If macOS configured it but the logged-in
   user's helper says the screen has no touchscreen for a while, it is pulled out and plugged in again, once: what
   WindowServer decides as a device arrives is then decided with the preference in place. */

static int ts_ignored_said; /* CPU 0's thread: "unplugging it" was logged (it takes a moment to go) */
static int ts_enabled = 1, ts_w_mm = 300, ts_h_mm = 200; /* the app's: allowed, and the size of the screen touched */
static int ts_always; /* the app's "Native (Always)": no check that macOS responds, and never back to the pointer by itself */

void hv_vm_touchscreen_always(int always) {
    int was = __atomic_exchange_n(&ts_always, always != 0, __ATOMIC_RELAXED);
    hv_share_touchscreen_always(always);
    /* Either way a verdict from before counts for nothing: Auto starts its check afresh. */
    if ((always != 0) != was) hv_share_touchscreen_retry();
}

/* What the helper's check says (hv_share_touchscreen_verdict), unless the person has Native (Always): then never "ignored". */
static int ts_verdict(void) {
    int v = hv_share_touchscreen_verdict();
    return v == 0 && __atomic_load_n(&ts_always, __ATOMIC_RELAXED) ? -1 : v;
}

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

void hv_vm_touchscreen_config(int enabled, int w_mm, int h_mm) {
    int was = __atomic_exchange_n(&ts_enabled, enabled != 0, __ATOMIC_RELAXED);
    if (enabled && !was) hv_share_touchscreen_retry(); /* turned off and on again: macOS is given another chance */
    if (w_mm > 0 && h_mm > 0) {
        __atomic_store_n(&ts_w_mm, w_mm, __ATOMIC_RELAXED);
        __atomic_store_n(&ts_h_mm, h_mm, __ATOMIC_RELAXED);
    }
}

/* The power button: PL061 pin 3, as QEMU's vmapple wires it (gpio-key). A press is held for 100 ms, as a finger
   would, on CPU 0's tick (under the device lock, like every device's state). Nothing about it is saved: a Mac saved
   in the middle of a press comes back with the pin high (the GPIO's levels are saved), so a pin found high with no
   press under way is let go first. */
#define POWER_PIN 3
static atomic_int power_pressed; /* one press waiting: two taps before the tick are one press */
static uint64_t power_release_ns;

void hv_vm_power_button(void) { atomic_store(&power_pressed, 1); }

static void power_tick(void) {
    if (!board.gpio) return;
    int held = (board.gpio->input >> POWER_PIN) & 1;
    if (power_release_ns) {
        if (now_ns() < power_release_ns) return;
        power_release_ns = 0;
        hv_pl061_set_input(board.gpio, POWER_PIN, 0);
        return; /* the next press, if any, after a tick with the button up */
    }
    if (held) { /* left pressed by a save taken mid-press */
        hv_pl061_set_input(board.gpio, POWER_PIN, 0);
        return;
    }
    if (!atomic_exchange(&power_pressed, 0)) return;
    hv_pl061_set_input(board.gpio, POWER_PIN, 1);
    power_release_ns = now_ns() + 100000000ull;
    fprintf(stderr, "hengevm: power button pressed\n");
}

static void touchscreen_tick(void) {
    if (!board.usb || vm_installer) return;
    uint64_t now = now_ns();
    if (now < ts_next_ns) return;
    ts_next_ns = now + 50000000ull; /* every 50 ms is plenty */
    /* The Mac was paused (or stopped) since the last look: it hadn't the time to take the touchscreen, nor to
       let go of it, so what was being waited for is waited for afresh. */
    if (ts_last_tick_ns && now - ts_last_tick_ns > 1000000000ull) {
        ts_unconfirmed_since_ns = 0;
        if (ts_replug_at_ns) ts_replug_at_ns = now + 1000000000ull;
    }
    ts_last_tick_ns = now;
    int forced = __atomic_load_n(&ts_forced, __ATOMIC_RELAXED);
    /* Not when macOS was seen to do nothing with its touches: out, so the Mac is as it was without one (the pointer,
       and touch imitated with it), until the Mac restarts and it's tried again. */
    int ignored = ts_verdict() == 0;
    if (!ignored) ts_ignored_said = 0;
    int want = forced >= 0 ? forced : __atomic_load_n(&ts_enabled, __ATOMIC_RELAXED) && hv_share_touchscreen_ready() == 1 && !ignored;
    int st = hv_xhci_touch_state(board.usb);
    /* The helper tagged it with the screen's ID after WindowServer may have looked: out, and in again tagged. */
    if (hv_share_touchscreen_take_replug()) {
        hv_share_touchscreen_retry(); /* a verdict on it untagged counts for nothing */
        if (!ts_unplugged_to_replug && st != HV_XHCI_TOUCH_ABSENT) {
            ts_unplugged_to_replug = 1;
            ts_replug_at_ns = 0;
            hv_xhci_touch_plug(board.usb, 0, 0, 0);
            return;
        }
    }
    if (ts_unplugged_to_replug) { /* pulled out a moment ago: in again, a second after it's gone (macOS lets go of it) */
        if (st != HV_XHCI_TOUCH_ABSENT) return;
        if (!ts_replug_at_ns) ts_replug_at_ns = now + 1000000000ull;
        if (now < ts_replug_at_ns) return;
        ts_unplugged_to_replug = 0;
        ts_replug_at_ns = 0;
        if (want) hv_xhci_touch_plug(board.usb, 1, __atomic_load_n(&ts_w_mm, __ATOMIC_RELAXED), __atomic_load_n(&ts_h_mm, __ATOMIC_RELAXED));
        ts_unconfirmed_since_ns = 0;
        return;
    }
    if (st == HV_XHCI_TOUCH_ABSENT) {
        if (want) {
            hv_xhci_touch_plug(board.usb, 1, __atomic_load_n(&ts_w_mm, __ATOMIC_RELAXED), __atomic_load_n(&ts_h_mm, __ATOMIC_RELAXED));
            ts_unconfirmed_since_ns = 0;
        }
        return;
    }
    if (forced == 0 || (forced < 0 && (!__atomic_load_n(&ts_enabled, __ATOMIC_RELAXED) || ignored))) { /* the app turned it off, or macOS ignores it */
        if (ignored && forced < 0 && __atomic_load_n(&ts_enabled, __ATOMIC_RELAXED) && st != HV_XHCI_TOUCH_ABSENT &&
            !ts_ignored_said)
            ts_ignored_said = 1, fprintf(stderr, "hengevm: macOS ignores the touchscreen's touches: unplugging it\n");
        hv_xhci_touch_plug(board.usb, 0, 0, 0);
        return;
    }
    /* In and configured, someone at their desktop with a helper that answers, and it says no touchscreen. */
    int unconfirmed = st == HV_XHCI_TOUCH_READY && hv_share_session() == HV_SESSION_USER && hv_share_touchscreen() == 0;
    if (!unconfirmed) {
        ts_unconfirmed_since_ns = 0;
        return;
    }
    if (!ts_unconfirmed_since_ns) ts_unconfirmed_since_ns = now;
    if (!ts_replugged && now - ts_unconfirmed_since_ns > 8000000000ull) { /* the helper checks every second */
        fprintf(stderr, "hengevm: macOS hasn't taken the touchscreen: plugging it in again\n");
        ts_replugged = 1;
        ts_unplugged_to_replug = 1;
        hv_xhci_touch_plug(board.usb, 0, 0, 0);
    }
}

int hv_vm_touchscreen_state(void) { return board.usb ? hv_xhci_touch_state(board.usb) : HV_XHCI_TOUCH_ABSENT; }

int hv_vm_touchscreen_usable(void) {
    if (hv_vm_touchscreen_state() != HV_XHCI_TOUCH_READY) return 0;
    /* Locked, asleep or the screen saver, in Native (Always) too: macOS does nothing with touches then, and only the
       pointer wakes the display and reaches the password field. With touches, the Mac stayed locked and Henge looked
       frozen (8 Nov 2026). */
    if (hv_share_screen_away() == 1) return 0;
    /* Native (Always): plugged in and set up is enough, whatever the helper says (the login window included). */
    if (__atomic_load_n(&ts_always, __ATOMIC_RELAXED)) return 1;
    return hv_share_touchscreen() == 1 && ts_verdict() != 0;
}

/* Asked in Native (Always) too, though its answer changes nothing there: the helper says what macOS made of the touch. */
void hv_vm_touchscreen_watch(void) { hv_share_touchscreen_watch(); }

int hv_vm_touchscreen_status(void) {
    if (!__atomic_load_n(&ts_enabled, __ATOMIC_RELAXED)) return HV_VM_TS_OFF;
    int verdict = ts_verdict();
    if (verdict == 0) return HV_VM_TS_IGNORED;
    if (hv_vm_touchscreen_usable()) return verdict == 1 ? HV_VM_TS_WORKS : HV_VM_TS_TRYING;
    if (hv_share_touchscreen_ready() == 1 && hv_vm_touchscreen_state() != HV_XHCI_TOUCH_UNAVAILABLE) return HV_VM_TS_WAITING;
    return HV_VM_TS_NONE;
}

static uint16_t ts_axis(double unit) {
    if (!(unit > 0)) return 0; /* also NaN */
    if (unit >= 1) return 32767;
    return (uint16_t)(unit * 32767 + 0.5);
}

int hv_vm_touchscreen_frame(int n, const HvVmContact *c) {
    if (!board.usb || n < 0) return -1;
    HvXhciContact k[HV_XHCI_CONTACTS];
    if (n > HV_XHCI_CONTACTS) n = HV_XHCI_CONTACTS;
    int down = 0, cancelled = 0;
    for (int i = 0; i < n; i++) {
        k[i] = (HvXhciContact){.id = (uint8_t)c[i].id, .down = c[i].down != 0, .cancel = c[i].cancel != 0, .x = ts_axis(c[i].x), .y = ts_axis(c[i].y)};
        down |= k[i].down, cancelled |= !k[i].down && k[i].cancel;
    }
    hv_share_touchscreen_gesture(!down, cancelled);
    return hv_xhci_touch_frame(board.usb, n, k);
}
