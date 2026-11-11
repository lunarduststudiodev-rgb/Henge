/* Malformed guest graphics input fed to pvg_lite/pvg_metal on the Mac. Metal stops the whole app on a description
   it can't make, so each case runs in a child process of its own (this program again: Metal's shader compiler is
   not there in a child only forked). A case passes when its child lives and finishes within its time (a hang is
   killed by an alarm and fails), any exec it sends still finishes (the session goes on), and what it was to refuse
   was told as refused. The last cases send every command tag an encoder takes, with bodies made up from sizes and
   values a guest could send: run them with MTL_DEBUG_LAYER=1 too, where Metal stops on what it would otherwise leave
   undefined. HV_FUZZ_ITERS bodies each (default 24), from HV_FUZZ_SEED; HV_FUZZ_SHOW prints each exec's records.
   Needs Metal: on a Mac with none it says so and passes.
   Nothing here runs on the GPU: every case sets HV_PVG_ENCODE_ONLY=1, so the device does all of its work up to and
   including encoding (where Metal's debug layer checks) and never commits it. Made-up work on the real GPU once
   asked for 59 billion vertices, hung the GPU, and the Mac with it. The switch is only in the tests' build of the
   device (pvg_metal_test.o, made with HV_PVG_TEST_HOOKS, which this program links): the app's and the iPad's builds
   fail closed on it, running none of the guest's work (test_pvg_present_app). Older builds without the switch would
   run on the GPU: don't run the made-up cases (33-36) against them. Cases 47-54 are the second review's: indirect draws clamped on
   the GPU (as encoded: the clamp's jobs are read back, not run), an exec's work budget, tile sizes by sample count,
   MSAA colour per sample by GPU family, depth store 5 and copies within one texture. Cases 55 on are the third
   review's. */
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "pvg_command.h"
#include "pvg_lite.h"
#include "vmapple.h"

/* Missing from older builds (to run these cases against them): the cases that need it say so */
#pragma weak hv_pvg_metal_test_objects
#pragma weak hv_pvg_metal_test_gpu_failed
#pragma weak hv_pvg_metal_test_indirect
#pragma weak hv_pvg_lite_gpu_faulted

#define RAM_SIZE (64ull << 20)

typedef struct {
    HvMachine m;
    HvCpu cpu;
    HvVmapple b;
} Board;

static Board X;

static int build(Board *x) {
    if (hv_machine_init(&x->m, 4 << 20) != 0) return -1;
    HvCfgParams cfg = {.nr_cpus = 1, .ecid = 0x1234, .mac_en0 = {2, 0, 0, 0x12, 0x34, 0x56}};
    static const uint8_t mac[6] = {2, 0, 0, 0x12, 0x34, 0x56};
    if (hv_vmapple_create(&x->b, &x->m, &x->cpu, RAM_SIZE, NULL, NULL, &cfg, mac) != 0) return -1;
    hv_vmapple_reset_cpu(&x->b);
    return 0;
}

static void wr32(Board *x, uint64_t pa, uint32_t v) { hv_phys_write(&x->m, pa, &v, 4); }
static uint32_t rd32(Board *x, uint64_t pa) {
    uint32_t v = 0;
    hv_phys_read(&x->m, pa, &v, 4);
    return v;
}
static void mmio(Board *x, uint64_t pa, uint64_t off, uint64_t v, unsigned size) {
    HvMmio *d = hv_mmio_lookup(&x->m, pa);
    hv_dev_lock(&x->m);
    d->write(d->opaque, off, v, size);
    hv_dev_unlock(&x->m);
}
static void gfx(Board *x, uint64_t off, uint32_t v) { mmio(x, VMAPPLE_GFX, off, v, 4); }

/* The guest's side, in its memory: 16 KB pages */
#define PG(n) (VMAPPLE_MEM + 0x4000ull * (n))
#define STAMPS PG(0)  /* a stamp per channel */
#define ROOT PG(1)    /* the channels' records, from +0x400 */
#define TABLE PG(2)   /* channel 1's ring pages */
#define RING PG(3)    /* channel 1's ring: one page */
#define TROOT PG(4)   /* task 1's root: its page table and depth */
#define TL0 PG(5)     /* task 1's page table */
#define TMEM PG(6)    /* task 1's offset 0: its object entries and records */
#define HEAP_PAGE PG(8)
#define BUF_PAGE PG(9) /* task VA 0xC000: the command buffers, and buffer ref 6 */
#define TASK 1u
#define OBJS 0x100u   /* task offset of its object list */
#define CH 1u
#define HEAP_REF 5u
#define BUF_REF 6u

/* A command into channel 1's ring, and its doorbell */
static void command(Board *x, uint32_t op, uint32_t stamp, const uint32_t *args, uint32_t n) {
    uint32_t wp = rd32(x, ROOT + 0x400), hdr[3] = {op, 12 + 4 * n, stamp};
    hv_phys_write(&x->m, RING + wp % 0x4000, hdr, 12);
    if (n) hv_phys_write(&x->m, RING + (wp + 12) % 0x4000, args, 4 * n);
    wr32(x, ROOT + 0x400, wp + hdr[1]);
    gfx(x, 0x1020, CH);
}

static int wait_stamp(Board *x, uint32_t stamp) {
    for (int i = 0; i < 20000; i++) {
        if (rd32(x, STAMPS + 4 * CH) == stamp) return 1;
        usleep(1000);
    }
    return 0;
}
static void must_finish(Board *x, uint32_t stamp) {
    if (!wait_stamp(x, stamp)) {
        printf("    exec %u never finished\n", stamp);
        exit(3);
    }
}

static void object_entry(Board *x, uint32_t ref, uint32_t type, uint32_t at, const void *rec, uint32_t size) {
    hv_phys_write(&x->m, TMEM + at, rec, size);
    uint32_t e[3] = {type | size << 8, at, 0};
    hv_phys_write(&x->m, TMEM + OBJS + ref * 12ull, e, 12);
}

/* v1 texture description: {type, usage, format, width, height, depth, mips, samples, array, options} */
static void v1(uint8_t *d, uint8_t type, uint8_t usage, uint16_t format, uint32_t w, uint32_t h, uint16_t mips, uint16_t samples, uint16_t array) {
    uint32_t depth = 1;
    uint16_t options = 0x20; /* private */
    memset(d, 0, 24);
    d[0] = type, d[1] = usage;
    memcpy(d + 2, &format, 2), memcpy(d + 4, &w, 4), memcpy(d + 8, &h, 4);
    memcpy(d + 12, &depth, 4), memcpy(d + 16, &mips, 2), memcpy(d + 18, &samples, 2), memcpy(d + 20, &array, 2), memcpy(d + 22, &options, 2);
}

/* A texture with no guest memory (type 8, tag 0x14): shader read, shader write and render target */
static void private_texture(Board *x, uint32_t ref, uint8_t type, uint16_t format, uint32_t w, uint32_t h, uint16_t mips, uint16_t samples,
                            uint16_t array) {
    uint8_t rec[36] = {0};
    uint32_t hdr[3] = {0x14, sizeof rec, ref};
    memcpy(rec, hdr, 12);
    v1(rec + 12, type, 1 | 2 | 4, format, w, h, mips, samples, array);
    object_entry(x, ref, 8, 0x1000 + ref * 0x40, rec, sizeof rec);
}

static void buffer_object(Board *x, uint32_t ref, uint32_t at) {
    struct __attribute__((packed)) { uint64_t length; uint32_t vpage, flags; } b = {0x4000, 3, 0};
    object_entry(x, ref, 1, at, &b, sizeof b);
}

