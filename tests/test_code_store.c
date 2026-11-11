// The code store (core/codestore.c): one machine's translated code saved to a file and loaded into another, whose
// RAM, code region and slot table are at other host addresses, as a resumed Mac's are. The loaded code runs with the
// same results and nothing translated again (except blocks that hold this run's host addresses), exits to another
// page chain with their TLB keys remade, and a store that doesn't fit the machine, or is damaged, is passed over.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "codestore.h"
#include "dispatch.h"
#include "machine.h"
#include "evict_lists.h"
#include "rosetta.h"

#define PAGE (1u << HV_RAM_PAGE_BITS) /* a RAM page: its code has its own generation */
#define BASE 0x80000000ull
#define HOT BASE                    /* page 0: a loop */
#define S2 (BASE + 2 * PAGE)        /* page 2: b to T2 on page 3, which loops back x1 times */
#define T2 (BASE + 3 * PAGE)
#define CNT (BASE + 4 * PAGE)       /* page 4: reads the counter (its code holds a host address: not saved) */
#define STOP (BASE + 5 * PAGE)      /* never translated: the dispatcher stops there */
#define COLD (BASE + 6 * PAGE)      /* pages of one-instruction blocks, to fill more than one zone */
#define NCHUNKS 12
#define NPAGES (6 + NCHUNKS)
#define INSNS (PAGE / 4)
#define JIT_BYTES (8u << 20)

static int failures;
static char path[512];

