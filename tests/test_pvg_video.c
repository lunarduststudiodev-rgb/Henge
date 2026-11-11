/* Video surfaces (pvg_video.h): each plane the guest describes checked against its memory, the plane a view reads
   picked by its size and element bytes (and none when two fit), the rows the guest changed found exactly against a
   shadow of what went up (so what the GPU drew into the surface is never overwritten with stale guest bytes), and
   what the GPU drew written back. And what finding them costs on a 1080p NV12 frame that did not change, against the
   sampled hash it replaced (every 64th 8 bytes, which misses most one-byte changes). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The guest's CPU writing a row while it goes up (pvg_video.h's hook, between its read and its write into dst) */
static void between(unsigned long long y);
#define HV_PVG_VIDEO_BETWEEN(y) between(y)
#include "pvg_video.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* A plane as the guest's descriptor holds it */
static const uint8_t *plane(uint64_t off, uint64_t w, uint64_t h, uint64_t row, uint16_t bpe) {
    static uint8_t pl[0x80];
    memset(pl, 0, sizeof pl);
    memcpy(pl + 8, &off, 8), memcpy(pl + 0x20, &w, 8), memcpy(pl + 0x28, &h, 8), memcpy(pl + 0x30, &row, 8), memcpy(pl + 0x38, &bpe, 2);
    return pl;
}

static void test_plane(void) {
    printf("planes checked against the guest's memory\n");
    HvVideoPlane p;
    CHECK(hv_pvg_video_plane(plane(0, 1920, 1080, 2048, 1), 2048 * 1080, &p) && p.rows == 1080 && p.row_bytes == 1920 && p.row == 2048 &&
          p.width == 1920 && p.bpe == 1, "1080p luma, rows 2048 apart");
    CHECK(hv_pvg_video_plane(plane(2048 * 1080, 960, 540, 2048, 2), 2048 * 1620, &p) && p.row_bytes == 1920 && p.width == 960 && p.bpe == 2,
          "its chroma: 960 elements of 2 bytes");
    /* the last row need only hold its pixels, not a whole row's padding */
    CHECK(hv_pvg_video_plane(plane(0, 1920, 1080, 2048, 1), 2048 * 1079 + 1920, &p), "last row's pixels end at the end");
    CHECK(!hv_pvg_video_plane(plane(0, 1920, 1080, 2048, 1), 2048 * 1079 + 1919, &p), "one byte short");
    CHECK(!hv_pvg_video_plane(plane(1, 1920, 1080, 2048, 1), 2048 * 1079 + 1920, &p), "offset by one: one byte short");
    CHECK(!hv_pvg_video_plane(plane(0, 0, 1080, 2048, 1), 1 << 24, &p), "width 0");
    CHECK(!hv_pvg_video_plane(plane(0, 1920, 0, 2048, 1), 1 << 24, &p), "height 0");
    CHECK(!hv_pvg_video_plane(plane(0, 1920, 1080, 2048, 0), 1 << 24, &p), "0 bytes an element");
    CHECK(!hv_pvg_video_plane(plane(0, 1920, 1080, 1919, 1), 1 << 24, &p), "rows closer than their pixels");
    CHECK(hv_pvg_video_plane(plane(0, 1, 1, (1 << 24) + 1, 1), 1 << 24, &p), "one row: its stride past the memory is never used");
    CHECK(!hv_pvg_video_plane(plane(0, 1, 2, (1 << 24) + 1, 1), 1 << 24, &p), "two rows: the second past the memory");
    CHECK(!hv_pvg_video_plane(plane((1 << 24) + 1, 1, 1, 1, 1), 1 << 24, &p), "offset past the end");
    /* the guest's numbers where a product wraps */
    CHECK(!hv_pvg_video_plane(plane(0, UINT64_MAX / 2 + 1, 1, 0, 2), UINT64_MAX, &p), "width x bytes wraps to 0");
    CHECK(!hv_pvg_video_plane(plane(0, 16, (1ull << 60) + 1, 16, 1), 1ull << 32, &p), "height x row wraps");
    CHECK(!hv_pvg_video_plane(plane(UINT64_MAX - 8, 16, 1, 16, 1), UINT64_MAX, &p), "offset + pixels wraps");
    CHECK(hv_pvg_video_plane(plane(UINT64_MAX - 16, 16, 1, 16, 1), UINT64_MAX, &p), "...but just fits");
}