/* Task 1 with its root and object list, buffer ref 6 over task page 3 (BUF_PAGE), a 16 KB heap ref 5 over page 2 */
static void start_task(Board *x) {
    wr32(x, ROOT + 0x400 + 16, (uint32_t)(TABLE >> 14));
    wr32(x, TABLE, (uint32_t)(RING >> 14));
    wr32(x, TROOT, (uint32_t)(TL0 >> 14)), wr32(x, TROOT + 4, 1);
    wr32(x, TL0, (uint32_t)(TMEM >> 14));
    wr32(x, TL0 + 8, (uint32_t)(HEAP_PAGE >> 14)), wr32(x, TL0 + 12, (uint32_t)(BUF_PAGE >> 14));
    gfx(x, 0x1030, (uint32_t)(STAMPS >> 14));
    gfx(x, 0x101c, (uint32_t)(ROOT >> 14));
    buffer_object(x, BUF_REF, 0x3400);
    struct __attribute__((packed)) { uint32_t vpage; uint64_t length; uint8_t flags; } h = {2, 0x4000, 0};
    object_entry(x, HEAP_REF, 13, 0x3420, &h, sizeof h);
    uint32_t root[4] = {TASK << 1, 0, 0, (uint32_t)(TROOT >> 14)};
    command(x, 0x38, 1, root, 4);
    uint32_t list[7] = {TASK, OBJS, 0, 0x3000, 0, 0x100, 0x100};
    command(x, 0x44, 2, list, 7);
    must_finish(x, 2);
}

/* An exec that names one object (so it is made), with these flags */
static void use_ref(Board *x, uint32_t ref, uint32_t stamp) {
    uint32_t exec[3 + 2] = {TASK, 1, 0, ref, 1};
    command(x, 0x43, stamp, exec, 5);
    must_finish(x, stamp);
}

/* An exec running one segment of an encoder type (0 render, 1 compute, 2 blit, 4 info) from task VA 0xC000
   (BUF_PAGE): {length, type}, then the records. It uses texture ref 10. */
static void segment_exec(Board *x, uint32_t stamp, uint32_t type, const uint8_t *recs, uint32_t n) {
    if (n + 8 > 0x4000) { printf("    records of %u bytes don't fit\n", n); exit(92); }
    uint32_t hdr[2] = {n, type};
    hv_phys_write(&x->m, BUF_PAGE, hdr, 8);
    hv_phys_write(&x->m, BUF_PAGE + 8, recs, n);
    uint32_t exec[3 + 2 + 4] = {TASK, 1, 1, 10, 0, 0xC000, 0, 8 + n, 0};
    command(x, 0x43, stamp, exec, 9);
    must_finish(x, stamp);
}
static void render_exec(Board *x, uint32_t stamp, const uint8_t *recs, uint32_t n) { segment_exec(x, stamp, 0, recs, n); }
static uint32_t put_rec(uint8_t *at, uint32_t tag, const void *body, uint32_t n) {
    uint32_t sz = 8 + n;
    memcpy(at, &tag, 4), memcpy(at + 4, &sz, 4), memcpy(at + 8, body, n);
    return sz;
}
/* A pass on texture ref 10 (64x64 BGRA, render target): load, store and store options as given */
static uint32_t pass_rec(uint8_t *at, uint16_t load, uint16_t store, uint16_t options) {
    uint8_t b[0x88] = {0};
    uint32_t ref = 10;
    memcpy(b + 0x4c, &ref, 4);
    memcpy(b + 0x4c + 20, &load, 2), memcpy(b + 0x4c + 22, &store, 2), memcpy(b + 0x4c + 24, &options, 2);
    return put_rec(at, 26, b, sizeof b);
}
static double seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* A texture entry (type 2) over the 16 KB page at task VA 0xC000, of this type, with this mip table; the entry at
   task VA at_va (from 0x10000, on pages PG(30...)) */
static uint8_t layout_type = 2;
static void layout_texture(Board *x, uint32_t ref, uint16_t mips, uint16_t slices, uint64_t base, uint64_t stride, const uint8_t *table,
                           uint32_t w, uint32_t h, uint32_t at_va) {
    uint32_t tsize = mips * 36u, size = 12 + 0x18 + tsize + 12 + 0x18;
    uint8_t *rec = calloc(1, size);
    uint64_t length = 0x4000;
    uint32_t vpage = 3;
    memcpy(rec, &length, 8), memcpy(rec + 8, &vpage, 4);
    memcpy(rec + 12, &mips, 2), memcpy(rec + 14, &slices, 2);
    memcpy(rec + 16, &base, 8), memcpy(rec + 24, &stride, 8);
    memcpy(rec + 12 + 0x18, table, tsize);
    uint32_t th[3] = {0x14, 12 + 0x18, ref};
    memcpy(rec + 12 + 0x18 + tsize, th, 12);
    v1(rec + 12 + 0x18 + tsize + 12, layout_type, 1 | 4, 80, w, h, 1, 1, 1);
    if (at_va < 0x4000) object_entry(x, ref, 2, at_va, rec, size);
    else {
        for (uint32_t i = 0; i < (size + 0x3fff) / 0x4000 + 1; i++) wr32(x, TL0 + 4 * (at_va / 0x4000 + i), (uint32_t)(PG(30 + i) >> 14));
        hv_phys_write(&x->m, PG(30), rec, size); /* the pages follow each other */
        uint32_t e[3] = {2 | size << 8, at_va, 0};
        hv_phys_write(&x->m, TMEM + OBJS + ref * 12ull, e, 12);
    }
    free(rec);
}
static void mip(uint8_t *t, uint64_t off, uint64_t row, uint32_t w, uint32_t h, uint32_t d) {
    memcpy(t, &off, 8), memcpy(t + 0x10, &row, 8), memcpy(t + 0x18, &w, 4), memcpy(t + 0x1c, &h, 4), memcpy(t + 0x20, &d, 4);
}
/* An exec reading texture ref 10 (so it is made, and its memory read): finished within limit seconds */
static void use_texture(Board *x, double limit) {
    double t0 = seconds();
    use_ref(x, 10, 4);
    printf("    exec finished after %.2f s\n", seconds() - t0);
    if (seconds() - t0 > limit) exit(4);
}

/* ---- Every command tag, with made-up bodies ---- */

/* The tags each encoder's decoder takes (pvg_metal.m's render:, compute:, blit:, info:, and the pass's own) */
static const uint16_t render_tags[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 24, 25, 26, 27, 28, 29, 30, 32,
                                       33, 34, 35, 36, 101, 104, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 122, 123,
                                       124, 125, 126, 127, 128, 129, 130, 131, 134, 135, 137, 154, 155, 157, 161, 162, 164, 165, 166};
static const uint16_t compute_tags[] = {200, 202, 203, 204, 206, 207, 208, 209, 211, 212, 213, 214, 215, 216, 219};
static const uint16_t blit_tags[] = {300, 301, 302, 303, 306, 307, 308, 309, 310, 311, 314, 315, 316, 317};
static const uint16_t info_tags[] = {450, 451, 457, 460, 461, 462, 463, 464, 466, 467, 468, 469};
#define OBJ_FIRST 20u /* hv_pvg_metal_test_objects: render, patches, tile, compute, depth, sampler, fence, heap */

