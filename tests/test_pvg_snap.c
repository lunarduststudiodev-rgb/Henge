/* Suspend, the screen (devices/pvg_lite.c, devices/pvg_metal.m): a board whose guest has made GPU textures
   that live only on the GPU (no guest memory behind them) is saved, and loaded into a second board. Their
   pixels must be the same there, the device must carry on with the guest's next command, and a screen
   that is already in use, a damaged section, or a save that runs out of time must be refused cleanly.
   Needs Metal: on a Mac with none it says so and passes. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pvg_lite.h"
#include "snapshot.h"
#include "vmapple.h"

#define RAM_SIZE (64ull << 20)

static int checks, failures;
static void check(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got != want) {
        printf("  FAIL %s: %llx, expected %llx\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

typedef struct {
    HvMachine m;
    HvCpu cpu;
    HvVmapple b;
} Board;

static Board A, B, C, D, E, F, G, H, I;
#define S_PVG HV_SNAP_TAG('P', 'V', 'G', ' ')
#define S_PVGM HV_SNAP_TAG('P', 'V', 'G', 'M')

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
static void gfx(Board *x, uint64_t off, uint32_t v) {
    HvMmio *d = hv_mmio_lookup(&x->m, VMAPPLE_GFX);
    hv_dev_lock(&x->m);
    d->write(d->opaque, off, v, 4);
    hv_dev_unlock(&x->m);
}

/* The guest's side, in its memory: 16 KB pages */
#define PG(n) (VMAPPLE_MEM + 0x4000ull * (n))
#define STAMPS PG(0)  /* a stamp per channel */
#define ROOT PG(1)    /* the channels' records, from +0x400 */
#define TABLE PG(2)   /* channel 1's ring pages */
#define RING PG(3)    /* channel 1's ring: one page */
#define TROOT PG(4)   /* task 1's root: its page table and depth */
#define TL0 PG(5)     /* task 1's page table */
#define TMEM PG(6)    /* task 1's offset 0 */
#define TASK 1u
#define OBJS 0x100u   /* task offset of its object list */
#define CH 1u

static void setup_guest(Board *x) {
    wr32(x, ROOT + 0x400 + 16, (uint32_t)(TABLE >> 14));
    wr32(x, TABLE, (uint32_t)(RING >> 14));
    wr32(x, TROOT, (uint32_t)(TL0 >> 14)), wr32(x, TROOT + 4, 1);
    wr32(x, TL0, (uint32_t)(TMEM >> 14));
    gfx(x, 0x1030, (uint32_t)(STAMPS >> 14));
    gfx(x, 0x101c, (uint32_t)(ROOT >> 14));
}

/* A texture with no guest memory, as macOS makes render targets: {tag 0x14, size, ref, v1 description} */
static void texture_object(Board *x, uint32_t ref, uint32_t at, uint8_t type, uint16_t format, uint32_t w, uint32_t h, uint16_t mips,
                           uint16_t samples, uint16_t array) {
    uint8_t rec[36] = {0};
    uint32_t hdr[3] = {0x14, sizeof rec, ref};
    memcpy(rec, hdr, 12);
    uint8_t *d = rec + 12;
    d[0] = type, d[1] = 1 | 4; /* shader read, render target */
    memcpy(d + 2, &format, 2), memcpy(d + 4, &w, 4), memcpy(d + 8, &h, 4);
    uint32_t depth = 1;
    uint16_t options = 0x20; /* private */
    memcpy(d + 12, &depth, 4), memcpy(d + 16, &mips, 2), memcpy(d + 18, &samples, 2), memcpy(d + 20, &array, 2), memcpy(d + 22, &options, 2);
    hv_phys_write(&x->m, TMEM + at, rec, sizeof rec);
    uint32_t e[3] = {8 | (uint32_t)sizeof rec << 8, at, 0}; /* type 8, its size; where */
    hv_phys_write(&x->m, TMEM + OBJS + ref * 12ull, e, 12);
}

/* A command into channel 1's ring, and its doorbell */
static void command(Board *x, uint32_t op, uint32_t stamp, const uint32_t *args, uint32_t n) {
    uint32_t wp = rd32(x, ROOT + 0x400), hdr[3] = {op, 12 + 4 * n, stamp};
    hv_phys_write(&x->m, RING + wp % 0x4000, hdr, 12);
    if (n) hv_phys_write(&x->m, RING + (wp + 12) % 0x4000, args, 4 * n);
    wr32(x, ROOT + 0x400, wp + hdr[1]);
    gfx(x, 0x1020, CH);
}

static int wait_stamp(Board *x, uint32_t stamp) {
    for (int i = 0; i < 4000; i++) {
        if (rd32(x, STAMPS + 4 * CH) == stamp) return 1;
        usleep(1000);
    }
    return 0;
}

