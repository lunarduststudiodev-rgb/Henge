/* pvg_validate.h, the checks every texture description from the guest goes through before Metal sees it (Metal
   stops the whole app on one it can't make). First known answers, then descriptions made at random and made the
   way pvg_metal.m makes them, in forked children: whatever the checks let through, Metal must take or refuse with
   nil, never abort. Plain textures, textures in a heap, over a buffer, and views. */
#import <Metal/Metal.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pvg_command.h"

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const HvTexCaps caps_all = {1, 0x116, 0, 0, 1}; /* BC, 1, 2, 4 and 8 samples; tile memory not checked; filters 32-bit floats */
static HvTexDesc tex2d(uint32_t f, uint32_t w, uint32_t h) { return (HvTexDesc){HV_TEX_2D, f, w, h, 1, 1, 1, 1, 1, 0x20}; }

static void test_known(void) {
    printf("known answers\n");
    HvTexDesc d = tex2d(80, 64, 64);
    CHECK(!hv_pvg_texture_problem(&d, caps_all), "BGRA8 64x64");
    d.format = 9999;
    CHECK(hv_pvg_texture_problem(&d, caps_all), "format 9999");
    d = tex2d(0, 64, 64);
    CHECK(hv_pvg_texture_problem(&d, caps_all), "format 0 (what an unknown one is made)");
    d = tex2d(80, 0, 64);
    CHECK(hv_pvg_texture_problem(&d, caps_all), "width 0");
    d = tex2d(80, 16385, 64);
    CHECK(hv_pvg_texture_problem(&d, caps_all), "16385 wide");
    d = tex2d(80, 64, 64), d.mips = 8;
    CHECK(hv_pvg_texture_problem(&d, caps_all), "8 mips at 64x64 (7 is all)");
    d.mips = 7;
    CHECK(!hv_pvg_texture_problem(&d, caps_all), "7 mips at 64x64");
    d = tex2d(80, 64, 64), d.samples = 4;
    CHECK(hv_pvg_texture_problem(&d, caps_all), "2D with 4 samples");
    d.type = HV_TEX_2D_MS;
    CHECK(!hv_pvg_texture_problem(&d, caps_all), "2D multisample with 4");
    d.samples = 3;
    CHECK(hv_pvg_texture_problem(&d, caps_all), "3 samples, which the GPU doesn't take");
    d = tex2d(130, 64, 64);
    CHECK(!hv_pvg_texture_problem(&d, caps_all) && hv_pvg_texture_problem(&d, (HvTexCaps){0, 0x116, 0, 0, 1}), "BC1 only with BC");
    d = tex2d(80, 64, 32), d.type = HV_TEX_CUBE, d.array = 1;
    CHECK(hv_pvg_texture_problem(&d, caps_all), "cube not square");
    d = tex2d(80, 64, 64), d.type = HV_TEX_3D, d.depth = 2049;
    CHECK(hv_pvg_texture_problem(&d, caps_all), "3D 2049 deep");
    d = tex2d(80, 64, 64), d.usage = 0x40;
    CHECK(hv_pvg_texture_problem(&d, caps_all), "unknown usage bit");
    d = tex2d(115, 192, 128), d.mips = 3, d.usage = 0x10005;
    CHECK(!hv_pvg_texture_problem(&d, caps_all), "private usage 0x10000 (macOS's Liquid Glass blur textures)");
    d.usage = 0x20005;
    CHECK(hv_pvg_texture_problem(&d, caps_all), "usage bit 0x20000, which Metal aborts on");
    d = tex2d(80, 64, 64), d.options = 0x40;
    CHECK(hv_pvg_texture_problem(&d, caps_all), "storage mode 4");
    d = tex2d(80, 16, 16);
    CHECK(!hv_pvg_linear_problem(&d, caps_all, 4096, 0, 64, 16), "linear 16x16 BGRA8 in 4 KB, rows 64 apart");
    CHECK(hv_pvg_linear_problem(&d, caps_all, 4096, 0, 63, 0), "rows closer than a row's pixels");
    CHECK(hv_pvg_linear_problem(&d, caps_all, 4096, 0, 80, 64), "rows not aligned");
    CHECK(!hv_pvg_linear_problem(&d, caps_all, 4096, 3072, 64, 64), "the last 1 KB of the buffer");
    CHECK(hv_pvg_linear_problem(&d, caps_all, 4096, 3136, 64, 64), "past the end, by an offset");
    d.type = HV_TEX_BUFFER, d.height = 1;
    d.format = 130;
    CHECK(hv_pvg_texture_problem(&d, caps_all), "a texture buffer of a compressed format");
    d.format = 252;
    CHECK(hv_pvg_texture_problem(&d, caps_all), "a texture buffer of a depth format");
    d.format = 80, d.mips = 2;
    CHECK(hv_pvg_texture_problem(&d, caps_all), "a texture buffer with mips");
    d = tex2d(80, 16, 16);
    CHECK(hv_pvg_linear_problem(&d, caps_all, 4096, UINT64_MAX - 63, 64, 64), "an offset that wraps");
    CHECK(hv_pvg_linear_problem(&d, caps_all, UINT64_MAX, 0, UINT64_MAX / 8, 0), "height x row wraps");
    HvTexDesc parent = tex2d(80, 64, 64);
    parent.mips = 7;
    CHECK(!hv_pvg_view_problem(&parent, 81, HV_TEX_2D, 2, 3, 0, 1), "sRGB view, levels 2..4");
    CHECK(hv_pvg_view_problem(&parent, 80, HV_TEX_2D, 3, 5, 0, 1), "levels 3..7 of 7");
    CHECK(hv_pvg_view_problem(&parent, 80, HV_TEX_2D, UINT64_MAX, 2, 0, 1), "a first level that wraps");
    CHECK(hv_pvg_view_problem(&parent, 80, HV_TEX_2D, 0, 1, 1, 1), "slice 1 of 1");
    CHECK(hv_pvg_view_problem(&parent, 80, HV_TEX_3D, 0, 1, 0, 1), "a 2D texture as 3D");
    CHECK(hv_pvg_view_problem(&parent, 115, HV_TEX_2D, 0, 1, 0, 1), "RGBA16Float, of another size");
}

