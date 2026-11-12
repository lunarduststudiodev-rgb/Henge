#ifndef HENGEVM_PVG_COMMAND_H
#define HENGEVM_PVG_COMMAND_H

/* The guest's encoder commands, checked before Metal sees them. As with pvg_validate.h's textures, each rule is one
   of Metal's own assertions, measured (macOS 27, Apple GPU, with and without MTL_DEBUG_LAYER): a slot past an
   encoder's table, an enum past Metal's, a render pass or a copy Metal can't do. Some stop the whole app, the rest
   write where nothing was meant to be. A command that breaks one is told and dropped (pvg_metal.m), never sent on.
   Plain C, so the rules can be tested without a GPU (test_pvg_command). */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "pvg_validate.h"

/* A field of a record's body, zero past its end (as rec_at pads them). */
static inline uint64_t hv_pvg_field(const uint8_t *b, uint32_t size, uint32_t at, uint32_t width) {
    uint64_t v = 0;
    if (at < size) memcpy(&v, b + at, size - at < width ? size - at : width);
    return v;
}

/* ---- Slots: buffers, textures, samplers, viewports and scissors an encoder has, and threadgroup memory. */

enum { HV_SLOT_BUFFER, HV_SLOT_TEXTURE, HV_SLOT_SAMPLER, HV_SLOT_VIEWPORT, HV_SLOT_SCISSOR, HV_SLOT_THREADGROUP, HV_SLOTS };

typedef struct HvCmdLimits {
    uint32_t slots[HV_SLOTS];
    uint64_t threadgroup_bytes; /* maxThreadgroupMemoryLength */
} HvCmdLimits;

/* Textures: 128 on the GPUs that have argument buffers tier 2 (Apple6 and later, every Mac), 96 before (measured: 127 is
   taken, 128 stops the app). */
static inline HvCmdLimits hv_pvg_cmd_limits(uint32_t textures, uint64_t threadgroup_bytes) {
    return (HvCmdLimits){{31, textures, 16, 16, 16, 31}, threadgroup_bytes};
}

/* count slots from index, all of them within limit: index + count is never added, so it can't wrap */
static inline int hv_pvg_slots_ok(uint64_t index, uint64_t count, uint64_t limit) {
    return index < limit && count <= limit - index;
}

/* How a command names its slots: {index, count} then count elements stride apart from head; {index} alone, the record
   head bytes long; or as many elements as fill the record (after a header of size % stride bytes). */
enum { HV_RANGE, HV_ONE, HV_LIST };
typedef struct HvRangeRule {
    uint16_t tag;
    uint8_t form, slot, index_at, head, stride;
} HvRangeRule;

static const HvRangeRule hv_pvg_range_rules[] = {
    {125, HV_RANGE, HV_SLOT_BUFFER, 0, 8, 12},  /* vertex buffers: {ref, offset 64} */
    {110, HV_RANGE, HV_SLOT_BUFFER, 0, 8, 12},  /* fragment */
    {157, HV_RANGE, HV_SLOT_BUFFER, 0, 8, 12},  /* tile */
    {165, HV_RANGE, HV_SLOT_BUFFER, 0, 8, 20},  /* vertex, with strides */
    {126, HV_ONE, HV_SLOT_BUFFER, 0, 12, 0},    /* a vertex buffer's offset */
    {111, HV_ONE, HV_SLOT_BUFFER, 0, 12, 0},    /* a fragment buffer's */
    {166, HV_ONE, HV_SLOT_BUFFER, 0, 20, 0},    /* a vertex buffer's offset and stride */
    {114, HV_RANGE, HV_SLOT_TEXTURE, 0, 8, 4},  /* fragment textures */
    {161, HV_RANGE, HV_SLOT_TEXTURE, 0, 8, 4},  /* tile */
    {129, HV_RANGE, HV_SLOT_TEXTURE, 0, 8, 4},  /* vertex */
    {112, HV_RANGE, HV_SLOT_SAMPLER, 0, 8, 4},  /* fragment samplers */
    {127, HV_RANGE, HV_SLOT_SAMPLER, 0, 8, 4},  /* vertex */
    {113, HV_RANGE, HV_SLOT_SAMPLER, 0, 8, 12}, /* fragment, with LOD clamps */
    {128, HV_RANGE, HV_SLOT_SAMPLER, 0, 8, 12}, /* vertex, with LOD clamps */
    {118, HV_LIST, HV_SLOT_SCISSOR, 0, 0, 32},  /* MTLScissorRect */
    {131, HV_LIST, HV_SLOT_VIEWPORT, 0, 0, 48}, /* MTLViewport */
    {203, HV_RANGE, HV_SLOT_BUFFER, 0, 8, 12},  /* compute buffers */
    {207, HV_ONE, HV_SLOT_BUFFER, 0, 12, 0},    /* a compute buffer's offset */
    {206, HV_RANGE, HV_SLOT_TEXTURE, 0, 8, 4},  /* compute textures */
    {204, HV_RANGE, HV_SLOT_SAMPLER, 0, 8, 4},  /* compute samplers */
    {211, HV_ONE, HV_SLOT_THREADGROUP, 8, 12, 0}, /* threadgroup memory: {length 64, index} */
};

/* An enum or a set of flags in a record: the field (width bytes at at) is at least min and at most max, with no bit
   outside mask. */
typedef struct HvEnumRule {
    uint16_t tag;
    uint8_t at, width;
    uint32_t min, max, mask;
} HvEnumRule;

#define HV_ENUM(tag, at, width, max) {tag, at, width, 0, max, 0xffffffffu}
#define HV_FLAGS(tag, at, width, mask) {tag, at, width, 0, 0xffffffffu, mask}
#define HV_STAGES(tag, at, width, mask) {tag, at, width, 1, 0xffffffffu, mask} /* at least one stage */
static const HvEnumRule hv_pvg_enum_rules[] = {
    HV_ENUM(107, 0, 4, 2), /* cull: none, front, back */
    HV_ENUM(109, 0, 4, 1), /* depth clip: clip, clamp */
    HV_ENUM(115, 0, 4, 1), /* winding */
    HV_ENUM(124, 0, 4, 1), /* triangle fill: fill, lines */
    HV_STAGES(24, 4, 2, 0x1f), HV_STAGES(25, 4, 2, 0x1f), HV_STAGES(27, 4, 2, 0x1f), /* render stages */
    HV_FLAGS(135, 4, 4, 7), HV_FLAGS(137, 4, 2, 7), HV_STAGES(137, 6, 2, 0x1f),      /* resource usage, stages */
    /* draws: primitive (point ... triangle strip) and index type (16 or 32 bits) */
    HV_ENUM(0, 0, 4, 4), HV_ENUM(1, 0, 4, 4), HV_ENUM(2, 24, 2, 4), HV_ENUM(3, 6, 2, 4), HV_ENUM(4, 32, 2, 4), HV_ENUM(5, 8, 2, 4),
    HV_ENUM(16, 4, 2, 4),
    HV_ENUM(6, 0, 2, 4), HV_ENUM(6, 2, 2, 1), HV_ENUM(7, 0, 2, 4), HV_ENUM(7, 2, 2, 1), HV_ENUM(8, 0, 2, 4), HV_ENUM(8, 2, 2, 1),
    HV_ENUM(9, 0, 2, 4), HV_ENUM(9, 2, 2, 1), HV_ENUM(10, 0, 2, 4), HV_ENUM(10, 2, 2, 1), HV_ENUM(11, 0, 2, 4), HV_ENUM(11, 2, 2, 1),
    HV_ENUM(17, 0, 2, 4), HV_ENUM(17, 2, 2, 1), HV_ENUM(28, 0, 2, 4), HV_ENUM(28, 2, 2, 1), HV_ENUM(29, 0, 2, 4), HV_ENUM(29, 2, 2, 1),
    /* patches: control points, at most 32 */
    HV_ENUM(12, 44, 2, 32), HV_ENUM(13, 14, 2, 32), HV_ENUM(14, 56, 2, 32), HV_ENUM(15, 20, 2, 32), HV_ENUM(18, 24, 2, 32),
    HV_ENUM(19, 36, 2, 32),
};
#undef HV_ENUM
#undef HV_FLAGS
#undef HV_STAGES