static uint32_t bgra[32][64], back_bgra[32][64];

/* ---- Objects with guest memory: a texture, a heap and a buffer on task pages 1, 2 and 3 ---- */
#define GTEX_REF 4u
#define HEAP_REF 5u
#define BUF_REF 6u
#define GTEX_PAGE PG(7)
#define HEAP_PAGE PG(8)
#define BUF_PAGE PG(9)

static void object_entry(Board *x, uint32_t ref, uint32_t type, uint32_t at, const void *rec, uint32_t size) {
    hv_phys_write(&x->m, TMEM + at, rec, size);
    uint32_t e[3] = {type | size << 8, at, 0};
    hv_phys_write(&x->m, TMEM + OBJS + ref * 12ull, e, 12);
}

/* A BGRA8 16 x 8 texture on task page 1, one level: {length, page}, the layout, {tag, size, ref}, its description */
static void guest_texture_object(Board *x) {
    uint8_t rec[12 + 0x18 + 36 + 12 + 24] = {0};
    uint64_t length = 0x4000, row = 64, zero = 0;
    uint32_t vpage = 1, w = 16, h = 8, d = 1;
    uint16_t mips = 1, slices = 1;
    memcpy(rec, &length, 8), memcpy(rec + 8, &vpage, 4);
    uint8_t *layout = rec + 12;
    memcpy(layout, &mips, 2), memcpy(layout + 2, &slices, 2), memcpy(layout + 4, &zero, 8), memcpy(layout + 12, &zero, 8);
    uint8_t *mip = layout + 0x18;
    memcpy(mip, &zero, 8), memcpy(mip + 0x10, &row, 8), memcpy(mip + 0x18, &w, 4), memcpy(mip + 0x1c, &h, 4), memcpy(mip + 0x20, &d, 4);
    uint8_t *t = mip + 36;
    uint32_t hdr[3] = {0x14, 36, GTEX_REF};
    memcpy(t, hdr, 12);
    uint8_t *desc = t + 12;
    uint16_t format = 80, mipc = 1, samples = 1, array = 1, options = 0x20;
    desc[0] = 2, desc[1] = 1 | 4;
    memcpy(desc + 2, &format, 2), memcpy(desc + 4, &w, 4), memcpy(desc + 8, &h, 4);
    memcpy(desc + 12, &d, 4), memcpy(desc + 16, &mipc, 2), memcpy(desc + 18, &samples, 2), memcpy(desc + 20, &array, 2), memcpy(desc + 22, &options, 2);
    object_entry(x, GTEX_REF, 2, 0x1400, rec, sizeof rec);
}

static void heap_object(Board *x) {
    struct __attribute__((packed)) { uint32_t vpage; uint64_t length; uint8_t flags; } h = {2, 0x4000, 0};
    object_entry(x, HEAP_REF, 13, 0x1500, &h, sizeof h);
}

static void buffer_object(Board *x) {
    struct __attribute__((packed)) { uint64_t length; uint32_t vpage, flags; } b = {0x4000, 3, 0};
    object_entry(x, BUF_REF, 1, 0x1600, &b, sizeof b);
}

static void start_task(Board *x) {
    setup_guest(x);
    wr32(x, TL0 + 4, (uint32_t)(GTEX_PAGE >> 14)), wr32(x, TL0 + 8, (uint32_t)(HEAP_PAGE >> 14)), wr32(x, TL0 + 12, (uint32_t)(BUF_PAGE >> 14));
    guest_texture_object(x), heap_object(x), buffer_object(x);
    uint32_t root[4] = {TASK << 1, 0, 0, (uint32_t)(TROOT >> 14)};
    command(x, 0x38, 1, root, 4);
    uint32_t list[7] = {TASK, OBJS, 0, 0x3000, 0, 0x100, 0x100};
    command(x, 0x44, 2, list, 7);
}

/* An exec that uses the texture and the heap, with these flags (2: it writes them) */
static void use_guest_objects(Board *x, uint32_t stamp, uint32_t flags) {
    uint32_t exec[3 + 4] = {TASK, 2, 0, GTEX_REF, flags, HEAP_REF, flags};
    command(x, 0x43, stamp, exec, 7);
}

static uint8_t page_was[3][0x4000], page_now[0x4000];