/* ---- At random, against Metal ---- */
/* Render passes (pvg_command.h), against what Metal's own checks did with each (measured, macOS 27, M5 Pro, encoders
   made and never committed: scratch probes, and the second review's probe_strict) */
static HvPass pass_of(int colours, uint32_t format, uint32_t samples) {
    HvPass p;
    memset(&p, 0, sizeof p);
    for (int i = 0; i < colours; i++) {
        HvPassAttach *a = &p.color[i];
        a->present = 1, a->root = 100 + i, a->load = 2, a->store = 1;
        a->tex = (HvTexDesc){samples > 1 ? HV_TEX_2D_MS : HV_TEX_2D, format, 64, 64, 1, 1, samples, 1, 4, 0x20};
    }
    p.clear_depth = 1;
    return p;
}
static const char *pass_problem(HvPass p, HvTexCaps caps) { return hv_pvg_pass_problem(&p, caps); }

static void test_pass(void) {
    printf("render passes: tiles, tile memory, depth stores\n");
    HvTexCaps m5 = {0, 0x116, 128, 256 << 10, 1}, a14 = {0, 0x16, 128, 128 << 10, 1}, a13 = {0, 0x16, 64, 128 << 10, 0};
    HvPass p = pass_of(5, 115, 4);
    CHECK(!pass_problem(p, m5), "5 x RGBA16Float at 4x (40 bytes a sample): Metal took it (the second review's case 51)");
    p = pass_of(8, 115, 4);
    CHECK(!pass_problem(p, m5) && !pass_problem(p, a13), "8 x RGBA16Float at 4x (64 bytes a sample)");
    p = pass_of(8, 125, 1);
    CHECK(!pass_problem(p, m5), "8 x RGBA32Float (128 bytes a sample), from Apple7");
    CHECK(pass_problem(p, a13) != NULL, "128 bytes a sample before Apple7 (64)");
    p = pass_of(8, 125, 4);
    CHECK(!pass_problem(p, m5), "128 bytes at 4x, Metal's tile");
    p = pass_of(1, 80, 4), p.tile_w = 32, p.tile_h = 32;
    CHECK(pass_problem(p, m5) != NULL, "4x with a 32x32 tile: Metal's validation stops on it (case 52)");
    p.tile_h = 16;
    CHECK(!pass_problem(p, m5), "4x with 32x16");
    p.tile_w = p.tile_h = 16;
    CHECK(!pass_problem(p, m5), "4x with 16x16");
    p = pass_of(1, 80, 8), p.tile_w = 32, p.tile_h = 16;
    CHECK(pass_problem(p, m5) != NULL, "8x with 32x16");
    p.tile_w = 16;
    CHECK(!pass_problem(p, m5), "8x with 16x16");
    p = pass_of(1, 80, 2), p.tile_w = p.tile_h = 32;
    CHECK(!pass_problem(p, m5), "2x with 32x32");
    p = pass_of(8, 125, 2), p.tile_w = p.tile_h = 32;
    CHECK(!pass_problem(p, m5), "128 bytes, 2x, 32x32: 256 KB of implicit imageblock, Apple9's (Metal took it)");
    CHECK(pass_problem(p, a14) != NULL, "the same past Apple7's 128 KB");
    p = pass_of(8, 115, 1), p.tile_w = p.tile_h = 32;
    CHECK(!pass_problem(p, m5), "64 bytes, 32x32, no threadgroup memory: Metal took it");
    p.threadgroup = 16;
    CHECK(pass_problem(p, m5) != NULL, "and with 16 bytes of threadgroup memory: 65552 of 32768");
    p.tile_w = p.tile_h = 0;
    CHECK(!pass_problem(p, m5), "the same with Metal's tile");
    p = pass_of(8, 125, 1), p.threadgroup = 16;
    CHECK(pass_problem(p, m5) != NULL, "128 bytes a sample with threadgroup memory (64 then)");
    p = pass_of(1, 80, 1), p.tile_w = p.tile_h = 32, p.imageblock = 32;
    CHECK(!pass_problem(p, m5), "imageblock 32 over 32x32: 32 KB");
    p.imageblock = 64;
    CHECK(pass_problem(p, m5) != NULL, "imageblock 64 over 32x32: 64 KB");
    p.imageblock = 32, p.threadgroup = 16;
    CHECK(pass_problem(p, m5) != NULL, "32 KB and 16 bytes");
    p = pass_of(1, 80, 1), p.tile_w = p.tile_h = 32, p.threadgroup = 16384;
    CHECK(!pass_problem(p, m5), "4 bytes over 32x32 and 16 KB of threadgroup memory");
    p.threadgroup = 30000 - 30000 % 16;
    CHECK(pass_problem(p, m5) != NULL, "and 29984 bytes");
    p = pass_of(1, 80, 4), p.tile_w = 32, p.tile_h = 16, p.imageblock = 16;
    CHECK(!pass_problem(p, m5), "imageblock 16, 4x, 32x16: 32 KB");
    p.imageblock = 32;
    CHECK(pass_problem(p, m5) != NULL, "imageblock 32, 4x, 32x16: 64 KB");

    p = pass_of(1, 80, 4);
    p.depth.present = 1, p.depth.root = 99, p.depth.load = 2, p.depth.store = 5;
    p.depth.tex = (HvTexDesc){HV_TEX_2D_MS, 252, 64, 64, 1, 1, 4, 1, 4, 0x20};
    CHECK(!pass_problem(p, m5), "multisample depth stored with CustomSampleDepthStore: Metal took it (case 53)");
    p.color[0].store = 5;
    CHECK(pass_problem(p, m5) != NULL, "CustomSampleDepthStore on a colour attachment");
    p = pass_of(1, 80, 1);
    p.depth.present = 1, p.depth.root = 99, p.depth.load = 2, p.depth.store = 5;
    p.depth.tex = (HvTexDesc){HV_TEX_2D, 252, 64, 64, 1, 1, 1, 1, 4, 0x20};
    CHECK(pass_problem(p, m5) != NULL, "CustomSampleDepthStore on depth of one sample");
    p.depth.store = 6;
    CHECK(pass_problem(p, m5) != NULL, "store action 6");

    printf("copies within one texture\n");
    HvTexDesc t = tex2d(80, 64, 64);
    uint64_t at0[3] = {0, 0, 0}, at32[3] = {32, 32, 0}, at8[3] = {8, 8, 0}, at16x[3] = {16, 0, 0}, size[3] = {16, 16, 1};
    CHECK(!hv_pvg_copy_texture_problem(&t, 0, 0, 0, at0, &t, 0, 0, 0, at32, size, 1), "regions apart: Metal took it (case 54)");
    CHECK(!hv_pvg_copy_texture_problem(&t, 0, 0, 0, at0, &t, 0, 0, 0, at16x, size, 1), "regions side by side");
    CHECK(hv_pvg_copy_texture_problem(&t, 0, 0, 0, at0, &t, 0, 0, 0, at8, size, 1) != NULL, "regions that overlap");
    CHECK(hv_pvg_copy_texture_problem(&t, 0, 0, 0, at0, &t, 0, 0, 0, at0, size, 1) != NULL, "onto itself");
    HvTexDesc m = tex2d(80, 64, 64);
    m.mips = 2;
    CHECK(!hv_pvg_copy_texture_problem(&m, 0, 0, 0, at0, &m, 0, 0, 1, at0, size, 1), "level 0 to level 1");
}