static void test_view_plane(void) {
    printf("the plane a view reads\n");
    const HvVideoPlane nv12[2] = {{0, 2048, 1080, 1920, 1920, 1}, {2048 * 1080, 2048, 540, 1920, 960, 2}},
                       p010[2] = {{0, 4096, 1080, 3840, 1920, 2}, {4096 * 1080, 4096, 540, 3840, 960, 4}},
                       v0a8[3] = {{0, 64, 64, 64, 64, 1}, {4096, 64, 32, 64, 32, 2}, {8192, 64, 64, 64, 64, 1}}, /* Y, CbCr, alpha */
                       yuv3[3] = {{0, 64, 64, 64, 64, 1}, {4096, 32, 32, 32, 32, 1}, {5120, 32, 32, 32, 32, 1}}; /* Y, Cb, Cr */
    /* the plane the record names, if the view fits it */
    CHECK(hv_pvg_video_view_plane(nv12, 2, 0, 1920, 1080, 10) == 0, "NV12 R8 1920x1080 on plane 0: luma");
    CHECK(hv_pvg_video_view_plane(nv12, 2, 1, 960, 540, 30) == 1, "NV12 RG8 960x540 on plane 1: chroma");
    CHECK(hv_pvg_video_view_plane(nv12, 2, 1, 960, 540, 20) == 1, "NV12 R16 960x540 on plane 1: chroma (2 bytes an element too)");
    CHECK(hv_pvg_video_view_plane(nv12, 2, 1, 960, 540, 60) == -2, "NV12 RG16 on plane 1: element bytes differ");
    CHECK(hv_pvg_video_view_plane(nv12, 2, 0, 1920, 1080, 30) == -2, "NV12 RG8 on plane 0: element bytes differ");
    CHECK(hv_pvg_video_view_plane(nv12, 2, 1, 1920, 1080, 30) == -2, "NV12 RG8 at luma's size on plane 1: too big");
    CHECK(hv_pvg_video_view_plane(nv12, 2, 0, 1921, 1080, 10) == -2, "a pixel wider than the plane: too big");
    CHECK(hv_pvg_video_view_plane(nv12, 2, 0, 1920, 1081, 10) == -2, "a row taller than the plane: too big");
    CHECK(hv_pvg_video_view_plane(nv12, 2, 0, 0, 1080, 10) == -2, "no width");
    CHECK(hv_pvg_video_view_plane(nv12, 2, 0, 960, 540, 10) == 0, "a smaller view of the plane, as Metal allows");
    CHECK(hv_pvg_video_view_plane(nv12, 2, 2, 960, 540, 30) == -1, "plane 2 of two: none");
    CHECK(hv_pvg_video_view_plane(nv12, 2, 0xffff, 960, 540, 30) == -1, "plane 65535: none");
    CHECK(hv_pvg_video_view_plane(p010, 2, 0, 1920, 1080, 20) == 0, "P010 R16: luma");
    CHECK(hv_pvg_video_view_plane(p010, 2, 1, 960, 540, 60) == 1, "P010 RG16: chroma");
    CHECK(hv_pvg_video_view_plane(nv12, 2, 0, 1920, 1080, 500) == 0, "a YCbCr format: the whole surface");
    CHECK(hv_pvg_video_view_plane(nv12, 2, 1, 960, 540, 500) == -2, "a YCbCr format on plane 1: not the whole surface");
    /* the same size and element bytes: the record says which (these were refused as ambiguous before) */
    CHECK(hv_pvg_video_view_plane(v0a8, 3, 0, 64, 64, 10) == 0, "v0a8 R8 64x64 on plane 0: Y");
    CHECK(hv_pvg_video_view_plane(v0a8, 3, 2, 64, 64, 10) == 2, "v0a8 R8 64x64 on plane 2: alpha");
    CHECK(hv_pvg_video_view_plane(v0a8, 3, 1, 32, 32, 30) == 1, "v0a8 RG8 32x32: CbCr");
    CHECK(hv_pvg_video_view_plane(yuv3, 3, 1, 32, 32, 10) == 1, "three-plane R8 32x32 on plane 1: Cb");
    CHECK(hv_pvg_video_view_plane(yuv3, 3, 2, 32, 32, 10) == 2, "three-plane R8 32x32 on plane 2: Cr");
    CHECK(hv_pvg_video_view_plane(yuv3, 3, 0, 64, 64, 10) == 0, "three-plane R8 64x64: Y");
    CHECK(hv_pvg_video_view_plane(yuv3, 3, 1, 64, 64, 10) == -2, "three-plane R8 64x64 on plane 1: too big");
    /* where the record keeps it */
    uint8_t d[0x30] = {0};
    d[0x20] = 2, d[0x28] = 1;
    CHECK(hv_pvg_video_record_plane(0x39, d) == 1, "a v2 record (0x39): the plane at +0x28");
    CHECK(hv_pvg_video_record_plane(0xc, d) == 2 && hv_pvg_video_record_plane(0x2f, d) == 2, "a v1 record (0xc, 0x2f): the plane at +0x20");
    d[0x29] = 1;
    CHECK(hv_pvg_video_record_plane(0x39, d) == 0x101, "a u16");
}