/* A draw's vertex, instance and patch ids are 32 bits: its first one plus how many must fit in them, or the ids wrap
   (the GPU then works through ids it was never given; Metal's own check calls it a vertex_id too small). {start, count}
   as {offset, width}, width 0 for a start of 0. An instance count must not be 0 (Metal leaves that undefined). */
typedef struct HvSpanRule {
    uint16_t tag;
    uint8_t start_at, start_width, count_at, count_width, nonzero;
} HvSpanRule;

static const HvSpanRule hv_pvg_span_rules[] = {
    {0, 4, 8, 12, 8, 0},                        /* vertices */
    {2, 0, 8, 8, 8, 0}, {2, 0, 0, 16, 8, 1},    /* vertices, instances */
    {4, 0, 8, 8, 8, 0}, {4, 24, 8, 16, 8, 1},   /* vertices, instances from a base */
    {6, 0, 0, 8, 8, 0}, {8, 0, 0, 8, 8, 0}, {8, 0, 0, 24, 8, 1}, /* indexes, instances */
    {10, 0, 0, 16, 8, 0}, {10, 40, 8, 24, 8, 1}, {28, 0, 0, 16, 8, 0}, {28, 40, 8, 24, 8, 1},
    {12, 0, 8, 8, 8, 0}, {12, 36, 8, 28, 8, 1}, /* patches, instances */
    {14, 0, 8, 8, 8, 0}, {14, 48, 8, 40, 8, 1},
    /* the 16-bit forms always fit: their instance counts */
    {3, 0, 0, 4, 2, 1}, {5, 6, 2, 4, 2, 1}, {9, 0, 0, 12, 2, 1}, {11, 16, 2, 12, 2, 1}, {29, 16, 2, 12, 2, 1},
    {13, 12, 2, 10, 2, 1}, {15, 18, 2, 16, 2, 1},
};

/* How much work one draw may ask of the GPU: its vertices (or indexes, or patches) times its instances. Not Metal's
   limit but the host's: the GPU runs one command buffer at a time, and one that runs for seconds stalls everything the
   host draws, then is killed by the GPU's watchdog (a guest's draw of 15 points 3.9 billion times did that). 2^30 is
   well past any real draw. {count, instances} as {offset, width}, instances' width 0 for none. */
#define HV_PVG_MAX_WORK (1ull << 30)
typedef struct HvWorkRule {
    uint16_t tag;
    uint8_t count_at, count_width, inst_at, inst_width;
} HvWorkRule;

static const HvWorkRule hv_pvg_work_rules[] = {
    {0, 12, 8, 0, 0}, {2, 8, 8, 16, 8}, {3, 2, 2, 4, 2}, {4, 8, 8, 16, 8}, {5, 2, 2, 4, 2},    /* vertices */
    {6, 8, 8, 0, 0}, {7, 8, 2, 0, 0}, {8, 8, 8, 24, 8}, {9, 8, 2, 12, 2}, {10, 16, 8, 24, 8}, /* indexes */
    {28, 16, 8, 24, 8}, {11, 10, 2, 12, 2}, {29, 10, 2, 12, 2},
    {12, 8, 8, 28, 8}, {13, 2, 2, 10, 2}, {14, 8, 8, 40, 8}, {15, 2, 2, 16, 2},                /* patches */
    {1, 6, 2, 0, 0},                                                                           /* 16 bits: for the budget */
};

/* ---- One exec's work, all told. One command within HV_PVG_MAX_WORK can still be repeated: a guest's exec of 356
   draws of 2^30 vertices each (or as many dispatches of 2^30 threads) is 3.8e11, minutes of GPU time. So an exec's
   commands add up, in units of a vertex (index, patch) times an instance, a compute thread, a pixel of a pass or a
   copy, or 16 bytes copied or filled; a patch draw's times its pipeline's largest tessellation factor squared (the
   vertices a patch can become, at most 64 x 64). Past HV_PVG_EXEC_WORK the rest of the exec is dropped (told once),
   and the exec still completes. Indirect draws, whose counts are on the GPU, have a budget of their own of the same
   size, kept on the GPU by the clamp below; so an exec asks at most 2 x HV_PVG_EXEC_WORK.
   Why 2^35 (3.4e10): a frame of WindowServer, Safari or a game is 1e6 to 1e8 such units, in a few execs; the heaviest
   one-buffer compute (a diffusion model's step, a large matrix product per layer of a language model) about 1e10.
   2^35 is above all of them.
   What it bounds is work as counted here, not time. A unit is not a fixed cost: a pixel of a pass is counted once,
   but each draw over it shades it again, with a fragment shader of the guest's choosing, and a shader that loops
   makes any one unit as slow as it likes. So no budget of counts can bound an exec's GPU time, and this one does not
   try (a charge per draw for the pixels it may cover would refuse real frames long before it stopped a guest). The
   GPU's time is bounded by three other things: Metal's GPU watchdog ends a command buffer that runs too long (it
   fails with Timeout); the breaker (pvg_metal.m, gpuFailed:) trips at once on a Timeout, and from then on none of
   the guest's work is committed until the app re-arms it (hv_pvg_metal_gpu_rearm; Suspend and Resume do too); and
   at most HV_PVG_GUEST_INFLIGHT (4) of the guest's command buffers are committed and not done at any time. So a
   guest gets at most about 4 watchdog periods of the GPU each time the breaker is armed. */
#define HV_PVG_EXEC_WORK (1ull << 35)

static inline uint64_t hv_pvg_sat_mul(uint64_t a, uint64_t b) {
    uint64_t r;
    return __builtin_mul_overflow(a, b, &r) ? UINT64_MAX : r;
}

/* What a command asks, in the units above, from its own fields (0 for one that asks nothing of its own). A patch
   draw's weight, a pass's and mipmaps' pixels, and indirect draws are the caller's. */
static inline uint64_t hv_pvg_command_work(uint32_t tag, const uint8_t *b, uint32_t size) {
    for (size_t i = 0; i < sizeof hv_pvg_work_rules / sizeof *hv_pvg_work_rules; i++) {
        const HvWorkRule *w = &hv_pvg_work_rules[i];
        if (w->tag != tag) continue;
        uint64_t count = hv_pvg_field(b, size, w->count_at, w->count_width);
        return hv_pvg_sat_mul(count, w->inst_width ? hv_pvg_field(b, size, w->inst_at, w->inst_width) : 1);
    }
    uint64_t n = 1;
    switch (tag) {
    case 200: case 202: /* {threadgroups or threads, 3 x 64; threads per group, 3 x 64} */
        for (uint32_t i = 0; i < (tag == 200 ? 6u : 3u); i++) n = hv_pvg_sat_mul(n, hv_pvg_field(b, size, i * 8, 8));
        return n;
    case 300: case 302: case 303: /* a region's size, 3 x 64 at 0x20 */
        for (uint32_t i = 0; i < 3; i++) n = hv_pvg_sat_mul(n, hv_pvg_field(b, size, 0x20 + i * 8, 8));
        return n;
    case 301: return hv_pvg_field(b, size, 0x18, 8) / 16 + 1; /* bytes */
    case 306: return hv_pvg_field(b, size, 12, 8) / 16 + 1;
    default: return 0;
    }
}

