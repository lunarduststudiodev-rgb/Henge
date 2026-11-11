/* A CPU's reset and its machine's per-page tables use only the memory they need.

   hv_cpu_reset_index clears a 4.8 MB HvCpu. Most of it (the return tables, the ASID tables, the fill logs) is used a
   little at a time as the CPU runs, but a memset made all of it resident at once, on every CPU, at every boot and
   every PSCI CPU_ON. The reset now clears in place only the pages the CPU has in memory and gives the rest fresh
   mappings (hv_zero_kept): untouched pages stay so, and the pages a CPU used stay, zeroed, for the run that follows
   at once (a resume, a CPU_ON), instead of being swapped for fresh ones that fault straight back in.

   The TLB (2 MB) was written whole by every reset (its empty entries are 0x80 bytes, not 0): on a CPU that never runs
   too, and again on one that ran, which had only a few entries to drop. It is now made all invalid when the CPU
   first runs (hv_tlb_ready), and a reset writes only the entries the fill logs name: nothing of a fresh CPU's TLB is
   resident after its reset, nor after other CPUs' TLBIs or a write revoke reach it, and a CPU that ran keeps its
   TLB, ready, so its run after a reset faults nothing back in.

   The write hints (HvRam.whint) were one word per page for 8 CPUs whatever the Mac has: a 6-CPU Mac wasted a
   quarter, 6 MB of 6 GB of RAM. They are now as many as the machine's CPUs (hv_machine_set_cpus).

   Each CPU's front cache (jc, 1 MB) and branch-cache filters (ijc_keyed, ijc_hint, ijc_pages: 272 KB) were written
   whole by the machine's set-up (a memset of all 26.8 MB of it), by every CPU's reset, and by every flush (jc and the
   hints), on CPUs the guest never started too. They are now written only as far as fills and lookups used them, and
   the machine's set-up leaves untouched memory untouched. */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dispatch.h"
#include "machine.h"
#include "mmu.h"
#include "sysreg.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t footprint(void) {
    task_vm_info_data_t vi;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n);
    return vi.phys_footprint;
}

/* Resident whole pages in [p, p + len) */
static size_t resident(const void *p, size_t len) {
    uintptr_t page = (uintptr_t)getpagesize(), lo = ((uintptr_t)p + page - 1) & ~(page - 1), hi = ((uintptr_t)p + len) & ~(page - 1);
    if (hi <= lo) return 0;
    char vec[4096];
    size_t n = (hi - lo) / page, got = 0;
    if (n > sizeof vec || mincore((void *)lo, hi - lo, vec) != 0) return 0;
    for (size_t i = 0; i < n; i++) got += vec[i] & MINCORE_INCORE ? 1 : 0;
    return got;
}

static HvMachine m, m6, mr;
static HvCpu fresh, used, other, ran;

/* A CPU's front cache and branch-cache filters: pages resident, and bytes not zero */
static size_t cache_pages(const HvPerCpu *p) {
    return resident(p->jc, sizeof p->jc) + resident(p->ijc_keyed, sizeof p->ijc_keyed) + resident(p->ijc_hint, sizeof p->ijc_hint) +
           resident(p->ijc_pages, sizeof p->ijc_pages);
}

/* Bytes not zero, reading only pages in memory (a page never touched reads zero, and reading it would bring it in) */
static size_t nonzero(const void *p, size_t len) {
    uintptr_t page = (uintptr_t)getpagesize(), s = (uintptr_t)p, e = s + len;
    size_t n = 0;
    for (uintptr_t a = s; a < e;) {
        uintptr_t pe = ((a & ~(page - 1)) + page) < e ? (a & ~(page - 1)) + page : e;
        char v = 0;
        if (mincore((void *)(a & ~(page - 1)), page, &v) != 0 || (v & MINCORE_INCORE))
            for (uintptr_t i = a; i < pe; i++) n += *(const uint8_t *)i != 0;
        a = pe;
    }
    return n;
}