static int befores;
static void before(void *ctx) { (void)ctx; befores++; }

/* The hash the baseline used to tell a changed frame: every 64th 8 bytes of the whole allocation */
static uint64_t sampled(const uint8_t *from, uint64_t alloc) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i + 8 <= alloc; i += 64) {
        uint64_t v;
        memcpy(&v, from + i, 8);
        hash = (hash ^ v) * 0x100000001b3ull;
    }
    return hash;
}

static void test_update(void) {
    printf("changed rows found and copied, against the shadow\n");
    enum { W = 100, H = 20, ROW = 128, DROW = 112, OFF = 0x80, ALLOC = OFF + ROW * H };
    static uint8_t src[ALLOC], shadow[ALLOC], dst[DROW * H];
    HvVideoPlane p = {OFF, ROW, H, W, W, 1};
    memset(src, 0x11, sizeof src), memset(shadow, 0x11, sizeof shadow), memset(dst, 0x11, sizeof dst);
    befores = 0;
    CHECK(hv_pvg_video_plane_update(dst, DROW, shadow, src, p, before, NULL) == 0 && befores == 0, "unchanged: nothing written, no wait");
    src[OFF + ROW * 3 + W - 1] = 1, src[OFF + ROW * 17] = 2, src[OFF + ROW * 17 + 5] = 3;
    CHECK(hv_pvg_video_plane_update(dst, DROW, shadow, src, p, before, NULL) == 2 && befores == 1, "two rows, one wait first: %d", befores);
    CHECK(!memcmp(dst + DROW * 3, src + OFF + ROW * 3, W) && dst[DROW * 17 + 5] == 3, "their pixels there");
    CHECK(!memcmp(shadow + OFF + ROW * 3, src + OFF + ROW * 3, W) && shadow[OFF + ROW * 17 + 5] == 3, "and in the shadow");
    src[OFF + ROW * 4 + W] = 9; /* the guest's padding past the pixels */
    dst[DROW * 4 + W] = 7;      /* and the host's */
    befores = 0;
    CHECK(hv_pvg_video_plane_update(dst, DROW, shadow, src, p, before, NULL) == 0 && befores == 0 && dst[DROW * 4 + W] == 7,
          "padding is neither compared nor written");

    /* The GPU draws into the IOSurface (a decoder's output, a filter): the guest's bytes didn't change, so nothing of the
       guest's goes over what it drew. Compared against the IOSurface (as before the shadow), every row would differ and
       the guest's stale bytes would go up over the GPU's. */
    memset(dst, 0xee, sizeof dst);
    CHECK(hv_pvg_video_plane_update(dst, DROW, shadow, src, p, NULL, NULL) == 0 && dst[0] == 0xee && dst[DROW * H - 1] == 0xee,
          "the GPU's drawing kept");
    /* then back to the guest (it asked): the guest, shadow and IOSurface agree, and nothing goes up again */
    hv_pvg_video_plane_back(src, shadow, dst, DROW, p);
    CHECK(src[OFF] == 0xee && src[OFF + ROW * (H - 1) + W - 1] == 0xee && src[OFF + ROW * 4 + W] == 9, "the GPU's pixels in the guest's rows, padding left");
    int same = 1;
    for (int y = 0; y < H; y++) same &= !memcmp(shadow + OFF + ROW * y, src + OFF + ROW * y, W);
    CHECK(same, "and in the shadow");
    CHECK(hv_pvg_video_plane_update(dst, DROW, shadow, src, p, NULL, NULL) == 0, "nothing to send up after");

    /* a single changed byte anywhere is found; the sampled hash misses most of them */
    int missed = 0, hash_missed = 0;
    for (uint32_t at = 0; at < W * H; at++) {
        uint32_t y = at / W, x = at % W;
        uint64_t h0 = sampled(src, ALLOC);
        src[OFF + ROW * y + x] ^= 0x5a;
        if (sampled(src, ALLOC) == h0) hash_missed++;
        if (hv_pvg_video_plane_update(dst, DROW, shadow, src, p, NULL, NULL) != 1 || dst[DROW * y + x] != src[OFF + ROW * y + x]) missed++;
    }
    CHECK(!missed, "every one-byte change found (%d missed)", missed);
    printf("  of %d one-byte changes the sampled hash would have missed %d\n", W * H, hash_missed);
    CHECK(hash_missed > 0, "the old hash misses changes (why it went): %d", hash_missed);
}