static uint64_t rng;
static uint32_t rnd(void) {
    rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
    return (uint32_t)rng;
}
/* What a guest's fields hold: mostly small numbers and refs, sometimes edges, sometimes anything */
static uint32_t field(void) {
    static const uint32_t pool[] = {0, 0, 0, 1, 1, 2, 3, 4, 5, 7, 8, 16, 31, 32, 64, 100, 128, 0x100, 0x3fff, 0x4000, 0xffff, 0x10000,
                                    0x7fffffff, 0x80000000, 0xfffffff0, 0xffffffff, BUF_REF, 10, 11, 12, OBJ_FIRST, OBJ_FIRST + 1,
                                    OBJ_FIRST + 2, OBJ_FIRST + 3, OBJ_FIRST + 4, OBJ_FIRST + 5, OBJ_FIRST + 6, OBJ_FIRST + 7,
                                    0x3f800000 /* 1.0f */};
    uint32_t k = rnd() % 64;
    return k < sizeof pool / sizeof *pool ? pool[k] : k < 52 ? rnd() % 64 : rnd();
}
static uint32_t body(uint8_t *b) {
    static const uint32_t sizes[] = {0, 4, 8, 12, 16, 20, 24, 32, 40, 48, 56, 64, 72, 88, 0x58, 0x88, 128, 0x248, 0x250};
    uint32_t n = rnd() % 4 ? sizes[rnd() % (sizeof sizes / sizeof *sizes)] : rnd() % 0x300 & ~3u;
    for (uint32_t i = 0; i < n; i += 4) {
        uint32_t v = field();
        if (rnd() % 4 == 0) v = (field() & 0xffff) | field() << 16; /* two 16-bit fields */
        memcpy(b + i, &v, 4);
    }
    return n;
}
static int iterations(void) {
    const char *e = getenv("HV_FUZZ_ITERS");
    return e && atoi(e) > 0 ? atoi(e) : 24;
}

/* The objects the bodies name: buffer 6 and 12, textures 10 and 11 (64x64 BGRA, 11 with 4 levels), and the test's
   pipelines and states (made by the Metal side itself, at refs OBJ_FIRST...) */
static void fuzz_setup(Board *x) {
    private_texture(x, 10, 2, 80, 64, 64, 1, 1, 1);
    private_texture(x, 11, 2, 80, 64, 64, 4, 1, 1);
    buffer_object(x, 12, 0x3440);
    uint32_t dummy[4] = {0x77};
    for (uint32_t i = 0; i < 8; i++) object_entry(x, OBJ_FIRST + i, 0x77, 0x3500 + i * 16, dummy, sizeof dummy);
}
static void make_test_objects(Board *x) {
    HvPvgMetal *m = hv_pvg_lite_metal(x->b.pvg);
    if (!hv_pvg_metal_test_objects) { printf("    this build has no test objects\n"); exit(94); }
    if (hv_pvg_metal_test_objects(m, TASK, OBJ_FIRST) != 8) { printf("    test objects not made\n"); exit(93); }
}
static void fuzz(Board *x, int type) {
    fuzz_setup(x);
    use_ref(x, 10, 4), use_ref(x, 11, 5), use_ref(x, 12, 6);
    make_test_objects(x);
    const uint16_t *tags = type == 0 ? render_tags : type == 1 ? compute_tags : type == 2 ? blit_tags : info_tags;
    unsigned ntags = type == 0   ? sizeof render_tags / 2
                     : type == 1 ? sizeof compute_tags / 2
                     : type == 2 ? sizeof blit_tags / 2
                                 : sizeof info_tags / 2;
    uint32_t stamp = 10, iters = (uint32_t)iterations(), sent = 0;
    static uint8_t recs[0x3f00];
    for (unsigned t = 0; t < ntags; t++) {
        uint32_t tag = tags[t];
        for (uint32_t it = 0; it < iters; it++) {
            uint32_t n = 0;
            uint8_t b[0x300];
            if (type == 0) {
                int pass_tag = tag == 26 || (tag >= 30 && tag <= 36);
                int patches = (tag >= 12 && tag <= 15) || tag == 18 || tag == 19, tile = tag == 155 || tag == 162;
                if (pass_tag)
                    for (int k = 0; k < 2; k++) {
                        uint32_t len = body(b);
                        n += put_rec(recs + n, tag, b, len);
                    }
                if (tag != 26) n += pass_rec(recs + n, 2, 1, 0);
                uint32_t pipe = OBJ_FIRST + (patches ? 1 : tile ? 2 : 0);
                n += put_rec(recs + n, 116, &pipe, 4);
                if (patches) {
                    uint32_t f[5] = {BUF_REF, 0, 0, 0, 0};
                    n += put_rec(recs + n, 122, f, 20);
                }
                if (!pass_tag)
                    for (int k = 0; k < 3; k++) {
                        uint32_t len = body(b);
                        n += put_rec(recs + n, tag, b, len);
                    }
                struct { uint32_t primitive; uint16_t start, count; } d = {3, 0, 3};
                n += put_rec(recs + n, 1, &d, sizeof d);
            } else if (type == 1) {
                uint32_t pipe = OBJ_FIRST + 3;
                n += put_rec(recs + n, 208, &pipe, 4);
                for (int k = 0; k < 3; k++) {
                    uint32_t len = body(b);
                    n += put_rec(recs + n, tag, b, len);
                }
                uint64_t sizes[6] = {4, 1, 1, 4, 1, 1};
                n += put_rec(recs + n, 202, sizes, sizeof sizes);
            } else
                for (int k = 0; k < 3; k++) {
                    uint32_t len = body(b);
                    n += put_rec(recs + n, tag, b, len);
                }
            if (getenv("HV_FUZZ_SHOW")) { /* each exec's records, to find one again */
                printf("    exec %u:", stamp);
                for (uint32_t i = 0; i < n; i += 4) printf(" %08x", *(uint32_t *)(recs + i));
                printf("\n");
            }
            segment_exec(x, stamp++, (uint32_t)type, recs, n);
            sent++;
        }
    }
    printf("    %u execs, %u tags, %u bodies each\n", sent, ntags, iters * 3);
}

static int which;