/* Task 1, its objects, and one exec that makes them on the GPU */
static void make_textures(Board *x) {
    setup_guest(x);
    texture_object(x, 1, 0x1000, 2, 80, 64, 32, 1, 1, 1);  /* BGRA8, 64 x 32 */
    texture_object(x, 2, 0x1100, 3, 10, 13, 7, 3, 1, 2);   /* R8, 13 x 7, 3 levels, 2 slices: rows that need padding */
    texture_object(x, 3, 0x1200, 4, 80, 16, 16, 1, 4, 1);  /* multisample: can't be read back */
    uint32_t root[4] = {TASK << 1, 0, 0, (uint32_t)(TROOT >> 14)};
    command(x, 0x38, 1, root, 4);
    uint32_t list[7] = {TASK, OBJS, 0, 0x3000, 0, 0x100, 0x100};
    command(x, 0x44, 2, list, 7);
    uint32_t exec[3 + 6] = {TASK, 3, 0, 1, 0, 2, 0, 3, 0};
    command(x, 0x43, 3, exec, 9);
}

static void save_file(Board *x, const char *path, int want_ok) {
    HvSnapWriter *w = hv_snap_begin(path, HV_SNAP_NO_SYNC);
    int ok = w && hv_vmapple_snap_save(&x->b, w) == 0 && hv_snap_put_ram(w, &x->m, 4) == 0;
    if (ok) ok = hv_snap_commit(w) == 0;
    else if (w) hv_snap_abort(w);
    check(want_ok ? "save works" : "save refused", ok, want_ok);
}

static int load_file(Board *x, const char *path) {
    HvSnapReader *r = hv_snap_open(path, NULL, 0);
    if (!r) return -1;
    int rc = hv_snap_get_ram(r, &x->m, 4, HV_SNAP_DIRTY_RAM) != 0 || hv_vmapple_snap_load(&x->b, r) != 0 ? -1 : 0;
    hv_snap_close(r);
    return rc;
}

/* The snapshot with its screen section (or the GPU's, beside it) changed by `edit` */
static int rewrite_tag(const char *from, const char *to, void (*edit)(HvSnapOut *o), uint32_t which) {
    HvSnapReader *r = hv_snap_open(from, NULL, 0);
    HvSnapWriter *w = r ? hv_snap_begin(to, HV_SNAP_NO_SYNC) : NULL;
    if (!w) return -1;
    static const uint32_t tags[] = {HV_SNAP_TAG('B', 'O', 'R', 'D'), HV_SNAP_TAG('U', 'A', 'R', 'T'), HV_SNAP_TAG('P', 'L', '3', '1'),
                                    HV_SNAP_TAG('G', 'P', 'I', 'O'), HV_SNAP_TAG('A', 'E', 'S', ' '), HV_SNAP_TAG('B', 'D', 'I', 'F'),
                                    HV_SNAP_TAG('P', 'C', 'I', 'E'), HV_SNAP_TAG('V', 'N', 'E', 'T'), HV_SNAP_TAG('V', 'K', 'B', 'D'),
                                    HV_SNAP_TAG('V', 'M', 'O', 'U'), HV_SNAP_TAG('V', 'T', 'A', 'B'), HV_SNAP_TAG('V', 'S', 'N', 'D'),
                                    HV_SNAP_TAG('X', 'H', 'C', 'I'), S_PVGM, S_PVG, HV_SNAP_GIC};
    for (size_t i = 0; i < sizeof tags / sizeof *tags; i++) {
        HvSnapIn in;
        if (hv_snap_get(r, tags[i], 1, &in) != 0) continue;
        HvSnapOut o = {0};
        hv_snap_out(&o, in.p, in.len);
        if (tags[i] == which) edit(&o);
        if (o.len == 0 && tags[i] == S_PVGM) { free(o.p); continue; } /* edited away */
        hv_snap_put(w, tags[i], 1, &o);
    }
    /* The RAM too, through a board of its own, so that only the edit can make a load fail */
    static Board ram;
    static int built;
    if (!built) built = build(&ram) == 0;
    int ok = built && hv_snap_get_ram(r, &ram.m, 4, HV_SNAP_DIRTY_RAM) == 0 && hv_snap_put_ram(w, &ram.m, 4) == 0;
    hv_snap_close(r);
    if (!ok) { hv_snap_abort(w); return -1; }
    return hv_snap_commit(w);
}

/* An older snapshot's screen section (version 1): the GPU's part inside it, after its length */
static HvSnapOut gpu_part;
static void keep_gpu(HvSnapOut *o) {
    free(gpu_part.p);
    memset(&gpu_part, 0, sizeof gpu_part);
    hv_snap_out(&gpu_part, o->p, o->len);
    o->len = 0;
}
static void as_version_1(HvSnapOut *o) {
    o->p[4] = 1, o->p[5] = o->p[6] = o->p[7] = 0;
    hv_snap_out(o, gpu_part.p, gpu_part.len);
}
static int rewrite_v1(const char *from, const char *to) {
    char mid[1200];
    snprintf(mid, sizeof mid, "%s.mid", to);
    int rc = rewrite_tag(from, mid, keep_gpu, S_PVGM) != 0 || rewrite_tag(mid, to, as_version_1, S_PVG) != 0 ? -1 : 0;
    unlink(mid);
    return rc;
}
static void cut_short(HvSnapOut *o) { o->len -= 5; }
static void bad_version(HvSnapOut *o) { o->p[4] ^= 0x40; }
static void bad_metal_magic(HvSnapOut *o) { o->p[0] ^= 0x40; } /* the GPU's part: its own section, magic first */
static void gpu_cut_short(HvSnapOut *o) { o->len -= 3; }