static size_t cache_bytes(const HvPerCpu *p) {
    return nonzero(p->jc, sizeof p->jc) + nonzero(p->ijc_keyed, sizeof p->ijc_keyed) + nonzero(p->ijc_hint, sizeof p->ijc_hint) +
           nonzero(p->ijc_pages, sizeof p->ijc_pages);
}

static double ms_since(uint64_t t0) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
}

static long minor_faults(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_minflt;
}

/* Another CPU's TLBI reading whether a CPU being reset has a ready TLB, as fast as it can: how often it read not ready */
static HvCpu rd;
static int rd_stop;
static uint64_t rd_reads, rd_not, rd_other, rd_mpidr;
static void *ready_reader(void *arg) {
    (void)arg;
    while (!__atomic_load_n(&rd_stop, __ATOMIC_ACQUIRE)) {
        /* relaxed atomics: main reads them while this runs */
        if (!hv_tlb_is_ready(&rd)) __atomic_fetch_add(&rd_not, 1, __ATOMIC_RELAXED);
        if (__atomic_load_n(&rd.index, __ATOMIC_RELAXED) != 3) __atomic_fetch_add(&rd_other, 1, __ATOMIC_RELAXED);
        if (__atomic_load_n(&rd.mpidr, __ATOMIC_RELAXED) != 0x80000003u)
            __atomic_fetch_add(&rd_mpidr, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&rd_reads, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

/* Resets cpu as CPU index of mm in a child (mm set up again first, with no RAM, if again) and gives its wait status
   (-1: no child), with what it wrote to stderr in said: a refusal must be the one meant, not another abort on the
   way. */
static int reset_in_child(HvCpu *cpu, HvMachine *mm, uint32_t index, int again, char *said, size_t size) {
    int fd[2];
    said[0] = 0;
    if (pipe(fd) != 0) return -1;
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        close(fd[0]);
        dup2(fd[1], 2);
        if (again && (hv_machine_init(mm, 64u << 20) != 0 || mm->nram != 0)) _exit(2);
        hv_cpu_reset_index(cpu, mm, index);
        _exit(0);
    }
    close(fd[1]);
    size_t n = 0;
    for (ssize_t r; p > 0 && n < size - 1 && (r = read(fd[0], said + n, size - 1 - n)) > 0;) n += (size_t)r;
    said[n] = 0;
    close(fd[0]);
    int st = -1;
    if (p > 0) waitpid(p, &st, 0);
    return st;
}

int main(void) {
    uint64_t init0 = footprint();
    if (hv_machine_init(&m, 64u << 20) != 0) { printf("FAIL: machine\n"); return 1; }
    {   /* The set-up writes the fields it sets, not the whole machine: a CPU never started holds none of its caches */
        uint64_t made_init = footprint() - init0;
        size_t mach_kb = resident(&m, sizeof m) * getpagesize() >> 10, last = cache_pages(&m.percpu[HV_MAX_CPUS - 1]);
        printf("machine set-up: %llu KB resident; %zu of the machine's %zu KB; %zu pages of the last CPU's caches\n",
               (unsigned long long)(made_init >> 10), mach_kb, sizeof m >> 10, last);
        CHECK(last == 0, "the set-up made %zu pages of a CPU's caches resident", last);
        CHECK(mach_kb <= 1024, "the set-up made %zu KB of the machine resident", mach_kb);
    }

    /* A fresh CPU's reset makes resident only the few pages it writes fields in: not its TLB */
    uint64_t before = footprint();
    hv_cpu_reset_index(&fresh, &m, 0);
    uint64_t made = footprint() - before;
    printf("reset of a fresh CPU (%zu KB): %llu KB resident, %zu of its TLB's %zu KB\n", sizeof fresh >> 10,
           (unsigned long long)(made >> 10), resident(fresh.tlb, sizeof fresh.tlb) * getpagesize() >> 10, sizeof fresh.tlb >> 10);
    CHECK(made <= 256u << 10, "a reset made %llu KB resident", (unsigned long long)(made >> 10));
    CHECK(resident(fresh.tlb, sizeof fresh.tlb) == 0, "a reset made %zu pages of a fresh CPU's TLB resident",
          resident(fresh.tlb, sizeof fresh.tlb));
    CHECK(!hv_tlb_is_ready(&fresh), "a fresh CPU's TLB is ready before it ran");
    {   /* ...nor its caches: a reset writes what the CPU filled, and it filled none (a CPU_ON of one never run, a boot) */
        uint64_t t0 = mach_absolute_time();
        for (int k = 0; k < 20; k++) hv_cpu_reset_index(&fresh, &m, 0);
        double avg = ms_since(t0) / 20;
        size_t cp = cache_pages(&m.percpu[0]);
        printf("reset of a CPU that never ran: %.3f ms; %zu pages of its caches resident\n", avg, cp);
        CHECK(cp == 0, "a reset made %zu pages of the caches of a CPU that never ran resident", cp);
    }

    /* A CPU that ran, reset again, is as a fresh one: every byte the reset clears is clear */
    hv_cpu_reset_index(&used, &m, 1);
    memset(used.ret_tab, 0x5a, sizeof used.ret_tab);
    memset(used.asid_cid, 0x5a, sizeof used.asid_cid);
    memset(used.asid_base, 0x5a, sizeof used.asid_base);
    memset(used.user_fills, 0x5a, sizeof used.user_fills);
    memset(used.x, 0x5a, sizeof used.x);
    memset(used.tlb, 0x11, sizeof used.tlb);
    used.sctlr_el1 = 0x5a5a;
    hv_cpu_reset_index(&used, &m, 1);
    hv_cpu_reset_index(&other, &m, 1); /* the same index: the same pc_state and m */
    CHECK(memcmp(&used, &other, sizeof used) == 0, "a used CPU's reset differs from a fresh one's");
    /* ...and what it had used stays, zeroed, while what it never used stays untouched: a CPU that ran (a resume,
       CPU_OFF then CPU_ON) runs again at once, so swapping its used pages for fresh ones only faults them back in
       (giving memory back is a trim's job). Before, the reset gave the return and ASID tables back. */
    hv_cpu_reset_index(&used, &m, 1);
    hv_cpu_reset_index(&other, &m, 1);
    memset(used.ret_tab, 0x5a, sizeof used.ret_tab);
    memset(used.asid_base, 0x5a, sizeof used.asid_base);
    size_t held_before = resident(used.ret_tab, sizeof used.ret_tab) + resident(used.asid_base, sizeof used.asid_base);
    size_t unused_before = resident(used.asid_cid, sizeof used.asid_cid) + resident(used.user_fills, sizeof used.user_fills);
    hv_cpu_reset_index(&used, &m, 1);
    size_t held = resident(used.ret_tab, sizeof used.ret_tab) + resident(used.asid_base, sizeof used.asid_base);
    size_t unused = resident(used.asid_cid, sizeof used.asid_cid) + resident(used.user_fills, sizeof used.user_fills);
    printf("a CPU that ran: %zu pages of its return and ASID tables resident, %zu after its reset; %zu -> %zu of those it "
           "never used\n", held_before, held, unused_before, unused);
    CHECK(held_before > 0 && held == held_before, "a reset gave back %zu of the %zu pages the CPU had used", held_before - held,
          held_before);
    CHECK(unused <= unused_before, "a reset brought in %zu pages the CPU never used", unused - unused_before);
    hv_cpu_reset_index(&other, &m, 1);
    CHECK(memcmp(&used, &other, sizeof used) == 0, "kept in place, a used CPU's reset still reads as a fresh one's");

    /* Write hints: as many per page as the machine has CPUs */
    uint64_t size = 64u << 20, pages = size >> HV_RAM_PAGE_BITS;
    void *ram = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(hv_machine_add_ram(&m, 0x70000000, size, ram, 0) == 0, "add ram");
    CHECK(m.ram[0].whint_cpus == HV_MAX_CPUS, "machine with no CPU count: %u hints a page", m.ram[0].whint_cpus);
    if (hv_machine_init(&m6, 64u << 20) != 0) { printf("FAIL: machine\n"); return 1; }
    hv_machine_set_cpus(&m6, 6);
    CHECK(hv_machine_add_ram(&m6, 0x70000000, size, ram, 0) == 0, "add ram");
    CHECK(m6.ram[0].whint_cpus == 6, "6 CPUs: %u hints a page", m6.ram[0].whint_cpus);
    size_t got = malloc_size(m6.ram[0].whint);
    CHECK(got >= pages * 6 * sizeof(uint64_t) && got < pages * 7 * sizeof(uint64_t), "6 CPUs: %zu bytes of hints for %llu pages",
          got, (unsigned long long)pages);

    /* Nothing another CPU does writes or scans a TLB not ready: its TLBIs (done from there, or posted), a whole flush
       (a code cache's layout change), a write revoke (the scan of every CPU's TLB for a code page) */
    hv_tlbi_drop_remote_va(&fresh, 0xfffffe0000004000ull, -1);
    hv_tlbi_drop_remote_va(&fresh, 0x4000, 1);
    hv_tlbi_drop_remote_range(&fresh, 0x4000, 64, 1);
    hv_cpu_post_tlbi(&fresh, 0, 8, 3, 1, 0xfffffe0000004000ull >> 12); /* TLBI VAE1IS */
    hv_cpu_post_tlbi(&fresh, 0, 8, 3, 0, 0);                           /* TLBI VMALLE1IS */
    hv_cpu_process_requests(&fresh);
    fresh.nkernel_fills = HV_KERNEL_FILLS + 1;
    hv_tlb_flush(&fresh);
    __atomic_store_n(&m.ram[0].code_pages[0], 1, __ATOMIC_SEQ_CST);
    uint32_t page0 = 0;
    hv_ram_revoke_writes(&m, &m.ram[0], &page0, 1);
    __atomic_store_n(&m.ram[0].code_pages[0], 0, __ATOMIC_SEQ_CST);
    CHECK(resident(fresh.tlb, sizeof fresh.tlb) == 0, "other CPUs' TLBIs, a flush and a revoke made %zu pages of a TLB "
          "not ready resident", resident(fresh.tlb, sizeof fresh.tlb));
    CHECK(!fresh.nkernel_fills && !fresh.nuser_fills, "a flush of a TLB not ready left its logs");

    /* A flush of every block (a full flush, a trim) empties every CPU's return tables, as a run-out does a CPU's own:
       one that never ran (or is off) doesn't get its 2 MB made resident for it, and one that ran keeps the pages it
       used, zeroed in place, for its calls after */
    hv_cpu_reset_index(&other, &m, 1);
    memset(other.ret_tab[3], 0x5a, sizeof other.ret_tab[3]);
    size_t ret_used = resident(other.ret_tab, sizeof other.ret_tab);
    long rf0 = minor_faults();
    hv_flush_blocks(&m);
    long ret_faults = minor_faults() - rf0;
    size_t ret_fresh = resident(fresh.ret_tab, sizeof fresh.ret_tab), ret_kept = resident(other.ret_tab, sizeof other.ret_tab);
    printf("a flush of every block: %zu return table pages of a CPU that never ran resident; %zu of %zu kept by one that "
           "ran; %ld page faults\n", ret_fresh, ret_kept, ret_used, ret_faults);
    CHECK(ret_fresh == 0, "a flush made %zu return table pages of a CPU that never ran resident", ret_fresh);
    CHECK(ret_kept == ret_used, "a flush gave back %zu of the return table pages a CPU used", ret_used - ret_kept);
    size_t ret_nz = 0;
    for (size_t i = 0; i < sizeof other.ret_tab; i++) ret_nz += ((uint8_t *)other.ret_tab)[i] != 0;
    CHECK(ret_nz == 0, "a flush left %zu bytes of a return table", ret_nz);
    CHECK(ret_faults <= 64, "a flush took %ld page faults", ret_faults);
    {   /* ...nor their front caches and hints: CPU 0 never ran, CPU 1 ran no lookup */
        size_t cp = cache_pages(&m.percpu[0]) + cache_pages(&m.percpu[1]);
        CHECK(cp == 0, "a flush made %zu pages of the caches of CPUs that never ran resident", cp);
        uint64_t t0 = mach_absolute_time();
        for (int k = 0; k < 20; k++) hv_flush_blocks(&m);
        printf("a flush of every block, 2 CPUs that never ran: %.3f ms; %zu pages of their caches resident\n",
               ms_since(t0) / 20, cp);
        /* A zone's eviction scanned all of every CPU's front cache, bringing it into memory */
        hv_jit_make_room(&m, 1);
        cp = cache_pages(&m.percpu[0]) + cache_pages(&m.percpu[1]);
        CHECK(cp == 0, "a zone's eviction made %zu pages of the caches of CPUs that never ran resident", cp);
    }

    /* A CPU that runs has its TLB made ready (all invalid) first; one that ran keeps it through a reset (a CPU_ON, a
       resume), only its logged entries written, so its next run faults nothing back in */
    {
        enum { PG = 16384 };
        uint32_t *code = aligned_alloc(PG, PG);
        memset(code, 0, PG);
        code[0] = 0xd503201f, code[1] = 0xd503201f, code[2] = 0xd503201f; /* nop */
        CHECK(hv_machine_init(&mr, 64u << 20) == 0, "machine");
        hv_machine_add_ram(&mr, (uint64_t)(uintptr_t)code, PG, code, 0);
        mr.stop_pc = (uint64_t)(uintptr_t)&code[2];
        hv_cpu_reset_index(&ran, &mr, 0);
        ran.pc = (uint64_t)(uintptr_t)code;
        int r = hv_run(&ran);
        CHECK(r == HV_EXIT_HALT && ran.pc == mr.stop_pc, "the run ended with %d at %llx", r, (unsigned long long)ran.pc);
        CHECK(hv_tlb_is_ready(&ran), "a CPU ran with its TLB not ready");
        /* The stats' sample of the block hash (1 bucket in 64, every 512 bytes) reads only the 64ths inserts wrote:
           all 64 samples, as 64 stats lines would, find the blocks without bringing the rest of the 16 MB in */
        {
            uint64_t found = 0, parts = (uint64_t)__builtin_popcountll(mr.hash_used), odd = 0;
            for (uint32_t part = 0; part < 64; part++) {
                HvHashSample hs;
                hv_block_hash_sample(&mr, part, &hs);
                found += hs.used;
                odd += !hs.used != !hs.worst; /* a worst chain only where a bucket held a block: none read otherwise */
            }
            CHECK(!odd, "%llu samples named a worst chain they didn't find, or none for one they did",
                  (unsigned long long)odd);
            size_t hp = resident(mr.hash, sizeof mr.hash), most = parts * (sizeof mr.hash / 64) / getpagesize();
            printf("the stats' hash samples: %llu buckets with blocks found, %zu of the hash's %zu pages in memory\n",
                   (unsigned long long)found, hp, sizeof mr.hash / getpagesize());
            CHECK(found > 0 && parts > 0 && hp <= most, "the hash samples found %llu buckets, with %zu hash pages in "
                  "memory (%zu at most)", (unsigned long long)found, hp, most);
        }
        size_t pg = (size_t)getpagesize(), bad = 0;
        size_t tlb_pages = ((((uintptr_t)ran.tlb + sizeof ran.tlb) & ~(pg - 1)) - (((uintptr_t)ran.tlb + pg - 1) & ~(pg - 1))) / pg;
        CHECK(resident(ran.tlb, sizeof ran.tlb) == tlb_pages, "a CPU ran with %zu of its %zu TLB pages",
              resident(ran.tlb, sizeof ran.tlb), tlb_pages);
        /* Two entries filled and logged (as a fill does), one not logged: the reset must write those two only */
        HvTlbEntry probe = {.read_tag = 0x1230, .write_tag = 0x1230, .exec_tag = 0x1230, .addend = 7};
        ran.tlb[100] = probe, ran.tlb[20000] = probe, ran.tlb[40000] = probe;
        ran.user_fills[ran.nuser_fills++] = 100;
        ran.kernel_fills[ran.nkernel_fills++] = 20000;
        long f0 = minor_faults();
        hv_cpu_reset_index(&ran, &mr, 0);
        ran.pc = (uint64_t)(uintptr_t)code;
        r = hv_run(&ran);
        long faults = minor_faults() - f0;
        CHECK(r == HV_EXIT_HALT, "the second run ended with %d", r);
        CHECK(hv_tlb_is_ready(&ran), "a reset left a CPU that ran with its TLB not ready (its next run writes all 2 MB)");
        CHECK(resident(ran.tlb, sizeof ran.tlb) == tlb_pages, "a reset gave back %zu of a TLB the CPU runs with at once",
              tlb_pages - resident(ran.tlb, sizeof ran.tlb));
        CHECK(ran.tlb[100].read_tag == HV_TLB_INVALID && ran.tlb[20000].exec_tag == HV_TLB_INVALID,
              "a reset left a logged entry valid");
        CHECK(!memcmp(&ran.tlb[40000], &probe, sizeof probe), "a reset wrote an entry the logs don't name (the whole TLB)");
        for (size_t i = 0; i < sizeof ran.tlb / sizeof ran.tlb[0]; i++)
            if (i != 40000 && (ran.tlb[i].read_tag != HV_TLB_INVALID || ran.tlb[i].exec_tag != HV_TLB_INVALID ||
                               ran.tlb[i].write_tag != HV_TLB_INVALID)) bad++;
        printf("a CPU that ran: reset and run again with %ld page faults; %zu entries valid after\n", faults, bad);
        CHECK(faults <= 32, "a reset and the run after it took %ld page faults", faults);
        /* What a CPU that ran filled is gone after its reset, a flush and an eviction alike, in place: its front cache
           (lookups) and filters (fills, both halves) */
        {
            HvPerCpu *pc = ran.pc_state;
            static HvBlock fb[3];
            fb[0].va = 0x123454000ull, fb[0].flags = 0, fb[0].code = (void *)(uintptr_t)0x1000;
            fb[1].va = 0x223458000ull, fb[1].flags = 0, fb[1].code = (void *)(uintptr_t)0x2000;
            fb[2].va = 0xfffffe0000123400ull, fb[2].flags = HV_BLOCK_EL1, fb[2].code = (void *)(uintptr_t)0x3000;
            for (int round = 0; round < 3; round++) {
                for (int k = 0; k < 2; k++) { /* the second finds its blocks: a lookup that translates doesn't cache */
                    ran.pc = (uint64_t)(uintptr_t)code;
                    r = hv_run(&ran);
                }
                size_t jc_ran = nonzero(pc->jc, sizeof pc->jc);
                for (int k = 0; k < 3; k++) hv_ijc_fill(&ran, &fb[k]);
                size_t held = cache_pages(pc), filled = cache_bytes(pc);
                const char *what = round == 0 ? "reset" : round == 1 ? "flush" : "eviction";
                int zone = mr.zone; /* the blocks' */
                if (round == 2) hv_jit_make_room(&mr, zone ? 0 : 1); /* another zone to fill next: theirs can go */
                uint64_t t0 = mach_absolute_time();
                if (round == 0) hv_cpu_reset_index(&ran, &mr, 0);
                else if (round == 1) hv_flush_blocks(&mr);
                else hv_jit_make_room(&mr, zone);
                double ms = ms_since(t0);
                        /* A flush and an eviction keep ijc_keyed: the new ids and generation make its bits meaningless */
                size_t left = round == 0 ? cache_bytes(pc) : cache_bytes(pc) - nonzero(pc->ijc_keyed, sizeof pc->ijc_keyed);
                size_t kept = cache_pages(pc);
                printf("a CPU that ran: %s %.3f ms; %zu bytes of its caches set (%zu of jc), %zu after; %zu pages held, %zu after\n",
                       what, ms, filled, jc_ran, left, held, kept);
                CHECK(jc_ran > 0 && filled > jc_ran, "the %s round filled nothing (%zu, %zu)", what, jc_ran, filled);
                CHECK(left == 0, "a %s left %zu bytes of a CPU's caches", what, left);
                CHECK(kept == held, "a %s changed the pages a CPU's caches hold: %zu -> %zu", what, held, kept);
            }
        }
        memset(&ran.tlb[40000], 0x80, sizeof probe);
        hv_machine_free(&mr);
        free(code);
    }

    /* A running CPU's user branch-cache id follows its ASID even with no user table filled: TCR.A1 going 1 -> 0
       (ASIDs from TTBR1, then from TTBR0) flushes without a TTBR0 write, and its reset must still give the current
       ASID its id. Otherwise later fills carry the no-ASID id, and a TLBI by ASID looks up another. */
    {
        static HvCpu sw;
        hv_cpu_reset_index(&sw, &m, 2);
        hv_tlb_ready(&sw); /* it runs */
        sw.x[1] = 1ull << 22; hv_sysreg_msr(&sw, 0xd5182041u, 0);              /* msr tcr_el1: A1 */
        sw.x[1] = 5ull << 48 | 0x80000000ull; hv_sysreg_msr(&sw, 0xd5182001u, 0); /* msr ttbr0_el1: ASID 5 */
        sw.x[1] = 0; hv_sysreg_msr(&sw, 0xd5182041u, 0);                        /* msr tcr_el1: A1 off */
        printf("after TCR.A1 1 -> 0: user salt id %llu, ASID 5's id %u\n", (unsigned long long)(sw.ijc_salt[0] >> 48),
               (unsigned)sw.asid_cid[5]);
        CHECK(sw.asid_cid[5] && sw.ijc_salt[0] == (uint64_t)sw.asid_cid[5] << 48,
              "the user salt (id %llu) isn't ASID 5's id (%u)", (unsigned long long)(sw.ijc_salt[0] >> 48),
              (unsigned)sw.asid_cid[5]);
    }

    /* A ready CPU's reset (a resume, a CPU_ON after a short off) keeps it ready, and nothing else writes tlb_ready
       meanwhile: another CPU's TLBI reads it at any time. The reset zeroed it with the rest and stored it back after,
       a race in which such a TLBI read the TLB as not ready. */
    {
        hv_cpu_reset_index(&rd, &m, 3);
        rd.mpidr = 0x80000003u; /* the board's, as vmapple sets it */
        hv_tlb_ready(&rd);
        pthread_t th;
        pthread_create(&th, NULL, ready_reader, NULL);
        while (!__atomic_load_n(&rd_reads, __ATOMIC_RELAXED)) sched_yield();
        int n = 20000;
        for (int k = 0; k < n; k++) hv_cpu_reset_index(&rd, &m, 3);
        __atomic_store_n(&rd_stop, 1, __ATOMIC_RELEASE);
        pthread_join(th, NULL);
        uint64_t not = __atomic_load_n(&rd_not, __ATOMIC_RELAXED), reads = __atomic_load_n(&rd_reads, __ATOMIC_RELAXED);
        printf("%d resets of a ready CPU: %llu of %llu reads by another thread found it not ready\n", n,
               (unsigned long long)not, (unsigned long long)reads);
        CHECK(hv_tlb_is_ready(&rd), "a reset of a ready CPU left it not ready");
        CHECK(not == 0, "another CPU read a ready CPU being reset as not ready %llu times", (unsigned long long)not);
        /* Its index too, which code-page protection reads to find its write hints: zeroed and stored back, the same
           race had it take CPU 0's */
        uint64_t other = __atomic_load_n(&rd_other, __ATOMIC_RELAXED);
        printf("  and %llu found an index other than 3\n", (unsigned long long)other);
        CHECK(other == 0, "another CPU read the index of CPU 3 being reset as another %llu times",
              (unsigned long long)other);
        /* And its mpidr, by which another CPU's PSCI call finds it (CPU_ON, AFFINITY_INFO) */
        uint64_t mp = __atomic_load_n(&rd_mpidr, __ATOMIC_RELAXED);
        printf("  and %llu found an mpidr other than the board's\n", (unsigned long long)mp);
        CHECK(mp == 0, "another CPU read the mpidr of CPU 3 being reset as another %llu times", (unsigned long long)mp);
        /* and on its own, should the reader not have run during the resets */
        CHECK(rd.mpidr == 0x80000003u && rd.index == 3, "a reset left CPU 3 with mpidr %llx, index %u",
              (unsigned long long)rd.mpidr, rd.index);
    }

    /* A CPU reset into another machine, or one never reset (m NULL) whose memory says ready, keeps nothing: an entry
       no fill log names (filled under another TLB layout, or left by whatever held the memory) would live on */
    {
        static HvCpu stale;
        static const uint8_t zero[sizeof rd.tlb[0]];
        memset(&rd.tlb[7], 0x11, sizeof rd.tlb[7]); /* rd: ready in m, as CPU 3 */
        hv_cpu_reset_index(&rd, &m6, 3);
        CHECK(!hv_tlb_is_ready(&rd) && !rd.mpidr && !memcmp(&rd.tlb[7], zero, sizeof zero),
              "CPU 3 of one machine reset into another kept: ready %d, mpidr %llx, an unlogged entry %d",
              hv_tlb_is_ready(&rd), (unsigned long long)rd.mpidr, !!memcmp(&rd.tlb[7], zero, sizeof zero));
        stale.tlb_ready = &stale, stale.mpidr = 0x80000004u;
        memset(&stale.tlb[7], 0x11, sizeof stale.tlb[7]);
        hv_cpu_reset_index(&stale, &m6, 4);
        CHECK(!hv_tlb_is_ready(&stale) && !stale.mpidr && !memcmp(&stale.tlb[7], zero, sizeof zero),
              "a CPU never reset kept: ready %d, mpidr %llx, an unlogged entry %d", hv_tlb_is_ready(&stale),
              (unsigned long long)stale.mpidr, !!memcmp(&stale.tlb[7], zero, sizeof zero));
        /* or one of a machine set up again since (a new TLB layout, maybe): that machine's CPU no more */
        hv_tlb_ready(&rd);
        memset(&rd.tlb[7], 0x11, sizeof rd.tlb[7]);
        CHECK(hv_machine_init(&m6, 64u << 20) == 0, "m6 again");
        hv_machine_set_cpus(&m6, 6);
        CHECK(hv_machine_add_ram(&m6, 0x70000000, size, ram, 0) == 0, "add ram");
        hv_cpu_reset_index(&rd, &m6, 3);
        CHECK(!hv_tlb_is_ready(&rd) && !memcmp(&rd.tlb[7], zero, sizeof zero),
              "a CPU of a machine set up again kept: ready %d, an unlogged entry %d", hv_tlb_is_ready(&rd),
              !!memcmp(&rd.tlb[7], zero, sizeof zero));
    }

    /* A CPU past the count would write past the hints: refused, by that check (its message) */
    static HvCpu seventh, past;
    char said[4096];
    int st = reset_in_child(&seventh, &m6, 6, 0, said, sizeof said);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT && strstr(said, "of a machine made for"),
          "CPU 6 of 6 wasn't refused by the hint check (status %d, said \"%s\")", st, said);

    /* Nor one past what a machine holds, with no RAM added (no hints to check it against): it would write past
       m->cpus and m->percpu */
    st = reset_in_child(&past, &m6, HV_MAX_CPUS, 1, said, sizeof said);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT && strstr(said, "past the"),
          "CPU %d of a machine with no RAM wasn't refused by the bound (status %d, said \"%s\")", HV_MAX_CPUS, st,
          said);

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