/* The indirect draws' clamp (HV_PVG_CLAMP_MSL): known answers in C (hv_pvg_clamp), then the same jobs once on the GPU,
   which must leave the same. A fixed, tiny dispatch: one thread, sixteen jobs. */
static void test_clamp(void) {
    printf("indirect draws' clamp\n");
    enum { N = 16 };
    static const HvClampJob jobs[N] = {
        {HV_CLAMP_DRAW, 1, 0xffffffffu, 0, 0, 0, 0, 0},    /* 15 vertices x 3,949,456,801 instances: what hung the GPU */
        {HV_CLAMP_INDEXED, 1, 4096, 0, 0, 0, 0, 0},        /* 2^31 x 2^31 from a buffer of 4096 indexes */
        {HV_CLAMP_INDEXED, 1, 4096, 0, 0, 0, 0, 0},        /* starting past the buffer's indexes */
        {HV_CLAMP_PATCHES, 256, 8, 0, 0, 0, 0, 0},         /* patches past their index buffer, a pipeline of factor 16 */
        {HV_CLAMP_NONE, 1, 0xffffffffu, 0, 0, 0, 0, 0},    /* arguments that weren't copied (outside their buffer) */
        {HV_CLAMP_DRAW, 1, 0xffffffffu, 0, 0, 0, 0, 0},    /* ids past 32 bits: vertices from 2^32 - 11, instances from 2^32 - 16 */
        /* tessellation factors for each patch (step 1): 100 patches from 4 of 10 triangles' (80 bytes), 5 instances
           sharing them */
        {HV_CLAMP_PATCHES, 1, 0xffffffffu, 8, 80, 0, 0, 1},
        /* for each patch and instance (step 3): 1000 instances from base 2 of 10 quad patches, 120 bytes apart in 1200: instances 2 to 9 */
        {HV_CLAMP_PATCHES, 1, 0xffffffffu, 12, 1200, 120, 0, 3},
        /* ids from a patch index buffer of 8 (the guest's: any 32-bit ids), factors for each patch (step 1), however
           many: none (review 4) */
        {HV_CLAMP_PATCHES, 1, 8, 8, 4096, 0, 1, 1},
        /* the same for each patch and instance, 64 bytes of factors 16 apart: none either */
        {HV_CLAMP_PATCHES, 1, 8, 8, 64, 16, 1, 3},
        /* one record for all (step 0, Constant): all 100 x 5 */
        {HV_CLAMP_PATCHES, 1, 0xffffffffu, 8, 8, 0, 0, 0},
        /* the same with an instance stride, which Metal stops the app on: none */
        {HV_CLAMP_PATCHES, 1, 0xffffffffu, 8, 8, 8, 0, 0},
        /* one for each instance (step 2): 3 records 12 apart, 10 instances from base 1: 2; the patches all */
        {HV_CLAMP_PATCHES, 1, 0xffffffffu, 12, 36, 12, 0, 2},
        /* one for each patch (step 1): 3 records, 10 patches from 1: 2; the instances all */
        {HV_CLAMP_PATCHES, 1, 0xffffffffu, 8, 24, 0, 0, 1},
        {HV_CLAMP_DRAW, 1, 0xffffffffu, 0, 0, 0, 0, 0},    /* within the budget left */
        {HV_CLAMP_DRAW, 1, 0xffffffffu, 0, 0, 0, 0, 0},    /* past it */
    };
    static const uint32_t in[N][8] = {
        {15, 3949456801u, 0, 0},
        {0x80000000u, 0x80000000u, 0, 0, 0},
        {100, 1, 5000, 0, 0},
        {10, 10, 0, 0},
        {1, 2, 3, 4, 5},
        {100, 100, 0xffffffffu - 11, 0xffffffffu - 16},
        {100, 5, 4, 0},
        {10, 1000, 0, 2},
        {5, 1, 0, 0},
        {8, 100, 0, 0},
        {100, 5, 4, 0},
        {100, 5, 4, 0},
        {50, 10, 0, 1},
        {10, 7, 1, 0},
        {6, 1, 0, 0},
        {10, 1, 0, 0},
    };
    const uint64_t start = 1073741820ull + 1073741824ull + 20480 + 11 * 16 + 6 * 5 + 10 * 8 + 100 * 5 + 50 * 2 + 2 * 7 + 6 + 5;
    uint32_t want[N][2] = {{15, 71582788}, {4096, 262144}, {0, 1}, {8, 10}, {0, 0}, {11, 16}, {6, 5}, {10, 8}, {0, 1}, {0, 100}, {100, 5}, {0, 5}, {50, 2}, {2, 7},
                           {6, 1}, {0, 0}};
    uint32_t c[N][8];
    memcpy(c, in, sizeof c);
    uint64_t left = start;
    hv_pvg_clamp(&c[0][0], jobs, N, &left);
    for (int i = 0; i < N; i++)
        CHECK(c[i][0] == want[i][0] && c[i][1] == want[i][1], "job %d: %u x %u, not %u x %u", i, c[i][0], c[i][1], want[i][0], want[i][1]);
    CHECK(c[4][2] == 0 && c[4][3] == 0 && c[4][4] == 0, "a job not copied is all zeros");
    CHECK(left == 5, "budget left %llu, not 5", (unsigned long long)left);
    left = HV_PVG_EXEC_WORK;
    uint32_t big[40][8], n = 0;
    HvClampJob many[40];
    for (; n < 40; n++) {
        uint32_t a[8] = {32768, 32768};
        memcpy(big[n], a, sizeof a), many[n] = (HvClampJob){HV_CLAMP_DRAW, 1, 0xffffffffu, 0, 0, 0, 0, 0};
    }
    hv_pvg_clamp(&big[0][0], many, n, &left);
    CHECK(big[31][1] == 32768 && big[32][0] == 0 && big[39][0] == 0 && left == 0, "40 draws of 2^30: 32 fit the budget, the rest none");
    id<MTLDevice> dev = getenv("HV_NO_GPU") ? nil : MTLCreateSystemDefaultDevice(); /* last: children forked after it would have no Metal */
    if (!dev) { printf("  no GPU here: only in C\n"); return; }
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@HV_PVG_CLAMP_MSL options:nil error:&err];
    id<MTLComputePipelineState> pipe = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"hv_clamp"] error:&err] : nil;
    CHECK(pipe != nil, "the clamp builds: %s", err.localizedDescription.UTF8String);
    if (!pipe) return;
    id<MTLBuffer> args = [dev newBufferWithBytes:in length:sizeof in options:MTLResourceStorageModeShared];
    id<MTLBuffer> jb = [dev newBufferWithBytes:jobs length:sizeof jobs options:MTLResourceStorageModeShared];
    id<MTLBuffer> lb = [dev newBufferWithBytes:&start length:8 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [[dev newCommandQueue] commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    uint32_t count = N;
    [e setComputePipelineState:pipe];
    [e setBuffer:args offset:0 atIndex:0], [e setBuffer:jb offset:0 atIndex:1], [e setBytes:&count length:4 atIndex:2], [e setBuffer:lb offset:0 atIndex:3];
    [e dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted, "the clamp ran: %s", cb.error.localizedDescription.UTF8String);
    CHECK(!memcmp(args.contents, c, sizeof c), "the GPU's clamp left what the C one did");
    CHECK(*(uint64_t *)lb.contents == 5, "the GPU's budget left %llu, not 5", *(unsigned long long *)lb.contents);
}

/* Fields Metal stops the app on, or that reach the GPU unbounded: viewports, the tessellation factor scale, amplification
   viewports, the stage-in region */
static void test_fields(void) {
    printf("command fields\n");
    HvCmdLimits l = hv_pvg_cmd_limits(128, 32768);
    double vp[6] = {0, 0, 64, 64, 0, 1};
    CHECK(!hv_pvg_command_problem(130, (const uint8_t *)vp, sizeof vp, &l), "a viewport");
    vp[2] = NAN;
    CHECK(hv_pvg_command_problem(130, (const uint8_t *)vp, sizeof vp, &l), "a NaN viewport");
    vp[2] = 64, vp[0] = INFINITY;
    CHECK(hv_pvg_command_problem(130, (const uint8_t *)vp, sizeof vp, &l), "an infinite viewport");
    vp[0] = NAN; /* a short one (40 bytes): the replay sets it padded with zeros, so its first five are checked too */
    CHECK(hv_pvg_command_problem(130, (const uint8_t *)vp, 40, &l), "a NaN viewport of 40 bytes");
    vp[0] = 0, vp[4] = INFINITY;
    CHECK(hv_pvg_command_problem(130, (const uint8_t *)vp, 40, &l), "an infinite near of 40 bytes");
    vp[4] = 0, vp[5] = NAN;
    CHECK(!hv_pvg_command_problem(130, (const uint8_t *)vp, 40, &l), "40 bytes: far read as 0, not the NaN past them");
    CHECK(!hv_pvg_command_problem(130, (const uint8_t *)vp, 0, &l), "an empty viewport: all zeros");
    vp[5] = 1;
    uint8_t two[2] = {0, 0x3c};
    CHECK(hv_pvg_command_problem(123, two, 2, &l), "factor scale of 2 bytes");
    uint8_t many[8 + 3 * 48] = {0};
    double ok[6] = {0, 0, 64, 64, 0, 1};
    for (int i = 0; i < 3; i++) memcpy(many + 8 + i * 48, ok, 48);
    CHECK(!hv_pvg_command_problem(131, many, sizeof many, &l), "3 viewports");
    double bad = -INFINITY;
    memcpy(many + 8 + 2 * 48 + 40, &bad, 8);
    CHECK(hv_pvg_command_problem(131, many, sizeof many, &l), "the third's far infinite");
    float f = 1;
    CHECK(!hv_pvg_command_problem(123, (const uint8_t *)&f, 4, &l), "factor scale 1");
    float fs[4] = {0, -1, NAN, 1e-40f};
    for (int i = 0; i < 4; i++) CHECK(hv_pvg_command_problem(123, (const uint8_t *)&fs[i], 4, &l), "factor scale %g", fs[i]);
    uint32_t amp[5] = {2, 0, 0, 15, 0};
    CHECK(!hv_pvg_command_problem(154, (const uint8_t *)amp, sizeof amp, &l), "views at viewports 0 and 15");
    amp[3] = 16;
    CHECK(hv_pvg_command_problem(154, (const uint8_t *)amp, sizeof amp, &l), "a view at viewport 16");
    amp[0] = 9;
    CHECK(hv_pvg_command_problem(154, (const uint8_t *)amp, sizeof amp, &l), "9 views");
    uint64_t region[6] = {0, 0, 0, 64, 1, 1};
    CHECK(!hv_pvg_command_problem(209, (const uint8_t *)region, sizeof region, &l), "stage-in 64");
    region[0] = 0xfffffff0ull, region[3] = 0x100;
    CHECK(hv_pvg_command_problem(209, (const uint8_t *)region, sizeof region, &l), "stage-in past 2^32");
    region[0] = 1ull << 40, region[3] = 1;
    CHECK(hv_pvg_command_problem(209, (const uint8_t *)region, sizeof region, &l), "stage-in at 2^40");
}

static uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}
typedef struct { uint64_t s; } Rng;
static uint32_t pick(Rng *g, uint32_t n) { g->s = mix(g->s); return (uint32_t)(g->s % n); }