static void run_case(void) {
    if (build(&X) != 0) { printf("    no board\n"); exit(90); }
    Board *x = &X;
    start_task(x);
    use_ref(x, BUF_REF, 3); /* the first exec makes the Metal side */
    if (!hv_pvg_lite_metal(x->b.pvg)) { printf("    no Metal here: nothing to try\n"); exit(0); }
    switch (which) {
    case 1: { /* info 451 (a texture's size in a heap) with buffer ref 0 */
        uint8_t body[0x2c] = {0}, recs[64];
        v1(body, 2, 1, 80, 64, 64, 1, 1, 1);
        segment_exec(x, 4, 4, recs, put_rec(recs, 451, body, sizeof body));
        break;
    }
    case 2: { /* info 461 (a buffer's address) to an offset that wraps the bounds check */
        struct __attribute__((packed)) { uint32_t ref, buf; uint64_t off; uint32_t pad[4]; } q = {BUF_REF, BUF_REF, 0xfffffffffffffff8ull, {0}};
        uint8_t recs[64];
        segment_exec(x, 4, 4, recs, put_rec(recs, 461, &q, sizeof q));
        break;
    }
    case 3: { /* a serialized pipeline whose data length is past its record */
        uint32_t rec[4] = {0xe, 16, 7, 0x100000};
        object_entry(x, 7, 7, 0x2000, rec, sizeof rec);
        use_ref(x, 7, 4);
        break;
    }
    case 4: case 5: case 6: case 7: case 8: { /* textures Metal can't make */
        static const struct { uint8_t type; uint16_t format; uint32_t w, h; uint16_t mips, samples; } t[] = {
            [4] = {2, 80, 0, 64, 1, 1}, [5] = {2, 80, 64, 64, 1, 4}, [6] = {2, 9999, 64, 64, 1, 1}, [7] = {2, 80, 64, 64, 9, 1},
            [8] = {2, 80, 100000, 4, 1, 1}};
        private_texture(x, 10, t[which].type, t[which].format, t[which].w, t[which].h, t[which].mips, t[which].samples, 1);
        use_ref(x, 10, 4);
        break;
    }
    case 9: { /* the root ring (at the stamps page + 0x100, 0x100 long): a command of 16 bytes, and the write pointer
                 at 4. Walking the ring by 16 never reaches it. */
        uint32_t hdr[4] = {0x99, 16, 0, 0};
        hv_phys_write(&x->m, STAMPS + 0x100, hdr, sizeof hdr);
        gfx(x, 0x1010, 0x100), gfx(x, 0x1004, 0x200);
        gfx(x, 0x1008, 4);
        use_ref(x, BUF_REF, 4); /* and the device carries on */
        break;
    }
    case 10: { /* the mapper told 2^40 entries were submitted */
        uint32_t entries[4] = {2, 999, 1, 0};
        hv_phys_write(&x->m, PG(13), entries, sizeof entries);
        mmio(x, VMAPPLE_IOSFC, 0x1000, PG(13), 8);
        mmio(x, VMAPPLE_IOSFC, 0x1008, 1, 8);
        mmio(x, VMAPPLE_IOSFC, 0x1018, 1ull << 40, 8);
        use_ref(x, BUF_REF, 4);
        break;
    }
    case 11: { /* a swap command with only four of its ten words */
        uint32_t args[4] = {0, 0, 1, 0};
        command(x, 8, 4, args, 4);
        must_finish(x, 4);
        break;
    }
    case 12: { /* a texture entry of 20 bytes: shorter than its layout */
        uint8_t rec[20] = {0};
        object_entry(x, 10, 2, 0x2000, rec, sizeof rec);
        use_ref(x, 10, 4);
        break;
    }
    case 13: { /* an info record of 8 bytes: shorter than its fields */
        uint32_t q[2] = {BUF_REF, BUF_REF};
        uint8_t recs[64];
        segment_exec(x, 4, 4, recs, put_rec(recs, 461, q, sizeof q));
        break;
    }
    case 14: { /* a sampler record of 16 bytes: its LOD clamps are past it */
        uint32_t rec[4] = {3, 16, 7, 0};
        object_entry(x, 7, 7, 0x2000, rec, sizeof rec);
        use_ref(x, 7, 4);
        break;
    }
    case 15: { /* a view of levels 0..4 of a 1-level texture */
        private_texture(x, 10, 2, 80, 64, 64, 1, 1, 1);
        use_ref(x, 10, 4);
        struct __attribute__((packed)) { uint32_t tag, size, ref, parent; uint16_t format, type; uint64_t levels[2], slices[2]; uint8_t swz[4]; } v = {
            8, 0, 11, 10, 80, 2, {0, 4}, {0, 1}, {2, 3, 4, 5}};
        v.size = sizeof v;
        object_entry(x, 11, 8, 0x2000, &v, sizeof v);
        use_ref(x, 11, 5);
        break;
    }
    case 16: { /* a linear texture over buffer 6 with rows of 3 bytes */
        struct __attribute__((packed)) { uint32_t tag, size, ref, buffer; uint64_t offset, row; uint8_t d[24]; } l = {9, 0, 11, BUF_REF, 0, 3, {0}};
        l.size = sizeof l;
        v1(l.d, 2, 1, 80, 16, 16, 1, 1, 1);
        object_entry(x, 11, 8, 0x2000, &l, sizeof l);
        use_ref(x, 11, 4);
        break;
    }
    case 17: { /* a sampler with anisotropy 31 and address modes 9 */
        uint64_t bits = 31ull << 26 | 9ull << 8 | 9ull << 12 | 9ull << 16 | 15ull << 20 | 3ull << 24;
        uint32_t rec[8] = {3, 32, 7};
        memcpy(rec + 3, &bits, 8);
        float lod[2] = {0, 1000};
        memcpy(rec + 5, lod, 8);
        object_entry(x, 7, 7, 0x2000, rec, sizeof rec);
        use_ref(x, 7, 4);
        break;
    }
    case 18: { /* a heap texture placed 1 MB into its 16 KB heap */
        uint8_t rec[12 + 4 + 0x24 + 8] = {0};
        uint32_t hdr[4] = {0x15, sizeof rec, 11, HEAP_REF};
        memcpy(rec, hdr, 16);
        v1(rec + 16, 2, 1, 80, 64, 64, 1, 1, 1);
        uint64_t offset = 1 << 20;
        memcpy(rec + 16 + 0x24, &offset, 8);
        object_entry(x, 11, 8, 0x2000, rec, sizeof rec);
        use_ref(x, HEAP_REF, 4);
        use_ref(x, 11, 5);
        break;
    }
    case 19: case 20: { /* pass store action 9; store action options 0xffff */
        private_texture(x, 10, 2, 80, 64, 64, 1, 1, 1);
        uint8_t b[512];
        uint32_t n = pass_rec(b, 2, which == 19 ? 9 : 1, which == 19 ? 0 : 0xffff);
        render_exec(x, 4, b, n);
        break;
    }
    case 21: { /* tile size 7x7 before the pass */
        private_texture(x, 10, 2, 80, 64, 64, 1, 1, 1);
        uint8_t b[512];
        uint32_t n = 0, t = 7 | 7u << 16;
        n += put_rec(b, 36, &t, 4), n += pass_rec(b + n, 2, 1, 0);
        render_exec(x, 4, b, n);
        break;
    }
    case 22: { /* fragment textures index 120, count 128 */
        private_texture(x, 10, 2, 80, 64, 64, 1, 1, 1);
        uint8_t b[2048];
        uint32_t n = pass_rec(b, 2, 1, 0), list[2 + 128] = {120, 128};
        for (int i = 0; i < 128; i++) list[2 + i] = 10;
        n += put_rec(b + n, 114, list, sizeof list);
        render_exec(x, 4, b, n);
        break;
    }
    case 23: { /* vertex buffers index 0xfffffff0, count 31 */
        private_texture(x, 10, 2, 80, 64, 64, 1, 1, 1);
        uint8_t b[2048];
        uint32_t n = pass_rec(b, 2, 1, 0), list[2 + 31 * 3] = {0xfffffff0u, 31};
        for (int i = 0; i < 31; i++) list[2 + i * 3] = BUF_REF;
        n += put_rec(b + n, 125, list, sizeof list);
        render_exec(x, 4, b, n);
        break;
    }
    case 24: { /* 100 viewports */
        private_texture(x, 10, 2, 80, 64, 64, 1, 1, 1);
        static uint8_t b[8192];
        uint32_t n = pass_rec(b, 2, 1, 0);
        double v[100 * 6];
        for (int i = 0; i < 100; i++) v[i * 6] = 0, v[i * 6 + 1] = 0, v[i * 6 + 2] = 8, v[i * 6 + 3] = 8, v[i * 6 + 4] = 0, v[i * 6 + 5] = 1;
        n += put_rec(b + n, 131, v, sizeof v);
        render_exec(x, 4, b, n);
        break;
    }
    case 25: case 26: { /* a buffer's ref as the pipeline; as a fragment texture */
        private_texture(x, 10, 2, 80, 64, 64, 1, 1, 1);
        uint8_t b[512];
        uint32_t n = pass_rec(b, 2, 1, 0), ref = BUF_REF, list[3] = {0, 1, BUF_REF};
        n += which == 25 ? put_rec(b + n, 116, &ref, 4) : put_rec(b + n, 114, list, sizeof list);
        render_exec(x, 4, b, n);
        break;
    }
    case 27: { /* a 1-slice 1-level 64x64 texture whose table says 4 slices, its level 1024x1024 with rows of 16 bytes */
        uint8_t t[36] = {0};
        mip(t, 0, 16, 1024, 1024, 1);
        layout_texture(x, 10, 1, 4, 0, 0, t, 64, 64, 0x2000);
        use_texture(x, 5);
        break;
    }
    case 28: { /* bytes per row times rows wraps to 0 */
        uint8_t t[36] = {0};
        mip(t, 0, 1ull << 62, 64, 4, 1);
        layout_texture(x, 10, 1, 1, 0, 0, t, 64, 64, 0x2000);
        use_texture(x, 5);
        break;
    }
    case 29: case 31: { /* 16383 levels by 65535 slices (by 6 faces: a cube), each one past the memory: once looped through */
        layout_type = which == 31 ? 5 : 2;
        uint32_t mips = 0x3fff;
        uint8_t *t = calloc(mips, 36);
        layout_texture(x, 10, (uint16_t)mips, 65535, 1ull << 40, 0, t, 64, 64, 0x10000);
        free(t);
        use_texture(x, 2);
        break;
    }
    case 30: { /* 2 levels on a 1-level texture, level 1 within the memory */
        uint8_t t[72] = {0};
        mip(t, 0, 256, 64, 64, 1), mip(t + 36, 0, 256, 32, 32, 1);
        layout_texture(x, 10, 2, 1, 0, 0, t, 64, 64, 0x2000);
        use_texture(x, 5);
        break;
    }
    case 32: { /* a cube, 1 level by 65535 slices by 6 faces, each of size 0: once a copy encoded for each */
        layout_type = 5;
        uint8_t t[36] = {0};
        mip(t, 0, 0, 0, 0, 0);
        layout_texture(x, 10, 1, 65535, 0, 0, t, 64, 64, 0x2000);
        use_texture(x, 2);
        break;
    }
    case 33: fuzz(x, 0); break;
    case 34: fuzz(x, 1); break;
    case 35: fuzz(x, 2); break;
    case 36: fuzz(x, 4); break;
    case 37: { /* a heap of 1 TB */
        struct __attribute__((packed)) { uint32_t vpage; uint64_t length; uint8_t flags; } h = {2, 1ull << 40, 0};
        object_entry(x, 7, 13, 0x2000, &h, sizeof h);
        use_ref(x, 7, 4);
        break;
    }
    case 38: case 39: case 40: { /* a patch draw with a pipeline that draws none; a BGRA pipeline in an RGBA pass; a draw
                                    with the patch pipeline and no patches */
        fuzz_setup(x);
        if (which == 39) private_texture(x, 10, 2, 70, 64, 64, 1, 1, 1);
        use_ref(x, 10, 4);
        make_test_objects(x);
        uint8_t b[1024];
        uint32_t n = pass_rec(b, 2, 1, 0), pipe = OBJ_FIRST + (which == 40 ? 1 : 0);
        n += put_rec(b + n, 116, &pipe, 4);
        uint64_t f[3] = {BUF_REF, 0, 0};
        n += put_rec(b + n, 122, f, 20);
        if (which == 38) { /* drawPatches: {count, start, count, ...} */
            uint32_t d[8] = {3, 0, 1, 0, 0, 0, 0, 1}; /* instances (at 28) 1 */
            n += put_rec(b + n, 12, d, sizeof d);
        } else {
            struct { uint32_t primitive; uint16_t start, count; } d = {3, 0, 3};
            n += put_rec(b + n, 1, &d, sizeof d);
        }
        render_exec(x, 5, b, n);
        break;
    }
    case 41: case 42: { /* a copy from texture 10 to 11 past 11's level 3; a fill past buffer 6's end */
        fuzz_setup(x);
        use_ref(x, 10, 4), use_ref(x, 11, 5);
        uint8_t b[256] = {0}, recs[512];
        uint32_t n;
        if (which == 41) {
            uint32_t refs[2] = {10, 11};
            uint64_t size[3] = {64, 64, 1};
            uint16_t sl[4] = {0, 0, 0, 3};
            memcpy(b, refs, 8), memcpy(b + 0x20, size, 24), memcpy(b + 0x50, sl, 8);
            n = put_rec(recs, 303, b, 0x58);
        } else {
            uint32_t ref = BUF_REF;
            uint64_t at = 0x3ff0, len = 0x20;
            memcpy(b, &ref, 4), memcpy(b + 4, &at, 8), memcpy(b + 12, &len, 8);
            n = put_rec(recs, 306, b, 24);
        }
        segment_exec(x, 6, 2, recs, n);
        break;
    }
    case 44: case 45: { /* a draw of 15 points 3,949,456,801 times (what hung the GPU); a dispatch of 2^34 threads */
        fuzz_setup(x);
        use_ref(x, 10, 4);
        make_test_objects(x);
        uint8_t b[1024];
        uint32_t n;
        if (which == 44) {
            n = pass_rec(b, 2, 1, 0);
            uint32_t pipe = OBJ_FIRST;
            n += put_rec(b + n, 116, &pipe, 4);
            struct __attribute__((packed)) { uint64_t start, count; uint32_t instances; } d = {0, 15, 3949456801u};
            n += put_rec(b + n, 2, &d, sizeof d); /* 20 bytes, as the guest sent it: instances' high half and the primitive 0 */
            render_exec(x, 5, b, n);
        } else {
            uint32_t pipe = OBJ_FIRST + 3;
            n = put_rec(b, 208, &pipe, 4);
            uint64_t sizes[6] = {65536, 65536, 1, 4, 1, 1};
            n += put_rec(b + n, 200, sizes, sizeof sizes);
            segment_exec(x, 5, 1, b, n);
        }
        break;
    }
    case 46: { /* the GPU fault breaker: a failure the guest can't cause (NotPermitted, a background iPad app) leaves it,
                  a GPU timeout trips it, and the guest's next execs still finish */
        if (!hv_pvg_metal_test_gpu_failed || !hv_pvg_lite_gpu_faulted) { printf("    this build has no breaker\n"); exit(94); }
        HvPvgMetal *m = hv_pvg_lite_metal(x->b.pvg);
        hv_pvg_metal_test_gpu_failed(m, 7); /* MTLCommandBufferErrorNotPermitted */
        if (hv_pvg_lite_gpu_faulted(x->b.pvg)) { printf("    tripped by NotPermitted\n"); exit(6); }
        uint8_t body[0x2c] = {0}, recs[64];
        v1(body, 2, 1, 80, 64, 64, 1, 1, 1);
        uint32_t buf = BUF_REF;
        uint64_t off = 0x2000;
        memcpy(body + 0x20, &buf, 4), memcpy(body + 0x24, &off, 8);
        uint32_t n = put_rec(recs, 451, body, sizeof body);
        segment_exec(x, 4, 4, recs, n); /* runs */
        hv_pvg_metal_test_gpu_failed(m, 2); /* MTLCommandBufferErrorTimeout */
        hv_pvg_metal_test_gpu_failed(m, 3); /* a second failure: told once */
        if (!hv_pvg_lite_gpu_faulted(x->b.pvg)) { printf("    not tripped by a timeout\n"); exit(6); }
        uint64_t was[2] = {0, 0};
        hv_phys_write(&x->m, BUF_PAGE + 0x2000, was, 16);
        segment_exec(x, 5, 4, recs, n); /* dropped, and finished */
        segment_exec(x, 6, 4, recs, n);
        uint64_t got[2];
        hv_phys_read(&x->m, BUF_PAGE + 0x2000, got, 16);
        if (got[0]) { printf("    an exec ran after the breaker tripped\n"); exit(7); }
        mmio(x, VMAPPLE_IOSFC, 0x1020, 0, 8); /* the guest's driver starting its rings again: the breaker stays */
        if (!hv_pvg_lite_gpu_faulted(x->b.pvg)) { printf("    re-armed by a restart\n"); exit(8); }
        break;
    }
    case 47: case 48: { /* 47: an indirect draw (tag 16) whose arguments in buffer 6 ask 15 vertices x 3,949,456,801
                           instances; 48: an indexed one (tag 17), 2^31 indexes x 2^31 instances, 32-bit indexes in buffer 12
                           (16 KB: 4096 of them). Each must draw from the pass's clamped copy, with the job the clamp needs
                           (the clamp's own numbers: test_pvg_validate, on the GPU) */
        if (!hv_pvg_metal_test_indirect) { printf("    this build clamps no indirect draws\n"); exit(94); }
        fuzz_setup(x);
        use_ref(x, 10, 4);
        make_test_objects(x);
        uint32_t args[5] = {15, 3949456801u, 0, 0, 0};
        if (which == 48) args[0] = 0x80000000u, args[1] = 0x80000000u;
        hv_phys_write(&x->m, BUF_PAGE + 0x3000, args, sizeof args);
        uint8_t b[1024];
        uint32_t n = pass_rec(b, 2, 1, 0), pipe = OBJ_FIRST;
        n += put_rec(b + n, 116, &pipe, 4);
        if (which == 47) {
            struct __attribute__((packed)) { uint32_t ref; uint16_t primitive, unused; uint64_t off; } d = {BUF_REF, 0, 0, 0x3000};
            n += put_rec(b + n, 16, &d, sizeof d);
        } else {
            struct __attribute__((packed)) { uint16_t primitive, type; uint32_t index, indirect; uint64_t ioff, aoff; } d = {0, 1, 12, BUF_REF, 0, 0x3000};
            n += put_rec(b + n, 17, &d, sizeof d);
        }
        render_exec(x, 5, b, n);
        uint32_t jobs[4 * 4] = {0}, njobs = 0;
        uint64_t drawn = hv_pvg_metal_test_indirect(hv_pvg_lite_metal(x->b.pvg), jobs, 4, &njobs);
        printf("    %llu drawn from clamped arguments; %u jobs, the first kind %u weight %u limit %u\n", (unsigned long long)drawn, njobs, jobs[0],
               jobs[1], jobs[2]);
        if (drawn != 1 || njobs != 1) exit(5);
        if (which == 47 && (jobs[0] != 0 || jobs[1] != 1 || jobs[2] != 0xffffffffu)) exit(6);
        if (which == 48 && (jobs[0] != 1 || jobs[1] != 1 || jobs[2] != 4096)) exit(6);
        break;
    }
    case 49: case 50: { /* one exec of as many 2^30 draws (each at the cap for one command), or 2^30-thread dispatches, as fit */
        fuzz_setup(x);
        use_ref(x, 10, 4);
        make_test_objects(x);
        static uint8_t b[0x3f00];
        uint32_t n, count = 0;
        if (which == 49) {
            uint32_t pipe = OBJ_FIRST;
            n = pass_rec(b, 2, 1, 0);
            n += put_rec(b + n, 116, &pipe, 4);
            struct __attribute__((packed)) { uint64_t start, count, instances; uint16_t primitive; } d = {0, 32768, 32768, 0};
            while (n + 8 + sizeof d + 8 < 0x3000) n += put_rec(b + n, 2, &d, sizeof d), count++;
            render_exec(x, 5, b, n);
        } else {
            uint32_t pipe = OBJ_FIRST + 3;
            n = put_rec(b, 208, &pipe, 4);
            uint64_t sizes[6] = {1073741824, 1, 1, 1, 1, 1};
            while (n + 8 + sizeof sizes < 0x3000) n += put_rec(b + n, 202, sizes, sizeof sizes), count++;
            segment_exec(x, 5, 1, b, n);
        }
        printf("    %u commands of 2^30 each in one exec\n", count);
        break;
    }
    case 51: case 52: case 53: { /* 51: 5 x RGBA16Float at 4x (Metal takes it: tile 32x16); 52: BGRA8 at 4x with a 32x32 tile
                                    asked (Metal refuses it); 53: 4x depth stored with CustomSampleDepthStore (Metal takes it) */
        static const uint32_t refs[5] = {10, 11, 13, 14, 15};
        int colours = which == 51 ? 5 : 1;
        for (int i = 0; i < colours; i++) private_texture(x, refs[i], 4, which == 51 ? 115 : 80, 64, 64, 1, 4, 1);
        if (which == 53) private_texture(x, 16, 4, 252, 64, 64, 1, 4, 1);
        for (int i = 0; i < colours; i++) use_ref(x, refs[i], 4 + (uint32_t)i);
        if (which == 53) use_ref(x, 16, 9);
        static uint8_t b[0x1000];
        uint8_t pass[0x248] = {0};
        for (int i = 0; i < colours; i++) {
            uint32_t o = 0x4c + (uint32_t)i * 0x3c;
            uint16_t load = 2, store = 1;
            memcpy(pass + o, &refs[i], 4), memcpy(pass + o + 20, &load, 2), memcpy(pass + o + 22, &store, 2);
        }
        if (which == 53) {
            uint32_t d = 16;
            uint16_t load = 2, store = 5;
            memcpy(pass, &d, 4), memcpy(pass + 20, &load, 2), memcpy(pass + 22, &store, 2);
        }
        uint32_t n = 0;
        if (which == 52) {
            uint32_t t = 32 | 32u << 16;
            n += put_rec(b + n, 36, &t, 4);
        }
        n += put_rec(b + n, 26, pass, sizeof pass);
        render_exec(x, 10, b, n);
        break;
    }
    case 54: { /* a copy within texture 10, level 0 slice 0: 16x16 from (0,0) to (32,32), apart (Metal takes it) */
        private_texture(x, 10, 2, 80, 64, 64, 1, 1, 1);
        use_ref(x, 10, 4);
        uint8_t body[0x58] = {0}, recs[256];
        uint32_t refs[2] = {10, 10};
        uint64_t from[3] = {0, 0, 0}, size[3] = {16, 16, 1}, to[3] = {32, 32, 0};
        memcpy(body, refs, 8), memcpy(body + 8, from, 24), memcpy(body + 0x20, size, 24), memcpy(body + 0x38, to, 24);
        segment_exec(x, 5, 2, recs, put_rec(recs, 303, body, sizeof body));
        break;
    }
    case 55: case 56: case 57: { /* (the third review's A) one exec of 8 command buffers of about 64 MB each, task VA 16 KB
                                    to 64 MB all one page: 55 the same range 8 times, the segment as long as the range; 56
                                    8 ranges, each segment empty; 57 8 ranges, each segment as long as its range. Each
                                    copy is kept to the exec's end: at most one 64 MB range's worth of them, all told. */
        for (uint32_t v = 1; v < 0x1000; v++) wr32(x, TL0 + 4 * v, (uint32_t)(BUF_PAGE >> 14));
        uint32_t head[2] = {which == 56 ? 0 : 0xffffffffu, 4}; /* an info segment of nothing (its records all 0) */
        hv_phys_write(&x->m, BUF_PAGE, head, 8);
        uint32_t ex[3 + 4 * 8] = {TASK, 0, 8};
        for (uint32_t b = 0; b < 8; b++) {
            uint64_t off = which == 55 ? 0x4000 : 0x4000ull * (b + 1), len = (64ull << 20) - off - 0x4000;
            memcpy(&ex[3 + 4 * b], &off, 8), memcpy(&ex[5 + 4 * b], &len, 8);
        }
        struct rusage u0, u1;
        getrusage(RUSAGE_SELF, &u0);
        command(x, 0x43, 4, ex, 3 + 4 * 8);
        must_finish(x, 4);
        getrusage(RUSAGE_SELF, &u1);
        double grew = (u1.ru_maxrss - u0.ru_maxrss) / 1048576.0, most = which == 56 ? 16 : 100;
        printf("    the exec's peak memory grew %.0f MB (at most %.0f)\n", grew, most);
        if (grew > most) exit(5);
        break;
    }
    case 58: { /* (A) a command buffer with a pass and an indirect draw, named twice in one exec: one copy, two
                  segments; each draw takes its own pass's clamped arguments */
        if (!hv_pvg_metal_test_indirect) { printf("    this build clamps no indirect draws\n"); exit(94); }
        fuzz_setup(x);
        use_ref(x, 10, 4);
        make_test_objects(x);
        uint32_t args[5] = {15, 3, 0, 0, 0};
        hv_phys_write(&x->m, BUF_PAGE + 0x3000, args, sizeof args);
        uint8_t b[1024];
        uint32_t n = pass_rec(b, 2, 1, 0), pipe = OBJ_FIRST;
        n += put_rec(b + n, 116, &pipe, 4);
        struct __attribute__((packed)) { uint32_t ref; uint16_t primitive, unused; uint64_t off; } d = {BUF_REF, 0, 0, 0x3000};
        n += put_rec(b + n, 16, &d, sizeof d);
        uint32_t hdr[2] = {n, 0};
        hv_phys_write(&x->m, BUF_PAGE, hdr, 8);
        hv_phys_write(&x->m, BUF_PAGE + 8, b, n);
        uint32_t exec[3 + 2 + 8] = {TASK, 1, 2, 10, 0, 0xC000, 0, 8 + n, 0, 0xC000, 0, 8 + n, 0};
        command(x, 0x43, 5, exec, 13);
        must_finish(x, 5);
        uint32_t jobs[4] = {0}, njobs = 0;
        uint64_t drawn = hv_pvg_metal_test_indirect(hv_pvg_lite_metal(x->b.pvg), jobs, 1, &njobs);
        printf("    %llu drawn from clamped arguments\n", (unsigned long long)drawn);
        if (drawn != 2) exit(5);
        break;
    }
    case 59: case 60: case 61: case 63: { /* (D) the patch pipeline (triangles: 8 bytes of factors a patch) with factors at
                                    the end of buffer 6, read for each patch and instance 8 bytes apart. 59: 8 bytes there,
                                    draw 12 of 1 patch, then of 2; 60: 16 bytes, 1 patch x 3 instances; 61: the same factors, an indirect patch
                                    draw (18): its clamp job carries them; 63 (review 4): 16 bytes, 1 patch x 1 instance, its patch id from
                                    an index buffer (buffer 6): the guest's, which may be any */
        fuzz_setup(x);
        use_ref(x, 10, 4);
        make_test_objects(x);
        uint8_t b[1024];
        uint32_t n = pass_rec(b, 2, 1, 0), pipe = OBJ_FIRST + 1;
        n += put_rec(b + n, 116, &pipe, 4);
        struct __attribute__((packed)) { uint32_t ref; uint64_t off, stride; } f = {BUF_REF, which == 59 ? 0x3ff8 : 0x3ff0, 8};
        n += put_rec(b + n, 122, &f, sizeof f);
        struct __attribute__((packed)) { uint64_t start, count; uint32_t index; uint64_t off, instances, base; uint16_t points; } d = {0, 1, 0, 0, 1, 0, 3};
        if (which == 63) d.index = BUF_REF;
        if (which == 59) {
            n += put_rec(b + n, 12, &d, sizeof d);
            d.count = 2;
            n += put_rec(b + n, 12, &d, sizeof d);
        } else if (which == 60 || which == 63) {
            if (which == 60) d.instances = 3;
            n += put_rec(b + n, 12, &d, sizeof d);
        } else {
            if (!hv_pvg_metal_test_indirect) { printf("    this build clamps no indirect draws\n"); exit(94); }
            uint32_t args[4] = {1, 3, 0, 0};
            hv_phys_write(&x->m, BUF_PAGE + 0x3000, args, sizeof args);
            struct __attribute__((packed)) { uint32_t patches, indirect; uint64_t poff, aoff; uint16_t points; } i = {0, BUF_REF, 0, 0x3000, 3};
            n += put_rec(b + n, 18, &i, sizeof i);
        }
        render_exec(x, 5, b, n);
        if (which == 61) {
            HvClampJob job = {0};
            uint32_t njobs = 0;
            uint64_t drawn = hv_pvg_metal_test_indirect(hv_pvg_lite_metal(x->b.pvg), &job, 1, &njobs);
            printf("    %llu drawn from clamped arguments; job kind %u, factors of %u bytes, %llu there, %llu apart, indexed %u\n",
                   (unsigned long long)drawn, job.kind, job.frec, (unsigned long long)job.froom, (unsigned long long)job.fstride, job.indexed);
            if (drawn != 1 || njobs != 1 || job.kind != HV_CLAMP_PATCHES) exit(5);
            if (job.frec != 8 || job.froom != 16 || job.fstride != 8 || job.indexed || job.step != 3) exit(6);
        }
        break;
    }
    case 62: { /* (H) a pass of 200 indirect draws: their jobs (8000 bytes) are more than go with the dispatch, so they get
                  a buffer; each draw still takes its clamped arguments */
        if (!hv_pvg_metal_test_indirect) { printf("    this build clamps no indirect draws\n"); exit(94); }
        fuzz_setup(x);
        use_ref(x, 10, 4);
        make_test_objects(x);
        uint32_t args[5] = {3, 1, 0, 0, 0};
        hv_phys_write(&x->m, BUF_PAGE + 0x3000, args, sizeof args);
        static uint8_t b[0x2000];
        uint32_t n = pass_rec(b, 2, 1, 0), pipe = OBJ_FIRST;
        n += put_rec(b + n, 116, &pipe, 4);
        struct __attribute__((packed)) { uint32_t ref; uint16_t primitive, unused; uint64_t off; } d = {BUF_REF, 0, 0, 0x3000};
        for (int i = 0; i < 200; i++) n += put_rec(b + n, 16, &d, sizeof d);
        render_exec(x, 5, b, n);
        HvClampJob jobs[200];
        uint32_t njobs = 0;
        uint64_t drawn = hv_pvg_metal_test_indirect(hv_pvg_lite_metal(x->b.pvg), jobs, 200, &njobs);
        printf("    %llu drawn from clamped arguments, %u jobs\n", (unsigned long long)drawn, njobs);
        if (drawn != 200 || njobs != 200 || jobs[199].kind != HV_CLAMP_DRAW) exit(5);
        break;
    }
    case 43: { /* info 451 answered where it was asked: a 64x64 BGRA texture's size and alignment at buffer 6 + 0x2000 */
        uint8_t body[0x2c] = {0}, recs[64];
        v1(body, 2, 1, 80, 64, 64, 1, 1, 1);
        uint32_t buf = BUF_REF;
        uint64_t off = 0x2000;
        memcpy(body + 0x20, &buf, 4), memcpy(body + 0x24, &off, 8);
        uint64_t was[2] = {0, 0};
        hv_phys_write(&x->m, BUF_PAGE + 0x2000, was, 16);
        segment_exec(x, 4, 4, recs, put_rec(recs, 451, body, sizeof body));
        uint64_t got[2];
        hv_phys_read(&x->m, BUF_PAGE + 0x2000, got, 16);
        printf("    answer: %llu bytes, aligned to %llu\n", (unsigned long long)got[0], (unsigned long long)got[1]);
        if (got[0] < 64 * 64 * 4 || !got[1]) exit(5);
        break;
    }
    }
    exit(0);
}

