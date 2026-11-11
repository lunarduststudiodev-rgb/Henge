#ifndef HENGEVM_PVG_VIDEO_H
#define HENGEVM_PVG_VIDEO_H

/* Multi-plane (video) surfaces: the guest's layout of each plane, checked, and its bytes moved between the guest's
   pages and the host's IOSurface. Which side is newer is the exec's to say (pvg_metal.m): what the guest wrote goes
   up, against a shadow of what went up last (never against the IOSurface, which the GPU may have drawn into since),
   and what the GPU drew comes back when the guest asks for it. Plain C, so it can be tested without a GPU. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pvg_validate.h"

/* A plane in the guest's pages: rows of row_bytes (width pixels of bpe bytes), row apart, from offset. */
typedef struct HvVideoPlane {
    uint64_t offset, row, rows, row_bytes, width;
    uint32_t bpe;
} HvVideoPlane;

/* The plane as the guest described it ({offset +8, width +0x20, height +0x28, row +0x30, bytes per element +0x38}),
   1 if all of it lies within alloc bytes. */
static inline int hv_pvg_video_plane(const uint8_t *pl, uint64_t alloc, HvVideoPlane *out) {
    uint64_t off, w, h, row;
    uint16_t bpe;
    memcpy(&off, pl + 8, 8), memcpy(&w, pl + 0x20, 8), memcpy(&h, pl + 0x28, 8), memcpy(&row, pl + 0x30, 8), memcpy(&bpe, pl + 0x38, 2);
    if (!w || !h || !bpe || w > UINT64_MAX / bpe || row < w * bpe || off > alloc || alloc - off < w * bpe) return 0;
    if (h - 1 > (alloc - off - w * bpe) / row) return 0; /* the last row's pixels end within alloc */
    *out = (HvVideoPlane){off, row, h, w * bpe, w, bpe};
    return 1;
}

/* The plane a surface texture's record names ({mapping u64, tag, size, ref, description, plane u16, rotation u16}):
   a u16 just after the description, d + 0x28 for a v2 one (tag 0x39) and d + 0x20 for the others (tags 0xc, 0x2f),
   where Apple's host reads it (ParavirtualizedGraphicsGPUTask's PGResourceManager::createMapperRefTexture reads the
   mapping, then {tag, size}, then 0x28 or 0x30 bytes {ref, description, plane, rotation}, and gives the plane to
   validateIOSurfaceDescriptor and newTextureWithDescriptor:iosurface:plane:). d holds at least 0x2a bytes. */
static inline uint32_t hv_pvg_video_record_plane(uint32_t tag, const uint8_t *d) {
    uint16_t plane;
    memcpy(&plane, d + (tag == 0x39 ? 0x28 : 0x20), 2);
    return plane;
}

/* Which of the surface's planes a view reads: the one the guest's record names (pvg_metal.m's makeMappedTexture: Apple's
   own host reads it as a u16 after the description and gives it to newTextureWithDescriptor:iosurface:plane:), if the
   view fits it: no wider or taller than the plane, with its element bytes, as Metal checks a view of an IOSurface's
   plane (it stops the app on one that doesn't fit). A format with no element bytes of its own (Metal's YCbCr ones)
   covers the whole surface, from plane 0 only. -1 if the plane is not one of the surface's, -2 if the view doesn't
   fit it. Three-plane I420's Cb and Cr, and v0a8's Y and alpha, are the same size: the record tells them apart. */
static inline int hv_pvg_video_view_plane(const HvVideoPlane *planes, uint32_t count, uint32_t plane, uint64_t w, uint64_t h,
                                          uint32_t format) {
    if (plane >= count) return -1;
    uint32_t bpe = hv_pvg_linear_bpp(format);
    if (!bpe) return plane == 0 ? 0 : -2;
    const HvVideoPlane *p = &planes[plane];
    if (!w || !h || w > p->width || h > p->rows || p->bpe != bpe) return -2;
    return (int)plane;
}

/* Tests only: runs between a changed row's read from the guest and its write into dst (test_pvg_video's guest writing
   that row meanwhile, as its CPU may at any time) */
#ifndef HV_PVG_VIDEO_BETWEEN
#define HV_PVG_VIDEO_BETWEEN(y) ((void)0)
#endif

/* The guest's rows of a plane that changed since they last went up (shadow: the guest's bytes as they were then, laid
   out as src), copied into the shadow, and from the shadow into dst. Exact, and only each row's pixels are read. The
   guest's row is read once: its CPU may write it meanwhile, and a second read (one for dst, one for the shadow) could
   give the shadow bytes dst never got, which no later compare would find (the row stale on screen for good). Read once,
   dst holds what the shadow says went up, and a write meanwhile differs from the shadow at the next use. before runs
   once, before the first write (to let the GPU finish with dst). Returns the rows copied. */
static inline uint64_t hv_pvg_video_plane_update(uint8_t *dst, size_t dst_row, uint8_t *shadow, const uint8_t *src, HvVideoPlane p,
                                                 void (*before)(void *), void *ctx) {
    uint64_t copied = 0;
    for (uint64_t y = 0; y < p.rows; y++) {
        const uint8_t *from = src + p.offset + y * p.row;
        uint8_t *was = shadow + p.offset + y * p.row;
        if (!memcmp(was, from, (size_t)p.row_bytes)) continue;
        if (!copied && before) before(ctx);
        memcpy(was, from, (size_t)p.row_bytes);
        HV_PVG_VIDEO_BETWEEN(y);
        memcpy(dst + y * dst_row, was, (size_t)p.row_bytes);
        copied++;
    }
    return copied;
}