/* ---- Indirect draws (render 16 to 19): their counts are in a buffer, written by the GPU as often as by the guest,
   so they are only known on the GPU. As WebGPU's implementations do, each pass's indirect arguments are first copied
   into a buffer of the host's and clamped there by a compute dispatch before the pass, and the draws read the clamped
   copy: a draw's count times instances (times its weight) within HV_PVG_MAX_WORK, its ids within 32 bits, an indexed
   draw's indexes within its index buffer, a patch draw's patches within its patch and control point index buffers
   and its patches and instances within its tessellation factors (hv_pvg_tess_fit), and all of an exec's within its budget (HV_PVG_EXEC_WORK, in left). One thread does a pass's, in order.
   args: HV_PVG_CLAMP_STRIDE bytes a draw, the arguments as Metal reads them (MTLDrawPrimitivesIndirectArguments,
   MTLDrawIndexedPrimitivesIndirectArguments: {count, instances, start, base vertex, base instance};
   MTLDrawPatchIndirectArguments). jobs: what each is. The same in C (hv_pvg_clamp) for the tests. */
#define HV_PVG_CLAMP_STRIDE 32
enum { HV_CLAMP_DRAW = 0, HV_CLAMP_INDEXED = 1, HV_CLAMP_PATCHES = 2, HV_CLAMP_NONE = 3 };
typedef struct HvClampJob {
    uint32_t kind, weight, limit; /* limit: the ids (indexes, patches) there are from 0; 0xffffffff for any */
    uint32_t frec; /* patches: their tessellation factors, as hv_pvg_tess_fit takes them (frec 0: none to fit) */
    uint64_t froom, fstride;
    uint32_t indexed, step;
} HvClampJob;

/* A patch draw's tessellation factors (render 12 to 15, 18 and 19). Metal reads rec bytes of them (8, an
   MTLTriangleTessellationFactorsHalf, or 12, an MTLQuadTessellationFactorsHalf) from the factor buffer's offset (room
   bytes there), as the pipeline's step function (MTLTessellationFactorStepFunction) says: one record for all (0,
   Constant), one for each patch id, at id x rec (1, PerPatch), one for each instance, stride bytes on from the one
   before (2, PerInstance), or both (3). Instance ids count from the base instance, as Metal's do. count and inst are
   cut to the patches and instances whose factors are all within. Metal stops the app (its debug layer) on a stride
   with Constant or PerPatch, on none with the other two, and with both on patches (start + count) whose records are
   more than the stride: nothing drawn, or the patches cut to the stride. Patch ids read from a patch index buffer
   (indexed) are the guest's, any 32-bit value (Metal's patch index buffers are always uint): with factors for each patch
   (1 or 3) no factor buffer covers them all (2^32 records), so none are drawn; with one record for all or for each
   instance only (0 or 2) the ids read none. */
static inline void hv_pvg_tess_fit(uint64_t rec, uint64_t room, uint64_t stride, uint32_t step, int indexed, uint64_t start,
                                   uint64_t base, uint64_t *count, uint64_t *inst) {
    uint64_t recs = rec ? room / rec : 0;
    int per_patch = step == 1 || step == 3, per_inst = step >= 2;
    if (!recs || per_inst != (stride != 0) || (per_patch && indexed)) *count = 0;
    else {
        uint64_t most = per_patch ? recs : UINT64_MAX; /* patch ids, from 0 */
        if (step == 3 && stride / rec < most) most = stride / rec;
        if (most != UINT64_MAX) *count = start >= most ? 0 : *count < most - start ? *count : most - start;
    }
    if (!*count || !per_inst) return;
    uint64_t last = (room - (per_patch ? start + *count : 1) * rec) / stride; /* the last instance id with room */
    *inst = base > last ? 0 : *inst < last - base + 1 ? *inst : last - base + 1;
}

/* A direct patch draw's index buffers (render 12 to 15) hold the indexes it reads. Metal takes patch i (start to
   start + count) as id i, or as the id at place i of the patch index buffer when there is one (32-bit, room bytes from
   its offset), and reads cps control point indexes for it at id x cps of the control point index buffer, when there is
   one: point_size bytes each (the pipeline's tessellationControlPointIndexType: 2 or 4; 4, the fewest, if it says
   neither). So the patch index buffer is bound by place; the control point index buffer by place only with no patch
   index buffer, ids and places then being one. The ids the guest wrote in a patch index buffer are read on the GPU,
   where no check here sees them, so they bound nothing here (as the indirect forms' clamp, 18 and 19: HvClampJob's
   limit); but whatever the id, a patch reads cps indexes, so the buffer holds at least that many. NULL when they all
   fit, else what does not. */
static inline const char *hv_pvg_patch_ids_problem(uint64_t start, uint64_t count, uint64_t cps, int patches, uint64_t patch_room,
                                                   int points, uint64_t point_room, uint64_t point_size) {
    if (!count) return NULL;
    uint64_t n = patch_room / 4;
    if (patches && (count > n || start > n - count)) return "patches past the end of their index buffer";
    if (!points) return NULL;
    if (!cps) return "no control points";
    n = point_room / (point_size == 2 ? 2 : 4) / cps;
    if (patches) return n ? NULL : "fewer control point indexes than one patch reads";
    if (count > n || start > n - count) return "patches past the end of the control point index buffer";
    return NULL;
}

#define HV_PVG_CLAMP_MSL \
    "#include <metal_stdlib>\n" \
    "using namespace metal;\n" \
    "struct Job { uint kind, weight, limit, frec; ulong froom, fstride; uint indexed, step; };\n" \
    "kernel void hv_clamp(device uint *args [[buffer(0)]], constant Job *jobs [[buffer(1)]], constant uint &n [[buffer(2)]],\n" \
    "                     device ulong *left [[buffer(3)]], uint3 at [[thread_position_in_grid]]) {\n" \
    "    if (at.x != 0 || at.y != 0 || at.z != 0) return;\n" \
    "    const ulong most = 1ul << 30, ids = 0xfffffffful;\n" \
    "    ulong budget = *left;\n" \
    "    for (uint i = 0; i < n; i++) {\n" \
    "        device uint *a = args + i * 8;\n" \
    "        Job j = jobs[i];\n" \
    "        if (j.kind > 2) { a[0] = 0, a[1] = 0, a[2] = 0, a[3] = 0, a[4] = 0; continue; }\n" \
    "        ulong count = a[0], inst = a[1], start = a[2], base = j.kind == 1 ? a[4] : a[3], w = max(j.weight, 1u);\n" \
    "        if (j.kind != 1) count = min(count, ids - start);\n" \
    "        if (j.limit != 0xffffffffu) count = start >= j.limit ? 0 : min(count, ulong(j.limit) - start);\n" \
    "        inst = min(inst, ids - base);\n" \
    "        if (j.frec && count) {\n" \
    "            ulong recs = j.froom / j.frec;\n" \
    "            bool per_patch = j.step == 1 || j.step == 3, per_inst = j.step >= 2;\n" \
    "            if (!recs || per_inst != (j.fstride != 0) || (per_patch && j.indexed != 0)) count = 0;\n" \
    "            else {\n" \
    "                ulong most = per_patch ? recs : 0xfffffffffffffffful;\n" \
    "                if (j.step == 3) most = min(most, j.fstride / j.frec);\n" \
    "                if (most != 0xfffffffffffffffful) count = start >= most ? 0 : min(count, most - start);\n" \
    "            }\n" \
    "            if (count && per_inst) {\n" \
    "                ulong last = (j.froom - (per_patch ? start + count : 1ul) * j.frec) / j.fstride;\n" \
    "                inst = base > last ? 0 : min(inst, last - base + 1);\n" \
    "            }\n" \
    "        }\n" \
    "        if (count > most / w) count = most / w;\n" \
    "        ulong per = count * w;\n" \
    "        if (per && inst > most / per) inst = most / per;\n" \
    "        ulong work = per * inst;\n" \
    "        if (work > budget) count = 0, inst = 0, work = 0;\n" \
    "        budget -= work;\n" \
    "        a[0] = uint(count), a[1] = uint(inst);\n" \
    "    }\n" \
    "    *left = budget;\n" \
    "}\n"