static uint8_t *between_at; /* the byte the guest writes */
static uint8_t between_value;
static unsigned long long between_row;
static void between(unsigned long long y) {
    if (between_at && y == between_row) *between_at = between_value, between_at = NULL;
}

/* (gate S1, 10 Nov 2026) The guest writes a row again while it goes up: the IOSurface gets what the shadow says it got,
   so the write differs from the shadow at the next use and goes up then. Before, the row was read twice (once into
   the IOSurface, once into the shadow): the shadow had the new byte, the IOSurface the old, and no compare ever found
   it again (a stale row on a paused frame). */
static void test_update_written_meanwhile(void) {
    printf("a row the guest writes while it goes up\n");
    enum { W = 64, H = 8, ROW = 64, DROW = 80 };
    static uint8_t src[ROW * H], shadow[ROW * H], dst[DROW * H];
    HvVideoPlane p = {0, ROW, H, W, W, 1};
    memset(src, 0, sizeof src), memset(shadow, 0, sizeof shadow), memset(dst, 0, sizeof dst);
    src[ROW * 5 + 3] = 0x41; /* the guest writes row 5 */
    between_at = &src[ROW * 5 + 3], between_value = 0x42, between_row = 5; /* and again while it goes up */
    CHECK(hv_pvg_video_plane_update(dst, DROW, shadow, src, p, NULL, NULL) == 1 && !between_at, "row 5 up, written meanwhile");
    int same = 1;
    for (int y = 0; y < H; y++) same &= !memcmp(dst + DROW * y, shadow + ROW * y, W);
    CHECK(same, "the IOSurface holds what the shadow says went up (row 5: %#x, shadow %#x)", dst[DROW * 5 + 3], shadow[ROW * 5 + 3]);
    CHECK(hv_pvg_video_plane_update(dst, DROW, shadow, src, p, NULL, NULL) == 1 && dst[DROW * 5 + 3] == 0x42,
          "the write meanwhile goes up at the next use (%#x)", dst[DROW * 5 + 3]);
    CHECK(hv_pvg_video_plane_update(dst, DROW, shadow, src, p, NULL, NULL) == 0, "then nothing");
}