/* 0xff in each byte of x that is not 0 (each byte's low 7 bits, plus 0x7f, carry into its top bit only) */
static inline uint64_t hv_pvg_bytes_nonzero(uint64_t x) {
    uint64_t t = ((x & 0x7f7f7f7f7f7f7f7full) + 0x7f7f7f7f7f7f7f7full) | x;
    return ((t & 0x8080808080808080ull) >> 7) * 0xff;
}

/* As hv_pvg_video_plane_update, while dst holds the GPU's drawing that is not yet back in the guest's pages or the
   shadow: of each changed row, only the bytes the guest changed (those that differ from the shadow) go into dst, and
   the GPU's drawing in the rest is kept, as it is in a real GPU's memory when the guest's CPU writes only some bytes of
   a row. A whole row would put the guest's stale bytes over the drawing. Each of the guest's bytes is read once (an
   8-byte word at a time), into both. A byte the guest wrote with the value it already had is not seen (no compare
   can), so the GPU's byte stays, where on a real GPU the guest's would replace it: each row written to is marked in
   merged (a bit a row, set), so hv_pvg_video_plane_rewrite can send it whole once the guest says it wrote the surface.
   Returns the rows written to. */
static inline uint64_t hv_pvg_video_plane_merge(uint8_t *dst, size_t dst_row, uint8_t *shadow, const uint8_t *src, HvVideoPlane p,
                                                uint8_t *merged) {
    uint64_t rows = 0;
    for (uint64_t y = 0; y < p.rows; y++) {
        const uint8_t *from = src + p.offset + y * p.row;
        uint8_t *was = shadow + p.offset + y * p.row, *to = dst + y * dst_row;
        size_t n = (size_t)p.row_bytes, i = 0;
        if (!memcmp(was, from, n)) continue;
        for (; i + 8 <= n; i += 8) {
            uint64_t g, w, d;
            memcpy(&g, from + i, 8), memcpy(&w, was + i, 8);
            if (g == w) continue;
            uint64_t m = hv_pvg_bytes_nonzero(g ^ w);
            memcpy(&d, to + i, 8);
            d = (d & ~m) | (g & m);
            memcpy(was + i, &g, 8), memcpy(to + i, &d, 8);
        }
        for (; i < n; i++) {
            uint8_t g = from[i];
            if (g != was[i]) was[i] = g, to[i] = g;
        }
        merged[y / 8] |= (uint8_t)(1u << (y % 8));
        rows++;
    }
    return rows;
}

/* The rows hv_pvg_video_plane_merge marked in merged, whole from the shadow into dst, and unmarked: once the guest says
   it wrote the surface (an exec's flag 1), all of each such row is its, the bytes it wrote with the values they had
   too, which the merge left as the GPU drew them. Before the merge, a flag 1 sent those rows up whole, the shadow then
   still the guest's older bytes; after it, the guest's bytes are the shadow's, and no compare finds them. Returns the
   rows copied: a marked row dst already holds as the shadow does (all the drawing came back over it, or the guest
   changed it since and it went up whole) is unmarked, not copied. */
static inline uint64_t hv_pvg_video_plane_rewrite(uint8_t *dst, size_t dst_row, const uint8_t *shadow, uint8_t *merged, HvVideoPlane p) {
    uint64_t copied = 0;
    for (uint64_t y = 0; y < p.rows; y++) {
        if (!(merged[y / 8] & (1u << (y % 8)))) continue;
        merged[y / 8] &= (uint8_t)~(1u << (y % 8));
        uint8_t *to = dst + y * dst_row;
        const uint8_t *from = shadow + p.offset + y * p.row;
        if (!memcmp(to, from, (size_t)p.row_bytes)) continue;
        memcpy(to, from, (size_t)p.row_bytes);
        copied++;
    }
    return copied;
}

/* The bytes of a bit a row for each plane, each plane's from a byte of its own (hv_pvg_video_plane_merge's merged) */
static inline uint64_t hv_pvg_video_row_bits(const HvVideoPlane *p, uint32_t planes) {
    uint64_t n = 0;
    for (uint32_t i = 0; i < planes; i++) n += (p[i].rows + 7) / 8;
    return n;
}

/* What the GPU drew into a plane (src, src_row apart), back into the guest's pages and the shadow: only the rows it
   changed, those that differ from the shadow (the bytes guest and host last agreed on), so the work is what the GPU
   drew, and a row the guest's CPU wrote since the exec went in is kept unless the GPU drew that row too. Then the
   GPU's row is the one kept: the guest asked for it back (flag 4), and writing a row while its GPU work runs is the
   guest's own race, as it is on a real GPU sharing the memory. The guest's CPU runs meanwhile: a row it writes between
   the compare and the copy is lost to the GPU's the same way (one row's copy, the race's only window). Returns the
   rows copied. */
static inline uint64_t hv_pvg_video_plane_back(uint8_t *guest, uint8_t *shadow, const uint8_t *src, size_t src_row, HvVideoPlane p) {
    uint64_t copied = 0;
    for (uint64_t y = 0; y < p.rows; y++) {
        const uint8_t *from = src + y * src_row;
        uint8_t *was = shadow + p.offset + y * p.row;
        if (!memcmp(was, from, (size_t)p.row_bytes)) continue;
        memcpy(guest + p.offset + y * p.row, from, (size_t)p.row_bytes);
        memcpy(was, from, (size_t)p.row_bytes);
        copied++;
    }
    return copied;
}

/* The most a video surface's planes may hold (rows times bytes per row, all planes): what the compare and the copy
   back on the GPU's completion cost is bounded by it (about 50 ms at the worst, every row changed, on one core). An 8K
   4:2:0 10-bit frame is 100 MB, 8K 4:4:4 16-bit three planes 200 MB. */
#define HV_PVG_VIDEO_MAX (256ull << 20)

#endif