/* What the GPU drew into objects with guest memory comes back from the snapshot, not from their pages (which
   the save never writes); and the host addresses the guest was told are told again where it still has them. */
static void guest_memory_objects(const char *dir) {
    char path[1100];
    snprintf(path, sizeof path, "%s/d.hvsnap", dir);
    if (build(&D) != 0 || build(&E) != 0) { check("boards D and E", 0, 1); return; }
    start_task(&D);
    /* The guest's own bytes: the GPU's copies are made from them when first used */
    static uint32_t p0[8][16], p1[8][16], back[8][16];
    static uint8_t h0[0x4000], h1[0x4000], hb[0x4000];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 16; x++) p0[y][x] = 0xff000000u | (uint32_t)(y * 16 + x), p1[y][x] = 0xff800000u | (uint32_t)(y * 31 + x * 7);
    for (int i = 0; i < 0x4000; i++) h0[i] = (uint8_t)(i * 7 + 1), h1[i] = (uint8_t)(i * 13 + 5);
    hv_phys_write(&D.m, GTEX_PAGE, p0, sizeof p0);
    hv_phys_write(&D.m, HEAP_PAGE, h0, sizeof h0);
    use_guest_objects(&D, 3, 0);
    check("D: the exec with guest objects finished", wait_stamp(&D, 3), 1);
    HvPvgMetal *md = hv_pvg_lite_metal(D.b.pvg);
    check("D: the texture came from its pages", md && hv_pvg_metal_test_texture(md, TASK, GTEX_REF, back, 64, 0) == 0 && !memcmp(back, p0, sizeof p0), 1);
    check("D: the heap came from its pages", md && hv_pvg_metal_test_bytes(md, TASK, HEAP_REF, hb, sizeof hb, 0) == 0 && !memcmp(hb, h0, sizeof h0), 1);
    if (!md) return;
    /* The GPU draws into both: newer than their pages from here on (bit 2), which the guest hasn't asked back */
    use_guest_objects(&D, 4, 2);
    check("D: the writing exec finished", wait_stamp(&D, 4), 1);
    check("D: the GPU's texture changes", hv_pvg_metal_test_texture(md, TASK, GTEX_REF, p1, 64, 1), 0);
    check("D: the GPU's heap changes", hv_pvg_metal_test_bytes(md, TASK, HEAP_REF, h1, sizeof h1, 1), 0);

    /* Addresses told the guest: one it still has, one it has since written over */
    struct { uint32_t ref, bufref; uint64_t off; } __attribute__((packed)) q1 = {BUF_REF, BUF_REF, 0x100}, q2 = {BUF_REF, BUF_REF, 0x200},
                                                                            q3 = {BUF_REF, BUF_REF, 0x300};
    hv_pvg_metal_test_info(md, TASK, 461, &q1, sizeof q1);
    hv_pvg_metal_test_info(md, TASK, 461, &q2, sizeof q2);
    uint64_t d_addr = 0, over = 0x5eed5eed5eed5eedull;
    hv_phys_read(&D.m, BUF_PAGE + 0x100, &d_addr, 8);
    check("D: the guest was told its buffer's address", d_addr != 0, 1);
    hv_phys_write(&D.m, BUF_PAGE + 0x200, &over, 8);

    hv_phys_read(&D.m, GTEX_PAGE, page_was[0], 0x4000);
    hv_phys_read(&D.m, HEAP_PAGE, page_was[1], 0x4000);
    hv_phys_read(&D.m, BUF_PAGE, page_was[2], 0x4000);
    save_file(&D, path, 1);
    int untouched = 1;
    hv_phys_read(&D.m, GTEX_PAGE, page_now, 0x4000), untouched &= !memcmp(page_now, page_was[0], 0x4000);
    hv_phys_read(&D.m, HEAP_PAGE, page_now, 0x4000), untouched &= !memcmp(page_now, page_was[1], 0x4000);
    hv_phys_read(&D.m, BUF_PAGE, page_now, 0x4000), untouched &= !memcmp(page_now, page_was[2], 0x4000);
    check("saving wrote nothing to the guest's memory", untouched, 1);

    check("E: the snapshot loads", load_file(&E, path), 0);
    hv_vmapple_restored(&E.b);
    HvPvgMetal *me = hv_pvg_lite_metal(E.b.pvg);
    memset(back, 0, sizeof back), memset(hb, 0, sizeof hb);
    check("E: the GPU's texture came back, not its pages'", me && hv_pvg_metal_test_texture(me, TASK, GTEX_REF, back, 64, 0) == 0 && !memcmp(back, p1, sizeof p1), 1);
    check("E: the GPU's heap came back, not its pages'", me && hv_pvg_metal_test_bytes(me, TASK, HEAP_REF, hb, sizeof hb, 0) == 0 && !memcmp(hb, h1, sizeof h1), 1);
    if (!me) return;
    /* Used again without the guest writing them: the GPU's copies stand */
    use_guest_objects(&E, 5, 0);
    check("E: the next exec finished", wait_stamp(&E, 5), 1);
    memset(back, 0, sizeof back), memset(hb, 0, sizeof hb);
    check("E: the texture is still the GPU's", hv_pvg_metal_test_texture(me, TASK, GTEX_REF, back, 64, 0) == 0 && !memcmp(back, p1, sizeof p1), 1);
    check("E: the heap is still the GPU's", hv_pvg_metal_test_bytes(me, TASK, HEAP_REF, hb, sizeof hb, 0) == 0 && !memcmp(hb, h1, sizeof h1), 1);

    uint64_t e_addr = 0, e_fresh = 0, e_over = 0;
    hv_phys_read(&E.m, BUF_PAGE + 0x100, &e_addr, 8);
    hv_pvg_metal_test_info(me, TASK, 461, &q3, sizeof q3);
    hv_phys_read(&E.m, BUF_PAGE + 0x300, &e_fresh, 8);
    hv_phys_read(&E.m, BUF_PAGE + 0x200, &e_over, 8);
    check("E: the guest is told its buffer's address here", e_addr, e_fresh);
    check("E: which is not the old one", e_addr != d_addr, 1);
    check("E: what the guest wrote over an old answer is left alone", e_over, over);
}