/* The kernel above, in C: args n x 8 words, as the kernel leaves them */
static inline void hv_pvg_clamp(uint32_t *args, const HvClampJob *jobs, uint32_t n, uint64_t *left) {
    const uint64_t most = HV_PVG_MAX_WORK, ids = 0xffffffffull;
    uint64_t budget = *left;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t *a = args + i * 8;
        HvClampJob j = jobs[i];
        if (j.kind > 2) { a[0] = a[1] = a[2] = a[3] = a[4] = 0; continue; }
        uint64_t count = a[0], inst = a[1], start = a[2], base = j.kind == 1 ? a[4] : a[3], w = j.weight > 1 ? j.weight : 1;
        if (j.kind != 1 && count > ids - start) count = ids - start;
        if (j.limit != 0xffffffffu) count = start >= j.limit ? 0 : count < j.limit - start ? count : j.limit - start;
        if (inst > ids - base) inst = ids - base;
        if (j.frec) hv_pvg_tess_fit(j.frec, j.froom, j.fstride, j.step, (int)j.indexed, start, base, &count, &inst);
        if (count > most / w) count = most / w;
        uint64_t per = count * w;
        if (per && inst > most / per) inst = most / per;
        uint64_t work = per * inst;
        if (work > budget) count = inst = work = 0;
        budget -= work;
        a[0] = (uint32_t)count, a[1] = (uint32_t)inst;
    }
    *left = budget;
}

/* A compute grid: threadgroups (or threads, for dispatchThreads) times threads per group. Each thread's position is
   32 bits a side, and the whole is within the same work as a draw. NULL if it may run. */
static inline const char *hv_pvg_grid_problem(int threads, const uint64_t grid[3], const uint64_t group[3], uint64_t max_group) {
    uint64_t per = 1, all = 1;
    for (int i = 0; i < 3; i++) {
        if (!grid[i] || !group[i]) return "an empty grid or group";
        if (__builtin_mul_overflow(per, group[i], &per)) return "a group past the pipeline's";
        uint64_t side = grid[i];
        if (!threads && __builtin_mul_overflow(grid[i], group[i], &side)) return "more threads than 32 bits hold";
        if (side > 0xffffffffull) return "more threads than 32 bits hold";
        if (__builtin_mul_overflow(all, side, &all) || all > HV_PVG_MAX_WORK) return "more work than one command may ask";
    }
    if (per > max_group) return "a group past the pipeline's";
    return 0;
}

/* NULL if the command's slots and enums are ones Metal has, else what is wrong. b is its body, size bytes. */
static inline const char *hv_pvg_command_problem(uint32_t tag, const uint8_t *b, uint32_t size, const HvCmdLimits *l) {
    for (size_t i = 0; i < sizeof hv_pvg_range_rules / sizeof *hv_pvg_range_rules; i++) {
        const HvRangeRule *r = &hv_pvg_range_rules[i];
        if (r->tag != tag) continue;
        uint32_t limit = l->slots[r->slot];
        uint64_t index = hv_pvg_field(b, size, r->index_at, 4);
        if (r->form == HV_RANGE) {
            uint64_t count = hv_pvg_field(b, size, 4, 4);
            if (!hv_pvg_slots_ok(index, count, limit)) return "slots past the encoder's";
            if (size < r->head + count * r->stride) return "fewer bytes than its list";
        } else if (r->form == HV_ONE) {
            if (!hv_pvg_slots_ok(index, 1, limit)) return "a slot past the encoder's";
            if (size < r->head) return "fewer bytes than its fields";
        } else {
            uint64_t n = size / r->stride;
            if (!n || n > limit) return "more in its list than the encoder has";
        }
    }
    for (size_t i = 0; i < sizeof hv_pvg_enum_rules / sizeof *hv_pvg_enum_rules; i++) {
        const HvEnumRule *e = &hv_pvg_enum_rules[i];
        if (e->tag != tag) continue;
        uint64_t v = hv_pvg_field(b, size, e->at, e->width);
        if (v < e->min || v > e->max || (v & ~(uint64_t)e->mask)) return "a value Metal has no meaning for";
    }
    for (size_t i = 0; i < sizeof hv_pvg_span_rules / sizeof *hv_pvg_span_rules; i++) {
        const HvSpanRule *d = &hv_pvg_span_rules[i];
        if (d->tag != tag) continue;
        uint64_t start = d->start_width ? hv_pvg_field(b, size, d->start_at, d->start_width) : 0;
        uint64_t count = hv_pvg_field(b, size, d->count_at, d->count_width);
        if (start > 0xffffffffull || count > 0xffffffffull - start) return "more ids than 32 bits hold";
        if (d->nonzero && !count) return "no instances";
    }
    for (size_t i = 0; i < sizeof hv_pvg_work_rules / sizeof *hv_pvg_work_rules; i++) {
        const HvWorkRule *w = &hv_pvg_work_rules[i];
        if (w->tag != tag) continue;
        uint64_t count = hv_pvg_field(b, size, w->count_at, w->count_width), work;
        uint64_t inst = w->inst_width ? hv_pvg_field(b, size, w->inst_at, w->inst_width) : 1;
        if (__builtin_mul_overflow(count, inst, &work) || work > HV_PVG_MAX_WORK) return "more work than one command may ask";
    }
    if (tag == 10 || tag == 28) { /* base vertex: added to every index, within what an id can be either way */
        int64_t base = (int64_t)hv_pvg_field(b, size, 32, 8);
        if (base < INT32_MIN || base > INT32_MAX) return "base vertex past 32 bits";
    }
    if (tag == 130 || tag == 131) { /* viewports, {x, y, w, h, near, far} doubles (131: after a header): Metal stops the app on
                                       a NaN or infinite one (measured, its debug layer). Each double as the replay reads it:
                                       130's one viewport, all six, from a record padded with zeros if short */
        uint32_t head = tag == 130 ? 0 : size % 48, n = tag == 130 ? 1 : (size - head) / 48;
        for (uint32_t i = 0; i < n * 6; i++) {
            uint64_t bits = hv_pvg_field(b, size, head + i * 8, 8);
            double v;
            memcpy(&v, &bits, 8);
            if (!isfinite(v)) return "a viewport not a finite number";
        }
    }
    if (tag == 123) { /* the tessellation factor scale: Metal stops the app on one not a normal float above 0 (measured) */
        if (size < 4) return "tessellation factor scale";
        uint32_t bits = (uint32_t)hv_pvg_field(b, size, 0, 4);
        float f;
        memcpy(&f, &bits, 4);
        if (!isnormal(f) || f <= 0) return "tessellation factor scale";
    }
    if (tag == 154) { /* vertex amplification: each view's viewport within the 16 a draw has (its render target's layer
                         against the pass: the caller) */
        uint64_t count = hv_pvg_field(b, size, 0, 4);
        if (!count || count > 8 || size < 4 + count * 8) return "amplification views";
        for (uint32_t i = 0; i < count; i++)
            if (hv_pvg_field(b, size, 4 + i * 8, 4) >= 16) return "an amplification viewport past the 16";
    }
    if (tag == 209) { /* the stage-in region, 3 x {origin, size} 64: each thread's place in it within 32 bits */
        for (uint32_t i = 0; i < 3; i++) {
            uint64_t at = hv_pvg_field(b, size, i * 8, 8), n = hv_pvg_field(b, size, 24 + i * 8, 8);
            if (size < 48 || at > 0xffffffffull || n > 0xffffffffull - at) return "stage-in region past 32 bits";
        }
    }
    if (tag == 211) { /* threadgroup memory: a multiple of 16, at most the GPU's */
        uint64_t len = hv_pvg_field(b, size, 0, 8);
        if (len % 16 || len > l->threadgroup_bytes) return "threadgroup memory length";
    }
    return 0;
}