static uint32_t known[700], nknown;
static uint32_t any_format(Rng *g) {
    if (pick(g, 10) < 8) return known[pick(g, nknown)];
    return pick(g, 4) ? pick(g, 700) : pick(g, 65536);
}
static uint32_t size(Rng *g) {
    static const uint32_t s[] = {0, 1, 2, 3, 4, 6, 7, 8, 16, 31, 64, 100, 256, 1024, 2048, 2049, 4096, 8192, 16384, 16385, 65536};
    return pick(g, 3) ? 1u << pick(g, 8) : s[pick(g, sizeof s / sizeof s[0])];
}
/* Near what a guest sends, with a field or two anywhere */
static HvTexDesc any_desc(Rng *g) {
    HvTexDesc d = {HV_TEX_2D, any_format(g), 1u << pick(g, 8), 1u << pick(g, 8), 1, 1, 1, 1, 1 + pick(g, 7), 0x20};
    if (pick(g, 2)) d.type = pick(g, 12);
    if (d.type == HV_TEX_1D || d.type == HV_TEX_1D_ARRAY || d.type == HV_TEX_BUFFER) d.height = 1;
    if (d.type == HV_TEX_CUBE || d.type == HV_TEX_CUBE_ARRAY) d.height = d.width;
    if (d.type == HV_TEX_3D) d.depth = 1u << pick(g, 6);
    if (hv_pvg_type_is_array(d.type)) d.array = 1 + pick(g, 8);
    if (d.type == HV_TEX_2D_MS || d.type == HV_TEX_2D_MS_ARRAY) d.samples = 1u << pick(g, 4);
    else if (pick(g, 2)) d.mips = 1 + pick(g, 9);
    for (uint32_t n = pick(g, 3); n; n--) {
        switch (pick(g, 10)) {
        case 0: d.width = size(g); break;
        case 1: d.height = size(g); break;
        case 2: d.depth = size(g); break;
        case 3: d.mips = pick(g, 18); break;
        case 4: d.samples = pick(g, 40); break;
        case 5: d.array = pick(g, 4) ? pick(g, 20) : 2040 + pick(g, 20); break;
        case 6: d.usage = pick(g, 4) ? pick(g, 0x40) : pick(g, 0x10000); break;
        case 7: d.options = pick(g, 4) << 4 | pick(g, 2) | pick(g, 3) << 8; break;
        case 8: d.options = pick(g, 0x10000); break;
        default: d.type = pick(g, 16); break;
        }
    }
    return d;
}
static MTLTextureDescriptor *made(const HvTexDesc *d) { /* as pvg_metal.m makes one: an unknown format stays Invalid */
    MTLTextureDescriptor *t = [MTLTextureDescriptor new];
    t.textureType = (MTLTextureType)d->type;
    t.pixelFormat = hv_pvg_format_known(d->format) ? (MTLPixelFormat)d->format : MTLPixelFormatInvalid;
    t.width = d->width, t.height = d->height, t.depth = d->depth;
    t.mipmapLevelCount = d->mips, t.sampleCount = d->samples, t.arrayLength = d->array;
    t.usage = (MTLTextureUsage)d->usage;
    t.resourceOptions = (MTLResourceOptions)d->options;
    return t;
}
static HvTexDesc read_back(MTLTextureDescriptor *t) {
    return (HvTexDesc){(uint32_t)t.textureType, (uint32_t)t.pixelFormat, (uint32_t)t.width, (uint32_t)t.height, (uint32_t)t.depth,
                       (uint32_t)t.mipmapLevelCount, (uint32_t)t.sampleCount, (uint32_t)t.arrayLength, t.usage, t.resourceOptions};
}
static uint64_t bytes_of(const HvTexDesc *d) { /* what making it could take: big ones aren't made here, only checked */
    return (uint64_t)d->width * d->height * d->depth * d->array * (d->type == HV_TEX_CUBE || d->type == HV_TEX_CUBE_ARRAY ? 6 : 1) * d->samples * 16;
}