/* What a case must have told (in its stderr), NULL for nothing in particular */
static const char *expect[] = {
    [1] = "heap size query: no buffer",
    [2] = "info 461: ref 6 buffer 6",
    [3] = "serialized object tag 14: 16 bytes, too few for its fields",
    [4] = "texture: ", [5] = "texture: ", [6] = "texture: ", [7] = "texture: ", [8] = "texture: ",
    [9] = "bad root command",
    [10] = "mapper submitted 10000000000 is not within a ring",
    [12] = "texture entry of 20 bytes",
    [13] = "info 461 of 8 bytes skipped",
    [14] = "serialized object tag 3: 16 bytes, too few for its fields",
    [15] = "view tag 8: ",
    [16] = "linear texture: ",
    [18] = "heap texture format 80 64x64 at 1048576 of ref 5 failed",
    [19] = "render pass dropped: ", [20] = "render pass dropped: ", [21] = "render pass dropped: ",
    [22] = "render command 114 (520 bytes) dropped: ", [23] = "render command 125 (380 bytes) dropped: ",
    [24] = "render command 131 (4800 bytes) dropped: ",
    [25] = "a buffer used as a render pipeline", [26] = "a buffer used as a texture",
    [27] = "texture layout: ", [28] = "texture layout: ", [29] = "texture layout: ", [30] = "texture layout: ", [31] = "texture layout: ",
    [32] = "texture layout: ",
    [37] = "heap of 1099511627776 bytes refused",
    [38] = "draw 12 with a pipeline that draws no patches",
    [39] = "pipeline 20 not set: a colour format not the pass's",
    [40] = "draw 1 with a pipeline that draws patches",
    [41] = "blit command 303 dropped: ",
    [42] = "blit command 306 dropped: ",
    [44] = "render command 2 (20 bytes) dropped: more work than one command may ask",
    [45] = "dispatch of 65536x65536x1 groups of 4x1x1: more work than one command may ask",
    [46] = "exec dropped: the GPU failed this Mac's work before",
    [49] = "an exec's work past its budget: the rest of the exec dropped",
    [50] = "an exec's work past its budget: the rest of the exec dropped",
    [52] = "render pass dropped: tile size, for its sample count",
    [57] = "an exec's command buffers past what one may copy",
    [59] = "draw 12 dropped: 2 patches x 1 instances past their tessellation factors",
    [60] = "draw 12 dropped: 1 patches x 3 instances past their tessellation factors",
    [63] = "draw 12 dropped: patch ids from the guest's index buffer, and tessellation factors for each patch",
};
/* What a case must not have told: what it sends is what Metal takes, or must be */
static const char *never[] = {
    [47] = "indirect draw", [48] = "indirect draw",
    [51] = "render pass dropped", [53] = "render pass dropped",
    [54] = "blit command 303 dropped",
    [55] = "past what one may copy", [56] = "past what one may copy", [58] = "indirect draw", [61] = "indirect draw", [62] = "indirect draw",
};
#define CASES 63