/* ---- Formats and the blocks their pixels come in. */

/* bw x bh pixels in bytes (1x1 for the uncompressed). bytes 0: not one a copy or a guest layout can name (PVRTC, the
   formats Apple keeps to itself, depth with stencil unless a copy says which). */
typedef struct HvPixelBlock {
    uint32_t w, h, bytes;
} HvPixelBlock;

static inline HvPixelBlock hv_pvg_block(uint32_t f) {
    uint32_t bpp = hv_pvg_linear_bpp(f);
    if (bpp && f != 301 && f != 302 && f != 303 && f != 304 && f != 313 && f != 314 && f != 323 && f != 324) return (HvPixelBlock){1, 1, bpp};
    switch (f) {
    case 130: case 131: case 140: case 141: return (HvPixelBlock){4, 4, 8};                                 /* BC1, BC4 */
    case 132 ... 135: case 142: case 143: case 150 ... 153: return (HvPixelBlock){4, 4, 16};                /* BC2, 3, 5, 6H, 7 */
    case 170: case 172: case 180 ... 183: return (HvPixelBlock){4, 4, 8};                                    /* EAC R11, ETC2 */
    case 174: case 176: case 178: case 179: return (HvPixelBlock){4, 4, 16};                                 /* EAC RG11, RGBA8 */
    case 186 ... 200: case 204 ... 218: case 222 ... 236: {                                             /* ASTC: sRGB, LDR, HDR */
        static const uint8_t astc[15][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {0, 0}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6},
                                            {10, 8}, {10, 10}, {12, 10}, {12, 12}};
        uint32_t i = f - (f >= 222 ? 222 : f >= 204 ? 204 : 186);
        return astc[i][0] ? (HvPixelBlock){astc[i][0], astc[i][1], 16} : (HvPixelBlock){0, 0, 0};
    }
    case 240: case 241: return (HvPixelBlock){2, 1, 4}; /* 4:2:2 */
    case 250: return (HvPixelBlock){1, 1, 2};           /* Depth16 */
    case 252: return (HvPixelBlock){1, 1, 4};           /* Depth32Float */
    case 253: return (HvPixelBlock){1, 1, 1};           /* Stencil8 */
    default: return (HvPixelBlock){0, 0, 0};
    }
}

/* What a texture of a format may be in a render pass: a colour attachment, depth, stencil (measured: every format
   Metal knows, as each role; the rest stop the app, or give no encoder). */
static inline int hv_pvg_color_format(uint32_t f) {
    switch (f) {
    case 301 ... 304: case 313: case 314: case 323: case 324: case 588: case 589: case 643: case 645: case 647: case 649: return 0;
    default: return hv_pvg_format_known(f) && hv_pvg_linear_bpp(f) != 0;
    }
}
static inline int hv_pvg_depth_format(uint32_t f) { return f == 250 || f == 252 || f == 260; }
static inline int hv_pvg_stencil_format(uint32_t f) { return f == 253 || f == 260; }

static inline int hv_pvg_is_cube(uint32_t type) { return type == HV_TEX_CUBE || type == HV_TEX_CUBE_ARRAY; }

/* Slices a texture has: each face of a cube one, and a 3D texture's depth none */
static inline uint64_t hv_pvg_slices(const HvTexDesc *t) {
    uint64_t n = hv_pvg_type_is_array(t->type) ? t->array : 1;
    return hv_pvg_is_cube(t->type) ? n * 6 : n;
}

/* A level's size in pixels */
static inline void hv_pvg_level_size(const HvTexDesc *t, uint64_t level, uint64_t s[3]) {
    int one = t->type == HV_TEX_1D || t->type == HV_TEX_1D_ARRAY || t->type == HV_TEX_BUFFER;
    s[0] = level < 32 && (t->width >> level) ? t->width >> level : 1;
    s[1] = one ? 1 : level < 32 && (t->height >> level) ? t->height >> level : 1;
    s[2] = t->type != HV_TEX_3D ? 1 : level < 32 && (t->depth >> level) ? t->depth >> level : 1;
}

/* ---- Render passes. */

/* One attachment as the pass record gives it, with what its textures are (as made: a view's own type, format, levels
   and slices). root, root_level and root_slice: the texture a view is of and where in it, to see two that overlap. */
typedef struct HvPassAttach {
    int present, has_resolve;
    HvTexDesc tex, resolve;
    uint64_t texture, resolve_texture; /* which objects they are */
    uint64_t root;
    uint32_t root_level, root_slice;
    uint32_t level, slice, depth_plane, resolve_level, resolve_slice, resolve_depth_plane;
    uint32_t load, store, options;
} HvPassAttach;

typedef struct HvPass {
    HvPassAttach color[8], depth, stencil; /* as the record lists them: depth, stencil, colours */
    double clear_depth;
    uint64_t layers, width, height;                                  /* 0: Metal's default */
    uint32_t raster_samples, imageblock, threadgroup, tile_w, tile_h; /* from the commands before the pass; 0: default */
} HvPass;

/* An attachment's place in its texture (level, slice or depth plane) and, when it resolves, the resolve texture's. */
static inline const char *hv_pvg_place_problem(const HvTexDesc *t, uint32_t level, uint32_t slice, uint32_t plane) {
    if (level >= t->mips) return "level";
    uint64_t s[3];
    hv_pvg_level_size(t, level, s);
    if (slice >= hv_pvg_slices(t)) return "slice";
    if (plane >= s[2]) return "depth plane";
    return 0;
}