enum { KIND_PLAIN, KIND_HEAP, KIND_LINEAR, KIND_VIEW, KINDS };
static const char *kind_name[KINDS] = {"texture", "heap texture", "linear texture", "view"};
/* Shared with the children: where each is, and the description it is about to hand to Metal */
typedef struct {
    volatile uint64_t at;
    HvTexDesc d;
    uint64_t extra[6];
    uint64_t passed[KINDS], made[KINDS], refused[KINDS], big[KINDS];
} Shared;
static Shared *sh;

static void run(int kind, uint64_t from, uint64_t to, uint64_t seed) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        HvTexCaps caps = {dev.supportsBCTextureCompression, 0, 0, 0, dev.supports32BitFloatFiltering};
        for (uint32_t n = 2; n <= 8; n++) if ([dev supportsTextureSampleCount:n]) caps.samples |= 1u << n;
        caps.samples |= 2;
        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.size = 16 << 20, hd.storageMode = MTLStorageModePrivate, hd.type = MTLHeapTypePlacement;
        id<MTLHeap> heap = kind == KIND_HEAP ? [dev newHeapWithDescriptor:hd] : nil;
        id<MTLBuffer> buffer = kind == KIND_LINEAR ? [dev newBufferWithLength:4 << 20 options:MTLResourceStorageModeShared] : nil;
        NSMutableArray<id<MTLTexture>> *parents = [NSMutableArray new];
        if (kind == KIND_VIEW) {
            static const HvTexDesc p[] = {{HV_TEX_2D, 80, 64, 64, 1, 7, 1, 1, 7, 0x20}, {HV_TEX_2D_ARRAY, 70, 32, 32, 1, 6, 1, 4, 7, 0x20},
                                          {HV_TEX_CUBE, 115, 16, 16, 1, 5, 1, 1, 7, 0x20}, {HV_TEX_CUBE_ARRAY, 70, 16, 16, 1, 1, 1, 2, 1, 0x20},
                                          {HV_TEX_3D, 70, 16, 16, 8, 3, 1, 1, 1, 0x20}, {HV_TEX_2D_MS, 80, 64, 64, 1, 1, 4, 1, 4, 0x20},
                                          {HV_TEX_2D, 130, 64, 64, 1, 4, 1, 1, 1, 0x20}, {HV_TEX_2D, 260, 64, 64, 1, 1, 1, 1, 5, 0x20},
                                          {HV_TEX_1D_ARRAY, 10, 64, 1, 1, 1, 1, 3, 1, 0x20}, {HV_TEX_2D, 552, 64, 64, 1, 1, 1, 1, 1, 0x20}};
            for (size_t i = 0; i < sizeof p / sizeof p[0]; i++) {
                id<MTLTexture> t = hv_pvg_texture_problem(&p[i], caps) ? nil : [dev newTextureWithDescriptor:made(&p[i])];
                if (t) [parents addObject:t];
            }
        }
        for (uint64_t i = from; i < to; i++) @autoreleasepool {
            Rng g = {seed ^ mix(i * 0x10001 + kind)};
            sh->at = i;
            if (kind == KIND_VIEW) {
                id<MTLTexture> from_tex = parents[pick(&g, (uint32_t)parents.count)];
                HvTexDesc pd = {(uint32_t)from_tex.textureType, (uint32_t)from_tex.pixelFormat, 0, 0, 0, (uint32_t)from_tex.mipmapLevelCount, 0,
                                (uint32_t)from_tex.arrayLength, 0, 0};
                uint64_t all = (uint64_t)pd.array * (pd.type == HV_TEX_CUBE || pd.type == HV_TEX_CUBE_ARRAY ? 6 : 1);
                uint32_t f = pick(&g, 3) ? any_format(&g) : pd.format, type = pick(&g, 2) ? pd.type : pick(&g, 12);
                uint64_t level = pick(&g, 4), levels = pick(&g, 5), slice = pick(&g, 8), slices = pick(&g, 4) ? pick(&g, 4) : pick(&g, 14);
                if (pick(&g, 8) == 0) level = UINT64_MAX - pick(&g, 2);
                BOOL whole = pick(&g, 4) == 0; /* as tag 7 asks it */
                const char *why = whole ? hv_pvg_view_problem(&pd, f, pd.type, 0, pd.mips, 0, all) : hv_pvg_view_problem(&pd, f, type, level, levels, slice, slices);
                sh->d = pd, sh->d.format = f, sh->d.type = type;
                sh->extra[0] = level, sh->extra[1] = levels, sh->extra[2] = slice, sh->extra[3] = slices, sh->extra[4] = whole, sh->extra[5] = from_tex.pixelFormat;
                if (why) { sh->refused[kind]++; continue; }
                sh->passed[kind]++;
                id<MTLTexture> v = whole ? [from_tex newTextureViewWithPixelFormat:(MTLPixelFormat)f]
                                         : [from_tex newTextureViewWithPixelFormat:(MTLPixelFormat)f textureType:(MTLTextureType)type
                                                                          levels:NSMakeRange(level, levels) slices:NSMakeRange(slice, slices)];
                if (v) sh->made[kind]++;
                continue;
            }
            HvTexDesc raw = any_desc(&g);
            if (kind == KIND_LINEAR && pick(&g, 4)) { /* mostly what a buffer can hold */
                raw.type = pick(&g, 4) ? HV_TEX_2D : HV_TEX_BUFFER, raw.mips = 1, raw.depth = 1, raw.array = 1, raw.samples = 1;
                if (raw.type == HV_TEX_BUFFER) raw.height = 1;
            }
            MTLTextureDescriptor *t = made(&raw);
            uint64_t offset = 0, row = 0;
            if (kind == KIND_HEAP) t.resourceOptions = heap.resourceOptions, offset = pick(&g, 3) ? (uint64_t)pick(&g, 64) << 16 : pick(&g, 1 << 25);
            if (kind == KIND_LINEAR) {
                t.resourceOptions = buffer.resourceOptions;
                t.usage |= MTLTextureUsageShaderRead;
                HvTexDesc now = read_back(t);
                uint64_t bpp = hv_pvg_linear_bpp(now.format), align = bpp ? [dev minimumLinearTextureAlignmentForPixelFormat:t.pixelFormat] : 1;
                row = (uint64_t)now.width * (bpp ? bpp : 4);
                if (align) row = (row + align - 1) / align * align;
                if (pick(&g, 4) == 0) row += pick(&g, 3) ? align : pick(&g, 64);
                if (pick(&g, 8) == 0) row = pick(&g, 0x10000);
                offset = pick(&g, 2) ? 0 : pick(&g, 3) ? (uint64_t)pick(&g, 1024) * align : pick(&g, 5 << 20);
            }
            HvTexDesc d = read_back(t);
            sh->d = d, sh->extra[0] = offset, sh->extra[1] = row;
            const char *why;
            if (kind == KIND_LINEAR) {
                uint64_t align = hv_pvg_linear_bpp(d.format) ? [dev minimumLinearTextureAlignmentForPixelFormat:t.pixelFormat] : 0;
                why = hv_pvg_linear_problem(&d, caps, buffer.length, offset, row, align);
            } else why = hv_pvg_texture_problem(&d, caps);
            if (why) { sh->refused[kind]++; continue; }
            sh->passed[kind]++;
            if (bytes_of(&d) > (512ull << 20)) { sh->big[kind]++; continue; }
            id<MTLTexture> x = kind == KIND_PLAIN ? [dev newTextureWithDescriptor:t]
                             : kind == KIND_HEAP ? [heap newTextureWithDescriptor:t offset:(NSUInteger)offset]
                                                 : [buffer newTextureWithDescriptor:t offset:(NSUInteger)offset bytesPerRow:(NSUInteger)row];
            if (x) sh->made[kind]++;
        }
    }
}