extern char **environ;

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *seed = getenv("HV_FUZZ_SEED");
    setenv("HV_PVG_ENCODE_ONLY", "1", 1); /* never the GPU (see the top): the children have it from here too */
    if (argc > 2 && !strcmp(argv[1], "--child")) {
        which = atoi(argv[2]);
        rng = (seed ? strtoull(seed, NULL, 0) : 0x9e3779b97f4a7c15ull) + (uint64_t)which;
        if (!rng) rng = 1;
        alarm(which >= 33 && which <= 36 ? 900 : 60); /* a hang is a failure, not a test that never ends */
        run_case();
    }
    static const char *names[CASES + 1] = {"", "info 451 with buffer ref 0", "info 461 offset wraps the bounds check",
        "serialized pipeline length past its record", "texture width 0", "2D texture with 4 samples", "texture format 9999",
        "9 mips on 64x64", "texture 100000 wide", "root ring never reaches wp", "mapper submitted index 2^40", "short swap command",
        "short texture entry", "short info record", "short sampler record", "view levels past its parent", "linear texture rows of 3",
        "sampler aniso 31 / address 9", "heap texture past its heap's end", "pass store action 9", "pass store options 0xffff", "tile 7x7",
        "fragment textures 120 + 128", "vertex buffers 0xfffffff0 + 31", "100 viewports", "a buffer as the pipeline",
        "a buffer as a fragment texture", "layout: 4 slices, 1024x1024 level", "layout: row x rows wraps",
        "layout: 16383 levels x 65535 slices", "layout: 2 levels on a 1-level texture", "layout: cube 16383 x 65535 x 6",
        "layout: cube 65535 x 6 of size 0", "every render tag", "every compute tag", "every blit tag", "every info tag",
        "heap of 1 TB", "patch draw, pipeline without patches", "BGRA pipeline in an RGBA pass", "plain draw, patch pipeline",
        "texture copy past a level", "fill past a buffer's end", "info 451 answered at its buffer",
        "draw of 15 points 3.9 billion times", "dispatch of 2^34 threads", "GPU fault breaker", "indirect draw 15 x 3.9e9",
        "indexed indirect 2^31 x 2^31", "many 2^30 draws in one exec", "many 2^30 dispatches in one exec", "5 x RGBA16F at 4x MSAA",
        "4x MSAA tile 32x32", "depth CustomSampleDepthStore", "copy within one texture, apart",
        "one 64 MB command buffer 8 times", "8 empty segments of 64 MB ranges", "8 full 64 MB command buffers",
        "an indirect draw's command buffer named twice", "patches past their tessellation factors",
        "instances past their tessellation factors", "an indirect patch draw's tessellation factors", "200 indirect draws in a pass",
        "patch ids from an index buffer, factors per patch"};
    int first = 1, last = CASES, failures = 0;
    if (argc > 1) first = last = atoi(argv[1]);
    if (argc > 2) last = atoi(argv[2]);
    if (first < 1 || last > CASES || first > last) { fprintf(stderr, "cases 1..%d\n", CASES); return 2; }
    for (which = first; which <= last; which++) {
        printf("case %d: %s\n", which, names[which]);
        int out[2];
        if (pipe(out)) return 2;
        char number[16];
        snprintf(number, sizeof number, "%d", which);
        char *args[] = {argv[0], "--child", number, NULL};
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, out[1], 2);
        posix_spawn_file_actions_addclose(&fa, out[0]), posix_spawn_file_actions_addclose(&fa, out[1]);
        pid_t pid;
        int spawned = posix_spawn(&pid, argv[0], &fa, NULL, args, environ);
        posix_spawn_file_actions_destroy(&fa);
        close(out[1]);
        if (spawned) {
            printf("  FAIL: no child (%s)\n", strerror(spawned)), failures++, close(out[0]);
            continue;
        }
        size_t len = 0, cap = 1 << 16;
        char *told = malloc(cap);
        for (ssize_t got; (got = read(out[0], told + len, cap - len - 1)) != 0;) {
            if (got < 0) {
                if (errno == EINTR) continue;
                break;
            }
            len += (size_t)got;
            if (len + 1 == cap) told = realloc(told, cap *= 2);
        }
        told[len] = 0;
        close(out[0]);
        int st = 0, bad = 0;
        waitpid(pid, &st, 0);
        if (WIFSIGNALED(st))
            printf("  FAIL: child killed by signal %d (%s)\n", WTERMSIG(st), WTERMSIG(st) == SIGALRM ? "hung" : strsignal(WTERMSIG(st))), bad = 1;
        else if (WEXITSTATUS(st))
            printf("  FAIL: child exit %d\n", WEXITSTATUS(st)), bad = 1;
        else if (which < (int)(sizeof expect / sizeof *expect) && expect[which] && !strstr(told, expect[which]))
            printf("  FAIL: never told \"%s\"\n", expect[which]), bad = 1;
        else if (which < (int)(sizeof never / sizeof *never) && never[which] && strstr(told, never[which]))
            printf("  FAIL: told \"%s\"\n", never[which]), bad = 1;
        if (bad || getenv("HV_FUZZ_TOLD")) fprintf(stderr, "%s", len > 6000 ? told + len - 6000 : told);
        failures += bad;
        free(told);
    }
    printf("%d cases, %d failures\n", last - first + 1, failures);
    return failures != 0;
}