/* role: 0 colour, 1 depth, 2 stencil */
static inline const char *hv_pvg_attach_problem(const HvPassAttach *a, int role) {
    if (!a->present) return 0;
    const HvTexDesc *t = &a->tex;
    if (!(t->usage & 4)) return "a texture made without render target use";
    if (role == 0 ? !hv_pvg_color_format(t->format) : role == 1 ? !hv_pvg_depth_format(t->format) : !hv_pvg_stencil_format(t->format))
        return "a pixel format it can't be";
    if (t->type == HV_TEX_1D || t->type == HV_TEX_1D_ARRAY || t->type == HV_TEX_BUFFER) return "a texture type it can't be";
    const char *why = hv_pvg_place_problem(t, a->level, a->slice, a->depth_plane);
    if (why) return why;
    if (((t->options >> 4) & 0xf) == 3 && ((a->store != 0 && a->store != 2) || a->load == 1)) return "memoryless, but loaded or stored";
    if (a->store == 2 || a->store == 3) { /* multisample resolve */
        const HvTexDesc *r = &a->resolve;
        if (!a->has_resolve || t->samples < 2) return "a resolve without a multisample texture and one to resolve into";
        if (r->format != t->format || r->samples != 1 || !(r->usage & 4) || ((r->options >> 4) & 0xf) == 3) return "resolve texture";
        if (r->type == HV_TEX_1D || r->type == HV_TEX_1D_ARRAY || r->type == HV_TEX_BUFFER) return "resolve texture type";
        if (hv_pvg_place_problem(r, a->resolve_level, a->resolve_slice, a->resolve_depth_plane)) return "resolve level or slice";
        uint64_t s[3], rs[3]; /* Metal: "texture and resolveTexture size mismatch." */
        hv_pvg_level_size(t, a->level, s);
        hv_pvg_level_size(r, a->resolve_level, rs);
        if (s[0] != rs[0] || s[1] != rs[1]) return "resolve texture's size";
    }
    return 0;
}

static inline uint64_t hv_pvg_attach_layers(const HvPassAttach *a) {
    uint64_t s[3];
    hv_pvg_level_size(&a->tex, a->level, s);
    return a->tex.type == HV_TEX_3D ? s[2] : hv_pvg_slices(&a->tex);
}

/* Bytes a sample of colour targets of these formats (0: none) takes in tile memory: each aligned to its size (8 at
   most), the whole to 8 */
static inline uint32_t hv_pvg_format_bytes(const uint32_t formats[8]) {
    uint32_t at = 0;
    for (int i = 0; i < 8; i++) {
        if (!formats[i]) continue;
        uint32_t b = hv_pvg_linear_bpp(formats[i]), align = b < 8 ? b : 8;
        at = (at + align - 1) / align * align + b;
    }
    return at;
}
static inline uint32_t hv_pvg_color_bytes(const HvPass *p) {
    uint32_t formats[8];
    for (int i = 0; i < 8; i++) formats[i] = p->color[i].present ? p->color[i].tex.format : 0;
    return hv_pvg_format_bytes(formats);
}

/* NULL if Metal can begin the pass, else what is wrong. First the record's enums, as given, then the pass as it will be
   made: an attachment with no texture loads and stores nothing, Unknown stores, a resolve action with nothing to
   resolve into stores, a resolve texture with no resolve action is dropped, and with no depth texture the clear depth
   is Metal's default (each of those Metal stops the app on). p is changed to that. Measured: macOS 27, Apple GPU. */
static inline const char *hv_pvg_pass_problem(HvPass *p, HvTexCaps caps) {
    HvPassAttach *all[10] = {&p->depth, &p->stencil};
    for (int i = 0; i < 8; i++) all[2 + i] = &p->color[i];
    for (int i = 0; i < 10; i++) {
        HvPassAttach *a = all[i];
        /* store 5, CustomSampleDepthStore: multisample depth only (below) */
        if (a->load > 2 || a->store > 5 || a->options > 1) return "load or store action";
        if (!a->present) {
            a->load = a->store = a->options = 0, a->has_resolve = 0;
            continue;
        }
        if (a->store == 4) a->store = 1;
        int resolving = a->store == 2 || a->store == 3;
        if (resolving && !a->has_resolve) a->store = 1;
        if (!resolving) a->has_resolve = 0;
    }
    if (!p->depth.present) p->clear_depth = 1;
    if (!(p->clear_depth >= 0 && p->clear_depth <= 1)) return "clear depth";

    uint32_t samples = 0;
    int any = 0;
    uint64_t min_w = UINT64_MAX, min_h = UINT64_MAX, min_layers = UINT64_MAX;
    for (int i = 0; i < 10; i++) {
        const HvPassAttach *a = all[i];
        if (!a->present) continue;
        const char *why = hv_pvg_attach_problem(a, i == 0 ? 1 : i == 1 ? 2 : 0);
        if (why) return why;
        if (a->store == 5 && (i != 0 || a->tex.samples < 2)) return "load or store action";
        if (any && a->tex.samples != samples) return "attachments of different sample counts";
        any = 1, samples = a->tex.samples;
        uint64_t s[3];
        hv_pvg_level_size(&a->tex, a->level, s);
        if (s[0] < min_w) min_w = s[0];
        if (s[1] < min_h) min_h = s[1];
        uint64_t layers = hv_pvg_attach_layers(a);
        if (layers < min_layers) min_layers = layers;
    }
    /* Depth and stencil in the combined format: Metal's "When depth and stencil are used together, the texture bound to
       the depth and stencil render pass attachments must be the same depth and stencil texture", "...has to write to
       the same level and slice for both", and the same of their resolve textures. Two textures of their own formats
       (Depth32Float and Stencil8) are fine. */
    const HvPassAttach *pd = &p->depth, *ps = &p->stencil;
    if (pd->present && ps->present && (pd->tex.format == 260 || ps->tex.format == 260)) {
        if (pd->texture != ps->texture || pd->level != ps->level || pd->slice != ps->slice)
            return "depth and stencil in the combined format, but not the same texture, level and slice";
        if (pd->has_resolve && ps->has_resolve &&
            (pd->resolve_texture != ps->resolve_texture || pd->resolve_level != ps->resolve_level || pd->resolve_slice != ps->resolve_slice))
            return "depth and stencil in the combined format, but not the same resolve texture, level and slice";
    }
    /* Two colour attachments on the same pixels (a view of a texture is its texture) */
    for (int i = 0; i < 8; i++)
        for (int j = i + 1; j < 8; j++) {
            const HvPassAttach *a = &p->color[i], *b = &p->color[j];
            if (a->present && b->present && a->root == b->root && a->root_level + a->level == b->root_level + b->level &&
                a->root_slice + a->slice == b->root_slice + b->slice && a->depth_plane == b->depth_plane)
                return "two colour attachments on the same pixels";
        }
    if (p->raster_samples && (p->raster_samples > 31 || !(caps.samples >> p->raster_samples & 1))) return "raster sample count";
    if (!any) {
        if (!p->width || !p->height || p->width > 16384 || p->height > 16384) return "size, with no attachments";
        if (!p->raster_samples) p->raster_samples = 1;
        if (p->layers > 2048) return "render target array length";
        samples = p->raster_samples;
    } else {
        if (p->width > min_w || p->height > min_h) return "bigger than its attachments";
        if (p->layers > 1 && p->layers > min_layers) return "render target array length";
    }
    /* Tile memory, as Metal's checks take it (measured, macOS 27, M5 Pro, encoders made and never committed; and Apple's
       feature set tables):
       - a tile size, when given, is one the sample count takes: 32x32 to 2 samples, 32x16 at 4, 16x16 at 8;
       - each sample of the colour attachments fits the GPU's implicit imageblock (caps.color_bytes: 32 B Apple2-3,
         64 B Apple4-6, 128 B from Apple7);
       - with imageblock or threadgroup memory asked for, the colour fits 64 bytes a sample (the explicit imageblock),
         and the larger of it and the imageblock, over the tile (16x16 when Metal picks it), with the threadgroup
         memory, fits 32 KB;
       - with a tile size alone, the colour over the tile fits the implicit imageblock memory (caps.implicit_bytes,
         128 KB, 256 KB from Apple9).
       With none of them asked for Metal picks a tile the colour fits, so nothing is summed. */
    uint32_t tile_samples = samples > p->raster_samples ? samples : p->raster_samples;
    int tile_ok = (!p->tile_w && !p->tile_h) || (p->tile_w == 16 && p->tile_h == 16) ||
                  (tile_samples <= 4 && p->tile_w == 32 && p->tile_h == 16) || (tile_samples <= 2 && p->tile_w == 32 && p->tile_h == 32);
    if (!tile_ok) return "tile size, for its sample count";
    if (p->imageblock > 64 || p->threadgroup % 16) return "imageblock or threadgroup length";
    uint32_t color = hv_pvg_color_bytes(p);
    if (caps.color_bytes && color > caps.color_bytes) return "colour attachments' bytes a sample";
    uint64_t pixels = p->tile_w ? (uint64_t)p->tile_w * p->tile_h : 256;
    if (p->imageblock || p->threadgroup) {
        if (color > 64) return "tile memory";
        uint64_t block = (color > p->imageblock ? color : p->imageblock);
        block = (block + 7) / 8 * 8;
        if (block * tile_samples * pixels + p->threadgroup > 32768) return "tile memory";
    } else if (p->tile_w && caps.implicit_bytes && (uint64_t)color * tile_samples * pixels > caps.implicit_bytes)
        return "tile memory";
    return 0;
}