static int count_sink(void *ctx, const void *p, size_t n) {
    (void)p;
    *(uint64_t *)ctx += n;
    return 0;
}

/* Storage that is full: nothing may be written, and the person is told */
static uint64_t asked_room;
static int full_room(void *ctx, uint64_t bytes, char *why, size_t why_len) {
    (void)ctx;
    asked_room = bytes;
    snprintf(why, why_len, "there isn't enough free storage (test)");
    return -1;
}

/* A texture bigger than the window the save reads the GPU through (64 MB): read back, and put back, in bands */
static void big_texture(const char *dir) {
    enum { W = 4608, H = 4096 }; /* BGRA8: 72 MB */
    char path[1100];
    snprintf(path, sizeof path, "%s/big.hvsnap", dir);
    if (build(&F) != 0 || build(&G) != 0) { check("big: boards", 0, 1); return; }
    make_textures(&F);
    check("big: the first exec finished", wait_stamp(&F, 3), 1);
    texture_object(&F, 4, 0x1300, 2, 80, W, H, 1, 1, 1);
    uint32_t exec[3 + 2] = {TASK, 1, 0, 4, 0};
    command(&F, 0x43, 4, exec, 5);
    check("big: made", wait_stamp(&F, 4), 1);
    HvPvgMetal *mf = hv_pvg_lite_metal(F.b.pvg);
    uint32_t *px = malloc((size_t)W * H * 4), *back = malloc((size_t)W * H * 4);
    if (!mf || !px || !back) { check("big: memory", 0, 1); free(px), free(back); return; }
    for (size_t i = 0; i < (size_t)W * H; i++) px[i] = (uint32_t)(i * 2654435761u) | 0xff000000u;
    check("big: pixels go in", hv_pvg_metal_test_texture(mf, TASK, 4, px, W * 4, 1), 0);
    save_file(&F, path, 1);
    check("big: loads", load_file(&G, path), 0);
    hv_vmapple_restored(&G.b);
    HvPvgMetal *mg = hv_pvg_lite_metal(G.b.pvg);
    memset(back, 0, (size_t)W * H * 4);
    check("big: pixels read back", mg && hv_pvg_metal_test_texture(mg, TASK, 4, back, W * 4, 0) == 0, 1);
    size_t first_bad = (size_t)-1;
    for (size_t i = 0; i < (size_t)W * H && first_bad == (size_t)-1; i++)
        if (back[i] != px[i]) first_bad = i;
    if (first_bad != (size_t)-1) printf("  big: first different pixel at row %zu, column %zu\n", first_bad / W, first_bad % W);
    check("big: every pixel came back", first_bad == (size_t)-1, 1);
    free(px), free(back);
    unlink(path);
}