static void test_random(int kind, uint64_t n, uint64_t seed) {
    printf("%s: %llu at random\n", kind_name[kind], (unsigned long long)n);
    int aborts = 0;
    for (uint64_t from = 0; from < n;) {
        pid_t pid = fork();
        if (!pid) { run(kind, from, n, seed); _exit(0); }
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st) && !WEXITSTATUS(st)) break;
        HvTexDesc d = sh->d;
        aborts++;
        printf("  FAIL: Metal stopped on #%llu (%s): type %u format %u %ux%ux%u, %u mips, %u samples, %u long, usage %llx, options %llx; %llu %llu %llu %llu %llu %llu\n",
               (unsigned long long)sh->at, WIFSIGNALED(st) ? strsignal(WTERMSIG(st)) : "exit", d.type, d.format, d.width, d.height, d.depth, d.mips,
               d.samples, d.array, d.usage, d.options, sh->extra[0], sh->extra[1], sh->extra[2], sh->extra[3], sh->extra[4], sh->extra[5]);
        from = sh->at + 1;
    }
    checks++, failures += aborts != 0;
    printf("  %llu let through (%llu made, %llu too big to make here), %llu refused, Metal stopped %d times\n",
           sh->passed[kind], sh->made[kind], sh->big[kind], sh->refused[kind], aborts);
    checks++;
    if (sh->passed[kind] < n / 20) failures++, printf("  FAIL: too few let through for this to show much\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_known();
    test_pass();
    test_fields();
    for (uint32_t f = 0; f < 700; f++) if (hv_pvg_format_known(f)) known[nknown++] = f;
    sh = mmap(NULL, sizeof *sh, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    uint64_t n = argc > 1 ? strtoull(argv[1], NULL, 0) : 4000, seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 0x5eed;
    for (int kind = 0; kind < KINDS; kind++) test_random(kind, n, seed);
    test_clamp();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