static void check(int ok, const char *what) {
    printf("%s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

static uint32_t branch(uint64_t from, uint64_t to) {
    return 0x14000000u | ((uint32_t)((int64_t)(to - from) / 4) & 0x03ffffff);
}

static uint64_t run(HvMachine *m, HvCpu *cpu, uint64_t pc, uint64_t x1) {
    hv_cpu_reset(cpu, m);
    cpu->el = 0;
    cpu->spsel = 0;
    cpu->pc = pc;
    cpu->x[0] = 0;
    cpu->x[1] = x1;
    cpu->cpacr_el1 = 3u << 20;
    cpu->el0_cnt = 3;
    m->stop_pc = STOP;
    int reason = hv_run(cpu);
    if (reason != HV_EXIT_HALT || cpu->pc != STOP) {
        printf("run from %llx stopped with %d at %llx\n", (unsigned long long)pc, reason, (unsigned long long)cpu->pc);
        failures++;
    }
    return cpu->x[0];
}

static uint32_t *guest_ram(void) {
    uint32_t *ram = aligned_alloc(PAGE, (size_t)NPAGES * PAGE);
    if (!ram) exit(1);
    uint32_t *at;
    for (size_t i = 0; i < (size_t)NPAGES * INSNS; i++) ram[i] = 0xd503201f; /* nop */
#define AT(va) (ram + ((va) - BASE) / 4)
    at = AT(HOT);
    at[0] = 0x91000400; /* add x0, x0, #1 */
    at[1] = 0xf1000421; /* subs x1, x1, #1 */
    at[2] = 0x54000001 | ((uint32_t)(-2 & 0x7ffff) << 5); /* b.ne HOT */
    at[3] = branch(HOT + 12, STOP);
    AT(S2)[0] = branch(S2, T2);
    at = AT(T2);
    at[0] = 0x91001400; /* add x0, x0, #5 */
    at[1] = 0xf1000421; /* subs x1, x1, #1 */
    at[2] = 0x54000001 | (uint32_t)((((int64_t)S2 - (int64_t)(T2 + 8)) / 4) & 0x7ffff) << 5; /* b.ne S2 */
    at[3] = branch(T2 + 12, STOP);
    at = AT(CNT);
    at[0] = 0xd53be042; /* mrs x2, cntvct_el0 */
    at[1] = 0x91001c00; /* add x0, x0, #7 */
    at[2] = branch(CNT + 8, STOP);
    for (int c = 0; c < NCHUNKS; c++) {
        uint64_t va = COLD + (uint64_t)c * PAGE;
        for (uint32_t i = 0; i + 1 < INSNS; i++) AT(va)[i] = branch(va + 4 * i, va + 4 * i + 4);
        AT(va)[INSNS - 1] = branch(va + 4 * (INSNS - 1), STOP);
    }
#undef AT
    return ram;
}

/* A machine as a restored Mac has one: its own RAM (a copy of the guest's), code region and tables. */
static void machine(HvMachine *m, uint32_t **ram_out) {
    memset(m, 0, sizeof *m);
    if (hv_machine_init(m, JIT_BYTES) != 0) exit(1);
    /* Zones emptied only when the cache is full, in the stop (HV_EVICT_SYNC), so which zones hold code is as the test
       fills them: the reclaimer would empty one ahead once the last is 3/4 full (test_evict_reclaim saves with
       one being emptied). */
    m->reclaim_off = 1;
    uint32_t *ram = guest_ram();
    if (hv_machine_add_ram(m, BASE, (uint64_t)NPAGES * PAGE, ram, 0) != 0) exit(1);
    hv_machine_set_zones(m, 4);
    m->stats_interval = 0;
    m->gen_checks = 1; /* any jump left into code that isn't live is reported */
    m->stop_pc = STOP; /* part of what the translation depends on: the same for every machine here */
    if (ram_out) *ram_out = ram;
}

static int file_there(void) {
    char p[600];
    snprintf(p, sizeof p, "%s.code", path);
    struct stat st;
    return stat(p, &st) == 0;
}

static HvBlock *block(HvMachine *m, uint64_t va) {
    HvRam *r = &m->ram[0];
    return hv_block_find(m, hv_block_hash(va, va, 0), va, va, 0, r->page_gen[(va - r->pa) >> HV_RAM_PAGE_BITS]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *tmp = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/hv_code_store_test_%d.hvsnap", tmp && *tmp ? tmp : "/tmp", (int)getpid());
    hv_code_store_remove(path);
    static HvMachine a, b, c, d, e, f, g; /* each its own: a CPU thread's code chunk is per machine address */
    static HvCpu ca, cb, cc;

    /* 1. The first machine runs everything: the cache fills one zone and goes on into the next. */
    machine(&a, NULL);
    for (int i = 0; i < NCHUNKS; i++) run(&a, &ca, COLD + (uint64_t)i * PAGE, 0);
    check(run(&a, &ca, HOT, 10) == 10, "the loop runs");
    check(run(&a, &ca, S2, 4) == 20, "the exit to another page runs");
    check(run(&a, &ca, CNT, 0) == 7, "the counter read runs");
    HvBlock *cnt = block(&a, CNT);
    int cnt_nosave = cnt && cnt->nosave;
    printf("zones %d, filling %d, %u switches; the counter's block %s\n", a.nzones, a.zone, (unsigned)a.zone_switches,
           cnt_nosave ? "holds a host address" : "doesn't hold a host address (this host's counter goes through the helper)");
    check(a.nzones == 4 && a.zone >= 1 && a.zones[0].nblocks > 0 && block(&a, COLD) != NULL,
          "more than one zone holds code, none emptied");

    /* 2. Saved; called off, nothing is kept. */
    HvCodeKey k, other;
    hv_code_key_new(&k);
    hv_code_key_new(&other);
    uint64_t cancel = 1;
    check(hv_code_store_save(&a, path, &k, &cancel, 2) != 0 && !file_there(), "a save called off keeps nothing");
    setenv("HV_CODE_STORE_VERIFY", "1", 1); /* its pages' checksums too, for 6 */
    check(hv_code_store_save(&a, path, &k, NULL, 0) == 0 && file_there(), "saved");
    unsetenv("HV_CODE_STORE_VERIFY");

    /* 3. Loaded into a machine at other addresses: the same results, nothing translated again. */
    uint32_t *rb;
    machine(&b, &rb);
    check(b.jit.base != a.jit.base && b.slots != a.slots && b.ram[0].host != a.ram[0].host, "the second machine is elsewhere");
    long n = hv_code_store_load(&b, path, &k);
    printf("loaded %ld blocks\n", n);
    check(n > NCHUNKS * (long)(INSNS / 2), "most blocks loaded");
    check(b.zone == a.zone && b.zones[0].nblocks == a.zones[0].nblocks, "the zones are as they were");
    uint64_t t = HV_STAT_TOTAL(&b, translations);
    check(run(&b, &cb, HOT, 10) == 10, "the loaded loop runs");
    check(run(&b, &cb, S2, 5) == 25, "the loaded exit to another page runs");
    for (int i = 0; i < NCHUNKS; i++) run(&b, &cb, COLD + (uint64_t)i * PAGE, 0);
    check(HV_STAT_TOTAL(&b, translations) == t, "nothing was translated again");
    check(b.nxchained > 0, "the exit to another page chained (its key made for the new RAM address)");
    check(check_lists(&b, "loaded store") == 0, "the hash and the page lists hold together (loaded store)");
    t = HV_STAT_TOTAL(&b, translations);
    check(run(&b, &cb, CNT, 0) == 7, "the counter read runs");
    check(HV_STAT_TOTAL(&b, translations) - t == (uint64_t)cnt_nosave, "the block holding a host address was translated again");
    check(run(&b, &cb, HOT, 3) == 3 && run(&b, &cb, S2, 2) == 10, "and everything runs on");

    /* A loaded block as the translator left it: its entry the do-nothing word, its gate and size as saved. */
    HvBlock *ha = block(&a, HOT), *hb = block(&b, HOT);
    check(ha && hb && hb->gate == ha->gate && hb->words == ha->words && hb->gate && !hb->dead &&
              hb->code[0] == HV_BLOCK_NOP,
          "a loaded block keeps its gate (in its cold part) and size, its entry still the no-op");
    int ok = 1;
    for (int z = 0; z < b.nzones; z++) ok &= b.zones[z].ndead <= b.zones[z].nblocks && b.zones[z].dead_words <= b.zones[z].used;
    check(ok, "each zone's dead count, as loaded, within what it holds");

    /* 4. Code changed: the loaded blocks of its page go, and the new code runs. */
    rb[1] = 0xf1000821; /* subs x1, x1, #2 */
    uint32_t nd = hb ? b.zones[hb->zone].ndead : 0;
    hv_invalidate_code(&b, HOT);
    check(hb && hb->dead && hb->code[0] == (0x14000000u | hb->gate) && b.zones[hb->zone].ndead > nd,
          "a write retires the loaded block: its entry jumps to its saved gate, and its zone counts it (and its page's others) dead");
    check(run(&b, &cb, HOT, 10) == 5, "a loaded block whose page is written is dropped");
    check(check_lists(&b, "loaded block retired") == 0, "the hash and the page lists hold together (loaded block retired)");

    /* 5. A store for another Suspend, or another translator setting, isn't loaded. */
    machine(&c, NULL);
    check(hv_code_store_load(&c, path, &other) == -1 && hv_blocks_held(&c) == 0, "another Suspend's key: not loaded");
    c.chain = !c.chain;
    check(hv_code_store_load(&c, path, &k) == -1 && hv_blocks_held(&c) == 0, "another translator setting: not loaded");
    c.chain = !c.chain;
    check(run(&c, &cc, HOT, 10) == 10, "that machine translates as before");
    if (hv_rosetta_host_supported()) { /* its code was made for mode 0 */
        check(hv_machine_set_rosetta(&c, 1) == -2 && c.rosetta == 0 && cc.rosetta_mode == 0,
              "with code translated, the Rosetta mode can't change");
        check(hv_machine_set_rosetta(&c, 0) == 0 && c.rosetta == 0, "but can be set to the one it is in");
    }
    check(hv_code_store_load(&c, path, &k) == -1, "nor into a cache already in use");
    hv_machine_free(&c);

    /* 6. Checked against the guest's pages (HV_CODE_STORE_VERIFY=1): a changed page's blocks are dropped. */
    uint32_t *rc;
    machine(&d, &rc);
    rc[0] = 0x91000800; /* add x0, x0, #2 */
    setenv("HV_CODE_STORE_VERIFY", "1", 1);
    n = hv_code_store_load(&d, path, &k);
    unsetenv("HV_CODE_STORE_VERIFY");
    check(n > 0 && !block(&d, HOT) && block(&d, S2), "verified: only the changed page's blocks dropped");
    check(run(&d, &cc, HOT, 10) == 20, "and the changed code runs");
    check(check_lists(&d, "verified load") == 0, "the hash and the page lists hold together (verified load)");
    hv_machine_free(&d);

    /* 7. Damaged: the zone it is in is dropped, the rest loads, and everything runs right. */
    char p[600];
    snprintf(p, sizeof p, "%s.code", path);
    FILE *fp = fopen(p, "r+b");
    uint32_t word = 0;
    if (fp && fseek(fp, 16384 + 4, SEEK_SET) == 0 && fread(&word, 4, 1, fp) == 1) {
        word ^= 0x00100000;
        fseek(fp, 16384 + 4, SEEK_SET);
        fwrite(&word, 4, 1, fp);
    }
    if (fp) fclose(fp);
    machine(&e, NULL);
    n = hv_code_store_load(&e, path, &k);
    check(n > 0 && e.zones[0].nblocks == 0 && e.zones[a.zone].nblocks > 0, "a damaged zone is dropped, the others load");
    check(e.zones[0].filled == 0 && e.zones[0].score == 0 && e.zones[1].filled == a.zones[1].filled && a.zones[1].filled,
          "the dropped zone is as one never filled (eviction takes it first), the loaded ones as saved");
    int cleared = e.slot_site[0] == NULL && e.slot_stub[0] == NULL && e.slot_xa[0] == 0;
    for (uint32_t i = 0; i < a.zones[0].nblocks; i++) cleared &= e.blocks[i].jit_gen == 0 && e.blocks[i].code == NULL;
    check(cleared, "and what it had loaded of that zone is cleared again");
    check(check_lists(&e, "damaged zone dropped") == 0, "the hash and the page lists hold together (damaged zone dropped)");
    t = HV_STAT_TOTAL(&e, translations);
    check(run(&e, &cc, HOT, 10) == 10 && run(&e, &cc, S2, 3) == 15, "and everything runs right");
    for (int i = 0; i < NCHUNKS; i++) run(&e, &cc, COLD + (uint64_t)i * PAGE, 0);
    check(HV_STAT_TOTAL(&e, translations) > t, "the dropped zone's code was translated again");
    hv_jit_zones_sync(&e);
    int kept = e.zone == 0;
    for (int z = 1; z < e.nzones; z++) kept &= e.zones[z].nblocks == a.zones[z].nblocks || (z == a.zone && e.zones[z].nblocks >= a.zones[z].nblocks);
    check(kept, "the cache, full again, went on into the dropped zone: no loaded zone emptied");
    check(check_lists(&e, "after refilling") == 0, "the hash and the page lists hold together (after refilling)");
    hv_machine_free(&e);

    /* 7b. An exit record damaged where only the CRC finds it (a pad word), so the whole zone was fixed up first: all
       of it is cleared again. The file's layout as codestore.c writes it: the code first, then the block records
       (56 bytes each), then the exits (24 each, their pad at 12), each part at a 16 KB boundary. */
    {
        uint64_t al = 16384, code_off = 16384;
        uint64_t blocks_off = code_off + ((a.zones[0].used * 4 + al - 1) / al) * al;
        uint64_t slots_off = blocks_off + (((uint64_t)a.zones[0].nblocks * 56 + al - 1) / al) * al;
        fp = fopen(p, "r+b");
        uint32_t w2 = 0;
        if (fp && fseek(fp, (long)(code_off + 4), SEEK_SET) == 0 && fread(&w2, 4, 1, fp) == 1) { /* 7's damage undone */
            w2 ^= 0x00100000;
            fseek(fp, (long)(code_off + 4), SEEK_SET);
            fwrite(&w2, 4, 1, fp);
        }
        if (fp && fseek(fp, (long)(slots_off + 12), SEEK_SET) == 0 && fread(&w2, 4, 1, fp) == 1) {
            w2 ^= 1;
            fseek(fp, (long)(slots_off + 12), SEEK_SET);
            fwrite(&w2, 4, 1, fp);
        }
        if (fp) fclose(fp);
        machine(&g, NULL);
        n = hv_code_store_load(&g, path, &k);
        int sites = 0, cleared = 1;
        for (uint32_t s = 0; s < a.zones[0].nslots; s++) {
            sites += a.slot_site[s] != NULL;
            cleared &= g.slot_site[s] == NULL && g.slot_stub[s] == NULL && g.slot_cond[s] == NULL && g.slot_xa[s] == 0;
        }
        for (uint32_t i = 0; i < a.zones[0].nblocks; i++) cleared &= g.blocks[i].jit_gen == 0;
        check(n > 0 && g.zones[0].nblocks == 0 && sites > 0 && cleared, "damaged exits: the zone's exits cleared again");
        check(run(&g, &cc, HOT, 10) == 10 && run(&g, &cc, S2, 3) == 15, "and everything runs right");
        hv_machine_free(&g);
    }

    /* 8. Off: HV_CODE_STORE=0 neither loads nor saves. */
    machine(&f, NULL);
    setenv("HV_CODE_STORE", "0", 1);
    check(hv_code_store_load(&f, path, &k) == -1, "HV_CODE_STORE=0: not loaded");
    unsetenv("HV_CODE_STORE");
    hv_machine_free(&f);

    hv_code_store_remove(path);
    check(!file_there(), "removed");
    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