/* The GPU's part of a snapshot, damaged every which way: refused or read, never read past */
static void fuzz_metal(const char *path) {
    HvSnapReader *r = hv_snap_open(path, NULL, 0);
    HvSnapIn in, gpu;
    if (!r || hv_snap_get(r, S_PVG, 1, &in) != 0) { check("fuzz: the screen section", 0, 1); if (r) hv_snap_close(r); return; }
    if (hv_snap_get(r, S_PVGM, 1, &gpu) != 0) { check("fuzz: a GPU part to damage", 0, 1); hv_snap_close(r); return; }
    const uint8_t *at = gpu.p;
    uint64_t len = gpu.len;
    check("fuzz: the GPU part as saved is whole", hv_pvg_metal_check(at, len) == 0, 1);
    /* The screen's state whole, as the restore checks it before loading anything (hv_pvg_lite_check) */
    {
        check("early check: the screen's state as saved is taken", hv_pvg_lite_check(in.p, in.len, at, len), 0);
        check("early check: without its GPU part refused", hv_pvg_lite_check(in.p, in.len, NULL, 0), (uint64_t)-1);
        uint8_t *other = malloc(in.len), *other_gpu = malloc(len);
        memcpy(other, in.p, in.len);
        other[4] ^= 0x7f; /* its version */
        check("early check: another version refused", hv_pvg_lite_check(other, in.len, at, len), (uint64_t)-1);
        memcpy(other_gpu, at, len);
        other_gpu[4] ^= 0x7f; /* the GPU part's version */
        check("early check: another GPU part version refused", hv_pvg_lite_check(in.p, in.len, other_gpu, len), (uint64_t)-1);
        check("early check: cut short refused", hv_pvg_lite_check(in.p, in.len - 1, at, len), (uint64_t)-1);
        check("early check: GPU part cut short refused", hv_pvg_lite_check(in.p, in.len, at, len - 1), (uint64_t)-1);
        free(other), free(other_gpu);
    }
    int bad = 0;
    for (uint64_t n = 0; n < len; n++) { /* every length short of whole */
        uint8_t *cut = malloc(n ? n : 1);
        memcpy(cut, at, n);
        bad += hv_pvg_metal_check(cut, n) == 0;
        free(cut);
    }
    check("fuzz: every cut-short GPU part refused", bad, 0);
    srand(1234);
    uint8_t *copy = malloc(len);
    for (int i = 0; i < 20000; i++) {
        memcpy(copy, at, len);
        int flips = 1 + rand() % 4;
        for (int k = 0; k < flips; k++) {
            size_t where = (size_t)rand() % len;
            copy[where] = rand() % 3 == 0 ? 0xff : (uint8_t)rand();
        }
        (void)hv_pvg_metal_check(copy, len); /* any answer: what matters is that nothing past the bytes is read */
    }
    free(copy);
    hv_snap_close(r);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    char dir[1024], snap[1100], bad[1100];
    snprintf(dir, sizeof dir, "%s/henge-pvgsnap.XXXXXX", tmp);
    if (!mkdtemp(dir)) return 1;
    snprintf(snap, sizeof snap, "%s/a.hvsnap", dir);
    snprintf(bad, sizeof bad, "%s/bad.hvsnap", dir);

    if (build(&A) != 0 || build(&B) != 0) {
        printf("cannot build boards\n");
        return 1;
    }
    /* Nothing drawn yet: the screen saves with no GPU side */
    save_file(&A, snap, 1);

    make_textures(&A);
    check("the exec finished", wait_stamp(&A, 3), 1);
    HvPvgMetal *ma = hv_pvg_lite_metal(A.b.pvg);
    if (!ma) {
        printf("no Metal here: skipped\n%d checks, %d failures\n", checks, failures);
        return failures != 0;
    }
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 64; x++) bgra[y][x] = 0xff000000u | (uint32_t)(x * 4) << 16 | (uint32_t)(y * 8) << 8 | (uint32_t)((x ^ y) & 0xff);
    uint8_t r8b[7][16]; /* 16 bytes to a row: only 13 used */
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 16; x++) r8b[y][x] = (uint8_t)(x < 13 ? 17 * x + 3 * y + 1 : 0);
    check("pixels go in", hv_pvg_metal_test_texture(ma, TASK, 1, bgra, 64 * 4, 1), 0);
    check("small pixels go in", hv_pvg_metal_test_texture(ma, TASK, 2, r8b, 16, 1), 0);

    uint8_t back8[7][16];
    /* Out of time: the GPU's work is waited for, and a deadline already gone is refused */
    HvPvgBlob late = {0};
    char why[160] = "";
    uint64_t late_gpu = 0, sunk = 0;
    HvPvgSink late_sink = {count_sink, &sunk, NULL};
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    hv_dev_lock(&A.m);
    int late_rc = hv_pvg_lite_save(A.b.pvg, &late, &late_sink, &late_gpu, NULL, now - 1, why, sizeof why);
    hv_dev_unlock(&A.m);
    check("a save out of time is refused", late_rc, (uint64_t)-1);
    check("and says why", why[0] != 0, 1);
    free(late.p);

    /* No room for what the GPU holds: refused before a byte is written */
    HvPvgBlob full = {0};
    uint64_t full_gpu = 0;
    sunk = 0, why[0] = 0;
    HvPvgSink full_sink = {count_sink, &sunk, full_room};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    hv_dev_lock(&A.m);
    int full_rc = hv_pvg_lite_save(A.b.pvg, &full, &full_sink, &full_gpu, NULL, now + 5000000000ull, why, sizeof why);
    hv_dev_unlock(&A.m);
    check("no room: refused", full_rc, (uint64_t)-1);
    check("no room: asked with what the textures need", asked_room >= sizeof bgra + sizeof r8b, 1);
    check("no room: nothing written", sunk, 0);
    check("no room: says so", strstr(why, "free storage") != NULL, 1);
    free(full.p);

    /* Read ahead, as on leaving the screen: a save after it writes the screen as it was then, without the GPU */
    {
        char ra[1100], live[1100];
        snprintf(ra, sizeof ra, "%s/ahead.hvsnap", dir);
        snprintf(live, sizeof live, "%s/live.hvsnap", dir);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        why[0] = 0;
        check("read ahead: more than the limit is refused", hv_vmapple_snap_capture(&A.b, 64, now + 5000000000ull, why, sizeof why), (uint64_t)-1);
        check("read ahead: says it's memory", strstr(why, "memory") != NULL, 1);
        check("read ahead: nothing kept", A.b.capture == NULL, 1);
        check("read ahead: late is refused", hv_vmapple_snap_capture(&A.b, 1ull << 30, now - 1, why, sizeof why), (uint64_t)-1);
        check("read ahead: late, nothing kept", A.b.capture == NULL, 1);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        check("read ahead", hv_vmapple_snap_capture(&A.b, 1ull << 30, now + 5000000000ull, why, sizeof why), 0);
        struct HvVmappleCapture *first = A.b.capture;
        check("read ahead again: the one taken is kept", hv_vmapple_snap_capture(&A.b, 64, now - 1, why, sizeof why) == 0 &&
                                                         A.b.capture == first, 1);
        static uint32_t other[32][64];
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 64; x++) other[y][x] = 0xff112233u ^ (uint32_t)(x * 77 + y);
        check("read ahead: the pixels change after", hv_pvg_metal_test_texture(ma, TASK, 1, other, 64 * 4, 1) == 0, 1);
        save_file(&A, ra, 1);
        save_file(&A, ra, 1); /* tried again: the read-ahead is kept for it */
        check("read ahead: still kept", A.b.capture != NULL, 1);
        if (build(&H) != 0 || build(&I) != 0) { check("boards H and I", 0, 1); return 1; }
        check("read ahead: loads", load_file(&H, ra), 0);
        hv_vmapple_restored(&H.b);
        HvPvgMetal *mh = hv_pvg_lite_metal(H.b.pvg);
        memset(back_bgra, 0, sizeof back_bgra);
        check("read ahead: the pixels as they were when read", mh && hv_pvg_metal_test_texture(mh, TASK, 1, back_bgra, 64 * 4, 0) == 0 &&
                                                                   !memcmp(back_bgra, bgra, sizeof bgra), 1);
        memset(back8, 0xee, sizeof back8);
        int same = mh && hv_pvg_metal_test_texture(mh, TASK, 2, back8, 16, 0) == 0;
        for (int y = 0; same && y < 7; y++)
            for (int x = 0; x < 13; x++) same &= back8[y][x] == r8b[y][x];
        check("read ahead: the small texture too", same, 1);
        /* Dropped (the Mac went on): the save reads the GPU as it is now */
        hv_vmapple_snap_drop_capture(&A.b);
        check("dropped", A.b.capture == NULL, 1);
        save_file(&A, live, 1);
        check("dropped: loads", load_file(&I, live), 0);
        hv_vmapple_restored(&I.b);
        HvPvgMetal *mi = hv_pvg_lite_metal(I.b.pvg);
        memset(back_bgra, 0, sizeof back_bgra);
        check("dropped: the pixels as they are now", mi && hv_pvg_metal_test_texture(mi, TASK, 1, back_bgra, 64 * 4, 0) == 0 &&
                                                         !memcmp(back_bgra, other, sizeof other), 1);
        check("the pixels go back", hv_pvg_metal_test_texture(ma, TASK, 1, bgra, 64 * 4, 1), 0);
    }

    save_file(&A, snap, 1);

    /* B: a fresh board, loaded */
    check("the snapshot loads", load_file(&B, snap), 0);
    hv_vmapple_restored(&B.b);
    HvPvgMetal *mb = hv_pvg_lite_metal(B.b.pvg);
    check("the GPU side is made at once", mb != NULL, 1);
    memset(back_bgra, 0, sizeof back_bgra);
    memset(back8, 0xee, sizeof back8);
    check("pixels read back", hv_pvg_metal_test_texture(mb, TASK, 1, back_bgra, 64 * 4, 0), 0);
    check("the texture's pixels came back", memcmp(back_bgra, bgra, sizeof bgra), 0);
    check("small pixels read back", hv_pvg_metal_test_texture(mb, TASK, 2, back8, 16, 0), 0);
    int same8 = 1;
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 13; x++) same8 &= back8[y][x] == r8b[y][x];
    check("the padded texture's pixels came back", same8, 1);

    /* B carries on: the guest's next command, on the same ring, gets its stamp */
    check("B's stamps as saved", rd32(&B, STAMPS + 4 * CH), 3);
    uint32_t exec[3 + 2] = {TASK, 1, 0, 1, 0};
    command(&B, 0x43, 4, exec, 5);
    check("B runs the next exec", wait_stamp(&B, 4), 1);
    check("and its texture is the restored one", hv_pvg_metal_test_texture(mb, TASK, 1, back_bgra, 64 * 4, 0) == 0 &&
                                                     !memcmp(back_bgra, bgra, sizeof bgra), 1);

    /* B saved again, and loaded into C, still has them */
    char snap2[1100];
    snprintf(snap2, sizeof snap2, "%s/b.hvsnap", dir);
    save_file(&B, snap2, 1);
    if (build(&C) != 0) return 1;
    check("a second snapshot loads", load_file(&C, snap2), 0);
    hv_vmapple_restored(&C.b);
    HvPvgMetal *mc = hv_pvg_lite_metal(C.b.pvg);
    memset(back_bgra, 0, sizeof back_bgra);
    check("twice saved, pixels the same", mc && hv_pvg_metal_test_texture(mc, TASK, 1, back_bgra, 64 * 4, 0) == 0 &&
                                              !memcmp(back_bgra, bgra, sizeof bgra), 1);

    /* Refusals: into a screen in use; damaged sections. Each into a fresh board. */
    check("into a screen in use: refused", load_file(&A, snap) != 0, 1);
    struct { void (*edit)(HvSnapOut *); const char *what; } bads[] = {
        {cut_short, "a cut-short screen section: refused"},
        {bad_version, "a screen section from another version: refused"},
        {bad_metal_magic, "a damaged GPU part: refused"},
        {gpu_cut_short, "a cut-short GPU part: refused"},
        {keep_gpu, "a screen section without its GPU part: refused"},
    };
    for (size_t i = 0; i < sizeof bads / sizeof *bads; i++) {
        static Board Z;
        memset(&Z, 0, sizeof Z);
        uint32_t which = bads[i].edit == cut_short || bads[i].edit == bad_version ? S_PVG : S_PVGM;
        if (build(&Z) != 0 || rewrite_tag(snap, bad, bads[i].edit, which) != 0) { check(bads[i].what, 0, 1); continue; }
        int quiet = dup(2), null = open("/dev/null", O_WRONLY);
        dup2(null, 2);
        int rc = load_file(&Z, bad);
        dup2(quiet, 2), close(quiet), close(null);
        check(bads[i].what, rc != 0, 1);
        check("and the screen is left unused", hv_pvg_lite_metal(Z.b.pvg) == NULL, 1);
    }

    /* An older snapshot (the GPU's part inside the screen's section) still loads */
    {
        static Board Z;
        memset(&Z, 0, sizeof Z);
        char old[1100];
        snprintf(old, sizeof old, "%s/v1.hvsnap", dir);
        check("an older snapshot is made", build(&Z) == 0 && rewrite_v1(snap, old) == 0, 1);
        check("an older snapshot loads", load_file(&Z, old), 0);
        hv_vmapple_restored(&Z.b);
        HvPvgMetal *mz = hv_pvg_lite_metal(Z.b.pvg);
        memset(back_bgra, 0, sizeof back_bgra);
        check("an older snapshot: pixels the same", mz && hv_pvg_metal_test_texture(mz, TASK, 1, back_bgra, 64 * 4, 0) == 0 &&
                                                        !memcmp(back_bgra, bgra, sizeof bgra), 1);
        unlink(old);
    }

    guest_memory_objects(dir);
    big_texture(dir);
    fuzz_metal(snap);

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