/* ---- Pipelines. */

/* A pipeline description as Metal will be given it: form 0 render, 1 tile, 2 compute; which functions it has (kernel:
   a tile or compute pipeline's); and for render and tile pipelines, its targets' formats (0: none), with blending. */
typedef struct HvPipe {
    int form, vertex, fragment, kernel, alpha_to_coverage, alpha_to_one;
    uint32_t color[8], blending[8], depth, stencil;
    uint32_t raster_samples; /* render and tile */
    uint32_t tess_factor, tess_mode, topology; /* render: maxTessellationFactor, tessellationPartitionMode, inputPrimitiveTopology */
    uint64_t vertex_buffer; /* render: the largest buffer index a vertex attribute in use reads (+1), 0 none */
} HvPipe;

/* Integer formats: Metal blends none of them. Metal's own words (this OS's Metal.framework): "Blending is enabled
   for render target %lu; however, the pixelformat %s for this render target is not blendable", and alpha to coverage
   or one "requires a float/unorm pixel format, but colorAttachments[0].pixelFormat(%s) is not blendable". */
static inline int hv_pvg_blendable(uint32_t f) {
    switch (f) {
    case 13: case 14: case 23: case 24: case 33: case 34: case 53: case 54: case 63: case 64: case 73: case 74: case 91: case 103:
    case 104: case 113: case 114: case 123: case 124: return 0;
    default: return hv_pvg_color_format(f);
    }
}

/* Formats this GPU filters, so can make mipmaps of (Metal: "tex(%s) is not filterable."): a colour format that isn't an
   integer one, and a 32-bit float one only on a GPU that filters those */
static inline int hv_pvg_filterable(uint32_t f, HvTexCaps caps) {
    if (f == 55 || f == 105 || f == 125) return caps.float32_filter; /* R32, RG32, RGBA32 Float */
    return hv_pvg_blendable(f);
}

/* NULL if Metal's checks of the description would pass, else what is wrong. Metal checks a pipeline description with
   assertions (newRenderPipelineState..., -[MTLCompiler newRenderPipelineStateWithDescriptorInternal:...] calls
   validateWithDevice, whose failures go through MTLReportFailure, which stops the app), not with an error. The guest's
   Metal ran the same checks before sending it, but against the guest's device: these are those that can come out
   otherwise here, against this GPU, as the texture and pass checks do. A function the description needs (Metal: "%s
   must not be nil"); target formats this OS knows and can render to; blending only on a format that blends, and
   alpha to coverage or one only with a first target that does; a depth and a stencil format each of its kind, and the
   same one if either is a combined one ("must match"); and the colour targets' bytes a sample within what this GPU's
   tile memory takes ("requires %lu bytes of pixel storage. This device supports %lu"); a raster sample count this GPU
   takes; and a few cheap ones whose words are in the same table of Metal's messages as those (tessellation limits, the
   input topology, the vertex buffer an attribute reads). The others that don't depend on the device (blend factors,
   write masks, attribute formats) are the guest's Metal's, and their ranges change between OS versions. No rule for a
   vertex layout's stride (no such message in Metal) or a compute pipeline's thread count (Metal checks that against
   the pipeline built, when dispatching) is made up here. */
static inline const char *hv_pvg_pipeline_problem(const HvPipe *p, HvTexCaps caps) {
    if (p->form == 2) return p->kernel ? 0 : "no compute function";
    if (p->form == 1 ? !p->kernel : !p->vertex) return p->form == 1 ? "no tile function" : "no vertex function";
    for (int i = 0; i < 8; i++) {
        uint32_t f = p->color[i];
        if (!f) continue;
        if (!hv_pvg_color_format(f)) return "a colour target's pixel format";
        if (p->form == 0 && p->blending[i] && !hv_pvg_blendable(f)) return "blending on a pixel format that doesn't blend";
    }
    if (p->form == 0 && (p->alpha_to_coverage || p->alpha_to_one) && p->color[0] && !hv_pvg_blendable(p->color[0]))
        return "alpha to coverage or one, on a first target that doesn't blend";
    if (p->form == 0) {
        if (p->depth && !hv_pvg_depth_format(p->depth)) return "the depth pixel format";
        if (p->stencil && !hv_pvg_stencil_format(p->stencil)) return "the stencil pixel format";
        if (p->depth && p->stencil && p->depth != p->stencil && (p->depth == 260 || p->stencil == 260))
            return "a depth and stencil format that don't match";
    }
    if (p->form == 0 && caps.color_bytes && hv_pvg_format_bytes(p->color) > caps.color_bytes) return "colour targets' bytes a sample";
    /* Metal: "rasterSampleCount (%lu) is not supported by device." */
    if (p->form != 2 && p->raster_samples > 1 && (p->raster_samples > 31 || !(caps.samples >> p->raster_samples & 1)))
        return "a raster sample count this GPU doesn't take";
    if (p->form == 0) {
        /* Metal: "tessellationPartitionMode is not a valid MTLTessellationPartitionMode.", "maxTessellationFactor must
           be >= 1 and <= 64", "... must be a power of 2 when using MTLTessellationPartitionModePow2", "... must be even
           when using MTLTessellationPartitionModeFractionalOdd or MTLTessellationPartitionModeFractionalEven" */
        if (p->tess_mode > 3) return "the tessellation partition mode";
        if (p->tess_factor < 1 || p->tess_factor > 64) return "the largest tessellation factor";
        if (p->tess_mode == 0 && (p->tess_factor & (p->tess_factor - 1))) return "a largest tessellation factor not a power of 2";
        if (p->tess_mode >= 2 && p->tess_factor % 2) return "an odd largest tessellation factor for fractional partitions";
        /* "inputPrimitiveTopology value (%u) is invalid"; "buffer index (%lu) must be < %lu" (31 vertex buffers) */
        if (p->topology > 3) return "the input primitive topology";
        if (p->vertex_buffer > 31) return "a vertex attribute's buffer index";
    }
    return 0;
}