/* (gate S3, 10 Nov 2026) While the GPU's drawing is not back in the guest's pages, only the bytes the guest changed go
   over it: the rest of the row is the guest's stale copy */
static void test_merge(void) {
    printf("the guest's bytes onto the GPU's drawing\n");
    int nonzero_ok = 1;
    for (int b = 0; b < 8; b++)
        for (int v = 1; v < 256; v++) nonzero_ok &= hv_pvg_bytes_nonzero((uint64_t)v << (8 * b)) == 0xffull << (8 * b);
    CHECK(nonzero_ok && hv_pvg_bytes_nonzero(0) == 0 && hv_pvg_bytes_nonzero(0x0100800001ff0000ull) == 0xff00ff00ffff0000ull,
          "each byte that is not 0, and only those");
    enum { W = 61, H = 6, ROW = 64, DROW = 72 }; /* 61: seven words and a tail of 5 */
    static uint8_t src[ROW * H], shadow[ROW * H], dst[DROW * H], merged[1];
    HvVideoPlane p = {0, ROW, H, W, W, 1};
    memset(src, 0, sizeof src), memset(shadow, 0, sizeof shadow), memset(dst, 0x55, sizeof dst); /* the GPU drew it all */
    CHECK(hv_pvg_video_plane_merge(dst, DROW, shadow, src, p, merged) == 0 && dst[0] == 0x55, "the guest wrote nothing: the drawing kept");
    src[ROW * 2 + 3] = 0x99, src[ROW * 2 + 58] = 0x98, src[ROW * 4] = 0x97; /* a byte in a word, one in the tail, another row */
    CHECK(hv_pvg_video_plane_merge(dst, DROW, shadow, src, p, merged) == 2, "two rows written to");
    int kept = 1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t want = (y == 2 && x == 3) ? 0x99 : (y == 2 && x == 58) ? 0x98 : (y == 4 && x == 0) ? 0x97 : 0x55;
            kept &= dst[DROW * y + x] == want;
        }
    CHECK(kept, "the guest's three bytes there, the GPU's drawing in all the rest (row 2: %#x %#x %#x)", dst[DROW * 2 + 2], dst[DROW * 2 + 3], dst[DROW * 2 + 58]);
    CHECK(!memcmp(shadow, src, sizeof src), "the shadow has the guest's bytes");
    CHECK(hv_pvg_video_plane_merge(dst, DROW, shadow, src, p, merged) == 0, "then nothing");
    CHECK(merged[0] == (1 << 2 | 1 << 4), "rows 2 and 4 marked (%#x)", merged[0]);
}

/* (video gate review, 10 Nov 2026) A byte the guest writes with the value it had is not seen by the merge, so the GPU's
   byte stays there. Once the guest says it wrote the surface (flag 1), each row the merge wrote to goes whole from the
   shadow: the guest's row, as a flag 1 sent it before the merge */
static void test_rewrite(void) {
    printf("the rows merged onto the GPU's drawing, whole once the guest says it wrote them\n");
    enum { W = 20, H = 12, ROW = 32, DROW = 40 };
    static uint8_t src[ROW * H], shadow[ROW * H], dst[DROW * H], merged[2];
    HvVideoPlane p = {0, ROW, H, W, W, 1};
    memset(src, 0x25, sizeof src), memset(shadow, 0x25, sizeof shadow), memset(dst, 0x60, sizeof dst); /* the GPU drew it all */
    src[ROW * 9 + 1] = 0x31; /* the guest writes row 9 (byte 5 again as 0x25, unseen) */
    CHECK(hv_pvg_video_plane_merge(dst, DROW, shadow, src, p, merged) == 1 && dst[DROW * 9 + 1] == 0x31 && dst[DROW * 9 + 5] == 0x60,
          "merged: its changed byte only, the GPU's in the rest (%#x)", dst[DROW * 9 + 5]);
    CHECK(merged[0] == 0 && merged[1] == 1 << 1, "row 9 marked, in the second byte");
    CHECK(hv_pvg_video_plane_update(dst, DROW, shadow, src, p, NULL, NULL) == 0, "a flag 1's compare finds nothing changed");
    CHECK(hv_pvg_video_plane_rewrite(dst, DROW, shadow, merged, p) == 1 && !memcmp(dst + DROW * 9, src + ROW * 9, W),
          "the marked row whole: the guest's (%#x)", dst[DROW * 9 + 5]);
    CHECK(dst[DROW * 8] == 0x60 && merged[1] == 0, "no other row; unmarked");
    CHECK(hv_pvg_video_plane_rewrite(dst, DROW, shadow, merged, p) == 0, "then nothing");
    merged[0] = 1 << 3; /* row 3, marked, already as the shadow (all the drawing came back over it) */
    memcpy(dst + DROW * 3, shadow + ROW * 3, W);
    CHECK(hv_pvg_video_plane_rewrite(dst, DROW, shadow, merged, p) == 0 && merged[0] == 0, "a marked row already the shadow's: unmarked, not copied");
    HvVideoPlane two[2] = {{0, ROW, 8, W, W, 1}, {0, ROW, 9, W, W, 1}};
    CHECK(hv_pvg_video_row_bits(two, 2) == 1 + 2, "each plane's bits from a byte of its own");
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void test_cost(void) {
    printf("cost on an unchanged 1080p NV12 frame\n");
    const uint64_t row = 2048, luma = row * 1080, alloc = luma + row * 540;
    uint8_t *guest = malloc(alloc), *shadow = calloc(1, alloc), *y = malloc(1920 * 1080), *c = malloc(1920 * 540);
    for (uint64_t i = 0; i < alloc; i++) guest[i] = (uint8_t)(i * 2654435761u >> 13);
    HvVideoPlane py = {0, row, 1080, 1920, 1920, 1}, pc = {luma, row, 540, 1920, 960, 2};
    hv_pvg_video_plane_update(y, 1920, shadow, guest, py, NULL, NULL), hv_pvg_video_plane_update(c, 1920, shadow, guest, pc, NULL, NULL);
    enum { N = 200 };
    volatile uint64_t sink = 0;
    double t0 = now();
    for (int i = 0; i < N; i++) sink += hv_pvg_video_plane_update(y, 1920, shadow, guest, py, NULL, NULL) + hv_pvg_video_plane_update(c, 1920, shadow, guest, pc, NULL, NULL);
    double exact = (now() - t0) / N;
    t0 = now();
    for (int i = 0; i < N; i++) sink += sampled(guest, alloc);
    double hash = (now() - t0) / N;
    printf("  exact compare %.3f ms a frame, sampled hash %.3f ms (%.1fx)\n", exact * 1e3, hash * 1e3, exact / hash);
    CHECK(sink != 1 && exact < 4e-3, "under 4 ms a frame (a 60 Hz frame is 16.7 ms): %.3f ms", exact * 1e3);
    free(guest), free(shadow), free(y), free(c);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_plane();
    test_view_plane();
    test_update();
    test_update_written_meanwhile();
    test_merge();
    test_rewrite();
    test_cost();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