/* ---- Copies (blit commands). */

static inline int hv_pvg_blittable(const HvTexDesc *t, int framebuffer_only) {
    return !framebuffer_only && ((t->options >> 4) & 0xf) != 3;
}

/* A region of a level and slice: within it, in whole blocks unless it reaches the level's edge. */
static inline const char *hv_pvg_region_problem(const HvTexDesc *t, HvPixelBlock b, uint64_t slice, uint64_t level, const uint64_t o[3],
                                                const uint64_t s[3]) {
    if (level >= t->mips) return "level";
    if (slice >= hv_pvg_slices(t)) return "slice";
    uint64_t L[3];
    hv_pvg_level_size(t, level, L);
    for (int i = 0; i < 3; i++)
        if (!s[i] || o[i] > L[i] || s[i] > L[i] - o[i]) return "region past the level";
    if (o[0] % b.w || o[1] % b.h) return "origin inside a block";
    if ((s[0] % b.w && o[0] + s[0] != L[0]) || (s[1] % b.h && o[1] + s[1] != L[1])) return "size inside a block";
    return 0;
}

/* The bytes of a region of size s in a buffer of len bytes: rows of blocks row apart, images image apart, from offset. */
static inline const char *hv_pvg_buffer_image_problem(HvPixelBlock b, const uint64_t s[3], uint64_t len, uint64_t offset, uint64_t row,
                                                      uint64_t image) {
    uint64_t across = (s[0] + b.w - 1) / b.w * b.bytes, rows = (s[1] + b.h - 1) / b.h, need;
    if (row < across || row % b.bytes || offset % b.bytes) return "bytes per row or offset";
    if (__builtin_mul_overflow(row, rows - 1, &need) || __builtin_add_overflow(need, across, &need)) return "bytes per row";
    if (s[2] > 1) {
        uint64_t more;
        if (image < need || image % row || __builtin_mul_overflow(image, s[2] - 1, &more) || __builtin_add_overflow(need, more, &need))
            return "bytes per image";
    }
    if (offset > len || need > len - offset) return "past the end of the buffer";
    return 0;
}

/* The block a copy moves: depth with stencil says which of the two (blit options 1 depth, 2 stencil); others say none. */
static inline HvPixelBlock hv_pvg_copy_block(uint32_t f, uint64_t options) {
    if (f == 260) return options == 1 ? (HvPixelBlock){1, 1, 4} : options == 2 ? (HvPixelBlock){1, 1, 1} : (HvPixelBlock){0, 0, 0};
    return options ? (HvPixelBlock){0, 0, 0} : hv_pvg_block(f);
}

/* Between a buffer of len bytes and a texture (blit 300, 302): o and s the texture's region */
static inline const char *hv_pvg_copy_buffer_texture_problem(const HvTexDesc *t, int framebuffer_only, uint64_t slice, uint64_t level,
                                                             const uint64_t o[3], const uint64_t s[3], uint64_t len, uint64_t offset,
                                                             uint64_t row, uint64_t image, uint64_t options) {
    if (t->samples != 1 || !hv_pvg_blittable(t, framebuffer_only)) return "a texture that can't be copied";
    HvPixelBlock b = hv_pvg_copy_block(t->format, options);
    if (!b.bytes) return "a pixel format or options a copy can't take";
    const char *why = hv_pvg_region_problem(t, b, slice, level, o, s);
    return why ? why : hv_pvg_buffer_image_problem(b, s, len, offset, row, image);
}

/* Texture to texture (blit 303): the same format and samples, each region within its texture; not onto the pixels it
   copies from */
static inline const char *hv_pvg_copy_texture_problem(const HvTexDesc *a, int a_fb, uint64_t a_slice, uint64_t a_level, const uint64_t from[3],
                                                      const HvTexDesc *b, int b_fb, uint64_t b_slice, uint64_t b_level, const uint64_t to[3],
                                                      const uint64_t s[3], int same) {
    if (!hv_pvg_blittable(a, a_fb) || !hv_pvg_blittable(b, b_fb)) return "a texture that can't be copied";
    if (a->format != b->format || a->samples != b->samples) return "textures of different formats or samples";
    HvPixelBlock k = hv_pvg_block(a->format);
    if (!k.bytes) return "a pixel format a copy can't take";
    const char *why = hv_pvg_region_problem(a, k, a_slice, a_level, from, s);
    if (!why) why = hv_pvg_region_problem(b, k, b_slice, b_level, to, s);
    if (why || !same || a_slice != b_slice || a_level != b_level) return why;
    /* Within one level and slice of one texture: Metal takes regions apart (measured), not ones that overlap. Both
       regions are within the level, so nothing here overflows. */
    for (int i = 0; i < 3; i++)
        if (from[i] >= to[i] + s[i] || to[i] >= from[i] + s[i]) return 0;
    return "a copy onto itself";
}

/* Bytes of a buffer of len: from offset, size of them, within it */
static inline int hv_pvg_range_ok(uint64_t len, uint64_t offset, uint64_t size) {
    return size && offset <= len && size <= len - offset;
}

/* ---- A texture's layout in the guest's memory (makeTexture's table, page:toGuest's copies). */

/* Each level's entry in the table, 36 bytes: {offset, ?, bytes per row, width, height, depth} */
static inline void hv_pvg_mip_entry(const uint8_t *e, uint64_t *off, uint64_t *row, uint32_t size[3]) {
    memcpy(off, e, 8), memcpy(row, e + 0x10, 8), memcpy(size, e + 0x18, 12);
}

/* The guest's table against the texture as made: as many levels as it has, as many slices (either its array length, a
   cube's faces counted apart, or every slice), each level's own size, rows that hold a row of blocks, and every byte
   page:toGuest copies within len. slices_out: the slices to copy. */
static inline const char *hv_pvg_layout_problem(const HvTexDesc *t, uint32_t mips, uint32_t slices, uint64_t base, uint64_t stride,
                                                uint32_t block_h, const uint8_t *table, uint64_t len, uint32_t *slices_out) {
    HvPixelBlock b = hv_pvg_block(t->format);
    if (!b.bytes) return "a pixel format its memory can't be copied in";
    if (mips != t->mips) return "level count";
    uint64_t all = hv_pvg_slices(t);
    if (slices != all && slices != (hv_pvg_type_is_array(t->type) ? t->array : 1)) return "slice count";
    if (block_h ? block_h != b.h : b.h != 1) return "block height";
    if (base % b.bytes || stride % b.bytes) return "alignment";
    for (uint32_t m = 0; m < mips; m++) {
        uint64_t off, row, L[3], last, image, end;
        uint32_t size[3];
        hv_pvg_mip_entry(table + m * 36, &off, &row, size);
        hv_pvg_level_size(t, m, L);
        if (size[0] != L[0] || size[1] != L[1] || size[2] != L[2]) return "a level's size";
        uint64_t across = (L[0] + b.w - 1) / b.w * b.bytes, rows = (L[1] + b.h - 1) / b.h;
        if (row < across || row % b.bytes || off % b.bytes) return "bytes per row";
        if (__builtin_mul_overflow(stride, all - 1, &last) || __builtin_add_overflow(last, base, &last) ||
            __builtin_add_overflow(last, off, &last) || __builtin_mul_overflow(row, rows, &image) ||
            __builtin_mul_overflow(image, L[2], &end) || __builtin_add_overflow(end, last, &end) || end > len)
            return "past the end of its memory";
    }
    *slices_out = (uint32_t)all;
    return 0;
}

#endif
